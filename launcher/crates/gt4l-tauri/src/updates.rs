//! Update checks and installs (owner rules, 2026-10-20). Background checks cover launcher releases and the selected
//! external renderer, with a short timeout, and never block Play. It offers an update; it never installs one by
//! itself. The one exception is the first gt4-gs install, which has nothing to replace.
//!
//! - gt4-gs: installed into a new version folder with the same staging rules as setup (`gs_release`), then
//!   `current.txt` and `version.json` point at it. Only when the game is not running.
//! - GT4Recomp: the setup program is downloaded to a temporary folder, its SHA-256 checked against `SHA256SUMS`,
//!   started, and the launcher exits. The installer upgrades the program files in place and keeps the data folder.
//!
//! Events to the page: `update-available` (offers), `renderer-progress` (install steps), `update-finished`.

use gt4l_core::messages;
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Mutex;

use gt4l_core::gs_release::{self, Outcome, Step};
use gt4l_core::paths::DataDirs;
use gt4l_core::portable_update;
use gt4l_core::releases::{self, GithubSource, ReleaseSource, Stream};
use gt4l_core::update::{self, Offer, UpdateChannel};
use gt4l_core::version_list;
use gt4l_core::versions::{self, VersionFile};
use gt4l_core::{Platform, ReleaseVersion, Version, LAUNCHER_VERSION};
use serde::Serialize;
use serde_json::{json, Value};
use tauri::{AppHandle, Emitter, State};

use crate::state::{self, lock, Shared};

/// One renderer install at a time: the launch check, setup and an update must not write the same folder.
static RENDERER_INSTALL: Mutex<()> = Mutex::new(());

/// The installed renderer in use: `<data>\gs`, or a read-only or 1.2.0 folder that still has it (see `DataDirs`).
pub fn renderer_root(dirs: &DataDirs) -> PathBuf {
    dirs.renderer_root()
}

/// The folder of `GT4Recomp.exe` (the install folder).
pub fn install_directory() -> PathBuf {
    std::env::current_exe()
        .ok()
        .and_then(|path| path.parent().map(Path::to_path_buf))
        .unwrap_or_else(|| PathBuf::from("."))
}

/// The `version.json` location: the install folder when writable, else the data folder (logged).
pub fn version_location(dirs: &DataDirs) -> versions::Location {
    let location = versions::location(&install_directory(), dirs.root());
    if let Some(notice) = &location.notice {
        state::log(dirs, notice);
    }
    location
}

/// The launcher-facing view of one offer (the popup and the Settings check use it).
#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct OfferView {
    pub stream: &'static str,
    /// `null` when the stream is not installed yet.
    pub installed: Option<String>,
    pub latest: String,
    pub notes: String,
    pub package: String,
    pub update: bool,
}

impl OfferView {
    pub fn of(offer: &Offer) -> OfferView {
        OfferView {
            stream: offer.stream.key(),
            installed: offer.installed.map(|version| version.text()),
            latest: offer.latest.text(),
            notes: offer.notes.clone(),
            package: match offer.stream {
                Stream::Launcher => Platform::current().launcher_package_name(offer.latest, gt4l_core::build_kind::BUILD.is_portable()),
                Stream::Renderer => offer.stream.package_name_for(offer.latest, gt4l_core::build_kind::BUILD.is_portable()),
            },
            update: offer.is_update(),
        }
    }
}

/// The result of one check of both streams.
pub struct Check {
    pub offers: Vec<Offer>,
    /// Failures of the check. They are logged; the popup shows none of them.
    pub errors: Vec<String>,
    /// The installed gt4-gs version, if any.
    pub renderer_installed: Option<Version>,
}

fn fetch(stream: Stream) -> Result<Vec<releases::Release>, String> {
    let source = GithubSource::for_stream(stream);
    let mut list = releases::parse_releases(&source.releases_json()?, stream)?;
    // TASK-180: Android sees only the launcher releases that ship its APK (Windows: every release, as before).
    if stream == Stream::Launcher {
        list.retain(|release| Platform::current().takes_release(release));
    }
    Ok(list)
}

/// An explicit update check includes both streams. GSdx is the only renderer (TASK-096), so every check covers it.
/// Blocking: run it on a worker thread.
pub fn check(
    dirs: &DataDirs,
    channel: UpdateChannel,
    skipped_launcher: Option<ReleaseVersion>,
    skipped_renderer: Option<Version>,
) -> Check {
    check_with(dirs, channel, skipped_launcher, skipped_renderer, Platform::current().has_gsdx(), fetch)
}

/// The injected fetcher lets tests verify which network requests an automatic check actually makes.
fn check_with(
    dirs: &DataDirs,
    channel: UpdateChannel,
    skipped_launcher: Option<ReleaseVersion>,
    skipped_renderer: Option<Version>,
    include_renderer: bool,
    mut fetch_releases: impl FnMut(Stream) -> Result<Vec<releases::Release>, String>,
) -> Check {
    let mut offers = Vec::new();
    let mut errors = Vec::new();
    let renderer_installed = gs_release::installed(&renderer_root(dirs)).map(|installed| installed.version);
    let launcher_installed = gt4l_core::launcher_release_version();
    let streams = [
        (Stream::Launcher, launcher_installed, skipped_launcher),
        (Stream::Renderer, renderer_installed.map(ReleaseVersion::from), skipped_renderer.map(ReleaseVersion::from)),
    ];
    for (stream, installed, skipped) in streams {
        if stream == Stream::Renderer && !include_renderer {
            continue;
        }
        match fetch_releases(stream) {
            Ok(list) => {
                if let Some(offer) = update::decide(stream, channel, installed, &list, skipped) {
                    offers.push(offer);
                }
            }
            Err(message) => errors.push(format!("{}: {message}", stream.key())),
        }
    }
    for error in &errors {
        state::log_warn(dirs, &format!("update check: {error}"));
    }
    Check { offers, errors, renderer_installed }
}

/// Installs the newest gt4-gs (first install or update). Records the version in `version.json`.
pub fn install_renderer(dirs: &DataDirs, progress: &mut dyn FnMut(Step)) -> Result<Outcome, String> {
    // TASK-180 (owner, 2026-11-06): GSdx is a Windows program, so no platform without it starts the transfer. Setup, the
    // Settings dialog and a stored choice all pass through here; refusing at the one place means none of them can fetch it.
    if !gt4l_core::launch::gsdx_installs_here(gt4l_core::platform::Platform::current()) {
        return Err(gt4l_core::launch::GSDX_WINDOWS_ONLY.to_string());
    }
    let _serial = RENDERER_INSTALL.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    // Installs and updates always write the data folder's root; a renderer read from elsewhere is left as it is.
    let root = dirs.renderer_install_root();
    let source = GithubSource::for_stream(Stream::Renderer);
    let outcome = gs_release::install_latest_reporting(&source as &dyn ReleaseSource, &root, progress)?;
    record_renderer_install(dirs, &outcome);
    Ok(outcome)
}

/// TASK-158: installs a gt4-gs package the player picked by hand, verified against the `SHA256SUMS` of its release (fetched
/// online; without them nothing is installed). Same lock and same records as [`install_renderer`]. The error is the text to show.
pub fn install_renderer_archive(
    dirs: &DataDirs,
    archive: &std::path::Path,
    language: gt4l_core::language::Language,
    progress: &mut dyn FnMut(Step),
) -> Result<Outcome, String> {
    let _serial = RENDERER_INSTALL.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    let root = dirs.renderer_install_root();
    let source = GithubSource::for_stream(Stream::Renderer);
    let outcome = gs_release::install_archive_with_release(&source as &dyn ReleaseSource, archive, &root, progress)
        .map_err(|problem| problem.message(language))?;
    record_renderer_install(dirs, &outcome);
    Ok(outcome)
}

/// Writes the installed gt4-gs version to `version.json` and the log.
fn record_renderer_install(dirs: &DataDirs, outcome: &Outcome) {
    let location = version_location(dirs);
    if let Err(message) = versions::record_renderer(
        &location.path,
        location.path.parent().unwrap_or(dirs.root()),
        outcome.installed().version,
        LAUNCHER_VERSION,
    ) {
        state::log_warn(dirs, &format!("version.json not updated: {message}"));
    }
    state::log(dirs, &format!("gt4-gs {} ready", outcome.installed().version.text()));
}

pub fn step_name(step: Step) -> &'static str {
    match step {
        Step::Listing => "listing",
        Step::Downloading | Step::Transfer { .. } => "downloading",
        Step::Installing => "installing",
    }
}

/// Emits a step change; the per-chunk byte counts are not sent from here (the setup page polls them).
pub fn emit_step(app: &AppHandle, step: Step) {
    if !matches!(step, Step::Transfer { .. }) {
        emit_progress(app, step_name(step));
    }
}

pub fn emit_progress(app: &AppHandle, step: &str) {
    let _ = app.emit("renderer-progress", json!({ "step": step }));
}

/// Makes sure `version.json` exists, is readable and names what is installed. A damaged file is replaced by a fresh
/// one (the damaged copy is kept as `version.json.bad`). The launcher version stays the Cargo version; a file that
/// disagrees (after an upgrade the installer writes a null renderer) is rewritten with the Cargo version and the checked
/// gt4-gs install (`versions::reconciled`).
pub fn ensure_version_file(dirs: &DataDirs) {
    let location = version_location(dirs);
    match versions::read(&location.path) {
        Ok(Some(file)) => {
            let (_, warning) = versions::reconcile_launcher_version(Some(&file), LAUNCHER_VERSION);
            if let Some(warning) = warning {
                state::log(dirs, &warning);
            }
            let renderer = gs_release::installed(&renderer_root(dirs)).map(|installed| installed.version);
            if let Some(next) = versions::reconciled(&file, LAUNCHER_VERSION, renderer) {
                if let Err(message) = versions::write(&location.path, location.path.parent().unwrap_or(dirs.root()), &next) {
                    state::log_warn(dirs, &format!("version.json not updated: {message}"));
                }
            }
        }
        Ok(None) => {
            let file = VersionFile::for_install(LAUNCHER_VERSION, gt4l_core::config::SCHEMA_VERSION);
            if let Err(message) = versions::write(&location.path, location.path.parent().unwrap_or(dirs.root()), &file) {
                state::log_warn(dirs, &format!("version.json not written: {message}"));
            }
        }
        Err(message) => {
            state::log_warn(dirs, &format!("{message}; kept as version.json.bad and replaced"));
            let _ = fs::rename(&location.path, location.path.with_extension("json.bad"));
            let file = VersionFile::for_install(LAUNCHER_VERSION, gt4l_core::config::SCHEMA_VERSION);
            let _ = versions::write(&location.path, location.path.parent().unwrap_or(dirs.root()), &file);
        }
    }
}

/// The startup repair applies only to a prepared game using the external renderer: under CoreGS 2.0, GSdx is not needed.
fn needs_renderer_install(dirs: &DataDirs, set_up: bool, uses_gsdx: bool) -> bool {
    set_up && uses_gsdx && gs_release::needs_install(&renderer_root(dirs))
}

/// The background work at every launch: version file, the selected external renderer install when none exists or
/// the installed one is below the minimum (gt4-gs 1.0.1 under launcher 1.1.0 is replaced by the newest stable release),
/// and the update check. Failures are logged only. Emits `update-available` when there is something to offer.
pub fn check_on_launch(app: AppHandle, shared: Shared) {
    let (dirs, config, set_up) = {
        let state = lock(&shared);
        (state.dirs.clone(), state.config.clone(), state.home_ready())
    };
    ensure_version_file(&dirs);
    // Before the first setup the renderer is installed by setup itself, with its progress on the setup page. Here it
    // is the retry after a setup that was offline, or the update of a version below the minimum.
    if needs_renderer_install(&dirs, set_up, !gt4l_core::launch::uses_coregs20(&config.settings)) {
        let progress_app = app.clone();
        match install_renderer(&dirs, &mut |step| emit_step(&progress_app, step)) {
            Ok(_) => emit_progress(&app, "done"),
            Err(message) => state::log_error(&dirs, &format!("gt4-gs not installed at launch: {message}")),
        }
    }
    let channel = UpdateChannel::of(&config.settings);
    let result = check_with(&dirs, channel, config.skipped_launcher, config.skipped_renderer, Platform::current().has_gsdx(), fetch);
    let offers: Vec<OfferView> = result.offers.iter().filter(|offer| offer.is_update()).map(OfferView::of).collect();
    if !offers.is_empty() {
        let _ = app.emit("update-available", json!({ "offers": offers }));
    }
}

/// `check_updates()`: `{offers: [OfferView], errors: [string], upToDate: bool, launcher, renderer}`.
/// Used by the Settings "Check for updates" button and the About page.
#[tauri::command]
pub async fn check_updates(shared: State<'_, Shared>) -> Result<Value, String> {
    let shared: Shared = shared.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        let (dirs, channel) = {
            let state = lock(&shared);
            (state.dirs.clone(), UpdateChannel::of(&state.config.settings))
        };
        // The button ignores "Skip this version": the player asked for the answer now.
        let result = check(&dirs, channel, None, None);
        let offers: Vec<OfferView> = result.offers.iter().filter(|offer| offer.is_update()).map(OfferView::of).collect();
        let up_to_date = offers.is_empty() && result.errors.is_empty();
        Ok(json!({
            "offers": offers,
            "errors": result.errors,
            "upToDate": up_to_date,
            "launcher": LAUNCHER_VERSION,
            "channel": channel.key(),
            "renderer": result.renderer_installed.map(|version| version.text()),
        }))
    })
    .await
    .map_err(|error| error.to_string())?
}

/// `skip_update({stream, version})`: no offer again for exactly this version. Saved in `settings.toml`.
#[tauri::command]
pub fn skip_update(stream: String, version: String, shared: State<'_, Shared>) -> Result<(), String> {
    let not_a_version = || format!("'{version}' is not a version");
    let mut state = lock(shared.inner());
    match stream.as_str() {
        "launcher" => state.config.skipped_launcher = Some(ReleaseVersion::parse(&version).ok_or_else(not_a_version)?),
        "renderer" => state.config.skipped_renderer = Some(Version::parse(&version).ok_or_else(not_a_version)?),
        other => return Err(format!("unknown update stream '{other}'")),
    }
    state.save_config();
    Ok(())
}

/// `apply_update({stream})`: installs the newest stable version of the stream the player accepted.
/// The renderer is installed in place; the launcher starts its setup program and exits.
#[tauri::command]
pub async fn apply_update(app: AppHandle, stream: String, shared: State<'_, Shared>) -> Result<Value, String> {
    let shared: Shared = shared.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        let (dirs, game_running, channel) = {
            let state = lock(&shared);
            (state.dirs.clone(), state.game_child.is_some(), UpdateChannel::of(&state.config.settings))
        };
        match stream.as_str() {
            "renderer" => {
                if game_running {
                    return Err(messages::text("renderer.close_game", state::current_language()).to_string());
                }
                let progress_app = app.clone();
                let outcome = install_renderer(&dirs, &mut |step| emit_step(&progress_app, step));
                let (ok, message) = match outcome {
                    Ok(outcome) => (true, format!("gt4-gs {} is ready.", outcome.installed().version.text())),
                    Err(message) => (false, message),
                };
                let _ = app.emit("update-finished", json!({ "stream": "renderer", "ok": ok, "message": message }));
                if ok {
                    Ok(json!({ "ok": true, "message": message }))
                } else {
                    Err(message)
                }
            }
            "launcher" => {
                // Updating ends the launcher, and the game ends with it (kill-on-close job).
                if game_running {
                    return Err(messages::text("update.close_game", state::current_language()).to_string());
                }
                let message = run_launcher_update(&app, &dirs, &Target::Latest(channel))?;
                Ok(json!({ "ok": true, "message": message }))
            }
            other => Err(format!("unknown update stream '{other}'")),
        }
    })
    .await
    .map_err(|error| error.to_string())?
}

/// Which release a launcher install takes (TASK-109): the newest of the player's channel (a normal update), or the exact
/// release the player picked in the version list. Both run the same download, SHA256SUMS check and install code.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Target {
    Latest(UpdateChannel),
    Tag(String),
}

/// The release to install and its version, or the plain reason there is nothing to do. Pure (tests).
/// - `Latest`: the newest on the channel; nothing when the installed build is already that or newer.
/// - `Tag`: the exact release, which must be one the list would show; the same version again is refused; an older one
///   is allowed (the page asked for the downgrade confirmation).
pub fn pick_release<'a>(
    list: &'a [releases::Release],
    target: &Target,
    installed: Option<ReleaseVersion>,
) -> Result<(&'a releases::Release, ReleaseVersion), String> {
    let up_to_date = || messages::text("update.up_to_date", state::current_language()).to_string();
    match target {
        Target::Latest(channel) => {
            let release = releases::select_offer(list, *channel, Stream::Launcher.minimum().into())
                .ok_or("no GT4Recomp release on this channel")?;
            let version = release.release_version.ok_or("the release has no version")?;
            if installed.is_some_and(|current| current >= version) {
                return Err(up_to_date());
            }
            Ok((release, version))
        }
        Target::Tag(tag) => {
            let (release, version) = version_list::find_installable(list, tag)?;
            if installed == Some(version) {
                return Err(up_to_date());
            }
            Ok((release, version))
        }
    }
}

/// The installer or portable path for the target. Updating ends the launcher.
/// TASK-180: Android has no installer here; the browser downloads the release's APK and the player installs it.
fn run_launcher_update(app: &AppHandle, dirs: &DataDirs, target: &Target) -> Result<String, String> {
    if !Platform::current().installs_updates() {
        let _ = app;
        return open_launcher_download(dirs, target);
    }
    if gt4l_core::build_kind::BUILD.is_portable() {
        start_portable_update(app, dirs, target)
    } else {
        start_launcher_setup(app, dirs, target)
    }
}

/// `list_launcher_versions()`: the version picker's list for the saved channel, read from GitHub every time:
/// `{channel, installed, entries: [{tag, version, date, candidate, installed, downgrade}], nothingNewer}`.
/// An error string is the plain message the row shows (offline, GitHub error); the current version stays.
#[tauri::command]
pub async fn list_launcher_versions(shared: State<'_, Shared>) -> Result<Value, String> {
    let shared: Shared = shared.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        let (dirs, channel) = {
            let state = lock(&shared);
            (state.dirs.clone(), UpdateChannel::of(&state.config.settings))
        };
        let language = state::current_language();
        let list = fetch(Stream::Launcher).map_err(|reason| {
            state::log_warn(&dirs, &format!("version list: {reason}"));
            messages::render("update.versions_failed", language, &[("reason", &reason)])
        })?;
        let installed = gt4l_core::launcher_release_version();
        let entries = version_list::build_list(&list, channel, installed);
        let nothing_newer = version_list::nothing_at_or_above(&entries, installed) && !entries.iter().any(|entry| entry.installed);
        Ok(json!({
            "channel": channel.key(),
            "installed": LAUNCHER_VERSION,
            "entries": entries.iter().map(|entry| json!({
                "tag": entry.tag,
                "version": entry.version.text(),
                "date": entry.date,
                "candidate": entry.candidate,
                "installed": entry.installed,
                "downgrade": entry.downgrade,
            })).collect::<Vec<_>>(),
            "nothingNewer": nothing_newer,
        }))
    })
    .await
    .map_err(|error| error.to_string())?
}

/// `install_launcher_version({tag})`: installs the exact version the player picked and confirmed (the page asks first;
/// an older version gets the strong warning). Same path as an update: download, SHA256SUMS check, the setup program
/// (or the portable zip swap), and the launcher closes.
#[tauri::command]
pub async fn install_launcher_version(app: AppHandle, tag: String, shared: State<'_, Shared>) -> Result<Value, String> {
    let shared: Shared = shared.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        let (dirs, game_running) = {
            let state = lock(&shared);
            (state.dirs.clone(), state.game_child.is_some())
        };
        if game_running {
            return Err(messages::text("update.close_game", state::current_language()).to_string());
        }
        state::log(&dirs, &format!("installing the chosen version {tag}"));
        let message = run_launcher_update(&app, &dirs, &Target::Tag(tag))?;
        Ok(json!({ "ok": true, "message": message }))
    })
    .await
    .map_err(|error| error.to_string())?
}

/// The command line of the setup of a self-update (TASK-091): `/S /UPDATE /D=<install folder>`, so the update is
/// silent, and the installer starts the launcher again as the player. `/D=` must be last and unquoted (NSIS), so the
/// folder is passed as it is. Only when this launcher runs from the registered per-machine install: a portable copy
/// elsewhere, or no registered install, gets `None` (an interactive setup with no arguments), so a portable folder is
/// never silently turned into an install. `read_registered` is the registry read (`InstallDir`), faked in tests.
pub fn setup_arguments(exe_dir: &Path, read_registered: &dyn Fn() -> Option<String>) -> Option<String> {
    let registered = read_registered()?;
    if folder_key(&exe_dir.to_string_lossy()) == folder_key(&registered) {
        Some(format!("/S /UPDATE /D={}", exe_dir.display()))
    } else {
        None
    }
}

/// A folder compared the way Windows compares it: case-insensitive, one trailing backslash (or slash) ignored.
fn folder_key(folder: &str) -> String {
    folder.trim().trim_end_matches(['\\', '/']).to_ascii_lowercase()
}

/// The portable build's self-update (TASK-098): downloads `GT4Recomp-<version>-portable.zip` of the chosen release into
/// `<exe dir>\.update-download`, checks its SHA-256 against `SHA256SUMS`, extracts it, swaps the program files in (never
/// `user\` or `data\`; a failure rolls back), starts the new launcher and ends this one. No administrator prompt.
fn start_portable_update(app: &AppHandle, dirs: &DataDirs, target: &Target) -> Result<String, String> {
    let exe_dir = install_directory();
    let list = fetch(Stream::Launcher)?;
    let (latest, version) = pick_release(&list, target, gt4l_core::launcher_release_version())?;
    let (zip_asset, sums_asset) = portable_update::release_assets(latest, version)?;
    let package = zip_asset.name.clone();
    let sums = GithubSource::for_stream(Stream::Launcher).download(&sums_asset.url)?;
    let sums_text = String::from_utf8_lossy(&sums).into_owned();

    let zip = portable_update::download_path(&exe_dir, &package);
    let fs = portable_update::RealFs;
    let _ = fs::remove_dir_all(exe_dir.join(portable_update::DOWNLOAD_FOLDER));
    fs::create_dir_all(zip.parent().unwrap_or(&exe_dir))
        .map_err(|error| format!("{}: {error}. Move the GT4Recomp folder to a place you can write to.", exe_dir.display()))?;
    emit_progress(app, "downloading-launcher");
    let actual = releases::download_to_file(&zip_asset.url, &zip, releases::MAX_SETUP_BYTES).inspect_err(|_| {
        let _ = fs::remove_dir_all(exe_dir.join(portable_update::DOWNLOAD_FOLDER));
    })?;
    state::log(dirs, &format!("GT4Recomp {} portable package downloaded; checking and installing it", version.text()));
    let outcome = portable_update::verify_and_install(&fs, &exe_dir, &zip, &package, &actual, &sums_text);
    let _ = fs::remove_dir_all(exe_dir.join(portable_update::DOWNLOAD_FOLDER));
    let report = match outcome {
        Ok(report) => report,
        Err(message) => {
            state::log_error(dirs, &format!("portable update failed: {message}"));
            return Err(message);
        }
    };
    state::log(dirs, &format!("portable update installed: {}", report.items.join(", ")));
    // The new launcher starts outside this launcher's kill-on-close job and waits for this one to exit (single instance).
    let new_launcher = exe_dir.join(portable_update::LAUNCHER_FILE);
    let started = gt4l_platform::process::spawn_outside_job(std::process::Command::new(&new_launcher).arg(portable_update::RESTART_ARGUMENT));
    if let Err(error) = started {
        // The files are swapped; the player starts GT4Recomp.exe by hand.
        state::log_error(dirs, &format!("the updated launcher did not start: {error}"));
        let _ = app.emit("update-finished", json!({ "stream": "launcher", "ok": true, "message": messages::text("update.updated_start_again", state::current_language()) }));
        return Ok(messages::render("update.installed_start_again", state::current_language(), &[("version", &version.text())]));
    }
    let _ = app.emit("update-finished", json!({ "stream": "launcher", "ok": true, "message": messages::text("update.restarting", state::current_language()) }));
    app.exit(0);
    Ok(messages::render("update.installed_restarting", state::current_language(), &[("version", &version.text())]))
}

/// Portable build, at every start (TASK-098): finishes or undoes the previous update (an interrupted swap is restored, the
/// old copy is removed). The old launcher may still be exiting, so a leftover is tried again for a while in the background.
pub fn portable_start_cleanup(dirs: &DataDirs, exe_dir: &Path) {
    let fs = portable_update::RealFs;
    for line in portable_update::finish_previous_update(&fs, exe_dir) {
        state::log(dirs, &format!("portable update: {line}"));
    }
    if portable_update::cleanup_pending(&fs, exe_dir) {
        let dirs = dirs.clone();
        let exe_dir = exe_dir.to_path_buf();
        std::thread::spawn(move || {
            for _ in 0..20 {
                std::thread::sleep(std::time::Duration::from_secs(1));
                for line in portable_update::finish_previous_update(&portable_update::RealFs, &exe_dir) {
                    state::log(&dirs, &format!("portable update: {line}"));
                }
                if !portable_update::cleanup_pending(&portable_update::RealFs, &exe_dir) {
                    return;
                }
            }
        });
    }
}

/// Android (TASK-180, owner 2026-11-05): no update is installed by the app. The chosen release (the same pick as the
/// installer path) has its APK opened in the browser, which downloads it; the player then opens it to install.
fn open_launcher_download(dirs: &DataDirs, target: &Target) -> Result<String, String> {
    let list = fetch(Stream::Launcher)?;
    let (release, version) = pick_release(&list, target, gt4l_core::launcher_release_version())?;
    let url = Platform::current()
        .launcher_download_url(release)
        .ok_or_else(|| format!("release {} has no Android download", release.tag))?;
    state::log(dirs, &format!("update: opening the download of {} in the browser", version.text()));
    #[cfg(target_os = "android")]
    crate::android::open_url(&url)?;
    #[cfg(not(target_os = "android"))]
    let _ = url;
    Ok(messages::render("update.download_opened", state::current_language(), &[("version", &version.text())]))
}

/// Downloads the GT4Recomp setup program of the chosen release (see [`Target`]), checks it, starts it, and ends the launcher.
fn start_launcher_setup(app: &AppHandle, dirs: &DataDirs, target: &Target) -> Result<String, String> {
    let list = fetch(Stream::Launcher)?;
    let (latest, version) = pick_release(&list, target, gt4l_core::launcher_release_version())?;
    let package = Stream::Launcher.package_name(version);
    let package_asset = latest.asset(&package).ok_or_else(|| format!("release {} has no {package}", latest.tag))?;
    let sums_asset = latest
        .asset(releases::CHECKSUM_ASSET)
        .ok_or_else(|| format!("release {} has no {}", latest.tag, releases::CHECKSUM_ASSET))?;
    let sums = GithubSource::for_stream(Stream::Launcher).download(&sums_asset.url)?;
    let sums_text = String::from_utf8_lossy(&sums).into_owned();

    let folder = std::env::temp_dir().join("GT4Recomp-update");
    let _ = fs::remove_dir_all(&folder);
    fs::create_dir_all(&folder).map_err(|error| error.to_string())?;
    let path = folder.join(&package);
    emit_progress(app, "downloading-launcher");
    let actual = releases::download_to_file(&package_asset.url, &path, releases::MAX_SETUP_BYTES)?;
    releases::check_sum(&package, &actual, &sums_text).inspect_err(|_| {
        let _ = fs::remove_file(&path);
    })?;
    state::log(dirs, &format!("GT4Recomp {} setup verified; starting it", version.text()));
    // The 1.2.1 setup installs per machine and needs administrator rights (TASK-091): CreateProcess cannot start it
    // (ERROR_ELEVATION_REQUIRED). The launcher first clears kill-on-close on its job, so nothing it starts dies when
    // it exits (the game is not running here), then asks Windows to start the setup elevated (`runas`, the UAC
    // prompt). The breakaway start and the Explorer fallback stay behind it (gt4l_platform::process::start_setup).
    if let Err(message) = gt4l_platform::process::release_kill_on_close() {
        state::log_warn(dirs, &format!("kill-on-close not cleared ({message}); the setup is started outside the job"));
    }
    // Silent, with this folder as the install folder, when this launcher is the registered install (see setup_arguments).
    let arguments = setup_arguments(&install_directory(), &gt4l_platform::install_record::registered_install_folder);
    state::log(dirs, &format!("setup arguments: {}", arguments.as_deref().unwrap_or("none (interactive)")));
    let started = match gt4l_platform::process::start_setup(&path, arguments.as_deref(), &gt4l_platform::process::WindowsSetupStarter, &mut |line| {
        state::log(dirs, &line)
    }) {
        Ok(started) => started,
        Err(message) => {
            // No setup runs, so the launcher goes back to ending the game with it.
            match gt4l_platform::process::restore_kill_on_close() {
                Ok(()) => state::log(dirs, "kill-on-close restored after the setup did not start"),
                Err(restore) => state::log_warn(dirs, &format!("kill-on-close not restored ({restore})")),
            }
            return Err(message);
        }
    };
    state::log(dirs, &format!("setup started ({started:?})"));
    if started == gt4l_platform::process::SetupStart::Explorer {
        // Explorer starts it a moment later; the launcher waits so the hand-over completes first.
        std::thread::sleep(std::time::Duration::from_secs(3));
    }
    let _ = app.emit("update-finished", json!({ "stream": "launcher", "ok": true, "message": messages::text("update.setup_started", state::current_language()) }));
    app.exit(0);
    Ok(messages::render("update.setup_started_version", state::current_language(), &[("version", &version.text())]))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_registered_install_updates_silently_into_its_own_folder() {
        let exe_dir = Path::new("C:\\Program Files\\GT4Recomp");
        let registered = || Some("c:\\program files\\gt4recomp\\".to_string());
        assert_eq!(setup_arguments(exe_dir, &registered).as_deref(), Some("/S /UPDATE /D=C:\\Program Files\\GT4Recomp"));
        let plain = || Some("C:\\Program Files\\GT4Recomp".to_string());
        assert_eq!(setup_arguments(exe_dir, &plain), setup_arguments(exe_dir, &registered));
    }

    #[test]
    fn a_portable_copy_or_no_registration_stays_interactive() {
        let portable = Path::new("D:\\Games\\GT4Recomp");
        let registered = || Some("C:\\Program Files\\GT4Recomp".to_string());
        assert_eq!(setup_arguments(portable, &registered), None, "a portable folder is never turned into an install");
        let nothing = || None;
        assert_eq!(setup_arguments(Path::new("C:\\Program Files\\GT4Recomp"), &nothing), None);
    }

    #[test]
    fn the_offer_names_the_package_of_this_build() {
        let offer = Offer {
            stream: Stream::Launcher,
            installed: Some(Version(1, 2, 3).into()),
            latest: Version(1, 3, 0).into(),
            notes: String::new(),
            package: "GT4Recomp-Setup-1.3.0.exe".into(),
        };
        let expected = if gt4l_core::build_kind::BUILD.is_portable() { "GT4Recomp-1.3.0-portable.zip" } else { "GT4Recomp-Setup-1.3.0.exe" };
        assert_eq!(OfferView::of(&offer).package, expected);
        // Both packages exist for either build; the renderer has one package.
        assert_eq!(Stream::Launcher.package_name_for(Version(1, 3, 0), true), "GT4Recomp-1.3.0-portable.zip");
        assert_eq!(Stream::Launcher.package_name_for(Version(1, 3, 0), false), "GT4Recomp-Setup-1.3.0.exe");
        assert_eq!(Stream::Renderer.package_name_for(Version(1, 2, 2), true), Stream::Renderer.package_name(Version(1, 2, 2)));
    }

    #[test]
    fn the_start_cleanup_removes_the_old_copy_of_a_finished_update() {
        let base = std::env::temp_dir().join(format!("gt4l-portable-cleanup-{}", std::process::id()));
        let _ = fs::remove_dir_all(&base);
        let exe_dir = base.join("GT4Recomp");
        fs::create_dir_all(exe_dir.join(portable_update::OLD_FOLDER)).unwrap();
        fs::write(exe_dir.join(portable_update::OLD_FOLDER).join("GT4Recomp.exe"), b"old").unwrap();
        fs::write(exe_dir.join("GT4Recomp.exe"), b"new").unwrap();
        let dirs = DataDirs::new(exe_dir.join("data"));
        portable_start_cleanup(&dirs, &exe_dir);
        assert!(!exe_dir.join(portable_update::OLD_FOLDER).exists());
        assert_eq!(fs::read(exe_dir.join("GT4Recomp.exe")).unwrap(), b"new");
        let _ = fs::remove_dir_all(&base);
    }

    #[test]
    fn the_offer_view_names_both_versions_and_the_update_flag() {
        let offer = Offer {
            stream: Stream::Renderer,
            installed: Some(Version(1, 0, 0).into()),
            latest: Version(1, 0, 1).into(),
            notes: "Notes.".into(),
            package: "gt4-gs-1.0.1-windows-x64.zip".into(),
        };
        let view = OfferView::of(&offer);
        assert_eq!(view.stream, "renderer");
        assert_eq!(view.installed.as_deref(), Some("1.0.0"));
        assert_eq!(view.latest, "1.0.1");
        assert!(view.update);
    }

    #[test]
    fn the_renderer_folder_lives_under_the_data_folder() {
        let dirs = DataDirs::new("C:/data/GT4Recomp");
        assert_eq!(renderer_root(&dirs), PathBuf::from("C:/data/GT4Recomp").join("gs"));
    }

    #[test]
    fn gsdx_startup_repairs_missing_packages_and_checks_both_streams() {
        let root = std::env::temp_dir().join(format!("gt4l-gsdx-check-{}", std::process::id()));
        let dirs = DataDirs::new(&root);
        assert!(!needs_renderer_install(&dirs, false, true), "setup owns the first install");
        assert!(!needs_renderer_install(&dirs, true, false), "CoreGS 2.0 does not need GSdx");
        assert!(needs_renderer_install(&dirs, true, true), "a prepared game repairs its missing package");
        let mut requested = Vec::new();
        let result = check_with(&dirs, UpdateChannel::Stable, None, None, true, |stream| {
            requested.push(stream);
            Ok(Vec::new())
        });
        assert_eq!(requested, vec![Stream::Launcher, Stream::Renderer]);
        assert!(result.errors.is_empty());
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn an_explicit_check_can_still_query_the_optional_renderer() {
        let root = std::env::temp_dir().join(format!("gt4l-explicit-check-{}", std::process::id()));
        let dirs = DataDirs::new(&root);
        let mut requested = Vec::new();
        let result = check_with(&dirs, UpdateChannel::Stable, None, None, true, |stream| {
            requested.push(stream);
            Ok(Vec::new())
        });
        assert_eq!(requested, vec![Stream::Launcher, Stream::Renderer]);
        assert!(result.errors.is_empty());
        let _ = fs::remove_dir_all(root);
    }

    fn list_of(json: &str) -> Vec<releases::Release> {
        releases::parse_releases(json, Stream::Launcher).unwrap()
    }

    const PICK_LIST: &str = r#"[
      {"tag_name":"v1.3.0-rc6","prerelease":true,"draft":false,"published_at":"2026-10-25T09:00:00Z","assets":[]},
      {"tag_name":"v1.3.0-rc5","prerelease":true,"draft":false,"published_at":"2026-10-23T09:00:00Z","assets":[]},
      {"tag_name":"v1.3.0-rc4","prerelease":true,"draft":false,"published_at":"2026-10-22T09:00:00Z","assets":[]},
      {"tag_name":"v1.3.0-rc7","prerelease":true,"draft":true,"assets":[]},
      {"tag_name":"v1.2.3","prerelease":false,"draft":false,"published_at":"2026-10-01T09:00:00Z","assets":[]}
    ]"#;

    fn rc(number: u32) -> Option<ReleaseVersion> {
        Some(ReleaseVersion::candidate(Version(1, 3, 0), number))
    }

    #[test]
    fn a_normal_update_picks_the_newest_of_the_channel_and_refuses_when_up_to_date() {
        let list = list_of(PICK_LIST);
        let (release, version) = pick_release(&list, &Target::Latest(UpdateChannel::Beta), rc(5)).unwrap();
        assert_eq!((release.tag.as_str(), version.text().as_str()), ("v1.3.0-rc6", "1.3.0-rc6"));
        assert!(pick_release(&list, &Target::Latest(UpdateChannel::Beta), rc(6)).is_err());
        assert!(pick_release(&list, &Target::Latest(UpdateChannel::Stable), rc(5)).is_err(), "1.2.3 is older than rc5");
    }

    #[test]
    fn a_chosen_version_installs_that_exact_release_even_when_older() {
        let list = list_of(PICK_LIST);
        let (release, _) = pick_release(&list, &Target::Tag("v1.3.0-rc5".into()), rc(6)).unwrap();
        assert_eq!(release.tag, "v1.3.0-rc5", "a downgrade within the supported range is allowed");
        assert!(pick_release(&list, &Target::Tag("v1.3.0-rc6".into()), rc(6)).is_err(), "the same version again");
        for refused in ["v1.3.0-rc4", "v1.2.3", "v1.3.0-rc7", "v0.0.1"] {
            assert!(pick_release(&list, &Target::Tag(refused.into()), rc(6)).is_err(), "{refused}");
        }
    }

    #[test]
    fn the_chosen_version_uses_the_asset_names_of_the_build_kind() {
        let version = ReleaseVersion::candidate(Version(1, 3, 0), 5);
        assert_eq!(Stream::Launcher.package_name(version), "GT4Recomp-Setup-1.3.0-rc5.exe");
        assert_eq!(Stream::Launcher.package_name_for(version, true), "GT4Recomp-1.3.0-rc5-portable.zip");
    }
}

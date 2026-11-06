//! Setup commands: pick the disc image, then copy it, check it, prepare the game files and install the optional
//! PCSX2 GS renderer when selected (docs/launcher/FLOW-AUDIT.md). The work runs on a thread. The page polls
//! `app_state` (which carries the step, the fraction and the bytes) and also gets `setup-progress` and `setup-finished` events.
//!
//! The command bodies are plain functions over [`Shared`] (`choose_disc_path`, `start_with`) so the flow test
//! (`flow_audit.rs`) drives exactly the code the page drives, without a window.

use gt4l_core::messages;
use std::path::{Path, PathBuf};
use std::sync::atomic::Ordering;
use std::sync::Arc;

use gt4l_core::games::{self, Game, GT4, SPEC2};
use gt4l_core::gpu::GpuDevice;
use gt4l_core::gs_release::Step;
use gt4l_core::launch::{self, SetupRenderer};
use gt4l_core::paths::{self, DataDirs};
use gt4l_core::platform::Platform;
use gt4l_core::shader_prepare::PrepareOutcome;
use gt4l_core::{display, settings};
use gt4l_core::LAUNCHER_VERSION;
use gt4l_disc::identify::{identify, DiscGame};
use gt4l_disc::setup::{self as disc_setup, SetupError, SetupRequest, Stage};
use gt4l_disc::spec2::{self as disc_spec2, Spec2Request};
use serde::Serialize;
use serde_json::{json, Value};
use tauri::{AppHandle, Emitter, State};
use tauri_plugin_dialog::DialogExt;

use crate::state::{self, lock, SetupOutcome, SetupRun, Shared};
use crate::updates;

/// Sends an event to the page (`Emitter::emit` in the app, a recorder in tests).
pub type Notify = Arc<dyn Fn(&str, Value) + Send + Sync>;

/// Installs the newest gt4-gs, reporting steps; returns the installed version text.
pub type RendererInstaller = Arc<dyn Fn(&DataDirs, &mut dyn FnMut(Step)) -> Result<String, String> + Send + Sync>;

/// Whether CoreGS 2.0 runs on this PC, reporting (done, total) of the shaders it compiles on the way.
pub type Coregs20Check = Arc<dyn Fn(&Shared, &'static Game, &mut dyn FnMut(u32, u32)) -> bool + Send + Sync>;

/// The page's event sink for an app handle.
pub fn notifier(app: &AppHandle) -> Notify {
    let app = app.clone();
    Arc::new(move |name: &str, payload: Value| {
        let _ = app.emit(name, payload);
    })
}

/// The real renderer install: GitHub releases of moesuito/gt4recomp-gs (see `updates::install_renderer`).
pub fn github_renderer() -> RendererInstaller {
    Arc::new(|dirs: &DataDirs, progress: &mut dyn FnMut(Step)| {
        updates::install_renderer(dirs, progress).map(|outcome| outcome.installed().version.text())
    })
}

/// The real check of a new install: a graphics card with Vulkan 1.2, and then the game program itself starting CoreGS 2.0 on that
/// card without a window (`--prepare-shaders`, IN-507), which refuses a card that lacks what the renderer needs. It also leaves the
/// shaders compiled, so the first Play has nothing to wait for.
pub fn coregs20_on_this_pc() -> Coregs20Check {
    Arc::new(|shared: &Shared, game: &'static Game, progress: &mut dyn FnMut(u32, u32)| {
        let (settings, dirs, textures, executable, slot, language) = {
            let state = lock(shared);
            let game_dir = paths::game_directory(&crate::commands::program_directory(), state.config.dev_game_dir.as_deref());
            (
                state.config.settings.clone(),
                state.game_dirs(game),
                state.texture_location().0,
                launch::game_program(game, &game_dir),
                state.shaders_child.clone(),
                state::language_of(&state),
            )
        };
        let (settings, graphics, cards) = gt4l_platform::gpu::run_settings_with_cards(&settings, language, gt4l_platform::gpu::refresh_gpus());
        state::log(&dirs, &format!("setup: cards [{}]", gt4l_platform::gpu::card_list(&cards)));
        if !graphics.card.as_ref().is_some_and(GpuDevice::has_vulkan) {
            state::log(&dirs, "setup: no graphics card with Vulkan 1.2 for CoreGS 2.0");
            return false;
        }
        let monitors = gt4l_platform::display::monitors();
        let display = display::resolve_target(&monitors, settings.get("monitor"), settings.get("resolution"));
        let cancelled = lock(shared).shaders_cancelled.clone();
        cancelled.store(false, Ordering::SeqCst);
        // The setup page counts the shaders; a retry (TASK-170b) simply counts again.
        let mut count = |step: gt4l_platform::game::PrepareProgress| progress(step.done, step.total);
        match gt4l_platform::game::prepare_shaders(&dirs, &executable, &settings, display.as_ref(), &textures, &slot, &cancelled, &mut count) {
            Ok(PrepareOutcome::Finished { .. }) => true,
            Ok(_) => {
                state::log_warn(&dirs, "setup: CoreGS 2.0 did not start on this card (logs\\shaders.log)");
                false
            }
            Err(message) => {
                state::log_warn(&dirs, &format!("setup: CoreGS 2.0 could not be checked: {message}"));
                false
            }
        }
    })
}

/// What the page shows after a disc was accepted.
#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DiscInfo {
    pub path: String,
    pub serial: String,
    pub version: String,
    /// `Gran Turismo 4 (USA v2.00)`, or `Gran Turismo 4 Spec II v1.10 (PT-BR)`.
    pub name: String,
    /// The game the disc is (`gt4`, `spec2`): the setup that follows installs this game.
    pub game: String,
    pub game_name: String,
    /// The page asked for another game (`requested`) than the disc is: it offers to install the disc as the game it is,
    /// or to pick another file. Nothing is installed until the page calls `setup_start`.
    pub mismatch: bool,
    pub requested: Option<String>,
}

/// The Spec II version the launcher supports (the only one; other versions have other hashes and are refused).
pub const SPEC2_VERSION: &str = "1.10";

/// The title of a game as the player sees it: Spec II names its supported version.
pub fn game_title(game: &Game) -> String {
    match DiscGame::from_id(game.id) {
        Some(DiscGame::Spec2) => format!("{} v{SPEC2_VERSION}", game.display_name),
        _ => game.display_name.to_string(),
    }
}

/// What tells apart copies of one game, or an empty text: the region and version of Gran Turismo 4 (`USA v2.00`), and
/// for Spec II the pinned image name (`PT-BR`; empty for the plain 1.10 image, and `other image` for an image with the
/// known executable and an unpinned hash). `image_name` is the receipt's name, empty before the copy was checked.
pub fn game_variant(game: &Game, version: &str, image_name: &str) -> String {
    match DiscGame::from_id(game.id) {
        Some(DiscGame::Spec2) => spec2_variant(image_name),
        _ => format!("USA v{version}"),
    }
}

/// The Spec II part of `game_variant`. The names are the pinned image names (`docs/inputs`), not file names.
fn spec2_variant(image_name: &str) -> String {
    let plain = format!("Spec II {SPEC2_VERSION}");
    if image_name.is_empty() || image_name == plain {
        return String::new();
    }
    let region = image_name
        .strip_prefix(&format!("{plain} ("))
        .and_then(|rest| rest.strip_suffix(')'));
    match region {
        Some(region) => region.to_string(),
        None => messages::text("game.variant_other", state::current_language()).to_string(),
    }
}

/// The name of an accepted disc of `game`, as the setup shows it: the title and the variant, for example
/// `Gran Turismo 4 (USA v2.00)` or `Gran Turismo 4 Spec II v1.10 (PT-BR)`.
pub fn game_disc_name(game: &Game, version: &str, image_name: &str) -> String {
    let title = game_title(game);
    let variant = game_variant(game, version, image_name);
    if variant.is_empty() {
        title
    } else {
        format!("{title} ({variant})")
    }
}

/// Opens the file picker for a plain `.iso` and checks it. `None` means the picker was cancelled.
/// A disc that is not accepted gives its plain-language reason as the error (and `app_state` says `wrong_disc`).
#[tauri::command]
pub async fn setup_choose_disc(app: AppHandle, game: Option<String>, shared: State<'_, Shared>) -> Result<Option<DiscInfo>, String> {
    let shared: Shared = shared.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        if lock(&shared).setup_busy {
            return Err(messages::text("setup.already_running", state::current_language()).to_string());
        }
        refuse_unsupported_system(&shared)?;
        // TASK-180: on Android the image is a document the app keeps a read permission to (never copied); its content
        // URI is the image's path from here on, read through gt4l_disc::source.
        #[cfg(target_os = "android")]
        {
            let _ = &app;
            let picked = crate::android::pick_document(&["application/x-iso9660-image", "application/octet-stream", "*/*"], true, None)?;
            let dirs = lock(&shared).dirs.clone();
            let Some(picked) = picked else {
                state::log(&dirs, "setup: the disc picker was cancelled");
                return Ok(None);
            };
            let uri = picked.uri.unwrap_or_default();
            let size = gt4l_disc::source::metadata(Path::new(&uri)).map(|metadata| metadata.len());
            state::log(
                &dirs,
                &format!(
                    "setup: picked {} ({}), {}; read permission kept",
                    picked.name.as_deref().unwrap_or("no name"),
                    uri,
                    match &size {
                        Ok(bytes) => format!("{bytes} bytes"),
                        Err(error) => format!("size unreadable: {error}"),
                    }
                ),
            );
            choose_disc_path_for(&shared, Path::new(&uri), game.as_deref()).map(Some)
        }
        #[cfg(not(target_os = "android"))]
        {
            let picked = app
                .dialog()
                .file()
                .set_title(match game.as_deref().and_then(games::by_id) {
                    Some(requested) => messages::render("setup.picker_title_game", state::current_language(), &[("game", requested.display_name)]),
                    None => messages::text("setup.picker_title_any", state::current_language()).to_string(),
                })
                .add_filter(messages::text("setup.picker_filter", state::current_language()), &["iso"])
                .blocking_pick_file();
            let Some(picked) = picked else {
                return Ok(None);
            };
            let path: PathBuf = picked.into_path().map_err(|error| error.to_string())?;
            choose_disc_path_for(&shared, &path, game.as_deref()).map(Some)
        }
    })
    .await
    .map_err(|error| error.to_string())?
}

/// The check behind `setup_choose_disc` for a path (the picker's result, or a path from the flow test).
pub fn choose_disc_path(shared: &Shared, path: &Path) -> Result<DiscInfo, String> {
    choose_disc_path_for(shared, path, None)
}

/// [`choose_disc_path`] when the page asked for a game (`requested`). The disc decides which game is installed; when it is
/// another game than the one asked for, the answer says so (`mismatch`) and the page offers to install it as that game.
pub fn choose_disc_path_for(shared: &Shared, path: &Path, requested: Option<&str>) -> Result<DiscInfo, String> {
    let (pins, spec2_pins) = {
        let state = lock(shared);
        if state.setup_busy {
            return Err(messages::text("setup.already_running", state::current_language()).into());
        }
        if state.migration.running {
            return Err(messages::text("setup.migrating", state::current_language()).into());
        }
        (state.pins.clone(), state.spec2_pins.clone())
    };
    match identify(path, &pins, &spec2_pins) {
        Ok(found) => {
            let game: &'static Game = match found.game {
                DiscGame::Gt4 => &GT4,
                DiscGame::Spec2 => &SPEC2,
            };
            let summary = found.summary;
            let mut state = lock(shared);
            // The image stays here, and the game must be able to open it here (decision 0075).
            if gt4l_platform::disk::game_readable_path(path).is_none() {
                let message: String = messages::text("setup.path_not_readable", state::current_language()).into();
                state::log_warn(&state.dirs, &format!("disc refused, the game cannot open its path: {}", path.display()));
                state.setup_outcome = SetupOutcome::Refused(message.clone());
                return Err(message);
            }
            // Updates replace the `game` folder: an image there would go with them.
            if paths::is_within(path, &state.install_dir.join("game")) {
                let message: String = messages::text("setup.image_in_game_folder", state::current_language()).into();
                state::log_warn(&state.dirs, &format!("disc refused, it is in the folder updates replace: {}", path.display()));
                state.setup_outcome = SetupOutcome::Refused(message.clone());
                return Err(message);
            }
            state.config.disc_path = Some(path.to_path_buf());
            state.setup_outcome = SetupOutcome::None;
            state.setup_game = Some(game);
            state.save_config();
            state::log(
                &state.dirs,
                &format!("disc accepted as {}: {} v{} ({})", game.id, summary.serial, summary.version, path.display()),
            );
            let requested_game = requested.and_then(games::by_id);
            Ok(DiscInfo {
                path: path.display().to_string(),
                name: game_disc_name(game, &summary.version, ""),
                serial: summary.serial,
                version: summary.version,
                game: game.id.to_string(),
                game_name: game.display_name.to_string(),
                mismatch: requested_game.is_some_and(|asked| asked != game),
                requested: requested_game.map(|asked| asked.id.to_string()),
            })
        }
        Err(error) => {
            let message = error.user_message_in(state::current_language());
            let mut state = lock(shared);
            state::log_warn(&state.dirs, &format!("disc rejected ({}): {error}", path.display()));
            state.setup_outcome = if error.is_disc_refusal() {
                SetupOutcome::Refused(message.clone())
            } else {
                SetupOutcome::Failed(message.clone())
            };
            Err(message)
        }
    }
}

/// Starts the setup thread for the chosen disc. Returns at once; progress arrives through `app_state` and events.
#[tauri::command]
pub fn setup_start(app: AppHandle, shared: State<'_, Shared>) -> Result<(), String> {
    refuse_unsupported_system(shared.inner())?;
    start_with(shared.inner(), notifier(&app), github_renderer(), coregs20_on_this_pc()).map(|_| ())
}

/// TASK-081: the processor check before setup (`gt4l_core::system_check`). The graphics card decides the renderer, not whether
/// setup goes on (`prepare_renderer`). A problem is shown on the setup page as a failed setup with its message, and logged.
fn refuse_unsupported_system(shared: &Shared) -> Result<(), String> {
    let language = state::language_of(&lock(shared));
    let Some(problem) = gt4l_platform::system::processor_problem(language) else {
        return Ok(());
    };
    let mut state = lock(shared);
    state::log_error(&state.dirs, &format!("system requirement not met: {problem}"));
    state.setup_outcome = SetupOutcome::Failed(problem.clone());
    Err(problem)
}

/// The body of `setup_start`. Returns the thread so tests can wait for it.
pub fn start_with(
    shared: &Shared,
    notify: Notify,
    renderer: RendererInstaller,
    coregs20: Coregs20Check,
) -> Result<std::thread::JoinHandle<()>, String> {
    let shared: Shared = shared.clone();
    // The image is identified in place again before anything is copied: a file that changed since it was picked, or one
    // that is not a supported game, is refused here and not one byte reaches the data folder. The game installed is the one
    // the image really is, whatever the page asked for.
    let identified = {
        let state = lock(&shared);
        state.config.disc_path.clone().map(|disc| (disc, state.pins.clone(), state.spec2_pins.clone()))
    };
    let identified_game: Option<&'static Game> = match identified {
        Some((disc, pins, spec2_pins)) => match identify(&disc, &pins, &spec2_pins) {
            Ok(found) => Some(match found.game {
                DiscGame::Gt4 => &GT4,
                DiscGame::Spec2 => &SPEC2,
            }),
            Err(error) => {
                let message = error.user_message_in(state::current_language());
                let mut state = lock(&shared);
                state::log_warn(&state.dirs, &format!("setup refused before copying ({}): {error}", disc.display()));
                state.setup_outcome =
                    if error.is_disc_refusal() { SetupOutcome::Refused(message.clone()) } else { SetupOutcome::Failed(message.clone()) };
                return Err(message);
            }
        },
        None => None,
    };
    let (disc, pins, spec2_pins, game, installed_before, dirs, cancel, install_dir) = {
        let mut state = lock(&shared);
        if state.setup_busy {
            return Err(messages::text("setup.already_running", state::current_language()).into());
        }
        if state.migration.running {
            return Err(messages::text("setup.migrating", state::current_language()).into());
        }
        let Some(disc) = state.config.disc_path.clone() else {
            return Err(messages::text("setup.choose_first", state::current_language()).into());
        };
        let Some(game) = identified_game.or(state.setup_game) else {
            return Err(messages::text("setup.choose_first", state::current_language()).into());
        };
        state.setup_game = Some(game);
        let installed_before = state.installed_games();
        state.setup_busy = true;
        state.setup_outcome = SetupOutcome::None;
        state.setup_run = SetupRun { step: "copying".into(), ..SetupRun::default() };
        state.setup_cancel.store(false, Ordering::Relaxed);
        (disc, state.pins.clone(), state.spec2_pins.clone(), game, installed_before, state.game_dirs(game), state.setup_cancel.clone(), state.install_dir.clone())
    };
    let handle = std::thread::spawn(move || {
        // Setup always writes the data folder's root, even when older game files are read from another folder.
        let prepared = dirs.setup_prepared();
        let staging = dirs.prepared_staging();
        let total_bytes = std::fs::metadata(&disc).map(|meta| meta.len()).unwrap_or(0);
        let mut last: (Stage, i32) = (Stage::Copying, -1);
        let progress_shared = shared.clone();
        let progress_notify = notify.clone();
        let mut report = |stage: Stage, fraction: f32| {
            let permille = (fraction * 1000.0) as i32;
            if (stage, permille) == last {
                return;
            }
            last = (stage, permille);
            let step = match stage {
                Stage::Copying => "copying",
                Stage::Checking => "checking",
                Stage::Preparing => "preparing",
            };
            // Rc5 owner report: the Spec II copy bar stopped short. The figure is exact at the end of the copy, and the
            // later steps keep the finished copy's size (done == total), so the page can draw the copy step full after it
            // moved on (a poll can miss the last 100% value of a fast copy).
            let copy_finished = stage != Stage::Copying || fraction >= 1.0;
            let bytes_done = if copy_finished { total_bytes } else { (fraction as f64 * total_bytes as f64) as u64 };
            {
                let mut state = lock(&progress_shared);
                state.setup_run.step = step.into();
                state.setup_run.fraction = fraction;
                state.setup_run.bytes_done = bytes_done;
                state.setup_run.bytes_total = total_bytes;
            }
            if permille % 10 == 0 || fraction >= 1.0 {
                progress_notify(
                    "setup-progress",
                    json!({ "step": step, "percent": permille / 10, "bytesDone": bytes_done, "bytesTotal": total_bytes }),
                );
            }
        };
        let result: Result<disc_setup::Receipt, SetupError> = match DiscGame::from_id(game.id) {
            Some(DiscGame::Spec2) => disc_spec2::run(
                &Spec2Request {
                    disc: &disc,
                    prepared: &prepared,
                    staging: &staging,
                    install_dir: &install_dir,
                    pins: &spec2_pins,
                    launcher_version: LAUNCHER_VERSION,
                    cancel: &cancel,
                },
                &mut report,
            ),
            _ => disc_setup::run(
                &SetupRequest {
                    disc: &disc,
                    prepared: &prepared,
                    staging: &staging,
                    install_dir: &install_dir,
                    pins: &pins,
                    launcher_version: LAUNCHER_VERSION,
                    cancel: &cancel,
                },
                &mut report,
            ),
        };
        let receipt = match result {
            Ok(receipt) => receipt,
            Err(error) => {
                state::log_error(&dirs, &format!("setup stopped: {error}"));
                let message = error.user_message_in(state::current_language());
                let mut state = lock(&shared);
                state.setup_busy = false;
                state.setup_outcome = if error.is_disc_refusal() {
                    SetupOutcome::Refused(message.clone())
                } else {
                    SetupOutcome::Failed(message.clone())
                };
                drop(state);
                notify("setup-finished", json!({ "ok": false, "message": message }));
                return;
            }
        };
        state::log(
            &dirs,
            &format!(
                "setup complete ({}): {} v{} image {} ({}; known image: {}) elf {}",
                game.id,
                receipt.disc.serial,
                receipt.disc.version,
                receipt.image.as_ref().map(|image| image.file.as_str()).unwrap_or("?"),
                if receipt.image_name.is_empty() { "-" } else { receipt.image_name.as_str() },
                receipt.qualified_dump,
                receipt.elf.sha256
            ),
        );
        if game.id == SPEC2.id && !receipt.qualified_dump {
            state::log(&dirs, "disc is accepted as a Spec II 1.10 variant: its game code (executable and IOP modules) is 1.10's but the image hash is not pinned");
        }

        let renderer_note = prepare_renderer(&shared, &dirs, &notify, &renderer, &coregs20, game, installed_before.is_empty());
        {
            let mut state = lock(&shared);
            state.setup_busy = false;
            state.setup_run.renderer_step = if renderer_note.is_empty() { "done" } else { "failed" }.into();
            state.home_message = renderer_note.clone();
            state.setup_outcome = SetupOutcome::Done { renderer_note };
            // The first game installed is the stored choice, and the page stays on the game just set up.
            state.config.selected_game = games::after_install(state.config.selected_game.as_deref(), &installed_before, game);
            state.selected = game;
            state.save_config();
        }
        notify("setup-finished", json!({ "ok": true, "message": "" }));
    });
    Ok(handle)
}

/// The renderer of this install (`launch::setup_renderer`): CoreGS 2.0 when this PC runs it, else GSdx, installed unless it is
/// already there. The returned text is the setup note (empty when there is nothing to report).
fn prepare_renderer(
    shared: &Shared,
    dirs: &DataDirs,
    notify: &Notify,
    renderer: &RendererInstaller,
    coregs20: &Coregs20Check,
    game: &'static Game,
    first_game: bool,
) -> String {
    let gsdx_missing = gt4l_core::gs_release::needs_install(&dirs.renderer_root());
    let plan = launch::setup_renderer(Platform::current(), &lock(shared).config.settings, first_game, gsdx_missing);
    let mut choice = String::new();
    match plan {
        SetupRenderer::Coregs20 => {
            lock(shared).setup_run.renderer_choice = settings::CORE_GS_20.into();
            // TASK-180 (owner, 2026-11-06): on Android this is the whole renderer story, there is nothing to look for and no
            // GSdx to download; the line says which of the two reasons it is.
            state::log(dirs, if Platform::current().has_gsdx() {
                "setup: CoreGS 2.0 is the renderer; nothing to install"
            } else {
                "setup: CoreGS 2.0 is the only renderer here; GSdx is a Windows program and is not downloaded"
            });
            return String::new();
        }
        SetupRenderer::TryCoregs20 => {
            lock(shared).setup_run = SetupRun { step: "renderer".into(), renderer_step: "checking".into(), ..SetupRun::default() };
            let progress_shared = shared.clone();
            let works = coregs20(shared, game, &mut |done, total| {
                let mut state = lock(&progress_shared);
                state.setup_run.renderer_step = "compiling".into();
                state.setup_run.bytes_done = u64::from(done);
                state.setup_run.bytes_total = u64::from(total);
                state.setup_run.fraction = if total > 0 { done as f32 / total as f32 } else { 0.0 };
            });
            if works {
                let mut state = lock(shared);
                state.setup_run.renderer_choice = settings::CORE_GS_20.into();
                match state.config.settings.set_by_user("renderer", settings::CORE_GS_20) {
                    Ok(()) => state::log(dirs, "setup: CoreGS 2.0 runs on this PC; it is the renderer, and GSdx is not downloaded"),
                    Err(error) => state::log_error(dirs, &format!("setup: could not store CoreGS 2.0: {error}")),
                }
                return String::new();
            }
            state::log(dirs, "setup: CoreGS 2.0 does not run on this PC; GSdx is the renderer");
            choice = "gsdx_fallback".into();
        }
        SetupRenderer::Gsdx => {}
    }
    // TASK-088 (1.2.0): a GSdx that is installed and meets the minimum (for example a local package) is kept, with no
    // download attempt and no misleading note.
    if !gsdx_missing {
        state::log(dirs, "GSdx already installed; setup keeps it");
        return String::new();
    }
    {
        let mut state = lock(shared);
        state.setup_run = SetupRun { step: "renderer".into(), renderer_step: "listing".into(), renderer_choice: choice, ..SetupRun::default() };
    }
    let renderer_shared = shared.clone();
    let renderer_notify = notify.clone();
    let mut last_percent = -1i64;
    let result = renderer(dirs, &mut |step| {
        let mut state = lock(&renderer_shared);
        match step {
            Step::Transfer { done, total } => {
                state.setup_run.renderer_step = "downloading".into();
                state.setup_run.bytes_done = done;
                state.setup_run.bytes_total = total.unwrap_or(0);
                state.setup_run.fraction = total.filter(|total| *total > 0).map(|total| done as f32 / total as f32).unwrap_or(0.0);
                let percent = (state.setup_run.fraction * 100.0) as i64;
                if percent != last_percent {
                    last_percent = percent;
                    renderer_notify(
                        "renderer-progress",
                        json!({ "step": "downloading", "percent": percent, "bytesDone": done, "bytesTotal": total }),
                    );
                }
            }
            other => {
                let name = updates::step_name(other);
                state.setup_run.renderer_step = name.into();
                if other == Step::Installing {
                    state.setup_run.fraction = 1.0;
                }
                renderer_notify("renderer-progress", json!({ "step": name }));
            }
        }
    });
    match result {
        Ok(version) => {
            notify("renderer-progress", json!({ "step": "done", "version": version }));
            String::new()
        }
        Err(message) => {
            state::log_warn(dirs, &format!("GSdx not installed during setup: {message}"));
            notify("renderer-progress", json!({ "step": "failed", "message": message }));
            messages::text("renderer.not_installed_note", state::current_language()).to_string()
        }
    }
}

/// Asks the setup thread to stop after its current chunk. The copy and the staging folder are removed.
#[tauri::command]
pub fn setup_cancel(shared: State<'_, Shared>) -> Result<(), String> {
    cancel_with(shared.inner());
    Ok(())
}

pub fn cancel_with(shared: &Shared) {
    lock(shared).setup_cancel.store(true, Ordering::Relaxed);
}

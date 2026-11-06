//! TASK-180 (docs/ANDROID-PORT-PLAN.md section 4): the launcher as the Android app's main activity.
//!
//! The same page and the same Rust core as on Windows. What Windows does with files, dialogs and processes, Android does
//! through a small Kotlin plugin of the app (`GameBridgePlugin`, gen/android): the Storage Access Framework for the disc
//! image (a persistable permission, never a copy) and for save, replay and log import or export, the browser for links,
//! and Play, which starts the game activity (SDL3, in its own `:game` process) with the launch contract: the same
//! arguments and environment the Windows launcher gives `gt4vulkan.exe`.
//!
//! Platform rules (CoreGS 2.0 only, no GSdx, update check without installer) are in `gt4l_core::settings` and the page
//! model, keyed by `gt4l_core::platform`, not here.

use std::fs::File;
use std::os::fd::FromRawFd;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::Instant;

use serde::{Deserialize, Serialize};
use tauri::plugin::{PluginHandle, TauriPlugin};
use tauri::{Manager, Wry};

use gt4l_core::paths::{DataDirs, UserDirs, UserRootSource};
use gt4l_disc::Pins;

use crate::state::{self, AppState, Shared};
use crate::webview_guard::{Guard, PageReady};

/// The Kotlin side, registered when the app starts.
static BRIDGE: OnceLock<PluginHandle<Wry>> = OnceLock::new();

fn bridge() -> Result<&'static PluginHandle<Wry>, String> {
    BRIDGE.get().ok_or_else(|| "the Android bridge is not ready".to_string())
}

fn bridge_plugin() -> TauriPlugin<Wry> {
    tauri::plugin::Builder::new("gamebridge")
        .setup(|_app, api| {
            let handle = api.register_android_plugin("io.github.moesuito.gt4recomp", "GameBridgePlugin")?;
            let _ = BRIDGE.set(handle);
            Ok(())
        })
        .build()
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct PickArgs<'a> {
    mime_types: &'a [&'a str],
    /// Keep a persistable read permission (the disc image, read again at every Play).
    persist: bool,
    /// Copy the picked document into this file (imports read a plain file).
    copy_to: Option<String>,
}

/// The folder the player picked on the phone for replacement textures: the document-tree URI and its display name.
#[derive(Deserialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct PickedTree {
    pub uri: Option<String>,
    #[serde(default)]
    pub name: Option<String>,
}

#[derive(Serialize)]
struct SyncTreeArgs<'a> {
    uri: &'a str,
    destination: &'a str,
}

#[derive(Deserialize, Default)]
struct SyncedTree {
    #[serde(default)]
    files: u64,
}

/// The document the player picked.
#[derive(Deserialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct Picked {
    pub uri: Option<String>,
    pub name: Option<String>,
}

#[derive(Serialize)]
struct UriArgs<'a> {
    uri: &'a str,
}

#[derive(Deserialize)]
struct Descriptor {
    fd: i32,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct SaveArgs<'a> {
    source: &'a str,
    name: &'a str,
    mime_type: &'a str,
}

#[derive(Deserialize)]
struct Saved {
    saved: bool,
}

#[derive(Serialize)]
struct GameArgs<'a> {
    arguments: &'a [String],
    environment: Vec<String>,
    /// The pause dialog's texts in the launcher's language: title, keep playing, minimize, close game.
    texts: Vec<String>,
    /// TASK-180 test5: `gt4` (v2.00) or `spec2`; the game Activity loads the matching library.
    game: &'a str,
}

/// What the bridge knows about the game's process (`gameState`).
#[derive(Deserialize, Default, Clone, Debug)]
#[serde(rename_all = "camelCase")]
struct GameState {
    #[serde(default)]
    alive: bool,
    #[serde(default)]
    pid: i32,
    #[serde(default)]
    state: Option<String>,
    #[serde(default)]
    heartbeat_age_ms: Option<i64>,
}

/// The last answer and when it was read (the home page asks every second or so).
static GAME_STATE: Mutex<Option<(Instant, GameState)>> = Mutex::new(None);

/// TASK-180 test2: whether the game's process is alive (Minimize keeps it, paused). Asked from the system's list of
/// this app's processes, at most once a second. When a game seen alive is gone, the launcher logs it and reads
/// Android's record of how it ended.
pub fn game_alive(dirs: &DataDirs) -> bool {
    let mut cached = GAME_STATE.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    if let Some((when, state)) = cached.as_ref() {
        if when.elapsed() < std::time::Duration::from_millis(900) {
            return state.alive;
        }
    }
    let now = match bridge().and_then(|bridge| bridge.run_mobile_plugin::<GameState>("gameState", ()).map_err(|error| error.to_string())) {
        Ok(state) => state,
        Err(message) => {
            state::log_warn(dirs, &format!("game state not read: {message}"));
            return cached.as_ref().is_some_and(|(_, state)| state.alive);
        }
    };
    let before = cached.as_ref().map(|(_, state)| state.clone());
    *cached = Some((Instant::now(), now.clone()));
    drop(cached);
    match before {
        Some(before) if before.alive && !now.alive => {
            state::log(dirs, &format!("the game process {} is gone", before.pid));
            record_game_exits(dirs);
            finish_bench_session(dirs);
        }
        Some(before) if before.alive == now.alive && before.state == now.state => {}
        _ if now.alive => state::log(
            dirs,
            &format!(
                "the game process {} is alive ({}, heartbeat {} ms old)",
                now.pid,
                now.state.as_deref().unwrap_or("state unknown"),
                now.heartbeat_age_ms.map(|age| age.to_string()).unwrap_or_else(|| "-".into())
            ),
        ),
        _ => {}
    }
    now.alive
}

#[derive(Serialize)]
struct ShaderArgs<'a> {
    environment: Vec<String>,
    log: &'a str,
}

#[derive(Deserialize, Default)]
#[serde(rename_all = "camelCase")]
struct ShadersState {
    #[serde(default)]
    shaders_alive: bool,
}

fn shaders_alive() -> bool {
    bridge()
        .and_then(|bridge| bridge.run_mobile_plugin::<ShadersState>("gameState", ()).map_err(|error| error.to_string()))
        .map(|state| state.shaders_alive)
        .unwrap_or(false)
}

/// TASK-180 test2 (owner): the shader prepare before Play, as on Windows (`gt4l_platform::game::prepare_shaders`): the
/// same attempts and guard-file rules (TASK-170b/170c), but each attempt is ShaderPrepareService in its own process,
/// which writes its `shaders progress D T` / `shaders ready` lines into `logs/shaders.log`; the launcher follows them.
#[allow(clippy::too_many_arguments)]
pub fn prepare_shaders(
    dirs: &DataDirs,
    settings: &gt4l_core::Settings,
    display: Option<&gt4l_core::display::DisplayTarget>,
    textures: &gt4l_core::textures::TextureLocation,
    cancelled: &std::sync::atomic::AtomicBool,
    progress: &mut dyn FnMut(u32, u32, u32),
) -> Result<gt4l_core::shader_prepare::PrepareOutcome, String> {
    use gt4l_core::shader_prepare::{self, PrepareOutcome};
    use std::sync::atomic::Ordering;
    let guard = textures.shader_cache.clone();
    let snapshot = || {
        let read = |name: &str| guard.as_ref().and_then(|folder| std::fs::read(folder.join(name)).ok());
        shader_prepare::snapshot_of(
            read(shader_prepare::QUARANTINE_FILE).as_deref(),
            read(shader_prepare::COMPILING_FILE).as_deref(),
            read(shader_prepare::SUSPECTS_FILE).as_deref(),
        )
    };
    let last_total = std::cell::Cell::new(0u32);
    let outcome = shader_prepare::run_attempts(
        shader_prepare::MAX_PREPARE_ATTEMPTS,
        &|| cancelled.load(Ordering::SeqCst),
        &snapshot,
        &mut |attempt| {
            if attempt > 1 {
                progress(0, last_total.get(), attempt);
            }
            prepare_once(dirs, settings, display, textures, attempt, cancelled, &mut |done, total| {
                last_total.set(total);
                progress(done, total, attempt);
            })
        },
    );
    match outcome {
        PrepareOutcome::Failed { message, .. } => Err(message),
        outcome => Ok(outcome),
    }
}

fn prepare_once(
    dirs: &DataDirs,
    settings: &gt4l_core::Settings,
    display: Option<&gt4l_core::display::DisplayTarget>,
    textures: &gt4l_core::textures::TextureLocation,
    attempt: u32,
    cancelled: &std::sync::atomic::AtomicBool,
    progress: &mut dyn FnMut(u32, u32),
) -> Result<gt4l_core::shader_prepare::AttemptReport, String> {
    use gt4l_core::launch::{self, LaunchInputs, ShaderPrepareLine};
    use std::io::{Read, Seek, SeekFrom, Write};
    use std::sync::atomic::Ordering;
    let logs = dirs.logs();
    std::fs::create_dir_all(&logs).map_err(|error| format!("logs folder: {error}"))?;
    let log_path = logs.join("shaders.log");
    if attempt <= 1 {
        std::fs::write(&log_path, b"").map_err(|error| format!("shaders.log: {error}"))?;
    }
    let mut header = std::fs::OpenOptions::new().append(true).open(&log_path).map_err(|error| format!("shaders.log: {error}"))?;
    let _ = header.write_all(format!("=== shader prepare, attempt {attempt} ===\n").as_bytes());
    let start = header.seek(SeekFrom::End(0)).unwrap_or(0);
    drop(header);
    // Only the environment of the plan is used (the renderer, the card, the shader cache folder, the options).
    let dummy = Path::new("libgt4.so");
    let inputs = LaunchInputs { game_executable: dummy, elf: dummy, disc: dummy, card: dummy, session_dir: &logs };
    let plan = launch::plan_coregs20_launch(&inputs, settings, display, false, textures, &[]);
    let environment: Vec<String> = plan.environment.iter().map(|(name, value)| format!("{name}={value}")).collect();
    let log_text = log_path.display().to_string();
    let _: Done = bridge()?
        .run_mobile_plugin("prepareShaders", ShaderArgs { environment, log: &log_text })
        .map_err(|error| format!("shader prepare: {error}"))?;
    let began = Instant::now();
    let mut offset = start;
    let mut pending = String::new();
    let (mut finished, mut best_done, mut seen_alive) = (false, 0u32, false);
    loop {
        std::thread::sleep(std::time::Duration::from_millis(150));
        if let Ok(mut file) = std::fs::File::open(&log_path) {
            if file.seek(SeekFrom::Start(offset)).is_ok() {
                let mut chunk = String::new();
                if let Ok(read) = file.read_to_string(&mut chunk) {
                    offset += read as u64;
                    pending.push_str(&chunk);
                }
            }
        }
        while let Some(end) = pending.find('\n') {
            let line: String = pending.drain(..=end).collect();
            match launch::parse_shader_prepare_line(line.trim_end()) {
                Some(ShaderPrepareLine::Progress { done, total }) => {
                    best_done = best_done.max(done);
                    progress(done, total);
                }
                Some(ShaderPrepareLine::Ready) => finished = true,
                None => {}
            }
        }
        let alive = shaders_alive();
        seen_alive |= alive;
        if cancelled.load(Ordering::SeqCst) {
            let _: Result<Done, _> = bridge()?.run_mobile_plugin("stopShaders", ());
            break;
        }
        // Ended (or never came up within 10 s, or still going after 30 minutes: a hung driver).
        if (!alive && (seen_alive || began.elapsed().as_secs() > 10)) || began.elapsed().as_secs() > 1800 {
            if alive {
                let _: Result<Done, _> = bridge()?.run_mobile_plugin("stopShaders", ());
            }
            break;
        }
    }
    if let Ok(mut log) = std::fs::OpenOptions::new().append(true).open(&log_path) {
        let _ = log.write_all(
            format!("=== attempt {attempt} ended {} after {} s ===\n", if finished { "(finished)" } else { "(not finished)" }, began.elapsed().as_secs())
                .as_bytes(),
        );
    }
    Ok(gt4l_core::shader_prepare::AttemptReport { finished, best_done })
}

/// Brings the minimized game back (its activity unpauses itself).
pub fn resume_game(shared: &Shared) -> Result<(), String> {
    let dirs = state::lock(shared).dirs.clone();
    if !game_alive(&dirs) {
        return Err("The game is not running any more; press Play.".to_string());
    }
    state::log(&dirs, "resume: the minimized game comes back");
    let _: Done = bridge()?.run_mobile_plugin("resumeGame", ()).map_err(|error| format!("resume: {error}"))?;
    Ok(())
}

#[derive(Serialize)]
struct UrlArgs<'a> {
    url: &'a str,
}

#[derive(Deserialize)]
struct Done {}

#[derive(Deserialize, Default)]
struct DeviceInfo {
    #[serde(default)]
    lines: Vec<String>,
}

#[derive(Serialize)]
struct ExitsArgs<'a> {
    since: i64,
    folder: &'a str,
}

/// One end of the game's process, as Android recorded it (ApplicationExitInfo).
#[derive(Deserialize, Debug)]
#[serde(rename_all = "camelCase")]
struct GameExit {
    time: i64,
    pid: i32,
    #[serde(default)]
    process: String,
    reason: i32,
    reason_name: String,
    status: i32,
    importance: i32,
    #[serde(default)]
    description: String,
    #[serde(default)]
    pss_kb: i64,
    #[serde(default)]
    rss_kb: i64,
    #[serde(default)]
    trace_file: Option<String>,
    #[serde(default)]
    trace_error: Option<String>,
}

#[derive(Deserialize, Default)]
struct GameExits {
    #[serde(default)]
    exits: Vec<GameExit>,
}

// Logcat (liblog): the launcher's lines under GT4Recomp-launcher, as the game's under GT4Recomp-game.
#[link(name = "log")]
extern "C" {
    fn __android_log_write(priority: std::ffi::c_int, tag: *const std::ffi::c_char, text: *const std::ffi::c_char) -> std::ffi::c_int;
}

/// The app's external files folder (`/sdcard/Android/data/<package>/files`), read at start.
static EXTERNAL_DIR: OnceLock<PathBuf> = OnceLock::new();

#[derive(Deserialize, Default)]
struct ExternalDir {
    #[serde(default)]
    path: String,
}

fn load_external_dir() {
    if let Ok(found) = bridge().and_then(|bridge| bridge.run_mobile_plugin::<ExternalDir>("externalDir", ()).map_err(|error| error.to_string())) {
        if !found.path.is_empty() {
            let folder = PathBuf::from(found.path);
            // TASK-180 test3 (part A): the bench folder, created by the app so it owns it. A folder made by adb (root)
            // is not statable by the app even when the files inside are readable, which refused a pushed bench card.
            let _ = std::fs::create_dir_all(folder.join("bench"));
            let _ = EXTERNAL_DIR.set(folder);
        }
    }
}

/// TASK-180 test2 (remote diagnosis): `debug.env` in the external files folder, pushed with adb, adds variables to the
/// next Play. Only `GT4_*` names are taken; everything else is logged as ignored. No file: nothing changes.
fn debug_environment(dirs: &DataDirs) -> Vec<(String, String)> {
    let Some(path) = EXTERNAL_DIR.get().map(|folder| folder.join("debug.env")) else {
        return Vec::new();
    };
    let Ok(text) = std::fs::read_to_string(&path) else {
        return Vec::new();
    };
    let mut out = Vec::new();
    for line in text.lines().map(str::trim).filter(|line| !line.is_empty() && !line.starts_with('#')) {
        match line.split_once('=') {
            Some((name, value))
                if name.starts_with("GT4_") && name.bytes().all(|byte| byte.is_ascii_alphanumeric() || byte == b'_') =>
            {
                state::log(dirs, &format!("debug.env: {name}={value}"));
                out.push((name.to_string(), value.to_string()));
            }
            _ => state::log_warn(dirs, &format!("debug.env: ignored \"{line}\" (only GT4_NAME=VALUE lines are taken)")),
        }
    }
    out
}

// ---- Bench mode (TASK-180 test3, part A) ---------------------------------------------------------------------------

/// The bench session in progress: the card copy and its session folder, both removed when the game ends. Play refuses a
/// card outside the app's external files folder, so a bench can never open a card the player keeps elsewhere.
#[derive(Clone, Debug)]
struct BenchSession {
    card: PathBuf,
    session: PathBuf,
}

static BENCH_SESSION: Mutex<Option<BenchSession>> = Mutex::new(None);

/// What `GT4_BENCH_*` in debug.env asks for this Play.
#[derive(Default)]
struct BenchRequest {
    /// The recording's card, to be copied into the session folder and passed as `--memcard`.
    card: Option<PathBuf>,
    /// The game the bench starts, whatever the home page shows.
    game: Option<&'static gt4l_core::games::Game>,
}

impl BenchRequest {
    /// Whether the game library a bench asks for is in this app. TASK-180 test5: both are, `libgt4.so` for v2.00 and
    /// `libgt4spec2.so` for Spec II, so every bench the port offers starts the library its game needs.
    fn has_library(&self, game: &gt4l_core::games::Game) -> bool {
        game.id == gt4l_core::games::GT4.id || game.id == gt4l_core::games::SPEC2.id
    }
}

/// Reads the bench request from debug.env's values. The card must be an absolute `.ps2` path inside the app's external
/// files folder (`/sdcard/Android/data/<package>/files/...`, where adb pushes a recording's card); anything else stops
/// Play with a message in the launcher log, because a bench must never touch the player's own card.
fn bench_request(debug: &[(String, String)], dirs: &DataDirs) -> Result<BenchRequest, String> {
    let value = |name: &str| debug.iter().find(|(key, _)| key == name).map(|(_, text)| text.clone());
    let mut request = BenchRequest::default();
    if let Some(game) = value("GT4_BENCH_GAME") {
        request.game = Some(match game.trim() {
            "gt4" => &gt4l_core::games::GT4,
            "spec2" => &gt4l_core::games::SPEC2,
            other => {
                let message = format!("bench: GT4_BENCH_GAME={other} is not gt4 or spec2; Play refused");
                state::log_error(dirs, &message);
                return Err(message);
            }
        });
    }
    if let Some(text) = value("GT4_BENCH_CARD") {
        let path = PathBuf::from(text.trim());
        // The Java side reports /storage/emulated/0/...; debug.env is written by hand, often with /sdcard/... (the same
        // place through a symlink, and /storage/self/primary too). Both spellings are reduced to one before the check.
        let external = EXTERNAL_DIR.get().cloned();
        let inside = external.as_ref().is_some_and(|folder| external_form(&path).starts_with(&external_form(folder)));
        if !path.is_absolute() || !inside {
            let message = format!(
                "bench: GT4_BENCH_CARD={} is not an absolute path under the app's external files folder {}; Play refused",
                path.display(),
                external.as_ref().map(|folder| folder.display().to_string()).unwrap_or_else(|| "(not known yet)".into())
            );
            state::log_error(dirs, &message);
            return Err(message);
        }
        if path.extension().and_then(|extension| extension.to_str()).map(str::to_ascii_lowercase).as_deref() != Some("ps2") {
            let message = format!("bench: GT4_BENCH_CARD={} is not a .ps2 card; Play refused", path.display());
            state::log_error(dirs, &message);
            return Err(message);
        }
        if !path.is_file() {
            let message = format!("bench: GT4_BENCH_CARD={} does not exist; Play refused", path.display());
            state::log_error(dirs, &message);
            return Err(message);
        }
        request.card = Some(path);
    }
    Ok(request)
}

/// One spelling of an external-storage path: `/sdcard/...`, `/storage/emulated/0/...` and `/storage/self/primary/...` are
/// the same place, so all three become `/storage/emulated/0/...` before a comparison.
fn external_form(path: &Path) -> PathBuf {
    let text = path.to_string_lossy();
    for alias in ["/sdcard/", "/storage/self/primary/"] {
        if let Some(rest) = text.strip_prefix(alias) {
            return PathBuf::from(format!("/storage/emulated/0/{rest}"));
        }
    }
    path.to_path_buf()
}

/// Removes a bench session's card copy left behind by a launcher that was killed with the game (the normal end goes
/// through [`finish_bench_session`]): a session folder with `bench-card.ps2` but no live game. Called once at start-up.
fn remove_stale_bench_cards(dirs: &DataDirs) {
    let Ok(entries) = std::fs::read_dir(dirs.sessions()) else {
        return;
    };
    for entry in entries.flatten() {
        let card = entry.path().join("bench-card.ps2");
        if card.is_file() && std::fs::remove_file(&card).is_ok() {
            state::log(dirs, &format!("bench: stale session card {} removed at start-up", card.display()));
        }
    }
}

/// Removes the session's card copy when the game ends (the recording's own card is never modified: the copy is what the
/// game wrote). Called from the game-state poll, and at start for a copy a killed launcher left behind.
fn finish_bench_session(dirs: &DataDirs) {
    let session = BENCH_SESSION.lock().unwrap_or_else(|poisoned| poisoned.into_inner()).take();
    if let Some(session) = session {
        match std::fs::remove_file(&session.card) {
            Ok(()) => state::log(dirs, &format!("bench: session card {} removed; the player's card was not touched", session.card.display())),
            Err(error) => state::log_warn(dirs, &format!("bench: session card {} not removed: {error}", session.card.display())),
        }
        let _ = std::fs::remove_dir(&session.session);
    }
}

/// The screen read from the Java side at start (`displayInfo`): width and height in landscape, refresh rate.
static SCREEN: OnceLock<(gt4l_core::display::Size, u32)> = OnceLock::new();

#[derive(Deserialize, Default)]
#[serde(rename_all = "camelCase")]
struct DisplayInfo {
    width: u32,
    height: u32,
    refresh_hz: f32,
}

/// Reads the screen's current mode once (from a worker thread: the bridge answers through the main thread), then
/// converts the Render scale with it: the row is a percentage of the screen's height, and a file written under the old
/// model (or a new one, whose first value is the table's 100%) becomes this screen's default before the first Play, so
/// the Settings page shows the value the phone will use (owner, 2026-11-06).
fn load_screen(dirs: &DataDirs, shared: &Shared) {
    match bridge().and_then(|bridge| bridge.run_mobile_plugin::<DisplayInfo>("displayInfo", ()).map_err(|error| error.to_string())) {
        Ok(info) if info.width > 0 && info.height > 0 => {
            let size = gt4l_core::display::Size::new(info.width.max(info.height), info.width.min(info.height));
            let _ = SCREEN.set((size, info.refresh_hz.round() as u32));
            state::log(dirs, &format!("screen: {} at {:.2} Hz (the Render scale's output)", size.text(), info.refresh_hz));
            let mut state = state::lock(shared);
            if crate::state::migrate_render_scale_at(size, &mut state.config, dirs) {
                state.save_config();
            }
        }
        Ok(_) => state::log_warn(dirs, "screen: the display reported no size"),
        Err(message) => state::log_warn(dirs, &format!("screen: not read ({message})")),
    }
}

/// The phone's screen as the launcher's display model sees a monitor (TASK-180 test2, owner: 100% is the screen's
/// native output, as a 1440p monitor on the PC): the mode read at start, else the one MainActivity put in the
/// environment (`GT4_ANDROID_DISPLAY=3088x1440@120`; not visible to an arm64 library run by an x86 emulator's
/// translation, hence the bridge first).
pub fn screen() -> Option<(gt4l_core::display::Size, u32)> {
    if let Some(screen) = SCREEN.get() {
        return Some(*screen);
    }
    let text = std::env::var("GT4_ANDROID_DISPLAY").ok()?;
    let (size, refresh) = text.split_once('@').unwrap_or((text.as_str(), "0"));
    let (width, height) = size.split_once('x')?;
    let (width, height): (u32, u32) = (width.trim().parse().ok()?, height.trim().parse().ok()?);
    let refresh = refresh.trim().parse::<f32>().map(|hertz| hertz.round() as u32).unwrap_or(0);
    (width > 0 && height > 0).then(|| (gt4l_core::display::Size::new(width.max(height), width.min(height)), refresh))
}

/// The monitors of the display model: the phone's screen, its current mode as the desktop and only mode.
pub fn monitors() -> Vec<gt4l_core::display::Monitor> {
    match screen() {
        Some((size, _)) => vec![gt4l_core::display::Monitor {
            device: "android-screen".to_string(),
            name: "Phone screen".to_string(),
            primary: true,
            desktop: size,
            modes: vec![size],
        }],
        None => Vec::new(),
    }
}

/// Writes one launcher log line to logcat (tag `GT4Recomp-launcher`).
pub fn logcat(level: gt4l_core::logs::Level, message: &str) {
    let priority = match level {
        gt4l_core::logs::Level::Info => 4,
        gt4l_core::logs::Level::Warn => 5,
        gt4l_core::logs::Level::Error => 6,
    };
    // TASK-180 test2: also into the external files folder, which adb can read on a release build:
    // adb pull /sdcard/Android/data/io.github.moesuito.gt4recomp/files/logs/launcher.log
    if let Some(folder) = EXTERNAL_DIR.get() {
        let logs = folder.join("logs");
        let _ = std::fs::create_dir_all(&logs);
        if let Ok(mut file) = std::fs::OpenOptions::new().create(true).append(true).open(logs.join("launcher.log")) {
            use std::io::Write;
            let stamp = gt4l_core::logs::LocalTime::stamp(&gt4l_platform::system::local_time());
            let _ = writeln!(file, "{stamp} {level:?} {message}");
        }
    }
    let text = std::ffi::CString::new(message.replace('\0', " ")).unwrap_or_default();
    // SAFETY: both strings are NUL-terminated and live through the call.
    unsafe {
        __android_log_write(priority, c"GT4Recomp-launcher".as_ptr(), text.as_ptr());
    }
}

/// The phone's facts for system.txt (model, Android version, SoC, memory, display, Vulkan features).
pub fn device_lines() -> Vec<String> {
    match bridge().and_then(|bridge| {
        bridge.run_mobile_plugin::<DeviceInfo>("deviceInfo", ()).map_err(|error| error.to_string())
    }) {
        Ok(info) => info.lines,
        Err(message) => vec![format!("device facts unavailable: {message}")],
    }
}

/// The folder of the game's exit records: `logs/game-exits.log`, the raw tombstones and their text.
fn exits_folder(dirs: &DataDirs) -> PathBuf {
    dirs.logs().join("game-exits")
}

/// Records the ends of the game's process that Android reported since the last call: one line each in the launcher log
/// and in `logs/game-exits/game-exits.log`, and a native crash's tombstone as text next to its raw file. The time of the
/// newest one is kept in `logs/game-exits/seen`, so each end is recorded once. Returns how many were new.
pub fn record_game_exits(dirs: &DataDirs) -> usize {
    let folder = exits_folder(dirs);
    if let Err(error) = std::fs::create_dir_all(&folder) {
        state::log_warn(dirs, &format!("game exits: {}: {error}", folder.display()));
        return 0;
    }
    let seen_path = folder.join("seen");
    let since: i64 = std::fs::read_to_string(&seen_path).ok().and_then(|text| text.trim().parse().ok()).unwrap_or(0);
    let folder_text = folder.display().to_string();
    let exits = match bridge().and_then(|bridge| {
        bridge
            .run_mobile_plugin::<GameExits>("gameExits", ExitsArgs { since, folder: &folder_text })
            .map_err(|error| error.to_string())
    }) {
        Ok(found) => found.exits,
        Err(message) => {
            state::log_warn(dirs, &format!("game exits could not be read: {message}"));
            return 0;
        }
    };
    let mut newest = since;
    for exit in &exits {
        newest = newest.max(exit.time);
        let when = gt4l_platform::system::utc_time_of(std::time::UNIX_EPOCH + std::time::Duration::from_millis(exit.time.max(0) as u64));
        let mut line = format!(
            "{} process {} ended {} UTC: {} (reason {}, status {}, importance {}), memory pss {} MB rss {} MB",
            if exit.process.ends_with(":game") {
                "game"
            } else if exit.process.ends_with(":shaders") {
                "shader prepare"
            } else {
                "launcher"
            },
            exit.pid,
            when.stamp(),
            exit.reason_name,
            exit.reason,
            exit.status,
            exit.importance,
            exit.pss_kb / 1024,
            exit.rss_kb / 1024
        );
        if !exit.description.is_empty() {
            line.push_str(&format!("; {}", exit.description));
        }
        if let Some(trace) = &exit.trace_file {
            line.push_str(&format!("; trace {trace}"));
            if trace.ends_with(".pb") {
                match std::fs::read(trace).map(|bytes| gt4l_core::tombstone::to_text(&bytes, 400)) {
                    Ok(Some(text)) => {
                        let text_path = PathBuf::from(trace.trim_end_matches(".pb").to_string() + ".txt");
                        if let Err(error) = std::fs::write(&text_path, text) {
                            line.push_str(&format!(" (text not written: {error})"));
                        }
                    }
                    Ok(None) => line.push_str(" (not a readable tombstone)"),
                    Err(error) => line.push_str(&format!(" (unreadable: {error})")),
                }
            }
        }
        if let Some(error) = &exit.trace_error {
            line.push_str(&format!("; trace unreadable: {error}"));
        }
        // A failure: a crash (Java 4, native 5), not responding (6), an initialization failure (7), low memory (3),
        // excessive resource use (9), or the game's own exit with a status other than 0 (android_main.cpp ends the process
        // with the game's result). The rest (the activity closed, the player stopped the app) is a normal end.
        const EXIT_SELF: i32 = 1;
        let failed = matches!(exit.reason, 3..=7 | 9) || (exit.reason == EXIT_SELF && exit.status != 0);
        if failed {
            state::log_error(dirs, &line);
        } else {
            state::log(dirs, &line);
        }
        let mut file = std::fs::OpenOptions::new().create(true).append(true).open(folder.join("game-exits.log"));
        if let Ok(file) = file.as_mut() {
            use std::io::Write;
            let _ = writeln!(file, "{line}");
        }
    }
    if newest != since {
        let _ = std::fs::write(&seen_path, newest.to_string());
    }
    exits.len()
}

/// Lets the player pick a document. `None` when cancelled.
pub fn pick_document(mime_types: &[&str], persist: bool, copy_to: Option<&Path>) -> Result<Option<Picked>, String> {
    let picked: Picked = bridge()?
        .run_mobile_plugin(
            "pickDocument",
            PickArgs { mime_types, persist, copy_to: copy_to.map(|path| path.display().to_string()) },
        )
        .map_err(|error| {
            logcat(gt4l_core::logs::Level::Error, &format!("document picker failed: {error}"));
            format!("document picker: {error}")
        })?;
    logcat(
        gt4l_core::logs::Level::Info,
        &format!(
            "document picker: {} ({}), persistable permission {}",
            picked.uri.as_deref().unwrap_or("cancelled"),
            picked.name.as_deref().unwrap_or("-"),
            persist
        ),
    );
    Ok(picked.uri.is_some().then_some(picked))
}

/// Lets the player pick a folder on the phone for replacement textures, with the system folder picker. The app keeps a
/// persistable read permission to the tree. `None` when the player cancels.
pub fn pick_textures_folder() -> Result<Option<PickedTree>, String> {
    let picked: PickedTree = bridge()?
        .run_mobile_plugin("pickTexturesFolder", ())
        .map_err(|error| {
            logcat(gt4l_core::logs::Level::Error, &format!("textures folder picker failed: {error}"));
            format!("textures folder picker: {error}")
        })?;
    Ok(picked.uri.is_some().then_some(picked))
}

/// Copies the picked folder into `destination` (the app's own storage), returning how many files were written. The
/// ARMSX2 GS reads plain file paths, never document URIs; a later start reads the copy, so this runs only when the
/// player chooses a folder.
pub fn sync_textures_tree(uri: &str, destination: &Path) -> Result<u64, String> {
    let synced: SyncedTree = bridge()?
        .run_mobile_plugin(
            "syncTexturesTree",
            SyncTreeArgs { uri, destination: &destination.display().to_string() },
        )
        .map_err(|error| {
            logcat(gt4l_core::logs::Level::Error, &format!("textures folder copy failed: {error}"));
            format!("textures folder copy: {error}")
        })?;
    Ok(synced.files)
}

/// Opens a document for reading: the descriptor the system hands the app for a permission it holds.
pub fn open_document(uri: &str) -> std::io::Result<File> {
    let descriptor: Descriptor = bridge()
        .map_err(std::io::Error::other)?
        .run_mobile_plugin("openFd", UriArgs { uri })
        .map_err(|error| {
            logcat(gt4l_core::logs::Level::Error, &format!("cannot open {uri}: {error}"));
            std::io::Error::new(std::io::ErrorKind::PermissionDenied, error.to_string())
        })?;
    if descriptor.fd < 0 {
        return Err(std::io::Error::new(std::io::ErrorKind::NotFound, format!("{uri} cannot be opened")));
    }
    // SAFETY: the Kotlin side detached this descriptor from its ParcelFileDescriptor for us; it is ours to own and close.
    Ok(unsafe { File::from_raw_fd(descriptor.fd) })
}

/// Lets the player save `source` as a new document. False when cancelled.
pub fn save_document(source: &Path, name: &str, mime_type: &str) -> Result<bool, String> {
    let saved: Saved = bridge()?
        .run_mobile_plugin("saveDocument", SaveArgs { source: &source.display().to_string(), name, mime_type })
        .map_err(|error| format!("save as: {error}"))?;
    Ok(saved.saved)
}

/// Starts the game activity (`:game` process) with the launch contract. `game` picks the library: `gt4` (v2.00) or
/// `spec2` (Spec II, TASK-180 test5).
pub fn start_game(
    arguments: &[String],
    environment: &[(String, String)],
    texts: Vec<String>,
    game: &str,
) -> Result<(), String> {
    let environment = environment.iter().map(|(name, value)| format!("{name}={value}")).collect();
    let _: Done = bridge()?
        .run_mobile_plugin("startGame", GameArgs { arguments, environment, texts, game })
        .map_err(|error| format!("game start: {error}"))?;
    Ok(())
}

/// Opens a web page in the browser (project links, the download page of a newer version).
pub fn open_url(url: &str) -> Result<(), String> {
    let _: Done = bridge()?.run_mobile_plugin("openUrl", UrlArgs { url }).map_err(|error| format!("{url}: {error}"))?;
    Ok(())
}

/// A file in the app's cache, for imports and exports that pass through a plain file.
pub fn cache_file(app: &tauri::AppHandle, name: &str) -> Result<PathBuf, String> {
    let folder = app.path().app_cache_dir().map_err(|error| error.to_string())?.join("transfer");
    std::fs::create_dir_all(&folder).map_err(|error| format!("{}: {error}", folder.display()))?;
    let safe: String = name.chars().map(|c| if c.is_alphanumeric() || ".-_ ".contains(c) { c } else { '_' }).collect();
    Ok(folder.join(if safe.is_empty() { "file".to_string() } else { safe }))
}

/// The app: the same commands as the desktop launcher (minus the tray), the bridge, and the data in app storage.
pub fn run() {
    let started = Instant::now();
    gt4l_disc::source::set_document_opener(open_document);
    let result = tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(bridge_plugin())
        .setup(move |app| {
            // App-specific storage: removed with the app, never shared with other apps. Large data (the prepared game
            // files, sessions, logs) under data\, the player's things (settings, controls, the save library) under user\.
            let root = app.path().app_data_dir()?;
            let dirs = DataDirs::new(root.join("data"));
            std::fs::create_dir_all(dirs.root())?;
            let header = vec![
                format!("GT4Recomp {} (Android)", gt4l_core::LAUNCHER_VERSION),
                format!("Date: {}", gt4l_core::logs::LocalTime::stamp(&gt4l_platform::system::local_time())),
            ];
            if let Err(message) = crate::session_log::start(&dirs, &header) {
                eprintln!("GT4Recomp cannot open its log: {message}");
            }
            let user = UserDirs::new(root.join("user"), UserRootSource::AppData);
            let pins = Pins::embedded()?;
            let app_state = AppState::load_with(dirs.clone(), pins, user.clone(), None, root.join("install"))?;
            state::log(&dirs, &format!("GT4Recomp {} (Android) starting, data in {}", gt4l_core::LAUNCHER_VERSION, root.display()));
            // The app's external files folder at once (a bench card path is checked against it); the screen comes from a
            // worker thread, because the bridge answers through the main thread and is not ready inside setup.
            load_external_dir();
            let shared: Shared = Arc::new(Mutex::new(app_state));
            // The phone's facts and the game's last ends, read off the main thread (the bridge answers through it); the
            // screen read also converts the Render scale, so the shared state is made before the thread starts.
            let start_dirs = dirs.clone();
            let start_shared = shared.clone();
            std::thread::spawn(move || {
                load_screen(&start_dirs, &start_shared);
                for line in device_lines() {
                    state::log(&start_dirs, &line);
                }
                record_game_exits(&start_dirs);
                remove_stale_bench_cards(&start_dirs);
            });
            app.manage(shared);
            app.manage(Arc::new(Guard { page: PageReady::default(), dirs, user_root: user.root().to_path_buf(), started }));
            // TASK-180 test3 (owner's phone): the pad commands (capture_binding, pad_status, the Controls page's live
            // view, the Wheel page's status) need this state on Android too. It was only managed on Windows, so every
            // one of them stopped with "state not managed for fields 'pads'".
            let pads = crate::pad_service::spawn(app.handle().clone());
            app.manage(pads);
            crate::managed_states::check(app.handle());
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            crate::commands::app_state,
            crate::commands::get_settings_table,
            crate::commands::set_setting,
            crate::commands::reset_gs_settings_to_defaults,
            crate::renderer_switch::gsdx_download_needed,
            crate::renderer_switch::download_gsdx,
            crate::renderer_switch::install_gsdx_archive,
            crate::renderer_switch::coregs20_offer,
            crate::renderer_switch::use_coregs20,
            crate::setup::setup_choose_disc,
            crate::setup::setup_start,
            crate::setup::setup_cancel,
            crate::commands::prepare_shaders,
            crate::commands::cancel_shader_prepare,
            crate::commands::play,
            crate::commands::resume_game,
            crate::commands::first_start_hint,
            crate::commands::stop_game,
            crate::commands::saves_list,
            crate::commands::saves_import,
            crate::commands::saves_activate,
            crate::commands::saves_delete,
            crate::commands::saves_new,
            crate::commands::saves_export,
            crate::commands::saves_rename,
            crate::commands::replays_list,
            crate::commands::replays_import,
            crate::commands::replays_delete,
            crate::webview_guard::ui_ready,
            crate::webview_guard::ui_log,
            crate::commands::get_controls,
            crate::commands::pad_status,
            crate::commands::capture_binding,
            crate::commands::cancel_capture,
            crate::commands::reset_binding,
            crate::commands::assign_binding,
            crate::commands::reset_all_bindings,
            crate::wheel_page::get_wheel_config,
            crate::wheel_page::set_wheel_setting,
            crate::wheel_page::apply_wheel_preset,
            crate::wheel_page::assign_wheel_binding,
            crate::wheel_page::reset_wheel_binding,
            crate::wheel_page::reset_wheel_config,
            crate::wheel_page::capture_wheel_binding,
            crate::wheel_page::cancel_wheel_capture,
            crate::wheel_page::test_wheel_ffb,
            crate::wheel_page::wheel_watch,
            crate::wheel_page::get_wheel_session_check,
            crate::wheel_page::list_wheel_presets,
            crate::wheel_page::save_wheel_preset,
            crate::wheel_page::delete_wheel_preset,
            crate::commands::about_info,
            crate::commands::release_notes,
            crate::commands::open_link,
            crate::commands::open_log_folder,
            crate::commands::open_latest_log,
            crate::commands::export_logs,
            crate::commands::open_export_folder,
            crate::commands::open_game_files_folder,
            crate::commands::open_player_folder,
            crate::commands::open_saves_folder,
            crate::commands::open_replays_folder,
            crate::commands::open_textures_folder,
            crate::commands::textures_state,
            crate::commands::choose_textures_folder,
            crate::commands::reset_textures_folder,
            crate::commands::games_list,
            crate::commands::select_game,
            crate::commands::games_remove,
            crate::commands::ra_status,
            crate::commands::ra_login,
            crate::commands::ra_logout,
            crate::commands::ra_set_options,
            crate::updates::check_updates,
            crate::updates::list_launcher_versions,
            crate::updates::install_launcher_version,
            crate::updates::apply_update,
            crate::updates::skip_update,
        ])
        .run(tauri::generate_context!());
    if let Err(error) = result {
        eprintln!("GT4Recomp could not start: {error}");
    }
}

/// Play on Android: the same checks and launch plan as `commands::prepare_play` and `game::start` on Windows, with CoreGS
/// 2.0 (the only renderer here) and no monitor list (the game sizes itself from its window: the aspect correction of
/// TASK-079/080 follows the screen as GT4_DISPLAY_ASPECT=auto does on the PC). The disc stays a document: the game
/// activity opens it itself with the app's permission.
pub fn play(shared: &Shared) -> Result<(), String> {
    use gt4l_core::launch::{self, LaunchInputs};
    use gt4l_core::messages;
    use gt4l_disc::setup::Installed;

    // A minimized game is resumed, never started twice.
    let data_dirs = state::lock(shared).dirs.clone();
    if game_alive(&data_dirs) {
        return resume_game(shared);
    }
    // The previous run's end first: a crash is in the launcher log before the next start.
    record_game_exits(&data_dirs);
    // TASK-180 test3: the Render scale stored under the old notch model is converted the first time Play knows the
    // screen (on Android the load-time attempt has no screen: the bridge is not ready inside setup).
    let monitors = monitors();
    let mut state = state::lock(shared);
    if let Some(screen) = monitors.first().map(|monitor| monitor.desktop) {
        if crate::state::migrate_render_scale_at(screen, &mut state.config, &data_dirs) {
            state.save_config();
        }
    }
    let language = state::language_of(&state);
    let settings = state.config.settings.clone();
    // TASK-180 test2: the values debug.env adds (remote diagnosis), read once here: the launch plan carries them, and
    // they decide a bench session before the card and the game are chosen.
    let debug = debug_environment(&data_dirs);
    // TASK-180 test3 (Part A): bench mode. `GT4_BENCH_CARD` (debug.env) makes Play use a copy of that card for the
    // session, so a recorded drive runs from the recording's card and the player's card is never written, backed up or
    // synced. `GT4_BENCH_GAME` picks the game (gt4 or spec2) the bench starts, whatever the home page shows.
    let bench = bench_request(&debug, &data_dirs)?;
    let game = bench.game.unwrap_or(state.selected);
    let disc = match state.installed_of(game) {
        Installed::Ready { image, .. } => image,
        Installed::NotSetUp => return Err(messages::text("game.set_up_first", language).into()),
        _ => return Err(messages::render("setup.game_files_missing_reason", language, &[("reason", "")])),
    };
    let dirs = state.game_dirs(game);
    let elf = launch::game_elf(game, &dirs.prepared());
    let card = dirs.runtime_card();
    if let Some(parent) = card.parent() {
        std::fs::create_dir_all(parent).map_err(|error| format!("runtime folder: {error}"))?;
    }
    if bench.card.is_none() {
        crate::saves::prepare_card(state.library_of(game), &card, language).inspect_err(|message| {
            state::log_error(&dirs, &format!("play: the memory card could not be prepared: {message}"))
        })?;
        state::log(&dirs, &format!("play: card ready at {}", card.display()));
    }
    let seconds = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    // Named as on Windows (session-<seconds>), so the log bundle takes its game.log.
    let session_dir = dirs.session(&format!("session-{seconds}"));
    // As on Windows: the newest KEEP_SESSIONS (20) stay, more than the 10 that Export logs carries on Android.
    gt4l_platform::game::prune_sessions(&dirs.sessions(), gt4l_platform::game::KEEP_SESSIONS);
    std::fs::create_dir_all(&session_dir).map_err(|error| format!("session folder: {error}"))?;
    let (textures, _) = state.texture_location();
    let session_card = match &bench.card {
        Some(source) => {
            let copy = session_dir.join("bench-card.ps2");
            std::fs::copy(source, &copy).map_err(|error| format!("bench card {}: {error}", source.display()))?;
            state::log(
                &dirs,
                &format!(
                    "bench: card {} copied to {} ({} bytes); the player's card {} is not written, backed up or synced in this session",
                    source.display(),
                    copy.display(),
                    std::fs::metadata(&copy).map(|metadata| metadata.len()).unwrap_or(0),
                    card.display()
                ),
            );
            BENCH_SESSION
                .lock()
                .unwrap_or_else(|poisoned| poisoned.into_inner())
                .replace(BenchSession { card: copy.clone(), session: session_dir.clone() });
            state::log(
                &dirs,
                &format!(
                    "bench: session {} runs {} with the recording's card; GT4_INPUT_REPLAY={}",
                    session_dir.display(),
                    game.display_name,
                    debug.iter().find(|(name, _)| name == "GT4_INPUT_REPLAY").map(|(_, value)| value.as_str()).unwrap_or("(none)")
                ),
            );
            copy
        }
        None => card.clone(),
    };
    let inputs = LaunchInputs {
        // TASK-180 test5: one game library per game, both packaged in the APK (android/stage-native-libs.cmd).
        game_executable: Path::new(if game.id == "spec2" { "libgt4spec2.so" } else { "libgt4.so" }),
        elf: &elf,
        disc: &disc,
        card: &session_card,
        session_dir: &session_dir,
    };
    // TASK-180 test2: the output is the phone's screen (100% = its native multiplier, as a monitor on the PC). The
    // decision is logged, so a scale seen in the game can be traced to the setting it came from.
    let display = gt4l_core::display::resolve_target(&monitors, settings.get("monitor"), settings.get("resolution"));
    let scale = launch::session_plan(&settings, display.as_ref(), false);
    state::log(
        &dirs,
        &format!(
            "play: render scale {}% gives {}x ({}x{} internal) for the screen {} ({})",
            settings.get("render_scale"),
            scale.multiplier,
            scale.internal_width,
            scale.internal_height,
            display.as_ref().map(|target| target.output.text()).unwrap_or_else(|| "unknown, 1920x1080 assumed".to_string()),
            screen().map(|(_, hertz)| format!("{hertz} Hz")).unwrap_or_else(|| "refresh unknown".to_string()),
        ),
    );
    // PRIVATE test build: the ARMSX2 GS in the game process, beside CoreGS 2.0. The launch plan is the CoreGS one
    // (same arguments, card and paths); the renderer variables and the settings files are replaced, and the CoreGS-only
    // defaults are left out (the library reads none of them).
    let armsx2 = settings.get("renderer") == gt4l_core::settings::ARMSX2_GS;
    let mut plan = launch::plan_coregs20_launch(&inputs, &settings, display.as_ref(), false, &textures, &[]);
    if armsx2 {
        plan.environment.retain(|(name, _)| !name.starts_with("GT4_HW_") && !name.starts_with("GT4_CORE2_"));
        if let Some(external) = EXTERNAL_DIR.get() {
            let paths = launch::armsx2_paths(external);
            std::fs::create_dir_all(&paths.root).map_err(|error| format!("ARMSX2 GS folder: {error}"))?;
            std::fs::create_dir_all(&paths.resources).map_err(|error| format!("ARMSX2 GS resources: {error}"))?;
            // The ARMSX2 GS reads its replacements from the folder the ini names. A folder picked on the phone (the synced
            // copy) wins; otherwise the library's own `textures` folder under its data root, which it creates. CoreGS 2.0
            // keeps the launcher's shared folder through GT4_HW_TEXTURES (the plan above).
            let armsx2_textures = if textures.custom {
                textures.folder.clone()
            } else {
                paths.root.join("textures")
            };
            let text = launch::armsx2_config_text(&settings, &paths, &armsx2_textures, display.as_ref())?;
            if let Err(error) = gt4l_core::gs_table::write_ini_atomic(&paths.config, &text) {
                state::log_warn(&data_dirs, &format!("ARMSX2 GS settings: {error}"));
            }
            if !paths.overrides.exists() {
                let example = "; GT4Recomp private test build: extra GS lines for the ARMSX2 GS.
; Every [EmuCore/GS] line here is applied on top of armsx2-gs.ini, so a line you add wins.
; Example: UserHacks = true
";
                let _ = std::fs::write(&paths.overrides, example);
            }
            state::log(
                &data_dirs,
                &format!(
                    "graphics: renderer GSdx (ARMSX2) in process, multiplier {}x, vsync {}, frame rate {}, settings {}, extra {}",
                    gt4l_core::launch::format_multiplier(gt4l_core::launch::armsx2_multiplier(&settings, display.as_ref())),
                    settings.get("vsync"),
                    settings.get("frame_rate"),
                    paths.config.display(),
                    paths.overrides.display()
                ),
            );
            plan.environment.retain(|(name, _)| name != "GT4_RENDERER");
            plan.environment.push(("GT4_RENDERER".into(), "armsx2".into()));
            plan.environment.push(("GT4_ARMSX2_GS_DIR".into(), paths.root.display().to_string()));
            // The game's serial, so the GS reads its own texture folder and GameDB entry (v2.00 or Spec II).
            plan.environment.push(("GT4_GAME_SERIAL".into(), game.serial.to_string()));
            plan.environment.push(("GT4_ARMSX2_GS_CONFIG".into(), paths.config.display().to_string()));
            plan.environment.push(("GT4_ARMSX2_GS_OVERRIDES".into(), paths.overrides.display().to_string()));
            // The display mode the HUD's right-hand row shows ("[2400x1080] 60Hz"), the same environment name the
            // launcher's own display model reads. The library has no other way to ask Android for the refresh rate.
            if let Some((size, hertz)) = screen() {
                plan.environment.push(("GT4_ANDROID_DISPLAY".into(), format!("{}x{}@{}", size.width, size.height, hertz)));
            }
            // The API row (OpenGL ES by default, Vulkan experimental) and the performance HUD, which the library draws
            // itself (the game's own HUD belongs to CoreGS 2.0).
            // Auto sends no backend at all: the library keeps its own choice (and its per-GPU rules) instead of the
            // launcher forcing one.
            let api = settings.get("armsx2_api");
            if api != gt4l_core::settings::ARMSX2_API_AUTO {
                plan.environment.push(("GT4_ARMSX2_GS_RENDERER".into(), api.to_string()));
            }
            let hud = match settings.get("performance_hud") {
                gt4l_core::settings::HUD_FULL => "full",
                gt4l_core::settings::HUD_FPS_ONLY => "fps",
                _ => "off",
            };
            if hud != "off" {
                plan.environment.push(("GT4_ARMSX2_GS_HUD".into(), hud.to_string()));
            }
            // TASK-186: the flare row is the game-side local-zero answer, exactly the variables the PC GSdx path sends
            // (pcsx2_environment). With the flares off, the local readback answer keeps the car icons next to the gap time
            // and the loading picture accurate. The reset keeps a debug.env value from surviving into a later session.
            plan.environment.retain(|(name, _)| name != "GT4_GS_HIDE_FLARES" && name != "GT4_GS_READBACK_LOCAL");
            let flares = settings.gs_is_on("effect_flares");
            plan.environment.push(("GT4_GS_HIDE_FLARES".into(), if flares { "0".into() } else { "1".into() }));
            if !flares {
                plan.environment.push(("GT4_GS_READBACK_LOCAL".into(), "1".into()));
            }
        } else {
            state::log_error(&data_dirs, "graphics: the app's external files folder is unknown; ARMSX2 GS cannot start");
        }
    }
    // TASK-180 test4: the exact TASK-182 deinterlace savings are the Android default. A deinterlaced frame writes its
    // display merge straight into that field's history image (two full-display copies and two full barriers fewer per
    // deinterlaced VBlank) and the adaptive kernel skips the motion tests its loaded words already decide. Both are
    // bit-identical (TASK-182 steps 2b and 2c), so no setting gates them; they matter on the phone's 480i menus. Set
    // before debug.env is merged, so a diagnosis build can still turn either one off with GT4_CORE2_...=0.
    plan.environment.push(("GT4_CORE2_DEINT_FIELD_COMPOSE".into(), "1".into()));
    plan.environment.push(("GT4_CORE2_DEINT_SKIP_DECISION".into(), "1".into()));
    // TASK-180 test4: the phone presents from the rendering queue (`PresentPath::rendering_queue`, the path of a device
    // with one graphics queue). The picture is drawn straight into the acquired swapchain image, so the window-sized
    // picture and the second copy into the swapchain image (about 36 MB per presented frame at 3088x1440, STUDY-UNIFIED
    // MEMORY 4.2) disappear. Presentation only, so the picture is exact. debug.env still overrides it.
    plan.environment.push(("GT4_HW_PRESENT_QUEUE".into(), "0".into()));
    // TASK-182 step 3 measured on the owner's S23 showroom bench: a submission every 128 draw batches gave +11.6% VBlanks/s
    // and 32% fewer render passes. Only submission boundaries change, never what is drawn. The K rule's device default
    // (8 batches after a waited readback on a tiler) is clamped to this value by the renderer, so the two never fight.
    plan.environment.push(("GT4_CORE2_SUBMIT_EVERY".into(), "128".into()));
    state::log(
        &dirs,
        "android defaults: GT4_CORE2_DEINT_FIELD_COMPOSE=1, GT4_CORE2_DEINT_SKIP_DECISION=1 (TASK-182, exact), \
         GT4_HW_PRESENT_QUEUE=0 (single-queue present), GT4_CORE2_SUBMIT_EVERY=128 (TASK-182 step 3, exact)",
    );
    let controls_path = state.user_dirs.controls_file();
    if controls_path.exists() {
        plan.environment.push(("GT4_CONTROLS_FILE".into(), controls_path.display().to_string()));
    }
    if let Some(choice) = launch::controller_environment(&settings) {
        plan.environment.push(("GT4_PAD".into(), choice));
    }
    let mappings = state.user_dirs.config_dir().join(launch::GAMEPAD_MAPPINGS_FILE);
    if mappings.exists() {
        plan.environment.push(("SDL_GAMECONTROLLERCONFIG_FILE".into(), mappings.display().to_string()));
    }
    // RetroAchievements needs the desktop HTTP and overlay layers, not built for Android yet.
    plan.environment.push(("GT4_RA_ENABLED".into(), "0".into()));
    // debug.env (remote diagnosis): its GT4_* values replace the plan's; the game logs them again (GT4_ANDROID_DEBUG_ENV).
    let debug = debug.clone();
    if !debug.is_empty() {
        plan.environment.retain(|(name, _)| !debug.iter().any(|(added, _)| added == name));
        let names: Vec<&str> = debug.iter().map(|(name, _)| name.as_str()).collect();
        plan.environment.push(("GT4_ANDROID_DEBUG_ENV".into(), names.join(",")));
        plan.environment.extend(debug.iter().cloned());
    }
    plan.environment.push(("GT4_ANDROID_LOG".into(), plan.log_path.display().to_string()));
    // The game log also goes to the external files folder (adb pull .../files/sessions/<session>/game.log).
    if let (Some(folder), Some(name)) = (EXTERNAL_DIR.get(), session_dir.file_name()) {
        let mirror = folder.join("sessions").join(name);
        if std::fs::create_dir_all(&mirror).is_ok() {
            plan.environment.push(("GT4_ANDROID_LOG_MIRROR".into(), mirror.join("game.log").display().to_string()));
        }
    }
    plan.environment.push(("GT4_ANDROID_CWD".into(), plan.working_directory.display().to_string()));
    state::log(&dirs, &format!("game start (Android): session {}", session_dir.display()));
    state::log(&dirs, &format!("game start: arguments {}", plan.arguments.join(" ")));
    for (name, value) in &plan.environment {
        state::log(&dirs, &format!("game start: {name}={value}"));
    }
    drop(state);
    let texts = ["game.pause_title", "game.pause_keep", "game.pause_minimize", "game.pause_close"]
        .iter()
        .map(|key| messages::text(key, language).to_string())
        .collect();
    start_game(&plan.arguments, &plan.environment, texts, game.id)
}

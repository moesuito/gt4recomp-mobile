//! The launcher's commands other than setup: the snapshot, Settings, Play, Controls, Saves and
//! Replays, and About. Each command is a thin wrapper over a crate; the rules live in `model`,
//! `saves` and the `gt4l-*` crates.

use gt4l_core::messages;
use std::fs;
use std::panic::{self, AssertUnwindSafe};
use std::path::{Path, PathBuf};
use std::sync::atomic::Ordering;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use gt4controls::{self as controls_file, default_bindings};
use gt4l_core::display::{self as display_rules, DisplayTarget};
use gt4l_core::exit::{self, Outcome};
use gt4l_core::games::{self, Game, GT4};
use gt4l_core::gs_release;
use gt4l_core::launch;
use gt4l_core::paths::{self, DataDirs};
use gt4l_core::settings;
use gt4l_core::shader_prepare::{GaveUp, PrepareOutcome};
use gt4l_core::{Platform, Settings, LAUNCHER_VERSION};
use gt4l_disc::setup::{self as disc_setup, Installed};
use gt4l_platform::game;
use serde::Serialize;
use serde_json::{json, Value};
use tauri::{AppHandle, Emitter, Manager, State};
use tauri_plugin_dialog::DialogExt;

use crate::model;
use crate::pad_service;
use crate::setup;
use crate::saves::{self, Item};
use gt4saves::Library;
use crate::state::{self, lock, AppState, SetupOutcome, Shared};
use crate::webview_guard;

/// `app_state()` (ui/dev/README.md):
/// `{setup: {state, step, progress, bytes_done, bytes_total, renderer_step, disc_name, problem, note}, game_running,
/// game_files}`.
///
/// `state` is `needed` (never set up), `wrong_disc` (the chosen file was refused), `checking` (setup runs),
/// `failed` (a run stopped: cancel, no space, write error), `files_missing` (set up once, but the copied image or
/// the prepared files are gone or changed) or `done`. No hash runs here: the copied image is trusted by its receipt.
#[tauri::command]
pub fn app_state(shared: State<'_, Shared>) -> Result<Value, String> {
    Ok(app_state_of(&lock(shared.inner())))
}

/// Whether a game is running: the child process on Windows; on Android the game's own process, which Minimize keeps
/// alive (the home page then offers Resume).
fn game_running(state: &AppState) -> bool {
    #[cfg(target_os = "android")]
    return crate::android::game_alive(&state.dirs);
    #[cfg(not(target_os = "android"))]
    state.game_child.is_some()
}

/// `resume_game()`: brings the minimized game back (Android, TASK-180 test2).
#[tauri::command]
pub fn resume_game(shared: State<'_, Shared>) -> Result<(), String> {
    #[cfg(target_os = "android")]
    return crate::android::resume_game(shared.inner());
    #[cfg(not(target_os = "android"))]
    {
        let _ = shared;
        Err("Resume is for the Android app".to_string())
    }
}

/// The game the setup section of `app_state` talks about: the game of a setup that is running or has just ended, else the
/// game the page shows.
fn setup_subject(state: &AppState) -> &'static Game {
    if state.setup_busy || !matches!(state.setup_outcome, SetupOutcome::None) {
        state.setup_game.unwrap_or(state.selected)
    } else {
        state.selected
    }
}

/// The image name the disc a game was set up from was checked as, from its receipt (empty when there is none).
fn installed_image_name(state: &AppState, game: &'static Game) -> String {
    let dirs = state.game_dirs(game);
    disc_setup::read_receipt(&dirs.prepared()).map(|receipt| receipt.image_name).unwrap_or_default()
}

/// The display name of the disc a game was set up from, as the receipt records it.
fn installed_disc_name(state: &AppState, game: &'static Game, version: &str) -> String {
    setup::game_disc_name(game, version, &installed_image_name(state, game))
}

/// Why a game set up once cannot start now, for the player: a broken link says where the image was. `None`: it was never set up,
/// or it is ready.
fn missing_reason(installed: &Installed, language: gt4l_core::language::Language) -> Option<String> {
    match installed {
        Installed::Missing { reason } => Some(reason.clone()),
        Installed::LinkBroken { image } => {
            Some(messages::render("setup.image_gone_reason", language, &[("path", &image.display().to_string())]))
        }
        Installed::NotSetUp | Installed::Ready { .. } => None,
    }
}

/// The folder that holds a game's disc image: where the player keeps it, or the launcher's own copy of an older install.
fn game_files_folder(state: &AppState, game: &'static Game) -> PathBuf {
    match state.installed_of(game) {
        Installed::Ready { image, .. } | Installed::LinkBroken { image } => {
            image.parent().map(Path::to_path_buf).unwrap_or_else(|| state.game_dirs(game).disc_folder())
        }
        _ => state.game_dirs(game).disc_folder(),
    }
}

/// One row of the game list: `{id, name, title, variant, installed, linked, selected, state, disc_name, folder, size_bytes,
/// image_path, image_bytes}`. `size_bytes` is what removing the game frees; `image_path` and `image_bytes` are the disc image the game
/// reads, wherever it is.
fn game_row(state: &AppState, game: &'static Game) -> Value {
    let installed = state.installed_of(game);
    let dirs = state.game_dirs(game);
    let (ready, version, image_path) = match &installed {
        Installed::Ready { version, image, .. } => (true, version.clone(), Some(image.clone())),
        _ => (false, String::new(), None),
    };
    let image_bytes = image_path.as_ref().and_then(|image| fs::metadata(image).ok()).map_or(0, |metadata| metadata.len());
    let reason = missing_reason(&installed, state::language_of(state)).unwrap_or_default();
    let linked = disc_setup::read_receipt(&dirs.prepared()).and_then(|receipt| receipt.image).is_some_and(|image| image.linked_path.is_some());
    let image_name = if ready { installed_image_name(state, game) } else { String::new() };
    let disc_name = if ready { setup::game_disc_name(game, &version, &image_name) } else { String::new() };
    let variant = if ready { setup::game_variant(game, &version, &image_name) } else { String::new() };
    let size_bytes = if ready { folder_size(&dirs.disc_folder()) + folder_size(&dirs.prepared()) } else { 0 };
    json!({
        "id": game.id,
        "name": game.display_name,
        "title": setup::game_title(game),
        "variant": variant,
        "installed": ready,
        "linked": linked,
        "damaged": !ready && !reason.is_empty(),
        "reason": reason,
        "selected": game == state.selected,
        "disc_name": disc_name,
        "folder": dirs.game_files_root().display().to_string(),
        "size_bytes": size_bytes,
        "image_path": image_path.map(|image| image.display().to_string()).unwrap_or_default(),
        "image_bytes": image_bytes,
    })
}

/// Total size of the files directly under or inside `folder` (0 when it is missing).
fn folder_size(folder: &Path) -> u64 {
    let Ok(entries) = fs::read_dir(folder) else {
        return 0;
    };
    entries
        .filter_map(|entry| entry.ok())
        .map(|entry| match entry.metadata() {
            Ok(meta) if meta.is_dir() => folder_size(&entry.path()),
            Ok(meta) => meta.len(),
            Err(_) => 0,
        })
        .sum()
}

pub fn app_state_of(state: &AppState) -> Value {
    let language = state::language_of(state);
    let subject = setup_subject(state);
    let installed = state.installed_of(subject);
    // TASK-180: an Android document shows its file name, not its URI.
    let chosen_name = state.disc_path().map(gt4l_disc::source::display_name).unwrap_or_default();
    let run = &state.setup_run;
    let (screen, problem, disc_name) = if visual_ready() {
        ("done", String::new(), setup::game_disc_name(&GT4, "2.00", ""))
    } else if state.setup_busy {
        ("checking", String::new(), chosen_name)
    } else if let Installed::Ready { version, .. } = &installed {
        ("done", String::new(), installed_disc_name(state, subject, version))
    } else {
        match &state.setup_outcome {
            SetupOutcome::Refused(message) => ("wrong_disc", message.clone(), chosen_name),
            SetupOutcome::Failed(message) => ("failed", message.clone(), chosen_name),
            SetupOutcome::Done { .. } => (
                "files_missing",
                messages::text("setup.game_files_missing", language).to_string(),
                chosen_name,
            ),
            SetupOutcome::None => match missing_reason(&installed, language) {
                Some(reason) => (
                    "files_missing",
                    messages::render("setup.game_files_missing_reason", language, &[("reason", &reason)]),
                    chosen_name,
                ),
                None => ("needed", String::new(), chosen_name),
            },
        }
    };
    let note = match &state.setup_outcome {
        SetupOutcome::Done { renderer_note } => renderer_note.clone(),
        _ => String::new(),
    };
    let installed_games = state.installed_games();
    // The page names a game when one is already installed, when the disc picked is identified (`setup_game`), or when this
    // game's files are damaged. A setup with nothing installed and no disc identified yet has no target: the disc decides
    // (rc5 laptop test: a stored choice of Spec II made the first-run page say "Install Spec II").
    let target = if installed_games.is_empty() && state.setup_game.is_none() && !matches!(installed, Installed::Missing { .. } | Installed::LinkBroken { .. }) {
        Value::Null
    } else {
        json!({ "id": subject.id, "name": subject.display_name })
    };
    json!({
        "setup": {
            "state": screen,
            "step": if run.step.is_empty() { "copying" } else { run.step.as_str() },
            "progress": run.fraction,
            "bytes_done": run.bytes_done,
            "bytes_total": run.bytes_total,
            "renderer_step": run.renderer_step,
            "renderer_choice": run.renderer_choice,
            "disc_name": disc_name,
            "problem": problem,
            "note": note,
            "game": target,
        },
        "game_running": game_running(state),
        "game_files": game_files_folder(state, state.selected).display().to_string(),
        "selected_game": state.selected.id,
        "any_installed": !installed_games.is_empty(),
        "language": state.config.settings.get("language"),
        // TASK-180: the page's platform rules (the update dialog downloads in the browser on Android).
        "platform": Platform::current().name(),
        // TASK-180 test3 (owner): the Wheel page is Windows only; Android offers pads only.
        "wheel": Platform::current().offers_wheel_page(),
        // TASK-185: the on-screen pad and its layout editor exist on the phone, where the game draws them.
        "virtual_pad": Platform::current().offers_virtual_pad(),
        "games": games::GAMES.iter().map(|game| game_row(state, game)).collect::<Vec<_>>(),
        "migration": {
            "running": state.migration.running,
            "item": state.migration.item,
            "bytes_done": state.migration.bytes_done,
            "bytes_total": state.migration.bytes_total,
            "notice": state.migration.notice,
            "problem": state.migration.problem,
        },
        "visual_page": visual_page(),
    })
}

#[cfg(feature = "visual-check")]
fn visual_page() -> Option<String> {
    std::env::var("GT4RECOMP_VISUAL_PAGE").ok()
}

#[cfg(not(feature = "visual-check"))]
fn visual_page() -> Option<String> {
    None
}

#[cfg(feature = "visual-check")]
fn visual_ready() -> bool {
    std::env::var("GT4RECOMP_VISUAL_READY").is_ok_and(|value| value == "1")
}

#[cfg(not(feature = "visual-check"))]
fn visual_ready() -> bool {
    false
}

// Settings ------------------------------------------------------------------------------------

/// `get_settings_table()`: `{max_upscale, rows}`, the rows of ui/dev/README.md (see `model::settings_table`).
/// TASK-080 addendum: the Monitor and Resolution rows list the connected monitors and their modes, read each time.
#[tauri::command]
pub fn get_settings_table(shared: State<'_, Shared>) -> Result<Value, String> {
    // TASK-180: on Android the output is the phone's own screen.
    #[cfg(target_os = "android")]
    let monitors = crate::android::monitors();
    #[cfg(not(target_os = "android"))]
    let monitors = gt4l_platform::display::monitors();
    get_settings_table_with_monitors(shared.inner(), &monitors)
}

/// The table without monitors (tests and the flow audit): Monitor shows "Primary monitor", Resolution the desktop.
pub fn get_settings_table_of(shared: &Shared) -> Result<Value, String> {
    get_settings_table_with_monitors(shared, &[])
}

pub fn get_settings_table_with_monitors(shared: &Shared, monitors: &[display_rules::Monitor]) -> Result<Value, String> {
    let state = lock(shared);
    // TASK-114 rc5: one card list, read from the cache (no probe here), serves both the rows and the integrated check.
    let cards = gt4l_platform::gpu::cached_gpus();
    let integrated_gpu = gt4l_platform::gpu::integrated_gpu_name_of(state.config.settings.get("graphics_card"), &cards).is_some();
    let mut table = model::settings_table_with_monitors(&state.config.settings, &state.controls, monitors, &cards, integrated_gpu);
    // TASK-091: the launcher's own row (Graphics > Advanced), not a game setting.
    let safe_mode = webview_guard::safe_mode_on(state.user_dirs.root());
    if let Some(rows) = table.get_mut("rows").and_then(Value::as_array_mut).filter(|_| Platform::current().offers(webview_guard::GPU_ROW_ID)) {
        rows.push(webview_guard::gpu_row(safe_mode));
    }
    Ok(table)
}

/// `set_setting({id, value})`: a launcher row, a PCSX2 GS table row, a trigger row (`l2_deadzone`, `r2_curve`, ...) or
/// a stick row (`lstick_deadzone`, `rstick_deadzone`).
/// A refused value leaves the setting unchanged and returns the reason.
#[tauri::command]
pub fn set_setting(app: AppHandle, id: String, value: Value, shared: State<'_, Shared>) -> Result<(), String> {
    set_setting_of(id.clone(), value, shared.inner())?;
    // TASK-089 extension (1.2.0): choosing GSdx installs it at once when it is missing (renderer_switch.rs).
    if id == "renderer" {
        crate::renderer_switch::start(app, shared.inner().clone());
    }
    Ok(())
}

pub fn set_setting_of(id: String, value: Value, shared: &Shared) -> Result<(), String> {
    // TASK-180: a row the platform does not offer (the renderer on Android, ...) is never stored (gt4l_core::platform).
    if !model::platform_accepts(Platform::current(), &id) {
        return Err(format!("'{id}' is not a setting on this platform"));
    }
    let mut state = lock(shared);
    if id == webview_guard::GPU_ROW_ID {
        // TASK-091: the launcher's own toggle; takes effect at the next start.
        let on = value.as_bool().ok_or_else(|| format!("'{id}' needs on or off"))?;
        let now = webview_guard::unix_seconds();
        return webview_guard::set_gpu_acceleration(state.user_dirs.root(), on, now);
    }
    if let Some(definition) = settings::definition(&id) {
        let text = model::value_text(&value, definition.kind == settings::Kind::Toggle)
            .ok_or_else(|| format!("'{id}' needs a plain value"))?;
        state.config.settings.set_by_user(&id, &text).map_err(|error| error.to_string())?;
        // TASK-180: on Android every change is logged (a touch can move a slider; the log shows what was stored and when).
        #[cfg(target_os = "android")]
        state::log(&state.dirs, &format!("setting {id} = {text}"));
        if id == "language" {
            // The messages from Rust follow the new language at once (the UI re-draws on its own).
            state::set_current_language(state::language_of(&state));
        }
        if id == "monitor" {
            // Another monitor has other modes: the Resolution goes back to that monitor's desktop mode.
            state.config.settings.set("resolution", display_rules::DESKTOP).map_err(|error| error.to_string())?;
        }
        if id == "resolution" {
            // TASK-088 (1.2.0): a size equal to the monitor's desktop is stored as "desktop" (it then follows the desktop
            // at every start). Any other size is stored with the monitor's identifier, which records the desktop size at
            // the time of the choice (`display::desktop_at_save`).
            let monitors = gt4l_platform::display::monitors();
            if let Some(monitor) = display_rules::resolve_monitor(&monitors, state.config.settings.get("monitor")) {
                let desktop_choice = text == display_rules::DESKTOP || display_rules::Size::parse(&text) == Some(monitor.desktop);
                if desktop_choice {
                    state.config.settings.set("resolution", display_rules::DESKTOP).map_err(|error| error.to_string())?;
                } else {
                    let identifier = display_rules::monitor_id(monitor);
                    state.config.settings.set("monitor", &identifier).map_err(|error| error.to_string())?;
                }
            }
        }
    } else if let Some((trigger, field)) = id.split_once('_').filter(|(name, _)| ["l2", "r2"].contains(name)) {
        let current = state.controls.trigger_tuning(trigger);
        let (deadzone, curve) = match field {
            "deadzone" => (
                value.as_f64().ok_or_else(|| format!("'{id}' needs a number"))? as f32,
                current.curve.as_str().to_string(),
            ),
            "curve" => (
                current.deadzone * 100.0,
                value.as_str().ok_or_else(|| format!("'{id}' needs a curve name"))?.to_string(),
            ),
            _ => return Err(format!("unknown setting '{id}'")),
        };
        let tuning = model::trigger_tuning(deadzone, &curve)?;
        return change_controls(&mut state, |controls| {
            controls.set_trigger_tuning(trigger, tuning).map_err(|error| error.to_string())
        });
    } else if let Some(stick) = model::stick_setting(&id) {
        let percent = value.as_f64().ok_or_else(|| format!("'{id}' needs a number"))?;
        if !(0.0..=100.0).contains(&percent) {
            return Err("the deadzone must be between 0 and 100 percent".into());
        }
        return change_controls(&mut state, |controls| {
            controls.set_stick_deadzone(stick, percent as f32 / 100.0).map_err(|error| error.to_string())
        });
    } else if let Some(row) = model::pcsx2_table_rows().into_iter().find(|row| row.id == id) {
        let text = model::gs_value_text(row, &value).ok_or_else(|| format!("'{id}' needs a plain value"))?;
        state.config.settings.set_gs(&id, &text).map_err(|error| error.to_string())?;
    } else {
        return Err(format!("unknown setting '{id}'"));
    }
    if id == "discord_rich_presence" {
        let enabled = state.config.settings.is_on("discord_rich_presence");
        if state.game_child.is_some() {
            state.discord.apply_toggle(enabled);
        }
    }
    state.save_config();
    Ok(())
}

/// `reset_gs_settings_to_defaults()`: every PCSX2 GS table row back to its default. Returns the new table.
#[tauri::command]
pub fn reset_gs_settings_to_defaults(shared: State<'_, Shared>) -> Result<Value, String> {
    reset_gs_settings_to_defaults_of(shared.inner())
}

pub fn reset_gs_settings_to_defaults_of(shared: &Shared) -> Result<Value, String> {
    let mut state = lock(shared);
    state.config.settings.clear_gs();
    state.save_config();
    Ok(model::settings_table(&state.config.settings, &state.controls))
}

// Play ------------------------------------------------------------------------------------------

/// Everything Play needs, checked: the copied disc image, the prepared ELF, the game program, the settings, the
/// renderer and the extra environment. The flow test builds the launch plan from this without starting anything.
pub struct PlayInputs {
    /// The game that starts (TASK-101): its executable, ELF, disc image and runtime card are the ones below.
    pub game: &'static Game,
    pub dirs: DataDirs,
    pub disc: PathBuf,
    pub elf: PathBuf,
    pub executable: PathBuf,
    pub settings: Settings,
    /// The installed GSdx; a session that uses it does not start without it. `None`: this is a CoreGS 2.0 session.
    pub pcsx2: Option<gs_release::Installed>,
    /// The resolved Monitor and Resolution (TASK-080 addendum); `None` when no monitor could be read.
    pub display: Option<DisplayTarget>,
    pub extra: Vec<(String, String)>,
    /// The note for the player when this run falls back to Direct3D 11 (TASK-108); `None` otherwise.
    pub graphics_notice: Option<String>,
    /// The API this run resolved to (`graphics.api`: `vulkan`, `d3d12` or `d3d11`). Used after the run to remember a
    /// lost Vulkan device so the next Play does not repeat it (owner, 2026-10-26).
    pub api: String,
}

/// Stores `game` as the choice the next start shows (owner, 2026-11-06): the Windows Play and the Android Play both call
/// it; a bench run does not. Kept here, outside `mod android`, so the flow audit can exercise it on every host.
pub(crate) fn remember_game_played(state: &mut AppState, game: &'static games::Game) {
    state.config.selected_game = games::after_play(game);
    state.save_config();
}

/// The checks of Play before anything starts. `program_dir` is the folder of `GT4Recomp.exe`.
pub fn prepare_play(shared: &Shared, program_dir: &Path) -> Result<PlayInputs, String> {
    let state = lock(shared);
    let language = state::language_of(&state);
    if state.game_child.is_some() {
        return Err(messages::text("game.already_running", language).into());
    }
    if state.migration.running {
        return Err(messages::text("setup.migrating", language).into());
    }
    let disc = match state.installed() {
        Installed::Ready { image, .. } => match gt4l_platform::disk::game_readable_path(&image) {
            Some(readable) => readable,
            None => return Err(messages::text("setup.path_not_readable", language).into()),
        },
        Installed::NotSetUp => return Err(messages::text("game.set_up_first", language).into()),
        other => {
            let reason = missing_reason(&other, language).unwrap_or_default();
            return Err(messages::render("setup.game_files_missing_reason", language, &[("reason", &reason)]));
        }
    };
    let game = state.selected;
    let dirs = state.game_dirs(game);
    let game_dir = paths::game_directory(program_dir, state.config.dev_game_dir.as_deref());
    let executable = launch::game_program(game, &game_dir);
    if !executable.exists() {
        return Err(messages::render("game.program_missing", language, &[("game", game.display_name)]));
    }
    let mut extra: Vec<(String, String)> = Vec::new();
    let controls_path = state.user_dirs.controls_file();
    if controls_path.exists() {
        extra.push(("GT4_CONTROLS_FILE".into(), controls_path.display().to_string()));
    }
    // TASK-181: the racing wheel file (the game's virtual Driving Force Pro), only when it exists.
    let wheel_path = state.wheel_path();
    if wheel_path.exists() {
        extra.push(("GT4_WHEEL_FILE".into(), wheel_path.display().to_string()));
    }
    // TASK-148: the controller chosen on the Controls page, and the player's SDL gamepad mappings when there are any.
    if let Some(choice) = launch::controller_environment(&state.config.settings) {
        extra.push(("GT4_PAD".into(), choice));
    }
    let mappings = state.user_dirs.config_dir().join(launch::GAMEPAD_MAPPINGS_FILE);
    if mappings.exists() {
        extra.push(("SDL_GAMECONTROLLERCONFIG_FILE".into(), mappings.display().to_string()));
    }
    if game.id == games::GT4.id {
        let ra_cfg = gt4l_core::retroachievements::RaConfig::load(&state.user_dirs.retroachievements_file());
        for (key, value) in ra_cfg.launch_environment(&dirs.retroachievements_cache(), language) {
            extra.push((key, value.to_string_lossy().into_owned()));
        }
    } else {
        // For Spec II force RA off after inherited GT4_RA_* values are removed; do not send its process credentials,
        // original-game hash or enabled RA options. Native target guard stays intact.
        extra.push(("GT4_RA_ENABLED".into(), "0".into()));
    }
    // GSdx runs the game (decision 0070) unless the player chose CoreGS 2.0 (in the game executable): without a
    // verified GSdx such a session stops here with the error.
    // TASK-108 (release 1.3.0): the graphics card and API of this run (automatic unless the player chose them). The
    // stored choices stay as they are; the run uses the resolved API, card and Adapter.
    // TASK-114 rc5: Play probes the cards once, so the run sees the hardware of this moment; the rest of Play reads that list.
    let fresh_cards = gt4l_platform::gpu::refresh_gpus();
    let (settings, graphics, cards) = gt4l_platform::gpu::run_settings_with_cards(&state.config.settings, language, fresh_cards);
    state::log(&dirs, &format!("graphics: cards [{}]", gt4l_platform::gpu::card_list(&cards)));
    for line in &graphics.log {
        state::log(&dirs, line);
    }
    let card_name = graphics.card.as_ref().and_then(gt4l_core::gpu::GpuDevice::name).unwrap_or("none");
    let adapter = if graphics.adapter.is_empty() { "system default" } else { graphics.adapter.as_str() };
    state::log(&dirs, &format!("graphics: card {card_name}, API {}, Adapter {adapter}", graphics.api));
    let coregs20 = launch::uses_coregs20(&settings);
    let pcsx2 = if coregs20 {
        state::log(&dirs, "graphics: renderer CoreGS 2.0 (Vulkan)");
        None
    } else {
        let installed = gs_release::installed(&dirs.renderer_root());
        Some(launch::choose_renderer(installed.as_ref(), &dirs.logs())?.clone())
    };
    // TASK-080 addendum: the stored Monitor and Resolution against the monitors connected now.
    let monitors = gt4l_platform::display::monitors();
    let display = display_rules::resolve_target(&monitors, settings.get("monitor"), settings.get("resolution"));
    // TASK-088 (1.2.0): a stored size the monitor no longer has plays at its desktop mode, and the log says so.
    if let Some(monitor) = display_rules::resolve_monitor(&monitors, settings.get("monitor")) {
        if let Some(desktop) = display_rules::resolution_fallback(monitor, settings.get("resolution")) {
            state::log(
                &dirs,
                &format!(
                    "display: resolution {} is not a mode of {} now; playing at its desktop {}",
                    settings.get("resolution"),
                    monitor.device,
                    desktop.text()
                ),
            );
        }
    }
    // Issue #89: on an integrated GPU the untouched default scale plays at 1x (the GPU the system check reports).
    let integrated_gpu = gt4l_platform::gpu::integrated_gpu_name_of(settings.get("graphics_card"), &cards);
    let plan = launch::session_plan(&settings, display.as_ref(), integrated_gpu.is_some());
    state::log(
        &dirs,
        &format!(
            "display: {} output {} -> {}x internal {}x{} (k {}, render scale {}%, aspect {:.4}{})",
            display.as_ref().map(|target| target.device.as_str()).unwrap_or("unknown monitor"),
            display.as_ref().map(|target| target.output.text()).unwrap_or_else(|| "unknown".into()),
            plan.multiplier,
            plan.internal_width,
            plan.internal_height,
            plan.base_multiplier,
            plan.render_scale,
            plan.aspect,
            if plan.ultrawide_on { ", aspect path on" } else { "" }
        ),
    );
    if let (true, Some(name)) = (plan.integrated_default, integrated_gpu.as_deref()) {
        state::log(&dirs, &format!("display: integrated GPU {name}: default internal {}x", plan.multiplier));
    }
    Ok(PlayInputs {
        display,
        game,
        elf: launch::game_elf(game, &dirs.prepared()),
        dirs,
        disc,
        executable,
        settings,
        pcsx2,
        extra,
        // The Direct3D 11 fallback note is about GSdx; CoreGS 2.0 always draws with Vulkan.
        graphics_notice: if coregs20 { None } else { graphics.notice },
        api: graphics.api,
    })
}

/// Starts the game with the chosen settings. Returns once the process is running; the page hears
/// about the end through the `game-state` event.
#[tauri::command]
pub fn play(app: AppHandle, shared: State<'_, Shared>) -> Result<(), String> {
    let shared: Shared = shared.inner().clone();
    // TASK-180: Android starts the game activity (android.rs); everything below is the Windows process start.
    #[cfg(target_os = "android")]
    {
        let _ = &app;
        return crate::android::play(&shared).inspect_err(|message| lock(&shared).home_message = message.clone());
    }
    // TASK-081: the processor check before the game starts (`gt4l_core::system_check`). Vulkan is checked per run in
    // `prepare_play`, and only when Vulkan is the chosen Graphics API (TASK-108).
    let language = state::language_of(&lock(&shared));
    if let Some(problem) = gt4l_platform::system::processor_problem(language) {
        let mut state = lock(&shared);
        state::log_error(&state.dirs, &format!("system requirement not met: {problem}"));
        state.home_message = problem.clone();
        return Err(problem);
    }
    let inputs = prepare_play(&shared, &program_directory()).inspect_err(|message| {
        lock(&shared).home_message = message.clone();
    })?;
    let PlayInputs { game, dirs, disc, elf, executable, settings, pcsx2, display, extra, graphics_notice, api } = inputs;
    // A lost device under CoreGS 2.0 says nothing about the API GSdx should use next time.
    let run_api = if pcsx2.is_some() { api.clone() } else { "coregs20".to_string() };
    // TASK-097: a direct start of the game reads these files; they follow the monitors connected now.
    lock(&shared).write_standalone_files(&settings);
    let card = dirs.runtime_card();
    saves::prepare_card(lock(&shared).library_of(game), &card, language)?;
    let (textures, textures_note) = lock(&shared).texture_location();
    if let Some(note) = &textures_note {
        state::log_warn(&dirs, note);
    }
    // TASK-157: a folder the player chose is read in place, never created or written to.
    if !textures.custom {
        paths::ensure_texture_folders(&textures.folder).map_err(|error| format!("texture folder: {error}"))?;
    }
    // TASK-181: the launcher closes every SDL handle first (the wheel's and its force feedback): the game owns them.
    let pads = app.try_state::<pad_service::Handle>().map(|pads| pads.inner().clone());
    if let Some(pads) = &pads {
        if !pads.release_for_game() {
            state::log_warn(&dirs, "controller input: SDL3 did not confirm it closed its devices before the game started");
        }
    }
    let running = match game::start(&dirs, &executable, &elf, &disc, &settings, pcsx2.as_ref(), display.as_ref(), &textures, &extra) {
        Ok(running) => running,
        Err(error) => {
            if let Some(pads) = &pads {
                pads.game_ended();
            }
            return Err(error);
        }
    };
    let start_unix = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0);
    {
        let mut state = lock(&shared);
        state.game_child = Some(running.child.clone());
        state.home_message.clear();
        // TASK-118: the API and the card have no "Automatic" choice any more. Store what this start resolved, so the
        // page shows the real value from the next start on.
        if state.config.settings.gs_value("graphics_api") == gt4l_core::gpu::AUTOMATIC {
            let _ = state.config.settings.set_gs("graphics_api", &api);
        }
        let resolved_card = settings.get("graphics_card").to_string();
        if state.config.settings.get("graphics_card") == gt4l_core::gpu::AUTOMATIC && !resolved_card.is_empty() {
            let _ = state.config.settings.set("graphics_card", &resolved_card);
        }
        // The game played last is the one the next start shows (owner, 2026-10-23).
        remember_game_played(&mut state, game);
        // save_config writes the stored choice; a run that fell back to Direct3D 11 keeps its own direct-start file.
        state.write_standalone_files(&settings);
        let discord_enabled = state.config.settings.is_on("discord_rich_presence");
        state.discord.on_game_start(game.display_name, start_unix, discord_enabled);
    }
    state::log(&dirs, &format!("game started ({}), session {}", game.id, running.session_dir.display()));
    // TASK-112: the last 20 game session folders are kept; the one just started is never removed.
    match gt4l_core::logs::prune_sessions(&dirs.sessions(), gt4l_core::logs::KEPT_SESSIONS, &running.session_dir) {
        Ok(removed) if !removed.is_empty() => state::log(&dirs, &format!("old game sessions removed: {}", removed.join(", "))),
        Ok(_) => {}
        Err(error) => state::log_warn(&dirs, &format!("game sessions not cleaned up: {error}")),
    }
    let _ = app.emit(
        "game-state",
        json!({ "running": true, "message": graphics_notice.clone().unwrap_or_default(), "notice": graphics_notice.is_some() }),
    );
    #[cfg(desktop)]
    crate::tray::handle(&app, crate::tray::Event::GameStarted);

    std::thread::spawn(move || {
        let outcome = game::wait_for_exit(&running);
        // GSdx failed and the player chose CoreGS 2.0 in the game's message (`use_coregs20_after_gsdx_failure`).
        // The game writes its last line as it exits; wait for it before classifying, so a normal close is never read
        // mid-write (owner, 2026-10-26: three normal closes were reported as crashes).
        let log_text = read_settled_log(&running.log_path);
        let gsdx_error = launch::restart_requested(&log_text)
            .then(|| launch::gsdx_unavailable_message(launch::GSDX_RESTART_REQUESTED, &dirs.logs()));
        // Issue #89: a lost graphics card is named in plain words, ahead of the restart and the generic crash text.
        let gpu_lost = matches!(outcome, Outcome::Crashed { .. }).then(|| exit::gpu_lost_line(&log_text)).flatten();
        match saves::harvest_card(lock(&shared).library_of(game), &card) {
            Ok(report) => state::log(&dirs, &format!("card harvested: {report:?}")),
            Err(error) => state::log_error(&dirs, &format!("card not harvested: {error}")),
        }
        let log_path = running.log_path.clone();
        drop(running);
        lock(&shared).discord.on_game_end();
        lock(&shared).game_child = None;
        if let Some(pads) = &pads {
            pads.game_ended();
        }
        let renderer_changed = gsdx_error.is_some() && {
            let stored = lock(&shared).config.settings.clone();
            let (_, graphics, _) = gt4l_platform::gpu::run_settings(&stored, state::current_language());
            use_coregs20_after_gsdx_failure(&shared, graphics.card.as_ref().is_some_and(gt4l_core::gpu::GpuDevice::has_vulkan))
        };
        // Owner (2026-10-26): Vulkan loses the device on the Intel HD 620 (issue #89). Remember it for this machine so
        // the next Play starts on Direct3D 11 instead of losing the device again.
        let switched = if gpu_lost.is_some() && run_api == "vulkan" {
            let mut state = lock(&shared);
            match state.config.settings.set_gs("graphics_api", "d3d11") {
                Ok(()) => {
                    state.save_config();
                    state::log(&dirs, "graphics: Vulkan lost the device; the stored API is now d3d11");
                    true
                }
                Err(error) => {
                    state::log_warn(&dirs, &format!("graphics: could not store d3d11 after the Vulkan loss: {error}"));
                    false
                }
            }
        } else {
            false
        };
        let ended = format!("game ended: {outcome:?} (log {})", log_path.display());
        if matches!(outcome, gt4l_core::exit::Outcome::Crashed { .. }) {
            state::log_error(&dirs, &ended);
        } else {
            state::log(&dirs, &ended);
        }
        // TASK-111: a crash or a lost graphics card offers the log bundle (the page asks, see app.js).
        let offer_logs = gpu_lost.is_some() || matches!(outcome, Outcome::Crashed { .. });
        let message = match (gpu_lost, gsdx_error, &outcome) {
            (Some(line), _, _) => {
                state::log_error(&dirs, &format!("graphics card lost: {line}"));
                let mut text = exit::gpu_lost_message_in(state::current_language());
                if switched {
                    text.push_str("\n\n");
                    text.push_str(&messages::text("game.gpu_lost_switched", state::current_language()));
                }
                text
            }
            (None, Some(_), _) if renderer_changed => messages::text("game.gsdx_switched", state::current_language()).to_string(),
            (None, Some(error), _) => {
                state::log_warn(&dirs, "GSdx failed and this PC has no graphics card for CoreGS 2.0; the renderer stays");
                error
            }
            (None, None, Outcome::NormalClose) => String::new(),
            (None, None, Outcome::Crashed { .. }) => exit::user_message_in(&outcome, state::current_language()),
        };
        lock(&shared).home_message = message.clone();
        if offer_logs {
            let _ = app.emit("logs-offer", json!({ "reason": message.clone() }));
        }
        #[cfg(desktop)]
        crate::tray::handle(&app, crate::tray::Event::GameEnded { message });
    });
    Ok(())
}

/// Reads a finished session's log, waiting for the game's last line: the runtime writes `[ee] stopped after ...` while
/// the process exits, and the launcher used to read the file in that same instant, so a normal close could be classified
/// as a crash (owner, 2026-10-26). Waits at most `SETTLE_TIMEOUT`, returning as soon as the line is there.
fn read_settled_log(path: &Path) -> String {
    const SETTLE_TIMEOUT: Duration = Duration::from_millis(1500);
    let deadline = Instant::now() + SETTLE_TIMEOUT;
    loop {
        let text = fs::read_to_string(path).unwrap_or_default();
        if exit::is_settled(&text) || Instant::now() >= deadline {
            return text;
        }
        std::thread::sleep(Duration::from_millis(25));
    }
}

/// GSdx could not run and the player chose CoreGS 2.0 in the game's message: it is stored as the renderer, when this PC's card can
/// run it. Returns whether the renderer changed.
pub fn use_coregs20_after_gsdx_failure(shared: &Shared, card_has_vulkan: bool) -> bool {
    if !card_has_vulkan {
        return false;
    }
    let mut state = lock(shared);
    match state.config.settings.set_by_user("renderer", settings::CORE_GS_20) {
        Ok(()) => {
            state.save_config();
            state::log(&state.dirs, "GSdx could not run; the player chose CoreGS 2.0, now the renderer");
            true
        }
        Err(error) => {
            state::log_error(&state.dirs, &format!("could not store CoreGS 2.0 after GSdx failed: {error}"));
            false
        }
    }
}

/// The player accepted Play's offer of CoreGS 2.0 (issues #139 and #179: GSdx is stored but not installed): it becomes the
/// stored renderer when this PC's card can run it, with the same rule as after a GSdx failure. The error is the text to show.
pub fn use_coregs20_for_play(shared: &Shared, card_has_vulkan: bool) -> Result<(), String> {
    if use_coregs20_after_gsdx_failure(shared, card_has_vulkan) {
        Ok(())
    } else {
        Err(messages::text("game.coregs20_not_selected", state::language_of(&lock(shared))).to_string())
    }
}

/// Before Play (IN-507): under CoreGS 2.0 the game program compiles what the shader cache lacks, and `shaders-progress` opens
/// the window only when there is work. Another renderer, or a problem here, goes straight to Play: the game then compiles
/// on its own as it plays. False: the player cancelled, so no game starts.
#[tauri::command(async)]
pub fn prepare_shaders(app: AppHandle, shared: State<'_, Shared>) -> Result<bool, String> {
    let shared: Shared = shared.inner().clone();
    // TASK-180 test2: on Android the prepare is ShaderPrepareService in its own process (android.rs); the same progress
    // card, attempts and log lines as on Windows. A minimized game resumes without one.
    #[cfg(target_os = "android")]
    {
        let (settings, dirs, textures, game) = {
            let state = lock(&shared);
            if crate::android::game_alive(&state.dirs) {
                return Ok(true);
            }
            state.shaders_cancelled.store(false, Ordering::SeqCst);
            (state.config.settings.clone(), state.dirs.clone(), state.texture_location().0, state.selected)
        };
        let monitors = crate::android::monitors();
        let display = display_rules::resolve_target(&monitors, settings.get("monitor"), settings.get("resolution"));
        let cancelled = { lock(&shared).shaders_cancelled.clone() };
        let mut worked = false;
        let result = crate::android::prepare_shaders(&dirs, &settings, display.as_ref(), &textures, &cancelled, &mut |done, total, attempt| {
            worked = true;
            let _ = app.emit("shaders-progress", json!({ "done": done, "total": total, "attempt": attempt }));
        });
        let state = lock(&shared);
        if state.shaders_cancelled.load(Ordering::SeqCst) {
            state::log(&state.dirs, &format!("shaders: compile of {} cancelled", game.display_name));
            return Ok(false);
        }
        log_prepare_outcome(&state.dirs, &result, worked);
        return Ok(true);
    }
    #[allow(unreachable_code)]
    let (settings, game, dirs, textures, executable, slot, language) = {
        let state = lock(&shared);
        if state.game_child.is_some() || !launch::uses_coregs20(&state.config.settings) {
            return Ok(true);
        }
        state.shaders_cancelled.store(false, Ordering::SeqCst);
        let game = state.selected;
        let game_dir = paths::game_directory(&program_directory(), state.config.dev_game_dir.as_deref());
        (state.config.settings.clone(), game, state.game_dirs(game), state.texture_location().0, launch::game_program(game, &game_dir), state.shaders_child.clone(), state::language_of(&state))
    };
    if !executable.exists() {
        return Ok(true);
    }
    let (settings, _, cards) = gt4l_platform::gpu::run_settings_with_cards(&settings, language, gt4l_platform::gpu::refresh_gpus());
    if cards.is_empty() {
        return Ok(true);
    }
    let monitors = gt4l_platform::display::monitors();
    let display = display_rules::resolve_target(&monitors, settings.get("monitor"), settings.get("resolution"));
    let mut worked = false;
    let cancelled = { lock(&shared).shaders_cancelled.clone() };
    let result = game::prepare_shaders(&dirs, &executable, &settings, display.as_ref(), &textures, &slot, &cancelled, &mut |progress| {
        worked = true;
        let _ = app.emit("shaders-progress", json!({ "done": progress.done, "total": progress.total, "attempt": progress.attempt }));
    });
    let state = lock(&shared);
    if state.shaders_cancelled.load(Ordering::SeqCst) {
        state::log(&state.dirs, &format!("shaders: compile of {} cancelled", game.display_name));
        return Ok(false);
    }
    log_prepare_outcome(&state.dirs, &result, worked);
    Ok(true)
}

/// What the log says about a prepare (TASK-170b): a finished one, a retried one, or the reason the game starts without it
/// (it then compiles as it plays).
fn log_prepare_outcome(dirs: &gt4l_core::paths::DataDirs, result: &Result<PrepareOutcome, String>, worked: bool) {
    match result {
        Ok(PrepareOutcome::Finished { attempts }) if *attempts > 1 => {
            state::log(dirs, &format!("shaders: compiled after {attempts} attempts (logs\\shaders.log)"));
        }
        Ok(PrepareOutcome::Finished { .. }) if worked => state::log(dirs, "shaders: compiled"),
        Ok(PrepareOutcome::Finished { .. }) | Ok(PrepareOutcome::Cancelled { .. }) => {}
        Ok(PrepareOutcome::GaveUp { attempts, why }) => {
            let reason = match why {
                GaveUp::NoProgress => "the last attempt got no further",
                GaveUp::LimitReached => "the attempt limit was reached",
            };
            state::log_error(dirs, &format!("shaders: the compile did not finish after {attempts} attempts, {reason}; the game compiles as it plays (logs\\shaders.log)"));
        }
        Ok(PrepareOutcome::Failed { message, .. }) | Err(message) => state::log_error(dirs, &format!("shaders: {message}")),
    }
}

/// Cancel in the "Compiling shaders" window. What was compiled stays in the cache for the next Play.
#[tauri::command]
pub fn cancel_shader_prepare(shared: State<'_, Shared>) -> Result<(), String> {
    let state = lock(shared.inner());
    state.shaders_cancelled.store(true, Ordering::SeqCst);
    // TASK-180: on Android the prepare's own loop sees the flag and ends its process.
    #[cfg(target_os = "android")]
    return Ok(());
    let mut guard = state.shaders_child.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    if let Some(process) = guard.as_mut() {
        process.kill().map_err(|error| format!("could not stop the shader compile: {error}"))?;
    }
    Ok(())
}

/// TASK-164 (issue #160): `first_start_hint()` is `null`, or `{game, confirm, pad, right}` when the next start of the selected
/// game meets its first-start questions (no save yet, [`saves::first_start_expected`]). Play then says which keys answer them,
/// from the current controls: `confirm` the keyboard keys of Cross (`W / Enter`), `pad` the controller button of Cross
/// (`A`, empty when none), `right` the keyboard key of D-pad right (the format question starts on Cancel).
#[tauri::command]
pub fn first_start_hint(shared: State<'_, Shared>) -> Value {
    first_start_hint_of(shared.inner())
}

pub fn first_start_hint_of(shared: &Shared) -> Value {
    let state = lock(shared);
    let game = state.selected;
    if !saves::first_start_expected(state.library_of(game), &state.game_dirs(game).runtime_card()) {
        return Value::Null;
    }
    let names_of = |input: &str, family: &str| -> Vec<String> {
        state
            .controls
            .sources(input)
            .unwrap_or_default()
            .iter()
            .filter(|source| controls_file::source_family(source) == family)
            .filter_map(|source| controls_file::source_base(source).split_once(':').map(|(_, name)| name.to_string()))
            .collect()
    };
    json!({
        "game": game.display_name,
        "confirm": names_of("cross", "key").join(" / "),
        "pad": names_of("cross", "xinput").first().cloned().unwrap_or_default(),
        "right": names_of("dpad_right", "key").last().cloned().unwrap_or_default(),
    })
}

/// Stops the game process. The card is harvested by the watcher thread after the exit.
#[tauri::command]
pub fn stop_game(shared: State<'_, Shared>) -> Result<(), String> {
    let child = lock(shared.inner()).game_child.clone();
    if let Some(child) = child {
        let mut guard = match child.lock() {
            Ok(guard) => guard,
            Err(poisoned) => poisoned.into_inner(),
        };
        if let Some(process) = guard.as_mut() {
            process.kill().map_err(|error| format!("could not stop the game: {error}"))?;
        }
    }
    Ok(())
}

pub(crate) fn program_directory() -> PathBuf {
    std::env::current_exe()
        .ok()
        .and_then(|path| path.parent().map(Path::to_path_buf))
        .unwrap_or_else(|| PathBuf::from("."))
}

// Controls ---------------------------------------------------------------------------------------

/// Applies `change` to a copy of the controls, writes the file, and only then keeps the copy. A refused change or a
/// failed write leaves the controls and the file as they were, and returns the reason.
fn change_controls<T>(
    state: &mut AppState,
    change: impl FnOnce(&mut controls_file::Controls) -> Result<T, String>,
) -> Result<T, String> {
    let language = state::language_of(state);
    if state.controls_locked {
        return Err(messages::text("controls.newer_file", language).to_string());
    }
    let mut next = state.controls.clone();
    let result = change(&mut next)?;
    controls_file::save(&state.controls_path(), &next)
        .map_err(|error| messages::render("controls.could_not_save", language, &[("error", &error.to_string())]))?;
    state.controls = next;
    Ok(result)
}

fn controls_view(state: &AppState) -> Value {
    model::controls_view(&state.config.settings, &state.controls, state.controls_locked)
}

/// `get_controls()`: `{controls: [{id, name, binding, sources, xinput, key, is_default}], triggers: [row], locked}`.
/// `triggers` holds the analog rows (trigger deadzone and curve, stick deadzones) in the settings row shape.
#[tauri::command]
pub fn get_controls(shared: State<'_, Shared>) -> Result<Value, String> {
    get_controls_of(shared.inner())
}

pub fn get_controls_of(shared: &Shared) -> Result<Value, String> {
    Ok(controls_view(&lock(shared)))
}

/// `capture_binding({control})`: waits for the next newly pressed control of the active pad (SDL3, the pad the game
/// reads) and resolves with `{control, source}` (`xinput:A`, `xinput:LT`, in the controls-file names). It binds
/// nothing: the page checks for a conflict and then calls `assign_binding`. A control held when the wait starts does
/// not count. `cancel_capture` ends the wait with an error.
#[tauri::command]
pub async fn capture_binding(
    control: String,
    shared: State<'_, Shared>,
    pads: State<'_, pad_service::Handle>,
) -> Result<Value, String> {
    if !model::bindable(&control) {
        return Err(format!("'{control}' cannot be rebound"));
    }
    let shared: Shared = shared.inner().clone();
    lock(&shared).capturing = Some(control.clone());
    let answer = pads.inner().start_capture();
    let outcome = tauri::async_runtime::spawn_blocking(move || answer.recv()).await;
    {
        let mut state = lock(&shared);
        if state.capturing.as_deref() == Some(control.as_str()) {
            state.capturing = None;
        }
    }
    match outcome {
        Ok(Ok(Ok(source))) => Ok(json!({ "control": control, "source": source })),
        Ok(Ok(Err(message))) => Err(message),
        Ok(Err(_)) => Err(pad_service::CAPTURE_CANCELLED.to_string()),
        Err(error) => Err(error.to_string()),
    }
}

/// `cancel_capture()`: ends a running capture without binding anything.
#[tauri::command]
pub fn cancel_capture(shared: State<'_, Shared>, pads: State<'_, pad_service::Handle>) -> Result<(), String> {
    pads.inner().cancel_capture();
    let mut state = lock(shared.inner());
    state.capturing = None;
    state.controls_status.clear();
    Ok(())
}

/// `pad_status()`: the last `pad-status` payload (`available`, `connected`, `name`, `kind`, `player_index`, `count`,
/// `error`), for a page that opens after the event was sent.
#[tauri::command]
pub fn pad_status(pads: State<'_, pad_service::Handle>) -> Result<Value, String> {
    Ok(pads.inner().status())
}

/// `assign_binding({control, source, swap})`: binds `source` (`xinput:RB`, `key:Q`) to the button `control`, in place
/// of its source of the same family. When another button has the source, `swap: true` gives that button the previous
/// source; `swap: false` refuses. Writes the file at once and returns the `get_controls` view plus `swapped_with`
/// (the other button, or null) and `unchanged`.
#[tauri::command]
pub fn assign_binding(control: String, source: String, swap: bool, shared: State<'_, Shared>) -> Result<Value, String> {
    assign_binding_of(control, source, swap, shared.inner())
}

pub fn assign_binding_of(control: String, source: String, swap: bool, shared: &Shared) -> Result<Value, String> {
    let mut state = lock(shared);
    let assignment = change_controls(&mut state, |controls| {
        controls.assign(&control, &source, swap).map_err(|error| error.to_string())
    })?;
    state.controls_status = format!("{} is now bound to {}.", model::ps2_name(&control), model::describe_source(&source));
    let mut view = controls_view(&state);
    view["swapped_with"] = json!(assignment.swapped_with);
    view["unchanged"] = json!(assignment.unchanged);
    Ok(view)
}

/// `reset_binding({control})`: restores the built-in sources of one input.
#[tauri::command]
pub fn reset_binding(control: String, shared: State<'_, Shared>) -> Result<(), String> {
    reset_binding_of(control, shared.inner())
}

pub fn reset_binding_of(control: String, shared: &Shared) -> Result<(), String> {
    let mut state = lock(shared);
    let sources = default_bindings().get(&control).cloned().ok_or_else(|| format!("unknown input '{control}'"))?;
    change_controls(&mut state, |controls| controls.set_sources(&control, sources).map_err(|error| error.to_string()))?;
    state.controls_status = format!("{} is back to its built-in binding.", model::ps2_name(&control));
    Ok(())
}

/// `reset_all_bindings()`: every button, trigger and stick setting back to the built-in mapping. Returns the
/// `get_controls` view.
#[tauri::command]
pub fn reset_all_bindings(shared: State<'_, Shared>) -> Result<Value, String> {
    reset_all_bindings_of(shared.inner())
}

pub fn reset_all_bindings_of(shared: &Shared) -> Result<Value, String> {
    let mut state = lock(shared);
    change_controls(&mut state, |controls| {
        controls.reset_all();
        Ok(())
    })?;
    let language = state::language_of(&state);
    state.controls_status = messages::text("controls.defaults_restored", language).to_string();
    Ok(controls_view(&state))
}

// The virtual controller (TASK-185): its settings row and the layout file of the on-screen pad ---------------------

/// The view of the virtual pad page: the Auto/Always/Off mode, the player's layout or the built-in one, the built-in
/// layout itself (the editor's Reset), and why the player's file was not used (`notes`). `locked` means the file comes
/// from a newer launcher and must be kept as it is.
fn virtual_pad_view(state: &AppState) -> Value {
    let path = gt4l_core::virtual_pad::layout_path(&state.user_dirs);
    let loaded = gt4l_core::virtual_pad::load(&path);
    json!({
        "mode": state.config.settings.get("virtual_pad"),
        "layout": loaded.layout,
        "default_layout": gt4l_core::virtual_pad::default_layout(),
        "source": loaded.source,
        "locked": loaded.locked,
        "has_file": path.exists(),
        "notes": loaded.notes,
        "path": path.display().to_string(),
    })
}

/// `get_virtual_pad()`: the Controls page's "Virtual controller" section and the layout editor.
#[tauri::command]
pub fn get_virtual_pad(shared: State<'_, Shared>) -> Result<Value, String> {
    get_virtual_pad_of(shared.inner())
}

pub fn get_virtual_pad_of(shared: &Shared) -> Result<Value, String> {
    Ok(virtual_pad_view(&lock(shared)))
}

/// `save_virtual_pad({layout})`: the editor's Save. The layout is checked with the contract's rules before anything is
/// written; the file is replaced atomically. Returns the new view. A refused layout leaves the file as it was.
#[tauri::command]
pub fn save_virtual_pad(layout: Value, shared: State<'_, Shared>) -> Result<Value, String> {
    save_virtual_pad_of(layout, shared.inner())
}

pub fn save_virtual_pad_of(layout: Value, shared: &Shared) -> Result<Value, String> {
    let parsed: gt4l_core::virtual_pad::Layout = serde_json::from_value(layout)
        .map_err(|error| format!("the layout is not valid: {error}"))?;
    let state = lock(shared);
    let path = gt4l_core::virtual_pad::layout_path(&state.user_dirs);
    gt4l_core::virtual_pad::save(&path, &parsed)?;
    state::log(&state.dirs, &format!("virtual pad: layout saved ({} elements, opacity {:.2})", parsed.elements.len(), parsed.opacity));
    Ok(virtual_pad_view(&state))
}

/// `reset_virtual_pad()`: deletes the player's layout, so the game uses the built-in one again. Returns the new view.
#[tauri::command]
pub fn reset_virtual_pad(shared: State<'_, Shared>) -> Result<Value, String> {
    reset_virtual_pad_of(shared.inner())
}

pub fn reset_virtual_pad_of(shared: &Shared) -> Result<Value, String> {
    let state = lock(shared);
    let path = gt4l_core::virtual_pad::layout_path(&state.user_dirs);
    let removed = gt4l_core::virtual_pad::reset(&path)?;
    state::log(
        &state.dirs,
        if removed {
            "virtual pad: layout reset, the player's file was deleted"
        } else {
            "virtual pad: layout reset, there was no player's file"
        },
    );
    Ok(virtual_pad_view(&state))
}

// Saves and replays --------------------------------------------------------------------------------

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct LibraryView {
    pub items: Vec<Item>,
    pub status: String,
    /// The folder that holds the page's files (`Documents\Gran Turismo 4\Saves` or `...\Replays`), shown with an
    /// "Open folder" button so a player can find and back up the files.
    pub folder: String,
    /// The game these saves or replays belong to (the Saves page names it in its title): `gt4` or `spec2`.
    pub game: String,
    pub game_name: String,
}

fn saves_view(state: &AppState) -> Result<LibraryView, String> {
    Ok(LibraryView {
        items: saves_items(state)?,
        status: state.saves_status.clone(),
        folder: state.selected_user().saves().display().to_string(),
        game: state.selected.id.to_string(),
        game_name: state.selected.display_name.to_string(),
    })
}

/// The saves of every game in one list (TASK-101 item 2): each game's saves in slot order, each tagged with its game id.
fn saves_items(state: &AppState) -> Result<Vec<Item>, String> {
    let mut items = Vec::new();
    for &game in games::GAMES {
        for mut item in saves::saves(state.library_of(game))? {
            item.game = game.id.to_string();
            items.push(item);
        }
    }
    Ok(items)
}

/// The game and the library name of a Saves page item id (`gt4:Save 02`). An id without a known game prefix (a replay,
/// or an id from an older page) belongs to the game the page shows.
pub fn item_target(state: &AppState, id: &str) -> (&'static Game, String) {
    match id.split_once(':').and_then(|(game, name)| games::by_id(game).map(|game| (game, name))) {
        Some((game, name)) => (game, name.to_string()),
        None => (state.selected, id.to_string()),
    }
}

fn replays_view(state: &AppState) -> Result<LibraryView, String> {
    Ok(LibraryView {
        items: saves::replays(state.library())?,
        status: state.replays_status.clone(),
        folder: state.selected_user().replays().display().to_string(),
        game: state.selected.id.to_string(),
        game_name: state.selected.display_name.to_string(),
    })
}

#[tauri::command]
pub fn saves_list(shared: State<'_, Shared>) -> Result<LibraryView, String> {
    saves_list_of(shared.inner())
}

pub fn saves_list_of(shared: &Shared) -> Result<LibraryView, String> {
    saves_view(&lock(shared))
}

#[tauri::command]
pub fn replays_list(shared: State<'_, Shared>) -> Result<LibraryView, String> {
    replays_list_of(shared.inner())
}

pub fn replays_list_of(shared: &Shared) -> Result<LibraryView, String> {
    replays_view(&lock(shared))
}

/// Picks a save file or card image and imports it. Each item goes to the library of the game its name belongs to, so the
/// page's game (`game`, optional) only decides what the status says when the file is of another game (TASK-101 item 1).
/// Imports never overwrite.
#[tauri::command]
pub async fn saves_import(app: AppHandle, game: Option<String>, shared: State<'_, Shared>) -> Result<Option<LibraryView>, String> {
    import_with_picker(app, shared.inner().clone(), Kind::Saves, game).await
}

#[tauri::command]
pub async fn replays_import(app: AppHandle, game: Option<String>, shared: State<'_, Shared>) -> Result<Option<LibraryView>, String> {
    import_with_picker(app, shared.inner().clone(), Kind::Replays, game).await
}

/// The game the page shows for an import: the page's answer when it names a game of the launcher, else the selected one.
pub fn page_game(game: Option<&str>, selected: &'static Game) -> &'static Game {
    game.and_then(games::by_id).unwrap_or(selected)
}

#[derive(Clone, Copy)]
pub enum Kind {
    Saves,
    Replays,
}

async fn import_with_picker(app: AppHandle, shared: Shared, kind: Kind, game: Option<String>) -> Result<Option<LibraryView>, String> {
    let language = state::language_of(&lock(&shared));
    tauri::async_runtime::spawn_blocking(move || {
        // TASK-180: on Android the picked document is copied into the app's cache first; the import reads that file.
        #[cfg(target_os = "android")]
        let path = {
            let _ = language;
            let staging = crate::android::cache_file(&app, "import")?;
            let Some(picked) = crate::android::pick_document(&["*/*"], false, Some(&staging))? else {
                return Ok(None);
            };
            let named = crate::android::cache_file(&app, picked.name.as_deref().unwrap_or("import.psu"))?;
            fs::rename(&staging, &named).map_err(|error| format!("{}: {error}", named.display()))?;
            named
        };
        #[cfg(not(target_os = "android"))]
        let path = {
            let picked = app
                .dialog()
                .file()
                .set_title(messages::text("saves.import_title", language))
                .add_filter(messages::text("saves.import_filter", language), &["psu", "ps2", "mcd", "bin", "card"])
                .blocking_pick_file();
            let Some(picked) = picked else {
                return Ok(None);
            };
            picked.into_path().map_err(|error| error.to_string())?
        };
        let page = page_game(game.as_deref(), lock(&shared).selected);
        import_into_game(&shared, &path, kind, page).map(Some)
    })
    .await
    .map_err(|error| error.to_string())?
}

/// The import into the libraries of every game, for a picked path (tests and the flow audit). The answer is the page's
/// own list: the game the page shows.
#[cfg_attr(not(test), allow(dead_code))]
pub fn import_into(shared: &Shared, path: &Path, kind: Kind) -> Result<LibraryView, String> {
    let selected = lock(shared).selected;
    import_into_game(shared, path, kind, selected)
}

/// The import behind `saves_import` and `replays_import` for a picked path. Each item goes to its own game's library.
/// The status names the game of an item when that game is not the page's one.
pub fn import_into_game(shared: &Shared, path: &Path, kind: Kind, page: &'static Game) -> Result<LibraryView, String> {
    let mut state = lock(shared);
    let language = state::language_of(&state);
    // A panic in the import must never close the launcher (issue #86): it becomes a message, and the log names the file.
    let file_name = path.file_name().map(|name| name.to_string_lossy().into_owned()).unwrap_or_default();
    let size = fs::metadata(path).map(|metadata| metadata.len()).unwrap_or(0);
    let libraries: Vec<(&'static Game, &Library)> = games::GAMES.iter().map(|game| (*game, state.library_of(game))).collect();
    let attempt = panic::catch_unwind(AssertUnwindSafe(|| saves::import_path(&libraries, path, language)));
    let summary = match attempt {
        Ok(Ok(outcome)) => {
            let summary = saves::import_summary(&outcome, page, language);
            state::log(&state.dirs, &format!("import of {file_name} ({size} bytes): {summary}"));
            summary
        }
        Ok(Err(message)) => {
            state::log_error(&state.dirs, &format!("import of {file_name} ({size} bytes) refused: {message}"));
            message
        }
        Err(_) => {
            state::log_error(&state.dirs, &format!("import of {file_name} ({size} bytes) stopped by an internal error"));
            messages::text("saves.import_crashed", language).to_string()
        }
    };
    match kind {
        Kind::Saves => {
            state.saves_status = summary;
            saves_view(&state)
        }
        Kind::Replays => {
            state.replays_status = summary;
            replays_view(&state)
        }
    }
}

/// Makes a save the one the next start uses. The save that was active moves to a new slot.
#[tauri::command]
pub fn saves_activate(id: String, shared: State<'_, Shared>) -> Result<LibraryView, String> {
    saves_activate_of(id, shared.inner())
}

pub fn saves_activate_of(id: String, shared: &Shared) -> Result<LibraryView, String> {
    let mut state = lock(shared);
    let language = state::language_of(&state);
    let (game, name) = item_target(&state, &id);
    let library = state.library_of(game);
    let item = saves::saves(library)?
        .into_iter()
        .find(|item| item.name == name)
        .ok_or_else(|| messages::text("saves.not_in_library", language).to_string())?;
    if item.active {
        return saves_view(&state);
    }
    saves::activate(library, &name)?;
    state.saves_status = format!("{} will be used the next time you play.", name);
    saves_view(&state)
}

/// `saves_new({game})`: starts a new save for `game` (the page's game when it names none). The active save of that game is
/// kept as a new `Save NN`; the next Play of that game starts a new profile. Refused while a game runs. The page asks first,
/// and asks which game when two are installed.
#[tauri::command]
pub fn saves_new(game: Option<String>, shared: State<'_, Shared>) -> Result<LibraryView, String> {
    saves_new_of(game, shared.inner())
}

pub fn saves_new_of(game: Option<String>, shared: &Shared) -> Result<LibraryView, String> {
    let mut state = lock(shared);
    let language = state::language_of(&state);
    if state.game_child.is_some() {
        return Err(messages::text("saves.game_running", language).to_string());
    }
    let game = page_game(game.as_deref(), state.selected);
    let card = state.game_dirs(game).runtime_card();
    let parked = saves::new_save(state.library_of(game), &card)?;
    let text = match parked {
        Some(slot) => messages::render("saves.new_kept", language, &[("slot", &slot)]),
        None => messages::text("saves.new_started", language).to_string(),
    };
    state.saves_status = format!("{}: {text}", game.display_name);
    saves_view(&state)
}

/// `saves_export({id})`: writes a save (the active one or a slot) as a `.psu` file the player picks. Cancelling changes
/// nothing.
#[tauri::command]
pub async fn saves_export(app: AppHandle, id: String, shared: State<'_, Shared>) -> Result<Option<LibraryView>, String> {
    let shared = shared.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        let (bytes, suggested, language) = {
            let state = lock(&shared);
            let language = state::language_of(&state);
            let (game, name) = item_target(&state, &id);
            let (bytes, suggested) = saves::export(state.library_of(game), &name)?;
            (bytes, suggested, language)
        };
        // TASK-180: on Android the save is written to the app's cache, then the player picks where the copy goes.
        #[cfg(target_os = "android")]
        let path = {
            let path = crate::android::cache_file(&app, &suggested)?;
            fs::write(&path, &bytes).map_err(|error| format!("{}: {error}", path.display()))?;
            if !crate::android::save_document(&path, &suggested, "application/octet-stream")? {
                return Ok(None);
            }
            PathBuf::from(&suggested)
        };
        #[cfg(not(target_os = "android"))]
        let path = {
            let picked = app
                .dialog()
                .file()
                .set_title(messages::text("saves.export_title", language))
                .set_file_name(&suggested)
                .add_filter(messages::text("saves.export_filter", language), &["psu"])
                .blocking_save_file();
            let Some(picked) = picked else {
                return Ok(None);
            };
            let path = picked.into_path().map_err(|error| error.to_string())?;
            fs::write(&path, &bytes).map_err(|error| format!("{}: {error}", path.display()))?;
            path
        };
        let mut state = lock(&shared);
        state.saves_status = messages::render("saves.exported", language, &[("path", &path.display().to_string())]);
        saves_view(&state).map(Some)
    })
    .await
    .map_err(|error| error.to_string())?
}

/// `saves_rename({id, name})`: gives a save a display name (at most 40 characters). The folder name stays.
#[tauri::command]
pub fn saves_rename(id: String, name: String, shared: State<'_, Shared>) -> Result<LibraryView, String> {
    saves_rename_of(id, name, shared.inner())
}

pub fn saves_rename_of(id: String, name: String, shared: &Shared) -> Result<LibraryView, String> {
    let mut state = lock(shared);
    let language = state::language_of(&state);
    let (game, library_name) = item_target(&state, &id);
    let stored = saves::rename(state.library_of(game), &library_name, &name, language)?;
    state.saves_status = messages::render("saves.renamed", language, &[("name", &stored)]);
    saves_view(&state)
}

/// Sends a save or replay to the Recycle Bin.
#[tauri::command]
pub fn saves_delete(id: String, shared: State<'_, Shared>) -> Result<LibraryView, String> {
    saves_delete_of(id, shared.inner())
}

pub fn saves_delete_of(id: String, shared: &Shared) -> Result<LibraryView, String> {
    delete_item(&id, shared, Kind::Saves)
}

#[tauri::command]
pub fn replays_delete(id: String, shared: State<'_, Shared>) -> Result<LibraryView, String> {
    replays_delete_of(id, shared.inner())
}

pub fn replays_delete_of(id: String, shared: &Shared) -> Result<LibraryView, String> {
    delete_item(&id, shared, Kind::Replays)
}

/// Deletes one save (of the game its id names) or one replay (of the game the page shows).
fn delete_item(id: &str, shared: &Shared, kind: Kind) -> Result<LibraryView, String> {
    let mut state = lock(shared);
    let language = state::language_of(&state);
    let (game, name) = match kind {
        Kind::Saves => item_target(&state, id),
        Kind::Replays => (state.selected, id.to_string()),
    };
    let library = state.library_of(game);
    let items = match kind {
        Kind::Saves => saves::saves(library)?,
        Kind::Replays => saves::replays(library)?,
    };
    let item = items
        .into_iter()
        .find(|item| item.name == name)
        .ok_or_else(|| messages::text("saves.item_not_in_library", language).to_string())?;
    if item.active && state.game_child.is_some() {
        return Err(messages::text("saves.delete_game_running", language).to_string());
    }
    let card = state.game_dirs(game).runtime_card();
    let library = state.library_of(game);
    let result = saves::delete(library, &item, &card);
    let status = match result {
        Ok(()) => format!("{} moved to the Recycle Bin.", item.label),
        Err(message) => message,
    };
    match kind {
        Kind::Saves => {
            state.saves_status = status;
            saves_view(&state)
        }
        Kind::Replays => {
            state.replays_status = status;
            replays_view(&state)
        }
    }
}

// Games -----------------------------------------------------------------------------------------

/// `games_list()`: `{selected, any_installed, games: [{id, name, title, variant, installed, damaged, reason, selected,
/// disc_name, folder, size_bytes}]}` (Settings > Games and the home selector). `title` is the name shown in Settings
/// (Spec II names its version); `variant` is what tells this copy apart (`USA v2.00`, `PT-BR`), empty when none.
#[tauri::command]
pub fn games_list(shared: State<'_, Shared>) -> Result<Value, String> {
    games_list_of(shared.inner())
}

pub fn games_list_of(shared: &Shared) -> Result<Value, String> {
    let state = lock(shared);
    Ok(json!({
        "selected": state.selected.id,
        "any_installed": !state.installed_games().is_empty(),
        "games": games::GAMES.iter().map(|game| game_row(&state, game)).collect::<Vec<_>>(),
    }))
}

/// `select_game({id})`: the game the home screen, the Saves page and Play use. It is only the page's choice; the stored
/// choice (the game played last) changes when a game starts. Not while the game runs.
#[tauri::command]
pub fn select_game(id: String, shared: State<'_, Shared>) -> Result<Value, String> {
    select_game_of(id, shared.inner())
}

pub fn select_game_of(id: String, shared: &Shared) -> Result<Value, String> {
    let game = games::by_id(&id).ok_or_else(|| format!("{id} is not a game of this launcher."))?;
    {
        let mut state = lock(shared);
        let language = state::language_of(&state);
        if state.game_child.is_some() {
            return Err(messages::text("game.stop_before_choosing", language).to_string());
        }
        if state.selected != game {
            state.selected = game;
            state.ra_revision = state.ra_revision.wrapping_add(1);
            state.saves_status.clear();
            state.replays_status.clear();
            state.home_message.clear();
            // A finished setup of another game is not this game's result.
            if state.setup_game != Some(game) && !state.setup_busy {
                state.setup_outcome = SetupOutcome::None;
            }
        }
    }
    games_list_of(shared)
}

/// What "Remove" deletes of a game: the copy of the disc image and the prepared files, in the data folder's write places.
/// Never the save library, the replays, the runtime card or any shared folder.
pub const REMOVED_FOLDERS: &[&str] = &["disc", "prepared", "prepared.tmp"];

/// `games_remove({id})`: deletes the prepared files of a game, and the launcher's own copy of the disc image of an older install
/// (the page asks first). An image the player keeps elsewhere is never touched, and its saves and replays stay. Refused while the game runs or setup works. Returns the new `games_list`.
#[tauri::command]
pub fn games_remove(id: String, shared: State<'_, Shared>) -> Result<Value, String> {
    games_remove_of(id, shared.inner())
}

pub fn games_remove_of(id: String, shared: &Shared) -> Result<Value, String> {
    let game = games::by_id(&id).ok_or_else(|| format!("{id} is not a game of this launcher."))?;
    {
        let state = lock(shared);
        let language = state::language_of(&state);
        if state.game_child.is_some() {
            return Err(messages::text("game.stop_before_removing", language).to_string());
        }
        if state.setup_busy || state.migration.running {
            return Err(messages::text("game.wait_for_setup", language).to_string());
        }
        let dirs = state.game_dirs(game);
        let mut removed = Vec::new();
        for place in dirs.write_places() {
            // The player may have linked an image that sits in this folder: that file is theirs.
            let linked = disc_setup::read_receipt(&place.join("prepared"))
                .and_then(|receipt| receipt.image)
                .and_then(|image| disc_setup::linked_image(&image, &state.install_dir));
            for name in REMOVED_FOLDERS {
                let folder = place.join(name);
                if linked.as_ref().is_some_and(|image| image.starts_with(&folder)) {
                    continue;
                }
                if folder.exists() {
                    fs::remove_dir_all(&folder).map_err(|error| format!("{} was not removed: {error}", folder.display()))?;
                    removed.push(folder.display().to_string());
                }
            }
            // Leftovers of an interrupted preparation (`prepared.old-<time>`).
            if let Ok(entries) = fs::read_dir(&place) {
                for entry in entries.filter_map(|entry| entry.ok()) {
                    if entry.file_name().to_string_lossy().starts_with("prepared.old-") {
                        let _ = fs::remove_dir_all(entry.path());
                    }
                }
            }
        }
        state::log(&state.dirs, &format!("game removed ({}): {}; the saves and replays are kept", game.id, removed.join(", ")));
    }
    {
        let mut state = lock(shared);
        if state.setup_game == Some(game) {
            state.setup_outcome = SetupOutcome::None;
        }
    }
    games_list_of(shared)
}

// About -----------------------------------------------------------------------------------------

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AboutInfo {
    pub text: String,
    pub licenses: String,
    pub disc_path: String,
    /// The launcher version (GT4Recomp), the same as `gt4recomp`. Kept for the existing pages.
    pub version: String,
    /// GT4Recomp (launcher, game and installer) version.
    pub gt4recomp: String,
    /// The gt4-gs version in use, or `not installed`.
    pub gt4gs: String,
    /// Where the game files live: the copy of the disc image (`<data>\disc`). The picked file is not needed.
    pub game_files: String,
    /// The large data folder (`<install>\data`: disc copy, prepared files, renderer, logs).
    pub data_folder: String,
    /// The user data folder with the settings, saves and replays (`%APPDATA%\GT4Recomp`).
    pub saves_folder: String,
    /// The game the page shows (game files and the Saves folder below belong to it).
    pub game_name: String,
    /// Credits of the people behind the games GT4Recomp runs (the Spec II authors; see `model::credits_text`).
    pub credits: String,
    /// The build kind: `Installer` or `Portable` (TASK-098). Settings > About shows it.
    pub build: String,
}

// TASK-099: `RELEASE_NOTES`, every short `docs/release-notes/launcher/<version>.md` up to this launcher version, newest
// first, embedded by build.rs (a version without its own short notes does not build). The home page renders the
// Markdown itself and shows no raw HTML; its arrows step through the older notes.
include!(concat!(env!("OUT_DIR"), "/release_notes.rs"));

/// The links the home screen may open (owner, 2026-10-23), by name: the page never passes an address.
/// `kofi`: "Support the project" (top right); `youtube`: "Moesuito" in the version line (bottom right).
pub const PROJECT_LINKS: &[(&str, &str)] =
    &[
        ("kofi", "https://ko-fi.com/moesuito"),
        ("youtube", "https://www.youtube.com/@Moesuito"),
        ("report_problem", "https://github.com/moesuito/gt4-staticrecomp/issues/new"),
    ];

/// The address of a named project link, or None for any other name.
pub fn project_link(name: &str) -> Option<&'static str> {
    PROJECT_LINKS.iter().find(|(key, _)| *key == name).map(|(_, url)| *url)
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ReleaseNotes {
    /// This launcher's version (the first entry of `releases`).
    pub version: String,
    /// Every embedded release, newest first.
    pub releases: Vec<ReleaseNote>,
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ReleaseNote {
    pub version: String,
    /// The en-US text: the fallback when the page's language has no note for this version.
    pub markdown: String,
    /// The other languages of this release: language tag (`pt-BR`) -> Markdown.
    pub translations: std::collections::BTreeMap<String, String>,
}

/// The embedded notes grouped by version, newest first (TASK-102: en-US plus every translated folder).
#[tauri::command]
pub fn release_notes() -> ReleaseNotes {
    let mut releases: Vec<ReleaseNote> = Vec::new();
    for (tag, version, markdown) in RELEASE_NOTES {
        let index = match releases.iter().position(|note| note.version == *version) {
            Some(index) => index,
            None => {
                releases.push(ReleaseNote {
                    version: version.to_string(),
                    markdown: String::new(),
                    translations: std::collections::BTreeMap::new(),
                });
                releases.len() - 1
            }
        };
        if *tag == "en-US" {
            releases[index].markdown = markdown.to_string();
        } else {
            releases[index].translations.insert(tag.to_string(), markdown.to_string());
        }
    }
    // A translation whose version has no en-US note is not shown (the build keeps the en-US note of this version).
    releases.retain(|note| !note.markdown.is_empty());
    ReleaseNotes { version: env!("CARGO_PKG_VERSION").to_string(), releases }
}

/// Opens one of the `PROJECT_LINKS` in the default browser. Any other name is refused.
#[tauri::command]
pub fn open_link(name: String) -> Result<(), String> {
    let url = project_link(&name).ok_or_else(|| format!("{name} is not a link of this launcher"))?;
    #[cfg(target_os = "android")]
    return crate::android::open_url(url);
    #[cfg(windows)]
    {
        std::process::Command::new("explorer.exe")
            .arg(url)
            .spawn()
            .map(|_| ())
            .map_err(|error| format!("could not open {url}: {error}"))
    }
    #[cfg(not(any(windows, target_os = "android")))]
    {
        Err(format!("open {url} in a browser"))
    }
}

#[cfg(test)]
mod project_link_tests {
    use super::*;

    #[test]
    fn only_the_named_project_links_open() {
        assert_eq!(project_link("kofi"), Some("https://ko-fi.com/moesuito"));
        assert_eq!(project_link("youtube"), Some("https://www.youtube.com/@Moesuito"));
        assert_eq!(project_link("https://example.com"), None);
        assert_eq!(project_link(""), None);
    }
}

#[tauri::command]
pub fn about_info(shared: State<'_, Shared>) -> Result<AboutInfo, String> {
    about_info_of(shared.inner())
}

pub fn about_info_of(shared: &Shared) -> Result<AboutInfo, String> {
    let state = lock(shared);
    let renderer = gs_release::installed(&state.dirs.renderer_root())
        .map(|installed| installed.version.text())
        .unwrap_or_else(|| "not installed".to_string());
    Ok(AboutInfo {
        text: gt4l_core::messages::render("about.text", state::current_language(), &[("version", LAUNCHER_VERSION)]),
        licenses: model::licenses_text().to_string(),
        disc_path: state.disc_path().map(|path| path.display().to_string()).unwrap_or_default(),
        version: LAUNCHER_VERSION.to_string(),
        gt4recomp: LAUNCHER_VERSION.to_string(),
        gt4gs: renderer,
        game_files: game_files_folder(&state, state.selected).display().to_string(),
        data_folder: state.dirs.root().display().to_string(),
        saves_folder: state.user_dirs.root().display().to_string(),
        game_name: state.selected.display_name.to_string(),
        credits: model::credits_text().to_string(),
        // TASK-180: the Android app is neither the installer nor the portable build.
        build: if cfg!(target_os = "android") { "Android app".to_string() } else { gt4l_core::build_kind::BUILD.label().to_string() },
    })
}

/// Opens the log folder in Explorer. The path is fixed by the launcher; nothing from the page runs.
#[tauri::command]
pub fn open_log_folder(shared: State<'_, Shared>) -> Result<(), String> {
    let logs = lock(shared.inner()).dirs.logs();
    open_folder(&logs)
}

/// Selects `logs\latest.log` in Explorer: the file a player attaches to an issue (TASK-112). The path is fixed by the
/// launcher. Before the file exists the log folder opens.
#[tauri::command]
pub fn open_latest_log(shared: State<'_, Shared>) -> Result<(), String> {
    let (latest, logs) = {
        let state = lock(shared.inner());
        (crate::session_log::latest_path(&state.dirs), state.dirs.logs())
    };
    if !latest.is_file() {
        return open_folder(&logs);
    }
    #[cfg(windows)]
    {
        std::process::Command::new("explorer.exe")
            .arg(format!("/select,{}", latest.display()))
            .spawn()
            .map(|_| ())
            .map_err(|error| format!("could not open {}: {error}", latest.display()))
    }
    #[cfg(not(windows))]
    {
        open_folder(&logs)
    }
}

/// `export_logs()`: writes the log bundle (TASK-111) into a folder the player picks (Desktop by default). Returns the zip's
/// path, or `None` when the player cancels. Nothing is written before the folder is picked.
#[tauri::command]
pub async fn export_logs(app: AppHandle, shared: State<'_, Shared>, pads: State<'_, pad_service::Handle>) -> Result<Option<String>, String> {
    let shared = shared.inner().clone();
    // The pads the launcher's gamepad service knows now, by name (no serial numbers).
    let controllers = crate::log_bundle::controller_names(&pads.inner().status());
    tauri::async_runtime::spawn_blocking(move || {
        let (language, start) = {
            let state = lock(&shared);
            let start = gt4l_platform::known_folder::desktop()
                .or_else(gt4l_platform::known_folder::documents)
                .unwrap_or_else(|| state.dirs.root().to_path_buf());
            (state::language_of(&state), start)
        };
        // TASK-180: Android has no folder picker for this; the bundle is written to the app's cache and the player then
        // picks where the copy goes (below).
        #[cfg(target_os = "android")]
        let folder = {
            let _ = &start;
            crate::android::cache_file(&app, "logs")?.with_file_name("")
        };
        #[cfg(not(target_os = "android"))]
        let folder = {
            let picked = app
                .dialog()
                .file()
                .set_title(messages::text("logs.export_title", language))
                .set_directory(&start)
                .blocking_pick_folder();
            let Some(picked) = picked else {
                return Ok(None);
            };
            picked.into_path().map_err(|error| error.to_string())?
        };
        let user = std::env::var("USERNAME").unwrap_or_default();
        // TASK-180: the game's last ends (Android's records, a crash's tombstone) go into the bundle too.
        #[cfg(target_os = "android")]
        {
            let dirs = lock(&shared).dirs.clone();
            crate::android::record_game_exits(&dirs);
        }
        let state = lock(&shared);
        match crate::log_bundle::export(&state, &folder, &user, &controllers) {
            Ok(path) => {
                state::log(&state.dirs, &format!("logs exported to {}", path.display()));
                // TASK-180: the player picks where the bundle goes (Storage Access Framework); the launcher state is not
                // held while the picker is open.
                #[cfg(target_os = "android")]
                {
                    let dirs = state.dirs.clone();
                    drop(state);
                    let name = path.file_name().map(|name| name.to_string_lossy().into_owned()).unwrap_or_else(|| "logs.zip".into());
                    let saved = crate::android::save_document(&path, &name, "application/zip");
                    let _ = fs::remove_file(&path);
                    match &saved {
                        Ok(true) => state::log(&dirs, &format!("log bundle {name} saved by the player")),
                        Ok(false) => state::log(&dirs, "log bundle not saved (cancelled)"),
                        Err(message) => state::log_error(&dirs, &format!("log bundle not saved: {message}")),
                    }
                    if !saved? {
                        return Ok(None);
                    }
                    Ok(Some(name))
                }
                #[cfg(not(target_os = "android"))]
                Ok(Some(path.display().to_string()))
            }
            Err(error) => {
                state::log_error(&state.dirs, &format!("logs not exported: {error}"));
                Err(messages::render("logs.export_failed", language, &[("error", &error)]))
            }
        }
    })
    .await
    .map_err(|error| error.to_string())?
}

/// `open_export_folder({path})`: selects a log bundle in Explorer. Only a `GT4Recomp-logs-*.zip` file that exists is
/// accepted, whatever the page sends.
#[tauri::command]
pub fn open_export_folder(path: String) -> Result<(), String> {
    let file = std::path::Path::new(&path);
    let name = file.file_name().map(|name| name.to_string_lossy().into_owned()).unwrap_or_default();
    if !(name.starts_with("GT4Recomp-logs-") && name.ends_with(".zip") && file.is_file()) {
        return Err(format!("{name} is not a log bundle of this launcher"));
    }
    #[cfg(windows)]
    {
        std::process::Command::new("explorer.exe")
            .arg(format!("/select,{}", file.display()))
            .spawn()
            .map(|_| ())
            .map_err(|error| format!("could not open {}: {error}", file.display()))
    }
    #[cfg(not(windows))]
    {
        let _ = file;
        Err("open the folder of the log bundle".to_string())
    }
}

/// Opens the folder of the game's disc image in Explorer.
#[tauri::command]
pub fn open_game_files_folder(shared: State<'_, Shared>) -> Result<(), String> {
    let folder = {
        let state = lock(shared.inner());
        game_files_folder(&state, state.selected)
    };
    open_folder(&folder)
}

/// Opens the user data folder (`%APPDATA%\GT4Recomp`, with Saves and Replays) in Explorer.
#[tauri::command]
pub fn open_player_folder(shared: State<'_, Shared>) -> Result<(), String> {
    let folder = lock(shared.inner()).user_dirs.root().to_path_buf();
    open_folder(&folder)
}

/// Opens the Saves folder of the user data folder (`%APPDATA%\GT4Recomp\Saves`) in Explorer.
#[tauri::command]
pub fn open_saves_folder(shared: State<'_, Shared>) -> Result<(), String> {
    let folder = {
        let state = lock(shared.inner());
        state.selected_user().saves()
    };
    open_folder(&folder)
}

/// Opens the Replays folder of the user data folder (`%APPDATA%\GT4Recomp\Replays`) in Explorer.
#[tauri::command]
pub fn open_replays_folder(shared: State<'_, Shared>) -> Result<(), String> {
    let folder = {
        let state = lock(shared.inner());
        state.selected_user().replays()
    };
    open_folder(&folder)
}

/// Opens the custom texture folder (`<install>\data\Textures`) in Explorer. It is made first, so the owner can
/// drop a pack in (TASK-088; TASK-093: the pack goes straight in, no replacements subfolder).
#[tauri::command]
pub fn open_textures_folder(shared: State<'_, Shared>) -> Result<(), String> {
    let (location, _) = lock(shared.inner()).texture_location();
    // TASK-157: a folder the player chose is opened as it is, never created.
    if !location.custom {
        paths::ensure_texture_folders(&location.folder).map_err(|error| error.to_string())?;
    }
    open_folder(&location.folder)
}

/// What the page shows of the textures folder choice (TASK-157): `path` is the stored choice (empty for the default),
/// `default` the default folder, `valid` whether the choice can be used now (a missing folder falls back to the default).
fn textures_state_of(state: &AppState) -> Value {
    let chosen = state.config.textures_path.as_deref();
    let valid = chosen.is_none_or(|path| gt4l_core::textures::check_textures_path(path, state.dirs.root()).is_ok());
    // A folder synced from the phone carries the name the player saw in the picker, so the page can show plain words
    // instead of the app's internal path.
    let label = chosen.and_then(gt4l_core::textures::source_name).unwrap_or_default();
    json!({
        "path": chosen.map(|path| path.display().to_string()).unwrap_or_default(),
        "label": label,
        "default": state.dirs.textures().display().to_string(),
        "custom": chosen.is_some(),
        "valid": valid,
    })
}

/// `textures_state()`: the textures folder choice, for the Settings page.
#[tauri::command]
pub fn textures_state(shared: State<'_, Shared>) -> Value {
    textures_state_of(&lock(shared.inner()))
}

/// `choose_textures_folder()`: the player picks an existing textures folder (for example the `textures` folder of PCSX2)
/// to be read in place (TASK-157). The folder is checked, stored and the direct-start files are rewritten; nothing is
/// copied or written into it. Returns the new state, or `None` when the player cancels.
#[tauri::command]
pub async fn choose_textures_folder(app: AppHandle, shared: State<'_, Shared>) -> Result<Option<Value>, String> {
    let shared = shared.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        // Only the desktop picker's texts use it; on the phone the messages are the app's own.
        #[allow(unused_variables)]
        let language = state::language_of(&lock(&shared));
        // TASK-180 (owner, 2026-11-06): on the phone the system folder picker chooses a folder (a tree the app keeps a
        // persistable read permission to). The ARMSX2 GS reads plain file paths, never content URIs, so the picked
        // tree is copied once into the app's own folder, laid out as a PCSX2 pack (`<serial>/replacements`); a later
        // start reads the copy, and nothing is copied again.
        #[cfg(target_os = "android")]
        let folder: PathBuf = {
            let _ = &app;
            let Some(picked) = crate::android::pick_textures_folder()? else {
                return Ok(None);
            };
            let uri = picked.uri.clone().unwrap_or_default();
            let (destination, staging) = {
                let state = lock(&shared);
                let destination = state.dirs.root().join(gt4l_core::textures::PHONE_SYNC_FOLDER_NAME);
                (destination.clone(), destination.with_extension("incoming"))
            };
            if staging.exists() {
                fs::remove_dir_all(&staging).map_err(|error| format!("textures: {}: {error}", staging.display()))?;
            }
            let files = crate::android::sync_textures_tree(&uri, &staging)?;
            let serial = gt4l_core::games::GT4.serial;
            let layout = gt4l_core::textures::prepare_phone_textures(&staging, serial)
                .map_err(|error| format!("textures: {}: {error}", staging.display()))?;
            if let Some(name) = picked.name.as_deref() {
                let _ = fs::write(staging.join(gt4l_core::textures::SOURCE_NAME_FILE), name);
            }
            if destination.exists() {
                fs::remove_dir_all(&destination)
                    .map_err(|error| format!("textures: {}: {error}", destination.display()))?;
            }
            fs::rename(&staging, &destination).map_err(|error| format!("textures: {}: {error}", destination.display()))?;
            let replacements = gt4l_core::textures::count_replacements(&destination, serial);
            {
                let state = lock(&shared);
                state::log(
                    &state.dirs,
                    &format!(
                        "textures: \"{}\" copied into {} ({} files, {} replacements under {}/{}/replacements, layout {:?})",
                        picked.name.as_deref().unwrap_or("the picked folder"),
                        destination.display(),
                        files,
                        replacements,
                        destination.display(),
                        serial,
                        layout
                    ),
                );
                if layout == gt4l_core::textures::PhoneLayout::Nothing {
                    state::log_warn(
                        &state.dirs,
                        "textures: the picked folder holds neither the game's folder nor a replacements folder; the pack will not load",
                    );
                }
            }
            destination
        };
        #[cfg(not(target_os = "android"))]
        let folder: PathBuf = {
            let start = {
                let state = lock(&shared);
                state
                    .config
                    .textures_path
                    .clone()
                    .filter(|path| path.is_dir())
                    .or_else(gt4l_platform::known_folder::documents)
                    .unwrap_or_else(|| state.dirs.root().to_path_buf())
            };
            let picked = app
                .dialog()
                .file()
                .set_title(messages::text("textures.pick_title", language))
                .set_directory(&start)
                .blocking_pick_folder();
            let Some(picked) = picked else {
                return Ok(None);
            };
            picked.into_path().map_err(|error| error.to_string())?
        };
        let mut state = lock(&shared);
        let checked = gt4l_core::textures::check_textures_path(&folder, state.dirs.root())
            .map_err(|error| messages::text(error.message_id(), language).to_string())?;
        state::log(&state.dirs, &format!("textures: folder chosen: {}", checked.display()));
        state.config.textures_path = Some(checked);
        state.save_config();
        Ok(Some(textures_state_of(&state)))
    })
    .await
    .map_err(|error| error.to_string())?
}

/// `reset_textures_folder()`: back to the default `Textures` folder. A folder the player chose on a computer is left as
/// it is; the phone's copy is the app's own, and going back to the default frees it.
#[tauri::command]
pub fn reset_textures_folder(shared: State<'_, Shared>) -> Value {
    let mut state = lock(shared.inner());
    if let Some(chosen) = state.config.textures_path.take() {
        let _ = &chosen;
        #[cfg(target_os = "android")]
        if chosen.file_name().is_some_and(|name| name == gt4l_core::textures::PHONE_SYNC_FOLDER_NAME)
            && chosen.parent() == Some(state.dirs.root())
        {
            let _ = fs::remove_dir_all(&chosen);
        }
        state::log(&state.dirs, "textures: back to the default folder");
        state.save_config();
    }
    textures_state_of(&state)
}

fn open_folder(folder: &Path) -> Result<(), String> {
    fs::create_dir_all(folder).map_err(|error| error.to_string())?;
    #[cfg(windows)]
    {
        std::process::Command::new("explorer.exe")
            .arg(&folder)
            .spawn()
            .map(|_| ())
            .map_err(|error| format!("could not open {}: {error}", folder.display()))
    }
    #[cfg(not(windows))]
    {
        let _ = folder;
        Ok(())
    }
}

/// Keeps the setup helpers in one place for tests of the command layer.
pub fn data_dirs_for(root: &Path) -> DataDirs {
    DataDirs::new(root)
}

/// The settings a fresh launcher starts with (used by the command tests).
pub fn default_settings() -> Settings {
    Settings::default()
}

/// Controls file path for a data folder (used by the command tests).
pub fn controls_file_for(root: &Path) -> PathBuf {
    root.join(controls_file::CONTROLS_FILE_NAME)
}

#[cfg(test)]
mod settings_table_probe_tests {
    use super::*;
    use gt4l_core::paths::{DataDirs, UserDirs, UserRootSource, USER_FOLDER_NAME};
    use gt4l_disc::Pins;
    use std::sync::{Arc, Mutex};

    fn shared_in(root: &Path) -> Shared {
        let dirs = DataDirs::new(root.join("data"));
        let user = UserDirs::new(root.join("appdata").join(USER_FOLDER_NAME), UserRootSource::Override);
        let state = AppState::load_with(dirs, Pins::embedded().expect("embedded pins"), user, None, root.to_path_buf())
            .expect("state loads");
        Arc::new(Mutex::new(state))
    }

    /// TASK-114 rc5: with the cards cached, opening the Settings table probes nothing (the old path probed twice).
    #[test]
    fn settings_table_reads_the_cached_cards_without_probing() {
        let root = std::env::temp_dir().join(format!("gt4l-table-probe-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        let shared = shared_in(&root);
        gt4l_platform::gpu::store_gpus(Vec::new());
        let before = gt4l_platform::gpu::probe_count();
        let table = get_settings_table_with_monitors(&shared, &[]).expect("table");
        assert!(table.get("rows").and_then(Value::as_array).is_some_and(|rows| !rows.is_empty()));
        assert_eq!(gt4l_platform::gpu::probe_count(), before, "the settings table must read the cache");
        let _ = std::fs::remove_dir_all(&root);
    }
}


// RetroAchievements -----------------------------------------------------------------------------

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RaStatusView {
    pub enabled: bool,
    pub hardcore: bool,
    pub encore: bool,
    pub signed_in: bool,
    pub username: String,
    pub display_name: String,
    pub score: u32,
    pub score_softcore: u32,
    pub game: Option<gt4l_core::retroachievements::RaGameOverview>,
    pub selected_game: String,
    pub supported: bool,
    pub relogin_required: bool,
    pub token_error_kind: Option<gt4l_core::retroachievements::TokenErrorKind>,
}

fn ra_base_url() -> String {
    std::env::var("GT4_RA_HOST")
        .ok()
        .map(|s| s.trim().to_string())
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| gt4l_core::retroachievements::DEFAULT_RA_BASE_URL.to_string())
}

fn build_ra_status_view(
    cfg: &gt4l_core::retroachievements::RaConfig,
    game: Option<gt4l_core::retroachievements::RaGameOverview>,
    selected: &games::Game,
) -> RaStatusView {
    let supported = selected.id == games::GT4.id;
    let relogin_required = cfg.token_error.is_some()
        || (!cfg.username.is_empty() && cfg.token.is_empty() && cfg.token_protected.is_some());
    let token_error_kind = cfg.token_error.or_else(|| {
        if !cfg.username.is_empty() && cfg.token.is_empty() && cfg.token_protected.is_some() {
            Some(gt4l_core::retroachievements::TokenErrorKind::Inaccessible)
        } else {
            None
        }
    });
    RaStatusView {
        enabled: cfg.enabled,
        hardcore: cfg.hardcore,
        encore: cfg.encore,
        signed_in: cfg.is_signed_in(),
        username: cfg.username.clone(),
        display_name: if cfg.display_name.is_empty() {
            cfg.username.clone()
        } else {
            cfg.display_name.clone()
        },
        score: cfg.score,
        score_softcore: cfg.score_softcore,
        game: if supported { game } else { None },
        selected_game: selected.id.to_string(),
        supported,
        relogin_required,
        token_error_kind,
    }
}

/// Returns the current RetroAchievements account state, mode toggles, and GT4 achievement overview.
#[tauri::command]
pub async fn ra_status(shared: State<'_, Shared>) -> Result<RaStatusView, String> {
    let shared_clone = shared.inner().clone();
    tauri::async_runtime::spawn_blocking(move || ra_status_of(&shared_clone))
        .await
        .map_err(|e| format!("RetroAchievements status task failed: {e}"))?
}

pub fn ra_status_of(shared: &Shared) -> Result<RaStatusView, String> {
    ra_status_at(&ra_base_url(), shared)
}

pub fn ra_status_at(base_url: &str, shared: &Shared) -> Result<RaStatusView, String> {
    ra_status_at_with(base_url, shared, &gt4l_core::secret::default_protector())
}

pub fn ra_status_at_with(
    base_url: &str,
    shared: &Shared,
    protector: &dyn gt4l_core::secret::SecretProtector,
) -> Result<RaStatusView, String> {
    ra_status_with_protector(shared, protector, |cfg, game_id| {
        let session = gt4l_core::retroachievements::login_with_token(base_url, &cfg.username, &cfg.token).ok();
        let game = if game_id == games::GT4.id {
            gt4l_core::retroachievements::fetch_game_overview(base_url, &cfg.username, &cfg.token).ok()
        } else {
            None
        };
        (session, game)
    })
}

pub(crate) fn ra_status_with(
    shared: &Shared,
    refresh: impl FnOnce(&gt4l_core::retroachievements::RaConfig, &'static str) -> (
        Option<gt4l_core::retroachievements::RaAccountSession>,
        Option<gt4l_core::retroachievements::RaGameOverview>,
    ),
) -> Result<RaStatusView, String> {
    ra_status_with_protector(shared, &gt4l_core::secret::default_protector(), refresh)
}

pub(crate) fn ra_status_with_protector(
    shared: &Shared,
    protector: &dyn gt4l_core::secret::SecretProtector,
    refresh: impl FnOnce(&gt4l_core::retroachievements::RaConfig, &'static str) -> (
        Option<gt4l_core::retroachievements::RaAccountSession>,
        Option<gt4l_core::retroachievements::RaGameOverview>,
    ),
) -> Result<RaStatusView, String> {
    let (snapshot, revision, selected) = {
        let state = lock(shared);
        let cfg = gt4l_core::retroachievements::RaConfig::load_with(&state.user_dirs.retroachievements_file(), protector);
        (cfg, state.ra_revision, state.selected)
    };

    if !snapshot.is_signed_in() {
        return Ok(build_ra_status_view(&snapshot, None, selected));
    }

    let (session, game) = refresh(&snapshot, selected.id);

    let state = lock(shared);
    let path = state.user_dirs.retroachievements_file();
    let mut current = gt4l_core::retroachievements::RaConfig::load_with(&path, protector);

    if state.ra_revision != revision || state.selected.id != selected.id || current.username != snapshot.username || current.token != snapshot.token {
        return Ok(build_ra_status_view(&current, None, state.selected));
    }

    if let Some(sess) = session {
        current.display_name = sess.display_name;
        current.score = sess.score;
        current.score_softcore = sess.score_softcore;
        let _ = current.save_with(&path, protector);
    }

    Ok(build_ra_status_view(&current, game, state.selected))
}

/// Signs in to RetroAchievements with username and password, storing the API token in `retroachievements.toml`.
#[tauri::command]
pub async fn ra_login(username: String, password: String, shared: State<'_, Shared>) -> Result<RaStatusView, String> {
    let shared_clone = shared.inner().clone();
    tauri::async_runtime::spawn_blocking(move || ra_login_of(&username, &password, &shared_clone))
        .await
        .map_err(|e| format!("RetroAchievements login task failed: {e}"))?
}

pub fn ra_login_of(username: &str, password: &str, shared: &Shared) -> Result<RaStatusView, String> {
    ra_login_at(&ra_base_url(), username, password, shared)
}

pub fn ra_login_at(base_url: &str, username: &str, password: &str, shared: &Shared) -> Result<RaStatusView, String> {
    ra_login_at_with(base_url, username, password, shared, &gt4l_core::secret::default_protector())
}

pub fn ra_login_at_with(
    base_url: &str,
    username: &str,
    password: &str,
    shared: &Shared,
    protector: &dyn gt4l_core::secret::SecretProtector,
) -> Result<RaStatusView, String> {
    let (revision, selected) = {
        let mut state = lock(shared);
        state.ra_revision = state.ra_revision.wrapping_add(1);
        (state.ra_revision, state.selected)
    };

    let session = gt4l_core::retroachievements::login_with_password(base_url, username, password)?;

    let (ra_path, dirs) = {
        let state = lock(shared);
        let ra_path = state.user_dirs.retroachievements_file();
        let mut cfg = gt4l_core::retroachievements::RaConfig::load_with(&ra_path, protector);
        if state.ra_revision != revision {
            return Ok(build_ra_status_view(&cfg, None, state.selected));
        }
        cfg.username = session.username;
        cfg.token = session.token;
        cfg.display_name = session.display_name;
        cfg.score = session.score;
        cfg.score_softcore = session.score_softcore;
        cfg.save_with(&ra_path, protector)?;
        (ra_path, state.dirs.clone())
    };

    state::log(&dirs, &format!("retroachievements: signed in as {}", username.trim()));

    let game = if selected.id == games::GT4.id {
        let cfg = gt4l_core::retroachievements::RaConfig::load_with(&ra_path, protector);
        gt4l_core::retroachievements::fetch_game_overview(&base_url, &cfg.username, &cfg.token).ok()
    } else {
        None
    };

    let state = lock(shared);
    let current = gt4l_core::retroachievements::RaConfig::load_with(&state.user_dirs.retroachievements_file(), protector);
    let valid_game = if state.ra_revision == revision && state.selected.id == selected.id {
        game
    } else {
        None
    };
    Ok(build_ra_status_view(&current, valid_game, state.selected))
}

/// Signs out of RetroAchievements and removes the stored API token from `retroachievements.toml`.
#[tauri::command]
pub fn ra_logout(shared: State<'_, Shared>) -> Result<RaStatusView, String> {
    ra_logout_of(shared.inner())
}

pub fn ra_logout_of(shared: &Shared) -> Result<RaStatusView, String> {
    ra_logout_with(shared, &gt4l_core::secret::default_protector())
}

pub fn ra_logout_with(
    shared: &Shared,
    protector: &dyn gt4l_core::secret::SecretProtector,
) -> Result<RaStatusView, String> {
    let mut state = lock(shared);
    let ra_path = state.user_dirs.retroachievements_file();
    let mut cfg = gt4l_core::retroachievements::RaConfig::load_with(&ra_path, protector);
    cfg.clear_credentials();
    cfg.save_with(&ra_path, protector)?;
    state.ra_revision = state.ra_revision.wrapping_add(1);
    state::log(&state.dirs, "retroachievements: signed out");
    Ok(build_ra_status_view(&cfg, None, state.selected))
}

/// Updates RetroAchievements mode options (`enabled`, `hardcore`, `encore`).
#[tauri::command]
pub fn ra_set_options(
    enabled: bool,
    hardcore: bool,
    encore: bool,
    shared: State<'_, Shared>,
) -> Result<RaStatusView, String> {
    ra_set_options_of(enabled, hardcore, encore, shared.inner())
}

pub fn ra_set_options_of(
    enabled: bool,
    hardcore: bool,
    encore: bool,
    shared: &Shared,
) -> Result<RaStatusView, String> {
    ra_set_options_with(enabled, hardcore, encore, shared, &gt4l_core::secret::default_protector())
}

pub fn ra_set_options_with(
    enabled: bool,
    hardcore: bool,
    encore: bool,
    shared: &Shared,
    protector: &dyn gt4l_core::secret::SecretProtector,
) -> Result<RaStatusView, String> {
    ra_set_options_with_custom(enabled, hardcore, encore, shared, protector, &gt4l_core::retroachievements::StdAtomicFileOps)
}

pub fn ra_set_options_with_custom(
    enabled: bool,
    hardcore: bool,
    encore: bool,
    shared: &Shared,
    protector: &dyn gt4l_core::secret::SecretProtector,
    ops: &dyn gt4l_core::retroachievements::AtomicFileOps,
) -> Result<RaStatusView, String> {
    let mut state = lock(shared);
    let ra_path = state.user_dirs.retroachievements_file();
    let mut cfg = gt4l_core::retroachievements::RaConfig::load_with_custom(&ra_path, protector, ops);
    cfg.enabled = enabled;
    cfg.hardcore = hardcore;
    cfg.encore = encore;
    cfg.save_with_custom(&ra_path, protector, ops)?;
    state.ra_revision = state.ra_revision.wrapping_add(1);
    Ok(build_ra_status_view(&cfg, None, state.selected))
}

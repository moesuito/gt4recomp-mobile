//! Choosing GSdx in Settings (TASK-089 extension, release 1.2.0). GSdx is optional: CoreGS 2.0 is inside the game.
//! When GSdx is missing the page asks first and shows the download in its dialog (`gsdx_download_needed`, `download_gsdx`), and
//! stores the choice only after the install; `start` stays as the guard of a choice stored any other way.
//! Choosing it installs GSdx at once when it is missing, with the progress of the renderer install
//! (`renderer-progress`). A failed install is reported, and Play does not start the game until GSdx is there
//! (`launch::choose_renderer`). Setup installs GSdx itself (`setup.rs`), so a choice made during setup does not start
//! a second install.
//! The other way in is Play with GSdx stored but not installed (its download failed, for example with GitHub blocked;
//! issues #139 and #179): when this PC's card has Vulkan 1.2, Play asks first (`coregs20_offer`) and the player can switch
//! to CoreGS 2.0 (`use_coregs20`), which is inside the game, instead of being left with an error.

use gt4l_core::gpu::GpuDevice;
use gt4l_core::gs_release::{self, Step};
use gt4l_core::language::Language;
use gt4l_core::launch;
use gt4l_core::messages;
use gt4l_core::platform::Platform;
use gt4l_core::settings::{self, Settings};
use serde_json::{json, Value};
use tauri::{AppHandle, Emitter, State};
use tauri_plugin_dialog::DialogExt;

use crate::state::{lock, Shared};
use crate::{commands, state, updates};

/// Shown after a successful switch.
pub const INSTALLED_NOTICE_ID: &str = "renderer.installed";

/// Whether choosing the renderer asks for the GSdx install: the game is set up (setup installs it otherwise) and
/// GSdx is missing or too old.
pub fn needs_install(home_ready: bool, installed: bool) -> bool {
    home_ready && !installed
}

/// What the switch did.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Outcome {
    NotNeeded,
    Installed,
    Failed(String),
}

/// The notice after a failed install: the reason, and what the player can do.
pub fn failure_notice(message: &str) -> String {
    gt4l_core::messages::render("renderer.failure", crate::state::current_language(), &[("message", message)])
}

/// Runs the install when one is needed. `install` runs at most once; a failure is returned to the caller.
pub fn run(home_ready: bool, installed: bool, install: &mut dyn FnMut() -> Result<(), String>) -> Outcome {
    if !needs_install(home_ready, installed) {
        return Outcome::NotNeeded;
    }
    match install() {
        Ok(()) => Outcome::Installed,
        Err(message) => Outcome::Failed(message),
    }
}

/// `gsdx_download_needed()`: whether choosing GSdx now needs a download. Before setup it does not: setup installs it. On a
/// platform without GSdx (Android, TASK-180) the answer is always no: there is no row to choose and no program to fetch.
#[tauri::command]
pub fn gsdx_download_needed(shared: State<'_, Shared>) -> bool {
    if !launch::gsdx_installs_here(Platform::current()) {
        return false;
    }
    let state = lock(shared.inner());
    needs_install(state.home_ready(), !gt4l_core::gs_release::needs_install(&state.dirs.renderer_root()))
}

/// What the dialog hears about a step of the download.
pub fn download_event(step: Step) -> Value {
    match step {
        Step::Transfer { done, total } => json!({ "step": "downloading", "bytesDone": done, "bytesTotal": total }),
        other => json!({ "step": updates::step_name(other) }),
    }
}

/// `download_gsdx()`: installs GSdx, with `gsdx-download` events for the dialog. An error is the notice the dialog shows.
#[tauri::command(async)]
pub fn download_gsdx(app: AppHandle, shared: State<'_, Shared>) -> Result<(), String> {
    // TASK-180 (owner, 2026-11-06): the download never starts where GSdx does not exist. The page does not offer it there
    // either; this is the guard behind that.
    if !launch::gsdx_installs_here(Platform::current()) {
        return Err(launch::GSDX_WINDOWS_ONLY.to_string());
    }
    let dirs = lock(shared.inner()).dirs.clone();
    updates::install_renderer(&dirs, &mut |step| {
        let _ = app.emit("gsdx-download", download_event(step));
    })
    .map(|_| ())
    .map_err(|message| {
        state::log_warn(&dirs, &format!("GSdx not installed from the dialog in Settings: {message}"));
        failure_notice(&message)
    })
}

/// `install_gsdx_archive()`: the download failed (blocked network, proxy), so the player picks the release package by hand
/// (TASK-158). The file must be named `gt4-gs-<version>-windows-x64.zip`; it is checked against the `SHA256SUMS` of that
/// release, fetched online, exactly like the download, and refused when they cannot be fetched or do not match. `None` means
/// the picker was cancelled; an error is the text the dialog shows. Emits the same `gsdx-download` steps.
#[tauri::command]
pub async fn install_gsdx_archive(app: AppHandle, shared: State<'_, Shared>) -> Result<Option<()>, String> {
    let (dirs, language) = {
        let state = lock(shared.inner());
        (state.dirs.clone(), state::language_of(&state))
    };
    tauri::async_runtime::spawn_blocking(move || {
        // TASK-180: GSdx (gt4-gs.exe) is Windows-only; Android has CoreGS 2.0 alone.
        if !launch::gsdx_installs_here(Platform::current()) {
            let _ = (&app, &dirs);
            return Err(launch::GSDX_WINDOWS_ONLY.to_string());
        }
        let picked = app
            .dialog()
            .file()
            .set_title(messages::text("renderer.archive_picker_title", language))
            .add_filter(messages::text("renderer.archive_picker_filter", language), &["zip"])
            .blocking_pick_file();
        let Some(picked) = picked else {
            return Ok(None);
        };
        let path = picked.into_path().map_err(|error| error.to_string())?;
        updates::install_renderer_archive(&dirs, &path, language, &mut |step| {
            let _ = app.emit("gsdx-download", download_event(step));
        })
        .map(|_| Some(()))
        .map_err(|message| {
            state::log_warn(&dirs, &format!("GSdx not installed from {}: {message}", path.display()));
            message
        })
    })
    .await
    .map_err(|error| error.to_string())?
}

/// Whether the card a run would use has Vulkan 1.2, so CoreGS 2.0 can run on it (the card list is the cached one).
fn card_runs_coregs20(settings: &Settings, language: Language) -> bool {
    let (_, graphics, _) = gt4l_platform::gpu::run_settings(settings, language);
    graphics.card.as_ref().is_some_and(GpuDevice::has_vulkan)
}

/// `coregs20_offer()`: whether Play should offer CoreGS 2.0 before it starts (`launch::offers_coregs20`): GSdx is the stored
/// renderer, it is not installed, and this PC's card has Vulkan 1.2. False when Play can just go on (GSdx installed, CoreGS
/// 2.0 already chosen), and when there is no such card: the plain "GSdx is not installed yet" message then stays.
#[tauri::command(async)]
pub fn coregs20_offer(shared: State<'_, Shared>) -> bool {
    // TASK-180 (owner, 2026-11-06): Play's "GSdx is missing" offer belongs to the PC alone. Android always plays with CoreGS
    // 2.0, so it never asks and never shows a GSdx download.
    if !launch::gsdx_installs_here(Platform::current()) {
        return false;
    }
    let (settings, renderer_root, language) = {
        let state = lock(shared.inner());
        (state.config.settings.clone(), state.dirs.renderer_root(), state::language_of(&state))
    };
    let gsdx_installed = gs_release::installed(&renderer_root).is_some();
    launch::offers_coregs20(&settings, gsdx_installed, card_runs_coregs20(&settings, language))
}

/// `use_coregs20()`: the player accepted Play's offer. CoreGS 2.0 becomes the stored renderer (the page then goes on with
/// the shader preparation and Play). An error is the text to show.
#[tauri::command(async)]
pub fn use_coregs20(shared: State<'_, Shared>) -> Result<(), String> {
    let (settings, language) = {
        let state = lock(shared.inner());
        (state.config.settings.clone(), state::language_of(&state))
    };
    commands::use_coregs20_for_play(shared.inner(), card_runs_coregs20(&settings, language))
}

/// Starts the switch on a worker thread after a `renderer` setting was saved; Settings is never blocked.
pub fn start(app: AppHandle, shared: Shared) {
    let (dirs, home_ready, installed) = {
        let state = lock(&shared);
        // CoreGS 2.0 is inside the game executable: nothing to install.
        if state.config.settings.get("renderer") != settings::PCSX2_GS {
            return;
        }
        let installed = !gt4l_core::gs_release::needs_install(&state.dirs.renderer_root());
        (state.dirs.clone(), state.home_ready(), installed)
    };
    if !needs_install(home_ready, installed) {
        return;
    }
    std::thread::spawn(move || {
        let progress_app = app.clone();
        let outcome = run(home_ready, installed, &mut || {
            updates::install_renderer(&dirs, &mut |step| updates::emit_step(&progress_app, step)).map(|_| ())
        });
        match outcome {
            Outcome::NotNeeded => {}
            Outcome::Installed => {
                let _ = app.emit("renderer-switched", json!({ "ok": true, "renderer": settings::PCSX2_GS, "message": gt4l_core::messages::text(INSTALLED_NOTICE_ID, crate::state::current_language()) }));
            }
            Outcome::Failed(message) => {
                state::log(&dirs, &format!("GSdx not installed after the switch in Settings: {message}"));
                let notice = failure_notice(&message);
                let _ = app.emit("renderer-switched", json!({ "ok": false, "renderer": settings::PCSX2_GS, "message": notice }));
            }
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_dialog_hears_each_step_and_the_bytes_of_the_download() {
        assert_eq!(download_event(Step::Listing), json!({ "step": "listing" }));
        assert_eq!(
            download_event(Step::Transfer { done: 5, total: Some(10) }),
            json!({ "step": "downloading", "bytesDone": 5, "bytesTotal": 10 })
        );
        assert_eq!(download_event(Step::Installing), json!({ "step": "installing" }));
    }

    #[test]
    fn choosing_gsdx_triggers_exactly_one_install_when_it_is_missing() {
        let mut calls = 0;
        let outcome = run(true, false, &mut || {
            calls += 1;
            Ok(())
        });
        assert_eq!(outcome, Outcome::Installed);
        assert_eq!(calls, 1, "exactly one install");
    }

    #[test]
    fn no_install_when_gsdx_is_present_or_before_setup_is_done() {
        let mut calls = 0;
        assert_eq!(run(true, true, &mut || { calls += 1; Ok(()) }), Outcome::NotNeeded);
        assert_eq!(run(false, false, &mut || { calls += 1; Ok(()) }), Outcome::NotNeeded, "setup installs it");
        assert_eq!(calls, 0, "no install was requested");
    }

    #[test]
    fn a_failed_install_is_reported_with_the_reason_and_what_to_do() {
        let mut calls = 0;
        let outcome = run(true, false, &mut || {
            calls += 1;
            Err("offline".to_string())
        });
        assert_eq!(outcome, Outcome::Failed("offline".to_string()));
        assert_eq!(calls, 1, "one attempt, no retry loop");
        let notice = failure_notice("offline");
        assert!(notice.contains("GSdx could not be installed (offline)"));
        assert!(notice.contains("cannot start until GSdx is installed"));
        assert!(!notice.contains("CoreGS"), "no CoreGS 1.1 fallback");
        assert_eq!(gt4l_core::messages::text(INSTALLED_NOTICE_ID, gt4l_core::language::Language::fallback()), "GSdx installed successfully");
    }
}

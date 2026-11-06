//! GT4Recomp launcher (Tauri 2): the library behind `GT4Recomp.exe` (src/main.rs) and, on Android (TASK-180), the app's
//! main activity (`run` is its mobile entry point). The page is `ui/` (plain HTML, CSS and JS, bundled; no remote
//! content). The Rust side holds all state and talks to the game through the `gt4l-*` crates. See
//! `docs/launcher/LAUNCHER-PLAN.md` section 15 and `docs/ANDROID-PORT-PLAN.md` section 4.

mod commands;
mod discord_presence;
#[cfg(test)]
mod contract;
#[cfg(test)]
mod flow_audit;
mod model;
mod pad_nav;
mod pad_service;
mod relocation;
mod renderer_switch;
mod saves;
mod log_bundle;
mod managed_states;
mod session_log;
mod setup;
mod state;
#[cfg(desktop)]
mod tray;
mod updates;
mod webview_guard;
mod wheel_page;
mod wheel_service;
#[cfg(feature = "visual-check")]
mod visual;

#[cfg(desktop)]
use std::sync::{Arc, Mutex};
#[cfg(desktop)]
use std::time::Instant;

#[cfg(desktop)]
use tauri::Manager;

#[cfg(desktop)]
use gt4l_core::LAUNCHER_VERSION;

/// The desktop launcher (Windows): everything `GT4Recomp.exe` does from its start to its exit.
#[cfg(desktop)]
pub fn run() {
    // TASK-091 (1.2.1 start-up timings): the time from this point to `ui_ready` is logged, with the steps before the window.
    let started = Instant::now();
    // A second launcher brings the first window forward instead of starting. A restart in safe mode (TASK-091) waits
    // for the launcher that is exiting instead.
    let restarted = webview_guard::restart_requested(std::env::args().skip(1));
    let portable = gt4l_core::build_kind::BUILD.is_portable();
    let exe_dir = updates::install_directory();
    if portable {
        // TASK-098: the lock belongs to this folder, so two portable copies (or a portable and the installed one) on one PC do
        // not block each other, and one copy cannot start twice.
        gt4l_platform::process::set_single_instance_scope(&exe_dir);
    }
    // A launcher started by the portable self-update waits for the old one to exit, as a safe-mode restart does.
    let after_update = std::env::args().skip(1).any(|argument| argument == gt4l_core::portable_update::RESTART_ARGUMENT);
    let claimed = if restarted || after_update {
        gt4l_platform::process::claim_single_instance_retrying(
            webview_guard::RESTART_CLAIM_TIMEOUT,
            webview_guard::RESTART_CLAIM_STEP,
        )
    } else {
        gt4l_platform::process::claim_single_instance()
    };
    if !claimed {
        return;
    }
    // TASK-091: large data in `<exe dir>\data` (portable), user data in `%APPDATA%\GT4Recomp`. The 1.2.0 user
    // data moves here, before the window; the large data moves in the background below.
    let startup = match relocation::prepare(&exe_dir) {
        Ok(startup) => startup,
        Err(message) => {
            eprintln!("GT4Recomp cannot find its folders: {message}");
            // No window and no console: a portable folder that cannot be written gets a native message.
            if portable {
                gt4l_platform::dialog::error_box_ok("GT4Recomp", &message);
            }
            return;
        }
    };
    let dirs = startup.layout.data.clone();
    let _ = std::fs::create_dir_all(dirs.root());
    // TASK-112: one log file for this launch, with a header (version, install kind, Windows, date).
    let header = vec![
        format!("GT4Recomp {}", gt4l_core::LAUNCHER_VERSION),
        format!("Install: {}", if portable { "portable" } else { "installer" }),
        format!("Windows: {}", gt4l_platform::system::windows_version()),
        format!("Date: {}", gt4l_core::logs::LocalTime::stamp(&gt4l_platform::system::local_time())),
    ];
    if let Err(message) = session_log::start(&dirs, &header) {
        eprintln!("GT4Recomp cannot open its log: {message}");
    }
    if portable {
        // TASK-098: finish or undo the previous update before anything reads the program files.
        updates::portable_start_cleanup(&dirs, &exe_dir);
    }
    state::log(
        &dirs,
        &format!("GT4Recomp {LAUNCHER_VERSION} ({} build) starting from {}", gt4l_core::build_kind::BUILD.label(), exe_dir.display()),
    );
    state::log(&dirs, &format!("start-up: data layout and relocation ready after {} ms", started.elapsed().as_millis()));
    for line in &startup.log {
        state::log(&dirs, line);
    }
    // The old per-user install clean-up is not needed to draw the first screen: it runs after the window is created
    // (see the setup closure below).
    if let Err(message) = gt4l_platform::process::join_kill_on_close_job() {
        state::log(&dirs, &format!("the game would not end with the launcher: {message}"));
    }
    // TASK-091 WebView2 guard: the safe mode flag decides the WebView2 arguments, before any web view exists.
    let user_root = startup.user.root().to_path_buf();
    let safe_mode = webview_guard::apply_safe_mode_environment(&user_root);
    let webview_version = tauri::webview_version().unwrap_or_else(|_| "unknown".to_string());
    state::log(&dirs, &format!("WebView2 runtime version: {webview_version}"));
    // TASK-091 (issue #80): a missing runtime is reported before any window exists, not after the watchdog's wait.
    if webview_guard::check_webview2_at_start(&dirs) {
        webview_guard::show_failure(&dirs, webview_guard::Problem::Missing);
        return;
    }
    if safe_mode {
        state::log(&dirs, "WebView2 safe mode (GPU acceleration off)");
    } else {
        state::log(&dirs, "WebView2 safe mode off (GPU acceleration on)");
    }
    if restarted {
        state::log(&dirs, "started again in WebView2 safe mode after the window did not draw");
    }
    let guard = Arc::new(webview_guard::Guard {
        page: webview_guard::PageReady::default(),
        dirs: dirs.clone(),
        user_root,
        started,
    });
    let settings_started = Instant::now();
    let mut app_state = match state::AppState::load(dirs.clone(), startup.user.clone(), startup.layout.legacy.local.clone()) {
        Ok(app_state) => app_state,
        Err(message) => {
            state::log_error(&dirs, &format!("the launcher could not start: {message}"));
            return;
        }
    };
    state::log(&dirs, &format!("start-up: settings loaded in {} ms", settings_started.elapsed().as_millis()));
    if let Some((notice, problem)) = startup.notice.clone() {
        app_state.migration.notice = notice;
        app_state.migration.problem = problem;
    }
    let legacy = startup.layout.legacy.clone();
    let clean_exe_dir = exe_dir.clone();
    #[cfg(feature = "visual-check")]
    visual::seed(&app_state);

    let shared: state::Shared = Arc::new(Mutex::new(app_state));
    let dirs_for_setup = dirs.clone();
    let shared_for_check = shared.clone();
    let guard_for_setup = guard.clone();
    let mut context = tauri::generate_context!();
    if portable {
        // TASK-098: the web view keeps its profile in `data\launcher-webview`, not in %LOCALAPPDATA%, so a portable copy
        // leaves nothing in the system folders.
        context.config_mut().app.app_directories_override =
            Some(tauri::utils::config::AppDirectoriesOverride::Root(dirs.root().join("launcher-webview")));
    }
    let run_result = tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(shared.clone())
        .manage(guard.clone())
        .setup(move |app| {
            tray::install(app)?;
            // TASK-091: the window is shown by ui_ready; without it in 12 s the watchdog restarts in safe mode.
            webview_guard::start_watchdog(app.handle().clone(), guard_for_setup);
            // The gamepad service (SDL3) runs on its own thread for the whole session.
            let pads = pad_service::spawn(app.handle().clone());
            app.manage(pads);
            // TASK-180 test3: every state the commands ask for is in the log at start-up (a missing one stops its
            // command with "state not managed for fields ...", which is how the Android pad commands failed).
            managed_states::check(app.handle());
            // Every launch: version file, first gt4-gs install if needed, and the update check (never blocks Play).
            let launch_shared = app.state::<state::Shared>().inner().clone();
            let launch_app = app.handle().clone();
            // The 1.2.0 large data moves first (Play and setup wait for it), then the version file and update check.
            std::thread::spawn(move || {
                relocation::move_large_data(&launch_shared, &legacy);
                relocation::flatten_textures(&launch_shared);
                // TASK-097: the files a direct start of the game reads (game.ini, gt4-gs.ini) follow the saved settings.
                let launch_state = state::lock(&launch_shared);
                launch_state.write_standalone_files(&launch_state.config.settings);
                drop(launch_state);
                updates::check_on_launch(launch_app, launch_shared);
            });
            // TASK-091 (1.2.1): the old per-user install clean-up runs in the background, after the window exists.
            let clean_dirs = dirs_for_setup.clone();
            std::thread::spawn(move || {
                let began = Instant::now();
                for line in relocation::clean_old_install(&clean_exe_dir) {
                    state::log(&clean_dirs, &line);
                }
                state::log(&clean_dirs, &format!("start-up: old install clean-up finished in {} ms", began.elapsed().as_millis()));
            });
            // TASK-081: probe the processor once in the background; a problem is logged here and shown when the player
            // starts setup or Play. TASK-108: the graphics cards are listed too, so the log shows what Play will choose from.
            let check_dirs = dirs_for_setup.clone();
            let language = state::language_of(&state::lock(&shared_for_check));
            std::thread::spawn(move || {
                match gt4l_platform::system::processor_problem(language) {
                    Some(problem) => state::log(&check_dirs, &format!("system requirement not met: {problem}")),
                    None => state::log(&check_dirs, "system requirements: processor OK"),
                }
                let (cards, inputs) = gt4l_platform::gpu::probe_gpus_with_inputs();
                for line in &inputs {
                    state::log(&check_dirs, line);
                }
                state::log(&check_dirs, &format!("graphics: cards [{}]", gt4l_platform::gpu::card_list(&cards)));
                // TASK-114 rc5: the Settings page, the saves and the log read this list; Play refreshes it.
                gt4l_platform::gpu::store_gpus(cards);
            });
            Ok(())
        })
        .on_window_event(|window, event| {
            // The close button ends the launcher, or asks first while the game runs. It never goes to the tray.
            if let tauri::WindowEvent::CloseRequested { api, .. } = event {
                if window.label() == "main" {
                    api.prevent_close();
                    tray::handle(window.app_handle(), tray::Event::CloseRequested);
                }
            }
        })
        .invoke_handler(tauri::generate_handler![
            commands::app_state,
            commands::get_settings_table,
            commands::set_setting,
            commands::reset_gs_settings_to_defaults,
            renderer_switch::gsdx_download_needed,
            renderer_switch::download_gsdx,
            renderer_switch::install_gsdx_archive,
            renderer_switch::coregs20_offer,
            renderer_switch::use_coregs20,
            setup::setup_choose_disc,
            setup::setup_start,
            setup::setup_cancel,
            commands::prepare_shaders,
            commands::cancel_shader_prepare,
            commands::play,
            commands::resume_game,
            commands::first_start_hint,
            commands::stop_game,
            commands::saves_list,
            commands::saves_import,
            commands::saves_activate,
            commands::saves_delete,
            commands::saves_new,
            commands::saves_export,
            commands::saves_rename,
            commands::replays_list,
            commands::replays_import,
            commands::replays_delete,
            webview_guard::ui_ready,
            webview_guard::ui_log,
            commands::get_controls,
            commands::pad_status,
            commands::capture_binding,
            commands::cancel_capture,
            commands::reset_binding,
            commands::assign_binding,
            commands::reset_all_bindings,
            commands::get_virtual_pad,
            commands::save_virtual_pad,
            commands::reset_virtual_pad,
            wheel_page::get_wheel_config,
            wheel_page::set_wheel_setting,
            wheel_page::apply_wheel_preset,
            wheel_page::assign_wheel_binding,
            wheel_page::reset_wheel_binding,
            wheel_page::reset_wheel_config,
            wheel_page::capture_wheel_binding,
            wheel_page::cancel_wheel_capture,
            wheel_page::test_wheel_ffb,
            wheel_page::wheel_watch,
            wheel_page::get_wheel_session_check,
            wheel_page::list_wheel_presets,
            wheel_page::save_wheel_preset,
            wheel_page::delete_wheel_preset,
            commands::about_info,
            commands::release_notes,
            commands::open_link,
            commands::open_log_folder,
            commands::open_latest_log,
            commands::export_logs,
            commands::open_export_folder,
            commands::open_game_files_folder,
            commands::open_player_folder,
            commands::open_saves_folder,
            commands::open_replays_folder,
            commands::open_textures_folder,
            commands::textures_state,
            commands::choose_textures_folder,
            commands::reset_textures_folder,
            commands::games_list,
            commands::select_game,
            commands::games_remove,
            commands::ra_status,
            commands::ra_login,
            commands::ra_logout,
            commands::ra_set_options,
            updates::check_updates,
            updates::list_launcher_versions,
            updates::install_launcher_version,
            updates::apply_update,
            updates::skip_update,
            tray::confirm_close,
        ])
        .run(context);
    // TASK-091: a web view that could not start (WebView2 missing) gets the native message, not a silent exit.
    if let Err(error) = run_result {
        state::log_error(&dirs, &format!("WebView2 is missing or could not start: {error}"));
        webview_guard::show_failure(&dirs, webview_guard::Problem::Missing);
    }
    // Clear Discord presence on launcher exit.
    state::lock(&shared).discord.on_launcher_exit();
}

/// TASK-180: the Android app's entry point (the Tauri main activity). See android.rs.
#[cfg(mobile)]
#[tauri::mobile_entry_point]
pub fn run() {
    android::run();
}

#[cfg(mobile)]
mod android;

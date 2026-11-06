//! Launch contract tests: the argument and environment construction is compared with what
//! `scripts/play_performance.py --mode main --force-480p` sends through `external_runtime_probe.run_probe`.
//!
//! Python reference (read from the scripts, not run): `run_probe` builds
//!   arguments = [exe, <session>/SCUS_973.28, --ntsc, --disc, <iso>, --frames, 0,
//!                --memcard, <card>, --misses, <session>/misses.txt, --window]
//! and, for performance profiles, `GT4_PERF=1 PS2_SCALE=1 PS2_GS=gpu`; `--force-480p` adds
//! `GT4_FORCE_480P=1`. The launcher matches that except where the approved design says otherwise:
//! `GT4_PERF` is only set when the Performance report is on (default off), and `GT4_FULLSCREEN` and
//! `GT4_PRESENT_MODE` are always stated. Those two differences are asserted explicitly below.

use std::path::{Path, PathBuf};

use gt4l_core::display::{DisplayTarget, Size};
use gt4l_core::launch::{self, LaunchInputs};
use gt4l_core::settings::{self, Settings};
use gt4l_core::textures::TextureLocation;

fn env_map(pairs: &[(String, String)]) -> std::collections::BTreeMap<String, String> {
    pairs.iter().cloned().collect()
}

/// The variables of a GSdx session (the only renderer since TASK-096).
fn game_environment(settings: &Settings) -> Vec<(String, String)> {
    launch::pcsx2_environment(settings, &pcsx2_paths())
}

/// The launch plan of a GSdx session.
fn plan_launch(
    inputs: &LaunchInputs<'_>,
    settings: &Settings,
    display: Option<&DisplayTarget>,
    inherited_names: &[String],
) -> launch::LaunchPlan {
    launch::plan_pcsx2_launch(inputs, settings, &pcsx2_paths(), display, inherited_names)
}

#[test]
fn default_settings_send_the_main_profile_variables_plus_the_launcher_defaults() {
    let environment = env_map(&game_environment(&Settings::default()));
    // Same as `--mode main --force-480p`: the platform variables and the 480p hold.
    assert_eq!(environment["PS2_GS"], "gpu");
    assert_eq!(environment["GT4_FORCE_480P"], "1");
    assert_eq!(environment["GT4_RENDERER"], "pcsx2");
    // Differences with the Python launcher, approved in LAUNCHER-PLAN section 6:
    assert!(!environment.contains_key("GT4_PERF"), "Performance report is off by default");
    assert_eq!(environment["GT4_FULLSCREEN"], "1");
    // 16:9 is fixed (owner, 2026-10-21): GT4_FORCE_WIDESCREEN=1 is always stated; there is no setting.
    assert_eq!(environment["GT4_FORCE_WIDESCREEN"], "1");
    // TASK-080 addendum: the aspect is always Auto (the output size's shape); there is no setting.
    assert_eq!(environment["GT4_DISPLAY_ASPECT"], "auto");
    assert!(!environment.contains_key("GT4_RACE_DISPLAY"), "480p is fixed: no 480i or 1080i path");
    assert_eq!(environment["GT4_PRESENT_MODE"], "fifo");
    // Asynchronous GS is the default, so GT4_GS_SYNC stays unset.
    assert!(!environment.contains_key("GT4_GS_SYNC"));
    // Nothing paraLLEl-GS specific, and no CoreGS 1.1 variable (TASK-096: the launcher never starts CoreGS 1.1).
    assert!(!environment.keys().any(|name| name.starts_with("GT4_PGS_")));
    for gone in ["PS2_SCALE", "GT4_DEINTERLACE", "GT4_AA", "GT4_PRESENT_FILTER"] {
        assert!(!environment.contains_key(gone), "{gone} is a CoreGS 1.1 variable");
    }
    // TASK-161: the full trigger range is always stated, on by default since rc14 (owner, 2026-11-04; the game reads it
    // for both of its sides).
    assert_eq!(environment["GT4_TRIGGER_FULL_RANGE"], "1");
    // TASK-180 test5: the flare effect toggle rides as the stream client's zero answer (TASK-117); every effect is on.
    assert_eq!(environment["GT4_GS_HIDE_FLARES"], "0", "the flares are drawn by default");
    assert_eq!(
        environment.len(),
        12,
        "the GSdx variables: renderer, process pin, config, PS2_GS, the flare effect and the common holds"
    );
}

/// TASK-180 test5: turning an effect off is what sends the renderer's switch, and the road sheen is CoreGS-only.
#[test]
fn the_effect_toggles_drive_the_renderer_switches() {
    let mut settings = Settings::default();
    assert_eq!(env_map(&game_environment(&settings))["GT4_GS_HIDE_FLARES"], "0");
    settings.set_gs("effect_flares", "false").unwrap();
    assert_eq!(
        env_map(&game_environment(&settings))["GT4_GS_HIDE_FLARES"],
        "1",
        "the flares are off"
    );

    let display = None;
    let textures = TextureLocation::default_folder(Path::new("C:/GT4Recomp/user/Textures"));
    let core = |settings: &Settings| env_map(&launch::coregs20_environment(settings, display, false, &textures));
    let all_on = core(&Settings::default());
    assert_eq!(all_on["GT4_CORE2_PERF_FLARES"], "0");
    assert_eq!(all_on["GT4_CORE2_PERF_ROAD_SHEEN"], "0");
    let mut one_off = Settings::default();
    one_off.set_gs("effect_road_sheen", "false").unwrap();
    let road = core(&one_off);
    assert_eq!(road["GT4_CORE2_PERF_ROAD_SHEEN"], "1", "the road sheen is off");
    assert_eq!(road["GT4_CORE2_PERF_FLARES"], "0", "the flares stay on");
    assert!(!road.contains_key("GT4_CORE2_PERFORMANCE"), "the old bundle is never sent");
}

#[test]
fn argument_list_matches_run_probe_order() {
    let session = Path::new(r"C:\Users\me\AppData\Local\GT4Recomp\sessions\session-1");
    let inputs = LaunchInputs {
        game_executable: Path::new(r"C:\Program Files\GT4Recomp\game\gt4.exe"),
        elf: Path::new(r"C:\Users\me\AppData\Local\GT4Recomp\prepared\SCUS_973.28"),
        disc: Path::new(r"D:\Games\GT4 (USA) (v2.00).iso"),
        card: Path::new(r"C:\Users\me\AppData\Local\GT4Recomp\runtime\card.ps2"),
        session_dir: session,
    };
    let plan = plan_launch(&inputs, &Settings::default(), None, &[]);
    assert_eq!(
        plan.arguments,
        vec![
            r"C:\Users\me\AppData\Local\GT4Recomp\prepared\SCUS_973.28",
            "--ntsc",
            "--disc",
            r"D:\Games\GT4 (USA) (v2.00).iso",
            "--frames",
            "0",
            "--memcard",
            r"C:\Users\me\AppData\Local\GT4Recomp\runtime\card.ps2",
            "--misses",
            r"C:\Users\me\AppData\Local\GT4Recomp\sessions\session-1\misses.txt",
            "--window",
        ]
    );
    assert_eq!(plan.working_directory, session);
    assert_eq!(plan.log_path, session.join("game.log"));
    assert_eq!(plan.program, Path::new(r"C:\Program Files\GT4Recomp\game\gt4.exe"));
}

#[test]
fn inherited_gt4_and_ps2_variables_are_scrubbed_and_others_kept() {
    let inputs = LaunchInputs {
        game_executable: Path::new("gt4.exe"),
        elf: Path::new("SCUS_973.28"),
        disc: Path::new("disc.iso"),
        card: Path::new("card.ps2"),
        session_dir: Path::new("session"),
    };
    let inherited = vec![
        "GT4_PACING".to_string(),
        "PS2_DUMP_AUDIO".to_string(),
        "PATH".to_string(),
        "VK_LAYER_PATH".to_string(),
        "GT4_ASPECT_FOV_EXTRA".to_string(),
        "GT4_AUDIO_LATENCY_MS".to_string(),
        "GT4_DISPLAY_ASPECT".to_string(),
    ];
    let plan = plan_launch(&inputs, &Settings::default(), None, &inherited);
    // TASK-080 T1: the hidden FOV factor passes through, and TASK-156's audio target; the aspect itself is always auto.
    assert_eq!(plan.removed_inherited, vec!["GT4_DISPLAY_ASPECT", "GT4_PACING", "PS2_DUMP_AUDIO"]);
    assert!(!plan.removed_inherited.iter().any(|name| name == "PATH" || name == "VK_LAYER_PATH"));
}

#[test]
fn settings_map_to_their_variables() {
    let mut settings = Settings::default();
    settings.set("vsync", "fast").unwrap();
    settings.set("performance_report", "on").unwrap();
    settings.set("check_vu_code", "1000").unwrap();
    let environment = env_map(&game_environment(&settings));
    assert_eq!(environment["GT4_FORCE_480P"], "1", "480p is fixed whatever the other settings");
    assert_eq!(environment["GT4_PRESENT_MODE"], "mailbox");
    assert_eq!(environment["GT4_PERF"], "1");
    assert_eq!(environment["GT4_VU1_VERIFY"], "1000");
    assert_eq!(environment["GT4_VU0_VERIFY"], "1000");

    assert_eq!(environment["GT4_FORCE_WIDESCREEN"], "1", "16:9 is fixed whatever the other settings");
    // TASK-080 addendum: no aspect or internal-resolution setting exists any more.
    for gone in ["aspect_ratio", "aspect_custom"] {
        assert!(settings.set(gone, "auto").is_err(), "{gone} is not a setting");
    }
    assert!(settings.set_gs("upscale", "4K").is_err(), "the internal resolution is derived, not set");
    for (id, good, bad) in [
        ("render_scale", "150", "1201"),
        ("resolution", "2560x1080", "2560*1080"),
        ("monitor", r"\\.\DISPLAY2@3440x1440", "DISPLAY2"),
    ] {
        settings.set(id, good).unwrap();
        assert!(settings.set(id, bad).is_err(), "{id} refuses {bad}");
    }
    assert_eq!(env_map(&game_environment(&settings))["GT4_DISPLAY_ASPECT"], "auto");
    settings.set("vsync", "off").unwrap();
    let environment = env_map(&game_environment(&settings));
    assert_eq!(environment["GT4_PRESENT_MODE"], "immediate");
}

#[test]
fn check_every_100th_call_maps_as_documented() {
    let mut settings = Settings::default();
    settings.set("check_vu_code", "100").unwrap();
    let environment = env_map(&game_environment(&settings));
    assert_eq!(environment["GT4_VU1_VERIFY"], "100");
    assert_eq!(environment["GT4_VU0_VERIFY"], "100");
}

#[test]
fn input_resolution_and_widescreen_are_retired_and_fixed() {
    // Owner, 2026-10-21: the game always runs at 480p and 16:9. The rows are gone; old values cannot be set.
    let mut settings = Settings::default();
    for id in ["input_resolution", "widescreen"] {
        assert!(settings::definition(id).is_none(), "{id} has no row");
        assert!(settings::retired(id).is_some(), "{id} is listed as retired");
        assert!(settings.set(id, "480i").is_err());
    }
    for environment in [env_map(&game_environment(&settings))] {
        assert_eq!(environment["GT4_FORCE_480P"], "1");
        assert_eq!(environment["GT4_FORCE_WIDESCREEN"], "1");
        assert!(!environment.contains_key("GT4_RACE_DISPLAY"));
    }
}

#[test]
fn removed_parallel_gs_renderer_and_its_options_are_refused() {
    let mut settings = Settings::default();
    assert!(settings.set("renderer", "parallel").is_err(), "paraLLEl-GS is dropped (decision 0070)");
    assert!(settings::definition("super_sampling").is_none());
    assert!(settings::definition("high_resolution_output").is_none());
    assert!(!env_map(&game_environment(&settings)).keys().any(|name| name.starts_with("GT4_PGS_")));
}

#[test]
fn asynchronous_gs_is_always_on_whatever_an_old_file_holds() {
    let mut settings = Settings::default();
    assert!(settings::definition("async_gs").is_none(), "async_gs is not a setting");
    assert!(settings::retired("async_gs").is_some(), "async_gs is listed as retired");
    assert!(settings.set("async_gs", "off").is_err(), "async_gs cannot be set");
    assert!(!env_map(&game_environment(&settings)).contains_key("GT4_GS_SYNC"));
    assert!(!launch::standalone_config_text(&settings, None).contains("async_gs"));
}

#[test]
fn the_coregs11_rows_are_gone_and_retired_so_their_stored_values_are_ignored() {
    let mut settings = Settings::default();
    for id in ["internal_scale", "present_filter", "fsr_sharpness", "deinterlacing", "anti_aliasing"] {
        assert!(settings::definition(id).is_none(), "{id} is a CoreGS 1.1 row, not a setting");
        assert!(settings::retired(id).is_some(), "{id} is listed as retired");
        assert!(settings.set(id, "1").is_err(), "{id} cannot be set");
    }
    let environment = env_map(&game_environment(&settings));
    assert!(!environment.contains_key("GT4_PRESENT_FILTER"));
    assert!(!environment.contains_key("GT4_FSR_SHARPNESS"));
}

#[test]
fn the_lines_of_the_shader_compile_are_read_and_anything_else_is_ignored() {
    use launch::{parse_shader_prepare_line as parse, ShaderPrepareLine as Line};
    assert_eq!(parse("shaders ready"), Some(Line::Ready));
    assert_eq!(parse("shaders progress 0 353"), Some(Line::Progress { done: 0, total: 353 }));
    assert_eq!(parse("shaders progress 12 353\r"), Some(Line::Progress { done: 12, total: 353 }));
    for other in ["", "shaders", "shaders progress", "shaders progress 1", "shaders progress a b", "shaders ready now", "[core2] ready"] {
        assert_eq!(parse(other), None, "{other}");
    }
}

#[test]
fn a_coregs20_session_runs_the_hw_renderer_with_the_shared_choices() {
    use gt4l_core::games;
    let textures = TextureLocation::default_folder(Path::new(r"D:\data\Textures"));
    let mut settings = Settings::default();
    assert!(!launch::uses_coregs20(&settings), "GSdx by default");
    settings.set("renderer", "coregs20").unwrap();
    assert!(launch::uses_coregs20(&settings), "the choice is shared by both games");

    let display = display_target(r"\\.\DISPLAY1", 1920, 1080);
    let on = env_map(&launch::coregs20_environment(&settings, Some(&display), false, &textures));
    assert_eq!(on["GT4_RENDERER"], "hw");
    assert_eq!(on["GT4_HW_CORE"], "2");
    assert_eq!(on["GT4_HW_SCALE"], "2", "100% of a 1080-high screen is 2x (TASK-180 test3)");
    assert_eq!(on["GT4_HW_TEXTURES"], r"D:\data\Textures", "custom textures are on by default");
    assert_eq!(on["GT4_FORCE_480P"], "1");
    assert_eq!(on["GT4_FORCE_WIDESCREEN"], "1");
    for gsdx_only in ["GT4_GS_PROCESS", "GT4_GS_PROCESS_SHA256", "GT4_GS_CONFIG", "PS2_GS"] {
        assert!(!on.contains_key(gsdx_only), "{gsdx_only} belongs to GSdx");
    }
    assert!(!on.contains_key("GT4_CORE2_PERFORMANCE"), "full precision unless Performance Mode is on");
    assert!(!on.contains_key("GT4_HW_FRAMETIME_LOG"), "no frametime.csv in the session folder (owner, 2026-11-03)");
    assert!(!on.contains_key("GT4_FRAME_SKIP_FORCE"), "no fixed pattern unless the player asks for 30 FPS");
    assert_eq!(on["GT4_FRAME_SKIP"], "1", "TASK-146: 60 FPS leaves frames out in a race only when the PC falls behind");
    assert!(launch::coregs20_section(&settings, Some(&display), false, &textures).contains("hw_frame_skip_adaptive=1\n"));
    assert!(!on.contains_key("GT4_CORE2_FSR_SHARPNESS"), "no sharpening by default");
    assert_eq!(on["GT4_PRESENT_MODE"], "fifo", "vsync On");

    // "Fast" is not a CoreGS 2.0 choice: a value stored for GSdx draws as On, and only there.
    settings.set("vsync", "fast").unwrap();
    let stored_fast = env_map(&launch::coregs20_environment(&settings, None, false, &textures));
    assert_eq!(stored_fast["GT4_PRESENT_MODE"], "fifo");
    assert!(launch::coregs20_section(&settings, None, false, &textures).contains("hw_present_mode=fifo\n"));
    settings.set("vsync", "off").unwrap();
    let vsync_off = env_map(&launch::coregs20_environment(&settings, None, false, &textures));
    assert_eq!(vsync_off["GT4_PRESENT_MODE"], "immediate", "Off stays a choice");
    assert!(!launch::coregs20_section(&settings, None, false, &textures).contains("hw_present_mode"));
    settings.set("vsync", "on").unwrap();

    settings.set_gs("effect_flares", "false").unwrap();
    settings.set_gs("effect_road_sheen", "false").unwrap();
    settings.set_gs("texture_load", "false").unwrap();
    settings.set_gs("fxaa", "true").unwrap();
    settings.set("coregs_sharpening", "medium").unwrap();
    settings.set("frame_rate", "30").unwrap();
    let tuned = env_map(&launch::coregs20_environment(&settings, None, true, &textures));
    assert_eq!(tuned["GT4_CORE2_PERF_FLARES"], "1", "the flare toggle off");
    assert_eq!(tuned["GT4_CORE2_PERF_ROAD_SHEEN"], "1", "the road sheen toggle off");
    assert!(!tuned.contains_key("GT4_CORE2_PERFORMANCE"), "the bundle is not sent any more");
    assert_eq!(tuned["GT4_FRAME_SKIP_FORCE"], "2");
    assert!(!tuned.contains_key("GT4_FRAME_SKIP"), "30 FPS is the fixed pattern alone");
    assert!(!tuned.contains_key("GT4_HW_TEXTURES"));
    assert_eq!(tuned["GT4_HW_POSTFX"], "fxaa");
    assert_eq!(tuned["GT4_CORE2_FSR_SHARPNESS"], "0.2");
    assert_eq!(tuned["GT4_HW_SCALE"], "2", "the session plan: 100% of 1080 is 2x, whatever the GPU");

    let section = launch::coregs20_section(&settings, Some(&display), false, &textures);
    assert!(section.starts_with("renderer=coregs20\n"));
    assert!(section.contains("hw_scale=2\n") && section.contains("hw_effect_flares=1\n") && section.contains("hw_postfx=fxaa\n"));
    assert!(section.contains("hw_effect_road_sheen=1\n"));
    assert!(section.contains("hw_sharpening=0.2\n"));
    assert!(section.contains("hw_frame_skip=2\n"));
    assert!(!section.contains("hw_frame_skip_adaptive"));
    assert!(!section.contains("hw_textures"));
}

#[test]
fn the_one_queue_presentation_path_is_the_games_default_and_only_off_is_passed() {
    let textures = TextureLocation::default_folder(Path::new("T"));
    let mut settings = Settings::default();
    assert!(settings.is_on("present_thread"), "on by default");
    let on = env_map(&launch::coregs20_environment(&settings, None, false, &textures));
    assert!(!on.contains_key("GT4_HW_PRESENT_SHARED_QUEUE"), "on passes nothing: the game decides by its queue count");
    assert!(!launch::coregs20_section(&settings, None, false, &textures).contains("hw_present_shared_queue"));
    settings.set("present_thread", "off").unwrap();
    let off = env_map(&launch::coregs20_environment(&settings, None, false, &textures));
    assert_eq!(off["GT4_HW_PRESENT_SHARED_QUEUE"], "0");
    assert!(launch::coregs20_section(&settings, None, false, &textures).contains("hw_present_shared_queue=0\n"));
    // Only CoreGS 2.0 has it.
    assert!(!env_map(&game_environment(&settings)).contains_key("GT4_HW_PRESENT_SHARED_QUEUE"));
}

#[test]
fn the_performance_hud_is_off_by_default_and_fps_only_or_full_when_chosen() {
    let textures = TextureLocation::default_folder(Path::new("T"));
    let mut settings = Settings::default();
    assert_eq!(settings.get("performance_hud"), "off");
    let off = env_map(&launch::coregs20_environment(&settings, None, false, &textures));
    assert!(!off.contains_key("GT4_HW_HUD"));
    assert!(!launch::coregs20_section(&settings, None, false, &textures).contains("hw_hud"));
    settings.set("performance_hud", "fps").unwrap();
    let fps = env_map(&launch::coregs20_environment(&settings, None, false, &textures));
    assert_eq!(fps["GT4_HW_HUD"], "fps");
    assert!(launch::coregs20_section(&settings, None, false, &textures).contains("hw_hud=fps\n"));
    settings.set("performance_hud", "full").unwrap();
    let full = env_map(&launch::coregs20_environment(&settings, None, false, &textures));
    assert_eq!(full["GT4_HW_HUD"], "1");
    assert!(launch::coregs20_section(&settings, None, false, &textures).contains("hw_hud=1\n"));
    // It is CoreGS 2.0's own: GSdx has its overlay.
    assert!(!env_map(&game_environment(&settings)).contains_key("GT4_HW_HUD"));
    assert!(settings.set("performance_hud", "maybe").is_err());
}

#[test]
fn a_stored_performance_hud_on_from_the_toggle_days_reads_as_full() {
    let textures = TextureLocation::default_folder(Path::new("T"));
    let mut settings = Settings::default();
    settings.set("performance_hud", "on").unwrap();
    assert_eq!(settings.get("performance_hud"), "full");
    assert_eq!(env_map(&launch::coregs20_environment(&settings, None, false, &textures))["GT4_HW_HUD"], "1");
    // Reading a settings file does the same, and an old `off` stays off.
    let (file, warnings) = gt4l_core::config::parse("[settings]
performance_hud = \"on\"
").unwrap();
    assert!(warnings.is_empty(), "{warnings:?}");
    assert_eq!(file.settings.get("performance_hud"), "full");
    let (file, _) = gt4l_core::config::parse("[settings]
performance_hud = \"off\"
").unwrap();
    assert_eq!(file.settings.get("performance_hud"), "off");
}

#[test]
fn setup_tries_coregs20_only_on_a_new_install_with_no_choice_and_no_gsdx() {
    use launch::{setup_renderer, SetupRenderer};
    let settings = Settings::default();
    assert_eq!(setup_renderer(&settings, true, true), SetupRenderer::TryCoregs20, "a new install");
    assert_eq!(setup_renderer(&settings, true, false), SetupRenderer::Gsdx, "GSdx is already there: it stays");
    assert_eq!(setup_renderer(&settings, false, true), SetupRenderer::Gsdx, "a second game of a player on GSdx");
    let mut chose_gsdx = Settings::default();
    chose_gsdx.set_by_user("renderer", "pcsx2").unwrap();
    assert_eq!(setup_renderer(&chose_gsdx, true, true), SetupRenderer::Gsdx, "the player's choice wins");
    let mut chose_coregs = Settings::default();
    chose_coregs.set_by_user("renderer", "coregs20").unwrap();
    assert_eq!(setup_renderer(&chose_coregs, true, true), SetupRenderer::Coregs20, "nothing to install or to try");
    assert_eq!(setup_renderer(&chose_coregs, false, true), SetupRenderer::Coregs20);
}

#[test]
fn gsdx_is_the_default_renderer_and_a_gsdx_session_needs_it_installed() {
    let mut settings = Settings::default();
    assert_eq!(settings.get("renderer"), "pcsx2", "GSdx is the default");
    assert!(settings.set("renderer", "coregs11").is_err(), "CoreGS 1.1 is not a choice (TASK-096)");
    settings.set("renderer", "coregs20").expect("CoreGS 2.0 is a beta choice");
    settings.set("renderer", "pcsx2").unwrap();

    let installed = installed_fixture();
    assert!(launch::choose_renderer(Some(&installed), Path::new("logs")).is_ok());
    let error = launch::choose_renderer(None, Path::new("logs")).unwrap_err();
    assert_eq!(
        error,
        "GSdx could not run the game: GSdx is not installed yet. Check your internet connection and try again. Details are in the log folder: logs."
    );
    assert!(!error.contains("CoreGS"), "no CoreGS 1.1 fallback");
}

#[test]
fn play_offers_coregs20_only_when_gsdx_is_the_renderer_missing_and_the_card_has_vulkan() {
    // Issues #139 and #179: GSdx could not be downloaded (GitHub unreachable), and Play had no way out.
    let gsdx = Settings::default();
    assert_eq!(gsdx.get("renderer"), "pcsx2");
    assert!(launch::offers_coregs20(&gsdx, false, true), "GSdx missing, a card with Vulkan 1.2: offer CoreGS 2.0");
    assert!(!launch::offers_coregs20(&gsdx, false, false), "no Vulkan 1.2: the plain GSdx message stays");
    assert!(!launch::offers_coregs20(&gsdx, true, true), "GSdx is installed: Play just starts");
    assert!(!launch::offers_coregs20(&gsdx, true, false));
    let mut coregs = Settings::default();
    coregs.set_by_user("renderer", "coregs20").unwrap();
    assert!(!launch::offers_coregs20(&coregs, false, true), "CoreGS 2.0 is already the renderer: nothing to offer");
    // With no GSdx and no Vulkan card the refusal is still the old one, word for word.
    assert!(launch::choose_renderer(None, Path::new("logs")).unwrap_err().contains("GSdx is not installed yet"));
}

#[test]
fn pcsx2_environment_carries_the_process_pin_config_and_480p() {
    let paths = pcsx2_paths();
    let environment = env_map(&launch::pcsx2_environment(&Settings::default(), &paths));
    assert_eq!(environment["GT4_RENDERER"], "pcsx2");
    assert_eq!(environment["GT4_GS_PROCESS"], paths.executable.display().to_string());
    assert_eq!(environment["GT4_GS_PROCESS_SHA256"], paths.executable_sha256);
    assert_eq!(environment["GT4_GS_CONFIG"], paths.config.display().to_string());
    assert_eq!(environment["GT4_FORCE_480P"], "1", "480p by default");
    assert_eq!(environment["GT4_PRESENT_MODE"], "fifo");
    assert!(!environment.contains_key("GT4_GS_READBACK"), "readback is written in the INI");
    assert!(!environment.contains_key("PS2_SCALE"), "CoreGS 1.1 variables are not sent to PCSX2 GS");
}

#[test]
fn pcsx2_plan_keeps_the_probe_argument_list_and_scrubs_inherited_names() {
    let paths = pcsx2_paths();
    let inherited = vec!["GT4_GS_PROCESS".to_string(), "PATH".to_string(), "PS2_SCALE".to_string()];
    let plan = launch::plan_pcsx2_launch(&inputs(), &Settings::default(), &paths, None, &inherited);
    assert_eq!(plan.arguments.last().unwrap(), "--window");
    assert_eq!(plan.arguments[0], "C:/work/SCUS_973.28");
    assert_eq!(plan.removed_inherited, vec!["GT4_GS_PROCESS".to_string(), "PS2_SCALE".to_string()]);
}

#[test]
fn session_ini_matches_the_python_writer_for_the_default_settings() {
    // The fixture is the table writer's 3x (scripts/gs_settings.py). TASK-180 test3: the multiplier now comes from the
    // Render scale as a percentage of the screen's height, so the scale that gives 3x on 1920 x 1080 (124%) reproduces
    // the fixture, and the untouched default (100%) is 2x there.
    let paths = pcsx2_paths();
    let mut settings = Settings::default();
    let three_x = gt4l_core::display::percent_for_multiplier(3, 1080);
    settings.set("render_scale", &three_x.to_string()).unwrap();
    let text = launch::gs_config_text(&settings, &paths, None, false).unwrap();
    let bytes = gt4l_core::gs_table::ini_file_bytes(&text);
    let expected = gt4l_core::gs_table::ini_file_bytes(include_str!("fixtures/gs/default-session.ini"));  // LF in the file
    assert_eq!(bytes.as_slice(), expected.as_slice(), "Rust gt4-gs.ini differs from scripts/gs_settings.py");
    let full_hd = display_target(r"\\.\DISPLAY1", 1920, 1080);
    assert_eq!(launch::gs_config_text(&settings, &paths, Some(&full_hd), false).unwrap(), text);
    assert_eq!(
        multiplier_line(&launch::gs_config_text(&Settings::default(), &paths, Some(&full_hd), false).unwrap()),
        "upscale_multiplier = 2",
        "100% is 448 pixels of 1080: 2x"
    );
}

fn display_target(device: &str, width: u32, height: u32) -> DisplayTarget {
    DisplayTarget { device: device.to_string(), output: Size::new(width, height) }
}

fn multiplier_line(text: &str) -> &str {
    text.lines().find(|line| line.starts_with("upscale_multiplier = ")).unwrap()
}

#[test]
fn the_internal_resolution_follows_the_output_size_and_the_render_scale() {
    let paths = pcsx2_paths();
    let mut settings = Settings::default();
    // TASK-180 test3: the percentage is the internal frame's height against the screen's height, on every platform.
    for (width, height, scale, expected) in [
        (1920, 1080, "100", "2"),
        (1920, 1080, "124", "3"),
        (2560, 1080, "100", "2"),
        (2560, 1440, "50", "2"),
        (2560, 1440, "100", "3"),
        (2560, 1440, "200", "6"),
        (3440, 1440, "100", "3"),
        (3440, 1440, "300", "10"),
        (3088, 1440, "187", "6"),
        (3840, 2160, "75", "4"),
        (1280, 720, "100", "2"),
    ] {
        settings.set("render_scale", scale).unwrap();
        let target = display_target(r"\\.\DISPLAY1", width, height);
        let text = launch::gs_config_text(&settings, &paths, Some(&target), false).unwrap();
        assert_eq!(multiplier_line(&text), format!("upscale_multiplier = {expected}"), "{width}x{height} at {scale}%");
    }
    // A stored upscale (an old file edited by hand) never wins: the plan does.
    settings.set("render_scale", "100").unwrap();
    let text = launch::gs_config_text(&settings, &paths, Some(&display_target("x", 2560, 1440)), false).unwrap();
    assert_eq!(multiplier_line(&text), "upscale_multiplier = 3");
}

#[test]
fn the_gpu_no_longer_changes_the_render_scale() {
    // TASK-180 test3: the percentage alone decides (the old issue #89 default gave an integrated GPU 1x at 100%); 1440 x
    // 900 at 100% is 2x everywhere, and 150% (1350 pixels, 3.01x) is 3x.
    let paths = pcsx2_paths();
    let target = display_target(r"\\.\DISPLAY1", 1440, 900);
    let mut settings = Settings::default();
    let integrated = launch::gs_config_text(&settings, &paths, Some(&target), true).unwrap();
    let discrete = launch::gs_config_text(&settings, &paths, Some(&target), false).unwrap();
    assert_eq!(multiplier_line(&integrated), "upscale_multiplier = 2");
    assert_eq!(integrated, discrete, "the same plan on both kinds of GPU");
    settings.set("render_scale", "150").unwrap();
    let text = launch::gs_config_text(&settings, &paths, Some(&target), false).unwrap();
    assert_eq!(multiplier_line(&text), "upscale_multiplier = 3");
}

#[test]
fn the_display_target_becomes_gt4_monitor_and_gt4_output_size() {
    let target = display_target(r"\\.\DISPLAY2", 3440, 1440);
    let plan = plan_launch(&inputs(), &Settings::default(), Some(&target), &[]);
    let environment = env_map(&plan.environment);
    assert_eq!(environment["GT4_MONITOR"], r"\\.\DISPLAY2");
    assert_eq!(environment["GT4_OUTPUT_SIZE"], "3440x1440");
    assert_eq!(environment["GT4_DISPLAY_ASPECT"], "auto");
    let gs_plan = launch::plan_pcsx2_launch(&inputs(), &Settings::default(), &pcsx2_paths(), Some(&target), &[]);
    assert_eq!(env_map(&gs_plan.environment)["GT4_OUTPUT_SIZE"], "3440x1440");
    let sorted = {
        let mut names: Vec<&String> = gs_plan.environment.iter().map(|(name, _)| name).collect();
        let before = names.clone();
        names.sort();
        names == before
    };
    assert!(sorted, "the environment stays sorted by name");
    // No monitor known: no display variables (the game opens on the primary monitor at its desktop size).
    let unknown = plan_launch(&inputs(), &Settings::default(), None, &[]);
    assert!(!env_map(&unknown.environment).contains_key("GT4_MONITOR"));
    assert!(!env_map(&unknown.environment).contains_key("GT4_OUTPUT_SIZE"));
}

#[test]
fn restart_marker_is_detected_in_the_game_log() {
    assert!(launch::restart_requested(&format!("boot\n{}\nmore\n", launch::GS_RESTART_MARKER)));
    assert!(!launch::restart_requested("[gs-ipc] restart requested: renderer pcsx2\n"));
}

fn installed_fixture() -> gt4l_core::gs_release::Installed {
    gt4l_core::gs_release::Installed {
        version: gt4l_core::gs_release::Version(0, 3, 2),
        executable: PathBuf::from("C:/Users/x/AppData/Local/GT4Recomp/gs/0.3.2/gt4-gs.exe"),
        executable_sha256: "f".repeat(64),
    }
}

fn pcsx2_paths() -> launch::Pcsx2Paths {
    launch::Pcsx2Paths {
        executable: PathBuf::from("C:/gs/0.3.2/gt4-gs.exe"),
        executable_sha256: "a".repeat(64),
        config: PathBuf::from("C:/session/gt4-gs.ini"),
        cache: PathBuf::from("C:/gs/cache"),
        data_root: PathBuf::from("C:/gs/data"),
        textures: PathBuf::from("C:/Users/x/Documents/Gran Turismo 4/Textures"),
    }
}

fn inputs() -> LaunchInputs<'static> {
    LaunchInputs {
        game_executable: Path::new("C:/game/gt4.exe"),
        elf: Path::new("C:/work/SCUS_973.28"),
        disc: Path::new("C:/disc/game.iso"),
        card: Path::new("C:/cards/card.ps2"),
        session_dir: Path::new("C:/session"),
    }
}

/// Both window modes start the game window (`--window` is required for any window). Only GT4_FULLSCREEN changes,
/// and the native game reads it at start: 1 starts borderless fullscreen, 0 starts windowed.
#[test]
fn both_window_modes_pass_window_and_send_gt4_fullscreen_to_the_game_and_the_gs_child() {
    let mut fullscreen = Settings::default();
    fullscreen.set("fullscreen", "on").unwrap();
    let mut windowed = Settings::default();
    windowed.set("fullscreen", "off").unwrap();

    for (settings, expected) in [(&fullscreen, "1"), (&windowed, "0")] {
        let plan = plan_launch(&inputs(), settings, None, &[]);
        assert_eq!(plan.arguments.last().unwrap(), "--window", "a window is always started");
        assert!(!plan.arguments.iter().any(|argument| argument == "--fullscreen"));
        assert_eq!(env_map(&plan.environment)["GT4_FULLSCREEN"], expected);

        let gs_plan = launch::plan_pcsx2_launch(&inputs(), settings, &pcsx2_paths(), None, &[]);
        assert_eq!(gs_plan.arguments.last().unwrap(), "--window");
        assert_eq!(env_map(&gs_plan.environment)["GT4_FULLSCREEN"], expected, "the PCSX2 GS path gets the same mode");
    }
}

// ---- TASK-117: the flare limits of the parked guest patch ---------------------------------------------------------

/// The derived-ELF flare limits were parked in rc6: Performance Mode removes the flares in the host (the client answers
/// the visibility table as zeros), so the launcher never sends `GT4_FLARE_LIMITS`, for either game or mode. An inherited
/// value from a parent process is scrubbed.
#[test]
fn the_parked_flare_limits_are_never_sent() {
    let mut performance = Settings::default();
    performance.set_gs("effect_flares", "false").unwrap();
    let accurate = Settings::default();
    for settings in [&performance, &accurate] {
        assert!(!env_map(&plan_launch(&inputs(), settings, None, &[]).environment)
            .contains_key("GT4_FLARE_LIMITS"));
        let spec2 = LaunchInputs { elf: Path::new("C:/work/SCUS_974.36"), ..inputs() };
        assert!(!env_map(&plan_launch(&spec2, settings, None, &[]).environment)
            .contains_key("GT4_FLARE_LIMITS"));
    }
    let inherited = vec!["GT4_FLARE_LIMITS".to_string()];
    let plan = plan_launch(&inputs(), &performance, None, &inherited);
    assert_eq!(plan.removed_inherited, vec!["GT4_FLARE_LIMITS".to_string()]);
    assert!(!env_map(&plan.environment).contains_key("GT4_FLARE_LIMITS"));
}

// ---- TASK-097: the config file of a direct start ---------------------------------------------------------------------

/// The sample the game's fixture (`native/standalone_fixture.cpp`) parses.
const STANDALONE_SAMPLE: &str = include_str!("../../../../native/standalone_samples/game.ini");

#[test]
fn the_game_ini_matches_the_sample_the_game_fixture_reads() {
    let display = display_target(r"\\.\DISPLAY2", 3440, 1440);
    let text = launch::standalone_config_text(&Settings::default(), Some(&display));
    assert_eq!(text.replace("\r\n", "\n"), STANDALONE_SAMPLE.replace("\r\n", "\n"));
}

/// The variable a `game.ini` key stands for (the mapping `native/standalone.cpp` `config_environment` implements).
fn variables_of_game_ini(text: &str) -> std::collections::BTreeMap<String, String> {
    let mut variables = std::collections::BTreeMap::new();
    for line in text.lines().filter(|line| !line.starts_with('#') && !line.is_empty()) {
        let (key, value) = line.split_once('=').expect("key=value");
        match key {
            "version" => {}
            "fullscreen" => {
                variables.insert("GT4_FULLSCREEN".to_string(), value.to_string());
            }
            "present_mode" => {
                variables.insert("GT4_PRESENT_MODE".to_string(), value.to_string());
            }
            "monitor" => {
                variables.insert("GT4_MONITOR".to_string(), value.to_string());
            }
            "output_size" => {
                variables.insert("GT4_OUTPUT_SIZE".to_string(), value.to_string());
            }
            "performance_report" => {
                if value == "1" {
                    variables.insert("GT4_PERF".to_string(), "1".to_string());
                }
            }
            "check_vu_code" => {
                if value != "0" {
                    variables.insert("GT4_VU1_VERIFY".to_string(), value.to_string());
                    variables.insert("GT4_VU0_VERIFY".to_string(), value.to_string());
                }
            }
            "trigger_full_range" => {
                variables.insert("GT4_TRIGGER_FULL_RANGE".to_string(), value.to_string());
            }
            // TASK-181: written beside the other keys only when wheel.toml exists (state.rs `write_standalone_files`).
            "wheel_file" => {
                variables.insert("GT4_WHEEL_FILE".to_string(), value.to_string());
            }
            other => panic!("unknown game.ini key {other}"),
        }
    }
    variables
}

#[test]
fn every_game_ini_key_stands_for_the_variable_the_launcher_sets_at_play() {
    let variants: Vec<Vec<(&str, &str)>> = vec![
        vec![],
        vec![("fullscreen", "off")],
        vec![("vsync", "fast")],
        vec![("vsync", "off")],
        vec![("performance_report", "on"), ("check_vu_code", "100")],
        vec![("check_vu_code", "1000"), ("fullscreen", "off"), ("vsync", "on")],
        vec![("trigger_full_range", "on")],
    ];
    let display = display_target(r"\\.\DISPLAY1", 2560, 1440);
    for changes in variants {
        let mut settings = Settings::default();
        for (id, value) in &changes {
            settings.set(id, value).unwrap_or_else(|error| panic!("{id}={value}: {error}"));
        }
        for shown in [Some(&display), None] {
            let text = launch::standalone_config_text(&settings, shown);
            let from_ini = variables_of_game_ini(&text);
            let mut at_play = env_map(&game_environment(&settings));
            at_play.extend(env_map(&launch::display_environment(shown)));
            // Every variable the file stands for has exactly the value Play sets.
            for (name, value) in &from_ini {
                assert_eq!(at_play.get(name), Some(value), "{name} for {changes:?}, display {}", shown.is_some());
            }
            // And the file leaves out nothing that Play sets and the game does not default itself.
            for name in ["GT4_FULLSCREEN", "GT4_PRESENT_MODE", "GT4_GS_SYNC", "GT4_PERF", "GT4_VU1_VERIFY", "GT4_VU0_VERIFY", "GT4_MONITOR", "GT4_OUTPUT_SIZE"] {
                assert_eq!(from_ini.get(name), at_play.get(name), "{name} for {changes:?}, display {}", shown.is_some());
            }
        }
    }
}

#[test]
fn the_game_defaults_are_the_values_the_launcher_sets_for_default_settings() {
    // native/standalone.cpp default_environment(): the six variables the game applies when nothing else says otherwise.
    let defaults = [
        ("GT4_RENDERER", "pcsx2"),
        ("PS2_GS", "gpu"),
        ("GT4_FORCE_480P", "1"),
        ("GT4_FORCE_WIDESCREEN", "1"),
        ("GT4_DISPLAY_ASPECT", "auto"),
        ("GT4_FULLSCREEN", "1"),
        ("GT4_PRESENT_MODE", "fifo"),
    ];
    let at_play = env_map(&game_environment(&Settings::default()));
    for (name, value) in defaults {
        assert_eq!(at_play[name], value, "{name}");
    }
}

#[test]
fn the_chosen_controller_reaches_the_game_as_gt4_pad_and_in_game_ini() {
    // TASK-148: Automatic sends nothing; a chosen controller is GT4_PAD (commands.rs) and `pad=` for a direct start.
    let mut settings = Settings::default();
    assert_eq!(settings.get("controller"), "auto");
    assert_eq!(launch::controller_environment(&settings), None);
    assert!(!launch::standalone_config_text(&settings, None).contains("pad="));
    settings.set("controller", "050000004C050000E60C000000810000/a0:ab:51:12:34:56").unwrap();
    assert_eq!(settings.get("controller"), "050000004c050000e60c000000810000/a0:ab:51:12:34:56", "the GUID is stored lowercase");
    assert_eq!(launch::controller_environment(&settings).as_deref(), Some("050000004c050000e60c000000810000/a0:ab:51:12:34:56"));
    assert!(launch::standalone_config_text(&settings, None).contains("pad=050000004c050000e60c000000810000/a0:ab:51:12:34:56\n"));
    assert!(settings.set("controller", "not a controller").is_err());
    settings.set("controller", "auto").unwrap();
    assert_eq!(launch::controller_environment(&settings), None);
}

/// TASK-157: a folder the player chose reaches CoreGS 2.0 (`GT4_HW_TEXTURES`, `hw_textures`) and GSdx
/// (`[Folders] Textures`), and the shader cache never goes beside it.
#[test]
fn a_chosen_textures_folder_reaches_the_renderers_and_the_shader_cache_stays_in_the_data_folder() {
    let data = Path::new(r"D:\GT4Recomp\data");
    let chosen = TextureLocation::custom_folder(Path::new(r"E:\Emulators\PCSX2\textures"), data);
    let mut settings = Settings::default();
    settings.set("renderer", "coregs20").unwrap();

    let environment = env_map(&launch::coregs20_environment(&settings, None, false, &chosen));
    assert_eq!(environment["GT4_HW_TEXTURES"], r"E:\Emulators\PCSX2\textures");
    let cache = PathBuf::from(&environment["GT4_HW_SHADER_CACHE"]);
    assert_eq!(cache, data.join("shader-cache"), "the cache comes from the data folder");
    assert!(!cache.starts_with(r"E:\Emulators"), "nothing is written beside the player's folder");
    assert!(launch::coregs20_section(&settings, None, false, &chosen).contains(&format!("hw_textures={}\n", chosen.folder.display())));

    // The default folder keeps its cache where earlier versions put it, so existing caches stay valid.
    let default = TextureLocation::default_folder(Path::new(r"D:\GT4Recomp\data\Textures"));
    let environment = env_map(&launch::coregs20_environment(&settings, None, false, &default));
    assert_eq!(PathBuf::from(&environment["GT4_HW_SHADER_CACHE"]), Path::new(r"D:\GT4Recomp\data").join("shader-cache"));

    // GSdx: the same folder in `[Folders] Textures`.
    let paths = launch::Pcsx2Paths { textures: chosen.folder.clone(), ..pcsx2_paths() };
    let text = launch::gs_config_text(&Settings::default(), &paths, None, false).unwrap();
    assert!(text.contains(r"E:\Emulators\PCSX2\textures"), "{text}");
}

#[test]
fn the_wheel_file_line_of_game_ini_stands_for_gt4_wheel_file() {
    let path = std::path::Path::new(r"C:\Users\Player\AppData\Roaming\GT4Recomp\wheel.toml");
    let line = launch::wheel_file_line(path);
    assert_eq!(line, format!("{}={}\n", launch::STANDALONE_WHEEL_KEY, path.display()));
    let variables = variables_of_game_ini(&line);
    assert_eq!(variables.get("GT4_WHEEL_FILE").map(String::as_str), Some(r"C:\Users\Player\AppData\Roaming\GT4Recomp\wheel.toml"));
    assert!(!launch::standalone_config_text(&Settings::default(), None).contains("wheel_file"), "only written when the file exists");
}

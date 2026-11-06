//! Settings table and config file: validation, defaults and round trips.

use gt4l_core::config::{self, ConfigFile};
use gt4l_core::settings::{self, Settings, SettingError, SETTINGS};

#[test]
fn every_definition_id_is_unique_and_has_a_valid_default() {
    let mut seen = std::collections::BTreeSet::new();
    for definition in SETTINGS {
        assert!(seen.insert(definition.id), "duplicate id {}", definition.id);
        assert!(
            settings::validate(definition, definition.default).is_ok(),
            "default of {} is not allowed",
            definition.id
        );
    }
}

#[test]
fn renderer_defaults_to_gsdx_and_coregs20_can_be_selected() {
    let mut settings = Settings::default();
    assert_eq!(settings.get("renderer"), "pcsx2");
    settings.set("renderer", "coregs20").expect("CoreGS 2.0 is a beta choice");
    settings.set("renderer", "pcsx2").unwrap();
    assert_eq!(
        settings.set("renderer", "coregs11"),
        Err(SettingError::InvalidValue { id: "renderer".into(), value: "coregs11".into() }),
        "CoreGS 1.1 is no longer a choice (TASK-096)"
    );
    assert_eq!(settings.get("renderer"), "pcsx2", "a refused value leaves the state unchanged");
}

#[test]
fn a_stored_coregs11_renderer_loads_as_gsdx() {
    let text = "schema_version = 6

[settings]
renderer = \"coregs11\"
vsync = \"fast\"
";
    let (config, _warnings) = config::parse(text).expect("parse");
    assert_eq!(config.settings.get("renderer"), "pcsx2", "schema 7 maps CoreGS 1.1 to GSdx");
    assert_eq!(config.settings.get("vsync"), "fast");
    let written = config::render(&config);
    assert!(written.contains("renderer = \"pcsx2\""), "{written}");
}

#[test]
fn unknown_ids_and_bad_toggle_values_are_refused() {
    let mut settings = Settings::default();
    assert!(matches!(settings.set("gamma", "2"), Err(SettingError::UnknownSetting(_))));
    assert!(settings.set("fullscreen", "yes").is_err());
    assert!(settings.set("super_sampling", "3").is_err());
    assert!(settings.set("high_resolution_output", "on").is_err());
}

#[test]
fn retired_and_forced_values_of_an_old_file_are_ignored_and_dropped_at_the_next_write() {
    // Owner, 2026-10-21: 480p and 16:9 are fixed, and the PCSX2 OSD rows are forced off. A file written by an earlier
    // 1.0.0 launcher may still hold the old values; they are ignored on load (no schema step) and gone after a save.
    let text = concat!(
        "schema_version = 3\n\n",
        "[settings]\ninput_resolution = \"1080i\"\nwidescreen = \"off\"\nvsync = \"fast\"\n\n",
        "[choices]\ninput_resolution = true\nwidescreen = true\n\n",
        "[gs]\naspect = \"4:3\"\nosd_fps = \"true\"\nosd_gpu = \"true\"\nosd_resolution = \"true\"\nfxaa = \"true\"\n",
    );
    let (config, warnings) = config::parse(text).expect("parse");
    assert_eq!(config.settings.get("vsync"), "fast", "other values are kept");
    assert_eq!(config.settings.gs_value("fxaa"), "true", "other GS values are kept");
    assert!(warnings.iter().any(|w| w.contains("'input_resolution' is no longer used")), "{warnings:?}");
    assert!(warnings.iter().any(|w| w.contains("'widescreen' is no longer used")), "{warnings:?}");
    for id in ["aspect", "osd_fps", "osd_gpu", "osd_resolution"] {
        assert!(!config.settings.gs_overrides().contains_key(id), "{id} is not taken from the file");
    }
    assert_eq!(config.settings.gs_value("aspect"), "16:9");
    assert_eq!(config.settings.gs_value("osd_fps"), "false");
    assert!(config.settings.chosen_ids().is_empty(), "retired ids are not kept as choices");
    let written = config::render(&config);
    assert!(written.contains("resolution = \"desktop\""), "the TASK-080 display rows replace them: {written}");
    for retired in ["input_resolution", "widescreen", "aspect =", "osd_"] {
        assert!(!written.contains(retired), "{retired} is dropped at the next write:\n{written}");
    }
}

#[test]
fn config_round_trip_keeps_values_and_paths() {
    let mut config = ConfigFile::default();
    config.settings.set("vsync", "fast").unwrap();
    config.settings.set_gs("fxaa", "true").unwrap();
    // TASK-080 addendum: the monitor identifier holds backslashes; it must survive the TOML round trip.
    config.settings.set("monitor", r"\\.\DISPLAY2@3440x1440").unwrap();
    config.settings.set("resolution", "2560x1080").unwrap();
    config.settings.set("render_scale", "175").unwrap();
    config.settings.set_gs("effect_flares", "false").unwrap();
    config.settings.set_gs("hw_download_mode", "unsynchronized").unwrap();
    config.disc_path = Some(r#"D:\Games\Gran Turismo 4 "quoted" (USA).iso"#.into());
    config.textures_path = Some(r"E:\Emulators\PCSX2\textures".into());
    config.dev_game_dir = Some(r"C:\Antigravity\build\game".into());
    let text = config::render(&config);
    let (parsed, warnings) = config::parse(&text).unwrap();
    assert!(warnings.is_empty(), "{warnings:?}");
    assert_eq!(parsed, config);
}

#[test]
fn textures_path_is_optional_and_keeps_windows_backslashes() {
    // No key, no choice (no schema bump).
    let (absent, warnings) = config::parse("schema_version = 8\n[install]\ndisc_path = \"D:/gt4.iso\"\n").unwrap();
    assert!(warnings.is_empty(), "{warnings:?}");
    assert_eq!(absent.textures_path, None);
    assert!(!config::render(&absent).contains("textures_path"));

    let mut config = ConfigFile::default();
    config.textures_path = Some(r"E:\Emulators\PCSX2\textures".into());
    let text = config::render(&config);
    assert!(text.contains("[install]\ntextures_path = \"E:\\\\Emulators\\\\PCSX2\\\\textures\"\n"), "{text}");
    let (parsed, warnings) = config::parse(&text).unwrap();
    assert!(warnings.is_empty(), "{warnings:?}");
    assert_eq!(parsed.textures_path.as_deref(), Some(std::path::Path::new(r"E:\Emulators\PCSX2\textures")));
    assert_eq!(parsed, config);
}

#[test]
fn invalid_values_and_unknown_keys_fall_back_with_warnings() {
    let text = "schema_version = 1\n[settings]\nvsync = \"maybe\"\nflux = \"on\"\nfullscreen = \"off\"\n";
    let (config, warnings) = config::parse(text).unwrap();
    assert_eq!(config.settings.get("vsync"), "on");
    assert_eq!(config.settings.get("fullscreen"), "off");
    assert_eq!(warnings.len(), 2, "{warnings:?}");
}

#[test]
fn newer_schema_is_refused_and_syntax_errors_are_reported() {
    // TASK-180 test5: the current schema is 10 (the effect toggles), so 11 is a newer file.
    assert!(matches!(config::parse("schema_version = 11\n"), Err(config::ConfigError::NewerSchema(11))));
    assert!(matches!(config::parse("[settings\n"), Err(config::ConfigError::Syntax(_))));
}

#[test]
fn load_of_a_damaged_file_moves_it_aside() {
    let directory = std::env::temp_dir().join(format!("gt4l-cfg-{}", std::process::id()));
    std::fs::create_dir_all(&directory).unwrap();
    let path = directory.join("settings.toml");
    std::fs::write(&path, "[settings\n").unwrap();
    let (config, warnings) = config::load(&path);
    assert_eq!(config, ConfigFile::default());
    assert_eq!(warnings.len(), 1);
    assert!(directory.join("settings.toml.bad").exists());
    assert!(!path.exists());
    std::fs::remove_dir_all(&directory).unwrap();
}

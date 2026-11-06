//! File-level migration tests for `settings.toml` and `version.json` (temporary folders, no network).

use std::fs;
use std::path::{Path, PathBuf};

use gt4l_core::config::{self, ConfigFile};
use gt4l_core::versions::{self, VersionFile};
use gt4l_core::Version;

fn scratch(name: &str) -> PathBuf {
    let directory = std::env::temp_dir().join(format!("gt4l-migration-test-{}-{name}", std::process::id()));
    let _ = fs::remove_dir_all(&directory);
    fs::create_dir_all(&directory).unwrap();
    directory
}

fn backups(directory: &Path, prefix: &str) -> Vec<PathBuf> {
    fs::read_dir(directory)
        .unwrap()
        .flatten()
        .map(|entry| entry.path())
        .filter(|path| path.file_name().unwrap().to_string_lossy().starts_with(prefix))
        .collect()
}

#[test]
fn an_unversioned_dev_build_file_is_backed_up_and_migrated_to_schema_one() {
    let directory = scratch("v0");
    let path = directory.join("settings.toml");
    let original = "[settings]\nvsync = \"fast\"\nfullscreen = \"off\"\n\n[install]\ndisc_path = \"D:/gt4.iso\"\n";
    fs::write(&path, original).unwrap();

    let (loaded, notes) = config::load(&path);
    assert_eq!(loaded.settings.get("vsync"), "fast", "the values survive the migration");
    assert_eq!(loaded.disc_path.as_deref(), Some(Path::new("D:/gt4.iso")));
    assert!(notes.iter().any(|note| note.contains("migrated from schema 0 to 10")), "{notes:?}");

    let copies = backups(&directory, "settings.toml.bak-schema0-");
    assert_eq!(copies.len(), 1, "exactly one backup");
    assert_eq!(fs::read_to_string(&copies[0]).unwrap(), original, "the backup is the untouched original");

    let rewritten = fs::read_to_string(&path).unwrap();
    assert!(rewritten.contains("schema_version = 10"), "{rewritten}");
    assert!(!rewritten.contains("widescreen"), "{rewritten}");
    assert!(!rewritten.contains("aspect_"), "{rewritten}");
    assert!(rewritten.contains("render_scale = \"100\""), "{rewritten}");
    assert!(!rewritten.contains("\r\r"));
}

#[test]
fn a_migrated_file_is_not_migrated_or_backed_up_again() {
    let directory = scratch("second-load");
    let path = directory.join("settings.toml");
    fs::write(&path, "[settings]\nvsync = \"off\"\n").unwrap();
    let _ = config::load(&path);
    let _ = config::load(&path);
    assert_eq!(backups(&directory, "settings.toml.bak-schema0-").len(), 1, "one backup for one migration");
}

#[test]
fn a_newer_schema_is_used_as_defaults_and_the_file_is_not_touched() {
    let directory = scratch("newer");
    let path = directory.join("settings.toml");
    // TASK-180 test5: the current schema is 10 (the effect toggles), so a newer file is 11.
    let newer = "schema_version = 11\n[settings]\nvsync = \"fast\"\n";
    fs::write(&path, newer).unwrap();
    let (loaded, notes) = config::load(&path);
    assert_eq!(loaded, ConfigFile::default());
    assert!(notes.iter().any(|note| note.contains("schema 11")), "{notes:?}");
    assert_eq!(fs::read_to_string(&path).unwrap(), newer, "a newer file is never rewritten");
    assert!(backups(&directory, "settings.toml.bak").is_empty());
}

#[test]
fn a_corrupt_file_is_kept_as_a_copy_and_defaults_are_used() {
    let directory = scratch("corrupt");
    let path = directory.join("settings.toml");
    fs::write(&path, "[settings\nvsync = ").unwrap();
    let (loaded, notes) = config::load(&path);
    assert_eq!(loaded, ConfigFile::default());
    assert!(!notes.is_empty());
    assert!(directory.join("settings.toml.bad").exists(), "the damaged file is kept");
}

#[test]
fn skipped_update_versions_round_trip_through_settings() {
    let directory = scratch("skip");
    let path = directory.join("settings.toml");
    let mut config = ConfigFile::default();
    config.skipped_renderer = Some(Version(1, 0, 2));
    config.skipped_launcher = Some(Version(1, 0, 1).into());
    config::save(&path, &config).unwrap();
    let (loaded, _) = config::load(&path);
    assert_eq!(loaded.skipped_renderer, Some(Version(1, 0, 2)));
    assert_eq!(loaded.skipped_launcher, Some(Version(1, 0, 1).into()));
}

#[test]
fn a_damaged_version_file_is_refused_so_the_caller_falls_back_to_the_program_version() {
    let directory = scratch("version-damaged");
    let path = directory.join("version.json");
    fs::write(&path, "{ this is not json").unwrap();
    assert!(versions::read(&path).is_err());
    fs::write(&path, r#"{"schema": 4, "gt4recomp": "9.0.0", "settings_schema": 1}"#).unwrap();
    assert!(versions::read(&path).is_err(), "a newer version file is not used");
    assert_eq!(fs::read_to_string(&path).unwrap(), r#"{"schema": 4, "gt4recomp": "9.0.0", "settings_schema": 1}"#);
    let file: Option<VersionFile> = versions::read(&directory.join("missing.json")).unwrap();
    assert!(file.is_none());
}

#[test]
fn the_fullscreen_default_is_on_for_new_installs() {
    let settings = gt4l_core::settings::Settings::default();
    assert_eq!(settings.get("fullscreen"), "on");
    assert!(!settings.was_chosen("fullscreen"));
}

#[test]
fn schema_one_fullscreen_off_that_was_never_chosen_becomes_on_and_keeps_a_backup() {
    let directory = scratch("fullscreen-default");
    let path = directory.join("settings.toml");
    let original = "schema_version = 1

[settings]
fullscreen = \"off\"
vsync = \"fast\"
";
    fs::write(&path, original).unwrap();

    let (loaded, notes) = config::load(&path);
    assert_eq!(loaded.settings.get("fullscreen"), "on", "the old windowed default is replaced");
    assert_eq!(loaded.settings.get("vsync"), "fast", "other values survive");
    assert!(notes.iter().any(|note| note.contains("migrated from schema 1 to 10")), "{notes:?}");

    let copies = backups(&directory, "settings.toml.bak-schema1-");
    assert_eq!(copies.len(), 1, "exactly one backup of the schema 1 file");
    assert_eq!(fs::read_to_string(&copies[0]).unwrap(), original, "the backup is the untouched original");
    assert!(fs::read_to_string(&path).unwrap().contains("fullscreen = \"on\""));
}

#[test]
fn a_fullscreen_value_the_user_chose_is_kept_through_the_migration() {
    let directory = scratch("fullscreen-chosen");
    let path = directory.join("settings.toml");
    fs::write(&path, "schema_version = 1

[settings]
fullscreen = \"off\"

[choices]
fullscreen = true
").unwrap();

    let (loaded, _) = config::load(&path);
    assert_eq!(loaded.settings.get("fullscreen"), "off", "a deliberate windowed choice stays");
    assert!(loaded.settings.was_chosen("fullscreen"));
}

#[test]
fn a_choice_made_on_the_settings_page_is_saved_and_read_back() {
    let directory = scratch("fullscreen-save");
    let path = directory.join("settings.toml");
    let mut config_file = ConfigFile::default();
    config_file.settings.set_by_user("fullscreen", "off").unwrap();
    config::save(&path, &config_file).unwrap();

    let text = fs::read_to_string(&path).unwrap();
    assert!(text.contains("[choices]
fullscreen = true"), "{text}");
    let (loaded, _) = config::load(&path);
    assert_eq!(loaded.settings.get("fullscreen"), "off");
    assert!(loaded.settings.was_chosen("fullscreen"));
}

#[test]
fn a_release_1_0_0_schema_3_file_is_backed_up_and_migrated_to_schema_five() {
    // The settings file of the 1.0.0 launcher, with the keys it retired by a load-time rule still stored.
    let directory = scratch("v3-release-100");
    let path = directory.join("settings.toml");
    let original = concat!(
        "schema_version = 3\n\n",
        "[settings]\nrenderer = \"pcsx2\"\ninput_resolution = \"480p\"\nwidescreen = \"on\"\nvsync = \"fast\"\n\n",
        "[choices]\nwidescreen = true\nvsync = true\n\n",
        "[gs]\naspect = \"16:9\"\nfxaa = \"true\"\n\n",
        "[install]\ndisc_path = \"D:/gt4.iso\"\n",
    );
    fs::write(&path, original).unwrap();

    let (loaded, notes) = config::load(&path);
    assert!(notes.iter().any(|note| note.contains("migrated from schema 3 to 10")), "{notes:?}");
    assert!(!notes.iter().any(|note| note.contains("no longer used")), "the steps already dropped them: {notes:?}");
    assert_eq!(loaded.settings.get("vsync"), "fast");
    assert_eq!(loaded.settings.get("monitor"), "primary");
    assert_eq!(loaded.settings.get("resolution"), "desktop");
    assert_eq!(loaded.settings.get("render_scale"), "100");
    assert_eq!(loaded.settings.gs_value("fxaa"), "true");
    assert_eq!(loaded.settings.gs_value("aspect"), "16:9", "the GS settings-file aspect stays forced");
    assert_eq!(loaded.disc_path.as_deref(), Some(Path::new("D:/gt4.iso")));

    let copies = backups(&directory, "settings.toml.bak-schema3-");
    assert_eq!(copies.len(), 1, "exactly one backup");
    assert_eq!(fs::read_to_string(&copies[0]).unwrap(), original);
    let rewritten = fs::read_to_string(&path).unwrap();
    assert!(rewritten.contains("schema_version = 10"), "{rewritten}");
    for retired in ["input_resolution", "widescreen", "aspect = ", "aspect_"] {
        assert!(!rewritten.contains(retired), "{retired} in {rewritten}");
    }
}

#[test]
fn an_ultrawide_test_schema_4_file_is_backed_up_and_migrated_to_schema_five() {
    // The owner's real file from the TASK-080 test builds: a custom ratio and a chosen internal resolution.
    let directory = scratch("v4-ultrawide-test");
    let path = directory.join("settings.toml");
    let original = concat!(
        "schema_version = 4\n\n",
        "[settings]\nrenderer = \"pcsx2\"\naspect_ratio = \"custom\"\naspect_custom = \"244\"\nvsync = \"fast\"\n\n",
        "[choices]\naspect_ratio = true\naspect_custom = true\nvsync = true\n\n",
        "[gs]\nupscale = \"4K\"\nfxaa = \"true\"\n\n",
        "[install]\ndisc_path = \"D:/gt4.iso\"\n",
    );
    fs::write(&path, original).unwrap();

    let (loaded, notes) = config::load(&path);
    assert!(notes.iter().any(|note| note.contains("migrated from schema 4 to 10")), "{notes:?}");
    assert!(!notes.iter().any(|note| note.contains("no longer used") || note.contains("set by the launcher")), "{notes:?}");
    assert_eq!(loaded.settings.get("vsync"), "fast");
    assert!(loaded.settings.was_chosen("vsync"));
    assert_eq!(loaded.settings.get("render_scale"), "100");
    assert_eq!(loaded.settings.gs_value("fxaa"), "true");
    assert!(!loaded.settings.gs_overrides().contains_key("upscale"), "the old internal resolution is gone");

    let copies = backups(&directory, "settings.toml.bak-schema4-");
    assert_eq!(copies.len(), 1, "exactly one backup");
    assert_eq!(fs::read_to_string(&copies[0]).unwrap(), original);
    let rewritten = fs::read_to_string(&path).unwrap();
    assert!(rewritten.contains("schema_version = 10"), "{rewritten}");
    for gone in ["aspect_", "upscale"] {
        assert!(!rewritten.contains(gone), "{gone} in {rewritten}");
    }
    for added in ["monitor = \"primary\"", "resolution = \"desktop\"", "render_scale = \"100\""] {
        assert!(rewritten.contains(added), "{added} in {rewritten}");
    }
}

#[test]
fn a_schema_five_file_keeps_its_values_and_gets_the_texture_defaults_with_a_backup() {
    // TASK-088 (gt4-gs 1.2.0): schema 6 adds no value. The texture rows are gs-table rows with table defaults: an
    // absent key means Load custom textures on and Precache off, and a chosen value is kept.
    let directory = scratch("v5-textures");
    let path = directory.join("settings.toml");
    let original = concat!(
        "schema_version = 5\n\n",
        "[settings]\nvsync = \"fast\"\nrender_scale = \"100\"\n\n",
        "[gs]\nfxaa = \"true\"\ntexture_load = \"false\"\n\n",
        "[install]\ndisc_path = \"D:/gt4.iso\"\n",
    );
    fs::write(&path, original).unwrap();

    let (loaded, notes) = config::load(&path);
    assert!(notes.iter().any(|note| note.contains("migrated from schema 5 to 10")), "{notes:?}");
    assert_eq!(loaded.settings.get("vsync"), "fast");
    assert_eq!(loaded.settings.gs_value("fxaa"), "true");
    assert!(!loaded.settings.gs_overrides().contains_key("texture_precache"), "absent key: table default");

    let copies = backups(&directory, "settings.toml.bak-schema5-");
    assert_eq!(copies.len(), 1, "exactly one backup");
    assert_eq!(fs::read_to_string(&copies[0]).unwrap(), original);
    let rewritten = fs::read_to_string(&path).unwrap();
    assert!(rewritten.contains("schema_version = 10"), "{rewritten}");
}

#[test]
fn a_schema_five_resolution_equal_to_the_monitor_desktop_becomes_desktop() {
    // TASK-088 (schema 6): the Monitor identifier records the desktop size when it was chosen. A Resolution equal to
    // it follows the desktop from now on; a different size is a deliberate choice and stays.
    let directory = scratch("v5-resolution-desktop");
    let path = directory.join("settings.toml");
    let original = concat!(
        "schema_version = 5\n\n",
        "[settings]\nmonitor = '\\\\.\\DISPLAY2@3440x1440'\nresolution = \"3440x1440\"\n\n",
        "[install]\ndisc_path = \"D:/gt4.iso\"\n",
    );
    fs::write(&path, original).unwrap();
    let (loaded, notes) = config::load(&path);
    assert!(notes.iter().any(|note| note.contains("migrated from schema 5 to 10")), "{notes:?}");
    assert_eq!(loaded.settings.get("resolution"), "desktop");
    assert_eq!(loaded.settings.get("monitor"), r"\\.\DISPLAY2@3440x1440", "the identifier is kept");
    let copies = backups(&directory, "settings.toml.bak-schema5-");
    assert_eq!(copies.len(), 1, "exactly one backup");
    assert_eq!(fs::read_to_string(&copies[0]).unwrap(), original);
}

#[test]
fn a_schema_five_resolution_that_differs_or_has_no_recorded_desktop_is_kept() {
    let directory = scratch("v5-resolution-kept");
    let path = directory.join("settings.toml");
    let original = concat!(
        "schema_version = 5\n\n",
        "[settings]\nmonitor = '\\\\.\\DISPLAY2@5120x2160'\nresolution = \"3440x1440\"\n",
    );
    fs::write(&path, original).unwrap();
    let (loaded, _) = config::load(&path);
    assert_eq!(loaded.settings.get("resolution"), "3440x1440", "a deliberate size of another mode");

    let primary = directory.join("primary.toml");
    fs::write(&primary, "schema_version = 5\n\n[settings]\nmonitor = \"primary\"\nresolution = \"3440x1440\"\n").unwrap();
    let (loaded, _) = config::load(&primary);
    assert_eq!(loaded.settings.get("resolution"), "3440x1440", "no desktop size is recorded for primary");
}

#[test]
fn a_skipped_candidate_and_the_chosen_channel_round_trip() {
    use gt4l_core::ReleaseVersion;
    let mut config = ConfigFile::default();
    config.skipped_launcher = ReleaseVersion::parse("1.3.0-rc5");
    config.settings.set_by_user("update_channel", "beta").unwrap();
    let (loaded, _) = gt4l_core::config::parse(&gt4l_core::config::render(&config)).unwrap();
    assert_eq!(loaded.skipped_launcher, ReleaseVersion::parse("1.3.0-rc5"));
    assert_eq!(loaded.settings.get("update_channel"), "beta");
    assert!(loaded.settings.was_chosen("update_channel"));
}

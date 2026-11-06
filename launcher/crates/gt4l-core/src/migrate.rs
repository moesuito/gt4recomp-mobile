//! Schema migrations of the launcher-owned files: settings, controls and version.json (see
//! `docs/launcher/MIGRATIONS.md`, the rule that every change to a persisted format adds a migration and a test).
//!
//! A [`Chain`] holds the ordered steps `v0 -> v1 -> ... -> current` of one file kind. [`plan`] decides what to do
//! with a document, [`migrate`] runs the steps on it, and [`backup_name`] names the copy kept before a file is
//! rewritten. The functions are pure; the callers do the file reads and writes.
//!
//! Rules (owner, 2026-10-20):
//! - a file without a schema number is schema 0 and is migrated to the current schema;
//! - a file with a newer schema is never touched: the launcher uses defaults in memory and warns;
//! - a file that cannot be read is kept as a backup and replaced by defaults, with a notice;
//! - memory card images are never migrated or rewritten by the launcher.

use serde_json::Value;

/// One migration: it changes a document of schema `from` into schema `from + 1`. The chain sets the schema key
/// itself, so a step only changes the content.
#[derive(Clone, Copy)]
pub struct Step {
    pub from: u32,
    pub apply: fn(&mut Value) -> Result<(), String>,
}

/// The migrations of one file kind.
#[derive(Clone, Copy)]
pub struct Chain {
    /// File name, for the log and the backup name.
    pub file: &'static str,
    /// The key that holds the schema number (`schema_version` for settings, `schema` for the others).
    pub key: &'static str,
    /// The oldest schema the chain can read. Settings start at 0 (no schema number); the others at 1.
    pub first: u32,
    /// The schema this launcher writes.
    pub current: u32,
    /// Steps in order: `steps[i].from == first + i`, and `first + steps.len() == current`.
    pub steps: &'static [Step],
}

/// What to do with a document, decided before anything is changed.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Verdict {
    /// Already at the current schema.
    Current,
    /// Migrate from this schema to the current one (0 when the file has no schema number).
    Migrate { from: u32 },
    /// Written by a newer launcher: leave the file alone.
    Newer { found: u32 },
    /// The schema key holds something that is not a whole number.
    Corrupt(String),
}

/// Reads the schema number of a document: the chain's key, or 0 when it is absent.
pub fn schema_of(chain: &Chain, document: &Value) -> Result<u32, String> {
    match document.get(chain.key) {
        None => Ok(0),
        Some(value) => value
            .as_u64()
            .and_then(|number| u32::try_from(number).ok())
            .ok_or_else(|| format!("{}: {} is not a schema number", chain.file, chain.key)),
    }
}

/// Decides what happens to a document of this chain.
pub fn plan(chain: &Chain, document: &Value) -> Verdict {
    match schema_of(chain, document) {
        Err(message) => Verdict::Corrupt(message),
        Ok(found) if found > chain.current => Verdict::Newer { found },
        Ok(found) if found == chain.current => Verdict::Current,
        Ok(found) if found < chain.first => {
            Verdict::Corrupt(format!("{}: schema {found} is below the first schema {}", chain.file, chain.first))
        }
        Ok(found) => Verdict::Migrate { from: found },
    }
}

/// Checks that the steps of a chain are in order and complete. Tested for every chain the launcher has.
pub fn check_chain(chain: &Chain) -> Result<(), String> {
    if chain.first + chain.steps.len() as u32 != chain.current {
        return Err(format!("{}: {} steps from schema {} do not reach {}", chain.file, chain.steps.len(), chain.first, chain.current));
    }
    for (index, step) in chain.steps.iter().enumerate() {
        if step.from != chain.first + index as u32 {
            return Err(format!("{}: step {index} starts at schema {}", chain.file, step.from));
        }
    }
    Ok(())
}

/// Runs the steps from the document's schema to the current one. Returns the schema it started from.
/// A newer or corrupt document is an error here: [`plan`] must be checked first.
pub fn migrate(chain: &Chain, document: &mut Value) -> Result<u32, String> {
    let from = match plan(chain, document) {
        Verdict::Current => return Ok(chain.current),
        Verdict::Migrate { from } => from,
        Verdict::Newer { found } => return Err(format!("{} is schema {found}; not migrated", chain.file)),
        Verdict::Corrupt(message) => return Err(message),
    };
    for version in from..chain.current {
        let step = chain
            .steps
            .get((version - chain.first) as usize)
            .ok_or_else(|| format!("{}: no migration from schema {version}", chain.file))?;
        (step.apply)(document).map_err(|message| format!("{} migration {version} -> {}: {message}", chain.file, version + 1))?;
    }
    let object = document.as_object_mut().ok_or_else(|| format!("{}: the document is not a table", chain.file))?;
    object.insert(chain.key.to_string(), Value::from(chain.current));
    Ok(from)
}

/// The name of the copy kept before a migration: `<file>.bak-schema<N>-<unix seconds>`.
pub fn backup_name(file: &str, schema: u32, unix_seconds: u64) -> String {
    format!("{file}.bak-schema{schema}-{unix_seconds}")
}

/// Settings file (`settings.toml`), schema key `schema_version`. Schema 1 is the first versioned file.
/// Schema 0 (no `schema_version`) came from the earlier dev builds; schema 1 only adds the number.
pub const SETTINGS: Chain = Chain {
    file: "settings.toml",
    key: "schema_version",
    first: 0,
    current: 10,
    steps: &[
        Step { from: 0, apply: settings_0_to_1 },
        Step { from: 1, apply: settings_1_to_2 },
        Step { from: 2, apply: settings_2_to_3 },
        Step { from: 3, apply: settings_3_to_4 },
        Step { from: 4, apply: settings_4_to_5 },
        Step { from: 5, apply: settings_5_to_6 },
        Step { from: 6, apply: settings_6_to_7 },
        Step { from: 7, apply: settings_7_to_8 },
        Step { from: 8, apply: settings_8_to_9 },
        Step { from: 9, apply: settings_9_to_10 },
    ],
};

/// Schema 9 (TASK-180 test3): the Render scale is a percentage of the screen's height; it used to be a notch of the
/// screen's native multiplier. A schema 8 file's `render_scale` is in the old meaning, and this step records that in the
/// file (`[install] render_scale_model = 1`); the launcher then converts the value with the screen in hand and writes 2
/// (`state::migrate_render_scale`). Nothing else changes.
fn settings_8_to_9(document: &mut Value) -> Result<(), String> {
    let table = document.as_object_mut().ok_or("settings.toml: the document is not a table")?;
    let install = table.entry("install".to_string()).or_insert_with(|| Value::Object(serde_json::Map::new()));
    if let Some(section) = install.as_object_mut() {
        section.entry("render_scale_model".to_string()).or_insert_with(|| Value::from(1));
    }
    Ok(())
}

/// Schema 10 (TASK-180 test5, owner 2026-11-05): Performance Mode is replaced by independent effect toggles, each on by
/// default (the effect is drawn as the original game). A stored `[gs] performance_mode = "no-readbacks"` is the player's
/// old choice to turn the bundle on: it turns every effect toggle off, which is what it did, and the key goes. A file
/// without it means every effect on, which is the toggles' default, so nothing is written for it. The rest is untouched.
fn settings_9_to_10(document: &mut Value) -> Result<(), String> {
    let table = document.as_object_mut().ok_or("settings.toml: the document is not a table")?;
    if let Some(gs) = table.get_mut("gs").and_then(Value::as_object_mut) {
        let performance = gs.get("performance_mode").and_then(Value::as_str) == Some("no-readbacks");
        if performance {
            gs.insert("effect_flares".to_string(), Value::from("false"));
            gs.insert("effect_road_sheen".to_string(), Value::from("false"));
        }
        gs.remove("performance_mode");
    }
    Ok(())
}

/// Controls file (`controls.toml`), schema key `schema`. Written as schema 1 since the first release.
pub const CONTROLS: Chain = Chain { file: "controls.toml", key: "schema", first: 1, current: 1, steps: &[] };

/// version.json in the install folder (or the data folder when the install folder is read-only).
pub const VERSION_FILE: Chain = Chain { file: "version.json", key: "schema", first: 1, current: 1, steps: &[] };

/// Schema 2: Fullscreen is on by default. A file of schema 1 may hold the old default ("off") that was never
/// chosen on the Settings page. Such a value becomes "on"; a value the user chose (`[choices] fullscreen = true`)
/// is kept. The backup of the old file is made by `config::prepare` before this step runs.
fn settings_1_to_2(document: &mut Value) -> Result<(), String> {
    let chosen = document.pointer("/choices/fullscreen").and_then(Value::as_bool) == Some(true);
    if chosen {
        return Ok(());
    }
    if let Some(settings) = document.get_mut("settings").and_then(Value::as_object_mut) {
        if settings.get("fullscreen").and_then(Value::as_str) == Some("off") {
            settings.insert("fullscreen".to_string(), Value::from("on"));
        }
    }
    Ok(())
}

/// Schema 3: the Widescreen 16:9 row (TASK-079, `GT4_FORCE_WIDESCREEN`). A file of schema 2 has no such key; it gets
/// the row's default "on" written out, so the file records the value the game is sent. Other values are kept.
fn settings_2_to_3(document: &mut Value) -> Result<(), String> {
    if let Some(settings) = document.get_mut("settings").and_then(Value::as_object_mut) {
        settings.entry("widescreen".to_string()).or_insert_with(|| Value::from("on"));
    }
    Ok(())
}

/// Schema 4: the Aspect ratio and Custom ratio rows (TASK-080, `GT4_DISPLAY_ASPECT`). A file of schema 3 has neither
/// key; it gets the defaults written out ("auto" and 233 hundredths), so the file records what the game is sent. Values
/// already present are kept.
///
/// A schema 3 file written by the 1.0.0 launcher may still hold the settings that 1.0.0 retired by a load-time rule
/// (`settings::RETIRED_SETTINGS`): `input_resolution` and `widescreen` in `[settings]` and `[choices]`, and the PCSX2 GS
/// `aspect` in `[gs]`. The Aspect ratio row replaces all of them, so this step drops them; the old 16:9/4:3 value is
/// not carried over (1.0.0 always ran 16:9, and Auto is the new default). The ids are literal here on purpose: a
/// migration step describes the files of its own time and never changes with later tables.
fn settings_3_to_4(document: &mut Value) -> Result<(), String> {
    if let Some(settings) = document.get_mut("settings").and_then(Value::as_object_mut) {
        settings.remove("input_resolution");
        settings.remove("widescreen");
        settings.entry("aspect_ratio".to_string()).or_insert_with(|| Value::from("auto"));
        settings.entry("aspect_custom".to_string()).or_insert_with(|| Value::from("233"));
    }
    if let Some(choices) = document.get_mut("choices").and_then(Value::as_object_mut) {
        choices.remove("input_resolution");
        choices.remove("widescreen");
    }
    if let Some(gs) = document.get_mut("gs").and_then(Value::as_object_mut) {
        gs.remove("aspect");
    }
    Ok(())
}

/// Schema 5: seamless display (TASK-080 addendum, owner 2026-10-21). There is no aspect choice and no internal
/// resolution choice any more: the aspect is the Resolution's shape and the internal resolution follows the
/// Resolution and the Render scale (`display::display_plan`). This step drops `aspect_ratio` and `aspect_custom`
/// (written by the schema 4 ultrawide test builds) from `[settings]` and `[choices]`, and the PCSX2 GS `upscale`
/// (the old "Internal resolution" row) and `aspect` from `[gs]`; it adds `monitor` = "primary", `resolution` =
/// "desktop" and `render_scale` = "100", so the file records what the game is sent. A schema 3 file (release 1.0.0)
/// reaches this step through 3 -> 4 first, so both kinds of file end the same. Ids are literal on purpose (see
/// `settings_3_to_4`).
fn settings_4_to_5(document: &mut Value) -> Result<(), String> {
    if let Some(settings) = document.get_mut("settings").and_then(Value::as_object_mut) {
        settings.remove("aspect_ratio");
        settings.remove("aspect_custom");
        settings.entry("monitor".to_string()).or_insert_with(|| Value::from("primary"));
        settings.entry("resolution".to_string()).or_insert_with(|| Value::from("desktop"));
        settings.entry("render_scale".to_string()).or_insert_with(|| Value::from("100"));
    }
    if let Some(choices) = document.get_mut("choices").and_then(Value::as_object_mut) {
        choices.remove("aspect_ratio");
        choices.remove("aspect_custom");
    }
    if let Some(gs) = document.get_mut("gs").and_then(Value::as_object_mut) {
        gs.remove("upscale");
        gs.remove("aspect");
    }
    Ok(())
}

/// Schema 6 (TASK-088, gt4-gs 1.2.0). Two changes, both in this one step (schema 6 is not released yet):
///
/// - Custom textures: the Load custom textures row (`[gs] texture_load`, default on) and the Precache row
///   (`[gs] texture_precache`, default off) are gs-table rows. A value is kept in `[gs]` only when the user changed it,
///   and an absent key takes the table default, so nothing is rewritten for them.
/// - Resolution follows the desktop (owner, 2026-10-21). A stored Resolution equal to the desktop size of the stored
///   Monitor becomes "desktop", so it follows the monitor's desktop at each start. The desktop size is known only from
///   the Monitor identifier (`\\.\DISPLAY2@3440x1440` records the desktop when it was chosen, `display::desktop_at_save`).
///   A file whose Monitor is "primary" records no desktop size: its Resolution is kept as it was (it cannot be told
///   apart from a deliberate choice of that size). The rule is recorded here so that later readers know why.
///
/// - Performance Mode (TASK-091, release 1.2.1, owner 2026-10-22): the Readbacks row (`[gs] hw_download_mode`) is
///   replaced by the Performance Mode toggle (`[gs] performance_mode`). A stored readback mode other than "accurate"
///   becomes Performance Mode On ("no-readbacks"); "accurate", or no value, is Off, the default, which needs no key.
///   The old key is dropped in both cases.
///
/// The backup of the old file is made by `config::prepare`, as for every step.
fn settings_5_to_6(document: &mut Value) -> Result<(), String> {
    if let Some(gs) = document.get_mut("gs").and_then(Value::as_object_mut) {
        if let Some(old) = gs.remove("hw_download_mode") {
            if old.as_str() != Some("accurate") {
                gs.insert("performance_mode".to_string(), Value::from("no-readbacks"));
            }
        }
    }
    let Some(settings) = document.get_mut("settings").and_then(Value::as_object_mut) else {
        return Ok(());
    };
    let monitor = settings.get("monitor").and_then(Value::as_str).unwrap_or("primary").to_string();
    let resolution = settings.get("resolution").and_then(Value::as_str).unwrap_or("desktop").to_string();
    if let Some(desktop) = crate::display::desktop_at_save(&monitor) {
        if crate::display::Size::parse(&resolution) == Some(desktop) {
            settings.insert("resolution".to_string(), Value::from("desktop"));
        }
    }
    Ok(())
}

/// Schema 7 (TASK-096, launcher 1.2.3, owner 2026-10-22): CoreGS 1.1 is no longer started by the launcher. A stored
/// renderer of "coregs11" becomes "pcsx2" (GSdx), the only renderer the game runs with now. The CoreGS 1.1 rows
/// (internal resolution, scaling filter, FSR sharpness, deinterlacing, anti-aliasing) need no step: they are listed in
/// `settings::RETIRED_SETTINGS` and ignored on load. The `[choices]` flag of the renderer is kept as it was.
/// The id is literal on purpose (see `settings_3_to_4`).
fn settings_6_to_7(document: &mut Value) -> Result<(), String> {
    if let Some(settings) = document.get_mut("settings").and_then(Value::as_object_mut) {
        if settings.get("renderer").and_then(Value::as_str) == Some("coregs11") {
            settings.insert("renderer".to_string(), Value::from("pcsx2"));
        }
    }
    Ok(())
}

/// Schema 8 (TASK-101, launcher 1.3.0): the launcher runs more than one game, and the file records which one the home
/// screen shows (`[game] selected`, the first game installed, then the game played last). Every file of schema 7 or
/// older belongs to a launcher that ran Gran Turismo 4 only, so it selects `gt4`; a value already written (a hand edit)
/// is kept. A fresh install has no file, so nothing is selected until its first game is set up.
fn settings_7_to_8(document: &mut Value) -> Result<(), String> {
    let Some(root) = document.as_object_mut() else {
        return Ok(());
    };
    let game = root.entry("game").or_insert_with(|| Value::Object(serde_json::Map::new()));
    if let Some(game) = game.as_object_mut() {
        game.entry("selected").or_insert_with(|| Value::from("gt4"));
    }
    Ok(())
}

/// Example and first real step: the earlier dev builds wrote no schema number and nothing else differs, so the
/// step has no content change. It is the template for later steps: change the document, nothing else.
fn settings_0_to_1(_document: &mut Value) -> Result<(), String> {
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    /// A test chain with two steps, to prove the chaining order and the schema key being set.
    const TEST_CHAIN: Chain = Chain {
        file: "test.toml",
        key: "schema",
        first: 0,
        current: 2,
        steps: &[
            Step { from: 0, apply: add_a },
            Step { from: 1, apply: rename_a_to_b },
        ],
    };

    fn add_a(document: &mut Value) -> Result<(), String> {
        document["a"] = json!(1);
        Ok(())
    }

    fn rename_a_to_b(document: &mut Value) -> Result<(), String> {
        let value = document.as_object_mut().unwrap().remove("a").ok_or("no a")?;
        document["b"] = value;
        Ok(())
    }

    fn failing(_document: &mut Value) -> Result<(), String> {
        Err("broken".into())
    }

    #[test]
    fn every_real_chain_has_complete_ordered_steps() {
        for chain in [&SETTINGS, &CONTROLS, &VERSION_FILE, &TEST_CHAIN] {
            check_chain(chain).unwrap_or_else(|error| panic!("{error}"));
        }
    }

    #[test]
    fn an_unversioned_document_is_schema_zero_and_migrates_through_every_step() {
        let mut document = json!({});
        assert_eq!(plan(&TEST_CHAIN, &document), Verdict::Migrate { from: 0 });
        assert_eq!(migrate(&TEST_CHAIN, &mut document).unwrap(), 0);
        assert_eq!(document, json!({"b": 1, "schema": 2}));
    }

    #[test]
    fn a_document_one_step_behind_runs_only_the_remaining_step() {
        let mut document = json!({"schema": 1, "a": 7});
        assert_eq!(migrate(&TEST_CHAIN, &mut document).unwrap(), 1);
        assert_eq!(document, json!({"schema": 2, "b": 7}));
    }

    #[test]
    fn a_current_document_is_left_alone() {
        let mut document = json!({"schema": 2, "b": 3});
        assert_eq!(plan(&TEST_CHAIN, &document), Verdict::Current);
        assert_eq!(migrate(&TEST_CHAIN, &mut document).unwrap(), 2);
        assert_eq!(document, json!({"schema": 2, "b": 3}));
    }

    #[test]
    fn a_newer_document_is_reported_and_never_changed() {
        let mut document = json!({"schema": 9, "x": true});
        assert_eq!(plan(&TEST_CHAIN, &document), Verdict::Newer { found: 9 });
        let before = document.clone();
        assert!(migrate(&TEST_CHAIN, &mut document).is_err());
        assert_eq!(document, before, "a newer file is not touched");
    }

    #[test]
    fn a_schema_that_is_not_a_number_is_corrupt() {
        let document = json!({"schema": "two"});
        assert!(matches!(plan(&TEST_CHAIN, &document), Verdict::Corrupt(_)));
        assert!(matches!(plan(&TEST_CHAIN, &json!({"schema": -1})), Verdict::Corrupt(_)));
    }

    #[test]
    fn a_failing_step_stops_the_chain_and_names_the_step() {
        const BROKEN: Chain = Chain {
            file: "broken.toml",
            key: "schema",
            first: 0,
            current: 1,
            steps: &[Step { from: 0, apply: failing }],
        };
        let mut document = json!({});
        let error = migrate(&BROKEN, &mut document).unwrap_err();
        assert!(error.contains("0 -> 1") && error.contains("broken"), "{error}");
    }

    #[test]
    fn a_chain_with_a_gap_is_refused_by_the_check() {
        const GAP: Chain = Chain {
            file: "gap.toml",
            key: "schema",
            first: 0,
            current: 2,
            steps: &[Step { from: 0, apply: add_a }, Step { from: 0, apply: add_a }],
        };
        assert!(check_chain(&GAP).is_err());
    }

    #[test]
    fn a_document_below_the_first_schema_of_its_chain_is_corrupt() {
        assert!(matches!(plan(&VERSION_FILE, &json!({"gt4recomp": "1.0.0"})), Verdict::Corrupt(_)));
    }

    /// TASK-180 test3: a schema 8 file's Render scale is in the old notch model, and step 9 says so in the file so the
    /// launcher can convert it with the screen in hand.
    #[test]
    fn schema_eight_marks_the_old_render_scale_model() {
        let mut document = json!({"schema_version": 8, "settings": {"render_scale": "125"}, "install": {"disc_path": "D:/gt4.iso"}});
        assert_eq!(migrate(&SETTINGS, &mut document).unwrap(), 8);
        assert_eq!(document["schema_version"], json!(10));
        assert_eq!(document["install"]["render_scale_model"], json!(1));
        assert_eq!(document["install"]["disc_path"], json!("D:/gt4.iso"), "the rest of [install] is kept");
        assert_eq!(document["settings"]["render_scale"], json!("125"), "the value is converted at load, with the screen");
    }

    #[test]
    fn backup_names_carry_the_schema_and_the_time() {
        assert_eq!(backup_name("settings.toml", 0, 1760000000), "settings.toml.bak-schema0-1760000000");
    }

    #[test]
    fn settings_schema_zero_reaches_the_current_schema_and_keeps_its_values() {
        let mut document = json!({"settings": {"vsync": "fast"}});
        assert_eq!(migrate(&SETTINGS, &mut document).unwrap(), 0);
        assert_eq!(document["schema_version"], json!(10));
        assert_eq!(document["settings"]["vsync"], json!("fast"));
        assert!(document["settings"].get("widescreen").is_none(), "added by step 3, dropped by step 4");
        assert!(document["settings"].get("aspect_ratio").is_none(), "added by step 4, dropped by step 5");
        assert!(document["settings"].get("aspect_custom").is_none(), "added by step 4, dropped by step 5");
        assert_eq!(document["settings"]["monitor"], json!("primary"));
        assert_eq!(document["settings"]["resolution"], json!("desktop"));
        assert_eq!(document["settings"]["render_scale"], json!("100"));
    }

    #[test]
    fn a_release_1_0_0_file_drops_the_retired_keys_and_gets_aspect_auto() {
        // A schema 3 file as the 1.0.0 launcher could leave it: the retired rows still stored (an older 1.0.0
        // candidate wrote them, and 1.0.0 only ignores them on load), plus a GS aspect value.
        let mut released = json!({
            "schema_version": 3,
            "settings": {"vsync": "fast", "input_resolution": "1080i", "widescreen": "off", "fullscreen": "on"},
            "choices": {"input_resolution": true, "widescreen": true, "vsync": true},
            "gs": {"aspect": "4:3", "fxaa": "true", "osd_fps": "true"},
            "install": {"disc_path": "D:/gt4.iso"}
        });
        assert_eq!(migrate(&SETTINGS, &mut released).unwrap(), 3);
        assert_eq!(released["schema_version"], json!(10));
        assert!(released["settings"].get("aspect_ratio").is_none());
        assert_eq!(released["settings"]["monitor"], json!("primary"));
        assert_eq!(released["settings"]["resolution"], json!("desktop"));
        assert_eq!(released["settings"]["render_scale"], json!("100"));
        for retired in ["input_resolution", "widescreen"] {
            assert!(released["settings"].get(retired).is_none(), "{retired} dropped from [settings]");
            assert!(released["choices"].get(retired).is_none(), "{retired} dropped from [choices]");
        }
        assert!(released["gs"].get("aspect").is_none(), "the GS aspect is replaced by the Aspect ratio row");
        assert_eq!(released["settings"]["vsync"], json!("fast"), "other values survive");
        assert_eq!(released["choices"]["vsync"], json!(true));
        assert_eq!(released["gs"]["fxaa"], json!("true"));
        assert_eq!(released["gs"]["osd_fps"], json!("true"), "forced GS rows stay with the load-time rule");
        assert_eq!(released["install"]["disc_path"], json!("D:/gt4.iso"));

    }

    #[test]
    fn an_ultrawide_test_build_file_drops_aspect_and_upscale_and_gets_the_display_rows() {
        // A schema 4 file as the TASK-080 test builds wrote it (the owner's real file): a custom ratio and a chosen
        // internal resolution.
        let mut file = json!({
            "schema_version": 4,
            "settings": {"aspect_ratio": "custom", "aspect_custom": "244", "vsync": "on", "fullscreen": "off"},
            "choices": {"aspect_ratio": true, "aspect_custom": true, "fullscreen": true},
            "gs": {"upscale": "4K", "fxaa": "true"},
            "install": {"disc_path": "D:/gt4.iso"}
        });
        assert_eq!(migrate(&SETTINGS, &mut file).unwrap(), 4);
        assert_eq!(file["schema_version"], json!(10));
        for gone in ["aspect_ratio", "aspect_custom"] {
            assert!(file["settings"].get(gone).is_none(), "{gone} dropped from [settings]");
            assert!(file["choices"].get(gone).is_none(), "{gone} dropped from [choices]");
        }
        assert!(file["gs"].get("upscale").is_none(), "the internal resolution follows the Resolution now");
        assert_eq!(file["gs"]["fxaa"], json!("true"));
        assert_eq!(file["settings"]["monitor"], json!("primary"));
        assert_eq!(file["settings"]["resolution"], json!("desktop"));
        assert_eq!(file["settings"]["render_scale"], json!("100"));
        assert_eq!(file["settings"]["fullscreen"], json!("off"), "other values survive");
        assert_eq!(file["choices"]["fullscreen"], json!(true));
        assert_eq!(file["install"]["disc_path"], json!("D:/gt4.iso"));

        // Values already present (a file edited by hand) are kept.
        let mut kept = json!({"schema_version": 4, "settings": {"render_scale": "150", "resolution": "2560x1080"}});
        migrate(&SETTINGS, &mut kept).unwrap();
        assert_eq!(kept["settings"]["render_scale"], json!("150"));
        assert_eq!(kept["settings"]["resolution"], json!("2560x1080"));
    }

    #[test]
    fn schema_two_step_adds_widescreen_on_and_keeps_a_value_already_written() {
        // The 2 -> 3 step as shipped; step 4 removes the key again (tested above).
        let mut missing = json!({"settings": {"vsync": "on"}});
        settings_2_to_3(&mut missing).unwrap();
        assert_eq!(missing["settings"]["widescreen"], json!("on"));

        let mut off = json!({"settings": {"widescreen": "off"}});
        settings_2_to_3(&mut off).unwrap();
        assert_eq!(off["settings"]["widescreen"], json!("off"));

        let mut chained = json!({"schema_version": 2, "settings": {"vsync": "on"}});
        assert_eq!(migrate(&SETTINGS, &mut chained).unwrap(), 2);
        assert_eq!(chained["schema_version"], json!(10));
    }

    #[test]
    fn schema_one_fullscreen_off_becomes_on_unless_chosen() {
        let mut unchosen = json!({"schema_version": 1, "settings": {"fullscreen": "off"}});
        assert_eq!(migrate(&SETTINGS, &mut unchosen).unwrap(), 1);
        assert_eq!(unchosen["settings"]["fullscreen"], json!("on"));

        let mut chosen = json!({"schema_version": 1, "settings": {"fullscreen": "off"}, "choices": {"fullscreen": true}});
        migrate(&SETTINGS, &mut chosen).unwrap();
        assert_eq!(chosen["settings"]["fullscreen"], json!("off"));
    }

    #[test]
    fn a_stored_coregs11_renderer_becomes_gsdx_in_schema_7() {
        let mut document = json!({"schema_version": 6, "settings": {"renderer": "coregs11", "vsync": "fast"}, "choices": {"renderer": true}});
        assert_eq!(migrate(&SETTINGS, &mut document).unwrap(), 6);
        assert_eq!(document["schema_version"], json!(10));
        assert_eq!(document["settings"]["renderer"], json!("pcsx2"), "CoreGS 1.1 is gone: GSdx");
        assert_eq!(document["settings"]["vsync"], json!("fast"), "other values survive");
        assert_eq!(document["choices"]["renderer"], json!(true), "the choice flag is kept");

        let mut gsdx = json!({"schema_version": 6, "settings": {"renderer": "pcsx2"}});
        migrate(&SETTINGS, &mut gsdx).unwrap();
        assert_eq!(gsdx["settings"]["renderer"], json!("pcsx2"));

        let mut from_zero = json!({"settings": {"renderer": "coregs11"}});
        migrate(&SETTINGS, &mut from_zero).unwrap();
        assert_eq!(from_zero["settings"]["renderer"], json!("pcsx2"), "a file of any older schema takes the step too");
    }

    #[test]
    fn a_non_accurate_readback_mode_becomes_the_effect_toggles_off_and_the_old_keys_go() {
        for old in ["unsynchronized", "no-readbacks", "enabled-force-full", "disabled"] {
            let mut document = json!({"schema_version": 5, "gs": {"hw_download_mode": old, "fxaa": "true"}});
            assert_eq!(migrate(&SETTINGS, &mut document).unwrap(), 5, "{old}: returns the schema it started from");
            // TASK-180 test5 (schema 10): the old Performance Mode was every effect off, which is what its toggles say.
            assert_eq!(document["gs"]["effect_flares"], json!("false"), "{old}");
            assert_eq!(document["gs"]["effect_road_sheen"], json!("false"), "{old}");
            assert!(document["gs"].get("performance_mode").is_none(), "{old}: the key is gone");
            assert!(document["gs"].get("hw_download_mode").is_none(), "{old}");
            assert_eq!(document["gs"]["fxaa"], json!("true"), "other values are kept");
        }
    }

    #[test]
    fn an_accurate_or_missing_readback_mode_leaves_every_effect_on() {
        let mut accurate = json!({"schema_version": 5, "gs": {"hw_download_mode": "accurate"}});
        migrate(&SETTINGS, &mut accurate).unwrap();
        assert!(accurate["gs"].get("hw_download_mode").is_none(), "the old key is dropped");
        assert!(accurate["gs"].get("performance_mode").is_none(), "the old key stays gone");
        assert!(accurate["gs"].get("effect_flares").is_none(), "absent means on, the table default");

        let mut missing = json!({"schema_version": 5, "gs": {"fxaa": "false"}});
        migrate(&SETTINGS, &mut missing).unwrap();
        assert!(missing["gs"].get("performance_mode").is_none());
        assert!(missing["gs"].get("effect_flares").is_none());
    }

    /// TASK-180 test5: a schema 9 file that carries the old Performance Mode turns every effect off and loses the key.
    #[test]
    fn schema_nine_performance_mode_becomes_every_effect_off() {
        let mut on = json!({"schema_version": 9, "gs": {"performance_mode": "no-readbacks", "fxaa": "true"}});
        migrate(&SETTINGS, &mut on).unwrap();
        assert_eq!(on["schema_version"], json!(10));
        assert_eq!(on["gs"]["effect_flares"], json!("false"));
        assert_eq!(on["gs"]["effect_road_sheen"], json!("false"));
        assert!(on["gs"].get("performance_mode").is_none());
        assert_eq!(on["gs"]["fxaa"], json!("true"));

        let mut off = json!({"schema_version": 9, "gs": {"performance_mode": "accurate"}});
        migrate(&SETTINGS, &mut off).unwrap();
        assert!(off["gs"].get("performance_mode").is_none());
        assert!(off["gs"].get("effect_flares").is_none(), "every effect on, the default");
    }
}

//! The PCSX2 GS settings table and the `gt4-gs.ini` writer (decision 0070, TASK-039, TASK-019).
//!
//! `docs/launcher/gs-settings.toml` is the single source. It is compiled into the launcher with `include_str!`,
//! so the launcher and `scripts/gs_settings.py` read the same rows. This module is a port of the writer in
//! `scripts/gs_settings.py` (`parse_value`, `render_ini`); a golden test compares both byte for byte.
//!
//! Columns read here: id, label, help, section, key, type, min, max, choices, default, written,
//! status, env. `page` (basic | advanced | mods | hidden) replaces the older `visibility` column (TASK-049); it
//! also carries the UI page, with `control`
//! (slider | toggle | dropdown). An unknown or missing `page` is treated as advanced, an unknown or missing
//! `control` as dropdown. The older `page` values (Graphics, Display, Advanced) are not UI pages.

use std::collections::BTreeMap;
use std::sync::OnceLock;

use serde::Deserialize;

/// The table text, compiled in. Path is relative to this file (launcher/crates/gt4l-core/src).
pub const TABLE_TEXT: &str = include_str!("../../../../docs/launcher/gs-settings.toml");

/// The only table schema this launcher reads.
pub const SCHEMA_VERSION: i64 = 1;

/// First line of the gt4-gs.ini text. Kept verbatim: the default output must stay byte-identical to TASK-036.
pub const INI_HEADER: &str = "; gt4-gs.ini written by scripts/external_runtime_probe.py (TASK-036 minimal settings)";

/// INI sections in the order they are written (the same order as `SECTION_ORDER` in scripts/gs_settings.py).
const SECTION_ORDER: [&str; 4] = ["EmuCore/GS", "EmuCore", "Folders", "GT4GS"];

const UI_PAGES: [&str; 4] = ["basic", "advanced", "mods", "hidden"];
/// Rows of the mod manager (release 1.1.0): never offered and never user-set in 1.0.0.
pub const MODS_PAGE: &str = "mods";
const UI_CONTROLS: [&str; 3] = ["slider", "toggle", "dropdown"];

/// One allowed value of an enum row. `ini` is the value written to the file; `status` as in the table.
#[derive(Clone, Debug, Deserialize, PartialEq, Eq)]
pub struct Choice {
    pub name: String,
    #[serde(default)]
    pub ini: String,
    pub status: String,
    /// Display text of the choice (English); empty means the name. The UI translates it by key (TASK-102).
    #[serde(default)]
    pub label: String,
}

/// One row of the table, as read from the file.
#[derive(Clone, Debug, Deserialize, PartialEq)]
pub struct Row {
    pub id: String,
    pub label: String,
    #[serde(default)]
    pub help: String,
    #[serde(default)]
    pub page: String,
    #[serde(default)]
    pub control: String,
    #[serde(default)]
    pub section: String,
    #[serde(default)]
    pub key: String,
    #[serde(rename = "type")]
    pub kind: String,
    #[serde(default)]
    pub min: Option<f64>,
    #[serde(default)]
    pub max: Option<f64>,
    #[serde(default)]
    pub choices: Vec<Choice>,
    /// Enum rows: the INI value is also accepted as input (upscale 3 = 1080p). Mirrors `accept_ini`.
    #[serde(default)]
    pub accept_ini: bool,
    pub default: String,
    pub written: String,
    pub status: String,
    /// A hidden, always-written row whose value is fixed: only the default is accepted (owner, 2026-10-21: aspect
    /// 16:9 and the PCSX2 OSD rows off). Mirrors `forced` in scripts/gs_settings.py.
    #[serde(default)]
    pub forced: bool,
    #[serde(default)]
    pub env: String,
    /// A launcher-only row the Settings page still shows (TASK-180 test5: the effect toggles, which reach the game as an
    /// environment variable and are never written to the INI). `written = "never"` rows are otherwise hidden from the UI.
    #[serde(default)]
    pub offered: bool,
}

#[derive(Deserialize)]
struct RawTable {
    schema_version: i64,
    setting: Vec<Row>,
}

/// A table or value that is refused. The message names the row.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TableError(pub String);

impl std::fmt::Display for TableError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str(&self.0)
    }
}

impl std::error::Error for TableError {}

fn refuse<T>(message: impl Into<String>) -> Result<T, TableError> {
    Err(TableError(message.into()))
}

/// Parses and checks table text. Used on the compiled-in text and by tests with other texts.
pub fn parse_table(text: &str) -> Result<Vec<Row>, TableError> {
    let raw: RawTable = toml::from_str(text).map_err(|error| TableError(format!("GS settings table: {error}")))?;
    if raw.schema_version != SCHEMA_VERSION {
        return refuse(format!("GS settings table schema_version must be {SCHEMA_VERSION}"));
    }
    let mut seen_ids: Vec<&str> = Vec::new();
    let mut seen_keys: Vec<(&str, &str)> = Vec::new();
    for row in &raw.setting {
        if seen_ids.contains(&row.id.as_str()) {
            return refuse(format!("{}: duplicate id", row.id));
        }
        seen_ids.push(&row.id);
        check_row(row)?;
        if !row.section.is_empty() && !row.key.is_empty() {
            let pair = (row.section.as_str(), row.key.as_str());
            if seen_keys.contains(&pair) {
                return refuse(format!("{}: duplicate INI key {} {}", row.id, row.section, row.key));
            }
            seen_keys.push(pair);
        }
    }
    Ok(raw.setting)
}

fn check_row(row: &Row) -> Result<(), TableError> {
    let name = &row.id;
    if !is_id(name) {
        return refuse(format!("{name}: id must be lowercase letters, digits and underscores"));
    }
    if !matches!(row.kind.as_str(), "bool" | "int" | "float" | "enum" | "string" | "path") {
        return refuse(format!("{name}: unknown type {}", row.kind));
    }
    if !matches!(row.written.as_str(), "always" | "when-set" | "launcher" | "never") {
        return refuse(format!("{name}: unknown written value {}", row.written));
    }
    if !is_status(&row.status) {
        return refuse(format!("{name}: status must be ok, resolved or pending-TASK-NNN"));
    }
    if row.written == "never" && (!row.section.is_empty() || !row.key.is_empty()) {
        return refuse(format!("{name}: launcher-only row must have no INI section or key"));
    }
    if row.offered && (row.written != "never" || row.env.is_empty()) {
        return refuse(format!("{name}: an offered launcher-only row must be written = \"never\" with an env variable"));
    }
    if row.written != "never" && row.status == "ok" && (row.section.is_empty() || row.key.is_empty()) {
        return refuse(format!("{name}: settable row needs an INI section and key"));
    }
    if matches!(row.written.as_str(), "always" | "launcher") && row.status != "ok" {
        return refuse(format!("{name}: {} rows must have status ok", row.written));
    }
    if row.kind == "path" && row.written != "launcher" {
        return refuse(format!("{name}: path rows are launcher-supplied only"));
    }
    if row.kind == "enum" {
        let mut names: Vec<&str> = row.choices.iter().map(|choice| choice.name.as_str()).collect();
        names.sort_unstable();
        names.dedup();
        if names.len() != row.choices.len() {
            return refuse(format!("{name}: duplicate choice names"));
        }
        if row.status == "ok" && !row.choices.iter().any(|choice| choice.status == "ok") {
            return refuse(format!("{name}: ok enum row needs at least one ok choice"));
        }
    }
    if row.forced && (row.page != "hidden" || row.written != "always" || row.status != "ok") {
        return refuse(format!("{name}: forced rows must be page hidden, written always and status ok"));
    }
    if row.status == "ok" && matches!(row.written.as_str(), "always" | "when-set") && row.kind != "path" {
        parse_value(row, &row.default).map_err(|error| TableError(format!("{name}: default refused: {error}")))?;
    }
    Ok(())
}

fn is_id(text: &str) -> bool {
    let mut characters = text.chars();
    matches!(characters.next(), Some(first) if first.is_ascii_lowercase())
        && characters.all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_')
}

fn is_status(text: &str) -> bool {
    matches!(text, "ok" | "resolved") || text.strip_prefix("pending-TASK-").is_some_and(|number| {
        number.len() == 3 && number.chars().all(|c| c.is_ascii_digit())
    })
}

/// The compiled-in table, checked once. A bad table is a build error caught by the tests.
pub fn rows() -> &'static [Row] {
    static ROWS: OnceLock<Vec<Row>> = OnceLock::new();
    ROWS.get_or_init(|| parse_table(TABLE_TEXT).expect("docs/launcher/gs-settings.toml is valid"))
}

/// The row with this id.
pub fn row(id: &str) -> Option<&'static Row> {
    rows().iter().find(|row| row.id == id)
}

/// The default text of a settable row (enum rows: the launcher name).
pub fn default_value(id: &str) -> Option<String> {
    row(id).map(|row| row.default.clone())
}

/// Checks a user value (from settings.toml or the UI) against the compiled-in table and returns its normalized text.
pub fn check_user_value(id: &str, raw: &str) -> Result<String, String> {
    let row = row(id).ok_or_else(|| format!("unknown GS setting '{id}'"))?;
    // TASK-180 test5: an offered launcher-only row (the effect toggles) takes a user value; it simply never reaches the INI.
    if (matches!(row.written.as_str(), "never" | "launcher") && !row.offered) || row.kind == "path" {
        return Err(format!("{id} is set by the launcher, not by a user setting"));
    }
    if row.page == MODS_PAGE {
        // Mod rows belong to release 1.1.0 (owner decision): 1.0.0 never takes a user value for them.
        return Err(format!("{id} is not available in this release"));
    }
    parse_value(row, raw).map_err(|error| error.0)
}

/// Rows the launcher may show: basic or advanced, settable, and with a settable value for enums.
pub fn offered() -> Vec<&'static Row> {
    rows()
        .iter()
        .filter(|row| {
            row.page != MODS_PAGE
                && row.page != "hidden"
                && row.status == "ok"
                && (!matches!(row.written.as_str(), "never" | "launcher") || (row.offered && !row.env.is_empty()))
                && (row.kind != "enum" || row.choices.iter().any(|choice| choice.status == "ok"))
        })
        .collect()
}

/// The UI page of a row: basic, advanced, mods or hidden. Unknown or missing values are advanced.
pub fn ui_page(row: &Row) -> &str {
    if UI_PAGES.contains(&row.page.as_str()) { row.page.as_str() } else { "advanced" }
}

/// The UI control of a row: slider, toggle or dropdown. Unknown or missing values are dropdown.
pub fn ui_control(row: &Row) -> &str {
    if UI_CONTROLS.contains(&row.control.as_str()) { row.control.as_str() } else { "dropdown" }
}

/// The choice names the launcher may offer for an enum row. A forced row offers only its default.
pub fn offered_choices(row: &Row) -> Vec<&str> {
    if row.forced {
        return vec![row.default.as_str()];
    }
    row.choices.iter().filter(|choice| choice.status == "ok").map(|choice| choice.name.as_str()).collect()
}

/// Checks one value of a row and returns its normalized text (enum name, bool, integer text, float text or path).
/// Mirrors `parse_value` in scripts/gs_settings.py: a forced row accepts only its default value.
pub fn parse_value(row: &Row, raw: &str) -> Result<String, TableError> {
    let value = parse_unforced(row, raw)?;
    if row.forced {
        let default = parse_unforced(row, &row.default)?;
        if value != default {
            return refuse(format!("{} is fixed at {default} in this release", row.id));
        }
    }
    Ok(value)
}

fn parse_unforced(row: &Row, raw: &str) -> Result<String, TableError> {
    let name = &row.id;
    if row.kind == "path" {
        if raw.is_empty() || raw.contains('\n') || raw.contains('\r') {
            return refuse(format!("{name} must be a single-line folder path"));
        }
        return Ok(raw.to_string());
    }
    if row.status != "ok" {
        return refuse(format!("{name} is not settable yet ({})", row.status));
    }
    match row.kind.as_str() {
        "bool" => match raw {
            "true" | "false" => Ok(raw.to_string()),
            _ => refuse(format!("{name} must be true or false, got {raw:?}")),
        },
        "int" => {
            let number = parse_integer(raw).ok_or_else(|| TableError(format!("{name} must be an integer, got {raw:?}")))?;
            check_range(row, number as f64)?;
            Ok(number.to_string())
        }
        "float" => {
            if !is_decimal(raw) {
                return refuse(format!("{name} must be a number, got {raw:?}"));
            }
            let number: f64 = raw.parse().map_err(|_| TableError(format!("{name} must be a number, got {raw:?}")))?;
            check_range(row, number)?;
            Ok(raw.to_string())
        }
        "enum" => {
            // With accept_ini, the INI value is accepted as well (upscale: 3 = 1080p). Mirrors parse_value in Python.
            let by_name = row.choices.iter().any(|choice| choice.name == raw);
            let name_text = match row.choices.iter().find(|choice| choice.ini == raw) {
                Some(choice) if !by_name && row.accept_ini => choice.name.as_str(),
                _ => raw,
            };
            let choice = row
                .choices
                .iter()
                .find(|choice| choice.name == name_text)
                .ok_or_else(|| TableError(format!("{name} must be one of {}, got {raw:?}", names(row))))?;
            if choice.status != "ok" {
                return refuse(format!("{name}={raw} is not settable yet ({})", choice.status));
            }
            Ok(name_text.to_string())
        }
        other => refuse(format!("{name} cannot be set from a value (type {other})")),
    }
}

fn names(row: &Row) -> String {
    let mut names: Vec<&str> = row.choices.iter().map(|choice| choice.name.as_str()).collect();
    names.sort_unstable();
    names.join(", ")
}

/// Python's `int(text)` for the forms the table accepts: optional minus sign and digits only.
fn parse_integer(text: &str) -> Option<i64> {
    let digits = text.strip_prefix('-').unwrap_or(text);
    if digits.is_empty() || !digits.chars().all(|c| c.is_ascii_digit()) {
        return None;
    }
    text.parse().ok()
}

fn is_decimal(text: &str) -> bool {
    let unsigned = text.strip_prefix('-').unwrap_or(text);
    let mut parts = unsigned.splitn(2, '.');
    let whole = parts.next().unwrap_or("");
    let fraction = parts.next();
    !whole.is_empty()
        && whole.chars().all(|c| c.is_ascii_digit())
        && fraction.is_none_or(|part| !part.is_empty() && part.chars().all(|c| c.is_ascii_digit()))
}

fn check_range(row: &Row, number: f64) -> Result<(), TableError> {
    let (Some(min), Some(max)) = (row.min, row.max) else {
        return refuse(format!("{} has no range", row.id));
    };
    if number < min || number > max {
        return refuse(format!("{} must be from {} to {}, got {}", row.id, min, max, number));
    }
    Ok(())
}

fn ini_value(row: &Row, name: &str) -> String {
    if row.kind == "enum" {
        row.choices.iter().find(|choice| choice.name == name).map(|choice| choice.ini.clone()).unwrap_or_default()
    } else {
        name.to_string()
    }
}

/// The gt4-gs.ini text. `settings` holds user values by id; `launcher` the values the launcher supplies
/// (folders, the deferred swap chain flag, headless dumps). Mirrors `render_ini` in scripts/gs_settings.py.
///
/// 'always' rows are written with their default unless a user value overrides them; 'when-set' rows only when a
/// user value is given; 'launcher' rows only when the launcher supplies one. Output: the header, then the sections
/// in `SECTION_ORDER`, each with its rows in table order, separated by a blank line, ending with a newline.
pub fn render_ini(settings: &BTreeMap<String, String>, launcher: &BTreeMap<String, String>) -> Result<String, TableError> {
    let mut checked_settings: BTreeMap<String, String> = BTreeMap::new();
    for (identifier, value) in settings {
        let row = row(identifier).ok_or_else(|| TableError(format!("unknown GS setting '{identifier}'")))?;
        if matches!(row.written.as_str(), "never" | "launcher") || row.kind == "path" {
            return refuse(format!("{identifier} is set by the launcher, not by a user setting"));
        }
        checked_settings.insert(identifier.clone(), parse_value(row, value)?);
    }
    let mut checked_launcher: BTreeMap<String, String> = BTreeMap::new();
    for (identifier, value) in launcher {
        let row = row(identifier).ok_or_else(|| TableError(format!("unknown GS setting '{identifier}'")))?;
        if row.written != "launcher" {
            return refuse(format!("{identifier} is not a launcher row"));
        }
        checked_launcher.insert(identifier.clone(), parse_value(row, value)?);
    }
    let mut entries: Vec<(&str, &str, String)> = Vec::new();
    for row in rows() {
        if row.section.is_empty() || row.key.is_empty() {
            continue;
        }
        let value = match row.written.as_str() {
            "always" => checked_settings.get(&row.id).cloned().unwrap_or_else(|| row.default.clone()),
            // Written only when set and different from the default (Python: render_ini).
            "when-set" => match checked_settings.get(&row.id) {
                Some(value) if *value != parse_value(row, &row.default)? => value.clone(),
                _ => continue,
            },
            "launcher" => match checked_launcher.get(&row.id) {
                Some(value) => value.clone(),
                None => continue,
            },
            _ => continue,
        };
        entries.push((row.section.as_str(), row.key.as_str(), ini_value(row, &value)));
    }
    let mut lines: Vec<String> = vec![INI_HEADER.to_string()];
    let mut first = true;
    for section in SECTION_ORDER {
        let block: Vec<&(&str, &str, String)> = entries.iter().filter(|entry| entry.0 == section).collect();
        if block.is_empty() {
            continue;
        }
        if !first {
            lines.push(String::new());
        }
        first = false;
        lines.push(format!("[{section}]"));
        lines.extend(block.iter().map(|(_, key, value)| format!("{key} = {value}")));
    }
    Ok(lines.join("\n") + "\n")
}

/// The bytes of a gt4-gs.ini file. `render_ini` returns LF lines; the file is written the way Python's text-mode
/// write does it (CRLF on Windows), so the launcher and scripts/gs_settings.py produce the same bytes.
pub fn ini_file_bytes(text: &str) -> Vec<u8> {
    if cfg!(windows) {
        text.replace('\n', "\r\n").into_bytes()
    } else {
        text.as_bytes().to_vec()
    }
}

/// Writes the file through a temporary file next to it, then renames it over the old one (atomic on NTFS).
pub fn write_ini_atomic(path: &std::path::Path, text: &str) -> std::io::Result<()> {
    use std::io::Write;
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let temporary = path.with_extension("ini.tmp");
    {
        let mut file = std::fs::File::create(&temporary)?;
        file.write_all(&ini_file_bytes(text))?;
        file.sync_all()?;
    }
    std::fs::rename(&temporary, path)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn compiled_table_is_valid_and_has_the_graphics_api_row() {
        assert!(rows().len() > 40);
        let api = row("graphics_api").expect("graphics_api row");
        assert_eq!(api.default, "auto");
        assert_eq!(api.page, "basic", "Graphics page");
        assert!(offered().iter().any(|offered| offered.id == "graphics_api"));
        // TASK-103: three APIs, Vulkan first; no OpenGL (12) and no software renderer (13) yet. TASK-108: Automatic first.
        let names: Vec<&str> = api.choices.iter().map(|choice| choice.name.as_str()).collect();
        assert_eq!(names, ["auto", "vulkan", "d3d12", "d3d11"]);
        let inis: Vec<&str> = api.choices.iter().map(|choice| choice.ini.as_str()).collect();
        assert_eq!(inis, ["14", "14", "15", "3"]);
        assert_eq!(api.choices[0].label, "Automatic");
        assert_eq!(api.choices[1].label, "Vulkan");
        assert_eq!(api.choices[2].label, "Direct3D 12");
        assert_eq!(api.choices[3].label, "Direct3D 11");
        // The engine setting of the launcher is also called "renderer": the table row must not use that id.
        assert!(row("renderer").is_none(), "the table row is graphics_api");
        assert!(!offered().is_empty());
    }

    #[test]
    fn each_graphics_api_choice_writes_its_renderer_number() {
        // auto writes 14 only if it was never resolved; the launcher resolves it before every write (gpu::plan).
        for (choice, number) in [("auto", "14"), ("vulkan", "14"), ("d3d12", "15"), ("d3d11", "3")] {
            let mut settings = BTreeMap::new();
            settings.insert("graphics_api".to_string(), choice.to_string());
            let text = render_ini(&settings, &BTreeMap::new()).unwrap();
            assert!(text.contains(&format!("Renderer = {number}
")), "{choice} writes {number}");
        }
    }

    #[test]
    fn a_missing_or_unknown_graphics_api_is_automatic() {
        // Missing: the table default is Automatic (TASK-108; an install that never changed the row has no stored value).
        assert_eq!(default_value("graphics_api").as_deref(), Some("auto"));
        let text = render_ini(&BTreeMap::new(), &BTreeMap::new()).unwrap();
        assert!(text.contains("Renderer = 14
"));
        // Unknown: refused by the table check, so the launcher keeps the default (config.rs warns and uses it).
        assert!(check_user_value("graphics_api", "opengl").is_err());
        assert!(check_user_value("graphics_api", "software").is_err());
        assert!(check_user_value("graphics_api", "").is_err());
        let mut unknown = BTreeMap::new();
        unknown.insert("graphics_api".to_string(), "opengl".to_string());
        assert!(render_ini(&unknown, &BTreeMap::new()).is_err(), "the writer never writes an unknown value");
    }

    #[test]
    fn unknown_or_missing_page_and_control_are_advanced_and_dropdown() {
        let mut probe = row("upscale").unwrap().clone();
        probe.page = String::new();
        probe.control = "knob".to_string();
        assert_eq!(ui_page(&probe), "advanced");
        assert_eq!(ui_control(&probe), "dropdown");
        probe.page = "basic".to_string();
        probe.control = "slider".to_string();
        assert_eq!(ui_page(&probe), "basic");
        assert_eq!(ui_control(&probe), "slider");
    }

    #[test]
    fn values_are_checked_like_the_python_writer() {
        // Main's table: upscale accepts its INI value (accept_ini) and the name; 9 is the INI value of 9x, out of
        // range values are refused. Old expectation (upscale 9 refused) encoded the older table.
        assert_eq!(check_user_value("upscale", "2").unwrap(), "720p");
        assert_eq!(check_user_value("upscale", "9").unwrap(), "9x");
        assert!(check_user_value("upscale", "13").is_err());
        assert!(check_user_value("upscale", "x").is_err());
        // TASK-088 (1.2.0): every gt4-gs value the binary accepts is settable (the resolved ones were enabled).
        assert!(check_user_value("graphics_api", "d3d12").is_ok());
        assert!(check_user_value("graphics_api", "vulkan").is_ok());
        assert!(check_user_value("filter", "nearest").is_ok());
        assert!(check_user_value("graphics_api", "opengl").is_err(), "not a value of the table");
        assert!(check_user_value("cache_folder", "C:/x").is_err(), "launcher rows are not user settings");
        assert!(check_user_value("vsync", "yes").is_err());
    }

    #[test]
    fn forced_rows_are_hidden_written_and_fixed() {
        // Owner, 2026-10-21: 16:9 until the ultrawide update, and PCSX2's own OSD rows always off.
        for (id, fixed) in [("aspect", "16:9"), ("osd_fps", "false"), ("osd_gpu", "false"), ("osd_resolution", "false")] {
            let forced = row(id).unwrap();
            assert!(forced.forced && forced.page == "hidden" && forced.written == "always", "{id}");
            assert!(!offered().iter().any(|offered| offered.id == id), "{id} is not offered");
            assert_eq!(check_user_value(id, fixed).unwrap(), fixed);
            assert!(check_user_value(id, if fixed == "false" { "true" } else { "4:3" }).is_err(), "{id}");
        }
        assert_eq!(offered_choices(row("aspect").unwrap()), vec!["16:9"]);
        let text = render_ini(&BTreeMap::new(), &BTreeMap::new()).unwrap();
        for line in ["AspectRatio = 16:9\n", "OsdShowFPS = false\n", "OsdShowGPU = false\n", "OsdShowResolution = false\n"] {
            assert!(text.contains(line), "{line}");
        }
        // The other PCSX2 OSD rows (position, scale) are hidden and not settable.
        for id in ["osd_messages_pos", "osd_performance_pos", "osd_scale"] {
            assert_eq!(row(id).unwrap().page, "hidden", "{id}");
        }
        // TASK-118: one Performance overlay switch stays on the Advanced section; its six detail rows are hidden and keep
        // their defaults (the overlay shows everything when it is on).
        assert_eq!(
            (row("perf_overlay").unwrap().section.as_str(), row("perf_overlay").unwrap().key.as_str(),
             row("perf_overlay").unwrap().page.as_str()),
            ("GT4GS", "PerfOverlay", "advanced")
        );
        assert!(offered().iter().any(|offered| offered.id == "perf_overlay"), "perf_overlay is offered");
        for (id, key) in [
            ("perf_overlay_fps", "PerfOverlayFPS"),
            ("perf_overlay_frame_time", "PerfOverlayFrameTime"),
            ("perf_overlay_cpu", "PerfOverlayCPU"),
            ("perf_overlay_gpu", "PerfOverlayGPU"),
            ("perf_overlay_resolution", "PerfOverlayResolution"),
            ("perf_overlay_breakdown", "PerfOverlayBreakdown"),
        ] {
            let overlay = row(id).unwrap();
            assert_eq!((overlay.section.as_str(), overlay.key.as_str(), overlay.page.as_str()), ("GT4GS", key, "hidden"));
            assert!(!offered().iter().any(|offered| offered.id == id), "{id} is hidden");
        }
    }

    #[test]
    fn default_ini_has_the_task_036_values_and_no_user_rows() {
        let text = render_ini(&BTreeMap::new(), &BTreeMap::new()).unwrap();
        assert!(text.starts_with(INI_HEADER));
        assert!(text.contains("\n[EmuCore/GS]\n"));
        assert!(text.contains("Renderer = 14\n"));
        assert!(text.contains("upscale_multiplier = 3\n"));
        assert!(text.contains("HWDownloadMode = 0\n"));
        assert!(text.contains("VsyncEnable = true\n"));
        assert!(!text.contains("fxaa"), "when-set rows are not written by default");
        assert!(!text.contains("[Folders]"), "launcher rows are not written without launcher values");
    }

    #[test]
    fn launcher_rows_are_written_only_when_supplied() {
        let mut launcher = BTreeMap::new();
        launcher.insert("cache_folder".to_string(), "C:/cache".to_string());
        launcher.insert("defer_swap_chain_acquire".to_string(), "false".to_string());
        let text = render_ini(&BTreeMap::new(), &launcher).unwrap();
        assert!(text.contains("\n[Folders]\nCache = C:/cache\n"));
        assert!(text.contains("DeferSwapChainAcquire = false\n"));
    }
}

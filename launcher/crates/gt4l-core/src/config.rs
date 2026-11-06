//! `settings.toml`: settings, the chosen disc image and the optional development game folder.
//!
//! The file is written by hand (one `key = "value"` line per setting) so it stays readable, and it
//! is parsed with the `toml` crate. A file with a newer schema is never overwritten.

use std::fmt;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};

use crate::migrate::{self, Verdict};
use crate::settings::{self, Settings};
use crate::version::{ReleaseVersion, Version};

/// Schema of this file (`migrate::SETTINGS`). Files without a schema number are schema 0 and are migrated.
pub const SCHEMA_VERSION: u32 = 10;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ConfigFile {
    pub settings: Settings,
    pub disc_path: Option<PathBuf>,
    /// An existing texture folder the player chose (`[install] textures_path`, TASK-157), read in place instead of
    /// `<data>\Textures`. Launcher-only; the launcher never writes into it. `None`: the default folder.
    pub textures_path: Option<PathBuf>,
    pub dev_game_dir: Option<PathBuf>,
    /// "Skip this version" choices of the update popup: no offer again for exactly this version.
    pub skipped_launcher: Option<ReleaseVersion>,
    pub skipped_renderer: Option<Version>,
    /// The game the home screen shows (`[game] selected`, schema 8, TASK-101): the first game installed, then the game
    /// played last. `None` before any game is installed. An id the launcher does not know reads as `None`.
    pub selected_game: Option<String>,
    /// `[install] render_scale_model` (schema 9, TASK-180 test3): 2 when `render_scale` is a percentage of the screen's
    /// height (the current meaning), 1 for a file written under the old one-notch-of-25% model. A file without the key is
    /// model 1, which `state::migrate_render_scale` converts with the screen in hand.
    pub render_scale_model: u32,
}

impl Default for ConfigFile {
    fn default() -> Self {
        ConfigFile {
            settings: Settings::default(),
            disc_path: None,
            textures_path: None,
            dev_game_dir: None,
            skipped_launcher: None,
            skipped_renderer: None,
            selected_game: None,
            render_scale_model: 1,
        }
    }
}

#[derive(Debug)]
pub enum ConfigError {
    Syntax(String),
    NewerSchema(u32),
    Io(std::io::Error),
}

impl fmt::Display for ConfigError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ConfigError::Syntax(detail) => write!(formatter, "settings file is not valid TOML: {detail}"),
            ConfigError::NewerSchema(version) => {
                write!(formatter, "settings file uses schema {version}, newer than this launcher")
            }
            ConfigError::Io(error) => write!(formatter, "settings file: {error}"),
        }
    }
}

impl std::error::Error for ConfigError {}

impl From<std::io::Error> for ConfigError {
    fn from(error: std::io::Error) -> Self {
        ConfigError::Io(error)
    }
}

/// Parses the file text. Invalid setting values fall back to their defaults and are reported.
pub fn parse(text: &str) -> Result<(ConfigFile, Vec<String>), ConfigError> {
    let table: toml::Table = text
        .parse()
        .map_err(|error: toml::de::Error| ConfigError::Syntax(error.to_string()))?;
    let schema = table
        .get("schema_version")
        .and_then(|value| value.as_integer())
        .unwrap_or(SCHEMA_VERSION as i64);
    if schema > SCHEMA_VERSION as i64 {
        return Err(ConfigError::NewerSchema(schema as u32));
    }
    let mut warnings = Vec::new();
    let mut config = ConfigFile::default();
    if let Some(section) = table.get("settings").and_then(|value| value.as_table()) {
        for (key, value) in section {
            match (settings::definition(key), value.as_str()) {
                (None, _) => match settings::retired(key) {
                    // Load-time rule (no schema step): a retired setting is fixed now; its stored value never matters.
                    Some(reason) => warnings.push(format!("setting '{key}' is no longer used ({reason}); ignored")),
                    None => warnings.push(format!("unknown setting '{key}' ignored")),
                },
                (Some(_), None) => warnings.push(format!("setting '{key}' is not text; using the default")),
                (Some(_), Some(text)) => {
                    if let Some(warning) = config.settings.set_or_default(key, text) {
                        warnings.push(warning);
                    }
                }
            }
        }
    }
    // `[choices]`: settings the user set on the Settings page (see `Settings::set_by_user`).
    if let Some(section) = table.get("choices").and_then(|value| value.as_table()) {
        for (key, value) in section {
            if settings::definition(key).is_some() && value.as_bool() == Some(true) {
                config.settings.mark_chosen(key);
            }
        }
    }
    if let Some(section) = table.get("gs").and_then(|value| value.as_table()) {
        for (key, value) in section {
            match value.as_str() {
                None => warnings.push(format!("gs setting '{key}' is not text; using the default")),
                Some(text) => {
                    if let Err(error) = config.settings.set_gs(key, text) {
                        warnings.push(format!("{error}; using the default"));
                    }
                }
            }
        }
    }
    config.disc_path = table
        .get("install")
        .and_then(|value| value.as_table())
        .and_then(|section| section.get("disc_path"))
        .and_then(|value| value.as_str())
        .filter(|text| !text.is_empty())
        .map(PathBuf::from);
    config.textures_path = table
        .get("install")
        .and_then(|value| value.as_table())
        .and_then(|section| section.get("textures_path"))
        .and_then(|value| value.as_str())
        .filter(|text| !text.is_empty())
        .map(PathBuf::from);
    config.render_scale_model = table
        .get("install")
        .and_then(|value| value.as_table())
        .and_then(|section| section.get("render_scale_model"))
        .and_then(|value| value.as_integer())
        .and_then(|number| u32::try_from(number).ok())
        .unwrap_or(1);
    config.dev_game_dir = table
        .get("dev")
        .and_then(|value| value.as_table())
        .and_then(|section| section.get("game_dir"))
        .and_then(|value| value.as_str())
        .map(PathBuf::from);
    config.selected_game = table
        .get("game")
        .and_then(|value| value.as_table())
        .and_then(|section| section.get("selected"))
        .and_then(|value| value.as_str())
        .and_then(|id| match crate::games::by_id(id) {
            Some(game) => Some(game.id.to_string()),
            None => {
                warnings.push(format!("game '{id}' is not known to this launcher; ignored"));
                None
            }
        });
    if let Some(section) = table.get("updates").and_then(|value| value.as_table()) {
        let text = |key: &str| section.get(key).and_then(|value| value.as_str());
        config.skipped_launcher = text("skip_launcher").and_then(ReleaseVersion::parse);
        config.skipped_renderer = text("skip_renderer").and_then(Version::parse);
    }
    Ok((config, warnings))
}

/// Renders the file. Output is deterministic: settings in table order, then the other sections.
pub fn render(config: &ConfigFile) -> String {
    let mut text = String::new();
    text.push_str("# GT4Recomp settings. Changes apply the next time you play.\n");
    text.push_str(&format!("schema_version = {SCHEMA_VERSION}\n\n[settings]\n"));
    for definition in settings::SETTINGS {
        text.push_str(&format!("{} = {}\n", definition.id, quote(config.settings.get(definition.id))));
    }
    let chosen = config.settings.chosen_ids();
    if !chosen.is_empty() {
        text.push_str("
[choices]
");
        for id in chosen {
            text.push_str(&format!("{id} = true
"));
        }
    }
    if !config.settings.gs_overrides().is_empty() {
        // PCSX2 GS table values (decision 0070): only the values the user set are stored.
        text.push_str("\n[gs]\n");
        for (key, value) in config.settings.gs_overrides() {
            text.push_str(&format!("{key} = {}\n", quote(value)));
        }
    }
    if config.skipped_launcher.is_some() || config.skipped_renderer.is_some() {
        text.push_str("\n[updates]\n");
        if let Some(version) = config.skipped_launcher {
            text.push_str(&format!("skip_launcher = {}\n", quote(&version.text())));
        }
        if let Some(version) = config.skipped_renderer {
            text.push_str(&format!("skip_renderer = {}\n", quote(&version.text())));
        }
    }
    if let Some(game) = &config.selected_game {
        text.push_str(&format!("\n[game]\nselected = {}\n", quote(game)));
    }
    text.push_str("\n[install]\n");
    if let Some(path) = &config.disc_path {
        text.push_str(&format!("disc_path = {}\n", quote(&path.to_string_lossy())));
    }
    if let Some(path) = &config.textures_path {
        text.push_str(&format!("textures_path = {}\n", quote(&path.to_string_lossy())));
    }
    text.push_str(&format!("render_scale_model = {}\n", config.render_scale_model));
    if let Some(path) = &config.dev_game_dir {
        text.push_str("\n[dev]\n");
        text.push_str(&format!("game_dir = {}\n", quote(&path.to_string_lossy())));
    }
    text
}

/// A TOML basic string: quotes, backslashes and control characters are escaped.
fn quote(value: &str) -> String {
    let mut out = String::with_capacity(value.len() + 2);
    out.push('"');
    for character in value.chars() {
        match character {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            control if control.is_control() => out.push_str(&format!("\\u{:04X}", control as u32)),
            other => out.push(other),
        }
    }
    out.push('"');
    out
}

/// Loads the config file. A missing file gives defaults. A damaged file is renamed to
/// `settings.toml.bad` and defaults are used, so a bad file never blocks the launcher.
pub fn load(path: &Path) -> (ConfigFile, Vec<String>) {
    let text = match fs::read_to_string(path) {
        Ok(text) => text,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return (ConfigFile::default(), Vec::new()),
        Err(error) => return (ConfigFile::default(), vec![format!("settings file unreadable: {error}")]),
    };
    let (text, mut notes) = match prepare(path, &text) {
        Ok(prepared) => prepared,
        Err(message) => {
            let backup = path.with_extension("toml.bad");
            let _ = fs::rename(path, &backup);
            return (ConfigFile::default(), vec![format!("{message}; moved to {}", backup.display())]);
        }
    };
    match parse(&text) {
        Ok((config, warnings)) => {
            notes.extend(warnings);
            (config, notes)
        }
        Err(ConfigError::NewerSchema(version)) => (
            ConfigFile::default(),
            vec![format!("settings file uses schema {version}; defaults used and the file is kept")],
        ),
        Err(error) => {
            let backup = path.with_extension("toml.bad");
            let _ = fs::rename(path, &backup);
            (ConfigFile::default(), vec![format!("{error}; moved to {}", backup.display())])
        }
    }
}

/// Runs the settings migrations before the file is read. A file of schema 0 (no `schema_version`, from the
/// earlier dev builds) is copied to `settings.toml.bak-schema0-<time>`, migrated, and written back. A newer file
/// is left as it is (`parse` refuses it). Returns the text to parse and the notes for the log.
fn prepare(path: &Path, text: &str) -> Result<(String, Vec<String>), String> {
    // A file that does not parse as TOML is reported by `parse`; nothing is migrated.
    let Ok(table) = text.parse::<toml::Table>() else {
        return Ok((text.to_string(), Vec::new()));
    };
    let mut document = serde_json::to_value(&table).map_err(|error| format!("settings file: {error}"))?;
    match migrate::plan(&migrate::SETTINGS, &document) {
        Verdict::Current | Verdict::Newer { .. } => Ok((text.to_string(), Vec::new())),
        Verdict::Corrupt(message) => Err(message),
        Verdict::Migrate { from } => {
            let backup = path.with_file_name(migrate::backup_name("settings.toml", from, unix_now()));
            fs::copy(path, &backup).map_err(|error| format!("settings backup: {error}"))?;
            migrate::migrate(&migrate::SETTINGS, &mut document)?;
            let migrated: toml::Table =
                serde_json::from_value(document).map_err(|error| format!("settings migration: {error}"))?;
            let migrated_text = toml::to_string(&migrated).map_err(|error| format!("settings migration: {error}"))?;
            let temporary = path.with_extension("toml.tmp");
            fs::write(&temporary, &migrated_text).map_err(|error| error.to_string())?;
            fs::rename(&temporary, path).map_err(|error| error.to_string())?;
            Ok((
                migrated_text,
                vec![format!("settings migrated from schema {from} to {SCHEMA_VERSION}; copy kept at {}", backup.display())],
            ))
        }
    }
}

fn unix_now() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|value| value.as_secs())
        .unwrap_or(0)
}

/// Writes the file atomically (temporary file, then rename).
pub fn save(path: &Path, config: &ConfigFile) -> Result<(), ConfigError> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    let temporary = path.with_extension("toml.tmp");
    {
        let mut file = fs::File::create(&temporary)?;
        file.write_all(render(config).as_bytes())?;
        file.sync_all()?;
    }
    fs::rename(&temporary, path)?;
    Ok(())
}

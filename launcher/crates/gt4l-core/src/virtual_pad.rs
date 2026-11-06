//! TASK-185: the virtual controller's layout file, `user/virtual-pad.json`, format version 1.
//!
//! The editor on the Controls page writes this file; the game's on-screen pad reads the same file (TASK-185 contract,
//! shared with the backend agent). Coordinates are fractions of the screen in landscape:
//!
//! - `x` and `y` are the element's centre, 0..1;
//! - `size` is the element's height as a fraction of the screen height, 0..1;
//! - `width` is optional and, when present, the element's width as a fraction of the screen **width** (the default
//!   layout's L1/L2 and R1/R2 are 0.16 wide of the screen width, which is what makes them sit side by side). Without
//!   `width` the element is square: `size` of the screen height on both axes.
//!
//! The built-in layout ships as `android/virtual-pad/default-layout.json`, byte-identical to the backend's APK asset,
//! and is compiled into the launcher (the file stays the single source of truth for both agents).
//!
//! Nothing here draws anything: [`load`], [`save`], [`reset`] and the validation are pure file rules, so they are tested
//! without a launcher. A file with a newer `version` is never overwritten (the same rule as `controls.toml`).
//!
//! Keeping the format open for the next versions (several named layouts, per-context layouts, analog triggers, gyro):
//! unknown fields are ignored on read, optional fields are skipped when empty on write, and `version` gates everything
//! the launcher may not touch.

use std::collections::BTreeSet;
use std::fmt;
use std::fs;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::paths::UserDirs;

/// File name inside the user folder (`<app external files>/user/` on Android, `%APPDATA%\GT4Recomp` on Windows).
pub const FILE_NAME: &str = "virtual-pad.json";

/// The format the launcher reads and writes. A file with a larger number is left alone.
pub const FORMAT_VERSION: u32 = 1;

/// The built-in layout, identical to the backend's asset (the contract's JSON).
pub const DEFAULT_JSON: &str = include_str!("../../../../android/virtual-pad/default-layout.json");

/// The stick dead zone when an element does not name one (the contract's 0.08).
pub const DEFAULT_DEADZONE: f64 = 0.08;

/// The binds each element type accepts (the contract's field rules).
pub const BUTTON_BINDS: [&str; 12] = [
    "cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "l3", "r3", "select", "start",
];

/// The largest layout the launcher accepts (the default has 12; a hand-made one has room for more).
pub const MAX_ELEMENTS: usize = 32;

/// One element of the pad: a button, the D-pad or a stick.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Element {
    pub id: String,
    #[serde(rename = "type")]
    pub kind: String,
    pub binds: String,
    pub x: f64,
    pub y: f64,
    pub size: f64,
    /// Width as a fraction of the screen width; absent: square (`size` on both axes).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub width: Option<f64>,
    /// `false` hides the element without removing it (the editor's Hide).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub visible: Option<bool>,
    /// Overrides the layout's global opacity for this element.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub opacity: Option<f64>,
    /// Sticks only: the dead zone, 0..0.5 (`DEFAULT_DEADZONE` when absent).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub deadzone: Option<f64>,
}

impl Element {
    /// Whether the element is drawn (an absent `visible` means yes).
    pub fn is_visible(&self) -> bool {
        self.visible.unwrap_or(true)
    }

    /// The opacity the element is drawn with: its own, else the layout's.
    pub fn opacity_with(&self, layout_opacity: f64) -> f64 {
        self.opacity.unwrap_or(layout_opacity)
    }

    /// The stick dead zone, or the contract's default.
    pub fn deadzone_or_default(&self) -> f64 {
        self.deadzone.unwrap_or(DEFAULT_DEADZONE)
    }
}

/// A version 1 layout.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Layout {
    pub version: u32,
    #[serde(default = "default_name")]
    pub name: String,
    #[serde(default = "default_opacity")]
    pub opacity: f64,
    #[serde(default = "default_haptics")]
    pub haptics: bool,
    pub elements: Vec<Element>,
}

fn default_name() -> String {
    "Custom".to_string()
}

fn default_opacity() -> f64 {
    0.6
}

fn default_haptics() -> bool {
    true
}

/// One reason a layout cannot be used, with the element it names (empty for the layout itself).
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct Problem {
    /// A stable code for the log and for tests: `version`, `opacity`, `elements`, `too_many`, `id`, `duplicate_id`,
    /// `type`, `binds`, `x`, `y`, `size`, `width`, `element_opacity`, `deadzone`.
    pub code: &'static str,
    pub element: String,
    pub detail: String,
}

impl fmt::Display for Problem {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.element.is_empty() {
            write!(formatter, "{}", self.detail)
        } else {
            write!(formatter, "{} ({}): {}", self.element, self.code, self.detail)
        }
    }
}

fn problem(code: &'static str, element: &str, detail: String) -> Problem {
    Problem { code, element: element.to_string(), detail }
}

/// Whether `binds` is a value the element type accepts.
pub fn bind_allowed(kind: &str, binds: &str) -> bool {
    match kind {
        "button" => BUTTON_BINDS.contains(&binds),
        "dpad" => binds == "dpad",
        "stick" => binds == "left_stick" || binds == "right_stick",
        _ => false,
    }
}

/// Whether `kind` is one of the three element types.
pub fn kind_allowed(kind: &str) -> bool {
    matches!(kind, "button" | "dpad" | "stick")
}

/// Parses the text of a layout file. Unknown fields are ignored (they belong to a later version of the format).
pub fn parse(text: &str) -> Result<Layout, String> {
    serde_json::from_str::<Layout>(text).map_err(|error| error.to_string())
}

/// The built-in layout. The asset is compiled in, so this cannot fail in a build whose tests pass.
pub fn default_layout() -> Layout {
    parse(DEFAULT_JSON).expect("android/virtual-pad/default-layout.json is the built-in layout")
}

impl Layout {
    /// The first element with this id, if any.
    pub fn element(&self, id: &str) -> Option<&Element> {
        self.elements.iter().find(|element| element.id == id)
    }

    /// Every reason the layout cannot be used; empty means valid.
    pub fn problems(&self) -> Vec<Problem> {
        let mut out = Vec::new();
        if self.version != FORMAT_VERSION {
            out.push(problem("version", "", format!("the layout version is {}, not {FORMAT_VERSION}", self.version)));
        }
        if !self.opacity.is_finite() || !(0.0..=1.0).contains(&self.opacity) {
            out.push(problem("opacity", "", format!("the opacity {} is not between 0 and 1", self.opacity)));
        }
        if self.elements.is_empty() {
            out.push(problem("elements", "", "the layout has no elements".to_string()));
        }
        if self.elements.len() > MAX_ELEMENTS {
            out.push(problem("too_many", "", format!("the layout has {} elements; at most {MAX_ELEMENTS} are allowed", self.elements.len())));
        }
        let mut seen: BTreeSet<&str> = BTreeSet::new();
        for element in &self.elements {
            let id = element.id.as_str();
            if id.is_empty() || id.len() > 64 || id.chars().any(|character| character.is_control()) {
                out.push(problem("id", id, "the id must be 1 to 64 printable characters".to_string()));
            } else if !seen.insert(id) {
                out.push(problem("duplicate_id", id, format!("'{id}' is used by more than one element")));
            }
            if !kind_allowed(&element.kind) {
                out.push(problem("type", id, format!("'{}' is not button, dpad or stick", element.kind)));
            } else if !bind_allowed(&element.kind, &element.binds) {
                out.push(problem("binds", id, format!("'{}' is not a {kind} bind", element.binds, kind = element.kind)));
            }
            for (axis, value) in [("x", element.x), ("y", element.y)] {
                if !value.is_finite() || !(0.0..=1.0).contains(&value) {
                    out.push(problem(axis, id, format!("{axis} {value} is not between 0 and 1")));
                }
            }
            if !element.size.is_finite() || element.size <= 0.0 || element.size > 1.0 {
                out.push(problem("size", id, format!("the size {} is not above 0 and at most 1", element.size)));
            }
            if let Some(width) = element.width {
                if !width.is_finite() || width <= 0.0 || width > 1.0 {
                    out.push(problem("width", id, format!("the width {width} is not above 0 and at most 1")));
                }
            }
            if let Some(opacity) = element.opacity {
                if !opacity.is_finite() || !(0.0..=1.0).contains(&opacity) {
                    out.push(problem("element_opacity", id, format!("the opacity {opacity} is not between 0 and 1")));
                }
            }
            if let Some(deadzone) = element.deadzone {
                if element.kind != "stick" {
                    out.push(problem("deadzone", id, "only a stick has a dead zone".to_string()));
                } else if !deadzone.is_finite() || !(0.0..=0.5).contains(&deadzone) {
                    out.push(problem("deadzone", id, format!("the dead zone {deadzone} is not between 0 and 0.5")));
                }
            }
        }
        out
    }
}

/// Where the layout on screen came from.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Source {
    /// The player's `user/virtual-pad.json`.
    Player,
    /// The built-in layout (no file, or one the launcher cannot use).
    Default,
}

/// What [`load`] found.
#[derive(Clone, Debug)]
pub struct Loaded {
    pub layout: Layout,
    pub source: Source,
    /// The file is from a newer launcher: it is kept as it is, and the editor may not save over it.
    pub locked: bool,
    /// Why the built-in layout was used, or what was dropped from the player's file.
    pub notes: Vec<String>,
}

impl Loaded {
    fn fallback(notes: Vec<String>, locked: bool) -> Loaded {
        Loaded { layout: default_layout(), source: Source::Default, locked, notes }
    }
}

/// `<user>/virtual-pad.json`.
pub fn layout_path(user: &UserDirs) -> PathBuf {
    user.root().join(FILE_NAME)
}

/// The temporary file next to the layout (`virtual-pad.json.tmp`), renamed over it when the write is complete.
fn temporary_path(path: &Path) -> PathBuf {
    path.with_extension("json.tmp")
}

/// The `version` number of a layout text, without requiring the rest of the file to be readable by this launcher: a
/// newer format may change fields, and the file still must be left alone. `None` when the text is not JSON.
fn peek_version(text: &str) -> Option<u64> {
    serde_json::from_str::<serde_json::Value>(text).ok()?.get("version")?.as_u64()
}

/// The version of the layout already on disk, when it can be read. `None` for a missing or unreadable file.
fn version_on_disk(path: &Path) -> Option<u64> {
    peek_version(&fs::read_to_string(path).ok()?)
}

/// Whether the file on disk comes from a newer launcher and must be left alone.
fn on_disk_from_newer(path: &Path) -> bool {
    version_on_disk(path).is_some_and(|version| version > u64::from(FORMAT_VERSION))
}

/// Drops the elements whose bind the type does not accept (the contract: rejected, with a log line), returning the notes.
fn drop_unknown_binds(layout: &mut Layout) -> Vec<String> {
    let mut notes = Vec::new();
    layout.elements.retain(|element| {
        if kind_allowed(&element.kind) && !bind_allowed(&element.kind, &element.binds) {
            notes.push(format!("layout element '{}' was dropped: '{}' is not a {} bind", element.id, element.binds, element.kind));
            false
        } else {
            true
        }
    });
    notes
}

/// Reads the player's layout, or the built-in one when there is none or it cannot be used. Never writes anything.
pub fn load(path: &Path) -> Loaded {
    let text = match fs::read_to_string(path) {
        Ok(text) => text,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Loaded::fallback(Vec::new(), false),
        Err(error) => {
            return Loaded::fallback(vec![format!("{FILE_NAME} could not be read ({error}); the built-in layout is used")], false);
        }
    };
    if peek_version(&text).is_some_and(|version| version > u64::from(FORMAT_VERSION)) {
        let version = peek_version(&text).unwrap_or(u64::from(FORMAT_VERSION));
        return Loaded::fallback(
            vec![format!("{FILE_NAME} uses layout version {version} and is kept as it is; the built-in layout is shown")],
            true,
        );
    }
    let mut layout = match parse(&text) {
        Ok(layout) => layout,
        Err(detail) => {
            return Loaded::fallback(vec![format!("{FILE_NAME} is not a valid layout ({detail}); the built-in layout is used")], false);
        }
    };
    let notes = drop_unknown_binds(&mut layout);
    let problems = layout.problems();
    if !problems.is_empty() {
        let mut all = vec![format!("{FILE_NAME} is not a valid layout ({}); the built-in layout is used", problems[0])];
        all.extend(notes);
        return Loaded::fallback(all, false);
    }
    Loaded { layout, source: Source::Player, locked: false, notes }
}

/// Writes the layout at once: the whole file goes to `virtual-pad.json.tmp` first and is renamed over the old one, so
/// a crash or a full disk leaves either the old file or the new one, never half of each.
/// Refuses an invalid layout and a file written by a newer launcher.
pub fn save(path: &Path, layout: &Layout) -> Result<(), String> {
    if let Some(problem) = layout.problems().first() {
        return Err(format!("{FILE_NAME} was not written: {problem}"));
    }
    if on_disk_from_newer(path) {
        return Err(format!("{FILE_NAME} was written by a newer GT4Recomp and is kept as it is"));
    }
    let text = format!("{}\n", serde_json::to_string_pretty(layout).map_err(|error| error.to_string())?);
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|error| format!("{FILE_NAME}: {error}"))?;
    }
    let temporary = temporary_path(path);
    fs::write(&temporary, text).map_err(|error| format!("{FILE_NAME}: {error}"))?;
    fs::rename(&temporary, path).map_err(|error| {
        let _ = fs::remove_file(&temporary);
        format!("{FILE_NAME}: {error}")
    })
}

/// Deletes the player's file, so the game uses the built-in layout again. Returns whether a file was removed.
/// A file written by a newer launcher is kept.
pub fn reset(path: &Path) -> Result<bool, String> {
    if on_disk_from_newer(path) {
        return Err(format!("{FILE_NAME} was written by a newer GT4Recomp and is kept as it is"));
    }
    let _ = fs::remove_file(temporary_path(path));
    match fs::remove_file(path) {
        Ok(()) => Ok(true),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(false),
        Err(error) => Err(format!("{FILE_NAME}: {error}")),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU32, Ordering};

    static NEXT: AtomicU32 = AtomicU32::new(0);

    /// A folder of this test's own under the system temporary folder, removed when the test ends.
    struct Scratch(PathBuf);

    impl Scratch {
        fn new(name: &str) -> Scratch {
            let nanos = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_nanos();
            let folder = std::env::temp_dir().join(format!(
                "gt4-vpad-{name}-{}-{}-{nanos}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::Relaxed)
            ));
            fs::create_dir_all(&folder).unwrap();
            Scratch(folder)
        }

        fn file(&self) -> PathBuf {
            self.0.join(FILE_NAME)
        }
    }

    impl Drop for Scratch {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }

    #[test]
    fn the_built_in_layout_is_the_contracts_default() {
        let layout = default_layout();
        assert_eq!(layout.version, FORMAT_VERSION);
        assert_eq!(layout.name, "GT4 default");
        assert_eq!(layout.opacity, 0.6);
        assert!(layout.haptics);
        assert_eq!(layout.elements.len(), 12);
        let ids: Vec<&str> = layout.elements.iter().map(|element| element.id.as_str()).collect();
        for id in ["lstick", "dpad", "cross", "square", "circle", "triangle", "l1", "r1", "l2", "r2", "select", "start"] {
            assert!(ids.contains(&id), "{id}");
        }
        let stick = layout.element("lstick").unwrap();
        assert_eq!((stick.kind.as_str(), stick.binds.as_str()), ("stick", "left_stick"));
        assert_eq!((stick.x, stick.y, stick.size), (0.13, 0.70, 0.32));
        assert_eq!(stick.deadzone_or_default(), DEFAULT_DEADZONE);
        let l1 = layout.element("l1").unwrap();
        assert_eq!((l1.size, l1.width), (0.12, Some(0.16)));
        assert!(layout.problems().is_empty(), "{:?}", layout.problems());
        assert_eq!(serde_json::to_string(&Source::Default).unwrap(), "\"default\"");
    }

    #[test]
    fn a_missing_file_gives_the_built_in_layout() {
        let scratch = Scratch::new("missing");
        let loaded = load(&scratch.file());
        assert_eq!(loaded.source, Source::Default);
        assert_eq!(loaded.layout, default_layout());
        assert!(!loaded.locked);
        assert!(loaded.notes.is_empty(), "{:?}", loaded.notes);
    }

    #[test]
    fn the_players_file_is_read_and_reported_as_theirs() {
        let scratch = Scratch::new("player");
        let mut layout = default_layout();
        layout.name = "Mine".to_string();
        layout.opacity = 0.35;
        layout.haptics = false;
        layout.elements[0].x = 0.4;
        layout.elements[0].y = 0.5;
        layout.elements[0].opacity = Some(0.2);
        layout.elements[0].visible = Some(false);
        fs::write(scratch.file(), serde_json::to_string_pretty(&layout).unwrap()).unwrap();
        let loaded = load(&scratch.file());
        assert_eq!(loaded.source, Source::Player);
        assert_eq!(loaded.layout, layout);
        assert!(!loaded.locked);
        assert!(loaded.notes.is_empty(), "{:?}", loaded.notes);
    }

    #[test]
    fn unknown_fields_belong_to_a_later_version_and_are_ignored() {
        let text = r#"{
          "version": 1, "name": "Mine", "opacity": 0.5, "haptics": true, "context": "race",
          "elements": [
            { "id": "cross", "type": "button", "binds": "cross", "x": 0.5, "y": 0.5, "size": 0.1,
              "pressure": "analog", "future": [1, 2] }
          ]
        }"#;
        let layout = parse(text).unwrap();
        assert!(layout.problems().is_empty(), "{:?}", layout.problems());
        let scratch = Scratch::new("unknown");
        fs::write(scratch.file(), text).unwrap();
        let loaded = load(&scratch.file());
        assert_eq!(loaded.source, Source::Player);
        assert_eq!(loaded.layout.elements.len(), 1);
    }

    #[test]
    fn an_unknown_bind_is_dropped_with_a_note_and_the_rest_is_kept() {
        let scratch = Scratch::new("bind");
        let mut layout = default_layout();
        layout.elements[2].binds = "hyperdrive".to_string(); // cross
        fs::write(scratch.file(), serde_json::to_string(&layout).unwrap()).unwrap();
        let loaded = load(&scratch.file());
        assert_eq!(loaded.source, Source::Player);
        assert!(loaded.layout.element("cross").is_none());
        assert_eq!(loaded.layout.elements.len(), 11);
        assert!(loaded.notes.iter().any(|note| note.contains("hyperdrive")), "{:?}", loaded.notes);
        // The file itself was not touched.
        assert!(fs::read_to_string(scratch.file()).unwrap().contains("hyperdrive"));
    }

    #[test]
    fn a_file_without_usable_elements_falls_back_to_the_built_in_layout() {
        let scratch = Scratch::new("empty");
        fs::write(scratch.file(), r#"{"version":1,"elements":[]}"#).unwrap();
        let loaded = load(&scratch.file());
        assert_eq!(loaded.source, Source::Default);
        assert!(loaded.notes.iter().any(|note| note.contains("no elements")), "{:?}", loaded.notes);
    }

    #[test]
    fn a_damaged_file_falls_back_and_is_kept() {
        let scratch = Scratch::new("damaged");
        fs::write(scratch.file(), "{ not json").unwrap();
        let loaded = load(&scratch.file());
        assert_eq!(loaded.source, Source::Default);
        assert_eq!(loaded.layout, default_layout());
        assert_eq!(fs::read_to_string(scratch.file()).unwrap(), "{ not json");
    }

    #[test]
    fn a_newer_file_is_locked_and_never_overwritten_or_deleted() {
        let scratch = Scratch::new("newer");
        let text = r#"{"version":7,"name":"Future","opacity":0.6,"haptics":true,"elements":[{"id":"a","type":"button","binds":"cross","x":0.5,"y":0.5,"size":0.1}]}"#;
        fs::write(scratch.file(), text).unwrap();
        let loaded = load(&scratch.file());
        assert_eq!(loaded.source, Source::Default);
        assert!(loaded.locked);
        assert!(loaded.notes.iter().any(|note| note.contains("version 7")), "{:?}", loaded.notes);
        assert!(save(&scratch.file(), &default_layout()).is_err());
        assert!(reset(&scratch.file()).is_err());
        assert_eq!(fs::read_to_string(scratch.file()).unwrap(), text);
    }

    #[test]
    fn save_writes_the_layout_and_load_reads_it_back() {
        let scratch = Scratch::new("save");
        let mut layout = default_layout();
        layout.name = "Living room".to_string();
        layout.opacity = 0.45;
        layout.elements[1].visible = Some(false); // the D-pad
        layout.elements[0].deadzone = Some(0.12);
        save(&scratch.file(), &layout).unwrap();
        let text = fs::read_to_string(scratch.file()).unwrap();
        assert!(text.ends_with('\n'));
        assert!(text.contains("\"visible\": false"), "{text}");
        assert!(!text.contains("null"), "optional fields are left out, not written as null: {text}");
        let loaded = load(&scratch.file());
        assert_eq!(loaded.source, Source::Player);
        assert_eq!(loaded.layout, layout);
        // A second save replaces the file atomically, and no temporary file is left behind.
        layout.opacity = 0.7;
        save(&scratch.file(), &layout).unwrap();
        assert_eq!(load(&scratch.file()).layout.opacity, 0.7);
        assert!(!temporary_path(&scratch.file()).exists());
    }

    #[test]
    fn save_creates_the_user_folder() {
        let scratch = Scratch::new("folder");
        let path = scratch.0.join("user").join(FILE_NAME);
        save(&path, &default_layout()).unwrap();
        assert!(path.exists());
        assert_eq!(load(&path).source, Source::Player);
    }

    #[test]
    fn save_refuses_an_invalid_layout() {
        let scratch = Scratch::new("refuse");
        let cases: [(&str, fn(&mut Layout)); 7] = [
            ("version", |layout| layout.version = 2),
            ("opacity", |layout| layout.opacity = 1.4),
            ("type", |layout| layout.elements[0].kind = "wheel".to_string()),
            ("binds", |layout| layout.elements[0].binds = "dpad".to_string()),
            ("x", |layout| layout.elements[0].x = 1.2),
            ("size", |layout| layout.elements[0].size = 0.0),
            ("deadzone", |layout| layout.elements[2].deadzone = Some(0.1)),
        ];
        for (code, change) in cases {
            let mut layout = default_layout();
            change(&mut layout);
            let error = save(&scratch.file(), &layout).expect_err(code);
            assert!(error.contains(code) || error.contains("not a"), "{code}: {error}");
            assert!(!scratch.file().exists(), "{code} wrote a file");
        }
        // Duplicate ids and an element with no id are refused too.
        let mut duplicate = default_layout();
        duplicate.elements[1].id = duplicate.elements[0].id.clone();
        assert!(duplicate.problems().iter().any(|problem| problem.code == "duplicate_id"));
        let mut unnamed = default_layout();
        unnamed.elements[0].id = String::new();
        assert!(unnamed.problems().iter().any(|problem| problem.code == "id"));
        // Too many elements.
        let mut many = default_layout();
        while many.elements.len() <= MAX_ELEMENTS {
            let mut copy = many.elements[0].clone();
            copy.id = format!("copy{}", many.elements.len());
            many.elements.push(copy);
        }
        assert!(many.problems().iter().any(|problem| problem.code == "too_many"));
    }

    #[test]
    fn every_problem_names_its_element() {
        let mut layout = default_layout();
        layout.elements[4].size = -1.0; // circle
        let problems = layout.problems();
        assert_eq!(problems.len(), 1);
        assert_eq!(problems[0].element, "circle");
        assert_eq!(problems[0].code, "size");
        assert!(problems[0].to_string().contains("circle"), "{}", problems[0]);
    }

    #[test]
    fn reset_removes_the_file_and_reports_whether_there_was_one() {
        let scratch = Scratch::new("reset");
        assert!(!reset(&scratch.file()).unwrap());
        save(&scratch.file(), &default_layout()).unwrap();
        assert!(reset(&scratch.file()).unwrap());
        assert!(!scratch.file().exists());
        let loaded = load(&scratch.file());
        assert_eq!(loaded.source, Source::Default);
        assert!(loaded.notes.is_empty());
    }

    #[test]
    fn reset_cleans_a_temporary_file_left_by_an_interrupted_save() {
        let scratch = Scratch::new("leftover");
        fs::write(temporary_path(&scratch.file()), "{ half").unwrap();
        assert!(!reset(&scratch.file()).unwrap());
        assert!(!temporary_path(&scratch.file()).exists());
    }

    #[test]
    fn the_location_is_the_user_folder() {
        let user = UserDirs::new("C:/somewhere/user", crate::paths::UserRootSource::Override);
        assert_eq!(layout_path(&user), Path::new("C:/somewhere/user").join("virtual-pad.json"));
        assert_eq!(FILE_NAME, "virtual-pad.json");
    }

    #[test]
    fn the_layout_keeps_its_shape_for_the_page() {
        let layout = default_layout();
        let value = serde_json::to_value(&layout).unwrap();
        assert_eq!(value["version"], 1);
        assert_eq!(value["elements"][0]["type"], "stick");
        assert_eq!(value["elements"][0]["binds"], "left_stick");
        assert!(value["elements"][0].get("width").is_none(), "an absent width is not written");
        assert!(value["elements"][0].get("visible").is_none(), "an absent visible is not written");
    }

    #[test]
    fn the_stick_and_button_rules_are_the_contracts() {
        assert!(bind_allowed("stick", "left_stick"));
        assert!(bind_allowed("stick", "right_stick"));
        assert!(!bind_allowed("stick", "dpad"));
        assert!(bind_allowed("dpad", "dpad"));
        assert!(!bind_allowed("dpad", "cross"));
        for button in BUTTON_BINDS {
            assert!(bind_allowed("button", button), "{button}");
        }
        assert!(!bind_allowed("button", "dpad"));
        assert!(!bind_allowed("button", "left_stick"));
        assert!(!bind_allowed("trigger", "l2"));
        assert!(kind_allowed("button") && kind_allowed("dpad") && kind_allowed("stick"));
        assert!(!kind_allowed("wheel"));
    }
}

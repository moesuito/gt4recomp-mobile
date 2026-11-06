//! Custom texture folder layout (TASK-093, owner 2026-10-22). gt4-gs 1.2.1 reads the `Textures` folder itself, so a
//! pack goes straight into it. Older gt4-gs versions read only `Textures\replacements`, so this module moves the
//! contents of an old `replacements` folder up into `Textures`, but only once gt4-gs 1.2.1 or later is in use (an
//! older renderer would stop loading the files otherwise).
//!
//! The rules of [`flatten_replacements`]:
//! - Runs only when the gt4-gs in use (`current.txt` under the renderer root) is at least [`FLATTEN_MINIMUM`].
//! - Runs only when `Textures\replacements` is a real folder (not a junction, symlink or other reparse point).
//! - Each direct child moves with a rename into `Textures` (same folder, same volume). A child whose name already
//!   exists in `Textures` is never overwritten or merged: it stays in `replacements` and the log says so.
//! - The `replacements` folder is removed only when it is empty, after the moves.
//! - The first error stops the moves; the rest stays in place and the next start tries again. Nothing is deleted
//!   except an empty `replacements` folder.
//! - Every target is checked with [`crate::guard::check_write`] first.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use crate::guard;
use crate::version::Version;

/// Name of the old texture subfolder that gt4-gs up to 1.2.0 read.
pub const REPLACEMENTS_FOLDER_NAME: &str = "replacements";

/// Lowest gt4-gs version that reads `Textures` itself, so the old folder may be flattened.
pub const FLATTEN_MINIMUM: Version = Version(1, 2, 1);

/// The file operations of the flatten, so tests can fail them.
pub struct FlattenOps<'a> {
    pub rename: &'a dyn Fn(&Path, &Path) -> io::Result<()>,
    pub remove_dir: &'a dyn Fn(&Path) -> io::Result<()>,
}

fn real_rename(from: &Path, to: &Path) -> io::Result<()> {
    fs::rename(from, to)
}

fn real_remove_dir(folder: &Path) -> io::Result<()> {
    fs::remove_dir(folder)
}

impl FlattenOps<'static> {
    /// The operations of the launcher.
    pub fn real() -> FlattenOps<'static> {
        FlattenOps { rename: &real_rename, remove_dir: &real_remove_dir }
    }
}

/// What the flatten did, as log lines for the launcher log.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum FlattenOutcome {
    /// gt4-gs below [`FLATTEN_MINIMUM`], or no readable `current.txt`: nothing moved.
    RendererTooOld(String),
    /// No `replacements` folder: nothing to do.
    Absent,
    /// `replacements` is a junction, symlink or other reparse point: left alone.
    Refused(String),
    /// The move ran. `moved` children went up, `kept` names stayed (their name was taken), `error` stopped the rest.
    Ran { moved: usize, kept: Vec<String>, removed_folder: bool, error: Option<String> },
    /// The folder could not be inspected or a guard refused a target.
    Failed(String),
}

impl FlattenOutcome {
    /// One log line per fact, for the launcher log.
    pub fn log_lines(&self) -> Vec<String> {
        match self {
            FlattenOutcome::RendererTooOld(reason) => {
                vec![format!("textures: not flattened ({reason}); replacements stays in use until gt4-gs 1.2.1")]
            }
            FlattenOutcome::Absent => Vec::new(),
            FlattenOutcome::Refused(reason) => vec![format!("textures: replacements skipped: {reason}")],
            FlattenOutcome::Failed(error) => vec![format!("textures: replacements not flattened: {error}")],
            FlattenOutcome::Ran { moved, kept, removed_folder, error } => {
                let mut lines = Vec::new();
                if *moved > 0 {
                    lines.push(format!("textures: moved {moved} item(s) from replacements into Textures"));
                }
                for name in kept {
                    lines.push(format!("textures: {name} already exists in Textures; it stays in replacements"));
                }
                if let Some(error) = error {
                    lines.push(format!("textures: flatten stopped, the rest stays in replacements; it runs again at the next start: {error}"));
                }
                if *removed_folder {
                    lines.push("textures: removed the empty replacements folder".to_string());
                }
                lines
            }
        }
    }
}

/// The gt4-gs version named by `current.txt` under the renderer root, if it parses.
pub fn renderer_version_in_use(renderer_root: &Path) -> Option<Version> {
    let text = fs::read_to_string(renderer_root.join("current.txt")).ok()?;
    Version::parse(text.trim())
}

/// Whether a path is a reparse point (junction, symlink, mount point or other): never followed by the flatten.
fn is_reparse_point(metadata: &fs::Metadata) -> bool {
    if metadata.file_type().is_symlink() {
        return true;
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::MetadataExt;
        const FILE_ATTRIBUTE_REPARSE_POINT: u32 = 0x400;
        if metadata.file_attributes() & FILE_ATTRIBUTE_REPARSE_POINT != 0 {
            return true;
        }
    }
    false
}

/// Moves the direct children of `Textures\replacements` up into `Textures` and removes the folder when it is empty.
/// `data_root` is the data folder the write guard checks against. Never panics; every problem is in the outcome.
pub fn flatten_replacements(textures: &Path, renderer_root: &Path, data_root: &Path, ops: &FlattenOps<'_>) -> FlattenOutcome {
    match renderer_version_in_use(renderer_root) {
        Some(version) if version >= FLATTEN_MINIMUM => {}
        Some(version) => return FlattenOutcome::RendererTooOld(format!("gt4-gs {} is in use", version.text())),
        None => return FlattenOutcome::RendererTooOld("no gt4-gs version in use".to_string()),
    }
    let folder = textures.join(REPLACEMENTS_FOLDER_NAME);
    let metadata = match fs::symlink_metadata(&folder) {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return FlattenOutcome::Absent,
        Err(error) => return FlattenOutcome::Failed(format!("{}: {error}", folder.display())),
    };
    if is_reparse_point(&metadata) {
        return FlattenOutcome::Refused(format!("{} is a junction or link", folder.display()));
    }
    if !metadata.is_dir() {
        return FlattenOutcome::Refused(format!("{} is not a folder", folder.display()));
    }
    let entries = match fs::read_dir(&folder) {
        Ok(entries) => entries,
        Err(error) => return FlattenOutcome::Failed(format!("{}: {error}", folder.display())),
    };
    let mut children = Vec::new();
    for entry in entries {
        match entry {
            Ok(entry) => children.push(entry.path()),
            Err(error) => return FlattenOutcome::Failed(format!("{}: {error}", folder.display())),
        }
    }
    children.sort();

    let mut moved = 0;
    let mut kept = Vec::new();
    let mut error = None;
    for source in &children {
        let Some(name) = source.file_name() else { continue };
        let destination = textures.join(name);
        if fs::symlink_metadata(&destination).is_ok() {
            kept.push(name.to_string_lossy().into_owned());
            continue;
        }
        if let Err(reason) = guard::check_write(data_root, &destination) {
            error = Some(reason);
            break;
        }
        if let Err(reason) = (ops.rename)(source, &destination) {
            error = Some(format!("{} to {}: {reason}", source.display(), destination.display()));
            break;
        }
        moved += 1;
    }

    // Removed only when empty: a kept child, a failed move or an unreadable entry leaves the folder in place.
    let mut removed_folder = false;
    if error.is_none() && kept.is_empty() {
        let still_empty = fs::read_dir(&folder).map(|mut rest| rest.next().is_none()).unwrap_or(false);
        if still_empty {
            if let Err(reason) = guard::check_write(data_root, &folder).and_then(|()| (ops.remove_dir)(&folder).map_err(|e| e.to_string())) {
                error = Some(format!("{}: {reason}", folder.display()));
            } else {
                removed_folder = true;
            }
        }
    }
    FlattenOutcome::Ran { moved, kept, removed_folder, error }
}

/// Name of the shader cache folder the renderer keeps (`GT4_HW_SHADER_CACHE`), beside the default `Textures` folder.
pub const SHADER_CACHE_FOLDER_NAME: &str = "shader-cache";

/// Why a folder the player chose cannot be the textures folder (TASK-157). `message_id` names the launcher text.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TexturesPathError {
    NotAbsolute,
    NotFound,
    NotAFolder,
    /// The data folder itself: its other contents would be taken for textures.
    DataRoot,
    /// Inside the renderer folder (`<data>\gs`), which the launcher replaces on updates.
    InsideRenderer,
}

impl TexturesPathError {
    /// The id of the text in the locale files (`rust.<id>`).
    pub fn message_id(self) -> &'static str {
        match self {
            TexturesPathError::NotAbsolute => "textures.invalid_not_absolute",
            TexturesPathError::NotFound => "textures.invalid_not_found",
            TexturesPathError::NotAFolder => "textures.invalid_not_folder",
            TexturesPathError::DataRoot => "textures.invalid_data_root",
            TexturesPathError::InsideRenderer => "textures.invalid_renderer",
        }
    }
}

/// `path` with its separators tidied (a trailing one dropped, `.` removed), or the reason it cannot be the textures
/// folder. Nothing is created and nothing is written.
pub fn check_textures_path(path: &Path, data_root: &Path) -> Result<PathBuf, TexturesPathError> {
    if !path.is_absolute() {
        return Err(TexturesPathError::NotAbsolute);
    }
    let tidy: PathBuf = path.components().collect();
    let metadata = fs::metadata(&tidy).map_err(|_| TexturesPathError::NotFound)?;
    if !metadata.is_dir() {
        return Err(TexturesPathError::NotAFolder);
    }
    // Compare resolved paths (`..`, links and case), so a roundabout spelling cannot slip past.
    let resolved = fs::canonicalize(&tidy).unwrap_or_else(|_| tidy.clone());
    let data = fs::canonicalize(data_root).unwrap_or_else(|_| data_root.to_path_buf());
    if same_folder(&resolved, &data) {
        return Err(TexturesPathError::DataRoot);
    }
    if is_inside(&resolved, &data.join("gs")) {
        return Err(TexturesPathError::InsideRenderer);
    }
    Ok(tidy)
}

fn lowered(path: &Path) -> PathBuf {
    path.to_string_lossy().to_lowercase().into()
}

fn same_folder(a: &Path, b: &Path) -> bool {
    lowered(a) == lowered(b)
}

/// Whether `path` is `folder` or below it.
fn is_inside(path: &Path, folder: &Path) -> bool {
    lowered(path).starts_with(lowered(folder))
}

/// The folder the renderers read custom textures from in one session, and where the CoreGS 2.0 shader cache goes.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TextureLocation {
    pub folder: PathBuf,
    /// `GT4_HW_SHADER_CACHE`. `None`: the default folder has no parent, so the renderer finds its own place.
    pub shader_cache: Option<PathBuf>,
    /// The player's own folder (never written to by the launcher).
    pub custom: bool,
}

impl TextureLocation {
    /// The default folder: the shader cache stays beside it, where the caches of earlier versions are.
    pub fn default_folder(folder: &Path) -> TextureLocation {
        TextureLocation {
            folder: folder.to_path_buf(),
            shader_cache: folder.parent().map(|user| user.join(SHADER_CACHE_FOLDER_NAME)),
            custom: false,
        }
    }

    /// A folder the player chose: the shader cache goes into the data folder, never beside the player's folder.
    pub fn custom_folder(folder: &Path, data_root: &Path) -> TextureLocation {
        TextureLocation {
            folder: folder.to_path_buf(),
            shader_cache: Some(data_root.join(SHADER_CACHE_FOLDER_NAME)),
            custom: true,
        }
    }
}

/// Where this session reads textures: the chosen folder when it is usable, else `default_folder`. The second value is
/// the launcher log line when the choice could not be used (the choice itself stays stored).
pub fn resolve_location(default_folder: &Path, chosen: Option<&Path>, data_root: &Path) -> (TextureLocation, Option<String>) {
    let Some(chosen) = chosen else {
        return (TextureLocation::default_folder(default_folder), None);
    };
    match check_textures_path(chosen, data_root) {
        Ok(folder) => (TextureLocation::custom_folder(&folder, data_root), None),
        Err(TexturesPathError::NotFound) => (
            TextureLocation::default_folder(default_folder),
            Some(format!("textures: {} not found; using {}", chosen.display(), default_folder.display())),
        ),
        Err(error) => (
            TextureLocation::default_folder(default_folder),
            Some(format!("textures: {} cannot be used ({}); using {}", chosen.display(), error.message_id(), default_folder.display())),
        ),
    }
}

/// The folder a phone's picked texture folder is copied into (under the launcher's data folder). It is the app's own
/// copy, laid out as PCSX2 packs are; a later start reads it as it is.
pub const PHONE_SYNC_FOLDER_NAME: &str = "textures-sync";

/// The file the sync writes with the name the player saw in the phone's picker, so the Settings page can show plain
/// words instead of the app's internal path.
pub const SOURCE_NAME_FILE: &str = ".gt4-source";

/// What a synced phone folder held, as [`prepare_phone_textures`] found it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PhoneLayout {
    /// The picked folder already was a textures root (it held the game's serial folder).
    Root,
    /// The picked folder was the game's own folder (it held `replacements`): the same files, now under the serial.
    Nested,
    /// Neither the serial folder nor `replacements` was found: the pack will not load, and the caller says so.
    Nothing,
}

/// Turns the copy of a folder picked on the phone into the layout the ARMSX2 GS reads: `<root>/<serial>/replacements`
/// (PCSX2's texture pack layout, the same the PC's GSdx path uses). A folder that already holds the serial folder is
/// kept as it is; one that holds `replacements` at its top (the player picked the game's own folder) has its contents
/// moved under the serial folder. Nothing is deleted; a move that fails leaves the file where it was.
pub fn prepare_phone_textures(root: &Path, serial: &str) -> io::Result<PhoneLayout> {
    if root.join(serial).is_dir() {
        return Ok(PhoneLayout::Root);
    }
    if !root.join(REPLACEMENTS_FOLDER_NAME).is_dir() {
        return Ok(PhoneLayout::Nothing);
    }
    let game = root.join(serial);
    fs::create_dir_all(&game)?;
    let mut entries: Vec<_> = fs::read_dir(root)?.collect::<Result<_, _>>()?;
    entries.sort_by_key(|entry| entry.file_name());
    for entry in entries {
        if entry.file_name() == std::ffi::OsStr::new(serial) {
            continue;
        }
        fs::rename(entry.path(), game.join(entry.file_name()))?;
    }
    Ok(PhoneLayout::Nested)
}

/// How many files sit under `<root>/<serial>/replacements`, recursively: the count the Settings page logs after a pick
/// and the GS logs when it starts (`game.log`).
pub fn count_replacements(root: &Path, serial: &str) -> u64 {
    fn walk(folder: &Path, count: &mut u64) {
        let Ok(entries) = fs::read_dir(folder) else {
            return;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            match fs::metadata(&path) {
                Ok(metadata) if metadata.is_dir() => walk(&path, count),
                Ok(metadata) if metadata.is_file() => *count += 1,
                _ => {}
            }
        }
    }
    let mut count = 0;
    walk(&root.join(serial).join(REPLACEMENTS_FOLDER_NAME), &mut count);
    count
}

/// The name the player saw for a synced folder, when the sync wrote it.
pub fn source_name(root: &Path) -> Option<String> {
    let text = fs::read_to_string(root.join(SOURCE_NAME_FILE)).ok()?;
    let name = text.trim();
    (!name.is_empty()).then(|| name.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::paths::TEXTURES_FOLDER_NAME;

    fn scratch(name: &str) -> std::path::PathBuf {
        let path = std::env::temp_dir().join(format!("gt4l-core-textures-{}-{name}", std::process::id()));
        let _ = fs::remove_dir_all(&path);
        fs::create_dir_all(&path).unwrap();
        path
    }

    fn set_renderer(root: &Path, version: &str) {
        fs::create_dir_all(root).unwrap();
        fs::write(root.join("current.txt"), version).unwrap();
    }

    fn real() -> FlattenOps<'static> {
        FlattenOps::real()
    }

    #[test]
    fn children_move_up_and_the_empty_folder_is_removed() {
        let base = scratch("moves");
        let data = base.join("data");
        let textures = data.join(TEXTURES_FOLDER_NAME);
        let folder = textures.join(REPLACEMENTS_FOLDER_NAME);
        fs::create_dir_all(folder.join("SCUS-97328")).unwrap();
        fs::write(folder.join("pack.txt"), b"pack").unwrap();
        fs::write(folder.join("SCUS-97328").join("a.png"), b"tex").unwrap();
        set_renderer(&data.join("gs"), "1.2.1");

        let outcome = flatten_replacements(&textures, &data.join("gs"), &data, &real());
        assert_eq!(outcome, FlattenOutcome::Ran { moved: 2, kept: vec![], removed_folder: true, error: None });
        assert_eq!(fs::read(textures.join("pack.txt")).unwrap(), b"pack");
        assert_eq!(fs::read(textures.join("SCUS-97328").join("a.png")).unwrap(), b"tex");
        assert!(!folder.exists());
    }

    #[test]
    fn a_name_collision_keeps_that_child_and_the_folder() {
        let base = scratch("collision");
        let data = base.join("data");
        let textures = data.join(TEXTURES_FOLDER_NAME);
        let folder = textures.join(REPLACEMENTS_FOLDER_NAME);
        fs::create_dir_all(&folder).unwrap();
        fs::create_dir_all(&textures).unwrap();
        fs::write(folder.join("same.png"), b"old").unwrap();
        fs::write(textures.join("same.png"), b"new").unwrap();
        fs::write(folder.join("other.png"), b"other").unwrap();
        set_renderer(&data.join("gs"), "1.2.1");

        let outcome = flatten_replacements(&textures, &data.join("gs"), &data, &real());
        assert_eq!(
            outcome,
            FlattenOutcome::Ran { moved: 1, kept: vec!["same.png".to_string()], removed_folder: false, error: None }
        );
        assert_eq!(fs::read(textures.join("same.png")).unwrap(), b"new", "the Textures file is not overwritten");
        assert_eq!(fs::read(folder.join("same.png")).unwrap(), b"old", "the colliding child stays");
        assert!(folder.is_dir());
    }

    #[test]
    fn gt4_gs_1_2_0_or_no_current_file_moves_nothing() {
        let base = scratch("old-renderer");
        let data = base.join("data");
        let textures = data.join(TEXTURES_FOLDER_NAME);
        let folder = textures.join(REPLACEMENTS_FOLDER_NAME);
        fs::create_dir_all(&folder).unwrap();
        fs::write(folder.join("pack.txt"), b"pack").unwrap();

        set_renderer(&data.join("gs"), "1.2.0");
        let outcome = flatten_replacements(&textures, &data.join("gs"), &data, &real());
        assert!(matches!(outcome, FlattenOutcome::RendererTooOld(_)), "{outcome:?}");

        let missing = data.join("no-gs");
        let outcome = flatten_replacements(&textures, &missing, &data, &real());
        assert!(matches!(outcome, FlattenOutcome::RendererTooOld(_)), "{outcome:?}");
        assert_eq!(fs::read(folder.join("pack.txt")).unwrap(), b"pack");
        assert!(!textures.join("pack.txt").exists());
    }

    #[test]
    fn a_missing_replacements_folder_is_a_no_op() {
        let base = scratch("missing");
        let data = base.join("data");
        let textures = data.join(TEXTURES_FOLDER_NAME);
        fs::create_dir_all(&textures).unwrap();
        set_renderer(&data.join("gs"), "1.2.1");
        assert_eq!(flatten_replacements(&textures, &data.join("gs"), &data, &real()), FlattenOutcome::Absent);
        assert!(FlattenOutcome::Absent.log_lines().is_empty());
    }

    #[test]
    fn a_failing_rename_deletes_nothing_and_reports_one_error() {
        let base = scratch("failing");
        let data = base.join("data");
        let textures = data.join(TEXTURES_FOLDER_NAME);
        let folder = textures.join(REPLACEMENTS_FOLDER_NAME);
        fs::create_dir_all(&folder).unwrap();
        fs::write(folder.join("a.png"), b"a").unwrap();
        fs::write(folder.join("b.png"), b"b").unwrap();
        set_renderer(&data.join("gs"), "1.2.1");

        let fail = |_: &Path, _: &Path| -> io::Result<()> { Err(io::Error::other("locked")) };
        let remove = |_: &Path| -> io::Result<()> { panic!("nothing may be removed after a failed rename") };
        let ops = FlattenOps { rename: &fail, remove_dir: &remove };
        let outcome = flatten_replacements(&textures, &data.join("gs"), &data, &ops);
        match &outcome {
            FlattenOutcome::Ran { moved: 0, kept, removed_folder: false, error: Some(_) } => assert!(kept.is_empty()),
            other => panic!("unexpected {other:?}"),
        }
        assert_eq!(outcome.log_lines().iter().filter(|line| line.contains("flatten stopped")).count(), 1);
        assert_eq!(fs::read(folder.join("a.png")).unwrap(), b"a");
        assert_eq!(fs::read(folder.join("b.png")).unwrap(), b"b");
        assert!(!textures.join("a.png").exists());
    }

    #[test]
    fn a_folder_that_is_a_file_is_refused() {
        let base = scratch("file");
        let data = base.join("data");
        let textures = data.join(TEXTURES_FOLDER_NAME);
        fs::create_dir_all(&textures).unwrap();
        fs::write(textures.join(REPLACEMENTS_FOLDER_NAME), b"not a folder").unwrap();
        set_renderer(&data.join("gs"), "1.2.1");
        assert!(matches!(flatten_replacements(&textures, &data.join("gs"), &data, &real()), FlattenOutcome::Refused(_)));
        assert!(textures.join(REPLACEMENTS_FOLDER_NAME).is_file());
    }

    #[test]
    fn an_empty_replacements_folder_is_removed_without_moves() {
        let base = scratch("empty");
        let data = base.join("data");
        let textures = data.join(TEXTURES_FOLDER_NAME);
        fs::create_dir_all(textures.join(REPLACEMENTS_FOLDER_NAME)).unwrap();
        set_renderer(&data.join("gs"), "1.2.1");
        let outcome = flatten_replacements(&textures, &data.join("gs"), &data, &real());
        assert_eq!(outcome, FlattenOutcome::Ran { moved: 0, kept: vec![], removed_folder: true, error: None });
        assert!(!textures.join(REPLACEMENTS_FOLDER_NAME).exists());
    }

    #[test]
    fn a_pack_that_already_has_the_serial_folder_is_kept() {
        let base = scratch("phone-root");
        let pack = base.join("textures");
        fs::create_dir_all(pack.join("SCUS-97328").join(REPLACEMENTS_FOLDER_NAME)).unwrap();
        fs::write(pack.join("SCUS-97328").join(REPLACEMENTS_FOLDER_NAME).join("a.png"), b"tex").unwrap();
        assert_eq!(prepare_phone_textures(&pack, "SCUS-97328").unwrap(), PhoneLayout::Root);
        assert!(pack.join("SCUS-97328").join(REPLACEMENTS_FOLDER_NAME).join("a.png").is_file());
    }

    #[test]
    fn a_pack_that_is_the_game_folder_is_moved_under_the_serial() {
        let base = scratch("phone-nest");
        let pack = base.join("SCUS-97328");
        fs::create_dir_all(pack.join(REPLACEMENTS_FOLDER_NAME).join("sub")).unwrap();
        fs::write(pack.join(REPLACEMENTS_FOLDER_NAME).join("a.png"), b"tex").unwrap();
        fs::write(pack.join(REPLACEMENTS_FOLDER_NAME).join("sub").join("b.png"), b"tex").unwrap();
        assert_eq!(prepare_phone_textures(&pack, "SCUS-97328").unwrap(), PhoneLayout::Nested);
        let replacements = pack.join("SCUS-97328").join(REPLACEMENTS_FOLDER_NAME);
        assert!(replacements.join("a.png").is_file());
        assert!(replacements.join("sub").join("b.png").is_file());
    }

    #[test]
    fn a_folder_with_neither_the_serial_nor_replacements_is_reported() {
        let base = scratch("phone-nothing");
        let pack = base.join("textures");
        fs::create_dir_all(pack.join("something")).unwrap();
        fs::write(pack.join("something").join("a.png"), b"tex").unwrap();
        assert_eq!(prepare_phone_textures(&pack, "SCUS-97328").unwrap(), PhoneLayout::Nothing);
    }

    #[test]
    fn every_replacement_file_is_counted_recursively() {
        let base = scratch("phone-count");
        let replacements = base.join("SCUS-97328").join(REPLACEMENTS_FOLDER_NAME);
        fs::create_dir_all(replacements.join("sub")).unwrap();
        fs::write(replacements.join("a.png"), b"tex").unwrap();
        fs::write(replacements.join("sub").join("b.png"), b"tex").unwrap();
        fs::write(base.join("SCUS-97328").join("notes.txt"), b"not a replacement").unwrap();
        assert_eq!(count_replacements(&base, "SCUS-97328"), 2);
    }

    #[test]
    fn the_source_name_is_read_back_trimmed_and_empty_is_none() {
        let base = scratch("phone-name");
        assert_eq!(source_name(&base), None);
        fs::write(base.join(SOURCE_NAME_FILE), b"  Texturas do GT4  \n").unwrap();
        assert_eq!(source_name(&base).as_deref(), Some("Texturas do GT4"));
        fs::write(base.join(SOURCE_NAME_FILE), b"  \n").unwrap();
        assert_eq!(source_name(&base), None);
    }

}

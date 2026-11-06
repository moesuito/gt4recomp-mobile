//! The multi-game rules of the launcher core (TASK-101): the folders of each game, the stored selection, the schema 8
//! step and the launch line of each game's program.

use std::fs;
use std::path::{Path, PathBuf};

use gt4l_core::config::{self, ConfigFile};
use gt4l_core::games::{self, GT4, SPEC2};
use gt4l_core::launch::{self, LaunchInputs};
use gt4l_core::paths::{DataDirs, UserDirs, UserRootSource};
use gt4l_core::settings::Settings;

fn scratch(name: &str) -> PathBuf {
    let path = std::env::temp_dir().join(format!("gt4l-multi-game-{}-{name}", std::process::id()));
    let _ = fs::remove_dir_all(&path);
    fs::create_dir_all(&path).unwrap();
    path
}

fn receipt(folder: &Path) {
    fs::create_dir_all(folder.join("prepared")).unwrap();
    fs::write(folder.join("prepared").join("prepare.json"), b"{}").unwrap();
}

#[test]
fn each_game_has_its_own_folders_and_shares_the_rest() {
    let data = DataDirs::new("D:/GT4Recomp/data");
    let gt4 = data.for_game(&GT4);
    let spec2 = data.for_game(&SPEC2);
    let root = Path::new("D:/GT4Recomp/data");
    assert_eq!(gt4.prepared(), root.join("games/gt4/prepared"));
    assert_eq!(spec2.prepared(), root.join("games/spec2/prepared"));
    assert_eq!(gt4.disc_folder(), root.join("games/gt4/disc"));
    assert_eq!(spec2.disc_folder(), root.join("games/spec2/disc"));
    assert_eq!(gt4.runtime_card(), root.join("games/gt4/runtime/card.ps2"));
    assert_eq!(spec2.runtime_card(), root.join("games/spec2/runtime/card.ps2"));
    assert_eq!(spec2.setup_prepared(), root.join("games/spec2/prepared"));
    assert_eq!(spec2.setup_disc_folder(), root.join("games/spec2/disc"));
    assert_ne!(gt4.runtime_card(), spec2.runtime_card(), "the two games never share a card");
    // Shared: the renderer, logs, sessions and textures.
    assert_eq!(gt4.renderer_root(), spec2.renderer_root());
    assert_eq!(gt4.logs(), spec2.logs());
    assert_eq!(gt4.sessions(), spec2.sessions());
    assert_eq!(gt4.textures(), spec2.textures());
}

#[test]
fn spec2_never_reads_the_flat_1_2_x_folders() {
    let base = scratch("flat");
    receipt(&base);
    let data = DataDirs::new(&base);
    assert_eq!(data.for_game(&GT4).prepared(), base.join("prepared"), "Gran Turismo 4 reads the flat place until it moves");
    assert_eq!(data.for_game(&SPEC2).prepared(), base.join("games/spec2/prepared"));
    receipt(&base.join("games/gt4"));
    assert_eq!(data.for_game(&GT4).prepared(), base.join("games/gt4/prepared"), "the new place wins");
    let _ = fs::remove_dir_all(&base);
}

#[test]
fn the_user_libraries_are_apart_per_game() {
    let user = UserDirs::new("U:/AppData/GT4Recomp", UserRootSource::Override);
    let gt4 = user.for_game(&GT4);
    let spec2 = user.for_game(&SPEC2);
    let root = Path::new("U:/AppData/GT4Recomp");
    assert_eq!(gt4.saves(), root.join("games/gt4/Saves"));
    assert_eq!(spec2.saves(), root.join("games/spec2/Saves"));
    assert_eq!(gt4.replays(), root.join("games/gt4/Replays"));
    assert_eq!(spec2.replays(), root.join("games/spec2/Replays"));
    assert_eq!(gt4.card_backups(), root.join("games/gt4/backups"));
    assert_eq!(spec2.card_backups(), root.join("games/spec2/backups"));
    // The settings and the controls are one set for both games.
    assert_eq!(gt4.config_file(), spec2.config_file());
    assert_eq!(gt4.controls_file(), spec2.controls_file());
    // Until Gran Turismo 4's flat library has moved, it is read where it is; Spec II never is.
    let mut flat = user.clone();
    flat.set_flat_gt4(true);
    assert_eq!(flat.for_game(&GT4).saves(), root.join("Saves"));
    assert_eq!(flat.for_game(&SPEC2).saves(), root.join("games/spec2/Saves"));
}

#[test]
fn the_selected_game_is_stored_and_a_file_of_schema_7_selects_gt4() {
    let mut file = ConfigFile::default();
    assert_eq!(file.selected_game, None, "a fresh install selects nothing until its first game is set up");
    assert!(!config::render(&file).contains("[game]"));
    file.selected_game = Some("spec2".to_string());
    let text = config::render(&file);
    assert!(text.contains("schema_version = 10"));
    assert!(text.contains("[game]\nselected = \"spec2\""), "{text}");
    let (read, warnings) = config::parse(&text).unwrap();
    assert_eq!(read.selected_game.as_deref(), Some("spec2"));
    assert!(warnings.is_empty());
    // A game this launcher does not know is ignored with a warning.
    let (unknown, warnings) = config::parse("schema_version = 8\n[game]\nselected = \"gt9\"\n").unwrap();
    assert_eq!(unknown.selected_game, None);
    assert!(warnings.iter().any(|warning| warning.contains("gt9")));
    // The step from schema 7: every older file belongs to a launcher that ran Gran Turismo 4 only.
    let dir = scratch("schema7");
    let path = dir.join("settings.toml");
    fs::write(&path, "schema_version = 7\n[settings]\nvsync = \"fast\"\n[install]\ndisc_path = \"D:/gt4.iso\"\n").unwrap();
    let (loaded, notes) = config::load(&path);
    assert!(notes.iter().any(|note| note.contains("migrated from schema 7 to 10")), "{notes:?}");
    assert_eq!(loaded.selected_game.as_deref(), Some("gt4"));
    assert_eq!(loaded.settings.get("vsync"), "fast", "other values survive");
    let _ = fs::remove_dir_all(&dir);
}

#[test]
fn each_game_starts_its_own_program_with_its_own_elf_disc_and_card() {
    let settings = Settings::default();
    let data = DataDirs::new("D:/GT4Recomp/data");
    let game_dir = Path::new("D:/GT4Recomp/game");
    let session = Path::new("D:/GT4Recomp/data/sessions/session-1");
    let mut plans = Vec::new();
    for game in games::GAMES {
        let dirs = data.for_game(game);
        let elf = launch::game_elf(game, &dirs.prepared());
        let disc = dirs.disc_folder().join(format!("{}.iso", game.serial));
        let card = dirs.runtime_card();
        let program = launch::game_program(game, game_dir);
        let inputs = LaunchInputs { game_executable: &program, elf: &elf, disc: &disc, card: &card, session_dir: session };
        let paths = launch::Pcsx2Paths {
            executable: PathBuf::from("gs/gt4-gs.exe"),
            executable_sha256: "00".repeat(32),
            config: session.join("gt4-gs.ini"),
            cache: PathBuf::from("gs/cache"),
            data_root: PathBuf::from("gs/data"),
            textures: PathBuf::from("Textures"),
        };
        plans.push((game.id, launch::plan_pcsx2_launch(&inputs, &settings, &paths, None, &[])));
    }
    let (_, gt4) = &plans[0];
    let (_, spec2) = &plans[1];
    assert_eq!(gt4.program, game_dir.join("gt4.exe"));
    assert_eq!(spec2.program, game_dir.join("gt4-spec2.exe"));
    assert!(gt4.arguments[0].ends_with("SCUS_973.28") && gt4.arguments[0].contains("gt4"));
    assert!(spec2.arguments[0].ends_with("SCUS_974.36") && spec2.arguments[0].contains("spec2"), "the argv0 name rule");
    let after = |plan: &launch::LaunchPlan, flag: &str| {
        let at = plan.arguments.iter().position(|argument| argument == flag).unwrap();
        plan.arguments[at + 1].clone()
    };
    assert!(after(gt4, "--disc").contains("SCUS-97328.iso") && after(gt4, "--memcard").contains("gt4"));
    assert!(after(spec2, "--disc").contains("SCUS-97436.iso") && after(spec2, "--memcard").contains("spec2"));
    // GSdx and every shared setting are the same for both games.
    assert_eq!(gt4.environment, spec2.environment);
    assert_eq!(gt4.arguments.iter().filter(|argument| argument.starts_with("--")).collect::<Vec<_>>(), spec2.arguments.iter().filter(|argument| argument.starts_with("--")).collect::<Vec<_>>());
}

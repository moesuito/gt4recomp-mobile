//! The launcher flow, driven the way the page drives it (docs/launcher/FLOW-AUDIT.md): the same command bodies the
//! Tauri commands call, in the page's order, without a window.
//!
//! - `synthetic_flow_end_to_end` runs on every `cargo test` with a small synthetic disc (no game data) and a fake
//!   renderer installer.
//! - `real_flow_end_to_end` is ignored by default. It uses the owner's discs (read-only), copies the real image into
//!   a sandbox data folder and downloads the real gt4-gs release from GitHub:
//!     set GT4_AUDIT_DATA_DIR=<fresh scratch folder>
//!     set GT4_REAL_ISO=<Gran Turismo 4 (USA) (v2.00).iso>
//!     set GT4_SPEC2_ISO=<Gran Turismo 4 Spec II v1.10.iso>        (optional)
//!     set GT4_AUDIT_GAME_DIR=<folder with a stand-in gt4.exe>  (optional; Play is planned, never started)
//!     cargo test -p gt4l-tauri --release real_flow -- --ignored --nocapture
//!
//! Play is never started here: the test stops at the launch plan (program, arguments, environment).

use std::fs;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use gt4l_core::launch::{self, LaunchInputs};
use gt4l_core::paths::{DataDirs, UserDirs, UserRootSource, USER_FOLDER_NAME};
use gt4l_disc::testing::{build_iso, pins_for, synthetic_game, system_cnf};
use gt4l_disc::Pins;
use serde_json::{json, Value};

use crate::commands::{self, Kind};
use crate::setup::{self, Notify, RendererInstaller};
use gt4l_disc::setup as disc_setup;
use crate::state::{lock, AppState, Shared};

/// Every event the page would have received, in order.
#[derive(Clone, Default)]
struct Recorder {
    events: Arc<Mutex<Vec<(String, Value)>>>,
}

impl Recorder {
    fn notify(&self) -> Notify {
        let events = self.events.clone();
        Arc::new(move |name: &str, payload: Value| events.lock().unwrap().push((name.to_string(), payload)))
    }

    fn names(&self, name: &str) -> Vec<Value> {
        self.events.lock().unwrap().iter().filter(|(event, _)| event == name).map(|(_, payload)| payload.clone()).collect()
    }
}

fn launch(dirs: &DataDirs, pins: &Pins) -> Shared {
    // A stand-in for %APPDATA%\GT4Recomp inside the sandbox: the flow never touches the real user folder. The
    // sandbox itself stands in for the install folder (the data folder is its `data`).
    let user = UserDirs::new(dirs.root().join("appdata-stand-in").join(USER_FOLDER_NAME), UserRootSource::Override);
    let install = dirs.root().parent().map(Path::to_path_buf).unwrap_or_else(|| dirs.root().to_path_buf());
    Arc::new(Mutex::new(AppState::load_with(dirs.clone(), pins.clone(), user, None, install).expect("state loads")))
}

fn screen(shared: &Shared) -> Value {
    commands::app_state_of(&lock(shared))
}

fn setup_state(shared: &Shared) -> String {
    screen(shared)["setup"]["state"].as_str().unwrap().to_string()
}

/// `setup_start`, then wait like the page's poll until the thread ends. Returns the states seen while polling.
fn run_setup(shared: &Shared, recorder: &Recorder, renderer: RendererInstaller) -> Vec<String> {
    let handle = setup::start_with(shared, recorder.notify(), renderer, no_coregs20()).expect("setup starts");
    let mut seen = Vec::new();
    while !handle.is_finished() {
        let view = screen(shared);
        let label = format!("{}:{}", view["setup"]["state"], view["setup"]["step"]);
        if seen.last() != Some(&label) {
            seen.push(label);
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    handle.join().unwrap();
    seen
}

/// A PC where CoreGS 2.0 does not run.
fn no_coregs20() -> setup::Coregs20Check {
    Arc::new(|_: &Shared, _: &'static gt4l_core::games::Game, _: &mut dyn FnMut(u32, u32)| false)
}

/// A PC where CoreGS 2.0 runs, compiling two shaders on the way.
fn coregs20_works() -> setup::Coregs20Check {
    Arc::new(|_: &Shared, _: &'static gt4l_core::games::Game, progress: &mut dyn FnMut(u32, u32)| {
        progress(0, 2);
        progress(2, 2);
        true
    })
}

fn fake_renderer() -> RendererInstaller {
    Arc::new(|_: &DataDirs, progress: &mut dyn FnMut(gt4l_core::gs_release::Step)| {
        use gt4l_core::gs_release::Step;
        progress(Step::Listing);
        progress(Step::Downloading);
        progress(Step::Transfer { done: 50, total: Some(100) });
        progress(Step::Transfer { done: 100, total: Some(100) });
        progress(Step::Installing);
        Ok("1.0.1".to_string())
    })
}

fn offline_renderer() -> RendererInstaller {
    Arc::new(|_: &DataDirs, progress: &mut dyn FnMut(gt4l_core::gs_release::Step)| {
        progress(gt4l_core::gs_release::Step::Listing);
        Err("release list: offline".to_string())
    })
}

fn scratch(name: &str) -> PathBuf {
    let root = std::env::temp_dir().join(format!("gt4l-flow-{name}-{}", std::process::id()));
    let _ = fs::remove_dir_all(&root);
    fs::create_dir_all(&root).unwrap();
    root
}

/// A synthetic memory card with one synthetic save (no game data).
fn synthetic_card(path: &Path) {
    use gt4saves::card::{DirEntry, Node, MODE_DIR_DEFAULT, MODE_FILE_DEFAULT};
    use gt4l_core::games::GT4;
    let stamp = [0, 39, 50, 7, 12, 10, 0xEA, 0x07];
    let file = Node::file(DirEntry::new("icon.sys", MODE_FILE_DEFAULT, stamp, stamp).unwrap(), vec![3; 964]);
    let save = Node::dir(DirEntry::new(GT4.save_folder, MODE_DIR_DEFAULT, stamp, stamp).unwrap(), vec![file]);
    fs::write(path, gt4saves::Card::build(&[save]).unwrap().to_image()).unwrap();
}

/// A synthetic Spec II memory card with one synthetic save (no game data).
fn synthetic_spec2_card(path: &Path) {
    use gt4saves::card::{DirEntry, Node, MODE_DIR_DEFAULT, MODE_FILE_DEFAULT};
    let stamp = [0, 39, 50, 7, 12, 10, 0xEA, 0x07];
    let file = Node::file(DirEntry::new("icon.sys", MODE_FILE_DEFAULT, stamp, stamp).unwrap(), vec![5; 964]);
    let save = Node::dir(DirEntry::new(gt4l_core::games::SPEC2.save_folder, MODE_DIR_DEFAULT, stamp, stamp).unwrap(), vec![file]);
    fs::write(path, gt4saves::Card::build(&[save]).unwrap().to_image()).unwrap();
}

/// The settings, saves, controls and about steps of the walk. Returns a short log.
fn walk_pages(shared: &Shared, card: &Path) -> Vec<String> {
    let mut log = Vec::new();
    let table = commands::get_settings_table_of(shared).expect("settings table");
    let rows = table["rows"].as_array().unwrap();
    assert!(rows.iter().any(|row| row["id"] == "renderer"), "renderer row");
    commands::set_setting_of("fxaa".into(), json!(true), shared).expect("fxaa on");
    let table = commands::get_settings_table_of(shared).unwrap();
    let fxaa = table["rows"].as_array().unwrap().iter().find(|row| row["id"] == "fxaa").unwrap().clone();
    assert_eq!(fxaa["current"], json!(true));
    assert!(commands::set_setting_of("no_such_setting".into(), json!(1), shared).is_err());
    let reset = commands::reset_gs_settings_to_defaults_of(shared).expect("reset");
    log.push(format!("settings: {} rows, fxaa set and read back, reset ok", reset["rows"].as_array().unwrap().len()));

    let before = commands::saves_list_of(shared).unwrap().items.len();
    synthetic_card(card);
    let after = commands::import_into(shared, card, Kind::Saves).expect("import");
    assert!(after.items.len() > before, "the card's save is in the library: {}", after.status);
    let again = commands::import_into(shared, card, Kind::Saves).expect("second import");
    assert_eq!(again.items.len(), after.items.len(), "imports never duplicate: {}", again.status);
    let replays = commands::replays_list_of(shared).unwrap();
    log.push(format!("saves: {} -> {} items ({}); replays: {}", before, after.items.len(), after.status, replays.items.len()));

    let controls = commands::get_controls_of(shared).expect("controls");
    assert!(controls["controls"].as_array().unwrap().len() >= 16);
    commands::reset_binding_of("cross".into(), shared).expect("reset cross");
    // A taken source is refused without swap; with swap both buttons change and the file follows at once.
    assert!(commands::assign_binding_of("cross".into(), "key:Q".into(), false, shared).is_err());
    let swapped = commands::assign_binding_of("cross".into(), "key:Q".into(), true, shared).expect("swap");
    assert_eq!(swapped["swapped_with"], json!("l1"));
    let file = fs::read_to_string(lock(shared).controls_path()).expect("controls file written");
    assert!(file.contains("cross = [\"xinput:A\", \"key:Q\"]") && file.contains("l1 = [\"xinput:LB\", \"key:W\"]"), "{file}");
    commands::set_setting_of("lstick_deadzone".into(), json!(10), shared).expect("stick deadzone");
    let tuned = fs::read_to_string(lock(shared).controls_path()).unwrap();
    assert!(tuned.contains("xinput:LX | deadzone=0.100"), "{tuned}");
    let reset = commands::reset_all_bindings_of(shared).expect("reset all");
    let cross = reset["controls"].as_array().unwrap().iter().find(|row| row["id"] == "cross").unwrap().clone();
    assert_eq!(cross["key"], json!("W"));
    log.push(format!("controls: {} inputs, cross reset, swap with l1, stick deadzone, reset all", controls["controls"].as_array().unwrap().len()));

    let about = commands::about_info_of(shared).expect("about");
    assert!(Path::new(&about.game_files).is_dir(), "{}", about.game_files);
    log.push(format!("about: GT4Recomp {} gt4-gs {} files {}", about.gt4recomp, about.gt4gs, about.game_files));
    log
}

/// What Play would run, without running it.
fn play_plan(shared: &Shared, program_dir: &Path) -> Result<launch::LaunchPlan, String> {
    let inputs = commands::prepare_play(shared, program_dir)?;
    let session = inputs.dirs.session("session-plan-only");
    let card = inputs.dirs.runtime_card();
    let launch_inputs = LaunchInputs {
        game_executable: &inputs.executable,
        elf: &inputs.elf,
        disc: &inputs.disc,
        card: &card,
        session_dir: &session,
    };
    let inherited: Vec<String> = std::env::vars().map(|(name, _)| name).collect();
    let textures = crate::state::lock(shared).dirs.textures();
    let paths = launch::Pcsx2Paths::for_session(inputs.pcsx2.as_ref().expect("a GSdx session"), &inputs.dirs.root().join("gs"), &session, &textures);
    Ok(launch::plan_pcsx2_launch(&launch_inputs, &inputs.settings, &paths, inputs.display.as_ref(), &inherited))
}

#[test]
fn synthetic_flow_end_to_end() {
    let root = scratch("synthetic");
    let data = root.join("data");
    let dirs = DataDirs::new(&data);
    let game = synthetic_game(0x0010_0000, 0x0010_0008, b"synthetic guest code, not game data, 0123456789abcdef");
    let image = |version: &str, serial_boot: &str| {
        build_iso(
            "GT4 USA",
            &[
                ("SYSTEM.CNF;1", system_cnf(serial_boot, version, "NTSC")),
                ("CORE.GT4;1", game.core.clone()),
                ("SCUS_973.28;1", vec![0xA5; 3000]),
            ],
        )
    };
    let good = image("2.00", "SCUS_973.28");
    let pins = pins_for(&good, &game, "SCUS-97328", "2.00");
    let picked = root.join("picked").join("Gran Turismo 4 (USA) (v2.00).iso");
    fs::create_dir_all(picked.parent().unwrap()).unwrap();
    fs::write(&picked, &good).unwrap();

    // First launch: Setup, never Home.
    let shared = launch(&dirs, &pins);
    assert_eq!(setup_state(&shared), "needed");

    // Refused files: not an ISO, a truncated copy, Spec II. The page shows the message and "Choose another file".
    let text = root.join("notes.iso");
    fs::write(&text, b"not a disc image").unwrap();
    assert!(setup::choose_disc_path(&shared, &text).is_err());
    assert_eq!(setup_state(&shared), "wrong_disc");
    let truncated = root.join("truncated.iso");
    fs::write(&truncated, &good[..good.len() - 2048]).unwrap();
    let message = setup::choose_disc_path(&shared, &truncated).unwrap_err();
    assert!(message.contains("incomplete"), "{message}");
    let spec = root.join("spec2.iso");
    fs::write(&spec, image("1.00", "SCUS_974.36")).unwrap();
    let message = setup::choose_disc_path(&shared, &spec).unwrap_err();
    assert!(message.starts_with("This disc is not supported yet."), "{message}");
    assert_eq!(screen(&shared)["setup"]["problem"], json!(message));
    // Setup cannot start before a disc was accepted.
    let refused = setup::start_with(&shared, Recorder::default().notify(), fake_renderer(), no_coregs20()).map(|_| ()).unwrap_err();
    assert_eq!(refused, "Choose your disc image first.");

    // Accept the good disc; the page then calls setup_start.
    let shared = launch(&dirs, &pins);
    let info = setup::choose_disc_path(&shared, &picked).expect("accepted");
    assert_eq!(info.name, "Gran Turismo 4 (USA v2.00)");
    let recorder = Recorder::default();
    let seen = run_setup(&shared, &recorder, fake_renderer());
    // The poll can catch the final state ("done") between the last state write and the thread's exit (it still sends
    // setup-finished), so only the last label may be the final one.
    let (last, before) = seen.split_last().expect("at least one label");
    assert!(before.iter().all(|label| label.starts_with("\"checking\"")), "the page shows setup until it ends: {seen:?}");
    assert!(last.starts_with("\"checking\"") || last.starts_with("\"done\""), "the page shows setup until it ends: {seen:?}");
    let view = screen(&shared);
    assert_eq!(view["setup"]["state"], "done", "{view}");
    assert_eq!(view["setup"]["note"], "");
    let copying: Vec<Value> = recorder.names("setup-progress").into_iter().filter(|event| event["step"] == "copying").collect();
    assert!(copying.iter().any(|event| event["percent"] == 100 && event["bytesDone"] == json!(good.len())), "{copying:?}");
    let renderer = recorder.names("renderer-progress");
    assert!(renderer.iter().any(|event| event["step"] == "downloading" && event["percent"] == 50), "{renderer:?}");
    assert!(renderer.iter().any(|event| event["step"] == "done"));
    assert_eq!(recorder.names("setup-finished"), vec![json!({ "ok": true, "message": "" })]);
    assert!(!data.join("games").join("gt4").join("disc").exists(), "the image is not copied");
    assert!(picked.is_file(), "the image stays where it is");

    // The next launch goes straight to Home, with no hash (well under the time a hash of a real image takes).
    let started = Instant::now();
    let second = launch(&dirs, &pins);
    assert_eq!(setup_state(&second), "done");
    assert!(started.elapsed() < Duration::from_secs(2));

    // Settings, saves, controls, about.
    let log = walk_pages(&second, &root.join("test-card.ps2"));
    assert_eq!(log.len(), 4);

    // Play: GSdx is not installed in this test, so Play refuses with the GSdx error and starts nothing. The stand-in
    // program is a text file. The GSdx launch line is covered by the launch contract tests.
    let program = root.join("program");
    fs::create_dir_all(program.join("game")).unwrap();
    fs::write(program.join("game").join("gt4.exe"), b"stand-in, never started").unwrap();
    let refusal = play_plan(&second, &program).unwrap_err();
    assert!(refusal.starts_with("GSdx could not run the game: GSdx is not installed yet"), "{refusal}");
    assert!(refusal.contains("log folder"), "{refusal}");

    // The player moved the image: Home is not shown, Play refuses, Setup offers to choose the disc again and says where it was.
    let moved = root.join("moved.iso");
    fs::rename(&picked, &moved).unwrap();
    let third = launch(&dirs, &pins);
    let view = screen(&third);
    assert_eq!(view["setup"]["state"], "files_missing", "{view}");
    let problem = view["setup"]["problem"].as_str().unwrap();
    assert!(problem.contains("Choose your disc image again") && problem.contains(&picked.display().to_string()), "{problem}");
    assert_eq!(view["games"][0]["damaged"], true, "{view}");
    assert_eq!(view["games"][0]["linked"], true, "{view}");
    assert!(play_plan(&third, &program).unwrap_err().contains("Game files missing"));

    // Offline renderer: setup still finishes, with the note; Play then refuses until GSdx is installed.
    let picked_again = root.join("again.iso");
    fs::write(&picked_again, &good).unwrap();
    setup::choose_disc_path(&third, &picked_again).unwrap();
    run_setup(&third, &Recorder::default(), offline_renderer());
    let view = screen(&third);
    assert_eq!(view["setup"]["state"], "done");
    assert!(view["setup"]["note"].as_str().unwrap().contains("cannot start yet"));
    assert!(play_plan(&third, &root.join("program")).unwrap_err().contains("GSdx could not run the game"));

    // Cancel: back to a failed run with nothing half-written, and the earlier copy stays usable.
    let shared = launch(&dirs, &pins);
    setup::choose_disc_path(&shared, &picked_again).unwrap();
    // start_with clears the flag; Cancel is pressed right after the start, as the page's Cancel button would.
    let handle = setup::start_with(&shared, Recorder::default().notify(), fake_renderer(), no_coregs20()).unwrap();
    setup::cancel_with(&shared);
    handle.join().unwrap();
    let state = setup_state(&shared);
    assert!(state == "failed" || state == "done", "{state}");
    assert!(!data.join("games").join("gt4").join("disc").exists());
    assert!(!data.join("games").join("gt4").join("prepared.tmp").exists());
    let _ = fs::remove_dir_all(&root);
}

#[test]
fn settings_store_no_absolute_path_inside_the_install_folder() {
    // TASK-091: a copied install folder (another drive or PC) must still find what the settings name.
    let root = scratch("portable-settings");
    let dirs = DataDirs::new(root.join("data"));
    let game = synthetic_game(0x0010_0000, 0x0010_0008, b"synthetic guest code, not game data, 0123456789abcdef");
    let image = build_iso("GT4 USA", &[("SYSTEM.CNF;1", system_cnf("SCUS_973.28", "2.00", "NTSC")), ("CORE.GT4;1", game.core.clone())]);
    let pins = pins_for(&image, &game, "SCUS-97328", "2.00");
    let picked = root.join("images").join("gt4.iso");
    fs::create_dir_all(picked.parent().unwrap()).unwrap();
    fs::write(&picked, &image).unwrap();
    let shared = launch(&dirs, &pins);
    setup::choose_disc_path(&shared, &picked).expect("accepted");
    {
        let mut state = lock(&shared);
        state.config.dev_game_dir = Some(root.join("game-build"));
        state.save_config();
    }
    let file = lock(&shared).user_dirs.config_file();
    let text = fs::read_to_string(&file).unwrap();
    // TOML doubles each backslash, so the stored form of the sandbox path is checked too.
    let root_text = root.display().to_string().replace('\\', "\\\\");
    assert!(!text.contains(&root_text) && !text.contains(&root.display().to_string()), "{text}");
    assert!(text.contains("disc_path = \"images\\\\gt4.iso\""), "{text}");
    // The next start resolves it against the install folder again.
    let again = launch(&dirs, &pins);
    assert_eq!(lock(&again).config.disc_path.as_deref(), Some(picked.as_path()));
    assert_eq!(lock(&again).config.dev_game_dir.as_deref(), Some(root.join("game-build").as_path()));
    let _ = fs::remove_dir_all(&root);
}

#[test]
fn a_settings_change_writes_the_files_a_direct_start_of_the_game_reads() {
    // TASK-097: the game started without the launcher reads game.ini and gt4-gs.ini from the user config folder.
    let root = scratch("standalone-files");
    let dirs = DataDirs::new(root.join("data"));
    let pins = Pins::embedded().expect("embedded pins");
    let shared = launch(&dirs, &pins);
    let config_dir = lock(&shared).user_dirs.config_dir().to_path_buf();
    let game_ini = config_dir.join(launch::STANDALONE_CONFIG_FILE);
    let gs_ini = config_dir.join(launch::STANDALONE_GS_INI_FILE);
    {
        let mut state = lock(&shared);
        state.config.settings.set("vsync", "fast").unwrap();
        state.config.settings.set("fullscreen", "off").unwrap();
        state.save_config();
    }
    let text = fs::read_to_string(&game_ini).expect("game.ini is written with the settings");
    assert!(text.contains("present_mode=mailbox"), "{text}");
    assert!(text.contains("fullscreen=0"), "{text}");
    assert!(!text.contains("async_gs"), "{text}");
    assert!(!gs_ini.exists(), "no gt4-gs.ini without an installed GSdx");

    // An installed GSdx (stand-in files with the recorded hash) gives the game its ini too.
    let version = dirs.root().join("gs").join("1.2.3");
    fs::create_dir_all(&version).unwrap();
    let program = b"stand-in gt4-gs.exe, never started";
    fs::write(version.join("gt4-gs.exe"), program).unwrap();
    fs::write(version.join("sha256.txt"), gt4l_core::gs_release::sha256_hex(program)).unwrap();
    fs::write(dirs.root().join("gs").join("current.txt"), "1.2.3").unwrap();
    lock(&shared).save_config();
    let ini = fs::read_to_string(&gs_ini).expect("gt4-gs.ini is written once GSdx is installed");
    assert!(ini.contains("[EmuCore/GS]"), "{ini}");
    assert!(ini.contains(&dirs.root().join("gs").join("cache").display().to_string()), "the cache folder of the data folder: {ini}");
    assert_eq!(fs::read_to_string(&game_ini).unwrap(), text, "game.ini does not change with the renderer");
    let _ = fs::remove_dir_all(root);
}

/// Turns a game's folder into what a launcher from before decision 0075 left: the image copied in `disc`, no link in the receipt.
fn make_it_an_install_with_a_copy(game_folder: &Path, image: &Path, name: &str) {
    fs::create_dir_all(game_folder.join("disc")).unwrap();
    fs::rename(image, game_folder.join("disc").join(name)).unwrap();
    let receipt_path = game_folder.join("prepared").join("prepare.json");
    let mut receipt: Value = serde_json::from_slice(&fs::read(&receipt_path).unwrap()).unwrap();
    receipt["schema_version"] = 2.into();
    receipt["image"]["file"] = name.into();
    receipt["image"].as_object_mut().unwrap().remove("linked_path");
    fs::write(&receipt_path, serde_json::to_vec(&receipt).unwrap()).unwrap();
}

#[test]
fn an_install_with_a_copy_stays_installed_and_removing_it_never_touches_a_linked_image() {
    // The safety rule of decision 0075: updating the launcher does not uninstall anyone.
    let (root, dirs, pins, shared) = new_install("copy-stays");
    run_setup(&shared, &Recorder::default(), fake_renderer());
    let game_folder = dirs.root().join("games").join("gt4");
    make_it_an_install_with_a_copy(&game_folder, &root.join("disc.iso"), "SCUS-97328.iso");
    let updated = launch(&dirs, &pins);
    let view = screen(&updated);
    assert_eq!(view["setup"]["state"], "done", "{view}");
    assert_eq!(view["games"][0]["installed"], true, "{view}");
    assert_eq!(view["games"][0]["linked"], false, "{view}");
    let copy = game_folder.join("disc").join("SCUS-97328.iso");
    assert!(copy.is_file());

    // Choosing the image again links it. The launcher's old copy stays until the player removes the game.
    let elsewhere = root.join("my-isos");
    fs::create_dir_all(&elsewhere).unwrap();
    let linked = elsewhere.join("gt4.iso");
    fs::copy(&copy, &linked).unwrap();
    setup::choose_disc_path(&updated, &linked).unwrap();
    run_setup(&updated, &Recorder::default(), fake_renderer());
    assert_eq!(screen(&updated)["games"][0]["linked"], true);
    assert!(copy.is_file());

    // Removing the game deletes what is the launcher's, and never the player's image.
    commands::games_remove_of("gt4".into(), &updated).unwrap();
    assert!(linked.is_file(), "the player's image is not touched");
    assert!(!copy.exists() && !game_folder.join("prepared").exists());

    // An image the player linked from inside the old copy's folder is theirs too.
    fs::create_dir_all(game_folder.join("disc")).unwrap();
    let inside = game_folder.join("disc").join("SCUS-97328.iso");
    fs::rename(&linked, &inside).unwrap();
    setup::choose_disc_path(&updated, &inside).unwrap();
    run_setup(&updated, &Recorder::default(), fake_renderer());
    commands::games_remove_of("gt4".into(), &updated).unwrap();
    assert!(inside.is_file(), "a linked image inside the folder is kept");
    assert!(!game_folder.join("prepared").exists());
    let _ = fs::remove_dir_all(&root);
}

#[test]
fn a_portable_folder_carried_with_its_image_stays_installed_and_one_outside_keeps_its_place() {
    // `launch` takes the data folder's parent as the launcher's folder.
    let root = scratch("portable-carried");
    let first = root.join("first-place");
    fs::create_dir_all(first.join("ISOs")).unwrap();
    let game = synthetic_game(0x0010_0000, 0x0010_0008, b"synthetic guest code, not game data, 0123456789abcdef");
    let image = build_iso(
        "GT4 USA",
        &[
            ("SYSTEM.CNF;1", system_cnf("SCUS_973.28", "2.00", "NTSC")),
            ("CORE.GT4;1", game.core.clone()),
            ("SCUS_973.28;1", vec![0xA5; 3000]),
        ],
    );
    let pins = pins_for(&image, &game, "SCUS-97328", "2.00");
    let inside = first.join("ISOs").join("gt4.iso");
    fs::write(&inside, &image).unwrap();
    let dirs = DataDirs::new(first.join("data"));
    let shared = launch(&dirs, &pins);
    setup::choose_disc_path(&shared, &inside).unwrap();
    run_setup(&shared, &Recorder::default(), fake_renderer());
    drop(shared);
    let receipt = disc_setup::read_receipt(&first.join("data/games/gt4/prepared")).unwrap();
    assert_eq!(receipt.image.unwrap().linked_path.as_deref(), Some(Path::new("ISOs").join("gt4.iso").to_str().unwrap()));

    let second = root.join("another-drive");
    fs::rename(&first, &second).unwrap();
    let carried = launch(&DataDirs::new(second.join("data")), &pins);
    assert_eq!(setup_state(&carried), "done", "the folder moved with its image");
    assert!(matches!(lock(&carried).installed(), disc_setup::Installed::Ready { image, .. } if image == second.join("ISOs").join("gt4.iso")));
    // The Games page names the image and its size, not what the launcher itself keeps.
    let row = &screen(&carried)["games"][0];
    assert_eq!(row["image_path"], json!(second.join("ISOs").join("gt4.iso").display().to_string()), "{row}");
    assert_eq!(row["image_bytes"], json!(image.len()), "{row}");
    assert!(row["size_bytes"].as_u64().unwrap() < image.len() as u64, "removing the game frees only the launcher's files: {row}");
    let _ = fs::remove_dir_all(&root);
}

#[test]
fn an_image_in_the_folder_updates_replace_is_refused_with_the_reason() {
    let (root, dirs, _pins, shared) = new_install("image-in-game-folder");
    let install = dirs.root().parent().unwrap().to_path_buf();
    fs::create_dir_all(install.join("game")).unwrap();
    let there = install.join("game").join("gt4.iso");
    fs::copy(root.join("disc.iso"), &there).unwrap();
    let message = setup::choose_disc_path(&shared, &there).unwrap_err();
    assert!(message.contains("Updates replace"), "{message}");
    assert_eq!(screen(&shared)["setup"]["state"], "wrong_disc");
    let _ = fs::remove_dir_all(&root);
}

/// The real setup in a real folder, for a test that then moves the folder and starts the game from it. GT4_E2E_PORTABLE is the
/// folder; its `ISOs\spec2.iso` is the image.
#[test]
#[ignore = "a real disc image; set GT4_E2E_PORTABLE"]
fn real_setup_in_a_portable_folder() {
    let folder = PathBuf::from(std::env::var("GT4_E2E_PORTABLE").expect("GT4_E2E_PORTABLE"));
    let dirs = DataDirs::new(folder.join("data"));
    let pins = gt4l_disc::Pins::embedded().unwrap();
    let shared = launch(&dirs, &pins);
    setup::choose_disc_path(&shared, &folder.join("ISOs").join("spec2.iso")).unwrap();
    run_setup(&shared, &Recorder::default(), fake_renderer());
    let receipt = disc_setup::read_receipt(&folder.join("data/games/spec2/prepared")).unwrap();
    println!("linked_path = {:?}", receipt.image.as_ref().unwrap().linked_path);
    assert!(Path::new(receipt.image.unwrap().linked_path.as_deref().unwrap()).is_relative());
}

#[test]
fn choosing_coregs20_after_gsdx_failed_stores_it_when_the_card_can_run_it() {
    let (root, dirs, pins, shared) = new_install("gsdx-fallback");
    lock(&shared).config.settings.set_by_user("renderer", "pcsx2").unwrap();
    assert!(!commands::use_coregs20_after_gsdx_failure(&shared, false), "a card without Vulkan 1.2 keeps GSdx");
    assert_eq!(lock(&shared).config.settings.get("renderer"), "pcsx2");
    assert!(commands::use_coregs20_after_gsdx_failure(&shared, true));
    assert_eq!(lock(&shared).config.settings.get("renderer"), "coregs20");
    assert_eq!(lock(&launch(&dirs, &pins)).config.settings.get("renderer"), "coregs20", "and it stays at the next start");
    let _ = fs::remove_dir_all(&root);
}

#[test]
fn accepting_plays_coregs20_offer_stores_it_and_a_card_without_vulkan_keeps_gsdx_with_a_reason() {
    // Issues #139 and #179: GSdx is the stored renderer but is not installed, and its download cannot be reached.
    let (root, dirs, pins, shared) = new_install("play-offer");
    assert_eq!(lock(&shared).config.settings.get("renderer"), "pcsx2", "GSdx is the default renderer");
    assert!(gt4l_core::gs_release::installed(&lock(&shared).dirs.renderer_root()).is_none(), "GSdx is not installed here");
    let refused = commands::use_coregs20_for_play(&shared, false).unwrap_err();
    assert_eq!(refused, "CoreGS 2.0 could not be selected. Choose it in Settings > Graphics > Renderer.");
    assert_eq!(lock(&shared).config.settings.get("renderer"), "pcsx2", "a refused switch changes nothing");
    commands::use_coregs20_for_play(&shared, true).expect("a card with Vulkan 1.2 can run CoreGS 2.0");
    assert_eq!(lock(&shared).config.settings.get("renderer"), "coregs20");
    assert_eq!(lock(&launch(&dirs, &pins)).config.settings.get("renderer"), "coregs20", "and it stays at the next start");
    assert!(
        !gt4l_core::launch::offers_coregs20(&lock(&shared).config.settings, false, true),
        "with CoreGS 2.0 stored, Play has nothing to offer"
    );
    let _ = fs::remove_dir_all(&root);
}

/// A launcher with no game set up and a synthetic disc accepted.
fn new_install(name: &str) -> (PathBuf, DataDirs, Pins, Shared) {
    let root = scratch(name);
    let dirs = DataDirs::new(root.join("data"));
    let game = synthetic_game(0x0010_0000, 0x0010_0008, b"synthetic guest code, not game data, 0123456789abcdef");
    let image = build_iso(
        "GT4 USA",
        &[
            ("SYSTEM.CNF;1", system_cnf("SCUS_973.28", "2.00", "NTSC")),
            ("CORE.GT4;1", game.core.clone()),
            ("SCUS_973.28;1", vec![0xA5; 3000]),
        ],
    );
    let pins = pins_for(&image, &game, "SCUS-97328", "2.00");
    let picked = root.join("disc.iso");
    fs::write(&picked, image).unwrap();
    let shared = launch(&dirs, &pins);
    setup::choose_disc_path(&shared, &picked).unwrap();
    (root, dirs, pins, shared)
}

#[test]
fn a_new_install_uses_coregs20_when_the_pc_runs_it_and_downloads_nothing() {
    let (root, dirs, pins, shared) = new_install("coregs20-first");
    let recorder = Recorder::default();
    let downloads = Arc::new(std::sync::atomic::AtomicUsize::new(0));
    let counted = downloads.clone();
    let renderer: RendererInstaller = Arc::new(move |_: &DataDirs, _: &mut dyn FnMut(gt4l_core::gs_release::Step)| {
        counted.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
        Ok("1.0.1".to_string())
    });
    let handle = setup::start_with(&shared, recorder.notify(), renderer, coregs20_works()).expect("setup starts");
    handle.join().unwrap();
    assert_eq!(downloads.load(std::sync::atomic::Ordering::SeqCst), 0, "GSdx is not downloaded");
    assert!(recorder.names("renderer-progress").is_empty());
    let view = screen(&shared);
    assert_eq!(view["setup"]["state"], "done", "{view}");
    assert_eq!(view["setup"]["note"], "");
    assert_eq!(view["setup"]["renderer_choice"], "coregs20");
    assert_eq!(lock(&shared).config.settings.get("renderer"), "coregs20");
    // The choice is stored: the next launch has it, and Play needs no GSdx.
    let again = launch(&dirs, &pins);
    assert_eq!(lock(&again).config.settings.get("renderer"), "coregs20");
    let _ = fs::remove_dir_all(&root);
}

#[test]
fn a_new_install_downloads_gsdx_when_coregs20_does_not_run_and_says_why() {
    let (root, _dirs, _pins, shared) = new_install("coregs20-fallback");
    let recorder = Recorder::default();
    run_setup(&shared, &recorder, fake_renderer());
    assert!(recorder.names("renderer-progress").iter().any(|event| event["step"] == "done"), "GSdx is installed");
    let view = screen(&shared);
    assert_eq!(view["setup"]["renderer_choice"], "gsdx_fallback", "{view}");
    assert_eq!(lock(&shared).config.settings.get("renderer"), "pcsx2");
    let _ = fs::remove_dir_all(&root);
}

/// The real check on this PC, with a synthetic disc: GT4_SETUP_GAME_DIR is a game folder (`gt4.exe`, a distribution folder).
#[test]
#[ignore = "this PC's graphics card and a real game program; set GT4_SETUP_GAME_DIR"]
fn the_real_check_starts_coregs20_with_the_game_program() {
    let (root, dirs, _pins, shared) = new_install("coregs20-real");
    lock(&shared).config.dev_game_dir = Some(PathBuf::from(std::env::var("GT4_SETUP_GAME_DIR").expect("GT4_SETUP_GAME_DIR")));
    let handle = setup::start_with(&shared, Recorder::default().notify(), offline_renderer(), setup::coregs20_on_this_pc()).expect("setup starts");
    handle.join().unwrap();
    assert_eq!(screen(&shared)["setup"]["renderer_choice"], "coregs20", "{}", fs::read_to_string(dirs.logs().join("shaders.log")).unwrap_or_default());
    assert_eq!(lock(&shared).config.settings.get("renderer"), "coregs20");
    let _ = fs::remove_dir_all(root);
}

/// A game program that cannot start CoreGS 2.0 sends setup to GSdx, on any PC.
#[test]
#[ignore = "probes this PC's graphics cards"]
fn the_real_check_falls_back_when_the_game_program_is_missing() {
    let (root, _dirs, _pins, shared) = new_install("coregs20-missing");
    lock(&shared).config.dev_game_dir = Some(root.join("no-such-folder"));
    let handle = setup::start_with(&shared, Recorder::default().notify(), fake_renderer(), setup::coregs20_on_this_pc()).expect("setup starts");
    handle.join().unwrap();
    let view = screen(&shared);
    assert_eq!(view["setup"]["renderer_choice"], "gsdx_fallback", "{view}");
    assert_eq!(view["setup"]["note"], "", "GSdx was installed");
    assert_eq!(lock(&shared).config.settings.get("renderer"), "pcsx2");
    let _ = fs::remove_dir_all(root);
}

#[test]
fn without_gsdx_setup_finishes_and_play_refuses_with_the_log_folder() {
    let root = scratch("gsdx-required");
    let dirs = DataDirs::new(root.join("data"));
    let game = synthetic_game(0x0010_0000, 0x0010_0008, b"synthetic guest code, not game data, 0123456789abcdef");
    let image = build_iso(
        "GT4 USA",
        &[
            ("SYSTEM.CNF;1", system_cnf("SCUS_973.28", "2.00", "NTSC")),
            ("CORE.GT4;1", game.core.clone()),
            ("SCUS_973.28;1", vec![0xA5; 3000]),
        ],
    );
    let pins = pins_for(&image, &game, "SCUS-97328", "2.00");
    let picked = root.join("disc.iso");
    fs::write(&picked, image).unwrap();
    let shared = launch(&dirs, &pins);
    setup::choose_disc_path(&shared, &picked).unwrap();
    let recorder = Recorder::default();
    run_setup(&shared, &recorder, offline_renderer());
    let view = screen(&shared);
    assert_eq!(view["setup"]["state"], "done");
    assert!(view["setup"]["note"].as_str().unwrap().contains("cannot start yet"));
    assert_eq!(recorder.names("setup-finished"), vec![json!({ "ok": true, "message": "" })]);

    let second = launch(&dirs, &pins);
    let program = root.join("program");
    fs::create_dir_all(program.join("game")).unwrap();
    fs::write(program.join("game").join("gt4.exe"), b"stand-in, never started").unwrap();
    let refusal = play_plan(&second, &program).unwrap_err();
    assert_eq!(
        refusal,
        launch::gsdx_unavailable_message(launch::GSDX_NOT_INSTALLED, &dirs.logs()),
        "the reason, what to do and the log folder"
    );
    assert!(refusal.contains("Check your internet connection and try again"), "{refusal}");
    let _ = fs::remove_dir_all(root);
}

#[test]
#[ignore = "real discs, a real copy of the image and a real download; see the module comment"]
fn real_flow_end_to_end() {
    let data = PathBuf::from(std::env::var("GT4_AUDIT_DATA_DIR").expect("set GT4_AUDIT_DATA_DIR"));
    let real = PathBuf::from(std::env::var("GT4_REAL_ISO").expect("set GT4_REAL_ISO"));
    let dirs = DataDirs::new(&data);
    let pins = Pins::embedded().unwrap();
    let mut report: Vec<String> = Vec::new();
    let mut note = |line: String| {
        println!("{line}");
        report.push(line);
    };

    let shared = launch(&dirs, &pins);
    note(format!("first launch: {}", screen(&shared)["setup"]));
    assert_ne!(setup_state(&shared), "done");

    let scratch = data.parent().unwrap().join("audit-inputs");
    fs::create_dir_all(&scratch).unwrap();
    let text = scratch.join("not-a-disc.iso");
    fs::write(&text, b"plain text, not a disc image").unwrap();
    note(format!("non-ISO: {}", setup::choose_disc_path(&shared, &text).unwrap_err()));
    let missing = scratch.join("missing.iso");
    note(format!("missing file: {}", setup::choose_disc_path(&shared, &missing).unwrap_err()));
    // A truncated copy: the first 64 MiB of the real image (holds SYSTEM.CNF and CORE.GT4, not the rest).
    let truncated = scratch.join("truncated.iso");
    {
        use std::io::{Read, Write};
        let mut input = fs::File::open(&real).unwrap().take(64 * 1024 * 1024);
        let mut output = fs::File::create(&truncated).unwrap();
        let mut buffer = Vec::new();
        input.read_to_end(&mut buffer).unwrap();
        output.write_all(&buffer).unwrap();
    }
    note(format!("truncated copy: {}", setup::choose_disc_path(&shared, &truncated).unwrap_err()));
    let _ = fs::remove_file(&truncated);
    if let Ok(spec) = std::env::var("GT4_SPEC2_ISO") {
        note(format!("Spec II: {}", setup::choose_disc_path(&shared, Path::new(&spec)).unwrap_err()));
        assert_eq!(setup_state(&shared), "wrong_disc");
    }

    let info = setup::choose_disc_path(&shared, &real).expect("the v2.00 disc is accepted");
    note(format!("accepted: {} ({})", info.name, info.serial));
    let free = gt4l_platform::disk::free_space(&dirs.disc_folder());
    note(format!("free space before the copy: {free:?} bytes"));
    let recorder = Recorder::default();
    let started = Instant::now();
    let seen = run_setup(&shared, &recorder, setup::github_renderer());
    note(format!("setup took {:.1} s; states seen: {seen:?}", started.elapsed().as_secs_f64()));
    let progress = recorder.names("setup-progress");
    note(format!("setup-progress events: {} (first {:?}, last {:?})", progress.len(), progress.first(), progress.last()));
    note(format!("renderer-progress events: {:?}", recorder.names("renderer-progress").into_iter().filter(|event| event["step"] != "downloading").collect::<Vec<_>>()));
    note(format!("setup-finished: {:?}", recorder.names("setup-finished")));
    let view = screen(&shared);
    note(format!("after setup: {}", view["setup"]));
    assert_eq!(view["setup"]["state"], "done");

    let started = Instant::now();
    let second = launch(&dirs, &pins);
    let state = setup_state(&second);
    note(format!("second launch: {state} in {} ms", started.elapsed().as_millis()));
    assert_eq!(state, "done");

    for line in walk_pages(&second, &scratch.join("test-card.ps2")) {
        note(line);
    }
    let check = crate::updates::check(&dirs, gt4l_core::update::UpdateChannel::Stable, None, None);
    note(format!(
        "check_updates: {} offers, errors {:?}, gt4-gs installed {:?}",
        check.offers.len(),
        check.errors,
        check.renderer_installed.map(|version| version.text())
    ));
    if let Ok(game_dir) = std::env::var("GT4_AUDIT_GAME_DIR") {
        lock(&second).config.dev_game_dir = Some(PathBuf::from(game_dir));
        match play_plan(&second, Path::new(".")) {
            Ok(plan) => {
                note(format!("play program: {}", plan.program.display()));
                note(format!("play arguments: {:?}", plan.arguments));
                note(format!("play environment: {:?}", plan.safe_environment()));
                note(format!("play removes inherited: {:?}", plan.removed_inherited));
            }
            Err(message) => note(format!("play plan refused: {message}")),
        }
    }
    fs::write(data.parent().unwrap().join("real-flow-report.txt"), report.join("\n")).unwrap();
}

// Two games (TASK-101) -----------------------------------------------------------------------------------------------

/// A synthetic Spec II executable (a small ELF32 with the plugin segment) and pins that describe it.
fn synthetic_spec2() -> (Vec<u8>, gt4_disc_spec2::Spec2Pins) {
    gt4_disc_spec2::synthetic()
}

/// Helpers for a synthetic Spec II disc (no game data).
mod gt4_disc_spec2 {
    use gt4l_disc::elf::sha256_hex;
    use gt4l_disc::spec2_elf;
    use gt4l_disc::spec2_pins::Spec2Patch;
    pub use gt4l_disc::Spec2Pins;
    use gt4l_disc::Identity;

    pub fn synthetic() -> (Vec<u8>, Spec2Pins) {
        const PLUGIN_VA: u32 = 0x68A480;
        let headers: [[u32; 8]; 3] = [
            [1, 0x1000, 0x100000, 0x100000, 0x100, 0x100, 5, 0x1000],
            [1, 0x1100, 0x680000, 0x680000, 0x20000, 0x30000, 6, 0x1000],
            [1, 0x21100, PLUGIN_VA, PLUGIN_VA, 0x40, 0x40, 7, 0x10],
        ];
        let mut image = vec![0u8; 0x21140];
        image[..4].copy_from_slice(&[0x7f, b'E', b'L', b'F']);
        image[4] = 1;
        image[5] = 1;
        image[28..32].copy_from_slice(&52u32.to_le_bytes());
        image[42..44].copy_from_slice(&32u16.to_le_bytes());
        image[44..46].copy_from_slice(&3u16.to_le_bytes());
        for (index, header) in headers.iter().enumerate() {
            for (field, value) in header.iter().enumerate() {
                let at = 52 + index * 32 + field * 4;
                image[at..at + 4].copy_from_slice(&value.to_le_bytes());
            }
        }
        image[0x1000..0x1004].copy_from_slice(&0x1111_2222u32.to_le_bytes());
        let patches = vec![Spec2Patch { address: 0x100000, size: 4, old: 0x1111_2222, new: 0xAABB_CCDD }];
        let derived = spec2_elf::derive(&image, &patches).unwrap();
        let pins = Spec2Pins {
            serial: "SCUS-97436".into(),
            boot_path: "/SCUS_974.36;1".into(),
            system_version: "1.00".into(),
            executable: Identity { size_bytes: image.len() as u64, sha256: sha256_hex(&image) },
            derived: Identity { size_bytes: derived.len() as u64, sha256: sha256_hex(&derived) },
            known_images: Vec::new(),
            // No module check for these synthetic images (the disc crate's tests cover it).
            iop_modules: Vec::new(),
            beta_boot_sha256: sha256_hex(b"beta"),
            patches,
        };
        (image, pins)
    }
}

/// The synthetic Spec II pins in place of the embedded ones, and the selection a start would make with them.
fn use_spec2_pins(shared: &Shared, pins: &gt4_disc_spec2::Spec2Pins) {
    let mut state = lock(shared);
    state.spec2_pins = pins.clone();
    let installed = state.installed_games();
    state.selected = gt4l_core::games::initial_selection(state.config.selected_game.as_deref(), &installed);
}

fn stand_in_programs(root: &Path, names: &[&str]) -> PathBuf {
    let program = root.join("program");
    fs::create_dir_all(program.join("game")).unwrap();
    for name in names {
        fs::write(program.join("game").join(name), b"stand-in, never started").unwrap();
    }
    program
}

#[test]
fn two_games_install_side_by_side_and_never_mix() {
    let root = scratch("two-games");
    let data = root.join("data");
    let dirs = DataDirs::new(&data);
    let game = synthetic_game(0x0010_0000, 0x0010_0008, b"synthetic guest code, not game data, 0123456789abcdef");
    let gt4_image = build_iso(
        "GT4 USA",
        &[
            ("SYSTEM.CNF;1", system_cnf("SCUS_973.28", "2.00", "NTSC")),
            ("CORE.GT4;1", game.core.clone()),
            ("SCUS_973.28;1", vec![0xA5; 3000]),
        ],
    );
    let pins = pins_for(&gt4_image, &game, "SCUS-97328", "2.00");
    let (executable, spec2_pins) = synthetic_spec2();
    let spec2_image = build_iso(
        "GRANTURISMO4",
        &[("SYSTEM.CNF;1", system_cnf("SCUS_974.36", "1.00", "NTSC")), ("SCUS_974.36;1", executable)],
    );
    let gt4_disc = root.join("gt4.iso");
    let spec2_disc = root.join("spec2.iso");
    fs::write(&gt4_disc, &gt4_image).unwrap();
    fs::write(&spec2_disc, &spec2_image).unwrap();

    // First start, Spec II first: the disc decides the game; it becomes the selected and the stored choice.
    let shared = launch(&dirs, &pins);
    use_spec2_pins(&shared, &spec2_pins);
    assert_eq!(screen(&shared)["setup"]["game"], Value::Null, "the first setup has no target: the disc decides");
    let info = setup::choose_disc_path(&shared, &spec2_disc).expect("a Spec II disc is accepted");
    assert_eq!((info.game.as_str(), info.mismatch), ("spec2", false));
    run_setup(&shared, &Recorder::default(), fake_renderer());
    let view = screen(&shared);
    assert_eq!(view["setup"]["state"], "done", "{view}");
    assert_eq!(view["selected_game"], "spec2");
    assert_eq!(lock(&shared).config.selected_game.as_deref(), Some("spec2"));
    assert!(data.join("games/spec2/prepared/SCUS_974.36").is_file());
    assert!(data.join("games/spec2/prepared/prepare.json").is_file());
    assert!(!data.join("games/gt4").exists(), "Gran Turismo 4 is untouched");
    let games_view = commands::games_list_of(&shared).unwrap();
    assert_eq!(games_view["games"][0]["installed"], json!(false));
    assert_eq!(games_view["games"][1]["installed"], json!(true));

    // The home selector moves to Gran Turismo 4, which is not installed: its setup is the "Install" button.
    let view = commands::select_game_of("gt4".into(), &shared).unwrap();
    assert_eq!(view["selected"], "gt4");
    assert_eq!(screen(&shared)["setup"]["state"], "needed");
    assert_eq!(screen(&shared)["any_installed"], json!(true), "the page stays on Home, with Install in place of Play");
    assert_eq!(screen(&shared)["setup"]["game"]["id"], "gt4");

    // The Spec II disc under "Install Gran Turismo 4": the answer says it is another game.
    let wrong = setup::choose_disc_path_for(&shared, &spec2_disc, Some("gt4")).unwrap();
    assert!(wrong.mismatch && wrong.game == "spec2" && wrong.requested.as_deref() == Some("gt4"));
    // The right disc installs Gran Turismo 4; the stored choice (the game played last) does not change.
    let right = setup::choose_disc_path_for(&shared, &gt4_disc, Some("gt4")).unwrap();
    assert!(!right.mismatch && right.game == "gt4");
    let seen = run_setup(&shared, &Recorder::default(), fake_renderer());
    let s = screen(&shared);
    println!("SPEC2 SETUP SEEN: {:?}", seen);
    println!("SPEC2 SETUP VIEW: {:?}", s["setup"]);
    assert_eq!(s["setup"]["state"], "done");
    assert!(data.join("games/gt4/prepared/prepare.json").is_file());
    assert!(data.join("games/spec2/prepared/prepare.json").is_file(), "installing one game leaves the other alone");
    assert_eq!(lock(&shared).config.selected_game.as_deref(), Some("spec2"));
    assert_eq!(lock(&shared).selected.id, "gt4", "the page stays on the game just set up");

    // The next start shows the game played last.
    let second = launch(&dirs, &pins);
    use_spec2_pins(&second, &spec2_pins);
    assert_eq!(screen(&second)["selected_game"], "spec2");
    assert_eq!(commands::games_list_of(&second).unwrap()["games"].as_array().unwrap().iter().filter(|g| g["installed"] == json!(true)).count(), 2);

    // Each game plays with its own program, ELF, disc and card (checked up to the GSdx step, which a test has none of).
    let program = stand_in_programs(&root, &["gt4.exe"]);
    commands::select_game_of("spec2".into(), &second).unwrap();
    let missing = commands::prepare_play(&second, &program).map(|_| ()).unwrap_err();
    assert!(missing.contains("Gran Turismo 4 Spec II") && missing.contains("missing"), "{missing}");
    stand_in_programs(&root, &["gt4-spec2.exe"]);
    let refusal = commands::prepare_play(&second, &program).map(|_| ()).unwrap_err();
    assert!(refusal.starts_with("GSdx could not run the game"), "reaches the shared GSdx check: {refusal}");

    // Saves are apart: an import into Spec II is not in Gran Turismo 4, and the page names the game it shows.
    let card = root.join("card.ps2");
    synthetic_spec2_card(&card);
    let view = commands::import_into_game(&second, &card, Kind::Saves, &gt4l_core::games::SPEC2).unwrap();
    assert_eq!(view.game, "spec2");
    assert_eq!(view.items.len(), 1, "{}", view.status);
    commands::select_game_of("gt4".into(), &second).unwrap();
    let gt4_view = commands::saves_list_of(&second).unwrap();
    assert_eq!((gt4_view.game.as_str(), gt4_view.game_name.as_str()), ("gt4", "Gran Turismo 4"));
    // One list of both games (TASK-101 item 2): the Spec II save is listed with its game id, and no Gran Turismo 4 one.
    assert_eq!(gt4_view.items.iter().filter(|item| item.game == "gt4").count(), 0, "no Gran Turismo 4 save yet");
    assert_eq!(gt4_view.items.iter().filter(|item| item.game == "spec2").count(), 1);
    assert!(gt4_view.folder.replace('\\', "/").contains("games/gt4/Saves"), "{}", gt4_view.folder);
    // An import goes to the game its save belongs to, whatever page is shown; the status says so (TASK-101 item 1).
    assert_eq!(commands::page_game(None, &gt4l_core::games::GT4).id, "gt4");
    assert_eq!(commands::page_game(Some("spec2"), &gt4l_core::games::GT4).id, "spec2");
    assert_eq!(commands::page_game(Some("gt9"), &gt4l_core::games::GT4).id, "gt4");
    let gt4_card = root.join("gt4-card.ps2");
    synthetic_card(&gt4_card);
    commands::select_game_of("spec2".into(), &second).unwrap();
    let routed = commands::import_into_game(&second, &gt4_card, Kind::Saves, &gt4l_core::games::SPEC2).unwrap();
    assert_eq!(routed.game, "spec2", "the page keeps showing Spec II");
    assert!(routed.status.contains("Gran Turismo 4"), "{}", routed.status);
    commands::select_game_of("gt4".into(), &second).unwrap();
    let both = commands::saves_list_of(&second).unwrap().items;
    assert_eq!(both.iter().filter(|item| item.game == "gt4").count(), 1, "the Gran Turismo 4 save went to its own library");
    assert_eq!(both.iter().filter(|item| item.game == "spec2").count(), 1, "the Spec II save is still listed");

    // Remove deletes the disc copy and the prepared files of one game, never its saves or the other game.
    let after = commands::games_remove_of("spec2".into(), &second).unwrap();
    assert_eq!(after["games"][1]["installed"], json!(false));
    assert_eq!(after["games"][0]["installed"], json!(true));
    assert!(!data.join("games/spec2/disc").exists() && !data.join("games/spec2/prepared").exists());
    assert!(data.join("games/gt4/prepared/prepare.json").is_file());
    commands::select_game_of("spec2".into(), &second).unwrap();
    assert_eq!(commands::saves_list_of(&second).unwrap().items.len(), 2, "both games' saves are in the one list");
    let _ = fs::remove_dir_all(&root);
}

#[test]
fn the_disc_is_identified_before_any_byte_is_copied_and_decides_the_game() {
    let root = scratch("identify-first");
    let data = root.join("data");
    let dirs = DataDirs::new(&data);
    let game = synthetic_game(0x0010_0000, 0x0010_0008, b"synthetic guest code, not game data, 0123456789abcdef");
    let gt4_image = build_iso(
        "GT4 USA",
        &[
            ("SYSTEM.CNF;1", system_cnf("SCUS_973.28", "2.00", "NTSC")),
            ("CORE.GT4;1", game.core.clone()),
            ("SCUS_973.28;1", vec![0xA5; 3000]),
        ],
    );
    let pins = pins_for(&gt4_image, &game, "SCUS-97328", "2.00");
    let (executable, spec2_pins) = synthetic_spec2();
    let spec2_image = build_iso(
        "GRANTURISMO4",
        &[("SYSTEM.CNF;1", system_cnf("SCUS_974.36", "1.00", "NTSC")), ("SCUS_974.36;1", executable)],
    );
    let unknown_image = build_iso("UNKNOWN", &[("SYSTEM.CNF;1", system_cnf("SCUS_999.99", "1.00", "NTSC")), ("SCUS_999.99;1", vec![1; 64])]);
    let spec2_disc = root.join("spec2.iso");
    let unknown_disc = root.join("unknown.iso");
    fs::write(&spec2_disc, &spec2_image).unwrap();
    fs::write(&unknown_disc, &unknown_image).unwrap();

    // A stored choice of Spec II does not make the first-run page name it: with nothing installed and no disc identified,
    // the disc decides (rc5 laptop test).
    let shared = launch(&dirs, &pins);
    use_spec2_pins(&shared, &spec2_pins);
    lock(&shared).config.selected_game = Some("spec2".into());
    lock(&shared).selected = &gt4l_core::games::SPEC2;
    assert_eq!(screen(&shared)["setup"]["game"], Value::Null);

    // An unsupported image is refused in place: nothing exists in the data folder, and setup cannot start from it.
    assert!(setup::choose_disc_path(&shared, &unknown_disc).is_err());
    assert!(!data.join("games").exists(), "a refused image copies nothing");
    assert!(setup::start_with(&shared, Recorder::default().notify(), fake_renderer(), no_coregs20()).is_err());
    assert!(!data.join("games").exists());

    // The image is identified again when setup starts: a stale choice cannot send a refused file to the copy.
    setup::choose_disc_path(&shared, &spec2_disc).unwrap();
    lock(&shared).config.disc_path = Some(unknown_disc.clone());
    let refused = setup::start_with(&shared, Recorder::default().notify(), fake_renderer(), no_coregs20()).map(|_| ()).unwrap_err();
    assert!(refused.contains("not supported"), "{refused}");
    assert!(!data.join("games").exists(), "setup start copied nothing for a refused image");

    // Install Gran Turismo 4 was asked for, the image is Spec II: the answer says so and Spec II is what gets installed.
    let info = setup::choose_disc_path_for(&shared, &spec2_disc, Some("gt4")).unwrap();
    assert!(info.mismatch && info.game == "spec2");
    lock(&shared).setup_game = Some(&gt4l_core::games::GT4);
    run_setup(&shared, &Recorder::default(), fake_renderer());
    assert!(data.join("games/spec2/prepared/prepare.json").is_file());
    assert!(!data.join("games/gt4").exists(), "the page asked for Gran Turismo 4 but the image is Spec II");
    let _ = fs::remove_dir_all(&root);
}

#[test]
fn a_1_2_x_install_moves_into_games_gt4_at_the_first_start() {
    let root = scratch("migration");
    let data = root.join("data");
    let dirs = DataDirs::new(&data);
    let game = synthetic_game(0x0010_0000, 0x0010_0008, b"synthetic guest code, not game data, 0123456789abcdef");
    let image = build_iso(
        "GT4 USA",
        &[
            ("SYSTEM.CNF;1", system_cnf("SCUS_973.28", "2.00", "NTSC")),
            ("CORE.GT4;1", game.core.clone()),
            ("SCUS_973.28;1", vec![0xA5; 3000]),
        ],
    );
    let pins = pins_for(&image, &game, "SCUS-97328", "2.00");
    let picked = root.join("gt4.iso");
    fs::write(&picked, &image).unwrap();
    let shared = launch(&dirs, &pins);
    setup::choose_disc_path(&shared, &picked).unwrap();
    run_setup(&shared, &Recorder::default(), fake_renderer());
    drop(shared);
    make_it_an_install_with_a_copy(&data.join("games/gt4"), &picked, "SCUS-97328.iso");

    // Put the files back where 1.2.x kept them: <data>\disc, <data>\prepared, <data>\runtime.
    fs::create_dir_all(data.join("runtime")).unwrap();
    fs::write(data.join("runtime").join("card.ps2"), b"card").unwrap();
    for name in ["disc", "prepared"] {
        fs::rename(data.join("games/gt4").join(name), data.join(name)).unwrap();
    }
    fs::remove_dir_all(data.join("games")).unwrap();

    // The launcher still reads the flat files before the move: nothing is lost if the move fails.
    let shared = launch(&dirs, &pins);
    assert_eq!(setup_state(&shared), "done");
    assert!(lock(&shared).dirs.disc_folder().ends_with("disc") && !lock(&shared).dirs.disc_folder().to_string_lossy().contains("games"));
    crate::relocation::move_large_data(&shared, &gt4l_core::paths::LegacyLocations::default());
    assert!(!lock(&shared).migration.running);
    assert!(data.join("games/gt4/disc/SCUS-97328.iso").is_file(), "moved by a rename");
    assert!(data.join("games/gt4/prepared/SCUS_973.28").is_file());
    assert_eq!(fs::read(data.join("games/gt4/runtime/card.ps2")).unwrap(), b"card");
    assert!(!data.join("disc").exists() && !data.join("prepared").exists() && !data.join("runtime").exists());
    assert_eq!(setup_state(&shared), "done", "Play is available again after the move");
    assert!(lock(&shared).dirs.disc_folder().to_string_lossy().contains("games"));
    let _ = fs::remove_dir_all(&root);
}

#[test]
fn retroachievements_options_and_launch_environment() {
    let root = scratch("ra-options");
    let data = root.join("data");
    let dirs = DataDirs::new(&data);
    let game = synthetic_game(0x0010_0000, 0x0010_0008, b"synthetic guest code, not game data, 0123456789abcdef");
    let gt4_image = build_iso(
        "GT4 USA",
        &[
            ("SYSTEM.CNF;1", system_cnf("SCUS_973.28", "2.00", "NTSC")),
            ("CORE.GT4;1", game.core.clone()),
            ("SCUS_973.28;1", vec![0xA5; 3000]),
        ],
    );
    let pins = pins_for(&gt4_image, &game, "SCUS-97328", "2.00");
    let (executable, spec2_pins) = synthetic_spec2();
    let spec2_image = build_iso(
        "GRANTURISMO4",
        &[("SYSTEM.CNF;1", system_cnf("SCUS_974.36", "1.00", "NTSC")), ("SCUS_974.36;1", executable)],
    );
    let gt4_disc = root.join("gt4.iso");
    let spec2_disc = root.join("spec2.iso");
    fs::write(&gt4_disc, &gt4_image).unwrap();
    fs::write(&spec2_disc, &spec2_image).unwrap();

    let shared = launch(&dirs, &pins);
    use_spec2_pins(&shared, &spec2_pins);

    // 1. Install Spec II first
    let info = setup::choose_disc_path(&shared, &spec2_disc).expect("a Spec II disc is accepted");
    assert_eq!((info.game.as_str(), info.mismatch), ("spec2", false));
    run_setup(&shared, &Recorder::default(), fake_renderer());
    assert_eq!(screen(&shared)["setup"]["state"], "done");

    // 2. Install Gran Turismo 4 second
    commands::select_game_of("gt4".into(), &shared).unwrap();
    let right = setup::choose_disc_path_for(&shared, &gt4_disc, Some("gt4")).unwrap();
    assert!(!right.mismatch && right.game == "gt4");
    run_setup(&shared, &Recorder::default(), fake_renderer());
    let s2 = screen(&shared);
    if s2["setup"]["state"] != "done" {
        panic!("GT4 setup failed! state: {:?}, outcome: {:?}", s2["setup"]["state"], s2["setup"]["outcome"]);
    }

    // 3. Stand-in executables for both
    let program = stand_in_programs(&root, &["gt4.exe", "gt4-spec2.exe"]);

    // 4. Stand-in GSdx installation so prepare_play succeeds
    let gs_dir = data.join("gs");
    let gs_v1 = gs_dir.join("1.2.1");
    fs::create_dir_all(&gs_v1).unwrap();
    fs::write(gs_dir.join("current.txt"), "1.2.1\n").unwrap();
    fs::write(gs_v1.join("gt4-gs.exe"), b"gsdx-stand-in").unwrap();
    fs::write(gs_v1.join("sha256.txt"), "a2b9ce769077cd9e191ec914dd772351d9c2eaa05122592038996809c2f5169c\n").unwrap();

    // 5. Configure RA in user config
    let ra_file = lock(&shared).user_dirs.retroachievements_file();
    let mut cfg = gt4l_core::retroachievements::RaConfig::default();
    cfg.enabled = true;
    cfg.hardcore = true;
    cfg.encore = false;
    cfg.username = "Driver99".to_string();
    cfg.token = "tok_test_123".to_string();
    cfg.save(&ra_file).unwrap();

    // 6. Prepare play for GT4
    commands::select_game_of("gt4".to_string(), &shared).unwrap();
    let play_gt4 = commands::prepare_play(&shared, &program).expect("prepare play succeeds for GT4");
    assert_eq!(play_gt4.game.id, "gt4");

    let extra_map: std::collections::HashMap<_, _> = play_gt4.extra.clone().into_iter().collect();
    assert_eq!(extra_map.get("GT4_RA_ENABLED").map(|s| s.as_str()), Some("1"));
    assert_eq!(extra_map.get("GT4_RA_HARDCORE").map(|s| s.as_str()), Some("1"));
    assert_eq!(extra_map.get("GT4_RA_ENCORE").map(|s| s.as_str()), Some("0"));
    assert_eq!(extra_map.get("GT4_RA_USERNAME").map(|s| s.as_str()), Some("Driver99"));
    assert_eq!(extra_map.get("GT4_RA_TOKEN").map(|s| s.as_str()), Some("tok_test_123"));
    assert_eq!(extra_map.get("GT4_RA_HASH").map(|s| s.as_str()), Some("b7e2e47572f4163a216ea85eed3c6c63"));
    assert_eq!(extra_map.get("GT4_RA_LANGUAGE").map(|s| s.as_str()), Some("en-US"));
    assert!(extra_map.get("GT4_RA_CACHE_DIR").unwrap().contains("games"));
    assert!(extra_map.get("GT4_RA_CACHE_DIR").unwrap().contains("ra_cache"));

    // 7. Switch to Spec II and prepare play: verify RA is disabled, credentials and hash are NOT sent
    commands::select_game_of("spec2".to_string(), &shared).unwrap();
    let play_spec2 = commands::prepare_play(&shared, &program).expect("prepare play succeeds for Spec II");
    assert_eq!(play_spec2.game.id, "spec2");

    let spec2_extra: std::collections::HashMap<_, _> = play_spec2.extra.clone().into_iter().collect();
    assert_eq!(spec2_extra.get("GT4_RA_ENABLED").map(|s| s.as_str()), Some("0"), "Spec II must force GT4_RA_ENABLED=0");
    assert!(!spec2_extra.contains_key("GT4_RA_USERNAME"), "Spec II must NOT receive RA username");
    assert!(!spec2_extra.contains_key("GT4_RA_TOKEN"), "Spec II must NOT receive RA token");
    assert!(!spec2_extra.contains_key("GT4_RA_HASH"), "Spec II must NOT receive original game RA hash");
    assert!(!spec2_extra.contains_key("GT4_RA_HARDCORE"), "Spec II must NOT receive RA hardcore");
    assert!(!spec2_extra.contains_key("GT4_RA_ENCORE"), "Spec II must NOT receive RA encore");

    // 8. End-to-end launch plan composition with hostile inherited variables (including case variations on Windows)
    let hostile_inherited = vec![
        "gt4_ra_token".to_string(),
        "GT4_RA_TOKEN".to_string(),
        "Gt4_Ra_Token".to_string(),
        "gt4_ra_password".to_string(),
        "GT4_RA_PASSWORD".to_string(),
        "Gt4_Ra_Password".to_string(),
        "gt4_ra_enabled".to_string(),
        "GT4_RA_ENABLED".to_string(),
        "Gt4_Ra_Enabled".to_string(),
        "gt4_ra_hash".to_string(),
        "GT4_RA_HASH".to_string(),
        "ps2_disc".to_string(),
        "PS2_DISC".to_string(),
    ];

    // GT4 full launch plan
    let session_gt4 = dirs.session("plan-test-gt4");
    let card_gt4 = dirs.runtime_card();
    let launch_inputs_gt4 = launch::LaunchInputs {
        game_executable: &play_gt4.executable,
        elf: &play_gt4.elf,
        disc: &play_gt4.disc,
        card: &card_gt4,
        session_dir: &session_gt4,
    };
    let textures = lock(&shared).dirs.textures();
    let paths_gt4 = launch::Pcsx2Paths::for_session(play_gt4.pcsx2.as_ref().expect("a GSdx session"), &dirs.root().join("gs"), &session_gt4, &textures);
    let mut plan_gt4 = launch::plan_pcsx2_launch(&launch_inputs_gt4, &play_gt4.settings, &paths_gt4, play_gt4.display.as_ref(), &hostile_inherited);
    plan_gt4.environment.extend(play_gt4.extra);

    for hostile in &hostile_inherited {
        assert!(plan_gt4.removed_inherited.contains(hostile), "Hostile var {hostile} must be in removed_inherited for GT4");
        assert!(!plan_gt4.environment.iter().any(|(k, _)| k == hostile && (k.ends_with("password") || k.ends_with("PASSWORD"))),
            "Hostile password must not remain in environment");
    }
    assert!(plan_gt4.environment.iter().any(|(k, v)| k == "GT4_RA_ENABLED" && v == "1"));
    assert!(plan_gt4.environment.iter().any(|(k, v)| k == "GT4_RA_USERNAME" && v == "Driver99"));
    assert!(plan_gt4.environment.iter().any(|(k, v)| k == "GT4_RA_TOKEN" && v == "tok_test_123"));
    assert!(plan_gt4.environment.iter().any(|(k, v)| k == "GT4_RA_LANGUAGE" && v == "en-US"));

    let plan_gt4_debug = format!("{:?}", plan_gt4);
    assert!(plan_gt4_debug.contains("[REDACTED]"), "Debug output of LaunchPlan must redact token");
    assert!(!plan_gt4_debug.contains("tok_test_123"), "Debug output of LaunchPlan must NEVER print secret token!");

    // Spec II full launch plan
    let session_spec2 = dirs.session("plan-test-spec2");
    let card_spec2 = dirs.runtime_card();
    let launch_inputs_spec2 = launch::LaunchInputs {
        game_executable: &play_spec2.executable,
        elf: &play_spec2.elf,
        disc: &play_spec2.disc,
        card: &card_spec2,
        session_dir: &session_spec2,
    };
    let paths_spec2 = launch::Pcsx2Paths::for_session(play_spec2.pcsx2.as_ref().expect("a GSdx session"), &dirs.root().join("gs"), &session_spec2, &textures);
    let mut plan_spec2 = launch::plan_pcsx2_launch(&launch_inputs_spec2, &play_spec2.settings, &paths_spec2, play_spec2.display.as_ref(), &hostile_inherited);
    plan_spec2.environment.extend(play_spec2.extra);

    for hostile in &hostile_inherited {
        assert!(plan_spec2.removed_inherited.contains(hostile), "Hostile var {hostile} must be in removed_inherited for Spec II");
    }
    assert!(plan_spec2.environment.iter().any(|(k, v)| k == "GT4_RA_ENABLED" && v == "0"), "Spec II plan must force GT4_RA_ENABLED=0");
    assert!(!plan_spec2.environment.iter().any(|(k, _)| k == "GT4_RA_USERNAME" || k == "GT4_RA_TOKEN" || k == "GT4_RA_HASH" || k.ends_with("PASSWORD")),
        "Spec II plan must NEVER contain RA credentials, token, password, or hash");

    let _ = fs::remove_dir_all(&root);
}

#[test]
fn retroachievements_stale_response_and_target_switch_races() {
    let root = scratch("ra-races");
    let data = root.join("data");
    let dirs = DataDirs::new(&data);
    let shared = launch(&dirs, &Pins::embedded().unwrap());

    // 1. Initial login
    let ra_file = lock(&shared).user_dirs.retroachievements_file();
    let mut cfg = gt4l_core::retroachievements::RaConfig::default();
    cfg.username = "Racer1".to_string();
    cfg.token = "tok_initial".to_string();
    cfg.save(&ra_file).unwrap();

    // 2. Race 1: While status refresh is in flight, target switches to Spec II
    let status_view = commands::ra_status_with(&shared, |_, _| {
        // While background network runs, UI/user selects Spec II
        commands::select_game_of("spec2".to_string(), &shared).unwrap();
        (
            Some(gt4l_core::retroachievements::RaAccountSession {
                username: "Racer1".to_string(),
                token: "tok_initial".to_string(),
                display_name: "Racer One".to_string(),
                score: 500,
                score_softcore: 100,
            }),
            Some(gt4l_core::retroachievements::RaGameOverview {
                game_id: 3843,
                title: "Gran Turismo 4".to_string(),
                total_achievements: 75,
                unlocked_achievements: 10,
                total_points: 600,
                unlocked_points: 80,
                achievements: Vec::new(),
            }),
        )
    }).expect("ra_status_with succeeds");

    // Because state.selected changed to Spec II and revision bumped, stale overview is discarded
    assert_eq!(status_view.selected_game, "spec2");
    assert!(!status_view.supported, "Spec II must report supported: false");
    assert!(status_view.game.is_none(), "Stale GT4 overview must be dropped when Spec II is active");

    // 3. Race 2: While status refresh is in flight, user logs out
    commands::select_game_of("gt4".to_string(), &shared).unwrap();
    let status_after_logout = commands::ra_status_with(&shared, |_, _| {
        // User clicks Sign Out while network response is delayed
        commands::ra_logout_of(&shared).unwrap();
        (
            Some(gt4l_core::retroachievements::RaAccountSession {
                username: "Racer1".to_string(),
                token: "tok_initial".to_string(),
                display_name: "Racer One".to_string(),
                score: 500,
                score_softcore: 100,
            }),
            None,
        )
    }).expect("ra_status_with succeeds");

    // Account remains signed out, stale session is dropped
    assert!(!status_after_logout.signed_in);
    assert_eq!(status_after_logout.username, "");
    assert_eq!(status_after_logout.score, 0);

    let _ = fs::remove_dir_all(&root);
}

#[test]
fn retroachievements_network_race_tests_with_real_tcp_server() {
    use std::io::{Read, Write};
    use std::net::TcpListener;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::mpsc::{channel, Receiver, Sender};
    use std::sync::{Arc, Mutex};
    use std::thread;
    use std::time::Duration;

    struct SyncGate {
        trigger: String,
        arrived_tx: Sender<()>,
        release_rx: Receiver<()>,
    }

    let gate: Arc<Mutex<Option<SyncGate>>> = Arc::new(Mutex::new(None));
    let gate_srv = gate.clone();

    let listener = TcpListener::bind("127.0.0.1:0").expect("bind test server");
    let addr = listener.local_addr().unwrap();
    let base_url = format!("http://127.0.0.1:{}", addr.port());
    listener.set_nonblocking(true).unwrap();

    let stop = Arc::new(AtomicBool::new(false));
    let stop_srv = stop.clone();

    let server_thread = thread::spawn(move || {
        while !stop_srv.load(Ordering::Relaxed) {
            match listener.accept() {
                Ok((mut stream, _)) => {
                    let _ = stream.set_nonblocking(false);
                    let _ = stream.set_read_timeout(Some(Duration::from_secs(3)));
                    let gate_conn = gate_srv.clone();
                    thread::spawn(move || {
                        let mut buf = [0u8; 4096];
                        let mut read_bytes = 0;
                        while read_bytes < buf.len() {
                            match stream.read(&mut buf[read_bytes..]) {
                                Ok(0) => break,
                                Ok(n) => {
                                    read_bytes += n;
                                    let text = String::from_utf8_lossy(&buf[..read_bytes]);
                                    if let Some(pos) = text.find("\r\n\r\n") {
                                        let cl = text.lines()
                                            .find(|l| l.to_ascii_lowercase().starts_with("content-length:"))
                                            .and_then(|l| l.split(':').nth(1))
                                            .and_then(|s| s.trim().parse::<usize>().ok())
                                            .unwrap_or(0);
                                        if read_bytes >= pos + 4 + cl {
                                            break;
                                        }
                                    }
                                }
                                Err(_) => break,
                            }
                        }
                        if read_bytes == 0 {
                            return;
                        }
                        let req = String::from_utf8_lossy(&buf[..read_bytes]);

                        // Interrupted network connection route
                        if req.contains("mode_abort") || req.starts_with("POST /abort") {
                            drop(stream);
                            return;
                        }

                        // HTTP 401 Unauthorized route
                        if req.contains("badpass_http_401") {
                            let resp = "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                            let _ = stream.write_all(resp.as_bytes());
                            let _ = stream.flush();
                            let _ = stream.shutdown(std::net::Shutdown::Write);
                            return;
                        }

                        // Service error route (HTTP 200 with JSON Success:false)
                        if req.contains("badpass_service_fail") || req.contains("mode_fail") {
                            let body = r#"{"Success":false,"Error":"Invalid user or password."}"#;
                            let resp = format!("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", body.len(), body);
                            let _ = stream.write_all(resp.as_bytes());
                            let _ = stream.flush();
                            let _ = stream.shutdown(std::net::Shutdown::Write);
                            return;
                        }

                        // Check coordination gate
                        let active_gate = {
                            let mut g = gate_conn.lock().unwrap();
                            if let Some(ref current) = *g {
                                if req.contains(&current.trigger) {
                                    g.take()
                                } else {
                                    None
                                }
                            } else {
                                None
                            }
                        };
                        if let Some(active) = active_gate {
                            let _ = active.arrived_tx.send(());
                            // Bounded wait: max 3 seconds to guarantee server never hangs tests
                            let _ = active.release_rx.recv_timeout(Duration::from_secs(3));
                        }

                        // RA Game Overview endpoints
                        if req.contains("r=patch") {
                            let body = r#"{"Success":true,"PatchData":{"ID":20387,"Title":"Gran Turismo 4","Achievements":[{"ID":101,"Title":"First Win","Description":"Win race","Points":10,"Flags":3,"BadgeName":"00001"}]}}"#;
                            let resp = format!("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", body.len(), body);
                            let _ = stream.write_all(resp.as_bytes());
                            let _ = stream.flush();
                            let _ = stream.shutdown(std::net::Shutdown::Write);
                            return;
                        }
                        if req.contains("r=startsession") {
                            let body = r#"{"Success":true,"Unlocks":[],"HardcoreUnlocks":[{"ID":101}]}"#;
                            let resp = format!("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", body.len(), body);
                            let _ = stream.write_all(resp.as_bytes());
                            let _ = stream.flush();
                            let _ = stream.shutdown(std::net::Shutdown::Write);
                            return;
                        }

                        let user = if req.contains("UserOld") {
                            "UserOld"
                        } else if req.contains("UserNew") {
                            "UserNew"
                        } else if req.contains("UserA") {
                            "UserA"
                        } else if req.contains("RefreshUser") {
                            "RefreshUser"
                        } else if req.contains("TargetUser") {
                            "TargetUser"
                        } else {
                            "TestRacer"
                        };
                        let body = format!(
                            r#"{{"Success":true,"User":"{}","Token":"tok_{}","Score":2500,"SoftcoreScore":500}}"#,
                            user, user
                        );
                        let resp = format!("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}", body.len(), body);
                        let _ = stream.write_all(resp.as_bytes());
                        let _ = stream.flush();
                        let _ = stream.shutdown(std::net::Shutdown::Write);
                    });
                }
                Err(ref e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                    thread::sleep(Duration::from_millis(5));
                }
                Err(_) => break,
            }
        }
    });

    let root = scratch("ra-net-races");
    let data = root.join("data");
    let dirs = DataDirs::new(&data);
    let shared = launch(&dirs, &Pins::embedded().unwrap());
    let ra_path = lock(&shared).user_dirs.retroachievements_file();

    // 1. Delayed Login vs Logout Race (Coordinated via Bounded Channel Gate)
    {
        let (arrived_tx, arrived_rx) = channel();
        let (release_tx, release_rx) = channel();
        *gate.lock().unwrap() = Some(SyncGate {
            trigger: "UserA".to_string(),
            arrived_tx,
            release_rx,
        });

        let shared1 = shared.clone();
        let url1 = base_url.clone();
        let login_thread1 = thread::spawn(move || {
            commands::ra_login_at(&url1, "UserA", "pass_wait", &shared1)
        });

        // Deterministically wait for fake server to receive UserA request
        arrived_rx.recv_timeout(Duration::from_secs(3)).expect("UserA request arrived at server");

        // Mutate local state while response is held by fake server
        commands::ra_logout_of(&shared).expect("logout succeeds");
        let cfg_mid = gt4l_core::retroachievements::RaConfig::load(&ra_path);
        assert!(!cfg_mid.is_signed_in(), "Disk config must be signed out immediately");

        // Release fake server response
        release_tx.send(()).expect("release gate");

        let res1 = login_thread1.join().unwrap().expect("login call completes");
        assert!(!res1.signed_in, "Delayed login must not sign in after concurrent logout");
        let cfg1 = gt4l_core::retroachievements::RaConfig::load(&ra_path);
        assert!(!cfg1.is_signed_in(), "Disk config must remain signed out after concurrent logout");
        assert_eq!(cfg1.username, "");
    }

    // 2. Delayed Login vs Newer Login Race (UserOld vs UserNew)
    {
        let (arrived_tx, arrived_rx) = channel();
        let (release_tx, release_rx) = channel();
        *gate.lock().unwrap() = Some(SyncGate {
            trigger: "UserOld".to_string(),
            arrived_tx,
            release_rx,
        });

        let shared2 = shared.clone();
        let url2 = base_url.clone();
        let login_old = thread::spawn(move || {
            commands::ra_login_at(&url2, "UserOld", "pass_wait", &shared2)
        });

        // Deterministically wait for fake server to receive UserOld request
        arrived_rx.recv_timeout(Duration::from_secs(3)).expect("UserOld request arrived at server");

        // Execute newer login immediately while UserOld response is held
        let res_new = commands::ra_login_at(&base_url, "UserNew", "pass_fast", &shared)
            .expect("newer login succeeds");
        assert!(res_new.signed_in);
        assert_eq!(res_new.username, "UserNew");
        let cfg_mid = gt4l_core::retroachievements::RaConfig::load(&ra_path);
        assert_eq!(cfg_mid.username, "UserNew");

        // Release UserOld response
        release_tx.send(()).expect("release gate");

        let res_old = login_old.join().unwrap().expect("old login completes without panic");
        assert_ne!(res_old.username, "UserOld", "Stale login result must never apply UserOld");

        let cfg2 = gt4l_core::retroachievements::RaConfig::load(&ra_path);
        assert_eq!(cfg2.username, "UserNew", "Disk config must retain UserNew and not be overwritten by UserOld");
        assert_eq!(cfg2.token, "tok_UserNew");
    }

    // 3. Delayed Refresh vs Logout Race (Exercising Real ra_status_at Network Path)
    {
        let mut cfg_ref = gt4l_core::retroachievements::RaConfig::default();
        cfg_ref.username = "RefreshUser".to_string();
        cfg_ref.token = "tok_refresh_123".to_string();
        cfg_ref.score = 1000;
        cfg_ref.save(&ra_path).unwrap();

        let (arrived_tx, arrived_rx) = channel();
        let (release_tx, release_rx) = channel();
        *gate.lock().unwrap() = Some(SyncGate {
            trigger: "RefreshUser".to_string(),
            arrived_tx,
            release_rx,
        });

        let shared3 = shared.clone();
        let url3 = base_url.clone();
        let refresh_thread = thread::spawn(move || {
            commands::ra_status_at(&url3, &shared3)
        });

        // Deterministically wait for fake server to receive RefreshUser request
        arrived_rx.recv_timeout(Duration::from_secs(3)).expect("RefreshUser request arrived at server");

        // Concurrently logout while refresh response is held
        commands::ra_logout_of(&shared).expect("logout succeeds");
        let cfg_mid = gt4l_core::retroachievements::RaConfig::load(&ra_path);
        assert!(!cfg_mid.is_signed_in());

        // Release server response
        release_tx.send(()).expect("release gate");

        let res3 = refresh_thread.join().unwrap().expect("refresh completes");
        assert!(!res3.signed_in, "Delayed refresh must not restore signed-in state after logout");
        let cfg3 = gt4l_core::retroachievements::RaConfig::load(&ra_path);
        assert!(!cfg3.is_signed_in(), "Disk config must remain signed out");
        assert_eq!(cfg3.username, "");
    }

    // 4. Delayed Refresh vs Selected Game Switch Race (Exercising Real ra_status_at Network Path)
    {
        let mut cfg_target = gt4l_core::retroachievements::RaConfig::default();
        cfg_target.username = "TargetUser".to_string();
        cfg_target.token = "tok_target_123".to_string();
        cfg_target.score = 2500;
        cfg_target.save(&ra_path).unwrap();
        assert_eq!(lock(&shared).selected.id, "gt4");

        let (arrived_tx, arrived_rx) = channel();
        let (release_tx, release_rx) = channel();
        *gate.lock().unwrap() = Some(SyncGate {
            trigger: "TargetUser".to_string(),
            arrived_tx,
            release_rx,
        });

        let shared4 = shared.clone();
        let url4 = base_url.clone();
        let refresh_switch = thread::spawn(move || {
            commands::ra_status_at(&url4, &shared4)
        });

        // Deterministically wait for fake server to receive TargetUser request
        arrived_rx.recv_timeout(Duration::from_secs(3)).expect("TargetUser request arrived at server");

        // Switch selected game to spec2 while response is held
        commands::select_game_of("spec2".to_string(), &shared).expect("select spec2 succeeds");
        assert_eq!(lock(&shared).selected.id, "spec2");

        // Release server response
        release_tx.send(()).expect("release gate");

        let res_switch = refresh_switch.join().unwrap().expect("delayed refresh completes");
        assert_eq!(res_switch.selected_game, "spec2");
        assert!(!res_switch.supported, "Spec II must be marked unsupported");
        assert!(res_switch.game.is_none(), "Stale GT4 game overview must be discarded when game changed to spec2");

        // Assert persisted account on disk is preserved
        let cfg_target_disk = gt4l_core::retroachievements::RaConfig::load(&ra_path);
        assert_eq!(cfg_target_disk.username, "TargetUser");
        assert_eq!(cfg_target_disk.token, "tok_target_123");

        // Restore GT4 target
        commands::select_game_of("gt4".to_string(), &shared).expect("restore gt4 succeeds");
    }

    // 5. Interrupted Network Connection: Preserves Account State & Returns View Gracefully
    {
        let mut cfg5 = gt4l_core::retroachievements::RaConfig::default();
        cfg5.username = "StableUser".to_string();
        cfg5.token = "tok_stable_123".to_string();
        cfg5.score = 777;
        cfg5.save(&ra_path).unwrap();

        // ra_status_at catches network drop via .ok() and returns Ok(RaStatusView) from local cache
        let res_abort = commands::ra_status_at(&format!("{}/abort", base_url), &shared);
        assert!(res_abort.is_ok(), "ra_status_at returns Ok(view) falling back to local state on network error");
        let view_abort = res_abort.unwrap();
        assert_eq!(view_abort.username, "StableUser");
        assert_eq!(view_abort.score, 777);
        assert!(view_abort.game.is_none(), "Game overview is None when network disconnected");

        let cfg_after_abort = gt4l_core::retroachievements::RaConfig::load(&ra_path);
        assert_eq!(cfg_after_abort.username, "StableUser");
        assert_eq!(cfg_after_abort.token, "tok_stable_123");
        assert_eq!(cfg_after_abort.score, 777);
    }

    // 6. Failed Login: HTTP 200 Service Error vs Genuine HTTP 401 (Both Preserve Account State)
    {
        let mut cfg6 = gt4l_core::retroachievements::RaConfig::default();
        cfg6.username = "StableUser".to_string();
        cfg6.token = "tok_stable_123".to_string();
        cfg6.score = 777;
        cfg6.save(&ra_path).unwrap();

        // 6A: Service JSON Error with HTTP 200 OK
        let res_svc_fail = commands::ra_login_at(&base_url, "StableUser", "badpass_service_fail", &shared);
        assert!(res_svc_fail.is_err(), "Service error must return Err");
        assert!(res_svc_fail.unwrap_err().contains("Invalid user or password"));
        let cfg_after_svc_fail = gt4l_core::retroachievements::RaConfig::load(&ra_path);
        assert_eq!(cfg_after_svc_fail.username, "StableUser");
        assert_eq!(cfg_after_svc_fail.token, "tok_stable_123");
        assert_eq!(cfg_after_svc_fail.score, 777);

        // 6B: Genuine HTTP 401 Unauthorized
        let res_401_fail = commands::ra_login_at(&base_url, "StableUser", "badpass_http_401", &shared);
        assert!(res_401_fail.is_err(), "HTTP 401 must return Err");
        let err_401 = res_401_fail.unwrap_err();
        assert!(
            err_401.contains("Unexpected response from RetroAchievements") || err_401.contains("401"),
            "Error message must reflect non-JSON response from HTTP 401: {err_401}"
        );
        let cfg_after_401_fail = gt4l_core::retroachievements::RaConfig::load(&ra_path);
        assert_eq!(cfg_after_401_fail.username, "StableUser");
        assert_eq!(cfg_after_401_fail.token, "tok_stable_123");
        assert_eq!(cfg_after_401_fail.score, 777);
    }

    // 7. Unprotectable Token Envelope Surfaces relogin_required with Inaccessible Error Without Network Call
    {
        let unprotectable_toml = r#"
username = "PortableTraveler"
display_name = "Portable Traveler"
token_protected = "dpapi:v1:01000000d08c9ddf0115d1118c7a00c04fc297eb01000000ffffffffffffffff"
enabled = true
hardcore = true
score = 3000
"#;
        fs::write(&ra_path, unprotectable_toml).unwrap();

        let status_res = commands::ra_status_at(&base_url, &shared);
        assert!(status_res.is_ok(), "ra_status_at must succeed without panicking");
        let view = status_res.unwrap();
        assert!(!view.signed_in, "Unprotectable token must NOT report signed_in");
        assert!(view.relogin_required, "Unprotectable token MUST set relogin_required = true");
        assert_eq!(view.token_error_kind, Some(gt4l_core::retroachievements::TokenErrorKind::Inaccessible));
        assert_eq!(view.username, "PortableTraveler");
        assert_eq!(view.display_name, "Portable Traveler");
        assert_eq!(view.score, 3000);
        assert!(view.hardcore);

        let disk_current = fs::read_to_string(&ra_path).unwrap();
        assert!(disk_current.contains("token_protected"), "Ciphertext must be preserved on disk");
    }

    // 8. Migration & Write Failure at Launcher Command Boundary
    {
        let legacy_toml = r#"username = "PreservedUser"
token = "tok_do_not_erase_me"
enabled = true
hardcore = false
"#;
        fs::write(&ra_path, legacy_toml).unwrap();

        let fail_protector = gt4l_core::secret::FakeProtector { fail_protect: true, ..gt4l_core::secret::FakeProtector::default() };

        // 8A. Initial status query via commands::ra_status_at_with detects migration failure without crashing
        let status_res = commands::ra_status_at_with(&base_url, &shared, &fail_protector);
        assert!(status_res.is_ok(), "ra_status_at_with must succeed on migration failure");
        let view = status_res.unwrap();
        assert!(!view.signed_in);
        assert!(view.relogin_required);
        assert_eq!(view.token_error_kind, Some(gt4l_core::retroachievements::TokenErrorKind::MigrationFailed));
        assert_eq!(fs::read_to_string(&ra_path).unwrap(), legacy_toml, "Legacy file must be 100% untouched after status check");

        // 8B. Command boundary: ra_set_options_with fails save and preserves original legacy file 100% untouched
        let opt_res = commands::ra_set_options_with(true, true, false, &shared, &fail_protector);
        assert!(opt_res.is_err(), "ra_set_options_with must return Err when migration protection fails");
        let opt_err = opt_res.unwrap_err();
        assert!(opt_err.contains("legacy credentials could not be protected"), "Error message must explain migration safety abort: {opt_err}");
        assert_eq!(fs::read_to_string(&ra_path).unwrap(), legacy_toml, "Legacy config must be preserved intact after option toggle failure");

        // 8C. Command boundary with injected atomic partial write failure
        // Uses a working protector so secret protection succeeds and the partial-write filesystem seam is reached.
        let working_protector = gt4l_core::secret::FakeProtector::default();
        let partial_write_ops = gt4l_core::retroachievements::MockAtomicFileOps {
            fail_during_write: Some(10),
            ..Default::default()
        };
        let write_fail_res = commands::ra_set_options_with_custom(true, true, false, &shared, &working_protector, &partial_write_ops);
        assert!(write_fail_res.is_err(), "ra_set_options_with_custom must return Err on write failure");
        let write_err = write_fail_res.unwrap_err();
        assert!(
            write_err.contains("cannot write") || write_err.contains("injected partial write failure"),
            "Error must report write failure, not protection failure: {write_err}"
        );
        assert!(
            partial_write_ops.partial_write_reached.load(Ordering::SeqCst),
            "Filesystem seam must prove partial write was actually reached"
        );
        assert_eq!(
            fs::read_to_string(&ra_path).unwrap(),
            legacy_toml,
            "Legacy config must be preserved intact byte-for-byte after partial write failure"
        );
        // Verify owned temp cleanup occurred and no temporary files remain in folder
        let temp_files_in_folder: Vec<_> = fs::read_dir(ra_path.parent().unwrap()).unwrap()
            .filter_map(|e| e.ok())
            .filter(|e| e.file_name().to_string_lossy().starts_with(".retroachievements.toml.tmp-"))
            .collect();
        assert_eq!(temp_files_in_folder.len(), 0, "Owned temporary files must be cleaned up on write failure");

        // 8D. Competing launcher action at command boundary while network reply is pending:
        // Instead of manual revision increments, an actual competing launcher command (ra_logout_with)
        // is executed while the server gate holds an in-flight login request.
        let (arrived_tx, arrived_rx) = channel();
        let (release_tx, release_rx) = channel();
        *gate.lock().unwrap() = Some(SyncGate {
            trigger: "StaleCandidate".to_string(),
            arrived_tx,
            release_rx,
        });

        let shared_stale = shared.clone();
        let url_stale = base_url.clone();
        let working_protector_clone = working_protector.clone();
        let stale_login_task = thread::spawn(move || {
            commands::ra_login_at_with(&url_stale, "StaleCandidate", "pass_wait", &shared_stale, &working_protector_clone)
        });

        arrived_rx.recv_timeout(Duration::from_secs(3)).expect("StaleCandidate request arrived at server");

        // Execute a REAL competing launcher command at the boundary while StaleCandidate is in flight:
        let logout_competing_res = commands::ra_logout_with(&shared, &working_protector);
        assert!(logout_competing_res.is_ok(), "Competing ra_logout_with must succeed");
        let logout_mid_view = logout_competing_res.unwrap();
        assert!(!logout_mid_view.signed_in);
        assert_eq!(logout_mid_view.username, "");

        // Verify disk is immediately updated to logged out state by the competing command
        let disk_mid = fs::read_to_string(&ra_path).unwrap();
        assert!(!disk_mid.contains("PreservedUser"));
        assert!(!disk_mid.contains("tok_do_not_erase_me"));

        // Release delayed server response for StaleCandidate
        release_tx.send(()).expect("release gate");
        let stale_res = stale_login_task.join().unwrap().expect("stale login completes without panic");

        // The late response must NOT apply StaleCandidate and must NOT resurrect the old legacy token
        assert_ne!(stale_res.username, "StaleCandidate", "Late reply must NOT apply StaleCandidate");
        assert!(!stale_res.signed_in, "Late reply must NOT report signed_in after competing logout");

        // Verify disk still preserves the result of the competing command: no StaleCandidate, no legacy token
        let disk_after_stale = fs::read_to_string(&ra_path).unwrap();
        assert!(!disk_after_stale.contains("StaleCandidate"), "Late reply must NOT write to disk");
        assert!(!disk_after_stale.contains("tok_do_not_erase_me"), "Late reply must NOT resurrect legacy credentials");

        // 8E. Replacement login via commands::ra_login_at_with succeeds and cleanly replaces legacy credentials
        let login_res = commands::ra_login_at_with(&base_url, "FreshUser", "goodpass", &shared, &working_protector);
        assert!(login_res.is_ok(), "Replacement login must succeed");
        let login_view = login_res.unwrap();
        assert!(login_view.signed_in);
        assert_eq!(login_view.username, "FreshUser");
        assert_eq!(login_view.token_error_kind, None);
        let disk_after_login = fs::read_to_string(&ra_path).unwrap();
        assert!(!disk_after_login.contains("tok_do_not_erase_me"), "Old plaintext token must be gone");
        assert!(disk_after_login.contains("token_protected"), "New credentials must be saved as protected envelope");

        // 8F. Explicit logout via commands::ra_logout_with cleanly removes credentials
        let logout_res = commands::ra_logout_with(&shared, &working_protector);
        assert!(logout_res.is_ok(), "Explicit logout must succeed");
        let logout_view = logout_res.unwrap();
        assert!(!logout_view.signed_in);
        assert_eq!(logout_view.username, "");
        let disk_after_logout = fs::read_to_string(&ra_path).unwrap();
        assert!(!disk_after_logout.contains("FreshUser"));
        assert!(!disk_after_logout.contains("token_protected"));
        assert!(!disk_after_logout.contains("tok_do_not_erase_me"));
    }

    stop.store(true, Ordering::Relaxed);
    let _ = server_thread.join();
    let _ = fs::remove_dir_all(&root);
}

#[test]
fn the_active_save_cannot_be_deleted_while_the_game_runs() {
    let (root, _dirs, _pins, shared) = new_install("delete-active-game-running");
    let library_save = {
        let state = lock(&shared);
        let active = state.library_of(state.selected).active_save_dir();
        fs::create_dir_all(&active).unwrap();
        fs::write(active.join("icon.sys"), b"synthetic save, not game data").unwrap();
        active
    };
    lock(&shared).game_child = Some(Arc::new(std::sync::Mutex::new(None)));
    let id = gt4l_core::games::GT4.save_folder;
    let error = commands::saves_delete_of(id.to_string(), &shared).unwrap_err();
    assert_eq!(error, "Close the game before you delete the active save.");
    assert!(library_save.is_dir(), "the save is still there");
    let _ = fs::remove_dir_all(&root);
}

#[test]
fn play_names_the_first_start_keys_until_the_game_has_a_save() {
    // TASK-164 (issue #160): a new install has no save, so Play first says which keys answer the game's first-start questions,
    // from the default controls; once the library holds the active save, the note is gone.
    let (root, _dirs, _pins, shared) = new_install("first-start-hint");
    let hint = commands::first_start_hint_of(&shared);
    assert_eq!(hint["game"], gt4l_core::games::GT4.display_name);
    assert_eq!(hint["confirm"], "W / Enter", "{hint}");
    assert_eq!(hint["pad"], "A", "{hint}");
    assert_eq!(hint["right"], "Right", "{hint}");
    {
        let state = lock(&shared);
        let active = state.library_of(state.selected).active_save_dir();
        fs::create_dir_all(&active).unwrap();
        fs::write(active.join("icon.sys"), b"synthetic save, not game data").unwrap();
    }
    assert!(commands::first_start_hint_of(&shared).is_null());
    let _ = fs::remove_dir_all(&root);
}

/// A guard file as the renderer writes it (`GuardFileHeader` in core2_gpu.cpp) with 3-word keys.
fn synthetic_guard_file(flags: u32, keys: &[[u32; 3]]) -> Vec<u8> {
    let mut bytes = b"GT4GRD1\0".to_vec();
    bytes.extend_from_slice(&0xA1B2_C3D4_E5F6_0718u64.to_le_bytes());
    for value in [0x1002u32, 0x73df, 0x0003_0002, 3, flags, keys.len() as u32] {
        bytes.extend_from_slice(&value.to_le_bytes());
    }
    for word in keys.iter().flatten() {
        bytes.extend_from_slice(&word.to_le_bytes());
    }
    bytes
}

#[test]
fn the_log_bundle_lists_the_shader_guard_files_and_nothing_else_of_the_cache() {
    // TASK-170b: quarantine.bin and compiling.bin of the shader cache become logs/shader-guard.txt (header fields, keys as hex);
    // the binary files and the rest of the cache folder never go in.
    let (root, dirs, _pins, shared) = new_install("bundle-shader-guard");
    let cache = lock(&shared).texture_location().0.shader_cache.expect("a shader cache folder");
    fs::create_dir_all(&cache).unwrap();
    fs::write(cache.join("quarantine.bin"), synthetic_guard_file(1, &[[0x11, 0x22, 0x33], [0xcafe, 0, 1]])).unwrap();
    fs::write(cache.join("compiling.bin"), synthetic_guard_file(0, &[[7, 8, 9]])).unwrap();
    fs::write(cache.join("variants.bin"), b"pipeline cache bytes").unwrap();
    let entries = crate::log_bundle::collect(&lock(&shared), &[]).unwrap();
    let guard = entries.iter().find(|entry| entry.path == "logs/shader-guard.txt").expect("logs/shader-guard.txt in the bundle");
    assert!(guard.text.contains("quarantine.bin") && guard.text.contains("compiling.bin"));
    assert!(guard.text.contains("a1b2c3d4e5f60718") && guard.text.contains("vendor 1002, device 73df"));
    assert!(guard.text.contains("00000011 00000022 00000033") && guard.text.contains("0000cafe 00000000 00000001"));
    assert!(guard.text.contains("00000007 00000008 00000009"));
    assert!(entries.iter().all(|entry| !gt4l_core::log_export::is_forbidden(&entry.path)), "listing is text, not a .bin");
    assert!(entries.iter().all(|entry| !entry.text.contains("pipeline cache bytes")), "nothing else of the cache");
    drop(dirs);
    let _ = fs::remove_dir_all(&root);
}

#[test]
fn the_log_bundle_names_a_damaged_guard_file_and_has_no_listing_without_files() {
    let (root, _dirs, _pins, shared) = new_install("bundle-shader-guard-damaged");
    let none = crate::log_bundle::collect(&lock(&shared), &[]).unwrap();
    assert!(none.iter().all(|entry| entry.path != "logs/shader-guard.txt"));
    let cache = lock(&shared).texture_location().0.shader_cache.expect("a shader cache folder");
    fs::create_dir_all(&cache).unwrap();
    let mut damaged = synthetic_guard_file(0, &[[1, 2, 3]]);
    damaged.truncate(damaged.len() - 2);
    fs::write(cache.join("quarantine.bin"), damaged).unwrap();
    let entries = crate::log_bundle::collect(&lock(&shared), &[]).unwrap();
    let guard = entries.iter().find(|entry| entry.path == "logs/shader-guard.txt").expect("a damaged file is still named");
    assert!(guard.text.contains("quarantine.bin") && guard.text.contains("damaged"));
    let _ = fs::remove_dir_all(&root);
}

#[test]
fn the_log_bundle_system_report_has_the_machine_facts_and_no_personal_value() {
    // The owner's system report: processor, memory, system, drivers, displays, storage, power and pads. A fact that cannot be
    // read says "unknown" and never fails the bundle. `--nocapture` prints the report this machine makes.
    let (root, _dirs, _pins, shared) = new_install("bundle-system-facts");
    let pads = vec!["Xbox Wireless Controller".to_string()];
    let entries = crate::log_bundle::collect(&lock(&shared), &pads).unwrap();
    let system = entries.iter().find(|entry| entry.path == "system.txt").expect("system.txt in the bundle");
    println!("{}", system.text);
    for key in [
        "cpu cores: ", "cpu features: ", "memory: ", "windows edition: ", "wine: ", "storage: ", "power: ", "controller 1: Xbox Wireless Controller",
    ] {
        assert!(system.text.lines().any(|line| line.starts_with(key)), "missing {key:?} in\n{}", system.text);
    }
    // The existing lines keep their meaning.
    for key in ["launcher: GT4Recomp ", "install: ", "windows: ", "cpu: ", "chosen card: ", "selected game: "] {
        assert!(system.text.lines().any(|line| line.starts_with(key)), "missing {key:?}");
    }
    // The storage line names the drive letter and nothing else of the path.
    let storage = system.text.lines().find(|line| line.starts_with("storage: ")).unwrap();
    assert!(!storage.contains('\\') && !storage.contains(root.to_string_lossy().as_ref()), "{storage}");
    // No computer name and no user name.
    if let Ok(computer) = std::env::var("COMPUTERNAME") {
        assert!(!system.text.to_lowercase().contains(&computer.to_lowercase()), "the computer name is in the report");
    }
    let _ = fs::remove_dir_all(&root);
}

#[test]
fn the_pad_names_of_the_log_bundle_come_from_the_pad_status_without_serials() {
    let status = serde_json::json!({
        "pads": [
            { "name": "Xbox Wireless Controller", "kind": "xbox", "key": "guid:serial-123", "active": true },
            { "name": "  ", "kind": "x", "key": "k", "active": false },
            { "name": "DualSense Wireless Controller", "kind": "ps5", "key": "guid2:serial-456", "active": false }
        ]
    });
    let names = crate::log_bundle::controller_names(&status);
    assert_eq!(names, vec!["Xbox Wireless Controller".to_string(), "DualSense Wireless Controller".to_string()]);
    assert!(crate::log_bundle::controller_names(&serde_json::Value::Null).is_empty());
}

#[test]
fn the_log_bundle_carries_the_shader_prepare_log() {
    // TASK-170: a compile that stops (a driver that ends the process) is named in logs/shaders.log; bug reports need it.
    let (root, dirs, _pins, shared) = new_install("bundle-shaders-log");
    fs::create_dir_all(dirs.logs()).unwrap();
    fs::write(dirs.logs().join("shaders.log"), "[core2] shader guard: 1 variants quarantined\n").unwrap();
    let entries = crate::log_bundle::collect(&lock(&shared), &[]).unwrap();
    let shaders = entries.iter().find(|entry| entry.path == "logs/shaders.log").expect("logs/shaders.log in the bundle");
    assert!(shaders.text.contains("shader guard"));
    assert!(entries.iter().all(|entry| !gt4l_core::log_export::is_forbidden(&entry.path)));
    let _ = fs::remove_dir_all(&root);
}

/// TASK-181: the Wheel page's command bodies in the page's order: no file at first; a family template for a G27 writes
/// `wheel.toml` (and game.ini names it for a direct start); captured bindings, settings and a preset; the file reads back
/// with the game's rules; the log bundle carries it with the wheel lines; the last session's force feedback is read.
#[test]
fn the_wheel_page_writes_a_dfp_file_the_game_reads() {
    use crate::wheel_page;
    const G27: &str = "030000006d0400009bc2000000000000";
    let root = scratch("wheel-page");
    let dirs = DataDirs::new(root.join("data"));
    let pins = Pins::embedded().expect("embedded pins");
    let shared = launch(&dirs, &pins);
    let wheel_path = lock(&shared).wheel_path();
    let first = wheel_page::get_wheel_config_of(&shared);
    assert_eq!(first["file"]["state"], json!("missing"));
    assert_eq!(first["config"]["enabled"], json!(false));
    assert!(!wheel_path.exists(), "nothing is written before a change");

    let status = json!({ "available": true, "devices": [{ "guid": G27, "name": "Logitech G27 Racing Wheel USB" }] });
    let view = wheel_page::apply_wheel_preset_of("logitech_legacy".into(), false, Some(G27.into()), &shared, status.clone(), false).unwrap();
    assert_eq!(view["config"]["device_guid"], json!(G27));
    assert_eq!(view["config"]["device_name"], json!("Logitech G27 Racing Wheel USB"));
    assert_eq!(view["config"]["preset"], json!("logitech_legacy"));
    assert!(wheel_path.exists());
    let game_ini = fs::read_to_string(lock(&shared).user_dirs.config_dir().join(launch::STANDALONE_CONFIG_FILE)).unwrap();
    assert!(game_ini.contains(&format!("wheel_file={}", wheel_path.display())), "{game_ini}");

    wheel_page::set_wheel_setting_of("wheel.enabled".into(), json!(true), &shared, status.clone(), false).unwrap();
    wheel_page::assign_wheel_binding_of("throttle".into(), "axis:2".into(), Some("inverted".into()), false, &shared, status.clone(), false).unwrap();
    wheel_page::assign_wheel_binding_of("brake".into(), "axis:3".into(), Some("inverted".into()), false, &shared, status.clone(), false).unwrap();
    wheel_page::assign_wheel_binding_of("r2".into(), "btn:4".into(), None, false, &shared, status.clone(), false).unwrap();
    let moved = wheel_page::assign_wheel_binding_of("cross".into(), "btn:4".into(), None, false, &shared, status.clone(), false).unwrap();
    assert_eq!(moved["taken_from"], json!(["r2"]), "one wheel button presses one DFP button");
    wheel_page::set_wheel_setting_of("ffb.strength".into(), json!(0.7), &shared, status.clone(), false).unwrap();
    assert!(wheel_page::set_wheel_setting_of("steering.rotation_degrees".into(), json!(2000), &shared, status.clone(), false).is_err());
    assert!(wheel_page::assign_wheel_binding_of("steering".into(), "btn:1".into(), None, false, &shared, status.clone(), false).is_err());

    let text = fs::read_to_string(&wheel_path).unwrap();
    let back = gt4l_core::wheel::WheelConfig::from_toml(&text).expect("the game reads the file");
    assert!(back.enabled);
    assert_eq!(back.buttons["cross"], vec!["btn:4"]);
    assert_eq!(back.ffb.strength, 0.7);
    assert!(text.contains("mode = \"dfp\"") && !text.contains("pedal_map_mode"), "{text}");

    let presets = wheel_page::save_wheel_preset_of("Owner G27".into(), &shared).unwrap();
    assert_eq!(presets[0]["name"], json!("Owner G27"));
    wheel_page::reset_wheel_config_of(&shared, status.clone(), false).unwrap();
    assert_eq!(gt4l_core::wheel::WheelConfig::from_toml(&fs::read_to_string(&wheel_path).unwrap()).unwrap().enabled, false);
    let restored = wheel_page::apply_wheel_preset_of("Owner G27".into(), true, None, &shared, status.clone(), false).unwrap();
    assert_eq!(restored["config"]["buttons"]["cross"], json!(["btn:4"]));
    assert_eq!(restored["config"]["enabled"], json!(false), "a preset keeps the wheel on or off as it was");

    lock(&shared).wheel.enabled = true;
    lock(&shared).wheel_facts = vec![gt4l_core::log_export::WheelFact {
        name: "Logitech G27 Racing Wheel USB".into(),
        vid_pid: "046D:C29B".into(),
        sdl_type: "wheel".into(),
        role: "wheel".into(),
        haptic: "constant spring damper friction gain autocenter".into(),
    }];
    let entries = crate::log_bundle::collect(&lock(&shared), &[]).unwrap();
    assert!(entries.iter().any(|entry| entry.path == "wheel.toml"));
    let system = entries.iter().find(|entry| entry.path == "system.txt").unwrap();
    assert!(system.text.contains("racing wheel: virtual Driving Force Pro on"), "{}", system.text);
    assert!(system.text.contains("wheel 1: Logitech G27 Racing Wheel USB (046D:C29B, SDL type wheel"), "{}", system.text);

    assert_eq!(wheel_page::get_wheel_session_check_of(&shared)["found"], json!(false));
    let session = dirs.session("session-1");
    fs::create_dir_all(&session).unwrap();
    fs::write(
        session.join("game.log"),
        "[wheel] wheel.toml found (enabled=true, preset=logitech_legacy)\n[wheel] force feedback summary: game force commands 10, effect updates sent 9, effects started 2, haptic device open, effects supported, game forces played 9, reached the wheel yes\n",
    )
    .unwrap();
    let check = wheel_page::get_wheel_session_check_of(&shared);
    assert_eq!((check["found"].clone(), check["received"].clone()), (json!(true), json!("yes")));
    assert_eq!(check["session"], json!("session-1"));
    let _ = fs::remove_dir_all(root);
}

/// TASK-181: a `mode = "pad"` wheel.toml (the removed 1.2.x bridge, which the game refuses) is migrated when the launcher
/// starts: written again as a Driving Force Pro file with its bindings.
#[test]
fn a_pad_mode_wheel_file_is_migrated_at_start() {
    let root = scratch("wheel-migrate");
    let dirs = DataDirs::new(root.join("data"));
    let pins = Pins::embedded().expect("embedded pins");
    let user = UserDirs::new(dirs.root().join("appdata-stand-in").join(USER_FOLDER_NAME), UserRootSource::Override);
    let wheel_path = user.wheel_file();
    fs::create_dir_all(wheel_path.parent().unwrap()).unwrap();
    fs::write(
        &wheel_path,
        "[wheel]\nversion = 1\nenabled = true\nmode = \"pad\"\npedal_map_mode = \"both\"\n[steering]\neffective_degrees = 540\n[buttons]\nshift_up = [\"btn:12\"]\n",
    )
    .unwrap();
    let shared = launch(&dirs, &pins);
    let text = fs::read_to_string(&wheel_path).unwrap();
    assert!(text.contains("mode = \"dfp\"") && !text.contains("\"pad\"") && !text.contains("effective_degrees"), "{text}");
    let config = gt4l_core::wheel::WheelConfig::from_toml(&text).expect("the game reads the migrated file");
    assert!(config.enabled);
    assert_eq!(config.buttons["r2"], vec!["btn:12"]);
    assert_eq!(crate::wheel_page::get_wheel_config_of(&shared)["file"]["state"], json!("migrated"));
    let _ = fs::remove_dir_all(root);
}

/// TASK-181: a wheel.toml the game refuses is kept with one copy beside it (not one per launcher start); the page
/// says why, and the first change replaces it with a valid file.
#[test]
fn a_refused_wheel_file_gets_one_copy_and_the_first_change_replaces_it() {
    let root = scratch("wheel-refused");
    let dirs = DataDirs::new(root.join("data"));
    let pins = Pins::embedded().expect("embedded pins");
    let user = UserDirs::new(dirs.root().join("appdata-stand-in").join(USER_FOLDER_NAME), UserRootSource::Override);
    let wheel_path = user.wheel_file();
    fs::create_dir_all(wheel_path.parent().unwrap()).unwrap();
    fs::write(&wheel_path, "[wheel]\nversion = 1\nenabled = true\n[steering]\nrotation_degrees = 90\n").unwrap();
    let copies = |folder: &Path| {
        fs::read_dir(folder).unwrap().filter_map(|entry| entry.ok()).filter(|entry| entry.file_name().to_string_lossy().starts_with("wheel.toml.bak-invalid-")).count()
    };
    let shared = launch(&dirs, &pins);
    let view = crate::wheel_page::get_wheel_config_of(&shared);
    assert_eq!(view["file"]["state"], json!("refused"));
    assert!(view["file"]["detail"].as_str().unwrap().contains("rotation_degrees"), "{}", view["file"]["detail"]);
    assert_eq!(view["config"]["enabled"], json!(false), "the defaults (the wheel off) until a change");
    drop(shared);
    let shared = launch(&dirs, &pins);
    assert_eq!(copies(wheel_path.parent().unwrap()), 1, "one copy, however many starts");
    crate::wheel_page::set_wheel_setting_of("wheel.enabled".into(), json!(true), &shared, json!({ "devices": [] }), false).unwrap();
    let config = gt4l_core::wheel::WheelConfig::from_toml(&fs::read_to_string(&wheel_path).unwrap()).expect("the game reads the new file");
    assert!(config.enabled);
    assert_eq!(crate::wheel_page::get_wheel_config_of(&shared)["file"]["state"], json!("loaded"));
    let _ = fs::remove_dir_all(root);
}

/// Round 9 (owner, 2026-11-06): Android's Play now stores the game it started, so the next launch opens on it; a bench run
/// (GT4_BENCH_GAME) must leave the page and the stored choice alone.
#[test]
fn the_home_remembers_the_game_played_and_a_bench_does_not_move_it() {
    use gt4l_core::games::{GT4, SPEC2};
    let root = scratch("played-game");
    let dirs = DataDirs::new(&root.join("data"));
    let pins = Pins::embedded().expect("embedded pins");
    let shared = launch(&dirs, &pins);
    commands::remember_game_played(&mut lock(&shared), &GT4);
    let second = launch(&dirs, &pins);
    assert_eq!(screen(&second)["selected_game"], "gt4", "v2.00, the game just played");
    assert_eq!(lock(&second).config.selected_game.as_deref(), Some("gt4"));
    commands::remember_game_played(&mut lock(&second), &SPEC2);
    let third = launch(&dirs, &pins);
    assert_eq!(screen(&third)["selected_game"], "spec2", "Spec II, the game just played");
    assert_eq!(lock(&third).config.selected_game.as_deref(), Some("spec2"));
    // A bench run never calls remember_played: the stored choice and the page stay on Spec II.
    assert_eq!(screen(&third)["selected_game"], "spec2");
}

//! Launcher state shared by the commands: settings, controls, the save library and the running game.
//! The state lives behind one mutex. Long jobs (setup, the game, controller capture) run on their
//! own threads and lock the state only to read or write their results.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::Child;
use std::sync::atomic::AtomicBool;
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{SystemTime, UNIX_EPOCH};

use gt4controls::{self as controls_file, Controls, CONTROLS_FILE_NAME};
use std::collections::BTreeMap;

use gt4l_core::settings::Settings;
use gt4l_core::config::{self, ConfigFile};
use gt4l_core::language::{self, Language};
use gt4l_core::display as display_rules;
use gt4l_core::gs_release;
use gt4l_core::gs_table;
use gt4l_core::launch;
use gt4l_core::games::{self, Game, GT4};
use gt4l_core::paths::{self, DataDirs, UserDirs};
use gt4l_core::textures::{self, TextureLocation};
use gt4l_core::log_export::WheelFact;
use gt4l_core::wheel::{self, LoadState, WheelConfig};
use gt4l_disc::setup as disc_setup;
use gt4l_disc::spec2 as disc_spec2;
use gt4l_disc::{DiscGame, Pins, Spec2Pins};
use gt4saves::library::Library;

use crate::discord_presence::DiscordPresence;
use crate::saves;

/// One game's save library and the user folders it was opened in (the Documents-free `games\<id>` place, or the data-folder
/// fallback). Two games never share one (TASK-101).
pub struct GameLibrary {
    pub library: Library,
    pub user: UserDirs,
}

pub struct AppState {
    pub dirs: DataDirs,
    pub pins: Pins,
    pub spec2_pins: Spec2Pins,
    pub config: ConfigFile,
    pub controls: Controls,
    /// The user data in use: `%APPDATA%\GT4Recomp`, or the 1.2.0 places until their move has happened.
    pub user_dirs: UserDirs,
    /// One save library per game (`games\<id>\Saves`, `Replays`, `backups`).
    pub libraries: BTreeMap<&'static str, GameLibrary>,
    /// The game the home screen shows. The stored choice (`[game] selected`) is the game played last; picking another game
    /// on the page only changes this until that game starts.
    pub selected: &'static Game,
    /// The game of the setup that is running or was last run (set when a disc is accepted).
    pub setup_game: Option<&'static Game>,
    /// The folder of `GT4Recomp.exe`: stored paths inside it are kept relative (portable copies).
    pub install_dir: PathBuf,
    /// The move of the 1.2.0 large data, while it runs and its result (TASK-091).
    pub migration: MigrationRun,
    /// Set by the Cancel button of setup; the setup thread checks it between steps.
    pub setup_cancel: Arc<AtomicBool>,
    pub setup_busy: bool,
    /// Progress of the running setup (the page polls it through `app_state`).
    pub setup_run: SetupRun,
    /// How the last pick or setup run ended, until the next one starts.
    pub setup_outcome: SetupOutcome,
    /// The running game's child process (shared with the watcher thread and Stop).
    pub game_child: Option<Arc<Mutex<Option<Child>>>>,
    /// IN-507: the shader compile before Play, and whether its window's Cancel stopped it.
    pub shaders_child: Arc<Mutex<Option<Child>>>,
    pub shaders_cancelled: Arc<AtomicBool>,
    /// Control name while a rebind waits for a press from the pad service (`pad_service`).
    pub capturing: Option<String>,
    /// Message shown on Home, e.g. why the last start failed.
    pub home_message: String,
    /// Status line of the Controls page.
    pub controls_status: String,
    /// Result of the last import, activate or delete on the Saves page.
    pub saves_status: String,
    /// Result of the last import or delete on the Replays page.
    pub replays_status: String,
    /// True when the controls file was written by a newer launcher: it is kept as it is (never overwritten).
    pub controls_locked: bool,
    pub ra_revision: u64,
    /// TASK-181: the racing wheel configuration (`wheel.toml`, the game's virtual Driving Force Pro).
    pub wheel: WheelConfig,
    /// What loading `wheel.toml` found at start (missing, read, migrated, refused, newer).
    pub wheel_load: LoadState,
    /// True when `wheel.toml` was written by a newer launcher: it is kept as it is (never overwritten).
    pub wheel_locked: bool,
    /// The connected wheels for `system.txt`, kept up to date by the pad service.
    pub wheel_facts: Vec<WheelFact>,
    /// Discord Rich Presence manager for the current launcher session.
    pub discord: Arc<DiscordPresence>,
}

pub type Shared = Arc<Mutex<AppState>>;

/// The background move of the 1.2.0 large data. The page polls it through `app_state` and shows a notice.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct MigrationRun {
    pub running: bool,
    /// The unit being moved (`game files`, `gt4-gs`, `textures`, ...).
    pub item: String,
    pub bytes_done: u64,
    pub bytes_total: u64,
    /// The result for the player, shown once (empty when there is nothing to say).
    pub notice: String,
    /// Whether the notice reports a problem.
    pub problem: bool,
}

/// The step setup is on and how far it is. `step` is `copying`, `checking`, `preparing` or `renderer`.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SetupRun {
    pub step: String,
    /// 0..1 of the current step.
    pub fraction: f32,
    /// Bytes done and total of a copy or a download (0 when not known).
    pub bytes_done: u64,
    pub bytes_total: u64,
    /// The renderer sub-step: `checking`, `compiling` (bytes hold the shaders done and total), `listing`, `downloading`,
    /// `installing`, `done` or `failed`.
    pub renderer_step: String,
    /// What setup decided: `coregs20`, `gsdx_fallback` (this PC cannot run CoreGS 2.0), or empty (GSdx stayed the renderer).
    pub renderer_choice: String,
}

/// The result of the last disc pick or setup run.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub enum SetupOutcome {
    #[default]
    None,
    /// The chosen file is not a supported disc (the page says "Not supported").
    Refused(String),
    /// The run stopped (cancel, no space, write error).
    Failed(String),
    /// The run finished. `renderer_note` is set when the PCSX2 GS could not be installed.
    Done { renderer_note: String },
}

/// The language the launcher shows now: set at start-up and whenever the setting changes (`set_current_language`), so
/// the messages built deep in the launcher do not need the state.
static CURRENT_LANGUAGE: Mutex<Language> = Mutex::new(Language::fallback());

/// The language of the messages the launcher sends now.
pub fn current_language() -> Language {
    // The tests of this crate check the English texts; the language of the machine running them must not change that.
    if cfg!(test) {
        return Language::fallback();
    }
    *CURRENT_LANGUAGE.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}

/// Makes `language` the language of the messages from now on.
pub fn set_current_language(language: Language) {
    *CURRENT_LANGUAGE.lock().unwrap_or_else(|poisoned| poisoned.into_inner()) = language;
}

/// The language of the messages the launcher sends now (see `current_language`), for a settings table.
pub fn language_of(state: &AppState) -> Language {
    language_of_settings(&state.config.settings)
}

/// The language the settings choose, with `auto` resolved by the Windows user language.
pub fn language_of_settings(settings: &gt4l_core::Settings) -> Language {
    if cfg!(test) {
        // See `current_language`: the tests of this crate check the English texts.
        return Language::fallback();
    }
    language::resolve(settings.get("language"), &gt4l_platform::system::os_locale())
}

/// Locks the state; a poisoned lock still gives its data (a panicked job must not hang the UI).
pub fn lock(shared: &Shared) -> MutexGuard<'_, AppState> {
    match shared.lock() {
        Ok(guard) => guard,
        Err(poisoned) => poisoned.into_inner(),
    }
}

/// Writes a small text file through a temporary file and a rename.
fn write_text_atomic(path: &Path, text: &str) -> std::io::Result<()> {
    let temporary = path.with_extension("ini.tmp");
    fs::write(&temporary, text)?;
    fs::rename(&temporary, path)
}

/// Appends an INFO line to the launcher's session log (see `session_log`).
pub fn log(dirs: &DataDirs, message: &str) {
    crate::session_log::write(dirs, gt4l_core::logs::Level::Info, message);
}

/// TASK-180 test3 (owner's phone): the Render scale written before schema 9 is a notch of the screen's native multiplier,
/// not the percentage of the screen's height the row shows now. Converted once, with the screen and GPU of this launch:
/// Windows keeps the multiplier the player had, Android goes to the 2x default (the phone's values were never chosen as
/// percentages). A screen that cannot be read leaves the file alone; the next start converts it. Returns what it did.
pub fn migrate_render_scale(config: &mut ConfigFile, dirs: &DataDirs) -> bool {
    match monitors_for_scale().first().map(|monitor| monitor.desktop) {
        Some(screen) => migrate_render_scale_at(screen, config, dirs),
        None => {
            if config.render_scale_model < 2 {
                log(dirs, "settings: the Render scale is from the old scale model; the screen is not known yet, Play converts it");
            }
            false
        }
    }
}

/// [`migrate_render_scale`] for a known screen size (Android's Play converts once the screen has been read).
pub fn migrate_render_scale_at(screen: display_rules::Size, config: &mut ConfigFile, dirs: &DataDirs) -> bool {
    if config.render_scale_model >= 2 {
        return false;
    }
    let stored = config.settings.get("render_scale").parse::<u32>().unwrap_or(display_rules::RENDER_SCALE_DEFAULT);
    // The old model climbed from the screen's own multiplier (k) on a discrete card, which is what a PC's plan used for
    // the stored notches; an integrated GPU's untouched 100% was 1x, and that value is converted like any other notch.
    let integrated = false;
    let (percent, line) = display_rules::migrate_render_scale(
        stored,
        screen,
        integrated,
        display_rules::RendererCaps { max_multiplier: crate::model::MAX_UPSCALE_DEFAULT },
        cfg!(target_os = "android"),
    );
    let _ = config.settings.set("render_scale", &percent.to_string());
    config.render_scale_model = 2;
    log(dirs, &line);
    true
}

/// The monitor the Render scale is measured against: the stored one when it can be read, else the primary.
fn monitors_for_scale() -> Vec<display_rules::Monitor> {
    #[cfg(target_os = "android")]
    {
        crate::android::monitors()
    }
    #[cfg(not(target_os = "android"))]
    {
        gt4l_platform::display::monitors()
    }
}

/// Appends a WARN line: a soft problem the player may notice (TASK-112).
pub fn log_warn(dirs: &DataDirs, message: &str) {
    crate::session_log::write(dirs, gt4l_core::logs::Level::Warn, message);
}

/// Appends an ERROR line: a real failure (a crash, a lost GPU, a failed setup, import, harvest or update) (TASK-112).
pub fn log_error(dirs: &DataDirs, message: &str) {
    crate::session_log::write(dirs, gt4l_core::logs::Level::Error, message);
}

impl AppState {
    /// Loads the settings and controls from the user data folder and opens the save library there.
    /// `pre_1_0_root` is the 1.2.0 `%LOCALAPPDATA%\GT4Recomp`, which may still hold a library of launchers before 1.0.0.
    pub fn load(dirs: DataDirs, user: UserDirs, pre_1_0_root: Option<PathBuf>) -> Result<AppState, String> {
        let pins = Pins::embedded()?;
        AppState::load_with(dirs, pins, user, pre_1_0_root, crate::updates::install_directory())
    }

    /// [`AppState::load`] with given pins, user folder and install folder (the flow test uses synthetic pins for a
    /// synthetic disc and scratch folders).
    pub fn load_with(dirs: DataDirs, pins: Pins, user: UserDirs, pre_1_0_root: Option<PathBuf>, install_dir: PathBuf) -> Result<AppState, String> {
        std::fs::create_dir_all(dirs.root()).map_err(|error| format!("data folder: {error}"))?;
        let (mut config, warnings) = config::load(&user.config_file());
        for warning in &warnings {
            log(&dirs, &format!("settings: {warning}"));
        }
        set_current_language(language_of_settings(&config.settings));
        // TASK-109: a release candidate build with no chosen update channel stays on Beta; any other build on Stable.
        gt4l_core::update::apply_channel_default(&mut config.settings, gt4l_core::launcher_release_version());
        // TASK-180 test3 (owner's phone): a Render scale stored under the old notch model is converted with the screen in
        // hand, then the file records the new meaning (schema 9). Windows keeps the picture the player had; Android goes
        // to the 2x default (test1's "125" became 6x on the S23 and the menus ran at 12 fps).
        migrate_render_scale(&mut config, &dirs);
        // Stored paths inside the install folder are relative (see `save_config`); resolve them here.
        config.disc_path = config.disc_path.map(|path| paths::resolve_portable(&path, &install_dir));
        config.dev_game_dir = config.dev_game_dir.map(|path| paths::resolve_portable(&path, &install_dir));
        let controls_path = user.controls_file();
        let mut controls_locked = false;
        let controls = match controls_file::load(&controls_path) {
            Ok(Some(controls)) => controls,
            Ok(None) => Controls::default(),
            Err(message) => {
                // A file from a newer launcher is kept as it is. Any other damaged file gets a copy before it is
                // replaced by the defaults (docs/launcher/MIGRATIONS.md).
                let text = fs::read_to_string(&controls_path).unwrap_or_default();
                if controls_schema_is_newer(&text) {
                    controls_locked = true;
                    log(&dirs, &format!("controls: {message}; the file is from a newer launcher and is kept"));
                } else if controls_path.exists() {
                    let copy = controls_path.with_file_name(format!("{CONTROLS_FILE_NAME}.bak-corrupt-{}", unix_seconds()));
                    let _ = fs::copy(&controls_path, &copy);
                    log(&dirs, &format!("controls: {message}; copy kept at {}", copy.display()));
                } else {
                    log(&dirs, &format!("controls: {message}; built-in bindings used"));
                }
                Controls::default()
            }
        };
        let (wheel_config, wheel_load) = load_wheel(&dirs, &user.wheel_file());
        let mut libraries = BTreeMap::new();
        let mut library_notice = None;
        for game in games::GAMES {
            // Only Gran Turismo 4 has a library from before 1.0.0 to take over.
            let old_root = if game.id == GT4.id { pre_1_0_root.as_deref() } else { None };
            let opened = saves::open(&dirs.for_game(game), &user.for_game(game), game, old_root)?;
            for line in &opened.log {
                log(&dirs, &format!("{}: {line}", game.id));
            }
            if game.id == GT4.id {
                library_notice = opened.notice.clone();
            }
            libraries.insert(game.id, GameLibrary { library: opened.library, user: opened.user });
        }
        let spec2_pins = Spec2Pins::embedded()?;
        let mut state = AppState {
            dirs,
            pins,
            spec2_pins,
            config,
            controls,
            user_dirs: user,
            libraries,
            selected: &GT4,
            setup_game: None,
            install_dir,
            migration: MigrationRun::default(),
            setup_cancel: Arc::new(AtomicBool::new(false)),
            setup_busy: false,
            setup_run: SetupRun::default(),
            setup_outcome: SetupOutcome::None,
            game_child: None,
            shaders_child: Arc::new(Mutex::new(None)),
            shaders_cancelled: Arc::new(AtomicBool::new(false)),
            capturing: None,
            home_message: String::new(),
            controls_status: String::new(),
            saves_status: library_notice.unwrap_or_default(),
            replays_status: String::new(),
            controls_locked,
            ra_revision: 0,
            wheel_locked: matches!(wheel_load, LoadState::Newer(_)),
            wheel: wheel_config,
            wheel_load,
            wheel_facts: Vec::new(),
            discord: Arc::new(DiscordPresence::new_default()),
        };
        // TASK-118: the RC settings must not survive into a final build: an RC install can leave the release report or
        // the VU check on, and the final build does not ship their rows. Force the factory values and save.
        // TASK-180 test3 fix (owner, 2026-11-05): the test builds are the release candidates AND the Android test builds
        // (`1.3.0-android-testN`); keying only off "-rc" turned the report off on the owner's phone.
        let test_build = gt4l_core::test_build();
        if !test_build {
            let mut changed = false;
            for (id, factory) in [("performance_report", "off"), ("check_vu_code", "off")] {
                if state.config.settings.get(id) != factory {
                    let _ = state.config.settings.set(id, factory);
                    changed = true;
                }
            }
            if changed {
                state.save_config();
                log(&state.dirs, "final build: the release-candidate settings went back to their factory values");
            }
        }
        // TASK-180 test4 (owner, 2026-11-05): an Android test build comes with the performance report ON, so the F7/F8
        // touch button is in the game from the first launch and the owner's reports keep coming. A value the player chose
        // on the Settings page is kept; a final build never reaches here (it forces the report off just above).
        let platform = gt4l_core::Platform::current();
        if test_build
            && platform == gt4l_core::Platform::Android
            && !state.config.settings.was_chosen("performance_report")
            && state.config.settings.get("performance_report") != "on"
        {
            let _ = state.config.settings.set("performance_report", "on");
            state.save_config();
            log(
                &state.dirs,
                "android test build: the performance report is on by default (F7 starts an interval, F8 writes it)",
            );
        }
        // TASK-180: a value the platform fixes (CoreGS 2.0 on Android) replaces whatever is stored, and is saved.
        let mut forced = false;
        for definition in gt4l_core::SETTINGS {
            let stored = state.config.settings.get(definition.id).to_string();
            if let Some(value) = platform.forced_value_for(definition.id, &stored) {
                if stored != value {
                    let _ = state.config.settings.set(definition.id, value);
                    forced = true;
                }
            }
        }
        if forced {
            state.save_config();
            log(&state.dirs, &format!("{}: the settings this platform fixes were set", platform.name()));
        }
        let installed = state.installed_games();
        state.selected = games::initial_selection(state.config.selected_game.as_deref(), &installed);
        Ok(state)
    }

    /// The data folders of one game (the shared parts are the same for every game).
    pub fn game_dirs(&self, game: &'static Game) -> DataDirs {
        self.dirs.for_game(game)
    }

    /// The save library of a game.
    pub fn library_of(&self, game: &'static Game) -> &Library {
        &self.libraries[game.id].library
    }

    /// The user folders of a game's library (`Saves`, `Replays`, `backups`).
    pub fn user_for(&self, game: &'static Game) -> &UserDirs {
        &self.libraries[game.id].user
    }

    /// The library of the game the page shows.
    pub fn library(&self) -> &Library {
        self.library_of(self.selected)
    }

    /// The user folders of the game the page shows.
    pub fn selected_user(&self) -> &UserDirs {
        self.user_for(self.selected)
    }

    /// The cheap check of one game's copied disc image and prepared files (no hash; see `gt4l_disc::setup`).
    pub fn installed_of(&self, game: &'static Game) -> disc_setup::Installed {
        let dirs = self.game_dirs(game);
        match DiscGame::from_id(game.id) {
            Some(DiscGame::Spec2) => disc_spec2::check_installed(&dirs.prepared(), &dirs.disc_folder(), &self.install_dir, &self.spec2_pins),
            _ => disc_setup::check_installed(&dirs.prepared(), &dirs.disc_folder(), &self.install_dir, &self.pins),
        }
    }

    /// The check for the game the page shows.
    pub fn installed(&self) -> disc_setup::Installed {
        self.installed_of(self.selected)
    }

    /// The games whose files pass the cheap check, in table order.
    pub fn installed_games(&self) -> Vec<&'static Game> {
        games::GAMES
            .iter()
            .copied()
            .filter(|game| matches!(self.installed_of(game), disc_setup::Installed::Ready { .. }))
            .collect()
    }

    /// Home is ready when setup is not running and at least one game passes the cheap check.
    pub fn home_ready(&self) -> bool {
        !self.setup_busy && !self.installed_games().is_empty()
    }

    /// Writes the settings to the user data folder. A path inside the install folder is stored relative to it, so a
    /// copied install folder (another drive or PC) still finds it; no absolute data path is stored.
    pub fn save_config(&self) {
        let mut stored = self.config.clone();
        stored.disc_path = stored.disc_path.map(|path| paths::portable_path(&path, &self.install_dir));
        stored.dev_game_dir = stored.dev_game_dir.map(|path| paths::portable_path(&path, &self.install_dir));
        if let Err(error) = config::save(&self.user_dirs.config_file(), &stored) {
            log_error(&self.dirs, &format!("settings not saved: {error}"));
        }
        // TASK-097: a direct start of the game reads the same choices.
        self.write_standalone_files(&self.config.settings);
    }

    /// Writes what a direct start of the game reads (TASK-097) into the user config folder: `game.ini` (fullscreen, present
    /// mode, monitor, output size and the diagnostic switches) and `gt4-gs.ini` (the session ini a Play writes, with the
    /// same display plan). The game finds the folder as the launcher does (`<install>\user\config` for the portable
    /// build, `%APPDATA%\GT4Recomp\config` otherwise). `settings` are the values of the run (TASK-108: a run that
    /// falls back to Direct3D 11 writes its own). Failures are logged; nothing here can stop the launcher.
    pub fn write_standalone_files(&self, stored: &Settings) {
        // TASK-108: a save writes the stored choices, so the API, card and Adapter are resolved here as a Play does.
        let (resolved, _, _) = gt4l_platform::gpu::run_settings(stored, gt4l_core::language::Language::fallback());
        let settings = &resolved;
        let monitors = gt4l_platform::display::monitors();
        let display = display_rules::resolve_target(&monitors, settings.get("monitor"), settings.get("resolution"));
        let config_dir = self.user_dirs.config_dir().to_path_buf();
        let game_ini = config_dir.join(launch::STANDALONE_CONFIG_FILE);
        let mut text = launch::standalone_config_text(settings, display.as_ref());
        if settings.get("renderer") == gt4l_core::settings::CORE_GS_20 {
            let integrated = gt4l_platform::gpu::integrated_gpu_name(settings.get("graphics_card")).is_some();
            text.push_str(&launch::coregs20_section(settings, display.as_ref(), integrated, &self.texture_location().0));
        }
        // TASK-181: a direct start finds the racing wheel file through game.ini (only when the file exists).
        let wheel_path = self.wheel_path();
        if wheel_path.exists() {
            text.push_str(&launch::wheel_file_line(&wheel_path));
        }
        if let Err(error) = fs::create_dir_all(&config_dir).and_then(|()| write_text_atomic(&game_ini, &text)) {
            log_error(&self.dirs, &format!("{} not written: {error}", game_ini.display()));
        }
        // gt4-gs.ini needs the installed GSdx only for its folders; without it the previous file stays.
        let renderer_root = self.dirs.renderer_root();
        if let Some(installed) = gs_release::installed(&renderer_root) {
            let paths = launch::Pcsx2Paths::for_session(&installed, &self.dirs.renderer_install_root(), &config_dir, &self.texture_location().0.folder);
            let integrated = gt4l_platform::gpu::integrated_gpu_name(settings.get("graphics_card")).is_some();
            match launch::gs_config_text(settings, &paths, display.as_ref(), integrated) {
                Ok(text) => {
                    if let Err(error) = gs_table::write_ini_atomic(&paths.config, &text) {
                        log_error(&self.dirs, &format!("{} not written: {error}", paths.config.display()));
                    }
                }
                Err(message) => log(&self.dirs, &format!("gt4-gs.ini for a direct start not rendered: {message}")),
            }
        }
    }

    /// The controls file. Commands write it through `commands::change_controls`, which refuses while
    /// `controls_locked` is set and keeps the change only when the write succeeded.
    pub fn controls_path(&self) -> PathBuf {
        self.user_dirs.controls_file()
    }

    /// TASK-181: the racing wheel file, beside the controls file (the game looks there without `GT4_WHEEL_FILE`).
    pub fn wheel_path(&self) -> PathBuf {
        self.user_dirs.wheel_file()
    }

    /// TASK-181: the folder of the named wheel presets.
    pub fn wheel_presets_dir(&self) -> PathBuf {
        self.user_dirs.wheel_presets()
    }

    pub fn disc_path(&self) -> Option<&Path> {
        self.config.disc_path.as_deref()
    }

    /// Where the renderers read custom textures (TASK-157): the folder the player chose when it is usable, else the
    /// default `Textures` folder. The second value is the log line for a choice that cannot be used (it stays stored).
    pub fn texture_location(&self) -> (TextureLocation, Option<String>) {
        textures::resolve_location(&self.dirs.textures(), self.config.textures_path.as_deref(), self.dirs.root())
    }
}

/// Whether a controls file declares a schema above the one this launcher reads (`schema = 1`).
fn controls_schema_is_newer(text: &str) -> bool {
    text.parse::<toml::Table>()
        .ok()
        .and_then(|table| table.get("schema").and_then(|value| value.as_integer()))
        .is_some_and(|schema| schema > 1)
}

/// TASK-181: reads `wheel.toml` at start. A 1.2.x or `mode = "pad"` file is migrated and written again at once as a
/// Driving Force Pro file (the game refuses `pad`); a file the game refuses gets a copy beside it and the defaults are
/// used (the wheel off) until the player changes something; a newer file is kept as it is.
fn load_wheel(dirs: &DataDirs, path: &Path) -> (WheelConfig, LoadState) {
    let loaded = wheel::load(path);
    match &loaded.state {
        LoadState::Missing => {}
        LoadState::Loaded => log(dirs, &format!("wheel: {} read (enabled {}, preset {})", path.display(), loaded.config.enabled, loaded.config.preset)),
        LoadState::Migrated(why) => match loaded.config.save_atomic(path) {
            Ok(()) => log(dirs, &format!("wheel: {} migrated to the Driving Force Pro file: {why}", path.display())),
            Err(error) => log_error(dirs, &format!("wheel: {} not migrated ({why}): {error}", path.display())),
        },
        LoadState::Refused(error) => {
            // One copy per content: a launcher started again before any change does not pile up copies.
            let text = fs::read(path).unwrap_or_default();
            let prefix = format!("{}.bak-invalid-", wheel::FILE_NAME);
            let kept = path.parent().and_then(|folder| fs::read_dir(folder).ok()).into_iter().flatten().filter_map(|entry| entry.ok()).find(|entry| {
                entry.file_name().to_string_lossy().starts_with(&prefix) && fs::read(entry.path()).is_ok_and(|other| other == text)
            });
            let copy = match kept {
                Some(entry) => entry.path(),
                None => {
                    let copy = path.with_file_name(format!("{prefix}{}", unix_seconds()));
                    let _ = fs::copy(path, &copy);
                    copy
                }
            };
            log_warn(dirs, &format!("wheel: {} is refused by the game ({error}); copy kept at {}; the wheel is off", path.display(), copy.display()));
        }
        LoadState::Newer(error) => log(dirs, &format!("wheel: {} is from a newer launcher and is kept ({error})", path.display())),
    }
    (loaded.config, loaded.state)
}

fn unix_seconds() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|value| value.as_secs()).unwrap_or(0)
}

//! Builds the exact command line and environment for `gt4.exe`.
//!
//! Contract (from `scripts/external_runtime_probe.py::run_probe` and `gs_stream_environment`):
//! - arguments: `<elf> --ntsc --disc <iso> --frames 0 --memcard <card> --misses <session>\misses.txt --window`
//! - the working directory is the session folder;
//! - inherited `GT4_*` and `PS2_*` variables are removed, then only the variables below are set.
//!
//! The renderer is the PCSX2 GS (`GT4_RENDERER=pcsx2`, the downloaded `gt4-gs.exe` in its own process, see
//! [`pcsx2_environment`], decision 0070) or, when the player chose it, CoreGS 2.0 (`GT4_RENDERER=hw`, inside the game
//! executable, see [`coregs20_environment`], TASK-027). CoreGS 1.1 is still inside the game executable, but the launcher
//! never starts it (TASK-096, owner 2026-10-22). Only the child process gets these variables. Nothing is written to the
//! user or system environment.
use crate::language::Language;
use crate::messages;
use crate::platform::Platform;


use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use crate::display::{self, DisplayTarget};
use crate::games::Game;
use crate::gs_release::Installed;
use crate::gs_table;
use crate::settings::{self, Settings};
use crate::textures::TextureLocation;

/// Inherited variables with these prefixes are removed before the game starts.
pub const SCRUBBED_PREFIXES: &[&str] = &["GT4_", "PS2_"];

/// Printed by the game when the player asked for "restart with the built-in renderer" after a GS process failure.
/// The launcher no longer starts CoreGS 1.1 (TASK-096): it ends the session and shows the GSdx error instead.
pub const GS_RESTART_MARKER: &str = "[gs-ipc] renderer change requested: CoreGS 2.0";

/// The reason shown when the PCSX2 GS (GSdx) is not installed or its files fail verification.
pub const GSDX_NOT_INSTALLED: &str = "GSdx is not installed yet";

/// The reason shown when GSdx stopped and asked for a restart with CoreGS 1.1.
pub const GSDX_RESTART_REQUESTED: &str = "GSdx stopped while playing";

/// The text of a GSdx download or install asked for where GSdx does not exist (TASK-180, owner 2026-11-06): `gt4-gs.exe` is a
/// Windows program, so Android never downloads it. The commands refuse instead of starting a transfer.
pub const GSDX_WINDOWS_ONLY: &str = "GSdx is not available on Android.";

/// The executable of a game inside the game folder (`<program folder>\game`, TASK-101: one program per game).
pub fn game_program(game: &Game, game_dir: &Path) -> PathBuf {
    game_dir.join(game.executable)
}

/// The prepared ELF of a game. Its file name stays `SCUS_973.28` / `SCUS_974.36` (the argv0 rule: the donor derives the
/// boot name from it).
pub fn game_elf(game: &Game, prepared: &Path) -> PathBuf {
    prepared.join(game.elf_name)
}

/// Everything needed to start one game session.
#[derive(Clone, Debug)]
pub struct LaunchInputs<'a> {
    /// `gt4.exe`.
    pub game_executable: &'a Path,
    /// The rebuilt ELF in the prepared folder. Its file name must stay `SCUS_973.28` (argv0 rule).
    pub elf: &'a Path,
    pub disc: &'a Path,
    pub card: &'a Path,
    /// A new session folder; it becomes the working directory and receives `misses.txt` and `game.log`.
    pub session_dir: &'a Path,
}

/// The planned launch. `removed_inherited` lists variables to remove from the child environment.
#[derive(Clone, PartialEq, Eq)]
pub struct LaunchPlan {
    pub program: PathBuf,
    pub arguments: Vec<String>,
    pub environment: Vec<(String, String)>,
    pub removed_inherited: Vec<String>,
    pub working_directory: PathBuf,
    pub log_path: PathBuf,
    pub misses_path: PathBuf,
}

impl LaunchPlan {
    /// Safe environment view with sensitive values (_TOKEN, _PASSWORD) masked as `[REDACTED]`.
    pub fn safe_environment(&self) -> Vec<(String, String)> {
        self.environment
            .iter()
            .map(|(k, v)| {
                let upper = k.to_ascii_uppercase();
                if upper.ends_with("_TOKEN")
                    || upper.ends_with("_PASSWORD")
                    || upper == "GT4_RA_TOKEN"
                    || upper == "GT4_RA_PASSWORD"
                {
                    (k.clone(), "[REDACTED]".to_string())
                } else {
                    (k.clone(), v.clone())
                }
            })
            .collect()
    }
}

impl std::fmt::Debug for LaunchPlan {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let safe_env = self.safe_environment();
        f.debug_struct("LaunchPlan")
            .field("program", &self.program)
            .field("arguments", &self.arguments)
            .field("environment", &safe_env)
            .field("removed_inherited", &self.removed_inherited)
            .field("working_directory", &self.working_directory)
            .field("log_path", &self.log_path)
            .field("misses_path", &self.misses_path)
            .finish()
    }
}

/// The files and pins of the PCSX2 GS process for one session.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Pcsx2Paths {
    /// `gt4-gs.exe` in the installed version folder.
    pub executable: PathBuf,
    /// SHA-256 of that file (`GT4_GS_PROCESS_SHA256`, checked by the game).
    pub executable_sha256: String,
    /// `gt4-gs.ini` written for this session.
    pub config: PathBuf,
    /// Shader and pipeline cache folder (`%LOCALAPPDATA%\GT4Recomp\gs\cache`).
    pub cache: PathBuf,
    /// Data folder of the GS process (`%LOCALAPPDATA%\GT4Recomp\gs\data`).
    pub data_root: PathBuf,
    /// The custom texture folder, `Documents\Gran Turismo 4\Textures` (TASK-088, gt4-gs `[Folders] Textures`).
    pub textures: PathBuf,
}

impl Pcsx2Paths {
    /// The paths of an installed renderer, with the cache and data folders under the renderer root.
    pub fn for_session(installed: &Installed, renderer_root: &Path, session_dir: &Path, textures: &Path) -> Pcsx2Paths {
        Pcsx2Paths {
            executable: installed.executable.clone(),
            executable_sha256: installed.executable_sha256.clone(),
            config: session_dir.join("gt4-gs.ini"),
            cache: renderer_root.join("cache"),
            data_root: renderer_root.join("data"),
            textures: textures.to_path_buf(),
        }
    }
}

/// Chooses the renderer of a session. Only GSdx runs the game (owner, 2026-10-22): when it is not installed, or its
/// files failed verification, the session does not start and the error says why and where the log is. There is no
/// CoreGS 1.1 fallback.
pub fn choose_renderer<'a>(installed: Option<&'a Installed>, log_folder: &Path) -> Result<&'a Installed, String> {
    installed.ok_or_else(|| gsdx_unavailable_message(GSDX_NOT_INSTALLED, log_folder))
}

/// Whether Play offers CoreGS 2.0 instead of stopping with "GSdx is not installed yet" (issues #139 and #179): GSdx is the
/// stored renderer, it is not installed (its download failed, for example with GitHub unreachable), and the card this PC
/// would use has Vulkan 1.2, which is all CoreGS 2.0 needs (it is inside the game). On a PC without such a card the plain
/// GSdx message stays, since CoreGS 2.0 could not start either. Nothing is offered when CoreGS 2.0 is already the renderer.
pub fn offers_coregs20(settings: &Settings, gsdx_installed: bool, card_has_vulkan: bool) -> bool {
    !uses_coregs20(settings) && !gsdx_installed && card_has_vulkan
}

/// What setup does about the renderer (owner, 2026-11-02): GSdx is a download, so a new install starts with CoreGS 2.0, which is
/// inside the game, when the PC can run it. Nothing changes for a player who chose a renderer or already plays with GSdx.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SetupRenderer {
    /// CoreGS 2.0 is the stored renderer: there is nothing to install.
    Coregs20,
    /// The first game of a new install: try CoreGS 2.0 on this PC, and fall back to GSdx.
    TryCoregs20,
    /// GSdx stays the renderer: installed now if it is missing or too old.
    Gsdx,
    /// This platform has no PC GSdx to install (Android): keep its own renderer (GSdx (ARMSX2), or CoreGS 2.0 if the
    /// player chose it) and do not touch the download path.
    NothingToInstall,
}

/// `first_game`: no game was set up before this one. `gsdx_missing`: GSdx is not installed, or is below the minimum.
///
/// TASK-180 (owner, 2026-11-06): a platform without GSdx (Android) installs nothing at the end of setup. CoreGS 2.0 is inside the
/// game there, so it is the answer whatever the stored value, the game number or the presence of `gt4-gs.exe` says: setup must
/// never fall back to a download that cannot run.
pub fn setup_renderer(platform: Platform, settings: &Settings, first_game: bool, gsdx_missing: bool) -> SetupRenderer {
    if !platform.has_gsdx() {
        // The phone keeps its own renderer (GSdx (ARMSX2) by default, CoreGS 2.0 if chosen). Setup must neither try
        // CoreGS 2.0 "on this PC" nor download the PC GSdx; it only reports that there is nothing to install.
        return SetupRenderer::NothingToInstall;
    }
    if uses_coregs20(settings) {
        SetupRenderer::Coregs20
    } else if first_game && gsdx_missing && !settings.was_chosen("renderer") {
        SetupRenderer::TryCoregs20
    } else {
        SetupRenderer::Gsdx
    }
}

/// Whether a GSdx download or install may start here: `gt4-gs.exe` is a Windows program, so only where the platform has it.
pub fn gsdx_installs_here(platform: Platform) -> bool {
    platform.has_gsdx()
}

/// The message of a session GSdx cannot run: the reason, what to do, and the log folder. Shown on the home page and in
/// the tray, so the player is never left without a reason.
pub fn gsdx_unavailable_message(reason: &str, log_folder: &Path) -> String {
    gsdx_unavailable_message_in(reason, log_folder, Language::fallback())
}

/// [`gsdx_unavailable_message`] in `language`. The two known reasons are translated; any other reason is shown as it is.
pub fn gsdx_unavailable_message_in(reason: &str, log_folder: &Path, language: Language) -> String {
    let reason_text = match reason {
        GSDX_NOT_INSTALLED => messages::text("game.gs_not_installed", language).to_string(),
        GSDX_RESTART_REQUESTED => messages::text("game.gs_restart", language).to_string(),
        other => other.to_string(),
    };
    messages::render(
        "game.gs_could_not_run",
        language,
        &[("reason", &reason_text), ("folder", &log_folder.display().to_string())],
    )
}

/// The variables of a PCSX2 GS session: `GT4_RENDERER=pcsx2`, the process and its pin, the session INI, the
/// folders, and the fixed 480p and 16:9 holds (see `add_common`). The rows themselves (`HWDownloadMode`, blending) are
/// written into the INI by the table writer; the flare toggle is the game-side answer, so it travels as a variable.
pub fn pcsx2_environment(settings: &Settings, paths: &Pcsx2Paths) -> Vec<(String, String)> {
    let mut variables: BTreeMap<&'static str, String> = BTreeMap::new();
    variables.insert("GT4_RENDERER", "pcsx2".to_string());
    variables.insert("GT4_GS_PROCESS", paths.executable.display().to_string());
    variables.insert("GT4_GS_PROCESS_SHA256", paths.executable_sha256.clone());
    variables.insert("GT4_GS_CONFIG", paths.config.display().to_string());
    variables.insert("PS2_GS", "gpu".to_string());
    // TASK-186: the flare toggle drives the stream client's local zero answer (TASK-117) on this path; the local answer is
    // enabled only when the flares are off, so the car icons and the loading picture keep draining accurately. The road
    // sheen is identified in CoreGS 2.0's batch classifier, which the external GS path does not run on the game side, so
    // that item stays CoreGS-only and has no row here.
    let flares = settings.gs_is_on("effect_flares");
    variables.insert("GT4_GS_HIDE_FLARES", flag(!flares));
    if !flares {
        variables.insert("GT4_GS_READBACK_LOCAL", "1".to_string());
    }
    add_common(&mut variables, settings);
    variables.into_iter().map(|(name, value)| (name.to_string(), value)).collect()
}

/// Whether this session runs CoreGS 2.0: the player chose it (TASK-027; both games, the choice is shared).
pub fn uses_coregs20(settings: &Settings) -> bool {
    settings.get("renderer") == settings::CORE_GS_20
}

/// The variables of a CoreGS 2.0 session (TASK-027). The renderer lives in the game executable, so no process, pin or
/// INI goes with it. Scale: the session plan's multiplier, as GSdx gets it. The effect toggles (TASK-186): each of the old
/// Performance Mode's items is its own switch. Custom textures: the same Textures folder and the same row as GSdx. The card
/// is passed by name; the game takes the best card when no card has that name, and `GT4_HW_ADAPTER` is left out when no
/// card is known.
pub fn coregs20_environment(
    settings: &Settings,
    display: Option<&DisplayTarget>,
    integrated_gpu: bool,
    textures: &TextureLocation,
) -> Vec<(String, String)> {
    let mut variables: BTreeMap<&'static str, String> = BTreeMap::new();
    variables.insert("GT4_RENDERER", "hw".to_string());
    variables.insert("GT4_HW_CORE", "2".to_string());
    variables.insert("GT4_HW_SCALE", session_plan(settings, display, integrated_gpu).multiplier.to_string());
    // TASK-186 (owner, 2026-11-06): Performance Mode is replaced by independent effect toggles. The renderer's switches name
    // the effect they turn OFF (GT4_CORE2_PERF_<ITEM>=1); a toggle that is on sends 0, so the default of every effect is
    // drawn as the original game. The road sheen is CoreGS-only (its classifier is core 2's). Blending accuracy and the
    // Readbacks mode are GSdx rows: CoreGS 2.0 has neither setting.
    variables.insert("GT4_CORE2_PERF_FLARES", flag(!settings.gs_is_on("effect_flares")));
    variables.insert("GT4_CORE2_PERF_ROAD_SHEEN", flag(!settings.gs_is_on("effect_road_sheen")));
    // TASK-082: one frame drawn in two, in a running race. The physics stay at 60 Hz.
    // TASK-146 (rc12, experimental): 60 FPS lets the framerate drop in a running race when the PC falls behind, leaving
    // frames out so the game keeps its full speed; with time to spare every frame is drawn.
    if settings.get("frame_rate") == "30" {
        variables.insert("GT4_FRAME_SKIP_FORCE", "2".to_string());
    } else {
        variables.insert("GT4_FRAME_SKIP", "1".to_string());
    }
    // TASK-180 test5 (owner, 2026-11-05): Performance Mode is replaced by independent effect toggles. The renderer's
    // switches name the effect they turn OFF (GT4_CORE2_PERF_<ITEM>=1); a toggle that is on sends 0, so the default of
    // every effect is drawn as the original game. The road sheen is CoreGS-only (its classifier is core 2's).
    variables.insert("GT4_CORE2_PERF_FLARES", flag(!settings.gs_is_on("effect_flares")));
    variables.insert("GT4_CORE2_PERF_ROAD_SHEEN", flag(!settings.gs_is_on("effect_road_sheen")));
    // TASK-186 round 8 (owner, 2026-11-06): the old bundle's third picture item (IN-497), the small effect targets at 1x.
    // Its exact default is the full resolution, so it is a choice and only "1x" turns it down; with this the launcher can
    // reproduce every item of GT4_CORE2_PERFORMANCE=1 (the delayed downloads are answered by the flare row).
    variables.insert(
        "GT4_CORE2_EFFECTS_1X",
        if settings.gs_value("effect_targets") == "1x" { "1" } else { "0" }.to_string(),
    );
    // TASK-174: the one-queue presentation path is the game's default on a GPU with one graphics queue; only "off" is passed.
    if !settings.is_on("present_thread") {
        variables.insert("GT4_HW_PRESENT_SHARED_QUEUE", "0".to_string());
    }
    // TASK-135, TASK-175: the performance HUD. Full is GT4_HW_HUD=1, FPS only is GT4_HW_HUD=fps, Off sets nothing.
    match settings.get("performance_hud") {
        crate::settings::HUD_FULL => drop(variables.insert("GT4_HW_HUD", "1".to_string())),
        crate::settings::HUD_FPS_ONLY => drop(variables.insert("GT4_HW_HUD", "fps".to_string())),
        _ => {}
    }
    // No frametime.csv in the session folder any more (owner, 2026-11-03: not needed). GT4_HW_FRAMETIME_LOG=<csv> by hand
    // still writes it for a bench (scripts/coregs2_bench/frametime_plot.py).
    if settings.gs_value("texture_load") == "true" {
        variables.insert("GT4_HW_TEXTURES", textures.folder.display().to_string());
    }
    if settings.gs_value("fxaa") == "true" {
        variables.insert("GT4_HW_POSTFX", "fxaa".to_string());
    }
    // FSR1 RCAS, in stops: 0 is the strongest, 0.2 FSR's usual, 2 mild (absent: off).
    match settings.get("coregs_sharpening") {
        "mild" => drop(variables.insert("GT4_CORE2_FSR_SHARPNESS", "2".to_string())),
        "medium" => drop(variables.insert("GT4_CORE2_FSR_SHARPNESS", "0.2".to_string())),
        "strong" => drop(variables.insert("GT4_CORE2_FSR_SHARPNESS", "0".to_string())),
        _ => {}
    }
    let card = settings.get("graphics_card");
    if !card.is_empty() && card != crate::gpu::AUTOMATIC {
        variables.insert("GT4_HW_ADAPTER", card.to_string());
    }
    // The renderer's compiled shaders: beside the default Textures folder, or in the data folder when the player chose a
    // folder of their own (TASK-157; never beside it). The game finds its own place when it starts alone.
    if let Some(cache) = &textures.shader_cache {
        variables.insert("GT4_HW_SHADER_CACHE", cache.display().to_string());
    }
    add_common(&mut variables, settings);
    // "Fast" vsync is not offered under CoreGS 2.0 (owner, 2026-10-30): a stored one, kept for GSdx, draws as On here.
    if settings.get("vsync") == "fast" {
        variables.insert("GT4_PRESENT_MODE", "fifo".to_string());
    }
    variables.into_iter().map(|(name, value)| (name.to_string(), value)).collect()
}

/// The variables of a session: the fixed 480p and 16:9 holds, display mode, present mode, async GS and diagnostics.
fn add_common(variables: &mut BTreeMap<&'static str, String>, settings: &Settings) {
    // Fixed (owner, 2026-10-21): the game always runs at 480p with its own 16:9. No setting changes these, and no stored
    // value of the retired `input_resolution` or `widescreen` settings is read (settings::RETIRED_SETTINGS).
    variables.insert("GT4_FORCE_480P", "1".to_string());
    variables.insert("GT4_FORCE_WIDESCREEN", "1".to_string());
    variables.insert("GT4_FULLSCREEN", flag(settings.is_on("fullscreen")));
    variables.insert("GT4_DISPLAY_ASPECT", "auto".to_string());
    variables.insert(
        "GT4_PRESENT_MODE",
        match settings.get("vsync") {
            "fast" => "mailbox",
            "off" => "immediate",
            _ => "fifo",
        }
        .to_string(),
    );
    if settings.is_on("performance_report") {
        variables.insert("GT4_PERF", "1".to_string());
    }
    // TASK-161: the game's full trigger range and the controls' trigger ranges follow this one switch (game_main.cpp).
    variables.insert("GT4_TRIGGER_FULL_RANGE", flag(settings.is_on("trigger_full_range")));
    match settings.get("check_vu_code") {
        "100" => {
            variables.insert("GT4_VU1_VERIFY", "100".to_string());
            variables.insert("GT4_VU0_VERIFY", "100".to_string());
        }
        "1000" => {
            variables.insert("GT4_VU1_VERIFY", "1000".to_string());
            variables.insert("GT4_VU0_VERIFY", "1000".to_string());
        }
        _ => {}
    }
}

/// `GT4_MONITOR` (the monitor's device name) and `GT4_OUTPUT_SIZE` (`WxH`) for a resolved display target; nothing
/// when no monitor is known (the game then opens on the primary monitor at its desktop size).
pub fn display_environment(display: Option<&DisplayTarget>) -> Vec<(String, String)> {
    match display {
        Some(target) => vec![
            ("GT4_MONITOR".to_string(), target.device.clone()),
            ("GT4_OUTPUT_SIZE".to_string(), target.output.text()),
        ],
        None => Vec::new(),
    }
}

/// The render plan of a session (TASK-080 addendum): the output size of the display target (1920 x 1080 when none is
/// known) and the Render scale, for the PCSX2 GS. `integrated_gpu` is true when the game renders on an integrated GPU
/// (issue #89): the untouched default scale then plays at 1x. Never fails: an output the plan refuses falls back to
/// 1920 x 1080.
pub fn session_plan(settings: &Settings, display: Option<&DisplayTarget>, integrated_gpu: bool) -> display::DisplayPlan {
    let scale = settings.get("render_scale").parse::<u32>().unwrap_or(display::RENDER_SCALE_DEFAULT);
    let output = display.map(|target| target.output).unwrap_or(display::FALLBACK_OUTPUT);
    let fallback = || {
        display::display_plan(
            display::FALLBACK_OUTPUT.width,
            display::FALLBACK_OUTPUT.height,
            display::RENDER_SCALE_DEFAULT,
            display::PCSX2_GS_CAPS,
        )
        .expect("1920x1080 at 100% is a valid plan")
    };
    display::display_plan_on(output.width, output.height, scale, display::PCSX2_GS_CAPS, integrated_gpu)
        .unwrap_or_else(|_| fallback())
}

/// The gt4-gs.ini text of a session, rendered from the table (`gs_table::render_ini`, same rules as
/// scripts/gs_settings.py). The vsync and mailbox rows follow the present mode the way the probe does:
/// fifo writes both true, mailbox writes mailbox false, immediate writes vsync false. `upscale_multiplier` comes from
/// the session plan ([`session_plan`]); a stored `upscale` value is never used (there is no such setting). With no
/// display known and Render scale 100% the plan gives 3x, the table default, so that INI is unchanged.
pub fn gs_config_text(
    settings: &Settings,
    paths: &Pcsx2Paths,
    display: Option<&DisplayTarget>,
    integrated_gpu: bool,
) -> Result<String, String> {
    let mut user: BTreeMap<String, String> = settings.gs_overrides().clone();
    // TASK-186: the effect toggles are launcher-only rows (written = "never"): they reach the game as variables, never in
    // this file, so they are dropped before the table's own refusal sees them.
    user.retain(|id, _| gs_table::row(id).map(|row| row.written != "never").unwrap_or(true));
    // TASK-108: the Adapter (the chosen graphics card's name) is written by the launcher, not by the table: its row is
    // not settable from a value yet (status resolved). It is added below the table's render, in its section.
    let adapter = user.remove("adapter").filter(|name| !name.is_empty());
    user.insert("upscale".to_string(), session_plan(settings, display, integrated_gpu).multiplier.to_string());
    // TASK-186 (owner, 2026-11-06): Performance Mode is no longer a preset. Blending accuracy and the Readbacks mode are
    // player rows of the table now (`blending`, `hw_download_mode`), written by the table writer like every other row; no
    // effect toggle writes either, so an effect that is on is never broken.
    let present = match settings.get("vsync") {
        "fast" => "mailbox",
        "off" => "immediate",
        _ => "fifo",
    };
    let vsync = if present == "immediate" { "false" } else { "true" };
    let mailbox = if present == "mailbox" { "false" } else { "true" };
    user.entry("vsync".to_string()).or_insert_with(|| vsync.to_string());
    user.entry("mailbox".to_string()).or_insert_with(|| mailbox.to_string());
    let mut launcher: BTreeMap<String, String> = BTreeMap::new();
    launcher.insert("cache_folder".to_string(), paths.cache.display().to_string());
    launcher.insert("data_root".to_string(), paths.data_root.display().to_string());
    launcher.insert("textures_folder".to_string(), paths.textures.display().to_string());
    let text = gs_table::render_ini(&user, &launcher).map_err(|error| error.to_string())?;
    match adapter {
        None => Ok(text),
        Some(name) => {
            let header = "[EmuCore/GS]
";
            if !text.contains(header) {
                return Err("gt4-gs.ini has no [EmuCore/GS] section for the Adapter".to_string());
            }
            Ok(text.replacen(header, &format!("{header}Adapter = {name}
"), 1))
        }
    }
}

/// PRIVATE Android test build: the ARMSX2 GS folders under the app's external files folder. `config` is the file the
/// launcher writes (the same base settings a gt4-gs session gets on Windows); `overrides` is the owner's own file, created
/// empty once and never rewritten, so his extra lines survive every Play.
pub struct Armsx2Paths {
    pub root: PathBuf,
    pub config: PathBuf,
    pub overrides: PathBuf,
    pub resources: PathBuf,
}

/// The ARMSX2 GS folders under `external`, the app's external files folder.
pub fn armsx2_paths(external: &std::path::Path) -> Armsx2Paths {
    let root = external.join("armsx2gs");
    Armsx2Paths {
        config: root.join("armsx2-gs.ini"),
        overrides: root.join("armsx2-gs-extra.ini"),
        resources: root.join("resources"),
        root,
    }
}

/// The armsx2-gs.ini text of a session: the Windows launcher's own base settings (`gs_config_text`: the same table, the
/// same blending accuracy, aspect, OSD and folder rows) with the internal-resolution multiplier taken from the shared
/// Render scale row and the screen, snapped to what the ARMSX2 GS accepts (whole, or fractional 1.25x .. 1.75x). A
/// fractional multiplier needs `ExtendedUpscalingMultipliers`, so that key is added where the result is fractional.
pub fn armsx2_config_text(
    settings: &Settings,
    paths: &Armsx2Paths,
    textures: &std::path::Path,
    display: Option<&DisplayTarget>,
) -> Result<String, String> {
    let gs_paths = Pcsx2Paths {
        executable: paths.root.join("libgt4gs.so"),
        executable_sha256: String::new(),
        config: paths.config.clone(),
        cache: paths.root.join("cache"),
        data_root: paths.root.clone(),
        textures: textures.to_path_buf(),
    };
    let mut text = gs_config_text(settings, &gs_paths, None, false)?;
    // The same rule as the PC's GSdx: the Render scale is a percentage of the screen's height over the game's 448
    // lines. On the phone it lands between two whole multipliers, so it becomes the nearest one ARMSX2 can use
    // (1x, 1.25x, 1.5x, 1.75x, 2x, 2.5x, 3x, 4x, 5x, 6x); a fractional one turns ExtendedUpscalingMultipliers on.
    let multiplier = armsx2_multiplier(settings, display);
    let scale = format_multiplier(multiplier);
    text = text
        .lines()
        .map(|line| {
            if line.trim_start().starts_with("upscale_multiplier") {
                if scale.contains('.') {
                    format!("upscale_multiplier = {scale}\nExtendedUpscalingMultipliers = true")
                } else {
                    format!("upscale_multiplier = {scale}")
                }
            } else if line.trim_start().starts_with("LoadTextureReplacements")
                && !line.trim_start().starts_with("LoadTextureReplacementsAsync")
            {
                // The "Custom textures" row owns loading on the phone: the CoreGS rows (and their texture_load value) are
                // hidden while the ARMSX2 GS runs, so the table's line is replaced by this row's (owner, 2026-11-06).
                format!("LoadTextureReplacements = {}", settings.is_on("armsx2_textures"))
            } else {
                line.to_string()
            }
        })
        .collect::<Vec<_>>()
        .join("\n");
    if !text.ends_with('\n') {
        text.push('\n');
    }
    // The ARMSX2 GS picture options, at the end of the [EmuCore/GS] block so they win over the table's own values.
    // The owner's armsx2-gs-extra.ini is applied after this file by the library, so his lines still override everything.
    let effects = armsx2_effect_lines(settings);
    // The file says which renderer it is for: 12 is PCSX2's OpenGL renderer, 14 its Vulkan one.
    // Auto is GSRendererType::Auto (-1): the library keeps its own choice instead of reading a forced backend from the
    // launcher's default (owner, 2026-11-06, round 6: the per-GPU rules stay in charge unless the player picks one).
    let renderer = match settings.get("armsx2_api") {
        crate::settings::ARMSX2_API_VULKAN => 14,
        crate::settings::ARMSX2_API_GLES => 12,
        _ => -1,
    };
    let mut out: Vec<String> = Vec::new();
    let mut in_gs = false;
    let mut inserted = false;
    for line in text.lines() {
        let trimmed = line.trim_start();
        let is_header = trimmed.starts_with('[') && trimmed.ends_with(']');
        if is_header && in_gs && !inserted {
            out.extend(effects.iter().cloned());
            inserted = true;
        }
        if is_header {
            in_gs = trimmed == "[EmuCore/GS]";
        }
        if trimmed.starts_with("Renderer = ") {
            out.push(format!("Renderer = {renderer}"));
            continue;
        }
        out.push(line.to_string());
    }
    if in_gs && !inserted {
        out.extend(effects.iter().cloned());
    }
    text = out.join("\n") + "\n";
    Ok(text)
}

/// The internal-resolution multipliers the ARMSX2 GS accepts with `ExtendedUpscalingMultipliers` on. The Render scale
/// row lands between two of them on a phone screen; the nearest one is used.
pub const ARMSX2_MULTIPLIERS: [f32; 10] = [1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0];

/// The multiplier nearest to `wanted` among [`ARMSX2_MULTIPLIERS`] (ties go to the smaller one, so rounding never
/// grows the internal resolution behind the player's back).
pub fn armsx2_snap_multiplier(wanted: f32) -> f32 {
    let mut best = ARMSX2_MULTIPLIERS[0];
    let mut best_distance = f32::MAX;
    for candidate in ARMSX2_MULTIPLIERS {
        let distance = (candidate - wanted).abs();
        if distance < best_distance || (distance == best_distance && candidate < best) {
            best = candidate;
            best_distance = distance;
        }
    }
    best
}

/// The internal-resolution multiplier of a session: the PC's Render scale rule (the percentage of the screen's height
/// over the game's 448 lines) snapped to what the ARMSX2 GS can use. No screen known falls back to the PC's 1920x1080.
pub fn armsx2_multiplier(settings: &Settings, display: Option<&DisplayTarget>) -> f32 {
    let percent = settings
        .get("render_scale")
        .parse::<u32>()
        .unwrap_or(display::RENDER_SCALE_DEFAULT);
    let output = display.map(|target| target.output).unwrap_or(display::FALLBACK_OUTPUT);
    let height = output.height.min(output.width);
    if height == 0 {
        return 1.0;
    }
    let wanted = percent as f32 * height as f32 / (100.0 * display::NATIVE_FRAME_HEIGHT as f32);
    armsx2_snap_multiplier(wanted)
}

/// `2` for a whole multiplier, `2.5` for a fractional one: what the ini and the row's label show.
pub fn format_multiplier(multiplier: f32) -> String {
    if (multiplier - multiplier.round()).abs() < 0.001 {
        format!("{}", multiplier.round() as i64)
    } else {
        let text = format!("{multiplier:.2}");
        text.trim_end_matches('0').trim_end_matches('.').to_string()
    }
}

/// The `[EmuCore/GS]` lines of the ARMSX2 GS picture options: FXAA, CAS (mode and amount), texture filtering,
/// anisotropic filtering and dithering, each from its launcher row. The values are the ones PCSX2's own settings
/// file uses; the owner's extra file still overrides them, because the library applies it after this file.
pub fn armsx2_effect_lines(settings: &Settings) -> Vec<String> {
    let strength: u32 = settings.get("armsx2_cas_strength").parse().unwrap_or(50);
    let filter = match settings.get("armsx2_filter") {
        "nearest" => 0,
        "bilinear-forced" => 1,
        "bilinear-forced-sprite-excluded" => 3,
        _ => 2,
    };
    let aniso = match settings.get("armsx2_aniso") {
        "2" => 2,
        "4" => 4,
        "8" => 8,
        "16" => 16,
        _ => 0,
    };
    let dither = match settings.get("armsx2_dither") {
        "off" => 0,
        "scaled" => 1,
        _ => 2,
    };
    // The upscaler is Vulkan-only: FSR1 = 2 and SGSR = 3 in ARMSX2's GSUpscaler enum; OpenGL ES always writes Off (0),
    // so a value left over from a Vulkan session cannot ask the GLES device for a compute pass it does not have.
    let upscaler = if settings.get("armsx2_api") == "vk" {
        match settings.get("armsx2_upscaler") {
            "fsr1" => 2,
            "sgsr" => 3,
            _ => 0,
        }
    } else {
        0
    };
    vec![
        format!("fxaa = {}", settings.is_on("armsx2_fxaa")),
        format!("Upscaler = {upscaler}"),
        // The table's hidden texture_load_async row already writes LoadTextureReplacementsAsync (on), so only the
        // preloading switch of the ARMSX2 GS is added here: the owner asked for it with the pack (2026-11-06), and it
        // follows the Custom textures row.
        format!("PrecacheTextureReplacements = {}", settings.is_on("armsx2_textures")),
        format!("CASMode = {}", if settings.is_on("armsx2_cas") { 1 } else { 0 }),
        format!("CASSharpness = {}", strength.min(100)),
        format!("filter = {filter}"),
        format!("MaxAnisotropy = {aniso}"),
        format!("dithering_ps2 = {dither}"),
    ]
}

/// Name of the file the game reads for a direct start (TASK-097), in the user config folder.
pub const STANDALONE_CONFIG_FILE: &str = "game.ini";

/// Name of the gt4-gs.ini the launcher keeps in the user config folder for a direct start (TASK-097). A Play still writes
/// its own copy into the session folder.
pub const STANDALONE_GS_INI_FILE: &str = "gt4-gs.ini";

/// The `game.ini` text (TASK-097): the player's choices that the launcher turns into variables at Play, so a direct start of
/// the game (no launcher) honours them. `key=value` lines read by `native/standalone.cpp`:
/// `fullscreen`, `present_mode`, `monitor`, `output_size` (only when a monitor is known), `performance_report`,
/// `check_vu_code` and `trigger_full_range` (TASK-161). Each key stands for the variable [`add_common`] and [`display_environment`] set (the mapping is
/// tested against them). Anything the game defaults itself (GSdx, 480p, 16:9, automatic ultrawide) is not written.
/// With CoreGS 2.0 chosen, [`coregs20_section`] adds its keys.
pub fn standalone_config_text(settings: &Settings, display: Option<&DisplayTarget>) -> String {
    let present = match settings.get("vsync") {
        "fast" => "mailbox",
        "off" => "immediate",
        _ => "fifo",
    };
    let check_vu_code = match settings.get("check_vu_code") {
        "100" => "100",
        "1000" => "1000",
        _ => "0",
    };
    let mut text = String::from(
        "# GT4Recomp: what the launcher chose for a direct start of the game (TASK-097).\n\
         # Written by the launcher whenever a setting changes and before every Play. Do not edit by hand.\n\
         version=1\n",
    );
    text.push_str(&format!("fullscreen={}\n", flag(settings.is_on("fullscreen"))));
    text.push_str(&format!("present_mode={present}\n"));
    if let Some(target) = display {
        text.push_str(&format!("monitor={}\n", target.device));
        text.push_str(&format!("output_size={}\n", target.output.text()));
    }
    text.push_str(&format!("performance_report={}\n", flag(settings.is_on("performance_report"))));
    text.push_str(&format!("check_vu_code={check_vu_code}\n"));
    text.push_str(&format!("trigger_full_range={}\n", flag(settings.is_on("trigger_full_range"))));
    // TASK-148: the chosen controller, as GT4_PAD.
    if let Some(choice) = controller_environment(settings) {
        text.push_str(&format!("pad={choice}\n"));
    }
    text
}

/// TASK-181: the `game.ini` line of the racing wheel file, `wheel_file=<path>` (the game sets `GT4_WHEEL_FILE` from it on
/// a direct start). The launcher adds it only when `wheel.toml` exists; at Play it passes `GT4_WHEEL_FILE` itself.
pub fn wheel_file_line(path: &std::path::Path) -> String {
    format!("{STANDALONE_WHEEL_KEY}={}\n", path.display())
}

/// The `game.ini` key of [`wheel_file_line`].
pub const STANDALONE_WHEEL_KEY: &str = "wheel_file";

/// TASK-148: the `GT4_PAD` value of the chosen controller (`controller::PadChoice` text), `None` while none is stored.
pub fn controller_environment(settings: &Settings) -> Option<String> {
    let value = settings.get("controller");
    crate::controller::PadChoice::parse(value).map(|_| value.to_string())
}

/// TASK-185: the `GT4_VIRTUAL_PAD` mode of one stored value: `always`, `off`, and `auto` for the default and for anything
/// unrecognised (an edited settings file never takes the pad away by accident).
pub fn virtual_pad_mode(value: &str) -> &'static str {
    match value {
        "always" => "always",
        "off" => "off",
        _ => "auto",
    }
}

/// TASK-185: the `GT4_VIRTUAL_PAD` value at Play, from the `virtual_pad` setting of the launcher's Controls page. `auto`
/// is the default and also what a build without the setting yet passes (the row belongs to the launcher's editor).
pub fn virtual_pad_environment(settings: &Settings) -> &'static str {
    if crate::settings::definition("virtual_pad").is_none() {
        return "auto";
    }
    virtual_pad_mode(settings.get("virtual_pad"))
}

/// TASK-148: the SDL gamepad mappings file in the user config folder (`gamecontrollerdb.txt` format). The launcher loads
/// it and the game gets it as SDL_GAMECONTROLLERCONFIG_FILE: a mapping there makes a controller SDL3 does not know usable.
pub const GAMEPAD_MAPPINGS_FILE: &str = "gamecontrollerdb.txt";

/// The `game.ini` keys of a CoreGS 2.0 choice: `renderer=coregs20`, then `hw_scale`, `hw_performance`, `hw_textures`,
/// `hw_frame_skip`, `hw_frame_skip_adaptive`, `hw_hud`, `hw_present_shared_queue` (written only when off), `hw_postfx`, `hw_sharpening` and `hw_adapter`, each standing for its variable in [`coregs20_environment`], and
/// `hw_present_mode=fifo` over a stored "Fast" vsync (only here, so GSdx keeps its `present_mode`).
pub fn coregs20_section(settings: &Settings, display: Option<&DisplayTarget>, integrated_gpu: bool, textures: &TextureLocation) -> String {
    let mut text = String::from("renderer=coregs20\n");
    if settings.get("vsync") == "fast" {
        text.push_str("hw_present_mode=fifo\n");
    }
    for (name, value) in coregs20_environment(settings, display, integrated_gpu, textures) {
        let key = match name.as_str() {
            "GT4_HW_SCALE" => "hw_scale",
            "GT4_CORE2_PERF_FLARES" => "hw_effect_flares",
            "GT4_CORE2_PERF_ROAD_SHEEN" => "hw_effect_road_sheen",
            "GT4_FRAME_SKIP_FORCE" => "hw_frame_skip",
            "GT4_FRAME_SKIP" => "hw_frame_skip_adaptive",
            "GT4_HW_HUD" => "hw_hud",
            "GT4_HW_PRESENT_SHARED_QUEUE" => "hw_present_shared_queue",
            "GT4_HW_TEXTURES" => "hw_textures",
            "GT4_HW_POSTFX" => "hw_postfx",
            "GT4_CORE2_FSR_SHARPNESS" => "hw_sharpening",
            "GT4_HW_ADAPTER" => "hw_adapter",
            _ => continue,
        };
        text.push_str(&format!("{key}={value}\n"));
    }
    text
}

/// Whether the game asked for a restart with the built-in renderer (its log holds the marker line). The launcher
/// answers with the GSdx error, not with a restart (TASK-096).
pub fn restart_requested(log_text: &str) -> bool {
    log_text.lines().any(|line| line.contains(GS_RESTART_MARKER))
}

fn flag(on: bool) -> String {
    if on { "1" } else { "0" }.to_string()
}

/// Inherited variables that are passed through to the game although they carry a scrubbed prefix. Each one is a
/// hidden owner test knob that the game validates itself (an invalid value refuses the start):
/// - `GT4_ASPECT_FOV_EXTRA` (TASK-080 owner test T1, 0.5..3.0, default 1.0): an extra horizontal FOV factor on top of
///   the geometry-correct Hor+ widening, to compare roundness. It is not a setting and no row writes it.
/// - `GT4_AUDIO_LATENCY_MS` (TASK-156, 20..500): the audio queue target, for Proton and SteamOS players whose sound
///   crackles (Steam launch options); the game keeps its default and says so for any other value.
pub const PASSED_THROUGH: &[&str] = &["GT4_ASPECT_FOV_EXTRA", "GT4_AUDIO_LATENCY_MS"];

/// Whether an inherited variable must be removed for the game process.
pub fn is_scrubbed(name: &str) -> bool {
    let upper = name.to_ascii_uppercase();
    !PASSED_THROUGH.iter().any(|p| p.eq_ignore_ascii_case(name))
        && SCRUBBED_PREFIXES.iter().any(|prefix| upper.starts_with(prefix))
}

/// The argument that makes the game program compile the CoreGS 2.0 shader cache and leave (IN-507).
pub const PREPARE_SHADERS_ARGUMENT: &str = "--prepare-shaders";

/// One line of `--prepare-shaders` on stdout.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ShaderPrepareLine {
    Progress { done: u32, total: u32 },
    Ready,
}

/// Reads a line of `--prepare-shaders`; anything else is `None`.
pub fn parse_shader_prepare_line(line: &str) -> Option<ShaderPrepareLine> {
    let mut words = line.split_whitespace();
    if words.next()? != "shaders" {
        return None;
    }
    let line = match words.next()? {
        "ready" => ShaderPrepareLine::Ready,
        "progress" => ShaderPrepareLine::Progress { done: words.next()?.parse().ok()?, total: words.next()?.parse().ok()? },
        _ => return None,
    };
    words.next().is_none().then_some(line)
}

/// Plans one CoreGS 2.0 launch. Pure.
pub fn plan_coregs20_launch(
    inputs: &LaunchInputs<'_>,
    settings: &Settings,
    display: Option<&DisplayTarget>,
    integrated_gpu: bool,
    textures: &TextureLocation,
    inherited_names: &[String],
) -> LaunchPlan {
    let environment = coregs20_environment(settings, display, integrated_gpu, textures);
    plan_with(inputs, with_display(environment, display), inherited_names)
}

/// Plans one PCSX2 GS launch. Pure: the caller writes `paths.config` (see [`gs_config_text`]) before starting.
pub fn plan_pcsx2_launch(
    inputs: &LaunchInputs<'_>,
    settings: &Settings,
    paths: &Pcsx2Paths,
    display: Option<&DisplayTarget>,
    inherited_names: &[String],
) -> LaunchPlan {
    let environment = pcsx2_environment(settings, paths);
    plan_with(inputs, with_display(environment, display), inherited_names)
}

fn with_display(mut environment: Vec<(String, String)>, display: Option<&DisplayTarget>) -> Vec<(String, String)> {
    environment.extend(display_environment(display));
    environment.sort();
    environment
}

fn plan_with(inputs: &LaunchInputs<'_>, environment: Vec<(String, String)>, inherited_names: &[String]) -> LaunchPlan {
    let misses = inputs.session_dir.join("misses.txt");
    let arguments = vec![
        inputs.elf.display().to_string(),
        "--ntsc".to_string(),
        "--disc".to_string(),
        inputs.disc.display().to_string(),
        "--frames".to_string(),
        "0".to_string(),
        "--memcard".to_string(),
        inputs.card.display().to_string(),
        "--misses".to_string(),
        misses.display().to_string(),
        "--window".to_string(),
    ];
    let mut removed: Vec<String> = inherited_names.iter().filter(|name| is_scrubbed(name)).cloned().collect();
    removed.sort();
    removed.dedup();
    LaunchPlan {
        program: inputs.game_executable.to_path_buf(),
        arguments,
        environment,
        removed_inherited: removed,
        working_directory: inputs.session_dir.to_path_buf(),
        log_path: inputs.session_dir.join("game.log"),
        misses_path: misses,
    }
}

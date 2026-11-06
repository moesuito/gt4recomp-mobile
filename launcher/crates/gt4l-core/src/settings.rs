//! The single table of user settings.
//!
//! Every setting appears once in [`SETTINGS`]. The config file, the validation, the Settings pages
//! and the game environment all read this table. Values are stored as strings (`"on"`/`"off"` for
//! toggles) so the TOML file stays readable. Environment variables are produced in `launch.rs`.
//!
//! `connected` is `false` for an option whose game-side variable is not in the build yet. Such an
//! option is shown greyed out and never sent, so an unknown value cannot stop the game.
//! The renderer is GSdx (the PCSX2 GS in its own process) only: the launcher never starts CoreGS 1.1 (TASK-096).

use std::collections::{BTreeMap, BTreeSet};
use std::fmt;

/// Settings pages in the order of the Settings navigation (Saves, Replays, Controls and About
/// have their own pages).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Page {
    General,
    Graphics,
    Display,
    Advanced,
}

/// One allowed value of a choice setting.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Choice {
    pub value: &'static str,
    pub label: &'static str,
    /// `false` keeps the entry visible but greyed out ("coming soon"); it cannot be selected.
    pub selectable: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    /// One of a fixed list of values, shown as a dropdown or a segmented control.
    Choice(&'static [Choice]),
    /// `on` or `off`.
    Toggle,
    /// A whole percentage from 0 to 100, shown as a slider. Stored as decimal text, e.g. `50`.
    Percent,
    /// A whole number from `min` to `max` in steps of `step`, shown as a slider (TASK-080 Render scale, percent).
    /// Stored as decimal text, e.g. `100`.
    Steps { min: u32, max: u32, step: u32 },
    /// A connected monitor (TASK-080 addendum): `primary`, or a stored identifier `<device>@<W>x<H>`
    /// (`display::monitor_id`). The list of choices comes from the platform at run time, so any well-formed
    /// identifier is accepted here; a monitor that is gone resolves to the primary one (`display::resolve_monitor`).
    Monitor,
    /// The graphics card that renders the game (TASK-108): `auto` (Automatic), or the name of a card as the launcher
    /// lists it. The list comes from the platform at run time; a card that is gone falls back to Automatic for that run.
    Gpu,
    /// An output resolution (TASK-080 addendum): `desktop` (the monitor's desktop mode) or `<W>x<H>`. A size the
    /// chosen monitor does not support resolves to its desktop mode (`display::resolve_resolution`).
    Resolution,
    /// The launcher's language (TASK-102): `auto`, or a tag listed in `ui/locales/index.json` (`language::LANGUAGE_TAGS`).
    /// The choices come from that list, not from this table.
    Language,
    /// The controller the game reads (TASK-148): `auto` (controller 1), or `<guid>` / `<guid>/<serial>` of a controller
    /// (`controller::PadChoice`). Chosen on the Controls page from the pads connected now; a controller that is gone falls
    /// back to controller 1 for that run.
    Controller,
}

/// One row of a Settings page.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SettingDef {
    /// Stable identifier used in `settings.toml`. Never rename without a schema migration.
    pub id: &'static str,
    pub page: Page,
    pub group: &'static str,
    pub title: &'static str,
    /// Text of the "i" hover card. No helper text is shown under rows.
    pub info: &'static str,
    pub kind: Kind,
    pub default: &'static str,
    /// `false` = shown but not sent to the game yet (its variable is not in this build).
    pub connected: bool,
}

/// The PCSX2 GS in its own process (downloaded, default when installed). Decision 0070.
pub const PCSX2_GS: &str = "pcsx2";
/// Our own hardware renderer, inside the game executable (beta).
pub const CORE_GS_20: &str = "coregs20";
/// PRIVATE Android test build only: the ARMSX2 (GPL) GS running inside the game process beside CoreGS 2.0. Never shipped,
/// never offered on Windows.
pub const ARMSX2_GS: &str = "armsx2";

/// The Vulkan-only upscalers (owner, 2026-11-06): ARMSX2's own `Upscaler` values. OpenGL ES keeps the plain picture
/// plus CAS.
const ARMSX2_UPSCALER_CHOICES: &[Choice] = &[
    Choice { value: "off", label: "Off", selectable: true },
    Choice { value: "fsr1", label: "FSR1 (AMD)", selectable: true },
    Choice { value: "sgsr", label: "Snapdragon Super Resolution", selectable: true },
];

const RENDERER_CHOICES: &[Choice] = &[
    Choice { value: PCSX2_GS, label: "GSdx", selectable: true },
    Choice { value: CORE_GS_20, label: "CoreGS 2.0", selectable: true },
    Choice { value: ARMSX2_GS, label: "GSdx (ARMSX2)", selectable: true },
];

/// The ARMSX2 GS resolution multipliers (private Android test build): whole and fractional scales. PCSX2 applies a
/// fractional multiplier only with ExtendedUpscalingMultipliers on, which `armsx2_gs_config_text` writes.
/// The API the ARMSX2 GS runs on. OpenGL ES is its Android default; Vulkan is experimental (see the help text).
pub const ARMSX2_API_GLES: &str = "ogl";
pub const ARMSX2_API_VULKAN: &str = "vk";

const ARMSX2_API_CHOICES: &[Choice] = &[
    Choice { value: ARMSX2_API_GLES, label: "OpenGL ES", selectable: true },
    Choice { value: ARMSX2_API_VULKAN, label: "Vulkan (experimental)", selectable: true },
];

/// The ARMSX2 GS texture filter (PCSX2's `filter` key: 0 nearest, 1 bilinear forced, 2 PS2, 3 forced but sprites excluded).
const ARMSX2_FILTER_CHOICES: &[Choice] = &[
    Choice { value: "nearest", label: "Nearest", selectable: true },
    Choice { value: "bilinear-ps2", label: "Bilinear (PS2)", selectable: true },
    Choice { value: "bilinear-forced", label: "Bilinear (forced)", selectable: true },
    Choice { value: "bilinear-forced-sprite-excluded", label: "Bilinear (sprites excluded)", selectable: true },
];

/// The ARMSX2 GS anisotropic filtering (`MaxAnisotropy`, the plain number).
const ARMSX2_ANISO_CHOICES: &[Choice] = &[
    Choice { value: "off", label: "Off", selectable: true },
    Choice { value: "2", label: "2x", selectable: true },
    Choice { value: "4", label: "4x", selectable: true },
    Choice { value: "8", label: "8x", selectable: true },
    Choice { value: "16", label: "16x", selectable: true },
];

/// The ARMSX2 GS dithering (`dithering_ps2`: 0 off, 1 scaled, 2 unscaled as the PS2).
const ARMSX2_DITHER_CHOICES: &[Choice] = &[
    Choice { value: "off", label: "Off", selectable: true },
    Choice { value: "unscaled", label: "PS2 (unscaled)", selectable: true },
    Choice { value: "scaled", label: "Scaled", selectable: true },
];

const ARMSX2_SCALE_CHOICES: &[Choice] = &[
    Choice { value: "1", label: "1x (native)", selectable: true },
    Choice { value: "1.25", label: "1.25x", selectable: true },
    Choice { value: "1.5", label: "1.5x", selectable: true },
    Choice { value: "1.75", label: "1.75x", selectable: true },
    Choice { value: "2", label: "2x", selectable: true },
    Choice { value: "2.5", label: "2.5x", selectable: true },
    Choice { value: "3", label: "3x", selectable: true },
    Choice { value: "4", label: "4x", selectable: true },
    Choice { value: "5", label: "5x", selectable: true },
    Choice { value: "6", label: "6x", selectable: true },
];

/// The strength of CoreGS 2.0's FSR1 sharpening (GSdx has CAS instead).
const FRAME_RATE_CHOICES: &[Choice] = &[
    Choice { value: "60", label: "60 FPS", selectable: true },
    Choice { value: "30", label: "30 FPS", selectable: true },
];

const COREGS_SHARPENING_CHOICES: &[Choice] = &[
    Choice { value: "off", label: "Off", selectable: true },
    Choice { value: "mild", label: "Mild", selectable: true },
    Choice { value: "medium", label: "Medium", selectable: true },
    Choice { value: "strong", label: "Strong", selectable: true },
];

const CHANNEL_CHOICES: &[Choice] = &[
    Choice { value: "stable", label: "Stable", selectable: true },
    Choice { value: "beta", label: "Beta", selectable: true },
];

const VSYNC_CHOICES: &[Choice] = &[
    Choice { value: "on", label: "On", selectable: true },
    Choice { value: "fast", label: "Fast", selectable: true },
    Choice { value: "off", label: "Off", selectable: true },
];

/// TASK-175: the performance HUD's three states. Earlier launchers stored a toggle (`on`/`off`): `on` is read as `full`
/// ([`legacy_value`]).
pub const HUD_FPS_ONLY: &str = "fps";
pub const HUD_FULL: &str = "full";

const HUD_CHOICES: &[Choice] = &[
    Choice { value: "off", label: "Off", selectable: true },
    Choice { value: HUD_FPS_ONLY, label: "FPS only", selectable: true },
    Choice { value: HUD_FULL, label: "Full", selectable: true },
];

const VERIFY_CHOICES: &[Choice] = &[
    Choice { value: "off", label: "Off", selectable: true },
    Choice { value: "100", label: "Every 100th call", selectable: true },
    Choice { value: "1000", label: "Every 1000th call", selectable: true },
];

const TOGGLE: Kind = Kind::Toggle;

/// Every user setting, in display order. Ids are the TOML keys.
pub const SETTINGS: &[SettingDef] = &[
    SettingDef {
        id: "controller",
        page: Page::General,
        group: "Controls",
        title: "Controller",
        info: "The controller the game reads. At every start the launcher selects a connected controller: the one chosen last time, or else the first one connected.",
        kind: Kind::Controller,
        default: crate::controller::AUTO_CONTROLLER,
        connected: true,
    },
    SettingDef {
        id: "trigger_full_range",
        page: Page::General,
        group: "Controls",
        title: "Full trigger range",
        info: "Throttle and brake follow the whole travel of the triggers in finer steps: about 255 levels instead of the game's \
               own 46 for the throttle and 85 for the brake, still 100% at the end of the travel. Off is the game as it was \
               made.",
        kind: TOGGLE,
        // On by default since the owner's rc14 test (2026-11-04).
        default: "on",
        connected: true,
    },
    SettingDef {
        id: "language",
        page: Page::General,
        group: "Language",
        title: "Language",
        info: "The language of the launcher. Auto follows your Windows language (Portuguese (Brazil) or English); the other two choices always use that language.",
        kind: Kind::Language,
        default: "auto",
        connected: true,
    },
    SettingDef {
        id: "update_channel",
        page: Page::General,
        group: "Language",
        title: "Channel",
        info: "Stable gets the normal releases. Beta also gets the release candidates, to test new features early.                Going back to Stable never installs an older version by itself.",
        kind: Kind::Choice(CHANNEL_CHOICES),
        default: "stable",
        connected: true,
    },
    SettingDef {
        id: "renderer",
        page: Page::Graphics,
        group: "Renderer",
        title: "Renderer",
        info: "GSdx (ARMSX2) runs the ARMSX2 emulator's GS inside the game and is the default of this private build. \n               CoreGS 2.0 is our own renderer and the second choice, with a much lower processor cost.",
        kind: Kind::Choice(RENDERER_CHOICES),
        default: PCSX2_GS,
        connected: true,
    },
    SettingDef {
        id: "armsx2_textures",
        page: Page::Graphics,
        group: "ARMSX2 GS",
        title: "Custom textures",
        info: "Load texture packs for the ARMSX2 GS from the launcher's textures folder (the same folder and packs as                on the PC). Off uses the game's own textures.",
        kind: Kind::Toggle,
        default: "on",
        connected: true,
    },
    SettingDef {
        id: "armsx2_api",
        page: Page::Graphics,
        group: "ARMSX2 GS",
        title: "ARMSX2 GS graphics API",
        info: "OpenGL ES is the API the ARMSX2 GS uses on Android by default: it is the one ARMSX2 keeps supported there.                Vulkan is experimental: ARMSX2 keeps it out of its Android renderer choice because of blending bugs, and                this private build may not carry it at all (the game log then says so and stays on OpenGL ES).",
        kind: Kind::Choice(ARMSX2_API_CHOICES),
        default: ARMSX2_API_GLES,
        connected: true,
    },
    SettingDef {
        id: "armsx2_fxaa",
        page: Page::Graphics,
        group: "ARMSX2 GS",
        title: "Anti-aliasing (FXAA)",
        info: "Fast approximate anti-aliasing applied to the finished picture by the ARMSX2 GS. Off by default: it costs                a little graphics time and softens the image slightly.",
        kind: Kind::Toggle,
        default: "off",
        connected: true,
    },
    SettingDef {
        id: "armsx2_cas",
        page: Page::Graphics,
        group: "ARMSX2 GS",
        title: "Sharpening (CAS)",
        info: "Contrast adaptive sharpening of the finished picture, the ARMSX2 GS's own sharpening filter. Off by default.",
        kind: Kind::Toggle,
        default: "off",
        connected: true,
    },
    SettingDef {
        id: "armsx2_cas_strength",
        page: Page::Graphics,
        group: "ARMSX2 GS",
        title: "Sharpening amount",
        info: "Strength of the ARMSX2 GS sharpening (0 to 100). Used when sharpening is on.",
        kind: Kind::Steps { min: 0, max: 100, step: 1 },
        default: "50",
        connected: true,
    },
    SettingDef {
        id: "armsx2_upscaler",
        page: Page::Graphics,
        group: "ARMSX2 GS",
        title: "Upscaler",
        info: "FSR1 (AMD) and Snapdragon Super Resolution (Qualcomm) work only on the Vulkan back end: they appear while                Vulkan is chosen and are off under OpenGL ES, which has only the normal sharpening. Off keeps the plain                picture; the render scale still sets the internal resolution.",
        kind: Kind::Choice(ARMSX2_UPSCALER_CHOICES),
        default: "off",
        connected: true,
    },
    SettingDef {
        id: "armsx2_filter",
        page: Page::Graphics,
        group: "ARMSX2 GS",
        title: "Texture filtering",
        info: "How the ARMSX2 GS samples game textures. Bilinear (PS2) follows the console; Nearest is the sharpest and                the most aliased; the forced modes override what the game asks for.",
        kind: Kind::Choice(ARMSX2_FILTER_CHOICES),
        default: "bilinear-ps2",
        connected: true,
    },
    SettingDef {
        id: "armsx2_aniso",
        page: Page::Graphics,
        group: "ARMSX2 GS",
        title: "Anisotropic filtering",
        info: "Anisotropic filtering of the ARMSX2 GS. Off keeps the frame time budget; 16x is the sharpest and costs                graphics time.",
        kind: Kind::Choice(ARMSX2_ANISO_CHOICES),
        default: "off",
        connected: true,
    },
    SettingDef {
        id: "armsx2_dither",
        page: Page::Graphics,
        group: "ARMSX2 GS",
        title: "Dithering",
        info: "The dithering the ARMSX2 GS applies. PS2 (unscaled) is the console's own, which is what the original                picture has; Off shows the bands the dithering hides; Scaled grows it with the resolution.",
        kind: Kind::Choice(ARMSX2_DITHER_CHOICES),
        default: "unscaled",
        connected: true,
    },
    SettingDef {
        id: "render_scale",
        page: Page::Graphics,
        group: "Image",
        title: "Render scale",
        info: "How sharp the picture is drawn. 100% draws it at least at your resolution (taken from the Resolution                setting on the Display page); 50% is faster and softer, 200% is sharper and needs a much faster GPU.                The number next to the slider is the resulting internal resolution.",
        kind: Kind::Steps {
            min: crate::display::RENDER_SCALE_MIN,
            max: crate::display::RENDER_SCALE_MAX,
            step: crate::display::RENDER_SCALE_STEP,
        },
        default: "100",
        connected: true,
    },
    SettingDef {
        id: "frame_rate",
        page: Page::Graphics,
        group: "Performance",
        title: "Frame rate (experimental)",
        info: "60 FPS is the default: in a race, when the PC cannot keep up, the frame rate drops instead of the game slowing down, with the same physics. The 30 FPS mode is for weaker PCs: races draw 30 frames per second. Both are experimental; menus stay at 60.",
        kind: Kind::Choice(FRAME_RATE_CHOICES),
        default: "60",
        connected: true,
    },
    SettingDef {
        id: "present_thread",
        page: Page::Graphics,
        group: "Performance",
        title: "Presentation thread (one-queue GPUs)",
        info: "For the Steam Deck and other GPUs that have a single queue for graphics work. The game shows finished frames from a separate thread, so it does not wait for the screen. On other graphics cards this changes nothing: they already do it.",
        kind: TOGGLE,
        default: "on",
        connected: true,
    },
    SettingDef {
        id: "coregs_sharpening",
        page: Page::Graphics,
        group: "Image",
        title: "Sharpening (FSR1)",
        info: "AMD FidelityFX Super Resolution 1 sharpening of the final picture. Off leaves the picture as the game drew it.",
        kind: Kind::Choice(COREGS_SHARPENING_CHOICES),
        default: "off",
        connected: true,
    },
    SettingDef {
        id: "graphics_card",
        page: Page::Graphics,
        group: "Image",
        title: "Graphics card",
        info: "The graphics card that draws the game. Automatic picks a dedicated card over integrated graphics, then the one with the most video memory.",
        kind: Kind::Gpu,
        default: "auto",
        connected: true,
    },
    SettingDef {
        id: "monitor",
        page: Page::Display,
        group: "Window",
        title: "Monitor",
        info: "The screen the game opens on. The primary monitor is used until you pick another one.",
        kind: Kind::Monitor,
        default: crate::display::PRIMARY,
        connected: true,
    },
    SettingDef {
        id: "resolution",
        page: Page::Display,
        group: "Window",
        title: "Resolution",
        info: "The size of the picture, from the sizes your monitor supports. The shape of the screen follows it:                at 16:9 the game is unchanged, other shapes widen or heighten the race view. A size below the                monitor's own is drawn at that size and stretched to fill the screen.",
        kind: Kind::Resolution,
        default: crate::display::DESKTOP,
        connected: true,
    },
    SettingDef {
        id: "fullscreen",
        page: Page::Display,
        group: "Window",
        title: "Fullscreen",
        info: "Start in fullscreen on the chosen monitor (a borderless window that covers it). Alt+Enter or F11                switches while playing.",
        kind: TOGGLE,
        default: "on",
        connected: true,
    },
    SettingDef {
        id: "vsync",
        page: Page::Display,
        group: "Window",
        title: "VSync",
        info: "On keeps motion smooth with no tearing. Fast and Off lower latency and may tear. Modes \
               the graphics driver lacks are not offered.",
        kind: Kind::Choice(VSYNC_CHOICES),
        default: "on",
        connected: true,
    },
    SettingDef {
        id: "performance_hud",
        page: Page::Advanced,
        group: "Performance",
        title: "Performance HUD",
        info: "Shows a panel in the corner while you play. FPS only shows just the frame rate. Full shows the graphics card and \
               processor names, the frame rate and frame time with a live graph, the game's use of the processor, the graphics \
               card and memory, and the state of the performance report when that is on.",
        kind: Kind::Choice(HUD_CHOICES),
        default: "off",
        connected: true,
    },
    SettingDef {
        id: "performance_report",
        page: Page::Advanced,
        group: "Diagnostics",
        title: "Performance report",
        info: "Records frame timing while you play for bug reports.",
        kind: TOGGLE,
        default: "off",
        connected: true,
    },
    SettingDef {
        id: "check_vu_code",
        page: Page::Advanced,
        group: "Diagnostics",
        title: "Check compiled VU code",
        info: "Compares compiled vector-unit code with the reference while playing. Slower.",
        kind: Kind::Choice(VERIFY_CHOICES),
        default: "off",
        connected: true,
    },
    SettingDef {
        id: "discord_rich_presence",
        page: Page::General,
        group: "Discord",
        title: "Discord Rich Presence",
        info: "Shows that you are playing Gran Turismo 4 or Gran Turismo 4 Spec II in your Discord profile while a game is running. The game name, GT4Recomp and the session time are shown. No mode, track, car, save, account or path is shared. On by default; turn off to keep your play private.",
        kind: TOGGLE,
        default: "on",
        connected: true,
    },
];

/// Settings that earlier launchers stored and that are now fixed (owner, 2026-10-21). A value of one of these in
/// `settings.toml` (or in `[choices]`) is ignored when the file is read and dropped at the next write; no schema step is
/// needed because the launcher no longer reads them. The game always gets the fixed value (`launch.rs`).
pub const RETIRED_SETTINGS: &[(&str, &str)] = &[
    ("input_resolution", "the game always runs at 480p (GT4_FORCE_480P=1)"),
    ("async_gs", "graphics work always goes to the GPU without waiting; GT4_GS_SYNC stays a developer switch of the game"),
    ("widescreen", "the game's own 16:9 is always on (GT4_FORCE_WIDESCREEN=1); the Resolution sets the screen shape"),
    ("aspect_ratio", "the aspect is always the Resolution's shape (TASK-080 addendum)"),
    ("aspect_custom", "the aspect is always the Resolution's shape (TASK-080 addendum)"),
    ("internal_scale", "a CoreGS 1.1 row (TASK-096): the launcher no longer starts CoreGS 1.1"),
    ("present_filter", "a CoreGS 1.1 row (TASK-096): the launcher no longer starts CoreGS 1.1"),
    ("fsr_sharpness", "a CoreGS 1.1 row (TASK-096): the launcher no longer starts CoreGS 1.1"),
    ("deinterlacing", "a CoreGS 1.1 row (TASK-096): the launcher no longer starts CoreGS 1.1"),
    ("anti_aliasing", "a CoreGS 1.1 row (TASK-096): the launcher no longer starts CoreGS 1.1"),
];

/// PCSX2 GS table rows the launcher derives from other settings, never stored as a user value (TASK-080 addendum):
/// `upscale` (`upscale_multiplier`) follows the Resolution and the Render scale (`display::display_plan`).
pub const LAUNCHER_DERIVED_GS: &[(&str, &str)] =
    &[("upscale", "the internal resolution follows the Resolution and the Render scale")];

/// Why a PCSX2 GS row is derived by the launcher, or `None`.
pub fn launcher_derived_gs(id: &str) -> Option<&'static str> {
    LAUNCHER_DERIVED_GS.iter().find(|(derived, _)| *derived == id).map(|(_, reason)| *reason)
}

/// Why a retired setting is ignored, or `None` for any other id.
pub fn retired(id: &str) -> Option<&'static str> {
    RETIRED_SETTINGS.iter().find(|(retired_id, _)| *retired_id == id).map(|(_, reason)| *reason)
}

/// Looks up a definition by id.
pub fn definition(id: &str) -> Option<&'static SettingDef> {
    SETTINGS.iter().find(|definition| definition.id == id)
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SettingError {
    UnknownSetting(String),
    InvalidValue { id: String, value: String },
}

impl fmt::Display for SettingError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            SettingError::UnknownSetting(id) => write!(formatter, "unknown setting '{id}'"),
            SettingError::InvalidValue { id, value } => {
                write!(formatter, "value '{value}' is not allowed for '{id}'")
            }
        }
    }
}

impl std::error::Error for SettingError {}

/// A value an earlier launcher stored for a setting that has since changed type, read as its current value (TASK-175: the
/// performance HUD was a toggle; `on` meant the whole panel). Any other value is returned as it is.
pub fn legacy_value<'a>(id: &str, value: &'a str) -> &'a str {
    if id == "performance_hud" && value == "on" {
        HUD_FULL
    } else {
        value
    }
}

/// Checks one value against its definition. Choices must be selectable; toggles are `on`/`off`.
pub fn validate(definition: &SettingDef, value: &str) -> Result<String, SettingError> {
    let invalid = || SettingError::InvalidValue { id: definition.id.to_string(), value: value.to_string() };
    match definition.kind {
        Kind::Toggle => match value {
            "on" | "off" => Ok(value.to_string()),
            _ => Err(invalid()),
        },
        Kind::Choice(choices) => choices
            .iter()
            .find(|choice| choice.selectable && choice.value == value)
            .map(|choice| choice.value.to_string())
            .ok_or_else(invalid),
        Kind::Percent => match value.parse::<u8>() {
            Ok(percent) if percent <= 100 => Ok(percent.to_string()),
            _ => Err(invalid()),
        },
        Kind::Steps { min, max, step } => match value.parse::<u32>() {
            Ok(number) if (min..=max).contains(&number) && (number - min) % step.max(1) == 0 => Ok(number.to_string()),
            _ => Err(invalid()),
        },
        Kind::Gpu => {
            let name = value.trim();
            if !name.is_empty() && name.len() <= 256 && !name.chars().any(|c| c.is_control()) {
                Ok(name.to_string())
            } else {
                Err(invalid())
            }
        }
        Kind::Monitor => {
            if value == crate::display::PRIMARY || crate::display::parse_monitor_id(value).is_some() {
                Ok(value.to_string())
            } else {
                Err(invalid())
            }
        }
        Kind::Controller => {
            if value == crate::controller::AUTO_CONTROLLER {
                return Ok(value.to_string());
            }
            match crate::controller::PadChoice::parse(value) {
                Some(choice) => Ok(crate::controller::PadChoice::key(&choice.guid, &choice.serial)),
                None => Err(invalid()),
            }
        }
        Kind::Language => {
            if value == "auto" {
                return Ok(value.to_string());
            }
            match crate::language::Language::from_tag(value) {
                Some(language) => Ok(language.tag().to_string()),
                None => Err(invalid()),
            }
        }
        Kind::Resolution => {
            if value == crate::display::DESKTOP {
                return Ok(value.to_string());
            }
            match crate::display::Size::parse(value) {
                Some(size) => Ok(size.text()),
                None => Err(invalid()),
            }
        }
    }
}

/// Validated values for every setting. Always holds one entry per definition.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Settings {
    values: BTreeMap<&'static str, String>,
    gs: BTreeMap<String, String>,
    /// Settings the user set on the Settings page (`[choices]` in settings.toml). A migration keeps these as they are.
    chosen: BTreeSet<&'static str>,
}

impl Default for Settings {
    fn default() -> Self {
        let values = SETTINGS.iter().map(|definition| (definition.id, definition.default.to_string())).collect();
        Settings { values, gs: BTreeMap::new(), chosen: BTreeSet::new() }
    }
}

impl Settings {
    /// The value of a setting. Every definition has a value, so this never fails for a known id.
    pub fn get(&self, id: &str) -> &str {
        match self.values.get(id) {
            Some(value) => value.as_str(),
            None => definition(id).map(|definition| definition.default).unwrap_or("off"),
        }
    }

    pub fn is_on(&self, id: &str) -> bool {
        self.get(id) == "on"
    }

    /// Sets a validated value. Unknown ids and invalid values are refused and leave the state unchanged.
    pub fn set(&mut self, id: &str, value: &str) -> Result<(), SettingError> {
        let definition = definition(id).ok_or_else(|| SettingError::UnknownSetting(id.to_string()))?;
        let value = validate(definition, legacy_value(id, value))?;
        self.values.insert(definition.id, value);
        Ok(())
    }

    /// Sets a value the user chose on the Settings page, and marks it as chosen.
    pub fn set_by_user(&mut self, id: &str, value: &str) -> Result<(), SettingError> {
        self.set(id, value)?;
        if let Some(definition) = definition(id) {
            self.chosen.insert(definition.id);
        }
        Ok(())
    }

    /// Marks a setting as chosen by the user (used when a file is read).
    pub fn mark_chosen(&mut self, id: &str) {
        if let Some(definition) = definition(id) {
            self.chosen.insert(definition.id);
        }
    }

    /// Whether the user chose this value (see [`Settings::set_by_user`]).
    pub fn was_chosen(&self, id: &str) -> bool {
        self.chosen.contains(id)
    }

    /// The ids the user chose, in table order.
    pub fn chosen_ids(&self) -> Vec<&'static str> {
        self.chosen.iter().copied().collect()
    }

    /// Sets a value, or resets it to the default when it is invalid. Returns a warning for the log.
    pub fn set_or_default(&mut self, id: &str, value: &str) -> Option<String> {
        match self.set(id, value) {
            Ok(()) => None,
            Err(error) => {
                if let Some(definition) = definition(id) {
                    self.values.insert(definition.id, definition.default.to_string());
                }
                Some(format!("{error}; using the default"))
            }
        }
    }

    /// Whether a shown row can be changed now: it is connected to the game.
    pub fn is_active(&self, id: &str) -> bool {
        definition(id).map(|definition| definition.connected).unwrap_or(false)
    }

    /// Whether a GS table toggle is on (TASK-180 test5: the effect toggles read `[gs] effect_<name> = true`).
    pub fn gs_is_on(&self, id: &str) -> bool {
        self.gs_value(id) == "true"
    }

    /// A PCSX2 GS table value (`gs_table`), or its table default when the user has not set it.
    pub fn gs_value(&self, id: &str) -> String {
        match self.gs.get(id) {
            Some(value) => value.clone(),
            None => crate::gs_table::default_value(id).unwrap_or_default(),
        }
    }

    /// Sets a PCSX2 GS table value after the table's own check (range, choice, status).
    pub fn set_gs(&mut self, id: &str, value: &str) -> Result<(), String> {
        if let Some(reason) = launcher_derived_gs(id) {
            return Err(format!("{id} is set by the launcher: {reason}"));
        }
        let normalized = crate::gs_table::check_user_value(id, value)?;
        self.gs.insert(id.to_string(), normalized);
        Ok(())
    }

    /// Sets a PCSX2 GS value for one run, without the table's checks: the run's resolved API, card and Adapter
    /// (TASK-108). `None` removes the value, so the game's own default applies. Never stored in settings.toml.
    pub fn set_run_gs(&mut self, id: &str, value: Option<&str>) {
        match value {
            Some(value) => {
                self.gs.insert(id.to_string(), value.to_string());
            }
            None => {
                self.gs.remove(id);
            }
        }
    }

    /// Drops every PCSX2 GS value the user set, so each table row is back at its default.
    pub fn clear_gs(&mut self) {
        self.gs.clear();
    }

    /// The PCSX2 GS values the user has set (only these are stored in `settings.toml`).
    pub fn gs_overrides(&self) -> &BTreeMap<String, String> {
        &self.gs
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_language_setting_takes_auto_and_the_listed_languages_only() {
        let definition = definition("language").expect("the language row");
        assert_eq!(definition.default, "auto");
        for value in ["auto", "en-US", "pt-BR"] {
            assert_eq!(validate(definition, value).as_deref(), Ok(value), "{value}");
        }
        // Spelling of a listed tag is normalised to the tag of index.json.
        assert_eq!(validate(definition, "pt_BR").as_deref(), Ok("pt-BR"));
        assert_eq!(validate(definition, "pt-br").as_deref(), Ok("pt-BR"));
        for value in ["fr-FR", "xx", ""] {
            assert!(validate(definition, value).is_err(), "{value:?} is not a language of the launcher");
        }
    }
}

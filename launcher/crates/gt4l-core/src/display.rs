//! Display target and the automatic render plan (TASK-080 addendum, owner design 2026-10-21; decision 0071).
//!
//! The player picks a **Monitor** and a **Resolution** (the output size W x H) and a **Render scale**. There is no
//! aspect choice and no internal-resolution choice:
//! - the aspect is always the output size's ratio (`GT4_DISPLAY_ASPECT=auto` with `GT4_OUTPUT_SIZE`); at 16:9 nothing
//!   changes, at any other ratio the game's P1/P2 path (camera Hor+/Vert+, race view fill, HUD centred 16:9) turns on
//!   by itself;
//! - the internal render resolution is never below the output: `k = ceil(max(W / 640, H / 448))`, where 640 x 448 is
//!   GT4's native frame, and the effective multiplier is `k x render scale`, snapped to a multiplier the renderer
//!   accepts (100% is exactly `k`, so at 100% the frame is never smaller than the screen).
//!
//! Everything here is pure and platform-independent (Windows reads the monitors in `gt4l-platform::display`; Android
//! and iOS will feed their own screen size into [`display_plan`]).

use std::fmt;

/// GT4's native frame (the game draws 640 x 448 at 480p; 16:9 is the same frame stretched).
pub const NATIVE_FRAME_WIDTH: u32 = 640;
pub const NATIVE_FRAME_HEIGHT: u32 = 448;

/// The game treats a ratio within this relative distance of 16:9 as exactly 16:9 (`native/display_aspect.hpp`
/// `kSnapTolerance`); 1366 x 768 is 16:9.
pub const SNAP_TOLERANCE: f64 = 0.005;
/// The ratio range the game accepts (`kMinRatio`, `kMaxRatio`); the output size must give a ratio inside it.
pub const MIN_RATIO: f64 = 0.5;
pub const MAX_RATIO: f64 = 8.0;
/// Output sizes the game accepts for `GT4_OUTPUT_SIZE` (each side), the same rule as `native/display_aspect.cpp`.
pub const MIN_OUTPUT_SIDE: u32 = 320;
pub const MAX_OUTPUT_SIDE: u32 = 16384;

/// Render scale: a percentage of the **automatic multiplier** (owner, 2026-11-06). `k` is the smallest multiplier whose
/// internal frame covers the screen (`640 x k >= W` and `448 x k >= H`), so 100% is `k` on every device and 50% is half
/// of it. The multiplier is the accepted value closest to `k x percentage`, with an exact tie going up. The range reaches
/// below 50% so a value stored under the old screen-height model still keeps its multiplier (1x on a 1440p phone was
/// 31%); the row itself offers one position per multiplier.
pub const RENDER_SCALE_MIN: u32 = 10;
pub const RENDER_SCALE_MAX: u32 = 200;
pub const RENDER_SCALE_STEP: u32 = 1;
pub const RENDER_SCALE_DEFAULT: u32 = 100;
/// The multiplier the phone ran before this model: the owner's 2x on the S23 at 720p (100% on a 16:9 720p screen, where
/// k = 2; 67% on a 19.3:9 one, where k = 3).
pub const RENDER_SCALE_DEFAULT_MULTIPLIER: u32 = 2;

/// The fractional scales GSdx (ARMSX2) accepts on top of the whole multipliers (TASK-200): finer positions of the same
/// Render scale, never a separate choice.
pub const FRACTIONAL_MULTIPLIERS: &[f64] = &[1.25, 1.5, 1.75, 2.5];

/// What a renderer accepts as an internal-resolution multiplier.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RendererCaps {
    /// Multipliers 1 to `max_multiplier`.
    pub max_multiplier: u32,
    /// Whether the fractional multipliers of [`FRACTIONAL_MULTIPLIERS`] are accepted too.
    pub fractional: bool,
}

/// The PCSX2 GS (gt4-gs) up to 1.2.1: whole numbers 1 to 12 (the gt4-gs allow-list; the table row `upscale`).
pub const PCSX2_GS_CAPS: RendererCaps = RendererCaps { max_multiplier: 12, fractional: false };

/// The PCSX2 GS from 1.3.0 and the ARMSX2 GS: the same range plus the four fractional scales.
pub const PCSX2_GS_FRACTIONAL_CAPS: RendererCaps = RendererCaps { max_multiplier: 12, fractional: true };

/// CoreGS 2.0 (`GT4_HW_SCALE`): whole multipliers only, the list it has always taken.
pub const CORE_GS_CAPS: RendererCaps = RendererCaps { max_multiplier: 12, fractional: false };

/// Every multiplier a renderer accepts, ascending.
pub fn accepted_multipliers(caps: RendererCaps) -> Vec<f64> {
    let max = caps.max_multiplier.max(1) as f64;
    let mut values: Vec<f64> = (1..=caps.max_multiplier.max(1)).map(|multiplier| multiplier as f64).collect();
    if caps.fractional {
        values.extend(FRACTIONAL_MULTIPLIERS.iter().copied().filter(|multiplier| *multiplier <= max));
    }
    values.sort_by(|left, right| left.partial_cmp(right).expect("multipliers are finite"));
    values
}

/// The accepted multiplier closest to `value`, clamped to 1 .. `caps.max_multiplier`; an exact tie goes to the larger
/// value (the Render scale's "ties up" rule).
pub fn snap_multiplier(value: f64, caps: RendererCaps) -> f64 {
    let maximum = caps.max_multiplier.max(1) as f64;
    let value = value.clamp(1.0, maximum);
    let mut best = 1.0;
    let mut best_distance = f64::INFINITY;
    for candidate in accepted_multipliers(caps) {
        let distance = (candidate - value).abs();
        if distance <= best_distance {
            best = candidate;
            best_distance = distance;
        }
    }
    best
}

/// The multiplier as it is written and shown: "3" for a whole one, "2.5" for a fractional one.
pub fn multiplier_text(multiplier: f64) -> String {
    format!("{multiplier}")
}

/// The slider's steps in order: 10% to 200% in 1% steps (the reference's 25% steps could not hold the owner's 62.5%).
pub fn render_scale_steps() -> Vec<u32> {
    (RENDER_SCALE_MIN..=RENDER_SCALE_MAX).step_by(RENDER_SCALE_STEP as usize).collect()
}

/// The 16:9 picture of a multiplier's width, for the label: width 640 x multiplier, height floor(width x 9 / 16).
pub fn label_size(multiplier: f64) -> (u32, u32) {
    let width = (NATIVE_FRAME_WIDTH as f64 * multiplier).round() as u32;
    let height = ((width as f64) * 9.0 / 16.0).floor() as u32;
    (width, height)
}

/// The plan for one output size.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct DisplayPlan {
    /// The output size, landscape (a portrait input is turned sideways: the game is landscape only).
    pub output_width: u32,
    pub output_height: u32,
    /// Whether the input was portrait (height above width) and was turned.
    pub portrait_input: bool,
    /// Always false: kept for the callers that log the plan (the old integrated-GPU baseline no longer exists).
    pub integrated_default: bool,
    /// k = ceil(max(W / 640, H / 448)): the smallest whole multiplier whose frame covers the screen on both axes. It is
    /// the 100% plan, so at 100% the internal picture is never below the screen.
    pub base_multiplier: u32,
    /// The render scale in percent that was applied.
    pub render_scale: u32,
    /// The accepted multiplier closest to `k x render scale / 100` (an exact tie goes up), clamped to
    /// 1 .. caps.max_multiplier; whole numbers, or the fractional values too when the renderer takes them.
    pub multiplier: f64,
    /// 640 x multiplier by 448 x multiplier: the frame the renderer really draws.
    pub internal_width: u32,
    pub internal_height: u32,
    /// W / H.
    pub aspect: f64,
    /// Whether the aspect is not 16:9 (within the game's snap tolerance): the game's P1/P2 aspect path is on
    /// (Hor+ for wider screens, Vert+ for narrower ones such as 16:10 and 4:3).
    pub ultrawide_on: bool,
}

/// Why an output size or render scale is refused.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PlanError(pub String);

impl fmt::Display for PlanError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.0)
    }
}

impl std::error::Error for PlanError {}

fn ceil_div(value: u32, divisor: u32) -> u32 {
    value.div_ceil(divisor)
}

/// Whether a ratio is 16:9 for the game (within [`SNAP_TOLERANCE`]).
pub fn is_sixteen_nine(ratio: f64) -> bool {
    (ratio / (16.0 / 9.0) - 1.0).abs() <= SNAP_TOLERANCE
}

/// Whether a render scale is one of the slider's steps (10, 11, ..., 200).
pub fn is_render_scale(percent: u32) -> bool {
    (RENDER_SCALE_MIN..=RENDER_SCALE_MAX).contains(&percent) && (percent - RENDER_SCALE_MIN) % RENDER_SCALE_STEP == 0
}

/// k, the multiplier 100% gives on an output: the smallest whole multiplier whose frame covers the screen on **both**
/// axes (`ceil(max(W / 640, H / 448))`, the PC reference of task/200), so 100% is never below the screen. A portrait
/// input is read as its landscape size. A 16:9 720p screen gives 2 (2x is 100%), 1080p gives 3, 1440p 4 and 4K 6.
pub fn base_multiplier(output_width: u32, output_height: u32) -> u32 {
    let (width, height) = if output_height > output_width { (output_height, output_width) } else { (output_width, output_height) };
    ceil_div(width, NATIVE_FRAME_WIDTH).max(ceil_div(height, NATIVE_FRAME_HEIGHT)).max(1)
}

/// The render plan for an output size, a render scale (percent of the automatic multiplier) and a renderer. Pure.
///
/// Refused: a zero or out-of-range side, a ratio outside 1:2 .. 8:1 after turning a portrait input sideways, and a
/// render scale that is not a slider step.
pub fn display_plan(
    output_width: u32,
    output_height: u32,
    render_scale: u32,
    caps: RendererCaps,
) -> Result<DisplayPlan, PlanError> {
    display_plan_on(output_width, output_height, render_scale, caps, false)
}

/// [`display_plan`] for the GPU the game will use (`integrated_gpu` is kept for the callers and no longer changes the
/// plan).
pub fn display_plan_on(
    output_width: u32,
    output_height: u32,
    render_scale: u32,
    caps: RendererCaps,
    _integrated_gpu: bool,
) -> Result<DisplayPlan, PlanError> {
    if !is_render_scale(render_scale) {
        return Err(PlanError(format!(
            "render scale {render_scale}% is not one of {RENDER_SCALE_MIN}% to {RENDER_SCALE_MAX}% in steps of {RENDER_SCALE_STEP}%"
        )));
    }
    let portrait_input = output_height > output_width;
    let (width, height) = if portrait_input { (output_height, output_width) } else { (output_width, output_height) };
    let side_ok = |side: u32| (MIN_OUTPUT_SIDE..=MAX_OUTPUT_SIDE).contains(&side);
    if !side_ok(width) || !side_ok(height) {
        return Err(PlanError(format!(
            "output size {output_width}x{output_height} is outside {MIN_OUTPUT_SIDE} to {MAX_OUTPUT_SIDE} pixels per side"
        )));
    }
    let aspect = width as f64 / height as f64;
    if !(MIN_RATIO..=MAX_RATIO).contains(&aspect) {
        return Err(PlanError(format!("output size {output_width}x{output_height} is not a display ratio the game accepts")));
    }
    // The Render scale is a percentage of the automatic multiplier (owner, 2026-11-06): 100% is exactly k, 50% is half
    // of it, and the result lands on the accepted multiplier closest to that value.
    let base_multiplier = base_multiplier(width, height);
    let multiplier = snap_multiplier(base_multiplier as f64 * render_scale as f64 / 100.0, caps);
    Ok(DisplayPlan {
        output_width: width,
        output_height: height,
        portrait_input,
        integrated_default: false,
        base_multiplier,
        render_scale,
        multiplier,
        internal_width: (NATIVE_FRAME_WIDTH as f64 * multiplier).round() as u32,
        internal_height: (NATIVE_FRAME_HEIGHT as f64 * multiplier).round() as u32,
        aspect,
        ultrawide_on: !is_sixteen_nine(aspect),
    })
}

/// "3x · 1920x1080": the slider's readout for a plan (the 16:9 label of [`label_size`]).
pub fn describe_plan(plan: &DisplayPlan) -> String {
    let (width, height) = label_size(plan.multiplier);
    format!("{}x \u{00B7} {width}x{height}", multiplier_text(plan.multiplier))
}

// Render scale as a percentage of the automatic multiplier ----------------------------------------------------------

/// The whole multiplier the old screen-height percentage model gave (TASK-180 test3): nearest whole with halves going
/// down, exactly as its slider did. Used only by the store migration.
pub fn old_multiplier_for_percent(percent: u32, output_height: u32) -> u32 {
    if output_height == 0 {
        return 1;
    }
    let numerator = u64::from(percent) * u64::from(output_height);
    let denominator = 100 * u64::from(NATIVE_FRAME_HEIGHT);
    let multiplier = (2 * numerator + denominator - 1) / (2 * denominator);
    multiplier.max(1) as u32
}

/// The multiplier a value stored under the old notch model meant on this output and GPU (before TASK-180 test3):
/// `baseline + (percent - 100) / 25`, clamped to 1 .. `caps.max_multiplier`.
pub fn notch_multiplier(old_percent: u32, output: Size, integrated_gpu: bool, caps: RendererCaps) -> u32 {
    let baseline = if integrated_gpu { 1 } else { base_multiplier(output.width, output.height) };
    let steps = (old_percent as i32 - 100) / 25;
    (baseline as i32 + steps).clamp(1, caps.max_multiplier.max(1) as i32) as u32
}

/// The slider step that gives `target` on this output: the step whose snapped multiplier is the target, else the step
/// whose multiplier is closest. Among steps that achieve it, the one closest to the exact `100 x target / k` is stored
/// (an exact half goes up), so 2x at 1080p is 67% and 2.5x at 1440p is 63%.
pub fn percentage_for_multiplier(target: f64, output: Size, caps: RendererCaps) -> u32 {
    let base = base_multiplier(output.width, output.height) as f64;
    let ideal = (100.0 * target / base).clamp(RENDER_SCALE_MIN as f64, RENDER_SCALE_MAX as f64);
    let mut best = RENDER_SCALE_DEFAULT;
    let mut best_distance = f64::INFINITY;
    for percent in render_scale_steps() {
        let multiplier = snap_multiplier(base * percent as f64 / 100.0, caps);
        let distance = (multiplier - target).abs();
        let closer = distance < best_distance - 1e-9;
        let same = (distance - best_distance).abs() <= 1e-9;
        if closer || (same && (percent as f64 - ideal).abs() <= (best as f64 - ideal).abs()) {
            best = percent;
            best_distance = distance;
        }
    }
    best
}

/// The row's positions: one per multiplier the renderer accepts and 200% can reach, with the percentage that gives it.
pub fn render_scale_choices(output: Size, caps: RendererCaps) -> Vec<(u32, f64)> {
    let base = base_multiplier(output.width, output.height) as f64;
    let reachable = base * RENDER_SCALE_MAX as f64 / 100.0;
    let mut choices: Vec<(u32, f64)> = Vec::new();
    for candidate in accepted_multipliers(caps) {
        if candidate > reachable + 1e-9 {
            break;
        }
        let percent = percentage_for_multiplier(candidate, output, caps);
        if (snap_multiplier(base * percent as f64 / 100.0, caps) - candidate).abs() > 1e-9 {
            continue;
        }
        if choices.iter().any(|(_, multiplier)| (*multiplier - candidate).abs() <= 1e-9) {
            continue;
        }
        choices.push((percent, candidate));
    }
    choices
}

/// The percentage that keeps the phone's default multiplier (2x) on this output: 100% at 720p, 67% at 1080p, 50% at
/// 1440p. A fresh file is migrated to it, so phones keep running the multiplier they run today.
pub fn default_percentage(output: Size, caps: RendererCaps) -> u32 {
    percentage_for_multiplier(RENDER_SCALE_DEFAULT_MULTIPLIER as f64, output, caps)
}

// Monitors ---------------------------------------------------------------------------------------------------------

/// A width x height pair in physical pixels.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Size {
    pub width: u32,
    pub height: u32,
}

impl Size {
    pub const fn new(width: u32, height: u32) -> Size {
        Size { width, height }
    }

    /// "2560x1440".
    pub fn text(&self) -> String {
        format!("{}x{}", self.width, self.height)
    }

    /// Parses "WxH" (decimal digits only, each side 1 .. 16384). The game applies the same rule to
    /// `GT4_OUTPUT_SIZE` (with the side range above).
    pub fn parse(text: &str) -> Option<Size> {
        let (width, height) = text.split_once('x')?;
        let side = |part: &str| -> Option<u32> {
            if part.is_empty() || part.len() > 5 || !part.chars().all(|c| c.is_ascii_digit()) {
                return None;
            }
            part.parse::<u32>().ok().filter(|value| (1..=MAX_OUTPUT_SIDE).contains(value))
        };
        Some(Size { width: side(width)?, height: side(height)? })
    }
}

/// One connected monitor as the platform reads it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Monitor {
    /// The GDI device name, e.g. `\\.\DISPLAY1` (what the game's `GT4_MONITOR` takes).
    pub device: String,
    /// The friendly name ("DELL U3419W"); empty when the system has none.
    pub name: String,
    pub primary: bool,
    /// The current desktop mode.
    pub desktop: Size,
    /// The modes the monitor supports: unique sizes, sorted (largest width first, then largest height).
    pub modes: Vec<Size>,
}

/// Smallest mode offered: the game's own 640 x 480 picture (smaller modes are display oddities, not choices).
pub const MIN_MODE: Size = Size::new(640, 480);

/// Unique sizes, at least [`MIN_MODE`] on both sides, sorted largest first. The desktop size is always included.
pub fn tidy_modes(raw: &[Size], desktop: Size) -> Vec<Size> {
    let mut modes: Vec<Size> = raw
        .iter()
        .copied()
        .chain(std::iter::once(desktop))
        .filter(|size| size.width >= MIN_MODE.width && size.height >= MIN_MODE.height)
        .collect();
    modes.sort_by(|a, b| b.cmp(a));
    modes.dedup();
    if modes.is_empty() {
        modes.push(desktop);
    }
    modes
}

/// The stored identifier of a monitor: the device name and its desktop size, `\\.\DISPLAY1@3440x1440`. Stable across
/// launches; not an index (monitor order changes when screens are plugged in or out).
pub fn monitor_id(monitor: &Monitor) -> String {
    format!("{}@{}", monitor.device, monitor.desktop.text())
}

/// Splits a stored monitor identifier into the device name and the desktop size. `None` when it is not of that form.
pub fn parse_monitor_id(text: &str) -> Option<(&str, Size)> {
    let (device, size) = text.rsplit_once('@')?;
    if device.is_empty() || device.len() > 64 || !device.chars().all(|c| c.is_ascii_graphic()) {
        return None;
    }
    Some((device, Size::parse(size)?))
}

/// The stored value that means "the primary monitor" (the default until the player picks one).
pub const PRIMARY: &str = "primary";
/// The stored value that means "the monitor's desktop mode" (the default resolution).
pub const DESKTOP: &str = "desktop";

/// The monitor's friendly name ("DELL U3419W"); a monitor without one is called by its device ("DISPLAY1").
pub fn monitor_base_name(monitor: &Monitor) -> String {
    if monitor.name.trim().is_empty() {
        monitor.device.trim_start_matches(r"\\.\").to_string()
    } else {
        monitor.name.trim().to_string()
    }
}

/// The labels of the Monitor row, one per monitor in order: the friendly name only (no resolution, no "primary").
/// A monitor whose name is already taken gets the smallest suffix that tells it apart: " 2", then " 3" and so on.
pub fn monitor_labels(monitors: &[Monitor]) -> Vec<String> {
    let mut used: Vec<String> = Vec::new();
    let mut labels = Vec::new();
    for monitor in monitors {
        let base = monitor_base_name(monitor);
        let mut label = base.clone();
        let mut number = 2;
        while used.contains(&label) {
            label = format!("{base} {number}");
            number += 1;
        }
        used.push(label.clone());
        labels.push(label);
    }
    labels
}

/// The monitor a stored value names. Order: the exact identifier; the same device; a monitor with the same desktop
/// size; the primary monitor; the first monitor. `None` only for an empty list.
pub fn resolve_monitor<'a>(monitors: &'a [Monitor], stored: &str) -> Option<&'a Monitor> {
    let primary = || monitors.iter().find(|monitor| monitor.primary).or_else(|| monitors.first());
    if stored == PRIMARY {
        return primary();
    }
    let Some((device, desktop)) = parse_monitor_id(stored) else {
        return primary();
    };
    monitors
        .iter()
        .find(|monitor| monitor.device == device && monitor.desktop == desktop)
        .or_else(|| monitors.iter().find(|monitor| monitor.device == device))
        .or_else(|| monitors.iter().find(|monitor| monitor.desktop == desktop))
        .or_else(primary)
}

/// The output size a stored resolution gives on a monitor: the stored size when the monitor supports it, otherwise
/// (and for [`DESKTOP`]) the monitor's desktop mode.
pub fn resolve_resolution(monitor: &Monitor, stored: &str) -> Size {
    match Size::parse(stored) {
        Some(size) if monitor.modes.contains(&size) => size,
        _ => monitor.desktop,
    }
}

/// The desktop size a stored monitor identifier recorded when it was saved (`\\.\DISPLAY2@3440x1440` gives 3440x1440).
/// `None` for "primary" and for anything else without a size.
pub fn desktop_at_save(stored_monitor: &str) -> Option<Size> {
    parse_monitor_id(stored_monitor).map(|(_, size)| size)
}

/// The mode a stored explicit resolution asked for when this monitor no longer has it: `Some(desktop)` then (the
/// launcher logs it and plays at the desktop mode). `None` for "desktop", a mode the monitor supports, or no size.
pub fn resolution_fallback(monitor: &Monitor, stored: &str) -> Option<Size> {
    match Size::parse(stored) {
        Some(size) if !monitor.modes.contains(&size) => Some(monitor.desktop),
        _ => None,
    }
}

/// What the game is started with: the monitor's device name and the output size.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DisplayTarget {
    pub device: String,
    pub output: Size,
}

/// The target for the stored Monitor and Resolution values; `None` when no monitor is known (the game then opens on
/// the primary monitor at its desktop size, as before).
pub fn resolve_target(monitors: &[Monitor], stored_monitor: &str, stored_resolution: &str) -> Option<DisplayTarget> {
    let monitor = resolve_monitor(monitors, stored_monitor)?;
    Some(DisplayTarget { device: monitor.device.clone(), output: resolve_resolution(monitor, stored_resolution) })
}

/// The output size used when no monitor is known (tests, a platform without monitors): 1920 x 1080, which gives 3x,
/// the table default the 1.0.0 launcher always wrote.
pub const FALLBACK_OUTPUT: Size = Size::new(1920, 1080);

#[cfg(test)]
mod tests {
    use super::*;

    fn plan(width: u32, height: u32, scale: u32) -> DisplayPlan {
        display_plan(width, height, scale, PCSX2_GS_CAPS).unwrap()
    }

    fn plan_fractional(width: u32, height: u32, scale: u32) -> DisplayPlan {
        display_plan(width, height, scale, PCSX2_GS_FRACTIONAL_CAPS).unwrap()
    }

    #[test]
    fn at_100_percent_the_internal_frame_covers_the_screen() {
        for (width, height) in [(1280, 720), (1920, 1080), (2560, 1440)] {
            for caps in [PCSX2_GS_CAPS, PCSX2_GS_FRACTIONAL_CAPS, CORE_GS_CAPS] {
                let result = display_plan(width, height, 100, caps).unwrap();
                assert_eq!(result.multiplier, result.base_multiplier as f64, "{width}x{height}");
                assert!(result.internal_height >= height, "{width}x{height}: {} tall", result.internal_height);
                assert!(result.internal_width >= width, "{width}x{height}: {} wide", result.internal_width);
            }
        }
        assert_eq!(plan(1280, 720, 100).multiplier, 2.0);
        assert_eq!(plan(1920, 1080, 100).multiplier, 3.0);
        assert_eq!(plan(2560, 1440, 100).multiplier, 4.0);
    }

    #[test]
    fn the_percentage_scales_the_automatic_multiplier() {
        let full = plan_fractional(2560, 1440, 100);
        assert_eq!(full.base_multiplier, 4);
        assert_eq!(full.multiplier, 4.0);
        assert_eq!(plan_fractional(2560, 1440, 50).multiplier, 2.0);
        // The owner's example: 62.5% of 4x is 2.5x. The stored percentage is whole, so 62% and 63% both land on 2.5x;
        // the formula itself is tested at the exact 62.5%.
        assert_eq!(snap_multiplier(4.0 * 62.5 / 100.0, PCSX2_GS_FRACTIONAL_CAPS), 2.5);
        assert_eq!(plan_fractional(2560, 1440, 62).multiplier, 2.5);
        assert_eq!(plan_fractional(2560, 1440, 63).multiplier, 2.5);
        // Fractional values are GSdx (ARMSX2) only; CoreGS 2.0 keeps whole numbers.
        assert_eq!(plan(2560, 1440, 62).multiplier, 2.0, "CoreGS: 2.48 snaps to 2");
        assert_eq!(snap_multiplier(2.5, CORE_GS_CAPS), 3.0, "2.5 is an exact tie of 2 and 3, ties up");
        // At 1080p k is 3 and 67% keeps the old 2x default; at 720p k is 2, so 2x is 100%.
        assert_eq!(plan(1920, 1080, 67).multiplier, 2.0);
        assert_eq!(plan(1280, 720, 100).multiplier, 2.0);
    }

    #[test]
    fn the_labels_are_the_16_9_picture_of_the_same_width() {
        assert_eq!(label_size(1.0), (640, 360));
        assert_eq!(label_size(1.25), (800, 450));
        assert_eq!(label_size(1.5), (960, 540));
        assert_eq!(label_size(1.75), (1120, 630));
        assert_eq!(label_size(2.0), (1280, 720));
        assert_eq!(label_size(2.5), (1600, 900));
        assert_eq!(label_size(3.0), (1920, 1080));
        assert_eq!(label_size(4.0), (2560, 1440));
        assert_eq!(label_size(5.0), (3200, 1800));
        assert_eq!(label_size(12.0), (7680, 4320));
        assert_eq!(describe_plan(&plan(1920, 1080, 100)), "3x \u{00B7} 1920x1080");
    }

    #[test]
    fn the_row_offers_one_position_per_multiplier() {
        assert_eq!(
            render_scale_choices(Size::new(1280, 720), PCSX2_GS_CAPS),
            vec![(50, 1.0), (100, 2.0), (150, 3.0), (200, 4.0)]
        );
        assert_eq!(
            render_scale_choices(Size::new(1920, 1080), PCSX2_GS_CAPS),
            vec![(33, 1.0), (67, 2.0), (100, 3.0), (133, 4.0), (167, 5.0), (200, 6.0)]
        );
        let choices = render_scale_choices(Size::new(2560, 1440), PCSX2_GS_FRACTIONAL_CAPS);
        for (percent, multiplier) in [(25, 1.0), (38, 1.5), (50, 2.0), (63, 2.5), (75, 3.0), (100, 4.0), (125, 5.0), (200, 8.0)] {
            assert!(choices.contains(&(percent, multiplier)), "{percent}% = {multiplier}x in {choices:?}");
        }
        for (percent, multiplier) in &choices {
            assert_eq!(plan_fractional(2560, 1440, *percent).multiplier, *multiplier, "{percent}%");
        }
    }

    #[test]
    fn accepted_multipliers_are_ordered_and_fractional_only_when_allowed() {
        assert_eq!(accepted_multipliers(PCSX2_GS_CAPS), (1..=12).map(f64::from).collect::<Vec<_>>());
        let fractional = accepted_multipliers(PCSX2_GS_FRACTIONAL_CAPS);
        for value in [1.25, 1.5, 1.75, 2.5] {
            assert!(fractional.contains(&value), "{value}");
        }
        assert!(fractional.windows(2).all(|pair| pair[0] < pair[1]), "ascending");
        assert_eq!(snap_multiplier(2.25, PCSX2_GS_FRACTIONAL_CAPS), 2.5, "ties up");
        assert_eq!(snap_multiplier(2.25, CORE_GS_CAPS), 2.0, "whole values: 2.25 is nearer 2");
        assert_eq!(snap_multiplier(0.2, PCSX2_GS_CAPS), 1.0);
        assert_eq!(snap_multiplier(99.0, PCSX2_GS_CAPS), 12.0);
    }

    #[test]
    fn the_migration_keeps_the_multiplier_a_stored_percentage_gave() {
        // The old model was the frame height over the screen's height (TASK-180 test3). The phone's 100% at 720p was 2x,
        // 83% at 1080p was 2x, 62% at 1440p was 2x and 31% at 1440p was 1x.
        for (percent, output, old_multiplier) in [
            (100, Size::new(1280, 720), 2),
            (83, Size::new(1920, 1080), 2),
            (62, Size::new(2560, 1440), 2),
            (31, Size::new(2560, 1440), 1),
            (41, Size::new(1920, 1080), 1),
        ] {
            assert_eq!(old_multiplier_for_percent(percent, output.height.min(output.width)), old_multiplier, "{output:?} {percent}%");
            let stored = percentage_for_multiplier(old_multiplier as f64, output, PCSX2_GS_CAPS);
            assert_eq!(
                display_plan(output.width, output.height, stored, PCSX2_GS_CAPS).unwrap().multiplier,
                old_multiplier as f64,
                "{output:?}: the old {percent}% ({old_multiplier}x) is stored as {stored}%"
            );
        }
        assert_eq!(percentage_for_multiplier(2.0, Size::new(1280, 720), PCSX2_GS_CAPS), 100);
        assert_eq!(percentage_for_multiplier(2.0, Size::new(1920, 1080), PCSX2_GS_CAPS), 67);
        assert_eq!(percentage_for_multiplier(2.0, Size::new(2560, 1440), PCSX2_GS_CAPS), 50);
    }

    #[test]
    fn the_default_is_the_percentage_that_keeps_two_x() {
        assert_eq!(default_percentage(Size::new(1280, 720), PCSX2_GS_CAPS), 100);
        assert_eq!(default_percentage(Size::new(1920, 1080), PCSX2_GS_CAPS), 67);
        assert_eq!(default_percentage(Size::new(2560, 1440), PCSX2_GS_CAPS), 50);
        for (width, height) in [(1280, 720), (1920, 1080), (2560, 1440)] {
            let percent = default_percentage(Size::new(width, height), PCSX2_GS_CAPS);
            assert_eq!(plan(width, height, percent).multiplier, 2.0, "{width}x{height}");
        }
    }

    #[test]
    fn a_notch_value_keeps_the_multiplier_it_meant() {
        // Before TASK-180 test3, one 25% notch was one multiplier from the screen's own k (both axes).
        assert_eq!(notch_multiplier(100, Size::new(1920, 1080), false, PCSX2_GS_CAPS), 3);
        assert_eq!(notch_multiplier(125, Size::new(3088, 1440), false, PCSX2_GS_CAPS), 6);
        assert_eq!(notch_multiplier(0, Size::new(1920, 1080), false, PCSX2_GS_CAPS), 1);
        assert_eq!(notch_multiplier(100, Size::new(1280, 800), true, PCSX2_GS_CAPS), 1);
        assert_eq!(percentage_for_multiplier(6.0, Size::new(3088, 1440), PCSX2_GS_CAPS), 120);
        assert_eq!(display_plan(3088, 1440, 120, PCSX2_GS_CAPS).unwrap().multiplier, 6.0);
    }

    #[test]
    fn the_owner_k_table() {
        for (width, height, k) in [
            (1280, 720, 2),
            (1920, 1080, 3),
            (1920, 1200, 3),
            (1680, 1050, 3),
            (2560, 1080, 4),
            (2560, 1440, 4),
            (3440, 1440, 6),
            (3088, 1440, 5),
            (3840, 2160, 6),
        ] {
            let result = plan(width, height, 100);
            assert_eq!(result.base_multiplier, k, "{width}x{height}");
            assert_eq!(result.multiplier, k as f64, "{width}x{height} at 100%");
            assert!(result.internal_height >= height, "{width}x{height}: never below the screen's height");
        }
        for (width, height) in [(1280, 720), (1920, 1080), (2560, 1440), (3840, 2160)] {
            let result = plan(width, height, 100);
            assert!(result.internal_width >= width, "{width}x{height} is covered on both axes");
        }
    }

    #[test]
    fn aspect_and_the_ultrawide_path() {
        assert!(!plan(1920, 1080, 100).ultrawide_on, "16:9: nothing changes");
        assert!(!plan(1366, 768, 100).ultrawide_on, "1366x768 snaps to 16:9 as in the game");
        assert!(!plan(3840, 2160, 100).ultrawide_on);
        for (width, height) in [(2560, 1080), (3440, 1440), (3088, 1440), (1920, 1200), (1680, 1050), (1280, 1024)] {
            assert!(plan(width, height, 100).ultrawide_on, "{width}x{height}");
        }
        assert!((plan(3440, 1440, 100).aspect - 3440.0 / 1440.0).abs() < 1e-12);
    }

    #[test]
    fn a_phone_screen_and_portrait_input_are_safe() {
        let phone = plan(3088, 1440, 100);
        assert_eq!((phone.base_multiplier, phone.internal_width, phone.internal_height), (5, 3200, 2240));
        assert!(phone.ultrawide_on && !phone.portrait_input);
        let portrait = plan(1440, 3088, 100);
        assert!(portrait.portrait_input);
        assert_eq!((portrait.output_width, portrait.output_height), (3088, 1440));
        assert_eq!(portrait.multiplier, phone.multiplier);
        assert_eq!(portrait.aspect, phone.aspect);
        // A 16:9 720p screen: k = 2, so 2x is 100%. The S23's 19.3:9 panel at 720p is wider: k = 3.
        assert_eq!(plan(1280, 720, 100).multiplier, 2.0);
        let hd = plan(1544, 720, 100);
        assert_eq!((hd.base_multiplier, hd.multiplier), (3, 3.0));
        assert!(hd.internal_width >= 1544 && hd.internal_height >= 720);
        let square = plan(1080, 1080, 100);
        assert_eq!(square.base_multiplier, 3);
        assert_eq!(square.multiplier, 3.0);
        assert!(square.ultrawide_on);
    }

    #[test]
    fn invalid_inputs_are_refused() {
        assert!(display_plan(0, 1080, 100, PCSX2_GS_CAPS).is_err());
        assert!(display_plan(1920, 0, 100, PCSX2_GS_CAPS).is_err());
        assert!(display_plan(100, 100, 100, PCSX2_GS_CAPS).is_err(), "below 320");
        assert!(display_plan(20000, 2000, 100, PCSX2_GS_CAPS).is_err(), "above 16384");
        assert!(display_plan(16000, 1000, 100, PCSX2_GS_CAPS).is_err(), "ratio 16:1");
        for scale in [0, 9, 201, 5000] {
            assert!(display_plan(1920, 1080, scale, PCSX2_GS_CAPS).is_err(), "{scale}%");
        }
        assert!(display_plan(1920, 1080, 10, PCSX2_GS_CAPS).is_ok());
        assert!(display_plan(1920, 1080, 200, PCSX2_GS_CAPS).is_ok());
        assert!(is_render_scale(10) && is_render_scale(62) && is_render_scale(200));
        assert!(!is_render_scale(9) && !is_render_scale(201));
    }

    fn monitor(device: &str, name: &str, primary: bool, desktop: Size) -> Monitor {
        let raw = [Size::new(800, 600), Size::new(1280, 720), Size::new(1920, 1080), desktop, Size::new(320, 200)];
        Monitor { device: device.into(), name: name.into(), primary, desktop, modes: tidy_modes(&raw, desktop) }
    }

    #[test]
    fn modes_are_unique_sorted_and_without_tiny_sizes() {
        let wide = monitor(r"\\.\DISPLAY2", "DELL U3419W", false, Size::new(3440, 1440));
        assert_eq!(
            wide.modes,
            vec![Size::new(3440, 1440), Size::new(1920, 1080), Size::new(1280, 720), Size::new(800, 600)]
        );
        assert_eq!(monitor_base_name(&wide), "DELL U3419W");
    }

    #[test]
    fn labels_are_the_friendly_name_with_a_suffix_only_for_duplicates() {
        let wide = monitor(r"\\.\DISPLAY2", "DELL U3419W", false, Size::new(3440, 1440));
        let unnamed = monitor(r"\\.\DISPLAY1", "", true, Size::new(1920, 1080));
        assert_eq!(monitor_labels(&[wide.clone(), unnamed.clone()]), vec!["DELL U3419W".to_string(), "DISPLAY1".to_string()]);
        let twin = monitor(r"\\.\DISPLAY3", "DELL U3419W", false, Size::new(2560, 1080));
        let third = monitor(r"\\.\DISPLAY4", "DELL U3419W", false, Size::new(1920, 1080));
        assert_eq!(
            monitor_labels(&[wide, twin, unnamed, third]),
            vec!["DELL U3419W", "DELL U3419W 2", "DISPLAY1", "DELL U3419W 3"]
        );
    }

    #[test]
    fn a_stored_mode_the_monitor_lost_falls_back_to_the_desktop() {
        let wide = monitor(r"\\.\DISPLAY2", "DELL", false, Size::new(3440, 1440));
        assert_eq!(resolution_fallback(&wide, "1920x1080"), None, "a mode the monitor has");
        assert_eq!(resolution_fallback(&wide, "5120x2160"), Some(Size::new(3440, 1440)), "a lost mode");
        assert_eq!(resolution_fallback(&wide, DESKTOP), None, "desktop is never a fallback");
        assert_eq!(desktop_at_save(r"\\.\DISPLAY2@3440x1440"), Some(Size::new(3440, 1440)));
        assert_eq!(desktop_at_save(PRIMARY), None);
    }

    #[test]
    fn monitors_resolve_by_identifier_device_size_then_primary() {
        let monitors = vec![
            monitor(r"\\.\DISPLAY1", "TV", true, Size::new(1920, 1080)),
            monitor(r"\\.\DISPLAY2", "DELL", false, Size::new(3440, 1440)),
        ];
        let id = monitor_id(&monitors[1]);
        assert_eq!(id, r"\\.\DISPLAY2@3440x1440");
        assert_eq!(parse_monitor_id(&id), Some((r"\\.\DISPLAY2", Size::new(3440, 1440))));
        assert_eq!(resolve_monitor(&monitors, PRIMARY).unwrap().device, r"\\.\DISPLAY1");
        assert_eq!(resolve_monitor(&monitors, &id).unwrap().device, r"\\.\DISPLAY2");
        // Desktop size changed since it was stored: the same device still wins.
        assert_eq!(resolve_monitor(&monitors, r"\\.\DISPLAY2@2560x1080").unwrap().device, r"\\.\DISPLAY2");
        // Device numbers moved (screens re-plugged): the one with the stored desktop size.
        assert_eq!(resolve_monitor(&monitors, r"\\.\DISPLAY7@3440x1440").unwrap().device, r"\\.\DISPLAY2");
        // Gone entirely: the primary.
        assert_eq!(resolve_monitor(&monitors, r"\\.\DISPLAY7@5120x1440").unwrap().device, r"\\.\DISPLAY1");
        assert_eq!(resolve_monitor(&monitors, "garbage").unwrap().device, r"\\.\DISPLAY1");
        assert!(resolve_monitor(&[], PRIMARY).is_none());
    }

    #[test]
    fn resolutions_resolve_to_a_supported_mode_or_the_desktop() {
        let wide = monitor(r"\\.\DISPLAY2", "DELL", false, Size::new(3440, 1440));
        assert_eq!(resolve_resolution(&wide, DESKTOP), Size::new(3440, 1440));
        assert_eq!(resolve_resolution(&wide, "1920x1080"), Size::new(1920, 1080));
        assert_eq!(resolve_resolution(&wide, "2560x1600"), Size::new(3440, 1440), "not a mode of this monitor");
        let target = resolve_target(std::slice::from_ref(&wide), PRIMARY, "1280x720").unwrap();
        assert_eq!(target, DisplayTarget { device: r"\\.\DISPLAY2".into(), output: Size::new(1280, 720) });
        assert!(resolve_target(&[], PRIMARY, DESKTOP).is_none());
    }

    #[test]
    fn sizes_parse_strictly() {
        assert_eq!(Size::parse("2560x1440"), Some(Size::new(2560, 1440)));
        for bad in ["", "2560", "2560x", "x1440", "2560X1440", "-1x5", "2560x1440x1", " 2560x1440", "0x0", "99999x1"] {
            assert_eq!(Size::parse(bad), None, "{bad:?}");
        }
    }

}

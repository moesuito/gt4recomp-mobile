//! TASK-180: what the launcher offers on each platform, in one place (owner, 2026-11-05: platform rules belong to the
//! settings model, not to checks spread over the code).
//!
//! - **Windows:** everything, as before.
//! - **Android:** CoreGS 2.0 is the only renderer (GSdx is a Windows program), so the Renderer row and every GSdx-only
//!   row disappear; the screen is the phone's (no monitor, resolution or fullscreen rows; the game always fills it);
//!   the system's graphics driver is the only one (no card choice); no Discord; the update check only shows a newer
//!   version and opens its download page (no installer: the player installs the APK by hand).

use crate::releases::{Release, Stream};
use crate::settings::{ARMSX2_GS, CORE_GS_20, PCSX2_GS};
use crate::version::ReleaseVersion;

/// The platform the launcher runs on.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Platform {
    Windows,
    Android,
}

/// Rows the Android launcher does not offer, with why.
const ANDROID_ROWS_NOT_OFFERED: &[(&str, &str)] = &[
    ("graphics_card", "the phone's own graphics driver is the only one"),
    ("monitor", "the game always fills the phone's screen"),
    ("resolution", "the game always fills the phone's screen"),
    ("fullscreen", "the game always fills the phone's screen"),
    ("discord_rich_presence", "Discord has no game status on Android"),
    ("launcher_gpu", "the launcher's own graphics switch guards WebView2, a Windows component"),
];

/// Launcher rows that are not game settings (`SETTINGS`) but are drawn on the Settings page too.
const LAUNCHER_ONLY_ROWS: &[&str] = &["launcher_gpu"];

/// Rows that exist only on one platform. `armsx2_scale` is the private Android test build's ARMSX2 GS multiplier; the
/// renderer itself is offered on both platforms (GSdx and CoreGS 2.0 on Windows, CoreGS 2.0 and ARMSX2 on Android).
const ANDROID_ONLY_ROWS: &[&str] = &[
    "armsx2_scale",
    "armsx2_api",
    "armsx2_fxaa",
    "armsx2_cas",
    "armsx2_cas_strength",
    "armsx2_filter",
    "armsx2_aniso",
    "armsx2_dither",
];

impl Platform {
    /// The platform of this build.
    pub const fn current() -> Platform {
        if cfg!(target_os = "android") {
            Platform::Android
        } else {
            Platform::Windows
        }
    }

    /// The name the page reads (`"windows"`, `"android"`).
    pub const fn name(self) -> &'static str {
        match self {
            Platform::Windows => "windows",
            Platform::Android => "android",
        }
    }

    /// GSdx (the PCSX2 GS in `gt4-gs.exe`): downloads, its settings and the renderer choice exist only on Windows.
    pub const fn has_gsdx(self) -> bool {
        matches!(self, Platform::Windows)
    }

    /// Whether the Render scale slider starts at the step that gives 1x on the screen (Android: the phone's own screen is
    /// the output, 100% is its native size, and the owner wants 720p-class output too). Windows keeps its 50% floor.
    pub const fn scale_floor_follows_screen(self) -> bool {
        matches!(self, Platform::Android)
    }

    /// How many game sessions' logs Export logs carries. Android (owner, test2): 10, because each F8 report lives in its
    /// session's game.log and a benchmark evening is many sessions; Windows keeps its 3.
    pub const fn game_sessions_exported(self) -> usize {
        match self {
            Platform::Windows => crate::log_export::KEPT_FOR_EXPORT,
            Platform::Android => 10,
        }
    }

    /// Whether the launcher offers the Wheel page (TASK-181). Android has no wheel support: pads only (owner, test3), so
    /// the page, its tab and its commands are not offered there. Windows is unchanged.
    pub const fn offers_wheel_page(self) -> bool {
        matches!(self, Platform::Windows)
    }

    /// Whether the launcher downloads and installs its own updates. Android only checks and opens the download page.
    pub const fn installs_updates(self) -> bool {
        matches!(self, Platform::Windows)
    }

    /// Whether a launcher setting row is offered.
    pub fn offers(self, id: &str) -> bool {
        self.offers_platform_row(id)
            && match self {
                Platform::Windows => true,
                Platform::Android => !ANDROID_ROWS_NOT_OFFERED.iter().any(|(row, _)| *row == id),
            }
    }

    /// Whether the row exists on this platform at all (a row of the other platform is not shown and not sent).
    fn offers_platform_row(self, id: &str) -> bool {
        match self {
            Platform::Windows => !ANDROID_ONLY_ROWS.contains(&id),
            Platform::Android => true,
        }
    }

    /// Why a row is not offered here (for the log), or `None` when it is.
    pub fn why_not_offered(self, id: &str) -> Option<&'static str> {
        match self {
            Platform::Windows => None,
            Platform::Android => ANDROID_ROWS_NOT_OFFERED.iter().find(|(row, _)| *row == id).map(|(_, why)| *why),
        }
    }

    /// The value a setting must have on this platform whatever was stored (the renderer on Android).
    pub fn forced_value(self, id: &str) -> Option<&'static str> {
        match (self, id) {
            (Platform::Android, "renderer") => Some(CORE_GS_20),
            _ => None,
        }
    }

    /// Whether a renderer value can be chosen here.
    pub fn allows_renderer(self, value: &str) -> bool {
        match self {
            Platform::Windows => value == CORE_GS_20 || value == PCSX2_GS,
            // Android: CoreGS 2.0 (default) and, in the private test build, ARMSX2's GS in process. Never GSdx.
            Platform::Android => value == CORE_GS_20 || value == ARMSX2_GS,
        }
    }

    /// The value a setting takes on this platform for a *stored* value: only a value this platform cannot use is
    /// replaced (Android's CoreGS 2.0 floor for an old GSdx choice), so a chosen ARMSX2 GS is kept.
    pub fn forced_value_for(self, id: &str, stored: &str) -> Option<&'static str> {
        match (self, id) {
            (Platform::Android, "renderer") if !self.allows_renderer(stored) => Some(CORE_GS_20),
            _ => None,
        }
    }

    /// The launcher's download in a release: the setup program or the portable zip on Windows, the APK on Android
    /// (`GT4Recomp-1.3.0-rc16-android.apk`).
    pub fn launcher_package_name(self, version: ReleaseVersion, portable: bool) -> String {
        match self {
            Platform::Windows => Stream::Launcher.package_name_for(version, portable),
            Platform::Android => format!("GT4Recomp-{}-android.apk", version.text()),
        }
    }

    /// Whether a launcher release is one this platform can take. On Windows every release (as before); on Android only
    /// a release that ships the APK, so a Windows-only release is never offered to a phone.
    pub fn takes_release(self, release: &Release) -> bool {
        match self {
            Platform::Windows => true,
            Platform::Android => release
                .release_version
                .is_some_and(|version| release.asset(&self.launcher_package_name(version, false)).is_some()),
        }
    }

    /// Android: the address the browser opens to download a release's APK (the player installs it by hand).
    pub fn launcher_download_url(self, release: &Release) -> Option<String> {
        let version = release.release_version?;
        release.asset(&self.launcher_package_name(version, false)).map(|asset| asset.url.clone())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn windows_offers_every_row_and_forces_nothing() {
        for definition in crate::SETTINGS {
            // The ARMSX2 GS rows are the private Android test build's; Windows has no such renderer.
            if definition.id.starts_with("armsx2_") {
                assert!(!Platform::Windows.offers(definition.id));
                continue;
            }
            assert!(Platform::Windows.offers(definition.id), "{}", definition.id);
            assert_eq!(Platform::Windows.forced_value(definition.id), None);
        }
        assert!(Platform::Windows.has_gsdx());
        assert!(Platform::Windows.installs_updates());
        assert!(Platform::Windows.allows_renderer(PCSX2_GS));
    }

    #[test]
    fn android_has_coregs20_and_the_private_armsx2_renderer_but_no_installer() {
        let android = Platform::Android;
        // Private test build: the Renderer row exists on Android with CoreGS 2.0 (default) and ARMSX2's GS in process;
        // GSdx is never offered there. A stored GSdx choice still falls back to CoreGS 2.0, a chosen ARMSX2 stays.
        assert!(android.offers("renderer"));
        assert_eq!(android.forced_value("renderer"), Some(CORE_GS_20));
        assert_eq!(android.forced_value_for("renderer", CORE_GS_20), None);
        assert_eq!(android.forced_value_for("renderer", ARMSX2_GS), None);
        assert_eq!(android.forced_value_for("renderer", PCSX2_GS), Some(CORE_GS_20));
        assert!(!android.has_gsdx());
        assert!(!android.allows_renderer(PCSX2_GS));
        assert!(android.allows_renderer(ARMSX2_GS));
        assert!(!Platform::Windows.allows_renderer(ARMSX2_GS));
        assert!(android.offers("armsx2_scale") && !Platform::Windows.offers("armsx2_scale"));
        assert!(android.allows_renderer(CORE_GS_20));
        assert!(!android.installs_updates());
        assert!(android.scale_floor_follows_screen() && !Platform::Windows.scale_floor_follows_screen());
        assert_eq!(android.game_sessions_exported(), 10);
        assert_eq!(Platform::Windows.game_sessions_exported(), crate::log_export::KEPT_FOR_EXPORT);
        // TASK-180 test3 (owner): wheels are Windows only; Android offers pads only, so no Wheel page.
        assert!(!android.offers_wheel_page());
        assert!(Platform::Windows.offers_wheel_page());
        assert!(android.offers("performance_hud"), "the HUD draws on Android since test2");
        for row in ["graphics_card", "monitor", "resolution", "fullscreen", "discord_rich_presence"] {
            assert!(!android.offers(row), "{row}");
            assert!(android.why_not_offered(row).is_some(), "{row}");
        }
        for row in ["language", "update_channel", "render_scale", "frame_rate", "trigger_full_range", "controller", "vsync"] {
            assert!(android.offers(row), "{row}");
        }
    }

    #[test]
    fn every_android_rule_names_a_real_row() {
        for (row, _) in ANDROID_ROWS_NOT_OFFERED {
            let known = crate::SETTINGS.iter().any(|definition| definition.id == *row) || LAUNCHER_ONLY_ROWS.contains(row);
            assert!(known, "{row} is not a setting");
        }
    }

    fn release(tag: &str, assets: &[&str]) -> Release {
        Release {
            tag: tag.to_string(),
            version: Stream::Launcher.parse_tag(tag),
            release_version: Stream::Launcher.parse_release_tag(tag),
            published_at: Some("2026-11-05T09:00:00Z".to_string()),
            prerelease: tag.contains("-rc"),
            draft: false,
            notes: String::new(),
            assets: assets
                .iter()
                .map(|name| crate::releases::Asset { name: name.to_string(), url: format!("https://example.invalid/{name}") })
                .collect(),
        }
    }

    #[test]
    fn android_takes_only_releases_with_the_apk() {
        let windows_only = release("v1.3.0-rc16", &["GT4Recomp-Setup-1.3.0-rc16.exe", "SHA256SUMS"]);
        let with_apk = release("v1.3.0-rc17", &["GT4Recomp-Setup-1.3.0-rc17.exe", "GT4Recomp-1.3.0-rc17-android.apk"]);
        assert!(Platform::Windows.takes_release(&windows_only));
        assert!(!Platform::Android.takes_release(&windows_only));
        assert!(Platform::Android.takes_release(&with_apk));
        assert_eq!(
            Platform::Android.launcher_download_url(&with_apk).as_deref(),
            Some("https://example.invalid/GT4Recomp-1.3.0-rc17-android.apk")
        );
        assert_eq!(Platform::Android.launcher_download_url(&windows_only), None);
        let version = ReleaseVersion::parse("1.3.0-rc17").unwrap();
        assert_eq!(Platform::Windows.launcher_package_name(version, false), "GT4Recomp-Setup-1.3.0-rc17.exe");
        assert_eq!(Platform::Windows.launcher_package_name(version, true), "GT4Recomp-1.3.0-rc17-portable.zip");
    }

    #[test]
    fn this_build_is_windows_or_android() {
        assert_eq!(Platform::current() == Platform::Android, cfg!(target_os = "android"));
    }
}

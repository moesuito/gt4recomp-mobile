//! Core of the GT4Recomp launcher: settings, config file, paths, launch plan, exit classification, release
//! lists, updates, `version.json`, schema migrations and the data-path guard. No UI here. The one
//! Windows-specific piece is the process-lock file, which uses std APIs only.

pub mod build_kind;
pub mod config;
pub mod controller;
pub mod display;
pub mod exit;
pub mod guard;
pub mod games;
pub mod games_layout;
pub mod gpu;
pub mod gs_release;
pub mod gs_table;
pub mod language;
pub mod launch;
pub mod lock;
pub mod log_export;
pub mod logs;
pub mod messages;
pub mod migrate;
pub mod paths;
pub mod platform;
pub mod portable_update;
pub mod releases;
pub mod relocate;
pub mod retroachievements;
pub mod secret;
pub mod settings;
pub mod shader_prepare;
pub mod system_check;
pub mod textures;
pub mod tombstone;
pub mod update;
pub mod version;
pub mod version_list;
pub mod versions;
pub mod virtual_pad;
pub mod wheel;
pub mod wheel_capture;
pub mod wheel_identity;
pub mod wheel_session;

pub use config::ConfigFile;
pub use platform::Platform;
pub use settings::{SettingDef, Settings, SETTINGS};
pub use version::{ReleaseVersion, Version};

/// Version shown in the UI, written to receipts and used for update decisions: the Cargo version, plus `-rcN` for a
/// test build (the label comes from the `GT4_RELEASE_LABEL` build variable, read by build.rs; none = a stable build).
/// The Cargo package version stays numeric because the Windows file version must be.
pub const LAUNCHER_VERSION: &str = env!("GT4_LAUNCHER_VERSION");
/// The release label of this build: `rc5`, or empty for a stable build.
pub const RELEASE_LABEL: &str = env!("GT4_RELEASE_LABEL");

/// The release version of this build (`1.3.0-rc5`).
pub fn launcher_release_version() -> Option<ReleaseVersion> {
    ReleaseVersion::parse(LAUNCHER_VERSION)
}

/// Whether a version string names a test build. Test builds are the release candidates (`1.3.0-rc5`) and the Android
/// test builds (`1.3.0-android-test4`); both carry the release-candidate settings (TASK-118: the performance report and
/// the VU code check) and neither may keep them on in a final build. A stable version (`1.3.0`) has no such rows.
pub fn is_test_build(version: &str) -> bool {
    let version = version.trim();
    version.contains("-rc") || version.contains("-android-test")
}

/// Whether the launcher running now is a test build (see [`is_test_build`]).
pub fn test_build() -> bool {
    is_test_build(LAUNCHER_VERSION)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_builds_are_release_candidates_and_android_tests() {
        assert!(is_test_build("1.3.0-rc5"), "a release candidate");
        assert!(is_test_build("1.3.0-rc15"), "a later release candidate");
        assert!(is_test_build("1.3.0-android-test4"), "an Android test build");
        assert!(is_test_build("1.3.0-android-test10"), "a later Android test build");
        assert!(!is_test_build("1.3.0"), "a final build ships no test rows");
        assert!(!is_test_build("1.2.5"), "an older final build");
        assert!(!is_test_build(""), "an empty version is not a test build");
        // The rule that broke test3: its label carries neither "-rc" nor a stable-only version.
        assert!(is_test_build("1.3.0-android-test3"), "test3 is a test build even though it has no -rc label");
    }
}

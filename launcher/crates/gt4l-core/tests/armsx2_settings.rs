//! The private ARMSX2 GS build's settings (owner, 2026-11-06): the picture options the launcher writes for the library,
//! and their defaults for the S23.
use gt4l_core::launch;
use gt4l_core::settings::Settings;

fn set(pairs: &[(&str, &str)]) -> Settings {
    let mut settings = Settings::default();
    for (id, value) in pairs {
        settings.set(id, value).unwrap_or_else(|error| panic!("{id}={value}: {error}"));
    }
    settings
}

#[test]
fn the_defaults_are_the_ones_the_private_build_ships() {
    let settings = Settings::default();
    assert_eq!(settings.get("armsx2_api"), "auto", "Auto leaves the API to the library, so its per-GPU rules stay in charge");
    assert_eq!(settings.get("armsx2_textures"), "on", "the pack is loaded unless the player turns it off");
    assert_eq!(settings.get("armsx2_fxaa"), "off");
    assert_eq!(settings.get("armsx2_cas"), "off");
    assert_eq!(settings.get("armsx2_cas_strength"), "50");
    assert_eq!(settings.get("armsx2_filter"), "bilinear-ps2");
    assert_eq!(settings.get("armsx2_aniso"), "off");
    assert_eq!(settings.get("armsx2_dither"), "unscaled");
}

#[test]
fn each_row_maps_to_the_armsx2_gs_setting_it_controls() {
    let defaults = launch::armsx2_effect_lines(&Settings::default()).join("\n");
    assert_eq!(
        defaults,
        "fxaa = false\nUpscaler = 0\nPrecacheTextureReplacements = true\nCASMode = 0\nCASSharpness = 50\nfilter = 2\nMaxAnisotropy = 0\ndithering_ps2 = 2"
    );

    let tuned = launch::armsx2_effect_lines(&set(&[
        ("armsx2_fxaa", "on"),
        ("armsx2_cas", "on"),
        ("armsx2_cas_strength", "70"),
        ("armsx2_filter", "nearest"),
        ("armsx2_aniso", "16"),
        ("armsx2_dither", "off"),
    ]))
    .join("\n");
    assert_eq!(
        tuned,
        "fxaa = true\nUpscaler = 0\nPrecacheTextureReplacements = true\nCASMode = 1\nCASSharpness = 70\nfilter = 0\nMaxAnisotropy = 16\ndithering_ps2 = 0"
    );

    let forced = launch::armsx2_effect_lines(&set(&[
        ("armsx2_filter", "bilinear-forced"),
        ("armsx2_aniso", "8"),
        ("armsx2_dither", "scaled"),
    ]))
    .join("\n");
    assert!(forced.contains("filter = 1") && forced.contains("MaxAnisotropy = 8") && forced.contains("dithering_ps2 = 1"));
}

/// TASK-186 round 6: the two GSdx player rows go into the ARMSX2 GS file with ARMSX2's own keys, and Auto writes the
/// library's Auto renderer instead of a backend the launcher forced.
#[test]
fn the_two_gsdx_rows_and_auto_renderer_reach_the_armsx2_gs_file() {
    let paths = launch::armsx2_paths(std::path::Path::new("C:/tmp/gt4recomp-round6"));
    let textures = std::path::Path::new("C:/tmp/gt4recomp-round6/textures");

    let defaults = launch::armsx2_config_text(&Settings::default(), &paths, textures, None).unwrap();
    assert!(defaults.contains("accurate_blending_unit = 3\n"), "Blending accuracy High by default");
    assert!(defaults.contains("HWDownloadMode = 0\n"), "Readbacks Accurate by default");
    assert!(defaults.contains("Renderer = -1\n"), "Auto lets the library choose");

    let mut gs = Settings::default();
    gs.set_gs("blending", "minimum").unwrap();
    gs.set_gs("hw_download_mode", "unsynchronized").unwrap();
    let tuned = launch::armsx2_config_text(&gs, &paths, textures, None).unwrap();
    assert!(tuned.contains("accurate_blending_unit = 0\n"), "Minimum reaches ARMSX2");
    assert!(tuned.contains("HWDownloadMode = 3\n"), "Unsynchronized reaches ARMSX2");

    let gles = launch::armsx2_config_text(&set(&[("armsx2_api", "ogl")]), &paths, textures, None).unwrap();
    assert!(gles.contains("Renderer = 12\n"), "OpenGL ES forced");
    let vulkan = launch::armsx2_config_text(&set(&[("armsx2_api", "vk")]), &paths, textures, None).unwrap();
    assert!(vulkan.contains("Renderer = 14\n"), "Vulkan forced");
}

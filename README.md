# GT4Recomp for Android (source only)

GT4Recomp for Android with **ARMSX2's GS** as an extra renderer beside **CoreGS 2.0**, for people who build it
themselves.

This repository is the source of the Android port of [GT4Recomp](https://github.com/moesuito/gt4-staticrecomp) with the
ARMSX2 GS embedded (GPL-3.0-or-later). It contains no game code and no game data: the build produces the game code from
your own disc.

> [!IMPORTANT]
> **No builds are distributed here.** The official, supported GT4Recomp release is the one in
> [moesuito/gt4-staticrecomp](https://github.com/moesuito/gt4-staticrecomp), with CoreGS 2.0 only. This variant gets no
> support, no release and no installer. Build it for yourself at your own risk.

## What you need

- **Your own copy of Gran Turismo 4 NTSC-U (SCUS-97328)**, plus a PS2 BIOS to extract the game's files. Nothing from
  the game is in this repository.
- Windows x86-64 for the native build, with the toolchain listed in [docs/BUILD.md](docs/BUILD.md)
  ("Qualified Environment"): VS2022 Build Tools, clang-cl, CMake 3.24+, Ninja, Rust/Cargo, Naga and the local Python
  environment.
- For the APK: Android SDK with NDK **26.1.10909125**, CMake, Ninja, a JDK 21, the Rust Android targets and the Tauri
  CLI, as listed in [android/README.md](android/README.md) ("Toolchain").
- The Android build also needs SDL3 3.4.18 for Android (the official AAR) for the launcher and game activities.

## Build, in short

The full, maintained recipes are in [docs/BUILD.md](docs/BUILD.md), [docs/EE-PRODUCER.md](docs/EE-PRODUCER.md),
[docs/VU1-MICROCODE-SOURCE.md](docs/VU1-MICROCODE-SOURCE.md), [android/README.md](android/README.md) and
[docs/ANDROID-PORT-PLAN.md](docs/ANDROID-PORT-PLAN.md). The order is:

1. **Prepare the game inputs** from your disc: the ISO, `CORE.GT4` and the reconstructed ELF, with the scripts and hash
   pins described in [docs/BUILD.md](docs/BUILD.md) ("Private Prerequisites") and [docs/EE-PRODUCER.md](docs/EE-PRODUCER.md).
2. **Translate the EE code** with `tools/ee_producer` into the 326 C++ units and the AOT library, then **build the VU1
   code** with `tools/vu_recomp` ([docs/VU1-MICROCODE-SOURCE.md](docs/VU1-MICROCODE-SOURCE.md)).
3. **Build the native Windows tree** with `native/build.cmd` (the CoreGS 2.0 shaders are reused as C++ by the Android
   build).
4. **Build `libgt4.so`** for arm64-v8a with `android/native/configure-android.cmd`, and stage it with
   `android/stage-native-libs.cmd` ([android/README.md](android/README.md)).
5. **Fetch the shader compiler sources** for the ARMSX2 Vulkan renderer:
   `python tools/fetch_armsx2_shader_deps.py` (pinned glslang, SPIRV-Headers and SPIRV-Tools).
6. **Build `libgt4gs.so`** from the tracked ARMSX2 import:
   `cmake -S third_party/armsx2/platforms/android/app/src/main/cpp -B build/armsx2-core ...` and
   `cmake --build build/armsx2-core --target gt4gs`, with `-DUSE_VULKAN=ON -DUSE_OPENGL=ON` and
   `-DGT4GS_FRONTEND_DIR=<this repository>/android/armsx2gs`. Stage it with `android/stage-armsx2gs.cmd`.
7. **Package and sign the APK** with the launcher's Gradle project
   (`cargo tauri android build --target aarch64 --apk --ci`) and **your own key**, through `GT4_ANDROID_SIGNING`.

### Known gaps (read before trying)

- The production native build links a **pinned private `ps2game.lib`** and prepared assets; the EE producer can rebuild
  them from your disc, but it does not yet replace the production library or automate a clean-checkout playable build.
  See the "Reproduction limit" in [docs/BUILD.md](docs/BUILD.md).
- The Android build expects the SDL3 3.4.18 Android AAR and the same pinned inputs as the PC build.
- The **ARMSX2 GS** renderer here is built with `GT4GS_NO_JIT`: it generates no code at run time. It is an extra
  renderer, not the supported default.

## Maintenance

- **GT4Recomp updates:** add this repository's `upstream` remote (the main repository), `git fetch upstream`, merge and
  resolve conflicts.
- **ARMSX2 updates:** re-import the pristine subtree from the new upstream commit into `third_party/armsx2/`, then
  re-apply the patches listed in [docs/ARMSX2-GS-IMPORT.md](docs/ARMSX2-GS-IMPORT.md). Re-run
  `tools/fetch_armsx2_shader_deps.py` if the new shaderc pins different revisions.

## Credits

GT4Recomp by João Alano and the GT4Recomp contributors. The GS renderer comes from **ARMSX2**, which derives from
**PCSX2**; both are GPL-3.0-or-later. Third-party libraries keep their own licences under `third_party/armsx2/`.
Gran Turismo 4 is a trademark of Sony Interactive Entertainment; this project is not affiliated with Sony.

## Licence

The combined work is **GPL-3.0-or-later**: see [LICENSE](LICENSE) and [NOTICE](NOTICE). Files taken from GT4Recomp keep
their MIT notices and are used here under the GPL as part of the combined work.

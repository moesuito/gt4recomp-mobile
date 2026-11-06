# GT4Recomp for Android: how the APK is built

TASK-180 (docs/ANDROID-PORT-PLAN.md). The Android app is the PC launcher (Tauri 2) as its main activity plus the game as
one native library, `libgt4.so`, run by a second activity (SDL3's `SDLActivity`) in its own `:game` process. The
launcher always opens first; Play starts the game with the same launch contract the Windows launcher gives
`gt4vulkan.exe` (arguments, and the game.ini values as environment). Nothing game-derived is committed: the translated
units, the capsules, the generated VU code, the built libraries and the APK stay under `build/` or `private/`.

Where things are:

| Piece | Where |
|---|---|
| The app (Gradle project, manifest, activities, Kotlin bridge) | `launcher/crates/gt4l-tauri/gen/android/` |
| The launcher's Android side (Rust) | `launcher/crates/gt4l-tauri/src/android.rs`, `gt4l_core::platform` (what Android offers) |
| The game library build | `android/native/` (`configure-android.cmd`, `CMakeLists.txt`) |
| Staging the game libraries into the app | `android/stage-native-libs.cmd` |
| The app icons (from the PC art) | `android/tools/make_icons.py` |
| Emulator procedure | `docs/ANDROID-EMULATOR.md` |

## Toolchain (exact versions)

| Piece | Version | Where |
|---|---|---|
| NDK | 26.1.10909125 | `%LOCALAPPDATA%\Android\Sdk\ndk\26.1.10909125` |
| CMake, Ninja | CMake 3.22+ (4.3.1 used), Ninja 1.13 | PATH |
| Android platform, build-tools | android-36, build-tools 36.1.0 | SDK |
| JDK | Eclipse Adoptium 21.0.11 | `C:\Program Files\Eclipse Adoptium\jdk-21.0.11.10-hotspot` |
| Gradle, Android Gradle Plugin, Kotlin | 8.14.3 (wrapper), AGP 8.11.0, Kotlin 1.9.25 (as `cargo tauri android init` writes them) | `gen/android` |
| Rust | the launcher's toolchain plus `aarch64-linux-android` (and `x86_64-linux-android` for the emulator) | rustup |
| Tauri CLI | `cargo-tauri` 2.x matching the launcher's `tauri` crate | `cargo install tauri-cli` |
| SDL3 | 3.4.18, `SDL3-devel-3.4.18-android.zip` (sha256 `e09e4d6593335d089ba124375e8b8abd0fd75173dffcd2c17012b45af121d4e9`, git `829a65d7`); its `SDL3-3.4.18.aar` sha256 `bea5cbd6ec56a247196d70c3626860e0d6f36163e562e20a33041bee2fb339f8` | `private/tooling/sdl3-3.4.18-android/` |
| Windows build (for the CoreGS 2.0 shaders) | any `GT4_HW_GS=ON` build of the same commit | e.g. `build/t180-v200` |

## Build (arm64-v8a for phones; `x86_64` for the emulator the same way)

From the repository root, in a normal `cmd` (the NDK brings its own clang):

```
rem 1. The translated v2.00 units (rc14b, hooked) and the CoreGS 2.0 shaders as C++ from the Windows build.
set GT4_EE_UNITS_DIR=C:/Antigravity/gt4-wt-180-android/build/android-inputs/v200-units-rc14b
set GT4_HW_SHADER_CPP_DIR=C:/Antigravity/gt4-wt-180-android/build/t180-v200/native/hw_gs/shaders
rem 2. libgt4.so (configure checks every capsule against docs/inputs and prints the units' combined SHA-256).
android\native\configure-android.cmd arm64-v8a build\android-arm64
rem 3. Stage libgt4.so (line tables stripped, symbol table kept), libSDL3.so, libc++_shared.so and the SDL3 AAR into the app.
android\stage-native-libs.cmd arm64-v8a build\android-arm64
rem 4. The launcher library and the APK, signed with the release key (kept out of git, see below).
set ANDROID_HOME=%LOCALAPPDATA%\Android\Sdk
set NDK_HOME=%LOCALAPPDATA%\Android\Sdk\ndk\26.1.10909125
set JAVA_HOME=C:\Program Files\Eclipse Adoptium\jdk-21.0.11.10-hotspot
set GT4_RELEASE_LABEL=rc14
set GT4_SDL3_ANDROID_ROOT=C:\Antigravity\gt4-staticrecomp\private\tooling\sdl3-3.4.18-android
set GT4_ANDROID_SIGNING=C:\Antigravity\gt4-staticrecomp\private\android-signing\gt4recomp-release.properties
cd launcher\crates\gt4l-tauri
cargo tauri android build --target aarch64 --apk --ci
```

Output: `launcher\crates\gt4l-tauri\gen\android\app\build\outputs\apk\universal\release\app-universal-release.apk`
(`--target x86_64` for the emulator; both targets in one call make one APK with both ABIs once both are staged).

- **Spec II needs a runtime capsule with `sif.cpp`** (TASK-180b). `libgt4spec2.so` is `GT4_TARGET=spec2`, and its
  `GT4_PERF_RUNTIME_SOURCE` must carry the TASK-100 S7b IOP reboot rule (`iop/sif.cpp`: DNAS310.IMG selects the
  asynchronous FILEIO server), e.g. `slice276-spec2-iop-runtime`. `android/native/CMakeLists.txt` verifies that file
  against the capsule pin and compiles it in place of the donor's `iop/sif.cpp`, exactly like the Windows build; a Spec II
  configure with a capsule that has no `sif.cpp` fails and names the file. Without it the game waits for ever on the
  `dev9x:` devctl after `INETCTL.IRX` and the picture stays black (TASK-180b).
- **Spec II sets `GT4_TARGET_SPEC2`** (TASK-180b). That definition selects the Spec II row of `native/game_target.hpp`, the
  guest addresses of every host hook (16:9 hold, 480p hold, display aspect, race camera, HUD canvas, license guard). The
  Android CMake now sets it for `GT4_TARGET=spec2` as the Windows build does; without it the Spec II library ran the
  v2.00 addresses, found no game-data object and stopped after the publisher screens.

- **Version:** `tauri.android.conf.json` sets the APK's `versionName` (`1.3.0-android-test1`) and `versionCode`. The
  launcher inside reports the PC version it is built from (`GT4_RELEASE_LABEL`), which also decides its update channel.
- **Signing:** `GT4_ANDROID_SIGNING` names a properties file with `storeFile`, `storePassword`, `keyAlias` and
  `keyPassword`. The owner's release key is in `private/android-signing/` of the main checkout (git-ignored). Without the
  variable the release APK is unsigned. Never commit the key or its password; every APK of the app must be signed with the
  same key, or Android refuses it as an update.
- **Symbols:** the full `libgt4.so` (with line tables) stays in the build folder and is copied beside each published APK
  (`symbols/`), so a tombstone from a phone can be symbolized with `llvm-symbolizer` by its Build ID.

## Logs (how Android is debugged)

| What | File in the app's data | logcat tag |
|---|---|---|
| Launcher (setup, disc pick, Play, updates, page) | `data/logs/latest.log` and the session logs | `GT4Recomp-launcher` |
| Shader prepare before Play (progress, attempts) | `data/logs/shaders.log` | `GT4Recomp-game` |
| Game (the same `[core2]`, `[perf]`, `[hw]`, `[dfp]` lines as on the PC) | `data/sessions/session-<time>/game.log` | `GT4Recomp-game` |
| How the game's process ended (exit status, crash, ANR, low memory) | `data/logs/game-exits/game-exits.log`; a native crash's tombstone as `.pb` and as text | `GT4Recomp-launcher` |

Over USB on a release (non-debuggable) build the same logs are mirrored into the app's external folder, readable
without root: `adb -s <serial> pull /sdcard/Android/data/io.github.moesuito.gt4recomp/files/` gives `logs/launcher.log`
(every launcher line) and `sessions/session-<time>/game.log` (each game session, written live). A `debug.env` pushed
there (`NAME=VALUE` lines, only `GT4_*` names are taken) adds variables to the next Play; each one is logged as
`debug.env: NAME=VALUE` in both logs.

Settings > About > Export logs saves one zip through the system's "save as" (latest.log, the newest launcher session
logs, the newest game logs, the game exits and crash texts, `settings.toml` and `system.txt` with the phone's model,
Android version, SoC, memory, display and refresh rate, Vulkan device and driver, and Vulkan features). Over USB:
`adb logcat -s GT4Recomp-launcher GT4Recomp-game`.

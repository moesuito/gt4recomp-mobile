# Private test build: ARMSX2's GS inside the GT4Recomp Android app (owner's phone only)

Status: 2026-11-05, **PRIVATE and PCSX2-tainted, GPL-3.0+**. Never distributed, never merged, never pushed. This clone
(`gt4-private-armsx2gs`, branch `private/armsx2-gs-s23`) has its push URL disabled on purpose.

## What it is

A second GS renderer for the Android app, beside CoreGS 2.0: the GS of ARMSX2 (a PCSX2-family emulator, commit
`1049db37fb`, 2026-11-04) running in the same process as the game, fed by the very stream the game already produces for
`gt4-gs.exe` on Windows. No IPC: the game's EE thread pushes the records of the GS stream protocol (GIF chunks per path,
privileged register writes, VSYNC with the field, local-to-host reads, resets, display aspect) into an in-process queue
that the ARMSX2 GS thread consumes in order.

| Piece | Where |
|---|---|
| The GS library (`libgt4gs.so`, GPL) | `android/armsx2gs/` (our frontend) built into a vendored ARMSX2 copy |
| The vendored ARMSX2 tree (git-ignored) | `private/armsx2-vendor/` (a copy of `C:\Antigravity\armsx2` without `.git`) |
| The game side (`GT4_RENDERER=armsx2`) | `native/gs_stream/armsx2_gs_sink.cpp` (+ the renderer value in `gs_stream_client`) |
| Settings and Play on Android | `gt4l_core::launch::armsx2_paths` / `armsx2_config_text`, `android.rs` |
| Build recipe | `build-android-game.cmd`, `build-hw-shaders.cmd`, `android/stage-armsx2gs.cmd` |

## How the core is built

The vendored copy is only there because ARMSX2's Android CMake entry point resolves its 3rdparty folders relative to
itself. `private/armsx2-vendor/platforms/android/app/src/main/cpp/CMakeLists.txt` replaces the emulator app's entry
point (kept as `CMakeLists-app-emucore.txt`) with one that builds the core and our frontend:

```
cmake -S private/armsx2-vendor/platforms/android/app/src/main/cpp -B build/armsx2-core -G Ninja
  -DCMAKE_TOOLCHAIN_FILE=<NDK>/build/cmake/android.toolchain.cmake
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static -DANDROID=true
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-O2 -DGT4GS_NO_PRESERVE_ALL"
  -DUSE_VULKAN=OFF -DUSE_OPENGL=ON -DENABLE_QT_UI=OFF -DENABLE_TESTS=OFF -DENABLE_GSRUNNER=OFF
  -DGT4GS_FRONTEND_DIR=<clone>/android/armsx2gs
cmake --build build/armsx2-core --target gt4gs -j 10
```

### Back end: OpenGL ES, not Vulkan

The Vulkan back end needs a runtime GLSL to SPIR-V compiler (shaderc), whose sources are the glslang and SPIRV-Tools
submodules; the local ARMSX2 clone does not have them and this task allows no downloads. The library is therefore built
with ARMSX2's own Android default, the OpenGL ES back end, and reports it. `gt4gs_has_vulkan()` returns 0 and the
launcher's ARMSX2 renderer runs OpenGL ES. The Vulkan path is a rebuild away the day those sources are present.

### Two small patches to the vendored copy (private build only)

- `EeFpuModel.h`: NDK 26.1 is clang 17, whose AArch64 backend refuses `__attribute__((preserve_all))`; the documented
  wide-spill fallback is used (`-DGT4GS_NO_PRESERVE_ALL`).
- `HeapArray.h`: libc++ 17 has no `std::lexicographical_compare_three_way`; the same comparison is written out.
- `GSDumpReplayer.cpp` / `VMManager.cpp`: the hooks the owner's fork frontend uses (`GT4GSStreamHook.h`) so the GS
  boots without a BIOS or a disc and the CPU provider is the record loop; the dump serial and CRC answer 0 for a stream.
- `GSDumpReplayer.cpp` / `GS/Renderers/Common/GSRenderer.cpp` (2026-11-06): the dump-replay overlay returns when there is
  no dump file, and the end-of-present path ends the present and returns while a stream runs (no overlay, fullscreen UI,
  OSD or ImGui frame). This is the first-VSync crash fix; see the section below.

## Settings

`armsx2-gs.ini` is written on every Play from the launcher's own GS table (`gs_config_text`, the same base settings a
`gt4-gs` session gets on Windows: blending accuracy, aspect 16:9, the GT4 display crop, no OSD, texture loading only) with
the resolution multiplier from the new **ARMSX2 GS resolution** row: 1x, 1.25x, 1.5x, 1.75x, 2x, 2.5x, 3x, 4x, 5x, 6x.
A fractional value adds `ExtendedUpscalingMultipliers = true` (PCSX2 ignores fractional multipliers without it).

The owner's own lines go into `armsx2-gs-extra.ini`, beside it (created once with an example, never rewritten): every
`[EmuCore/GS]` line there is applied on top of the base file, so a line he adds wins. Both files live in the app's
external files folder:

```
/sdcard/Android/data/io.github.moesuito.gt4recomp/files/armsx2gs/armsx2-gs.ini
/sdcard/Android/data/io.github.moesuito.gt4recomp/files/armsx2gs/armsx2-gs-extra.ini
```

`resources/` beside them holds `GameIndex.yaml` and the OpenGL ES shader sources, copied out of the APK assets by the
launcher on Play.

## Logging

The library writes through PCSX2's console to stderr, which the game already sends to the session's `game.log` (and to
logcat, tag `GT4Recomp-game`), with a `[gt4gs]` prefix for its own lines. `GT4_ARMSX2_GS_PERF=1` in `debug.env` adds the
GS thread and GPU times every 300 VSYNCs. The game's performance report and HUD are the game's own and are unaffected.

## Build result (2026-11-05)

- Version name `1.3.0-android-test4-armsx2gs-private`, versionCode 4, signed with the owner's release key.
- Output: `C:\Antigravity\gt4-staticrecomp\private\android-private-armsx2gs\`.
- Emulator: `adb -s emulator-5554`, Lavapipe (see below).

## Emulator result (2026-11-05, `adb -s emulator-5554`)

What works, in the emulator (`-gpu lavapipe`, ARM translation):

- the APK installs and updates over the test4 build (versionCode 4, same release key);
- the launcher starts; the Renderer row offers **CoreGS 2.0 (default)** and **ARMSX2 GS (experimental, private build)**,
  and **ARMSX2 GS resolution** appears with the fractional values while ARMSX2 is chosen;
- Play with ARMSX2 writes `armsx2-gs.ini` (with `upscale_multiplier = 1.5` and `ExtendedUpscalingMultipliers = true`)
  and `armsx2-gs-extra.ini`, starts the game with `GT4_RENDERER=armsx2`, and the game loads `libgt4gs.so` in its own
  process;
- the library boots the ARMSX2 core as a GS-only VM: the stream source is prepared, the GameDB applies GT4's GS fixes
  (`halfPixelOffset`, `autoFlush`, `getSkipCount`, `recommendedBlendingLevel`), the game's window is attached, EGL is
  loaded and an OpenGL ES context is created;
- CoreGS 2.0 still runs afterwards in the same APK (boot screen drawn at 2x, HUD shows "CoreGS 2.0").

What stops it in the emulator: **the emulator's guest GL is SwiftShader OpenGL ES 3.0**, and ARMSX2's GLES backend needs
ES 3.1 (its uber shader uses `uvec2`, GLSL ES 3.10). The log is explicit: the context is created for 3.0 only, then
`Shader failed to compile: ERROR: 0:32: 'uvec2' : syntax error` and `Failed to create GS device`. `-feature ForceANGLE`
does not change the guest GL version (still SwiftShader 3.0). The owner's S23 Ultra (Adreno 740, OpenGL ES 3.2 and
Vulkan 1.3) is above that floor; this build is for that phone.

The Vulkan back end would run on the emulator's Lavapipe, but it needs shaderc's runtime GLSL compiler and this clone has
no glslang/SPIRV-Tools sources (see above); that is the one thing to obtain for a Vulkan build.

## Owner's S23 report (2026-11-06): black screen, and the fix

The owner's log bundle (`private/owner-logs-20261106-013308`, session-1793928633) shows the whole start-up working on
Adreno 740 until one missing file:

```
[0.1971] Opening GS...
[0.1979] GLAD EGL Version: 1.5 ... eglCreateContext() succeeded for version 3.2
[0.2079] GL: Qualcomm Adreno GPU detected ... Adreno profile active (EXT/PLS framebuffer fetch)
[0.2107] GS cache: read 58 entries (127 KB) ... @@ANDROID_GL_INIT@@ stage=convert_present_merge_interlace_done
[0.2542] GL: Failed to read fxaa.fs
[0.2543] Failed to create GS device
```

**Cause.** The GS device creates its effects at start-up and one of them is FXAA, whose source is
`resources/shaders/common/fxaa.fx` (the message says "fxaa.fs"); the CAS/FSR helpers in the same folder
(`ffx_a.h`, `ffx_cas.h`, `ffx_a_fsr1.h`, `ffx_fsr1.h`) are read the same way. The staging script copied only
`resources/shaders/opengl/*.glsl`, so `shaders/common` never reached the phone. The device failed at creation, the
library refused, the game halted — and because the game's own message box is drawn by the renderer that just failed to
start, the owner saw a black screen with no text.

**Fix.** `android/stage-armsx2gs.cmd` now stages the whole `resources/shaders` tree (opengl, common, and the dx11 or
vulkan sources for completeness: 33 files, 3.6 MB) and the app extracts it to the external files folder on every Play.

**GLES on Adreno was not the problem.** The context was created at ES 3.2, the Adreno profile with EXT/PLS framebuffer
fetch and depth feedback was chosen, the uber shaders compiled and the shader cache was read; the device died on the
missing file, not on the driver. A Vulkan build would not have avoided this one (its shaders have the same staging
rule), and it is not needed for this failure.

## Settings page (owner, 2026-11-06)

While the ARMSX2 GS is selected the CoreGS 2.0 rows are hidden (render scale, vsync, performance HUD, Performance Mode,
texture loading, FXAA) and come back with CoreGS 2.0; the ARMSX2 resolution row only shows for the ARMSX2 GS. Both
directions were checked in the emulator. The new rows are translated in pt-BR: "Resolução do ARMSX2 GS", "1,5x",
"ARMSX2 GS (experimental, build privada)".

## Owner's S23 report, second run (2026-11-06): the GS thread aborted on the first frame

With the shaders staged, his log (`private/owner-logs-20261106-020257`) shows the device being created on the Adreno 740
(ES 3.2 context, Adreno profile, uber shaders compiled, `create_done`) and the stream starting — then:

```
[0.4262] (gt4gs) stream started (renderer OpenGL ES)
Aborting application.
```

The tombstone of all three runs is the same: SIGABRT on the **GS thread**, in the dump-replay overlay, called from the
GS renderer's end-of-present path. That overlay is the status text the emulator shows while it plays a recorded GS dump;
this library has graphic state but no dump file, so the overlay read a null dump and aborted on the first VSync.

**Fix.**
- The end-of-present path returns as soon as it has ended the present while the stream runs: no dump overlay, no
  fullscreen UI, no OSD and no ImGui frame (this library has no UI; the game's own HUD is the only overlay).
- The dump overlay itself now returns when there is no dump file, for any other caller.
- For diagnosis, `GT4_ARMSX2_GS_RENDERER=null` in `debug.env` selects the deviceless renderer: no GPU context and no
  picture, but the whole stream (GIF, privileged writes, VSync) runs. `ogl`/`vk` force a back end.

## Emulator check of this fix (2026-11-06)

- Deviceless run (`GT4_ARMSX2_GS_RENDERER=null`): the GS opens (`Null host device`), the stream starts and the VSyncs
  advance for as long as the game runs — 2,700+ VSyncs in about 70 s, no abort (the crash happened on the *first*
  VSync before). The GS thread cost was 0.6 ms/frame at 2x scale.
- Normal GL run on the emulator: unchanged, it stops at the emulator's guest OpenGL ES 3.0 (the uber shader needs 3.1)
  and returns cleanly — the emulator cannot show the Adreno path.

## API choice and picture options (owner, 2026-11-06)

**API.** A new row, "ARMSX2 GS graphics API", with **OpenGL ES** (default) and **Vulkan (experimental)**, plus one help
line: ARMSX2 keeps Vulkan out of its Android renderer choice because of blending bugs. It maps to
`GT4_ARMSX2_GS_RENDERER` (`ogl`/`vk`) and from there to the library's existing back-end choice. This library is built
without the Vulkan back end (no shaderc sources locally), so asking for Vulkan **falls back to OpenGL ES with a line in
`game.log`** ("Vulkan was requested but this library has no Vulkan back end…") instead of failing; a build that carries
Vulkan and fails at device creation reports the failure in `game.log` and stops the game the same way a GS failure always
does.

**Picture options.** Each has its own row and its own key in `armsx2-gs.ini` (PCSX2's own keys):

| Row | Key | Values written |
|---|---|---|
| Anti-aliasing (FXAA) | `fxaa` | `true` / `false` |
| Sharpening (CAS) | `CASMode` | `1` (sharpen only) / `0` |
| Sharpening amount | `CASSharpness` | 0..100 |
| Texture filtering | `filter` | 0 nearest, 1 bilinear forced, 2 PS2, 3 forced but sprites excluded |
| Anisotropic filtering | `MaxAnisotropy` | 0, 2, 4, 8, 16 |
| Dithering | `dithering_ps2` | 0 off, 1 scaled, 2 unscaled (PS2) |

The rows are written into the `[EmuCore/GS]` block of `armsx2-gs.ini`; the owner's `armsx2-gs-extra.ini` is applied after
it by the library, so his own lines still override everything.

**FSR1: not offered.** This ARMSX2 version does have it (and Snapdragon Game Super Resolution too), but both are compute
passes that ARMSX2 implements **only on Vulkan**, and this library has no Vulkan back end. Offering them here would be a
switch that does nothing.

**Defaults for the S23** (a fresh install): ARMSX2 GS at **2x**, OpenGL ES, FXAA off, CAS off (amount 50), texture
filtering Bilinear (PS2), anisotropic off, dithering PS2 (unscaled). A stored choice is always kept.

## Performance HUD with the ARMSX2 GS (owner, 2026-11-06)

The launcher's Performance HUD row (Off / FPS only / Full) works with both renderers now. With the ARMSX2 GS the library
draws the panel itself (via the ImGui context it already has) over the picture it presents; it never draws the emulator's
own OSD, and the `debug` text that fix2 removed stays removed. Lines, with the label each number really is:

- `ARMSX2 GS  <API>  <scale>x` and the SoC name (as Android reports it);
- `FPS` and `Frame` (the emulator's frame timing);
- `GS` — this library's GS worker: ms per frame and its share of a core;
- `GPU` — the graphics device's own timer, `n/a` when the driver does not report one;
- `CPU` — the game process's share of all threads (the same rule as the desktop HUD);
- `Mem` — the process's resident memory.

The game's F7/F8 performance report and its touch button are the game's own and keep working unchanged; the HUD's CoreGS
"Report:" line has no equivalent here and is not shown.

## Performance Mode report (no work this round)

The ARMSX2 sink is fed **after** the game-side producer: the stream leaves the game at the GIF render point (and at the
privileged-register and VSYNC points), inside `gs_stream_client`, so the individual effect toggles built on
`task/180-android-emulator` (draws left out, flare readbacks answered as hidden) change the records *before* they reach
the ARMSX2 library. Nothing is needed on this side for them: the sink forwards whatever the game sends, in order, and the
readback protocol is the game's (a readback answered locally never reaches the library). The two-frame back-pressure is
unaffected.

# Private test build: ARMSX2's GS inside the GT4Recomp Android app (owner's phone only)

Status: 2026-11-06 (1.3.0-rc15, round 4), **PRIVATE and PCSX2-tainted, GPL-3.0+**. Never distributed, never merged,
never pushed. **The GS in this build has no JIT:** no run-time machine code is generated and no executable code memory is
mapped (build flag `GT4GS_NO_JIT`). This clone
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
the internal-resolution multiplier taken from the shared **Render scale** row and the screen, snapped to what the ARMSX2
GS accepts (1x, 1.25x, 1.5x, 1.75x, 2x, 2.5x, 3x, 4x, 5x, 6x). A fractional value adds
`ExtendedUpscalingMultipliers = true` (PCSX2 ignores fractional multipliers without it).

The owner's own lines go into `armsx2-gs-extra.ini`, beside it (created once with an example, never rewritten): every
`[EmuCore/GS]` line there is applied on top of the base file, so a line he adds wins. Both files live in the app's
external files folder:

```
/sdcard/Android/data/io.github.moesuito.gt4recomp/files/armsx2gs/armsx2-gs.ini
/sdcard/Android/data/io.github.moesuito.gt4recomp/files/armsx2gs/armsx2-gs-extra.ini
```

`resources/` beside them holds `GameIndex.yaml` and the OpenGL ES shader sources, copied out of the APK assets by the
launcher on Play.

## Render scale (owner, 2026-11-06)

The separate "ARMSX2 GS resolution" row is gone. Both renderers take their internal resolution from the shared **Render
scale** row, the same percentage-of-the-screen-height model as the PC (`display::percent_for_multiplier`: 1x is 448
pixels of the screen's height, so 31% on a 1440-high screen and 83% on the S23's 1080-high landscape picture). The row's
positions are the multipliers the chosen renderer takes: with the ARMSX2 GS its own 1x, 1.25x, 1.5x, 1.75x, 2x, 2.5x,
3x, 4x, 5x and 6x (a fractional one adds `ExtendedUpscalingMultipliers = true`), with CoreGS 2.0 the whole 1x..12x as
before. The stored value is the percentage, so the label, the positions and the value are one number on every screen.

**The S23 default is 2x (83%).** The PC rule is the same 2x (`RENDER_SCALE_DEFAULT_MULTIPLIER = 2`), and on the S23's
1080-high picture that percentage is exactly an ARMSX2 step, so nothing had to be lowered: 2x is what the previous build
shipped and what the phone held at full speed. The reason not to go higher: 100% ("as tall as the screen") asks for
2.5x on this screen, and the earlier 6x test (test1's notch model) ran the menus at 12 fps. A player who wants more can
pick 100% or the steps above 2x in the row.

## Texture replacement (owner, 2026-11-06)

On the phone the player picks a folder with the system folder picker (SAF, a persistable read permission). The ARMSX2 GS
reads plain file paths and never content URIs, so the picked tree is copied **once** into the app's own storage
(`<app data>/data/textures-sync`) when the player chooses it; every later start reads that copy, and nothing is copied
again. A new pick replaces the copy; "Use default folder" deletes it and goes back to `<data>/Textures`.

The expected layout is PCSX2's own, the same one the PC's GSdx path reads (`GSTextureReplacements::GetGameTextureDirectory`):

```
<chosen folder>/
  SCUS-97328/
    replacements/
      <the pack's files, any subfolders>
```

Picking the game's folder itself (one that holds `replacements` at its top) also works: after the copy its contents are
moved under `SCUS-97328`. The Settings page shows the folder by the name the picker gave it ("Pasta de texturas: GT4
textures (escolhida no celular)") instead of the internal path.

The library gets `[Folders] Textures = <copy>` in `armsx2-gs.ini`, `LoadTextureReplacements = true` and
`LoadTextureReplacementsAsync = true` (the "Custom textures" row, on by default). It logs at every start the folder,
whether loading is on and how many replacement files it found:

```
[gt4gs] textures: folder .../data/textures-sync, loading on, 4 replacement files under .../SCUS-97328/replacements
```

The launcher logs the pick itself (`textures: "name" copied into ... (N files, M replacements ..., layout ...)`).

## Version and file name like the PC (owner, 2026-11-06)

- `versionName` is `1.3.0-rc15` (`GT4_RELEASE_LABEL=rc15` plus the crate version), so the launcher's footer, the logs
  and Android's own app information say what the PC's rc15 says.
- `versionCode` is `major | minor | patch | rc | build` as five groups: `1 03 00 15 00` = 1.3.0-rc15 build 0, so
  **103001500 + build number** (this build: **103001501**). A final release of 1.3.0 uses `99` for the rc group
  (103009900 + build) so it sorts above every rc of it; 1.3.1 starts at 103010000.
- **Installing the public test builds (versionCode 5) on the same phone:** Android refuses an APK with a lower
  versionCode than the installed one, so a public build cannot be installed over this private one. Uninstall this app
  first (its app data, including the prepared game files, the card and the texture copy, is removed with it), or give the
  public build a higher code. This private build is never published, so the public line's code is unchanged.


## Final polish2 build, rc15 (owner, 2026-11-06)

**Version and file.** `1.3.0-rc15` (versionName; the launcher's footer and General page, the logs and Android's app
information say the same), versionCode **103001501**, file `GT4Recomp-1.3.0-rc15-mobile-arm64.apk`,
**143,919,214 bytes**, sha256 `71ec60d870b11076cb33ba0f497f1e61c7367b9ef509d6d350934d42e5df5a97`, signed with the owner's
release key (CN=GT4Recomp, O=moesuito) and `zipalign -c -P 16` clean. The old `android-test4` APK and its `.sha256` are
gone; `symbols/` carries the new `libgt4gs-unstripped.so`.

**What the player sees.**
- **GSdx (ARMSX2) is the default renderer**, CoreGS 2.0 the second choice, in both languages; a renderer the player
  chose is kept. A fresh install logs `android: the default renderer is armsx2` and saves it.
- **The Render scale row** (same percentage-of-the-screen-height model as the PC) sits right under the Renderer and
  feeds both renderers. On the emulator's 1080-high screen a fresh install shows **83% · 2x · 1280x896**: the screen is
  read at start-up and the table's 100% (old model) becomes the 2x default before the first Play.
- **The Performance HUD row shows with GSdx (ARMSX2)**, and the setting reaches the library: the game starts with
  `GT4_ARMSX2_GS_HUD=full` and every VSync line ends in `HUD full`.
- **No empty headings:** the Display and Advanced sections are hidden when they have no visible row.
- **"Escolher pasta…" opens the phone's folder picker** (SAF, persistable read permission). The picked tree is copied
  once into `<data>/data/textures-sync`, laid out as PCSX2 packs are (`<serial>/replacements`), and the page shows the
  name in plain words: "Pasta de texturas: GT4-textures (escolhida no celular)". "Usar a pasta padrão" deletes the copy.

**Texture proof (`game.log` via logcat).** `[gt4gs] textures: folder .../textures-sync, loading on, 4 replacement files
under .../SCUS-97328/replacements`; the launcher logs `textures: "GT4-textures" copied into .../textures-sync (4 files, 4
replacements ..., layout Root)`. `armsx2-gs.ini` holds `upscale_multiplier = 2`, `LoadTextureReplacements = true`,
`LoadTextureReplacementsAsync = true`, `PrecacheTextureReplacements = true` and `[Folders] Textures =
.../textures-sync`. The test pack is four 64x64 PNGs made for this check (never game data), at
`build/evidence/t180-rc15/textures/SCUS-97328/replacements/`.

**Emulator evidence** (`emulator-5556`, x86_64 API 36 image, arm64 APK through translation, `-gpu lavapipe`; the
scratchpad's `emu2.cmd` refused to start while emulator-5554 held the same AVD without `-read-only`, so the separate AVD
`F:\GT4Recomp-android\avd-b` (`build/emu-second.cmd`) was used on the same port). Screenshots in
`build/evidence/t180-rc15/` (not committed; `build/` is ignored):
- fresh install, English: `06-settings-en.png` (GSdx (ARMSX2), 83% · 2x · 1280x896), `09-graphics-advanced-en.png`
  (Performance HUD row, RC settings, no empty heading), `10-hud-full-en.png` (HUD set to Full), `05-home-en.png`
  (`GT4Recomp 1.3.0-rc15`), `18-general-en.png` (Version `1.3.0-rc15`);
- textures: `12-textures-block-full-en.png`, `13-folder-picker.png`, `15-picker-gt4textures.png` (the system folder
  picker in GT4-textures), `17-textures-chosen-en.png` ("Textures folder: GT4-textures (chosen on this phone)");
- pt-BR: `20-graphics-ptbr.png` (Renderizador GSdx (ARMSX2), Escala de renderização 83% · 2x · 1280x896),
  `21-textures-ptbr.png` ("Pasta de texturas: GT4-textures (escolhida no celular)"), `19-language.png`;
- Play: the deviceless renderer (`debug.env`: `GT4_ARMSX2_GS_RENDERER=null`, because the emulator's guest GL is ES 3.0
  and ARMSX2 needs 3.1) ran the stream with `HUD full`; every line above is from that run.

**Vulkan and FSR1: still not available.** The Android build links a `shaderc` target that the local ARMSX2 source tree
does not carry: `3rdparty/shaderc` and its glslang / SPIRV-Tools / SPIRV-Headers sources are absent, and the clone has no
`.gitmodules` pinning their revisions. Building the Vulkan back end means fetching all four at the right revisions and
building them for arm64; the brief allows stopping there, so the library keeps OpenGL ES, `gt4gs_has_vulkan()` returns 0,
a Vulkan request falls back with a `game.log` line, and FSR1 / Snapdragon Super Resolution stay unoffered (they are
Vulkan-only in this ARMSX2).


## Round 4 (owner, 2026-11-06): no JIT, test5, the CoreGS HUD, VSync and frame rate rows, Vulkan

### No JIT

**What was there.** The ARMSX2 tree carries three run-time code generators:
- the GS software rasterizer's scanline and setup-prim generators (`GSDrawScanlineCodeGenerator`, `GSSetupPrimCodeGenerator`,
  xbyak on x86 and vixl on arm64, through `GSNewCodeGenerator` and `GSCodeReserve`);
- the emulator's ARM64 guest recompilers (EE `arm64/iR5900-*`, IOP `iR3000A-*`, microVU, VIF), all vixl emitters;
- the executable code arena `SysMemory` maps at boot (`HostMemoryMap` code region), which is what `HasCodeMemory()` tests.

**What was done.** A `GT4GS_NO_JIT` build (`platforms/android/app/src/main/cpp/CMakeLists.txt`, a CMake option plus the
compile definition):
- the GS scanline/setup-prim generators are not compiled (`pcsx2/CMakeLists.txt`), `GSDrawScanline` always takes its
  C++ rasterizer (`CSetupPrim` / `CDrawScanline`) and `GSCodeGeneratorFunctionMap` never generates;
- `SysMemory` maps **no** executable memory and logs it; `HasCodeMemory()` stays false, so
  `VMManager::InitializeCPUProviders` initializes no recompiler provider, `UpdateCPUImplementations` selects the
  interpreters, and the GS-only VM uses the stream CPU provider as before. Nothing in the shipped library can generate
  code at run time.

**Proof (final APK).**
- `game.log`: `[gt4gs] no JIT: GT4GS_NO_JIT build; executable code memory absent` and
  `GT4GS_NO_JIT: no executable code memory is mapped (no JIT)`.
- `llvm-nm libgt4gs.so`: no `GSDrawScanlineCodeGenerator`/`GSSetupPrimCodeGenerator` code (only the function-map template
  metadata), and no xbyak symbols at all (x86 only, never in this arm64 build).
- The game reaches the title/boot and the Vulkan back end renders (screenshots below) with the deviceless renderer too.

**Cost and limits.** The hardware renderer uses the C++ rasterizer only for its small palette/texel copy path; at 2x in
the emulator the GS thread stays around 5-13 ms/frame, the same order as before. The SW renderer itself is never
selected. The ARM64 **guest** recompilers (EE/IOP/VU/VIF) are still linked (vixl symbols are in the binary) because the
emulator's provider table references them; they cannot run without code memory and the GS-only VM never uses them.
Removing that dead weight from the link is a separate, larger surgery.

### The Performance HUD is CoreGS 2.0's

`gt4gs_hud.cpp` now reproduces the CoreGS 2.0 panel (TASK-135): top right with a 12 px margin, black 63% rounded
background, the same 36-column layout and font size rule (`clamp(window/1080*16, 12, 44)` px), the same line order and
labels — graphics device name, processor/SoC name, renderer and scale with `[WxH] Hz` on the right, `FPS:   x [low high]`,
`Frame: x [low high] ms`, `CPU:  n%`, `GPU:  n%`, `Mem:` / `VRAM:`, `Report: Idle|Recording|Saved` — the same colours
(white, light red, light blue, light green, dim) and number formats, the same 500 ms text pace, the same 5-line
frame-time graph (50 ms top, guides at one and two 60 Hz frames, blue line, red spikes), and the same FPS-only panel
(10 columns, `%5.1f FPS`). The renderer row says `GSdx (ARMSX2)`; every other reading comes from the same sources CoreGS
uses on Android (process CPU time, `/proc/self/statm`, the SoC properties with the marketing name, the Adreno driver's
kgsl busy share), and a reading Android does not give the app shows `n/a`/`--` instead of a guess. The GPU row is now the
driver's busy percentage, exactly as CoreGS shows it: the old "0.68 ms" line was this library's GS device timer, not the
whole GPU frame, and it is gone.

Limits: the game's F7/F8 report state lives in the game library, so the GS library can only show `Report: Idle` when the
launcher's report is on (`GT4_PERF`); it cannot see Recording/Saved across the `.so` boundary. The FPS-only panel and the
full panel were checked in the emulator side by side with CoreGS (screenshots below).

### VSync and frame rate rows under GSdx (ARMSX2)

- **VSync** is no longer CoreGS-only: the row shows with GSdx (ARMSX2). The value goes into `armsx2-gs.ini` as
  `VsyncEnable` (and `DisableMailboxPresentation` for the mailbox bit) through the same table row the PC's gt4-gs gets.
  Emulator: `vsync on` in the launcher's plan line and `VsyncEnable = true` in the ini.
- **Frame rate (experimental)** is no longer CoreGS-only either: the ARMSX2 plan *is* the CoreGS plan, so the row passes
  `GT4_FRAME_SKIP` / `GT4_FRAME_SKIP_FORCE` to the game exactly as CoreGS does. Emulator: `frame rate 30` in the plan
  line and `GT4_FRAME_SKIP_FORCE=2` in the game's start line.

### Vulkan back end and FSR1

**Sources and versions.** The Android ARMSX2 tree already vendors shaderc (`3rdparty/shaderc`, `v2026.2-dev` per its
`CHANGES`); its DEPS pins glslang `09c541ee5b22bbac307987b50d86ec2b4f683d75`, SPIRV-Headers
`465055f6c9128772e20082e893d974146acf7a02` and SPIRV-Tools `e4bceacf59fdfe742047e94e41ef65a48999a0dd`. (The clone's own
Android CI pins shaderc `v2025.4` and glslang `7a47e253...`, but it does not match the vendored Android tree, so the
tree's own DEPS were used.) The three Khronos repositories were downloaded at those exact revisions into
`private/armsx2-vendor/platforms/android/app/src/main/cpp/3rdparty/shaderc/third_party/` (ignored, never tracked).

**Build and result.** `libgt4gs.so` is built with `USE_VULKAN=ON` and links the shaderc/glslang/SPIRV-Tools static
libraries; `gt4gs_has_vulkan()` now reports 1. The emulator's Vulkan (Lavapipe, Vulkan 1.3) opens a swapchain, compiles
its pipelines at run time through shaderc (`Vulkan: wrote ... bytes of pipeline cache`) and renders the game: the v2.00
build and the HUD screenshots below come from that run. OpenGL ES stays the default API; Vulkan is "Vulkan
(experimental)"; a Vulkan start failure falls back to OpenGL ES with the existing log line.

**FSR1 and SGSR.** A new "Upscaler" row (Off / FSR1 (AMD) / Snapdragon Super Resolution) is visible **only while Vulkan is
chosen** (`visible_when: armsx2_api == vk`); under OpenGL ES the row is hidden and the ini always writes `Upscaler = 0`,
so only the normal sharpening (CAS) is used. The values are ARMSX2's own `GSUpscaler` enum (`2` FSR1, `3` SGSR). Emulator
proof: `Upscaler = 2` in the ini and `@@ANDROID_FSR1@@ upscaling 1280x896 -> 1928x1080 (sharpness 50)` in `game.log`.
No upscaler is written by us.

### test5 merged, Spec II on Android

`origin/task/180-android-emulator` at `7f2e6c43` (effect toggles `42268f4b`, Spec II `593fabed`) is merged into
`private/armsx2-gs-s23`; the conflicts (STATUS, the 2026-11-06 journal, the model row test) were resolved keeping both
sides. The effect toggles show under GSdx (ARMSX2): "Sun glare and light flares" is a shared row (the game side answers it
before the stream leaves), the road sheen stays CoreGS-only. A TASK-186 correction (a GSdx "Blending accuracy" toggle) had
not landed by `7f2e6c43`, so there was nothing to map; ARMSX2 keeps its High blending accuracy.

**Spec II check.** The PT-BR Spec II image (`Gran Turismo 4 Spec II v1.10 (PTBR).iso`) was picked with the system picker;
the launcher identified it (`disc accepted as spec2: SCUS-97436 v1.10`), set it up (`Gran Turismo 4 Spec II v1.10 (PT-BR)
is ready`), and Play used the Spec II library and its own folder: `[mc] card in slot 1:
.../data/games/spec2/runtime/card.ps2`, and the ARMSX2 GS now receives the right serial from the launcher
(`GT4_GAME_SERIAL`): `[gt4gs] game: title 'Gran Turismo 4 [Online Public Beta]', serial 'SCUS-97436'`. It runs with the
Vulkan renderer and the stream advances (2700+ VSyncs, HUD full) in the emulator; the picture stayed black there (ARM
translation + this image), which is as far as this emulator lets Spec II go. The APK grew by **83,079,506 bytes** with the
second library (143,919,214 to 226,998,720 before the FSR row, 227,015,104 final).

### Final APK (rc15, round 4)

`GT4Recomp-1.3.0-rc15-mobile-arm64.apk`, versionName `1.3.0-rc15`, versionCode **103001502**, owner's release key,
**227,015,104 bytes**, sha256 `b86066309d52f093864f49af9c01052b6f33e8a1dfb6c65f5c6fe32ab1207c32`, `symbols/` with
`libgt4`, `libgt4spec2` and `libgt4gs` unstripped. Screenshots: `build/evidence/t180-rc15b/` (ignored).

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

## Default renderer and the settings page (owner, 2026-11-06)

- **Default.** This build starts new installs on **GSdx (ARMSX2)**; **CoreGS 2.0** is the second choice. The labels are
  the PC's where they exist, in both languages. A renderer the player already chose is kept: the platform default fills
  only a value the player never set (`Settings::was_chosen`), and an unusable stored value (GSdx, a computer value) in an
  old file becomes the ARMSX2 default once and is saved.
- **Rows under GSdx (ARMSX2):** the ARMSX2 rows (Custom textures, graphics API, FXAA, CAS and its amount, texture
  filtering, anisotropic filtering, dithering), plus the rows both renderers share: Render scale, frame rate, the
  performance HUD, Performance report, VU code check, language, update channel and the controls. CoreGS 2.0's own rows
  (vsync policy, Performance Mode, texture loading, FXAA) hide.
- **Rows under CoreGS 2.0:** its own rows (vsync, Performance Mode, texture loading, FXAA, sharpening) plus the same
  shared rows; every ARMSX2 row hides.
- **Render scale and the Performance HUD are shared now:** the render scale feeds both renderers (see below), and the
  HUD's panel is drawn by the ARMSX2 library. Neither row is tied to a renderer any more.
- **Empty headings.** A heading with no visible row under it is not drawn: the empty "Advanced" heading the owner saw is
  gone.
- **The Performance report stays the player's:** the owner turned it off; nothing here changes it.

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

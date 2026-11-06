# Android port: plan (TASK-022, CoreGS 2.0 only)

Status: **plan only** (owner, 2026-11-04). Phase 4 of the roadmap; the owner's long-term goal. Builds on the
[Linux plan](LINUX-PORT-PLAN.md) foundation (platform seam, determinism, Linux arm64 rehearsal). CoreGS 2.0 is the only
renderer; GSdx is Windows-only. Sources: four read-only audits of 2026-11-04 (game code, CoreGS 2.0, launcher, previous
plans) and the decision "efficiency first, mobile later" (ai-memory `decisions/efficiency-first-mobile`). Claims about
Android GPUs, drivers and store rules are general knowledge (Hypothesis) until measured on devices.

## Goal

Gran Turismo 4 (v2.00 and Spec II) running natively on arm64 Android phones and handhelds: the same game logic, CoreGS
2.0 on Vulkan, 60 FPS at 1x (2x on strong devices) with Performance Mode, a controller first, the player's own disc
image. No JIT is needed, which is a real advantage on Android (no W^X or code-download conflicts).

## What already works in our favour

- No Windows API and no x86 intrinsics in the game code; the VU output uses portable clang vector extensions.
- The EE FPU is integer soft-float; VU results go through integer flush/clamp helpers, so flush-to-zero modes do not change
  them; float-to-int conversions are masked against x86/arm64 saturation differences.
- Guest memory is a plain static array accessed through `memcpy` (unaligned access is safe on arm64); no page tricks, no
  `VirtualProtect`, no guard pages; the MTVU ring was already reviewed for weak ordering (TASK-092).
- Input is already SDL3, which has a mature Android backend (`SDLActivity`, controllers, sensors, audio).
- CoreGS 2.0 has a path without pixel interlock (IN-466/471) and a colour-attachment path (IN-481) for weaker GPUs.

## The hard parts, ranked

### 1. CoreGS 2.0 on tile-based mobile GPUs (the largest risk)

CoreGS 2.0 does the whole pixel stage in the fragment shader as a read-modify-write of `R32_UINT` storage images, inside a
render pass with no attachments. Mobile GPUs:
- **lack fragment shader interlock** (Mali, Adreno, PowerVR as far as known): the barrier-based ordering path becomes the
  default. It was tuned on desktop (6.85 ms vs 3.9 ms per VBlank at 3x on an RX 9060 XT) and never measured on a tiler,
  where in-pass framebuffer barriers without `BY_REGION` may flush tiles;
- get no tile-memory benefit from storage-image read-modify-write (all pixel traffic goes to memory; mutable formats also
  prevent the framebuffer compression Mali and Adreno use);
- pay for readbacks with fence waits on the EE's critical path (the night flare table), and for 284-1026 full barriers
  per VBlank and per-batch descriptor updates (about 443 batches per VBlank).

What it needs, in order:
1. Start-up capability probe and **per-vendor profiles** (Adreno, Mali, Xclipse, PowerVR) instead of today's
   `INTEGRATED_GPU` heuristic, which on Android would switch on defaults tuned on an Intel HD 620 (an immediate-mode GPU).
2. Measure the no-interlock path on a real Adreno and a real Mali with the recorded drives before any redesign.
3. `BY_REGION` dependencies where only same-pixel order matters; fewer full barriers.
4. The roadmap's **D1 path** (common draws through real colour/depth attachments, hardware blending where the GS blend is
   expressible; `COREGS2-ROADMAP.md:279`) as the main mobile workstream; storage-image fallback only for the GS cases
   hardware cannot express. The decision "no mobile-only paths yet" holds until the owner starts this phase; the probe
   and measurements come first.
5. Performance Mode on by default on phones (it already hides the flare readback); scale 1x default.

### 2. Vulkan correctness gaps that are bugs everywhere (fix on Windows now; not mobile-only)

- Features used but never enabled: `shaderStorageImageArrayDynamicIndexing` (`hw_deinterlace_scaled.comp:30,62`),
  `largePoints` (`hw_vertex.vert:67`, a POINT_LIST pipeline), and lines without the wide-line feature. Strict mobile
  drivers may misdraw or crash. Query, enable, or fall back (quad expansion from `hw_gs/decide`).
  **Done in A1 (TASK-179):** both features enabled where supported, with fallbacks that draw the same pixels; no line is
  ever wider than 1.0, so wideLines is not needed (see "Stage A1 result").
- Required formats that core 2 does not use (`D32_SFLOAT` depth attachment, blendable `R16G16B16A16_UNORM`) can refuse a
  device for nothing (`hw_gs_backend.cpp:78-89`): drop them. **Done in A1.**
- Vulkan 1.2 is required (`hw_gs_backend.cpp:583`); much of the Android installed base reports 1.1. Accept 1.1 with
  `VK_KHR_timeline_semaphore` (and what else 1.2 core is used for) if the device floor needs it.
- The swapchain uses `preTransform = currentTransform` but never rotates the content (`hw_gs_backend.cpp:895`): Android
  surfaces are often portrait-native, so the picture would be rotated or distorted. Use identity with a rotated blit, or
  handle the transform. **Done in A1** (identity wherever supported; quarter-turn-only surfaces refused, see below).
- Memory: about 400 MiB of mapped host buffers (256 MiB staging, 64 vertex, 16 uniform, 64 readback) before any image,
  sized by constants. Size the rings from the heaps and `VK_EXT_memory_budget`; Android's low-memory killer acts fast.
  (400 MiB is the integrated-GPU figure with 8 recordings in flight; a discrete GPU has 16 and maps 736 MiB.) **Done in
  A1:** sized from the heap's budget with today's sizes as the cap.
- Uploads require `HOST_COHERENT` memory and never flush; correct, but on Adreno/Mali the cached type may be non-coherent,
  so readbacks get slow: choose memory types per vendor.
- Interpolation exactness: depth from `floor(gl_FragCoord.z * 2^32)` and colour from `floor(interpolant + 0.001)`
  (`hw_pixel_uber.frag:582,587`) without `precise`/`invariant` in the vertex shader; mobile precision and FMA can move
  one-pixel ties. Golden frames per GPU family decide.

### 3. Determinism (game logic identical to Windows)

Shared with Linux: `-ffp-contract=off` everywhere (arm64 always has FMA; `vu_ops.hpp:304` fuses otherwise), the libm
calls in EFU/IPU paths (bionic differs from UCRT in the last bit), the IOP `strtol` `long` clamp, ThreadSanitizer and arm64
hardware runs for memory ordering, and the cross-platform identity test on the recorded drives. Proven first on **Linux
arm64** (stage L1b), where tools are easier than on a phone.

### 4. App structure: no second executable on Android

An Android app cannot start the game as a separate program. Design:
- **Launcher**: the existing HTML/JS UI through Tauri 2 Android (its own focus engine, `nav.js`, already drives the UI
  from keys and pads), with Rust core crates cross-compiled for `aarch64-linux-android`. Tauri mobile is less mature than
  desktop; file picking with persistent permissions needs a small Kotlin plugin. Fallback: a native Kotlin launcher calling
  the same Rust core through JNI.
- **Game**: a second Activity in **its own process** (`android:process=":game"`), hosting SDL3's `SDLActivity`, loading
  `libgt4.so` or `libgt4spec2.so`. A separate process keeps the game's global state fresh for each Play and lets the
  launcher survive a game crash.
- **Launch contract**: the environment-variable and `game.ini` contract is kept: the game Activity receives the values as
  Intent extras and sets them in its own process before the game starts. Logs go to app files.
- **Exit and crash reporting**: no exit codes. A result file written by the game on normal exit, `ApplicationExitInfo`
  and tombstones for crashes, and the existing log-based classification.

### 5. The disc image and storage

- The player picks the ISO through the Storage Access Framework; the app keeps a persistable permission and reads the
  file through a file descriptor (`ContentResolver.openFileDescriptor` + `pread`). Today both the launcher (`gt4l-disc`
  opens a `Path`) and the game (donor `disc.cpp:68` uses `fopen` on a path) need an fd-based reader. No copy (4+ GB).
- First hash of a 4 GB image over SAF is slow: hash once, then trust size and modification time (`DocumentsContract`
  queries) as the launcher already does with paths.
- FAT32 SD cards cannot hold a file of 4 GiB or more: explain, do not fail obscurely.
- Cards, saves, replays, settings and the shader cache live in app-specific storage; import/export through SAF.
- **Durability**: Android kills background apps without warning. The memory card must reach storage when the game saves
  (fsync the file and the directory: `memcard_store.cpp:65` renames without a directory sync today).

### 6. Lifecycle

`onPause`: stop the VBlank pump, mute and pause audio, stop haptics; `surfaceDestroyed`: drop the swapchain;
`onResume`/`surfaceCreated`: recreate it and continue. A process killed in the background loses unsaved progress (no
save states in this project): say so in the UI. Multi-window, foldables (resize, aspect changes mid-game), and the
display cutout (an option to keep the HUD clear of it, using TASK-080's parameterised HUD layout).

### 7. Performance, heat and threads

- Threads: EE (with an explicit large stack: today it runs on the Windows main thread's 1 MB; arm64 frames differ),
  VU1/MTVU worker, GS worker, audio, presentation. `hardware_concurrency()` counts little cores: size pools from big cores.
- Android Dynamic Performance Framework hint sessions for the EE and GS threads; sustained-performance mode; thermal
  status to step down (scale, Performance Mode, the 30 FPS mode).
- `fp4.hpp` needs a NEON form (the scalar fallback is bit-correct but costs about a fifth of EE time in races).
- The IOP and VU0 run on interpreters; profile them on a big core early (stage A2).
- Shader preparation: about 544 specialised pipelines of a very large shader on first run; driver caches are not
  portable and are invalidated by driver updates. A "preparing graphics" screen with background compilation and a
  persistent cache; watch for driver watchdogs on long compiles.

### 8. Audio, display and input

- Audio: SDL3 audio (AAudio). Bluetooth headsets add 150-250 ms: the pace stays VBlank-based; consider an A/V offset.
- Display: 90/120/144 Hz panels: present at 60 with the platform frame-rate API or Swappy/`VK_GOOGLE_display_timing`;
  the game pace never follows the display rate.
- Controllers: SDL3 on Android (Bluetooth and USB); the controller choice of rc12 carries over. Touch controls are a new
  feature (an on-screen DualShock 2 with analog throttle/brake, and optionally tilt steering through SDL3 sensors): owner
  decision on scope. Racing wheels over USB-OTG: out of scope (no force-feedback API for wheels on Android).

### 9. Toolchain and distribution

- NDK clang (LLVM family like our clang-cl 22; the FP flags, not the version, carry determinism), CMake + Ninja, `arm64-v8a`
  only, **16 KB page alignment** for every native library (`-Wl,-z,max-page-size=16384`: the game, Rust, SDL3), current
  `targetSdk`.
- Two game libraries of roughly the Windows executables' size (94-102 MB each before compression), plus the launcher.
- First channel: **APK on GitHub** (sideload; updates through `PackageInstaller` with the user's consent; `SHA256SUMS`).
  Play Store later, if ever: no runtime code downloads (our renderer is built in, fine), the size limits (Play Feature
  Delivery per game if needed), and the store's stance on apps that need a game disc image (a policy risk to assess).
- Discord: none on Android. RetroAchievements: token in the Android Keystore.

## Stages

| Stage | Content | Gate |
|---|---|---|
| A0 | Linux L0-L1b done: platform seam, determinism foundation, Linux arm64 identity, NEON `fp4` | arm64 identity on the recorded drives |
| A1 | Fix the Vulkan correctness gaps on Windows (missing feature enables, vestigial formats, rotation-safe present, budget-sized rings, per-vendor profile structure) | Windows identity and frames unchanged (done on Windows, TASK-179: see "Stage A1 result") |
| A2 | Android skeleton: NDK build of `libgt4.so`, SDL3 game Activity in its own process, Android surface, boot to the title screen at 1x with CoreGS 2.0; IOP/VU0 interpreter and EE profile on a big core | boots on an Adreno and a Mali device |
| A3 | Storage: SAF disc picker, fd-based reader in launcher and game, app storage for cards and caches, durable card writes | Spec II and v2.00 reach a race from the player's ISO |
| A4 | Correctness on mobile GPUs: golden frames of the recorded drives against Windows, per vendor; fix driver issues behind per-vendor switches | picture parity on Adreno and Mali |
| A5 | Performance: measure the no-interlock path, BY_REGION and barrier reduction, then the D1 attachment path; Performance Mode default; ADPF and thermal steps | 60 FPS at 1x in races on the target device class, stable over 20 minutes |
| A6 | Lifecycle, audio, input, display (pause/resume, Bluetooth audio, 120 Hz panels, cutouts, controller choice, optional touch) | owner checklist |
| A7 | Launcher on Android (Tauri or native), shader preparation screen, settings subset for phones | focus checks with a controller |
| A8 | Packaging: signed APK, 16 KB alignment check, update flow, release assets | sideload install and update on two devices |
| A9 | Owner test and a closed beta | owner approval |

## Stage A2 progress (TASK-180, from 2026-11-04)

Started on the Android Studio emulator (x86_64, API 36) before any phone, with the owner's approval. Procedure, the
download list, the AVD and the adb commands: [ANDROID-EMULATOR](ANDROID-EMULATOR.md).
- **Seam:** the Linux plan's L0 layer, done for Android first (see LINUX-PORT-PLAN, stage L0); Windows unchanged.
- **Build:** `android/native/CMakeLists.txt` builds the whole game as `libgt4.so` with NDK 26.1 (API 29, CMake + Ninja
  outside Gradle): the translated v2.00 units (rc14b), the pinned donor runtime and capsules (checked against
  `docs/inputs`), VU1/VU0 AOT, CoreGS 2.0, SDL3 3.4.18 from the official AAR. Flags pinned for determinism and ABI parity
  with clang-cl: `-ffp-contract=off`, `-fno-strict-aliasing`, `-fsigned-char`; 16 KB page alignment for every ABI.
- **App:** `android/` holds a minimal Gradle project (AGP 9.5.0, Gradle 9.8.2): one `GameActivity` extending
  `SDLActivity` in its own process (`:game`), landscape, Vulkan, controller; the native libraries are staged into it.
- **arm64-v8a probe (2026-11-05):** every native library (runtime and capsules, VU1/VU0 AOT, CoreGS 2.0, the GS seam,
  host I/O), the entry sources and a sample of translated units (with `table.cpp` and the hooked units) compile for
  `aarch64-linux-android29` with no change: `fp4.hpp` takes its scalar form (bit-identical, slower; NEON is still to
  write), `core2_upload.cpp` its new portable column store, the perf clock the generic timer. The full arm64 link was
  not run (disk); the x86_64 link has no undefined symbols. Build command: `android/README.md`.
- **First boot (2026-11-05):** on the emulator with Lavapipe (CPU Vulkan) the game boots, plays the opening movie
  (IPU video), takes Start from the emulator keyboard and reaches the GT Mode home at 1x with CoreGS 2.0, in landscape,
  with SDL3 audio (AAudio). Host-GPU emulation crashes inside AMD's OpenGL driver on this desktop (details in
  ANDROID-EMULATOR); SwiftShader lacks `dualSrcBlend`, which CoreGS 2.0 requires.
- **Learned so far:** the donor runtime and the translated units need no source change for Android x86_64; CoreGS 2.0
  needed only the surface/loader seam and POSIX forms of three diagnostics; one LP64 fix in the pinned runtime capsule
  (configure-time copy, see LINUX-PORT-PLAN); the SDL3 AAR ships its libraries as prefab modules only, so they are staged
  into `jniLibs` by hand. Orientation: SDLActivity overrides the manifest's orientation from `SDL_HINT_ORIENTATIONS`
  (a resizable window without it gets "any orientation"); this Android 16 image reports the window's own size as
  `currentExtent` with `currentTransform` = rotate 90 in landscape, so the identity pre-transform with the extent as
  reported is right (the picture was turned with `preTransform = currentTransform`, and squashed when the extent was
  swapped). That confirms TASK-179's rotation-safe present; the Android-only branch in `hw_gs_backend.cpp` gives way to it.
  The EE thread is SDL's game thread, with a 1 MB stack (as the Windows main thread); enough for the boot.

## First APK: `1.3.0-android-test1` (TASK-180, 2026-11-05)

The owner's rule (2026-11-05): the Android app always opens the PC launcher first (the same Tauri launcher, same
page, same Rust core); Play starts the game. Built from the TASK-180 branch, arm64-v8a, signed with the owner's
release key (kept in `private/`), installed by hand (no store, no in-app installer). Build: `android/README.md`.

- **Structure:** the launcher (`gt4l-tauri` as a library) is `MainActivity`; `GameActivity` (SDL3) runs `libgt4.so` in
  its own `:game` process, started by Play with the Windows launch contract (arguments; game.ini values as environment).
  A Kotlin bridge (`GameBridgePlugin`) gives the Rust side what Windows does with files and processes.
- **Disc image:** picked with the Storage Access Framework, kept as a document with a persistable read permission,
  never copied. Setup (`gt4l-disc`) reads it through a descriptor (`gt4l_disc::source`), so the pins are checked exactly
  as on Windows; the game gets `--disc fd:N` and reads with `pread` through the donor's disc reader API.
- **Platform rules** (`gt4l_core::platform`, tested): CoreGS 2.0 only (no Renderer row, no GSdx rows or downloads), no
  monitor, resolution, fullscreen, graphics card or Discord rows, the GPU counts as integrated (1x at the default Render
  scale), updates are a check plus the APK download in the browser (only releases that ship an APK are offered).
- **Cards and saves:** app storage; card writes `fsync` the file and its folder. Saves and replays import and export
  through the system picker.
- **Logs:** the launcher's lines go to its log files and logcat (`GT4Recomp-launcher`), the game's to the session
  `game.log` and logcat (`GT4Recomp-game`). Android's records of how each app process ended (exit status, crash, ANR,
  low memory) are read at start, at Play and at export; a native crash's tombstone is kept and decoded to text
  (`gt4l_core::tombstone`). Export logs saves one zip through the system picker, with `system.txt` holding the phone's
  model, Android version, SoC, memory, display and refresh rate, Vulkan device and features. The game library keeps its
  symbol table (tombstones name functions); the full one with line tables stays beside the APK in `private/`.
- **Performance report:** with the launcher's report setting on, a small touch button on the game's left edge starts an
  interval (F7) and writes the report (F8) to the game log; it never takes the controller's focus.
- **16 KB pages:** `libgt4.so` and the launcher library are linked with `max-page-size` and `common-page-size` 16384; the
  C++ runtime is linked into `libgt4.so` (NDK r26's `libc++_shared.so` is 4 KB aligned). Checked in the APK with `llvm-readelf -l`
  and `zipalign -c -P 16`: every LOAD segment 0x4000, every library stored 16 KB aligned, the RELRO end of `libgt4.so`
  (0x4168000) and of the launcher library (0xdc0000) on 16 KB. `libSDL3.so` from the official 3.4.18 package ends its
  RELRO at 0x243000 (a 4 KB boundary), but its next writable segment starts at 0x246eb0, in the following 16 KB page,
  so the linker's read-only protection rounded up to 0x244000 covers no writable data; a rebuilt SDL3 would remove the
  question.
- **Not on Android yet, and why:** GSdx (a Windows program; CoreGS 2.0 is the only renderer), Discord status (no Discord
  game status on Android), the in-app updater (Android installs APKs only through the system installer), the launcher's
  own graphics switch (a WebView2 guard), RetroAchievements in the game (its HTTP and overlay layers are Windows-only),
  a texture folder outside the app (needs a document tree), the performance HUD (no text renderer on Android yet), and
  the "open folder" buttons (Android has no file manager entry point for app storage).
- **Noted for later (no change in this APK):** Qualcomm advises against `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` before
  Adreno 750 (it turns off UBWC compression); CoreGS 2.0 sets it on the ROP image (`core2_gpu.cpp`). The performance
  items are in [ANDROID-PERFORMANCE-PLAN](ANDROID-PERFORMANCE-PLAN.md).
- **Tested in the emulator** (arm64 APK through the image's ARM translation, Lavapipe): launcher in landscape, SAF pick
  from Downloads, setup with exact pins, Play, the game's first start and "Save successful" on its card. Not tested
  yet: a real phone (Adreno), pause and resume (A6), performance.

### Stage A1 result (TASK-179, 2026-11-05)

Done on Windows on branch `task/179-vulkan-correctness`; CoreGS 2.0 only, no mobile-only path. Frames, race clock and
simulation state are identical to the rc14 PGO references (gates below). Probe results are from the desktop's RX 9060 XT.

1. **Features used without being enabled.** The references held: `history[index]` in `hw_deinterlace_scaled.comp`
   (declared at line 30, indexed with a push constant at line 62) and `gl_PointSize = pc.scale` in `hw_vertex.vert:67`.
   - `largePoints` and `shaderStorageImageArrayDynamicIndexing` are now enabled where the device has them
     (`vulkan_loader::enable_profile_features`, also on the fixture and replay devices).
   - Without `largePoints` (or with a largest point size under the scale) core 2 draws each point as two triangles: a
     `HWR_POINT_QUAD` build of `hw_vertex.vert`, a triangle-list pipeline that reads the point's vertex record per
     instance and draws six corners per point, the square being the one point rasterization covers. This is done in the
     vertex stage rather than in `hw_gs/decide` because the corners are `centre +- scale / 2` in GPU pixels, which the
     12.4 front-end coordinates cannot express in every half-pixel mode. `GT4_CORE2_POINT_QUADS=1/0` forces it (A/B).
   - Without dynamic indexing the deinterlace kernel's `HWR_CONSTANT_INDEXING=1` build runs (a switch with constant
     indices; every caller's index is below 4, so the pixels are the same). Core 1 picks the same build.
   - Lines: every pipeline (core 2, core 1, the HUD) has `lineWidth = 1.0` and no dynamic line width, so `wideLines` is
     never needed. GS lines are drawn one GPU pixel wide at scale; widening them would be a fidelity change, not a Vulkan
     fix (`hw_gs/decide` already has a LineQuad plan if it is ever wanted).
   - RX 9060 XT: `largePoints` yes (max 8191.9), `wideLines` yes (max 8191.9, not used),
     `shaderStorageImageArrayDynamicIndexing` yes; both enabled, so the default paths are unchanged.
   - GT4 drew no point primitive in the recorded scenes checked (`[core2] points=0` on the night race and the George V
     Paris replay). The fallbacks are covered by tests instead: `hw_core2_point_quads` (900 blended, depth-tested points,
     many on the same pixels, at 2x and 3x: images and GS memory equal with and without the quads), and the GPU fixtures
     run with `GT4_HW_PRETEND_MISSING=large_points,dynamic_indexing` (the probe reports both missing): 84 GPU
     fixtures (core 2, scaling, pixel, tone, GLM, smoke) pass (a first run had one failure, `hw_core2_shader_cache`
     writing no cache in its first session under the parallel load; it passed alone and on the rerun);
     `hw_core2_interlaced_present` (every deinterlace mode at 2x and 4x) runs the constant-index kernel with 0 pixels off
     the CPU deinterlacer; and the Arcade gate at 3x with both missing: 14 of 14 frames identical to the reference, 11 of
     them deinterlaced.
2. **Required formats.** The device check required `D32_SFLOAT` depth attachments, `R16_UINT` sampled,
   `R16G16B16A16_UNORM` blendable and `R32_SFLOAT` colour attachments, which nothing creates. It now requires what every
   path uses: `R32_UINT` storage images (every target) and `R8G8B8A8_UNORM` blits with a linear filter (the presentation's
   upload image); both are mandatory in Vulkan. The ROP path (IN-481) checks its own need, `R8G8B8A8_UNORM` blendable
   colour attachments, and stays off without it.
3. **Rotation-safe present** (`hw_present_transform.hpp`). The pre-transform is now the identity wherever the surface
   supports it (every desktop surface; Android's too): the picture is drawn upright and the compositor turns it for a
   rotated display. Without the identity: `INHERIT`, else a transform the final blit can draw (half turn, mirror, mirror
   with half turn: the blit reverses those axes; the performance HUD stays off there), else (quarter turns only, no known
   surface) the swapchain is refused with a clear error rather than showing a turned picture; a rotation pass would be
   stage A6 work if a device ever needs it. Windows: identity as before, nothing changes. Checked by the
   `hw_present_transform` fixture (every transform a surface may report; every pixel of several pictures through the
   blit's offset rule).
4. **Memory sized from the device** (`plan_core2_rings`). Today's sizes are 400 MiB on an integrated GPU (8 recordings)
   and 736 MiB on a discrete one (16). They stay the sizes and the cap wherever they fit in a quarter of the ring heap's
   budget (`VK_EXT_memory_budget`, enabled where present; else the heap size). Where they do not, the share count goes
   down first (a share keeps its size, so no request that fits today stops fitting), to two at least, then the readback
   buffer, to 16 MiB at least. One log line gives the sizes, the heap, its budget and the allowance.
   `GT4_CORE2_RINGS=fixed` keeps today's sizes; `GT4_CORE2_RINGS=<MiB>` plans for that budget (a small device's sizes on
   the desktop). RX 9060 XT: heap 0 (host, 49074 MiB, budget 48306 MiB), allowance 12076 MiB: today's 736 MiB.
   With `GT4_CORE2_RINGS=256` (64 MiB allowed: 2 shares and 16 MiB of readback, 100 MiB mapped instead
   of 736) the v2.00 night race at 3x is identical to the reference (10 of 10 frames, race clock at all 12,097 VBlanks,
   the simulation state). Fixture: `hw_device_profile`.
5. **Per-vendor profile structure** (`hw_device_profile.hpp`, `vulkan_loader::probe_device_profile`): name, vendor,
   device class (discrete; integrated from a desktop vendor; tiler-or-unknown for Adreno, Mali, PowerVR, Apple, Xclipse,
   software and unknown devices), the features above with what was enabled, and every heap with its budget, logged in one
   `[hw] device profile:` line and handed to the renderer with the device. Core 2's integrated-GPU defaults keep today's
   rule (`VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU`) through `integrated_defaults`; nothing keys off the class yet. Today
   on the RX 9060 XT: `class discrete, integrated defaults off, Vulkan 1.4.359`.

Checks:
- Validation layer (`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`, core checks), short headless runs at 3x of the v2.00
  night race (7,700 VBlanks) and the George V Paris replay (3,000 VBlanks): before, 1 message in each run; after, the same
  1 message. No feature-not-enabled message was reported before either: the layer cannot see these two (glslang declares
  no `StorageImageArrayDynamicIndexing` capability for a push-constant index, and a point size is a run-time value), which
  is why the fix rests on the probe and the fixtures. The one message is `VUID-VkSubpassDependency-srcSubpass-02243`:
  core 2's draw render pass has a self-dependency between fragment stages without `VK_DEPENDENCY_BY_REGION_BIT`. Making
  it by-region changes what the barrier guarantees for reads of other pixels (a texture taken from a target), so it
  belongs to the `BY_REGION` work of stage A5, not here.
- Identity against the rc14 PGO references (`gates6.sh`): Spec II George V Paris, El Capitan and Tokyo R246 at 1x (race
  clock at every VBlank, simulation state, 24, 35 and 35 frames), the v2.00 night race at 3x (10 frames, clock, state),
  the Arcade path at 3x (14 frames), George V Paris at 6x (12 frames), the VU1 and VU0 verify runs (0 mismatches) and the
  MTVU check (0 mismatches in 8,032 frames): all identical. A first George V Paris run, made while other runs loaded the machine, had
  20 bytes of the race object (past the dynamics block) different, with the clock, the dynamics, the frames and the EE
  cycle count identical; reruns of the base build and of this build alone were identical, so that was run-to-run
  variation under load, not this change.
- CTest: 304 of 304 pass on the v2.00 build (three new: `hw_device_profile`, `hw_present_transform`,
  `hw_core2_point_quads`); `hw_replace_q07_pcsx2_dumps` skipped as expected.

Recorded, not done (out of A1's scope): Vulkan 1.1 acceptance; `precise`/`invariant` interpolation; the no-interlock
tuning; the D1 attachment path; `-ffp-contract=off` and the other determinism items (Linux stage L0). Also found: core 1's
scaled display compose (`hw_display_compose_scaled.comp`, not on the product path) indexes `scaled_frames[slot - 1u]`
with a per-circuit value, which needs the dynamic-indexing feature that is now enabled where present but has no fallback
there; and the validation message above.

## Test APK 2: `1.3.0-android-test2` (TASK-180, 2026-11-05)

From the owner's S23 Ultra test of test1:
- **Render scale:** the phone's screen is the launcher's monitor (100% = its native multiplier, 5x at 3088x1440); on
  Android the slider starts at the step that gives 1x (`display::render_scale_floor`); a vertical swipe no longer moves a
  slider (the owner's accidental 5x); every setting change and the scale chosen at Play are logged.
- **Performance HUD** (TASK-175's panel, merged from rc15): glyphs from the system monospace font through the activity,
  CPU line = the SoC, GPU line = the Vulkan device, refresh = the display's current rate, GPU busy from the Adreno driver
  when readable, else "n/a".
- **Pause, Minimize, Resume (A6):** Back pauses (EE parked, card files written, audio paused) and asks Keep playing /
  Minimize / Close game; Minimize keeps the game process alive and paused in its own task, and the launcher's Play reads
  Resume while that process lives. Leaving the foreground (Home, recents, shade, Settings, screen off) parks the game and
  releases its swapchain and surface through the game thread before the window is destroyed (GameActivity.onPause waits
  for it); the new surface is attached on return once it has a real size (the owner's black screen after Settings).
- **Provider references:** the disc document's ParcelFileDescriptor is closed right after `detachFd()` in both
  processes (the owner's launcher was killed twice with "dependency died ... ExternalStorageProvider").
- **Shader prepare before Play,** as on Windows: ShaderPrepareService in its own `:shaders` process runs libgt4's
  `--prepare-shaders` path with the session's environment; the launcher shows the same card and runs the same attempts
  and guard rules (300 variants, 198 s on the emulator's software Vulkan).
- **Diagnostics:** threads named gt4-ee/gs/vu1/present; cluster shares, frequencies and thermal status in F8 and the HUD
  log; Export logs carries 10 game sessions on Android; logs mirrored to the external folder; `debug.env` for remote
  variables.
- **TASK-182 diagnostics merged** (`task/182-barrier-scope` at 6f0e4f10): with `GT4_CORE2_BARRIER_TRACE=1` and
  `GT4_CORE2_GPU_CATEGORIES=1` in `debug.env`, the Android game log carries the `[core2] barriers by reason`,
  `[core2] sync vblank=...` and `[core2] gpu categories ms/vblank ...` lines every 600 VBlanks (checked in the emulator;
  GPU timestamps work there).
- **Emulator limits (not phone findings):** Lavapipe keeps no compiled pipelines in its cache data (`pipelines.bin` is a
  header, "0 KiB"), so every session there recompiles the 300 known variants in the background (about 150 s under
  translation, uber shader meanwhile); on the phone check `[core2] shader cache: N KiB` (should be large after a prepare)
  and a small `compile_ms` in the second session. The first `[core2] ms/vblank` line of an emulator session can show
  huge stage times (a counter read across translated cores; Hypothesis), later lines are normal.

## Test APK 3: `1.3.0-android-test3` (TASK-180, 2026-11-05)

From the owner's S23 Ultra test of test2 (2026-11-05 13:54). The APK is arm64-v8a, versionCode 3, signed with the same
release key (certificate SHA-256 `bed20009...79bd7`), `zipalign -c -P 16` clean, and lives in `private/android-test3/`
with its `.sha256` and `symbols/`. It is 117,924,022 bytes (test2: 117,513,418; the difference is the code, not a second
game library).

- **The controller in the game ([input]):** the pad path itself works (the emulator's gamepad moves the menus), so the
  owner's pad is not being opened by the game. `game.log` now carries an `[input]` block at the first VBlank and on every
  device change: every device SDL sees (name, VID:PID, GUID, gamepad or joystick, the mapping found, its axes and
  buttons), which one the game reads, and the window's input focus (the pad reads neutral without it). A pad that Android
  reports as keys and motion but SDL never lists as a gamepad is now read anyway: `GameActivity` forwards its key and
  motion events to `libgt4.so` (`nativePadKey`/`nativePadMotion`), and the game uses that state only while SDL has no
  gamepad open, so nothing is ever read twice. Coming back from the background re-reads the window's focus.
- **Wheel page:** Android offers pads only (owner). The Wheel page, its tab and its commands are not offered there
  (`Platform::offers_wheel_page`, `app_state.wheel`), and a `GT4_BENCH_GAME=spec2` bench is refused rather than starting
  the wrong library. Windows is unchanged.
- **Pad state for the launcher's commands:** the Android app builder now manages `pad_service::Handle` (it was only
  managed on Windows, which is the "state not managed for fields 'pads'" error), and both builders log a start-up check
  of every state the commands ask for (`managed_states`, with a source-scanning test that keeps the list in step).
- **Bench mode (`debug.env`):** `GT4_BENCH_CARD` (an absolute `.ps2` under the app's external files folder; anything else
  stops Play with the reason in the log) is copied into the session folder as `bench-card.ps2` and passed as `--memcard`;
  the player's card is not written, backed up or synced, the copy is deleted when the game ends (and a stale one at the
  next start), and both logs say so ("bench: ..."). `GT4_BENCH_GAME=gt4|spec2` picks the game (Spec II refuses: no
  library in this APK). The game logs a `[bench]` line when `GT4_INPUT_REPLAY` drives the pad.
- **Render scale is a percentage of the screen height, on every platform:** `display::percent_for_multiplier` (1x = 448
  px of the screen's height: 31% at 1440, 62% at 2x) and `multiplier_for_percent`; the row is one position per multiplier
  ("31% · 1x · 640x448"), so label, positions and the stored value are one formula. Settings schema 9 marks a file written
  under the old notch model and the value is converted once: Windows keeps the multiplier the old value meant, Android
  goes to the 2x default (test1's "125" was 6x on the S23 and the menus ran at 12 fps). On the emulator 25% → 83% (2x).
- **Shader prepare:** nothing has the focus in its card (Cancel took it, so one A press cancelled the prepare).
- **Pause dialog:** centred, in the launcher's language (the Android `os_locale` reads `GT4_ANDROID_LOCALE`, set by
  MainActivity from the device locale: "Auto" used to fall back to English while the page was Portuguese), and
  controller-navigable with Keep playing focused.
- **Version:** `1.3.0-android-test3` from `GT4_RELEASE_LABEL` (accepted as `rc<N>` or `android-test<N>`), shown by the
  footer, the logs, and Android's own `versionName`.
- **Windows:** the home page preselects Play there too (owner).
- **One app (test3 follow-up, owner):** the game Activity runs in the launcher's own task (`GameActivity` is
  `singleTop` with no task affinity, a black theme and no window animation), so recents shows one card. The app icon
  enters through a windowless `LaunchGateActivity` that starts the running game when there is one (a `singleTask`
  launcher would reset the task and destroy the game; a `singleTop` one would put a second launcher on top of it), and
  the launcher otherwise. Home or the app switcher leave the paused game on top of the task, so the icon and the
  recents card land back in it (surface rebuilt, audio resumed); Minimize brings the launcher to the front of the same
  task with the game alive below, Resume brings the game back, and Back on the launcher leaves the app in the background
  instead of finishing the task under a live game. Close game still ends the Activity and its process. Checked in the
  emulator: Play, Home, icon → game, recents → game, Back → pause dialog, Minimize → launcher ("Retomar"), Resume → game,
  Close → launcher ("Jogar"), Play → a new session.

## Test APK 4: `1.3.0-android-test4` (TASK-180, 2026-11-05)

From the owner's S23 test of test3: the controller and the Render scale are fixed, and the **Performance report came back
missing** (its Settings row and the in-game F7/F8 touch button). That was a launcher regression, not a game one, and this
build's first fix.

- **Why it vanished.** TASK-118 keeps the release-candidate rows (the performance report and the VU code check) out of
  final builds: `model.rs` drops them and `state.rs` forces the report off unless the launcher version contains `-rc`.
  Test builds are labelled `1.3.0-android-testN`, which has no `-rc`, so test3 was read as a final build; the game was
  never given `GT4_PERF=1`, so `GameActivity` never added the touch button. `gt4l_core::is_test_build` now accepts `-rc`
  and `-android-test`, and `test_build()` drives both places; an Android test build turns the report on unless the player
  chose a value (a chosen "off" is kept; a final build still forces it off).
- **The touch button** moved from the middle of the left edge (over the owner's front camera) to the **top-left corner**,
  adding the window insets (display cutout and system bars) to its margin on every change and once attached, so it never
  sits under a camera hole or the status bar. Same 44 dp faint circle, same F7/F8 taps, never focusable.
- **The exact TASK-182 savings are Android defaults** now: `GT4_CORE2_DEINT_FIELD_COMPOSE=1` and
  `GT4_CORE2_DEINT_SKIP_DECISION=1` in the launcher's game environment (merged before `debug.env`, so a diagnosis build can
  still override them), logged by the game as `[core2] deinterlace order (TASK-182) ...`; Windows keeps them off. The
  branch also carries `task/182-barrier-scope` at 8d25d4e4 (steps 2a-2c).
- **Single-queue present on Android:** `GT4_HW_PRESENT_QUEUE=0`, the rendering-queue path: the picture is drawn into the
  acquired swapchain image instead of a window-sized picture plus a second copy, about 36 MB less per presented frame at
  3088x1440. Presentation only.
- **Unified-memory phase 1** (STUDY-UNIFIED-MEMORY 7.2): memory shape with the `GT4_HW_MEMORY_SHAPE` override, the shape
  and every memory type in the `[hw] device profile` line, a `[core2] placement` line, per-path byte counters and ring
  high-water marks in `[core2] data paths/vblank`, and the spec-required host-read barrier (`GT4_CORE2_HOST_READ_BARRIER`,
  on by default). Diagnostics and one exact barrier: no picture changes, covered by `hw_core2_data_paths` and the extended
  `hw_device_profile`.
- **Still open:** unified-memory phase 2 (phase 2 of the study: the final picture drawn straight into the swapchain image
  with pre-rotation, `GT4_HW_PRESENT_DIRECT`) and Spec II on Android (below).

## Test APK 5: `1.3.0-android-test5` (TASK-180, 2026-11-06)

The owner paused the CoreGS 2.0 optimisation work on 2026-11-06; this build keeps the exact TASK-182 savings and delivers
the Performance Mode split and Spec II.

- **TASK-182 step 3** (`task/182-barrier-scope` at 54d2a0f4): newest-fence readback wait (on), bounded spin (off), K rule
  device default (8 on a tiler or unified-memory GPU, 0 on a discrete one); Android sets `GT4_CORE2_SUBMIT_EVERY=128`
  (S23 bench +11.6% VBlanks/s, 32% fewer render passes) and K is clamped to it.
- **Performance Mode replaced by independent effect toggles**, on by default, on the Graphics page for both renderers:
  - *Sun glare and light flares* (one toggle): the 64 x 8 visibility table is answered as zeros, so neither is drawn and
    the readback wait is gone. One table feeds both (IN-441), so a split would need a guess.
  - *Road sheen* (CoreGS 2.0 only): the full-screen pass and its alpha-lane fills are left out; the GSdx path has no batch
    classifier on the game side, so the row is not offered there.
  - Switches `GT4_CORE2_PERF_FLARES` / `GT4_CORE2_PERF_ROAD_SHEEN` (1 = off); `GT4_CORE2_PERFORMANCE=1` still turns all
    off; the GSdx path's `GT4_GS_HIDE_FLARES` is set from the flare toggle and the stream client also accepts the new name.
    gt4-gs's fast download mode is written only when the flares are off.
- **Spec II:** hooked unit set regenerated (`scripts/camera360_units.py spec2`), `libgt4spec2.so` for arm64, both
  libraries packaged, the game Activity loads the chosen game's library, and Play/bench use the game's own ELF. The
  emulator boot test is pending the pinned ISO (see below).

### Spec II on Android: what a later build needs

**Status in test5:** steps 1-3 are done and step 4 is pending the disc image.

1. **Done (test5).** The hooked Spec II unit set was regenerated with `scripts/camera360_units.py spec2` from
   `private/slice405-spec2-aspect-producer/generated` into `build/android-inputs/spec2-units-rc14b-hooked` (348 units, the
   manifest's source inventory), because the pre-hook copy fails the manifest's `sha256_after` pins.
2. **Done.** `libgt4spec2.so` links for `arm64-v8a` from `android/native/configure-android-spec2.cmd` (with
   `GT4_EE_UNITS_DIR` pointed at the hooked set), and `android/stage-native-libs.cmd` stages both libraries into `jniLibs`
   (`libgt4.so` and `libgt4spec2.so`), each stripped of line tables but keeping its symbol table.
3. **Done.** The launcher passes `GAME=gt4|spec2`, `GameActivity.getLibraries()` loads `gt4spec2` for Spec II (and logs
   it), Play uses the game's own ELF and the library name, and a `GT4_BENCH_GAME=spec2` bench is allowed. The disc, card,
   saves and settings were already per game.
4. **Pending.** The emulator check that Spec II reaches its title screen at 1x needs the pinned image
   (`docs/inputs/spec2-iso.json`, sha256 `6a3484b9…`); this PC holds two PTBR Spec II builds (`fe8b51cf…`, `96557472…`)
   that the identity check refuses by design. On a phone, Play installs Spec II through the same Storage Access Framework
   pick as v2.00.

The original checklist, kept for the record, was:

1. **The hooked Spec II unit set.** The translated Spec II units in `private/slice405-spec2-aspect-producer/generated` are
   the pre-hook set, so the build stops on the manifest's `sha256_after` pins. Regenerate them with
   `private/tooling-venv/Scripts/python.exe scripts/camera360_units.py spec2 <translated units folder> <fresh output>`
   (the script refuses an existing output folder and pins the translation hashes), then point `GT4_EE_UNITS_DIR` at it.
2. Build `libgt4spec2.so` for `arm64-v8a` with that script and stage both libraries in `jniLibs` (`stage-native-libs.cmd`).
3. The game Activity picking the library by game (`getLibraries()` returning `gt4spec2` for Spec II) and the launcher's
   install/Play path for Spec II on Android (its own disc, ELF, memory card and settings, as v2.00 has).
4. The emulator check that Spec II reaches its title screen at 1x, and the APK size with both libraries.

## Subtle risks (Android), ranked

1. The no-interlock CoreGS path is slow or misordered on tilers; D1 may be required before playable speed.
2. Desktop-tuned "integrated GPU" defaults switching on by accident on every phone.
3. Strict drivers misdrawing because of features used without being enabled.
4. Landscape picture rotated or squashed by the unrotated `preTransform`.
5. FP contraction or bionic libm changing game results silently (identity test is the guard).
6. Weak memory ordering exposing a missing acquire/release in the GS or VU hand-offs.
7. EE thread stack overflow on arm64 in very large translated functions.
8. Low-memory kills from 400 MiB of fixed rings plus upscaled images on 6-8 GB phones.
9. Progress lost when Android kills the backgrounded game before the card reaches storage.
10. Long first-run pipeline compiles hitting driver watchdogs; caches lost after driver updates.
11. SAF quirks: cloud providers without seekable fds, revoked permissions, FAT32 limits.
12. Thermal throttling turning a 60 FPS race into 45 after ten minutes.
13. 16 KB page devices refusing to load a library built with 4 KB alignment.
14. Store policy for disc-image-based games.

## What can be done now on Windows without breaking the "PC first" rule

Correctness and portability work that helps the PC too: `-ffp-contract=off` by default, the missing Vulkan feature
enables, dropping vestigial format requirements, budget-sized rings, the directory fsync for cards, the IOP `strtol` clamp,
the platform seam (L0), keeping new code free of Win32 and x86-only constructs. No mobile-only renderer paths until the
owner starts Phase 4. The feature enables, the format requirements, the budget-sized rings, the rotation-safe present and
the profile structure are done (stage A1, TASK-179).

## Owner decisions needed

Decided (owner, 2026-11-04):
- **First test device and target:** the owner's Samsung Galaxy S23 Ultra (Snapdragon 8 Gen 2, Adreno 740).
- **Goal on it:** at least 2x internal scale in races, 3x if possible.
- **6x is not an Android target.**

Still open: target device floor (for example Snapdragon 8 Gen 1 / Dimensity 9000 / Exynos 2200 class and newer), whether
Spec II and v2.00 ship together, touch controls scope, Tauri or native launcher, GitHub APK only or Play Store too, and
which test devices besides the S23 Ultra (at least one Mali).

## Private GPL test build: the ARMSX2 GS beside CoreGS 2.0 (private/armsx2-gs-s23, 2026-11-05)

**PRIVATE, PCSX2-tainted, GPL-3.0+, never distributed and never merged.** The clone `gt4-private-armsx2gs` (push disabled)
carries a third GS renderer for the Android app: ARMSX2's GS (commit `1049db37fb`) running inside the game process, fed
by the same GS stream the game sends to `gt4-gs.exe` on Windows, through an in-process queue (no IPC). Document:
`docs/ARMSX2-GS-PRIVATE.md`.

- **Selection:** `GT4_RENDERER=armsx2` (`native/gs_stream/armsx2_gs_sink.cpp` loads `libgt4gs.so` at run time and fills the
  existing in-process sink table). The launcher's Renderer row on Android offers CoreGS 2.0 (default) and
  "ARMSX2 GS (experimental, private build)"; a new `armsx2_scale` row gives 1x, 1.25x, 1.5x, 1.75x, 2x, 2.5x, 3x, 4x, 5x
  and 6x.
- **Settings:** `armsx2-gs.ini` written on every Play from the same GS table as a Windows gt4-gs session (blending,
  aspect 16:9, the GT4 display crop, OSD off, texture loading only), with the chosen multiplier and
  `ExtendedUpscalingMultipliers = true` for a fractional one; the owner's own lines go into `armsx2-gs-extra.ini` beside
  it, applied on top. Both in the app's external files folder, under `armsx2gs/`.
- **Back end:** OpenGL ES (ARMSX2's Android default). The Vulkan back end needs shaderc's runtime GLSL compiler, whose
  glslang/SPIRV-Tools sources are not in the local ARMSX2 clone and were not downloaded; the library reports
  `gt4gs_has_vulkan() == 0`.
- **APK:** `1.3.0-android-test4-armsx2gs-private`, versionCode 4, the owner's release key, in the main checkout's
  `private/android-private-armsx2gs/` (sha256 `645efd4679d307f83d60ae66b56441ff940bd0abf70fdc913c75ebc3d41b6fc5`,
  143,684,202 bytes).
- **Emulator evidence:** the app installs and updates, the launcher shows the new rows, Play with ARMSX2 writes the two
  files, starts the game with `GT4_RENDERER=armsx2`, the game loads `libgt4gs.so`, the ARMSX2 core boots as a GS-only VM
  (serial `SCUS-97328`, GameDB GS fixes applied), the window is attached and an EGL context is created — then the GLES
  backend refuses: the emulator's guest GL is SwiftShader **ES 3.0** and ARMSX2 needs 3.1 (`uvec2`). CoreGS 2.0 still
  runs in the same APK.
- **Owner's S23 test (2026-11-06): black screen, fixed.** His log shows the Adreno 740 device creating correctly (ES 3.2,
  Adreno profile, uber shaders compiled) and dying on `GL: Failed to read fxaa.fs`: the staging copied only
  `shaders/opengl/*.glsl`, so `shaders/common/fxaa.fx` (and the FidelityFX headers) never reached the phone. The staging
  now ships the whole `shaders` tree (33 files) and the app extracts it on Play. GLES on Adreno was not the cause; a
  Vulkan build would not have avoided this one. The same APK hides the CoreGS 2.0 rows while ARMSX2 is chosen and has
  them translated in pt-BR.
- **Second S23 run (2026-11-06): the GS thread aborted on the first frame.** With the shaders in place the device opened
  on the Adreno and the stream started, then the per-frame dump-replay overlay read a null dump and aborted (three
  identical tombstones). The overlay and the end-of-present UI block are skipped while the stream runs (this library has
  no UI), and the overlay itself returns when there is no dump. Verified in the emulator with a deviceless renderer
  (`GT4_ARMSX2_GS_RENDERER=null`): 2,700+ VSyncs processed, no abort. Rebuilt APK: sha256
  `8f9d10607f52ad026fbde0da531249514d99ab7376f1f3c40daa77d11a4c49ae`.

## Android test4: the Performance report back, TASK-182 savings on, UMA phase 1 (TASK-180, 2026-11-05 evening)

`1.3.0-android-test4` (arm64, versionCode 4, same release key) answers the owner's test3 findings.

- **Performance report (regression fixed).** TASK-118 hid its Settings row and forced it off in every build whose version
  lacks "-rc", and test builds are labelled `1.3.0-android-testN`; `gt4l_core::is_test_build` now accepts both test labels,
  an Android test build comes with the report ON unless the player chose a value, and the in-game touch button moved from
  the middle of the left edge (over the owner's front camera) to the top-left corner, offset by the window insets (display
  cutout and system bars). Tests: core version rule, model row on Windows and Android for test builds and not in a final
  build, focus-check case for the toggle.
- **The exact TASK-182 savings on by default on Android:** `GT4_CORE2_DEINT_FIELD_COMPOSE=1` and
  `GT4_CORE2_DEINT_SKIP_DECISION=1` from the launcher's game environment (overridable by `debug.env`, logged by the game);
  Windows unchanged. `task/182-barrier-scope` at 8d25d4e4 is merged (steps 2a-2c).
- **Single-queue present on Android:** `GT4_HW_PRESENT_QUEUE=0`, about 36 MB less per presented frame at 3088x1440;
  presentation only.
- **Unified-memory phase 1** (STUDY-UNIFIED-MEMORY 7.2, P1-A to P1-D): memory shape with `GT4_HW_MEMORY_SHAPE`, the shape
  and every memory type in the `[hw] device profile` line, a `[core2] placement` line, per-path byte counters with ring
  high-water marks in `[core2] data paths/vblank`, and the spec-required host-read barrier
  (`GT4_CORE2_HOST_READ_BARRIER`, on by default). No rendering change; `hw_core2_data_paths` and the extended
  `hw_device_profile` cover it, the `hw_*` CTest suite is green (176 passed, 1 skipped as designed).
- **Not in this build:** unified-memory phase 2 (the picture drawn straight into the swapchain image with pre-rotation) and
  Spec II on Android (the hooked unit set, `libgt4spec2.so`, both libraries, the Activity choosing by game, the launcher's
  install and Play path).

## Android test3: one app (TASK-180 follow-up, 2026-11-05 evening)

The owner asked for the launcher and the game to feel like one app. The game Activity now runs in the launcher's own task
(`singleTop`, no task affinity, a black theme, no window animation), so recents shows one card with the game's own picture,
and the app icon enters through a windowless `LaunchGateActivity` that starts the running game when there is one (or the
launcher) and finishes at once — the two ways the launcher itself could be the icon's target either reset the task and
destroy the game (`singleTask`) or put a second launcher on top of it (`singleTop`). Home and the app switcher leave the
paused game on top, so the icon and the recents card land back in it (surface rebuilt, audio resumed); Minimize brings the
launcher to the front of that task with the game alive below and Resume takes it back, with Resume focused after the
WebView settles; Back on the launcher leaves the app in the background instead of finishing the task under a live game;
Close game still ends the Activity and its process. Verified in the emulator step by step (screenshots and the launcher log's
`game start`/`resume`/`game process gone` lines): Play, Home, icon → game, recents → game, Back → pt-BR pause dialog,
Minimize → launcher (Retomar focused), Resume → game, Close → launcher (Jogar focused), Play → a new session. The APK in
`private/android-test3/` was rebuilt with it (sha256 `8a776323…bbc`, 117,924,214 bytes, same key, `zipalign -P 16` clean);
launcher tests 291 + 142 (the flow_audit flake passed with no game process running) and the focus checks pass in both
languages. Spec II is still groundwork only (see the Android plan for the regenerated hooked unit set it needs).

## Android test3 (TASK-180, 2026-11-05 afternoon)

`1.3.0-android-test3` (arm64, versionCode 3, same release key, `zipalign -c -P 16` clean) is built from
`task/180-android-emulator` and kept in `private/android-test3/` with its `.sha256` and `symbols/`; it is 117,924,022 bytes.
It answers the owner's test2 findings: the game's `[input]` block names every device SDL sees and the one the game reads, and
a pad Android reports only as keys and motion is now read through `GameActivity` (forwarded to `libgt4.so`); the Wheel page is
off on Android (pads only); the launcher's pad state is managed on Android (the "state not managed for fields 'pads'" error)
with a start-up check of every command state; bench mode from `debug.env` (`GT4_BENCH_CARD`/`GT4_BENCH_GAME`: the recording's
own card copy, the player's card untouched, the copy removed when the game ends); the Render scale is a percentage of the
screen height on both platforms, with a schema 9 migration (Windows keeps the picture, Android goes to 2x); the shader
prepare card focuses nothing; the pause dialog is centred, localised through the device locale and pad-driven; the version
comes from `GT4_RELEASE_LABEL`; Windows preselects Play too. Checks: CTest 320/320 (1 skipped), launcher tests 291 + suites
passing except the known `flow_audit::a_1_2_x_install_moves...` flake while another game process ran, focus checks pass in
en-US and pt-BR, locale keys 917 in both. Spec II on Android is groundwork only (`GT4_TARGET=spec2` builds
`libgt4spec2.so`; the hooked unit set must be regenerated first), so the APK is still v2.00 only.

# Current Status

## 1.3.0-rc15 test build as a GitHub draft (2026-11-05, morning)

Integrated on `release/1.3.0-rc15` (merge commits, in order): `origin/main` (PR #205, the native DFP wheel of WaimateWihongi,
and README), TASK-170b and TASK-170c (shader prepare retry, two-crash quarantine), TASK-174 and TASK-174b (presentation off the
GS thread on one-queue GPUs, the launcher option, a richer `system.txt`), TASK-175 (performance HUD Off / FPS only / Full),
TASK-179 (Vulkan correctness, rotation-safe present), TASK-168b (palette segments off by default; tone-run early drop and one
CLUT walk on), TASK-178 at c73b3be8 (K=0, counters) and TASK-181 (the launcher Wheel page and the DFP fixes, USB capsule
slice409). Not in it: TASK-180 (Android). Conflicts resolved by keeping every feature (`log_bundle.rs`, `launch.rs`,
`standalone.cpp`, `hw_gs_backend.cpp`, the writeback handles, `core2_gpu.cpp/.hpp`, the vertex shader builds and the CTest
lists); TASK-179's point-quad vertex stage now also carries TASK-168's palette row. The three pt-BR texts of TASK-174b's option
were mojibake and are repaired.

Games slice410, plain (no PGO, no ThinLTO), the slice407 EE libraries plus the USB capsule (pins `docs/inputs/slice410-*.json`,
exe `7060f8bf...` v2.00, `a4d84180...` Spec II; distribution `private/slice410-dist-130-rc15`). Gates against the rc14 PGO
builds (`gates6.sh`, wrong=0): race clock, simulation state and every compared frame identical on three Spec II laps (24, 35,
35 frames), the v2.00 night race (10/10) and Arcade path (14/14) at 3x and Spec II at 6x (12/12); VU1 236,448 and 355,388
samples and MTVU 8,032 frames with 0 differences. CTest 318/318 on both games (q07 skipped), Python 802 (57 skipped), launcher
`cargo test` 750 passed (9 ignored), focus checks 357/357 and 364/364, nav-test 260/260, locale keys 909 in both.
Launchers labelled rc15 (`private/slice410-launchers-rc15`), installer `20b438e0...` and portable zip `8534510f...`
(`private/release-130-rc15-out`; contents equal to their sources). Uploaded as the GitHub **draft** `v1.3.0-rc15` (not
published) for the owner's G27 test; a pre-release and PGO only on his word.

## 1.3.0-rc14: closing as a pre-release (2026-11-04, afternoon)

On top of the slice407 test build: the full trigger range on by default with a fixed-width meter (TASK-161), the shader
compile guard and `shaders.log` in the log bundle (TASK-170), and Spec II images accepted by their code (TASK-173). Games slice408 (PGO and ThinLTO from `build/cam360/*-units-rc14b`;
pins `docs/inputs/slice408-*.json`, exe `a4e1c742...` v2.00, `e5267b8f...` Spec II; distribution
`private/slice408-dist-130-rc14`), launchers labelled rc14 (`private/slice408-launchers-rc14`). Gates against rc13 identical
(race clock, simulation state, every compared frame, VU1/VU0 and MTVU 0 differences); CTest 301/301 on both games, Python 790,
launcher 665, focus checks 294/294 and 301/301. Installer `d8ac9593...`, portable zip `961047fc...`
(`private/release-130-rc14-out`; contents checked against the distribution and the launchers). Published as the pre-release
`v1.3.0-rc14` (the owner's word given). Next: TASK-171 (the first license on laptops) for rc15. Recorded: TASK-172 (an earlier
Spec II release needs its own build).

## TASK-095: 2D overlays over the race view at ultrawide ratios, both games (2026-11-04)

Branch `task/095-ultrawide-menu-canvas` (re-does PR #177, Luan Antony co-author); `docs/ULTRAWIDE-OVERLAYS.md`. Pre-race screens,
pause, pit menu and results keep their 16:9 shape, centred, at 21:9 and 32:9; 16:9 identical. Owner test pending.

## 1.3.0-rc14 test build on the owner's desktop and laptop (2026-11-04, morning)

For the owner's test before any pre-release (no PGO until the owner asks). Contents: TASK-095 (PR #200, re-does Luan Antony's
#177: the 2D overlays over the race view keep their 16:9 shape at 21:9 and 32:9), TASK-159b, TASK-160 and TASK-161 (PR #197: the
camera's pedal zeroing only on the right stick Y, FoV zoom on R3 + D-pad on by default, full trigger range off by default with a
launcher switch), TASK-164 (first-start note on Play, issue #160), TASK-166 (Spec II ultrawide: the race font x scale and the
mirror divisor patched as on v2.00, `scripts/spec2_aspect_patch.py`, units `private/slice405-spec2-aspect-producer`) and TASK-167
(the Spec II PT-BR + CPSP image named). Games slice407, plain (pins `docs/inputs/slice407-*.json`, exe `4e87eb83...` v2.00,
`10b7ce52...` Spec II, EE libraries from `build/cam360/*-units-rc14b`; distribution `private/slice407-dist-130-rc14-test`),
launchers labelled rc14 (`private/slice407-launchers-rc14-test`), installer
`private/slice407-installer-130-rc14-test/GT4Recomp-Setup-1.3.0-rc14.exe`. Gates against rc13 (`gates5.sh`, wrong=0): race clock,
simulation state and every compared frame identical on three Spec II laps, the v2.00 night race and Arcade path, Spec II at 6x;
VU1/VU0 and MTVU checks 0 differences. TASK-095 overlay check on Spec II `george-pausa`: 16:9 identical to the build without the
overlay calls (clock, state, 121 of 121 frames); at 21:9 the race identical and only the lobby and pause frames differ, 5 of 5
narrowed by 1/1.3125 around the centre. CTest 300/300 in both builds, Python 785/785 (after the TASK-118 test update), launcher
tests 662, focus checks 293/293 and 300/300, nav-test 217/217, locale keys 695 in both. Desktop updated through the installer's
update mode (backups `private/_program-backups/rc13test-before-rc14test-20261104T120226Z` and
`rc14test-before-rc14b-20261104T134327Z`), the laptop's portable copy in place (backups `avell-rc13test-before-rc14test-...`,
`avell-rc14test-before-rc14b-...`). The slice406 build (the same without TASK-095) was installed first and is superseded.

## 1.3.0-rc13 published as a pre-release (2026-11-04)

Tag `v1.3.0-rc13`. Contents: TASK-149 (Nordschleife fences), TASK-150 (showroom: large writes merged on the GPU, IN-538),
TASK-151 (photo mode flicker), TASK-152 to TASK-158 (contributor pull requests re-done, Luan Antony co-author) and TASK-159 (360
chase camera on the right stick, on by default, PR #194). Games slice404 (PGO and ThinLTO from the camera's hooked units; pins
`docs/inputs/slice404-*.json`, exe `901f997e...` v2.00, `10cb28b5...` Spec II; distribution `private/slice404-dist-130-rc13`);
launchers labelled rc13 (`private/slice404-launchers-rc13`); installer `e96ca3e1...`, portable zip `742ac156...`
(`private/release-130-rc13-out`). Gates against rc12 (`gates4.sh`): race clock and simulation state identical on three Spec II
laps and the v2.00 night race, every compared frame identical, VU1/VU0 and MTVU checks 0 differences. Launcher: locale keys 687 in
both languages, focus checks 278/278 and 285/285, nav-test 209/209. Laptop (RTX 3060, 6x): showroom 60.0 FPS, photo mode
59.4-60.0 FPS. Not in it, off by default after measurement: IN-537 and IN-539 (photo mode frame times, TASK-162).

## 1.3.0-rc13 test build on the owner's desktop and laptop (2026-11-04, night)

For the owner's test suite before any pre-release. Contents: TASK-149 (an upload over a mip level draws the pending batch
first: the Nordschleife's far fences), TASK-151 (the display prefers the circuit's width: photo mode no longer black every
other picture), and the contributor pull requests re-done by hand with Luan Antony as co-author: TASK-152 (#120 PCSX2 cards),
TASK-153 (#116 delete the active save), TASK-154 (#121 ReShade's Home key with GSdx), TASK-155 (#136 portable.ini beside
gt4-gs), TASK-156 (#141 Wine audio profile, `GT4_AUDIO_LATENCY_MS`), TASK-157 (#145 custom textures folder), TASK-158 (#143
GSdx retry and install from a file). Each PR recorded as merged on `main` (tree-neutral) and its issue answered.

Games slice403, plain (pins `docs/inputs/slice403-*.json`, exe `b2f12a10...` v2.00, `d16acc88...` Spec II; distribution
`private/slice403-dist-130-rc13-test`), launchers labelled rc13 (`private/slice403-launchers-rc13-test`), installer
`private/slice403-installer-130-rc13-test/GT4Recomp-Setup-1.3.0-rc13.exe`. Gates against rc12 (`gates4.sh`): race clock,
simulation state and every compared frame identical, VU1/VU0 and MTVU checks 0 differences. Launcher tests 659, focus checks
278/285, nav-test 209, locale keys 687 in both. Desktop updated through the installer's update mode (rc12 test files in
`private/_program-backups/rc12test-before-rc13test-20261104T064835Z`); the laptop's portable copy updated in place (backup
`avell-rc12test-before-rc13test-20261104T064859Z`). Still open for rc13: TASK-150 (showroom frame times) and TASK-151's frame
times: both scenes read GS memory back (the showroom every other frame, photo mode every frame) and wait for the GPU.

## TASK-161: full trigger range for the throttle and the brake, both games, off by default (2026-11-04)

Branch `task/161-trigger-sensitivity`; `docs/research/TRIGGER-SENSITIVITY.md`. Owner test decides the default.

## TASK-160: field of view zoom on R3 + D-pad, both games, on by default, checked headless (2026-11-04)

Branch `task/160-fov-adjustment`; `docs/FOV-ADJUST.md`. Owner test pending.

## TASK-159: 360-degree chase camera on the right stick, both games checked headless, on by default (2026-11-04)

On `task/159-360-chase-camera` (from task/134 at `ad457090`). On by default in v2.00 and Spec II (owner, 2026-11-04; in rc13):
`GT4_CAMERA_360=0` or `camera_360=0` in `game.ini` turns it off.
The right stick X turns the chase camera round the car through the game's own camera spring (a host call that
`scripts/camera360_units.py` adds to the translated yaw call), and the right stick Y stops being the analog throttle and brake
while it is on. Headless v2.00 Arcade race (plain test build): switch off identical to the build without the hook (frames,
race clock, state); switch on: race clock identical at every VBlank, simulation identical once the stick is released (only the
camera's own words differ while it is held), the view turns and comes back; with the stick at rest, identical. Spec II, the
owner's Tokyo R246 Time Trial with the stick moved: the same results (the simulation words that differ after release are the
camera's and the ones drawing writes). Owner test in game pending. `docs/CAMERA-360.md`.

## 1.3.0-rc12 built with PGO and published as a pre-release (2026-11-04, night)

On the owner's word ("a rc12 atual assim que buildar, vc já sobe como pre-release"): TASK-145 to TASK-148 on top of rc11. Games
slice402 (PGO and ThinLTO, `private/task134-tools/final12.sh`; exe `3c8f870c...` v2.00, `9b3c4d38...` Spec II; pins
`docs/inputs/slice402-*.json`; distribution `private/slice402-dist-130-rc12`), launchers labelled rc12 from `e562048f`
(`private/release-130-rc12-launchers`), package `private/release-130-rc12-out` (installer `28b6776b...` 51,870,003 bytes, portable
zip `0932edb7...` 80,813,207 bytes, SHA256SUMS), checked against its sources by extraction. Gates against rc11
(`private/task134-tools/gates3.sh`): race clock and simulation state identical on three Spec II laps and the v2.00 night race,
every compared frame identical, VU1/VU0 and MTVU checks 0 differences. Locale keys 664 in both languages, focus checks 275/282
and nav-test 209 on this launcher's UI. Tag `v1.3.0-rc12` at `e562048f` (the launcher's source; the game sources are those of
the build).

## TASK-147 and TASK-148 for rc12: triggers, sticks and the controller choice (2026-11-03, night)

On `task/134-game-code-optimization-study`, plain test builds on the owner's laptop. Triggers: R2 spans GT4's own 0..45
throttle range and L2 its 0..84 brake range (the game scales the pressure byte and reached 100% at a third of the travel);
the owner approved them. Sticks: a 10% dead zone, and the bytes step over GT4's own 29% stick dead zone, so steering is
proportional from the first step (it was dead up to about 46% of the travel, then too steep). Controller choice: a dropdown
on the Controls page (`controller` setting, `GT4_PAD`); the game reads the chosen pad, else one of its model, else
controller 1. Owner change (2026-11-04): no "Automatic" entry; the dropdown lists only the connected controllers and the
launcher stores the one it reads when the stored one is not connected (checked again at every launcher start). TASK-147 is
done (owner, 2026-11-04: control precision solved); issues #183, #134 and #178 are closed as fixed for rc12.

GitHub issues (2026-11-04, owner's request to close what is fixed): 11 closed with a note each. For rc12: #183, #134, #178.
Fixed in published pre-releases: #191 (GT Auto bubbles, rc10), #185 (Spec II replays, rc10), #139 and #179 (GSdx missing:
Play offers CoreGS 2.0, rc10), #115 (no sound device, rc10), #123 (toggles with a controller, rc8), #132 (portable build,
rc7), #89 (integrated GPU start, rc5; the reporter races on rc6 in #112). Left open: #99 (flashing window then a hang exit,
the TASK-145 fix) until rc12 is public; the others have no confirmed fix or are feature requests. 25 issues stay open.
Later the same day (owner): TASK-148 done too, #99 closed (rc12 goes out today), and #161, #88 and #128 answered; 24 issues
stay open (`docs/ISSUE-TRIAGE.md`).

Test installer (2026-11-04 UTC): the same plain builds pinned as slice 401 (`docs/inputs/slice401-pcsx2.json`, gt4.exe
`eac9918f`; `slice401-spec2.json`, gt4-spec2.exe `2b1f3202`), distribution `private/slice401-dist-130-rc12-test`, launcher
labelled rc12 (`89164318`), `private/slice401-installer-130-rc12-test/GT4Recomp-Setup-1.3.0-rc12.exe` (`63fcdb23`). Installed
on the owner's desktop through the installer's update mode over rc11 (silent, result ok; installed game files, launcher and
SDL3 match their sources); rc11 program files backed up in
`private/_program-backups/rc11-before-rc12test-20261104T023355Z`. Not uploaded; the rc12 release build (PGO) comes later.
Then the launcher without the "Automatic" controller entry (TASK-148 owner change; launcher `4e0a7e50`, same games):
`private/slice401-installer-130-rc12-test2/GT4Recomp-Setup-1.3.0-rc12.exe` (`cf90c041`), installed the same way (result ok,
files match); the first test install is in `private/_program-backups/rc12test-before-rc12test2-20261104T031129Z`.
The owner's laptop got the same launcher as a portable build (`7e46a64a`, copied over `Y:\GT4Recomp.exe`; its games already
were the slice 401 builds); the previous one is in `private/_program-backups/avell-rc12test-before-rc12test2-20261104T041009Z`.

## TASK-146 for the owner's test: 60 FPS lets a race drop frames instead of slowing (2026-11-03, night)

On the owner's word, experimental in the rc12 line: the launcher's Frame rate "60 FPS" (CoreGS 2.0, the default) now passes
`GT4_FRAME_SKIP=1` (`game.ini` `hw_frame_skip_adaptive=1`), TASK-082's frame skipping when behind. With time to spare nothing
is left out; when the PC falls behind in a running race, frames are left out and the game keeps its full speed and 60 Hz
physics. 30 FPS is unchanged; menus never skip. The first laptop test crashed at a race's end inside the NVIDIA driver: the
paced presentation path (frames left out, the 30 FPS mode too) shared the GS thread's command pool, and the frametime
thread's present wait overlapped the swapchain's acquires and presents in every mode. Both fixed (IN-536; the validation
layer's thread checks: 10 overlaps before, 0 after). Second test: no crash, but 99.1% game speed and audio underruns at 6x.
On the owner's rules (60 Hz simulation always; 30 FPS from the race intro): the gate opens in the intro (race object, clock 0),
the 60 FPS policy acts at a frame of delay and may leave out up to 8 in a row, the pacer owes up to 12 frames. Both frame
skip suites pass; 100.00% at 12x on the desktop. Laptop builds write `frameskip.csv`; owner test pending.

## TASK-145 fixed for rc12: the desktop flash every 6 seconds in fullscreen on NVIDIA RTX (2026-11-03, night)

On `task/134-game-code-optimization-study`, not built as an rc yet (the owner has more for rc12). In borderless fullscreen with a
gamepad the game window's message queue stays empty, and a PeekMessage on an empty queue does not count as a read, so Windows
declared the window "not responding" every few seconds; on RTX PCs the shell and `dwm.exe` then flashed another window or a white
Ghost window in front of the game for a frame (CoreGS 2.0 and GSdx alike), and after Alt+Tab the game could come back white. The
game window now keeps a one-second timer (`native/window_heartbeat.hpp`, decision 0076: keep it, and why). Owner test on the
laptop: a full lap with no hang (before: one every 360 VBlanks), Alt+Tab and GSdx fine. CTest `native_window_heartbeat`, Python
`test_window_heartbeat.py`.

## 1.3.0-rc11 published as a pre-release (2026-11-03, night)

On the owner's word after their test ("tá tudo certo"): `v1.3.0-rc11` at `52645c6b` (the build's source), pre-release, not
Latest; v1.2.5 stays Latest; rc10, rc9 and rc8 stay published. Assets from `private/release-130-rc11-out` (installer
`afb176ef...`, zip `fbdd4abb...`, SHA256SUMS), uploaded as a draft, the three GitHub digests compared with the local files, then
made a pre-release. The owner chose not to wait for a 2026-11-04 tag: on the releases page rc11 sorts by tag text among the
2026-11-03 releases (the launcher compares numerically, so Beta players are offered rc11).

## 1.3.0-rc11 built and installed for the owner's test (2026-11-03, night)

On the owner's word: TASK-142 (IN-535, the R34 rear glass) and TASK-143 (GitHub contributors in About, with Kaezzey; README
credits on `main`; Luan's #118, #119, #154, #174 marked merged by a tree-neutral merge). Games slice400 (exe `7a4c51f1...` v2.00,
`10fd1b88...` Spec II; pins `docs/inputs/slice400-*.json`; distribution `private/slice400-dist-130-rc11`), package
`private/release-130-rc11-out` (installer `afb176ef...` 51,842,164 bytes, zip `fbdd4abb...` 80,762,662 bytes), checked against
its sources. Gates against rc9: clock and state identical everywhere, v2.00 frames identical, Spec II frames differ only on car
glass (identical with the fix switched off); VU1 and MTVU checks 0 differences. CTest 293/294 (python_native_contracts, known),
launcher cargo test 630, locale keys 660 in both, focus checks 275/282, nav-test 209. The installer now has a version resource
and the launcher names its publisher (a laptop's Defender flagged the rc10 installer by machine learning; local scans are clean).
Installed through the installer's update mode; rc10's program files in
`private/_program-backups/rc10b-before-rc11-20261103T213036Z`. Not uploaded. When it is, the tag must point at a commit dated
2026-11-04 (UTC) or later: GitHub lists releases of the same day by tag text, so an rc11 from 2026-11-03 would sort below rc9.

## TASK-142 fixed for rc11: the car in front through a Skyline R34's rear glass (2026-11-03)

On `task/134-game-code-optimization-study`; in 1.3.0-rc11 (above). GT4 clears its environment map with two sprites whose depth buffer
lies in the map's own memory; CoreGS 2.0 kept that depth apart, half of the map kept alpha 0, and the R34's glass reflection (scaled
by the alpha a car's reflection left behind the glass) vanished over the car in front, which stayed sharp and dark. A depth fill
now writes the colour target whose memory it covers (IN-535, `hw_core2_depth_alias`). On the owner's recording at 6x the glass is
misted as on GSdx (8 of 13 frames change, 7 closer to GSdx, 1 equal); Tokyo R246 (17 of 43 frames, all closer to GSdx) and El
Capitan (6 of 43, equal) change only on car glass, by at most 133 pixels. The owner saw the frames: fixed. New diagnostics: `GT4_CORE2_PROBE_DRAWS`/`_RECT`, `GT4_CORE2_SKIP_DRAWS`,
`GT4_CORE2_DEPTH_ALIAS_TRACE`, depth range, tests and TEX0 in the frame trace.

TASK-143 (owner): the launcher's About page credits the GitHub contributors with a merged or open pull request (gustavocanepa,
JulioHeroZ, luanantony), en-US and pt-BR; focus checks, nav-test and launcher tests pass. Release launcher built with label rc11,
not installed and not packaged.

## 1.3.0-rc10 published as a pre-release (2026-11-03)

On the owner's word after their tests ("tudo certo nos meus testes"): `v1.3.0-rc10` at `63bd3321` (no code after `079601ac`, the
build's source), pre-release, not Latest; v1.2.5 stays Latest. Assets from `private/release-130-rc10b-out` (installer `b07b7097...`,
zip `2eef869d...`, SHA256SUMS), uploaded as a draft, the three GitHub digests compared with the local files, then made a
pre-release. Known issue added to the notes and to TASKS (TASK-142, rc11): on CoreGS 2.0 close behind another car, a car's
transparent glass shows parts of the car in front (owner, 6x). Launcher language checks as for the first rc10 build (the UI did
not change since).

## 1.3.0-rc10 rebuilt with the CoreGS 2.0 custom texture fixes (TASK-141), installed, not uploaded (2026-11-03, night)

The owner's test of the first rc10 found two glitches with the HD HUD & UI pack on CoreGS 2.0 (a player saw a third): HUD digits
showing the game's texture for a frame on Tokyo R246, and the GT Auto logo and Oil Change title doubled. Fixed (IN-533, IN-534,
commit `079601ac`; TASK-141) and rc10 rebuilt under the same label: game slice399 (exe `0f0a4b3d...` v2.00, `289396db...` Spec II,
pins `docs/inputs/slice399-*.json`, distribution `private/slice399-dist-130-rc10`), package `private/release-130-rc10b-out`
(installer `b07b7097...` 51,818,176 bytes, zip `2eef869d...` 80,744,000 bytes), checked against its sources. Gates against rc9
all equal (the same set as below). GT Auto recording at 4x and 6x with the pack: the logo and title drawn from the pack in every
frame, no doubled copy. Installed on the owner's desktop through the installer's update mode (files match the manifest); the
first rc10's program files are in `private/_program-backups/rc10a-before-rc10b-20261103T190658Z`. Not uploaded (owner).

## 1.3.0-rc10 built for the owner's test, not uploaded (2026-11-03)

TASK-140 (the player fixes the owner chose from the triage of 47 open PRs and the open issues; ai-memory
`notes/pr-issue-triage-2026-11-03`) and one more TASK-134 step, on `task/134-game-code-optimization-study`. **Owner: do not
publish yet, not even a draft.** Package `private/release-130-rc10-out`: installer `GT4Recomp-Setup-1.3.0-rc10.exe` 51,814,328
bytes `7e90395f...`, portable zip 80,742,596 bytes `fea09567...`; game `private/slice398-dist-130-rc10`, pins `slice398-*.json`
(exe `9b71df8b...` v2.00, `531eeccb...` Spec II).

- **Fixes:** Spec II replays kept (its names are `BASCUS-97436R` plus seven letters and digits; the launcher kept only four digits,
  issue #185); no audio device runs silent (#93, #115); the SPU2 exponential release reaches silence, so GT Auto's bubbles stop
  (#104, capsule `private/slice396-spu2-runtime`); card backups limited to the newest 10 card images, 5 before a New save, 10
  active-save copies (owner: 74 images, about 600 MB, in two weeks); plain exit messages (#107, #99); Play offers CoreGS 2.0 when
  GSdx is not installed (#139, #179); the card error names the file (#160); the 480p hold covers a 1080i save. Ideas of PRs #118,
  #119, #154 and #174 by luanantony, re-done with him as co-author; items 3, 5 and 7 by a Sonnet agent on a merged branch.
- **CPU:** VU1 products with VF0.w emitted as copies (table `private/slice397-vu1-gen`, 20,530 lines, every one that rewrite): VU1
  program -3.4%; whole process in a player's session rc9 9.31 -> rc10 9.26 ms per VBlank (-0.6%, inside the spread).
- **Gates against rc9, all equal:** clock, state and 24/35/35 frames on the three recorded laps; v2.00 night 3x 10/10 and state;
  Arcade 14/14; 6x 12/12; VU1 verify 236,448 and 355,388 samples, MTVU 8,032 frames, 0 differences.
- **Tests:** CTest 292 of 293 (`python_native_contracts`, known); Python 766, the 13 known failures; launcher `cargo test` 630
  with `flow_audit::a_1_2_x_install_moves_into_games_gt4_at_the_first_start` failing under heavy disk load (also on unmodified rc9
  sources) and passing 3 of 3 on a quiet machine; locale keys 655 equal; focus checks en-US 275, pt-BR 282, nav-test 209.
- **Installed on the owner's desktop** (owner's word, 2026-11-03): the rc10 installer in its update mode (`/S /UPDATE`, elevated);
  `version.json` reads 1.3.0-rc10, the launcher is the installer build (`e0208c96...`), the 19 game files match `MANIFEST.json`.
  The rc9 program files are backed up in `private/_program-backups/rc9-before-rc10-20261103T174300Z`.
- **Open:** the owner's test (a car wash or oil change in GT Auto; a Spec II replay saved, the game closed, the replay found again;
  a launcher on GSdx without it installed). The notebook still has the first rc9.

## 1.3.0-rc9 published as a pre-release (2026-11-03)

The owner approved rc9 (slice395, installed on their desktop) and asked to publish it as a pre-release at once, because
rc8 has the CoreGS 2.0 picture bugs that TASK-139 fixed. `v1.3.0-rc9` at `39320b99` on `task/134-game-code-optimization-study`
(the branch is not merged into `main`); pre-release, not Latest; `v1.2.5` stays Latest and `v1.3.0-rc8` stays published.

- **Game:** slice395 (`private/slice395-dist-t134r2`, pins `slice395-pcsx2.json` and `slice395-spec2.json`), the files installed
  on the owner's desktop.
- **Assets** (`private/release-130-rc9b-out`, launchers labelled rc9 from the same commit): installer
  `GT4Recomp-Setup-1.3.0-rc9.exe` 52,620,178 bytes `4f2c54bc...`, portable zip 81,746,509 bytes `fdb54662...`, `SHA256SUMS`.
  The GitHub digests of all three equal the local files.
- **Checks:** installer (7-Zip extraction) and zip against the game folder, 0 differences, no game data, no build paths in the game
  programs; both launchers carry the rc9 label and the updated 1.3.0 notes in both languages; locale keys and placeholders equal
  (645); focus checks en-US 258 of 258, pt-BR 265 of 265; nav-test 209 of 209; launcher `cargo test` 0 failures.
- **Notes:** `docs/release-notes/1.3.0-rc9.md` (the GitHub body) rewritten with highlights, the picture fixes first and the
  measured numbers; the short notes in en-US and pt-BR have the same 10 bullets.
- **Not done:** the notebook still has the first rc9 build (slice390) under the same version label, so its launcher does not offer
  this one; it needs a manual update when the notebook is on.

## TASK-134, second round: rc9 with the picture fixes uses about 10% less processor (2026-11-03)

The owner resumed the CPU work after TASK-139 and asked for every optimisation overnight, one at a time, measured, kept even when
small, on the desktop only (the notebook was off). Final build `build/t134-cand/f3-spec2` and `f3-v200` in the worktree (PGO and
ThinLTO again, exe `6ad2e34b...` and `b3989168...`), pins `docs/inputs/slice395-spec2.json` and `slice395-pcsx2.json`, game files
in `private/slice395-dist-t134r2`. **Installed on the owner's desktop** (owner's word, 2026-11-03, elevated copy into
`C:\Program Files\GT4Recomp\game`; the 19 files match `MANIFEST.json` by hash). The slice392 files it replaced are backed up in
`private/_program-backups/t139-before-t134r2-20261103T072312Z`; the notebook keeps rc9. Not uploaded; rc9 is still not published.
- **El Capitan and Tokyo R246** (same session kind, 2 rounds, slice392 -> this build): process CPU 7.76 -> 7.10 ms (-8.5%) and
  7.43 -> 6.94 ms (-6.6%); wall -10% on both.

- **Result, player's session (warm shader cache), recorded George V Paris lap, Spec II, 1x, headless, 3 interleaved rounds, Ryzen
  7 5700X and RX 9060 XT, process CPU per VBlank:** rc8 12.96 ms, rc9 10.48, rc9 with the TASK-139 fixes (slice392) 10.59, this
  build **9.57 (-9.7% against slice392, -26% against rc8)**. Wall without pacing 5.65 -> 5.15 ms (-9%).
- **Bench fix first:** the headless runs had no shader cache and compiled CoreGS 2.0's pipelines during the lap; the bench tools
  now warm one per executable (ai-memory `gotchas/headless-bench-shader-cache`). rc9's 2026-11-02 numbers were taken without it.
- **Kept, each measured against the step before:** CoreGS 2.0 submits every 64 batches on a discrete GPU (GS -5% at 1x); the VU1
  CLIP flags over the lanes (-2.6% of the VU1 program); the VU1 outer product as one vector operation (-8.5%); the VU0 programs over
  four lanes with fp4 (-24% of VU0); GPU vertices inserted per primitive (GS -3.1%); partial VU loads and stores as whole qwords
  (VU1 -2.8%). The VU1 and VU0 tables are regenerated by the changed emitter (`private/slice394-*`). Journal 2026-11-03.
- **Dropped with a measurement:** the VIF wrap mask and a per-format generic VIF unpack (exact, no gain); uber pipelines only (2%
  less CPU, 5% slower frame).
- **Same game (gates against slice392, `private/task134-tools/gates2.sh`):** race clock and simulation state identical on the
  George V Paris, El Capitan and Tokyo R246 recordings, with 24, 35 and 35 frames identical; v2.00 night race at 3x, state and
  10 of 10 frames; Arcade path 14 of 14; Spec II lap at 6x 12 of 12; VU1 and VU0 verify against the exact interpreter on both
  games and the MTVU check over 8,032 frames, 0 differences.
- **Not verified:** the notebook (off); a window session; the owner's own test.
- **The dark block on a track sign** (VBlank 3750, 2 of 12 runs on rc8 and rc9): not seen in 12 runs of this build, probably fixed with IN-528.
- **Left:** a CoreGS 2.0 submission thread (presentation submits on the same queue, so every submission needs one ordered queue);
  a deferred flare readback on the GS thread; the SPU2 envelope; P8. Study section 8, "Second round".

## TASK-139: CoreGS 2.0 picture bugs, fixed on rc9 and merged into the CPU branch (2026-11-03)

**Done:** the owner tested slice392 in the game ("everything fine so far") and approved the merge into `task/134-game-code-optimization-study`, where the TASK-134 agent continues. Not uploaded.

Branch `task/139-coregs2-picture-bugs`, from `task/134-game-code-optimization-study` at `56f90c96`. The owner saw graphic bugs and records drives for us to replay on both renderers.

- **Tools:** `scripts/coregs2_bench/record.cmd <name>` records on the rc9 package; `replay-run.sh` replays on CoreGS 2.0 and the new `replay-gsdx.sh` on the reference renderer, final frames only.
- **Checked:** a short recording through `record.cmd` ended with a complete file. The owner's Ford GT lap replayed headless on the rc9 package on both renderers at 3x reaches the same race, with the same lap of 1'23.394. The reference's dump numbering is one frame ahead of ours (0.016 s on the race clock).
- **Not verified:** that Tokyo R246 is listed in Arcade on the bench's card; replay in sync on the reference renderer with AI cars (IN-506 saw a different rival list between the two), so a Time Trial is the safe recording.
- **Bugs registered (TASK-139 in `TASKS.md`):** the time trial ghost drawn solid (Confirmed: the reference and a real PS2 draw it in alternating rows); the Tokyo R246 asphalt texture (owner and a player); a glitch when a race returns to the menus at the 480p to 480i switch (owner, random, not on GSdx).
- **The owner's recordings:** Tokyo R246 and El Capitan (Time Trial, Spec II), replayed headless at 3x on both renderers. The reference stays in sync with the recordings.
- **Fixed, all four found on those replays (IMPLEMENTATION-NOTES IN-528 to IN-531):**
  - IN-528, black blocks and broken textures on Tokyo R246: a CPU sprite left its blocks' identities stale, so the upload memo skipped restoring them. Upload check to VBlank 8000: 4,276 mismatches -> 0.
  - IN-529, the wrong frame as a race opens and in the fade out (signs missing, white trees), and El Capitan's grey road at VBlank 2700: GT4 clears its depth through FRAME and ZBUF at once; drawn now as a depth write.
  - IN-530, the solid time trial ghost: SCANMSK was not drawn at all; now on output rows, like GSdx.
  - IN-531, green screens and junk when a race returns to the menus: the display showed GS memory no buffer was written to.
  - IN-532, El Capitan's exit: the language menu of the game's start shown for 13 VBlanks: memory or a target that the race's draws had replaced.
- **Checks:** Tokyo VBlanks 10750-10780 against GSdx: median difference 0.00 (was green); the fade frames and frame 8000 look like GSdx; George V Paris lap: the car's rear glass now shows the interior, as on GSdx. New tests `hw_core2_depth_clear`, `hw_core2_scanmsk`, `hw_core2_display_memory`, and a CPU sprite case in `hw_core2_upload_memo`; each fails with its A/B switch.
- **v2.00:** the night race against rc9: 3 of 10 frames identical, the rest differ only on the wheels, which now spin with their blur as on GSdx (rc9 showed still spokes).
- **Installed for the owner's test (2026-11-03):** game files only, `private/slice392-dist-t139` (pins `docs/inputs/slice392-pcsx2.json` exe `27cbf602...`, `slice392-spec2.json` exe `72eea060...`; slice391 was the same without IN-532), into `C:\Program Files\GT4Recomp\game`; every file checked by hash. The rc9 game files are kept in `private/_program-backups/rc9-before-t139-20261103T025446Z`. Built by `scripts/coregs2_bench/game/use-build.sh` with slice390's EE libraries and profiles (no new training); its George V Paris frames are equal to the debug build's, 8 of 8, and the debug build's to the one before IN-532. Launcher, saves and settings untouched. The first Play compiles shaders again.
- **CTest on the debug build:** 291 of 292, `python_native_contracts` the known failure (its GS settings and VU options cases).
- **With the owner's texture pack** (`data\Textures`, an HD HUD and UI pack, 15,423 files): Tokyo VBlanks 7700 and 8000 show the black blocks on rc9 and none on the installed build. The player's own pack was not available.
- **Not verified:** a window run and the owner's own test; the notebook (off). Not uploaded.

## TASK-134: 1.3.0-rc9, the CPU optimisation, installed for the owner's test (2026-11-02)

Branch `task/134-game-code-optimization-study`, on top of rc8. **Not published and not uploaded, on the owner's word:** the owner has CoreGS 2.0 picture bugs to fix first. Details: `docs/GAME-CODE-OPTIMIZATION-STUDY.md` section 8, `docs/release-notes/1.3.0-rc9.md`, `docs/BUILD.md`.

- **Result, process CPU per VBlank in a player's session, recorded lap on George V Paris, Spec II, 1x, headless, interleaved:** i5-7200U 33.87 -> 27.64 ms (-18%); Ryzen 7 5700X 12.59 -> 10.14 ms (-19%). Wall 15.77 -> 14.48 and 5.88 -> 5.43.
- **What did it:** PGO and ThinLTO of the whole game with the EE library rebuilt; Spec II's VU0 programs compiled; the VU0 macro operations over four lanes; no measuring without a report.
- **Same game:** race clock and simulation state byte-identical to rc8 on the lap and on the v2.00 night race; 36 of 36 frames at 3x and 6x; VU1, VU0 and MTVU verify at 0 differences.
- **Baseline from now on (owner):** the recorded lap, `scripts/coregs2_bench/game/ab.sh`.
- **Without the performance report,** the stage times in `[core2]` read 0; F7/F8 needs that report on, as before.
- **Open:** the owner's own test on both machines; everything above is headless. A dark block on a track sign at VBlank 3750 in about 1 run in 6, on rc8 too, cause Unknown. The GS worker is now the largest thread.
- **Installed:** the owner's desktop and the notebook's portable copy (backup `_program-backups/rc8-before-rc9-20261102T235701Z`). Pins slice390; output `private/release-130-rc9-out`.
- **On hold (owner, 2026-11-02):** the CPU work stops here for now and rc9 is not finished. The owner goes back to CoreGS 2.0 for the picture: textures that break and graphic glitches. Those fixes build on this branch, and the slice390 package will need rebuilding with them.

## 1.3.0-rc7 and 1.3.0-rc8 published as pre-releases; the laptop test passed by the owner (2026-11-02)

**The owner's test on the i5-7200U / HD 620 closes the beta gate:** "2x 720p locked at 30 FPS was a really nice experience". A player on this card chooses 1x at 60 FPS or 2x at 30 FPS. Full speed at 2x and 60 FPS is no longer the gate. Sessions on the notebook, Spec II, candidate 382, table in `docs/release-notes/1.3.0-rc8.md`; every interval is the owner's own F7/F8, so the stretches differ.

- **At 2x and 60 FPS the limit in this report is the game's own frame (Confirmed), and the cause is a Hypothesis:** critical path 20.28 ms, with the VU1 program at 12.42 ms per frame against 7.07-8.03 ms at 1x on the same kind of work, GS waits for the GPU 0.91 ms. The processor seems slower when the graphics part is busy, since the two share power and memory on this chip. Not measured apart.
- **Known issue, left as it is (owner):** at 2x on the HD 620, the showroom runs at about 30 FPS with the card at 100%, because menus stay at 60 and the 30 FPS choice applies to races only.
- **The notes:** the "about half the processor of GSdx" claim is removed, since nothing measured it for the whole process. Discord Rich Presence, on by default since rc7, is now named in the rc7 notes. The owner tested it with the real Discord client: it works (2026-11-02).
- **Later (owner), no work now:** an experimental unlocked frame rate up to 60 that skips frames when it falls (`docs/TASK-082-UNLOCKED-FPS-STATE.md`).
- **Published on the owner's word:**
  - `v1.3.0-rc7` at `c21e934c`, launcher rebuilt from that commit with the game of `private/slice337-dist-130-rc7` (pins 335 and 336), output `private/release-130-rc7-out`: installer `94cffac0...`, zip `b1f8ca47...`.
  - `v1.3.0-rc8` at `31753907`, candidate 382 with the final notes, output `private/release-130-rc8w-out`: installer `189ab428...`, zip `73deb5e9...`. It replaces the old draft of candidate 354. The owner's desktop and notebook have this launcher's previous build, which differs only in the notes.
  - `v1.2.5` stays Latest.
- **Checks before each:** installer and zip against their sources by hash; locale keys and placeholders equal; `dev/focus-check.html` in both languages with no failure (rc7 215 and 222 passed; rc8 258 and 265); launcher `cargo test`. On rc7, a clean checkout fails `gs_golden` and `launch_contract` because `tests/fixtures/gs` is stored with LF and the tests compare CRLF; with those fixtures in CRLF both pass, and the file is not in the program. This branch's tests normalise both sides.
- **Not verified:** rc7 and rc8 installed from the published assets; rc7's launcher in a real session (its game files are the ones of the earlier kit); GSdx in an accented folder.
- **Merged on the owner's word:** PR moesuito/gt4-staticrecomp#180 into `task/134-game-code-optimization-study` (`d1f9e765`).
- **Next (owner):** rc9 is the CPU optimisation (TASK-134), on this branch.

## TASK-138: a portable folder carries its image, accented paths, the GSdx fallback; candidate 382 (2026-11-02)

Branch `task/138-relative-image-link`, on top of TASK-137. Decision 0075. Not merged, not uploaded. Pins `docs/inputs/slice382-pcsx2.json`, `slice382-spec2.json` (capsule of 376); game `private/slice382-rc8-game`, distribution `private/slice382-dist-130-rc8`, package `private/release-130-rc8u-out`.

- **Relative link:** an image inside the launcher's folder is saved relative to it, one outside by its absolute path. The folder can move with the image. A time within 2 s is not a change (exFAT and FAT round it). The `game` folder is refused for the image, because updates replace it.
- **Bugs found by testing it for real:**
  - A direct start of a release was always refused: the game knew only the build's names (`gt4vulkan.exe`), not `gt4.exe` and `gt4-spec2.exe`.
  - CoreGS 2.0 closed at once in an accented folder: `u8path` on the shader cache and textures folders, which are in the ANSI code page. Now `_wgetenv`; `test_paths_from_environment.py` guards it.
  - The GSdx path and config, and the achievements cache folder, were decoded as UTF-8 too; the message box drew bytes as characters.
- **Fallback:** when GSdx fails, the game offers CoreGS 2.0 (not CoreGS 1.1). Yes stores it, when the card has Vulkan 1.2. A direct start says where to choose it.
- **Checks:** ctest 173 passed; launcher `cargo test` passed; `dev/focus-check.html` in both languages; frames 30 of 30 equal to 381; real setup, then the folder renamed and moved, then a direct start: plain name, and `Usu\u00e1rios\\Jo\u00e3o\\Pendrive` with the shader cache and textures, on CoreGS 2.0.
- **Not verified, open:** GSdx in a portable folder started directly exits with code 2 during the handshake, with a plain name and with an accent, and the game of rc7 does the same there, so this work did not cause it. A TASK-027 agent may not read `gt4-gs` logs or the fork. So GSdx in an accented folder is **not confirmed**, and needs the owner's test through the launcher, or a PCSX2-tainted agent.
- **Installed (owner's word, 2026-11-02):** the owner's desktop through the installer's update mode and the notebook's portable copy, both candidate 382. On the desktop the owner's onboarding had set up Spec II on the earlier candidate 381: no copy of the image and no GSdx download, with CoreGS 2.0 as the renderer, and its absolute link stays valid. The notebook keeps its copies of the images and its setup.
- **Settings > Games:** names the disc image's size and location; a launcher-only change on top of candidate 382, same game executables (package `private/release-130-rc8v-out`). Installed on the owner's desktop and the notebook's portable copy (owner's word, 2026-11-02).

## TASK-137: the launcher links the disc image, candidate 381 (2026-11-02)

Branch `task/137-link-disc-image`, on top of TASK-136. Decision 0075. Not merged, not uploaded. Pins `docs/inputs/slice381-pcsx2.json`, `slice381-spec2.json` (capsule of 376); game `private/slice381-rc8-game`, distribution `private/slice381-dist-130-rc8`, package `private/release-130-rc8t-out`.

- **What changes:** setup no longer copies the image; the receipt records where it is. An image that moved or changed asks to be chosen again. The game's direct start reads the same receipt.
- **Safety:** an install with a copy keeps working with it; removing a game never touches a linked image.
- **Checks:** ctest and launcher `cargo test` passed; `dev/focus-check.html` in both languages; setup on the real 5.3 GB image took 1 min 59 s, wrote 8.1 MB and left the image untouched; frames 30 of 30 equal to 378; a session with the game, card and logs under `Usuários\\João`, with the image in an accented folder and in a Japanese one (by its short name).
- **Found by that test:** a path outside the ANSI code page reached the game as `???`, and it went on with no disc. It now goes by its short name, or setup refuses it.
- **Not verified:** the owner's onboarding with the real page, an emulator holding the image open, another code page.

**The owner's desktop was reset for the onboarding (2026-11-02), nothing deleted:** `%APPDATA%\\GT4Recomp`, `%LOCALAPPDATA%\\GT4Recomp`, `%LOCALAPPDATA%\\io.github.moesuito.gt4recomp` and `<install>\\data` were set aside as `*.before-onboarding-20261102` (`data.before-onboarding-20261102`; the saves are there). `data\\Textures` stayed in place.

## TASK-136: simple onboarding, GSdx optional (2026-11-02)

Branch `task/136-simple-onboarding`, on top of TASK-135. Launcher only, game exes unchanged (candidate 378). Not merged, not uploaded, not built into a package yet. Decision 0074.

- **New install:** setup checks the card (Vulkan 1.2, then the game starts CoreGS 2.0 with `--prepare-shaders`). It passes: CoreGS 2.0, nothing downloaded. It fails: GSdx is downloaded and the page says why.
- **Settings:** choosing GSdx when it is missing asks in a dialog, which shows the download and closes by itself; declining or a failure goes back to CoreGS 2.0.
- **No change** for a player who chose a renderer or already has GSdx. The launch-time repair no longer downloads GSdx under CoreGS 2.0.
- **Checks:** launcher `cargo test` passed, with two new flow tests and the rule's test; two ignored tests with the real check on this PC passed, CoreGS 2.0 with `gt4.exe`, GSdx with the program missing; `--prepare-shaders` from an empty cache: 15 s here, 3 min 40 s on the notebook, exit 0 on both; `dev/focus-check.html` in both languages.
- **Not verified:** a whole setup with a real disc through the page, a real card refused by the renderer (the fallback was exercised with the program missing), a PC without Vulkan.

## TASK-135: performance HUD on CoreGS 2.0, candidate 378 (2026-11-02)

Branch `task/135-coregs2-performance-hud`, on top of `task/082-coregs2-frame-skip`. Not merged, not uploaded. Candidate 378 is 377 with the launcher's icon on both game executables (owner): 10 of 10 icon images in each, 30 of 30 frames equal to 377. Pins `docs/inputs/slice378-pcsx2.json`, `slice378-spec2.json` (capsule of 376); game `private/slice378-rc8-game`, distribution `private/slice378-dist-130-rc8`, portable zip in `private/release-130-rc8r-out` (`bb4538cc...`). No installer was built.

**Where it is:** the owner's notebook (portable copy, 25 program files replaced over SMB, each checked by hash; 639 files of `user` and `data` unchanged). It was not started there. The owner's desktop is still candidate 376, without the option selector and without the HUD.

- **The option:** Settings > Graphics > Advanced > Performance HUD, under CoreGS 2.0 only, off by default. A panel in the top right corner of the window: graphics card, output size, render scale, refresh rate, FPS, frame time, CPU and GPU of the game, memory, video memory, and a live frame time graph.
- **Cost:** none measurable on the graphics thread; 26.6 us per presentation by its own counter (the ledger has the table).
- **Checks:** ctest 168 passed, one skipped, with `hw_hud`; launcher `cargo test` passed; `dev/focus-check.html` in both languages; frames 30 of 30 equal to 376 and equal with it on; validation layer on the own-queue, one-queue and 30 FPS paths adds no message over a run with it off; a window resize works.
- **Not verified:** the owner's session, fullscreen at the owner's resolution, the laptop, other graphics cards (the GPU and video memory lines read counters of Windows, and show dashes where those are missing).
- **Known:** the panel covers what is behind it at 63% opacity (the owner did not ask to change it). The GSdx overlay is unchanged: changing it is fork work, for a PCSX2-tainted agent.

## TASK-082: the 30 FPS mode, candidate 376; unlocked FPS paused (2026-11-02)

**The owner's decision:** any framerate needs interpolation to be smooth, so for now the option is a fixed **30 FPS mode**. The study of unlocked FPS paused and is saved in `docs/TASK-082-UNLOCKED-FPS-STATE.md`. The owner goes on with CPU optimisation with another agent. This replaces the Skip frames when slow of candidate 375, which was never released.

Candidate 376 (pins `docs/inputs/slice376-pcsx2.json`, `slice376-spec2.json`, capsule `slice376-frame-render-license-guard-runtime`), installed on the owner's desktop through the installer's update mode (output `private/release-130-rc8q-out`, installer `fc920e62...`, portable zip `76593a0b...`). **Not uploaded.** Branch `task/082-coregs2-frame-skip`, on top of `task/027-coregs2`, not merged.

- **The option:** Settings > Graphics > Frame rate (experimental), 60 or 30 FPS, under CoreGS 2.0 only, 60 by default. In races only; menus stay at 60, because both ways of doing it in menus failed (`docs/TASK-082-UNLOCKED-FPS-STATE.md`).
- **Numbers (the ledger has the table):** 12x: 100% speed at 30 frames per second, intervals 33.0 to 33.7 ms, against 77% speed at 46 without it.
- **Checks:** ctest 167 passed, one skipped; `test_frame_sheet.py` passed; launcher `cargo test` passed; locale keys equal. Frames with it off: 77 of 77 equal to 375.
- **Not verified:** the owner's session with the mode, the laptop, a one-queue device, split screen, a real 60 Hz monitor.

## TASK-082 frame skipping on CoreGS 2.0, candidate 375 (2026-11-02)

The owner asked for frame skipping after the optimisations, with a frame by frame sheet, because the first attempt (Gemini, `task/082-render-skip`, PR moesuito/gt4-staticrecomp#113) showed old frames between new ones. Branch `task/082-coregs2-frame-skip`, on top of `task/027-coregs2`. Candidate 375 (pins `docs/inputs/slice375-pcsx2.json`, `slice375-spec2.json`, capsule `slice375-frame-skip-license-guard-runtime`), installed on the owner's desktop, **not uploaded**.

- **New option:** Settings > Graphics > Skip frames when slow, under CoreGS 2.0 only, **off by default**.
- **The stale image is explained and gone (IN-524):** Confirmed by the sheet: on both games, the frame shown is always the last one drawn. The old report's hypothesis had the right flip and the wrong timing: a drawing list runs a frame after it is built. Its proposed fix (undo the toggle on skipped frames) would have shown each frame a frame later: the first version here did that, and the sheet showed it (age 1 and 2 where 0 and 1 are possible).
- **Physics:** race clock equal at every VBlank, state after a whole lap equal except for 22 wheel floats on Spec II, 0 words on v2.00.
- **Numbers (the ledger has the table):** George V Paris at 12x on the RX 9060 XT: 100.02% speed at 38.5 frames per second, against 76.85% at 46.1 without it.
- **Spec II works too:** the first attempt was v2.00 only.
- **Pause and leaving a race:** checked frame by frame, 383 of 383 and 483 of 483 frames ok; frames are left out again a second after the race goes on.
- **Off, nothing changes:** 108 of 108 frames equal to candidate 374.
- **Decisions of mine, the owner can change:** off by default; floor of 20 frames per second; it engages at 33 ms behind.
- **Tests:** ctest 166 passed, one skipped, with `native_frame_skip`; `test_frame_sheet.py` 8 passed; launcher `cargo test` passed; locale keys equal.
- **Known:** the speed number on the race display reads up to about 3 mph differently while skipping (smoothed per frame drawn). 480p only, which the launcher always uses.
- **Not verified:** the owner's own session, the laptop (the reason for this feature), split screen, a long race with pit stops, a 60 Hz display.
- **An optimisation note for later:** 30.7% of a race's area at 12x is constant-colour blended layers (IN-523).

## TASK-027 CoreGS 2.0 candidate 374: input lag and the EE's wait (2026-11-02)

Candidate 374 (pins `docs/inputs/slice374-pcsx2.json`, `36044156...`, and `docs/inputs/slice374-spec2.json`, `647d7585...`), installed on the owner's desktop through the installer's update mode. Output `private/release-130-rc8l-out`: installer `14d4cf5c...`, portable zip `f20efb45...`. **Not uploaded.**

- **Input lag (IN-521):** presentation to display on the 60 Hz virtual display, whole lap of the owner's recording at 6x: 53.0 ms on 373's behaviour, 31 to 39.5 ms now, depending on the run (the phase of the presentation against the flip). The owner considers 30-35 ms the normal range for 60 Hz with vsync and finds this great against the 50 ms before. Input to presentation is not in these figures.
- **The waits (IN-522):** the EE no longer waits for the flare table: 9.5 ms per VBlank to 0.00 at 6x. The flares stay exact: this replaces the late flares of IN-514, and the owner's decision on them is no longer needed.
- **A bug of mine in candidate 373, fixed:** from IN-514 on, with a window, Performance Mode went through the read with a limit before its own answer at once. So on 373 it waited for the read and took the real table. Confirmed by reading the code; the effect was not measured apart, and the owner's Performance Mode reports on 373 include it. It may be why the GS looked slow in that mode.
- **D1 (IN-523): measured, not built.** The attachment path that exists is worse on this GPU at 12x (GPU busy 19.73 to 21.37 ms). 43% of a race's area has no depth test, 26% tests with alpha test and a depth write, where an early write is wrong, and 18% could reject early, where the shader already rejects before texturing. High confidence the gain is below 10% of GPU time, not worth the rewrite now. The owner can overrule. At 12x normal mode does not hold 60 FPS on George V Paris (presented mean 21.3 ms, GPU busy 19.73): the larger target is the 30.7% of area in constant-colour blended layers.
- **Frames:** against 373, headless: 122 of 122 identical on the PGO build before the last change (night 1x and 3x and 12x, Arcade 3x, George V Paris 6x in both modes, Spec II night 3x), and 47 of 47 after it (Arcade 3x, George V Paris 6x). The lap at 3x, EE waiting against not waiting: 18 of 20 identical, with the same 2 frames differing between two runs of the same mode; EE cycles equal.
- **Checks:** `ctest -R "^hw_|native_ipu_fast|standalone|native_pacing|native_input|native_mtvu"` 162 passed, one skipped; after the last change `hw_present_latency` and `native_pacing` again; launcher `cargo test --workspace` passed. Locale files unchanged since 373. Launcher notes changed in both languages with the same bullets.
- **Not verified:**
  - The flip edge step (`note_flip_miss`) has only its fixture: it was written after one lap of the final build showed 12 pairs of a late frame and a frame right behind it, and the three laps after it never landed there.
  - The owner's own session, a real 60 Hz monitor, the laptop.

**To reproduce:** `scripts/coregs2_bench/lap-run.sh <run> [ENV=VAL ...]`. A/B switches: `GT4_HW_LATENCY_CONTROL=0`, `GT4_HW_FLARE_DEFER=0`.

## TASK-027 CoreGS 2.0: owner's direction after candidate 373, open items and the input recorder (2026-11-01)

**The owner's view and decisions:**
- CoreGS 2.0 is already better than GSdx in the owner's opinion: GSdx without Performance Mode loses frame rate on George V Paris at 6x, and CoreGS 2.0 does not.
- **The darker road in Performance Mode stays:** it is not a problem.
- **The next two optimisations, both inside CoreGS 2.0:** hardware depth (D1), and reducing the waits. Waits are what to reduce from now on. Other optimisations stay for later.
- First, the input recorder below, to make the tests easier.

**Issue, improved in candidate 374 above (owner, 2026-11-01): the input lag.** The owner finds it very bad on candidate 373, about 50 ms by their estimate over a remote link that alone adds about 10 ms, and says it was not like this before. Measured, from `vkQueuePresentKHR` to the display taking the frame, at 60 Hz on the virtual display: 3 images and the second queue (373) 51-55 ms, and 42 ms in the replay of the owner's drive; 3 images, old path 49-51 ms; 2 images, old path 35-39 ms; 2 images, second queue 44 ms. At 144 Hz 13-20 ms. Input to presentation, at least one more frame, is not in these figures. High confidence that IN-511 (three images, candidate 372) added about a refresh and IN-516 2-6 ms; nothing was measured on 371, which predates the log. The same build gave 42 and 51-55 ms depending on what came before it (fast menus or not), which points to how full the display queue stays. A fix must not bring back the stall IN-516 removed.

**Open items, none started:**
- **Before publishing rc8:** the owner's own session on 373 at 60 Hz (showroom, a car ahead, a night race); a real monitor at 60 Hz; the laptop (still on 367, the beta gate is the HD 620 at 2x); the note's "about half the processor of GSdx", which needs a GSdx F8 to compare; en-US focus pages varying with unchanged code; the installer against its sources; replacing the old rc8 draft (candidate 354). Publishing and the PR into `task/134` need the owner's word, and TASK-134's P0/P1 wait for the owner's "CoreGS 2.0 build done".
- **Decided (owner, 2026-11-02):** the colour filter stays in Performance Mode too. IN-505's cut is not built: -4.4% of GPU work on the HD 620 at 2x by day, against a change to the game's whole look.
- **For 2.0.0:** fractional scales (requirement, large refactor); GPU use and power measured beside GSdx.
- **Optimisation, later unless named above:** the race start's camera cut (about 30 ms, once per race).
- **Unknown:** a few pixels (1-30, at most 88 of 255, at the right edge of the picture) differ at VBlanks 21300 and 21400 between two runs of the same exe and the same `--press` list at 3x on George V Paris. The 6x gates did not show it.
- **Out of scope for now:** GSdx stutter (a PCSX2-tainted task).

**IN-520, the input recorder:** `GT4_INPUT_RECORD=<file>` writes the pad state the game receives, one line per change, keyed by guest VBlank, so it does not depend on the key map or the frame rate. F9 ends it and closes the game; Alt+F4 also leaves a complete file. `GT4_INPUT_REPLAY=<file>` gives the game those states, with or without a window. `GT4_PACING_FROM_VBLANK=<n>` does not wait before that VBlank. `scripts/coregs2_bench/record-input.sh` and `replay-run.sh`. New CTest `native_input_recording`.
- **Verified:** the old George V Paris `--press` list converted to a recording, 3x, headless: 18 of 20 frames identical to the `--press` run, and the other 2 differ as two `--press` runs differ between them. F9: file complete, game closed, exit 0. Window replay with fast menus: 1 min 27 s against about 6 min.
- **A bug found in testing:** with the fast menus the pacer's grid ran ahead of the clock, and the first paced frame slept for about 4 minutes, which Windows showed as "not responding" on the owner's screen. Fixed: the grid is re-anchored while not waiting. It never reached an installed build.
- **The owner's recording `george-volta2`:** a time trial on George V Paris, 8,032 VBlanks. Replayed headless at 3x in 65 s: a whole lap of 1'23.394, on the track, the ghost ahead on lap 2. Two headless runs: 13 of 14 frames identical; frame 4500 differs in 876 pixels (at most 176 of 255) at the right edge of the picture, the Unknown noise above. Not yet compared with the lap time the owner saw.
- **`scripts/coregs2_bench/lap-run.sh <run> [ENV=VAL ...]`:** menus without waiting, the drive in real time, graph and figures: 1 min 50 s. First run, normal mode, 6x, 16:9, 60 Hz virtual display, 5,991 VBlanks: sent p99 17.2 ms, longest 21.7 ms, 4 over 20 ms; shown p99 18.1, longest 18.9 ms; late flares 108 in the session.

## TASK-027 CoreGS 2.0 candidate 373: frame pacing, from the owner's five F8 reports on 372 (2026-11-01)

Candidate 373 (pins `docs/inputs/slice373-pcsx2.json`, `35a11ca7...`, and `docs/inputs/slice373-spec2.json`, `abe87b24...`), installed on the owner's desktop through the installer's update mode. Output `private/release-130-rc8k-out` in the main checkout: installer `c21f0f85...`, portable zip `bbbaf160...`. Installed game folder (20 files) and launcher equal the build. Of 16,446 files of settings, saves, replays, disc copies and textures one changed: `config/game.ini`, which the launcher reopened by the installer rewrites with the active monitor (`DISPLAY1`, no stream connected). Not uploaded.

- **The owner's report on 372:** showroom frametime strange only in the first opening, in Performance Mode; normal mode 100% there. A one-off drop to about 50 FPS at the end of George V Paris's main straight, only with a car ahead (about 58 with a time trial ghost); no drop without one. Small spikes in the graph that cannot be felt.
- **Findings:**
  - The owner plays on a 1920 x 1080, 60 Hz virtual display (a game stream's). It exists only while a client is connected; when the owner left, the runs moved to the real 3440 x 1440, 144 Hz monitor at 21.5:9.
  - **The refresh rate, not the virtual display (High confidence):** the same virtual display at 144 Hz, 16:9, old path, 8 slots: showroom GPU waits 0.79 ms, against 10.96 ms at 60 Hz. A real monitor at 60 Hz was not tested. **At 60 Hz (the owner's logs and ours):** with vsync the GS thread waits about 11 ms per VBlank in the showroom, and one or two whole refreshes (17.7, 33 ms) in races. A real monitor and a plain window show none of it, and neither does no vsync.
  - **The drop:** without Performance Mode the EE waits for the GS at the flare table every frame. Frames of GS 17 ms make wall 20-22 ms; Performance Mode does not wait and held 60 FPS.
- **What it adds:** IN-514 the flare read with a limit (normal mode, window only); IN-516 the presentation on its own queue; IN-517 16 submission slots; IN-518 no block walks for targets that need no readback; IN-519 no bursts when the pacer catches up; a fresh PGO profile; IN-515 measured and left off; `frametime.csv` in every CoreGS 2.0 session and `scripts/coregs2_bench/frametime_plot.py`.
- **Measured, window, Spec II George V Paris at 6x, 16:9, real monitor, 372 against 373, one run each, 3,700 race VBlanks:** normal mode frames over 17.5 ms 40 to 5, longest 19.6 to 18.8 ms; Performance Mode longest 17.5 to 17.6 ms. **GS work per VBlank, all stages with the flare read:** normal 11.00 to 10.73 ms, of which CPU (without GPU waits) 7.94 to 7.31 ms, -8%; Performance 8.70 to 7.72 ms, CPU 7.78 to 7.24 ms, -7%. The `gs` figure of the spike lines, 9.42 to 6.79 ms, leaves out the flare read: with 16 slots the wait moved from inside the stream to that read (1.43 to 3.80 ms), and the EE waits the same (8.55 to 8.58 ms). **So the flatter frametime in normal mode comes from IN-514, not from a faster GS.** Measured before IN-519, which does not touch these frames' pacing unless the game is behind.
- **The cost of IN-514:** late flare answers on about 8% of the frames of that race (291 of 3,700; 8% to 32% across runs, 1,681 at 21.5:9). In those frames a flare is one or two frames late. **The owner decides whether normal mode keeps it;** `GT4_HW_FLARE_DEADLINE_MS=0` removes it.
- **Frames against 372, headless:** 154 of 154 identical: 1x 37/37, 3x 10/10, 12x 31/31, Arcade 27/27, Spec II George V Paris at 6x 20/20 in each mode, C.S. Route 5 4/4, Costa di Amalfi 5/5. Taken before IN-519 and the teardown fix, which change no pixel path. Headless runs always wait for the flare table, so they do not exercise a late answer.
- **Checks:** `ctest -R "^hw_|native_ipu_fast|standalone|native_pacing"` 154 passed, one skipped, with the new `hw_flare_table`. `hw_core2_gpu_clut`, last night's unexplained single failure, is explained and fixed: the fixture makes dozens of renderers on one device and each kept its rings and descriptor pools, so memory ran out; they now go with the renderer. Launcher `cargo test --workspace` passed. Window resize and minimise: 8 swapchains in one run, clean exit.
- **Measured at 60 Hz on the virtual display, candidate 373's build (the ledger has the table):** the old path with 8 slots reproduces the owner's stall (showroom waits 11.42 ms, the jagged RivaTuner graph); yet the frames reached the display evenly, so that graph was not visible on screen, as the owner said. 16 slots remove most of it (0.47 to 1.61 ms), with 2 to 4 presentation spikes of 25-33 ms per run. The second queue removes the rest: waits 0.01 ms and no spike in two runs, longest 18.2 ms, at 2-6 ms more latency and a few shown intervals of 18-20 ms. **It stays the default.** Late flares here: 0.5-1.4% of the race. A frame takes about 50 ms to reach this display, against 13-20 ms at 144 Hz; the two-image and latest-ready options did not solve it (Unknown how much of it is the display's own path).
- **Not verified:** a real monitor at 60 Hz, the owner's own session on 373, the car ahead on the owner's save. The car ahead on the owner's save (the bench's heaviest stretches, up to 48 submissions per VBlank, stand in for it). The laptop (off). Focus pages (see 372).
- **Left:** the v2.00 Arcade race start has about 1 s where the GPU needs 15-18 ms at its low clock and the GS thread waits for it at the flare read (window: presented p99 19.9 ms, longest 32.5 ms at the camera cut). Next step: the GS thread does not wait at the flare read (the reduction goes through `ReduceLater`, and the EE waits on its fence with the same limit).

## TASK-027 CoreGS 2.0 candidate 372: final 1.3.0-rc8 of the night, race start, vsync and showroom (2026-11-01)

Candidate 372 (pins `docs/inputs/slice372-pcsx2.json`, `ec4d3bd8...`, and `docs/inputs/slice372-spec2.json`, `7ccf3a68...`), installed on the owner's desktop through the installer's update mode (`private/release-130-rc8j-out`, installer `5115b8bd...`, zip `45ab8ba9...`). **Not uploaded: the owner said no GitHub yet.** **The owner's F8 on this build is pending.**

- **The owner's F8 on 371:** "sensational", normal mode nearly at full speed on George V Paris, with a pointed drop as the cars roll to the line, in both modes and not always, and an unstable frametime in the showroom only, worse on some cars.
- **What it adds, none of it cuts an effect, all in normal mode:** IN-510 GLM writes left out when nothing changed (the race start drop); IN-511 three swapchain images under vsync; IN-512 small writes over drawn pages merged on the GPU (the showroom wave); IN-513 decodes over targets reused inside a VBlank.
- **Measured in a window against 371:** race on George V Paris without Performance Mode, VBlanks over 20 ms 29 to 1 of 1000 and mean 18.13 to 16.79 ms; showroom GS thread 6.1 / 9.9 to 6.0 / 6.0 ms. Table in `docs/hw-renderer-spec/CORE2-PERF-LEDGER.md`.
- **Frames against 371:** 1x 37 of 37 identical; 3x 10 of 10, Arcade 27 of 27; Spec II George V Paris at 6x 20 of 20 twice, C.S. Route 5 4 of 4, Costa di Amalfi 5 of 5. **At 12x 30 of 31: frame 11000 differs in 2 pixels of 33 million, by at most 8 of 255.** It is deterministic and comes from IN-512, which keeps scaled pixels the whole-row patch used to replace at 1x. Named relaxation of the 12x gate; the 1x gate holds.
- **Checks:** `ctest -R "^hw_|native_ipu_fast|standalone"` passed, one skipped, with the new `hw_core2_glm_memo`, `hw_core2_patch_bits` and `hw_core2_over_target_memo`; the first two and the earlier page and grace tests also pass without the interlock. `hw_core2_gpu_clut` failed once under `-j 4` with an intermediate build and passed alone and in every later full run: not explained. Launcher `cargo test --workspace` 599 passed. Locale files: 626 keys each, same placeholders; launcher notes have the same bullets in both languages. Installed game folder equal to the distribution; 16,441 files of settings, saves, replays, disc copies and textures unchanged.
- **Focus pages, a pending check before any upload:** pt-BR 218 of 230, the same 12 known failures. en-US gave 210 of 223, 203 of 222 and 196 of 222 in three runs in a row with the same files, against 211 of 223 on candidate 369. The launcher's code and locales are unchanged since 369, so this build did not cause it, but the cause is Unknown.
- **Not fixed:** the race start's camera cut (27 to 33 readbacks, about 42 ms, once). The owner's heaviest showroom cars and the per-VBlank store they showed at the rolling start (`upload=14`, `store 1`) were not reproduced in the bench, so only the F8 can say they are gone. Three images can add up to one frame of latency under vsync. The PGO profile was not regenerated.

## TASK-027 CoreGS 2.0 candidate 371: the frametime wave on George V Paris removed (2026-11-01)

Candidate 371 (pins `docs/inputs/slice371-pcsx2.json`, `0c509aa3...`, and `docs/inputs/slice371-spec2.json`, `011ecb9f...`), installed on the owner's desktop through the installer's update mode (`private/release-130-rc8i-out`, installer `8ce07302...`, zip `ace931e9...`). **Not uploaded until the owner says so.** **The owner's F8 on George V Paris at 6x, with and without Performance Mode, is pending.**

- **The owner's report on candidate 370:** much faster (59.20 and 59.45 VBlanks/s, from 49.60 and 57.73) but a bad frame pacing in both modes: at 60 FPS the frametime looks like an audio waveform, 10-12 ms beside more than 20 ms. The owner says it was there before 370.
- **Cause (Confirmed, reproduced headless):** IN-509: GT4 draws a 256 x 256 Z target at BP 0 every second VBlank. One VBlank of grace for a target of another width dropped it each time and kept its memory: six readbacks every second VBlank, four of 640 x 448 targets. In the owner's windowed session that readback waited 7 to 11 ms for the GPU.
- **What it adds:** two VBlanks of grace. It cuts no effect, so it is in normal mode. `GT4_CORE2_WIDTH_GRACE_LONG=0` is the A/B switch.
- **Measured (headless race, three interleaved rounds):** change from one VBlank to the next 4.38 to 0.44 ms without Performance Mode and 3.87 to 0.40 with it; wall p95 21.87 to 15.13 and 17.35 to 12.75. Table in `docs/hw-renderer-spec/CORE2-PERF-LEDGER.md`.
- **Frames against candidate 370:** 117 of 117 identical: v2.00 10/10 at 3x, 27/27 Arcade, 31/31 at 12x; Spec II George V Paris at 6x 20/20 and 20/20, C.S. Route 5 4/4 and Costa di Amalfi 5/5 at 3x.
- **Checks:** `ctest -R "^hw_|native_ipu_fast|standalone"` 149 of 149 passed (one skipped), with the new `hw_core2_width_grace`. Installed game folder equal to the distribution; 16,427 files of settings, saves, replays, disc copies and textures unchanged.
- **Not fixed, seen in the owner's sessions:** the race start's camera cut (27 readbacks, about 42 ms, once; IN-429 reduced it before), and rare GPU waits of 26 to 30 ms inside a draw with no readback. In the window, GPU waits outside readbacks run at 0.5 to 7 ms per VBlank against about 0.03 to 0.5 headless: cause Unknown; Hypothesis: the swapchain's vsync. A headless run cannot show it, so the owner's F8 decides.
- **Not run on this build:** a window, the launcher's `cargo test` and focus pages (its code and locales are unchanged since 369; only its notes changed), the laptop (off), the full installer comparison required before any upload.

## TASK-027 CoreGS 2.0 candidate 370: the readback before a write on George V Paris removed (2026-11-01)

Candidate 370 (pins `docs/inputs/slice370-pcsx2.json`, `361d3310...`, and `docs/inputs/slice370-spec2.json`, `a2262c2e...`), installed on the owner's desktop through the installer's update mode (`private/release-130-rc8h-out`, installer `74c8a0a6...`, zip `66221cde...`). **Not uploaded until the owner says so**; the GitHub draft still holds candidate 354. **The owner's F8 on George V Paris at 6x, with and without Performance Mode, is pending.**

- **What it adds:** IN-508: a write that shares no page with a target's drawn rectangle neither stores nor patches that rectangle. It cuts no effect, so it is in normal mode. `GT4_CORE2_DRAWN_PAGE_TEST=0` is the A/B switch.
- **Frames against candidate 369:** 78 of 78 identical: v2.00 10/10 at 3x, 14/14 Arcade, 31/31 at 12x; Spec II George V Paris at 6x 7/7 without and 7/7 with Performance Mode, C.S. Route 5 4/4 and Costa di Amalfi 5/5 at 3x.
- **Measured (headless race, three interleaved rounds):** GS thread 17.41 to 14.04 ms per VBlank without Performance Mode (-19%), 14.95 to 12.25 with it (-18%); GPU waits 4.86 to 2.08 and 2.44 to 0.39; frame median 20.03 to 16.54 and 16.15 to 12.97. Table in `docs/hw-renderer-spec/CORE2-PERF-LEDGER.md`.
- **Checks:** `ctest -R "^hw_|native_ipu_fast|standalone"` 148 of 148 passed (one skipped), with the new `hw_core2_drawn_page_test` and new cases in `hw_core2_upload_swizzle`. With the dirty side turned off on purpose the new test fails, 9 failures. Installed game folder equal to the distribution; 16,424 files of settings, saves, replays, disc copies and textures unchanged.
- **Not run on this build:** a window, the launcher's `cargo test` and focus pages (the launcher's code and locales are unchanged since 369; only its notes changed), the laptop (off), the full installer comparison required before any upload.

## TASK-027 CoreGS 2.0 candidate 369: shaders compiled before Play (2026-11-01)

Candidate 369 (pins `docs/inputs/slice369-pcsx2.json`, `1aa79cf0...`, and `docs/inputs/slice369-spec2.json`, `d3380f68...`), installed on the owner's desktop through the installer's update mode (`private/release-130-rc8g-out`, installer `59dd9a42...`, zip `372f141d...`). **Not uploaded until the owner says so.**

- **What it adds:** IN-507: under CoreGS 2.0 Play checks the shader cache and shows "Compiling shaders" only when it must compile. GSdx is unchanged.
- **Checks:** v2.00 frames against candidate 367 10/10 and 14/14; `ctest` 147 of 147 passed (one skipped); launcher `cargo test --workspace` 599; focus pages en-US 211 of 223 and pt-BR 218 of 230, the same 12 known failures, the 6 new checks pass; a run with the prepared cache had no uber batches.
- **Real compile:** the owner's first Play on 369 compiled 300 variants through the launcher (`shaders: compiled`, 00:59); the next session found them. **Not checked:** the HD 620 (laptop off).
- **Against GSdx at 6x on George V Paris:** GSdx 57.50 VBlanks/s, 60.00 in Performance Mode (no late frame in 5871), against 49.60 and 57.73 here; p99 19.64 ms against 27.74 ms in Performance Mode. Table in the journal of 2026-11-01. GSdx wins this scene.
- **Owner's F8, George V Paris at 6x on Spec II, RX 9060 XT (candidate 368):** without Performance Mode 49.6 VBlanks/s, median 19.9 ms, p99 28.9 ms; with it 57.7 VBlanks/s, median 16.8 ms, p99 27.7 ms. GS thread 17.1 and 16.0 ms per VBlank, GPU busy 12.2 and 10.4 ms: the GS thread is the limit, not the GPU. Readbacks are the largest cost, 7.7 and 7.2 ms, of which 5.5 and 5.0 ms wait for the GPU; "before write" is the largest reason (3.0 and 3.6 ms). The worst spike was 40.2 ms, 24.7 of them waiting for the GPU. Shaders did not limit these intervals.

## TASK-027 CoreGS 2.0 candidate 368: Spec II too, installed on the owner's desktop (2026-11-01)

Candidate 368 (pins `docs/inputs/slice368-pcsx2.json`, `4ac5b8a8...`, and `docs/inputs/slice368-spec2.json`, `bfa80aa4...`), the current 1.3.0-rc8, is installed in the owner's `C:\Program Files\GT4Recomp` through the installer's update mode (`private/release-130-rc8f-out`, installer `22013da9...`, zip `d197f05a...`). Saves, replays and renderer data are unchanged; only `setup.log` changed. **Not uploaded until the owner says so**; the GitHub draft still holds candidate 354.

- **What changed:** CoreGS 2.0 is no longer limited to GT4 (IN-506). v2.00's program differs from candidate 367 only in `standalone.cpp`.
- **Checks:** Spec II headless at 3x, C.S. Route 5 and Costa di Amalfi in both Performance Mode settings, two night runs 22/22 identical, 2x and 12x ran, beside GSdx at 3x by eye. `standalone_fixture`, `spec2_hooks`, `game_target`, `spec2_iop`, launcher `launch_contract` 22 and `multi_game` 5 pass; locale keys equal.
- **Not run (the owner cancelled the tests and tests by hand):** George V Paris at 4x and 6x with and without Performance Mode (it ran at 2x and 3x); candidate 368's v2.00 frame gates; focus pages; the full launcher suite; a window; GT Mode on Spec II.

## TASK-027 CoreGS 2.0 candidate 367: GPU work for weak graphics cards (2026-10-31)

Candidate 367 (pin `docs/inputs/slice367-pcsx2.json`), the current game of 1.3.0-rc8, is on the owner's laptop only, **not uploaded until the owner says so**. Against candidate 362 on the HD 620 at 2x, no window, shaders warm, GPU busy per VBlank: day 25.84 to 18.85 ms in normal mode (-27%), 25.74 to 16.75 in Performance Mode (-35%); night 32.9 to 25.4 (-23%), 33.0 to 22.7 (-31%). RX 9060 XT, 12x night: 20.7 to 19.1 ms (-8%). No owner test of this build yet.

- **What it is:** IN-485 to IN-503: palette and upload memos, shader cache between sessions, video decoder rewrite, whole clears, depth and alpha fail before the interlock, no interlock for large disjoint batches. Results and dropped steps: `docs/hw-renderer-spec/CORE2-GPU-PLAN.md`.
- **Performance Mode now also cuts:** the road's sheen and the alpha fills that serve it (IN-501, IN-502), and small effect targets stay at 1x (IN-497). The road is darker.
- **Checks:** frames 10/10 at 3x, 14/14 Arcade, 31/31 at 12x, 28/28 video, and 10/10 and 14/14 on the integrated GPU path on the desktop; 146 renderer tests (one skipped); launcher `cargo test --workspace` 598 passed. Language keys and placeholders match in both locales. Focus pages in the browser pane: en-US 205 of 217, pt-BR 212 of 224, the same 12 failures in both, the 11 tab-highlight timing checks and the focus check known from rc8 in this pane. The release game ran with a window on the desktop, with the launcher's variables and the integrated GPU path forced, in both modes at 2x: exit 0, 16.67 ms per VBlank, the sheen cut and the early variant firing. Not tested: a window on the laptop and the launcher's own window: the owner's test decides.
- **Open:** fractional scales (G3), a launcher screen that prepares shaders with a shipped first list, frame skipping, D1 (real depth for opaque draws, the largest cost left: 7.7 of 19 ms at 2x by day).

## TASK-027 CoreGS 2.0 in the launcher as a beta, 1.3.0-rc8 (2026-10-30, evening)

Candidate 362 (pin `docs/inputs/slice362-pcsx2.json`), the current game of 1.3.0-rc8, on the owner's laptop only; the owner measured 59.3 VBlanks/s at 1x on a day track there: **not uploaded until the owner says so** (their word, 2026-10-30). The GitHub **draft** v1.3.0-rc8 still holds candidate 354, which fails to start on GPUs without mailbox when vsync is Fast (IN-475). Renderer choice CoreGS 2.0 (beta), GT4 only, GSdx stays the default; the renderer defines the graphics options (IN-474). Texture replacement (IN-470), GPUs without pixel interlock (IN-471: GPU 49.05 -> 6.85 ms at 3x), PGO in the release game (IN-473: GS worker -13%). Hardware raster path measured and left out, pending the laptop test (IN-472). Frames identical to the references (10/10, 14/14, 31/31). CTest 269/270 (the known GS settings contract failure from the rc7 merge), `cargo test --workspace` clean. Waiting for: the owner's test on the laptop; the PR into the game-code branch comes after their approval.

## TASK-027 CoreGS 2.0 candidate 347: CPU work of this side finished, handed over (2026-10-30, afternoon)

`build\hwr-play-scale.ps1` points at `build\candidates\coregs2-candidate-347` (commit 68af02f, PGO and ThinLTO of the GS libraries): 337 + IN-464 (GLM hazards by page: full barriers 579 -> 276 per race VBlank), IN-465 (statistics query opt-in), IN-466 (disjoint primitives skip the per-primitive wait), IN-467 (strip runs), IN-468 (PGO, ThinLTO), IN-469 (VU1 output scan). Against 313, 3x, interleaved: process CPU 22.0 -> 13.8 ms per VBlank (-37%), GS worker -35%, VU1 worker -23%, wall -26%, frame p50 10.9 -> 7.75 ms (-29%), p95 13.7 -> 10.6 ms. Against 332: process CPU -5%, GS worker -13%, frame p50 -7.5%. Frames identical to 337 at 3x (10/10) and 12x (31/31), and in Arcade to 345; the only difference to 332 is IN-463 (3 pixels at Arcade VBlank 9100, where the race is replaced by the copy). CTest 267/268 (the known `python_native_contracts` GS settings failure), 137/137 `hw_` tests without the interlock. Not play tested yet. Not wired into the release build: `build-release.ps1` does not pass the PGO options. Measured for later: without the pixel interlock (most mobile GPUs) the 3x race needs about 44 ms of GPU against 3.9 ms; the owner set mobile aside for now. Handover for the game-code agent: `docs/hw-renderer-spec/CORE2-HANDOVER-TASK134.md`; bench tools: `scripts/coregs2_bench/`.

## TASK-027 CoreGS 2.0 candidate 337: 12x determinism fixed, direct intake by default (2026-10-30, afternoon)

`build\hwr-play-scale.ps1` points at `build\candidates\coregs2-candidate-337`: 332 + IN-462 (direct GS intake on by default after the owner's play test of 332; `-NoDirect` sets `GT4_MTVU_DIRECT=0`; frame dumps a row at a time) + IN-463 (spec 8.8: a batch whose texture is the target it draws into samples a copy of the sampled rectangle taken before it). About 35 such batches per race VBlank (glow buffers, the in-place luminance reduction, a full-screen 1:1 pass in strips) read texels other fragments of the same batch were writing, so 12x frames differed from run to run (17-21 of 31 frames, up to 15,500 pixels; full barriers did not help, every renderer counter was equal). With the copy: two 12x runs identical (31 of 31), two 3x long-race runs identical at all 10 checks (frames 6600, 7200, 9000 and 12000 included, the pixels the ledger listed as varying in every build; without the copy the same pair differs at 6600, 9000, 10200, 11400 and 12000), 3x frames equal to 332. Cost: +1.3 ms GPU at 12x (+6%, mostly the full-screen copy), one sixteenth at 3x. New CTest `hw_core2_self_texture`; CORE2 29/29, MTVU and stream sink 13/13, front end 80/80. The branch now also carries `task/134-game-code-optimization-study` (documents only).

## TASK-027 CoreGS 2.0 candidate 332: transport and CLUT fronts, direct GS intake (2026-10-30, owner asleep)

`build\hwr-play-scale.ps1` points at `build\candidates\coregs2-candidate-332` with `GT4_MTVU_DIRECT=1` (`-NoDirect`
turns it off): 326 + IN-456 (block of a pixel from the address tables), IN-457 (CSM1 CLUT loads by table, the
renderer's CLUT halfwords as ranges), IN-458 (redundant draw binds left out), IN-459 (the VU1 worker wakes the
sleeping EE only past 64 KiB of output, when idle or stopping, or with a full ring: EE wake-ups 650 -> 16 per VBlank,
process CPU -12%), IN-461 (direct GS intake: the GS worker reads the VU1 output ring itself; off by default in the
code, on in the play script: process CPU -9%, wall -2.7%). Frames identical to candidate 310 at every check (night
race; Arcade menus, showroom and race start with direct intake on and off); CTest 28/28 CORE2, 13/13 MTVU and stream
sink, front end 79/79. Measured and dropped: IN-460 (GLM patch kernel with shared words, no change at 12x) and
expanding indexed textures instead of palette rows (worse on GPU and CPU). The 12x draw passes are bound by frame and
depth traffic; big GPU gains there need CoreGS 3.0. New instruments: `[core2] batch flushes/vblank` (flushes by
reason: CLUT loads flush about 275 of the 480 batches), `notifies` in `[mtvu] doorbells`, `mtvu_*` in `[hw] transport`.
Against 313 (start of the CPU work; headless night race, interleaved): 3x process CPU 20.2 -> 14.5 ms per VBlank
(-28%), GS worker -21%, wall 10.3 -> 8.7 ms (-15%), frame p50 10.1 -> 8.4 ms, p95 12.3 -> 11.0 ms; 12x process CPU
-23%, GPU busy 20.8 -> 20.3 ms. Found: two 12x runs of the same executable differ in a 1-pixel column of one frame
(non-determinism, to be traced in its own task; every 3x check is identical).

## TASK-027 CoreGS 2.0 candidate 326: night of CPU and GPU fronts (2026-10-30, owner asleep)

`build\hwr-play-scale.ps1` points at `build\candidates\coregs2-candidate-326`: 313 + IN-446 (GS stream through a
lock-free ring), IN-447 (draw path without allocations), IN-455 (vertex queue ring), IN-450 (unchanged uploads keep page versions), IN-451
(speculative depth rejection before texturing, exact), IN-452 (CT32 uploads by whole columns, SSE2), IN-453 (texture
cache by block identity). Frames identical to candidate 310 at every check; CTest 266/267 in both interlock modes
(the known GS settings contract failure). Final comparison with 313 (headless night race, interleaved): 3x GS worker
7.19 -> 6.41 ms per VBlank (-11%), process CPU 20.2 -> 18.9 ms, EE critical path 9.69 -> 8.96 ms, frame p50 9.7 ->
8.9 ms and p95 11.9 -> 10.9 ms, GPU busy 4.16 -> 3.90 ms; 12x GPU busy 20.77 -> 20.25 ms, frame p50 24.0 -> 22.4 ms. Measured and dropped tonight:
MTVU journal slot rings (no gain), longer submission intervals (worse), textureGather (+2.6% GPU at 12x). Details in
the ledger and the journal.

## TASK-027 CoreGS 2.0 candidate 313: CPU survey done, SSE4.1 adopted (2026-10-30)

`build\hwr-play-scale.ps1` points at `build\candidates\coregs2-candidate-313-sse41`: 309 + the CPU survey
instruments (IN-445: `GT4_CPU_SAMPLER` process sampler, `[hw] transport` line, exclusive stage times) built with
SSE4.1 (`GT4_SSE41`, now ON by default; owner rule). No visual change: frames identical to SSE2 and AVX2 builds. 3x
headless night race, medians of 3: SSE2 -> SSE4.1 10.00 -> 9.62 ms per VBlank (-3.8%, the VU1 worker -16%); AVX2 no
further gain, so no AVX2 executable. The survey (`docs/hw-renderer-spec/CORE2-CPU-SURVEY.md`) lists the CPU fronts
of the GS pipeline and transport with measured costs and a proposed task order (replay bench first, zero-copy MTVU
intake last); they wait for the owner's go-ahead. CTest 266/267 in both interlock modes (the known GS settings
contract failure from the rc7 merge).

## TASK-027 CoreGS 2.0 candidate 309: constant fills as compute (2026-10-30, fine-tuning)

`build\hwr-play-scale.ps1` points at `build\candidates\coregs2-candidate-309`: 308 + IN-442, GT4's full-screen colour,
depth and alpha clears drawn by a compute shader with the uber shader's per-pixel functions (exact; storage writes from
compute run about twice as fast as from the fragment stage). GPU busy per VBlank, night race: 12x 21.6 -> 20.6 ms, 6x
8.0 -> 7.9, 4x 5.4 -> 5.2, 3x 4.3 -> 4.1; frames identical. CTest 264/265 in both interlock modes.

## TASK-027 CoreGS 2.0 candidate 308: on the 1.3.0-rc7 base, flares hidden in performance mode (2026-10-30)

Candidate 308: 1.3.0-rc7 (`task/133-rc7-ra-controls`
c21e934) is merged into the branch as its base (Spec II, rc7 launcher; CoreGS 2.0 stays out of 1.3.0). Performance
mode (`-Performance`, `GT4_CORE2_PERFORMANCE=1`) now also hides the light flares and the sun glare as rc7's
Performance mode does for the PCSX2 GS (IN-441): Clubman Stage Route 5 night at 10x 18.1 -> 16.9 ms per VBlank.
CTest 263/264 in both interlock modes. Next: the owner's fine-tuning of CoreGS 2.0, then a PR to the EE optimisation
branch.

## TASK-027 CoreGS 2.0 candidate 307: performance mode starts (2026-10-30)

`build\hwr-play-scale.ps1` points at `build\candidates\coregs2-candidate-307` in this worktree (everything moved off
the external SSD). 307 (gt4vulkan.exe sha256 a7884470...) = 306 + IN-438 (frame/depth reads only when needed, exact) + IN-439, the first performance
mode item (decision 0073, `-Performance` / `GT4_CORE2_PERFORMANCE=1`): GT4's flare-test downloads answered one VBlank
late without a GPU wait. Owner test of 306 at 10x: 43-52 FPS, GPU-bound (128-136 W). Next levers measured at 12x:
fixed-cost upkeep batching (exact, up to ~2 ms), hardware texture filtering in performance mode (up to ~1.8 ms),
full-screen fills as clears (~1 ms).

## TASK-027 CoreGS 2.0 candidate 306: specialised pipelines (2026-10-29, waiting for the owner test)

`build\hwr-play-scale.ps1` now points at `O:\gt4recomp-scratch\coregs2-candidate-306` (gt4vulkan.exe sha256
213c29b7...): candidate 305 plus IN-437. The 12x cost was the uber pixel shader re-deciding its draw-state branches in
every wave (IN-436); pipelines specialised per draw state cut the night race GPU time per VBlank by 25-36% (12x
33.3 -> 21.4 ms, 10x 23.8 -> 15.5, 8x 15.9 -> 11.2, 3x 5.6 -> 4.2) with byte-identical frames.
`GT4_CORE2_SPECIALIZE=0` is the A/B switch. Next GPU levers, measured at 12x: the remaining fragment cost against
the 0.41 ms per full-screen pass of a fully folded shader, then D1 (ROP path, 0.21 ms per pass).

## TASK-027 CoreGS 2.0 beta candidate 305 (2026-10-29, waiting for the owner test)

Branch `task/027-coregs2`, `build\hwr-play-scale.ps1` points at `O:\gt4recomp-scratch\coregs2-candidate-305`
(gt4vulkan.exe sha256 b10cefd8...). Done overnight on the owner's beta list (`decisions/coregs2-beta-scope`):
- 1x fixed (IN-427): the 3D scene shows at 1x (was HUD only).
- Mipmapping and LOD (IN-428): GS mip levels and trilinear; distant asphalt less noisy. `GT4_CORE2_MIPMAP=0` A/B.
- Pre-race camera-cut spike reduced (IN-429 batched readbacks, IN-430 merged dirty rectangles): 46 -> 40 ms once per
  race start; in the race the only readback left is the game's own download.
- Image memory budget (IN-431): texture cache 192 MiB on integrated GPUs (768 discrete); at 192 MiB the night race
  needs 507 MB of images instead of 621 MB.
- Presentation resampling to the exact window rectangle (IN-432): Lanczos-2 when shrinking (3x+ into 1080p, sharper
  than the old blit), FSR1 EASU when growing (1x, 2x); FSR1 RCAS sharpening optional (`-Sharpen 0.2`).
- Forced bilinear `GT4_HW_FILTER=forced|forced_no_sprite` (IN-433) and anisotropic filtering of mipmapped textures
  `GT4_CORE2_ANISO=2..16` (IN-434), both off by default (`-Filter`, `-Aniso`).
- 12x checked on the night race (1.6 GB of images, 27 ms of GPU per VBlank on the RX 9060 XT); frame dumps above the
  64 MB readback buffer read in bands (IN-435).
- Not in this beta (decided, see CORE2-ROADMAP "Beta closing plan"): fractional scales (integer scale everywhere, a
  large refactor; the window resampling already gives an exact output size), texture replacement (needs the
  GSdx-compatible hash and naming from the specification team), D1 hardware raster (only if the laptop's GPU is the
  limit), frame skipping (owner).
- Checks: CTest 252/253 in both interlock modes (python_native_contracts, the pin); night race 3x frames equal 301's
  except a few wheel and road-marking pixels that vary run to run (a run with both new switches off varies the same);
  title identical to 287; 1x renders; windowed run equals the forced-size headless frame.

## TASK-027 CoreGS 2.0: CORE2 rewrite and the shared scale defect (IN-401..IN-404, candidate 285)

Branch `task/027-coregs2`. Owner baseline from 2026-10-28: a 1920 x 1080 window at 3x, 16:9, no ultrawide
(`build\hwr-play-scale.ps1`, `GT4_HW_CORE=2` by default). The core was rewritten on the reference model (CORE2,
`docs/hw-renderer-spec/CORE2-SPEC.md` and `CORE2-DESIGN.md`, `native/hw_gs/core2/`): one scaled image per target, GS
memory only through readbacks, EE uploads as dirty rectangles patched on the GPU, textures decoded on the GPU from a GPU
copy of GS memory, GT4's channel-shuffle tone as one pass (IN-401). Performance pass (IN-402): double-buffered command
recording, cached descriptor sets, fast host-to-local uploads; headless race 26.5 -> 18.8 ms per VBlank at 3x; the
owner measured stable 60 FPS in menus. The owner then saw the same defects as with the old core (ring notches in menus,
horizon stripes, random shadows), so the cause was in shared code: the front end dropped every primitive whose box holds
no pixel centre, which is right only at 1x (IN-403, found by black-box comparison with gt4-gs.exe final frames and
synthetic GS streams in `scripts/hw_blackbox/`). Fixed for both cores: the 6x menu ring now equals gt4-gs's, the grey
line across the road near the horizon is gone, race480p memory at 6x identical. Target heights round to page rows
(IN-404, no more menu-field overlap with the texture memory). Open: 480i deinterlace in CORE2, the car shadow,
frametime spikes at race start, CORE2 at 1x (HUD only), the remaining speed gap. Owner test 12 (candidate 285): passed
for picture faults ("zero bugs visuais"); open: race-start stutter and mid-race drops (the GS thread costs 20 ms per
VBlank at 3x, the GPU waits), no anti-aliasing, 480i deinterlace. Next steps in
[CORE2-ROADMAP](hw-renderer-spec/CORE2-ROADMAP.md). Roadmap phase A done in candidate 286 (IN-405): 480i deinterlace
and FXAA in core 2, with an adaptive rule that keeps GT4's fast-scrolling ticker clean as in gt4-gs. Owner test 13:
deinterlace clean like GSdx, but the race picture softer than GSdx's in the same 1080p window. Cause: the present
halved the 3x frame to 960 x 672 and stretched it back (IN-390 rule); fixed in candidate 287 (IN-406, both cores), one
bilinear reduction like the reference. Owner test 13b: sharpness solved. Phase B (deep performance pass, owner goal:
16.7 ms is the ceiling, beta gate a 2x i5-7200U/HD 620 laptop) started on a headless night B-Spec scene: palettes and
textures over drawn targets now read on the GPU, indexed textures keep their indices, submissions every 96 batches
(IN-407..IN-410, candidate 288): night race at 3x 20.8-26.4 -> 17.0-20.6 ms per VBlank, readbacks 6.9-8.7 -> 0.8-1.6 ms,
frames byte-identical. Owner test of 288: lighter at night, but the title backdrop broke after a GT Mode visit (GPU
reads kept a target alive past its ageing); fixed in candidate 289 (IN-411), night 17.8-22.2 ms per VBlank headless. Owner test of 289: title clean; night
race 57.0 VBlanks/s over 192 s (frame time p95 20.6 ms, p99 23.4 ms). Measured next: the GLM page copies cost only
0.3-0.5 ms per night VBlank, the CPU swizzle of EE uploads 2.3-2.8 ms; page-row runs cut it to 0.9 ms (IN-414,
candidate 290), and the new upload fixture found and fixed wrong T8H/T4HL/T4HH uploads. Most IN-411 store waits
were for nothing (GT4's streamed textures replace whole pages under its night render): skipped in candidate 291
(IN-415), GPU waits 2.3-3.4 -> 1.1-2.0 ms and GS thread 16-22 -> 14-19 ms per night VBlank. Owner test of 291:
night race 59.2 VBlanks/s over 190 s (p95 18.0, p99 19.6 ms), 60 FPS on the hood camera, no defects. Dirty
rectangles are now exact page by page and block by block for writes in the same layout (IN-416, candidate 292): the
remaining store waits went to about 0 ms on a long night race. CLUT loads over several or depth targets are read on
the GPU too (IN-417, candidate 293): GS thread 7.2-11.1 ms per VBlank on the long night race (292: 8.3-13.9).
Owner test of 293: 59.3 VBlanks/s, EE critical path 14.0 ms per frame. No allocation per batch and images reused by
size (IN-418, candidate 294): GS thread 5.5-8.9 ms per VBlank; the GPU side now shows up as the next limit. Owner
test of 294: 59.1 VBlanks/s, GPU-limited in the window (the GPU at 45 W of 180 W). GPU timestamps show a fixed cost
per command (1x 4.3-7.3 ms against 3x 6.6-9.1 ms); barriers only where a command depends on another (IN-419,
candidate 295) halved them, GPU time 3-13% less. Draw batches deferred into one render pass (IN-420, candidate
296): 4-8 render passes per VBlank instead of 106-312, GPU time 5-13% less; timestamps split the GPU time into
drawing 3.8-4.5 ms at 3x (scales with pixels, D1's target) and a fixed 1.4-3.8 ms of compute and transfer work.
D1 measured before building it (IN-421): the whole fragment stage is only 0.4-0.7 ms of GPU time at 3x, and the
draw-pass timestamps include overlapping compute; the GPU time is the chain of small dependent commands, the next
target. Textures over no target are now decoded from a CPU page window (IN-422, candidate 297): barriers down again,
GPU time 3-10% less. Every GPU-side step gives far less than its share; a low GPU clock at this partial load is the
next thing to check. All steps are tabled in `hw-renderer-spec/CORE2-PERF-LEDGER.md`. The owner's overlay confirmed
the GPU idles at a low clock (as with GSdx), so the frame is the EE critical path. The race-start stutters were found
with a per-VBlank spike log (IN-423): 40-160 image allocations per VBlank. Image memory now comes from 64 MiB blocks
(IN-424, candidate 298): race-start VBlanks over 20 ms 60 -> 2. Owner test of 298: race smooth at 60 FPS, frametime
still jittery in menus and slightly in the race. A GPU cost map (IN-425) showed the GPU has the power but is full of
gaps; with only two submissions in flight the CPU waited for it. Up to 8 submissions in flight and submissions every
32 batches (IN-426, candidate 299): Arcade B-Spec race VBlanks over 17 ms 59 -> 15, GPU waits cut to about a third.
Owner test of 299: 60 FPS, frametime nearly flat, fidelity perfect; phase B closed. Beta closing plan in CORE2-ROADMAP
(1x shows only the HUD, mipmapping, spec step 3 audit, the remaining spikes, a memory budget, CAS, present resampling,
the HD 620 laptop gate). After the beta, the 1.3.0-rc7 branch comes in and the PR goes to the code optimisation branch.
Not merged; no pins.

## TASK-027 CoreGS 2.0: scaled images sized to their slot (IN-384, parity plan Part A)

Branch `task/027-coregs2`. First half of item 1 of `docs/hw-renderer-spec/PARITY-PLAN.md`: every scaled presentation
image now holds only its slot's real extent (FBW * 64 wide, the loaded or drawn rows, rounded to a page), created
lazily and grown on demand with the old content copied. A draw, load or texture copy that reaches past the extent grows
the image first, so no pixel is clipped (`scaled_draws_clipped=0`). A texture taken from a resident slot copies only
the batch's sampled footprint (IN-380), so the 640 x 448 race frame no longer grows to its 1024 x 512 texture view.
Scaled image device memory on the 480p race capture: 64.1 MB at 2x, 243.1 MB at 4x, 541.4 MB at 6x, against 184.5 /
738.2 / 1,660.9 MB before (65-67% less; one 640 x 448 target is 41 MB instead of 151 MB at 6x). Gates unchanged:
CTest 245/245 in both interlock modes (new `hw_scale_target_extent`), the five memory captures byte-identical (menu
1,101 frames, race 121, car 121, night 121 with `fifo_hash=3a2fe617a174e7e7`, race480p 121), scaled memory equal to
1x and 0 low-detail presented frames at 2x/4x/6x, and the 06.3/IN-377/IN-379 fixtures unchanged. GPU busy on the
480p race replay (`--gpu-timing`, this build against candidate 273): 5.25/6.42/9.58/14.77 ms at 1/2/4/6x against
5.35/5.99/8.68/16.92; the 6x gain (about 13%, scaled pass 9.18 -> 8.04 ms) is the only one outside the run-to-run
spread, so the item is reported mainly as a memory win. Reported as `scaled_image_mb` on the `[hw] cost` line and on
the replay's `scale=` line with `scaled_image_grows` and `scaled_draws_clipped`. Next: the fast scaled raster itself
(an early-Z second pass with real attachments, tolerance only on the presented image). Not merged; no pins.

## TASK-027 CoreGS 2.0: tone-sequence strip executor (IN-383, parity plan item 2)

Branch `task/027-coregs2`. Owner requirement (2026-10-23): CoreGS 2.0 must reach GSdx's performance at the same
scale; item 2 of `docs/hw-renderer-spec/PARITY-PLAN.md` (tone-sequence HLE, memory-exact) landed first.
`hw_tone_strips.comp` writes the eight 8 x 32 decal T8 strips of one 64 x 32 frame-buffer page in one dispatch from
the GLM (index texel -> 07.4 CSM1 palette entry of the strip's quadrant -> CT24 RGB, alpha kept), replacing 135,399
strip batches with 16,940 page dispatches on the 480p race capture (140 per frame) and 118,580 palette loads held
(GPU CLUT loads 200,467 -> 98,827). All five memory captures are
byte-identical (menu 1,101 frames, race 121, car 121, night 121 with `fifo_hash=3a2fe617a174e7e7`, race480p 121),
`hw_tone_hle_exact` compares the dispatch against the batches (0 of 2 frames differ in 4 MiB, 0 pixels off the CPU
form of the rule, 0 alpha bytes changed) and CTest passes in both interlock modes. Replay CPU per frame: GS stream
9.95 -> 8.14 ms, batch 5.54 -> 4.28, translate 1.83 -> 0.97, execute 3.52 -> 2.89, CLUT 1.12 -> 0.75; GLM swizzles
33,938 -> 18,272. In the game (headless 480p race, same build with the HLE off and on): GS thread 13.4-15.6 ->
10.6-11.6 ms, render passes 341-347 -> 198-204, barriers 1,591-1,645 -> 1,437-1,474; GPU busy 7.19 -> 5.35 ms at 1x
and 19.01 -> 16.92 ms at 6x, which leaves the scaled pass (9.2 ms at 6x) as the next target (item 1 of the plan).
Switches `GT4_HW_TONE_HLE=0` / `--no-tone-hle` and `GT4_HW_VERIFY_SHUFFLE=1` / `--tone-verify`. Not merged; no pins.

## TASK-134: game-code optimization study for the i5-7200U laptop (2026-10-28)

2026-10-29: the branch is synced with 1.3.0-rc7 (`task/133-rc7-ra-controls` c21e934; program files identical);
the only per-frame change since rc6, the RetroAchievements hook on the EE thread, is in study section 3.8.

Research and plan only (no code, build, benchmark or game run): [GAME-CODE-OPTIMIZATION-STUDY](GAME-CODE-OPTIMIZATION-STUDY.md),
branch task/134-game-code-optimization-study. From the owner's laptop logs (rc6, Spec II, 1x, D3D11) the four busy
threads use about 35 ms of CPU per 16.7 ms frame (EE 11.7, VU1 worker 9.3, gt4-gs CPU thread 3.4, gt4-gs GS thread
~11); half or more of the EE critical path (12.2 ms) is emulation plumbing; syscalls and guest idle are negligible and
no large spin-wait exists. Ordered plan P0 (measure) to P8 (IOP AOT); the GSdx bridge is left to the CoreGS 2.0
intake, which must be zero-copy. Owner decisions recorded the same day (study section 7): cheapest steps first,
SSE4.1 only, translator last, no GSdx bridge work, game-side gains ship with CoreGS 2.0. P0 baselines (headless Arcade
night race and its replay, WPR profile) start when the owner says the CoreGS 2.0 build has freed the desktop.

## Local rc7 owner test candidate (2026-10-27)

Codex built an unpublished installer including the current Discord changes and verified extracted game and launcher hashes. Automated Rust tests passed; real Discord and gameplay remain untested. Important lifecycle and transport edge-case validation remains open; see docs/tasks/TASK-115-CODEX-LOCAL-KIT.md. This supersedes any earlier readiness claim.

## TASK-115 rc7 Discord Rich Presence review corrections (2026-10-27)

Superseded by the second review below; the earlier "corrected" wording was premature. The feature remains unreviewed by the owner and the Discord client is not yet validated.

## TASK-115 rc7 Discord Rich Presence second review (2026-10-27)

Second review corrections on branch `task/133-rc7-ra-controls`: `FILE_FLAG_OVERLAPPED`, safe completion waiting, protocol ack/error handling, generation fencing, non-blocking stop, and real Windows named-pipe tests (`windows_pipe_roundtrip`, `windows_pipe_read_timeout`) plus 6 manager tests, flow_audit and UI checks. Live Discord still pending owner authorization. Game Bar/GPU recognition remains future scope.

## TASK-133: rc7 scope reset to RetroAchievements and controller upgrades (2026-10-26)

Owner requested mapping only: published rc6 is the fixed program baseline, with RetroAchievements
from1.2.4 and controller-only changes from1.2.5. Frame skipping is excluded, preserved remotely
at15b7bf3 in task/082-render-skip (PR113 draft, report committed) for future CoreGS2.0 resumption.

Prepared task/133-rc7-ra-controls in C:/Antigravity/gt4-wt-133-rc7-ra-controls from exact tag
v1.3.0-rc6/5832222. Integration map: docs/tasks/TASK-133-RC7-RA-CONTROLS-INTEGRATION.md.
Main's1.2.5 program is the source reference, not a launcher/base replacement. RA is originalGT4-only
with a native and launcher SpecII guard; controller curves are shared by both games. Preserve modern
multi-game/portable paths, localization, channels and rc6 performance behavior. No wheel code.

Mapping/preparation only, no implementation/build/test run or agent dispatch. Installer and portable
rc7 will require new target builds/pins, both languages and draft checks. No public rc7 yet.


## TASK-132: controller-only 1.2.5 published as Latest (2026-10-26)

Owner authorized replacing the private wheel draft and publishing a final1.2.5 after checks.
Branch task/132-release-125-controls reapplies exact reviewed controller integration8b73fa0
(PR #100/#106) over main749af82, with only version/packaging changes beyond that program code.
Wheel source, commands and UI are absent. RetroAchievements from1.2.4 remains unchanged.
Native pin slice1200 reused; fresh distribution slice1320-controls125-dist. Legacy English UI
exception retained; matching en-US/pt-BR short notes were written before the launcher build.

Repeated checks: CTest43/43, Python641 tests/24skipped, Rust381 passed/4ignored, UI focus114/114
and navigation154/154 with no JS errors. Official release build/path scan passed. Installer
extraction matched all24 staged program files plus WebView2 (the signed bootstrapper was verified too).
Installer29,041,073bytes, SHA256556e7725d11146f1f79613814fbf961e1299000bd56df7c8831a324213003a74.
Launcher SHA2563a2044c63e2a245e62c212232cee6d88b567b1cd8c84a994f7983e51101fb8c6;
game SHA2566ea86e695d058bf954292d2967be42d5d3e0b85d4aa25c0cabfcf98b38a47116.
Published stable Latest v1.2.5 after draft upload/digest verification, PR #125 / merge5d7a669.
The old private wheel draft was deleted. Tag v1.2.5 points at5d7a669; public API confirms
Latest=v1.2.5 and isDraft=false/isPrerelease=false. No new physical controller,
real-account unlock or installer execution is claimed. CoreGS/modern worktrees remain untouched.


## TASK-131: restore the 1.2.4 program baseline (2026-10-26)

Owner requested shelving the existing private 1.2.5 wheel build, returning main's program
sources to published v1.2.4, and later preparing a new 1.2.5 with controller improvements only.
All tracked files outside docs/, TASKS.md and README.md are restored directly from tag v1.2.4
(ad46df65c03894ed204beafbd02819a11e4037bc). Git blob identity is the validation for this mechanical
restoration; no code is authored, rebuilt or newly hardware-tested. Existing RetroAchievements
fixes stay exactly as released in 1.2.4. Documentation and historical manifests remain available.

The original contributions and reviewed integrations remain in history: controller PR #100 via
#106 (8b73fa0), wheel PR #101/#109 via #111 (cbfd8b8). Reintroduce only the reviewed controller
changes in a separate future task, with a fresh build/pins and acceptance. Do not assume a merged
PR can simply be merged again after restoration. No current wheel bridge enters that release.
The existing v1.2.5 stays an unpublished, superseded draft; v1.2.4 remains public Latest.

Native DFP is still planned under TASK-018, with TASK-123 research; no native protocol backend
exists yet. Owner-authorized reply to JulioHeroZ: PR #101 comment 6422692723, requesting native
DFP recognition / game-generated FFB through SDL3 and linking both tasks verified on main.
This entry supersedes earlier descriptions of the controller/wheel integrations as current code.

## Open issue task intake registered (2026-10-26)

All 14 open issues map to existing tasks or newly recorded TASK-124..130; see ISSUE-TRIAGE.md.
Claude's existing save/import, GPU, performance, aspect, texture and wheel work is reused.
Only intake, deduplication and urgency are recorded; no implementation plan or code change.
Urgent launch/crash/save/audio cases are candidates for later 1.3.0 rcs, subject to owner selection.
Cheats remain not planned; native wheels remain planned for 1.3.1. No issue or release was changed.

## Experimental rc7 kit copied to owner laptop (2026-10-26)

At the owner request, Z: portable program files now match the rc7-test082 kit. Four files
changed, all backed up and hash-verified; user/data trees and settings hash stayed unchanged.
No ISO, saves, renderer or Spec II executable changed; no program was launched. Only Spec II
is prepared there, so the current v2.00-only frame skipping cannot yet be exercised on it.
PR #113 review and the 13 observed Python failures remain unresolved; this is not release approval.
See journal 2026-10-26 and private/task082-laptop-update-20261026T065206Z/receipt.json.


## Native DFP wheel scope and research (2026-10-26)

Owner corrected the guest model to Logitech Driving Force Pro (DFP), 900-degree steering with
GT4-generated wheel force feedback translated to SDL3. Planned release 1.3.1; 1.3.0 stays focused
on Spec II and performance. TASK-123 research brief is ready for an owner-run OpenCode agent,
not dispatched and not implemented. See docs/tasks/TASK-123-DFP-WHEEL-RESEARCH.md.
Existing PR #101/#109 host-input work remains in main as reusable groundwork; TASK-018 is incomplete.
The 1.2.5 draft stays private, publication on hold. No release, native source, active rc7 worktree
or owner data is changed by this documentation update. TASK-122 remains reserved on its release branch.


## TASK-121 configurable wheel integration for 1.2.5 (2026-10-26)

PR #100 is merged through #106 at main8b73fa0. PR #101 (latest6da047cf) and integration #109
(99f8dc48) are reviewed on task/121-wheel-integration, preserving both external authors.
The original conflicts and routing/capture/configuration/haptic lifecycle defects are corrected.
Fresh slice1210 built and passed CTest 44/44, Python 641 tests (42 skipped), Rust 391 passed
(4 ignored), UI focus 114/114 and navigation 169/169 with no JavaScript errors. Recorded real
wheel UI calls passed replay through Rust. Both independent static reviews approved the result.

Merged through PR #111 at main cbfd8b8 on 2026-10-26; original #101 and #109 also merged.
Target is 1.2.5, then a forward-port to 1.3.0-rc7 with
frame skipping and RA disabled for Spec II. No release publication or 1.3.0 worktree change here.
Current Latest remains 1.2.4; rc6 remains public. Hardware gameplay and actual force-feedback
quality remain unvalidated. Guest wheel USB protocol, direct gears, clutch and active damper/
force inversion are not implemented; see docs/WHEEL.md for the supported input/feedback scope.

## TASK-120 external controller contribution integration (2026-10-25)

Owner requested sequential acceptance of PR #100 (DualSense triggers), then PR #101 (racing wheels).
PR #100 is isolated on `task/120-dualsense-integration`, based on main `90af154` after stable 1.2.4.
Its TASKS-only conflict is resolved without dropping TASK-119 and its conflicting TASK-104 label
is replaced by TASK-120. Review corrections retain standard SDL trigger semantics, remove unsupported
signed-axis modes and keep the contributed response curves. Full native slice1200 build and focused CTest native_audio_input, native_controls and native_pad_mapping
passed (3/3), including the actual production SDL adapter with virtual PS5/Xbox devices. New immutable
`docs/inputs/slice1200-pcsx2.json` retains slice1190 inputs and RA hashes. Full CTest 43/43, Python
641 tests/42 skipped, Rust 381 passed/4 ignored, UI focus 114/114 and navigation 154/154 passed.
Merged through PR #106 at `8b73fa0`; actual controller hardware remains unvalidated. No release.
Preserve all 1.2.4 RetroAchievements fixes. See [integration mapping](PR-100-101-INTEGRATION.md).

## TASK-119 release 1.2.4: RetroAchievements (2026-10-25)

Owner approved final stable 1.2.4 publication on 2026-10-25. The contributor commit is preserved.
Final launcher and installer rebuilt without a label; CTest 43/43 passed (Python 641 tests,
42 skipped), final Rust 379 passed/4 ignored, UI focus 114/114 and navigation 150/150.
All 24 final installer payload files match their sources. Installer SHA256:
`100192215644918fa1bdd876810efa48f743399354d5c23d9de651b0c4d8be6d`.
Published [v1.2.4](https://github.com/moesuito/gt4-staticrecomp/releases/tag/v1.2.4) as Latest.
PR #96 and follow-up #105 merged; release commit ad46df6. Private rc1 and public 1.3.0-rc5 retained.
Real-account login, online unlocks, gameplay notifications and installer execution were not tested.
Owner-approved scope: legacy English UI, en-US/pt-BR notes, installer-only distribution.
Spec II remains unsupported here; RA must stay disabled for it in the later 1.3.0 port.
ai-memory MCP unavailable; see TASK-119 and the daily journal.
## rc6 performance, TASK-116 step 1 and TASK-117 (2026-10-25, `task/116-117-flare-perf`)

Implemented and tested headless, owner test pending; nothing merged or released.

- **TASK-116 step 1 (both targets' code, no new ELF):** in Performance mode only the 128-qword flare readback is answered
  locally with the previous reply (one frame old, never a zero table); `sync("download")` no longer drains the VU1 worker
  for it, the READ_FIFO is still posted in stream order (one record per readback, the night record counts equal the
  rc5 Performance-mode run), and every other readback keeps the blocking drain and the wait. Result (night race, headless
  1x): blocking drains 5,026 -> 29, the readback wait total 4,596.6 (Accurate) / 2,329.1 (rc5 Performance) -> 46.1 ms,
  4,997 of 4,998 flare replies from the cache. The Accurate run still hashes `c091716e3cb40d79` (release 1.3.0 pin). The
  P0 probe (`GT4_GS_READBACK_LOG=1`) answered the gate: in Performance mode gt4-gs returns stale real pixels, not zeros
  (139 distinct replies against 2,161), so the local answer can only be one frame late; the launcher help text of
  Performance Mode was corrected in both languages.
- **TASK-117 (GT4 v2.00 only): built, NOT shipped in rc6.** The derived ELF with four `jal` redirects and our own
  helpers in the zero padding (`scripts/flare_limits_elf_patch.py`, pinned table and verify mode), the new EE library
  profile `flare` (8 units differ from the slice208 library) and the host block at 0x7F10 are implemented and tested, but
  the switch-off identity gate failed: with every helper taking the original path, the night race still diverges from rc5
  from the first frame that draws flares (dumps from frame 10000; 27,246,609 records against 27,259,715). The rc6 v2.00
  build keeps the `aspect` library and the launcher sends no flare switch until the cause is found.
- **Checks:** CTest 46/46 and 47/47 (new `native_gs_readback_probe` and `native_flare_limits`), Python 697 tests OK
  (43 skipped) plus the new ELF patch test, `cargo test -p gt4l-core` 313 passed, MTVU verify 11,000 frames 0 mismatches.
  The Accurate identity vs rc5 holds (night stream hash `c091716e3cb40d79`). Next: the rc6 builds (v2.00 on the `aspect`
  library, Spec II with step 1 only), the pins and the draft.

## rc5 Settings open at once, TASK-114 item 8 (2026-10-24, `task/101-multi-game`, 1.3.0-rc5)

Implemented, owner test pending; nothing merged or released. The graphics cards are probed once and kept (Play refreshes
them once), and the Settings tabs open from memory: the settings table, About info, the launcher version list, the Games list and the Saves and Replays lists are read
when the home screen is up and drawn again only when something changed. "Loading settings..." appears only with no data
and after 300 ms. A tab's rows fade in once. Probes per action (code paths): start 1 (unchanged), open a Settings tab 2 to 0,
save a setting 2 to 0, Play 3 to 1, export logs 1 to 0. Checks: `cargo test --workspace` 556 passed, 0 failed, 5 ignored;
focus-check en-US 195/195 and pt-BR 202/202; nav-test 150/150 (headless Edge, own server stopped). The monitor list stays
live (not measured as slow). Not verified: the animation's feel on the owner's machine.

## rc5 polish, TASK-114 items 5-7 (2026-10-24, `task/101-multi-game`)

Implemented, owner test pending; nothing merged or released. Settings > Games names the version (Spec II v1.10) and shows
the variant and size; the button is Update; the setup steps say what they do (recognised, copying and checking, final
check); the launcher is denser on every page (CSS in app.css, changed in place); the launcher icon is the emblem on a white
rounded square. Checks: `cargo test --workspace` 550 passed, 0 failed, 5 ignored; focus-check en-US 193/193 and pt-BR
200/200; nav-test 150/150 (headless Edge, own server stopped). Screenshots pt-BR in `O:\gt4recomp-scratch\slice292-shots\`
(each page at 1280x720 and 1440x900). The launcher executable with the new icon is built at the next release build.

## rc5 hotfixes, TASK-114 (2026-10-23, `task/101-multi-game`)

Implemented, owner test pending: setup identifies the disc before copying and the first-run page is generic; the copy bar ends
full; the GPU list merges identical integrated entries and the log shows the raw entries; the 1.3.0 intro is looked up by base
version and hardened (the black window cause is not confirmed). Details in TASKS (TASK-114) and the journal.

## rc5 TASK-109: update channels and version picker (2026-10-23, `task/101-multi-game`, 1.3.0-rc5)

Implemented, owner test pending; nothing merged or released. Settings > General has Channel (Stable or Beta) and Version
(list read from GitHub on open, refresh button). The launcher version carries `-rcN` (`build-release.ps1 -Label rc5`).
Beta offers candidates and stable releases by version, Stable never offers a candidate and never downgrades, the picker lists
1.3.0-rc5 and newer, and an older target gets the strong downgrade warning. The real GitHub fetch and a real install of a
chosen version are not verified here (fake release JSON only).

Checks: `cargo test --workspace` 544 passed, 0 failed (`a_1_2_x_install_moves_into_games_gt4_at_the_first_start` failed
in 2 of 5 full runs and passed alone and in later runs: a timing flake on a shared machine; the cause was not
investigated and the code touched does not feed it); focus-check en-US 183/183 and pt-BR 190/190, nav-test 150/150 (headless Edge, server
stopped); Python `make_portable_zip` 9 tests OK. No installer built, no game launched.

## rc4 (2026-10-23, `task/101-multi-game`, 1.3.0-rc4): owner test 1 fixes and TASK-111, TASK-112

Implemented in the working tree, owner test pending; nothing merged or released. Items:
- **Saves per game (TASK-101 item 1).** Spec II's game data is `BASCUS-97436GAMEDATA` (the rc3 harvest dropped it).
  `gt4saves::SaveNames` per game; each library takes its own items; a mixed card is split by game; the import routes by
  name. The Spec II replay prefix `BASCUS-97436` is not verified with a real replay.
- **One Saves list (item 2).** Both games in one list, tagged; actions go to the item's game; New save asks the game when
  two are installed. Replays stay per game.
- **GPU list (item 3).** Vulkan and DXGI merged per physical card (LUID, else PCI ids, else name). The Vulkan probe now reads
  deviceID, driverVersion and the LUID; checked on this machine (AMD RX 9060 XT, LUID read). NVIDIA not checked.
- **Copy bar (item 4).** The bar uses the same byte total as its figure. The owner's exact case was not reproduced here.
- **Launcher logs (TASK-112).** One file per launch with INFO/WARN/ERROR, the last 20 kept, `latest.log` with the earlier
  errors, the 1.2.x log migrated once, game session folders capped at 20; About > Open latest log.
- **Export logs (TASK-111).** About and the crash / lost-card dialog; the zip with masked user names and paths; the system
  report; Open the folder and Report a problem (named link `report_problem`).

Checks: `cargo test --workspace` 519 passed, 0 failed; focus-check en-US 160/160 and pt-BR 167/167, nav-test 150/150
(headless Edge, server stopped); `cargo check --features visual-check` OK; the language check (keys, placeholders, bullets)
passes, with three identical pt-BR values that are product or language names. No game launched. Not done: owner tests
(Spec II save recovery, export, the crash dialog), the installer build (not asked).



## TASK-108 launcher: automatic graphics API and card; game programs gt4.exe and gt4-spec2.exe (2026-10-23, on `task/101-multi-game`, release 1.3.0)

Implemented in the working tree, not committed until the checks below pass; owner test pending. Graphics API has an
Automatic choice (default; Vulkan when the chosen card reports Vulkan 1.2, else Direct3D 11), resolved at every start and
Play. A Graphics card row picks the card (Automatic = discrete before integrated, then most video memory); its name goes
to the game's Adapter key (written by the launcher under `[EmuCore/GS]`). Explicit Vulkan without Vulkan 1.2 runs with
Direct3D 11, with a note in both languages. The game programs ship as `gt4.exe` and `gt4-spec2.exe` (installer, portable zip,
game folder, launcher, process detection for both names). Build output names unchanged; pins unchanged. Planned, no code: TASK-109
(update channels, 1.3.1) and TASK-110 (controller tuning, 1.3.1). Checks: `cargo test --workspace` 494 passed, 0 failed,
4 ignored; focus-check en-US 158/158 and pt-BR 165/165, nav-test 150/150 (headless Edge); `test_gs_settings` OK (72),
`test_make_distribution` OK (5), `test_make_portable_zip` OK (7), `test_gs_osd` / `test_gs_window` / `test_gs_nojit` OK.
`test_portable_install` skipped (no `GT4_NATIVE_BUILD` here). Open: owner test on NVIDIA + integrated, and on a machine
without Vulkan 1.2; the installer is built by the main agent.

## TASK-105 integrated GPUs: lighter default and GPU-lost message (#89) (2026-10-23, on `task/101-multi-game`, release 1.3.0)

Implemented, not merged; owner in-game test pending. Issue #89 (Intel UHD only, 1440 x 900): the default Render scale gave 3x
and GSdx lost the device. On an integrated GPU the untouched default scale (100%) now plays at 1x (640 x 448); a changed scale
keeps its choice; discrete machines are unchanged. A session that ends with GSdx's device-lost text in `game.log` shows a plain
message in en-US and pt-BR. Saves dates follow the interface language. Checks: `cargo test --workspace` 487 passed, 0 failed,
4 ignored; focus-check 158/158 (en-US) and 165/165 (pt-BR), nav-test 150/150. Open: owner test on an integrated GPU; the real
device-lost line in `game.log` is not yet confirmed.

## TASK-104 launcher saves: crash-proof card import, New save, Export, Rename (2026-10-23, on `task/101-multi-game`, release 1.3.0)

Implemented, not merged; owner test pending. Issue #86 (importing a PCSX2 `Mcd001.ps2` closed the launcher): the card reader
now reads the layout from the superblock (8 to 64 MB, with or without spare bytes), bounds directory depth (8), visits each
directory once, checks every length before allocating, and the import runs under a panic guard that logs the file name and
size. The Saves page gets New save (confirmed; the current save is kept as a new slot), Export of the active save as `.psu`
and Rename (40 characters, `name.txt` beside the save). Checks: `cargo test --workspace` 474 passed, 0 failed, 4 ignored;
focus-check 158/158 (en-US) and 165/165 (pt-BR), nav-test 150/150. Open: owner test with a real card and in-game New save.

## TASK-103 Graphics API for GSdx (2026-10-23, on `task/101-multi-game`, release 1.3.0)

Implemented, not merged; owner in-game test pending. Settings > Graphics offers "Graphics API (GSdx)" (table row `graphics_api`,
`[EmuCore/GS] Renderer` = 14, 15, 3): Vulkan (recommended, default), Direct3D 12 (recommended), Direct3D 11 (older PCs). The
engine setting stays `renderer`. A missing or unknown value falls back to Vulkan. Checks: `cargo test --workspace` 453 passed,
Python `test_gs_*` 168 OK, focus-check 151/151 (en-US) and 158/158 (pt-BR), nav-test 150/150; screenshots in
`O:\gt4recomp-scratch\slice280-shots`. Open: owner test on Direct3D 11 and 12 against Vulkan.

## TASK-102 launcher language: en-US and pt-BR (2026-10-23, on `task/101-multi-game`)

Implemented, not merged; owner test pending. The launcher follows the Windows language (Auto); Settings > General > Language
changes it at once. The texts are data: `launcher/crates/gt4l-tauri/ui/locales/<tag>.json` and `index.json` (the UI and the
launcher's own messages under `rust.*`, generated into a table by `gt4l-core/build.rs`), the release notes under
`docs/release-notes/launcher/<tag>/`. Adding a language is documented in `docs/TRANSLATING.md`. The CSP allows the page to
read its own locale files (`connect-src 'self'`). Checks: focus-check 151 of 151 (en-US) and 158 of 158 (pt-BR), nav-test
150 of 150, `cargo test --workspace` 450 passed. Gaps: the PCSX2 GS rows that are not in the UI keys show English, and the
XInput and key names in the controller chips stay English.

## TASK-098 portable build and TASK-097 self-sufficient game (2026-10-23, slice 278, on `task/101-multi-game`)

Release 1.3.0 (both moved from 1.2.4). **TASK-098:** a second launcher build from one source (Cargo feature `portable`):
`user\` and `data\` beside `GT4Recomp.exe`, no registry value and no system folder (the web view profile goes to
`data\launcher-webview`), per-folder single-instance lock, self-update from `GT4Recomp-<version>-portable.zip` by a
rename swap that never touches `user\` or `data\` and rolls back on failure; `scripts/make_portable_zip.py` and
`build-release.ps1 -Portable`. `cargo test --workspace` 431 passed in each build. **TASK-097:** the game executable starts
on its own (no ELF argument): paths from `<install>\game\gt4vulkan*.exe`, built-in defaults (GSdx, 480p, 16:9 hold,
automatic ultrawide, fullscreen, vsync), the launcher's `game.ini` and `gt4-gs.ini` from the user config folder, and a
native message that says to open the launcher once when a piece is missing. With an ELF argument (the launcher, the
harnesses) nothing changed: headless identity against slice268 is exact (see docs/BUILD.md "Slice 278"). Not tested by the
owner yet: the portable zip on a real PC (none was built), a direct double click of the game.

## TASK-101 multi-game launcher on branch (2026-10-23)

Branch `task/101-multi-game` (from `task/100-spec2` and main). The launcher now holds two games: a game table, per-game
disc copy, prepared files, runtime card, saves and replays (`games\<id>`), identification by serial and the pinned
executable hash (Spec II 1.10, the PT-BR image and unknown images with the known executable accepted; the unmodified
Public Beta and other SCUS-97436 programs refused), the Spec II setup (Rust port of the ELF patch, byte-identical to
`scripts/spec2_elf_patch.py`), per-game launch, the 1.2.x to `games\gt4` migration and the UI (selector, Install, Settings >
Games). `cargo test --workspace`: 405 passed, 4 ignored; the dev mock checks pass headless (121 DOM checks, 150 model
checks). Open: the look on a real window, the authors' logo and names, the installer build. Plan progress:
[MULTI-GAME-LAUNCHER-PLAN](MULTI-GAME-LAUNCHER-PLAN.md).

## Release 1.3.0 candidate built (2026-10-23, NOT published)

Branch `task/101-multi-game`, merges `63c110e` (origin/main `db5c559`) and `b8a6b5f` (origin/task/100-spec2
`9206af5`, S8c). Version 1.3.0 in `launcher/Cargo.toml`, the workspace crates of `Cargo.lock`, `tauri.conf.json` and
the installer default. No PR, merge, tag or GitHub release yet (owner approval first).

- **Two game builds, fresh slices:** v2.00 `private/slice279-build/build/gt4vulkan.exe` 97,377,792 bytes, sha256
  `9dccb5a4…` (pin `inputs/slice279-pcsx2.json`); Spec II `private/slice280-build/build/gt4vulkan-spec2.exe`
  104,612,352 bytes, sha256 `174e7adf…` (pin `inputs/slice280-spec2.json`). The Spec II pin records the S7b IOP
  runtime (`slice276`), the S8b guard (`slice277`) and the S8c EE library (`slice278`); the v2.00 pin keeps the
  slice262 capsules and the slice217 aspect library. `PERF_PROFILES['pcsx2']` now points at slice279 (slice268 kept
  as `pcsx2-slice268`); `SPEC2_RELEASE` holds slice280. The release gt4-gs pin moved to **1.2.1**
  (`private/slice281-gt4gs-121`, `gt4-gs.exe` 11,347,456 bytes `887f5bf9…`, zip 13,025,171 bytes `7eb7e4a3…`).
- **Release notes written before the build:** `docs/release-notes/1.3.0.md` (full), launcher `1.3.0.md` (en-US) and
  `pt-BR/1.3.0.md`. They cover Spec II 1.10 and same-executable variants side by side, separate per-game saves, the
  multi-game launcher, What's new, automatic language, the Graphics API choice, the portable build and the standalone
  game executable. **Owner decision 2026-10-23: no permission from the Spec II authors is needed**; they are credited
  by name in the full notes, the About page (both languages) and README, and the notes state that GT4Recomp ships no
  Spec II files (the player supplies the disc image).
- **Distribution/installer/portable:** `GAME_DIR` `private/slice282-dist-130` (both exes, shaders, CRT DLLs, SDL3,
  licences, MANIFEST; 24 files). Launcher release `GT4Recomp.exe` 17,038,336 bytes `69530dfa…` and portable
  `GT4Recomp.exe` 16,973,824 bytes `a1a0ed0d…`, both without user/build paths. Installer
  `private/release-130-out/GT4Recomp-Setup-1.3.0.exe` 51,373,344 bytes sha256 `d1569b35…`; 7-Zip extraction compared
  24 files with their sources (0 differences). Portable `GT4Recomp-1.3.0-portable.zip` 83,032,500 bytes sha256
  `bfa3a08a…`; `SHA256SUMS` lists both.
- **Checks:** CTest 45/45 in both builds; Python 694 OK (42 skipped with the v2.00 build, 56 with Spec II);
  `cargo test --workspace` clean (new relocate retry test); launcher focus-check en-US 151/151 and pt-BR 158/158,
  nav-test 150/150 in headless Edge; the launcher's Rust Spec II derivation is byte-identical to
  `scripts/spec2_elf_patch.py` (`36f60b1a…`) and both real-image tests pass. **v2.00 identity against slice268 is
  exact** in menus/day/night dumps and stream hashes (`463d455c…`/`1ccb97c5…`, `992f1ac9…`/`0c79dcf5…`,
  `c57e7d03…`/`c091716e…`).
- **Flaky migration test:** a scanner/indexer can hold a freshly written file, so the rename failed transiently. The
  move now retries sharing/lock/access-denied errors five times (20-100 ms apart) before reporting a failure; the
  test policy is covered by `only_scanner_and_lock_errors_are_retried`. The migration test passed 10/10 in isolation
  and in the full workspace run.
- **Owner test checklist** (not yet done): fresh install; update over 1.2.3 with the owner data in `%APPDATA%`
  kept; Spec II install from the ISO; Spec II GT mode and a license DEMO watched (log must show `[license-guard]`);
  Options menu; saves separate both ways; Graphics API Vulkan/D3D12/D3D11 in game; language switch; standalone exe;
  portable build on another PC.

## TASK-100 Spec II on branch (2026-10-22, S1 done)

Branch `task/100-spec2`. The owner supplied the Public Beta ISO and it is confirmed as the exact base of Spec II 1.10
(the community `.xdelta` decodes to the owner's Spec II ISO byte for byte). The beta's game image was rebuilt from its
encrypted `CORE.GT4` (Salsa20/20, key `PolyphonyDigital`, CRC-32C trailer) into
`private/slice271-spec2-beta-elf` (ELF `2e5621ed…`), which makes the static patch set measurable: Spec II changes
**one word** of the beta's own code (`0x001001E8`, a trampoline into the plugin) and adds the `.plugin` segment at
`0x0068A480` (0x2CCC bytes) over zero-filled data; everything else is byte-identical. All other behaviour changes are
written at boot by the injector: 35 word stores (18 text, 17 data), unconditional, recovered by executing the init on
an EE-RAM image. Next: S3 (target-aware host hooks, v2.00 byte-identical), S5/S6, S2/S4. Report:
[SPEC2-PATCHES](SPEC2-PATCHES.md). Nothing merged.

S3 is done: `native/game_target.hpp` holds every guest address of the host hooks per target; v2.00 keeps the release
values, and a target without an address disables the hook (one log line) with the capsule text unchanged. CTest 42/42
(new `native_game_target`); the Python suite passes except the release-pin distribution test, which any rebuilt
executable fails. Headless identity against the 1.2.3 build (slice268) is exact in all three scenes, dumps and GS
stream hashes (menus `463d455c…`/`1ccb97c5…`, day `992f1ac9…`/`0c79dcf5…`, night `c57e7d03…`/`c091716e…`), on the
final code state. S5: the VU1 blob decrypts to the v2.00 image (`ae6b6cb3…`), so the AOT table carries over; the VU0
upload differs (0x920 vs 0xA80, 1,050 bytes) and uses the counted interpreter fallback. S6: 35 IOP files mapped with
per-module policy (6 new, inherited from the beta lineage).

S2 is done too: `scripts/spec2_elf_patch.py` applies the 35 injector stores to `SCUS_974.36` from the ISO and writes
`private/slice272-spec2-elf` (`10a794df…`; `--revert` reproduces the original `db08421b…`). The trampoline at
`0x001001E8` is deliberately kept: the plugin's init calls the beta's boot initialiser and one more game function, so
only the stores are reproduced ahead of time. Pins `inputs/spec2-iso.json` and `inputs/spec2-elf.json`. S4 (its own
translated library and executable) and S7b (first boot) are next.

The Spec II translation now runs: `scripts/spec2_ee_generate.py` feeds the derived ELF to the pinned donor translator
(plain-ELF path, no CORE) and produces 346 units (1,286,297 instructions, 17,061 functions, 148,522 entries, 2 rejected
seeds), pinned in `inputs/spec2-ee-generated.json`; the generated C++ stays in scratch. Still open: the Spec II library
and its own executable (a second CMake profile), then the first boot.

S4 is now built: `-DGT4_TARGET=spec2 -DGT4_EE_PROFILE=spec2` links the Spec II library
(`private/slice274-spec2-ee-producer/game-build/ps2game.lib`, `5e483768…`) into `gt4vulkan-spec2.exe`, with v2.00
untouched. Two S7b fixes landed on the way: the plugin PT_LOAD sits inside the data PT_LOAD, so the derived ELF now
splits the data segment around the plugin (the translator otherwise read zeros for it), and 12 plugin entry seeds
cover the plugin's own pointer tables.

**S7b is unblocked, and the Spec II target boots to the menus and into a race (2026-10-23).** The stall was the FILEIO
protocol generation: the Spec II disc reboots the IOP with `DNAS310.IMG` instead of `IOPRP300.IMG`, and the donor
selected its asynchronous FILEIO server (the one that answers `devctl`, method 23, with the SIF packet `0x80000011`)
only for a path containing `IOPRP`. On the synchronous fallback the game's fileio thread waited forever on its
completion semaphore. The reboot image itself needs no loading: DNAS310.IMG is the same romdir as IOPRP300.IMG with
the same 16 Sony system modules in the same order, all served as HLE (`docs/IOP-MAP.md`, `inputs/spec2-dnas310.json`).
Fix: capsule `private/slice276-spec2-iop-runtime` (`--spec2-iop`, `scripts/spec2_iop_overlay.inc` +
`native/spec2_iop.hpp`); v2.00 keeps its capsule and is unchanged. Headless evidence: 4,200 menu frames with 1.8 M
primitives, the language menu, the main menu, Arcade Mode, track and car selection, and a 21,000-frame race at Costa
di Amalfi (169.7 M primitives, HUD and lap timer), with no "no translated code". CTest 43/43 in the Spec II build; the
Python suite passes with the Spec II build (668 tests) and keeps only the expected release-pin failure with the v2.00
build. v2.00 identity re-run: menus `463d455c…`/`1ccb97c5…`, day `992f1ac9…`/`0c79dcf5…`, night
`c57e7d03…`/`c091716e…`, all equal to the 1.2.3 build.

**S7c is done: Spec II runs on GSdx**, the shipping external PCSX2 GS process (`GT4_RENDERER=pcsx2`). Headless:
menus 4,200 frames, a Costa di Amalfi race (21,000 frames, 12 readbacks), a night race at C.S. Route 5 (2,928
readbacks, one 483 ms outlier), 3x upscale (1920x1440 dumps) and the owner's `!_HD HUD & UI` texture pack
(`[Folders] Textures` + `LoadTextureReplacements`; needs gt4-gs 1.2.1, the repo pin is 1.0.1), all clean (protocol
1.2, no stream errors or back-pressure stalls). The GT4 display crop is a fixed gt4-gs constant (`{0,0,1,2}`) that
logs for both games; the game still announces the v2.00 serial SCUS-97328 in the GS handshake and gt4-gs has no
SCUS-97436 GameDB entry (recorded for S8/S9). `scripts/frame_cost_headless.py` gained `--exe/--elf/--iso/--press/
--textures/--gs-sha256` with the v2.00 gate defaults unchanged.

**S8 is done for the main hooks (2026-10-23).** The Spec II table now has the option chain (owner `0x664EDC`, option
`+0x680`, wide flag `+0x14`, record `+0x24` with width/height at `+0xC`/`+0x10`, presets `0x664E20/50/80/B0`,
published wide word `0x6655A0`, race display index `+0x1718`), `RaceBase::initMonitor` `0x26BDB0` with the camera
return label `0x26BE60`, `RaceDisplay::render` `0x1C5040` and the HUD matrix at `+0x70`. Values were found from the
v2.00 code shapes in the derived ELF and confirmed with a runtime RAM dump of the Spec II menus. New fixture
`native_spec2_hooks` asserts every value and the write rules in synthetic RAM; a headless GSdx race at 21:9 shows the
camera matrix scaled (`x 0.761905`), the first `RaceDisplay::render`, the centred HUD canvas and the fill policy. The
16:9 hold finds the settled owner (the game already boots 16:9, so it writes nothing); the 480p hold is a
`window.cpp` VBlank hook, so headless runs cover its Spec II rule through the fixture only. Still open, rows left 0
and the hook logs once: the display request struct (photo mode), the license demo guard (Spec II has the feature; the
loaders are not mapped) and the shape observer. CTest 44/44 in the Spec II build; Python 671 tests clean on the Spec
II build (51 skipped) and only the expected release-pin failure on v2.00; v2.00 identity against slice268 exact.
Next: S9 (launcher, installer, credits).

**S8b is done: the license demo guard is on for Spec II (2026-10-23).** `kSpec2Hooks` now has loadDemoInputs
`0x2012C8` (RaceSolitaire vtable `0x6B3130 + 0x510`, the same virtual v2.00 has at `+0x4D8`), loaders
`0x10CA18`/`0x10CBA8` with return labels `0x10CB54`/`0x10CCE4` (each calls the `license/%s` helper `0x1A8BE8` and
makes one vtable call), `Manager::load` `0x207AA0`, the game CRC-32 table `0x731DB0`, the Manager add `0x1A24` and a
loader body pointer shift `0x80`. The cipher is unchanged (`0x459E90`, FT at `0x667048`, T8 at `0x731C98`); the Spec
II volume's 256 `replay/license` demos are the retail files (171 shared with v2.00, 170 bodies byte-identical) and
CRC-confirmed plaintexts keep the single-Recorder stream at body `+0x1AA4`, so the layout and CRC rule are unchanged
and the code's `0x1A24` add applies to a pointer 0x80 into the body (High confidence; not observed at runtime).
The guard now evaluates the header's body and writes `n = 0xFFFFFFFF` at `a1 + manager_add`, exactly where the
virtual reads (a static assert keeps add + shift = 0x1AA4); the overlay was re-derived into the fresh capsule
`private/slice277-license-guard-runtime` (slice262 profile), pinned in `inputs/slice277-license-guard-runtime.json`,
and CMake accepts slice27x guard capsules (v2.00 keeps slice262). Oracle on the Spec II set with our FPU: 0/256 CRC
passes, 114 sane layouts, **142 demos guarded**. Headless license test not reachable (GT mode needs game data; the
seed cards have none), so the owner run remains the gate; its log prints `demo`, `body` and `manager`. CTest 44/44 and
Python 675 OK (51 skipped) on the Spec II build, including the overlay harness compiled for `GT4_TARGET_SPEC2`;
v2.00 identity against slice268 exact in all three scenes (dumps `463d455c.`/`992f1ac9.`/`c57e7d03.`, streams
`1ccb97c5.`/`0c79dcf5.`/`c091716e.`). Next: S9 (launcher, installer, credits).

**S8c fixes the owner's Options-menu halt and widens the Spec II entry seeds (2026-10-23).** The owner's first Spec
II test halted twice opening Options (`missing code at 0x003CA690`, ra `0x0050D1B0`). The callback is built by the
Options page itself: `lui s0,0x003D` at `0x003CC8EC` + `addiu s0,s0,-22896` 53 instructions later, stored in the
descriptor; the translator's `luipair` window is 3 instructions and no dataword points at it, so it never became an
entry. New `scripts/spec2_entry_seeds.py` seeds the missing classes (wide `luipair` window 64, stack-frame
fallthrough, executable-segment literal code entries) as a 1,299-entry delta; the regenerated translation has
1,332,727 instructions, 18,597 functions and 153,474 entries, and the new library
`private/slice278-spec2-ee-producer/game-build/ps2game.lib` (80,229,124 bytes, `9a94c3ed.`) is pinned in
`inputs/spec2-ee-libraries.json` / `inputs/spec2-ee-generated.json` (CMake accepts only slice278 for the spec2
profile). After the reseed the sweep finds no real missing entries in the luipair/fallthrough/dataword/executable
classes (only plugin strings/zeros remain). Headless: the old exe reproduces the halt after 11,159 frames; the new
exe opens Options and every submenu (Race, Replay, Screen 16:9, Picture Quality, Sound, Arcade Mode, LAN Connection
Settings, Photo Lab), Replay Theater, Save Game, Credits, GT Mode and the Arcade menu with no halt. The harness gained
`--card` for an owner-card copy; a license demo was still not reached headless (the mod's "Spec II Settings" dialog
blocks the GT map in the current schedule), so the `[license-guard]` confirmation stays with the owner. New executable:
`O:\gt4recomp-scratch\spec2-s7d-build\build\gt4vulkan-spec2.exe`, 104,475,136 bytes, sha256 `301bbb4a.`. CTest 44/44
and Python 678 OK (51 skipped) on the Spec II build; v2.00 identity against slice268 unchanged (menus/day/night dumps
and stream hashes). Next: S9 (launcher, installer, credits).

## Release 1.2.3 published (2026-10-22, hotfix: TASK-092, 093, 094, 096)

Game slice268 (the 1.2.2 game plus the CoreGS 1.1 crash fix and the MTVU test-switch fixes); launcher 1.2.3: GSdx only
(CoreGS 1.1 removed from the launcher, CoreGS 2.0 shown as coming soon), textures straight in `data\Textures` with
gt4-gs 1.2.1 (published earlier the same day), desktop shortcut from the installer. Installer
`GT4Recomp-Setup-1.2.3.exe` (sha256 `475defe07926f6e41e8f8c91fd4bb3565a17d6aac92f5c8fc857d0a143530a0b`). Owner tested.
Notes: [1.2.3](release-notes/1.2.3.md). Next: 1.2.4 (TASK-097, 098, 099), 1.3.0 wheels, 1.4.0 Spec II.

## TASK-096 on branch (2026-10-22, for launcher 1.2.3, waiting for the owner test)

Branch `task/096-remove-coregs11`: the launcher no longer starts CoreGS 1.1. GSdx is the only renderer; a missing or
unverified GSdx stops Play with a message that names the log folder, and a GSdx restart request does the same. A stored
CoreGS 1.1 choice becomes GSdx in settings schema 7. CoreGS 1.1 stays in the game executable, unused by the launcher.
Not merged.

## TASK-093 on branch (2026-10-22, waiting for gt4-gs 1.2.1 and the owner test)

Branch `task/093-textures-root`: custom textures go straight in `data\Textures`; the launcher no longer creates
`replacements`, `RENDERER_MINIMUM` is gt4-gs 1.2.1, and an old `replacements` folder is flattened into `Textures` at
start once gt4-gs 1.2.1 is in use (`gt4l-core/src/textures.rs`). Workspace tests and the Python gs suite pass. Not
merged; planned for release 1.2.3 with TASK-092.

## MTVU test switches fixed (2026-10-22, TASK-092, branch `task/092-mtvu-test-switches`, waiting for release)

Slice264 (not the player path): `GT4_MTVU=0` and `GT4_MTVU_VERIFY=1` in pcsx2 mode work again. One condition,
`vblank_journal_active()` (journal on, worker on, verify off), decides the GS client VSYNC deferral and the VBlank
drain. The doorbell handshake uses seq_cst. Default path identical to slice262 (dump hashes and GS stream hash equal in
menus and night). Open: two `gt4vulkan.exe` pin tests still point at slice262's executable; a slice264 pin set needs an
owner decision. Details: BUILD.md "Slice 264".

## Release 1.2.2 published (2026-10-22, TASK-084, PR #82)

Launcher 1.2.1 at version 1.2.2 with the TASK-084 game build slice262 (owner tested and approved). Installer
`GT4Recomp-Setup-1.2.2.exe` (28,862,899 bytes, sha256 `6ecab016197fa2869261f9d3fd4ca099cb974b576ff05e1dc7b284847628a35f`),
gt4-gs unchanged (1.2.0). Release notes: [1.2.2](release-notes/1.2.2.md). Follow-up: TASK-092 (MTVU test switches).

## Frame cost, runtime side (2026-10-22, TASK-084, DONE, branch `task/084-frame-cost-122`, owner approved)

The study's missing F8 instrumentation and items 2-4 are implemented; item 1 (deferred flare readback) stays out
by owner decision (readback work belongs to CoreGS 2.0). New runtime switches, all on by default:
`GT4_MTVU_CHAIN` (one MTVU job per VIF1 DMA chain), `GT4_GIF_FASTPATH` (no second GS stream walk),
`GT4_MTVU_VBLANK_JOURNAL` (VBlank as a journal entry in pcsx2 mode). Release-candidate build `slice262`
(pin `docs/inputs/slice262-pcsx2.json`; mode `pcsx2` points at it for the owner test). Checks: CTest 40/40; the
Python suite with the new `tests/python/test_frame_cost.py`; menus 41/41, day race 15/15 and night race 15/15 frame
dumps and the GS stream session hash identical to release 1.2.0 (slice240); `GT4_MTVU_VERIFY=1` with 0
mismatches in 4,200 frames. **Owner in-game test 2026-10-22 approved** (session
`session-20261022-163408-808184`, 0 late frames in every scene, picture unchanged): showroom critical path
6.06 ms/frame with 35.8 VIF jobs/frame, Nordschleife TT 480p 6.13 with 58.6, night race 9.97 with 80.4 (the
TASK-041 owner baseline was 11.8). The night-race download wait and the GS readbacks stay out of scope
(TASK-041 Performance Mode and CoreGS 2.0). Numbers, build recipe and the owner F7/F8 checklist:
[BUILD slice 260/262](BUILD.md). PR into main opened; the reviewer merges main (launcher 1.2.1) and builds
release 1.2.2.

## Release 1.2.1 prepared (2026-10-22, TASK-091, branch `task/091-install-layout-121`, waiting for owner test)

Owner decisions: per-machine install in `C:\Program Files\GT4Recomp` (administrator rights, any folder allowed); the
large data in `<install>\data` (found relative to `GT4Recomp.exe`, so the folder is portable; Users get Modify on that
folder only); settings, controls, saves and replays per Windows user in `%APPDATA%\GT4Recomp`. The launcher moves the
1.2.0 files at its first start (user data copied and checked, the Documents folder renamed with a README; game files
renamed or copied, checked and then removed) and removes the old per-user program install; the self-update starts the
elevated setup through the UAC prompt. Game build and gt4-gs unchanged (slice240, gt4-gs 1.2.0). Owner test installer
`private/slice250-installer/GT4Recomp-Setup-1.2.1-test.exe` (28,838,764 bytes, sha256
`333d72bcbc05066f020244b32c1c0a1e890da635451f757586498c1bf7316fb6`), not run on this PC; nothing published. Details:
[BUILD slice 250](BUILD.md), [TASK-091](../TASKS.md), [release notes 1.2.1](release-notes/1.2.1.md), journal 2026-10-22.

## Release 1.2.0 prepared (2026-10-21, TASK-088, branch `task/088-custom-textures-120`, waiting for owner test)

Owner decisions: custom textures in gt4-gs (load only; folder `Documents\Gran Turismo 4\Textures`, Graphics option
"Load custom textures" on by default, Advanced "Precache custom textures" off, "Open textures folder"), the bottom crop
of 2 native rows, and a black window from the first frame. Versions: GT4Recomp 1.2.0 and gt4-gs 1.2.0 (the launcher needs
gt4-gs 1.2.0 at least). Settings schema 6 (backup kept). Game build `slice240` (`docs/inputs/slice240-pcsx2.json`).
Local artifacts, nothing published: gt4-gs package `private/slice240-gt4gs-120/gt4-gs-1.2.0-windows-x64.zip`
(branch `gt4gs/textures-1.2.0`, commit `c9dc54e93`); owner installer `private/slice240-installer/GT4Recomp-Setup-1.2.0-test.exe`
(28,765,729 bytes, sha256 `0edcfefe3a143c180bdc1255357780d414aac9cd1e60bf65fc6b583648187ec7`); helper
`private/slice240-gt4gs-120/install-gt4gs-1.2.0-test.ps1` for the local gt4-gs. Details: [BUILD slice 240](BUILD.md),
[TASK-088](../TASKS.md), journal 2026-10-21. Owner test pending: white start, crop, the texture folder with a real pack.
## CoreGS independent launcher candidate (2026-10-21, TASK-089)

Branch `task/089-coregs-independent-launcher`, baseline main `6422988`. Selecting CoreGS 1.1 now
skips the optional PCSX2 GS installer during setup, automatic package repair at startup and background
renderer release queries. Launcher updates and explicit manual renderer updates remain available.
Play also avoids inspecting an unused external renderer executable. Synthetic setup/launch and injected
release-request controls pass: the full Rust launcher workspace passes on Windows, including 53 Tauri tests
(one real-disc flow ignored). Maintainer launcher acceptance remains pending. Branch audit and limits:
[renderer independence](reverse-engineering/renderer-independence-2026-10-21.md).
No renderer implementation, game executable, input pins or saves changed. CoreGS 2.0 is not enabled by this change.
Contribution: [PR #72](https://github.com/moesuito/gt4-staticrecomp/pull/72), from the
`JulioHeroZ/gt4-staticrecomp` fork; implementation commit `aa51f2a`. Awaiting maintainer acceptance.


## Launcher self-update fixed (2026-10-21, TASK-085, branch `task/085-launcher-self-update`)

Owner report: launcher 1.0.0 offered 1.1.0, closed after the download and installed nothing. The setup joined the
launcher's kill-on-close job and died with it; now it leaves the job (`CREATE_BREAKAWAY_FROM_JOB`, Explorer fallback).
Release v1.1.0 went back to draft (0 installer downloads) and was rebuilt: launcher GT4Recomp.exe 16,010,752 bytes,
sha256 `884f4e9b4a17e54ead031bca744534ca9f50e8b94d988e0ff92b83f897979c77`; installer
`private/release-110b-out/GT4Recomp-Setup-1.1.0.exe` 28,761,196 bytes, sha256
`6ad624307370998142bdfcacbd330f6cbe6a17e70e35c8ab5a2d44db8097da0d` (same game files, slice218). Launchers 1.0.0 cannot
self-update; the release notes tell those users to run the setup.

## Release 1.1.0 prepared (2026-10-21, branch `task/080-aspect-ratios`, draft release, not merged)

TASK-080 is done: the owner tested slice218 on ultrawide and approved ("funcionou perfeitamente"). Release prep on the
branch: version 1.1.0 (Cargo workspace, `tauri.conf.json`, NSIS default; release notes
[1.1.0](release-notes/1.1.0.md)); the launcher's Settings has no Display tab any more (Graphics holds the Display,
Graphics and Advanced sections; `#/settings/display` falls back to Graphics); the launcher's gt4-gs minimum is 1.1.0 and
an older install is replaced at launch (`gs_release::needs_install`); `version.json` is brought in line with the
installed versions at start. Game executable unchanged (slice218, pin `docs/inputs/slice218-pcsx2.json`). Installer
`private/release-110-out/GT4Recomp-Setup-1.1.0.exe` (sha256 `2ca0f80f8eb28023ffabe2446a30ffcdb6a139a0d96ac101fd6bc56fa7dd19de`) on a draft GitHub release v1.1.0; the coordinator
merges and publishes. Open follow-ups: P3 progressive menus (with TASK-078); split screen and photo mode in ultrawide
unverified. README: ultrawide headline, no emojis.

## TASK-080 ultrawide (2026-10-21, branch `task/080-aspect-ratios`, not merged)

Rebuilt on the 1.0.0 release recipe after merging main db99f9e: slice215 = `--mode pcsx2` on the branch (slice214 plus
native display aspect ratios: `GT4_DISPLAY_ASPECT`, auto by default; race camera Hor+/Vert+ hook; race view filled,
menus 16:9; protocol 1.3 for gt4-gs 1.1.0; launcher Aspect ratio row, settings schema 4 migrating the 1.0.0 file).
16:9 frames byte-identical to slice214. Owner test installer `private/release-110-test-out/GT4Recomp-Setup-ultrawide-test.exe`
(app version 1.0.0, no GitHub release). Waiting for the owner tests and approval; planned as GT4Recomp 1.1.0. Details:
[BUILD](BUILD.md) slice 215 (and 212b), decision [0071](decisions/0071-native-aspect-ratios-runtime-hooks.md).

P2 (slice217, `--mode pcsx2` on the branch): the race HUD stays the game's 16:9 layout, undistorted and centred, at any
ratio (HUD canvas hook at `RaceDisplay::render`; race font x scale and mirror divisor through the ELF patch table and the
re-translated library profile `aspect`); split screen stays 16:9. Protocol 1.3 fill carries the target aspect so a custom
ratio is letterboxed (CoreGS 1.1 already was; gt4-gs needs a release that reads it). 16:9 byte-identical (menus 42/42,
race 160/160). New owner installer and the portable folder `private/ultrawide-test-portable`. Details: [BUILD](BUILD.md)
slice 217.

Addendum (slice218, `--mode pcsx2` on the branch, owner design 2026-10-21): seamless display. No Aspect ratio and no
Internal resolution rows: the aspect is always the chosen Resolution's shape (16:9 unchanged; other ratios turn P1/P2
on), Display has Monitor and Resolution (real monitors and modes), Graphics has Render scale 50-200% with the resulting
internal resolution (k = ceil(max(W/640, H/448)), whole multipliers 1..12). Game: `GT4_OUTPUT_SIZE`, `GT4_MONITOR`,
per-monitor DPI awareness. Settings schema 5. 16:9 byte-identical (menus 42/42, race 160/160). New owner installer and
portable folder. Details: [BUILD](BUILD.md) slice 218, decision 0071 addendum.

## Current state (2026-10-21)

**TASK-027 CoreGS 2.0 (2026-10-23, Claude):** owner test 6 (slice250, 4x + FXAA, 3440 x 1440) failed: every second race frame was shown from the 1x copy (GT4 reuses the buffer it is about to show as a read-only depth buffer, which dropped its scaled image; IN-377) and the ultrawide fill request never reached the in-process presenter (IN-378). Both fixed in local slice270 on top of a merge of `main` 1.2.3: display shadows with a per-pixel memory check (1x-sourced presented frames on the 480p race capture 61 -> 1 of 121 at 2x/4x/6x, memory identical to 1x), and the race fills the screen at the game's ratio like the default renderer. Owner test 7 (candidate 270) confirmed the ultrawide fill but the race still looked 1x: GT4's per-frame RGB tone curve (T8 reinterpretation, 09.4 variant A) and its full-screen copies through a 1024 x 512 REGION_CLAMP texture both went through the 1x words. Fixed in local slice271 (IN-379 tone carry of the scaled image, per-pixel checked; IN-380 textures from targets need only the sampled footprint); the presented-frame check now also flags 1x pictures with a scaled HUD (low-detail frames 121 -> 1 of 121). Owner test 8 (candidate 271, 6x + AA) passed: races at real resolution; the beta minimum (upscaling and AA) is met in races. New owner requirement: GSdx-level performance at the same scale. Local slice272: 480i menus at scale (IN-381, GPU deinterlace in native rows), the tone sequence is no longer repeated at scale (IN-382: 1.3-2 ms less GPU per race frame, same picture), clean per-category measurement (6x race 17.1 ms GPU busy, GS thread about 11 ms) and a ranked parity plan. Waiting for owner test 9 (`build\hwr-play-scale.ps1 -Scale 1|2|4|6 [-AA]`, candidate `O:\gt4recomp-scratch\coregs2-candidate-272`), see [PROGRESS](hw-renderer-spec/PROGRESS.md).

**TASK-027 CoreGS 2.0 (2026-10-22, Claude):** owner test 5 on local slice247 approved (flat 60 FPS in the Nordschleife Time Trial, a night race and Opera Paris; 0 dropped frames; colours, textures and flares as GSdx at 1x). The branch now merges `main` 1.2.2 (TASK-084 VBlank journal, chain jobs, GIF fast path): the held VSYNC reaches the hw sink in stream order (fixture `hw_stream_sink_deferred_vsync`); hw memory hashes, menu dumps and race frames identical to slice247 on local slice248. The merge also fixes a donor-mode crash present in release 1.2.2 (an uninitialised `post_vsync` callback in `mtvu::Donor`: `GT4_RENDERER` unset crashed at the first VBlank; default renderer 42/42 = slice214 again). Upscaling (2x-8x, `GT4_HW_SCALE`) and FXAA (`GT4_HW_POSTFX=fxaa`) are in local slice250 (IN-371..IN-376): races render at scale through a second, presentation-only copy of every target, so memory and readbacks stay identical to 1x at every scale; menus (480i) still show at 1x. Waiting for owner test 6 (`build\hwr-play-scale.ps1 -Scale N [-AA]`), see [PROGRESS](hw-renderer-spec/PROGRESS.md).

**TASK-027 CoreGS 2.0 resumed (owner decision, 2026-10-21):** development branch `task/027-coregs2` (worktree
`C:\Antigravity\gt4-wt-027-coregs2`) = `main` 1.0.0 + the paused TASK-027 work, without paraLLEl-GS. With
`GT4_HW_GS=ON`, `GT4_RENDERER=hw` sends main's GS stream hooks to the in-process HWR backend (black output; nothing
drawn yet); the default renderer stays byte-identical (check build slice216, [BUILD](BUILD.md) slice 216). The
implementation goes to an external clean-room agent (GPT 6.1 Sol); brief `AGENT-BRIEF-COREGS2.md` on that branch. The shader IN-07 AEM mismatch is now fixed: fresh slice217 build and all three GPU exactness fixtures pass. Native v2 capture replay now passes 69/69 CPU-library tests and produces 1,101/1,101 byte-identical PPMs from legacy, converted v2 and fresh donor v2 menus (slice219). Default headless output remains 42/42 identical to slice214. The CPU replay now has a calibrated direct VRAM output (local221,75/75 tests). Correct frame pairing explains the historical IN-315 offset; the A+D vertex kick gap (IN-326) was the cause of the garage frame900 defect (IN-316), fixed in local222 (76/76 tests): the same menu capture now has11/11 aligned samples inside the2D thresholds (garage0.2051%,45.992dB), while990/1,096 VRAM and995/1,101 scanout images stay unchanged outside frames811..916. The M4 GPU write-back path is now ported from `task/027-m4-gpuref`, built and wired into the backend (IN-329, local223): eleven fixtures pass, the default renderer stays 42/42 and `GT4_RENDERER=hw` runs 600 headless frames with 0 faults, but nothing is dumped yet. The M5 pixel pipeline is now wired into the same draw stage (blend unit over 81 modes, COLCLAMP, PABE, alpha test, DATE, FBA, FBMASK, dither) with five new GPU-versus-CPU fixtures at zero mismatches, and the headless HWR run stays clean at about 62 ms per frame. Actual composed scanout phase still needs M6 qualification. The M6 presentation interfaces are decided (IN-331) with the owner's variable-frame-rate requirements: a present scheduler decoupled from the VBlank (hold/re-present, newest-wins backlog, late-input hook), auxiliary frames that restore guest memory and refuse readbacks, and per-pass CPU timers. Parity is unchanged (default 42/42). HWR frame dumps now work (`GT4_HW_DUMP_DIR`, IN-332): 4,200 headless frames with 0 faults produce the same 42-dump schedule as the donor renderer. The first measurement is 42/42 frames outside the 2D gate: the boot text loses one line and the garage list keeps content from the previous screen, so some primitives never write. The new HWR replay tool (IN-333) replays the same capture through both paths: every EVEN frame is pixel-identical, every ODD frame loses content (the first boot text line, 4,394 pixels; garage 10.6%). So the defect is field-parity dependent draw loss in the GPU path, not textures or the front end. The memory-level comparison (IN-334) shows the odd-field loss is a VRAM WRITE problem: the two shadow memories differ only in the framebuffer pages that hold the boot text (6,591 bytes), while even frames match byte for byte. That loss is now fixed (IN-335, local slice228): the GPU write-back wrote stale depth words back over a frame buffer that shares pages with the unused depth buffer. The menu capture now replays with 678/1,101 frames byte-identical to the CPU reference (the rest inside the gate, IN-336), and the headless game run has 42/42 dumps inside the chapter 14.4 2D gate against the donor renderer (before: 0/42). The presentation now follows the VBlank field (13.3 offset, 13.4 deinterlace; IN-337/IN-338, local slice229): static menu screens no longer bob between VBlanks. Next: owner test 1 (windowed look at 1x), then M7.

**TASK-081 release 1.0.0 hardening (branch `task/081-release-1-0-0`, waiting for the owner's installer test):**
- Game: slice213 = the slice211 recipe with `-DGT4_RELEASE_DISTRIBUTION=ON` (no build-time shader folder in the
  executable; shaders only from `<exe folder>\shaders`). slice214 = slice213 plus the Vulkan baseline path (older
  drivers: swapchain maintenance 1 only when present, `GT4_VULKAN_FORCE_BASELINE=1`). `--mode pcsx2` = slice214,
  `pcsx2-slice213` and `pcsx2-slice211` kept. String scan
  clean; headless 42/42 frames identical to slice211; CTest and Python green ([BUILD](BUILD.md) slice 213).
- Launcher: release build through `launcher/tools/build-release.ps1` (`--remap-path-prefix`; no user or build path in
  `GT4Recomp.exe`); processor (SSE4.1, owner decision) and Vulkan 1.2 checks before setup and Play; the controls crate's built-in
  keyboard keys now match the TASK-076 layout (they were the old Z/X table, which a remap would have written to
  `controls.toml`).
- Installer: WebView2 detection plus Microsoft's Evergreen bootstrapper, name `GT4Recomp-Setup-1.0.0.exe`, the
  uninstaller removes `SDL3.dll`. Built in `private/release-100-out` with `SHA256SUMS`; draft GitHub release v1.0.0
  (not published). Release notes: [1.0.0](release-notes/1.0.0.md); steps: [RELEASES](RELEASES.md).
- Saves in Documents (owner request): saves and replays live in `Documents\Gran Turismo 4` (`Saves`, `Replays`; the
  Documents known folder, data-folder fallback); everything else, including the live `runtime\card.ps2`, stays in
  `%LOCALAPPDATA%\GT4Recomp`. An older AppData library is copied, checked (size, sha256) and parked as
  `library.migrated-<t>` at the first start; the guard protects the player's folder; the uninstaller never touches it.
  See [FLOW-AUDIT section 8](launcher/FLOW-AUDIT.md) and [MIGRATIONS](launcher/MIGRATIONS.md).
- Settings feedback (owner, 2026-10-21): 480p and 16:9 are fixed (the Input resolution, Widescreen 16:9 and GS
  Aspect ratio rows are gone; the launcher always sends `GT4_FORCE_480P=1` and `GT4_FORCE_WIDESCREEN=1` and writes
  `AspectRatio = 16:9`); PCSX2's own OSD rows are hidden and always written off (GS table column `forced`); Advanced
  is a section at the bottom of Graphics, not a tab. Old values are ignored by a load-time rule (settings stay
  schema 3). See [GS-SETTINGS](GS-SETTINGS.md) and [MIGRATIONS](launcher/MIGRATIONS.md).
- Next: the owner installs from the draft and tests end to end (ideally also an NVIDIA or Intel PC and a fresh Windows
  user), approves, then the merge and the publish.

## Earlier state (2026-10-17)

**TASK-044 (packaging, docs only, branch `task/044-packaging`):** release 1.0.0 ships gt4-gs 1.0.1 (`gt4gs-v1.0.1`, the
renderer already pinned for `--mode pcsx2`). The fork's release notes were edited (upstream base PCSX2 v2.8.2, commit
`fd9d310cc`; patch list; GPL-3.0 and source; SHA256SUMS). README (renderers, known issues, license and legal),
[THIRD_PARTY](../native/THIRD_PARTY.md) and [RELEASE-1.0.0-PLAN](RELEASE-1.0.0-PLAN.md) are updated. No build, no game run.
Release order: the launcher and installer (TASK-019, TASK-043), the owner's end-to-end test, then the v1.0.0 tag.

**slice206 (TASK-017 done, branch `task/017-license-crash`, main `ed123ed` merged in):** `--mode pcsx2` = slice206, the
slice203 game recipe (gt4-gs 1.0.1) plus the license demo guard v2 (capsule `private/slice206-license-guard-runtime`,
`ps2game.lib` unchanged). License demos that decrypt wrong (our FPU is not bit-exact, so 0 of 592 pass the game CRC; 52
of the 85 US demos land in the wrong phase) get record count 0xFFFFFFFF at both demo loaders (0x001100D0 and 0x00110230),
so the licenses are playable and those demos do not play, as in PCSX2 default. Memory-card loads check the CRC
themselves; replays and ghosts from a real PS2 card fail to load cleanly. Consumer map and remaining risk:
[LICENSE-DEMO-CIPHER](LICENSE-DEMO-CIPHER.md). Halt dump and FSV trace are built in, off unless GT4_HALT_DUMP /
GT4_FSV_TRACE. New `--play-card PATH` (a separate private test card, never `private/manual-play/card.ps2`). CTest
33/33 (native_memcard_store and native_license_guard now registered), Python suite with GT4_NATIVE_BUILD = slice206 all
passing. `pcsx2-slice203` and `license-guard` (slice205, guard v1) kept. Follow-up TASK-063: bit-exact div.s/mul.s.

Earlier (2026-10-16):

**slice203 (TASK-044, branch `task/044-gs-101`):** the slice202 game executable (same hash) with the stable gt4-gs 1.0.1
(tag `gt4gs-v1.0.1`, fork PR #7 merged as `d90c0f111`), which no longer writes under `Documents\PCSX2`. `--mode pcsx2` = slice203;
`pcsx2-slice202` kept. Checks: Python 472 run (18 skipped), CTest 29/30 (`python_native_contracts` uses the stale slice202 build path),
donor 42/42 = slice202, pcsx2 4200 frames 41/41 = slice202 (1x), no-JIT check of the 1.0.1 package passed, `Documents\PCSX2`
unchanged. [BUILD](BUILD.md) slice 203.

TASK-039 tooling follow-up (external contribution by Gustavo Canepa, PR #36, `fix/duplicate-gs-json-settings`): duplicate JSON setting ids now
raise the same error as duplicate CLI assignments. The 58 GS settings module tests pass, including
unchanged default INI output; no native build or gameplay validation was performed for this change.

**slice202 (main `b955235` merged into `task/060-062-osd`):** one candidate with the owner-approved TASK-058/059 (crash-safe
memory card, installed-folder paths, `--portable-check`, `make_distribution.py`) and TASK-060/061/062 (cursor, shader
notice, overlay) on the stable gt4-gs 1.0.0. CTest 30/30, Python 469 OK, donor 42/42, pcsx2 41/41 = slice201, the same
from the distribution folder, `--portable-check` ok. `--mode pcsx2` was slice202 until slice203 (gt4-gs 1.0.1). [BUILD](BUILD.md) slice202.

**slice201 (TASK-060/061/062, branch `task/060-062-osd`, fork `gt4gs/osd`, `gt4gs-v1.0.0` stable):** the mouse cursor
hides over the picture after 3 s without movement in fullscreen and windowed mode (game window and gt4-gs child agree
through protocol 1.2 CONTROL cursor; `GT4_CURSOR_AUTOHIDE=0` keeps it); gt4-gs shows "Compiling shaders…" bottom-left
while it really compiles (pipeline cache hits excluded); an optional top-right performance overlay (FPS, frame time,
CPU and GPU usage of the game and gt4-gs, resolutions, EE/VU1/GS write/readback/GS thread ms and the headroom) fed by
protocol 1.2 OSD_STATS (+0.35 us/frame on the EE thread, +0.02 ms/frame GS thread). Internal resolution presets 480p to
8K (default 1080p); gt4-gs 1.0.0 ignores INI keys outside the allow-list with a warning and is the **first stable
gt4-gs release** (zip sha256 `2e878019...`). CTest 30/30, Python 455 OK, donor 42/42, pcsx2 1x 41/41 = slice199,
no-JIT check passes. `--mode pcsx2` = slice201, `--mode pcsx2-slice199` kept. Owner windowed test pending.
[GS-LIVE](GS-LIVE.md) section 11.
**slice200 (TASK-058 crash-safe memory card, TASK-059 installed folder; branch `task/058-059-release-hardening` from main `3b5334d`):** `--mode pcsx2` = slice200 (`docs/inputs/slice200-pcsx2.json`, game build `private/slice200-build`, 95,680,000 bytes). The runtime writes the card atomically through the TASK-025 writer (memcard capsule, dirty retry, flush on exit and after the GS-loss grace VSYNCs). The executable finds `shaders\` and `gs\gt4-gs.exe` next to itself, has `--portable-check`, and `scripts/make_distribution.py` assembles the folder (`private/slice200-dist`, 16 files, no GS package, no game data; `docs/DISTRIBUTION.md`). Checks: CTest 29/29; Python kill test, portable-check and distribution tests; donor 42/42 identical to slice157; pcsx2 41/41 identical to slice199, both run from the distribution folder. The headless schedule never saves, so the card check is the fixture and kill test; the in-game save is the owner's test. `--mode main` is not rebuilt. No PR opened.

**slice199 (TASK-041 on the JIT-free gt4-gs 0.4.0, branch `task/041-gs-perf`, fork `gt4gs/nojit`, `gt4gs-v0.4.0`):** the slice198 game build (same file and hash) with the JIT-free gt4-gs 0.4.0 package of slice197. `--mode pcsx2` = slice199; `--mode pcsx2-slice198` and `--mode pcsx2-slice197` stay for A/B. Owner test of slice198 (gt4-gs 0.3.2) on 2026-10-16, night race with 5 AI: 59.97 FPS, 53 late frames (69 before), EE waits for the VU1 worker 2.99 ms (3.48 before), readback 0.69 ms, EE critical path 11.81 of 16.68 ms (71%), headroom 4.87 ms, no frametime spikes once the shader cache was warm. CTest 29/29, Python 438 OK, 20 skipped; donor 42/42 identical to slice157; pcsx2 1x 41/41 identical to slice198; no-JIT static check passes on the package. [GS-LIVE](GS-LIVE.md) sections 9 and 10. Owner night-race check on 0.4.0 pending.

**slice198 (TASK-041, branch `task/041-gs-perf`, fork unchanged, `gt4gs-v0.3.2`):** performance of the PCSX2 GS path.
A headless race schedule (El Capitan, 5 AI) reproduces the night-race privileged-write pattern: the 3.48 ms/frame
`gs_privileged` wait is the EE waiting for the frame's VU1 work at the CSR write right before the flare readback.
`GT4_MTVU_PRIV_ASYNC=1` (default) queues privileged writes in the MTVU journal: byte-identical stream and frames, the
gs_privileged wait is 0 but moves to the readback's download sync (no net gain expected in the night race). Long
readbacks are pipeline compiles (cold cache max 58 ms, warm 2.7 ms; TASK-046). Wake batching (`GT4_GS_WAKE_BYTES`)
cuts GS wake-ups from 322 to about 12 per frame and `gs_ipc_write` by about 30% per MB. F8 `[gs-ipc]` gains
per-register lines and an EE critical path summary. CTest 29/29, Python 416 OK, donor 42/42, pcsx2 41/41.
Owner test 2026-10-16 (see slice199 above). `--mode pcsx2` was slice198 until slice199. [GS-LIVE](GS-LIVE.md) section 9.

**slice197 (TASK-042, branch `task/042-gs-nojit`, fork `gt4gs/nojit`, `gt4gs-v0.4.0`):** JIT-free `gt4-gs.exe`. Build
option `GT4GS_ONLY`: no EE/IOP/VU/VIF recompilers, no x86 emitter, no GS software JIT (C rasterizer), no executable
memory; Windows Arbitrary Code Guard on at start (works with the AMD Vulkan driver). Static check passes (0.3.2: 22
markers); replayed menu frames byte-identical to 0.3.2 at 1x and 3x; GS thread time unchanged within noise. CTest
29/29, Python 414 OK, donor 42/42, live pcsx2 41/41 = slice196. `--mode pcsx2` was slice197 until slice199 (same game runtime). Owner
windowed check pending. [GS-LIVE](GS-LIVE.md) section 10.


**slice196 (TASK-047, branch `task/047-gs-deinterlace`, fork `gt4gs/deinterlace`, `gt4gs-v0.3.2`):** 480i menus in the
PCSX2 GS mode. Cause: the stream had no SYNCV (PCSX2 never deinterlaced; fields line-doubled and bouncing) and,
once SYNCV is sent, gt4-gs 0.3.1 used the previous VSYNC's field (wrong order, combing). Fix: SetGsCrt posts SYNCV;
protocol 1.1 VSYNC field = display field; gt4-gs 0.3.2 uses it. Garage menu: comb 0.50 -> 0.57 with flicker 3.23 ->
0; progressive unchanged. CTest 29/29, Python 410 OK, donor 42/42. `--mode pcsx2` = slice196. Owner menu check
pending. [GS-LIVE](GS-LIVE.md) section 8.

**slice195 (TASK-038 click fix):** owner night race with slice194: accurate 59.97 FPS, 69 late frames (was 213),
unsynchronized 60.00 FPS but headlight flares missing; accurate stays the default. A click on the picture deactivated
the game window; gt4-gs 0.3.1 gives the foreground back (visible click fixture passes). `--mode pcsx2` = slice195.

**TASK-037/038 (slice194, branch `task/037-038-gs-window`, fork `gt4gs/window`, tag `gt4gs-v0.3.0`):** night-race late
frames explained (one readback per frame removes the GS buffering, so shader/pipeline compiles and GS-thread spikes
land in the game frame; the shader cache was in `Documents\PCSX2`, not in our folder). Readback modes
`--gs-readback accurate|unsynchronized`, F8 readback histograms, our own cache/data folders, deferred swap chain
acquire, window hardening (detached input queues, minimize, close while the GS window hangs) and crash handling
(card flush grace, restart offer). CTest 29/29, Python 350 OK, donor 42/42, pcsx2 41/41. Owner windowed test pending.
[GS-LIVE](GS-LIVE.md) section 6.

**TASK-036 (slice193, branch `task/036-gs-live`, fork `gt4gs/live`, 2026-10-16):** the live PCSX2 GS process
`gt4-gs.exe` runs the game's GS stream (`--mode pcsx2`, `--gs-scale N`). Headless: CTest 29/29, Python 335 OK,
donor frames 42/42, live frames 41/41 identical to the offline `--replay-gscap` frames and frames 300/700 identical
to the TASK-034 PoC; a killed GS process halts the game cleanly. Owner windowed test at 1x and 3x pending.
[GS-LIVE](GS-LIVE.md).

## Earlier state (2026-10-15)

**Current renderer plan (2026-10-15, [decision 0070](decisions/0070-pcsx2-gs-separate-process.md)):** TASK-032 (decision docs) is done in its PR; TASK-034 (PCSX2 GS proof of concept) is in progress, with an owner go/no-go gate before TASK-035/036. paraLLEl-GS (TASK-012) is dropped; CoreGS 1.1 stays the built-in fallback. Plan: [GSDX-PLAN](GSDX-PLAN.md).

**TASK-035 (slice191, branch `task/035-gs-stream`, 2026-10-15):** the MIT GS stream protocol ([spec](GS-STREAM-PROTOCOL.md)), shared-memory transport and the `GT4_RENDERER=pcsx2` seam are built and checked headless (CTest 29/29, Python 321 OK; donor frames 42/42 identical; the stream is byte-identical in donor mode, pcsx2 mode and in the recorder process; the converted stream equals the TASK-034 PoC dump and replays to identical GSRunner frames). The consumer is a test recorder, so `--mode pcsx2-stream` shows no image until TASK-036. Owner review pending.

**Latest (merged to main):** TASK-008 slice 157 (all 2,048 VU1 entries, owner: night
58.9, rally 59.9, showroom 60.0 VBlanks/s, 0 interpreted VU1 calls), TASK-010 build
pipeline (slice 159) and TASK-009 slice 164 `--mode quickfix3 --force-480p` (title,
16:9, fullscreen, races held at 480p; owner approved). In flight: TASK-012 paraLLEl-GS
(step 5 headless, textures missing), TASK-013 VU0 AOT (owner test), TASK-014 stage A.
TASK-011 MTVU (branch `task/011-mtvu`, release 1.0.0 W1): slice 178 `--mode main` runs the VU1 worker
(slice 172 runtime, `GT4_MTVU` default on, `GT4_MTVU=0` is the A/B switch) and holds 480p by default.
Headless 42/42 identical with MTVU on (three runs), off (one run) and default. PR ready for review; owner A/B and approval pending.

**Earlier:** slice 154 `--mode vu1-flags` (executable `800da980`) adds VU1 flag
elision: showroom 59.14 VBlanks/s (was 44.79), 5-AI race 60.00 at 480p. Next:
full static VU1 entry coverage: slice 155 `--mode vu1-full` (426 entries) is
built and checked headless, waiting for the owner test.

2026-10-15 — Slice 154, O1 follow-up (candidate, not owner-measured): `--mode vu1-flags` = slice 153 pacing with a VU1 table that computes MAC and clip only where a reader or the exit observes them (exit state exact); VU1 status is not kept, since the image never reads it and the EE cannot read VU1 flags. Local evidence: corpus differential 0 failures over 100 programs, fixture differential 0 failures, 42 identical headless frames versus slice 153, verify smoke with 0 mismatches. Read [journal 2026-10-15](journal/2026-10-15.md) and [BUILD](BUILD.md).

Integration branch `main`. Machine: Ryzen 7
5700X, RX 9060 XT. Plan: [OPTIMIZATION-PLAN](OPTIMIZATION-PLAN.md). Journal:
[2026-10-15](journal/2026-10-15.md).

**Playable candidate:** slice 153 `--mode pacing` (executable `c8d31fdf`):
VU1 AOT (102 entries) + asynchronous GS submission + single-clock pacing with
rate-controlled audio. A/B switches: `GT4_PACING=audio`, `GT4_GS_SYNC=1`,
`--vu1-interpret-only`, `--vu1-verify N`, `GT4_PRESENT_MODE`.

**Confirmed (owner measurements):**

| Scene | VBlanks/s | Speed | Notes |
|---|---|---|---|
| Time Trial, Nordschleife, Supra, 480p | 60.00 | 100% | median 16.665 ms, 99.6% of intervals 16-17 ms |
| Arcade race, Nordschleife, 5 AI | 57.98 | 96.6% | VU1 8.4 ms |
| Replay | 59.10 | 98.5% | |
| Night race | 46.15 | 76.9% | uncompiled VU1 entries (3.7 ms interpreted) + 4.1 ms GS wait (readback suspected) |
| Rally, Grand Canyon | 53.85 | 89.8% | uncompiled VU1 entries, VU1 11.3 ms |
| Car showroom | 44.79 | 74.6% | VU1 16.2 ms, 46k primitives/frame |

Starting point the same day was about 30 FPS and 51% speed (VU1 interpreted
17 ms, synchronous GS wait 10.8 ms). VU1 AOT verified on real game state
(140,034 in-game comparisons, 0 mismatches). RTSS overlay works on AMD since the
swapchain requests color-attachment usage.

**Known issues:** license tests crash in every configuration (pre-existing;
traced to `RaceSolitaire::loadDemoInputs` → `AutomobileControlRecord::Manager::load`
reading a bad record count); night/rally VU1 entry coverage; showroom VU1 load;
night GS readback wait; counters `late_frames`/`audio_underruns` look
session-cumulative.

**In progress / next:** VU1 per-program flag elimination (slice 154, subagent);
then full static VU1 entry coverage, MTVU (VU1 thread, default on), window title
"GT4Recomp" and fixed 16:9 (remove F6/4:3), license-crash instrumentation, LDL/LDR
semantics check, GS readback counting/staging, Vulkan (GS) internal scale 2-3x
experiment, then O3 (VU0 AOT, EE entry coverage, IOP review, build tuning).

**Research done (docs):** [VU1 microcode source](VU1-MICROCODE-SOURCE.md),
[VU0 map](VU0-MICROCODE-MAP.md), [IOP map](IOP-MAP.md),
[decomp identity](DECOMP-IDENTITY.md), [decomp integration](DECOMP-INTEGRATION.md),
[GS async design](GS-ASYNC-DESIGN.md), [PCSX2 lessons](PCSX2-LESSONS.md),
[hardware renderer plan](HW-RENDERER-PLAN.md), [Spec II study](SPEC2-STUDY.md),
[menus and wheel](MENUS-AND-WHEEL.md), [online notes](ONLINE-NOTES.md).

## History (newest first)

2026-10-15 — Slice 178, release 1.0.0 W1 (candidate, not owner-measured): `--mode main` is the build of main with the
MTVU worker runtime (slice 172 capsule, byte-identical; `GT4_MTVU` default 1, 0 is the single-thread path) and holds the race
display at 480p by default (hidden opt-out `GT4_FORCE_480P=0`). Build `private/slice178-main-build` (pin `docs/inputs/slice178-main.json`),
CTest 27/27, Python 281 OK (12 skipped), headless 4,200 frames 42/42 identical to slice157 in four runs (on x3, off x1) plus one
default-environment run. `--mode mtvu` keeps slice 172. Details: BUILD.md "Slice178", MTVU-PLAN section 12.

2026-10-15 — Slice 153, O4 frame pacing (candidate, not owner-measured): `--mode pacing` (VU1 AOT plus asynchronous GS, as slice 152) with one 60 Hz wall-clock guest VBlank grid, non-blocking audio (16 x 256-frame buffers, 60 ms target, +-0.5 % resample control), and `GT4_PACING=audio` for the old blocking clock. FIFO stays default; `GT4_PRESENT_MODE` for experiments. Local checks: pacing and paced-device fixtures, CTest 18/18, Python 217 OK, 42 identical headless frames versus slice 152. Owner measurement pending. Read [journal 2026-10-15](journal/2026-10-15.md) and [BUILD](BUILD.md).

2026-10-14 — Slice 152, O2 asynchronous GS submission (candidate, not owner-measured):
`--mode gs-async` = VU1 AOT plus a three-slot submission ring on one timeline semaphore,
presentation merged into the slot submission, at most two presents in flight, cache
eviction guarded by completion, and `GT4_GS_SYNC=1` for synchronous A/B. Confirmed headless:
4200-frame dumps identical sync vs async; stress VRAM byte-identical; CTest 17/17; Python 203
OK. Synchronization validation is clean with overlay layers disabled; with the installed
overlay layers it reports present-after-write (pre-existing class). Step 0: swapchain now
requests COLOR_ATTACHMENT (overlay layers previously hit render-pass VUIDs). Owner gameplay,
overlay check and frame timing pending. Read [GS async design](GS-ASYNC-DESIGN.md#implementation-status-slice-152).

2026-10-14 — Slice 151, owner replanning: [Optimization Plan](OPTIMIZATION-PLAN.md)
and [decision 0065](decisions/0065-aot-all-guest-code-no-jit.md) set the order of work:
VU1 AOT first (donor recording + `vurecomp` flow never activated for GT4; all VU1
calls hit the interpreter), then asynchronous GS submission, then VU0-micro/IOP AOT
and EE build tuning. Read-only source review; no build, run or code change.
Branch `codex/optimization-plan-151` from slice 150.

2026-10-14 — Slice 150, owner split report identifies VU1 interpreted wall cost.
Playable base remains slice114; slice143 includes slice136's keyboard correction.
Active track: `codex/hybrid-no-jit-port-plan-141`; slice143 based on5c7ab92.
`main` retains slice140/native-scene work; this candidate stays on the hybrid track.
Slice 118 candidates remain separate and owner gameplay qualification is pending.

## Active priority on this branch

**Confirmed (slice143):** portable timing core is connected to a separately pinned
production-EE player. Measures exclusive/inclusive wall categories and owner-thread
CPU time, clips active scopes and guest counters at VBlank boundaries, preserves
keyboard correction, excludes shape capture and inherited guest tuning settings.
F7 starts a continuous manual interval, F8 reports, F9 pauses/resumes diagnostics.
Candidate native CTest7/7; separate macro-off observation controls9/9,
Python171/171 zero skips, warning-free builds. Launcher retains card/backup/lock flow.
**Confirmed (slice144):** owner F8 report saved completely:970/970 samples across
40.323s,24.06 VBlanks/s; no invalid/reset marker. Exclusive wall sum reconciles
within printed rounding. VU interpretation25.646ms/sample is61.69% of the interval;
GS fence wait9.392ms/sample is completion latency, not GPU execution time.
Owner identifies that interval as Toyota Prius, external camera, Nordschleife;
year/trim, exact position and GT4 video setting are not supplied.
**Unknown:** diagnostic overhead and exact interval scene composition,
distinct scene frame rate and GPU timestamp time. No performance improvement
claim. VU is the leading observed wall category; qualify overhead and the matching
scene before choosing an optimization. No runtime changes in this verification.
**Confirmed (slice145):** fresh detailed and frame-only profiles share production
EE/runtime/port/shaders and all guest settings. Frame-only compiles subsystem
scopes and host operation counter hooks out; common boundary CPU/wall clocks and
guest progress counters remain. Native CTest7/7 each, separate full9/9 with
Python173/173 zero skips. Object-symbol controls confirm no Probe references in
five frame-only hot-path objects. Owner closed143 normally and completed the145
detailed trial recorded below. Matched control intervals remain pending.
Next: owner matched detailed/control/detailed intervals, then VU0-micro/VU1 timing
separation and one content-identified offline AOT experiment. No speedup claim.
**Confirmed (slice146):**145 detailed manual interval covers962 samples/40.732s,
23.62 VBlanks/s, median42.065ms; VU26.246ms (61.99%), GS fence9.392ms,
owner-thread CPU32.533ms/76.8%. SecondF8 covers only2.704s, recorded separately.
Independent review and arithmetic controls pass; detailed session closed normally,
card hashes unchanged. Owner confirmed closure;145 frame-only now open through
protected launcher. Its manual interval/overhead result remain pending.
Owner screenshot is a single logical-CPU/overlay snapshot: no fixed CPU at100%
does not rule out serial work with waits/migration; neither migration nor thermal
limiting is measured. See [detailed evidence](inputs/slice146-owner-detailed-results.json).
**Confirmed (slice147):**145 frame-only report covers1175 samples/56.199s,
20.91VBlanks/s, median46.311ms,p9983.614ms, threadCPU37.274ms/77.9%.
Complete five-line report, normal closure, unchanged card and independent review
pass. It is slower than detailed in this one sample; different durations/scenes/
machine state prevent causal overhead attribution. Protected145 detailed repeat
now open, owner controls F7/F8. No optimization or thermal-cause conclusion.
See [control evidence](inputs/slice147-owner-control-results.json).
**Confirmed (slice148):** detailed repeat1191 samples/53.161s, median43.779ms,
p9956.683ms, threadCPU35.094ms/78.6%, VU28.155ms. Normal close/card unchanged;
exclusive partition and independent log checks pass. Initial sequence medians
42.065/46.311/43.779ms: control slower than both detailed runs. Causal overhead
remains Unknown under varying scenes/durations/machine state; no more repeats of
this initial sequence requested now. Next is a fresh VU0-micro/VU1-interpreter
scope split, keeping149+ candidate separate from frozen143/145. No optimization.
See [sequence evidence](inputs/slice148-owner-aba-results.json).
**Confirmed (slice149):** fresh pinned source capsule replaces aggregate run_fast
timing with disjoint VU0-micro/VU1-interpreter categories. Old aggregate stays zero;
derived rollup is separate from the16-category partition. Core controls prove70us
aggregate equals20+50us split after active cut/nested waits. Native7/7, separate
full9/9 with Python175 zero skips. Initial category rejection was caught/corrected
by fixtures/review. Frozen143/145 unchanged; no algorithm/clock optimization.
Next owner F7/F8 interval identifies the slower interpreter unit. VU0 macro in EE
and generated VU1 AOT are outside this scope. [Candidate](inputs/slice149-vu-unit-attribution.json).
Independent final artifact gate passed; protected149 player initialized native
Vulkan/audio with verified backup, then owner completed and closed the150 session.
**Confirmed (slice150):** completed149 interval1139 samples/49.161s,
median42.711ms. VU1 interpreted26.987ms is62.53% of interval wall; VU0 micro
prints0.000ms (rounded, not proof of no calls). VU0 macro remains in EE; generated
VU1 AOT excluded. GS completion wait9.367ms is next largest category. All16 terms
sum within rounding; normal close/card unchanged. [Evidence](inputs/slice150-owner-vu-split-results.json).
Owner additionally reports menu pacing sometimes43FPS then60FPS in the same
opening. Screenshot/description recorded only; cause and transition timing Unknown.
Next: bounded VU1 content-key/call inventory before selecting one offline-AOT
program. No menu audit/fix or runtime optimization in150; no new game launch.
See [pair protocol](PERFORMANCE-ATTRIBUTION.md#matched-overhead-pair-slice145)
and [VU AOT feasibility](VU-AOT-FEASIBILITY.md).
See [attribution](PERFORMANCE-ATTRIBUTION.md#playable-diagnostic-candidate-slice143)
and [candidate pins](inputs/slice143-performance-candidate.json).
Protected candidate launch confirmed native Vulkan/16:9/48kHz initialization and
per-session card backup; owner controls the open game. Manual report and immutable
observed prefix preserved privately; see [report evidence](inputs/slice144-owner-performance-report.json).
Slice142 reconciled OpenCode's old executable/receipts and reproduced accounting
defects; its reported494-interval selection remains unreproduced.

Owner changed direction: optimize the hybrid base for correct game speed and
stable60FPS on Windows, then add GS internal scaling/dithering and portability.
Mobile targets: S23 Ultra/Snapdragon8 Gen2 and iPhone15 Pro Max; Windows first.
Guest JIT is excluded. AOT EE and selectively compiled VU may coexist with
qualified instruction interpreters/device models. No zero-emulation claim.
Selective PCSX2 GS implementation reuse is authorized; GPL-compatible derivative
planning and attribution are required before imports. No code/license change yet.

Independent Luna source review confirms Windows host/COFF pins, Win32 WSI and
mandatory presentation-extension dependencies. Donor has offline VU compilation
and scale-aware1..3 GS code, not qualified in our current1x adapter. Costs and
ARM64/mobile60FPS remain Unknown. No profiling/build/game action in this slice.
See [Hybrid Port Plan](HYBRID-PORT-PLAN.md), [decision0064](decisions/0064-hybrid-no-jit-windows-first.md)
and [next H0/H1 package](tasks/HYBRID-H0-H1.md).
Read-only source hashes/review limits are published in
[slice141 evidence](inputs/slice141-hybrid-port-plan.json).

The game-scene replacement research below is preserved and deferred on this
track. Its next richer shape capture is not the default next task here. Existing
OpenCode experiments stay separate; review their measured evidence before reuse.

## Authorized P0/P1 Progress

- **Confirmed (slice 140):** exact pinned offline/capture comparison and independent
  Luna parsing align18 of22 menu Ford Ka shapes at candidate MDLS base0x01748740,
  accounting for97 calls. Other53 menu and all60 race-LOD candidate bases match
  one distinct address each. Four menu shapes are unobserved;12 other live addresses
  are outside the candidate menu extent. Ten authored matching controls pass.
- **High confidence:** partial loaded menu-asset layout correspondence.
  **Unknown:** resource identity/lifetime, descriptor relocation/payloads, car-part
  roles, camera/transforms and geometry. No runtime/build/session/performance change.
  Next: bounded prefix/descriptor observation; see [crosswalk](LIVE-SHAPE-CROSSWALK.md)
  and [evidence](inputs/slice140-shape-crosswalk.json).

- **Confirmed (slice 139):** owner capture exports valid schema1,180767 bytes,
  256 completed records, all before/after validity masks0x3f. Started/completed257,
  dropped1 reconcile with automatic record-cap stop; no orphan/abort/rejected/
  ambiguous/incomplete sample counters. Independent bounded JSON review matches.
- **Owner context:** screenshot shows Ford Ka '01 in Arcade selection and
  `capture saved`. **Unknown:** which of30 distinct raw shape arguments belong
  to that car, geometry and submission ownership. No timestamps/frame IDs exist;
  capture speed/duration is not measured. Original preserved and frozen private
  copy hash-matched. Game remains owner-controlled. See
  [capture verification](inputs/slice139-live-capture.json).

- **Confirmed (slice 138):** owner reported capture began before the car appeared
  and explicitly requested close/reopen. Delivered normal close; exit0/session
  completion confirmed, previous capture preserved. Qualified136 reopened with
  the same save, fresh backup/session and capture off. This authorization covers
  this restart only; owner still controls scene navigation and F8. Target scene
  remains unqualified. See [retry evidence](inputs/slice138-owner-capture-retry.json).

- **Confirmed (slice 137):** owner requested opening the corrected build. Old134
  session closed normally with exit0; qualified136 runtime launched with existing
  save and a fresh8650752-byte backup. Native window/audio initialization logged.
  Owner controls keyboard and F8; no agent scene navigation or performance probe.
  Menu/steering/capture identity confirmation remains pending. See
  [corrected launch evidence](inputs/slice137-corrected-owner-launch.json).

- **Confirmed (slice 136):** owner separately authorized the input fix blocking
  car selection. Arrows previously emitted both a digital D-pad bit and a full
  left-stick axis; keyboard merge now emits only buttons/pressures. XInput axes,
  rumble and all other button mappings remain unchanged. No arbitrary debounce.
- **Confirmed (slice 136):** fresh CTest7/7 in13.03s and Python164/164, zero skips.
  Native controls cover all16 keyboard bits, four directions, hold/release/chord
  and preservation of non-neutral XInput axes. The same new fixture rejects the
  old implementation specifically for non-neutral keyboard-derived axes.
- **High confidence:** duplicate digital/analog channels explain the reported
  menu double movement. **Unknown:** owner keyboard/menu confirmation and digital
  D-pad steering in races. The strict capture launcher now selects the frozen136
  candidate; physical input/game navigation remains owner-controlled.
  See [keyboard evidence](inputs/slice136-keyboard-input.json).

The owner authorized P0/P1 while OpenCode continues its separate performance
branch. Work uses an isolated checkout; no OpenCode edits, session or saves changed.

- **Confirmed (slice 131):** all 328 reviewed two-probe sources copied into a
  fresh producer and all 327 AMD64 objects rebuilt. Exactly 326 normalized object
  payloads match baseline; only D5 changes and matches its independent slice 129
  unit compilation. Compiler recipe differs only in generated-source directory.
- **Confirmed (slice 131):** producer CTest 1/1, Python 155/155 with zero skips,
  using frozen slice 130 native fixtures. This does not repeat their native build.
- **Unknown:** retail runtime linkage, live shape identity/reachability and capture
  overhead. The ordinary playable target and launcher remain unchanged.
  See [full library evidence](inputs/slice131-observation-library.json).
- **Confirmed (slice 132):** fresh `observation` native profile links the pinned
  full probe library and reviewed runtime overlays directly into its game target.
  Ordinary production selection is unchanged; the runtime archive stays original.
  Capture starts disabled and empty; activation/export remain unimplemented.
- **Confirmed (slice 132):** CTest 5/5 and Python 155/155, zero skips. Actual game
  map/graph checks cover retained EE hooks, boot, adapter and D5. Synthetic guest
  outputs still match their unoverlaid control. Invalid profile/missing overlay,
  extra header and post-configure source edit are rejected before compilation.
- **Unknown:** this candidate's boot/gameplay, live shape association and overhead.
  No game session launched. See [runtime evidence](inputs/slice132-observation-runtime.json).
- **Confirmed (slice 133):** fixed metadata-only JSON preserves every raw argument
  lane, FPR/SP bits, original owner identities, counters and before/after validity.
  It requires a stopped observer and exclusively creates a new file. Actual native
  controls cover 0/1/256 records, no overwrite, invalid reason and missing parent.
- **Confirmed (slice 133):** fresh CTest 6/6, Python 156/156, zero skips. Exporter
  remains fixture-only; no window hook, game activation or owner capture added.
- **Unknown:** injected write/flush/cleanup/close failures and crash atomicity;
  partial cleanup may fail. The host path is caller-trusted; no public path UI.
  Next: owner capture latch at the EE vblank boundary and fresh private launcher
  output, then owner-led scene evidence. See [export evidence](inputs/slice133-capture-export.json).
- **Confirmed (slice 134):** observation-only F8 latches a start/stop request;
  the EE-owned vblank host boundary services it. One capture per launch, default
  off; manual, capacity/ambiguity stop and shutdown preserve/export fixed records.
  Failed export retains records for F8 retry or one best-effort shutdown attempt.
- **Confirmed (slice 134):** launcher pins the exact manual manifest/runtime,
  clears inherited capture output, uses a new private session path and preserves
  the existing card/backup/locks. CTest 7/7 in11.72s, Python164/164, zero skips.
  Link evidence retains the control/export code; baseline graph excludes it.
- **Unknown:** physical F8, actual scene reachability/identity and runtime cost.
  Synchronous bounded export may pause a frame; no gameplay/performance probe was
  used for build qualification. Owner-led capture is next. See
  [manual capture evidence](inputs/slice134-manual-capture.json).
- **Confirmed (slice 135):** qualified134 candidate launched after checking no
  existing game process. Native window/Vulkan, Windows audio and guest IOP boot
  services are present in the startup log. Existing owner save selected; session
  backup8650752 bytes created. Capture starts off; no capture file at snapshot.
- **Unknown:** menu/race scene, physical F8 and real shape identity. No agent GUI
  navigation/input or performance measurement. The owner controls the open game.
  Startup proof is frozen separately from changing live session logs:
  [launch evidence](inputs/slice135-owner-capture-launch.json).

- **Confirmed:** fresh CORE-to-ELF reconstruction matches the pinned native ELF;
  independent verified ISO extraction produces the same CORE/ELF.
- **Confirmed:** the donor translator rebuilt with Clang 23.1.2 generates all
  328 source files identically to the frozen producer. No old CMake cache is used.
- **Confirmed:** the fresh library's 327 AMD64 object payloads match the baseline
  after normalizing only their COFF timestamps; archive paths explain the size delta.
- **Confirmed (slice 117):** producer CTest 2/2 passes; Python discovery 55 tests,
  52 passing and 3 native integration skips in the isolated checkout. These are
  not candidate runtime or gameplay qualification.
- **Confirmed:** seven model/camera candidates map to real generated registry
  entries and prefix hashes; camera helpers and model VM/callbacks have state effects.
- **Confirmed:** baseline callback starts `0x0044EA58`/`0x0044EAC8` lack generated
  bodies/entries. An offline supplier-only seed recovers both, adding 45 instructions
  and nine entries. Supplier-only/both source matches; all 18 changed units compile.
  Existing bodies/callers match under independently reviewed dispatch-ID normalization.
  Production seeds/library are unchanged. The donor EE dispatcher halts on missing code without a module
  replacement; it has no EE interpreter fallback.
- **Unknown:** live reachability of these omissions and any performance relationship.
  Generation equality is not complete-code coverage or PS2 fidelity proof.
- **Confirmed (slice 118):** complete recovered library compiled in a fresh cache;
  all 327 normalized AMD64 object members checked: exactly 18 reviewed differences,
  309 unchanged. Baseline remains the qualified rebuilt slice 116 library.
- **Confirmed (slice 118):** both isolated native runtime profiles link and pass
  CTest 3/3 and Python 59/59 with zero skips and zero observed compile warnings.
  All nine shaders match production. The actual candidate executables are used
  by native integration fixtures; no manual game session was launched during build qualification.
- **Confirmed (slice 118):** bounded research located the actual GT4.VOL header/
  offset table; parent independently verified its metadata. Page decoding, named
  car/asset/live-instance identification were Unknown at that checkpoint. No car payload was extracted.
- **Confirmed (slice 119):** six pinned research repositories cloned; Luna decoded
  all 407 ToC pages with bounded raw-DEFLATE-before-XOR processing. Parent replay
  independently matches decoded hash, 18,562 records, 210 directories, directory
  reachability and 18,352 file extent bounds. Car/SpecDB paths are now observed.
  Packed version matches GT4FS's 0x00030001 constant.
- **Unknown:** production reader/lookup qualification, named-car crosswalk,
  model/texture payloads and live instance/camera effects. No asset payload was
  read and no renderer or performance change was made in slice 119.
- **Confirmed (slice 120):** public bounded volume reader reproduces slice 119
  catalogue hashes/counts. Five selected SpecDB files extracted after whole-ISO
  verification on the same handle; strict PS2ZIP framing, sizes and hashes pass.
- **Confirmed (slice 120):** offline `ka_01` / car code 59 resolves to Ford Ka '01
  in USA GT4_US2560. Name/Grade/ShortName use localized string 84; NarrationID
  is raw integer 435. Worker and independent parent code-list Huffman replay
  match both selected rows. SpecDB decoding remains a private research probe.
- **Confirmed (slice 120):** 22 new public controls pass; existing baseline
  candidate CTest 3/3 and Python 81/81, zero skips. No native code/build change.
- **Unknown:** which menu/LOD file corresponds to this car; exact `ka_01`
  basenames and MODEL_INFO pair are absent. Eleven Ford-prefixed leaves remain
  candidates, not identified assets. No model/texture payload read or live hook added.
  B-tree key routing and comprehensive format/fidelity qualification remain open.
- **Confirmed (slice 121):** CAR_VARIATION row 59 selects VARIATION row 444/table
  42; all 11 physical variations use SDB model index 1598, `fdeu0006`. Independent
  Huffman replay matches all 12 selected row hashes. Exact garage/LOD leaves,
  not guessed Ford prefixes, are identified and extracted with verified pins.
- **Confirmed (slice 121):** both CAR4 main MDLS bases are 0xC00; garage/race
  have 4 models each and 22/113 shapes. Independent header checks match. Signed
  -1 VM words are opaque; five zero-size Tex1 slots are reported header-only.
  Geometry, texture records/pixels, relocation and live argument chain remain Unknown.
- **Confirmed (slice 121):** 17 public header controls; CTest 3/3, Python 98/98
  with zero skips. No native rebuild, live hook or runtime behavior change.
- **Confirmed (slice 122):** selected garage/racing main models contain 154/312
  bounded shape descriptors, 14/18 material records and 49/123 PGLU texture
  records plus 5/15 transfer records. Packet spans and fixed arrays are bounded;
  shape +4 is opaque, transfer storage extent and material semantics Unknown.
- **Confirmed (slice 122):** 11 new public controls; CTest 3/3, Python 109/109,
  zero skips. No VIF/vertex/pixel decoding or runtime change. Independent record
  review is recorded in [Model Records](MODEL-RECORDS.md).
- **Confirmed, conditional (slice 123):** under explicit initial CL/WL=(1,1),
  all 466 selected packet spans frame to their exact QWC end. No STCYCL appears,
  so live incoming cycle remains Unknown. No VIF/VU execution, GIF meaning,
  channels, topology, vertices or renderer substitution is qualified.
- **Confirmed (slice 123):** 11 public framing controls, CTest 3/3, Python
  120/120, zero skips. Fill cycles reduce source consumption; NUM0 expansion
  is explicit. Packed/reserved UNPACK and zero-cycle fields reject as unqualified.
- **Confirmed (slice 124, static only):** pinned ELF corroborates shape argument+4
  loaded to F12 and helper `0x0049E548`'s conditional matrix/VU and 144-byte buffer
  effects. Submission reads descriptor addresses directly; serialized relocation
  and selected live object identity remain unqualified.
- **Confirmed (slice 124, static only):** three local packet builders emit (0,4)
  then (4,4) cycle words. Channel submission, previous/live cycle and selected
  Ford Ka relation remain Unknown. Channel-role formulas remain reference leads.
- **Confirmed:** documentation-only slice, source/ELF pins and bounded independent
  review pass. Existing CTest 3/3 and Python 120/120 are inherited from slice 123;
  no new build/test or runtime change claimed.
- **Confirmed, conditional (slice 125):** 2,320 ordinary UNPACK commands supply
  157,441 raw scalar lanes under explicit cycle/signedness assumptions. Integer
  decoding preserves 32-bit patterns and opaque alignment; no channels, floats,
  geometry, VU results or live behavior inferred.
- **Confirmed (slice 125):** 11 new literal controls; CTest 3/3, Python 131/131,
  zero skips. Exact selected pins and frozen framing reproduced; no native rebuild.
- **Confirmed, conditional (slice 126):** source predicates match a position
  candidate in 466/466 packets but float-normal candidates in only 6/312 racing
  packets; 23 garage UV candidates have three components and would be skipped.
  Independent derived-data audits and complete qualified-header check agree.
- **Unknown:** actual channel/normal/UV/topology semantics. No converter adopted;
  predicate coverage is not game geometry proof. Research-only slice inherits
  CTest 3/3 and Python 131/131 from125; no new runtime/build/test claim.

- **Confirmed (slice 127, source only):** existing donor probes can observe pre-call
  0x00455A5C and registered continuation 0x00455A64 without translator changes.
  Direct dispatch bypass, depth spill/resume, stale pre-PC and reused thread IDs
  require original-input tokens and lifecycle invalidation, not a post-only hook.
- **High confidence proposal:** default-off fixed metadata observer, bounded RAM
  reads and no duplicate game evaluation. New generation/profile and lifecycle
  adapter still required. No live hook, build or gameplay qualification this slice;
  CTest 3/3/Python 131/131 inherited from125. See [Observation](SCENE-OBSERVATION.md).

- **Confirmed (slice128):** default-off fixed observation core and safe direct
  RAM/scratchpad sampler pass actual native synthetic controls. Persistent
  original inputs, exact continuation/SP, owner isolation, incarnation/epoch,
  handler skips and deterministic storage/ambiguity failure are exercised.
- **Confirmed:** fresh baseline-profile Windows/Clang build, CTest4/4 and
  Python132/132 with zero skips. Core is fixture-only, not linked into playable
  runtime. Generated probes, lifecycle adapter and live evidence remain pending.

- **Confirmed (slice129):** separate fresh --probes generation changes only D5:
  exactly two27-byte statements at intended pre/return frontiers. All327 other
  files, including registry/header, match frozen116 byte-for-byte. Independent
  removal of the two lines restores D5 exactly; baseline producer gates unchanged.
- **Confirmed:** changed D5 compiles with qualified Clang/VS ABI. Producer CTest1/1
  and Python141/141, zero skips, reuse128 native fixtures. No full probed library,
  lifecycle/runtime adapter, new game executable or live capture yet.

- **Confirmed (slice130):** optional fixture-only adapter observes actual runtime
  probes, preserves original shape inputs across a synthetic handler dispatched
  exactly once, and invalidates tokens through real kernel lifecycle/load paths.
  Three pinned source overlays invert exactly to donor bytes after removing hooks.
- **Confirmed:** fresh Clang build, CTest5/5 and Python147/147, zero skips.
  Unoverlaid/observed controls emit identical46,036-byte defined-field transcripts.
  Link-map/COFF gates select reviewed direct EE bindings; game link excludes hooks.
  Extra headers and post-configure source edits reject before altered compilation.
- **Unknown:** retail call reachability, full probed-library/game linkage, geometry
  and live scheduling/overhead. Owner session unchanged; no performance probe or
  drawing replacement. See [Observation](SCENE-OBSERVATION.md).

See [EE Producer](EE-PRODUCER.md), [Render Contracts](RENDER-CONTRACTS.md) and
[Callback Coverage](EE-CALLBACK-COVERAGE.md), with [slice 116](inputs/slice116-ee-producer-contracts.json)
and [slice 117 evidence](inputs/slice117-ee-entry-coverage.json), for build/test
results and remaining gates. [EE Candidates](EE-CANDIDATES.md) and
[Car Asset Research](CAR-ASSET-RESEARCH.md) cover slice 118. P0/P1 remain **in
progress**: [Nenkai References](NENKAI-REFERENCES.md) and
[slice 119 metadata](inputs/slice119-nenkai-assets.json) record subsequent catalogue
progress. [Volume Reader](VOLUME-READER.md) and [slice 120 metadata](inputs/slice120-car-identity.json)
record the selected extraction and offline identity; candidate gameplay, remaining entry coverage and one real car's
data/conventions are pending. Production library, default launcher and runtime
have not been replaced. Candidate checks do not qualify game boot or racing.
See [Model Asset Probe](MODEL-ASSET-PROBE.md) and
[slice 121 metadata](inputs/slice121-model-file-map.json) for the now-confirmed
offline file mapping and exact header-only parsing boundary. P1 remains open.
[Model Records](MODEL-RECORDS.md) and
[slice 122 metadata](inputs/slice122-shape-records.json) extend this to fixed
record and declared packet-span inventory; renderable geometry remains pending.
[VIF Framing](VIF-FRAMING.md) and [slice 123 metadata](inputs/slice123-vif-framing.json)
record conditional framing, corrected source assumptions and remaining live-state gates.
[Static VIF State Contract](VIF-STATE-CONTRACT.md) and
[slice 124 metadata](inputs/slice124-vif-state-contract.json) record submission
effects, buffered cycle words and the boundary before raw lane/geometry decoding.
[Raw Channel Lanes](RAW-CHANNEL-LANES.md) and
[slice 125 metadata](inputs/slice125-raw-channel-lanes.json) extend this to
controlled raw integer inputs, with channel/topology/geometry gates still open.
[Channel Selection Audit](CHANNEL-SELECTION-AUDIT.md) and
[slice 126 metadata](inputs/slice126-channel-selection.json) record reference
coverage gaps before any semantic converter or native car renderer is adopted.
The owner's active OpenCode baseline session was preserved during build qualification.
On the owner's subsequent explicit request, it closed normally and the slice 118
baseline candidate launched at 13:16 local time. Its window responded and module
loading progressed; owner gameplay confirmation remains pending. The existing
card, normal session locks and per-session backup were used. This startup
observation does not qualify racing or performance. Recovered remains unlaunched.

## Active Path

`native/` + MIT ps2-web-recomp runtime frozen at
1b4808793a12c45125262fd2d5f50a9ec9f7c020. EE translated AOT to x86-64,
IOP/VU interpreted, and PS2 hardware/services modeled in software. Vulkan compute,
Win32, WinMM, and XInput. Does not use a browser, WebAssembly, or embedded PCSX2.

Earlier pre-donor work was removed from this repository and lives in the private
`gt4-staticrecomp-legacy` repository. Private data, saves, and frozen producers remain local.

## Confirmed Results and Limits

- Boot/menus, car/track selection, and genuine Nürburgring racing gameplay; limited 9,000-frame automated controls.
- In-game 16:9 camera/HUD and default 16:9 window framing; 4:3 optional.
- Owner manually tested 480i/480p/1080i during gameplay, all in 16:9; 480p subjectively
  most stable. Human functional confirmation without independent proof of scan fields.
- 48 kHz stereo PCM delivered to Windows; buffer underrun/continuity issues remain open.
- XInput/analog/rumble implementation covered by test fixtures; physical controller hardware untested.
- Persistent memory card, profile/settings saved by the game, per-session backup, and mutual exclusion lock.

Severe performance and frametime degradation recorded: 19 FPS in-race, 11 FPS in 3D car showroom,
18 FPS / 72.5 ms in 1080i screenshot; overlay metrics are not independently calibrated.
Keyboard arrow keys produced double-movements per keystroke per owner report; other keys
remain suspect. Slice136 removes the confirmed dual digital/analog keyboard mapping;
live menu/steering confirmation remains pending. The prior record-only performance restriction is now
superseded for an owner-delegated OpenCode investigation; Codex prepares context only.
The owner separately authorized this keyboard correction. Gameplay is owner-controlled: no Computer Use,
GUI automation, simulated input or scripted navigation. Manual play window and memory card are untouched
by this cleanup. CPU/GPU fidelity, CFC2(28), temporal deinterlacing, 100% realtime rate,
prolonged stability, completed full races, and broad save scenarios remain open qualification items.

## Build and Verification

The root build now only covers the current Clang/C++20 pipeline, generated by
`native/build.cmd` into `private/slice114-native-base/`. Runtime 113 and nine original SPIR-V
shaders remain preserved. The private AOT library is a prerequisite,
validated by hash; the playable build does not yet consume the separate P0 producer.
Clean build with zero warnings; 3/3 CTest passing in 11.13s, 32 Python tests passing without skips (final rerun in 5.812s).
A 120-frame headless smoke test exits cleanly with memory card duplication,
without navigating or launching a new race. All 9 SPIR-V shaders match slice 113; frozen pins preserved.
The original 54 CTest / 117 Python tests are historical: tests for the removed engine reside
on the archive branch; the active test suite covers the surviving codebase.

## Native Port Direction

Historical scene-renderer direction below is deferred on the slice 141 hybrid
branch. Use [Hybrid Port Plan](HYBRID-PORT-PLAN.md) for the active next task.

The owner requested a comprehensive native-renderer/native-port and multi-model plan:
[Native Port Plan](NATIVE-PORT-PLAN.md), [Agent Workflow](AGENT-WORKFLOW.md).
Initial target selected by owner: Windows, entry-level dedicated GPU, internal
1920x1080 at 60 FPS. Exact hardware/preset and achieved throughput remain Unknown.
Owner clarified that deferred rendering and a graphical remaster are outside the
current roadmap. Native graphics, progressive output, configurable internal
resolution, ultrawide and independent simulation/presentation timing remain goals.

The owner authorized reproducible EE generation and bounded model/camera research
in slice 116; slice 117 qualifies one callback recovery mechanism offline. The
planned following stage is one live showroom car drawn through a game-level
Vulkan renderer. Expand to racing, progressive/internal resolution and temporal
separation; retire all graphics paths, then remaining execution/device dependencies
through separate qualified service/core migrations. AOT alone does not mean zero
emulation. No Windows JIT is active today; the proposed port does not introduce one.

The new plan supersedes the phase order of [ROADMAP.md](ROADMAP.md), whose timing
analysis remains useful. Eliminating IOP/VU models is not a prerequisite for
FPS independence; 144 distinct scenes require qualified visual state/hooks.
Plans include economical worker routing, independent review, serial integration
and owner-operated gameplay gates. No workers, provider setup or recurring jobs
were dispatched outside the read-only planning review.

This revision is documentation only: no renderer/code generation implementation,
profiling, build, game run or new qualification result. Existing OpenCode changes
and the owner's session/card are preserved. Input, audio, save and stability work
continues to require its own authorized package and acceptance controls.

Technical reference: [Architecture](ARCHITECTURE.md), [Build](BUILD.md),
[Slice 114 Report](reverse-engineering/slice114-native-base-cleanup.md).
Prior functional evidence: reports 110–113, corresponding manifests, and
`inputs/slice113-owner-results.json`. Slice 114 does not reclassify these results
as complete fidelity proofs or official benchmarks.

README updated for public presentation: project vision, confirmed features,
experimental boundaries, and future goals. Editorial change only; does not alter
runtime, requirements, priorities, or validation results.

## OpenCode Performance Handoff

The owner requested an investigation prompt, not debugging by Codex.
[Task prompt](OPENCODE-PERFORMANCE-HANDOFF.md) provides verified paths, build and
launcher constraints, hypotheses to test, fresh producer/manifest requirements,
manual feedback iterations and save protection. Initial source baseline is
cbccf80; executing agent must recheck HEAD. No profiling, build or game run occurred
while preparing this handoff. Maintain documentation and new comments in en-US.

**Round 2 (same day):** launcher `49d25309...` and owner installer `private\slice243-installer\GT4Recomp-Setup-1.2.0-test.exe`
(28,771,288 bytes, sha256 `92666e14386d92fa5098f69a0aab4ae452c1e8978e3329abfd606ed606f0e585`). Adds: monitor names
without resolution, Resolution default "desktop" (follows the desktop at each start), every gt4-gs value enabled, the
renderer choice in setup and the GSdx switch in Settings, the GSdx naming, and PR #72 (TASK-089) merged.

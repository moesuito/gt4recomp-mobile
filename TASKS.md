# GT4Recomp Tasks

Single ordered task list. Each task has its own branch, automated checks, an
in-game test by the owner and an explicit owner approval before its PR is merged
into `main` (workflow rules in [AGENTS.md](AGENTS.md)). Evidence for finished
tasks lives in `docs/journal/` and `docs/inputs/`; background studies are linked
from [STATUS](docs/STATUS.md).

States: ✅ Done · 🧪 Waiting for owner test · 🔄 In progress · ⏭️ Next · 📋 Planned

Tasks are numbered in execution order (renumbered on 2026-10-15). Older
journals and commits use the previous numbers: 018→010, 010→011, 031→012,
014→013, 015→014, 016→015, 017→016, 020→017, 022→018, 023→019, 026→020,
013→021, 024→022, 027→023, 025→024, 028→025. Second renumbering (same day): 022→017, 021→018,
020→019, 019→020, phase 2/3 tasks 022-025→021-024; the in-game 16:9 task was
dropped (the owner's game already boots in 16:9). Earlier IDs 011, 012, 019, 029
were merged into the new 010/012; the pre-donor cleanup was done outside the
list (private repository `moesuito/gt4-staticrecomp-legacy`). Third renumbering
(2026-10-15, owner): 018→017 (license crash), 019→018 (wheels), 020→019
(launcher), 017→020 (decomp names, last in phase 1, after the first stable release).

## Phases

| Phase | Platform | Goal |
|---|---|---|
| **Phase 1** (current) | Windows PC | Original game running natively: correct speed, smooth 60 FPS, no interpreter on the normal path, PCSX2 GS renderer in a separate process (decision 0070), CoreGS 1.1 fallback |
| **Phase 2** | Android | ARM64 build with the ARMSX2 mobile GS pipeline (Vulkan) |
| **Phase 3** | iOS | ARM64 build with the ARMSX2 Metal GS pipeline |

**Renderer decision (2026-10-15, [decision 0070](docs/decisions/0070-pcsx2-gs-separate-process.md)):**
PC uses the **PCSX2 GS** in a separate GPL process, `gt4-gs.exe`, built from our fork
`moesuito/gt4recomp-gs` and downloaded by the launcher (TASK-032..044). **CoreGS 1.1** (our MIT
Vulkan compute renderer) is built in as the fallback and offline default. **CoreGS 2.0** (our MIT
hardware renderer, clean room from the TASK-026 specification; TASK-027) is planned and paused. paraLLEl-GS
is dropped from the release (0070 supersedes the renderer options of 0069). The game executable never
contains GPL code. Mobile uses the **ARMSX2 GS pipeline** (Vulkan on Android, Metal on iOS) without its JIT.
See [GS renderer options](docs/GS-RENDERER-OPTIONS.md) and [GSDX-PLAN](docs/GSDX-PLAN.md).

## Phase 1 — Windows PC

| Task | Goal | State |
|---|---|---|
| TASK‑001 | Rebuild the slice 149 baseline on this machine | ✅ Done |
| TASK‑002 | Owner-launched VU1 program recording mode | ✅ Done |
| TASK‑003 | Hardened VU1 AOT emitter with differential tests | ✅ Done |
| TASK‑004 | VU1 microcode compiled ahead of time in the game | ✅ Done |
| TASK‑005 | Asynchronous GPU submission + overlay (RTSS) fix | ✅ Done |
| TASK‑006 | Frame pacing on a single clock, rate-controlled audio | ✅ Done |
| TASK‑007 | VU1 per-program flag elision | ✅ Done |
| TASK‑008 | Full static VU1 entry coverage (all 2,048 entries) | ✅ Done |
| TASK‑009 | Quick fixes: title, fixed 16:9, fullscreen, interval counters, races in 480p | ✅ Done |
| TASK‑010 | Build pipeline: faster builds and build tuning (parallel, cache, lld-link, fp-contract off, x86-64-v3 variant, LTO, PGO) | ✅ Done (tuning measurements pending) |
| TASK‑011 | MTVU: VU1 on its own thread, on by default | ✅ Done · in `--mode main` (slice178), `GT4_MTVU=0` A/B |
| TASK‑012 | paraLLEl-GS as an optional PC renderer | ❌ Dropped (decision 0070): LGPL code not shipped. Branch `task/012-parallel-gs` kept as history; MIT seam and capture code salvaged into TASK-035 |
| TASK‑013 | VU0 microcode compiled ahead of time | ✅ Done |
| TASK‑014 | EE entry coverage from the decomp function list | ✅ Done (2026-10-21) · 1,105 new entries (+3,728 dispatch ids), slice208 `pcsx2-coverage`; headless 42/42 identical; owner play-through approved |
| TASK‑015 | LWR sign-extension fidelity check | ✅ Done (already correct; no change) |
| TASK‑016 | IOP per-module cost counters and review | ⏸️ Deferred (not needed for 1.0.0) |
| TASK‑017 | License-test crash | ✅ Done (2026-10-16) · license demo guard on both demo loaders (slice206, `--mode pcsx2`): licenses playable, demos that decrypt wrong are skipped as in PCSX2 default; root cause moved to TASK‑063 |
| TASK‑018 | Native Driving Force Pro (DFP): 900-degree steering, game-generated force feedback bridged to SDL3, wheel configuration | 📋 Planned for 1.3.1; TASK-123 research brief ready; TASK-121 host-input groundwork is partial |
| TASK‑019 | Launcher 1.0.0 (Rust core + Tauri 2 HTML frontend) and NSIS per-user installer | ✅ Launcher merged into main (2026-10-21): setup with disc copy, gt4-gs download, settings, saves, controls (SDL3), tray, updates; owner tested end to end. Still for 1.0.0: NSIS installer build and the v1.0.0 release |
| TASK‑020 | Decomp names in generated EE code (waits for more decompilation progress) | 📋 Planned |
| TASK‑025 | Save manager: memory card slots, import/export | ✅ Code done, merged into `task/019-launcher`; ships with the launcher (owner test pending) |
| TASK‑026 | CoreGS 2.0, part 1: clean-room specification of the PCSX2 hardware renderer | ✅ Done (docs/hw-renderer-spec, PR #17) |
| TASK‑027 | CoreGS 2.0, part 2: MIT implementation from the specification (3x-6x, texture packs) | 🧪 Candidate 332 on `task/027-coregs2` (2026-10-30): CPU survey and fronts (process CPU -28%, wall -15% at 3x against 313), direct GS intake (P6); specialised pipelines, performance mode started (decision 0073); 1.3.0-rc7 merged in as the base (CoreGS 2.0 is not part of 1.3.0); fine-tuning, then a PR to the EE optimisation branch |
| TASK‑028 | CoreGS 1.1: upscaling, interlaced-only deinterlace, FXAA-class AA, FSR 1.0 | ⏸️ Paused by the owner: 3x shows native-size aliasing on 3D models; stays the built-in fallback renderer (`task/028-donor-upscale`, latest slice186) |
| TASK‑029 | Controller and keyboard mapping (binding table, XInput LT/RT as L2/R2 pressure, controls file for the launcher) | ✅ Done (2026-10-21) · merged with the launcher; owner tested remapping in game |
| TASK‑030 | Night-race readback wait (CoreGS 1.1) | ⏸️ Paused: owner test 59.07 FPS, wait unchanged at 4.3 ms because the page is drawn the same frame (`task/030-night-readback`) |
| TASK‑031 | High frame rate (120-144 FPS) with game physics untouched | ❌ Dropped in this form; redone as TASK-045 on the PCSX2 GS (study docs on `task/031-high-fps`) |
| TASK‑032 | Decision 0070 docs: PCSX2 GS in a separate process, clean-room wording, renderer docs | ✅ Done (PR #24) |
| TASK‑033 | PCSX2 GS fork (`moesuito/gt4recomp-gs`): reproducible build and CI releases | ✅ Done · standalone repo, CI green, prerelease `gt4gs-v0.1.0-poc` (docs/GS-FORK.md) |
| TASK‑034 | Proof of concept: replay our `.gscap` capture through the PCSX2 GS and compare images | ✅ Done · owner approved the images on 2026-10-16; gate passed (docs/GS-PCSX2-POC.md) |
| TASK‑035 | MIT GS stream protocol, shared-memory IPC and renderer seam on main | ✅ Done · slice191: CTest 29/29, Python 321 OK, donor 42/42, stream converts to the PoC dump byte for byte (docs/GS-STREAM-PROTOCOL.md) |
| TASK‑036 | Live `gt4-gs.exe` MVP: the game drives the PCSX2 GS over IPC | ✅ Done · owner test 2026-10-16 at 3x (sharp, fullscreen OK): Nordschleife 59.98 FPS, night race with 5 AI 59.89 FPS, readback wait 0.93 ms; `--mode pcsx2` holds 480p by default |
| TASK‑037 | PCSX2 GS readbacks and the night race | ✅ Done · owner test 2026-10-16: night race 59.97 FPS, 69 late frames (was 213); accurate readbacks are the default, unsynchronized optional (drops headlight flares) |
| TASK‑038 | GS window, fullscreen, focus, input and crash handling | ✅ Done · owner test 2026-10-16: fullscreen, Alt+Tab and the click-in-picture focus fix (gt4-gs 0.3.1); crash grace frames and CoreGS 1.1 restart offer |
| TASK‑039 | GS settings table and `gt4-gs.ini` | ✅ Done (main-repo part): 51-row table drives `gt4-gs.ini`; allow-list enforced by gt4-gs 1.0.0 (slice201: ignored with a warning) |
| TASK‑040 | Texture dumping and replacement packs | ➡️ Folded into TASK-050 (mod manager, release 1.1.0) |
| TASK‑041 | GS process performance: F8 `[gs-ipc]`, tuning, owner A/B | ✅ Done · owner test 2026-10-16: night race 59.97 FPS, 53 late frames (was 69), EE critical path 11.8 of 16.7 ms (headroom 4.9 ms); GS wake batching and ordered privileged writes |
| TASK‑042 | JIT-free `gt4-gs.exe` (no recompilers, no RWX, no SW JIT); required before 1.0.0 | ✅ Done · owner test 2026-10-16 (menus, night race): gt4-gs 0.4.0 JIT-free, enforced by Arbitrary Code Guard; images identical to 0.3.2 |
| TASK‑043 | Launcher: download, pin and update the GS renderer | ✅ Done (2026-10-21) · the launcher installs gt4-gs 1.0.1 from the GitHub release at setup; owner tested |
| TASK‑075 | Input layer on SDL3 (game side in slice210: SDL3 3.4.18 gamepads mapped to the DS2 layout, controls.toml applied, GT4_INPUT=sdl default / xinput for one release) | ✅ Done (2026-10-21) · game (slice210) and launcher read pads through SDL3 3.4.18; owner tested and approved |
| TASK‑076 | Modern fixed keyboard layout (several keys per DS2 button, table in docs/CONTROLS.md `keyboard-default`) | ✅ Done (2026-10-21) · fixed keyboard layout in game and launcher, P = Start (owner choice); owner tested |
| TASK‑044 | Packaging, release notes and the release 1.0.0 plan update | ✅ Done (2026-10-17) · release 1.0.0 ships gt4-gs 1.0.1; fork release notes, README, THIRD_PARTY and RELEASE-1.0.0-PLAN updated (docs only) |
| TASK‑045 | High frame rate on the PCSX2 GS (TASK-031 redone) | 📋 Planned (after 1.0.0) |
| TASK‑046 | Launcher "Prepare shaders": compile GT4's PCSX2 GS pipeline list headless (gt4-gs --warm-selectors) and keep the cache warm | 📋 Planned (study done, 2026-10-16) |
| TASK‑047 | PCSX2 GS deinterlacing in the 480i menus (field and interlace state in the stream) | ✅ Done · owner test 2026-10-16: 480i menus clean on Automatic; root cause missing SYNCV and a one-field-early VSYNC field (protocol 1.1, gt4-gs 0.3.2) |
| TASK‑048 | Mod manager in the launcher (umbrella): texture packs first, Content Manager style | 📋 Planned for 1.1.0 (umbrella; owner decisions 2026-10-16) |
| TASK‑049 | GS settings curation for the launcher: Graphics and Advanced pages, reset to defaults, 12x, widescreen finding | ✅ Done · decisions 2026-10-16; default gt4-gs.ini unchanged; 58-row table ([settings](docs/GS-SETTINGS.md)) |
| TASK‑050 | Texture replacement contract and headless dump/replace round trip (absorbs TASK-040) | 📋 Planned for 1.1.0 |
| TASK‑051 | `gt4mods` core: manifest, pack detection, validation, import, store | 📋 Planned for 1.1.0 |
| TASK‑052 | Mods active view (hardlinks), priority, conflicts, uninstall, `gt4mods` CLI | 📋 Planned for 1.1.0 |
| TASK‑053 | GS settings and dev launcher wiring for mods (texture rows off Graphics, `[Folders] Textures`) | 📋 Planned for 1.1.0 |
| TASK‑054 | Launcher Mods page (mockup, then Tauri) | 📋 Planned for 1.1.0 |
| TASK‑055 | Mod creator tools: dump workspace, package as mod | 📋 Planned for 1.1.0 |
| TASK‑056 | Live texture reload in gt4-gs | 📋 Planned for 1.1.0 |
| TASK‑057 | Study: other mod kinds (GS presets, VOL overlay, data patches) | 📋 Planned for 1.1.0 |
| TASK‑058 | Crash-safe memory card in the real build: atomic writer, dirty retry, exit flush (GS-loss path too) | ✅ Done · owner test 2026-10-16; kill test 25/25; card flush on exit and after a GS loss (slice200; in slice202) |
| TASK‑059 | Distribution executable: installed-folder shaders and GS package, no repo paths, `--portable-check`, `scripts/make_distribution.py` | ✅ Done · owner test 2026-10-16; distribution folder smoke 42/42 and 41/41 (slice200; slice202: also from `private/slice202-dist`) |
| TASK‑060 | Mouse cursor auto-hide over the picture (fullscreen and windowed, game and gt4-gs agree) | ✅ Done · owner test 2026-10-16 (fullscreen and windowed); slice201, in slice202 |
| TASK‑061 | "Compiling shaders…" notice in gt4-gs (real compiles only) | ✅ Done · owner test 2026-10-16; gt4-gs 1.0.0 (cold cache 238 compiles shown, warm cache none) |
| TASK‑062 | Performance overlay (FPS, frame time, CPU/GPU of the game, resolutions, frame cost and headroom) | ✅ Done · owner test 2026-10-16; overlay off by default, +0.35 us/frame EE, +0.02 ms/frame GS thread |
| TASK‑063 | Bit-exact R5900 FPU `div.s`/`mul.s` (license demo phase, real-PS2 card replays and ghosts) | 📋 Planned · oracle ready (`scripts/license_demo_oracle.py`); needs hardware vectors |
| TASK‑064 | Third-party overlays (MSI Afterburner / RivaTuner) with the PCSX2 GS: Arbitrary Code Guard off by default | ✅ Done (2026-10-17) · owner test: the RivaTuner/Afterburner overlay shows in game with default settings |
| TASK‑077 | Context-aware input mapping (race vs menus, e.g. Esc = Start in a race, Triangle in menus) | 📋 Open, study done (docs/CONTEXT-INPUT-STUDY.md); not in 1.0.0 |
| TASK‑078 | Graphics settings inside GT4's own Options menu (output resolution 480p to 8K replacing the 480i/480p/1080i row, fullscreen, anti-aliasing); game fixed at 480p | 📋 Planned (after 1.0.0), study done (docs/INGAME-SETTINGS-STUDY.md) |
| TASK‑079 | Force 16:9 at boot: `GT4_FORCE_WIDESCREEN` holds the game's own Wide flag at 16:9 (slice211); launcher setting "Widescreen 16:9", default on | ✅ Done (2026-10-21) · slice211, GT4_FORCE_WIDESCREEN default on, launcher "Widescreen 16:9"; owner tested (boots 16:9 and 480p; the 100% save loads). The "Load failed!" seen on the way was the launcher card bug, fixed in TASK-019 |
| TASK‑080 | Native ultrawide and other aspect ratios (16:10, phones, 21:9, 32:9): screen-ratio detection, automatic Width/Aspect for the 3D, HUD/UI fix, renderer stretch | ✅ Done 2026-10-21 (P0, P1, P2 and the seamless-display addendum, owner approved; release 1.1.0) · P3 progressive menus open with TASK-078 |
| TASK‑081 | Release 1.0.0 hardening and installer: release game build without build paths (slice213), launcher without user paths, CPU (SSE4.1) and Vulkan checks before setup and Play, game Vulkan baseline path for older drivers (slice214), WebView2 bootstrapper in the installer, installer asset name `GT4Recomp-Setup-X.Y.Z.exe`, release notes and draft release | ✅ Done (2026-10-21) · owner tested and approved the installer; released as v1.0.0 |
| TASK‑082 | Native variable frame rate: fixed 60 Hz simulation, render at any rate (render skip below 60, extra render-only passes above 60 with the game's own render time t) | Deferred by owner (2026-10-26); preserved at task/082-render-skip15b7bf3 for CoreGS2.0; excluded from rc7 (TASK-133) |
| TASK‑083 | Faster loading and saves: instant disc reads (deeper than PCSX2 Fast CDVD) and loading/save speed-ups in our runtime and game code; gameplay untouched | 📋 Future study (owner, 2026-10-21) |
| TASK‑084 | Frame cost optimisation: cut the emulation-side EE overhead (VIF1 chain jobs, GIF fast path, VBlank journal) | ✅ Done (2026-10-22) · owner test approved, night race critical path 11.8 → 10.0 ms; release 1.2.2 (PR #82); flare readback moved to CoreGS 2.0 (TASK-087) |
| TASK‑085 | Launcher self-update: the setup starts outside the launcher's kill-on-close job | ✅ Done (2026-10-21) · PR #67; v1.1.0 republished with the fix; launchers 1.0.0 update by running the setup |
| TASK‑086 | Progressive 480p everywhere (menus and races at 480p through ELF patches) | ⏸️ Shelved (owner, 2026-10-21) · owner test broke the car select and the race load; branch `task/086-progressive-480p` kept |
| TASK‑087 | GS readbacks without stalls, in CoreGS 2.0 (GPU-driven flare visibility, no EE wait) | 📋 Planned (owner, 2026-10-21) · study done (docs/GS-READBACK-STUDY.md); CoreGS 2.0 only, gt4-gs frozen |
| TASK‑088 | Release 1.2.0: custom textures (gt4-gs texture replacements from `Documents\Gran Turismo 4\Textures`, two launcher options), 2-row bottom crop, black window from the first frame, monitor names, desktop resolution, every gt4-gs value enabled, GSdx setup choice and switch | ✅ Done (2026-10-22) · owner tested and approved; released as GT4Recomp v1.2.0 with gt4-gs 1.2.0 (fork PR #9, tag `gt4gs-v1.2.0`) |
| TASK‑089 | Respect CoreGS 1.1 selection during setup and automatic renderer maintenance (contribution by JulioHeroZ, PR #72) | ✅ Done in release 1.2.0 (merged into `task/088-custom-textures-120` with a merge commit; PR #72 closes when 1.2.0 lands on main); owner tested in 1.2.0; superseded by TASK‑096 in 1.2.3 |
| TASK‑090 | CoreGS 3.0: modern render pipeline (VU1 work on the GPU, native effects restored one by one), opt-in; faithful renderer stays the default | 📋 Planned (owner, 2026-10-22) · after CoreGS 2.0; VU1 study in progress |
| TASK‑091 | Release 1.2.1: per-machine install with data beside the program, user data in %APPDATA%, portable folder, silent and safe self-update, WebView2 checks (issue #80), Performance Mode, home screen layout | ✅ Done (2026-10-22) · owner approved; released as v1.2.1 |
| TASK‑095 | Ultrawide: undistorted 2D overlay on the pre-race menu (race lobby), as TASK-080 P2 does for the race HUD | 📋 Planned (owner, 2026-10-22) · study first; idea only |
| TASK‑097 | Self-sufficient game executable: defaults built in (GSdx, 480p, 16:9 and ultrawide, fullscreen), paths found from the exe folder, settings read from the launcher's config file; environment variables only as test overrides | 🟡 Done on branch `task/101-multi-game` (2026-10-23), waiting for the owner test · release 1.3.0 (moved from 1.2.4) |
| TASK‑098 | Portable build: a second launcher build shipped as a zip, user data in `user\` and large data in `data\` beside the exe, no registry or system folders, self-update by zip swap | 🟡 Done on branch `task/101-multi-game` (2026-10-23), waiting for the owner test · release 1.3.0 (moved from 1.2.4) |
| TASK‑099 | Launcher home: patch notes panel on the right (glassmorphism card), Markdown release notes of the installed version embedded in the build | 🟡 Done on branch `task/101-multi-game` (2026-10-23), waiting for the owner visual check · release 1.3.0 (moved from 1.2.4) |
| TASK‑100 | Spec II support: SCUS-97436 as a second target (decision 0072); subtasks S1-S9 in docs/SPEC2-PLAN.md | 🟡 Owner test passed (2026-10-23): Spec II menus, Options and license tests; license demo and installer test pending · release 1.3.0 |
| TASK‑101 | Multi-game launcher: Gran Turismo 4 and Spec II side by side (game selector, Play/Install, per-game disc, card and saves, shared settings, Spec II logo) | 🟡 Implemented on `task/101-multi-game` (2026-10-23), owner test pending · release 1.3.0 with TASK-100 |
| TASK‑102 | Launcher language: automatic en-US / pt-BR with a General > Language setting (locale files, Rust messages, release notes) | 🔧 Implemented on `task/101-multi-game` (2026-10-23), owner test pending · release 1.3.0 |
| TASK‑103 | Graphics API choice for GSdx: Vulkan (recommended, default), Direct3D 12 (recommended), Direct3D 11 (older PCs), OpenGL later; hardware backends only, in Settings > Graphics (locale files, same ids kept apart from the engine Renderer) | 🔧 Implemented on `task/101-multi-game` (2026-10-23), owner in-game test pending · release 1.3.0 |
| TASK‑104 | Launcher saves: crash-proof PCSX2 card import (issue #86: superblock layout for 8 to 64 MB, bounded directory walk, import under a panic guard), New save, Export of the active save, Rename (display name, 40 characters) | 🔧 Implemented on `task/101-multi-game` (2026-10-23), owner test pending · release 1.3.0 |
| TASK‑105 | Integrated GPUs: 1x default and a plain GPU-lost message (issue #89) | 🔧 Implemented on `task/101-multi-game` (2026-10-23), owner test pending · release 1.3.0 |
| TASK‑106 | CHD disc images: accept `.chd` at setup and decompress to the launcher's own ISO image (issue #90) | 📋 Planned (owner, 2026-10-23) · release 1.3.1 |
| TASK‑107 | Automatic graphics setup per GPU: automatic Graphics API, GPU generation table, step down after a lost GPU, game without Vulkan when GSdx draws, OpenGL in gt4-gs | 📋 Planned (owner, 2026-10-23) · release 1.3.1 |
| TASK‑108 | Launcher: automatic Graphics API (Vulkan, else Direct3D 11) and graphics card (Adapter), no Vulkan requirement; game programs shipped as gt4.exe and gt4-spec2.exe | 🔧 Implemented on `task/101-multi-game` (2026-10-23), owner test pending · release 1.3.0 |
| TASK‑109 | Launcher update channels: Stable (default) or Test builds (rc pre-releases) | 📋 Planned (owner, 2026-10-23) · release 1.3.1 |
| TASK‑110 | Controller tuning: stick and trigger deadzone, sensitivity, response curve, saturation, pressure, inversion, vibration, live test view | 📋 Planned (owner, 2026-10-23) · release 1.3.1 |
| TASK‑111 | Logs for bug reports: one-click "Export logs" zip (latest log, recent launcher and game logs, settings, system summary; user name and paths masked), offered after a crash or a lost card | 🔧 Implemented on `task/101-multi-game` (rc4, 2026-10-23), owner test pending · release 1.3.0 |
| TASK‑112 | Launcher logs: one file per session with INFO, WARN and ERROR lines, the last 20 kept, `latest.log` with the earlier errors, game session folders capped at 20, Open latest log | 🔧 Implemented on `task/101-multi-game` (rc4, 2026-10-23), owner test pending · release 1.3.0 |
| TASK‑113 | Spec II race HUD at ultrawide ratios (wider than 16:9): speed figure and odometer digits stretched and spilling past the gauge (known issue in 1.3.0) | 📋 Planned (owner, 2026-10-23) · next update |
| TASK‑115 | Game recognition and Discord: Windows Game Mode/Game Bar, AMD and NVIDIA capture (Instant Replay, ShadowPlay) and Discord Rich Presence | 📋 Planned (owner, 2026-10-24) · future (1.3.1 or later). Discord Rich Presence portion implemented on branch task/133-rc7-ra-controls (2026-10-27); Game Bar/GPU recognition remains future. |
| TASK‑116 | George V Paris below 60 FPS: one GS readback per frame makes the game wait for VU1 and GSdx; 1.3.1 = no VU1 drain when the readback returns nothing, plus unsynchronized readbacks in gt4-gs 1.2.2 | 🔧 rc6 step 1 implemented and tested headless (2026-10-25); owner test pending; step 2 stays 1.3.1 |
| TASK‑117 | Performance mode, part 2: fewer light flares at night (no halo, distance cull, light caps), our own clean implementation in GT4 v2.00, off when Performance mode is off | ⚠ rc6 ELF, library and host block built and tested (2026-10-25), NOT shipped: the switch-off identity gate failed (journal 2026-10-25) |
| TASK‑118 | Graphics settings simplification: Performance Mode becomes a real preset (blending Basic, trilinear/dithering off, preloading partial), the Advanced page loses the cosmetic rows, RC-only settings move to an RC Settings area, GS async and deinterlace become fixed | 🔧 Planned (2026-10-25, owner), 1.3.0-rc6 |
| TASK‑093 | Custom textures straight in the Textures folder: gt4-gs 1.2.1 reads the folder itself; the launcher stops creating `replacements` and moves an old `replacements` folder's contents up once gt4-gs 1.2.1 is in use | ✅ Done (2026-10-22) · release 1.2.3 (owner tested) |
| TASK‑096 | Remove CoreGS 1.1 from the launcher (owner, 2026-10-22): GSdx only, CoreGS 2.0 shown as "Coming soon" (disabled), a stored CoreGS 1.1 choice migrates to GSdx, a GSdx failure shows a clear error instead of falling back; CoreGS 1.1 stays in the game executable, unused by the launcher. For launcher release 1.2.3 | ✅ Done (2026-10-22) · release 1.2.3 (owner tested) |
| TASK‑092 | MTVU test switches after TASK-084: `GT4_MTVU=0` and `GT4_MTVU_VERIFY=1` in pcsx2 mode, portable doorbell ordering | ✅ Done (2026-10-22) · release 1.2.3 (owner tested) |
| TASK‑094 | Desktop shortcut: finish-page checkbox (on by default), all-users desktop `Gran Turismo 4.lnk`, choice kept in HKLM `DesktopShortcut`, silent updates create it for older installs and restore it when chosen | ✅ Done (2026-10-22) · release 1.2.3 (owner tested) |
| TASK‑134 | Game-code optimization study: CPU time and energy outside the GS renderer, ordered plan for the i5-7200U laptop (docs/GAME-CODE-OPTIMIZATION-STUDY.md) | 🧪 rc9 (slice390) installed on the desktop and notebook for the owner's test; not published (2026-11-02): process CPU -18% on the i5-7200U, -19% on the 5700X, physics identical to rc8 |
| TASK‑135 | Performance HUD for CoreGS 2.0, in the spirit of the macOS Metal HUD: FPS, frame time, CPU and GPU of the game, memory and a live frame time graph | 🧪 Built, waits for the owner's test (branch `task/135-coregs2-performance-hud`, candidate 378, on the notebook, 2026-11-02) |
| TASK‑136 | Simple onboarding: a new install uses CoreGS 2.0 when the PC runs it, GSdx becomes optional and is downloaded only when the card needs it | 🧪 Built, waits for the owner's test (branch `task/136-simple-onboarding`, 2026-11-02) |
| TASK‑137 | The launcher links the disc image and no longer copies it; an image that moved asks to be chosen again; installs with a copy keep working | 🧪 Built, waits for the owner's test (branch `task/137-link-disc-image`, candidate 381, 2026-11-02) |
| TASK‑138 | A portable folder carries its disc image (relative link); accented paths on CoreGS 2.0; the GSdx fallback goes to CoreGS 2.0; a release can be started directly | 🧪 Built, waits for the owner's test (branch `task/138-relative-image-link`, candidate 382, 2026-11-02); GSdx in an accented folder not confirmed |
| 🧩 | **gt4-gs 1.0.0, the first stable renderer release** (with TASK‑060/061/062; settings allow-list enforced) | ✅ Published 2026-10-16 (`gt4gs-v1.0.0`, zip sha256 `2e878019...`) |
| 🚀 | **First stable release (1.0.0) on GitHub** (after TASK‑044, TASK‑043 and the owner's end-to-end test) | 📋 Planned |

## Phase 2 — Android

| Task | Goal | State |
|---|---|---|
| TASK‑021 | Mobile GS: ARMSX2 GS pipeline (Vulkan) behind our GS front end | 📋 Planned |
| TASK‑022 | Android app: ARM64 build, host layers, touch controls, Bluetooth controllers | 📋 Planned |

## Phase 3 — iOS

| Task | Goal | State |
|---|---|---|
| TASK‑023 | ARMSX2 Metal GS backend | 📋 Planned |
| TASK‑024 | iOS app: ARM64 build, signing, touch and controller input | 📋 Planned |

## Done

### TASK-001 — Rebuild the slice 149 baseline on this machine
Rebuilt the measured diagnostic candidate on the Ryzen 7 5700X / RX 9060 XT
machine; fixed a fixture that assumed every GPU limits workgroup counts below
`UINT32_MAX`. Commit `8b65693`.

### TASK-002 — VU1 program recording mode
`--mode vu1-record` captured 86 VU1 programs in two owner sessions. Commit `8702d90`.

### TASK-003 — Hardened VU1 AOT emitter
`tools/vu_recomp`: exact inputs, fresh outputs, identity digests, budget/resume
contract, differential tests. Commit `96c71f4`.

### TASK-004 — VU1 AOT in the game (slice 151)
Race frame 33.3 ms → 22.0 ms; 140,034 in-game verifications, 0 mismatches.
Commit `2d21abb`.

### TASK-005 — Asynchronous GPU submission (slice 152)
GS wait 9.5 ms → 0.8 ms; race at 100% speed; RTSS overlay visible on AMD.
Commit `56819b0`.

### TASK-006 — Frame pacing (slice 153)
Flat 16.7 ms frametime (99.6% of intervals in 16-17 ms). Commit `fe51478`.

### TASK-007 — VU1 flag elision (slice 154)
Showroom 44.8 → 59.1 FPS; 5-AI race at 60.0. Commit `26e211d`, merged in PR #1.

### TASK-008 — Full static VU1 entry coverage (slice 157)
All 2,048 entries of the decrypted VU1 image compiled. Owner test on an idle
CPU: night race 58.9 FPS (remaining cost is the 4.1 ms GS fence wait, for
TASK-012), rally 59.9, showroom 60.0; 0 interpreted VU1 calls in all three.

### TASK-009 — Quick fixes (slice 164)
Title "GT4Recomp", fixed 16:9, Alt+Enter/F11 borderless fullscreen, interval
counters, `late_frames` fixed. `--force-480p` holds the game's race display
option (index at `[0x622F4C]+0x38CB0+0x16B0`) at 480p: owner confirmed races
render 480p while menus stay 480i as in the original.

### TASK-010 — Build pipeline (slice 159)
Full build 3,942 → about 1,700 CPU-seconds: shared corpus library, split
generated tables, `-Wno-unused-label` on generated code. `GT4_FP_CONTRACT_OFF`
and `GT4_X86_64_V3` options exist (default off); their speed effect is Unknown
until an idle-CPU bench. ThinLTO/PGO not attempted.

### TASK-013 — VU0 microcode AOT (slice 160; main build slice 165)
All 14 VCALLMS entries compiled with exact MAC/status/clip (the EE reads them via
CFC2) and exact `run(0,pc)` hand-offs. 28,000-case differential vs the donor,
0 failures; headless frames identical. Owner test: 0 interpreted VU0 calls;
showroom 58.0, night 57.9, rally 60.0 VBlanks/s (performance neutral, VU0 cost
0.1-0.3 ms/frame). `--mode main` (slice 165) is the build of main with all of it.

### TASK-032 — Decision 0070 docs
Docs only, branch `docs/decision-0070-gs-process`: decision 0070 (PCSX2 GS in a separate process, owner answers recorded), `docs/GSDX-PLAN.md` with the go/no-go gate, TASK-032..045 in this file, CLAUDE.md clean-room wording, RELEASE-1.0.0-PLAN workstream W8, README renderer text, GS-RENDERER-OPTIONS note, journal 2026-10-15. No code, build or game launch.

## Phase 1 details

### TASK-011 — MTVU
Run VU1 programs on a worker thread (donor `start_vu_thread` exists), on by
default with an env switch to disable; exact sync points wherever the EE observes
VU1 results. Acceptance: identical headless frames, verify 0 mismatches, owner
showroom/night/race frametime equal or better.
State: ✅ Done (release 1.0.0 W1, branch `task/011-mtvu`). Plan in docs/MTVU-PLAN.md; step 0 (slice169 measurement), steps 1-4 (ring and journal, slice170 worker, slice171 per-thread F8, slice172 EE-side signals) and step 6 (`--cpu-cores N`) are in. Release W1 (slice178): MTVU is part of `--mode main`, on by default (`GT4_MTVU=0` is the A/B switch), and `--mode main` holds 480p by default. Checks: CTest 27/27, Python 281 OK (12 skipped), headless 4,200 frames: 42/42 identical to slice157 with MTVU on (three runs) and off (one run), and with the default environment. The PR into `main` is not opened yet (ready for review); the merge waits for owner approval. Owner A/B (MTVU-PLAN section 12.6) still pending. Details: BUILD.md "Slice178".

### TASK-012 — paraLLEl-GS as the PC renderer
Dropped (decision 0070): not shipped; LGPL code is not merged. Branch `task/012-parallel-gs` is kept as history; its MIT renderer seam and capture code are salvaged in TASK-035. The text below is the original plan.
One large task, delivered in verified steps on its own branch:
1. Import paraLLEl-GS (github.com/Arntzen-Software/parallel-gs, LGPL-3.0+, with
   Granite MIT) pinned by commit; build it in our CMake; bridge its Vulkan device
   to ours (Granite device shim or port).
2. Adapter from our GS front end (`write`, `write_packed`, `write_image`,
   `read_image`, privileged registers, signals, vsync) to its interface
   (`gif_transfer`, `write_register`, `map_vram_*`, `read_transfer_fifo`,
   `vsync` → scanout). Renderer selected by env (later the launcher); the donor
   renderer stays as experimental fallback.
3. GT4 validation: boot, menus, races, night, rally, showroom; image comparison
   against the donor renderer at 1x; channel-shuffle brightness effects, lens
   flares, half-pixel issues.
4. Options: super-sampling upscaling 2x-16x, deinterlacing/field options for the
   interlaced menus, page-tracked readbacks against the night-race wait.
5. Extras where paraLLEl-GS allows: post-processing (sharpening, FXAA, filters),
   dithering/texture options, texture dump/replacement feasibility.
Replaces the former donor-renderer upscaling, readback-wait and renderer-feature tasks.

### TASK-014 — EE entry coverage
Add translator entries for decomp functions without one, starting with the 15
referenced ones (docs/DECOMP-INTEGRATION.md).

State (2026-10-20, branch task/014-ee-coverage-b, Phase 1 done, not merged): static entry ledger
(scripts/ee_entry_ledger.py, docs/EE-ENTRY-LEDGER.md). Translated entries (case labels): 135,806. New
candidates: 1,122 (high 1,076, medium 39, low 6, rejected 1), 246 unit windows. Decomp names untranslated: 83
of 7,740. The 16 stage A seeds are found by the strict rules (14) and a 40-word window (2). The name table at
decomp e0b70c4d6 equals the committed one. Open: the generated folder has 135,806 labels but its log says
135,093; resolve before Phase 2 (extra-entries library A/B, then producer regeneration after owner approval).
State (2026-10-20, branch task/014-ee-coverage-b, phase 2 candidate built, not merged): 1,105 accepted seeds
(scripts/ee_entries.py). Library profile coverage: 139,534 dispatch ids (+3,728), 325 of 326 units change. Built with
the portable clang-cl 23.1.2; the 8-unit object check is identical to production. CTest 33/33, Python 583 OK,
headless 42/42 identical to slice206. New mode pcsx2-coverage (docs/inputs/slice208-pcsx2.json). Owner play-through
pending; the 135,806 vs 135,093 gap is the codegen boundary entries (EE-ENTRY-LEDGER).
State (2026-10-21): ✅ Done. The owner approved the slice208 candidate after the play-through and asked to close the task (PR into main). `--mode pcsx2` stays slice206 until the default is switched to slice208 in a follow-up (new pin labelled pcsx2, distribution and launcher test folder rebuilt).
Follow-up (2026-10-21, slice209): `--mode pcsx2` runs the slice209 pin (the slice208 coverage library plus the GT4_FULLSCREEN start, for the TASK-019 launcher test). `pcsx2-coverage` and `pcsx2-slice206` stay as history modes.

### TASK-015 — LWR fidelity
State: analysis done, no runtime change (branch task/015-lwr-fidelity). The donor helper matches PCSX2:
offset 0 sign-extends, offsets 1-3 preserve the upper 32 bits. The brief's premise does not hold in code.
The lone-LWR scan found one site (0x00549178, `lwr r5, 0(r2)`). A change would need hardware evidence and
an overlay or ps2game.lib regeneration, so it is parked. Part B (silent audio test) is done.

### TASK-016 — IOP per-module counters
Per-module instruction counters and per-library HLE call counts in F8; decide
whether the sound modules need native HLE (docs/IOP-MAP.md).

### TASK-017 — License-test crash
Pre-existing; traced to `RaceSolitaire::loadDemoInputs` (0x3EAD18) →
`AutomobileControlRecord::Manager::load` (0x346BB8) reading a bad record count.
Env-gated logging there and in the PS2ZIP loader for `replay/license/` files,
plus a CDVD read trace; one owner reproduction; fix the cause.
State (2026-10-15, branch task/017-license-crash): FPU rounding lead checked. Donor mul/div/sqrt equal an exact
chop reference; add/sub truncate the smaller operand (one-ulp, magnitude-larger). Not the crash cause by itself
(Hypothesis). The failing record-size words (0xFF80FF80 / 0x80FF80FF at offset 0x14F8, every file) look like a
decode or layout error in the triage parser, not FP results (Hypothesis, to verify with the owner trace).

State (2026-10-16, branch task/017-license-crash, slice 204): halt forensic dump built, not yet run by the owner. `GT4_HALT_DUMP=1` adds `[halt-dump]` lines at the halt (every thread's registers; for a thread whose saved pc is in the record copy loop of `Manager::load` (0x346BB8..0x346EB0), its record base and count, and whether the record window overlaps any thread stack, with the stack of thread 17 at 0x008722C0 as the suspect; the halted thread's stack top). Overlay `scripts/halt_dump_overlay.inc`; capsule `private/slice204-halt-runtime`; build `private/slice204-halt-build`; pins `docs/inputs/slice204-halt-runtime.json` and `slice204-halt-trace.json`. Owner command: `play_performance.py --assets-root C:\Antigravity\gt4-staticrecomp --mode license-halt`. The FSV read trace is on in the same run. Next: decide from the dump whether the copy overruns (Hypothesis) or the record count is wrong (Unknown).

State (2026-10-16, FPU oracle phase 1): cause Confirmed by the owner test (B1, B2, B5 crash; B3, B4 load, as predicted): the demos are decrypted by GT4's FPU-keyed cipher (func_00579E00), which needs bit-exact R5900 mul.s/div.s. `scripts/license_demo_oracle.py` checks the game CRC on all 592 demos; `tests/python/test_license_demo_oracle.py`. No model passes yet (chop/chop 0/592; every model tried 0/592). A CRC-confirmed, model-free beam search recovered exact chains for 528/592 files (52,469 unique div.s steps): the hardware quotient is always chop or chop + 1 ulp; per-step agreement chop 60.9%, nearest 82.8%, radix-2 carry-save SRT candidate 93.1% (best 94.1%). mul.s is a truncated Booth array (hardware vector 1.0 x 0x3FFFFFFF = 0x3FFFFFFE) that equals chop on this data. Next: identify the divider's digit-selection details (Hypothesis: uncorrected radix-2 SRT), then phase 2 (fpu.hpp overlay, ps2game.lib rebuild, new pins).

State (2026-10-17, done): owner approved guard v1 (slice205: B1 no longer crashes, its background movie fades and the demo does not run, the license is playable, as PCSX2 default; B3's demo plays). B5 then crashed through the second loader 0x00110230 (vtbl[57], target 0x003D5F00; session-20261016-235625-870886), which v1 did not see. Guard v2 (capsule `private/slice206-license-guard-runtime`, build slice206) keys on both loaders' call sites; `--mode pcsx2` = slice206 (`docs/inputs/slice206-pcsx2.json`), `pcsx2-slice203` and `license-guard` (slice205) kept. Consumer map: [LICENSE-DEMO-CIPHER](docs/LICENSE-DEMO-CIPHER.md) (memory-card loads check the CRC themselves). PCSX2 with EE clamping Full plays B1's demo but crashes in B3 (owner): another phase coin flip. Follow-up: TASK-063.

State (2026-10-16, round 3, slice205): overflow clamping models (from the public PCSX2 "Full clamping" workaround prose) refuted: every 0x7F7FFFFF profile gives 0 sane layouts and no model passes a CRC; CRC-confirmed chains use exponent-255 states as large normals, as the donor does. Mitigation per owner decision: license demo guard in the runtime's dispatch.cpp (capsule `private/slice205-license-guard-runtime`, ps2game.lib unchanged): when a decrypted demo fails the game CRC and is not the single-Recorder layout, its record count is set to 0xFFFFFFFF so Manager::load copies nothing; one `[license-guard]` line per demo load. Build slice205 on the slice203 recipe (gt4-gs 1.0.1), mode `license-guard`, pins `docs/inputs/slice205-license-guard*.json`. Known issue: those demos and license backgrounds do not play (as in PCSX2). Waiting for the owner license test (B1-B5).

State (2026-10-16, round 2): oracle reports layout sanity per model. The record payload is past the saturation point, so it is all right or all wrong (one key-stream phase); no FPU model gives a sane layout on all files (best 270/592, US best 50/85, every model near a coin flip), so there is no 1.0 candidate among FPU models. Divider fitting: radix-2 SRT carry-save variants plateau at 94.1% of 52,469 confirmed steps; radix-4 -2..2 ruled out; exact rule still Unknown.

### TASK-063 — Bit-exact R5900 FPU div.s/mul.s
Follow-up of TASK-017. Our `ps2/fpu.hpp` is not bit-exact with the R5900 for the cipher's mul.s/div.s chain: 0 of 592
license demos pass the game CRC, so demos in the wrong key-stream phase play no demo (guarded) and real-PS2 card
replays and ghosts fail to load (cleanly). The oracle (`scripts/license_demo_oracle.py`) and 52,469 CRC-confirmed div.s
steps are ready; the divider's digit selection is unknown (best model 94.1% per step). Needs targeted hardware div.s
vectors (or a published design description). Acceptance: 592/592 CRC passes, then an fpu.hpp overlay, a ps2game.lib
rebuild with new pins, and an F8 physics check.

### TASK-064 — Third-party overlays (MSI Afterburner / RivaTuner) with the PCSX2 GS
Goal: the RivaTuner Statistics Server on-screen display (MSI Afterburner) appears in game with the default settings.
Cause (suspected, not confirmed in game): Windows Arbitrary Code Guard (ACG), which gt4-gs enabled by default
(`[GT4GS] ProhibitDynamicCode`, missing key = true). RTSS injects a hook that needs executable memory, and ACG refuses it.
Change: ACG is OFF by default. Row `prohibit_dynamic_code` (gs-settings.toml) is on the Advanced page, default `false`,
written `always`, so gt4-gs.ini carries `ProhibitDynamicCode = false` under `[GT4GS]`; the Advanced page switch turns
ACG back on. The no-JIT guarantee does not depend on ACG: gt4-gs.exe is built JIT-free (GT4GS_ONLY) and `check-nojit.ps1`
verifies every release. Tests: `ProhibitDynamicCodeTests` and the default-ini assertions in `test_gs_settings.py` and
`test_gs_live.py`. README known issue left unchanged until the owner confirms the overlay.
Acceptance: owner sees the RTSS overlay in game with the default settings.
State: ✅ Done (2026-10-17). Owner test: the MSI Afterburner / RivaTuner overlay shows in game with the default settings, confirming Arbitrary Code Guard as the cause. Branch `task/064-overlay-compat` (single writer, from main da1eb95). Python suite
544 tests OK (38 skipped for missing private files). Merged with owner approval (PR #46).

### TASK-077 — Context-aware input mapping (idea)
Different mappings by game state (race vs menus), e.g. Esc = Start (pause) in a race and Triangle in menus. Needs a
study of how to read the game state reliably. After 1.0.0; the owner has more ideas.
State (2026-10-20): open, out of the 1.0.0 scope (owner). The read-only study is in docs/CONTEXT-INPUT-STUDY.md:
a host-side VBlank read of guest RAM (the race_display pattern) can tell race classes apart by vtable and see the pause
object; replay is still unknown. Proposed first step: a logging-only probe build (S1). Depends on TASK-029 reaching
main and on TASK-075.

### TASK-078 — Graphics settings in the game's own menu
Owner idea 2026-10-20, after 1.0.0. Remove GT4's video-output row (480i / 480p / 1080i, the "HiVision" widget on
Options → Screen), keep the game fixed at 480p, and show our settings in the native Options menu: output resolution
(480p to 8K, the renderer's internal resolution), fullscreen, anti-aliasing and maybe more. Read-only study in
docs/INGAME-SETTINGS-STUDY.md. Recommended path: host-side substitution of the option menu data when the game reads
the disc (patch recipes applied to the user's own disc, never shipped game files) plus a host bridge for the script
natives in `dispatch`; a host overlay menu is the fallback. Phases: P0 hold races at 480p for any saved value; P1 hide
the old row; P2 the "Output resolution" row and a settings file the launcher merges; later fullscreen and AA rows and
live apply in gt4-gs. Estimate: 4-5 slices for P0-P2, 6-8 plus a gt4-gs release in total. Owner decisions pending:
provenance of OpenAdhoc-derived script edits, progressive menus (ELF patch) or not.

### TASK-080 — Native ultrawide and other aspect ratios
Owner request 2026-10-21. The launcher reads the monitor resolution. 4:3 and 16:9 use the game's own modes
(TASK-079 forces 16:9). For other ratios the renderer stretches to the screen and the game's projection values
(hidden Detail Monitor Settings: Width, the display width in mm that acts as FOV, default 425; Aspect, default 0.97
for 16:9) are computed from the ratio (owner data points: 3088x1440 → Width 515, Aspect 1.20; 3440x1440 → Width 573,
Aspect 1.34). The HUD, UI and text are corrected too. The SilentWarrior112 PCSX2 patches (no licence) are a
reference only, mapped to decomp functions and redone in our own parameterised way. Progressive menus overlap
TASK-078. Study first, then a phased plan.
State (2026-10-21): study done, `docs/ULTRAWIDE-NATIVE-STUDY.md` (owner asked to keep it). Findings: the patch file
targets our ELF; the hidden-menu record (Width, Aspect...) lives in the saved option object, so the plan writes no
save data. 3D: a dispatch hook on the race camera call after `RaceBase::initMonitor` (return 0x388B38) widens the
projection by s = R / (16/9) (Hor+, Vert+ below 16:9; at 16:9 frames stay byte-identical). The owner's values
scale both Width and Aspect, which is about 23-37% too wide by the game's maths: first owner test. HUD: a hook on
`RaceDisplay::render` for the canvas (320 -/+ 240 R), plus an ELF patch table and re-translation for the fixed HUD
constants. Menus, FMV and photo stay pillarboxed 16:9. gt4-gs needs "Stretch" and a per-scene control message (a
new gt4-gs release). Plan: P0 a decision superseding the 0055 lines; P1 auto 3D aspect plus a launcher row; P1b
gt4-gs stretch; P2 HUD; P3 progressive menus (TASK-078); P4 optional true-FOV mode. After 1.0.0; seven owner
decisions listed in the study.
State (2026-10-21, P0 + P1 in progress, branch `task/080-aspect-ratios`, slice212b): owner decisions taken
(geometry-correct Hor+, Vert+ below 16:9, no extra FOV by default; menus, FMV and photo pillarboxed 16:9; order P1, P1b,
P2, P3). P0: decision 0071 supersedes the 0055 sentence on game patches, memory writes and camera projection (guard
rails: no save-data writes, 16:9 byte-identical, every hook listed with tests, the SilentWarrior112 file is a reference
only). P1: `GT4_DISPLAY_ASPECT` (auto = the window client area; 16:9, 4:3, W:H, decimal; invalid refuses the start) and
the hidden `GT4_ASPECT_FOV_EXTRA` (default 1.0) for owner test T1; race camera hook at return 0x00388B38 scales the
stack matrix (x by 16/(9R) Hor+, y by 9R/16 Vert+); race view = RaceDisplay::render (0x0039F498) in the last 30 VBlanks
minus photo/movie requests; CoreGS 1.1 fill with linear filter; protocol 1.3 CONTROL display_aspect producer (only to a
consumer with READY minor >= 3); launcher row "Aspect ratio: Auto / 16:9 / 4:3 / Custom" (Auto names the monitor,
Custom ratio slider 1.00-6.00), settings schema 4; TASK-079 hold hardened (writes only after the game-data pointer
settled). Checks: CTest 37/37, Python 612 OK, headless 16:9 and auto 42/42 identical to slice211, race frames differ
from frame 7700 at 21:9/32:9 (hook evidence in docs/BUILD.md slice 212b), gt4-gs 1.0.1 gets no kind 7, gt4-gs 1.1.0
switches to fill at frame 7670. Pins `docs/inputs/slice212b-pcsx2.json` (`--mode pcsx2`; `pcsx2-slice211` history).
Open: owner tests T1 (roundness, auto vs `GT4_ASPECT_FOV_EXTRA`), T2 (3440x1440, phone-like window), T3 (edge pop-in
at 32:9), menus pillarboxed, the 100% save still loads; then owner approval. HUD (P2) is still 16:9-stretched.
State (2026-10-21, after release 1.0.0, branch `task/080-aspect-ratios`, slice215): main db99f9e merged (480p stays
fixed; the Aspect ratio row is the only aspect control; `GT4_FORCE_WIDESCREEN=1` always, the game's own 16:9 is the base;
the GS settings-file aspect stays forced 16:9 and the per-scene fill comes only from protocol 1.3). Settings schema 4
migrates the 1.0.0 schema 3 file (drops the retired keys, adds Auto). The launcher passes `GT4_ASPECT_FOV_EXTRA`
through for T1. slice215 = the slice214 release recipe plus the slice212c capsule and the TASK-080 host code: CTest
39/39, Python 616 OK, cargo 256, headless 16:9/auto 42/42 = slice214, race 21:9/32:9 = slice212b, gt4-gs 1.0.1 no kind 7,
gt4-gs 1.1.0 fill from frame 7670. Pin `docs/inputs/slice215-pcsx2.json` (`--mode pcsx2`). Owner test installer
`private/release-110-test-out/GT4Recomp-Setup-ultrawide-test.exe` (app version 1.0.0; no GitHub release). Planned as
GT4Recomp 1.1.0 (Auto default). Open: the owner tests above, then approval, PR and the 1.1.0 release.
State (2026-10-21, P2 race HUD, branch `task/080-aspect-ratios`, slice217): owner test of P1 on 1920x1080 with Custom
2.44: the race was stretched because gt4-gs 1.1.0 fills the whole window; CoreGS 1.1 already letterboxed. Protocol 1.3
fill now carries the target aspect x 1e6 in `args[1]` (0 = whole window); gt4-gs needs a release that reads it (fork
work). HUD (owner design: 3D full screen, HUD centred 16:9): runtime hook at `RaceDisplay::render` rewrites the HUD
canvas (320 -/+ 240 R; Vert+ below 16:9); the race font x scale (0x0039F554) and the mirror divisor (0x0033EF98) are
immediates behind direct calls, so they go through the ELF patch table (`scripts/aspect_elf_patch.py`) as loads from a
host-filled block at guest 0x7F00; library profile `aspect` (only `unit_008F` and `unit_00A7` change). Split screen is
left alone and stays pillarboxed 16:9. Checks: CTest 39/39, Python 627 OK; headless 16:9 menus 42/42 = slice215, 16:9
race 160/160 = slice212b; tachometer aspect at 21:9, 2.44 and 32:9 equal to 16:9 within 0.5% (P1: +31% at 21:9) and
centred like 16:9. Pins `docs/inputs/slice217-pcsx2.json` (`--mode pcsx2`; `pcsx2-slice215` history),
`slice217-license-guard-runtime.json`, `slice217-ee-libraries.json`; decision 0071 lists every hook and word. New owner
installer `private/release-110-test-out/GT4Recomp-Setup-ultrawide-test.exe` and portable folder
`private/ultrawide-test-portable`. Open: owner T4 (HUD at 21:9/2.44/32:9: dials, lap times, mini-map, mirror,
countdown, results; Custom ratios with CoreGS 1.1 until a gt4-gs release reads `args[1]`), T5 (license test, split
screen if reachable), photo mode; then approval, PR and the 1.1.0 release.
State (2026-10-21, addendum: seamless display, branch `task/080-aspect-ratios`, slice218): owner design: no Aspect ratio
and no Internal resolution option. The aspect is always the chosen Resolution's shape (`GT4_DISPLAY_ASPECT=auto` with
`GT4_OUTPUT_SIZE`; 16:9 unchanged, other ratios turn P1/P2 on); Display has Monitor (connected monitors, default
primary, stable identifier `<GDI device>@<W>x<H>`) and Resolution (the monitor's real modes, default desktop);
fullscreen stays borderless on the chosen monitor; Graphics has Render scale 50-200% (step 25%) shown as "100% · 4x ·
2560x1792". Rule `gt4l_core::display::display_plan`: k = ceil(max(W/640, H/448)), multiplier = k x scale rounded ties
up, clamped 1..12 (gt4-gs takes whole `upscale_multiplier` 1..12). Settings schema 5 (from 3 or 4, backup first). Game
slice218: `GT4_OUTPUT_SIZE`, `GT4_MONITOR`, per-monitor DPI awareness. Checks: CTest 39/39, Python 629 OK, cargo 270,
nav-test 150/150, focus-check 100/100; headless 1920x1080 menus 42/42 and race 160/160 = slice217; 2560x1080 and
3440x1440 hook scaled, HUD widened, fill from VBlank 7670. Pin `docs/inputs/slice218-pcsx2.json` (`--mode pcsx2`;
`pcsx2-slice217` history). New owner installer (sha256 9b039d61...) and portable folder `private/ultrawide-test-portable`.
Open: owner tests on the 1080p remote screen and the 3440x1440 setup; then approval, PR and the 1.1.0 release.
State (2026-10-21, ✅ done, release 1.1.0): the owner tested slice218 on ultrawide and approved ("funcionou
perfeitamente"). Done: P0 (decision 0071), P1 (3D Hor+/Vert+, race view fill, protocol 1.3), P2 (race HUD centred 16:9)
and the seamless-display addendum (Monitor, Resolution, automatic internal resolution, Render scale 50-200%). Release
prep on the branch: the launcher's Settings has no Display tab any more (Graphics holds Display, Graphics and Advanced
sections; `#/settings/display` falls back to Graphics); version 1.1.0 (Cargo workspace, tauri.conf.json, NSIS default,
release notes `docs/release-notes/1.1.0.md`); the launcher's gt4-gs minimum is 1.1.0 and an older install is replaced at
launch (`gs_release::needs_install`, test with an installed 1.0.1); `version.json` is brought in line with the installed
versions at start. The game executable is unchanged (slice218, `docs/inputs/slice218-pcsx2.json`). Draft GitHub
release v1.1.0 with `GT4Recomp-Setup-1.1.0.exe`. Follow-ups: P3 progressive menus (with TASK-078) stays open; split
screen and photo mode in ultrawide are unverified; P4 (optional true-FOV mode) not started.

### TASK-081 — Release 1.0.0 hardening and installer
Release audit 2026-10-21. Branch `task/081-release-1-0-0` (from main 26f83b8). Not PCSX2-tainted.
- Game: CMake `GT4_RELEASE_DISTRIBUTION=ON` drops the build-time shader folder from the executable; shaders load only
  from `<exe folder>\shaders` (the build tree gets its own copy) and a missing folder stops with a clear message.
  slice213 = the slice211 recipe plus the switch; `--mode pcsx2` = slice213 (`docs/inputs/slice213-pcsx2.json`),
  slice211 kept as `pcsx2-slice211`. String scan: no absolute path or user name left.
- Launcher: `launcher/tools/build-release.ps1` builds with `--remap-path-prefix` (Cargo trim-paths is unstable) and
  fails on any user or build path left. The launcher already links the VC runtime statically (Tauri default); it
  imports only the Windows UCRT (`api-ms-win-crt-*`, part of Windows 10/11).
- Requirement checks before setup and Play (`gt4l_core::system_check`, pure and tested; `gt4l_platform::system`
  reads CPUID and probes `vulkan-1.dll`): SSE4.1 with SSE3/SSSE3 (owner decision 2026-10-21; the game executable is
  baseline x86-64 and the PCSX2 GS needs SSE4.1, so the game has no own check), and a GPU with Vulkan 1.2 plus
  `VK_KHR_surface` and `VK_KHR_win32_surface` (the PCSX2 GS renders with Vulkan too, so the check is always on).
- Game Vulkan baseline path (slice214, release blocker): `native/vulkan_extensions.cpp` (pure, CTest
  `native_vulkan_extensions`) enables swapchain maintenance 1 (KHR or EXT, with its instance prerequisites) only when
  the driver has it; otherwise slots wait on the GS timeline and each swapchain image has its own present semaphore.
  `GT4_VULKAN_FORCE_BASELINE=1` forces it (CTest `native_vulkan_pixels_baseline` presents to a hidden window on it).
  slice214 = slice213 plus this change; `--mode pcsx2` = slice214.
- Installer: detects the Evergreen WebView2 Runtime (HKLM WOW6432Node, HKLM, HKCU `pv`) and runs Microsoft's
  bootstrapper (embedded, Authenticode-checked) only when it is missing; default name `GT4Recomp-Setup-X.Y.Z.exe`;
  the uninstaller also removes `SDL3.dll`.
- Launcher controls: the built-in keyboard keys of the `gt4controls` crate now match the TASK-076 layout (they were
  the old Z/X table; a remap wrote them into `controls.toml`). `cargo test --workspace` green again (233 passed).
- Release: README for 1.0.0, `docs/release-notes/1.0.0.md`, draft GitHub release v1.0.0 with the installer and
  `SHA256SUMS`. The owner tests the installer end to end before the merge and the publish.
- Saves in Documents (owner request 2026-10-21): the save library and replays live in `Documents\Gran Turismo 4`
  (`Saves\Active`, `Saves\Save NN`, `Saves\.backup`, `Replays`), found with SHGetKnownFolderPath(FOLDERID_Documents)
  (OneDrive and redirection followed; data-folder fallback `<data>\Gran Turismo 4` with a logged warning;
  `GT4RECOMP_DOCUMENTS_DIR` is a dev and test override). The disc copy, prepared files, gs, config, controls, logs,
  sessions, `runtime\card.ps2` and `library\backups` stay in `%LOCALAPPDATA%\GT4Recomp`. One-time migration
  (`gt4saves::migration`): copy, size and sha256 check, rename the old `library\memcard` and `library\saves` into
  `library.migrated-<UTC stamp>`; both non-empty = no merge, conflict logged; failure = staging copy removed, old
  data untouched; marker `config\library-migration.json`. The guard refuses the player's folder for install,
  update and renderer writes. Saves, Replays and About show the folder with Open folder. The page items now carry
  `id` and `modified` (the real launcher sent neither, so activate and delete could not name an item). Uninstaller
  page: saves and replays in Documents are always kept; the AppData box stays unchecked. `cargo test --workspace`
  250 passed; nav-test 150/150, focus-check pass; scratch exe run migrated a copy of the owner's library.
- Settings feedback (owner, 2026-10-21, round 4): the game is fixed at 480p and 16:9 until the ultrawide update. The
  Input resolution, Widescreen 16:9 and GS Aspect ratio rows are removed; the launcher always sends
  `GT4_FORCE_480P=1` and `GT4_FORCE_WIDESCREEN=1` (no `GT4_RACE_DISPLAY`) and writes `AspectRatio = 16:9`. Old values
  are ignored by a load-time rule (`settings::RETIRED_SETTINGS`; settings stay schema 3, TASK-080 keeps schema 4).
  GS table column `forced` (Python and Rust writers): `aspect` 16:9 and PCSX2's `osd_fps`, `osd_gpu`,
  `osd_resolution` hidden and always written off; the other stock OSD rows hidden; allow-list unchanged. Advanced is a
  section at the bottom of Graphics (no Advanced tab; D-pad reaches it, B back, Y Reset). `cargo test --workspace`
  252 passed; Python 598 OK; nav-test 150/150; focus-check 93/93 pass. Installer rebuilt (sha256 8cf61ef4...), draft
  assets replaced.
State (2026-10-21): ✅ Done. The owner tested the installer end to end and approved it; merged into main and published as release v1.0.0.

### TASK-082 — Native variable frame rate

Owner update2026-10-26: experimental branch preserved at15b7bf3, deferred for CoreGS2.0; excluded from rc7. See TASK-133 and the remote resumption report.
Owner request 2026-10-21: 30, 45, 120, 144 or unlocked FPS with game speed unchanged, real frames from the game rather
than image interpolation (TASK-045 stays the fallback and runs its race oracle in parallel). Study:
docs/NATIVE-VARIABLE-FPS-STUDY.md. GT4's main loop (`func_001016E8`) updates every object with dt 1/60, renders every
object, then flips; render already takes a time `t` (`render_world(buf, t)`), and the race camera lerps two poses with
it. Race time counts simulation steps, and replays and ghosts are frame-indexed inputs.
Owner decisions (2026-10-21, the study's recommendations accepted):
- N1: the simulation (physics, AI, timers, the 60 Hz guest VBlank) stays fixed at 60 Hz; only rendering changes rate.
  N2 (BLACK-style variable dt) is rejected for vehicle physics and allowed only for cosmetic effects.
- First P1, render skip below 60: game speed stays 100% down to 20 FPS with an identical simulation. Then extra
  render-only passes above 60 (P2 camera-only at 120 Hz in Time Trial; kill gate: a pass over 7.3 ms), P3 cars and
  scene, P4 present pacing (cap, VRR, unlocked), P5 a combined scheduler.
- Extra frames interpolate (+1 frame) by default; latency is cut elsewhere later. Extrapolation stays an option.
- Allowed: the main-loop ELF patch with re-translation (superseding decision 0055 for this feature), a GS stream
  PRESENT record and the gt4-gs change. Menus stay at 60 Hz.
Cost today: race critical path 10-19 ms of 16.7; one extra render pass 7-15 ms, so 120/144 needs cheaper passes.

### TASK-083 — Faster loading and saves (future study)
Owner request 2026-10-21. Loading and saves run at PS2 speed today: the CDVD HLE models every read as 0.4 ms plus
0.25 ms per 2 KiB sector of guest time (about 8 MB/s, donor `iop/cdvd.cpp` `read_cycles`), and the guest clock stays
paced at 60 VBlanks per second during loading screens and saves. The owner reports that PCSX2's Fast CDVD breaks
nothing in GT4 but does not gain much, and asks for a deeper, deliberate speed-up, game code changes included.
Study first: measure where boot, race-entry and save time goes (disc wait, decompression, fixed waits in game code,
card writes, VBlank pacing). Candidate levers: instant disc reads (and a faster byte copy than `write8` per byte);
an unthrottled guest clock while a loading or save screen is active (the TASK-077 study found the loading object),
with audio muted or kept at the right pace; hooks or ELF patches that shorten fixed waits and per-frame save
chunking. Never during races or replays; physics, lap times and replays untouched. After 1.0.0.

### TASK-084 — Frame cost optimisation (Done 2026-10-22)
Owner request 2026-10-21. Study: docs/FRAME-COST-STUDY.md (slice211, headless, timed only with no builds running).
In the day race the EE thread takes 8.2 ms per frame, of which the translated game code is 2.9 ms; the rest is VIF1
job posting (about 5,200 MTVU jobs per frame, one doorbell), the journal GS front end, stream writes, IOP, run loop and
waits for the VU1 worker. The night-race lens-flare readback (2 KiB per frame) adds 3.3 ms of EE wait plus a 0.9-1.6 ms
GS round trip and serialises EE and VU1. Ranked plan: (1) defer the flare readback one frame, ordered through the
journal, after checking nothing in gameplay reads it (-4 to -5 ms night); (2) one MTVU job per VIF1 DMA chain and
wake-ups only when waiting (-1 to -2 ms); (3) no EE wait for the VU1 worker at every VBlank in pcsx2 mode (-1 to -3 ms);
(4) drop the second GIF walk and a copy (-0.4 to -0.8 ms); then compiler flags, run-loop fast paths, VU1 speed. Add the
missing F8 instrumentation first (pacing-sleep scope, fixed summary formula, per-readback records). Each step: its
own slice, headless frame identity, CTest and Python, owner F8 check. Feeds TASK-082 (a render pass must fit 7.3 ms for
120 Hz) and weaker PCs. After 1.0.0. **Done 2026-10-22** (branch `task/084-frame-cost-122`, build `slice262`, pin
`docs/inputs/slice262-pcsx2.json`; PR into main opened the same day). Owner in-game test, session
`private/manual-play/session-20261022-163408-808184`, `--mode pcsx2`, 0 late frames in every scene, picture
unchanged, approved:
- Showroom: frame median 16.689 ms, p99 19.056; critical path 6.06 ms/frame; vif jobs/frame 35.8; EE wait 0.000.
- Nordschleife Time Trial (Supra Wood One, 480p): median 16.664, p99 18.304; critical path 6.13; jobs/frame 58.6;
  EE wait 1.135 (download), readback 0.68.
- Night race, Clubman Stage Route 5, 5 AI: median 16.663, p99 18.565; critical path 9.97 (TASK-041 owner baseline
  11.8); jobs/frame 80.4; EE wait 3.052 (download), readback 1.30; 0 audio underruns.
Headless (slice262, this PC): day race 12.07 -> 10.38 ms, night 13.21 -> 12.08, showroom 9.88 -> 8.61, menu
1.54 -> 1.41; frame dumps and the GS stream session hash identical to release 1.2.0 (slice240);
`GT4_MTVU_VERIFY=1` 0 mismatches in 4,200 frames; CTest 40/40; Python 640 tests OK (42 skipped). Switches:
`GT4_MTVU_CHAIN` (one VIF1 job per DMA chain), `GT4_GIF_FASTPATH` (no second GS stream walk),
`GT4_MTVU_VBLANK_JOURNAL` (VBlank as a journal entry in pcsx2 mode), `GT4_PERF_COUNTERS`. Note: the night-race
download wait and the GS readbacks stay out of scope (TASK-041 Performance Mode and CoreGS 2.0 cover them). Build,
numbers and the owner checklist: docs/BUILD.md "Slice 260/262".

### TASK-085 — Launcher self-update starts the setup outside its job
Done 2026-10-21 (branch `task/085-launcher-self-update`). Owner report: launcher 1.0.0 offered 1.1.0, downloaded it,
closed, and nothing was installed. Cause (Confirmed): the launcher sits in a kill-on-close Job Object so the game ends
with it, and the setup program it started for the update joined that job; Windows ended the setup as soon as the
launcher exited. The log showed "setup verified; starting it" and the verified setup stayed in
`%TEMP%\GT4Recomp-update`. Fix: the job allows breakaway and the setup starts with `CREATE_BREAKAWAY_FROM_JOB`
(`gt4l_platform::process::spawn_outside_job`); when an outer job forbids that, Explorer starts it and the launcher waits
3 s. The game still ends with the launcher, and a launcher update is refused while the game runs. Test
`gt4l-platform/tests/job_breakaway.rs` runs the real job code in a child process: a helper started outside the job
survives the exit, a plain one dies. Launchers 1.0.0 keep the bug, so their users install 1.1.0 by running the setup
(release notes). Release v1.1.0 was set back to draft while fixing it and republished with the fixed launcher.

### TASK-086 — Progressive 480p everywhere
**Shelved (owner, 2026-10-21): the game stays 480i.** Step 1 was built on branch `task/086-progressive-480p` (a3873a1,
slice232, kept for a later restart; not merged). Owner test of the 1.1.1-test installer: boot menus stay 480i (known);
after the first race the menus are 480p, but the car selection shows large green/black garbage blocks over the car
picture, and the next race load hangs on its loading screen (frames still produced). Likely a VRAM layout or
render-target difference of the 480p CT16 menu mode in the car-select/garage path; not investigated further.
Original plan (owner, 2026-10-21; after the CoreGS 2.0 parity work, shares the ELF patch table with TASK-078 P5 and TASK-080
P3). Study: [docs/PROGRESSIVE-EVERYWHERE-STUDY.md](docs/PROGRESSIVE-EVERYWHERE-STUDY.md). The 480p "Try..." pop-up in
the Screen options proves the menus already render at 480p: `MOption.applyRaceDisplayMode` (`func_00436640`) requests
CRT row 6 (480p, 640x448) and `applyMenuDisplayMode` (`func_00436740`) returns to row 1 (480i field). Plan:
(1) menus and races, behind a switch: `func_00436740` requests 480p (0x436750 `24050001`→`24050002`, 0x436754
`0000302D`→`24060006`) and races always take the 480p index (0x43665C); same VRAM footprint as 480i menus; removes the
1-second black between menus and races; (2) CRT table row 2 (photo pause, after movies) and boot init
(`func_001063A8`, `func_00107788`); (3) optional: movies and photo develop (256x384 buffers, risky) and CT16 vs CT24
menus. Checks: SMODE gate, the 42-dump headless run, owner pass on menus, garage, colour window, license tests, race,
replay and split screen. Risks: two hard-coded halvings (`func_00218170`, `mRenderContextPS2__virtual_67`), 16-bit
banding in menus. For CoreGS 2.0 it moves the deinterlacers (most of M15) and field compose off the main path; a small
interlaced fallback stays. Owner decisions open: progressive by default, CT16 or CT24 menus, movies in scope.

### TASK-096 — Remove CoreGS 1.1 from the launcher

State (2026-10-22): done on branch `task/096-remove-coregs11` for launcher release 1.2.3. Not merged; waiting for the owner test and approval.
Owner decision (2026-10-22): CoreGS 1.1 is removed from the launcher. Only GSdx (the external PCSX2 GS process `gt4-gs.exe`) runs
the game. CoreGS 1.1 stays inside the game executable (no native change) and the launcher never starts it.
- Renderer: the choice lists GSdx (selected) and CoreGS 2.0 (coming soon, disabled). `launch::choose_renderer` refuses to start
  the session when GSdx is not installed or fails verification; Play does not start the game.
- Error text: "GSdx could not run the game: {reason}. Check your internet connection and try again. Details are in the log
  folder: {folder}." Reasons: "GSdx is not installed yet", and "GSdx stopped while playing" for a restart request.
- Restart: the game's restart-with-CoreGS-1.1 request (`GS_RESTART_MARKER`) ends the session with that error; nothing restarts.
- Settings schema 7 (`migrate::settings_6_to_7`): a stored `renderer = "coregs11"` becomes `pcsx2`. The CoreGS 1.1 rows
  (Internal resolution, Scaling filter, FSR sharpness, Deinterlacing, Anti-aliasing) are retired: ignored on load, dropped at the next write.
- Setup: the renderer chooser dialog is gone; setup installs GSdx. A failed download finishes setup with a note, and Play
  refuses until GSdx is installed.
- Docs: README, `docs/launcher/gs-settings.toml` (`gs_engine` lists GSdx only), `docs/launcher/FLOW-AUDIT.md`. Old plans and
  release notes keep their history.
- Supersedes TASK-089 (its CoreGS 1.1 path no longer exists).
- Checks: `cargo test --workspace` all green (launcher tests with the new migration, renderer-choice, restart and renderer-switch tests).

### TASK-093 — Custom textures straight in the Textures folder
State (2026-10-22): done on branch `task/093-textures-root`; waiting for gt4-gs 1.2.1 (not published yet) and the owner
test. Planned for release 1.2.3 with TASK-092.
- gt4-gs 1.2.1 reads the `Textures` folder itself, recursively (`Textures\replacements\...` still works). A pack goes
  straight into `<install>\data\Textures`.
- The launcher creates only `Textures` (no `replacements`). `RENDERER_MINIMUM` is 1.2.1: an installed gt4-gs 1.2.0 is
  replaced at the next launch (`gs_release::needs_install`, as before with 1.0.1 and 1.2.0).
- One-time flatten at start, after the large-data move, on the same thread (`relocation::flatten_textures`): only when
  gt4-gs 1.2.1+ is in use (`current.txt`), only when `replacements` is a real folder (junctions and links are skipped),
  each child renames up into `Textures`, a name already there stays in `replacements` (logged), `replacements` is
  removed only when empty, and the first error stops the rest until the next start. Nothing is deleted or overwritten.
  Pure logic and tests: `gt4l-core/src/textures.rs`.
- Text: `docs/launcher/gs-settings.toml` (`texture_load`, `textures_folder` notes), the Open textures folder comment,
  README bullet. The 1.2.1 release notes stay as history.

### TASK-094 — Desktop shortcut for the installed program
Built on branch `task/094-desktop-shortcut` (2026-10-22), waiting for the owner test; planned for launcher release 1.2.3.
Owner request (2026-10-22): the installer makes Start-menu shortcuts only, and players want a desktop icon.
- Interactive install and update: the finish page offers "Create a desktop shortcut", ticked by default (after "Start Gran Turismo 4"). A choice made earlier (`DesktopShortcut` = 0) unticks it.
- The shortcut `Gran Turismo 4.lnk` is on the all-users desktop and starts `$INSTDIR\GT4Recomp.exe` (working folder and icon: the same).
- The choice is kept in HKLM `DesktopShortcut` (1 created, 0 declined). Uninstall removes the shortcut; the app key goes with the uninstall.
- Silent runs (`/S`, and the launcher's `/S /UPDATE` self-update): `DesktopShortcut` missing (an install from before 1.2.3, or a fresh silent install) or 1 creates or makes again the shortcut; 0 does nothing.
Acceptance: `makensis` compiles with no new warnings; owner test of a fresh interactive install (box ticked and unticked), an update from 1.2.2 (box follows the stored choice), a silent update, and the uninstall.

### TASK-092 — MTVU test switches after TASK-084
🟡 **Done on branch (2026-10-22), waiting for release.** Branch `task/092-mtvu-test-switches`, slice264 (build recipe
in BUILD.md "Slice 264"). Owner decisions 2026-10-22: release 1.2.2 ships the owner-tested slice262 build; this fixes
the TASK-084 review findings afterwards. None is reachable from the launcher, which sets neither variable.
Result: `GT4_MTVU=0` in pcsx2 mode no longer crashes (slice262: access violation, no frames; slice264: 4,200 frames,
41 dumps, 4,200 VSYNCs at the GS); `GT4_MTVU_VERIFY=1` sends its VSYNCs (4,200 frames, 0 mismatches); the default path
is identical to slice262 (dump hashes and GS stream session hash equal in menus and night). Open before merge: the
`gt4vulkan.exe` pin tests (`test_make_distribution`, `test_portable_install`) still point at slice262's executable;
the new pins (a slice264 pin set) need an owner decision on the release path. CTest 40 of 41 pass (the 41st is
`python_native_contracts`, the same two pin tests).
- `GT4_MTVU=0` in pcsx2 mode crashes at the first VBlank (Confirmed by reading): `mtvu_bridge.cpp` sets
  `set_vsync_deferral(settings.vblank_journal)` without checking `enabled()`, `start()` returns before the journal
  exists, and `vblank_sync()` calls `r.journal->append_vblank_flush()` on a null journal.
- `GT4_MTVU_VERIFY=1` in pcsx2 mode sends no VSYNC (Confirmed by reading): `vblank_sync()` keeps the blocking drain,
  but the GS stream client still holds the field and nothing calls `post_pending_vsync`.
- Fix: one condition, `vblank_journal && enabled() && !verify`, for both the client deferral and `defer`.
- Doorbell sleeper handshake (`ring` / `wait_changed`): seq_cst on the four operations so the store-then-load
  pattern holds on ARM (Hypothesis today; x86 locked RMWs already order it). Needed before the Android phase.
- Acceptance: CTest fixtures for both switches (a VSYNC reaches the stream; no crash), the frame identity matrix and
  the GS stream session hash unchanged against slice262 with default switches, a new slice and pins.
- 1.2.2 bug found by the CoreGS 2.0 agent (2026-10-22), fixed here: donor mode (CoreGS 1.1) crashed at the first
  guest VBlank (exit 0xC0000005; slice262 and slice264 crash, slice240 runs). `mtvu_bridge.cpp` declares `Donor donor;`
  and sets `post_vsync` only in pcsx2/hw mode; the struct had no initialisers, so `vblank_sync` took the TASK-084
  deferred path and called stack garbage. Every `Donor` callback is now `= nullptr`; `native_mtvu_switches` checks a
  Donor default-initialised over 0xFF memory. slice268 (O:\gt4recomp-scratch\slice268-build, the slice264 recipe):
  CTest 40/40; headless menus with `--renderer donor` and `pcsx2` both run (exit 0). The launcher drops CoreGS 1.1 in
  1.2.3 (TASK-096); the fix stays as protection for runs without the launcher and the built-in restart path.

### TASK-101 — Multi-game launcher (Gran Turismo 4 and Spec II)
Planned (owner, 2026-10-23) for release 1.3.0, the product side of TASK-100 (its S9). Plan and owner decisions:
[docs/MULTI-GAME-LAUNCHER-PLAN.md](docs/MULTI-GAME-LAUNCHER-PLAN.md). One executable per game, one shared settings set,
per-game disc, prepared files, card, saves and replays (saves are incompatible both ways and share the PS2 folder
name), identification by ISO, the selected game is the last played, Spec II 1.10 and same-executable variants, and
the Spec II logo on the home screen when it is selected.
Progress (2026-10-23): launcher side implemented on `task/101-multi-game` (pieces 1 to 6 of the plan; the UI in the mock and
headless checks): game table, per-game folders and libraries, identification by serial and executable hash with named image
variants (1.10, PT-BR, variant, refusals), Spec II setup with the Rust port of the ELF patch (byte-identical to the script),
per-game launch, the 1.2.x to `games\gt4` migration, selector, Install, Settings > Games. Owner decision 2026-10-23: no permission from the Spec II authors is needed; credit them by name and ship none of
their files (full credit list in the release notes, About and README). Open: look on a real window, the authors'
logo, installer build, `PreviousDataDir` into the games layout.

### TASK-102 — Launcher language: automatic en-US / pt-BR with a General > Language setting
Implemented on `task/101-multi-game` (owner, 2026-10-23). The launcher follows the Windows language (`auto`): pt-BR, pt_BR
and pt (and pt-PT, by the base language) give Portuguese; anything else English. Settings > General > Language (Auto,
English, Português (Brasil)) applies at once, without a restart. The texts are data: `launcher/crates/gt4l-tauri/ui/locales/<tag>.json`
(one flat file per language, keys with `{placeholders}`, en-US as the fallback), `ui/locales/index.json` (the language list),
`ui/js/i18n.js` (`GT4.t`), and the launcher's own messages in the same files under `rust.*` keys (`gt4l-core/build.rs`
generates the table; `gt4l-core/src/messages.rs`). The short release notes are in `docs/release-notes/launcher/<version>.md`
(en-US) and `docs/release-notes/launcher/<tag>/<version>.md` (pt-BR for 1.0.0 to 1.2.3). A new language is documented in
[docs/TRANSLATING.md](docs/TRANSLATING.md). Checks: focus-check in en-US (151 of 151) and pt-BR (158 of 158, with the
locale parity, placeholder and index checks), nav-test 150 of 150, `cargo test --workspace` 450 passed. Screenshots of Home,
Settings > General and Settings > Graphics in pt-BR at 1440 x 860: `O:\gt4recomp-scratch\slice279-shots`. Not translated yet:
the PCSX2 GS rows that the table does not list in the UI keys (they show their English text), the XInput names in the
controller chips, the key names, the Release notes text of 1.3.0 in the sample (dev mock only), and the library's
`Active save` slot name (a value the code compares). Owner approved the mock in pt-BR (2026-10-23; main-agent review:
cargo 450 passed, focus-check en-US 151/151 and pt-BR 158/158, nav-test 150/150). Open: the owner test on a real
window with the 1.3.0 candidate.

### TASK-103 — Graphics API choice for GSdx (Vulkan, Direct3D 12, Direct3D 11)
Implemented on `task/101-multi-game` (owner, 2026-10-23), release 1.3.0; owner in-game test pending. Settings > Graphics
shows "Graphics API (GSdx)" (table row `graphics_api`, written as `[EmuCore/GS] Renderer` = 14, 15 or 3): Vulkan
(recommended, default), Direct3D 12 (recommended), Direct3D 11 (older PCs). The row moved from the Advanced section to the
Graphics page. The engine setting keeps its id `renderer` (GSdx or CoreGS 2.0) and its label "Renderer"; the new row
cannot be confused with it in the UI, the settings store or the table. A missing or unknown stored value, and an existing
install, use Vulkan. No OpenGL and no software renderer (OpenGL later, with a new gt4-gs build and its allow-list entry).
Research (2026-10-23, read-only): gt4-gs.exe already contains Direct3D 11, Direct3D 12 and OpenGL; the backend follows
`[EmuCore/GS] Renderer` (DX11 3, OpenGL 12, Vulkan 14, DX12 15). Checks: `cargo test --workspace` 453 passed; Python
`test_gs_*` 168 tests OK; focus-check en-US 151 of 151 and pt-BR 158 of 158, nav-test 150 of 150; screenshots of
Settings > Graphics in `O:\gt4recomp-scratch\slice280-shots` (pt-BR and en-US). Open: the owner in-game test on Direct3D 11
and Direct3D 12 against Vulkan (Nordschleife TT 480p, night race, menus, ultrawide crop and fill, texture pack, upscaling,
flares and readbacks). A backend that fails the owner test is not offered.

### TASK-104 — Launcher saves: crash-proof card import, New save, Export, Rename
Planned and implemented (owner, 2026-10-23) for release 1.3.0. GitHub issue moesuito/gt4-staticrecomp#86: importing a PCSX2
`Mcd001.ps2` closed the launcher with no message. Reproduced on the old reader with synthetic cards: a directory that points
back to an ancestor overflowed the stack (0xc00000fd), and a 4 GB length aborted on a 2 TB allocation (0xc0000409). Both are
silent in the launcher. Fix: the reader takes the card size, the allocation area and the indirect FAT list from the
superblock and accepts 8, 16, 32 and 64 MB cards (with or without spare bytes); every length is checked against the chain
and the card before allocating; directories nest at most 8 levels and each directory is visited once (a loop is
`corrupt card`); the import runs under `catch_unwind` and logs the file name and size. Messages: not a PS2 memory card,
unsupported card size, card damaged (with the reason), no Gran Turismo 4 save on this card (with what it holds).
Saves page: New save (one confirmation; the current save is kept as a new `Save NN`, the last run's card is harvested and
moved to `backups` first), Export of the active save or a slot as `.psu`, Rename (name in `name.txt` beside the save,
sanitised, 40 characters, moved with the save when saves are swapped). Tests: `gt4saves/tests/hostile_cards.rs` (10 synthetic
card cases, built in `tests/common`), `gt4saves/tests/save_management.rs` (9 library cases). Checks: `cargo test --workspace`
474 passed, 0 failed, 4 ignored; focus-check en-US 158/158 and pt-BR 165/165, nav-test 150/150. Screenshots:
`O:\gt4recomp-scratch\slice283-shots\saves-en-US.png` and `saves-pt-BR.png`. Open: the panic guard is not covered by a test
(no injected panic); the owner test with a real PCSX2 card and in-game New save; gamepad rename needs a keyboard to type.

### TASK-105 — Integrated GPUs: lighter default and GPU-lost message (#89)
Planned and implemented (owner report, 2026-10-23) for release 1.3.0. GitHub issue moesuito/gt4-staticrecomp#89, reported on
1.2.3: Intel Core i3-1005G1 with the integrated Intel UHD only, 1440 x 900 screen. The default Render scale gave k 3
(1920 x 1344 internal); GSdx lost the device ("Host GPU lost too many times, device is probably completely wedged") at 3x and
at 2x. Fix: `display::integrated_default_applies` and `display_plan_on`: when the GPU the game uses is integrated (Vulkan
device type 1, `system_check::integrated_gpu_name`; with any discrete card, or none, the default is unchanged) a Render scale
that is the untouched default (100%) plays at 1x (`INTEGRATED_DEFAULT_MULTIPLIER`, 640 x 448). A changed scale keeps its
multiplier. The stored value cannot tell an untouched 100% from a chosen one: a chosen 100% on an integrated GPU also plays at
1x. The decision is logged (`display: integrated GPU <name>: default internal 1x`); the Render scale readout follows the plan.
The device comes from the same Vulkan probe as the system check (`gt4l_platform::system::integrated_gpu_name`, read once per
launcher process). GPU lost: a crashed session whose `game.log` holds a GSdx device-lost text (`exit::GPU_LOST_MARKERS`:
"Host GPU lost", "device is probably completely wedged", `VK_ERROR_DEVICE_LOST`) shows the plain message in en-US and pt-BR
(`rust.game.gpu_lost`) in place of the generic crash text; the matched line goes to the launcher log. Saves dates use the
interface locale (`Intl.DateTimeFormat` with `GT4.i18n.intlLocale()`; pt-BR showed English months). Not changed: the default
Graphics API (Vulkan) and gt4-gs. Checks: `cargo test --workspace` (launcher) 487 passed, 0 failed, 4 ignored (new tests in
`display.rs`, `system_check.rs`, `exit.rs`, `tests/launch_contract.rs`, `model.rs`); focus-check en-US 158/158 and pt-BR
165/165, nav-test 150/150. Open: the owner test on the Intel UHD machine (1x default and the message are not yet seen in-game);
the exact device-lost line in a real `game.log` is not confirmed (the markers come from the GSdx dialog text); a multi-GPU
laptop keeps the discrete default by design.

### TASK-106 — CHD disc images (issue #90)
Planned (owner, 2026-10-23) for release 1.3.1. Issue moesuito/gt4-staticrecomp#90 asks for CHD support. The launcher
already writes its own ISO image of the player's disc at setup, so the game never reads the player's file: accept `.chd`
(and keep `.iso`) in the disc picker, decompress it to that image with a permissively licensed CHD reader (MIT or
BSD; check the licence before use; never GPL in the launcher), then run the normal identification (serial and
executable hash, both games and their variants). Show progress and free-space needs (a full image is about 3.7 GB for
Spec II). Acceptance: synthetic CHD fixtures in tests (no game data), a CD and a DVD CHD, a damaged CHD refused with a
plain message, and an owner test with a real CHD of each game.

### TASK-107 — Automatic graphics setup per GPU
Planned (owner, 2026-10-23) for release 1.3.1, after issue #89 and TASK-105. Owner design:
1. **Game without Vulkan (check first).** Done by TASK-108 (1.3.0): the game program is not checked for Vulkan
   when the chosen API is not Vulkan, and Automatic falls back to Direct3D 11. Today `gt4vulkan.exe` itself requires Vulkan 1.2 (`native/native_vulkan.cpp`,
   `system_check.rs`), so a GPU without Vulkan cannot play even when GSdx draws with Direct3D or OpenGL. Find out
   whether the game needs its own Vulkan device when `GT4_RENDERER=pcsx2` and, if not, make that path Vulkan-free and
   relax the launcher check for it. Everything below depends on this.
   First finding (2026-10-23, High confidence, from the owner's 1.2.3 game.log): in pcsx2 mode the game logs
   `[gs-ipc] GT4_RENDERER=pcsx2: donor Vulkan renderer not created`, so the game creates no Vulkan device and only
   gt4-gs renders; the Vulkan 1.2 requirement then comes from the launcher's system check (and gt4-gs on Vulkan). To
   confirm: no other Vulkan call on that path (the loader is loaded dynamically) and a run on a machine without Vulkan.
2. **Automatic Graphics API.** Vulkan when the GPU reports Vulkan 1.2 or newer with what GSdx needs; otherwise OpenGL
   on NVIDIA and Direct3D 11 on Intel and AMD (OpenGL tends to stall on non-NVIDIA GPUs; OpenGL has the same blending
   precision as Vulkan and Direct3D 12; Direct3D 11 is for weak and old hardware only). A player's own choice always
   wins; the automatic choice is shown as such.
3. **OpenGL in gt4-gs.** Allow `Renderer = 12` in a new gt4-gs release (allow-list and SHA256SUMS in the fork) and in
   `docs/launcher/gs-settings.toml`; offer OpenGL in the Graphics API list ("older PCs" group) after an owner test.
4. **GPU generation table.** A small table we maintain ourselves (no third-party database copied; check the licence of
   any source used for reference): vendor ID plus PCI device-ID ranges per GPU generation (for example Intel Gen9, Gen11,
   Xe; AMD GCN, RDNA1-3; NVIDIA Maxwell to Ada), so every variant of a family (RX 6600, 6600M, 6600 XT, 6650 XT) falls
   in the right range. Each generation gives a starting profile (internal scale, API). An unknown GPU uses the driver's
   own facts (integrated or discrete, video memory, Vulkan or Direct3D level) and else the default.
5. **Step down after a lost GPU.** When a session ends with a GPU-lost line (TASK-105), the next start uses one level
   less (lower internal scale, then Direct3D 11), told to the player, never below 1x; a successful session keeps it.
6. **Auto render scale token.** Store the untouched Render scale as `auto` instead of 100 (settings migration), so an
   automatic choice and a chosen 100% are told apart (TASK-105 limit).
Acceptance: pure unit tests for every rule (tables, fallbacks, step-down), the migration test, and owner tests on this PC
plus at least one integrated-GPU machine.

### TASK-108 — Launcher: automatic graphics API and card; game programs renamed gt4.exe and gt4-spec2.exe
Status: done in the working tree (branch `task/101-multi-game`, release 1.3.0); owner test pending. Owner decisions 2026-10-23.
Completes step 1 of TASK-107 (the game no longer needs Vulkan to start).
1. **Automatic Graphics API.** `graphics_api` gets a new first choice, **Automatic** (`auto`), and it is the default for
   new installs and for installs that never changed the row (no stored value). It is resolved at every launcher start and
   before each Play: Vulkan when the chosen graphics card reports Vulkan 1.2 or newer, otherwise Direct3D 11. The Settings
   page shows the resolved API ("Automatic (Vulkan)"). An explicit Vulkan, Direct3D 12 or Direct3D 11 is kept.
   Explicit Vulkan on a card without Vulkan 1.2 runs with Direct3D 11 for that run, with the note "No Vulkan 1.2 graphics
   card was found, so the game uses Direct3D 11. You can change this in Settings > Graphics." (pt-BR in the locale file).
   The Vulkan check is per run, so a driver update brings Vulkan back without a setting change. Setup needs no GPU;
   the processor check (SSE4.1) is unchanged.
2. **Graphics card and Adapter.** The launcher lists the cards: Vulkan device names from its Vulkan probe and DXGI
   adapter descriptions (`IDXGIFactory1::EnumAdapters1`; software adapters skipped). The **Graphics card** row follows the
   Graphics API row: "Automatic (best)" (default) and every card by name. Automatic picks a discrete card over an
   integrated one, then the most dedicated video memory. The chosen card's name goes to the game's `Adapter` key: the
   Vulkan name for Vulkan, the DXGI description for Direct3D 11 and 12. No name for the API means an empty Adapter (system
   default) with a log line. A stored card that is gone (unplugged eGPU, new PC) falls back to Automatic for that run,
   with a log line. The `Adapter` line is written under `[EmuCore/GS]` by the launcher (`gs_config_text`), because the
   table row `adapter` is still `resolved` (not settable from a value; the table and `scripts/gs_settings.py` are unchanged).
3. **One chosen card.** The Vulkan support decision (point 1) and the integrated-GPU 1x default (TASK-105) use the chosen
   card instead of "all GPUs integrated".
4. **Logs.** Every Play logs the card list, the chosen card, the API and the Adapter value.
5. **Game programs renamed at distribution.** `gt4vulkan.exe` is `gt4.exe` and `gt4vulkan-spec2.exe` is `gt4-spec2.exe`
   in the installer, the portable zip, the game folder and the launcher (`games.rs`, paths, launch, process detection,
   update and migration). The build output names are unchanged. The files are byte-identical and pinned by hash, so no
   game rebuild and no new pins. `scripts/make_distribution.py` copies them under the new names; the installer deletes
   the 1.2.x names from `game\` on install and update, and checks both sets of names for locks; the uninstaller removes
   the whole `game` folder. Process detection and the portable swap cover both old and new names.
   The developer scripts keep `gt4vulkan.exe`, the build output name.
Acceptance: pure unit tests for each rule in `gt4l-core::gpu` (laptop integrated + NVIDIA, integrated only without
Vulkan, two discrete cards, explicit choice kept, missing card, software skipped, name mismatch leaves Adapter empty), the
Vulkan fallback in both languages, `system_check` for the processor, the settings migration of the Automatic default,
the distribution tests with the new names, `cargo test --workspace`, the touched Python modules, the headless Edge focus
checks, and an owner test on this PC (NVIDIA + integrated) and on one machine without Vulkan 1.2.

### TASK-109 — Launcher update channels and version picker
**Implemented for 1.3.0** (owner, 2026-10-23; pulled forward from 1.3.1), on `task/101-multi-game`, owner test pending.
- Settings > General, below Language: **Channel** (Stable default, or Beta) and **Version** (a dropdown read from GitHub
  each time the page opens, with a refresh button).
- Stable offers stable releases. Beta offers the release candidates (`vX.Y.Z-rcN`, GitHub pre-releases) and stable releases
  together, newest by version; a draft is never offered. A candidate build with no stored choice starts on Beta.
- Moving back to Stable never downgrades automatically: the launcher waits for a stable release newer than the installed
  build.
- The version carries its release candidate part (`gt4l-core::version::ReleaseVersion`): `1.3.0-rc4 < 1.3.0-rc5 < 1.3.0 <
  1.3.1-rc1 < 1.3.1`. A build gets its label from `launcher/tools/build-release.ps1 -Label rc5` (`GT4_RELEASE_LABEL`, read
  by `gt4l-core/build.rs`); About, the home credit line, `version.json` and the update decisions use `1.3.0-rc5`. Asset
  names of a test build: `GT4Recomp-Setup-1.3.0-rc5.exe`, `GT4Recomp-1.3.0-rc5-portable.zip`.
- The picker lists, per channel, stable releases by version (Stable) or candidates by publication date with a newer stable on
  top (Beta); drafts and versions below 1.3.0-rc5 are left out (older builds cannot read the multi-game data layout).
  Installing a chosen version asks first. A target older than the installed version (SemVer, any channel) shows the strong
  downgrade warning (back up the saves, a saves folder button, an explicit "Install older version" button that is never the
  default focus); a newer target is always the plain question. Yes runs the normal update path (`install_launcher_version`:
  download, `SHA256SUMS` check, silent setup or portable zip swap); No restores the dropdown. When a channel lists only
  versions older than the running build the page says updates resume when a newer release ships.
- Both languages. Tests: version parsing and ordering, channel default, `decide` per channel, no automatic downgrade, list
  building and sorting, `is_downgrade` and the dialog variant, asset names, a fake GitHub JSON through `FolderSource`,
  `make_portable_zip` with a label; headless Edge focus checks (Yes calls the update path, No restores the selection).
- Not verified here: the real GitHub fetch (the fake JSON follows the REST format), a real install of a chosen version.

### TASK-110 — Controller tuning: stick and trigger deadzone, sensitivity and response
Planned (owner, 2026-10-23) for release 1.3.1. Players report that the game sometimes reads the stick and triggers
differently from what the controller sends. No code yet.
- A per-controller tuning section on the launcher's Controls page, applied by the game's input layer
  (`native/host_io.cpp`, SDL3) and the controls config the launcher writes. It needs a game rebuild and new pins.
- Settings (the feature list only, from what PCSX2 offers its players; no PCSX2 code is used, clean room):
  left and right stick deadzone; stick sensitivity or scale; response curve (linear or curved); outer deadzone or
  saturation; trigger deadzone and saturation (the analog throttle and brake mapped to GT4's pressure buttons and stick
  axes); button pressure modifier; stick inversion; vibration strength.
- A live test view in the launcher showing the raw value and the value the game will see, for each stick and trigger.
- Defaults reproduce today's behaviour exactly. Unit tests for every curve and deadzone rule.

### TASK-111 — Logs for bug reports
Implemented (rc4, 2026-10-23, `task/101-multi-game`) for release 1.3.0, together with TASK-112. The owner moved it from
1.3.1 into rc4 (2026-10-23). Nothing from this entry is left for 1.3.1.
- **Export logs** (About, and offered by a dialog after a crash or a lost graphics card): one zip
  `GT4Recomp-logs-YYYYMMDD-HHMMSS.zip` in a folder the player picks (Desktop by default). It holds `latest.log`, the
  last 3 launcher session logs, the last 3 game session folders' `game.log` and `gt4-gs.ini`, the settings file and
  `system.txt` (Windows version, CPU, every GPU with vendor, driver version and video memory, the chosen card and API,
  the displays, launcher version, install or portable, the installed games).
- The Windows user name and the user profile paths are masked in every text (`C:\Users\<user>`). Cards, saves,
  replays, disc images, prepared game files and programs are never put in (checked by a unit test).
- After the export the zip path is shown, with "Open the folder" and "Report a problem on GitHub" (the named link
  `report_problem`, through `open_link`).
- Both languages. Unit tests in `gt4l-core/src/log_export.rs`: the path and user masking, the forbidden entries, the
  zip contents, the newest-first selection and the system report.
- The size limits are the session scheme of TASK-112 (20 session logs, 20 game session folders), not a size cap.

### TASK-112 — Launcher logs: one file per session, last 20 kept, latest.log
Implemented (rc4, 2026-10-23, `task/101-multi-game`) for release 1.3.0, with TASK-111. Replaces the single
`launcher.log` of 1.2.x, which had no size limit.
- One file per launcher session: `logs\launcher-YYYYMMDD-HHMMSS.log` (local time; a counter when two starts share a
  second). Each line is `[time] LEVEL text`, with `INFO`, `WARN` or `ERROR`.
- `ERROR`: a game crash, a lost graphics card, a setup or import failure, a failed harvest, an update failure, a
  WebView2 problem, a settings file that could not be written. `WARN`: a soft problem (a refused disc, a GSdx
  not installed during setup, a version file not updated).
- The last 20 session logs are kept; the oldest go when a session starts. The running session's file is never removed.
- `logs\latest.log`: a header (version, install kind, Windows, date), the `ERROR` lines of the earlier kept sessions,
  oldest first and each prefixed with its file name, then the current session's lines as they are written. About >
  "Open latest log" selects it in Explorer.
- A 1.2.x `launcher.log` is renamed once to `launcher-legacy-YYYYMMDD.log` and counts as one of the kept logs.
- Game session folders (`data\sessions\session-*`) are capped at the last 20, never the running one.
- Both languages. Unit tests in `gt4l-core/src/logs.rs`: names, rotation at 20, the running file kept, the `latest.log`
  error collection, the legacy migration and the session-folder cap.

### TASK-113 — Spec II race HUD digits at ultrawide ratios
Known issue of 1.3.0 (owner test on rc4, 2026-10-23, ultrawide 3440 x 1440, GSdx 6x): in Spec II only, the race HUD's
current-speed figure and the odometer digits are stretched horizontally and spill past their place on the gauges; the
rest of the HUD (gauges, gear, G meter, labels) is right; at 16:9 and narrower it is all right, and GT4 v2.00 is fine at every ratio. Hypothesis: Spec II draws those digits
through a path the S8 HUD hook (`RaceDisplay::render` 0x1C5040, HUD matrix at this+0x70) does not cover, or with a
second matrix or sprite list the widening scales twice. Find where Spec II draws the speed and odometer digits, compare
with v2.00, and extend the Spec II hook table; headless check at 16:9 and 21:9 with frame dumps. Planned for the next
update; listed under Known issues in the 1.3.0 notes.

### TASK-114 — rc5 hotfixes (laptop test, RTX 3060 Laptop + Intel UHD 630, Windows 11 25H2)
Done in the working tree (branch `task/101-multi-game`, release 1.3.0, 2026-10-23); owner test pending. Four items:
1. **Setup identifies the disc first.** The first-run page is generic ("Install a game") until the picked image is
   identified; the stored game choice no longer names a game there. The image is identified in place (SYSTEM.CNF serial,
   executable hash, known-image table) before any copy, both when picked and again when setup starts; a refused or changed
   file copies nothing. The game installed is the one the image really is, also for an Install button of the other game.
2. **Copy bar reaches 100%.** The steps after the copy keep the finished copy's size, the copy step is drawn with a full bar
   and its final figure, without a width transition.
3. **GPU list.** Two integrated Vulkan devices (or two small-memory DXGI adapters) with the same PCI ids are one card; two
   identical discrete cards with different LUIDs stay two. The launcher log lists every raw Vulkan and DXGI entry with PCI
   ids and LUID at start, and the cards line shows source, vendor and kind.
4. **Black window.** The intro looks up its version without the `-rcN` suffix (an rc never played it before), is never
   started while the page is hidden, never on the setup page, has a 12 s hard limit, and logs start, end and skip. Cause not
   confirmed; see the 1.3.0 notes.
5. **Names and wording (rc5 polish, 2026-10-24).** Settings > Games titles the rows by the game, and Spec II as
   "Gran Turismo 4 Spec II v1.10" (the only supported version). The detail is the status, what tells the copy apart
   (`USA v2.00`; Spec II `PT-BR`, or nothing for the plain 1.10 image, `other image` for an unpinned image) and the size.
   The button "Change disc image" is "Update" (`games.update`). Setup step 1 says `recognised: <title (variant)>`; the Spec
   II variant is known only after the copy is checked, so step 1 shows the title alone until then. Step 2 is "Copying
   and checking the image", step 3 "Final check". "Spec II 1.10" became "Spec II v1.10" in the texts that name the version.
6. **Compact density (2026-10-24).** Every page is denser (body 13 px, 36 px hints and tabs, 38 px rows, 34 px buttons,
   190 px controls); the existing rules changed, no override file. Checked at 1280x720 and 1440x900 in pt-BR.
7. **Launcher icon (2026-10-24).** `launcher.ico` is the GT emblem on a white rounded square (corner 22%, emblem 80%,
   10 sizes), generated by `launcher/tools/make_brand.py` (`write_launcher_ico`). `game.ico` is unchanged.

8. **Settings open at once (rc5, 2026-10-24; Games, Saves and Replays included).** The graphics cards are probed once and kept (`gt4l-platform::gpu`:
   `cached_gpus`, `store_gpus` from the start-up probe, `refresh_gpus` once at Play); the Settings table, the saves and
   the log read the kept list. The Settings tabs open from memory (settings table, About info, launcher version list,
   the Games list, the Saves and Replays lists), prefetched once the home screen is up. A change (import, new save, rename,
   delete, activate, remove a game) updates the kept list from the command's answer; each tab reads again behind and draws again only when something changed.
   "Loading settings..." waits 300 ms and shows only with no data. A tab's rows fade in once (180 ms, 18 ms apart, the last
   starts within 150 ms; off with reduced motion). Probes per action, from the code paths (before, after): start 1, 1;
   open a Settings tab 2, 0 (1 when the first open comes before the start-up probe ends); save a setting 2, 0; Play 3, 1;
   export logs 1, 0. Checks: `cargo test --workspace` 556 passed, 0 failed, 5 ignored (6 new: the cache, and a settings
   table read that probes nothing); focus-check en-US 195/195 and pt-BR 202/202 (one new check: a loaded Saves tab shows its
   list at once), nav-test 150/150. No locale text added.
   Owner test pending; no release-notes bullet (rc polish).
9. **Dropdown values stay on one line (rc5, 2026-10-24; pt-BR owner screenshot).** The value of a dropdown is nowrap and
   cut with an ellipsis when it is too long, with the full text in its tooltip (`app.css` `.select`, `settings.js`). The
   Language row is wide, so "Automático (Português (Brasil))" fits. The opened list may wrap its names inside the panel.
   Focus check: every General and Graphics dropdown value is one line, and the Language value stays one line with its
   longest text, in both languages (en-US 198/198, pt-BR 205/205, nav-test 150/150). Screenshots at 1280x720:
   `O:\gt4recomp-scratch\slice295-shots\general-pt-BR.png` and `graphics-pt-BR.png`. Owner test pending.

### TASK-115 — Game recognition (Windows, GPU drivers, Discord) and Discord Rich Presence
State (2026-11-02): Discord Rich Presence is in 1.3.0-rc7 and rc8, on by default. The owner tested it with the real Discord client: it works.
Planned (owner, 2026-10-24) as a future plan (maybe 1.3.1, maybe later; the owner decides when). Goal: Windows, the AMD and NVIDIA drivers and Discord treat GT4Recomp
as a game, so Game Mode, AMD Instant Replay / NVIDIA ShadowPlay and Discord's "Playing" status work out of the box.
Facts to start from (main agent, to verify): the picture is drawn by `gt4-gs.exe` (GSdx) into a child window of
`gt4.exe`, so a driver or overlay sees the 3D work in `gt4-gs.exe` and the window in `gt4.exe`; RivaTuner already
hooks it (owner screenshots). Windows decides "game" from its own list, a fullscreen 3D swap chain, or the player's
Game Bar choice "Remember this is a game" (stored per user in `HKCU\System\GameConfigStore`); there is no
manifest flag. AMD Adrenalin and NVIDIA App detect games from their own databases and 3D activity and let players
add an executable by hand; their Instant Replay can also record the desktop.
Work:
1. Identity: version resources on `gt4.exe`, `gt4-spec2.exe` and `gt4-gs.exe` (ProductName "Gran Turismo 4" /
   "Gran Turismo 4 Spec II", CompanyName "GT4Recomp", FileDescription, icon), window title the game name.
2. Measure on the owner's machines (AMD Adrenalin, NVIDIA App on the RTX laptop, Windows Game Bar) whether each one
   detects the game today, in fullscreen and windowed, and what changes it (fullscreen mode, flip-model swap chain in
   gt4-gs, the identity above). Never write GameConfigStore or driver profiles behind the player's back; if a manual
   step is needed, a short in-launcher guide ("Add GT4Recomp to AMD Adrenalin / NVIDIA App / Game Bar").
3. Discord Rich Presence (owner: 1.3.1): the launcher (it stays alive in the tray while the game runs) talks to the
   local Discord client over its IPC pipe with a permissively licensed crate (MIT/Apache, never GPL), a GT4Recomp
   Discord application id (owner creates it), and shows "Gran Turismo 4" or "Gran Turismo 4 Spec II", elapsed time
   and the GT4Recomp logo; a Settings > General switch (on by default), off when Discord is not running, no data
   sent anywhere else. In-game details (mode, track, car) need guest-memory reads: a later step.
Acceptance: the owner sees each feature work on his machines; unit tests for the presence payload and the switch.

### TASK-117 — Performance mode, part 2: no light flares at all in Performance Mode (sun included)
**Done in rc6 as a host change; owner visual test passed, F8 pending (2026-10-25).** Two forms were tried: the derived-ELF guest patch (built, parked below: a runtime-switched guest patch cannot pass its identity gate) and the host-side zero table, which ships. The owner tested v2.00 and Spec II, night and day: flares gone, no other visual difference, and the glows no longer draw over the virtual rear-view mirror (a known GT4/GSdx artefact). Partial success until his F8 on a free machine confirms the frame times. Owner decision (2026-10-25 evening): Performance Mode removes every flare, the sun included, because weaker PCs lose frames on the Nordschleife only when driving into the sun.
History (2026-10-24): planned for 1.3.0-rc6 together with TASK-116 step 1, because both need a new GT4 v2.00 game
build (one rebuild, one set of pins).
Idea source: the owner found https://github.com/nobz226/gt-pcsx2-patches (PCSX2 pnach files for GT4 USA, CRC
77E61C8A, and GT3 JP). That repository has **no license**, so it is a reference only: we never copy its stub code or
words; we write our own guest code from our own disassembly and credit the idea in the release notes. Its README
explains the mechanism: each frame the game fills two flare queues (track lights, car lights; limit 100 each) through
one shared visibility test; each queued light draws a glow and, when close, a second larger halo.
Facts checked against our reconstructed ELF (`private/reconstructed/SCUS_973.28.elf`, 2026-10-24, Confirmed):
- 0x3B7BA8 `slti a2, a2, 0x64` (track-light queue limit 100);
- visibility test at 0x3B7C98 (prologue `addiu sp,sp,-16`), its `jr ra` at 0x3B7DCC; two callers: `jal` at 0x3B7BE4
  (track lights) and 0x3B85C0 (car lights);
- car-light flare draw call `jal 0x498198` at 0x3B8BA0; flare draw routine 0x498198, halo decision near 0x49822C;
- sun flare draw call `jal 0x4985F8` at 0x3B89E0;
- zero padding at 0x495580 and 0x495C40..0x495CFF (room for helper code).
Why it should help George V (hypothesis to measure): with Performance mode (no readbacks) the owner measured
59.8 VBlanks/s but 9% late frames, the dips to about 55 FPS in the street-lamp section. Fewer and simpler flares mean
less EE work to queue them, less VU1 work (shorter VU1 drain before the per-frame readback) and fewer GS sprites.
What Performance mode would change (fixed values, the patch author's tested set; no player tuning in rc6):
1. No halo: each flare draws its glow only (glow size unchanged).
2. Distance cull: track lights beyond 60 units and car lights beyond 40 units are not queued.
3. Light caps: at most 12 track lights and 12 car lights queued per frame (with the distance cull, so no flicker).
4. Far car-light thinning: cars beyond 50 units draw about half their light pairs, always both lights of a pair.
Left out: sun flare at half size (tiny gain, visible change) and the "GT3 chase camera" (not performance; it changes
the camera feel).
Design:
- The game executable keeps one code path for both modes. A derived v2.00 ELF (the same mechanism as the Spec II
  boot patches, `scripts/spec2_patches.py`) places our helper routines in the zero padding and redirects the four
  sites above to them. Each helper first reads one guest word, the flare-limits switch, which the host writes at boot
  from Performance mode (new env var from the launcher, for example `GT4_FLARE_LIMITS=1`); with the switch at 0 the
  helper runs exactly the original instruction, so Performance mode off renders as today.
- Direct `jal` calls are compiled as direct C++ calls (only virtual calls pass through `ps2::dispatch`), so a host
  hook cannot catch them; the redirect has to be in the ELF before translation. Confirm this in step 1.
- Game logic never changes: the flares are presentation only. Confirm that nothing the game reads back for gameplay
  (physics, AI, timing, saves, replays) depends on the flare queues or the halo (step 1, with gt4-decomp names).
- Spec II uses different code: these addresses do not apply. Spec II gets the same feature later from its own
  disassembly (a follow-up step), not in rc6.
Steps:
1. Study (read-only): map the visibility test, both queue fills, the flare draw and halo decision, and the car-lamp
   object field used for pair thinning, with gt4-decomp names; list every reader of the queues; confirm the padding is
   never written at run time; confirm whether the per-frame 2 KB readback is the flare occlusion test (shared with
   TASK-116 step 2).
2. Derived ELF: our helpers and redirects, a pinned patch table, a verify mode (like the Spec II patch tool), Python
   tests for every word.
3. Host switch: the guest word written once at boot from `GT4_FLARE_LIMITS`; CTest fixture; the launcher sends it
   when Performance mode is on (GT4 v2.00 only).
4. Identity: with the switch off, the headless frame comparison matches today's v2.00 build (Nordschleife TT and a
   night scene); with it on, no new halts or interpreter fallbacks.
5. New v2.00 game build (with TASK-116 step 1), new `docs/inputs` pins, launcher manifest.
6. Owner test: George V Paris night race and another night track, F7/F8 with Performance mode on and off; visual check
   (far lights appearing, flicker, car lights).
Acceptance: Performance mode on holds 60 FPS on George V Paris (late frames clearly below 9%, F8 report), no visible
flicker; Performance mode off is unchanged (identity checks); automated tests for the patch table and the switch.

#### rc6 plan after the step 1 studies (2026-10-24; reports in the main agent's scratchpad, key words re-checked)
Study facts (C = confirmed in the ELF or code, H = high confidence):
- Track queue fill 0x3B7B70 (count flare+0x1848, limit `slti` 0x3B7BA8), car queue fill 0x3B8530 (count +0xA34,
  limit `slti` 0x3B8568 = 0x28620064); flare object at RacePS2Base+0xDA0 (C).
- Visibility test 0x3B7C98 already takes a per-type maximum view depth in f13 (default 1000.0): the distance cull is
  only a smaller f13 (C). Only two callers, 0x3B7BE4 (track) and 0x3B85C0 (car) (C).
- Flare draw 0x498198, called only from 0x3B8BA0 and 0x3B8CE8; the halo flag s5 is set at 0x49823C (0x24150001) when
  f18 > 0, f19 > 0 and depth > 1.0; zeroing f19 before the call removes only the halo (C).
- **The per-frame 2 KB readback is the flare occlusion test** (H, matches docs/GS-READBACK-STUDY.md): each queued
  light renders a small depth-tested sample reduced to one pixel of a 64x8 table, read back to flare+0x104 and fed
  to VU1 as the glow brightness; the EE never reads it. Culling cuts the sampling, VU1 and GS work but keeps the
  readback while any light is queued.
- Free code space: zero run 0x495574..0x495FFF (2,700 bytes, unreferenced, ends at the function at 0x496000) (C/H).
- Direct `jal` is compiled as a direct C++ call; changed ELF words are translated with no seed change; new entries in
  the hole renumber `unit_00E5` and regenerate its callers plus 00A4, 00A5, 00AD, 00AE (C). The aspect derived-ELF
  tool (`aspect_elf_patch.py`) needs a "redirect + code block" row type.
rc6 scope (fixed values, no player tuning):
1. TASK-116 step 1 (no VU1 drain on the flare readback in Performance mode), native only, no new ELF:
   - the game reads HWDownloadMode from the INI named by `GT4_GS_CONFIG` (one source of truth, no launcher change);
     A/B kill switch `GT4_GS_READBACK_LOCAL=0` (added to `GS_STREAM_NAMES` in external_runtime_probe.py);
   - only the 128-qword (64x8) flare readback in mode 2 is answered locally (with the previous flare reply, see
     step 3); car icons, loading picture,
     thumbnail and photo readbacks keep today's drain and round trip;
   - the READ_FIFO record is still posted in journal order without waiting (new journal entry kind, no-wait IPC post),
     so the GS stream hash is unchanged; `sync("download")` becomes lazy (the real drain runs only for non-flare reads);
   - CTest fixtures for the client, IPC, journal order and the switch; F8 text counter for local answers.
2. TASK-117 flare limits, derived v2.00 ELF: four `jal` redirects (0x3B7BE4, 0x3B85C0 to our track/car helpers;
   0x3B8BA0, 0x3B8CE8 to our halo helper) and about 350 bytes of our own helpers at 0x495580; the queue limits of 100
   stay. Parameters in a host-written guest block next to the HUD block (0x7F10 flags, 0x7F14 track depth 60,
   0x7F18 car depth 40, 0x7F20 / 0x7F24 caps 12), written at boot and every VBlank from `GT4_FLARE_LIMITS` when
   Performance mode is on; zero means every helper jumps straight to the original routine.
   Far car-light pair thinning is left out of rc6: its left/right key needs a lamp-table dump first, and with the
   car cull at 40 units a thinning start at 50 never acts.
3. Gate P0 before enabling either: `GT4_GS_READBACK_LOG=1` logs each readback (size, destination, hash of the reply)
   in Accurate and Performance mode on George V night, a day race, menus, photo and a replay, v2.00 and Spec II.
   Owner (2026-10-24): in Performance mode the street-light glows show normally on George V, so gt4-gs does NOT
   answer with zeros there and a local zero answer would hide the flares. Local answer = the previous flare reply:
   the no-wait READ_FIFO's reply is collected when it completes and copied into the next frame's flare readback
   (one frame late; the first frame of a race waits as today). P0 shows whether the reply is constant (then the
   result is byte-identical to rc5) or varies (then one frame late, judged by the owner on screen).
4. One new v2.00 game build (both changes) and a Spec II build (step 1 only), new `docs/inputs` pins, launcher
   manifest, rc6 notes in both languages, language gate, draft upload.
Identity: Performance mode off renders as rc5 (headless frame dumps, Nordschleife TT and a night race, GS stream hash
equal, MTVU and VU1 verify clean, hole hash unchanged at the first race frame and at exit, no "no translated code").
Owner test: George V Paris night F7/F8 with Performance mode on (target: mtvu download and readback wait near 0,
60 FPS without the dips), flare look on and off.



#### Scope change (owner, 2026-10-25 evening): cut every flare, the sun included
Performance Mode no longer reduces the flares: it removes them. No track lamps, no car lamps and no sun flare (weak PCs
lose FPS on the Nordschleife only when driving into the sun). With no light queued the game skips the 2 KiB sample, so
the per-frame readback, the VU1 drain, the round trip and the one-frame lag all disappear. Perceived on the owner's rc6
test build. Implementation: redirect the two lamp-queue fills (0x3B7BE4, 0x3B85C0) and the sun sample/draw (inside
0x3B8400 and `jal 0x4985F8` at 0x3B89E0) to helpers that return "not visible" with the switch on; the flare draw sites
need nothing because no entry is ever queued. The switch-off path must stay byte-identical to rc5: the derived ELF built
for the earlier "fewer flares" variant still diverges with the switch off, so the bisect below comes first. Full spec:
ai-memory `decisions/rc6-performance-mode-cuts-flares`.
#### rc6 delivery (2026-10-25): built, not shipped
Derived v2.00 ELF `SCUS_973.28.flare.elf` (sha256 `6c156b5f…`, 6,123,004 bytes) built by
`scripts/flare_limits_elf_patch.py produce` and pinned by that script (`check`, `verify`, `record-library`): the four
`jal` redirects (0x3B7BE4 track fill and 0x3B85C0 car fill → 0x4955A0 / 0x495600; 0x3B8BA0 and 0x3B8CE8 halo →
0x495580) plus our helpers in the zero padding, each ending in `j <original routine>` so `ra` (call + 8) is kept and
a zero flags word runs the original instruction in effect. Only caller-saved registers are touched (t0-t2, $f0, $f19).
The generated code calls the helpers from units 00AD (track fill) and 00AE (car fill and both halo sites), with the
delay slot unchanged; unit_00E5 holds the three helper entries (91-93) and unit_00E6 the flare routine. The accepted
seed set is unchanged (translator entries 138,815 against 138,813 for the original: the three new entries). Eight
units differ from the slice208 library: 008E, 008F, 00A6, 00A7 (the TASK-080 rows and the id renumbering), 00AD,
00AE, 00E5, 00E6. Library pin `docs/inputs/slice299-ee-libraries.json`, profile `flare` (CMake `GT4_EE_PROFILE=flare`).
Host: `native/flare_limits.*` writes the six-word block at 0x7F10 (flags 7, track depth 60, car depth 40, caps 12/12)
at every guest VBlank when `GT4_FLARE_LIMITS=1`, a zero block otherwise; CTest `native_flare_limits`. Launcher:
`launch.rs` sends `GT4_FLARE_LIMITS=1` for GT4 v2.00 (its ELF name) with Performance Mode on, never for Spec II, and
the direct start reads `HWDownloadMode = 2` from the config `gt4-gs.ini`; `launch_contract` and the standalone
fixture cover both. Tests: `tests/python/test_flare_limits_elf_patch.py` pins every helper word, the redirects, the
block offsets against `native/flare_limits.hpp`, the old words against the pinned ELF and the produced derivative.



#### Final implementation (2026-10-25, in rc6): the host answers the flare table as zeros
In Performance Mode the gs_stream client already answers the 128-qword (64x8) flare readback from the VU1 journal.
It now answers **zeros**: the VU1 scales every lamp, every headlight and the sun by its own pixel of that table
(`docs/GS-READBACK-STUDY.md` 1.1), so an all-zero table draws no flare, halo or sun glare. The READ_FIFO is still
queued, so the journal keeps deferring `sync("download")` exactly as in TASK-116 step 1; only the bytes the guest
receives change. Switch: `GT4_GS_HIDE_FLARES` (unset = on with the local answer, `0` = the previous reply). No guest
code and no launcher wiring, so Performance Mode off stays byte-identical to rc5 by construction; the sun needs no
separate path. The one-frame occlusion lag also disappears, because no flare is drawn at all.

Measured headless (night, 12,000 frames, 1x, `HWDownloadMode=2`, `GT4_GS_READBACK_LOG=1`): with hiding, 3,763 of 3,763
tables answered as zeros (probe `nz=0..0`), no readback wait, `mtvu download` 0.000 ms and the EE critical path
unchanged (5.87-6.52 ms against 6.00-6.44 ms with the previous reply); every dump before the race is byte-identical,
and the race dumps differ only on the lights and their glows. CTest: new `pcsx2-hidden` scenario; `pcsx2-local` now
pins `GT4_GS_HIDE_FLARES=0`.

Why the guest patch was dropped: the emulated EE clock is counted in instructions (`system.cpp` `now()` =
`g_base + (g_given - budget)`; VBlank and alarms are scheduled at guest cycles), so any runtime switch that adds
instructions moves where the VBlank lands in the guest frame. The derived ELF therefore diverged from rc5 on the
first frame that drew flares even with every helper taking the original path, and no placement or entry-id fix can
remove that. The derived ELF (`scripts/flare_limits_elf_patch.py`, the `flare` profile, `native/flare_limits.*`)
stays on the branch as a documented dead end; only a guest patch removes the GS-side 2 KiB download, which belongs to
TASK-116 step 2 (async download in gt4-gs, 1.3.1).

Owner test (2026-10-25, night): **visual acceptance passed** in v2.00 and Spec II, night and day: flares gone, no
other visual difference, and the glows stopped drawing over the virtual rear-view mirror (a known artefact in GT4
and in PCSX2 GSdx). Performance felt clearly better even with the machine busy compiling; **F8 metrics pending** on
a free machine. The build he tested stays as delivered until that run, so TASK-118 presets do not mix with its
numbers. Split screen keeps the headlight glow (constant path, see the study); the sun is hidden there too.

#### In progress (2026-10-26, owner switching agents): the four laptop fixes
The owner tested rc6 on an i5-7200U / Intel HD 620 and sent the sessions (`Z:` share). Four defects, each measured:

1. **The launcher cost ~20% CPU in the background** (six full-screen 72 s CSS animations kept running after Play;
   hiding the window does not stop WebView2). F8 proof: the game EE CPU itself went from 6-12 ms to 19.61/19.16 ms
   on that run. Fix: pause them while hidden/blurred/in game (body.is-idle) and keep WebView2 GPU acceleration on.
2. **A normal close was reported as a crash**: the launcher read `game.log` in the same instant the game wrote its last
   line (three sessions: `Crashed { reason: None, exit_code: Some(0) }` with a normal `[ee] stopped ... window closed`).
   Fix: wait for the shutdown line (25 ms poll, 1.5 s cap) and treat exit 0 without one as a normal close.
3. **Vulkan loses the device on the HD 620** at the showroom (issue #89; it reports Vulkan 1.3, so the automatic API
   rule picks it). Direct3D 11 runs fine; Direct3D 12 is clearly slower there. Fix: remember `graphics_api = d3d11`
   after a Vulkan device loss and say so.
4. **Render scale jumped 1x -> 4x in one notch** on the integrated GPU (100% = 1x by issue #89, then k x percent).
   Fix: additive steps, one multiplier per 25% from the 100% plan (integrated 1/2/3/4/5x), clamped 1..12.

Code state when the session ended (all in the worktree, `cargo test -p gt4l-core` green, `cargo check -p gt4l-tauri`
green with `GT4_SDL3_ROOT` set):
- `display.rs`: additive mapping + tests; `exit.rs`: `is_settled` + relaxed `classify` + test;
- `commands.rs`: `read_settled_log`, `PlayInputs.api`, the Vulkan->D3D11 memory; locale key
  `rust.game.gpu_lost_switched`; `gs-settings.toml`: `blending` hidden; `launch.rs`: preset writes blending
  Minimum (Performance Mode on) or High (off).
- **Still missing:** the UI pause (`app.css` body.is-idle + `app.js` game-state/visibilitychange/blur), and the
  portable launcher rebuild + owner/laptop copies. Full checklist: ai-memory `procedures/task-118-launcher-fixes`.

Performance Mode preset (supersedes the earlier list): `HWDownloadMode 2` + `accurate_blending_unit Minimum` only.
Trilinear, dithering, preloading, filtering and mipmapping stay at their accurate defaults: measured on the laptop,
turning them off changed nothing outside the noise and cost picture quality. Details in ai-memory
`gotchas/weak-laptop-ee-bound` and `decisions/rc6-laptop-launcher-fixes`.

### TASK‑118 — Graphics settings simplification (owner, 2026-10-25, 1.3.0-rc6)
Owner decision, recorded directly by the implementation session (no orchestrator round trip). The Advanced page of
Settings > Graphics is too technical and Performance Mode is only a readback switch today.

What changes:
1. **Performance Mode becomes a preset.** On: `HWDownloadMode` 2 (as today), `accurate_blending_unit` Basic,
   `TriFilter` off, `dithering_ps2` off, `TexturePreloadingLevel` Partial. Off: exactly today's values (blending
   High, trilinear automatic, dithering unscaled, preloading full). Blending accuracy leaves the Advanced page.
2. **Fixed values, rows removed:** deinterlace automatic, present filter smooth, integer scaling off, interlace offset
   off, anti-blur on, offsets/overscan off, TV shader none, anisotropic off, accurate alpha test off, AA1 off, ROV off,
   shade boost and its four sliders off, disable shader cache off, texture filtering PS2, mipmapping on.
3. **GS async leaves the launcher** and is always on (it was a debugging A/B switch and is the default).
4. **RC Settings area** (only in release-candidate builds, `gt4l_core::build_kind`): Release performance report
   (`performance_report`) and Verificar código VU compilado (`check_vu_code`). Final builds do not ship them.
5. **Kept for everyone, default off:** the PCSX2 performance overlay (OSD) and the "compiling shaders" notice.
6. **Removing a row never stops writing the key**: each removed row keeps a fixed value in `gt4-gs.ini`, otherwise an
   existing install keeps a stale value.

Rules: gameplay never changes (an icon, loading picture or flare may be lost; physics, timing, AI, saves and replays
may not). Every preset key needs a headless A/B on the F8 harness before it ships, and the owner tests in game.
6. **No "Automatic" choice for API or card** (owner, 2026-10-25): the launcher resolves both on the first start and
   stores the result, so the UI shows "Vulkan" and the player's card name directly; the player can change either one,
   and a card that disappears falls back to the rule's pick for that run and updates the stored value (never silently
   back to "Automatic"). Needs a settings migration that materialises a stored "auto".
7. **Render scale becomes one slider**: 100% by default, 50%..200%, no "6x" text and no resolution text beside it. It
   snaps to the integer GSdx multipliers 1x..12x and stores the multiplier (the ini keeps integers); the percentage is
   relative to the automatic plan, which stays the 100% baseline and must keep working from the monitor resolution.
   **Hard cap at 8K**: 12x is 7680x6144, so an 8K output tops out at 100% (12x).
8. **AA1 stays disabled by default** (owner, 2026-10-25): it does not join the anti-aliasing row and does not enter the
   Performance Mode preset unless a headless A/B plus the owner's eyes show a real GT4 gain.
Acceptance: the Advanced page holds only rows a player can reason about; Performance Mode on/off is the single
switch; no lost option changes gameplay; tests for the preset layer, the fixed rows and the RC-only gate.

### TASK-116 — George V Paris performance
Recorded (owner, 2026-10-24) on 1.3.0-rc5, Spec II (Arcade unlocks every track), night race on George V Paris,
GSdx Vulkan 6x, Performance Mode off, owner PC (Ryzen 7 5700X, RX 9060 XT). The owner calls it the heaviest and least
optimised track of the game. Measured on screen (RivaTuner): 43-52 FPS in the race (60 on the track-select screen),
frametime spikes to about 23 ms; GPU only 32-42% busy at about 2.5 GHz, CPU about 33% total, so one thread is the limit,
not the GPU. Hypotheses (unconfirmed): many street lamps with lens flares cause GS readbacks that make the game wait
for the GS process each frame (the C.S. Route 5 night capture already had 2,928 readbacks), or the EE / VU1 / GS-stream
thread is saturated by the scene. Next: the owner records an F7/F8 interval on this track (Graphics > Advanced >
Performance report), the F8 report lands in that session's `game.log`; compare with Nordschleife TT and a day race.
Applies to GT4 v2.00 too if the track behaves the same there.
Diagnosis (two owner F8 intervals, 99 s each, 2026-10-24; details in ai-memory notes/george-v-performance):
- Accurate readbacks: 55.3 VBlanks/s (92% game speed, 66 audio underruns); EE critical path 17.73 ms of 16.68;
  about one 2 KB readback per frame, and before it the EE drains the VU1 worker (5.65 ms/frame) and then waits for
  gt4-gs to render and download (5.28 ms/frame): a serial chain, so the GPU idles at 30-40%.
- No readbacks (Performance mode): 59.8 VBlanks/s, 99.7% speed, 9% late frames (the dips to ~55 FPS); the game still
  asks for the readback every frame and the EE still drains the VU1 worker first (5.52 ms/frame) although gt4-gs answers
  with no data (1.23 ms).
Plan for 1.3.1 (owner, 2026-10-24):
1. No VU1 drain when the readback returns nothing: with HWDownloadMode NoReadbacks the game exe answers the readback at
   once without waiting for the VU1 worker (stream order unchanged). Native change in the game's MTVU / gs_stream client
   (our MIT code): new v2.00 and Spec II builds, new pins, v2.00 identity checks; expected about 5.5 ms/frame less on
   George V, enough to hold 60 FPS with Performance mode.
2. Unsynchronized readbacks: gt4-gs 1.2.2 accepts HWDownloadMode Unsynchronized (fork allow-list and SHA256SUMS; ship
   with the OpenGL entry of TASK-107); the readback returns the previous frame's data without waiting, so the flares stay
   and react one frame late, and the game skips the VU1 drain as in step 1. Before shipping, confirm the 2 KB readback is
   the flare visibility test (game logic must never change) and offer it as a third Readbacks choice after an owner test.


#### rc6 delivery of step 1 (2026-10-25)
Performance mode answers only the 128-qword flare readback locally with the previous reply (one frame old, never a
zero table): `sync("download")` closes the VIF1 batch and returns (`download_deferred`), the client queues the
READ_FIFO behind the journal (`EntryKind::Readback`, `Session::post_read_fifo_nowait`) so the record keeps its stream
position, and the reply is collected one frame later by exact request id. Every other readback (car icons, loading
picture, thumbnail, photo) drains on demand (`mtvu::drain_download`) and waits, as does the first flare readback of a
scene. `GT4_GS_READBACK_LOCAL=0` restores the old behaviour; `GT4_GS_READBACK_LOG=1` logs every reply (P0 probe).
Night race headless 1x: blocking drains 5,026 → 29, readback wait total 4,596.6 / 2,329.1 → 46.1 ms, 4,997 of
4,998 flare replies from the cache, record counts equal to the pre-rc6 Performance-mode run; the Accurate run still
hashes `c091716e3cb40d79`. The owner F8 decides the gain: part of the old 5.5 ms "download" was the EE's own ring
writing (now `EE gs_drain` 2.2-4.2 ms/frame), so the net saving is about 2-3.5 ms/frame in the race stretch.

### TASK-100 — Spec II support (release 1.3.0)
Planned (owner, 2026-10-22) for release 1.3.0: Spec II ships before the wheels and before CoreGS 2.0 (owner, 2026-10-23), on GSdx. Scope as decision 0072:
the SCUS-97436 Public Beta lineage image with its own target executable, not a content merge into v2.00. Identity
study: docs/SPEC2-STUDY.md. Expected to be most of the work at the start; once one Spec II build runs, keeping it
working should be much cheaper. Work to map first:
- How Spec II's plugin injector puts its code and scripts into the game at start (where the injected MIPS code and
  data come from, when it runs, what it patches). Everything the EE runs must be compiled ahead of time (no JIT), so
  the injected code has to be captured and translated like the rest of the ELF, or its effects reproduced natively.
- A second EE pipeline: `SCUS_974.36` (no `CORE.GT4`) through the donor translator into its own pinned library,
  with its own manifests, coverage runs and "no translated code" hunt.
- Our own game-side hooks are bound to v2.00 addresses and must be remapped (decomp names where gt4-decomp helps):
  the 16:9 hold (TASK-079), the aspect/race camera and HUD hooks (TASK-080), the 480p race display (GT4_FORCE_480P),
  the license demo guard, the memory card store, the MTVU/VU1 AOT (Spec II VU1 microcode image and its keys), VU0
  AOT, the IOP IRX set, and every pinned constant in docs/inputs.
- Adhoc v7 scripts and the natives Spec II registers (menus, GT mode, `projects/%s/%s`), the `GT4_PREMIUM_US2560`
  spec database, `/sound_gt` and `carsound` paths: data the game reads, so they need no translation, but every native
  the scripts call must exist in the translated executable.
- Launcher: a second game target (disc identity check, setup, saves kept apart from v2.00), chosen per disc image.
Plan (2026-10-22): [docs/SPEC2-PLAN.md](docs/SPEC2-PLAN.md), subtasks S1 patch set and injector map, S2 derived
ELF, S3 target-aware host hooks (v2.00 byte-identical), S4 second-target build, S5 VU0/VU1, S6 IOP network modules,
S7 first boot, S8 re-targeted hooks, S9 launcher and installer. Worktree `C:\Antigravity\gt4-wt-100-spec2`; the
owner runs the long parts in OpenCode. The unmodified Public Beta ISO is wanted for S1 (exact diff) and S7a.
Progress (2026-10-22, S1 done): the owner supplied the Public Beta ISO; the community `.xdelta` decodes it to exactly
the owner's Spec II ISO (byte identity), so the beta is the confirmed base. The beta's encrypted `CORE.GT4` was
recovered (Salsa20/20, key `PolyphonyDigital`, CRC-32C trailer) and rebuilt into `private/slice271-spec2-beta-elf`
(ELF `2e5621ed…`). The diff against Spec II's `SCUS_974.36` gives the complete static patch set: **one word**
(`0x001001E8`, the trampoline into the plugin) plus the `.plugin` segment at `0x0068A480` (0x2CCC bytes) over
zero-filled data; every other byte is identical. The injector is mapped by executing its init on an EE-RAM image:
35 word stores (18 text, 17 data), unconditional (no variants; identical under stack/return-value perturbation).
Report [docs/SPEC2-PATCHES.md](docs/SPEC2-PATCHES.md), pins `docs/inputs/spec2-patches.json` and
`docs/inputs/spec2-beta.json`, tools `scripts/spec2_patches.py` and `scripts/spec2_beta_elf.py`,
`tests/python/test_spec2.py`. Open: S3 build/identity, then S5/S6, S2/S4.
Progress (2026-10-22, S3 done): `native/game_target.hpp` carries every guest address of the host hooks per target
(v2.00 keeps the release values; an absent address disables the hook and logs once; `GT4_TARGET=v200|spec2`). The
capsule overlay text is unchanged. CTest 42/42 (new `native_game_target`); Python 653 OK, 42 skipped and the known
release-pin distribution failure that any rebuilt executable has. Headless identity against the 1.2.3 build (slice268)
is exact in menus, day and night, dumps and GS stream hashes, on the final code state. Open: the new executable's
release pin set (release decision), S8 fills the Spec II table.
Progress (2026-10-22, S5/S6 mapped): VU1 decrypts to the v2.00 image (`ae6b6cb3…`), the AOT table carries over; VU0
differs (0x920 vs 0xA80, 1,050 bytes) and runs on the counted interpreter fallback. IOP: 35 files pinned with policy
(`inputs/spec2-iop.json`), 6 new, inherited from the beta lineage. Open: S7b for both (VU0 profile, offline hangs).
Progress (2026-10-22, S2 done): `scripts/spec2_elf_patch.py` applies the 35 injector stores to `SCUS_974.36` from the
ISO and writes `private/slice272-spec2-elf` (`10a794df…`, original `db08421b…`), with `--revert` reproducing the
original exactly. Pins `docs/inputs/spec2-iso.json` and `docs/inputs/spec2-elf.json`; the trampoline at 0x1001E8 stays
(documented: the init also calls the beta's boot initialiser and one more game function). Tests 21 in
`tests/python/test_spec2.py`. Open: S4 (plain-ELF translation, own library and executable) and S7b.
Progress (2026-10-22, S4 translation): `scripts/spec2_ee_generate.py` runs the pinned translator on the derived ELF:
346 units, 348 files, inventory `13354c95…`; 1,285,650 instructions, 17,080 functions, 148,756 entries, 2 rejected
seeds. Pin `docs/inputs/spec2-ee-generated.json`; output stays in scratch. Open: the Spec II library and executable
(own CMake profile) and S7b.
Progress (2026-10-22, S4 build done): profile `spec2` in CMake (`-DGT4_TARGET=spec2 -DGT4_EE_PROFILE=spec2`, output
`gt4vulkan-spec2.exe`); library `private/slice274-spec2-ee-producer/game-build/ps2game.lib` 77,369,746 bytes
(`5e483768…`), pin `docs/inputs/spec2-ee-libraries.json`. v2.00 untouched.
Progress (2026-10-22, S7b BLOCKED before the menus): the derived image boots headless with **no "no translated code"**
(600 and 4,200 frames). Two findings fixed on the way: the plugin PT_LOAD sits inside the data PT_LOAD, so the
translator read zeros for it (the derived ELF now splits the data segment around the plugin, image still identical);
and the plugin's own pointer tables are inside its executable segment, so 12 entry seeds were added. The game then
draws nothing (8 primitives against v2.00's 2,524 at the same frame), all EE threads wait, the IOP is idle. The
original unpatched image behaves identically, so our patches are not the cause. Next: semaphore trace, the DNAS310
IOP-reboot path, and an IOP module A/B.
Progress (2026-10-23, S7b unblocked: menus and a race): the stall was the FILEIO protocol generation. The Spec II disc
reboots the IOP with `DNAS310.IMG` instead of `IOPRP300.IMG`; the donor selected its asynchronous FILEIO server (the
one that answers `devctl`, method 23, with the SIF packet `0x80000011`) only for a path containing `IOPRP`, so the
game's fileio thread waited forever on its completion semaphore. The image needs no loading: DNAS310.IMG is the same
romdir as IOPRP300.IMG, with the same 16 Sony system modules in the same order, all served as HLE (pin
`docs/inputs/spec2-dnas310.json`, section in docs/IOP-MAP.md). Fix: capsule `private/slice276-spec2-iop-runtime`
(`scripts/prepare_perf_runtime.py --spec2-iop`, `scripts/spec2_iop_overlay.inc` + `native/spec2_iop.hpp`); v2.00
keeps its capsule. Headless: 4,200 menu frames (1.8 M primitives), the main menu, Arcade Mode, track and car
selection, and a 21,000-frame race at Costa di Amalfi (169.7 M primitives, HUD, lap timer), no "no translated code".
CTest 43/43 in the Spec II build (new `native_spec2_iop`); Python 668 tests: all pass with the Spec II build (51
skipped), only the expected release-pin failure with the v2.00 build. v2.00 identity against the 1.2.3 build: menus,
day and night dump and GS stream hashes all equal. Open: S8 (Spec II hook addresses), S9 (launcher, installer,
credits); the race used a scripted accelerator press, and GT mode still needs game data on the card.
Progress (2026-10-23, S7c PASS on GSdx): the harness gained target overrides (--exe/--elf/--iso/--press/--textures/--gs-sha256; defaults are the v2.00 gate values and the menus/day/night identity still equals slice268). Spec II runs on the shipping PCSX2 GS process: menus 4,200 frames, Costa di Amalfi race 21,000 frames (12 readbacks), night race at C.S. Route 5 (2,928 readbacks), 3x upscale, and the owner's HD HUD texture pack (with [Folders] Textures + LoadTextureReplacements, needs gt4-gs 1.2.1; the repo pin is 1.0.1). The GT4 display crop is a fixed gt4-gs constant ({0,0,1,2}) and logs for both games; gt4-gs has no SCUS-97436 GameDB entry and the game still announces SCUS-97328 (recorded for S8/S9).
Progress (2026-10-23, S8 done for the main hooks): `native/game_target.hpp` kSpec2Hooks now carries the Spec II option chain (owner 0x664EDC, option +0x680, flag +0x14, record +0x24 44 bytes at width +0xC/height +0x10, presets 0x664E20/50/80/B0, published wide word 0x6655A0, race display index +0x1718), RaceBase::initMonitor 0x26BDB0 with camera return 0x26BE60, RaceDisplay::render 0x1C5040 and the HUD matrix at +0x70. Found from the v2.00 code shapes in the derived ELF and confirmed with a runtime RAM dump. New fixture native_spec2_hooks asserts every value and the write rules in synthetic RAM; native_game_target asserts the filled table. Headless GSdx race at 21:9 shows the camera matrix scaled, the first RaceDisplay::render, the HUD canvas (left -240 right 880) and the fill policy; the Spec II build now uses the slice262 license guard capsule for the aspect overlay. The 16:9 hold finds the settled owner 0x9BD690 but writes nothing because the game itself boots 16:9; the 480p hold runs in window.cpp's VBlank, so headless runs only cover it through the fixture. Not mapped (rows 0, hook logs once): display request struct (photo mode), license demo guard (feature exists, loaders unmapped), shape observer. CTest 44/44 in the Spec II build; Python 671 tests clean on the Spec II build (51 skipped) and only the expected release-pin failure on v2.00; v2.00 identity against slice268 exact (menus/day/night dumps and stream hashes).
Progress (2026-10-23, S8b license demo guard mapped): kSpec2Hooks now carries the Spec II license values: loadDemoInputs 0x2012C8 (RaceSolitaire vtable 0x6B3130 +0x510, the same virtual v2.00 has at +0x4D8), loader entries 0x10CA18/0x10CBA8 with return labels 0x10CB54/0x10CCE4 (each loader calls the license/%s helper 0x1A8BE8 and makes one vtable call), Manager::load 0x207AA0, CRC-32 table 0x731DB0, Manager add 0x1A24 and body pointer shift 0x80. The cipher is the same (0x459E90, FT at 0x667048, T8 at 0x731C98); the Spec II volume's 256 replay/license demos are the retail files (171 shared with v2.00, 170 bodies byte-identical) and CRC-confirmed plaintexts keep the single-Recorder stream at body+0x1AA4, so the layout and the CRC rule are unchanged and the code's 0x1A24 add applies to a pointer 0x80 into the body (High confidence; not observed at runtime). Guard: two table fields (license_manager_add, license_body_shift), the overlay evaluates the header body and writes n=0xFFFFFFFF at a1+manager_add, static assert add+shift == 0x1AA4; fresh capsule private/slice277-license-guard-runtime (slice262 profile) and pin docs/inputs/slice277-license-guard-runtime.json, CMake accepts slice27x; v2.00 keeps slice262. Oracle on the Spec II set with our FPU (chop/chop): 0/256 CRC passes, 114 sane layouts, 142 demos guarded (other div models 130-152). Headless license test not reachable (GT mode needs game data; the seed cards have none), owner run remains the gate. CTest 44/44 and Python 675 OK (51 skipped) on the Spec II build, including the overlay harness compiled for GT4_TARGET_SPEC2; v2.00 identity against slice268 exact in menus/day/night (dumps and stream hashes).
Progress (2026-10-23, S8c Options-menu halt fixed): the owner test halted twice opening Options at 0x3CA690 (ra 0x50D1B0). The callback is built by the Options page (lui s0,0x003D at 0x3CC8EC + addiu s0,s0,-22896 53 instructions later) and stored in the descriptor; the translator's luipair window is 3 instructions and no dataword points at it. New scripts/spec2_entry_seeds.py seeds the missing classes (wide luipair window 64, stack-frame fallthrough, executable-segment literal code entries): 1,299 seed delta, regenerated 346 units, 1,332,727 instructions, 18,597 functions, 153,474 entries (inventory 2d8c92a4.), new library private/slice278-spec2-ee-producer/game-build/ps2game.lib (80,229,124 bytes, 9a94c3ed.), pins spec2-ee-libraries/generated updated, CMake accepts only slice278 for the spec2 profile. Sweep after the reseed: no real missing entries in the luipair/fallthrough/dataword/executable classes (only plugin strings/zeros). Headless: the old exe reproduces the halt after 11,159 frames; the new exe O:\gt4recomp-scratch\spec2-s7d-build\build\gt4vulkan-spec2.exe (104,475,136 bytes, 301bbb4a.) opens Options plus every submenu (Race, Replay, Screen 16:9, Picture Quality, Sound, Arcade Mode, LAN Connection Settings, Photo Lab), Replay Theater, Save Game, Credits, GT Mode and the Arcade menu with no halt; the harness gained --card. A license demo was not reached headless (the mod's Spec II Settings dialog blocks the GT map), so the [license-guard] confirmation stays with the owner. CTest 44/44, Python 678 OK (51 skipped), v2.00 identity against slice268 unchanged.
Progress (2026-10-23, 1.3.0 integration candidate, not published): merged origin/main db5c559 and origin/task/100-spec2 9206af5 (S8c) into task/101-multi-game (63c110e, b8a6b5f); resolved TASKS/BUILD/journal conflicts keeping the implemented TASK-102/103 rows and CRLF. Version 1.3.0 in launcher/Cargo.toml, Cargo.lock workspace crates, tauri.conf.json and the installer default. Release notes written: docs/release-notes/1.3.0.md, launcher 1.3.0 en-US and pt-BR, with the Spec II credit list (owner decision: no permission needed) and "GT4Recomp ships no Spec II files". New game builds in fresh slices: v2.00 private/slice279-build/build/gt4vulkan.exe (97,377,792; 9dccb5a4.), pin slice279-pcsx2.json; Spec II private/slice280-build/build/gt4vulkan-spec2.exe (104,612,352; 174e7adf.), pin slice280-spec2.json (slice276 IOP + slice277 guard + slice278 library); gt4-gs 1.2.1 package slice281; PERF_PROFILES pcsx2 -> slice279 (slice268 kept as pcsx2-slice268), SPEC2_RELEASE -> slice280. GAME_DIR slice282-dist-130 (both exes); launcher release 69530dfa. and portable a1a0ed0d.; installer GT4Recomp-Setup-1.3.0.exe 51,373,344 / d1569b35. (7-Zip extraction: 24 files match their sources, 0 differences); portable zip 83,032,500 / bfa3a08a. with SHA256SUMS. Checks: CTest 45/45 both builds, Python 694 OK (42/56 skipped), cargo workspace clean, focus en-US 151/151 + pt-BR 158/158 + nav 150/150, Rust Spec II derivation byte-identical to the script, v2.00 identity vs slice268 exact in menus/day/night. Flaky migration test cause: a scanner/indexer can hold a freshly written file, so the rename failed transiently; the real rename now retries 5 times (20-100 ms apart) and the policy has a test. Not done: owner tests, no PR/merge/tag/release.
S9 recorded (2026-10-23, not started): Spec II ships before CoreGS 2.0 on GSdx with upscaling and texture packs; the launcher keeps ONE shared settings set and only the game executable, the disc and the save folder differ per game; texture packs use the same Textures folder and settings, no new option; the planned release notes are two files (en-US and pt-BR) per AGENTS.md. GT mode needs game data on the card: the first boot creates it (decline -> 'GT mode is not playable'); plan the fresh Spec II card in normal play and keep its save separate from v2.00's. Open: launcher disc identity/prepared layout without CORE.GT4, second exe name, chooser, the Logo (credits are decided: no permission needed, credit by name) and the GS handshake serial decision (still SCUS-97328; gt4-gs has no SCUS-97436 GameDB entry).

### TASK-099 — Patch notes panel on the launcher home screen
Planned (owner, 2026-10-22) for release 1.2.4. The home screen keeps the logo, Play and Settings on the left; a new
block on the right shows the patch notes of the installed version, rendered from Markdown, in a light, minimal
glassmorphism card (translucent background with a backdrop blur, thin border, soft shadow) that matches the current
dark theme.

Plan:
- Source: the release notes the project already writes, `docs/release-notes/<version>.md` (the same text as the
  GitHub release body). The launcher build embeds the file of its own version (`include_str!` from the version in
  `Cargo.toml`, with a build check that the file exists), so it works offline, in the installer and the portable
  build (TASK-098), and can never be missing or edited by accident. A file beside the launcher is not needed.
- When an update is available, the panel can also show the new version's notes, read from the GitHub release body
  that the update check already downloads (`releases.rs`), with the update button next to them. Optional, after
  the basic panel.
- Rendering: a small Markdown subset (headings, paragraphs, bold/italic, inline code, lists, links, tables if
  cheap) converted to HTML in Rust with an MIT crate (for example pulldown-cmark), raw HTML disabled and the output
  escaped; links open in the default browser through the existing opener, never inside the web view. A link at the
  bottom: "All release notes on GitHub".
- UI (`ui/js/pages/home.js`, `ui/css/app.css`): the right-side card scrolls on its own; on narrow windows it moves
  under the buttons or hides behind a "What's new" button; keyboard and gamepad focus can reach and scroll it; the
  WebView2 safe mode (no GPU) falls back to a plain translucent background without blur.
- Release notes style: start each file with a short "Highlights" list so the panel reads well; the long sections
  stay below.
- Tests: Rust tests for the Markdown conversion (escaping, links, no raw HTML) and the embedded version; the UI mock
  shows the panel with real notes; the owner checks the look.

### TASK-098 — Portable build (zip) beside the installer
**Done on branch `task/101-multi-game` (2026-10-23), waiting for the owner test; release 1.3.0** (moved from 1.2.4 with
TASK-097, on top of the multi-game layout of TASK-101). Planned by the owner on 2026-10-22. Two launcher builds from one source: the
installer build (as today) and a portable build shipped as a zip. The portable copy keeps everything in its own
folder, so it can be carried to another PC as one folder. No path is hardcoded: every folder is found from the
folder of `GT4Recomp.exe`.

Layout of the portable folder:
- `GT4Recomp.exe` (portable build), `SDL3.dll`, `game\` (game executable, shaders, runtimes), licences.
- `user\`: what the installer build keeps in `%APPDATA%\GT4Recomp`: `config\settings.toml`, `controls.toml`,
  `Saves`, `Replays`, `backups`. Shipped in the zip (empty).
- `data\`: as in the installer build: disc copy, prepared files, `gs\` (gt4-gs versions and caches), runtime card,
  sessions, logs, `Textures`.

Map of the work:
- Build kind: a Cargo feature (`portable`) gives a compile-time build kind; `build-release.ps1` builds both
  (`-Portable`); Settings > About shows "Portable".
- Paths (`gt4l-core/src/paths.rs`, `resolve_layout`): in the portable build `UserDirs` = `<exe dir>\user` (a new
  `UserRootSource::Portable`) and `DataDirs` = `<exe dir>\data`, with no read layers and no `%LOCALAPPDATA%` write
  fallback: a folder that cannot be written (for example a copy inside Program Files) gives a clear message to move
  the folder somewhere writable. `UserDirs::new` already builds the standard layout under one root.
- No registry and no system places in the portable build: no `InstallDir`/`PreviousDataDir` reads
  (`relocation.rs` `previous_data_for`), no 1.2.0 legacy migration from `%LOCALAPPDATA%` and Documents
  (`relocate.rs`; an optional "import saves from the installed GT4Recomp" button can come later), no
  `clean_old_install`, no Start-menu or desktop shortcuts, no uninstall entry.
- Self-update for the portable build (`updates.rs`, `releases.rs`): the launcher stream gets a second asset,
  `GT4Recomp-<version>-portable.zip`, listed in the release `SHA256SUMS`. The portable launcher downloads it, checks
  it, extracts it to `<exe dir>\.update-staging`, renames the running `GT4Recomp.exe` aside (Windows allows renaming
  a running exe), swaps the program files in (never `user\` or `data\`), restarts itself, and removes the old copy at
  the next start; a failure rolls back as the installer's swap does (TASK-091). No administrator prompt.
- gt4-gs: unchanged; it is downloaded into `data\gs` as today (portable already; check its caches stay in `data\gs`).
- Single instance: the lock is per folder, so two portable copies (or a portable and the installed one) on one PC do
  not block each other but one copy cannot start twice.
- WebView2: the portable zip cannot install it; the TASK-091 detection message (with the download link) covers it.
- Game executable (TASK-097): one game build for both. When it starts on its own it reads the settings from
  `<install>\user\config` if that folder exists, else from `%APPDATA%\GT4Recomp\config`; the installer never creates
  `user\`.
- Release: `scripts/make_distribution.py` or a new script makes the zip (with the empty `user\` and `data\`
  folders) from the same files as the installer; release notes and README explain both downloads.
- Tests: `cargo test --workspace` for both builds (`--features portable`); path resolution, no registry use, the
  portable self-update swap and rollback with an injected file system, the per-folder lock.

Result (slice 278, 2026-10-23): `cargo test --workspace` 431 passed in each build (the installer build and
`--features portable`), 0 failed. Python: `tests/python/test_make_portable_zip.py` (7 tests, fixtures only).
What was built, and the decisions behind it:
- `gt4l_core::build_kind` (`BUILD`, `BuildKind::label`): the Cargo feature `portable` of `gt4l-core`, forwarded by
  `gt4l-tauri`. Logic takes the kind as a value (`LayoutInputs::portable`, `relocation::prepare_as(portable)`), so both
  behaviours are tested in either build. Settings > About shows "Build: Portable" (`about_info.build`).
- Paths: `resolve_layout` with `portable` gives `UserDirs` = `<exe dir>\user` (`UserRootSource::Portable`) and `DataDirs` =
  `<exe dir>\data`, the per-game layout inside both, no read layers, no `%LOCALAPPDATA%` fallback: an unwritable folder
  gives "Move the GT4Recomp folder to a place you can write to" (a native message box, because no window exists yet).
  The test overrides (`GT4RECOMP_DATA_DIR`, `GT4RECOMP_USER_DIR`) still win.
- Nothing in the registry or system folders: `prepare_as(portable)` reads no `InstallDir`/`PreviousDataDir`, no Documents
  folder, runs no 1.2.0 or 1.2.x migration; `clean_old_install_as(portable)` does nothing; the launcher creates no
  shortcut and no uninstall entry (only the NSIS installer does). The WebView2 profile goes to `data\launcher-webview`
  (`appDirectoriesOverride`, Tauri 2.12) instead of `%LOCALAPPDATA%\io.github.moesuito.gt4recomp`. The only registry read
  left is the existing read-only WebView2 runtime detection (TASK-091), which the portable zip cannot replace.
- Self-update (`gt4l_core::portable_update`, injected `UpdateFs`): `GT4Recomp-<version>-portable.zip` + `SHA256SUMS` ->
  `.update-download\` (SHA-256 checked while downloading) -> extracted into `.update-staging\` (zip-slip, size and entry
  limits) -> each top-level item except `user\`, `data\` and the update's own folders is renamed into `.update-old\` and the
  staged item renamed in (the running `GT4Recomp.exe` is renamed aside, which Windows allows) -> the new exe starts with
  `--after-update` (waits for the old one to exit) and the old one exits. A failed rename undoes every step; a marker
  `.update-swapping` survives a crash and the next start restores `.update-old\`; a finished update's `.update-old\` is removed
  at the next start (retried for 20 s in the background while the old launcher exits). No administrator prompt.
- Single instance: `Local\GT4Recomp.Launcher.<key of the exe folder>` in the portable build, so two copies never block each
  other and one copy cannot start twice; the installer build keeps `Local\GT4Recomp.Launcher`. Known limit: a second start of
  the same folder brings forward the window titled "GT4Recomp", which may be another copy's window when both run.
- `scripts/make_portable_zip.py` makes the zip from the same files the installer stages (`GT4Recomp.exe`, `SDL3.dll`,
  `NOTICE.md`, `LICENSE.txt`, `game\`) plus empty `user\` and `data\` and a `PORTABLE.txt`, refuses game data in the game
  folder, is deterministic and appends the zip's line to `SHA256SUMS`. `launcher\tools\build-release.ps1 -Portable` builds
  the portable launcher into `<target dir>-portable`. No release zip was built (owner rule); the script is tested on fixtures.
- Not done on purpose: an "import saves from the installed GT4Recomp" button (later); the game executable reading its
  settings from `<install>\user\config` is part of TASK-097 below.

### TASK-097 — Self-sufficient game executable
**Done on branch `task/101-multi-game` (2026-10-23), waiting for the owner test; release 1.3.0** (moved from 1.2.4). Planned
by the owner on 2026-10-22, after the 1.2.3 hotfix. Principle: the game runs on its own with the
same defaults every time; the launcher is only an easier way to start it and to manage settings. Today a double click
on gt4vulkan.exe does not start the game: the launcher passes about 14 arguments and variables (ELF, disc, card,
window; GT4_RENDERER, GT4_GS_PROCESS(_SHA256), GT4_GS_CONFIG; GT4_FORCE_480P, GT4_FORCE_WIDESCREEN,
GT4_DISPLAY_ASPECT; GT4_FULLSCREEN, GT4_PRESENT_MODE, GT4_MONITOR, GT4_OUTPUT_SIZE, ...). Plan:
- Built-in defaults: GSdx (pcsx2 renderer), 480p race display, 16:9 hold with the automatic ultrawide correction,
  fullscreen, vsync. The variables stay as developer/test overrides; test harnesses and pins that assume today's
  defaults set them explicitly.
- Paths from the exe folder (`<install>\data`): prepared ELF, disc copy, runtime card, the gt4-gs version in use
  (`data\gs\current.txt`), checked as the launcher checks them.
- Settings: the launcher writes the player's choices (monitor, resolution, GSdx options, the gt4-gs.ini) to
  `%APPDATA%\GT4Recomp\config`; the game reads them at start, so a direct start honours them.
- Missing pieces (no prepared files, no GSdx): a clear message that says to open the launcher once.
- New game slice with the frame identity checks, launcher changes, tests.

Design (slice 278, 2026-10-23; written before the code, release 1.3.0 on branch `task/101-multi-game`):
- **Two ways to start the game.** *Explicit mode* is the way the launcher, the harnesses and the pins start it today: the
  first argument is the ELF. It runs exactly the old code path: no built-in default, no config file, no path search. This is
  what keeps v2.00 frames byte-identical and what keeps every test and pin working; variables and arguments are the test
  overrides. *Standalone mode* is a start without an ELF argument (a double click, or only `--` options such as `--frames 600`
  or `--fullscreen`): the game builds the old command line and environment itself, from the rules below.
- **Where the code lives.** A new `native/game_main.cpp` provides `main`; the pinned donor `main.cpp` (port `slice200`) is
  compiled unchanged with `main` renamed to `gt4_donor_main` (a compile definition on that one source), and `game_main`
  calls it with the argument vector it built. The pure rules (paths, defaults, config parsing, the command line) are in
  `native/standalone.cpp/.hpp` with no Windows calls, tested by the CTest fixture `native_standalone`.
- **Order of every value:** explicit argument or environment variable (wins) > the launcher's config file > the built-in
  default. An empty variable counts as unset.
- **Built-in defaults:** renderer GSdx (`GT4_RENDERER=pcsx2`, `PS2_GS=gpu`), race display 480p (`GT4_FORCE_480P=1`), 16:9 hold
  (`GT4_FORCE_WIDESCREEN=1`), automatic ultrawide (`GT4_DISPLAY_ASPECT=auto`), fullscreen (`GT4_FULLSCREEN=1`, `--window`),
  vsync (`GT4_PRESENT_MODE=fifo`), NTSC, the monitor the window opens on and its desktop size (no `GT4_MONITOR`).
- **Paths from the executable's own folder:** the game must run from `<install>\game\gt4vulkan*.exe`; the game id follows the file
  name (`gt4vulkan.exe` -> `gt4`, `gt4vulkan-spec2.exe` -> `spec2`) and the files are
  `<install>\data\games\<id>\prepared\<ELF>`, the disc copy named by the setup receipt `prepare.json` (`image.file`; its size
  and modified time must equal the receipt, the same cheap check the launcher makes at every Play, and the prepared ELF size
  must equal `elf.size_bytes`), and `...\runtime\card.ps2` (a missing card is a blank card, as today). When
  `<install>\data` has no prepared files, `%LOCALAPPDATA%\GT4Recomp` is tried too (the launcher's write fallback for a
  read-only data folder). The renderer is `<data>\gs\current.txt` -> `<data>\gs\<version>\gt4-gs.exe`, whose SHA-256 must equal the
  `sha256.txt` recorded at install (`gs_release::installed`); that recorded hash is passed as `GT4_GS_PROCESS_SHA256`, so the game's
  existing pin check does the hashing. The game takes the same exclusive lock as the launcher (`launcher.lock` beside the card),
  so a direct start and a launcher start never share a card.
- **Session:** `<data>\sessions\standalone-<unix seconds>`; it becomes the working directory, `misses.txt` goes there, and
  stdout/stderr go to `game.log` there (the console is released). The newest 20 `standalone-*` folders are kept.
- **Settings:** the launcher writes `game.ini` (key=value) and `gt4-gs.ini` (the session ini it renders today) into the user
  config folder: `<install>\user\config` when `<install>\user` exists (portable build), else `%APPDATA%\GT4Recomp\config`. It does so
  at start-up, after every settings change and before every Play. `game.ini` keys: `fullscreen`, `present_mode`, `monitor`,
  `output_size`, `async_gs`, `performance_report`, `check_vu_code`. Each key becomes the variable the launcher would have set
  (`GT4_FULLSCREEN`, `GT4_PRESENT_MODE`, `GT4_MONITOR`, `GT4_OUTPUT_SIZE`, `GT4_GS_SYNC`, `GT4_PERF`, `GT4_VU1_VERIFY`/`GT4_VU0_VERIFY`);
  `gt4-gs.ini` becomes `GT4_GS_CONFIG`; `controls.toml` in the user folder becomes `GT4_CONTROLS_FILE` when it exists. A missing or
  damaged config file means the built-in defaults, with one line in `game.log`.
- **Missing pieces:** no prepared files, no disc copy, a changed disc copy, no GSdx or a GSdx that fails its hash, or a game not
  in an install `game\` folder: a native message box (`MessageBoxW`) says what is missing and to open GT4Recomp once, and the
  game exits with code 3. Nothing is started.
- **Launcher side:** `gt4l_core::launch::standalone_config_text` renders `game.ini` from the same settings and display target
  that build the Play environment (a test compares the two); the Play path is unchanged (explicit arguments and variables), so
  behaviour when the launcher starts the game does not change.
- **Tests:** CTest `native_standalone` (layout from the executable path, game ids, the receipt reader, defaults, precedence,
  command line, config parsing, missing-piece messages); the launcher's cargo tests; the frame identity checks of
  `scripts/frame_cost_headless.py` against the slice268 build (menus, day, night); a headless standalone run.

Result (slice 278, 2026-10-23; recipe and numbers in docs/BUILD.md "Slice 278"):
- Built from the slice268 recipe into `O:\gt4recomp-scratch\slice278-build\build`. CTest 43 of 44 pass (new
  `native_standalone`); the failing test is `python_native_contracts`, whose only error is the release-pin check
  (`test_make_distribution`), expected for any rebuilt executable. Python with `GT4_NATIVE_BUILD` = slice278: 682 tests,
  1 error (that one), 42 skipped; new `tests/python/test_standalone_start.py` (7 tests) passes.
- **Identity with the launcher's arguments (explicit mode), exact:** menus dumps `463d455c3204cb56` / stream
  `1ccb97c58a6d2945`, day `992f1ac9a6871901` / `0c79dcf5208e5e4d`, night `c57e7d03accbb3aa` / `c091716e3cb40d79`, equal to the
  slice268 run in each scene (41, 159 and 159 dumps, name and hash lists byte-identical).
- **Headless standalone start** (a scratch install, no ELF argument, `--no-window`): menus and day give the same dump lists and
  stream hashes as above, and as the harness run with the defaults passed as explicit variables.
- `cargo test --workspace`: 436 passed, 4 ignored, 0 failed in each build (installer and `--features portable`); new tests: the
  `game.ini` text against the sample the game fixture reads and against the variables Play sets, and the files written
  after a settings change (`flow_audit`).
- Decisions: the donor `main.cpp` stays untouched (pins): `game_main.cpp` provides `main`. **Standalone mode is a start with no
  ELF argument**; with an ELF argument the old path runs unchanged, so no harness needed an explicit "off" value and no
  pin or test changed. The launcher keeps passing its explicit arguments and variables at Play (behaviour unchanged) and
  writes `game.ini` / `gt4-gs.ini` for the direct start. `controls.toml` is found in the same user folder.
- Not done or not seen: a real double click (window, fullscreen, the console release and `game.log`), the native message boxes
  on screen (they are tested through `GT4_STANDALONE_MESSAGE_FILE`), the portable zip on another PC, a direct start before the
  launcher ever played (the card is then blank). Paths with characters outside the system code page are not handled (the donor
  main reads its arguments as narrow text). A direct start does not harvest the card into the saves library; the launcher
  does it at the next Play (`prepare_card`).

### TASK-095 — Ultrawide: undistorted pre-race menu overlay
Planned (owner, 2026-10-22): at ratios wider than 16:9 (owner screenshot at 3440x1440, Time Trial on Trial Mountain) the
pre-race menu over the race (car name, CV/kg/tires table, Session Record, the icon row) is stretched with the screen, and so
are the other 2D screens over the race (pause, results). Same guard rails as TASK-080 (decision 0071). Re-does PR #177 (Luan
Antony), whose canvas maths had no call site. Branch `task/095-ultrawide-menu-canvas` (from
`task/134-game-code-optimization-study` at `bb4c7ed7`); `docs/ULTRAWIDE-OVERLAYS.md`, decision 0071.

- Found (both games): the pre-race screens are drawn by the menu context (v2.00 `mUpdateContextPS2` render `0x00214FB0`,
  Spec II `0x0036D278`), the pause menu, pit menu and results by the strobe overlay renderer (v2.00 `func_003EC0B0`, Spec II
  `0x001FDCC8`); each sets a 0..640 x 480..0 canvas through a direct call of the ortho function (`0x004A5A90`, Spec II
  `0x005A2600`). The strobe text is sized by the text style parser (`0x00490290`, Spec II `0x0033AF90`), not by the canvas.
- Change: `scripts/camera360_units.py` adds host calls before those ortho calls and after the strobe draw, and routes the
  parser's four scale stores through the host. While the presentation fills the screen at R, the canvas is widened by
  R / (16/9) around its centre (taller for R < 16:9) and the strobe text narrowed by the same factor; at 16:9 and in the 16:9
  pillarbox nothing changes. The host writes no guest RAM. The race monitor is left as the game draws it.
- Acceptance: CTest `native_display_aspect` (`test_overlay_canvas`) and `native_game_target`; Python `test_camera360_units.py`;
  `scripts/coregs2_bench/overlay-aspect-check.sh` on both games: 16:9 identical to the reference (clock, state, frames); at
  21:9 and 32:9 the race identical and the overlay frames the reference's narrowed by 1/s, centred.
- State (2026-11-04): done on both games and checked headless (v2.00 Arcade pre-race screen and pause, Spec II Time Trial lobby
  and pause): 16:9 identical in clock, state and every frame; at 21:9 and 32:9 only the overlay frames differ, each the
  16:9 look narrowed and centred, pause text inside its buttons. Owner test in game pending.

### TASK-091 — Release 1.2.1: per-machine install, data beside the program, user data in AppData
✅ **Done (2026-10-22): owner tested and approved; released as v1.2.1.** Branch `task/091-install-layout-121`. Owner decisions 2026-10-22:
- the installer installs per machine in `C:\Program Files\GT4Recomp` (administrator rights; the folder page stays);
- the large data (disc copy, prepared files, gt4-gs, caches, sessions, logs, custom textures) lives in
  `<install>\data`, found relative to `GT4Recomp.exe` (portable: the folder can be copied to another drive or PC; a
  read-only copy is read and writes go to `%LOCALAPPDATA%\GT4Recomp`); the installer gives the Users group
  (`S-1-5-32-545`) Modify on that folder only;
- settings, controls, saves and replays live per Windows user in `%APPDATA%\GT4Recomp`;
- the launcher, not the installer, moves the 1.2.0 files at its first start (checked copies, Documents folder renamed
  with a README, nothing overwritten, never while the game runs, retried after a failure) and removes leftovers of the
  old per-user install; the installer removes the old per-user program install;
- the self-update starts the elevated setup (`runas`); uninstall removes `<install>\data`, and the "also delete" box
  acts only for the current user.
- the setup of a self-update from 1.2.1 on is silent (`/S /UPDATE /D=<install folder>`, one UAC prompt) and the launcher starts again as the player; a reinstall into another folder records the old data folder (`PreviousDataDir`) and the launcher moves its large data at its first start.

Acceptance: `cargo test --workspace` green with the new path, move, cleanup, elevation and settings tests; UI checks;
installer compiled and extracted; owner test of a fresh install, the 1.2.0 update, the move, a portable copy and the
uninstall. Result so far: 330 Rust tests passed; owner test installer `private/slice250-installer/GT4Recomp-Setup-1.2.1-test.exe`. Slice 251 (review fixes): 332 passed. Slice 252: 343 passed; installer `private\slice252-installer\GT4Recomp-Setup-1.2.1-test.exe` (28,845,242 bytes, sha256 `2FD43DCF6D18FDAE981AF0CD22D79E259E7175ED08DFF41AE3007802A1CF42FF`), not run yet: owner test of the silent update from 1.2.1 and of a reinstall into another folder (data moved from `PreviousDataDir`).
Slice 257 (GitHub issue #80, blank white launcher window): the window starts hidden and shows on `ui_ready`; a 12 s watchdog restarts the launcher in WebView2 safe mode (`--disable-gpu`, flag `webview-safe-mode`, Settings toggle "Launcher GPU acceleration"), then a native message offers the WebView2 download page or the log folder. `cargo test --workspace` 351 passed. Installer `private\slice257-installer\GT4Recomp-Setup-1.2.1-test.exe` (28,859,313 bytes, sha256 `51998294e3ccd9b0b80b6175e5c6e3b969d89c6236f4669878dcf274c6bbb70a`), not run yet: owner test with `GT4RECOMP_TEST_UI_NEVER_READY=1`.
Slice 258 (1.2.1 final candidate): a missing WebView2 runtime is detected before any window (registry as the installer checks it, `GT4RECOMP_TEST_WEBVIEW2_MISSING=1` for tests); the update path is faster (no icacls on an update unless its probe fails, EstimatedSize without the data tree, the launcher starts before the old copies are removed; phase timings in setup.log); the launcher clean-up runs after the window exists; a second start never shows the hidden window. Slice 259: **Performance Mode** (Settings > Graphics, Off by default; On = GSdx hardware downloads NoReadbacks, replaces the Readbacks row; CoreGS 1.1 shows it disabled). Installer `private\slice259-installer\GT4Recomp-Setup-1.2.1-test.exe` (28,859,775 bytes). Not run on this PC; owner test pending.
Details: BUILD.md slice 250, [release notes 1.2.1](docs/release-notes/1.2.1.md).

### TASK-090 — CoreGS 3.0: modern render pipeline
Planned (owner, 2026-10-22), after CoreGS 2.0. The VU1 work (the PS2's vertex shader, today AOT C++ on the MTVU
thread) moves onto the GPU and the game renders natively. It may first run the whole game with some effects missing
or approximate, then restore them one by one natively (flares through occlusion queries, no GS readbacks). Fidelity
is a measured, per-scene target, not a bit-exact gate. A faithful renderer stays the default and the reference; CoreGS
3.0 is opt-in until it beats it. Game logic, physics, timing, saves and gameplay-relevant read-backs never change.
Scope rule in AGENTS.md; roadmap in docs/COREGS2-ROADMAP.md; study `vu1-direct-gpu-study` (in progress).

### TASK-087 — GS readbacks without stalls
**Owner decision 2026-10-21: CoreGS 2.0 only.** gt4-gs (the PCSX2 GS fork) gets no more feature work; at most texture
replacement is switched on (its implementation already exists) with a launcher option (TASK-048). So the gt4-gs steps
below (S1 protocol hint, S3 asynchronous download in the fork) are dropped. The readback work lands in CoreGS 2.0:
S4 GPU-driven flare after M8/M9, with P0/P1 as its proofs and the one-shot captures kept on the GPU. Removing EE-side
waits in our runtime (S2 deferral, TASK-084) stays the general goal: optimise where possible and remove the waits.
Planned (owner, 2026-10-21). Study: [docs/GS-READBACK-STUDY.md](docs/GS-READBACK-STUDY.md). Owner report: at 8x
(5120x2160) on gt4-gs a night race drops to 40 FPS when the headlight flares show; "unsynchronized" readbacks run
smoothly but lose the flares, the loading-screen logo and the car icon beside the gap time. Findings (Confirmed): all
readbacks go through `func_004A5178` (4 call sites), queued in the pgl display list; the EE never reads the flare pixels.
The GS reduces each light's depth-tested square to one visibility pixel in a 64x8 table, the table is copied back and
pointed at by a DMA REF to VU1, which scales the flare sprite in the same frame. The other readbacks are one-shots
(`CarIconMaker::render` icons at race load, `func_00104B68` loading-screen capture, a 64x64 thumbnail) plus photo mode
(8x4 depth autofocus, tile capture). Plan: P0 per-call-site readback log and game-state hash, accurate vs zero-filled;
P1 VU1 flare program checked over visibility 0..255; S1 protocol 1.4 per-call-site hint so one-shots stay accurate;
S2 deferred flare readback (TASK-084) with a per-light remap (about -4.2 ms per night frame at 1x); S3 asynchronous
download in the gt4-gs fork (PCSX2-tainted agent; expected 60 FPS with flares at 8x); S4 GPU-driven flare in CoreGS 2.0
after M8/M9 (owner's idea: the renderer multiplies the marked flare draws by the visibility texel on the GPU; zero
readbacks per frame). Corrections: `updateFlare` does not read the result (NATIVE-VARIABLE-FPS-STUDY), and deferral
moves the flare from 0 to 1 frame late, not 1 to 2 (FRAME-COST-STUDY).

### TASK-088 — Release 1.2.0: custom textures, crop, black start
**Done 2026-10-22: owner approved ("1.2.0 pronta pra lançamento"); released as GT4Recomp v1.2.0 and gt4-gs 1.2.0.**
Owner decisions (2026-10-21). Release 1.2.0 of both the game/launcher and gt4-gs; the work is one branch per repository
(`task/088-custom-textures-120` in this repository, `gt4gs/textures-1.2.0` in `moesuito/gt4recomp-gs`).
- **Custom textures (gt4-gs 1.2.0).** PCSX2's replacement loader was already compiled into gt4-gs; the fork now reads the
  pack from `[Folders] Textures` directly (`replacements` inside it, no game-serial folder) and never dumps (load only;
  the Dump keys are refused and forced off). The launcher sets `[Folders] Textures` to `Documents\Gran Turismo 4\Textures`
  and creates `Textures\replacements` on demand. Keys: `LoadTextureReplacements` (Graphics, "Load custom textures", default on,
  always written) and `PrecacheTextureReplacements` (Advanced, "Precache custom textures", default off). An "Open textures
  folder" button sits in Graphics. Settings schema 6 (no value changes; the backup of schema 5 is kept as before).
- **Crop.** The GT4 display crop is 2 native pixels at the bottom (was 1) and 1 at the right; a log line states it.
- **White start.** The gt4-gs window had `COLOR_WINDOW` as its background and erased nothing, and the game window had no
  background brush, so both showed white until the first frame. Now black (class brush, `WM_ERASEBKGND` paints black
  in gt4-gs; `window.cpp` class brush black in the game). A new game build (slice240) carries the game change.
- **Versions.** gt4-gs 1.2.0; GT4Recomp 1.2.0 (Cargo workspace, `tauri.conf.json`, NSIS default). The launcher's minimum
  gt4-gs is 1.2.0, so an older gt4-gs is replaced at launch by the existing `needs_install` path.
- **Checks so far.** `cargo test --workspace` (launcher) all green; new tests in `launcher/crates/gt4l-core/tests/textures.rs`
  and the migration test for schema 5 to 6. Browser UI: nav-test 150 of 150; focus-check 92 of 100 on this checkout and on
  the baseline copy (the same 8 tab-highlight checks fail on both: a headless timing difference, not this change).
  Game build slice240 (`private/slice240-game-build`): see `docs/BUILD.md` slice 240.
- **Status.** Waiting for the owner's test. Nothing is tagged, released, drafted or merged; gt4-gs 1.2.0 is a local package
  until the owner approves it.

### TASK-018 — Racing wheels

Study (2026-11-04): Luan Antony's wheel, DFP and force feedback roadmap (`docs/studies/WHEEL-FFB-ROADMAP.md`, PR #151, issue
#88) is kept as a reference beside the owner's plan (`docs/WHEEL-DFP-PLAN.md`, PR #193).
High-level emulation at the PDIUSB boundary with SDL3 wheels and force feedback
(docs/MENUS-AND-WHEEL.md). Builds on the TASK-075 SDL3 input layer.
Planned for 1.3.1 (owner update, 2026-10-26; previously 1.4.0): the game must recognise the connected wheel as a Logitech Driving Force Pro (DFP)
(900-degree range and force feedback in GT4), whatever wheel SDL3 finds (Logitech, Thrustmaster, Fanatec); the
game's force-feedback commands go to SDL3 haptics. The launcher gets a new Wheel tab (detection, rotation, force
feedback strength, pedals and buttons, a live test). Study first: the GT4 USB stack (USBD, PDIUSB, LGDEV IRX,
docs/IOP-MAP.md), where to answer as a DFP, and the force-feedback command set.

Owner scope update 2026-10-26: the virtual target is Driving Force Pro (DFP), not DFGT.
Native 900-degree steering and game-generated wheel force feedback are planned for 1.3.1;
1.3.0 stays focused on Spec II and performance. TASK-123 prepares research and an implementation
plan only. The private 1.2.5 draft is held back from publication; existing input contributions remain
in main as reusable groundwork and are not proof of native wheel support.

Host-input groundwork was merged through TASK-121 and packaged in the private 1.2.5 draft.
Publication is now on hold. The guest USB/Logitech identity and native force-feedback protocol
remain future acceptance for 1.3.1; this contribution does not complete TASK-018.

### TASK-075 — Input layer on SDL3
Owner decision 2026-10-21. Replace the game's XInput-only pad code (native/host_io.cpp) with one input layer on
SDL3 (zlib licence, fine in the MIT game exe; also the Linux and Android path, TASK-074 and TASK-022). SDL3 opens
only its gamepad/joystick subsystems; the game window stays ours. Every pad SDL3 knows (Xbox/XInput, DualShock 4,
DualSense, Switch Pro, generic) maps to the DS2 layout with analog L2/R2 pressure and stick dead zones kept. The game
reads the launcher's controls.toml (the branch controls code, GT4_CONTROLS_FILE), so remapping in the launcher
applies in game. The launcher's Controls page reads pads through the same SDL3 database (Rust sdl3 crate) instead of
the WebView Gamepad API, so the launcher shows exactly what the game sees. The input layer runs in the game process
(no launcher bridge: the launcher is hidden in the tray during play, and latency and force feedback need the game
side). Racing wheels (TASK-018) build on this layer. Acceptance: Xbox and DS4/DualSense pads work in game with no
third-party tools; launcher remaps apply in game; headless frame dumps unchanged; owner test.

State: ✅ Done (2026-10-21). Game side slice210 (SDL3 3.4.18 pinned in `docs/inputs/slice210-sdl3.json`, `native/pad_mapping.*`, `GT4_INPUT=sdl` default, `xinput` kept as a fallback), launcher side on the Controls page (SDL3 through the Rust `sdl3` crate). Owner tested pads and remapping, and approved.

### TASK-076 — Modern fixed keyboard layout
Owner design 2026-10-21, hard-coded (not remappable for now), several keys per DS2 button: D-pad left/right = A/D and
arrows; D-pad up/down = arrows; D-pad up (ghost) also E; Cross (accelerate, confirm) = W and Enter; Square (brake) = S;
Circle (handbrake) = Space; Triangle (back) = Backspace and Esc; nitro = Shift; gear up / gear down = their own keys
near WASD; look back = Q; camera = C or Tab. Map each action to the DS2 button GT4 really uses (check the decomp and
the manual: nitro, gears, rear view, camera). The launcher Controls page shows the layout read-only in Keyboard view.
State: ✅ Done (2026-10-21): the hard-coded table (`native/controls.cpp`, `docs/CONTROLS.md` keyboard-default, `docs/INPUT-SDL3.md`). Nitro = R1, gears = R2/L2, rear view = L1, camera = Select (ludo.guide); Start = P (owner). Owner tested.

### TASK-079 — Force 16:9 at boot
Owner request 2026-10-20. A fresh save starts the game in 4:3. When the player picks 16:9 (Wide) in GT4's own Options and
saves, it stays 16:9. Force 16:9 at boot for everyone, the same way GT4_FORCE_480P holds 480p (TASK-009, slice164).
Environment `GT4_FORCE_WIDESCREEN`: 1 forces 16:9, 0 leaves the game's own setting, unset means 1 (the launcher passes
its setting). Any other value refuses the start. The launcher setting "Widescreen 16:9" (Graphics page, default on) sends
it. Forced in guest RAM only; the owner's card file is never written by the host.

Guest chain (decompiled source at identical addresses, gt4-decomp e0b70c4d6; Confirmed unless marked):
- owner pointer `0x622F4C` (the game-data object); option object `owner + 0x38CB0` (MOption).
- wide flag `option + 0x14` = `owner + 0x38CC4`: 0 = 4:3, 1 = 16:9. Set by `func_00437110` (the script setter
  `MOption.wide_mode`, `MOption__get_wide_mode`), read by `func_001AB4C0` (race HUD rectangle, 0.75 scale of the
  4:3 squeeze) and by the photo mode code. Saved with the option object (High confidence: the same object holds the
  garage and calendar that the save path writes).
- aspect record `option + 0x24`, 11 words, written by `func_00439108` from one of four presets in guest data:
  `0x622E90` (4:3), `0x622EC0` (wide), `0x622EF0` (4:3 NTSC), `0x622F20` (wide NTSC); index = wide + 2 * (video == 1).
  `func_004390D0` says "wide" when width / height >= 1.5 (f32) at `record + 0xC` and `+ 0x10`.
- presets are filled at start by `func_00439B30` from `0x6A6188` (4:3, 275 x 205) and `0x6A61B8` (wide, 425 x 240);
  the NTSC presets hold the same two records (checked in the pinned ELF, Confirmed).
- video system `option + 0x1388` (1 = NTSC).
- published word `0x6184F0` (D_006184F0) = `func_004390D0(option + 0x24)`, written by `MOption.apply` (`func_004365B8`).
  It is the value the renderers read: RaceDisplay, RaceDisplayUtil::wideOffset, the 0.75 scale in `func_00393790`
  and `func_00102B30`, the minimap offset.

Native (`native/race_widescreen.*`, pure, fixture `native_race_widescreen`): at each guest VBlank, when the owner chain
is valid and the wide preset is wide, it sets the flag to 1, copies the wide preset into the record when the record is
not wide (what `func_004391C8` does), and publishes `0x6184F0` from the record. A preset that is not wide is refused
with no write (logged once).

Limits: the game's menu can show 4:3 for one frame before the next VBlank forces it back (same limit as 480p); a save
made with the force on stores 16:9. The presentation of the 4:3 and 16:9 layouts of the game is otherwise unchanged.

Checks (2026-10-21, slice211): see `docs/BUILD.md` slice 211 and `docs/journal/2026-10-21.md`.
State: ✅ Done (2026-10-21). slice211 (`docs/inputs/slice211-pcsx2.json`, `--mode pcsx2`) and the launcher setting
"Widescreen 16:9" (default on). Owner tested: a blank card boots in 16:9 and 480p, and the 100% save loads. A "Load
failed!" seen during the test was the launcher card bug (save folder "." entry), fixed in TASK-019; the force was
ruled out headless (slice210 and slice211 with the force off failed the same way on the bad card).

### TASK-019 — Launcher
State (2026-10-21, TASK-075 launcher side, branch task/019-launcher): the launcher reads pads through SDL3 3.4.18 (`gt4l-platform` `sdl_pads`, the same layer the game uses), in a Rust pad service that emits `pad-status`, `pad-live` (about 60 Hz) and `pad-nav`; the Controls page and nav.js use these events, the Web Gamepad API is only in the dev mock. The Keyboard view shows the fixed modern layout (TASK-076), read only. Checks: `cargo test --workspace` green, nav-test 149 of 149, focus-check 90 of 90 (headless Edge), real exe over WebView2 with 0 ACL denials. Not done: the game side (task/075-sdl3-input) and the owner's pad test; the launcher's controls file still uses XInput-named sources (the game reads them until its SDL layer lands).
State (2026-10-21, slice209): native game reads `GT4_FULLSCREEN` (strict 0/1; the launcher default is on). Game executable slice209 (build `private/slice209-build`, sha256 a75c58fa..., 97,105,408 bytes), mode `pcsx2` (docs/inputs/slice209-pcsx2.json): the slice208 coverage library and the fullscreen start only. CTest 33 of 33, Python 589 OK (23 skipped), headless 42 of 42 frame dumps identical to slice208. The owner launcher test folder was rebuilt (`private/owner-test-launcher/game`, slice209; the slice206 folder is `game-206-old`). Owner test of the fullscreen start pending; the branch is not in a PR.
State (2026-10-20): in progress on `task/019-launcher`. Main is merged (c4c7304) and the UI branch is merged (4292f7f).
The settings table is refreshed from main (TASK-049 page/curation, TASK-064 prohibit_dynamic_code always written), and the
Rust gt4-gs.ini writer equals scripts/gs_settings.py (golden fixtures regenerated by the Python writer). Checks: cargo
test --workspace 117 passed, 0 failed, 1 ignored; Python unittest 555 OK, 38 skipped. The logo has a white-text variant
(ui/assets/logo-gt4-white.png, launcher/tools/make_white_logo.py) on the setup, home and settings pages. The home page
has a background slideshow (12 s per image, 1.5 s crossfade, slow pan, reduced-motion fallback) with placeholder images
(ui/assets/backgrounds, see its README; the owner is making new images). The version is 1.0.0 everywhere it is declared
(Cargo workspace, tauri.conf.json, installer NSIS, home label, About). UI pass 3 (2026-10-20, owner requirements: touch, controller, Play): every control reachable by touch
(48 px targets, tap help tips, slider drag, swipe between Settings tabs, scrolling lists), every page
reachable by controller (X Save on Settings, Y secondary action, right-stick scroll, footer hints per
page), and the glass Play button (idle sheen, solid white when focused). Branch task/019-launcher, not yet
in a PR. Still open: the owner test of setup, saves, controls and the new touch and pad behaviour;
greying the presets above the device limit (max_upscale); the installer test and the live update test (TASK-043).
State (2026-10-20, owner requirements, one writer): SemVer streams (GT4Recomp `vX.Y.Z`, gt4-gs `gt4gs-vX.Y.Z`, minimum
gt4-gs 1.0.1, asset `gt4-gs-X.Y.Z-windows-x64.zip`), `version.json` (installer writes it, launcher records gt4-gs),
first gt4-gs install during setup and at launch, a launch-time update check (never blocks Play; popup with Update,
Not now, Skip this version; updates never automatic), data-folder guard, schema migrations with backups (settings,
controls, version.json), tray (hidden while the game runs, back on Home when it ends, close asks while it runs), and
the Graphics table of the UI mock. Docs: `docs/RELEASES.md`, `docs/launcher/MIGRATIONS.md`. Checks: `cargo test
--workspace` green. Not done here: a real installer build, a live GitHub check and the owner's end-to-end test.
State (2026-10-20, flow audit, `docs/launcher/FLOW-AUDIT.md`): the whole flow mapped and tested end to end with the
real discs in a sandbox (45 steps: 23 PASS, 17 FIXED, 0 FAIL, 5 NEEDS-OWNER). Disc rule: SCUS-97328 whose CORE.GT4
equals the pin is accepted (v2.00 must also match the full image pin); Spec II and other discs get "This disc is not
supported yet". Setup now copies the image to `%LOCALAPPDATA%\GT4Recomp\disc\SCUS-97328.iso` (free-space check,
verify then rename, schema 2 receipt), so the picked file is needed only once; a missing copy leads back to setup.
Setup draws copy, check, prepare and gt4-gs download progress; the real gt4-gs 1.0.1 zip (one wrapping folder) now
installs. The real GT4Recomp.exe opened on Setup, Home and "Game files missing" in the sandbox. Checks: cargo test
--workspace 186 passed, 3 ignored; nav-test 55/55. Owner: file picker, Play, tray, update popup, pad, touch.
Rust + egui or GPUI: game image, renderer (paraLLEl-GS / donor fallback),
upscaling, video mode, controls; CPU feature detection for build variants.

### TASK-025 — Save manager
One standard 8 MB PS2 card image per slot under the user data folder; the
launcher passes the active slot's card to the game. Import from single-save files
(.psu first; .max/.cbs/.xps later), from card images (.ps2/.mcd, listing their
saves) and from PCSX2 folder cards; import always creates a new slot and never
overwrites one. Export, delete to Recycle Bin, backups. Optional slot summary
(day, credits) from Nenkai's PDTools save format. Atomic card writes in the
runtime. Design: `docs/launcher/LAUNCHER-PLAN.md` on branch `task/019-launcher`.

### TASK-028 — CoreGS 1.1
Our current Vulkan compute GS renderer (CoreGS 1.x, donor lineage, MIT) gains:
internal resolution scale 1x-4x (`--mode upscale --scale N`, PS2_SCALE; 3x is
the release target), a deinterlace filter active only for interlaced output
(behaviour from our clean-room description in docs/hw-renderer-spec chapter 13,
no GPL/LGPL code copied), an FXAA-class AA pass and AMD FSR 1.0 (MIT) for
viewport scaling. Branch `task/028-donor-upscale`. Part of release 1.0.0
(docs/RELEASE-1.0.0-PLAN.md).

### TASK-029 — Controller and keyboard mapping
Binding table in the runtime (XInput auto-map, LT/RT as L2/R2 pressure,
keyboard remap, deadzones/curves) read from a controls file the launcher
writes; schema in docs/CONTROLS.md. Branch `task/029-input-mapping`.

### TASK-026 — Own hardware renderer: clean-room specification
Study only (decision 0069). The specification team reads the PCSX2 hardware GS
renderer (pinned source) and public GS documentation and writes a behavioral
specification in plain English under `docs/hw-renderer-spec/` (branch
`task/026-hw-renderer-spec`): architecture and data flow, GS state to GPU draw
translation, texture cache and render-target management, upscaling (internal
resolution multiplier and its fixes), CLUT, readbacks, blending and channel
shuffles, texture dumping/replacement and pack format, GT4-specific fixes,
post-processing, validation scenes and test cases, and an implementation plan
with milestones for TASK-027. No PCSX2 code or code-like translation in the
specification. Sizes and coupling overview: `docs/GSDX-STUDY.md` on branch
`task/012-parallel-gs`.

State (2026-10-15): specification complete in `docs/hw-renderer-spec/`
(README index, chapters 00-15; chapter 15 holds milestones M0-M19 for TASK-027,
~94 implementer-days, risks and 13 open owner questions). Ready for owner review;
TASK-027 starts after approval and the answers to Q-01 to Q-03.

### TASK-027 — Own hardware renderer (CoreGS 2.0): implementation
**State 2026-11-02, published:** 🧪 in the 1.3.0-rc8 pre-release (candidate 382). The owner's laptop test closes the beta gate: on the HD 620, 1x at 60 FPS or 2x at 30 FPS in races; the showroom at 2x (about 30 FPS) is a known issue. D1 is measured and not built (IN-523). The colour filter stays in Performance Mode (IN-505 left out). For 2.0.0: fractional scales, GPU use and power beside GSdx. Next (owner): CPU optimisation, TASK-134, for rc9.

**State 2026-11-02, end:** 🧪 candidate 376 (not uploaded), branch `task/082-coregs2-frame-skip`: a frame rate choice, 60 or 30 in races, replaces the option of 375 (IN-525), 60 by default: 12x 100% speed at 30 frames per second, intervals 33.0-33.7 ms. Unlocked FPS paused, `docs/TASK-082-UNLOCKED-FPS-STATE.md`. Next: CPU optimisation with another agent (TASK-134).

**State 2026-11-02, later:** candidate 375 (not uploaded), branch `task/082-coregs2-frame-skip`: candidate 374 plus TASK-082, frame skipping on CoreGS 2.0, off by default (IN-524): 12x on George V Paris 100% speed at 38.5 frames per second, against 77% at 46; the frame shown is always the last one drawn; race clock equal on both games. Waiting for the owner's test, above all on the laptop.

**State 2026-11-02:** 🧪 candidate 374 (not uploaded), the base of 375: input lag at 60 Hz 53 to 31-39.5 ms, the EE's wait at the flare read 9.5 ms to 0, flares exact, frames identical to 373 (122 of 122, then 47 of 47).

**State 2026-11-01:** 🧪 candidate 373 (not uploaded), adding to 372 the frame pacing work IN-514 to IN-519 (race on George V Paris at 6x without Performance Mode: GS CPU 7.94 to 7.31 ms, frames over 17.5 ms 40 to 5 of 3,700; frames identical 154 of 154; the second presentation queue is not yet confirmed on the owner's virtual display; the owner decides on the late flares of IN-514). Candidate 372 before it: Spec II on CoreGS 2.0
(IN-506), shaders compiled before Play (IN-507), no readback for a write beside a drawn area (IN-508: George V Paris at 6x,
headless, GS thread -19%), no frametime wave from a target drawn every second VBlank (IN-509), and IN-510 to IN-513: the race start
drop, three swapchain images under vsync, the showroom wave and reused decodes (window, race without Performance Mode: VBlanks over
20 ms 29 to 1 of 1000). Frames identical at 1x, 3x and 6x; at 12x 2 pixels of one frame. The owner's F8 on 372 is pending. The owner's F8 at 6x on George V Paris: 49.6 VBlanks/s, 57.7 in
Performance Mode; the GS thread's waits for GPU readbacks are the largest cost. Open: GSdx's F8 for comparison.

**State 2026-10-31:** 🧪 candidate 367 (not uploaded) waits for the owner's test on the laptop: on the HD 620 at 2x GPU
busy 27% less by day and 23% at night against candidate 362 in normal mode, 35% and 31% in Performance Mode (IN-485 to
IN-504, `docs/hw-renderer-spec/CORE2-GPU-PLAN.md`). In the launcher as a beta in 1.3.0-rc8 (GSdx stays the
default): texture replacement (IN-470), GPUs without interlock (IN-471), PGO in the release game (IN-473), options by
renderer (IN-474). Open: the owner's test, then the upload of rc8 and the PR into
`task/134-game-code-optimization-study` on their word; fractional scales, a screen that prepares shaders, frame skipping.

**Resumed on 2026-10-21 (owner decision)** on branch `task/027-coregs2` (worktree
`C:\Antigravity\gt4-wt-027-coregs2`): `main` at release 1.0.0 (slice214) merged with
`task/027-hw-renderer` and the `task/027-cpuref` docs checkpoint. paraLLEl-GS code, pins and launcher modes
were left out (decision 0070); `GT4_RENDERER=hw` now runs through main's GS stream seam into the in-process
HWR backend (`GT4_HW_GS=ON` builds only; black output). `task/027-m4-gpuref` (e31fcc4, GPU write-back WIP)
is not merged. Implementation continues with the external agent **GPT 6.1 Sol** as the only writer of the
branch, clean room from `docs/hw-renderer-spec` (brief `AGENT-BRIEF-COREGS2.md`, progress in
`docs/hw-renderer-spec/PROGRESS.md` and ai-memory `decisions/coregs2-implementation`). No pins until a
candidate is offered to the owner. Warm-up complete (2026-10-21): shader TEXA/AEM follows 07.5 / IN-07; slice217 builds and all three GPU exactness fixtures pass. Native v2 replay is implemented (slice219: 69/69 CPU-library tests; 1,101/1,101 identical menu PPMs across legacy, converted v2 and fresh v2). VRAM diagnostic added (local221:75/75 tests, deterministic1,096-image replay; original scanout unchanged). Correct pairing resolves the IN-315 comparison offset; the separate A+D kick fix (IN-326, local222: 76/76 tests) closes IN-316: the garage defect was the dropped A+D kicks, exactly frames 811..916 change, and 11/11 aligned samples now sit inside the chapter 14.4 2D thresholds (garage 0.2051%, 45.992 dB) while 990/1,096 VRAM and 995/1,101 scanout images stay unchanged. The M4 GPU write-back path of `task/027-m4-gpuref` is then ported, built and wired into the backend (local223, IN-329): eleven fixtures pass (03.3 coverage vectors and seven GPU/CPU equivalence cases), the default renderer stays 42/42 vs slice214 and `GT4_RENDERER=hw` runs 600 headless frames with 0 faults, still without frame dumps. IN-328 fixed the CPU reference's constant-attribute truncation. M5 then wired the whole pixel pipeline into the same draw stage (blend unit over 81 modes, COLCLAMP, PABE, alpha test, DATE, FBA, FBMASK, dither; five new raster fixtures at zero mismatches); `GT4_RENDERER=hw` stays clean at about 62 ms per headless frame. M6 then started with the presentation interfaces (IN-331): a present scheduler decoupled from the VBlank (hold/re-present, newest-wins backlog, late-input hook), auxiliary frames that restore guest memory and refuse readbacks, and per-pass CPU timers; default 42/42 parity unchanged. HWR frame dumps then landed (IN-332): the 4,200-frame headless run writes the same 42-frame schedule with 0 faults and measures 42/42 frames outside the 2D gate, with two visible draw-loss defects (boot text line, garage list). The HWR replay tool then landed (IN-333): on the same records the GPU path is pixel-identical on every even frame and loses content on odd frames (boot text line, garage 10.6%), so field-parity draw loss is the open defect. The cause was found and fixed (IN-335, local slice228): the write-back stored stale depth words over a frame buffer sharing pages with the unused depth buffer. The 42-dump headless run is now 42/42 inside the 2D gate against the donor (before 0/42). The window now follows the VBlank field (13.3 offset, 13.4 deinterlace; IN-337/IN-338, local slice229). Next: owner test 1 (windowed look at 1x), then M7. History of the pause follows.

**2026-10-21, Claude (clean-room writer after GPT 6.1 Sol):** M6 output finished (odd-frame fix, field phase,
deinterlace; owner tests 1 and 2), then the 3D speed work: resident target slots, pixel-interlock batching and runs,
the GLM with GPU CLUT, GLM-owned pages and GPU uploads, CPU front-end savings, double-buffered submission and an
asynchronous presentation source (IN-340..IN-355). Every step keeps per-frame memory identical to the write-back
mode on the menu, car and race captures and the 42 menu dumps byte-identical. Headless race 300 -> 17-18.6 ms per
frame. Waiting for owner test 3 (local slice239, `build/hwr-play.ps1`). Next: GPU compose of the display, fewer
barriers around CLUT loads, the GIF front end, M8 readbacks per the readback study.

**2026-10-22, Claude:** owner tests 3-5 (60 FPS in 3D races, flares, 16:9 480p; test 5 approved a flat 60 FPS frametime on local slice247). `main` 1.2.2 merged into the branch (local slice248/249; hw output identical to slice247, default renderer 42/42 after fixing the 1.2.2 donor-mode crash in `mtvu::Donor`). M12 upscaling (2x-8x) and FXAA in local slice250 (IN-371..IN-376; memory and readbacks identical to 1x at every scale), waiting for owner test 6. Next: owner verdict vs GSdx, 480i menus at scale, then GS-thread CPU cost.

**2026-10-23, Claude:** `main` 1.2.3 merged. Owner test 6 failed (race shown at 1x on every second frame at 4x; ultrawide race squeezed into 16:9); both fixed in local slice270 (IN-377 display shadows checked per pixel against memory, IN-378 display-aspect fill in the HWR presenter) with new controls (`hw_scale_display_shadow`, `hw_smoke_window_aspect_*`, `hw_stream_sink_display_aspect`, replay `--check-presented`). Owner test 7: ultrawide confirmed, race still 1x; fixed in local slice271 (IN-379 tone carry for GT4's T8 tone curve, IN-380 texture footprint for full-screen copies; fixture `hw_scale_fullscreen_passes`; `--check-presented` counts low-detail frames). Owner test 8 passed (races at real resolution, beta minimum met in races); new owner requirement: GSdx-level performance at the same scale. Local slice272: 480i menus at scale (IN-381, fixture `hw_scale_interlaced_present`), no scaled repetition of the tone sequence (IN-382), timing split into 1x/scaled/refresh, scene sweep findings and a ranked parity plan in PROGRESS. Waiting for owner test 9.

**2026-10-23, OpenCode (parity work after the hand-over, `PARITY-PLAN.md`):** owner decisions: item 1 (fast scaled
raster) approved with a tolerance only on the presentation-only scaled image, item 2 (tone-sequence HLE at 1x)
approved as memory-exact, item 5 (skipping the 1x redraw) rejected. Item 2's first step landed (IN-383): the eight
strip batches of one 64 x 32 page run as one GLM dispatch (`hw_tone_strips.comp`), 135,399 -> 16,940 dispatches on the
480p race capture, with 118,580 of its palette loads held (GPU CLUT loads 200,467 -> 98,827); all five memory captures
byte-identical, `hw_tone_hle_exact` 0 of 2 frames different, replay CPU GS stream 9.95 -> 8.14 ms per frame; in the game the GS thread drops 13.4-15.6 -> 10.6-11.6 ms and the
render passes 341-347 -> 198-204. Item 1 (fast scaled raster) is designed but not implemented, see PARITY-PLAN. Switches
`GT4_HW_TONE_HLE` and `GT4_HW_VERIFY_SHUFFLE` (replay `--no-tone-hle`/`--tone-verify`). No PR, no merge, no pins;
owner test 10 pending.

**2026-10-23, OpenCode (parity Plan A):** the presentation-only scaled images are no longer fixed 1024 x 1024 squares:
each covers its slot's real extent (FBW * 64 wide, loaded/drawn rows), is created lazily and grows on demand with the
old content copied, and a texture copy only takes the batch's sampled footprint (IN-380). Scaled image device memory
on the 480p race capture: 184.5 -> 64.1 MB at 2x, 738.2 -> 243.1 MB at 4x, 1,660.9 -> 541.4 MB at 6x. New fixture
`hw_scale_target_extent` (growth, a second FBW); CTest 245/245 both interlock modes, five memory captures
byte-identical, scaled memory = 1x and 0 low-detail frames at 2x/4x/6x. GPU time unchanged (memory win, not a time
win). Next: the fast scaled raster (early-Z second pass) itself, then owner test 11.

**2026-10-27..30, Claude:** CORE2 (one image per target at scale), owner tests through candidate 299 (60 FPS, flat frametime, fidelity perfect), the beta list (IN-427..IN-435: 1x, mipmapping, spike cuts, memory budget, FSR1/Lanczos presentation, filters, 12x), specialised pipelines (IN-437: 12x GPU 33.3 -> 21.4 ms) and the first performance mode item (IN-439, decision 0073). 1.3.0-rc7 (`task/133-rc7-ra-controls` c21e934) merged into the branch as its base on 2026-10-30; CoreGS 2.0 stays out of 1.3.0. Details in STATUS, the journals and `docs/hw-renderer-spec/CORE2-PERF-LEDGER.md`. CPU survey of the GS pipeline and transport (IN-445, 2026-10-30): instruments (process sampler, transport line, exclusive stage times), the list of CPU fronts and a proposed task order in `docs/hw-renderer-spec/CORE2-CPU-SURVEY.md`; SSE4.1 adopted (`GT4_SSE41` ON, -3.8% wall time), AVX2 measured with no further gain (candidate 313). CPU and transport fronts the same night (IN-446..IN-461, candidate 332): lock-free GS stream ring, allocation-free draw path, unchanged uploads keep page versions, speculative early-Z, CT32 column swizzle, texture cache by block identity, CLUT loads by table, quiet VU1 output wakes, and direct GS intake (`GT4_MTVU_DIRECT=1`, TASK-134's P6: the GS worker reads the VU1 output ring itself); against 313 at 3x process CPU -28%, wall -15%, frames identical. Afternoon of 2026-10-30 (candidates 333-337): direct intake on by default (IN-462); the 12x run-to-run differences traced to batches that sample the target they draw into and fixed with spec 8.8's copy (IN-463, CTest `hw_core2_self_texture`; also removes the 3x run-to-run pixels the ledger listed); `task/134-game-code-optimization-study` merged into the branch (documents). Then (candidates 338-347, IN-464..IN-469): GLM hazards by page, statistics query opt-in, disjoint primitives without waits, strip run skip, PGO and ThinLTO of the GS libraries (option, not yet in the release build), VU1 output scan; against 313 at 3x process CPU -37%, frame p50 -29%, frames identical to 337. Handover to TASK-134 in `docs/hw-renderer-spec/CORE2-HANDOVER-TASK134.md`.

**Paused by the owner on 2026-10-15** to focus on release 1.0.0. State on the
integration branch `task/027-hw-renderer`: CPU-verified pure libraries for GS
memory/swizzle (M2), GIF front end and CLUT (M3), exact pixel arithmetic (M5),
display/deinterlace/LOD, texture-replacement keys and pack loading, surface
cache bookkeeping with an authority fuzz (M7 core), per-draw decisions, the
shader toolchain with GPU shaders bit-exact against the CPU reference (42.5 M
blend cases), a CPU reference renderer that replays real GT4 captures (menus
mostly match the donor; garage frame and a 2-line field offset open as
IN-315/IN-316), the M1 comparison tools, and the game hookup (GT4_RENDERER=hw,
black output). WIP branches: task/027-m4-gpuref (GPU write-back path) and
task/027-cpuref. Interpretations awaiting confirmation:
`docs/hw-renderer-spec/IMPLEMENTATION-NOTES.md` on the integration branch.
MIT implementation written only from the TASK-026 specification, behind our GS
front end next to paraLLEl-GS and the donor renderer. Implementers never open
PCSX2 GS sources. Milestones and acceptance come from the specification.

Roadmap beyond parity (owner, 2026-10-21): docs/COREGS2-ROADMAP.md. It covers the separate UI layer, a fractional
render scale, per-target resolution, precompiled pipelines, readback removal, AF and SMAA, FSR 2/3, DLSS and XeSS with
camera motion vectors and jitter, frame generation with TASK-082, effects gated by game state (AO before the UI),
HDR10, texture packs and mobile backends. Every enhancement is opt-in; the faithful image stays the default.

### First stable release
After TASK-019: a tagged GitHub Release of this repository with the NSIS
installer (prebuilt game executable and launcher; the user supplies their own
disc; official branding with a trademark notice; decision 0069), under the MIT license
([decision 0067](docs/decisions/0067-mit-license-and-release-model.md)). TASK-020
is deliberately after it, to give the gt4-decomp project more time to progress
before its names are imported.

### TASK-020 — Decomp names in generated code
Label comments in generated units with identical object code.

**Level 1 delivered (2026-10-18, PR #41, not the whole task).** Read-only name table from gt4-decomp `0b54ed5c`:
`docs/data/ee-names.csv` (11,481 rows: address, name, kind, origin, quality), built by `scripts/ee_symbols.py`; see
[docs/EE-NAMES.md](docs/EE-NAMES.md). No generated EE file, pin or runtime code changed. Still open: labelling `unit_XXXX.cpp`
needs the new producer contract described under "Level 1" in [docs/DECOMP-INTEGRATION.md](docs/DECOMP-INTEGRATION.md).

Common rules for TASK-032..045: agents that open PCSX2 sources or the fork are PCSX2-tainted and never
work on TASK-027. MIT files in this repository never contain PCSX2 code. The fork's own rules: one writer at
a time, commits as `moesuito`, every release built by CI from a tag.

### TASK-032 — Decision 0070 docs
Repo: main, branch `docs/decision-0070-gs-process`. Haiku, 1 session.
Goal: add `docs/decisions/0070-pcsx2-gs-separate-process.md`, `docs/GSDX-PLAN.md`, the CLAUDE.md
clean-room paragraph and renderer lines, GS-RENDERER-OPTIONS update, these TASKS rows, STATUS and journal.
Acceptance: owner answers to GSDX-PLAN section G recorded; links valid; Python suite unchanged.
Parallel: none needed; first.

### TASK-033 — Fork, reproducible build, CI releases
Repo: standalone public repository `moesuito/gt4recomp-gs` (not a GitHub fork: the account already has a fork in
the PCSX2 network, ARMSX2, and GitHub allows one per network). Full PCSX2 history at tag `v2.8.2`
(`fd9d310c`); branch `gt4gs/main` is the default. Haiku, Opus if the dependency build stalls; 2-3 sessions.

**Result (2026-10-15, 3 CI runs, 1 fix).** Details and run URLs in [docs/GS-FORK.md](docs/GS-FORK.md).
- Fork: `gt4gs/main` at `70a0aa66f`; `gt4gs/` docs, trimmed dependency script (Qt removed), packaging script,
  `gt4gs-windows.yml`; upstream workflows set to `workflow_dispatch` only (no file deleted).
- Release `gt4gs-v0.1.0-poc` (prerelease, tag on `3fd7faba8`): zip 18,202,463 bytes, source archive 24,954,987
  bytes, `SHA256SUMS`. Checksums verified after download.
- Deviation: MSVC (VS 18, MSVC 14.51, CMake 4.4.4, Ninja 1.13.2) instead of clang-cl; upstream's CI matrix
  also builds CMake with MSVC.
- Not done: GSRunner smoke with `-renderer null` (no dump in the repo, and game data must not be committed);
  bit-identical local rebuild; build provenance attestation. The first run failed on the VS locator and is kept
  in the run history.
Goal: CMake + Ninja + clang-cl build of the unmodified `pcsx2-gsrunner` (`-DENABLE_QT_UI=OFF
-DENABLE_GSRUNNER=ON`, Vulkan/D3D11/D3D12), dependency script trimmed of Qt and cached, `gt4gs/BUILD.md`,
NOTICE, third-party license list, a release workflow that publishes `gt4-gs-<ver>-win64.zip`, its SHA-256,
a source archive and build provenance.
Acceptance: CI green from a clean runner; release zip contains COPYING.GPLv3, NOTICE, licenses; GSRunner
smoke on CI with `-renderer null` on a tiny dump; local rebuild from the tag on the owner's machine produces
the same file list (bit-identical is a goal, recorded either way).
Risks: dependency build time and breakage; runner disk limits.
Parallel: with TASK-035.

### TASK-034 — Proof of concept: our capture through the PCSX2 GS
Repos: fork branch `gt4gs/gscap2gs` (GPL converter) and main branch `task/034-gs-replay-compare` (MIT
compare script). Haiku, Opus for the GS freeze state; 2-3 sessions.
Goal: `gscap2gs` converts `private/slice175-capture/menu-0-1100.gscap` to a `.gs` dump: header with serial
`SCUS-97328`, freeze state of a freshly reset GS, an 8 KiB privileged block maintained from 'P' records and
emitted as Registers before each VSync, 'G' path 1/2/3 to Path1New/Path2/Path3, 'R' to ReadFIFO2, 'V' to
VSync with alternating field. Stock GSRunner replays it (`-dumpdir`, `-renderer vulkan`, `-upscale 1` and
`4`, `-perf`).
Acceptance: 1,101 frames replayed, no errors; frame count and VSync count match the capture; a MIT script
compares frames 100..1100 with the donor frames in `private/slice175-capture/run-menu/frames` (pct_bad > 8,
PSNR, as in `scripts/compare_frame_dumps.py`) and writes a JSON report and contact sheet; the 4x replay
shows upscaled output; GS-thread ms/frame from `-perf` recorded; GameDB fixes for SCUS-97328 reported as
active in the log. Owner review of side-by-side images.
Risks: freeze-state format (only in the fork); field parity; the donor is not bit-identical to PCSX2, so
the gate is "recognisably the same frame", not equality.
Parallel: with TASK-035 (different repos).

### TASK-035 — MIT GS stream protocol, IPC and renderer seam
Repo: main, branch `task/035-gs-stream`. Opus, 4-6 sessions.
Goal: `native/gs_stream/gs_stream_protocol.h` (plain C, versioned records GIF, PRIV, VSYNC, READ_FIFO,
RESET, CONTROL, reserved STATE and AUX_FRAME; back-channel READY, STATS, ERROR, SNAPSHOT_DONE, EXITING;
control block; written from GS hardware semantics and our `.gscap`, not from PCSX2 code); cross-process SPSC
ring with named events and peer-handle waits; `GT4_RENDERER` dispatch on main with values `donor` (default)
and `pcsx2`, salvaging only the MIT seam from task/012; `GT4_GS_CAPTURE` (generalized `GT4_PGS_CAPTURE`,
capture v2 header); a MIT consumer `gs_stream_sink` (records to `.gscap`, or discards) used as the test peer;
the MTVU journal drain writes to the ring; signals applied on the EE for all paths.
Acceptance: CTest ring fixtures (wrap, split, full ring, peer death, spurious wake, 10^6 random records
byte-identical); protocol round trip; headless 4,200-frame run with `GT4_RENDERER=pcsx2` against the sink:
the sink's capture is byte-identical to the in-process `GT4_GS_CAPTURE` of a `donor` run with the same
schedule; donor frames still 42/42 identical to slice157 with the default renderer; Python suite green;
launcher pins updated.
Risks: ordering between MTVU worker output and EE work; readback blocking with the worker.
Parallel: with TASK-033/034.
State (2026-10-15, slice191, branch `task/035-gs-stream`): built; owner review pending, no PR.
- Protocol `native/gs_stream/gs_stream_protocol.h` (C++20 header, fixed-width structs) and the normative
  [GS-STREAM-PROTOCOL](docs/GS-STREAM-PROTOCOL.md); transport `gs_stream_ipc` (mapping, rings, events with
  waiter flags, peer-handle waits, Job Object spawn); `.gscap` v2 files; game client `gs_stream_client` spliced by
  `prepare_external_vulkan.py --gs-stream-hooks` and `prepare_perf_runtime.py --gs-stream`; consumer
  `tools/gs_stream_record` (recorder or null consumer; readbacks answered with zeros); F8 `[gs-ipc]`.
- CTest 29/29 (`native_gs_stream_ipc`, `native_gs_stream_client` new); Python 321 OK, 12 skipped.
- Headless 4,200 frames: donor 42/42 identical (default and with `GT4_GS_CAPTURE`); `GT4_RENDERER=pcsx2` completes;
  donor capture == pcsx2 in-process capture == recorder file (plus CONTROL shutdown); 619 KB and 390 records per
  frame, 0 readbacks. Converted frames 0-1100 equal the TASK-034 PoC dump byte for byte; GSRunner PNGs 1,100/1,100
  identical to the PoC (300 and 700 included). EE cost in menus: `gs_ipc_write` about 0.08 ms/frame, VSYNC post
  0.004 ms/frame, no back-pressure.
- Not verified: readbacks over IPC in the game (none in the schedule; covered by fixtures only), race scenes,
  window mode (`--mode pcsx2-stream` shows a black window by design), weak CPUs.

### TASK-036 — Live MVP
Repos: fork branch `gt4gs/live`, main branch `task/036-gs-live`. Opus, 4-6 sessions.
Goal: fork adds `pcsx2-gt4gs/` frontend (from GSRunner) producing `gt4-gs.exe`, `GSStreamReplayer` (an R5900
CPU provider reading the ring or a `.gscap` with `--replay-gscap`), handshake, child window in the game
window, VMManager hooks; the protocol header vendored with its hash checked. Main: spawn with Job Object,
handshake, pin check of `gt4-gs.exe`, `play_performance.py --mode pcsx2-gs` with a new pinned manifest,
headless frame dumps via `CONTROL snapshot`.
Acceptance: headless run: live frames equal the offline `--replay-gscap` frames of the same session capture;
boot to menus, Time Trial, race at 1x and 3x; GS crash (killed) ends the game cleanly with a message and a
flushed card; protocol-version mismatch refused; owner plays the reference scene.
Risks: VMManager boot without BIOS in stream mode; first-run shader compile stutter.
Parallel: 037/038/039 start once the MVP boots.
State (2026-10-16, slice193): built; owner windowed test pending, no PR.
- Fork `gt4gs/live` (commits `0b51d4c3c`, `acf4f986e`; tag `gt4gs-v0.2.0-live`): `pcsx2/GSStreamReplayer`, frontend
  `pcsx2-gt4gs` (`gt4-gs.exe`: `--ipc/--parent/--config`, `--replay-gscap`, GSRunner dump mode), child window,
  vendored protocol files checked by SHA-256. Launcher mode is `--mode pcsx2` (not `pcsx2-gs`), with `--gs-scale N`.
- Main: GS process SHA-256 pin (`GT4_GS_PROCESS_SHA256`), `GT4_GS_CONFIG`, `WS_CLIPCHILDREN`, crash message box;
  minimal `gt4-gs.ini` from the launcher; slice193 pins; `tools/gs_live_check`.
- Headless frame dumps come from `gt4-gs.exe` itself (`[GT4GS] DumpDir`), so the game port is unchanged; CONTROL
  snapshot is implemented in `gt4-gs.exe` and used by the window check.
- Checks: CTest 29/29, Python 335 OK; donor 42/42; live 41/41 PNGs = offline replay, 300/700 = PoC; GS kill halts
  the game; hidden-window attach/resize/snapshot/shutdown. The PoC vertical-scale question is the aspect ratio
  (Auto 4:3 gives 480 rows). Not verified: windowed image, races, 480i, readbacks, GPU time.

### TASK-037 — Readbacks and night race
Repos: both, branches `task/037-038-gs-window` (main, shared with TASK-038), `gt4gs/window`. Opus, 2-3 sessions.
Goal: READ_FIFO round trip through `MTGS::InitAndReadFIFO`; `HWDownloadMode` and readback spin options
exposed; F8 counts and wait time; Unsynchronized mode served without draining the ring (if the owner allows).
Acceptance: CTest/fixture with a scripted stream (draw, TRXDIR=1, read) compared with the donor bytes;
headless frames unchanged; owner night race at 60 FPS with `gs_ipc_readback_wait` reported; lens flare and
night lighting correct versus CoreGS 1.1.
Risks: GPU sync cost at high upscale.
Parallel: with 038/039.
State (2026-10-16, slice194): built; owner night-race test pending, no PR.
- Cause of the TASK-036 night-race late frames (213/7,467): one readback per frame (1.07 ms each) removes the GS
  buffering, so GS-thread spikes (pipeline compiles at race start) land in the game frame; the shader cache was in
  `Documents\PCSX2`; the MTVU `gs_privileged` wait (3.4 ms/frame) leaves about 3.7 ms of headroom.
- `GT4_GS_READBACK=accurate|unsynchronized` (`--gs-readback`), F8 readback histograms, `gt4-gs.exe` readback and
  acquire timing, own cache/data folders, deferred swap chain acquire (A/B `GT4_GS_EAGER_ACQUIRE=1`).
- Checks: readback fixture exact in accurate mode (drawn sprite and uploaded bytes); headless frames unchanged;
  cold vs warm cache replay (max GS frame 84 ms -> 6 ms).
- Owner night race (slice194): accurate 59.97 FPS, 69 late frames (was 213), readback mean 0.78 ms; unsynchronized
  60.00 FPS, 1 late frame, but headlight flares missing. Owner chose accurate as the default; unsynchronized stays
  an option ("faster, may drop effects").

### TASK-038 — Window, fullscreen, focus, input, lifecycle
Repos: both, branches `task/037-038-gs-window` (main, shared with TASK-037), `gt4gs/window`. Opus, 3-4 sessions.
Goal: child window hardening (own pump thread, `HTTRANSPARENT`, `MA_NOACTIVATE`), Alt+Enter/F11 borderless,
resize, minimize, DPI, multi-monitor, focus loss; GS hotkeys forwarded (screenshot, OSD toggle); fallback
option 2 (GS-owned window, keyboard forwarding) behind a switch.
Acceptance: Python/CTest for the message contract; a scripted soak (resize and fullscreen toggles, 1,000
cycles) with no hang; a GS-thread stall test does not freeze our window beyond the stall; RTSS overlay
visible; owner checklist (fullscreen, alt-tab, minimize, controller and keyboard).
Risks: attached input queues; DWM latency in child windows.
Parallel: with 037/039.
State (2026-10-16, slice194): built; owner checklist pending, no PR.
- Input queues detached after SetParent; click on the picture brings the game forward; minimized (0x0) resizes
  ignored; WM_CLOSE in pcsx2 mode halts before the window is destroyed; GS death: 180 grace VSYNCs (card flush),
  restart offer with CoreGS 1.1 (launcher relaunches with `GT4_RENDERER=donor`); handshake timeout message.
- Checks: stall fixture (3 s GS window stall, parent answers within 17 ms), soak 3,000 resizes, aspect check (same
  rectangles as TASK-009), CTest handshake-timeout / game-killed / grace scenarios.
- Not done: own pump thread for the child (the main thread only pumps), GS hotkeys, fallback option 2, DPI and
  multi-monitor checks, RTSS.
- slice195 (2026-10-16): owner found that a click in the picture deactivated the game window (detached queues make
  the GS thread the foreground thread). gt4-gs 0.3.1 gives the foreground back after every click; the game keeps the
  keyboard focus on activation. Visible click fixture passes (0.3.0 fails). Owner click/focus check pending.

### TASK-039 — Settings table and `gt4-gs.ini`
Follow-up (2026-10-16, `fix/duplicate-gs-json-settings`): reject duplicate JSON setting ids consistently
with CLI assignments. Module validation: 58 tests pass; native/gameplay validation not performed.
Repos: main `task/039-gs-settings`, fork `gt4gs/config`. Haiku, 2-3 sessions.
Goal: `docs/launcher/gs-settings.toml` (GSDX-PLAN D.1), generator to `gt4-gs.ini` (dev script now, launcher
later), allow-list validation in `gt4-gs.exe`, folders under the user data directory, `CONTROL
reload_config`.
Acceptance: Python tests (table schema, generated INI golden file, refusal of unknown keys and out-of-range
values); `gt4-gs.exe` reports effective settings in READY; owner checks 1x/3x/6x, FXAA, CAS, dithering,
deinterlace in menus.
Parallel: with 037/038.
State: ✅ Done (main-repo part); fork allow-list enforcement pending (TASK-037/038 agent). All rows resolved. Branch `task/039-gs-settings` (2026-10-16). Details: [GS-SETTINGS](docs/GS-SETTINGS.md).

### TASK-040 — Texture dumping and replacement (folded into TASK-050, release 1.1.0)
Repos: fork `gt4gs/textures`, main docs. Haiku, 1-2 sessions.
Goal: folder layout `...\gs\textures\SCUS-97328\{replacements,dumps}` and `.zip` packs; dump, load, async,
precache options; user documentation.
Acceptance: scripted test: dump from a replayed capture, replace one texture with a solid colour, replay,
detect it in the frame; owner tries a community GT4 pack if available.
Parallel: after 039.

### TASK-041 — Performance
Repos: both, branches `task/041-gs-perf`, `gt4gs/perf`. Haiku for counters, Opus for tuning; 3-4 sessions.
Goal: F8 `[gs-ipc]` section (game: write, back-pressure, vsync post, readback wait, bytes, records; GS:
thread busy, GPU ms, draws, uploads, readbacks, present wait, ring occupancy); tuning: frames in flight,
ring size, direct GS-thread feed; `--cpu-cores 2` check.
Acceptance: `perf_probe_fixture` covers the new categories; owner A/B in showroom, night race, rally,
reference scene: CoreGS 1.1 vs PCSX2 GS at 1x and 3x, 60 FPS, flat frametime, no measurements while builds run.
Parallel: after 037.
State (2026-10-16, slice198, branch `task/041-gs-perf`; fork unchanged, `gt4gs-v0.3.2`): built and checked headless;
owner night-race A/B pending, no PR.
- Headless race schedule added (Arcade, El Capitan, 5 AI, 16,000 frames): readbacks with the night-race pattern.
- Night-race `gs_privileged` wait (3.48 ms/frame): 8 privileged writes per frame = 5 at the VBlank (PMODE, SMODE2,
  DISPFB2, DISPLAY2, BGCOLOR) + CSR and BUSDIR x2 per readback; the wait sits before the CSR write (the EE waits for
  the frame's VU1 work right before the flare readback). `GT4_MTVU_PRIV_ASYNC` (default 1) queues privileged writes
  in the MTVU journal: stream bytes identical (menus captures, race stream hash with 588 queued writes), frames
  identical; the gs_privileged wait becomes 0 but moves to the download sync of the readback (no net gain expected
  in the night race).
- Readback tail: long waits are pipeline compiles (cold cache: 12 of 294 at 8 ms or more, max 58 ms; warm: none, max
  2.7 ms); a GS-side spin does not help (fork unchanged); fix is TASK-046.
- Stream: 322 GS wake-ups per frame -> 11-14 with `GT4_GS_WAKE_BYTES` (default 256 KiB); `gs_ipc_write` about -30%
  per MB; one copy less per worker GIF record in the MTVU journal.
- F8 `[gs-ipc]`: privileged writes and MTVU waits by register, GS wake-ups, GS CPU thread per interval, one-line EE
  critical path summary against the 16.68 ms budget.
- Menus, same build: EE CPU donor 7.6 vs pcsx2 7.2 ms/frame. CTest 29/29, Python 416 OK, donor 42/42, pcsx2 41/41.
  Details: [GS-LIVE](docs/GS-LIVE.md) section 9.

### TASK-042 — JIT-free `gt4-gs.exe`
Repos: both, branches `gt4gs/nojit` (fork, from `gt4gs/main` `660017bb1`) and `task/042-gs-nojit` (main, from `a1a06a4`).
Opus, 1 session.
Goal: `GT4GS_ONLY` build without EE/IOP/VU recompilers, without RWX code mapping, with the C draw-scanline
path or CPU sprite/CLUT render refused.
Acceptance: binary scan finds no recompiler symbols; a VirtualAlloc/VirtualProtect audit shows no executable
private memory; PoC and live frames unchanged; GSRunner tests still pass.
Parallel: with 037-041 (fork-only).
State (2026-10-16, slice197): built and published (`gt4gs-v0.4.0`, zip sha256 `5c92ad00...`); owner windowed check
pending, no PR.
- Fork option `GT4GS_ONLY`: no EE/IOP/VU/VIF recompilers, no x86 emitter, no GS software JIT (C rasterizer for CPU
  sprite/CLUT render and the software renderer; no setting refused), no code area, executable protections refused;
  Windows Arbitrary Code Guard on at start (works with the AMD Vulkan driver); executable-memory audit in the log.
- Static check `tools/gs_nojit_check/check-nojit.ps1` (fork `gt4gs/check-nojit.ps1`, run by the release script): 0.4.0
  passes, 0.3.2 shows 22 markers; `tests/python/test_gs_nojit.py` repeats it on the pinned package.
- Offline menus replays (slice191 and slice196 streams, 1x and 3x): frames byte-identical to 0.3.2; also with CPU
  sprite and CLUT render on. Live headless pcsx2 1x 41/41 = slice196; donor 42/42. GS thread time unchanged within
  noise. `--mode pcsx2` = slice197 (same game runtime as slice196), `--mode pcsx2-slice196` kept.
  Details [GS-LIVE](docs/GS-LIVE.md) section 9.

### TASK-043 — Launcher integration
Repo: main, branch `task/019-launcher` (when the launcher resumes). Haiku, 2-3 sessions.
State (2026-10-20): in progress on `task/019-launcher`. Code: `launcher/crates/gt4l-core/src/{releases,update,version,versions,guard,migrate,gs_release}.rs`
and `launcher/crates/gt4l-tauri/src/{updates,tray}.rs`. Stable releases only (drafts and prereleases ignored); the
renderer minimum is gt4-gs 1.0.1; the install is verified against `SHA256SUMS` and staged as before. Checks: fixtures
for both repositories (no network), SemVer, update decision, version.json, migrations, data-path guard, tray state machine.
Still open: the owner's live test against GitHub, and a real NSIS build (the installer was edited, not built here).
Goal: renderer manifest (version, URL, size, SHA-256, protocol version, upstream commit) in the launcher;
download with progress and resume, verify, unpack to the user data folder, update to newer pinned versions,
offline fallback to CoreGS 1.1; settings pages generated from the table; license/source links shown.
Acceptance: Rust tests (hash mismatch refused, partial download resumed, wrong protocol refused, fallback);
owner end-to-end install.

### TASK-044 — Packaging and release notes
Repos: both. Haiku, 1-2 sessions.
Goal: fork release `gt4-gs v1.0.0` (notes: upstream commit, our patch list, license, source); main: README
renderer section, `native/THIRD_PARTY.md`, RELEASE-1.0.0-PLAN renderer table and W8, known issues.
Acceptance: release assets complete; links valid; owner approval.
Note (2026-10-16): `gt4gs-v1.0.0` is published (stable) and pinned in slice202. Known issue for the release notes: gt4-gs 1.0.0 writes under `Documents\PCSX2`: `VMManager::Internal::LoadStartupSettings()` runs before `[GT4GS] DataRoot` is applied, so it rewrites `Documents\PCSX2\logs\emulog.txt` every session (Confirmed: before/after snapshot of the folder) and creates the PCSX2 user folders there (bios, memcards, logs, ...) on a PC that has none (High confidence, `EmuFolders::EnsureFoldersExist`). The log line "MemoryCards Directory: ...Documents\PCSX2\memcards" is that early pass; the session itself uses DataRoot. Fix on fork branch `gt4gs/datadir` (commit `b30870fe8`, version 1.0.1, NOT released): the call moves after the stream folders; checked: 0 changes under `Documents\PCSX2`, emulog in DataRoot\logs.
Decided (2026-10-17): release 1.0.0 ships gt4-gs 1.0.1 (see Result below).
Note (2026-10-16, TASK-044): gt4-gs 1.0.1 is published (stable, tag `gt4gs-v1.0.1`, fork PR #7 merged) and pinned in
slice203 (`--mode pcsx2`). The `Documents\PCSX2` known issue is fixed in 1.0.1: no changes in the `Documents\PCSX2` snapshot for a
live run or an offline replay (slice203, TASK-044). The clean-profile check was not run (see BUILD.md slice 203).
Result (2026-10-17, docs only; no build, no game run): **decision: release 1.0.0 ships gt4-gs 1.0.1** (`gt4gs-v1.0.1`,
pinned in slice203 and slice206, decision 0070). The gt4-gs 1.0.0 release stays as history. The fork release notes of
`gt4gs-v1.0.1` were edited (upstream base PCSX2 v2.8.2, commit `fd9d310cc`; patch list; GPL-3.0 and source; SHA256SUMS);
assets unchanged. README renderer section, known issues and legal notes updated; `native/THIRD_PARTY.md` rewritten;
RELEASE-1.0.0-PLAN renderer table and W8 updated. Release order: this task, then the launcher and installer (TASK-019/043),
then the owner's end-to-end test, then the v1.0.0 tag and GitHub Release. Mods (TASK-048, 050-057) are release 1.1.0 and
appear only in this list.

### TASK-045 — High frame rate on the PCSX2 GS
Repos: both. Opus, 10-14 sessions, after release 1.0.0.
Plan: (1) D1: VRR and present-mode tests, no interpolation (1 session). (2) Oracle on the PCSX2 GS: the MIT
`tools/gs_interp` builds interpolated and extrapolated frames as streams; replay them with `gt4-gs.exe
--replay-gscap` and compare with real frames (race capture needed; kill if matched primitives < 95% or
race PSNR < 32 dB) (2-3 sessions). (3) In engine: the game holds frame N+1, generates the in-between stream
(MIT, game side) and sends it as `AUX_FRAME` before N+1; the GS renders and presents it without guest-visible
side effects (readbacks and signals stripped; targets overwritten by N+1); present scheduler for 120/144 Hz;
+1 frame latency measured (6-10 sessions).
Risks: texture-cache pollution by auxiliary frames, page alternation (HIGH-FPS 9.3), HUD.

### TASK-058 — Crash-safe memory card in the real build
Repo: worktree `C:\Antigravity\gt4-wt-058-harden`, branch `task/058-059-release-hardening` (from main `3b5334d`). Release 1.0.0 hardening, game side. One writer.
Goal: the runtime writes the card through the atomic writer of TASK-025 Part A (temporary file, flush, rename), keeps a failed write dirty and retries, and flushes a pending save on exit, including the pcsx2-mode GS-process loss path after its grace VSYNCs.
Result (slice200, docs/BUILD.md): TASK-025 Part A was on `task/025-save-manager`, not on main, and is imported; memcard capsule `private/slice200-memcard-runtime` (derived `iop/sio2.cpp`, pinned by `runtime.json`); CMake `GT4_MEMCARD_RUNTIME_SOURCE`; `flush_cards_now()` after `emulate()`. CTest 29/29 (`native_memcard_store`), Python kill test (25 kills of a copy: old or new image, never torn). The headless menus schedule never saves: the card of the donor and pcsx2 runs equals the seed, so the byte comparison of a saved card is not available headless; the in-game save check is the owner's test.

### TASK-059 — Distribution executable
Repo: same worktree and branch as TASK-058. Haiku, 1 session.
Goal: an install folder that runs with no repository path: `<exe folder>\shaders` when present (else the build-time folder), the gt4-gs package at `<exe folder>\gs\gt4-gs.exe` or `GT4_GS_PROCESS`, no build path embedded except the development fallback, a `--portable-check` self-test, and `scripts/make_distribution.py` for the folder (no gt4-gs package, no game data; GPL kept separate, decision 0070).
Result (slice200, docs/DISTRIBUTION.md): the shader change of `task/019-launcher` ported (its printf string repaired); `native/install_paths.*`; port `private/slice200-port` with `--portable-check`; `make_distribution.py` wrote `private/slice200-dist` (16 files, 93 MB); the exe holds one build path (the build-time shader fallback). Smoke from that folder, headless: donor 42/42 frames identical to slice157; pcsx2 41/41 identical to slice199 (gt4-gs 0.4.0 via `GT4_GS_PROCESS`, ACG on). Tests: `test_portable_install.py`, `test_make_distribution.py`.
Open: the release build should drop the build-time shader fallback; `--mode main` is not rebuilt (its build is not shared); the owner's disc is not copied into the worktree.

### TASK-047 — PCSX2 GS 480i deinterlace
Repos: both, branches `task/047-gs-deinterlace` (main, from `6806f78`) and `gt4gs/deinterlace` (fork). Opus, 1 session.
Goal: the 480i menus look like real PCSX2 with deinterlace Automatic (owner report 2026-10-16: soft and flickering
in gt4-gs; races at 480p fine).
Acceptance: root cause with evidence; headless before/after menu frames with a comb and flicker metric; progressive
output unchanged; CTest, Python, donor 42/42; published `gt4gs-v0.3.2` pinned; owner check of the menus.
State (2026-10-16, slice196): built; owner check pending, no PR.
- Cause (Confirmed): the stream never carried SYNCV (the BIOS writes it; the donor's SetGsCrt HLE does not), so
  PCSX2's GS treated the 480i output as not interlaced and showed each half-height field line-doubled, bouncing one
  line per frame. With SYNCV alone the fields were woven in the wrong order: gt4-gs used the previous VSYNC's field,
  but the donor flips FIELD before GT4's VBLANK_S handler (`0x004AB430`) reads it, one field earlier than hardware.
- Fix: SetGsCrt posts SYNCV for NTSC/PAL; protocol 1.1 defines VSYNC `field` as the display field (same values);
  gt4-gs 0.3.2 presents with the record's own field. `--mode pcsx2` = slice196, `--mode pcsx2-slice195` kept.
- Static Garage menu, frames 1000-1199: slice195 comb 0.50, flicker 3.23; SYNCV with the old field comb 1.00
  (combing); slice196 comb 0.57, flicker 0. Progressive control identical. Samples:
  `private/slice196-deinterlace-samples`; details [GS-LIVE](docs/GS-LIVE.md) section 8.

### TASK-048 — Mod manager in the launcher (umbrella)
Owner, 2026-10-21: gt4-gs is frozen apart from this; texture replacement is the one feature still to switch on there
(the PCSX2 GS implementation exists; enable it and expose it in the launcher).
Plan: [MOD-MANAGER-PLAN](docs/launcher/MOD-MANAGER-PLAN.md).
Goal: a Mods page where users install, enable, order and remove mods; texture packs for the PCSX2 GS first.
Texture replacement is not a graphics setting: the launcher writes the GS keys from the enabled mods.
Children: TASK-050..057. Legal: users bring their own packs; we never host or download packs.
State: plan written 2026-10-16; owner answered all ten questions the same day (see below); release 1.1.0.

### TASK-050 — Texture replacement contract and headless round trip
Repos: main, branch `task/050-texture-contract` (from `main`); fork `gt4gs/textures` only if a summary log line is
needed. Haiku, 1-2 sessions. PCSX2-tainted agent (never TASK-027).
Goal: confirm the PCSX2 v2.8.2 replacement contract on our stream and correct GSDX-PLAN D.3 (no zip packs;
duplicates resolved by enumeration order; PNG only 8-bit RGB/RGBA; `-mipN` files ignored; `[Folders] Textures`
relative to `DataRoot`). Measure map scan time, RAM (with and without precache) and VRAM on a synthetic
10,000-file pack.
Acceptance:
- Python test (skipped without the private capture): replay twice with dumping on, identical dump name sets;
  solid-magenta replacement of a texture used in frame K appears in frame K (`Async = false`) and within 2 vsyncs
  with `Async = true`; one-digit-changed name, `-mip1` name and empty view leave frames byte-identical to the
  baseline; duplicate-key rule recorded.
- Name-parser fixture built from real dump names (all parse; round trip name → key → name).
- Measurements recorded in GS-LIVE or a new `docs/GS-TEXTURES.md`; D.3 corrected; `gs-settings.toml` texture rows
  get their sources.
- Optional: name-set overlap ≥ 95% against a PCSX2 v2.8.2 dump of the same menu scene; owner tries a community
  pack if available.
Risks: hash-relevant settings (`HWMipmap`, `TriFilter`, `UserHacks_EstimateTextureRegion`) differ from a pack
author's; the capture may not contain the texture chosen (pick from the dump set itself).

### TASK-051 — `gt4mods` core: manifest, detection, validation, import, store
Repo: main, branch `task/051-mods-core` cut from `task/019-launcher` (the launcher workspace lives there); new
UI-free crate `launcher/crates/gt4mods`. Haiku, 3 sessions; Opus reviews the validation code (plan 5.2).
Goal: `gt4mod.toml` schema 1 and `files.lock.json`; import from zip or folder with or without a manifest;
detection of raw PCSX2 layouts (`SCUS-97328/replacements`, bare `replacements`, name-majority folders); texture
key parser (all five name forms); PNG/DDS validation; staging and atomic commit into `mods\store\<id>\`;
registry `mods.toml` written with temp + `ReplaceFileW`; operation journal and recovery at start.
Acceptance: Rust tests with synthetic zips: zip-slip (`..`, absolute, drive, ADS, device names, case
collisions), zip bomb (ratio, total, entries, lying headers), symlink and encrypted entries refused; PNG 16-bit,
paletted and interlaced detected; DDS block-size and truncated data detected; within-pack duplicate rule; kill
between steps leaves a recoverable state; update of an installed id keeps enabled state and order.
Risks: very large packs (streaming hash and extraction, progress and cancel); non-ASCII and long paths.

### TASK-052 — Active view, priority, conflicts, uninstall, CLI
Repo: main, branch `task/052-mods-view` (after TASK-051, same base). Haiku, 2 sessions.
Goal: build `mods\active\textures\SCUS-97328\replacements\` with one hardlink per winning texture key (top of
the list wins), copy fallback when hardlinks fail; view digest; atomic swap (`active.new` → `active`); conflict
report per mod; enable/disable/move without reinstall; uninstall to the Recycle Bin with the quota check; a
`gt4mods` CLI (`list`, `import`, `enable`, `disable`, `move`, `remove`, `build-view`, `report`) for tests and
the dev launcher.
Acceptance: Rust tests: each key exactly once in the view; priority changes switch the winner; identical-content
collisions not reported; view rebuilt only when the digest changes; refused while the game lock is held;
tampered view detected and rebuilt; copy fallback on a non-NTFS temp volume (or simulated failure); Recycle Bin
path covered by a fake platform layer. Manual: 10,000-file view build time recorded.
Risks: hardlink limits (1,023 links per file, irrelevant here); antivirus scanning many new links.

### TASK-053 — GS settings and dev launcher wiring for mods
Repo: main, branch `task/053-mods-gs-wiring` (from `main`). Haiku, 1 session.
Goal: in `docs/launcher/gs-settings.toml` move `texture_load`, `texture_load_async`, `texture_precache` off the
Graphics page (`page = "Mods"`, `visibility = "hidden"`, `written = "launcher"`), move the dump rows to a creator
group, add `textures_folder` (`[Folders] Textures`, `path`, launcher, absolute); `scripts/gs_settings.py`
accepts the new page; `external_runtime_probe.py` / `play_performance.py --textures-root <dir>` writes the
folder and `LoadTextureReplacements = true` when the folder has files; `result.json` records the folder digest.
Acceptance: Python tests: golden INI unchanged without the option; with the option, the three keys and the
folder appear; relative paths refused; UTF-8 path with non-ASCII characters round trips; allow-list regenerated;
pins regenerated where the probe changes. Owner: one in-game run with a synthetic or community pack.
Risks: gt4-gs pin and allow-list drift (TASK-037/038 enforcement).

### TASK-054 — Launcher Mods page
Repo: launcher branch after TASK-019 resumes (Tauri 2). Haiku, 2-3 sessions.
Goal: `mockup-mods.html` (states: empty, list, details, import progress, conflict, remove confirm, wrong
renderer) for owner approval, then the Tauri page and commands (`mods_list`, `mods_import`, `mods_set_enabled`,
`mods_move`, `mods_remove`, `mods_details`, progress and cancel events); drag and drop of zips; Inbox folder
import at start; controller navigation (D-pad focus, Cross toggle, Triangle details, L1/R1 priority, Square
remove, Start install help, Circle back); the view is rebuilt before Play and its digest written to `launch.json`.
Acceptance: owner approves the mockup; Rust command tests; UI test of keyboard and XInput focus order; owner
installs, reorders and removes a pack end to end.
Risks: WebView file drop and CSP; long imports must not block the UI thread.

### TASK-055 — Mod creator tools
Repos: launcher branch (UI and packaging), main docs. Haiku, 1-2 sessions.
Goal: collapsed "Creator" section: "Dump textures" for the next Play (folder `mods\creator\textures`, dump keys
written by the launcher, FMV off), open dumps and work folders, "Package as mod" (zip with `gt4mod.toml`,
validated by the TASK-051 importer), "Clear dumps" (Recycle Bin); `docs/MODDING.md` for pack authors (names,
formats, alpha scale, BC7 with mips advice, hash-relevant settings).
Acceptance: a dump session produces PNGs in the creator folder only; a packaged work folder re-imports cleanly;
docs reviewed by the owner.
Risks: dumping cost in races (documented banner).

### TASK-056 — Live texture reload in gt4-gs (after 1.0.0)
Repos: fork `gt4gs/texture-reload`, main. Haiku, 1-2 sessions.
Goal: a control message (`reload_textures`) that calls the replacement map reload on the GS thread, and a game
window key that sends it, so creators see edits without restarting.
Acceptance: headless test: replacement added during a replay appears after the message; no leak across reloads.
Risks: protocol version bump; reload stalls (sync worker thread wait).

### TASK-057 — Study: other mod kinds (after 1.0.0)
Repo: main docs only. Opus, 1 session.
Goal: decide on `gs-preset` mods (bundles of `gs-settings.toml` rows), a read-only `GT4.VOL` overlay for data mods
(cars, tracks, menus; Spec II idea), and RAM data patches; code patches stay out (AOT EE).
Acceptance: a decision record with risks and effort; no code.

Release: 1.1.0. Owner decisions (2026-10-16)

1. The mod manager is **not in release 1.0.0**; it is planned for **1.1.0**. It is tracked in TASKS.md only
   (not in the README task table). The launcher's Mods tab stays hidden in 1.0.0.
2. Priority: the mod higher in the list wins a texture conflict.
3. TASK-040 is folded into TASK-050.
4. The "precache all textures" toggle is hidden (off by default; RAM risk).
5. PAL-version packs are allowed with a warning.
6. PNGs in an unsupported format: the launcher shows a warning and asks whether to convert them to 8-bit.
7. Third-party pack catalogue: not now, but planned later (we never host packs ourselves).
8. Limits: 20 GiB per pack; zip and loose-folder import.
9. Creator texture dumps are taken with mods off, from a creator option in the Advanced settings.
10. A pack too big for the Recycle Bin: ask clearly, then delete permanently only after on-screen confirmation.

### TASK-060 — Cursor auto-hide
Repos: both, branches `task/060-062-osd` (main, from `3b5334d`) and `gt4gs/osd` (fork, from `0e40edc92`). Opus, 1 session
(with 061 and 062).
Goal: hide the mouse cursor after about 3 s without movement, in fullscreen and in windowed mode only over the picture;
show it on any movement; never touch it elsewhere; the game window and the gt4-gs child window (another process) agree.
`GT4_CURSOR_AUTOHIDE=0` switch (default on) and an Advanced-page row.
Acceptance: CTest of the rule; a visible tiny fixture with the real child (move, hold, assert visibility); owner check.
State: ✅ done, owner test 2026-10-16 (fullscreen and windowed); merged with main as slice202.
- `native/cursor_autohide.*` (pure rule, CTest `native_cursor_autohide`), used by `window.cpp`; protocol 1.2 CONTROL
  cursor carries the policy to gt4-gs 1.0.0, whose window thread applies the same rule and answers `WM_SETCURSOR`.
- `tools/gs_live_check --cursor-check` (visible, about 1.7 s) passes; details [GS-LIVE](docs/GS-LIVE.md) section 11.1.

### TASK-061 — "Compiling shaders…" notice
Repos: fork (`gt4gs/osd`) and main (settings row). Same session.
Goal: plain text "Compiling shaders…" bottom-left while the PCSX2 GS really compiles shaders or pipelines (not cache
loads), gone when compiling stops; reuse PCSX2's compile signal; on by default with an Advanced-page row.
State: ✅ done, owner test 2026-10-16 (gt4-gs 1.0.0). `GSShaderCompileIndicator` plus a real-compile timestamp;
pipelines from the driver cache are excluded with `VK_EXT_pipeline_creation_feedback`; shown until 0.5 s after the last
compile. Cold cache 238 compiles (notice drawn), warm cache 0 compiles and 138 cache hits (no notice). [GS-LIVE](docs/GS-LIVE.md) 11.2.

### TASK-062 — Performance overlay
Repos: both. Same session.
Goal: PCSX2-OSD-like overlay, top-right, each item switchable: FPS, frame time, CPU and GPU usage of the game only (game
process plus gt4-gs), output and internal resolution, per-part ms per frame and the headroom against 16.67 ms (the F8
summary, live, about twice per second); off by default; negligible overhead (measured).
State: ✅ done, owner test 2026-10-16. Game numbers in protocol 1.2 OSD_STATS records (sent only when
READY asks for them); CPU via GetProcessTimes of both processes, GPU via PDH "GPU Engine" counters of both pids; drawn
with the PCSX2 ImGui OSD. Overhead: 0.35 us/frame on the EE thread, 12-15 us/frame of ImGui text and +0.02 ms/frame of GS
thread time. Rows `perf_overlay` (master) and one per item. [GS-LIVE](docs/GS-LIVE.md) 11.3.

Settings and stable renderer (same branch): internal resolution presets 480p (1x), 720p (2x), 1080p (3x), 1440p (4x),
4K (6x), 8K (12x) only (default 1080p; launcher rules for the monitor height and the GPU limit documented in
[GS-SETTINGS](docs/GS-SETTINGS.md)); `--gs-resolution`; gt4-gs 1.0.0 ignores INI keys and values outside the allow-list
with a warning; **`gt4gs-v1.0.0` published as the first stable release** and pinned by slice201.

### TASK-049 - GS settings curation for the launcher
Done (2026-10-16). Owner decisions applied to `docs/launcher/gs-settings.toml` (58 rows, `page`, `order`, `control`,
`gt4-defaults` profile): Graphics page = renderer choice (PCSX2 GS or CoreGS 1.1), internal resolution slider 1 to 12,
aspect, FXAA, CAS mode and sharpness; Advanced page = 36 rows pre-configured to the GT4 ideal; texture rows on `mods`.
Findings (PCSX2 v2.8.2, [GS-SETTINGS](docs/GS-SETTINGS.md)): FSR 1.0 does not exist in v2.8.2 (CAS only);
12x needs no `ExtendedUpscalingMultipliers` (the device limit is `max texture size / 1280`); READY does not carry the
device limit yet; widescreen patches are pnach EE patches and are not exposed. `reset_to_defaults()` reproduces the
default gt4-gs.ini byte for byte. Default gt4-gs.ini unchanged. The `mipmap` key is now `hw_mipmap`.

## Phase 2 — Android details

### TASK-021 — Mobile GS (ARMSX2 pipeline, Vulkan)
Bring the ARMSX2 GS pipeline (PCSX2 hardware renderer with ARMSX2's Android
changes: GPU driver profiles, tile-friendly pass scheduler, framebuffer-fetch
policy) behind our GS front end. ARMSX2's ARM64 JIT is not used (no JIT rule).
Owner's ARMSX2 checkout: `C:/Antigravity/armsx2` (upstream `ARMSX2/ARMSX2`).

### TASK-022 — Android app
ARM64 build of the AOT EE/VU code and runtime (fp-contract off), Android host
layers (window, audio, input, storage, lifecycle), on-screen controls that hide
when a Bluetooth controller connects; target Galaxy S23 Ultra.

## Phase 3 — iOS details

### TASK-023 — Metal GS (ARMSX2)
ARMSX2's Metal backend for the same GS pipeline.

### TASK-024 — iOS app
ARM64 build, iOS host layers, signing/installation on iPhone 15 Pro Max and
17 Pro Max (built on a Mac with Xcode).

## Ideas (not scheduled)

LAN-over-internet and an online lobby launcher (docs/ONLINE-NOTES.md), in-game
settings menus.

**Two-player split screen (open, not numbered; owner, 2026-10-24).** Make GT4's own two-player split-screen mode
(Arcade "2P Battle", `RaceSplitBattle`) work properly with two controllers. Not a priority; map it before any work.
What we know today:
- Input: the host feeds only player one. `native/host_io.cpp` polls one XInput slot (the first connected) and SDL3
  opens one gamepad (`gt4::pad::first_pad`); the guest's second pad port gets nothing. Needs a second guest pad
  (port 2) fed from a second controller, plus launcher Controls support (player 1 / player 2 assignment, a keyboard
  layout for player 2 or keyboard + pad).
- Rendering: split screen has its own path (`RacePS2Base::render_world_split` 0x0034028C, `func_0033EAB8` second
  view; docs/GS-READBACK-STUDY.md, docs/NATIVE-RENDERER-STUDY.md). In split screen the headlight flares use a constant
  (only the sun uses the GS readback), so its readback cost is lower.
- Ultrawide: split screen stays pillarboxed 16:9 today (TASK-080 scene policy); a widened split view is a later
  question.
- Owner checks wanted: both players drive, both HUDs right, performance at 60 FPS on both renderers, Spec II too.

**Release plan (owner, 2026-10-22):** big features take a minor version, intermediate updates and hotfixes the
patch number: 1.2.3 hotfix, 1.2.4 (TASK-097, 098, 099), 1.3.0 Spec II (TASK-100, on GSdx), 1.3.1 native DFP wheels (TASK-018; owner update 2026-10-26);
2.0.0 is CoreGS 2.0 (phase 2).

**Spec II is a second target** (owner, 2026-10-22): support means the SCUS-97436
Public Beta lineage ISO with its own target executable, not a content merge into
v2.00. Recorded, not scheduled: [decision 0072](docs/decisions/0072-spec2-second-target.md).
The asset-merge variant is closed by the 2026-10-22 file study (same audio and
art already in v2.00; menus are Adhoc v7 against v5).

A decompilation-based source port (native renderer, changing the game's physics
timing, remaster) is out of scope for this repository and would be a
separate project, possibly a fork. Presenting more than 60 FPS with the game's
fixed 1/60 s physics untouched is in scope (TASK-031, owner 2026-10-15). Ultrawide and other aspect ratios
are in scope (TASK-080, owner 2026-10-21).

### TASK-031 — High frame rate (physics untouched)
Dropped in this form (decision 0070): redone as TASK-045 on the PCSX2 GS after 1.0.0. Study docs stay on branch `task/031-high-fps`.
Goal: present 120-144 FPS while the game's physics keeps its fixed 1/60 s step
(`RaceBase__updateRace` uses a constant dt; read-only study, 2026-10-15).
Approaches: (A) GS primitive interpolation between two guest frames, with real
rasterised in-between frames and no optical flow or frame generation. It costs
about half a guest frame of latency, and an extrapolation variant is measured
alongside. (B/C) A native render rate: re-run the game's render path (EE scene
submission and VU1) with interpolated car and camera transforms, via decomp
hooks. Step 1: an offline oracle that builds frame N from captured frames N-1
and N+1 (and an extrapolated frame from N-2 and N-1) and compares it with the
real frame N; kill if fewer than 95% of primitives match or race PSNR is below
32 dB. In parallel, a read-only study of the native render-rate path: main
loop, update/render split, transforms and render cost. Input-latency trims
(submit ring depth, present mode) are measured with the prototype.

### TASK-034 — PCSX2 GS proof of concept (offline replay)
Branch `task/034-gs-replay-compare`. Goal: prove that the PCSX2 GS (decision 0070, separate GPL process) renders
GT4 correctly from our GS stream, offline. Done: `tools/gscap_to_gsdump/` converts the menu capture to a `.gs`
dump (23 unit tests); stock `pcsx2-gsrunner` built from PCSX2 `685637d` replays it at 1x and 3x without errors
(1,100 frames each); the GameDB fixes for SCUS-97328 apply from the serial. Frames 300 and 700 match the reference
exactly; frames 900 and 1100 do not, and the PCSX2 output is 480 rows against the references' 448.
Recommendation: GO for TASK-033 and TASK-035; conditional GO for TASK-036 (open: the vertical scale, the 900/1100
differences, a freeze state). Details and numbers: [docs/GS-PCSX2-POC.md](docs/GS-PCSX2-POC.md). Clean room:
PCSX2-tainted work, not touching CoreGS 2.0. Owner review: the side-by-side images and frames 900 and 1100.


### TASK-089 — Independent CoreGS launcher path

**Done (2026-10-21) in release 1.2.0.** Contribution by JulioHeroZ (PR #72, branch `task/089-coregs-independent-launcher`, based on main `6422988`), merged into `task/088-custom-textures-120` with a merge commit so their commits and authorship are kept. The launcher review of the diff found no problem. Added on the 1.2.0 branch: the setup dialog chooses GSdx (recommended) or CoreGS 1.1 (built in, no download) before setup starts; choosing GSdx in Settings installs it at once when it is missing (inline progress, "GSdx installed successfully", and a failure keeps CoreGS 1.1 selected); every user-facing "PCSX2 GS" string now says GSdx (settings keys and file names unchanged).

Original branch description:

Branch `task/089-coregs-independent-launcher`, based on main `6422988` (2026-10-21).
A saved `renderer = "coregs11"` choice skips the external renderer installer during setup,
its automatic missing/outdated-package repair at startup and its background release-list request.
The automatic launcher update check still runs. The explicit Check for updates and renderer
update actions continue to include the optional package. Play prepares a built-in renderer session
without reading or pinning an unused external executable.

Acceptance: synthetic setup completes without invoking the injected external installer, preserves the
choice across a launcher restart and prepares `GT4_RENDERER=donor` without an external process;
injected update requests query only launcher releases for CoreGS, while default PCSX2 startup and
explicit manual checks retain both streams. Full Rust launcher workspace passed on Windows (Tauri: 53 passed, one real-disc flow ignored).
Maintainer launcher acceptance remains pending.
Owner test: select CoreGS 1.1, restart the launcher with no gt4-gs package, launch a game and confirm
there is no renderer download; switch back to PCSX2 and confirm its existing setup/update path.
This changes package management, not rendering fidelity, performance or CoreGS 2.0 readiness.



### TASK-119 — RetroAchievements release 1.2.4

Owner decision 2026-10-25: integrate JulioHeroZ's PR #96 on the 1.2.3 code line, preserve authorship,
fix the reviewed account refresh/logout race, HTTP shutdown delay and missing test coverage, and prepare
a focused 1.2.4 release before bringing the feature to 1.3.0. Worktree `C:\Antigravity\gt4-wt-119-ra-124`,
branch `task/119-retroachievements-124`. The original PR commit `d6c9ea0` is preserved by merge.

State: completed, owner approved and published as stable 1.2.4 on 2026-10-25. PR #96 and #105 merged.
CTest 43/43, Python 641 tests/42 skipped, final Rust 379 passed/4 ignored; UI focus 114/114,
navigation 150/150; final launcher rebuild and all 24 installer/source checks passed.
Owner authorized final publication; real-account and installer execution remain untested.
Final asset GT4Recomp-Setup-1.2.4.exe, tag v1.2.4, Latest.
User-approved exception: the legacy launcher remains English-only in 1.2.4, with en-US and pt-BR notes.
Installer-only legacy packaging; the separate portable edition and multi-game launcher remain in 1.3.0.
Native inputs are reused unchanged from slice268; new output reservations are slice1190 (build/pin),
slice1191 (distribution), slice1192 (Cargo target). No owner saves or installation paths may be changed.
Acceptance: CTest, Python with the exact new build/pin, launcher workspace and UI checks; no credential
restoration after logout; bounded cancellation of a slow loopback HTTP request; installer/source identity;
owner real-account login, unlock, overlay and save test. Later 1.3.0 integration must keep RA disabled for
Spec II until its identity and achievement conditions are independently validated.

### TASK-120 — Integrate PR #100: DualSense trigger range and response curves

Owner request 2026-10-25: accept external PR #100 before PR #101, resolve integration conflicts,
and validate each contribution separately. Contributor: luanantony (Luan Antony); original commits
`b58024245253d548f3a8306e3eadc680bc547bd6` and `a36486d333c8fa23b89e1bddaa2404979b53dd87`.
Worktree `C:\Antigravity\gt4-wt-120-dualsense-integration`, branch `task/120-dualsense-integration`,
based on stable 1.2.4 main `90af154`.

State: merged through integration PR #106 at `8b73fa0` on 2026-10-25; original PR #100 is merged.
CTest 43/43, Python 641 tests/42 skipped, Rust 381 passed/4 ignored, UI focus 114/114 and
navigation 154/154 passed. Immutable slice1200 preserves slice1190 inputs and RA hashes.
No hardware gameplay or release performed. The incoming task label TASK-104 was incorrect: that number belongs to save import
on the 1.3.0 line. This contribution is tracked as TASK-120 and relates to planned controller tuning
TASK-110; it does not mark that broader future task complete.

Scope: preserve the contributed extended curves (s_curve, smooth_power, sublinear, logarithmic).
The proposed signed-axis modes and device-name heuristics are removed after review: SDL3 Gamepad
triggers already use 0..32767 for DualSense and Xbox alike. The unsupported raw-joystick mode was
never in a released build; native and Rust reject it consistently. Existing controls remain compatible.
Preserve the 1.2.4 RetroAchievements account, HTTP-cancellation and UI response guards.

Acceptance: independently reviewed SDL3 normalization and mode semantics, shared Rust/native parser and
writer compatibility, default and custom controller mapping regression fixtures, native CTest, Python
against the new build, Rust workspace and Controls UI navigation checks. Actual DualSense USB/Bluetooth
travel, reconnect and in-game pressure remain hardware checks; synthetic fixtures alone cannot prove them.
Record observed results before marking acceptance complete. Integration only: no release assets, labels,
installed application, owner card/save or active game session is changed. PR #101 begins only afterward.
See `docs/PR-100-101-INTEGRATION.md` for the sequential mapping and acceptance limits.

TASK-120 validation result (2026-10-25): CTest 43/43; Python 641 tests, 42 skipped; Rust 381 passed, 4 ignored; UI focus 114/114 and navigation 154/154, no JavaScript errors. Owner authorized sequential integration. Physical controller gameplay remains unvalidated; no release created.


### TASK-121 — Integrate PR #101 and #109: configurable racing wheels

Owner approved sequential integration on 2026-10-25 and assigned these contributions to stable
1.2.5 on 2026-10-26, followed by a port to 1.3.0-rc7 preserving frame skipping. PR #100 is already
merged through #106 at main8b73fa0. PR #101 by JulioHeroZ now ends at
`6da047cf5ca9829224ade13dd84e9c797129bdf8`; PR #109 by luanantony contributes conflict-resolution
commit `99f8dc48f6f6fc9ecfc0421bf0e75758203ea6bf`. Both histories are preserved by integration.

State: complete and merged on 2026-10-26 through PR #111 at main cbfd8b8.
Original PR #101 (latest head) and PR #109 are also marked merged.
Three original conflicts are resolved. Independent native and Rust/UI static reviews approved
routing, opt-in defaults, unique physical identity, full haptic stop, capture and UI/backend contracts.
Fresh slice1210 build/pin: CTest 44/44, Python 641 tests (42 skipped), Rust workspace 391 passed
(4 ignored), actual UI focus 114/114 and navigation 169/169. Actual UI setting commands were
recorded and replayed through real Rust command bodies. No physical haptics or gameplay claimed.

This implements configurable host input through existing gamepad actions, contributing to TASK-018.
Native guest wheel USB identity/protocol, physics-derived torque, direct gear selection and manual
clutch remain outside this implementation; damper/inversion fields are reserved and inactive.
Existing RetroAchievements protections and TASK-120 controller curves/standardized SDL triggers stay.
See docs/WHEEL.md and docs/PR-100-101-INTEGRATION.md. No new release is built or published here;
1.2.4 remains Latest. The later rc7 port must keep RA disabled for Spec II and pass both-game,
frame-skipping, input and language gates without replacing published rc6 assets.

### TASK-122 — Release 1.2.5 private draft (reserved on release branch)

Packaging is recorded on task/122-release-125 at 70aa363, without a main source merge. Draft v1.2.5
remains private; owner update 2026-10-26 puts publication on hold in favor of the native DFP plan.
This reservation prevents reusing TASK-122 while its release branch is open.

### TASK-123 — Native DFP wheel research and implementation plan

Study (2026-11-04): Luan Antony's wheel, DFP and force feedback roadmap (`docs/studies/WHEEL-FFB-ROADMAP.md`, PR #151, issue
#88) is kept as a reference beside the owner's plan (`docs/WHEEL-DFP-PLAN.md`, PR #193).

State 2026-10-26: research brief ready for an owner-run OpenCode agent; research not started.
Parent TASK-018, planned release 1.3.1. The virtual guest target is Logitech Driving Force Pro
(DFP), 900-degree steering, original game-generated force feedback translated to SDL3.
The launcher/host SDL3 configuration from TASK-121 is reusable groundwork, not the guest protocol.
Branch task/123-dfp-wheel-study; worktree C:\Antigravity\gt4-wt-123-dfp-wheel-study.
Brief: docs/tasks/TASK-123-DFP-WHEEL-RESEARCH.md. Study both USA v2.00 and Spec II independently.
Deliver an evidence-backed device/protocol map, HLE-versus-USB boundary analysis, reuse audit,
FFB effect coverage and staged implementation/test plan. No implementation, build, game launch,
physical force test, release or integration into active rc7 work is authorized by this research.
The owner starts the economical research agent in OpenCode; Codex reviews its deliverables.

## Open issue intake (owner, 2026-10-26)

Registration and priority triage only: no implementation plan or work has started.
Urgent defects are candidates for a later 1.3.0 rc before final, subject to owner selection.
Checked main, the modern launcher and rc6/rc7 task lists, and Claude's ai-memory notes.
Complete issue coverage: [issue triage](docs/ISSUE-TRIAGE.md).

Existing tasks reused; do not allocate replacements:
- #86 -> TASK-104 on the 1.3.0 line: the new chain-entry-7937 import failure needs follow-up.
- #89 -> TASK-105 on the 1.3.0 line: existing integrated-GPU/startup work, reporter validation pending.
- #98 -> TASK-025: deleted save returns; related to card persistence TASK-058.
- #88 -> TASK-018 and research TASK-123: native DFP/FFB remains planned for 1.3.1.
- #97 and #112 -> existing performance backlog TASK-041, related TASK-082/105/116/117/118
  and the CPU-parity TASK-119 on task/116-117-flare-perf. Retain both hardware reports.
- #95 -> TASK-053 for reuse of an existing texture directory, TASK-128 for optional external ISO.
- #92 -> TASK-080/TASK-079 for 4:3, TASK-130 for split-screen display.
- #87 -> existing TASK-057 mod-scope disposition and the owner's not-planned cheat policy.
  This is not approval or scheduling of a cheat feature.
- #161 -> request recorded 2026-11-04, not planned: a built-in option for Xbox button icons in the game (the icons are game
  artwork; a texture pack can replace them). Contributor design notes in PR #166.
- #128 -> request recorded 2026-11-04, not planned: the European release SCES-51719, which would be a third target
  executable next to v2.00 and Spec II.

TASK-104/105 exist in frozen rc6 commit 58322229c0bd63a0652b0550c5ad2154bd907039.
The CPU-parity TASK-119 is recorded in commit 9226927 on task/116-117-flare-perf, distinct
from main's RetroAchievements TASK-119 and task/101's player-one-controller TASK-119.
No existing task is renumbered, copied as a new task or silently dropped here.

### TASK-124 — Game cannot start (issue #107)

State: recorded, not planned (2026-10-26).
Issue: https://github.com/moesuito/gt4-staticrecomp/issues/107
Report: cannot start on both 1.2.4 and 1.3.0; screenshot and log bundle supplied. Cause unknown.
Priority: high, candidate for an upcoming 1.3.0 rc before final.
Related: TASK-105 and TASK-129 only if evidence establishes a shared cause; no duplicate confirmed.

### TASK-125 — GT Auto service audio keeps looping (issue #104)

State: recorded, not planned (2026-10-26).
Issue: https://github.com/moesuito/gt4-staticrecomp/issues/104
Report: oil-change/car-wash bubble audio persists after the service has finished.
Priority: normal backlog, release unassigned.
Related: distinct from missing-output-device startup behavior in TASK-129; no shared cause established.

### TASK-126 — Home key unavailable to ReShade (issue #102)

State: recorded, not planned (2026-10-26).
Issue: https://github.com/moesuito/gt4-staticrecomp/issues/102
Report: Home/Pos1 does not open the ReShade options while the game is active.
Priority: normal backlog, release unassigned.
Related: existing window/input tasks TASK-038 and TASK-076. No compatibility fix is claimed yet.

### TASK-127 — Flashing window followed by crashes (issue #99)

State: recorded, not planned (2026-10-26).
Issue: https://github.com/moesuito/gt4-staticrecomp/issues/99
Report: repeated flashing and random crashes in 1.2.2, 1.2.3 and rc5, for both games; logs attached.
Priority: high, candidate for an upcoming 1.3.0 rc before final.
Related: TASK-105 and TASK-124 remain separate until evidence confirms the same cause.
State (2026-11-04): fixed by TASK-145 (the window heartbeat: Windows took the fullscreen game for not responding, showed the
window behind it and closed it as hung, 0xCFFFFFFF), in 1.3.0-rc12; issue #99 closed at the owner's word.

### TASK-128 — Optional use of an existing disc image (issue #95)

State: request recorded, not planned or approved for implementation (2026-10-26).
Issue: https://github.com/moesuito/gt4-staticrecomp/issues/95
Request: reuse the original ISO without storing a second full copy. Claude had recorded the idea
without a task number; it remains an option under consideration, with no release assignment.
Related: the texture-directory portion of the same issue belongs to existing TASK-053.

### TASK-129 — Keep running without an audio output device (issue #93)

State: recorded, not planned (2026-10-26).
Issue: https://github.com/moesuito/gt4-staticrecomp/issues/93
Existing diagnosis: no Windows audio output causes the game to close. Desired behavior: run silently.
Priority: high, candidate for an upcoming 1.3.0 rc before final. Claude recorded the fix without an ID.
Deduplication: closed issue #94 is already a duplicate of #93. Issue #107 joins only if confirmed.

### TASK-130 — Split-screen display quality follow-up (issue #92)

State: recorded, not planned (2026-10-26).
Issue: https://github.com/moesuito/gt4-staticrecomp/issues/92
Report: split-screen resolution/quality concern; the reporter supplied the requested screenshot.
Priority: backlog, owner to select release priority. The old split-screen concern had no task number.
Related: 4:3 stays with TASK-080/TASK-079. Broader two-controller support remains the existing future
idea; this display intake does not silently schedule it.


### TASK-131 — Restore main to the released 1.2.4 program baseline

State: completed source restoration, owner-directed (2026-10-26).
Branch: task/131-restore-124. Restores program files directly from v1.2.4 / ad46df6;
preserves documentation and historical input pins. Git blob identity against that tag is
the validation; no build, hardware test, release or owner-data modification is performed.

Owner shelves the existing private 1.2.5 wheel draft. Public Latest stays 1.2.4. A future
1.2.5 will reintroduce only reviewed controller improvements from PR #100/#106 (8b73fa0),
with fresh packaging and acceptance; do not reintroduce the current wheel bridge.
This supersedes TASK-120/121 entries describing those changes as active on main and the
TASK-122 draft as a publication candidate. All contribution commits/authorship remain in
history (wheel integration cbfd8b8); native DFP remains TASK-018 with research TASK-123.

Contributor reply: https://github.com/moesuito/gt4-staticrecomp/pull/101#issuecomment-6422692723
TASK-018 and the TASK-123 brief were verified on main before linking. Requested native DFP
recognition (900 degrees) and original wheel FFB through SDL3, not pad-state/rumble adaptation.
No native wheel backend is claimed to exist, no new feature implementation dispatched.


### TASK-132 — Publish controller-only 1.2.5 from the reviewed PR #100 integration

Owner request 2026-10-26: replace the private wheel-containing 1.2.5 draft with a final
controller-only 1.2.5, after review and checks. Branch task/132-release-125-controls starts
from main749af82 and reapplies exact program blobs from reviewed integration8b73fa0
(PR #100 via #106), before wheel integration. No Git history is rewritten.

State: done, published stable Latest v1.2.5 (2026-10-26), PR #125 / merge5d7a669. Only launcher,
workspace lockfile, Tauri metadata and installer version change beyond that reviewed code.
Native game reuses immutable slice1200-pcsx2.json; fresh distribution slice1320-controls125-dist.
No wheel implementation or wheel UI is included; native DFP remains TASK-018 / research123.
RetroAchievements1.2.4 is retained. Legacy English UI exception remains owner-authorized;
matching en-US/pt-BR short notes precede the launcher build. Installer-only legacy release.

Observed checks: CTest43/43, Python641 tests/24 skipped, Rust381 passed/4 ignored, UI focus114/114
and navigation154/154 with no script errors. Official launcher build/path scan and installer
extraction/hash comparison passed. Publication follows the owner-authorized final release step.
No new physical controller, account-unlock or installer execution validation is claimed.
This supersedes the held TASK-122 wheel release candidate; keep its historical branch intact.

TASK-132 publication receipt: old private wheel draft deleted; new controller-only draft asset
sizes/SHA256 digests verified before owner-authorized final publication. Release v1.2.5 is
public, not a pre-release, Latest; tag points at5d7a669bdecf5e60c0782ca449f48aacac3c7a32.
https://github.com/moesuito/gt4-staticrecomp/releases/tag/v1.2.5
Installer SHA256556e7725d11146f1f79613814fbf961e1299000bd56df7c8831a324213003a74.
No new wheel support, Spec II, frame skipping or CoreGS changes are included.
### TASK‑119 — CPU: reach PCSX2 usage at 1x, then go below it (owner, 2026-10-26)
Owner decision (pinned in ai-memory `decisions/next-objective-cpu-parity-with-pcsx2`). On the weak laptop (i5-7200U,
HD 620, 1x, Direct3D 11, blending Minimum, MTVU on, no speed hacks) the same scene (Time Trial Nordschleife, Pitwork Z)
runs at **~50% CPU on PCSX2 with 60 FPS and the flares on**, against **70-80% and dips** on this port. GPU is the same
20-30%. A static recompiler must not use more CPU than the JIT: the gap is our overhead.

Order: **first match PCSX2** (about 50% on that laptop, 60 FPS, no dips, correctness unchanged), then go beyond it.
Where to look, in order: (1) per-instruction `c.budget -= N` / `preempt(c)` / `ld32`-`set32` in the generated code;
(2) the GS IPC, ~1,670 records per frame to a separate process plus journal ordering and frames-in-flight waits;
(3) FPU/MMI semantic wrappers per operation; (4) kernel/syscall and idle handling.
Acceptance: F8 on the laptop with the launcher idle, same scene and settings, EE CPU and EE critical path at or below
PCSX2's, 60 FPS without dips, identity gates still green. Baseline and measurement plan: ai-memory
`notes/pcsx2-baseline-weak-laptop`. Another agent is already working on the EE CPU side; coordinate before touching
the same files.


### TASK-133 — Integrate RetroAchievements and controller-only 1.2.5 into rc7

State: mapped/prepared, not implemented or dispatched (owner decision, 2026-10-26).
New rc7 scope: published rc6 + stable1.2.4 RetroAchievements + stable1.2.5 controller improvements.
OriginalGT4 v2.00 achievements only; Spec II stays disabled at both launcher and native boundaries.
All7 controller curves apply to both targets. No wheel bridge, native DFP or frame skipping.

Frozen base: v1.3.0-rc6 /58322229c0bd63a0652b0550c5ad2154bd907039.
References: v1.2.4/ad46df6; reviewed controller integration8b73fa0; finalv1.2.5/5d7a669.
Branch task/133-rc7-ra-controls, worktree C:/Antigravity/gt4-wt-133-rc7-ra-controls,
created directly from published rc6. Brief: docs/tasks/TASK-133-RC7-RA-CONTROLS-INTEGRATION.md.
Do not merge legacy main or the frame-skip branch wholesale into this candidate.

Preserve TASK082 on task/082-render-skip at15b7bf3971f4046faabc2d0cbd08768ae4883374,
PR113 OPEN/DRAFT, unmerged, report docs/TASK-082-RESUMPTION-REPORT.md. Resume later with
CoreGS2.0, without a version assignment. The former test082 kit is not the new rc7 candidate.

Acceptance: target isolation, account/logout/HTTP cancellation regressions, control persistence/
preview consistency, full native/Python/Rust and en-US/pt-BR checks, both target builds/pins,
installer+portable verification. New builds go to draft; owner gameplay/review before merge or
publication. No code, builds, running agents or public rc7 authorized by this mapping step.

### TASK-134 — Game-code optimization study (research and plan; closed 2026-11-04)

**State 2026-11-04: ✅ closed** (owner: "fechar a task depois pra não voltar em otimizações que já foram feitas"). Three
rounds: rc9 (process CPU per VBlank -18% on the i5-7200U, -19% on the 5700X against rc8), the second round (slice395, -26%
against rc8 on the desktop) and a third, measured round during rc14 that kept nothing: a CLUT same-palette probe (about 5% of the
CLUT flushes keep their palette), the VU1 `vfix` census (what is left is needed), the SPU2 envelope (about 0.03 ms) and CoreGS 2.0
batches per submission from 32 to 256 on the Avell at 1x and 6x (64, the default, is the best balance: larger submissions save
GS CPU and lose frame time). Every kept step, every dropped
idea with its numbers, and what limits the Avell at 6x (the game's one GS download per frame) are in
`docs/GAME-CODE-OPTIMIZATION-STUDY.md` sections 8 and 9. What is left is large and gets its own task if it is ever started:
TASK-168 (several palettes in one batch), P8 (the translator: `Cpu & __restrict`, one host function per guest function) and a
CoreGS 2.0 submission thread. Performance numbers come from the Avell from now on (owner, 2026-11-04).
Owner decision (2026-11-04): the per-frame GS download (the light-flare table) stays exact in the normal mode; a one-frame-late
answer was tried during CoreGS 2.0 and did not look right, and Performance Mode (flares hidden) is the choice for speed.

**State 2026-11-03:** 🧪 second round on top of the TASK-139 picture fixes, final build `build/t134-cand/f3-spec2` and `f3-v200` (pins slice395), packed in `private/slice395-dist-t134r2` and installed on the owner's desktop for their test (2026-11-03, files checked against the manifest; the notebook keeps the first rc9 build). **Published on the owner's word as pre-release `v1.3.0-rc9`** (2026-11-03, tag `39320b99`, installer `4f2c54bc...`, zip `fdb54662...`); the task goes on toward rc10. Player's session with a warm shader cache, George V Paris lap, 1x, 5700X: process CPU 12.96 (rc8) / 10.59 (slice392) -> 9.57 ms per VBlank (-26% / -9.7%). Kept: CoreGS 2.0 submits every 64 batches on a discrete GPU, VU1 CLIP over lanes, VU1 outer product as one vector operation, VU0 over four lanes, GPU vertices inserted per primitive, partial VU loads and stores as whole qwords (VU tables regenerated, `private/slice394-*`). Dropped with a measurement: VIF wrap mask, per-format generic VIF unpack, uber pipelines only. Gates against slice392 all equal (three recorded laps, v2.00 night and Arcade, 6x, VU and MTVU verify). The bench now uses a warm shader cache per executable (rc9's earlier numbers compiled pipelines during the lap). Left: CoreGS 2.0 submission thread, deferred flare readback, SPU2 envelope, P8.

**State 2026-11-02:** 🧪 1.3.0-rc9, slice390, on branch `task/134-game-code-optimization-study`, installed on the owner's desktop and notebook, not published or uploaded (owner: CoreGS 2.0 picture bugs first). Player's session on the recorded George V Paris lap, 1x: i5-7200U 33.87 -> 27.64 ms of process CPU per VBlank (-18%), Ryzen 7 5700X 12.59 -> 10.14 (-19%). Race clock, simulation state and frames identical to rc8. Done: P0, P1a-d, P4a-b, P7 in part, and two steps the profile found (Spec II VU0 compiled, VU0 macro operations over four lanes). Dropped with evidence: P2. Left, cheapest first: study section 8. Waiting for: the owner's test.

State: study done; owner decisions recorded (2026-10-28); P0 waits for the owner's signal that the desktop is free. Branch `task/134-game-code-optimization-study`, worktree
`C:/Antigravity/gt4-wt-134-game-code-study`, from main 795cdc0. Research and documents only: no code, build, benchmark,
profiler or game run (a CoreGS 2.0 session was measuring on the desktop). Agent status: not PCSX2-tainted.
Follows the owner's CPU-parity objective (PCSX2 ~50% vs GT4Recomp 70-80% on the i5-7200U), recorded as "TASK-119" in
commit 9226927 on `task/116-117-flare-perf` and in ai-memory `decisions/next-objective-cpu-parity-with-pcsx2`.

Deliverable: [docs/GAME-CODE-OPTIMIZATION-STUDY.md](docs/GAME-CODE-OPTIMIZATION-STUDY.md). Evidence: 13 desktop F8
blocks and the owner's laptop logs (6 race F8 intervals, ~200 periodic reports), copied to the worktree's ignored
`private/laptop-i5-7200u-f8/` with hashes.

Result: on the laptop race the four busy threads use ~35 ms of CPU per 16.7 ms frame (EE 11.7, VU1 worker 9.3,
gt4-gs CPU thread 3.4, gt4-gs GS thread ~11); half or more of the EE critical path (12.2 ms) is emulation plumbing, the
rest translated game code; no large spinning wait exists. Ordered plan P0 (measure) .. P8 (IOP AOT), with gains, effort,
risk and identity gates per step; GSdx-bridge costs are left to the CoreGS 2.0 intake (section 3.6).
Owner decisions (2026-10-28, study section 7): measure SSE4.1 first, no AVX2 executable unless a large gain; always
the easiest and cheapest step first, built and tested right away, the translator (now P8) last; thread priority and
core placement yes; the WPR profile and the first baselines run when the owner says the current CoreGS 2.0 build is
done; v2.00 is already on the laptop; no more work on the GSdx bridge, the game-side optimisations ship with CoreGS 2.0.
Test method: one fixed headless Arcade night race on the desktop (Clubman Stage Route 5, Ford Ka, 5 AI, plus its
replay for the varied cameras; B-Spec exists only in GT Mode, which the owner ruled out); laptop runs by the owner with F7/F8 on builds the agent copies to the laptop's
portable folder over the network, logs collected the same way.
2026-10-29: branch synced with 1.3.0-rc7 (`task/133-rc7-ra-controls` c21e934, program files identical); the
**Owner, 2026-11-02:** CoreGS 2.0 is done for this step: rc8 is published and this branch receives it, so P0 and P1 can start. They are rc9. Laptop numbers on candidate 382: `docs/hw-renderer-spec/CORE2-HANDOVER-TASK134.md`.
RetroAchievements per-frame hook added to the study (section 3.8, P1g). Next: P0 (measurement) and P1 (host quick wins) on this branch once the desktop is free; each later step gets its own
task or branch as it starts.

### TASK-135 — Performance HUD for CoreGS 2.0
Repos: this one. Clean room: a TASK-027 agent, not PCSX2-tainted.
Goal (owner, 2026-11-02): CoreGS 2.0 has no in-game overlay today. The overlay of TASK-062 is drawn inside `gt4-gs.exe` with the
PCSX2 OSD, so it exists only under GSdx and its code cannot be ported: this one is our own, MIT, in the game executable. It
shows what a player understands, CPU and GPU instead of EE and GS: graphics card, output resolution, render scale and refresh
rate; FPS with its minimum and maximum; frame time; CPU and GPU use of the game; memory; a live frame time graph. The owner's
reference is the look of the macOS Metal HUD (screenshots only).
Acceptance: off by default, launcher rows under CoreGS 2.0, overhead measured, the identity gates unchanged with it off, owner
test. Updating the GSdx overlay to match is separate work in the fork, for a PCSX2-tainted agent.
State (2026-11-02): candidate 378, off by default; 26.6 us per presentation, no measurable cost on the graphics thread (IN-526, ledger). Published in 1.3.0-rc8.

### TASK-136 — Simple onboarding, GSdx optional
Decision 0074. Owner, 2026-11-02: no GSdx download on a first install when the PC runs CoreGS 2.0; automatic, nothing to choose. Acceptance: a new install on a capable PC ends on CoreGS 2.0 with no download; a PC that cannot run it ends on GSdx, downloaded, and is told why; a player on GSdx or with a stored choice sees no change; owner test.

State (2026-11-02): published in 1.3.0-rc8. A whole setup on a capable PC passed in the owner's onboarding; a PC without Vulkan is not verified.

### TASK-137 — Link the disc image
Decision 0075. Owner, 2026-11-02, asked for by players and needed for Android: one image for this and an emulator. Acceptance: setup writes no copy of the image; an image that moved or changed asks to be chosen again, and choosing it is all it takes; an install with a copy stays installed; accented paths work; owner test.
State (2026-11-02): TASK-137 and TASK-138 (a portable folder carries its image, accented paths, the GSdx fallback; decision 0075) are published in 1.3.0-rc8. Settings > Games shows the image's size and location.

### TASK-139 — CoreGS 2.0 picture bugs from recorded drives

State (2026-11-02): started on `task/139-coregs2-picture-bugs`, on top of rc9. Waiting for the owner's recordings (Tokyo R246, El Capitan, possibly a third track). Each one is replayed on CoreGS 2.0 and on the reference renderer, and the differences the owner reports get fixed in `native/hw_gs/`. Acceptance: the owner's test on the same scenes.

Bugs reported by the owner (2026-11-02):

1. **Time trial ghost drawn solid.** On a real PS2 (the owner's video capture) and on GSdx the ghost lets the scene through; on CoreGS 2.0 it looks like an opponent's car. Confirmed on the replayed Ford GT lap at 3x, frame 8000: the reference draws the ghost in alternating dark rows, as the PS2 capture also shows, and CoreGS 2.0 draws it solid. So it is a row pattern rather than plain 50% blending. Cause Unknown. Reproduces with `george-volta2`.
2. **Asphalt texture on Tokyo R246.** Sent by a player on Discord, reproduced by the owner without custom textures. The player's picture has large black areas over the buildings as well; the owner's Hypothesis is that more places break for them because they use a high resolution texture mod. Needs the owner's recording.
3. **Glitch when a race returns to the menus (480p to 480i).** A random frame or green squares for a moment, not predictable; not seen on GSdx. The launcher always forces 480p in races, and the recording scripts do too, so a replay passes through this switch if the drive leaves the race before F9. Not reproduced by us yet.

The owner's pictures and the ghost comparison are in `private/coregs2-bench/reports/` (not in git).

State (2026-11-03): the owner recorded Tokyo R246 and El Capitan and reported two more bugs on them: a wrong frame as the race opens (bug 4) and a glitch in the fade when leaving the race (bug 5). All five are fixed and match GSdx on the replays (IN-528 to IN-532): bug 1 IN-530, bug 2 IN-528, bug 3 IN-531 and IN-532, bugs 4 and 5 IN-529. Installed on the owner's desktop (slice392).

State (2026-11-03): ✅ done. The owner tested slice392 in the game ("everything fine so far") and approved the merge into `task/134-game-code-optimization-study`, where the CPU work continues. Not uploaded. The replay tools stay for later bugs: `scripts/coregs2_bench/README.md`, section "Recording a drive and replaying it".

### TASK-140 — 1.3.0-rc10: player fixes from the open issues and external PRs

Owner, 2026-11-03: after the triage of the 47 open PRs and the open issues (ai-memory `notes/pr-issue-triage-2026-11-03`), the
owner chose these items for rc10, next to the CPU work of TASK-134. The external PRs were written against 1.2.4/1.2.5; their
ideas are re-done by hand on this branch, with the contributor (luanantony) credited as co-author, and no branch is merged.

1. **No audio output device (TASK-129, issues #93 and #115; PR #119):** the game runs silent instead of exiting.
2. **GT Auto bubbles that never stop (TASK-125, issue #104; PR #118):** the SPU2 exponential envelope decrease truncated a
   small step to 0, so a released voice never reached silence. Capsule `scripts/prepare_spu2_runtime.py`.
3. **Plain exit messages (TASK-124 and TASK-127, issues #107 and #99; PR #154):** a missing DLL, a game closed as not
   responding, and other Windows codes in hex, in both languages.
4. **480p hold for 1080i saves (TASK-078; PR #174):** a save or imported card that holds 1080i races at 480p too.
5. **GSdx missing or unreachable (issues #139, #179):** Play offers CoreGS 2.0 when the card can run it.
6. **Spec II replays lost (issue #185):** Spec II names its replays `BASCUS-97436R` plus seven letters and digits (two replays
   on the owner's PCSX2 card), and the launcher only kept four digits.
7. **The memory card error names the file (issue #160).**
8. **Card backups have a limit (owner, 2026-11-03):** every Play kept a copy of the card (about 8 MB) and none was ever
   removed (74 copies, about 600 MB, in two weeks on the owner's PC). The newest 10 card images, 5 taken before a New save and
   10 copies of the active save stay.

State (2026-11-03): items 1-8 done on `task/134-game-code-optimization-study` (items 3, 5 and 7 by a Sonnet agent on
`task/rc10-launcher`, merged). CPU work of TASK-134 for rc10: VU1 products with VF0.w emitted as copies (VU1 program -3.4%).

Acceptance: CTest, Python and launcher tests; identity gates against rc9 (the 480p hold and the SPU2 change are the only
intended differences, and neither shows in the gated scenes); language gate; owner test, including a car wash in GT Auto and a
Spec II replay saved, then found again after a restart.

Owner test of rc10 (2026-11-03): the GT Auto oil change sound stops (item 2 confirmed).

### TASK-141 — CoreGS 2.0 custom textures: a frame of the game's texture, and doubled logos in GT Auto

Clean room: a TASK-027 agent, not PCSX2-tainted. Reported 2026-11-03, CoreGS 2.0 with the owner's HD HUD & UI pack
(`data\Textures`, 15,973 files):

1. **A player:** when a lap ends on Tokyo R246, the lap and position digits show the game's own texture for about a frame, then
   the pack's again. Cause (Confirmed in the code): a replacement needed by a batch and not on the GPU is loaded on the worker
   thread and only taken at a later VBlank; until then the batch draws the game's texture (IN-470). New content (a new digit)
   or an image evicted past the budget does this every time.
2. **The owner:** in GT Auto (Spec II), the GT Auto logo and the Oil Change title look doubled, with a shifted copy; without the
   pack they are right. Cause Unknown; needs a recording of that menu and a comparison with GSdx and the same pack.

Acceptance: no batch drawn without its replacement in the recorded scenes (`late=0` in the `[core2] texture replacement` line),
GT Auto as on GSdx with the pack, the identity gates without a pack unchanged, owner test.

State (2026-11-03): both fixed on `task/134-game-code-optimization-study` (IN-533, IN-534). Item 2's cause: the logo's palette
lives in a display buffer's memory and was loaded on the GPU in every other frame, which skipped the replacement there; it is
now read from GS memory for the key, as GSdx does. Recording `private/coregs2-bench/inputs/gtauto-oil.txt` on a copy of the
owner's Spec II card (`gtauto-card.ps2`): from the GT mode home to GT Auto and the Oil Change. A third report (lap time digits not
in HD in a race with opponents) is not reproduced on the Time Trial recording, where they are HD; the palette fix may cover it
(16,000 more HUD batches replaced on Tokyo R246). Owner test next.

State (2026-11-03, night): rc10 rebuilt with these fixes (slice399) and installed; the owner's tests passed and rc10 is published
as a pre-release on the owner's word.

### TASK-142 — CoreGS 2.0: a car's glass shows parts of the car in front when close behind it

Clean room: a TASK-027 agent, not PCSX2-tainted. Reported by the owner on rc10 (2026-11-03), CoreGS 2.0 at 6x: driving close
behind another car, where the transparency of a car's glass is drawn, the 3D of the car in front shows through in places. Cause
Unknown (Hypothesis: the order or the depth of the transparent glass batches against the other car, or a target read while it
is being drawn). Next: record a drive close behind a car (`record.cmd`), replay it on CoreGS 2.0 and GSdx and compare frame by
frame. Planned for rc11 (owner).

The owner added (2026-11-03): only behind the Skyline R34s (4th and 5th at the start), never behind the Supras or the NSX, and
what shows is the car in front, not the R34's interior. Recording `private/coregs2-bench/inputs/r34-glass.txt` (the owner's replay
opened on a copy of their Spec II card, `r34-glass-card.ps2`).

State (2026-11-03): fixed on `task/134-game-code-optimization-study` (IN-535). Cause (Confirmed by reading back the stored words
after each draw): GT4 clears its environment map with two 64-wide sprites whose depth buffer lies one page into the map's own
memory, so half of the map gets its alpha (0x80) from the depth write; CoreGS 2.0 kept that depth in its own image, the map kept
alpha 0 there, a car's reflection pass that read it left alpha 0 on the car, and the R34's glass reflection (scaled by the alpha
behind the glass) then left the car in front sharp and dark. A depth fill now writes the colour target whose memory it covers.
R34 recording at 6x: the car in front misted behind the glass as on GSdx; 8 of 13 frames change, 7 closer to GSdx and 1 equal.
Tokyo R246: 17 of 43 frames change by at most 127 pixels, all closer to GSdx; El Capitan: 6 of 43, at most 133 pixels, equal to
GSdx. The owner looked at the R34 frames: fixed.
Acceptance: CTest (`hw_core2_depth_alias`), the other recordings changed only where a reflection reads that alpha, owner test.

State (2026-11-03, night): 1.3.0-rc11 built on the owner's word and installed on the desktop, not uploaded: games slice400 (PGO
and ThinLTO; pins `docs/inputs/slice400-*.json`; distribution `private/slice400-dist-130-rc11`), package
`private/release-130-rc11-out` (installer `afb176ef...`, zip `fbdd4abb...`). Gates against rc9: race clock and simulation state
identical on the three laps and the v2.00 night race; v2.00 frames identical (10/10, 14/14); Spec II frames differ only on car
glass by a few pixels (with `GT4_CORE2_DEPTH_INTO_COLOUR=0` El Capitan is 35/35 identical); VU1 236,448 and 355,388 samples and
MTVU 8,032 frames with 0 differences. The installer now carries a version resource (product, publisher, version, copyright) and
the launcher names its publisher (it said "github"): a laptop's Defender had flagged the rc10 installer by machine learning
(Program:Win32/Contebrew.A!ml); local scans of rc9, rc10 and rc11 find nothing. Owner test passed; published as a pre-release on the owner's
word (`v1.3.0-rc11` at `52645c6b`, 2026-11-03).

### TASK-143 — Launcher About: credits for GitHub contributors

Owner (2026-11-03): the About page credits everyone outside the project who has a merged or open pull request on
`moesuito/gt4-staticrecomp`, by GitHub name. Mapped with `gh pr list --state all` (142 PRs): gustavocanepa (Wise; #36, #41
merged), JulioHeroZ (Julio Vitor Rufino; #72, #96, #101 merged) and luanantony (Luan Antony; #100, #109 merged, 44 open). Left
out by that rule: Wackyhcky (#117, closed without a merge). A new contributor gets a key in both locales and in the list in
`ui/js/pages/settings.js`.

State (2026-11-03): done on `task/134-game-code-optimization-study`, in a block of its own above the Spec II credits, one paragraph
per contributor, en-US and pt-BR. Checks: locale keys 659 in both with the same placeholders, focus checks en-US 275 and pt-BR
282 and nav-test 209 all passed, launcher cargo test 630 passed; release launcher built with label rc11 (not installed, not in a
package yet).

State (2026-11-03, later, owner): Kaezzey (Niket Shah), who started gt4-staticrecomp (this repository is its fork; the upstream
repository no longer resolves), heads the list; Wise's line names him as the author of gt4-decomp (WiseBR3000's commits are his,
same author name and e-mail). The README credits them on `main` (`11fcc44a`). Luan's #118, #119, #154 and #174, whose ideas
shipped in rc10, are marked as merged by a tree-neutral merge on `main` (`7693f5c4`, strategy "ours": no file of `main` changed;
the other 40 stay open). The 11 merged outside PRs were checked against the About lines: nothing missing (JulioHeroZ #72, #96
RetroAchievements, #101 wheels; luanantony #100, #109; gustavocanepa #36, #41). Locale keys 660; focus checks and nav-test pass.
Shipped in 1.3.0-rc11 with the installer's new version resource (TASK-142 state).

### TASK-144 — A laptop's first 1.3.0 run chose GSdx and Performance Mode (investigated)

Owner (2026-11-03, rc12 line): on a laptop with an RTX 3060 Laptop GPU and Intel UHD Graphics (one 3440x1440 144 Hz monitor on the
RTX's USB-C), the first run of rc10 downloaded GSdx and set it up with Performance Mode on, instead of CoreGS 2.0. Cause
(Confirmed from the log bundle): that install was an update, not a new install. The settings file of an earlier build was still
in the user's folder (the first run warned that `async_gs`, retired in 1.3.0, is no longer used), with GSdx and Performance Mode
already chosen; `launch::setup_renderer` tries CoreGS 2.0 only on a first game, with GSdx missing and no renderer choice stored,
so the stored choice was kept, as the 1.3.0 notes say ("Installs already on GSdx stay on it"). A fresh portable rc11 on the same
laptop chose CoreGS 2.0 by itself ("CoreGS 2.0 runs on this PC; it is the renderer, and GSdx is not downloaded") with Performance
Mode off. GPU selection was right in both (the RTX; Vulkan reports it as device type "other", the DXGI list marks it discrete).
No code change; offering CoreGS 2.0 to GSdx users on capable cards is an owner decision.

### TASK-145 — A one-frame flash of the window behind the game, and a stutter every 6 seconds (done)

Owner and other players (2026-11-03): in borderless fullscreen on NVIDIA RTX PCs the game showed, for one frame, the desktop, the
taskbar or the window behind it, with a stutter every 6 seconds, on CoreGS 2.0 and GSdx (Vulkan, Direct3D 11 and 12) alike, and
after Alt+Tab it sometimes came back as a white "Not responding" window while the game kept running. Not in windowed mode.

Cause (High confidence, measured on the owner's laptop, RTX 3060 Laptop GPU, 3440x1440 144 Hz): Windows takes a window as not
responding when its thread has not read a message for about 5 seconds, and the game thread's PeekMessage calls on an empty
queue do not count (`IsHungAppWindow` TRUE 0-16 ms after one). In fullscreen with a gamepad the queue stays empty, so the game
window was declared hung every few seconds; the shell and `dwm.exe` put another window or a white Ghost window in front for a
frame and restored the game with an asynchronous `SetWindowPos`, whose message reset the clock (every 360 VBlanks).

Fix on `task/134-game-code-optimization-study`: the game window keeps a one-second timer whose message the window procedure
answers (`native/window_heartbeat.hpp`; decision 0076, which says why it must stay and how to test any change;
`GT4_WINDOW_HEARTBEAT=0` for an A/B). Kept with it: a size message with an unchanged client size is not passed on to the
renderer, a CoreGS 2.0 swapchain that is suboptimal at the same size is kept and each rebuild logs its reason, and
`GT4_WINDOW_WATCH=1` diagnostics (hang watcher, activation and position-change logs). Removed after testing as not the cause:
Vulkan full-screen exclusive DISALLOWED, dropping foreign `SWP_FRAMECHANGED`, and an import-table call log. The GSdx-side
change prepared in the `gt4recomp-gs` fork (gt4gs 1.2.2-test1) is not needed and is not released.

Result (owner test, 2026-11-03, Spec II, plain build): a full lap (10,017 VBlanks) with no hang, no position change from
elsewhere and no same-size size message (before: one every 360 VBlanks in each of 15 sessions); a 22-second Alt+Tab came back
normally (before: Ghost window 5.7 s after losing focus); GSdx also without flashes. CTest `native_window_heartbeat` and
`tests/python/test_window_heartbeat.py`. For rc12.

### TASK-146 — 60 FPS lets the frame rate drop in a race instead of slowing the game (experimental, rc12)

Owner (2026-11-03): the 60 FPS mode should let the frame rate fall when the PC cannot keep up, skipping frames, with the
simulation and the game speed staying at 60 Hz; under ideal conditions it stays at 60; the 30 FPS mode stays. The policy
exists since TASK-082 (`GT4_FRAME_SKIP=1`, `frame_skip::Policy`: nothing is left out until the host is behind by 33 ms, then
drawn frames are paid for from the time that goes by, at least 20 drawn per second; even presentation, IN-525), measured then at
100% game speed (George V Paris 12x: 42 frames drawn per second), but never offered, because the motion steps between the
game's 16.7 ms states.

Change on `task/134-game-code-optimization-study`: the launcher's Frame rate "60 FPS" (CoreGS 2.0, the default) now passes
`GT4_FRAME_SKIP=1` (`game.ini` `hw_frame_skip_adaptive=1`; the game accepts that key); "30 FPS" stays `GT4_FRAME_SKIP_FORCE=2`.
As before, only in a running race and in 480p; menus never skip. Help text in both languages. Checks: launch contract
(`GT4_FRAME_SKIP` with 60, not with 30), `standalone_fixture` (the key and its variable).

Acceptance: owner test on the laptop (plain build, rc12 line): no frame left out where 60 holds, full game speed where it does
not, no picture fault; then the owner decides whether it stays the default.

First laptop test (2026-11-03): it worked, then the game crashed at the end of a race (access violation in nvoglv64.dll inside
`vkQueueSubmit` on the presentation thread, from the crash dump read on the laptop). Cause (High confidence): the paced
presentation path that frames left out use (since TASK-082, also the 30 FPS mode) broke Vulkan's external synchronization: the
presentation thread recorded its show commands from the GS thread's command pool, and the frametime thread's present wait
overlapped acquires and presents on the swapchain (that one in every mode). Fixed (IN-536): the show commands have their own
pool, and every acquire, present and present wait holds a swapchain mutex, the wait polled every 0.5 ms. The validation
layer's thread checks on a replay: 10 overlaps before at 30 and 60 FPS, 0 after; frames left out unchanged. New plain builds
on the laptop; owner retest pending.

Second laptop test (2026-11-03): no crash. F8 over 198 s of a Spec II race at 6x: game speed 99.1%, 625 frames left out (never
two in a row), 10 audio underruns (the crackles: the 60 ms audio buffer ran dry in stalls of up to 77 ms, mostly the GS thread
waiting for the GPU's readbacks, 7.4 of its 15.6 ms per frame). The owner: pacing is fine at a steady 60 and only bad when it
drops (interpolation, a large job, stays out of rc12); the simulation must keep 60 Hz always, so leave out as many frames as
needed; the 30 FPS mode ran steady but only from the moment the camera takes the car, not in the intro before the start or a
replay's. Changes:
- the gate also opens when the race object is there with its clock still at 0, the intro from the first race camera to the
  start (v2.00 night race: 828 VBlanks); pause (clock still above 0), menus (no race object; the pre-race screen has none) and
  accelerated time stay closed. Frame skip log column `race`.
- the 60 FPS policy keeps speed first: a frame of delay is a shortfall (was 33 ms), delay past a frame is paid back four times
  faster, up to 8 frames in a row may be left out (was 2; at least one drawn in nine); the pacer owes up to 12 frames while
  frames can be left out (was 6).
Checks: `frame-skip-suite-v200.sh` 30 FPS over VBlanks 6450-7600 (intro and start), 1,143 frames ok, 528 left out, the
simulation state identical over the whole race (2,478 left out); `frame-skip-suite.sh` (Spec II) ok, 22 of the known drawing
words; 60 FPS at 12x on the desktop's RX 9060 XT: game speed 100.00%, 40.3 frames drawn per second. New laptop builds write
`frameskip.csv` beside game.log (compile-time switch, test builds only); owner test pending.

### TASK-147 — Triggers and sticks: GT4's 100% came at a third of the trigger (rc12, done)

Owner and players (2026-11-03): the throttle reaches 100% with the trigger 30-40% pressed, and a small stick push steers
more than it should. Our host path is linear (trigger byte to pressure byte, stick to the PS2 byte after the XInput dead
zone; the emulated pad passes the bytes on unchanged). The game scales it (Confirmed, translated code of both games): the
pressure byte p of the button assigned to a function becomes min(100 * min(2p, 256) / d, 256), d = 35 throttle (v2.00
0x0043BEE8, Spec II 0x00319194), 65 brake (0x0043BF48, 0x003191F0), 35 reverse, so 100% throttle from p = 45 and 100%
brake from p = 84, made for the DualShock 2's pressure buttons: about 27% and 40% of a trigger's travel with our 30/255
dead zone. Community reference (not copied): Silent and Aero_'s "Adjusted triggers sensitivity" PCSX2 patch changes those
instructions; Spec II ships a similar patch as an optional PCSX2 extra, not on its disc, so both our games have the stock
scaling.

Trigger fix on `task/134-game-code-optimization-study` (owner's choice, host side, no game code change): a trigger on R2
spans 0..45 and one on L2 0..84 over its travel past the dead zone, the game's own 100% at the end, rounded to the nearest
step; the controls option `range` (1..255) changes it per binding, and the launcher accepts and keeps it. A game-side
patch for finer steps (like Silent's, about 230 instead of 46 and 85) is for later. Checks: controls, pad mapping and SDL
fixtures; launcher controls tests. Owner test on the laptop (2026-11-03, LT brake and RT throttle in the game's
options): the triggers are right, approved.

Sticks (same day; disassembly study of the pad path, both games): GT4's remapper maps a stick byte b to (128b + 45) / 90
below 90, to 128 from 90 to 165 and to 128 + (127(b - 165) + 45) / 90 above 165 (knot table v2.00 0x0064C4D8, Spec II
0x00681088, byte for byte the same; Confirmed), a 29% dead zone per half, and steering is -32 (out - 128). Stacked on our
XInput dead zones (24% left, 26.5% right) nothing happened up to about 46% of the travel, then the rest steered 1.9 times too
steeply ("a small push steers too much"). Fix: both sticks have a 10% dead zone, and past it a reading `steps` away from rest
becomes 165 + round(90 steps / 127) or 90 - round(90 steps / 128), so the game steers in proportion to the travel from the
first step, within one of its own steps (90 bytes for 127 steps); option `game_deadzone=skip|keep` (skip by default), the
launcher accepts it and its stick dead zone defaults are 10%. The right stick on throttle or brake (curve 37..127 of its
one-sided magnitude) starts from the same bytes. Checks: controls fixture over all 65,536 raw values against the game's own
formula, SDL fixture.

State (2026-11-04, owner): done. "Control precision is solved": triggers and sticks approved after the owner's tests; ships in
1.3.0-rc12. GitHub issues closed as fixed for rc12: moesuito/gt4-staticrecomp#183 (triggers full too early), #134 (full range
triggers) and #178 (stick dead zone). Later, not scheduled: a game-side patch for finer trigger steps.

### TASK-148 — Choose the controller the game reads (rc12, done)

A player (2026-11-03, via the owner): with several controllers connected the game takes one at random. The game and the
launcher read "controller 1", the lowest SDL instance id. Change on `task/134-game-code-optimization-study`:
- the launcher setting `controller` (`auto`, or `<guid>` / `<guid>/<serial>`: the GUID SDL3 gives the model and the serial
  number when the pad reports one), chosen on the Controls page from a dropdown of the connected pads, Automatic first;
- the game gets it as `GT4_PAD` (and `pad=` in game.ini for a direct start) and reads the same unit, else the first pad of
  the same model, else controller 1, and keeps looking for the chosen one every two seconds while another stands in; the
  launcher's pad service (live view, menu navigation) follows the same rule (`pad::chosen_pad`, `pad_mapping` choose_pad);
- joysticks SDL3 has no gamepad mapping for are listed greyed out with "could not map automatically"; an SDL mapping for
  them in `gamecontrollerdb.txt` in the user config folder is loaded by the launcher and passed to the game
  (`SDL_GAMECONTROLLERCONFIG_FILE`). A manual mapping wizard for such controllers is not built.
Checks: launcher tests (choice parsing, chosen pad, status payload, launch contract), pad mapping and standalone fixtures,
the dev page with mock pads (chosen, unplugged, unrecognized). Texts in en-US and pt-BR. Owner test pending.

Owner change (2026-11-04): no "Automatic" entry. The dropdown lists only the connected controllers, with the one the game
reads selected, and is disabled with "No controller" when none is connected. The pad service stores the pad it reads as the
`controller` setting whenever the stored one is not that pad for 2 seconds (`CHOICE_SETTLE`, `choice_to_store`): at every
launcher start the pads are mapped again, the controller chosen last time stays selected when it is connected, otherwise
the first one connected is selected and stored (also when the chosen one is unplugged while the launcher is open). `auto`
stays only as the value before anything is stored (a fresh install). Checks: launcher tests 636, focus checks en-US 275 and
pt-BR 282, nav-test 209 (headless Edge), locale keys 664 in both.

State (2026-11-04, owner): done; the controller choice works on the owner's PCs. Ships in 1.3.0-rc12.

### TASK-149 — CoreGS 2.0: the Nordschleife fences do not load their textures properly (rc13, fixed)

Owner (2026-11-04): on CoreGS 2.0 the fence textures along the Nordschleife do not load properly. Reference: GSdx (final frames
only, clean room). Recording: `nordschleife-fences` (Spec II, card `spec2-replays-20261104`: this PC's Spec II save and the five
Spec II replays of both PCs), a saved race replay played through the fences. Method: the TASK-139 bug hunt
(`procedures/coregs2-replay-bench` in ai-memory). State: recorded (2,919 VBlanks; the replay on the track from about VBlank
1800), investigation next.
First look (2026-11-04, slice401 Spec II, 2x, final frames of GSdx at the same VBlanks, in sync): at VBlank 1900 the far
stretch of the catch fence on the right bank is an opaque dark band on CoreGS 2.0 where GSdx draws see-through mesh with posts;
the near fence, the walls and the guard rails match. The same with `GT4_HW_MIPMAP=off`, `basic` and `full`, so not our mipmap
mode (`GT4_HW_MIPMAP` is not read by CoreGS 2.0). With `GT4_CORE2_MIPMAP=0` (level 0 only) the far fence is see-through mesh
again, close to GSdx: the fault is in CoreGS 2.0's mip levels 1..MXL (Confirmed). The batch of VBlank 1898 (trace) is TBP0 13488,
TBW 2, PSMT4 128 x 128, CLUT CT32 at 13688, TCC 1, alpha test GREATER 32 (fail KEEP), TEX1 MXL 2, MMIN 2 (nearest mip, nearest),
MTBA 0, LCM 0, K -3.3125: levels 1 and 2 (64 x 64 and 32 x 32) come from MIPTBP1. An opaque dark result fits levels read at the
wrong place or not decoded (palette index 0) (Hypothesis). Next: MIPTBP1 in the trace, and the decoded levels.

Cause (Confirmed, 2026-11-04): MIPTBP1 puts level 1 at block 13680 and level 2 at block 13018 (TBW 2). The decoded texture
(`GT4_CORE2_DUMP_TEX`, now with the levels) had level 0 and 1 as mesh and level 2 as another texture's pixels, fully opaque.
GT4 sends the fences by PATH1 and then, before CoreGS 2.0 drew their batch, a 64 x 1024 CT32 upload by PATH3 at block 12224
(262,144 bytes, all of it sent) over 12224..13247, level 2 included. The front end draws a pending batch before an upload
that touches its frame, depth or texture, but it checked only level 0 (`FrontEnd::BatchTouches`), so the batch was drawn
after the upload, with the new pixels in level 2. Not the palette path, the GPU palette, the texture cache, the upload paths
or MTVU (each A/B unchanged).
Fix: `FrontEnd::MipLevelsTouch` adds levels 1..MXL (MIPTBP1/MIPTBP2, or the next level-0 footprint with MTBA) when the draw
uses mip levels (MXL > 0, MMIN 2-5). Test `m3_hw_transfers` (an upload over level 1 or 2 flushes; elsewhere, MMIN 1 or MXL 0
does not). Replay at VBlank 1900: the far fence is see-through mesh, as on GSdx. Diagnostics added: the batch trace prints
MIPTBP1/MIPTBP2/TEXA, and with `GT4_CORE2_BATCH_TRACE_AT` the traced VBlank also lists each GIF transfer's path, each upload
with its bytes, and each memory write. State: fixed on `task/134-game-code-optimization-study`, for rc13 (owner test).

### TASK-150 — CoreGS 2.0: uneven frame times in the used-car showroom (rc13; done)

Owner (2026-11-04, NVIDIA RTX at 3440x1440, CoreGS 2.0 6x): the used-car showroom (the car turning) runs at about 55 FPS with
frame times from 9.6 to 26.6 ms. Recording: `usedcar-showroom` (v2.00, card `avell-gt4-20261104`, the owner's GT4 save from
the laptop), the showroom left on the turning car. Measure with a window run (`WINDOW=1`, `PACED_FROM`), then attack the
largest cost in the `[core2]` accounting. State: recorded (1,684 VBlanks; the CR-X SiR turning from about VBlank 900).

Analysis (2026-11-04, plain rc13 test build, RX 9060 XT, 6x; a peer session ran games on the PC, so no frame-time figures):
the GPU work is moderate (4.3 ms of isolated GPU time per frame by `GT4_CORE2_GPU_PROFILE_AT=1400`: full-screen effect draws
and fills lead; `gpu_busy` 8.9 ms on average with the frame's waits). The uneven frames come from a GS readback every other
frame: GT4 uploads the studio backdrop, a 640 x 438 PSMT8 image, over the memory of the 640 x 224 CT32 frame target at block
6720 (`GT4_CORE2_STORE_TRACE`), and `StoreDrawnBeforeWrite` reads the whole drawn target back (2,240 blocks) so the 8-bit
write merges with its pixels in memory. Each readback makes the GS thread wait for all the GPU work so far, which a slower GPU
at a higher resolution (the owner's RTX 3060 at 3440 x 1440) turns into frames of 20-27 ms. Reading back fewer blocks does not
remove the wait (the last page row of the write is partial). Way forward (design needed, not done): merge such a write on
the GPU (targets into the GLM, the upload over it) and track the CPU memory those pages leave stale, reading it back only if
something reads it (an EE download, a CPU-side decode).

Done (2026-11-04, IN-538): such a write is now merged on the GPU block by block. A block it fills in every bit is taken by the
target from the GLM; a block it writes in part (the backdrop's last 6 rows, 40 blocks) goes through IN-512's bit merge; the
blocks it does not touch keep the drawn pixels. No readback. Frames identical to the store (showroom 32 of 32 and photo mode
26 of 26 at 6x, George V Paris 49 of 49 at 2x). Laptop (RTX 3060 Laptop GPU, 3440 x 1440, CoreGS 2.0 6x, the launcher's
settings, three interleaved runs each): 60 FPS in every run of both builds that day (frame time 16.67 ms, deviation 0.12-0.19
ms); GPU waiting 1.61-1.67 to 0.03 ms per VBlank and stores before writes 0.34 to 0 per VBlank, for about 1 ms more GPU work per
VBlank (9.6-9.8 to 10.7 ms busy). Earlier that night the same laptop showed the owner's stalls with the old build (26 frames
over 20 ms in 680, each with a 4-6 ms GPU wait in the store); the new build removes that wait. `GT4_CORE2_PATCH_BLOCKS=0` (A/B);
test `hw_core2_patch_blocks`.

### TASK-151 — CoreGS 2.0: photo mode flickers to black every other frame and runs slow (rc13; flicker fixed; frame times in TASK-162)

Owner (2026-11-04, same PC): in photo mode (v2.00 only; Spec II disables it) the picture alternates between the right frame
and a black frame, at about 40 FPS with frame times from 13 to 48 ms. Recording: `photo-mode` (v2.00, card
`avell-gt4-20261104`), a Photo Travel location with the camera screen open. Check on both renderers frame by frame
(`frame_compare.py`). State: recorded (1,539 VBlanks; the Beacon Hill camera screen from about VBlank 1000).
Owner (same day): the flicker shows on NVIDIA and AMD alike; the RTX 3060 also drops FPS, the RX 9060 XT holds 60 FPS with very
uneven frame times. Headless at 2x, every composed frame of VBlanks 1480-1539 is the right picture (same brightness, no black
frame), so the black frames come from the presentation path of a window run, not from the drawn picture (Hypothesis).

Cause (Confirmed, 2026-11-04): photo mode shows two 640 x 448 CT24 buffers in turn (FBP 0 and FBP 140, 480i, two VBlanks
each). A whole-memory draw at VBlank 873 leaves a 1024-wide CT32 target at block 0, the same base and format class as the
first buffer. `CircuitSource` showed the newest target of that base and class whatever its width, so the 1024-wide target
(nothing drawn) was shown, black, on every other picture; being shown marked it used every frame, so it never aged out. In a
headless run without frame dumps nothing is composed, the target aged out, and every frame was right (a headless run with
dumps from VBlank 1440 started after it was gone). A window run reproduces it every time: VBlanks 1440-1451 black, black,
right, right. Not the deferred downloads nor IN-532 (A/B). Fix: the target of the circuit's base, class and width first,
another width only when there is none. Window run: 12 of 12 frames right. Diagnostics: `GT4_CORE2_TARGET_TRACE=1` logs every
target created. Still open: the uneven frame times (measure on an idle machine and on the laptop's RTX 3060).
Frame times (analysis, 2026-11-04, 6x): two readbacks every frame (`GT4_CORE2_READBACK_TRACE`): an EE download of an
8 x 4-pixel rectangle at block 10048 (a PSMZ target, likely the viewfinder's light reading) and a CLUT loaded from a Z-format
target at block 11520; plus about 38 texture decodes per frame. Each readback waits for the GPU. Performance Mode already
answers such downloads with the previous result (IN-439, one frame late, decision 0073); normal mode stays exact, so changing
that there is the owner's call. The CLUT one could go through the GPU palette path (IN-408) if it covers Z formats (to check).
Tried (2026-11-04, kept off by default): the CLUT readback comes from photo mode's 640 x 448 Z16S depth buffer, and 16-bit depth
targets can go into the GLM (IN-537); the buffer then counts as read on the GPU, so about 24 uploads per frame over its memory
each need a store first, on the CPU or on the GPU (IN-539). Laptop at 6x: 57.2-58.2 FPS with them against 59.6-60.1 without, so
both are off (`GT4_CORE2_GPU_Z16=1`, `GT4_CORE2_WHOLE_BLOCK_WRITES=1` turn them on). With the flicker fix photo mode holds
59.2-60 FPS on the laptop at 6x (launcher settings, four runs); the rest of its frame times moves to TASK-162 (owner, 2026-11-04:
not a priority; close rc13 with the flicker fixed).

### TASK-162 — CoreGS 2.0: photo mode frame times (after rc13)

Owner (2026-11-04): photo mode is not a priority; rc13 ships with its flicker fixed (TASK-151) and these frame times are left
for later. What is known (TASK-151, IN-537, IN-539): each game frame (every second VBlank) reads back a CLUT loaded from the
640 x 448 Z16S depth buffer at BP 11520 and an 8 x 4 rectangle of the 320 x 192 Z24 target at BP 10048 (an EE download, likely
the camera's focus), 3-4 ms of GPU waiting together on the laptop at 6x; the GS thread spends about 10 ms per game frame on
top. Ways: let the GLM hold pages as the newest copy of memory, so a target read on the GPU needs one store per round of reads
(then IN-537 and IN-539 can be on); or answer that small depth download one frame late, a named relaxation for normal mode that
needs the owner's word (decision 0073 keeps it to Performance Mode). Measure on the laptop only (owner).

### TASK-163 — Bonnet camera wind volume, and 480p colour (issue #135; study #171)

Owner (2026-11-04): taken in as a planned task, after 1.3.0. Issue #135 (shadowcla02-cloud) asks for an option to lower the wind
sound in the bonnet camera and for the full colour range in 480p. Luan Antony's study (`docs/studies/BONNET-WIND-480P-COLOR.md`,
PR #171) finds the wind in the race car sound (a looped voice scaled by speed and a camera factor) and proposes a volume
setting that leaves the engine and every other sound alone; it reads the 480p colour difference as the game's own 16-bit frame
buffers in progressive mode. A sound option changes no gameplay; a colour change would alter the original picture, so it needs
the owner's word first. Check both in the game before building.

### TASK-164 — First save creation: tell keyboard players which key confirms (issue #160; study #167; done in rc14)

Owner (2026-11-04): planned. On a new card GT4 asks whether to create its game data and answers only Cross; players who press
another key think it froze (issue #160, rinegan33). Luan Antony's study (`docs/studies/FIRST-SAVE-CREATION.md`, PR #167)
proposes a first-boot reminder in the launcher, among other options. Smallest step first: a clear hint before the first Play
with an empty card, in both languages.

Done (2026-11-04, rc14): Play shows a note after the shader step and before the start whenever the selected game's library has
no active save and the card the game left behind holds none (`saves::first_start_expected`; command `first_start_hint`). It
names the keys from the player's current controls (default: Cross = `W / Enter`, controller `A`, D-pad right = `Right`) and
says that a question starting on Cancel or No needs Right first (the slice110 pilot: the format question starts on Cancel).
Play starts, Cancel and Back start nothing, a failed check starts at once. Tests: `saves` unit test, flow audit
`play_names_the_first_start_keys_until_the_game_has_a_save`, focus checks (six cases) in both languages; locale keys
`home.first_start_*` in en-US and pt-BR.

### TASK-165 — Xbox and Switch button prompts in the game (issue #161; study #166)

Owner (2026-11-04): planned. Issue #161 asks for Xbox or Switch button prompts instead of the PlayStation ones. Luan Antony's
study (`docs/studies/BUTTON-PROMPTS.md`, PR #166) designs it through texture replacement of the button glyphs plus a launcher
setting (PlayStation, Xbox, Switch). Needs the glyph textures drawn (own or CC0 art) and the replacement path on both
renderers (CoreGS 2.0 and GSdx). No change to the game code.

### TASK-166 — Spec II ultrawide: race-font text and mirror stretched (owner, 2026-11-04; done in rc14)

Owner (2026-11-04): on Spec II, at ratios wider than 16:9, the digital speedometer and the odometer (clearest in the bumper
camera's gauges) look distorted; v2.00 is fine. Cause (Confirmed): TASK-080 P2 patched two immediates of v2.00 into loads from
the host-filled HUD parameter block (`scripts/aspect_elf_patch.py`, decision 0071): the race font x scale (`lui at,0x3F40`, 0.75)
and the rear-view mirror divisor (`addiu t0,zero,640`). The Spec II port (TASK-100 S8) mapped the HUD canvas hook but not these
two words, so every glyph of the race font (lap timer, speed, lap times, gauges) and the mirror kept 16:9 widths in frame
pixels and were stretched with the frame.

Done (2026-11-04): `scripts/spec2_aspect_patch.py` patches the same two instructions of the Spec II derived ELF (0x001C519C in
`RaceDisplay::render` 0x001C5040, 0x001EB65C in the mirror placement) to the same words and block; the pinned translator and
seeds give units byte-identical to the pinned Spec II translation except `unit_0031` and `unit_003A`, one line each
(`private/slice405-spec2-aspect-producer`, receipt `spec2-aspect-generation.json`). The 360 camera hooks apply on top unchanged.
Checks: `tests/python/test_spec2_aspect_patch.py`; George V lap replayed at 21:9 and 16:9 on rc13 and on the patched build:
at 21:9 the timer and the speed keep their 16:9 shape (rc13 stretched them by 21/16); at 16:9 every compared frame is
identical to rc13.

### TASK-167 — Spec II PT-BR + CPSP disc image (owner, 2026-11-04; done in rc14)

Owner (2026-11-04): support the "GT4 Spec II PTBR + CPSP" image: the Spec II 1.10 PT-BR translation with some cars replaced by
Gran Turismo PSP cars, on the same 1.10 base. Checked (Confirmed): its `SCUS_974.36` and `SYSTEM.CNF` are the pinned ones, and
every file under the root, `IRX`, `NET` and `EPSON` is byte-identical to the PT-BR image; only `GT4.VOL` differs (3,447,201,792
-> 3,954,532,352 bytes). The launcher already accepts any image with the pinned executable as a "Spec II 1.10 variant"; the
image is now named `Spec II 1.10 (PT-BR + CPSP)` in `docs/inputs/spec2-variants.json` (sha256 `96557472...`, 4,202,430,464
bytes; `spec2_pins` test). The George V lap recording replayed on it (rc14a Spec II build) boots, races and finishes the lap
with frames identical to the PT-BR image at VBlanks 3000-8000. The image itself stays outside git (`*.iso`).

### TASK-168 — CoreGS 2.0: draw primitives with different palettes in one batch (rc15, with TASK-171; owner, 2026-11-04)

Found by TASK-134's third round (2026-11-04, rc14a Spec II build, the George V lap): a CLUT load forces a batch flush before it
(the batch would otherwise draw with the new palette). On that lap there are about 1,500-1,630 such flushes per VBlank, 1,013-1,020
of them with the same texture, and only 45-105 load an identical palette, so the flushes are real palette changes. Each flush is a
batch through the renderer and the graphics driver; together they are the largest part of the GS thread (the CLUT path costs
2.0 ms per VBlank on the desktop's plain build, 1.5 ms of it the flush). Idea: give each primitive the palette it was drawn with
(a palette slot per draw or per vertex, palettes kept on the GPU) so a palette change no longer ends the batch, with palettes
read from GPU targets (IN-417) kept exact. Large: the vertex format, the shaders, the CLUT upload path and the texture cache
change, so it waited until after 1.3.0 and the owner's word. **Owner, 2026-11-04: in rc15**, as the fix TASK-171's track map points
at (the palette flushes are the largest batch cause on every track in the player's view). Gates: every identity gate, plus the
owner's picture tests.

- Done on `task/168-palettes-per-batch` (e655a630, 2026-11-04): a CLUT load that only changes the palette of the pending batch's
  indexed texture starts a palette segment instead of a flush (each segment keeps a copy of its CLUT buffer; core 2 gives each
  later segment's vertices their own palette row, `GpuVertex` bytes 30-31 as `R16_UINT`). Loads from drawn targets or after a
  GPU palette, CSM2, palette or texture memory under the batch's buffers, possible auto-flushes and texture replacement still
  flush. `GT4_CORE2_PALETTE_SEGMENTS=0` keeps the old rule. Design: `docs/hw-renderer-spec/TASK-168-PALETTE-SEGMENTS.md`
  (its known limit: a target grown by later small batches does not take CPU-written memory, so a merged batch can match the
  CPU reference where the flushes did not).
- Checks: CTest 303/303 on both games (rerun by the coordinator); gates against the rc14 PGO builds and the A-Spec track-map A/B
  in progress.

### TASK-170 — CoreGS 2.0: a shader compile that ends the process no longer stops every start (rc14, done)

A Discord player's rc13 log (RX 6700 XT, RDNA2; issues #187-#190 look the same): the launcher's shader prepare stops at about
131 of 300 variants, then the game on CoreGS 2.0 stalls 6.3 s and 3.1 s at boot and ends with 0xC0000005, every start; GSdx
sessions on the same PC run fine. Hypothesis (High confidence, not reproduced here): one pipeline compile ends the process inside
AMD's RDNA1/2 driver; the parallel prepare (several compiles at once) may matter. The bundle did not carry `logs\shaders.log`.

- Guard (`native/hw_gs/core2/core2_gpu.cpp`): every variant compile (the prepare's workers, the specialised worker and the
  synchronous path) is written to `compiling.bin` in the shader cache folder before it starts and removed when it ends. A start
  that finds keys left there knows a compile ended the process: several keys (a parallel prepare) → the prepare runs one compile
  at a time from then on; one key → that variant is quarantined (`quarantine.bin`) and drawn with the general pipeline instead
  of its own. Both files carry a header (magic, shader set, GPU vendor, device and driver, key size), so a driver update or a
  new shader set starts clean and a damaged file is ignored. The prepare's log line names the quarantined count.
- Log bundle: `logs/shaders.log` (the last prepare step) is now in the exported bundle.
- Acceptance: CTest `hw_core2_shader_guard` (a simulated end inside the 4th compile → serial; a serial end inside the 2nd →
  exactly one quarantined, the rest compiled; the quarantine stays; a damaged file is ignored), with `hw_core2_prepare_shaders`,
  `hw_core2_shader_cache` and the specialised tests; flow audit test for the bundle; every identity gate unchanged.
- State (2026-11-04): in rc14. Player retest (rc14, the same RX 6700 XT, driver 2.0.353; log bundle 2026-11-04 21:49):
  the guard works, but slowly for the player: each Play ran a prepare that ended inside a compile ("the compile did not
  finish"), then the game started anyway and ended (0xC0000005) about 11 s later compiling the same kind of variant; each end
  quarantined one variant, and after 7 the prepare finished (299/299) and the game runs at 7x (an 18-minute session and later
  ones). Scale is not the cause: a 7x session also ran once enough variants were quarantined. On that PC one specialised
  pipeline takes up to 580-790 ms to compile (`compile_max_ms`). The player also uses a texture pack of about 100,000 files.
- **rc15 follow-up:** (1) the launcher runs the prepare again by itself when it ends inside a compile (each run quarantines
  the variant in flight), up to a bounded number of attempts with progress shown, and starts the game only when the prepare
  finished or stopped making progress, so the player never sees the game end; (2) the prepare and the game log the
  quarantined variants with their decoded fields, and the log bundle carries them, so the variants that end AMD's driver can
  be identified (root cause); (3) asked: the player's `data\shader-cache\quarantine.bin`.
- rc15 state (2026-11-04): (1) and (2) are **TASK-170b**, done on `task/170b-prepare-retry` (0e4e6d9e): the launcher repeats
  the prepare up to 12 attempts while each one gets further (guard files changed or more variants done), `shaders.log` keeps
  every attempt under a header, the window says "attempt N" (`shaders.retry`, both languages), and the log bundle gets
  `logs/shader-guard.txt` (both files' headers and keys in hex). Reviewed: launcher 681 tests, focus checks 297/297 en-US and
  304/304 pt-BR. (3) arrived: 7 quarantined keys with no field in common (an intermittent driver crash is suspected, not
  confirmed). **TASK-170c** (planned, after TASK-168): quarantine a key only after it ended two runs, and make the launcher's
  progress rule see that state (today it compares the files' key counts; it should compare their bytes).

### TASK-171 — CoreGS 2.0: per-primitive front end for big scenes (the first license; rc15)

Owner report (2026-11-04): the first license (B-1, Test Course, v2.00) runs below 60 FPS on the laptop (i7-11800H, RTX 3060
laptop) while menus and races are fine: 77-138 late frames per 300 VBlanks in the owner's session. Recording
`private/coregs2-bench/inputs/licenca-b1.txt` (+ `.setup`). Measured headless on the laptop (Confirmed by counters): about
26,000-27,000 primitives and 1,930 batches per VBlank while driving, against 13,000-21,000 on the George V lap; GS thread about
19-20 ms per VBlank, VU1 worker about 15 ms, EE about 13 ms, GPU busy 7-10 ms; rc13 PGO about 21.2 ms against rc14 plain about
23.0 ms. Desktop: 60 FPS paced. Not a doubled pass or a bug: the GS thread's profile is flat over the per-primitive front end
(ShouldDraw, AppendPrimitive, Kick, StripVertices, GIF parsing).

- Plan: cheapest first, one change per candidate; measure on the laptop (headless replay of the license) against the rc14 PGO
  build. Candidates: the per-primitive draw checks and vertex append (fewer branches and copies per primitive), the strip
  path, the GIF packet parse; then batching (TASK-168) if the batch count is the cost.
- Acceptance: every identity gate and the overlay check unchanged; the license replay faster on the laptop with the George V
  lap no slower; the owner's test on the laptop.
- **Track map (owner, 2026-11-04: map every track once instead of optimising track by track).** Spec II Arcade, all 52
  tracks unlocked, headless on the desktop (counts are exact on any machine; times are the desktop at 1x, indicative only):
  `private/task134-tools/t171-route.sh` (the menu route to any track), `t171-survey.sh` (one Single Race per track, Ford GT LM
  Race Car Spec II, or the Focus Rally Car on dirt and snow; B-Spec where the track has it, else A-Spec with the throttle held;
  [hw] counters every 60 VBlanks over 90 s of race), `t171_survey_table.py` (the table). Findings:
  - B-Spec (TV cameras, no mirror) gives about 300-460 batches per VBlank; the player's view (A-Spec: HUD and rear-view mirror)
    gives about 1,700-2,500 on every track measured (Paris George V 2,499 mean, 3,024 max; GS thread 7.4 ms at 1x on the
    desktop). The first license is not special: it has the player's view (about 1,930).
  - The largest flush reason in the player's view is a CLUT load: about 1,340-1,730 per VBlank (session averages), about 1,000
    of them with the same texture (only the palette changes); texture changes about 160-290, PRIM class changes about 185.
  - So the shared cause is batches, not primitives (GS time follows batches; about 3 us per batch in the player's view at 1x
    on the desktop), and the largest share of the batches comes from palette changes: TASK-168.
- State (2026-11-04): planned for rc15 (owner): TASK-168 is the main change; the track map is the before/after check of every
  track at once (plus the laptop for the owner's real settings). The map itself is done (scripts `private/task134-tools/t171-*`,
  survey folders `build/evidence/press-*`); the rc15 A/B reruns it with TASK-168 on and off.

### TASK-172 — An earlier Spec II release ("Gran Turismo 4 Spec 2.iso", owner, 2026-11-04)

The owner's image (3,681,452,032 bytes; SYSTEM.CNF `BOOT2 = cdrom0:\SCUS_974.36;1`, `VER = 1.00`) is **not the Spec II 1.10
program** (Confirmed, compared with the PT-BR 1.10 image): its SCUS_974.36 is 6,518,884 bytes, SHA-256
`0c9db7c5939b352855e760d649d1053a5c9e6379755608f94a9345cf7db0f3b4` (1.10: 6,520,944, `db08421b...`). The game's text and data
segments are identical except the boot hook at `0x001001E8` (`jal 0x0068A760` instead of `0x0068A768`); the plugin segment at
`0x0068A480` is another build (0x24C0 bytes instead of 0x2CCC; 2,284 words differ). Its injector (`scripts/spec2_patches.py
--elf`) writes 34 words instead of 35: the same 30 in the game, four `jal` hooks into the plugin at other targets, and no hook at
`0x00132E60`. The plugin has none of 1.10's `getPerformanceIndex`, `makeNew`, odometer and engine life, `overrideLaps` and
`toggleMultiCategoryAI` strings, so it is an earlier Spec II release (number unknown). GT4.VOL is 3,433,562,112 bytes. The
launcher refuses it today with "a different game program than Spec II v1.10".

- Support means a third game program: its own patch table and derived ELF, its own translation and VU1/IOP checks, the TASK-166
  aspect points and the camera hooks (same game addresses), its own executable, pins and launcher target. About the size of one
  Spec II build plus tests; the image's own assets need their own checks.
- State (2026-11-04): recorded; not in rc14. The owner decides if and when (players can also update their image to 1.10).

### TASK-173 — Spec II: any image with 1.10's code installs (owner, 2026-11-04; done in rc14)

Owner request: support the Spec II 1.10 images people make (translations, car packs) without naming each one, and refuse
anything that is not 1.10. A Tested / Not tested badge was tried and dropped the same day (owner: a compatible image works, an
incompatible one is refused, so there is nothing more to say). Compared on 2026-11-04 (coordinator, three images: PT-BR 1.10, PT-BR + CPSP,
the earlier release of TASK-172): only `SCUS_974.36` and `GT4.VOL` differ between them; the 30 IOP modules (`IRX`), `SYSTEM.CNF`,
`NET`, `EPSON` and `DNAS310.IMG` are byte-identical. The executable holds the whole Spec II plugin, so the executable and the
IOP modules are the game's code; an image is Spec II 1.10 exactly when both match the pins, and only its data can differ.

- Rule (`launcher/crates/gt4l-disc/src/spec2.rs`): the precheck keeps the executable check and now also reads every module in
  `docs/inputs/spec2-variants.json` `iop_modules` (path, size, SHA-256; read from the PT-BR image) and refuses the image when one
  is missing or differs (the same "different game program than Spec II v1.10" message). An accepted image with a pinned full
  hash keeps its name; any other is a "Spec II 1.10 variant" (log and receipt), shown as "other image" as before.
- ISO reader: `Iso::find_path` finds files inside a folder (`IRX/PADMAN.IRX;1`); the synthetic test ISO takes one folder level.
- Acceptance: `gt4l-disc` tests (a changed, missing or shorter module is refused; the same module passes; files found by path;
  the real PT-BR and PT-BR + CPSP images pass with `GT4_FULL_HASH=1`, the earlier release is refused with
  `GT4_SPEC2_EARLIER_ISO`); the launcher's tests; focus checks in both languages.
- State (2026-11-04): done in rc14.

### TASK-174 — CoreGS 2.0: presentation off the GS thread on a GPU with one queue (issue #204, Steam Deck; rc15)

Issue #204 (a Steam Deck player, Proton): stutter in CoreGS 2.0. His log says `[hw] presentation queue: the rendering queue`:
RADV gives the graphics family one queue, so IN-516's second queue is not there and the GS thread acquired the swapchain image
(an unbounded wait) and presented itself, so the display's waits fell on it. The GS thread was also CPU-bound at about 12.8 ms
per VBlank on the Deck (gif_parse 3.6, draw 3.2, translate 1.67), which TASK-168 helps.

- Change (branch `task/174-single-queue-present`, 34516dfe, 02afc12d, dcbbe4d9): on a GPU whose graphics family has one queue
  the GS thread draws the frame into the IN-516 picture and hands it to the presentation thread of TASK-082, which acquires,
  copies and presents on the same queue; every queue call of both threads, the renderer's and write-back submissions included,
  holds one lock (`hw_queue_lock.hpp`). Nothing on the GS thread waits for the display: a frame the presentation side cannot
  take is not shown and counted. `present_paced` (second queue, frames left out) got the same slot rule: before, it could draw
  over a picture whose copy had not run. Switches: `GT4_HW_PRESENT_SHARED_QUEUE=0` (old path), `GT4_HW_PRESENT_FORCE_ONE_QUEUE=1`
  (a desktop runs the Deck path), `GT4_HW_PRESENT_SHARED_WAIT=<n>` (diagnostic). New `[hw] presentation` line. Design and
  limits: `docs/PRESENTATION-ONE-QUEUE.md`.
- Which GPUs: one graphics queue on RADV (Confirmed, #204 log) and maybe Intel (Hypothesis); AMD Windows (8 queues here) and
  NVIDIA keep their own queue and are unaffected.
- Checks: CTest 309/309 (rerun by the coordinator). Windowed desktop test (forced one queue, v2.00 night race 2x, 144 Hz,
  indicative): the GS thread's presentation step 0.04 ms against about 0.5 ms (old and own-queue paths) in the race, every race
  frame shown, frames left out, resize, minimise and fullscreen clean, 0 validation and thread-check errors; one 86 ms hitch in
  one of two runs (a present blocked 73 ms under the queue lock, AMD Windows FIFO; the repeat run had none).
- **Launcher option (owner, 2026-11-04; branch `task/174b-launcher-option`):** a Graphics row, on by default, with an (i) note;
  the launcher reads the graphics family's queue count and shows the row as already active (disabled) on GPUs with two or more;
  off passes `GT4_HW_PRESENT_SHARED_QUEUE=0`. The same branch makes `system.txt` richer (owner): CPU cores and SSE4.1/AVX2, RAM,
  the Windows build and Wine/Proton, a readable GPU driver, refresh and scale, free disk space, power, controllers; no names or
  serial numbers.
- Still to test: the Deck player's `[hw] presentation` line with and without `GT4_HW_PRESENT_SHARED_QUEUE=0`; the laptop with
  HD 620 when the owner frees it ("not now" on 2026-11-04).
- State (2026-11-04): game side reviewed and tested on the desktop; launcher side in progress.
- State (2026-11-05): TASK-174 (314b0236) and TASK-174b (c8d20c08) merged into `release/1.3.0-rc15` and in the rc15 test
  build (the three pt-BR texts of the option, written as mojibake on the branch, repaired at a07f63ad). Still to test: the Deck
  player's log.

### TASK-175 — CoreGS 2.0 performance HUD: full names, the report state, two modes (owner, 2026-11-04; rc15)

The owner's photo of the HUD on the RTX 3060 laptop: the GPU name is cut ("NVIDIA GeForce RTX 3.."). Asked:
1. the GPU name never cut; the CPU model on its own line;
2. the performance report's state: "Recording" during an F7 interval, "Saved" for a few seconds after F8, then "Idle"; the
   line is not drawn when the report is off in the launcher;
3. launcher choice Off / FPS only / Full (FPS only draws just the FPS; Full everything); an old "on" becomes Full.
- Branch `task/175-performance-hud`; tests: HUD fixture (fit rule, CPU line, report state machine, FPS-only layout), launcher
  mapping and compatibility, focus checks in both languages; the coordinator checks it in a game run.
- State (2026-11-04): in progress.
- State (2026-11-05): 2e2f87f3 merged into `release/1.3.0-rc15` and in the rc15 test build (beside TASK-174's
  `hw_present_shared_queue` in `launch.rs` and `standalone.cpp`). The HUD's look in a game run is still to check.

### TASK-176 — Experimental 120 FPS races with the simulation at 120 Hz (study; not in any rc; owner, 2026-11-04)

The owner found a Japanese PCSX2 cheat page (Vyerq, "make the race screen 120 FPS, simplified", last updated 2024-09-10;
the site is offline) and sent a research summary and an English copy of the page's code archive. They are kept as
**references only** in `private/references/vyerq-gt4-patches-2026-11-04/` (git-ignored; third-party patches are never
copied into the repository, AGENTS.md). The patch writes 64 bytes in v2.00 (two `byte,19` writes at 0x357F78 and 0x357FBC,
62 `byte,8` writes from 0x10110C to 0x57B038) and 68 in the Online Public Beta (not checked against Spec II 1.10), and then
runs PCSX2 at 200 % speed on a 120 Hz screen: the game is made to step its race at half the time per frame while the
emulator doubles the frame rate.

- Different from TASK-082 (`docs/NATIVE-VARIABLE-FPS-STUDY.md`: real frames with the 1/60 s simulation untouched) and from
  TASK-045: here the race simulation itself steps at 120 Hz. That changes game logic (physics steps, AI, lap timers,
  replays and ghosts are tied to 1/60 s steps), which the mission keeps out of the faithful build ("game logic never
  changes"; physics timing changes belong to a separate project or fork). So this is an experimental branch only, never a
  default, and any release use is the owner's decision.
- Plan when the owner starts it:
  1. Map every v2.00 location to its instruction, original bytes and function (gt4-decomp names): float 1/60 constants,
     integer step counts or other values, and what each one drives.
  2. Find the Spec II 1.10 equivalents from the code, not from the Public Beta offsets.
  3. Design our own hook (no patch bytes copied): a switch for a 120 Hz race step plus the runtime's VBlank and pacing at
     120 per second.
  4. Measure against the 60 Hz baseline on the replayed laps: race clock, positions and speed at fixed real times, the
     simulation state, AI, sound, replays and ghosts, menus.
  5. Report what holds and what breaks.
- State (2026-11-04): recorded, not scheduled; rc15 stays the focus (owner).

### TASK-177 — Per-track renderer settings, only where a general optimisation is not enough (owner idea, 2026-11-04)

Owner: an optimisation that helps one track may hurt another; if no general change fixes the heaviest tracks, read from game
memory which track the race loaded and apply that track's own setting, starting with the worst ones (Motegi Road Course and
Hong Kong at 6x on the Avell). Needs the address of the loaded course id (gt4-decomp names), a small scene hint from the EE
side to the renderer, and a per-track table with its own identity checks. State: recorded; general optimisations first
(TASK-168b palette segments, the per-frame download study).

### TASK-178 — CoreGS 2.0: restart the GPU right after the per-frame flare read (rc15 optimisation; 2026-11-04)

Study: `docs/hw-renderer-spec/STUDY-FRAME-DOWNLOAD-COST.md`. The per-frame download is GT4's light-flare visibility table
(64x8 PSMCT32, 2 KiB; lamps, headlights and the sun), read at the end of the world pass. The copy is tiny; the cost is that
the GS worker waits for the whole GPU queue, which grows with the scale, and the GPU then idles until the next submission.
Ceiling of the whole wait: about 4.1 ms/VBlank in Hong Kong and 2.0 at Motegi (6x, Avell), but Hong Kong at 6x is also
GPU-bound (about 15 ms of GPU work per VBlank). First step (O1, exact): submit once K batches after any waited submission
(`GT4_CORE2_SUBMIT_AFTER_WAIT=K`, 0 = off), plus idle, I_cpu and reply-hash counters, and three traces (a GPU profile on a
Motegi sun frame and on Hong Kong, the Motegi store-before-write readback, Hong Kong's readback targets). Branch
`task/178-submit-after-wait`. Keep it only with a measured gain (at least 0.3 ms/VBlank on two scenes on the Avell).
Result (2026-11-05, Avell, 18 runs at 6x): no gain. Wall per VBlank K0/K16: Hong Kong 16.68/16.69, Motegi 11.32/11.39,
George V 18.86/19.03. GPU idle after a drain fell, but the GS worker rose. So the default stays off; the switch and the new
counters stay. Next on the same branch: O3, exact. In Hong Kong the 4-pixel table read reduces whole old targets (640x224
CT24 and 128x128 Z24, 99.9% of the downloaded pixels), so limit the reduction to the read's blocks. Traces in the study's
section 9: GPU time is about 30% full-screen post/glare passes and 15% world geometry; Hong Kong dirty patches 18%;
Motegi Performance mode has a 16x16 CT32 store into a drawn Z24 target every second VBlank (IN-539, off by default). TASK-168 found on the Avell: no gain as built (segments are split back for GT4's T8-over-target batches); the
fix is TASK-168b (`task/168b-segments-fix`). Owner (2026-11-04): no hurry; keep optimising, change approach when one fails.
In rc15 (2026-11-05): `task/178-submit-after-wait` at c73b3be8 only (K=0 by default, the counters kept); the branch's
uncommitted O3 work is not in it.
O3 result (2026-11-05, desktop RX 9060 XT, 6x/10x/12x, Spec II B-Spec Hong Kong and Motegi; study section 9.8): **dropped**.
`GT4_CORE2_READ_BLOCKS_ONLY=1` cuts 98% of Hong Kong's downloaded pixels, but it is not exact. One Motegi flare reply in
6,588 differs at 6x, and the final hash differs at 10x and 12x: GT4's flare chain draws overlapping 64x192 scratch windows
at a new base each frame, and cut targets stay drawn. It also brings no gain. Wall per VBlank, off to on:
- Hong Kong: 6x 10.95 to 11.96, 10x 17.79 to 19.10, 12x 26.53 to 24.97;
- Motegi: flat at every scale.

An exact O3 would need a deferred write-back layer in front of every GS memory reader and writer, for at most the CPU
write-back of the extra pixels (0.30-0.33 ms/VBlank). It stays only on the task branch (80cf04c9, off by default, with the
fixture, the per-download pixel counters and `GT4_HW_FLARE_REPLY_TRACE`) and is not merged anywhere. Next GPU step:
TASK-182.

### TASK-179 — Android stage A1 on Windows: Vulkan correctness gaps (owner, 2026-11-04; in parallel to rc15)

From `docs/ANDROID-PORT-PLAN.md` section 2: features used without being enabled (`shaderStorageImageArrayDynamicIndexing`,
`largePoints`, wide lines: query, enable or a same-pixel fallback), required formats core 2 never uses (no longer refuse a
device), a rotation-safe present, host rings sized from the heaps and `VK_EXT_memory_budget` (today's sizes as the cap), and a
start-up capability profile struct (today's integrated-GPU behaviour unchanged). Frames identical to rc14 (gates6), validation
layer clean, CTest. Out of scope: Vulkan 1.1, precise/invariant interpolation, no-interlock tuning, D1, determinism items.
Branch `task/179-vulkan-correctness`. State (2026-11-05): done (4a70b53d), merged into `release/1.3.0-rc15` with its
rotation-safe present; the point-quad vertex stage now also carries TASK-168's palette row. Android target (owner, 2026-11-04): his S23 Ultra (Adreno 740),
at least 2x in races, 3x if possible. Suggested next step after A1, cheapest first: build the renderer alone with its tests
for arm64 Android and measure the no-interlock path on the S23 (owner: later, not now). Also on 2026-11-04: an Avell comparison of GSdx and CoreGS 2.0
(normal and Performance) at 6x on Motegi Road Course and Hong Kong (owner).

### TASK-180 — Android: the game boots in the Android Studio emulator (owner, 2026-11-04; Phase 4 started)

Owner, 2026-11-04: focus first on opening and running the game in the PC's Android emulator; when there is an APK, upload
it to GitHub as a private draft for him to test. Steps:
1. The platform seam of the Linux plan (L0), Windows unchanged and identical.
2. An NDK build of `libgt4.so` for x86_64 (the emulator) and arm64-v8a (phones, 16 KB pages) with `-ffp-contract=off`.
3. A minimal Gradle app with an SDLActivity game Activity in its own process; for the first runs the disc and card come in by
   `adb push`.
4. Boot to the title screen at 1x with CoreGS 2.0, v2.00 first.
Downloads approved by the owner: the emulator system image (Google, sdkmanager), SDL3 3.4.18 for Android (libsdl-org) and
Gradle. Installed: SDK platforms 34/36/36.1, NDK 26.1, build-tools, emulator (no image or AVD before this). Branch
`task/180-android-emulator`. State: in progress.

**APK test1 (2026-11-05)** is the GitHub draft `v1.3.0-android-test1` (arm64, branch at 95056cd0; the release signing key
is in `private/android-signing`, and every later APK must use the same key).

**Owner's S23 Ultra test (Adreno 740, Android 17), from his log bundles:**
- **Nordschleife race, Performance mode:** about 60 VBlanks/s at both 1x and 2x. GPU busy is about 11.5 ms/VBlank at both
  scales, with draw passes about 9.5 ms. That is fixed per-pass and per-barrier overhead, not fill: per VBlank, about 750
  fragment barriers, 75 full barriers and 20 render passes.
- **Hitches:** 5% of frames skipped, audio underruns, and in-game pipeline compiles of 2.2 s per session (up to 44 ms each).
- **Showroom:** 20, 46 and 36 VBlanks/s (normal, Performance 1x, Performance 2x).
- **Normal mode:** readback waits of 1.4 to 4 ms/VBlank.
- **FXAA plus FSR1 at 2x:** GPU 11.5 rises to 15.5 ms. Both filters run at 3088x1440.
- **Swapchain:** every present reports suboptimal, because the identity pre-transform makes the compositor rotate.
- **Launcher killed:** the launcher was killed twice ("dependency died": an open ExternalStorageProvider descriptor).
- **Black screen:** after a round trip to Settings the screen stayed black while the game kept running.

**Owner decisions (2026-11-05):**
- Performance mode stays OFF by default on Android; make the normal mode lighter instead.
- Snapdragon GSR is a later candidate to replace FSR1 on Android. Check its licence and A/B it against FSR1.

**APK test2 scope (in progress):** merge rc15 (HUD); render scale 100% = native screen, with the slider below 50%; the
HUD on Android with the SoC GPU, resolution, and GPU and CPU use; Export logs carries 10 sessions.

The owner also asked for:
- Back pauses the game and asks Close or Minimize; the launcher shows Resume while the game lives;
- pause on background, and the surface and swapchain rebuilt on return (the black-screen fix);
- the provider fix (`detachFd`);
- the first-open shader prepare, as on Windows;
- Play preselected on the home page;
- F8 per-cluster thread residency and thermal status.

**Test3 (on the device over USB):** pre-rotation, ADPF hints and big-core placement, FXAA at internal resolution, a
1080p-output option, and Snapdragon GSR.

**APK test3 scope (owner's S23 test of test2, 2026-11-05 13:54):** bench mode from `debug.env` (a recording's own card,
never the player's); the game's input path made visible and fixed for a pad that only Android sees; the Wheel page gone on
Android; the launcher's pad state registered (rebind and the live view); the Render scale as a percentage of the screen
height on every platform, migrated once; the shader prepare's Cancel no longer focused; the pause dialog centred, in the
launcher's language and pad-driven; the version from one source (`1.3.0-android-test3`); Windows home preselects Play too.
Spec II on Android (Part B): the `GT4_TARGET=spec2` NDK recipe and `libgt4spec2.so`'s name are in place, but the APK still
ships v2.00 only (the hooked Spec II unit set has to be regenerated with `scripts/camera360_units.py spec2` first), and a
bench for Spec II is refused with a message instead of starting the wrong library.

**APK test4 (owner's S23 test of test3, 2026-11-05 ~17:50):** the controller and the Render scale are fixed; the
Performance report came back missing (its Settings row and the in-game F7/F8 touch button), which is this build's first
fix. Scope in order, easiest and biggest gain first:
- **The TASK-182 exact savings on by default on Android:** `GT4_CORE2_DEINT_FIELD_COMPOSE=1` (a deinterlaced frame's
  display merge straight into that field's history image: two full-display copies and two full barriers fewer per
  deinterlaced VBlank) and `GT4_CORE2_DEINT_SKIP_DECISION=1` (the adaptive kernel skips the motion tests its loaded words
  already decide). Set by the Android launcher's game environment, overridable from `debug.env`, logged by the game
  (`[core2] deinterlace order (TASK-182) ...`). Windows defaults unchanged. Merged first:
  `task/182-barrier-scope` at 8d25d4e4 (steps 2a-2c), journal conflict resolved keeping both sides.
- **Single-queue present on Android:** `GT4_HW_PRESENT_QUEUE=0` (the rendering-queue path), so the picture is drawn into
  the acquired swapchain image instead of a window-sized picture plus a second copy: about 36 MB less per presented frame
  at 3088x1440 (STUDY-UNIFIED-MEMORY 4.2). Presentation only. Logged by the backend (`[hw] presentation queue`).
- **Unified-memory phase 1** (STUDY-UNIFIED-MEMORY 7.2, P1-A to P1-D): memory-shape classification with
  `GT4_HW_MEMORY_SHAPE`, the memory-type and placement logs, per-path byte counters with ring high-water marks
  (`[core2] data paths/vblank`), and the spec-required host-read barrier (`GT4_CORE2_HOST_READ_BARRIER`, on by default).
  Tests: `hw_device_profile` extended, new `hw_core2_data_paths` (three captures at 1x and 2x, barrier off against on:
  identical images and GS memory, counters and rings checked). No rendering change.
- **The Performance report fix (test3 regression):** TASK-118 hid its row and forced it off in any build without "-rc" in
  the version, and test builds are labelled `1.3.0-android-testN`; `gt4l_core::is_test_build` now accepts both test
  labels, an Android test build comes with the report ON unless the player chose a value, and the in-game touch button
  moved from the middle of the left edge (over the owner's front camera) to the top-left corner, offset by the window
  insets (display cutout and system bars). Tests: a core test of the version rule, a model test that the row exists on
  Windows and Android for test builds and not in a final build, and a focus-check case for the toggle.
- **Still open for the next run:** unified-memory phase 2 (the final picture drawn straight into the swapchain image with
  pre-rotation, `GT4_HW_PRESENT_DIRECT`) and Spec II on Android (`libgt4spec2.so`: the hooked unit set, both libraries in
  the APK, the Activity picking by game, the launcher's install and Play path).

**APK test5 (owner, 2026-11-06):** the CoreGS 2.0 optimisation work is paused for now (owner, 2026-11-06); this build
keeps the exact TASK-182 savings and adds the Performance Mode split, and Spec II ships on Android.
- **TASK-182 step 3 merged** (`task/182-barrier-scope` at 54d2a0f4): the newest-fence readback wait
  (`GT4_CORE2_WAIT_NEWEST_FENCE`, on), the bounded spin (`GT4_HW_READBACK_SPIN`, off) and the K rule's device default
  (8 batches after a waited readback on a tile-based or unified-memory GPU; 0 on a discrete one), logged on the
  `[core2] submissions` line. Android also gets `GT4_CORE2_SUBMIT_EVERY=128` (measured on the S23 showroom bench:
  +11.6% VBlanks/s, 32% fewer render passes; only submission boundaries change). The K default is clamped to it.
- **Performance Mode is replaced by independent effect toggles** (owner, 2026-11-05), on by default, on the Graphics page
  for both renderers:
  - *Sun glare and light flares*: the 64 x 8 flare visibility table is answered as zeros, so neither is drawn and the
    processor never waits for it. One toggle: one table feeds both (IN-441), so they cannot be split.
  - *Road sheen*: the full-screen sheen pass and its alpha-lane fills are left out (IN-501/502). CoreGS 2.0 only (its
    classifier is core 2's); the GSdx path streams raw GIF and has none, so the row is not offered there.
  - Not player toggles: delayed downloads (it only made the hidden flare read cheap; the zeros answer supersedes it) and
    small effect targets at 1x (a resolution choice, not an effect on/off).
  - Switches: `GT4_CORE2_PERF_FLARES=1`, `GT4_CORE2_PERF_ROAD_SHEEN=1` turn one effect off; `GT4_CORE2_PERFORMANCE=1`
    still means all off; the older names win as A/B. The GSdx path gets `GT4_GS_HIDE_FLARES` from the launcher, and the
    game-side stream client also accepts the new name; the gt4-gs fast download mode is written only when the flares are
    off. Settings in `[gs]` (`effect_flares`, `effect_road_sheen`), schema 10 migrates a stored `no-readbacks` to every
    effect off.
  - Fixtures: `hw_core2_effect_toggles`, the stream client fixture (new name → zeros), launcher rows/env/migration and the
    Python table twin.
- **Spec II on Android:** the hooked Spec II unit set was regenerated (`scripts/camera360_units.py spec2`), `libgt4spec2.so`
  builds for arm64, both libraries are packaged, and the game Activity loads the library of the chosen game (extra
  `GAME`); the launcher's Play uses the game's own ELF and `GT4_BENCH_GAME=spec2` is no longer refused. The boot test in
  the emulator could not run: the only Spec II images on this PC are PTBR builds whose ISO hash does not match the pin
  (`docs/inputs/spec2-iso.json`).
- **Test5 Spec II follow-up (TASK-180b, 2026-11-06):** two pieces the Windows build splices and the Android build did not.
  (1) `android/native/CMakeLists.txt` never compiled the Spec II runtime capsule's `iop/sif.cpp` (the TASK-100 S7b
  asynchronous FILEIO server), so the game stopped at the `dev9x:` devctl after `INETCTL.IRX`: black picture and ~1 s per
  guest frame inside `run_due_events`. (2) It never defined `GT4_TARGET_SPEC2`, so the Spec II library ran the **v2.00**
  guest addresses of every host hook and stopped after the publisher screens repeating seven GIF records. Both are now set
  by the Android CMake (`sif.cpp` spliced and pinned, `GT4_TARGET_SPEC2` defined, only `v200`/`spec2` accepted at all);
  with them Spec II plays its opening and reaches its title flow in the emulator. See `docs/STATUS.md` and the 2026-11-06
  journal for the full Windows/Android splice table.
- **Unified-memory phase 2 is cancelled for this build** (owner pause).

**One app (owner, same day; APK rebuilt as `1.3.0-android-test3`):** the game runs in the launcher's own task
(`singleTop`, no task affinity, black window theme, no activity animation), one card in recents; the app icon enters
through a windowless `LaunchGateActivity` that starts the running game or the launcher and finishes, because a
`singleTask` launcher resets the task (destroying the game) and a `singleTop` one pushes a second launcher on top; Minimize
brings the launcher to the front of the same task with the game alive below and Resume takes it back (focused, after the
WebView settles); Back on the launcher leaves the app in the background instead of finishing the task under a live game.
Checked in the emulator, step by step with screenshots: Play, Home, icon → game, recents → game (one card), Back → pt-BR
pause dialog, Minimize → launcher with Retomar focused, Resume → game, Close → launcher with Jogar focused, Play → a new
session.

### TASK-181 — Launcher Wheel page for the native DFP wheel (owner, 2026-11-05; with PR #205)

PR #205 (WaimateWihongi, TASK-018) makes GT4 see a virtual Driving Force Pro on its USB port (option A of
`docs/WHEEL-DFP-PLAN.md`), configured by a `wheel.toml`; it has no launcher UI. The owner wants, for the rc15 test build and
his Logitech G27:
- a launcher **Wheel** page: the detected wheel, mapping of its axes and buttons onto the DFP's controls with capture
  ("press the button"), pedal calibration and dead zones, force feedback strength and a "test force" button, and a check that
  the game's own force feedback really reaches the wheel;
- **automatic mapping for known wheels** (not only the G27: Logitech, Thrustmaster, Fanatec, MOZA, Simagic, Turtle Beach, PXN
  and others, from a researched VID:PID table, `docs/research/WHEEL-AUTOMAP-TABLE.md`); unknown wheels are mapped by hand;
- a nice UI: the on-screen wheel **rotates with the physical wheel** at its real angle, and the accelerator, brake and clutch
  show their real values;
- the base is the launcher wheel UI of PR #101 by **JulioHeroZ** (merged via #109/#111, removed in 1.2.5 because it drove the
  game through the normal pad); credit JulioHeroZ for it in the notes and the credits.
PR #205 was reviewed (in scope, no GPL, the game identical without a wheel file: v2.00 night race and Arcade gates,
CTest 302/303) and **merged into main on 2026-11-05 (a4a3a5b6, owner)**, with a thank-you comment. The fixes the review
asked for are part of this task:
- **High:** a `wheel.toml` without `mode` turns on the withdrawn 1.2.x pad bridge. Make `dfp` the default and remove pad mode,
  keeping SDL enumeration, device ownership, sources and calibration. **Owner (2026-11-05): the native DFP path only; no pad
  bridge. Any wheel goes through DFP: auto-mapped if known, mapped by hand if not. Any other mode stays off and is built only
  if players ask.**
- **High:** the USB capsule (`GT4_USB_RUNTIME_SOURCE`) must be pinned (`private/sliceNNN-usb-runtime` plus a `docs/inputs` pin) and
  built into both games.
- **Medium:** update only the force effects that changed (about 400 DirectInput calls per second today, on the emulation
  thread); run `stop()` once on focus loss; add host-side DFP fixtures with a fake haptic sink; pin the donor hash in
  `prepare_usb_runtime.py` and give `test_usb_runtime.py` a synthetic donor.
- **Low:** the unused VID helpers (MOZA is 0x346E, not 0x1209), old comments, `-Wswitch` warnings.
- **Launcher:** pass `GT4_WHEEL_FILE` at Play and in `game.ini`; validate the file on save, since the game refuses a bad
  one; put `wheel.toml` in the log bundle; add wheels to `system.txt`; close the launcher's SDL and haptic handles before
  Play.
The research table (`docs/research/WHEEL-AUTOMAP-TABLE.md`, written beside the PR): VID:PIDs are well known (SDL's list
of 71 wheels), but button and axis indices are mostly unpublished. So the auto-map uses family templates plus a short
capture wizard, and saves the result as a shareable preset. The old UI lives at cbfd8b8: `ui/js/pages/wheel.js`,
`gt4l-core/src/wheel.rs`, `gt4l-platform/src/sdl_wheels.rs` and the nine wheel commands. State (2026-11-05): done on
`task/181-wheel-page` (a2374ee0), merged into `release/1.3.0-rc15` and in the rc15 test build (USB capsule slice409 in both
games). Next: the owner's G27 test on that build (`docs/WHEEL-NATIVE-DFP.md`).

### TASK-182 — CoreGS 2.0: scope GPU barriers and render passes (2026-11-05; desktop first, also for Android)

**Why.** CoreGS issues about 660-680 FULL barriers (all commands to all commands) per VBlank on the desktop (George V, Hong
Kong). On the S23's Adreno 740 (tile-based), the Nordschleife race costs about 11.5 ms of GPU time per VBlank at both 1x and
2x: about 750 fragment barriers, 75 full barriers and 20 render passes per VBlank. On a tile-based GPU every pass end stores
tile memory, and in-pass barriers can push the driver out of tile mode.

**Target behaviour.** The GSdx oracle's prose report (`docs/hw-renderer-spec/ORACLE-GSDX-TONE-READBACK-FLUSH.md`, D2-D4)
describes it:
- the narrowest barrier per real hazard;
- three paths for self-sampling draws;
- one pass per run of draws to the same targets;
- descriptors pushed only when they change.

**Steps.**
1. **Measure and classify** (no behaviour change; `docs/hw-renderer-spec/STUDY-BARRIER-SCOPE.md`):
   - full barriers by command and hazard;
   - fragment barriers by reason;
   - render passes by end reason;
   - descriptor writes per VBlank.

   Scenes: Hong Kong and Motegi at 2x, 10x and 12x on the desktop. Output: a ranked list of exact changes.
2. **Implement** one change at a time, each behind an A/B switch:
   - exact: flare reply hashes, gates6 wrong=0, CTest;
   - desktop A/B at 10x/12x (owner: the Avell is off-limits; desktop only);
   - then on the S23 over USB.

Clean room: implementation agents never open PCSX2 sources. Branch `task/182-barrier-scope` from `release/1.3.0-rc15`, in the
`gt4-wt-178-download-wait` worktree. State: step 1 in progress.

### rc15 plan (owner, 2026-11-04): what 1.3.0-rc15 carries

The owner gave rc15 two to three days; a plain test build first (no PGO), Telegram when it is ready; the Avell only on the
owner's word (busy on 2026-11-04). Each task is built by a subagent in its own worktree and branch; the coordinator reviews
(diff, tests, gates) and integrates on `release/1.3.0-rc15` (this branch, from `task/171-license-front-end`).

| Task | What | Branch | State (2026-11-05) |
|---|---|---|---|
| TASK-171 | Track map of every Spec II track (A-Spec, B-Spec) headless on the desktop; batches/VBlank, flush reasons | `task/171-license-front-end` | ✅ Done: palette flushes are the largest batch cause in the player's view on every track |
| PR #205 (TASK-018) | Native Driving Force Pro wheel (WaimateWihongi) | `main` a4a3a5b6 | ✅ Merged into rc15 (6288ac5a) |
| TASK-168 / 168b | Several palettes in one batch (palette segments); tone-run early drop; one target walk per CLUT load | `task/168b-segments-fix` (1c9f8240) | ✅ In rc15 (dea88144). Segments no gain on GT4 (the ~1,000 loads/VBlank are tone-sequence copy-back strips); **owner (2026-11-05): segments off by default, code and switch kept**; the early drop and the single CLUT walk on (Avell, George V 1x: wall 12.55 against 12.94 ms/VBlank). Next: study drawing the tone strips as one GPU pass |
| TASK-170b | The launcher runs the shader prepare again while each attempt gets further; `logs/shader-guard.txt` | `task/170b-prepare-retry` (0e4e6d9e) | ✅ In rc15 (892d82f5) |
| TASK-170c | Quarantine a variant only after two crashes of the same key (`suspects.bin`); the launcher compares the guard files' bytes; 20 attempts | `task/170c-two-crash-quarantine` (965dc8e7) | ✅ In rc15 (48d63a64) |
| TASK-174 | Presentation off the GS thread on one-queue GPUs (Steam Deck, issue #204) | `task/174-single-queue-present` (314b0236) | ✅ In rc15 (38a1d9ef); the Deck player's test pending |
| TASK-174b | Launcher: the TASK-174 option with queue detection, and a richer `system.txt` | `task/174b-launcher-option` (c8d20c08) | ✅ In rc15 (a6a855a6; pt-BR mojibake repaired at a07f63ad) |
| TASK-175 | Performance HUD: full names, report state, Off / FPS only / Full | `task/175-performance-hud` (2e2f87f3) | ✅ In rc15 (49acaf41); the HUD's look in a game run still to check |
| TASK-179 | Android stage A1 on Windows: Vulkan correctness | `task/179-vulkan-correctness` (4a70b53d) | ✅ In rc15 (a2390b2d) |
| TASK-178 | Submit after a waited readback (K=0 by default) and the flare-read counters | `task/178-submit-after-wait` (c73b3be8 only) | ✅ In rc15 (6f318594); O3 dropped (not exact on Motegi, no gain; 80cf04c9 stays on the branch only) |
| TASK-181 | Launcher Wheel page and the DFP review fixes (DFP only, USB capsule slice409) | `task/181-wheel-page` (a2374ee0) | ✅ In rc15 (7c9041ad); the owner's G27 test pending |
| TASK-180 | Android emulator | `task/180-android-emulator` | Not in rc15 (owner) |

Then: merge the branches here (TASK-168 and TASK-174 both touch `core2_gpu.cpp` and `native/hw_gs/CMakeLists.txt`; TASK-170b,
174b and 175 touch the launcher locales and settings), CTest, launcher tests and focus checks in both languages, the gates
against the rc14 PGO builds (`private/task134-tools/gates6.sh`), the A-Spec track map before/after, the rc15 notes in both
languages, plain test builds of both games and the launcher, install on the desktop, Telegram the owner. PGO and the GitHub
draft and pre-release only on the owner's word.

**Integration and test build (2026-11-05, morning; owner: a draft for his G27 test).** All the rows above merged here in
order (main, 170b, 170c, 174, 174b, 175, 179, 168b, 178 at c73b3be8, 181), every feature kept (STATUS has the conflicts).
Games slice410, plain (pins `docs/inputs/slice410-*.json`, exe `7060f8bf...` v2.00, `a4d84180...` Spec II, the slice407 EE
libraries plus the USB capsule slice409). Gates against rc14 PGO (`gates6.sh`) wrong=0; CTest 318/318 on both; Python 802;
launcher 750 tests, focus checks 357/357 and 364/364, nav-test 260/260; locale keys 909 in both. Uploaded as the GitHub
**draft** `v1.3.0-rc15` (never published; pre-release only on the owner's word). Not done yet: the A-Spec track map
before/after (TASK-168b's gain was measured on the Avell instead) and PGO.

### rc14 plan (owner, 2026-11-04): a test build first, PGO only on the owner's word

1.3.0-rc13 is published (2026-11-04). rc14 starts after the owner compacts the session's context. Steps:

1. **The camera session's pull request** (one PR into `task/134-game-code-optimization-study`, not merged by that session):
   TASK-159b (the pedal zeroing reaches only pad 0's right stick Y), TASK-160 (field of view in real time, on by default) and
   TASK-161 (trigger range 0-251, off by default, with a launcher switch and locale keys in both languages; it changes
   TASK-147's 45/84 ranges when on). Review, rebuild the hooked units (`scripts/camera360_units.py` and the new hooks), check
   that the race clock, state and frames stay identical with the new options at their defaults, merge.
   Done: PR #197 merged on 2026-11-04 (units `build/cam360/*-units-rc14`, Spec II on the TASK-166 units).
2. **PR #177** (Luan Antony, TASK-095, ultrawide 2D menu canvas): the camera agent finds where the pause, results, replay and
   pre-race screens set their projection in v2.00 and Spec II and adds the hook; it comes as its own PR. Same checks; 16:9 must
   stay identical.
   Done: PR #200 merged on 2026-11-04 (slice407); #177 recorded as merged on main and thanked.
3. **TASK-164** (issue #160): a launcher hint, before the first Play with an empty card, that Cross confirms GT4's question
   about creating its game data. Both languages. Done.
3b. **TASK-166** (owner): Spec II ultrawide race-font text and mirror (the TASK-080 P2 patch table for Spec II). Done.
3c. **TASK-167** (owner): the Spec II PT-BR + CPSP image named as a known variant. Done.
4. **Build and install:** plain test builds of both games (everything enabled), launchers labelled rc14, installed on the
   owner's desktop (installer update mode) and the laptop's portable copy; gates against rc13 with
   `private/task134-tools/gates5.sh` (references `build/evidence/*r13pgo*`); performance measured on the laptop only. Telegram
   the owner when it is ready. No PGO build until the owner asks; then a draft, and a pre-release only on the owner's word.
   Done (2026-11-04): slice407 on the desktop and the laptop's portable copy, gates against rc13 identical.

5. **Close rc14** (owner, 2026-11-04): the full trigger range on by default with the fixed-width meter (TASK-161), the shader
   compile guard and `shaders.log` in the bundle (TASK-170), then the PGO build of both games, the gates against rc13 and the
   GitHub pre-release `v1.3.0-rc14` (the owner's word given). The license front end is TASK-171 for rc15.
   Done (2026-11-04): games slice408 (PGO and ThinLTO from `build/cam360/*-units-rc14b`; exe `a4e1c742...` v2.00,
   `e5267b8f...` Spec II; distribution `private/slice408-dist-130-rc14`). Gates against rc13 (`gates5.sh`, wrong=0): race clock
   and simulation state identical on three Spec II laps and the v2.00 night race, every compared frame identical, VU1/VU0 and
   MTVU checks 0 differences; overlay check 10/10 at 21:9 and 9/10 at 32:9 (one pause-menu fade frame; 32:9 frames identical to
   slice407). CTest 301/301 on both games, Python 790, launcher 665, focus checks 294/294 and 301/301, nav-test 217/217.
   TASK-173's Tested / Not tested badge dropped by the owner the same day.

Waiting on players (issues answered 2026-11-04, `docs/ISSUE-TRIAGE.md`): the AMD crash reports #187-#190 retest on rc13; a new
task if they still crash (TASK-170 is the rc14 answer to the Discord report).

### rc13 plan (owner, 2026-11-04): close the open pull requests before 1.3.0 final

The owner wants every open pull request settled so 1.3.0 can leave the release candidates. For rc13, on
`task/134-game-code-optimization-study`: the CoreGS 2.0 fixes TASK-149 to TASK-151, and the contributor pull requests below,
each re-done by hand on the current code (they were written against 1.2.4/1.2.5), with the contributor credited as co-author,
tests added, then the pull request marked merged by a tree-neutral merge on `main` (strategy "ours", no file of `main` changes,
as for #118, #119, #154 and #174) and its issue answered and closed for the owner. Build: plain (no PGO or ThinLTO), installed on
the owner's desktop and the laptop's portable copy for the owner's test suite; the pre-release only after that test.

Already shipped by us, credit merge only: #138 (gamepad toggles in Settings, issue #123; rc8), #140 and its study #156 (full
trigger travel, issue #134; rc12). Duplicates of what existed before them, closed with thanks: #150 (integrated GPUs at 1x,
TASK-105, rc5), #175 (GSdx graphics API, TASK-103), #122 (`vcruntime140_1.dll`: the game executable does not import it; the
missing-DLL message came with #154). Later, not rc13: #173, #170, #172, #176, #177, #146; study-only pull requests stay open as
references until their topic is implemented. Our own #110 and #113 are superseded by #130.

### TASK-152 — PCSX2 card import: allocation chains up to the card's real capacity (rc13; PR #120, issue #86; done)

Contributor: luanantony (PR #120). A player's PCSX2 `Mcd001.ps2` is refused with "The memory card is damaged (chain entry 7937
outside the allocation area)" while PCSX2 reads it. Re-do the card reader change on `launcher/crates/gt4saves` with the PR's
hostile-card tests. Acceptance: cargo tests, an 8 MB card with chains past the default allocation area imports, damaged cards are
still refused.

Done (2026-11-04): the superblock's `alloc_end` is a count relative to `alloc_offset` (8 MB: 41 + 8135 + 16 backup clusters =
8192), capped by the room on the card; the old reading lost 41 clusters and refused chains near the end, as on the reporter's
card. The chain error now names the layout. Tests (gt4saves 62 pass): a chain at relative 8100, relative 7937 on an
`alloc_end` 7978 card, a chain equal to the usable count still refused, `alloc_end` 0 refused. Not checked on the reporter's
own card. PR #120 recorded as merged (tree-neutral), issue #86 answered and closed.

### TASK-153 — Delete the active save without it coming back (rc13; PR #116, issue #98; done)

Contributor: luanantony (PR #116). Deleting the save (in the launcher, or its files) brought it back at the next Play. Re-do
the PR's change on the current Saves page and save library. Acceptance: launcher tests; deleting the active save leaves the
game to start a new one, and no older card copy restores it.

Done (2026-11-04): the cause was the runtime card (`data/games/<game>/runtime/card.ps2`): each Play harvests it into the
library first, so a save removed from the library came back from it. Deleting the active save on the Saves page now harvests
that card (no unharvested progress is lost), parks it in `backups/<timestamp>-before-delete.ps2` (kept with the New save
images), sends the save to the Recycle Bin and clears its display name; the next Play builds a card without a game save and the
game starts a new profile. Refused while the game runs. Text for the active save's confirmation in both languages; the unused
`rust.saves.active_not_deletable` key is gone (665 keys each). Tests: launcher 641 pass (deleting the active save clears it;
the runtime card is parked and the next card has no game save; refused while the game runs), focus checks 275/282, nav-test
209. Not run in game. PR #116 recorded as merged (tree-neutral), issue #98 answered and closed.

### TASK-154 — The ReShade Home key with GSdx (rc13; PR #121, issue #102; done, owner test)

Contributor: luanantony (PR #121). With GSdx the picture is a child window of gt4-gs, so keys pressed in the game window do not
reach ReShade inside gt4-gs. Re-do the forwarding in `native/window.cpp` next to the TASK-145 heartbeat (decision 0076).
Acceptance: fixtures; the game's own keys unchanged; owner test with ReShade.

Done (2026-11-04): the game window keeps the keyboard focus with GSdx (TASK-038), so gt4-gs never received ReShade's Home. The
window now posts to the gt4-gs picture window only the key presses and releases of Home, End, Insert, Delete, Page Up, Page
Down and Scroll Lock (`native/key_forward.hpp`), never Alt or character messages, and still handles them itself; the game
reads its keys with GetAsyncKeyState, so driving is unchanged, and none of these keys is a default control. The PR's version
passed every key and Alt message on. The heartbeat (decision 0076) is untouched. Test `native_key_forward`, with
`native_window_heartbeat`. Whether ReShade inside gt4-gs acts on posted messages is a Hypothesis until the owner's test with
ReShade. PR #121 recorded as merged (tree-neutral), issue #102 answered and closed.

### TASK-155 — GSdx starts when the install is inside OneDrive (rc13; PR #136, issue #131; done)

Contributor: luanantony (PR #136). gt4-gs found its data root under OneDrive and closed at start. Re-do the PR's
`portable.ini` beside gt4-gs (launcher `gs_release.rs`, the game's `gs_stream_client.cpp`). Clean room: from the PR and the
issue only. Acceptance: launcher tests, a GSdx start from a folder under OneDrive.

Done (2026-11-04): an empty `portable.ini` is written beside gt4-gs.exe when GSdx is installed and, for installs made before,
before each GSdx start (best effort, logged on failure), so gt4-gs keeps its settings beside it instead of looking up a
Documents folder that OneDrive had moved. The game side (`gs_stream_client.cpp`) needed no change: the launcher installs
gt4-gs. Tests: gs_release (marker after install; added once, an existing one kept). The OneDrive start itself is not
reproduced here. PR #136 recorded as merged (tree-neutral), issue #131 answered and closed.

### TASK-156 — Audio crackling on Proton and SteamOS (rc13; PR #141, issue #129; done)

Contributor: luanantony (PR #141). Re-do the audio buffer change in `native/host_io.cpp` only where it does not change Windows'
behaviour unless needed, with an A/B of underruns (F8) on the desktop and the laptop. Acceptance: no new underruns on Windows,
fixtures; Proton is untested here.

Done (2026-11-04): Windows keeps exactly its paced audio (sixteen 256-frame buffers under 60 ms; the fixture checks the default
profile is the old constants, so no Windows A/B is needed). Under Wine (ntdll exports `wine_get_version`) twenty-four
512-frame buffers under 100 ms; `GT4_AUDIO_LATENCY_MS=20..500` sets only the target and passes the launcher's scrub (a
Steam launch option); other values keep the default with a log line. The PR also passed `GT4_PACING` through; not taken
(a stray variable would change Windows pacing). Tests: `native_audio_input` (profiles), `native_pacing`, launch contract.
Proton is not tested here. PR #141 and the study #164 recorded as merged (tree-neutral), issue #129 answered and closed.

### TASK-157 — Choose the custom textures folder (rc13; PR #145, study #155, issue #95; done)

Contributor: luanantony (PR #145, study #155). Let the player point GT4Recomp at an existing texture folder (a PCSX2 one)
instead of copying it. Re-do in the launcher settings and paths, passed to both renderers. Acceptance: launcher tests, texture
pack loads from the chosen folder on CoreGS 2.0 and GSdx, both languages.

Done (2026-11-04): `[install] textures_path` in settings.toml; Settings > Graphics shows the textures folder with Choose folder…
and Use default folder beside Open textures folder, and a hint when the chosen folder is missing. The choice is checked
(absolute, exists, a folder, not the data folder, not inside the renderer folder) and wins at Play over the default and over
a folder not moved yet; a missing or unusable one falls back to the default with a log line and stays stored. The launcher
never writes into a chosen folder (no folders created, nothing flattened), and CoreGS 2.0's shader cache goes to the data
folder for a chosen folder (beside the default one as before). GSdx (`[Folders] Textures`), CoreGS 2.0 (`GT4_HW_TEXTURES`) and
game.ini (`hw_textures`) all take the resolved folder. Commands `textures_state`, `choose_textures_folder`,
`reset_textures_folder`. Locale keys 675 in both languages. Tests: launcher (7 new), focus checks 275/282, nav-test 209. Not run
with a real pack yet. PR #145 and its study #155 recorded as merged (tree-neutral), issue #95 answered and closed.

### TASK-158 — Download GSdx again, or install it from an archive by hand (rc13; PR #143, issue #139; done)

Contributor: luanantony (PR #143). When the GSdx download fails, offer a retry and installing the release archive by hand,
checked against the same pin. Re-do on the current launcher (the CoreGS 2.0 offer of rc10 stays). Acceptance: launcher tests,
focus checks and nav-test in both languages.

Done (2026-11-04): after a failed GSdx download the dialog offers Try again, Install from a file… (a .zip picker) and Close,
with the CoreGS 2.0 alternative kept where it was, and a hint naming github.com/moesuito/gt4recomp-gs/releases. The archive
must be named exactly `gt4-gs-<version>-windows-x64.zip`, at least the minimum version, within the size limit, and match the
SHA-256 line of that release's SHA256SUMS fetched online; with no SHA256SUMS nothing is installed. Both paths share
`gs_release::install_package` (size, hash, minimum version, unpack, check, publish, portable.ini), so the download behaves as
before. Command `install_gsdx_archive`. The PR guessed the version from the name or the zip and checked no hash: not taken.
Tests: launcher 659 pass (8 new archive cases), focus checks 278/285 (Try again, Install from a file, a cancelled picker; the
check's dialog buttons are the shown ones), nav-test 209, locale keys 687 in both languages. Not run against a real release.
PR #143 recorded as merged (tree-neutral); issue #139 (closed with rc10) answered.

### TASK-159 — 360-degree chase camera on the right stick (v2.00, then Spec II; done, in 1.3.0-rc13)

Owner request (2026-11-04). The right stick X turns the chase camera round the car, as the community "360 Degree Chase Camera"
PCSX2 patches do (idea and call site from the patch by Vyerq/unko_ for v2.00 and Nenkai's port from Duchi/Vyerq for Spec II;
references only, no patch words used). Branch `task/159-360-chase-camera` (from task/134 at ad457090). **On by default in
both games** (owner, 2026-11-04; ships in rc13): `GT4_CAMERA_360=0`, or `camera_360=0` in `game.ini` for a direct start, turns
it off; a launcher toggle is a later step. Design, decoded patches, addresses and checks: `docs/CAMERA-360.md`.

- Mechanism: the chase camera's yaw spring calls the angle-difference function with a direct `jal`, which the translator turns
  into a direct C++ call, so `scripts/camera360_units.py` adds one host call per call site to the translated units (proved to
  be the pinned units plus the insertions only) and the EE library is built from them. `native/camera_360*` adds the stick
  angle (pi * (stick - 128) / 127, exactly nothing at rest) to f13; the right stick X byte is the game's own pad buffer byte
  (v2.00 `0x0086F99E`, Spec II `0x0089B0DE`, Confirmed by RAM peeks). The right stick Y is the game's default analog
  throttle and brake in both games (Confirmed); while the camera is on, the analog channel handler's accelerator and brake
  cases get a 0 value, the effect of the community "Disable right analog throttle/brake" patch, whose addresses are v2.00 ones
  (the Spec II equivalents are at `0x0031494C`/`0x00314974`).
- Acceptance: fixtures (CTest `native_camera_360`, `native_game_target`, `native_standalone`; Python
  `test_camera360_units.py`); headless replays (`scripts/coregs2_bench/camera360-check.sh`): with the switch off, frames, race
  clock and state equal the build without the hook; with it on and the stick moved, the race clock equals it at every VBlank,
  the simulation differs only in the camera's own words while the stick is off rest and not at all once it is released, and
  the camera turns and comes back; owner test in game.
- TASK-159b (branch `task/159b-camera-pedal-scope`): the pedal zeroing reached any analog axis assigned to the pedals and a
  wheel's pedals (the handler has two callers); it now acts only in the DualShock analog loop on the right stick Y
  channels assigned to the accelerator or the brake. Checks as for TASK-159, both games, plus a left stick accelerator
  (harness) that still works with the camera on.
- State (2026-11-04): v2.00 and Spec II done and checked headless (Arcade race on v2.00, the owner's Tokyo R246 Time Trial
  on Spec II), on by default; owner test in game pending; the coordinator opens the PR into task/134 after review.

### TASK-160 — Field of view adjustment on R3 + D-pad (both games, on by default)

Owner request (2026-11-04): built in, on by default in both games. Idea from the "FoV Adjustment" PCSX2 patch by Duchi/Vyerq
(v2.00) and Blitznoz (Spec II); references only. Branch `task/160-fov-adjustment` (from `task/159b-camera-pedal-scope`).
`GT4_FOV_ADJUST=0` or `fov_adjust=0` in `game.ini` turns it off. Design, decoded patch, addresses and checks:
`docs/FOV-ADJUST.md`.

- The race cameras' projection function (v2.00 `func_0037A170`, Spec II `0x0023E7E0`) stores the scales 1, 1 and 1 / tan(fov/2);
  the patch rewrote that 1.0 immediate in guest code. Our version: `scripts/camera360_units.py` adds a host call before the
  store call (`0x0037A1BC`, Spec II `0x0023E82C`) that gives the scales k, k and k / t; k = 1 + n/128, 0.5 to 2.0, stepped once
  per guest VBlank from the port 0 button halfword the game keeps (`0x0064C794`, Spec II `0x00681344`): R3 + up, R3 + down,
  L3 + R3 resets; other buttons may be held; no wrap.
- Acceptance: CTest `native_fov_adjust` (switch, steps and clamp, factor); headless replays: switch off and nothing pressed
  identical to the reference; with the combinations the race clock is identical at every VBlank, the view zooms and comes
  back, and only what the camera draws changes in the state (light flares, view lists, Spec II's drawing-written car words).
  The game ignores the combinations in races (both games). Owner test in game pending.

### TASK-161 — Full trigger range for the throttle and the brake (study, then both games; on by default from rc14)

Owner request (2026-11-04): throttle and brake precision is low. Idea from the "Full Trigger Sensitivity" PCSX2 patch by Silent
& Aero_; references only. Branch `task/161-trigger-sensitivity` (from `task/160-fov-adjustment`). Study and design:
`docs/research/TRIGGER-SENSITIVITY.md`; controls: `docs/CONTROLS.md`.

- Study: the game turns a pressure byte p into min(100 * min(2p, 256) / d, 256) with d = 35 (throttle) and 65 (brake): 100%
  from p = 45 and 84, 46 and 85 levels (both games, Confirmed). The host already gives every level (TASK-147 maps the trigger
  travel onto 0..45 and 0..84), so the loss is in the game, not on our side. The reference patch's v2.00 reverse line hits the
  wrong instruction (`0x0043C018` is `slti`, the divisor is at `0x0043C01C`).
- Change: `GT4_TRIGGER_FULL_RANGE=1` (`game.ini` `trigger_full_range`, launcher toggle "Full trigger range (experimental)" on the
  Controls page; off by default until the owner's test): `scripts/camera360_units.py` replaces the doubling and the divisor of
  the throttle and brake cases with host functions (2p and d when off; p and 99 when on: 100% from p = 254, about 255 levels),
  and the same switch moves the triggers' default ranges to the whole byte; `range` applies to the game's scale, the new
  `range_full` to the full one.
- Acceptance: CTest `native_trigger_range` and `native_controls` (every range combination), Rust and launcher tests, focus
  checks in both languages and nav-test; headless sweeps: off identical to the reference, on 254 throttle and 254 brake levels
  with 100% at p = 254, race clock identical at every VBlank. Owner test in game decides the default.
- State (2026-11-04): done on both games and checked headless; launcher toggle, Rust tests, nav-test and focus checks in
  en-US and pt-BR pass. Owner test in game passed; **on by default from rc14** (owner, 2026-11-04): `GT4_TRIGGER_FULL_RANGE`
  unset means on, `0` turns it off; `game.ini` and the launcher default are on. The Controls page meter keeps the width of its
  widest text (100% and the whole byte, plus a margin) with equal-width digits, so the bar no longer changes size while a
  trigger moves (owner, 2026-11-04; focus check in both languages).

### TASK-169 — Save states for endurance races (study accepted; planned after 1.3.0)

Owner (2026-11-04): PR #192's study is accepted and kept here as a planned task; it does not enter rc14 or 1.3.0. Goal: close the
game during a long endurance race and resume it later on the same build. Study: `docs/SAVE-STATE-FEASIBILITY.md` (read-only
inspection of the donor runtime and the 1.3.0-rc line at `232b1437`; no build, capture or restore was run). Findings: the MIT
donor has a machine-state mechanism (CPU, RAM, IOP, timers), but today's stream renderers refuse it and a safe round trip in the
shipping configuration is unproven. Steps it proposes, each with its own gate: an inventory of every mutable state owner and a
compatibility pin (executable, target, disc, renderer, settings); a disposable donor proof of concept on a test card compared with
an uninterrupted run at the same guest boundary; a coordinated capture boundary (EE, VU and GS workers fenced, GPU work finished,
in-flight card and disc I/O settled); a CoreGS 2.0 logical state independent of Vulkan handles (the external GS process separate
or excluded); then the launcher's save and resume flow. Effort is not measured. Starts only on the owner's word after 1.3.0.

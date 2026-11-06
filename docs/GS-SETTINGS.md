# GS settings table, launcher pages and gt4-gs.ini (TASK-039, curated by TASK-049)

Status (2026-10-16, TASK-049; TASK-060/061/062): the table is curated for the launcher (owner decisions 1 to 6) and
since slice201 holds the internal-resolution presets, the cursor, shader-notice and performance-overlay rows; since
TASK-064 (2026-10-17) the Arbitrary Code Guard row is on Advanced, off by default and always written. Source:
PCSX2 v2.8.2 (tag `v2.8.2` = commit `fd9d310c`). Design source: [GSDX-PLAN](GSDX-PLAN.md) sections D.1 and D.2 (see
"Corrections"). **The allow-list is enforced by `gt4-gs.exe` 1.0.0** (section "Allow-list in gt4-gs"): a key or value
outside it is ignored with one warning line; gt4-gs never refuses to start because of the INI.

**Owner changes of 2026-10-21 (TASK-081, release 1.0.0):**

- The launcher's Advanced rows are a **section at the bottom of the Graphics page**, not a page of their own. `page`
  keeps its values: `basic` rows come first on Graphics, `advanced` rows follow under the "Advanced" heading.
- **`aspect` is fixed at 16:9** until the ultrawide update: `page = hidden`, `forced = true`, always written as
  `AspectRatio = 16:9`. `--gs-setting aspect=4:3` and `GT4_GS_ASPECT=auto` are refused.
- **PCSX2's own on-screen display is always off.** `osd_fps`, `osd_gpu` and `osd_resolution` are `page = hidden`,
  `forced = true`, `written = always`, default `false`: every gt4-gs.ini now holds `OsdShowFPS = false`,
  `OsdShowGPU = false` and `OsdShowResolution = false`, and no user value can turn them on. The other stock OSD rows
  (`osd_messages_pos`, `osd_performance_pos`, `osd_scale`, status `resolved`, never written) are hidden too. GT4Recomp's
  own performance overlay (`perf_overlay*`, `[GT4GS] PerfOverlay*`, TASK-062) stays on the Advanced section.
- The allow-list did not change (`forced` is not part of a rule; `false` and `16:9` were already accepted), so gt4-gs
  needs no new build.

**TASK-080 addendum (owner, 2026-10-21, slice218): the internal resolution is automatic.**

- The `upscale` row ("Internal resolution") is no longer offered: `page = hidden` (order 22), still `written = always`
  and `status = ok`. The launcher writes `upscale_multiplier` from its display plan (`gt4l_core::display::display_plan`):
  k = ceil(max(W / 640, H / 448)) for the chosen output size W x H, times the **Render scale** (owner, 2026-11-06: a
  launcher row where 100% is k; it offers the accepted multipliers and stores at most 200%), snapped to a multiplier the
  renderer takes (whole numbers here, ties up) and clamped to 1..12. A stored `[gs] upscale` is dropped by settings
  schema 5 and refused by `Settings::set_gs`.
- **What gt4-gs accepts for upscale:** `[EmuCore/GS] upscale_multiplier` takes the whole numbers 1 to 12 only (the
  allow-list values `1|2|3|4|6|12|5|7|8|9|10|11`; no fractional multipliers, `ExtendedUpscalingMultipliers` is never
  written). gt4-gs lowers a value above the GPU's limit (max texture size / 1280: 12x on 16384-texel GPUs, 6x on 8192)
  with a notice; READY does not report that limit yet. Every value 1 to 12 is a choice of the row, so the allow-list
  and gt4-gs are unchanged (no fork change).
- k per output: 1920x1080 3x, 1920x1200 3x, 1680x1050 3x, 2560x1080 4x, 2560x1440 4x, 3440x1440 6x, 3088x1440 5x,
  3840x2160 6x. Render scale 2560x1440: 50% 2x, 100% 4x, 200% 8x. The default INI (1920x1080 or no monitor known, 100%)
  is unchanged: `upscale_multiplier = 3`.
- `aspect` stays forced 16:9 in the INI: the per-scene fill still comes only from protocol 1.3 `display_aspect`; the
  launcher has no aspect setting any more (always auto from the output size, decision 0071 addendum).

## What it is

`docs/launcher/gs-settings.toml` is the single source for the PCSX2 GS options that GT4Recomp exposes. Each row is one
setting: launcher id, label, help, `page`, `order`, `control`, INI section and key, type, range or choices, GT4 default,
`written`, `status`, `env` (launcher-only rows), notes and sources (file:line at v2.8.2). The table has 71 rows and one
`[[profile]]`, `gt4-defaults`.

`scripts/gs_settings.py` (stdlib and `tomllib`, Python 3.11 or newer) does these things:

- `load_table()` validates the table: schema version, unique ids and INI keys, pages, orders, controls, types,
  statuses and ranges. A broken table is refused with every error listed.
- `settings_from_assignments()` and `settings_from_json()` check user values. Unknown ids, unknown choices, values out
  of range, duplicate setting ids, non-settable rows and launcher-only rows are refused, with an error that names the
  row. JSON duplicates are rejected before conversion to a dictionary, so later values cannot silently replace
  earlier ones (including ids written with JSON Unicode escapes).
- `reset_to_defaults()` returns the `gt4-defaults` profile: the GT4 default of every settable row.
  `render_ini(rows, reset_to_defaults())` is the default gt4-gs.ini, byte for byte (tests prove it).
- `render_ini()` writes gt4-gs.ini text. Sections come in a fixed order (`EmuCore/GS`, `EmuCore`, `Folders`,
  `GT4GS`); rows follow the table order. `always` rows are written with their default. `when-set` rows are written
  only when the value differs from the row default, so an explicit default adds no line. `launcher` rows are written
  only when the launcher supplies them.
- `write_ini_atomic()` writes a temporary file in the same folder, then calls `os.replace`. A failed write leaves the
  old file in place and removes the temporary.
- `offered()` lists the rows the launcher may show, in page and order.

`allowlist()` emits the INI section and key pairs with their accepted values as
`docs/launcher/gs-settings-allowlist.json`: `type`, and `values` (bool and enum: every choice's INI value, whatever its
status) or `min`/`max` (int and float); string and path rows take any single-line text. Every row with a key is
included, whatever its status. Launcher-only rows and rows without a key are not. `upscale_presets()`,
`upscale_preset_for_monitor()` and `upscale_presets_within_texture_limit()` are the launcher helpers for the resolution
presets (section "Internal resolution presets").

## Columns

| Column | Meaning |
|---|---|
| `id` | launcher identifier, used by `--gs-setting ID=VALUE` |
| `page` | `basic` (launcher Graphics page), `advanced` (the Advanced section at the bottom of the Graphics page), `mods` (TASK-048 mod manager; not shown), `hidden` (never shown) |
| `order` | position on its page (unique per page) |
| `control` | `slider` (int range), `toggle` (bool), `dropdown` (enum), `text` (string), `none` (launcher-supplied, path) |
| `label`, `help` | the name and one-line help shown to the owner |
| `section`, `key` | INI section and key (empty for launcher-only rows) |
| `type` | `bool`, `int`, `float`, `enum`, `string`, or `path` (a folder the launcher supplies) |
| `min`, `max`, `choices` | range for numbers; `{name, ini, status, label}` entries for enums; a choice may add `offered = false` (settable from the command line, not shown) and `height` (upscale presets) |
| `accept_ini` | enum rows: the INI value is accepted as input too (`upscale`: `3` = `1080p`) |
| `default` | GT4 default (launcher name for enums; `true` or `false` for bool) |
| `written` | `always`, `when-set`, `launcher` (supplied by the launcher), or `never` (launcher-only) |
| `status` | `ok` (settable), `resolved` (confirmed in the sources, not settable until the owner checks it in game), `pending-TASK-NNN` |
| `forced` | optional; `true` on a hidden, always-written, `ok` row whose value is fixed. Both writers (`scripts/gs_settings.py` and `gs_table.rs`) write the default and refuse any other value ("X is fixed at D in this release"); `offered_choices` returns only the default. A value stored by an earlier launcher is ignored on load. Since 2026-10-21: `aspect`, `osd_fps`, `osd_gpu`, `osd_resolution` |
| `env` | launcher-only rows: the environment variable the launcher reads |
| `notes` | sources (file:line at v2.8.2) and open points |

A value is settable only when its row and its choice both have status `ok`. The launcher offers only rows on the
`basic` or `advanced` page that are settable (`gs_settings.offered()`).

## Launcher pages (owner decisions, 2026-10-16)

### GRAPHICS page (`page = basic`, 4 rows; `aspect` moved to hidden on 2026-10-21, `upscale` on 2026-10-21 by the TASK-080 addendum)

| Order | Row | Control | GT4 default | Status | Why it is here |
|---|---|---|---|---|---|
| 1 | `gs_engine` "Renderer" | dropdown | PCSX2 GS | ok (launcher-only) | Decision 1: PCSX2 GS, or CoreGS 1.1 as the secondary renderer. Not written to the INI. |
| 3 | `fxaa` "Anti-aliasing (FXAA)" | toggle | off | ok | Decision 1. |
| 4 | `cas_mode` "Sharpening (CAS)" | dropdown | off | resolved | Decision 1: FidelityFX CAS mode. Settable after the owner checks it in game. |
| 5 | `cas_sharpness` "Sharpening amount" | slider 0 to 100 | 50 | resolved | Decision 1: CAS sharpness. Used with CAS on. |

Rows with status `resolved` are shown in the layout but not offered until the owner's in-game check turns them `ok`.
Today the launcher offers three Graphics rows from this table (`gs_engine`, `upscale`, `fxaa`). `aspect` was row 3
until 2026-10-21; it is now hidden and fixed at 16:9 (see HIDDEN).

CoreGS 1.1 options (upscale, FSR, FXAA, deinterlace) are hidden for now (decision 5). They are CoreGS controls, not rows of
this table. Only the renderer choice `coregs-1.1` remains.

### ADVANCED section of the Graphics page (`page = advanced`, 40 rows, 22 offered today)

Shown under the "Advanced" heading at the bottom of the Graphics page (owner, 2026-10-21; before, a separate Advanced
tab), followed by "Reset to defaults" (Y on the pad). Pre-configured to the GT4 ideal. Groups in order:

- Renderer and presentation: `renderer` (Graphics API, Vulkan; D3D12 and D3D11 after the owner checks them), `adapter`
  (empty, resolved), `vsync` (always set by `GT4_PRESENT_MODE`), `mailbox`, `linear_present` (present filter, Smooth),
  `integer_scaling` (off).
- Display: `deinterlace` (Automatic, always; TASK-047), `disable_interlace_offset` (off), `antiblur` (on),
  `offsets` (off), `overscan` (off), `fmv_aspect` (off), `tv_shader` (None).
- Filtering and accuracy: `filter` (texture filtering, PS2 bilinear), `trilinear` (Automatic), `anisotropic` (off, the
  60 FPS budget), `mipmap` (hardware mipmapping on, `hw_mipmap`), `blending` (High, always), `dithering` (Unscaled),
  `texture_preloading` (Full), `performance_mode` "Performance Mode" (a toggle, Off by default: Off = Accurate, always
  written; On = NoReadbacks, GSdx only, TASK-091), `accurate_alpha_test` (off), `aa1` (off), `rov` (off, experimental).
- Colour: `shade_boost` (off) and the four levels (50 each).
- OSD: none since 2026-10-21. PCSX2's own OSD rows moved to HIDDEN (forced off); the performance overlay below is
  GT4Recomp's own.
- Other: `disable_shader_cache` (off).
- Overlays (TASK-064, owner decision 2026-10-17): `prohibit_dynamic_code` "Block third-party overlays (Arbitrary Code
  Guard)" (`[GT4GS] ProhibitDynamicCode`, order 40 since 2026-10-21 (was 46), default `false`, always written). Off (default) lets MSI Afterburner /
  RivaTuner Statistics Server draw their overlay; on turns Windows Arbitrary Code Guard back on for gt4-gs, which blocks
  that code injection. The no-JIT guarantee does not depend on this switch (GT4GS_ONLY build and `check-nojit.ps1`).
- TASK-060/061/062 (slice201, gt4-gs 1.0.0): `cursor_autohide` "Hide the mouse cursor" (on; launcher-only, the game
  reads `GT4_CURSOR_AUTOHIDE`), `shader_compile_notice` "Show Compiling shaders notice" (`[GT4GS] ShaderCompileNotice`,
  on), `perf_overlay` "Performance overlay" (`[GT4GS] PerfOverlay`, off; the master switch) and one row per overlay
  item, on by default so the master switch alone shows everything: `perf_overlay_fps` (`PerfOverlayFPS`),
  `perf_overlay_frame_time` (`PerfOverlayFrameTime`), `perf_overlay_cpu` (`PerfOverlayCPU`), `perf_overlay_gpu`
  (`PerfOverlayGPU`), `perf_overlay_resolution` (`PerfOverlayResolution`), `perf_overlay_breakdown`
  (`PerfOverlayBreakdown`). All `when-set`: the default gt4-gs.ini is unchanged. What each item shows:
  [GS-LIVE](GS-LIVE.md) section 11.

Rows marked `resolved` (for example `filter`, `trilinear`, `dithering`, `texture_preloading`, `linear_present`,
`tv_shader`, the shade-boost levels) are on the page but not offered until the owner checks them in game.

Blending High, Performance Mode Off (Accurate) and deinterlace Automatic are the owner's values. The GT4 default for the rest is the
PCSX2 default, unchanged, except where the table says otherwise.

### MODS (`page = mods`, 6 rows, TASK-048)

Texture replacement and dumping: `texture_load`, `texture_load_async`, `texture_precache`, `texture_dump`,
`texture_dump_mipmaps`, `texture_dump_fmv`. Decision 4: they are on neither page. The future mod manager (TASK-048,
Content Manager style) manages them. Status stays `pending-TASK-040`.

### HIDDEN (`page = hidden`, 20 rows)

Never shown to the owner:

- `manual_hacks` and `user_hacks`: UserHacks switches bypass the GameDB fixes that GT4 relies on; per-key rows wait for
  TASK-043.
- `game_fixes` (`EnableGameFixes`, on; the GameDB fixes for GT4).
- `extended_upscale` (`ExtendedUpscalingMultipliers`, off): not needed up to 12x (section 12x below). Never written.
- `cache_folder`, `data_root`, `defer_swap_chain_acquire`, `headless_surfaceless`, `headless_dump_dir`,
  `headless_dump_every`: supplied by the launcher or the headless checks, not by the owner.
- `spin_us` (`SpinUs`), `debug_stall_window_ms`
  (`DebugStallWindowMs`), `debug_stall_after_ms` (`DebugStallAfterMs`): fork keys of tuning and test tools (status
  `resolved`, never written by the launcher), listed so the gt4-gs 1.0.0 allow-list accepts them.
- Since 2026-10-21 (TASK-080 addendum): `upscale` (`upscale_multiplier`, always written, set by the launcher from the
  Resolution and the Render scale; not forced, every choice 1 to 12 stays settable from the command line).
- Since 2026-10-21 (owner): `aspect` (`AspectRatio`, forced 16:9, always written) until the ultrawide update;
  `osd_fps`, `osd_gpu`, `osd_resolution` (`OsdShowFPS`, `OsdShowGPU`, `OsdShowResolution`, forced `false`, always
  written); `osd_messages_pos`, `osd_performance_pos`, `osd_scale` (status `resolved`, never written).

## Reset to defaults

`scripts/gs_settings.py` `reset_to_defaults()` returns the `gt4-defaults` profile: the GT4 default of every settable row
(`status = ok`, `written` `always` or `when-set`, not a path). Launcher-only and launcher-supplied rows are left out.
The launcher writes the reset file with `write_ini_atomic(path, render_ini(rows, reset_to_defaults(rows)))`.
`tests/python/test_gs_settings.py` (`ResetToDefaultsTests`) proves that this reproduces the default gt4-gs.ini
byte for byte.

## Internal resolution presets (owner decision 2026-10-16, slice201; replaced by the automatic plan)

Superseded for the launcher on 2026-10-21 by the TASK-080 addendum (see the top of this page): the launcher no longer
offers presets. The presets below remain the row's choice names and the command-line shortcuts.

The `upscale` row is a dropdown with ONLY these presets, using PCSX2's width-based mapping (640 x scale is at least the
target width): **480p (1x)**, **720p (2x)** (640 x 2 = 1280), **1080p (3x)** (1920), **1440p (4x)** (2560),
**4K (6x)** (3840) and **8K (12x)** (7680). The table default stays 1080p (3x), so the default gt4-gs.ini still writes
`upscale_multiplier = 3`. The other multipliers (5x, 7x to 11x) are choices with `offered = false`: settable from the
command line, never shown.

For the launcher (TASK-019/043; not implemented here):

- **Initial preset from the monitor**: the smallest preset whose height is at least the monitor's VERTICAL resolution
  (480, 720, 1080, 1440, 2160, 4320 lines); a taller monitor gets 8K; an unknown monitor gets 1080p.
  `gs_settings.upscale_preset_for_monitor(rows, height)` is the reference (tests: 768 lines gives 1080p, 1200 gives
  1440p, 1600 gives 4K).
- **GPU limit**: presets above `max(max texture size / 1280, 1)` (PCSX2 `GSGetMaxUpscaleMultiplier`) are greyed out:
  16384-texel GPUs allow every preset, 8192 up to 4K, 4096 up to 1080p.
  `gs_settings.upscale_presets_within_texture_limit(rows, max_texture_size)` is the reference.
- **Fullscreen by default** is a launcher setting (the game's Alt+Enter/F11 borderless fullscreen, TASK-009/038): the
  launcher starts the game window and toggles it to fullscreen; it is not a gt4-gs.ini key.

Command line: `play_performance.py --gs-scale N` stays numeric (1 to 12); `--gs-resolution 480p|720p|1080p|1440p|4K|8K`
is the same as `--gs-scale 1|2|3|4|6|12`; `--gs-setting upscale=4K` (or `upscale=6`) sets the row. Two of them together
are refused.

## Allow-list in gt4-gs (gt4-gs 1.0.0)

`gt4-gs.exe` 1.0.0 embeds `docs/launcher/gs-settings-allowlist.json` (vendored by hash in the fork,
`3rdparty/gt4recomp-gs-stream/`, and turned into a C++ table at configure time). Every key of `--config` is checked:
a key that is not listed, or a value outside its rule, is **ignored** with one line in the session log, for example
`gt4-gs: ignoring [EmuCore/GS] upscale_multiplier = 13 in <file> (allowed values: 1|2|3|4|6|12|5|7|8|9|10|11)`, then
`gt4-gs: N setting(s) of <file> ignored; the session starts with the others`. gt4-gs never refuses to start because of
the INI; the ignored key keeps its built-in default. `[EmuCore]` keys of the list (`EnableGameFixes`) are now applied
too. A table change that adds or changes a key therefore needs a new gt4-gs build (the allow-list is part of it).

## 12x internal resolution (decision 6)

Answer from the PCSX2 v2.8.2 sources:

- The GPU limit: `GSGetMaxUpscaleMultiplier(max_texture_size) = max(max_texture_size / 1280, 1)`
  (`pcsx2/GS/GS.cpp:677-681`). A GPU with 16384-texel textures allows 12x; with 8192 it allows 6x.
- The loader does not refuse a value above the limit. `GSClampUpscaleMultiplier` (`pcsx2/GS/GS.cpp:225-240`) lowers it
  to the device limit and shows an OSD warning.
- `ExtendedUpscalingMultipliers` is NOT needed for 12x and is not needed for any value the table offers. It only lifts
  the PCSX2 UI list and the hotkey cap from 12 to 25 (`pcsx2/GS/GS.cpp:1176`;
  `pcsx2/ImGui/FullscreenUI_Settings.cpp:2975`). The table keeps the row (`extended_upscale`, hidden) and the launcher
  never writes it, so the INI has no extended key at 12x.
- The table range is 1 to 12 (`upscale`, `min = 1`, `max = 12`). `--gs-scale` accepts 1 to 12.
- The device clamp is not yet in the protocol. The owner asked for the launcher to clamp to the limit that gt4-gs reports
  in READY (`max upscale`). The READY payload (`3rdparty/gt4recomp-gs-stream/gs_stream_protocol.h:177-187`) has no such
  field: it has only a reserved `u32`. Until gt4-gs reports the limit (a protocol change in the fork, an open item), the
  launcher offers 1 to 12 and PCSX2 clamps any value above the device limit with an OSD warning. Nothing crashes.

## FSR (decision 1)

FSR 1.0 does NOT exist in PCSX2 v2.8.2. Searching the v2.8.2 sources for `FSR` and `FidelityFX` finds only the CAS
option (`pcsx2/ImGui/FullscreenUI_Settings.cpp:3317`, `pcsx2-qt/Settings/GraphicsSettingsWidget.cpp:685`) and the MIPS
`QFSRV` opcode. No FSR shader or code is in the tree. The only sharpening filter is FidelityFX CAS (`CASMode`,
`CASSharpness`). FSR is not in the table and is not invented.

## Widescreen patches (finding)

PCSX2 has "Apply Widescreen Patches" (`EmuCore/EnableWideScreenPatches`, `pcsx2/Pcsx2Config.cpp:1987`; UI
`pcsx2/ImGui/FullscreenUI_Settings.cpp:3045`). The patches are pnach files: EE memory patches applied to the PS2 program
(`pcsx2/Patch.cpp`). With "Auto" aspect, PCSX2 switches to 16:9 only when a widescreen patch is enabled
(`pcsx2/Patch.cpp:822`).

GT4Recomp runs the game natively: there is no PS2 program memory in `gt4-gs.exe`, which only receives the GS stream.
So a pnach cannot apply. GT4 already runs in 16:9 through the game's own widescreen option and the fixed 16:9 of
TASK-009. The option is therefore NOT exposed (no row, no broken switch). The Aspect ratio row only sets the GS display
aspect. `EnableNoInterlacingPatches` (`pcsx2/Pcsx2Config.cpp:1988`) is also a pnach switch and is not exposed; GT4's
deinterlace is the GS deinterlace (TASK-047).

## Other PCSX2 GS keys (not in the table)

Each key the PCSX2 UI shows, or the loader reads, that has no row. GT4 default: the PCSX2 default, not written, unless
noted. "Not exposed" means no launcher control. Nothing here is written by the launcher.

| PCSX2 key (UI label) | GT4 default | Note |
|---|---|---|
| `EnableWideScreenPatches`, `EnableNoInterlacingPatches`, `EnablePatches` | off | pnach patches cannot apply to the native game (see the widescreen finding) |
| `ExclusiveFullscreenControl` (Allow Exclusive Fullscreen) | PCSX2 default | GT4 runs borderless fullscreen (TASK-038); hidden |
| `SkipDuplicateFrames` (Skip Presenting Duplicate Frames) | PCSX2 default | presentation and pacing belong to the launcher (single clock, TASK-006); not exposed |
| `UseBlitSwapChain` (Use Blit Swap Chain) | off (`pcsx2/Pcsx2Config.cpp:721`) | the present path is gt4-gs's; not exposed |
| `HWSpinCPUForReadbacks`, `HWSpinGPUForReadbacks` (Spin ... During Readbacks) | off (UI default) | latency trade-off; the readback work is TASK-037; not exposed |
| `DisableFramebufferFetch`, `DisableVertexShaderExpand` | PCSX2 default | compatibility switches; not exposed |
| `OverrideTextureBarriers` (Override Texture Barriers) | PCSX2 default | Vulkan barrier workaround; not exposed |
| `GSDumpCompression`, `DumpGSData`, `Save*` (RT, Frame, Texture, Depth, Alpha, Info, Transfer images, Draw stats, Frame stats, HW config), `SaveDraw*`, `PNGCompressionLevel`, `DumpDirectTextures`, `DumpPaletteTextures` | off | debugging and dump tools; hidden |
| `UseDebugDevice`, `UseDebugBlend`, `HWROVLogging`, `HWROVBarriersVK` | off | debug switches; hidden |
| `ScreenshotSize`, `ScreenshotFormat`, `ScreenshotQuality`, `OrganizeScreenshotsByGame`, `OrganizeVideoCaptureByGame` | UI defaults (window size, PNG, 90) | not exposed; the launcher owns screenshots |
| `EnableVideoCapture*`, `VideoCaptureAutoResolution`, `EnableAudioCapture*` | off | not exposed |
| `StretchY` (Vertical Stretch), `CropLeft`, `CropTop`, `CropRight`, `CropBottom` | 100, 0 | the picture stays uncropped; not exposed |
| `FramerateNTSC`, `FrameratePAL` (Frame rate) | PCSX2 default | the launcher's frame pacing decides the rate (TASK-006); not exposed |
| `VsyncQueueSize` | PCSX2 default | not exposed |
| `OsdShowSpeed`, `OsdShowVPS`, `OsdShowCPU`, `OsdShowGPUDebug`, `OsdShowGPUStats`, `OsdShowGSStats`, `OsdShowIndicators`, `OsdShowSettings`, `OsdshowPatches`, `OsdShowInputs`, `OsdShowFrameTimes`, `OsdShowVersion`, `OsdShowHardwareInfo`, `OsdShowVideoCapture`, `OsdShowInputRec`, `OsdShowTextureReplacements`, `OsdBoldText`, `OsdMargin`, `OsdFontPath` | PCSX2 default | OSD extras; not written (the table keeps FPS, GPU, resolution, messages, performance position and scale) |
| `OsdPerformancePos` | top right | in the table (`osd_performance_pos`) |
| `SWExtraThreads`, `SWExtraThreadsHeight`, `autoflush_sw`, `extrathreads` | not used | software renderer only; GT4 runs the hardware renderer |
| `paltex` (GPU Palette Conversion) | off (UI default) | GameDB-level fix; hidden until TASK-043 |
| `preload_frame_with_gs_data` (Preload Frame Data) | PCSX2 default | GameDB-level fix; hidden until TASK-043 |
| `UserHacks_*` (about 30 keys, the Hardware Fixes and Upscaling Fixes sections) | off | take effect only with `UserHacks = true`; names in the `user_hacks` row notes; per-key rows in TASK-043 |
| `EnableGameFixes` | on | in the table (`game_fixes`, hidden) |
| `HWMipmap` / `Mipmap` | on | in the table as `mipmap` (`hw_mipmap`, the hardware key); `mipmap` is the software toggle |
| `ExtendedUpscalingMultipliers` | off | in the table (`extended_upscale`, hidden); not needed up to 12x |
| `Adapter` | empty | in the table (`adapter`, resolved): names exist only at run time |

## Defaults and changes in TASK-049

The default gt4-gs.ini did not change. `scripts/gs_settings.py ini` (no options) still produces the TASK-036 plus
`HWDownloadMode = 0` file of main, byte for byte. No owner-ideal default changed a value: the GT4 ideal is the default
the table already had, or the PCSX2 default where the owner did not ask for a change.

Behaviour changes that need a note:

- The `when-set` rule: a `when-set` row is written only when its value differs from its default (an explicit default
  adds no line). Before, any value was written.
- `mipmap` now writes `hw_mipmap` (the hardware key). Its default (on) is the same, so no line appears by default. The
  previous key `mipmap` is the software renderer's toggle.
- `upscale` is 1 to 12 (was 1 to 8). `--gs-scale` and `GT4_GS_SCALE` accept 1 to 12.
- New rows (all `resolved` or launcher-only, none written by default): `gs_engine`, `disable_interlace_offset`,
  `accurate_alpha_test`, `aa1`, `rov`, `osd_performance_pos`, `extended_upscale`.
- Rows moved: `aspect` to Graphics; `renderer` (the API) to Advanced; the texture rows to `mods`. `visibility` was
  replaced by `page` (`basic`, `advanced`, `mods`, `hidden`).

## PCSX2 values resolved from v2.8.2

The INI facts below are interface facts (key names and value numbers), not code. Each row's `notes` has the file:line
citations.

| Launcher row | INI key | Values (INI) | GT4 default |
|---|---|---|---|
| `renderer` | `Renderer` | Vulkan 14, D3D12 15, D3D11 3 (D3D resolved, not offered) | Vulkan 14 |
| `filter` | `filter` | Nearest 0, Forced 1, PS2 2, Forced but sprite excluded 3 | PS2 (2) |
| `trilinear` | `TriFilter` | Automatic -1, Off 0, PS2 1, Forced 2 | Automatic |
| `anisotropic` | `MaxAnisotropy` | 0, 2, 4, 8, 16 | 0 |
| `blending` | `accurate_blending_unit` | Minimum 0 ... High 3 ... Maximum 5 | High (3) |
| `dithering` | `dithering_ps2` | Off 0, Scaled 1, Unscaled 2, Force 32 bit 3 | Unscaled (2) |
| `texture_preloading` | `texture_preloading` | None 0, Partial 1, Full 2 | Full |
| `deinterlace` | `deinterlace_mode` | Automatic 0, Off 1, Weave, Bob, Blend, Adaptive (TFF/BFF) 2 to 9 | Automatic |
| `linear_present` | `linear_present_mode` | Off 0, Smooth 1, Sharp 2 | Smooth |
| `cas_mode` | `CASMode` | Off 0, Sharpen only 1, Sharpen and resize 2 | Off |
| `cas_sharpness` | `CASSharpness` | 0 to 100 | 50 |
| `tv_shader` | `TVShader` | None 0 ... Lottes CRT 5, 4xRGSS 6, NxAGSS 7 | None |
| `shade_boost` | `ShadeBoost` | bool | off |
| `shade_boost_*` | `ShadeBoost_Brightness`, `_Contrast`, `_Gamma`, `_Saturation` | 1 to 100 | 50 |
| `performance_mode` | `HWDownloadMode` | toggle: Off = Accurate 0, On = NoReadbacks 2 (TASK-091) | Off, Accurate (0) |
| `osd_messages_pos`, `osd_performance_pos` | `OsdMessagesPos`, `OsdPerformancePos` | None 0, Top left 1 ... Bottom right 9 | Top left (1); Top right (3) |
| `osd_scale` | `OsdScale` | 25 to 500 (percent) | 100 |
| `adapter` | `Adapter` | empty or a name from `GSGetAdapterInfo` | empty |
| `mipmap` | `hw_mipmap` | bool | on |

Readback mapping (TASK-091, 1.2.1): the launcher row is Performance Mode (`performance_mode`). `--gs-readback`
(`GT4_GS_READBACK`) is a shortcut for it: accurate = `HWDownloadMode` Enabled (0, written by default); no-readbacks =
NoReadbacks (2). Unsynchronized (3) is no longer a table value. A shortcut and `--gs-setting performance_mode=...` for
the same row conflict. The allow-list (`gs-settings-allowlist.json`) lists the values of the table, so gt4-gs now
ignores `HWDownloadMode` 1, 3 and 4. The launcher stores the old readback key as Performance Mode On when it was not
accurate (settings schema 5 to 6).

`UserHacks_*`: the names are recorded in the `user_hacks` row notes. Each name is a hidden per-key row for TASK-043.

## Corrections to the plan

| Plan name | Loader name | Note |
|---|---|---|
| `Dithering` | `dithering_ps2` | |
| `HWMipmap` | `hw_mipmap` | TASK-039 wrote `mipmap`, the software toggle. Corrected in TASK-049. |
| `FXAA` | `fxaa` | |
| `ManualUserHacks` | `UserHacks` | |
| `PCRTCAntiBlur`, `PCRTCOffsets`, `PCRTCOverscan` | `pcrtc_antiblur`, `pcrtc_offsets`, `pcrtc_overscan` | |
| `FMVAspectRatioSwitch` | enum of aspect names | was a bool in the table |
| `ShadeBoost*` | `ShadeBoost` (bool) and four level keys | one row per key |

## Using it

Launcher (dev script now, TASK-019/043 later): `scripts/play_performance.py --mode pcsx2`

- `--gs-scale N` sets the upscale row (1 to 12, default 3; the same as `GT4_GS_SCALE`); `--gs-resolution 1080p` sets a
  preset (480p, 720p, 1080p, 1440p, 4K, 8K).
- `--gs-setting perf_overlay=true` turns the performance overlay on (every item); `--gs-setting perf_overlay_gpu=false`
  hides one item; `--gs-setting shader_compile_notice=false` hides the "Compiling shaders" notice.
- `GT4_CURSOR_AUTOHIDE=0` (environment) keeps the mouse cursor visible (row `cursor_autohide`, launcher-only).
- `--gs-setting ID=VALUE` (repeatable) sets any other settable row, for example `--gs-setting fxaa=true`.
  Only with `--mode pcsx2`. The values go to the probe as `GT4_GS_SETTINGS` (JSON). The probe checks them again.
  Resolved rows are refused with "not settable yet" until the owner checks them in game.
- `GT4_PRESENT_MODE` still sets `VsyncEnable` and `DisableMailboxPresentation`.
- `--gs-scale` and `--gs-setting upscale=...` together are refused.

Command line checks (repository root):

```
python scripts/gs_settings.py check
python scripts/gs_settings.py allowlist            # print; --write updates docs/launcher/gs-settings-allowlist.json
python scripts/gs_settings.py ini --set upscale=12 --out <file>
```

## Adding or changing a setting

1. Add or edit a row in the table: `page`, `order` (unique on its page), `control` (the type decides it), `default`,
   `status`, notes with file:line.
2. Run `scripts/gs_settings.py check` and `scripts/gs_settings.py allowlist --write`.
3. Run `tests/python/test_gs_settings.py`. The committed allow-list must match the table.
4. A row becomes settable (`status = "ok"`) only after the owner checks it in the game and the change is recorded.

## Open items

- Fork enforcement: done in gt4-gs 1.0.0 (ignored with a warning, never refused; owner decision 2026-10-16).
- READY max-upscale field: gt4-gs must report the device limit (a protocol change in the fork), so the launcher can clamp
  the slider to it. Until then PCSX2 clamps (see 12x).
- Owner in-game checks to turn `resolved` rows into `ok`: CAS mode and sharpness (decision 1), texture filtering,
  trilinear, anisotropic, dithering, texture preloading, present filter, TV shader, shade boost, OSD position and
  scale, deinterlace and blending levels other than Automatic and High, disable interlace offset, accurate alpha
  test, AA1, ROV, and the D3D renderers (not offered in the Vulkan build).
- Texture replacement and dump rows (`mods`) wait for TASK-040 and the mod manager (TASK-048).
- Per-key `UserHacks_*` rows, the adapter name list and the `EnableGameFixes` decision wait for TASK-043. D.2 also names
  the `Textures` and `Snapshots` folders (not in the table yet).
- Launcher detail: the PCSX2 UI shows Bilinear (Sharp) as the present-filter default, while the loader defaults to
  Smooth. The table keeps Smooth (plan D.1).
- Aspect ratio also has `Stretch` and `10:7` in PCSX2 (`pcsx2/Pcsx2Config.cpp:639-645`). Only the TASK-036 names are
  offered.

Bool rows take their range from the On/Off wording in the plan. Their behaviour in the fork is checked by the owner in
the game menus.

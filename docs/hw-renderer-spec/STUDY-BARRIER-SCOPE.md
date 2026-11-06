# CoreGS 2.0 study: how the GPU is synchronised and fed (TASK-182 step 1), and a ranking of exact changes

Date: 2026-11-05. Branch `task/182-barrier-scope` (base `release/1.3.0-rc15` `02315e95`). Agent status: **not PCSX2-tainted**
(TASK-027 implementation agent; no PCSX2 sources, fork, `private/tooling/pcsx2-*` or `private/slice189-pcsx2-ref` opened; the
only outside input is the clean-room prose `ORACLE-GSDX-TONE-READBACK-FLUSH.md`, sections D2-D4 and "What CoreGS 2.0 could
do", item 1). Step 1 measures and classifies; **no rendering changed, no barrier change implemented, no timing A/B**.

Labels: **C** Confirmed (code read, or a count/hash from a run named here), **H** High confidence, **Hyp** Hypothesis,
**U** Unknown. All counts are per VBlank, means over VBlanks 4800-9600 of the press scenes (the race window; a cumulative
snapshot every 600 VBlanks, so the window is 8 snapshots). Counts do not depend on timing, so a loaded machine does not
disturb them. The only timings here are in section 6 and are indicative (the desktop was shared with an Android emulator and
builds).

## 1. Summary

- **Full (all-commands) barriers:** Hong Kong **566**, Motegi **240** per VBlank, the same at 2x, 10x and 12x (C: the totals
  match to the second decimal at every scale). By hazard (C):
  - target images (read-after-write): 224 and 123, that is **40% and 51%**; the commands are `dirty patch` (105, 29), `patch (cpu
    words)` (60, 46), `self copy` (34, 31), `fill`/`clear` (17, 15). Every one of them declares the **whole image**.
  - the single GPU CLUT buffer: 175 (**31%**) in Hong Kong, 34 (14%) in Motegi; `palette row`, `clut from target`, `clut write`.
  - GLM pages: 104 (18%) and 30 (13%).
  - the hazard at the start of a draw flush (`draws`): 46 and 41 (8% and 17%).
  - the 512-pending overflow: **0**. Readback (`Reduce`, `ReduceMany`): 2 and 1.3. Uploads (`glm upload`): 1.3 and 0.1.
- **Render passes:** 52 (Hong Kong) and 47 (Motegi) per VBlank, 7.8 and 6.6 batches per pass. **65% and 65% end because a
  `self copy` needs the pass closed** (34 and 31 per VBlank, 1.7 batches per pass); 13% end for a `clear`, 5-6% for a `fill`,
  8-10% for the submission cadence (every 64 batches), 2% for the VBlank flush, 1.5% for a readback. **No pass ends for a
  target switch (0) and none for a layout transition (0: every image stays in `VK_IMAGE_LAYOUT_GENERAL`).**
- **Fragment (in-pass) barriers with pixel interlock (this desktop):** 33 and 28 per VBlank, 89-96% "texture is another
  target". **Without it (`GT4_HW_INTERLOCK=0`, the path of most mobile GPUs): 1,103 (Hong Kong) and 593 (Motegi)**, 97% and 95%
  "a primitive may share a pixel with an earlier one". The same exact result either way (flare hash unchanged).
- **Descriptors:** 800 (Hong Kong) and 337 (Motegi) `vkUpdateDescriptorSets` calls per VBlank, 4,139 and 1,781 writes. The
  transient sets are 59% decode sets in Hong Kong. **Pool exhaustion forced 0 submissions** (C).
- **Outside draw passes (section 6):** in a 480i menu, presenting at 2x, the GPU spends 0.68 ms per VBlank of which draw passes
  are 0.10 ms; at 10x it spends 7.2 ms of which draw passes are 0.7 ms and compose/deinterlace 5.1 ms. The non-draw share is
  large on the desktop too (85-90%), and scales with the output pixel count; the absolute ms are small on an RX 9060 XT.
- **Ranked changes (section 7):** R1 draw the self copy inside the open pass (removes about two thirds of the pass ends and
  the `self copy` full barriers); R2 region-granular hazards for target images; R3 CLUT hazards by entry range; R4 barrier
  scope instead of all-commands; R5 per-image tracking of fragment barriers; R6 persistent descriptor sets; R7 GLM below the
  8 KiB page; plus the compose/deinterlace/present items of section 6.

## 2. What was added (diagnostics only)

All in `native/hw_gs/core2/`. Nothing records a command differently; everything counts or timestamps.

| Switch | What it adds |
|---|---|
| `GT4_CORE2_BARRIER_TRACE=1` | A `[core2] sync vblank=N <section> <key>: count=...` line per row at every 600-VBlank summary, cumulative: `fragment_barrier` by reason, `pass_end` by reason (with batches, commands and fragment barriers of the ended passes), `flush` (what asked for the deferred draws to be recorded), `submit` by caller (waited or not), `set_allocation` by kind, and a `totals` line (full and fragment barriers, passes, batches, draw calls, descriptor binds, pipeline, vertex-buffer, viewport, scissor and push-constant records, descriptor update calls and writes, submissions). The existing `barriers by reason` line is finer: image hazards say `raw`, `waw` or `war`, and a barrier no command labelled is named after the function that recorded it. With the switch off the old always-on counting of barrier reasons is gone (it only fed this line), so a normal run does a little less work. |
| `GT4_CORE2_GPU_CATEGORIES=1` | A `[core2] gpu categories ms/vblank draw_passes=... compose_deinterlace=... present=... uploads_copies=... compute=... readback=... start_gap=... unattributed=... (gpu_busy=...)` line. One bottom-of-pipe timestamp (no barrier) is written at each change of kind of work; a kind owns the time from its mark to the next mark (or the recording's end). Draw flushes mark `draw_passes`; `compose`, `Deinterlace`, `Fxaa` mark `compose_deinterlace`; `PresentScale` marks `present`; the transfer and upload commands mark `uploads_copies`; fills, decodes, patches, palette rows, CLUT work and tone mark `compute`; the host-read functions mark `readback`. |
| CTest `hw_core2_sync_trace` | Each switch on against off over the cache fixture and the GPU palette fixture: images and GS memory of every frame identical (exact). |

**How `gpu_busy` is measured today (C, code).** Each recording writes a top-of-pipe timestamp when it starts and a
bottom-of-pipe timestamp when it ends; `gpu_busy` is the **sum over submissions of (last minus first timestamp)**
(`wait_slot`, `stats.gpuNs`), read when the slot's fence signals, in the 600-VBlank line divided by 600. So it **counts the idle gaps
inside a submission**, and because the first timestamp is top-of-pipe, two submissions queued back to back can overlap and
count the same time twice (H). It does not include anything outside the Gpu's command buffers: the blit of the
presented image to the swapchain happens in `hw_gs_backend.cpp` on its own command buffer and is in no counter (H, from the
code; the owner's log can confirm: `present` below is the Gpu's own `PresentScale` into the output-size image, not the
swapchain blit). `gpu_draw_passes` is the sum of up to 64 draw-pass (start, end) timestamp pairs per submission.

## 3. Method

- Build: `build/t178-spec2` (Spec II, plain Release, SSE4.1; the TASK-178 configure recipe `build/t178-configure.cmd spec2`).
  - Base executable (rc15 source with the TASK-178 O3 switch off, built before this change): SHA256 `05d1a922a11abb30c7579be06dbba9a14090b6d7b9214eefe31b8549a0d0918e`.
  - Candidate (this change): SHA256 `a5c581c0f1b4c69262d5fd1a5cee249d070ca6d6ea10e00b178838305fd974c6`. Scene runs and hash gates used the candidate; the 2x baselines used the base.
- Scenes and presses: the TASK-178 press scenes (`private/task178-tools/scenes.sh`, `run_scene`): Spec II Hong Kong
  (reverse) and Motegi Road Course B-Spec, headless, unpaced, `--frames 9876`/`9860`, the card
  `spec2-replays-20261104.ps2`. Command shape: `SCALE=<s> run_scene <hk|mot> <name> <exe> GT4_CORE2_BARRIER_TRACE=1 [GT4_HW_INTERLOCK=0]
  [GT4_CORE2_PERFORMANCE=1]` with `GT4_HW_PROFILE_EVERY=600 GT4_RENDERER=hw GT4_HW_CORE=2 GT4_FORCE_480P=1
  GT4_FORCE_WIDESCREEN=1 GT4_DISPLAY_ASPECT=16:9`. Logs: `build/evidence182/press-trace-*` (ignored by git).
- Tables: `private/task182-tools/sync_table.py <log> 4800 9600` (differences of two cumulative snapshots, per VBlank).
- **Identity (C).** The flare reply hash after the run, base against candidate and against the TASK-178 references:

| Run | Replies | Hash | Reference |
|---|---|---|---|
| Hong Kong 2x | 6835 | `2f4c1cc9f174d00e` | base run at 2x: same |
| Hong Kong 10x | 6835 | `61109e8f3143264a` | TASK-178 O3 run `-k0`: same |
| Hong Kong 12x | 6835 | `7a427a3ccaeb9d4d` | same |
| Hong Kong 2x, no interlock | 6835 | `2f4c1cc9f174d00e` | same as 2x with interlock |
| Motegi 2x | 6588 | `7bf932fbb8799965` | base run at 2x: same |
| Motegi 10x | 6588 | `9ab7085aa7c6d425` | same |
| Motegi 12x | 6588 | `6370c79dfe9d9bcc` | same |
| Motegi 2x, no interlock | 6588 | `7bf932fbb8799965` | same as 2x with interlock |
| Motegi 2x Performance mode, with and without interlock | 6588 | `a0d73fbfe507a325` | equal to each other (Performance changes the picture on purpose, so it has no normal-mode reference) |

- **CTest (C).** Full run on the candidate (`ctest -j 4`, desktop under load): 316 of 318 passed, 1 skipped
  (`hw_replace_q07_pcsx2_dumps`), 2 failed under load and **passed alone**: `vu_recomp_corpus` (the VU1 corpus differential,
  "program did not end" budget, 92 s alone) and `hw_queue_lock` (19 s). The new `hw_core2_sync_trace` (added after that run) passes,
  as do `hw_core2_gpu_clut` and `hw_core2_self_texture` in the second run. The diagnostics are behind environment switches; with
  both off the only change to a normal run is that barrier-reason bookkeeping, which only fed the trace line, no longer runs.

## 4. Full barriers (question 1)

Every FULL barrier is `ALL_COMMANDS` to `ALL_COMMANDS`, memory write to memory read and write, recorded outside a render pass.
Window 4800-9600, 2x; the same at 10x and 12x (C). The "command" is the label of the `touch` that needed it, or the function.

| Class | Hong Kong | Motegi | Commands (Hong Kong / Motegi) |
|---|---|---|---|
| image, read after write (`image raw`) | 224.3 | 123.0 | dirty patch 104.9 / 28.7; patch (cpu words) 60.2 / 45.8; self copy 33.8 / 30.6; fill 9.3 / 7.2; clear 8.1 / 8.1; glm write (target) 4.7 / 1.5; clut from target 3.3 / 1.2 |
| CLUT buffer (RAW) | 113.2 | 25.3 | palette row 62.2 / 11.6; clut from target 40.4 / 10.5; clut write 10.6 / 3.2 |
| CLUT buffer (WAR) | 61.9 | 9.0 | clut from target 50.0 / 4.3; clut write 11.9 / 4.7 |
| GLM pages (RAW) | 91.1 | 26.5 | dirty patch 36.2 / 17.2; glm write (target) 25.0 / 2.7; decode 19.5 / 4.5; clut from target 10.4 / 2.1 |
| GLM pages (WAR) | 13.0 | 3.5 | glm write (target) 11.7 / 3.5; glm upload 1.2 / 0.1 |
| the hazard at the start of a draw flush (`draws`) | 45.5 | 41.1 | the deferred draws read or write what a pending compute or transfer command wrote or read |
| image write-after-write, write-after-read | 1.9 | 0.6 | decode 1.5 / 0.6; dirty patch 0.3 / 0 |
| 512-pending overflow | 0 | 0 | |
| readback | 2.0 | 1.4 | `Reduce` 1.6 / 1.3, `ReduceMany` 0.4 / 0.1 |
| begin of a recording, new image clear (functions) | 12.9 | 9.4 | `begin_recording` 7.2 / 5.6, `clear_new_image` 5.7 / 3.8 |
| total | 565.7 | 239.8 | |

Reading:
- **The hazards are real but over-declared (H, from code):** `dirty patch` reads and writes `{image}` and `clear`/`fill` read
  and write `{frame, depth}`, always the whole image, though a patch or a fill covers a rectangle. Two patches into
  different corners of the same target are ordered. A palette row declares the CLUT buffer as a whole, so two rows with different
  entries are ordered too. This is the room for R2 and R3.
- **Not the cause (C):** the 512-pending overflow, uploads and readbacks.
- **The count is scale-independent (C):** the dependencies come from the guest's draw order, not from pixels.
  The study STUDY-FRAME-DOWNLOAD-COST quoted about 660 (George V) and 683 (Hong Kong) at 6x; here Hong Kong is 566 over
  4800-9600 at every scale (the earlier figure was a different window, or a spike; not reconciled: U).

## 5. Render passes, fragment barriers, descriptors (questions 2-4)

### 5.1 Render pass ends, 2x (C)

| Why the pass ended | Hong Kong | Motegi | Batches per pass |
|---|---|---|---|
| `self copy` (reads what a draw wrote): copy of the sampled rectangle | 33.8 | 30.6 | 1.7 / 1.9 |
| `clear` of a frame or depth (reads what a draw wrote) | 6.8 | 6.4 | 6.5 / 3.8 |
| the submission cadence (`Draw`, every 64 batches) | 5.3 | 3.9 | 35.7 / 33.8 |
| `fill` (compute fill) | 2.9 | 3.0 | 35.2 / 25.9 |
| `clut from target` | 1.0 | 1.1 | 4.3 / 5.3 |
| the VBlank flush (`Submit (renderer)`) | 1.0 | 1.0 | 3.0 |
| readback (`Reduce`, `ReduceMany`) | 0.9 | 0.6 | 2.7 / 3.5 |
| dirty patch, glm write, palette row, decode, patch | 0.7 | 0.2 | |
| target switch | 0 | 0 | |
| layout transition of a sampled image | 0 | 0 | |
| barrier | 0 | 0 | a barrier never ends a pass here |
| **total** | **52.3** | **46.7** | **7.8 / 6.6** |

The render pass has no attachments (storage-image writes, one subpass with a self-dependency; the optional colour-attachment path is
off on this device), so the passes in the table cover the deferred draw batches only; compute and transfer commands are
outside them (C). A `self copy` is a `vkCmdCopyImage` of the sampled rectangle plus border into a pooled image, which needs the pass closed.
Over the run, copies average 8,300 px (Hong Kong) and 11,800 px (Motegi) at 2x (`self_copy_mpx` over `self_copies`), small.
Whether the copied rectangles are disjoint from what the batch draws (the oracle's path 2, no copy at all): **U**, no counter yet.

### 5.2 Fragment barriers (C)

| | with interlock (2x) | no interlock (`GT4_HW_INTERLOCK=0`, 2x) |
|---|---|---|
| Hong Kong | 32.7 (another target 29.2, the frame itself 2.0, depth peek 1.5) | **1,103.2** (primitive may share a pixel **1,070.5**, another target 29.7, frame 2.0, depth peek 1.0) |
| Motegi | 27.9 (another target 26.9, depth peek 1.0) | **593.4** (primitive 565.5, another target 26.9, depth peek 1.0) |
| Motegi Performance mode | 27.7 | 569.1 (primitive 541.4) |

Without the interlock the draw calls become 1,409 (Hong Kong) and 821 (Motegi) against 410 and 306 batches: each overlapping
primitive is its own draw after a barrier. The phone's figure (about 750 fragment barriers, 75 full barriers, 20 passes per VBlank
in the Nordschleife in Performance mode) is the same order as Motegi without interlock here (569, 253, 46); the full-barrier and
pass counts are 3x and 2x the phone's, so the scenes are not the same (U: the Nordschleife was not run here; step 2 should run
the trace on the phone).

"Texture is another target" (about 27-29 per VBlank): a batch that samples a target waits for **any** earlier draw of the pass,
whether or not that draw wrote the sampled target (code: `draw.textureFromTarget && drewSinceBarrier`).

### 5.3 Descriptors, binds, submissions (C)

| Per VBlank, 2x | Hong Kong | Motegi |
|---|---|---|
| transient sets allocated: decode / draw / compute | 472 / 233 / 95 | 109 / 162 / 67 |
| `vkUpdateDescriptorSets` calls / descriptor writes | 800 / 4,139 | 337 / 1,781 |
| descriptor-set binds / pipeline binds / vertex-buffer binds | 410 / 187 / 52 | 306 / 151 / 47 |
| viewport / scissor / push-constant records | 61 / 396 / 96 | 55 / 292 / 91 |
| submissions (cadence 5.3 / 3.9, VBlank flush 1.0, waited readbacks 1.0 / 0.7) | 7.2 | 5.6 |
| submissions forced by descriptor-pool exhaustion or a ring | **0** | **0** |

The draw descriptor set is cached per recording by its four images, so every submission rebuilds the sets of the batches after
it (233 draw sets for 410 batches in Hong Kong). Nothing is pushed or updated per draw otherwise: binds are already skipped when
unchanged (IN-458).

## 6. GPU time outside draw passes (coordinator's request, from the owner's S23 logs)

**The finding to explain.** In 480i menus the phone's normal mode at 2x reports `gpu_busy` 13.7-16.8 ms and `gpu_draw_passes` 1.0-2.0 ms.

**Where the desktop stands.** A presenting run (`--window`, `GT4_OUTPUT_SIZE=3088x1440`; the window's client area came out
2858x1333, the same shape, because the work area of the 3440x1440 monitor is smaller), Spec II, the first six presses of the
Hong Kong timeline then idle in the menu, 90 s, unpaced, `GT4_CORE2_GPU_CATEGORIES=1`, the desktop's RX 9060 XT; mean per VBlank
over the last intervals (a 600-VBlank line each, stable to 0.02 ms). **Indicative timings** (the desktop was shared with other
work; one run each):

| Scale | Mode | draw passes | compose + deinterlace | present (`PresentScale`) | uploads/copies | compute | unattributed | `gpu_busy` |
|---|---|---|---|---|---|---|---|---|
| 2x | normal | 0.10 | 0.22 | 0.35 | 0.00 | 0.02 | 0.00 | 0.68 |
| 2x | Performance | 0.09 | 0.20 | 0.34 | 0.00 | 0.02 | 0.00 | 0.65 |
| 10x | normal | 0.73 | 5.06 | 1.18 | 0.00 | 0.22 | 0.00 | 7.19 |
| 10x | Performance | 0.68 | 5.10 | 1.17 | 0.00 | 0.21 | 0.00 | 7.16 |

- **The non-draw share is large on the desktop too (C for the numbers, H for the reading):** draw passes are 15% (2x) and 10%
  (10x) of `gpu_busy`. The absolute cost is small at 2x (0.58 ms) and grows with the output pixel count: from 2x to 10x the
  compose/deinterlace time is 23x for 25x the pixels. So the cost is bandwidth over full-output images, which is the kind of
  work a tile-based GPU does worst (Hyp). The phone's 12-15 ms against this desktop's 0.58 ms is a ratio of 20-26x for the same
  work at 2x; the desktop cannot say how much of it is the same passes (U), but there is no other large category here.
- **Normal and Performance mode are the same in menus (C):** Performance mode changes races only.
- **Unattributed is 0 and the start gap is 0 on the desktop (C):** with timestamps tiling the submission there is no idle inside
  a menu submission; the phone's 12-15 ms is therefore not idle gaps if its timings behave the same (Hyp; Adreno timestamps can
  differ).
- **What is not measured (U):** the blit of the output image to the swapchain (`hw_gs_backend.cpp`, own command buffer) is in no
  counter, and the headless runs (the whole TASK-178 bench) never reach compose, deinterlace or present (`presents=0`), so they
  say nothing about this share.
- **Compose/deinterlace is two things (U):** the category merges `compose`, `Deinterlace` (up to two dispatches over the
  display image plus an image copy, a full barrier after each) and `Fxaa`. Splitting it needs one more category; step 2.

Candidate changes for this section are in R8 below. None is measured.

## 7. Ranked changes (each exact, each with an A/B switch)

"Exact" here: every read-after-write and write-after-read pair on the touched region stays ordered, so the memory and images
are bit-identical (gate: the flare reply hash, the frame and state comparisons of TASK-178 9.2, and CTest). Gains are
**hypotheses**; the counts say how many things there are to remove, not what each costs. Tile-GPU gains need the phone to
confirm.

| # | Change | Narrows | Exactness | Gain, desktop | Gain, tile-based GPU | Risk | Switch |
|---|---|---|---|---|---|---|---|
| R1 | **Draw the self copy inside the open pass.** A tiny fragment (or compute-free) pipeline writes the sampled rectangle plus border into the pooled image as a storage-image write, ordered with a fragment barrier (draw to draw), instead of `vkCmdCopyImage` after closing the pass. Falls back to the copy on the colour-attachment path. | 34 / 31 of 52 / 47 pass ends; the 34 / 31 `self copy/image raw` full barriers and, Hyp, most of the `draws` ones (the batch reads the copy); one flush per copy | A copy of words; the barrier orders the writes of earlier draws to the source and the later read by the batch (RAW) and the next write of the same rectangle (WAR). Bit-identical | Hyp: 34 fewer passes, 34 fewer full barriers, 34 fewer deferred-flush boundaries per VBlank; a few tenths of a ms at 2x | Hyp: the largest: passes 52 to about 18 (47 to 16); the 20 passes of the phone's Nordschleife are probably this chain too (U) | Low-medium: new tiny pipeline and descriptor set; copy and draw must agree on the border and on scaled addressing | `GT4_CORE2_SELF_COPY_DRAW=0` |
| R2 | **Region-granular hazards for target images.** `AccessSet` records rectangles (in 64x64 output-pixel tiles) instead of a whole image; `dirty patch`, `patch (cpu words)`, `fill`, `clear`, `self copy` and the flush hazard test declare their rectangles; two accesses conflict only if they share a tile. A command that cannot name a rectangle stays whole-image. | `image raw/waw/war` 224 / 123 per VBlank (40% / 51% of full barriers), part of `draws` (46 / 41) | Conservative rectangles (scissor boxes and sampled areas padded to tile size; anything unknown is whole-image) never miss a real pair | Hyp: the largest barrier count to remove; fewer drains; a ms or more in Hong Kong spike frames (the study's hypothesis, unmeasured) | Hyp: medium-large (fewer full barriers between small patches) | Medium-high: the rectangle of every reader (texture sampling with bilinear border, mip levels, palette rows) must be right; needs an A/B on a stream that overlaps patches on purpose | `GT4_CORE2_REGION_HAZARDS=0` |
| R3 | **CLUT hazards by entry range.** The GPU CLUT buffer (512 entries) is tracked as ranges: a palette row reads the entries of its CSA and storage mode, a CLUT load or write names the entries it writes. | `clut buffer` + `war`: 175 / 34 per VBlank (31% / 14%) | The entry range of each CLUT access is exact from CSA, CPSM and the load size | Hyp: moderate in Hong Kong, small in Motegi | Hyp: moderate | Low-medium | `GT4_CORE2_CLUT_RANGES=0` |
| R4 | **Scope the remaining full barriers** from `ALL_COMMANDS` to the stages and accesses that matter (compute and transfer as source and destination, fragment shader writes of the draws as source), and to one `VkImageMemoryBarrier2`/buffer range when the hazard is on one image or the GLM/CLUT buffer. Also carry the pending-access sets over a submission boundary so `begin_recording` (7.2 / 5.6 per VBlank) needs no barrier. | all 566 / 240 (scope), the 12.9 / 9.4 function barriers | Same hazards, narrower masks. Cross-submission order still needs a barrier unless the pending sets say nothing is pending | Hyp: small by itself (a drain is a drain), it lets later work overlap | Hyp: small-moderate; ALL_COMMANDS includes vertex and binning work that nothing needs | Low | `GT4_CORE2_BARRIER_SCOPE=0` |
| R5 | **Fragment barriers by image.** For a batch sampling another target, wait only if an earlier draw of the pass wrote that target (track written image serials since the last barrier). With interlock off, the 1,070 / 565 `primitive may share a pixel` barriers stay (they are real overlaps; tightening them needs a better disjointness test or the colour-attachment path, see R10). | 29 / 27 per VBlank now; not the 565-1,070 | A later draw reading a target no earlier draw wrote cannot see a hazard | Hyp: small | Hyp: small-moderate (in-pass barriers are expensive) | Low | `GT4_CORE2_FRAGMENT_BY_IMAGE=0` |
| R6 | **Persistent descriptor sets.** Keep draw sets across submissions (the key already carries the image serials) and give decodes a per-image set or push descriptors; write a set only when its images change. | 233 / 162 draw sets and 472 / 109 decode sets per VBlank; 800 / 337 update calls, 4,139 / 1,781 writes | Pure CPU bookkeeping | Hyp: CPU only, small-medium on the GS thread in Hong Kong | Hyp: driver cost of descriptor allocation and writes can be much higher on mobile | Low | `GT4_CORE2_PERSISTENT_SETS=0` |
| R7 | **GLM hazards below the 8 KiB page** (by block run). | `glm pages` + `war` 104 / 30 per VBlank | Page to block precision, rectangles already exact | Hyp: small | Hyp: small | Medium (page bookkeeping is per page everywhere) | `GT4_CORE2_GLM_BLOCKS=0` |
| R8 | **Compose, deinterlace and present at output size** (section 6): fuse the deinterlace weave, compose and present into fewer full-output passes and skip a pass when its inputs did not change (a static menu repeats the same source). Needs the compose/deinterlace split of step 2 before it is ranked. | up to 5-6 of 7.2 ms at 10x in menus on the desktop | Fusing is exact; skipping an unchanged frame is exact for a stateless pass, not for deinterlace modes with history | Hyp: large in menus at high scale | Hyp: the 12-15 ms of the S23 menus | Medium | `GT4_CORE2_FUSE_POST=0` (done as `GT4_CORE2_DEINT_FIELD_COMPOSE`, step 2b) |
| R9 | **Raise `GT4_CORE2_SUBMIT_EVERY` on tile GPUs** (existing switch): the cadence ends 5.3 / 3.9 passes per VBlank; not exact by construction (submission boundaries never change what is drawn, TASK-178 9.2). A/B only. | 5.3 / 3.9 pass ends | Submission boundaries do not change results | none | Hyp: small | None (existing) | `GT4_CORE2_SUBMIT_EVERY=<n>` |
| R10 | **More batches through colour attachments (IN-481)** on devices without interlock, so ROP order replaces the per-primitive barriers (565-1,070 per VBlank). The attachment path is off here; which batches it takes and why is a separate study. | the no-interlock fragment barriers | ROP order is the GS order per pixel | n/a (interlock is on) | Hyp: the largest for the 593-1,103 fragment barriers | High | existing `GT4_CORE2_ROP` family |

Order of attack for step 2 (cheapest, exact, biggest first): **R1** (one new pipeline, removes two thirds of the pass ends),
then **R3** (small bookkeeping, 31% of Hong Kong's barriers), then **R2** (the biggest barrier count, the most careful), with
**R4** and **R6** as low-risk additions. Step 2 should first add two counters (the disjointness of each self-copy rectangle
against its batch's draw rectangle, and the compose/deinterlace split), run the trace on the phone, and A/B each switch on the
Avell and the S23 (not on this shared desktop).

## 8. Unknowns

- U: whether the S23's counts are Motegi-like (here without interlock: 569 fragment, 253 full, 46 passes; the phone: 750, 75, 20 in the
  Nordschleife). The trace works on any device (`GT4_CORE2_BARRIER_TRACE=1`); the phone run decides R1's share of the 20.
- U: the share of self-copy rectangles disjoint from their draw (the oracle's path 2).
- U: how much of the S23's 12-15 ms the compose/deinterlace/present passes are, and the swapchain blit (outside `gpu_busy`).
- U: the time each barrier costs; the counts here rank the things to remove, not their cost. The TASK-178 profile (section 9.4 of
  STUDY-FRAME-DOWNLOAD-COST) isolates commands with barriers and cannot give it either.
- Hyp: R1's pass reduction brings the desktop's 52 passes near the phone's 20 only if the chain is the same one.

## Step 2a: R1, the self copy drawn inside the open render pass

Date 2026-11-05, same branch and the same **not PCSX2-tainted** status as step 1 (no PCSX2 source, fork,
`private/tooling/pcsx2-*` or reference tree opened; the only outside input is the clean-room prose
`ORACLE-GSDX-TONE-READBACK-FLUSH.md`, D3). This section records the implementation of row R1, the diagnostic it added and
its gates. Timing is not measured: the desktop is shared and the gate is counts plus identity.

### What changed (C, code)

- **`core2_self_copy.vert` / `.frag`** (new): a full-screen triangle and a fragment stage that stores
  `imageLoad(source, texel)` into the destination at the same texel. Both images are the core 2 target format (r32ui GS
  words), so a word keeps its pixel, exactly what `vkCmdCopyImage` moved. Built by the pinned glslang like every core 2
  shader (no new tool).
- **`Gpu::Draw`** (`core2_gpu.cpp`): with `GT4_CORE2_SELF_COPY_DRAW=1` the spec 8.8 copy is recorded as a draw inside the
  render pass the earlier batches opened (`Impl::record_self_copy`), after a fragment barrier (earlier stores to the
  source), and the batch that samples the copy is recorded right after it in the same pass, ordered by its own
  texture-from-target fragment barrier. The copy vitals are identical to the transfer's: the same clipped native
  rectangle times the scale, at the same offsets, into the same pooled image.
- **`Impl::flush_draws`** gained `keepPassOpen`: the flush that used to end the pass at the copy now leaves it open (and
  skips the pass end), and later batches continue in it. A pass left open is closed by the next `outside_pass` /
  `end_render_pass` of whatever command follows (all of them still call it), and if a full barrier is needed while a
  pass is open, `flush_draws` ends the pass before the barrier (a barrier inside a pass is illegal).
- **The transfer stays** wherever the draw cannot apply, and each reason is a plain condition in `Gpu::Draw`: the
  colour-attachment path (IN-481: a sampled image may be an attachment of the open pass), `GT4_CORE2_LAZY_BARRIERS=0`
  (which records a full barrier per command), a rectangle outside the pass's framebuffer scissor, and the default (the
  switch unset or 0).
- **WAR across passes**: a pooled copy image is reused from pass to pass. `copyImagesUnordered` is set when a copy is
  drawn and cleared by any full barrier; a copy that has to begin a new pass (a compute or transfer command ended the
  previous one) records a full barrier first, so the next write of the pooled image is ordered against the previous
  pass's readers. Inside one pass the two fragment barriers cover RAW and WAR.
- **Diagnostics** (`GT4_CORE2_BARRIER_TRACE=1`): `[core2] sync ... self_copy <key>: count=` rows for
  `drawn in the pass, overlapping the batch`, `vkCmdCopyImage, overlapping the batch` and, from the renderer
  (`Gpu::NoteSelfSampleNoCopy`), `no copy: sampled rectangle disjoint from the drawn one` and
  `no copy: frame untouched (IN-495)`.
- **CTest `hw_core2_self_copy_draw`** (new): a synthetic stream with two copied self-sampling sprites (a half-size
  reduction that meets its draw, and a 1:1 pass) and one whose sampled rectangle is disjoint from what it draws, at 1x,
  2x, 3x and 12x, switch off against on: images and GS memory bit-identical, the 1x memory equal to the CPU reference,
  two copies drawn in the pass with the switch on and none with it off.

### The disjoint question (C)

**The oracle's D3 path 2 is already there, upstream of the copy.** `ProcessBatch` takes the copy only when the sampled
footprint meets the batch's drawn rectangle (`core2_renderer.cpp`, the spec 8.8 block, `Intersect(textureFootprint_,
t.rect)`); otherwise the batch samples the target itself. So the copy path never sees a disjoint rectangle by
construction, and the counters say exactly that: **none of the copies is disjoint** (0 in both scenes, either path), and
**the disjoint case is already handled without a copy**: 2.00 per VBlank in Hong Kong (5.4% of the 37.35 self-sampled
batches), 0 in Motegi. IN-495's "frame untouched" case is another 1.00 per VBlank (2.7% / 3.1%). **Implementing path 2
at the copy site would save nothing** (C for these two scenes, H for others); what remains of the self-copy problem is
the 92% / 97% of self-sampled batches that really do overlap, which is what R1 addresses. The new fixture forces a
disjoint sprite and gets its row (1 per replay), so the counter can see it when it happens.

### Counts (C)

Window (4800, 9600] of the press scenes, per VBlank, `private/task182-tools/sync_table.py`; one build
(`build/t178-spec2`), `GT4_CORE2_BARRIER_TRACE=1`, the switch off against on, Spec II headless, 2x.

| Per VBlank | Hong Kong off | Hong Kong on | Motegi off | Motegi on |
|---|---|---|---|---|
| render passes | 52.31 | **23.81** | 46.71 | **20.30** |
| pass ends: `draws recorded for self copy` | 33.76 | 0 | 30.56 | 0 |
| full barriers | 565.73 | 505.51 | 239.78 | 184.07 |
| fragment barriers | 32.71 | 97.51 | 27.90 | 87.96 |
| draw flushes | 52.31 | 23.09 | 46.71 | 19.57 |
| submissions | 7.22 | 7.22 | 5.58 | 5.58 |
| self-sampled batches | 37.35 | 37.35 | 32.02 | 32.02 |
| ... copied (of which drawn in the pass) | 34.35 (0) | 34.35 (**100%**) | 31.02 (0) | 31.02 (**100%**) |
| ... no copy: sampled rectangle disjoint | 2.00 | 2.00 | 0 | 0 |
| ... no copy: frame untouched (IN-495) | 1.00 | 1.00 | 1.00 | 1.00 |

Pass ends by reason with the switch on (per VBlank, Hong Kong / Motegi): the clear (`draws recorded for clear: reads
what a draw writes`) 6.81 / 6.37, the submission cadence (`draws recorded for Draw: submission`) 4.71 / 3.43, the fill
2.87 / 2.97, the VBlank flush 0.96 / 0.99, the readback 0.72 / 0.60, and the ends the open pass now pays when a compute
or transfer command closes it: `Draw` 3.20 / 2.13 (the clear and fill paths), `clear_new_image` 0.95 / 0.89,
`WriteClut` 0.86 / 0.77, `a full barrier before the draws` 0.95 / 0.78, then `UploadGlm`, `ClutFromTarget`,
`DecodeTexture`, `MaterializeTarget` and `Patch` (0.02-0.33 each).

Reading: **the pass count is 54% lower in Hong Kong and 57% lower in Motegi**; the copies all take the drawn path
(100%, no fallback in these scenes). The full barriers fall by 60 and 56 per VBlank — the copy's own `image raw` barrier
and the flush hazard that preceded it — and the fragment barriers rise by 65 and 60, the two per copy that replace them.
It is not the full 34/31: a copy still needs a pass of its own when the submission cadence, a readback or a compute
command falls between it and its batch, and the pass the copy used to share with the draws before it is now closed by
whichever command follows the chain instead of by the copy.

### Exactness gates (C)

Flare reply hashes, `build/t178-spec2` (Spec II), the study's scenes and commands, `GT4_CORE2_SELF_COPY_DRAW=1`:

| Run | Replies | Hash | Section 3 reference |
|---|---|---|---|
| Hong Kong 2x | 6835 | `2f4c1cc9f174d00e` | same |
| Hong Kong 10x | 6835 | `61109e8f3143264a` | same |
| Motegi 2x | 6588 | `7bf932fbb8799965` | same |
| Motegi 2x, `GT4_HW_INTERLOCK=0` | 6588 | `7bf932fbb8799965` | same |

The switch-off runs of the same build give the same hashes (Hong Kong 2x and Motegi 2x were run both ways).

CTest: the full suite on the same build (`build/t178-spec2`, run serially, `ctest --output-on-failure`): **320 tests,
319 passed, 1 skipped** (`hw_replace_q07_pcsx2_dumps`: no PCSX2 dumps on this machine), **0 failed**, 471.8 s. The new
`hw_core2_self_copy_draw` passes at 1x, 2x, 3x and 12x, as do `hw_core2_self_texture`, `hw_core2_sync_trace`,
`hw_core2_rop`, `hw_core2_whole_clears` and `hw_core2_compute_fills`. No test failed under load, so no test needed a
rerun alone.

### Doubts (U, Hyp)

- **U: the GPU cost of a copy drawn instead of transferred.** Two fragment barriers and a small draw replace a transfer
  and a full barrier; the count is right, the cost is not measured (no timing A/B on this shared desktop).
- **Hyp: the remaining passes.** The pass ends that remain are the clear, the submission cadence, the fill, the VBlank
  flush and the readback, plus the ends the open pass now pays when a compute or transfer command closes it (`Draw`,
  `clear_new_image`, `WriteClut`, `UploadGlm`, ...). Making a command wait for the pass instead of ending it would need
  the pass to survive a compute dispatch, which it cannot; recording that work before the chain starts would.
- **U: other tracks and the phone.** The disjoint share and the copy rate are measured on Hong Kong and Motegi only; the
  Nordschleife (rally, rain, night) and the S23's chain are not. `GT4_CORE2_BARRIER_TRACE=1` answers both on the device.
- **U: the colour-attachment path** (integrated GPUs) always falls back to the transfer, so R1's gain there is zero.
- **Hyp: the first copy after `Init`** compiles a pipeline inside the first frame unless the driver had it cached; the
  pipeline is created at `Init`, not at first use.

## Step 2b: R8, compose, deinterlace and present split, explained, and the field-compose order

Date 2026-11-05. Same branch, same **not PCSX2-tainted** status as step 1 (no PCSX2 source, fork,
`private/tooling/pcsx2-*` or reference tree opened; only `docs/hw-renderer-spec/` and our own code). This section splits
the step 1 `compose_deinterlace` category, explains the cost per pass, and implements one exact reduction: with
`GT4_CORE2_DEINT_FIELD_COMPOSE=1` the display merge of a deinterlaced frame writes that field's deinterlace history image
directly and the deinterlace reads the last output from the display image, so both full-display copies and two of the four
full barriers a deinterlaced VBlank recorded are gone. Switch off by default; the old order is the A/B reference.

### 1. The split (C)

`GT4_CORE2_GPU_CATEGORIES=1` now prints one category per pass: `compose`, `deinterlace`, `fxaa`, `present`, plus the sum
`compose_deinterlace` so the step 1 numbers stay comparable (the sum no longer includes FXAA; FXAA is off in every run
here). Two counters come with it: `post_copies=<n>/vblank`, the full-display `vkCmdCopyImage` calls the deinterlace
recorded, and `field_composes=<n>`, the display merges written into a field history image instead of the display image.

Method: `build/t178-spec2` (Spec II, Release, SSE4.1; final executable SHA-256 `86a4f5a230d4941d`), the TASK-178 press
scenes, **presenting** (`--window`, `GT4_OUTPUT_SIZE=3088x1440`, the client area 2858x1333), paced at 60 Hz,
`GT4_CORE2_GPU_CATEGORIES=1`; every row of this table is one run of that build. The menu rows are
the title screen with no presses (1500 VBlanks, `build/t182-r8-window.sh menu`; the table reads the 600-1200 interval).
The race rows are the last 600 VBlanks of a 5400-VBlank Hong Kong reverse and Motegi Road Course B-Spec run (the game
reaches the race at about VBlank 3000; `build/t182-r8-window.sh hk|mot`). Indicative timings: the desktop was shared with
an Android build and other work. ms per VBlank:

| Scene | Scale | `GT4_CORE2_DEINT_FIELD_COMPOSE` | draw passes | compose | deinterlace | present | `gpu_busy` |
|---|---|---|---|---|---|---|---|
| 480i menu | 2x | 0 (today) | 0.06 | 0.03 | 0.16 | 0.34 | 0.65 |
| 480i menu | 2x | 1 | 0.06 | 0.04 | **0.13** | 0.35 | 0.62 |
| 480i menu | 10x | 0 (today) | 1.03 | 0.76 | 3.74 | 1.60 | 7.43 |
| 480i menu | 10x | 1 | 1.03 | 0.76 | **3.04** | 1.67 | **6.79** |
| Hong Kong 480p race | 2x | 0 (today) | 1.29 | 0.04 | 0.00 | 0.34 | 3.32 |
| Hong Kong 480p race | 2x | 1 | 1.30 | 0.04 | 0.00 | 0.34 | 3.34 |
| Motegi 480p race | 2x | 0 (today) | 1.14 | 0.04 | 0.00 | 0.34 | 2.18 |
| Motegi 480p race | 2x | 1 | 1.13 | 0.04 | 0.00 | 0.34 | 2.18 |
| Hong Kong 480p race | 10x | 0 (today) | 10.08 | 0.46 | 0.00 | 0.96 | 17.01 |
| Hong Kong 480p race | 10x | 1 | 10.21 | 0.47 | 0.00 | 0.97 | 17.21 |
| Motegi 480p race | 10x | 0 (today) | 9.07 | 0.48 | 0.00 | 1.00 | 13.53 |
| Motegi 480p race | 10x | 1 | 8.90 | 0.48 | 0.00 | 0.99 | 13.30 |

Reading:
- **Compose and deinterlace already run at the internal scale, never at the screen (C, code).** `ComposeGpu` composes
  `merge_output_size(layout, deinterlaced, 1) * scale_` into the display image: 640x448 x s in a 480i menu (1280x896 at
  2x, 6400x4480 = 28.7 Mpx at 10x), and the deinterlace works on that same image. Only `PresentScale` touches the window
  rectangle. The owner's rule ("every post step at the render resolution, only the final upscale at screen size") is
  therefore already the order in core 2: the S23's 43.3 ms is not a screen-resolution mistake, it is how many
  internal-resolution passes run per VBlank.
- **In a 480i menu the deinterlace is the largest post cost (C):** 0.16 of 0.53 post ms at 2x and 3.74 of 6.10 at 10x
  (compose 0.03 / 0.76, present 0.34 / 1.60).
- **A 480p race has no deinterlace at all (C):** the race display is progressive, so the post chain is one compose plus
  one present, 0.38 of 3.32 `gpu_busy` at 2x and 1.43 of 17.01 at 10x. The race is draw-bound, and the phone's menu
  problem does not exist there; the 480p rows also show that the R8 switch changes nothing in a race (0 copies either
  way) — it can only pay where the game is interlaced.
- **Present is the largest single category at 2x (C):** 0.34 of 0.65 `gpu_busy`. At 2x the internal image is smaller
  than the window (1280x896 -> 2858x1333), so it is one FSR1 EASU pass (12 taps per output pixel); at 10x the internal
  image is larger (6400x4480 -> 2858x1333) and it becomes two Lanczos-2 passes (ratios 2.24 and 3.36, about 10 and 14
  taps per output pixel) for 1.6 ms.
- **The race rows are draw-bound and switch-insensitive (C):** draw passes are 39% (Hong Kong 2x), 52% (Motegi 2x), 59%
  (Hong Kong 10x) and 67% (Motegi 10x) of `gpu_busy`, and each off/on pair agrees within 1.3% (the two 10x race rows
  within 0.15 ms of draw passes, which is the run-to-run noise of a shared desktop). The switch only touches deinterlaced
  VBlanks, and a 480p race has none.

### 2. Why it scales with the internal scale (the 43 ms at 6x)

Every image here is `VK_FORMAT_R32_UINT` (one GS word per texel, the display image holding packed RGBA8) with
`STORAGE | TRANSFER_SRC | TRANSFER_DST`, always in `VK_IMAGE_LAYOUT_GENERAL`; no post pass uses a colour or depth
attachment (the IN-481 attachment path is off on this device), and the compute passes write storage images (C, code).
Per deinterlaced VBlank, before this step (C, code):

| Pass | Size | Reads | Writes | Per VBlank |
|---|---|---|---|---|
| `core2_compose.comp` (display merge) | internal: native display x s (6400x4480 = 28.7 Mpx at 10x) | 1 texel of 4 B per enabled circuit (<= 2) from an r32ui storage image, plus `imageSize` | 1 4-byte texel per output pixel (storage image) | 1 dispatch (8x8 workgroups) |
| `hw_deinterlace_scaled.comp` (mode 6, the default) | same | up to 19 4-byte `imageLoad`s per missing-row pixel: this field 1, woven 1, up/down 2, previous same-parity up/down 2, older opposite-parity 1, and 4 neighbours x 3; a field-row pixel reads 1 | 1 4-byte texel per output pixel | 1 dispatch |
| `vkCmdCopyImage` display -> `history[current]` | same | a full-display copy (read + write) | image copy | 1 |
| `vkCmdCopyImage` display -> `deinterlaceOutput` (modes 5 and 6) | same | a full-display copy | image copy | 1 |
| `hw_postfx_fxaa.comp` (off by default) | same | 9 luma taps + 2 x `kSearchSteps` + 2 | 1 texel | 0 here |
| `core2_present_scale.comp` | the window rect (2858x1333) | 12 taps (EASU, growing) or 2 + 4 x widen per axis per pass (Lanczos-2, shrinking) + 5 (RCAS) | 1 texel per pass | 1-3 dispatches |
| full barriers | - | - | - | 4, all in the deinterlace: before the history copy, after it, after the dispatch, after the last-output copy |

Answer, labelled:
- **C:** every pass except present has (native display x s) pixels squared. At 6x a 640x448 menu is 3840x2688 = 10.3 Mpx,
  **2.3x the phone's 3088x1440 screen**; at 10x on the desktop it is 28.7 Mpx, 6.5x the window. Five full-image passes
  run per deinterlaced VBlank (merge write, history copy, deinterlace reads, deinterlace write, last-output copy).
- **C:** the deinterlace reads ~19 taps on half the rows, about 10 4-byte loads per output pixel, and writes 4 B: at 10x
  that is ~1.1 GB of *requested* reads and 115 MB of writes for one dispatch. Neighbouring invocations reuse most of
  those texels, so the DRAM traffic is far lower (Hyp), and the measured 3.8 ms is more than bandwidth alone explains.
- **Hyp:** on a tile-based GPU each of those five passes is a full flush of a ~41 MB image (at 6x) to memory and back
  with a barrier between passes, so the cost is the number of passes and their round trips, not arithmetic: 5 x 41 MB in
  43.3 ms is only 4.7 GB/s of DRAM traffic, far under the Adreno 740's bandwidth. The desktop shows the same shape at a
  much smaller absolute cost: the two copies plus their two drains are 0.72 ms at 10x, 0.36 ms per copy of 115 MB
  (bottom-of-pipe marks charge a preceding command's tail to the next category, so a copy's span includes the drain).
- **C:** the internal scale enters twice, as pixels (quadratic) and as passes (linear). That is why the chain is
  invisible at 2x (0.53 of 0.65 `gpu_busy`) and dominant at 10x (6.10 of 7.43), and why the phone's 6x menus are the
  worst case in the project. Present scales differently — its output is the window, but its tap count grows with the
  shrink ratio, which is why it costs 1.6 ms at 10x and 0.34 ms at 2x.
- **U:** how much of the phone's 43.3 ms is per-pass overhead (barriers, tile flushes) against per-pixel work. The
  clean separation experiment is the same 6x menu with `GT4_HW_DEINTERLACE=off` (one full-image merge and no deinterlace
  or copies) on the phone; not run here.

### 3. The switch: `GT4_CORE2_DEINT_FIELD_COMPOSE=1` (exact, default off)

What changes (C, code):
- `Renderer::PlanDeinterlace` computes the field, the history indices and the kernel mode **before** the merge (none of it
  needs the composed image), and `Gpu::PrepareDeinterlace` creates the history and woven images at the display size and
  remembers this field's history image. `Gpu::Compose(..., intoField=true)` then writes the merge into it, so the
  "composed display -> history" copy disappears; the separate last-output image is never created on this path.
- `Gpu::Deinterlace(step, fieldComposed=true)` records no copy at all and binds the **display image** as the kernel's
  `previous_output`: at that moment the display still holds the previous deinterlaced output, because this VBlank's merge
  no longer wrote it. The kernel reads it at the texel it writes (new push word `previous_in_output`; `display_out` is
  read-write, the same in-place-per-texel pattern `core2_tone.comp` already uses), so the last-output image and its copy
  disappear too.
- The one VBlank the new order cannot serve is the kernel's fallback to the last output of an earlier field (no history
  entry of the opposite parity while `haveOutput` is set): it takes the old order. `Gpu::SnapshotLastOutput` keeps the
  fallback image current across a 480i -> 480p transition (one copy, once per transition). With alternating fields and a
  history cleared on a size change the fallback does not arise (Hyp: 0 in both scenes and in every fixture).
- What it removes per deinterlaced VBlank: **2 full-display copies, 2 full barriers**, and the separate last-output image
  at the display size — `image_mb` 1115.3 -> 1005.9 at 10x in the menus (-109 MB; about -41 MB at 6x on the phone).
- The old order is untouched behind the switch (unset or 0): same passes, same barriers, same copies.

### 4. Identity evidence (C)

- **Fixture `hw_core2_post_field_compose` (new, CTest).** The stream of `hw_core2_interlaced_present` (a 64x32 field
  buffer, per-field lines, a right half that changes every VBlank), 8 fields, at 1x, 2x and 3x, for the adaptive
  (rule 6), weave, bob and a progressive stream: **every composed frame is bit-identical** with the switch off and on
  (32,768 / 131,072 / 294,912 pixels per case, 0 differing), the deinterlace count is 8/8, and the counters show the path
  each run took: field composes 0 -> 8, display copies 16 -> 0 (adaptive, two per field) and 8 -> 0 (weave, bob, one per
  field). The switch-off order is the one `hw_core2_interlaced_present` compares with the CPU presentation at 1x
  (weave and adaptive rule 5, all eight VBlanks); with the two fixtures together the switch-on output equals the old
  order equals the CPU reference where that reference exists.
- **Composed frames from the existing dump tool** (`GT4_HW_DUMP_SOURCE=upscaled`, P6 PPM), off against on:

| Run | Scale | VBlanks dumped | Composed image (SHA-256, first 16) | Switch off = on? |
|---|---|---|---|---|
| 480i menu (title) | 2x | 1200 / 1500 | `2ec8aaf864975a1e` / `1eb80181cf16157a` (1280x896) | yes, 2 of 2 |
| 480i menu (title) | 10x | 1200 / 1500 | `f921e8ca555c1a9e` / `44a6082802dd186a` (6400x4480) | yes, 2 of 2 |
| Hong Kong race | 1x | 6000 / 7500 / 9000 | `b4806526f617e277` / `159ab52a605ba3b9` / `1fd5868e80593448` (640x448) | yes, 3 of 3 |
| Hong Kong race | 2x | 6000 / 7500 / 9000 | `cd48f790cd3ea660` / `2c52b62d42836f72` / `e6fa4eae3ee06f57` (1280x896) | yes, 3 of 3 |
| Motegi race | 2x | 6000 / 7500 / 9000 | `ef8bc58eb50ac93f` / `7259e3633353ca31` / `a08349df75c8cf20` (1280x896) | yes, 3 of 3 |
| Hong Kong race | 10x | 6000 / 7500 / 9000 | `172f0c6378a79f67` / `dc2e6bb99b24c332` / `3dd53b5fc493cd51` (6400x4480) | yes, 3 of 3 |

- **Presented image** (the same menu runs with `--window`, so the dump goes through `ComposeFrameGpu`, `PresentScale` and
  `ReadGpuFrame`, that is the picture the swapchain receives): 2x `0d6366e465cb0b7d` / `41cdf22983493433` and 10x
  `ac2ec9d4f5c9569c` / `a03b7fba92c77472`, 2369x1333 each, **identical off against on**, 2 of 2 at both scales.
  Side-by-side crops for the owner: `build/evidence182/r8-crops/` (menu 2x and 10x, Hong Kong 2x and 10x, composed and
  presented; the old order above, the new below, 1:1 and 4x zoom).
- **Flare reply hashes** of section 3, with the switch on:

| Run | Replies | Hash | Section 3 reference |
|---|---|---|---|
| Hong Kong 2x | 6835 | `2f4c1cc9f174d00e` | same |
| Hong Kong 10x | 6835 | `61109e8f3143264a` | same |
| Motegi 2x | 6588 | `7bf932fbb8799965` | same |
| Hong Kong 1x (no section 3 row) | 6835 | `3f22899e9703be24` | switch off and on equal |

### 5. Times, off against on

From the table in section 1: at 10x in a 480i menu the deinterlace drops 3.74 -> 3.04 ms (-19%) and `gpu_busy` 7.43 ->
6.79 (-9%); at 2x 0.16 -> 0.13 ms (0.65 -> 0.62). Compose and present do not change (0.76/1.60 at 10x, 0.03/0.34 at 2x). In the 480p
races nothing changes, as expected (no deinterlace). On the phone the same change removes two of the five full-image
passes the 6x menu runs per VBlank, so it should pay proportionally more than on the desktop (Hyp; the phone was not run
for this step).

### 6. What is left, and why it is not in this step

- **The deinterlace dispatch itself is now the biggest post item in a 480i menu (C: 3.04 of 6.79 `gpu_busy` at 10x).**
  Its motion decision is per *native* row and column (it samples at +-`row_step` and +-k x `scale`), so every one of the
  s x s output pixels inside a native pixel takes the same decision and repeats the same 19 loads. Computing the decision
  once per native pixel (1/scale^2 of the work: 100x less at 10x) and resolving it per output pixel with 1-2 loads is the
  obvious next exact reduction; it needs a decision image and a proof per mode, so it is a step of its own.
  *Step 2c tested this premise and did not ship it: the default kernel decides per output row (IN-386), and a per-native
  decision changes 0.49% of the missing-row pixels on a real menu frame. What shipped instead is the exact part of the
  same idea (`GT4_CORE2_DEINT_SKIP_DECISION=1`), -39% of the deinterlace at 10x. See "Step 2c" below for the measurements
  and for the next exact reduction, which is a "moved" flags pass, not a per-native decision.*
- **Skipping an unchanged frame** (a static menu) was not implemented: every VBlank in the measured menus composes a
  different image (the title animates; the dump pairs at VBlank 1200 and 1500 differ), and the deinterlace carries field
  history that advances every VBlank, so a skip would have to be proven against the history, not just the sources. The
  exact form is a per-image write stamp plus an identical push block; nothing to gain until a scene repeats a frame.
- **Present** (1.6 ms at 10x in the menu): no exact reduction is left (its output is the picture). The cheaper bilinear
  path already exists as Performance mode on integrated GPUs, not here.
- **A `GT4_CORE2_POST_AT_SCREEN` order** (compose/deinterlace at the window size) is not implemented on purpose: it
  changes the picture (the owner accepts a scaling-only difference, but this is a resolution change, not a scaling one)
  and it is what today's code already avoids. If it is ever wanted, it belongs to Performance mode and must be measured
  against the faithful order.


## Step 2c: the deinterlace decision, "once per native pixel", and what the adaptive pass really costs

Date 2026-11-05. Same branch, same **not PCSX2-tainted** status as steps 1, 2a and 2b (no PCSX2 source, fork,
`private/tooling/pcsx2-*` or reference tree opened). Section 6 of step 2b proposed the next exact reduction: "the motion
decision is per native row and column, so every one of the s x s output pixels inside a native pixel takes the same
decision and repeats the same 19 loads; computing the decision once per native pixel (1/scale^2 of the work) and
resolving it per output pixel with 1-2 loads is the obvious next exact reduction". This step tested that premise before
implementing it. The premise does not hold, in two independent ways (sections 1 and 2), so the planned
`GT4_CORE2_DEINT_NATIVE_DECISION` path was not built: it cannot be bit-identical on real content. What did ship is a
smaller exact change in the same kernel, `GT4_CORE2_DEINT_SKIP_DECISION=1` (sections 3 and 4): the adaptive modes skip
the motion tests where the two words they choose between are already equal. It is the part of the decision that is
provably dead, and it is 39-42% of the deinterlace category at 10x.

### 1. There is no per-native-pixel decision in the default kernel (C, code)

`PlanDeinterlace` sets `step.rowStep = nativeRows ? scale_ : 1u`, and `nativeRows` is only true with
`GT4_HW_DEINTERLACE_ROWS=native` (IN-386). The default is **one output row per algorithm row**: `row = gid.y`,
`up`/`down` are the output rows `y-1` and `y+1`, and the field parity alternates every output row. At 10x the three rows
of one decision are therefore *three sub-rows of the same native pixel*, and inside one native block the decision is
taken once per output row. A "decision per native pixel" is the IN-381 interpretation (one native row per algorithm
row), which IN-386 replaced because it weaved a full native-row ghost at scale - the owner's doubled menu lines. So the
proposed change is not a refactor of today's kernel; it is a different deinterlace.

### 2. Even with native rows, the inputs of the decision differ inside a native block (C, measured)

`core2_compose.comp` maps an output pixel to the source pixel one for one when the placement is the read size at the
same scale (`sx = (p.x - x0)`, `at = read * source_scale + sx`), so the composed image is a 1:1 copy of a render made at
the internal scale, not a nearest-neighbour blow-up of a native image. Measured on the real 480i menu (no presses, 1500
VBlanks, `GT4_HW_DEINTERLACE=off`, `GT4_HW_DUMP_SOURCE=upscaled`, `GT4_HW_DUMP_EVERY=1`, `GT4_HW_DUMP_FROM=1197`,
`GT4_HW_SCALE=10`): of the 640x448 native blocks of the 6400x4480 frame at VBlank 1200, **46.7% are one colour in all
100 of their output pixels**, so 53.3% carry sub-native detail.

The decision itself was then replayed on the real merge images. With `GT4_HW_DEINTERLACE=off` each dump of VBlank t is
exactly the image the kernel keeps in `history[]`, so the four consecutive dumps 1197..1200 are `older_other` (1197),
`previous_same` (1198), `previous_other` (1199) and `current` (1200) for VBlank 1200; the model recomputes mode 6 with
`row_step = 1`, threshold 80 permille, the k loop at +-1 and +-2 x `scale`, and the same integer `channel_excess`
(max channel difference * 1000 > 80 * 255). Two rules were compared:

- today's, per output pixel: 14,329,600 missing-row pixels;
- "one decision per native pixel": the block's first missing output row (row `10 * ny + 1`) at sub-column 0, applied to
  every missing row and column of that block.

Result (C, model on real dumps):

| Quantity | Value |
|---|---|
| Native blocks whose decision is not constant inside the block | 4,183 of 286,720 (1.46%) |
| Missing-row output pixels whose saved word changes | 69,608 of 14,329,600 (0.49%) |
| Pixels of the whole frame that change | 0.24% |
| Missing-row pixels where `bob` already equals the woven row (the decision cannot matter) | 46.5% |
| Missing-row pixels the base comparisons already mark as moving | 0.64% |

So the per-native-pixel decision changes the picture in about one pixel of four hundred, which is enough to fail a
bit-identity gate, and enough to make it a different deinterlace. (The replay is
`build/t182-b2c-native-decision-model.py`, a numpy script that reads the four dumps and prints the three counts; it is a
replay of the kernel's arithmetic on the real merge images, not the kernel itself: it cannot see the GPU's
`previous_output` fallback, which does not arise in this stream because alternating fields always have an
opposite-parity history entry. Label: High confidence for the decision counts, C for the pixel counts.)

### 3. The exact change that did ship: `GT4_CORE2_DEINT_SKIP_DECISION=1`

Modes 5 and 6 end in one of two words: `bob` (the average of the rows above and below, rounded up per channel) or the
woven row (`previous_other` at this texel, or the fallback). `interpolate` - mode 6, or no woven history, or rule 5's
`up`/`down` test - decides whether a moving pixel takes `bob` at all. Both are known before any motion test runs, from
words the invocation has already loaded (`up`, `down`, `woven_word`). Therefore:

- if `interpolate` is false (mode 5 with an older opposite-parity field to weave), the saved word is the woven row
  whatever the tests say;
- otherwise, when `bob` equals the woven row, both branches save the same word.

In both cases the kernel now stores that word and returns before the three base comparisons and the k loop. The switch
is off by default; with it unset the kernel runs exactly the code of step 2b. Bit-identity is by construction (the same
4-byte word lands in the same texel), and it was verified (section 4).

On the measured menu frame this covers 46.5% of the missing-row pixels: those invocations no longer read `previous_same`
twice, `older_other` once, or the 12-24 words of the k loop.

*Found while adding the switch's push word (C, code):* the kernel's push block is 16 words, but the core 2 pipeline
layout declared 15 and the core 1 writeback path declared and pushed 14, so `previous_in_output` (the word TASK-182 R8
added) was undefined for core 1 and a new word would have reached neither. Both layouts now declare 16 words and both
callers push all 16 (core 1 pushes 0 for the two TASK-182 words).

### 4. Identity evidence (C)

- **Fixture `hw_core2_post_field_compose`, extended.** The same 480i stream (a 64x32 field buffer, per-field lines, a
  right half that changes every VBlank, and now rows 4..11 in one flat colour so the skip path actually runs), 8 fields,
  at 1x, 2x, 3x **and 10x**, for adaptive (rule 6), weave, bob and a progressive stream, in **four** switch
  combinations: today's (off/off), field compose only, skip only, and both. Every composed frame is bit-identical in the
  four: 32,768 / 131,072 / 294,912 / 3,276,800 pixels per case, 0 differing; the counters still show 8/8 field composes
  and 16 -> 0 (adaptive) or 8 -> 0 (weave, bob) display copies.
- **Composed frames from the game** (`GT4_HW_DUMP_SOURCE=upscaled`, P6 PPM, VBlanks 1200 and 1500), all four switch
  combinations, equal to the step 2b hashes:

| Scene | Scale | Field compose | Skip | Composed image (SHA-256, first 16) |
|---|---|---|---|---|
| 480i menu | 1x | 0 | 0 / 1 | `e01e4cd6f27a33c3` / `304912a9f41c1d52` (640x448), 2 of 2 equal |
| 480i menu | 2x | 0 / 1 | 0 / 1 | `2ec8aaf864975a1e` / `1eb80181cf16157a` (1280x896), 8 of 8 equal |
| 480i menu | 3x | 0 | 0 / 1 | `b63232d9afcdd383` / `df5983514225aee6` (1920x1344), 2 of 2 equal |
| 480i menu | 10x | 0 / 1 | 0 / 1 | `f921e8ca555c1a9e` / `44a6082802dd186a` (6400x4480), 8 of 8 equal |

- **Race scenes** (480p, no deinterlace: the switch cannot act there) with the skip on: the nine composed dumps of
  Hong Kong 2x, Motegi 2x and Hong Kong 10x (VBlanks 6000, 7500, 9000) are **identical** to the step 2b runs, and the
  flare reply hashes equal section 3:

| Run | Replies | Hash | Section 3 reference |
|---|---|---|---|
| Hong Kong 2x | 6835 | `2f4c1cc9f174d00e` | same |
| Hong Kong 10x | 6835 | `61109e8f3143264a` | same |
| Motegi 2x | 6588 | `7bf932fbb8799965` | same |

- **Full CTest, serial:** 321 tests, **100% passed, 0 failed**, 1 skipped (`hw_replace_q07_pcsx2_dumps`, the PCSX2
  dump fixture, absent in a clean room), 643.9 s (`ctest --test-dir build/t178-spec2 --output-on-failure`).

### 5. Times, off against on

Same method as step 2b: `build/t178-spec2` (Spec II, Release), presenting (`--window`, `GT4_CORE2_GPU_CATEGORIES=1`,
client area 2858x1333, paced at 60 Hz), the 480i title screen with no presses, 1500 VBlanks, the 600-1200 interval.
Indicative only: the desktop was shared with an Android emulator image for the whole batch, and the absolute values are
higher than step 2b's table (which read 3.74 / 3.04 at 10x); the off/on pairs were run back to back and the direction
held in both orders (see the note below the table). ms per VBlank:

| Scene | Scale | `GT4_CORE2_DEINT_FIELD_COMPOSE` | `..._SKIP_DECISION` | deinterlace | `gpu_busy` |
|---|---|---|---|---|---|
| 480i menu | 2x | 0 | 0 | 0.23 / 0.20 | 0.71 / 0.62 |
| 480i menu | 2x | 0 | 1 | **0.16 / 0.15** | 0.62 / 0.60 |
| 480i menu | 2x | 1 | 0 | 0.16 / 0.18 | 0.55 / 0.63 |
| 480i menu | 2x | 1 | 1 | **0.13 / 0.12** | 0.57 / 0.57 |
| 480i menu | 10x | 0 | 0 | 4.85 / 5.15 / 5.38 / 4.79 / 5.28 | 7.85 / 8.28 / 8.69 / 7.68 / 8.54 |
| 480i menu | 10x | 0 | 1 | **2.59 / 3.45 / 3.46 / 2.98 / 3.00** | 5.07 / 6.80 / 6.85 / 5.97 / 6.01 |
| 480i menu | 10x | 1 | 0 | 4.61 / 4.61 | 7.89 / 7.91 |
| 480i menu | 10x | 1 | 1 | **2.69 / 2.69** | 6.04 / 6.04 |

Means at 10x: 5.09 -> 3.10 ms of deinterlace (-39%) and 8.21 -> 6.14 of `gpu_busy` (-25%) in today's order; 4.61 ->
2.69 (-42%) and 7.90 -> 6.04 (-24%) with the field compose. At 2x: 0.215 -> 0.155 (-28%). The last two 10x rows were also
run in reverse order (skip first: 2.98 / 3.00, then 4.79 / 5.28), so the difference is not an ordering or warm-up
effect. The 480p races are unchanged (they deinterlace nothing).

### 6. What is left

- **The decision is still the largest part of the adaptive pass, and it is now the only thing in it (C, the 10x rows).**
  With the field compose, the 46.5% of missing-row pixels the skip removes are 1.9 ms of the 4.6, so the whole decision is
  about 4 ms of that 4.6; a study probe that stored the woven row and returned (no decision at all, temporary shader, not
  shipped) measured 0.65 ms of deinterlace in the same batch. The remaining 2.7 ms is the other 52.8% of missing-row
  pixels, which run the three base comparisons and the five-column k loop in full. The exact form of that reduction is
  *not* a per-native decision (it changes the picture, section 2): it is to compute the three comparisons **once per
  output pixel** into a small "moved" image and then dilate it over the +-2 native columns the k loop samples (about 30
  word loads per missing pixel cut to 6-8, one word per row and column group instead of six). It needs a new image, a
  second dispatch and a packing layout, so it is a step of its own; it is reported here with its cost model and not
  implemented.
- **The step 2b proposal would have been worth real time, had it been exact.** The decision is *not* free: it is 85-90%
  of the adaptive pass at 10x (4.6 -> 0.65 ms with no decision at all). The proposal's only problem is that its decision
  is per native pixel, which is a different picture.
- **The phone (Hyp, not run).** The S23's 30+ ms at 6x is about ten times the desktop's whole 10x deinterlace for 2.8x
  the pixels, so its cost is dominated by per-pass and per-barrier work, not by the kernel's arithmetic. The skip removes
  work inside the kernel and should pay in proportion to the decision share there; that share is unmeasured on Adreno.
- **Skipping an unchanged frame, present, and `GT4_CORE2_POST_AT_SCREEN`** are unchanged from section 6 of step 2b.

## Step 3: the two cheapest exact asynchronies around the readback (rows 2 and 5)

Date 2026-11-05, same branch (`task/182-barrier-scope`) and the same **not PCSX2-tainted** status as steps 1-2c: no PCSX2
source, no `gt4recomp-gs` fork, no `private/tooling/pcsx2-*`, no `private/slice189-pcsx2-ref` opened. The outside input is
the clean-room prose of `STUDY-ASYNC-READBACK.md` (9.1 rows 2 and 5, section 2) and `STUDY-FRAME-DOWNLOAD-COST.md` (9.3,
9.6). Neither change touches what is drawn: both move *when the CPU waits* and *when it submits*, never a command, a
barrier or a memory-write order.

### 1. The two rows

**Row 2, the K rule** (TASK-178). `GT4_CORE2_SUBMIT_AFTER_WAIT=K` sends the first mid-frame submission after a waited
readback K draw batches later instead of `submitEvery` batches later. A readback drains the queue, so when the GS worker
resumes the GPU has nothing queued and everything the worker records until its next submission is GPU idle. K shortens
that bubble. Exact because submission boundaries never change what is drawn (TASK-178 9.2: the same flare reply hashes at
K = 0, 8, 16, 32 and `gates6 wrong=0`). It shipped in rc15 with K = 0 by default, waiting for an A/B.

**Row 5, O2.** When the GS worker must wait for a readback, `wait_all` waited for **every slot's fence** (up to 16
`vkWaitForFences`). One fence is enough; section 2 is the argument, and section 3 the optional spin.

### 2. O2: exactly one fence, and why it is the same wait

Every submission of `Gpu` goes to the one queue `queue`, in `vkQueueSubmit` order. Vulkan signals a submission's fence only
after that submission **and every submission before it on the same queue** have completed (Vulkan 1.3 spec, fences: "if a
fence is signaled, then all commands submitted before it ... have completed"). `Reduce`, `ReduceMany` and `ReadClut` hand
the read's recording to `submit(true)`, so at the wait the read's submission **is** the newest one. Waiting for its fence
therefore implies every earlier slot's completion - exactly what waiting for all of them returned - and the guest bytes, the
GS-memory write-back and the reduced words become visible at the same instant. Nothing observable moves; the number of
blocking driver calls falls from up to 16 to one, and with it the number of scheduler wake-ups (9.1 row 5's "no 16
sequential `vkWaitForFences` calls"; `10-readbacks-and-sync.md` 10.5 already asked for the same thing for HWR).

The older in-flight slots keep their own bookkeeping (their timestamps are read and their doomed images released inside
`wait_slot`) until their slot is reused by `begin_recording` - which waits for it there - or until the final drain. That is
the same work at the same total cost as before, only off the read's critical path instead of on it: the first version of
this change waited for the older fences too (they return immediately once the newest signalled), and the A/B showed the
bookkeeping it moved after the wait cost 0.1-0.3 ms per drain, which is why it is deferred now. A submission whose slot is
still marked in flight is never reused before `begin_recording` waits for it, and `~Gpu` waits for every slot before it
destroys the images and query pools. `GT4_CORE2_WAIT_NEWEST_FENCE=0` restores the slot-by-slot wait for the A/B. The code
comment in `Gpu::Impl::wait_newest` carries the same argument.

### 3. The bounded spin

`GT4_HW_READBACK_SPIN` was parsed for the donor renderer (TASK-087) and documented in `10-readbacks-and-sync.md` 10.5, but
core 2 had no consumer for it. It now spins on the read's fence before blocking: `vkWaitForFences` with a timeout of 0 in a
loop, bounded by a wall budget (`=1`: 200 us; a larger number: that many microseconds, capped at 100 ms). A poll that sees
the fence signalled returns at once and skips the blocking call; a poll that does not costs at most the budget and then
blocks as before. Same semantics, one fewer trip through the driver's wait queue. Default off (0 or unset).

### 4. The device default of the K rule

`hw_device_profile.hpp` gains `unified_memory` - a memory type that is both device-local and host-visible, the Vulkan test
for "the CPU and the GPU share this memory" - filled by `probe_device_profile` from the memory types next to the heaps, and
three pure functions: `k_rule_default_on`, `k_rule_default_batches` and `k_rule_default_reason`. `Gpu::Init` classifies
the physical device itself when the device's creator did not probe it, so a fixture or a test path never lands on a
"discrete" default by accident.

| Device class | K by default | Why |
|---|---|---|
| `tiler_or_unknown` (Adreno, Mali, PowerVR, Apple, Xclipse, software, virtual) | **8** | tile-based: a drain is a full pipeline flush and the CPU is slow against the GPU |
| `integrated` with `unified_memory` or the integrated device type | **8** | an APU: one memory for both, so the GPU competes with the recording CPU |
| `integrated` without either | 0 | its own memory, no evidence |
| `discrete` | **0** | the desktop A/B (section 6) |

Two findings worth keeping:
- **This desktop reports a device-local host-visible memory type.** The RX 9060 XT (resizable BAR) exposes a
  `DEVICE_LOCAL | HOST_VISIBLE` type, so the raw unified-memory bit is *yes* on a discrete card (the `[hw] device profile`
  line says so). The class therefore wins over the bit: a discrete GPU stays at K = 0 whatever its memory types report,
  and the fixture pins that case.
- The device default is clamped to `GT4_CORE2_SUBMIT_EVERY`, so a user who asked for a cadence below 8 (or for
  submissions only when needed, `0`) never has it silently lengthened.

The K, its reason and the wait mode are in game.log in every run, and both switches still override:

```
[core2] submissions: 16 slots, every 64 batches, after a wait 0 batches (TASK-182 step 3: device default, discrete GPU; 0 = off)
[core2] readback wait: newest fence only (GT4_CORE2_WAIT_NEWEST_FENCE=1), block at once (GT4_HW_READBACK_SPIN off)
```

The 600-VBlank download line carries the new counters: `newest_fence_waits/vblank`, `spin_polls/vblank (hits ...)`.

### 5. Identity (C)

Four configurations of the same build, headless, Spec II press scenes, full 9,876/9,860-VBlank runs:
`off` = `GT4_CORE2_WAIT_NEWEST_FENCE=0` (the pre-step-3 slot-by-slot wait, K 0), `k8` = the K rule on and the old wait,
`o2` = the newest-fence wait only (the default), `both` = both.

| Scene | Reference (study 3) | off | k8 | o2 | both |
|---|---|---|---|---|---|
| Hong Kong 2x, 6,835 replies | `2f4c1cc9f174d00e` | same | same | same | same |
| Hong Kong 10x, 6,835 replies | `61109e8f3143264a` | same | same | same | same |
| Motegi 2x, 6,588 replies | `7bf932fbb8799965` | same | same | same | same |

All **16** cumulative `[hw] flare replies` lines of every run are identical to the reference, not only the final hash
(`build/evidence182/id3-hashes-final.txt`). The same run set was repeated on every build along the way - the first O2
(which moved the older slots' bookkeeping after the newest fence), the same with the final log lines, and the final
deferred-bookkeeping O2 - and every repetition reproduced every hash, which is the expected result for switches that only
move when the CPU waits.

`gates6.sh` (the TASK-178 copy of the TASK-134 gate, read only, candidate = this build: `wrong=0`):

| Gate | Result |
|---|---|
| Spec II George V / El Capitan / Tokyo R246, 1x | race clock identical, simulation state identical (212,992 B), `IDENTITY-OK` x3 |
| v2.00 night race 3x | state identical (131,072 B), clock identical (the reference folder's 10 frames were missing, see the gap below) |
| v2.00 Arcade path 3x | 14 frames identical |
| Spec II George V lap 6x | 12 frames identical |
| VU1/VU0 against the interpreter (Spec II 1x and v2.00 night 1x) | `verify_mismatches=0` |
| MTVU check | 8,032 frames, 0 mismatches |

Full CTest, serial (`ctest --test-dir build/t178-spec2 -j 1`): **321 tests, 100% passed, 0 failed, 1 skipped**
(`hw_replace_q07_pcsx2_dumps`), 647.3 s. The new cases live in the existing `hw_core2_gpu_clut`:
`GT4_CORE2_WAIT_NEWEST_FENCE` off against on over the stream fixture and the ageing fixture (images and GS memory
bit-identical, `newestFenceWaits` 0 -> 14 and 0 -> 71, so the path really ran) and `GT4_HW_READBACK_SPIN` off against on
(identical, `spinPolls` 0 -> 16,320); `hw_device_profile` pins the class rule (discrete 0, a tiler 8, an APU with unified
memory 8, and a discrete card that reports unified memory still 0) and the new profile-line field. The Python suite
against the same build (`GT4_NATIVE_BUILD=build/t178-spec2`): 802 tests, OK, 70 skipped, 52.6 s.

The r14pgo reference folders of the three 1x Spec II recordings and of the v2.00 night race had **lost their `frames`**
before this step (0 files, while the candidate runs dumped 24, 35, 35 and 10), so `gates6.sh` first reported "0 identical,
0 differ" for them: their state and clock comparisons ran, their frame comparisons could not. The reference frames were
regenerated from the same r14pgo executables with the same scripts and compared against this build (section 6a): **24, 35,
35 and 10 frames, 0 differ** - the same counts TASK-178 recorded.

### 6a. The four reference frame sets, regenerated and compared (C)

`build/t182-step3-ref-frames.sh` runs the r14pgo executables of `gt4-wt-134-game-code-study` with the exact `replay-run.sh`
/ `night-run.sh` environment `gates6.sh` uses, into new `-frames` folders, so no existing evidence was touched:

| Reference pair | Frames | Compare with this build |
|---|---|---|
| George V 1x (`r14pgo-spec2`) | 24 | 24 identical, 0 differ |
| El Capitan 1x (`r14pgo-spec2`) | 35 | 35 identical, 0 differ |
| Tokyo R246 1x (`r14pgo-spec2`) | 35 | 35 identical, 0 differ |
| v2.00 night race 3x (`r14pgo-v200`) | 10 | 10 identical, 0 differ |

Each regenerated `state.bin` is byte-identical to the old reference state (212,992 B for the three 1x recordings, 131,072 B
for the night race, `cmp` clean), so the reference is the same state as the one the earlier comparisons used.

### 6. Indicative desktop A/B (Hong Kong and Motegi, 10x and 12x)

Method: `build/t182-step3-ab.sh` (same run naming as `private/task178-tools/ab.sh`, so `ab_table.py` reads it), paired and
interleaved **A B C C B A** per scene and scale, 7,200-frame runs, headless, unpaced, `GT4_HW_PROFILE_EVERY=600`, the
TASK-168 press timelines. Configurations: **0** = K 0 + the old slot-by-slot wait (the pre-step-3 behaviour); **1** = K 0 +
the newest fence (O2); **2** = K 8 + the old wait, so K is measured against config 0 and O2 against config 1, one switch at
a time. The spin has its own pair (3 = newest fence, 4 = newest fence + `GT4_HW_READBACK_SPIN=1`) at 10x. Two runs per
configuration and cell; each run's value is the mean of its 600-VBlank lines inside the TASK-178 window (4,476-9,876 for
Hong Kong, 4,460-9,860 for Motegi), so five lines per run.

**How noisy it is (C).** The desktop is shared and this is the whole honesty of the table:
- No other game process ran during any run of the final waves (`foreign_samples=0`, `waited_s=0` in every `done.txt`),
  except the runs named below, where another agent's game was seen for 9-22 samples of the run.
- Runs of the *same* configuration and scene differ by up to **1.4 ms/VBlank** (Hong Kong 10x, K 8: 14.25 and 15.61), and
  `gpu_busy` differs by up to 1.1 ms for identical content: the card's clock moves between runs. That spread is larger
  than every effect measured below.
- Some runs were hit by load that is not a game process (a build, the Android emulator). They are named and **excluded**:
  Hong Kong 10x r6 (127 s against 83 s, GPU idle 3.16 ms/drain), Motegi 10x r6 (same shape), Hong Kong 12x r2, Motegi 12x
  r2, and the spin pair's Hong Kong r4 and Motegi r1/r4.
- An earlier wave with the first O2 (which did the older slots' bookkeeping *after* the newest fence) is not in this table:
  it measured +0.1 to +0.3 ms per drain on Motegi in all four cells, which is why the final O2 defers that bookkeeping
  instead.

**10x** (ms/VBlank unless stated; single value = median of the two runs, when both are clean):

| Scene | cfg | wall | read wait | GPU idle/drain | CPU gap/drain |
|---|---|---|---|---|---|
| Hong Kong | 0 | 15.62 | 9.11 | 1.05 | 0.86 |
| Hong Kong | 1 (O2) | 15.66 (15.63, 15.69) | 8.24 | 1.01 | 0.86 |
| Hong Kong | 2 (K 8) | 14.93 (14.25, 15.61) | 8.47 | **0.70** | **0.51** |
| Motegi | 0 | 10.90 | 4.33 | 0.79 | 0.61 |
| Motegi | 1 (O2) | 10.89 | 4.31 | 0.78 | 0.63 |
| Motegi | 2 (K 8) | 10.79 (10.78, 10.79) | 4.32 | **0.59** | **0.41** |

**12x**:

| Scene | cfg | wall | read wait | GPU idle/drain | CPU gap/drain |
|---|---|---|---|---|---|
| Hong Kong | 0 | 21.79 (21.35, 22.22) | 12.84 | 1.83 | 1.25 |
| Hong Kong | 1 (O2) | 21.42 | 12.74 | 1.14 | 0.96 |
| Hong Kong | 2 (K 8) | 21.10 | 12.70 | **0.77** | **0.56** |
| Motegi | 0 | 15.31 (15.29, 15.32) | 6.62 | 0.94 | 0.72 |
| Motegi | 1 (O2) | 15.28 | 6.61 | 0.91 | 0.73 |
| Motegi | 2 (K 8) | 15.08 | 6.81 | **0.64** | **0.45** |

**Spin, 10x** (config 3 against 4):

| Scene | no spin | spin | read wait (no spin / spin) |
|---|---|---|---|
| Hong Kong | 14.62 | 15.57 (15.97, 15.17) | 9.17 / 7.92 |
| Motegi | 11.09 | 12.07 (13.03, 11.10) | 4.23 / 4.46 |

**Reading (C for the counters, H for the wall).**
- **The mechanisms are visible in the counters in every cell.** K 8 takes the GPU idle after a drain from 0.94-1.83 to
  0.64-0.77 ms and the CPU gap with it (0.72-1.25 to 0.45-0.56); O2 removes the slot-by-slot fence calls, which lowers the
  CPU gap in the same direction (`newest_fence_waits/vblank` equals the drain rate in every O2 run and is 0 in every
  baseline run).
- **K's wall medians fall in all four cells by 0.11 (Motegi 10x) to 0.69 ms (both Hong Kong cells)**, and the two runs of
  a cell agree to 0.01 ms at Motegi 10x and 0.08 at Motegi 12x, but Hong Kong's two K 8 runs differ by 1.36 ms. With this
  spread the result does not reach the project's bar (0.3 ms/VBlank on two scenes with no loss on the third: Hong Kong
  clears it at both scales, Motegi does not), so **the discrete default stays 0** and the study's "K 8 on tilers and
  unified memory" default is all the K rule gets by default.
- **O2 is neutral in the wall** (+0.04 and -0.01 at 10x, -0.37 and -0.03 at 12x) and never worse outside the spread, with
  the CPU gap lower wherever the runs are clean. It is exact, it removes up to 15 driver wake-ups per readback, and its
  intended gain (a wake-up that costs more than the polling budget) is on the phone; the switch, default **on**, is how the
  phone tests it.
- **The spin shows no gain**: +0.95 ms on Hong Kong with the read wait lower (7.92 against 9.17), and Motegi's two spin
  runs differ by 1.93 ms. Default stays **off** (0), as the brief required unless it measured a gain.

### 7. What step 3 leaves

- **The phone run decides both defaults.** One 600-VBlank Nordschleife run at 2x in normal mode with
  `GT4_HW_PROFILE_EVERY=600` on the S23 gives `drains/vblank`, `gpu_idle_ms/drain`, `wait_ms/readback_download`,
  `newest_fence_waits/vblank` and `spin_polls/vblank`; then the same scene with `GT4_CORE2_SUBMIT_AFTER_WAIT=0/8` and
  `GT4_CORE2_WAIT_NEWEST_FENCE=0/1` decides whether K 8 and O2 stay. Nothing in the code needs to change for that A/B.
- **The spin is the one to revisit there**: the desktop's fence wake-up is too cheap for a 200 us budget to matter, and a
  phone's is the opposite (Hyp). If the phone shows a gain, `GT4_HW_READBACK_SPIN=1` is the only switch to flip.
- **K 8 is not the phone's whole answer**: the S23's read wait is GPU backlog (11.5 ms GPU against 8.7 ms GS CPU, the UMA
  study), so K shortens the bubble behind the read but not the backlog itself; the store-before-write merge (section 7 of
  the barrier study, row 4 of 9.1) is the larger phone lever and is still open.
- **`gpu_busy` is not comparable across K settings** (it sums per-submission spans and the spans overlap): read it only
  inside one configuration, and use the wall for the comparison.

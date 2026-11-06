"""Run a qualified external pilot with pinned inputs and private outputs."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time
import tomllib

from cpu_affinity import apply_process_affinity, plan_affinity
import gs_settings
from gt4disc import fingerprint


SEM_FAILCRITICALERRORS = 0x0001
SEM_NOGPFAULTERRORBOX = 0x0002


def suppress_loader_dialogs():
    """Windows: a child that cannot load a DLL (for example SDL3.dll beside gt4vulkan.exe) fails quietly.

    The error mode is inherited by child processes created afterwards, so the run never opens a system error dialog
    on the owner's desktop. No effect on other platforms.
    """
    if os.name != 'nt':
        return
    import ctypes
    ctypes.windll.kernel32.SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX)


ROOT = Path(__file__).resolve().parents[1]
CAPTURE_MANIFEST = 'slice136-keyboard-input.json'
CAPTURE_RUNTIME = 'private/slice136-keyboard-input/build/gt4vulkan.exe'
PERF_MANIFEST = 'slice143-performance-candidate.json'
PERF_RUNTIME = 'private/slice143-performance-candidate/build/gt4vulkan.exe'
PERF_PROFILES = {
    'vu-split': ('slice149-vu-unit-attribution.json',
                 'private/slice149-vu-unit-attribution/build/gt4vulkan.exe'),
    'detailed': ('slice145-performance-detailed.json',
                 'private/slice145-performance-pair/detailed/build/gt4vulkan.exe'),
    'frame-only': ('slice145-performance-frame-only.json',
                   'private/slice145-performance-pair/frame-only/build/gt4vulkan.exe'),
    # VU1 AOT candidate: generated VU1 programs in place of the interpreter (O1c).
    'vu1-aot': ('slice151-vu1-aot.json', 'private/slice151-vu1-aot-build/build/gt4vulkan.exe'),
    # O2 candidate: VU1 AOT plus asynchronous GS submission (GT4_GS_SYNC=1 restores the synchronous path).
    'gs-async': ('slice152-gs-async.json', 'private/slice152-gs-async-build/build/gt4vulkan.exe'),
    # O4 candidate: gs-async plus the paced clock (GT4_PACING=audio restores the blocking audio clock).
    'pacing': ('slice153-pacing.json', 'private/slice153-pacing-build/build/gt4vulkan.exe'),
    # O1 follow-up candidate: pacing plus per-program VU1 flag elision (slice154 table and capsule).
    'vu1-flags': ('slice154-vu1-flags.json', 'private/slice154-vu1-flags-build/build/gt4vulkan.exe'),
    # TASK-008 candidate: vu1-flags plus the static VU1 table with 426 entries generated from the pinned image.
    'vu1-full': ('slice155-vu1-full.json', 'private/slice155-vu1-full-build/build/gt4vulkan.exe'),
    # TASK-009 candidate: vu1-full with 16:9 fixed, fullscreen toggle, interval counters and the GT4_FORCE_480P hook.
    'quickfix': ('slice156-quickfix.json', 'private/slice156-quickfix-build/build/gt4vulkan.exe'),

    # TASK-008 candidate: vu1-full with the all-entries table (every 2,048 addresses, split into 128 parts).
    'vu1-all': ('slice157-vu1-all.json', 'private/slice157-vu1-all-build/build/gt4vulkan.exe'),
    # TASK-009 candidate: vu1-all with the quick fixes and the 480p CRT table rewrite (GT4_FORCE_480P=1).
    'quickfix2': ('slice158-quickfix2.json', 'private/slice158-quickfix2-build/build/gt4vulkan.exe'),
    # TASK-009 candidate (slice164): vu1-all with the quick fixes; GT4_FORCE_480P=1 holds the race display index at 480p.
    'quickfix3': ('slice164-quickfix3.json', 'private/slice164-quickfix3-build/build/gt4vulkan.exe'),
    # TASK-013 candidate: vu1-all plus the VU0 AOT overlay with the 14 VCALLMS entries (slice 160).
    'vu0-aot': ('slice160-vu0-aot.json', 'private/slice160-vu0-build/build/gt4vulkan.exe'),
    # Current main (slice178, release 1.0.0 W1): the slice166 main features plus the slice172 MTVU worker runtime
    # (GT4_MTVU default 1; 0 is the A/B path). The launcher holds 480p by default in this mode (play_performance.py).
    # slice166 (main without MTVU) stays pinned as history: docs/inputs/slice166-main.json.
    'main': ('slice178-main.json', 'private/slice178-main-build/build/gt4vulkan.exe'),
    # TASK-011 step 0 (slice169): main plus MTVU measurement counters ([mtvu-measure] lines in F8;
    # GT4_MTVU_MEASURE=0 disables them). No guest-visible change.
    'mtvu-measure': ('slice169-mtvu-measure.json', 'private/slice169-mtvu-measure-build/build/gt4vulkan.exe'),
    # TASK-011 step 2 (slice170): VU1 worker thread. GT4_MTVU (default on) selects the worker; 0 is the main path.
    # slice171 (TASK-011 step 3): the slice170 worker runtime with per-thread accounting ([mtvu] F8 lines).
    # slice172 (TASK-011 step 4): SIGNAL/FINISH/LABEL applied at acceptance; the launcher mode mtvu points here.
    'mtvu': ('slice172-mtvu.json', 'private/slice172-mtvu-build/build/gt4vulkan.exe'),
    'mtvu-slice171': ('slice171-mtvu.json', 'private/slice171-mtvu-build/build/gt4vulkan.exe'),
    # TASK-035 (slice191): main plus the GS stream seam. The manifest's gs_stream block makes the launcher run
    # GT4_RENDERER=pcsx2 with the pinned test consumer (tools/gs_stream_record): no image; not for owner play yet.
    'pcsx2-stream': ('slice191-pcsx2-stream.json', 'private/slice191-gs-stream-build/build/gt4vulkan.exe'),
    # TASK-036 (slice193): the live PCSX2 GS process. The manifest's gs_stream block pins the gt4-gs package
    # (private/slice193-gt4gs-pkg, GPL fork moesuito/gt4recomp-gs); the GS process renders into the game window.
    'pcsx2-slice193': ('slice193-pcsx2.json', 'private/slice193-gs-live-build/build/gt4vulkan.exe'),
    # TASK-037/038 (slice194): readback modes and timing, own gt4-gs data and cache folders, deferred swap chain
    # acquire, window hardening and crash handling (gt4-gs 0.3.0, private/slice194-gt4gs-pkg).
    'pcsx2-slice194': ('slice194-pcsx2.json', 'private/slice194-gs-window-build/build/gt4vulkan.exe'),
    # slice195: slice194 plus the click fix (gt4-gs 0.3.1 gives the foreground back to the game window after a click on
    # the picture; the game keeps the keyboard focus on activation).
    'pcsx2-slice195': ('slice195-pcsx2.json', 'private/slice195-gs-window-build/build/gt4vulkan.exe'),
    # TASK-047 (slice196): 480i deinterlacing (SetGsCrt posts SYNCV; gt4-gs 0.3.2 presents with the VSYNC display
    # field, protocol 1.1).
    'pcsx2-slice196': ('slice196-pcsx2.json', 'private/slice196-gs-deinterlace-build/build/gt4vulkan.exe'),
    # TASK-041 (slice198): privileged GS writes queued in the MTVU journal instead of draining the VU1 worker
    # (GT4_MTVU_PRIV_ASYNC, default 1), one copy less per worker GIF record, F8 [gs-ipc] per-register lines and the EE
    # critical path summary. Same gt4-gs 0.3.2 package as slice196.
    'pcsx2-slice198': ('slice198-pcsx2.json', 'private/slice198-gs-perf-build/build/gt4vulkan.exe'),
    # TASK-042 (slice197): the JIT-free gt4-gs 0.4.0 (no recompilers, no GS software JIT, no executable memory,
    # Arbitrary Code Guard). Same game runtime as slice196.
    'pcsx2-slice197': ('slice197-pcsx2.json', 'private/slice196-gs-deinterlace-build/build/gt4vulkan.exe'),
    # TASK-041 (slice199): the slice198 game runtime with the JIT-free gt4-gs 0.4.0 (no recompilers, no GS software JIT).
    'pcsx2-slice199': ('slice199-pcsx2.json', 'private/slice198-gs-perf-build/build/gt4vulkan.exe'),
    # TASK-058/059 (slice200): the slice199 candidate with the crash-safe memory card (memcard capsule), the installed-folder
    # paths and --portable-check (port slice200-port).
    'pcsx2-slice200': ('slice200-pcsx2.json', 'private/slice200-build/build/gt4vulkan.exe'),
    # TASK-060/061/062 (slice201): cursor auto-hide, protocol 1.2 OSD_STATS for the performance overlay, and the stable
    # gt4-gs 1.0.0 ("Compiling shaders" notice, overlay, settings allow-list).
    'pcsx2-slice201': ('slice201-pcsx2.json', 'private/slice201-osd-build/build/gt4vulkan.exe'),
    # slice202: one candidate with both: the slice200 game-side changes (TASK-058/059) and the slice201 ones
    # (TASK-060/062), with the stable gt4-gs 1.0.0 package of slice201.
    'pcsx2-slice202': ('slice202-pcsx2.json', 'private/slice202-build/build/gt4vulkan.exe'),
    # slice203 (TASK-044): the slice202 game executable (same hash) with the stable gt4-gs 1.0.1 package (no writes under
    # Documents\PCSX2). Kept as pcsx2-slice203 after TASK-017.
    'pcsx2-slice203': ('slice203-pcsx2.json', 'private/slice203-build/build/gt4vulkan.exe'),
    # TASK-017 (slice206): the slice203 game recipe plus the license demo guard v2 (both demo loaders), gt4-gs 1.0.1.
    # Halt dump and FSV trace are built in and off unless GT4_HALT_DUMP / GT4_FSV_TRACE are set. History mode after slice209.
    'pcsx2-slice206': ('slice206-pcsx2.json', 'private/slice206-build/build/gt4vulkan.exe'),
    # TASK-019 launcher test (slice209): the slice208 coverage build (accepted EE library, 1,105 seeds) plus the
    # GT4_FULLSCREEN start (native/window.cpp open_window). Mode pcsx2 points here since the owner approved the coverage
    # build as the default (TASK-014). Only the window start changes from slice208; the guest frames are the same.
    # TASK-088 (release 1.2.0 test build): the slice218 build with the black game window background (native/window.cpp).
    # Kept as history; mode pcsx2 now points at the TASK-084 frame-cost candidate.
    'pcsx2-slice240': ('slice240-pcsx2.json', 'private/slice240-game-build/build/gt4vulkan.exe'),
    # TASK-084 (release 1.2.2): the slice240 recipe with the frame-cost runtime (instrumentation, VIF1 chain jobs, GIF
    # fast path, VBlank journal). Kept as history; mode pcsx2 now points at the release 1.2.3 build.
    'pcsx2-slice262': ('slice262-pcsx2.json', 'private/slice262-build/build/gt4vulkan.exe'),
    # TASK-092 (release 1.2.3): slice262 plus the MTVU test-switch fixes and null Donor callbacks (the 1.2.2 CoreGS 1.1
    # crash). Kept as history after the 1.3.0 release (TASK-100/TASK-101).
    'pcsx2-slice268': ('slice268-pcsx2.json', 'private/slice268-build/build/gt4vulkan.exe'),
    # TASK-100/TASK-101 (release 1.3.0): the task/101-multi-game source with the standalone start (TASK-097), the
    # target-aware hooks and both games; v2.00 release build, slice262 capsules, gt4-gs 1.2.1.
    'pcsx2': ('slice279-pcsx2.json', 'private/slice279-build/build/gt4vulkan.exe'),
    # TASK-080 addendum (slice218): the slice217 build plus the seamless display host code (GT4_OUTPUT_SIZE gives the
    # auto aspect, GT4_MONITOR the monitor, per-monitor DPI awareness).
    'pcsx2-slice218': ('slice218-pcsx2.json', 'private/slice218-build/build/gt4vulkan.exe'),
    # TASK-080 P2 (slice217): the slice215 build plus the race HUD for other ratios (HUD canvas hook in the slice217
    # license guard capsule, HUD parameter block, EE library profile "aspect" re-translated from the ELF patch table,
    # protocol 1.3 fill target). History.
    'pcsx2-slice217': ('slice217-pcsx2.json', 'private/slice217-build/build/gt4vulkan.exe'),
    # TASK-080 (slice215): the slice214 release build plus native display aspect ratios (GT4_DISPLAY_ASPECT, the race
    # camera Hor+/Vert+ hook in the slice212c license guard capsule, race-view fill presentation, protocol 1.3 producer)
    # and the TASK-079 hold hardening. History.
    'pcsx2-slice215': ('slice215-pcsx2.json', 'private/slice215-build/build/gt4vulkan.exe'),
    # TASK-081 round 2 (slice214, release 1.0.0): the slice213 release build with the Vulkan baseline path (swapchain
    # maintenance 1 only when the driver has it; GT4_VULKAN_FORCE_BASELINE=1 forces the plain path). History.
    'pcsx2-slice214': ('slice214-pcsx2.json', 'private/slice214-build/build/gt4vulkan.exe'),
    # TASK-081 (slice213): the slice211 recipe as a release build (GT4_RELEASE_DISTRIBUTION=ON: no build-time shader
    # folder in the executable; shaders from <exe folder>\shaders). History.
    'pcsx2-slice213': ('slice213-pcsx2.json', 'private/slice213-build/build/gt4vulkan.exe'),
    # TASK-080 P1 (slice212b): the slice211 build plus the display aspect work (first owner test build). History.
    'pcsx2-slice212b': ('slice212b-pcsx2.json', 'private/slice212b-build/build/gt4vulkan.exe'),
    # TASK-079 (slice211): the slice210 build plus GT4_FORCE_WIDESCREEN (16:9 held at guest VBlank, default on).
    'pcsx2-slice211': ('slice211-pcsx2.json', 'private/slice211-build/build/gt4vulkan.exe'),
    # Mode pcsx2 pointed at slice211; slice210 stays as history.
    'pcsx2-slice210': ('slice210-pcsx2.json', 'private/slice210-build/build/gt4vulkan.exe'),
    # TASK-075/076 (slice210): the slice209 build with the SDL3 gamepad layer and the TASK-076 keyboard table.
    # Mode pcsx2-slice209 stays as history.
    'pcsx2-slice209': ('slice209-pcsx2.json', 'private/slice209-build/build/gt4vulkan.exe'),
    # TASK-017 diagnostic candidate (slice181): the slice166 profile plus the FSV read trace (GT4_FSV_TRACE=1
    # writes one [fsv] line per guest disc read to stderr). Not a performance candidate.
    'license-trace': ('slice181-license-trace.json', 'private/slice181-license-build/build/gt4vulkan.exe'),
    # TASK-017 halt-dump candidate (slice204): the slice181 license-trace profile plus the halt forensic dump in kernel.cpp.
    # The mode sets GT4_HALT_DUMP=1 and GT4_FSV_TRACE=1 for the game; [halt-dump] lines go to stderr at the halt.
    # Diagnostic only; not a performance candidate.
    'license-halt': ('slice204-halt-trace.json', 'private/slice204-halt-build/build/gt4vulkan.exe'),
    # TASK-017 license guard v1 (slice205, owner test 2026-10-16; guards the 0x001100D0 loader only). History.
    # (private/slice205-license-guard-runtime; halt dump and FSV trace built in, off unless GT4_HALT_DUMP/GT4_FSV_TRACE).
    'license-guard': ('slice205-license-guard.json', 'private/slice205-license-guard-build/build/gt4vulkan.exe'),
    # TASK-014 stage B (slice208): the slice206 game recipe with the accepted explicit-seed EE library (1,105 seeds,
    # docs/inputs/slice208-ee-entries.txt; library profile coverage). History mode: mode pcsx2 is slice209 (same library).
    'pcsx2-coverage': ('slice208-pcsx2.json', 'private/slice208-build/build/gt4vulkan.exe'),
}
# TASK-100/TASK-101 (release 1.3.0): the Spec II game build. The launcher starts it with the Spec II disc and the derived
# ELF (launcher gt4l-disc spec2.rs); the v2.00 probe modes do not run it. This constant keeps the release pin and the
# executable in one place for the release tests.
SPEC2_RELEASE = ('slice280-spec2.json', 'private/slice280-build/build/gt4vulkan-spec2.exe')
GS_ASYNC_MANIFEST = 'slice152-gs-async.json'
PACING_MANIFEST = 'slice153-pacing.json'
# Manifests whose binaries accept the VU1 AOT verification and interpreter options.
VU1_AOT_MANIFESTS = ('slice151-vu1-aot.json', GS_ASYNC_MANIFEST, PACING_MANIFEST, 'slice154-vu1-flags.json', 'slice155-vu1-full.json',
                     'slice156-quickfix.json', 'slice157-vu1-all.json', 'slice158-quickfix2.json', 'slice164-quickfix3.json',
                     'slice160-vu0-aot.json', 'slice166-main.json', 'slice169-mtvu-measure.json', 'slice170-mtvu.json',
                     'slice171-mtvu.json', 'slice172-mtvu.json', 'slice178-main.json', 'slice191-pcsx2-stream.json',
                     'slice193-pcsx2.json', 'slice194-pcsx2.json', 'slice195-pcsx2.json', 'slice196-pcsx2.json',
                     'slice197-pcsx2.json', 'slice198-pcsx2.json', 'slice199-pcsx2.json', 'slice200-pcsx2.json',
                     'slice201-pcsx2.json', 'slice202-pcsx2.json',
                     'slice203-pcsx2.json', 'slice205-license-guard.json', 'slice206-pcsx2.json', 'slice208-pcsx2.json',
                     'slice209-pcsx2.json', 'slice210-pcsx2.json', 'slice211-pcsx2.json', 'slice213-pcsx2.json',
                     'slice212b-pcsx2.json',
                     'slice214-pcsx2.json', 'slice215-pcsx2.json', 'slice217-pcsx2.json', 'slice218-pcsx2.json',
                     'slice240-pcsx2.json', 'slice260-pcsx2.json', 'slice262-pcsx2.json',
                     'slice268-pcsx2.json', 'slice279-pcsx2.json')
# Manifests whose binaries contain the VU0 AOT table and accept --vu0-verify.
VU0_AOT_MANIFESTS = ('slice160-vu0-aot.json', 'slice166-main.json', 'slice169-mtvu-measure.json', 'slice170-mtvu.json',
                     'slice171-mtvu.json', 'slice172-mtvu.json', 'slice178-main.json', 'slice191-pcsx2-stream.json',
                     'slice193-pcsx2.json', 'slice194-pcsx2.json', 'slice195-pcsx2.json', 'slice196-pcsx2.json',
                     'slice197-pcsx2.json', 'slice198-pcsx2.json', 'slice199-pcsx2.json', 'slice200-pcsx2.json',
                     'slice201-pcsx2.json', 'slice202-pcsx2.json',
                     'slice203-pcsx2.json', 'slice205-license-guard.json', 'slice206-pcsx2.json', 'slice208-pcsx2.json',
                     'slice209-pcsx2.json', 'slice210-pcsx2.json', 'slice211-pcsx2.json', 'slice213-pcsx2.json',
                     'slice212b-pcsx2.json',
                     'slice214-pcsx2.json', 'slice215-pcsx2.json', 'slice217-pcsx2.json', 'slice218-pcsx2.json',
                     'slice240-pcsx2.json', 'slice260-pcsx2.json', 'slice262-pcsx2.json',
                     'slice268-pcsx2.json', 'slice279-pcsx2.json')
# Host pacing overrides, forwarded unchanged and recorded in run evidence.
PACING_OVERRIDES = {'GT4_PACING': ('paced', 'audio'),
                    'GT4_PRESENT_MODE': ('fifo', 'mailbox', 'immediate')}
# TASK-060: GT4_CURSOR_AUTOHIDE (unset or 1 = hide the cursor over the picture after 3 s, 0 = never). Forwarded and
# recorded; the settings table row cursor_autohide (launcher-only) names it.
CURSOR_AUTOHIDE_VALUES = ('0', '1')
# GT4_CONTROLS_FILE (docs/CONTROLS.md): input bindings in a TOML file. Unset means the built-in map.
CONTROLS_SCHEMA = 1
CONTROLS_INPUTS = ('dpad_up', 'dpad_right', 'dpad_down', 'dpad_left', 'cross', 'circle', 'square',
                   'triangle', 'l1', 'r1', 'l2', 'r2', 'l3', 'r3', 'select', 'start',
                   'rstick_x', 'rstick_y', 'lstick_x', 'lstick_y')
CONTROLS_SOURCE_KINDS = ('xinput', 'key')
# GT4_FORCE_480P (slice164): holds the race display index at 480p; unset means off. Forwarded and recorded.
FORCE_480P_VALUES = ('0', '1')
# GT4_FORCE_WIDESCREEN (TASK-079, slice211): holds the game's wide flag at 16:9 at guest VBlank. Unset means on (the
# native default, the launcher default); 0 leaves the game's own setting. Forwarded and recorded; never scrubbed.
FORCE_WIDESCREEN_VALUES = ('0', '1')
FORCE_WIDESCREEN_DEFAULT = '1'
# GT4_DISPLAY_ASPECT and GT4_ASPECT_FOV_EXTRA (TASK-080, slice212b; rules in native/display_aspect.hpp). Unset means
# auto (the window's client area) and 1.0. Validated with the game's own rules, forwarded and recorded; never scrubbed.
DISPLAY_ASPECT_RANGE = (0.5, 8.0)
ASPECT_FOV_EXTRA_RANGE = (0.5, 3.0)


def _positive_decimal(text):
    """A plain positive decimal (digits and at most one dot, at most 16 characters) or None."""
    if not text or len(text) > 16 or text.count('.') > 1 or not all(c.isdigit() or c == '.' for c in text):
        return None
    if not any(c.isdigit() for c in text):
        return None
    value = float(text)
    return value if value > 0 else None


def display_aspect_ratio(value):
    """GT4_DISPLAY_ASPECT as the game reads it: None for auto, else the ratio. Raises ValueError like the game."""
    if value is None or value == 'auto':
        return None
    if ':' in value:
        width_text, _, height_text = value.partition(':')
        width, height = _positive_decimal(width_text), _positive_decimal(height_text)
        ratio = width / height if width and height else None
    else:
        ratio = _positive_decimal(value)
    if ratio is None or not DISPLAY_ASPECT_RANGE[0] <= ratio <= DISPLAY_ASPECT_RANGE[1]:
        raise ValueError('GT4_DISPLAY_ASPECT must be auto, 16:9, 4:3, W:H or a decimal ratio between 0.5 and 8')
    return ratio


def aspect_fov_extra(value):
    """GT4_ASPECT_FOV_EXTRA as the game reads it (default 1.0). Raises ValueError like the game."""
    if value is None:
        return 1.0
    factor = _positive_decimal(value)
    if factor is None or not ASPECT_FOV_EXTRA_RANGE[0] <= factor <= ASPECT_FOV_EXTRA_RANGE[1]:
        raise ValueError('GT4_ASPECT_FOV_EXTRA must be a decimal factor between 0.5 and 3.0')
    return factor


# GT4_OUTPUT_SIZE and GT4_MONITOR (TASK-080 addendum, slice218; rules in native/display_aspect.hpp and
# native/window_mode.hpp). The launcher sends both; unset means the window's client area and the primary monitor.
OUTPUT_SIDE_RANGE = (320, 16384)


def output_size(value):
    """GT4_OUTPUT_SIZE as the game reads it: None when unset, else (W, H). Raises ValueError like the game."""
    if value is None:
        return None
    width_text, separator, height_text = value.partition('x')
    sides = []
    for text in (width_text, height_text):
        if not separator or not text or len(text) > 5 or not all('0' <= c <= '9' for c in text):
            raise ValueError('GT4_OUTPUT_SIZE must be WxH with each side from 320 to 16384')
        sides.append(int(text))
    width, height = sides
    if not all(OUTPUT_SIDE_RANGE[0] <= side <= OUTPUT_SIDE_RANGE[1] for side in sides) or \
            not DISPLAY_ASPECT_RANGE[0] <= width / height <= DISPLAY_ASPECT_RANGE[1]:
        raise ValueError('GT4_OUTPUT_SIZE must be WxH with each side from 320 to 16384 and a ratio from 1:2 to 8:1')
    return width, height


def monitor_device(value):
    """GT4_MONITOR as the game reads it: '' for the primary monitor, else the device name. Raises ValueError."""
    if not value:
        return ''
    if not 5 <= len(value) <= 31 or not value.startswith('\\\\.\\') or \
            not all(0x20 < ord(c) < 0x7F for c in value):
        raise ValueError('GT4_MONITOR must be a monitor device name such as \\\\.\\DISPLAY2')
    return value
# GT4_MTVU_MEASURE (slice169): counters of the mtvu-measure build; unset means on. Forwarded and recorded.
MTVU_MEASURE_VALUES = ('0', '1')
# TASK-011 step 2 (slice170): worker runtime switches. Unset means the default shown; forwarded and recorded.
# GT4_MTVU_PRIV_ASYNC (TASK-041, slice198): privileged GS writes queued in the MTVU journal in pcsx2 mode (default on).
MTVU_SWITCHES = {'GT4_MTVU': '1', 'GT4_MTVU_VERIFY': '0', 'GT4_MTVU_STRICT': '0', 'GT4_MTVU_PRIV_ASYNC': '1',
                 # TASK-084 (slice260): frame-cost switches, all on by default (0 is the pre-TASK-084 behaviour).
                 'GT4_MTVU_CHAIN': '1', 'GT4_GIF_FASTPATH': '1', 'GT4_MTVU_VBLANK_JOURNAL': '1'}
# GT4_MTVU_REPORT_EVERY (slice171): VBlank count for the headless [mtvu] F8-equivalent report; unset means off.
MTVU_REPORT_EVERY = 'GT4_MTVU_REPORT_EVERY'
# TASK-035 (slice191): GS stream settings (native/gs_stream/gs_stream_client.hpp, tools/gs_stream_record).
GS_STREAM_RENDERERS = ('donor', 'pcsx2')
# GS display settings an owner run keeps from the caller (play_performance.py --gs-scale, --gs-readback, --gs-setting).
GS_USER_SETTINGS = ('GT4_GS_SCALE', 'GT4_GS_ASPECT', 'GT4_GS_SETTINGS', 'GT4_GS_READBACK', 'GT4_GS_EAGER_ACQUIRE',
                    'GT4_GS_WAKE_BYTES')
GS_STREAM_NAMES = ('GT4_RENDERER', 'GT4_GS_PROCESS', 'GT4_GS_CAPTURE', 'GT4_GS_CAPTURE_FRAMES', 'GT4_GS_FRAMES_IN_FLIGHT',
                   'GT4_GS_RECORD_OUTPUT', 'GT4_GS_RECORD_FRAMES', 'GT4_GS_PROCESS_SHA256', 'GT4_GS_CONFIG',
                   'GT4_GS_SCALE', 'GT4_GS_ASPECT', 'GT4_GS_READBACK', 'GT4_GS_EAGER_ACQUIRE',
                   'GT4_GS_HANDSHAKE_TIMEOUT_MS', 'GT4_GS_LOST_GRACE_FRAMES', 'GT4_GS_SETTINGS',
                   # TASK-041 (slice198): game-side test and A/B switches (wake batching, stream hash).
                   'GT4_GS_WAKE_BYTES', 'GT4_GS_STREAM_HASH',
                   # TASK-116 (rc6): the local flare readback (Performance mode) and its probe.
                   'GT4_GS_READBACK_LOCAL', 'GT4_GS_READBACK_LOG',
                   # TASK-117 (rc6): answer the flare table as zeros (no flares, no sun glare) and its A/B switch.
                   'GT4_GS_HIDE_FLARES')
# TASK-039: user GS settings as JSON (play_performance.py --gs-setting ID=VALUE and the shortcuts). The table:
# docs/launcher/gs-settings.toml; the gt4-gs.ini writer is gs_config_text below (scripts/gs_settings.py).
GS_SETTINGS_ENV = 'GT4_GS_SETTINGS'
# TASK-036 (slice193): the minimal gt4-gs.ini values, kept as the names the launcher and tests use.
GS_SCALES = tuple(str(n) for n in range(1, 13))
GS_DEFAULT_SCALE = '3'
# GT4_GS_ASPECT: display aspect of the GS output. 16:9 is the game's setting (widescreen card); auto is PCSX2's
# "Auto 4:3/3:2" (used to compare with the TASK-034 proof-of-concept images).
GS_ASPECTS = {'16:9': '16:9', '4:3': '4:3', 'auto': 'Auto 4:3/3:2'}
GS_DEFAULT_ASPECT = '16:9'
# PCSX2 enum values written to the ini: GSRendererType::VK, AccBlendLevel::High, GSInterlaceMode::Automatic.
GS_RENDERER_VULKAN = 14
GS_BLENDING_HIGH = 3
GS_DEINTERLACE_AUTOMATIC = 0
# TASK-037: GT4_GS_READBACK, the PCSX2 hardware download mode (HWDownloadMode, GSHardwareDownloadMode in PCSX2's
# Config.h): accurate = Enabled (0, the default: the reply waits for the GS thread and the GPU). TASK-186: the row is
# hw_download_mode (Readbacks) again, with the five values of the row before TASK-091; the shortcut accepts them all.
GS_READBACK_MODES = {'accurate': 0, 'enabled-force-full': 1, 'no-readbacks': 2, 'unsynchronized': 3, 'disabled': 4}
GS_DEFAULT_READBACK = 'accurate'
# The shader/pipeline cache and the gt4-gs data folder persist between sessions and candidates (same PCSX2 version);
# before slice194 gt4-gs.exe ignored them and used the PCSX2 user folder (Documents\PCSX2).
GS_SHADER_CACHE = 'private/slice193-gt4gs-cache'
GS_DATA_ROOT = 'private/slice193-gt4gs-data'
# Printed by the game when the player chose "restart with the built-in renderer" after a GS process failure.
GS_RESTART_MARKER = '[gs-ipc] renderer change requested: CoreGS 2.0'
# Headless runs write a PNG every N vsyncs (the table's DumpEvery row).
GS_HEADLESS_DUMP_EVERY = 100
# Owner-launched VU1 recording reuses the vu-split binary; only this manifest may record.
VU1_RECORD_MANIFEST = 'slice151-vu1-record.json'
VU1_RECORD_RUNTIME = 'private/slice149-vu-split-build/build/gt4vulkan.exe'
VU1_AOT_MANIFEST = 'slice151-vu1-aot.json'
VU1_CORPUS_NAME = 'vu1-corpus'
VU1_CORPUS_SUMMARY = 'vu1-corpus-summary.json'
VU1_PROGRAM_BYTES = 16384
VU1_NAME_PATTERN = re.compile(r'([0-9A-F]{16})_([0-9A-F]{4})\.bin')


def expected_performance_pin(manifest, runtime_workspace):
    inputs = runtime_workspace / 'docs/inputs'
    if manifest == (inputs / VU1_RECORD_MANIFEST).resolve():
        return VU1_RECORD_RUNTIME, 'vu-split'
    for mode, (name, runtime) in PERF_PROFILES.items():
        if manifest == (inputs / name).resolve():
            return runtime, mode
    return PERF_RUNTIME, None


# Inherited names that a performance run must not carry into the measured binary.
# GT4_PACING and GT4_PRESENT_MODE are not scrubbed: they are recorded and forwarded.
PERFORMANCE_SCRUB_PREFIXES = ('PS2_', 'GT4_PERF_', 'GT4_VU1_', 'GT4_VU0_')


def scrub_performance_environment(environment):
    """Removes inherited measurement-affecting names from a performance environment in place."""
    for name in list(environment):
        if name.upper().startswith(PERFORMANCE_SCRUB_PREFIXES):
            environment.pop(name)


def pacing_overrides(environment):
    """Validates GT4_PACING and GT4_PRESENT_MODE; returns the values to record in run evidence.

    Unknown values are refused here, before launch, and the native host refuses them too.
    Unset names are not recorded.
    """
    recorded = {}
    for name, allowed in PACING_OVERRIDES.items():
        if name not in environment:
            continue
        if environment[name] not in allowed:
            raise ValueError(f"{name} must be one of {', '.join(allowed)}")
        recorded[name] = environment[name]
    return {'pacing_environment': recorded} if recorded else {}


# Keyboard identities a performance candidate may carry: the slice136 digital-only fix, and the TASK-076 keyboard
# table that every pcsx2 manifest from slice210 on records (without it, --mode pcsx2 was refused).
PERFORMANCE_KEYBOARD_FIXES = (
    'digital_only_slice136',
    'TASK-076 keyboard table (controls.cpp default_controls, docs/CONTROLS.md keyboard-default); digital only',
)


def verify_performance_profile(manifest, evidence, runtime_workspace):
    expected_runtime, expected_mode = expected_performance_pin(manifest, runtime_workspace)
    if ((expected_mode is None and manifest != (runtime_workspace / 'docs/inputs' / PERF_MANIFEST).resolve()) or
            (expected_mode is not None and evidence.get('measurement_mode') != expected_mode) or
            (expected_mode in ('vu-split', 'vu1-aot', 'gs-async', 'pacing', 'vu1-flags', 'vu1-full', 'quickfix', 'vu1-all', 'quickfix2', 'quickfix3', 'vu0-aot', 'main', 'mtvu-measure', 'mtvu', 'pcsx2-stream', 'pcsx2', 'license-guard', 'pcsx2-coverage') and evidence.get('vu_scope_profile') != 'unit_split_run_fast_only') or
            evidence.get('profile') != 'performance' or
            type(evidence.get('perf_schema_version')) is not int or evidence['perf_schema_version'] != 1 or
            evidence.get('keyboard_input_fix') not in PERFORMANCE_KEYBOARD_FIXES or
            evidence.get('runtime', {}).get('path') != expected_runtime or
            evidence.get('capture_activation_implemented') is not False):
        raise ValueError('Performance test requires the qualified corrected candidate')


def package_tree_digest(directory):
    """SHA-256 of a package directory: one line "path<TAB>size<TAB>sha256" per file, sorted by POSIX path."""
    directory = Path(directory)
    lines = []
    for path in sorted(directory.rglob('*'), key=lambda item: item.relative_to(directory).as_posix()):
        if path.is_symlink():
            raise ValueError(f'GS package must not contain links: {path}')
        if path.is_file():
            lines.append(f'{path.relative_to(directory).as_posix()}\t{path.stat().st_size}\t{sha256_file(path)}\n')
    return hashlib.sha256(''.join(lines).encode('utf-8')).hexdigest(), len(lines)


def gs_settings_from_environment(environment, rows):
    """The checked GS settings of a pcsx2 run, from the table (docs/GS-SETTINGS.md).

    GT4_GS_SETTINGS (JSON from play_performance.py --gs-setting ID=VALUE) holds the user settings. The shortcuts
    GT4_GS_SCALE (upscale, 1 to 12, default 3), GT4_GS_ASPECT (aspect) and GT4_GS_READBACK (hw_download_mode, default accurate)
    set the same rows; a shortcut and a JSON setting for the same row conflict.
    """
    settings = {}
    if environment.get(GS_SETTINGS_ENV):
        settings = gs_settings.settings_from_json(environment[GS_SETTINGS_ENV], rows)
    index = gs_settings.rows_by_id(rows)
    shortcuts = (('GT4_GS_SCALE', 'upscale', 'GT4_GS_SCALE must be an integer from 1 to 12'),
                 ('GT4_GS_ASPECT', 'aspect', 'GT4_GS_ASPECT must be one of ' + ', '.join(
                     gs_settings.offered_choices(index['aspect']))),
                 ('GT4_GS_READBACK', 'hw_download_mode', 'GT4_GS_READBACK must be one of ' + ', '.join(
                     gs_settings.offered_choices(index['hw_download_mode']))))
    for variable, identifier, message in shortcuts:
        if variable not in environment:
            continue
        if identifier in settings:
            raise ValueError(f'{variable} and {GS_SETTINGS_ENV} both set {identifier}')
        try:
            settings[identifier] = gs_settings.parse_value(index[identifier], environment[variable])
        except gs_settings.SettingsError as error:
            raise ValueError(message) from error
    present = environment.get('GT4_PRESENT_MODE', 'fifo')
    if present not in PACING_OVERRIDES['GT4_PRESENT_MODE']:
        raise ValueError('GT4_PRESENT_MODE must be one of fifo, mailbox, immediate')
    if 'vsync' in settings or 'mailbox' in settings:
        if present != 'fifo':
            raise ValueError(f'GT4_PRESENT_MODE={present} and {GS_SETTINGS_ENV} both set vsync or mailbox')
    else:
        settings['vsync'] = 'false' if present == 'immediate' else 'true'
        settings['mailbox'] = 'false' if present == 'mailbox' else 'true'
    return settings


def gs_config_text(environment, headless_dump_directory=None, shader_cache=None, data_root=None):
    """The gt4-gs.ini of a pcsx2 run, generated from docs/launcher/gs-settings.toml (TASK-039, docs/GS-SETTINGS.md).

    With no settings the text is the TASK-036 default plus HWDownloadMode (accurate): Vulkan, 3x, blending High,
    deinterlace Automatic, 16:9, vsync, mailbox allowed, readback accurate. shader_cache passes the [Folders] Cache
    folder and data_root the [GT4GS] DataRoot folder. GT4_GS_EAGER_ACQUIRE=1 writes DeferSwapChainAcquire = false.
    Headless runs pass headless_dump_directory ([GT4GS] Surfaceless and PNG dumps every 100 vsyncs).
    """
    rows = gs_settings.load_table()
    settings = gs_settings_from_environment(environment, rows)
    eager = environment.get('GT4_GS_EAGER_ACQUIRE', '0')
    if eager not in ('0', '1'):
        raise ValueError('GT4_GS_EAGER_ACQUIRE must be 0 or 1')
    launcher = {}
    if shader_cache:
        launcher['cache_folder'] = str(shader_cache)
    if data_root:
        launcher['data_root'] = str(data_root)
    if eager == '1':
        launcher['defer_swap_chain_acquire'] = 'false'
    if headless_dump_directory:
        launcher['headless_surfaceless'] = 'true'
        launcher['headless_dump_dir'] = str(headless_dump_directory)
        launcher['headless_dump_every'] = str(GS_HEADLESS_DUMP_EVERY)
    return gs_settings.render_ini(rows, settings, launcher)


def gs_stream_environment(environment, runtime_evidence, binary_workspace, performance, run_directory=None,
                          headless=False, renderer_override=None):
    """TASK-035: sets the GS stream variables for a run (in place) and returns the values to record.

    Manifests without a gs_stream block never pass GT4_RENDERER or GT4_GS_* to their binary (removed). With the
    block (slice191), a performance run uses the manifest's renderer and its pinned GS process; a headless run
    keeps the caller's GT4_RENDERER (donor when unset) and, for pcsx2, defaults GT4_GS_PROCESS to the pinned
    process. The GS process must be one of the manifest's pinned runtime dependencies. renderer_override='donor' is the
    owner's restart with the built-in renderer (CoreGS 1.1) after a GS process failure (TASK-038).
    """
    stream = runtime_evidence.get('gs_stream')
    if not stream:
        for name in GS_STREAM_NAMES:
            environment.pop(name, None)
        return {}
    pinned = {item['path'] for item in runtime_evidence.get('runtime_dependencies', [])}
    if stream.get('gs_process') not in pinned or stream.get('renderer') not in GS_STREAM_RENDERERS:
        raise ValueError('GS stream manifest must pin its GS process as a runtime dependency')
    process = (Path(binary_workspace) / stream['gs_process']).resolve()
    if performance:
        # Owner runs use the manifest's renderer and pinned process; only the GS display settings stay the caller's.
        for name in GS_STREAM_NAMES:
            if name not in GS_USER_SETTINGS:
                environment.pop(name, None)
        environment['GT4_RENDERER'] = stream['renderer']
    if renderer_override is not None:
        if renderer_override != 'donor':
            raise ValueError('only the built-in renderer (donor) can replace the manifest renderer')
        environment['GT4_RENDERER'] = renderer_override
        for name in GS_USER_SETTINGS:
            environment.pop(name, None)
    renderer = environment.get('GT4_RENDERER', 'donor')
    if renderer not in GS_STREAM_RENDERERS:
        raise ValueError('GT4_RENDERER must be donor or pcsx2')
    if renderer == 'pcsx2' and not environment.get('GT4_GS_PROCESS'):
        environment['GT4_GS_PROCESS'] = str(process)
    if environment.get('GT4_GS_FRAMES_IN_FLIGHT', '2') not in ('1', '2', '3'):
        raise ValueError('GT4_GS_FRAMES_IN_FLIGHT must be 1, 2 or 3')
    extra = {}
    package = stream.get('gs_package')
    if package and renderer == 'pcsx2':
        # TASK-036: the pinned gt4-gs package (tree digest), the exe pin for the game's own check, and gt4-gs.ini.
        directory = (Path(binary_workspace) / package['directory']).resolve()
        digest, files = package_tree_digest(directory)
        if (digest, files) != (package['tree_sha256'], package['files']):
            raise ValueError(f'GS package differs from its pin: {directory} (tree sha256 {digest}, {files} files)')
        if Path(environment['GT4_GS_PROCESS']).resolve() == process:
            exe_pin = next(item for item in runtime_evidence['runtime_dependencies'] if item['path'] == stream['gs_process'])
            environment['GT4_GS_PROCESS_SHA256'] = exe_pin['sha256']
        if run_directory is not None and not environment.get('GT4_GS_CONFIG'):
            cache = (Path(binary_workspace) / GS_SHADER_CACHE).resolve()
            cache.mkdir(parents=True, exist_ok=True)
            data_root = (Path(binary_workspace) / GS_DATA_ROOT).resolve() if package.get('data_root', False) else None
            if data_root is not None:
                data_root.mkdir(parents=True, exist_ok=True)
            dumps = (Path(run_directory) / 'gs-frames') if headless else None
            config = Path(run_directory) / 'gt4-gs.ini'
            gs_settings.write_ini_atomic(config, gs_config_text(environment, dumps, cache, data_root))
            environment['GT4_GS_CONFIG'] = str(config)
        extra['gs_package'] = {'directory': package['directory'], 'tree_sha256': digest, 'files': files,
                               'source': package.get('source')}
        extra['gs_readback_mode'] = environment.get('GT4_GS_READBACK', GS_DEFAULT_READBACK)
        if extra['gs_readback_mode'] not in GS_READBACK_MODES:
            raise ValueError('GT4_GS_READBACK must be one of ' + ', '.join(GS_READBACK_MODES))
    elif renderer == 'pcsx2':
        for name in ('GT4_GS_SCALE', 'GT4_GS_ASPECT', GS_SETTINGS_ENV, 'GT4_GS_READBACK', 'GT4_GS_EAGER_ACQUIRE'):
            if name in environment:
                raise ValueError(f'{name} needs a manifest with a gt4-gs package (--mode pcsx2)')
    recorded = {name: environment[name] for name in GS_STREAM_NAMES if name in environment}
    if recorded:
        extra['gs_stream_environment'] = recorded
    return extra


def verify_manual_capture_profile(manifest, evidence, runtime_workspace):
    schema = evidence.get('capture_schema_version')
    if (manifest != (runtime_workspace / 'docs/inputs' / CAPTURE_MANIFEST).resolve() or evidence.get('profile') != 'observation' or
            evidence.get('capture_activation_implemented') is not True or type(schema) is not int or schema != 1 or
            evidence.get('runtime', {}).get('path') != CAPTURE_RUNTIME):
        raise ValueError('Shape capture requires the qualified manual observation runtime')


def verify_file(path, expected):
    with path.open('rb') as source:
        actual = fingerprint(source)
    if actual != expected:
        raise ValueError(f'Input differs from its pinned size/hash: {path}')


def summarize_exit(log_text, frame_count, exit_code, timed_out):
    stops = re.findall(r'\[ee\] stopped after (\d+) frames, (\d+) cycles: (.+)', log_text)
    summary = {'exit_code': exit_code, 'timed_out': timed_out,
               'completed_requested_frames': False}
    if stops:
        frames, budget, reason = stops[-1]
        summary.update(reported_frames=int(frames), reported_budget=int(budget), reason=reason.strip())
        summary['completed_requested_frames'] = (
            not timed_out and exit_code == 0 and int(frames) == frame_count
            and reason.strip() == 'frame limit reached')
    return summary


def prepare_vu1_corpus(run_directory):
    """Return the session's VU1 corpus path; the game creates it on the first miss."""
    corpus = run_directory / VU1_CORPUS_NAME
    if corpus.exists():
        raise ValueError('VU1 corpus directory already exists; select a fresh session')
    return corpus


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(65536), b''):
            digest.update(chunk)
    return digest.hexdigest()


def summarize_vu1_corpus(corpus):
    """Describe recorded VU1 programs; any entry that breaks the contract is listed in rejected."""
    summary = {'schema_version': 1, 'directory': VU1_CORPUS_NAME,
               'expected_program_bytes': VU1_PROGRAM_BYTES,
               'file_count': 0, 'total_bytes': 0, 'files': [], 'rejected': []}
    if not corpus.is_dir():
        return summary
    for path in sorted(corpus.iterdir()):
        size = path.lstat().st_size
        record = {'name': path.name, 'size_bytes': size}
        reasons = []
        if path.is_symlink() or not path.is_file():
            reasons.append('not_regular_file')
        else:
            record['sha256'] = sha256_file(path)
        match = VU1_NAME_PATTERN.fullmatch(path.name)
        if match:
            record.update(key=match.group(1), entry=match.group(2))
        else:
            reasons.append('unexpected_name')
        if size != VU1_PROGRAM_BYTES:
            reasons.append('wrong_size')
        summary['file_count'] += 1
        summary['total_bytes'] += size
        summary['files'].append(record)
        if reasons:
            summary['rejected'].append({'name': path.name, 'reasons': reasons})
    return summary


# The owner's persistent card. --play-card selects another private card and may never name this one.
OWNER_PLAY_CARD = 'private/manual-play/card.ps2'


def separate_play_card(workspace, path):
    """Validates a --play-card path: an existing file inside private/, never the owner's card. Returns it resolved.

    Relative paths are taken from the assets checkout. The card is never created here: it must already exist, and
    the usual backup (card-before.ps2 in the session folder) and session lock apply to it.
    """
    workspace = Path(workspace).resolve()
    candidate = Path(path)
    if not candidate.is_absolute():
        candidate = workspace / candidate
    resolved = candidate.resolve()
    private = (workspace / 'private').resolve()
    if not resolved.is_relative_to(private) or resolved == private:
        raise ValueError(f'--play-card must be a card file inside private/: {path}')
    if resolved == (workspace / OWNER_PLAY_CARD).resolve():
        raise ValueError(f"--play-card must not be the owner's card {OWNER_PLAY_CARD}")
    if not resolved.is_file():
        raise ValueError(f'--play-card must name an existing card file (it is never created): {path}')
    return resolved


def resolve_controls_file(controls_file):
    """Returns the absolute controls path, or None for the built-in bindings.

    Checks the TOML shape the launcher can see: schema 1, [bindings] only, known input names,
    and string sources with a known kind. The C++ reader (native/controls.cpp) applies the full
    rules at start; this check refuses the obvious mistakes before any run output exists.
    """
    if controls_file is None:
        return None
    path = Path(controls_file).resolve()
    if not path.is_file():
        raise ValueError(f'Controls file not found: {path}')
    try:
        document = tomllib.loads(path.read_bytes().decode('utf-8'))
    except (UnicodeDecodeError, tomllib.TOMLDecodeError) as error:
        raise ValueError(f'Controls file is not valid TOML: {path}: {error}') from error
    unknown_tables = sorted(set(document) - {'schema', 'bindings'})
    if unknown_tables:
        raise ValueError(f'Controls file has unknown keys: {unknown_tables}')
    if type(document.get('schema')) is not int or document['schema'] != CONTROLS_SCHEMA:
        raise ValueError(f'Controls file needs schema = {CONTROLS_SCHEMA}')
    bindings = document.get('bindings', {})
    if not isinstance(bindings, dict):
        raise ValueError('Controls file: [bindings] must be a table')
    for name, sources in bindings.items():
        if name not in CONTROLS_INPUTS:
            raise ValueError(f'Controls file: unknown input {name!r}')
        items = [sources] if isinstance(sources, str) else sources
        if not isinstance(items, list) or not all(isinstance(item, str) for item in items):
            raise ValueError(f'Controls file: {name} must be a string or a list of strings')
        for item in items:
            kind = item.split('|', 1)[0].partition(':')[0].strip().lower()
            if kind not in CONTROLS_SOURCE_KINDS:
                raise ValueError(f'Controls file: {name} has an unknown source {item!r}')
    return path


def controls_evidence(path):
    """Run evidence for the bindings in force: the file hash when a file was used."""
    if path is None:
        return {'source': 'default'}
    data = path.read_bytes()
    return {'source': 'file', 'path': str(path), 'size_bytes': len(data),
            'sha256': hashlib.sha256(data).hexdigest()}


def apply_controls_environment(environment, path):
    """Sets GT4_CONTROLS_FILE for the game only; an inherited value never selects the bindings."""
    environment.pop('GT4_CONTROLS_FILE', None)
    if path is not None:
        environment['GT4_CONTROLS_FILE'] = str(path)

def binary_workspace_for(runtime_workspace, workspace):
    """Checkout holding the pinned binaries: the runtime checkout, or the assets checkout when the runtime
    checkout is a worktree without private/ (worktrees never link the main private/ directory)."""
    runtime_workspace = Path(runtime_workspace).resolve()
    return runtime_workspace if (runtime_workspace / 'private').is_dir() else Path(workspace).resolve()


def run_probe(workspace, run_directory, frame_count, timeout_seconds, presses, card_seed=None,
              runtime_manifest=None, window=False, capture_audio=False, interactive=False,
              play_card=None, runtime_workspace=None, capture_shapes=False,
              performance=False, vu1_record=False, vu1_verify=0, vu1_interpret_only=False, force_480p=False, vu0_verify=0,
              binary_workspace=None, cpu_cores=None, controls_file=None, renderer_override=None, separate_card=False):
    workspace = workspace.resolve()
    if cpu_cores is not None and not performance:
        raise ValueError('--cpu-cores is refused outside performance modes')
    affinity_plan = plan_affinity(cpu_cores) if cpu_cores is not None else None
    if performance and (not interactive or capture_shapes):
        raise ValueError('Performance play must be owner-controlled without shape capture')
    runtime_workspace = Path(runtime_workspace or workspace).resolve()
    # Manifests come from runtime_workspace/docs/inputs; pinned binaries from binary_workspace/private.
    binary_workspace = Path(binary_workspace or runtime_workspace).resolve()
    runtime_private = (binary_workspace / 'private').resolve()
    if capture_shapes and run_directory.absolute() != run_directory.resolve():
        raise ValueError('Shape capture session must use an ordinary canonical private path')
    run_directory = run_directory.resolve()
    private_root = workspace / 'private'
    # Refused before any output: the window is fixed 16:9, and unknown GT4_FORCE_480P values stop the run.
    if os.environ.get('GT4_FORCE_480P', '0') not in FORCE_480P_VALUES:
        raise ValueError('GT4_FORCE_480P must be 0 or 1')
    if os.environ.get('GT4_FORCE_WIDESCREEN', FORCE_WIDESCREEN_DEFAULT) not in FORCE_WIDESCREEN_VALUES:
        raise ValueError('GT4_FORCE_WIDESCREEN must be 0 or 1')
    display_aspect_ratio(os.environ.get('GT4_DISPLAY_ASPECT'))
    aspect_fov_extra(os.environ.get('GT4_ASPECT_FOV_EXTRA'))
    output_size(os.environ.get('GT4_OUTPUT_SIZE'))
    monitor_device(os.environ.get('GT4_MONITOR'))
    if os.environ.get('GT4_CURSOR_AUTOHIDE', '1') not in CURSOR_AUTOHIDE_VALUES:
        raise ValueError('GT4_CURSOR_AUTOHIDE must be 0 or 1')
    if os.environ.get('GT4_MTVU_MEASURE', '1') not in MTVU_MEASURE_VALUES:
        raise ValueError('GT4_MTVU_MEASURE must be 0 or 1')
    if not os.environ.get('GT4_MTVU_MEASURE_EVERY', '0').isdigit():
        raise ValueError('GT4_MTVU_MEASURE_EVERY must be a VBlank count')
    # Worker switches are validated here, before any output, and never scrubbed: a bad value stops the run.
    for switch, default in MTVU_SWITCHES.items():
        if os.environ.get(switch, default) not in ('0', '1'):
            raise ValueError(f'{switch} must be 0 or 1')
    if not run_directory.is_relative_to(private_root) or run_directory == private_root:
        raise ValueError('Probe output must be a new directory inside private/')
    if run_directory.exists():
        raise ValueError('Probe output already exists; preserve it and select a new directory')
    if (frame_count <= 0 and not (interactive and frame_count == 0)) or timeout_seconds <= 0:
        raise ValueError('Frames and timeout must be positive')
    if interactive and (not window or presses or frame_count != 0):
        raise ValueError('Interactive play requires a window, zero frame limit and no scripted presses')
    if capture_shapes and not interactive:
        raise ValueError('Shape capture requires owner-controlled interactive play')
    if play_card is not None:
        play_card = play_card.resolve()
        if not interactive or not play_card.is_relative_to(private_root):
            raise ValueError('Persistent play card requires interactive mode and a private path')
        if play_card.exists() and play_card.stat().st_size != 8650752:
            raise ValueError('Persistent card has an unexpected size; preserve it for inspection')
        if not play_card.exists() and card_seed is None:
            raise ValueError('First manual play needs a pinned seed card')
        if separate_card:
            play_card = separate_play_card(workspace, play_card)

    # Refused before any output: a bad controls file must stop the start, not the game.
    controls_path = resolve_controls_file(controls_file)
    inputs = workspace / 'docs/inputs'
    native = json.loads((inputs / 'usa-v2.00-native.json').read_text(encoding='utf-8'))
    disc_manifest = json.loads((inputs / 'usa-v2.00.json').read_text(encoding='utf-8'))
    if runtime_manifest is None:
        runtime_manifest = runtime_workspace / 'docs/inputs/slice114-native-base.json'
    runtime_manifest = runtime_manifest.resolve()
    if not runtime_manifest.is_relative_to(runtime_workspace / 'docs/inputs'):
        raise ValueError('Runtime manifest must be inside docs/inputs')
    runtime_evidence = json.loads(runtime_manifest.read_text(encoding='utf-8'))
    if capture_shapes:
        verify_manual_capture_profile(runtime_manifest, runtime_evidence, runtime_workspace)
    if performance:
        verify_performance_profile(runtime_manifest, runtime_evidence, runtime_workspace)
    is_vu1_manifest = runtime_manifest == (runtime_workspace / 'docs/inputs' / VU1_RECORD_MANIFEST).resolve()
    if vu1_record and (not performance or not is_vu1_manifest):
        raise ValueError('VU1 recording requires the qualified VU1 record manifest')
    if is_vu1_manifest and not vu1_record:
        raise ValueError('The VU1 record manifest is only valid with VU1 recording')
    is_aot_manifest = runtime_manifest in {(runtime_workspace / 'docs/inputs' / name).resolve() for name in VU1_AOT_MANIFESTS}
    if (vu1_verify or vu1_interpret_only) and (not performance or not is_aot_manifest):
        raise ValueError('VU1 AOT verification and interpret-only require the qualified VU1 AOT manifest')
    if vu1_verify < 0 or (vu1_verify and vu1_interpret_only):
        raise ValueError('VU1 AOT verification needs a positive sample period and the AOT path (not interpret-only)')
    is_vu0_manifest = runtime_manifest in {(runtime_workspace / 'docs/inputs' / name).resolve() for name in VU0_AOT_MANIFESTS}
    if vu0_verify < 0 or (vu0_verify and not (performance and is_vu0_manifest)):
        raise ValueError('VU0 AOT verification needs a positive sample period and the vu0-aot manifest')
    executable_pin = runtime_evidence['runtime']
    for dependency in runtime_evidence.get('runtime_dependencies', []):
        dependency_path = (binary_workspace / dependency['path']).resolve()
        if not dependency_path.is_relative_to(runtime_private):
            raise ValueError('Runtime dependency must be inside private/')
        verify_file(dependency_path, {key: dependency[key] for key in ('size_bytes', 'sha256')})
    executable = binary_workspace / executable_pin['path']
    if not executable.resolve().is_relative_to(runtime_private):
        raise ValueError('Runtime executable must be inside private/')
    verify_file(executable, {key: executable_pin[key] for key in ('size_bytes', 'sha256')})
    core = workspace / 'private/fingerprint-check/CORE.GT4'
    verify_file(core, disc_manifest['files']['/CORE.GT4;1'])
    source_elf = workspace / 'private/reconstructed/SCUS_973.28.elf'
    verify_file(source_elf, {'size_bytes': native['native_elf_size'], 'sha256': native['native_elf_sha256']})
    disc = workspace / 'Gran Turismo 4 (USA) (v2.00).iso'
    verify_file(disc, disc_manifest['disc'])

    seed_pin = None
    if card_seed is not None:
        card_seed = card_seed.resolve()
        if not card_seed.is_relative_to(private_root):
            raise ValueError('Card seed must be inside private/')
        evidence = json.loads((inputs / 'slice110-external-menu-pilot.json').read_text(encoding='utf-8'))
        relative_seed = card_seed.relative_to(workspace).as_posix()
        qualified_seeds = evidence['artifacts'] + runtime_evidence.get('card_seeds', [])
        seed_pin = next((item for item in qualified_seeds if item['path'] == relative_seed), None)
        if seed_pin is None:
            raise ValueError(f'Card seed has no pinned artifact: {card_seed}')
        verify_file(card_seed, {key: seed_pin[key] for key in ('size_bytes', 'sha256')})
    if play_card is not None:
        evidence = json.loads((inputs / 'slice110-external-menu-pilot.json').read_text(encoding='utf-8'))
        protected = {item['path'] for item in evidence['artifacts'] + runtime_evidence.get('card_seeds', [])}
        if play_card.relative_to(workspace).as_posix() in protected:
            raise ValueError('Persistent play must never overwrite a pinned evidence artifact')

    # Keep argv0 equal to the actual disc boot name without modifying the ELF.
    run_directory.mkdir(parents=True)
    guest = run_directory / 'SCUS_973.28'
    shutil.copyfile(source_elf, guest)
    verify_file(guest, {'size_bytes': native['native_elf_size'], 'sha256': native['native_elf_sha256']})
    if card_seed is not None:
        card = run_directory / 'card.ps2'
        shutil.copyfile(card_seed, card)
        verify_file(card, {key: seed_pin[key] for key in ('size_bytes', 'sha256')})
    if play_card is not None:
        if not play_card.exists():
            play_card.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(run_directory / 'card.ps2', play_card)
            verify_file(play_card, {key: seed_pin[key] for key in ('size_bytes', 'sha256')})
        shutil.copyfile(play_card, run_directory / 'card-before.ps2')
    frames = run_directory / 'frames'
    if not interactive:
        frames.mkdir()
    arguments = [str(executable), str(guest), '--ntsc', '--disc', str(disc),
                 '--frames', str(frame_count), '--memcard', str(play_card or run_directory / 'card.ps2'),
                 '--misses', str(run_directory / 'misses.txt')]
    if not interactive:
        arguments.extend(['--frames-dir', str(frames), '--frame-every', '100', '--trace-syscalls'])
    for press in presses:
        arguments.extend(['--press', press])
    if window:
        arguments.append('--window')
    result = {'arguments': arguments, 'timeout_seconds': timeout_seconds,
              'presentation_aspect': '16:9',
              'disc_sha256': disc_manifest['disc']['sha256'],
              'runtime_sha256': executable_pin['sha256']}
    result['controls'] = controls_evidence(controls_path)
    if seed_pin is not None:
        result['card_seed'] = seed_pin
    if play_card is not None:
        with (run_directory / 'card-before.ps2').open('rb') as source:
            result['play_card_before'] = fingerprint(source)
        result['play_card_path'] = str(play_card)
        if separate_card:
            # --play-card: a separate test card, not the owner's private/manual-play/card.ps2.
            result['play_card_option'] = str(play_card)
    start = time.monotonic()
    log_path = run_directory / 'probe.log'
    environment = os.environ.copy()
    environment.pop('PS2_DUMP_AUDIO', None)
    # Aspect selection was removed in slice156; a stale inherited value must not reach the game.
    environment.pop('GT4_ASPECT', None)
    environment.pop('GT4_OBSERVATION_OUTPUT', None)
    apply_controls_environment(environment, controls_path)
    environment.pop('GT4_PERF', None)
    if performance:
        scrub_performance_environment(environment)
        environment.update(GT4_PERF='1', PS2_SCALE='1', PS2_GS='gpu')
        result['performance_environment'] = {'GT4_PERF': '1', 'PS2_SCALE': '1', 'PS2_GS': 'gpu'}
        result['measurement_mode'] = runtime_evidence.get('measurement_mode', 'detailed')
        if runtime_evidence.get('measurement_mode') == 'license-halt':
            # TASK-017 (slice204): the halt dump and the FSV read trace are both on for this diagnostic mode.
            environment.update(GT4_FSV_TRACE='1', GT4_HALT_DUMP='1')
            result['halt_dump_environment'] = {'GT4_FSV_TRACE': '1', 'GT4_HALT_DUMP': '1'}
        result['vulkan_loader_environment'] = {
            name: value for name, value in environment.items() if name.upper().startswith('VK_')}
    if 'GT4_GS_SYNC' in environment:
        result['gs_sync_environment'] = {'GT4_GS_SYNC': environment['GT4_GS_SYNC']}
    result.update(pacing_overrides(environment))
    if 'GT4_CURSOR_AUTOHIDE' in environment:
        if environment['GT4_CURSOR_AUTOHIDE'] not in CURSOR_AUTOHIDE_VALUES:
            raise ValueError('GT4_CURSOR_AUTOHIDE must be 0 or 1')
        result['cursor_environment'] = {'GT4_CURSOR_AUTOHIDE': environment['GT4_CURSOR_AUTOHIDE']}
    result.update(gs_stream_environment(environment, runtime_evidence, binary_workspace, performance,
                                        run_directory=run_directory, headless=not window,
                                        renderer_override=renderer_override))
    if renderer_override is not None:
        result['gs_renderer_override'] = renderer_override
    if force_480p:
        environment['GT4_FORCE_480P'] = '1'
    if 'GT4_FORCE_480P' in environment:
        result['force_480p_environment'] = {'GT4_FORCE_480P': environment['GT4_FORCE_480P']}
    # TASK-079: the game default is on; the explicit value is written to the environment so the evidence records it.
    environment.setdefault('GT4_FORCE_WIDESCREEN', FORCE_WIDESCREEN_DEFAULT)
    if environment['GT4_FORCE_WIDESCREEN'] not in FORCE_WIDESCREEN_VALUES:
        raise ValueError('GT4_FORCE_WIDESCREEN must be 0 or 1')
    result['force_widescreen_environment'] = {'GT4_FORCE_WIDESCREEN': environment['GT4_FORCE_WIDESCREEN']}
    # TASK-080: unset means auto and 1.0; set values are checked again (the caller may pass its own environment).
    aspect = {name: environment[name] for name in ('GT4_DISPLAY_ASPECT', 'GT4_ASPECT_FOV_EXTRA', 'GT4_OUTPUT_SIZE',
                                                   'GT4_MONITOR') if name in environment}
    display_aspect_ratio(aspect.get('GT4_DISPLAY_ASPECT'))
    aspect_fov_extra(aspect.get('GT4_ASPECT_FOV_EXTRA'))
    output_size(aspect.get('GT4_OUTPUT_SIZE'))
    monitor_device(aspect.get('GT4_MONITOR'))
    result['display_aspect_environment'] = aspect or {'GT4_DISPLAY_ASPECT': 'auto (unset)'}
    if 'GT4_FSV_TRACE' in environment:
        result['fsv_trace_environment'] = {'GT4_FSV_TRACE': environment['GT4_FSV_TRACE']}
    if 'GT4_HALT_DUMP' in environment and 'halt_dump_environment' not in result:
        result['halt_dump_environment'] = {'GT4_HALT_DUMP': environment['GT4_HALT_DUMP']}
    mtvu_measure = {name: environment[name] for name in ('GT4_MTVU_MEASURE', 'GT4_MTVU_MEASURE_EVERY') if name in environment}
    if mtvu_measure:
        result['mtvu_measure_environment'] = mtvu_measure
    if MTVU_REPORT_EVERY in environment and not environment[MTVU_REPORT_EVERY].isdigit():
        raise ValueError('GT4_MTVU_REPORT_EVERY must be a VBlank count')
    mtvu_switches = {name: environment[name] for name in (*MTVU_SWITCHES, MTVU_REPORT_EVERY) if name in environment}
    if mtvu_switches:
        result['mtvu_environment'] = mtvu_switches
    if capture_shapes:
        environment['GT4_OBSERVATION_OUTPUT'] = str(run_directory / 'shape-capture.json')
        result['shape_capture_path'] = environment['GT4_OBSERVATION_OUTPUT']
    if capture_audio:
        environment['PS2_DUMP_AUDIO'] = str(run_directory / 'audio.wav')
    corpus = None
    if vu1_record:
        corpus = prepare_vu1_corpus(run_directory)
        environment.update(PS2_VU_RECORD=str(corpus), PS2_VU_COUNT='1')
        result['vu1_record_environment'] = {'PS2_VU_RECORD': str(corpus), 'PS2_VU_COUNT': '1'}
    if vu1_verify:
        environment['GT4_VU1_VERIFY'] = str(vu1_verify)
        result['vu1_verify_environment'] = {'GT4_VU1_VERIFY': str(vu1_verify)}
    if vu0_verify:
        environment['GT4_VU0_VERIFY'] = str(vu0_verify)
        result['vu0_verify_environment'] = {'GT4_VU0_VERIFY': str(vu0_verify)}
    if vu1_interpret_only:
        environment['PS2_VU_INTERPRET'] = '1'
        result['vu1_interpret_environment'] = {'PS2_VU_INTERPRET': '1'}
    suppress_loader_dialogs()
    with log_path.open('wb') as output:
        try:
            if affinity_plan is None:
                process = subprocess.run(arguments, cwd=run_directory, stdout=output,
                                         stderr=subprocess.STDOUT, timeout=timeout_seconds, env=environment)
                exit_code, timed_out = process.returncode, False
            else:
                # Affinity: start, then set the mask on the running process (see scripts/cpu_affinity.py).
                process = subprocess.Popen(arguments, cwd=run_directory, stdout=output,
                                           stderr=subprocess.STDOUT, env=environment)
                try:
                    apply_process_affinity(process.pid, affinity_plan['mask'])
                except OSError:
                    process.kill()
                    process.wait()
                    raise
                result['cpu_affinity'] = dict(affinity_plan, applied=True, mask=hex(affinity_plan['mask']),
                                              method='SetProcessAffinityMask after process start')
                try:
                    exit_code, timed_out = process.wait(timeout=timeout_seconds), False
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                    raise
        except subprocess.TimeoutExpired:
            exit_code, timed_out = None, True
    log_text = log_path.read_text(encoding='utf-8', errors='replace')
    result.update(summarize_exit(log_text, frame_count, exit_code, timed_out))
    # TASK-038: GS process failures and the player's restart choice (message box) are recorded for the launcher.
    if '[gs-ipc] GS renderer stopped' in log_text:
        result['gs_process_stopped'] = True
    if GS_RESTART_MARKER in log_text:
        result['gs_restart_requested'] = 'donor'
    if corpus is not None:
        corpus_summary = summarize_vu1_corpus(corpus)
        (run_directory / VU1_CORPUS_SUMMARY).write_text(json.dumps(corpus_summary, indent=2) + '\n', encoding='utf-8')
        result['vu1_corpus'] = {'summary': VU1_CORPUS_SUMMARY, 'file_count': corpus_summary['file_count'],
                                'total_bytes': corpus_summary['total_bytes'],
                                'rejected_count': len(corpus_summary['rejected'])}
    result['elapsed_seconds'] = time.monotonic() - start
    if interactive:
        result['manual_session_closed'] = (not timed_out and exit_code == 0
                                           and result.get('reason') == 'window closed')
    if play_card is not None:
        with play_card.open('rb') as source:
            result['play_card_after'] = fingerprint(source)
    if capture_shapes:
        capture = run_directory / 'shape-capture.json'
        result['shape_capture_created'] = capture.exists()
        if capture.exists():
            if capture.is_symlink() or capture.is_junction() or capture.stat().st_size > 256 * 1024:
                raise ValueError('Shape capture output exceeds the ordinary bounded-file contract')
            with capture.open('rb') as source:
                result['shape_capture_file'] = fingerprint(source)
    result['frames'] = sorted(path.name for path in frames.glob('*.ppm'))
    (run_directory / 'result.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run_directory', type=Path)
    parser.add_argument('--frames', type=int, default=2000)
    parser.add_argument('--timeout', type=int, default=110)
    parser.add_argument('--press', action='append', default=[])
    parser.add_argument('--card-seed', type=Path, help='Copy a private card pinned in slice110 evidence')
    parser.add_argument('--runtime-manifest', type=Path, help='Select a separately pinned experimental runtime')
    parser.add_argument('--window', action='store_true', help='Present real guest output in a native window')
    parser.add_argument('--capture-audio', action='store_true', help='Record guest PCM in private audio.wav')
    parser.add_argument('--interactive', action='store_true', help='Manual window play without scripted input or captures')
    parser.add_argument('--controls', type=Path, default=None,
                        help='Controls TOML file (docs/CONTROLS.md); default: built-in bindings')
    parser.add_argument('--force-480p', action='store_true',
                        help='Experimental: hold the race display index at 480p (GT4_FORCE_480P=1)')
    parser.add_argument('--assets-root', type=Path,
                        help='Checkout with private/ and the disc (default: this checkout); outputs go under it')
    parser.add_argument('--play-card', type=Path,
                        help='Interactive only: play on this existing card inside private/ (never the owner card '
                             'private/manual-play/card.ps2); backed up like the owner card and recorded in result.json')
    options = parser.parse_args()
    try:
        assets = (options.assets_root or ROOT).resolve()
        play_card = separate_play_card(assets, options.play_card) if options.play_card else None
        result = run_probe(assets, options.run_directory, options.frames, options.timeout,
                           options.press, options.card_seed, options.runtime_manifest, options.window,
                           options.capture_audio, options.interactive, play_card=play_card,
                           force_480p=options.force_480p, controls_file=options.controls,
                           runtime_workspace=ROOT,
                           binary_workspace=binary_workspace_for(ROOT, assets), separate_card=play_card is not None)
    except (OSError, ValueError, KeyError, StopIteration) as error:
        parser.exit(1, f'FAIL: {error}\n')
    print(json.dumps(result, indent=2))
    return 0 if result['completed_requested_frames'] or result.get('manual_session_closed') else 1


if __name__ == '__main__':
    raise SystemExit(main())

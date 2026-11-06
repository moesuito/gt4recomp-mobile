"""Open the corrected performance candidate with protected owner-controlled saves.

VU1 AOT candidate (--mode vu1-aot):
  --vu1-verify N         compare every Nth generated VU1 call with the interpreter; mismatches
                         are counted and printed to the session log.
  --vu1-interpret-only   run the same vu1-aot binary with the interpreter only (A/B).

Asynchronous GS candidate (--mode gs-async): VU1 AOT plus asynchronous GS submission. The
same verify and interpreter options apply. Set GT4_GS_SYNC=1 in the environment before
launching to restore the synchronous GS path for A/B comparison.

Pacing candidate (--mode pacing): gs-async plus the paced guest clock (60 Hz wall grid,
non-blocking audio with rate control). Set GT4_PACING=audio to restore the blocking audio
clock for A/B, and GT4_PRESENT_MODE=fifo|mailbox|immediate to test presentation modes
(default fifo). Both variables are recorded in the run evidence.

VU1 flag candidate (--mode vu1-flags): pacing with the slice154 VU1 table, which computes
MAC and clip only where a later reader or the program exit can observe them. Same options,
same pacing environment variables; the VU1 status word is not kept (no image instruction reads it).

VU1 full-entry candidate (--mode vu1-full): vu1-flags with the slice155 static VU1 table (426
entries generated from the pinned image, not from recordings). Same options and environment.

Quick-fix candidate (--mode quickfix): vu1-full with the 16:9 presentation fixed, borderless
fullscreen on Alt+Enter or F11, per-interval pacing counters in the F8 report, and the
experimental 480p request rewrite. --force-480p (GT4_FORCE_480P=1) enables the rewrite;
without it the guest request is passed through unchanged.

VU1 all-entries candidate (--mode vu1-all): vu1-flags with the slice157 static VU1 table, every
8-byte address of the pinned image (2,048 programs, built as 128 parts). Same options and environment.

Quick-fix 2 candidate (--mode quickfix2): vu1-all with the quick fixes of slice156 (16:9, fullscreen,
interval counters, late_frames with a 1 ms tolerance) and the slice158 CRT table rewrite. Kept as history:
its build no longer compiles from this tree (native/display_table.* was removed in slice164).

Current main (--mode main, slice178): everything merged to main (all VU1 and VU0 entries compiled, quick
fixes) plus the MTVU VU1 worker runtime of slice172 (docs/MTVU-PLAN.md section 12). GT4_MTVU=1 is the default
worker; GT4_MTVU=0 is the single-thread A/B path. The race display is held at 480p progressive by default
(release 1.0.0 owner decision); the hidden opt-out is GT4_FORCE_480P=0. Accepts --force-480p, --cpu-cores N,
--vu0-verify N and the VU1 options. This is the build to play.

MTVU measurement (--mode mtvu-measure, slice169, TASK-011 step 0): main plus counters for the VU1 worker
plan (docs/MTVU-PLAN.md). Each F8 report adds [mtvu-measure] lines: GIF packets per path with their
SIGNAL/FINISH/LABEL/TRXDIR writes, VIF1 register access, EE access to VU1 memory, VU1 time split into
program and PATH1 GS front end, and a worker overlap model. GT4_MTVU_MEASURE=0 disables the counters.
Same options as main.

Quick-fix 3 candidate (--mode quickfix3, slice164): vu1-all with the quick fixes. --force-480p (GT4_FORCE_480P=1,
default off) holds the game's race display option at 480p progressive: at each guest VBlank, when the option
index is 0 (normal) it is written 1 (480p). Indices 1 and 2 are never changed.
GS stream candidate (--mode pcsx2-stream, slice191, TASK-035): main plus the GS stream seam. The launcher runs
GT4_RENDERER=pcsx2 with the pinned test consumer tools/gs_stream_record as the GS process: the game sends GIF,
privileged writes, VSYNC and readbacks over shared memory (docs/GS-STREAM-PROTOCOL.md) and creates no Vulkan
device, so the window stays black. For TASK-036 development only, not for owner play; F8 adds a [gs-ipc] section.
Live PCSX2 GS (--mode pcsx2, slice193, TASK-036): the GS stream build with the pinned gt4-gs.exe package (GPL fork
moesuito/gt4recomp-gs, private/slice193-gt4gs-pkg). The PCSX2 GS renders into the game window (Vulkan); pacing,
input, audio, F7/F8 and the memory card stay in the game. --gs-scale N (1..12, default 3) sets the internal
resolution; GT4_PRESENT_MODE sets the GS vsync. --gs-setting ID=VALUE (repeatable) sets any other GS option from
docs/launcher/gs-settings.toml and is checked against that table (docs/GS-SETTINGS.md). F8 adds a [gs-ipc] section
with the GS process STATS.
VU0 candidate (--mode vu0-aot, slice 160): vu1-all plus the VU0 AOT overlay, which runs the 14 VCALLMS
entries of the pinned VU0 image compiled. --vu0-verify N compares every Nth generated VU0 call with the
exact interpreter path (GT4_VU0_VERIFY); the VU1 options apply as in vu1-all.

License trace (--mode license-trace, slice181, TASK-017): the slice166 profile with the FSV read trace.
Set GT4_FSV_TRACE=1 before launching; each guest disc read is logged to stderr as "[fsv] read ...".
Diagnostic only: the run is not a performance measurement. Compare the log with the disc using
scripts/compare_fsv_trace.py <log> <iso>.

License halt dump (--mode license-halt, slice204, TASK-017): the slice181 profile with the halt forensic dump.
The mode sets GT4_HALT_DUMP=1 and GT4_FSV_TRACE=1 for the game; the [halt-dump] lines are written at the halt.
Diagnostic only: the run is not a performance measurement.

TASK-080 addendum (slice218): --mode pcsx2 runs the slice218 build: slice217 plus GT4_OUTPUT_SIZE=WxH (the aspect
follows the output size when GT4_DISPLAY_ASPECT is auto) and GT4_MONITOR (the monitor's GDI device name, e.g. DISPLAY2 with its device prefix),
as the launcher sends them; both unset = the window's client area on the primary monitor. slice217 stays as
--mode pcsx2-slice217.
TASK-080 P2 (slice217): --mode pcsx2 ran the slice217 build: slice215 plus the race HUD kept as an undistorted, centred
16:9 layout at other ratios (HUD canvas hook, HUD parameter block, EE library profile "aspect"). slice215 stays as
--mode pcsx2-slice215.
TASK-080 (slice215): --mode pcsx2 ran the slice215 build: the slice214 release build plus native display aspect ratios.
GT4_DISPLAY_ASPECT (unset or auto = the window's client area; 16:9, 4:3, W:H or a decimal) widens the race camera for
the display ratio and fills the screen in races; menus stay 16:9. GT4_ASPECT_FOV_EXTRA (default 1.0) is the hidden FOV
comparison factor. The slice214 build stays as --mode pcsx2-slice214 and slice212b as --mode pcsx2-slice212b.
TASK-081 round 2 (slice214): --mode pcsx2 ran the slice214 release build (the slice213 build plus the Vulkan
baseline path for older drivers; GT4_VULKAN_FORCE_BASELINE=1 forces it). The slice213 build stays as
--mode pcsx2-slice213.
TASK-081 (slice213): --mode pcsx2 ran the slice213 release build (the slice211 recipe with no build-time shader folder
in the executable; shaders come from shaders next to it, which the build tree has). The slice211 build stays as
--mode pcsx2-slice211.
TASK-079 (slice211): --mode pcsx2 ran the slice211 build: GT4_FORCE_WIDESCREEN is on by default (the game wide flag is held
at 16:9); GT4_FORCE_WIDESCREEN=0 leaves the game's own Wide setting. The slice210 build stays as --mode pcsx2-slice210.
TASK-075/076 (slice210): --mode pcsx2 ran the slice210 build (SDL3 gamepad input, GT4_INPUT=sdl by default; the TASK-076
keyboard table). The slice209 build stays as --mode pcsx2-slice209.
TASK-019 launcher test: --mode pcsx2 ran the slice209 build (slice208 coverage EE library plus the GT4_FULLSCREEN start;
--mode pcsx2-slice206 keeps the slice206 build). License guard (TASK-017): the slice206 build has the license
demo guard on both demo loaders (--mode pcsx2-slice203 keeps the build without it). --mode license-guard is the slice205
test build (guard v1, first loader only), kept as history. A license demo that decrypts wrong (fails the game's CRC and is not the expected layout)
gets no demo inputs instead of crashing: the license test stays playable, its demo or background replay may not
play (as in PCSX2). One [license-guard] line per demo load in probe.log. Same options as --mode pcsx2. The halt dump
and the FSV read trace are in this build but off; set GT4_HALT_DUMP=1 or GT4_FSV_TRACE=1 before launching to turn
them on.
"""
import argparse
import os
import sys
from pathlib import Path
from play_external_runtime import ROOT, play
from external_runtime_probe import GS_SETTINGS_ENV, PERF_PROFILES, VU1_RECORD_MANIFEST
import gs_settings

# Modes that run the live PCSX2 GS package and accept the GS options (TASK-017 license-guard is pcsx2 plus the guard).
PCSX2_MODES = ('pcsx2', 'pcsx2-slice268', 'license-guard', 'pcsx2-coverage')

MODES = tuple(PERF_PROFILES) + ('vu1-record',)
# TASK-060/061/062 (owner decision 2026-10-16): the internal-resolution presets of the settings table (upscale row),
# as multipliers; --gs-resolution X is the same as --gs-scale N.
GS_RESOLUTIONS = {name: multiplier for name, multiplier, _ in gs_settings.upscale_presets(gs_settings.load_table())}


def controls_options(args):
    """--controls as a play() option; absent when not requested (built-in bindings)."""
    return {} if args.controls is None else {'controls_file': args.controls}


def cpu_options(args):
    """--cpu-cores as a play() option; absent when not requested."""
    return {} if args.cpu_cores is None else {'cpu_cores': args.cpu_cores}


def force_480p_requested(args):
    """--force-480p, or the default of --mode main and --mode pcsx2 (480p progressive race display).

    Release 1.0.0 owner decision: --mode main holds 480p unless the hidden opt-out GT4_FORCE_480P=0 is set.
    The owner extended the default to --mode pcsx2 (2026-10-16, TASK-036 windowed test).
    Only the exact value 0 opts out; other values are passed on and refused by the probe.
    """
    if args.force_480p:
        return True
    return args.mode in ('main',) + PCSX2_MODES and os.environ.get('GT4_FORCE_480P') != '0'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--assets-root', type=Path, required=True)
    parser.add_argument('--mode', choices=MODES, default='detailed')
    parser.add_argument('--vu1-verify', type=int, default=0, metavar='N',
                        help='sample every Nth generated VU1 call and compare (vu1-aot, gs-async, pacing and vu1-flags)')
    parser.add_argument('--vu1-interpret-only', action='store_true',
                        help='force the interpreter in the vu1-aot, gs-async, pacing and vu1-flags binaries')
    parser.add_argument('--force-480p', action='store_true',
                        help='hold the race display option at 480p (quickfix3, main, mtvu, pcsx2; main and pcsx2 have it by default)')
    parser.add_argument('--cpu-cores', type=int, default=None, metavar='N',
                        help='weak-CPU simulation: limit the game to N logical CPUs (1..logical count), '
                             'one per physical core first; recorded in result.json (docs/MTVU-PLAN.md section 12)')
    parser.add_argument('--gs-scale', type=int, default=None, metavar='N',
                        help='pcsx2 only: GS internal resolution multiplier 1..12 (default 3; GT4_GS_SCALE)')
    parser.add_argument('--gs-resolution', choices=GS_RESOLUTIONS, default=None,
                        help='pcsx2 only: internal resolution preset (480p = 1x, 720p = 2x, 1080p = 3x, 1440p = 4x, '
                             '4K = 6x, 8K = 12x; the same as --gs-scale N)')
    parser.add_argument('--gs-setting', action='append', default=[], metavar='ID=VALUE',
                        help='pcsx2 only: set a GS option from docs/launcher/gs-settings.toml (repeatable; '
                             'checked against the table; docs/GS-SETTINGS.md)')
    parser.add_argument('--gs-readback', choices=('accurate', 'no-readbacks'), default=None,
                        help='pcsx2 only: GS readback mode, the Performance Mode row (TASK-091). accurate (default); '
                             'no-readbacks: faster, some effects that read the image back are skipped (GT4_GS_READBACK)')
    parser.add_argument('--play-card', type=Path, default=None, metavar='PATH',
                        help='play on this existing card inside private/ instead of the owner card '
                             '(never private/manual-play/card.ps2); backed up and locked the same way, recorded in result.json')
    parser.add_argument('--vu0-verify', type=int, default=0, metavar='N',
                        help='sample every Nth generated VU0 call and compare with the exact interpreter (vu0-aot only)')
    parser.add_argument('--controls', type=Path, default=None,
                        help='controls TOML file (docs/CONTROLS.md); default: built-in bindings')
    args = parser.parse_args()
    try:
        if (args.vu1_verify or args.vu1_interpret_only) and args.mode not in ('vu1-aot', 'gs-async', 'pacing', 'vu1-flags', 'vu1-full', 'quickfix', 'vu1-all', 'quickfix2', 'quickfix3', 'vu0-aot', 'main', 'mtvu-measure', 'mtvu'):
            raise ValueError('--vu1-verify and --vu1-interpret-only require --mode vu1-aot, gs-async, pacing, vu1-flags, vu1-full, quickfix, vu1-all, quickfix2, quickfix3, vu0-aot, main or mtvu-measure')
        if args.vu0_verify and args.mode not in ('vu0-aot', 'main', 'mtvu-measure', 'mtvu'):
            raise ValueError('--vu0-verify requires --mode vu0-aot, main, mtvu-measure or mtvu')
        if args.vu1_verify < 0:
            raise ValueError('--vu1-verify takes a positive N')
        if args.vu1_verify and args.vu1_interpret_only:
            raise ValueError('--vu1-verify compares against the AOT path; it cannot be combined with --vu1-interpret-only')
        if args.force_480p and args.mode not in ('quickfix', 'quickfix2', 'quickfix3', 'main', 'mtvu-measure', 'mtvu') + PCSX2_MODES:
            raise ValueError('--force-480p requires --mode quickfix, quickfix2, quickfix3, main, mtvu-measure, mtvu or pcsx2')
        if args.gs_resolution is not None:
            if args.mode not in PCSX2_MODES:
                raise ValueError('--gs-resolution requires --mode pcsx2')
            if args.gs_scale is not None:
                raise ValueError('--gs-resolution and --gs-scale both set the internal resolution')
            args.gs_scale = GS_RESOLUTIONS[args.gs_resolution]
        if args.gs_scale is not None:
            if args.mode not in PCSX2_MODES or not 1 <= args.gs_scale <= 12:
                raise ValueError('--gs-scale takes 1..12 and requires --mode pcsx2')
            os.environ['GT4_GS_SCALE'] = str(args.gs_scale)
        os.environ.pop(GS_SETTINGS_ENV, None)
        if args.gs_setting:
            if args.mode not in PCSX2_MODES:
                raise ValueError('--gs-setting requires --mode pcsx2')
            settings = gs_settings.settings_from_assignments(args.gs_setting, gs_settings.load_table())
            if args.gs_scale is not None and 'upscale' in settings:
                raise ValueError('--gs-scale (or --gs-resolution) and --gs-setting upscale=... both set the internal '
                                 'resolution')
            if args.gs_readback is not None and 'hw_download_mode' in settings:
                raise ValueError('--gs-readback and --gs-setting hw_download_mode=... both set the readback mode')
            os.environ[GS_SETTINGS_ENV] = gs_settings.settings_to_json(settings)
        if args.gs_readback is not None:
            if args.mode not in PCSX2_MODES:
                raise ValueError('--gs-readback requires --mode pcsx2')
            os.environ['GT4_GS_READBACK'] = args.gs_readback
        if args.play_card is not None and args.mode == 'vu1-record':
            raise ValueError('--play-card is not available with --mode vu1-record')
        if args.mode == 'vu1-record':
            play(args.assets_root, ROOT / 'docs/inputs' / VU1_RECORD_MANIFEST,
                 performance=True, vu1_record=True, **cpu_options(args),
                 **controls_options(args))
        else:
            manifest, _ = PERF_PROFILES[args.mode]
            options = {}
            if args.vu1_verify:
                options['vu1_verify'] = args.vu1_verify
            if args.vu1_interpret_only:
                options['vu1_interpret_only'] = True
            if force_480p_requested(args):
                options['force_480p'] = True
            if args.vu0_verify:
                options['vu0_verify'] = args.vu0_verify
            options.update(cpu_options(args))
            options.update(controls_options(args))
            if args.play_card is not None:
                options['play_card'] = args.play_card
            play(args.assets_root, ROOT / 'docs/inputs' / manifest, performance=True, **options)
        return 0
    except (OSError, ValueError, KeyError, StopIteration) as error:
        print(f'FAIL: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())

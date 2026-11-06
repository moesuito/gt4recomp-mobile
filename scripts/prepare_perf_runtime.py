"""Derive fresh, hash-pinned measurement-only runtime sources; preserve the donor.

Options:
  --split-vu  attribute VU interpreter time per unit (vu_interpreter_category).
  --vu1-aot   also overlay the VU1 AOT registry (scripts/vu1_aot_overlay.inc). Implies
              --split-vu. Default and --split-vu derivations are unchanged by this option.
  --vu0-aot   also overlay the VU0 AOT registry (scripts/vu0_aot_overlay.inc, slice 160). Implies
              --vu1-aot (the VU0 capsule keeps the VU1 table) and --split-vu.
  --mtvu-measure  TASK-011 step 0 (slice 169): also add the counting hooks of native/mtvu_measure.hpp
              to system.cpp and vif.cpp and derive gif.cpp and memory.cpp. Implies --vu0-aot. Counters
              only (GT4_MTVU_MEASURE=0 disables them); no guest-visible behavior change. Derivations
              without this option are unchanged.

  --mtvu      TASK-011 step 2 (slice 170): the VU1 worker runtime (native/mtvu_runtime.hpp). Implies
              --vu0-aot (and so --vu1-aot, --split-vu). Shadows VIF1 on the EE thread, runs the VIF1 parser
              and VU1 on the worker, and drains GS operations in journal order. Derived sources: system.cpp,
              vif.cpp, vu_micro.cpp (patched), and gif.cpp, memory.cpp, hw.cpp, dmac.cpp (new). Excludes
              --mtvu-measure. Derivations without this option are unchanged.

  --gs-stream TASK-035 (slice191): the GS stream seam (native/gs_stream/gs_stream_client.hpp) on top of --mtvu
              (implied). gif.cpp: with GT4_RENDERER=pcsx2 the GIF render point posts the bytes to the GS stream and
              runs the path parser with a signal-only sink (no donor GS front end); a GIF reset is posted.
              hw.cpp: SetGsCrt posts the SMODE1/SMODE2 writes the BIOS performs. Every hook returns at once when
              GT4_RENDERER is unset or donor and GT4_GS_CAPTURE is unset. Derivations without this option are
              unchanged.
  --fsv-trace add the FSV read trace to iop/cdvdfsv.cpp (scripts/fsv_trace_overlay.inc, slice 181).
              Adds cdvdfsv.cpp to the capsule; without GT4_FSV_TRACE set at run time the game behaves as the donor.
  --halt-dump add the halt forensic dump to kernel.cpp (scripts/halt_dump_overlay.inc, slice 204). Adds kernel.cpp
              to the capsule; without GT4_HALT_DUMP set at run time the game behaves as the donor.
  --spec2-iop TASK-100 S7b: add the Spec II IOP reboot rule to iop/sif.cpp (scripts/spec2_iop_overlay.inc,
              native/spec2_iop.hpp). The Spec II disc reboots the IOP with DNAS310.IMG; like IOPRP300.IMG it is a Sony
              IOP image whose 16 system modules the runtime serves as HLE, and it reports LOADFILE 3100, so the
              asynchronous FILEIO server (the one the donor selected for "IOPRP") must be selected there too. Adds
              sif.cpp to the capsule; a game that never asks for DNAS310.IMG behaves as the donor.

Slices 156 and 158 used --gs-crt-override and --display-table-override. Both hooks were removed in slice164
(the 480p switch is a host-side guest RAM write, native/race_display.*). Their capsules stay pinned in
docs/inputs and are not re-derivable from this script.
"""
import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PINS = {
    'system.cpp': 'f30c8dd90c40e930635375ae2a3091f052655d797744de4348a5c7261a710bb2',
    'vu_micro.cpp': '5fc349d96364f72818da96cd2376637421d3d4d9150547c22034863c15d22a1d',
    'vif.cpp': '6fc9efc739daec951172bc7f97f64b05802145314bc17b049c0c3219be654e18',
}
PATCHES = {
    'system.cpp': [
        ('        iop::run(t - g_iop_synced);',
         '        gt4::perf::Scope perf_iop(gt4::perf::Category::iop_run);\n        iop::run(t - g_iop_synced);'),
        ('        dispatch(c, 0, 0);',
         '        gt4::perf::Scope perf_ee(gt4::perf::Category::ee_dispatch);\n        dispatch(c, 0, 0);')],
    'vu_micro.cpp': [
        ('    run_fast(unit, pc);',
         '    {\n        gt4::perf::Scope perf_vu(gt4::perf::Category::vu_interpret);\n        run_fast(unit, pc);\n    }')],
    'vif.cpp': [
        ('void transfer(int unit, const std::uint8_t *data, std::uint32_t qwc) {\n',
         'void transfer(int unit, const std::uint8_t *data, std::uint32_t qwc) {\n    gt4::perf::Scope perf_vif(gt4::perf::Category::vif_gif);\n')],
}
# FSV read trace (slice 181, TASK-017): one donor file, read as bytes (CRLF in the donor, LF in the overlay).
FSV_PIN = {'iop/cdvdfsv.cpp': 'c313e10af0baaf63d9bd31040c168f38936c979c37fdce45afdf060d11560fbe'}
FSV_OVERLAY_PATH = ROOT / 'scripts/fsv_trace_overlay.inc'
FSV_OVERLAY_ANCHORS = {
    'includes': ('after', '#include <cstdio>\n'),
    'local_headers': ('replace', '#include "kernel.hpp"\n#include "sif.hpp"\n'),
    'helpers': ('before', 'void read_to_ee(std::uint32_t sector, std::uint32_t count, std::uint32_t dst) {\n'),
    'read_trace': ('after', '    std::memcpy(ps2::g_ram + at, data.data(), std::min<std::size_t>(data.size(), ps2::kRamSize - at));\n'),
}
# Halt forensic dump (slice 204, TASK-017): one donor file, runtime/src/kernel.cpp, dump() at the halt.
HALT_PIN = {'kernel.cpp': '003c9e0a0e2c1cb61a6fd8ec846a4798c3027ed933b6825c6377112106f0168a'}
HALT_OVERLAY_PATH = ROOT / 'scripts/halt_dump_overlay.inc'
HALT_OVERLAY_ANCHORS = {
    'helpers': ('before', 'void dump() {\n'),
    'dump_call': ('after', '        if (!s.waiters.empty()) std::fprintf(stderr, "[sema] %d count %d waiters %zu\\n", id, s.count, s.waiters.size());\n'),
}
# Spec II IOP reboot rule (TASK-100 S7b): one donor file, runtime/src/iop/sif.cpp. The Spec II disc reboots the IOP
# with DNAS310.IMG instead of IOPRP300.IMG; both are Sony reboot images with the same 16 HLE system modules, and both
# report a 3.x LOADFILE version, but the donor enabled the asynchronous FILEIO server only for "IOPRP". The overlay
# replaces the donor's line with the rule in native/spec2_iop.hpp, and rewrites its local include block to
# runtime/src-relative paths because the derived file is compiled from the capsule directory.
SPEC2_IOP_PIN = {'iop/sif.cpp': '182a24218d719a669b3742867f9901b49d2e1043393f2211e93363dc0180dacc'}
SPEC2_IOP_OVERLAY_PATH = ROOT / 'scripts/spec2_iop_overlay.inc'
SPEC2_IOP_ANCHORS = {
    'includes': ('replace',
                 '#include "sif.hpp"\n\n#include "../hw.hpp"\n#include "iop.hpp"\n#include "io.hpp"\n#include "kernel.hpp"\n'),
    'reboot': ('replace',
               '    if (path.find("IOPRP") != std::string::npos) g_fileio_async = true;\n'
               '    if (const auto space = path.find(\' \'); space != std::string::npos) read_loadfile_version(path.substr(space + 1));\n'),
}
OVERLAY_PATH = ROOT / 'scripts/vu1_aot_overlay.inc'
VU0_OVERLAY_PATH = ROOT / 'scripts/vu0_aot_overlay.inc'
# Each overlay section is inserted at one unique donor anchor, by mode.
OVERLAY_ANCHORS = {
    'includes': ('after', '#include "ps2/vu_ops.hpp"\n'),
    'kick_sink': ('before', 'std::uint32_t g_top = 0, g_itop = 0;\n'),
    'xgkick_hook': ('replace', '    gif::transfer(1, buffer, count);\n'),
    'helpers': ('before', 'void run_entry(int unit, std::uint32_t pc) {\n'),
    'run_gate': ('before', '    if (unit == 1 && !interpret_only) {\n'),
}
# VU0 overlay (slice 160). The run() gate sits before the donor's PS2_VU_EXACT line; the helpers before run().
VU0_OVERLAY_ANCHORS = {
    'includes': ('after', '#include "ps2/vu_ops.hpp"\n'),
    'helpers': ('before', 'void run(int n, std::uint32_t address) {\n'),
    'run_gate': ('before', '    static const bool exact = std::getenv("PS2_VU_EXACT") != nullptr;\n'),
}
PREFIX = '// Measurement-only MIT donor derivation; see native/THIRD_PARTY.md.\n#include "perf_probe.hpp"\n'
# TASK-011 step 0 (slice 169): counting hooks only. Each site is one unique donor text; a hook adds
# lines and never changes donor statements, so removing the hooks restores the input (checked).
MTVU_MEASURE_PREFIX = '#include "mtvu_measure.hpp"\n'
MTVU_MEASURE_PATCHES = {
    'system.cpp': [
        ('void vblank() {\n    ++g_frames;\n',
         'void vblank() {\n    ++g_frames;\n    gt4::mtvu::on_vblank();\n'),
        ('    vif::stats();\n',
         '    vif::stats();\n    gt4::mtvu::final_report(stderr);\n')],
    'vif.cpp': [
        ('    vu::run(unit, address);\n}\n',
         '    {\n        gt4::mtvu::VuCallScope mtvu_call(unit);\n        vu::run(unit, address);\n    }\n}\n'),
        ('void transfer(int unit, const std::uint8_t *data, std::uint32_t qwc) {\n'
         '    gt4::perf::Scope perf_vif(gt4::perf::Category::vif_gif);\n',
         'void transfer(int unit, const std::uint8_t *data, std::uint32_t qwc) {\n'
         '    gt4::perf::Scope perf_vif(gt4::perf::Category::vif_gif);\n    gt4::mtvu::on_vif_transfer(unit, qwc);\n'),
        ('std::uint32_t read(int unit, std::uint32_t reg) {\n',
         'std::uint32_t read(int unit, std::uint32_t reg) {\n    gt4::mtvu::on_vif_read(unit, reg);\n'),
        ('void write(int unit, std::uint32_t reg, std::uint32_t value) {\n',
         'void write(int unit, std::uint32_t reg, std::uint32_t value) {\n    gt4::mtvu::on_vif_write(unit, reg);\n')],
    'gif.cpp': [
        ('void transfer(int path, const std::uint8_t *data, std::uint32_t qwc) {\n',
         'void transfer(int path, const std::uint8_t *data, std::uint32_t qwc) {\n'
         '    gt4::mtvu::GifScope mtvu_gif(path, data, qwc);\n')],
    'memory.cpp': [
        ('    const Target t = resolve(a);\n    if (t.host) {\n        T v;\n',
         '    const Target t = resolve(a);\n    if (t.region == Region::Vu) gt4::mtvu::on_ee_vu_memory(t.address, false);\n'
         '    if (t.host) {\n        T v;\n'),
        ('    const Target t = resolve(a);\n    if (t.host) {\n        std::memcpy(t.host, &v, sizeof v);\n',
         '    const Target t = resolve(a);\n    if (t.region == Region::Vu) gt4::mtvu::on_ee_vu_memory(t.address, true);\n'
         '    if (t.host) {\n        std::memcpy(t.host, &v, sizeof v);\n'),
        ('    if (t.host) std::memcpy(&out, t.host, 16);\n',
         '    if (t.region == Region::Vu) gt4::mtvu::on_ee_vu_memory(t.address, false);\n'
         '    if (t.host) std::memcpy(&out, t.host, 16);\n'),
        ('    if (t.host) std::memcpy(t.host, &v, 16);\n',
         '    if (t.region == Region::Vu) gt4::mtvu::on_ee_vu_memory(t.address, true);\n'
         '    if (t.host) std::memcpy(t.host, &v, 16);\n')],
}
# Extra donor sources derived only with --mtvu-measure: capsule name -> (donor path, pinned donor sha256).
MTVU_MEASURE_SOURCES = {
    'gif.cpp': ('gs/gif.cpp', 'eee761d759464b18fa84ca0970dd76fad5da6b7ce594a0b8383905d7016d1884'),
    'memory.cpp': ('memory.cpp', 'c33b9fedc5727f96bcacffad1c806406b442ce86685e3d60777990a67d11f52e'),
}

# TASK-011 step 2 (slice 170): the MTVU worker runtime. Each anchor is one unique donor text; the derivation
# checks that removing the replacements restores the input. The worker-facing glue is native/mtvu_bridge.cpp.
MTVU_PREFIX = '#include "mtvu_runtime.hpp"\n'
MTVU_PATCHES = {
    'system.cpp': [
        ('    if (g_options.on_vblank) g_options.on_vblank();\n',
         '    gt4::mtvu::vblank_sync();\n    if (g_options.on_vblank) g_options.on_vblank();\n'),
        ('void run_due_events() {\n    sync_iop();\n',
         'void run_due_events() {\n    gt4::mtvu::pump();\n    sync_iop();\n'),
        ('void sync_vu_thread() {\n    if (!gs::queue::vu_active()) return;\n',
         'void sync_vu_thread() {\n    gt4::mtvu::sync("state");\n    if (!gs::queue::vu_active()) return;\n'),
        ('int run() {\n    Cpu &c = g_cpu;\n',
         'int run() {\n    gt4::mtvu::start_with_donor(gt4::mtvu::settings_from_environment());\n    Cpu &c = g_cpu;\n'),
        ('    kernel::dump();\n    iop::dump();\n    sif::dump();\n',
         '    gt4::mtvu::stop();\n    gt4::mtvu::report(stderr);\n    kernel::dump();\n    iop::dump();\n    sif::dump();\n'),
    ],
    'vif.cpp': [
        ('bool on_vu_thread() { return gs::queue::role() == gs::queue::Role::Vu; }\n',
         'bool on_vu_thread() { return gt4::mtvu::on_worker(); }\n'),
        ('bool shadow(int unit) { return unit == 1 && gs::queue::vu_active() && !on_vu_thread(); }\n',
         'bool shadow(int unit) { return unit == 1 && gt4::mtvu::enabled() && !on_vu_thread(); }\n'),
        ('        if (shadow(unit)) return;\n        vu::set_tops(u.top, u.itop);\n',
         '        if (shadow(unit)) {\n            vu::set_tops(u.top, u.itop);\n            return;\n        }\n'
         '        vu::set_tops(u.top, u.itop);\n'),
        ('    if (shadow(unit)) gs::queue::vu().post(gs::queue::kVif1, qwc, 0, data, qwc * 16);\n',
         '    if (shadow(unit)) gt4::mtvu::post_vif_packet(data, qwc);\n'),
        ('    if (shadow(unit)) gs::queue::vu().post(gs::queue::kVif1Write, reg, value, nullptr, 0);\n',
         '    if (shadow(unit)) gt4::mtvu::post_vif_write(reg, value);\n'),
        ('void reset() {\n    for (Unit &u : g_units) u = Unit{};\n}\n',
         'void reset() {\n    gt4::mtvu::sync("vif_reset");\n    for (Unit &u : g_units) u = Unit{};\n    g_vu_side = Unit{};\n}\n'),
        ('bool stalled(int unit) { return state(unit).stalled; }\n',
         'bool stalled(int unit) { return state(unit).stalled; }\n'
         '// Hash of one VIF1 unit (the EE shadow, or the worker side); compared at each VBlank under GT4_MTVU_VERIFY.\n'
         'std::uint64_t verify_hash(bool worker_side) {\n'
         '    const Unit &u = worker_side ? g_vu_side : g_units[1];\n'
         '    std::uint64_t hash = 1469598103934665603ull;\n'
         '    auto mix = [&hash](const void *data, std::size_t bytes) {\n'
         '        const auto *at = static_cast<const std::uint8_t *>(data);\n'
         '        for (std::size_t i = 0; i < bytes; ++i) {\n'
         '            hash ^= at[i];\n'
         '            hash *= 1099511628211ull;\n'
         '        }\n'
         '    };\n'
         '    mix(&u.stat, 4);\n    mix(&u.err, 4);\n    mix(&u.mark, 4);\n    mix(&u.cycle, 4);\n    mix(&u.mode, 4);\n'
         '    mix(&u.num, 4);\n    mix(&u.mask, 4);\n    mix(&u.code, 4);\n    mix(&u.itops, 4);\n    mix(&u.base, 4);\n'
         '    mix(&u.ofst, 4);\n    mix(&u.tops, 4);\n    mix(&u.itop, 4);\n    mix(&u.top, 4);\n    mix(u.row, 16);\n'
         '    mix(u.col, 16);\n    mix(&u.dbf, 1);\n    mix(&u.stalled, 1);\n'
         '    const std::uint64_t pending = u.pending.size();\n    mix(&pending, 8);\n'
         '    mix(u.pending.data(), u.pending.size() * 4);\n'
         '    return hash;\n'
         '}\n'),
    ],
    'vu_micro.cpp': [
        ('std::uint32_t g_top = 0, g_itop = 0;\n',
         '// Per thread: the EE copy follows the shadow VIF1 (set_tops) for VU0; the worker keeps its own.\n'
         'thread_local std::uint32_t g_top = 0, g_itop = 0;\n'),
        ('void reset() {\n    if (gs::queue::vu_active() && gs::queue::role() != gs::queue::Role::Vu) {',
         'void reset() {\n    gt4::mtvu::sync("vu_reset");\n    if (gs::queue::vu_active() && gs::queue::role() != gs::queue::Role::Vu) {'),
    ],
}
# Extra donor sources of the --mtvu capsule (capsule name -> donor path, pinned donor sha256).
MTVU_RUNTIME_SOURCES = {
    'gif.cpp': ('gs/gif.cpp', 'eee761d759464b18fa84ca0970dd76fad5da6b7ce594a0b8383905d7016d1884'),
    'memory.cpp': ('memory.cpp', 'c33b9fedc5727f96bcacffad1c806406b442ce86685e3d60777990a67d11f52e'),
    'hw.cpp': ('hw.cpp', '3fefa5a15284b7f4780c49820bc3d007746e84bae4cb74dec883269d759ce944'),
    'dmac.cpp': ('dmac.cpp', 'f7883686794be6bb00c4a7a87e145da15e5a0a24d7bcbfc564a87a30f5d0b5a7'),
}
MTVU_PATCHES.update({
    'gif.cpp': [
        ('void transfer(int path, const std::uint8_t *data, std::uint32_t qwc) {\n',
         'void transfer(int path, const std::uint8_t *data, std::uint32_t qwc) {\n'
         '    if (gt4::mtvu::on_worker()) {\n        gt4::mtvu::worker_gif(path, data, qwc);\n        return;\n    }\n'
         '    if (gt4::mtvu::enabled()) {\n        gt4::mtvu::gif_transfer(path, data, qwc);\n        return;\n    }\n'),
        ('void reset() {\n    g_stat = 0;\n',
         'void reset() {\n    gt4::mtvu::gif_reset();\n    g_stat = 0;\n'),
        ('void render(int path, const std::uint8_t *data, std::uint32_t qwc) {\n    GsSink sink{path};',
         'void render(int path, const std::uint8_t *data, std::uint32_t qwc) {\n'
         '    gt4::mtvu::note_render(path, data, qwc);\n    GsSink sink{path};'),
        ('    void packed(std::uint32_t reg, const Reg128 &q) { gs::write_packed(reg, q); }\n',
         '    void packed(std::uint32_t reg, const Reg128 &q) {\n'
         '        if (reg == 0xEu && gt4::mtvu::signal_pre_applied(path, static_cast<std::uint32_t>(q.d[1] & 0xFFu))) return;\n'
         '        gs::write_packed(reg, q);\n    }\n'),
        ('void scan_signals(int path, const std::uint8_t *data, std::uint32_t qwc) { scan(g_scan[path], data, qwc); }',
         'void scan_signals(int path, const std::uint8_t *data, std::uint32_t qwc) {\n'
         '    gt4::mtvu::shadow_direct(path, data, qwc);\n}'),
    ],
    'memory.cpp': [
        ('    const Target t = resolve(a);\n    if (t.host) {\n        T v;\n',
         '    const Target t = resolve(a);\n    if (t.region == Region::Vu) gt4::mtvu::sync("vu_memory");\n'
         '    if (t.host) {\n        T v;\n'),
        ('    const Target t = resolve(a);\n    if (t.host) {\n        std::memcpy(t.host, &v, sizeof v);\n',
         '    const Target t = resolve(a);\n    if (t.region == Region::Vu) gt4::mtvu::sync("vu_memory");\n'
         '    if (t.host) {\n        std::memcpy(t.host, &v, sizeof v);\n'),
        ('    if (t.host) std::memcpy(&out, t.host, 16);\n',
         '    if (t.region == Region::Vu) gt4::mtvu::sync("vu_memory");\n    if (t.host) std::memcpy(&out, t.host, 16);\n'),
        ('    if (t.host) std::memcpy(t.host, &v, 16);\n',
         '    if (t.region == Region::Vu) gt4::mtvu::sync("vu_memory");\n    if (t.host) std::memcpy(t.host, &v, 16);\n'),
    ],
    'hw.cpp': [
        ('void reset() {\n    g_intc_stat = g_intc_mask = g_dmac_stat = 0;\n',
         'void reset() {\n    gt4::mtvu::sync("hw_reset");\n    g_intc_stat = g_intc_mask = g_dmac_stat = 0;\n'),
        ('void gs_set_imr(std::uint64_t value) { gs::write_privileged(0x12001010u, value); }',
         'void gs_set_imr(std::uint64_t value) { gt4::mtvu::sync("gs_privileged"); gs::write_privileged(0x12001010u, value); }'),
        ('        if (!(a & 4u)) gs::write_privileged(a, v);',
         '        if (!(a & 4u)) {\n            gt4::mtvu::sync("gs_privileged");\n            gs::write_privileged(a, v);\n        }'),
        ('    if (a >= 0x12000000u && a < 0x12002000u) {\n        gs::write_privileged(a, v);',
         '    if (a >= 0x12000000u && a < 0x12002000u) {\n        gt4::mtvu::sync("gs_privileged");\n        gs::write_privileged(a, v);'),
        ('        gs::read_image(reinterpret_cast<std::uint8_t *>(&out), 1);',
         '        gt4::mtvu::sync("download");\n        gs::read_image(reinterpret_cast<std::uint8_t *>(&out), 1);'),
    ],
    'dmac.cpp': [
        ('    } else if (ch == 1) {\n        for (std::uint32_t i = 0, n = 1; i < c.qwc; i += n, n = 1) {',
         '    } else if (ch == 1) {\n        gt4::mtvu::sync("download");\n'
         '        for (std::uint32_t i = 0, n = 1; i < c.qwc; i += n, n = 1) {'),
    ],
})

# TASK-084 step 0 (slice260): kernel-side counters. The release build replaces donor kernel.cpp with the license guard
# capsule (scripts/prepare_license_guard.py), so the counters are applied there (--frame-counters), on top of the halt
# dump. Both hooks are increments; nothing guest-visible changes.
KERNEL_FRAME_COUNTER_PATCHES = [
    ('#include "ps2/system.hpp"\n', '#include "ps2/system.hpp"\n#include "perf_probe.hpp"\n'),
    ('void switch_to(Cpu &c, Thread *next) {\n',
     'void switch_to(Cpu &c, Thread *next) {\n    gt4::perf::count_thread_switch();\n'),
    ('void handle_syscall(Cpu &c, std::uint32_t pc) {\n    std::uint32_t n = c.r[3].w[0];\n',
     'void handle_syscall(Cpu &c, std::uint32_t pc) {\n'
     '    gt4::perf::SyscallScope gt4_syscall_scope(c.r[3].w[0]);\n'
     '    std::uint32_t n = c.r[3].w[0];\n'),
]


def apply_kernel_frame_counters(source):
    """Adds the TASK-084 kernel counters; checks that removing them restores the input."""
    result = source
    for before, after in KERNEL_FRAME_COUNTER_PATCHES:
        if result.count(before) != 1:
            raise ValueError('Expected one qualified kernel frame-counter site')
        result = result.replace(before, after)
    inverse = result
    for before, after in reversed(KERNEL_FRAME_COUNTER_PATCHES):
        if inverse.count(after) != 1:
            raise ValueError('Unexpected kernel frame-counter reversal')
        inverse = inverse.replace(after, before)
    if inverse != source:
        raise ValueError('Kernel frame counters do not reverse to the input')
    return result


# TASK-035 (slice191): GS stream hooks, applied on top of the --mtvu derivation of gif.cpp and hw.cpp. Same rules:
# each anchor is one unique text of the MTVU-derived source and removing the replacements restores it.
GS_STREAM_PREFIX = '#include "gs_stream_client.hpp"\n'
GS_STREAM_PATCHES = {
    'gif.cpp': [
        ('    void image(const std::uint8_t *data, std::uint32_t qwords) { gs::write_image(data, qwords); }\n};\n',
         '    void image(const std::uint8_t *data, std::uint32_t qwords) { gs::write_image(data, qwords); }\n};\n'
         '\n'
         '// TASK-035 (GT4_RENDERER=pcsx2): the GIF bytes went to the GS stream and the donor GS front end does not run.\n'
         '// The path parser still runs (busy() and the tag counts), and SIGNAL/FINISH/LABEL writes that were not applied\n'
         '// at acceptance (PATH1, or every path with GT4_MTVU=0) are applied here, at the render point, as GsSink would.\n'
         'struct StreamSink {\n'
         '    int path;\n'
         '    void tag(const Path &p, const Reg128 &) { ++g_tags[path][p.flg]; }\n'
         '    void packed(std::uint32_t reg, const Reg128 &q) {\n'
         '        if (reg != 0xEu) return;\n'
         '        const std::uint32_t address = static_cast<std::uint32_t>(q.d[1] & 0xFFu);\n'
         '        if (address < 0x60u || address > 0x62u || gt4::mtvu::signal_pre_applied(path, address)) return;\n'
         '        gs::signal(address, q.d[0]);\n'
         '    }\n'
         '    void reglist(std::uint32_t, std::uint64_t) {}\n'
         '    void image(const std::uint8_t *, std::uint32_t) {}\n'
         '};\n'),
        ('void render_reset() {\n    for (Path &p : g_paths) p = Path{};\n}\n',
         'void render_reset() {\n    gt4::gs_stream::on_gif_reset();\n    for (Path &p : g_paths) p = Path{};\n}\n'),
        ('    gt4::mtvu::note_render(path, data, qwc);\n    GsSink sink{path};',
         '    gt4::mtvu::note_render(path, data, qwc);\n'
         '    if (gt4::gs_stream::on_gif_render(path, data, qwc)) {\n'
         '        StreamSink stream_sink{path};\n'
         '        walk(g_paths[path], data, qwc, stream_sink);\n'
         '        return;\n'
         '    }\n'
         '    GsSink sink{path};'),
    ],
    'hw.cpp': [
        ('    if (!g_options.rate_forced) g_options.pal = mode == 3u;\n',
         '    gt4::gs_stream::on_set_gs_crt(interlace, mode, field);\n'
         '    if (!g_options.rate_forced) g_options.pal = mode == 3u;\n'),
    ],
}


def apply_gs_stream(name, source):
    """Adds the TASK-035 GS stream hooks to one MTVU-derived source; checks that removing them restores the input."""
    result = source
    for before, after in GS_STREAM_PATCHES[name]:
        if result.count(before) != 1:
            raise ValueError(f'Expected one qualified GS stream site in {name}')
        result = result.replace(before, after)
    inverse = result
    for before, after in reversed(GS_STREAM_PATCHES[name]):
        if inverse.count(after) != 1:
            raise ValueError(f'Unexpected GS stream reversal in {name}')
        inverse = inverse.replace(after, before)
    if inverse != source:
        raise ValueError(f'GS stream hooks do not reverse to the input: {name}')
    return result


def overlay_sections(text, anchors=OVERLAY_ANCHORS):
    sections, name, lines = {}, None, []
    for line in text.split('\n'):
        if line.startswith('//@section '):
            if name is not None:
                sections[name] = '\n'.join(lines).rstrip('\n') + '\n'
            name, lines = line.removeprefix('//@section ').strip(), []
        else:
            lines.append(line)
    if name is not None:
        sections[name] = '\n'.join(lines).rstrip('\n') + '\n'
    if set(sections) != set(anchors):
        raise ValueError('AOT overlay sections do not match the anchor table')
    return sections


def apply_overlay(source, path, anchors, label):
    """Inserts every overlay section at its anchor; checks that removing them restores the input."""
    sections = overlay_sections(path.read_text(encoding='utf-8').replace('\r\n', '\n'), anchors)
    result = source
    edits = []
    for name, (mode, anchor) in anchors.items():
        if result.count(anchor) != 1:
            raise ValueError(f'Expected one qualified {label} anchor: {name}')
        text = sections[name]
        if mode == 'after':
            replacement = anchor + text
        elif mode == 'before':
            replacement = text + anchor
        else:
            replacement = text
        result = result.replace(anchor, replacement)
        edits.append((anchor, replacement))
    inverse = result
    for anchor, replacement in reversed(edits):
        if inverse.count(replacement) != 1:
            raise ValueError(f'Unexpected {label} overlay reversal')
        inverse = inverse.replace(replacement, anchor)
    if inverse != source:
        raise ValueError(f'{label} overlay does not reverse to the input')
    return result


def apply_vu1_aot(source):
    """VU1 overlay (slice 151-157); its output and receipt are unchanged by the VU0 overlay."""
    return apply_overlay(source, OVERLAY_PATH, OVERLAY_ANCHORS, 'VU1 AOT')


def apply_vu0_aot(source):
    """VU0 overlay (slice 160), applied on top of the VU1 overlay."""
    return apply_overlay(source, VU0_OVERLAY_PATH, VU0_OVERLAY_ANCHORS, 'VU0 AOT')


def apply_fsv_trace(source):
    """FSV read trace overlay (slice 181): checks that removing its sections restores the input."""
    return apply_overlay(source, FSV_OVERLAY_PATH, FSV_OVERLAY_ANCHORS, 'FSV trace')


def apply_halt_dump(source):
    """Halt forensic dump overlay (slice 204): checks that removing its sections restores the input."""
    return apply_overlay(source, HALT_OVERLAY_PATH, HALT_OVERLAY_ANCHORS, 'halt dump')


def apply_spec2_iop(source):
    """Spec II IOP reboot rule (slice 276): checks that removing its sections restores the input."""
    return apply_overlay(source, SPEC2_IOP_OVERLAY_PATH, SPEC2_IOP_ANCHORS, 'Spec II IOP reboot')


def apply_mtvu_measure(name, source):
    """Adds the TASK-011 step 0 counting hooks; checks that removing them restores the input."""
    result = source
    for before, after in MTVU_MEASURE_PATCHES[name]:
        if result.count(before) != 1:
            raise ValueError(f'Expected one qualified MTVU measure site in {name}')
        result = result.replace(before, after)
    inverse = result
    for before, after in reversed(MTVU_MEASURE_PATCHES[name]):
        if inverse.count(after) != 1:
            raise ValueError(f'Unexpected MTVU measure reversal in {name}')
        inverse = inverse.replace(after, before)
    if inverse != source:
        raise ValueError(f'MTVU measure hooks do not reverse to the input: {name}')
    return result


def derive_mtvu_source(name, source):
    """Derives one extra --mtvu-measure source (gif.cpp, memory.cpp): hooks only."""
    return PREFIX + MTVU_MEASURE_PREFIX + apply_mtvu_measure(name, source)


def apply_mtvu_runtime(name, source):
    """Adds the TASK-011 step 2 MTVU hooks to one donor source; checks that removing them restores the input."""
    result = source
    for before, after in MTVU_PATCHES[name]:
        if result.count(before) != 1:
            raise ValueError(f'Expected one qualified MTVU runtime site in {name}')
        result = result.replace(before, after)
    inverse = result
    for before, after in reversed(MTVU_PATCHES[name]):
        if inverse.count(after) != 1:
            raise ValueError(f'Unexpected MTVU runtime reversal in {name}')
        inverse = inverse.replace(after, before)
    if inverse != source:
        raise ValueError(f'MTVU runtime hooks do not reverse to the input: {name}')
    return result


# TASK-084 (slice260): the frame-cost instrumentation hooks (docs/FRAME-COST-STUDY.md section 6). They are applied only
# with --frame-cost, on top of the --mtvu (and --gs-stream) derivation, so every capsule before slice260 stays
# reproducible byte for byte from this script. Each site is a scope or a counter; nothing guest-visible changes.
FRAME_COST_PATCHES = {
    'system.cpp': [
        # The run loop: events, interrupt service and guest idle get their own scopes (they were inside ee_dispatch
        # or unattributed).
        ('void run_due_events() {\n    gt4::mtvu::pump();\n    sync_iop();\n',
         'void run_due_events() {\n'
         '    gt4::perf::Scope perf_events(gt4::perf::Category::run_due_events);\n'
         '    gt4::mtvu::pump();\n    sync_iop();\n'),
        ('        c.pc &= ~kUnwind;\n        run_due_events();\n        kernel::service(c);\n',
         '        c.pc &= ~kUnwind;\n        run_due_events();\n        {\n'
         '            gt4::perf::Scope perf_service(gt4::perf::Category::kernel_service);\n'
         '            kernel::service(c);\n        }\n'),
        ('void idle(Cpu &c) {\n    std::uint64_t next = next_event_time();\n',
         'void idle(Cpu &c) {\n'
         '    gt4::perf::Scope perf_idle(gt4::perf::Category::guest_idle);\n'
         '    std::uint64_t next = next_event_time();\n'),
    ],
    'dmac.cpp': [
        # TASK-084 step 1 (slice261): the VIF1 chain's packets travel as one job. The batch closes at the chain start
        # (counted, for the job-shape report), at a stall, and at the chain end.
        ('void start(int ch) {\n    Channel &c = g_ch[ch];\n',
         'void start(int ch) {\n    Channel &c = g_ch[ch];\n'
         '    if (ch == 1) gt4::mtvu::note_vif_chain();\n'),
        ('void pause(int ch, bool end) {\n',
         'void pause(int ch, bool end) {\n'
         '    if (ch == 1) gt4::mtvu::flush_vif_chain();\n'),
        ('void finish(int ch) {\n    Channel &c = g_ch[ch];\n',
         'void finish(int ch) {\n    Channel &c = g_ch[ch];\n'
         '    if (ch == 1) gt4::mtvu::flush_vif_chain();\n'),
    ],
}

# The GS stream walk (pcsx2 mode) is skipped when the packet's signals were already applied at acceptance (TASK-084
# step 2) and gets a scope so the [frame] section shows its share of gs_drain.
FRAME_COST_GS_STREAM_PATCHES = {
    'gif.cpp': [
        ('        StreamSink stream_sink{path};\n        walk(g_paths[path], data, qwc, stream_sink);\n',
         '        if (gt4::mtvu::stream_signals_pre_applied(path)) return;\n'
         '        StreamSink stream_sink{path};\n'
         '        gt4::perf::Scope perf_walk(gt4::perf::Category::gif_walk);\n'
         '        walk(g_paths[path], data, qwc, stream_sink);\n'),
    ],
}


def apply_frame_cost(name, source):
    """Adds the TASK-084 instrumentation hooks; checks that removing them restores the input."""
    result = source
    for before, after in FRAME_COST_PATCHES.get(name, ()):
        if result.count(before) != 1:
            raise ValueError(f'Expected one qualified TASK-084 frame-cost site in {name}')
        result = result.replace(before, after)
    inverse = result
    for before, after in reversed(FRAME_COST_PATCHES.get(name, ())):
        if inverse.count(after) != 1:
            raise ValueError(f'Unexpected TASK-084 frame-cost reversal in {name}')
        inverse = inverse.replace(after, before)
    if inverse != source:
        raise ValueError(f'TASK-084 frame-cost hooks do not reverse to the input: {name}')
    return result


# TASK-180b: the inside of run_due_events (one scope in the F8 report). Each of its parts gets a scope of its own, so the
# F8 [frame] line says whether the worker pump, the SIF pump, the event drain or the VBlank callback took the time. Only
# the measurement changes; the guest sees nothing. Applied after --frame-cost, whose scope anchors come first.
RUNLOOP_BREAKDOWN_PATCHES = {
    'system.cpp': [
        ('    gt4::mtvu::pump();\n    sync_iop();\n',
         '    {\n'
         '        gt4::perf::Scope perf_pump(gt4::perf::Category::runloop_mtvu_pump);\n'
         '        gt4::mtvu::pump();\n'
         '    }\n'
         '    sync_iop();\n'),
        ('    sif::pump();\n',
         '    {\n'
         '        gt4::perf::Scope perf_sif(gt4::perf::Category::runloop_sif_pump);\n'
         '        sif::pump();\n'
         '    }\n'),
        ('    for (;;) {\n        const std::uint64_t t = now();\n',
         '    gt4::perf::Scope perf_loop(gt4::perf::Category::runloop_events);\n'
         '    for (;;) {\n        const std::uint64_t t = now();\n'),
        ('    if (g_options.on_vblank) g_options.on_vblank();\n',
         '    if (g_options.on_vblank) {\n'
         '        gt4::perf::Scope perf_vblank(gt4::perf::Category::vblank_action);\n'
         '        g_options.on_vblank();\n'
         '    }\n'),
    ],
}


def apply_runloop_breakdown(name, source):
    """Adds the TASK-180b run_due_events breakdown; checks that removing it restores the input."""
    result = source
    for before, after in RUNLOOP_BREAKDOWN_PATCHES.get(name, ()):
        if result.count(before) != 1:
            raise ValueError(f'Expected one qualified TASK-180b run-loop site in {name}')
        result = result.replace(before, after)
    inverse = result
    for before, after in reversed(RUNLOOP_BREAKDOWN_PATCHES.get(name, ())):
        if inverse.count(after) != 1:
            raise ValueError(f'Unexpected TASK-180b run-loop reversal in {name}')
        inverse = inverse.replace(after, before)
    if inverse != source:
        raise ValueError(f'TASK-180b run-loop hooks do not reverse to the input: {name}')
    return result


def apply_frame_cost_gs_stream(name, source):
    """Adds the TASK-084 GS stream scope; checks that removing it restores the input."""
    result = source
    for before, after in FRAME_COST_GS_STREAM_PATCHES.get(name, ()):
        if result.count(before) != 1:
            raise ValueError(f'Expected one qualified TASK-084 GS stream site in {name}')
        result = result.replace(before, after)
    inverse = result
    for before, after in reversed(FRAME_COST_GS_STREAM_PATCHES.get(name, ())):
        if inverse.count(after) != 1:
            raise ValueError(f'Unexpected TASK-084 GS stream reversal in {name}')
        inverse = inverse.replace(after, before)
    if inverse != source:
        raise ValueError(f'TASK-084 GS stream scope does not reverse to the input: {name}')
    return result


def derive_mtvu_runtime_source(name, source, gs_stream=False, frame_cost=False, runloop=False):
    """Derives one extra --mtvu source (gif.cpp, memory.cpp, hw.cpp, dmac.cpp): hooks only."""
    derived = apply_mtvu_runtime(name, source)
    if frame_cost:
        derived = apply_frame_cost(name, derived)
    if runloop:
        derived = apply_runloop_breakdown(name, derived)
    if gs_stream and name in GS_STREAM_PATCHES:
        derived = apply_gs_stream(name, derived)
        if frame_cost:
            derived = apply_frame_cost_gs_stream(name, derived)
        return PREFIX + MTVU_PREFIX + GS_STREAM_PREFIX + derived
    return PREFIX + MTVU_PREFIX + derived


def derive(name, source, *, split_vu=False, vu1_aot=False, vu0_aot=False, mtvu_measure=False, mtvu=False,
           frame_cost=False, runloop=False):
    original = source
    patches = [(before, after.replace('gt4::perf::Category::vu_interpret',
                'gt4::perf::vu_interpreter_category(unit)') if split_vu else after)
               for before, after in PATCHES[name]]
    for before, after in patches:
        if source.count(before) != 1:
            raise ValueError(f'Expected one qualified patch site in {name}')
        source = source.replace(before, after)
    patched = source
    if vu1_aot and name == 'vu_micro.cpp':
        # apply_vu1_aot checks that its own overlay reverses to the patched text.
        source = apply_vu1_aot(patched)
    if vu0_aot and name == 'vu_micro.cpp':
        # apply_vu0_aot checks that its overlay reverses to the VU1-overlaid text.
        source = apply_vu0_aot(source)
    inverse = patched
    for before, after in reversed(patches):
        inverse = inverse.replace(after, before)
    if inverse != original:
        raise ValueError('Unexpected patch reversal')
    if mtvu_measure:
        if name in MTVU_MEASURE_PATCHES:
            source = apply_mtvu_measure(name, source)
        return PREFIX + MTVU_MEASURE_PREFIX + source
    if mtvu and name in MTVU_PATCHES:
        source = apply_mtvu_runtime(name, source)
        if frame_cost:
            source = apply_frame_cost(name, source)
        if runloop:
            source = apply_runloop_breakdown(name, source)
        return PREFIX + MTVU_PREFIX + source
    return PREFIX + source


def prepare(output, assets_root=None, *, split_vu=False, vu1_aot=False, vu0_aot=False, mtvu_measure=False,
            mtvu=False, gs_stream=False, fsv_trace=False, halt_dump=False, frame_cost=False,
            spec2_iop=False, runloop=False):
    if gs_stream:
        mtvu = True
    if frame_cost and not mtvu:
        raise ValueError('--frame-cost is derived on top of --mtvu')
    if runloop and not frame_cost:
        raise ValueError('--runloop-breakdown is derived on top of --frame-cost')
    if mtvu and mtvu_measure:
        raise ValueError('--mtvu and --mtvu-measure are separate capsules')
    if mtvu:
        vu0_aot = True
    if mtvu_measure:
        vu0_aot = True
    if vu0_aot:
        vu1_aot = True
    if vu1_aot:
        split_vu = True
    output = Path(output).absolute()
    # The capsule lives in the assets checkout: a worktree has no private/ of its own and must not link it.
    private_root = (Path(assets_root or ROOT) / 'private').resolve()
    if output != output.resolve() or output.parent != private_root:
        raise ValueError('Runtime capsule must be an ordinary immediate private child')
    if output.exists():
        raise ValueError('Preserve existing output; choose a fresh runtime capsule')
    donor = Path(assets_root or ROOT) / 'private/references/ps2-web-recomp/runtime/src'
    sources = {}
    for name, expected in PINS.items():
        data = (donor / name).read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f'Qualified donor source differs: {name}')
        text = data.decode('utf-8').replace('\r\n', '\n')
        sources[name] = derive(name, text, split_vu=split_vu, vu1_aot=vu1_aot, vu0_aot=vu0_aot,
                               mtvu_measure=mtvu_measure, mtvu=mtvu, frame_cost=frame_cost, runloop=runloop)
    if mtvu_measure:
        for name, (donor_path, expected) in MTVU_MEASURE_SOURCES.items():
            data = (donor / donor_path).read_bytes()
            if hashlib.sha256(data).hexdigest() != expected:
                raise ValueError(f'Qualified donor source differs: {donor_path}')
            sources[name] = derive_mtvu_source(name, data.decode('utf-8').replace('\r\n', '\n'))
    if mtvu:
        for name, (donor_path, expected) in MTVU_RUNTIME_SOURCES.items():
            data = (donor / donor_path).read_bytes()
            if hashlib.sha256(data).hexdigest() != expected:
                raise ValueError(f'Qualified donor source differs: {donor_path}')
            sources[name] = derive_mtvu_runtime_source(name, data.decode('utf-8').replace('\r\n', '\n'), gs_stream,
                                                       frame_cost, runloop)
    fsv_sources = {}
    if fsv_trace:
        for name, expected in FSV_PIN.items():
            data = (donor / name).read_bytes()
            if hashlib.sha256(data).hexdigest() != expected:
                raise ValueError(f'Qualified donor source differs: {name}')
            text = data.decode('utf-8').replace('\r\n', '\n')
            fsv_sources[Path(name).name] = apply_fsv_trace(text)
    sources.update(fsv_sources)
    halt_sources = {}
    if halt_dump:
        for name, expected in HALT_PIN.items():
            data = (donor / name).read_bytes()
            if hashlib.sha256(data).hexdigest() != expected:
                raise ValueError(f'Qualified donor source differs: {name}')
            text = data.decode('utf-8').replace('\r\n', '\n')
            halt_sources[Path(name).name] = apply_halt_dump(text)
    sources.update(halt_sources)
    spec2_sources = {}
    if spec2_iop:
        for name, expected in SPEC2_IOP_PIN.items():
            data = (donor / name).read_bytes()
            if hashlib.sha256(data).hexdigest() != expected:
                raise ValueError(f'Qualified donor source differs: {name}')
            text = data.decode('utf-8').replace('\r\n', '\n')
            spec2_sources[Path(name).name] = apply_spec2_iop(text)
    sources.update(spec2_sources)
    output.mkdir()
    receipt = {'donor_revision': '1b4808793a12c45125262fd2d5f50a9ec9f7c020',
               'donor_sha256': dict(PINS), 'source_sha256': {}}
    if fsv_trace:
        receipt['donor_sha256'].update(FSV_PIN)
        receipt['fsv_trace_profile'] = 'overlay-v1'
        receipt['fsv_trace_overlay_sha256'] = hashlib.sha256(FSV_OVERLAY_PATH.read_bytes().replace(b'\r\n', b'\n')).hexdigest()
    if halt_dump:
        receipt['donor_sha256'].update(HALT_PIN)
        receipt['halt_dump_profile'] = 'overlay-v1'
        receipt['halt_dump_overlay_sha256'] = hashlib.sha256(HALT_OVERLAY_PATH.read_bytes().replace(b'\r\n', b'\n')).hexdigest()
    if spec2_iop:
        receipt['donor_sha256'].update(SPEC2_IOP_PIN)
        receipt['spec2_iop_profile'] = 'dnas310-reboot-v1'
        receipt['spec2_iop_overlay_sha256'] = hashlib.sha256(
            SPEC2_IOP_OVERLAY_PATH.read_bytes().replace(b'\r\n', b'\n')).hexdigest()
    if split_vu:
        receipt['vu_scope_profile'] = 'unit_split_run_fast_only'
    if vu1_aot:
        receipt['vu1_aot_profile'] = 'overlay-v1'
        # Hashed with LF line endings: a checkout with core.autocrlf stores the overlay with CRLF, and the
        # pinned value (slice 154) is the LF text that the derivation parses.
        receipt['vu1_aot_overlay_sha256'] = hashlib.sha256(OVERLAY_PATH.read_bytes().replace(b'\r\n', b'\n')).hexdigest()
    if vu0_aot:
        receipt['vu0_aot_profile'] = 'overlay-v1'
        receipt['vu0_aot_overlay_sha256'] = hashlib.sha256(VU0_OVERLAY_PATH.read_bytes().replace(b'\r\n', b'\n')).hexdigest()
    if mtvu_measure:
        receipt['mtvu_measure_profile'] = 'step0-counters-v1'
        receipt['mtvu_measure_donor_sha256'] = {name: expected for name, (_, expected) in MTVU_MEASURE_SOURCES.items()}
    if mtvu:
        # TASK-084 (slice260): the profile names the chain-batched VIF1 jobs and the per-direction doorbells; the
        # capsules before slice260 keep their pinned receipts.
        receipt['mtvu_runtime_profile'] = 'task084-frame-cost-v1' if frame_cost else 'step4-runtime-v2'
        receipt['mtvu_runtime_donor_sha256'] = {name: expected for name, (_, expected) in MTVU_RUNTIME_SOURCES.items()}
    if gs_stream:
        receipt['gs_stream_profile'] = 'task035-seam-v1'
        receipt['gs_stream_sources'] = sorted(GS_STREAM_PATCHES)
    if runloop:
        receipt['runloop_breakdown_profile'] = 'task180b-v1'
        receipt['runloop_breakdown_sources'] = sorted(RUNLOOP_BREAKDOWN_PATCHES)
    for name, text in sources.items():
        path = output / name
        path.write_text(text, encoding='utf-8', newline='\n')
        receipt['source_sha256'][name] = hashlib.sha256(path.read_bytes()).hexdigest()
    (output / 'runtime.json').write_text(json.dumps(receipt, indent=2) + '\n', encoding='utf-8')
    return receipt


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--assets-root', type=Path)
    parser.add_argument('--split-vu', action='store_true')
    parser.add_argument('--vu1-aot', action='store_true')
    parser.add_argument('--vu0-aot', action='store_true')
    parser.add_argument('--mtvu-measure', action='store_true')
    parser.add_argument('--mtvu', action='store_true')
    parser.add_argument('--gs-stream', action='store_true')
    parser.add_argument('--fsv-trace', action='store_true')
    parser.add_argument('--halt-dump', action='store_true')
    parser.add_argument('--spec2-iop', action='store_true',
                        help='TASK-100 S7b: the Spec II IOP reboot rule (DNAS310.IMG selects the asynchronous FILEIO server)')
    parser.add_argument('--frame-cost', action='store_true',
                        help='TASK-084: also add the frame-cost instrumentation hooks (run loop, VIF1 chain, GIF walk)')
    parser.add_argument('--runloop-breakdown', action='store_true',
                        help='TASK-180b: split run_due_events into worker pump, SIF pump, event drain and VBlank callback')
    args = parser.parse_args()
    prepare(args.output, args.assets_root, split_vu=args.split_vu, vu1_aot=args.vu1_aot, vu0_aot=args.vu0_aot,
            mtvu_measure=args.mtvu_measure, mtvu=args.mtvu, gs_stream=args.gs_stream, fsv_trace=args.fsv_trace,
            halt_dump=args.halt_dump, frame_cost=args.frame_cost, spec2_iop=args.spec2_iop,
            runloop=args.runloop_breakdown)

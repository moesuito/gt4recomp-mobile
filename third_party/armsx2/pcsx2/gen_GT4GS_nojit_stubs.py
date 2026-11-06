#!/usr/bin/env python3
"""Generates pcsx2/GT4GSNoJitStubs.cpp for the GT4GS_NO_JIT build.

With GT4GS_NO_JIT the ARM64 recompiler sources (EE, IOP, microVU, VIF unpack and the VU AOT reporters) are not compiled
into libgt4gs, so vixl is not linked. The R5900 opcode tables and SaveStateBase still name their entry points, which
keeps the link honest; this script writes the empty definitions that close it.

Re-run from the repository root whenever third_party/armsx2/pcsx2/R5900OpcodeTables.h changes:

    python third_party/armsx2/pcsx2/gen_GT4GS_nojit_stubs.py

The generated file is committed; nothing calls these functions at run time (no executable code memory is mapped and no
recompiler provider is initialized).
"""

import io
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
HEADER = os.path.join(HERE, "R5900OpcodeTables.h")
OUTPUT = os.path.join(HERE, "GT4GSNoJitStubs.cpp")

with io.open(HEADER, encoding="utf-8") as f:
    src = f.read().split("\n")

# The Dynarec declarations run from "namespace R5900 {" to "namespace OpcodeTables" (exclusive).
start = next(i for i, line in enumerate(src) if line.strip() == "namespace R5900")
end = next(i for i, line in enumerate(src) if line.strip() == "namespace OpcodeTables")
region = src[start:end]

base = ["R5900", "Dynarec", "OpcodeImpl"]
current = base[:]
entries = []
declaration = re.compile(r"^\s*void\s+(rec\w+)\(([^)]*)\);")
for line in region:
    stripped = line.strip()
    match = re.match(r"^namespace\s+(MMI|COP0|COP1)\s*$", stripped)
    if match:
        current = base + [match.group(1)]
        continue
    match = re.match(r"^}\s*//\s*namespace\s+(MMI|COP0|COP1)", stripped)
    if match:
        current = base[:]
        continue
    match = declaration.match(line)
    if match:
        entries.append((tuple(current), match.group(1), match.group(2).strip()))

out = [
    "// SPDX-FileCopyrightText: 2026 GT4Recomp contributors",
    "// SPDX-License-Identifier: GPL-3.0+",
    "//",
    "// GT4GS_NO_JIT (private Android build): the ARM64 recompiler sources (EE/IOP/microVU/VIF/VU-AOT) are not compiled",
    "// into this library, so vixl is not linked. The opcode tables and the savestate class still name their entry points,",
    "// which keeps the link honest; these empty definitions close it. Nothing calls them: the library maps no executable",
    "// code memory, VMManager initializes no recompiler provider, and every CPU provider is an interpreter (the GS-only VM",
    "// uses the stream CPU provider). Generated from R5900OpcodeTables.h by gen_GT4GS_nojit_stubs.py, not hand-edited.",
    "",
    '#include "R5900OpcodeTables.h"',
    '#include "SaveState.h"',
    '#include "VU.h"',
    '#include "arm64/FPU-divunit-arm64.h"',
    "",
]
current_namespace = None
for names, name, params in entries:
    if names != current_namespace:
        if current_namespace is not None:
            out.append("} // namespace " + "::".join(current_namespace))
            out.append("")
        out.append("namespace " + "::".join(names))
        out.append("{")
        current_namespace = names
    out.append("\tvoid " + name + "(" + params + ") {}")
if current_namespace is not None:
    out.append("} // namespace " + "::".join(current_namespace))
out.append("")
out.append("u32 R5900::Interpreter::OpcodeImpl::COP1::eeDivideSignificandArm64(u32, u32, u64, u32)")
out.append("{")
out.append("\treturn 0;")
out.append("}")
out.append("")
out.append("bool SaveStateBase::vuJITFreeze()")
out.append("{")
out.append("\treturn true;")
out.append("}")
out.append("")
out.append("template <int idx> void dVifUnpack(const u8*, bool) {}")
out.append("template void dVifUnpack<0>(const u8*, bool);")
out.append("template void dVifUnpack<1>(const u8*, bool);")
out.append("void dVifReset(int) {}")
out.append("void vuAotNoteGlobalSetting(bool) {}")
out.append("void vuAotNoVmSettingsApplied(bool, bool) {}")
out.append("void vuAotSettingChanged(bool, bool, bool) {}")
out.append("void vuAotNoteOutsideStatusRead() {}")
out.append("void vuAotResetStickyTripwire() {}")
out.append("void vuAotSetTitle(const std::string&, u32) {}")

with io.open(OUTPUT, "w", encoding="utf-8", newline="\n") as f:
    f.write("\n".join(out) + "\n")

print("wrote %s (%d opcode entries)" % (OUTPUT, len(entries)))

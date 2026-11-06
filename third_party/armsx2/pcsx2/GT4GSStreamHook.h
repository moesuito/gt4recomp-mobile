// SPDX-License-Identifier: GPL-3.0+
//
// GT4Recomp private Android test build (PCSX2-tainted, GPL), not distributed.
//
// The two hooks ARMSX2's core needs to boot a GS-only stream instead of a disc or a GS dump. They are the same three
// hooks the owner's fork frontend adds to its PCSX2 tree (gt4recomp-gs): GSDumpReplayer::IsReplayingDump() answers true
// while a stream is prepared, AutoDetectSource routes the boot to the stream, and UpdateCPUImplementations selects the
// record-loop CPU provider.

#pragma once

#include "common/Error.h"

#include <string>

struct R5900cpu;

namespace gt4gs {

bool IsActive();
bool Initialize(Error *error);
// The performance HUD of this library (nothing when the launcher did not ask for it).
void DrawHud();
void Shutdown();
std::string GetSerial();

}  // namespace gt4gs

extern R5900cpu Gt4gsStreamCpu;

// SPDX-License-Identifier: GPL-3.0+
//
// GT4Recomp private Android test build (PCSX2-tainted, GPL), not distributed.
//
// The performance HUD of the ARMSX2 GS (see gt4gs_hud.cpp): the same idea as the game's CoreGS 2.0 HUD, drawn by this
// library over its own picture, and only when the launcher asks for it.
#pragma once

namespace gt4gs {

// 0 off, 1 frame rate only, 2 the full panel. Set once before the GS boots.
void SetHudMode(int mode);
int HudMode();

// Called on the GS thread just before the frame is presented (the vendored renderer calls it); draws nothing when off.
void DrawHud();

}  // namespace gt4gs

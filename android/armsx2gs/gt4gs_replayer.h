// SPDX-License-Identifier: GPL-3.0+
//
// GT4Recomp private Android test build (PCSX2-tainted, GPL). NOT distributed.
//
// The GS stream replayer for the in-process Android build: a port of the owner's fork frontend
// (gt4recomp-gs, pcsx2/GSStreamReplayer.cpp), reduced to what an in-process consumer needs. The records and their
// per-record work are identical (protocol 1.3, docs/GS-STREAM-PROTOCOL.md of the game repository):
//
//   GIF        the bytes go to the GIF unit path named in the record, as completed GS packets (parts flagged MORE are
//              joined first).
//   PRIV       stored in the 8 KiB privileged block; CSR RESET (bit 9) resets the block, IMR, CSR and the GS; bits 0-4
//              clear interrupt flags; IMR keeps the mask bits; PMODE/DISPFB1/DISPFB2 mark "registers written" for the
//              next VSync.
//   VSYNC      CSR FIELD is set to the record's field (the display field), then the vsync is posted to the GS thread,
//              which presents; vsyncs_done is bumped after the GS thread processed it (the game's pacing).
//   READ_FIFO  local-to-host read through MTGS::InitAndReadFIFO into the in-process response area.
//   RESET      kind 1 GS hard reset, kind 2 GIF path parsers.
//   CONTROL    shutdown, attach_window, resize, snapshot; display_aspect (kind 7) switches the presented picture between
//              the configured aspect and fill.
//
// This library is private and never shipped: it exists to compare the ARMSX2 GS with CoreGS 2.0 on the owner's phone.

#pragma once

#include <cstdint>
#include <string>

#include "common/Error.h"

namespace gt4gs {

// Whether a stream was prepared (the VMManager hooks ask this before booting).
bool IsActive();
bool Initialize(Error *error);
void Shutdown();
std::string GetSerial();

// Frame counter of the stream (VSYNC records processed), for the log and the frame dumps.
std::uint64_t GetFrameNumber();
// 0 after a CONTROL shutdown or the end of the stream, non-zero otherwise.
int GetExitCode();
// Posts a fatal ERROR and EXITING to the log; used when the GS device cannot be created after boot.
void ReportFatal(std::uint32_t code, const char *text);

}  // namespace gt4gs

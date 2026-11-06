// SPDX-License-Identifier: MIT
// GT4Recomp private Android test build (owner's phone only): the ARMSX2 GS as an in-process renderer.
//
// GT4_RENDERER=armsx2 sends the very same calls the GS stream seam sends to the Windows gt4-gs process (GIF chunks at
// the render point, privileged writes in stream order, VSYNC with the field, resets, local-to-host reads, window
// attach and resize) to libgt4gs.so, which runs ARMSX2's GS on its own thread in this process. No IPC.
//
// The library is GPL-3.0+ and is loaded dynamically: this MIT file only declares the flat C ABI of gt4gs_api.h, so the
// game library never links GPL code at build time and a build without the private library still starts (the renderer
// refuses with a clear message instead).
#pragma once

#include <string>

#include "gs_stream/gs_stream_sink.hpp"

namespace gt4::gs_stream {

// The sink backed by libgt4gs.so. The hooks return without doing anything when the library is missing and start()
// reports why.
const InProcessSink &armsx2_in_process_sink();

// True when libgt4gs.so was found and its ABI matches (checked once, lazily).
bool armsx2_available(std::string &error);

}  // namespace gt4::gs_stream

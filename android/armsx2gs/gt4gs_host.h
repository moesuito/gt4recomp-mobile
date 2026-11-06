// SPDX-License-Identifier: GPL-3.0+
//
// GT4Recomp private Android test build (PCSX2-tainted, GPL). Internal interfaces of libgt4gs.so.

#pragma once

#include <cstdint>
#include <optional>

#include "common/Pcsx2Defs.h"
#include "common/WindowInfo.h"

struct R5900cpu;

namespace gt4gs {

class QueueSource;

// Installs the stream the replayer consumes; called once before the VM boots.
void SetStream(QueueSource *source, const char *renderer_name, const char *version);

namespace host {

// Window: the ANativeWindow* the game gave us and its pixel size. Safe from any thread.
void SetNativeWindow(void *native_window, std::uint32_t width, std::uint32_t height);
void DetachNativeWindow();
// The size the GS should present at (CONTROL attach_window / resize, on the CPU thread).
void ApplyWindowSize(std::uint32_t width, std::uint32_t height);

// Host::AcquireRenderWindow / ReleaseRenderWindow backing store (GS thread).
std::optional<WindowInfo> AcquireWindow(bool recreate_window);
void ReleaseWindow();

// Host::RunOnCPUThread: tasks queued for the CPU thread, drained at each VSYNC.
void RunOnCpuThread(void (*task)(void *), void *argument);
void DrainQueuedTasks();

// 1 while the library is booting or running.
bool Running();
// Data root of the session (the game passes it in gt4gs_start).
const char *DataRoot();

}  // namespace host
}  // namespace gt4gs

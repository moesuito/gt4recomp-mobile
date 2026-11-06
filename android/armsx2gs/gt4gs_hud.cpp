// SPDX-License-Identifier: GPL-3.0+
//
// GT4Recomp private Android test build (PCSX2-tainted, GPL), not distributed.
//
// The performance HUD for the ARMSX2 GS, drawn by this library over its own picture (the game's HUD layout is CoreGS
// 2.0's and does not run here). It shows the same kind of information the game's HUD shows with CoreGS 2.0 — frame
// rate, frame time, processor use, graphics use, memory, resolution — from this library's own counters, and labels
// every line for what it really is:
//   "GS"  the GS thread's time per frame (the emulator's GS worker).
//   "GPU" the graphics device's timer, when the driver reports it; "n/a" otherwise.
// The game's own F7/F8 performance report is untouched by this file.

#include "gt4gs_hud.h"

#include "common/Timer.h"

#include "GS.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "ImGui/ImGuiManager.h"
#include "PerformanceMetrics.h"

#include "fmt/format.h"
#include "imgui.h"

#include <android/log.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>

namespace gt4gs {
namespace {

int s_mode = 0;  // 0 off, 1 frame rate only, 2 the full panel

// The game's share of the processor, as the desktop HUD reports it (all threads, so 100% is one whole core per thread):
// utime + stime of this process over the wall time, divided by the cores.
struct CpuSampler {
  std::uint64_t last_ticks = 0;
  Common::Timer::Value last_time = 0;
  double percent = -1.0;

  void Update() {
    std::FILE *file = std::fopen("/proc/self/stat", "r");
    if (file == nullptr) return;
    char buffer[1024];
    const std::size_t read = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    buffer[read] = '\0';
    // Field 14 (utime) and 15 (stime), after the parenthesised command name.
    const char *close = std::strrchr(buffer, ')');
    if (close == nullptr) return;
    unsigned long long utime = 0, stime = 0;
    int field = 3;
    const char *cursor = close + 1;
    while (*cursor != '\0' && field < 15) {
      while (*cursor == ' ') ++cursor;
      if (field == 14)
        std::sscanf(cursor, "%llu", &utime);
      else if (field == 15)
        std::sscanf(cursor, "%llu", &stime);
      while (*cursor != ' ' && *cursor != '\0') ++cursor;
      ++field;
    }
    const std::uint64_t ticks = utime + stime;
    const Common::Timer::Value now = Common::Timer::GetCurrentValue();
    if (last_time != 0 && now != last_time) {
      const double seconds = Common::Timer::ConvertValueToSeconds(now - last_time);
      const double tick_seconds = static_cast<double>(ticks - last_ticks) / static_cast<double>(sysconf(_SC_CLK_TCK));
      const long cores = std::max(1L, sysconf(_SC_NPROCESSORS_ONLN));
      if (seconds > 0.0) percent = 100.0 * tick_seconds / seconds / static_cast<double>(cores);
    }
    last_ticks = ticks;
    last_time = now;
  }
};

CpuSampler s_cpu;

double MemoryMb() {
  std::FILE *file = std::fopen("/proc/self/statm", "r");
  if (file == nullptr) return -1.0;
  unsigned long size = 0, resident = 0;
  const int got = std::fscanf(file, "%lu %lu", &size, &resident);
  std::fclose(file);
  if (got != 2) return -1.0;
  return static_cast<double>(resident) * static_cast<double>(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
}

std::string SocName() {
  char manufacturer[PROP_VALUE_MAX] = {};
  char model[PROP_VALUE_MAX] = {};
  __system_property_get("ro.soc.manufacturer", manufacturer);
  __system_property_get("ro.soc.model", model);
  std::string name;
  if (manufacturer[0] != '\0') name = manufacturer;
  if (model[0] != '\0') name += name.empty() ? model : std::string(" ") + model;
  if (name.empty()) {
    char hardware[PROP_VALUE_MAX] = {};
    __system_property_get("ro.hardware", hardware);
    name = hardware;
  }
  return name;
}

std::string RendererName() {
  const GSRendererType type = GSConfig.Renderer;
  switch (type) {
    case GSRendererType::VK: return "Vulkan";
    case GSRendererType::OGL: return "GLES";
    case GSRendererType::SW: return "SW";
    case GSRendererType::Null: return "Null";
    default: return "?";
  }
}

std::string ScaleText() {
  const float scale = GSConfig.UpscaleMultiplier;
  if (scale <= 0.0f) return std::string();
  return fmt::format("{:g}x", scale);
}

}  // namespace

void SetHudMode(int mode) { s_mode = std::clamp(mode, 0, 2); }

int HudMode() { return s_mode; }

void DrawHud() {
  if (s_mode == 0) return;
  ImDrawList *draw = ImGui::GetBackgroundDrawList();
  if (draw == nullptr) return;

  // The picture's size sets the text size: the same panel is readable at 1080p and at 1440p.
  const ImGuiIO &io = ImGui::GetIO();
  const float height = io.DisplaySize.y > 0.0f ? io.DisplaySize.y : 1080.0f;
  const float size = std::max(14.0f, std::floor(height / 55.0f));
  const float line = std::ceil(size * 1.25f);
  const float margin = std::ceil(size * 0.6f);
  ImFont *font = ImGuiManager::GetFixedFont();
  if (font == nullptr) font = ImGui::GetFont();
  if (font == nullptr) font = ImGui::GetIO().FontDefault;
  if (font == nullptr) return;

  const ImU32 white = IM_COL32(255, 255, 255, 255);
  const ImU32 dim = IM_COL32(176, 176, 176, 255);
  const ImU32 blue = IM_COL32(255, 185, 138, 255);
  const ImU32 green = IM_COL32(157, 232, 157, 255);
  const ImU32 shadow = IM_COL32(0, 0, 0, 200);

  std::vector<std::string> lines;
  std::vector<ImU32> colours;
  if (s_mode == 1) {
    const float fps = PerformanceMetrics::GetFPS();
    lines.push_back(fps > 0.0f ? fmt::format("{:.1f} FPS", fps) : std::string("-- FPS"));
    colours.push_back(white);
  } else {
    s_cpu.Update();
    lines.push_back(fmt::format("ARMSX2 GS  {}  {}", RendererName(), ScaleText()));
    colours.push_back(dim);
    const std::string soc = SocName();
    if (!soc.empty()) {
      lines.push_back(soc);
      colours.push_back(dim);
    }
    const float fps = PerformanceMetrics::GetFPS();
    const float frame_ms = PerformanceMetrics::GetAverageFrameTime();
    lines.push_back(fps > 0.0f ? fmt::format("FPS:   {:6.2f}", fps) : std::string("FPS:      --"));
    colours.push_back(white);
    lines.push_back(frame_ms > 0.0f ? fmt::format("Frame: {:6.2f} ms", frame_ms) : std::string("Frame:    --"));
    colours.push_back(blue);
    const float gs_ms = PerformanceMetrics::GetGSThreadAverageTime();
    const float gs_pct = PerformanceMetrics::GetGSThreadUsage();
    lines.push_back(gs_ms > 0.0f ? fmt::format("GS:    {:5.2f} ms  {:4.1f}%", gs_ms, gs_pct) : std::string("GS:      --"));
    colours.push_back(green);
    const float gpu_ms = PerformanceMetrics::GetGPUAverageTime();
    const float gpu_pct = PerformanceMetrics::GetGPUUsage();
    lines.push_back(gpu_ms > 0.0f ? fmt::format("GPU:   {:5.2f} ms  {:4.1f}%", gpu_ms, gpu_pct) : std::string("GPU:     n/a"));
    colours.push_back(green);
    lines.push_back(s_cpu.percent >= 0.0 ? fmt::format("CPU:   {:5.1f}%", s_cpu.percent) : std::string("CPU:     --"));
    colours.push_back(green);
    const double memory = MemoryMb();
    lines.push_back(memory >= 0.0 ? fmt::format("Mem:  {:5.0f} MB", memory) : std::string("Mem:     --"));
    colours.push_back(white);
  }

  float widest = 0.0f;
  for (const std::string &text : lines) {
    const ImVec2 extent = font->CalcTextSizeA(size, std::numeric_limits<float>::max(), -1.0f, text.c_str());
    widest = std::max(widest, extent.x);
  }
  const float panel_width = widest + 2.0f * margin;
  const float panel_height = static_cast<float>(lines.size()) * line + 2.0f * margin;
  draw->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(panel_width, panel_height), IM_COL32(0, 0, 0, 160), margin);
  float y = margin;
  for (std::size_t i = 0; i < lines.size(); i++) {
    draw->AddText(font, size, ImVec2(margin + 1.0f, y + 1.0f), shadow, lines[i].c_str());
    draw->AddText(font, size, ImVec2(margin, y), colours[i], lines[i].c_str());
    y += line;
  }
}

}  // namespace gt4gs

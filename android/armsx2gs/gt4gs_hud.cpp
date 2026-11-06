// SPDX-License-Identifier: GPL-3.0+
//
// GT4Recomp private Android test build (PCSX2-tainted, GPL), not distributed.
//
// The performance HUD of the ARMSX2 GS, drawn by this library over its own picture. It is the CoreGS 2.0 HUD (TASK-135,
// native/hw_gs/hud/hw_hud.*): the same panel in the window's top right corner, the same black 63% background, the same
// 36-column layout and line order, the same labels, units, colours and number formats, the same 500 ms text pace, the
// same frame-time graph (5 text lines, 50 ms top, guides at one and two 60 Hz frames) and the same FPS-only panel.
// The renderer's own row says "GSdx (ARMSX2)" where CoreGS says "CoreGS 2.0"; every other line comes from the same
// game-side sources (the process's CPU time and resident memory, the SoC as Android reports it, the driver's busy share)
// and a reading the phone does not give this app shows "n/a" instead of being invented.
//
// The game's own F7/F8 performance report is untouched by this file.

#include "gt4gs_hud.h"

#include "common/Timer.h"

#include "GS.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "ImGui/ImGuiManager.h"

#include "imgui.h"

#include <android/log.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace gt4gs {
namespace {

// --- the panel's constants, copied from the CoreGS 2.0 HUD --------------------------------------
constexpr int kColumns = 36;
constexpr int kFpsColumns = 10;
constexpr double kTextPeriodMs = 500.0;
constexpr double kWindowMs = 1000.0;
constexpr double kGraphTopMs = 50.0;
constexpr int kGraphRows = 5;
constexpr int kMarginPixels = 12;
constexpr std::size_t kTimesCapacity = 1024;

// Colours are 0xAABBGGRR, the byte order of ImGui's packed colours (R8G8B8A8 in memory).
constexpr ImU32 kWhite = 0xFFFFFFFFu;
constexpr ImU32 kRed = 0xFF8A8AFFu;
constexpr ImU32 kBlue = 0xFFFFB98Au;
constexpr ImU32 kGreen = 0xFF9DE89Du;
constexpr ImU32 kDim = 0xFFB0B0B0u;
constexpr ImU32 kPanel = 0xA0000000u;  // black, 63% opaque
constexpr ImU32 kGuide = 0x50FFFFFFu;
constexpr double kOneFrameMs = 1000.0 / 60.0;

int s_mode = 0;  // 0 off, 1 frame rate only, 2 the full panel

// --- the frame times, as CoreGS keeps them ------------------------------------------------------
struct FrameWindow {
  std::uint32_t intervals = 0;
  double fps = 0.0, fps_low = 0.0, fps_high = 0.0;
  double frame_ms = 0.0, frame_ms_low = 0.0, frame_ms_high = 0.0;
};

struct FrameTimes {
  std::array<double, kTimesCapacity> times{};
  std::size_t next = 0, stored = 0;

  void Note(double now_ms) {
    times[next] = now_ms;
    next = (next + 1) % kTimesCapacity;
    stored = std::min(stored + 1, kTimesCapacity);
  }

  FrameWindow Window(double now_ms) const {
    FrameWindow out;
    double newest = 0.0, oldest = 0.0, low = 0.0, high = 0.0;
    for (std::size_t back = 1; back <= stored; ++back) {
      const double time = times[(next + kTimesCapacity - back) % kTimesCapacity];
      if (now_ms - time > kWindowMs) break;
      if (back == 1) {
        newest = time;
      } else {
        const double interval = oldest - time;
        low = out.intervals == 0 ? interval : std::min(low, interval);
        high = out.intervals == 0 ? interval : std::max(high, interval);
        ++out.intervals;
      }
      oldest = time;
    }
    if (out.intervals == 0 || newest <= oldest) return FrameWindow{};
    out.frame_ms = (newest - oldest) / out.intervals;
    out.frame_ms_low = low;
    out.frame_ms_high = high;
    out.fps = 1000.0 / out.frame_ms;
    out.fps_low = high > 0.0 ? 1000.0 / high : 0.0;
    out.fps_high = low > 0.0 ? 1000.0 / low : 0.0;
    return out;
  }

  void RecentIntervals(std::vector<float> &out, std::size_t count) const {
    out.clear();
    if (stored < 2) return;
    const std::size_t have = std::min(count, stored - 1);
    for (std::size_t back = have; back >= 1; --back) {
      const double newer = times[(next + kTimesCapacity - back) % kTimesCapacity];
      const double older = times[(next + kTimesCapacity - back - 1) % kTimesCapacity];
      out.push_back(static_cast<float>(newer - older));
    }
  }
};

FrameTimes s_times;

// --- the machine's readings, as CoreGS reads them on Android ------------------------------------
struct SystemReading {
  double cpu_percent = -1.0;  // the game's share of all the processor's threads
  double gpu_percent = -1.0;  // the driver's own busy share (kgsl), n/a when the system hides it
  double memory_mb = -1.0;
  double video_memory_mb = -1.0;  // Android gives this app no VRAM figure: "--"
  std::uint32_t refresh_hz = 0;
};

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
    // Fields 14 (utime) and 15 (stime), after the parenthesised command name.
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

// The Adreno driver's busy share: "gpu_busy_percentage" ("37 %"), else "gpubusy" (busy and total ticks). Many phones
// keep these from apps (SELinux); then the GPU line stays unknown, exactly as CoreGS's does.
double GpuBusyPercent() {
  if (std::FILE *file = std::fopen("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage", "r")) {
    int percent = -1;
    const int read = std::fscanf(file, "%d", &percent);
    std::fclose(file);
    if (read == 1 && percent >= 0 && percent <= 100) return percent;
  }
  if (std::FILE *file = std::fopen("/sys/class/kgsl/kgsl-3d0/gpubusy", "r")) {
    long long busy = 0, total = 0;
    const int read = std::fscanf(file, "%lld %lld", &busy, &total);
    std::fclose(file);
    if (read == 2 && total > 0 && busy >= 0 && busy <= total) return 100.0 * static_cast<double>(busy) / static_cast<double>(total);
  }
  return -1.0;
}

std::string SystemProperty(const char *name) {
  char value[PROP_VALUE_MAX] = {};
  return __system_property_get(name, value) > 0 ? std::string(value) : std::string();
}

// The commercial names of the common phone chips (the SoC model is all Android reports), as CoreGS shows them.
const char *SocMarketingName(const std::string &model) {
  static const std::pair<const char *, const char *> kNames[] = {
      {"SM8750", "Snapdragon 8 Elite"},   {"SM8650", "Snapdragon 8 Gen 3"},  {"SM8550", "Snapdragon 8 Gen 2"},
      {"SM8475", "Snapdragon 8+ Gen 1"},  {"SM8450", "Snapdragon 8 Gen 1"},  {"SM8350", "Snapdragon 888"},
      {"SM8250", "Snapdragon 865"},       {"SM7675", "Snapdragon 7+ Gen 3"}, {"SM7550", "Snapdragon 7 Gen 3"},
  };
  for (const auto &[soc, name] : kNames)
    if (model.rfind(soc, 0) == 0) return name;
  return nullptr;
}

std::string CleanDeviceName(const std::string &raw) {
  std::string out;
  bool pending_space = false, in_sequence = false;
  for (const char character : raw) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte >= 0x80 && byte < 0xC0 && in_sequence) continue;  // the rest of a UTF-8 sequence
    in_sequence = false;
    if (byte <= ' ' || byte == 0x7F) {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) out += ' ';
    pending_space = false;
    if (byte > '~') {
      out += '?';
      in_sequence = byte >= 0xC0;
    } else {
      out += character;
    }
  }
  return out;
}

std::string SoCName() {
  const std::string model = SystemProperty("ro.soc.model");
  std::string name = SystemProperty("ro.soc.manufacturer");
  if (!model.empty()) name += (name.empty() ? "" : " ") + model;
  if (name.empty()) name = SystemProperty("ro.board.platform");
  if (const char *marketing = SocMarketingName(model)) name += std::string(" (") + marketing + ")";
  return CleanDeviceName(name);
}

std::vector<std::string> FitName(const std::string &raw, int columns) {
  std::vector<std::string> lines;
  const std::string name = CleanDeviceName(raw);
  if (name.empty() || columns < 1) return lines;
  const std::size_t width = static_cast<std::size_t>(columns);
  std::string current;
  std::size_t start = 0;
  while (start < name.size()) {
    std::size_t end = name.find(' ', start);
    if (end == std::string::npos) end = name.size();
    std::string word = name.substr(start, end - start);
    start = end + 1;
    while (word.size() > width) {
      if (!current.empty()) {
        lines.push_back(current);
        current.clear();
      }
      lines.push_back(word.substr(0, width));
      word.erase(0, width);
    }
    if (current.empty()) {
      current = word;
    } else if (current.size() + 1 + word.size() <= width) {
      current += ' ';
      current += word;
    } else {
      lines.push_back(current);
      current = word;
    }
  }
  if (!current.empty()) lines.push_back(current);
  return lines;
}

std::string Formatted(const char *pattern, double a, double b = 0.0, double c = 0.0) {
  char text[64];
  std::snprintf(text, sizeof text, pattern, a, b, c);
  return text;
}

std::string PercentText(double percent) { return percent < 0.0 ? std::string("  n/a") : Formatted("%5.1f%%", percent); }

std::string MemoryText(double mb) {
  if (mb < 0.0) return "--";
  return mb >= 1024.0 ? Formatted("%.2f GB", mb / 1024.0) : Formatted("%.0f MB", mb);
}

// The refresh rate the activity puts in the environment ("GT4_ANDROID_DISPLAY=3088x1440@120"); 0 when not there.
std::uint32_t RefreshFromEnvironment() {
  const char *text = std::getenv("GT4_ANDROID_DISPLAY");
  if (text == nullptr) return 0;
  const char *at = std::strrchr(text, '@');
  if (at == nullptr) return 0;
  const long hz = std::strtol(at + 1, nullptr, 10);
  return hz > 0 && hz <= 1000 ? static_cast<std::uint32_t>(hz) : 0;
}

struct Segment {
  std::string text;
  ImU32 colour = kWhite;
};

struct HudLine {
  std::vector<Segment> left;
  Segment right;
};

// CoreGS 2.0's hud_lines, with the renderer named for this build.
std::vector<HudLine> BuildLines(int mode, const std::string &gpu_name, const std::string &cpu_name, const std::string &scale,
                                std::uint32_t width, std::uint32_t height, std::uint32_t refresh_hz, const FrameWindow &frames,
                                const SystemReading &system, bool report_enabled) {
  std::vector<HudLine> lines;
  if (mode == 0) return lines;
  if (mode == 1) {
    const std::string text = frames.intervals == 0 ? std::string("  -- FPS") : Formatted("%5.1f FPS", frames.fps);
    lines.push_back(HudLine{{Segment{text, kWhite}}, Segment{}});
    return lines;
  }

  for (const std::string &part : FitName(gpu_name, kColumns)) lines.push_back(HudLine{{Segment{part, kWhite}}, Segment{}});
  for (const std::string &part : FitName(cpu_name, kColumns)) lines.push_back(HudLine{{Segment{part, kWhite}}, Segment{}});

  char size[32];
  std::snprintf(size, sizeof size, "[%ux%u]", width, height);
  std::string output = size;
  if (refresh_hz != 0) output += " " + std::to_string(refresh_hz) + "Hz";
  std::string renderer = "GSdx (ARMSX2)";
  if (!scale.empty()) renderer += "  " + scale + "x";
  lines.push_back(HudLine{{Segment{renderer, kWhite}}, Segment{output, kDim}});

  if (frames.intervals == 0) {
    lines.push_back(HudLine{{Segment{"FPS:      --", kWhite}}, Segment{}});
    lines.push_back(HudLine{{Segment{"Frame:    --", kBlue}}, Segment{}});
  } else {
    lines.push_back(HudLine{{Segment{"FPS:   " + Formatted("%6.2f", frames.fps) + " [", kWhite},
                             Segment{Formatted("%6.2f", frames.fps_low), kRed},
                             Segment{Formatted(" %6.2f", frames.fps_high) + "]", kWhite}},
                            Segment{}});
    lines.push_back(HudLine{{Segment{"Frame: " + Formatted("%6.2f [%6.2f", frames.frame_ms, frames.frame_ms_low), kBlue},
                             Segment{Formatted(" %6.2f", frames.frame_ms_high), kRed},
                             Segment{"] ms", kBlue}},
                            Segment{}});
  }
  lines.push_back(HudLine{{Segment{"CPU:   " + PercentText(system.cpu_percent), kGreen}}, Segment{}});
  lines.push_back(HudLine{{Segment{"GPU:   " + PercentText(system.gpu_percent), kGreen}}, Segment{}});
  lines.push_back(HudLine{{Segment{"Mem:   " + MemoryText(system.memory_mb), kWhite}},
                          Segment{"VRAM: " + MemoryText(system.video_memory_mb), kWhite}});
  // The game's F7/F8 report state lives in the game library; this library only knows that the launcher turned it on
  // (GT4_PERF). The three states are drawn as CoreGS draws them.
  if (report_enabled) lines.push_back(HudLine{{Segment{"Report:", kWhite}, Segment{" Idle", kDim}}, Segment{}});
  return lines;
}

std::string ScaleText() {
  const float scale = GSConfig.UpscaleMultiplier;
  if (scale <= 0.0f) return std::string();
  char text[32];
  std::snprintf(text, sizeof text, "%g", static_cast<double>(scale));
  return text;
}

bool ReportEnabled() {
  const char *text = std::getenv("GT4_PERF");
  return text != nullptr && *text != '\0' && std::strcmp(text, "0") != 0 && std::strcmp(text, "off") != 0;
}

// One glyph column: the advance of a monospace cell at this size.
float CellWidth(ImFont *font, float size) {
  const ImVec2 advance = font->CalcTextSizeA(size, std::numeric_limits<float>::max(), 0.0f, "M");
  return std::max(1.0f, std::ceil(advance.x));
}

}  // namespace

void SetHudMode(int mode) { s_mode = std::clamp(mode, 0, 2); }

int HudMode() { return s_mode; }

void DrawHud() {
  if (s_mode == 0) return;
  ImDrawList *draw = ImGui::GetBackgroundDrawList();
  if (draw == nullptr) return;
  ImFont *font = ImGuiManager::GetFixedFont();
  if (font == nullptr) font = ImGui::GetIO().FontDefault;
  if (font == nullptr) return;

  const ImGuiIO &io = ImGui::GetIO();
  const float height = io.DisplaySize.y > 0.0f ? io.DisplaySize.y : 1080.0f;
  const float width = io.DisplaySize.x > 0.0f ? io.DisplaySize.x : 1920.0f;
  const float size = std::clamp(std::round(height / 1080.0f * 16.0f), 12.0f, 44.0f);
  // CoreGS's font is the system monospace face at `size` px, whose cell is 19 px high at 16 (ascent + descent, the GDI
  // tmHeight rule) and 10 px wide. The bundled RobotoMono-Medium is the same face the PCSX2 ImGui uses; its line box is
  // size * 19/16 and its cell advance is the one below.
  const float line = std::ceil(size * (19.0f / 16.0f));
  const float pad = std::max(4.0f, std::floor(line * 0.5f));
  const float cell = CellWidth(font, size);
  const int columns = s_mode == 1 ? kFpsColumns : kColumns;
  const bool with_graph = s_mode == 2;
  const float panel_width = columns * cell + 2.0f * pad;

  // The panel's own pixel clock: the graph on every presentation, the text twice a second.
  const double now = Common::Timer::ConvertValueToSeconds(Common::Timer::GetCurrentValue()) * 1000.0;
  s_times.Note(now);

  static double s_next_text_ms = 0.0;
  static std::vector<HudLine> s_lines;
  static SystemReading s_reading;
  const bool text_due = now >= s_next_text_ms;
  if (text_due) {
    s_cpu.Update();
    s_reading.cpu_percent = s_cpu.percent;
    s_reading.gpu_percent = GpuBusyPercent();
    s_reading.memory_mb = MemoryMb();
    s_reading.video_memory_mb = -1.0;
    s_reading.refresh_hz = RefreshFromEnvironment();
    s_lines = BuildLines(s_mode, g_gs_device != nullptr ? g_gs_device->GetName() : std::string(), SoCName(), ScaleText(),
                         static_cast<std::uint32_t>(io.DisplaySize.x), static_cast<std::uint32_t>(io.DisplaySize.y),
                         s_reading.refresh_hz, s_times.Window(now), s_reading, ReportEnabled());
    s_next_text_ms = now + kTextPeriodMs;
  }

  // The panel was made for this many text lines; the number depends on the names and the mode, never on the readings.
  const float text_lines = std::max<std::size_t>(1, s_lines.size());
  const float graph_size = with_graph ? kGraphRows * line : 0.0f;
  const float text_top = pad;
  const float graph_top = pad + text_lines * line + (with_graph ? pad * 0.5f : 0.0f);
  const float panel_height = graph_top + graph_size + pad;

  const float left = std::max(0.0f, width - panel_width - static_cast<float>(kMarginPixels));
  const float top = std::min(static_cast<float>(kMarginPixels), std::max(0.0f, height - panel_height));
  draw->AddRectFilled(ImVec2(left, top), ImVec2(left + panel_width, top + panel_height), kPanel, pad);

  // The whole panel is redrawn on every frame (ImGui has no retained draw list); the lines themselves change twice a
  // second, exactly as CoreGS redraws its canvas.
  {
    float y = top + text_top;
    for (const HudLine &hud_line : s_lines) {
      float x = left + pad;
      for (const Segment &segment : hud_line.left) {
        draw->AddText(font, size, ImVec2(x, y), segment.colour, segment.text.c_str());
        x += static_cast<float>(segment.text.size()) * cell;
      }
      if (!hud_line.right.text.empty()) {
        const float right = left + panel_width - pad - static_cast<float>(hud_line.right.text.size()) * cell;
        draw->AddText(font, size, ImVec2(right, y), hud_line.right.colour, hud_line.right.text.c_str());
      }
      y += line;
    }
  }

  // The frame-time graph: one column per interval, newest on the right; guides at one and two 60 Hz frames.
  if (with_graph) {
    const float graph_left = left + pad;
    const float graph_width = panel_width - 2.0f * pad;
    const int graph_columns = std::max(1, static_cast<int>(graph_width / 1.0f));
    std::vector<float> intervals;
    s_times.RecentIntervals(intervals, static_cast<std::size_t>(graph_columns));
    const auto row_of = [&](double ms) {
      const double shown = std::clamp(ms, 0.0, kGraphTopMs);
      const int row = static_cast<int>((1.0 - shown / kGraphTopMs) * (graph_size - 1.0f) + 0.5f);
      return top + graph_top + static_cast<float>(row);
    };
    for (const double guide : {kOneFrameMs, 2.0 * kOneFrameMs, 0.0}) {
      const float y = row_of(guide);
      draw->AddRectFilled(ImVec2(graph_left, y), ImVec2(graph_left + graph_width, y + 1.0f), kGuide);
    }
    const int count = std::min(static_cast<int>(intervals.size()), graph_columns);
    const std::size_t first = intervals.size() - static_cast<std::size_t>(count);
    const float thickness = std::max(1.0f, line / 12.0f);
    int previous_row = -1;
    for (int i = 0; i < count; ++i) {
      const double ms = intervals[first + static_cast<std::size_t>(i)];
      const float x = graph_left + graph_width - static_cast<float>(count - i);
      const int row = static_cast<int>(row_of(ms) - (top + graph_top));
      const int from = previous_row < 0 ? row : std::min(previous_row, row);
      const int to = previous_row < 0 ? row : std::max(previous_row, row);
      const ImU32 colour = ms > 2.0 * kOneFrameMs ? kRed : kBlue;
      draw->AddRectFilled(ImVec2(x, top + graph_top + static_cast<float>(from)),
                          ImVec2(x + 1.0f, top + graph_top + static_cast<float>(to) + thickness), colour);
      previous_row = row;
    }
  }
}

}  // namespace gt4gs

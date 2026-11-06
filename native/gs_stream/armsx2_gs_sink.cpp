// SPDX-License-Identifier: MIT
// GT4Recomp private Android test build: the dynamic loader and sink table for libgt4gs.so (GPL, ARMSX2's GS).
//
// Nothing here knows ARMSX2 or PCSX2: only the flat C ABI of android/armsx2gs/gt4gs_api.h. See armsx2_gs_sink.hpp.

#include "armsx2_gs_sink.hpp"

#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace gt4::gs_stream {
namespace {

constexpr const char *kLibraryName = "libgt4gs.so";

// The subset of gt4gs_api.h this side calls. Duplicated rather than included so the game build never needs the GPL
// header on its include path (and never picks up the GPL library at link time).
using StartFn = int (*)(const void *info);
using VoidVoid = void (*)();
using AttachFn = void (*)(void *native_window, std::uint32_t width, std::uint32_t height);
using ResizeFn = void (*)(std::uint32_t width, std::uint32_t height);
using GifFn = void (*)(int path, const void *data, std::uint32_t bytes);
using PrivFn = void (*)(std::uint32_t address, std::uint64_t value);
using VsyncFn = void (*)(int odd_field);
using ReadFifoFn = int (*)(void *data, std::uint32_t qwords);
using ResetFn = void (*)(int kind);
using AspectFn = void (*)(int fill, std::uint32_t ratio_millionths);
using TextFn = const char *(*)();
using IntFn = int (*)();

// Layout of Gt4gsStartInfo in gt4gs_api.h (pointer-sized fields only, checked by the ABI version at load).
struct StartInfo {
  const char *data_root;
  const char *config_path;
  const char *overrides_path;
  void *native_window;
  std::uint32_t surface_width;
  std::uint32_t surface_height;
  const char *serial;
  int preferred_backend;
  int log_performance;
  int hud_mode;
};

struct Library {
  void *handle = nullptr;
  StartFn start = nullptr;
  AttachFn attach_window = nullptr;
  ResizeFn resize = nullptr;
  VoidVoid detach_window = nullptr;
  GifFn gif = nullptr;
  PrivFn privileged = nullptr;
  VsyncFn vsync = nullptr;
  ReadFifoFn read_fifo = nullptr;
  ResetFn reset = nullptr;
  AspectFn display_aspect = nullptr;
  VoidVoid finish = nullptr;
  TextFn last_error = nullptr;
  TextFn version = nullptr;
  TextFn renderer_name = nullptr;
  IntFn has_vulkan = nullptr;
  bool usable = false;
  std::string error;
};

Library &library() {
  static Library library = []() -> Library {
    Library out;
    const char *explicit_path = std::getenv("GT4_ARMSX2_GS_LIBRARY");
    const char *path = (explicit_path != nullptr && *explicit_path != '\0') ? explicit_path : kLibraryName;
    out.handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (out.handle == nullptr) {
      const char *why = dlerror();
      out.error = std::string("cannot load ") + path + ": " + (why != nullptr ? why : "unknown error");
      return out;
    }
    const auto resolve = [&out](const char *name) { return dlsym(out.handle, name); };
    out.start = reinterpret_cast<StartFn>(resolve("gt4gs_start"));
    out.attach_window = reinterpret_cast<AttachFn>(resolve("gt4gs_attach_window"));
    out.resize = reinterpret_cast<ResizeFn>(resolve("gt4gs_resize"));
    out.detach_window = reinterpret_cast<VoidVoid>(resolve("gt4gs_detach_window"));
    out.gif = reinterpret_cast<GifFn>(resolve("gt4gs_gif"));
    out.privileged = reinterpret_cast<PrivFn>(resolve("gt4gs_privileged"));
    out.vsync = reinterpret_cast<VsyncFn>(resolve("gt4gs_vsync"));
    out.read_fifo = reinterpret_cast<ReadFifoFn>(resolve("gt4gs_read_fifo"));
    out.reset = reinterpret_cast<ResetFn>(resolve("gt4gs_reset"));
    out.display_aspect = reinterpret_cast<AspectFn>(resolve("gt4gs_display_aspect"));
    out.finish = reinterpret_cast<VoidVoid>(resolve("gt4gs_finish"));
    out.last_error = reinterpret_cast<TextFn>(resolve("gt4gs_last_error"));
    out.version = reinterpret_cast<TextFn>(resolve("gt4gs_version"));
    out.renderer_name = reinterpret_cast<TextFn>(resolve("gt4gs_renderer_name"));
    out.has_vulkan = reinterpret_cast<IntFn>(resolve("gt4gs_has_vulkan"));
    if (out.start == nullptr || out.gif == nullptr || out.vsync == nullptr || out.read_fifo == nullptr ||
        out.privileged == nullptr || out.reset == nullptr || out.attach_window == nullptr) {
      out.error = std::string(path) + " does not export the gt4gs ABI (wrong library or version)";
      return out;
    }
    out.usable = true;
    return out;
  }();
  return library;
}

std::string data_root() {
  const char *value = std::getenv("GT4_ARMSX2_GS_DIR");
  return value != nullptr ? std::string(value) : std::string();
}

std::string config_path() {
  const char *value = std::getenv("GT4_ARMSX2_GS_CONFIG");
  if (value != nullptr && *value != '\0') return std::string(value);
  const std::string root = data_root();
  return root.empty() ? std::string() : root + "/armsx2-gs.ini";
}

std::string overrides_path() {
  const char *value = std::getenv("GT4_ARMSX2_GS_OVERRIDES");
  if (value != nullptr && *value != '\0') return std::string(value);
  const std::string root = data_root();
  return root.empty() ? std::string() : root + "/armsx2-gs-extra.ini";
}

bool start(std::string &error) {
  Library &lib = library();
  if (!lib.usable) {
    error = lib.error;
    return false;
  }
  const std::string root = data_root();
  if (root.empty()) {
    error = "GT4_ARMSX2_GS_DIR is not set (the launcher's ARMSX2 GS settings folder)";
    return false;
  }
  const std::string config = config_path();
  const std::string overrides = overrides_path();
  const char *scale = std::getenv("GT4_ARMSX2_GS_SCALE");
  // 0 OpenGL ES, 1 Vulkan (a build that has it), 2 the deviceless (Null) renderer: no GPU and no picture, for a
  // diagnosis run that still exercises the whole stream, -1 the library's own choice. debug.env sets
  // GT4_ARMSX2_GS_RENDERER=null.
  const char *renderer_choice = std::getenv("GT4_ARMSX2_GS_RENDERER");
  int backend = -1;
  if (renderer_choice != nullptr) {
    if (std::strcmp(renderer_choice, "null") == 0 || std::strcmp(renderer_choice, "sw") == 0 ||
        std::strcmp(renderer_choice, "software") == 0)
      backend = 2;
    else if (std::strcmp(renderer_choice, "ogl") == 0 || std::strcmp(renderer_choice, "gl") == 0) backend = 0;
    else if (std::strcmp(renderer_choice, "vk") == 0 || std::strcmp(renderer_choice, "vulkan") == 0) backend = 1;
  }
  StartInfo info{};
  info.data_root = root.c_str();
  info.config_path = config.c_str();
  info.overrides_path = overrides.c_str();
  info.native_window = nullptr;  // attached by the window host before the first frame begins
  info.surface_width = 0;
  info.surface_height = 0;
  // The game's own serial (SCUS-97328 for v2.00, SCUS-97436 for Spec II): the texture folder and the GameDB fixes follow
  // it. GT4_GAME_SERIAL comes from the launcher's plan; the fallback keeps an older launcher working for v2.00.
  const char *game_serial = std::getenv("GT4_GAME_SERIAL");
  info.serial = (game_serial != nullptr && *game_serial != '\0') ? game_serial : "SCUS-97328";
  info.preferred_backend = backend;
  info.log_performance = std::getenv("GT4_ARMSX2_GS_PERF") != nullptr ? 1 : 0;
  // The launcher's Performance HUD setting: off, frame rate only or the full panel (drawn by the library, never the
  // emulator's own OSD).
  const char *hud = std::getenv("GT4_ARMSX2_GS_HUD");
  info.hud_mode = 0;
  if (hud != nullptr) {
    if (std::strcmp(hud, "fps") == 0) info.hud_mode = 1;
    else if (std::strcmp(hud, "1") == 0 || std::strcmp(hud, "full") == 0) info.hud_mode = 2;
  }
  std::fprintf(stderr, "[gs] ARMSX2 GS in process: %s (%s), settings %s, extra %s, scale %s\n",
               lib.version != nullptr ? lib.version() : kLibraryName,
               lib.renderer_name != nullptr ? lib.renderer_name() : "?", config.c_str(), overrides.c_str(),
               scale != nullptr ? scale : "unset");
  std::fprintf(stderr, "[gs] ARMSX2 GS renderer request: %s\n", renderer_choice != nullptr ? renderer_choice : "auto");
  if (lib.start(&info) != 0) {
    error = std::string("gt4gs_start failed: ") + (lib.last_error != nullptr ? lib.last_error() : "");
    return false;
  }
  return true;
}

void gif(int path, const std::uint8_t *data, std::uint32_t qwc) {
  Library &lib = library();
  if (lib.usable && data != nullptr && qwc != 0) lib.gif(path, data, qwc * 16u);
}

void privileged(std::uint32_t address, std::uint64_t value) {
  Library &lib = library();
  if (lib.usable) lib.privileged(address, value);
}

void read_fifo(std::uint8_t *data, std::uint32_t qwords) {
  Library &lib = library();
  if (lib.usable && data != nullptr) {
    lib.read_fifo(data, qwords);
    return;
  }
  if (data != nullptr) std::memset(data, 0, static_cast<std::size_t>(qwords) * 16u);
}

void vsync(bool odd_field) {
  Library &lib = library();
  if (lib.usable) lib.vsync(odd_field ? 1 : 0);
}

void gs_reset() {
  Library &lib = library();
  if (lib.usable) lib.reset(1);
}

void gif_reset() {
  Library &lib = library();
  if (lib.usable) lib.reset(2);
}

bool attach_window(std::uint64_t window, std::uint32_t width, std::uint32_t height) {
  Library &lib = library();
  if (!lib.usable || window == 0) return false;
  lib.attach_window(reinterpret_cast<void *>(static_cast<std::uintptr_t>(window)), width, height);
  return true;
}

void resize_window(std::uint32_t width, std::uint32_t height) {
  Library &lib = library();
  if (lib.usable && lib.resize != nullptr) lib.resize(width, height);
}

void detach_window() {
  Library &lib = library();
  if (lib.usable && lib.detach_window != nullptr) lib.detach_window();
}

void display_aspect(bool fill, std::uint32_t ratio_millionths) {
  Library &lib = library();
  if (lib.usable && lib.display_aspect != nullptr) lib.display_aspect(fill ? 1 : 0, ratio_millionths);
}

void finish(std::FILE *output) {
  Library &lib = library();
  if (!lib.usable) return;
  if (output != nullptr) {
    std::fprintf(output, "[gs] ARMSX2 GS in process: %s, %s\n", lib.version != nullptr ? lib.version() : kLibraryName,
                 lib.renderer_name != nullptr ? lib.renderer_name() : "?");
  }
  lib.finish();
}

InProcessSink make_sink() {
  InProcessSink sink;
  sink.start = &start;
  sink.gif = &gif;
  sink.privileged = &privileged;
  sink.read_fifo = &read_fifo;
  sink.vsync = &vsync;
  sink.gs_reset = &gs_reset;
  sink.gif_reset = &gif_reset;
  sink.attach_window = &attach_window;
  sink.resize_window = &resize_window;
  sink.display_aspect = &display_aspect;
  sink.finish = &finish;
#ifdef __ANDROID__
  sink.detach_window = &detach_window;
#endif
  sink.mtvu_job = nullptr;  // no direct GS intake: the records are copied into the queue
  return sink;
}

}  // namespace

const InProcessSink &armsx2_in_process_sink() {
  static const InProcessSink sink = make_sink();
  return sink;
}

bool armsx2_available(std::string &error) {
  Library &lib = library();
  error = lib.error;
  return lib.usable;
}

}  // namespace gt4::gs_stream

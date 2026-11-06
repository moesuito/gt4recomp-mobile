// SPDX-License-Identifier: GPL-3.0+
//
// GT4Recomp private Android test build (owner's phone only; PCSX2-tainted, GPL). NOT part of any GT4Recomp release and
// never distributed.
//
// C ABI between the game library (libgt4.so, MIT side) and the in-process ARMSX2 GS library (libgt4gs.so, GPL side).
// The two live in the same Android process; there is no IPC. The game calls these functions on its EE thread at the
// same points where the Windows build posts GS stream protocol records, and libgt4gs executes them on the ARMSX2 GS.
//
// Everything is behaviour-compatible with the GS stream protocol of the game repository (docs/GS-STREAM-PROTOCOL.md):
// GIF chunks per path in acceptance order, privileged register writes, the VSYNC field, local-to-host FIFO reads,
// resets and the display aspect. The C ABI is flat here because both sides share one thread boundary, not a ring.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GT4GS_ABI_VERSION 1

// The library is built with -fvisibility=hidden (PCSX2's own flags); the ABI entry points must stay exported or the game
// cannot resolve a single one of them.
#if defined(__GNUC__) || defined(__clang__)
#define GT4GS_PUBLIC __attribute__((visibility("default")))
#else
#define GT4GS_PUBLIC
#endif
#define GT4GS_MAX_PATH 260

// What the library needs before the first record. All strings are UTF-8 and NULL-terminated; the pointers only need to
// stay valid for the call (the library copies them).
typedef struct Gt4gsStartInfo {
  // Writable directory: cache/ (shader and pipeline caches), textures/, resources/ (GameIndex.yaml and the GL shaders
  // extracted from the APK), logs/. Created if missing.
  const char *data_root;
  // The base GS settings file (PCSX2 INI syntax, section [EmuCore/GS]); may be NULL for the built-in defaults.
  const char *config_path;
  // The owner's own extra lines, applied on top of the base file; may be NULL.
  const char *overrides_path;
  // ANativeWindow* of the game's window; may be NULL (the GS opens surfaceless until gt4gs_attach_window).
  void *native_window;
  uint32_t surface_width;
  uint32_t surface_height;
  // Game serial for the GameDB lookup ("SCUS-97328"); may be NULL.
  const char *serial;
  // 1 keeps the Vulkan back end when the library has it, 0 forces OpenGL ES, 2 forces the deviceless (Null) renderer
  // (no GPU context and no picture: a diagnosis path that still runs the whole stream), -1 lets the library choose
  // (Vulkan first, then OpenGL ES). A build without Vulkan (this one) runs OpenGL ES unless 2 is given.
  int preferred_backend;
  // 1 asks for the performance numbers in the log every 300 VSYNCs (the game's own F8 report stays separate).
  int log_performance;
} Gt4gsStartInfo;

// Starts the GS thread and boots the GS core. Returns 0 on success; on failure the message is in gt4gs_last_error().
GT4GS_PUBLIC int gt4gs_start(const Gt4gsStartInfo *info);

// The game's window (ANativeWindow*) and its pixel size. Safe from the window thread: it only stores the handle and
// asks the GS thread to rebuild its surface.
GT4GS_PUBLIC void gt4gs_attach_window(void *native_window, uint32_t width, uint32_t height);
GT4GS_PUBLIC void gt4gs_resize(uint32_t width, uint32_t height);
GT4GS_PUBLIC void gt4gs_detach_window(void);

// One accepted GIF chunk of `path` (1 = PATH1/VU1, 2 = PATH2/VIF1, 3 = PATH3/GIF), `bytes` of GIF data (a multiple of
// 16: tags and data exactly as the GIF received them). Called on the EE thread in stream order.
GT4GS_PUBLIC void gt4gs_gif(int path, const void *data, uint32_t bytes);
// One 64-bit privileged GS register write, address 0x12000000-0x12001FF8.
GT4GS_PUBLIC void gt4gs_privileged(uint32_t address, uint64_t value);
// Start of the vertical blank; odd_field is the CSR FIELD bit the EE now reads (the display field of the frame that
// ends here). Blocks while too many frames are in flight, as the Windows stream does.
GT4GS_PUBLIC void gt4gs_vsync(int odd_field);
// Local-to-host FIFO read of `qwords` quadwords: fills `data` (always) and returns 1 when the GS answered.
GT4GS_PUBLIC int gt4gs_read_fifo(void *data, uint32_t qwords);
// kind 1: GS hard reset (local memory cleared); kind 2: GIF path parsers back to "waiting for a tag".
GT4GS_PUBLIC void gt4gs_reset(int kind);
// Protocol 1.3 display aspect: fill = 1 stretches the picture over the whole window, 0 keeps the game aspect
// (pillarbox/letterbox). ratio_millionths is the target aspect x 1e6 for fill (0 = the whole window).
GT4GS_PUBLIC void gt4gs_display_aspect(int fill, uint32_t ratio_millionths);

// End of the session: waits for the GS thread, prints the summary, frees the device. Safe to call twice.
GT4GS_PUBLIC void gt4gs_finish(void);

// Last start-up or fatal error text ("" when none). Never NULL.
GT4GS_PUBLIC const char *gt4gs_last_error(void);
// "gt4gs <version> (<renderer>)" of the library.
GT4GS_PUBLIC const char *gt4gs_version(void);
// "OpenGL" or "Vulkan": the back end the library was built with (before start) or chose (after start).
GT4GS_PUBLIC const char *gt4gs_renderer_name(void);
// 1 when the library has the Vulkan back end compiled in.
GT4GS_PUBLIC int gt4gs_has_vulkan(void);

#ifdef __cplusplus
}  // extern "C"
#endif

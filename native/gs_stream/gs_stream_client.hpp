// SPDX-License-Identifier: MIT
// Game-side GS stream backend (TASK-035; docs/GS-STREAM-PROTOCOL.md section 7, docs/GSDX-PLAN.md section C.3).
//
// Renderer selection, GT4_RENDERER:
//   unset or "donor"  the donor GS front end and the native Vulkan renderer (unchanged; every hook returns at once)
//   "pcsx2"           the GS stream: no donor Vulkan device; GIF, privileged writes, VSYNC and readbacks go to a GS
//                     process (GT4_GS_PROCESS=<executable>) over shared memory (gs_stream_ipc.hpp)
//   "hw"              TASK-027 development builds only (CMake GT4_HW_GS=ON, compile definition GT4_GS_STREAM_HW): no
//                     donor Vulkan device and no GS process; the same hooks feed the in-process HWR backend
//                     (gs_stream_sink.hpp). GT4_GS_CAPTURE is refused in this mode. Other builds refuse "hw".
//   "armsx2"          PRIVATE Android test build only (compile definition GT4_GS_STREAM_ARMSX2): the same in-process
//                     sink, filled by libgt4gs.so, which runs the ARMSX2 (GPL) GS in this process. Never shipped.
// Any other value stops the process before the game starts.
//
// TASK-036 (slice193), pcsx2 mode: GT4_GS_PROCESS_SHA256=<hex> refuses a GS process whose SHA-256 differs (the launcher
// sets it from the pinned manifest); GT4_GS_CONFIG=<ini> is passed to the GS process as --config. A GS process that
// dies during the session halts the game through the normal exit path; with a window, a message box explains it.
//
// TASK-037/038 (slice194): F8 prints the readback wait distribution; a GS process death lets the guest run
// GT4_GS_LOST_GRACE_FRAMES (default 180) more VSYNCs so the runtime flushes a changed memory card, then halts; with a
// window, a message box offers CoreGS 2.0 (the launcher reads the restart line from the log);
// GT4_GS_HANDSHAKE_TIMEOUT_MS (default 20000) bounds the handshake; 0x0 resizes (minimized window) are not sent.
//
// GT4_GS_CAPTURE=<file> writes the same records to a .gscap version 2 file in either renderer mode (the donor output is
// unchanged). GT4_GS_CAPTURE_FRAMES=first:last limits it to VSYNC frames first..last.
//
// The hooks are called by the spliced donor sources (scripts/prepare_external_vulkan.py --gs-stream-hooks for gs.cpp,
// scripts/prepare_perf_runtime.py --gs-stream for gif.cpp and hw.cpp), always on the EE thread.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "gs_stream/gs_stream_protocol.h"

namespace gt4::mtvu {
class SpscByteRing;
class Doorbell;
}  // namespace gt4::mtvu

namespace gt4::gs_stream {

enum class Renderer { donor, pcsx2, hw, armsx2 };

// Parsed once from GT4_RENDERER. An unknown value prints a message and exits with EXIT_FAILURE.
Renderer renderer();
// True when GT4_RENDERER=pcsx2 (or hw in a GT4_GS_STREAM_HW build): the donor GS front end and Vulkan renderer are
// not used.
bool stream_renderer();

// --- hooks (EE thread) ---
// One accepted GIF chunk of `path`, in acceptance order (the gif render point, after the MTVU journal drain).
// Returns stream_renderer(): when true the caller does not run the donor GS front end for this chunk.
bool on_gif_render(int path, const std::uint8_t *data, std::uint32_t qwc);
// Privileged GS register write (the donor keeps its own copy for CSR/IMR/SIGLBLID reads).
void on_privileged_write(std::uint32_t address, std::uint64_t value);

// TASK-041 (slice198): ordering of privileged writes with the VU1 worker (MTVU). The game's privileged writes reach the
// GS process only through the stream, so they need only their stream position, not an idle worker. The MTVU bridge
// installs these hooks in pcsx2 mode (native/mtvu_bridge.cpp):
//   queue        true when the write was queued behind outstanding worker output (the MTVU journal posts it later,
//                in order, through post_privileged_now); false: the client posts it now. Null: always post now.
//   take_wait_us the EE wait of the blocking MTVU sync that preceded this write (0 when none); per-register F8 lines.
//   active       true when privileged writes skip the blocking MTVU sync (GT4_MTVU_PRIV_ASYNC on, worker running).
struct PrivilegedOrdering {
  bool (*queue)(std::uint32_t address, std::uint64_t value) = nullptr;
  std::uint64_t (*take_wait_us)() = nullptr;
  bool (*active)() = nullptr;
};
void set_privileged_ordering(const PrivilegedOrdering &ordering);
// Posts one privileged write to the stream now (the MTVU journal drain calls it for queued writes).
void post_privileged_now(std::uint32_t address, std::uint64_t value);
// Index of a privileged register for the per-register counters: 0-14 = PMODE..BGCOLOR (0x12000000-0x120000E0), 15 CSR,
// 16 IMR, 17 BUSDIR, 18 SIGLBLID, 19 any other address. privileged_register_name(index) is the GS manual name.
constexpr int kPrivilegedRegisterSlots = 20;
int privileged_register_index(std::uint32_t address);
const char *privileged_register_name(int index);
// Local-to-host FIFO read. Returns true when the stream renderer answered it (data filled); false in donor mode,
// where the capture only records the request and the donor serves the data.
bool read_fifo(std::uint8_t *data, std::uint32_t qwords);
// TASK-116 step 0 (P0 gate): the guest RAM window used to report a READ_FIFO destination as an EE address in the
// GT4_GS_READBACK_LOG lines. native/game_main.cpp sets it once from ps2::g_ram; unset, the destination is reported as 0.
// A probe aid only: read_fifo itself does not look at it.
void set_guest_ram(std::uint8_t *base, std::uint32_t size);

// TASK-116 step 1 (rc6): the flare readback answered locally in Performance mode (gt4-gs HWDownloadMode 2). The game
// must not wait for the GS process to render and download the per-frame 2 KiB flare visibility table: only that
// readback (128 qwords, a 64x8 PSMCT32 buffer) is answered with the previous reply, one frame old, and its READ_FIFO is
// queued behind the MTVU journal, so the GS process still receives it at its stream position (the stream hash does not
// change). Every other readback (car icons, loading picture, thumbnail, photo) keeps the blocking drain and the round
// trip, and so does the first flare readback of a scene. GT4_GS_READBACK_LOCAL=0 turns the local answer off (A/B);
// GT4_GS_READBACK_LOG=1 logs every reply. The journal services come from mtvu_bridge.cpp (pcsx2 mode only): with none
// registered, every readback is served exactly as before.
struct ReadbackJournal {
  // EE thread: queues a READ_FIFO behind the journal entries already queued. False: nothing was queued, and the
  // caller drains and waits for an accurate reply.
  bool (*queue_readback)(std::uint32_t qwords) = nullptr;
  // EE thread: the blocking drain the accurate path needs (the deferred sync("download")).
  void (*drain_download)() = nullptr;
};
void set_readback_journal(const ReadbackJournal &journal);
// True when the flare readback is answered locally: the GS stream runs, Performance mode is on, GT4_GS_READBACK_LOCAL is
// not 0, and the journal is registered. Read by the MTVU download ordering to defer sync("download").
bool readback_local_active();
// Posts the queued READ_FIFO without waiting for the reply; called by the MTVU journal drain, in stream order.
void post_read_fifo_nowait(std::uint32_t qwords);
// Start of the vertical blank, after the donor toggled the field: odd_field is the CSR FIELD bit the EE now reads.
// The donor toggles the field together with INTC VBLANK_S, before the game's VBLANK_S handler runs, so this is also
// the display field of the frame that ends here (protocol 1.1 VSYNC.field; docs/GS-STREAM-PROTOCOL.md 6.1).
void on_vsync(bool odd_field);
// TASK-084 step 3 (slice263): VBlank as an MTVU journal entry. With deferral on, on_vsync() holds the field instead
// of posting; post_pending_vsync() (called from the journal drain, EE thread) posts it after the frame's GIF output,
// so the GS process sees the frame complete before its VSYNC. Without deferral both calls behave as before.
void set_vsync_deferral(bool defer);
bool post_pending_vsync();
// IN-461 (CPU survey T3, direct GS intake): with GT4_RENDERER=hw the in-process GS worker can read the VU1 worker's
// output ring itself. mtvu_direct_available() says whether it can (asked once by the MTVU runtime at start);
// on_mtvu_job() puts VU1 job `job` at this point of the GS stream (EE thread, the job's program position): the GS worker
// then executes that job's GIF records straight from `output`, waiting on `bell`, up to its JobDone.
bool mtvu_direct_available();
void on_mtvu_job(std::uint64_t job, gt4::mtvu::SpscByteRing &output, gt4::mtvu::Doorbell &bell);
// GS hard reset (donor render_reset) and GIF path reset.
void on_gs_reset();
void on_gif_reset();
// BIOS SetGsCrt: posts the SMODE1/SMODE2 writes the BIOS performs, and SYNCV for the analog NTSC/PAL modes (the
// donor HLE only records the arguments).
void on_set_gs_crt(std::uint32_t interlace, std::uint32_t mode, std::uint32_t field);
// Window: the GS process renders into a child of this window (CONTROL attach_window / resize).
bool attach_window(std::uint64_t window, std::uint32_t width, std::uint32_t height);
#ifdef __ANDROID__
// TASK-180: the in-process renderer lets go of the window's surface (the game goes to the background).
void detach_window();
#endif
// TASK-060 (protocol 1.2): the mouse cursor policy the game window uses (GT4_CURSOR_AUTOHIDE, native/cursor_autohide.hpp).
// Call before attach_window; attach_window then sends CONTROL cursor to a 1.2 consumer, so the GS child window hides the
// cursor over the picture with the same rule. Default: auto-hide on, 3000 ms.
void set_cursor_policy(bool auto_hide, std::uint32_t hide_delay_ms);
// TASK-080 (protocol 1.3): how the GS process fits the picture into the window: the game's aspect (pillarbox, the
// default) or fill (stretched to the window, the race view at a display ratio other than 16:9). Any thread; the client
// posts CONTROL display_aspect after the next VSYNC when the mode changed, and only to a consumer whose READY minor is
// 3 or more (a 1.2 consumer is never sent it and keeps the game's aspect).
//
// P2 (slice217): with fill, `fill_aspect_millionths` = the target aspect (width / height) x 1e6 of the rectangle the
// picture fills: the largest centred rectangle of that aspect inside the window (letterbox or pillarbox bars), so a
// custom ratio wider or narrower than the window is never stretched to the window. 0 = the whole window. It is sent as
// args[1] of the record (GS-STREAM-PROTOCOL 6.7); a consumer from before P2 (gt4-gs 1.1.0) ignores it and fills the
// whole window. A change of the target alone also posts a record.
enum class DisplayAspectMode : std::uint32_t { game_aspect = 0, fill = 1 };
void set_display_aspect_mode(DisplayAspectMode mode, std::uint32_t fill_aspect_millionths = 0);
// The protocol value of a ratio: round(ratio x 1e6), or 0 (the whole window) outside kMinFillAspect..kMaxFillAspect.
inline constexpr double kMinFillAspect = 0.5;
inline constexpr double kMaxFillAspect = 8.0;
std::uint32_t aspect_millionths(double ratio);
// TASK-062 (protocol 1.2): cumulative busy time of the VU1 worker in microseconds (installed by the MTVU bridge in
// pcsx2 mode); read for the OSD_STATS records. Null: the worker field is not sent.
void set_worker_busy_source(std::uint64_t (*worker_busy_us)());
void resize_window(std::uint32_t width, std::uint32_t height);
// Called when the GS process dies, with a reason (the spliced gs.cpp passes ps2::halt).
void set_halt_handler(void (*halt)(const char *reason));
// End of the run (donor gs::stats): CONTROL shutdown, wait for the GS process, print the session summary.
void finish(std::FILE *output);

// TASK-041: the F8 one-line summary of the EE critical path against the frame budget (all values ms per frame).
// The EE critical path is the EE thread's measured CPU time plus the time it blocks for the VU1 worker (MTVU syncs),
// for readback replies and for the GS process (ring space, frames in flight). Spin time inside those waits is also
// CPU time, so the sum may count a few microseconds twice; the pacer's sleep is in neither.
struct FrameCosts {
  // TASK-084 step 0 (slice260): the EE frame's wall time and the pacer's sleep inside it. The critical path is
  // frame_wall_ms - pacing_sleep_ms; the waits below are already inside that wall time.
  double frame_wall_ms = -1.0;      // wall ms per VBlank of the interval; negative: Unknown
  double pacing_sleep_ms = 0.0;     // pacer sleep inside frame_wall_ms (pacing_wait category)
  double ee_cpu_ms = -1.0;          // measured EE thread CPU; negative: Unknown
  double vu1_worker_wait_ms = 0.0;  // MTVU syncs, every reason
  double readback_wait_ms = 0.0;
  double backpressure_ms = 0.0;     // GS ring space and frames in flight
  double gs_cpu_thread_ms = -1.0;   // GS process CPU thread (STATS of the interval); negative: Unknown
  double budget_ms = 1000.0 / 59.94;
};
std::string format_frame_summary(const FrameCosts &costs);

// TASK-062 (protocol 1.2): OSD_STATS for a consumer that asked for them (READY kReadyFlagOsdStats). The client sends one
// record about every kOsdStatsIntervalUs of wall time, right after a VSYNC. OsdSample holds cumulative counters taken at
// the VSYNC; make_osd_stats turns two samples into the record (times per frame, microseconds; pure, tested by the
// client fixture).
constexpr std::uint64_t kOsdStatsIntervalUs = 500000;
struct OsdSample {
  std::uint64_t wall_us = 0;
  std::uint64_t vsyncs = 0;
  std::uint64_t main_cpu_us = UINT64_MAX;     // EE thread CPU; UINT64_MAX: unknown
  std::uint64_t worker_busy_us = UINT64_MAX;  // VU1 worker busy; UINT64_MAX: unknown
  bool accounting = false;  // the perf accounting (GT4_PERF=1 or F7) is on: the four waits below are measured
  std::uint64_t stream_write_us = 0, readback_wait_us = 0, backpressure_us = 0, worker_wait_us = 0;
  // TASK-084 step 0: the pacer's sleep, so the critical path is the frame wall minus the sleep (the F8 formula).
  std::uint64_t pacing_sleep_us = 0;
};
::gs_stream::OsdStatsPayload make_osd_stats(const OsdSample &begin, const OsdSample &end, std::uint64_t max_frame_us,
                                            double budget_ms = 1000.0 / 59.94);

// SHA-256 of a file as 64 lowercase hex digits (Windows CNG). Used for the GS process pin (GT4_GS_PROCESS_SHA256).
bool file_sha256(const std::wstring &path, std::string &hex, std::string &error);

// SetGsCrt (interlace, mode, field) to the SMODE1/SMODE2 values the BIOS writes; false for an unknown mode.
// Same table as the TASK-012 seam (native/gs_video_mode.cpp on task/012-parallel-gs).
bool map_set_gs_crt(std::uint32_t interlace, std::uint32_t mode, std::uint32_t field, std::uint64_t &smode1,
                    std::uint64_t &smode2);

// SetGsCrt mode to the SYNCV value (GS privileged register 0x12000060) of the analog modes: NTSC (0x02) and PAL
// (0x03); false for the other modes (no SYNCV posted). The values are the vertical timings of the GS User's Manual
// (NTSC: VS 6, VDP 480, VBPE 6, VBP 26, VFPE 6, VFP 1; PAL: VS 5, VDP 576, VBPE 5, VBP 33, VFPE 5, VFP 1). A consumer
// needs them: SYNCV VFP bit 0 with a non-zero SMODE1 CMOD is how a GS knows the output really alternates fields.
bool map_set_gs_crt_syncv(std::uint32_t mode, std::uint64_t &syncv);

}  // namespace gt4::gs_stream

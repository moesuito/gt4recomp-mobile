// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from the TASK-026 specification (docs/hw-renderer-spec), clean room per decision 0070.
// Chapter 12.2/12.3 hookup on main's TASK-035 seam: GT4_RENDERER=hw feeds the GS stream hooks to the HWR backend
// in the game process (native/gs_stream/gs_stream_sink.hpp). Replaces the M0b gs.cpp splice of task/027-hw-renderer
// (native/hw_gs_frontend.inc and the paraLLEl-GS dispatch, dropped with paraLLEl-GS by decision 0070).
//
// The backend receives every GIF chunk, privileged write and VSYNC, draws through the GPU write-back renderer (M4/M5)
// and presents the composed display with the VBlank's field (M6: 13.3 field offset, 13.4 deinterlace). Local-to-host
// reads are answered from the shadow local memory after the open batch is flushed (10.2).

#include "gs_stream/gs_stream_sink.hpp"

#include "thread_placement.hpp"
#include "hw_cpu_sampler.hpp"
#include "hw_flare_table.hpp"
#include "hw_present_spacing.hpp"
#include "../pacing.hpp"
#include "hw_gs_backend.hpp"
#include "hw_gs_options.hpp"
#include "mtvu_ring.hpp"

#include "platform/cycle_counter.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <time.h>  // TASK-180: CLOCK_MONOTONIC and the CPU-time clocks of the profile lines
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gt4::hw {
namespace {

Backend *g_backend = nullptr;
bool g_odd_field = false;
std::uint64_t g_vsyncs = 0;
std::uint64_t g_unmirrored_privileged = 0;
// GT4_HW_PROFILE_EVERY state (see maybe_profile).
std::uint32_t g_profile_every = 0;
RenderCounters g_profile_last;
std::chrono::steady_clock::time_point g_profile_wall;

// M6 frame dumps (chapter 12.9 asks for them; the donor renderer cannot provide them in this mode).
// GT4_HW_DUMP_DIR=<folder> writes P6 PPM files, GT4_HW_DUMP_EVERY=<n> the interval in VBlanks (default
// 100). The image is the composed scanout of the GPU write-back path (Backend::scanout). The RAW VRAM
// buffer decode of chapter 12.9 stays with the CPU reference tool, which owns that oracle (IN-325).
std::string g_dump_dir;
std::uint32_t g_dump_every = 0;
std::uint64_t g_dump_from = 0;  // GT4_HW_DUMP_FROM=<vblank>: no dumps before it (consecutive frames of one scene)
std::uint64_t g_dumps = 0;
std::uint64_t g_dumps_without_image = 0;

void read_dump_options() {
  const char *dir = std::getenv("GT4_HW_DUMP_DIR");
  if (dir && *dir) {
    g_dump_dir = dir;
    g_dump_every = 100;
    if (const char *every = std::getenv("GT4_HW_DUMP_EVERY")) {
      const long value = std::strtol(every, nullptr, 10);
      g_dump_every = value > 0 ? static_cast<std::uint32_t>(value) : 100u;
    }
    if (const char *from = std::getenv("GT4_HW_DUMP_FROM")) g_dump_from = std::strtoull(from, nullptr, 10);
    std::fprintf(stderr, "[hw] frame dumps: %s every %u VBlanks from %llu (composed scanout image)\n",
                 g_dump_dir.c_str(), g_dump_every, static_cast<unsigned long long>(g_dump_from));
  }
}

// One P6 PPM of the composed scanout image. Returns false when there was no image this VBlank.
bool write_dump(Backend &hw, std::uint64_t index) {
  ScanoutRequest request;
  if (!hw.scanout(request) || request.width == 0 || request.height == 0) return false;
  char name[64];
  std::snprintf(name, sizeof(name), "frame_%05llu.ppm", static_cast<unsigned long long>(index));
  const std::string path = g_dump_dir + "\\" + name;
  std::FILE *file = std::fopen(path.c_str(), "wb");
  if (!file) return false;
  std::fprintf(file, "P6\n%u %u\n255\n", request.width, request.height);
  // One write per row (a 12x frame has 41 million pixels; a write per pixel took longer than the frame itself).
  std::vector<unsigned char> row(std::size_t{request.width} * 3u);
  for (std::size_t y = 0; y < request.height; ++y) {
    const std::uint32_t *pixels = request.pixels.data() + y * request.width;
    for (std::size_t x = 0; x < request.width; ++x) {
      row[3 * x] = static_cast<unsigned char>(pixels[x] & 0xFFu);
      row[3 * x + 1] = static_cast<unsigned char>((pixels[x] >> 8) & 0xFFu);
      row[3 * x + 2] = static_cast<unsigned char>((pixels[x] >> 16) & 0xFFu);
    }
    std::fwrite(row.data(), 1, row.size(), file);
  }
  std::fclose(file);
  return true;
}

void maybe_dump(Backend &hw) {
  if (g_dump_every == 0) return;
  if (g_vsyncs < g_dump_from || g_vsyncs % g_dump_every != 0) return;
  if (write_dump(hw, g_vsyncs)) {
    ++g_dumps;
  } else {
    ++g_dumps_without_image;
  }
}

void start_gs_worker();

// TASK-027 CPU survey: the cost of carrying the GS stream from the game (EE) thread to the GS worker, in TSC ticks
// (converted with the QPC rate between two profile lines). Each counter has one writer: the producer counters are
// written by the thread that calls the hooks (the EE thread), the worker counters by the GS worker. With the stream
// ring (IN-446) the hook's whole cost is the copy into the ring, so copy_ms equals gif_ms, and max_depth is in bytes.
struct TransportCounters {
  // Producer (EE thread).
  std::atomic<std::uint64_t> gif_commands{0}, gif_bytes{0}, privileged_commands{0}, vsync_commands{0};
  std::atomic<std::uint64_t> gif_ticks{0};         // inside gif(): buffer, copy and queue push
  std::atomic<std::uint64_t> copy_ticks{0};        // the byte copy alone
  std::atomic<std::uint64_t> privileged_ticks{0};  // inside privileged()
  std::atomic<std::uint64_t> vsync_wait_ticks{0}, vsync_waits{0};  // back-pressure (kFramesAhead)
  std::atomic<std::uint64_t> drain_wait_ticks{0}, drains{0};       // read_fifo / window calls waiting for the worker
  std::atomic<std::uint64_t> read_fifo_ticks{0}, read_fifos{0};    // read_fifo including its drain
  std::atomic<std::uint64_t> flare_reads{0}, flare_late{0}, flare_start_us{0}, flare_answer_us{0};  // IN-514
  std::atomic<std::uint64_t> flare_deferred{0};  // IN-522
  std::atomic<std::uint64_t> wake_pushes{0};       // pushes that found the worker waiting for work
  // Worker (GS worker thread).
  std::atomic<std::uint64_t> busy_ticks[5] = {};   // per GsCommand::Kind: Gif, Privileged, Vsync, GsReset, GifReset
  std::atomic<std::uint64_t> idle_ticks{0}, wakeups{0}, max_depth{0};
  // IN-461 (direct GS intake): VU1 jobs and records executed straight from the MTVU output ring, and the waits for them.
  std::atomic<std::uint64_t> mtvu_jobs{0}, mtvu_records{0}, mtvu_wait_ticks{0}, mtvu_sleeps{0};
};
TransportCounters g_transport;

void add(std::atomic<std::uint64_t> &counter, std::uint64_t value) {
  counter.store(counter.load(std::memory_order_relaxed) + value, std::memory_order_relaxed);  // single writer
}

bool start_backend(std::string &error) {
  Options options;
  try {
    options = parse_options([](const char *name) -> const char * { return std::getenv(name); });
  } catch (const OptionError &problem) {
    error = problem.what();
    return false;
  }
  read_dump_options();
  sampler::name_current_thread("ee");
  thread_placement::apply("ee");
  if (sampler::start_from_env()) std::fprintf(stderr, "[cpu-sampler] sampling the process (GT4_CPU_SAMPLER)\n");
  if (const char *every = std::getenv("GT4_HW_PROFILE_EVERY")) {
    const long value = std::strtol(every, nullptr, 10);
    g_profile_every = value > 0 ? static_cast<std::uint32_t>(value) : 0u;
    g_profile_wall = std::chrono::steady_clock::now();
  }
  // Created once and never destroyed: the Vulkan device must not be torn down during process exit.
  auto *created = new Backend;
  if (!created->init(options, error)) return false;
  g_backend = created;
  std::fprintf(stderr, "[hw] GT4_RENDERER=hw: HWR backend started in process, no GS process and no donor Vulkan renderer\n");
  start_gs_worker();
  for (const std::string &line : describe_options(options)) std::fprintf(stderr, "[hw] %s\n", line.c_str());
  return true;
}

Backend &backend() {
  if (!g_backend) {
    std::fprintf(stderr, "[hw] HWR backend used before start; stopping\n");
    std::exit(EXIT_FAILURE);
  }
  return *g_backend;
}

void gif_now(int path, const std::uint8_t *data, std::size_t bytes) { backend().gif_transfer(path, data, bytes); }

bool g_display_page_changed = true;  // TASK-082, GS worker: a DISPFB write with a new value since the last VBlank
gt4::hw::PresentSpacing g_one_queue_spacing{216};
std::chrono::steady_clock::time_point g_gs_cost_start;
std::chrono::steady_clock::duration g_gs_idle{};
std::int64_t g_gs_cost_us = 0;
// When the EE reached each VBlank, for the GS worker to tell how late it is. Indexed by the count of VBlanks, which is at most
// kFramesAhead ahead on the EE.
std::array<std::atomic<std::chrono::steady_clock::time_point>, 8> g_vsync_pushed{};
std::uint64_t g_ee_vsyncs_pushed = 0;  // EE only
std::uint64_t g_last_dispfb[2] = {};

void privileged_now(std::uint32_t address, std::uint64_t value) {
  if (!backend().write_privileged(address, value)) ++g_unmirrored_privileged;
  // DISPFB1 and DISPFB2, in any of the addresses the stream carries them.
  const std::uint32_t low = address & 0xFFFFu;
  if (low == 0x70u || low == 0x90u) {
    std::uint64_t &last = g_last_dispfb[low == 0x90u ? 1 : 0];
    if (last != value) g_display_page_changed = true;
    last = value;
  }
}

// TASK-178 diagnostics: an FNV-1a hash over every flare table reply (128 qwords) in stream order, so runs that must give the
// VU1 the same tables can be compared. Written under the backend lock (or on the only thread, GT4_HW_GS_THREAD=0).
std::uint64_t g_flare_reply_hash = 14695981039346656037ull;
std::uint64_t g_flare_replies = 0;

void read_fifo_now(std::uint8_t *data, std::uint32_t qwords) {
  const std::size_t filled = backend().read_transfer_fifo(data, qwords);
  if (filled < qwords) std::memset(data + filled * 16, 0, (static_cast<std::size_t>(qwords) - filled) * 16);
  if (qwords == FlareTableReads::kQwords) {
    for (std::size_t i = 0; i < FlareTableReads::kBytes; ++i) {
      g_flare_reply_hash ^= data[i];
      g_flare_reply_hash *= 1099511628211ull;
    }
    ++g_flare_replies;
  }
}

// TASK-178 diagnostics (I_cpu of STUDY-FRAME-DOWNLOAD-COST.md, option O4): the GS worker's busy time from the end of a flare read
// (IN-522 FlareRead) to the start of the first VU1 job given that read's words (IN-522 TablePatch), the CPU work an
// asynchronous read could overlap with the GPU. Its wall time too (idle and VU1 waits included). GS worker only.
struct FlareGap {
  bool open = false;
  std::uint64_t ticket = 0;  // the read whose first patched job is awaited
  std::uint64_t busyTicks = 0;
  std::chrono::steady_clock::time_point readEnd{};
  std::uint64_t matched = 0, unmatched = 0, busyTotalTicks = 0;
  double wallTotalMs = 0.0;
};
FlareGap g_flare_gap;
std::uint64_t g_tsc_start = 0;  // the worker's start, to turn TSC ticks into ms
std::chrono::steady_clock::time_point g_tsc_start_time{};

void flare_gap_read(std::uint64_t ticket) {
  FlareGap &g = g_flare_gap;
  if (g.open) ++g.unmatched;  // the previous read had no patched job before this one
  g.open = true;
  g.ticket = ticket;
  g.busyTicks = 0;
  g.readEnd = std::chrono::steady_clock::now();
}

void flare_gap_busy(std::uint64_t ticks) {
  if (g_flare_gap.open) g_flare_gap.busyTicks += ticks;
}

void flare_gap_job(std::uint64_t job) {  // before VU1 job `job` runs on the GS worker
  FlareGap &g = g_flare_gap;
  if (!g.open || gt4::gs_stream::g_deferred_table.first_job == nullptr) return;
  const std::uint64_t first = gt4::gs_stream::g_deferred_table.first_job(g.ticket);
  if (first == 0 || first != (job & 0xFFFFFFFFull)) return;
  g.open = false;
  ++g.matched;
  g.busyTotalTicks += g.busyTicks;
  g.wallTotalMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g.readEnd).count();
}

// Every 600 VBlanks: the reply hash and count (cumulative), and I_cpu per matched read over the interval.
void print_flare_replies() {
  static std::uint64_t lastMatched = 0, lastUnmatched = 0, lastBusy = 0;
  static double lastWall = 0.0;
  const FlareGap &g = g_flare_gap;
  double msPerTick = 0.0;
  const std::uint64_t tsc = gt4::platform::cycle_counter();
  if (g_tsc_start != 0 && tsc > g_tsc_start) {
    msPerTick = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_tsc_start_time).count() /
                static_cast<double>(tsc - g_tsc_start);
  }
  const std::uint64_t matched = g.matched - lastMatched;
  const double perRead = matched != 0 ? 1.0 / static_cast<double>(matched) : 0.0;
  std::fprintf(stderr,
               "[hw] flare replies vsync=%llu replies=%llu hash=%016llx | i_cpu_ms/read=%.3f i_wall_ms/read=%.3f "
               "(read end to its first patched VU1 job; %llu matched, %llu without one)\n",
               static_cast<unsigned long long>(g_vsyncs), static_cast<unsigned long long>(g_flare_replies),
               static_cast<unsigned long long>(g_flare_reply_hash),
               static_cast<double>(g.busyTotalTicks - lastBusy) * msPerTick * perRead, (g.wallTotalMs - lastWall) * perRead,
               static_cast<unsigned long long>(matched), static_cast<unsigned long long>(g.unmatched - lastUnmatched));
  lastMatched = g.matched;
  lastUnmatched = g.unmatched;
  lastBusy = g.busyTotalTicks;
  lastWall = g.wallTotalMs;
}

// Once per guest VBlank: submit the recorded work, then present (window) or close the frame (headless).
// GT4_HW_PROFILE_EVERY=<n>: every n VBlanks, one stderr line with the interval's per-frame averages (CPU ms per
// pass and work counts), so heavy scenes (3D cars, races) can be profiled from a headless run (M9 work).

double flare_deadline_ms();

// IN-514: when the EE reaches the flare table and gets its answer, after the VBlank, and how many answers were late.
void print_flare_table() {
  const TransportCounters &t = g_transport;
  static std::uint64_t lastFlareReads = 0, lastFlareLate = 0, lastFlareStart = 0, lastFlareAnswer = 0;
  const std::uint64_t flareReads = t.flare_reads.load(), flareLate = t.flare_late.load();
  const std::uint64_t flareStart = t.flare_start_us.load(), flareAnswer = t.flare_answer_us.load();
  if (flareReads != lastFlareReads) {
    const double reads = static_cast<double>(flareReads - lastFlareReads);
    std::fprintf(stderr, "[hw] flare table: reads=%.0f late=%llu (total %llu) reached at %.2f ms, answered at %.2f ms after the VBlank, limit %.2f ms\n", reads,
                 static_cast<unsigned long long>(flareLate - lastFlareLate), static_cast<unsigned long long>(flareLate),
                 static_cast<double>(flareStart - lastFlareStart) / reads / 1000.0,
                 static_cast<double>(flareAnswer - lastFlareAnswer) / reads / 1000.0, flare_deadline_ms());
  }
  lastFlareReads = flareReads;
  lastFlareLate = flareLate;
  lastFlareStart = flareStart;
  lastFlareAnswer = flareAnswer;
  if (const std::uint64_t deferred = t.flare_deferred.load()) {
    const auto patches = gt4::gs_stream::g_deferred_table.patches;
    std::fprintf(stderr, "[hw] flare table: %llu reads without the EE waiting, %llu VIF packets given their words\n",
                 static_cast<unsigned long long>(deferred), static_cast<unsigned long long>(patches != nullptr ? patches() : 0));
  }
}

// TASK-027 CPU survey: one [hw] transport line per profile interval (per-VBlank averages).
void print_transport(double n) {
  constexpr int kValues = 25;
  static std::uint64_t lastTsc = 0;
#ifdef _WIN32
  static LARGE_INTEGER lastQpc{};
#else
  static std::int64_t lastQpc = 0;  // TASK-180: CLOCK_MONOTONIC nanoseconds
#endif
  static std::uint64_t last[kValues] = {};
  const std::uint64_t tsc = gt4::platform::cycle_counter();
#ifdef _WIN32
  LARGE_INTEGER qpc{}, frequency{};
  QueryPerformanceCounter(&qpc);
  QueryPerformanceFrequency(&frequency);
#else
  timespec monotonic{};
  clock_gettime(CLOCK_MONOTONIC, &monotonic);
  const std::int64_t qpc = static_cast<std::int64_t>(monotonic.tv_sec) * 1000000000 + monotonic.tv_nsec;
#endif
  const TransportCounters &t = g_transport;
  const std::uint64_t values[kValues] = {
      t.gif_commands.load(), t.gif_bytes.load(), t.privileged_commands.load(), t.vsync_commands.load(),
      t.gif_ticks.load(), t.copy_ticks.load(), t.privileged_ticks.load(), t.vsync_wait_ticks.load(),
      t.vsync_waits.load(), t.drain_wait_ticks.load(), t.drains.load(), t.read_fifo_ticks.load(),
      t.read_fifos.load(), t.wake_pushes.load(), t.busy_ticks[0].load(), t.busy_ticks[1].load(),
      t.busy_ticks[2].load(), t.busy_ticks[3].load() + t.busy_ticks[4].load(), t.idle_ticks.load(),
      t.wakeups.load(), t.max_depth.load(), t.mtvu_jobs.load(), t.mtvu_records.load(), t.mtvu_wait_ticks.load(),
      t.mtvu_sleeps.load()};
  if (lastTsc != 0 && tsc > lastTsc) {
#ifdef _WIN32
    const double seconds = static_cast<double>(qpc.QuadPart - lastQpc.QuadPart) / static_cast<double>(frequency.QuadPart);
#else
    const double seconds = static_cast<double>(qpc - lastQpc) / 1e9;
#endif
    const double msPerTick = seconds * 1000.0 / static_cast<double>(tsc - lastTsc);
    const auto d = [&](int i) { return static_cast<double>(values[i] - last[i]) / n; };
    const auto ms = [&](int i) { return d(i) * msPerTick; };
    std::fprintf(stderr,
                 "[hw] transport vsync=%llu ee: gif_cmds=%.1f gif_kib=%.0f priv_cmds=%.1f gif_ms=%.3f copy_ms=%.3f "
                 "priv_ms=%.3f vsync_wait_ms=%.3f (%.2f) drain_wait_ms=%.3f (%.2f) read_fifo_ms=%.3f (%.2f) "
                 "wake_pushes=%.1f | worker: gif_ms=%.3f priv_ms=%.3f vsync_ms=%.3f other_ms=%.3f idle_ms=%.3f "
                 "wakeups=%.1f max_depth=%llu mtvu_jobs=%.1f mtvu_records=%.1f mtvu_wait_ms=%.3f mtvu_sleeps=%.1f\n",
                 static_cast<unsigned long long>(g_vsyncs), d(0), d(1) / 1024.0, d(2), ms(4), ms(5), ms(6), ms(7), d(8),
                 ms(9), d(10), ms(11), d(12), d(13), ms(14), ms(15), ms(16), ms(17), ms(18), d(19),
                 static_cast<unsigned long long>(values[20]), d(21), d(22), ms(23), d(24));
  }
  std::copy(std::begin(values), std::end(values), std::begin(last));
  lastTsc = tsc;
  lastQpc = qpc;
}

// TASK-174: the presentation path, what became of the presentations, and what they cost the GS thread, as totals over `requests`
// presentations (per presentation for the averages). gs_present_ms is the whole presentation step on the GS thread, gs_wait_ms the
// part inside calls that can wait (fences, acquire, queue submit and present), gs_lock_wait_ms the GS thread's wait for the shared
// queue's lock (all its submissions, the renderer's included), presenter_lock_wait_ms and presenter_queue_ms the presentation
// thread's wait for that lock and its time in the copy and present calls, presenter_display_wait_ms its waits for the display
// (acquire, previous presentation shown) that no longer fall on the GS thread; the two longest_ms are session maxima (the GS thread's
// longest wait for the lock, the presentation thread's longest call holding it).
void print_presentation(std::FILE *output, const char *label, const Stats &now, const Stats &was) {
  const std::uint64_t requests = now.present_requests - was.present_requests;
  if (requests == 0) return;
  const double per = static_cast<double>(requests) * 1000.0;  // microsecond totals to milliseconds per presentation
  std::fprintf(output,
               "[hw] presentation %s path=%s requests=%llu shown=%llu not_shown=%llu failed=%llu left_out=%llu gs_present_ms=%.3f "
               "gs_wait_ms=%.3f gs_lock_wait_ms=%.3f gs_lock_longest_ms=%.3f gs_queue_calls=%.2f presenter_lock_wait_ms=%.3f "
               "presenter_queue_ms=%.3f presenter_longest_ms=%.3f presenter_display_wait_ms=%.3f\n",
               label, now.present_path, static_cast<unsigned long long>(requests),
               static_cast<unsigned long long>(now.presents_shown - was.presents_shown),
               static_cast<unsigned long long>(now.presents_not_shown - was.presents_not_shown),
               static_cast<unsigned long long>(now.presents_failed - was.presents_failed),
               static_cast<unsigned long long>(now.presents_left_out - was.presents_left_out),
               (now.gs_present_step_us - was.gs_present_step_us) / per, (now.gs_present_wait_us - was.gs_present_wait_us) / per,
               (now.gs_queue_lock_wait_us - was.gs_queue_lock_wait_us) / per, now.gs_queue_lock_longest_wait_us / 1000.0,
               static_cast<double>(now.gs_queue_calls - was.gs_queue_calls) / static_cast<double>(requests),
               (now.presenter_queue_lock_wait_us - was.presenter_queue_lock_wait_us) / per,
               (now.presenter_queue_call_us - was.presenter_queue_call_us) / per, now.presenter_queue_longest_call_us / 1000.0,
               (now.presenter_display_wait_us - was.presenter_display_wait_us) / per);
}

void maybe_profile(Backend &hw) {
  if (g_profile_every == 0 || g_vsyncs % g_profile_every != 0) return;
  const RenderCounters now = hw.render_counters();
  const RenderCounters &was = g_profile_last;
  const double n = static_cast<double>(g_profile_every);
  const auto wall = std::chrono::steady_clock::now();
  const double wallMs = std::chrono::duration<double, std::milli>(wall - g_profile_wall).count() / n;
  std::fprintf(stderr,
               "[hw] profile vsync=%llu wall_ms=%.2f gif_ms=%.2f upload_ms=%.2f translate_ms=%.2f execute_ms=%.2f "
               "decode_ms=%.2f "
               "writeback_ms=%.2f load_ms=%.2f batches=%.1f prims=%.1f draws=%.1f chunks=%.1f textures=%.1f "
               "cached=%.1f texels=%.0f writebacks=%.2f loads=%.2f submissions=%.2f barriers=%.1f "
               "wb_other=%.2f wb_texture=%.2f wb_clut=%.2f wb_transfer=%.2f wb_display=%.2f wb_evict=%.2f "
               "wb_overlap=%.2f wb_fallback=%.2f wb_sprite=%.2f\n",
               static_cast<unsigned long long>(g_vsyncs), wallMs, (now.gif_us - was.gif_us) / n / 1000.0,
               (now.upload_us - was.upload_us) / n / 1000.0, (now.translate_us - was.translate_us) / n / 1000.0,
               (now.execute_us - was.execute_us) / n / 1000.0, (now.decode_us - was.decode_us) / n / 1000.0,
               (now.cache_writeback_us - was.cache_writeback_us) / n / 1000.0,
               (now.cache_load_us - was.cache_load_us) / n / 1000.0, static_cast<double>(now.batches - was.batches) / n,
               static_cast<double>(now.primitives - was.primitives) / n,
               static_cast<double>(now.gpu_draws - was.gpu_draws) / n, static_cast<double>(now.gpu_chunks - was.gpu_chunks) / n,
               static_cast<double>(now.textures - was.textures) / n,
               static_cast<double>(now.textures_cached - was.textures_cached) / n,
               static_cast<double>(now.texels - was.texels) / n,
               static_cast<double>(now.cache_writebacks - was.cache_writebacks) / n,
               static_cast<double>(now.cache_loads - was.cache_loads) / n,
               static_cast<double>(now.gpu_submissions - was.gpu_submissions) / n,
               static_cast<double>(now.gpu_barriers - was.gpu_barriers) / n,
               static_cast<double>(now.writeback_reasons[0] - was.writeback_reasons[0]) / n,
               static_cast<double>(now.writeback_reasons[1] - was.writeback_reasons[1]) / n,
               static_cast<double>(now.writeback_reasons[2] - was.writeback_reasons[2]) / n,
               static_cast<double>(now.writeback_reasons[3] - was.writeback_reasons[3]) / n,
               static_cast<double>(now.writeback_reasons[4] - was.writeback_reasons[4]) / n,
               static_cast<double>(now.writeback_reasons[5] - was.writeback_reasons[5]) / n,
               static_cast<double>(now.writeback_reasons[6] - was.writeback_reasons[6]) / n,
               static_cast<double>(now.writeback_reasons[7] - was.writeback_reasons[7]) / n,
               static_cast<double>(now.writeback_reasons[8] - was.writeback_reasons[8]) / n);
  if (now.gpu_busy_ns != was.gpu_busy_ns) {
    std::fprintf(stderr,
                 "[hw] gpu vsync=%llu busy_ms=%.2f pass_ms=%.2f decode_ms=%.2f clut_ms=%.2f swizzle_ms=%.2f "
                 "upload_ms=%.2f download_ms=%.2f scaled_ms=%.2f refresh_ms=%.2f early_submissions=%.2f\n",
                 static_cast<unsigned long long>(g_vsyncs), (now.gpu_busy_ns - was.gpu_busy_ns) / n / 1e6,
                 (now.gpu_ns[0] - was.gpu_ns[0]) / n / 1e6, (now.gpu_ns[1] - was.gpu_ns[1]) / n / 1e6,
                 (now.gpu_ns[2] - was.gpu_ns[2]) / n / 1e6, (now.gpu_ns[3] - was.gpu_ns[3]) / n / 1e6,
                 (now.gpu_ns[4] - was.gpu_ns[4]) / n / 1e6, (now.gpu_ns[5] - was.gpu_ns[5]) / n / 1e6,
                 (now.gpu_ns[6] - was.gpu_ns[6]) / n / 1e6, (now.gpu_ns[7] - was.gpu_ns[7]) / n / 1e6,
                 static_cast<double>(now.gpu_early_submissions - was.gpu_early_submissions) / n);
  }
  // IN-367 efficiency line (owner principle 2026-10-22: measure cost, not only FPS): CPU time of the thread that
  // runs the GS stream (the GS worker, or the EE thread with GT4_HW_GS_THREAD=0) and of the whole process (all
  // threads: EE, VU1 worker, GS worker, audio, driver), in ms per frame; render passes, barriers and bytes moved
  // between CPU and GPU per frame.
  {
    static double lastThreadMs = 0.0;
    static double lastProcessMs = 0.0;
    static bool primed = false;
#ifdef _WIN32
    auto filetime_ms = [](const FILETIME &kernel, const FILETIME &user) {
      const auto ticks = [](const FILETIME &t) {
        return (static_cast<unsigned long long>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
      };
      return static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;  // 100 ns units
    };
    FILETIME created{}, exited{}, kernel{}, user{};
    double threadMs = 0.0, processMs = 0.0;
    if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) threadMs = filetime_ms(kernel, user);
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) processMs = filetime_ms(kernel, user);
#else
    const auto cpu_ms = [](clockid_t clock) {
      timespec value{};
      return clock_gettime(clock, &value) == 0 ? static_cast<double>(value.tv_sec) * 1000.0 + value.tv_nsec / 1e6 : 0.0;
    };
    const double threadMs = cpu_ms(CLOCK_THREAD_CPUTIME_ID), processMs = cpu_ms(CLOCK_PROCESS_CPUTIME_ID);
#endif
    if (primed) {
      std::fprintf(stderr,
                   "[hw] cost vsync=%llu gs_thread_cpu_ms=%.2f process_cpu_core_ms=%.2f render_passes=%.1f "
                   "barriers=%.1f upload_mb=%.2f readback_mb=%.3f scaled_image_mb=%.1f\n",
                   static_cast<unsigned long long>(g_vsyncs), (threadMs - lastThreadMs) / n,
                   (processMs - lastProcessMs) / n,
                   static_cast<double>(now.gpu_render_passes - was.gpu_render_passes) / n,
                   static_cast<double>(now.gpu_barriers - was.gpu_barriers) / n,
                   static_cast<double>(now.gpu_upload_bytes - was.gpu_upload_bytes) / n / 1e6,
                   static_cast<double>(now.gpu_readback_bytes - was.gpu_readback_bytes) / n / 1e6,
                   static_cast<double>(now.gpu_scaled_image_bytes) / 1e6);
      // IN-384 follow-up (owner test 11, sporadic 1x frames): why a VBlank fell back to the CPU 1x presentation.
      // 0 GpuComposeApplies declined, 1 zero output size, 2 interlaced frame larger than the deinterlace images,
      // 3 the GPU compose or its flush failed.
      const unsigned long long skips[4] = {now.gpu_compose_skips[0] - was.gpu_compose_skips[0],
                                           now.gpu_compose_skips[1] - was.gpu_compose_skips[1],
                                           now.gpu_compose_skips[2] - was.gpu_compose_skips[2],
                                           now.gpu_compose_skips[3] - was.gpu_compose_skips[3]};
      if (skips[0] + skips[1] + skips[2] + skips[3] != 0) {
        std::fprintf(stderr, "[hw] compose_skips applies=%llu zero_size=%llu too_big=%llu failed=%llu\n", skips[0],
                     skips[1], skips[2], skips[3]);
      }
    }
    primed = true;
    lastThreadMs = threadMs;
    lastProcessMs = processMs;
  }
  {
    static Stats lastPresentation;
    const Stats presentation = hw.stats();
    char label[32];
    std::snprintf(label, sizeof label, "vsync=%llu", static_cast<unsigned long long>(g_vsyncs));
    print_presentation(stderr, label, presentation, lastPresentation);
    lastPresentation = presentation;
  }
  print_transport(n);
  g_profile_last = now;
  g_profile_wall = wall;
}

void vsync_now(bool odd_field) {
  Backend &hw = backend();
  g_odd_field = odd_field;
  ++g_vsyncs;
  sampler::note_vblank(g_vsyncs);
  std::chrono::steady_clock::duration waited{};
  hw.set_display_field(odd_field);
  // TASK-082: in a running race with frame skipping, the game draws into the page not shown, so the display page unchanged means
  // nothing new to show. No presentation then, so a frame counter counts what was drawn and the frame takes no time of its own. Nor
  // a frame for the renderer: targets age by frames, and a VBlank with no drawing aged them into being dropped and read back (a
  // drawn frame at 12x cost 29 ms against 22 ms, most of it six readbacks in place of one).
  const bool hold = gt4::pacing::frame_skip_engaged.load(std::memory_order_relaxed) && !g_display_page_changed;
  g_display_page_changed = false;
  hw.set_age_targets(!hold);
  // TASK-180b: the per-VBlank work of this thread, so a slow present on a phone or an emulator can be told from the
  // renderer's own stages (GT4_GS_PRESENT_LOG=1; off, this reads one static flag and prints nothing).
  static const bool present_log = std::getenv("GT4_GS_PRESENT_LOG") != nullptr;
  const auto flush_start = std::chrono::steady_clock::now();
  hw.flush();
  const auto flush_end = std::chrono::steady_clock::now();
  hw.next_frame();  // one guest frame per VBlank, windowed or headless (the session line's `frames`)
  const auto before_present = std::chrono::steady_clock::now();
  if (hw.has_window() && !hold) {
    // Without a thread to show pictures from (hw_gs_backend.cpp, present_paced; the rendering queue path) the wait is here, and holds
    // up the next picture. So it leaves room, or waiting would raise the mean interval it waits for: it locked at 20 frames per second.
    if (gt4::pacing::frame_skip_recent.load(std::memory_order_relaxed) && !hw.has_presentation_thread()) {
      const auto before = std::chrono::steady_clock::now();
      const auto arrival = std::chrono::duration_cast<std::chrono::microseconds>(before.time_since_epoch()).count();
      std::this_thread::sleep_until(std::chrono::steady_clock::time_point(std::chrono::microseconds(g_one_queue_spacing.when(arrival))));
      waited = std::chrono::steady_clock::now() - before;
    }
    hw.present(odd_field ? PresentPhase::field_odd : PresentPhase::field_even);
  }
  const auto finished = std::chrono::steady_clock::now();
  if (present_log && g_vsyncs % 10 == 0) {
    const auto ms = [](std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) {
      return std::chrono::duration<double, std::milli>(to - from).count();
    };
    std::fprintf(stderr, "[gs] vblank %llu: flush %.2f ms, next_frame %.2f ms, present %.2f ms, total %.2f ms (window %d)\n",
                 static_cast<unsigned long long>(g_vsyncs), ms(flush_start, flush_end), ms(flush_end, before_present),
                 ms(before_present, finished), ms(flush_start, finished), hw.has_window() ? 1 : 0);
  }
  // What a drawn frame took: from the last frame drawn, without the time with nothing to do or spent waiting above.
  g_gs_idle += waited;
  if (!hold) {
    if (g_gs_cost_start.time_since_epoch().count() != 0) {
      const std::int64_t cost = std::chrono::duration_cast<std::chrono::microseconds>(finished - g_gs_cost_start - g_gs_idle).count();
      g_gs_cost_us += (cost - g_gs_cost_us) / 16;
      gt4::pacing::gs_cost_us.store(static_cast<std::int32_t>(g_gs_cost_us), std::memory_order_relaxed);
    }
    g_gs_cost_start = finished;
    g_gs_idle = {};
  }
  const auto pushed = g_vsync_pushed[g_vsyncs % g_vsync_pushed.size()].load(std::memory_order_acquire);
  gt4::pacing::gs_late_us.store(static_cast<std::int32_t>(std::clamp<std::int64_t>(
                                    std::chrono::duration_cast<std::chrono::microseconds>(finished - pushed).count() - 16667, -100000, 1000000)),
                                std::memory_order_relaxed);
  maybe_dump(hw);
  maybe_profile(hw);
  if (g_vsyncs % 600 == 0) print_flare_replies();  // TASK-178
}

void gs_reset_now() {
  Backend &hw = backend();
  hw.reset_context();
  hw.reset_gif_paths();
  hw.clear_vram();
}

void gif_reset_now() { backend().reset_gif_paths(); }

// ---- GS worker thread (IN-363) ----
// The game's EE thread used to run the whole GS stream (parse, translate, record, submit, compose, present)
// synchronously. Now it queues the stream, in order, to one worker thread, which runs exactly the calls above in
// the same order, so results are unchanged; the EE continues while the GS work of the frame runs. The calls that
// return data or change the window (LOCAL->HOST FIFO reads, window attach and resize, the end of the session)
// first wait until the worker has executed everything queued before them, then run under the lock the worker
// holds for each command. At a VBlank the EE waits while kFramesAhead VBlanks are still queued (back-pressure).
// GT4_HW_GS_THREAD=0 keeps the synchronous path (diagnostic switch, not a 12.8 option).
//
// IN-446 (TASK-027 CPU survey, front T2): the stream travels through one single-producer/single-consumer byte ring
// instead of a deque of commands with a byte vector each, a mutex and a notify per command. The EE copies a GIF chunk
// once, into the ring, and takes no lock; the worker executes each record in place and then releases it. Records use
// the MTVU ring's framing (mtvu_ring.hpp: a 16-byte header, the payload padded to 16 bytes, a wrap filler before a
// record that would cross the end). Waking is conditional: the EE rings the work bell after a push, which wakes the
// worker only when it sleeps; the worker rings the progress bell only when what the EE waits for has happened (free
// space, a VBlank done, the ring empty).
enum class SinkRecord : std::uint16_t { Gif = 32, Privileged = 33, Vsync = 34, GsReset = 35, GifReset = 36, MtvuJob = 37, SkippedRead = 38, FlareRead = 39 };

class StreamRing {
public:
  explicit StreamRing(std::size_t capacity) : capacity_(capacity), storage_(capacity) {}

  std::size_t capacity() const { return capacity_; }
  std::size_t max_payload() const { return capacity_ / 2 - mtvu::kRecordHeaderBytes; }
  std::uint64_t head() const { return head_.load(std::memory_order_seq_cst); }
  std::uint64_t tail() const { return tail_.load(std::memory_order_seq_cst); }

  // Producer only. False when the record does not fit now.
  bool try_push(const mtvu::RecordHeader &header, const std::uint8_t *payload) {
    const std::size_t total = mtvu::record_bytes(header.bytes);
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    const std::uint64_t tail = tail_.load(std::memory_order_seq_cst);  // pairs with the producer's want_ store
    const std::size_t offset = static_cast<std::size_t>(head % capacity_);
    const std::size_t filler = offset + total > capacity_ ? capacity_ - offset : 0;
    if (capacity_ - static_cast<std::size_t>(head - tail) < filler + total) return false;
    std::size_t at = offset;
    if (filler != 0) {
      mtvu::RecordHeader wrap;
      wrap.kind = static_cast<std::uint16_t>(mtvu::RecordKind::Wrap);
      wrap.bytes = static_cast<std::uint32_t>(filler - mtvu::kRecordHeaderBytes);
      std::memcpy(storage_.data() + offset, &wrap, mtvu::kRecordHeaderBytes);
      at = 0;
    }
    std::memcpy(storage_.data() + at, &header, mtvu::kRecordHeaderBytes);
    if (header.bytes != 0) std::memcpy(storage_.data() + at + mtvu::kRecordHeaderBytes, payload, header.bytes);
    head_.store(head + filler + total, std::memory_order_seq_cst);
    return true;
  }

  // Consumer only. Lends the next record (skipping wrap fillers) until release(). False when empty.
  bool try_peek(mtvu::RecordHeader &header, const std::uint8_t *&payload) {
    std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    const std::uint64_t head = head_.load(std::memory_order_seq_cst);
    while (tail != head) {
      const std::size_t offset = static_cast<std::size_t>(tail % capacity_);
      std::memcpy(&header, storage_.data() + offset, mtvu::kRecordHeaderBytes);
      const std::size_t total = mtvu::record_bytes(header.bytes);
      if (header.kind == static_cast<std::uint16_t>(mtvu::RecordKind::Wrap)) {
        tail += total;
        continue;
      }
      peekedTail_ = tail + total;
      payload = storage_.data() + offset + mtvu::kRecordHeaderBytes;
      return true;
    }
    return false;
  }

  // Consumer only: frees the record lent by the last try_peek (and the fillers before it).
  void release() { tail_.store(peekedTail_, std::memory_order_seq_cst); }

private:
  std::size_t capacity_;
  std::vector<std::uint8_t> storage_;
  alignas(64) std::atomic<std::uint64_t> head_{0};
  alignas(64) std::atomic<std::uint64_t> tail_{0};
  std::uint64_t peekedTail_ = 0;  // consumer only
};

class GsWorker {
public:
  static constexpr std::size_t kFramesAhead = 2;
  // Two busy race VBlanks (about 6 MB of GIF data each) fit; a larger chunk is split into several records.
  static constexpr std::size_t kRingBytes = std::size_t{16} << 20;
  static constexpr std::uint64_t kWantNothing = 0;
  static constexpr std::uint64_t kWantEmpty = ~std::uint64_t{0};

  GsWorker() : ring_(kRingBytes) {}

  void start() {
    thread_ = std::thread([this] { run(); });
  }

  // Producer (EE thread): one record, in stream order. A GIF chunk larger than a record is split at qword boundaries
  // (the GIF parser keeps each path's state across transfers).
  void push(SinkRecord kind, std::uint8_t path, std::uint64_t value, const std::uint8_t *payload, std::size_t bytes) {
    if (kind == SinkRecord::Vsync) {
      if (queuedVsyncs_.load(std::memory_order_seq_cst) >= kFramesAhead) {
        const std::uint64_t start = gt4::platform::cycle_counter();
        const auto waitStart = std::chrono::steady_clock::now();
        wait_for([this] { return queuedVsyncs_.load(std::memory_order_seq_cst) < kFramesAhead; }, kWantNothing);
        gt4::pacing::ee_gs_wait_us.fetch_add(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - waitStart).count(), std::memory_order_relaxed);
        add(g_transport.vsync_wait_ticks, gt4::platform::cycle_counter() - start);
        add(g_transport.vsync_waits, 1);
      }
      gt4::pacing::gs_vsyncs_queued.store(static_cast<std::uint32_t>(queuedVsyncs_.fetch_add(1, std::memory_order_seq_cst) + 1),
                                          std::memory_order_relaxed);
    }
    const std::size_t limit = ring_.max_payload() & ~std::size_t{15};
    do {
      const std::size_t part = bytes < limit ? bytes : limit;
      mtvu::RecordHeader header;
      header.kind = static_cast<std::uint16_t>(kind);
      header.path = path;
      header.bytes = static_cast<std::uint32_t>(part);
      header.job = value;
      if (!ring_.try_push(header, payload)) {
        const std::uint64_t need = mtvu::record_bytes(part) * 2;  // the record and a worst-case wrap filler
        wait_for([&] { return ring_.try_push(header, payload); }, need);
      }
      if (sleeping_.load(std::memory_order_relaxed)) add(g_transport.wake_pushes, 1);
      work_.ring();
      payload += part;
      bytes -= part;
    } while (bytes != 0);
    const std::uint64_t depth = ring_.head() - ring_.tail();
    if (depth > maxDepth_) {
      maxDepth_ = depth;
      g_transport.max_depth.store(depth, std::memory_order_relaxed);
    }
  }

  // IN-461 (CPU survey T3, direct GS intake; EE thread): VU1 job `job` at this point of the stream. The worker executes
  // the job's GIF records straight from the MTVU output ring when it reaches this record, so the EE neither copies nor
  // forwards them. The ring and its bell never change; they are stored before the first record that needs them.
  void post_mtvu_job(std::uint64_t job, mtvu::SpscByteRing &output, mtvu::Doorbell &bell) {
    if (mtvuOutput_ == nullptr) {
      mtvuOutput_ = &output;
      mtvuBell_ = &bell;
    }
    push(SinkRecord::MtvuJob, 0, job, nullptr, 0);
  }

  // IN-477: a read the EE does not wait for, at its place in the stream.
  void post_skipped_read(std::uint32_t qwords) { push(SinkRecord::SkippedRead, 0, qwords, nullptr, 0); }

  // IN-522 (EE thread): the flare table read at its place in the stream, not waited for: the worker writes `dest`.
  std::uint64_t post_flare_table(std::uint8_t *dest, std::uint64_t vblank) {
    const std::uint64_t ticket = flareReads_.post(vblank, dest);
    push(SinkRecord::FlareRead, 0, FlareTableReads::kQwords, nullptr, 0);
    return ticket;
  }
  void wait_copy_flare_table(std::uint64_t ticket, std::size_t offset, std::size_t bytes, std::uint8_t *out) {
    flareReads_.wait_copy(ticket, offset, bytes, out);
  }

  // IN-514 (EE thread): the flare table read at its place in the stream, answered by `deadline` (hw_flare_table.hpp).
  bool read_flare_table(std::uint8_t *data, std::uint64_t vblank, FlareTableReads::Clock::time_point deadline) {
    const std::uint64_t ticket = flareReads_.post(vblank);
    push(SinkRecord::FlareRead, 0, FlareTableReads::kQwords, nullptr, 0);
    return flareReads_.answer(ticket, vblank, deadline, data);
  }

  // Waits until every queued record has executed; the caller then runs with the backend lock held.
  std::unique_lock<std::mutex> drain() {
    const std::uint64_t start = gt4::platform::cycle_counter();
    if (ring_.head() != ring_.tail()) wait_for([this] { return ring_.head() == ring_.tail(); }, kWantEmpty);
    std::unique_lock<std::mutex> backend(backendMutex_);
    add(g_transport.drain_wait_ticks, gt4::platform::cycle_counter() - start);
    add(g_transport.drains, 1);
    return backend;
  }

private:
  std::vector<std::uint8_t> skippedRead_;  // IN-477: worker only
  FlareTableReads flareReads_;  // IN-514

  // Producer side: sleeps on the progress bell until `ready()` holds. `want` tells the worker when to ring: the free
  // bytes needed, kWantEmpty, or kWantNothing (a VBlank done always rings). The want/snapshot/re-check/wait order
  // pairs with the worker's release/check/ring order (all sequentially consistent), so no wakeup is lost.
  // waitMutex_ keeps one waiter at a time (a window message may drain while the EE waits for space).
  template <typename Ready> void wait_for(Ready ready, std::uint64_t want) {
    std::lock_guard<std::mutex> waiting(waitMutex_);
    want_.store(want, std::memory_order_seq_cst);
    for (;;) {
      const std::uint32_t seen = progress_.snapshot();
      if (ready()) break;
      bool slept = false;
      progress_.wait_changed(seen, slept);
    }
    want_.store(kWantNothing, std::memory_order_seq_cst);
  }

  // Worker side, after a release: rings the progress bell when what the producer waits for has happened.
  void notify_progress(bool vsyncDone) {
    const std::uint64_t want = want_.load(std::memory_order_seq_cst);
    if (want == kWantNothing && !vsyncDone) return;
    const std::uint64_t used = ring_.head() - ring_.tail();
    const bool ready = vsyncDone || (want == kWantEmpty ? used == 0 : ring_.capacity() - used >= want);
    if (ready) progress_.ring();
  }

  void run() {
    sampler::name_current_thread("gs-worker");
    thread_placement::apply("gs");
    g_tsc_start_time = std::chrono::steady_clock::now();
    g_tsc_start = gt4::platform::cycle_counter();
    for (;;) {
      mtvu::RecordHeader header;
      const std::uint8_t *payload = nullptr;
      if (!ring_.try_peek(header, payload)) {
        const std::uint32_t seen = work_.snapshot();
        if (!ring_.try_peek(header, payload)) {
          const std::uint64_t start = gt4::platform::cycle_counter();
          const auto idleStart = std::chrono::steady_clock::now();
          sleeping_.store(true, std::memory_order_relaxed);
          bool slept = false;
          work_.wait_changed(seen, slept);
          sleeping_.store(false, std::memory_order_relaxed);
          g_gs_idle += std::chrono::steady_clock::now() - idleStart;
          add(g_transport.idle_ticks, gt4::platform::cycle_counter() - start);
          if (slept) add(g_transport.wakeups, 1);
          continue;
        }
      }
      const SinkRecord kind = static_cast<SinkRecord>(header.kind);
      const std::uint64_t busyStart = gt4::platform::cycle_counter();
      if (kind == SinkRecord::MtvuJob) {
        flare_gap_job(header.job);  // TASK-178 diagnostics
        // IN-461: the job's records, outside the backend lock while the worker waits for them.
        const std::uint64_t waited = execute_mtvu_job(header.job);
        const std::uint64_t busy = gt4::platform::cycle_counter() - busyStart - waited;
        add(g_transport.busy_ticks[0], busy);
        add(g_transport.mtvu_wait_ticks, waited);
        flare_gap_busy(busy);
        ring_.release();
        notify_progress(false);
        continue;
      }
      std::uint64_t flareTicket = 0;  // TASK-178 diagnostics: the flare read this record finished
      {
        std::lock_guard<std::mutex> backendLock(backendMutex_);
        switch (kind) {
          case SinkRecord::Gif: gif_now(header.path, payload, header.bytes); break;
          case SinkRecord::Privileged: {
            std::uint32_t address = 0;
            std::memcpy(&address, payload, sizeof address);
            privileged_now(address, header.job);
            break;
          }
          case SinkRecord::Vsync: vsync_now(header.job != 0); break;
          case SinkRecord::GsReset: gs_reset_now(); break;
          case SinkRecord::GifReset: gif_reset_now(); break;
          case SinkRecord::MtvuJob: break;  // executed above, outside the lock
          case SinkRecord::SkippedRead: {  // IN-477: the EE already has its answer; the read still leaves the GS FIFO
            std::vector<std::uint8_t> &scratch = skippedRead_;
            scratch.resize(static_cast<std::size_t>(header.job) * 16u);
            read_fifo_now(scratch.data(), static_cast<std::uint32_t>(header.job));
            break;
          }
          case SinkRecord::FlareRead: {  // IN-514, IN-522
            FlareTableReads::Table table;
            read_fifo_now(table.data(), FlareTableReads::kQwords);
            if (std::uint8_t *dest = flareReads_.next_dest()) std::memcpy(dest, table.data(), table.size());
            flareTicket = flareReads_.finish(table);
            break;
          }
        }
      }
      const int slot = kind == SinkRecord::Gif ? 0 : kind == SinkRecord::Privileged ? 1 : kind == SinkRecord::Vsync ? 2 : 3;
      const std::uint64_t busy = gt4::platform::cycle_counter() - busyStart;
      add(g_transport.busy_ticks[slot], busy);
      if (flareTicket != 0) {
        flare_gap_read(flareTicket);
      } else {
        flare_gap_busy(busy);
      }
      ring_.release();
      const bool vsyncDone = kind == SinkRecord::Vsync;
      if (vsyncDone) {
        gt4::pacing::gs_vsyncs_queued.store(static_cast<std::uint32_t>(queuedVsyncs_.fetch_sub(1, std::memory_order_seq_cst) - 1),
                                            std::memory_order_relaxed);
      }
      notify_progress(vsyncDone);
    }
  }

  // IN-461: executes VU1 job `job`'s GIF records from the MTVU output ring up to its JobDone, then frees them (the same
  // bytes the EE journal forwarded through gif() before; this thread is now the ring's one reader). Sleeps on the
  // ring's bell while the VU1 worker has not published the next record; the backend lock is taken per record, never
  // while sleeping. Returns the ticks spent waiting for the VU1 worker.
  std::uint64_t execute_mtvu_job(std::uint64_t job) {
    std::uint64_t waited = 0;
    for (;;) {
      mtvu::RecordHeader record;
      const std::uint8_t *bytes = nullptr;
      if (!mtvuOutput_->try_peek(record, bytes)) {
        const std::uint32_t seen = mtvuBell_->snapshot();
        if (!mtvuOutput_->try_peek(record, bytes)) {
          const std::uint64_t start = gt4::platform::cycle_counter();
          bool slept = false;
          mtvuBell_->wait_changed(seen, slept);
          waited += gt4::platform::cycle_counter() - start;
          if (slept) add(g_transport.mtvu_sleeps, 1);
          continue;
        }
      }
      const auto kind = static_cast<mtvu::RecordKind>(record.kind);
      if (record.job != job || (kind != mtvu::RecordKind::Gif && kind != mtvu::RecordKind::JobDone)) {
        std::fprintf(stderr, "[hw] IN-461: VU1 output record kind %u of job %llu while executing job %llu; stopping\n",
                     static_cast<unsigned>(record.kind), static_cast<unsigned long long>(record.job),
                     static_cast<unsigned long long>(job));
        std::exit(EXIT_FAILURE);
      }
      if (kind == mtvu::RecordKind::JobDone) {
        mtvuOutput_->release();
        add(g_transport.mtvu_jobs, 1);
        return waited;
      }
      {
        std::lock_guard<std::mutex> backendLock(backendMutex_);
        gif_now(record.path, bytes, record.bytes);
      }
      add(g_transport.mtvu_records, 1);
      mtvuOutput_->release();
    }
  }

  StreamRing ring_;
  mtvu::SpscByteRing *mtvuOutput_ = nullptr;  // IN-461: the VU1 worker's output ring (set by the EE, see post_mtvu_job)
  mtvu::Doorbell *mtvuBell_ = nullptr;
  mtvu::Doorbell work_;      // EE -> worker: records published (wakes the worker only when it sleeps)
  mtvu::Doorbell progress_;  // worker -> EE: space freed, a VBlank done, the ring empty (only when wanted)
  std::atomic<std::uint64_t> want_{kWantNothing};
  std::atomic<std::size_t> queuedVsyncs_{0};
  std::atomic<bool> sleeping_{false};  // the worker waits for work (TASK-027 CPU survey: pushes that wake it)
  std::uint64_t maxDepth_ = 0;         // producer only: deepest ring use, in bytes
  std::thread thread_;
  std::mutex backendMutex_;
  std::mutex waitMutex_;
};

GsWorker *g_worker = nullptr;  // null: synchronous (GT4_HW_GS_THREAD=0)

void start_gs_worker() {
  const char *thread = std::getenv("GT4_HW_GS_THREAD");
  if (!(thread && std::string(thread) == "0")) {
    g_worker = new GsWorker;  // never destroyed, like the backend
    g_worker->start();
    std::fprintf(stderr, "[hw] GS stream on its own worker thread (IN-363; GT4_HW_GS_THREAD=0 for the EE thread)\n");
  }
}

void gif(int path, const std::uint8_t *data, std::uint32_t qwc) {
  const std::size_t bytes = static_cast<std::size_t>(qwc) * 16;
  if (!g_worker) {
    gif_now(path, data, bytes);
    return;
  }
  const std::uint64_t start = gt4::platform::cycle_counter();
  g_worker->push(SinkRecord::Gif, static_cast<std::uint8_t>(path), 0, data, bytes);
  const std::uint64_t ticks = gt4::platform::cycle_counter() - start;
  add(g_transport.gif_ticks, ticks);
  add(g_transport.copy_ticks, ticks);  // the copy into the ring is the whole hook now
  add(g_transport.gif_commands, 1);
  add(g_transport.gif_bytes, bytes);
}

// IN-461 (direct GS intake): the VU1 job's place in the stream. Needs the worker thread: on the EE thread the job's
// output would be awaited before the job is even sent to the VU1 worker.
void mtvu_job(std::uint64_t job, mtvu::SpscByteRing &output, mtvu::Doorbell &bell) {
  if (!g_worker) {
    std::fprintf(stderr, "[hw] GT4_MTVU_DIRECT=1 needs the GS worker thread (GT4_HW_GS_THREAD is 0); stopping\n");
    std::exit(EXIT_FAILURE);
  }
  g_worker->post_mtvu_job(job, output, bell);
}

void privileged(std::uint32_t address, std::uint64_t value) {
  if (!g_worker) {
    privileged_now(address, value);
    return;
  }
  const std::uint64_t start = gt4::platform::cycle_counter();
  std::uint8_t payload[sizeof address];
  std::memcpy(payload, &address, sizeof address);
  g_worker->push(SinkRecord::Privileged, 0, value, payload, sizeof payload);
  add(g_transport.privileged_ticks, gt4::platform::cycle_counter() - start);
  add(g_transport.privileged_commands, 1);
}

// IN-477 (performance mode): GT4 reads its light flare visibility table once per frame, 128 qwords, and the renderer
// answers it with zeros (IN-441). Waiting for the GS worker to reach that read only served to take the zeros: the EE was
// blocked for most of the GS work of every frame (10-16 ms per VBlank on the i5-7200U). The same rule as the GSdx path's
// (`local_readback`, gs_stream_client.cpp). GT4_HW_FLARE_NO_WAIT=0 waits as before (A/B).
bool flare_table_answered_at_once() {
  static const bool on = [] {
    // TASK-180 test5: the flares are one effect toggle (GT4_CORE2_PERF_FLARES=1 hides them); the older names still work
    // (GT4_GS_HIDE_FLARES is the A/B and the GSdx switch; GT4_CORE2_PERFORMANCE turns every item off).
    const char *hide = std::getenv("GT4_GS_HIDE_FLARES");
    const char *effect = std::getenv("GT4_CORE2_PERF_FLARES");
    const char *performance = std::getenv("GT4_CORE2_PERFORMANCE");
    const bool hidden = hide != nullptr        ? std::strcmp(hide, "0") != 0
                        : effect != nullptr    ? std::strcmp(effect, "0") != 0
                        : performance != nullptr && std::strcmp(performance, "0") != 0;
    const char *noWait = std::getenv("GT4_HW_FLARE_NO_WAIT");
    return hidden && (noWait == nullptr || std::strcmp(noWait, "0") != 0);
  }();
  return on;
}

// IN-514 (normal mode, with a window): the EE waits for the flare table only until this long after the VBlank, the time
// that still leaves it room for the rest of its frame. In GT4 the EE waits at this read for most of the GS work of the
// frame, so a heavy stretch (a car ahead on George V Paris at 6x: GS 17 ms) cost the frame rate, while Performance mode, which
// does not wait, stayed at 60 FPS. A late answer is the last finished table: a flare is one or two frames late in such a frame
// only (gotchas/flare-readback-one-frame-lag), and the VU1 alone reads the table. Headless runs always wait, so frames stay
// comparable. GT4_HW_FLARE_DEADLINE_MS=<ms> fixes the limit, 0 always waits (A/B).
std::atomic<bool> g_window_attached{false};
FlareDeadline g_flare_deadline;  // EE only
double g_flare_answer_ms = -1.0;  // EE only: this frame's read, -1 without one
std::uint64_t g_ee_vblanks = 0;  // EE only
std::chrono::steady_clock::time_point g_ee_last_vblank;  // EE only
double flare_deadline_ms() {
  static const double fixed = [] {
    const char *text = std::getenv("GT4_HW_FLARE_DEADLINE_MS");
    return text != nullptr ? std::atof(text) : -1.0;
  }();
  return fixed >= 0.0 ? fixed : g_flare_deadline.ms();
}

// IN-522 (normal mode): the EE does not wait for the flare table at all. It waited for most of the GS work of the frame there,
// 8.5 ms per VBlank at 6x, though it never reads the table: only the VU1 does, from a VIF packet after it in the stream, and
// that packet now waits for the words on the VU1 worker. So the flares are exact and no answer is late. Needs direct GS intake
// (the VU1 transport sets g_deferred_table.note). GT4_HW_FLARE_DEFER=0: the read waits, with the limit of IN-514 (A/B).
bool flare_table_deferred() {
  static const bool on = [] {
    const char *text = std::getenv("GT4_HW_FLARE_DEFER");
    return text == nullptr || std::strcmp(text, "0") != 0;
  }();
  return on && gt4::gs_stream::g_deferred_table.note != nullptr;
}

// The VU1 worker, for a VIF packet that uses a deferred read: the worker exists, the read was posted through it.
void wait_copy_flare_table(std::uint64_t ticket, std::size_t offset, std::size_t bytes, std::uint8_t *out) {
  g_worker->wait_copy_flare_table(ticket, offset, bytes, out);
}

void read_fifo(std::uint8_t *data, std::uint32_t qwords) {
  if (!g_worker) {
    read_fifo_now(data, qwords);
    return;
  }
  if (qwords == FlareTableReads::kQwords && flare_table_answered_at_once()) {
    std::memset(data, 0, static_cast<std::size_t>(qwords) * 16u);
    g_worker->post_skipped_read(qwords);
    add(g_transport.read_fifos, 1);
    return;
  }
  if (qwords == FlareTableReads::kQwords && flare_table_deferred()) {
    const std::uint64_t ticket = g_worker->post_flare_table(data, g_ee_vblanks);
    gt4::gs_stream::g_deferred_table.note(data, FlareTableReads::kBytes, ticket);
    add(g_transport.read_fifos, 1);
    add(g_transport.flare_deferred, 1);
    return;
  }
  if (qwords == FlareTableReads::kQwords && flare_deadline_ms() > 0.0 && g_window_attached.load(std::memory_order_relaxed) &&
      g_ee_vblanks != 0) {
    const std::uint64_t start = gt4::platform::cycle_counter();
    const auto startTime = std::chrono::steady_clock::now();
    const auto deadline = g_ee_last_vblank + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                 std::chrono::duration<double, std::milli>(flare_deadline_ms()));
    const bool late = g_worker->read_flare_table(data, g_ee_vblanks, deadline);
    const auto endTime = std::chrono::steady_clock::now();
    add(g_transport.read_fifo_ticks, gt4::platform::cycle_counter() - start);
    add(g_transport.read_fifos, 1);
    add(g_transport.flare_reads, 1);
    if (late) add(g_transport.flare_late, 1);
    add(g_transport.flare_start_us, static_cast<std::uint64_t>(
                                        std::chrono::duration_cast<std::chrono::microseconds>(startTime - g_ee_last_vblank).count()));
    add(g_transport.flare_answer_us, static_cast<std::uint64_t>(
                                         std::chrono::duration_cast<std::chrono::microseconds>(endTime - g_ee_last_vblank).count()));
    g_flare_answer_ms = std::chrono::duration<double, std::milli>(endTime - g_ee_last_vblank).count();
    return;
  }
  const std::uint64_t start = gt4::platform::cycle_counter();
  {
    const std::unique_lock<std::mutex> lock = g_worker->drain();  // the read observes everything queued before it
    read_fifo_now(data, qwords);
  }
  add(g_transport.read_fifo_ticks, gt4::platform::cycle_counter() - start);
  add(g_transport.read_fifos, 1);
}

void vsync(bool odd_field) {
  g_vsync_pushed[++g_ee_vsyncs_pushed % g_vsync_pushed.size()].store(std::chrono::steady_clock::now(), std::memory_order_release);
  if (!g_worker) {
    vsync_now(odd_field);
    return;
  }
  g_worker->push(SinkRecord::Vsync, 0, odd_field ? 1 : 0, nullptr, 0);
  if (g_flare_answer_ms >= 0.0 && gt4::pacing::ee_slack_us != gt4::pacing::kNoSlack) {
    g_flare_deadline.frame(g_flare_answer_ms, gt4::pacing::ee_slack_us / 1000.0);
  }
  g_flare_answer_ms = -1.0;
  ++g_ee_vblanks;
  g_ee_last_vblank = std::chrono::steady_clock::now();
  if (g_ee_vblanks % 600 == 0) print_flare_table();
  add(g_transport.vsync_commands, 1);
}

void gs_reset() {
  if (!g_worker) {
    gs_reset_now();
    return;
  }
  g_worker->push(SinkRecord::GsReset, 0, 0, nullptr, 0);
}

void gif_reset() {
  if (!g_worker) {
    gif_reset_now();
    return;
  }
  g_worker->push(SinkRecord::GifReset, 0, 0, nullptr, 0);
}

bool attach_window(std::uint64_t window, std::uint32_t width, std::uint32_t height) {
  std::unique_lock<std::mutex> lock;
  if (g_worker) lock = g_worker->drain();
  std::string error;
  if (!backend().attach_window(reinterpret_cast<void *>(window), error)) {
    std::fprintf(stderr, "[hw] window presentation refused: %s\n", error.c_str());
    return false;
  }
  backend().resize_window(width, height);
  g_window_attached.store(true, std::memory_order_relaxed);
  std::fprintf(stderr, "[hw] window output through the HWR swapchain\n");
  return true;
}

// TASK-080: the race view's display-aspect fill (protocol 1.3 CONTROL display_aspect in pcsx2 mode). Called on the EE
// thread only when the request changes; the backend reads it atomically at its next present, so no drain is needed.
std::uint64_t g_display_aspect_changes = 0;
void display_aspect(bool fill, std::uint32_t ratio_millionths) {
  backend().set_display_aspect(fill, ratio_millionths);
  ++g_display_aspect_changes;
  if (!fill) {
    std::fprintf(stderr, "[hw] presentation: 16:9 pillarbox\n");
  } else if (ratio_millionths == 0) {
    std::fprintf(stderr, "[hw] presentation: fill the whole window\n");
  } else {
    std::fprintf(stderr, "[hw] presentation: fill at ratio %.4f\n", ratio_millionths / 1e6);
  }
}

void resize_window(std::uint32_t width, std::uint32_t height) {
  std::unique_lock<std::mutex> lock;
  if (g_worker) lock = g_worker->drain();
  backend().resize_window(width, height);
}

#ifdef __ANDROID__
// TASK-180: see gs_stream_sink.hpp. Called on the EE thread while the game is parked.
void detach_window() {
  std::unique_lock<std::mutex> lock;
  if (g_worker) lock = g_worker->drain();
  backend().detach_window();
  g_window_attached.store(false, std::memory_order_relaxed);
  std::fprintf(stderr, "[hw] window output detached (the surface goes away)\n");
}
#endif

RenderCounters stats_counters() { return g_backend ? g_backend->render_counters() : RenderCounters{}; }

void finish(std::FILE *output) {
  if (!g_backend) return;
  sampler::finish(output);
  std::unique_lock<std::mutex> lock;
  if (g_worker) lock = g_worker->drain();
  g_backend->wait_idle();
  const Stats stats = g_backend->stats();
  std::fprintf(output,
               "[hw] session: vsyncs=%llu frames=%llu presents=%llu black_presents=%llu gif_bytes=%llu "
               "privileged=%llu unmirrored_privileged=%llu faults=%llu\n",
               static_cast<unsigned long long>(g_vsyncs), static_cast<unsigned long long>(stats.frames),
               static_cast<unsigned long long>(stats.presents), static_cast<unsigned long long>(stats.black_presents),
               static_cast<unsigned long long>(stats.gif_bytes),
               static_cast<unsigned long long>(stats.privileged_writes),
               static_cast<unsigned long long>(g_unmirrored_privileged), static_cast<unsigned long long>(stats.faults));
  std::fprintf(output,
               "[hw] m6: flushes=%llu held_presents=%llu dropped_frames=%llu auxiliary_frames=%llu translate_us=%.1f "
               "execute_us=%.1f compose_us=%.1f upload_us=%.1f present_us=%.1f\n",
               static_cast<unsigned long long>(stats.flushes),
               static_cast<unsigned long long>(stats.held_presents),
               static_cast<unsigned long long>(stats.dropped_frames),
               static_cast<unsigned long long>(stats.auxiliary_frames), stats.translate_us, stats.execute_us,
               stats.compose_us, stats.upload_us, stats.present_us);
  // TASK-080: fill requests received and the rectangle of the last present (window only).
  std::fprintf(output, "[hw] presentation: aspect_changes=%llu last_rect=%u,%u %ux%u fill=%u\n",
               static_cast<unsigned long long>(g_display_aspect_changes), stats.present_left, stats.present_top,
               stats.present_width, stats.present_height, stats.present_fill);
  print_presentation(output, "session", stats, Stats{});  // TASK-174
  const RenderCounters counters = stats_counters();
  std::fprintf(output,
               "[hw] counters: batches=%llu primitives=%llu primitives_skipped_texture=%llu batches_empty=%llu "
               "textures=%llu texels=%llu gpu_draws=%llu writeback_pixels=%llu flat_deindexed=%llu sprites=%llu\n",
               static_cast<unsigned long long>(counters.batches),
               static_cast<unsigned long long>(counters.primitives),
               static_cast<unsigned long long>(counters.primitives_skipped_texture),
               static_cast<unsigned long long>(counters.batches_empty),
               static_cast<unsigned long long>(counters.textures),
               static_cast<unsigned long long>(counters.texels),
               static_cast<unsigned long long>(counters.gpu_draws),
               static_cast<unsigned long long>(counters.writeback_pixels),
               static_cast<unsigned long long>(counters.flat_deindexed),
               static_cast<unsigned long long>(counters.sprites));
  {
    const RenderCounters &video = counters;
    std::fprintf(output, "[hw] video: vblanks_480i=%llu vblanks_480p=%llu (SMODE2.INT; menus are 480i, races 480p with "
                         "GT4_FORCE_480P=1)\n",
                 static_cast<unsigned long long>(video.vblanks_interlaced),
                 static_cast<unsigned long long>(video.vblanks_progressive));
  }
  std::fprintf(output,
               "[hw] output: presentation_frames=%llu blank_frames=%llu deinterlaced_frames=%llu "
               "interlace_offset_frames=%llu merge_alpha_from_texel=%llu writeback_depth_words=%llu "
               "fifo_overruns=%llu\n",
               static_cast<unsigned long long>(counters.presentation_frames),
               static_cast<unsigned long long>(counters.blank_frames),
               static_cast<unsigned long long>(counters.deinterlaced_frames),
               static_cast<unsigned long long>(counters.interlace_offset_frames),
               static_cast<unsigned long long>(counters.merge_alpha_from_texel),
               static_cast<unsigned long long>(counters.writeback_depth_words),
               static_cast<unsigned long long>(counters.fifo_overruns));
  std::fprintf(output, "[hw] tone: strip_pages=%llu strip_batches=%llu verify_mismatch_bytes=%llu (IN-383)\n",
               static_cast<unsigned long long>(counters.tone_strip_pages),
               static_cast<unsigned long long>(counters.tone_strip_batches),
               static_cast<unsigned long long>(counters.tone_verify_mismatch_bytes));
  if (counters.scale > 1) {
    std::fprintf(output,
                 "[hw] scale: x%u draws=%llu passes=%llu refreshes=%llu refresh_pixels=%llu composes=%llu "
                 "circuits_from_slots=%llu circuits_from_glm=%llu circuits_from_shadows=%llu shadow_saves=%llu "
                 "tone_runs=%llu tone_carries=%llu\n",
                 counters.scale, static_cast<unsigned long long>(counters.scaled_draws),
                 static_cast<unsigned long long>(counters.scaled_passes),
                 static_cast<unsigned long long>(counters.scaled_refreshes),
                 static_cast<unsigned long long>(counters.scaled_refresh_pixels),
                 static_cast<unsigned long long>(counters.scaled_composes),
                 static_cast<unsigned long long>(counters.scaled_circuits_from_slots),
                 static_cast<unsigned long long>(counters.scaled_circuits_from_glm),
                 static_cast<unsigned long long>(counters.scaled_circuits_from_shadows),
                 static_cast<unsigned long long>(counters.scaled_shadow_saves),
                 static_cast<unsigned long long>(counters.tone_runs),
                 static_cast<unsigned long long>(counters.tone_carries));
  }
  if (g_dump_every != 0) {
    std::fprintf(output, "[hw] dumps: written=%llu without_image=%llu every=%u dir=%s\n",
                 static_cast<unsigned long long>(g_dumps), static_cast<unsigned long long>(g_dumps_without_image),
                 g_dump_every, g_dump_dir.c_str());
  }
  std::fflush(output);
}

}  // namespace
}  // namespace gt4::hw

namespace gt4::gs_stream {

const InProcessSink &hw_in_process_sink() {
  static const InProcessSink sink = [] {
    InProcessSink s;
    s.start = &gt4::hw::start_backend;
    s.gif = &gt4::hw::gif;
    s.privileged = &gt4::hw::privileged;
    s.read_fifo = &gt4::hw::read_fifo;
    g_deferred_table.wait_copy = &gt4::hw::wait_copy_flare_table;
    s.vsync = &gt4::hw::vsync;
    s.gs_reset = &gt4::hw::gs_reset;
    s.gif_reset = &gt4::hw::gif_reset;
    s.attach_window = &gt4::hw::attach_window;
    s.resize_window = &gt4::hw::resize_window;
    s.display_aspect = &gt4::hw::display_aspect;
    s.finish = &gt4::hw::finish;
#ifdef __ANDROID__
    s.detach_window = &gt4::hw::detach_window;
#endif
    s.mtvu_job = &gt4::hw::mtvu_job;
    return s;
  }();
  return sink;
}

}  // namespace gt4::gs_stream

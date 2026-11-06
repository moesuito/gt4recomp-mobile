// SPDX-License-Identifier: MIT
// Game-side GS stream backend; see gs_stream_client.hpp and docs/GS-STREAM-PROTOCOL.md section 7.
#include "gs_stream_client.hpp"
#include "gs_stream_sink.hpp"
#ifdef GT4_GS_STREAM_ARMSX2
#include "armsx2_gs_sink.hpp"
#endif

// TASK-180: the GS process path (GT4_RENDERER=pcsx2, gt4-gs.exe over shared memory) and the stream capture files exist
// only on Windows. Elsewhere the client is the in-process CoreGS 2.0 sink alone (GT4_RENDERER=hw).
#ifdef _WIN32
#define GT4_GS_STREAM_PROCESS 1
#endif

#ifdef GT4_GS_STREAM_PROCESS
#include "gs_readback_probe.hpp"
#include "gs_stream_file.hpp"
#include "gs_stream_ipc.hpp"
#endif
#include "perf_probe.hpp"
#include "../install_paths.hpp"
#include "../display_aspect.hpp"

#ifdef GT4_GS_STREAM_PROCESS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#else
#include "../platform/platform.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#ifdef GT4_GS_STREAM_PROCESS
#pragma comment(lib, "bcrypt.lib")
#endif

namespace gt4::gs_stream {
namespace {

// TASK-084 step 3: when the MTVU journal owns the VBlank, the client holds the field here instead of posting.
bool g_defer_vsync = false;
bool g_pending_vsync = false;
bool g_has_pending_vsync = false;

namespace proto = ::gs_stream;
#ifdef GT4_GS_STREAM_PROCESS
namespace ipc = ::gs_stream::ipc;
#endif
using proto::RecordType;

// The serial the producer announces in the control block and in stream files. The consumer uses it only to select
// per-game settings (a GS renderer's game database); it never changes the meaning of a record.
constexpr const char *kGameSerial = "SCUS-97328";
constexpr std::uint32_t kHandshakeTimeoutMs = 20000;
// GT4_GS_HANDSHAKE_TIMEOUT_MS (tests): 1000..60000, default kHandshakeTimeoutMs.
constexpr std::uint32_t kHandshakeTimeoutMinMs = 1000, kHandshakeTimeoutMaxMs = 60000;
// After the GS process died the guest keeps running this many VSYNCs (records are dropped, readbacks answered with
// zeros) before it halts: the runtime writes a changed memory card after 50 quiet VSYNCs, so a save in progress is
// finished and flushed first (TASK-038). GT4_GS_LOST_GRACE_FRAMES (tests): 0..600.
constexpr std::uint32_t kLostGraceFrames = 180, kLostGraceMaxFrames = 600;
// Printed when the player chose CoreGS 2.0 after GSdx failed; the launcher looks for this line and stores the choice.
constexpr const char *kRestartMarker = "[gs-ipc] renderer change requested: CoreGS 2.0";
constexpr std::uint32_t kShutdownTimeoutMs = 3000;
// GIF data per record and qwords per READ_FIFO: fixed by the default region sizes, so the in-process capture and a
// recorder process split the same chunks the same way (byte-identical files).
constexpr std::uint64_t kGifChunkBytes =
    (proto::kDefaultCommandCapacity / 2 - sizeof(proto::RecordHeader) - sizeof(proto::GifPayload)) & ~std::uint64_t(15);
constexpr std::uint32_t kReadFifoQwordsPerRequest = static_cast<std::uint32_t>(proto::kDefaultResponseCapacity / 16);
// TASK-116 step 1: the flare visibility table the night race reads back once per frame: 64x8 PSMCT32 = 2048 bytes = 128
// qwords (docs/GS-READBACK-STUDY.md section 1.1). Unique among every readback measured in the P0 probe (car icons 256
// and 768, thumbnail 512, loading picture 26,880), so the size is the allow-list key.
constexpr std::uint32_t kFlareReadbackQwords = 128;
constexpr std::uint32_t kFlareReadbackBytes = kFlareReadbackQwords * 16;
// HWDownloadMode value of the launcher's Performance mode (docs/launcher/gs-settings.toml: no-readbacks = 2).
constexpr int kNoReadbacksMode = 2;
// TASK-116 step 1: GT4_GS_READBACK_LOCAL (A/B switch, on by default); 0 serves every readback as before.
constexpr const char *kReadbackLocalSwitch = "GT4_GS_READBACK_LOCAL";
// TASK-117: GT4_GS_HIDE_FLARES (on with the local answer, the launcher's Performance mode): the flare table is answered
// as zeros, so the VU1 draws no light flare and no sun glare at all. 0 = the previous reply (the rc6 step-1 look).
constexpr const char *kHideFlaresSwitch = "GT4_GS_HIDE_FLARES";

struct Counters {
  std::uint64_t records = 0, bytes = 0, gif_records = 0, gif_bytes = 0, priv = 0, vsyncs = 0, readbacks = 0,
                readback_bytes = 0, controls = 0, resets = 0;
};

// TASK-041: privileged writes by register: writes, EE wait of the blocking MTVU sync before them, and writes queued
// behind outstanding VU1 worker output (GT4_MTVU_PRIV_ASYNC).
struct PrivilegedSlot {
  std::uint64_t writes = 0, wait_us = 0, queued = 0;
};
using PrivilegedSlots = std::array<PrivilegedSlot, kPrivilegedRegisterSlots>;

// Installed by the MTVU bridge in pcsx2 mode; empty otherwise (every write is posted at once, as before).
PrivilegedOrdering g_privileged_ordering;
// TASK-062: cumulative VU1 worker busy time (MTVU bridge, pcsx2 mode); null: unknown.
std::uint64_t (*g_worker_busy_us)() = nullptr;
// TASK-060: cursor policy of the game window, sent to a 1.2 consumer after attach_window (set by window.cpp).
bool g_cursor_auto_hide = true;
std::uint32_t g_cursor_hide_delay_ms = 3000;
// TASK-080: display aspect wanted by the game (set_display_aspect_mode), posted after a VSYNC to a 1.3 consumer.
// Low 32 bits: the mode; high 32 bits: the fill target aspect x 1e6 (0 = the whole window; P2, slice217).
std::atomic<std::uint64_t> g_display_aspect_wanted{0};
// TASK-116 step 0 (P0 gate): the guest RAM window of the readback log's destination column (set_guest_ram).
std::uint8_t *g_guest_ram = nullptr;
std::uint32_t g_guest_ram_size = 0;
// TASK-116 step 1: the MTVU journal services of the local flare answer (set_readback_journal).
ReadbackJournal g_readback_journal{};

#ifdef GT4_GS_STREAM_PROCESS
// CPU time of the calling thread in microseconds (UINT64_MAX when unavailable).
std::uint64_t thread_cpu_us() {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return UINT64_MAX;
  const auto ticks = [](FILETIME value) { return (std::uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime; };
  return ticks(kernel) / 10 + ticks(user) / 10;
}

// Nanoseconds between two QueryPerformanceCounter values.
std::uint64_t elapsed_ns(LARGE_INTEGER from, LARGE_INTEGER to) {
  static const std::uint64_t frequency = [] {
    LARGE_INTEGER value;
    QueryPerformanceFrequency(&value);
    return static_cast<std::uint64_t>(value.QuadPart);
  }();
  const std::uint64_t ticks = static_cast<std::uint64_t>(to.QuadPart - from.QuadPart);
  return ticks / frequency * 1000000000ull + ticks % frequency * 1000000000ull / frequency;
}
#endif

constexpr const char *kPrivilegedNames[kPrivilegedRegisterSlots] = {
    "PMODE", "SMODE1", "SMODE2", "SRFSH", "SYNCH1", "SYNCH2", "SYNCV", "DISPFB1", "DISPLAY1", "DISPFB2",
    "DISPLAY2", "EXTBUF", "EXTDATA", "EXTWRITE", "BGCOLOR", "CSR", "IMR", "BUSDIR", "SIGLBLID", "other"};

#ifdef GT4_GS_STREAM_PROCESS
// A visible top-level window of this process (the game window exists before the GS process starts), or null.
HWND visible_game_window() {
  struct Search {
    DWORD pid;
    HWND found;
  } search{GetCurrentProcessId(), nullptr};
  EnumWindows(
      [](HWND window, LPARAM parameter) -> BOOL {
        auto *s = reinterpret_cast<Search *>(parameter);
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid == s->pid && IsWindowVisible(window) && !GetWindow(window, GW_OWNER)) {
          s->found = window;
          return FALSE;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&search));
  return search.found;
}

std::wstring wide_text(const std::string &text) { return ipc::widen(text); }

// Offers CoreGS 2.0 when GSdx failed. Under the launcher, yes prints the marker and the launcher stores the choice. A game started
// directly has no launcher to do that, so it only says where the choice is.
void offer_restart(HWND window, const std::wstring &what) {
  if (std::getenv("GT4_DIRECT_START") != nullptr) {
    const std::wstring direct = what + L"\n\nDetails are in the game log.\n\nOpen GT4Recomp and choose CoreGS 2.0 in Settings > Graphics: it is part of the game "
                                       L"and needs no download.";
    MessageBoxW(window, direct.c_str(), L"GT4Recomp: GS renderer", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    return;
  }
  const std::wstring text = what + L"\n\nDetails are in the game log.\n\nUse CoreGS 2.0 from now on? It is part of the game and needs no download. You can "
                                   L"change this in Settings > Graphics.";
  const int answer = MessageBoxW(window, text.c_str(), L"GT4Recomp: GS renderer", MB_YESNO | MB_ICONERROR | MB_SETFOREGROUND);
  if (answer == IDYES) {
    std::fprintf(stderr, "%s\n", kRestartMarker);
    std::fflush(stderr);
  }
}

[[noreturn]] void refuse(const std::string &message) {
  std::fprintf(stderr, "[gs-ipc] %s; refusing to start\n", message.c_str());
  std::fflush(stderr);
  // The game window is already open in a windowed run: say why it closes (headless runs have no window).
  if (HWND window = visible_game_window())
    offer_restart(window, L"The GS renderer (gt4-gs.exe) could not start:\n" + wide_text(message) + L".");
  std::exit(EXIT_FAILURE);
}
#else
[[noreturn]] void refuse(const std::string &message) {
  std::fprintf(stderr, "[gs-ipc] %s; refusing to start\n", message.c_str());
  std::fflush(stderr);
  gt4::platform::show_error("GT4Recomp: GS renderer", "The renderer could not start:\n" + message + ".");
  std::exit(EXIT_FAILURE);
}
#endif

#ifdef GT4_GS_STREAM_PROCESS

std::uint32_t environment_number(const char *name, std::uint32_t fallback, std::uint32_t low, std::uint32_t high) {
  const char *value = std::getenv(name);
  if (!value || !*value) return fallback;
  char *end = nullptr;
  const unsigned long parsed = std::strtoul(value, &end, 10);
  if (*end || parsed < low || parsed > high)
    refuse(std::string(name) + "=\"" + value + "\" is not a number from " + std::to_string(low) + " to " + std::to_string(high));
  return static_cast<std::uint32_t>(parsed);
}

// TASK-116 step 1: GT4_GS_READBACK_LOCAL, the A/B switch of the local flare answer. Unset keeps the rc6 marker (on with
// gt4-gs HWDownloadMode 2, the launcher's old Performance mode); 1 or 0 force it on or off with any download mode, so the
// other readbacks (car icons, loading picture, thumbnail) can stay Accurate while the flare table is answered locally.
bool readback_local_switch(int download_mode) {
  const char *value = std::getenv(kReadbackLocalSwitch);
  if (!value || !*value) return download_mode == kNoReadbacksMode;
  if (std::strcmp(value, "1") == 0) return true;
  if (std::strcmp(value, "0") == 0) return false;
  refuse(std::string(kReadbackLocalSwitch) + "=\"" + value + "\" is not 0 or 1");
}

// TASK-117: GT4_GS_HIDE_FLARES, the A/B switch of the no-flare traffic. Unset = `fallback` (on with the local answer);
// 0 keeps the previous reply, so the night lights and the sun glare come back exactly as in rc6 step 1.
bool hide_flares_switch(bool fallback) {
  const char *value = std::getenv(kHideFlaresSwitch);
  if (!value || !*value) return fallback;
  if (std::strcmp(value, "1") == 0) return true;
  if (std::strcmp(value, "0") == 0) return false;
  refuse(std::string(kHideFlaresSwitch) + "=\"" + value + "\" is not 0 or 1");
}

// Readback wait times of one F8 interval or of the session (EE wait for each READ_FIFO reply).
struct ReadbackWaits {
  static constexpr int kBuckets = 8;  // <0.25, <0.5, <1, <2, <4, <8, <16, >=16 ms
  std::uint64_t count = 0, total_us = 0, max_us = 0;
  std::uint64_t buckets[kBuckets] = {};

  void add(std::uint64_t us) {
    ++count;
    total_us += us;
    max_us = std::max(max_us, us);
    int bucket = 0;
    for (std::uint64_t limit = 250; bucket < kBuckets - 1 && us >= limit; limit *= 2) ++bucket;
    ++buckets[bucket];
  }

  void print(std::FILE *output, const char *label) const {
    std::fprintf(output,
                 "[gs-ipc] %s readback waits: %llu, mean %.3f ms, max %.3f ms, buckets(ms) <0.25=%llu 0.25-0.5=%llu "
                 "0.5-1=%llu 1-2=%llu 2-4=%llu 4-8=%llu 8-16=%llu >=16=%llu\n",
                 label, static_cast<unsigned long long>(count), count ? total_us / 1000.0 / count : 0.0, max_us / 1000.0,
                 static_cast<unsigned long long>(buckets[0]), static_cast<unsigned long long>(buckets[1]),
                 static_cast<unsigned long long>(buckets[2]), static_cast<unsigned long long>(buckets[3]),
                 static_cast<unsigned long long>(buckets[4]), static_cast<unsigned long long>(buckets[5]),
                 static_cast<unsigned long long>(buckets[6]), static_cast<unsigned long long>(buckets[7]));
  }
};

// TASK-041: FNV-1a (64-bit) over the records in stream order: type, flags, payload bytes. Equal hashes of two runs
// mean equal record streams (sequence numbers follow from the order); a test aid, not a security property.
struct StreamHash {
  bool enabled = false;
  std::uint64_t value = 0xCBF29CE484222325ull;
  std::uint64_t records = 0;

  void bytes(const void *data, std::size_t size) {
    const auto *at = static_cast<const std::uint8_t *>(data);
    for (std::size_t i = 0; i < size; ++i) {
      value ^= at[i];
      value *= 0x100000001B3ull;
    }
  }
  void add(RecordType type, std::uint16_t flags, const void *first, std::size_t first_bytes, const void *second,
           std::size_t second_bytes) {
    const std::uint16_t header[2] = {static_cast<std::uint16_t>(type), flags};
    bytes(header, sizeof header);
    if (first_bytes) bytes(first, first_bytes);
    if (second_bytes) bytes(second, second_bytes);
    ++records;
  }
};

std::uint64_t now_us() {
  LARGE_INTEGER counter, frequency;
  QueryPerformanceCounter(&counter);
  QueryPerformanceFrequency(&frequency);
  return static_cast<std::uint64_t>(counter.QuadPart / frequency.QuadPart * 1000000 +
                                    counter.QuadPart % frequency.QuadPart * 1000000 / frequency.QuadPart);
}

std::string ascii(const char *text, std::size_t size) {
  std::size_t length = 0;
  while (length < size && text[length]) ++length;
  return std::string(text, length);
}

// TASK-116 step 0: the EE RAM address of a host pointer into guest RAM (0 outside it; the probe's destination column).
std::uint32_t guest_address(const std::uint8_t *address) {
  if (!g_guest_ram) return 0;
  const auto value = reinterpret_cast<std::uintptr_t>(address);
  const auto base = reinterpret_cast<std::uintptr_t>(g_guest_ram);
  return value >= base && value - base < g_guest_ram_size ? static_cast<std::uint32_t>(value - base) : 0;
}

// TASK-059: the GS process path. GT4_GS_PROCESS wins; otherwise <executable folder>\gs\gt4-gs.exe (install_paths.cpp)
// is used when that file exists. Returns an empty string when neither is available.
std::string gs_process_path() {
  if (const char *value = std::getenv("GT4_GS_PROCESS"); value && *value) return value;
  return gt4::install::installed_gs_process().string();
}

class Client {
 public:
  Client(Renderer renderer) : renderer_(renderer) {}

  void start() {
    open_capture();
    if (renderer_ == Renderer::pcsx2) {
      start_process();
      // TASK-116 step 0 (P0 gate): which download mode the GS process was given, so a readback log says what it answers.
      report_download_mode();
      // TASK-080: in pcsx2 mode the race-view presentation goes to the GS process (CONTROL display_aspect). Installed
      // here so headless runs exercise it too; the game window (window.cpp) installs an equivalent sink later.
      gt4::display_aspect::set_presentation_sink([](bool fill, double ratio) {
        set_display_aspect_mode(fill ? DisplayAspectMode::fill : DisplayAspectMode::game_aspect,
                                fill ? aspect_millionths(ratio) : 0);
      });
    }
    gt4::perf::Probe::set_gs_ipc_hooks(&Client::report_interval_hook, &Client::reset_interval_hook);
  }

  bool stream() const { return renderer_ == Renderer::pcsx2; }

  void gif(int path, const std::uint8_t *data, std::uint32_t qwc) {
    const std::uint64_t total = static_cast<std::uint64_t>(qwc) * 16;
    std::uint64_t at = 0;
    do {
      const std::uint64_t bytes = std::min(kGifChunkBytes, total - at);
      const bool more = at + bytes < total;
      const proto::GifPayload header{static_cast<std::uint8_t>(path), {}, proto::kGifFlagSignalsApplied,
                                     static_cast<std::uint32_t>(bytes), 0};
      emit(RecordType::gif, more ? proto::kRecordFlagMore : 0, &header, sizeof header, data + at,
           static_cast<std::size_t>(bytes));
      ++counters_.gif_records;
      counters_.gif_bytes += bytes;
      at += bytes;
    } while (at < total);
  }

  void privileged(std::uint32_t address, std::uint64_t value) {
    if (address < proto::kPrivilegedBase || address >= proto::kPrivilegedBase + proto::kPrivilegedBytes) return;
    PrivilegedSlot &slot = privileged_[static_cast<std::size_t>(privileged_register_index(address))];
    ++slot.writes;
    if (g_privileged_ordering.take_wait_us) slot.wait_us += g_privileged_ordering.take_wait_us();
    // TASK-041: behind outstanding VU1 worker output the write waits in the MTVU journal, which posts it in order.
    if (g_privileged_ordering.queue && g_privileged_ordering.queue(address, value)) {
      ++slot.queued;
      return;
    }
    post_privileged(address, value);
  }

  void post_privileged(std::uint32_t address, std::uint64_t value) {
    const proto::PrivPayload payload{address & ~7u, 0, value};
    emit(RecordType::priv, 0, &payload, sizeof payload);
    ++counters_.priv;
  }

  // TASK-116 step 1: true when this READ_FIFO is the flare table answered locally (Performance mode, switch on, journal
  // registered). TASK-117: also true whenever the table is hidden, even if the journal is gone, so the no-flare answer
  // never depends on the MTVU state.
  bool local_readback(std::uint32_t qwords) const {
    if (qwords != kFlareReadbackQwords) return false;
    return hide_flares_ || (readback_local_enabled_ && journal_ready());
  }

  bool journal_ready() const {
    return g_readback_journal.queue_readback != nullptr && g_readback_journal.drain_download != nullptr;
  }

  // True when the MTVU download ordering should defer sync("download") for the readback that follows (EE thread).
  bool local_active() const { return readback_local_enabled_ && journal_ready() && stream() && session_ && !dead_; }

  // The client side of one READ_FIFO record: counters, capture and sequence. The request id in the payload is the
  // client's own counter, incremented once per record exactly like the session's.
  void note_readback_request(std::uint32_t qwords) {
    ++counters_.readbacks;
    counters_.readback_bytes += std::uint64_t(qwords) * 16;
    const proto::ReadFifoPayload request{qwords, 0, ++readback_id_};
    write_capture(RecordType::read_fifo, 0, &request, sizeof request);
    ++sequence_;
    count(sizeof request);
  }

  // The pre-rc6 path: the deferred drain (a real one when the download was deferred), then READ_FIFO and the wait.
  std::uint32_t accurate_readback(std::uint8_t *out, std::uint32_t qwords) {
    if (g_readback_journal.drain_download) g_readback_journal.drain_download();
    note_readback_request(qwords);
    gt4::perf::Scope wait(gt4::perf::Category::gs_ipc_readback_wait);
    const std::uint64_t start = now_us();
    std::uint32_t status = proto::kReadbackFailed;
    try {
      status = session_->read_fifo(qwords, out);
      if (status == proto::kReadbackZeroFilled) ++zero_filled_;
    } catch (const ipc::IpcError &error) {
      lost(error.what());
      std::memset(out, 0, std::size_t(qwords) * 16);
    }
    const std::uint64_t waited = now_us() - start;
    readback_interval_.add(waited);
    readback_session_.add(waited);
    readback_probe_.record({frame_, qwords, guest_address(out), status, waited, out}, stderr);
    return status;
  }

  // TASK-116 step 1: the flare table, one frame old. Collects the reply of the previous frame's request, copies the cache
  // into `out` and queues this frame's READ_FIFO behind the journal (no drain, no wait). The first readback of a scene
  // (no cache yet) waits as today, so the game never starts from a blank table.
  // TASK-117: in Performance mode the table is answered as zeros instead: the VU1 scales every light flare and the sun
  // by its own pixel (docs/GS-READBACK-STUDY.md 1.1), so an all-zero table draws none of them and the same-frame result
  // cannot be one frame late. The READ_FIFO is still queued, so the journal keeps deferring the download exactly as in
  // step 1; only the bytes the guest receives change. GT4_GS_HIDE_FLARES=0 restores the step-1 look.
  void local_flare_readback(std::uint8_t *out) {
    collect_flare_reply();
    if (hide_flares_) {
      std::memset(out, 0, kFlareReadbackBytes);
      if (!g_readback_journal.queue_readback(kFlareReadbackQwords)) {  // no usable journal (MTVU off): serve it accurately
        readback_local_enabled_ = false;
        accurate_readback(out, kFlareReadbackQwords);
        std::memset(out, 0, kFlareReadbackBytes);
        return;
      }
      ++local_answers_;
      ++flares_hidden_;
      readback_probe_.record({frame_, kFlareReadbackQwords, guest_address(out), proto::kReadbackOk, 0, out}, stderr);
      return;
    }
    if (!flare_cache_valid_) {
      accurate_readback(out, kFlareReadbackQwords);
      std::memcpy(flare_cache_.data(), out, flare_cache_.size());
      flare_cache_valid_ = true;
      return;
    }
    std::memcpy(out, flare_cache_.data(), flare_cache_.size());
    if (!g_readback_journal.queue_readback(kFlareReadbackQwords)) {  // no usable journal (MTVU off): serve it accurately
      readback_local_enabled_ = false;
      accurate_readback(out, kFlareReadbackQwords);
      std::memcpy(flare_cache_.data(), out, flare_cache_.size());
      return;
    }
    ++local_answers_;
    readback_probe_.record({frame_, kFlareReadbackQwords, guest_address(out), proto::kReadbackOk, 0, out}, stderr);
  }

  // Copies the reply of the request posted in the previous frame, when it is still the one in the response area.
  void collect_flare_reply() {
    if (flare_pending_id_ == 0) return;
    bool collected = false;
    if (session_ && !dead_) {
      try {
        collected = session_->readback_reply(flare_pending_id_, flare_cache_.data(), kFlareReadbackQwords);
      } catch (const ipc::IpcError &error) {
        lost(error.what());
      }
    }
    if (collected) {
      flare_cache_valid_ = true;
    } else {
      ++local_stale_;  // not answered yet or replaced by a later request: keep the older reply
    }
    flare_pending_id_ = 0;
  }

  // The MTVU journal drain posts the queued READ_FIFO here, at its stream position, without waiting for the reply.
  void post_queued_readback(std::uint32_t qwords) {
    if (!session_ || dead_) return;
    note_readback_request(qwords);
    try {
      flare_pending_id_ = session_->post_read_fifo_nowait(qwords);
    } catch (const ipc::IpcError &error) {
      lost(error.what());
    }
  }

  void clear_flare_cache() {
    flare_cache_valid_ = false;
    flare_pending_id_ = 0;
  }

  bool read_fifo(std::uint8_t *data, std::uint32_t qwords) {    for (std::uint32_t done = 0; done < qwords;) {
      const std::uint32_t part = std::min(qwords - done, kReadFifoQwordsPerRequest);
      std::uint8_t *out = data + std::size_t(done) * 16;
      if (!stream() || dead_) {
        note_readback_request(part);
        if (stream()) std::memset(out, 0, std::size_t(part) * 16);  // the GS process is gone: zeros, as before
      } else if (local_readback(part)) {
        local_flare_readback(out);
      } else {
        accurate_readback(out, part);
      }
      done += part;
    }
    return stream();
  }

  void vsync(bool odd_field) {
    const proto::VsyncPayload payload{frame_, static_cast<std::uint8_t>(odd_field ? 1 : 0), {},
                                      proto::kVsyncFlagFieldValid};
    write_capture(RecordType::vsync, 0, &payload, sizeof payload);
    ++sequence_;
    count(sizeof payload);
    ++counters_.vsyncs;
    if (session_ && !dead_) {
      gt4::perf::Scope post(gt4::perf::Category::gs_ipc_vsync_post);
      try {
        session_->post_vsync(payload);
      } catch (const ipc::IpcError &error) {
        lost(error.what());
      }
    }
    ++frame_;
    if (dead_ && !halted_ && frames_since_lost_++ >= lost_grace_frames_) {
      halted_ = true;
      if (halt_) halt_("GS process exited");
    }
    if (capture_.is_open() && frame_ > capture_last_) {
      capture_.close();
      std::fprintf(stderr, "[gs-ipc] capture closed after frame %llu\n", static_cast<unsigned long long>(capture_last_));
    }
    poll_events();
    if (osd_wanted_) osd_after_vsync();
    const std::uint64_t resize = pending_resize_.exchange(0);
    if ((resize >> 63) && stream()) control(proto::ControlKind::resize, (resize >> 32) & 0x7FFFFFFFu, resize & 0xFFFFFFFFu, 0);
    post_display_aspect();
  }

  void reset(proto::ResetKind kind, std::uint32_t mask) {
    const proto::ResetPayload payload{static_cast<std::uint32_t>(kind), mask, 0};
    emit(RecordType::reset, 0, &payload, sizeof payload);
    ++counters_.resets;
    // TASK-116 step 1: a GS hard reset means a new scene, so the flare table of the last one is not a valid previous reply.
    if (kind == proto::ResetKind::gs_hard) clear_flare_cache();
  }

  void control(proto::ControlKind kind, std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    const proto::ControlPayload payload{static_cast<std::uint32_t>(kind), 0, {a, b, c}};
    emit(RecordType::control, 0, &payload, sizeof payload);
    ++counters_.controls;
  }

  void attach(std::uint64_t window, std::uint32_t width, std::uint32_t height) {
    // The GS process draws into a child of our window: the parent must not paint over it.
    const HWND parent = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(window));
    SetWindowLongPtrW(parent, GWL_STYLE, GetWindowLongPtrW(parent, GWL_STYLE) | WS_CLIPCHILDREN);
    window_ = window;
    control(proto::ControlKind::attach_window, window, width, height);
    // TASK-060 (protocol 1.2): the GS child window applies the same cursor rule as this window. An older consumer
    // would answer an unknown CONTROL kind with an error, so it is not sent there (that consumer never hides it).
    if (session_ && ready_.version_minor >= 2)
      control(proto::ControlKind::cursor, g_cursor_auto_hide ? proto::kCursorFlagAutoHide : 0, g_cursor_hide_delay_ms, 0);
  }

  // TASK-080 (protocol 1.3): CONTROL display_aspect when the wanted mode or fill target differs from the last one sent
  // (the consumer starts in game_aspect). Effective from the next presented frame. A consumer below 1.3 is never sent
  // it. args[1] = the fill target aspect x 1e6 (P2, slice217; 0 = the whole window), see GS-STREAM-PROTOCOL 6.7.
  void post_display_aspect() {
    const std::uint64_t wanted_both = g_display_aspect_wanted.load();
    if (wanted_both == display_aspect_sent_ || !session_ || dead_) return;
    const std::uint32_t wanted = static_cast<std::uint32_t>(wanted_both);
    const std::uint32_t target = static_cast<std::uint32_t>(wanted_both >> 32);
    if (ready_.version_minor < 3) {
      if (!display_aspect_refused_logged_) {
        display_aspect_refused_logged_ = true;
        std::fprintf(stderr, "[gs-ipc] GS process speaks protocol 1.%u: display aspect mode %u not sent (needs 1.3); "
                     "the picture keeps the game's aspect\n", ready_.version_minor, wanted);
      }
      return;
    }
    control(proto::ControlKind::display_aspect, wanted, target, 0);
    display_aspect_sent_ = wanted_both;
    ++display_aspect_controls_;
  }

  void request_resize(std::uint32_t width, std::uint32_t height) {
    // A minimized window reports a 0x0 client area: keep the GS swap chain as it is until the window is restored.
    if (width == 0 || height == 0) return;
    // Bit 63 marks a pending request; the window thread may call this between VSYNCs.
    pending_resize_.store((std::uint64_t(1) << 63) | (std::uint64_t(width & 0x7FFFFFFFu) << 32) | height);
  }

  void set_halt(void (*halt)(const char *)) { halt_ = halt; }

  // TASK-116 step 0 (P0 gate): the download mode the GS process was given, read from the very ini the launcher writes
  // (GT4_GS_CONFIG), so a readback log says which mode produced the replies. Only with GT4_GS_READBACK_LOG=1.
  void report_download_mode() {
    if (!readback_probe_.enabled()) return;
    const char *path = std::getenv("GT4_GS_CONFIG");
    const int mode = download_mode_from_ini(path);
    if (mode >= 0)
      std::fprintf(stderr, "[gs-readback] gt4-gs.ini HWDownloadMode=%d (%s)\n", mode, path);
    else
      std::fprintf(stderr, "[gs-readback] gt4-gs.ini HWDownloadMode unknown (GT4_GS_CONFIG=%s)\n",
                   path && *path ? path : "unset");
  }  void finish(std::FILE *output) {
    if (finished_) return;
    finished_ = true;
    capture_.close();  // the capture holds the game's stream; the transport's shutdown request is not part of it
    if (session_ && !dead_) {
      control(proto::ControlKind::shutdown, 0, 0, 0);
      std::uint32_t code = 0;
      if (child_.wait_exit(kShutdownTimeoutMs, &code)) {
        poll_events();
        std::fprintf(output, "[gs-ipc] GS process exited with code %u after shutdown\n", code);
      } else {
        std::fprintf(output, "[gs-ipc] GS process did not exit within %u ms of shutdown; ending it\n", kShutdownTimeoutMs);
        child_.kill();
      }
    }
    const double frames = counters_.vsyncs ? static_cast<double>(counters_.vsyncs) : 1.0;
    std::fprintf(output,
                 "[gs-ipc] session: renderer %s vsyncs %llu records %llu bytes %llu gif %llu (%llu bytes) priv %llu "
                 "readbacks %llu (%llu bytes, %llu zero-filled) resets %llu controls %llu\n",
                 stream() ? "pcsx2" : "donor", static_cast<unsigned long long>(counters_.vsyncs),
                 static_cast<unsigned long long>(counters_.records), static_cast<unsigned long long>(counters_.bytes),
                 static_cast<unsigned long long>(counters_.gif_records), static_cast<unsigned long long>(counters_.gif_bytes),
                 static_cast<unsigned long long>(counters_.priv), static_cast<unsigned long long>(counters_.readbacks),
                 static_cast<unsigned long long>(counters_.readback_bytes), static_cast<unsigned long long>(zero_filled_),
                 static_cast<unsigned long long>(counters_.resets), static_cast<unsigned long long>(counters_.controls));
    std::fprintf(output, "[gs-ipc] session per frame: bytes=%.0f records=%.1f readbacks=%.3f\n", counters_.bytes / frames,
                 counters_.records / frames, counters_.readbacks / frames);
    if (session_) {
      const ipc::WaitTotals &w = session_->wait_totals();
      std::fprintf(output,
                   "[gs-ipc] session waits: space %llu (%.3f ms total), frames in flight %llu (%.3f ms total), "
                   "readback %.3f ms total\n",
                   static_cast<unsigned long long>(w.space_waits), w.space_wait_us / 1000.0,
                   static_cast<unsigned long long>(w.frames_waits), w.frames_wait_us / 1000.0, w.response_wait_us / 1000.0);
    }
    if (readback_session_.count) readback_session_.print(output, "session");
    if (readback_local_enabled_ && hide_flares_)
      std::fprintf(output,
                   "[gs-ipc] session flare readbacks answered as zeros: %llu (no light flare and no sun glare; %llu "
                   "replies arrived late)\n",
                   static_cast<unsigned long long>(local_answers_), static_cast<unsigned long long>(local_stale_));
    else if (readback_local_enabled_)
      std::fprintf(output,
                   "[gs-ipc] session flare readbacks answered locally: %llu (previous reply), %llu without a fresh reply "
                   "(kept the older table)\n",
                   static_cast<unsigned long long>(local_answers_), static_cast<unsigned long long>(local_stale_));
    readback_probe_.print_summary(output);
    if (stream_hash_.enabled)
      std::fprintf(output, "[gs-ipc] session stream hash (GT4_GS_STREAM_HASH): records %llu hash %016llx\n",
                   static_cast<unsigned long long>(stream_hash_.records), static_cast<unsigned long long>(stream_hash_.value));
    if (has_stats_) print_consumer_stats(output, last_stats_);
    if (osd_wanted_)
      std::fprintf(output, "[gs-ipc] OSD_STATS records sent: %llu (every %.1f s); EE thread cost: record build and post "
                   "mean %.2f us, max %.2f us; per-VSYNC tick mean %.3f us\n",
                   static_cast<unsigned long long>(osd_records_), kOsdStatsIntervalUs / 1e6,
                   osd_records_ ? osd_post_ns_total_ / 1000.0 / static_cast<double>(osd_records_) : 0.0,
                   osd_post_ns_max_ / 1000.0,
                   osd_ticks_ ? osd_tick_ns_total_ / 1000.0 / static_cast<double>(osd_ticks_) : 0.0);
    std::fflush(output);
    if (dead_ && window_) {
      // The player sees a window that stopped updating: say why. The guest ran on for the grace VSYNCs after the GS
      // process died, so a changed memory card was written by the runtime (50 quiet VSYNCs) before the halt.
      offer_restart(reinterpret_cast<HWND>(static_cast<std::uintptr_t>(window_)),
                    L"The GS renderer process (gt4-gs.exe) stopped" +
                        (exit_known_ ? L" with exit code " + std::to_wstring(exit_code_) : std::wstring()) +
                        L".\nThe game will now close. Your memory card was saved.");
    }
  }

 private:
  void open_capture() {
    // TASK-041: GT4_GS_STREAM_HASH=1 hashes every record (type, flags, payload) in stream order, so two runs can be
    // compared without writing multi-GB captures (F8 interval and session lines).
    if (const char *hash = std::getenv("GT4_GS_STREAM_HASH"); hash && *hash) {
      if (std::strcmp(hash, "0") != 0 && std::strcmp(hash, "1") != 0) refuse("GT4_GS_STREAM_HASH must be 0 or 1");
      stream_hash_.enabled = std::strcmp(hash, "1") == 0;
    }
    const char *path = std::getenv("GT4_GS_CAPTURE");
    if (!path || !*path) return;
    if (const char *range = std::getenv("GT4_GS_CAPTURE_FRAMES")) {
      unsigned long long first = 0, last = 0;
      char extra = 0;
      if (std::sscanf(range, "%llu:%llu%c", &first, &last, &extra) != 2 || last < first)
        refuse(std::string("GT4_GS_CAPTURE_FRAMES=\"") + range + "\" is not first:last");
      capture_first_ = first;
      capture_last_ = last;
    }
    std::string error;
    if (!capture_.open(path, kGameSerial, capture_first_, error)) refuse("GT4_GS_CAPTURE: " + error);
    std::fprintf(stderr, "[gs-ipc] capturing the GS stream to %s (frames %llu to %llu, .gscap version 2)\n", path,
                 static_cast<unsigned long long>(capture_first_), static_cast<unsigned long long>(capture_last_));
  }

  void start_process() {
    // TASK-059: GT4_GS_PROCESS names the GS process; an installed game without it uses <game folder>\gs\gt4-gs.exe.
    const std::string process_path = gs_process_path();
    const char *process = process_path.c_str();
    if (process_path.empty())
      refuse("GT4_RENDERER=pcsx2 needs GT4_GS_PROCESS=<path of the GS process> or <game folder>\\gs\\gt4-gs.exe");
    handshake_timeout_ms_ = environment_number("GT4_GS_HANDSHAKE_TIMEOUT_MS", kHandshakeTimeoutMs, kHandshakeTimeoutMinMs,
                                               kHandshakeTimeoutMaxMs);
    lost_grace_frames_ = environment_number("GT4_GS_LOST_GRACE_FRAMES", kLostGraceFrames, 0, kLostGraceMaxFrames);
    std::uint32_t frames_in_flight = 2;
    if (const char *value = std::getenv("GT4_GS_FRAMES_IN_FLIGHT")) {
      if (std::strcmp(value, "1") && std::strcmp(value, "2") && std::strcmp(value, "3"))
        refuse(std::string("GT4_GS_FRAMES_IN_FLIGHT=\"") + value + "\" is not 1, 2 or 3");
      frames_in_flight = static_cast<std::uint32_t>(value[0] - '0');
    }
    // TASK-036: the GS process must match its pin (GT4_GS_PROCESS_SHA256, set by the launcher from the manifest).
    if (const char *pin = std::getenv("GT4_GS_PROCESS_SHA256"); pin && *pin) {
      std::string actual, error;
      if (!file_sha256(ipc::widen(process), actual, error)) refuse(std::string("cannot hash the GS process ") + process + ": " + error);
      std::string expected(pin);
      std::transform(expected.begin(), expected.end(), expected.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (actual != expected)
        refuse(std::string("the GS process ") + process + " has SHA-256 " + actual + ", the pin is " + expected);
      std::fprintf(stderr, "[gs-ipc] GS process SHA-256 matches its pin (%s)\n", actual.c_str());
    }
    std::wstring config_argument;
    if (const char *config = std::getenv("GT4_GS_CONFIG"); config && *config) {
      if (GetFileAttributesW(ipc::widen(config).c_str()) == INVALID_FILE_ATTRIBUTES)
        refuse(std::string("GT4_GS_CONFIG=") + config + " does not exist");
      config_argument = L" --config \"" + ipc::widen(config) + L"\"";
    }
    // TASK-116 step 1: Performance mode was the launcher's HWDownloadMode 2 in the very ini the GS process gets
    // (GT4_GS_CONFIG, one source of truth). TASK-117 decoupled the local answer from that marker: an explicit
    // GT4_GS_READBACK_LOCAL=1 keeps the icons, loading picture and thumbnail Accurate while the flare table is answered
    // locally (zero in Performance mode). An unreadable file or a missing key keeps the flare readback accurate.
    if (const char *config = std::getenv("GT4_GS_CONFIG"); config && *config) download_mode_ = download_mode_from_ini(config);
    readback_local_enabled_ = readback_local_switch(download_mode_);
    hide_flares_ = readback_local_enabled_ && hide_flares_switch(true);
    if (readback_local_enabled_)
      std::fprintf(stderr,
                   "[gs-ipc] flare readback answered locally (gt4-gs.ini HWDownloadMode=%d) whenever the VU1 journal can "
                   "carry it, %s; GT4_GS_READBACK_LOCAL=0 turns it off\n",
                   download_mode_,
                   hide_flares_ ? "as zeros, so no light flare and no sun glare is drawn (GT4_GS_HIDE_FLARES=0 keeps "
                                  "the previous reply)"
                                : "one frame old");
    ipc::SessionOptions options;
    options.serial = kGameSerial;
    options.frames_in_flight = frames_in_flight;
    // TASK-041: GT4_GS_WAKE_BYTES (0..16 MiB): GIF/PRIV bytes between wake-ups of a sleeping GS process; 0 = every record.
    const std::uint32_t wake_bytes = environment_number(
        "GT4_GS_WAKE_BYTES", static_cast<std::uint32_t>(ipc::Session::kDefaultDataWakeBytes), 0, 16u << 20);
    try {
      session_ = ipc::Session::create_producer(options);
      session_->set_data_wake_bytes(wake_bytes);
      const std::wstring arguments = L"--ipc " + ipc::widen(session_->name()) + L" --parent " +
                                     std::to_wstring(ipc::current_process_id()) + config_argument;
      child_ = ipc::ChildProcess::spawn(ipc::widen(process), arguments);
      session_->set_peer(child_.process());
    } catch (const ipc::IpcError &error) {
      refuse(std::string("cannot start the GS process ") + process + ": " + error.what());
    }
    ipc::RecordView event;
    bool ready = false;
    try {
      ready = session_->wait_event(event, handshake_timeout_ms_);
    } catch (const ipc::PeerExited &) {
      std::uint32_t code = 0;
      child_.wait_exit(1000, &code);
      drain_errors();
      refuse(std::string("the GS process ") + process + " exited during the handshake (code " + std::to_string(code) + ")");
    }
    if (!ready)
      refuse(std::string("the GS process ") + process + " did not answer within " +
             std::to_string(handshake_timeout_ms_ / 1000.0).substr(0, 4) + " s (no READY record; the GS device or its "
             "shader cache may be stuck, or the process is not a GS stream consumer)");
    if (event.header->type == static_cast<std::uint16_t>(RecordType::error)) {
      proto::ErrorPayload payload{};
      std::memcpy(&payload, event.payload, std::min<std::size_t>(sizeof payload, event.header->payload_bytes));
      refuse(std::string("the GS process refused the session: ") + ascii(payload.text, sizeof payload.text));
    }
    if (event.header->type != static_cast<std::uint16_t>(RecordType::ready) ||
        event.header->payload_bytes < sizeof(proto::ReadyPayload))
      refuse("the GS process sent no READY record first");
    std::memcpy(&ready_, event.payload, sizeof ready_);
    session_->events_in().release(event);
    if (ready_.version_major != proto::kVersionMajor) refuse("the GS process implements another protocol major version");
    session_->set_wait_hooks({&Client::wait_begin, &Client::wait_end});
    // TASK-062 (protocol 1.2): a consumer that draws a performance overlay asks for OSD_STATS in READY.
    osd_wanted_ = ready_.version_minor >= 2 && (ready_.flags & proto::kReadyFlagOsdStats) != 0;
    if (osd_wanted_)
      std::fprintf(stderr, "[gs-ipc] the GS process draws a performance overlay: OSD_STATS every %.1f s\n",
                   kOsdStatsIntervalUs / 1e6);
    if (const char *mode = std::getenv("GT4_GS_READBACK"); mode && *mode)
      std::fprintf(stderr, "[gs-ipc] readback mode %s (GT4_GS_READBACK, applied by the GS process configuration)\n", mode);
    std::fprintf(stderr,
                 "[gs-ipc] GT4_RENDERER=pcsx2: donor Vulkan renderer not created; GS process %s pid %u ready "
                 "(consumer %s, renderer %s, version %s, protocol %u.%u, flags 0x%x, frames in flight %u, wake every %u "
                 "bytes)\n",
                 process, ready_.consumer_pid, ascii(ready_.consumer, 32).c_str(), ascii(ready_.renderer, 32).c_str(),
                 ascii(ready_.version, 64).c_str(), ready_.version_major, ready_.version_minor, ready_.flags,
                 frames_in_flight, static_cast<unsigned>(session_->data_wake_bytes()));
  }

  // TASK-062: cumulative counters at this VSYNC (EE thread).
  OsdSample osd_sample(std::uint64_t now) const {
    OsdSample sample;
    sample.wall_us = now;
    sample.vsyncs = counters_.vsyncs;
    sample.main_cpu_us = thread_cpu_us();
    if (g_worker_busy_us) sample.worker_busy_us = g_worker_busy_us();
    gt4::perf::Probe &probe = gt4::perf::Probe::shared();
    sample.accounting = probe.enabled();
    if (sample.accounting) {
      sample.stream_write_us = probe.cumulative_exclusive_us(gt4::perf::Category::gs_ipc_write);
      sample.readback_wait_us = probe.cumulative_exclusive_us(gt4::perf::Category::gs_ipc_readback_wait);
      sample.backpressure_us = probe.cumulative_exclusive_us(gt4::perf::Category::gs_ipc_backpressure);
      sample.pacing_sleep_us = probe.cumulative_exclusive_us(gt4::perf::Category::pacing_wait);
      for (gt4::perf::Category wait : {gt4::perf::Category::mtvu_wait_vblank, gt4::perf::Category::mtvu_wait_download,
                                       gt4::perf::Category::mtvu_wait_gs_privileged, gt4::perf::Category::mtvu_wait_vu_memory,
                                       gt4::perf::Category::mtvu_wait_gif_reset, gt4::perf::Category::mtvu_wait_reset,
                                       gt4::perf::Category::mtvu_wait_state, gt4::perf::Category::mtvu_wait_other})
        sample.worker_wait_us += probe.cumulative_exclusive_us(wait);
    }
    return sample;
  }

  // TASK-062: after each VSYNC record: the frame time of this VSYNC, and every kOsdStatsIntervalUs one OSD_STATS record.
  // Cost per VSYNC: QueryPerformanceCounter calls; per record: GetThreadTimes, the probe sums and one ring write. The
  // record goes to the GS process only (not to the .gscap capture or the stream hash: it is not GS traffic).
  void osd_after_vsync() {
    if (dead_ || !session_) return;
    LARGE_INTEGER tick_start;
    QueryPerformanceCounter(&tick_start);
    const std::uint64_t now = now_us();
    if (osd_last_vsync_us_) osd_max_frame_us_ = std::max(osd_max_frame_us_, now - osd_last_vsync_us_);
    osd_last_vsync_us_ = now;
    if (!osd_begin_.wall_us) {
      osd_begin_ = osd_sample(now);
      osd_max_frame_us_ = 0;
    } else if (now - osd_begin_.wall_us >= kOsdStatsIntervalUs) {
      const OsdSample end = osd_sample(now);
      const proto::OsdStatsPayload payload = make_osd_stats(osd_begin_, end, osd_max_frame_us_);
      try {
        session_->post(RecordType::osd_stats, 0, &payload, sizeof payload);
      } catch (const ipc::IpcError &error) {
        lost(error.what());
      }
      osd_begin_ = end;
      osd_max_frame_us_ = 0;
      ++osd_records_;
      LARGE_INTEGER done;
      QueryPerformanceCounter(&done);
      const std::uint64_t ns = elapsed_ns(tick_start, done);
      osd_post_ns_total_ += ns;
      osd_post_ns_max_ = std::max(osd_post_ns_max_, ns);
    }
    LARGE_INTEGER tick_end;
    QueryPerformanceCounter(&tick_end);
    osd_tick_ns_total_ += elapsed_ns(tick_start, tick_end);
    ++osd_ticks_;
  }

  void write_capture(RecordType type, std::uint16_t flags, const void *first, std::size_t first_bytes,
                     const void *second = nullptr, std::size_t second_bytes = 0) {
    if (capture_.is_open() && frame_ >= capture_first_)
      capture_.write(type, flags, sequence_ + 1, first, first_bytes, second, second_bytes);
    if (stream_hash_.enabled) stream_hash_.add(type, flags, first, first_bytes, second, second_bytes);
  }

  void count(std::size_t payload_bytes) {
    ++counters_.records;
    counters_.bytes += proto::record_size(payload_bytes);
  }

  // One record to every active output: the capture file and the GS process ring.
  void emit(RecordType type, std::uint16_t flags, const void *first, std::size_t first_bytes,
            const void *second = nullptr, std::size_t second_bytes = 0) {
    write_capture(type, flags, first, first_bytes, second, second_bytes);
    ++sequence_;
    count(first_bytes + second_bytes);
    if (!session_ || dead_) return;
    gt4::perf::Scope write(gt4::perf::Category::gs_ipc_write);
    try {
      session_->post(type, flags, first, first_bytes, second, second_bytes);
    } catch (const ipc::IpcError &error) {
      lost(error.what());
    }
  }

  void lost(const char *what) {
    if (dead_) return;
    dead_ = true;
    std::uint32_t code = 0;
    const bool exited = child_.wait_exit(1000, &code);
    drain_errors();
    exit_known_ = exited;
    exit_code_ = code;
    if (exited)
      std::fprintf(stderr, "[gs-ipc] GS renderer stopped (exit code %u): %s\n", code, what);
    else
      std::fprintf(stderr, "[gs-ipc] GS renderer stopped: %s\n", what);
    std::fprintf(stderr, "[gs-ipc] the game halts after %u more VSYNCs (memory card flush)\n", lost_grace_frames_);
    std::fflush(stderr);
    frames_since_lost_ = 0;
    if (lost_grace_frames_ == 0 && !halted_) {
      halted_ = true;
      if (halt_) halt_("GS process exited");
    }
  }

  void drain_errors() {
    if (!session_) return;
    poll_events();
  }

  void poll_events() {
    if (!session_) return;
    ipc::RingReader &events = session_->events_in();
    ipc::RecordView event;
    while (events.peek(event)) {
      const auto type = static_cast<RecordType>(event.header->type);
      if (type == RecordType::stats && event.header->payload_bytes >= sizeof(proto::StatsPayload)) {
        std::memcpy(&last_stats_, event.payload, sizeof last_stats_);
        has_stats_ = true;
      } else if (type == RecordType::error && event.header->payload_bytes >= sizeof(proto::ErrorPayload)) {
        proto::ErrorPayload payload;
        std::memcpy(&payload, event.payload, sizeof payload);
        std::fprintf(stderr, "[gs-ipc] GS process error %u%s: %s\n", payload.code, payload.fatal ? " (fatal)" : "",
                     ascii(payload.text, sizeof payload.text).c_str());
      } else if (type == RecordType::exiting) {
        std::fprintf(stderr, "[gs-ipc] GS process is exiting\n");
      } else if (type == RecordType::window) {
        std::fprintf(stderr, "[gs-ipc] GS window event\n");
      }
      events.release(event);
    }
  }

  // The consumer's cumulative counters since READY (its latest STATS record); times are per consumer VSYNC.
  static void print_consumer_stats(std::FILE *output, const proto::StatsPayload &stats) {
    const double frames = stats.vsyncs ? static_cast<double>(stats.vsyncs) : 1.0;
    std::fprintf(output,
                 "[gs-ipc] consumer STATS (cumulative): vsyncs %llu records %llu bytes %llu readbacks %llu zero-filled %llu; "
                 "busy ms/frame=%.3f wait ms/frame=%.3f\n",
                 static_cast<unsigned long long>(stats.vsyncs), static_cast<unsigned long long>(stats.records),
                 static_cast<unsigned long long>(stats.bytes), static_cast<unsigned long long>(stats.readbacks),
                 static_cast<unsigned long long>(stats.readbacks_zero_filled), stats.busy_us / 1000.0 / frames,
                 stats.wait_us / 1000.0 / frames);
  }

  // F8 [gs-ipc] section (EE thread, at each probe report).
  void report_interval(std::FILE *file, const gt4::perf::IntervalTime &interval, std::uint64_t frames) {
    if (!frames) return;
    const double scale = static_cast<double>(frames) * 1000.0;
    const auto ms = [&](gt4::perf::Category category) {
      return interval.categories[static_cast<std::size_t>(category)].exclusive_us / scale;
    };
    const double f = static_cast<double>(frames);
    const Counters d{counters_.records - baseline_.records, counters_.bytes - baseline_.bytes,
                     counters_.gif_records - baseline_.gif_records, counters_.gif_bytes - baseline_.gif_bytes,
                     counters_.priv - baseline_.priv, counters_.vsyncs - baseline_.vsyncs,
                     counters_.readbacks - baseline_.readbacks, counters_.readback_bytes - baseline_.readback_bytes,
                     counters_.controls - baseline_.controls, counters_.resets - baseline_.resets};
    std::fprintf(file, "[gs-ipc] renderer %s%s; EE ms/frame: gs_ipc_write=%.3f gs_ipc_backpressure=%.3f "
                 "gs_ipc_vsync_post=%.3f gs_ipc_readback_wait=%.3f\n",
                 stream() ? "pcsx2" : "donor", capture_.is_open() ? " (+capture)" : "", ms(gt4::perf::Category::gs_ipc_write),
                 ms(gt4::perf::Category::gs_ipc_backpressure), ms(gt4::perf::Category::gs_ipc_vsync_post),
                 ms(gt4::perf::Category::gs_ipc_readback_wait));
    std::fprintf(file, "[gs-ipc] stream per frame: bytes=%.0f records=%.1f gif_records=%.1f gif_bytes=%.0f priv=%.2f "
                 "readbacks=%.3f readback_bytes=%.0f (vsyncs %llu)\n",
                 d.bytes / f, d.records / f, d.gif_records / f, d.gif_bytes / f, d.priv / f, d.readbacks / f,
                 d.readback_bytes / f, static_cast<unsigned long long>(d.vsyncs));
    if (readback_interval_.count) readback_interval_.print(file, "interval");
    if (readback_local_enabled_) {
      const std::uint64_t local = local_answers_ - local_answers_baseline_;
      std::fprintf(file,
                   "[gs-ipc] flare readback answered locally: %.3f per frame (%llu of %llu readbacks, %s, "
                   "no VU1 drain and no wait)%s\n",
                   static_cast<double>(local) / f, static_cast<unsigned long long>(local),
                   static_cast<unsigned long long>(d.readbacks),
                   hide_flares_ ? "as zeros: no light flare and no sun glare" : "previous reply",
                   journal_ready() ? "" : " [journal not registered: accurate path]");
      if (hide_flares_) {
        const std::uint64_t hidden = flares_hidden_ - flares_hidden_baseline_;
        std::fprintf(file, "[gs-ipc] flare table answered as zeros: %.3f per frame (%llu; GT4_GS_HIDE_FLARES=0 restores "
                     "the previous reply)\n", static_cast<double>(hidden) / f, static_cast<unsigned long long>(hidden));
      }
    }
    if (stream_hash_.enabled)
      std::fprintf(file, "[gs-ipc] stream hash (GT4_GS_STREAM_HASH) after vsync %llu: records %llu hash %016llx\n",
                   static_cast<unsigned long long>(frame_), static_cast<unsigned long long>(stream_hash_.records),
                   static_cast<unsigned long long>(stream_hash_.value));
    report_privileged(file, f);
    if (session_) {
      const ipc::WaitTotals &w = session_->wait_totals();
      const auto data = static_cast<std::size_t>(ipc::Wake::data);
      std::fprintf(file, "[gs-ipc] GS process woken from sleep per frame: %.2f (records posted while its CPU thread "
                   "slept; one event signal each)\n",
                   (w.wakes_signalled[data] - wakes_baseline_) / f);
    }
    // The GS process CPU thread over this interval, from its STATS records (posted every 500 VSYNCs).
    double gs_cpu_thread_ms = -1.0;
    if (has_stats_ && has_stats_baseline_ && last_stats_.vsyncs > stats_baseline_.vsyncs) {
      const proto::StatsPayload &a = stats_baseline_, &b = last_stats_;
      const double n = static_cast<double>(b.vsyncs - a.vsyncs);
      gs_cpu_thread_ms = (b.busy_us - a.busy_us) / 1000.0 / n;
      std::fprintf(file, "[gs-ipc] GS process CPU thread (STATS vsyncs %llu-%llu): busy ms/frame=%.3f wait ms/frame=%.3f "
                   "records/frame=%.1f readbacks served %llu\n",
                   static_cast<unsigned long long>(a.vsyncs), static_cast<unsigned long long>(b.vsyncs), gs_cpu_thread_ms,
                   (b.wait_us - a.wait_us) / 1000.0 / n, (b.records - a.records) / n,
                   static_cast<unsigned long long>(b.readbacks - a.readbacks));
    } else if (has_stats_) {
      print_consumer_stats(file, last_stats_);
    }
    if (stream()) {
      FrameCosts costs;
      costs.frame_wall_ms = interval.elapsed_us / scale;
      costs.pacing_sleep_ms = ms(gt4::perf::Category::pacing_wait);
      costs.ee_cpu_ms = gt4::perf::Probe::shared().report_cpu_ms_per_frame();
      for (gt4::perf::Category wait : {gt4::perf::Category::mtvu_wait_vblank, gt4::perf::Category::mtvu_wait_download,
                                       gt4::perf::Category::mtvu_wait_gs_privileged, gt4::perf::Category::mtvu_wait_vu_memory,
                                       gt4::perf::Category::mtvu_wait_gif_reset, gt4::perf::Category::mtvu_wait_reset,
                                       gt4::perf::Category::mtvu_wait_state, gt4::perf::Category::mtvu_wait_other})
        costs.vu1_worker_wait_ms += ms(wait);
      costs.readback_wait_ms = ms(gt4::perf::Category::gs_ipc_readback_wait);
      costs.backpressure_ms = ms(gt4::perf::Category::gs_ipc_backpressure);
      costs.gs_cpu_thread_ms = gs_cpu_thread_ms;
      std::fprintf(file, "%s\n", format_frame_summary(costs).c_str());
    }
  }

  // Per-register privileged writes of the interval: count, MTVU wait before them, writes queued behind worker output.
  void report_privileged(std::FILE *file, double f) const {
    std::string writes, waits;
    double total_writes = 0.0, total_wait_ms = 0.0, total_queued = 0.0;
    char item[64];
    for (std::size_t i = 0; i < privileged_.size(); ++i) {
      if (privileged_[i].writes == privileged_baseline_[i].writes) continue;
      const double n = (privileged_[i].writes - privileged_baseline_[i].writes) / f;
      const double wait_ms = (privileged_[i].wait_us - privileged_baseline_[i].wait_us) / 1000.0 / f;
      total_writes += n;
      total_wait_ms += wait_ms;
      total_queued += (privileged_[i].queued - privileged_baseline_[i].queued) / f;
      std::snprintf(item, sizeof item, " %s=%.2f", kPrivilegedNames[i], n);
      writes += item;
      std::snprintf(item, sizeof item, " %s=%.3f", kPrivilegedNames[i], wait_ms);
      waits += item;
    }
    const bool asynchronous = g_privileged_ordering.active != nullptr && g_privileged_ordering.active();
    std::fprintf(file, "[gs-ipc] privileged writes/frame (total %.2f):%s\n", total_writes, writes.c_str());
    std::fprintf(file, "[gs-ipc] EE wait for the VU1 worker before privileged writes ms/frame (total %.3f):%s\n",
                 total_wait_ms, waits.c_str());
    std::fprintf(file, "[gs-ipc] privileged writes queued behind VU1 worker output/frame: %.2f (GT4_MTVU_PRIV_ASYNC %s)\n",
                 total_queued, asynchronous ? "on" : "off");
  }

  static void report_interval_hook(std::FILE *file, const gt4::perf::IntervalTime &interval, std::uint64_t frames);
  static void reset_interval_hook();
  static void wait_begin(ipc::Wake which);
  static void wait_end(ipc::Wake which);

 public:
  Counters counters_;
  Counters baseline_;
  ReadbackWaits readback_interval_;  // since the last F7/F8 (or periodic) report
  PrivilegedSlots privileged_{}, privileged_baseline_{};
  proto::StatsPayload stats_baseline_{};  // the latest STATS at the last interval reset
  bool has_stats_baseline_ = false;
  std::uint64_t wakes_baseline_ = 0;
  gt4::perf::Accounting::Token wait_token_ = 0;

 private:
  Renderer renderer_;
  std::unique_ptr<ipc::Session> session_;
  ipc::ChildProcess child_;
  proto::ReadyPayload ready_{};
  std::uint64_t display_aspect_sent_ = 0;  // mode | target << 32 last sent; game_aspect is the consumer's default
  bool display_aspect_refused_logged_ = false;
  std::uint64_t display_aspect_controls_ = 0;
  proto::FileWriter capture_;
  std::uint64_t capture_first_ = 0, capture_last_ = ~std::uint64_t(0);
  std::uint64_t sequence_ = 0;
  std::uint64_t frame_ = 0;
  std::uint64_t readback_id_ = 0;
  std::uint64_t zero_filled_ = 0;
  // TASK-116 step 1: the local flare answer (Performance mode).
  int download_mode_ = -1;             // gt4-gs.ini HWDownloadMode (-1: unknown, no local answer)
  bool readback_local_enabled_ = false;
  bool hide_flares_ = false;           // TASK-117: answer the table as zeros (no flares, no sun glare)
  std::array<std::uint8_t, kFlareReadbackBytes> flare_cache_{};
  bool flare_cache_valid_ = false;
  std::uint64_t flare_pending_id_ = 0;  // request id of the last queued READ_FIFO (0: none)
  std::uint64_t local_answers_ = 0;     // flare readbacks answered from the cache
  std::uint64_t local_answers_baseline_ = 0;
  std::uint64_t flares_hidden_ = 0;     // TASK-117: flare tables answered as zeros
  std::uint64_t flares_hidden_baseline_ = 0;
  std::uint64_t local_stale_ = 0;       // replies that had not arrived (or were replaced) when collected
  proto::StatsPayload last_stats_{};
  bool has_stats_ = false;
  bool dead_ = false;
  bool finished_ = false;
  std::atomic<std::uint64_t> pending_resize_{0};
  void (*halt_)(const char *) = nullptr;
  std::uint64_t window_ = 0;  // game window handle after attach_window (0: headless)
  bool exit_known_ = false;
  std::uint32_t exit_code_ = 0;
  ReadbackWaits readback_session_;
  ReadbackProbe readback_probe_{ReadbackProbe::enabled_from_environment()};  // TASK-116 step 0 (P0 gate)
  StreamHash stream_hash_;  std::uint32_t handshake_timeout_ms_ = kHandshakeTimeoutMs;
  std::uint32_t lost_grace_frames_ = kLostGraceFrames;
  std::uint32_t frames_since_lost_ = 0;
  bool halted_ = false;
  // TASK-062: OSD_STATS state (EE thread).
  bool osd_wanted_ = false;
  OsdSample osd_begin_{};
  std::uint64_t osd_last_vsync_us_ = 0, osd_max_frame_us_ = 0;
  std::uint64_t osd_records_ = 0, osd_ticks_ = 0, osd_tick_ns_total_ = 0, osd_post_ns_total_ = 0, osd_post_ns_max_ = 0;
};
#else
// TASK-180: no GS process and no capture files on this platform. client() never creates one (parse_renderer refuses
// pcsx2, start() refuses a capture); the members only keep the hooks below compiling, and do nothing.
class Client {
 public:
  explicit Client(Renderer) {}
  void start() { refuse("GT4_GS_CAPTURE (GS stream capture files) is available on Windows only"); }
  bool stream() const { return false; }
  void gif(int, const std::uint8_t *, std::uint32_t) {}
  void privileged(std::uint32_t, std::uint64_t) {}
  void post_privileged(std::uint32_t, std::uint64_t) {}
  bool local_active() const { return false; }
  void post_queued_readback(std::uint32_t) {}
  bool read_fifo(std::uint8_t *, std::uint32_t) { return false; }
  void vsync(bool) {}
  void reset(proto::ResetKind, std::uint32_t) {}
  void attach(std::uint64_t, std::uint32_t, std::uint32_t) {}
  void request_resize(std::uint32_t, std::uint32_t) {}
  void set_halt(void (*)(const char *)) {}
  void finish(std::FILE *) {}
};
#endif

Renderer parse_renderer() {
  const char *value = std::getenv("GT4_RENDERER");
  if (!value || std::strcmp(value, "donor") == 0) return Renderer::donor;
#ifdef GT4_GS_STREAM_PROCESS
  if (std::strcmp(value, "pcsx2") == 0) return Renderer::pcsx2;
#else
  if (std::strcmp(value, "pcsx2") == 0) refuse("GT4_RENDERER=pcsx2 (GSdx, gt4-gs.exe) is available on Windows only");
#endif
#ifdef GT4_GS_STREAM_HW
  if (std::strcmp(value, "hw") == 0) return Renderer::hw;
#endif
#ifdef GT4_GS_STREAM_ARMSX2
  if (std::strcmp(value, "armsx2") == 0) return Renderer::armsx2;
#endif
  refuse(std::string("GT4_RENDERER=\"") + value + "\" is not a renderer of this build");
}

// Created on first use; null when the donor renderer runs without a capture (every hook then returns at once).
// Intentionally never destroyed: the GS process ends through finish() or the job object at process exit.
Client *client() {
  static Client *const instance = []() -> Client * {
    const Renderer mode = parse_renderer();
    if (mode == Renderer::hw || mode == Renderer::armsx2) return nullptr;  // the in-process sink takes every hook
    const char *capture = std::getenv("GT4_GS_CAPTURE");
    if (mode == Renderer::donor && !(capture && *capture)) return nullptr;
    auto *created = new Client(mode);
    created->start();
    return created;
  }();
  return instance;
}

#ifdef GT4_GS_STREAM_PROCESS
void Client::report_interval_hook(std::FILE *file, const gt4::perf::IntervalTime &interval, std::uint64_t frames) {
  if (Client *c = client()) c->report_interval(file, interval, frames);
}

void Client::reset_interval_hook() {
  if (Client *c = client()) {
    c->baseline_ = c->counters_;
    c->readback_interval_ = {};
    c->local_answers_baseline_ = c->local_answers_;
    c->flares_hidden_baseline_ = c->flares_hidden_;
    c->privileged_baseline_ = c->privileged_;
    c->stats_baseline_ = c->last_stats_;
    c->has_stats_baseline_ = c->has_stats_;
    c->wakes_baseline_ =
        c->session_ ? c->session_->wait_totals().wakes_signalled[static_cast<std::size_t>(ipc::Wake::data)] : 0;
  }
}

void Client::wait_begin(ipc::Wake which) {
  Client *c = client();
  if (c && which == ipc::Wake::space && !c->wait_token_)
    c->wait_token_ = gt4::perf::Probe::shared().enter(gt4::perf::Category::gs_ipc_backpressure);
}

void Client::wait_end(ipc::Wake which) {
  Client *c = client();
  if (c && which == ipc::Wake::space && c->wait_token_) {
    gt4::perf::Probe::shared().leave(c->wait_token_);
    c->wait_token_ = 0;
  }
}
#endif

// SMODE1 fields (GS privileged register 0x12000010): LC bits 3-9, CMOD bits 13-14 (0 progressive, 2 NTSC, 3 PAL).
// SMODE2 (0x12000020): INT bit 0, FFMD bit 1. Values written for each SetGsCrt request, as in the TASK-012 seam.
constexpr std::uint64_t kCmodProgressive = 0, kCmodNtsc = 2, kCmodPal = 3;
constexpr std::uint64_t kLcAnalog = 32, kLcHdtv = 22, kLcVga = 15;

}  // namespace

#ifdef GT4_GS_STREAM_PROCESS
bool file_sha256(const std::wstring &path, std::string &hex, std::string &error) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    error = "CreateFileW failed (" + std::to_string(GetLastError()) + ")";
    return false;
  }
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
            BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
  std::vector<unsigned char> buffer(1 << 20);
  while (ok) {
    DWORD read = 0;
    if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
      ok = false;
      break;
    }
    if (read == 0) break;
    ok = BCryptHashData(hash, buffer.data(), read, 0) >= 0;
  }
  unsigned char digest[32] = {};
  if (ok) ok = BCryptFinishHash(hash, digest, sizeof digest, 0) >= 0;
  if (hash) BCryptDestroyHash(hash);
  if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
  CloseHandle(file);
  if (!ok) {
    error = "SHA-256 computation failed";
    return false;
  }
  static const char digits[] = "0123456789abcdef";
  hex.clear();
  for (unsigned char byte : digest) {
    hex.push_back(digits[byte >> 4]);
    hex.push_back(digits[byte & 15]);
  }
  return true;
}
#endif

bool map_set_gs_crt(std::uint32_t interlace, std::uint32_t mode, std::uint32_t field, std::uint64_t &smode1,
                    std::uint64_t &smode2) {
  if (field > 1 || interlace > 1) return false;
  std::uint64_t cmod = 0, lc = 0;
  if (mode >= 0x1A && mode <= 0x3B) {  // VESA modes: progressive only
    if (interlace) return false;
    cmod = kCmodProgressive;
    lc = kLcVga;
  } else if (mode == 0x02 || mode == 0x03) {  // NTSC, PAL
    cmod = mode == 0x02 ? kCmodNtsc : kCmodPal;
    lc = kLcAnalog;
  } else if (mode == 0x50) {  // DTV 480p
    if (interlace) return false;
    cmod = kCmodProgressive;
    lc = kLcAnalog;
  } else if (mode == 0x51) {  // DTV 1080i
    if (!interlace) return false;
    cmod = kCmodProgressive;
    lc = kLcHdtv;
  } else if (mode == 0x52) {  // DTV 720p
    if (interlace) return false;
    cmod = kCmodProgressive;
    lc = kLcHdtv;
  } else {
    return false;
  }
  smode1 = (cmod << 13) | (lc << 3);
  smode2 = interlace ? (1u | (field ? 2u : 0u)) : 0u;
  return true;
}

bool map_set_gs_crt_syncv(std::uint32_t mode, std::uint64_t &syncv) {
  // SYNCV fields: VFP bits 0-9, VFPE 10-19, VBP 20-31, VBPE 32-41, VDP 42-52, VS 53-63.
  const auto pack = [](std::uint64_t vs, std::uint64_t vdp, std::uint64_t vbpe, std::uint64_t vbp, std::uint64_t vfpe,
                       std::uint64_t vfp) {
    return (vs << 53) | (vdp << 42) | (vbpe << 32) | (vbp << 20) | (vfpe << 10) | vfp;
  };
  if (mode == 0x02) {
    syncv = pack(6, 480, 6, 26, 6, 1);  // 0x00C7800601A01801
    return true;
  }
  if (mode == 0x03) {
    syncv = pack(5, 576, 5, 33, 5, 1);  // 0x00A9000502101401
    return true;
  }
  return false;
}

Renderer renderer() {
  static const Renderer mode = parse_renderer();
  return mode;
}

bool stream_renderer() { return renderer() != Renderer::donor; }

namespace {

// In-process renderers: GT4_RENDERER=hw (TASK-027, CoreGS 2.0 development builds) and GT4_RENDERER=armsx2 (the
// private Android test build with libgt4gs.so). Started on first use (EE thread); a failed start refuses the game like a
// failed GS process.
const InProcessSink *hw_sink() {
  static const InProcessSink *const sink = []() -> const InProcessSink * {
    const Renderer mode = renderer();
    if (mode != Renderer::hw && mode != Renderer::armsx2) return nullptr;
    const char *capture = std::getenv("GT4_GS_CAPTURE");
    if (capture && *capture) refuse("GT4_GS_CAPTURE is not supported with an in-process GS yet");
#ifdef GT4_GS_STREAM_ARMSX2
    if (mode == Renderer::armsx2) {
      const InProcessSink &chosen = armsx2_in_process_sink();
      std::string error;
      if (!chosen.start(error)) refuse("GT4_RENDERER=armsx2: libgt4gs could not start: " + error);
      return &chosen;
    }
#endif
#ifdef GT4_GS_STREAM_HW
    if (mode == Renderer::hw) {
      const InProcessSink &chosen = hw_in_process_sink();
      std::string error;
      if (!chosen.start(error)) refuse("GT4_RENDERER=hw: the HWR backend could not start: " + error);
      return &chosen;
    }
#endif
    refuse("an in-process GS renderer was selected in a build without one");
  }();
  return sink;
}

// The hw form of Client::privileged: same address filter and the same MTVU ordering (TASK-041), no stream record.
void hw_privileged(const InProcessSink &sink, std::uint32_t address, std::uint64_t value) {
  if (address < proto::kPrivilegedBase || address >= proto::kPrivilegedBase + proto::kPrivilegedBytes) return;
  if (g_privileged_ordering.take_wait_us) g_privileged_ordering.take_wait_us();
  if (g_privileged_ordering.queue && g_privileged_ordering.queue(address, value)) return;
  sink.privileged(address, value);
}

}  // namespace

bool mtvu_direct_available() {
#ifdef GT4_GS_STREAM_HW
  // The GS worker thread reads the ring; GT4_HW_GS_THREAD=0 runs the stream on the EE thread, which cannot.
  const char *thread = std::getenv("GT4_HW_GS_THREAD");
  const bool gsThread = !(thread && std::strcmp(thread, "0") == 0);
  return renderer() == Renderer::hw && hw_in_process_sink().mtvu_job != nullptr && gsThread;
#else
  return false;
#endif
}

void on_mtvu_job(std::uint64_t job, gt4::mtvu::SpscByteRing &output, gt4::mtvu::Doorbell &bell) {
  const InProcessSink *sink = hw_sink();
  if (!sink || !sink->mtvu_job) refuse("IN-461 direct GS intake without the hw GS worker");
  sink->mtvu_job(job, output, bell);
}

bool on_gif_render(int path, const std::uint8_t *data, std::uint32_t qwc) {
  if (const InProcessSink *sink = hw_sink()) {
    if (qwc) sink->gif(path, data, qwc);
    return true;
  }
  Client *c = client();
  if (!c) return false;
  if (qwc) c->gif(path, data, qwc);
  return c->stream();
}

void on_privileged_write(std::uint32_t address, std::uint64_t value) {
  if (const InProcessSink *sink = hw_sink()) return hw_privileged(*sink, address, value);
  if (Client *c = client()) c->privileged(address, value);
}

void set_privileged_ordering(const PrivilegedOrdering &ordering) { g_privileged_ordering = ordering; }

void post_privileged_now(std::uint32_t address, std::uint64_t value) {
  if (const InProcessSink *sink = hw_sink()) return sink->privileged(address, value);
  if (Client *c = client()) c->post_privileged(address, value);
}

int privileged_register_index(std::uint32_t address) {
  if (address < ::gs_stream::kPrivilegedBase || address >= ::gs_stream::kPrivilegedBase + ::gs_stream::kPrivilegedBytes)
    return kPrivilegedRegisterSlots - 1;
  const std::uint32_t offset = (address - ::gs_stream::kPrivilegedBase) & ~0xFu;
  if (offset <= 0xE0u) return static_cast<int>(offset >> 4);
  switch (offset) {
    case 0x1000u: return 15;  // CSR
    case 0x1010u: return 16;  // IMR
    case 0x1040u: return 17;  // BUSDIR
    case 0x1080u: return 18;  // SIGLBLID
    default: return kPrivilegedRegisterSlots - 1;
  }
}

const char *privileged_register_name(int index) {
  return index >= 0 && index < kPrivilegedRegisterSlots ? kPrivilegedNames[index] : "invalid";
}

std::string format_frame_summary(const FrameCosts &costs) {
  char line[640];
  const double waits = costs.vu1_worker_wait_ms + costs.readback_wait_ms + costs.backpressure_ms;
  char gs[96] = "GS process CPU thread Unknown";
  if (costs.gs_cpu_thread_ms >= 0.0)
    std::snprintf(gs, sizeof gs, "GS process CPU thread %.2f ms/frame", costs.gs_cpu_thread_ms);
  // TASK-084 step 0: the critical path is the EE frame's wall time minus the pacer's sleep, the study's formula
  // (docs/FRAME-COST-STUDY.md section 6 item 2). The old "EE CPU + waits" double-counted the CPU spent inside waits.
  if (costs.frame_wall_ms < 0.0) {
    std::snprintf(line, sizeof line,
                  "[gs-ipc] summary: EE critical path Unknown (frame wall not measured); EE waits %.2f ms/frame (VU1 "
                  "worker %.2f, readbacks %.2f, GS back-pressure %.2f) of the %.2f ms frame budget; %s",
                  waits, costs.vu1_worker_wait_ms, costs.readback_wait_ms, costs.backpressure_ms, costs.budget_ms, gs);
    return line;
  }
  const double path = costs.frame_wall_ms - costs.pacing_sleep_ms;
  std::snprintf(line, sizeof line,
                "[gs-ipc] summary: EE critical path %.2f ms/frame of %.2f budget (%.0f%%) = frame wall %.2f minus "
                "pacing sleep %.2f; inside it: EE CPU %.2f, of which VU1 worker wait %.2f, readbacks %.2f, GS "
                "back-pressure %.2f (waits are part of the path, not added); headroom %.2f ms; %s",
                path, costs.budget_ms, path / costs.budget_ms * 100.0, costs.frame_wall_ms, costs.pacing_sleep_ms,
                costs.ee_cpu_ms, costs.vu1_worker_wait_ms, costs.readback_wait_ms, costs.backpressure_ms,
                costs.budget_ms - path, gs);
  return line;
}

bool read_fifo(std::uint8_t *data, std::uint32_t qwords) {
  if (const InProcessSink *sink = hw_sink()) {
    sink->read_fifo(data, qwords);
    return true;
  }
  Client *c = client();
  return c ? c->read_fifo(data, qwords) : false;
}

void set_guest_ram(std::uint8_t *base, std::uint32_t size) {
  g_guest_ram = base;
  g_guest_ram_size = size;
}

void set_readback_journal(const ReadbackJournal &journal) { g_readback_journal = journal; }

bool readback_local_active() {
  Client *c = client();
  return c != nullptr && c->local_active();
}

void post_read_fifo_nowait(std::uint32_t qwords) {
  if (Client *c = client()) c->post_queued_readback(qwords);
}

void on_vsync(bool odd_field) {
  // TASK-084 step 3 holds the VSYNC until the frame's VU1 output is in stream order; this applies to the in-process
  // hw sink as well (TASK-027), whose VSYNC flushes and presents the frame.
  if (g_defer_vsync) {
    g_pending_vsync = odd_field;
    g_has_pending_vsync = true;
    return;
  }
  if (const InProcessSink *sink = hw_sink()) return sink->vsync(odd_field);
  if (Client *c = client()) c->vsync(odd_field);
}

void set_vsync_deferral(bool defer) { g_defer_vsync = defer; }

bool post_pending_vsync() {
  if (!g_has_pending_vsync) return false;
  g_has_pending_vsync = false;
  const bool field = g_pending_vsync;
  if (const InProcessSink *sink = hw_sink()) {
    sink->vsync(field);
    return true;
  }
  if (Client *c = client()) c->vsync(field);
  return true;
}

void on_gs_reset() {
  if (const InProcessSink *sink = hw_sink()) return sink->gs_reset();
  if (Client *c = client()) c->reset(::gs_stream::ResetKind::gs_hard, 0);
}

void on_gif_reset() {
  if (const InProcessSink *sink = hw_sink()) return sink->gif_reset();
  if (Client *c = client()) c->reset(::gs_stream::ResetKind::gif_paths, 0xEu);
}

void on_set_gs_crt(std::uint32_t interlace, std::uint32_t mode, std::uint32_t field) {
  const InProcessSink *sink = hw_sink();
  Client *c = client();
  if (!c && !sink) return;
  std::uint64_t smode1 = 0, smode2 = 0;
  if (!map_set_gs_crt(interlace, mode, field, smode1, smode2)) {
    std::fprintf(stderr, "[gs-ipc] SetGsCrt interlace %u mode 0x%X field %u not mapped; no SMODE writes posted\n",
                 interlace, mode, field);
    return;
  }
  const auto post = [&](std::uint32_t address, std::uint64_t value) {
    if (sink) hw_privileged(*sink, address, value);
    else c->privileged(address, value);
  };
  post(0x12000010u, smode1);
  std::uint64_t syncv = 0;
  if (map_set_gs_crt_syncv(mode, syncv)) post(0x12000060u, syncv);
  post(0x12000020u, smode2);
}

bool attach_window(std::uint64_t window, std::uint32_t width, std::uint32_t height) {
  if (const InProcessSink *sink = hw_sink()) return sink->attach_window(window, width, height);
  Client *c = client();
  if (!c || !c->stream()) return false;
  c->attach(window, width, height);
  return true;
}

#ifdef __ANDROID__
void detach_window() {
  if (const InProcessSink *sink = hw_sink(); sink != nullptr && sink->detach_window != nullptr) sink->detach_window();
}
#endif

void resize_window(std::uint32_t width, std::uint32_t height) {
  if (const InProcessSink *sink = hw_sink()) return sink->resize_window(width, height);
  Client *c = client();
  if (c && c->stream()) c->request_resize(width, height);
}

void set_cursor_policy(bool auto_hide, std::uint32_t hide_delay_ms) {
  g_cursor_auto_hide = auto_hide;
  g_cursor_hide_delay_ms = std::clamp<std::uint32_t>(hide_delay_ms, 1, 60000);
}

void set_display_aspect_mode(DisplayAspectMode mode, std::uint32_t fill_aspect_millionths) {
  const std::uint32_t target = mode == DisplayAspectMode::fill ? fill_aspect_millionths : 0;
  g_display_aspect_wanted.store(static_cast<std::uint64_t>(mode) | (static_cast<std::uint64_t>(target) << 32));
  // TASK-027: GT4_RENDERER=hw has no GS process to post CONTROL display_aspect to; the in-process presenter takes the
  // same request directly (owner test 6: without it the widened ultrawide race stayed in the 16:9 pillarbox).
  if (const InProcessSink *sink = hw_sink(); sink && sink->display_aspect)
    sink->display_aspect(mode == DisplayAspectMode::fill, target);
}

std::uint32_t aspect_millionths(double ratio) {
  if (!(ratio >= kMinFillAspect && ratio <= kMaxFillAspect)) return 0;
  return static_cast<std::uint32_t>(std::floor(ratio * 1e6 + 0.5));
}

void set_worker_busy_source(std::uint64_t (*worker_busy_us)()) { g_worker_busy_us = worker_busy_us; }

proto::OsdStatsPayload make_osd_stats(const OsdSample &begin, const OsdSample &end, std::uint64_t max_frame_us,
                                      double budget_ms) {
  proto::OsdStatsPayload payload{};
  const std::uint64_t frames = end.vsyncs > begin.vsyncs ? end.vsyncs - begin.vsyncs : 0;
  const std::uint64_t interval = end.wall_us > begin.wall_us ? end.wall_us - begin.wall_us : 0;
  if (!frames || !interval) return payload;  // valid = 0: nothing to show yet
  const auto clamp32 = [](std::uint64_t value) {
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(value, UINT32_MAX));
  };
  const auto per_frame = [frames, clamp32](std::uint64_t first, std::uint64_t last) {
    return clamp32(last > first ? (last - first) / frames : 0);
  };
  payload.frames = clamp32(frames);
  payload.interval_us = interval;
  payload.frame_time_avg_us = clamp32(interval / frames);
  payload.frame_time_max_us = clamp32(max_frame_us);
  payload.valid = proto::kOsdStatFrameTimes;
  const bool cpu_known =
      begin.main_cpu_us != UINT64_MAX && end.main_cpu_us != UINT64_MAX && end.main_cpu_us >= begin.main_cpu_us;
  if (cpu_known) {
    payload.main_thread_cpu_us = per_frame(begin.main_cpu_us, end.main_cpu_us);
    payload.valid |= proto::kOsdStatMainThreadCpu;
  }
  // The worker publishes its busy time only while the accounting is on (GT4_PERF=1 in every launcher run).
  if (begin.worker_busy_us != UINT64_MAX && end.worker_busy_us != UINT64_MAX && begin.accounting && end.accounting) {
    payload.worker_busy_us = per_frame(begin.worker_busy_us, end.worker_busy_us);
    payload.valid |= proto::kOsdStatWorkerBusy;
  }
  if (begin.accounting && end.accounting) {
    payload.stream_write_us = per_frame(begin.stream_write_us, end.stream_write_us);
    payload.readback_wait_us = per_frame(begin.readback_wait_us, end.readback_wait_us);
    payload.backpressure_us = per_frame(begin.backpressure_us, end.backpressure_us);
    payload.worker_wait_us = per_frame(begin.worker_wait_us, end.worker_wait_us);
    payload.valid |= proto::kOsdStatStreamCosts | proto::kOsdStatWorkerWait;
    if (cpu_known) {
      // TASK-084 step 0: the same formula as format_frame_summary: the frame wall minus the pacer's sleep. The waits
      // below are already inside the wall time, so they are reported, not added.
      const std::uint32_t pacing = per_frame(begin.pacing_sleep_us, end.pacing_sleep_us);
      payload.critical_path_us = payload.frame_time_avg_us > pacing ? payload.frame_time_avg_us - pacing : 0;
      payload.frame_budget_us = static_cast<std::uint32_t>(budget_ms * 1000.0 + 0.5);
      payload.valid |= proto::kOsdStatCriticalPath;
    }
  }
  return payload;
}

void set_halt_handler(void (*halt)(const char *reason)) {
  if (Client *c = client()) c->set_halt(halt);
}

void finish(std::FILE *output) {
  if (const InProcessSink *sink = hw_sink()) return sink->finish(output);
  if (Client *c = client()) c->finish(output);
}

}  // namespace gt4::gs_stream

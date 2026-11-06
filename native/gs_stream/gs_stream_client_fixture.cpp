// SPDX-License-Identifier: MIT
// CTest native_gs_stream_client: the game-side GS stream backend (gs_stream_client.hpp) end to end, without the game.
//
//   gs_stream_client_fixture <path of gs_stream_record.exe> <scratch directory>
//
// Each scenario runs in a fresh child process of this executable (the client reads its environment once):
//   donor          GT4_RENDERER unset: every hook is a no-op and no file appears.
//   donor-capture  GT4_RENDERER=donor, GT4_GS_CAPTURE: the scripted hook sequence is written in process.
//   pcsx2          GT4_RENDERER=pcsx2, GT4_GS_PROCESS=gs_stream_record (recording) and GT4_GS_CAPTURE: the same
//                  sequence travels through shared memory to the recorder process.
//   bad-renderer   GT4_RENDERER=bogus is refused (exit code 1).
//   no-process     GT4_RENDERER=pcsx2 without GT4_GS_PROCESS is refused.
//   early-exit     the GS process exits during the handshake: refused with a message.
//   peer-death     the GS process dies mid-stream: the guest keeps running GT4_GS_LOST_GRACE_FRAMES (3 here) VSYNCs
//                  so the runtime can flush a changed memory card, then the halt handler runs once (TASK-038).
//   handshake-timeout  the GS process never posts READY: refused after GT4_GS_HANDSHAKE_TIMEOUT_MS (1.5 s here) with a
//                  clear message; the silent process is ended by the kill-on-close job when the game exits (TASK-038).
//   game-killed    the game process is terminated while its GS process ignores the game: the job ends the GS process
//                  (TASK-038).
//   pin-mismatch   GT4_GS_PROCESS_SHA256 differs from the GS process file: refused before the spawn (TASK-036).
//   bad-config     GT4_GS_CONFIG names a missing file: refused (TASK-036).
//   pcsx2-ordered  TASK-041: as pcsx2, but the PATH1/PATH2 data of each frame is held by a stand-in for the MTVU
//                  journal and the privileged writes are queued behind it (PrivilegedOrdering hooks); the drain posts
//                  them in order, so the stream must equal the pcsx2 capture byte for byte.
// The pcsx2 scenario also sets the correct GT4_GS_PROCESS_SHA256 and a GT4_GS_CONFIG file (passed as --config).
// Checks: the three stream files (donor capture, pcsx2 in-process capture, recorder output) are byte-identical; the
// file parses back with the expected record counts, MORE split and VSYNC fields; the SetGsCrt table.
#include "gs_stream_client.hpp"
#include "gs_stream_file.hpp"
#include "gs_stream_ipc.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace gs_stream;

void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

int g_halts = 0;
std::uint32_t g_frame = 0;  // frame of run_script at the time of the hook call
std::uint32_t g_halt_frame = 0;
void count_halt(const char *reason) {
  ++g_halts;
  g_halt_frame = g_frame;
  std::printf("[fixture] halt at frame %u: %s\n", g_frame, reason);
}

std::vector<std::uint8_t> qwords_of(std::size_t qwords, std::uint8_t seed) {
  std::vector<std::uint8_t> data(qwords * 16);
  for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(seed + i * 13);
  return data;
}

// TASK-041: a stand-in for the MTVU journal. Worker GIF output and privileged writes queued behind it, in order.
struct HeldEntry {
  bool privileged = false;
  int path = 0;
  std::vector<std::uint8_t> data;
  std::uint32_t address = 0;
  std::uint64_t value = 0;
};
std::vector<HeldEntry> g_held;
bool g_ordered = false;

bool queue_behind_held(std::uint32_t address, std::uint64_t value) {
  if (g_held.empty()) return false;
  g_held.push_back(HeldEntry{true, 0, {}, address, value});
  return true;
}
std::uint64_t no_wait_us() { return 0; }
bool ordered_active() { return true; }

// The "worker" output of the ordered scenario goes to the held list; otherwise straight to the client.
void render_worker_output(int path, const std::vector<std::uint8_t> &data) {
  if (g_ordered) {
    g_held.push_back(HeldEntry{false, path, data, 0, 0});
    return;
  }
  gt4::gs_stream::on_gif_render(path, data.data(), static_cast<std::uint32_t>(data.size() / 16));
}

// The drain: every held entry in order (GIF to the render hook, privileged writes posted now).
void drain_held() {
  for (const HeldEntry &entry : g_held) {
    if (entry.privileged)
      gt4::gs_stream::post_privileged_now(entry.address, entry.value);
    else
      gt4::gs_stream::on_gif_render(entry.path, entry.data.data(), static_cast<std::uint32_t>(entry.data.size() / 16));
  }
  g_held.clear();
}

// The scripted hook sequence of a short session (deterministic).
void run_script(std::uint32_t frames) {
  namespace client = gt4::gs_stream;
  if (g_ordered) client::set_privileged_ordering({&queue_behind_held, &no_wait_us, &ordered_active});
  client::set_halt_handler(count_halt);
  client::on_gs_reset();
  client::on_set_gs_crt(1, 0x02, 1);
  std::vector<std::uint8_t> reply(512 * 16);
  for (std::uint32_t frame = 0; frame < frames; ++frame) {
    g_frame = frame;
    const auto path3 = qwords_of(3, static_cast<std::uint8_t>(frame));
    const auto path1 = qwords_of(64, static_cast<std::uint8_t>(frame + 1));
    const auto path2 = qwords_of(5, static_cast<std::uint8_t>(frame + 2));
    client::on_gif_render(3, path3.data(), 3);
    render_worker_output(1, path1);
    render_worker_output(2, path2);
    client::on_privileged_write(0x12000070u, frame);
    client::on_privileged_write(0x12001000u, 8);
    drain_held();
    if (frame == 4) {
      // Larger than one record: split with kRecordFlagMore (16 MiB of the 32 MiB ring per record).
      const std::size_t qwords = (16u << 20) / 16 + 3;
      const auto big = qwords_of(qwords, 9);
      client::on_gif_render(3, big.data(), static_cast<std::uint32_t>(qwords));
    }
    if (frame == 6) {
      std::memset(reply.data(), 0xAB, reply.size());
      const bool served = client::read_fifo(reply.data(), 512);
      if (served)
        for (std::uint8_t byte : reply) require(byte == 0, "recorder readback is zero-filled");
      else
        for (std::uint8_t byte : reply) require(byte == 0xAB, "donor readback is left to the donor");
    }
    client::on_vsync((frame & 1) != 0);
  }
  client::on_gif_reset();
  client::finish(stdout);
}

// TASK-116 step 1: the flare visibility table (64x8 PSMCT32 = 128 qwords), the only readback answered locally.
constexpr std::uint32_t kFlareQwords = 128;

// TASK-116 step 1: the journal services of the client, faked so the local flare answer can be exercised without MTVU.
// queue_readback counts the queued READ_FIFO; drain_download models the journal drain (it posts the queued READ_FIFO, as
// the bridge does through post_read_fifo_nowait).
std::uint64_t g_queued = 0, g_drained = 0, g_queue_depth = 0;
std::uint32_t g_queued_qwords = 0;

bool fake_queue_readback(std::uint32_t qwords) {
  ++g_queued;
  ++g_queue_depth;
  g_queued_qwords = qwords;
  return true;
}

void fake_drain_download() {
  ++g_drained;
  while (g_queue_depth > 0) {
    --g_queue_depth;
    gt4::gs_stream::post_read_fifo_nowait(g_queued_qwords);
  }
}

// The reply the pattern consumer writes: a function of the request id and the byte index (child mode "pattern").
std::uint8_t pattern_byte(std::uint64_t id, std::uint64_t i) {
  return static_cast<std::uint8_t>((id * 131u + i * 7u + (i >> 8)) & 0xFFu);
}

// TASK-116 step 1: the local flare readback (Performance mode). The first readback waits as today; the next one is the
// previous reply, byte for byte, queued behind the journal with no drain; a 512-qword readback is never local and its
// request lands after the queued one (same id sequence, same records).
void run_local_script() {
  namespace client = gt4::gs_stream;
  client::set_readback_journal({&fake_queue_readback, &fake_drain_download});
  client::set_halt_handler(count_halt);
  client::on_gs_reset();
  g_queued = g_drained = g_queue_depth = 0;
  std::vector<std::uint8_t> flare(kFlareQwords * 16, 0xEE);
  std::vector<std::uint8_t> icon(512 * 16, 0xEE);

  // 1. First flare readback of the scene: the accurate path (drain, request 1, wait), cached.
  require(client::read_fifo(flare.data(), kFlareQwords), "the first flare readback is served");
  require(g_drained == 1 && g_queued == 0, "the first flare readback drains and waits");
  const std::vector<std::uint8_t> first = flare;
  for (std::uint64_t i = 0; i < first.size(); ++i) require(first[i] == pattern_byte(1, i), "first reply is request 1");

  // 2. Second flare readback: answered from the cache (request 1 again, byte for byte), queued, no drain.
  std::fill(flare.begin(), flare.end(), 0xEE);
  require(client::read_fifo(flare.data(), kFlareQwords), "the second flare readback is served");
  require(g_drained == 1, "the second flare readback does not drain");
  require(g_queued == 1 && g_queued_qwords == kFlareQwords, "the second flare readback queues one 128-qword request");
  require(flare == first, "the second reply is the previous one, byte for byte");

  // 3. A 512-qword readback is never local: drain (which posts the queued request 2), request 3, wait.
  require(client::read_fifo(icon.data(), 512), "the icon readback is served");
  require(g_drained == 2, "the icon readback drains");
  for (std::uint64_t i = 0; i < icon.size(); ++i) require(icon[i] == pattern_byte(3, i), "icon reply is request 3");

  client::finish(stdout);
}

// TASK-116 step 1 (A/B): with GT4_GS_READBACK_LOCAL=0 every readback drains and waits, so each reply is fresh.
void run_local_off_script() {
  namespace client = gt4::gs_stream;
  client::set_readback_journal({&fake_queue_readback, &fake_drain_download});
  client::set_halt_handler(count_halt);
  client::on_gs_reset();
  g_queued = g_drained = g_queue_depth = 0;
  std::vector<std::uint8_t> flare(kFlareQwords * 16, 0xEE);
  for (std::uint64_t n = 1; n <= 2; ++n) {
    require(client::read_fifo(flare.data(), kFlareQwords), "the flare readback is served");
    for (std::uint64_t i = 0; i < flare.size(); ++i)
      require(flare[i] == pattern_byte(n, i), "the kill switch keeps every reply fresh");
  }
  require(g_queued == 0 && g_drained == 2, "the kill switch drains and waits for every readback");
  client::finish(stdout);
}

// TASK-117: with GT4_GS_HIDE_FLARES=1 (the Performance mode default) the flare table is served as zeros from the first
// readback, while the READ_FIFO is still queued once per readback and the journal keeps the download order.
void run_hidden_script() {
  namespace client = gt4::gs_stream;
  client::set_readback_journal({&fake_queue_readback, &fake_drain_download});
  client::set_halt_handler(count_halt);
  client::on_gs_reset();
  g_queued = g_drained = g_queue_depth = 0;
  std::vector<std::uint8_t> flare(kFlareQwords * 16, 0xEE);
  for (std::uint64_t n = 1; n <= 2; ++n) {
    std::fill(flare.begin(), flare.end(), 0xEE);
    require(client::read_fifo(flare.data(), kFlareQwords), "the hidden flare readback is served");
    for (std::uint8_t byte : flare) require(byte == 0, "the hidden flare table is zeros");
    require(g_queued == n && g_queued_qwords == kFlareQwords, "every hidden readback queues one READ_FIFO");
    require(g_drained == 0, "the hidden readback never drains");
  }
  std::vector<std::uint8_t> icon(512 * 16, 0xEE);
  require(client::read_fifo(icon.data(), 512), "the icon readback is served");
  require(g_drained == 1, "the icon readback drains the queued requests");
  for (std::uint64_t i = 0; i < icon.size(); ++i) require(icon[i] == pattern_byte(3, i), "icon reply is request 3");
  client::finish(stdout);
}

int scenario_main(const std::string &scenario, std::uint32_t frames) {
  if (scenario == "pcsx2-local") {
    run_local_script();
    return 0;
  }
  if (scenario == "pcsx2-local-off") {
    run_local_off_script();
    return 0;
  }
  if (scenario == "pcsx2-hidden") {
    run_hidden_script();
    return 0;
  }
  if (scenario == "donor") {
    const auto data = qwords_of(4, 1);
    require(!gt4::gs_stream::stream_renderer(), "donor by default");
    require(!gt4::gs_stream::on_gif_render(3, data.data(), 4), "donor GIF hook returns false");
    std::uint8_t reply[16] = {7};
    require(!gt4::gs_stream::read_fifo(reply, 1) && reply[0] == 7, "donor readback untouched");
    gt4::gs_stream::on_vsync(true);
    gt4::gs_stream::finish(stdout);
    return 0;
  }
  if (scenario == "game-killed") {
    const auto data = qwords_of(1, 1);
    gt4::gs_stream::on_gif_render(3, data.data(), 1);  // starts the GS process (handshake)
    std::puts("[fixture] game-killed: GS process ready, waiting to be terminated");
    std::fflush(stdout);
    Sleep(60000);
    return 8;
  }
  if (scenario == "early-exit" || scenario == "no-process" || scenario == "bad-renderer" || scenario == "pin-mismatch" ||
      scenario == "bad-config" || scenario == "handshake-timeout") {
    const auto data = qwords_of(1, 1);
    gt4::gs_stream::on_gif_render(3, data.data(), 1);  // refuses (exits) on first use
    return 0;
  }
  if (scenario == "osd") {
    // TASK-060/062: a 1.2 consumer that asked for OSD_STATS. Frames are paced at about 25 ms so the 0.5 s OSD interval
    // passes at least twice; the window handle is a placeholder (only the CONTROL records are checked).
    namespace client = gt4::gs_stream;
    client::set_cursor_policy(true, 3000);
    client::set_halt_handler(count_halt);
    client::on_gs_reset();
    client::attach_window(0x1234, 640, 360);
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
      // TASK-080 (protocol 1.3): fill from frame 10 with the P2 target 2.44 (args[1] = 2,440,000; a custom ratio wider
      // than the window must be letterboxed by the consumer), the same again at 11 (no second record), game aspect at 30.
      if (frame == 10 || frame == 11)
        client::set_display_aspect_mode(client::DisplayAspectMode::fill, client::aspect_millionths(2.44));
      if (frame == 30) client::set_display_aspect_mode(client::DisplayAspectMode::game_aspect);
      const auto data = qwords_of(4, static_cast<std::uint8_t>(frame));
      client::on_gif_render(3, data.data(), 4);
      Sleep(25);
      client::on_vsync((frame & 1) != 0);
    }
    client::finish(stdout);
    return g_halts == 0 ? 0 : 6;
  }
  g_ordered = scenario == "pcsx2-ordered";
  run_script(frames);
  if (scenario == "peer-death") {
    // The fake GS process dies after 20 records (frame 1-2); the frames-in-flight wait sees it at the latest two VSYNCs
    // later, and the halt follows 3 VSYNCs after that.
    std::printf("[fixture] peer-death: %d halt(s), at frame %u\n", g_halts, g_halt_frame);
    return (g_halts == 1 && g_halt_frame >= 3 && g_halt_frame <= 8) ? 0 : 5;
  }
  return g_halts == 0 ? 0 : 6;
}

// --- parent side ---

std::wstring this_executable() {
  wchar_t path[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
  return std::wstring(path, length);
}

struct Env {
  const char *name;
  std::string value;  // empty: unset
};

std::uint32_t run_scenario(const std::string &scenario, const std::vector<Env> &env, std::uint32_t frames = 10) {
  for (const char *name : {"GT4_RENDERER", "GT4_GS_PROCESS", "GT4_GS_CAPTURE", "GT4_GS_RECORD_OUTPUT",
                           "GT4_GS_CAPTURE_FRAMES", "GT4_GS_FRAMES_IN_FLIGHT", "GT4_FIXTURE_CHILD",
                           "GT4_GS_PROCESS_SHA256", "GT4_GS_CONFIG", "GT4_GS_HANDSHAKE_TIMEOUT_MS",
                           "GT4_GS_LOST_GRACE_FRAMES", "GT4_FIXTURE_PID_FILE", "GT4_GS_READBACK_LOCAL",
                           "GT4_GS_HIDE_FLARES", "GT4_CORE2_PERF_FLARES", "GT4_GS_READBACK_LOG"})
    _putenv_s(name, "");
  for (const Env &item : env) _putenv_s(item.name, item.value.c_str());
  std::printf("[fixture] scenario %s\n", scenario.c_str());
  std::fflush(stdout);
  gs_stream::ipc::ChildProcess child = gs_stream::ipc::ChildProcess::spawn(
      this_executable(), L"--scenario " + gs_stream::ipc::widen(scenario) + L" " + std::to_wstring(frames));
  std::uint32_t code = 99;
  require(child.wait_exit(120000, &code), "scenario finished in time");
  for (const Env &item : env) _putenv_s(item.name, "");
  std::printf("[fixture] scenario %s exit code %u\n", scenario.c_str(), code);
  return code;
}

std::vector<char> read_all(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<char>(std::istreambuf_iterator<char>(file), {});
}

bool exists(const std::string &path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// The pid a fake GS process wrote to its pid file (0 if none yet).
DWORD read_pid(const std::string &path) {
  std::ifstream file(path);
  unsigned long pid = 0;
  file >> pid;
  return static_cast<DWORD>(pid);
}

// True when process `pid` ends within `timeout_ms` (or is already gone).
bool process_ends(DWORD pid, DWORD timeout_ms) {
  HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (!process) return true;
  const bool ended = WaitForSingleObject(process, timeout_ms) == WAIT_OBJECT_0;
  CloseHandle(process);
  return ended;
}

void check_stream_file(const std::string &path, std::uint32_t frames) {
  FileReader reader;
  std::string error;
  require(reader.open(path, error), "stream file opens");
  require(std::string(reader.header().serial) == "SCUS-97328" && reader.header().first_frame == 0, "file header");
  RecordHeader header;
  std::vector<std::uint8_t> payload;
  std::uint64_t sequence = 0, gif = 0, more = 0, priv = 0, vsync = 0, fifo = 0, resets = 0, controls = 0;
  std::uint64_t syncv_writes = 0;
  while (reader.next(header, payload)) {
    require(header.sequence == ++sequence, "file sequence numbers are consecutive");
    switch (static_cast<RecordType>(header.type)) {
      case RecordType::gif: {
        ++gif;
        if (header.flags & kRecordFlagMore) ++more;
        GifPayload g;
        std::memcpy(&g, payload.data(), sizeof g);
        require(g.path >= 1 && g.path <= 3 && g.bytes + sizeof g == payload.size(), "GIF payload");
        break;
      }
      case RecordType::priv: {
        ++priv;
        PrivPayload p;
        std::memcpy(&p, payload.data(), sizeof p);
        if (p.address == 0x12000060u) {
          require(p.value == 0x00C7800601A01801ull, "SetGsCrt NTSC posts the NTSC SYNCV");
          ++syncv_writes;
        }
        break;
      }
      case RecordType::vsync: {
        VsyncPayload v;
        std::memcpy(&v, payload.data(), sizeof v);
        require(v.frame == vsync && v.field == (vsync & 1) && (v.flags & kVsyncFlagFieldValid), "VSYNC frame and field");
        ++vsync;
        break;
      }
      case RecordType::read_fifo: {
        ReadFifoPayload r;
        std::memcpy(&r, payload.data(), sizeof r);
        require(r.qwords == 512 && r.request_id == 1, "READ_FIFO payload");
        ++fifo;
        break;
      }
      case RecordType::reset: ++resets; break;
      case RecordType::control: ++controls; break;
      default: throw std::runtime_error("unexpected record type in the file");
    }
  }
  // Per frame: 3 GIF, 2 PRIV, 1 VSYNC; plus the split chunk (2 records), 3 SetGsCrt PRIV (SMODE1, SYNCV, SMODE2),
  // 1 READ_FIFO, 2 RESET.
  require(gif == 3ull * frames + 2 && more == 1, "GIF record count and split");
  require(syncv_writes == 1, "one SYNCV write from SetGsCrt");
  require(priv == 2ull * frames + 3 && vsync == frames && fifo == 1 && resets == 2, "record counts");
  // The pcsx2 run also carries CONTROL shutdown at the end; the donor capture does not.
  require(controls <= 1, "control records");
  std::printf("[fixture] %s: %llu records ok\n", path.c_str(), static_cast<unsigned long long>(sequence));
}

void check_crt_table() {
  std::uint64_t smode1 = 0, smode2 = 0;
  require(gt4::gs_stream::map_set_gs_crt(1, 0x02, 1, smode1, smode2) && smode1 == 0x4100 && smode2 == 3, "NTSC 480i");
  require(gt4::gs_stream::map_set_gs_crt(1, 0x02, 0, smode1, smode2) && smode1 == 0x4100 && smode2 == 1, "NTSC field mode");
  require(gt4::gs_stream::map_set_gs_crt(0, 0x50, 1, smode1, smode2) && smode1 == 0x100 && smode2 == 0, "480p");
  require(gt4::gs_stream::map_set_gs_crt(1, 0x51, 1, smode1, smode2) && smode1 == 0xB0 && smode2 == 3, "1080i");
  require(gt4::gs_stream::map_set_gs_crt(0, 0x03, 0, smode1, smode2) && smode1 == 0x6100 && smode2 == 0, "PAL");
  require(gt4::gs_stream::map_set_gs_crt(0, 0x1A, 0, smode1, smode2) && smode1 == 0x78, "VESA");
  require(!gt4::gs_stream::map_set_gs_crt(0, 0x51, 1, smode1, smode2), "1080i must be interlaced");
  require(!gt4::gs_stream::map_set_gs_crt(1, 0x77, 0, smode1, smode2), "unknown mode refused");
  require(!gt4::gs_stream::map_set_gs_crt(1, 0x02, 2, smode1, smode2), "bad field refused");
  std::uint64_t syncv = 0;
  require(gt4::gs_stream::map_set_gs_crt_syncv(0x02, syncv) && syncv == 0x00C7800601A01801ull && (syncv & 1),
          "NTSC SYNCV (VFP odd)");
  require(gt4::gs_stream::map_set_gs_crt_syncv(0x03, syncv) && syncv == 0x00A9000502101401ull && (syncv & 1),
          "PAL SYNCV (VFP odd)");
  require(!gt4::gs_stream::map_set_gs_crt_syncv(0x50, syncv) && !gt4::gs_stream::map_set_gs_crt_syncv(0x51, syncv) &&
              !gt4::gs_stream::map_set_gs_crt_syncv(0x1A, syncv),
          "no SYNCV for the progressive, HDTV and VESA modes");
  std::puts("[fixture] SetGsCrt table ok");
}

// TASK-041: the privileged register table and the F8 summary line.
void check_privileged_table_and_summary() {
  namespace client = gt4::gs_stream;
  require(client::privileged_register_index(0x12000000u) == 0 && std::string(client::privileged_register_name(0)) == "PMODE",
          "PMODE");
  require(std::string(client::privileged_register_name(client::privileged_register_index(0x12000070u))) == "DISPFB1",
          "DISPFB1");
  require(std::string(client::privileged_register_name(client::privileged_register_index(0x120000E0u))) == "BGCOLOR",
          "BGCOLOR");
  require(std::string(client::privileged_register_name(client::privileged_register_index(0x12001000u))) == "CSR", "CSR");
  require(std::string(client::privileged_register_name(client::privileged_register_index(0x12001010u))) == "IMR", "IMR");
  require(std::string(client::privileged_register_name(client::privileged_register_index(0x12001040u))) == "BUSDIR",
          "BUSDIR");
  require(std::string(client::privileged_register_name(client::privileged_register_index(0x12001080u))) == "SIGLBLID",
          "SIGLBLID");
  require(std::string(client::privileged_register_name(client::privileged_register_index(0x120000F0u))) == "other" &&
              std::string(client::privileged_register_name(client::privileged_register_index(0x11000000u))) == "other",
          "unknown addresses");
  client::FrameCosts costs;
  // TASK-084 step 0: the critical path is the frame wall minus the pacing sleep; the waits are inside the wall time.
  // The owner's night-race numbers (EE CPU 8.761, VU1 wait 3.481, readback 0.776) inside a paced 19.50 ms frame with
  // 6.48 ms of pacer sleep give the same 13.02 ms path the old "CPU + waits" formula produced by accident.
  costs.frame_wall_ms = 19.50;
  costs.pacing_sleep_ms = 6.48;
  costs.ee_cpu_ms = 8.761;
  costs.vu1_worker_wait_ms = 3.481;
  costs.readback_wait_ms = 0.776;
  costs.backpressure_ms = 0.0;
  costs.gs_cpu_thread_ms = 0.948;
  const std::string line = client::format_frame_summary(costs);
  std::printf("[fixture] %s\n", line.c_str());
  require(line.find("EE critical path 13.02 ms/frame of 16.68 budget (78%)") != std::string::npos &&
              line.find("frame wall 19.50 minus pacing sleep 6.48") != std::string::npos &&
              line.find("headroom 3.66 ms") != std::string::npos &&
              line.find("GS process CPU thread 0.95 ms/frame") != std::string::npos,
          "summary of the owner's night-race numbers inside a paced frame");
  costs.frame_wall_ms = -1.0;
  costs.ee_cpu_ms = -1.0;
  costs.gs_cpu_thread_ms = -1.0;
  const std::string unknown = client::format_frame_summary(costs);
  require(unknown.find("EE critical path Unknown") != std::string::npos &&
              unknown.find("GS process CPU thread Unknown") != std::string::npos,
          "summary without a frame wall or CPU times");
  std::puts("[fixture] privileged register table and summary ok");
}

// TASK-062: OSD_STATS from two cumulative samples (the owner's slice194 night-race F8 numbers per frame, 30 frames).
void check_osd_stats() {
  namespace client = gt4::gs_stream;
  client::OsdSample begin, end;
  begin.wall_us = 1000000;
  end.wall_us = begin.wall_us + 30 * 16683;
  begin.vsyncs = 100;
  end.vsyncs = 130;
  begin.main_cpu_us = 5000000;
  end.main_cpu_us = begin.main_cpu_us + 30 * 8761;
  begin.worker_busy_us = 0;
  end.worker_busy_us = 30 * 6100;
  begin.accounting = end.accounting = true;
  end.worker_wait_us = 30 * 3481;
  end.readback_wait_us = 30 * 776;
  end.stream_write_us = 30 * 590;
  // TASK-084 step 0: the paced frame wall includes 3.66 ms of pacer sleep, so the critical path is 16683 - 3663 us.
  end.pacing_sleep_us = 30 * 3663;
  const OsdStatsPayload stats = client::make_osd_stats(begin, end, 17100);
  require(stats.valid == (kOsdStatFrameTimes | kOsdStatMainThreadCpu | kOsdStatWorkerBusy | kOsdStatStreamCosts |
                          kOsdStatWorkerWait | kOsdStatCriticalPath),
          "every OSD field is valid with the accounting on");
  require(stats.frames == 30 && stats.interval_us == 30 * 16683 && stats.frame_time_avg_us == 16683 &&
              stats.frame_time_max_us == 17100,
          "OSD frame times");
  require(stats.main_thread_cpu_us == 8761 && stats.worker_busy_us == 6100 && stats.worker_wait_us == 3481 &&
              stats.readback_wait_us == 776 && stats.stream_write_us == 590 && stats.backpressure_us == 0,
          "OSD per-frame costs");
  require(stats.critical_path_us == 16683 - 3663 && stats.frame_budget_us == 16683,
          "OSD critical path is the frame wall minus the pacer's sleep (13.02 of 16.68 ms)");
  // Without the accounting only the frame times and the EE CPU are known; an empty interval is not valid at all.
  begin.accounting = end.accounting = false;
  const OsdStatsPayload partial = client::make_osd_stats(begin, end, 17100);
  require(partial.valid == (kOsdStatFrameTimes | kOsdStatMainThreadCpu) && partial.critical_path_us == 0,
          "OSD without accounting");
  require(client::make_osd_stats(end, end, 0).valid == 0, "OSD of an empty interval");
  std::puts("[fixture] OSD_STATS values ok");
}

}  // namespace

int main(int argc, char **argv) {
  try {
    if (argc == 4 && std::string(argv[1]) == "--scenario") return scenario_main(argv[2], std::stoul(argv[3]));
    if (argc >= 2 && std::string(argv[1]) == "--ipc") {
      // Fake GS process. Without GT4_FIXTURE_CHILD it exits at once (early-exit); with it, it posts READY and dies
      // after 20 records (peer-death).
      const char *mode = std::getenv("GT4_FIXTURE_CHILD");
      if (!mode || !*mode) return 7;
      if (const char *pid_file = std::getenv("GT4_FIXTURE_PID_FILE"); pid_file && *pid_file) {
        std::ofstream out(pid_file);
        out << GetCurrentProcessId();
      }
      if (std::string(mode) == "silent") {
        Sleep(30000);  // never answers the handshake; the game's job ends this process
        return 10;
      }
      auto session = ipc::Session::open_consumer(argv[2]);
      if (std::string(mode) == "osd") {
        // TASK-060/062: a protocol 1.2 consumer with an overlay. Counts OSD_STATS and CONTROL cursor records until
        // CONTROL shutdown and writes the result to GT4_FIXTURE_PID_FILE's sibling ".osd" file.
        ReadyPayload osd_ready{};
        osd_ready.version_major = kVersionMajor;
        // TASK-080: GT4_FIXTURE_READY_MINOR = 3 plays a protocol 1.3 consumer; default 2 (a gt4-gs 1.0.x process).
        const char *minor_text = std::getenv("GT4_FIXTURE_READY_MINOR");
        osd_ready.version_minor = static_cast<std::uint16_t>(minor_text && *minor_text ? std::atoi(minor_text) : 2);
        osd_ready.flags = kReadyFlagOsdStats;
        osd_ready.consumer_pid = ipc::current_process_id();
        session->post_event(RecordType::ready, &osd_ready, sizeof osd_ready);
        std::uint64_t osd_records = 0, cursor_flags = 99, cursor_delay = 0, vsyncs = 0;
        std::uint64_t aspect_records = 0, aspect_modes = 0, aspect_after_vsync = 0;  // modes: one decimal digit each
        std::uint32_t valid_all = ~0u, frames_total = 0, budget = 0, max_avg = 0;
        ipc::RecordView view;
        for (;;) {
          session->next_command(view);
          const auto type = static_cast<RecordType>(view.header->type);
          if (type == RecordType::vsync) {
            ++vsyncs;
            session->release_command(view);
            session->complete_vsync();
            continue;
          }
          if (type == RecordType::osd_stats && view.header->payload_bytes == sizeof(OsdStatsPayload)) {
            OsdStatsPayload stats;
            std::memcpy(&stats, view.payload, sizeof stats);
            ++osd_records;
            valid_all &= stats.valid;
            frames_total += stats.frames;
            budget = stats.frame_budget_us;
            max_avg = std::max(max_avg, stats.frame_time_avg_us);
          }
          bool shutdown = false;
          if (type == RecordType::control) {
            ControlPayload control;
            std::memcpy(&control, view.payload, sizeof control);
            if (control.kind == static_cast<std::uint32_t>(ControlKind::cursor)) {
              cursor_flags = control.args[0];
              cursor_delay = control.args[1];
            }
            if (control.kind == static_cast<std::uint32_t>(ControlKind::display_aspect)) {
              ++aspect_records;
              aspect_modes = aspect_modes * 10 + control.args[0];
              aspect_after_vsync = vsyncs;
              // P2: fill carries the target aspect x 1e6 in args[1]; game aspect sends 0. args[2] stays 0.
              const std::uint64_t expected_target = control.args[0] == 1 ? 2440000u : 0u;
              if (control.args[1] != expected_target || control.args[2] != 0) aspect_modes = 999;
            }
            shutdown = control.kind == static_cast<std::uint32_t>(ControlKind::shutdown);
          }
          session->release_command(view);
          if (shutdown) break;
        }
        if (const char *result = std::getenv("GT4_FIXTURE_PID_FILE"); result && *result) {
          std::ofstream out(std::string(result) + ".osd");
          out << osd_records << ' ' << valid_all << ' ' << frames_total << ' ' << max_avg << ' ' << budget << ' '
              << cursor_flags << ' ' << cursor_delay << ' ' << vsyncs << ' ' << aspect_records << ' ' << aspect_modes
              << ' ' << aspect_after_vsync;
        }
        return 0;
      }
      if (std::string(mode) == "pattern") {
        // TASK-116 step 1: answers every READ_FIFO with pattern_byte(request_id, i) and writes how many it served, so the
        // local flare answer can be checked byte for byte (and the request sequence of the stream).
        ReadyPayload pattern_ready{};
        pattern_ready.version_major = kVersionMajor;
        pattern_ready.version_minor = 3;
        pattern_ready.consumer_pid = ipc::current_process_id();
        session->post_event(RecordType::ready, &pattern_ready, sizeof pattern_ready);
        std::uint64_t served = 0, last_id = 0;
        ipc::RecordView view;
        for (;;) {
          session->next_command(view);
          const auto type = static_cast<RecordType>(view.header->type);
          if (type == RecordType::read_fifo) {
            ReadFifoPayload request{};
            if (view.header->payload_bytes == sizeof request) {
              std::memcpy(&request, view.payload, sizeof request);
              const std::uint64_t bytes = std::uint64_t(request.qwords) * 16;
              for (std::uint64_t i = 0; i < bytes; ++i)
                session->response_area()[i] = pattern_byte(request.request_id, i);
              ++served;
              last_id = request.request_id;
              session->release_command(view);
              session->complete_readback(request.request_id, bytes, kReadbackOk);
              continue;
            }
            session->release_command(view);
            continue;
          }
          if (type == RecordType::vsync) {
            session->release_command(view);
            session->complete_vsync();
            continue;
          }
          session->release_command(view);
          if (type == RecordType::control) {
            ControlPayload pattern_control{};
            std::memcpy(&pattern_control, view.payload, sizeof pattern_control);
            if (pattern_control.kind == static_cast<std::uint32_t>(ControlKind::shutdown)) break;
          }
        }
        if (const char *result = std::getenv("GT4_FIXTURE_PID_FILE"); result && *result) {
          std::ofstream out(std::string(result) + ".pattern");
          out << served << ' ' << last_id;
        }
        return 0;
      }
      if (std::string(mode) == "hang") {
        // Posts READY, then ignores the game (no peer waits): only the kill-on-close job can end it.
        ReadyPayload hang_ready{};
        hang_ready.version_major = kVersionMajor;
        hang_ready.consumer_pid = ipc::current_process_id();
        session->post_event(RecordType::ready, &hang_ready, sizeof hang_ready);
        Sleep(60000);
        return 11;
      }
      ReadyPayload ready{};
      ready.version_major = kVersionMajor;
      ready.consumer_pid = ipc::current_process_id();
      session->post_event(RecordType::ready, &ready, sizeof ready);
      ipc::RecordView view;
      for (int n = 0; n < 20; ++n) {
        session->next_command(view);
        session->release_command(view);
      }
      ExitProcess(9);
    }
    if (argc != 3) {
      std::fputs("usage: gs_stream_client_fixture <gs_stream_record.exe> <scratch directory>\n", stderr);
      return 2;
    }
    check_crt_table();
    check_privileged_table_and_summary();
    check_osd_stats();
    const std::string recorder = argv[1];
    const std::string scratch = argv[2];
    CreateDirectoryA(scratch.c_str(), nullptr);
    const std::string donor_file = scratch + "\\donor-capture.gscap";
    const std::string pcsx2_capture = scratch + "\\pcsx2-capture.gscap";
    const std::string recorded = scratch + "\\pcsx2-recorded.gscap";
    for (const std::string &path : {donor_file, pcsx2_capture, recorded}) DeleteFileA(path.c_str());

    require(run_scenario("donor", {}) == 0, "donor scenario");
    require(!exists(donor_file), "donor without capture writes nothing");
    require(run_scenario("donor-capture", {{"GT4_RENDERER", "donor"}, {"GT4_GS_CAPTURE", donor_file}}) == 0,
            "donor capture scenario");
    std::string recorder_sha, hash_error;
    require(gt4::gs_stream::file_sha256(gs_stream::ipc::widen(recorder), recorder_sha, hash_error) && recorder_sha.size() == 64,
            "SHA-256 of the recorder");
    const std::string config = scratch + "\\gt4-gs.ini";
    {
      std::ofstream ini(config, std::ios::binary);
      ini << "[EmuCore/GS]\nupscale_multiplier = 1\n";
    }
    require(run_scenario("pcsx2", {{"GT4_RENDERER", "pcsx2"}, {"GT4_GS_PROCESS", recorder},
                                   {"GT4_GS_RECORD_OUTPUT", recorded}, {"GT4_GS_CAPTURE", pcsx2_capture},
                                   {"GT4_GS_PROCESS_SHA256", recorder_sha}, {"GT4_GS_CONFIG", config}}) == 0,
            "pcsx2 scenario (pinned process, --config)");    check_stream_file(donor_file, 10);
    check_stream_file(pcsx2_capture, 10);
    // The recorder's file also holds the CONTROL shutdown posted by finish() after the in-process capture closed.
    const std::vector<char> donor_bytes = read_all(donor_file), capture_bytes = read_all(pcsx2_capture);
    std::vector<char> recorded_bytes = read_all(recorded);
    require(donor_bytes == capture_bytes, "donor capture equals the pcsx2 in-process capture");
    require(recorded_bytes.size() == capture_bytes.size() + 48 &&
                std::equal(capture_bytes.begin(), capture_bytes.end(), recorded_bytes.begin()),
            "recorder file equals the in-process capture plus CONTROL shutdown");
    std::puts("[fixture] donor capture, pcsx2 capture and recorder file are byte-identical");
    // TASK-041: privileged writes queued behind held worker output reach the stream in the same order.
    const std::string ordered_capture = scratch + "\\pcsx2-ordered-capture.gscap";
    const std::string ordered_recorded = scratch + "\\pcsx2-ordered-recorded.gscap";
    for (const std::string &path : {ordered_capture, ordered_recorded}) DeleteFileA(path.c_str());
    require(run_scenario("pcsx2-ordered", {{"GT4_RENDERER", "pcsx2"}, {"GT4_GS_PROCESS", recorder},
                                           {"GT4_GS_RECORD_OUTPUT", ordered_recorded},
                                           {"GT4_GS_CAPTURE", ordered_capture}}) == 0,
            "pcsx2-ordered scenario");
    require(read_all(ordered_capture) == capture_bytes, "ordered privileged writes: capture equals the pcsx2 capture");
    require(read_all(ordered_recorded) == recorded_bytes, "ordered privileged writes: recorder file equals the pcsx2 one");
    std::puts("[fixture] queued privileged writes keep the stream byte-identical");

    // TASK-116 step 1: the flare readback answered locally in Performance mode (gt4-gs HWDownloadMode 2), and the kill
    // switch that turns it off. The consumer answers pattern_byte(request_id, i), so "the previous reply" is checked
    // byte for byte.
    const std::string local_config = scratch + "\\gt4-gs-local.ini";
    {
      std::ofstream ini(local_config, std::ios::binary);
      ini << "[EmuCore/GS]\nHWDownloadMode = 2\n";
    }
    const std::string local_result = scratch + "\\local.pid";
    DeleteFileA((local_result + ".pattern").c_str());
    require(run_scenario("pcsx2-local", {{"GT4_RENDERER", "pcsx2"}, {"GT4_GS_PROCESS", argv[0]},
                                         {"GT4_FIXTURE_CHILD", "pattern"}, {"GT4_FIXTURE_PID_FILE", local_result},
                                         {"GT4_GS_CONFIG", local_config}, {"GT4_GS_READBACK_LOCAL", "1"},
                                         {"GT4_GS_HIDE_FLARES", "0"}}) == 0,
            "pcsx2-local scenario");
    {
      std::ifstream in(local_result + ".pattern");
      std::uint64_t served = 0, last_id = 0;
      in >> served >> last_id;
      require(served == 3 && last_id == 3, "one READ_FIFO per readback, in stream order");
    }
    std::puts("[fixture] Performance mode: the flare readback is the previous reply, one READ_FIFO per readback");
    const std::string local_off_result = scratch + "\\local-off.pid";
    DeleteFileA((local_off_result + ".pattern").c_str());
    require(run_scenario("pcsx2-local-off", {{"GT4_RENDERER", "pcsx2"}, {"GT4_GS_PROCESS", argv[0]},
                                             {"GT4_FIXTURE_CHILD", "pattern"},
                                             {"GT4_FIXTURE_PID_FILE", local_off_result},
                                             {"GT4_GS_CONFIG", local_config}, {"GT4_GS_READBACK_LOCAL", "0"}}) == 0,
            "pcsx2-local-off scenario");
    {
      std::ifstream in(local_off_result + ".pattern");
      std::uint64_t served = 0;
      in >> served;
      require(served == 2, "the kill switch posts one READ_FIFO per readback and waits for each");
    }
    std::puts("[fixture] GT4_GS_READBACK_LOCAL=0 keeps every readback accurate");

    // TASK-117: GT4_GS_HIDE_FLARES=1 (the Performance mode default) answers the flare table as zeros from the first
    // readback, still queues one READ_FIFO per readback and keeps the icons accurate after the drain.
    const std::string hidden_result = scratch + "\\hidden.pid";
    DeleteFileA((hidden_result + ".pattern").c_str());
    require(run_scenario("pcsx2-hidden", {{"GT4_RENDERER", "pcsx2"}, {"GT4_GS_PROCESS", argv[0]},
                                          {"GT4_FIXTURE_CHILD", "pattern"}, {"GT4_FIXTURE_PID_FILE", hidden_result},
                                          {"GT4_GS_CONFIG", local_config}, {"GT4_GS_READBACK_LOCAL", "1"},
                                          {"GT4_GS_HIDE_FLARES", "1"}}) == 0,
            "pcsx2-hidden scenario");
    {
      std::ifstream in(hidden_result + ".pattern");
      std::uint64_t served = 0, last_id = 0;
      in >> served >> last_id;
      require(served == 3 && last_id == 3, "hidden flares: one READ_FIFO per readback, in stream order");
    }
    std::puts("[fixture] GT4_GS_HIDE_FLARES=1 answers the flare table as zeros and keeps the icons");

    // TASK-180 test5: the effect toggle's own name does the same on the game side, so the GSdx path honours the
    // Settings row without the older switch (the launcher sends both; a diagnosis build may send one).
    const std::string effect_result = scratch + "\\effect-hide.pid";
    DeleteFileA((effect_result + ".pattern").c_str());
    require(run_scenario("pcsx2-hidden", {{"GT4_RENDERER", "pcsx2"}, {"GT4_GS_PROCESS", argv[0]},
                                          {"GT4_FIXTURE_CHILD", "pattern"}, {"GT4_FIXTURE_PID_FILE", effect_result},
                                          {"GT4_GS_CONFIG", local_config}, {"GT4_GS_READBACK_LOCAL", "1"},
                                          {"GT4_CORE2_PERF_FLARES", "1"}}) == 0,
            "pcsx2-hidden scenario through the effect toggle");
    {
      std::ifstream in(effect_result + ".pattern");
      std::uint64_t served = 0, last_id = 0;
      in >> served >> last_id;
      if (served != 3 || last_id != 3) {
        std::printf("[fixture] the effect toggle served %llu with last id %llu\n",
                    static_cast<unsigned long long>(served), static_cast<unsigned long long>(last_id));
      }
      require(served == 3 && last_id == 3, "the effect toggle: one READ_FIFO per readback, in stream order");
    }
    std::puts("[fixture] GT4_CORE2_PERF_FLARES=1 answers the flare table the same way on the GSdx path");

    // TASK-117: GT4_GS_READBACK_LOCAL=1 also works with gt4-gs HWDownloadMode 0 (Accurate), so the flare table can be
    // answered locally while the car icons, loading picture and thumbnail stay Accurate (the mode-0 experiment).
    const std::string accurate_config = scratch + "\\gt4-gs-accurate.ini";
    {
      std::ofstream ini(accurate_config, std::ios::binary);
      ini << "[EmuCore/GS]\nHWDownloadMode = 0\n";
    }
    const std::string accurate_result = scratch + "\\accurate-local.pid";
    DeleteFileA((accurate_result + ".pattern").c_str());
    require(run_scenario("pcsx2-hidden", {{"GT4_RENDERER", "pcsx2"}, {"GT4_GS_PROCESS", argv[0]},
                                          {"GT4_FIXTURE_CHILD", "pattern"}, {"GT4_FIXTURE_PID_FILE", accurate_result},
                                          {"GT4_GS_CONFIG", accurate_config}, {"GT4_GS_READBACK_LOCAL", "1"}}) == 0,
            "pcsx2-hidden under Accurate");
    {
      std::ifstream in(accurate_result + ".pattern");
      std::uint64_t served = 0, last_id = 0;
      in >> served >> last_id;
      require(served == 3 && last_id == 3, "accurate mode: one READ_FIFO per readback, in stream order");
    }
    std::puts("[fixture] GT4_GS_READBACK_LOCAL=1 works with Accurate readbacks (mode 0)");

    // TASK-060/062 (protocol 1.2): CONTROL cursor after attach_window, OSD_STATS about every 0.5 s to a consumer that
    // asked for them, and never in the .gscap capture.
    const std::string osd_result = scratch + "\\osd.pid";
    const std::string osd_capture = scratch + "\\osd-capture.gscap";
    for (const std::string &path : {osd_result + ".osd", osd_capture}) DeleteFileA(path.c_str());
    require(run_scenario("osd", {{"GT4_RENDERER", "pcsx2"}, {"GT4_GS_PROCESS", argv[0]}, {"GT4_FIXTURE_CHILD", "osd"},
                                 {"GT4_FIXTURE_PID_FILE", osd_result}, {"GT4_GS_CAPTURE", osd_capture}},
                         60) == 0,
            "osd scenario");
    {
      std::ifstream in(osd_result + ".osd");
      std::uint64_t records = 0, valid = 0, frames_total = 0, max_avg = 0, budget = 0, flags = 0, delay = 0, vsyncs = 0;
      std::uint64_t aspect_records = 99, aspect_modes = 99, aspect_after = 99;
      in >> records >> valid >> frames_total >> max_avg >> budget >> flags >> delay >> vsyncs >> aspect_records >>
          aspect_modes >> aspect_after;
      // TASK-080: a 1.2 consumer (gt4-gs 1.0.x) is never sent CONTROL display_aspect.
      require(aspect_records == 0, "a protocol 1.2 consumer gets no CONTROL display_aspect");
      std::printf("[fixture] osd: %llu OSD_STATS records, valid bits 0x%llx, %llu frames, max mean frame %llu us, "
                  "cursor flags %llu delay %llu ms, %llu vsyncs\n",
                  static_cast<unsigned long long>(records), static_cast<unsigned long long>(valid),
                  static_cast<unsigned long long>(frames_total), static_cast<unsigned long long>(max_avg),
                  static_cast<unsigned long long>(flags), static_cast<unsigned long long>(delay),
                  static_cast<unsigned long long>(vsyncs));
      require(vsyncs == 60, "the osd consumer saw every VSYNC");
      require(records >= 2 && records <= 4, "about one OSD_STATS record per 0.5 s (60 frames of 25 ms)");
      require((valid & kOsdStatFrameTimes) && (valid & kOsdStatMainThreadCpu), "OSD frame times and EE CPU are valid");
      require(max_avg >= 20000 && max_avg < 200000, "OSD mean frame time near the 25 ms pacing");
      require(flags == kCursorFlagAutoHide && delay == 3000, "CONTROL cursor carries the window's policy");
      FileReader reader;
      std::string error;
      require(reader.open(osd_capture, error), "osd capture opens");
      RecordHeader header;
      std::vector<std::uint8_t> payload;
      std::uint64_t captured_osd = 0, captured_cursor = 0;
      while (reader.next(header, payload)) {
        if (header.type == static_cast<std::uint16_t>(RecordType::osd_stats)) ++captured_osd;
        if (header.type == static_cast<std::uint16_t>(RecordType::control) && payload.size() >= sizeof(ControlPayload)) {
          ControlPayload control;
          std::memcpy(&control, payload.data(), sizeof control);
          if (control.kind == static_cast<std::uint32_t>(ControlKind::cursor)) ++captured_cursor;
        }
      }
      require(captured_osd == 0, "OSD_STATS never reach the .gscap capture");
      require(captured_cursor == 1, "CONTROL cursor is part of the stream (once, after attach_window)");
    }
    std::puts("[fixture] protocol 1.2: CONTROL cursor and OSD_STATS ok");

    // TASK-080 (protocol 1.3): a 1.3 consumer gets CONTROL display_aspect once per change (fill, then game aspect),
    // each right after a VSYNC, and the records are part of the stream like CONTROL cursor.
    const std::string aspect_result = scratch + "\\aspect.pid";
    const std::string aspect_capture = scratch + "\\aspect-capture.gscap";
    for (const std::string &path : {aspect_result + ".osd", aspect_capture}) DeleteFileA(path.c_str());
    require(run_scenario("osd", {{"GT4_RENDERER", "pcsx2"}, {"GT4_GS_PROCESS", argv[0]}, {"GT4_FIXTURE_CHILD", "osd"},
                                 {"GT4_FIXTURE_READY_MINOR", "3"}, {"GT4_FIXTURE_PID_FILE", aspect_result},
                                 {"GT4_GS_CAPTURE", aspect_capture}},
                         60) == 0,
            "display aspect scenario (1.3 consumer)");
    {
      std::ifstream in(aspect_result + ".osd");
      std::uint64_t records = 0, valid = 0, frames_total = 0, max_avg = 0, budget = 0, flags = 0, delay = 0, vsyncs = 0;
      std::uint64_t aspect_records = 99, aspect_modes = 99, aspect_after = 99;
      in >> records >> valid >> frames_total >> max_avg >> budget >> flags >> delay >> vsyncs >> aspect_records >>
          aspect_modes >> aspect_after;
      std::printf("[fixture] display aspect: %llu CONTROL display_aspect records, modes %llu, last after VSYNC %llu\n",
                  static_cast<unsigned long long>(aspect_records), static_cast<unsigned long long>(aspect_modes),
                  static_cast<unsigned long long>(aspect_after));
      require(aspect_records == 2 && aspect_modes == 10,
              "1.3 consumer: fill (target 2,440,000) then game aspect (target 0), one record each");
      // P2 (slice217): the fill target value. 0 means the whole window; outside 0.5 .. 8.0 the producer sends 0.
      require(gt4::gs_stream::aspect_millionths(2.44) == 2440000u, "target 2.44 -> 2,440,000");
      require(gt4::gs_stream::aspect_millionths(16.0 / 9.0) == 1777778u, "target 16:9 -> 1,777,778");
      require(gt4::gs_stream::aspect_millionths(32.0 / 9.0) == 3555556u, "target 32:9 -> 3,555,556");
      require(gt4::gs_stream::aspect_millionths(0.4) == 0u && gt4::gs_stream::aspect_millionths(9.0) == 0u,
              "targets outside 0.5 .. 8.0 -> 0 (whole window)");
      require(aspect_after == 31, "the game-aspect record follows the VSYNC of the frame that asked for it");
      FileReader reader;
      std::string error;
      require(reader.open(aspect_capture, error), "aspect capture opens");
      RecordHeader header;
      std::vector<std::uint8_t> payload;
      std::uint64_t captured_aspect = 0;
      while (reader.next(header, payload)) {
        if (header.type == static_cast<std::uint16_t>(RecordType::control) && payload.size() >= sizeof(ControlPayload)) {
          ControlPayload control;
          std::memcpy(&control, payload.data(), sizeof control);
          if (control.kind == static_cast<std::uint32_t>(ControlKind::display_aspect)) ++captured_aspect;
        }
      }
      require(captured_aspect == 2, "CONTROL display_aspect records are part of the stream");
    }
    std::puts("[fixture] protocol 1.3: CONTROL display_aspect ok; a 1.2 consumer is not sent it");

    require(run_scenario("bad-renderer", {{"GT4_RENDERER", "bogus"}}) == 1, "unknown renderer refused");
    require(run_scenario("no-process", {{"GT4_RENDERER", "pcsx2"}}) == 1, "missing GT4_GS_PROCESS refused");
    require(run_scenario("pin-mismatch", {{"GT4_RENDERER", "pcsx2"}, {"GT4_GS_PROCESS", recorder},
                                          {"GT4_GS_PROCESS_SHA256", std::string(64, '0')}}) == 1,
            "GS process with another SHA-256 refused");
    require(run_scenario("bad-config", {{"GT4_RENDERER", "pcsx2"}, {"GT4_GS_PROCESS", recorder},
                                        {"GT4_GS_CONFIG", scratch + "\\missing.ini"}}) == 1,
            "missing GT4_GS_CONFIG file refused");
    const std::string self(argv[0]);
    // early-exit: GT4_GS_PROCESS points at a program that exits at once (this fixture with an unknown argument list).
    require(run_scenario("early-exit", {{"GT4_RENDERER", "pcsx2"}, {"GT4_GS_PROCESS", self}}) == 1,
            "GS process exiting during the handshake is refused");
    require(run_scenario("peer-death", {{"GT4_RENDERER", "pcsx2"}, {"GT4_GS_PROCESS", self}, {"GT4_FIXTURE_CHILD", "1"},
                                        {"GT4_GS_LOST_GRACE_FRAMES", "3"}},
                         10) == 0,
            "GS process death mid-stream halts once, after the grace VSYNCs");

    // TASK-038: handshake timeout with a clear message, and the job ends the silent GS process with the game.
    const std::string silent_pid = scratch + "\\silent.pid";
    DeleteFileA(silent_pid.c_str());
    const DWORD timeout_start = GetTickCount();
    require(run_scenario("handshake-timeout", {{"GT4_RENDERER", "pcsx2"}, {"GT4_GS_PROCESS", self},
                                               {"GT4_FIXTURE_CHILD", "silent"}, {"GT4_GS_HANDSHAKE_TIMEOUT_MS", "1500"},
                                               {"GT4_FIXTURE_PID_FILE", silent_pid}}) == 1,
            "GS process without READY is refused");
    const DWORD timeout_elapsed = GetTickCount() - timeout_start;
    std::printf("[fixture] handshake-timeout refused after %lu ms\n", static_cast<unsigned long>(timeout_elapsed));
    require(timeout_elapsed >= 1400 && timeout_elapsed < 15000, "refused after the configured handshake timeout");
    const DWORD silent = read_pid(silent_pid);
    require(silent != 0, "the silent GS process wrote its pid");
    require(process_ends(silent, 3000), "the job ended the silent GS process with the game");
    std::puts("[fixture] handshake timeout: refused, silent GS process ended by the job");

    // TASK-038: the game is killed; the GS process ignores the game, so only the kill-on-close job can end it.
    const std::string hang_pid = scratch + "\\hang.pid";
    DeleteFileA(hang_pid.c_str());
    for (const char *name : {"GT4_GS_PROCESS", "GT4_FIXTURE_CHILD", "GT4_FIXTURE_PID_FILE", "GT4_RENDERER"}) _putenv_s(name, "");
    _putenv_s("GT4_RENDERER", "pcsx2");
    _putenv_s("GT4_GS_PROCESS", self.c_str());
    _putenv_s("GT4_FIXTURE_CHILD", "hang");
    _putenv_s("GT4_FIXTURE_PID_FILE", hang_pid.c_str());
    {
      gs_stream::ipc::ChildProcess game = gs_stream::ipc::ChildProcess::spawn(this_executable(), L"--scenario game-killed 1");
      DWORD hung = 0;
      for (int tries = 0; tries < 200 && !(hung = read_pid(hang_pid)); ++tries) Sleep(50);
      require(hung != 0, "the hanging GS process started");
      Sleep(500);  // the game finishes its handshake
      require(!process_ends(hung, 0), "the hanging GS process runs while the game runs");
      TerminateProcess(game.process(), 1);
      std::uint32_t code = 0;
      require(game.wait_exit(5000, &code), "the game process ended");
      require(process_ends(hung, 3000), "the job ended the GS process after the game was killed");
    }
    for (const char *name : {"GT4_GS_PROCESS", "GT4_FIXTURE_CHILD", "GT4_FIXTURE_PID_FILE", "GT4_RENDERER"}) _putenv_s(name, "");
    std::puts("[fixture] game killed: GS process ended by the job");
    std::puts("[fixture] all checks passed");
    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "[fixture] FAIL: %s\n", error.what());
    return 1;
  }
}

// SPDX-License-Identifier: GPL-3.0+
//
// GT4Recomp private Android test build (PCSX2-tainted, GPL). NOT distributed. See replayer.h.
//
// Ported from the owner's fork frontend (gt4recomp-gs, pcsx2/GSStreamReplayer.cpp, GPL-3.0+); the per-record work is
// kept as it is there, minus the Windows window thread, the OSD overlay and the swap-chain statistics.

#include "gt4gs_replayer.h"

#include "common/Console.h"
#include "common/Timer.h"

#include "GS.h"
#include "GS/GS.h"
#include "GSDumpReplayer.h"
#include "Gif_Unit.h"
#include "Host.h"
#include "MTGS.h"
#include "PerformanceMetrics.h"
#include "R3000A.h"
#include "R5900.h"
#include "VMManager.h"
#include "VUmicro.h"
#include "Vif_Dma.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include "gt4gs_host.h"
#include "gt4gs_queue.h"

// The CPU provider object (defined at the end of this file; the VMManager hook names it at global scope).
extern R5900cpu Gt4gsStreamCpu;

namespace proto = gs_stream;

namespace gt4gs {
namespace {

// Privileged register offsets inside the 8 KiB block.
constexpr u32 kPmode = 0x0000;
constexpr u32 kDispfb1 = 0x0070;
constexpr u32 kDispfb2 = 0x0090;
constexpr u32 kCsr = 0x1000;
constexpr u32 kImr = 0x1010;
constexpr u32 kCsrFieldBit = 0x2000;
constexpr u32 kStatsEveryVsyncs = 300;
constexpr u32 kTransferRegisterFirst = 0x50;  // BITBLTBUF
constexpr u32 kTransferRegisterLast = 0x52;   // TRXREG
constexpr u64 kDisplayAspectGame = 0;
constexpr u64 kDisplayAspectFill = 1;
constexpr u32 kErrorCodeBadArgument = 7;

QueueSource *s_source = nullptr;
std::string s_renderer_name;
std::string s_version;

bool s_started = false;
bool s_finished = false;
bool s_running = false;
int s_exit_code = 1;

u64 s_display_aspect_mode = kDisplayAspectGame;
bool s_registers_written = false;
bool s_display_field = false;
std::vector<u8> s_gif_parts;
u8 s_gif_parts_path = 0;
std::vector<u8> s_readback_buffer;
std::vector<u32> s_snapshot_pixels;

// GIF tag scanner per path: follows PACKED/REGLIST/IMAGE tags across records and copies A+D writes to BITBLTBUF,
// TRXPOS and TRXREG (0x50-0x52) into vif1.transfer_registers, as Gif_HandlerAD does on the EE side. Only the
// unsynchronized readback mode reads these.
struct GifTransferRegisterScanner {
  u32 loops_left = 0;
  u32 registers = 0;
  u32 register_index = 0;
  u64 descriptors = 0;
  u32 format = 0;
  u64 words_left = 0;

  void Scan(const u8 *data, u32 bytes) {
    for (u32 at = 0; at + 16 <= bytes; at += 16) {
      const u8 *qword = data + at;
      u64 low, high;
      std::memcpy(&low, qword, 8);
      std::memcpy(&high, qword + 8, 8);
      if (loops_left == 0) {
        loops_left = static_cast<u32>(low & 0x7FFF);
        format = static_cast<u32>((low >> 58) & 3);
        registers = static_cast<u32>((low >> 60) & 0xF);
        if (registers == 0) registers = 16;
        descriptors = high;
        register_index = 0;
        words_left = static_cast<u64>(loops_left) * registers;
        continue;
      }
      if (format == 0) {
        const u32 descriptor = static_cast<u32>((descriptors >> (4 * register_index)) & 0xF);
        if (descriptor == 0xE) {
          const u32 address = static_cast<u32>(high & 0xFF);
          if (address >= kTransferRegisterFirst && address <= kTransferRegisterLast)
            vif1.transfer_registers[address - kTransferRegisterFirst] = low;
        }
        if (++register_index == registers) {
          register_index = 0;
          --loops_left;
        }
      } else if (format == 1) {
        words_left = words_left > 2 ? words_left - 2 : 0;
        if (words_left == 0) loops_left = 0;
      } else {
        --loops_left;
      }
    }
  }
};
GifTransferRegisterScanner s_transfer_scanners[3];
u64 s_gif_interleaved = 0;

u64 s_frame = 0;
u64 s_records = 0;
u64 s_bytes = 0;
u64 s_readbacks = 0;
u64 s_gif_bytes = 0;
u64 s_report_frame = 0;

void PostError(proto::ErrorCode code, bool fatal, const std::string &text) {
  Console.Error("(gt4gs) %s%s", text.c_str(), fatal ? " (fatal)" : "");
  proto::ErrorPayload payload{};
  payload.code = static_cast<u32>(code);
  payload.fatal = fatal ? 1 : 0;
  std::strncpy(payload.text, text.c_str(), sizeof(payload.text) - 1);
  if (s_source != nullptr) s_source->PostEvent(static_cast<u16>(proto::RecordType::error), &payload, sizeof(payload));
}

const char *ReadbackModeName() {
  switch (EmuConfig.GS.HWDownloadMode) {
    case GSHardwareDownloadMode::Enabled: return "accurate";
    case GSHardwareDownloadMode::EnabledForceFull: return "accurate, full";
    case GSHardwareDownloadMode::NoReadbacks: return "no GPU readbacks";
    case GSHardwareDownloadMode::Unsynchronized: return "unsynchronized";
    case GSHardwareDownloadMode::Disabled: return "disabled";
  }
  return "unknown";
}

void PostStats() {
  const double frames = static_cast<double>(std::max<u64>(1, s_frame - s_report_frame));
  Console.WriteLn("(gt4gs) vsync %llu: stream %.0f bytes/frame; readbacks (%s) %llu; GS thread %.3f ms/frame (%.1f%%); GPU "
                  "%.3f ms (%.1f%%)",
                  static_cast<unsigned long long>(s_frame), (s_bytes - 0) / frames, ReadbackModeName(),
                  static_cast<unsigned long long>(s_readbacks), PerformanceMetrics::GetGSThreadAverageTime(),
                  PerformanceMetrics::GetGSThreadUsage(), PerformanceMetrics::GetGPUAverageTime(),
                  PerformanceMetrics::GetGPUUsage());
  s_report_frame = s_frame;
}

void Finish(int exit_code, bool post_exiting) {
  if (s_finished) return;
  s_finished = true;
  s_running = false;
  s_exit_code = exit_code;
  if (MTGS::IsOpen()) MTGS::WaitGS(false, false, false);
  if (s_source != nullptr && post_exiting) {
    PostStats();
    s_source->SetStateBits(proto::kStateExiting);
    proto::ExitingPayload exiting{};
    exiting.exit_code = static_cast<u32>(exit_code);
    s_source->PostEvent(static_cast<u16>(proto::RecordType::exiting), &exiting, sizeof(exiting));
  }
  Console.WriteLn("(gt4gs) finished: %llu vsyncs, %llu records, %llu bytes (%llu GIF bytes), %llu readbacks; exit code %d",
                  static_cast<unsigned long long>(s_frame), static_cast<unsigned long long>(s_records),
                  static_cast<unsigned long long>(s_bytes), static_cast<unsigned long long>(s_gif_bytes),
                  static_cast<unsigned long long>(s_readbacks), exit_code);
  Host::RequestVMShutdown(false, false, false);
}

void HardReset() {
  if (MTGS::IsOpen()) MTGS::WaitGS(false, false, false);
  std::memset(g_RealGSMem, 0, sizeof(g_RealGSMem));
  CSRreg.Reset();
  GSIMR.reset();
  s_display_field = false;
  s_registers_written = false;
  s_gif_parts.clear();
  for (GifTransferRegisterScanner &scanner : s_transfer_scanners) scanner = {};
  std::memset(vif1.transfer_registers, 0, sizeof(vif1.transfer_registers));
  MTGS::ResetGS(true);
}

void SendGif(u8 path, const u8 *data, u32 bytes) {
  if (bytes == 0) return;
  const GIF_PATH gif_path = static_cast<GIF_PATH>(path - 1);
  for (u32 other = 0; other < 3; ++other) {
    if (other != static_cast<u32>(path - 1) && s_transfer_scanners[other].loops_left != 0) {
      ++s_gif_interleaved;
      break;
    }
  }
  s_transfer_scanners[path - 1].Scan(data, bytes);
  Gif_Path &unit_path = gifUnit.gifPath[gif_path];
  unit_path.CopyGSPacketData(const_cast<u8 *>(data), bytes);
  GS_Packet packet;
  packet.offset = unit_path.curOffset;
  packet.size = bytes;
  unit_path.curOffset += bytes;
  Gif_AddCompletedGSPacket(packet, gif_path);
  s_gif_bytes += bytes;
}

bool DoGif(const Record &record) {
  proto::GifPayload gif;
  if (record.payload_bytes < sizeof(gif)) return false;
  std::memcpy(&gif, record.payload, sizeof(gif));
  if (gif.path < 1 || gif.path > 3 || (gif.bytes % 16) != 0 || sizeof(gif) + gif.bytes > record.payload_bytes) return false;
  const u8 *data = record.payload + sizeof(gif);
  if ((record.flags & proto::kRecordFlagMore) || !s_gif_parts.empty()) {
    if (!s_gif_parts.empty() && s_gif_parts_path != gif.path) return false;
    s_gif_parts_path = gif.path;
    s_gif_parts.insert(s_gif_parts.end(), data, data + gif.bytes);
    if (record.flags & proto::kRecordFlagMore) return true;
    SendGif(gif.path, s_gif_parts.data(), static_cast<u32>(s_gif_parts.size()));
    s_gif_parts.clear();
    return true;
  }
  SendGif(gif.path, data, gif.bytes);
  return true;
}

bool DoPriv(const Record &record) {
  proto::PrivPayload priv;
  if (record.payload_bytes < sizeof(priv)) return false;
  std::memcpy(&priv, record.payload, sizeof(priv));
  if (priv.address < proto::kPrivilegedBase || priv.address >= proto::kPrivilegedBase + proto::kPrivilegedBytes ||
      (priv.address & 7) != 0)
    return false;
  const u32 offset = priv.address - proto::kPrivilegedBase;
  if (offset == kCsr) {
    if (priv.value & 0x200) {
      // Same effect as a CSR RESET written by the EE.
      std::memset(g_RealGSMem, 0, sizeof(g_RealGSMem));
      GSIMR.reset();
      CSRreg.Reset();
      MTGS::ResetGS(false);
    }
    GSCSRr &= ~static_cast<u32>(priv.value & 0x1F);
    return true;
  }
  if (offset == kImr) {
    GSIMR._u32 = (static_cast<u32>(priv.value) & 0x1f00) | 0x6000;
    return true;
  }
  std::memcpy(PS2MEM_GS + offset, &priv.value, sizeof(priv.value));
  if (offset == kPmode || offset == kDispfb1 || offset == kDispfb2) s_registers_written = true;
  return true;
}

bool DoVsync(const Record &record) {
  proto::VsyncPayload vsync;
  if (record.payload_bytes < sizeof(vsync)) return false;
  std::memcpy(&vsync, record.payload, sizeof(vsync));
  s_display_field = (vsync.flags & proto::kVsyncFlagFieldValid) ? (vsync.field != 0) : !s_display_field;
  if (s_display_field)
    GSCSRr |= kCsrFieldBit;
  else
    GSCSRr &= ~kCsrFieldBit;
  MTGS::PostVsyncStart(s_registers_written);
  s_registers_written = false;
  ++s_frame;
  if (s_source != nullptr) {
    QueueSource *source = s_source;
    MTGS::RunOnGSThread([source]() { source->CompleteVsync(); });
  }
  VMManager::Internal::VSyncOnCPUThread();
  Host::PumpMessagesOnCPUThread();
  if (s_source != nullptr && (s_frame % kStatsEveryVsyncs) == 0) PostStats();
  return true;
}

bool DoReadFifo(const Record &record) {
  proto::ReadFifoPayload read;
  if (record.payload_bytes < sizeof(read)) return false;
  std::memcpy(&read, record.payload, sizeof(read));
  ++s_readbacks;
  const u64 bytes = static_cast<u64>(read.qwords) * 16;
  // One extra quadword: some transfers write past the requested size (as in the dump replayer).
  s_readback_buffer.resize(static_cast<size_t>(bytes) + 16);
  MTGS::InitAndReadFIFO(s_readback_buffer.data(), read.qwords);
  const u32 status = (EmuConfig.GS.HWDownloadMode == GSHardwareDownloadMode::Disabled && GSIsHardwareRenderer())
                         ? proto::kReadbackZeroFilled
                         : proto::kReadbackOk;
  if (s_source == nullptr) return true;
  u64 capacity = 0;
  u8 *response = s_source->ResponseArea(&capacity);
  if (response == nullptr || bytes > capacity) {
    PostError(proto::ErrorCode::bad_record, false,
              "READ_FIFO of " + std::to_string(read.qwords) + " qwords exceeds the response area");
    s_source->CompleteReadback(read.request_id, 0, proto::kReadbackFailed);
    return true;
  }
  std::memcpy(response, s_readback_buffer.data(), static_cast<size_t>(bytes));
  s_source->CompleteReadback(read.request_id, bytes, status);
  return true;
}

bool DoReset(const Record &record) {
  proto::ResetPayload reset;
  if (record.payload_bytes < sizeof(reset)) return false;
  std::memcpy(&reset, record.payload, sizeof(reset));
  if (reset.kind == static_cast<u32>(proto::ResetKind::gs_hard)) {
    HardReset();
    return true;
  }
  if (reset.kind == static_cast<u32>(proto::ResetKind::gif_paths)) {
    // Protocol bit N = path N; GSState::SoftReset bit 0 = PATH1, bit 1 = PATH2, bit 2 = PATH3.
    const u32 mask = (reset.path_mask >> 1) & 7;
    s_gif_parts.clear();
    for (u32 path = 0; path < 3; ++path) {
      if (mask & (1u << path)) s_transfer_scanners[path] = {};
    }
    MTGS::RunOnGSThread([mask]() { GSgifSoftReset(mask); });
    return true;
  }
  return false;
}

void DoSnapshot(u64 id, u32 width, u32 height) {
  u32 out_width = 0, out_height = 0;
  s_snapshot_pixels.clear();
  const bool aspect_correct = (GSConfig.ScreenshotSize != GSScreenshotSize::InternalResolutionUncorrected);
  if (!MTGS::SaveMemorySnapshot(width, height, aspect_correct, true, &out_width, &out_height, &s_snapshot_pixels)) {
    PostError(proto::ErrorCode::device_failure, false, "snapshot failed (no frame to read)");
    return;
  }
  const u64 bytes = static_cast<u64>(out_width) * out_height * 4;
  u64 capacity = 0;
  u8 *response = s_source != nullptr ? s_source->ResponseArea(&capacity) : nullptr;
  if (response == nullptr || bytes > capacity || s_snapshot_pixels.size() * 4 < bytes) {
    PostError(proto::ErrorCode::internal, false,
              "snapshot " + std::to_string(out_width) + "x" + std::to_string(out_height) + " does not fit");
    return;
  }
  std::memcpy(response, s_snapshot_pixels.data(), static_cast<size_t>(bytes));
  proto::SnapshotDonePayload done{id, out_width, out_height, 0, bytes};
  s_source->PostEvent(static_cast<u16>(proto::RecordType::snapshot_done), &done, sizeof(done));
}

void SetDisplayAspect(u64 mode) {
  s_display_aspect_mode = mode;
  MTGS::RunOnGSThread([fill = (mode == kDisplayAspectFill)]() {
    EmuConfig.CurrentAspectRatio = fill ? AspectRatioType::Stretch : GSConfig.AspectRatio;
  });
}

void DoDisplayAspect(const proto::ControlPayload &control) {
  const u64 mode = control.args[0];
  if (mode != kDisplayAspectGame && mode != kDisplayAspectFill) {
    PostError(static_cast<proto::ErrorCode>(kErrorCodeBadArgument), false,
              "CONTROL display_aspect mode " + std::to_string(mode) + " is not 0 or 1");
    return;
  }
  Console.WriteLn("(gt4gs) display_aspect %s from frame %llu", mode == kDisplayAspectFill ? "fill" : "game aspect",
                  static_cast<unsigned long long>(s_frame));
  SetDisplayAspect(mode);
}

bool DoControl(const Record &record) {
  proto::ControlPayload control;
  if (record.payload_bytes < sizeof(control)) return false;
  std::memcpy(&control, record.payload, sizeof(control));
  if (control.kind == 7) {  // protocol 1.3 display_aspect
    DoDisplayAspect(control);
    return true;
  }
  switch (static_cast<proto::ControlKind>(control.kind)) {
    case proto::ControlKind::shutdown:
      Console.WriteLn("(gt4gs) CONTROL shutdown");
      Finish(0, true);
      return true;

    case proto::ControlKind::attach_window: {
      const u32 width = static_cast<u32>(control.args[1]);
      const u32 height = static_cast<u32>(control.args[2]);
      Console.WriteLn("(gt4gs) CONTROL attach_window %llu %ux%u", static_cast<unsigned long long>(control.args[0]),
                      width, height);
      host::ApplyWindowSize(width, height);
      return true;
    }

    case proto::ControlKind::resize: {
      const u32 width = static_cast<u32>(control.args[0]);
      const u32 height = static_cast<u32>(control.args[1]);
      if (width == 0 || height == 0 || width > 16384 || height > 16384) {
        Console.WriteLn("(gt4gs) CONTROL resize %ux%u ignored", width, height);
        return true;
      }
      host::ApplyWindowSize(width, height);
      return true;
    }

    case proto::ControlKind::snapshot:
      DoSnapshot(control.args[0], static_cast<u32>(control.args[1]), static_cast<u32>(control.args[2]));
      return true;

    case proto::ControlKind::reload_config:
      PostError(proto::ErrorCode::unsupported_record, false, "CONTROL reload_config is not supported");
      return true;

    case proto::ControlKind::cursor:
      // No cursor over the picture on Android.
      return true;
  }
  PostError(proto::ErrorCode::unsupported_record, false, "unknown CONTROL kind " + std::to_string(control.kind));
  return true;
}

bool Process(const Record &record) {
  switch (static_cast<proto::RecordType>(record.type)) {
    case proto::RecordType::gif: return DoGif(record);
    case proto::RecordType::priv: return DoPriv(record);
    case proto::RecordType::vsync: return DoVsync(record);
    case proto::RecordType::read_fifo: return DoReadFifo(record);
    case proto::RecordType::reset: return DoReset(record);
    case proto::RecordType::control: return DoControl(record);
    case proto::RecordType::osd_stats:
      return record.payload_bytes >= sizeof(proto::OsdStatsPayload);
    case proto::RecordType::state_save:
    case proto::RecordType::state_load:
    case proto::RecordType::aux_frame_begin:
    case proto::RecordType::aux_frame_end:
      PostError(proto::ErrorCode::unsupported_record, false, "reserved record type");
      return true;
    default: return false;
  }
}

void Start() {
  s_started = true;
  HardReset();
  SetDisplayAspect(kDisplayAspectGame);
  Console.WriteLn("(gt4gs) stream started (renderer %s)", s_renderer_name.c_str());
}

void ExecuteRecords() {
  if (s_finished) return;
  if (!s_started) Start();
  s_running = true;
  Record record;
  while (s_running) {
    u64 wait_us = 0;
    const Source::NextResult result = s_source->Next(record, &wait_us);
    if (result == Source::NextResult::End) {
      Finish(0, false);
      break;
    }
    if (result == Source::NextResult::PeerExited) {
      Console.Warning("(gt4gs) the game went away; stopping");
      Finish(4, false);
      break;
    }
    if (result == Source::NextResult::Failed) {
      Console.Error("(gt4gs) stream error: %s", s_source->LastError());
      PostError(proto::ErrorCode::internal, true, s_source->LastError());
      Finish(5, true);
      break;
    }

    ++s_records;
    s_bytes += proto::record_size(record.payload_bytes);
    const bool ok = Process(record);
    const bool shutdown = s_finished;
    if (!shutdown) s_source->Release();
    if (!ok) {
      PostError(proto::ErrorCode::bad_record, true,
                "malformed record type " + std::to_string(record.type) + " (sequence " +
                    std::to_string(record.sequence) + ", " + std::to_string(record.payload_bytes) + " bytes)");
      Finish(3, true);
      break;
    }
    if (shutdown) break;
    if (VMManager::Internal::IsExecutionInterrupted()) break;
  }
  s_running = false;
}

// The CPU provider the VM runs: the whole "CPU" is the record loop above.
void CpuReserve() {}
void CpuShutdown() {}
void CpuReset() {}
void CpuStep() {}
void CpuExitExecution() { s_running = false; }
void CpuCancelInstruction() {}
void CpuClear(u32, u32) {}
void CpuExecute() { ExecuteRecords(); }

}  // namespace

void SetStream(QueueSource *source, const char *renderer_name, const char *version) {
  s_source = source;
  s_renderer_name = renderer_name != nullptr ? renderer_name : "";
  s_version = version != nullptr ? version : "";
  s_started = s_finished = s_running = false;
  s_exit_code = 1;
}

bool IsActive() { return s_source != nullptr; }

bool Initialize(Error *error) {
  if (s_source == nullptr) {
    Error::SetString(error, "gt4gs: no stream prepared");
    return false;
  }
  Console.WriteLn("(gt4gs) in-process stream, serial %s", s_source->Serial().c_str());
  Cpu = &Gt4gsStreamCpu;
  psxCpu = &psxInt;
  CpuVU0 = &CpuIntVU0;
  CpuVU1 = &CpuIntVU1;
  return true;
}

void Shutdown() {
  Console.WriteLn("(gt4gs) Shutting down.");
  Cpu = nullptr;
  psxCpu = nullptr;
  CpuVU0 = nullptr;
  CpuVU1 = nullptr;
}

std::string GetSerial() { return s_source != nullptr ? s_source->Serial() : std::string(); }

u64 GetFrameNumber() { return s_frame; }

int GetExitCode() { return s_exit_code; }

void ReportFatal(u32 code, const char *text) {
  if (s_source == nullptr) return;
  PostError(static_cast<proto::ErrorCode>(code), true, text != nullptr ? text : "fatal error");
  s_source->SetStateBits(proto::kStateExiting);
}

R5900cpu MakeStreamCpu() {
  static R5900cpu cpu = {CpuReserve, CpuShutdown, CpuReset, CpuStep, CpuExecute, CpuExitExecution, CpuCancelInstruction,
                         CpuClear};
  return cpu;
}

}  // namespace gt4gs

// Referenced by the VMManager hook (pcsx2/VMManager.cpp, patched for this private build). Global scope: the hook header
// declares it outside any namespace.
R5900cpu Gt4gsStreamCpu = gt4gs::MakeStreamCpu();

// SPDX-License-Identifier: GPL-3.0+
//
// GT4Recomp private Android test build (PCSX2-tainted, GPL). In-process replacement for the Windows GS stream
// transport (the shared-memory session of gt4-gs): the game's EE thread pushes the same records into a bounded queue
// and the GS thread (the ARMSX2 CPU provider) consumes them in order. No IPC, no mapping, no events: one mutex and
// condition variables.
//
// Record layouts are the ones of the game repository's MIT header (native/gs_stream/gs_stream_protocol.h), which this
// file includes: a record here is exactly what the Windows ring carries, so the replayer below is a port of the fork's
// and keeps the same meaning for every field.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "gs_stream/gs_stream_protocol.h"

namespace gt4gs {

// One record of the producer -> consumer direction, valid until Source::Release().
struct Record {
  std::uint16_t type = 0;
  std::uint16_t flags = 0;
  std::uint32_t payload_bytes = 0;
  std::uint64_t sequence = 0;
  const std::uint8_t *payload = nullptr;
};

// What the replayer consumes. The live Windows source has more members (peer process, event ring); the in-process one
// keeps the same shape so that the record handling stays a straight port.
class Source {
 public:
  enum class NextResult { Record, End, PeerExited, Failed };

  virtual ~Source() = default;
  virtual bool IsLive() const = 0;
  virtual std::string Serial() const = 0;
  virtual NextResult Next(Record &record, std::uint64_t *wait_us) = 0;
  virtual void Release() = 0;
  virtual const char *LastError() const = 0;

  virtual void CompleteVsync() {}
  virtual std::uint8_t *ResponseArea(std::uint64_t *capacity) {
    *capacity = 0;
    return nullptr;
  }
  virtual void CompleteReadback(std::uint64_t request_id, std::uint64_t bytes, std::uint32_t status) {}
  virtual void PostEvent(std::uint16_t type, const void *payload, std::uint32_t bytes) {}
  virtual void SetStateBits(std::uint32_t bits) {}
};

// The queue between the game thread (producer) and the GS thread (consumer).
//
// Back-pressure follows the Windows stream: at most `kFramesInFlight` VSYNCs may be outstanding (the game waits after
// posting one until the GS thread completed an older one), and the queue itself is capped at 24 MiB of records. A
// producer that finds it full waits; nothing is dropped or reordered.
class QueueSource final : public Source {
 public:
  static constexpr std::uint32_t kFramesInFlight = 2;
  static constexpr std::size_t kMaxQueuedBytes = 24u * 1024u * 1024u;
  static constexpr std::size_t kResponseCapacity = 4u * 1024u * 1024u + 64u * 1024u;
  static constexpr std::uint32_t kReadbackTimeoutMs = 5000;

  explicit QueueSource(std::string serial);
  ~QueueSource() override;

  QueueSource(const QueueSource &) = delete;
  QueueSource &operator=(const QueueSource &) = delete;

  // --- producer side (the game's EE thread) ---
  void PushGif(int path, const void *data, std::uint32_t bytes);
  void PushPrivileged(std::uint32_t address, std::uint64_t value);
  void PushVsync(bool odd_field);  // waits for frames in flight, as the Windows transport does
  bool PushReadFifo(std::uint8_t *data, std::uint32_t qwords);  // blocks for the reply; false on timeout
  void PushReset(std::uint32_t kind, std::uint32_t path_mask);
  void PushControl(const gs_stream::ControlPayload &control);
  void PushShutdown();
  void Abort(const std::string &why);  // the producer is going away: the consumer stops with a failure

  // --- consumer side (the GS thread) ---
  bool IsLive() const override { return true; }
  std::string Serial() const override { return serial_; }
  NextResult Next(Record &record, std::uint64_t *wait_us) override;
  void Release() override;
  const char *LastError() const override { return error_.c_str(); }
  void CompleteVsync() override;
  std::uint8_t *ResponseArea(std::uint64_t *capacity) override;
  void CompleteReadback(std::uint64_t request_id, std::uint64_t bytes, std::uint32_t status) override;
  void PostEvent(std::uint16_t type, const void *payload, std::uint32_t bytes) override;
  void SetStateBits(std::uint32_t bits) override { state_bits_.store(bits, std::memory_order_relaxed); }

  std::uint32_t state_bits() const { return state_bits_.load(std::memory_order_relaxed); }

 private:
  struct Job {
    std::uint16_t type = 0;
    std::uint16_t flags = 0;
    std::uint32_t payload_bytes = 0;
    std::uint64_t sequence = 0;
    std::vector<std::uint8_t> payload;
  };

  bool Push(std::uint16_t type, std::uint16_t flags, const void *payload, std::uint32_t bytes);

  std::string serial_;
  std::string error_;

  mutable std::mutex mutex_;
  std::condition_variable data_ready_;
  std::condition_variable space_free_;
  std::condition_variable vsync_done_;
  std::condition_variable readback_done_;

  std::deque<Job> queue_;
  std::size_t queued_bytes_ = 0;
  std::uint64_t sequence_ = 0;
  bool end_of_stream_ = false;
  bool exited_ = false;
  Job current_;  // the record handed out by Next(), valid until Release()

  std::uint64_t vsyncs_posted_ = 0;
  std::uint64_t vsyncs_done_ = 0;

  std::vector<std::uint8_t> response_;
  std::uint64_t pending_readback_ = 0;
  std::uint64_t completed_readback_ = 0;
  std::uint64_t readback_bytes_ = 0;
  std::uint32_t readback_status_ = gs_stream::kReadbackFailed;

  std::atomic<std::uint32_t> state_bits_{0};
};

}  // namespace gt4gs

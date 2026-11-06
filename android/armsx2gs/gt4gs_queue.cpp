// SPDX-License-Identifier: GPL-3.0+
//
// GT4Recomp private Android test build (PCSX2-tainted, GPL). The in-process transport (queue_source.h).

#include "gt4gs_queue.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace gt4gs {

using gs_stream::ControlKind;
using gs_stream::ControlPayload;
using gs_stream::GifPayload;
using gs_stream::PrivPayload;
using gs_stream::ReadFifoPayload;
using gs_stream::RecordType;
using gs_stream::ResetKind;
using gs_stream::ResetPayload;
using gs_stream::VsyncPayload;

QueueSource::QueueSource(std::string serial) : serial_(std::move(serial)), response_(kResponseCapacity) {}

QueueSource::~QueueSource() = default;

bool QueueSource::Push(std::uint16_t type, std::uint16_t flags, const void *payload, std::uint32_t bytes) {
  Job job;
  job.type = type;
  job.flags = flags;
  job.payload_bytes = bytes;
  if (bytes != 0) {
    const auto *first = static_cast<const std::uint8_t *>(payload);
    job.payload.assign(first, first + bytes);
  }
  {
    std::unique_lock lock(mutex_);
    space_free_.wait(lock, [&] { return queued_bytes_ < kMaxQueuedBytes || end_of_stream_ || exited_; });
    if (end_of_stream_ || exited_) return false;
    job.sequence = ++sequence_;
    queued_bytes_ += job.payload_bytes;
    queue_.push_back(std::move(job));
  }
  data_ready_.notify_one();
  return true;
}

void QueueSource::PushGif(int path, const void *data, std::uint32_t bytes) {
  if (bytes == 0) return;
  // Protocol 6.1: GifPayload followed by the GIF bytes. The game already applied SIGNAL/FINISH/LABEL to its own copy,
  // as the Windows producer does, so flags stays 0.
  std::vector<std::uint8_t> payload(sizeof(GifPayload) + bytes);
  GifPayload header{};
  header.path = static_cast<std::uint8_t>(std::clamp(path, 1, 3));
  header.flags = 0;
  header.bytes = bytes;
  std::memcpy(payload.data(), &header, sizeof(header));
  std::memcpy(payload.data() + sizeof(header), data, bytes);
  Push(static_cast<std::uint16_t>(RecordType::gif), 0, payload.data(), static_cast<std::uint32_t>(payload.size()));
}

void QueueSource::PushPrivileged(std::uint32_t address, std::uint64_t value) {
  PrivPayload priv{};
  priv.address = address;
  priv.value = value;
  Push(static_cast<std::uint16_t>(RecordType::priv), 0, &priv, sizeof(priv));
}

void QueueSource::PushVsync(bool odd_field) {
  std::uint64_t posted = 0;
  {
    std::unique_lock lock(mutex_);
    VsyncPayload vsync{};
    vsync.frame = vsyncs_posted_;
    vsync.field = odd_field ? 1u : 0u;
    vsync.flags = gs_stream::kVsyncFlagFieldValid;
    Job job;
    job.type = static_cast<std::uint16_t>(RecordType::vsync);
    job.payload.resize(sizeof(vsync));
    std::memcpy(job.payload.data(), &vsync, sizeof(vsync));
    job.payload_bytes = static_cast<std::uint32_t>(sizeof(vsync));
    job.sequence = ++sequence_;
    queued_bytes_ += job.payload_bytes;
    queue_.push_back(std::move(job));
    posted = ++vsyncs_posted_;
  }
  data_ready_.notify_one();

  // Protocol 6.3: after posting VSYNC the producer waits while too many frames are in flight. It is the game's pacing
  // clock: the GS thread bumps vsyncs_done once the frame was handed to presentation.
  std::unique_lock lock(mutex_);
  vsync_done_.wait(lock, [&] {
    return exited_ || end_of_stream_ || posted - vsyncs_done_ <= kFramesInFlight;
  });
}

bool QueueSource::PushReadFifo(std::uint8_t *data, std::uint32_t qwords) {
  const std::uint64_t bytes = static_cast<std::uint64_t>(qwords) * 16u;
  std::uint64_t request = 0;
  {
    std::lock_guard lock(mutex_);
    request = ++pending_readback_;
  }
  ReadFifoPayload read{};
  read.qwords = qwords;
  read.request_id = request;
  if (!Push(static_cast<std::uint16_t>(RecordType::read_fifo), 0, &read, sizeof(read)) || bytes == 0) {
    if (data != nullptr && bytes != 0) std::memset(data, 0, static_cast<std::size_t>(bytes));
    return false;
  }

  std::unique_lock lock(mutex_);
  const bool answered = readback_done_.wait_for(lock, std::chrono::milliseconds(kReadbackTimeoutMs),
                                                [&] { return exited_ || completed_readback_ >= request; });
  const bool ok = answered && completed_readback_ >= request && readback_status_ != gs_stream::kReadbackFailed;
  if (!ok) {
    std::fprintf(stderr, "[gt4gs] READ_FIFO %llu qwords unanswered after %u ms; zeros returned\n",
                 static_cast<unsigned long long>(qwords), kReadbackTimeoutMs);
    if (data != nullptr) std::memset(data, 0, static_cast<std::size_t>(bytes));
    return false;
  }
  const std::size_t copied = static_cast<std::size_t>(std::min<std::uint64_t>(bytes, readback_bytes_));
  if (data != nullptr) {
    std::memcpy(data, response_.data(), copied);
    if (copied < bytes) std::memset(data + copied, 0, static_cast<std::size_t>(bytes - copied));
  }
  return true;
}

std::uint64_t QueueSource::PushReadFifoNoWait(std::uint32_t qwords) {
  if (qwords == 0) return 0;
  std::uint64_t request = 0;
  {
    std::lock_guard lock(mutex_);
    request = ++pending_readback_;
  }
  ReadFifoPayload read{};
  read.qwords = qwords;
  read.request_id = request;
  if (!Push(static_cast<std::uint16_t>(RecordType::read_fifo), 0, &read, sizeof(read))) return 0;
  return request;
}

void QueueSource::PushReset(std::uint32_t kind, std::uint32_t path_mask) {
  ResetPayload reset{};
  reset.kind = kind;
  reset.path_mask = path_mask;
  Push(static_cast<std::uint16_t>(RecordType::reset), 0, &reset, sizeof(reset));
}

void QueueSource::PushControl(const ControlPayload &control) {
  Push(static_cast<std::uint16_t>(RecordType::control), 0, &control, sizeof(control));
}

void QueueSource::PushShutdown() {
  ControlPayload control{};
  control.kind = static_cast<std::uint32_t>(ControlKind::shutdown);
  Push(static_cast<std::uint16_t>(RecordType::control), 0, &control, sizeof(control));
  {
    std::lock_guard lock(mutex_);
    end_of_stream_ = true;
  }
  data_ready_.notify_all();
  space_free_.notify_all();
  vsync_done_.notify_all();
  readback_done_.notify_all();
}

void QueueSource::Abort(const std::string &why) {
  {
    std::lock_guard lock(mutex_);
    error_ = why;
    exited_ = true;
  }
  data_ready_.notify_all();
  space_free_.notify_all();
  vsync_done_.notify_all();
  readback_done_.notify_all();
}

Source::NextResult QueueSource::Next(Record &record, std::uint64_t *wait_us) {
  if (wait_us != nullptr) *wait_us = 0;
  std::unique_lock lock(mutex_);
  data_ready_.wait(lock, [&] { return !queue_.empty() || exited_ || (end_of_stream_ && queue_.empty()); });
  if (queue_.empty()) {
    if (!error_.empty()) return NextResult::Failed;
    return NextResult::End;
  }
  current_ = std::move(queue_.front());
  queue_.pop_front();
  queued_bytes_ -= current_.payload_bytes;
  record.type = current_.type;
  record.flags = current_.flags;
  record.payload_bytes = current_.payload_bytes;
  record.sequence = current_.sequence;
  record.payload = current_.payload.data();
  return NextResult::Record;
}

void QueueSource::Release() {
  {
    std::lock_guard lock(mutex_);
    current_ = {};
  }
  space_free_.notify_all();
}

void QueueSource::CompleteVsync() {
  {
    std::lock_guard lock(mutex_);
    ++vsyncs_done_;
  }
  vsync_done_.notify_all();
}

std::uint8_t *QueueSource::ResponseArea(std::uint64_t *capacity) {
  if (capacity != nullptr) *capacity = response_.size();
  return response_.data();
}

void QueueSource::CompleteReadback(std::uint64_t request_id, std::uint64_t bytes, std::uint32_t status) {
  {
    std::lock_guard lock(mutex_);
    readback_bytes_ = std::min<std::uint64_t>(bytes, response_.size());
    readback_status_ = status;
    completed_readback_ = std::max(completed_readback_, request_id);
  }
  readback_done_.notify_all();
}

void QueueSource::PostEvent(std::uint16_t type, const void *payload, std::uint32_t bytes) {
  // The Windows consumer's events go back to the game process (READY, STATS, ERROR, ...). In process there is nobody
  // to hand them to: they are logged so that game.log keeps the same information the Windows session log has.
  const auto record_type = static_cast<gs_stream::RecordType>(type);
  if (record_type == gs_stream::RecordType::error && payload != nullptr &&
      bytes >= sizeof(gs_stream::ErrorPayload)) {
    const auto *error = static_cast<const gs_stream::ErrorPayload *>(payload);
    std::fprintf(stderr, "[gt4gs] ERROR %u%s: %s\n", error->code, error->fatal ? " (fatal)" : "", error->text);
    return;
  }
  std::fprintf(stderr, "[gt4gs] event %u (%u bytes)\n", type, bytes);
}

}  // namespace gt4gs

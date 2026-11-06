// TASK-011 step 2: VU1 worker thread runtime (see mtvu_runtime.hpp and docs/MTVU-PLAN.md section 11).
#include "thread_placement.hpp"
#include "mtvu_runtime.hpp"

#include "mtvu_journal.hpp"
#include "mtvu_measure.hpp"
#include "mtvu_ring.hpp"
#include "mtvu_vif_batch.hpp"
#include "race_display.hpp"
#include "race_widescreen.hpp"
#include "display_aspect.hpp"
#include "flare_limits.hpp"
#include "fov_adjust.hpp"
#include "perf_clock.hpp"
#include "perf_probe.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <string>
#include <thread>
#include <vector>

namespace ps2 {
extern std::uint8_t g_ram[]; // ps2/memory.hpp (guest RAM, 32 MB)
}  // namespace ps2

namespace gt4::mtvu {
namespace {

// Ring sizes from docs/MTVU-PLAN.md section 5.1.
constexpr std::size_t kInputBytes = 32u << 20;
constexpr std::size_t kOutputBytes = 8u << 20;
constexpr std::uint64_t kFnvOffset = 0xCBF29CE484222325ull;
constexpr std::uint64_t kFnvPrime = 0x100000001B3ull;

thread_local bool t_on_worker = false;

// Executes journal GS operations on the EE thread through the donor renderer.
class DonorSink final : public GsSink {
 public:
  DonorSink(void (*render)(int, const std::uint8_t *, std::uint32_t), bool (*post_vsync)())
      : render_(render), post_vsync_(post_vsync) {}
  void set_privileged_consumer(void (*post)(std::uint32_t, std::uint64_t)) { privileged_ = post; }
  void gif_transfer(std::uint8_t path, const std::uint8_t *data, std::size_t bytes) override {
    gt4::perf::Scope drain(gt4::perf::Category::gs_drain);
    render_(path, data, static_cast<std::uint32_t>(bytes / 16));
  }
  // The runtime synchronises GIF, VBlank and privileged operations instead of journaling them (section 5.3).
  void gif_reset() override { throw std::logic_error("mtvu journal GIF reset is not used"); }
  void vertex_reset() override { throw std::logic_error("mtvu journal vertex reset is not used"); }
  // TASK-084 step 3: the VBlank entry runs after the frame's output in stream order; it posts the held VSYNC and then
  // runs the F8 boundary / verify work that used to happen inside the blocking drain.
  void vblank_flush() override;
  // TASK-041: a queued privileged write reaches its ordered consumer (the GS stream) after the earlier GIF output.
  void privileged_write(std::uint32_t address, std::uint64_t value) override {
    if (!privileged_) throw std::logic_error("mtvu journal privileged write without an ordered consumer");
    privileged_(address, value);
  }
  // TASK-116 step 1: the queued READ_FIFO reaches the GS stream after the earlier GIF output, without waiting.
  void readback(std::uint32_t qwords) override {
    if (!post_readback_) throw std::logic_error("mtvu journal readback without an ordered consumer");
    post_readback_(qwords);
  }
  void set_readback_consumer(void (*post)(std::uint32_t)) { post_readback_ = post; }
  // IN-461: direct GS intake; the GS consumer reads the job's records from the output ring itself.
  void direct_job(std::uint64_t job) override {
    if (!post_gs_job_) throw std::logic_error("mtvu direct GS intake without a consumer");
    post_gs_job_(job, *output_, *output_bell_);
  }
  void set_direct_consumer(void (*post)(std::uint64_t, SpscByteRing &, Doorbell &), SpscByteRing &output,
                           Doorbell &output_bell) {
    post_gs_job_ = post;
    output_ = &output;
    output_bell_ = &output_bell;
  }

 private:
  void (*render_)(int, const std::uint8_t *, std::uint32_t);
  void (*privileged_)(std::uint32_t, std::uint64_t) = nullptr;
  bool (*post_vsync_)() = nullptr;
  void (*post_readback_)(std::uint32_t) = nullptr;
  void (*post_gs_job_)(std::uint64_t, SpscByteRing &, Doorbell &) = nullptr;
  SpscByteRing *output_ = nullptr;
  Doorbell *output_bell_ = nullptr;
};

// Cumulative worker totals, published by the worker after each job (before its JobDone). The EE reads them only
// after a blocking drain, when the worker is idle, so the release/acquire of the ring orders the reads.
struct WorkerTotals {
  std::uint64_t idle_us = 0, vu1_us = 0, output_us = 0, jobs = 0;
  // TASK-084 step 0: the VIF1 job shape and both doorbells (EE-side counters read on the EE thread).
  std::uint64_t vif_chains = 0, vif_jobs = 0, vif_job_bytes = 0, vif_packets = 0;
  // TASK-084 step 0: worker GIF records the EE forwarded to the GS stream (the copy accounting).
  std::uint64_t executed_records = 0, executed_bytes = 0;
  Doorbell::Stats input_bell{}, output_bell{};
};

std::uint64_t wall_us() { return gt4::perf::scope_wall_us(); }

void after_vblank_output();  // defined below; also called from the journal drain (TASK-084 step 3)

// Largest VIF1 batch payload: below the ring's max_payload/2 so one job never needs splitting and the ring keeps room
// for the frame's other records. A longer chain is posted as several jobs, still far below one per packet.
constexpr std::size_t kVifBatchBytes = 512u << 10;

struct Runtime {
  Settings settings;
  Donor donor{};
  bool started = false;
  std::atomic<bool> running{false};  // worker alive and GT4_MTVU on: the EE shadows VIF1

  // TASK-084 step 1: one doorbell per direction, so a publish only wakes the peer when the peer sleeps.
  Doorbell input_bell;   // EE -> worker
  Doorbell output_bell;  // worker -> EE
  std::unique_ptr<SpscByteRing> input;
  std::unique_ptr<SpscByteRing> output;
  std::unique_ptr<DonorSink> sink;
  std::unique_ptr<EeJournal> journal;     // EE thread only
  std::unique_ptr<WorkerEnd> worker_end;  // worker thread only
  std::thread worker;

  // TASK-084 step 1: the pending VIF1 batch (EE thread only); walk_vif_batch replays it on the worker.
  VifBatch vif_batch;

  // IN-522: the table the GS worker will write (EE thread only), the parts of the pending batch copied from it, and the parts
  // of posted jobs, for the worker. Empty nearly always, and `job_patches_posted` keeps the worker off the lock then.
  struct TablePatch {
    std::uint64_t job = 0, ticket = 0;
    std::size_t payload_offset = 0, table_offset = 0, bytes = 0;
  };
  const std::uint8_t *table_dest = nullptr;
  std::size_t table_bytes = 0;
  std::uint64_t table_ticket = 0;
  std::vector<TablePatch> batch_patches;
  std::mutex job_patches_mutex;
  std::deque<TablePatch> job_patches;
  std::atomic<std::uint64_t> job_patches_posted{0};
  std::uint64_t job_patches_applied = 0;  // worker thread only
  DeferredTableCopy table_copy = nullptr;
  // TASK-178 diagnostics: per read with patches, its first job, packed (ticket << 32 | job, low 32 bits of each) in slot
  // ticket % 16, stored before that job enters the GS stream; written by the EE only. The EE runs up to two VBlanks ahead of
  // the GS worker, so the slots keep the last 16 reads.
  std::array<std::atomic<std::uint64_t>, 16> first_patched_job{};
  std::uint64_t first_patched_ticket = 0;  // EE thread only

  std::atomic<bool> worker_failed{false};
  std::exception_ptr worker_error;  // written by the worker before worker_failed is released
  std::uint64_t worker_job = 0;     // worker thread only
  GifScanner worker_scanners[4];    // worker thread only: PATH1 invariant check
  GifScanner ee_scanners[4];        // EE thread only: signal detection before journaling
  std::vector<SignalWrite> signal_scratch;  // EE thread only: signals of the packet being accepted

  std::uint64_t next_job = 1;  // EE thread only
  // IN-461 (CPU survey T3): direct GS intake. The GS worker reads the output ring, so the EE never sees a JobDone; the
  // worker stores the job it finished here before its JobDone, and the EE waits on it where it needs the worker idle.
  bool direct = false;  // set at start
  std::atomic<std::uint64_t> completed_job{0};
  // TASK-084 step 3: a deferred VSYNC is queued behind the frame's worker output; never more than one at a time.
  bool pending_vblank = false;
  std::uint64_t vblank_posts = 0;
  // TASK-084 step 0: the job shape and the ring traffic, per interval (EE thread only; the worker's output counts are
  // published separately).
  std::uint64_t vif_chains = 0, vif_job_count = 0, vif_job_bytes = 0, vif_packet_count = 0;
  // TASK-041: ordered privileged writes (EE thread only).
  void (*ordered_privileged)(std::uint32_t, std::uint64_t) = nullptr;
  // TASK-116 step 1: the deferred download sync and its READ_FIFO entry (EE thread only).
  DownloadOrdering download{};
  bool download_deferred = false;  // the last sync("download") returned without draining
  std::uint64_t readbacks_queued = 0;
  std::uint64_t privileged_queued = 0;   // writes queued behind journal entries
  std::uint64_t privileged_wait_us = 0;  // wait of the last blocking gs_privileged sync, taken by the consumer
  std::uint64_t signals_applied = 0;  // EE thread only: SIGNAL/FINISH/LABEL applied at accept
  std::uint64_t vif_writes = 0, ee_transfers = 0, direct_transfers = 0, gs_renders = 0;
  std::map<std::string, std::uint64_t> syncs;
  std::atomic<std::uint64_t> path1_signals{0};
  std::uint64_t verify_frames = 0, verify_mismatches = 0, report_frames = 0;
  std::uint64_t frame_hash = kFnvOffset;
  bool path1_reported = false;

  // TASK-011 step 3 (slice171) per-thread accounting. The worker owns worker_accounting; the EE reads only
  // `published` (atomics) and `baseline` (EE only).
  gt4::perf::Accounting worker_accounting{false};
  std::atomic<std::uint64_t> published_idle_us{0}, published_vu1_us{0}, published_output_us{0}, published_jobs{0};
  std::uint64_t worker_idle_us = 0, worker_jobs = 0;  // worker thread only
  WorkerTotals baseline;                              // EE thread only: totals at the last interval reset
  std::uint64_t baseline_worker_cpu_us = 0;           // EE thread only: the worker's CPU time at the last interval reset
};

Runtime &rt() {
  static Runtime runtime;
  return runtime;
}

bool parse_flag(const char *name, const char *value, bool fallback) {
  if (value == nullptr) return fallback;
  if (std::strcmp(value, "0") == 0) return false;
  if (std::strcmp(value, "1") == 0) return true;
  throw std::invalid_argument(std::string(name) + " must be unset, 0 or 1");
}

void fnv_bytes(std::uint64_t &hash, const void *data, std::size_t bytes) {
  const auto *at = static_cast<const std::uint8_t *>(data);
  for (std::size_t i = 0; i < bytes; ++i) {
    hash ^= at[i];
    hash *= kFnvPrime;
  }
}

void rethrow_worker_error() {
  Runtime &r = rt();
  if (r.worker_failed.load(std::memory_order_acquire)) std::rethrow_exception(r.worker_error);
}

// IN-522, on the worker: the parts of this job's payload copied from a table the GS worker was going to write.
void apply_table_patches(Runtime &r, std::uint64_t job, std::vector<std::uint8_t> &payload) {
  if (r.job_patches_posted.load(std::memory_order_acquire) == r.job_patches_applied) return;
  for (;;) {
    Runtime::TablePatch patch;
    {
      const std::lock_guard<std::mutex> lock(r.job_patches_mutex);
      if (r.job_patches.empty() || r.job_patches.front().job != job) return;
      patch = r.job_patches.front();
      r.job_patches.pop_front();
    }
    r.worker_end->wake_before_waiting();
    r.table_copy(patch.ticket, patch.table_offset, patch.bytes, payload.data() + patch.payload_offset);
    ++r.job_patches_applied;
  }
}

void run_job(Runtime &r, RecordKind kind, const std::vector<std::uint8_t> &payload) {
  switch (kind) {
    case RecordKind::VifPacket:
      r.donor.worker_vif_packet(payload.data(), static_cast<std::uint32_t>(payload.size() / 16));
      break;
    case RecordKind::VifBatch: {
      // TASK-084 step 1: replay the chain's packets in order through the single-packet entry point, so the worker runs
      // exactly the same VIF1 command sequence as before; only the job boundary (and the wake-ups) changed.
      walk_vif_batch(payload.data(), payload.size(),
                     [&r](const std::uint8_t *data, std::uint32_t qwc) { r.donor.worker_vif_packet(data, qwc); });
      break;
    }
    case RecordKind::VifWrite: {
      if (payload.size() != 8) throw std::logic_error("mtvu VIF1 write record must carry 8 bytes");
      std::uint32_t reg = 0, value = 0;
      std::memcpy(&reg, payload.data(), 4);
      std::memcpy(&value, payload.data() + 4, 4);
      r.donor.worker_vif_write(reg, value);
      break;
    }
    case RecordKind::VuReset:
      r.donor.worker_vu_reset();
      break;
    default:
      throw std::logic_error("mtvu worker received an unexpected job kind");
  }
}

// Publishes the worker's cumulative totals. Category times come from the worker's own accounting (its scopes are
// closed at this point, so the interval is current).
void publish_worker_totals(Runtime &r) {
  const auto &categories = r.worker_accounting.interval().categories;
  std::uint64_t vu1 = categories[static_cast<std::size_t>(gt4::perf::Category::vu1_interpret)].exclusive_us;
#ifdef GT4_VU1_AOT
  vu1 += categories[static_cast<std::size_t>(gt4::perf::Category::vu1_aot)].exclusive_us;
#endif
  r.published_vu1_us.store(vu1, std::memory_order_relaxed);
  r.published_output_us.store(categories[static_cast<std::size_t>(gt4::perf::Category::mtvu_output)].exclusive_us,
                              std::memory_order_relaxed);
  r.published_idle_us.store(r.worker_idle_us, std::memory_order_relaxed);
  r.published_jobs.store(r.worker_jobs, std::memory_order_relaxed);
}

void worker_main() {
  thread_placement::apply("vu1");
  t_on_worker = true;
  Runtime &r = rt();
  gt4::perf::bind_worker(&r.worker_accounting, wall_us);
  RecordHeader header;
  std::vector<std::uint8_t> payload;
  for (;;) {
    // TASK-134: no clock read without the performance report, as in gt4::perf::Scope.
    const bool timed = r.worker_accounting.enabled();
    const std::uint64_t idle_start = timed ? wall_us() : 0;
    r.worker_end->receive(header, payload);
    if (timed) r.worker_idle_us += wall_us() - idle_start;
    const auto kind = static_cast<RecordKind>(header.kind);
    if (kind == RecordKind::Stop) return;
    r.worker_job = header.job;
    // After a failure the worker only acknowledges jobs, so the EE never waits forever; it rethrows at the next sync.
    if (!r.worker_failed.load(std::memory_order_acquire)) {
      try {
        apply_table_patches(r, header.job, payload);
        run_job(r, kind, payload);
      } catch (...) {
        r.worker_error = std::current_exception();
        r.worker_failed.store(true, std::memory_order_release);
      }
    }
    ++r.worker_jobs;
    publish_worker_totals(r);
    // IN-461: before the JobDone, so the EE that the JobDone (or the idle wake behind it) wakes sees the job done.
    r.completed_job.store(header.job, std::memory_order_release);
    r.worker_end->emit_job_done(header.job);
  }
}

}  // namespace

Settings parse_settings(const char *mtvu, const char *verify, const char *strict, const char *report_every,
                        const char *privileged_async, const char *chain_jobs, const char *gif_fast_path,
                        const char *vblank_journal) {
  Settings settings;
  settings.enabled = parse_flag("GT4_MTVU", mtvu, true);
  settings.verify = parse_flag("GT4_MTVU_VERIFY", verify, false);
  settings.strict = parse_flag("GT4_MTVU_STRICT", strict, false);
  settings.privileged_async = parse_flag("GT4_MTVU_PRIV_ASYNC", privileged_async, Settings::kPrivilegedAsyncDefault);
  settings.chain_jobs = parse_flag("GT4_MTVU_CHAIN", chain_jobs, true);
  settings.gif_fast_path = parse_flag("GT4_GIF_FASTPATH", gif_fast_path, true);
  settings.vblank_journal = parse_flag("GT4_MTVU_VBLANK_JOURNAL", vblank_journal, true);
  if (report_every != nullptr) {
    const std::string_view digits(report_every);
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string_view::npos)
      throw std::invalid_argument("GT4_MTVU_REPORT_EVERY must be a VBlank count");
    settings.report_every = std::stoull(report_every);
  }
  return settings;
}

Settings settings_from_environment() {
  return parse_settings(std::getenv("GT4_MTVU"), std::getenv("GT4_MTVU_VERIFY"), std::getenv("GT4_MTVU_STRICT"),
                        std::getenv("GT4_MTVU_REPORT_EVERY"), std::getenv("GT4_MTVU_PRIV_ASYNC"),
                        std::getenv("GT4_MTVU_CHAIN"), std::getenv("GT4_GIF_FASTPATH"),
                        std::getenv("GT4_MTVU_VBLANK_JOURNAL"));
}

WorkerTotals published_totals(const Runtime &r) {
  WorkerTotals totals;
  totals.idle_us = r.published_idle_us.load(std::memory_order_relaxed);
  totals.vu1_us = r.published_vu1_us.load(std::memory_order_relaxed);
  totals.output_us = r.published_output_us.load(std::memory_order_relaxed);
  totals.jobs = r.published_jobs.load(std::memory_order_relaxed);
  totals.vif_chains = r.vif_chains;
  totals.vif_jobs = r.vif_job_count;
  totals.vif_job_bytes = r.vif_job_bytes;
  totals.vif_packets = r.vif_packet_count;
  totals.input_bell = r.input_bell.stats();
  totals.output_bell = r.output_bell.stats();
  if (r.journal) {
    totals.executed_records = r.journal->outputs_executed();
    totals.executed_bytes = r.journal->executed_bytes();
  }
  return totals;
}

std::uint64_t worker_thread_cpu_us(Runtime &r) {
#ifdef _WIN32
  FILETIME created{}, exited{}, kernel{}, user{};
  if (r.worker.joinable() && GetThreadTimes(r.worker.native_handle(), &created, &exited, &kernel, &user)) {
    const auto ticks = [](FILETIME value) { return (std::uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime; };
    return (ticks(kernel) + ticks(user)) / 10;
  }
#else
  (void)r;
#endif
  return 0;
}

// Installed as the probe's [mtvu] section (EE thread, at each report). The EE categories come from the probe's own
// interval; the worker totals are the difference since the last interval reset. Times are ms per VBlank.
void report_interval(std::FILE *file, const gt4::perf::IntervalTime &ee, std::uint64_t frames) {
  if (!frames) return;
  Runtime &r = rt();
  const WorkerTotals now = published_totals(r);
  const double scale = static_cast<double>(frames) * 1000.0;
  const auto ee_ms = [&](gt4::perf::Category category) {
    return ee.categories[static_cast<std::size_t>(category)].exclusive_us / scale;
  };
  const double wait_vblank = ee_ms(gt4::perf::Category::mtvu_wait_vblank);
  const double wait_download = ee_ms(gt4::perf::Category::mtvu_wait_download);
  const double wait_privileged = ee_ms(gt4::perf::Category::mtvu_wait_gs_privileged);
  const double wait_vu_memory = ee_ms(gt4::perf::Category::mtvu_wait_vu_memory);
  const double wait_gif_reset = ee_ms(gt4::perf::Category::mtvu_wait_gif_reset);
  const double wait_reset = ee_ms(gt4::perf::Category::mtvu_wait_reset);
  const double wait_state = ee_ms(gt4::perf::Category::mtvu_wait_state);
  const double wait_other = ee_ms(gt4::perf::Category::mtvu_wait_other);
  const double wait_total = wait_vblank + wait_download + wait_privileged + wait_vu_memory + wait_gif_reset +
                            wait_reset + wait_state + wait_other;
  const double vu1 = (now.vu1_us - r.baseline.vu1_us) / scale;
  const double output = (now.output_us - r.baseline.output_us) / scale;
  const double idle = (now.idle_us - r.baseline.idle_us) / scale;
  const double busy = vu1 + output;
  const double overlap = busy > wait_total ? busy - wait_total : 0.0;
  std::fprintf(file, "[mtvu] EE wait ms/frame: vblank=%.3f download=%.3f gs_privileged=%.3f vu_memory=%.3f "
               "gif_reset=%.3f reset=%.3f state=%.3f other=%.3f total=%.3f\n",
               wait_vblank, wait_download, wait_privileged, wait_vu_memory, wait_gif_reset, wait_reset, wait_state,
               wait_other, wait_total);
  std::fprintf(file, "[mtvu] EE gs_drain ms/frame=%.3f (journal GS front end on the EE thread)\n",
               ee_ms(gt4::perf::Category::gs_drain));
  std::fprintf(file, "[mtvu] worker ms/frame: vu1_program=%.3f output_path12=%.3f busy=%.3f idle=%.3f jobs/frame=%.1f\n",
               vu1, output, busy, idle, (now.jobs - r.baseline.jobs) / static_cast<double>(frames));
  // TASK-134: the worker's real CPU, since busy above is wall time of its scopes.
  std::fprintf(file, "[mtvu] worker thread cpu ms/frame=%.3f\n",
               (worker_thread_cpu_us(r) - r.baseline_worker_cpu_us) / scale);
  std::fprintf(file, "[mtvu] overlap achieved ms/frame=%.3f (worker busy %.3f minus EE wait for the worker %.3f)\n",
               overlap, busy, wait_total);
  // TASK-084 step 0: the job shape and the doorbell traffic, so the cost of one job per packet (or per chain) and of
  // the wake-ups is visible per interval.
  const double chains = static_cast<double>(now.vif_chains - r.baseline.vif_chains);
  const double jobs = static_cast<double>(now.vif_jobs - r.baseline.vif_jobs);
  const double packets = static_cast<double>(now.vif_packets - r.baseline.vif_packets);
  const double job_bytes = static_cast<double>(now.vif_job_bytes - r.baseline.vif_job_bytes);
  const double frames_n = static_cast<double>(frames);
  const auto per_frame = [frames_n](std::uint64_t value, std::uint64_t baseline) {
    return (static_cast<double>(value) - static_cast<double>(baseline)) / frames_n;
  };
  std::fprintf(file,
               "[mtvu] vif jobs/frame=%.1f chains/frame=%.2f packets/job=%.1f bytes/job=%.0f chain_coalescing=%d\n",
               jobs / frames_n, chains / frames_n, jobs > 0.0 ? packets / jobs : 0.0,
               jobs > 0.0 ? job_bytes / jobs : 0.0, r.settings.chain_jobs ? 1 : 0);
  const auto &in_now = now.input_bell;
  const auto &in_base = r.baseline.input_bell;
  const auto &out_now = now.output_bell;
  const auto &out_base = r.baseline.output_bell;
  std::fprintf(file,
               "[mtvu] doorbells/frame: ee_to_worker rings=%.1f waits=%.1f wakeups=%.1f notifies=%.1f; worker_to_ee "
               "rings=%.1f waits=%.1f wakeups=%.1f notifies=%.1f\n",
               per_frame(in_now.rings, in_base.rings), per_frame(in_now.waits, in_base.waits),
               per_frame(in_now.wakeups, in_base.wakeups), per_frame(in_now.notifies, in_base.notifies),
               per_frame(out_now.rings, out_base.rings), per_frame(out_now.waits, out_base.waits),
               per_frame(out_now.wakeups, out_base.wakeups), per_frame(out_now.notifies, out_base.notifies));
  // Worker GIF output the EE forwarded to the GS stream: records and bytes per frame (the copy the fast path removes).
  std::fprintf(file, "[mtvu] worker output executed by the EE/frame: records=%.1f bytes=%.0f\n",
               per_frame(now.executed_records, r.baseline.executed_records),
               per_frame(now.executed_bytes, r.baseline.executed_bytes));
}

void reset_worker_baseline() {
  Runtime &r = rt();
  r.baseline = published_totals(r);
  r.baseline_worker_cpu_us = worker_thread_cpu_us(r);
}

// IN-459: the worker wakes a sleeping EE for its output only every GT4_MTVU_WAKE_KB KiB of output (default 64), at its
// last queued JobDone and before it waits for output space; the EE slept and woke about 700 times per VBlank of the
// night race, one wake per few records, and each wake was a system call on the worker. 0 wakes it for every record.
// IN-461: the hw renderer's GS worker reads the VU1 worker's output ring itself (direct GS intake). On by default since
// the owner's play test of candidate 332; GT4_MTVU_DIRECT=0 keeps the EE journal forwarding it (A/B), and a renderer
// without a GS worker in this process always uses the journal.
static bool direct_intake_from_environment() {
  const char *value = std::getenv("GT4_MTVU_DIRECT");
  if (value == nullptr || std::strcmp(value, "1") == 0) return true;
  if (std::strcmp(value, "0") == 0) return false;
  throw std::invalid_argument("GT4_MTVU_DIRECT must be unset, 0 or 1");
}

static std::size_t wake_output_bytes_from_environment() {
  const char *value = std::getenv("GT4_MTVU_WAKE_KB");
  if (value == nullptr) return std::size_t{64} << 10;
  const std::string_view digits(value);
  if (digits.empty() || digits.find_first_not_of("0123456789") != std::string_view::npos || digits.size() > 6) {
    throw std::invalid_argument("GT4_MTVU_WAKE_KB must be a size in KiB");
  }
  return static_cast<std::size_t>(std::stoul(value)) << 10;
}

void start(const Settings &settings, const Donor &donor) {
  Runtime &r = rt();
  if (r.started) throw std::logic_error("mtvu started twice");
  r.started = true;
  r.settings = settings;
  r.donor = donor;
  if (!settings.enabled) return;
  if (!donor.worker_vif_packet || !donor.worker_vif_write || !donor.worker_vu_reset || !donor.render_gs ||
      !donor.vif1_state_hash || !donor.apply_signal) {
    throw std::logic_error("mtvu needs every donor callback");
  }
  r.input = std::make_unique<SpscByteRing>(kInputBytes, r.input_bell);
  r.output = std::make_unique<SpscByteRing>(kOutputBytes, r.output_bell);
  r.sink = std::make_unique<DonorSink>(donor.render_gs, donor.post_vsync);
  r.sink->set_privileged_consumer(r.ordered_privileged);
  r.sink->set_readback_consumer(r.download.post_readback);
  r.journal = std::make_unique<EeJournal>(*r.input, *r.output, r.input_bell, r.output_bell, *r.sink);
  r.worker_end = std::make_unique<WorkerEnd>(*r.input, *r.output, r.input_bell, r.output_bell,
                                             wake_output_bytes_from_environment());
  // IN-461: direct GS intake (default; GT4_MTVU_DIRECT=0 turns it off) needs a GS consumer in this process that reads
  // the output ring (the hw renderer with its GS worker); verify keeps the journal (its hash covers the GS stream on
  // the EE).
  r.direct = direct_intake_from_environment() && !settings.verify && donor.direct_gs_available != nullptr &&
             donor.post_gs_job != nullptr && donor.direct_gs_available();
  if (r.direct) {
    r.sink->set_direct_consumer(donor.post_gs_job, *r.output, r.output_bell);
    r.journal->set_direct(true);
  }
  r.worker_accounting = gt4::perf::Accounting(gt4::perf::Probe::shared().enabled());
  r.baseline = WorkerTotals{};
  gt4::perf::Probe::set_mtvu_hooks(report_interval, reset_worker_baseline);
  r.running.store(true);
  r.worker = std::thread(worker_main);
}

bool enabled() { return rt().running.load(std::memory_order_acquire); }

std::uint64_t worker_busy_us() {
  const WorkerTotals totals = published_totals(rt());
  return totals.vu1_us + totals.output_us;
}

void set_ordered_privileged_consumer(void (*post)(std::uint32_t address, std::uint64_t value)) {
  Runtime &r = rt();
  r.ordered_privileged = post;
  if (r.sink) r.sink->set_privileged_consumer(post);
}

bool privileged_async() {
  const Runtime &r = rt();
  return r.settings.privileged_async && r.ordered_privileged != nullptr && enabled();
}

bool queue_privileged(std::uint32_t address, std::uint64_t value) {
  Runtime &r = rt();
  if (t_on_worker || !privileged_async() || r.journal->can_execute_now()) return false;
  r.journal->append_privileged(address, value);
  ++r.privileged_queued;
  return true;
}

std::uint64_t take_privileged_wait_us() {
  Runtime &r = rt();
  const std::uint64_t waited = r.privileged_wait_us;
  r.privileged_wait_us = 0;
  return waited;
}

void set_download_ordering(const DownloadOrdering &ordering) {
  Runtime &r = rt();
  r.download = ordering;
  if (r.sink) r.sink->set_readback_consumer(ordering.post_readback);
}

bool queue_readback(std::uint32_t qwords) {
  Runtime &r = rt();
  if (t_on_worker || !enabled() || r.download.post_readback == nullptr) return false;
  r.journal->append_readback(qwords);
  ++r.readbacks_queued;
  return true;
}

void drain_download() {
  Runtime &r = rt();
  if (t_on_worker || !enabled() || r.journal->queued_entries() == 0) return;
  if (r.direct) {
    // IN-461: the READ_FIFO then waits for the GS worker, which reads the VU1 output itself; the queued EE entries (the
    // deferred READ_FIFOs) still go first, and never wait.
    r.journal->drain(true);
    return;
  }
  r.download_deferred = false;
  ++r.syncs["download"];
  gt4::perf::Scope waiting(gt4::perf::Category::mtvu_wait_download);
  r.journal->drain(true);
  rethrow_worker_error();
}

bool on_worker() { return t_on_worker; }

namespace {

// IN-522, on the EE: the part of a VIF packet at `data` that comes from the pending table, at this offset of its payload.
void note_table_copy(Runtime &r, const std::uint8_t *data, std::size_t bytes, std::size_t payload_offset,
                     std::vector<Runtime::TablePatch> &out) {
  if (r.table_dest == nullptr) return;
  const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(data), end = begin + bytes;
  const std::uintptr_t table = reinterpret_cast<std::uintptr_t>(r.table_dest), table_end = table + r.table_bytes;
  if (end <= table || begin >= table_end) return;
  const std::uintptr_t from = std::max(begin, table), to = std::min(end, table_end);
  out.push_back({0, r.table_ticket, payload_offset + (from - begin), static_cast<std::size_t>(from - table),
                 static_cast<std::size_t>(to - from)});
}

void post_table_patches(Runtime &r, std::uint64_t job, std::vector<Runtime::TablePatch> &patches) {
  if (patches.empty()) return;
  {
    const std::lock_guard<std::mutex> lock(r.job_patches_mutex);
    for (Runtime::TablePatch &patch : patches) {
      patch.job = job;
      r.job_patches.push_back(patch);
      if (patch.ticket != r.first_patched_ticket) {  // TASK-178 diagnostics
        r.first_patched_ticket = patch.ticket;
        r.first_patched_job[patch.ticket % r.first_patched_job.size()].store((patch.ticket << 32) | (job & 0xFFFFFFFFull),
                                                                            std::memory_order_release);
      }
    }
  }
  r.job_patches_posted.fetch_add(patches.size(), std::memory_order_release);
  patches.clear();
}

}  // namespace

void note_deferred_table(const std::uint8_t *dest, std::size_t bytes, std::uint64_t ticket) {
  Runtime &r = rt();
  r.table_dest = dest;
  r.table_bytes = bytes;
  r.table_ticket = ticket;
}

void set_deferred_table_copy(DeferredTableCopy copy) { rt().table_copy = copy; }

std::uint64_t deferred_table_patches() { return rt().job_patches_posted.load(std::memory_order_relaxed); }

std::uint64_t deferred_table_first_job(std::uint64_t ticket) {
  const Runtime &r = rt();
  const std::uint64_t packed = r.first_patched_job[ticket % r.first_patched_job.size()].load(std::memory_order_acquire);
  return (packed >> 32) == (ticket & 0xFFFFFFFFull) ? (packed & 0xFFFFFFFFull) : 0;
}

bool direct_gs_intake() { return enabled() && rt().direct; }

void post_vif_packet(const std::uint8_t *data, std::uint32_t qwc) {
  Runtime &r = rt();
  if (!enabled()) return;
  const std::size_t bytes = static_cast<std::size_t>(qwc) * 16;
  ++r.vif_packet_count;
  gt4::perf::frame_counters().vif_packets += 1;
  if (!r.settings.chain_jobs) {
    const std::uint64_t job = r.next_job++;
    ++r.vif_job_count;
    r.vif_job_bytes += 4 + bytes;
    gt4::perf::frame_counters().vif_jobs += 1;
    gt4::perf::frame_counters().vif_job_bytes += 4 + bytes;
    note_table_copy(r, data, bytes, 0, r.batch_patches);
    post_table_patches(r, job, r.batch_patches);
    r.journal->post_job(RecordKind::VifPacket, job, data, bytes);
    r.journal->drain(false);
    rethrow_worker_error();
    return;
  }
  // TASK-084 step 1: append to the chain's batch. A batch above the size bound is posted as its own job, still far
  // below one job per packet; the journal placeholder keeps its position in the stream.
  if (!r.vif_batch.empty() && r.vif_batch.size() + 4 + bytes > kVifBatchBytes) flush_vif_chain();
  note_table_copy(r, data, bytes, r.vif_batch.size() + 4, r.batch_patches);
  r.vif_batch.append(data, qwc);
}

void flush_vif_chain() {
  Runtime &r = rt();
  if (!enabled() || r.vif_batch.empty()) return;
  const std::uint64_t job = r.next_job++;
  ++r.vif_job_count;
  r.vif_job_bytes += r.vif_batch.size();
  gt4::perf::frame_counters().vif_jobs += 1;
  gt4::perf::frame_counters().vif_job_bytes += r.vif_batch.size();
  post_table_patches(r, job, r.batch_patches);
  r.journal->post_job(RecordKind::VifBatch, job, r.vif_batch.payload(), r.vif_batch.size());
  r.vif_batch.reset();
  r.journal->drain(false);
  rethrow_worker_error();
}

void note_vif_chain() {
  Runtime &r = rt();
  if (!enabled()) return;
  ++r.vif_chains;
  ++gt4::perf::frame_counters().vif_chains;
}

void post_vif_write(std::uint32_t reg, std::uint32_t value) {
  Runtime &r = rt();
  if (!enabled()) return;
  // The register write must reach the worker after every packet the shadow already parsed.
  flush_vif_chain();
  const std::uint64_t job = r.next_job++;
  ++r.vif_writes;
  std::uint8_t payload[8];
  std::memcpy(payload, &reg, 4);
  std::memcpy(payload + 4, &value, 4);
  r.journal->post_job(RecordKind::VifWrite, job, payload, sizeof payload);
  r.journal->drain(false);
  rethrow_worker_error();
}

// Applies the signals of the packet being accepted, in stream order. SIGNAL/FINISH/LABEL change only SIGLBLID, CSR
// and the GS interrupt request, never the drawing. The single-thread path applies them at this same EE program
// position, so applying them now keeps the EE-observable order and removes the wait for the packet's worker job.
void apply_accepted_signals(Runtime &r) {
  for (const SignalWrite &write : r.signal_scratch) {
    ++r.signals_applied;
    r.donor.apply_signal(write.reg, write.value);
  }
  r.signal_scratch.clear();
}

bool signal_pre_applied(int path, std::uint32_t address) {
  return enabled() && pre_applies_signal(path, address);
}

bool stream_signals_pre_applied(int path) {
  // TASK-084 step 2: with MTVU on, PATH2 (VIF1 DIRECT) and PATH3 (DMA ch2) signals are applied by gif_transfer /
  // shadow_direct when the packet is accepted; the GS stream's second walk would only find them again.
  const Runtime &r = rt();
  return r.settings.gif_fast_path && enabled() && path != 1;
}

void gif_transfer(int path, const std::uint8_t *data, std::uint32_t qwc) {
  Runtime &r = rt();
  if (path < 0 || path > 3) throw std::logic_error("mtvu GIF path out of range");
  if (!enabled()) {
    r.donor.render_gs(path, data, qwc);
    return;
  }
  // The packet must render after every VIF1 packet the shadow already parsed (TASK-084 step 1).
  flush_vif_chain();
  ++r.ee_transfers;
  PathCounts counts;
  r.signal_scratch.clear();
  r.ee_scanners[path].scan(data, qwc, counts, &r.signal_scratch);
  if (path == 1 && !r.signal_scratch.empty()) {
    // PATH1 signals are an invariant violation (section 5.3): keep the single-thread order for them.
    sync("path1_signal");
    ++r.gs_renders;
    r.donor.render_gs(path, data, qwc);
    return;
  }
  apply_accepted_signals(r);
  if (qwc == 0) return;
  if (r.journal->can_execute_now()) {
    ++r.gs_renders;
    r.donor.render_gs(path, data, qwc);
    return;
  }
  r.journal->append_ee_gif(static_cast<std::uint8_t>(path), data, static_cast<std::size_t>(qwc) * 16);
  r.journal->drain(false);
  rethrow_worker_error();
}

void shadow_direct(int path, const std::uint8_t *data, std::uint32_t qwc) {
  Runtime &r = rt();
  if (path < 0 || path > 3) throw std::logic_error("mtvu GIF path out of range");
  if (!enabled()) return;
  ++r.direct_transfers;
  PathCounts counts;
  r.signal_scratch.clear();
  r.ee_scanners[path].scan(data, qwc, counts, &r.signal_scratch);
  // The worker renders this packet later; its signals are applied here, at the packet's program position.
  apply_accepted_signals(r);
}

void gif_reset() {
  Runtime &r = rt();
  sync("gif_reset");
  for (GifScanner &scanner : r.ee_scanners) scanner = GifScanner{};
}

void note_render(int path, const std::uint8_t *data, std::uint32_t qwc) {
  Runtime &r = rt();
  // TASK-084 step 0: GIF traffic per path, counted at the one point every render passes (both renderer modes).
  gt4::perf::count_gif(path, static_cast<std::uint64_t>(qwc) * 16);
  if (!r.settings.verify) return;
  const auto path_byte = static_cast<std::uint8_t>(path);
  fnv_bytes(r.frame_hash, &path_byte, 1);
  fnv_bytes(r.frame_hash, data, static_cast<std::size_t>(qwc) * 16);
}

// IN-461: waits until the worker finished every job posted so far (direct GS intake, where the EE sees no JobDone). The
// worker stores the finished job before its JobDone, and that JobDone, or its loud ring before it sleeps for input,
// wakes this thread.
static void wait_for_jobs(Runtime &r) {
  const std::uint64_t last = r.next_job - 1;
  for (;;) {
    const std::uint32_t seen = r.output_bell.snapshot();
    if (r.completed_job.load(std::memory_order_acquire) >= last) return;
    rethrow_worker_error();
    bool slept = false;
    r.output_bell.wait_changed(seen, slept);
  }
}

void sync(const char *reason) {
  Runtime &r = rt();
  if (!enabled() || t_on_worker) return;
  // The batch must reach the worker before the sync point so the drain covers it (TASK-084 step 1).
  flush_vif_chain();
  const std::string_view why(reason);
  const bool privileged = why == "gs_privileged";
  if (privileged && privileged_async()) {
    // TASK-041: the write that follows is queued behind the journal by its ordered consumer (queue_privileged), so
    // the stream order is kept without waiting for the worker. The EE-side register copy is written at this point.
    ++r.syncs["gs_privileged_ordered"];
    r.privileged_wait_us = 0;
    return;
  }
  if (why == "download" && r.download.defer && r.download.post_readback && r.download.defer()) {
    // TASK-116 step 1: the readback that follows is answered locally. Its READ_FIFO is queued behind this journal by the
    // caller (queue_readback) and posted at its stream position without waiting, so no drain is needed here; the
    // accurate path drains on demand (drain_download) and the VBlank drains as usual.
    ++r.syncs["download_deferred"];
    r.download_deferred = true;
    return;
  }
  // IN-461: with direct GS intake the journal holds EE entries only. A GS read (download) waits for the GS worker,
  // which waits for the VU1 output it reads, so it needs no wait here; a held VSYNC only needs the journal run; every
  // other reason (VU memory, resets, state, GIF reset, a PATH1 signal) waits until the worker finished every job.
  const bool waitForJobs = r.direct && why != "download" && why != "vblank_pending";
  ++r.syncs[r.direct && !waitForJobs ? std::string(reason) + "_direct" : std::string(reason)];
  using gt4::perf::Category;
  const Category wait = why == "vblank" ? Category::mtvu_wait_vblank
                      : why == "download" ? Category::mtvu_wait_download
                      : why == "gs_privileged" ? Category::mtvu_wait_gs_privileged
                      : why == "vu_memory" ? Category::mtvu_wait_vu_memory
                      : why == "gif_reset" ? Category::mtvu_wait_gif_reset
                      : (why == "vif_reset" || why == "vu_reset" || why == "hw_reset") ? Category::mtvu_wait_reset
                      : (why == "state" || why == "shutdown") ? Category::mtvu_wait_state
                      : Category::mtvu_wait_other;
  const std::uint64_t start = privileged ? wall_us() : 0;
  {
    gt4::perf::Scope waiting(wait);
    r.journal->drain(true);
    if (waitForJobs) wait_for_jobs(r);
  }
  if (privileged) r.privileged_wait_us = wall_us() - start;
  r.download_deferred = false;
  rethrow_worker_error();
}

void pump() {
  Runtime &r = rt();
  if (!enabled()) return;
  // TASK-180b: the pump is called from run_due_events, whose own scope hid this wait; gs_drain names the journal drain
  // (including any back-pressure it meets) apart from the worker error check.
  {
    gt4::perf::Scope drain(gt4::perf::Category::gs_drain);
    r.journal->drain(false);
  }
  rethrow_worker_error();
}

namespace {
// Runs when the frame's output is in stream order: after the blocking drain (pre-TASK-084) or when the deferred
// VBlank journal entry executes. EE thread. Prints the F8-equivalent report and compares the shadow/worker VIF1 state.
void after_vblank_output() {
  Runtime &r = rt();
  if (!r.started) return;
  if (r.settings.report_every) {
    // Headless runs have no window, so the probe's boundary never runs: drive it here (guest counters are zero).
    gt4::perf::Probe &probe = gt4::perf::Probe::shared();
    if (++r.report_frames % r.settings.report_every == 0) probe.request(gt4::perf::Request::report);
    probe.frame_boundary(gt4::perf::GuestSample{});
  }
  if (!r.settings.verify) return;
  ++r.verify_frames;
  const std::uint64_t shadow = enabled() ? r.donor.vif1_state_hash(false) : 0;
  const std::uint64_t worker = enabled() ? r.donor.vif1_state_hash(true) : 0;
  const bool same = !enabled() || shadow == worker;
  if (!same) ++r.verify_mismatches;
  std::fprintf(stderr, "[mtvu-verify] frame %llu gs %016llx vif1 shadow %016llx worker %016llx %s\n",
               static_cast<unsigned long long>(r.verify_frames), static_cast<unsigned long long>(r.frame_hash),
               static_cast<unsigned long long>(shadow), static_cast<unsigned long long>(worker), same ? "same" : "MISMATCH");
  if (!same && r.settings.strict) throw std::runtime_error("mtvu strict: shadow and worker VIF1 state differ");
  r.frame_hash = kFnvOffset;
}
}  // namespace

void DonorSink::vblank_flush() {
  Runtime &r = rt();
  r.pending_vblank = false;
  // TASK-084 step 3: the held VSYNC lands here, after every earlier record of the frame, in stream order.
  if (post_vsync_ && post_vsync_()) ++r.vblank_posts;
  after_vblank_output();
}

void vblank_sync() {
  // TASK-079: GT4_FORCE_WIDESCREEN holds the 16:9 state at every guest VBlank (windowed and headless).
  gt4::display::widescreen_hold_at_guest_vblank(ps2::g_ram, 32u << 20);
  // TASK-080: the display request sample drives the race view / pillarbox presentation (native/display_aspect.hpp).
  gt4::display_aspect::at_guest_vblank(ps2::g_ram, 32u << 20);
  // Slice164 GT4_FORCE_480P, also headless (TASK-027 owner request 2026-10-22: test runs use the launcher's 480p
  // races). The windowed path holds it in window.cpp as well; the hold writes only while the index is 0 (normal),
  // so running both changes nothing.
  {
    static const bool force480p = [] {
      try {
        return gt4::display::parse_force_480p(std::getenv("GT4_FORCE_480P"));
      } catch (const std::exception &error) {
        std::fprintf(stderr, "[display] %s; the 480p hold is off\n", error.what());
        return false;
      }
    }();
    static gt4::display::Force480pState state;
    if (force480p) gt4::display::force_race_480p_at_vblank(ps2::g_ram, 32u << 20, state);
  }
  // TASK-117: the flare-limits block of Performance mode (native/flare_limits.hpp; GT4_FLARE_LIMITS).
  gt4::flare_limits::at_guest_vblank(ps2::g_ram, 32u << 20);
  // TASK-160: the field of view zoom follows the pad buttons the game keeps (native/fov_adjust.hpp; GT4_FOV_ADJUST).
  gt4::fov_adjust::at_guest_vblank(ps2::g_ram, 32u << 20);
  Runtime &r = rt();
  if (!r.started) return;
  // TASK-084 step 3: in pcsx2 mode the VSYNC is a journal entry after the frame's VU1 output, so the EE does not
  // drain here. The verify comparison needs both sides idle, so it keeps the blocking drain; donor mode (CoreGS 1.1)
  // always drains because its present reads the output.
  // TASK-092: the same condition as the GS client deferral (vblank_journal_active); with the worker off there is no
  // journal to append to.
  const bool defer = vblank_journal_active(r.settings) && r.donor.post_vsync != nullptr;
  if (!defer) {
    sync("vblank");
    after_vblank_output();
    return;
  }
  flush_vif_chain();
  if (r.pending_vblank) sync("vblank_pending");  // never queue two VSYNCs behind the worker
  ++r.syncs["vblank_deferred"];
  // The entry runs on the next drain, after the guest VBlank handler has stored this frame's field in the client
  // (hw::vblank_start -> gs::vblank -> on_vsync runs after this function). No drain here: it would execute the entry
  // before the field exists.
  r.journal->append_vblank_flush();
  r.pending_vblank = true;
}

void worker_gif(int path, const std::uint8_t *data, std::uint32_t qwc) {
  Runtime &r = rt();
  if (!t_on_worker) throw std::logic_error("mtvu worker_gif called off the worker thread");
  if (qwc == 0) return;
  if (path == 1) {
    // Invariant (docs/MTVU-PLAN.md section 5.3): PATH1 never carries SIGNAL/FINISH/LABEL. Count and report.
    PathCounts counts;
    r.worker_scanners[1].scan(data, qwc, counts);
    if (counts.signal + counts.finish + counts.label != 0) {
      r.path1_signals.fetch_add(1, std::memory_order_relaxed);
      if (!r.path1_reported) {
        r.path1_reported = true;
        // IN-461: with direct GS intake the EE never walks the PATH1 output, so the IRQ is not applied at all.
        std::fprintf(stderr, "[mtvu] PATH1 SIGNAL/FINISH/LABEL in job %llu: IRQ %s (known divergence)\n",
                     static_cast<unsigned long long>(r.worker_job), r.direct ? "not applied" : "applied late");
      }
      if (r.settings.strict) throw std::runtime_error("mtvu strict: PATH1 SIGNAL/FINISH/LABEL");
    }
  }
  gt4::perf::Scope output(gt4::perf::Category::mtvu_output);
  r.worker_end->emit_gif(static_cast<std::uint8_t>(path), r.worker_job, data, static_cast<std::size_t>(qwc) * 16);
}

void stop() {
  Runtime &r = rt();
  if (!r.running.load(std::memory_order_acquire)) return;
  try {
    sync("shutdown");
  } catch (const std::exception &error) {
    std::fprintf(stderr, "[mtvu] worker failed: %s\n", error.what());
  }
  r.running.store(false);
  r.journal->send_stop();
  r.worker.join();
}

void report(std::FILE *output) {
  Runtime &r = rt();
  if (!r.started) return;
  std::fprintf(output, "[mtvu] enabled %d verify %d strict %d worker_failed %d chain_jobs %d gif_fastpath %d "
               "vblank_journal %d; deferred vsyncs %llu\n",
               r.settings.enabled ? 1 : 0, r.settings.verify ? 1 : 0, r.settings.strict ? 1 : 0,
               r.worker_failed.load() ? 1 : 0, r.settings.chain_jobs ? 1 : 0, r.settings.gif_fast_path ? 1 : 0,
               r.settings.vblank_journal ? 1 : 0, static_cast<unsigned long long>(r.vblank_posts));
  std::fprintf(output, "[mtvu] direct GS intake %d (IN-461: the GS worker reads the VU1 output ring)\n", r.direct ? 1 : 0);
  if (r.journal) {
    std::fprintf(output, "[mtvu] jobs %llu vif_packets %llu vif_writes %llu ee_transfers %llu direct_transfers %llu renders %llu\n",
                 static_cast<unsigned long long>(r.journal->jobs_posted()),
                 static_cast<unsigned long long>(r.vif_packet_count),
                 static_cast<unsigned long long>(r.vif_writes), static_cast<unsigned long long>(r.ee_transfers),
                 static_cast<unsigned long long>(r.direct_transfers), static_cast<unsigned long long>(r.gs_renders));
  }
  std::fprintf(output, "[mtvu] path1_signal %llu\n", static_cast<unsigned long long>(r.path1_signals.load()));
  for (const auto &[reason, count] : r.syncs)
    std::fprintf(output, "[mtvu] sync %s %llu\n", reason.c_str(), static_cast<unsigned long long>(count));
  std::fprintf(output, "[mtvu] signals applied on the EE at accept %llu\n",
               static_cast<unsigned long long>(r.signals_applied));
  std::fprintf(output, "[mtvu] privileged writes: async %d (GT4_MTVU_PRIV_ASYNC %d, ordered consumer %s); queued "
               "behind worker output %llu\n", privileged_async() ? 1 : 0, r.settings.privileged_async ? 1 : 0,
               r.ordered_privileged ? "registered" : "none", static_cast<unsigned long long>(r.privileged_queued));
  std::fprintf(output, "[mtvu] download sync: deferral %s, readback consumer %s; readbacks queued behind the journal %llu\n",
               r.download.defer ? "registered" : "none", r.download.post_readback ? "registered" : "none",
               static_cast<unsigned long long>(r.readbacks_queued));
  if (r.settings.verify)
    std::fprintf(output, "[mtvu] verify frames %llu mismatches %llu\n", static_cast<unsigned long long>(r.verify_frames),
                 static_cast<unsigned long long>(r.verify_mismatches));
}

}  // namespace gt4::mtvu

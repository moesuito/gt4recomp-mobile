#include "perf_probe.hpp"
#include "perf_clock.hpp"

#ifdef GT4_VU1_AOT
namespace ps2::vu {
__attribute__((weak)) void aot_report(std::FILE *) {}  // strong definition: VU1 AOT overlay
}
#endif
#ifdef GT4_VU0_AOT
namespace ps2::vu {
__attribute__((weak)) void vu0_aot_report(std::FILE *) {}  // strong definition: VU0 AOT overlay (slice 160)
}
#endif
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <time.h>  // TASK-180: CLOCK_THREAD_CPUTIME_ID
#endif

namespace gt4::perf {
namespace {
// Installed by the VU1 worker runtime (EE thread): report section and interval reset (TASK-011 step 3).
Probe::MtvuReport g_mtvu_report = nullptr;
Probe::MtvuReset g_mtvu_reset = nullptr;
Probe::MtvuReport g_gs_ipc_report = nullptr;
Probe::MtvuReset g_gs_ipc_reset = nullptr;
std::uint64_t wall_us() { return scope_wall_us(); }
#ifndef _WIN32
// TASK-180: the value of an environment variable, at most `size` - 1 characters, as GetEnvironmentVariableA gives it
// (the length copied; 0 when unset; the needed size when it does not fit).
unsigned long environment_value(const char *name, char *value, unsigned long size) {
  const char *text = std::getenv(name);
  if (text == nullptr) return 0;
  const std::size_t length = std::strlen(text);
  if (length + 1 > size) return static_cast<unsigned long>(length + 1);
  std::memcpy(value, text, length + 1);
  return static_cast<unsigned long>(length);
}
#endif
std::uint64_t cpu_us() {
#ifndef _WIN32
  timespec value{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) return UINT64_MAX;
  return static_cast<std::uint64_t>(value.tv_sec) * 1000000u + static_cast<std::uint64_t>(value.tv_nsec) / 1000u;
#else
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
    return UINT64_MAX;
  const auto ticks = [](FILETIME value) {
    return (std::uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime;
  };
  // Divide before adding: FILETIME units are 100ns, not guest cycles.
  return ticks(kernel) / 10 + ticks(user) / 10;
#endif
}
constexpr const char *names[] = {
    "ee_dispatch", "vu_interpret", "iop_run", "vif_gif", "gs_host_copy",
    "gs_execute", "presentation", "audio_push", "host_vblank",
    "gs_fence_wait", "acquire_wait", "present_wait", "audio_wait", "unattributed",
    "vu0_micro_interpret", "vu1_interpret"
#ifdef GT4_VU1_AOT
    , "vu1_aot"
#endif
#ifdef GT4_VU0_AOT
    , "vu0_aot"
#endif
    , "pacing_wait", "run_due_events", "kernel_service", "guest_idle", "syscall_dispatch"
    , "runloop_mtvu_pump", "runloop_sif_pump", "runloop_events", "vblank_action"
#ifdef GT4_MTVU_RUNTIME
    , "mtvu_wait_vblank", "mtvu_wait_download", "mtvu_wait_gs_privileged", "mtvu_wait_vu_memory",
    "mtvu_wait_gif_reset", "mtvu_wait_reset", "mtvu_wait_state", "mtvu_wait_other", "gs_drain", "mtvu_output",
    "gs_ipc_write", "gs_ipc_backpressure", "gs_ipc_vsync_post", "gs_ipc_readback_wait",
    "gif_walk"
#endif
    };
static_assert(std::size(names) == static_cast<std::size_t>(Category::count));

FrameCounters g_frame_counters;

// The probe prints the [frame] section when it reports, so the counters are visible in owner F8 and in the headless
// F8-equivalent runs (GT4_MTVU_REPORT_EVERY). GT4_PERF_COUNTERS=0 disables the increments (A/B without them).
bool counters_enabled() {
  static const bool enabled = [] {
    char value[8]{};
#ifdef _WIN32
    const DWORD length = GetEnvironmentVariableA("GT4_PERF_COUNTERS", value, sizeof value);
#else
    const unsigned long length = environment_value("GT4_PERF_COUNTERS", value, sizeof value);
#endif
    return !(length == 1 && value[0] == '0');
  }();
  return enabled;
}
}

Probe::Probe(Clocks clocks, bool enabled, std::FILE *output, bool detailed, bool split_vu)
    : clocks_(clocks), output_(output), detailed_(detailed), split_vu_(split_vu),
      owner_thread_(std::this_thread::get_id()),
      accounting_(enabled) {
  last_us_ = clocks_.wall_us();
  accounting_.restart(last_us_);
}

FrameCounters &frame_counters() noexcept { return g_frame_counters; }

void reset_frame_counters() noexcept { g_frame_counters = FrameCounters{}; }

SyscallScope::SyscallScope(std::uint32_t number) noexcept
    : start_us_(counters_enabled() && Probe::shared().enabled() ? wall_us() : 0), number_(number & 0x7Fu) {
  if (start_us_ != 0) ++g_frame_counters.syscalls;
}

SyscallScope::~SyscallScope() {
  if (start_us_ == 0) return;
  ++g_frame_counters.syscall_count[number_];
  g_frame_counters.syscall_us += wall_us() - start_us_;
}

void count_thread_switch() noexcept {
  if (counters_enabled()) ++g_frame_counters.thread_switches;
}

void count_gif(int path, std::uint64_t bytes) noexcept {
  if (!counters_enabled() || path < 1 || path > 3) return;
  g_frame_counters.gif_bytes[path] += bytes;
  ++g_frame_counters.gif_records[path];
}

Probe &Probe::shared() {
#ifdef GT4_PERF_FRAME_ONLY
  constexpr bool detailed = false;
#else
  constexpr bool detailed = true;
#endif
#ifdef GT4_PERF_VU_SPLIT
  constexpr bool split_vu = true;
#else
  constexpr bool split_vu = false;
#endif
  static Probe probe({wall_us, cpu_us}, [] {
    char value[8]{};
#ifdef _WIN32
    if (GetEnvironmentVariableA("GT4_PERF", value, sizeof value) == 1 && value[0] == '1')
      return true;
#else
    if (environment_value("GT4_PERF", value, sizeof value) == 1 && value[0] == '1')
      return true;
#endif
    // TASK-011 step 3: GT4_MTVU_REPORT_EVERY (headless [mtvu] report) turns the accounting on; "0" or unset does not.
    char every[32]{};
#ifdef _WIN32
    const DWORD length = GetEnvironmentVariableA("GT4_MTVU_REPORT_EVERY", every, sizeof every);
#else
    const unsigned long length = environment_value("GT4_MTVU_REPORT_EVERY", every, sizeof every);
#endif
    return length > 0 && !(length == 1 && every[0] == '0');
  }(), stderr, detailed, split_vu);
  probe.set_report_status(&ReportStatus::shared());
  return probe;
}

bool Probe::owner() noexcept {
  if (std::this_thread::get_id() == owner_thread_)
    return true;
  wrong_thread_.store(true);
  return false;
}

bool Probe::valid() const noexcept {
  return !wrong_thread_.load() && !counter_fault_ &&
         accounting_.fault() == AccountingFault::none;
}

std::uint64_t Probe::timestamp() noexcept {
  if (enabled())
    last_us_ = clocks_.wall_us();
  return last_us_;
}

Accounting::Token Probe::enter(Category category) noexcept {
  return owner() ? accounting_.enter(category, timestamp()) : 0;
}
void Probe::leave(Accounting::Token token) noexcept {
  if (owner()) accounting_.leave(token, timestamp());
}
void Probe::request(Request request, bool repeated) noexcept {
  if (!owner() || repeated) return;
  switch (request) {
  case Request::start: pending_start_ = true; break;
  case Request::report: pending_report_ = true; break;
  case Request::toggle: pending_toggle_ = !pending_toggle_; break;
  }
}

void Probe::add(std::uint64_t &value, std::uint64_t amount) noexcept {
  if (!owner() || !enabled()) return;
  if (amount > UINT64_MAX - value) counter_fault_ = true;
  else value += amount;
}
void Probe::bytes(bool read, std::uint64_t bytes) noexcept {
  add(read ? read_bytes_ : write_bytes_, bytes);
}
void Probe::presentation(std::uint64_t rgba_bytes) noexcept {
  add(presentations_, 1); add(rgba_bytes_, rgba_bytes);
}
void Probe::dispatch(std::uint64_t workgroups) noexcept {
  add(dispatches_, 1); add(workgroups_, workgroups);
}
void Probe::audio(std::uint64_t produced, std::uint64_t completed,
                  std::uint64_t empty_refills) noexcept {
  add(audio_produced_, produced); add(audio_completed_, completed);
  add(empty_refills_, empty_refills);
}

void Probe::reset(std::uint64_t now, GuestSample guest) noexcept {
  // TASK-062: keep the time of the finished interval for cumulative_exclusive_us() (checkpoint first, so the time up to
  // `now` belongs to the interval that ends here).
  accounting_.checkpoint(now);
  for (std::size_t index = 0; index < carried_exclusive_us_.size(); ++index)
    carried_exclusive_us_[index] += accounting_.interval().categories[index].exclusive_us;
  accounting_.restart(now);
  if (g_mtvu_reset)
    g_mtvu_reset();
  if (g_gs_ipc_reset)
    g_gs_ipc_reset();
  last_us_ = last_frame_us_ = now;
  last_guest_ = guest;
  frames_ = guest_cycles_ = primitives_ = vu_calls_ = 0;
  read_bytes_ = write_bytes_ = rgba_bytes_ = 0;
  presentations_ = dispatches_ = workgroups_ = 0;
  audio_produced_ = audio_completed_ = empty_refills_ = 0;
  period_count_ = next_period_ = 0;
  guest_reset_ = false;
  first_frame_ = false;
  cpu_start_ = cpu_at_boundary_;
  cpu_valid_ = cpu_start_ != UINT64_MAX;
  reset_frame_counters();
}

void Probe::frame_boundary(GuestSample guest) noexcept {
  if (!owner()) return;
  if (!enabled() && !pending_toggle_ && !pending_start_ && !pending_report_) return;
  const auto now = clocks_.wall_us();
  cpu_at_boundary_ = clocks_.thread_cpu_us ? clocks_.thread_cpu_us() : UINT64_MAX;
  last_us_ = now;
  accounting_.checkpoint(now);
  if (enabled() && !first_frame_) {
    add(frames_, 1);
    periods_[next_period_] = now >= last_frame_us_ ? now - last_frame_us_ : 0;
    next_period_ = (next_period_ + 1) % capacity;
    period_count_ = std::min(period_count_ + 1, capacity);
    const auto delta = [this](auto current, auto previous, auto &total) {
      if (current < previous) guest_reset_ = true;
      else add(total, current - previous);
    };
    delta(guest.cycles, last_guest_.cycles, guest_cycles_);
    delta(guest.primitives, last_guest_.primitives, primitives_);
    delta(guest.vu1_interpreted, last_guest_.vu1_interpreted, vu_calls_);
    last_frame_us_ = now;
    last_guest_ = guest;
  } else if (enabled()) reset(now, guest);

  // Preserve a requested report before a same-boundary pause/restart.
  if (pending_report_) {
    print("manual (F8)");
    reset(now, guest);
    manual_ = false;
    if (status_) {
      if (report_written_) status_->report_saved(ReportStatus::now_ms());
      else status_->interval_ended();
    }
  }
  if (pending_toggle_) {
    if (enabled() && !pending_report_) print("diagnostics paused (F9)");
    accounting_.set_enabled(!enabled(), now);
    reset(now, guest);
    manual_ = false;
    if (status_ && !enabled() && !pending_report_) status_->interval_ended();  // a report saved in this boundary stays Saved
    std::fprintf(output_, "[perf] diagnostics %s at VBlank; new interval\n",
                 enabled() ? "enabled" : "disabled");
  }
  if (pending_start_) {
    if (!enabled()) accounting_.set_enabled(true, now);
    reset(now, guest);
    manual_ = true;
    std::fputs("[perf] manual interval started (F7); automatic resets suspended\n", output_);
    if (status_) status_->interval_started();
  }
  const bool periodic = !manual_ && enabled() && accounting_.interval().elapsed_us >= 5000000;
  if (periodic) {
    print("periodic");
    reset(now, guest);
    manual_ = false;
  }
  pending_toggle_ = pending_start_ = pending_report_ = false;
}

// TASK-084 step 0: the [frame] section. It separates sleep from work (pacing_wait), splits the run loop, and reports
// the GIF/VIF/MTVU shape that the wall-time categories cannot show. Every number is per VBlank.
static void print_frame_counters(std::FILE *file, std::uint64_t frames, std::uint64_t elapsed_us,
                                 const IntervalTime &interval) {
  if (!frames || !elapsed_us) return;
  const FrameCounters &c = g_frame_counters;
  const double n = static_cast<double>(frames);
  const double wall_ms = elapsed_us / 1000.0 / n;
  const double pacing_ms = interval.categories[static_cast<std::size_t>(Category::pacing_wait)].exclusive_us / 1000.0 / n;
  std::fprintf(file,
               "[frame] pacing sleep=%.3f ms/frame; EE frame wall=%.3f; critical path=%.3f ms/frame (wall minus "
               "pacing sleep)\n",
               pacing_ms, wall_ms, wall_ms - pacing_ms);
  const auto cat_ms = [&interval, n](Category category) {
    return interval.categories[static_cast<std::size_t>(category)].exclusive_us / 1000.0 / n;
  };
  std::fprintf(file,
               "[frame] run loop ms/frame: run_due_events=%.3f (worker pump=%.3f sif pump=%.3f events=%.3f "
               "vblank=%.3f) kernel_service=%.3f guest_idle=%.3f "
               "syscall_dispatch=%.3f; syscalls/frame=%.1f in %.3f ms; thread_switches/frame=%.1f\n",
               cat_ms(Category::run_due_events), cat_ms(Category::runloop_mtvu_pump), cat_ms(Category::runloop_sif_pump),
               cat_ms(Category::runloop_events), cat_ms(Category::vblank_action), cat_ms(Category::kernel_service),
               cat_ms(Category::guest_idle), cat_ms(Category::syscall_dispatch), c.syscalls / n, c.syscall_us / 1000.0 / n,
               c.thread_switches / n);
  std::fprintf(file, "[frame] gif bytes/frame: path1=%.0f path2=%.0f path3=%.0f; records/frame: path1=%.1f path2=%.1f "
               "path3=%.1f\n", c.gif_bytes[1] / n, c.gif_bytes[2] / n, c.gif_bytes[3] / n, c.gif_records[1] / n,
               c.gif_records[2] / n, c.gif_records[3] / n);
  std::fprintf(file,
               "[frame] vif/frame: packets=%.1f jobs=%.1f bytes/job=%.0f dma_chains=%.2f\n",
               c.vif_packets / n, c.vif_jobs / n, c.vif_jobs ? static_cast<double>(c.vif_job_bytes) / c.vif_jobs : 0.0,
               c.vif_chains / n);
  // The six busiest syscalls of the interval, by number, so a change in the guest's kernel traffic is visible.
  std::array<std::size_t, 128> order{};
  for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
  const auto &counts = c.syscall_count;
  std::partial_sort(order.begin(), order.begin() + 6, order.end(), [&counts](std::size_t a, std::size_t b) {
    return counts[a] > counts[b];
  });
  std::fputs("[frame] syscalls/frame top:", file);
  for (std::size_t i = 0; i < 6 && c.syscall_count[order[i]] != 0; ++i)
    std::fprintf(file, " %02zX=%.1f", order[i], c.syscall_count[order[i]] / n);
  std::fputc('\n', file);
}

void Probe::print(const char *reason) noexcept {
  report_written_ = false;
  if (!enabled()) { std::fprintf(output_, "[perf] %s: disabled\n", reason); return; }
  if (!valid()) {
    std::fprintf(output_, "[perf] INVALID: accounting_fault=%u wrong_thread=%d counter_fault=%d\n",
                 static_cast<unsigned>(accounting_.fault()), int(wrong_thread_.load()), int(counter_fault_));
    return;
  }
  const auto elapsed = accounting_.interval().elapsed_us;
  if (!frames_ || !elapsed || !period_count_) {
    std::fprintf(output_, "[perf] %s: no complete interval\n", reason); return;
  }
  std::array<std::uint64_t, capacity> sorted = periods_;
  std::sort(sorted.begin(), sorted.begin() + period_count_);
  const auto percentile = [&sorted, count = period_count_](std::size_t percent) {
    const auto rank = (percent * count + 99) / 100;
    return sorted[rank - 1] / 1000.0;
  };
  const double median = period_count_ % 2 ? sorted[period_count_ / 2] / 1000.0 :
      sorted[period_count_ / 2 - 1] / 2000.0 + sorted[period_count_ / 2] / 2000.0;
  const double frames = static_cast<double>(frames_);
  const double seconds = elapsed / 1000000.0;
  std::fprintf(output_, "[perf] %s: %llu VBlank samples in %.3fs = %.2f VBlanks/s; fresh_scene_frames=Unknown; measurement_mode=%s\n",
               reason, static_cast<unsigned long long>(frames_), seconds, frames / seconds,
               split_vu_ ? "vu-split" : detailed_ ? "detailed" : "frame-only");
  std::fprintf(output_, "[perf] frame ms: median %.3f p95 %.3f p99 %.3f max %.3f; samples=%zu/%llu (tail cap4096)\n",
               median, percentile(95), percentile(99), sorted[period_count_ - 1] / 1000.0,
               period_count_, static_cast<unsigned long long>(frames_));
  if (!guest_reset_)
    std::fprintf(output_, "[perf] guest_clock_model_speed=%.2f%% (294.912MHz); primitives/frame=%.1f vu1_interpreted/frame=%.1f\n",
        guest_cycles_ / 294912000.0 / seconds * 100.0, primitives_ / frames, vu_calls_ / frames);
  else std::fputs("[perf] guest counters reset: guest speed/count deltas Unknown\n", output_);
  const auto cpu_now = cpu_at_boundary_;
  report_cpu_ms_ = -1.0;
  if (cpu_valid_ && cpu_now != UINT64_MAX && cpu_now >= cpu_start_) {
    report_cpu_ms_ = (cpu_now - cpu_start_) / 1000.0 / frames;
    std::fprintf(output_, "[perf] measured_thread_cpu_ms/frame=%.3f; occupancy=%.1f%%\n",
        report_cpu_ms_, double(cpu_now - cpu_start_) / elapsed * 100);
  }
  else std::fputs("[perf] measured_thread_cpu=Unknown\n", output_);
  print_frame_counters(output_, frames_, elapsed, accounting_.interval());
  report_written_ = true;
  if (report_section_)
    report_section_(output_);
  if (g_mtvu_report)
    g_mtvu_report(output_, accounting_.interval(), frames_);
  if (!detailed_) {
    std::fputs("[perf] subsystem timers/host traffic counters=Unavailable (compiled out); boundary clocks/guest counters remain\n", output_);
    std::fflush(output_);
    return;
  }
  std::fputs("[perf] exclusive wall ms/frame (additive, includes instrumentation/scheduler time):", output_);
  for (std::size_t index = 0; index < kLegacyCategoryCount; ++index)
    std::fprintf(output_, " %s=%.3f", names[index],
        accounting_.interval().categories[index].exclusive_us / 1000.0 / frames);
  std::fputc('\n', output_);
  std::fputs("[perf] inclusive wall ms/frame (overlapping, DO NOT SUM):", output_);
  for (std::size_t index = 0; index < kLegacyCategoryCount; ++index)
    std::fprintf(output_, " %s=%.3f", names[index],
        accounting_.interval().categories[index].inclusive_us / 1000.0 / frames);
  std::fprintf(output_, "\n[perf] mapped_write_bytes/frame=%.0f mapped_read_bytes/frame=%.0f presented_RGBA_bytes/frame=%.0f presents/frame=%.2f dispatches/frame=%.1f workgroups/frame=%.0f\n",
      write_bytes_ / frames, read_bytes_ / frames, rgba_bytes_ / frames,
      presentations_ / frames, dispatches_ / frames, workgroups_ / frames);
  std::fprintf(output_, "[perf] audio frames/frame: produced=%.1f completed=%.1f empty_refills=%.2f; gpu_timestamp_time=Unknown\n",
      audio_produced_ / frames, audio_completed_ / frames, empty_refills_ / frames);
  if (split_vu_) {
    const auto &categories = accounting_.interval().categories;
    const double total_us = static_cast<double>(categories[static_cast<std::size_t>(Category::vu0_micro_interpret)].exclusive_us) +
        categories[static_cast<std::size_t>(Category::vu1_interpret)].exclusive_us;
#ifdef GT4_VU1_AOT
    std::fprintf(output_, "[perf] vu_scope=unit-split run_fast only; vu_interpreter_total_exclusive_ms/frame=%.3f; VU0_macro_in_EE; generated_VU1_AOT_included (vu1_aot category)\n",
                 total_us / 1000.0 / frames);
#else
    std::fprintf(output_, "[perf] vu_scope=unit-split run_fast only; vu_interpreter_total_exclusive_ms/frame=%.3f; VU0_macro_in_EE; generated_VU1_AOT_excluded\n",
                 total_us / 1000.0 / frames);
#endif
  }
#ifdef GT4_VU1_AOT
  ps2::vu::aot_report(output_);
#endif
#ifdef GT4_VU0_AOT
  ps2::vu::vu0_aot_report(output_);
#endif
  if (g_gs_ipc_report)
    g_gs_ipc_report(output_, accounting_.interval(), frames_);
  std::fflush(output_);
}

std::uint64_t Probe::cumulative_exclusive_us(Category category) const noexcept {
  const auto index = static_cast<std::size_t>(category);
  if (index >= carried_exclusive_us_.size())
    return 0;
  return carried_exclusive_us_[index] + accounting_.interval().categories[index].exclusive_us;
}

void Probe::set_mtvu_hooks(MtvuReport report, MtvuReset reset) noexcept {
  g_mtvu_report = report;
  g_mtvu_reset = reset;
}

void Probe::set_gs_ipc_hooks(MtvuReport report, MtvuReset reset) noexcept {
  g_gs_ipc_report = report;
  g_gs_ipc_reset = reset;
}

void bind_worker(Accounting *accounting, std::uint64_t (*wall)()) noexcept {
  t_worker_binding = {accounting, wall};
}

void Probe::set_report_section(std::function<void(std::FILE *)> section) {
  if (owner())
    report_section_ = std::move(section);
}

void Probe::finish() noexcept {
  if (owner()) std::fputs("[perf] session end: partial interval omitted; use F8 before closing\n", output_);
}
} // namespace gt4::perf

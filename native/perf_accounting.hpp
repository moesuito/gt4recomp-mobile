#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace gt4::perf {

enum class Category : std::size_t {
  ee_dispatch, vu_interpret, iop_run, vif_gif, gs_upload, gs_execute,
  presentation, audio_push, host_vblank, gs_wait, acquire_wait,
  presentation_wait, audio_wait, unattributed, vu0_micro_interpret, vu1_interpret,
#ifdef GT4_VU1_AOT
  vu1_aot,  // generated VU1 execution (VU1 AOT overlay only)
#endif
#ifdef GT4_VU0_AOT
  vu0_aot,  // generated VU0 execution (VU0 AOT overlay only, slice 160)
#endif
  // TASK-084 step 0 (slice260, docs/FRAME-COST-STUDY.md section 6): the pacer's sleep and the run-loop scopes, so the
  // F8 interval separates work from sleep and splits ee_dispatch/unattributed. Printed in the legacy lines.
  pacing_wait, run_due_events, kernel_service, guest_idle, syscall_dispatch,
  // TASK-180b: the inside of run_due_events, which is one scope in the F8 report and used to hide which of its parts
  // took the time (the worker pump, the SIF pump, the event drain and the VBlank callback). Always measured with the
  // report on, so Android and Windows read the same sections.
  runloop_mtvu_pump, runloop_sif_pump, runloop_events, vblank_action,
#ifdef GT4_MTVU_RUNTIME
  // TASK-011 step 3 (slice171): EE waits for the VU1 worker, by sync reason; GS front end executed from the journal;
  // worker-side GIF output (PATH1/PATH2 bytes). Reported only in the [mtvu] section, never in the legacy lines.
  mtvu_wait_vblank, mtvu_wait_download, mtvu_wait_gs_privileged, mtvu_wait_vu_memory,
  mtvu_wait_gif_reset, mtvu_wait_reset, mtvu_wait_state, mtvu_wait_other,
  gs_drain, mtvu_output,
  // TASK-035 (slice191): GS stream (GT4_RENDERER=pcsx2) on the EE thread: record writes into the shared ring,
  // waits for ring space or frames in flight, the VSYNC post, and READ_FIFO round trips. Reported only in the
  // [gs-ipc] section.
  gs_ipc_write, gs_ipc_backpressure, gs_ipc_vsync_post, gs_ipc_readback_wait,
  // TASK-084 (slice260): the GS stream's path walk in pcsx2 mode (the StreamSink that applies signals not pre-applied).
  gif_walk,
#endif
  count
};

// Categories printed by the legacy exclusive/inclusive lines; the MTVU categories are appended after them.
constexpr std::size_t kLegacyCategoryCount =
#ifdef GT4_MTVU_RUNTIME
    static_cast<std::size_t>(Category::mtvu_wait_vblank);
#else
    static_cast<std::size_t>(Category::count);
#endif

constexpr Category vu_interpreter_category(int unit) noexcept {
  return unit == 0 ? Category::vu0_micro_interpret :
         unit == 1 ? Category::vu1_interpret : Category::count;
}

enum class AccountingFault {
  none, backward_clock, invalid_category, scope_capacity,
  scope_order, counter_overflow
};

struct CategoryTime {
  std::uint64_t inclusive_us = 0;
  std::uint64_t exclusive_us = 0;
  // Entries begun in this enabled interval; a carried scope is not a new call.
  std::uint64_t entries = 0;
};

struct IntervalTime {
  std::uint64_t elapsed_us = 0;
  std::array<CategoryTime, static_cast<std::size_t>(Category::count)> categories{};
};

// One host thread; timestamps come from a monotonic host clock, never guest time.
class Accounting {
public:
  using Token = std::uint64_t;
  static constexpr std::size_t scope_capacity = 64;

  explicit Accounting(bool enabled = false) noexcept : enabled_(enabled) {}

  Token enter(Category category, std::uint64_t now_us) noexcept {
    const auto index = static_cast<std::size_t>(category);
    if (index >= static_cast<std::size_t>(Category::count) || category == Category::unattributed) {
      fail(AccountingFault::invalid_category);
      return 0;
    }
    if (!checkpoint(now_us))
      return 0;
    if (depth_ == scope_capacity) {
      fail(AccountingFault::scope_capacity);
      return 0;
    }
    if (next_token_ == std::numeric_limits<Token>::max()) {
      fail(AccountingFault::counter_overflow);
      return 0;
    }
    if (enabled_ && !add(interval_.categories[index].entries, 1))
      return 0;
    const Token token = ++next_token_;
    active_[depth_++] = {category, token};
    return token;
  }

  bool leave(Token token, std::uint64_t now_us) noexcept {
    if (!depth_ || !token || active_[depth_ - 1].token != token)
      return fail(AccountingFault::scope_order);
    if (!checkpoint(now_us))
      return false;
    --depth_;
    return true;
  }

  // Clip pending scopes at frame/report boundaries without closing them.
  bool checkpoint(std::uint64_t now_us) noexcept {
    if (fault_ != AccountingFault::none)
      return false;
    if (!clock_started_) {
      clock_started_ = true;
      last_us_ = now_us;
      return true;
    }
    if (now_us < last_us_)
      return fail(AccountingFault::backward_clock);
    const auto elapsed = now_us - last_us_;
    last_us_ = now_us;
    if (!enabled_ || !elapsed)
      return true;
    if (!add(interval_.elapsed_us, elapsed))
      return false;
    std::array<bool, static_cast<std::size_t>(Category::count)> seen{};
    for (std::size_t index = 0; index < depth_; ++index) {
      const auto category = static_cast<std::size_t>(active_[index].category);
      // Recursive scopes in one category measure its union, not duplicate time.
      if (!seen[category] &&
          !add(interval_.categories[category].inclusive_us, elapsed))
        return false;
      seen[category] = true;
    }
    const auto top = depth_ ? active_[depth_ - 1].category : Category::unattributed;
    if (!depth_ &&
        !add(interval_.categories[static_cast<std::size_t>(top)].inclusive_us, elapsed))
      return false;
    return add(interval_.categories[static_cast<std::size_t>(top)].exclusive_us,
               elapsed);
  }

  bool restart(std::uint64_t now_us) noexcept {
    if (!checkpoint(now_us))
      return false;
    interval_ = {};
    return true;
  }

  bool set_enabled(bool enabled, std::uint64_t now_us) noexcept {
    if (!checkpoint(now_us))
      return false;
    if (enabled && !enabled_)
      interval_ = {};
    enabled_ = enabled;
    return true;
  }

  const IntervalTime &interval() const noexcept { return interval_; }
  AccountingFault fault() const noexcept { return fault_; }
  bool enabled() const noexcept { return enabled_; }
  std::size_t active_scopes() const noexcept { return depth_; }

private:
  struct ActiveScope {
    Category category{};
    Token token = 0;
  };

  bool fail(AccountingFault fault) noexcept {
    if (fault_ == AccountingFault::none)
      fault_ = fault;
    return false;
  }

  bool add(std::uint64_t &value, std::uint64_t amount) noexcept {
    if (amount > std::numeric_limits<std::uint64_t>::max() - value)
      return fail(AccountingFault::counter_overflow);
    value += amount;
    return true;
  }

  std::array<ActiveScope, scope_capacity> active_{};
  IntervalTime interval_{};
  AccountingFault fault_ = AccountingFault::none;
  Token next_token_ = 0;
  std::size_t depth_ = 0;
  bool enabled_ = false;
  bool clock_started_ = false;
  std::uint64_t last_us_ = 0;
};

} // namespace gt4::perf

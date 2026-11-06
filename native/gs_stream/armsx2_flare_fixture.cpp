// SPDX-License-Identifier: MIT
// Fixture for the ARMSX2 sink's flare-table zero answer (round 8). No device, no GPL library: the decision is the same
// function the sink calls.
#include "gs_stream/armsx2_flare.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void require(bool condition, const char *what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

std::uint64_t g_nowait_qwords = 0;
std::uint32_t g_nowait_calls = 0;
std::uint64_t nowait(std::uint32_t qwords) {
  ++g_nowait_calls;
  g_nowait_qwords = qwords;
  return 7;
}

void test_hidden_flare_table_is_zeroed_and_posted() {
  std::uint8_t table[gt4::gs_stream::armsx2::kFlareTableQwords * 16];
  std::memset(table, 0xAB, sizeof table);
  g_nowait_calls = 0;
  g_nowait_qwords = 0;
  require(gt4::gs_stream::armsx2::answer_flare_table_with_zeros(
              table, gt4::gs_stream::armsx2::kFlareTableQwords, true, &nowait),
          "the hidden flare table is answered");
  bool all_zero = true;
  for (std::uint8_t byte : table) all_zero = all_zero && byte == 0;
  require(all_zero, "every byte of the flare table is zero");
  require(g_nowait_calls == 1 && g_nowait_qwords == gt4::gs_stream::armsx2::kFlareTableQwords,
          "the read is posted without waiting, once, for 128 quadwords");
}

void test_flares_shown_keeps_the_real_reply() {
  std::uint8_t table[gt4::gs_stream::armsx2::kFlareTableQwords * 16];
  std::memset(table, 0x5A, sizeof table);
  g_nowait_calls = 0;
  require(!gt4::gs_stream::armsx2::answer_flare_table_with_zeros(
              table, gt4::gs_stream::armsx2::kFlareTableQwords, false, &nowait),
          "with the flares shown the table is not answered locally");
  require(table[0] == 0x5A && table[sizeof table - 1] == 0x5A, "the caller's buffer is untouched");
  require(g_nowait_calls == 0, "nothing is posted");
}

void test_other_readbacks_wait() {
  std::uint8_t other[16 * 16];
  std::memset(other, 0x11, sizeof other);
  g_nowait_calls = 0;
  require(!gt4::gs_stream::armsx2::answer_flare_table_with_zeros(other, 16, true, &nowait),
          "a 16-quadword read is not the flare table");
  require(other[0] == 0x11, "the read is left to the blocking path");
  require(g_nowait_calls == 0, "nothing is posted for other reads");
}

}  // namespace

int main() {
  test_hidden_flare_table_is_zeroed_and_posted();
  test_flares_shown_keeps_the_real_reply();
  test_other_readbacks_wait();
  if (failures == 0) std::printf("armsx2 flare fixture: all checks passed\n");
  return failures == 0 ? 0 : 1;
}

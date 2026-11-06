// SPDX-License-Identifier: MIT
// The flare-table zero answer for the ARMSX2 in-process sink (round 8, owner 2026-11-06).
//
// The night race reads back a 64x8 PSMCT32 visibility table once per frame (2048 bytes = 128 quadwords). With "Sun glare
// and light flares" off the game should not wait for the GS to render and download it: the sink answers zeros at once
// and posts the read to the library without waiting, so the late reply is discarded. CoreGS 2.0 keeps its own path
// (TASK-116/117). This header is the decision alone, so it can be tested without a device or the GPL library.
#pragma once

#include <cstdint>
#include <cstring>

namespace gt4::gs_stream::armsx2 {

inline constexpr std::uint32_t kFlareTableQwords = 128;  // 64x8 PSMCT32, the same test as the PC client

/// Posts the read to the library without waiting for the reply; returns the request id (0 when not posted).
using NowaitFn = std::uint64_t (*)(std::uint32_t qwords);

/// True when `qwords` is the flare table and the flares are hidden: fills `data` with zeros and posts the read through
/// `nowait` (the library's late reply is never read). False leaves `data` untouched and posts nothing.
inline bool answer_flare_table_with_zeros(std::uint8_t *data, std::uint32_t qwords, bool hide_flares, NowaitFn nowait) {
  if (!hide_flares || qwords != kFlareTableQwords || data == nullptr) return false;
  std::memset(data, 0, static_cast<std::size_t>(qwords) * 16u);
  if (nowait != nullptr) nowait(qwords);
  return true;
}

}  // namespace gt4::gs_stream::armsx2

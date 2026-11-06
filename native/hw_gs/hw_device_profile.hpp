// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// TASK-179 (Android plan stage A1): the capability probe of CoreGS 2.0. Written from the Vulkan specification, clean room
// (decision 0070).
//
// One small record of what the chosen GPU is and has, filled once at start (vulkan_loader::probe_device_profile), logged
// in one line and handed to the renderer with the device (gpu::VulkanHandles::profile), so that later stages can key
// settings off the vendor and device class instead of the single "integrated GPU" test of today. Only the structure is
// new: `integrated_defaults` is today's rule (VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) and nothing reads `device_class`
// yet. Also the sizing of core 2's host-visible rings from the heaps (plan_core2_rings).
//
// Pure arithmetic and strings, no Vulkan: the numbers that come from Vulkan are copied in as plain integers (the
// physical device type has the VkPhysicalDeviceType values, the transform bits the VkSurfaceTransformFlagBitsKHR ones).
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

namespace gt4::hw {

// PCI vendor ids (and the Khronos ids of vendors without one) that Vulkan reports in VkPhysicalDeviceProperties::vendorID.
constexpr std::uint32_t kVendorAmd = 0x1002;
constexpr std::uint32_t kVendorNvidia = 0x10DE;
constexpr std::uint32_t kVendorIntel = 0x8086;
constexpr std::uint32_t kVendorArm = 0x13B5;        // Mali
constexpr std::uint32_t kVendorQualcomm = 0x5143;   // Adreno
constexpr std::uint32_t kVendorImagination = 0x1010;  // PowerVR
constexpr std::uint32_t kVendorSamsung = 0x144D;    // Xclipse
constexpr std::uint32_t kVendorApple = 0x106B;
constexpr std::uint32_t kVendorMesa = 0x10005;      // VK_VENDOR_ID_MESA (llvmpipe, lavapipe)

// VkPhysicalDeviceType values.
constexpr std::uint32_t kDeviceTypeOther = 0;
constexpr std::uint32_t kDeviceTypeIntegrated = 1;
constexpr std::uint32_t kDeviceTypeDiscrete = 2;
constexpr std::uint32_t kDeviceTypeVirtual = 3;
constexpr std::uint32_t kDeviceTypeCpu = 4;

inline const char *vendor_name(std::uint32_t vendor_id) {
  switch (vendor_id) {
  case kVendorAmd: return "AMD";
  case kVendorNvidia: return "NVIDIA";
  case kVendorIntel: return "Intel";
  case kVendorArm: return "ARM";
  case kVendorQualcomm: return "Qualcomm";
  case kVendorImagination: return "Imagination";
  case kVendorSamsung: return "Samsung";
  case kVendorApple: return "Apple";
  case kVendorMesa: return "Mesa";
  default: return "unknown";
  }
}

// What kind of GPU it is, for settings that depend on it.
//   discrete: its own memory (VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU).
//   integrated: an immediate-mode GPU sharing the system memory, from a desktop vendor (AMD, Intel, NVIDIA): the class
//     today's "integrated GPU" defaults were tuned on (an Intel HD 620).
//   tiler_or_unknown: everything else: the tile-based mobile GPUs (Adreno, Mali, PowerVR, Apple, Xclipse until measured)
//     and software, virtual or unknown devices. Nothing is tuned for this class yet (Android plan stages A4, A5).
enum class DeviceClass : std::uint8_t { discrete, integrated, tiler_or_unknown };

inline const char *device_class_name(DeviceClass device_class) {
  switch (device_class) {
  case DeviceClass::discrete: return "discrete";
  case DeviceClass::integrated: return "integrated";
  case DeviceClass::tiler_or_unknown: return "tiler-or-unknown";
  }
  return "tiler-or-unknown";
}

inline bool desktop_vendor(std::uint32_t vendor_id) {
  return vendor_id == kVendorAmd || vendor_id == kVendorIntel || vendor_id == kVendorNvidia;
}

inline DeviceClass classify_device(std::uint32_t vendor_id, std::uint32_t device_type) {
  if (device_type == kDeviceTypeDiscrete) return DeviceClass::discrete;
  if (device_type == kDeviceTypeIntegrated && desktop_vendor(vendor_id)) return DeviceClass::integrated;
  return DeviceClass::tiler_or_unknown;
}

// Today's rule for core 2's integrated-GPU defaults (IN-431, IN-476, IN-481, IN-494, IN-496, IN-517 and TASK-134): the
// device type alone. Kept exactly; a phone GPU that reports itself integrated still gets them until stage A4 keys them
// off the class instead.
inline bool integrated_defaults(std::uint32_t device_type) { return device_type == kDeviceTypeIntegrated; }

// ---- Unified memory (TASK-180 test4, STUDY-UNIFIED-MEMORY P1-A) ----------------------------------------------------
//
// The memory shape of the device: where the CPU writes and the GPU reads, and whether Vulkan reports the two as the same
// memory. Nothing keys a rendering decision off it yet (phase 1 only reports it); phases 3-5 do.
// The VkMemoryPropertyFlagBits values (copied in as plain integers like the rest of this header).
constexpr std::uint32_t kMemoryLocal = 0x01, kMemoryVisible = 0x02, kMemoryCoherent = 0x04, kMemoryCached = 0x08,
                        kMemoryLazy = 0x10, kMemoryProtected = 0x20;

struct MemoryTypeInfo {
  std::uint32_t flags = 0;  // VkMemoryType::propertyFlags
  std::uint32_t heap = 0;   // VkMemoryType::heapIndex
};

// discrete: the device's own memory. unified_all_local: an integrated or CPU device whose every host-visible type is also
// device-local (Adreno, Mali, Intel, Apple through MoltenVK, Lavapipe). unified_split: integrated, with host-visible types
// outside the device-local heap (AMD APUs on Windows). unknown: anything else (no types, virtual or other devices).
enum class MemoryShape : std::uint8_t { discrete, unified_all_local, unified_split, unknown };

inline const char *memory_shape_name(MemoryShape shape) {
  switch (shape) {
  case MemoryShape::discrete: return "discrete";
  case MemoryShape::unified_all_local: return "unified-all-local";
  case MemoryShape::unified_split: return "unified-split";
  case MemoryShape::unknown: return "unknown";
  }
  return "unknown";
}

// The rules, in order: no types -> unknown; a discrete device -> discrete; an integrated or CPU device -> unified-all-local
// when every host-visible type is device-local, unified-split when one is not, unknown when it has no visible type at all;
// anything else -> unknown.
inline MemoryShape classify_memory(std::uint32_t device_type, const MemoryTypeInfo *types, std::uint32_t count) {
  if (types == nullptr || count == 0) return MemoryShape::unknown;
  if (device_type == kDeviceTypeDiscrete) return MemoryShape::discrete;
  if (device_type != kDeviceTypeIntegrated && device_type != kDeviceTypeCpu) return MemoryShape::unknown;
  bool any_visible = false;
  bool all_visible_local = true;
  for (std::uint32_t i = 0; i < count; ++i) {
    if ((types[i].flags & kMemoryVisible) == 0) continue;
    any_visible = true;
    if ((types[i].flags & kMemoryLocal) == 0) all_visible_local = false;
  }
  if (!any_visible) return MemoryShape::unknown;
  return all_visible_local ? MemoryShape::unified_all_local : MemoryShape::unified_split;
}

// "L", "V", "C", "H", "Z" (lazily allocated), "P" (protected), in that order; the S23's type 6 reads "LVCH".
inline std::string memory_type_letters(std::uint32_t flags) {
  std::string letters;
  if (flags & kMemoryLocal) letters += 'L';
  if (flags & kMemoryVisible) letters += 'V';
  if (flags & kMemoryCoherent) letters += 'C';
  if (flags & kMemoryCached) letters += 'H';
  if (flags & kMemoryLazy) letters += 'Z';
  if (flags & kMemoryProtected) letters += 'P';
  return letters;
}

struct HeapInfo {
  std::uint64_t size = 0;    // VkMemoryHeap::size
  std::uint64_t budget = 0;  // VK_EXT_memory_budget heapBudget at the probe; 0 without the extension
  std::uint64_t usage = 0;   // VK_EXT_memory_budget heapUsage at the probe
  bool device_local = false;
};

struct DeviceProfile {
  bool probed = false;  // false: the device's creator did not run the probe (every field below is a default)
  std::string name;
  std::uint32_t vendor_id = 0, device_id = 0, device_type = kDeviceTypeOther;
  std::uint32_t api_version = 0, driver_version = 0;
  DeviceClass device_class = DeviceClass::tiler_or_unknown;
  bool integrated_defaults = false;
  // TASK-182 step 3: a memory type that is both device-local and host-visible, i.e. the memory the CPU and the GPU both
  // use without a copy (an APU or a phone). The class above says what the device is; this says how its memory behaves.
  bool unified_memory = false;

  // Features that drawing at a scale above 1x relies on. `*_supported` is what the device has, `*_enabled` what the
  // device was created with (the creator sets it); the renderer keys its paths off `*_enabled`.
  bool large_points_supported = false, large_points_enabled = false;  // gl_PointSize = scale (hw_vertex.vert)
  float point_size_max = 1.0f, point_size_granularity = 0.0f;
  bool wide_lines_supported = false;  // reported only: every pipeline draws its lines 1.0 wide, so it is never needed
  float line_width_max = 1.0f;
  bool storage_image_array_dynamic_indexing_supported = false;  // hw_deinterlace_scaled.comp: history[index]
  bool storage_image_array_dynamic_indexing_enabled = false;

  // Memory: every heap, with its budget where VK_EXT_memory_budget exists (enabled on the device when it does).
  bool memory_budget = false;
  std::uint32_t heap_count = 0;
  std::array<HeapInfo, 16> heaps{};

  // Memory types (P1-A): their properties and heap, and the shape they make. `memory_shape_forced` says
  // GT4_HW_MEMORY_SHAPE replaced the probe's answer (diagnostics: a desktop can report a phone's shape in its logs).
  std::uint32_t type_count = 0;
  std::array<MemoryTypeInfo, 32> types{};
  MemoryShape memory_shape = MemoryShape::unknown;
  bool memory_shape_forced = false;

  // True when points of `size` pixels can be drawn as points (else core 2 draws them as two triangles).
  bool points_of_size(float size) const { return size <= 1.0f || (large_points_enabled && size <= point_size_max); }
};

// The largest heap with (device_local) or without device-local memory; -1 when there is none.
inline int largest_heap(const DeviceProfile &profile, bool device_local) {
  int best = -1;
  for (std::uint32_t i = 0; i < profile.heap_count && i < profile.heaps.size(); ++i) {
    if (profile.heaps[i].device_local != device_local) continue;
    if (best < 0 || profile.heaps[i].size > profile.heaps[static_cast<std::size_t>(best)].size) best = static_cast<int>(i);
  }
  return best;
}

inline std::string mib_text(std::uint64_t bytes) { return std::to_string(bytes >> 20) + " MiB"; }

// The one log line of the probe.
inline std::string profile_line(const DeviceProfile &p) {
  char text[512];
  auto yes = [](bool value) { return value ? "yes" : "no"; };
  std::snprintf(text, sizeof text,
                "%s, vendor %s (0x%04X) device 0x%04X, class %s, device-local host-visible memory %s, integrated defaults %s, "
                "Vulkan %u.%u.%u; "
                "large points %s (max %.1f, %s), wide lines %s (max %.1f, not used), "
                "storage image array dynamic indexing %s (%s); memory budget %s",
                p.name.c_str(), vendor_name(p.vendor_id), p.vendor_id, p.device_id, device_class_name(p.device_class),
                p.unified_memory ? "yes" : "no", p.integrated_defaults ? "on" : "off", p.api_version >> 22,
                (p.api_version >> 12) & 0x3FFu, p.api_version & 0xFFFu, yes(p.large_points_supported),
                static_cast<double>(p.point_size_max),
                p.large_points_enabled ? "enabled" : "points drawn as quads above 1x", yes(p.wide_lines_supported),
                static_cast<double>(p.line_width_max), yes(p.storage_image_array_dynamic_indexing_supported),
                p.storage_image_array_dynamic_indexing_enabled ? "enabled" : "constant-index shader",
                p.memory_budget ? "yes" : "no");
  std::string line = text;
  for (std::uint32_t i = 0; i < p.heap_count && i < p.heaps.size(); ++i) {
    const HeapInfo &heap = p.heaps[i];
    line += (i == 0 ? ", heaps " : ", ") + std::to_string(i) + (heap.device_local ? ":local " : ":host ") + mib_text(heap.size);
    if (p.memory_budget) line += " (budget " + mib_text(heap.budget) + ", used " + mib_text(heap.usage) + ")";
  }
  // P1-A: the memory shape and every memory type, `<index>:<letters>@<heap>`.
  line += ", memory shape ";
  line += memory_shape_name(p.memory_shape);
  if (p.memory_shape_forced) line += " (forced)";
  if (p.type_count == 0) {
    line += ", types none";
    return line;
  }
  line += ", types ";
  for (std::uint32_t i = 0; i < p.type_count && i < p.types.size(); ++i) {
    line += (i == 0 ? "" : " ") + std::to_string(i) + ":" + memory_type_letters(p.types[i].flags) + "@" +
            std::to_string(p.types[i].heap);
  }
  return line;
}

// ---- TASK-182 step 3: the K rule's device default (row 2 of STUDY-ASYNC-READBACK 9.1) -----------------------------
//
// The K rule (GT4_CORE2_SUBMIT_AFTER_WAIT) submits again K draw batches after a waited readback, so the GPU is not left
// idle while the GS worker records its way back to the next submission. That bubble is the product of two things: how
// long the CPU records before the next submission, and whether the GPU had queued work at all. It is largest on the
// devices with a slow CPU against a fast GPU and a bursty drain: the tile-based mobile GPUs (Adreno, Mali, PowerVR,
// Apple, Xclipse) and the unified-memory parts (an APU, where the GPU's work competes with the CPU for the same memory).
// On a discrete desktop GPU the measured wall gain is small on a fast CPU (TASK-178 9.3: -0.9..-2.4 ms in the medians
// under another agent's load, but the least disturbed pair differed by 0.07 ms), so K stays 0 there unless a device's own
// A/B says otherwise.
//
//   tiler_or_unknown: on (K = 8): the tile-based mobile GPUs, and software/virtual devices, which have no better data.
//   integrated:       on (K = 8) when the memory is unified (a memory type that is device-local and host-visible, or the
//                     integrated device type GT4's APU defaults already follow); off otherwise.
//   discrete:         off (K = 0).
inline bool k_rule_default_on(const DeviceProfile &profile) {
  if (profile.device_class == DeviceClass::discrete) return false;
  if (profile.device_class == DeviceClass::tiler_or_unknown) return true;
  return profile.unified_memory || profile.integrated_defaults;
}

constexpr std::uint32_t kRuleDefaultBatches = 8;  // the study's suggestion for a tiler (submitEvery is 32 there)

inline std::uint32_t k_rule_default_batches(const DeviceProfile &profile) {
  return k_rule_default_on(profile) ? kRuleDefaultBatches : 0u;
}

// Why the rule is on or off there, for the [core2] submissions line of game.log.
inline const char *k_rule_default_reason(const DeviceProfile &profile) {
  if (profile.device_class == DeviceClass::discrete) return "discrete GPU";
  if (profile.device_class == DeviceClass::tiler_or_unknown) return "tile-based or unknown GPU";
  if (profile.unified_memory) return "unified memory";
  return "integrated GPU with its own memory";
}

// ---- Core 2's host-visible rings, sized from the device (TASK-179) -------------------------------------------------
//
// Core 2 maps four rings: staging, vertex and uniform with one share per recording in flight, and the readback buffer.
// Today's sizes come from constants: per share 32 MiB staging, 8 MiB vertex and 2 MiB uniform, 8 or 16 shares (8 on an
// integrated GPU, 16 on a discrete one), and 64 MiB of readback: 400 or 736 MiB. Those stay the sizes, and the cap,
// wherever they fit in a quarter of the heap they live in (its VK_EXT_memory_budget budget when the device reports one,
// else its size). Where they do not, the number of shares goes down first (a share keeps its size, so no request that
// fits today stops fitting; fewer recordings are in flight), to two at least; then the readback buffer, to 16 MiB at
// least (a whole 2048 x 2048 1x reduction; larger display reads go in bands). A phone's low-memory killer acts fast.

constexpr std::uint64_t kRingMiB = std::uint64_t{1} << 20;
constexpr std::uint32_t kRingMinimumShares = 2;
constexpr std::uint64_t kReadbackMinimumBytes = 16 * kRingMiB;

struct RingRequest {
  std::uint64_t heap_size = 0;    // the heap of the ring memory type
  std::uint64_t heap_budget = 0;  // its budget (VK_EXT_memory_budget), 0 when unknown
  std::uint64_t forced_budget = 0;  // GT4_CORE2_RINGS=<MiB>: a budget to plan for instead (tests on a large desktop)
  bool fixed = false;               // GT4_CORE2_RINGS=fixed: today's sizes whatever the heap
  std::uint32_t shares = 16;        // today's share count
  std::uint64_t staging_share = 32 * kRingMiB, vertex_share = 8 * kRingMiB, uniform_share = 2 * kRingMiB;
  std::uint64_t readback = 64 * kRingMiB;
  std::uint64_t other = 0;  // other mapped buffers in the same heap (counted, never resized)
};

struct RingPlan {
  std::uint32_t shares = 0;
  std::uint64_t staging = 0, vertex = 0, uniform = 0, readback = 0;
  std::uint64_t allowance = 0;  // a quarter of the budget planned for (0 with `fixed`)
  bool reduced = false;         // below today's sizes
  const char *basis = "";       // "fixed", "forced budget", "heap budget" or "heap size"

  std::uint64_t total() const { return staging + vertex + uniform + readback; }
};

inline RingPlan plan_core2_rings(const RingRequest &r) {
  RingPlan plan;
  const std::uint64_t per_share = r.staging_share + r.vertex_share + r.uniform_share;
  auto fill = [&](std::uint32_t shares, std::uint64_t readback) {
    plan.shares = shares;
    plan.staging = r.staging_share * shares;
    plan.vertex = r.vertex_share * shares;
    plan.uniform = r.uniform_share * shares;
    plan.readback = readback;
  };
  fill(r.shares, r.readback);
  if (r.fixed) {
    plan.basis = "fixed";
    return plan;
  }
  std::uint64_t base = r.heap_size;
  plan.basis = "heap size";
  if (r.forced_budget != 0) {
    base = r.forced_budget;
    plan.basis = "forced budget";
  } else if (r.heap_budget != 0) {
    base = r.heap_size != 0 ? std::min(r.heap_budget, r.heap_size) : r.heap_budget;
    plan.basis = "heap budget";
  }
  if (base == 0) {  // nothing known about the heap: today's sizes
    plan.basis = "unknown heap";
    return plan;
  }
  plan.allowance = base / 4;
  const std::uint64_t room = plan.allowance > r.other ? plan.allowance - r.other : 0;
  if (plan.total() <= room) return plan;
  plan.reduced = true;
  const std::uint64_t for_shares = room > r.readback ? room - r.readback : 0;
  const std::uint64_t fitting = per_share != 0 ? for_shares / per_share : r.shares;
  const std::uint32_t shares = static_cast<std::uint32_t>(
      std::clamp<std::uint64_t>(fitting, kRingMinimumShares, std::max<std::uint64_t>(r.shares, kRingMinimumShares)));
  std::uint64_t readback = r.readback;
  const std::uint64_t share_bytes = per_share * shares;
  if (share_bytes + readback > room) {
    const std::uint64_t left = room > share_bytes ? room - share_bytes : 0;
    readback = std::clamp<std::uint64_t>(left / kRingMiB * kRingMiB, std::min(kReadbackMinimumBytes, r.readback), r.readback);
  }
  fill(shares, readback);
  return plan;
}

}  // namespace gt4::hw

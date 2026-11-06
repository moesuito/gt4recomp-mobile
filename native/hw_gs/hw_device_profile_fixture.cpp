// CTest hw_device_profile: TASK-179, the capability profile's class rule and log line, and the sizing of core 2's
// host-visible rings from the heap (hw_device_profile.hpp), host only.
#include "hw_device_profile.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace gt4::hw;

namespace {

void check(bool condition, const char *what) {
  if (condition) return;
  std::fprintf(stderr, "hw_device_profile: %s\n", what);
  std::exit(1);
}

constexpr std::uint64_t MiB = std::uint64_t{1} << 20;
constexpr std::uint64_t GiB = std::uint64_t{1} << 30;

RingRequest request(std::uint32_t shares, std::uint64_t heap, std::uint64_t budget) {
  RingRequest r;
  r.shares = shares;
  r.heap_size = heap;
  r.heap_budget = budget;
  r.other = 256 * 1024;  // core 2's delayed-reduction buffer
  return r;
}

bool today(const RingPlan &plan, std::uint32_t shares) {
  return plan.shares == shares && plan.staging == 32 * MiB * shares && plan.vertex == 8 * MiB * shares &&
         plan.uniform == 2 * MiB * shares && plan.readback == 64 * MiB && !plan.reduced;
}

}  // namespace

int main() {
  // ---- The class rule, and today's integrated-GPU rule kept as it is ----
  check(classify_device(kVendorAmd, kDeviceTypeDiscrete) == DeviceClass::discrete, "AMD discrete");
  check(classify_device(kVendorNvidia, kDeviceTypeDiscrete) == DeviceClass::discrete, "NVIDIA discrete");
  check(classify_device(kVendorIntel, kDeviceTypeIntegrated) == DeviceClass::integrated, "Intel integrated");
  check(classify_device(kVendorAmd, kDeviceTypeIntegrated) == DeviceClass::integrated, "AMD APU");
  check(classify_device(kVendorQualcomm, kDeviceTypeIntegrated) == DeviceClass::tiler_or_unknown, "Adreno is a tiler");
  check(classify_device(kVendorArm, kDeviceTypeIntegrated) == DeviceClass::tiler_or_unknown, "Mali is a tiler");
  check(classify_device(kVendorImagination, kDeviceTypeIntegrated) == DeviceClass::tiler_or_unknown, "PowerVR is a tiler");
  check(classify_device(kVendorMesa, kDeviceTypeCpu) == DeviceClass::tiler_or_unknown, "a software device is unknown");
  check(classify_device(kVendorNvidia, kDeviceTypeVirtual) == DeviceClass::tiler_or_unknown, "a virtual device is unknown");
  check(integrated_defaults(kDeviceTypeIntegrated), "the integrated defaults follow the device type");
  check(!integrated_defaults(kDeviceTypeDiscrete) && !integrated_defaults(kDeviceTypeOther), "only the integrated type");
  check(std::string(vendor_name(kVendorQualcomm)) == "Qualcomm" && std::string(vendor_name(0x1234)) == "unknown", "vendor names");

  // ---- TASK-180 test4 (P1-A): the memory shape and the type letters ----
  {
    // `list` is a device's memory types, in Vulkan's order, with each type in heap 0.
    auto shape = [](std::uint32_t device_type, std::initializer_list<std::uint32_t> list) {
      std::array<MemoryTypeInfo, 32> types{};
      std::uint32_t count = 0;
      for (std::uint32_t flags : list) types[count++].flags = flags;
      return classify_memory(device_type, types.data(), count);
    };
    const std::uint32_t L = kMemoryLocal, V = kMemoryVisible, C = kMemoryCoherent, H = kMemoryCached, Z = kMemoryLazy;
    // The owner's S23 Ultra, measured: 0:L 1:L 2:L 3:LVH 4:LVC 5:LVC 6:LVCH 7:L 8:L.
    check(shape(kDeviceTypeIntegrated, {L, L, L, L | V | H, L | V | C, L | V | C, L | V | C | H, L, L}) ==
              MemoryShape::unified_all_local,
          "the S23 is unified-all-local");
    // Intel-like, AMD APU-like and a ReBAR discrete card.
    check(shape(kDeviceTypeIntegrated, {L, L | V | C, L | V | C | H}) == MemoryShape::unified_all_local,
          "an Intel-like list is unified-all-local");
    {
      std::array<MemoryTypeInfo, 4> apu{};
      apu[0] = MemoryTypeInfo{L, 0};
      apu[1] = MemoryTypeInfo{V | C, 1};
      apu[2] = MemoryTypeInfo{V | C | H, 1};
      apu[3] = MemoryTypeInfo{L | V | C, 0};
      check(classify_memory(kDeviceTypeIntegrated, apu.data(), 4) == MemoryShape::unified_split,
            "host-visible types outside the local heap make an APU unified-split");
    }
    {
      std::array<MemoryTypeInfo, 4> rebar{};
      rebar[0] = MemoryTypeInfo{L, 0};
      rebar[1] = MemoryTypeInfo{V | C, 1};
      rebar[2] = MemoryTypeInfo{L | V | C, 0};  // ReBAR: a local type the CPU may write does not make it unified
      rebar[3] = MemoryTypeInfo{V | C | H, 1};
      check(classify_memory(kDeviceTypeDiscrete, rebar.data(), 4) == MemoryShape::discrete, "a ReBAR card is discrete");
    }
    check(shape(kDeviceTypeCpu, {L | V | C | H}) == MemoryShape::unified_all_local, "Lavapipe is unified-all-local");
    check(shape(kDeviceTypeIntegrated, {L, L | V | C, L | Z}) == MemoryShape::unified_all_local,
          "an Apple-like list is unified-all-local");
    check(shape(kDeviceTypeIntegrated, {L, L | Z}) == MemoryShape::unknown, "no host-visible type at all is unknown");
    check(shape(kDeviceTypeIntegrated, {}) == MemoryShape::unknown, "no types is unknown");
    check(shape(kDeviceTypeVirtual, {L | V | C}) == MemoryShape::unknown, "a virtual device is unknown");
    check(shape(kDeviceTypeOther, {L | V | C}) == MemoryShape::unknown, "any other device type is unknown");
    check(memory_type_letters(L | V | C | H) == "LVCH", "the S23's type 6 letters");
    check(memory_type_letters(L | Z) == "LZ", "a lazily allocated type");
    check(memory_type_letters(L | V | C | H | Z | kMemoryProtected) == "LVCHZP", "every letter, in order");
    check(std::string(memory_shape_name(MemoryShape::unified_split)) == "unified-split" &&
              std::string(memory_shape_name(MemoryShape::unknown)) == "unknown",
          "the shape names");
  }

  // ---- Points: one pixel always, larger only with largePoints and within the device's largest size ----
  DeviceProfile profile;
  check(profile.points_of_size(1.0f) && !profile.points_of_size(2.0f), "no largePoints: one-pixel points only");
  profile.large_points_supported = true;
  check(!profile.points_of_size(3.0f), "supported but not enabled is not enough");
  profile.large_points_enabled = true;
  profile.point_size_max = 64.0f;
  check(profile.points_of_size(12.0f) && !profile.points_of_size(65.0f), "enabled, up to the largest size");

  // ---- The log line ----
  profile.probed = true;
  profile.name = "Test GPU";
  profile.vendor_id = kVendorAmd;
  profile.device_type = kDeviceTypeDiscrete;
  profile.device_class = DeviceClass::discrete;
  profile.api_version = (1u << 22) | (4u << 12) | 335u;
  profile.memory_budget = true;
  profile.heap_count = 2;
  profile.heaps[0] = HeapInfo{16 * GiB, 15 * GiB, 1 * GiB, true};
  profile.heaps[1] = HeapInfo{32 * GiB, 30 * GiB, 2 * GiB, false};
  const std::string line = profile_line(profile);
  check(line.find("Test GPU, vendor AMD (0x1002)") == 0, "the line starts with the name and vendor");
  check(line.find("class discrete") != std::string::npos, "the line names the class");
  check(line.find("Vulkan 1.4.335") != std::string::npos, "the line names the Vulkan version");
  check(line.find("large points yes (max 64.0, enabled)") != std::string::npos, "the line names largePoints");
  check(line.find("heaps 0:local 16384 MiB (budget 15360 MiB, used 1024 MiB), 1:host 32768 MiB") != std::string::npos,
        "the line lists the heaps");
  check(largest_heap(profile, true) == 0 && largest_heap(profile, false) == 1, "largest heaps");
  // TASK-180 test4 (P1-A): the profile line carries the shape and every type.
  profile.memory_shape = MemoryShape::discrete;
  profile.type_count = 3;
  profile.types[0] = MemoryTypeInfo{kMemoryLocal, 0};
  profile.types[1] = MemoryTypeInfo{kMemoryLocal | kMemoryVisible | kMemoryCoherent, 0};
  profile.types[2] = MemoryTypeInfo{kMemoryLocal | kMemoryVisible | kMemoryCoherent | kMemoryCached, 0};
  {
    const std::string typed = profile_line(profile);
    check(typed.find("memory shape discrete") != std::string::npos, "the line names the memory shape");
    check(typed.find("types 0:L@0 1:LVC@0 2:LVCH@0") != std::string::npos, "the line lists every memory type");
    profile.memory_shape_forced = true;
    check(profile_line(profile).find("memory shape discrete (forced)") != std::string::npos, "a forced shape is marked");
    profile.memory_shape_forced = false;
    profile.type_count = 0;
    check(profile_line(profile).find("types none") != std::string::npos, "no types says so");
  }

  check(line.find("device-local host-visible memory no") != std::string::npos,
        "a discrete profile has no device-local host-visible type");

  // ---- TASK-182 step 3: the K rule's device default (row 2 of STUDY-ASYNC-READBACK 9.1) ----
  {
    DeviceProfile gpu;  // a discrete desktop GPU: K stays 0 until its own A/B
    gpu.device_class = DeviceClass::discrete;
    gpu.device_type = kDeviceTypeDiscrete;
    check(!k_rule_default_on(gpu) && k_rule_default_batches(gpu) == 0, "discrete: the rule stays off");
    check(std::string(k_rule_default_reason(gpu)) == "discrete GPU", "discrete: the reason");

    DeviceProfile phone;  // Adreno, Mali, PowerVR, Apple, Xclipse: tile-based
    phone.device_class = DeviceClass::tiler_or_unknown;
    phone.device_type = kDeviceTypeIntegrated;
    phone.vendor_id = kVendorQualcomm;
    check(k_rule_default_on(phone) && k_rule_default_batches(phone) == kRuleDefaultBatches, "a tiler: K = 8");
    check(std::string(k_rule_default_reason(phone)) == "tile-based or unknown GPU", "a tiler: the reason");

    DeviceProfile software;  // lavapipe and the like: no better data than the other tilers
    software.device_class = DeviceClass::tiler_or_unknown;
    software.device_type = kDeviceTypeCpu;
    check(k_rule_default_on(software), "a software device: K = 8");

    DeviceProfile apu;  // an integrated GPU sharing system memory: unified
    apu.device_class = DeviceClass::integrated;
    apu.device_type = kDeviceTypeIntegrated;
    apu.unified_memory = true;
    check(k_rule_default_on(apu) && k_rule_default_batches(apu) == kRuleDefaultBatches, "an APU with unified memory: K = 8");
    check(std::string(k_rule_default_reason(apu)) == "unified memory", "an APU: the reason");

    DeviceProfile integratedOwn;  // integrated by type, but its memory is its own (not the case today, kept honest)
    integratedOwn.device_class = DeviceClass::integrated;
    integratedOwn.device_type = kDeviceTypeIntegrated;
    integratedOwn.unified_memory = false;
    integratedOwn.integrated_defaults = false;
    check(!k_rule_default_on(integratedOwn), "an integrated GPU without unified memory keeps K = 0");
    check(std::string(k_rule_default_reason(integratedOwn)) == "integrated GPU with its own memory", "its reason");
    integratedOwn.integrated_defaults = true;  // today's integrated default rule (device type) still counts
    check(k_rule_default_on(integratedOwn), "the integrated device-type default is enough");

    DeviceProfile rebar;  // a discrete GPU with a host-visible device-local type (resizable BAR): the class wins
    rebar.device_class = DeviceClass::discrete;
    rebar.device_type = kDeviceTypeDiscrete;
    rebar.unified_memory = true;
    check(!k_rule_default_on(rebar), "a discrete GPU stays off even if it reports unified memory");
  }
  {
    DeviceProfile apuLine = profile;
    apuLine.unified_memory = true;
    const std::string text = profile_line(apuLine);
    check(text.find("device-local host-visible memory yes") != std::string::npos,
          "the line names the device-local host-visible type");
  }

  // ---- Rings: today's sizes wherever they fit in a quarter of the heap's budget ----
  check(today(plan_core2_rings(request(16, 16 * GiB, 15 * GiB)), 16), "a desktop keeps today's 736 MiB");
  check(plan_core2_rings(request(16, 16 * GiB, 15 * GiB)).total() == 736 * MiB, "736 MiB on a discrete GPU");
  check(today(plan_core2_rings(request(8, 4 * GiB, 0)), 8), "an integrated GPU without a budget keeps today's 400 MiB");
  check(plan_core2_rings(request(8, 4 * GiB, 0)).total() == 400 * MiB, "400 MiB on an integrated GPU");
  check(std::string(plan_core2_rings(request(8, 4 * GiB, 0)).basis) == "heap size", "the heap size without a budget");
  check(std::string(plan_core2_rings(request(8, 4 * GiB, 3 * GiB)).basis) == "heap budget", "the budget when there is one");
  check(plan_core2_rings(request(8, 4 * GiB, 3 * GiB)).allowance == 768 * MiB, "a quarter of the budget");
  check(plan_core2_rings(request(8, 2 * GiB, 3 * GiB)).allowance == 512 * MiB, "the budget is never above the heap");
  check(today(plan_core2_rings(request(16, 0, 0)), 16), "an unknown heap keeps today's sizes");
  {
    RingRequest fixed = request(16, 256 * MiB, 0);
    fixed.fixed = true;
    check(today(plan_core2_rings(fixed), 16), "GT4_CORE2_RINGS=fixed keeps today's sizes on any heap");
  }
  {  // 2 GiB of budget: 512 MiB allowed; 64 MiB of readback leaves 10 shares of 42 MiB.
    const RingPlan plan = plan_core2_rings(request(16, 6 * GiB, 2 * GiB));
    check(plan.reduced && plan.shares == 10 && plan.readback == 64 * MiB, "2 GiB: 10 shares");
    check(plan.staging == 320 * MiB && plan.vertex == 80 * MiB && plan.uniform == 20 * MiB, "2 GiB: ring sizes");
    check(plan.total() <= plan.allowance, "2 GiB: within the allowance");
  }
  {  // A forced budget of 256 MiB: 64 MiB allowed, below the floor of two shares and 16 MiB of readback.
    RingRequest forced = request(16, 16 * GiB, 15 * GiB);
    forced.forced_budget = 256 * MiB;
    const RingPlan plan = plan_core2_rings(forced);
    check(std::string(plan.basis) == "forced budget", "the forced budget wins");
    check(plan.shares == kRingMinimumShares && plan.readback == kReadbackMinimumBytes, "the floor: two shares, 16 MiB");
  }
  {  // 600 MiB of allowance: 13 shares would leave too little readback; 12 shares and 64 MiB fit.
    const RingPlan plan = plan_core2_rings(request(16, 8 * GiB, 2400 * MiB));
    check(plan.shares == 12 && plan.readback == 64 * MiB && plan.total() <= plan.allowance, "2400 MiB: 12 shares");
  }
  {  // Two shares fit but not the whole readback: it shrinks, in whole MiB, not below 16.
    RingRequest small = request(16, 1 * GiB, 0);
    small.forced_budget = 512 * MiB;  // 128 MiB allowed: two shares are 84 MiB, 43 MiB left (minus the 256 KiB)
    const RingPlan plan = plan_core2_rings(small);
    check(plan.shares == 2 && plan.readback == 43 * MiB && plan.total() <= plan.allowance, "128 MiB: two shares, 43 MiB");
  }
  // Every budget: never above today's sizes, never below the floor, within the allowance whenever the floor allows.
  for (std::uint64_t budget = 16 * MiB; budget <= 24 * GiB; budget += 48 * MiB) {
    for (std::uint32_t shares : {8u, 16u, 24u}) {
      const RingPlan plan = plan_core2_rings(request(shares, 32 * GiB, budget));
      check(plan.shares >= kRingMinimumShares && plan.shares <= shares, "shares between the floor and today's");
      check(plan.readback >= kReadbackMinimumBytes && plan.readback <= 64 * MiB, "readback between 16 and 64 MiB");
      check(plan.staging == 32 * MiB * plan.shares, "a share keeps its size");
      const bool atFloor = plan.shares == kRingMinimumShares && plan.readback == kReadbackMinimumBytes;
      check(atFloor || plan.total() + 256 * 1024 <= plan.allowance, "within the allowance above the floor");
      check(plan.reduced == !today(plan, shares), "reduced exactly when below today's sizes");
    }
  }
  std::printf("hw_device_profile: ok\n");
  return 0;
}

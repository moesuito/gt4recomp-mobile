// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from docs/hw-renderer-spec/CORE2-SPEC.md and CORE2-DESIGN.md (TASK-027 core rewrite), clean room
// (decision 0070). Vulkan entry points are called through pointers (hw_gs_vulkan_loader.hpp, device table).

// Vulkan create-info structs are initialised with sType only (pNext defaults to null by the spec).
#pragma clang diagnostic ignored "-Wmissing-field-initializers"

#include "core2_gpu.hpp"

#include "../writeback/hw_gs_vulkan_device_table.hpp"
#include "../hw_queue_lock.hpp"
#include "../shaders/hw_shader_blobs.hpp"
#include "../memory/hw_memory.hpp"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <unordered_map>

namespace hwgs::core2 {
namespace {

// Today's ring sizes for 8 recordings in flight (a recording's share is an eighth), and the readback buffer. TASK-179: the
// cap of the sizes planned from the device's heaps (gt4::hw::plan_core2_rings); GT4_CORE2_RINGS=fixed keeps them as they are.
constexpr VkDeviceSize kStagingBytes = static_cast<VkDeviceSize>(256) << 20;
constexpr VkDeviceSize kVertexBytes = static_cast<VkDeviceSize>(64) << 20;
constexpr VkDeviceSize kUniformBytes = static_cast<VkDeviceSize>(16) << 20;
constexpr VkDeviceSize kReadbackBytes = static_cast<VkDeviceSize>(64) << 20;
constexpr VkDeviceSize kPixelStateBytes = 256;  // one GpuPixelState block (hw_pixel_uber.frag)
constexpr std::uint32_t kAtlasSide = 4096;      // 1x texel atlas
constexpr std::uint32_t kGraphicsPushBytes = 32;
constexpr std::uint32_t kComputePushBytes = 128;
constexpr std::uint32_t kTransientSets = 32768;

bool check(VkResult result, const char *what, std::string &error) {
  if (result == VK_SUCCESS) return true;
  error = std::string(what) + " failed (VkResult " + std::to_string(static_cast<int>(result)) + ")";
  return false;
}

std::uint32_t FloatBits(float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof bits);
  return bits;
}

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void *mapped = nullptr;
  VkDeviceSize size = 0;
  VkDeviceSize used = 0;  // ring cursor for this recording, inside the recording's share
  VkDeviceSize base = 0;  // the recording's share: [base, base + size / slotCount) (S4: the slots take turns)
  std::uint32_t type = UINT32_MAX;  // the memory type it was allocated from (P1-B placement report)
};

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkImageView ropView = VK_NULL_HANDLE;  // IN-481: the same words as RGBA8 bytes, for colour attachments
  std::uint32_t nativeWidth = 0, nativeHeight = 0;
  std::uint32_t width = 0, height = 0;  // in output pixels
  std::uint32_t scale = 1;
  VkDeviceSize bytes = 0;
  bool alive = false;
  std::uint32_t serial = 0;  // B4: unique per make_image, so a cached descriptor set never names a reused image
  std::int32_t block = -1;   // S2: the image memory block it lives in (-1: an allocation of its own)
  VkDeviceSize offset = 0;   // S2: its place in that block
};

// B4: the images bound by one draw descriptor set, by serial.
struct DrawSetKey {
  std::uint32_t frame = 0, depth = 0, texels = 0, texture = 0;
  bool operator==(const DrawSetKey &other) const {
    return frame == other.frame && depth == other.depth && texels == other.texels && texture == other.texture;
  }
};
struct DrawSetKeyHash {
  std::size_t operator()(const DrawSetKey &k) const {
    std::uint64_t h = (static_cast<std::uint64_t>(k.frame) << 32 | k.depth) * 0x9E3779B97F4A7C15ull;
    h ^= (static_cast<std::uint64_t>(k.texels) << 32 | k.texture) + 0x7F4A7C159E3779B9ull + (h << 6) + (h >> 2);
    return static_cast<std::size_t>(h);
  }
};

// IN-471: the pixels a primitive of a batch can cover, in 12.4 native units, for deciding without the pixel interlock
// whether it must wait for earlier ones. A triangle, or (lines, points) only a box.
struct PrimitiveShape {
  std::int32_t x[3] = {}, y[3] = {};
  std::int32_t minX = 0, minY = 0, maxX = 0, maxY = 0;
  bool triangle = false;
};

// Whether an edge of `a` has all of `b` on its outer side. A vertex of `b` that is also a vertex of `a` may lie on the
// edge: identical vertices snap identically and the rasteriser's fill rule gives each sample of a shared edge or vertex
// to one triangle. Any other vertex needs a gap of 1/16 native pixel, more than the GPU's vertex snapping can close.
inline bool EdgeSeparates(const PrimitiveShape &a, const PrimitiveShape &b) {
  for (int i = 0; i < 3; ++i) {
    const int j = (i + 1) % 3, k = (i + 2) % 3;
    const std::int64_t ex = a.x[j] - a.x[i], ey = a.y[j] - a.y[i];
    const std::int64_t third = ex * (a.y[k] - a.y[i]) - ey * (a.x[k] - a.x[i]);
    if (third == 0) return true;  // no area: covers nothing
    const std::int64_t sign = third > 0 ? 1 : -1;
    const std::int64_t gap = (ex < 0 ? -ex : ex) + (ey < 0 ? -ey : ey);
    bool outside = true;
    for (int m = 0; m < 3 && outside; ++m) {
      const bool sharedVertex = (b.x[m] == a.x[0] && b.y[m] == a.y[0]) || (b.x[m] == a.x[1] && b.y[m] == a.y[1]) ||
                                (b.x[m] == a.x[2] && b.y[m] == a.y[2]);
      outside = sign * (ex * (b.y[m] - a.y[i]) - ey * (b.x[m] - a.x[i])) <= (sharedVertex ? 0 : -gap);
    }
    if (outside) return true;
  }
  return false;
}

inline bool ShapesMayShareAPixel(const PrimitiveShape &a, const PrimitiveShape &b) {
  if (a.maxX < b.minX || b.maxX < a.minX || a.maxY < b.minY || b.maxY < a.minY) return false;
  if (!a.triangle || !b.triangle) return true;
  return !EdgeSeparates(a, b) && !EdgeSeparates(b, a);
}

// IN-464: a set of the GLM's 512 pages of 8 KiB (32 blocks each), for page-granular GLM hazards (Impl::order).
struct GlmPages {
  std::array<std::uint64_t, 8> bits{};
  static GlmPages all() {
    GlmPages pages;
    pages.bits.fill(~std::uint64_t{0});
    return pages;
  }
  // Pages [first, first + count) of the 512, wrapping at the end of GS memory.
  void add_run(std::uint32_t first, std::uint32_t count) {
    if (count >= 512u) {
      bits.fill(~std::uint64_t{0});
      return;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::uint32_t page = (first + i) % 512u;
      bits[page >> 6] |= std::uint64_t{1} << (page & 63u);
    }
  }
  bool meets(const GlmPages &other) const {
    for (std::size_t i = 0; i < bits.size(); ++i) {
      if ((bits[i] & other.bits[i]) != 0) return true;
    }
    return false;
  }
  void merge(const GlmPages &other) {
    for (std::size_t i = 0; i < bits.size(); ++i) bits[i] |= other.bits[i];
  }
  void clear() { bits.fill(0); }
};

// B4: device memory kept in finished images for reuse by size (see Impl::retire).
constexpr VkDeviceSize kRecycledImageBytes = static_cast<VkDeviceSize>(128) << 20;
constexpr VkDeviceSize kRecycledImageBytesIntegrated = static_cast<VkDeviceSize>(32) << 20;  // IN-431

}  // namespace

// IN-507: the variants the game's scenes ask for, kept in the cache before the first race (scripts/coregs2_bench/known_variants.py).
static constexpr std::uint32_t kKnownVariantWords[] = {
#include "core2_known_variants.inc"
};

struct Gpu::Impl {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  gt4::hw::QueueLock *queueLock = nullptr;  // TASK-174: the presentation thread shares `queue` (VulkanHandles::queueLock)
  VkPhysicalDeviceMemoryProperties memoryProperties{};
  VkPhysicalDeviceLimits limits{};
  std::uint32_t scale = 1;
  float shift = 1.0f;
  bool interlock = false;
  bool ready = false;

  VkCommandPool pool = VK_NULL_HANDLE;
  // Two recordings alternate (double buffering): the CPU records one while the GPU runs the other, each with its own
  // command buffer, fence, descriptor pool and share of every ring.
  struct Slot {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    bool inFlight = false;
    std::uint32_t passTimestamps = 0;  // B4: draw pass timestamp pairs written in this slot's recording
    std::uint32_t catCount = 0;        // TASK-182: category marks written in this slot's recording
    std::uint8_t catKinds[512] = {};
    std::vector<Image> doomed;  // released once this slot's submission is complete
    std::uint64_t serial = 0;   // IN-439: the number of the submission this slot carries (1, 2, ...)
    // TASK-178 diagnostics (timestamp ticks, 0: none): this slot's last finished command buffer end, and, for the first
    // submission after a drain, the drained work's end, so its own start gives the GPU idle time in between.
    std::uint64_t endTick = 0;
    std::uint64_t idleFromTick = 0;
  };
  // S4: up to kMaxSlots submissions in flight (GT4_CORE2_SLOTS, 2 to 24; IN-517: default 16 on a discrete GPU, 8 on an
  // integrated one, set at Init, since a race VBlank at 6x has 25-45 submissions and 8 kept the GS thread within a third of a
  // frame of the GPU, waiting for it on every heavy stretch). With two, the CPU could run only
  // one submission ahead of the GPU: with the GPU at a low clock and full of small dependent commands, the GS thread
  // waited for it in the middle of frames (draw stage waits of 12-30 ms in the owner's menus and the Arcade race
  // start), and submitting more often made it worse.
  static constexpr std::uint32_t kMaxSlots = 24;
  static constexpr std::uint32_t kSubmitEveryAtInit = 0xFFFFFFFFu;  // submitEvery not set by the switch: decided at Init
  Slot slots[kMaxSlots];
  std::uint32_t slotCount = [] {
    const char *text = std::getenv("GT4_CORE2_SLOTS");
    if (!text) return std::uint32_t{0};  // decided at Init
    return static_cast<std::uint32_t>(std::clamp<long>(std::strtol(text, nullptr, 10), 2, static_cast<long>(kMaxSlots)));
  }();
  // B4: per slot, the start and end of its command buffer, then up to kPassTimestampPairs draw passes (start, end);
  // read when the slot's fence has signalled.
  static constexpr std::uint32_t kPassTimestampPairs = 64;
  // IN-465 A/B: GT4_CORE2_PASS_TIMESTAMPS=0 leaves out the draw pass timestamps (gpu_draw_passes reads 0). TASK-134: only a report
  // reads them, and leaving them out took 2-5% off a player's frame on the George V Paris lap, so they need the report or this switch.
  const bool passTimestampsOn = std::getenv("GT4_CORE2_PASS_TIMESTAMPS")
                                    ? std::strcmp(std::getenv("GT4_CORE2_PASS_TIMESTAMPS"), "0") != 0
                                    : std::getenv("GT4_PERF") || std::getenv("GT4_HW_PROFILE_EVERY") || std::getenv("GT4_MTVU_REPORT_EVERY");
  static constexpr std::uint32_t kTimestampsPerSlot = 2u + 2u * kPassTimestampPairs;
  VkQueryPool timestamps = VK_NULL_HANDLE;
  // TASK-182 (GT4_CORE2_GPU_CATEGORIES=1, diagnostics): where the GPU time of a recording goes by kind of work. Each change of
  // kind writes a bottom-of-pipe timestamp (no barrier), so a kind owns the time from its mark to the next one: the work
  // that finishes in between, including the tail of an earlier command that overlapped it. Times are read in wait_slot.
  enum : std::uint32_t { kCatDraw, kCatCompose, kCatDeinterlace, kCatFxaa, kCatPresent, kCatUpload, kCatCompute,
                         kCatReadback, kCatStartGap, kCatKinds };
  static constexpr std::uint32_t kCatPoints = 512;
  const bool catOn = std::getenv("GT4_CORE2_GPU_CATEGORIES") != nullptr;
  VkQueryPool catPool = VK_NULL_HANDLE;
  // IN-422: the window of the next texture decode (SetDecodeWindow).
  const std::uint8_t *windowMemory = nullptr;
  std::uint32_t windowFirstPage = 0, windowPages = 0;

  // Copies the decode window's pages into the staging ring (wrapping at the end of GS memory) and points `glm` at
  // them; returns the push words of the window (base word, word count), {0, 0} without a window.
  bool bind_window(VkDescriptorBufferInfo &glm, std::uint32_t &baseWord, std::uint32_t &words, std::string &error) {
    baseWord = 0;
    words = 0;
    if (!windowMemory) return true;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(windowPages) * 8192u;
    const VkDeviceSize align = std::max<VkDeviceSize>(limits.minStorageBufferOffsetAlignment, 16);
    VkDeviceSize offset = 0;
    if (!ring(staging, bytes, align, offset, error)) return false;
    std::uint8_t *out = static_cast<std::uint8_t *>(staging.mapped) + offset;
    const std::uint32_t first = windowFirstPage % 512u;
    const std::uint32_t head = std::min(windowPages, 512u - first);
    std::memcpy(out, windowMemory + static_cast<std::size_t>(first) * 8192u, static_cast<std::size_t>(head) * 8192u);
    if (head < windowPages) {
      std::memcpy(out + static_cast<std::size_t>(head) * 8192u, windowMemory,
                  static_cast<std::size_t>(windowPages - head) * 8192u);
    }
    glm = VkDescriptorBufferInfo{staging.buffer, offset, bytes};
    baseWord = first * 2048u;
    words = windowPages * 2048u;
    windowMemory = nullptr;
    stats.windowDecodes++;
    stats.windowBytes += bytes;
    return true;
  }
  // B4: one pipeline statistics query per slot over its whole command buffer (primitives, fragment and compute
  // invocations), where the device enables them.
  VkQueryPool statistics = VK_NULL_HANDLE;
  double timestampNs = 0.0;  // nanoseconds per timestamp tick (0: no timestamps)
  std::uint32_t slot = 0;           // the slot being recorded
  std::uint32_t lastSubmitted = kMaxSlots - 1;  // the slot submitted last (set to slotCount - 1 at Init)
  VkCommandBuffer cmd = VK_NULL_HANDLE;  // slots[slot].cmd
  VkFence fence = VK_NULL_HANDLE;
  bool recording = false;
  // IN-410: the recording is submitted every `submitEvery` draw batches, so the GPU draws while the CPU records the
  // rest of the frame and a readback in mid-frame (GT4's flare reads) waits only for the work after the last
  // submission. GT4_CORE2_SUBMIT_EVERY=<batches>, 0 = only when the CPU needs a result or at the end of the frame.
  // S4: 32 since more submissions can be in flight (was 96 with two slots). TASK-134: 64 on a discrete GPU, set at Init (George V
  // Paris lap on the RX 9060 XT: GS thread CPU -5% at 1x and -4% at 3x, frame -3% at 1x and the same at 3x; 128 made the 3x
  // frame 5% slower, the GPU starting later). An integrated GPU keeps 32 until it is measured.
  std::uint32_t submitEvery = [] {
    const char *text = std::getenv("GT4_CORE2_SUBMIT_EVERY");
    return text ? static_cast<std::uint32_t>(std::strtoul(text, nullptr, 10)) : kSubmitEveryAtInit;
  }();
  std::uint32_t batchesSinceSubmit = 0;
  // TASK-178 (O1 of docs/hw-renderer-spec/STUDY-FRAME-DOWNLOAD-COST.md): a waited submission (a readback: Reduce,
  // ReduceMany, ReadClut) drains the queue, and the GPU then idles until the next submission, which came `submitEvery`
  // batches later or at the VBlank flush (the flare and glare draws right after GT4's flare read). With
  // GT4_CORE2_SUBMIT_AFTER_WAIT=<K> the first mid-frame submission after a waited one comes after K batches; the cadence is
  // `submitEvery` again after it. Submission boundaries never change what is drawn. 0 = off (A/B); unset: decided at Init.
  std::uint32_t submitAfterWait = [] {
    const char *text = std::getenv("GT4_CORE2_SUBMIT_AFTER_WAIT");
    return text ? static_cast<std::uint32_t>(std::strtoul(text, nullptr, 10)) : kSubmitEveryAtInit;
  }();
  bool afterWait = false;  // the last submission was waited for, and none has followed it yet (the K rule applies)
  // TASK-182 step 3 (row 5 of STUDY-ASYNC-READBACK 9.1, O2): wait only for the newest submission's fence when the GS worker
  // has to wait for a readback, instead of the fence of every slot (see wait_newest for the in-order-queue argument).
  // GT4_CORE2_WAIT_NEWEST_FENCE=0 waits for every slot as before (A/B); unset: on, exact on any Vulkan queue.
  const bool waitNewestFence = [] {
    const char *text = std::getenv("GT4_CORE2_WAIT_NEWEST_FENCE");
    return text == nullptr || std::strcmp(text, "0") != 0;
  }();
  // TASK-182 step 3: a bounded spin on the read's fence before blocking on it (the study's O2; §10's "cost of one
  // vkWaitForFences wake-up against a spin"). GT4_HW_READBACK_SPIN=1 spins for kReadbackSpinUs, a larger number for that
  // many microseconds; 0 or unset blocks at once (A/B, default off).
  static constexpr std::uint32_t kReadbackSpinUs = 200;
  const std::uint32_t readbackSpinUs = [] {
    const char *text = std::getenv("GT4_HW_READBACK_SPIN");
    if (text == nullptr || text[0] == '\0' || std::strcmp(text, "0") == 0) return 0u;
    const long value = std::strtol(text, nullptr, 10);
    return value > 1 ? static_cast<std::uint32_t>(std::min<long>(value, 100000)) : kReadbackSpinUs;
  }();
  // TASK-178 diagnostics: a drain whose next submission has not been sent yet, the CPU time its wait ended and the GPU end of
  // the drained work (timestamp ticks, 0: none).
  bool drainOpen = false;
  std::chrono::steady_clock::time_point drainEndCpu{};
  std::uint64_t drainEndTick = 0;
  // IN-451: speculative depth rejection before texturing in the pixel shader (exact); GT4_CORE2_EARLY_Z=0 turns it
  // off (A/B, the shader's constant 103).
  bool earlyZ = std::getenv("GT4_CORE2_EARLY_Z") == nullptr || std::strcmp(std::getenv("GT4_CORE2_EARLY_Z"), "0") != 0;
  // IN-492: such a fragment is discarded before the interlock (the shader's constant 103 = 2). GT4_CORE2_EARLY_OUT=0: A/B.
  bool earlyOut = earlyZ && (std::getenv("GT4_CORE2_EARLY_OUT") == nullptr || std::strcmp(std::getenv("GT4_CORE2_EARLY_OUT"), "0") != 0);
  // The images a fragment may have lowered or overwritten since the last wait (record_draw).
  std::vector<std::uint64_t> mayLowerDepth;
  // Draw descriptor sets of this recording by their four images (a batch with the same targets reuses its set).
  std::unordered_map<DrawSetKey, VkDescriptorSet, DrawSetKeyHash> drawSets;
  // B4: images whose last use finished, kept for reuse by size instead of freed: every new image was a device memory
  // allocation and several driver calls, and core 2 replaces 5-6 targets and pooled textures per night VBlank.
  // GT4_CORE2_IMAGE_POOL=0 frees them at once (A/B).
  std::vector<Image> recycled;
  VkDeviceSize recycledBytes = 0;
  bool integrated = false;  // IN-431: VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
  bool recycleImages = std::getenv("GT4_CORE2_IMAGE_POOL") == nullptr || std::strcmp(std::getenv("GT4_CORE2_IMAGE_POOL"), "0") != 0;
  std::uint32_t nextSerial = 1;
  bool renderPassOpen = false;
  bool drewSinceBarrier = false;

  Buffer staging, vertices, uniforms, readback;
  // TASK-180 test4 (P1-B): the memory type each non-ring object took, for the placement line. glmType and clutType are
  // their own allocations, imageType the last type a CORE2 image used (they all ask for the same one).
  std::uint32_t glmType = UINT32_MAX, clutType = UINT32_MAX, imageType = UINT32_MAX;
  // IN-439: results of the reductions taken later (ReduceLater), kLaterSlots of up to kLaterWords words each, and the
  // submission that carries each one. The recording in progress becomes submission submitCount + 1.
  static constexpr std::uint32_t kLaterSlots = 64, kLaterWords = 1024;
  Buffer later;
  struct LaterReduce {
    std::uint64_t serial = 0;  // 0: free
    std::uint32_t count = 0;
  };
  LaterReduce laterReduces[kLaterSlots];
  std::uint32_t laterNext = 0;
  std::uint64_t submitCount = 0;

  std::vector<Image> images;
  std::vector<ImageId> freeIds;
  std::vector<Image> doomed;  // destroyed once the submission that may use them is complete

  Image atlas;
  std::uint32_t atlasShelfX = 0, atlasShelfY = 0, atlasShelfHeight = 0;
  Image display;
  // IN-405: deinterlace (hw_deinterlace_scaled.comp: 0 history[4], 4 woven, 5 previous output, 6 output) and FXAA
  // (hw_postfx_fxaa.comp: 0 source, 1 output), each with its own set layout; the images follow the display size.
  Image deinterlaceHistory[gpu::kGpuDeinterlaceHistory];
  Image deinterlaceWoven, deinterlaceOutput, fxaa;
  // TASK-182 R8: the image the post chain last produced (the display image, or with GT4_CORE2_DEINT_FIELD_COMPOSE=1 still
  // the display image: the deinterlace writes its result there). FXAA, the presentation resample and the host read use it.
  Image *post = &display;
  // TASK-182 R8: the history entry the next display merge composes into (PrepareDeinterlace), or null when the merge
  // writes the display image.
  Image *fieldTarget = nullptr;
  // IN-432: presentation resampling (core2_present_scale.comp: 0 source, 1 output); two passes ping-pong through
  // presentA and presentB, the last one writes presentOut.
  Image presentA, presentB, presentOut;
  VkDescriptorSetLayout presentSetLayout = VK_NULL_HANDLE;
  VkPipelineLayout presentLayout = VK_NULL_HANDLE;
  VkShaderModule presentModule = VK_NULL_HANDLE;
  VkPipeline presentPipeline = VK_NULL_HANDLE;
  VkDescriptorSetLayout deinterlaceSetLayout = VK_NULL_HANDLE, fxaaSetLayout = VK_NULL_HANDLE;
  VkPipelineLayout deinterlaceLayout = VK_NULL_HANDLE, fxaaLayout = VK_NULL_HANDLE;
  VkShaderModule deinterlaceModule = VK_NULL_HANDLE, fxaaModule = VK_NULL_HANDLE;
  VkPipeline deinterlacePipeline = VK_NULL_HANDLE, fxaaPipeline = VK_NULL_HANDLE;

  VkDescriptorSetLayout drawSetLayout = VK_NULL_HANDLE;
  VkPipelineLayout drawLayout = VK_NULL_HANDLE;
  VkRenderPass renderPass = VK_NULL_HANDLE;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  std::uint32_t framebufferWidth = 0, framebufferHeight = 0;
  VkShaderModule vertexModule = VK_NULL_HANDLE, fragmentModule = VK_NULL_HANDLE, interlockModule = VK_NULL_HANDLE;
  // IN-481: draws that read nothing back go through colour attachments: cheaper on GPUs whose storage writes are slow
  // (HD 620: about 2x, 3x with a texture). The pixels are the same. GT4_CORE2_ROP=0 / 1 forces it off / on (default: on an
  // integrated GPU); GT4_CORE2_ROP_FILLS=0 keeps the constant fills in compute (A/B).
  bool ropOn = false;
  bool dualSourceBlend = false;
  const bool ropFills = std::getenv("GT4_CORE2_ROP_FILLS") == nullptr || std::strcmp(std::getenv("GT4_CORE2_ROP_FILLS"), "0") != 0;
  VkShaderModule ropModule = VK_NULL_HANDLE, ropBlendModule = VK_NULL_HANDLE;
  // IN-482: blends without depth writes through the GPU's blending. It rounds where the GS truncates and clamps a factor
  // over 1, so it is not exact. Off unless GT4_CORE2_ROP_BLEND=1: on the HD 620 it gave only 3-4% of GPU time.
  const bool ropBlend = std::getenv("GT4_CORE2_ROP_BLEND") != nullptr && std::strcmp(std::getenv("GT4_CORE2_ROP_BLEND"), "0") != 0;
  VkRenderPass ropPassFrame = VK_NULL_HANDLE, ropPassBoth = VK_NULL_HANDLE;
  std::map<std::pair<VkImageView, VkImageView>, VkFramebuffer> ropFramebuffers;
  VkPipeline pipelines[3] = {};  // points, lines, triangles
  VkPipeline interlockTriangles = VK_NULL_HANDLE;
  // TASK-179: points are gl_PointSize = scale wide, which above one pixel needs largePoints. Without it (or with a largest
  // point size under the scale) pipelines[0] draws each point as two triangles (hw_vertex.vert, HWR_POINT_QUAD): a
  // triangle list reading the point's vertex record once per instance, drawn six vertices per point. The square is the
  // one point rasterization covers. GT4_CORE2_POINT_QUADS=1 / 0 forces quads / point sizes (A/B).
  bool pointQuads = false;
  VkShaderModule pointQuadModule = VK_NULL_HANDLE;
  // TASK-179: the readback buffer's size (kReadbackBytes unless the heap is small, plan_core2_rings).
  VkDeviceSize readbackBytes = kReadbackBytes;

  // IN-437: the interlocked triangle pipeline specialised per draw state. hw_pixel_uber.frag reads the state block
  // and branches on about 50 of its fields in every wave; with the fields as specialization constants the compiler
  // folds those branches (about 3x less fragment cost at 12x in the fill benchmark, IN-436). The source and
  // the values are the same, so a specialised draw is exactly the uber draw. Variants compile on a worker thread; a
  // batch whose variant is not ready yet draws with interlockTriangles. GT4_CORE2_SPECIALIZE=0 turns this off;
  // =sync compiles each variant on first use (deterministic, for the tests).
  enum class SpecMode { off, async, sync };
  SpecMode specMode = [] {
    const char *text = std::getenv("GT4_CORE2_SPECIALIZE");
    if (text && (std::strcmp(text, "0") == 0 || std::strcmp(text, "off") == 0)) return SpecMode::off;
    if (text && std::strcmp(text, "sync") == 0) return SpecMode::sync;
    return SpecMode::async;
  }();
  static constexpr std::uint32_t kSpecFieldCount = 30;
  // Diagnostics: GT4_CORE2_SPEC_FIELDS=<hex> specialises only the key slots whose bit is set (slot 30: the push word,
  // slot 31: the anisotropy of a mipmapped draw).
  const std::uint32_t specFieldMask = [] {
    const char *text = std::getenv("GT4_CORE2_SPEC_FIELDS");
    return text ? static_cast<std::uint32_t>(std::strtoul(text, nullptr, 16)) : 0xFFFFFFFFu;
  }();
  static constexpr std::size_t kMaxSpecPipelines = 2048;  // beyond this many draw states the uber pipeline draws
  // The specialised fields, then the push word, then the anisotropy (tex_origin_x bits 21-23) of a mipmapped draw.
  // Slot kSpecFieldCount + 2: the ROP word of a draw through colour attachments (rop_word), 0 for storage writes.
  using SpecKey = std::array<std::uint32_t, kSpecFieldCount + 3>;
  struct SpecKeyHash {
    std::size_t operator()(const SpecKey &key) const {
      std::uint64_t h = 1469598103934665603ull;
      for (std::uint32_t v : key) h = (h ^ v) * 1099511628211ull;
      return static_cast<std::size_t>(h);
    }
  };
  std::unordered_map<SpecKey, VkPipeline, SpecKeyHash> specLocal;  // GS thread: variants known to be ready
  std::unordered_set<SpecKey, SpecKeyHash> specSeen;               // GS thread: variants ready, compiling or queued
  std::mutex specMutex;  // guards the members below up to specThread, except the atomic
  std::condition_variable specWake;
  std::deque<SpecKey> specQueue;
  std::vector<std::pair<SpecKey, VkPipeline>> specDone;  // compiled, not yet moved into specLocal
  std::vector<VkPipeline> specAll;                       // every compiled variant (destroyed with the renderer)
  bool specStop = false;
  std::uint64_t specCompileNs = 0, specCompileMaxNs = 0, specFailures = 0;
  std::atomic<std::uint32_t> specDoneCount{0};
  std::uint32_t specDoneSeen = 0;  // GS thread: specDoneCount when specDone was last emptied
  std::thread specThread;
  // IN-489: GT4_HW_SHADER_CACHE=<folder> keeps the driver's compiled pipelines and the list of variants the game asked
  // for. The next session starts compiling that list at once, and a known one takes milliseconds instead of seconds: a
  // new install or update on the HD 620 drew with the uber pipeline for its first sessions, 13.7 ms of GPU per VBlank
  // instead of 8, while 90 variants took a minute of a core in every session.
  std::filesystem::path shaderCacheFolder;
  VkPipelineCache pipelineCache = VK_NULL_HANDLE;
  std::uint64_t shaderSetHash = 0;
  std::vector<SpecKey> specKnown;  // specMutex: every variant compiled or read from the file
  bool specKnownChanged = false;   // specMutex
  std::size_t specKnownFromFile = 0;
  // IN-494: GT4_CORE2_CLEAR_WHOLE=0 draws GT4's whole-target clears, =1 clears only what keeps every byte, =2 also a Z24
  // depth word with its top byte zero. The GS keeps that byte and nothing in GT4 shows it (frames identical, and the EE
  // never reads depth), but it is not the exact memory: unset, =2 at a scale above 1x and on integrated GPUs, else =1.
  const char *clearWholeText = std::getenv("GT4_CORE2_CLEAR_WHOLE");
  const int clearWholeSetting = clearWholeText ? std::atoi(clearWholeText) : -1;
  const bool clearWhole = clearWholeSetting != 0;
  bool clearTopZero = clearWholeSetting == 2;
  std::chrono::steady_clock::time_point specLastSave{};
  VkPhysicalDeviceProperties deviceProperties{};  // IN-507
  // TASK-170 (issues #187-#190 and an RX 6700 XT report on 1.3.0-rc13): some drivers end the process inside
  // vkCreateGraphicsPipelines, and a variant that does so would end every session that compiles it. Around each variant
  // compile the keys in flight are kept in <cache>/compiling.bin; a process that dies leaves the file, and the next start of
  // the renderer reads it (open_compile_guard): several keys in flight (a parallel prepare) make the next prepares compile
  // one variant at a time and name nobody; one key in flight is a SUSPECT the first time (suspects.bin, TASK-170c) and is
  // compiled again; the same key in flight at a second end is quarantined (quarantine.bin). A suspect whose compile ends
  // normally is cleared. A quarantined variant is never compiled again on this card and driver: its draws keep the uber
  // pipeline, which draws the same pixels. Two strikes, because an intermittent driver crash may end a run inside an innocent
  // variant (the RX 6700 XT report: seven variants with nothing in common). Another driver or card starts with an empty guard
  // (the files name the card, the driver and the shaders they were written for). The three files share one layout: this header,
  // then `count` keys of `keyWords` 32-bit words; `flags` matters in quarantine.bin only. An rc14 folder (no suspects.bin)
  // reads as no suspects, and its quarantine stays. Every key a start quarantines or suspects is logged by field name.
  struct GuardFileHeader {
    char magic[8] = {'G', 'T', '4', 'G', 'R', 'D', '1', '\0'};
    std::uint64_t shaderSet = 0;
    std::uint32_t vendorId = 0, deviceId = 0, driverVersion = 0;
    std::uint32_t keyWords = kSpecFieldCount + 3;
    std::uint32_t flags = 0;  // kGuardSerial: PrepareShaders compiles one variant at a time
    std::uint32_t count = 0;
  };
  static constexpr std::uint32_t kGuardSerial = 1u;
  std::mutex compilingMutex;
  std::vector<SpecKey> compiling;                        // compilingMutex: the keys inside vkCreateGraphicsPipelines now
  std::unordered_set<SpecKey, SpecKeyHash> quarantined;  // read at start, never compiled
  std::vector<SpecKey> suspects;                         // compilingMutex: keys that ended one run (suspects.bin), compiled again
  bool serialPrepare = false;
  // Tests only (GT4_CORE2_TEST_COMPILE_ABANDON=<n>): the n-th variant compile of this renderer leaves the guard files as a
  // process that died inside it would, and PrepareShaders stops.
  const unsigned long testAbandonAt = [] {
    const char *text = std::getenv("GT4_CORE2_TEST_COMPILE_ABANDON");
    return text && *text ? std::strtoul(text, nullptr, 10) : 0ul;
  }();
  std::atomic<unsigned long> compileStarts{0};
  std::atomic<bool> abandoned{false};

  VkDescriptorSetLayout computeSetLayout = VK_NULL_HANDLE;
  VkPipelineLayout computeLayout = VK_NULL_HANDLE;
  VkShaderModule patchModule = VK_NULL_HANDLE, reduceModule = VK_NULL_HANDLE, toneModule = VK_NULL_HANDLE,
                 composeModule = VK_NULL_HANDLE;
  // IN-442: constant fills (core2_fill.comp). GT4_CORE2_COMPUTE_FILLS=0 draws them as sprites (A/B).
  VkShaderModule fillModule = VK_NULL_HANDLE;
  VkPipeline fillPipeline = VK_NULL_HANDLE;
  const bool computeFills = std::getenv("GT4_CORE2_COMPUTE_FILLS") == nullptr ||
                            std::strcmp(std::getenv("GT4_CORE2_COMPUTE_FILLS"), "0") != 0;
  VkPipeline patchPipeline = VK_NULL_HANDLE, reducePipeline = VK_NULL_HANDLE, tonePipeline = VK_NULL_HANDLE,
             composePipeline = VK_NULL_HANDLE;

  // Texture decode from the GLM (hw_glm_decode.comp): 0 GLM, 1 address tables, 2 palettes, 3 texels, 5 GPU CLUT.
  Buffer glmTables;
  // IN-408: the GPU CLUT (512 halfwords, device local), the CT32 inverse in-page table, the target CLUT kernel.
  VkBuffer clut = VK_NULL_HANDLE;
  VkDeviceMemory clutMemory = VK_NULL_HANDLE;
  Buffer inverseCt32;
  VkShaderModule clutTargetModule = VK_NULL_HANDLE, materializeModule = VK_NULL_HANDLE;
  VkPipeline clutTargetPipeline = VK_NULL_HANDLE, materializePipeline = VK_NULL_HANDLE;
  VkBuffer glm = VK_NULL_HANDLE;
  VkDeviceMemory glmMemory = VK_NULL_HANDLE;
  VkDeviceSize glmTableBytes = 0;
  VkDescriptorSetLayout decodeSetLayout = VK_NULL_HANDLE;
  VkPipelineLayout decodeLayout = VK_NULL_HANDLE;
  VkShaderModule decodeModule = VK_NULL_HANDLE, patchGlmModule = VK_NULL_HANDLE;
  VkPipeline decodePipeline = VK_NULL_HANDLE, patchGlmPipeline = VK_NULL_HANDLE;

  VkDescriptorPool transientPool = VK_NULL_HANDLE;
  std::uint32_t transientSetsUsed = 0;

  Core2GpuStats stats;

  // ---- Memory and objects ----------------------------------------------------------------------------
  bool memory_type(std::uint32_t bits, VkMemoryPropertyFlags wanted, VkMemoryPropertyFlags preferred,
                   std::uint32_t &index) const {
    for (int pass = 0; pass < 2; ++pass) {
      const VkMemoryPropertyFlags need = pass == 0 ? (wanted | preferred) : wanted;
      for (std::uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & need) == need) {
          index = i;
          return true;
        }
      }
    }
    return false;
  }

  bool make_buffer(Buffer &out, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags preferred,
                   std::string &error) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!check(gt4_hwd_vkCreateBuffer(device, &info, nullptr, &out.buffer), "vkCreateBuffer", error)) return false;
    VkMemoryRequirements requirements{};
    gt4_hwd_vkGetBufferMemoryRequirements(device, out.buffer, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    if (!memory_type(requirements.memoryTypeBits,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, preferred,
                     allocate.memoryTypeIndex)) {
      error = "no host-visible memory type for a CORE2 buffer";
      return false;
    }
    if (!check(gt4_hwd_vkAllocateMemory(device, &allocate, nullptr, &out.memory), "vkAllocateMemory", error)) return false;
    if (!check(gt4_hwd_vkBindBufferMemory(device, out.buffer, out.memory, 0), "vkBindBufferMemory", error)) return false;
    if (!check(gt4_hwd_vkMapMemory(device, out.memory, 0, size, 0, &out.mapped), "vkMapMemory", error)) return false;
    out.size = size;
    out.type = allocate.memoryTypeIndex;
    return true;
  }

  // TASK-179: the sizes of the staging, vertex, uniform and readback buffers for `shares` recordings, planned from the
  // heap the upload rings live in (its VK_EXT_memory_budget budget when the profile has one): today's sizes wherever they
  // fit in a quarter of it (gt4::hw::plan_core2_rings). GT4_CORE2_RINGS=fixed keeps today's sizes whatever the heap (A/B);
  // GT4_CORE2_RINGS=<MiB> plans for a budget of that many MiB instead (a small device's sizes on a large desktop).
  bool plan_rings(const gt4::hw::DeviceProfile &profile, VkBufferUsageFlags usage, VkMemoryPropertyFlags preferred,
                  std::uint32_t shares, gt4::hw::RingPlan &plan, std::string &error) const {
    // The memory type a ring buffer takes, from a small buffer of the same usage.
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = 4096;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer probe = VK_NULL_HANDLE;
    if (!check(gt4_hwd_vkCreateBuffer(device, &info, nullptr, &probe), "vkCreateBuffer", error)) return false;
    VkMemoryRequirements requirements{};
    gt4_hwd_vkGetBufferMemoryRequirements(device, probe, &requirements);
    gt4_hwd_vkDestroyBuffer(device, probe, nullptr);
    std::uint32_t type = 0;
    if (!memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     preferred, type)) {
      error = "no host-visible memory type for a CORE2 buffer";
      return false;
    }
    const std::uint32_t heap = memoryProperties.memoryTypes[type].heapIndex;
    gt4::hw::RingRequest request;
    request.heap_size = memoryProperties.memoryHeaps[heap].size;
    request.heap_budget = profile.probed && profile.memory_budget && heap < profile.heap_count ? profile.heaps[heap].budget : 0;
    request.shares = shares;
    request.staging_share = kStagingBytes / 8u;
    request.vertex_share = kVertexBytes / 8u;
    request.uniform_share = kUniformBytes / 8u;
    request.readback = kReadbackBytes;
    request.other = static_cast<std::uint64_t>(kLaterSlots) * kLaterWords * 4u;
    if (const char *rings = std::getenv("GT4_CORE2_RINGS")) {
      if (std::strcmp(rings, "fixed") == 0) {
        request.fixed = true;
      } else if (std::atoll(rings) > 0) {
        request.forced_budget = static_cast<std::uint64_t>(std::atoll(rings)) << 20;
      }
    }
    plan = gt4::hw::plan_core2_rings(request);
    std::fprintf(stderr,
                 "[core2] rings: %u shares, staging %llu MiB, vertex %llu MiB, uniform %llu MiB, readback %llu MiB (%llu MiB "
                 "mapped); heap %u of %llu MiB, budget %llu MiB, allowance %llu MiB (%s): %s\n",
                 plan.shares, static_cast<unsigned long long>(plan.staging >> 20),
                 static_cast<unsigned long long>(plan.vertex >> 20), static_cast<unsigned long long>(plan.uniform >> 20),
                 static_cast<unsigned long long>(plan.readback >> 20), static_cast<unsigned long long>(plan.total() >> 20), heap,
                 static_cast<unsigned long long>(request.heap_size >> 20), static_cast<unsigned long long>(request.heap_budget >> 20),
                 static_cast<unsigned long long>(plan.allowance >> 20), plan.basis,
                 plan.reduced ? "reduced to fit" : "today's sizes");
    return true;
  }

  // Reserves `bytes` of a ring for this recording, aligned; submits and waits when the ring is full.
  // S4: one recording's share of a ring, a multiple of 4 KiB so every share starts aligned for any buffer offset rule
  // (dynamic uniform and storage offsets); size / slotCount alone is not aligned for 6 slots.
  VkDeviceSize ring_share(const Buffer &buffer) const {
    return buffer.size / slotCount / 4096u * 4096u;
  }

  bool ring(Buffer &buffer, VkDeviceSize bytes, VkDeviceSize alignment, VkDeviceSize &offset, std::string &error) {
    if (bytes > buffer.size) {
      error = "CORE2 ring request larger than the ring";
      return false;
    }
    const VkDeviceSize half = ring_share(buffer);  // this recording's share
    if (bytes > half) {
      error = "CORE2 ring request larger than a recording's share of the ring";
      return false;
    }
    VkDeviceSize at = (buffer.used + alignment - 1) / alignment * alignment;
    if (at + bytes > half) {
      ++stats.ringFullSubmits;  // TASK-180 test4 (P1-C): a ring ran out of its share mid-recording
      if (!submit(false, error) || !begin_recording(error)) return false;
      at = 0;
    }
    buffer.used = at + bytes;
    // TASK-180 test4 (P1-C): the high-water mark of each recording ring, reset by the data-path print. Only the three
    // per-share rings are measured.
    if (&buffer == &staging) {
      stats.ringHighStaging = std::max(stats.ringHighStaging, static_cast<std::uint64_t>(buffer.used));
    } else if (&buffer == &vertices) {
      stats.ringHighVertex = std::max(stats.ringHighVertex, static_cast<std::uint64_t>(buffer.used));
    } else if (&buffer == &uniforms) {
      stats.ringHighUniform = std::max(stats.ringHighUniform, static_cast<std::uint64_t>(buffer.used));
    }
    offset = buffer.base + at;
    return true;
  }

  // S2: device memory for images taken from 64 MiB blocks instead of one allocation per image. vkAllocateMemory is a
  // kernel call: at GT4's race start core 2 created 40-160 images per VBlank (texture decodes of new textures), which
  // made 20-65 ms spikes. Images over 16 MiB keep an allocation of their own; empty blocks are kept for reuse.
  // GT4_CORE2_IMAGE_BLOCKS=0 gives every image its own allocation (A/B).
  static constexpr VkDeviceSize kImageBlockBytes = static_cast<VkDeviceSize>(64) << 20;
  static constexpr VkDeviceSize kOwnAllocationBytes = static_cast<VkDeviceSize>(16) << 20;
  struct MemoryBlock {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::uint32_t type = 0;
    std::map<VkDeviceSize, VkDeviceSize> free;  // offset -> size of each free range
  };
  std::vector<MemoryBlock> imageBlocks;
  bool imageBlocksOn =
      std::getenv("GT4_CORE2_IMAGE_BLOCKS") == nullptr || std::strcmp(std::getenv("GT4_CORE2_IMAGE_BLOCKS"), "0") != 0;

  // First fit in the blocks of memory type `type`; a new block when none has room.
  bool take_block_memory(const VkMemoryRequirements &requirements, std::uint32_t type, Image &out, std::string &error) {
    const VkDeviceSize alignment = std::max<VkDeviceSize>(requirements.alignment, 1);
    for (int attempt = 0; attempt < 2; ++attempt) {
      for (std::size_t b = 0; b < imageBlocks.size(); ++b) {
        MemoryBlock &block = imageBlocks[b];
        if (block.type != type) continue;
        for (auto range = block.free.begin(); range != block.free.end(); ++range) {
          const VkDeviceSize start = range->first, end = range->first + range->second;
          const VkDeviceSize at = (start + alignment - 1) / alignment * alignment;
          if (at + requirements.size > end) continue;
          block.free.erase(range);
          if (at > start) block.free[start] = at - start;
          if (at + requirements.size < end) block.free[at + requirements.size] = end - (at + requirements.size);
          out.memory = block.memory;
          out.block = static_cast<std::int32_t>(b);
          out.offset = at;
          return true;
        }
      }
      MemoryBlock block;
      block.type = type;
      VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      allocate.allocationSize = kImageBlockBytes;
      allocate.memoryTypeIndex = type;
      if (!check(gt4_hwd_vkAllocateMemory(device, &allocate, nullptr, &block.memory), "vkAllocateMemory", error)) {
        return false;
      }
      block.free[0] = kImageBlockBytes;
      imageBlocks.push_back(std::move(block));
      stats.imageBlocks++;
    }
    error = "CORE2 image does not fit an image memory block";
    return false;
  }

  // Returns a block range, merged with its free neighbours.
  void give_block_memory(std::int32_t blockIndex, VkDeviceSize offset, VkDeviceSize size) {
    if (blockIndex < 0 || static_cast<std::size_t>(blockIndex) >= imageBlocks.size()) return;
    std::map<VkDeviceSize, VkDeviceSize> &free = imageBlocks[static_cast<std::size_t>(blockIndex)].free;
    auto next = free.lower_bound(offset);
    if (next != free.end() && offset + size == next->first) {
      size += next->second;
      next = free.erase(next);
    }
    if (next != free.begin()) {
      auto previous = std::prev(next);
      if (previous->first + previous->second == offset) {
        previous->second += size;
        return;
      }
    }
    free[offset] = size;
  }

  bool make_image(Image &out, std::uint32_t nativeWidth, std::uint32_t nativeHeight, std::uint32_t imageScale,
                  std::string &error) {
    out = Image{};
    out.nativeWidth = std::max<std::uint32_t>(nativeWidth, 1u);
    out.nativeHeight = std::max<std::uint32_t>(nativeHeight, 1u);
    out.scale = imageScale;
    out.width = out.nativeWidth * imageScale;
    out.height = out.nativeHeight * imageScale;
    if (out.width > limits.maxImageDimension2D || out.height > limits.maxImageDimension2D) {
      error = "CORE2 image larger than the device limit";
      return false;
    }
    out.serial = nextSerial++;
    // B4: a finished image of the same size is taken over (cleared below like a new one).
    for (std::size_t i = recycled.size(); i-- > 0;) {
      if (recycled[i].width != out.width || recycled[i].height != out.height) continue;
      out.image = recycled[i].image;
      out.memory = recycled[i].memory;
      out.view = recycled[i].view;
      out.ropView = recycled[i].ropView;
      out.bytes = recycled[i].bytes;
      out.block = recycled[i].block;
      out.offset = recycled[i].offset;
      recycledBytes -= recycled[i].bytes;
      recycled.erase(recycled.begin() + static_cast<std::ptrdiff_t>(i));
      out.alive = true;
      stats.recycledImages++;
      return clear_new_image(out, error);
    }
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R32_UINT;
    info.extent = {out.width, out.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    const VkFormat ropFormats[2] = {VK_FORMAT_R32_UINT, VK_FORMAT_R8G8B8A8_UNORM};
    VkImageFormatListCreateInfo formatList{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    formatList.viewFormatCount = 2;
    formatList.pViewFormats = ropFormats;
    if (ropOn) {
      info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
      info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      info.pNext = &formatList;
    }
    if (!check(gt4_hwd_vkCreateImage(device, &info, nullptr, &out.image), "vkCreateImage", error)) return false;
    VkMemoryRequirements requirements{};
    gt4_hwd_vkGetImageMemoryRequirements(device, out.image, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    if (!memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, allocate.memoryTypeIndex)) {
      error = "no device-local memory type for a CORE2 image";
      return false;
    }
    imageType = allocate.memoryTypeIndex;
    if (imageBlocksOn && requirements.size <= kOwnAllocationBytes) {
      if (!take_block_memory(requirements, allocate.memoryTypeIndex, out, error)) return false;
    } else {
      if (!check(gt4_hwd_vkAllocateMemory(device, &allocate, nullptr, &out.memory), "vkAllocateMemory", error)) return false;
      stats.imagesAllocated++;
    }
    if (!check(gt4_hwd_vkBindImageMemory(device, out.image, out.memory, out.offset), "vkBindImageMemory", error)) {
      return false;
    }
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = out.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R32_UINT;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (!check(gt4_hwd_vkCreateImageView(device, &view, nullptr, &out.view), "vkCreateImageView", error)) return false;
    if (ropOn) {
      view.format = VK_FORMAT_R8G8B8A8_UNORM;
      if (!check(gt4_hwd_vkCreateImageView(device, &view, nullptr, &out.ropView), "vkCreateImageView", error)) return false;
    }
    out.bytes = requirements.size;
    out.alive = true;
    stats.imageBytes += out.bytes;
    stats.imagesCreated++;
    return clear_new_image(out, error);
  }

  // UNDEFINED -> GENERAL and zero, in this recording (a recycled image's old content is discarded the same way).
  bool clear_new_image(Image &out, std::string &error) {
    if (!begin_recording(error)) return false;
    end_render_pass();
    VkImageMemoryBarrier toGeneral{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toGeneral.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneral.image = out.image;
    toGeneral.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toGeneral.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    gt4_hw_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                0, nullptr, 1, &toGeneral);
    VkClearColorValue zero{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    gt4_hwd_vkCmdClearColorImage(cmd, out.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    stats.gpuClearBytes += static_cast<std::uint64_t>(out.bytes);  // P1-C: a new image is cleared whole
    full_barrier();
    return true;
  }

  // B4: a doomed image whose submission finished: kept for reuse (newest last; the oldest go past the budget).
  void retire(Image &image) {
    if (!image.alive) return;
    if (!recycleImages) {
      release(image);
      return;
    }
    recycled.push_back(image);
    recycledBytes += image.bytes;
    image = Image{};
    while (recycledBytes > (integrated ? kRecycledImageBytesIntegrated : kRecycledImageBytes) && !recycled.empty()) {
      recycledBytes -= recycled.front().bytes;
      release(recycled.front());
      recycled.erase(recycled.begin());
    }
  }

  void release(Image &image) {
    if (!image.alive) return;
    if (image.ropView) {
      for (auto it = ropFramebuffers.begin(); it != ropFramebuffers.end();) {
        if (it->first.first == image.ropView || it->first.second == image.ropView) {
          gt4_hwd_vkDestroyFramebuffer(device, it->second, nullptr);
          it = ropFramebuffers.erase(it);
        } else {
          ++it;
        }
      }
      gt4_hwd_vkDestroyImageView(device, image.ropView, nullptr);
    }
    if (image.view) gt4_hwd_vkDestroyImageView(device, image.view, nullptr);
    if (image.image) gt4_hwd_vkDestroyImage(device, image.image, nullptr);
    if (image.block >= 0) {
      give_block_memory(image.block, image.offset, image.bytes);  // S2
    } else if (image.memory) {
      gt4_hwd_vkFreeMemory(device, image.memory, nullptr);
    }
    stats.imageBytes -= std::min(stats.imageBytes, image.bytes);
    image = Image{};
  }

  void doom(Image &image) {
    if (!image.alive) return;
    doomed.push_back(image);
    image = Image{};
  }

  // Spec 8.8: the images a batch samples instead of the target it draws into, one per image size, kept from batch to
  // batch (GT4 draws its glow and luminance passes into the same few effect buffers every frame); the least recently
  // used one goes when a fifth size is needed. Only the copied rectangle of one is ever sampled.
  struct SelfCopy {
    Image image;
    std::uint64_t lastUse = 0;
  };
  static constexpr std::size_t kSelfCopyImages = 4;
  std::vector<SelfCopy> selfCopies;
  std::uint64_t selfCopyUses = 0;
  Image *self_copy_image(const Image &like, std::string &error) {
    for (SelfCopy &copy : selfCopies) {
      if (copy.image.width == like.width && copy.image.height == like.height) {
        copy.lastUse = ++selfCopyUses;
        return &copy.image;
      }
    }
    if (selfCopies.size() >= kSelfCopyImages) {
      auto oldest = std::min_element(selfCopies.begin(), selfCopies.end(),
                                     [](const SelfCopy &a, const SelfCopy &b) { return a.lastUse < b.lastUse; });
      doom(oldest->image);
      selfCopies.erase(oldest);
    }
    SelfCopy copy;
    if (!make_image(copy.image, like.nativeWidth, like.nativeHeight, like.scale, error)) return nullptr;
    copy.lastUse = ++selfCopyUses;
    selfCopies.push_back(copy);
    return &selfCopies.back().image;
  }

  // TASK-182 step 2a, row R1 of docs/hw-renderer-spec/STUDY-BARRIER-SCOPE.md: the self copy drawn inside the open
  // render pass. core2_self_copy.frag moves the sampled rectangle (the rectangle the transfer copies) into the pooled
  // image as a storage-image write from the fragment stage, ordered with fragment barriers (draw to draw), instead of a
  // vkCmdCopyImage that needs the pass closed. GT4_CORE2_SELF_COPY_DRAW=1 turns it on; the default is the transfer.
  //
  // The transfer stays the path wherever the draw cannot apply: the colour-attachment path (IN-481, `ropOn`: a sampled
  // image may be an attachment of the open pass), a rectangle the framebuffer scissor cannot hold, and
  // GT4_CORE2_LAZY_BARRIERS=0 (which orders every command with a full barrier, illegal inside a pass). Every decision
  // is counted in the TASK-182 trace.
  bool selfCopyDraw = std::getenv("GT4_CORE2_SELF_COPY_DRAW") != nullptr &&
                      std::strcmp(std::getenv("GT4_CORE2_SELF_COPY_DRAW"), "0") != 0;
  VkDescriptorSetLayout selfCopySetLayout = VK_NULL_HANDLE;  // 0 the source view, 1 the pooled copy view
  VkPipelineLayout selfCopyLayout = VK_NULL_HANDLE;          // no push constants: the scissor says what to copy
  VkShaderModule selfCopyVertexModule = VK_NULL_HANDLE, selfCopyModule = VK_NULL_HANDLE;
  VkPipeline selfCopyPipeline = VK_NULL_HANDLE;  // core2_self_copy.vert + .frag, the storage draw pass
  // True while a pooled self-copy image may have been written or read since the last full barrier. The draws of a pass
  // are ordered by its fragment barriers; a copy recorded when no pass is open (a compute or transfer command ended it)
  // records a full barrier first. full_barrier() clears it.
  bool copyImagesUnordered = false;

  VkShaderModule make_module(const std::uint32_t *words, std::size_t count, std::string &error) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = count * sizeof(std::uint32_t);
    info.pCode = words;
    VkShaderModule module = VK_NULL_HANDLE;
    check(gt4_hwd_vkCreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule", error);
    return module;
  }

  // `pointQuad` (TASK-179): the point pipeline without largePoints: the HWR_POINT_QUAD vertex stage, a triangle list, and
  // the vertex record read per instance.
  VkPipeline make_graphics(VkPrimitiveTopology topology, VkShaderModule fragment, std::string &error,
                           const VkSpecializationInfo *specialization = nullptr, std::uint32_t rop = 0, bool sheenEarlyOut = false,
                           bool pointQuad = false) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = pointQuad ? pointQuadModule : vertexModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";
    // IN-451: constant 103 (SPEC_EARLY_Z) follows GT4_CORE2_EARLY_Z on every fragment module (a module without the
    // constant ignores the entry).
    VkSpecializationMapEntry entries[64];
    std::uint8_t data[64 * 4];
    std::uint32_t count = 0;
    std::size_t bytes = 0;
    if (specialization) {
      count = specialization->mapEntryCount;
      bytes = specialization->dataSize;
      std::memcpy(entries, specialization->pMapEntries, count * sizeof(VkSpecializationMapEntry));
      std::memcpy(data, specialization->pData, bytes);
    }
    const std::uint32_t earlyZValue = sheenEarlyOut ? 3u : earlyOut ? 2u : earlyZ ? 1u : 0u;
    entries[count++] = {103u, static_cast<std::uint32_t>(bytes), 4};
    std::memcpy(data + bytes, &earlyZValue, 4);
    bytes += 4;
    const VkSpecializationInfo withEarlyZ{count, entries, bytes, data};
    stages[1].pSpecializationInfo = &withEarlyZ;
    VkVertexInputBindingDescription binding{0, sizeof(gpu::GpuVertex),
                                            pointQuad ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[8]{};
    attributes[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};   // S, T
    attributes[1] = {1, 0, VK_FORMAT_R8G8B8A8_UINT, 8};   // R, G, B, A
    attributes[2] = {2, 0, VK_FORMAT_R32_SFLOAT, 12};     // Q
    attributes[3] = {3, 0, VK_FORMAT_R16G16_UINT, 16};    // X, Y
    attributes[4] = {4, 0, VK_FORMAT_R32_UINT, 20};       // Z
    attributes[5] = {5, 0, VK_FORMAT_R16G16_UINT, 24};    // U, V
    attributes[6] = {6, 0, VK_FORMAT_R8_UNORM, 28};       // F
    attributes[7] = {7, 0, VK_FORMAT_R16_UINT, 30};       // TASK-168: the primitive's palette row + 1 (0: the batch's)
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 8;
    vertexInput.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = pointQuad ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : topology;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;  // TASK-179: every line is one pixel wide (no wideLines needed; GS lines are not widened at scale)
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 0;  // the fragment stage writes storage images
    VkPipelineColorBlendAttachmentState ropAttachments[2]{};
    if (rop != 0) {
      ropAttachments[0].colorWriteMask = rop_frame_mask(rop);
      ropAttachments[1].colorWriteMask = rop_depth_mask(rop);
      blend.attachmentCount = rop_depth_mask(rop) != 0 ? 2 : 1;
      blend.pAttachments = ropAttachments;
      if (const RopBlend *mode = rop_blend_mode(rop)) {
        ropAttachments[0].blendEnable = VK_TRUE;
        ropAttachments[0].srcColorBlendFactor = mode->source;
        ropAttachments[0].dstColorBlendFactor = mode->destination;
        ropAttachments[0].colorBlendOp = mode->op;
        ropAttachments[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;  // the GS does not blend alpha
        ropAttachments[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        ropAttachments[0].alphaBlendOp = VK_BLEND_OP_ADD;
      }
    }
    VkDynamicState dynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = drawLayout;
    info.renderPass = rop == 0 ? renderPass : rop_depth_mask(rop) != 0 ? ropPassBoth : ropPassFrame;
    VkPipeline pipeline = VK_NULL_HANDLE;
    check(gt4_hwd_vkCreateGraphicsPipelines(device, pipelineCache, 1, &info, nullptr, &pipeline),
          "vkCreateGraphicsPipelines", error);
    return pipeline;
  }

  // TASK-182 R1: the pipeline of the self copy draw (core2_self_copy.vert/.frag). No vertex input and no vertex buffer
  // (the three corners of a full-screen triangle come from gl_VertexIndex), no colour attachment (the fragment stage
  // stores the source word at its own pixel of the destination), dynamic viewport and scissor, and the draw pass's
  // render pass so that the draw runs inside a pass the targets' own draws opened.
  VkPipeline make_self_copy_pipeline(std::string &error) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = selfCopyVertexModule;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = selfCopyModule;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 0;
    VkDynamicState dynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = selfCopyLayout;
    info.renderPass = renderPass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    check(gt4_hwd_vkCreateGraphicsPipelines(device, pipelineCache, 1, &info, nullptr, &pipeline),
          "vkCreateGraphicsPipelines", error);
    return pipeline;
  }

  // IN-496: the key's ROP slot for a batch of an interlock GPU drawn with the plain module: its primitives share no pixel,
  // so they need no order among them, and a read-modify-write of one image costs half on the HD 620 without the interlock
  // (GT4's full-screen colour passes). A wait before and after keeps it in order with the other batches.
  static constexpr std::uint32_t kPlainWord = 0x80000000u;
  // Integrated GPUs only: on the RX 9060 XT at 12x the waits cost more than the interlock (GPU busy 23.7 to 24.7 ms).
  // GT4_CORE2_PLAIN_DISJOINT=0 or 1 overrides that (A/B).
  bool plainDisjoint = false;
  bool skipRoadSheen = false;  // IN-501
  // IN-503: the plain variant of the road's sheen: a pixel with no destination alpha gets no colour, so it leaves before it
  // samples or stores. Its alpha lane stays as it was instead of taking the texel's, which the next pass overwrites.
  // GT4_CORE2_SHEEN_EARLY=0 turns it off (A/B).
  static constexpr std::uint32_t kPlainSheenWord = 0xC0000000u;
  const bool sheenEarly = std::getenv("GT4_CORE2_SHEEN_EARLY") == nullptr || std::strcmp(std::getenv("GT4_CORE2_SHEEN_EARLY"), "0") != 0;

  // IN-481: the ROP word: bit 0 set, bits 1-4 the frame's RGBA lanes, bits 5-8 the depth word's lanes.
  static std::uint32_t rop_frame_mask(std::uint32_t word) { return (word >> 1) & 15u; }
  static std::uint32_t rop_depth_mask(std::uint32_t word) { return (word >> 5) & 15u; }
  // IN-482: bits 9-12 of the word: 0 no blend, else 1 + an entry of kRopBlends. ((A - B) * F) + D of the GS with A, B, D
  // in {Cs 0, Cd 1, zero 2} and F the shader's second output (As or FIX over 128). The forms the GPU's equation can hold.
  struct RopBlend {
    std::uint8_t a, b, d;
    VkBlendFactor source, destination;
    VkBlendOp op;
  };
  static constexpr RopBlend kRopBlends[] = {
      {0, 1, 1, VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA, VK_BLEND_OP_ADD},       // (Cs - Cd) F + Cd
      {1, 0, 0, VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA, VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_OP_ADD},       // (Cd - Cs) F + Cs
      {0, 2, 2, VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD},                       // Cs F
      {0, 2, 1, VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD},                        // Cs F + Cd
      {1, 2, 2, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_OP_ADD},                       // Cd F
      {1, 2, 0, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_OP_ADD},                        // Cd F + Cs
      {0, 1, 2, VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_OP_SUBTRACT},            // (Cs - Cd) F
      {1, 0, 2, VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_OP_REVERSE_SUBTRACT},    // (Cd - Cs) F
      {2, 0, 1, VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_REVERSE_SUBTRACT},           // Cd - Cs F
      {2, 1, 0, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC1_ALPHA, VK_BLEND_OP_SUBTRACT},                   // Cs - Cd F
  };
  static const RopBlend *rop_blend_mode(std::uint32_t word) {
    const std::uint32_t index = (word >> 9) & 15u;
    return index == 0 ? nullptr : &kRopBlends[index - 1];
  }
  // The ROP word of a draw state that reads neither word back, or 0: no blend, no DATE, no depth test but ALWAYS, FBMSK in
  // whole bytes, and what the alpha test decides per fragment is all or nothing. The lanes the shader would keep from the
  // old word are masked, so the result is the word it would store.
  static std::uint32_t rop_word(const std::uint32_t *f, bool allowBlend) {
    namespace g = gpu::gpufield;
    const std::uint32_t format = f[g::Format];
    if (format > 2u || f[g::DateOn] != 0) return 0;  // CT32, CT24, CT16
    // IN-499: a CT16 word replaces all it stores, so its lanes are not the FBMSK's: it qualifies only with nothing to keep.
    static const bool ct16Allowed = std::getenv("GT4_CORE2_ROP_CT16") == nullptr || std::strcmp(std::getenv("GT4_CORE2_ROP_CT16"), "0") != 0;
    const bool whole16 = format == 2u;
    if (whole16 && (!ct16Allowed || f[g::BlendOn] != 0 || (f[g::Fbmsk] & 0x80F8F8F8u) != 0)) return 0;
    std::uint32_t blendIndex = 0;
    // IN-494: with every lane kept the blend has no effect (GT4 writes depth through such a textured pass).
    if (f[g::BlendOn] != 0 && (f[g::Fbmsk] & (format == 0u ? 0xFFFFFFFFu : 0x00FFFFFFu)) != (format == 0u ? 0xFFFFFFFFu : 0x00FFFFFFu)) {
      // No PABE, no wrapping past 255, a factor the shader can give (not Ad, which the GPU would read over 255).
      if (!allowBlend || f[g::Pabe] != 0 || f[g::Colclamp] == 0 || f[g::BlendC] == 1u) return 0;
      if (f[g::BlendC] == 2u && f[g::Fix] > 128u) return 0;  // a gain over 1: the GPU clamps its factor
      for (std::uint32_t i = 0; i < sizeof kRopBlends / sizeof kRopBlends[0]; ++i) {
        if (kRopBlends[i].a == f[g::BlendA] && kRopBlends[i].b == f[g::BlendB] && kRopBlends[i].d == f[g::BlendD]) blendIndex = i + 1;
      }
      if (blendIndex == 0) return 0;
    }
    if (f[g::Ztest] != 0 && f[g::Ztst] != 1u) return 0;                    // HW_DEPTH_ALWAYS
    bool writeDepth = f[g::Ztest] != 0 && f[g::Zmsk] == 0;
    std::uint32_t frameMask = 15u;
    const std::uint32_t fbmsk = f[g::Fbmsk];
    for (std::uint32_t lane = 0; !whole16 && lane < (format == 0u ? 4u : 3u); ++lane) {
      const std::uint32_t kept = (fbmsk >> (8u * lane)) & 0xFFu;
      if (kept == 0xFFu) {
        frameMask &= ~(1u << lane);
      } else if (kept != 0u) {
        return 0;
      }
    }
    if (format == 1u && f[g::KeepTopByte] != 0) frameMask &= 7u;
    if (f[g::Ate] != 0 && f[g::Atst] != 1u) {  // not ALWAYS
      const std::uint32_t afail = f[g::Afail];
      if (f[g::Atst] == 0u) {  // NEVER: every fragment takes the fail path
        if (afail == 0u) return 0;
        if (afail == 2u) {  // ZB_ONLY
          frameMask = 0;
        } else {  // FB_ONLY; RGB_ONLY keeps the alpha lane of CT32 and acts as FB_ONLY otherwise
          writeDepth = false;
          if (afail == 3u && format == 0u) frameMask &= 7u;
        }
      } else if (afail == 1u && !writeDepth) {
        // A failing fragment still writes the whole frame word: the test decides nothing here.
      } else if (afail != 0u) {
        return 0;  // the fail path writes other lanes than the pass path
      }
    }
    std::uint32_t depthMask = 0;
    if (writeDepth) depthMask = f[g::Zformat] == 0u ? 15u : f[g::Zformat] == 1u ? 7u : 3u;
    if (frameMask == 0 && depthMask == 0) return 0;
    if (blendIndex != 0 && (depthMask != 0 || frameMask == 0)) return 0;  // two outputs for one attachment: the frame alone
    return 1u | frameMask << 1 | depthMask << 5 | blendIndex << 9;
  }

  // IN-437: the state block fields that hw_pixel_uber.frag reads as specialization constants (constant id = 1 + the
  // field index), in key order.
  static constexpr std::uint32_t kSpecFields[kSpecFieldCount] = {
      gpu::gpufield::Format,  gpu::gpufield::Fba,   gpu::gpufield::Dither,    gpu::gpufield::Ate,
      gpu::gpufield::Atst,    gpu::gpufield::Afail, gpu::gpufield::DateOn,    gpu::gpufield::Datm,
      gpu::gpufield::Ztest,   gpu::gpufield::Ztst,  gpu::gpufield::Zmsk,      gpu::gpufield::Zformat,
      gpu::gpufield::BlendOn, gpu::gpufield::Pabe,  gpu::gpufield::BlendA,    gpu::gpufield::BlendB,
      gpu::gpufield::BlendC,  gpu::gpufield::BlendD, gpu::gpufield::Colclamp, gpu::gpufield::TexOn,
      gpu::gpufield::Tfx,     gpu::gpufield::Tcc,   gpu::gpufield::TexelKind, gpu::gpufield::Aem,
      gpu::gpufield::FogOn,   gpu::gpufield::TexFst, gpu::gpufield::TexLinear, gpu::gpufield::WrapS,
      gpu::gpufield::WrapT,   gpu::gpufield::KeepTopByte};

  // IN-437: the key of a draw state. Fields the draw cannot reach are zeroed so equal draws share one variant: the
  // texture fields without TexOn, the blend selectors without BlendOn, the alpha test fields without Ate, DATM
  // without DATE, the depth fields without Ztest, dither unless CT16, the kept top byte unless CT24; of the push word
  // bit 29 (read by the vertex stage only), the texture bits without TexOn, the position depth without Ztest and the
  // fine dither without dither. The shader never reads a zeroed value in that state.
  static SpecKey spec_key(const std::uint32_t *fields, std::uint32_t pushWord) {
    namespace g = gpu::gpufield;
    std::uint32_t f[g::Count] = {};
    for (std::uint32_t index : kSpecFields) f[index] = fields[index];
    std::uint32_t push = pushWord & ~0x20000000u;
    if (f[g::TexOn] == 0) {
      f[g::Tfx] = f[g::Tcc] = f[g::TexelKind] = f[g::Aem] = f[g::TexFst] = f[g::TexLinear] = f[g::WrapS] = f[g::WrapT] = 0;
      push = (push & ~(0x004FFFFFu | 0x01000000u | 0x00800000u | 0x04000000u | 0x80000000u)) | 1u;  // not SCANMSK
    }
    if (f[g::BlendOn] == 0) f[g::Pabe] = f[g::BlendA] = f[g::BlendB] = f[g::BlendC] = f[g::BlendD] = 0;
    if (f[g::Ate] == 0) f[g::Atst] = f[g::Afail] = 0;
    if (f[g::DateOn] == 0) f[g::Datm] = 0;
    if (f[g::Ztest] == 0) {
      f[g::Ztst] = f[g::Zmsk] = f[g::Zformat] = 0;
      push &= ~0x10000000u;
    }
    if (f[g::Format] != 2u) f[g::Dither] = 0;  // HW_FORMAT_CT16
    if (f[g::Dither] == 0) push &= ~0x40000000u;
    if (f[g::Format] != 1u) f[g::KeepTopByte] = 0;  // HW_FORMAT_CT24
    SpecKey key{};
    for (std::uint32_t i = 0; i < kSpecFieldCount; ++i) key[i] = f[kSpecFields[i]];
    key[kSpecFieldCount] = push;
    key[kSpecFieldCount + 1] = (push & 0x00800000u) != 0 ? (fields[g::TexOriginX] >> 21) & 7u : 0u;
    return key;
  }

  // Compiles one variant (on the worker thread, or on the GS thread in sync mode). Key slots outside specFieldMask
  // (diagnostics, GT4_CORE2_SPEC_FIELDS) stay dynamic: the shader reads them from the state block.
  VkPipeline make_specialised(const SpecKey &key, std::string &error) {
    std::uint32_t values[kSpecFieldCount + 2];
    VkSpecializationMapEntry entries[kSpecFieldCount + 2];
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < kSpecFieldCount + 2; ++i) {
      if ((specFieldMask >> i & 1u) == 0) continue;
      values[count] = key[i];
      entries[count] = {i == kSpecFieldCount ? 100u : i == kSpecFieldCount + 1 ? 101u : 1u + kSpecFields[i], 4 * count, 4};
      ++count;
    }
    VkSpecializationInfo info{count, entries, 4 * count, values};
    const std::uint32_t word = key[kSpecFieldCount + 2];
    const bool sheen = word == kPlainSheenWord;
    const std::uint32_t rop = word == kPlainWord || sheen ? 0u : word;
    return make_graphics(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, word == kPlainWord || sheen ? fragmentModule : rop != 0 ? (rop_blend_mode(rop) ? ropBlendModule : ropModule) : interlock ? interlockModule : fragmentModule,
                         error, &info, rop, sheen);  // the driver caches
  }

  // IN-489: every shader of a variant, so that a list written by another build is not used by this one.
  static std::uint64_t hash_shader_words(std::uint64_t h, const std::uint32_t *words, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) h = (h ^ words[i]) * 1099511628211ull;
    return h;
  }

  static std::vector<std::uint8_t> read_whole_file(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) bytes.clear();
    return bytes;
  }

  // Written under another name and renamed: a session closed halfway leaves the previous file.
  static void write_whole_file(const std::filesystem::path &path, const void *first, std::size_t firstSize, const void *second,
                               std::size_t secondSize) {
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
      std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
      file.write(static_cast<const char *>(first), static_cast<std::streamsize>(firstSize));
      if (secondSize != 0) file.write(static_cast<const char *>(second), static_cast<std::streamsize>(secondSize));
      if (!file) return;
    }
    std::error_code ignored;
    std::filesystem::rename(temporary, path, ignored);
  }

  struct VariantFileHeader {
    char magic[8] = {'G', 'T', '4', 'V', 'A', 'R', 'S', '1'};
    std::uint64_t shaderSet = 0;
    std::uint32_t keyWords = kSpecFieldCount + 3;
    std::uint32_t count = 0;
  };

  void open_shader_cache() {
    // Read as Windows holds it: in the narrow form an accented folder (C:\\Users\\Jo\u00e3o) is in the ANSI code page, not UTF-8.
#ifdef _WIN32
    const wchar_t *folder = _wgetenv(L"GT4_HW_SHADER_CACHE");
#else
    const char *folder = std::getenv("GT4_HW_SHADER_CACHE");  // TASK-180: UTF-8 paths outside Windows
#endif
    if (!folder || !*folder) return;
    std::error_code ignored;
    shaderCacheFolder = std::filesystem::path(folder);
    std::filesystem::create_directories(shaderCacheFolder, ignored);
    namespace sh = hwgs::shaders;
    std::uint64_t h = 1469598103934665603ull;
    h = hash_shader_words(h, sh::hw_vertex_core2_words, sh::hw_vertex_core2_word_count);
    h = hash_shader_words(h, sh::hw_pixel_uber_core2_words, sh::hw_pixel_uber_core2_word_count);
    h = hash_shader_words(h, sh::hw_pixel_uber_core2_interlock_words, sh::hw_pixel_uber_core2_interlock_word_count);
    h = hash_shader_words(h, sh::hw_pixel_uber_core2_rop_words, sh::hw_pixel_uber_core2_rop_word_count);
    h = hash_shader_words(h, sh::hw_pixel_uber_core2_rop_blend_words, sh::hw_pixel_uber_core2_rop_blend_word_count);
    shaderSetHash = h;
    // A cache from another driver or card is refused by the driver through its own header: it then starts empty.
    const std::vector<std::uint8_t> compiled = read_whole_file(shaderCacheFolder / "pipelines.bin");
    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    info.initialDataSize = compiled.size();
    info.pInitialData = compiled.empty() ? nullptr : compiled.data();
    if (gt4_hwd_vkCreatePipelineCache(device, &info, nullptr, &pipelineCache) != VK_SUCCESS) {
      info.initialDataSize = 0;
      info.pInitialData = nullptr;
      if (gt4_hwd_vkCreatePipelineCache(device, &info, nullptr, &pipelineCache) != VK_SUCCESS) pipelineCache = VK_NULL_HANDLE;
    }
    std::fprintf(stderr, "[core2] shader cache: %zu KiB of compiled pipelines\n", compiled.size() / 1024u);
    open_compile_guard();  // TASK-170
  }

  // After every pipeline the game may need exists: the variants of the last sessions go to the worker before any draw.
  void queue_known_variants() {
    if (shaderCacheFolder.empty() || specMode != SpecMode::async) return;
    const char *preparing = std::getenv("GT4_HW_SHADER_PREPARE");  // IN-507: PrepareShaders compiles them itself
    if (preparing && *preparing) return;
    const std::vector<std::uint8_t> bytes = read_whole_file(shaderCacheFolder / "variants.bin");
    VariantFileHeader header;
    const VariantFileHeader wanted;
    if (bytes.size() < sizeof header) return;
    std::memcpy(&header, bytes.data(), sizeof header);
    if (std::memcmp(header.magic, wanted.magic, sizeof header.magic) != 0 || header.shaderSet != shaderSetHash ||
        header.keyWords != wanted.keyWords || bytes.size() != sizeof header + std::size_t{header.count} * sizeof(SpecKey)) {
      return;
    }
    std::lock_guard<std::mutex> lock(specMutex);
    for (std::uint32_t i = 0; i < header.count && specSeen.size() < kMaxSpecPipelines; ++i) {
      SpecKey key;
      std::memcpy(key.data(), bytes.data() + sizeof header + i * sizeof(SpecKey), sizeof(SpecKey));
      const std::uint32_t rop = key[kSpecFieldCount + 2];
      if (rop != 0 && rop != kPlainWord && rop != kPlainSheenWord && (!ropOn || (rop_blend_mode(rop) && ropBlendModule == VK_NULL_HANDLE))) continue;
      if (!specSeen.insert(key).second) continue;
      specQueue.push_back(key);
      specKnown.push_back(key);
    }
    specKnownFromFile = specKnown.size();
    std::fprintf(stderr, "[core2] shader cache: %zu variants of the last sessions to prepare\n", specKnownFromFile);
    if (!specQueue.empty()) specThread = std::thread([this] { spec_worker(); });
  }

  // IN-507: what PrepareShaders last finished for. The shaders, the list and everything that decides the variants of this
  // card must match, so an update, a new driver or another card asks for a new compile.
  struct PreparedStamp {
    char magic[8] = {'G', 'T', '4', 'P', 'R', 'E', 'P', '1'};
    std::uint64_t shaderSet = 0;
    std::uint64_t list = 0;
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::uint32_t driverVersion = 0;
    std::uint32_t paths = 0;
    std::uint8_t cacheUuid[VK_UUID_SIZE] = {};
  };

  PreparedStamp prepared_stamp() const {
    PreparedStamp stamp;
    stamp.shaderSet = shaderSetHash;
    stamp.list = hash_shader_words(1469598103934665603ull, kKnownVariantWords, std::size(kKnownVariantWords));
    stamp.vendorId = deviceProperties.vendorID;
    stamp.deviceId = deviceProperties.deviceID;
    stamp.driverVersion = deviceProperties.driverVersion;
    stamp.paths = (ropOn ? 1u : 0u) | (plainDisjoint ? 2u : 0u) | (interlock ? 4u : 0u) | (earlyOut ? 8u : 0u) | (earlyZ ? 16u : 0u) |
                  (ropBlendModule ? 32u : 0u) | (sheenEarly ? 64u : 0u);
    std::memcpy(stamp.cacheUuid, deviceProperties.pipelineCacheUUID, VK_UUID_SIZE);
    return stamp;
  }

  bool prepared_for(const PreparedStamp &wanted) const {
    const std::vector<std::uint8_t> stored = read_whole_file(shaderCacheFolder / "prepared.bin");
    if (stored.size() != sizeof wanted || std::memcmp(stored.data(), &wanted, sizeof wanted) != 0) return false;
    // The file the game keeps adding to: gone or written for another card, the driver would start empty.
    const std::vector<std::uint8_t> compiled = read_whole_file(shaderCacheFolder / "pipelines.bin");
    VkPipelineCacheHeaderVersionOne header{};
    if (compiled.size() < sizeof header) return false;
    std::memcpy(&header, compiled.data(), sizeof header);
    return header.vendorID == wanted.vendorId && header.deviceID == wanted.deviceId &&
           std::memcmp(header.pipelineCacheUUID, wanted.cacheUuid, VK_UUID_SIZE) == 0 &&
           std::filesystem::exists(shaderCacheFolder / "variants.bin");
  }

  // The shipped list and the sessions' own, without what this card never asks for.
  std::vector<SpecKey> variants_to_prepare() const {
    std::vector<SpecKey> keys;
    std::unordered_set<SpecKey, SpecKeyHash> seen;
    const auto add = [&](const std::uint32_t *words) {
      SpecKey key;
      std::memcpy(key.data(), words, sizeof(SpecKey));
      const std::uint32_t word = key[kSpecFieldCount + 2];
      const bool plain = word == kPlainWord || word == kPlainSheenWord;
      if (plain ? !plainDisjoint : word != 0 && (!ropOn || (rop_blend_mode(word) && ropBlendModule == VK_NULL_HANDLE))) return;
      if (quarantined.count(key) != 0) return;  // TASK-170
      if (keys.size() < kMaxSpecPipelines && seen.insert(key).second) keys.push_back(key);
    };
    constexpr std::size_t kKeyWords = std::tuple_size_v<SpecKey>;
    for (std::size_t i = 0; i + kKeyWords <= std::size(kKnownVariantWords); i += kKeyWords) add(kKnownVariantWords + i);
    const std::vector<std::uint8_t> bytes = read_whole_file(shaderCacheFolder / "variants.bin");
    VariantFileHeader header;
    const VariantFileHeader expected;
    if (bytes.size() >= sizeof header) {
      std::memcpy(&header, bytes.data(), sizeof header);
      if (std::memcmp(header.magic, expected.magic, sizeof header.magic) == 0 && header.shaderSet == shaderSetHash &&
          header.keyWords == expected.keyWords && bytes.size() == sizeof header + std::size_t{header.count} * sizeof(SpecKey)) {
        for (std::uint32_t i = 0; i < header.count; ++i) {
          std::uint32_t words[kKeyWords];
          std::memcpy(words, bytes.data() + sizeof header + i * sizeof(SpecKey), sizeof words);
          add(words);
        }
      }
    }
    return keys;
  }

  // Worker thread, or the GS thread once the worker has stopped.
  void save_shader_cache() {
    if (shaderCacheFolder.empty()) return;
    std::vector<SpecKey> keys;
    {
      std::lock_guard<std::mutex> lock(specMutex);
      if (!specKnownChanged) return;
      specKnownChanged = false;
      keys = specKnown;
    }
    VariantFileHeader header;
    header.shaderSet = shaderSetHash;
    header.count = static_cast<std::uint32_t>(keys.size());
    write_whole_file(shaderCacheFolder / "variants.bin", &header, sizeof header, keys.data(), keys.size() * sizeof(SpecKey));
    if (pipelineCache) {
      std::size_t size = 0;
      if (gt4_hwd_vkGetPipelineCacheData(device, pipelineCache, &size, nullptr) == VK_SUCCESS && size != 0) {
        std::vector<std::uint8_t> data(size);
        if (gt4_hwd_vkGetPipelineCacheData(device, pipelineCache, &size, data.data()) == VK_SUCCESS) {
          write_whole_file(shaderCacheFolder / "pipelines.bin", data.data(), size, nullptr, 0);
        }
      }
    }
  }

  // Records a compile time (specMutex held).
  void note_compile(std::uint64_t ns) {
    specCompileNs += ns;
    specCompileMaxNs = std::max(specCompileMaxNs, ns);
  }

  // TASK-170: the guard files' header for these shaders, this card and this driver.
  GuardFileHeader guard_header(std::uint32_t flags, std::size_t count) const {
    GuardFileHeader header;
    header.shaderSet = shaderSetHash;
    header.vendorId = deviceProperties.vendorID;
    header.deviceId = deviceProperties.deviceID;
    header.driverVersion = deviceProperties.driverVersion;
    header.flags = flags;
    header.count = static_cast<std::uint32_t>(count);
    return header;
  }

  // The keys and flags of a guard file written for these shaders, card and driver; false when absent, damaged or not theirs.
  bool read_guard_file(const std::filesystem::path &path, std::vector<SpecKey> &keys, std::uint32_t &flags) const {
    const std::vector<std::uint8_t> bytes = read_whole_file(path);
    GuardFileHeader header;
    const GuardFileHeader wanted = guard_header(0, 0);
    if (bytes.size() < sizeof header) return false;
    std::memcpy(&header, bytes.data(), sizeof header);
    if (std::memcmp(header.magic, wanted.magic, sizeof header.magic) != 0 || header.shaderSet != wanted.shaderSet ||
        header.vendorId != wanted.vendorId || header.deviceId != wanted.deviceId ||
        header.driverVersion != wanted.driverVersion || header.keyWords != wanted.keyWords ||
        bytes.size() != sizeof header + std::size_t{header.count} * sizeof(SpecKey)) {
      return false;
    }
    keys.resize(header.count);
    if (header.count != 0) std::memcpy(keys.data(), bytes.data() + sizeof header, std::size_t{header.count} * sizeof(SpecKey));
    flags = header.flags;
    return true;
  }

  void write_guard_file(const std::filesystem::path &path, const std::vector<SpecKey> &keys, std::uint32_t flags) const {
    const GuardFileHeader header = guard_header(flags, keys.size());
    write_whole_file(path, &header, sizeof header, keys.data(), keys.size() * sizeof(SpecKey));
  }

  // TASK-170c: the field names of a SpecKey in key order (the kSpecFields, then push, anisotropy and ROP words).
  static constexpr const char *kSpecKeyNames[] = {
      "Format",  "Fba",      "Dither", "Ate",     "Atst",      "Afail",  "DateOn", "Datm",    "Ztest",   "Ztst",
      "Zmsk",    "Zformat",  "BlendOn", "Pabe",   "BlendA",    "BlendB", "BlendC", "BlendD",  "Colclamp", "TexOn",
      "Tfx",     "Tcc",      "TexelKind", "Aem",  "FogOn",     "TexFst", "TexLinear", "WrapS", "WrapT",   "KeepTopByte",
      "push",    "aniso",    "rop"};
  static_assert(std::size(kSpecKeyNames) == kSpecFieldCount + 3, "a name per SpecKey word");

  // The key as "Name=value ..." (values in hex), for the game log.
  static std::string describe_key(const SpecKey &key) {
    std::string text;
    char piece[48];
    for (std::uint32_t i = 0; i < kSpecFieldCount + 3; ++i) {
      std::snprintf(piece, sizeof piece, "%s%s=%x", i == 0 ? "" : " ", kSpecKeyNames[i], key[i]);
      text += piece;
    }
    return text;
  }

  // suspects.bin holds the list; an empty list removes the file (compilingMutex held, or at start).
  void write_suspects_file(const std::vector<SpecKey> &list) const {
    if (list.empty()) {
      std::error_code ignored;
      std::filesystem::remove(shaderCacheFolder / "suspects.bin", ignored);
    } else {
      write_guard_file(shaderCacheFolder / "suspects.bin", list, 0);
    }
  }

  // TASK-170, at the renderer's start (open_shader_cache): what a run that died while compiling left, then the quarantine and
  // the suspects. Quarantined keys go into specSeen, so specialised_pipeline never asks for them (their draws keep the uber
  // pipeline) and queue_known_variants skips them; variants_to_prepare leaves them out. Suspects are compiled as any variant.
  void open_compile_guard() {
    std::vector<SpecKey> list;
    std::uint32_t flags = 0;
    read_guard_file(shaderCacheFolder / "quarantine.bin", list, flags);
    std::uint32_t ignoredFlags = 0;
    std::vector<SpecKey> suspectList;
    read_guard_file(shaderCacheFolder / "suspects.bin", suspectList, ignoredFlags);
    // A key both quarantined and suspected (files edited by hand) is quarantined.
    suspectList.erase(std::remove_if(suspectList.begin(), suspectList.end(),
                                     [&](const SpecKey &key) { return std::find(list.begin(), list.end(), key) != list.end(); }),
                      suspectList.end());
    std::vector<SpecKey> inFlight;
    std::uint32_t inFlightFlags = 0;
    if (read_guard_file(shaderCacheFolder / "compiling.bin", inFlight, inFlightFlags) && !inFlight.empty()) {
      flags |= kGuardSerial;
      if (inFlight.size() == 1) {
        const SpecKey &key = inFlight[0];
        const auto suspect = std::find(suspectList.begin(), suspectList.end(), key);
        if (suspect != suspectList.end()) {
          suspectList.erase(suspect);
          if (std::find(list.begin(), list.end(), key) == list.end()) list.push_back(key);
          std::fprintf(stderr, "[core2] shader guard: a second run stopped while compiling the same variant: it is "
                       "quarantined on this card and driver (its draws keep the uber pipeline, the same pixels): %s\n",
                       describe_key(key).c_str());
        } else if (std::find(list.begin(), list.end(), key) == list.end()) {
          suspectList.push_back(key);
          std::fprintf(stderr, "[core2] shader guard: the last run stopped while compiling one variant: it is a suspect and "
                       "is compiled again; a second stop quarantines it: %s\n", describe_key(key).c_str());
        }
      } else {
        std::fprintf(stderr, "[core2] shader guard: the last run stopped while compiling %zu variants at once: the next "
                     "prepares compile one variant at a time\n", inFlight.size());
      }
      write_guard_file(shaderCacheFolder / "quarantine.bin", list, flags);
      write_suspects_file(suspectList);
    }
    std::error_code ignored;
    std::filesystem::remove(shaderCacheFolder / "compiling.bin", ignored);
    serialPrepare = (flags & kGuardSerial) != 0;
    quarantined.insert(list.begin(), list.end());
    for (const SpecKey &key : list) specSeen.insert(key);
    {
      std::lock_guard<std::mutex> lock(compilingMutex);
      suspects = suspectList;
    }
    std::fprintf(stderr, "[core2] shader guard: %zu quarantined, %zu suspects, prepares compile %s\n", list.size(),
                 suspectList.size(), serialPrepare ? "one variant at a time" : "in parallel");
  }

  // TASK-170: the guard file around one variant compile (any thread). False: do not compile (tests: the run "died").
  bool begin_compile(const SpecKey &key) {
    if (shaderCacheFolder.empty()) return true;
    std::lock_guard<std::mutex> lock(compilingMutex);
    if (abandoned.load()) return false;
    compiling.push_back(key);
    write_guard_file(shaderCacheFolder / "compiling.bin", compiling, 0);
    if (testAbandonAt != 0 && compileStarts.fetch_add(1) + 1 == testAbandonAt) {
      abandoned = true;  // tests: as if the process ended inside this compile
      return false;
    }
    return true;
  }

  void end_compile(const SpecKey &key) {
    if (shaderCacheFolder.empty()) return;
    std::lock_guard<std::mutex> lock(compilingMutex);
    if (abandoned.load()) return;  // tests: a dead process removes nothing
    const auto found = std::find(compiling.begin(), compiling.end(), key);
    if (found != compiling.end()) compiling.erase(found);
    // TASK-170c: the compile ended normally, so a suspect is cleared.
    const auto suspect = std::find(suspects.begin(), suspects.end(), key);
    if (suspect != suspects.end()) {
      suspects.erase(suspect);
      write_suspects_file(suspects);
    }
    if (compiling.empty()) {
      std::error_code ignored;
      std::filesystem::remove(shaderCacheFolder / "compiling.bin", ignored);
    } else {
      write_guard_file(shaderCacheFolder / "compiling.bin", compiling, 0);
    }
  }

  void spec_worker() {
#ifdef _WIN32
    // IN-476: a variant can take seconds on some drivers (2.2 s on the HD 620); on two cores it must not take time from
    // the game's own threads. The game draws with the uber pipeline meanwhile.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#endif
    for (;;) {
      SpecKey key;
      bool saveFirst = false;
      {
        std::unique_lock<std::mutex> lock(specMutex);
        // IN-489: at most every 10 s (a session may end without closing the renderer).
        saveFirst = specKnownChanged && !specStop && std::chrono::steady_clock::now() - specLastSave > std::chrono::seconds(10);
      }
      if (saveFirst) {
        save_shader_cache();
        specLastSave = std::chrono::steady_clock::now();
      }
      {
        std::unique_lock<std::mutex> lock(specMutex);
        specWake.wait_for(lock, std::chrono::seconds(5), [&] { return specStop || !specQueue.empty(); });
        if (specStop) return;
        if (specQueue.empty()) continue;
        key = specQueue.front();
        specQueue.pop_front();
      }
      const auto start = std::chrono::steady_clock::now();
      std::string error;
      const bool compile = begin_compile(key);  // TASK-170
      VkPipeline pipeline = compile ? make_specialised(key, error) : VK_NULL_HANDLE;
      if (compile) end_compile(key);
      const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);
      {
        std::lock_guard<std::mutex> lock(specMutex);
        note_compile(static_cast<std::uint64_t>(ns.count()));
        if (pipeline) {
          specDone.push_back({key, pipeline});
          specAll.push_back(pipeline);
          if (std::find(specKnown.begin(), specKnown.begin() + static_cast<std::ptrdiff_t>(specKnownFromFile), key) ==
              specKnown.begin() + static_cast<std::ptrdiff_t>(specKnownFromFile)) {
            specKnown.push_back(key);
          }
          specKnownChanged = true;
        } else if (specFailures++ == 0) {
          std::fprintf(stderr, "[core2] a specialised pipeline failed (%s); its draws keep the uber pipeline\n",
                       error.c_str());
        }
      }
      specDoneCount.fetch_add(1, std::memory_order_release);
    }
  }

  // Copies the worker's results and statistics for the GS thread (specMutex held).
  void take_spec_results() {
    for (const auto &[key, pipeline] : specDone) specLocal[key] = pipeline;
    specDone.clear();
    specDoneSeen = specDoneCount.load(std::memory_order_acquire);
    stats.specPipelines = specAll.size();
    stats.specCompileNs = specCompileNs;
    stats.specCompileMaxNs = specCompileMaxNs;
    stats.specFailures = specFailures;
  }

  // The pipeline of an interlocked triangle batch with this state block and push word (GS thread).
  VkPipeline triangle_pipeline(const std::uint32_t *fields, std::uint32_t pushWord) {
    // IN-471: without the interlock the triangle pipeline is the plain one, specialised the same way.
    const VkPipeline uber = interlock ? interlockTriangles : pipelines[2];
    if (specMode == SpecMode::off) return uber;
    const VkPipeline pipeline = specialised_pipeline(spec_key(fields, pushWord));
    if (pipeline) return pipeline;
    stats.uberBatches++;
    return uber;
  }

  // IN-496: the plain variant, or null while it compiles (the batch then draws with the interlock).
  VkPipeline plain_pipeline(const std::uint32_t *fields, std::uint32_t pushWord, bool sheen) {
    if (specMode == SpecMode::off) return VK_NULL_HANDLE;
    SpecKey key = spec_key(fields, pushWord);
    key[kSpecFieldCount + 2] = sheen ? kPlainSheenWord : kPlainWord;
    return specialised_pipeline(key);
  }

  // IN-481: the variant of a draw through colour attachments, or null: not eligible or not compiled yet (it then draws
  // with storage writes, the same pixels).
  VkPipeline rop_pipeline(const std::uint32_t *fields, std::uint32_t pushWord, std::uint32_t word) {
    if (!ropOn || word == 0 || specMode == SpecMode::off) return VK_NULL_HANDLE;
    SpecKey key = spec_key(fields, pushWord);
    key[kSpecFieldCount + 2] = word;
    return specialised_pipeline(key);
  }

  // The ready variant of `key`, or null after asking for it (GS thread).
  VkPipeline specialised_pipeline(const SpecKey &key) {
    auto found = specLocal.find(key);
    if (found == specLocal.end() && specDoneCount.load(std::memory_order_acquire) != specDoneSeen) {
      std::lock_guard<std::mutex> lock(specMutex);
      take_spec_results();
      found = specLocal.find(key);
    }
    if (found != specLocal.end()) {
      stats.specialisedBatches++;
      return found->second;
    }
    if (specSeen.size() < kMaxSpecPipelines && specSeen.insert(key).second) {
      stats.specRequested = specSeen.size();
      if (specMode == SpecMode::sync) {
        const auto start = std::chrono::steady_clock::now();
        std::string error;
        const bool compile = begin_compile(key);  // TASK-170
        VkPipeline pipeline = compile ? make_specialised(key, error) : VK_NULL_HANDLE;
        if (compile) end_compile(key);
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start);
        std::lock_guard<std::mutex> lock(specMutex);
        note_compile(static_cast<std::uint64_t>(ns.count()));
        if (pipeline) {
          specAll.push_back(pipeline);
          specLocal[key] = pipeline;
        } else {
          specFailures++;
        }
        take_spec_results();
        if (pipeline) {
          stats.specialisedBatches++;
          return pipeline;
        }
      } else {
        {
          std::lock_guard<std::mutex> lock(specMutex);
          specQueue.push_back(key);
        }
        if (!specThread.joinable()) specThread = std::thread([this] { spec_worker(); });
        specWake.notify_one();
      }
    }
    return VK_NULL_HANDLE;
  }

  VkPipeline make_compute(VkShaderModule module, std::string &error, VkPipelineLayout layout = VK_NULL_HANDLE) {
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = "main";
    info.layout = layout ? layout : computeLayout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    check(gt4_hwd_vkCreateComputePipelines(device, pipelineCache, 1, &info, nullptr, &pipeline),
          "vkCreateComputePipelines", error);
    return pipeline;
  }

  // A set layout of storage images only (`counts[i]` images at binding i) and its pipeline layout with `pushBytes`.
  bool image_layouts(const std::uint32_t *counts, std::uint32_t bindings, std::uint32_t pushBytes,
                     VkDescriptorSetLayout &setLayout, VkPipelineLayout &pipelineLayout, std::string &error) {
    std::vector<VkDescriptorSetLayoutBinding> list(bindings);
    for (std::uint32_t i = 0; i < bindings; ++i) {
      list[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, counts[i], VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = bindings;
    info.pBindings = list.data();
    if (!check(gt4_hwd_vkCreateDescriptorSetLayout(device, &info, nullptr, &setLayout), "vkCreateDescriptorSetLayout",
               error)) return false;
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &setLayout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    return check(gt4_hwd_vkCreatePipelineLayout(device, &layout, nullptr, &pipelineLayout), "vkCreatePipelineLayout",
                 error);
  }

  // A 1x-addressed post image of exactly width x height output pixels (recreated when the size changes; `fresh` tells
  // the caller the content is gone).
  bool post_image(Image &image, std::uint32_t width, std::uint32_t height, bool &fresh, std::string &error) {
    fresh = false;
    if (image.alive && image.width == width && image.height == height) return true;
    doom(image);
    fresh = true;
    return make_image(image, width, height, 1, error);
  }

  // ---- Recording ---------------------------------------------------------------------------------------
  // Diagnostics: full barriers by the command that needed one and why (BarrierReport, GT4_CORE2_BARRIER_TRACE=1).
  // Labels are string literals (or __builtin_FUNCTION names), compared by address first and then by text.
  const char *barrierLabel = "other";
  const char *barrierKind = "";
  struct BarrierReason {
    const char *label;
    const char *kind;
    std::uint64_t count;
  };
  std::vector<BarrierReason> barrierReasons;

  // TASK-182 diagnostics (GT4_CORE2_BARRIER_TRACE=1): how the GPU is synchronised and fed, as counts only; nothing here
  // changes what is recorded. Rows are keyed by text so a reason can name its command and its hazard.
  struct SyncTrace {
    bool on = std::getenv("GT4_CORE2_BARRIER_TRACE") != nullptr;
    struct Row {
      std::string key;
      std::uint64_t count = 0;
      std::uint64_t batches = 0, commands = 0, fragmentBarriers = 0;  // passes: what the ended passes held
    };
    std::vector<Row> fragmentBarriers;  // by reason, in-pass (and rop pass switches)
    std::vector<Row> passEnds;          // render pass ends by reason
    std::vector<Row> flushes;           // draw flushes (the deferred batches recorded) by what asked for them
    std::vector<Row> submits;           // submissions by caller
    std::vector<Row> setAllocations;    // transient descriptor sets by kind
    // TASK-182 step 2a: spec 8.8 self copies by the path they took and whether the copied rectangle meets the rectangle
    // the batch that samples it draws (the oracle's D3 path 2 needs a disjoint one to copy nothing at all).
    std::vector<Row> selfCopies;
    std::uint64_t passBatches = 0, passCommands = 0, passFragmentBarriers = 0;  // the open pass so far
    std::uint64_t updateCalls = 0, updateWrites = 0;
    std::uint64_t bindDescriptorSets = 0, bindPipelines = 0, bindVertexBuffers = 0, setViewports = 0, setScissors = 0,
                  pushConstants = 0, drawCalls = 0, batchesRecorded = 0;
    static Row &row(std::vector<Row> &rows, const std::string &key) {
      for (Row &candidate : rows) {
        if (candidate.key == key) return candidate;
      }
      rows.push_back(Row{key});
      return rows.back();
    }
  } trace;

  void full_barrier(const char *why = __builtin_FUNCTION()) {
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    gt4_hw_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                                &barrier, 0, nullptr, 0, nullptr);
    stats.fullBarriers++;
    if (trace.on) {
      // A barrier no command labelled is named after the function that recorded it.
      const char *label = std::strcmp(barrierLabel, "other") == 0 ? why : barrierLabel;
      bool counted = false;
      for (BarrierReason &reason : barrierReasons) {
        if ((reason.label == label || std::strcmp(reason.label, label) == 0) &&
            (reason.kind == barrierKind || std::strcmp(reason.kind, barrierKind) == 0)) {
          ++reason.count;
          counted = true;
          break;
        }
      }
      if (!counted) barrierReasons.push_back({label, barrierKind, 1});
    }
    drewSinceBarrier = false;
    mayLowerDepth.clear();
    copyImagesUnordered = false;  // a full barrier covers every pooled self-copy image
    pendingBufferReads = pendingBufferWrites = 0;  // everything before is ordered against everything after
    pendingGlmReads.clear();
    pendingGlmWrites.clear();
    pendingReads.clear();
    pendingWrites.clear();
  }

  // TASK-180 test4 (P1-D, STUDY-UNIFIED-MEMORY 7.2): Vulkan's host-read rule. Device writes are visible to a host read of
  // mapped memory only with a dependency into the host domain (the spec's fence note, section 2.7), which the full barrier
  // above does not give: it ends at ALL_COMMANDS, not HOST, and names no HOST_READ access. Exact; the old core does the
  // same (hw_gs_writeback_gpu.cpp). GT4_CORE2_HOST_READ_BARRIER=0 leaves it out (A/B).
  const bool hostReadBarrier = [] {
    const char *value = std::getenv("GT4_CORE2_HOST_READ_BARRIER");
    return value == nullptr || std::strcmp(value, "0") != 0;
  }();
  void host_read_barrier() {
    if (!hostReadBarrier) return;
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    gt4_hw_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    ++stats.hostReadBarriers;  // not counted in fullBarriers; the pending sets already cover the full barrier above
  }

  // IN-419: the accesses recorded since the last full barrier, so a compute or transfer command gets a barrier only
  // when it depends on one of them: it reads or writes what an earlier command wrote, or writes what one read. Core 2
  // recorded a barrier before and after every one (about 2000 per night VBlank, each draining the GPU), though most
  // touch unrelated things (two texture decodes write two different images). Buffers: the GLM and the GPU CLUT;
  // images by serial, whole or one row (the palette rows, IN-420). GT4_CORE2_LAZY_BARRIERS=0 records a barrier around
  // every command again (A/B).
  enum : unsigned { kGlmBuffer = 1u, kClutBuffer = 2u };
  static constexpr std::uint32_t kWholeImage = 0xFFFFFFFFu;
  struct Use {
    std::uint32_t serial = 0;  // 0: no image
    std::uint32_t region = kWholeImage;
    Use(const Image *image) : serial(image ? image->serial : 0u) {}
    Use(const Image *image, std::uint32_t row) : serial(image ? image->serial : 0u), region(row) {}
  };
  // IN-447 (TASK-027 CPU survey, front R2): the images a set touches live in a flat table with generation stamps (an
  // open-addressing index by serial into an entry array), so clear() frees nothing and adding an image after a barrier
  // allocates nothing once the arrays are warm. The answers are those of the former unordered_map of row vectors.
  struct AccessSet {
    struct Entry {
      std::uint32_t serial = 0;
      bool whole = false;
      std::vector<std::uint32_t> rows;  // kept allocated across clears
    };
    struct Slot {
      std::uint32_t serial = 0;
      std::uint32_t generation = 0;  // the slot is in use when this equals the set's generation
      std::uint32_t entry = 0;
    };
    std::vector<Slot> slots = std::vector<Slot>(2048);  // a power of two; doubled when half full
    std::vector<Entry> entries;  // entries[0, live) are this generation's
    std::size_t live = 0;
    std::uint32_t generation = 1;

    std::size_t size() const { return live; }
    std::size_t home(std::uint32_t serial) const { return (serial * 2654435761u) & (slots.size() - 1); }
    const Entry *find(std::uint32_t serial) const {
      for (std::size_t i = home(serial);; i = (i + 1) & (slots.size() - 1)) {
        const Slot &slot = slots[i];
        if (slot.generation != generation) return nullptr;
        if (slot.serial == serial) return &entries[slot.entry];
      }
    }
    Entry &insert(std::uint32_t serial) {
      if (2 * (live + 1) > slots.size()) grow();
      for (std::size_t i = home(serial);; i = (i + 1) & (slots.size() - 1)) {
        Slot &slot = slots[i];
        if (slot.generation == generation) {
          if (slot.serial == serial) return entries[slot.entry];
          continue;
        }
        if (live == entries.size()) entries.emplace_back();
        Entry &entry = entries[live];
        entry.serial = serial;
        entry.whole = false;
        entry.rows.clear();
        slot = Slot{serial, generation, static_cast<std::uint32_t>(live)};
        ++live;
        return entry;
      }
    }
    // Doubles the index and re-inserts this generation's entries (a probe always finds a free slot).
    void grow() {
      std::vector<Slot> bigger(slots.size() * 2);
      slots.swap(bigger);
      for (std::size_t e = 0; e < live; ++e) {
        std::size_t i = home(entries[e].serial);
        while (slots[i].generation == generation) i = (i + 1) & (slots.size() - 1);
        slots[i] = Slot{entries[e].serial, generation, static_cast<std::uint32_t>(e)};
      }
    }
    void add(std::uint32_t serial, std::uint32_t region) {
      if (serial == 0) return;
      Entry &entry = insert(serial);
      if (region == kWholeImage) {
        entry.whole = true;
      } else if (!entry.whole && std::find(entry.rows.begin(), entry.rows.end(), region) == entry.rows.end()) {
        entry.rows.push_back(region);
      }
    }
    bool conflicts(std::uint32_t serial, std::uint32_t region) const {
      if (serial == 0) return false;
      const Entry *entry = find(serial);
      if (!entry) return false;
      return entry->whole || region == kWholeImage ||
             std::find(entry->rows.begin(), entry->rows.end(), region) != entry->rows.end();
    }
    void merge(const AccessSet &other) {
      for (std::size_t i = 0; i < other.live; ++i) {
        const Entry &entry = other.entries[i];
        if (entry.whole) add(entry.serial, kWholeImage);
        for (std::uint32_t row : entry.rows) add(entry.serial, row);
      }
    }
    void clear() {
      live = 0;
      if (++generation == 0) {  // after 2^32 clears: reset the stamps once
        for (Slot &slot : slots) slot.generation = 0;
        generation = 1;
      }
    }
  };
  bool lazyBarriers =
      std::getenv("GT4_CORE2_LAZY_BARRIERS") == nullptr || std::strcmp(std::getenv("GT4_CORE2_LAZY_BARRIERS"), "0") != 0;
  unsigned pendingBufferReads = 0, pendingBufferWrites = 0;
  AccessSet pendingReads, pendingWrites;

  // IN-464: GLM accesses by 8 KiB page (512 pages of 32 blocks) instead of the whole 4 MiB buffer. A decode, a dirty
  // patch, a CLUT load or a target written into the GLM names the pages it reads or writes, so a GLM write needs a
  // barrier only after a command that read or wrote one of its pages (about half the full barriers of a race VBlank
  // were whole-buffer GLM hazards between unrelated pages). GT4_CORE2_GLM_PAGES=0 treats every GLM access as all pages.
  const bool glmPages =
      std::getenv("GT4_CORE2_GLM_PAGES") == nullptr || std::strcmp(std::getenv("GT4_CORE2_GLM_PAGES"), "0") != 0;
  GlmPages pendingGlmReads, pendingGlmWrites;

  // The pages of rectangle [x0, x1] x [y0, y1] of buffer (bp, bw, psm), conservatively: every page of the page rows
  // it crosses (more rows when it runs past the buffer width) and one more page when bp is not page aligned, as the
  // renderer's SpanOfRect counts them. An unknown format or a rectangle outside 2048 x 2048 is every page.
  GlmPages glm_pages_of_rect(std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, std::int32_t x0, std::int32_t y0,
                             std::int32_t x1, std::int32_t y1) const {
    const std::optional<memory::Psm> format = memory::decodePsm(psm);
    if (!glmPages || !format || x0 < 0 || y0 < 0 || x1 >= 2048 || y1 >= 2048) return GlmPages::all();
    GlmPages pages;
    if (x1 < x0 || y1 < y0) return pages;
    const memory::PsmInfo &info = memory::psmInfo(*format);
    const std::uint32_t bufferWidth = std::max<std::uint32_t>(bw, 1u) * 64u;
    const std::uint32_t pagesPerRow = std::max<std::uint32_t>(1u, (bufferWidth + info.pageWidth - 1u) / info.pageWidth);
    const std::uint32_t firstRow = static_cast<std::uint32_t>(y0) / info.pageHeight;
    std::uint32_t rows = static_cast<std::uint32_t>(y1) / info.pageHeight - firstRow + 1u;
    if (static_cast<std::uint32_t>(x1) >= bufferWidth) rows += 1u + static_cast<std::uint32_t>(x1) / bufferWidth;
    const std::uint64_t count = static_cast<std::uint64_t>(rows) * pagesPerRow + (bp % 32u != 0 ? 1u : 0u);
    pages.add_run((bp / 32u + firstRow * pagesPerRow) % 512u,
                  static_cast<std::uint32_t>(std::min<std::uint64_t>(count, 512u)));
    return pages;
  }
  // Blocks [first, first + count), wrapping.
  GlmPages glm_pages_of_blocks(std::uint32_t first, std::uint32_t count) const {
    if (!glmPages) return GlmPages::all();
    GlmPages pages;
    if (count == 0) return pages;
    const std::uint32_t firstPage = (first % 16384u) / 32u;
    const std::uint32_t lastPage = ((first % 16384u) + count - 1u) / 32u;
    pages.add_run(firstPage, static_cast<std::uint32_t>(std::min<std::uint64_t>(lastPage - firstPage + 1u, 512u)));
    return pages;
  }

  // Records a barrier first when accesses depend on the pending ones, then adds them. GLM accesses come as pages
  // (IN-464); `bufferReads` and `bufferWrites` name the GPU CLUT only.
  void order(std::initializer_list<Use> reads, std::initializer_list<Use> writes, unsigned bufferReads,
             unsigned bufferWrites, const GlmPages &glmReads = GlmPages{}, const GlmPages &glmWrites = GlmPages{}) {
    const bool clutHazard =
        ((bufferReads | bufferWrites) & pendingBufferWrites) != 0 || (bufferWrites & pendingBufferReads) != 0;
    const bool glmRaw = glmReads.meets(pendingGlmWrites) || glmWrites.meets(pendingGlmWrites);
    const bool glmWar = glmWrites.meets(pendingGlmReads);
    bool hazard = clutHazard || glmRaw || glmWar;
    bool imageRaw = false, imageWaw = false, imageWar = false;  // TASK-182: which image pair (diagnostics)
    for (const Use &use : reads) {
      imageRaw = imageRaw || pendingWrites.conflicts(use.serial, use.region);
    }
    for (const Use &use : writes) {
      imageWaw = imageWaw || pendingWrites.conflicts(use.serial, use.region);
      imageWar = imageWar || pendingReads.conflicts(use.serial, use.region);
    }
    hazard = hazard || imageRaw || imageWaw || imageWar;
    if (hazard || pendingReads.size() + pendingWrites.size() > 512) {
      barrierKind = clutHazard ? ((pendingBufferWrites & (bufferReads | bufferWrites)) != 0 ? "clut buffer" : "clut buffer war")
                    : glmRaw   ? "glm pages"
                    : glmWar   ? "glm pages war"
                    : imageRaw ? "image raw"
                    : imageWaw ? "image waw"
                    : imageWar ? "image war"
                               : "overflow";
      full_barrier();
      barrierKind = "";
    } else {
      stats.barriersSkipped++;
    }
    pendingBufferReads |= bufferReads;
    pendingBufferWrites |= bufferWrites;
    pendingGlmReads.merge(glmReads);
    pendingGlmWrites.merge(glmWrites);
    for (const Use &use : reads) pendingReads.add(use.serial, use.region);
    for (const Use &use : writes) pendingWrites.add(use.serial, use.region);
  }

  // Declares the accesses of the compute or transfer command about to be recorded. With IN-420 the deferred draws are
  // recorded first when the command depends on them (reads what they write, writes what they read or write).
  // kGlmBuffer in `bufferReads` / `bufferWrites` means the GLM pages `glmReads` / `glmWrites` (every page when null).
  void touch(const char *label, std::initializer_list<Use> reads, std::initializer_list<Use> writes,
             unsigned bufferReads, unsigned bufferWrites, const GlmPages *glmReads = nullptr,
             const GlmPages *glmWrites = nullptr) {
    if (!lazyBarriers) {  // outside_pass and after_op already order every command
      prof_begin(label);
      return;
    }
    if (!deferred.empty()) {
      bool dependsOnDraws = false;
      const char *dependency = "reads what a draw writes";  // TASK-182 diagnostics
      for (const Use &use : reads) dependsOnDraws = dependsOnDraws || drawWrites.conflicts(use.serial, use.region);
      for (const Use &use : writes) {
        if (!dependsOnDraws && drawWrites.conflicts(use.serial, use.region)) dependency = "writes what a draw writes";
        else if (!dependsOnDraws && drawReads.conflicts(use.serial, use.region)) dependency = "writes what a draw reads";
        dependsOnDraws = dependsOnDraws || drawWrites.conflicts(use.serial, use.region) ||
                         drawReads.conflicts(use.serial, use.region);
      }
      if (dependsOnDraws) flush_draws(label, dependency);
    }
    const GlmPages glmRead = (bufferReads & kGlmBuffer) == 0 ? GlmPages{} : glmReads ? *glmReads : GlmPages::all();
    const GlmPages glmWrite = (bufferWrites & kGlmBuffer) == 0 ? GlmPages{} : glmWrites ? *glmWrites : GlmPages::all();
    barrierLabel = label;
    order(reads, writes, bufferReads & ~kGlmBuffer, bufferWrites & ~kGlmBuffer, glmRead, glmWrite);
    barrierLabel = "other";
    prof_begin(label);
  }

  // After a compute or transfer command: a barrier at once only without IN-419 (touch orders it otherwise).
  void after_op() {
    if (!lazyBarriers) full_barrier();
    prof_end();
  }

  // S3: GPU cost map (diagnostics, GT4_CORE2_GPU_PROFILE_AT=<vblank>): during one VBlank every compute, transfer and
  // draw command waits for the work before it (a barrier) and is timed on its own (timestamps at the completion of
  // the work before it and of itself); ProfileReport prints the GPU time per kind of command. The barriers make that
  // VBlank slower; the numbers are each command's own cost.
  static constexpr std::uint32_t kProfileQueries = 16384;
  VkQueryPool profilePool = VK_NULL_HANDLE;
  bool profiling = false, profileOpen = false;
  std::uint32_t profileNext = 0;
  std::vector<std::string> profileLabels;  // label of query pair (2i, 2i + 1)
  // TASK-134: a view, so the commands' labels (touch passes a C string) build no std::string while nothing profiles.
  static std::uint32_t category_of(std::string_view label) {
    if (label == "compose") return kCatCompose;
    for (const char *upload : {"upload atlas", "upload image", "glm upload", "copy image", "self copy", "glm write (target)",
                               "patch (cpu words)"}) {
      if (label == upload) return kCatUpload;
    }
    return kCatCompute;  // clear, fill, decode, dirty patch, palette row, clut work, tone
  }
  void cat_mark(std::uint32_t kind) {
    if (!catOn || !catPool) return;
    Slot &current = slots[slot];
    if (current.catCount >= kCatPoints || (current.catCount != 0 && current.catKinds[current.catCount - 1] == kind)) return;
    gt4_hwd_vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, catPool, slot * kCatPoints + current.catCount);
    current.catKinds[current.catCount++] = static_cast<std::uint8_t>(kind);
  }
  void prof_begin(std::string_view label) {
    if (catOn && !profiling && !label.empty()) cat_mark(category_of(label));
    if (!profiling || profileOpen || profileNext + 2u > kProfileQueries) return;
    if (renderPassOpen) {
      fragment_barrier("profile");
    } else {
      full_barrier("profile");
    }
    gt4_hwd_vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, profilePool, profileNext);
    profileLabels.emplace_back(label);
    profileOpen = true;
  }
  void prof_end() {
    if (!profileOpen) return;
    gt4_hwd_vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, profilePool, profileNext + 1u);
    profileNext += 2u;
    profileOpen = false;
  }

  // IN-420: draw batches recorded later, in one render pass. Core 2 recorded each batch at once, so every texture
  // decode, dirty patch or GLM upload between two batches ended the render pass (250-310 render pass begins per night
  // VBlank, a fixed GPU cost each). Now a batch's commands wait here, and compute and transfer work that does not depend
  // on the waiting batches is recorded before them; work that does records them first, as do submissions, host reads
  // and the rare passes. GT4_CORE2_DEFER_DRAWS=0 records each batch at once (A/B; needs IN-419).
  struct DeferredCommand {
    VkRect2D scissor{};
    VkPipeline pipeline = VK_NULL_HANDLE;
    std::uint32_t first = 0, count = 0;
    bool barrierFirst = false;  // without the pixel interlock every primitive waits for the one before
  };
  struct DeferredDraw {
    VkDescriptorSet set = VK_NULL_HANDLE;
    std::uint32_t uniformOffset = 0;
    VkDeviceSize vertexOffset = 0;
    VkViewport viewport{};
    std::uint32_t push[8] = {};
    bool textureFromTarget = false;
    const char *sampleKind = "memory texture";  // TASK-182 diagnostics: what textureFromTarget samples
    // IN-492, image serials (0: none): the depth image this batch reads before the interlock, the one it writes other than
    // upwards, and the frame it writes (a frame's words may be read as depth by another batch).
    std::uint64_t peeksDepth = 0, lowersDepth = 0, frameSerial = 0;
    bool plain = false;  // IN-496
    bool rop = false;  // IN-481: through colour attachments
    VkRenderPass ropPass = VK_NULL_HANDLE;
    VkFramebuffer ropFramebuffer = VK_NULL_HANDLE;
    std::uint32_t ropWidth = 0, ropHeight = 0;
    std::uint32_t firstCommand = 0, commandCount = 0;
    std::string profileLabel;  // S3: only while profiling
  };
  bool deferDraws = lazyBarriers && (std::getenv("GT4_CORE2_DEFER_DRAWS") == nullptr ||
                                     std::strcmp(std::getenv("GT4_CORE2_DEFER_DRAWS"), "0") != 0);
  std::vector<DeferredDraw> deferred;
  std::vector<DeferredCommand> deferredCommands;
  // IN-466, IN-471: the shapes of the per-primitive commands since the last wait (Gpu::Draw), at most kDisjointGroupMax
  // before a wait anyway, and the images they draw into.
  static constexpr std::size_t kDisjointGroupMax = 256;
  const bool groupDisjoint =
      std::getenv("GT4_CORE2_GROUP_DISJOINT") == nullptr || std::strcmp(std::getenv("GT4_CORE2_GROUP_DISJOINT"), "0") != 0;
  std::vector<PrimitiveShape> disjointGroup;
  std::uint32_t disjointFrame = 0, disjointDepth = 0;
  struct FillRect { std::int32_t x0, y0, x1, y1; };  // IN-442 constant fills: output pixels [x0, x1) x [y0, y1)
  std::vector<FillRect> fillRects;                    // IN-447: reused by every constant-fill batch
  AccessSet drawReads, drawWrites;  // what the deferred batches read and write

  // IN-458: the graphics state recorded since the draw pass began, so record_draw leaves out binds that would set what
  // is already set (about 27 batches per pass in the 3x race, most with the same viewport, push words and vertex ring).
  // Only record_draw and fragment barriers record inside a draw pass, and every graphics pipeline uses drawLayout;
  // begin_draw_pass forgets it all. GT4_CORE2_SKIP_REDUNDANT=0 records every bind (A/B).
  const bool skipRedundant = std::getenv("GT4_CORE2_SKIP_REDUNDANT") == nullptr ||
                             std::strcmp(std::getenv("GT4_CORE2_SKIP_REDUNDANT"), "0") != 0;
  struct PassState {
    bool vertexRing = false;  // the vertex ring bound at offset 0
    VkDescriptorSet set = VK_NULL_HANDLE;
    std::uint32_t uniformOffset = 0;
    bool viewportSet = false;
    VkViewport viewport{};
    bool pushSet = false;
    std::uint32_t push[8] = {};
    VkPipeline pipeline = VK_NULL_HANDLE;
    bool scissorSet = false;
    VkRect2D scissor{};
  } passState;

  void begin_draw_pass() {
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = renderPass;
    begin.framebuffer = framebuffer;
    begin.renderArea = {{0, 0}, {framebufferWidth, framebufferHeight}};
    gt4_hwd_vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    stats.renderPasses++;
    renderPassOpen = true;
    passState = PassState{};
    trace.passBatches = trace.passCommands = trace.passFragmentBarriers = 0;
  }

  // One batch's commands, inside the open render pass; `first` is the pass's first batch (no barrier before it).
  // IN-481: the framebuffer of a frame and optional depth attachment. Colour attachments need the same size.
  bool rop_framebuffer(const Image &frame, const Image *depth, VkFramebuffer &out, std::string &error) {
    const std::pair<VkImageView, VkImageView> key{frame.ropView, depth ? depth->ropView : VK_NULL_HANDLE};
    auto found = ropFramebuffers.find(key);
    if (found != ropFramebuffers.end()) {
      out = found->second;
      return true;
    }
    const VkImageView views[2] = {key.first, key.second};
    VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    info.renderPass = depth ? ropPassBoth : ropPassFrame;
    info.attachmentCount = depth ? 2 : 1;
    info.pAttachments = views;
    info.width = frame.width;
    info.height = frame.height;
    info.layers = 1;
    if (!check(gt4_hwd_vkCreateFramebuffer(device, &info, nullptr, &out), "vkCreateFramebuffer", error)) return false;
    ropFramebuffers[key] = out;
    return true;
  }

  void begin_rop_pass(const DeferredDraw &draw) {
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = draw.ropPass;
    begin.framebuffer = draw.ropFramebuffer;
    begin.renderArea = {{0, 0}, {draw.ropWidth, draw.ropHeight}};
    gt4_hwd_vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    stats.renderPasses++;
    renderPassOpen = true;
    passState = PassState{};
    trace.passBatches = trace.passCommands = trace.passFragmentBarriers = 0;
  }

  // Between a pass of storage writes and one of attachment writes (either order), or two of attachments.
  void rop_barrier() {
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    const VkPipelineStageFlags stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    gt4_hw_vkCmdPipelineBarrier(cmd, stages, stages, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    stats.fragmentBarriers++;
    if (trace.on) ++SyncTrace::row(trace.fragmentBarriers, "rop pass switch (between passes)").count;
    drewSinceBarrier = false;
    mayLowerDepth.clear();
  }

  void record_draw(const DeferredDraw &draw, bool first) {
    prof_begin(draw.profileLabel);
    if (draw.plain && drewSinceBarrier) fragment_barrier("plain batch, wait before (IN-496)");
    if (earlyOut) {  // IN-492
      if (draw.peeksDepth != 0 && !draw.rop &&
          std::find(mayLowerDepth.begin(), mayLowerDepth.end(), draw.peeksDepth) != mayLowerDepth.end()) {
        fragment_barrier("depth peek before the interlock (IN-492)");
      }
      if (draw.frameSerial != 0 && std::find(mayLowerDepth.begin(), mayLowerDepth.end(), draw.frameSerial) == mayLowerDepth.end()) {
        mayLowerDepth.push_back(draw.frameSerial);
      }
      if (draw.lowersDepth != 0 && std::find(mayLowerDepth.begin(), mayLowerDepth.end(), draw.lowersDepth) == mayLowerDepth.end()) {
        mayLowerDepth.push_back(draw.lowersDepth);
      }
    }
    if (!draw.rop && !first && drewSinceBarrier && ((!interlock && !groupDisjoint) || draw.textureFromTarget)) {
      // A texture taken from a target reads pixels other than its own (no interlock order). Without pixel interlock a
      // batch waits for the one before through its first command (IN-471), or always with GT4_CORE2_GROUP_DISJOINT=0.
      fragment_barrier(draw.textureFromTarget ? draw.sampleKind : "batches wait for each other (no interlock, group off)");
    }
    PassState &bound = passState;
    if (trace.on) {
      ++trace.batchesRecorded;
      ++trace.passBatches;
      trace.passCommands += draw.commandCount;
    }
    if (!skipRedundant || draw.set != bound.set || draw.uniformOffset != bound.uniformOffset) {
      gt4_hwd_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, drawLayout, 0, 1, &draw.set, 1,
                                      &draw.uniformOffset);
      if (trace.on) ++trace.bindDescriptorSets;
      bound.set = draw.set;
      bound.uniformOffset = draw.uniformOffset;
    }
    // IN-458: the ring is bound at offset 0 and the draws start at the batch's first vertex (its ring offset is a
    // whole number of 32-byte vertices; no shader reads gl_VertexIndex).
    std::uint32_t baseVertex = 0;
    if (skipRedundant && draw.vertexOffset % sizeof(gpu::GpuVertex) == 0) {
      if (!bound.vertexRing) {
        const VkDeviceSize ringStart = 0;
        gt4_hwd_vkCmdBindVertexBuffers(cmd, 0, 1, &vertices.buffer, &ringStart);
        if (trace.on) ++trace.bindVertexBuffers;
        bound.vertexRing = true;
      }
      baseVertex = static_cast<std::uint32_t>(draw.vertexOffset / sizeof(gpu::GpuVertex));
    } else {
      gt4_hwd_vkCmdBindVertexBuffers(cmd, 0, 1, &vertices.buffer, &draw.vertexOffset);
      if (trace.on) ++trace.bindVertexBuffers;
      bound.vertexRing = false;
    }
    if (!skipRedundant || !bound.viewportSet || std::memcmp(&draw.viewport, &bound.viewport, sizeof draw.viewport) != 0) {
      gt4_hwd_vkCmdSetViewport(cmd, 0, 1, &draw.viewport);
      if (trace.on) ++trace.setViewports;
      bound.viewportSet = true;
      bound.viewport = draw.viewport;
    }
    if (!skipRedundant || !bound.pushSet || std::memcmp(draw.push, bound.push, sizeof draw.push) != 0) {
      gt4_hwd_vkCmdPushConstants(cmd, drawLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                 sizeof draw.push, draw.push);
      if (trace.on) ++trace.pushConstants;
      bound.pushSet = true;
      std::memcpy(bound.push, draw.push, sizeof draw.push);
    }
    for (std::uint32_t i = 0; i < draw.commandCount; ++i) {
      const DeferredCommand &command = deferredCommands[draw.firstCommand + i];
      if (!draw.rop && command.barrierFirst && drewSinceBarrier) fragment_barrier("primitive may share a pixel with an earlier one");
      if (!skipRedundant || !bound.scissorSet ||
          std::memcmp(&command.scissor, &bound.scissor, sizeof command.scissor) != 0) {
        gt4_hwd_vkCmdSetScissor(cmd, 0, 1, &command.scissor);
        if (trace.on) ++trace.setScissors;
        bound.scissorSet = true;
        bound.scissor = command.scissor;
      }
      if (!skipRedundant || command.pipeline != bound.pipeline) {
        gt4_hwd_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, command.pipeline);
        if (trace.on) ++trace.bindPipelines;
        bound.pipeline = command.pipeline;
      }
      if (pointQuads && command.pipeline == pipelines[0]) {
        // TASK-179: six corners per point, one instance per point record (the same records, read per instance).
        gt4_hwd_vkCmdDraw(cmd, 6, command.count, 0, baseVertex + command.first);
        stats.pointQuads += command.count;
      } else {
        gt4_hwd_vkCmdDraw(cmd, command.count, 1, baseVertex + command.first, 0);
      }
      if (command.pipeline == pipelines[0]) stats.points += command.count;
      stats.draws++;
      if (trace.on) ++trace.drawCalls;
      drewSinceBarrier = true;
    }
    if (draw.plain) fragment_barrier("plain batch, wait after (IN-496)");
    prof_end();
  }

  // TASK-182 R1: the self copy (spec 8.8) recorded as a draw inside the open render pass, in place of a vkCmdCopyImage
  // that needs the pass closed. `left, top, right, bottom` are the copied rectangle in output pixels, half-open. The
  // pass is the storage draw pass (no attachments), so a fragment barrier orders the earlier draws' stores to the
  // source before the copy's loads, and the batch that samples the copy (recorded right after it) is ordered by its own
  // texture-from-target barrier. A word keeps its own pixel, exactly what the transfer copied.
  bool record_self_copy(const Image &source, Image &destination, std::int32_t left, std::int32_t top,
                        std::int32_t right, std::int32_t bottom, std::string &error) {
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!allocate_set(selfCopySetLayout, set, error)) return false;  // may submit: before the pass, never inside it
    VkDescriptorImageInfo images[2] = {{VK_NULL_HANDLE, source.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, destination.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[2]{};
    for (std::uint32_t i = 0; i < 2; ++i) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = set;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      writes[i].pImageInfo = &images[i];
    }
    update_sets(2, writes);
    if (!renderPassOpen) {
      // A pass was ended by a compute or transfer command since the last copy: a full barrier is the only thing that
      // orders the pooled copy image's earlier readers against this write across the pass boundary.
      if (copyImagesUnordered) full_barrier("self copy draw: the pooled copy was used before this pass");
      if (!begin_recording(error)) return false;
      begin_draw_pass();
    }
    fragment_barrier("self copy draw: after the draws that wrote the source");
    gt4_hwd_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, selfCopyPipeline);
    gt4_hwd_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, selfCopyLayout, 0, 1, &set, 0, nullptr);
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(framebufferWidth), static_cast<float>(framebufferHeight),
                              0.0f, 1.0f};
    gt4_hwd_vkCmdSetViewport(cmd, 0, 1, &viewport);
    const VkRect2D scissor{{left, top},
                           {static_cast<std::uint32_t>(right - left), static_cast<std::uint32_t>(bottom - top)}};
    gt4_hwd_vkCmdSetScissor(cmd, 0, 1, &scissor);
    gt4_hwd_vkCmdDraw(cmd, 3, 1, 0, 0);
    passState = PassState{};  // the copy bound its own pipeline, set and viewport: the batch sets its own again
    drewSinceBarrier = true;
    copyImagesUnordered = true;
    stats.selfCopies++;
    stats.selfCopyDraws++;
    stats.selfCopyPixels += static_cast<std::uint64_t>(right - left) * static_cast<std::uint64_t>(bottom - top);
    stats.gpuCopyImageBytes += static_cast<std::uint64_t>(right - left) * static_cast<std::uint64_t>(bottom - top) * 4u;  // P1-C
    if (trace.on) ++trace.drawCalls;
    return true;
  }

  // Records the deferred batches in one render pass, ordered against the pending compute and transfer accesses.
  // `why` names what asked (a command's label, or the calling function); `detail` the dependency (diagnostics only).
  // `keepPassOpen` (TASK-182 R1, the self copy draw): the pass stays open after the batches, so the copy draw and the
  // batch that samples it join it instead of paying a pass end and a pass begin here.
  void flush_draws(const char *why = __builtin_FUNCTION(), const char *detail = "", bool keepPassOpen = false) {
    if (deferred.empty()) return;
    cat_mark(kCatDraw);
    std::string flushKey;  // TASK-182 diagnostics
    if (trace.on) {
      flushKey = std::string(why);
      if (*detail) flushKey += std::string(": ") + detail;
      ++SyncTrace::row(trace.flushes, flushKey).count;
    }
    bool hazard = false;
    for (std::size_t i = 0; i < drawReads.live; ++i) {
      const AccessSet::Entry &entry = drawReads.entries[i];
      if (entry.whole) hazard = hazard || pendingWrites.conflicts(entry.serial, kWholeImage);
      for (std::uint32_t row : entry.rows) hazard = hazard || pendingWrites.conflicts(entry.serial, row);
    }
    for (std::size_t i = 0; i < drawWrites.live; ++i) {
      const AccessSet::Entry &entry = drawWrites.entries[i];
      if (entry.whole) {
        hazard = hazard || pendingWrites.conflicts(entry.serial, kWholeImage) ||
                 pendingReads.conflicts(entry.serial, kWholeImage);
      }
      for (std::uint32_t row : entry.rows) {
        hazard = hazard || pendingWrites.conflicts(entry.serial, row) || pendingReads.conflicts(entry.serial, row);
      }
    }
    if (hazard) {
      // A full barrier cannot be recorded inside a render pass; a pass the self copy draw left open ends here.
      end_render_pass("a full barrier before the draws");
      barrierLabel = "draws";
      full_barrier(why);
      barrierLabel = "other";
    } else {
      stats.barriersSkipped++;
    }
    const std::uint32_t pair = slots[slot].passTimestamps;
    const bool timed = timestamps && passTimestampsOn && pair < kPassTimestampPairs;
    const std::uint32_t query = slot * kTimestampsPerSlot + 2u + 2u * pair;
    if (timed) gt4_hwd_vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, query);
    // IN-481: a new pass when the way of writing or the attachments change; a texture taken from a target must see what
    // the attachments of the current pass wrote. TASK-182 R1: a pass the self copy draw left open takes the first batch
    // as a continuation (no new pass, and the barrier of record_draw orders the batch against the copy draw).
    bool passOpen = renderPassOpen;
    const DeferredDraw *open = nullptr;  // the batch that began or continued the pass
    for (std::size_t i = 0; i < deferred.size(); ++i) {
      const DeferredDraw &draw = deferred[i];
      const bool sameKind = open == nullptr ? !draw.rop : draw.rop == open->rop;
      const bool newPass = !passOpen || !sameKind ||
                           (draw.rop && (draw.ropFramebuffer != open->ropFramebuffer || draw.ropPass != open->ropPass ||
                                         (draw.textureFromTarget && drewSinceBarrier)));
      if (newPass) {
        if (passOpen) {
          end_render_pass(open == nullptr || draw.rop != open->rop
                              ? "pass change: colour attachments on or off"
                          : draw.ropFramebuffer != open->ropFramebuffer || draw.ropPass != open->ropPass
                              ? "pass change: other colour attachments"
                              : "pass change: texture from target after a draw (attachments)");
          rop_barrier();
        }
        if (draw.rop) {
          begin_rop_pass(draw);
        } else {
          begin_draw_pass();
        }
        open = &draw;
        passOpen = true;
      }
      record_draw(draw, newPass);
    }
    if (!keepPassOpen) {
      if (trace.on) {
        end_render_pass_as("draws recorded for " + flushKey);
      } else {
        end_render_pass();
      }
    }
    if (timed) {
      gt4_hwd_vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, query + 1u);
      slots[slot].passTimestamps = pair + 1u;
    }
    pendingReads.merge(drawReads);
    pendingWrites.merge(drawWrites);
    drawReads.clear();
    drawWrites.clear();
    deferred.clear();
    deferredCommands.clear();
    stats.drawFlushes++;
  }

  // Inside the render pass (the subpass self-dependency): later fragments see earlier fragment stores.
  void fragment_barrier(const char *why) {
    stats.fragmentBarriers++;
    if (trace.on) {
      ++SyncTrace::row(trace.fragmentBarriers, why).count;
      ++trace.passFragmentBarriers;
    }
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    gt4_hw_vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                                1, &barrier, 0, nullptr, 0, nullptr);
    drewSinceBarrier = false;
    mayLowerDepth.clear();
  }

  // `why` is the function (or reason) that ended the pass; it only feeds the TASK-182 diagnostics.
  void end_render_pass_as(const std::string &why) {
    if (!renderPassOpen) return;
    gt4_hwd_vkCmdEndRenderPass(cmd);
    renderPassOpen = false;
    if (trace.on) {
      SyncTrace::Row &row = SyncTrace::row(trace.passEnds, why);
      ++row.count;
      row.batches += trace.passBatches;
      row.commands += trace.passCommands;
      row.fragmentBarriers += trace.passFragmentBarriers;
    }
  }
  void end_render_pass(const char *why = __builtin_FUNCTION()) {
    if (!renderPassOpen) return;
    if (trace.on) {
      end_render_pass_as(why);
      return;
    }
    gt4_hwd_vkCmdEndRenderPass(cmd);
    renderPassOpen = false;
  }

  // Leaves the render pass; without IN-419 also orders everything before against everything after (compute,
  // transfer). With it, the command's touch() decides.
  void outside_pass(const char *why = __builtin_FUNCTION()) {
    end_render_pass(why);
    if (!lazyBarriers) full_barrier();
  }

  bool wait_slot(std::uint32_t index, std::string &error, bool spin = false) {
    Slot &s = slots[index];
    if (!s.inFlight) return true;
    const auto waitStart = std::chrono::steady_clock::now();
    bool signalled = false;
    if (spin && readbackSpinUs != 0) {
      // TASK-182 step 3 (GT4_HW_READBACK_SPIN): poll the fence for a bounded time before blocking on it. Same wait, one
      // fewer trip through the driver: a fence that is about to signal is seen here, and one that is not costs at most
      // readbackSpinUs of CPU before the blocking call. The poll is exact by construction: a signalled fence means the
      // submission completed, which is what the blocking wait would have returned.
      const auto spinStart = waitStart;
      while (true) {
        const VkResult polled = gt4_hw_vkWaitForFences(device, 1, &s.fence, VK_TRUE, 0);
        stats.spinPolls++;
        if (polled == VK_SUCCESS) {
          stats.spinHits++;
          signalled = true;
          break;
        }
        if (polled != VK_TIMEOUT) return check(polled, "vkWaitForFences", error);
        const double elapsedUs =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - spinStart).count();
        if (elapsedUs >= static_cast<double>(readbackSpinUs)) break;
      }
    }
    if (!signalled &&
        !check(gt4_hw_vkWaitForFences(device, 1, &s.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences", error)) {
      return false;
    }
    stats.waitNs += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - waitStart).count());
    stats.waits++;
    s.endTick = 0;
    std::uint64_t startTick = 0;
    if (timestamps) {
      std::uint64_t ticks[2] = {};
      if (gt4_hwd_vkGetQueryPoolResults(device, timestamps, index * kTimestampsPerSlot, 2, sizeof ticks, ticks,
                                        sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
          ticks[1] >= ticks[0]) {
        stats.gpuNs += static_cast<std::uint64_t>(static_cast<double>(ticks[1] - ticks[0]) * timestampNs);
        stats.timedSubmissions++;
        s.endTick = ticks[1];
        startTick = ticks[0];
        if (s.idleFromTick != 0 && ticks[0] >= s.idleFromTick) {  // TASK-178: the first submission after a drain
          stats.drainIdleNs += static_cast<std::uint64_t>(static_cast<double>(ticks[0] - s.idleFromTick) * timestampNs);
          stats.drainIdles++;
        }
      }
      s.idleFromTick = 0;
      std::uint64_t pass[2 * kPassTimestampPairs] = {};
      if (s.passTimestamps != 0 &&
          gt4_hwd_vkGetQueryPoolResults(device, timestamps, index * kTimestampsPerSlot + 2u, 2u * s.passTimestamps,
                                        sizeof pass, pass, sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        for (std::uint32_t i = 0; i < s.passTimestamps; ++i) {
          if (pass[2 * i + 1] >= pass[2 * i]) {
            stats.drawPassNs += static_cast<std::uint64_t>(static_cast<double>(pass[2 * i + 1] - pass[2 * i]) * timestampNs);
          }
        }
      }
      s.passTimestamps = 0;
      if (catPool && s.catCount != 0 && s.endTick != 0) {  // TASK-182: tile the recording's span by the marks
        std::uint64_t marks[kCatPoints] = {};
        if (gt4_hwd_vkGetQueryPoolResults(device, catPool, index * kCatPoints, s.catCount, sizeof marks, marks,
                                          sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
          const auto add = [&](std::uint32_t kind, std::uint64_t from, std::uint64_t to) {
            if (to >= from) stats.catNs[kind] += static_cast<std::uint64_t>(static_cast<double>(to - from) * timestampNs);
          };
          add(kCatStartGap, startTick, marks[0]);
          for (std::uint32_t i = 0; i < s.catCount; ++i) add(s.catKinds[i], marks[i], i + 1 < s.catCount ? marks[i + 1] : s.endTick);
        }
      }
      s.catCount = 0;
    }
    if (statistics) {
      std::uint64_t counts[3] = {};
      if (gt4_hwd_vkGetQueryPoolResults(device, statistics, index, 1, sizeof counts, counts, sizeof counts,
                                        VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        stats.primitives += counts[0];
        stats.fragments += counts[1];
        stats.computeInvocations += counts[2];
      }
    }
    for (Image &image : s.doomed) retire(image);
    s.doomed.clear();
    s.inFlight = false;
    return true;
  }

  // Waits for everything submitted (a fence signals after every earlier submission on the queue too).
  //
  // TASK-182 step 3 (O2): with GT4_CORE2_WAIT_NEWEST_FENCE (on unless 0) only the newest submission's fence is waited for;
  // see wait_newest for why that is the same wait and what happens to the older slots. Without it, every slot is waited for
  // in slot order, which is what TASK-178 shipped. GT4_HW_READBACK_SPIN spins on the first fence either way.
  bool wait_all(std::string &error) { return waitNewestFence ? wait_newest(error) : wait_every_slot(error); }

  // The slot-by-slot wait: blocks on each in-flight slot in slot order, and does each one's bookkeeping as it completes.
  bool wait_every_slot(std::string &error) {
    const bool spin = readbackSpinUs != 0;
    bool firstWait = true;
    for (std::uint32_t i = 0; i < slotCount; ++i) {
      if (!slots[i].inFlight) continue;
      if (!wait_slot(i, error, spin && firstWait)) return false;
      firstWait = false;
    }
    return true;
  }

  // TASK-182 step 3 (row 5 of STUDY-ASYNC-READBACK 9.1, O2): wait for the fence of the submission that carries the read,
  // not for all of them.
  //
  // Why this is exact. Every submission of this class goes to the one queue `queue`, in vkQueueSubmit order, and Vulkan
  // signals a submission's fence only after that submission and every submission before it on the same queue have
  // completed (Vulkan 1.3 spec, fences: "if a fence is signaled, then all commands submitted before it ... have
  // completed"). The readback's own recording is submitted by submit(true), so its submission is the newest one when this
  // runs; waiting for that fence therefore implies the completion of every earlier slot, which is exactly what waiting all
  // of them returned. The guest bytes, the GS-memory write and the reduced words all become visible at the same moment as
  // before, so nothing observable moves; only the number of blocking driver calls falls (one wake-up instead of up to
  // slotCount).
  //
  // The older slots keep their bookkeeping (their timestamps are read and their doomed images released inside wait_slot)
  // until their slot is reused by begin_recording, which waits for it there, or until the final drain. That is the same
  // time that work took before - overlapped with the GPU finishing the newer submissions - only now it is off the read's
  // critical path instead of on it. Nothing else in the slot changes: the read's own slot is waited for here, and a
  // submission whose slot is still marked in flight is not reused before begin_recording waits for it.
  bool wait_newest(std::string &error) {
    std::uint32_t newest = kMaxSlots;  // none in flight
    for (std::uint32_t i = 0; i < slotCount; ++i) {
      if (!slots[i].inFlight) continue;
      if (newest == kMaxSlots || slots[i].serial > slots[newest].serial) newest = i;
    }
    if (newest == kMaxSlots) return true;
    stats.newestFenceWaits++;
    return wait_slot(newest, error, readbackSpinUs != 0);
  }

  bool begin_recording(std::string &error) {
    if (recording) return true;
    slot = (lastSubmitted + 1u) % slotCount;
    if (!wait_slot(slot, error)) return false;  // the submission before the last one: usually long done
    cmd = slots[slot].cmd;
    fence = slots[slot].fence;
    transientPool = slots[slot].pool;
    if (!check(gt4_hw_vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer", error)) return false;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!check(gt4_hw_vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer", error)) return false;
    if (!check(gt4_hwd_vkResetDescriptorPool(device, transientPool, 0), "vkResetDescriptorPool", error)) return false;
    if (timestamps) {
      gt4_hwd_vkCmdResetQueryPool(cmd, timestamps, slot * kTimestampsPerSlot, kTimestampsPerSlot);
      if (catPool) gt4_hwd_vkCmdResetQueryPool(cmd, catPool, slot * kCatPoints, kCatPoints);
      slots[slot].catCount = 0;
      gt4_hwd_vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, slot * kTimestampsPerSlot);
      slots[slot].passTimestamps = 0;
    }
    if (statistics) {
      gt4_hwd_vkCmdResetQueryPool(cmd, statistics, slot, 1);
      gt4_hwd_vkCmdBeginQuery(cmd, statistics, slot, 0);
    }
    transientSetsUsed = 0;
    drawSets.clear();
    for (Buffer *buffer : {&staging, &vertices, &uniforms}) {
      buffer->used = 0;
      buffer->base = slot * ring_share(*buffer);
    }
    readback.used = 0;  // the readback buffer is only used by waited submissions
    readback.base = 0;
    recording = true;
    // A barrier's first scope covers every command submitted before it on this queue (the presentation's copy of
    // the display image included), so the new recording never overwrites what an earlier submission still reads.
    barrierLabel = "begin recording";
    full_barrier();
    barrierLabel = "other";
    return true;
  }

  // TASK-178: after a waited submission every slot is idle; the next submission opens the K rule's window and the idle
  // diagnostics close on it.
  bool wait_all_drained(std::string &error) {
    if (!wait_all(error)) return false;
    stats.drains++;
    drainOpen = true;
    drainEndCpu = std::chrono::steady_clock::now();
    drainEndTick = slots[lastSubmitted].endTick;
    afterWait = submitAfterWait != 0;
    return true;
  }

  bool submit(bool wait, std::string &error, const char *why = __builtin_FUNCTION()) {
    if (!recording) {
      return wait ? wait_all_drained(error) : true;
    }
    flush_draws(why, "submission");
    end_render_pass(why);
    if (timestamps) {
      gt4_hwd_vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, slot * kTimestampsPerSlot + 1u);
    }
    if (statistics) gt4_hwd_vkCmdEndQuery(cmd, statistics, slot);
    if (!check(gt4_hw_vkEndCommandBuffer(cmd), "vkEndCommandBuffer", error)) return false;
    recording = false;
    if (!check(gt4_hw_vkResetFences(device, 1, &fence), "vkResetFences", error)) return false;
    VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    info.commandBufferCount = 1;
    info.pCommandBuffers = &cmd;
    if (drainOpen) {  // TASK-178 diagnostics: the first submission after a drain
      stats.drainGapCpuNs += static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - drainEndCpu).count());
      stats.drainGaps++;
      slots[slot].idleFromTick = drainEndTick;
      drainOpen = false;
    }
    const auto submitCall = [&] { return gt4_hw_vkQueueSubmit(queue, 1, &info, fence); };
    const VkResult submitted = queueLock ? queueLock->run(gt4::hw::QueueLock::Caller::gs_thread, submitCall) : submitCall();
    if (!check(submitted, "vkQueueSubmit", error)) return false;
    stats.submissions++;
    if (trace.on) ++SyncTrace::row(trace.submits, std::string(why) + (wait ? " (waited)" : "")).count;
    batchesSinceSubmit = 0;
    afterWait = false;
    slots[slot].inFlight = true;
    slots[slot].serial = ++submitCount;
    slots[slot].doomed.insert(slots[slot].doomed.end(), doomed.begin(), doomed.end());
    doomed.clear();
    lastSubmitted = slot;
    return wait ? wait_all_drained(error) : true;
  }

  bool allocate_set(VkDescriptorSetLayout layout, VkDescriptorSet &set, std::string &error) {
    if (transientSetsUsed >= kTransientSets) {
      if (!submit(false, error) || !begin_recording(error)) return false;
    }
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = transientPool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &layout;
    if (!check(gt4_hwd_vkAllocateDescriptorSets(device, &allocate, &set), "vkAllocateDescriptorSets", error)) return false;
    transientSetsUsed++;
    if (trace.on) {
      const char *kind = layout == drawSetLayout        ? "draw"
                         : layout == computeSetLayout   ? "compute"
                         : layout == decodeSetLayout    ? "decode"
                         : layout == deinterlaceSetLayout ? "deinterlace"
                         : layout == fxaaSetLayout      ? "fxaa"
                         : layout == presentSetLayout   ? "present"
                         : layout == selfCopySetLayout  ? "self copy"
                                                        : "other";
      ++SyncTrace::row(trace.setAllocations, kind).count;
    }
    return true;
  }

  // Every descriptor write goes through here (counted for TASK-182).
  void update_sets(std::uint32_t count, const VkWriteDescriptorSet *writes) {
    if (trace.on) {
      ++trace.updateCalls;
      trace.updateWrites += count;
    }
    gt4_hwd_vkUpdateDescriptorSets(device, count, writes, 0, nullptr);
  }

  const Image *image_of(ImageId id) const {
    if (id < 0 || static_cast<std::size_t>(id) >= images.size() || !images[static_cast<std::size_t>(id)].alive) {
      return nullptr;
    }
    return &images[static_cast<std::size_t>(id)];
  }

  // A compute set: binding 0 a storage buffer range, bindings 1..3 storage images.
  bool compute_set(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range, VkImageView a, VkImageView b,
                   VkImageView c, VkDescriptorSet &set, std::string &error) {
    if (!allocate_set(computeSetLayout, set, error)) return false;
    VkDescriptorBufferInfo bufferInfo{buffer, offset, range};
    VkDescriptorImageInfo imageInfo[3] = {{VK_NULL_HANDLE, a, VK_IMAGE_LAYOUT_GENERAL},
                                          {VK_NULL_HANDLE, b, VK_IMAGE_LAYOUT_GENERAL},
                                          {VK_NULL_HANDLE, c, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[4]{};
    for (std::uint32_t i = 0; i < 4; ++i) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = set;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      if (i == 0) {
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &bufferInfo;
      } else {
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &imageInfo[i - 1];
      }
    }
    update_sets(4, writes);
    return true;
  }

  void dispatch(VkPipeline pipeline, VkDescriptorSet set, const std::uint32_t *push, std::uint32_t pushWords,
                std::uint32_t width, std::uint32_t height) {
    gt4_hwd_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    gt4_hwd_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, computeLayout, 0, 1, &set, 0, nullptr);
    gt4_hwd_vkCmdPushConstants(cmd, computeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pushWords * 4u, push);
    gt4_hwd_vkCmdDispatch(cmd, (width + 7u) / 8u, (height + 7u) / 8u, 1);
    after_op();
  }
};

Gpu::Gpu() : impl_(std::make_unique<Impl>()) {}

Gpu::~Gpu() {
  Impl &s = *impl_;
  if (s.specThread.joinable()) {  // IN-437: the worker may be compiling against the device
    {
      std::lock_guard<std::mutex> lock(s.specMutex);
      s.specStop = true;
    }
    s.specWake.notify_all();
    s.specThread.join();
  }
  if (!s.device) return;
  s.save_shader_cache();  // IN-489
  if (s.pipelineCache) gt4_hwd_vkDestroyPipelineCache(s.device, s.pipelineCache, nullptr);
  std::string ignored;
  s.submit(false, ignored);      // finish the recording, if any
  s.wait_every_slot(ignored);    // every slot: the images and query pools below are about to be destroyed
  for (VkPipeline pipeline : s.specAll) gt4_hwd_vkDestroyPipeline(s.device, pipeline, nullptr);
  for (Image &image : s.images) s.release(image);
  for (Image &image : s.doomed) s.release(image);
  for (Image &image : s.recycled) s.release(image);
  for (Impl::SelfCopy &copy : s.selfCopies) s.release(copy.image);
  s.release(s.atlas);
  s.release(s.display);
  for (Image &image : s.deinterlaceHistory) s.release(image);
  s.release(s.deinterlaceWoven);
  s.release(s.deinterlaceOutput);
  s.release(s.fxaa);
  s.release(s.presentA);
  s.release(s.presentB);
  s.release(s.presentOut);
  if (s.timestamps) gt4_hwd_vkDestroyQueryPool(s.device, s.timestamps, nullptr);
  if (s.catPool) gt4_hwd_vkDestroyQueryPool(s.device, s.catPool, nullptr);
  if (s.statistics) gt4_hwd_vkDestroyQueryPool(s.device, s.statistics, nullptr);
  for (Impl::MemoryBlock &block : s.imageBlocks) gt4_hwd_vkFreeMemory(s.device, block.memory, nullptr);
  // IN-517: the rings and the descriptor pools, the largest objects, go with the renderer: a fixture makes dozens on one device.
  for (Buffer *buffer : {&s.staging, &s.vertices, &s.uniforms, &s.readback, &s.later, &s.glmTables}) {
    if (buffer->buffer) gt4_hwd_vkDestroyBuffer(s.device, buffer->buffer, nullptr);
    if (buffer->memory) gt4_hwd_vkFreeMemory(s.device, buffer->memory, nullptr);
  }
  for (Impl::Slot &slot : s.slots) {
    if (slot.pool) gt4_hwd_vkDestroyDescriptorPool(s.device, slot.pool, nullptr);
  }
  // The remaining objects live as long as the device (the renderer is destroyed with the backend).
}

bool Gpu::Ready() const { return impl_->ready; }

bool Gpu::Integrated() const { return impl_->integrated; }
bool Gpu::RoadSheenSkipped() const { return impl_->skipRoadSheen; }

bool Gpu::PrepareShaders(const ShaderProgress &progress, std::string &error) {
  Impl &s = *impl_;
  if (s.shaderCacheFolder.empty() || s.specMode != Impl::SpecMode::async || !s.pipelineCache) {
    error = "no shader cache to prepare (GT4_HW_SHADER_CACHE)";
    return false;
  }
  const Impl::PreparedStamp wanted = s.prepared_stamp();
  if (s.prepared_for(wanted)) return true;
  const std::vector<Impl::SpecKey> keys = s.variants_to_prepare();
  progress(0, keys.size());
  {
    std::lock_guard<std::mutex> lock(s.specMutex);
    s.specKnown = keys;
    s.specKnownFromFile = keys.size();
  }
  std::atomic<std::size_t> next{0}, done{0}, failures{0};
  // TASK-170: one variant at a time on a card where a parallel prepare died.
  const unsigned threadCount = s.serialPrepare ? 1u
                               : std::max(1u, std::thread::hardware_concurrency() > 1u ? std::thread::hardware_concurrency() - 1u : 1u);
  std::vector<std::thread> workers;
  for (unsigned i = 0; i < threadCount; ++i) {
    workers.emplace_back([&] {
#ifdef _WIN32
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);  // the launcher's window stays smooth
#endif
      for (std::size_t index = next.fetch_add(1); index < keys.size(); index = next.fetch_add(1)) {
        std::string compileError;
        if (!s.begin_compile(keys[index])) return;  // TASK-170 (tests: the run ends here)
        const VkPipeline pipeline = s.make_specialised(keys[index], compileError);
        s.end_compile(keys[index]);
        if (pipeline) {
          gt4_hwd_vkDestroyPipeline(s.device, pipeline, nullptr);  // what counts is what the driver keeps in the cache
        } else if (failures.fetch_add(1) == 0) {
          std::fprintf(stderr, "[core2] a variant did not compile (%s); its draws keep the uber pipeline\n", compileError.c_str());
        }
        done.fetch_add(1);
      }
    });
  }
  // A closed launcher or a cut power loses at most half a minute: the next Play finds the rest in the cache.
  auto lastSave = std::chrono::steady_clock::now();
  std::size_t reported = 0;
  while (done.load() < keys.size() && !s.abandoned.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (const std::size_t now = done.load(); now != reported && now < keys.size()) {
      reported = now;
      progress(now, keys.size());
    }
    if (std::chrono::steady_clock::now() - lastSave > std::chrono::seconds(30)) {
      {
        std::lock_guard<std::mutex> lock(s.specMutex);
        s.specKnownChanged = true;
      }
      s.save_shader_cache();
      lastSave = std::chrono::steady_clock::now();
    }
  }
  for (std::thread &worker : workers) worker.join();
  if (s.abandoned.load()) {
    error = "the prepare stopped inside a compile (GT4_CORE2_TEST_COMPILE_ABANDON)";
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(s.specMutex);
    s.specKnownChanged = true;
  }
  s.save_shader_cache();
  Impl::write_whole_file(s.shaderCacheFolder / "prepared.bin", &wanted, sizeof wanted, nullptr, 0);
  progress(keys.size(), keys.size());
  std::fprintf(stderr, "[core2] shader cache prepared: %zu variants, %zu failed, %u threads, %zu quarantined\n", keys.size(),
               failures.load(), threadCount, s.quarantined.size());
  return true;
}

Gpu::ShaderGuardState Gpu::ShaderGuard() const {
  ShaderGuardState state;
  state.quarantined = impl_->quarantined.size();
  {
    std::lock_guard<std::mutex> lock(impl_->compilingMutex);
    state.suspects = impl_->suspects.size();
  }
  state.serial = impl_->serialPrepare;
  return state;
}
std::uint32_t Gpu::Scale() const { return impl_->scale; }

std::uint32_t Gpu::MaxNativeSide() const {
  return std::min<std::uint32_t>(2048u, impl_->limits.maxImageDimension2D / std::max<std::uint32_t>(impl_->scale, 1u));
}

const Core2GpuStats &Gpu::Stats() const { return impl_->stats; }

std::string Gpu::BarrierReport() const {
  std::string report;
  for (const Impl::BarrierReason &reason : impl_->barrierReasons) {
    report += ' ';
    report += reason.label;
    if (*reason.kind) {
      report += '/';
      report += reason.kind;
    }
    report += '=' + std::to_string(reason.count);
  }
  return report;
}

// TASK-182 (GT4_CORE2_BARRIER_TRACE=1): the cumulative counts of how the GPU is synchronised and fed, one row per line:
// "[core2] sync <section> <key>: count=... [batches=... commands=... fragment_barriers=...]". Empty without the switch.
std::string Gpu::SyncReport(std::uint64_t vblanks) const {  const Impl::SyncTrace &t = impl_->trace;
  if (!t.on) return std::string();
  std::string report;
  char line[640];
  const auto rows = [&](const char *section, const std::vector<Impl::SyncTrace::Row> &list, bool pass) {
    for (const Impl::SyncTrace::Row &row : list) {
      if (pass) {
        std::snprintf(line, sizeof line, "[core2] sync vblank=%llu %s %s: count=%llu batches=%llu commands=%llu fragment_barriers=%llu\n",
                      static_cast<unsigned long long>(vblanks), section, row.key.c_str(), static_cast<unsigned long long>(row.count),
                      static_cast<unsigned long long>(row.batches), static_cast<unsigned long long>(row.commands),
                      static_cast<unsigned long long>(row.fragmentBarriers));
      } else {
        std::snprintf(line, sizeof line, "[core2] sync vblank=%llu %s %s: count=%llu\n", static_cast<unsigned long long>(vblanks),
                      section, row.key.c_str(), static_cast<unsigned long long>(row.count));
      }
      report += line;
    }
  };
  rows("fragment_barrier", t.fragmentBarriers, false);
  rows("pass_end", t.passEnds, true);
  rows("flush", t.flushes, false);
  rows("submit", t.submits, false);
  rows("set_allocation", t.setAllocations, false);
  rows("self_copy", t.selfCopies, false);
  std::snprintf(line, sizeof line,
                "[core2] sync vblank=%llu totals: full_barriers=%llu fragment_barriers=%llu render_passes=%llu draw_flushes=%llu "
                "batches_recorded=%llu draw_calls=%llu bind_descriptor_sets=%llu bind_pipelines=%llu bind_vertex_buffers=%llu "
                "set_viewports=%llu set_scissors=%llu push_constants=%llu descriptor_update_calls=%llu descriptor_writes=%llu "
                "submissions=%llu\n",
                static_cast<unsigned long long>(vblanks), static_cast<unsigned long long>(impl_->stats.fullBarriers),
                static_cast<unsigned long long>(impl_->stats.fragmentBarriers), static_cast<unsigned long long>(impl_->stats.renderPasses),
                static_cast<unsigned long long>(impl_->stats.drawFlushes), static_cast<unsigned long long>(t.batchesRecorded),
                static_cast<unsigned long long>(t.drawCalls), static_cast<unsigned long long>(t.bindDescriptorSets),
                static_cast<unsigned long long>(t.bindPipelines), static_cast<unsigned long long>(t.bindVertexBuffers),
                static_cast<unsigned long long>(t.setViewports), static_cast<unsigned long long>(t.setScissors),
                static_cast<unsigned long long>(t.pushConstants), static_cast<unsigned long long>(t.updateCalls),
                static_cast<unsigned long long>(t.updateWrites), static_cast<unsigned long long>(impl_->stats.submissions));
  report += line;
  return report;
}

void Gpu::NoteSelfSampleNoCopy(SelfSampleNoCopy why) {
  Impl &s = *impl_;
  if (!s.trace.on) return;
  ++Impl::SyncTrace::row(s.trace.selfCopies, why == SelfSampleNoCopy::Disjoint
                                                    ? "no copy: sampled rectangle disjoint from the drawn one"
                                                    : "no copy: frame untouched (IN-495)")
        .count;
}

bool Gpu::Init(const gpu::VulkanHandles &handles, std::uint32_t scale, float shift, std::string &error) {
  Impl &s = *impl_;
  if (s.ready) return true;
  s.instance = static_cast<VkInstance>(handles.instance);
  s.physical = static_cast<VkPhysicalDevice>(handles.physical);
  s.device = static_cast<VkDevice>(handles.device);
  s.queue = static_cast<VkQueue>(handles.queue);
  s.queueLock = handles.queueLock;
  s.scale = std::max<std::uint32_t>(scale, 1u);
  s.shift = shift;
  if (!s.instance || !s.physical || !s.device || !s.queue) {
    error = "CORE2 needs instance, physical device, device and queue";
    return false;
  }
  std::vector<std::string> missing;
  if (!gt4::hw::vulkan_device::load_device_entry_points(s.instance, missing)) {
    error = "missing Vulkan entry points:";
    for (const std::string &name : missing) error += " " + name;
    return false;
  }
  gt4_hwd_vkGetPhysicalDeviceMemoryProperties(s.physical, &s.memoryProperties);
  VkPhysicalDeviceProperties properties{};
  gt4_hw_vkGetPhysicalDeviceProperties(s.physical, &properties);
  s.limits = properties.limits;
  // TASK-179: from the device profile, by the same rule (VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU); from the device type when
  // the device's creator did not probe it.
  s.integrated = handles.profile.probed ? handles.profile.integrated_defaults
                                        : gt4::hw::integrated_defaults(static_cast<std::uint32_t>(properties.deviceType));
  // TASK-182 step 3: the K rule's device default uses the whole profile (class, unified memory). The creator of the device may
  // not have probed it (a fixture, a test path): classify the physical device here then, so the decision is never a default
  // that happens to be "discrete".
  gt4::hw::DeviceProfile deviceProfile = handles.profile;
  if (!deviceProfile.probed) {
    deviceProfile.vendor_id = properties.vendorID;
    deviceProfile.device_type = static_cast<std::uint32_t>(properties.deviceType);
    deviceProfile.device_class = gt4::hw::classify_device(deviceProfile.vendor_id, deviceProfile.device_type);
    deviceProfile.integrated_defaults = gt4::hw::integrated_defaults(deviceProfile.device_type);
  }
  s.deviceProperties = properties;  // IN-507
  {
    if (s.clearWholeSetting < 0) s.clearTopZero = s.integrated || scale > 1;  // IN-494
    const char *plainSwitch = std::getenv("GT4_CORE2_PLAIN_DISJOINT");  // IN-496
    s.plainDisjoint = plainSwitch ? std::strcmp(plainSwitch, "0") != 0 : s.integrated;
    // IN-501: GT4's sheen on the road, a full-screen pass that adds a small target's picture weighted by destination
    // alpha; Performance Mode left it out and the road got darker. TASK-180 test5: its own effect toggle now,
    // GT4_CORE2_PERF_ROAD_SHEEN=1 turns it off (the older GT4_CORE2_ROAD_SHEEN=0 says the same and wins; the
    // GT4_CORE2_PERFORMANCE bundle still turns every item off).
    const char *sheenSwitch = std::getenv("GT4_CORE2_ROAD_SHEEN");
    const char *sheenEffect = std::getenv("GT4_CORE2_PERF_ROAD_SHEEN");
    const char *performanceSwitch = std::getenv("GT4_CORE2_PERFORMANCE");
    s.skipRoadSheen = sheenSwitch  ? std::strcmp(sheenSwitch, "0") == 0
                      : sheenEffect ? std::strcmp(sheenEffect, "0") != 0
                      : performanceSwitch && std::strcmp(performanceSwitch, "0") != 0;
  }

  VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolInfo.queueFamilyIndex = handles.queueFamily;
  if (!check(gt4_hw_vkCreateCommandPool(s.device, &poolInfo, nullptr, &s.pool), "vkCreateCommandPool", error)) return false;
  VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocate.commandPool = s.pool;
  allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate.commandBufferCount = 1;
  for (Impl::Slot &slot : s.slots) {
    if (!check(gt4_hw_vkAllocateCommandBuffers(s.device, &allocate, &slot.cmd), "vkAllocateCommandBuffers", error)) {
      return false;
    }
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (!check(gt4_hw_vkCreateFence(s.device, &fenceInfo, nullptr, &slot.fence), "vkCreateFence", error)) return false;
  }
  if (s.slotCount == 0) s.slotCount = s.integrated ? 8u : 16u;  // IN-517
  if (s.submitEvery == Impl::kSubmitEveryAtInit) s.submitEvery = s.integrated ? 32u : 64u;  // TASK-134
  // TASK-178: the K rule's default; TASK-182 step 3: by device class. A tiler or a unified-memory device (a phone, an APU)
  // submits again 8 batches after a waited readback; a discrete GPU keeps 0 until its own A/B shows a gain (TASK-178 9.3:
  // no clear wall gain on the desktop). GT4_CORE2_SUBMIT_AFTER_WAIT always overrides, and the line below says which.
  char afterWaitReason[128] = "GT4_CORE2_SUBMIT_AFTER_WAIT";
  if (s.submitAfterWait == Impl::kSubmitEveryAtInit) {
    // Never later than the normal cadence: with GT4_CORE2_SUBMIT_EVERY below 8 (or 0, submissions only when needed) the device
    // default must not delay the submission the user asked for.
    s.submitAfterWait = std::min(gt4::hw::k_rule_default_batches(deviceProfile), s.submitEvery);
    std::snprintf(afterWaitReason, sizeof afterWaitReason, "device default, %s",
                  gt4::hw::k_rule_default_reason(deviceProfile));
  }
  const VkBufferUsageFlags stagingUsage =
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  // IN-476: on an integrated GPU the CPU writes these through its caches (the GPU shares them) rather than through the
  // write-combined type the first match would give. GT4_CORE2_CACHED_UPLOAD=0 / 1 forces it off / on (A/B).
  const char *cachedUpload = std::getenv("GT4_CORE2_CACHED_UPLOAD");
  const VkMemoryPropertyFlags uploadMemory =
      (cachedUpload ? std::strcmp(cachedUpload, "0") != 0 : s.integrated) ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0;
  // TASK-179: the rings sized from their heap (gt4::hw::plan_core2_rings), today's sizes as the cap. IN-517: today's sizes
  // are for 8 slots and a recording's share stays the same with more. Fewer shares than slots (a small heap): fewer slots.
  gt4::hw::RingPlan rings;
  if (!s.plan_rings(handles.profile, stagingUsage, uploadMemory, std::max<std::uint32_t>(s.slotCount, 8u), rings, error)) return false;
  s.slotCount = std::min(s.slotCount, rings.shares);
  s.readbackBytes = rings.readback;
  s.lastSubmitted = s.slotCount - 1u;  // S4: the first recording takes slot 0
  if (s.limits.timestampComputeAndGraphics && s.limits.timestampPeriod > 0.0f) {
    VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queryInfo.queryCount = Impl::kMaxSlots * Impl::kTimestampsPerSlot;
    if (gt4_hwd_vkCreateQueryPool(s.device, &queryInfo, nullptr, &s.timestamps) == VK_SUCCESS) {
      s.timestampNs = s.limits.timestampPeriod;
      if (s.catOn) {
        queryInfo.queryCount = Impl::kMaxSlots * Impl::kCatPoints;
        if (gt4_hwd_vkCreateQueryPool(s.device, &queryInfo, nullptr, &s.catPool) != VK_SUCCESS) s.catPool = VK_NULL_HANDLE;
      }
    }
  }
  {
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    gt4_hw_vkGetPhysicalDeviceFeatures2(s.physical, &features);
    // IN-465: the pipeline statistics query (prims, fragments and compute invocations of the reports) is diagnostics
    // only and some drivers pay for it on every command buffer: GT4_CORE2_GPU_STATISTICS=1 turns it on (off, the
    // report's prims, fragments and compute read 0).
    const char *statisticsSwitch = std::getenv("GT4_CORE2_GPU_STATISTICS");
    const bool statisticsOn = statisticsSwitch != nullptr && std::strcmp(statisticsSwitch, "1") == 0;
    if (features.features.pipelineStatisticsQuery && statisticsOn) {
      VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      queryInfo.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
      queryInfo.queryCount = Impl::kMaxSlots;
      queryInfo.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
                                     VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT |
                                     VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
      if (gt4_hwd_vkCreateQueryPool(s.device, &queryInfo, nullptr, &s.statistics) != VK_SUCCESS) {
        s.statistics = VK_NULL_HANDLE;
      }
    }
  }

  if (!s.make_buffer(s.staging, rings.staging, stagingUsage, uploadMemory, error)) return false;
  if (!s.make_buffer(s.vertices, rings.vertex, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, uploadMemory, error)) return false;
  if (!s.make_buffer(s.uniforms, rings.uniform, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, uploadMemory, error)) return false;
  // TASK-180 test4 (P1-C): one recording's share of each ring, for the high-water marks' "/share".
  s.stats.ringShareStagingBytes = static_cast<std::uint64_t>(s.ring_share(s.staging));
  s.stats.ringShareVertexBytes = static_cast<std::uint64_t>(s.ring_share(s.vertices));
  s.stats.ringShareUniformBytes = static_cast<std::uint64_t>(s.ring_share(s.uniforms));
  {
    // TASK-180 test4 (P1-B): the same line, with every type's letters from gt4::hw::memory_type_letters (Z lazily
    // allocated, P protected, both new here) and the heap it belongs to. The prefix stays: bench scripts read it.
    std::string types;
    for (std::uint32_t i = 0; i < s.memoryProperties.memoryTypeCount; ++i) {
      types += " " + std::to_string(i) + ":" + gt4::hw::memory_type_letters(s.memoryProperties.memoryTypes[i].propertyFlags) +
               "@" + std::to_string(s.memoryProperties.memoryTypes[i].heapIndex);
    }
    std::fprintf(stderr, "[core2] memory types (L local, V visible, C coherent, H cached, Z lazy, P protected):%s; upload buffers %s\n",
                 types.c_str(), uploadMemory ? "prefer cached" : "first visible");
  }
  if (!s.make_buffer(s.readback, s.readbackBytes, stagingUsage, VK_MEMORY_PROPERTY_HOST_CACHED_BIT, error)) return false;
  if (!s.make_buffer(s.later, static_cast<VkDeviceSize>(Impl::kLaterSlots) * Impl::kLaterWords * 4u, stagingUsage,
                     VK_MEMORY_PROPERTY_HOST_CACHED_BIT, error)) return false;

  // Draw set (hw_pixel_uber.frag): 0 pixel state, 1 frame, 2 depth, 3 1x texel atlas, 4 a target sampled at scale,
  // 5 the depth image again (IN-451: the speculative depth read, a descriptor of its own).
  {
    VkDescriptorSetLayoutBinding bindings[6]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    for (std::uint32_t i = 1; i < 6; ++i) {
      bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = 6;
    info.pBindings = bindings;
    if (!check(gt4_hwd_vkCreateDescriptorSetLayout(s.device, &info, nullptr, &s.drawSetLayout),
               "vkCreateDescriptorSetLayout", error)) return false;
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, kGraphicsPushBytes};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &s.drawSetLayout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    if (!check(gt4_hwd_vkCreatePipelineLayout(s.device, &layout, nullptr, &s.drawLayout), "vkCreatePipelineLayout",
               error)) return false;
  }
  // Compute set: 0 a storage buffer, 1..3 storage images.
  {
    VkDescriptorSetLayoutBinding bindings[4]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    for (std::uint32_t i = 1; i < 4; ++i) {
      bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    info.bindingCount = 4;
    info.pBindings = bindings;
    if (!check(gt4_hwd_vkCreateDescriptorSetLayout(s.device, &info, nullptr, &s.computeSetLayout),
               "vkCreateDescriptorSetLayout", error)) return false;
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, kComputePushBytes};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &s.computeSetLayout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    if (!check(gt4_hwd_vkCreatePipelineLayout(s.device, &layout, nullptr, &s.computeLayout), "vkCreatePipelineLayout",
               error)) return false;
  }
  {
    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kTransientSets},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5 * kTransientSets + 64},
                                     {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kTransientSets}};
    VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    info.maxSets = kTransientSets;
    info.poolSizeCount = 3;
    info.pPoolSizes = sizes;
    for (std::uint32_t i = 0; i < s.slotCount; ++i) {
      Impl::Slot &slot = s.slots[i];
      if (!check(gt4_hwd_vkCreateDescriptorPool(s.device, &info, nullptr, &slot.pool), "vkCreateDescriptorPool", error)) {
        return false;
      }
    }
  }
  // One subpass with no attachments; its self-dependency lets a draw see the storage-image stores of the draws before
  // it (the fragment barrier between batches and, without pixel interlock, between primitives).
  {
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    VkSubpassDependency self{};
    self.srcSubpass = 0;
    self.dstSubpass = 0;
    self.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    self.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    self.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    self.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &self;
    if (!check(gt4_hwd_vkCreateRenderPass(s.device, &info, nullptr, &s.renderPass), "vkCreateRenderPass", error)) {
      return false;
    }
    s.framebufferWidth = std::min<std::uint32_t>(s.limits.maxFramebufferWidth, 2048u * s.scale);
    s.framebufferHeight = std::min<std::uint32_t>(s.limits.maxFramebufferHeight, 2048u * s.scale);
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = s.renderPass;
    fb.width = s.framebufferWidth;
    fb.height = s.framebufferHeight;
    fb.layers = 1;
    if (!check(gt4_hwd_vkCreateFramebuffer(s.device, &fb, nullptr, &s.framebuffer), "vkCreateFramebuffer", error)) {
      return false;
    }
  }
  s.open_shader_cache();  // IN-489
  s.vertexModule = s.make_module(hwgs::shaders::hw_vertex_core2_words, hwgs::shaders::hw_vertex_core2_word_count, error);
  s.fragmentModule =
      s.make_module(hwgs::shaders::hw_pixel_uber_core2_words, hwgs::shaders::hw_pixel_uber_core2_word_count, error);
  if (!s.vertexModule || !s.fragmentModule) return false;
  {
    // TASK-179: points of `scale` pixels as points where the device was created with largePoints and draws them that
    // large (and always at 1x), as two triangles elsewhere.
    const char *quadSwitch = std::getenv("GT4_CORE2_POINT_QUADS");
    const bool sizesFit = handles.profile.points_of_size(static_cast<float>(s.scale));
    s.pointQuads = quadSwitch ? std::strcmp(quadSwitch, "0") != 0 : !sizesFit;
    if (s.pointQuads) {
      s.pointQuadModule = s.make_module(hwgs::shaders::hw_vertex_point_quad_words, hwgs::shaders::hw_vertex_point_quad_word_count, error);
      if (!s.pointQuadModule) return false;
    }
    std::fprintf(stderr, "[core2] points at %ux: %s\n", s.scale,
                 s.pointQuads ? "two triangles each (no largePoints, or a smaller largest point size)" : "points (largePoints or 1x)");
  }
  s.pipelines[0] = s.make_graphics(VK_PRIMITIVE_TOPOLOGY_POINT_LIST, s.fragmentModule, error, nullptr, 0, false, s.pointQuads);
  s.pipelines[1] = s.make_graphics(VK_PRIMITIVE_TOPOLOGY_LINE_LIST, s.fragmentModule, error);
  s.pipelines[2] = s.make_graphics(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, s.fragmentModule, error);
  if (!s.pipelines[0] || !s.pipelines[1] || !s.pipelines[2]) return false;
  // IN-481
  {
    const char *ropSwitch = std::getenv("GT4_CORE2_ROP");
    const bool wanted = ropSwitch ? std::strcmp(ropSwitch, "0") != 0 : s.integrated;
    s.dualSourceBlend = handles.dualSourceBlend;
    // TASK-179: the path's own format need (the device check no longer refuses a device without it): the targets'
    // R8G8B8A8_UNORM views as blendable colour attachments.
    VkFormatProperties rgba8{};
    gt4_hw_vkGetPhysicalDeviceFormatProperties(s.physical, VK_FORMAT_R8G8B8A8_UNORM, &rgba8);
    const VkFormatFeatureFlags ropFeatures = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
    const bool ropFormat = (rgba8.optimalTilingFeatures & ropFeatures) == ropFeatures;
    if (wanted && !ropFormat) std::fprintf(stderr, "[core2] colour attachment path (IN-481): R8G8B8A8_UNORM is not a blendable colour attachment here\n");
    if (wanted && ropFormat && handles.independentBlend && s.specMode != Impl::SpecMode::off && s.deferDraws) {
      VkAttachmentDescription attachment{};
      attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
      attachment.samples = VK_SAMPLE_COUNT_1_BIT;
      attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      attachment.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
      attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
      const VkAttachmentDescription attachments[2] = {attachment, attachment};
      const VkAttachmentReference references[2] = {{0, VK_IMAGE_LAYOUT_GENERAL}, {1, VK_IMAGE_LAYOUT_GENERAL}};
      for (std::uint32_t count = 1; count <= 2; ++count) {
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = count;
        subpass.pColorAttachments = references;
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        info.attachmentCount = count;
        info.pAttachments = attachments;
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        if (!check(gt4_hwd_vkCreateRenderPass(s.device, &info, nullptr, count == 1 ? &s.ropPassFrame : &s.ropPassBoth),
                   "vkCreateRenderPass", error)) {
          return false;
        }
      }
      s.ropModule = s.make_module(hwgs::shaders::hw_pixel_uber_core2_rop_words, hwgs::shaders::hw_pixel_uber_core2_rop_word_count, error);
      s.ropBlendModule = s.make_module(hwgs::shaders::hw_pixel_uber_core2_rop_blend_words, hwgs::shaders::hw_pixel_uber_core2_rop_blend_word_count, error);
      if (!s.ropModule || !s.ropBlendModule) return false;
      s.ropOn = true;
    }
    std::fprintf(stderr, "[core2] colour attachment path (IN-481) %s" "\n", s.ropOn ? "on" : "off");
  }
  if (handles.pixelInterlock) {
    s.interlockModule = s.make_module(hwgs::shaders::hw_pixel_uber_core2_interlock_words, hwgs::shaders::hw_pixel_uber_core2_interlock_word_count, error);
    if (!s.interlockModule) return false;
    s.interlockTriangles = s.make_graphics(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, s.interlockModule, error);
    if (!s.interlockTriangles) return false;
    s.interlock = true;
  }
  s.patchModule = s.make_module(hwgs::shaders::core2_patch_words, hwgs::shaders::core2_patch_word_count, error);
  s.reduceModule = s.make_module(hwgs::shaders::core2_reduce_words, hwgs::shaders::core2_reduce_word_count, error);
  s.toneModule = s.make_module(hwgs::shaders::core2_tone_words, hwgs::shaders::core2_tone_word_count, error);
  s.composeModule = s.make_module(hwgs::shaders::core2_compose_words, hwgs::shaders::core2_compose_word_count, error);
  if (!s.patchModule || !s.reduceModule || !s.toneModule || !s.composeModule) return false;
  s.patchPipeline = s.make_compute(s.patchModule, error);
  s.reducePipeline = s.make_compute(s.reduceModule, error);
  s.tonePipeline = s.make_compute(s.toneModule, error);
  s.composePipeline = s.make_compute(s.composeModule, error);
  if (!s.patchPipeline || !s.reducePipeline || !s.tonePipeline || !s.composePipeline) return false;
  s.fillModule = s.make_module(hwgs::shaders::core2_fill_words, hwgs::shaders::core2_fill_word_count, error);
  if (!s.fillModule) return false;
  s.fillPipeline = s.make_compute(s.fillModule, error);
  if (!s.fillPipeline) return false;
  // TASK-182 R1: the self copy draw's descriptor set (0 the source view, 1 the pooled copy view) and its pipeline, only
  // when the switch asks for the path (GT4_CORE2_SELF_COPY_DRAW).
  if (s.selfCopyDraw) {
    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = bindings;
    if (!check(gt4_hwd_vkCreateDescriptorSetLayout(s.device, &layoutInfo, nullptr, &s.selfCopySetLayout),
               "vkCreateDescriptorSetLayout", error)) {
      return false;
    }
    VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayout.setLayoutCount = 1;
    pipelineLayout.pSetLayouts = &s.selfCopySetLayout;
    if (!check(gt4_hwd_vkCreatePipelineLayout(s.device, &pipelineLayout, nullptr, &s.selfCopyLayout),
               "vkCreatePipelineLayout", error)) {
      return false;
    }
    s.selfCopyVertexModule = s.make_module(hwgs::shaders::core2_self_copy_vertex_words,
                                           hwgs::shaders::core2_self_copy_vertex_word_count, error);
    s.selfCopyModule =
        s.make_module(hwgs::shaders::core2_self_copy_words, hwgs::shaders::core2_self_copy_word_count, error);
    if (!s.selfCopyVertexModule || !s.selfCopyModule) return false;
    s.selfCopyPipeline = s.make_self_copy_pipeline(error);
    if (!s.selfCopyPipeline) return false;
  }
  {
    const std::uint32_t deinterlaceCounts[7] = {gpu::kGpuDeinterlaceHistory, 0, 0, 0, 1, 1, 1};
    const std::uint32_t fxaaCounts[2] = {1, 1};
    if (!s.image_layouts(deinterlaceCounts, 7, 16 * 4, s.deinterlaceSetLayout, s.deinterlaceLayout, error) ||
        !s.image_layouts(fxaaCounts, 2, 2 * 4, s.fxaaSetLayout, s.fxaaLayout, error)) {
      return false;
    }
    // TASK-179: the kernel indexes its history images with a push constant, which needs shaderStorageImageArrayDynamicIndexing;
    // without it the constant-index build runs (the same pixels).
    s.deinterlaceModule = handles.profile.storage_image_array_dynamic_indexing_enabled
                              ? s.make_module(hwgs::shaders::hw_deinterlace_scaled_words,
                                              hwgs::shaders::hw_deinterlace_scaled_word_count, error)
                              : s.make_module(hwgs::shaders::hw_deinterlace_scaled_constant_words,
                                              hwgs::shaders::hw_deinterlace_scaled_constant_word_count, error);
    s.fxaaModule = s.make_module(hwgs::shaders::hw_postfx_fxaa_words, hwgs::shaders::hw_postfx_fxaa_word_count, error);
    if (!s.deinterlaceModule || !s.fxaaModule) return false;
    s.deinterlacePipeline = s.make_compute(s.deinterlaceModule, error, s.deinterlaceLayout);
    s.fxaaPipeline = s.make_compute(s.fxaaModule, error, s.fxaaLayout);
    if (!s.deinterlacePipeline || !s.fxaaPipeline) return false;
    if (!s.image_layouts(fxaaCounts, 2, 6 * 4, s.presentSetLayout, s.presentLayout, error)) return false;
    s.presentModule = s.make_module(hwgs::shaders::core2_present_scale_words,
                                    hwgs::shaders::core2_present_scale_word_count, error);
    if (!s.presentModule) return false;
    s.presentPipeline = s.make_compute(s.presentModule, error, s.presentLayout);
    if (!s.presentPipeline) return false;
  }

  // ---- GLM and the decode kernel ----
  {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = 4u << 20;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!check(gt4_hwd_vkCreateBuffer(s.device, &info, nullptr, &s.glm), "vkCreateBuffer", error)) return false;
    VkMemoryRequirements requirements{};
    gt4_hwd_vkGetBufferMemoryRequirements(s.device, s.glm, &requirements);
    VkMemoryAllocateInfo allocateGlm{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocateGlm.allocationSize = requirements.size;
    if (!s.memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
                       allocateGlm.memoryTypeIndex)) {
      error = "no device-local memory type for the CORE2 GLM";
      return false;
    }
    s.glmType = allocateGlm.memoryTypeIndex;
    if (!check(gt4_hwd_vkAllocateMemory(s.device, &allocateGlm, nullptr, &s.glmMemory), "vkAllocateMemory", error) ||
        !check(gt4_hwd_vkBindBufferMemory(s.device, s.glm, s.glmMemory, 0), "vkBindBufferMemory", error)) {
      return false;
    }
    const memory::GpuAddressTables tables = memory::gpuAddressTables();
    std::vector<std::uint32_t> words(tables.directory);
    words.insert(words.end(), tables.entries.begin(), tables.entries.end());
    s.glmTableBytes = words.size() * sizeof(std::uint32_t);
    if (!s.make_buffer(s.glmTables, s.glmTableBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, error)) return false;
    std::memcpy(s.glmTables.mapped, words.data(), static_cast<std::size_t>(s.glmTableBytes));
    VkDescriptorSetLayoutBinding bindings[5]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[4] = {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount = 5;
    setInfo.pBindings = bindings;
    if (!check(gt4_hwd_vkCreateDescriptorSetLayout(s.device, &setInfo, nullptr, &s.decodeSetLayout),
               "vkCreateDescriptorSetLayout", error)) return false;
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, kComputePushBytes};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &s.decodeSetLayout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    if (!check(gt4_hwd_vkCreatePipelineLayout(s.device, &layout, nullptr, &s.decodeLayout), "vkCreatePipelineLayout",
               error)) return false;
    s.decodeModule = s.make_module(hwgs::shaders::hw_glm_decode_words, hwgs::shaders::hw_glm_decode_word_count, error);
    if (!s.decodeModule) return false;
    VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline.stage.module = s.decodeModule;
    pipeline.stage.pName = "main";
    pipeline.layout = s.decodeLayout;
    if (!check(gt4_hwd_vkCreateComputePipelines(s.device, s.pipelineCache, 1, &pipeline, nullptr, &s.decodePipeline),
               "vkCreateComputePipelines", error)) return false;
    s.patchGlmModule =
        s.make_module(hwgs::shaders::core2_patch_glm_words, hwgs::shaders::core2_patch_glm_word_count, error);
    if (!s.patchGlmModule) return false;
    pipeline.stage.module = s.patchGlmModule;
    if (!check(gt4_hwd_vkCreateComputePipelines(s.device, s.pipelineCache, 1, &pipeline, nullptr, &s.patchGlmPipeline),
               "vkCreateComputePipelines", error)) return false;
    // IN-408: the target CLUT kernel shares the decode set (0 GLM, 1 tables, 2 inverse table, 3 target, 5 CLUT).
    s.clutTargetModule =
        s.make_module(hwgs::shaders::core2_clut_target_words, hwgs::shaders::core2_clut_target_word_count, error);
    if (!s.clutTargetModule) return false;
    pipeline.stage.module = s.clutTargetModule;
    if (!check(gt4_hwd_vkCreateComputePipelines(s.device, s.pipelineCache, 1, &pipeline, nullptr, &s.clutTargetPipeline),
               "vkCreateComputePipelines", error)) return false;
    s.materializeModule =
        s.make_module(hwgs::shaders::core2_materialize_words, hwgs::shaders::core2_materialize_word_count, error);
    if (!s.materializeModule) return false;
    pipeline.stage.module = s.materializeModule;
    if (!check(gt4_hwd_vkCreateComputePipelines(s.device, s.pipelineCache, 1, &pipeline, nullptr, &s.materializePipeline),
               "vkCreateComputePipelines", error)) return false;
  }
  // IN-408: the GPU CLUT and the inverse of the CT32 in-page layout (word of a page -> pixel x | y << 8).
  {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = 512 * 4;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!check(gt4_hwd_vkCreateBuffer(s.device, &info, nullptr, &s.clut), "vkCreateBuffer", error)) return false;
    VkMemoryRequirements requirements{};
    gt4_hwd_vkGetBufferMemoryRequirements(s.device, s.clut, &requirements);
    VkMemoryAllocateInfo allocateClut{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocateClut.allocationSize = requirements.size;
    if (!s.memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
                       allocateClut.memoryTypeIndex)) {
      error = "no device-local memory type for the CORE2 GPU CLUT";
      return false;
    }
    s.clutType = allocateClut.memoryTypeIndex;
    if (!check(gt4_hwd_vkAllocateMemory(s.device, &allocateClut, nullptr, &s.clutMemory), "vkAllocateMemory", error) ||
        !check(gt4_hwd_vkBindBufferMemory(s.device, s.clut, s.clutMemory, 0), "vkBindBufferMemory", error)) {
      return false;
    }
    const memory::GpuAddressTables tables = memory::gpuAddressTables();
    const std::uint32_t tableOffset = tables.directory[0];  // PSMCT32 (code 0)
    const std::uint32_t pageWidth = tables.directory[1];
    const std::uint32_t pageHeight = tables.directory[2];
    if (!s.make_buffer(s.inverseCt32, 2048 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, error)) return false;
    auto *inverse = static_cast<std::uint32_t *>(s.inverseCt32.mapped);
    for (std::uint32_t y = 0; y < pageHeight; ++y) {
      for (std::uint32_t x = 0; x < pageWidth; ++x) {
        const std::uint32_t entry = tables.entries[tableOffset + y * pageWidth + x];
        const std::uint32_t word = (entry & 31u) * 64u + ((entry >> 5) & 255u) / 4u;
        inverse[word & 2047u] = x | (y << 8);
      }
    }
  }

  if (!s.make_image(s.atlas, kAtlasSide, kAtlasSide, 1, error)) return false;
  if (!s.make_image(s.display, 640u * s.scale, 448u * s.scale, 1, error)) return false;
  // TASK-180 test4 (P1-B): where every core 2 object landed. The phone's answer is the UMA question in one line: the
  // rings on a host-visible device-local type, the GLM, CLUT and images on an L type. The shape comes from the probe;
  // a device whose creator did not probe says unknown.
  {
    auto type_text = [&s](std::uint32_t type) {
      if (type == UINT32_MAX || type >= s.memoryProperties.memoryTypeCount) return std::string("none");
      return std::to_string(type) + ":" + gt4::hw::memory_type_letters(s.memoryProperties.memoryTypes[type].propertyFlags);
    };
    auto mib = [](VkDeviceSize bytes) { return std::to_string(static_cast<unsigned long long>(bytes >> 20)); };
    const gt4::hw::MemoryShape shape = handles.profile.probed ? handles.profile.memory_shape : gt4::hw::MemoryShape::unknown;
    std::fprintf(stderr,
                 "[core2] placement (memory shape %s%s): staging %s %s MiB, vertices %s %s MiB, uniforms %s %s MiB, "
                 "readback %s %s MiB, later %s %s MiB, glm_tables %s %s MiB, inverse_ct32 %s %s MiB, glm %s, clut %s, "
                 "images %s\n",
                 gt4::hw::memory_shape_name(shape),
                 handles.profile.probed && handles.profile.memory_shape_forced ? ", forced" : "",
                 type_text(s.staging.type).c_str(), mib(s.staging.size).c_str(),
                 type_text(s.vertices.type).c_str(), mib(s.vertices.size).c_str(),
                 type_text(s.uniforms.type).c_str(), mib(s.uniforms.size).c_str(),
                 type_text(s.readback.type).c_str(), mib(s.readback.size).c_str(),
                 type_text(s.later.type).c_str(), mib(s.later.size).c_str(),
                 type_text(s.glmTables.type).c_str(), mib(s.glmTables.size).c_str(),
                 type_text(s.inverseCt32.type).c_str(), mib(s.inverseCt32.size).c_str(), type_text(s.glmType).c_str(),
                 type_text(s.clutType).c_str(), type_text(s.imageType).c_str());
  }
  s.queue_known_variants();  // IN-489
  s.ready = true;
  std::fprintf(stderr, "[core2] ready: scale %u, shift %.3f, pixel interlock %s, max native side %u\n", s.scale,
               static_cast<double>(s.shift), s.interlock ? "yes" : "no", MaxNativeSide());
  std::fprintf(stderr, "[core2] submissions: %u slots, every %u batches, after a wait %u batches (TASK-182 step 3: %s; "
               "0 = off)\n",
               s.slotCount, s.submitEvery, s.submitAfterWait, afterWaitReason);
  std::fprintf(stderr, "[core2] readback wait: %s%s%s\n",
               s.waitNewestFence ? "newest fence only (GT4_CORE2_WAIT_NEWEST_FENCE=1)" : "every slot (GT4_CORE2_WAIT_NEWEST_FENCE=0)",
               s.readbackSpinUs != 0 ? ", bounded spin" : ", block at once",
               s.readbackSpinUs != 0 ? " (GT4_HW_READBACK_SPIN=1)" : " (GT4_HW_READBACK_SPIN off)");
  return true;
}

ImageId Gpu::CreateImage(std::uint32_t nativeWidth, std::uint32_t nativeHeight, bool scaled, std::string &error) {
  Impl &s = *impl_;
  Image image;
  if (!s.make_image(image, nativeWidth, nativeHeight, scaled ? s.scale : 1u, error)) return kNoImage;
  ImageId id;
  if (!s.freeIds.empty()) {
    id = s.freeIds.back();
    s.freeIds.pop_back();
    s.images[static_cast<std::size_t>(id)] = image;
  } else {
    id = static_cast<ImageId>(s.images.size());
    s.images.push_back(image);
  }
  return id;
}

bool Gpu::GrowImage(ImageId id, std::uint32_t nativeWidth, std::uint32_t nativeHeight, std::string &error) {
  Impl &s = *impl_;
  if (!s.image_of(id)) {
    error = "CORE2 grows an image that does not exist";
    return false;
  }
  Image &old = s.images[static_cast<std::size_t>(id)];
  if (nativeWidth <= old.nativeWidth && nativeHeight <= old.nativeHeight) return true;
  s.flush_draws();
  Image grown;
  if (!s.make_image(grown, std::max(nativeWidth, old.nativeWidth), std::max(nativeHeight, old.nativeHeight), old.scale,
                    error)) {
    return false;
  }
  VkImageCopy region{};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.extent = {old.width, old.height, 1};
  gt4_hwd_vkCmdCopyImage(s.cmd, old.image, VK_IMAGE_LAYOUT_GENERAL, grown.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  s.stats.gpuCopyImageBytes += static_cast<std::uint64_t>(old.width) * old.height * 4u;  // P1-C
  s.full_barrier();
  s.doom(old);
  old = grown;
  return true;
}

void Gpu::DestroyImage(ImageId id) {
  Impl &s = *impl_;
  if (!s.image_of(id)) return;
  s.doom(s.images[static_cast<std::size_t>(id)]);
  s.freeIds.push_back(id);
}

void Gpu::ImageSize(ImageId id, std::uint32_t &nativeWidth, std::uint32_t &nativeHeight) const {
  const Image *image = impl_->image_of(id);
  nativeWidth = image ? image->nativeWidth : 0;
  nativeHeight = image ? image->nativeHeight : 0;
}

bool Gpu::CopyImage(ImageId from, ImageId to, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                    std::string &error) {
  Impl &s = *impl_;
  const Image *source = s.image_of(from);
  const Image *destination = s.image_of(to);
  if (!source || !destination) {
    error = "CORE2 copies between images that do not exist";
    return false;
  }
  const std::uint32_t sc = source->scale;
  const std::int32_t right = std::min<std::int32_t>({x1 + 1, static_cast<std::int32_t>(source->nativeWidth),
                                                     static_cast<std::int32_t>(destination->nativeWidth)});
  const std::int32_t bottom = std::min<std::int32_t>({y1 + 1, static_cast<std::int32_t>(source->nativeHeight),
                                                      static_cast<std::int32_t>(destination->nativeHeight)});
  const std::int32_t left = std::max(x0, 0), top = std::max(y0, 0);
  if (right <= left || bottom <= top || source->scale != destination->scale) return true;
  if (!s.begin_recording(error)) return false;
  s.outside_pass();
  s.touch("copy image", {source}, {destination}, 0, 0);
  VkImageCopy region{};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.srcOffset = {left * static_cast<std::int32_t>(sc), top * static_cast<std::int32_t>(sc), 0};
  region.dstOffset = region.srcOffset;
  region.extent = {static_cast<std::uint32_t>(right - left) * sc, static_cast<std::uint32_t>(bottom - top) * sc, 1};
  gt4_hwd_vkCmdCopyImage(s.cmd, source->image, VK_IMAGE_LAYOUT_GENERAL, destination->image, VK_IMAGE_LAYOUT_GENERAL,
                         1, &region);
  s.stats.gpuCopyImageBytes += static_cast<std::uint64_t>(region.extent.width) * region.extent.height * 4u;  // P1-C
  s.after_op();
  return true;
}

bool Gpu::AllocateAtlas(std::uint32_t width, std::uint32_t height, std::uint32_t &x, std::uint32_t &y) {
  Impl &s = *impl_;
  if (width > kAtlasSide || height > kAtlasSide) return false;
  if (s.atlasShelfX + width > kAtlasSide) {
    s.atlasShelfY += s.atlasShelfHeight;
    s.atlasShelfX = 0;
    s.atlasShelfHeight = 0;
  }
  if (s.atlasShelfY + height > kAtlasSide) return false;
  x = s.atlasShelfX;
  y = s.atlasShelfY;
  s.atlasShelfX += width;
  s.atlasShelfHeight = std::max(s.atlasShelfHeight, height);
  return true;
}

void Gpu::ResetAtlas() {
  Impl &s = *impl_;
  s.atlasShelfX = s.atlasShelfY = s.atlasShelfHeight = 0;
  s.stats.atlasResets++;
}

bool Gpu::UploadAtlas(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height,
                      const std::uint32_t *texels, std::string &error) {
  Impl &s = *impl_;
  if (!s.begin_recording(error)) return false;
  const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4u;
  VkDeviceSize offset = 0;
  if (!s.ring(s.staging, bytes, 16, offset, error)) return false;
  std::memcpy(static_cast<std::uint8_t *>(s.staging.mapped) + offset, texels, static_cast<std::size_t>(bytes));
  s.stats.cpuImageBytes += bytes;  // P1-C
  s.outside_pass();
  s.touch("upload atlas", {}, {&s.atlas}, 0, 0);
  VkBufferImageCopy region{};
  region.bufferOffset = offset;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageOffset = {static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), 0};
  region.imageExtent = {width, height, 1};
  gt4_hwd_vkCmdCopyBufferToImage(s.cmd, s.staging.buffer, s.atlas.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  s.stats.gpuCopyBufferToImageBytes += bytes;  // P1-C
  s.after_op();
  s.stats.atlasUploads++;
  return true;
}

bool Gpu::UploadImage(ImageId id, std::uint32_t width, std::uint32_t height, const std::uint32_t *texels,
                      std::string &error) {
  Impl &s = *impl_;
  const Image *image = s.image_of(id);
  if (!image || width > image->width || height > image->height) {
    error = "CORE2 texture upload outside its image";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  // IN-470: in pieces no larger than half a recording's share of the staging ring (a 4K replacement is 64 MiB).
  const std::uint32_t rowsPerPiece = std::max<std::uint32_t>(
      1u, static_cast<std::uint32_t>(s.ring_share(s.staging) / 2u / std::max<VkDeviceSize>(VkDeviceSize{width} * 4u, 1u)));
  for (std::uint32_t top = 0; top < height; top += rowsPerPiece) {
    const std::uint32_t rows = std::min(rowsPerPiece, height - top);
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * rows * 4u;
    VkDeviceSize offset = 0;
    if (!s.ring(s.staging, bytes, 16, offset, error)) return false;
    std::memcpy(static_cast<std::uint8_t *>(s.staging.mapped) + offset, texels + static_cast<std::size_t>(top) * width,
                static_cast<std::size_t>(bytes));
    s.stats.cpuImageBytes += bytes;  // P1-C
    s.outside_pass();
    s.touch("upload image", {}, {image}, 0, 0);
    VkBufferImageCopy region{};
    region.bufferOffset = offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {0, static_cast<std::int32_t>(top), 0};
    region.imageExtent = {width, rows, 1};
    gt4_hwd_vkCmdCopyBufferToImage(s.cmd, s.staging.buffer, image->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    s.stats.gpuCopyBufferToImageBytes += bytes;  // P1-C
    s.after_op();
  }
  s.stats.atlasUploads++;
  return true;
}

bool Gpu::Draw(const DrawBatch &batch, std::string &error) {
  Impl &s = *impl_;
  const Image *frame = s.image_of(batch.frame);
  if (!frame || !batch.vertices || !batch.draws) {
    error = "CORE2 draw without a colour target";
    return false;
  }
  if (batch.draws->empty() || batch.vertices->empty()) return true;
  // IN-501: GT4's sheen on the road, a pass over the screen that adds a small target's picture weighted by destination
  // alpha (Cs Ad + Cd). Performance Mode leaves it out, the road gets darker.
  bool roadSheen = false;
  if (batch.texture != kNoImage) {
    namespace g = gpu::gpufield;
    const auto &f = batch.state.fields;
    if (f[g::BlendOn] != 0 && f[g::BlendA] == 0u && f[g::BlendB] == 2u && f[g::BlendC] == 1u && f[g::BlendD] == 1u &&
        (f[g::Ztest] == 0 || f[g::Ztst] == 1u) && frame->nativeWidth >= 640u) {
      std::int64_t area = 0;
      for (const gpu::GpuDraw &draw : *batch.draws) area += static_cast<std::int64_t>(std::max(draw.x1 - draw.x0 + 1, 0)) * std::max(draw.y1 - draw.y0 + 1, 0);
      roadSheen = area * 5 >= static_cast<std::int64_t>(frame->nativeWidth) * frame->nativeHeight * 2;
    }
  }
  if (roadSheen && s.skipRoadSheen) {
    s.stats.roadSheensSkipped++;
    return true;
  }
  const Image *depth = batch.depth == kNoImage ? frame : s.image_of(batch.depth);
  const Image *texture = batch.texture == kNoImage ? &s.atlas : s.image_of(batch.texture);
  const Image *texels = batch.memoryTexture == kNoImage ? &s.atlas : s.image_of(batch.memoryTexture);
  // IN-409: an indexed memory texture's palette row image goes where a target texture would (binding 4).
  const bool paletted = batch.paletteRow >= 0 && batch.palettes != kNoImage;
  if (paletted) texture = s.image_of(batch.palettes);
  if (!depth || !texture || !texels) {
    error = "CORE2 draw names a depth or texture image that does not exist";
    return false;
  }
  if (batch.texture != kNoImage && (batch.texture == batch.frame || batch.texture == batch.depth)) {
    s.stats.selfTextureBatches++;
  }
  if (!s.begin_recording(error)) return false;
  const VkDeviceSize vertexBytes = batch.vertices->size() * sizeof(gpu::GpuVertex);
  VkDeviceSize vertexOffset = 0, uniformOffset = 0;
  const VkDeviceSize uniformAlign = std::max<VkDeviceSize>(s.limits.minUniformBufferOffsetAlignment, 16);
  if (!s.ring(s.vertices, vertexBytes, sizeof(gpu::GpuVertex), vertexOffset, error)) return false;
  if (!s.ring(s.uniforms, kPixelStateBytes, uniformAlign, uniformOffset, error)) return false;
  if (!s.recording && !s.begin_recording(error)) return false;  // a ring may have submitted
  std::memcpy(static_cast<std::uint8_t *>(s.vertices.mapped) + vertexOffset, batch.vertices->data(),
              static_cast<std::size_t>(vertexBytes));
  s.stats.cpuVertexBytes += vertexBytes;  // P1-C
  gpu::GpuPixelState state = batch.state;
  state.fields[gpu::gpufield::TexOriginX] = batch.mipInfo;  // IN-428: the mip parameters (0 without mips)
  state.fields[gpu::gpufield::TexOriginY] = paletted ? static_cast<std::uint32_t>(batch.paletteRow) : 0u;
  if (paletted) state.fields[gpu::gpufield::TexelKind] = 3u;  // HW_TEXEL_INDEXED
  if (batch.replacedWidth != 0) {  // IN-470: these fields mean nothing else for RGBA8 words (hw_pixel_uber.frag)
    state.fields[gpu::gpufield::TexelKind] = 0u;
    state.fields[gpu::gpufield::TexOriginX] = batch.replacedWidth | (batch.replacedHeight << 16);
    state.fields[gpu::gpufield::TexOriginY] = batch.logicalWidth | (batch.logicalHeight << 16);
    state.fields[gpu::gpufield::Ta0] = batch.logicalX | (batch.logicalY << 16);
  }
  state.fields[gpu::gpufield::KeepTopByte] = 1u;
  std::memcpy(static_cast<std::uint8_t *>(s.uniforms.mapped) + uniformOffset, state.fields, sizeof state.fields);
  s.stats.cpuUniformBytes += sizeof state.fields;  // P1-C: once per draw (a rewrite below is not counted again)

  // Spec 8.8: the texture is the target this batch draws into and the batch samples where it draws. Its sampled
  // rectangle is copied first (after the draws that wrote it) and the batch samples the copy.
  // TASK-182 R1: with GT4_CORE2_SELF_COPY_DRAW the copy is a draw inside the open render pass (record_self_copy), taken
  // after the draws of the pass and before this batch; the transfer below stays the fallback. The rectangle is the same
  // either way, so the batch samples the same words.
  std::int32_t selfCopyLeft = 0, selfCopyTop = 0, selfCopyRight = 0, selfCopyBottom = 0;  // output pixels, half-open
  bool selfCopyInPass = false;
  if (batch.selfCopy && !paletted) {
    Image *copy = s.self_copy_image(*texture, error);
    if (!copy) return false;
    const std::int32_t sc = static_cast<std::int32_t>(texture->scale);
    const std::int32_t left = std::max(batch.copyX0, 0), top = std::max(batch.copyY0, 0);
    const std::int32_t right = std::min(batch.copyX1 + 1, static_cast<std::int32_t>(texture->nativeWidth));
    const std::int32_t bottom = std::min(batch.copyY1 + 1, static_cast<std::int32_t>(texture->nativeHeight));
    if (right > left && bottom > top) {
      selfCopyLeft = left * sc;
      selfCopyTop = top * sc;
      selfCopyRight = right * sc;
      selfCopyBottom = bottom * sc;
      // The copy as a draw applies where the pass has no attachments (IN-481 puts the targets there on an integrated GPU,
      // and a sampled image may be one), where LAZY_BARRIERS orders the commands that are not draws (a full barrier per
      // command cannot be recorded inside a pass), and where the rectangle fits the pass's framebuffer scissor. Every
      // core 2 image is one r32ui storage image (make_image), so the fragment stage can write the pooled copy.
      selfCopyInPass = s.selfCopyDraw && s.lazyBarriers && !s.ropOn &&
                       selfCopyRight <= static_cast<std::int32_t>(s.framebufferWidth) &&
                       selfCopyBottom <= static_cast<std::int32_t>(s.framebufferHeight);
      // TASK-182 step 2a diagnostics: does the copied rectangle meet what the batch draws? The renderer takes the copy only
      // when the sampled footprint meets the drawn rectangle, so this is always yes here; the counter is the check that it
      // stays so, and the disjoint case is counted where the renderer decides (`no copy: sampled rectangle disjoint`).
      bool meetsDrawn = false;
      for (const gpu::GpuDraw &draw : *batch.draws) {
        if (draw.count != 0 && draw.x1 >= left && draw.x0 <= right - 1 && draw.y1 >= top && draw.y0 <= bottom - 1) {
          meetsDrawn = true;
          break;
        }
      }
      if (s.trace.on) {
        std::string row = selfCopyInPass ? "drawn in the pass, " : "vkCmdCopyImage, ";
        row += meetsDrawn ? "overlapping the batch" : "disjoint from the batch";
        ++Impl::SyncTrace::row(s.trace.selfCopies, row).count;
      }
      if (!selfCopyInPass) {
        s.outside_pass();
        s.touch("self copy", {texture}, {copy}, 0, 0);
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.srcOffset = {left * sc, top * sc, 0};
        region.dstOffset = region.srcOffset;
        region.extent = {static_cast<std::uint32_t>((right - left) * sc), static_cast<std::uint32_t>((bottom - top) * sc), 1};
        gt4_hwd_vkCmdCopyImage(s.cmd, texture->image, VK_IMAGE_LAYOUT_GENERAL, copy->image, VK_IMAGE_LAYOUT_GENERAL, 1,
                               &region);
        s.stats.gpuCopyImageBytes += static_cast<std::uint64_t>(region.extent.width) * region.extent.height * 4u;  // P1-C
        s.after_op();
        s.stats.selfCopies++;
        s.stats.selfCopyPixels += static_cast<std::uint64_t>(region.extent.width) * region.extent.height;
      } else {
        s.flush_draws("self copy", "reads what a draw writes", /*keepPassOpen=*/true);
        if (!s.record_self_copy(*texture, *copy, selfCopyLeft, selfCopyTop, selfCopyRight, selfCopyBottom, error)) {
          return false;
        }
      }
    }
    texture = copy;
  }

  // IN-442: a constant fill as compute dispatches. Every vertex of the batch has the same colour and Z, every
  // primitive is an axis-aligned rectangle (a sprite as two triangles), and the rectangles of covered pixels, clipped
  // exactly as the draw's scissor clips them, are disjoint or repeated (a repeated rectangle of one constant fragment
  // writes what it already wrote: no blend, no DATE). Then each output pixel gets the fragment the uber shader would
  // give it (core2_fill.comp), with storage writes from compute, about twice as fast at high scales. The covered pixels
  // are those the rasteriser covers: hw_vertex.vert puts an edge at scale * X + scale_offset / 2 - 1/320 (no IN-388
  // snap in this core), and a pixel is covered when its centre lies on or right of the left edge and left of the right
  // one. The 1/320 bias keeps every edge at least 0.003 pixel from a pixel centre, more than the float rounding of the
  // edge, so this double-precision count matches the GPU's.
  // IN-481: the state and images this batch would draw through colour attachments with, if any.
  // IN-497: the frame's and the texture's own scale (a small effect target may be 1x in a scaled session).
  const std::uint32_t pushTexScale = (batch.texture != kNoImage ? (texture->scale | 0x01000000u) : 1u) |
                                     (frame->scale > 1 ? (0x40000000u | 0x10000000u) : 0u) |
                                     (batch.mipInfo != 0 ? 0x00800000u : 0u) |  // IN-428: tex_origin_x holds the mip parameters
                                     (batch.replacedWidth != 0 ? 0x00400000u : 0u) |  // IN-470: a replacement image
                                     (batch.forceLinear ? 0x80000000u : 0u) |         // IN-433: forced bilinear
                                     ((batch.scanmsk & 3u) << 20);                    // IN-530: SCANMSK
  VkPipeline ropPipeline = VK_NULL_HANDLE;
  std::uint32_t ropWord = 0;
  if (s.ropOn && (batch.scanmsk & 2u) == 0 && frame->width <= s.framebufferWidth && frame->height <= s.framebufferHeight) {
    bool triangles = true;
    for (const gpu::GpuDraw &draw : *batch.draws) triangles = triangles && draw.topology == gpu::GpuTopology::Triangles;
    ropWord = triangles ? Impl::rop_word(state.fields, s.ropBlend && s.dualSourceBlend) : 0u;
    // IN-495: sampling the frame is safe through its attachment only when no lane of it is written.
    if (texture == frame && (ropWord >> 1 & 15u) != 0) ropWord = 0;
    // A depth write needs its own attachment; a batch with no depth image names the frame there, and writes no depth.
    if (ropWord != 0 && Impl::rop_depth_mask(ropWord) != 0 &&
        (depth == frame || texture == depth || depth->width != frame->width || depth->height != frame->height)) {
      ropWord = 0;
    }
    ropPipeline = s.rop_pipeline(state.fields, pushTexScale, ropWord);
  }

  if (batch.constantFill && s.computeFills && s.fillPipeline && (!(ropPipeline && s.ropFills) || s.clearWhole)) {
    const std::vector<gpu::GpuVertex> &verts = *batch.vertices;
    const gpu::GpuVertex &v0 = verts[batch.draws->front().first];
    const std::uint32_t sc = frame->scale;
    const float shift = frame->scale == s.scale ? s.shift : 1.0f;
    const std::int32_t limitX = static_cast<std::int32_t>(std::min(frame->width, s.framebufferWidth));
    const std::int32_t limitY = static_cast<std::int32_t>(std::min(frame->height, s.framebufferHeight));
    using Rect = Impl::FillRect;
    std::vector<Rect> &rects = s.fillRects;  // IN-447: reused across batches
    rects.clear();
    bool eligible = true;
    for (const gpu::GpuDraw &draw : *batch.draws) {
      if (draw.topology != gpu::GpuTopology::Triangles || draw.count == 0 || draw.first + draw.count > verts.size()) {
        eligible = false;
        break;
      }
      std::int32_t nx0 = INT32_MAX, ny0 = INT32_MAX, nx1 = INT32_MIN, ny1 = INT32_MIN;
      for (std::uint32_t i = draw.first; i < draw.first + draw.count && eligible; ++i) {
        const gpu::GpuVertex &v = verts[i];
        const std::int32_t xi = static_cast<std::int16_t>(v.x), yi = static_cast<std::int16_t>(v.y);
        eligible = std::memcmp(v.rgba, v0.rgba, 4) == 0 && v.z == v0.z;
        nx0 = std::min(nx0, xi);  // 12.4 units (the vertices already carry XYOFFSET)
        ny0 = std::min(ny0, yi);
        nx1 = std::max(nx1, xi);
        ny1 = std::max(ny1, yi);
      }
      for (std::uint32_t i = draw.first; i < draw.first + draw.count && eligible; ++i) {  // corners only
        const std::int32_t x = static_cast<std::int16_t>(verts[i].x), y = static_cast<std::int16_t>(verts[i].y);
        eligible = (x == nx0 || x == nx1) && (y == ny0 || y == ny1);
      }
      if (!eligible) break;
      // Covered output pixels [px0, px1) x [py0, py1): centres k + 0.5 in [edge0, edge1).
      auto edge = [&](std::int32_t coordinate) {
        return static_cast<double>(sc) * (coordinate / 16.0) + 0.5 * static_cast<double>(shift) - 1.0 / 320.0;
      };
      const std::int32_t px0 = static_cast<std::int32_t>(std::ceil(edge(nx0) - 0.5));
      const std::int32_t px1 = static_cast<std::int32_t>(std::ceil(edge(nx1) - 0.5));
      const std::int32_t py0 = static_cast<std::int32_t>(std::ceil(edge(ny0) - 0.5));
      const std::int32_t py1 = static_cast<std::int32_t>(std::ceil(edge(ny1) - 0.5));
      if (px1 <= px0 || py1 <= py0) continue;  // covers no pixel
      // The draw's scissor (the recording below): the scaled pass's own native rectangle when it has one.
      const bool own = draw.scaledX1 >= draw.scaledX0 && draw.scaledY1 >= draw.scaledY0;
      const std::int32_t cx0 = own ? draw.scaledX0 : draw.x0, cy0 = own ? draw.scaledY0 : draw.y0;
      const std::int32_t cx1 = own ? draw.scaledX1 : draw.x1, cy1 = own ? draw.scaledY1 : draw.y1;
      const std::int32_t s32 = static_cast<std::int32_t>(sc);
      Rect r{std::max({px0, std::clamp(cx0 * s32, 0, limitX), 0}), std::max({py0, std::clamp(cy0 * s32, 0, limitY), 0}),
             std::min(px1, std::clamp((cx1 + 1) * s32, 0, limitX)), std::min(py1, std::clamp((cy1 + 1) * s32, 0, limitY))};
      if (r.x1 <= r.x0 || r.y1 <= r.y0) continue;
      bool repeated = false;
      for (const Rect &o : rects) {
        if (o.x0 == r.x0 && o.y0 == r.y0 && o.x1 == r.x1 && o.y1 == r.y1) {
          repeated = true;
        } else if (r.x0 < o.x1 && o.x0 < r.x1 && r.y0 < o.y1 && o.y0 < r.y1) {
          eligible = false;  // partly overlapping rectangles: drawn in order by the rasteriser
        }
      }
      if (!eligible) break;
      if (!repeated) rects.push_back(r);
    }
    // IN-494: a fill of the whole target with one constant word per image is a clear of the image. GT4 clears its depth
    // and colour this way several times per VBlank, in strips, some masked to the alpha lane (the rest of such a fill
    // goes on below without its depth) or only through the alpha test's fail path into the depth.
    if (eligible && s.clearWhole) {
      namespace g = gpu::gpufield;
      const auto &f = state.fields;
      std::uint64_t covered = 0;
      for (const Rect &r : rects) covered += static_cast<std::uint64_t>(r.x1 - r.x0) * static_cast<std::uint64_t>(r.y1 - r.y0);
      const bool whole = covered == static_cast<std::uint64_t>(frame->width) * frame->height;
      const bool onlyDepth = f[g::Ate] != 0 && f[g::Atst] == 0u && f[g::Afail] == 2u;  // NEVER, ZB_ONLY
      const bool testPasses = f[g::Ate] == 0 || f[g::Atst] == 1u || onlyDepth;
      const bool writesFrame = f[g::Fbmsk] != 0xFFFFFFFFu && !onlyDepth;
      const bool writesDepth = f[g::Ztest] != 0 && f[g::Ztst] == 1u && f[g::Zmsk] == 0 && depth != frame &&
                               depth->width == frame->width && depth->height == frame->height;
      const bool depthClearable = writesDepth && (f[g::Zformat] == 0u || (f[g::Zformat] == 1u && s.clearTopZero));
      const bool plain = f[g::Dither] == 0 && f[g::Fba] == 0 && testPasses && f[g::Format] == 0u &&
                         (f[g::Ztest] == 0 || f[g::Ztst] == 1u) && (!writesDepth || depthClearable);
      const bool wholeWords = f[g::Fbmsk] == 0 && writesFrame;
      // IN-502: the sheen was the main reader of GT4's fills of the alpha lane, so Performance Mode leaves them out with it
      // (their depth stays). GT4_CORE2_ALPHA_FILLS=1 keeps them (A/B).
      static const bool keepAlphaFills = std::getenv("GT4_CORE2_ALPHA_FILLS") != nullptr && std::strcmp(std::getenv("GT4_CORE2_ALPHA_FILLS"), "0") != 0;
      const bool alphaGone = !keepAlphaFills && s.skipRoadSheen && f[g::Fbmsk] == 0x00FFFFFFu && frame->nativeWidth >= 640u;
      if (whole && plain && alphaGone && !depthClearable) {
        s.stats.batches++;
        return true;
      }
      if (whole && plain && (wholeWords || depthClearable)) {
        if (!s.recording && !s.begin_recording(error)) return false;
        s.outside_pass();
        s.touch("clear", {frame, depth}, {frame, depth}, 0, 0);
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (wholeWords) {
          VkClearColorValue value{};
          value.uint32[0] = static_cast<std::uint32_t>(v0.rgba[0]) | static_cast<std::uint32_t>(v0.rgba[1]) << 8 |
                            static_cast<std::uint32_t>(v0.rgba[2]) << 16 | static_cast<std::uint32_t>(v0.rgba[3]) << 24;
          gt4_hwd_vkCmdClearColorImage(s.cmd, frame->image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
          s.stats.gpuClearBytes += static_cast<std::uint64_t>(frame->width) * frame->height * 4u;  // P1-C
        }
        if (depthClearable) {
          VkClearColorValue value{};
          value.uint32[0] = std::min(v0.z, batch.zMax);
          gt4_hwd_vkCmdClearColorImage(s.cmd, depth->image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
          s.stats.gpuClearBytes += static_cast<std::uint64_t>(depth->width) * depth->height * 4u;  // P1-C
        }
        s.after_op();
        s.stats.wholeClears++;
        if (wholeWords || !writesFrame || alphaGone) {
          s.stats.batches++;
          return true;
        }
        // Only some lanes of the frame are left, with no depth: the path below, as a batch that never had any.
        state.fields[g::Ztest] = 0;
        std::memcpy(static_cast<std::uint8_t *>(s.uniforms.mapped) + uniformOffset, state.fields, sizeof state.fields);
        ropWord = ropPipeline ? Impl::rop_word(state.fields, s.ropBlend && s.dualSourceBlend) : 0u;
        ropPipeline = ropWord != 0 ? s.rop_pipeline(state.fields, pushTexScale, ropWord) : VK_NULL_HANDLE;
        if (!s.recording && !s.begin_recording(error)) return false;
      }
    }
    if (eligible && !(ropPipeline && s.ropFills)) {
      VkDeviceSize stateOffset = 0;
      const VkDeviceSize align = std::max<VkDeviceSize>(s.limits.minStorageBufferOffsetAlignment, 16);
      if (!s.ring(s.staging, sizeof state.fields, align, stateOffset, error)) return false;
      if (!s.recording && !s.begin_recording(error)) return false;
      std::memcpy(static_cast<std::uint8_t *>(s.staging.mapped) + stateOffset, state.fields, sizeof state.fields);
      s.stats.cpuFillStateBytes += sizeof state.fields;  // P1-C
      s.outside_pass();
      s.touch("fill", {frame, depth}, {frame, depth}, 0, 0);
      VkDescriptorSet fillSet = VK_NULL_HANDLE;
      if (!s.compute_set(s.staging.buffer, stateOffset, sizeof state.fields, frame->view, depth->view, frame->view,
                         fillSet, error)) {
        return false;
      }
      const std::uint32_t colour = static_cast<std::uint32_t>(v0.rgba[0]) | static_cast<std::uint32_t>(v0.rgba[1]) << 8 |
                                   static_cast<std::uint32_t>(v0.rgba[2]) << 16 | static_cast<std::uint32_t>(v0.rgba[3]) << 24;
      const std::uint32_t flags = sc > 1 ? 3u : 0u;  // the push word bits 28 and 30 of a scaled draw
      if (s.profiling) {  // diagnostics (S3 profile VBlank): the shape of each fill, for the clear-path survey
        namespace g = gpu::gpufield;
        const auto &f = state.fields;
        for (const Rect &r : rects) {
          std::fprintf(stderr,
                       "[core2] gpu profile fill rect=%d,%d-%d,%d frame=%ux%u depth=%ux%u same=%d format=%u fbmsk=%08x "
                       "dither=%u keep_top=%u ztest=%u ztst=%u zmsk=%u zformat=%u fba=%u colour=%08x z=%08x\n",
                       r.x0, r.y0, r.x1, r.y1, frame->width, frame->height, depth->width, depth->height,
                       frame == depth ? 1 : 0, f[g::Format], f[g::Fbmsk], f[g::Dither], f[g::KeepTopByte], f[g::Ztest],
                       f[g::Ztst], f[g::Zmsk], f[g::Zformat], f[g::Fba], colour, v0.z);
        }
      }
      for (const Rect &r : rects) {
        const std::uint32_t push[9] = {static_cast<std::uint32_t>(r.x0), static_cast<std::uint32_t>(r.y0),
                                       static_cast<std::uint32_t>(r.x1), static_cast<std::uint32_t>(r.y1), colour, v0.z,
                                       batch.zMax, flags, sc};
        // Disjoint rectangles: no barrier between them (dispatch orders nothing, touch ordered the batch).
        gt4_hwd_vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.fillPipeline);
        gt4_hwd_vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.computeLayout, 0, 1, &fillSet, 0,
                                        nullptr);
        gt4_hwd_vkCmdPushConstants(s.cmd, s.computeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
        gt4_hwd_vkCmdDispatch(s.cmd, (static_cast<std::uint32_t>(r.x1 - r.x0) + 7u) / 8u,
                              (static_cast<std::uint32_t>(r.y1 - r.y0) + 7u) / 8u, 1);
        s.stats.fillPixels += static_cast<std::uint64_t>(r.x1 - r.x0) * static_cast<std::uint64_t>(r.y1 - r.y0);
      }
      s.after_op();
      s.stats.computeFills++;
      s.stats.batches++;
      return true;
    }
  }

  VkDescriptorSet set = VK_NULL_HANDLE;
  const DrawSetKey setKey{frame->serial, depth->serial, texels->serial, texture->serial};
  if (auto cached = s.drawSets.find(setKey); cached != s.drawSets.end()) {
    set = cached->second;
  } else {
    if (!s.allocate_set(s.drawSetLayout, set, error)) return false;
    s.drawSets[setKey] = set;
    VkDescriptorBufferInfo uniformInfo{s.uniforms.buffer, 0, kPixelStateBytes};
    VkDescriptorImageInfo imageInfo[5] = {{VK_NULL_HANDLE, frame->view, VK_IMAGE_LAYOUT_GENERAL},
                                          {VK_NULL_HANDLE, depth->view, VK_IMAGE_LAYOUT_GENERAL},
                                          {VK_NULL_HANDLE, texels->view, VK_IMAGE_LAYOUT_GENERAL},
                                          {VK_NULL_HANDLE, texture->view, VK_IMAGE_LAYOUT_GENERAL},
                                          {VK_NULL_HANDLE, depth->view, VK_IMAGE_LAYOUT_GENERAL}};  // IN-451
    VkWriteDescriptorSet writes[6]{};
    for (std::uint32_t i = 0; i < 6; ++i) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = set;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      if (i == 0) {
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[i].pBufferInfo = &uniformInfo;
      } else {
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &imageInfo[i - 1];
      }
    }
    s.update_sets(6, writes);
  }

  const std::uint32_t sc = frame->scale;
  Impl::DeferredDraw record;
  record.set = set;
  record.uniformOffset = static_cast<std::uint32_t>(uniformOffset);
  record.vertexOffset = vertexOffset;
  record.viewport = VkViewport{0.0f, 0.0f, static_cast<float>(frame->width), static_cast<float>(frame->height), 0.0f, 1.0f};
  record.push[0] = FloatBits(static_cast<float>(frame->width));
  record.push[1] = FloatBits(static_cast<float>(frame->height));
  record.push[2] = FloatBits(static_cast<float>(sc));
  record.push[3] = FloatBits(frame->scale == s.scale ? s.shift : 1.0f);
  record.push[4] = 0;  // the batch vertices already carry XYOFFSET
  record.push[5] = 0;
  record.push[6] = batch.zMax;
  // tex_scale: s for a texture sampled from a target (binding 4) with bit 24 set (IN-427: at 1x the scale alone could
  // not tell it from the atlas), 1 for the atlas; at scale the fine dither (8.6) and the floored rasteriser depth (1.6)
  // are on.
  record.push[7] = pushTexScale;
  record.textureFromTarget = batch.texture != kNoImage;
  if (batch.texture != kNoImage) {  // TASK-182 diagnostics
    const bool copied = batch.selfCopy && !paletted;
    record.sampleKind = batch.texture == batch.frame ? (copied ? "texture is the frame (copied first)" : "texture is the frame (sampled in place)")
                        : batch.texture == batch.depth ? (copied ? "texture is the depth (copied first)" : "texture is the depth (sampled in place)")
                                                      : "texture is another target";
  }
  {  // IN-492
    namespace g = gpu::gpufield;
    const auto &f = state.fields;
    const bool writesDepth = f[g::Ztest] != 0 && f[g::Zmsk] == 0;
    const bool rising = f[g::Ztst] == 2u || f[g::Ztst] == 3u;  // HW_DEPTH_GEQUAL, HW_DEPTH_GREATER
    record.peeksDepth = f[g::Ztest] != 0 && rising && depth != frame ? depth->serial : 0;
    record.lowersDepth = writesDepth && !rising ? depth->serial : 0;
    record.frameSerial = frame->serial;
  }
  if (s.profiling) {
    namespace g = gpu::gpufield;
    const auto &f = batch.state.fields;
    char label[240];
    // The area the batch's scissors cover, in output pixels, in the label's rounding: a large batch stands on its own line.
    double scissorArea = 0;
    for (const gpu::GpuDraw &draw : *batch.draws) {
      scissorArea += static_cast<double>(std::max(draw.x1 - draw.x0 + 1, 0)) * std::max(draw.y1 - draw.y0 + 1, 0) * sc * sc;
    }
    // GT4_CORE2_GPU_PROFILE_ORDER: each batch's masks and tests too (a line of its own per different batch).
    static const bool detailed = std::getenv("GT4_CORE2_GPU_PROFILE_ORDER") != nullptr;
    char detail[160] = "";
    if (detailed) {
      const gpu::GpuVertex &v0 = (*batch.vertices)[batch.draws->front().first];
      const gpu::GpuVertex &v1 = (*batch.vertices)[batch.draws->front().first + 1u];
      std::snprintf(detail, sizeof detail, " fbmsk=%08x fmt=%u zf=%u ztst=%u atst=%u afail=%u aref=%u fix=%u same=%u prims=%zu verts0=%u lin=%u texsc=%u xyuv=%u,%u,%u,%u,%u,%u,%u,%u",
                    f[g::Fbmsk], f[g::Format], f[g::Zformat], f[g::Ztst], f[g::Atst], f[g::Afail], f[g::Aref], f[g::Fix],
                    depth == frame ? 1u : 0u, batch.draws->size(), batch.draws->front().count, f[g::TexLinear], pushTexScale & 0xFFu,
                    v0.x, v0.y, v0.u, v0.v, v1.x, v1.y, v1.u, v1.v);
    }
    std::snprintf(label, sizeof label, "draw %ux%u area=%.1fM rop=%u const=%u topo=%u tex=%s blend=%s z=%s%s ate=%u fbmsk=%s prims=%s%s",
                  frame->width, frame->height, scissorArea / 1.0e6, ropPipeline ? 1u : 0u, batch.constantFill ? 1u : 0u,
                  static_cast<unsigned>(batch.draws->front().topology),
                  batch.texture != kNoImage ? "target" : batch.memoryTexture != kNoImage ? (paletted ? "indexed" : "memory") : "-",
                  f[g::BlendOn] ? (std::to_string(f[g::BlendA]) + std::to_string(f[g::BlendB]) + std::to_string(f[g::BlendC]) +
                                   std::to_string(f[g::BlendD])).c_str()
                                : "off",
                  f[g::Ztest] && f[g::Ztst] != 1u ? "test" : "-", f[g::Zmsk] == 0 ? "+write" : "", f[g::Ate],
                  f[g::Fbmsk] == 0 ? "0" : "set",
                  batch.draws->size() <= 4 ? "1-4" : batch.draws->size() <= 64 ? "5-64" : batch.draws->size() <= 1024 ? "65-1k" : "1k+", detail);
    record.profileLabel = label;
  }
  const std::int32_t limitX = static_cast<std::int32_t>(std::min(frame->width, s.framebufferWidth));
  const std::int32_t limitY = static_cast<std::int32_t>(std::min(frame->height, s.framebufferHeight));
  // 8.3: the GS scissor and the primitive's box (one native pixel of slack) times s.
  auto scissorOf = [&](std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1, VkRect2D &out) {
    const std::int32_t left = std::clamp(x0 * static_cast<std::int32_t>(sc), 0, limitX);
    const std::int32_t top = std::clamp(y0 * static_cast<std::int32_t>(sc), 0, limitY);
    const std::int32_t right = std::clamp((x1 + 1) * static_cast<std::int32_t>(sc), 0, limitX);
    const std::int32_t bottom = std::clamp((y1 + 1) * static_cast<std::int32_t>(sc), 0, limitY);
    if (right <= left || bottom <= top) return false;
    out = {{left, top}, {static_cast<std::uint32_t>(right - left), static_cast<std::uint32_t>(bottom - top)}};
    return true;
  };
  auto boundsOf = [](const gpu::GpuDraw &draw, std::int32_t &x0, std::int32_t &y0, std::int32_t &x1, std::int32_t &y1) {
    const bool own = draw.scaledX1 >= draw.scaledX0 && draw.scaledY1 >= draw.scaledY0;
    x0 = own ? draw.scaledX0 : draw.x0;
    y0 = own ? draw.scaledY0 : draw.y0;
    x1 = own ? draw.scaledX1 : draw.x1;
    y1 = own ? draw.scaledY1 : draw.y1;
  };
  record.firstCommand = static_cast<std::uint32_t>(s.deferredCommands.size());
  const std::uint32_t vertexCount = static_cast<std::uint32_t>(batch.vertices->size());
  if (ropPipeline) {
    const bool withDepth = Impl::rop_depth_mask(ropWord) != 0;
    if (!s.rop_framebuffer(*frame, withDepth ? depth : nullptr, record.ropFramebuffer, error)) return false;
    record.rop = true;
    record.ropPass = withDepth ? s.ropPassBoth : s.ropPassFrame;
    record.ropWidth = frame->width;
    record.ropHeight = frame->height;
    s.stats.ropBatches++;
  }
  bool allTriangles = s.interlock || record.rop;
  for (const gpu::GpuDraw &draw : *batch.draws) allTriangles = allTriangles && draw.topology == gpu::GpuTopology::Triangles;
  if (allTriangles) {
    std::int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
    std::uint32_t first = UINT32_MAX, end = 0;
    for (const gpu::GpuDraw &draw : *batch.draws) {
      if (draw.count == 0 || draw.first + draw.count > vertexCount) continue;
      std::int32_t a, b, c, d;
      boundsOf(draw, a, b, c, d);
      x0 = std::min(x0, a);
      y0 = std::min(y0, b);
      x1 = std::max(x1, c);
      y1 = std::max(y1, d);
      first = std::min(first, draw.first);
      end = std::max(end, draw.first + draw.count);
    }
    Impl::DeferredCommand command;
    if (end > first && scissorOf(x0, y0, x1, y1, command.scissor)) {
      command.pipeline = record.rop ? ropPipeline : VK_NULL_HANDLE;
      // IN-496: a batch over at least 40% of the target, on an interlock GPU, whose triangles share no pixel. Each costs two
      // waits, so a smaller one gains nothing.
      if (!record.rop && s.interlock && s.plainDisjoint && batch.draws->size() <= 64u &&
          static_cast<std::int64_t>(x1 - x0 + 1) * (y1 - y0 + 1) * sc * sc * 5 >= static_cast<std::int64_t>(frame->width) * frame->height * 2) {
        std::vector<PrimitiveShape> &shapes = s.disjointGroup;
        shapes.clear();
        for (const gpu::GpuDraw &draw : *batch.draws) {
          if (draw.count == 0 || draw.first + draw.count > vertexCount) continue;
          for (std::uint32_t v = draw.first; v + 3u <= draw.first + draw.count; v += 3u) {
            PrimitiveShape shape;
            shape.triangle = true;
            shape.minX = shape.minY = INT32_MAX;
            shape.maxX = shape.maxY = INT32_MIN;
            for (std::uint32_t corner = 0; corner < 3u; ++corner) {
              shape.x[corner] = static_cast<std::int16_t>((*batch.vertices)[v + corner].x);
              shape.y[corner] = static_cast<std::int16_t>((*batch.vertices)[v + corner].y);
              shape.minX = std::min(shape.minX, shape.x[corner]);
              shape.minY = std::min(shape.minY, shape.y[corner]);
              shape.maxX = std::max(shape.maxX, shape.x[corner]);
              shape.maxY = std::max(shape.maxY, shape.y[corner]);
            }
            shapes.push_back(shape);
          }
        }
        bool disjoint = shapes.size() <= 128u;
        for (std::size_t a = 0; a < shapes.size() && disjoint; ++a) {
          for (std::size_t b = a + 1; b < shapes.size() && disjoint; ++b) disjoint = !ShapesMayShareAPixel(shapes[a], shapes[b]);
        }
        shapes.clear();
        if (disjoint) {
          namespace g = gpu::gpufield;
          const bool sheen = roadSheen && s.sheenEarly && state.fields[g::Format] == 0u && state.fields[g::Fbmsk] == 0;
          command.pipeline = s.plain_pipeline(state.fields, record.push[7], sheen);
          if (command.pipeline) {
            record.plain = true;
            s.stats.plainBatches++;
            if (sheen) s.stats.sheensEarly++;
          }
        }
      }
      if (!command.pipeline) command.pipeline = s.triangle_pipeline(state.fields, record.push[7]);  // IN-437
      command.first = first;
      command.count = end - first;
      s.deferredCommands.push_back(command);
    }
  } else {
    // IN-466, IN-471: a primitive waits for the earlier ones only when it may cover a pixel one of them covered since
    // the last wait: the triangles are tested, not their boxes (the triangles of a strip share edges, not pixels).
    // Primitives that share no pixel need no order for their read-modify-writes, so they also become one draw. Without
    // the interlock this runs across the batches of a draw pass into the same images; with it (line and point batches
    // only) and after a texture taken from a target, each batch starts over with a wait. GT4_CORE2_GROUP_DISJOINT=0 waits
    // before every primitive (A/B).
    std::vector<PrimitiveShape> &group = s.disjointGroup;
    if (s.interlock || record.textureFromTarget || s.deferred.empty() || s.deferred.back().rop || s.disjointFrame != frame->serial ||
        s.disjointDepth != depth->serial) {
      group.clear();
    }
    s.disjointFrame = frame->serial;
    s.disjointDepth = depth->serial;
    const std::vector<gpu::GpuVertex> &vertexList = *batch.vertices;
    auto shapeOf = [&](std::uint32_t first, bool triangle) {
      PrimitiveShape shape;
      shape.triangle = triangle;
      shape.minX = shape.minY = INT32_MAX;
      shape.maxX = shape.maxY = INT32_MIN;
      for (std::uint32_t v = 0; v < (triangle ? 3u : 1u); ++v) {
        shape.x[v] = static_cast<std::int16_t>(vertexList[first + v].x);
        shape.y[v] = static_cast<std::int16_t>(vertexList[first + v].y);
        shape.minX = std::min(shape.minX, shape.x[v]);
        shape.minY = std::min(shape.minY, shape.y[v]);
        shape.maxX = std::max(shape.maxX, shape.x[v]);
        shape.maxY = std::max(shape.maxY, shape.y[v]);
      }
      return shape;
    };
    std::array<PrimitiveShape, 2> shapes;
    const VkPipeline trianglePipeline = s.interlock ? s.pipelines[2] : s.triangle_pipeline(state.fields, record.push[7]);
    for (const gpu::GpuDraw &draw : *batch.draws) {
      if (draw.count == 0 || draw.first + draw.count > vertexCount) continue;
      std::int32_t a, b, c, d;
      boundsOf(draw, a, b, c, d);
      Impl::DeferredCommand command;
      if (!scissorOf(a, b, c, d, command.scissor)) continue;
      const std::size_t pipeline = draw.topology == gpu::GpuTopology::Points  ? 0
                                   : draw.topology == gpu::GpuTopology::Lines ? 1
                                                                              : 2;
      command.pipeline = pipeline == 2 ? trianglePipeline : s.pipelines[pipeline];
      command.first = draw.first;
      command.count = draw.count;
      command.barrierFirst = true;
      std::size_t shapeCount = 0;
      if (s.groupDisjoint) {
        if (draw.topology == gpu::GpuTopology::Triangles) {
          for (std::uint32_t v = 0; v + 3u <= draw.count && shapeCount < shapes.size(); v += 3u) {
            shapes[shapeCount++] = shapeOf(draw.first + v, true);
          }
        } else {  // a box with a native pixel of slack
          PrimitiveShape box = shapeOf(draw.first, false);
          const PrimitiveShape end = shapeOf(draw.first + draw.count - 1u, false);
          box.minX = std::min(box.minX, end.minX) - 16;
          box.minY = std::min(box.minY, end.minY) - 16;
          box.maxX = std::max(box.maxX, end.maxX) + 16;
          box.maxY = std::max(box.maxY, end.maxY) + 16;
          shapes[shapeCount++] = box;
        }
        if (!group.empty() && group.size() + shapeCount <= Impl::kDisjointGroupMax) {
          bool overlaps = false;
          for (const PrimitiveShape &earlier : group) {
            for (std::size_t n = 0; n < shapeCount && !overlaps; ++n) overlaps = ShapesMayShareAPixel(earlier, shapes[n]);
            if (overlaps) break;
          }
          command.barrierFirst = overlaps;
        }
        if (command.barrierFirst) group.clear();
        group.insert(group.end(), shapes.begin(), shapes.begin() + static_cast<std::ptrdiff_t>(shapeCount));
      }
      // No wait and the next vertices of the same pipeline: the same draw, under both scissors.
      const std::size_t commandsOfBatch = s.deferredCommands.size() - record.firstCommand;
      if (!command.barrierFirst && commandsOfBatch != 0) {
        Impl::DeferredCommand &previous = s.deferredCommands.back();
        if (previous.pipeline == command.pipeline && previous.first + previous.count == command.first) {
          const std::int32_t right = std::max(previous.scissor.offset.x + static_cast<std::int32_t>(previous.scissor.extent.width),
                                              command.scissor.offset.x + static_cast<std::int32_t>(command.scissor.extent.width));
          const std::int32_t bottom = std::max(previous.scissor.offset.y + static_cast<std::int32_t>(previous.scissor.extent.height),
                                               command.scissor.offset.y + static_cast<std::int32_t>(command.scissor.extent.height));
          previous.scissor.offset.x = std::min(previous.scissor.offset.x, command.scissor.offset.x);
          previous.scissor.offset.y = std::min(previous.scissor.offset.y, command.scissor.offset.y);
          previous.scissor.extent = {static_cast<std::uint32_t>(right - previous.scissor.offset.x),
                                     static_cast<std::uint32_t>(bottom - previous.scissor.offset.y)};
          previous.count += command.count;
          continue;
        }
      }
      s.deferredCommands.push_back(command);
    }
  }
  record.commandCount = static_cast<std::uint32_t>(s.deferredCommands.size()) - record.firstCommand;

  // IN-419: the batch reads its textures (one palette row) and reads and writes its frame and depth.
  const Impl::Use textureUse = paletted ? Impl::Use(texture, static_cast<std::uint32_t>(batch.paletteRow)) : Impl::Use(texture);
  // TASK-168: and the palette rows of its later palette segments (every row its vertices name).
  static const std::vector<std::int32_t> kNoSegmentRows;
  const std::vector<std::int32_t> &segmentRows = paletted && batch.segmentPaletteRows ? *batch.segmentPaletteRows : kNoSegmentRows;
  if (selfCopyInPass) {
    // TASK-182 R1: the copy was drawn into the pass the flush left open; this batch follows it there. Its
    // texture-from-target barrier orders it after the copy (the fragment barrier of record_draw).
    if (!s.renderPassOpen) {  // a descriptor-set allocation may have submitted in between
      s.full_barrier("self copy draw: the batch's descriptor set began a new recording");
      s.begin_draw_pass();
    }
    s.record_draw(record, false);
    s.deferredCommands.resize(record.firstCommand);
    // The accesses of the batch, as the non-deferred path records them: a later compute or transfer command is ordered
    // against them. The copy draw's own write of the pooled image is ordered by the barriers around it (a full barrier
    // would be illegal inside the pass), so it is not added to the pending writes.
    s.pendingReads.add(textureUse.serial, textureUse.region);
    for (const Impl::Use &use : {Impl::Use(texels), Impl::Use(frame), Impl::Use(depth)}) {
      s.pendingReads.add(use.serial, use.region);
    }
    for (const std::int32_t row : segmentRows) s.pendingReads.add(texture->serial, static_cast<std::uint32_t>(row));
    for (const Impl::Use &use : {Impl::Use(frame), Impl::Use(depth)}) s.pendingWrites.add(use.serial, use.region);
  } else if (s.deferDraws) {
    // IN-420: recorded with the next render pass.
    for (const Impl::Use &use : {Impl::Use(texels), textureUse, Impl::Use(frame), Impl::Use(depth)}) {
      s.drawReads.add(use.serial, use.region);
    }
    for (const std::int32_t row : segmentRows) s.drawReads.add(texture->serial, static_cast<std::uint32_t>(row));
    for (const Impl::Use &use : {Impl::Use(frame), Impl::Use(depth)}) s.drawWrites.add(use.serial, use.region);
    s.deferred.push_back(record);
  } else {
    bool first = false;
    if (!s.renderPassOpen) {
      if (s.lazyBarriers) {
        s.order({texels, textureUse, frame, depth}, {frame, depth}, 0, 0);
        for (const std::int32_t row : segmentRows) s.order({Impl::Use(texture, static_cast<std::uint32_t>(row))}, {}, 0, 0);
      }
      s.begin_draw_pass();
      first = true;
    } else if (s.lazyBarriers) {
      for (const Impl::Use &use : {Impl::Use(texels), textureUse, Impl::Use(frame), Impl::Use(depth)}) {
        s.pendingReads.add(use.serial, use.region);
      }
      for (const std::int32_t row : segmentRows) s.pendingReads.add(texture->serial, static_cast<std::uint32_t>(row));
      for (const Impl::Use &use : {Impl::Use(frame), Impl::Use(depth)}) s.pendingWrites.add(use.serial, use.region);
    }
    s.record_draw(record, first);
    s.deferredCommands.resize(record.firstCommand);
  }
  s.stats.batches++;
  // TASK-178: the first submission after a waited one comes after submitAfterWait batches (afterWait is only set when it is
  // not 0), every later one after submitEvery.
  const bool afterWait = s.afterWait;
  const std::uint32_t every = afterWait ? s.submitAfterWait : s.submitEvery;
  if (every != 0 && ++s.batchesSinceSubmit >= every) {
    if (!s.submit(false, error)) return false;
    s.stats.midFrameSubmissions++;
    if (afterWait) s.stats.afterWaitSubmissions++;
  }
  return true;
}

bool Gpu::UploadGlm(const std::uint8_t *memory,
                    const std::vector<std::pair<std::uint32_t, std::uint32_t>> &ranges, std::string &error) {
  Impl &s = *impl_;
  if (ranges.empty()) return true;
  if (!s.begin_recording(error)) return false;
  std::vector<VkBufferCopy> copies;
  copies.reserve(ranges.size());
  GlmPages written;  // IN-464
  for (const auto &[offset, size] : ranges) {
    if (offset + size > (4u << 20)) continue;
    VkDeviceSize at = 0;
    if (!s.ring(s.staging, size, 16, at, error)) return false;
    std::memcpy(static_cast<std::uint8_t *>(s.staging.mapped) + at, memory + offset, size);
    copies.push_back(VkBufferCopy{at, offset, size});
    s.stats.cpuGlmBytes += size;  // P1-C: the pages this VBlank wrote into the GLM
    if (size != 0) written.merge(s.glm_pages_of_blocks(offset / 256u, (offset + size - 1u) / 256u - offset / 256u + 1u));
  }
  s.outside_pass();
  s.touch("glm upload", {}, {}, 0, s.kGlmBuffer, nullptr, &written);
  if (!copies.empty()) {
    gt4_hwd_vkCmdCopyBuffer(s.cmd, s.staging.buffer, s.glm, static_cast<std::uint32_t>(copies.size()), copies.data());
    for (const VkBufferCopy &copy : copies) s.stats.gpuCopyBufferBytes += copy.size;  // P1-C
  }
  s.after_op();
  return true;
}

bool Gpu::DecodeTexture(ImageId id, std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, std::uint32_t width,
                        std::uint32_t height, const std::uint32_t *palette, std::string &error, std::uint32_t originX,
                        std::uint32_t originY) {
  Impl &s = *impl_;
  const Image *image = s.image_of(id);
  if (!image || originX + width > image->width || originY + height > image->height) {
    error = "CORE2 texture decode outside its image";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  VkDeviceSize paletteOffset = 0;
  const VkDeviceSize align = std::max<VkDeviceSize>(s.limits.minStorageBufferOffsetAlignment, 16);
  // The window first: a ring that submits moves to the next share, so later offsets stay in this recording.
  VkDescriptorBufferInfo glmRange{s.glm, 0, 4u << 20};
  std::uint32_t windowBase = 0, windowWords = 0;
  if (!s.bind_window(glmRange, windowBase, windowWords, error)) return false;
  if (!s.ring(s.staging, 256 * 4, align, paletteOffset, error)) return false;
  if (palette) {
    std::memcpy(static_cast<std::uint8_t *>(s.staging.mapped) + paletteOffset, palette, 256 * 4);
    s.stats.cpuPaletteBytes += 256u * 4u;  // P1-C: the palette expanded for this decode
  }
  s.outside_pass();
  const GlmPages texturePages = s.glm_pages_of_rect(bp, bw, psm, 0, 0, static_cast<std::int32_t>(width) - 1,
                                                          static_cast<std::int32_t>(height) - 1);  // IN-464
  // IN-464: palette modes 0 and 1 never read the GPU CLUT (DecodeTextureGpuClut is mode 2), so no CLUT hazard.
  s.touch("decode", {}, {image}, windowWords != 0 ? 0u : s.kGlmBuffer, 0, &texturePages);
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!s.allocate_set(s.decodeSetLayout, set, error)) return false;
  VkDescriptorBufferInfo buffers[4] = {glmRange,
                                       {s.glmTables.buffer, 0, s.glmTableBytes},
                                       {s.staging.buffer, paletteOffset, 256 * 4},
                                       {s.clut, 0, 512 * 4}};  // the GPU CLUT: read only in mode 2 (IN-408)
  VkDescriptorImageInfo texels{VK_NULL_HANDLE, image->view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet writes[5]{};
  const std::uint32_t bindingOf[5] = {0, 1, 2, 3, 5};
  for (std::uint32_t i = 0; i < 5; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = bindingOf[i];
    writes[i].descriptorCount = 1;
    if (i == 3) {
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      writes[i].pImageInfo = &texels;
    } else {
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[i].pBufferInfo = &buffers[i < 3 ? i : 3];
    }
  }
  s.update_sets(5, writes);
  std::uint32_t push[22] = {};
  push[0] = bp;
  push[1] = bw;
  push[2] = psm;
  push[3] = width;
  push[4] = height;
  push[5] = palette ? 1u : 0u;
  push[12] = originX;  // IN-428: a mip level's place in the texture image
  push[13] = originY;
  push[20] = windowBase;  // IN-422
  push[21] = windowWords;
  gt4_hwd_vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.decodePipeline);
  gt4_hwd_vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.decodeLayout, 0, 1, &set, 0, nullptr);
  gt4_hwd_vkCmdPushConstants(s.cmd, s.decodeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
  gt4_hwd_vkCmdDispatch(s.cmd, (width + 7u) / 8u, (height + 7u) / 8u, 1);
  s.after_op();
  s.stats.atlasUploads++;
  return true;
}

bool Gpu::ProfileStart(std::string &error) {
  Impl &s = *impl_;
  if (!s.timestamps) return true;  // no timestamps on this device
  if (!s.profilePool) {
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = Impl::kProfileQueries;
    if (gt4_hwd_vkCreateQueryPool(s.device, &info, nullptr, &s.profilePool) != VK_SUCCESS) return true;
  }
  if (!s.begin_recording(error)) return false;
  s.flush_draws();
  s.end_render_pass();
  gt4_hwd_vkCmdResetQueryPool(s.cmd, s.profilePool, 0, Impl::kProfileQueries);
  s.profileNext = 0;
  s.profileLabels.clear();
  s.profileOpen = false;
  s.profiling = true;
  return true;
}

bool Gpu::ProfileReport(std::string &error) {
  Impl &s = *impl_;
  if (!s.profiling) return true;
  s.prof_end();
  s.profiling = false;
  if (!s.submit(true, error)) return false;
  std::vector<std::uint64_t> ticks(s.profileNext);
  if (s.profileNext == 0 ||
      gt4_hwd_vkGetQueryPoolResults(s.device, s.profilePool, 0, s.profileNext, ticks.size() * sizeof(std::uint64_t),
                                    ticks.data(), sizeof(std::uint64_t),
                                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) {
    std::fprintf(stderr, "[core2] gpu profile: no results\n");
    return true;
  }
  struct Kind {
    double total = 0, longest = 0;
    std::uint32_t count = 0;
  };
  std::map<std::string, Kind> kinds;
  double total = 0;
  for (std::size_t i = 0; i < s.profileLabels.size(); ++i) {
    const std::uint64_t a = ticks[2 * i], b = ticks[2 * i + 1];
    const double ms = b >= a ? static_cast<double>(b - a) * s.timestampNs / 1.0e6 : 0.0;
    Kind &kind = kinds[s.profileLabels[i]];
    kind.total += ms;
    kind.longest = std::max(kind.longest, ms);
    kind.count++;
    total += ms;
    // GT4_CORE2_GPU_PROFILE_ORDER=1: every command in the order it ran, to see what follows what on a target.
    static const bool inOrder = std::getenv("GT4_CORE2_GPU_PROFILE_ORDER") != nullptr;
    if (inOrder) std::fprintf(stderr, "[core2] gpu order %4zu %7.3f ms %s\n", i, ms, s.profileLabels[i].c_str());
  }
  std::vector<std::pair<double, std::string>> rows;
  for (const auto &[label, kind] : kinds) rows.push_back({kind.total, label});
  std::sort(rows.rbegin(), rows.rend());
  std::fprintf(stderr, "[core2] gpu profile: %zu commands, %.2f ms of isolated GPU time\n", s.profileLabels.size(), total);
  for (std::size_t i = 0; i < rows.size() && i < 40; ++i) {
    const Kind &kind = kinds[rows[i].second];
    std::fprintf(stderr, "[core2] gpu profile %7.3f ms %5u x (max %.3f) %s\n", kind.total, kind.count, kind.longest,
                 rows[i].second.c_str());
  }
  return true;
}

void Gpu::SetDecodeWindow(const std::uint8_t *memory, std::uint32_t firstPage, std::uint32_t pageCount) {
  impl_->windowMemory = memory;
  impl_->windowFirstPage = firstPage;
  impl_->windowPages = pageCount;
}

bool Gpu::DecodeTextureGpuClut(ImageId id, std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, std::uint32_t width,
                               std::uint32_t height, const GpuPalette &palette, std::string &error, std::uint32_t originX,
                               std::uint32_t originY) {
  Impl &s = *impl_;
  const Image *image = s.image_of(id);
  if (!image || originX + width > image->width || originY + height > image->height) {
    error = "CORE2 texture decode outside its image";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  VkDescriptorBufferInfo glmRange{s.glm, 0, 4u << 20};
  std::uint32_t windowBase = 0, windowWords = 0;
  if (!s.bind_window(glmRange, windowBase, windowWords, error)) return false;
  s.outside_pass();
  const GlmPages texturePages = s.glm_pages_of_rect(bp, bw, psm, 0, 0, static_cast<std::int32_t>(width) - 1,
                                                          static_cast<std::int32_t>(height) - 1);  // IN-464
  s.touch("decode gpu clut", {}, {image}, (windowWords != 0 ? 0u : s.kGlmBuffer) | s.kClutBuffer, 0, &texturePages);
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!s.allocate_set(s.decodeSetLayout, set, error)) return false;
  VkDescriptorBufferInfo buffers[4] = {glmRange,
                                       {s.glmTables.buffer, 0, s.glmTableBytes},
                                       {s.glmTables.buffer, 0, s.glmTableBytes},  // the palette ring is not read
                                       {s.clut, 0, 512 * 4}};
  VkDescriptorImageInfo texels{VK_NULL_HANDLE, image->view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet writes[5]{};
  const std::uint32_t bindingOf[5] = {0, 1, 2, 3, 5};
  for (std::uint32_t i = 0; i < 5; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = bindingOf[i];
    writes[i].descriptorCount = 1;
    if (i == 3) {
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      writes[i].pImageInfo = &texels;
    } else {
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[i].pBufferInfo = &buffers[i < 3 ? i : 3];
    }
  }
  s.update_sets(5, writes);
  std::uint32_t push[22] = {};  // IN-422: the decode window words last (0: no window)
  push[0] = bp;
  push[1] = bw;
  push[2] = psm;
  push[3] = width;
  push[4] = height;
  push[5] = 2u;  // GPU CLUT buffer
  push[7] = palette.cpsm;
  push[8] = palette.csa;
  push[9] = static_cast<std::uint32_t>(palette.texa & 0xFFu);          // TEXA.TA0
  push[10] = static_cast<std::uint32_t>((palette.texa >> 32) & 0xFFu); // TEXA.TA1
  push[11] = static_cast<std::uint32_t>((palette.texa >> 15) & 1u);    // TEXA.AEM
  push[12] = originX;  // IN-428
  push[13] = originY;
  push[20] = windowBase;  // IN-422
  push[21] = windowWords;
  gt4_hwd_vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.decodePipeline);
  gt4_hwd_vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.decodeLayout, 0, 1, &set, 0, nullptr);
  gt4_hwd_vkCmdPushConstants(s.cmd, s.decodeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
  gt4_hwd_vkCmdDispatch(s.cmd, (width + 7u) / 8u, (height + 7u) / 8u, 1);
  s.after_op();
  s.stats.atlasUploads++;
  return true;
}

bool Gpu::PaletteRowFromGpuClut(ImageId palettes, std::uint32_t row, std::uint32_t psm, const GpuPalette &palette,
                                std::string &error) {
  Impl &s = *impl_;
  const Image *image = s.image_of(palettes);
  if (!image || image->width < 256 || row >= image->height) {
    error = "CORE2 palette row outside the palette image";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  s.outside_pass();
  s.touch("palette row", {Impl::Use(image, row)}, {Impl::Use(image, row)}, s.kClutBuffer, 0);
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!s.allocate_set(s.decodeSetLayout, set, error)) return false;
  VkDescriptorBufferInfo buffers[4] = {{s.glm, 0, 4u << 20},
                                       {s.glmTables.buffer, 0, s.glmTableBytes},
                                       {s.glmTables.buffer, 0, s.glmTableBytes},  // the palette ring is not read
                                       {s.clut, 0, 512 * 4}};
  VkDescriptorImageInfo texels{VK_NULL_HANDLE, image->view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet writes[5]{};
  const std::uint32_t bindingOf[5] = {0, 1, 2, 3, 5};
  for (std::uint32_t i = 0; i < 5; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = bindingOf[i];
    writes[i].descriptorCount = 1;
    if (i == 3) {
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      writes[i].pImageInfo = &texels;
    } else {
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[i].pBufferInfo = &buffers[i < 3 ? i : 3];
    }
  }
  s.update_sets(5, writes);
  std::uint32_t push[22] = {};  // IN-422: the decode window words last (0: no window)
  push[2] = psm;
  push[3] = 256;
  push[4] = 1;
  push[5] = 5u;  // one palette row from the GPU CLUT
  push[7] = palette.cpsm;
  push[8] = palette.csa;
  push[9] = static_cast<std::uint32_t>(palette.texa & 0xFFu);          // TEXA.TA0
  push[10] = static_cast<std::uint32_t>((palette.texa >> 32) & 0xFFu); // TEXA.TA1
  push[11] = static_cast<std::uint32_t>((palette.texa >> 15) & 1u);    // TEXA.AEM
  push[13] = row;                                                      // origin_y
  gt4_hwd_vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.decodePipeline);
  gt4_hwd_vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.decodeLayout, 0, 1, &set, 0, nullptr);
  gt4_hwd_vkCmdPushConstants(s.cmd, s.decodeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
  gt4_hwd_vkCmdDispatch(s.cmd, 32, 1, 1);  // 256 entries, 8 x 8 groups
  s.after_op();
  s.stats.paletteRows++;
  return true;
}

bool Gpu::ClutFromTarget(ImageId target, const ClutTargetLoad &load, std::string &error) {
  Impl &s = *impl_;
  const Image *image = s.image_of(target);
  if (!image) {
    error = "CORE2 CLUT load from a target image that does not exist";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  s.outside_pass();
  // IN-464: a CSM1 palette lies in the page of CBP and the next one (as the front end reports it before a load); a
  // CSM2 TEXCLUT row can be anywhere.
  const GlmPages palettePages =
      load.csm == 0 ? s.glm_pages_of_blocks(load.cbp & ~31u, 64u) : GlmPages::all();
  s.touch("clut from target", {image}, {}, s.kGlmBuffer, s.kClutBuffer, &palettePages);
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!s.allocate_set(s.decodeSetLayout, set, error)) return false;
  VkDescriptorBufferInfo buffers[4] = {{s.glm, 0, 4u << 20},
                                       {s.glmTables.buffer, 0, s.glmTableBytes},
                                       {s.inverseCt32.buffer, 0, 2048 * 4},
                                       {s.clut, 0, 512 * 4}};
  VkDescriptorImageInfo source{VK_NULL_HANDLE, image->view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet writes[5]{};
  const std::uint32_t bindingOf[5] = {0, 1, 2, 3, 5};
  for (std::uint32_t i = 0; i < 5; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = bindingOf[i];
    writes[i].descriptorCount = 1;
    if (i == 3) {
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      writes[i].pImageInfo = &source;
    } else {
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[i].pBufferInfo = &buffers[i < 3 ? i : 3];
    }
  }
  s.update_sets(5, writes);
  std::uint32_t push[18] = {load.cbp,
                            load.cpsm,
                            load.csm,
                            load.csa,
                            load.eightBit ? 1u : 0u,
                            load.cbw,
                            load.cou,
                            load.cov,
                            load.tbp,
                            load.tbw,
                            image->nativeWidth,
                            image->nativeHeight,
                            static_cast<std::uint32_t>(load.heldX0),
                            static_cast<std::uint32_t>(load.heldY0),
                            static_cast<std::uint32_t>(load.heldX1),
                            static_cast<std::uint32_t>(load.heldY1),
                            load.fullWord ? 1u : 0u,
                            FloatBits(static_cast<float>(image->scale))};
  gt4_hwd_vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.clutTargetPipeline);
  gt4_hwd_vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.decodeLayout, 0, 1, &set, 0, nullptr);
  gt4_hwd_vkCmdPushConstants(s.cmd, s.decodeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
  gt4_hwd_vkCmdDispatch(s.cmd, 4, 1, 1);  // 256 entries, 64 per group
  s.after_op();
  s.stats.clutTargetLoads++;
  return true;
}

bool Gpu::MaterializeTarget(ImageId id, std::uint32_t psm, std::uint32_t tbp, std::uint32_t tbw, std::int32_t x0, std::int32_t y0,
                            std::int32_t x1, std::int32_t y1, bool fullWord, std::string &error) {
  Impl &s = *impl_;
  const Image *image = s.image_of(id);
  if (!image || x0 < 0 || y0 < 0 || x1 < x0 || y1 < y0) {
    error = "CORE2 GLM write of an image or rectangle that does not exist";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  s.outside_pass();
  const GlmPages targetPages = s.glm_pages_of_rect(tbp, tbw, psm, x0, y0, x1, y1);  // IN-464
  s.touch("glm write (target)", {image}, {}, s.kGlmBuffer, s.kGlmBuffer, &targetPages, &targetPages);
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!s.allocate_set(s.decodeSetLayout, set, error)) return false;
  VkDescriptorBufferInfo buffers[4] = {{s.glm, 0, 4u << 20},
                                       {s.glmTables.buffer, 0, s.glmTableBytes},
                                       {s.glmTables.buffer, 0, s.glmTableBytes},  // bindings 2 and 5 are not read
                                       {s.clut, 0, 512 * 4}};
  VkDescriptorImageInfo source{VK_NULL_HANDLE, image->view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet writes[5]{};
  const std::uint32_t bindingOf[5] = {0, 1, 2, 3, 5};
  for (std::uint32_t i = 0; i < 5; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = bindingOf[i];
    writes[i].descriptorCount = 1;
    if (i == 3) {
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      writes[i].pImageInfo = &source;
    } else {
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[i].pBufferInfo = &buffers[i < 3 ? i : 3];
    }
  }
  s.update_sets(5, writes);
  const std::uint32_t width = static_cast<std::uint32_t>(x1 - x0 + 1);
  const std::uint32_t height = static_cast<std::uint32_t>(y1 - y0 + 1);
  const std::uint32_t push[9] = {tbp,   tbw, static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(y0), width,
                                 height, fullWord ? 1u : 0u, FloatBits(static_cast<float>(image->scale)), psm};
  gt4_hwd_vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.materializePipeline);
  gt4_hwd_vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.decodeLayout, 0, 1, &set, 0, nullptr);
  gt4_hwd_vkCmdPushConstants(s.cmd, s.decodeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
  gt4_hwd_vkCmdDispatch(s.cmd, (width + 7u) / 8u, (height + 7u) / 8u, 1);
  s.after_op();
  s.stats.materializations++;
  return true;
}

bool Gpu::WriteClut(std::uint32_t first, std::uint32_t count, const std::uint16_t *halfwords, std::string &error) {
  Impl &s = *impl_;
  if (count == 0) return true;
  if (first + count > 512) {
    error = "CORE2 GPU CLUT write past its 512 halfwords";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  VkDeviceSize offset = 0;
  if (!s.ring(s.staging, count * 4u, 16, offset, error)) return false;
  auto *values = reinterpret_cast<std::uint32_t *>(static_cast<std::uint8_t *>(s.staging.mapped) + offset);
  for (std::uint32_t i = 0; i < count; ++i) values[i] = halfwords[first + i];
  s.stats.cpuClutBytes += count * 4u;  // P1-C
  s.outside_pass();
  s.touch("clut write", {}, {}, 0, s.kClutBuffer);
  VkBufferCopy copy{offset, first * 4u, count * 4u};
  gt4_hwd_vkCmdCopyBuffer(s.cmd, s.staging.buffer, s.clut, 1, &copy);
  s.stats.gpuCopyBufferBytes += count * 4u;  // P1-C
  s.after_op();
  s.stats.clutWrites++;
  return true;
}

bool Gpu::ReadClut(std::uint16_t halfwords[512], std::string &error) {
  Impl &s = *impl_;
  if (!s.begin_recording(error)) return false;
  s.flush_draws();
  s.end_render_pass();
  s.full_barrier();  // a host read or a rare pass: ordered at once
  s.cat_mark(Impl::kCatReadback);  // TASK-182 diagnostics
  VkBufferCopy copy{0, 0, 512 * 4};
  gt4_hwd_vkCmdCopyBuffer(s.cmd, s.clut, s.readback.buffer, 1, &copy);
  s.full_barrier();
  s.host_read_barrier();  // TASK-180 test4 (P1-D)
  if (!s.submit(true, error)) return false;
  const auto *values = static_cast<const std::uint32_t *>(s.readback.mapped);
  for (std::uint32_t i = 0; i < 512; ++i) halfwords[i] = static_cast<std::uint16_t>(values[i]);
  s.stats.gpuCopyBufferBytes += 512 * 4;  // P1-C: the GPU CLUT to the readback buffer
  s.stats.hostReadBytes += 512 * 4;
  s.stats.clutReads++;
  return true;
}

bool Gpu::PatchFromGlm(ImageId id, std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, std::int32_t x0,
                       std::int32_t y0, std::int32_t x1, std::int32_t y1, std::uint32_t mask, std::string &error) {
  Impl &s = *impl_;
  const Image *image = s.image_of(id);
  if (!image || x0 < 0 || y0 < 0 || x1 < x0 || y1 < y0) {
    error = "CORE2 GLM patch of an image or rectangle that does not exist";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  s.outside_pass();
  const GlmPages patchPages = s.glm_pages_of_rect(bp, bw, psm, x0, y0, x1, y1);  // IN-464
  s.touch("dirty patch", {image}, {image}, s.kGlmBuffer, 0, &patchPages);
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!s.allocate_set(s.decodeSetLayout, set, error)) return false;
  VkDescriptorBufferInfo buffers[3] = {{s.glm, 0, 4u << 20}, {s.glmTables.buffer, 0, s.glmTableBytes},
                                       {s.glm, 0, 1024 * 4}};  // bindings 2 and 5 are not read by this kernel
  VkDescriptorImageInfo target{VK_NULL_HANDLE, image->view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet writes[5]{};
  const std::uint32_t bindingOf[5] = {0, 1, 2, 3, 5};
  for (std::uint32_t i = 0; i < 5; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = bindingOf[i];
    writes[i].descriptorCount = 1;
    if (i == 3) {
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      writes[i].pImageInfo = &target;
    } else {
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[i].pBufferInfo = &buffers[i < 2 ? i : 2];
    }
  }
  s.update_sets(5, writes);
  const std::uint32_t width = static_cast<std::uint32_t>(x1 - x0 + 1);
  const std::uint32_t height = static_cast<std::uint32_t>(y1 - y0 + 1);
  const std::uint32_t push[9] = {bp, bw, psm, static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(y0),
                                 width, height, image->scale, mask};
  gt4_hwd_vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.patchGlmPipeline);
  gt4_hwd_vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.decodeLayout, 0, 1, &set, 0, nullptr);
  gt4_hwd_vkCmdPushConstants(s.cmd, s.decodeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
  gt4_hwd_vkCmdDispatch(s.cmd, (width * image->scale + 7u) / 8u, (height * image->scale + 7u) / 8u, 1);
  s.after_op();
  s.stats.patches++;
  return true;
}

bool Gpu::Patch(ImageId id, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                const std::vector<std::uint32_t> &words, std::uint32_t mask, std::string &error) {
  Impl &s = *impl_;
  const Image *image = s.image_of(id);
  if (!image) {
    error = "CORE2 patches an image that does not exist";
    return false;
  }
  const std::uint32_t width = static_cast<std::uint32_t>(x1 - x0 + 1);
  const std::uint32_t height = static_cast<std::uint32_t>(y1 - y0 + 1);
  if (x0 < 0 || y0 < 0 || x1 < x0 || y1 < y0 || words.size() != static_cast<std::size_t>(width) * height * (mask == 0u ? 2u : 1u)) {
    error = "CORE2 patch rectangle and words do not match";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  const VkDeviceSize bytes = words.size() * 4u;
  VkDeviceSize offset = 0;
  const VkDeviceSize align = std::max<VkDeviceSize>(s.limits.minStorageBufferOffsetAlignment, 16);
  if (!s.ring(s.staging, bytes, align, offset, error)) return false;
  std::memcpy(static_cast<std::uint8_t *>(s.staging.mapped) + offset, words.data(), static_cast<std::size_t>(bytes));
  s.stats.cpuPatchBytes += bytes;  // P1-C
  s.outside_pass();
  // One row is tracked as a row (the palette rows of IN-409; IN-420), anything else as the whole image.
  const std::uint32_t region = y0 == y1 ? static_cast<std::uint32_t>(y0) : Impl::kWholeImage;
  s.touch("patch (cpu words)", {Impl::Use(image, region)}, {Impl::Use(image, region)}, 0, 0);
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!s.compute_set(s.staging.buffer, offset, bytes, image->view, image->view, image->view, set, error)) return false;
  const std::uint32_t push[7] = {0u, static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(y0), width, height,
                                 image->scale, mask};
  s.dispatch(s.patchPipeline, set, push, 7, width * image->scale, height * image->scale);
  s.stats.patches++;
  return true;
}

bool Gpu::Reduce(ImageId id, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                 std::vector<std::uint32_t> &words, std::string &error) {
  Impl &s = *impl_;
  const Image *image = s.image_of(id);
  if (!image || x0 < 0 || y0 < 0 || x1 < x0 || y1 < y0) {
    error = "CORE2 reduces an image or rectangle that does not exist";
    return false;
  }
  const std::uint32_t width = static_cast<std::uint32_t>(x1 - x0 + 1);
  const std::uint32_t height = static_cast<std::uint32_t>(y1 - y0 + 1);
  const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4u;
  if (bytes > s.readbackBytes) {
    error = "CORE2 reduction larger than the readback buffer";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  s.flush_draws();
  s.end_render_pass();
  s.full_barrier();  // a host read or a rare pass: ordered at once
  s.cat_mark(Impl::kCatReadback);  // TASK-182 diagnostics
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!s.compute_set(s.readback.buffer, 0, bytes, image->view, image->view, image->view, set, error)) return false;
  const std::uint32_t push[6] = {0u, static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(y0), width, height,
                                 FloatBits(static_cast<float>(image->scale))};
  s.dispatch(s.reducePipeline, set, push, 6, width, height);
  s.full_barrier();       // the host reads the result
  s.host_read_barrier();  // TASK-180 test4 (P1-D): and the host-domain dependency the spec asks for
  if (!s.submit(true, error)) return false;
  words.resize(static_cast<std::size_t>(width) * height);
  std::memcpy(words.data(), s.readback.mapped, static_cast<std::size_t>(bytes));
  s.stats.hostReadBytes += bytes;
  s.stats.reductions++;
  return true;
}

bool Gpu::ReduceMany(const std::vector<ReduceRequest> &requests, std::vector<std::vector<std::uint32_t>> &words,
                     std::string &error) {
  Impl &s = *impl_;
  error.clear();
  const VkDeviceSize align = std::max<VkDeviceSize>(s.limits.minStorageBufferOffsetAlignment, 16);
  std::vector<VkDeviceSize> offsets;
  VkDeviceSize total = 0;
  for (const ReduceRequest &r : requests) {
    const Image *image = s.image_of(r.id);
    if (!image || r.x0 < 0 || r.y0 < 0 || r.x1 < r.x0 || r.y1 < r.y0) {
      error = "CORE2 reduces an image or rectangle that does not exist";
      return false;
    }
    total = (total + align - 1) / align * align;
    offsets.push_back(total);
    total += static_cast<VkDeviceSize>(r.x1 - r.x0 + 1) * static_cast<VkDeviceSize>(r.y1 - r.y0 + 1) * 4u;
  }
  if (total > s.readbackBytes) return false;  // empty error: reduce one by one
  if (!s.begin_recording(error)) return false;
  s.flush_draws();
  s.end_render_pass();
  s.full_barrier();  // a host read: ordered at once
  s.cat_mark(Impl::kCatReadback);  // TASK-182 diagnostics
  for (std::size_t i = 0; i < requests.size(); ++i) {
    const ReduceRequest &r = requests[i];
    const Image *image = s.image_of(r.id);
    const std::uint32_t width = static_cast<std::uint32_t>(r.x1 - r.x0 + 1);
    const std::uint32_t height = static_cast<std::uint32_t>(r.y1 - r.y0 + 1);
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!s.compute_set(s.readback.buffer, offsets[i], static_cast<VkDeviceSize>(width) * height * 4u, image->view,
                       image->view, image->view, set, error)) {
      return false;
    }
    const std::uint32_t push[6] = {0u, static_cast<std::uint32_t>(r.x0), static_cast<std::uint32_t>(r.y0), width, height,
                                   FloatBits(static_cast<float>(image->scale))};
    s.dispatch(s.reducePipeline, set, push, 6, width, height);  // distinct buffer ranges: no barrier between them
    s.stats.reductions++;
  }
  s.full_barrier();       // the host reads the results
  s.host_read_barrier();  // TASK-180 test4 (P1-D)
  if (!s.submit(true, error)) return false;
  words.resize(requests.size());
  for (std::size_t i = 0; i < requests.size(); ++i) {
    const ReduceRequest &r = requests[i];
    const std::size_t count = static_cast<std::size_t>(r.x1 - r.x0 + 1) * static_cast<std::size_t>(r.y1 - r.y0 + 1);
    words[i].resize(count);
    std::memcpy(words[i].data(), static_cast<const std::uint8_t *>(s.readback.mapped) + offsets[i], count * 4u);
    s.stats.hostReadBytes += count * 4u;
  }
  return true;
}

// IN-439: true once submission `serial` has completed (a fence poll, no wait); one still being recorded has not.
static bool SubmissionComplete(const VkDevice device, const VkFence fence, bool inFlight, std::string &error) {
  if (!inFlight) return true;
  const VkResult result = gt4_hw_vkWaitForFences(device, 1, &fence, VK_TRUE, 0);
  if (result == VK_TIMEOUT) return false;
  return check(result, "vkWaitForFences", error);
}

bool Gpu::ReduceLater(ImageId id, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                      std::uint32_t &ticket, std::string &error) {
  Impl &s = *impl_;
  error.clear();
  const Image *image = s.image_of(id);
  if (!image || x0 < 0 || y0 < 0 || x1 < x0 || y1 < y0) {
    error = "CORE2 reduces an image or rectangle that does not exist";
    return false;
  }
  const std::uint32_t width = static_cast<std::uint32_t>(x1 - x0 + 1);
  const std::uint32_t height = static_cast<std::uint32_t>(y1 - y0 + 1);
  if (static_cast<std::uint64_t>(width) * height > Impl::kLaterWords) return false;
  const std::uint32_t index = s.laterNext;
  Impl::LaterReduce &slotResult = s.laterReduces[index];
  if (slotResult.serial != 0) {  // its last result was never taken: reuse it once that submission is done
    std::vector<std::uint32_t> unused;
    if (!TakeReduced(index, unused, error)) return false;
  }
  if (!s.begin_recording(error)) return false;
  s.flush_draws();
  s.end_render_pass();
  s.full_barrier();  // a host read: ordered at once
  s.cat_mark(Impl::kCatReadback);  // TASK-182 diagnostics
  const VkDeviceSize offset = static_cast<VkDeviceSize>(index) * Impl::kLaterWords * 4u;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!s.compute_set(s.later.buffer, offset, static_cast<VkDeviceSize>(width) * height * 4u, image->view, image->view,
                     image->view, set, error)) {
    return false;
  }
  const std::uint32_t push[6] = {0u, static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(y0), width, height,
                                 FloatBits(static_cast<float>(image->scale))};
  s.dispatch(s.reducePipeline, set, push, 6, width, height);
  s.full_barrier();       // the host reads the result after the fence
  s.host_read_barrier();  // TASK-180 test4 (P1-D)
  s.stats.reductions++;
  s.stats.laterReductions++;
  slotResult.serial = s.submitCount + 1u;  // the recording in progress (compute_set may have submitted the one before)
  slotResult.count = width * height;
  s.laterNext = (index + 1u) % Impl::kLaterSlots;
  ticket = index;
  return true;
}

bool Gpu::TakeReduced(std::uint32_t ticket, std::vector<std::uint32_t> &words, std::string &error) {
  Impl &s = *impl_;
  error.clear();
  if (ticket >= Impl::kLaterSlots || s.laterReduces[ticket].serial == 0) return false;
  Impl::LaterReduce &result = s.laterReduces[ticket];
  if (result.serial > s.submitCount) return false;  // still in the recording
  for (std::uint32_t i = 0; i < s.slotCount; ++i) {
    const Impl::Slot &slot = s.slots[i];
    if (slot.serial != result.serial) continue;
    if (!SubmissionComplete(s.device, slot.fence, slot.inFlight, error)) return false;
    break;
  }  // no slot carries it any more: that slot was waited for and reused, so it is complete
  words.resize(result.count);
  std::memcpy(words.data(), static_cast<const std::uint8_t *>(s.later.mapped) + static_cast<std::size_t>(ticket) *
                                                                                  Impl::kLaterWords * 4u,
              static_cast<std::size_t>(result.count) * 4u);
  s.stats.hostReadBytes += static_cast<std::uint64_t>(result.count) * 4u;
  result.serial = 0;
  return true;
}

bool Gpu::Tone(ImageId id, std::uint32_t nativeWidth, std::uint32_t nativeHeight, const std::uint32_t palette[256],
               std::string &error) {
  Impl &s = *impl_;
  const Image *image = s.image_of(id);
  if (!image) {
    error = "CORE2 tone pass on an image that does not exist";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  VkDeviceSize offset = 0;
  const VkDeviceSize align = std::max<VkDeviceSize>(s.limits.minStorageBufferOffsetAlignment, 16);
  if (!s.ring(s.staging, 256 * 4, align, offset, error)) return false;
  std::memcpy(static_cast<std::uint8_t *>(s.staging.mapped) + offset, palette, 256 * 4);
  s.stats.cpuToneBytes += 256u * 4u;  // P1-C
  s.outside_pass();
  s.touch("tone", {image}, {image}, 0, 0);
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!s.compute_set(s.staging.buffer, offset, 256 * 4, image->view, image->view, image->view, set, error)) return false;
  const std::uint32_t width = std::min(nativeWidth, image->nativeWidth) * image->scale;
  const std::uint32_t height = std::min(nativeHeight, image->nativeHeight) * image->scale;
  const std::uint32_t push[2] = {width, height};
  s.dispatch(s.tonePipeline, set, push, 2, width, height);
  s.stats.tones++;
  return true;
}

bool Gpu::Compose(const std::uint32_t words[28], ImageId sourceA, ImageId sourceB, std::uint32_t outWidth,
                  std::uint32_t outHeight, bool intoField, std::string &error) {
  Impl &s = *impl_;
  if (outWidth == 0 || outHeight == 0) {
    error = "CORE2 compose of an empty display";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  // The display image always exists at the display size: it is the presentation target and, when the deinterlace writes
  // the field's own history image, the image the deinterlaced result is written over.
  if (s.display.width != outWidth || s.display.height != outHeight) {
    s.doom(s.display);
    if (!s.make_image(s.display, outWidth, outHeight, 1, error)) return false;
  }
  s.post = &s.display;
  Image *out = &s.display;
  if (intoField) {
    out = s.fieldTarget;
    if (!out || !out->alive || out->width != outWidth || out->height != outHeight) {
      error = "CORE2 compose into a deinterlace field that was not prepared";
      return false;
    }
    s.stats.fieldComposes++;
  }
  const Image *a = s.image_of(sourceA);
  const Image *b = s.image_of(sourceB);
  s.outside_pass();
  s.touch("compose", {a, b}, {out}, 0, 0);
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!s.compute_set(s.staging.buffer, 0, 16, a ? a->view : s.atlas.view, b ? b->view : s.atlas.view, out->view,
                     set, error)) {
    return false;
  }
  s.dispatch(s.composePipeline, set, words, 28, outWidth, outHeight);
  s.stats.composes++;
  return true;
}

std::uint64_t Gpu::DisplayImage() const {
  return reinterpret_cast<std::uint64_t>(impl_->post->image);
}

bool Gpu::PrepareDeinterlace(const gpu::GpuDrawer::DeinterlaceStep &step, std::string &error) {
  Impl &s = *impl_;
  s.fieldTarget = nullptr;
  if (step.width == 0 || step.height == 0 || step.current >= gpu::kGpuDeinterlaceHistory || step.mode < 2 ||
      step.mode > 6) {
    error = "CORE2 deinterlace outside the display image";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  // The history follows the display size. A new size starts a new history: the caller's step then says there is no
  // earlier field and no earlier output (its history is cleared on the same size change).
  bool fresh = false;
  for (Image &image : s.deinterlaceHistory) {
    if (!s.post_image(image, step.width, step.height, fresh, error)) return false;
  }
  if (!s.post_image(s.deinterlaceWoven, step.width, step.height, fresh, error)) return false;
  if (s.display.width != step.width || s.display.height != step.height) {
    s.doom(s.display);
    if (!s.make_image(s.display, step.width, step.height, 1, error)) return false;
  }
  s.fieldTarget = &s.deinterlaceHistory[step.current];
  return true;
}

bool Gpu::SnapshotLastOutput(std::string &error) {
  Impl &s = *impl_;
  if (!s.display.alive || s.display.width == 0 || s.display.height == 0) {
    error = "CORE2 last output snapshot without a display image";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  bool fresh = false;
  if (!s.post_image(s.deinterlaceOutput, s.display.width, s.display.height, fresh, error)) return false;
  s.flush_draws();
  s.end_render_pass();
  s.full_barrier();
  s.cat_mark(Impl::kCatDeinterlace);  // TASK-182 diagnostics
  VkImageCopy region{};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.extent = {s.display.width, s.display.height, 1};
  gt4_hwd_vkCmdCopyImage(s.cmd, s.display.image, VK_IMAGE_LAYOUT_GENERAL, s.deinterlaceOutput.image,
                         VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  s.full_barrier();
  s.stats.gpuCopyImageBytes += static_cast<std::uint64_t>(s.display.width) * s.display.height * 4u;  // P1-C
  s.stats.postCopies++;
  return true;
}

bool Gpu::Deinterlace(const gpu::GpuDrawer::DeinterlaceStep &step, bool fieldComposed, std::string &error) {
  Impl &s = *impl_;
  if (step.width == 0 || step.height == 0 || step.width > s.display.width || step.height > s.display.height ||
      step.current >= gpu::kGpuDeinterlaceHistory || step.mode < 2 || step.mode > 6) {
    error = "CORE2 deinterlace outside the display image";
    return false;
  }
  if (fieldComposed && (!s.fieldTarget || !s.fieldTarget->alive)) {
    error = "CORE2 deinterlace of a field that was not composed";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  // The history follows the display size. A new size starts a new history: the caller's step then says there is no
  // earlier field and no earlier output (its history is cleared on the same size change).
  bool fresh = false;
  for (Image &image : s.deinterlaceHistory) {
    if (!s.post_image(image, step.width, step.height, fresh, error)) return false;
  }
  if (!s.post_image(s.deinterlaceWoven, step.width, step.height, fresh, error)) return false;
  if (!fieldComposed && !s.post_image(s.deinterlaceOutput, step.width, step.height, fresh, error)) return false;
  const Image &field = s.deinterlaceHistory[step.current];
  s.flush_draws();
  s.end_render_pass();
  s.full_barrier();  // a host read or a rare pass: ordered at once
  s.cat_mark(Impl::kCatDeinterlace);  // TASK-182 diagnostics
  VkImageCopy region{};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.extent = {step.width, step.height, 1};
  if (!fieldComposed) {
    // The display merge wrote the display image: keep this field's words in its history entry, then deinterlace over
    // the display image.
    gt4_hwd_vkCmdCopyImage(s.cmd, s.display.image, VK_IMAGE_LAYOUT_GENERAL, field.image, VK_IMAGE_LAYOUT_GENERAL, 1,
                           &region);
    s.full_barrier();
    s.stats.gpuCopyImageBytes += static_cast<std::uint64_t>(step.width) * step.height * 4u;  // P1-C
    s.stats.postCopies++;
  }
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!s.allocate_set(s.deinterlaceSetLayout, set, error)) return false;
  VkDescriptorImageInfo history[gpu::kGpuDeinterlaceHistory];
  for (std::uint32_t i = 0; i < gpu::kGpuDeinterlaceHistory; ++i) {
    history[i] = {VK_NULL_HANDLE, s.deinterlaceHistory[i].view, VK_IMAGE_LAYOUT_GENERAL};
  }
  const VkDescriptorImageInfo woven{VK_NULL_HANDLE, s.deinterlaceWoven.view, VK_IMAGE_LAYOUT_GENERAL};
  // With the field composed into its history entry the last output is in the display image and the shader reads it
  // through `previous_in_output` (the display image is a valid binding either way and is never read here when set).
  const VkDescriptorImageInfo previous{VK_NULL_HANDLE,
                                      fieldComposed ? s.display.view : s.deinterlaceOutput.view,
                                      VK_IMAGE_LAYOUT_GENERAL};
  const VkDescriptorImageInfo output{VK_NULL_HANDLE, s.display.view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet writes[4]{};
  const std::uint32_t bindings[4] = {0, 4, 5, 6};
  const VkDescriptorImageInfo *infos[4] = {history, &woven, &previous, &output};
  for (std::uint32_t i = 0; i < 4; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = bindings[i];
    writes[i].descriptorCount = i == 0 ? gpu::kGpuDeinterlaceHistory : 1u;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[i].pImageInfo = infos[i];
  }
  s.update_sets(4, writes);
  gt4_hwd_vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.deinterlacePipeline);
  gt4_hwd_vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.deinterlaceLayout, 0, 1, &set, 0, nullptr);
  auto dispatch = [&](std::uint32_t mode) {
    const std::uint32_t push[16] = {mode, step.width, step.height, s.scale, step.rowParity, step.field, step.current,
                                    step.previousSame, step.previousOther, step.haveOutput ? 1u : 0u,
                                    step.thresholdPermille, step.initWoven ? 1u : 0u, step.rowStep,
                                    step.olderOther, fieldComposed ? 1u : 0u, step.skipDeadDecision};
    gt4_hwd_vkCmdPushConstants(s.cmd, s.deinterlaceLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
    gt4_hwd_vkCmdDispatch(s.cmd, (step.width + 7) / 8, (step.height + 7) / 8, 1);
    s.full_barrier();
  };
  if (step.mode == 2 || step.mode == 3) dispatch(1);  // the weave update first
  dispatch(step.mode);
  if (!fieldComposed && step.mode >= 5) {
    gt4_hwd_vkCmdCopyImage(s.cmd, s.display.image, VK_IMAGE_LAYOUT_GENERAL, s.deinterlaceOutput.image,
                           VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    s.full_barrier();
    s.stats.gpuCopyImageBytes += static_cast<std::uint64_t>(step.width) * step.height * 4u;  // P1-C
    s.stats.postCopies++;
  }
  s.post = &s.display;  // the deinterlaced display is written over the display image
  s.stats.deinterlaces++;
  return true;
}

bool Gpu::Fxaa(std::uint32_t width, std::uint32_t height, std::string &error) {
  Impl &s = *impl_;
  if (width == 0 || height == 0 || width > s.display.width || height > s.display.height) {
    error = "CORE2 FXAA outside the display image";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  bool fresh = false;
  if (!s.post_image(s.fxaa, s.display.width, s.display.height, fresh, error)) return false;
  s.flush_draws();
  s.end_render_pass();
  s.full_barrier();  // a host read or a rare pass: ordered at once
  s.cat_mark(Impl::kCatFxaa);  // TASK-182 diagnostics
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (!s.allocate_set(s.fxaaSetLayout, set, error)) return false;
  const VkDescriptorImageInfo images[2] = {{VK_NULL_HANDLE, s.post->view, VK_IMAGE_LAYOUT_GENERAL},
                                           {VK_NULL_HANDLE, s.fxaa.view, VK_IMAGE_LAYOUT_GENERAL}};
  VkWriteDescriptorSet writes[2]{};
  for (std::uint32_t i = 0; i < 2; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[i].pImageInfo = &images[i];
  }
  s.update_sets(2, writes);
  gt4_hwd_vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.fxaaPipeline);
  gt4_hwd_vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.fxaaLayout, 0, 1, &set, 0, nullptr);
  const std::uint32_t push[2] = {width, height};
  gt4_hwd_vkCmdPushConstants(s.cmd, s.fxaaLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
  gt4_hwd_vkCmdDispatch(s.cmd, (width + 7) / 8, (height + 7) / 8, 1);
  s.full_barrier();
  s.stats.fxaaFrames++;
  return true;
}

std::uint64_t Gpu::FxaaImage() const { return reinterpret_cast<std::uint64_t>(impl_->fxaa.image); }

bool Gpu::PresentScale(bool fromFxaa, std::uint32_t width, std::uint32_t height, std::uint32_t outWidth,
                       std::uint32_t outHeight, float sharpness, std::string &error) {
  Impl &s = *impl_;
  Image &source = fromFxaa ? s.fxaa : *s.post;
  if (width == 0 || height == 0 || width > source.width || height > source.height || outWidth == 0 ||
      outHeight == 0 || outWidth > s.limits.maxImageDimension2D || outHeight > s.limits.maxImageDimension2D) {
    error = "CORE2 presentation resampling outside the display image";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  s.flush_draws();
  s.end_render_pass();
  s.full_barrier();  // a rare pass: ordered at once
  s.cat_mark(Impl::kCatPresent);  // TASK-182 diagnostics
  // One pass: `from` (fromWidth x fromHeight) into `to` (toWidth x toHeight, made to that size).
  auto pass = [&](std::uint32_t mode, Image &from, std::uint32_t fromWidth, std::uint32_t fromHeight, Image &to,
                  std::uint32_t toWidth, std::uint32_t toHeight) -> bool {
    bool fresh = false;
    if (!s.post_image(to, toWidth, toHeight, fresh, error)) return false;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!s.allocate_set(s.presentSetLayout, set, error)) return false;
    const VkDescriptorImageInfo images[2] = {{VK_NULL_HANDLE, from.view, VK_IMAGE_LAYOUT_GENERAL},
                                             {VK_NULL_HANDLE, to.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[2]{};
    for (std::uint32_t i = 0; i < 2; ++i) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = set;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      writes[i].pImageInfo = &images[i];
    }
    s.update_sets(2, writes);
    gt4_hwd_vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.presentPipeline);
    gt4_hwd_vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.presentLayout, 0, 1, &set, 0, nullptr);
    const std::uint32_t push[6] = {fromWidth, fromHeight, toWidth, toHeight, mode, FloatBits(sharpness)};
    gt4_hwd_vkCmdPushConstants(s.cmd, s.presentLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
    gt4_hwd_vkCmdDispatch(s.cmd, (toWidth + 7) / 8, (toHeight + 7) / 8, 1);
    s.full_barrier();
    return true;
  };
  constexpr std::uint32_t kHorizontal = 0, kVertical = 1, kEasu = 2, kRcas = 3;
  const bool rcas = sharpness > 0.0f;
  Image *current = &source;
  std::uint32_t cw = width, ch = height;
  // The image a resampling pass writes: the output when nothing follows, otherwise the other scratch image.
  auto next = [&](bool last) -> Image & {
    if (last) return s.presentOut;
    return current == &s.presentA ? s.presentB : s.presentA;
  };
  if (outWidth >= width && outHeight >= height && (outWidth != width || outHeight != height)) {
    Image &to = next(!rcas);
    if (!pass(kEasu, *current, cw, ch, to, outWidth, outHeight)) return false;
    current = &to;
    cw = outWidth;
    ch = outHeight;
  } else {
    if (outWidth != width) {
      Image &to = next(!rcas && outHeight == height);
      if (!pass(kHorizontal, *current, cw, ch, to, outWidth, ch)) return false;
      current = &to;
      cw = outWidth;
    }
    if (outHeight != height) {
      Image &to = next(!rcas);
      if (!pass(kVertical, *current, cw, ch, to, cw, outHeight)) return false;
      current = &to;
      ch = outHeight;
    }
  }
  if (rcas || current == &source) {
    // RCAS (or, at the same size without sharpening, a plain copy through RCAS with no lobe).
    if (!pass(kRcas, *current, cw, ch, s.presentOut, cw, ch)) return false;
  }
  s.stats.presentScales++;
  return true;
}

std::uint64_t Gpu::PresentImage() const { return reinterpret_cast<std::uint64_t>(impl_->presentOut.image); }

bool Gpu::ReadDisplay(std::uint32_t width, std::uint32_t height, std::vector<std::uint32_t> &pixels,
                      std::string &error, bool presentScaled, bool fxaa) {
  Impl &s = *impl_;
  const Image &source = presentScaled ? s.presentOut : fxaa ? s.fxaa : *s.post;
  width = std::min(width, source.width);
  height = std::min(height, source.height);
  const VkDeviceSize rowBytes = static_cast<VkDeviceSize>(width) * 4u;
  if (rowBytes == 0 || height == 0 || rowBytes > s.readbackBytes) {
    error = "CORE2 display read outside the readback buffer";
    return false;
  }
  // IN-435: frames larger than the readback buffer (7680 x 5376 at 12x is 165 MB) are read in bands of rows.
  const std::uint32_t bandRows = static_cast<std::uint32_t>(std::min<VkDeviceSize>(height, s.readbackBytes / rowBytes));
  pixels.resize(static_cast<std::size_t>(width) * height);
  for (std::uint32_t top = 0; top < height; top += bandRows) {
    const std::uint32_t rows = std::min(bandRows, height - top);
    if (!s.begin_recording(error)) return false;
    s.flush_draws();
    s.end_render_pass();
    s.full_barrier();  // a host read or a rare pass: ordered at once
  s.cat_mark(Impl::kCatReadback);  // TASK-182 diagnostics
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {0, static_cast<std::int32_t>(top), 0};
    region.imageExtent = {width, rows, 1};
    gt4_hwd_vkCmdCopyImageToBuffer(s.cmd, source.image, VK_IMAGE_LAYOUT_GENERAL, s.readback.buffer, 1, &region);
    s.full_barrier();
    s.host_read_barrier();  // TASK-180 test4 (P1-D)
    if (!s.submit(true, error)) return false;
    std::memcpy(pixels.data() + static_cast<std::size_t>(top) * width, s.readback.mapped,
                static_cast<std::size_t>(rowBytes * rows));
    s.stats.gpuCopyImageToBufferBytes += static_cast<std::uint64_t>(rowBytes * rows);
    s.stats.hostReadBytes += static_cast<std::uint64_t>(rowBytes * rows);
  }
  return true;
}

bool Gpu::ReadImage(ImageId id, std::uint32_t width, std::uint32_t height, std::vector<std::uint32_t> &words,
                    std::string &error) {
  Impl &s = *impl_;
  const Image *image = s.image_of(id);
  if (!image) {
    error = "CORE2 reads an image that does not exist";
    return false;
  }
  width = std::min(width, image->width);
  height = std::min(height, image->height);
  const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4u;
  if (bytes == 0 || bytes > s.readbackBytes) {
    error = "CORE2 image read outside the readback buffer";
    return false;
  }
  if (!s.begin_recording(error)) return false;
  s.flush_draws();
  s.end_render_pass();
  s.full_barrier();  // a host read or a rare pass: ordered at once
  s.cat_mark(Impl::kCatReadback);  // TASK-182 diagnostics
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {width, height, 1};
  gt4_hwd_vkCmdCopyImageToBuffer(s.cmd, image->image, VK_IMAGE_LAYOUT_GENERAL, s.readback.buffer, 1, &region);
  s.full_barrier();
  s.host_read_barrier();  // TASK-180 test4 (P1-D)
  if (!s.submit(true, error)) return false;
  words.resize(static_cast<std::size_t>(width) * height);
  std::memcpy(words.data(), s.readback.mapped, static_cast<std::size_t>(bytes));
  s.stats.gpuCopyImageToBufferBytes += bytes;
  s.stats.hostReadBytes += bytes;
  return true;
}

void Gpu::ResetRingHighMarks() {
    impl_->stats.ringHighStaging = 0;
    impl_->stats.ringHighVertex = 0;
    impl_->stats.ringHighUniform = 0;
}

bool Gpu::Submit(bool wait, std::string &error) { return impl_->submit(wait, error, "Submit (renderer)"); }
std::uint32_t Gpu::SubmitEvery() const { return impl_->submitEvery; }
std::uint32_t Gpu::SubmitAfterWait() const { return impl_->submitAfterWait; }

}  // namespace hwgs::core2

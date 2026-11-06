// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from docs/hw-renderer-spec (TASK-027 M4, chapters 01.4, 03.2, 03.3, 03.4, 03.5, 03.7, 03.8,
// 15), clean room (decision 0069). CTest entry points of the M4 write-back reference renderer.
//
// Usage: hw_writeback_tests <test name>. GPU tests need a Vulkan 1.2 device with graphics and compute
// queues; without one they exit 77 (CTest reports Skipped, never a pass).
// Equivalence tests compare the GPU result with the CPU reference (cpuref::DrawBatch) pixel by pixel.
// Edges are placed on pixel boundaries where the two rasterisers are specified to agree; the diagonal
// and Gouraud cases are reported, not asserted (IN-500).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <process.h>
#include "png_encode.hpp"
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "../../core2/core2_gpu.hpp"
#include "../../core2/core2_renderer.hpp"
#include "../../cpuref/cpuref_draw.hpp"
#include "../../cpuref/cpuref_memory.hpp"
#include "../../memory/hw_memory.hpp"
#include "../../clut/hw_clut.hpp"
#include "../../frontend/hw_gs_registers.hpp"
#include "../../pixel/hw_gs_pixel.hpp"
#include "../../frontend/hw_gs_types.hpp"
#include "../../hw_gs_vulkan_loader.hpp"
#include "../hw_gs_vulkan_device_table.hpp"
#include "../hw_gs_present_scheduler.hpp"
#include "../hw_gs_writeback.hpp"
#include "../hw_writeback_replay_target.hpp"
#include "../../cpuref/cpuref_renderer.hpp"
#include "../../cpuref/cpuref_replay.hpp"

namespace {

using namespace hwgs;

// ---- Headless device -----------------------------------------------------------------------------

struct Headless {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    bool wantInterlock = true;
    bool interlock = false;
    bool independentBlend = false;
    bool dualSourceBlend = false;
    gt4::hw::DeviceProfile profile;  // TASK-179: the capability probe, with the features enabled below

    bool Create(std::string& error) {
        if (!gt4::hw::vulkan_loader::load_loader(error)) return false;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "hw_writeback_tests";
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &app;
        if (gt4_hw_vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS) {
            error = "vkCreateInstance failed";
            return false;
        }
        std::vector<std::string> missing;
        if (!gt4::hw::vulkan_loader::load_headless_entry_points(instance, missing)) {
            error = "instance entry points missing:";
            for (const std::string& name : missing) error += " " + name;
            return false;
        }
        uint32_t count = 0;
        gt4_hw_vkEnumeratePhysicalDevices(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        gt4_hw_vkEnumeratePhysicalDevices(instance, &count, devices.data());
        for (VkPhysicalDevice candidate : devices) {
            VkPhysicalDeviceProperties props{};
            gt4_hw_vkGetPhysicalDeviceProperties(candidate, &props);
            if (props.apiVersion < VK_API_VERSION_1_2) continue;
            uint32_t families = 0;
            gt4_hw_vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
            std::vector<VkQueueFamilyProperties> props_q(families);
            gt4_hw_vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, props_q.data());
            for (uint32_t f = 0; f < families; ++f) {
                const VkQueueFlags wanted = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
                if ((props_q[f].queueFlags & wanted) != wanted) continue;
                physical = candidate;
                family = f;
                break;
            }
            if (physical) break;
        }
        if (!physical) {
            error = "no Vulkan 1.2 device with graphics and compute";
            return false;
        }
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex = family;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        // IN-345: enable pixel interlock when the device has it (the batch-per-draw path), unless the
        // caller asked for the per-primitive path (A/B: --no-interlock / GT4_HW_TEST_NO_INTERLOCK=1).
        interlock = wantInterlock && gt4::hw::vulkan_loader::pixel_interlock_supported(physical);
        VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlockFeatures{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
        interlockFeatures.fragmentShaderPixelInterlock = VK_TRUE;
        const char* interlockExtension = VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME;
        VkDeviceCreateInfo dev{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dev.queueCreateInfoCount = 1;
        dev.pQueueCreateInfos = &queue;
        VkPhysicalDeviceFeatures2 supportedFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        VkPhysicalDeviceFeatures enabledFeatures{};
        gt4_hw_vkGetPhysicalDeviceFeatures2(physical, &supportedFeatures);
        enabledFeatures.independentBlend = supportedFeatures.features.independentBlend;
        enabledFeatures.dualSrcBlend = supportedFeatures.features.dualSrcBlend;
        // TASK-179: largePoints and shaderStorageImageArrayDynamicIndexing where the device has them, as the game's device.
        profile = gt4::hw::vulkan_loader::probe_device_profile(instance, physical);
        gt4::hw::vulkan_loader::enable_profile_features(profile, enabledFeatures);
        dualSourceBlend = supportedFeatures.features.dualSrcBlend == VK_TRUE;
        independentBlend = supportedFeatures.features.independentBlend == VK_TRUE;
        dev.pEnabledFeatures = &enabledFeatures;
        if (interlock) {
            dev.pNext = &interlockFeatures;
            dev.enabledExtensionCount = 1;
            dev.ppEnabledExtensionNames = &interlockExtension;
        }
        if (gt4_hw_vkCreateDevice(physical, &dev, nullptr, &device) != VK_SUCCESS) {
            error = "vkCreateDevice failed";
            return false;
        }
        gt4_hw_vkGetDeviceQueue(device, family, 0, &queue_handle());
        return gt4::hw::vulkan_device::load_device_entry_points(instance, missing);
    }

    VkQueue &queue_handle() { return queue; }
};

// ---- Batch construction --------------------------------------------------------------------------

struct Setup {
    uint32_t fbp = 0;     // FBP in pages
    uint32_t fbw = 10;    // FBW in 64-pixel units
    uint32_t fpsm = 0;    // CT32
    uint32_t fbmsk = 0;
    uint32_t zbp = 70;    // ZBP in pages
    uint32_t zpsm = 0;    // Z32 low bits (0x30 implied)
    bool zmsk = true;
    bool zte = false;
    uint32_t ztst = 1;    // ALWAYS
    bool ate = false;
    uint32_t atst = 1;    // ALWAYS
    uint32_t aref = 0x80;
    uint32_t afail = 0;   // KEEP
    int scax0 = 0, scax1 = 639, scay0 = 0, scay1 = 447;
    bool tme = false;
    bool fst = false;
    bool iip = true;      // Gouraud
    bool fge = false;
    uint32_t fogcol = 0;
    uint32_t tbp = 40;    // texture base in pages
    uint32_t tbw = 4;
    uint32_t tpsm = 0;
    uint32_t tw = 3, th = 3;
    uint32_t tfx = 0;
    bool tcc = false;
    bool mmag = false;
    uint32_t wms = 0, wmt = 0;
    uint32_t minu = 0, maxu = 0, minv = 0, maxv = 0;
    bool abe = false;        // PRIM.ABE: run the blend unit
    uint64_t alpha_reg = 0;  // ALPHA register (0x42), see pixel::encode_alpha_register
    bool pabe = false;       // skip the blend when source alpha MSB is 0
    bool colclamp = false;   // COLCLAMP: 0 wraps the low 8 bits
    bool fba = false;        // force the alpha byte to 0x80 on write
    bool dthe = false;       // dithering (16-bit frames)
    uint64_t dimx = 0;       // DIMX, the dither matrix
    bool date = false;       // DATE (destination alpha test)
    bool datm = false;       // DATE expected MSB
    PrimType type = PrimType::Triangle;
};

uint64_t Pack(uint64_t value, unsigned lo) { return value << lo; }

Batch MakeBatch(const Setup& s, const std::vector<Vertex>& vertices, const std::vector<PrimType>& prims) {
    Batch batch;
    batch.state.ctx.frame = Pack(s.fbp, 0) | Pack(s.fbw, 16) | Pack(s.fpsm, 24) | Pack(s.fbmsk, 32);
    batch.state.ctx.zbuf = Pack(s.zbp, 0) | Pack(s.zpsm, 24) | Pack(s.zmsk ? 1 : 0, 32);
    batch.state.ctx.test = Pack(s.ate ? 1 : 0, 0) | Pack(s.atst, 1) | Pack(s.aref, 4) | Pack(s.afail, 12) |
                           Pack(s.date ? 1 : 0, 14) | Pack(s.datm ? 1 : 0, 15) | Pack(s.zte ? 1 : 0, 16) |
                           Pack(s.ztst, 17);
    batch.state.ctx.alpha = s.alpha_reg;
    batch.state.ctx.fba = Pack(s.fba ? 1 : 0, 0);
    batch.state.env.pabe = s.pabe ? 1 : 0;
    batch.state.env.colclamp = s.colclamp ? 1 : 0;
    batch.state.env.dthe = s.dthe ? 1 : 0;
    batch.state.env.dimx = s.dimx;
    batch.state.ctx.scissor = Pack(static_cast<uint64_t>(s.scax0), 0) | Pack(static_cast<uint64_t>(s.scax1), 16) |
                              Pack(static_cast<uint64_t>(s.scay0), 32) | Pack(static_cast<uint64_t>(s.scay1), 48);
    batch.state.ctx.tex0 = Pack(s.tbp, 0) | Pack(s.tbw, 14) | Pack(s.tpsm, 20) | Pack(s.tw, 26) | Pack(s.th, 30) |
                           Pack(s.tcc ? 1 : 0, 34) | Pack(s.tfx, 35);
    batch.state.ctx.tex1 = Pack(s.mmag ? 1 : 0, 5);
    batch.state.ctx.clamp = Pack(s.wms, 0) | Pack(s.wmt, 2) | Pack(s.minu, 4) | Pack(s.maxu, 14) | Pack(s.minv, 24) |
                            Pack(s.maxv, 34);
    batch.state.env.fogcol = s.fogcol;
    uint64_t prim = static_cast<uint64_t>(s.type == PrimType::Triangle ? 3 : static_cast<uint8_t>(s.type));
    prim = prim | (s.iip ? 1ull << primbit::kIip : 0) | (s.tme ? 1ull << primbit::kTme : 0) |
           (s.fge ? 1ull << primbit::kFge : 0) | (s.abe ? 1ull << primbit::kAbe : 0) |
           (s.fst ? 1ull << primbit::kFst : 0);
    batch.state.effectivePrim = prim;
    batch.cls = ClassOfType(static_cast<uint32_t>(prim & 7u));
    batch.vertices = vertices;
    uint32_t first = 0;
    for (PrimType type : prims) {
        const uint32_t count = type == PrimType::Point ? 1 : (type == PrimType::Line || type == PrimType::LineStrip) ? 2
                             : type == PrimType::Sprite ? 2 : 3;
        PrimRecord record;
        record.type = type;
        record.first = first;
        record.count = count;
        int32_t x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
        for (uint32_t i = 0; i < count; ++i) {
            const Vertex& v = vertices[first + i];
            x0 = std::min(x0, v.x4 >> 4);
            y0 = std::min(y0, v.y4 >> 4);
            x1 = std::max(x1, v.x4 >> 4);
            y1 = std::max(y1, v.y4 >> 4);
        }
        record.rect = PixelRect{x0, y0, x1, y1};
        batch.prims.push_back(record);
        first += count;
    }
    return batch;
}

Vertex MakeVertex(int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a, float s = 0.0f, float t = 0.0f,
                  uint32_t u = 0, uint32_t v = 0, uint32_t z = 0, uint8_t fog = 255) {
    Vertex out;
    out.x4 = x * 16;
    out.y4 = y * 16;
    out.r = r;
    out.g = g;
    out.b = b;
    out.a = a;
    out.s = s;
    out.t = t;
    out.q = 1.0f;
    out.u = u;
    out.v = v;
    out.z = z;
    out.fog = fog;
    return out;
}

// ---- Equivalence harness -------------------------------------------------------------------------

// Deterministic fill of local memory: a texture pattern and a frame pattern in the given views.
void FillPattern(memory::LocalMemory& local, const Setup& s, uint32_t seed) {
    uint32_t state = seed * 2654435761u + 1u;
    auto next = [&] {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    const auto tpsm = memory::decodePsm(s.tpsm);
    if (tpsm) {
        const memory::BufferView view{memory::blockOfPage(s.tbp), s.tbw, *tpsm};
        for (uint32_t y = 0; y < (1u << s.th); ++y) {
            for (uint32_t x = 0; x < (1u << s.tw); ++x) local.writePixel(view, x, y, next() | 0x40000000u);
        }
    }
    const auto fpsm = memory::decodePsm(s.fpsm);
    if (fpsm) {
        const memory::BufferView view{memory::blockOfPage(s.fbp), s.fbw, *fpsm};
        for (uint32_t y = 0; y < 448; ++y) {
            for (uint32_t x = 0; x < 640; ++x) local.writePixel(view, x, y, next());
        }
    }
    const auto zpsm = memory::decodePsm(0x30u | s.zpsm);
    if (zpsm) {
        const memory::BufferView view{memory::blockOfPage(s.zbp), s.fbw, *zpsm};
        for (uint32_t y = 0; y < 448; ++y) {
            for (uint32_t x = 0; x < 640; ++x) local.writePixel(view, x, y, next() & 0x00FFFFFFu);
        }
    }
}

struct CaseResult {
    uint64_t compared = 0;
    uint64_t mismatches = 0;
    uint64_t firstX = 0, firstY = 0;
    bool gpuError = false;
    std::string error;
};

// Runs the batch through the CPU reference and through the GPU path, then compares frame and depth words
// over the batch rectangle. Both start from identical memory.
CaseResult RunCase(gpu::GpuDrawer& drawer, const Setup& s, const Batch& batch, uint32_t seed) {
    CaseResult result;
    memory::LocalMemory cpuMemory;
    memory::LocalMemory gpuMemory;
    FillPattern(cpuMemory, s, seed);
    FillPattern(gpuMemory, s, seed);
    CounterSet counterSet;
    clut::ClutUnit clut(counterSet);

    cpuref::DrawStats stats;
    cpuref::DrawBatch(batch, cpuMemory, clut, stats);

    writeback::BatchTranslation translation;
    writeback::Counters counters;
    writeback::TranslateBatch(batch, gpuMemory, clut, translation, counters);
    if (translation.skippedTexture) {
        result.error = "texture skipped";
        return result;
    }
    if (translation.draws.empty()) {
        // Nothing drawn: the reference must not have drawn either.
        return result;
    }
    if (!writeback::ExecuteBatch(drawer, translation, gpuMemory, counters, result.error)) {
        result.gpuError = true;
        return result;
    }
    const memory::BufferView frameCpu{memory::blockOfPage(s.fbp), s.fbw, *memory::decodePsm(s.fpsm)};
    const memory::BufferView zCpu{memory::blockOfPage(s.zbp), s.fbw, *memory::decodePsm(0x30u | s.zpsm)};
    const PixelRect& r = translation.rect;
    for (int32_t y = r.y0; y <= r.y1; ++y) {
        for (int32_t x = r.x0; x <= r.x1; ++x) {
            const uint32_t fc = cpuMemory.readPixel(frameCpu, x, y);
            const uint32_t fg = gpuMemory.readPixel(frameCpu, x, y);
            const uint32_t zc = cpuMemory.readPixel(zCpu, x, y);
            const uint32_t zg = gpuMemory.readPixel(zCpu, x, y);
            result.compared++;
            if (fc != fg || zc != zg) {
                if (result.mismatches == 0) {
                    result.firstX = static_cast<uint64_t>(x);
                    result.firstY = static_cast<uint64_t>(y);
                }
                // Bounded value report: the first few differences name the stage that diverged.
                if (result.mismatches < 4) {
                    std::printf("  mismatch at %d,%d: frame cpu=%08X gpu=%08X | depth cpu=%08X gpu=%08X\n", x, y, fc,
                                fg, zc, zg);
                }
                result.mismatches++;
            }
        }
    }
    return result;
}

int Report(const char* name, const CaseResult& r) {
    std::printf("%s: compared %llu pixels, mismatches %llu", name, static_cast<unsigned long long>(r.compared),
                static_cast<unsigned long long>(r.mismatches));
    if (r.mismatches) {
        std::printf(" (first at %llu,%llu)", static_cast<unsigned long long>(r.firstX),
                    static_cast<unsigned long long>(r.firstY));
    }
    std::printf("\n");
    if (r.gpuError) std::printf("%s: GPU error: %s\n", name, r.error.c_str());
    return (r.gpuError || r.mismatches != 0) ? 1 : 0;
}

// Quad on pixel boundaries: two triangles over [x0, x1) x [y0, y1) with one flat colour.
std::vector<Vertex> FlatQuad(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t r, uint8_t g, uint8_t b,
                             uint8_t a, float s0 = 0, float t0 = 0, float s1 = 0, float t1 = 0) {
    return {MakeVertex(x0, y0, r, g, b, a, s0, t0), MakeVertex(x1, y0, r, g, b, a, s1, t0),
            MakeVertex(x0, y1, r, g, b, a, s0, t1), MakeVertex(x1, y0, r, g, b, a, s1, t0),
            MakeVertex(x0, y1, r, g, b, a, s0, t1), MakeVertex(x1, y1, r, g, b, a, s1, t1)};
}

// ---- CPU translation tests (no GPU) --------------------------------------------------------------

int TestFlatProvoking() {
    Setup s;
    s.iip = false;  // flat: the last vertex supplies the colour (chapter 03.2)
    const std::vector<Vertex> v = {MakeVertex(0, 0, 10, 20, 30, 40), MakeVertex(16, 0, 50, 60, 70, 80),
                                   MakeVertex(0, 16, 90, 100, 110, 120)};
    Batch batch = MakeBatch(s, v, {PrimType::Triangle});
    writeback::BatchTranslation t;
    writeback::Counters counters;
    memory::LocalMemory local;
    CounterSet counterSet;
    clut::ClutUnit clut(counterSet);
    writeback::TranslateBatch(batch, local, clut, t, counters);
    if (t.vertices.size() != 3) return 1;
    for (const gpu::GpuVertex& g : t.vertices) {
        if (g.rgba[0] != 90 || g.rgba[1] != 100 || g.rgba[2] != 110 || g.rgba[3] != 120) {
            std::printf("hw_flat_provoking: triangle vertex not de-indexed to the last colour\n");
            return 1;
        }
    }
    // A line in flat mode takes the second vertex's colour.
    Setup line = s;
    line.type = PrimType::Line;
    Batch lineBatch = MakeBatch(line, {MakeVertex(0, 0, 1, 2, 3, 4), MakeVertex(8, 8, 5, 6, 7, 8)}, {PrimType::Line});
    writeback::BatchTranslation tl;
    writeback::TranslateBatch(lineBatch, local, clut, tl, counters);
    for (const gpu::GpuVertex& g : tl.vertices) {
        if (g.rgba[0] != 5 || g.rgba[3] != 8) {
            std::printf("hw_flat_provoking: line vertex not de-indexed\n");
            return 1;
        }
    }
    // Gouraud keeps every vertex colour.
    Setup smooth = s;
    smooth.iip = true;
    Batch smoothBatch = MakeBatch(smooth, v, {PrimType::Triangle});
    writeback::BatchTranslation ts;
    writeback::TranslateBatch(smoothBatch, local, clut, ts, counters);
    if (ts.vertices[0].rgba[0] != 10 || ts.vertices[2].rgba[0] != 90) {
        std::printf("hw_flat_provoking: smooth colours changed\n");
        return 1;
    }
    std::printf("hw_flat_provoking: triangle and line de-indexed to the last vertex, Gouraud unchanged\n");
    return 0;
}

int TestSpriteCorners() {
    Setup s;
    s.type = PrimType::Sprite;
    // First vertex at (2, 3) with S/T/U/V (4, 6)/(40, 60); second at (10, 9) with S/T (8, 12)/(80, 120).
    std::vector<Vertex> v = {MakeVertex(2, 3, 1, 1, 1, 1, 4, 6, 40, 60), MakeVertex(10, 9, 200, 201, 202, 203, 8, 12, 80, 120)};
    Batch batch = MakeBatch(s, v, {PrimType::Sprite});
    memory::LocalMemory local;
    CounterSet counterSet;
    clut::ClutUnit clut(counterSet);
    writeback::BatchTranslation t;
    writeback::Counters counters;
    writeback::TranslateBatch(batch, local, clut, t, counters);
    if (t.vertices.size() != 6 || counters.spritesExpanded != 1) {
        std::printf("hw_sprite_corners: expected 6 vertices, got %zu\n", t.vertices.size());
        return 1;
    }
    // Corner order of the two triangles: 0, 1, 2, 1, 2, 3 (chapter 03.2).
    const int32_t expectX[4] = {2, 10, 2, 10};
    const int32_t expectY[4] = {3, 3, 9, 9};
    const int order[6] = {0, 1, 2, 1, 2, 3};
    for (int i = 0; i < 6; ++i) {
        const gpu::GpuVertex& g = t.vertices[i];
        const int c = order[i];
        if (static_cast<int32_t>(g.x) != expectX[c] * 16 || static_cast<int32_t>(g.y) != expectY[c] * 16) {
            std::printf("hw_sprite_corners: corner %d at the wrong position\n", c);
            return 1;
        }
        if (g.rgba[0] != 200 || g.rgba[3] != 203) {
            std::printf("hw_sprite_corners: colour not taken from the second vertex\n");
            return 1;
        }
    }
    // S comes from the first vertex at X of the first corner and the second at the second corner.
    if (t.vertices[0].s != 4.0f || t.vertices[1].s != 8.0f || t.vertices[2].t != 12.0f) {
        std::printf("hw_sprite_corners: texture coordinates not per corner\n");
        return 1;
    }
    std::printf("hw_sprite_corners: six vertices, corner rule and second-vertex colour as chapter 03.2\n");
    return 0;
}

// ---- GPU tests -----------------------------------------------------------------------------------

int TestCoverageVectors(gpu::GpuDrawer& drawer) {
    // Chapter 03.3 vectors at 1x (sprite spans, pixel n covered when its centre is inside [min, max)):
    // 0.0..2.0 covers 0 and 1; 0.5..1.5 covers 1; 0.0625..1.0 covers nothing.
    Setup s;
    s.type = PrimType::Sprite;
    s.iip = false;
    const std::vector<Vertex> v = {
        MakeVertex(0, 0, 255, 0, 0, 128), MakeVertex(2, 1, 255, 0, 0, 128),   // row 0: x 0..2
        MakeVertex(0, 2, 0, 255, 0, 128), MakeVertex(1, 3, 0, 255, 0, 128),   // row 2: x 0..1
    };
    // Sprites with sub-pixel edges (12.4 units): 0.5..1.5 on row 4, 0.0625..1.0 on row 6.
    std::vector<Vertex> w = {MakeVertex(0, 4, 0, 0, 255, 128), MakeVertex(1, 5, 0, 0, 255, 128)};
    w[0].x4 = 8;
    w[1].x4 = 24;
    std::vector<Vertex> z = {MakeVertex(0, 6, 9, 9, 9, 128), MakeVertex(1, 7, 9, 9, 9, 128)};
    z[0].x4 = 1;
    z[1].x4 = 16;
    std::vector<Vertex> all;
    all.insert(all.end(), v.begin(), v.end());
    all.insert(all.end(), w.begin(), w.end());
    all.insert(all.end(), z.begin(), z.end());
    Batch batch = MakeBatch(s, all, {PrimType::Sprite, PrimType::Sprite, PrimType::Sprite});
    CaseResult result = RunCase(drawer, s, batch, 7);
    // Expected coverage from the vectors, checked against the GPU output directly.
    memory::LocalMemory gpuMemory;
    FillPattern(gpuMemory, s, 7);
    writeback::BatchTranslation t;
    writeback::Counters counters;
    CounterSet counterSet;
    clut::ClutUnit clut(counterSet);
    writeback::TranslateBatch(batch, gpuMemory, clut, t, counters);
    std::string error;
    if (!writeback::ExecuteBatch(drawer, t, gpuMemory, counters, error)) {
        std::printf("hw_gpu_coverage_vectors: GPU error %s\n", error.c_str());
        return 1;
    }
    const memory::BufferView frame{memory::blockOfPage(s.fbp), s.fbw, memory::Psm::Ct32};
    auto covered = [&](uint32_t x, uint32_t y) {
        return (gpuMemory.readPixel(frame, x, y) & 0xFFu) == 255u;  // red channel from the first sprite
    };
    auto coveredBlue = [&](uint32_t x, uint32_t y) { return ((gpuMemory.readPixel(frame, x, y) >> 16) & 0xFFu) == 255u; };
    bool ok = covered(0, 0) && covered(1, 0) && !covered(2, 0);
    ok = ok && !covered(0, 1) && !covered(1, 1);
    ok = ok && coveredBlue(1, 4) && !coveredBlue(0, 4) && !coveredBlue(2, 4);
    ok = ok && !coveredBlue(0, 6) && !coveredBlue(1, 6);
    if (!ok) {
        std::printf("hw_gpu_coverage_vectors: coverage differs from the chapter 03.3 vectors\n");
        return 1;
    }
    return Report("hw_gpu_coverage_vectors", result);
}

int TestEquivalenceFlat(gpu::GpuDrawer& drawer) {
    Setup s;
    s.iip = false;
    Batch batch = MakeBatch(s, FlatQuad(0, 0, 32, 32, 12, 200, 40, 255), {PrimType::Triangle, PrimType::Triangle});
    return Report("hw_gpu_equivalence_flat", RunCase(drawer, s, batch, 1));
}

int TestEquivalenceTexture(gpu::GpuDrawer& drawer) {
    // Textured quad, STQ coordinates on the pixel grid (decal, modulate, nearest, repeat).
    int failures = 0;
    for (uint32_t tfx = 0; tfx < 2; ++tfx) {
        Setup s;
        s.iip = false;
        s.tme = true;
        s.tpsm = 0;  // CT32 texture
        s.tw = 3;
        s.th = 3;
        s.tfx = tfx;
        s.fst = true;  // FST: UV in 1/16 texel units (chapter 03.5)
        std::vector<Vertex> q = {MakeVertex(0, 0, 128, 128, 128, 128, 0, 0, 0, 0),
                                 MakeVertex(40, 0, 128, 128, 128, 128, 0, 0, 40 * 16, 0),
                                 MakeVertex(0, 40, 128, 128, 128, 128, 0, 0, 0, 40 * 16),
                                 MakeVertex(40, 0, 128, 128, 128, 128, 0, 0, 40 * 16, 0),
                                 MakeVertex(0, 40, 128, 128, 128, 128, 0, 0, 0, 40 * 16),
                                 MakeVertex(40, 40, 128, 128, 128, 128, 0, 0, 40 * 16, 40 * 16)};
        Batch batch = MakeBatch(s, q, {PrimType::Triangle, PrimType::Triangle});
        failures += Report(tfx == 0 ? "hw_gpu_equivalence_texture_modulate" : "hw_gpu_equivalence_texture_decal",
                           RunCase(drawer, s, batch, 2 + tfx));
    }
    return failures ? 1 : 0;
}

int TestEquivalenceWrapRegion(gpu::GpuDrawer& drawer) {
    // STQ sprite over a 8x8 texture with region repeat and region clamp (chapter 03.5 table).
    int failures = 0;
    for (uint32_t mode = 2; mode <= 3; ++mode) {
        Setup s;
        s.tme = true;
        s.tw = 3;
        s.th = 3;
        s.wms = mode;
        s.wmt = mode;
        s.minu = mode == 3 ? 3 : 1;   // region repeat mask / region clamp minimum
        s.maxu = mode == 3 ? 1 : 6;   // region repeat fixed bits / region clamp maximum
        s.minv = s.minu;
        s.maxv = s.maxu;
        s.type = PrimType::Sprite;
        s.iip = false;
        s.fst = true;
        std::vector<Vertex> q = {MakeVertex(0, 0, 0, 0, 0, 0, 0, 0, 0, 0), MakeVertex(40, 40, 0, 0, 0, 0, 0, 0, 40 * 16, 40 * 16)};
        Batch batch = MakeBatch(s, q, {PrimType::Sprite});
        failures += Report(mode == 2 ? "hw_gpu_equivalence_region_clamp" : "hw_gpu_equivalence_region_repeat",
                           RunCase(drawer, s, batch, 4 + mode));
    }
    return failures ? 1 : 0;
}

int TestEquivalenceBilinear(gpu::GpuDrawer& drawer) {
    Setup s;
    s.tme = true;
    s.mmag = true;
    s.fst = true;
    s.iip = false;
    s.tw = 4;
    s.th = 4;
    std::vector<Vertex> q = {MakeVertex(0, 0, 0, 0, 0, 0, 0, 0, 8, 8), MakeVertex(32, 32, 0, 0, 0, 0, 0, 0, 8 + 32 * 16, 8 + 32 * 16)};
    s.type = PrimType::Sprite;
    Batch batch = MakeBatch(s, q, {PrimType::Sprite});
    return Report("hw_gpu_equivalence_bilinear", RunCase(drawer, s, batch, 9));
}

int TestEquivalenceTfxFog(gpu::GpuDrawer& drawer) {
    Setup s;
    s.iip = false;
    s.tme = false;
    s.tw = 3;
    s.th = 3;
    s.fst = true;
    s.tfx = 1;  // decal
    s.fge = true;
    s.fogcol = 0x402010;
    std::vector<Vertex> q = FlatQuad(0, 0, 40, 40, 200, 100, 50, 128, 0, 0, 40 * 16, 40 * 16);
    for (Vertex& v : q) v.fog = 90;
    Batch batch = MakeBatch(s, q, {PrimType::Triangle, PrimType::Triangle});
    return Report("hw_gpu_equivalence_tfx_fog", RunCase(drawer, s, batch, 11));
}

int TestEquivalenceDepthAlpha(gpu::GpuDrawer& drawer) {
    // Depth GEQUAL with a stored Z, then ALWAYS with Z writes, then the alpha test (KEEP).
    Setup s;
    s.iip = false;
    s.zte = true;
    s.ztst = 2;  // GEQUAL
    s.zmsk = false;
    Batch batch = MakeBatch(s, FlatQuad(0, 0, 32, 32, 90, 90, 90, 90), {PrimType::Triangle, PrimType::Triangle});
    int failures = Report("hw_gpu_equivalence_depth", RunCase(drawer, s, batch, 13));
    Setup a;
    a.iip = false;
    a.ate = true;
    a.atst = 6;   // GREATER
    a.aref = 0x20;
    a.afail = 0;  // KEEP
    Batch alpha = MakeBatch(a, FlatQuad(16, 16, 48, 48, 1, 2, 3, 0x40), {PrimType::Triangle, PrimType::Triangle});
    failures += Report("hw_gpu_equivalence_alpha_keep", RunCase(drawer, a, alpha, 14));
    return failures ? 1 : 0;
}

int TestEquivalenceScissor(gpu::GpuDrawer& drawer) {
    Setup s;
    s.iip = false;
    s.scax0 = 8;
    s.scax1 = 20;
    s.scay0 = 4;
    s.scay1 = 12;
    Batch batch = MakeBatch(s, FlatQuad(0, 0, 40, 40, 77, 66, 55, 255), {PrimType::Triangle, PrimType::Triangle});
    return Report("hw_gpu_equivalence_scissor", RunCase(drawer, s, batch, 15));
}

// ---- M5: blend unit, alpha test, DATE, FBA/FBMASK/DIMX through the raster path ------------------
// Every case compares the GPU result with the CPU reference over the whole batch rectangle, like the
// M4 equivalence cases: the CPU side is the bit-exact pixel library of chapter 08.

uint64_t AlphaRegister(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint8_t fix) {
    return pixel::encode_alpha_register(static_cast<pixel::BlendSource>(a), static_cast<pixel::BlendSource>(b),
                                        static_cast<pixel::BlendFactor>(c), static_cast<pixel::BlendSource>(d), fix);
}

// 81 blend modes of chapter 08.2 (A, B, C, D over Cs/Cd/Zero and As/Ad/FIX), with COLCLAMP both ways.
// The destination is the deterministic frame pattern, so every mode blends against real destination
// colours and alphas.
int TestRasterBlendModes(gpu::GpuDrawer& drawer) {
    uint64_t compared = 0;
    uint64_t mismatches = 0;
    for (uint32_t clamp = 0; clamp < 2; ++clamp) {
        for (uint32_t a = 0; a < 3; ++a) {
            for (uint32_t b = 0; b < 3; ++b) {
                for (uint32_t c = 0; c < 3; ++c) {
                    for (uint32_t d = 0; d < 3; ++d) {
                        Setup s;
                        s.iip = false;
                        s.abe = true;
                        s.colclamp = clamp != 0;
                        s.alpha_reg = AlphaRegister(a, b, c, d, 0x40);
                        Batch batch = MakeBatch(s, FlatQuad(0, 0, 8, 8, 120, 60, 30, 0x90),
                                                {PrimType::Triangle, PrimType::Triangle});
                        CaseResult r = RunCase(drawer, s, batch, 21 + a * 27 + b * 9 + c * 3 + d);
                        if (r.gpuError) {
                            std::printf("hw_gpu_raster_blend: GPU error: %s\n", r.error.c_str());
                            return 1;
                        }
                        compared += r.compared;
                        mismatches += r.mismatches;
                    }
                }
            }
        }
    }
    std::printf("hw_gpu_raster_blend: compared %llu pixels over 162 modes, mismatches %llu\n",
                static_cast<unsigned long long>(compared), static_cast<unsigned long long>(mismatches));
    return mismatches == 0 ? 0 : 1;
}

// PABE: with PABE = 1 the blend is skipped while the source alpha MSB is 0 (08.2).
int TestRasterPabe(gpu::GpuDrawer& drawer) {
    int failures = 0;
    for (uint32_t alpha : {0x40u, 0xC0u}) {
        Setup s;
        s.iip = false;
        s.abe = true;
        s.pabe = true;
        s.alpha_reg = AlphaRegister(0, 1, 0, 1, 0x00);  // Cs*Ad + Cd*(1-Ad)
        Batch batch = MakeBatch(s, FlatQuad(0, 0, 8, 8, 200, 40, 90, static_cast<uint8_t>(alpha)),
                                {PrimType::Triangle, PrimType::Triangle});
        failures += Report("hw_gpu_raster_pabe", RunCase(drawer, s, batch, 31 + alpha));
    }
    return failures ? 1 : 0;
}

// Alpha test: the eight comparisons of 08.6 and the four AFAIL actions, with alphas on both sides of
// the reference so both the pass and the fail path run.
int TestRasterAlphaTest(gpu::GpuDrawer& drawer) {
    uint64_t compared = 0;
    uint64_t mismatches = 0;
    for (uint32_t atst = 0; atst < 8; ++atst) {
        for (uint32_t afail = 0; afail < 4; ++afail) {
            Setup s;
            s.iip = false;
            s.ate = true;
            s.atst = atst;
            s.afail = afail;
            s.aref = 0x50;
            s.zte = true;
            s.ztst = 1;
            std::vector<Vertex> quad = FlatQuad(0, 0, 8, 8, 70, 80, 90, 0x30);
            std::vector<Vertex> other = FlatQuad(8, 0, 16, 8, 20, 30, 40, 0x70);
            quad.insert(quad.end(), other.begin(), other.end());
            Batch batch = MakeBatch(s, quad, {PrimType::Triangle, PrimType::Triangle, PrimType::Triangle,
                                              PrimType::Triangle});
            CaseResult r = RunCase(drawer, s, batch, 41 + atst * 4 + afail);
            if (r.gpuError) {
                std::printf("hw_gpu_raster_atest: GPU error: %s\n", r.error.c_str());
                return 1;
            }
            compared += r.compared;
            mismatches += r.mismatches;
        }
    }
    std::printf("hw_gpu_raster_atest: compared %llu pixels over 32 combinations, mismatches %llu\n",
                static_cast<unsigned long long>(compared), static_cast<unsigned long long>(mismatches));
    return mismatches == 0 ? 0 : 1;
}

// DATE (08.7): the destination alpha MSB decides whether a pixel is written. A first quad writes,
// a second one with the opposite alpha tests against the pixels the first one produced.
int TestRasterDate(gpu::GpuDrawer& drawer) {
    uint64_t compared = 0;
    uint64_t mismatches = 0;
    for (uint32_t datm = 0; datm < 2; ++datm) {
        Setup s;
        s.iip = false;
        s.date = true;
        s.datm = datm != 0;
        std::vector<Vertex> quad = FlatQuad(0, 0, 8, 8, 90, 40, 10, 0x80);
        std::vector<Vertex> inner = FlatQuad(2, 2, 6, 6, 10, 200, 30, 0x40);
        quad.insert(quad.end(), inner.begin(), inner.end());
        Batch batch = MakeBatch(s, quad, {PrimType::Triangle, PrimType::Triangle, PrimType::Triangle,
                                          PrimType::Triangle});
        CaseResult r = RunCase(drawer, s, batch, 51 + datm);
        if (r.gpuError) {
            std::printf("hw_gpu_raster_date: GPU error: %s\n", r.error.c_str());
            return 1;
        }
        compared += r.compared;
        mismatches += r.mismatches;
    }
    std::printf("hw_gpu_raster_date: compared %llu pixels, mismatches %llu\n",
                static_cast<unsigned long long>(compared), static_cast<unsigned long long>(mismatches));
    return mismatches == 0 ? 0 : 1;
}

// FBA (alpha forced to 0x80), FBMSK on a CT24 target and DIMX dithering on a CT16 target (05.6, 08.8).
int TestRasterFrameWrites(gpu::GpuDrawer& drawer) {
    int failures = 0;
    {
        Setup s;
        s.iip = false;
        s.fba = true;
        Batch batch = MakeBatch(s, FlatQuad(0, 0, 8, 8, 30, 60, 90, 0x12),
                                {PrimType::Triangle, PrimType::Triangle});
        failures += Report("hw_gpu_raster_fba", RunCase(drawer, s, batch, 61));
    }
    {
        Setup s;
        s.iip = false;
        s.fpsm = 1;  // CT24
        s.fbmsk = 0x00FFFFFF;
        Batch batch = MakeBatch(s, FlatQuad(0, 0, 8, 8, 240, 120, 60, 0xFF),
                                {PrimType::Triangle, PrimType::Triangle});
        failures += Report("hw_gpu_raster_fbmsk", RunCase(drawer, s, batch, 62));
    }
    {
        Setup s;
        s.iip = false;
        s.fpsm = 2;  // CT16
        s.dthe = true;
        s.dimx = 0x13579BDF2468ACE0ull & 0xFFFFFFFFFFFFull;  // 16 four-bit entries, values -4..+3
        Batch batch = MakeBatch(s, FlatQuad(0, 0, 8, 8, 130, 70, 210, 0xC0),
                                {PrimType::Triangle, PrimType::Triangle});
        failures += Report("hw_gpu_raster_dither", RunCase(drawer, s, batch, 63));
    }
    return failures ? 1 : 0;
}

// ---- M6 interface: present scheduler, auxiliary frames, per-pass timings ------------------------

// Default policy: a frame end is presented once and then re-presented on later ticks without a queue.
int TestPresentScheduler() {
    present::Scheduler s;
    if (s.Tick() != present::Action::none) {
        std::printf("hw_present_scheduler: a tick without any frame presented something\n");
        return 1;
    }
    present::FrameStamp first;
    first.guestFrame = 7;
    first.hasImage = true;
    s.SubmitFrame(first);
    if (s.Tick() != present::Action::presentNew || s.lastShown().guestFrame != 7) {
        std::printf("hw_present_scheduler: the submitted frame was not presented\n");
        return 1;
    }
    if (s.Tick() != present::Action::presentHeld) {
        std::printf("hw_present_scheduler: the held image was not re-presented\n");
        return 1;
    }
    if (s.presentedNew() != 1 || s.presentedHeld() != 1 || s.dropped() != 0) {
        std::printf("hw_present_scheduler: counters %llu/%llu/%llu\n",
                    static_cast<unsigned long long>(s.presentedNew()),
                    static_cast<unsigned long long>(s.presentedHeld()),
                    static_cast<unsigned long long>(s.dropped()));
        return 1;
    }
    // A frame end without a guest image still reaches the display (chapter 12.4 shows black).
    present::FrameStamp blank;
    blank.guestFrame = 8;
    blank.hasImage = false;
    s.SubmitFrame(blank);
    if (s.Tick() != present::Action::presentNew || s.hasImage()) {
        std::printf("hw_present_scheduler: an image-less frame end was mishandled\n");
        return 1;
    }
    std::printf("hw_present_scheduler: new/held/dropped = 2/1/0, newest frame %llu\n",
                static_cast<unsigned long long>(s.lastShown().guestFrame));
    return 0;
}

// Holding disabled: nothing is presented until a new frame end arrives, and a backlog keeps only the
// newest frame (never a queue).
int TestPresentSchedulerHold() {
    present::Scheduler s;
    present::Policy policy;
    policy.hold_last_image = false;
    s.SetPolicy(policy);
    present::FrameStamp stamp;
    stamp.hasImage = true;
    stamp.guestFrame = 1;
    s.SubmitFrame(stamp);
    if (s.Tick() != present::Action::presentNew) {
        std::printf("hw_present_scheduler_hold: first frame not presented\n");
        return 1;
    }
    if (s.Tick() != present::Action::none) {
        std::printf("hw_present_scheduler_hold: presented again with holding disabled\n");
        return 1;
    }
    stamp.guestFrame = 2;
    s.SubmitFrame(stamp);
    stamp.guestFrame = 3;
    s.SubmitFrame(stamp);
    if (s.dropped() != 1) {
        std::printf("hw_present_scheduler_hold: backlog was not collapsed (dropped %llu)\n",
                    static_cast<unsigned long long>(s.dropped()));
        return 1;
    }
    if (s.Tick() != present::Action::presentNew || s.lastShown().guestFrame != 3) {
        std::printf("hw_present_scheduler_hold: the newest frame did not win\n");
        return 1;
    }
    std::printf("hw_present_scheduler_hold: hold=none, newest wins, 1 dropped\n");
    return 0;
}

// Host rate above the guest rate: two ticks per guest frame give one new present and one re-present,
// and the late-input hook runs on every tick.
int TestPresentSchedulerRate() {
    present::Scheduler s;
    int hookCalls = 0;
    s.SetLateInputHook([](void* context) { ++*static_cast<int*>(context); }, &hookCalls);
    uint64_t newPresents = 0;
    uint64_t heldPresents = 0;
    for (uint64_t frame = 10; frame < 13; ++frame) {
        present::FrameStamp stamp;
        stamp.guestFrame = frame;
        stamp.hasImage = true;
        s.SubmitFrame(stamp);
        for (int tick = 0; tick < 2; ++tick) {
            const present::Action action = s.Tick();
            if (action == present::Action::presentNew) ++newPresents;
            if (action == present::Action::presentHeld) ++heldPresents;
        }
    }
    if (newPresents != 3 || heldPresents != 3) {
        std::printf("hw_present_scheduler_rate: %llu new / %llu held at 2x\n",
                    static_cast<unsigned long long>(newPresents),
                    static_cast<unsigned long long>(heldPresents));
        return 1;
    }
    if (hookCalls != 6) {
        std::printf("hw_present_scheduler_rate: late input hook ran %d times, expected 6\n", hookCalls);
        return 1;
    }
    std::printf("hw_present_scheduler_rate: 3 frames over 6 ticks = 3 new + 3 held, 6 late-input calls\n");
    return 0;
}

// GIF packet helpers for the auxiliary-frame test: one A+D packet writing registers, then an IMAGE
// packet carrying host->local data (chapter 02.1).
Qword PackedTag(uint32_t nloop, bool eop, uint32_t format, const std::vector<uint8_t>& regs) {
    Qword tag;
    tag.lo = uint64_t{nloop} | (uint64_t{eop ? 1u : 0u} << 15) | (uint64_t{format} << 58);
    tag.hi = 0;
    for (size_t i = 0; i < regs.size() && i < 16; ++i) {
        tag.hi |= uint64_t{regs[i]} << (4 * i);
    }
    if (!regs.empty()) {
        tag.lo |= uint64_t{static_cast<uint32_t>(regs.size())} << 60;
    }
    return tag;
}

// Uploads `bytes` (qword multiple) to GS page 0 with an 8x8 CT32 transfer.
void UploadToPage0(writeback::Renderer& renderer, uint32_t value) {
    std::vector<Qword> stream;
    stream.push_back(PackedTag(4, false, 0, {0xE}));
    const uint64_t bitbltbuf = (uint64_t{1} << 16) | (uint64_t{1} << 48);  // SBW = DBW = 1, CT32
    const uint64_t trxpos = 0;
    const uint64_t trxreg = uint64_t{8} | (uint64_t{8} << 32);
    stream.push_back(Qword{bitbltbuf, 0x50});
    stream.push_back(Qword{trxpos, 0x51});
    stream.push_back(Qword{trxreg, 0x52});
    stream.push_back(Qword{0, 0x53});  // TRXDIR = 0: host -> local
    const uint32_t qwords = 16;        // 8 x 8 CT32 = 256 bytes
    stream.push_back(PackedTag(qwords, true, 2, {}));
    for (uint32_t i = 0; i < qwords; ++i) {
        Qword data;
        data.lo = uint64_t{value} | (uint64_t{value} << 32);
        data.hi = uint64_t{value} | (uint64_t{value} << 32);
        stream.push_back(data);
    }
    renderer.GifTransfer(1, reinterpret_cast<const uint8_t*>(stream.data()), stream.size() * sizeof(Qword));
    renderer.Flush();
}

uint64_t SlmHash(const writeback::Renderer& renderer) {
    const std::uint8_t* bytes = renderer.Slm();
    uint64_t hash = 1469598103934665603ull;  // FNV-1a
    for (size_t i = 0; i < 4u * 1024u * 1024u; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

// Requirement 2: an auxiliary frame may draw, upload and clear, but the guest's memory is restored and
// a guest readback is never answered from it.
int TestAuxFrameGuestUntouched() {
    writeback::Renderer renderer;
    UploadToPage0(renderer, 0x11223344u);
    const uint64_t before = SlmHash(renderer);
    if (!renderer.BeginAuxiliaryFrame()) {
        std::printf("hw_aux_frame_guest_untouched: begin refused\n");
        return 1;
    }
    UploadToPage0(renderer, 0xAABBCCDDu);
    if (SlmHash(renderer) == before) {
        std::printf("hw_aux_frame_guest_untouched: the auxiliary upload changed nothing (test is blind)\n");
        renderer.EndAuxiliaryFrame();
        return 1;
    }
    std::uint8_t scratch[64] = {};
    if (renderer.ReadFifo(scratch, 4) != 0 || renderer.GetCounters().auxiliaryReadRefused != 1) {
        std::printf("hw_aux_frame_guest_untouched: a readback during an auxiliary frame was answered\n");
        renderer.EndAuxiliaryFrame();
        return 1;
    }
    renderer.EndAuxiliaryFrame();
    if (SlmHash(renderer) != before) {
        std::printf("hw_aux_frame_guest_untouched: guest memory differs after the auxiliary frame\n");
        return 1;
    }
    if (renderer.AuxiliaryActive() || renderer.GetCounters().auxiliaryFrames != 1) {
        std::printf("hw_aux_frame_guest_untouched: state not restored\n");
        return 1;
    }
    if (!renderer.BeginAuxiliaryFrame() || renderer.BeginAuxiliaryFrame()) {
        std::printf("hw_aux_frame_guest_untouched: nested auxiliary frame was accepted\n");
        return 1;
    }
    renderer.EndAuxiliaryFrame();
    std::printf("hw_aux_frame_guest_untouched: guest SLM identical, readback refused, nested begin refused\n");
    return 0;
}

// Requirement 4: per-pass CPU timers exist and are honest. Without a Vulkan device the drawer refuses
// the batch before any GPU work, so nothing is timed and the failure is reported instead of a fake zero
// being presented as a measurement. The compose pass is measured on its own.
int TestPassTimings() {
    writeback::Renderer renderer;
    UploadToPage0(renderer, 0x55AA55AAu);
    writeback::FrameOutput frame;
    renderer.ComposeFrame(frame);
    const writeback::PassTimings timings = renderer.Timings();  // copy: a later flush overwrites it
    if (!(timings.composeUs > 0.0)) {
        std::printf("hw_pass_timings: compose was not measured (%.3f us)\n", timings.composeUs);
        return 1;
    }
    if (timings.executeGpuUs != 0.0) {
        std::printf("hw_pass_timings: the GPU field is reserved and must stay 0\n");
        return 1;
    }

    // A draw without a device must fail cleanly and leave the draw timers at zero.
    std::vector<Qword> draw;
    draw.push_back(PackedTag(7, true, 0, {0xE}));
    draw.push_back(Qword{3, 0x00});                                          // PRIM = triangle
    draw.push_back(Qword{uint64_t{639} << 16 | uint64_t{447} << 48, 0x40});  // SCISSOR_1
    draw.push_back(Qword{uint64_t{1} << 16, 0x4C});                          // FRAME_1: FBW 1
    draw.push_back(Qword{0, 0x18});                                          // XYOFFSET_1
    draw.push_back(Qword{uint64_t{20} | (uint64_t{20} << 16) | (uint64_t{0x100} << 32), 0x05});
    draw.push_back(Qword{uint64_t{80} | (uint64_t{20} << 16) | (uint64_t{0x100} << 32), 0x05});
    draw.push_back(Qword{uint64_t{20} | (uint64_t{80} << 16) | (uint64_t{0x100} << 32), 0x05});
    renderer.GifTransfer(1, reinterpret_cast<const std::uint8_t *>(draw.data()), draw.size() * sizeof(Qword));
    renderer.Flush();
    if (!renderer.Failed() || renderer.LastError().empty()) {
        std::printf("hw_pass_timings: a draw without a device was not reported as a failure\n");
        return 1;
    }
    const writeback::PassTimings afterDraw = renderer.Timings();
    if (afterDraw.translateUs != 0.0 || afterDraw.executeUs != 0.0) {
        std::printf("hw_pass_timings: draw timers moved without a device (%.3f/%.3f us)\n",
                    afterDraw.translateUs, afterDraw.executeUs);
        return 1;
    }
    if (renderer.TimedFrames() < 2) {
        std::printf("hw_pass_timings: %llu timed frames, expected at least 2\n",
                    static_cast<unsigned long long>(renderer.TimedFrames()));
        return 1;
    }
    std::printf("hw_pass_timings: compose %.1f us, draw timers 0 without a device, %llu timed frames\n",
                timings.composeUs, static_cast<unsigned long long>(renderer.TimedFrames()));
    return 0;
}

// IN-335: the frame and depth buffers may share local memory (a 2D pass whose unused ZBUF points at the
// other display buffer, or at the frame itself). The write-back must not put the pre-draw depth words of
// the rectangle back over the pixels the batch just drew. The GPU result must equal the CPU reference,
// which stores the frame word and then the depth word per pixel. Cases: ZBUF = FRAME with Z writes
// masked, ZBUF = FRAME with Z written, and a ZBUF that starts one page row (32 lines) above FRAME, so
// only the lower rows of the depth rectangle alias the upper rows of the drawn frame rectangle (rows the
// write-back visits earlier, which is what exposes a stale depth word written over them).
// Z written into a buffer that aliases the frame is reported, not asserted: Z32 and CT32 swizzle the same
// page differently, so the last word stored at an address depends on the order in which the rasteriser
// visits pixels (CPU reference: primitive by primitive; write-back: one raster pass over the rectangle),
// which the specification does not define (IN-335).
int TestWritebackAliasedDepth(gpu::GpuDrawer& drawer) {
    struct AliasCase {
        const char* label;
        int32_t zbpOffsetPages;
        bool zmsk;
        bool asserted;
    };
    const AliasCase cases[] = {{"same pages, Z masked", 0, true, true},
                               {"same pages, Z written (report only)", 0, false, false},
                               {"one page row above, Z masked", -10, true, true}};
    int failures = 0;
    for (const AliasCase& c : cases) {
        Setup s;
        s.iip = false;
        s.fbp = 30;
        s.zbp = static_cast<uint32_t>(static_cast<int32_t>(s.fbp) + c.zbpOffsetPages);  // FBW 10: 10 pages per row
        s.zmsk = c.zmsk;
        s.zte = true;
        s.ztst = 1;  // ALWAYS
        Batch batch = MakeBatch(s, FlatQuad(4, 20, 60, 60, 200, 30, 90, 255), {PrimType::Triangle, PrimType::Triangle});
        const CaseResult r = RunCase(drawer, s, batch, static_cast<uint32_t>(41 + c.zbpOffsetPages));
        std::printf("  case: %s\n", c.label);
        const int failed = Report("hw_gpu_writeback_aliased_depth", r);
        if (c.asserted || r.gpuError) failures += failed;
    }
    return failures == 0 ? 0 : 1;
}

// Chapter 10.2: a LOCAL->HOST read observes every earlier draw, including draws still waiting in the open
// batch, and the data is consumed sequentially across FIFO reads. Two sprites of different colours fill
// rows 0..3 and 4..7 of an 8 x 8 CT32 rectangle; it is read back in two halves of 128 bytes with no VBlank
// flush in between, so the first half must hold only the top colour and the second only the bottom one.
int TestReadbackObservesPendingDraw(const gpu::VulkanHandles& handles) {
    writeback::Renderer renderer;
    std::string error;
    if (!renderer.Init(handles, error)) {
        std::printf("hw_readback_pending_draw: GPU drawer init failed: %s\n", error.c_str());
        return 1;
    }
    const uint32_t top = 0x80402010u;     // A 0x80, B 0x40, G 0x20, R 0x10
    const uint32_t bottom = 0x7F0A0B0Cu;  // A 0x7F, B 0x0A, G 0x0B, R 0x0C
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };  // Q = 1.0
    auto xyz = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    std::vector<Qword> stream;
    stream.push_back(PackedTag(16, true, 0, {0xE}));
    stream.push_back(Qword{uint64_t{0} | (uint64_t{1} << 16), 0x4C});            // FRAME_1: FBP 0, FBW 1, CT32
    stream.push_back(Qword{uint64_t{64} | (uint64_t{1} << 32), 0x4E});           // ZBUF_1: ZBP 64, ZMSK
    stream.push_back(Qword{(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40});  // SCISSOR_1: 0..63 x 0..63
    stream.push_back(Qword{0, 0x18});                                             // XYOFFSET_1 = 0
    stream.push_back(Qword{(uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47});    // TEST_1: ZTE, ZTST ALWAYS
    stream.push_back(Qword{6, 0x00});                                             // PRIM: sprite, context 1
    stream.push_back(Qword{rgbaq(top), 0x01});
    stream.push_back(Qword{xyz(0, 0), 0x05});
    stream.push_back(Qword{xyz(8, 4), 0x05});  // kick: rows 0..3
    stream.push_back(Qword{rgbaq(bottom), 0x01});
    stream.push_back(Qword{xyz(0, 4), 0x05});
    stream.push_back(Qword{xyz(8, 8), 0x05});                                     // kick: rows 4..7
    stream.push_back(Qword{uint64_t{0} | (uint64_t{1} << 16), 0x50});             // BITBLTBUF: SBP 0, SBW 1, CT32
    stream.push_back(Qword{0, 0x51});                                             // TRXPOS 0, 0
    stream.push_back(Qword{uint64_t{8} | (uint64_t{8} << 32), 0x52});             // TRXREG 8 x 8
    stream.push_back(Qword{1, 0x53});                                             // TRXDIR 1: local -> host
    if (!renderer.GifTransfer(1, reinterpret_cast<const uint8_t*>(stream.data()), stream.size() * sizeof(Qword))) {
        std::printf("hw_readback_pending_draw: GIF transfer refused\n");
        return 1;
    }
    std::vector<uint32_t> words(64, 0);
    const std::size_t first = renderer.ReadFifo(words.data(), 128);
    const std::size_t second = renderer.ReadFifo(words.data() + 32, 128);
    uint32_t pastEnd[4] = {};
    const std::size_t third = renderer.ReadFifo(pastEnd, sizeof(pastEnd));  // past the end: zeros, one overrun
    uint32_t wrong = 0;
    for (size_t i = 0; i < 32; ++i) wrong += words[i] != top ? 1u : 0u;
    for (size_t i = 32; i < 64; ++i) wrong += words[i] != bottom ? 1u : 0u;
    std::printf("hw_readback_pending_draw: read %zu + %zu bytes (+%zu past the end), %u of 64 words wrong, "
                "words 0/32 = %08X/%08X, overruns %llu\n",
                first, second, third, wrong, words[0], words[32],
                static_cast<unsigned long long>(renderer.GetCounters().fifoOverruns));
    if (renderer.Failed()) {
        std::printf("hw_readback_pending_draw: renderer failed: %s\n", renderer.LastError().c_str());
        return 1;
    }
    return (first == 128 && second == 128 && wrong == 0 && renderer.GetCounters().fifoOverruns == 1) ? 0 : 1;
}

// IN-335 replay control: a double-buffered 2D scene in which every frame draws into the back buffer
// while ZBUF (Z writes masked) stays at page 0, so on every other frame the unused depth buffer shares
// the pages of the buffer being drawn: the pattern of GT4's boot and menu screens (ZBUF = 0x100000000).
// The legacy capture is replayed through the CPU reference and through the GPU write-back renderer; every
// frame must give the same composed image and the same 4 MiB local memory hash. Only sprites are drawn,
// whose coverage and attributes both rasterisers are specified to agree on exactly, so any difference is
// a write-back or flush defect. Before the IN-335 fix the frames drawn into page 0 lost part of their draws here.
int TestReplayParityDoubleBuffer(const gpu::VulkanHandles& handles) {
    constexpr uint32_t kFrames = 6;
    constexpr uint32_t kBufferPages[2] = {0, 2};  // 64 x 64 CT32 buffers, FBW 1: two pages each
    cpuref::CaptureWriter capture;
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));                // PMODE: circuit 1, ALP 0xFF
    capture.Privileged(0x12000010u, uint64_t{2} << 13);                             // SMODE1: CMOD 2 (NTSC)
    capture.Privileged(0x12000080u, (uint64_t{63} << 32) | (uint64_t{63} << 44));  // DISPLAY1: 64 x 64
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };  // Q = 1.0
    auto xyz = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        const uint32_t back = kBufferPages[frame & 1u];
        const uint32_t background = 0x80000000u | (frame * 0x101010u + 0x203040u);
        const uint32_t text = 0x60000000u | (0xF0E0D0u - frame * 0x010203u);
        // A+D pairs: data, register address.
        const std::vector<uint64_t> words = {
            uint64_t{back} | (uint64_t{1} << 16), 0x4C,                         // FRAME_1: back buffer, FBW 1, CT32
            uint64_t{0} | (uint64_t{1} << 32), 0x4E,                            // ZBUF_1: page 0, ZMSK
            (uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40,                  // SCISSOR_1: 0..63 x 0..63
            0, 0x18,                                                            // XYOFFSET_1 = 0
            (uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47,                    // TEST_1: ZTE, ZTST ALWAYS
            (uint64_t{1} << 2) | (uint64_t{1} << 6), 0x42,                      // ALPHA_1: (Cs - Cd) * As + Cd
            6, 0x00,                                                            // PRIM: sprite
            rgbaq(background), 0x01, xyz(0, 0), 0x05, xyz(64, 64), 0x05,       // full-buffer clear
            6u | (1u << 6), 0x00,                                               // PRIM: sprite, ABE
            rgbaq(text), 0x01, xyz(4 + frame, 8), 0x05, xyz(40 + frame, 14), 0x05,  // a "text line"
        };
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0,
                                                     {0xE}, words));
        capture.Privileged(0x12000070u, uint64_t{back} | (uint64_t{1} << 9));  // DISPFB1: show the back buffer
        capture.FrameEnd(frame);
    }

    struct FrameRecord {
        bool valid = false;
        std::vector<uint8_t> rgb;
        uint64_t slmHash = 0;
    };
    std::vector<FrameRecord> cpuFrames;
    std::vector<FrameRecord> gpuFrames;
    auto record = [](std::vector<FrameRecord>& out, const cpuref::FrameImage& image, const uint8_t* slm) {
        FrameRecord r;
        r.valid = image.valid;
        if (image.valid) {
            for (const display::Rgba8& p : image.image.pixels) {
                r.rgb.push_back(p.r);
                r.rgb.push_back(p.g);
                r.rgb.push_back(p.b);
            }
        }
        r.slmHash = cpuref::HashLocalMemory(slm, 4u * 1024u * 1024u);
        out.push_back(std::move(r));
    };

    std::string error;
    cpuref::CpuRenderer cpu;
    cpuref::ReplaySummary cpuSummary;
    if (!cpuref::ReplayCapture(capture.Bytes(), cpu, cpuSummary,
                               [&](uint64_t, const cpuref::FrameImage& image) {
                                   record(cpuFrames, image, cpu.Memory().Local().bytes().data());
                               },
                               error)) {
        std::printf("hw_replay_parity_double_buffer: CPU replay failed: %s\n", error.c_str());
        return 1;
    }
    writeback::Renderer renderer;
    if (!renderer.Init(handles, error)) {
        std::printf("hw_replay_parity_double_buffer: GPU drawer init failed: %s\n", error.c_str());
        return 1;
    }
    writeback::WritebackReplayTarget target(renderer);
    cpuref::ReplaySummary gpuSummary;
    const bool replayed = cpuref::ReplayCapture(capture.Bytes(), target, gpuSummary,
                                                [&](uint64_t, const cpuref::FrameImage& image) {
                                                    record(gpuFrames, image, renderer.Slm());
                                                },
                                                error);
    if (!replayed || renderer.Failed()) {
        std::printf("hw_replay_parity_double_buffer: GPU replay failed: %s %s\n", error.c_str(),
                    renderer.LastError().c_str());
        return 1;
    }
    if (cpuFrames.size() != kFrames || gpuFrames.size() != kFrames) {
        std::printf("hw_replay_parity_double_buffer: frame counts %zu / %zu, expected %u\n", cpuFrames.size(),
                    gpuFrames.size(), kFrames);
        return 1;
    }
    int failures = 0;
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        const FrameRecord& c = cpuFrames[frame];
        const FrameRecord& g = gpuFrames[frame];
        const bool imageEqual = c.valid == g.valid && c.rgb == g.rgb;
        const bool memoryEqual = c.slmHash == g.slmHash;
        std::printf("  frame %u (%s): image %s (valid %d/%d, %zu bytes), memory %016llx / %016llx %s\n", frame,
                    (frame & 1u) ? "odd" : "even", imageEqual ? "equal" : "DIFFERENT", c.valid ? 1 : 0,
                    g.valid ? 1 : 0, c.rgb.size(), static_cast<unsigned long long>(c.slmHash),
                    static_cast<unsigned long long>(g.slmHash), memoryEqual ? "equal" : "DIFFERENT");
        if (!imageEqual || !memoryEqual) ++failures;
        // The fixture must not be blind: every frame changes memory and shows a non-empty image.
        if (frame > 0 && c.slmHash == cpuFrames[frame - 1].slmHash) {
            std::printf("  frame %u left memory unchanged (fixture is blind)\n", frame);
            ++failures;
        }
        if (!c.valid || c.rgb.empty()) {
            std::printf("  frame %u has no composed image (fixture is blind)\n", frame);
            ++failures;
        }
    }
    std::printf("hw_replay_parity_double_buffer: %u frames, %d failures\n", kFrames, failures);
    return failures == 0 ? 0 : 1;
}

// M6 output stage (13.3 step 3, 13.4, 12.9): the presentation compose follows the VBlank field, the
// reference compose (the donor-matching `vram` image) never does. A static 64 x 32 field buffer (one colour
// per row) is shown through a 480i NTSC circuit with FFMD = 1, so every source row covers two output rows.
//   - Reference: identical for even and odd fields; no deinterlace counters move.
//   - Deinterlace off: the presentation equals the reference for both fields.
//   - Weave over an even/odd pair: the odd field is shifted down one row (13.3 step 3) and woven, which
//     for static content must rebuild exactly the line-doubled reference image (no one-line bob).
//   - Weave of two field buffers (each with its own lines) gives the progressive frame (IN-339 phase).
//   - Bob: the CSR.FIELD = 1 field is shifted down one row (13.4, IN-339); the other is not.
// No Vulkan device is needed: uploads go to the shadow memory and composition is CPU work.
int TestPresentationFieldPhase() {
    constexpr uint32_t kWidth = 64;
    constexpr uint32_t kRows = 32;
    // Colour of progressive frame line `line` (448-line frame; field buffers hold every other line).
    auto lineColour = [](uint32_t line) {
        return 0x80000000u | ((line * 7u + 3u) & 0xFFu) | (((line * 13u) & 0xFFu) << 8);
    };
    // Field buffer: row k holds frame line 2k + fieldLine (fieldLine 0 = even field, 1 = odd field).
    auto upload = [&](writeback::Renderer& renderer, uint32_t fieldLine = 0) {
        std::vector<Qword> stream;
        stream.push_back(PackedTag(4, false, 0, {0xE}));
        stream.push_back(Qword{(uint64_t{1} << 16) | (uint64_t{1} << 48), 0x50});  // BITBLTBUF: DBP 0, DBW 1, CT32
        stream.push_back(Qword{0, 0x51});                                           // TRXPOS 0, 0
        stream.push_back(Qword{uint64_t{kWidth} | (uint64_t{kRows} << 32), 0x52});  // TRXREG 64 x 32
        stream.push_back(Qword{0, 0x53});                                           // TRXDIR 0: host -> local
        const uint32_t qwords = kWidth * kRows / 4;
        stream.push_back(PackedTag(qwords, true, 2, {}));
        for (uint32_t i = 0; i < qwords; ++i) {
            const uint32_t row = (i * 4) / kWidth;
            const uint32_t colour = lineColour(2 * row + fieldLine);
            stream.push_back(Qword{uint64_t{colour} | (uint64_t{colour} << 32), uint64_t{colour} | (uint64_t{colour} << 32)});
        }
        renderer.GifTransfer(1, reinterpret_cast<const uint8_t*>(stream.data()), stream.size() * sizeof(Qword));
    };
    auto setup = [&](writeback::Renderer& renderer, display::DeinterlaceMode mode) {
        writeback::OutputSettings settings;
        settings.deinterlace = mode;
        renderer.SetOutputSettings(settings);
        upload(renderer);
        renderer.WritePrivileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));  // PMODE: RC1, MMOD 1, ALP 255
        renderer.WritePrivileged(0x12000010u, uint64_t{2} << 13);              // SMODE1: NTSC
        renderer.WritePrivileged(0x12000020u, 3u);                             // SMODE2: INT, FFMD
        renderer.WritePrivileged(0x12000060u, 1u);                             // SYNCV: interlaced signal (13.1)
        renderer.WritePrivileged(0x12000070u, uint64_t{1} << 9);               // DISPFB1: FBP 0, FBW 1, CT32
        renderer.WritePrivileged(0x12000080u, (uint64_t{3} << 23) | (uint64_t{4 * kWidth - 1} << 32) |
                                                  (uint64_t{2 * kRows - 1} << 44));  // DISPLAY1: MAGH 3, 64 x 64
    };
    auto frame = [](writeback::Renderer& renderer, bool odd, writeback::ComposeKind kind) {
        renderer.SetDisplayField(odd);
        renderer.Flush();
        return renderer.Compose(kind);
    };
    using writeback::ComposeKind;
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("  %s: %s\n", what, ok ? "ok" : "FAILED");
        if (!ok) ++failures;
    };

    writeback::Renderer off;
    setup(off, display::DeinterlaceMode::Off);
    const writeback::FrameOutput refEven = frame(off, false, ComposeKind::Reference);
    const writeback::FrameOutput refOdd = frame(off, true, ComposeKind::Reference);
    const writeback::FrameOutput offEven = frame(off, false, ComposeKind::Presentation);
    const writeback::FrameOutput offOdd = frame(off, true, ComposeKind::Presentation);
    std::printf("  reference %ux%u valid %d\n", refEven.width, refEven.height, refEven.valid ? 1 : 0);
    check(refEven.valid && refEven.height >= 2 * kRows, "reference image exists and is full height");
    check(refEven.pixels == refOdd.pixels, "reference is independent of the field");
    check(offEven.pixels == refEven.pixels && offOdd.pixels == refEven.pixels, "deinterlace off presents the reference");
    check(off.GetCounters().deinterlacedFrames == 0 && off.GetCounters().interlaceOffsetFrames == 0,
          "deinterlace off moves no deinterlace counter");

    writeback::Renderer weave;
    setup(weave, display::DeinterlaceMode::Weave);
    frame(weave, false, ComposeKind::Presentation);
    const writeback::FrameOutput woven = frame(weave, true, ComposeKind::Presentation);
    check(woven.pixels == refEven.pixels, "weave of an even/odd pair rebuilds the line-doubled image");
    check(weave.GetCounters().interlaceOffsetFrames == 1 && weave.GetCounters().deinterlacedFrames == 2,
          "weave: one field offset (the CSR.FIELD = 0 field), two deinterlaced fields");

    // Field phase (IN-339): each field buffer holds its own lines, as GT4's field rendering does. The frame
    // whose display field is CSR.FIELD = 1 holds the upper lines (2k), the CSR.FIELD = 0 frame the lower
    // lines (2k + 1). Weaving the pair must give the progressive frame, line y = lineColour(y); the IN-35
    // reading (field 1 shifted down) puts every line two rows from its partner and fails here.
    writeback::Renderer phase;
    setup(phase, display::DeinterlaceMode::Weave);
    upload(phase, 1);
    frame(phase, false, ComposeKind::Presentation);
    upload(phase, 0);
    const writeback::FrameOutput progressive = frame(phase, true, ComposeKind::Presentation);
    bool progressiveOk = progressive.valid && progressive.height >= 2 * kRows && progressive.width >= kWidth;
    uint32_t wrongRows = 0;
    for (uint32_t y = 0; progressiveOk && y < 2 * kRows; ++y) {
        const uint32_t expected = lineColour(y) & 0x00FFFFFFu;
        const uint32_t got = progressive.pixels[static_cast<size_t>(y) * progressive.width] & 0x00FFFFFFu;
        if (got != expected) ++wrongRows;
    }
    std::printf("  progressive weave: %u of %u rows wrong\n", wrongRows, 2 * kRows);
    check(progressiveOk && wrongRows == 0, "weave of two field buffers gives the progressive frame");

    writeback::Renderer bob;
    setup(bob, display::DeinterlaceMode::Bob);
    // 13.4 bob shifts the row-0 field (CSR.FIELD = 1, IN-339) down by one row and leaves the other as is.
    const writeback::FrameOutput bobUpper = frame(bob, true, ComposeKind::Presentation);
    const writeback::FrameOutput bobLower = frame(bob, false, ComposeKind::Presentation);
    check(bobUpper.valid && bobUpper.pixels != refEven.pixels, "bob shifts the CSR.FIELD = 1 field by one row");
    check(bobLower.pixels == refEven.pixels, "bob leaves the CSR.FIELD = 0 field in place");

    std::printf("hw_presentation_field_phase: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// M7 gate (15.2: cache mode equals write-back mode), IN-340. A synthetic capture drives both sync modes of the
// GPU renderer through the same replay reader; every frame must give the same composed image and the same
// 4 MiB local memory hash. The frames exercise every coherence path of the resident targets:
//   - depth-tested draws with a separate depth buffer (colour and depth targets resident),
//   - a colour target switch (buffer A to buffer B) and back,
//   - a texture read from a target drawn in the same frame (feedback: written back before decoding),
//   - a host -> local upload into a drawn target (write back, then drop),
//   - a local -> local move out of a drawn target into another,
//   - a local -> host readback of a drawn target,
//   - a CLUT load from memory a draw just wrote, used by a T8 texture,
//   - a draw whose ZBUF shares the colour buffer's pages (the IN-335 case: write-back fallback).
// It also proves the cache is in use (resident batches, loads, write-backs) so a silently disabled cache fails.
// The hw_cache_equals_writeback stream (also replayed through core 2 by hw_core2_cpu_sprites, IN-407).
constexpr uint32_t kCacheFixtureFrames = 5;
cpuref::CaptureWriter CacheFixtureCapture() {
    constexpr uint32_t kFrames = kCacheFixtureFrames;
    constexpr uint64_t kA = 0, kB = 2, kZ = 8, kT8 = 12, kClut = 14;  // pages
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xyz = [](uint32_t x, uint32_t y, uint32_t z = 0) {
        return uint64_t{x * 16} | (uint64_t{y * 16} << 16) | (uint64_t{z} << 32);
    };
    auto uv = [](uint32_t u, uint32_t v) { return uint64_t{u * 16} | (uint64_t{v * 16} << 16); };
    auto frameReg = [](uint64_t page) { return page | (uint64_t{1} << 16); };  // FBW 1, CT32
    auto zbufReg = [](uint64_t page, bool masked) { return page | (masked ? (uint64_t{1} << 32) : 0); };  // Z32
    auto tex0 = [](uint64_t blocks, uint64_t psm, uint64_t log2, uint64_t cbpBlocks, bool load) {
        return blocks | (uint64_t{1} << 14) | (psm << 20) | (log2 << 26) | (log2 << 30) | (uint64_t{1} << 34) |
               (cbpBlocks << 37) | (uint64_t{load ? 1u : 0u} << 61);  // TBW 1, TCC, TFX modulate, CPSM CT32, CSM1
    };
    const uint64_t kSprite = 6, kTriangle = 3, kIip = 1u << 3, kTme = 1u << 4, kFst = 1u << 8;

    cpuref::CaptureWriter capture;
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));                // PMODE: RC1, MMOD 1, ALP 255
    capture.Privileged(0x12000010u, uint64_t{2} << 13);                             // SMODE1: NTSC
    capture.Privileged(0x12000070u, uint64_t{kA * 32} | (uint64_t{1} << 9));        // DISPFB1: buffer A
    capture.Privileged(0x12000080u, (uint64_t{63} << 32) | (uint64_t{63} << 44));  // DISPLAY1: 64 x 64
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    auto image = [&](const std::vector<uint64_t>& data) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(data.size() / 2), true, false, 0, 2, {}, data));
    };
    auto blit = [](uint64_t sbp, uint64_t spsm, uint64_t dbp, uint64_t dpsm) {
        return sbp | (uint64_t{1} << 16) | (spsm << 24) | (dbp << 32) | (uint64_t{1} << 48) | (dpsm << 56);
    };
    // Common state: scissor 0..63, no offset, depth test GEQUAL with writes (separate Z buffer).
    const std::vector<uint64_t> common = {(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18,
                                          (uint64_t{1} << 16) | (uint64_t{2} << 17), 0x47,  // TEST: ZTE, GEQUAL
                                          0, 0x42};                                          // ALPHA_1 (unused)
    // T8 texture data (64 x 64, one page) uploaded once: index = (x + 3y) & 255.
    {
        std::vector<uint64_t> words = {blit(0, 0, kT8 * 32, 0x13), 0x50, 0, 0x51, uint64_t{64} | (uint64_t{64} << 32), 0x52,
                                       0, 0x53};
        ad(words);
        std::vector<uint64_t> data;
        for (uint32_t i = 0; i < 64 * 64 / 8; ++i) {
            uint64_t word = 0;
            for (uint32_t b = 0; b < 8; ++b) {
                const uint32_t p = i * 8 + b;
                word |= uint64_t{((p % 64) + 3 * (p / 64)) & 255u} << (8 * b);
            }
            data.push_back(word);
        }
        image(data);
    }
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        std::vector<uint64_t> w = common;
        // 1. Buffer A, depth buffer Z: a clear sprite and two depth-tested Gouraud triangles.
        w.insert(w.end(), {frameReg(kA), 0x4C, zbufReg(kZ, false), 0x4E, kSprite, 0x00,
                           rgbaq(0x80102030u + frame), 0x01, xyz(0, 0, 0), 0x05, xyz(64, 64, 0), 0x05,
                           kTriangle | kIip, 0x00, rgbaq(0x80FF0000u), 0x01, xyz(3, 2, 500 + frame), 0x05,
                           rgbaq(0x8000FF00u), 0x01, xyz(60, 10, 900), 0x05, rgbaq(0x800000FFu), 0x01,
                           xyz(20 + frame, 61, 700), 0x05});
        // 2. Buffer B: clear, then a sprite textured from buffer A (drawn above, this frame).
        w.insert(w.end(), {frameReg(kB), 0x4C, zbufReg(kZ, true), 0x4E, kSprite, 0x00, rgbaq(0x80405060u), 0x01,
                           xyz(0, 0), 0x05, xyz(64, 64), 0x05, tex0(kA * 32, 0x00, 6, 0, false), 0x06, 0, 0x08,
                           kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(8, 8), 0x05,
                           uv(48, 48), 0x03, xyz(56, 56), 0x05});
        // 3. A palette drawn into memory (CT32 16 x 16 at page kClut), then a T8 texture using it, into A.
        w.insert(w.end(), {frameReg(kClut), 0x4C, kSprite | kIip, 0x00, rgbaq(0x80000000u | (frame * 0x111111u)), 0x01,
                           xyz(0, 0), 0x05, rgbaq(0x80FFFFFFu - frame), 0x01, xyz(16, 16), 0x05,
                           frameReg(kA), 0x4C, tex0(kT8 * 32, 0x13, 6, kClut * 32, true), 0x06,
                           kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(30, 30), 0x05,
                           uv(30, 30), 0x03, xyz(60, 60), 0x05});
        // 3b. Eighteen more small colour targets (pages 20, 24, ..., 44, then 52, 56, ..., 92, around page 48 used
        //     below): more targets than resident slots (IN-400: 15), so the least recently used ones are evicted
        //     (written back) and reloaded; then one is read as a texture.
        for (uint64_t k = 0; k < 18; ++k) {
            const uint64_t page = k < 7 ? 20 + 4 * k : 52 + 4 * (k - 7);
            w.insert(w.end(), {frameReg(page), 0x4C, kSprite, 0x00, rgbaq(0x80000000u + 0x0F0F0Fu * (k + 1) + frame),
                               0x01, xyz(2 * k, 1), 0x05, xyz(20 + 2 * k, 20), 0x05});
        }
        w.insert(w.end(), {frameReg(kA), 0x4C, tex0(20 * 32, 0x00, 5, 0, false), 0x06, kSprite | kTme | kFst, 0x00,
                           rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(40, 40), 0x05, uv(24, 24), 0x03,
                           xyz(64, 64), 0x05});
        // 3c. The same texture again after a state change (blending on, PRIM.ABE): a new batch whose GLM decode
        //     equals the previous one (IN-369 decode reuse).
        w.insert(w.end(), {kSprite | kTme | kFst | (uint64_t{1} << 6), 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03,
                           xyz(0, 16), 0x05, uv(24, 14), 0x03, xyz(24, 30), 0x05});
        ad(w);
        // 3d. An upload into that texture's page, then the texture once more (blending off again): the decode must
        //     not be reused, since the GLM changed in between (IN-369).
        ad({blit(0, 0, 20 * 32, 0x00), 0x50, uint64_t{0}, 0x51, uint64_t{8} | (uint64_t{8} << 32), 0x52, 0, 0x53});
        {
            std::vector<uint64_t> texels;
            for (uint32_t i = 0; i < 32; ++i) texels.push_back((uint64_t{0x80A0B0C0u ^ (i * 0x01030507u)} << 32) | (0x80405060u + i + frame));
            image(texels);
        }
        ad({kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(54, 22), 0x05, uv(10, 10), 0x03,
            xyz(64, 32), 0x05});
        // 3e. Deferred CLUT loads (IN-370), depth test ALWAYS. (1) A T4 load from the palette page with CSA 0, then
        //     an upload over that palette before the draw: the draw must use the palette as loaded (the load is
        //     recorded before the upload writes the GLM). (2) A T4 load of the uploaded palette with CSA 1 and a draw
        //     (palette read from the GLM). (3) A CSA 0 draw without a load: entries from load (1), CLUT buffer path.
        {
            auto t4 = [&](uint64_t cbpBlocks, uint64_t csa, bool load) {
                return tex0(kT8 * 32, 0x14, 4, cbpBlocks, load) | (csa << 56);
            };
            auto sprite = [&](uint32_t x0) {
                return std::vector<uint64_t>{kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03,
                                             xyz(x0, 16), 0x05, uv(16, 16), 0x03, xyz(x0 + 8, 28), 0x05};
            };
            ad({frameReg(kA), 0x4C, (uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47, t4(kClut * 32, 0, true), 0x06});
            ad({blit(0, 0, kClut * 32, 0x00), 0x50, uint64_t{0}, 0x51, uint64_t{8} | (uint64_t{2} << 32), 0x52, 0, 0x53});
            std::vector<uint64_t> palette;
            for (uint32_t i = 0; i < 8; ++i) {
                palette.push_back((uint64_t{0x80F00000u + 0x1020u * i + frame} << 32) | (0x800000F0u + 0x10200u * i));
            }
            image(palette);
            ad(sprite(26));
            std::vector<uint64_t> second = {t4(kClut * 32, 1, true), 0x06};
            for (uint64_t word : sprite(34)) second.push_back(word);
            ad(second);
            std::vector<uint64_t> third = {t4(kClut * 32, 0, false), 0x06};
            for (uint64_t word : sprite(42)) third.push_back(word);
            third.push_back((uint64_t{1} << 16) | (uint64_t{2} << 17));  // TEST back to GEQUAL
            third.push_back(0x47);
            ad(third);
        }
        // 4. Host -> local: 8 x 8 CT32 block into buffer A at (4 + frame, 4).
        ad({blit(0, 0, kA * 32, 0x00), 0x50, (uint64_t{4 + frame} << 32) | (uint64_t{4} << 48), 0x51,
            uint64_t{8} | (uint64_t{8} << 32), 0x52, 0, 0x53});
        std::vector<uint64_t> block;
        for (uint32_t i = 0; i < 32; ++i) block.push_back((uint64_t{0x11223344u + i + frame} << 32) | (0x55667788u - i));
        image(block);
        // 5. Local -> local: A (0,0) 16 x 16 to B (40, 40).
        ad({blit(kA * 32, 0x00, kB * 32, 0x00), 0x50, uint64_t{0} | (uint64_t{40} << 32) | (uint64_t{40} << 48), 0x51,
            uint64_t{16} | (uint64_t{16} << 32), 0x52, 2, 0x53});
        // 6. Local -> host: 8 x 8 of B at (36, 36) (drawn by the textured sprite).
        ad({blit(kB * 32, 0x00, 0, 0x00), 0x50, uint64_t{36} | (uint64_t{36} << 16), 0x51,
            uint64_t{8} | (uint64_t{8} << 32), 0x52, 1, 0x53});
        capture.FifoRead(16);
        // 7. A draw into A whose ZBUF is A itself with Z written (test ALWAYS): the write-back fallback.
        ad({frameReg(kA), 0x4C, zbufReg(kA, false), 0x4E, uint64_t{1} << 16 | (uint64_t{1} << 17), 0x47, kSprite, 0x00,
            rgbaq(0x80EEDDCCu), 0x01, xyz(50, 2), 0x05, xyz(62, 12), 0x05});
        // 8. GT4's buffer reuse (IN-350): page 48 drawn 128 wide (FBW 2), then 64 wide at the same base (the first
        //    target is parked in the GLM and the second loads from it), then partial CT16 and T4 uploads into it
        //    (merged into the GLM-owned pages), then read as a CT32 texture into A.
        ad({(uint64_t{1} << 16) | (uint64_t{2} << 17), 0x47, frameReg(48) | (uint64_t{1} << 16), 0x4C,
            zbufReg(kZ, true), 0x4E, kSprite, 0x00, rgbaq(0x80123456u + frame), 0x01, xyz(0, 0), 0x05, xyz(64, 40), 0x05,
            frameReg(48), 0x4C, kSprite, 0x00, rgbaq(0x80654321u), 0x01, xyz(5 + frame, 3), 0x05, xyz(30, 50), 0x05});
        ad({blit(0, 0, 48 * 32, 0x02), 0x50, (uint64_t{10} << 32) | (uint64_t{3 + frame} << 48), 0x51,
            uint64_t{8} | (uint64_t{4} << 32), 0x52, 0, 0x53});
        image({0x1111222233334444ull + frame, 0x5555666677778888ull, 0x99990000AAAABBBBull, 0xCCCCDDDDEEEEFFFFull,
               0x0123456789ABCDEFull, 0xFEDCBA9876543210ull, 0x0F0F0F0FF0F0F0F0ull, 0x1357924680ACE13Bull});
        ad({blit(0, 0, 48 * 32, 0x14), 0x50, (uint64_t{6} << 32) | (uint64_t{40} << 48), 0x51,
            uint64_t{8} | (uint64_t{4} << 32), 0x52, 0, 0x53});
        image({0x89ABCDEF01234567ull ^ frame, 0x76543210FEDCBA98ull});
        ad({frameReg(kA), 0x4C, tex0(48 * 32, 0x00, 6, 0, false), 0x06, kSprite | kTme | kFst, 0x00,
            rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(0, 32), 0x05, uv(64, 32), 0x03, xyz(64, 64), 0x05});
        capture.FrameEnd(frame);
    }
    return capture;
}

int TestCacheEqualsWriteback(const gpu::VulkanHandles& handles) {
    constexpr uint32_t kFrames = 5;
    const cpuref::CaptureWriter capture = CacheFixtureCapture();

    struct FrameRecord {
        bool valid = false;
        std::vector<uint8_t> rgb;
        uint64_t slmHash = 0;
        std::vector<uint8_t> slm;
    };
    uint64_t decodesReused = 0;  // IN-369, cache run
    uint64_t decodesGlmPalette = 0;  // IN-370, cache run
    uint64_t clutMaterialisations = 0;
    auto replay = [&](bool cacheMode, bool cpuSprites, std::vector<FrameRecord>& out, writeback::Counters& counters,
                      std::string& error) -> bool {
        writeback::Renderer renderer;
        if (!renderer.Init(handles, error)) return false;
        renderer.SetSyncMode(cacheMode);
        renderer.SetCpuSmallSprites(cpuSprites);
        renderer.SetGlmLoadsAlways(cpuSprites);  // the second cache run loads every slot through the GLM (IN-350)
        renderer.SetGpuUploads(!cpuSprites);     // ... and writes uploads on the CPU (GLM merges, IN-350/351)
        writeback::WritebackReplayTarget target(renderer);
        cpuref::ReplaySummary summary;
        const bool ok = cpuref::ReplayCapture(capture.Bytes(), target, summary,
                                              [&](uint64_t, const cpuref::FrameImage& image) {
                                                  FrameRecord r;
                                                  r.valid = image.valid;
                                                  for (const display::Rgba8& p : image.image.pixels) {
                                                      r.rgb.push_back(p.r);
                                                      r.rgb.push_back(p.g);
                                                      r.rgb.push_back(p.b);
                                                  }
                                                  const uint8_t* slm = renderer.Slm();
                                                  r.slmHash = cpuref::HashLocalMemory(slm, 4u * 1024u * 1024u);
                                                  r.slm.assign(slm, slm + 4u * 1024u * 1024u);
                                                  out.push_back(std::move(r));
                                              },
                                              error);
        counters = renderer.GetCounters();
        if (cacheMode && !cpuSprites) {
            decodesReused = renderer.GpuCounters().decodesReused;
            decodesGlmPalette = renderer.GpuCounters().decodesGlmPalette;
            clutMaterialisations = renderer.GpuCounters().clutMaterialisations;
        }
        if (renderer.Failed()) error += " " + renderer.LastError();
        return ok && !renderer.Failed();
    };
    // The cache run without the CPU sprite path (IN-348) keeps every small target on the GPU (evictions); the
    // second cache run with it must give the same images and memory too.
    std::vector<FrameRecord> reference;
    std::vector<FrameRecord> cached;
    std::vector<FrameRecord> cachedCpuSprites;
    writeback::Counters referenceCounters;
    writeback::Counters cacheCounters;
    writeback::Counters cpuSpriteCounters;
    std::string error;
    if (!replay(false, false, reference, referenceCounters, error) || !replay(true, false, cached, cacheCounters, error) ||
        !replay(true, true, cachedCpuSprites, cpuSpriteCounters, error)) {
        std::printf("hw_cache_equals_writeback: replay failed: %s\n", error.c_str());
        return 1;
    }
    int failures = 0;
    if (reference.size() != kFrames || cached.size() != kFrames) {
        std::printf("hw_cache_equals_writeback: frame counts %zu / %zu\n", reference.size(), cached.size());
        return 1;
    }
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        const bool image = reference[frame].valid == cached[frame].valid && reference[frame].rgb == cached[frame].rgb;
        const bool memory = reference[frame].slmHash == cached[frame].slmHash &&
                            cachedCpuSprites.size() == kFrames &&
                            reference[frame].slmHash == cachedCpuSprites[frame].slmHash &&
                            reference[frame].rgb == cachedCpuSprites[frame].rgb;
        std::printf("  frame %u: image %s, memory %016llx / %016llx %s\n", frame, image ? "equal" : "DIFFERENT",
                    static_cast<unsigned long long>(reference[frame].slmHash),
                    static_cast<unsigned long long>(cached[frame].slmHash), memory ? "equal" : "DIFFERENT");
        if (!memory) {
            // Name the 8 KiB GS pages that differ, so a failure points at the buffer involved.
            std::string pages;
            for (uint32_t page = 0; page < 512; ++page) {
                const size_t base = static_cast<size_t>(page) * 8192u;
                if (std::memcmp(reference[frame].slm.data() + base, cached[frame].slm.data() + base, 8192) != 0) {
                    pages += " " + std::to_string(page);
                }
            }
            std::printf("    differing pages:%s\n", pages.c_str());
        }
        if (!image || !memory) ++failures;
        if (frame > 0 && reference[frame].slmHash == reference[frame - 1].slmHash) {
            std::printf("  frame %u left memory unchanged (fixture is blind)\n", frame);
            ++failures;
        }
    }
    std::printf("  cache: resident %llu, fallback %llu, loads %llu, write-backs %llu\n",
                static_cast<unsigned long long>(cacheCounters.cacheResidentBatches),
                static_cast<unsigned long long>(cacheCounters.cacheFallbackBatches),
                static_cast<unsigned long long>(cacheCounters.cacheLoads),
                static_cast<unsigned long long>(cacheCounters.cacheWritebacks));
    std::printf("  cache: GLM upload merges %llu (%llu words, CPU-upload run), GPU host uploads %llu\n",
                static_cast<unsigned long long>(cpuSpriteCounters.glmUploadMerges),
                static_cast<unsigned long long>(cpuSpriteCounters.glmMergedWords),
                static_cast<unsigned long long>(cacheCounters.glmHostUploads));
    std::printf("  cache: GLM parks %llu, GLM slot loads %llu / %llu (always), GLM downloads %llu\n",
                static_cast<unsigned long long>(cacheCounters.glmParks),
                static_cast<unsigned long long>(cacheCounters.glmSlotLoads),
                static_cast<unsigned long long>(cpuSpriteCounters.glmSlotLoads),
                static_cast<unsigned long long>(cacheCounters.glmDownloads));
    std::printf("  cache: GLM decodes reused %llu, palettes from the GLM %llu, CLUT materialisations %llu\n",
                static_cast<unsigned long long>(decodesReused), static_cast<unsigned long long>(decodesGlmPalette),
                static_cast<unsigned long long>(clutMaterialisations));
    std::printf("  cache: evictions %llu, GPU CLUT loads %llu, GLM textures %llu, CPU sprite batches %llu\n",
                static_cast<unsigned long long>(cacheCounters.cacheEvictions),
                static_cast<unsigned long long>(cacheCounters.gpuClutLoads),
                static_cast<unsigned long long>(cacheCounters.texturesFromGlm),
                static_cast<unsigned long long>(cpuSpriteCounters.cpuSpriteBatches));
    if (cacheCounters.cacheResidentBatches == 0 || cacheCounters.cacheFallbackBatches == 0 ||
        cacheCounters.cacheLoads == 0 || cacheCounters.cacheWritebacks == 0 || cacheCounters.cacheEvictions == 0 ||
        referenceCounters.cacheResidentBatches != 0 || cpuSpriteCounters.cpuSpriteBatches == 0 ||
        cacheCounters.gpuClutLoads == 0 || cacheCounters.texturesFromGlm == 0 || cacheCounters.glmParks == 0 ||
        cacheCounters.glmSlotLoads == 0 || cacheCounters.glmDownloads == 0 || cpuSpriteCounters.glmUploadMerges == 0 || cacheCounters.glmBatchedWritebacks == 0 ||
        cacheCounters.glmHostUploads == 0 ||
        (handles.pixelInterlock && decodesReused == 0) ||  // runs (and their decode reuse) need interlock
        decodesGlmPalette == 0 || clutMaterialisations == 0 ||
        cpuSpriteCounters.glmSlotLoads < cpuSpriteCounters.cacheLoads) {
        std::printf("  the cache paths were not all exercised (fixture is blind)\n");
        ++failures;
    }
    // IN-355: presentation from the asynchronous capture equals the synchronous presentation one VBlank later
    // (nothing is shown at the first VBlank), and memory is unchanged by it.
    {
        uint64_t gpuComposeEqual = 0;
        uint64_t gpuComposeDifferent = 0;
        auto present = [&](bool async, std::vector<FrameRecord>& out, writeback::Counters& counters,
                           std::string& error) -> bool {
            writeback::Renderer renderer;
            if (!renderer.Init(handles, error)) return false;
            renderer.SetSyncMode(true);
            renderer.SetAsyncPresentation(async);
            renderer.SetGpuCompose(!async);  // the async run checks the CPU capture path (interlaced frames' path)
            writeback::WritebackReplayTarget target(renderer, writeback::ComposeKind::Presentation);
            target.SetGpuComposeCheck(!async);  // IN-364: the synchronous run also composes every frame on the GPU
            cpuref::ReplaySummary summary;
            const bool ok = cpuref::ReplayCapture(capture.Bytes(), target, summary,
                                                  [&](uint64_t, const cpuref::FrameImage& image) {
                                                      FrameRecord r;
                                                      r.valid = image.valid;
                                                      for (const display::Rgba8& p : image.image.pixels) {
                                                          r.rgb.push_back(p.r);
                                                          r.rgb.push_back(p.g);
                                                          r.rgb.push_back(p.b);
                                                      }
                                                      r.slmHash = cpuref::HashLocalMemory(renderer.Slm(),
                                                                                          4u * 1024u * 1024u);
                                                      out.push_back(std::move(r));
                                                  },
                                                  error);
            counters = renderer.GetCounters();
            if (!async) {
                gpuComposeEqual = target.GpuComposeEqual();
                gpuComposeDifferent = target.GpuComposeDifferent();
            }
            if (renderer.Failed()) error += " " + renderer.LastError();
            return ok && !renderer.Failed();
        };
        std::vector<FrameRecord> sync;
        std::vector<FrameRecord> async;
        writeback::Counters syncCounters;
        writeback::Counters asyncCounters;
        if (!present(false, sync, syncCounters, error) || !present(true, async, asyncCounters, error)) {
            std::printf("  async presentation: replay failed: %s\n", error.c_str());
            ++failures;
        } else if (sync.size() != kFrames || async.size() != kFrames) {
            std::printf("  async presentation: frame counts %zu / %zu\n", sync.size(), async.size());
            ++failures;
        } else {
            if (async[0].valid) {
                std::printf("  async presentation: frame 0 shows an image (expected none)\n");
                ++failures;
            }
            for (uint32_t frame = 0; frame < kFrames; ++frame) {
                const bool memory = sync[frame].slmHash == async[frame].slmHash;
                const bool image = frame == 0 || (async[frame].valid == sync[frame - 1].valid &&
                                                  async[frame].rgb == sync[frame - 1].rgb);
                std::printf("  async presentation frame %u: image %s previous VBlank's, memory %s\n", frame,
                            image ? "equals" : "DIFFERS from", memory ? "equal" : "DIFFERS");
                if (!image || !memory) ++failures;
            }
            std::printf("  GPU compose (IN-364): %llu frames equal to the CPU compose, %llu different\n",
                        static_cast<unsigned long long>(gpuComposeEqual),
                        static_cast<unsigned long long>(gpuComposeDifferent));
            if (gpuComposeEqual != kFrames || gpuComposeDifferent != 0) ++failures;
            uint32_t changing = 0;  // the shift is only visible when consecutive frames differ
            for (uint32_t frame = 1; frame < kFrames; ++frame) changing += sync[frame].rgb != sync[frame - 1].rgb ? 1u : 0u;
            if (changing < kFrames - 1 || asyncCounters.displayCaptures == 0 ||
                asyncCounters.presentationsFromCapture + 1 != kFrames ||
                syncCounters.presentationsFromCapture != 0) {
                std::printf("  async presentation: captures %llu, composes from captures %llu (fixture is blind)\n",
                            static_cast<unsigned long long>(asyncCounters.displayCaptures),
                            static_cast<unsigned long long>(asyncCounters.presentationsFromCapture));
                ++failures;
            }
        }
    }
    // IN-371/IN-372 (chapter 06): the same capture at an internal scale of 3. The 1x words stay the only authority,
    // so every frame's memory and 1x reference image equal the 1x cache run; the scaled GPU compose is 3x the size
    // and its 06.7 box downsample stays close to the 1x image (06.9 metric; exact equality is not expected where
    // triangles and filtered textures cover sub-pixels differently).
    {
        writeback::Renderer renderer;
        renderer.SetScale(3, 3.0f);
        std::vector<FrameRecord> scaledRun;
        std::vector<double> psnrs;
        uint32_t composed = 0;
        bool ok = renderer.Init(handles, error);
        if (ok) {
            renderer.SetSyncMode(true);
            writeback::WritebackReplayTarget target(renderer, writeback::ComposeKind::Reference);
            cpuref::ReplaySummary summary;
            ok = cpuref::ReplayCapture(
                capture.Bytes(), target, summary,
                [&](uint64_t, const cpuref::FrameImage& image) {
                    FrameRecord r;
                    r.valid = image.valid;
                    for (const display::Rgba8& p : image.image.pixels) {
                        r.rgb.push_back(p.r);
                        r.rgb.push_back(p.g);
                        r.rgb.push_back(p.b);
                    }
                    r.slmHash = cpuref::HashLocalMemory(renderer.Slm(), 4u * 1024u * 1024u);
                    writeback::GpuFrame gpuFrame;
                    std::vector<uint32_t> pixels;
                    if (image.valid && renderer.ComposeFrameGpu(gpuFrame) && renderer.ReadGpuFrame(gpuFrame, pixels) &&
                        gpuFrame.width == 3 * image.image.width && gpuFrame.height == 3 * image.image.height) {
                        ++composed;
                        double squared = 0.0;
                        for (uint32_t y = 0; y < image.image.height; ++y) {
                            for (uint32_t x = 0; x < image.image.width; ++x) {
                                uint32_t sums[3] = {0, 0, 0};
                                for (uint32_t dy = 0; dy < 3; ++dy) {
                                    for (uint32_t dx = 0; dx < 3; ++dx) {
                                        const uint32_t p = pixels[(y * 3 + dy) * gpuFrame.width + x * 3 + dx];
                                        sums[0] += p & 0xFFu;
                                        sums[1] += (p >> 8) & 0xFFu;
                                        sums[2] += (p >> 16) & 0xFFu;
                                    }
                                }
                                const display::Rgba8& native = image.image.at(x, y);
                                const int boxed[3] = {static_cast<int>((sums[0] + 4) / 9), static_cast<int>((sums[1] + 4) / 9),
                                                      static_cast<int>((sums[2] + 4) / 9)};
                                const int expected[3] = {native.r, native.g, native.b};
                                for (int c = 0; c < 3; ++c) squared += double(boxed[c] - expected[c]) * (boxed[c] - expected[c]);
                            }
                        }
                        const double mse = squared / (3.0 * image.image.width * image.image.height);
                        psnrs.push_back(mse == 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse));
                    }
                    scaledRun.push_back(std::move(r));
                },
                error);
            if (renderer.Failed()) error += " " + renderer.LastError();
            ok = ok && !renderer.Failed();
        }
        if (!ok || scaledRun.size() != kFrames) {
            std::printf("  scale 3: replay failed or %zu frames: %s\n", scaledRun.size(), error.c_str());
            ++failures;
        } else {
            for (uint32_t frame = 0; frame < kFrames; ++frame) {
                const bool same = scaledRun[frame].slmHash == cached[frame].slmHash && scaledRun[frame].rgb == cached[frame].rgb;
                if (!same) {
                    std::printf("  scale 3 frame %u: memory or 1x image DIFFERS from the 1x cache run\n", frame);
                    ++failures;
                }
            }
            const gpu::GpuStats& stats = renderer.GpuCounters();
            const writeback::Counters& c = renderer.GetCounters();
            double worst = 99.0;
            for (double value : psnrs) {
                worst = value < worst ? value : worst;
                std::printf("  scale 3: box PSNR %.2f dB\n", value);
            }
            std::printf("  scale 3: memory and 1x images equal to the 1x run; %u scaled composes, worst box PSNR %.2f dB; "
                        "scaled draws %llu, refreshes %llu, circuits from slots %llu\n",
                        composed, worst, static_cast<unsigned long long>(stats.scaledDraws),
                        static_cast<unsigned long long>(stats.scaledRefreshes),
                        static_cast<unsigned long long>(c.scaledCircuitsFromSlots));
            // This capture samples a noise-like T8 pattern and target copies with U = X (no texel-centre offset), so
            // sub-pixels sample neighbouring texels at scale and the box average differs from 1x (about 29 dB at
            // 3x, recorded in IN-372); hw_scale_exact checks the exact cases. Below 25 dB means a real defect.
            if (composed != kFrames || worst < 25.0 || stats.scaledDraws == 0 || stats.scaledRefreshes == 0 ||
                c.scaledCircuitsFromSlots == 0) {
                std::printf("  scale 3: composes, PSNR or scaled paths outside the expectation\n");
                ++failures;
            }
        }
    }
    std::printf("hw_cache_equals_writeback: %u frames, %d failures\n", kFrames, failures);
    return failures == 0 ? 0 : 1;
}

// hw_scale_exact (IN-371..IN-374, chapter 06): flat sprites on native pixel edges into buffer A, then A copied 1:1
// into buffer B as a nearest-sampled texture with the texel-centre coordinates U = X + 0.5 (the texture comes from
// the resident slot, so the scaled pass samples A's scaled image), and B displayed. At scales 2, 3 and 4 such content
// has an exact answer: memory equals the 1x run, every s x s block of the scaled compose is uniform (no seams, no
// half-pixel shift, no edge halos) and equals the 1x pixel, so the 06.7 box downsample equals the 1x image exactly.
int TestScaleExact(const gpu::VulkanHandles& handles) {
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xy = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    auto uvRaw = [](uint32_t u16, uint32_t v16) { return uint64_t{u16} | (uint64_t{v16} << 16); };
    auto frameReg = [](uint64_t page) { return page | (uint64_t{1} << 16); };  // FBW 1, CT32
    const uint64_t kSprite = 6, kTme = 1u << 4, kFst = 1u << 8, kA = 0, kB = 2, kZ = 8;
    cpuref::CaptureWriter capture;
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));                // PMODE: RC1, MMOD 1, ALP 255
    capture.Privileged(0x12000010u, uint64_t{2} << 13);                             // SMODE1: NTSC
    capture.Privileged(0x12000070u, uint64_t{kB} | (uint64_t{1} << 9));        // DISPFB1: buffer B (FBP in pages)
    capture.Privileged(0x12000080u, (uint64_t{3} << 23) | (uint64_t{255} << 32) | (uint64_t{63} << 44));  // 64 x 64
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    constexpr uint32_t kFrames = 3;
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        ad({(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18,
            (uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47,  // TEST: ZTE, ALWAYS
            frameReg(kA), 0x4C, kZ | (uint64_t{1} << 32), 0x4E,  // ZBUF masked
            kSprite, 0x00, rgbaq(0x80102030u + frame), 0x01, xy(0, 0), 0x05, xy(64, 64), 0x05,
            kSprite, 0x00, rgbaq(0x80E04010u), 0x01, xy(5 + frame, 7), 0x05, xy(20, 30), 0x05,
            kSprite, 0x00, rgbaq(0x8010E040u), 0x01, xy(33, 2), 0x05, xy(60, 9 + frame), 0x05,
            kSprite, 0x00, rgbaq(0x804010E0u), 0x01, xy(10, 40), 0x05, xy(50, 61), 0x05,
            frameReg(kB), 0x4C,
            uint64_t{kA * 32} | (uint64_t{1} << 14) | (uint64_t{6} << 26) | (uint64_t{6} << 30) | (uint64_t{1} << 34) |
                (uint64_t{1} << 35),
            0x06,  // TEX0: A, TBW 1, CT32, 64 x 64, TCC, TFX decal
            0, 0x14,  // TEX1_1: nearest
            kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uvRaw(8, 8), 0x03, xy(0, 0), 0x05,
            uvRaw(64 * 16 + 8, 64 * 16 + 8), 0x03, xy(64, 64), 0x05});
        capture.FrameEnd(frame);
    }

    struct Run {
        std::vector<uint64_t> slm;
        std::vector<std::vector<uint8_t>> native;    // 1x reference images
        std::vector<std::vector<uint32_t>> scaled;   // scaled GPU composes
        std::vector<uint32_t> width, height;
    };
    auto replay = [&](uint32_t scale, Run& run, gpu::GpuStats& stats, writeback::Counters& counters,
                      std::string& error) -> bool {
        writeback::Renderer renderer;
        renderer.SetScale(scale, static_cast<float>(scale));
        if (!renderer.Init(handles, error)) return false;
        renderer.SetSyncMode(true);
        renderer.SetCpuSmallSprites(false);  // the flat sprites go through the GPU (and its scaled pass)
        writeback::WritebackReplayTarget target(renderer, writeback::ComposeKind::Reference);
        cpuref::ReplaySummary summary;
        const bool ok = cpuref::ReplayCapture(capture.Bytes(), target, summary,
                                              [&](uint64_t, const cpuref::FrameImage& image) {
                                                  std::vector<uint8_t> rgb;
                                                  for (const display::Rgba8& p : image.image.pixels) {
                                                      rgb.push_back(p.r);
                                                      rgb.push_back(p.g);
                                                      rgb.push_back(p.b);
                                                  }
                                                  run.native.push_back(rgb);
                                                  run.width.push_back(image.image.width);
                                                  run.height.push_back(image.image.height);
                                                  run.slm.push_back(cpuref::HashLocalMemory(renderer.Slm(), 4u * 1024u * 1024u));
                                                  writeback::GpuFrame gpuFrame;
                                                  std::vector<uint32_t> pixels;
                                                  if (renderer.ComposeFrameGpu(gpuFrame) &&
                                                      renderer.ReadGpuFrame(gpuFrame, pixels) &&
                                                      gpuFrame.width == scale * image.image.width &&
                                                      gpuFrame.height == scale * image.image.height) {
                                                      run.scaled.push_back(std::move(pixels));
                                                  } else {
                                                      run.scaled.emplace_back();
                                                  }
                                              },
                                              error);
        stats = renderer.GpuCounters();
        counters = renderer.GetCounters();
        if (renderer.Failed()) error += " " + renderer.LastError();
        return ok && !renderer.Failed();
    };
    Run base;
    gpu::GpuStats baseStats;
    writeback::Counters baseCounters;
    std::string error;
    if (!replay(1, base, baseStats, baseCounters, error) || base.native.size() != kFrames) {
        std::printf("hw_scale_exact: 1x replay failed: %s\n", error.c_str());
        return 1;
    }
    int failures = 0;
    for (uint32_t scale = 2; scale <= 4; ++scale) {
        Run run;
        gpu::GpuStats stats;
        writeback::Counters counters;
        if (!replay(scale, run, stats, counters, error) || run.native.size() != kFrames) {
            std::printf("hw_scale_exact: scale %u replay failed: %s\n", scale, error.c_str());
            ++failures;
            continue;
        }
        uint64_t blockErrors = 0, pixelErrors = 0;
        bool sameMemory = true, composed = true;
        for (uint32_t frame = 0; frame < kFrames; ++frame) {
            sameMemory = sameMemory && run.slm[frame] == base.slm[frame] && run.native[frame] == base.native[frame];
            const std::vector<uint32_t>& pixels = run.scaled[frame];
            const uint32_t w = base.width[frame], h = base.height[frame];
            if (pixels.size() != static_cast<size_t>(w) * h * scale * scale) {
                composed = false;
                continue;
            }
            // 06.3 native mode: GS coordinate N lands on the centre of sub-pixel block N, so a sprite edge on a native
            // pixel edge falls floor(s/2) sub-pixels into the block: scaled pixel (sx, sy) shows the native pixel
            // ((sx - h) / s, (sy - h) / s) with h = floor(s/2), and the compose stands the first drawn sub-pixel in for
            // the h undrawn rows and columns at the top-left edge (IN-372).
            const uint32_t half = scale / 2;
            for (uint32_t sy = 0; sy < h * scale; ++sy) {
                for (uint32_t sx = 0; sx < w * scale; ++sx) {
                    const uint32_t nx = (std::max(sx, half) - half) / scale;
                    const uint32_t ny = (std::max(sy, half) - half) / scale;
                    const uint32_t got = pixels[static_cast<size_t>(sy) * w * scale + sx] & 0xFFFFFFu;
                    const uint8_t* n = &base.native[frame][(static_cast<size_t>(ny) * w + nx) * 3];
                    const uint32_t expected = uint32_t{n[0]} | (uint32_t{n[1]} << 8) | (uint32_t{n[2]} << 16);
                    if (got != expected) {
                        if (pixelErrors < 6) {
                            std::printf("    scale %u frame %u sub-pixel %u,%u: scaled %06x, expected (native %u,%u) %06x\n",
                                        scale, frame, sx, sy, got, nx, ny, expected);
                        }
                        ++pixelErrors;
                    }
                }
            }
            // The 06.7 box downsample equals 1x everywhere except on the half-pixel band around edges: counted only.
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const uint32_t first = pixels[(y * scale) * w * scale + x * scale] & 0xFFFFFFu;
                    for (uint32_t dy = 0; dy < scale; ++dy) {
                        for (uint32_t dx = 0; dx < scale; ++dx) {
                            if ((pixels[(y * scale + dy) * w * scale + x * scale + dx] & 0xFFFFFFu) != first) ++blockErrors;
                        }
                    }
                }
            }
        }
        std::printf("  scale %u: memory %s, composes %s, non-uniform blocks (edge bands) %llu, sub-pixels off the 06.3 "
                    "expectation %llu, "
                    "scaled draws %llu, scaled texture copies %llu, circuits from slots %llu, scaled passes %llu, "
                    "clipped %llu\n",
                    scale, sameMemory ? "equal" : "DIFFERENT", composed ? "all" : "MISSING",
                    static_cast<unsigned long long>(blockErrors), static_cast<unsigned long long>(pixelErrors),
                    static_cast<unsigned long long>(stats.scaledDraws),
                    static_cast<unsigned long long>(stats.scaledTextureCopies),
                    static_cast<unsigned long long>(counters.scaledCircuitsFromSlots),
                    static_cast<unsigned long long>(stats.scaledPasses),
                    static_cast<unsigned long long>(stats.scaledDrawsClipped));
        // Blindness: the displayed 1x image must show the clear colour and the three sprites (4 colours or more).
        std::vector<uint32_t> colours;
        for (size_t i = 0; i + 2 < base.native[0].size(); i += 3) {
            const uint32_t c = uint32_t{base.native[0][i]} | (uint32_t{base.native[0][i + 1]} << 8) |
                               (uint32_t{base.native[0][i + 2]} << 16);
            if (std::find(colours.begin(), colours.end(), c) == colours.end()) colours.push_back(c);
        }
        if (colours.size() < 4) {
            std::printf("  scale %u: the displayed image has %zu colours (fixture is blind)\n", scale, colours.size());
        }
        if (colours.size() < 4 || !sameMemory || !composed || blockErrors == 0 || pixelErrors != 0 || stats.scaledDraws == 0 ||
            stats.scaledTextureCopies == 0 || counters.scaledCircuitsFromSlots != kFrames) {
            ++failures;
        }
    }
    std::printf("hw_scale_exact: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_scale_target_extent (IN-384, parity plan Part A): a scaled image covers only its slot's real extent and grows on
// demand. Three shapes in one capture, all flat CT32 sprites so the 06.3 expectation is exact (as in hw_scale_exact):
//   - frame 0: target A at page 0, FBW 1 (64 wide), 64 rows, shown;
//   - frame 1: the same view grows to 128 rows (a load and a draw outside the old extent), so A's scaled image has to
//     grow while it already holds the previous frame's content, and the whole 64 x 128 picture must be presented;
//   - frame 2: another buffer B at page 8 with FBW 2 (128 wide), 64 rows: a different width, its own image;
//   - frame 3: A shown again, so the grown image is reused after the other view.
// Every scaled frame must equal the 06.3 expectation of its 1x image, memory must equal the 1x run, no scaled draw may
// leave its pair's images (clipped 0), the images must have grown, and their device memory must stay a small fraction
// of the fixed 1024 x 1024 - per - slot squares (184 MB at scale 2).
int TestScaleTargetExtent(const gpu::VulkanHandles& handles) {
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xy = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    auto frameReg = [](uint64_t page, uint64_t fbw) {
        return page | (fbw << 16);  // FBP in pages, FBW in 64-pixel units, PSM CT32
    };
    auto dispFb = [](uint64_t page, uint64_t fbw) { return page | (fbw << 9); };
    auto dispSize = [](uint64_t width, uint64_t height) {
        return (uint64_t{3} << 23) | ((width * 4 - 1) << 32) | ((height - 1) << 44);
    };
    const uint64_t kSprite = 6, kZ = 8;
    constexpr uint32_t kFrames = 4;
    cpuref::CaptureWriter capture;
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));  // PMODE: RC1, MMOD 1, ALP 255
    capture.Privileged(0x12000010u, uint64_t{2} << 13);              // SMODE1: NTSC
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    // Frame 0: A (page 0, FBW 1) 64 x 64, clear plus one sprite, shown as 64 x 64.
    ad({(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18,
        (uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47,  // TEST: ZTE, ALWAYS
        frameReg(0, 1), 0x4C, kZ | (uint64_t{1} << 32), 0x4E,
        kSprite, 0x00, rgbaq(0x80102030u), 0x01, xy(0, 0), 0x05, xy(63, 63), 0x05,
        kSprite, 0x00, rgbaq(0x80E04010u), 0x01, xy(8, 8), 0x05, xy(40, 40), 0x05});
    capture.Privileged(0x12000070u, dispFb(0, 1));
    capture.Privileged(0x12000080u, dispSize(64, 64));
    capture.FrameEnd(0);
    // Frame 1: the same view over 128 rows: the load and the draw reach past the 64-row extent.
    ad({(uint64_t{63} << 16) | (uint64_t{127} << 48), 0x40, 0, 0x18,
        (uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47,
        frameReg(0, 1), 0x4C, kZ | (uint64_t{1} << 32), 0x4E,
        kSprite, 0x00, rgbaq(0x80102030u), 0x01, xy(0, 0), 0x05, xy(63, 63), 0x05,
        kSprite, 0x00, rgbaq(0x804010E0u), 0x01, xy(16, 64), 0x05, xy(47, 111), 0x05});
    capture.Privileged(0x12000070u, dispFb(0, 1));
    capture.Privileged(0x12000080u, dispSize(64, 128));
    capture.FrameEnd(1);
    // Frame 2: buffer B (page 8, FBW 2 = 128 wide, 64 rows): another width, its own scaled image.
    ad({(uint64_t{127} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18,
        (uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47,
        frameReg(8, 2), 0x4C, kZ | (uint64_t{1} << 32), 0x4E,
        kSprite, 0x00, rgbaq(0x80304090u), 0x01, xy(0, 0), 0x05, xy(127, 63), 0x05,
        kSprite, 0x00, rgbaq(0x80C05020u), 0x01, xy(32, 16), 0x05, xy(96, 47), 0x05});
    capture.Privileged(0x12000070u, dispFb(8, 2));
    capture.Privileged(0x12000080u, dispSize(128, 64));
    capture.FrameEnd(2);
    // Frame 3: A again, 64 x 128, with a different picture (the grown image is reused after the other view).
    ad({(uint64_t{63} << 16) | (uint64_t{127} << 48), 0x40, 0, 0x18,
        (uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47,
        frameReg(0, 1), 0x4C, kZ | (uint64_t{1} << 32), 0x4E,
        kSprite, 0x00, rgbaq(0x802060B0u), 0x01, xy(0, 0), 0x05, xy(63, 127), 0x05,
        kSprite, 0x00, rgbaq(0x8010E040u), 0x01, xy(0, 0), 0x05, xy(31, 31), 0x05});
    capture.Privileged(0x12000070u, dispFb(0, 1));
    capture.Privileged(0x12000080u, dispSize(64, 128));
    capture.FrameEnd(3);

    struct Run {
        std::vector<uint64_t> slm;
        std::vector<std::vector<uint8_t>> native;
        std::vector<std::vector<uint32_t>> scaled;
        std::vector<uint32_t> width, height;
    };
    auto replay = [&](uint32_t scale, Run& run, gpu::GpuStats& stats, std::string& error) -> bool {
        writeback::Renderer renderer;
        renderer.SetScale(scale, static_cast<float>(scale));
        if (!renderer.Init(handles, error)) return false;
        renderer.SetSyncMode(true);
        renderer.SetCpuSmallSprites(false);
        writeback::WritebackReplayTarget target(renderer, writeback::ComposeKind::Reference);
        cpuref::ReplaySummary summary;
        const bool ok = cpuref::ReplayCapture(capture.Bytes(), target, summary,
                                              [&](uint64_t, const cpuref::FrameImage& image) {
                                                  std::vector<uint8_t> rgb;
                                                  for (const display::Rgba8& p : image.image.pixels) {
                                                      rgb.push_back(p.r);
                                                      rgb.push_back(p.g);
                                                      rgb.push_back(p.b);
                                                  }
                                                  run.native.push_back(rgb);
                                                  run.width.push_back(image.image.width);
                                                  run.height.push_back(image.image.height);
                                                  run.slm.push_back(cpuref::HashLocalMemory(renderer.Slm(), 4u * 1024u * 1024u));
                                                  writeback::GpuFrame gpuFrame;
                                                  std::vector<uint32_t> pixels;
                                                  if (renderer.ComposeFrameGpu(gpuFrame) &&
                                                      renderer.ReadGpuFrame(gpuFrame, pixels) &&
                                                      gpuFrame.width == scale * image.image.width &&
                                                      gpuFrame.height == scale * image.image.height) {
                                                      run.scaled.push_back(std::move(pixels));
                                                  } else {
                                                      run.scaled.emplace_back();
                                                  }
                                              },
                                              error);
        stats = renderer.GpuCounters();
        if (renderer.Failed()) error += " " + renderer.LastError();
        return ok && !renderer.Failed();
    };
    Run base;
    gpu::GpuStats baseStats;
    std::string error;
    if (!replay(1, base, baseStats, error) || base.native.size() != kFrames) {
        std::printf("hw_scale_target_extent: 1x replay failed: %s\n", error.c_str());
        return 1;
    }
    if (base.width[1] != 64 || base.height[1] != 128 || base.width[2] != 128 || base.height[2] != 64) {
        std::printf("hw_scale_target_extent: the 1x frames are not the shapes the fixture draws (%ux%u, %ux%u)\n",
                    base.width[1], base.height[1], base.width[2], base.height[2]);
        return 1;
    }
    int failures = 0;
    for (uint32_t scale = 2; scale <= 4; scale += 2) {
        Run run;
        gpu::GpuStats stats;
        if (!replay(scale, run, stats, error) || run.native.size() != kFrames) {
            std::printf("hw_scale_target_extent: scale %u replay failed: %s\n", scale, error.c_str());
            ++failures;
            continue;
        }
        uint64_t pixelErrors = 0;
        bool sameMemory = true, composed = true;
        for (uint32_t frame = 0; frame < kFrames; ++frame) {
            sameMemory = sameMemory && run.slm[frame] == base.slm[frame] && run.native[frame] == base.native[frame];
            const std::vector<uint32_t>& pixels = run.scaled[frame];
            const uint32_t w = base.width[frame], h = base.height[frame];
            if (pixels.size() != static_cast<size_t>(w) * h * scale * scale) {
                composed = false;
                continue;
            }
            const uint32_t half = scale / 2;
            for (uint32_t sy = 0; sy < h * scale; ++sy) {
                for (uint32_t sx = 0; sx < w * scale; ++sx) {
                    const uint32_t nx = (std::max(sx, half) - half) / scale;
                    const uint32_t ny = (std::max(sy, half) - half) / scale;
                    const uint32_t got = pixels[static_cast<size_t>(sy) * w * scale + sx] & 0xFFFFFFu;
                    const uint8_t* n = &base.native[frame][(static_cast<size_t>(ny) * w + nx) * 3];
                    const uint32_t expected = uint32_t{n[0]} | (uint32_t{n[1]} << 8) | (uint32_t{n[2]} << 16);
                    if (got != expected) {
                        if (pixelErrors < 6) {
                            std::printf("    scale %u frame %u sub-pixel %u,%u: scaled %06x, expected (native %u,%u) "
                                        "%06x\n",
                                        scale, frame, sx, sy, got, nx, ny, expected);
                        }
                        ++pixelErrors;
                    }
                }
            }
        }
        // The fixed 1024 x 1024 per-slot squares of IN-371 cost 11 images x (1024 * scale)^2 x 4 bytes; this capture's
        // slots are at most 128 x 128 native, so a few MB is all that may be allocated.
        const double fixedSquaresMb = 11.0 * 1024.0 * 1024.0 * 4.0 * scale * scale / 1e6;
        const double imageMb = static_cast<double>(stats.scaledImageBytes) / 1e6;
        std::printf("  scale %u: memory %s, composes %s, sub-pixels off the 06.3 expectation %llu, scaled draws %llu, "
                    "scaled passes %llu, grows %llu, clipped %llu, scaled image %.2f MB (fixed squares %.0f MB)\n",
                    scale, sameMemory ? "equal" : "DIFFERENT", composed ? "all" : "MISSING",
                    static_cast<unsigned long long>(pixelErrors),
                    static_cast<unsigned long long>(stats.scaledDraws),
                    static_cast<unsigned long long>(stats.scaledPasses),
                    static_cast<unsigned long long>(stats.scaledImageGrows),
                    static_cast<unsigned long long>(stats.scaledDrawsClipped), imageMb, fixedSquaresMb);
        if (!sameMemory || !composed || pixelErrors != 0 || stats.scaledDraws == 0 || stats.scaledImageGrows == 0 ||
            stats.scaledDrawsClipped != 0 || imageMb >= fixedSquaresMb / 8.0) {
            ++failures;
        }
    }
    std::printf("hw_scale_target_extent: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_scale_display_shadow (owner test 6, IN-377): GT4's race double buffer at an internal scale. Each frame first draws
// a small target whose depth buffer lies on the buffer the next VBlank shows (finished one frame earlier), then the
// back buffer, then shows the other buffer: the depth load drops the shown buffer's colour slot, so before IN-377 the
// presented frame was composed from the GLM at 1x (no visible upscaling). Two variants:
//   - read-only depth (ZMSK = 1): memory of the shown buffer does not change. Every shown frame from the third on
//     must come from a display shadow and match the 06.3 exact expectation of the 1x image at scale (the same
//     check as hw_scale_exact), so the presented picture is really upscaled.
//   - depth writes (ZMSK = 0, ZTST ALWAYS): part of the shown buffer's memory changes. Every native pixel the write
//     changed must be shown from memory (1x), every other one still from the shadow at scale.
// Memory must equal the 1x run in both variants (the 1x words stay the only authority).
int TestScaleDisplayShadow(const gpu::VulkanHandles& handles) {
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xyz = [](uint32_t x, uint32_t y, uint32_t z) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16) | (uint64_t{z} << 32); };
    auto frameReg = [](uint64_t page) { return page | (uint64_t{1} << 16); };  // FBW 1, CT32
    const uint64_t kSprite = 6;
    const uint64_t kBuffers[2] = {0, 2};  // 64 x 64 CT32 buffers (two pages each)
    const uint64_t kSmallTarget = 8;      // the small target drawn first each frame
    constexpr uint32_t kFrames = 6;
    constexpr uint32_t kFirstChecked = 3;  // the shown buffer was shown before (a display candidate) from here on

    auto makeCapture = [&](bool depthWrites) {
        cpuref::CaptureWriter capture;
        capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));  // PMODE: RC1, MMOD 1, ALP 255
        capture.Privileged(0x12000010u, uint64_t{2} << 13);               // SMODE1: NTSC
        capture.Privileged(0x12000080u, (uint64_t{3} << 23) | (uint64_t{255} << 32) | (uint64_t{63} << 44));  // 64 x 64
        for (uint32_t frame = 0; frame < kFrames; ++frame) {
            const uint64_t back = kBuffers[frame & 1u];
            const uint64_t front = kBuffers[(frame + 1u) & 1u];
            // ZTE with GEQUAL (reads depth) and ZMSK, or ALWAYS with depth writes.
            const uint64_t test = depthWrites ? ((uint64_t{1} << 16) | (uint64_t{1} << 17))
                                              : ((uint64_t{1} << 16) | (uint64_t{2} << 17));
            const uint64_t zbuf = front | (depthWrites ? 0 : (uint64_t{1} << 32));  // Z32 on the shown buffer
            const std::vector<uint64_t> words = {
                (uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18,
                test, 0x47, frameReg(kSmallTarget), 0x4C, zbuf, 0x4E,
                kSprite, 0x00, rgbaq(0x80305070u + frame), 0x01, xyz(4, 4, 0x00ABCDEFu), 0x05, xyz(28, 20, 0x00ABCDEFu), 0x05,
                (uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47,  // TEST: ZTE, ALWAYS
                frameReg(back), 0x4C, uint64_t{16} | (uint64_t{1} << 32), 0x4E,  // ZBUF elsewhere, masked
                kSprite, 0x00, rgbaq(0x80102030u + frame), 0x01, xyz(0, 0, 0), 0x05, xyz(64, 64, 0), 0x05,
                kSprite, 0x00, rgbaq(0x80E04010u), 0x01, xyz(5 + frame, 7, 0), 0x05, xyz(20, 30, 0), 0x05,
                kSprite, 0x00, rgbaq(0x8010E040u), 0x01, xyz(33, 2, 0), 0x05, xyz(60, 9 + frame, 0), 0x05,
                kSprite, 0x00, rgbaq(0x804010E0u), 0x01, xyz(10, 40, 0), 0x05, xyz(50, 61, 0), 0x05};
            capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                         words));
            capture.Privileged(0x12000070u, front | (uint64_t{1} << 9));  // DISPFB1: the buffer finished last frame
            capture.FrameEnd(frame);
        }
        return capture;
    };

    struct Run {
        std::vector<uint64_t> slm;
        std::vector<std::vector<uint8_t>> native;   // 1x reference images
        std::vector<std::vector<uint32_t>> scaled;  // scaled GPU composes (the presented frames)
        uint32_t width = 0, height = 0;
        writeback::Counters counters;
    };
    auto replay = [&](const cpuref::CaptureWriter& capture, uint32_t scale, Run& run, std::string& error) -> bool {
        writeback::Renderer renderer;
        renderer.SetScale(scale, static_cast<float>(scale));
        if (!renderer.Init(handles, error)) return false;
        renderer.SetSyncMode(true);
        renderer.SetCpuSmallSprites(false);  // the flat sprites go through the GPU (and its scaled pass)
        writeback::WritebackReplayTarget target(renderer, writeback::ComposeKind::Reference);
        cpuref::ReplaySummary summary;
        const bool ok = cpuref::ReplayCapture(capture.Bytes(), target, summary,
                                              [&](uint64_t, const cpuref::FrameImage& image) {
                                                  std::vector<uint8_t> rgb;
                                                  for (const display::Rgba8& p : image.image.pixels) {
                                                      rgb.push_back(p.r);
                                                      rgb.push_back(p.g);
                                                      rgb.push_back(p.b);
                                                  }
                                                  run.native.push_back(rgb);
                                                  run.width = image.image.width;
                                                  run.height = image.image.height;
                                                  run.slm.push_back(cpuref::HashLocalMemory(renderer.Slm(), 4u * 1024u * 1024u));
                                                  writeback::GpuFrame gpuFrame;
                                                  std::vector<uint32_t> pixels;
                                                  if (scale > 1 && renderer.ComposeFrameGpu(gpuFrame) &&
                                                      renderer.ReadGpuFrame(gpuFrame, pixels) &&
                                                      gpuFrame.width == scale * image.image.width &&
                                                      gpuFrame.height == scale * image.image.height) {
                                                      run.scaled.push_back(std::move(pixels));
                                                  } else {
                                                      run.scaled.emplace_back();
                                                  }
                                              },
                                              error);
        run.counters = renderer.GetCounters();
        if (renderer.Failed()) error += " " + renderer.LastError();
        return ok && !renderer.Failed() && run.native.size() == kFrames;
    };
    auto rgbAt = [](const std::vector<uint8_t>& image, uint32_t width, uint32_t x, uint32_t y) {
        const uint8_t* n = &image[(static_cast<size_t>(y) * width + x) * 3];
        return uint32_t{n[0]} | (uint32_t{n[1]} << 8) | (uint32_t{n[2]} << 16);
    };

    int failures = 0;
    std::string error;
    const cpuref::CaptureWriter readOnly = makeCapture(false);
    const cpuref::CaptureWriter writes = makeCapture(true);
    Run readOnlyBase, writesBase;
    if (!replay(readOnly, 1, readOnlyBase, error) || !replay(writes, 1, writesBase, error)) {
        std::printf("hw_scale_display_shadow: 1x replay failed: %s\n", error.c_str());
        return 1;
    }
    // Blindness: the depth writes must change what the shown buffer holds, and the read-only depth must not.
    uint64_t changedNative = 0;
    for (uint32_t frame = kFirstChecked; frame < kFrames; ++frame) {
        for (uint32_t y = 0; y < readOnlyBase.height; ++y) {
            for (uint32_t x = 0; x < readOnlyBase.width; ++x) {
                if (rgbAt(readOnlyBase.native[frame], readOnlyBase.width, x, y) !=
                    rgbAt(writesBase.native[frame], writesBase.width, x, y)) {
                    ++changedNative;
                }
            }
        }
    }
    if (changedNative == 0) {
        std::printf("hw_scale_display_shadow: the depth writes change no shown pixel (fixture is blind)\n");
        ++failures;
    }
    for (uint32_t scale : {2u, 4u}) {
        for (const bool depthWrites : {false, true}) {
            const Run& base = depthWrites ? writesBase : readOnlyBase;
            Run run;
            if (!replay(depthWrites ? writes : readOnly, scale, run, error)) {
                std::printf("hw_scale_display_shadow: scale %u replay failed: %s\n", scale, error.c_str());
                ++failures;
                continue;
            }
            const uint32_t w = base.width, h = base.height, half = scale / 2;
            uint64_t pixelErrors = 0, fromMemory = 0, fromScale = 0, framesWithoutEdges = 0;
            bool sameMemory = true, composed = true;
            for (uint32_t frame = 0; frame < kFrames; ++frame) {
                sameMemory = sameMemory && run.slm[frame] == base.slm[frame] && run.native[frame] == base.native[frame];
                if (frame < kFirstChecked) continue;
                const std::vector<uint32_t>& pixels = run.scaled[frame];
                if (pixels.size() != static_cast<size_t>(w) * h * scale * scale) {
                    composed = false;
                    continue;
                }
                uint64_t mixedBlocks = 0;
                for (uint32_t sy = 0; sy < h * scale; ++sy) {
                    for (uint32_t sx = 0; sx < w * scale; ++sx) {
                        // The native pixel the 1x compose shows here; if the depth writes changed it, memory (1x) wins.
                        // The shadow is used only where neither that pixel nor the one the sub-pixel was drawn for
                        // (06.3) changed.
                        const uint32_t px = sx / scale, py = sy / scale;
                        const uint32_t dx = (std::max(sx, half) - half) / scale, dy = (std::max(sy, half) - half) / scale;
                        const bool changed =
                            rgbAt(base.native[frame], w, px, py) != rgbAt(readOnlyBase.native[frame], w, px, py) ||
                            rgbAt(base.native[frame], w, dx, dy) != rgbAt(readOnlyBase.native[frame], w, dx, dy);
                        uint32_t expected = 0;
                        if (changed) {
                            expected = rgbAt(base.native[frame], w, px, py);
                            ++fromMemory;
                        } else {
                            // 06.3 native mode (hw_scale_exact): sub-pixel (sx, sy) shows native ((sx - h) / s, (sy - h) / s).
                            expected = rgbAt(readOnlyBase.native[frame], w, dx, dy);
                            ++fromScale;
                        }
                        const uint32_t got = pixels[static_cast<size_t>(sy) * w * scale + sx] & 0xFFFFFFu;
                        if (got != expected) {
                            if (pixelErrors < 6) {
                                std::printf("    scale %u %s frame %u sub-pixel %u,%u: %06x, expected %06x (%s)\n", scale,
                                            depthWrites ? "writes" : "read-only", frame, sx, sy, got, expected,
                                            changed ? "memory changed: 1x" : "unchanged: scaled");
                            }
                            ++pixelErrors;
                        }
                    }
                }
                for (uint32_t y = 0; y < h; ++y) {
                    for (uint32_t x = 0; x < w; ++x) {
                        const uint32_t first = pixels[(y * scale) * w * scale + x * scale] & 0xFFFFFFu;
                        bool uniform = true;
                        for (uint32_t dy = 0; dy < scale && uniform; ++dy) {
                            for (uint32_t dx = 0; dx < scale && uniform; ++dx) {
                                uniform = (pixels[(y * scale + dy) * w * scale + x * scale + dx] & 0xFFFFFFu) == first;
                            }
                        }
                        if (!uniform) ++mixedBlocks;
                    }
                }
                if (mixedBlocks == 0) ++framesWithoutEdges;  // a presented frame that is 1x-sourced
            }
            const uint64_t shownChecked = kFrames - kFirstChecked;
            std::printf("  scale %u %s: memory %s, composes %s, sub-pixels off expectation %llu (from memory %llu, at "
                        "scale %llu), 1x-sourced frames %llu, circuits from shadows %llu / slots %llu / GLM %llu, "
                        "shadow saves %llu\n",
                        scale, depthWrites ? "depth writes" : "read-only depth", sameMemory ? "equal" : "DIFFERENT",
                        composed ? "all" : "MISSING", static_cast<unsigned long long>(pixelErrors),
                        static_cast<unsigned long long>(fromMemory), static_cast<unsigned long long>(fromScale),
                        static_cast<unsigned long long>(framesWithoutEdges),
                        static_cast<unsigned long long>(run.counters.scaledCircuitsFromShadows),
                        static_cast<unsigned long long>(run.counters.scaledCircuitsFromSlots),
                        static_cast<unsigned long long>(run.counters.scaledCircuitsFromGlm),
                        static_cast<unsigned long long>(run.counters.scaledShadowSaves));
            if (!sameMemory || !composed || pixelErrors != 0 || framesWithoutEdges != 0 ||
                run.counters.scaledCircuitsFromShadows < shownChecked || (depthWrites && fromMemory == 0) ||
                (!depthWrites && fromMemory != 0)) {
                ++failures;
            }
        }
    }
    std::printf("hw_scale_display_shadow: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_scale_fullscreen_passes (owner test 7, IN-379/IN-380): the two full-screen passes that made GT4's race look 1x
// at every scale, each in a minimal capture, checked on the presented scaled frame against the 06.3 exact expectation
// of the final 1x image (the hw_scale_exact rule: sub-pixel (sx, sy) shows native ((sx - h) / s, (sy - h) / s)).
//   - "copy": the frame is copied into the shown buffer through a REGION_CLAMP texture whose power-of-two size
//     (256 x 128) is larger than the rendered area (100 x 60), as GT4's CT16 conversion does (640 x 448 through
//     1024 x 512). The copy must sample the target's scaled image (IN-380), not the 1x words.
//   - "tone": after the frame is drawn, a tone sequence reads it through an 8-bit (T8) view into a temporary page and
//     writes a result back into the same memory through a CT24 view with T8-textured, FIX-blended sprites. The
//     result is a per-channel function of the frame (Cd - Cd * FIX / 128), like GT4's RGB tone curve, so the scaled
//     image after the sequence must be that function of the scaled image before it (IN-379).
// Memory must equal the 1x run (the 1x words stay the only authority). Both cases fail without the fix.
int TestScaleFullscreenPasses(const gpu::VulkanHandles& handles) {
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xy = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    auto uvRaw = [](uint32_t u16, uint32_t v16) { return uint64_t{u16} | (uint64_t{v16} << 16); };
    const uint64_t kSprite = 6, kTme = 1u << 4, kAbe = 1u << 6, kFst = 1u << 8;
    auto tex0 = [](uint64_t tbpBlocks, uint64_t tbw, uint64_t psm, uint64_t twLog2, uint64_t thLog2, uint64_t cbpBlocks,
                   uint64_t cld) {
        return tbpBlocks | (tbw << 14) | (psm << 20) | (twLog2 << 26) | (thLog2 << 30) | (uint64_t{1} << 34) |
               (uint64_t{1} << 35) | (cbpBlocks << 37) | (cld << 61);  // TCC, TFX decal, CPSM CT32, CSM 0
    };

    struct Case {
        const char* name;
        cpuref::CaptureWriter capture;
        uint32_t width, height;
    };
    std::vector<Case> cases;
    {
        // copy: A = page 0 (CT32, FBW 2, 128 wide), shown buffer B = page 8 (CT32, FBW 2); 100 x 60 picture.
        Case c{"copy", {}, 100, 60};
        c.capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));  // PMODE: RC1, MMOD 1, ALP 255
        c.capture.Privileged(0x12000010u, uint64_t{2} << 13);               // SMODE1: NTSC
        c.capture.Privileged(0x12000070u, uint64_t{8} | (uint64_t{2} << 9));  // DISPFB1: page 8, FBW 2, CT32
        c.capture.Privileged(0x12000080u, (uint64_t{3} << 23) | (uint64_t{399} << 32) | (uint64_t{59} << 44));
        const std::vector<uint64_t> words = {
            (uint64_t{127} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18,
            (uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47,                // TEST: ZTE, ALWAYS
            uint64_t{0} | (uint64_t{2} << 16), 0x4C, uint64_t{30} | (uint64_t{1} << 32), 0x4E,  // FRAME A, ZBUF masked
            kSprite, 0x00, rgbaq(0x80203040u), 0x01, xy(0, 0), 0x05, xy(100, 60), 0x05,
            kSprite, 0x00, rgbaq(0x80E04010u), 0x01, xy(7, 5), 0x05, xy(31, 40), 0x05,
            kSprite, 0x00, rgbaq(0x8010E040u), 0x01, xy(50, 12), 0x05, xy(93, 27), 0x05,
            uint64_t{8} | (uint64_t{2} << 16), 0x4C,                        // FRAME B
            tex0(0, 2, 0x00, 8, 7, 0, 0), 0x06,                             // A as CT32, 256 x 128
            0, 0x14,                                                        // TEX1: nearest
            uint64_t{2} | (uint64_t{2} << 2) | (uint64_t{99} << 14) | (uint64_t{59} << 34), 0x08,  // REGION_CLAMP
            kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uvRaw(8, 8), 0x03, xy(0, 0), 0x05,
            uvRaw(100 * 16 + 8, 60 * 16 + 8), 0x03, xy(100, 60), 0x05};
        c.capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                       words));
        c.capture.FrameEnd(0);
        cases.push_back(std::move(c));
    }
    {
        // tone: A = page 0 (CT32, FBW 1, 64 x 64, shown); temporary T = page 8; a zero T8 texture at page 16 and a
        // zero CLUT at page 20, so every textured source colour is 0 and the blend gives Cd - Cd * 0x50 / 128.
        Case c{"tone", {}, 64, 64};
        c.capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
        c.capture.Privileged(0x12000010u, uint64_t{2} << 13);
        c.capture.Privileged(0x12000070u, uint64_t{0} | (uint64_t{1} << 9));  // DISPFB1: page 0, FBW 1, CT32
        c.capture.Privileged(0x12000080u, (uint64_t{3} << 23) | (uint64_t{255} << 32) | (uint64_t{63} << 44));
        const uint64_t blendFix = 0 | (uint64_t{1} << 2) | (uint64_t{2} << 4) | (uint64_t{1} << 6) | (uint64_t{0x50} << 32);
        const std::vector<uint64_t> words = {
            (uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18,
            (uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47,
            uint64_t{0} | (uint64_t{1} << 16), 0x4C, uint64_t{30} | (uint64_t{1} << 32), 0x4E,  // FRAME A (CT32)
            kSprite, 0x00, rgbaq(0x80406080u), 0x01, xy(0, 0), 0x05, xy(64, 64), 0x05,
            kSprite, 0x00, rgbaq(0x80E04010u), 0x01, xy(5, 7), 0x05, xy(20, 30), 0x05,
            kSprite, 0x00, rgbaq(0x8010E040u), 0x01, xy(33, 2), 0x05, xy(60, 11), 0x05,
            kSprite, 0x00, rgbaq(0x804010E0u), 0x01, xy(10, 40), 0x05, xy(50, 61), 0x05,
            // The sequence: A read through a T8 view into T ...
            uint64_t{8} | (uint64_t{1} << 16), 0x4C,                        // FRAME T
            tex0(0, 2, 0x13, 7, 6, 20 * 32, 1), 0x06,                       // A as T8 (128 x 64), CLUT load
            0, 0x14, 0, 0x08,                                               // nearest, REPEAT
            kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uvRaw(8, 8), 0x03, xy(0, 0), 0x05,
            uvRaw(64 * 16 + 8, 32 * 16 + 8), 0x03, xy(64, 32), 0x05,
            // ... then the result written back into A through a CT24 view, T8-textured and FIX-blended.
            uint64_t{0} | (uint64_t{1} << 16) | (uint64_t{1} << 24), 0x4C,  // FRAME A as CT24
            tex0(16 * 32, 2, 0x13, 7, 6, 20 * 32, 0), 0x06,                 // the zero T8 page
            blendFix, 0x42,
            kSprite | kTme | kAbe | kFst, 0x00, rgbaq(0x80808080u), 0x01, uvRaw(8, 8), 0x03, xy(0, 0), 0x05,
            uvRaw(64 * 16 + 8, 64 * 16 + 8), 0x03, xy(64, 64), 0x05};
        c.capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                       words));
        c.capture.FrameEnd(0);
        cases.push_back(std::move(c));
    }

    struct Run {
        uint64_t slm = 0;
        std::vector<uint8_t> native;
        std::vector<uint32_t> scaled;
        uint32_t width = 0, height = 0;
        writeback::Counters counters;
        gpu::GpuStats stats;
    };
    auto replay = [&](const Case& c, uint32_t scale, Run& run, std::string& error) -> bool {
        writeback::Renderer renderer;
        renderer.SetScale(scale, static_cast<float>(scale));
        if (!renderer.Init(handles, error)) return false;
        renderer.SetSyncMode(true);
        renderer.SetCpuSmallSprites(false);
        writeback::WritebackReplayTarget target(renderer, writeback::ComposeKind::Reference);
        cpuref::ReplaySummary summary;
        bool got = false;
        const bool ok = cpuref::ReplayCapture(c.capture.Bytes(), target, summary,
                                              [&](uint64_t, const cpuref::FrameImage& image) {
                                                  for (const display::Rgba8& p : image.image.pixels) {
                                                      run.native.push_back(p.r);
                                                      run.native.push_back(p.g);
                                                      run.native.push_back(p.b);
                                                  }
                                                  run.width = image.image.width;
                                                  run.height = image.image.height;
                                                  run.slm = cpuref::HashLocalMemory(renderer.Slm(), 4u * 1024u * 1024u);
                                                  writeback::GpuFrame frame;
                                                  got = scale == 1 || (renderer.ComposeFrameGpu(frame) &&
                                                                       renderer.ReadGpuFrame(frame, run.scaled) &&
                                                                       frame.width == scale * image.image.width &&
                                                                       frame.height == scale * image.image.height);
                                              },
                                              error);
        run.counters = renderer.GetCounters();
        run.stats = renderer.GpuCounters();
        if (renderer.Failed()) error += " " + renderer.LastError();
        return ok && got && !renderer.Failed();
    };

    int failures = 0;
    for (const Case& c : cases) {
        Run base;
        std::string error;
        if (!replay(c, 1, base, error) || base.width != c.width || base.height != c.height) {
            std::printf("hw_scale_fullscreen_passes: %s 1x replay failed (%ux%u): %s\n", c.name, base.width, base.height,
                        error.c_str());
            ++failures;
            continue;
        }
        // Blindness: the shown picture has several colours.
        std::vector<uint32_t> colours;
        for (size_t i = 0; i + 2 < base.native.size(); i += 3) {
            const uint32_t v = uint32_t{base.native[i]} | (uint32_t{base.native[i + 1]} << 8) |
                               (uint32_t{base.native[i + 2]} << 16);
            if (std::find(colours.begin(), colours.end(), v) == colours.end()) colours.push_back(v);
        }
        if (colours.size() < 3) {
            std::printf("hw_scale_fullscreen_passes: %s shows %zu colours (fixture is blind)\n", c.name, colours.size());
            ++failures;
        }
        for (uint32_t scale = 2; scale <= 4; scale += 2) {
            Run run;
            if (!replay(c, scale, run, error)) {
                std::printf("hw_scale_fullscreen_passes: %s scale %u replay failed: %s\n", c.name, scale, error.c_str());
                ++failures;
                continue;
            }
            const uint32_t w = base.width, h = base.height, half = scale / 2;
            uint64_t pixelErrors = 0, mixedBlocks = 0;
            for (uint32_t sy = 0; sy < h * scale; ++sy) {
                for (uint32_t sx = 0; sx < w * scale; ++sx) {
                    const uint32_t nx = (std::max(sx, half) - half) / scale;
                    const uint32_t ny = (std::max(sy, half) - half) / scale;
                    const uint8_t* n = &base.native[(static_cast<size_t>(ny) * w + nx) * 3];
                    const uint32_t expected = uint32_t{n[0]} | (uint32_t{n[1]} << 8) | (uint32_t{n[2]} << 16);
                    const uint32_t got = run.scaled[static_cast<size_t>(sy) * w * scale + sx] & 0xFFFFFFu;
                    if (got != expected) {
                        if (pixelErrors < 4) {
                            std::printf("    %s scale %u sub-pixel %u,%u: %06x, expected (native %u,%u) %06x\n", c.name,
                                        scale, sx, sy, got, nx, ny, expected);
                        }
                        ++pixelErrors;
                    }
                }
            }
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    const uint32_t first = run.scaled[(y * scale) * w * scale + x * scale] & 0xFFFFFFu;
                    bool uniform = true;
                    for (uint32_t dy = 0; dy < scale && uniform; ++dy) {
                        for (uint32_t dx = 0; dx < scale && uniform; ++dx) {
                            uniform = (run.scaled[(y * scale + dy) * w * scale + x * scale + dx] & 0xFFFFFFu) == first;
                        }
                    }
                    if (!uniform) ++mixedBlocks;
                }
            }
            const bool sameMemory = run.slm == base.slm && run.native == base.native;
            std::printf("  %s scale %u: memory %s, sub-pixels off the 06.3 expectation %llu, mixed blocks %llu, scaled "
                        "texture copies %llu, tone runs %llu, tone carries %llu\n",
                        c.name, scale, sameMemory ? "equal" : "DIFFERENT", static_cast<unsigned long long>(pixelErrors),
                        static_cast<unsigned long long>(mixedBlocks),
                        static_cast<unsigned long long>(run.stats.scaledTextureCopies),
                        static_cast<unsigned long long>(run.counters.toneRuns),
                        static_cast<unsigned long long>(run.counters.toneCarries));
            const bool toneCase = std::string(c.name) == "tone";
            if (!sameMemory || pixelErrors != 0 || mixedBlocks == 0 ||
                (toneCase ? run.counters.toneCarries == 0 : run.stats.scaledTextureCopies == 0)) {
                ++failures;
            }
        }
    }
    std::printf("hw_scale_fullscreen_passes: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_tone_hle_exact (IN-383, parity plan item 2; chapter 09.4 variant A). The tone sequence's strip template in a
// synthetic capture: three 64 x 32 frame-buffer pages converted through eight decal T8 strips each (page 0 and page 1
// with the sequence's palette bases, page 2 with a palette base that breaks the template), a 64 x 32 temporary page
// of varied CT32 data per page (uploaded, then drawn into so it is GPU-newer and must be swizzled), and an 8 x 32 T8
// index texture of varied bytes. Replayed with the strip dispatch (default) and with the batches themselves
// (SetToneHle(false)). For every frame:
//   - the two 4 MiB local memories are identical: the dispatch is exact against the batches (the 1x and memory gate);
//   - every pixel of pages 0 and 1 equals the CPU form of the strip rule read from the same memory (index texel ->
//     07.4 CSM1 palette entry of the strip's quadrant -> RGB), and every alpha byte is unchanged, so a wrong
//     quadrant, a wrong CSM1 position, a wrong index texel or a wrong destination address fails the fixture;
//   - the dispatch ran for pages 0 and 1 only, and page 2 (refused) still changed memory.
int TestToneHleExact(const gpu::VulkanHandles& handles) {
    constexpr uint32_t kWidth = 64;
    constexpr uint32_t kRows = 32;
    constexpr uint32_t kFrames = 2;
    constexpr uint32_t kDestBp[3] = {0u, 32u, 64u};      // a CT32 page drawn first, then the strips' CT24 view
    constexpr uint32_t kTempBp[2] = {96u, 128u};         // the temporary pages (CT32, FBW 1)
    constexpr uint32_t kIndexBp = 160u;                  // the T8 index texture (TBW 1, 8 x 32)
    static constexpr uint32_t kQuadrantBlock[8] = {0, 4, 16, 20, 8, 12, 24, 28};  // 01.3 block table, 07.4
    static constexpr uint32_t kBranchPage = 2;           // the page whose palette base breaks the template

    // Deterministic pseudo-random words (a fixture, not a generator): both runs get the same capture.
    uint32_t state = 0x12345u;
    auto next = [&state]() {
        state = state * 1664525u + 1013904223u;
        return state;
    };

    cpuref::CaptureWriter capture;
    // A display that shows the first converted page (not compared here).
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
    capture.Privileged(0x12000010u, uint64_t{2} << 13);
    capture.Privileged(0x12000070u, uint64_t{1} << 9);                 // DISPFB1: FBP 0, FBW 1, CT32
    capture.Privileged(0x12000080u, (uint64_t{3} << 23) | (uint64_t{127} << 32) | (uint64_t{63} << 44));

    // Emits one recorded GIF path-1 write (qword aligned).
    const auto emit = [&capture](const std::vector<Qword>& qwords) {
        std::vector<uint8_t> bytes(qwords.size() * sizeof(Qword));
        std::memcpy(bytes.data(), qwords.data(), bytes.size());
        capture.Gif(1, bytes);
    };
    // One host -> local transfer of `bytes` (qword multiple) into (bp, bw, psm) at (x, y), w x h pixels.
    const auto upload = [&emit](uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x, uint32_t y, uint32_t w,
                                uint32_t h, const std::vector<uint8_t>& bytes) {
        std::vector<Qword> qwords;
        qwords.push_back(PackedTag(4, false, 0, {0xE}));  // A+D: four registers
        qwords.push_back(Qword{uint64_t{bp} << 32 | uint64_t{bw} << 48 | uint64_t{psm} << 56, 0x50});  // BITBLTBUF
        qwords.push_back(Qword{uint64_t{x} << 32 | uint64_t{y} << 48, 0x51});                          // TRXPOS
        qwords.push_back(Qword{uint64_t{w} | uint64_t{h} << 32, 0x52});                                // TRXREG
        qwords.push_back(Qword{0, 0x53});                                                             // TRXDIR: host -> local
        const uint32_t count = static_cast<uint32_t>(bytes.size() / 16);
        qwords.push_back(PackedTag(count, true, 2, {}));  // the image packet
        for (uint32_t i = 0; i < count; ++i) {
            Qword data;
            std::memcpy(&data.lo, bytes.data() + i * 16, 8);
            std::memcpy(&data.hi, bytes.data() + i * 16 + 8, 8);
            qwords.push_back(data);
        }
        emit(qwords);
    };

    // --- one frame: uploads, a draw into each page (so the targets are GPU-newer), then the strips -----------------
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        // Temporary pages and index texture: varied CT32 words and varied bytes.
        for (uint32_t t = 0; t < 2; ++t) {
            std::vector<uint8_t> page(kWidth * kRows * 4);
            for (uint32_t word = 0; word < kWidth * kRows; ++word) {
                const uint32_t value = next() | 0xFF000000u;  // alpha 0xFF: the strips must leave it alone
                std::memcpy(page.data() + word * 4, &value, 4);
            }
            upload(kTempBp[t], 1, 0x00, 0, 0, kWidth, kRows, page);
        }
        std::vector<uint8_t> index(kWidth / 8 * kRows);  // 8 x 32 T8 texels
        for (uint8_t& texel : index) texel = static_cast<uint8_t>(next());
        upload(kIndexBp, 1, 0x13, 0, 0, 8, kRows, index);
        // The three destination pages as CT32 words (alpha 0x80), then a flat sprite over each so a resident target
        // holds them (the dispatch parks it in the GLM).
        for (uint32_t page = 0; page < 3; ++page) {
            std::vector<uint8_t> words(kWidth * kRows * 4);
            for (uint32_t word = 0; word < kWidth * kRows; ++word) {
                const uint32_t value = (next() & 0x00FFFFFFu) | (0x80u << 24);
                std::memcpy(words.data() + word * 4, &value, 4);
            }
            upload(kDestBp[page], 1, 0x00, 0, 0, kWidth, kRows, words);
        }
        const auto drawRegs = [&capture](const std::vector<uint64_t>& words) {
            capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                        words));
        };
        const auto xy = [](uint32_t x, uint32_t y) {
            return uint64_t{x * 16} | (uint64_t{y * 16} << 16);  // XYZ2/F2 in 12.4 window coordinates
        };
        drawRegs({uint64_t{63} << 16 | uint64_t{63} << 48, 0x40,       // SCISSOR 0..63 x 0..63
                  (uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47,      // TEST: ZTE, ALWAYS
                  uint64_t{1} << 32, 0x4E,                              // ZBUF: depth writes masked (ZMSK = 1)
                  (uint64_t{6} | (uint64_t{1} << 8)), 0x00,             // sprite, FST
                  0x80808080u | (uint64_t{0x3F800000u} << 32), 0x01,          // RGBAQ: a full grey quad
                  xy(0, 0), 0x05, xy(8, 8), 0x05});
        // A draw into each page over the upload, so a resident target holds it: the dispatch must park it in the GLM
        // (destination pages) or swizzle it (the temporary pages) instead of reading the shadow memory.
        for (uint32_t page = 0; page < 3; ++page) {
            drawRegs({uint64_t{kDestBp[page] / 32u} | (uint64_t{1} << 16), 0x4C,  // FRAME: the page (FBP in pages), CT32
                      0x80808080u | (uint64_t{0x3F800000u} << 32), 0x01, xy(1, 1), 0x05, xy(6, 6), 0x05});
        }
        for (uint32_t t = 0; t < 2; ++t) {
            drawRegs({uint64_t{kTempBp[t] / 32u} | (uint64_t{1} << 16), 0x4C,
                      0x80808080u | (uint64_t{0x3F800000u} << 32), 0x01, xy(2, 2), 0x05, xy(7, 7), 0x05});
        }
        // The strips of one page: eight decal T8 sprites, one texel per pixel, 8 x 32 each.
        const auto strips = [&](uint32_t destinationBp, uint32_t tempBp, bool brokenPalette) {
            for (uint32_t strip = 0; strip < 8; ++strip) {
                const uint32_t quadrant = strip;  // the template's order: quadrant q at strip q
                uint32_t cbp = tempBp + kQuadrantBlock[quadrant];
                if (brokenPalette && strip == 3) cbp += 1;  // not a quadrant base: the page must be refused
                const uint64_t tex0 = uint64_t{kIndexBp} | (uint64_t{1} << 14) | (uint64_t{0x13} << 20) |
                                      (uint64_t{3} << 26) | (uint64_t{5} << 30) | (uint64_t{1} << 34) |
                                      (uint64_t{1} << 35) | (uint64_t{cbp} << 37) | (uint64_t{5} << 61);
                drawRegs({uint64_t{destinationBp / 32u} | (uint64_t{1} << 16) | (uint64_t{1} << 24) |
                              (uint64_t{0xFF} << 56),  // FRAME: CT24, FBMSK 0xFF000000 (alpha protected)
                          0x4C,
                          tex0, 0x06,
                          0, 0x14,  // TEX1: nearest
                          0, 0x08,  // CLAMP: repeat
                          uint64_t{6} | (uint64_t{1} << 4) | (uint64_t{1} << 8), 0x00,  // sprite, textured, FST
                          0x80808080u | (uint64_t{0x3F800000u} << 32), 0x01,
                          uint64_t{0}, 0x03,  // UV (0, 0)
                          xy(strip * 8, 0), 0x05,
                          uint64_t{8 * 16} | (uint64_t{32 * 16} << 16), 0x03,  // UV (8, 32) texels
                          xy(strip * 8 + 8, 32), 0x05});
            }
        };
        strips(kDestBp[0], kTempBp[0], false);
        strips(kDestBp[1], kTempBp[1], false);
        strips(kDestBp[kBranchPage], kTempBp[0], true);
        capture.FrameEnd(frame);
    }

    struct Run {
        std::vector<std::vector<uint8_t>> memory;  // the 4 MiB image per frame
        writeback::Counters counters;
    };
    auto replay = [&](bool toneHle, Run& run, std::string& error) -> bool {
        writeback::Renderer renderer;
        if (!renderer.Init(handles, error)) return false;
        renderer.SetSyncMode(true);
        renderer.SetCpuSmallSprites(false);
        renderer.SetToneHle(toneHle);
        writeback::WritebackReplayTarget target(renderer, writeback::ComposeKind::Reference);
        cpuref::ReplaySummary summary;
        const bool ok = cpuref::ReplayCapture(
            capture.Bytes(), target, summary,
            [&](uint64_t, const cpuref::FrameImage&) {
                run.memory.emplace_back(renderer.Slm(), renderer.Slm() + 4u * 1024u * 1024u);
            },
            error);
        run.counters = renderer.GetCounters();
        if (renderer.Failed()) error += " " + renderer.LastError();
        return ok && !renderer.Failed();
    };

    int failures = 0;
    Run hle, exact;
    std::string error;
    if (!replay(true, hle, error) || !replay(false, exact, error)) {
        std::printf("hw_tone_hle_exact: replay failed: %s\n", error.c_str());
        return 1;
    }
    if (hle.memory.size() != kFrames || exact.memory.size() != kFrames) {
        std::printf("hw_tone_hle_exact: %zu and %zu frames replayed, expected %u\n", hle.memory.size(),
                    exact.memory.size(), kFrames);
        return 1;
    }
    uint64_t differingFrames = 0;
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        differingFrames += hle.memory[frame] != exact.memory[frame] ? 1u : 0u;
    }
    std::printf("  memory: %llu of %u frames differ between the dispatch and the batches\n",
                static_cast<unsigned long long>(differingFrames), kFrames);
    if (differingFrames != 0) ++failures;

    // The dispatch ran for pages 0 and 1 and for page 2's decoded prefix (strips 0..2; strip 3 names a block that is
    // not a palette quadrant and is refused, and the strips after it start at a non-page origin).
    const uint64_t pagesExpected = 3ull * kFrames;
    const uint64_t batchesExpected = 19ull * kFrames;   // 8 + 8 + 3
    const uint64_t deferredExpected = 19ull * kFrames;  // page 2's strips 3..7 are never collected
    std::printf("  counters: pages %llu (expected %llu), batches %llu (expected %llu), deferred %llu (expected %llu)\n",
                static_cast<unsigned long long>(hle.counters.toneStripPages),
                static_cast<unsigned long long>(pagesExpected),
                static_cast<unsigned long long>(hle.counters.toneStripBatches),
                static_cast<unsigned long long>(batchesExpected),
                static_cast<unsigned long long>(hle.counters.toneStripBatchesDeferred),
                static_cast<unsigned long long>(deferredExpected));
    if (hle.counters.toneStripPages != pagesExpected || hle.counters.toneStripBatches != batchesExpected ||
        hle.counters.toneStripBatchesDeferred != deferredExpected) {
        ++failures;
    }
    if (exact.counters.toneStripPages != 0) ++failures;

    // The strip rule read from the same memory (09.4 variant A): index texel -> 07.4 CSM1 palette entry of the
    // strip's quadrant -> RGB, alpha untouched.
    memory::LocalMemory memoryModel;
    uint64_t pixelErrors = 0, alphaErrors = 0, changed = 0;
    const std::vector<uint8_t>& finalMemory = hle.memory[kFrames - 1];
    std::copy(finalMemory.begin(), finalMemory.end(), memoryModel.bytes().begin());
    // The first bytes that differ between the two runs (diagnostic of a coherence or ordering error).
    uint64_t firstDifference = UINT64_MAX;
    uint64_t differingBytes = 0;
    for (size_t i = 0; i < finalMemory.size(); ++i) {
        if (finalMemory[i] != exact.memory[kFrames - 1][i]) {
            if (firstDifference == UINT64_MAX) firstDifference = i;
            ++differingBytes;
        }
    }
    std::printf("  bytes: %llu differ, first at 0x%llx (page %llu)\n", static_cast<unsigned long long>(differingBytes),
                static_cast<unsigned long long>(firstDifference),
                static_cast<unsigned long long>(firstDifference == UINT64_MAX ? 0 : firstDifference / 8192u));
    for (uint32_t page = 0; page < 2; ++page) {
        for (uint32_t strip = 0; strip < 8; ++strip) {
            const uint32_t quadrant = strip;
            for (uint32_t row = 0; row < kRows; ++row) {
                for (uint32_t column = 0; column < 8; ++column) {
                    const uint32_t index =
                        memoryModel.readPixel(memory::BufferView{kIndexBp, 1, memory::Psm::T8}, column, row);
                    const uint32_t entryX = (index & 7u) + 8u * ((index >> 4) & 1u);
                    const uint32_t entryY = 2u * (index >> 5) + ((index >> 3) & 1u);
                    const uint32_t entry = memoryModel.readPixel(
                        memory::BufferView{kTempBp[page], 1, memory::Psm::Ct32}, (quadrant & 3u) * 16u + entryX,
                        (quadrant >> 2) * 16u + entryY);
                    const uint32_t got = memoryModel.readPixel(
                        memory::BufferView{kDestBp[page], 1, memory::Psm::Ct24}, strip * 8u + column, row);
                    // The alpha byte of the same address (a CT24 pixel does not cover it; the CT32 view does).
                    const uint32_t stored = memoryModel.readPixel(
                        memory::BufferView{kDestBp[page], 1, memory::Psm::Ct32}, strip * 8u + column, row);
                    if ((got & 0x00FFFFFFu) != (entry & 0x00FFFFFFu)) {
                        if (pixelErrors < 4) {
                            std::printf("    page %u strip %u pixel %u,%u: %06x, expected %06x\n", page, strip,
                                        strip * 8u + column, row, got & 0x00FFFFFFu, entry & 0x00FFFFFFu);
                        }
                        ++pixelErrors;
                    }
                    if (((stored >> 24) & 0xFFu) != 0x80u) {
                        if (alphaErrors < 5) {
                            std::printf("    alpha page %u pixel %u,%u: %08x\n", page, strip * 8u + column, row, stored);
                        }
                        ++alphaErrors;
                    }
                    if ((got & 0x00FFFFFFu) != 0) ++changed;
                }
            }
        }
    }
    std::printf("  rule: %llu pixels differ, %llu alpha bytes wrong, %llu non-zero pixels\n",
                static_cast<unsigned long long>(pixelErrors), static_cast<unsigned long long>(alphaErrors),
                static_cast<unsigned long long>(changed));
    if (pixelErrors != 0 || alphaErrors != 0 || changed < 2u * 8u * kRows * 8u) ++failures;

    std::printf("hw_tone_hle_exact: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_scale_interlaced_present (IN-381): 480i field-mode frames (FFMD = 1, as GT4's menus) presented at an internal
// scale go through the GPU compose and the GPU deinterlace of 13.3/13.4 counted in native rows.
//   - Exactness: the field buffer is filled by EE uploads (1x words, so its scaled image is every native pixel as an
//     s x s block) with per-field lines and a moving half. For every mode (auto = adaptive, weave, bob, blend) and
//     eight VBlanks of alternating fields, the scaled presentation must equal the CPU presentation compose at 1x
//     (field offset and stateful deinterlacer) blown up by nearest, pixel for pixel.
//   - Detail: a sprite drawn into the field buffer must reach the scaled presentation as sub-pixel detail (mixed
//     s x s blocks), which a 1x presentation (candidate 271 for every 480i frame) cannot have.
int TestScaleInterlacedPresent(const gpu::VulkanHandles& handles) {
    // IN-386: this fixture pins the IN-381 interpretation (one algorithm row = one NATIVE row, so a 1x-block image
    // gives exactly the CPU deinterlacer's result blown up). The shipped default is now one OUTPUT row, the
    // reference's behaviour; the fixture sets the old mode explicitly so both stay covered.
    _putenv_s("GT4_HW_DEINTERLACE_ROWS", "native");
    constexpr uint32_t kWidth = 64;
    constexpr uint32_t kRows = 32;  // field buffer rows; the frame is 64 rows
    auto lineColour = [](uint32_t line, uint32_t x, uint32_t frame) {
        const uint32_t moving = x >= kWidth / 2 ? frame * 0x00302010u : 0u;  // the right half changes every VBlank
        return 0x80000000u | (((line * 7u + 3u) & 0xFFu) | (((line * 13u) & 0xFFu) << 8) | ((x * 3u) << 16)) + moving;
    };
    auto upload = [&](writeback::Renderer& renderer, uint32_t fieldLine, uint32_t frame) {
        std::vector<Qword> stream;
        stream.push_back(PackedTag(4, false, 0, {0xE}));
        stream.push_back(Qword{(uint64_t{1} << 16) | (uint64_t{1} << 48), 0x50});  // BITBLTBUF: DBP 0, DBW 1, CT32
        stream.push_back(Qword{0, 0x51});
        stream.push_back(Qword{uint64_t{kWidth} | (uint64_t{kRows} << 32), 0x52});
        stream.push_back(Qword{0, 0x53});
        const uint32_t qwords = kWidth * kRows / 4;
        stream.push_back(PackedTag(qwords, true, 2, {}));
        for (uint32_t i = 0; i < qwords; ++i) {
            const uint32_t row = (i * 4) / kWidth;
            const uint32_t x = (i * 4) % kWidth;
            const uint32_t c0 = lineColour(2 * row + fieldLine, x, frame);
            const uint32_t c1 = lineColour(2 * row + fieldLine, x + 1, frame);
            const uint32_t c2 = lineColour(2 * row + fieldLine, x + 2, frame);
            const uint32_t c3 = lineColour(2 * row + fieldLine, x + 3, frame);
            stream.push_back(Qword{uint64_t{c0} | (uint64_t{c1} << 32), uint64_t{c2} | (uint64_t{c3} << 32)});
        }
        renderer.GifTransfer(1, reinterpret_cast<const uint8_t*>(stream.data()), stream.size() * sizeof(Qword));
    };
    auto drawSprite = [](writeback::Renderer& renderer) {
        const uint64_t rgbaq = uint64_t{0x8020E0F0u} | (uint64_t{0x3F800000u} << 32);
        const std::vector<std::pair<uint64_t, uint64_t>> ad = {
            {(uint64_t{63} << 16) | (uint64_t{31} << 48), 0x40},             // SCISSOR 64 x 32
            {0, 0x18},                                                       // XYOFFSET 0
            {(uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47},               // TEST: ZTE, ALWAYS
            {uint64_t{0} | (uint64_t{1} << 16), 0x4C},                       // FRAME: page 0, FBW 1, CT32
            {uint64_t{30} | (uint64_t{1} << 32), 0x4E},                      // ZBUF masked
            {6, 0x00},                                                       // PRIM: sprite
            {rgbaq, 0x01},
            {uint64_t{9 * 16} | (uint64_t{5 * 16} << 16), 0x05},
            {uint64_t{40 * 16} | (uint64_t{21 * 16} << 16), 0x05}};
        std::vector<Qword> stream;
        stream.push_back(PackedTag(static_cast<uint32_t>(ad.size()), true, 0, {0xE}));
        for (const auto& [data, reg] : ad) stream.push_back(Qword{data, reg});
        renderer.GifTransfer(1, reinterpret_cast<const uint8_t*>(stream.data()), stream.size() * sizeof(Qword));
    };
    auto setup = [&](writeback::Renderer& renderer, display::DeinterlaceMode mode) {
        writeback::OutputSettings settings;
        settings.deinterlace = mode;
        renderer.SetOutputSettings(settings);
        renderer.WritePrivileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));  // PMODE: RC1, MMOD 1, ALP 255
        renderer.WritePrivileged(0x12000010u, uint64_t{2} << 13);              // SMODE1: NTSC
        renderer.WritePrivileged(0x12000020u, 3u);                             // SMODE2: INT, FFMD
        renderer.WritePrivileged(0x12000060u, 1u);                             // SYNCV: interlaced signal (13.1)
        renderer.WritePrivileged(0x12000070u, uint64_t{1} << 9);               // DISPFB1: FBP 0, FBW 1, CT32
        renderer.WritePrivileged(0x12000080u, (uint64_t{3} << 23) | (uint64_t{4 * kWidth - 1} << 32) |
                                                  (uint64_t{2 * kRows - 1} << 44));  // DISPLAY1: MAGH 3, 64 x 64
    };
    int failures = 0;
    const struct {
        display::DeinterlaceMode mode;
        const char* name;
    } modes[] = {{display::DeinterlaceMode::Auto, "auto (adaptive)"},
                 {display::DeinterlaceMode::Weave, "weave"},
                 {display::DeinterlaceMode::Bob, "bob"},
                 {display::DeinterlaceMode::Blend, "blend"}};
    for (uint32_t scale : {2u, 4u}) {
        for (const auto& m : modes) {
            writeback::Renderer native;
            writeback::Renderer scaled;
            std::string error;
            scaled.SetScale(scale, static_cast<float>(scale));
            if (!native.Init(handles, error) || !scaled.Init(handles, error)) {
                std::printf("hw_scale_interlaced_present: init failed: %s\n", error.c_str());
                return 1;
            }
            native.SetSyncMode(true);
            scaled.SetSyncMode(true);
            setup(native, m.mode);
            setup(scaled, m.mode);
            uint64_t pixelErrors = 0, gpuFrames = 0, compared = 0;
            for (uint32_t frame = 0; frame < 8; ++frame) {
                const bool odd = (frame & 1u) != 0;
                upload(native, odd ? 1u : 0u, frame);
                upload(scaled, odd ? 1u : 0u, frame);
                native.SetDisplayField(odd);
                scaled.SetDisplayField(odd);
                native.Flush();
                scaled.Flush();
                const writeback::FrameOutput reference = native.Compose(writeback::ComposeKind::Presentation);
                writeback::GpuFrame gpuFrame;
                std::vector<uint32_t> pixels;
                if (!scaled.ComposeFrameGpu(gpuFrame) || !scaled.ReadGpuFrame(gpuFrame, pixels)) continue;
                ++gpuFrames;
                if (!reference.valid || gpuFrame.width != reference.width * scale ||
                    gpuFrame.height != reference.height * scale) {
                    std::printf("    %s scale %u frame %u: size %ux%u vs 1x %ux%u\n", m.name, scale, frame,
                                gpuFrame.width, gpuFrame.height, reference.width, reference.height);
                    ++pixelErrors;
                    continue;
                }
                ++compared;
                for (uint32_t y = 0; y < gpuFrame.height; ++y) {
                    for (uint32_t x = 0; x < gpuFrame.width; ++x) {
                        const uint32_t got = pixels[static_cast<size_t>(y) * gpuFrame.width + x] & 0xFFFFFFu;
                        const uint32_t expected =
                            reference.pixels[static_cast<size_t>(y / scale) * reference.width + x / scale] & 0xFFFFFFu;
                        if (got != expected) {
                            if (pixelErrors < 4) {
                                std::printf("    %s scale %u frame %u pixel %u,%u: %06x, expected %06x\n", m.name, scale,
                                            frame, x, y, got, expected);
                            }
                            ++pixelErrors;
                        }
                    }
                }
            }
            std::printf("  %s scale %u: GPU frames %llu, compared %llu, pixels off the 1x deinterlacer %llu, scaled "
                        "deinterlaces %llu\n",
                        m.name, scale, static_cast<unsigned long long>(gpuFrames),
                        static_cast<unsigned long long>(compared), static_cast<unsigned long long>(pixelErrors),
                        static_cast<unsigned long long>(scaled.GpuCounters().scaledDeinterlaces));
            const bool expectsDeinterlace = true;  // every mode here deinterlaces (auto resolves to adaptive: FFMD)
            if (gpuFrames != 8 || compared != 8 || pixelErrors != 0 ||
                (expectsDeinterlace && scaled.GpuCounters().scaledDeinterlaces != 8)) {
                ++failures;
            }
            if (native.Failed() || scaled.Failed()) {
                std::printf("    renderer failed: %s %s\n", native.LastError().c_str(), scaled.LastError().c_str());
                ++failures;
            }
        }
        // Detail: a sprite drawn into the field buffer shows sub-pixel edges at scale.
        writeback::Renderer scaled;
        std::string error;
        scaled.SetScale(scale, static_cast<float>(scale));
        if (!scaled.Init(handles, error)) return 1;
        scaled.SetSyncMode(true);
        scaled.SetCpuSmallSprites(false);
        setup(scaled, display::DeinterlaceMode::Auto);
        uint64_t mixed = 0;
        bool composed = false;
        for (uint32_t frame = 0; frame < 2; ++frame) {
            upload(scaled, frame & 1u, 0);
            drawSprite(scaled);
            scaled.SetDisplayField((frame & 1u) != 0);
            scaled.Flush();
            writeback::GpuFrame gpuFrame;
            std::vector<uint32_t> pixels;
            composed = scaled.ComposeFrameGpu(gpuFrame) && scaled.ReadGpuFrame(gpuFrame, pixels);
            if (!composed) break;
            mixed = 0;
            for (uint32_t by = 0; by < gpuFrame.height / scale; ++by) {
                for (uint32_t bx = 0; bx < gpuFrame.width / scale; ++bx) {
                    const uint32_t first = pixels[static_cast<size_t>(by * scale) * gpuFrame.width + bx * scale] & 0xFFFFFFu;
                    bool uniform = true;
                    for (uint32_t dy = 0; dy < scale && uniform; ++dy) {
                        for (uint32_t dx = 0; dx < scale && uniform; ++dx) {
                            uniform = (pixels[static_cast<size_t>(by * scale + dy) * gpuFrame.width + bx * scale + dx] &
                                       0xFFFFFFu) == first;
                        }
                    }
                    if (!uniform) ++mixed;
                }
            }
        }
        std::printf("  drawn sprite scale %u: GPU compose %s, mixed blocks %llu\n", scale, composed ? "yes" : "NO",
                    static_cast<unsigned long long>(mixed));
        if (!composed || mixed == 0 || scaled.Failed()) ++failures;
    }
    std::printf("hw_scale_interlaced_present: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_core2_present_scale (IN-432): core 2's presentation resampling to the window rectangle. A progressive 64 x 32
// frame at 2x (128 x 64) whose left half is one colour and right half another is resampled up (EASU), down
// (Lanczos-2 per axis), across (down in x, up in y) and at the same size with RCAS, with and without RCAS:
//   - the frame has the requested size and comes from the resampled image;
//   - no channel leaves the range of the two colours without RCAS (no ringing); with RCAS by at most an eighth of
//     the step (sharpening darkens and brightens the two sides of an edge a little, by design);
//   - the left and right columns keep their colour exactly (far from the edge every kernel sees one colour).
int TestCore2PresentScale(const gpu::VulkanHandles& handles) {
    constexpr uint32_t kWidth = 64, kRows = 32;
    constexpr uint32_t kLeft = 0x80203040u, kRight = 0x80C0A060u;
    int failures = 0;
    const struct {
        uint32_t width, height;
        const char* what;
    } sizes[] = {{200, 90, "up (EASU)"}, {100, 40, "down (Lanczos)"}, {96, 100, "down x, up y"}, {128, 64, "same size"}};
    for (const char* sharpness : {"", "0.2"}) {
        _putenv_s("GT4_CORE2_FSR_SHARPNESS", sharpness);
        for (const auto& size : sizes) {
            if (sharpness[0] == 0 && size.width == 128) continue;  // same size, no RCAS: nothing to resample
            _putenv_s("GT4_HW_CORE", "2");
            writeback::Renderer renderer;
            _putenv_s("GT4_HW_CORE", "");
            std::string error;
            renderer.SetScale(2, 2.0f);
            if (!renderer.Init(handles, error)) {
                std::printf("hw_core2_present_scale: init failed: %s\n", error.c_str());
                _putenv_s("GT4_CORE2_FSR_SHARPNESS", "");
                return 1;
            }
            renderer.WritePrivileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));  // PMODE: RC1, MMOD 1, ALP 255
            renderer.WritePrivileged(0x12000010u, uint64_t{2} << 13);              // SMODE1: NTSC
            renderer.WritePrivileged(0x12000070u, uint64_t{1} << 9);               // DISPFB1: FBP 0, FBW 1, CT32
            renderer.WritePrivileged(0x12000080u, (uint64_t{3} << 23) | (uint64_t{4 * kWidth - 1} << 32) |
                                                      (uint64_t{kRows - 1} << 44));  // DISPLAY1: MAGH 3, 64 x 32
            std::vector<Qword> stream;
            stream.push_back(PackedTag(4, false, 0, {0xE}));
            stream.push_back(Qword{(uint64_t{1} << 16) | (uint64_t{1} << 48), 0x50});  // BITBLTBUF: DBP 0, DBW 1, CT32
            stream.push_back(Qword{0, 0x51});
            stream.push_back(Qword{uint64_t{kWidth} | (uint64_t{kRows} << 32), 0x52});
            stream.push_back(Qword{0, 0x53});
            const uint32_t qwords = kWidth * kRows / 4;
            stream.push_back(PackedTag(qwords, true, 2, {}));
            for (uint32_t i = 0; i < qwords; ++i) {
                const uint32_t x = (i * 4) % kWidth;
                const uint64_t c = x < kWidth / 2 ? kLeft : kRight;
                stream.push_back(Qword{c | (c << 32), c | (c << 32)});
            }
            renderer.GifTransfer(1, reinterpret_cast<const uint8_t*>(stream.data()), stream.size() * sizeof(Qword));
            renderer.Flush();
            renderer.SetPresentSize(size.width, size.height);
            writeback::GpuFrame frame;
            std::vector<uint32_t> pixels;
            if (!renderer.ComposeFrameGpu(frame) || !renderer.ReadGpuFrame(frame, pixels)) {
                std::printf("  %s%s: no GPU frame\n", size.what, sharpness[0] ? " + RCAS" : "");
                ++failures;
                continue;
            }
            uint32_t outOfRange = 0, edgeColumns = 0, overshoot = 0;
            const bool sized = frame.presentScaled && frame.width == size.width && frame.height == size.height &&
                               pixels.size() == static_cast<size_t>(size.width) * size.height;
            if (sized) {
                for (uint32_t y = 0; y < size.height; ++y) {
                    for (uint32_t x = 0; x < size.width; ++x) {
                        const uint32_t c = pixels[static_cast<size_t>(y) * size.width + x];
                        for (uint32_t shift = 0; shift < 24; shift += 8) {
                            const uint32_t v = (c >> shift) & 0xFFu, a = (kLeft >> shift) & 0xFFu,
                                           b = (kRight >> shift) & 0xFFu;
                            const uint32_t past = v < std::min(a, b) ? std::min(a, b) - v
                                                  : v > std::max(a, b) ? v - std::max(a, b)
                                                                       : 0u;
                            if (past != 0) ++outOfRange;
                            overshoot = std::max(overshoot, past * 8u > std::max(a, b) - std::min(a, b) ? past : 0u);
                        }
                    }
                    if ((pixels[static_cast<size_t>(y) * size.width] & 0xFFFFFFu) != (kLeft & 0xFFFFFFu)) ++edgeColumns;
                    if ((pixels[static_cast<size_t>(y) * size.width + size.width - 1] & 0xFFFFFFu) != (kRight & 0xFFFFFFu)) {
                        ++edgeColumns;
                    }
                }
            }
            std::printf("  %s%s: %ux%u%s, channels out of range %u (beyond an eighth of the step: max %u), wrong edge columns %u\n", size.what,
                        sharpness[0] ? " + RCAS" : "", frame.width, frame.height, frame.presentScaled ? " resampled" : "",
                        outOfRange, overshoot, edgeColumns);
            const bool ranged = sharpness[0] ? overshoot == 0 : outOfRange == 0;
            if (!sized || !ranged || edgeColumns != 0) ++failures;
        }
    }
    _putenv_s("GT4_CORE2_FSR_SHARPNESS", "");
    std::printf("hw_core2_present_scale: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_core2_interlaced_present (IN-405): core 2's GPU deinterlace of 480i field-mode frames, on the same stream as
// hw_scale_interlaced_present (EE uploads of a 64 x 32 field buffer, per-field lines, a right half that changes every
// VBlank), with one algorithm row = one native row (GT4_HW_DEINTERLACE_ROWS=native) so a 1x-block image has an exact
// 1x answer.
//   - Weave, bob, blend and adaptive rule 5 (GT4_HW_DEINTERLACE_MOVING=reference): the scaled presentation equals
//     the CPU presentation compose at 1x blown up by nearest, pixel for pixel, on all eight VBlanks.
//   - Adaptive (the default, rule 6): the static left half still equals the CPU result once four fields are known
//     (before that every pixel counts as moving), except next to the moving half; on the moving right half
//     every interior missing row is the rounded average of the field rows above and below it (no woven row).
//   - FXAA: with SetFxaa every frame comes from the FXAA image.
int TestCore2InterlacedPresent(const gpu::VulkanHandles& handles) {
    _putenv_s("GT4_HW_DEINTERLACE_ROWS", "native");
    constexpr uint32_t kWidth = 64;
    constexpr uint32_t kRows = 32;  // field buffer rows; the frame is 64 rows
    auto lineColour = [](uint32_t line, uint32_t x, uint32_t frame) {
        const uint32_t moving = x >= kWidth / 2 ? frame * 0x00302010u : 0u;  // the right half changes every VBlank
        return 0x80000000u | (((line * 7u + 3u) & 0xFFu) | (((line * 13u) & 0xFFu) << 8) | ((x * 3u) << 16)) + moving;
    };
    auto upload = [&](writeback::Renderer& renderer, uint32_t fieldLine, uint32_t frame) {
        std::vector<Qword> stream;
        stream.push_back(PackedTag(4, false, 0, {0xE}));
        stream.push_back(Qword{(uint64_t{1} << 16) | (uint64_t{1} << 48), 0x50});  // BITBLTBUF: DBP 0, DBW 1, CT32
        stream.push_back(Qword{0, 0x51});
        stream.push_back(Qword{uint64_t{kWidth} | (uint64_t{kRows} << 32), 0x52});
        stream.push_back(Qword{0, 0x53});
        const uint32_t qwords = kWidth * kRows / 4;
        stream.push_back(PackedTag(qwords, true, 2, {}));
        for (uint32_t i = 0; i < qwords; ++i) {
            const uint32_t row = (i * 4) / kWidth;
            const uint32_t x = (i * 4) % kWidth;
            uint32_t c[4];
            for (uint32_t k = 0; k < 4; ++k) c[k] = lineColour(2 * row + fieldLine, x + k, frame);
            stream.push_back(Qword{uint64_t{c[0]} | (uint64_t{c[1]} << 32), uint64_t{c[2]} | (uint64_t{c[3]} << 32)});
        }
        renderer.GifTransfer(1, reinterpret_cast<const uint8_t*>(stream.data()), stream.size() * sizeof(Qword));
    };
    auto setup = [&](writeback::Renderer& renderer, display::DeinterlaceMode mode) {
        writeback::OutputSettings settings;
        settings.deinterlace = mode;
        renderer.SetOutputSettings(settings);
        renderer.WritePrivileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));  // PMODE: RC1, MMOD 1, ALP 255
        renderer.WritePrivileged(0x12000010u, uint64_t{2} << 13);              // SMODE1: NTSC
        renderer.WritePrivileged(0x12000020u, 3u);                             // SMODE2: INT, FFMD
        renderer.WritePrivileged(0x12000060u, 1u);                             // SYNCV: interlaced signal (13.1)
        renderer.WritePrivileged(0x12000070u, uint64_t{1} << 9);               // DISPFB1: FBP 0, FBW 1, CT32
        renderer.WritePrivileged(0x12000080u, (uint64_t{3} << 23) | (uint64_t{4 * kWidth - 1} << 32) |
                                                  (uint64_t{2 * kRows - 1} << 44));  // DISPLAY1: MAGH 3, 64 x 64
    };
    auto average = [](uint32_t a, uint32_t b) {
        uint32_t out = 0;
        for (uint32_t shift = 0; shift < 24; shift += 8) {
            out |= ((((a >> shift) & 0xFFu) + ((b >> shift) & 0xFFu) + 1u) / 2u) << shift;
        }
        return out;
    };
    int failures = 0;
    const struct {
        display::DeinterlaceMode mode;
        bool reference;  // adaptive rule 5
        bool fxaa;
        const char* name;
    } modes[] = {{display::DeinterlaceMode::Weave, false, false, "weave"},
                 {display::DeinterlaceMode::Bob, false, false, "bob"},
                 {display::DeinterlaceMode::Blend, false, false, "blend"},
                 {display::DeinterlaceMode::Auto, true, false, "adaptive rule 5"},
                 {display::DeinterlaceMode::Auto, false, false, "adaptive rule 6"},
                 {display::DeinterlaceMode::Auto, false, true, "adaptive rule 6 + FXAA"}};
    for (uint32_t scale : {2u, 4u}) {
        for (const auto& m : modes) {
            _putenv_s("GT4_HW_DEINTERLACE_MOVING", m.reference ? "reference" : "");
            _putenv_s("GT4_HW_CORE", "");
            writeback::Renderer native;
            _putenv_s("GT4_HW_CORE", "2");
            writeback::Renderer scaled;
            _putenv_s("GT4_HW_CORE", "");
            std::string error;
            scaled.SetScale(scale, static_cast<float>(scale));
            if (!native.Init(handles, error) || !scaled.Init(handles, error)) {
                std::printf("hw_core2_interlaced_present: init failed: %s\n", error.c_str());
                return 1;
            }
            native.SetSyncMode(true);
            setup(native, m.mode);
            setup(scaled, m.mode);
            scaled.SetFxaa(m.fxaa);
            const bool rule6 = m.mode == display::DeinterlaceMode::Auto && !m.reference;
            uint64_t pixelErrors = 0, averageErrors = 0, compared = 0, fxaaFrames = 0;
            for (uint32_t frame = 0; frame < 8; ++frame) {
                const bool odd = (frame & 1u) != 0;
                upload(native, odd ? 1u : 0u, frame);
                upload(scaled, odd ? 1u : 0u, frame);
                native.SetDisplayField(odd);
                scaled.SetDisplayField(odd);
                native.Flush();
                scaled.Flush();
                const writeback::FrameOutput reference = native.Compose(writeback::ComposeKind::Presentation);
                writeback::GpuFrame gpuFrame;
                std::vector<uint32_t> pixels;
                if (!scaled.ComposeFrameGpu(gpuFrame) || !scaled.ReadGpuFrame(gpuFrame, pixels)) continue;
                if (gpuFrame.fxaa) ++fxaaFrames;
                if (!reference.valid || gpuFrame.width != reference.width * scale ||
                    gpuFrame.height != reference.height * scale) {
                    std::printf("    %s scale %u frame %u: size %ux%u vs 1x %ux%u\n", m.name, scale, frame,
                                gpuFrame.width, gpuFrame.height, reference.width, reference.height);
                    ++pixelErrors;
                    continue;
                }
                ++compared;
                if (m.fxaa) continue;  // FXAA changes edges: only the frame source is checked
                const uint32_t w = gpuFrame.width;
                auto at = [&](uint32_t x, uint32_t y) { return pixels[static_cast<size_t>(y) * w + x] & 0xFFFFFFu; };
                for (uint32_t y = 0; y < gpuFrame.height; ++y) {
                    for (uint32_t x = 0; x < w; ++x) {
                        // Rule 6: the static half once the history is full, away from the two native pixels next to
                        // the moving half (the motion test looks two native pixels left and right).
                        if (rule6 && (frame < 4 || x + 2 * scale >= w / 2)) continue;
                        const uint32_t expected =
                            reference.pixels[static_cast<size_t>(y / scale) * reference.width + x / scale] & 0xFFFFFFu;
                        if (at(x, y) != expected) {
                            if (pixelErrors < 4) {
                                std::printf("    %s scale %u frame %u pixel %u,%u: %06x, expected %06x\n", m.name,
                                            scale, frame, x, y, at(x, y), expected);
                            }
                            ++pixelErrors;
                        }
                    }
                }
                if (!rule6 || frame < 4) continue;  // a full history from the fifth field on
                // The missing native rows are the rows of one parity: every interior one must be the average of the
                // rows s above and below (field rows, passed through). The parity with no error is the missing one.
                uint64_t best = ~uint64_t{0};
                for (uint32_t parity = 0; parity < 2; ++parity) {
                    uint64_t errors = 0;
                    for (uint32_t row = 1; row + 1 < gpuFrame.height / scale; ++row) {
                        if ((row & 1u) != parity) continue;
                        for (uint32_t sub = 0; sub < scale; ++sub) {
                            const uint32_t y = row * scale + sub;
                            for (uint32_t x = w / 2 + scale; x < w; ++x) {
                                if (at(x, y) != average(at(x, y - scale), at(x, y + scale))) ++errors;
                            }
                        }
                    }
                    best = std::min(best, errors);
                }
                averageErrors += best;
            }
            std::printf("  %s scale %u: compared %llu, pixels off the 1x deinterlacer %llu, moving pixels not "
                        "interpolated %llu, FXAA frames %llu, deinterlaced %llu\n",
                        m.name, scale, static_cast<unsigned long long>(compared),
                        static_cast<unsigned long long>(pixelErrors), static_cast<unsigned long long>(averageErrors),
                        static_cast<unsigned long long>(fxaaFrames),
                        static_cast<unsigned long long>(scaled.GetCounters().deinterlacedFrames));
            if (compared != 8 || pixelErrors != 0 || averageErrors != 0 ||
                scaled.GetCounters().deinterlacedFrames != 8 || fxaaFrames != (m.fxaa ? 8u : 0u)) {
                ++failures;
            }
            if (native.Failed() || scaled.Failed()) {
                std::printf("    renderer failed: %s %s\n", native.LastError().c_str(), scaled.LastError().c_str());
                ++failures;
            }
        }
    }
    _putenv_s("GT4_HW_DEINTERLACE_MOVING", "");
    _putenv_s("GT4_HW_DEINTERLACE_ROWS", "");
    std::printf("hw_core2_interlaced_present: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_core2_post_field_compose (TASK-182 R8, step 2c): GT4_CORE2_DEINT_FIELD_COMPOSE=1 has the display merge of a
// deinterlaced frame write straight into that field's deinterlace history image, and reads the last adaptive output from
// the display image, so both full-display image copies the deinterlace used to record disappear; TASK-182 step 2c
// (GT4_CORE2_DEINT_SKIP_DECISION=1) has the adaptive kernel skip its motion tests where the words it already loaded
// decide the saved one. The composed output of the same 480i stream must be identical in all four switch combinations at
// 1x, 2x, 3x and 10x, for the adaptive (rule 6, the default), the weave and the bob modes; a progressive stream (no
// deinterlace: the merge writes the display in every run) must be identical too. The counters must show the path each
// run took: 8 field composes and no copy with the field switch, and no field compose with the copies of the old path
// (one per field, plus the last-output copy of the adaptive modes).
int TestCore2PostFieldCompose(const gpu::VulkanHandles& handles) {
    constexpr uint32_t kWidth = 64;
    constexpr uint32_t kRows = 32;  // field buffer rows; the frame is 64 rows when interlaced
    auto lineColour = [](uint32_t line, uint32_t x, uint32_t frame) {
        // TASK-182 step 2c: rows 4..11 are one flat colour, so the rows above and below a missing row and the
        // woven row hold the same word there and the motion-test skip of the adaptive modes runs (it must save
        // that same word).
        if (line >= 4 && line < 12) return 0x80000000u | 0x005A7B3Cu;
        const uint32_t moving = x >= kWidth / 2 ? frame * 0x00302010u : 0u;  // the right half changes every VBlank
        return 0x80000000u | (((line * 7u + 3u) & 0xFFu) | (((line * 13u) & 0xFFu) << 8) | ((x * 3u) << 16)) + moving;
    };
    auto upload = [&](writeback::Renderer& renderer, uint32_t fieldLine, uint32_t frame) {
        std::vector<Qword> stream;
        stream.push_back(PackedTag(4, false, 0, {0xE}));
        stream.push_back(Qword{(uint64_t{1} << 16) | (uint64_t{1} << 48), 0x50});  // BITBLTBUF: DBP 0, DBW 1, CT32
        stream.push_back(Qword{0, 0x51});
        stream.push_back(Qword{uint64_t{kWidth} | (uint64_t{kRows} << 32), 0x52});
        stream.push_back(Qword{0, 0x53});
        const uint32_t qwords = kWidth * kRows / 4;
        stream.push_back(PackedTag(qwords, true, 2, {}));
        for (uint32_t i = 0; i < qwords; ++i) {
            const uint32_t row = (i * 4) / kWidth;
            const uint32_t x = (i * 4) % kWidth;
            uint32_t c[4];
            for (uint32_t k = 0; k < 4; ++k) c[k] = lineColour(2 * row + fieldLine, x + k, frame);
            stream.push_back(Qword{uint64_t{c[0]} | (uint64_t{c[1]} << 32), uint64_t{c[2]} | (uint64_t{c[3]} << 32)});
        }
        renderer.GifTransfer(1, reinterpret_cast<const uint8_t*>(stream.data()), stream.size() * sizeof(Qword));
    };
    auto setup = [&](writeback::Renderer& renderer, display::DeinterlaceMode mode, bool interlaced) {
        writeback::OutputSettings settings;
        settings.deinterlace = mode;
        renderer.SetOutputSettings(settings);
        renderer.WritePrivileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));  // PMODE: RC1, MMOD 1, ALP 255
        renderer.WritePrivileged(0x12000010u, uint64_t{2} << 13);              // SMODE1: NTSC
        renderer.WritePrivileged(0x12000020u, interlaced ? 3u : 0u);           // SMODE2: INT and FFMD when interlaced
        renderer.WritePrivileged(0x12000060u, interlaced ? 1u : 0u);           // SYNCV: an interlaced signal (13.1)
        renderer.WritePrivileged(0x12000070u, uint64_t{1} << 9);               // DISPFB1: FBP 0, FBW 1, CT32
        renderer.WritePrivileged(0x12000080u, (uint64_t{3} << 23) | (uint64_t{4 * kWidth - 1} << 32) |
                                                  (uint64_t{2 * kRows - 1} << 44));  // DISPLAY1: MAGH 3, 64 x 64
    };
    struct PostRun {
        std::vector<std::vector<uint32_t>> frames;
        std::uint64_t deinterlaced = 0, fieldComposes = 0, copies = 0;
        std::uint32_t width = 0, height = 0;
    };
    auto run = [&](uint32_t scale, display::DeinterlaceMode mode, bool interlaced, bool on, bool skip, PostRun& out) {
        _putenv_s("GT4_HW_DEINTERLACE_ROWS", "native");
        _putenv_s("GT4_HW_DEINTERLACE_MOVING", "");
        _putenv_s("GT4_CORE2_DEINT_FIELD_COMPOSE", on ? "1" : "0");
        _putenv_s("GT4_CORE2_DEINT_SKIP_DECISION", skip ? "1" : "0");
        _putenv_s("GT4_HW_CORE", "2");
        writeback::Renderer renderer;
        _putenv_s("GT4_HW_CORE", "");
        std::string error;
        renderer.SetScale(scale, static_cast<float>(scale));
        const bool ready = renderer.Init(handles, error);  // reads both TASK-182 switches
        _putenv_s("GT4_CORE2_DEINT_FIELD_COMPOSE", "");
        _putenv_s("GT4_CORE2_DEINT_SKIP_DECISION", "");
        if (!ready) {
            std::printf("  init failed: %s\n", error.c_str());
            return false;
        }
        setup(renderer, mode, interlaced);
        for (uint32_t frame = 0; frame < 8; ++frame) {
            const bool odd = (frame & 1u) != 0;
            upload(renderer, odd ? 1u : 0u, frame);
            renderer.SetDisplayField(odd);
            renderer.Flush();
            writeback::GpuFrame gpuFrame;
            std::vector<std::uint32_t> pixels;
            if (!renderer.ComposeFrameGpu(gpuFrame) || !gpuFrame.valid || !renderer.ReadGpuFrame(gpuFrame, pixels)) {
                std::printf("  compose failed: %s\n", renderer.LastError().c_str());
                return false;
            }
            out.width = gpuFrame.width;
            out.height = gpuFrame.height;
            out.frames.push_back(std::move(pixels));
        }
        if (renderer.Failed()) {
            std::printf("  renderer failed: %s\n", renderer.LastError().c_str());
            return false;
        }
        out.deinterlaced = renderer.GetCounters().deinterlacedFrames;
        const core2::Core2Counters* counters = renderer.Core2Stats();
        if (!counters) return false;
        out.fieldComposes = counters->postFieldComposes;
        out.copies = counters->postCopies;
        return true;
    };
    int failures = 0;
    const struct {
        display::DeinterlaceMode mode;
        bool interlaced;
        std::uint64_t copiesPerField;  // the old path: the history copy, plus the last-output copy of modes 5 and 6
        const char* name;
    } cases[] = {{display::DeinterlaceMode::Auto, true, 2, "adaptive rule 6"},
                 {display::DeinterlaceMode::Weave, true, 1, "weave"},
                 {display::DeinterlaceMode::Bob, true, 1, "bob"},
                 {display::DeinterlaceMode::Auto, false, 0, "progressive (no deinterlace)"}};
    auto compare = [](const PostRun& a, const PostRun& b, std::uint64_t& compared, std::uint64_t& differing) {
        compared = differing = 0;
        if (a.frames.size() != b.frames.size()) {
            differing = 1;
            return;
        }
        for (std::size_t frame = 0; frame < a.frames.size(); ++frame) {
            if (a.frames[frame].size() != b.frames[frame].size()) {
                ++differing;
                continue;
            }
            for (std::size_t i = 0; i < a.frames[frame].size(); ++i) {
                ++compared;
                if (a.frames[frame][i] != b.frames[frame][i]) ++differing;
            }
        }
    };
    // TASK-182 step 2c adds the third and fourth runs: the motion-test skip alone, and both TASK-182 switches. All four
    // orders must produce the same composed frames as today's (off, off), at 10x too.
    for (uint32_t scale : {1u, 2u, 3u, 10u}) {
        for (const auto& c : cases) {
            PostRun base, fieldOnly, skipOnly, both;
            const bool ok = run(scale, c.mode, c.interlaced, false, false, base) &&
                            run(scale, c.mode, c.interlaced, true, false, fieldOnly) &&
                            run(scale, c.mode, c.interlaced, false, true, skipOnly) &&
                            run(scale, c.mode, c.interlaced, true, true, both);
            std::uint64_t compared = 0, differing = 0, compared2 = 0, differing2 = 0, compared3 = 0, differing3 = 0;
            compare(base, fieldOnly, compared, differing);
            compare(base, skipOnly, compared2, differing2);
            compare(base, both, compared3, differing3);
            const std::uint64_t fieldFrames = c.interlaced ? 8 : 0;
            const bool counters = fieldOnly.fieldComposes == fieldFrames && both.fieldComposes == fieldFrames &&
                                  base.fieldComposes == 0 && skipOnly.fieldComposes == 0 && fieldOnly.copies == 0 &&
                                  both.copies == 0 && base.copies == 8 * c.copiesPerField &&
                                  skipOnly.copies == 8 * c.copiesPerField && base.deinterlaced == both.deinterlaced &&
                                  base.deinterlaced == skipOnly.deinterlaced;
            std::printf("  %s at %ux: field compose %llu, skip %llu, both %llu differing of %llu, size %ux%u, "
                        "deinterlaced %llu/%llu, field composes %llu/%llu, display copies %llu/%llu\n",
                        c.name, scale, static_cast<unsigned long long>(differing),
                        static_cast<unsigned long long>(differing2), static_cast<unsigned long long>(differing3),
                        static_cast<unsigned long long>(compared), base.width, base.height,
                        static_cast<unsigned long long>(base.deinterlaced),
                        static_cast<unsigned long long>(both.deinterlaced),
                        static_cast<unsigned long long>(base.fieldComposes),
                        static_cast<unsigned long long>(both.fieldComposes),
                        static_cast<unsigned long long>(base.copies), static_cast<unsigned long long>(both.copies));
            if (!ok || differing != 0 || differing2 != 0 || differing3 != 0 || compared == 0 || !counters ||
                base.width != both.width || base.height != both.height) {
                ++failures;
            }
        }
    }
    _putenv_s("GT4_HW_DEINTERLACE_MOVING", "");
    _putenv_s("GT4_HW_DEINTERLACE_ROWS", "");
    std::printf("hw_core2_post_field_compose: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// ---- Core 2 A/B fixtures (IN-407, IN-408) ------------------------------------------------------------------------
// Core 2 at 2x replays one stream twice, with an A/B switch (an environment variable core 2 reads when it is built)
// set to "0" and left unset, and compares both runs with each other and with the CPU reference renderer (1x, the GS
// semantics): every frame's composed image must be equal in both runs, and the run with the switch on must never
// move GS memory away from the reference (a word may differ between the runs only where the run with the switch off
// is already off the reference: at scale, a target's undrawn first sub-pixel row and column, sampled as a texture at
// its edge, hold the image's earlier content). `exact` asks for the reference itself in both runs. `memoryLastOnly`
// reads GS memory at the last frame only (reading it stores every drawn target, which changes what follows).
struct Core2AbRun {
    std::vector<std::vector<uint8_t>> images;
    std::vector<std::vector<uint8_t>> memory;
    core2::Core2Counters stats;
    core2::Core2GpuStats gpu;  // TASK-180 test4 (P1-C): data paths, ring high-water marks, host-read barriers
};

constexpr size_t kGsMemoryBytes = 4u * 1024u * 1024u;

// `onValue` is the switch's value for the "on" run ("" = its default); "0" turns it off. `extraEnv` holds further
// variables set for both runs before the renderer reads them (name, value); cleared afterwards.
using EnvPairs = std::vector<std::pair<const char*, const char*>>;

void ApplyEnv(const EnvPairs& env) {
    for (const auto& [name, value] : env) _putenv_s(name, value);
}

void ClearEnv(const EnvPairs& env) {
    for (const auto& [name, value] : env) _putenv_s(name, "");
}

bool ReplayCore2Ab(const gpu::VulkanHandles& handles, const cpuref::CaptureWriter& capture, const char* abSwitch,
                   bool on, Core2AbRun& out, std::string& error, uint32_t memoryFromFrame = 0, uint32_t scale = 2,
                   const std::function<void(writeback::Renderer&)>& configure = {}, const char* onValue = "",
                   const EnvPairs& extraEnv = {}) {
    _putenv_s(abSwitch, on ? onValue : "0");
    ApplyEnv(extraEnv);
    _putenv_s("GT4_HW_CORE", "2");
    writeback::Renderer renderer;
    _putenv_s("GT4_HW_CORE", "");
    renderer.SetScale(scale, static_cast<float>(scale));
    const bool ready = renderer.Init(handles, error);  // some switches are read here
    _putenv_s(abSwitch, "");
    ClearEnv(extraEnv);
    if (!ready) return false;
    if (configure) configure(renderer);
    if (!renderer.Core2()) {
        error = "GT4_HW_CORE=2 did not select core 2";
        return false;
    }
    writeback::WritebackReplayTarget target(renderer);
    cpuref::ReplaySummary summary;
    const bool ok = cpuref::ReplayCapture(capture.Bytes(), target, summary,
                                          [&](uint64_t, const cpuref::FrameImage& image) {
                                              std::vector<uint8_t> rgb;
                                              for (const display::Rgba8& p : image.image.pixels) {
                                                  rgb.push_back(p.r);
                                                  rgb.push_back(p.g);
                                                  rgb.push_back(p.b);
                                              }
                                              out.images.push_back(std::move(rgb));
                                              if (out.images.size() > memoryFromFrame) {
                                                  const uint8_t* slm = renderer.Slm();
                                                  out.memory.emplace_back(slm, slm + kGsMemoryBytes);
                                              } else {
                                                  out.memory.emplace_back();
                                              }
                                          },
                                          error);
    if (const core2::Core2Counters* stats = renderer.Core2Stats()) out.stats = *stats;
    if (const core2::Core2GpuStats* gpu = renderer.Core2GpuCounters()) out.gpu = *gpu;
    if (renderer.Failed()) error += " " + renderer.LastError();
    return ok && !renderer.Failed();
}

// Returns the number of failures; `counters` checks the two runs' counters (switch off, switch on).
int CheckCore2Ab(const gpu::VulkanHandles& handles, const char* name, const cpuref::CaptureWriter& capture,
                 uint32_t frames, const char* abSwitch, bool exact,
                 const std::function<int(const Core2AbRun& off, const Core2AbRun& on)>& counters,
                 bool memoryLastOnly = false, uint32_t scale = 2, const char* onValue = "",
                 const EnvPairs& extraEnv = {}) {
    Core2AbRun off, on;
    std::vector<std::vector<uint8_t>> reference;
    std::string error;
    const uint32_t memoryFrom = memoryLastOnly ? frames - 1 : 0;
    bool ok = ReplayCore2Ab(handles, capture, abSwitch, false, off, error, memoryFrom, scale, {}, onValue, extraEnv) &&
              ReplayCore2Ab(handles, capture, abSwitch, true, on, error, memoryFrom, scale, {}, onValue, extraEnv);
    if (ok) {
        cpuref::CpuRenderer renderer;
        cpuref::ReplaySummary summary;
        ok = cpuref::ReplayCapture(capture.Bytes(), renderer, summary,
                                   [&](uint64_t, const cpuref::FrameImage&) {
                                       const auto& bytes = renderer.Memory().Local().bytes();
                                       reference.emplace_back(bytes.begin(), bytes.begin() + kGsMemoryBytes);
                                   },
                                   error);
    }
    if (!ok) {
        std::printf("  %s: replay failed: %s\n", name, error.c_str());
        return 1;
    }
    if (off.images.size() != frames || on.images.size() != frames || reference.size() != frames) {
        std::printf("  %s: frame counts %zu / %zu / %zu\n", name, off.images.size(), on.images.size(), reference.size());
        return 1;
    }
    int failures = 0;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        const bool image = !off.images[frame].empty() && off.images[frame] == on.images[frame];
        uint64_t differing = 0, newDeviations = 0, offDeviations = 0, onDeviations = 0;
        const size_t memoryBytes = off.memory[frame].empty() || on.memory[frame].empty() ? 0 : kGsMemoryBytes;
        for (size_t at = 0; at < memoryBytes; at += 4) {
            uint32_t a = 0, b = 0, r = 0;
            std::memcpy(&a, off.memory[frame].data() + at, 4);
            std::memcpy(&b, on.memory[frame].data() + at, 4);
            std::memcpy(&r, reference[frame].data() + at, 4);
            if (a != r) ++offDeviations;
            if (b != r) ++onDeviations;
            if (a == b) continue;
            ++differing;
            if (b != r && (a == r || exact)) {
                if (newDeviations < 4) {
                    std::printf("    %s frame %u byte %zu: switch off %08x, on %08x, reference %08x\n", name, frame, at,
                                a, b, r);
                }
                ++newDeviations;
            }
        }
        std::printf("  %s frame %u: image %s; memory words differing %llu, of which new deviations %llu "
                    "(words off the reference: switch off %llu, on %llu)\n",
                    name, frame, image ? "equal" : "DIFFERENT", static_cast<unsigned long long>(differing),
                    static_cast<unsigned long long>(newDeviations), static_cast<unsigned long long>(offDeviations),
                    static_cast<unsigned long long>(onDeviations));
        if (!image || newDeviations != 0 || (exact && (offDeviations != 0 || onDeviations != 0))) ++failures;
    }
    failures += counters(off, on);
    return failures;
}

// GT4's palette pattern (IN-407): a 16 x 16 palette drawn as untextured sprites with no depth test, a CLUT load from it
// right away and a T8 sprite through it, every frame with new colours.
cpuref::CaptureWriter Core2PaletteSpritesCapture(uint32_t frames) {
    cpuref::CaptureWriter palettes;
    constexpr uint64_t kA = 0, kT8 = 12, kClut = 14;  // pages
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xyz = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    auto uv = [](uint32_t u, uint32_t v) { return uint64_t{u * 16} | (uint64_t{v * 16} << 16); };
    auto frameReg = [](uint64_t page) { return page | (uint64_t{1} << 16); };  // FBW 1, CT32
    const uint64_t kSprite = 6, kIip = 1u << 3, kTme = 1u << 4, kFst = 1u << 8;
    const uint64_t t8 = kT8 * 32 | (uint64_t{1} << 14) | (uint64_t{0x13} << 20) | (uint64_t{6} << 26) |
                        (uint64_t{6} << 30) | (uint64_t{1} << 34) | (uint64_t{kClut * 32} << 37) |
                        (uint64_t{1} << 61);  // T8 64 x 64, CT32 palette at kClut, CSM1, load
    palettes.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
    palettes.Privileged(0x12000010u, uint64_t{2} << 13);
    palettes.Privileged(0x12000070u, uint64_t{kA * 32} | (uint64_t{1} << 9));
    palettes.Privileged(0x12000080u, (uint64_t{63} << 32) | (uint64_t{63} << 44));
    auto ad = [&](const std::vector<uint64_t>& words) {
        palettes.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                      words));
    };
    // The T8 indices (64 x 64): index = (x + 5y) & 255.
    ad({uint64_t{kT8 * 32} << 32 | (uint64_t{1} << 48) | (uint64_t{0x13} << 56), 0x50, 0, 0x51,
        uint64_t{64} | (uint64_t{64} << 32), 0x52, 0, 0x53});
    {
        std::vector<uint64_t> data;
        for (uint32_t i = 0; i < 64 * 64 / 8; ++i) {
            uint64_t word = 0;
            for (uint32_t b = 0; b < 8; ++b) {
                const uint32_t p = i * 8 + b;
                word |= uint64_t{((p % 64) + 5 * (p / 64)) & 255u} << (8 * b);
            }
            data.push_back(word);
        }
        palettes.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(data.size() / 2), true, false, 0, 2, {},
                                                      data));
    }
    for (uint32_t frame = 0; frame < frames; ++frame) {
        ad({(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18, uint64_t{1} << 17, 0x47,  // TEST: ZTST ALWAYS
            uint64_t{1} << 32, 0x4E,                                                             // ZBUF masked
            // the palette: two flat sprites (the halves of the 16 x 16 CSM1 block) in new colours
            frameReg(kClut), 0x4C, kSprite, 0x00, rgbaq(0x80000000u | (0x102030u * (frame + 1))), 0x01, xyz(0, 0), 0x05,
            xyz(16, 8), 0x05, kSprite | kIip, 0x00, rgbaq(0x80FFFFFFu - 0x030507u * frame), 0x01, xyz(0, 8), 0x05,
            xyz(16, 16), 0x05,
            // a clear of A, then the T8 sprite through the palette just drawn
            frameReg(kA), 0x4C, kSprite, 0x00, rgbaq(0x80000000u), 0x01, xyz(0, 0), 0x05, xyz(64, 64), 0x05, t8, 0x06,
            kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(0, 0), 0x05, uv(64, 64), 0x03,
            xyz(64, 64), 0x05});
        palettes.FrameEnd(frame);
    }
    return palettes;
}

// GT4's GPU palettes (IN-408, the night race): a palette drawn on the GPU from a texture (so the CPU sprite path cannot
// take it) and loaded right away as a T8 CLUT; then a T4 CLUT loaded from memory into group 1 (CSA 1); then the T8
// palette used again without a load, so its groups come from both loads; then a CT32 target drawn on the GPU read
// back as a T8 texture (GT4 reads its 128 x 128 night render that way), through the same palette.
cpuref::CaptureWriter Core2GpuPaletteCapture(uint32_t frames) {
    cpuref::CaptureWriter capture;
    constexpr uint64_t kA = 0, kT8 = 12, kClut = 14, kSource = 16, kClut4 = 18, kT4 = 20, kRender = 22;  // pages
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xyz = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    auto uv = [](uint32_t u, uint32_t v) { return uint64_t{u * 16} | (uint64_t{v * 16} << 16); };
    auto frameReg = [](uint64_t page) { return page | (uint64_t{1} << 16); };  // FBW 1, CT32
    auto blit = [](uint64_t dbp, uint64_t dpsm) { return (dbp << 32) | (uint64_t{1} << 48) | (dpsm << 56); };
    const uint64_t kSprite = 6, kTme = 1u << 4, kFst = 1u << 8;
    // TEX0: base, TBW 1, PSM, TW/TH log2, TCC, TFX (0 modulate, 1 decal), CBP, CPSM CT32, CSM1, CSA, CLD.
    auto tex0 = [](uint64_t blocks, uint64_t psm, uint64_t log2, uint64_t tfx, uint64_t cbpBlocks, uint64_t csa,
                   bool load) {
        return blocks | (uint64_t{1} << 14) | (psm << 20) | (log2 << 26) | (log2 << 30) | (uint64_t{1} << 34) |
               (tfx << 35) | (cbpBlocks << 37) | (csa << 56) | (uint64_t{load ? 1u : 0u} << 61);
    };
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
    capture.Privileged(0x12000010u, uint64_t{2} << 13);
    capture.Privileged(0x12000070u, uint64_t{kA * 32} | (uint64_t{1} << 9));
    capture.Privileged(0x12000080u, (uint64_t{63} << 32) | (uint64_t{63} << 44));
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    auto image = [&](const std::vector<uint64_t>& data) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(data.size() / 2), true, false, 0, 2, {}, data));
    };
    // T8 indices (64 x 64, index = (x + 5y) & 255) and T4 indices (64 x 64, index = (x + y) & 15).
    ad({blit(kT8 * 32, 0x13), 0x50, 0, 0x51, uint64_t{64} | (uint64_t{64} << 32), 0x52, 0, 0x53});
    {
        std::vector<uint64_t> data;
        for (uint32_t i = 0; i < 64 * 64 / 8; ++i) {
            uint64_t word = 0;
            for (uint32_t b = 0; b < 8; ++b) {
                const uint32_t p = i * 8 + b;
                word |= uint64_t{((p % 64) + 5 * (p / 64)) & 255u} << (8 * b);
            }
            data.push_back(word);
        }
        image(data);
    }
    ad({blit(kT4 * 32, 0x14), 0x50, 0, 0x51, uint64_t{64} | (uint64_t{64} << 32), 0x52, 0, 0x53});
    {
        std::vector<uint64_t> data;
        for (uint32_t i = 0; i < 64 * 64 / 16; ++i) {
            uint64_t word = 0;
            for (uint32_t n = 0; n < 16; ++n) {
                const uint32_t p = i * 16 + n;
                word |= uint64_t{((p % 64) + (p / 64)) & 15u} << (4 * n);
            }
            data.push_back(word);
        }
        image(data);
    }
    for (uint32_t frame = 0; frame < frames; ++frame) {
        // The palette's source texture (16 x 16 CT32), new every frame.
        ad({blit(kSource * 32, 0x00), 0x50, 0, 0x51, uint64_t{16} | (uint64_t{16} << 32), 0x52, 0, 0x53});
        {
            std::vector<uint64_t> texels;
            for (uint32_t i = 0; i < 16 * 16 / 2; ++i) {
                const uint32_t lo = 0x80000000u | ((i * 0x050301u + frame * 0x101010u) & 0xFFFFFFu);
                const uint32_t hi = 0x40000000u | ((i * 0x010507u + frame * 0x0F0F0Fu) & 0xFFFFFFu);
                texels.push_back(uint64_t{lo} | (uint64_t{hi} << 32));
            }
            image(texels);
        }
        // The T4 palette in memory (8 x 2 CT32, CSM1), new every frame.
        ad({blit(kClut4 * 32, 0x00), 0x50, 0, 0x51, uint64_t{8} | (uint64_t{2} << 32), 0x52, 0, 0x53});
        {
            std::vector<uint64_t> entries;
            for (uint32_t i = 0; i < 8; ++i) {
                entries.push_back((uint64_t{0x80F00000u + 0x1020u * i + frame} << 32) | (0x800000F0u + 0x10200u * i));
            }
            image(entries);
        }
        ad({(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18, uint64_t{1} << 17, 0x47,  // TEST: ZTST ALWAYS
            uint64_t{1} << 32, 0x4E,                                                             // ZBUF masked
            // the palette drawn on the GPU: the source texture copied (decal) into the 16 x 16 palette block
            frameReg(kClut), 0x4C, tex0(kSource * 32, 0x00, 4, 1, 0, 0, false), 0x06, kSprite | kTme | kFst, 0x00,
            rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(0, 0), 0x05, uv(16, 16), 0x03, xyz(16, 16), 0x05,
            // a clear of A, then the T8 sprite through the palette just drawn (a CLUT load from the target)
            frameReg(kA), 0x4C, kSprite, 0x00, rgbaq(0x80000000u), 0x01, xyz(0, 0), 0x05, xyz(64, 64), 0x05,
            tex0(kT8 * 32, 0x13, 6, 0, kClut * 32, 0, true), 0x06, kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u),
            0x01, uv(0, 0), 0x03, xyz(0, 0), 0x05, uv(32, 64), 0x03, xyz(32, 64), 0x05,
            // a T4 load from memory into group 1, and a T4 sprite through it
            tex0(kT4 * 32, 0x14, 6, 0, kClut4 * 32, 1, true), 0x06, kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u),
            0x01, uv(0, 0), 0x03, xyz(32, 0), 0x05, uv(32, 32), 0x03, xyz(64, 32), 0x05,
            // the T8 palette again without a load: groups 0 and 2..15 from the target, group 1 from memory
            tex0(kT8 * 32, 0x13, 6, 0, kClut * 32, 0, false), 0x06, kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u),
            0x01, uv(32, 0), 0x03, xyz(32, 32), 0x05, uv(64, 32), 0x03, xyz(64, 64), 0x05,
            // a CT32 render on the GPU (the source texture stretched over 32 x 32), then read as a T8 texture
            frameReg(kRender), 0x4C, tex0(kSource * 32, 0x00, 4, 1, 0, 0, false), 0x06, kSprite | kTme | kFst, 0x00,
            rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(0, 0), 0x05, uv(16, 16), 0x03, xyz(32, 32), 0x05,
            frameReg(kA), 0x4C, tex0(kRender * 32, 0x13, 5, 0, kClut * 32, 0, false), 0x06, kSprite | kTme | kFst,
            0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(0, 40), 0x05, uv(24, 24), 0x03, xyz(24, 64), 0x05});
        capture.FrameEnd(frame);
    }
    return capture;
}

// IN-408 ageing (owner report, GT4's title after a GT Mode visit): a CT32 target drawn once on the GPU and then only
// read through GS memory (a T8 texture over it, every frame) must still age out after 60 unused frames (6.2), its
// drawn pixels stored to memory first since no readback ever did; then an EE upload in the target's own view
// replaces part of that memory and the T8 sprite must show the upload.
cpuref::CaptureWriter Core2GpuReadAgeingCapture(uint32_t frames) {
    cpuref::CaptureWriter capture;
    constexpr uint64_t kA = 0, kSource = 16, kRender = 22;  // pages
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xyz = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    auto uv = [](uint32_t u, uint32_t v) { return uint64_t{u * 16} | (uint64_t{v * 16} << 16); };
    auto frameReg = [](uint64_t page) { return page | (uint64_t{1} << 16); };  // FBW 1, CT32
    auto blit = [](uint64_t dbp, uint64_t dpsm) { return (dbp << 32) | (uint64_t{1} << 48) | (dpsm << 56); };
    const uint64_t kSprite = 6, kTme = 1u << 4, kFst = 1u << 8;
    auto tex0 = [](uint64_t blocks, uint64_t psm, uint64_t log2, uint64_t tfx) {
        return blocks | (uint64_t{1} << 14) | (psm << 20) | (log2 << 26) | (log2 << 30) | (uint64_t{1} << 34) |
               (tfx << 35);
    };
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
    capture.Privileged(0x12000010u, uint64_t{2} << 13);
    capture.Privileged(0x12000070u, uint64_t{kA * 32} | (uint64_t{1} << 9));
    capture.Privileged(0x12000080u, (uint64_t{63} << 32) | (uint64_t{63} << 44));
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    auto image = [&](const std::vector<uint64_t>& data) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(data.size() / 2), true, false, 0, 2, {}, data));
    };
    // A grey ramp palette at page 14 (CT32 16 x 16), uploaded: the T8 sprite shows index values as greys.
    ad({blit(14 * 32, 0x00), 0x50, 0, 0x51, uint64_t{16} | (uint64_t{16} << 32), 0x52, 0, 0x53});
    {
        std::vector<uint64_t> entries;
        for (uint32_t i = 0; i < 128; ++i) {
            const uint32_t a = 2 * i, b = 2 * i + 1;  // CSM1 order does not matter for a check against the reference
            entries.push_back(uint64_t{0x80000000u | (a * 0x010101u)} | (uint64_t{0x80000000u | (b * 0x010101u)} << 32));
        }
        image(entries);
    }
    // The source texture (16 x 16 CT32).
    ad({blit(kSource * 32, 0x00), 0x50, 0, 0x51, uint64_t{16} | (uint64_t{16} << 32), 0x52, 0, 0x53});
    {
        std::vector<uint64_t> texels;
        for (uint32_t i = 0; i < 128; ++i) texels.push_back(uint64_t{0x80123456u + i * 0x00010203u} * 0x100000001ull);
        image(texels);
    }
    for (uint32_t frame = 0; frame < frames; ++frame) {
        std::vector<uint64_t> w = {(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18, uint64_t{1} << 17, 0x47,
                                   uint64_t{1} << 32, 0x4E};
        if (frame == 0) {
            // drawn once on the GPU: the source stretched over 32 x 32 of the render page
            w.insert(w.end(), {frameReg(kRender), 0x4C, tex0(kSource * 32, 0x00, 4, 1), 0x06, kSprite | kTme | kFst,
                               0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(0, 0), 0x05, uv(16, 16), 0x03,
                               xyz(32, 32), 0x05});
        }
        // every frame: A cleared, then the render page read as a T8 texture through the palette (CLD 1)
        w.insert(w.end(), {frameReg(kA), 0x4C, kSprite, 0x00, rgbaq(0x80000000u), 0x01, xyz(0, 0), 0x05, xyz(64, 64), 0x05,
                           tex0(kRender * 32, 0x13, 5, 0) | (uint64_t{14 * 32} << 37) | (uint64_t{1} << 61), 0x06,
                           kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(0, 0), 0x05,
                           uv(32, 32), 0x03, xyz(32, 32), 0x05});
        ad(w);
        if (frame == frames - 4) {
            // after the target aged out: an EE upload over the top 8 x 8 of the render page (its own view)
            ad({blit(kRender * 32, 0x00), 0x50, 0, 0x51, uint64_t{8} | (uint64_t{8} << 32), 0x52, 0, 0x53});
            std::vector<uint64_t> words;
            for (uint32_t i = 0; i < 32; ++i) words.push_back(uint64_t{0x80F0E0D0u - i} * 0x100000001ull);
            image(words);
        }
        capture.FrameEnd(frame);
    }
    return capture;
}

// IN-415 / IN-416 (GT4's streamed textures over its night render): a CT32 target drawn on the GPU and read as a T8
// texture (a GPU read, no readback), then an EE write over its memory in another view, three kinds in turn:
//   frame % 3 == 0: 128 x 64 T8 over the whole target page (IN-415: whole pages replaced, no store);
//   frame % 3 == 1: 64 x 16 T4 at the target's base (another layout, no shortcut: the drawn pixels are stored, IN-411);
//   frame % 3 == 2: CT32 in the same layout, no store (IN-416): frame 2 writes 64 x 16 from the page before (rows
//   40-55, page-exact), frame 5 writes 16 x 8 from 8 blocks after the target's base (block-exact).
// The T8 sprite then shows memory.
cpuref::CaptureWriter Core2StreamOverRenderCapture(uint32_t frames) {
    cpuref::CaptureWriter capture;
    constexpr uint64_t kA = 0, kSource = 16, kUpload = 21, kRender = 22;  // pages
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xyz = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    auto uv = [](uint32_t u, uint32_t v) { return uint64_t{u * 16} | (uint64_t{v * 16} << 16); };
    auto frameReg = [](uint64_t page) { return page | (uint64_t{1} << 16); };  // FBW 1, CT32
    auto blit = [](uint64_t dbp, uint64_t dpsm) { return (dbp << 32) | (uint64_t{1} << 48) | (dpsm << 56); };
    const uint64_t kSprite = 6, kTme = 1u << 4, kFst = 1u << 8;
    auto tex0 = [](uint64_t blocks, uint64_t psm, uint64_t log2, uint64_t tfx) {
        return blocks | (uint64_t{1} << 14) | (psm << 20) | (log2 << 26) | (log2 << 30) | (uint64_t{1} << 34) |
               (tfx << 35);
    };
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
    capture.Privileged(0x12000010u, uint64_t{2} << 13);
    capture.Privileged(0x12000070u, uint64_t{kA * 32} | (uint64_t{1} << 9));
    capture.Privileged(0x12000080u, (uint64_t{63} << 32) | (uint64_t{63} << 44));
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    auto image = [&](const std::vector<uint64_t>& data) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(data.size() / 2), true, false, 0, 2, {}, data));
    };
    // A grey ramp palette at page 14 and the source texture, as in the ageing stream.
    ad({blit(14 * 32, 0x00), 0x50, 0, 0x51, uint64_t{16} | (uint64_t{16} << 32), 0x52, 0, 0x53});
    {
        std::vector<uint64_t> entries;
        for (uint32_t i = 0; i < 128; ++i) {
            const uint32_t a = 2 * i, b = 2 * i + 1;
            entries.push_back(uint64_t{0x80000000u | (a * 0x010101u)} | (uint64_t{0x80000000u | (b * 0x010101u)} << 32));
        }
        image(entries);
    }
    ad({blit(kSource * 32, 0x00), 0x50, 0, 0x51, uint64_t{16} | (uint64_t{16} << 32), 0x52, 0, 0x53});
    {
        std::vector<uint64_t> texels;
        for (uint32_t i = 0; i < 128; ++i) texels.push_back(uint64_t{0x80123456u + i * 0x00010203u} * 0x100000001ull);
        image(texels);
    }
    const uint64_t readRender = tex0(kRender * 32, 0x13, 5, 0) | (uint64_t{14 * 32} << 37) | (uint64_t{1} << 61);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        // the render page drawn on the GPU, then A cleared and the render page read as a T8 texture
        ad({(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18, uint64_t{1} << 17, 0x47, uint64_t{1} << 32, 0x4E,
            frameReg(kRender), 0x4C, tex0(kSource * 32, 0x00, 4, 1), 0x06, kSprite | kTme | kFst, 0x00,
            rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(frame, 0), 0x05, uv(16, 16), 0x03, xyz(32 + frame, 32), 0x05,
            frameReg(kA), 0x4C, kSprite, 0x00, rgbaq(0x80000000u), 0x01, xyz(0, 0), 0x05, xyz(64, 64), 0x05, readRender,
            0x06, kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(0, 0), 0x05, uv(32, 32),
            0x03, xyz(32, 32), 0x05});
        std::vector<uint64_t> words;
        if (frame % 3 == 0) {
            // 128 x 64 T8, BW 2, at the target's own base: the whole target page in another format
            ad({(uint64_t{kRender * 32} << 32) | (uint64_t{2} << 48) | (uint64_t{0x13} << 56), 0x50, 0, 0x51,
                uint64_t{128} | (uint64_t{64} << 32), 0x52, 0, 0x53});
            for (uint32_t i = 0; i < 128 * 64 / 8; ++i) words.push_back((uint64_t{i + frame} * 0x0102030405060708ull) ^ 0x8040201008040201ull);
        } else if (frame % 3 == 1) {
            // 64 x 16 T4, BW 2, rows 8-23 at the target's base
            ad({(uint64_t{kRender * 32} << 32) | (uint64_t{2} << 48) | (uint64_t{0x14} << 56), 0x50, uint64_t{8} << 48,
                0x51, uint64_t{64} | (uint64_t{16} << 32), 0x52, 0, 0x53});
            for (uint32_t i = 0; i < 64 * 16 / 16; ++i) words.push_back(uint64_t{0x0123456789ABCDEFull} * (i + 3 + frame));
        } else if (frame == 2) {
            // 64 x 16 CT32 at rows 40-55 from page 21: rows 8-23 of the target page
            ad({blit(kUpload * 32, 0x00), 0x50, uint64_t{40} << 48, 0x51, uint64_t{64} | (uint64_t{16} << 32), 0x52, 0,
                0x53});
            for (uint32_t i = 0; i < 64 * 16 / 2; ++i) {
                words.push_back(uint64_t{0x80F0E0D0u - i - frame * 77u} | (uint64_t{0x80203040u + i + frame} << 32));
            }
        } else {
            // 16 x 8 CT32 from 8 blocks after the target's base: two blocks somewhere inside the target page
            ad({blit(kRender * 32 + 8, 0x00), 0x50, 0, 0x51, uint64_t{16} | (uint64_t{8} << 32), 0x52, 0, 0x53});
            for (uint32_t i = 0; i < 16 * 8 / 2; ++i) {
                words.push_back(uint64_t{0x80A0B0C0u + i * 3u + frame} | (uint64_t{0x80C0B0A0u - i} << 32));
            }
        }
        image(words);
        // the render page read again, next to the first sprite
        ad({readRender, 0x06, kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(32, 0), 0x05,
            uv(32, 32), 0x03, xyz(64, 32), 0x05});
        capture.FrameEnd(frame);
    }
    return capture;
}

// IN-508 (GT4's Z32 target and the stream after it on George V Paris): a CT32 target over pages 22 and 23 (FBW 2), drawn
// only inside page 22 and read as a T8 texture on the GPU, then an EE write in another layout:
//   frame % 2 == 0: 128 x 64 T8 in page 23, in the target's page row but sharing no page with its drawn area;
//   frame % 2 == 1: 64 x 16 T4 in page 22, over the drawn page: the drawn pixels are stored (IN-411).
// The target is then sampled in both pages, so both a lost drawn pixel and a lost write show in the picture. The drawn
// sprite has one colour per frame, so the picture does not depend on the scale. `readFirst` draws a small sprite between
// the T8 read and the write, which sends the read's batch first; without it the T8 write in page 23 overtakes that
// batch, so it finds a drawn target that nothing has read yet.
cpuref::CaptureWriter Core2WriteBesideDrawnCapture(uint32_t frames, bool readFirst) {
    cpuref::CaptureWriter capture;
    constexpr uint64_t kA = 0, kSource = 16, kRender = 22;  // pages
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xyz = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    auto uv = [](uint32_t u, uint32_t v) { return uint64_t{u * 16} | (uint64_t{v * 16} << 16); };
    auto frameReg = [](uint64_t page, uint64_t fbw) { return page | (fbw << 16); };  // CT32
    auto blit = [](uint64_t dbp, uint64_t dbw, uint64_t dpsm) { return (dbp << 32) | (dbw << 48) | (dpsm << 56); };
    const uint64_t kSprite = 6, kTme = 1u << 4, kFst = 1u << 8;
    auto tex0 = [](uint64_t blocks, uint64_t tbw, uint64_t psm, uint64_t log2, uint64_t tfx) {
        return blocks | (tbw << 14) | (psm << 20) | (log2 << 26) | (log2 << 30) | (uint64_t{1} << 34) | (tfx << 35);
    };
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
    capture.Privileged(0x12000010u, uint64_t{2} << 13);
    capture.Privileged(0x12000070u, uint64_t{kA * 32} | (uint64_t{1} << 9));
    capture.Privileged(0x12000080u, (uint64_t{63} << 32) | (uint64_t{63} << 44));
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    auto image = [&](const std::vector<uint64_t>& data) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(data.size() / 2), true, false, 0, 2, {}, data));
    };
    // A grey ramp palette at page 14, and a source texture of one colour (TFX modulate then gives the vertex colour).
    ad({blit(14 * 32, 1, 0x00), 0x50, 0, 0x51, uint64_t{16} | (uint64_t{16} << 32), 0x52, 0, 0x53});
    {
        std::vector<uint64_t> entries;
        for (uint32_t i = 0; i < 128; ++i) {
            const uint32_t a = 2 * i, b = 2 * i + 1;
            entries.push_back(uint64_t{0x80000000u | (a * 0x010101u)} | (uint64_t{0x80000000u | (b * 0x010101u)} << 32));
        }
        image(entries);
    }
    ad({blit(kSource * 32, 1, 0x00), 0x50, 0, 0x51, uint64_t{16} | (uint64_t{16} << 32), 0x52, 0, 0x53});
    image(std::vector<uint64_t>(128, uint64_t{0x80808080u} * 0x100000001ull));
    const uint64_t readRender = tex0(kRender * 32, 1, 0x13, 5, 0) | (uint64_t{14 * 32} << 37) | (uint64_t{1} << 61);
    const uint64_t sampleRender = tex0(kRender * 32, 2, 0x00, 7, 1);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        // page 22 drawn on the GPU, then A cleared and page 22 read as a T8 texture
        ad({(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18, uint64_t{1} << 17, 0x47, uint64_t{1} << 32, 0x4E,
            frameReg(kRender, 2), 0x4C, tex0(kSource * 32, 1, 0x00, 4, 0), 0x06, kSprite | kTme | kFst, 0x00,
            rgbaq(0x80000000u | ((frame + 1u) * 0x00102030u)), 0x01, uv(0, 0), 0x03, xyz(frame, 0), 0x05, uv(16, 16), 0x03,
            xyz(32 + frame, 32), 0x05, frameReg(kA, 1), 0x4C, kSprite, 0x00, rgbaq(0x80000000u), 0x01, xyz(0, 0), 0x05,
            xyz(64, 64), 0x05, readRender, 0x06, kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03,
            xyz(0, 0), 0x05, uv(32, 32), 0x03, xyz(32, 32), 0x05});
        if (readFirst) {
            ad({sampleRender, 0x06, kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uv(8, 8), 0x03, xyz(0, 60), 0x05,
                uv(12, 12), 0x03, xyz(4, 64), 0x05});
        }
        std::vector<uint64_t> words;
        if (frame % 2 == 0) {
            ad({blit((kRender + 1) * 32, 2, 0x13), 0x50, 0, 0x51, uint64_t{128} | (uint64_t{64} << 32), 0x52, 0, 0x53});
            for (uint32_t i = 0; i < 128 * 64 / 8; ++i) words.push_back((uint64_t{i + frame} * 0x0102030405060708ull) ^ 0x8040201008040201ull);
        } else {
            ad({blit(kRender * 32, 2, 0x14), 0x50, uint64_t{8} << 48, 0x51, uint64_t{64} | (uint64_t{16} << 32), 0x52, 0, 0x53});
            for (uint32_t i = 0; i < 64 * 16 / 16; ++i) words.push_back(uint64_t{0x0123456789ABCDEFull} * (i + 3 + frame));
        }
        image(words);
        // the target sampled in the drawn page and in the page next to it (inside the sprite: at scale a target's undrawn
        // first sub-pixel row and column hold earlier content, which the whole-row patch of the run without the test hides)
        ad({sampleRender, 0x06, kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uv(8, 8), 0x03, xyz(32, 0), 0x05,
            uv(24, 24), 0x03, xyz(48, 16), 0x05, uv(64, 0), 0x03, xyz(32, 32), 0x05, uv(96, 32), 0x03, xyz(64, 64), 0x05});
        capture.FrameEnd(frame);
    }
    return capture;
}

// IN-509 (GT4's Z target at BP 0 on George V Paris): page 22 is drawn as FBW 1 every frame and, after that, as FBW 2 every
// second frame, where it is also read as a T8 texture on the GPU. So the FBW 1 draw finds the FBW 2 target two VBlanks old,
// with pixels only the GPU read. The picture samples both targets inside this frame's sprites.
cpuref::CaptureWriter Core2WidthEverySecondVblankCapture(uint32_t frames) {
    cpuref::CaptureWriter capture;
    constexpr uint64_t kA = 0, kSource = 16, kRender = 22;  // pages
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xyz = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    auto uv = [](uint32_t u, uint32_t v) { return uint64_t{u * 16} | (uint64_t{v * 16} << 16); };
    auto frameReg = [](uint64_t page, uint64_t fbw) { return page | (fbw << 16); };  // CT32
    auto blit = [](uint64_t dbp, uint64_t dbw, uint64_t dpsm) { return (dbp << 32) | (dbw << 48) | (dpsm << 56); };
    const uint64_t kSprite = 6, kTme = 1u << 4, kFst = 1u << 8;
    auto tex0 = [](uint64_t blocks, uint64_t tbw, uint64_t psm, uint64_t log2, uint64_t tfx) {
        return blocks | (tbw << 14) | (psm << 20) | (log2 << 26) | (log2 << 30) | (uint64_t{1} << 34) | (tfx << 35);
    };
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
    capture.Privileged(0x12000010u, uint64_t{2} << 13);
    capture.Privileged(0x12000070u, uint64_t{kA * 32} | (uint64_t{1} << 9));
    capture.Privileged(0x12000080u, (uint64_t{63} << 32) | (uint64_t{63} << 44));
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    auto image = [&](const std::vector<uint64_t>& data) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(data.size() / 2), true, false, 0, 2, {}, data));
    };
    ad({blit(14 * 32, 1, 0x00), 0x50, 0, 0x51, uint64_t{16} | (uint64_t{16} << 32), 0x52, 0, 0x53});
    {
        std::vector<uint64_t> entries;
        for (uint32_t i = 0; i < 128; ++i) {
            const uint32_t a = 2 * i, b = 2 * i + 1;
            entries.push_back(uint64_t{0x80000000u | (a * 0x010101u)} | (uint64_t{0x80000000u | (b * 0x010101u)} << 32));
        }
        image(entries);
    }
    ad({blit(kSource * 32, 1, 0x00), 0x50, 0, 0x51, uint64_t{16} | (uint64_t{16} << 32), 0x52, 0, 0x53});
    image(std::vector<uint64_t>(128, uint64_t{0x80808080u} * 0x100000001ull));
    const uint64_t source = tex0(kSource * 32, 1, 0x00, 4, 0);
    const uint64_t readRender = tex0(kRender * 32, 1, 0x13, 5, 0) | (uint64_t{14 * 32} << 37) | (uint64_t{1} << 61);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        const uint32_t narrow = 0x80000000u | ((frame + 1u) * 0x00203010u), wide = 0x80000000u | ((frame + 1u) * 0x00102030u);
        // A cleared, then the FBW 1 target drawn in the page's lower half and sampled inside that sprite
        ad({(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18, uint64_t{1} << 17, 0x47, uint64_t{1} << 32, 0x4E,
            frameReg(kA, 1), 0x4C, kSprite, 0x00, rgbaq(0x80000000u), 0x01, xyz(0, 0), 0x05, xyz(64, 64), 0x05,
            frameReg(kRender, 1), 0x4C, source, 0x06, kSprite | kTme | kFst, 0x00, rgbaq(narrow), 0x01, uv(0, 0), 0x03,
            xyz(0, 16), 0x05, uv(16, 16), 0x03, xyz(64, 32), 0x05, frameReg(kA, 1), 0x4C, tex0(kRender * 32, 1, 0x00, 6, 1),
            0x06, kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uv(8, 20), 0x03, xyz(0, 48), 0x05, uv(24, 28), 0x03,
            xyz(16, 56), 0x05});
        if (frame % 2 == 1) {
            // The FBW 2 target drawn in the page's upper half, read as T8 on the GPU, and sampled inside its sprite.
            ad({frameReg(kRender, 2), 0x4C, source, 0x06, kSprite | kTme | kFst, 0x00, rgbaq(wide), 0x01, uv(0, 0), 0x03,
                xyz(0, 0), 0x05, uv(16, 16), 0x03, xyz(64, 16), 0x05, frameReg(kA, 1), 0x4C, readRender, 0x06,
                kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(0, 0), 0x05, uv(32, 16), 0x03,
                xyz(32, 16), 0x05, tex0(kRender * 32, 2, 0x00, 7, 1), 0x06, kSprite | kTme | kFst, 0x00,
                rgbaq(0x80808080u), 0x01, uv(8, 4), 0x03, xyz(32, 48), 0x05, uv(24, 12), 0x03, xyz(48, 56), 0x05});
        }
        capture.FrameEnd(frame);
    }
    return capture;
}

// IN-513 (GT4's textures over its small targets): a CT32 target drawn on the GPU and read as a T8 texture in several batches of
// one frame: the same read twice (reusable), after a new draw over it, after an EE write over its memory, and after a draw
// elsewhere in the same frame. Each change must show in the picture.
cpuref::CaptureWriter Core2TextureOverTargetCapture(uint32_t frames) {
    cpuref::CaptureWriter capture;
    constexpr uint64_t kA = 0, kSource = 16, kOther = 30, kRender = 22;  // pages
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xyz = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    auto uv = [](uint32_t u, uint32_t v) { return uint64_t{u * 16} | (uint64_t{v * 16} << 16); };
    auto frameReg = [](uint64_t page) { return page | (uint64_t{1} << 16); };  // FBW 1, CT32
    auto blit = [](uint64_t dbp, uint64_t dpsm) { return (dbp << 32) | (uint64_t{1} << 48) | (dpsm << 56); };
    const uint64_t kSprite = 6, kTme = 1u << 4, kFst = 1u << 8;
    auto tex0 = [](uint64_t blocks, uint64_t psm, uint64_t log2, uint64_t tfx) {
        return blocks | (uint64_t{1} << 14) | (psm << 20) | (log2 << 26) | (log2 << 30) | (uint64_t{1} << 34) | (tfx << 35);
    };
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
    capture.Privileged(0x12000010u, uint64_t{2} << 13);
    capture.Privileged(0x12000070u, uint64_t{kA * 32} | (uint64_t{1} << 9));
    capture.Privileged(0x12000080u, (uint64_t{63} << 32) | (uint64_t{63} << 44));
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    auto image = [&](const std::vector<uint64_t>& data) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(data.size() / 2), true, false, 0, 2, {}, data));
    };
    ad({blit(14 * 32, 0x00), 0x50, 0, 0x51, uint64_t{16} | (uint64_t{16} << 32), 0x52, 0, 0x53});
    {
        std::vector<uint64_t> entries;
        for (uint32_t i = 0; i < 128; ++i) {
            const uint32_t a = 2 * i, b = 2 * i + 1;
            entries.push_back(uint64_t{0x80000000u | (a * 0x010101u)} | (uint64_t{0x80000000u | (b * 0x010101u)} << 32));
        }
        image(entries);
    }
    ad({blit(kSource * 32, 0x00), 0x50, 0, 0x51, uint64_t{16} | (uint64_t{16} << 32), 0x52, 0, 0x53});
    image(std::vector<uint64_t>(128, uint64_t{0x80808080u} * 0x100000001ull));
    const uint64_t readRender = tex0(kRender * 32, 0x13, 5, 0) | (uint64_t{14 * 32} << 37);
    auto read = [&](uint32_t x, uint32_t y, uint32_t colour) {
        ad({frameReg(kA), 0x4C, readRender, 0x06, kSprite | kTme | kFst, 0x00, rgbaq(colour), 0x01, uv(0, 0), 0x03, xyz(x, y), 0x05,
            uv(16, 16), 0x03, xyz(x + 16, y + 16), 0x05});
    };
    auto draw = [&](uint64_t page, uint32_t colour, uint32_t x0, uint32_t x1) {
        ad({frameReg(page), 0x4C, tex0(kSource * 32, 0x00, 4, 0), 0x06, kSprite | kTme | kFst, 0x00, rgbaq(colour), 0x01, uv(0, 0), 0x03,
            xyz(x0, 0), 0x05, uv(16, 16), 0x03, xyz(x1, 32), 0x05});
    };
    for (uint32_t frame = 0; frame < frames; ++frame) {
        ad({(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18, uint64_t{1} << 17, 0x47, uint64_t{1} << 32, 0x4E,
            frameReg(kA), 0x4C, kSprite, 0x00, rgbaq(0x80000000u), 0x01, xyz(0, 0), 0x05, xyz(64, 64), 0x05});
        draw(kRender, 0x80000000u | ((frame + 1u) * 0x00102030u), 0, 32);
        read(0, 0, 0x80808080u);
        read(16, 0, 0x80606060u);  // nothing changed: the same decode
        draw(kOther, 0x80204060u, 0, 16);  // a draw elsewhere changes nothing for it
        read(32, 0, 0x80404040u);
        draw(kRender, 0x80000000u | ((frame + 5u) * 0x00030201u), 4, 12);  // a new draw over it
        read(48, 0, 0x80808080u);
        ad({blit(kRender * 32, 0x00), 0x50, 0, 0x51, uint64_t{4} | (uint64_t{4} << 32), 0x52, 0, 0x53});  // an EE write over it
        std::vector<uint64_t> words;
        for (uint32_t i = 0; i < 8; ++i) words.push_back(uint64_t{0x80F0A050u + (frame + 1u) * i * 0x00010305u} * 0x100000001ull);
        image(words);
        read(0, 16, 0x80808080u);
        read(16, 16, 0x80606060u);
        capture.FrameEnd(frame);
    }
    return capture;
}

// IN-417 (GT4's palettes in the memory of its night render): page 22 holds a CT32 target (a colour sprite over the
// palette's 16 x 16) and a Z24 target (a depth-only sprite over half of it), both drawn on the GPU; then a T8 texture
// loads its CSM1 palette from page 22 and is drawn. The CLUT load overlaps two drawn targets, one of them depth.
// `repeated` (IN-510): palettes of the same page, in turn as GT4 does, are then loaded again with nothing changed (the GLM
// writes can be left out), after an EE upload over them, and after a new draw over them: those two must show in the picture.
cpuref::CaptureWriter Core2MultiTargetClutCapture(uint32_t frames, bool repeated = false) {
    cpuref::CaptureWriter capture;
    constexpr uint64_t kA = 0, kT8 = 18, kShared = 22;  // pages
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xyz = [](uint32_t x, uint32_t y, uint32_t z = 0) {
        return uint64_t{x * 16} | (uint64_t{y * 16} << 16) | (uint64_t{z} << 32);
    };
    auto uv = [](uint32_t u, uint32_t v) { return uint64_t{u * 16} | (uint64_t{v * 16} << 16); };
    auto frameReg = [](uint64_t page) { return page | (uint64_t{1} << 16); };  // FBW 1, CT32
    const uint64_t kSprite = 6, kTme = 1u << 4, kFst = 1u << 8;
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
    capture.Privileged(0x12000010u, uint64_t{2} << 13);
    capture.Privileged(0x12000070u, uint64_t{kA * 32} | (uint64_t{1} << 9));
    capture.Privileged(0x12000080u, (uint64_t{63} << 32) | (uint64_t{63} << 44));
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    auto image = [&](const std::vector<uint64_t>& data) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(data.size() / 2), true, false, 0, 2, {}, data));
    };
    // A 16 x 16 T8 texture at page 18: every index once.
    ad({(uint64_t{kT8 * 32} << 32) | (uint64_t{2} << 48) | (uint64_t{0x13} << 56), 0x50, 0, 0x51,
        uint64_t{16} | (uint64_t{16} << 32), 0x52, 0, 0x53});
    {
        std::vector<uint64_t> texels;
        for (uint32_t i = 0; i < 32; ++i) {
            uint64_t word = 0;
            for (uint32_t b = 0; b < 8; ++b) word |= uint64_t{(i * 8 + b) & 255u} << (8 * b);
            texels.push_back(word);
        }
        image(texels);
    }
    const uint64_t zbufMasked = kShared | (uint64_t{1} << 24) | (uint64_t{1} << 32);
    const uint64_t zbufZ24 = kShared | (uint64_t{1} << 24);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        ad({(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18,
            // the colour target: a sprite over the palette's 16 x 16 at page 22
            uint64_t{1} << 17, 0x47, zbufMasked, 0x4E, frameReg(kShared), 0x4C, kSprite, 0x00,
            rgbaq(0x80402010u + frame * 0x01030507u), 0x01, xyz(0, 0), 0x05, xyz(16, 16), 0x05,
            // the depth target: Z24 at page 22, a depth-only sprite (colour into A) over half of the palette
            (uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47, zbufZ24, 0x4E, frameReg(kA), 0x4C, kSprite, 0x00,
            rgbaq(0x80000000u), 0x01, xyz(8, 4, 0x00123456u + frame * 0x010101u), 0x05,
            xyz(24, 20, 0x00123456u + frame * 0x010101u), 0x05,
            // the T8 texture through a palette loaded from page 22 (CSM1, CT32, CLD 1), drawn into A
            uint64_t{1} << 17, 0x47, zbufMasked, 0x4E,
            uint64_t{kT8 * 32} | (uint64_t{2} << 14) | (uint64_t{0x13} << 20) | (uint64_t{4} << 26) |
                (uint64_t{4} << 30) | (uint64_t{1} << 34) | (uint64_t{1} << 35) | (uint64_t{kShared * 32} << 37) |
                (uint64_t{1} << 61),
            0x06, kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(32, 0), 0x05,
            uv(16, 16), 0x03, xyz(48, 16), 0x05});
        if (repeated) {
            auto sprite = [&](uint32_t x, uint32_t y, uint64_t paletteBlock) {
                const uint64_t load = uint64_t{kT8 * 32} | (uint64_t{2} << 14) | (uint64_t{0x13} << 20) | (uint64_t{4} << 26) |
                                      (uint64_t{4} << 30) | (uint64_t{1} << 34) | (uint64_t{1} << 35) |
                                      ((kShared * 32 + paletteBlock) << 37) | (uint64_t{1} << 61);
                ad({load, 0x06, kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(x, y), 0x05,
                    uv(16, 16), 0x03, xyz(x + 16, y + 16), 0x05});
            };
            sprite(48, 0, 4);
            sprite(0, 32, 0);
            sprite(16, 32, 4);
            sprite(32, 32, 0);
            ad({(uint64_t{kShared * 32} << 32) | (uint64_t{1} << 48), 0x50, 0, 0x51, uint64_t{4} | (uint64_t{4} << 32), 0x52, 0, 0x53});
            std::vector<uint64_t> words;
            for (uint32_t i = 0; i < 8; ++i) words.push_back(uint64_t{0x80F0A050u + (frame + 1u) * i * 0x00010305u} * 0x100000001ull);
            image(words);
            sprite(32, 16, 4);
            ad({frameReg(kShared), 0x4C, kSprite, 0x00, rgbaq(0x80205080u + frame * 0x00070503u), 0x01, xyz(4, 4), 0x05, xyz(12, 12), 0x05,
                frameReg(kA), 0x4C});
            sprite(48, 16, 0);
        }
        capture.FrameEnd(frame);
    }
    return capture;
}

// IN-427: a CT32 page drawn on the GPU (a colour sprite), then sampled as a texture in its own view (CORE2-SPEC 3.2:
// a texture taken from a target) by a sprite into page 0, as GT4 copies its 3D scene through its tone pass. At 1x the
// copy read the texel atlas instead of the target, so page 0 stayed black; memory must equal the CPU reference.
cpuref::CaptureWriter Core2TargetTextureCapture(uint32_t frames) {
    cpuref::CaptureWriter capture;
    constexpr uint64_t kA = 0, kScene = 22;  // pages
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xyz = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    auto uv = [](uint32_t u, uint32_t v) { return uint64_t{u * 16} | (uint64_t{v * 16} << 16); };
    auto frameReg = [](uint64_t page) { return page | (uint64_t{1} << 16); };  // FBW 1, CT32
    const uint64_t kSprite = 6, kTme = 1u << 4, kFst = 1u << 8;
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
    capture.Privileged(0x12000010u, uint64_t{2} << 13);
    capture.Privileged(0x12000070u, uint64_t{kA * 32} | (uint64_t{1} << 9));
    capture.Privileged(0x12000080u, (uint64_t{63} << 32) | (uint64_t{63} << 44));
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    for (uint32_t frame = 0; frame < frames; ++frame) {
        ad({(uint64_t{127} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18, uint64_t{1} << 17, 0x47, uint64_t{1} << 32, 0x4E,
            // the scene: two colour sprites into page 22 at FBW 2, 128 x 64 (more than 64 x 64 pixels, so the GPU draws
            // it: IN-407 draws smaller untextured sprites into memory on the CPU)
            kScene | (uint64_t{2} << 16), 0x4C, kSprite, 0x00, rgbaq(0x80204060u + frame * 0x00030201u), 0x01,
            xyz(0, 0), 0x05, xyz(128, 64), 0x05, rgbaq(0x80C08040u), 0x01, xyz(8, 4), 0x05, xyz(100, 28), 0x05,
            // the copy: page 22 as a CT32 texture (128 x 64, TBW 2, decal), its left 64 x 32 over page 0
            frameReg(kA), 0x4C,
            uint64_t{kScene * 32} | (uint64_t{2} << 14) | (uint64_t{7} << 26) | (uint64_t{6} << 30) | (uint64_t{1} << 34) |
                (uint64_t{1} << 35),
            0x06, kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(0, 0), 0x05, uv(64, 32),
            0x03, xyz(64, 32), 0x05});
        capture.FrameEnd(frame);
    }
    return capture;
}

// IN-428 (C1): a 16 x 16 CT32 texture whose levels are solid red (level 0), green (level 1, 8 x 8, MIPTBP1) and blue
// (level 2, 4 x 4), MXL 2, drawn as five 16 x 16 FST sprites into page 0 with constant LOD K (LCM = 1):
//   x 0-15: MMIN 2 (nearest mip nearest), K = 1.0 -> level 1 (green);
//   x 16-31: MMIN 2, K = 2.0 -> level 2 (blue);
//   x 32-47: MMIN 3 (nearest mip linear), K = 0.5 -> levels 0 and 1 blended half and half;
//   x 48-63: MMIN 1 (linear, no mip levels), K = 1.0 -> level 0 (red);
//   x 0-15, y 16-31: MMIN 2, K = -1.0 (magnification) -> level 0 (red).
cpuref::CaptureWriter Core2MipCapture() {
    cpuref::CaptureWriter capture;
    constexpr uint64_t kA = 0, kLevel0 = 16, kLevel1 = 17, kLevel2 = 18;  // pages
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xyz = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    auto uv = [](uint32_t u, uint32_t v) { return uint64_t{u * 16} | (uint64_t{v * 16} << 16); };
    auto blit = [](uint64_t dbp) { return (dbp << 32) | (uint64_t{1} << 48); };
    const uint64_t kSprite = 6, kTme = 1u << 4, kFst = 1u << 8;
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
    capture.Privileged(0x12000010u, uint64_t{2} << 13);
    capture.Privileged(0x12000070u, uint64_t{kA * 32} | (uint64_t{1} << 9));
    capture.Privileged(0x12000080u, (uint64_t{63} << 32) | (uint64_t{63} << 44));
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    auto image = [&](const std::vector<uint64_t>& data) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(data.size() / 2), true, false, 0, 2, {}, data));
    };
    auto solid = [&](uint64_t page, uint32_t side, uint32_t colour) {
        ad({blit(page * 32), 0x50, 0, 0x51, uint64_t{side} | (uint64_t{side} << 32), 0x52, 0, 0x53});
        image(std::vector<uint64_t>(side * side / 2, uint64_t{colour} * 0x100000001ull));
    };
    solid(kLevel0, 16, 0x800000FFu);
    solid(kLevel1, 8, 0x8000FF00u);
    solid(kLevel2, 4, 0x80FF0000u);
    const uint64_t tex0 = uint64_t{kLevel0 * 32} | (uint64_t{1} << 14) | (uint64_t{4} << 26) | (uint64_t{4} << 30) |
                          (uint64_t{1} << 34) | (uint64_t{1} << 35);  // CT32 16 x 16, TCC, decal
    const uint64_t miptbp1 = uint64_t{kLevel1 * 32} | (uint64_t{1} << 14) | (uint64_t{kLevel2 * 32} << 20) |
                             (uint64_t{1} << 34);
    auto tex1 = [](uint64_t mmin, int k16) {
        return 1u | (uint64_t{2} << 2) | (mmin << 6) | (static_cast<uint64_t>(static_cast<uint32_t>(k16) & 0xFFFu) << 32);
    };
    std::vector<uint64_t> w = {(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18, uint64_t{1} << 17, 0x47,
                               uint64_t{1} << 32, 0x4E, kA | (uint64_t{1} << 16), 0x4C, tex0, 0x06, miptbp1, 0x34};
    auto sprite = [&](uint32_t x, uint32_t y, uint64_t mmin, int k16) {
        w.insert(w.end(), {tex1(mmin, k16), 0x14, kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03,
                           xyz(x, y), 0x05, uv(16, 16), 0x03, xyz(x + 16, y + 16), 0x05});
    };
    sprite(0, 0, 2, 16);
    sprite(16, 0, 2, 32);
    sprite(32, 0, 3, 8);
    sprite(48, 0, 1, 16);
    sprite(0, 16, 2, -16);
    // IN-434: an STQ sprite 16 x 2 pixels showing the whole 16 x 16 texture (8 texels per pixel down, 1 across),
    // MMIN 2 with LOD 2: level 2 (blue); with GT4_CORE2_ANISO=16 eight samples along the long axis bring the LOD
    // to 2 - 3 < 0, so level 0 (red).
    auto st = [](float s, float t) {
        uint32_t sb, tb;
        std::memcpy(&sb, &s, 4);
        std::memcpy(&tb, &t, 4);
        return uint64_t{sb} | (uint64_t{tb} << 32);
    };
    w.insert(w.end(), {tex1(2, 32), 0x14, kSprite | kTme, 0x00, rgbaq(0x80808080u), 0x01, st(0.0f, 0.0f), 0x02,
                       xyz(0, 32), 0x05, st(1.0f, 1.0f), 0x02, xyz(16, 34), 0x05});
    ad(w);
    capture.FrameEnd(0);
    return capture;
}

// IN-433: a 2 x 2 CT32 checker (black and white texels), nearest (TEX1 = 0), magnified 16 times by one FST sprite
// into page 0 (32 x 32 pixels).
cpuref::CaptureWriter Core2CheckerCapture() {
    cpuref::CaptureWriter capture;
    constexpr uint64_t kTexture = 16;  // page
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xyz = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    auto uv = [](uint32_t u, uint32_t v) { return uint64_t{u * 16} | (uint64_t{v * 16} << 16); };
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
    capture.Privileged(0x12000010u, uint64_t{2} << 13);
    capture.Privileged(0x12000070u, uint64_t{1} << 9);
    capture.Privileged(0x12000080u, (uint64_t{63} << 32) | (uint64_t{63} << 44));
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    ad({(kTexture * 32) << 32 | (uint64_t{1} << 48), 0x50, 0, 0x51, uint64_t{2} | (uint64_t{2} << 32), 0x52, 0, 0x53});
    capture.Gif(1, cpuref::CaptureWriter::Packet(1, true, false, 0, 2, {},
                                                 {uint64_t{0x80000000u} | (uint64_t{0x80FFFFFFu} << 32),
                                                  uint64_t{0x80FFFFFFu} | (uint64_t{0x80000000u} << 32)}));
    const uint64_t tex0 = uint64_t{kTexture * 32} | (uint64_t{1} << 14) | (uint64_t{1} << 26) | (uint64_t{1} << 30) |
                          (uint64_t{1} << 34) | (uint64_t{1} << 35);  // CT32 2 x 2, TCC, decal
    ad({(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18, uint64_t{1} << 17, 0x47, uint64_t{1} << 32, 0x4E,
        uint64_t{1} << 16, 0x4C, tex0, 0x06, 0, 0x14, 6u | (1u << 4) | (1u << 8), 0x00, rgbaq(0x80808080u), 0x01,
        uv(0, 0), 0x03, xyz(0, 0), 0x05, uv(2, 2), 0x03, xyz(32, 32), 0x05});
    capture.FrameEnd(0);
    return capture;
}

// hw_core2_forced_bilinear (IN-433): GT4_HW_FILTER forced filters the nearest checker (grey steps appear), the
// default and forced_no_sprite (a sprite) keep only black and white.
int TestCore2ForcedBilinear(const gpu::VulkanHandles& handles) {
    int failures = 0;
    for (uint32_t mode : {0u, 1u, 2u}) {
        Core2AbRun run;
        std::string error;
        if (!ReplayCore2Ab(handles, Core2CheckerCapture(), "GT4_CORE2_UNUSED_SWITCH", true, run, error, 0, 2,
                           [mode](writeback::Renderer& r) { r.SetForcedBilinear(mode); }) ||
            run.memory.empty() || run.memory.back().empty()) {
            std::printf("  forced bilinear mode %u: replay failed: %s\n", mode, error.c_str());
            ++failures;
            continue;
        }
        memory::LocalMemory local;
        std::memcpy(local.bytes().data(), run.memory.back().data(), kGsMemoryBytes);
        const memory::BufferView frame{0, 1, memory::Psm::Ct32};
        uint32_t greys = 0;
        for (uint32_t y = 0; y < 32; ++y) {
            for (uint32_t x = 0; x < 32; ++x) {
                const uint32_t rgb = local.readPixel(frame, x, y) & 0xFFFFFFu;
                if (rgb != 0 && rgb != 0xFFFFFFu) ++greys;
            }
        }
        const bool expectGreys = mode == 1;
        std::printf("  forced bilinear mode %u: %u grey pixels of 1024 (%s)\n", mode, greys,
                    expectGreys ? "filtered expected" : "nearest expected");
        if ((greys != 0) != expectGreys) ++failures;
    }
    std::printf("hw_core2_forced_bilinear: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_core2_overlap_order (IN-471): strips that fold over themselves, blended, so the result depends on the order of every
// overlapping pair, and whole sprites over them. Without the pixel interlock (GT4_HW_TEST_NO_INTERLOCK=1) primitives
// wait only for the ones they may share a pixel with; GS memory must equal that of waiting before every primitive. The
// slivers of these random strips also round a few words off the CPU reference, the same words with and without the waits.
cpuref::CaptureWriter Core2OverlapCapture() {
    cpuref::CaptureWriter capture;
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xyz = [](uint32_t x, uint32_t y) { return uint64_t{x} | (uint64_t{y} << 16); };  // 12.4
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
    capture.Privileged(0x12000010u, uint64_t{2} << 13);
    capture.Privileged(0x12000070u, uint64_t{1} << 9);
    capture.Privileged(0x12000080u, (uint64_t{63} << 32) | (uint64_t{63} << 44));
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    uint32_t seed = 12345u;
    auto next = [&seed](uint32_t range) {
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 16) % range;
    };
    std::vector<uint64_t> words = {(uint64_t{127} << 16) | (uint64_t{127} << 48), 0x40, 0, 0x18, uint64_t{1} << 17, 0x47,
                                   uint64_t{1} << 32, 0x4E, uint64_t{2} << 16, 0x4C, 0x44, 0x42,  // (Cs - Cd) * As + Cd
                                   4u | (1u << 3) | (1u << 6), 0x00};                              // strip, Gouraud, blend
    for (int v = 0; v < 400; ++v) {
        words.push_back(rgbaq(0x40000000u | next(0x1000000u)));
        words.push_back(0x01);
        words.push_back(xyz(next(127 * 16), next(127 * 16)));
        words.push_back(0x05);
    }
    words.push_back(6u | (1u << 6));  // blended sprites over them, some overlapping, some not
    words.push_back(0x00);
    for (int s = 0; s < 40; ++s) {
        const uint32_t x = next(100), y = next(100);
        words.push_back(rgbaq(0x60000000u | next(0x1000000u)));
        words.push_back(0x01);
        words.push_back(xyz(x * 16, y * 16));
        words.push_back(0x05);
        words.push_back(xyz((x + 8 + next(20)) * 16, (y + 8 + next(20)) * 16));
        words.push_back(0x05);
    }
    ad(words);
    capture.FrameEnd(0);
    return capture;
}

// ---- TASK-168: palette segments ------------------------------------------------------------------------------------
// The shared pieces of the palette segment captures: a 64 x 64 CT32 display at page 0, GS register words through A+D.
struct SegmentCapture {
    cpuref::CaptureWriter capture;
    static uint64_t Rgbaq(uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); }
    static uint64_t Xyz(uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); }
    static uint64_t Uv(uint32_t u, uint32_t v) { return uint64_t{u * 16} | (uint64_t{v * 16} << 16); }
    static uint64_t Frame(uint64_t page, uint32_t fbmsk = 0) { return page | (uint64_t{1} << 16) | (uint64_t{fbmsk} << 32); }
    static uint64_t Blit(uint64_t dbp, uint64_t dpsm) { return (dbp << 32) | (uint64_t{1} << 48) | (dpsm << 56); }
    // TEX0: base (blocks), TBW 1, PSM, TW = TH = log2, TCC, TFX, CBP (blocks), CPSM CT32, CSM1, CSA 0, CLD 1 when `load`.
    static uint64_t Tex0(uint64_t blocks, uint64_t psm, uint64_t log2, uint64_t tfx, uint64_t cbpBlocks, bool load) {
        return blocks | (uint64_t{1} << 14) | (psm << 20) | (log2 << 26) | (log2 << 30) | (uint64_t{1} << 34) | (tfx << 35) |
               (cbpBlocks << 37) | (uint64_t{load ? 1u : 0u} << 61);
    }
    static constexpr uint64_t kSprite = 6, kTme = 1u << 4, kAbe = 1u << 6, kFst = 1u << 8;

    SegmentCapture() {
        capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
        capture.Privileged(0x12000010u, uint64_t{2} << 13);
        capture.Privileged(0x12000070u, uint64_t{1} << 9);  // DISPFB1: page 0, FBW 1, CT32
        capture.Privileged(0x12000080u, (uint64_t{63} << 32) | (uint64_t{63} << 44));
    }
    void Ad(const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    }
    // A width x height upload of 32-bit words `pixel(x, y)` at block `dbp` (TBW 1), or 8-bit texels (T8) when `t8`.
    void Upload(uint64_t dbp, uint32_t width, uint32_t height, bool t8, const std::function<uint32_t(uint32_t, uint32_t)>& pixel) {
        Ad({Blit(dbp, t8 ? 0x13 : 0x00), 0x50, 0, 0x51, uint64_t{width} | (uint64_t{height} << 32), 0x52, 0, 0x53});
        std::vector<uint64_t> data;
        const uint32_t perWord = t8 ? 8u : 2u;
        for (uint32_t i = 0; i < width * height / perWord; ++i) {
            uint64_t word = 0;
            for (uint32_t n = 0; n < perWord; ++n) {
                const uint32_t p = i * perWord + n;
                const uint64_t value = pixel(p % width, p / width);
                word |= t8 ? (value & 0xFFu) << (8 * n) : (value & 0xFFFFFFFFu) << (32 * n);
            }
            data.push_back(word);
        }
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(data.size() / 2), true, false, 0, 2, {}, data));
    }
    // The T8 indices every capture samples: index = (x + 5y) & 255, 64 x 64 at page 12.
    static constexpr uint64_t kT8 = 12;
    void UploadT8() {
        Upload(kT8 * 32, 64, 64, true, [](uint32_t x, uint32_t y) { return (x + 5u * y) & 255u; });
    }
    // A 16 x 16 CT32 palette (CSM1) at block `cbp`, its colours from `seed`.
    void UploadPalette(uint64_t cbp, uint32_t seed) {
        Upload(cbp, 16, 16, false, [seed](uint32_t x, uint32_t y) {
            const uint32_t e = x + 16u * y;
            return ((0x40u + ((e + seed) & 0x3Fu)) << 24) | ((e * 0x030201u + seed * 0x0A0B0Cu) & 0xFFFFFFu);
        });
    }
    // Scissor 64 x 64, no XYOFFSET, depth test ALWAYS and no depth writes, FRAME at `page`, blending (Cs - Cd) As + Cd.
    static std::vector<uint64_t> Setup(uint64_t page) {
        return {(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18, (uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47,
                uint64_t{1} << 32, 0x4E, Frame(page), 0x4C, (uint64_t{1} << 2) | (uint64_t{1} << 6), 0x42};
    }
    // A T8 sprite through the palette at `cbp` (loaded first when `load`): (x, y) to (x + w, y + h), texels from (u, v).
    static void T8Sprite(std::vector<uint64_t>& words, uint64_t cbp, bool load, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                         uint32_t u, uint32_t v, bool blend) {
        const std::vector<uint64_t> part = {Tex0(kT8 * 32, 0x13, 6, 0, cbp, load), 0x06, kSprite | kTme | kFst | (blend ? kAbe : 0u),
                                            0x00, Rgbaq(0x80808080u), 0x01, Uv(u, v), 0x03, Xyz(x, y), 0x05,
                                            Uv(u + w, v + h), 0x03, Xyz(x + w, y + h), 0x05};
        words.insert(words.end(), part.begin(), part.end());
    }
};

// TASK-168: GT4's player view: one T8 texture drawn again and again, a CLUT load before each sprite that only changes its
// palette (CT32, CSM1, from GS memory). `palettes` palettes, eight per page from page 40 (four blocks each), uploaded with
// new colours every frame; the 12 x 12 sprites blend over each other, so their order matters. Each frame starts with an
// uploaded 16 x 16 texture stretched over the whole 64 x 64 frame (a GPU draw, so the target covers the frame from the
// first draw on: a target grown under later batches differs from one made for their union in the first frame).
cpuref::CaptureWriter Core2PaletteSegmentsCapture(uint32_t frames, uint32_t palettes) {
    SegmentCapture c;
    constexpr uint64_t kPalettePage = 40, kBackground = 16;
    auto paletteBlock = [](uint32_t k) { return kPalettePage * 32 + 32u * (k / 8u) + 4u * (k % 8u); };
    c.UploadT8();
    for (uint32_t frame = 0; frame < frames; ++frame) {
        c.Upload(kBackground * 32, 16, 16, false, [frame](uint32_t x, uint32_t y) {
            return 0x80000000u | (((x + 16u * y) * 0x020406u + frame * 0x090807u) & 0xFFFFFFu);
        });
        for (uint32_t k = 0; k < palettes; ++k) c.UploadPalette(paletteBlock(k), 7u * k + 131u * frame);
        std::vector<uint64_t> words = SegmentCapture::Setup(0);
        const std::vector<uint64_t> clear = {
            SegmentCapture::Tex0(kBackground * 32, 0x00, 4, 1, 0, false), 0x06,
            SegmentCapture::kSprite | SegmentCapture::kTme | SegmentCapture::kFst, 0x00, SegmentCapture::Rgbaq(0x80808080u), 0x01,
            SegmentCapture::Uv(0, 0), 0x03, SegmentCapture::Xyz(0, 0), 0x05, SegmentCapture::Uv(16, 16), 0x03,
            SegmentCapture::Xyz(64, 64), 0x05};
        words.insert(words.end(), clear.begin(), clear.end());
        for (uint32_t k = 0; k < palettes; ++k) {
            SegmentCapture::T8Sprite(words, paletteBlock(k), true, (k * 7u) % 52u, (k * 11u + frame) % 52u, 12, 12,
                                     (k * 3u) % 48u, (k * 5u) % 48u, true);
        }
        // The first palette again (a load: the last one was another), and one more sprite.
        SegmentCapture::T8Sprite(words, paletteBlock(0), true, 20, 20, 24, 24, 8, 8, true);
        c.Ad(words);
        c.capture.FrameEnd(frame);
    }
    return c.capture;
}

// TASK-168: a row the batch already names is never taken by its next segment. Frame 0 fills the whole row ring (1024 new
// palettes); frame 1 starts with frame 0's second palette (its row, 1, still holds it), then new palettes: the ring's
// next row is 0, then 1, which the first segment names and must keep.
cpuref::CaptureWriter Core2PaletteRowPinCapture() {
    SegmentCapture c;
    constexpr uint64_t kPalettePage = 40, kBackground = 16;
    auto paletteBlock = [](uint32_t k) { return kPalettePage * 32 + 32u * (k / 8u) + 4u * (k % 8u); };
    c.UploadT8();
    const std::vector<std::vector<uint32_t>> seeds = {
        [] {
            std::vector<uint32_t> all;
            for (uint32_t k = 0; k < 1024u; ++k) all.push_back(1000u + k);
            return all;
        }(),
        {1001u, 5000u, 5001u, 5002u, 5003u, 5004u, 5005u}};
    for (uint32_t frame = 0; frame < seeds.size(); ++frame) {
        c.Upload(kBackground * 32, 16, 16, false, [frame](uint32_t x, uint32_t y) {
            return 0x80000000u | (((x + 16u * y) * 0x020406u + frame * 0x090807u) & 0xFFFFFFu);
        });
        for (uint32_t k = 0; k < seeds[frame].size(); ++k) c.UploadPalette(paletteBlock(k), seeds[frame][k]);
        std::vector<uint64_t> words = SegmentCapture::Setup(0);
        const std::vector<uint64_t> clear = {
            SegmentCapture::Tex0(kBackground * 32, 0x00, 4, 1, 0, false), 0x06,
            SegmentCapture::kSprite | SegmentCapture::kTme | SegmentCapture::kFst, 0x00, SegmentCapture::Rgbaq(0x80808080u), 0x01,
            SegmentCapture::Uv(0, 0), 0x03, SegmentCapture::Xyz(0, 0), 0x05, SegmentCapture::Uv(16, 16), 0x03,
            SegmentCapture::Xyz(64, 64), 0x05};
        words.insert(words.end(), clear.begin(), clear.end());
        for (uint32_t k = 0; k < seeds[frame].size(); ++k) {
            // Frame 1's first sprite is large, so a wrong row for it shows.
            const uint32_t size = frame == 1 && k == 0 ? 40u : 12u;
            SegmentCapture::T8Sprite(words, paletteBlock(k), true, frame == 1 && k == 0 ? 0u : (k * 7u) % 52u,
                                     frame == 1 && k == 0 ? 0u : (k * 11u) % 52u, size, size, 0, 0, frame == 0);
        }
        c.Ad(words);
        c.capture.FrameEnd(frame);
    }
    return c.capture;
}

// TASK-168: the loads that must keep ending the batch in core 2. A palette drawn on the GPU (a decal copy of an uploaded
// texture into page 18, IN-408) is loaded under a pending T8 batch: the load reads a drawn target, so the batch ends; the
// next load from memory finds the batch's palette loaded on the GPU, so it ends that batch too; the last load from memory,
// after a palette from memory, keeps its batch (two segments).
cpuref::CaptureWriter Core2PaletteSegmentsGpuCapture(uint32_t frames) {
    SegmentCapture c;
    constexpr uint64_t kSource = 16, kClutGpu = 18, kClutMemory = 40, kClutMemory2 = 41;  // pages
    c.UploadT8();
    for (uint32_t frame = 0; frame < frames; ++frame) {
        c.Upload(kSource * 32, 16, 16, false, [frame](uint32_t x, uint32_t y) {
            return 0x80000000u | (((x + 16u * y) * 0x050301u + frame * 0x101010u) & 0xFFFFFFu);
        });
        c.UploadPalette(kClutMemory * 32, 11u + frame);
        c.UploadPalette(kClutMemory2 * 32, 57u + 3u * frame);
        std::vector<uint64_t> words = SegmentCapture::Setup(kClutGpu);
        const std::vector<uint64_t> gpuPalette = {
            SegmentCapture::Tex0(kSource * 32, 0x00, 4, 1, 0, false), 0x06,
            SegmentCapture::kSprite | SegmentCapture::kTme | SegmentCapture::kFst, 0x00, SegmentCapture::Rgbaq(0x80808080u), 0x01,
            SegmentCapture::Uv(0, 0), 0x03, SegmentCapture::Xyz(0, 0), 0x05, SegmentCapture::Uv(16, 16), 0x03,
            SegmentCapture::Xyz(16, 16), 0x05,
            SegmentCapture::Frame(0), 0x4C, SegmentCapture::kSprite, 0x00, SegmentCapture::Rgbaq(0x80000000u), 0x01,
            SegmentCapture::Xyz(0, 0), 0x05, SegmentCapture::Xyz(64, 64), 0x05};
        words.insert(words.end(), gpuPalette.begin(), gpuPalette.end());
        SegmentCapture::T8Sprite(words, kClutMemory * 32, true, 0, 0, 24, 24, 0, 0, false);
        SegmentCapture::T8Sprite(words, kClutGpu * 32, true, 24, 0, 24, 24, 8, 0, false);
        SegmentCapture::T8Sprite(words, kClutMemory2 * 32, true, 0, 24, 24, 24, 16, 8, false);
        SegmentCapture::T8Sprite(words, kClutMemory * 32, true, 24, 24, 24, 24, 4, 20, false);
        c.Ad(words);
        c.capture.FrameEnd(frame);
    }
    return c.capture;
}

// TASK-168: a T8 sprite batch over a drawn 32-bit target (GT4's tone sequence, CORE2-SPEC 7.2) whose palette changes between
// its sprites: ToneHook takes one palette per batch, so core 2 hands it the segments one by one. The 32 x 32 target at
// page 0 (the display) is drawn on the GPU from an uploaded texture; the 8-wide sprites read it as T8 into page 4.
cpuref::CaptureWriter Core2PaletteSegmentsToneCapture(uint32_t frames) {
    SegmentCapture c;
    constexpr uint64_t kSource = 16, kDraw = 4, kClutA = 40, kClutB = 41;  // pages
    for (uint32_t frame = 0; frame < frames; ++frame) {
        c.Upload(kSource * 32, 16, 16, false, [frame](uint32_t x, uint32_t y) {
            return 0x80000000u | (((x + 16u * y) * 0x030507u + frame * 0x0F0F0Fu) & 0xFFFFFFu);
        });
        c.UploadPalette(kClutA * 32, 5u + frame);
        c.UploadPalette(kClutB * 32, 77u + frame);
        std::vector<uint64_t> words = SegmentCapture::Setup(0);
        const std::vector<uint64_t> target = {
            SegmentCapture::Tex0(kSource * 32, 0x00, 4, 1, 0, false), 0x06,
            SegmentCapture::kSprite | SegmentCapture::kTme | SegmentCapture::kFst, 0x00, SegmentCapture::Rgbaq(0x80808080u), 0x01,
            SegmentCapture::Uv(0, 0), 0x03, SegmentCapture::Xyz(0, 0), 0x05, SegmentCapture::Uv(16, 16), 0x03,
            SegmentCapture::Xyz(32, 32), 0x05,
            SegmentCapture::Frame(kDraw), 0x4C};
        words.insert(words.end(), target.begin(), target.end());
        const auto t8OverTarget = [&](uint64_t cbp, uint32_t x) {
            const std::vector<uint64_t> part = {
                SegmentCapture::Tex0(0, 0x13, 5, 0, cbp, true), 0x06,
                SegmentCapture::kSprite | SegmentCapture::kTme | SegmentCapture::kFst, 0x00, SegmentCapture::Rgbaq(0x80808080u),
                0x01, SegmentCapture::Uv(x, 0), 0x03, SegmentCapture::Xyz(x, 0), 0x05, SegmentCapture::Uv(x + 8, 32), 0x03,
                SegmentCapture::Xyz(x + 8, 32), 0x05};
            words.insert(words.end(), part.begin(), part.end());
        };
        t8OverTarget(kClutA * 32, 0);
        t8OverTarget(kClutB * 32, 8);
        t8OverTarget(kClutA * 32, 16);
        c.Ad(words);
        c.capture.FrameEnd(frame);
    }
    return c.capture;
}

// TASK-168b: GT4's tone sequence as the player's view sends it: the run's first draw reads the 32-bit target (one 8 x 32 T8
// sprite, its own batch: the next TEX0 changes the texture base), then the copy-back strips read the T8 page at page 12
// with a CLUT load before each 8 x 32 sprite, one batch of three palette segments. ToneHook skips every strip (a run is
// going on, the same shape and FBMSK) without reading its palette, so core 2 skips the segments without a batch each.
cpuref::CaptureWriter Core2PaletteSegmentsToneRunCapture(uint32_t frames) {
    SegmentCapture c;
    constexpr uint64_t kSource = 16, kDraw = 4, kClutA = 40, kClutB = 41;  // pages
    c.UploadT8();
    for (uint32_t frame = 0; frame < frames; ++frame) {
        c.Upload(kSource * 32, 16, 16, false, [frame](uint32_t x, uint32_t y) {
            return 0x80000000u | (((x + 16u * y) * 0x030507u + frame * 0x0F0F0Fu) & 0xFFFFFFu);
        });
        c.UploadPalette(kClutA * 32, 9u + frame);
        c.UploadPalette(kClutB * 32, 61u + frame);
        std::vector<uint64_t> words = SegmentCapture::Setup(0);
        const std::vector<uint64_t> target = {
            SegmentCapture::Tex0(kSource * 32, 0x00, 4, 1, 0, false), 0x06,
            SegmentCapture::kSprite | SegmentCapture::kTme | SegmentCapture::kFst, 0x00, SegmentCapture::Rgbaq(0x80808080u), 0x01,
            SegmentCapture::Uv(0, 0), 0x03, SegmentCapture::Xyz(0, 0), 0x05, SegmentCapture::Uv(16, 16), 0x03,
            SegmentCapture::Xyz(32, 32), 0x05,
            SegmentCapture::Frame(kDraw), 0x4C};
        words.insert(words.end(), target.begin(), target.end());
        const auto strip = [&](uint64_t base, uint64_t cbp, uint32_t x) {
            const std::vector<uint64_t> part = {
                SegmentCapture::Tex0(base, 0x13, 5, 0, cbp, true), 0x06,
                SegmentCapture::kSprite | SegmentCapture::kTme | SegmentCapture::kFst, 0x00, SegmentCapture::Rgbaq(0x80808080u),
                0x01, SegmentCapture::Uv(x, 0), 0x03, SegmentCapture::Xyz(x, 0), 0x05, SegmentCapture::Uv(x + 8, 32), 0x03,
                SegmentCapture::Xyz(x + 8, 32), 0x05};
            words.insert(words.end(), part.begin(), part.end());
        };
        strip(0, kClutA * 32, 0);  // the run's first draw, over the target at page 0
        strip(SegmentCapture::kT8 * 32, kClutB * 32, 8);
        strip(SegmentCapture::kT8 * 32, kClutA * 32, 16);
        strip(SegmentCapture::kT8 * 32, kClutB * 32, 24);
        c.Ad(words);
        c.capture.FrameEnd(frame);
    }
    return c.capture;
}

// TASK-168b: T8 sprites over a 32-bit target (ShuffleSource finds it) with a CLUT load before each, but not of the tone
// sequence's shape (12 x 12): ToneHook would leave each segment's batch to the normal path, so core 2 draws the batch whole.
cpuref::CaptureWriter Core2PaletteSegmentsNoShuffleCapture(uint32_t frames) {
    SegmentCapture c;
    constexpr uint64_t kSource = 16, kDraw = 4, kClutA = 40, kClutB = 41;  // pages
    for (uint32_t frame = 0; frame < frames; ++frame) {
        c.Upload(kSource * 32, 16, 16, false, [frame](uint32_t x, uint32_t y) {
            return 0x80000000u | (((x + 16u * y) * 0x050709u + frame * 0x0B0B0Bu) & 0xFFFFFFu);
        });
        c.UploadPalette(kClutA * 32, 13u + frame);
        c.UploadPalette(kClutB * 32, 87u + frame);
        std::vector<uint64_t> words = SegmentCapture::Setup(0);
        const std::vector<uint64_t> target = {
            SegmentCapture::Tex0(kSource * 32, 0x00, 4, 1, 0, false), 0x06,
            SegmentCapture::kSprite | SegmentCapture::kTme | SegmentCapture::kFst, 0x00, SegmentCapture::Rgbaq(0x80808080u), 0x01,
            SegmentCapture::Uv(0, 0), 0x03, SegmentCapture::Xyz(0, 0), 0x05, SegmentCapture::Uv(16, 16), 0x03,
            SegmentCapture::Xyz(32, 32), 0x05,
            SegmentCapture::Frame(kDraw), 0x4C};
        words.insert(words.end(), target.begin(), target.end());
        const auto sprite = [&](uint64_t cbp, uint32_t x, uint32_t y) {
            const std::vector<uint64_t> part = {
                SegmentCapture::Tex0(0, 0x13, 5, 0, cbp, true), 0x06,
                SegmentCapture::kSprite | SegmentCapture::kTme | SegmentCapture::kFst, 0x00, SegmentCapture::Rgbaq(0x80808080u),
                0x01, SegmentCapture::Uv(x, y), 0x03, SegmentCapture::Xyz(x, y), 0x05, SegmentCapture::Uv(x + 12, y + 12), 0x03,
                SegmentCapture::Xyz(x + 12, y + 12), 0x05};
            words.insert(words.end(), part.begin(), part.end());
        };
        sprite(kClutA * 32, 0, 0);
        sprite(kClutB * 32, 12, 4);
        sprite(kClutA * 32, 20, 16);
        c.Ad(words);
        c.capture.FrameEnd(frame);
    }
    return c.capture;
}

// hw_core2_palette_segments (TASK-168): GT4_CORE2_PALETTE_SEGMENTS on CLUT loads that only change the palette of the pending
// batch's T8 texture. The same images and GS memory with the switch off and on, at 1x and 2x; with it on, one batch per
// run of loads (its sprites drawn with a palette row each); loads from a drawn target, or after a GPU palette, still end
// the batch; a T8 batch over a 32-bit target goes to the tone hook segment by segment; TASK-168b: a tone run's copy-back
// strips form no segment (the batch filter ends the batch at their loads), and T8 sprites over a 32-bit target that are
// not of the tone sequence's shape are drawn whole. The switch is off by default (owner, 2026-11-05), so the "on" runs set
// it to 1.
int TestCore2PaletteSegments(const gpu::VulkanHandles& handles) {
    const char* kSwitch = "GT4_CORE2_PALETTE_SEGMENTS";
    const char* kOn = "1";
    int failures = 0;
    constexpr uint32_t kFrames = 3;
    for (uint32_t scale : {1u, 2u}) {
        const std::string tag = scale == 1 ? " at 1x" : " at 2x";
        failures += CheckCore2Ab(handles, ("four palettes" + tag).c_str(), Core2PaletteSegmentsCapture(kFrames, 3), kFrames,
                                 kSwitch, true,
                                 [](const Core2AbRun& off, const Core2AbRun& on) {
                                     std::printf("  segmented batches %llu / %llu, segments %llu / %llu, rows written %llu / %llu\n",
                                                 static_cast<unsigned long long>(off.stats.paletteSegmentBatches),
                                                 static_cast<unsigned long long>(on.stats.paletteSegmentBatches),
                                                 static_cast<unsigned long long>(off.stats.paletteSegmentsDrawn),
                                                 static_cast<unsigned long long>(on.stats.paletteSegmentsDrawn),
                                                 static_cast<unsigned long long>(off.stats.paletteRowsWritten),
                                                 static_cast<unsigned long long>(on.stats.paletteRowsWritten));
                                     return off.stats.paletteSegmentBatches == 0 && on.stats.paletteSegmentBatches == kFrames &&
                                                    on.stats.paletteSegmentsDrawn == 4u * kFrames
                                                ? 0
                                                : 1;
                                 },
                                 false, scale, kOn);
        // Many palettes (the row ring): 1100 a frame, new every frame. Batches of 256 segments (the most a batch holds),
        // five a frame, and more palettes in a frame than the 1024 rows: rows are taken again while earlier batches of
        // the frame that name them may still wait to be recorded.
        constexpr uint32_t kRingFrames = 2, kPalettes = 1100, kBatchesPerFrame = 5;
        failures += CheckCore2Ab(handles, ("the palette row ring" + tag).c_str(), Core2PaletteSegmentsCapture(kRingFrames, kPalettes),
                                 kRingFrames, kSwitch, true,
                                 [](const Core2AbRun& off, const Core2AbRun& on) {
                                     std::printf("  segmented batches %llu / %llu, segments %llu / %llu, rows written %llu / %llu\n",
                                                 static_cast<unsigned long long>(off.stats.paletteSegmentBatches),
                                                 static_cast<unsigned long long>(on.stats.paletteSegmentBatches),
                                                 static_cast<unsigned long long>(off.stats.paletteSegmentsDrawn),
                                                 static_cast<unsigned long long>(on.stats.paletteSegmentsDrawn),
                                                 static_cast<unsigned long long>(off.stats.paletteRowsWritten),
                                                 static_cast<unsigned long long>(on.stats.paletteRowsWritten));
                                     return off.stats.paletteSegmentBatches == 0 &&
                                                    on.stats.paletteSegmentBatches == kBatchesPerFrame * kRingFrames &&
                                                    on.stats.paletteSegmentsDrawn == (kPalettes + 1u) * kRingFrames &&
                                                    on.stats.paletteRowsWritten > 1024u
                                                ? 0
                                                : 1;
                                 },
                                 true, scale, kOn);
        failures += CheckCore2Ab(handles, ("a row named by an earlier segment" + tag).c_str(), Core2PaletteRowPinCapture(), 2,
                                 kSwitch, true,
                                 [](const Core2AbRun& off, const Core2AbRun& on) {
                                     std::printf("  segmented batches %llu / %llu, rows written %llu / %llu, row hits %llu / %llu\n",
                                                 static_cast<unsigned long long>(off.stats.paletteSegmentBatches),
                                                 static_cast<unsigned long long>(on.stats.paletteSegmentBatches),
                                                 static_cast<unsigned long long>(off.stats.paletteRowsWritten),
                                                 static_cast<unsigned long long>(on.stats.paletteRowsWritten),
                                                 static_cast<unsigned long long>(off.stats.paletteRowHits),
                                                 static_cast<unsigned long long>(on.stats.paletteRowHits));
                                     return off.stats.paletteSegmentBatches == 0 && on.stats.paletteSegmentBatches == 5u &&
                                                    on.stats.paletteRowsWritten == 1024u + 6u && on.stats.paletteRowHits == 1u
                                                ? 0
                                                : 1;
                                 },
                                 true, scale, kOn);
        failures += CheckCore2Ab(handles, ("loads from a GPU palette" + tag).c_str(), Core2PaletteSegmentsGpuCapture(kFrames),
                                 kFrames, kSwitch, false,
                                 [](const Core2AbRun& off, const Core2AbRun& on) {
                                     std::printf("  segmented batches %llu / %llu, segments %llu / %llu, GPU CLUT loads %llu / %llu\n",
                                                 static_cast<unsigned long long>(off.stats.paletteSegmentBatches),
                                                 static_cast<unsigned long long>(on.stats.paletteSegmentBatches),
                                                 static_cast<unsigned long long>(off.stats.paletteSegmentsDrawn),
                                                 static_cast<unsigned long long>(on.stats.paletteSegmentsDrawn),
                                                 static_cast<unsigned long long>(off.stats.clutTargetLoads),
                                                 static_cast<unsigned long long>(on.stats.clutTargetLoads));
                                     return off.stats.paletteSegmentBatches == 0 && on.stats.paletteSegmentBatches == kFrames &&
                                                    on.stats.paletteSegmentsDrawn == 2u * kFrames && off.stats.clutTargetLoads > 0 &&
                                                    on.stats.clutTargetLoads == off.stats.clutTargetLoads
                                                ? 0
                                                : 1;
                                 },
                                 false, scale, kOn);
        failures += CheckCore2Ab(handles, ("T8 over a 32-bit target" + tag).c_str(), Core2PaletteSegmentsToneCapture(kFrames),
                                 kFrames, kSwitch, false,
                                 [](const Core2AbRun& off, const Core2AbRun& on) {
                                     std::printf("  split batches %llu / %llu, tone runs %llu / %llu, tone draws skipped %llu / %llu\n",
                                                 static_cast<unsigned long long>(off.stats.paletteSegmentSplits),
                                                 static_cast<unsigned long long>(on.stats.paletteSegmentSplits),
                                                 static_cast<unsigned long long>(off.stats.toneRuns),
                                                 static_cast<unsigned long long>(on.stats.toneRuns),
                                                 static_cast<unsigned long long>(off.stats.toneDrawsSkipped),
                                                 static_cast<unsigned long long>(on.stats.toneDrawsSkipped));
                                     return off.stats.paletteSegmentSplits == 0 && on.stats.paletteSegmentSplits == kFrames &&
                                                    on.stats.toneRuns == off.stats.toneRuns && off.stats.toneRuns > 0 &&
                                                    on.stats.toneDrawsSkipped == off.stats.toneDrawsSkipped
                                                ? 0
                                                : 1;
                                 },
                                 false, scale, kOn);
        // TASK-168b: the copy-back strips of a tone run: the batch filter ends the strip batch at each of its two loads
        // that keep the texture, so no segment forms and every strip reaches ToneHook as with the switch off.
        failures += CheckCore2Ab(handles, ("a tone run's copy-back strips" + tag).c_str(), Core2PaletteSegmentsToneRunCapture(kFrames),
                                 kFrames, kSwitch, false,
                                 [](const Core2AbRun& off, const Core2AbRun& on) {
                                     std::printf("  loads refused during a tone run %llu / %llu, segmented batches %llu / %llu, "
                                                 "split batches %llu / %llu, tone runs %llu / %llu, tone draws skipped %llu / %llu\n",
                                                 static_cast<unsigned long long>(off.stats.paletteSegmentToneRefusals),
                                                 static_cast<unsigned long long>(on.stats.paletteSegmentToneRefusals),
                                                 static_cast<unsigned long long>(off.stats.paletteSegmentBatches),
                                                 static_cast<unsigned long long>(on.stats.paletteSegmentBatches),
                                                 static_cast<unsigned long long>(off.stats.paletteSegmentSplits),
                                                 static_cast<unsigned long long>(on.stats.paletteSegmentSplits),
                                                 static_cast<unsigned long long>(off.stats.toneRuns),
                                                 static_cast<unsigned long long>(on.stats.toneRuns),
                                                 static_cast<unsigned long long>(off.stats.toneDrawsSkipped),
                                                 static_cast<unsigned long long>(on.stats.toneDrawsSkipped));
                                     return off.stats.paletteSegmentToneRefusals == 0 &&
                                                    on.stats.paletteSegmentToneRefusals == 2u * kFrames &&
                                                    on.stats.paletteSegmentBatches == 0 && on.stats.paletteSegmentSplits == 0 &&
                                                    off.stats.toneRuns == kFrames && on.stats.toneRuns == off.stats.toneRuns &&
                                                    on.stats.toneDrawsSkipped == off.stats.toneDrawsSkipped
                                                ? 0
                                                : 1;
                                 },
                                 false, scale, kOn);
        // TASK-168b: T8 sprites over a 32-bit target that no segment's batch would give the tone hook: drawn whole.
        failures += CheckCore2Ab(handles, ("T8 over a 32-bit target, not a shuffle" + tag).c_str(),
                                 Core2PaletteSegmentsNoShuffleCapture(kFrames), kFrames, kSwitch, false,
                                 [](const Core2AbRun& off, const Core2AbRun& on) {
                                     std::printf("  segmented batches %llu / %llu, drawn whole without the tone hook %llu / %llu, "
                                                 "split batches %llu / %llu, tone runs %llu / %llu\n",
                                                 static_cast<unsigned long long>(off.stats.paletteSegmentBatches),
                                                 static_cast<unsigned long long>(on.stats.paletteSegmentBatches),
                                                 static_cast<unsigned long long>(off.stats.paletteSegmentsWholeNoTone),
                                                 static_cast<unsigned long long>(on.stats.paletteSegmentsWholeNoTone),
                                                 static_cast<unsigned long long>(off.stats.paletteSegmentSplits),
                                                 static_cast<unsigned long long>(on.stats.paletteSegmentSplits),
                                                 static_cast<unsigned long long>(off.stats.toneRuns),
                                                 static_cast<unsigned long long>(on.stats.toneRuns));
                                     return off.stats.paletteSegmentBatches == 0 && on.stats.paletteSegmentBatches == kFrames &&
                                                    on.stats.paletteSegmentsWholeNoTone == kFrames &&
                                                    on.stats.paletteSegmentSplits == 0 && on.stats.toneRuns == 0 &&
                                                    off.stats.toneRuns == 0
                                                ? 0
                                                : 1;
                                 },
                                 false, scale, kOn);
    }
    std::printf("hw_core2_palette_segments: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_core2_tone_early_drop (TASK-168b): GT4_CORE2_TONE_EARLY_DROP. A tone run's copy-back strips are dropped as the front
// end emits them instead of in OnBatch: the same images and GS memory with the switch off and on, at 1x and 2x, the same
// tone runs and skipped draws (four per frame: the run's first draw and three strips), and three early drops per frame
// with it on (the strips after the run's first draw, which starts the run in OnBatch).
// T8 sprites over a 32-bit target that are not of the shuffle shape start no run: nothing is dropped.
int TestCore2ToneEarlyDrop(const gpu::VulkanHandles& handles) {
    const char* kSwitch = "GT4_CORE2_TONE_EARLY_DROP";
    int failures = 0;
    constexpr uint32_t kFrames = 3;
    for (uint32_t scale : {1u, 2u}) {
        const std::string tag = scale == 1 ? " at 1x" : " at 2x";
        failures += CheckCore2Ab(handles, ("a tone run's copy-back strips" + tag).c_str(), Core2PaletteSegmentsToneRunCapture(kFrames),
                                 kFrames, kSwitch, false,
                                 [](const Core2AbRun& off, const Core2AbRun& on) {
                                     std::printf("  early drops %llu / %llu, tone runs %llu / %llu, tone draws skipped %llu / %llu\n",
                                                 static_cast<unsigned long long>(off.stats.toneDropsOnArrival),
                                                 static_cast<unsigned long long>(on.stats.toneDropsOnArrival),
                                                 static_cast<unsigned long long>(off.stats.toneRuns),
                                                 static_cast<unsigned long long>(on.stats.toneRuns),
                                                 static_cast<unsigned long long>(off.stats.toneDrawsSkipped),
                                                 static_cast<unsigned long long>(on.stats.toneDrawsSkipped));
                                     return off.stats.toneDropsOnArrival == 0 && on.stats.toneDropsOnArrival == 3u * kFrames &&
                                                    off.stats.toneRuns == kFrames && on.stats.toneRuns == off.stats.toneRuns &&
                                                    on.stats.toneDrawsSkipped == off.stats.toneDrawsSkipped &&
                                                    off.stats.toneDrawsSkipped == 4u * kFrames
                                                ? 0
                                                : 1;
                                 },
                                 false, scale);
        failures += CheckCore2Ab(handles, ("T8 over a 32-bit target, not a shuffle" + tag).c_str(),
                                 Core2PaletteSegmentsNoShuffleCapture(kFrames), kFrames, kSwitch, false,
                                 [](const Core2AbRun& off, const Core2AbRun& on) {
                                     return off.stats.toneDropsOnArrival == 0 && on.stats.toneDropsOnArrival == 0 &&
                                                    on.stats.toneRuns == 0 && off.stats.toneRuns == 0
                                                ? 0
                                                : 1;
                                 },
                                 false, scale);
    }
    std::printf("hw_core2_tone_early_drop: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

int TestCore2OverlapOrder(const gpu::VulkanHandles& handles) {
    const int failures = CheckCore2Ab(handles, "overlapping strips and sprites", Core2OverlapCapture(), 1,
                                      "GT4_CORE2_GROUP_DISJOINT", false,
                                      [](const Core2AbRun&, const Core2AbRun&) { return 0; }, false, 1, "1");
    std::printf("hw_core2_overlap_order: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_core2_point_quads (TASK-179): on a device without largePoints core 2 draws each point as two triangles (hw_vertex.vert,
// HWR_POINT_QUAD) instead of a point gl_PointSize = scale wide. The square must be the same: blended points, many of them
// on the same pixels (their order matters), through a GEQUAL depth test that also writes depth, at 2x and 3x. The
// images and GS memory with GT4_CORE2_POINT_QUADS=1 must equal those of the points drawn as points.
cpuref::CaptureWriter Core2PointsCapture() {
    cpuref::CaptureWriter capture;
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xyz = [](uint32_t x, uint32_t y, uint32_t z) { return uint64_t{x} | (uint64_t{y} << 16) | (uint64_t{z} << 32); };
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
    capture.Privileged(0x12000010u, uint64_t{2} << 13);
    capture.Privileged(0x12000070u, uint64_t{1} << 9);
    capture.Privileged(0x12000080u, (uint64_t{63} << 32) | (uint64_t{63} << 44));
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    uint32_t seed = 4242u;
    auto next = [&seed](uint32_t range) {
        seed = seed * 1664525u + 1013904223u;
        return (seed >> 16) % range;
    };
    std::vector<uint64_t> words = {(uint64_t{127} << 16) | (uint64_t{127} << 48), 0x40,  // scissor 128 x 128
                                   0, 0x18,                                                 // XYOFFSET 0
                                   (uint64_t{1} << 16) | (uint64_t{2} << 17), 0x47,         // ZTE, GEQUAL
                                   16, 0x4E,                                                // Z32 at page 16, written
                                   uint64_t{2} << 16, 0x4C,                                 // CT32 at page 0, FBW 2
                                   0x44, 0x42,                                              // (Cs - Cd) * As + Cd
                                   0u | (1u << 3) | (1u << 6), 0x00};                       // points, Gouraud, blend
    uint32_t x = 0, y = 0;
    for (int p = 0; p < 900; ++p) {
        if (p % 3 != 0) {  // two in three on a new pixel, the rest on the one before (sub-pixel positions differ)
            x = next(60 * 16);
            y = next(60 * 16);
        }
        words.push_back(rgbaq(0x10000000u | (next(0x70u) << 24) | next(0x1000000u)));
        words.push_back(0x01);
        words.push_back(xyz(x + next(16), y + next(16), next(0x10000u) << 16));
        words.push_back(0x05);
    }
    ad(words);
    capture.FrameEnd(0);
    return capture;
}

int TestCore2PointQuads(const gpu::VulkanHandles& handles) {
    int failures = 0;
    for (uint32_t scale : {2u, 3u}) {
        const std::string tag = "points at " + std::to_string(scale) + "x";
        failures += CheckCore2Ab(handles, tag.c_str(), Core2PointsCapture(), 1, "GT4_CORE2_POINT_QUADS", false,
                                 [](const Core2AbRun&, const Core2AbRun&) { return 0; }, false, scale, "1");
    }
    std::printf("hw_core2_point_quads: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_core2_texture_replacement (IN-470, chapter 11): an uploaded 64 x 64 CT32 pattern drawn 1:1 into pages 0-1. A first
// run dumps it under its Format P name; a 4x image with that name in a pack folder then replaces it. Each native pixel
// shows a texel of its 4 x 4 cell of the image, in GS memory, and with no pack the run is unchanged.
cpuref::CaptureWriter Core2ReplacementCapture() {
    cpuref::CaptureWriter capture;
    constexpr uint64_t kPatternPage = 16;
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xyz = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    auto uv = [](uint32_t u, uint32_t v) { return uint64_t{u * 16} | (uint64_t{v * 16} << 16); };
    const uint64_t tex0 = uint64_t{kPatternPage * 32} | (uint64_t{1} << 14) | (uint64_t{6} << 26) | (uint64_t{6} << 30) |
                          (uint64_t{1} << 34) | (uint64_t{1} << 35);  // CT32 64 x 64, TBW 1, TCC, decal
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
    capture.Privileged(0x12000010u, uint64_t{2} << 13);
    capture.Privileged(0x12000070u, uint64_t{1} << 9);
    capture.Privileged(0x12000080u, (uint64_t{63} << 32) | (uint64_t{63} << 44));
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    ad({(kPatternPage * 32) << 32 | (uint64_t{1} << 48), 0x50, 0, 0x51, uint64_t{64} | (uint64_t{64} << 32), 0x52, 0,
        0x53});
    std::vector<uint64_t> pixels;
    for (uint32_t n = 0; n < 64 * 64; n += 2) {
        auto texel = [](uint32_t i) { return uint64_t{0x80000000u | ((i * 2654435761u) >> 8)}; };
        pixels.push_back(texel(n) | (texel(n + 1) << 32));
    }
    capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(pixels.size() / 2), true, false, 0, 2, {}, pixels));
    const uint64_t sprite = 6u | (1u << 4) | (1u << 8);  // sprite, TME, FST
    ad({(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18, uint64_t{1} << 17, 0x47, uint64_t{1} << 32, 0x4E,
        uint64_t{1} << 16, 0x4C, tex0, 0x06, 0, 0x14, sprite, 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(0, 0),
        0x05, uv(64, 64), 0x03, xyz(64, 64), 0x05});
    capture.FrameEnd(0);
    return capture;
}

int TestCore2TextureReplacement(const gpu::VulkanHandles& handles) {
    namespace fs = std::filesystem;
    int failures = 0;
    const fs::path work = fs::temp_directory_path() / ("gt4_replacement_test_" + std::to_string(_getpid()));
    const fs::path dumps = work / "dumps", pack = work / "pack";
    std::error_code ignored;
    fs::remove_all(work, ignored);
    fs::create_directories(pack);
    const cpuref::CaptureWriter capture = Core2ReplacementCapture();
    const memory::BufferView page0{0, 1, memory::Psm::Ct32};
    auto replay = [&](Core2AbRun& run, uint32_t scale) {
        std::string error;
        if (!ReplayCore2Ab(handles, capture, "GT4_CORE2_UNUSED_SWITCH", true, run, error, 0, scale) || run.memory.empty() ||
            run.memory.back().empty()) {
            std::printf("  replay failed: %s\n", error.c_str());
            ++failures;
            return false;
        }
        return true;
    };

    // The dump names the texture.
    Core2AbRun plain;
    const bool plainOk = replay(plain, 1);
    _putenv_s("GT4_HW_DUMP_TEXTURES", "1");
    _putenv_s("GT4_HW_TEXTURE_DUMP_DIR", dumps.string().c_str());
    Core2AbRun dumped;
    replay(dumped, 1);
    _putenv_s("GT4_HW_DUMP_TEXTURES", "");
    _putenv_s("GT4_HW_TEXTURE_DUMP_DIR", "");
    std::string name;
    uint64_t dumpCount = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(dumps, ignored)) {
        name = entry.path().filename().string();
        ++dumpCount;
    }
    std::printf("  dump: %llu file(s), %s\n", static_cast<unsigned long long>(dumpCount), name.c_str());
    // bits of CT32 64 x 64: PSM 0, TW 6, TH 6.
    if (dumpCount != 1 || name.find("-00001980.png") == std::string::npos || dumped.stats.replacementDumps != 1) ++failures;
    if (!plainOk || dumped.memory.empty() || dumped.memory.back() != plain.memory.back()) {
        std::printf("  a dump changed the picture\n");
        ++failures;
    }

    // A 256 x 256 image with that name: red and green hold its own x and y.
    if (dumpCount == 1) {
        std::vector<uint8_t> image(256u * 256u * 4u);
        for (uint32_t y = 0; y < 256; ++y) {
            for (uint32_t x = 0; x < 256; ++x) {
                const uint8_t texel[4] = {static_cast<uint8_t>(x), static_cast<uint8_t>(y), 0x55, 0x80};
                std::memcpy(image.data() + (y * 256u + x) * 4u, texel, 4);
            }
        }
        const std::vector<uint8_t> png = hw_gs_replace::testing::EncodePng(256, 256, 8, 6, image, 4);
        std::ofstream(pack / name, std::ios::binary).write(reinterpret_cast<const char*>(png.data()),
                                                           static_cast<std::streamsize>(png.size()));
        _putenv_s("GT4_HW_TEXTURES", pack.string().c_str());
        _putenv_s("GT4_HW_TEXTURES_SYNC", "1");
        for (uint32_t scale : {1u, 3u}) {
            Core2AbRun replaced;
            if (!replay(replaced, scale)) continue;
            memory::LocalMemory local;
            std::memcpy(local.bytes().data(), replaced.memory.back().data(), kGsMemoryBytes);
            uint64_t wrong = 0;
            for (uint32_t y = 0; y < 64; ++y) {
                for (uint32_t x = 0; x < 64; ++x) {
                    const uint32_t word = local.readPixel(page0, x, y);
                    const uint32_t want = 0x80550000u | (4u * y) << 8 | (4u * x);
                    // At 1x a pixel samples at U = x, the first texel of its cell. The reduction at scale takes some
                    // sub-pixel of the cell, so there only the cell is checked.
                    const uint32_t mask = scale == 1 ? 0xFFFFFFFFu : 0xFFFCFCFCu;
                    if ((word & mask) != (want & mask)) {
                        if (wrong++ == 0) std::printf("    first at (%u, %u): %08x, want %08x\n", x, y, word, want);
                    }
                }
            }
            std::printf("  %ux: %llu of 4096 pixels off the image, %llu batches replaced, %llu images loaded\n", scale,
                        static_cast<unsigned long long>(wrong), static_cast<unsigned long long>(replaced.stats.replacementHits),
                        static_cast<unsigned long long>(replaced.stats.replacementLoads));
            if (wrong != 0 || replaced.stats.replacementHits == 0 || replaced.stats.replacementLoads != 1) ++failures;
        }
        _putenv_s("GT4_HW_TEXTURES", "");
        _putenv_s("GT4_HW_TEXTURES_SYNC", "");
    }
    fs::remove_all(work, ignored);
    std::printf("hw_core2_texture_replacement: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// Spec 8.8: a 64 x 64 CT32 pattern drawn into pages 0-1 from an uploaded texture, then one FST sprite that samples
// pages 0-1 themselves and writes their top-left 32 x 32 at half size, as GT4's luminance reduction does in place: pixel
// (x, y) reads texels near (2x, 2y), which the same sprite also writes when they are inside 32 x 32. The reference
// rasterises in order, so every such texel is read before the sprite reaches it; the GPU must read a copy taken before
// the batch. With `selfSampled` false the pattern is also drawn into pages 2-3 and the sprite samples those instead,
// which the sprite never writes: the same result by definition.
cpuref::CaptureWriter Core2SelfTextureCapture(bool selfSampled) {
    cpuref::CaptureWriter capture;
    constexpr uint64_t kPatternPage = 16;
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xyz = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    auto uv = [](uint32_t u, uint32_t v) { return uint64_t{u * 16} | (uint64_t{v * 16} << 16); };
    auto tex0 = [](uint64_t page) {  // CT32 64 x 64, TBW 1, TCC, decal
        return uint64_t{page * 32} | (uint64_t{1} << 14) | (uint64_t{6} << 26) | (uint64_t{6} << 30) |
               (uint64_t{1} << 34) | (uint64_t{1} << 35);
    };
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
    capture.Privileged(0x12000010u, uint64_t{2} << 13);
    capture.Privileged(0x12000070u, uint64_t{1} << 9);
    capture.Privileged(0x12000080u, (uint64_t{63} << 32) | (uint64_t{63} << 44));
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    ad({(kPatternPage * 32) << 32 | (uint64_t{1} << 48), 0x50, 0, 0x51, uint64_t{64} | (uint64_t{64} << 32), 0x52, 0,
        0x53});
    auto texel = [](uint32_t n) {
        const uint32_t x = n % 64, y = n / 64;
        return uint64_t{(x * 4u) | ((y * 4u) << 8) | ((((x ^ y) * 4u) & 0xFFu) << 16) | (0x80u << 24)};
    };
    std::vector<uint64_t> pixels;
    for (uint32_t n = 0; n < 64 * 64; n += 2) pixels.push_back(texel(n) | (texel(n + 1) << 32));
    capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(pixels.size() / 2), true, false, 0, 2, {},
                                                 pixels));
    const uint64_t sprite = 6u | (1u << 4) | (1u << 8);  // sprite, TME, FST
    // The pattern 1:1 into pages 0-1 (a drawn target) and, for the other source, into pages 2-3; then the half-size
    // sprite into pages 0-1.
    const uint64_t source = selfSampled ? 0 : 2;
    ad({(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18, uint64_t{1} << 17, 0x47, uint64_t{1} << 32, 0x4E,
        uint64_t{1} << 16, 0x4C, tex0(kPatternPage), 0x06, 0, 0x14, sprite, 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0),
        0x03, xyz(0, 0), 0x05, uv(64, 64), 0x03, xyz(64, 64), 0x05});
    if (!selfSampled) {
        ad({source | (uint64_t{1} << 16), 0x4C, sprite, 0x00, uv(0, 0), 0x03, xyz(0, 0), 0x05, uv(64, 64), 0x03,
            xyz(64, 64), 0x05});
    }
    ad({uint64_t{1} << 16, 0x4C, 0, 0x3F, tex0(source), 0x06, sprite, 0x00, uv(0, 0), 0x03, xyz(0, 0), 0x05,
        uv(64, 64), 0x03, xyz(32, 32), 0x05});
    capture.FrameEnd(0);
    return capture;
}

// hw_core2_self_texture (spec 8.8, 12x determinism 2026-10-30): the in-place reduction above, with the batch sampling a
// copy (GT4_CORE2_SELF_COPY, on by default), leaves pages 0-1 as the same sprite sampling an untouched second copy of
// the pattern does, at 1x, 2x, 3x and 12x (with internal scaling CORE2 is not bit-exact to the reference, so the second
// copy stands in for it); at 1x both equal the CPU reference; two 12x runs give the same image and memory.
int TestCore2SelfTexture(const gpu::VulkanHandles& handles) {
    int failures = 0;
    constexpr size_t kPagesZeroOneBytes = 2u * 8192u;
    const cpuref::CaptureWriter self = Core2SelfTextureCapture(true), other = Core2SelfTextureCapture(false);
    std::vector<uint8_t> reference;
    {
        cpuref::CpuRenderer renderer;
        cpuref::ReplaySummary summary;
        std::string error;
        if (!cpuref::ReplayCapture(self.Bytes(), renderer, summary,
                                   [&](uint64_t, const cpuref::FrameImage&) {
                                       const auto& bytes = renderer.Memory().Local().bytes();
                                       reference.assign(bytes.begin(), bytes.begin() + kPagesZeroOneBytes);
                                   },
                                   error) ||
            reference.empty()) {
            std::printf("  reference replay failed: %s\n", error.c_str());
            return 1;
        }
    }
    auto pages = [](const Core2AbRun& run) {
        return std::vector<uint8_t>(run.memory.back().begin(), run.memory.back().begin() + kPagesZeroOneBytes);
    };
    auto words = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
        uint64_t differing = 0;
        for (size_t at = 0; at < a.size(); at += 4) differing += std::memcmp(a.data() + at, b.data() + at, 4) != 0;
        return differing;
    };
    std::vector<uint8_t> firstImage, firstMemory;
    for (uint32_t scale : {1u, 2u, 3u, 12u, 12u}) {
        Core2AbRun selfRun, otherRun;
        std::string error;
        if (!ReplayCore2Ab(handles, self, "GT4_CORE2_SELF_COPY", true, selfRun, error, 0, scale) ||
            !ReplayCore2Ab(handles, other, "GT4_CORE2_SELF_COPY", true, otherRun, error, 0, scale) ||
            selfRun.memory.empty() || selfRun.memory.back().empty() || otherRun.memory.empty() ||
            otherRun.memory.back().empty()) {
            std::printf("  %ux: replay failed: %s\n", scale, error.c_str());
            ++failures;
            continue;
        }
        const uint64_t offOther = words(pages(selfRun), pages(otherRun));
        const uint64_t offReference = words(pages(selfRun), reference);
        std::printf("  %ux: pages 0-1 words differing from the second-copy run %llu, from the reference %llu; batches "
                    "that sampled a copy %llu\n",
                    scale, static_cast<unsigned long long>(offOther), static_cast<unsigned long long>(offReference),
                    static_cast<unsigned long long>(selfRun.stats.selfCopies));
        if (offOther != 0 || (scale == 1 && offReference != 0) || selfRun.stats.selfCopies == 0) ++failures;
        if (scale == 12) {
            if (firstImage.empty()) {
                firstImage = selfRun.images.back();
                firstMemory = selfRun.memory.back();
            } else if (selfRun.images.back() != firstImage || selfRun.memory.back() != firstMemory) {
                std::printf("  12x: the second run differs from the first\n");
                ++failures;
            }
        }
    }
    // Information only: without the copy the sprite samples its own target while writing it, a race whose outcome
    // varies by run (no failure either way).
    {
        Core2AbRun raceRun, otherRun;
        std::string error;
        if (ReplayCore2Ab(handles, self, "GT4_CORE2_SELF_COPY", false, raceRun, error, 0, 12) &&
            ReplayCore2Ab(handles, other, "GT4_CORE2_SELF_COPY", true, otherRun, error, 0, 12) &&
            !raceRun.memory.empty() && !raceRun.memory.back().empty() && !otherRun.memory.empty() &&
            !otherRun.memory.back().empty()) {
            std::printf("  12x with GT4_CORE2_SELF_COPY=0: %llu words differ from the second-copy run (race, "
                        "information only)\n",
                        static_cast<unsigned long long>(words(pages(raceRun), pages(otherRun))));
        }
    }
    std::printf("hw_core2_self_texture: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// TASK-182 step 2a (R1): two self-sampling sprites over one 64 x 64 CT32 frame (pages 0-1), after a 64 x 64 pattern
// texture is drawn into it 1:1. The first samples the whole frame and writes its top-left quarter at half size, so the
// copied rectangle meets what it draws (the spec 8.8 case the renderer already had). A SCISSOR write flushes its batch
// under pixels the same sprite would draw anyway, and the second sprite samples the frame's top-right quarter into its
// bottom-left one, so the copied rectangle is disjoint from what it draws (the oracle's D3 path 2 case: a copy nothing
// needs, still copied by both paths here).
cpuref::CaptureWriter Core2SelfCopyDrawCapture() {
    cpuref::CaptureWriter capture;
    constexpr uint64_t kPatternPage = 16;
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xyz = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    auto uv = [](uint32_t u, uint32_t v) { return uint64_t{u * 16} | (uint64_t{v * 16} << 16); };
    auto tex0 = [](uint64_t page) {  // CT32 64 x 64, TBW 1, TCC, decal
        return uint64_t{page * 32} | (uint64_t{1} << 14) | (uint64_t{6} << 26) | (uint64_t{6} << 30) |
               (uint64_t{1} << 34) | (uint64_t{1} << 35);
    };
    auto scissor = [](uint32_t x1, uint32_t y1) {  // SCISSOR_1 at (0, 0) with inclusive corner (x1, y1)
        return uint64_t{x1} << 16 | uint64_t{y1} << 48;
    };
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
    capture.Privileged(0x12000010u, uint64_t{2} << 13);
    capture.Privileged(0x12000070u, uint64_t{1} << 9);
    capture.Privileged(0x12000080u, (uint64_t{63} << 32) | (uint64_t{63} << 44));
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    ad({(kPatternPage * 32) << 32 | (uint64_t{1} << 48), 0x50, 0, 0x51, uint64_t{64} | (uint64_t{64} << 32), 0x52, 0,
        0x53});
    auto texel = [](uint32_t n) {
        const uint32_t x = n % 64, y = n / 64;
        return uint64_t{(x * 4u) | ((y * 4u) << 8) | ((((x ^ y) * 4u) & 0xFFu) << 16) | (0x80u << 24)};
    };
    std::vector<uint64_t> pixels;
    for (uint32_t n = 0; n < 64 * 64; n += 2) pixels.push_back(texel(n) | (texel(n + 1) << 32));
    capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(pixels.size() / 2), true, false, 0, 2, {},
                                                 pixels));
    const uint64_t sprite = 6u | (1u << 4) | (1u << 8);  // sprite, TME, FST
    // The pattern into pages 0-1, under a scissor that encloses every self-sampling sprite.
    ad({scissor(63, 63), 0x40, 0, 0x18, uint64_t{1} << 17, 0x47, uint64_t{1} << 32, 0x4E,
        uint64_t{1} << 16, 0x4C, tex0(kPatternPage), 0x06, 0, 0x14, sprite, 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0),
        0x03, xyz(0, 0), 0x05, uv(64, 64), 0x03, xyz(64, 64), 0x05});
    // Sprite 1: samples pages 0-1 whole (they are the frame), writes their top-left 32 x 32 at half size: the copy meets
    // the draw.
    ad({tex0(0), 0x06, 0, 0x14, sprite, 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(0, 0), 0x05, uv(64, 64),
        0x03, xyz(32, 32), 0x05});
    // A scissor the sprites already fit in, written between them: it flushes the batch before it without changing a pixel,
    // so each sprite is a batch of its own.
    ad({scissor(31, 63), 0x40});
    // Sprite 2: samples texels 32..56 and writes pixels 4..27, so its sampled rectangle (with the copy's border) meets
    // nothing it draws: no copy at all (the oracle's D3 path 2, which the renderer already takes).
    ad({tex0(0), 0x06, 0, 0x14, sprite, 0x00, rgbaq(0x80808080u), 0x01, uv(32, 32), 0x03, xyz(4, 4), 0x05, uv(56, 56),
        0x03, xyz(28, 28), 0x05});
    ad({scissor(63, 63), 0x40});
    // Sprite 3: a 1:1 pass over the frame's bottom-right 16 x 16 (GT4's luminance reduction): the copy meets the draw
    // again, and it is the second copy of the stream.
    ad({tex0(0), 0x06, 0, 0x14, sprite, 0x00, rgbaq(0x80808080u), 0x01, uv(48, 48), 0x03, xyz(48, 48), 0x05, uv(64, 64),
        0x03, xyz(64, 64), 0x05});
    capture.FrameEnd(0);
    return capture;
}

// hw_core2_self_copy_draw (TASK-182 step 2a, R1): GT4_CORE2_SELF_COPY_DRAW=1 draws the spec 8.8 self copy inside the open
// render pass (core2_self_copy.frag) instead of running vkCmdCopyImage after closing it. The stream has two copies (one
// meeting the drawn rectangle at half size, one a 1:1 pass) and one self-sampling sprite whose sampled rectangle is
// disjoint from what it draws and so needs no copy in either path. The two runs must give the same image and the same GS
// memory at 1x and at scale, and the drawn path must be the one that ran.
int TestCore2SelfCopyDraw(const gpu::VulkanHandles& handles) {
    int failures = 0;
    const cpuref::CaptureWriter capture = Core2SelfCopyDrawCapture();
    for (uint32_t scale : {1u, 2u, 3u, 12u}) {
        const std::string tag = " at " + std::to_string(scale) + "x";
        failures += CheckCore2Ab(
            handles, ("self copy draw" + tag).c_str(), capture, 1, "GT4_CORE2_SELF_COPY_DRAW", scale == 1,
            [](const Core2AbRun& off, const Core2AbRun& on) {
                std::printf("  copies %llu (transfer run %llu), of which drawn in the pass %llu (transfer run %llu)\n",
                            static_cast<unsigned long long>(on.stats.selfCopies),
                            static_cast<unsigned long long>(off.stats.selfCopies),
                            static_cast<unsigned long long>(on.stats.selfCopyDraws),
                            static_cast<unsigned long long>(off.stats.selfCopyDraws));
                // Two self-sampling batches need a copy, both drawn in the pass with the switch on and neither with it off.
                return on.stats.selfCopies == 2 && on.stats.selfCopyDraws == 2 && off.stats.selfCopies == 2 &&
                               off.stats.selfCopyDraws == 0
                           ? 0
                           : 1;
            },
            false, scale, "1");
    }
    std::printf("hw_core2_self_copy_draw: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_core2_compute_fills (IN-442): constant fills drawn by core2_fill.comp (GT4_CORE2_COMPUTE_FILLS, on by default)
// against the same batches rasterised (=0) on the core 2 streams with untextured sprites: identical images and GS
// memory at 1x (and equal to the CPU reference where the stream is exact) and at 2x and 3x, with fills counted.
int TestCore2ComputeFills(const gpu::VulkanHandles& handles) {
    int failures = 0;
    auto ran = [](const char* what) {
        return [what](const Core2AbRun& off, const Core2AbRun& on) {
            std::printf("  %s: compute fills %llu (rasterised run %llu)\n", what,
                        static_cast<unsigned long long>(on.stats.computeFills),
                        static_cast<unsigned long long>(off.stats.computeFills));
            return on.stats.computeFills > 0 && off.stats.computeFills == 0 ? 0 : 1;
        };
    };
    const char* kSwitch = "GT4_CORE2_COMPUTE_FILLS";
    _putenv_s("GT4_CORE2_CLEAR_WHOLE", "0");  // IN-494 would answer the whole-target fills before the compute path
    for (uint32_t scale : {1u, 2u, 3u}) {
        const std::string tag = " at " + std::to_string(scale) + "x";
        failures += CheckCore2Ab(handles, ("palette sprites" + tag).c_str(), Core2PaletteSpritesCapture(4), 4, kSwitch,
                                 scale == 1, ran("palette sprites"), false, scale);
        failures += CheckCore2Ab(handles, ("cache stream" + tag).c_str(), CacheFixtureCapture(), kCacheFixtureFrames,
                                 kSwitch, false, ran("cache stream"), false, scale);
        failures += CheckCore2Ab(handles, ("GPU palettes" + tag).c_str(), Core2GpuPaletteCapture(4), 4, kSwitch,
                                 scale == 1, ran("GPU palettes"), false, scale);
    }
    _putenv_s("GT4_CORE2_CLEAR_WHOLE", "");
    std::printf("hw_core2_compute_fills: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_core2_delayed_downloads (IN-439 and IN-441, decision 0073): every frame draws a 2 x 1 sprite of a new colour into a CT32
// target and downloads it (GT4's light flare test pattern). With GT4_CORE2_DELAYED_DOWNLOADS=1 the first download is
// read back exactly and each later one answers with an earlier frame's colour, never its own; with =0 every download
// is the frame's own colour. At 3x.
int TestCore2DelayedDownloads(const gpu::VulkanHandles& handles) {
    int failures = 0;
    const uint32_t colours[6] = {0x80102030u, 0x80405060u, 0x80708090u, 0x80A0B0C0u, 0x80D0E0F0u, 0x80123456u};
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xyz = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    for (bool delayed : {false, true}) {
        _putenv_s("GT4_CORE2_DELAYED_DOWNLOADS", delayed ? "1" : "0");
        _putenv_s("GT4_CORE2_CPU_SPRITES", "0");  // the tiny sprite on the GPU, as GT4's flare tests are
        _putenv_s("GT4_HW_CORE", "2");
        writeback::Renderer renderer;
        _putenv_s("GT4_HW_CORE", "");
        _putenv_s("GT4_CORE2_CPU_SPRITES", "");
        _putenv_s("GT4_CORE2_DELAYED_DOWNLOADS", "");
        renderer.SetScale(3, 3.0f);
        std::string error;
        if (!renderer.Init(handles, error) || !renderer.Core2()) {
            std::printf("hw_core2_delayed_downloads: core 2 init failed: %s\n", error.c_str());
            return 1;
        }
        int wrong = 0;
        for (uint32_t frame = 0; frame < 6; ++frame) {
            std::vector<Qword> stream;
            stream.push_back(PackedTag(14, true, 0, {0xE}));
            stream.push_back(Qword{uint64_t{0} | (uint64_t{1} << 16), 0x4C});            // FRAME_1: FBP 0, FBW 1, CT32
            stream.push_back(Qword{uint64_t{64} | (uint64_t{1} << 32), 0x4E});           // ZBUF_1: ZBP 64, ZMSK
            stream.push_back(Qword{(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40});  // SCISSOR_1: 0..63 x 0..63
            stream.push_back(Qword{0, 0x18});                                             // XYOFFSET_1 = 0
            stream.push_back(Qword{(uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47});    // TEST_1: ZTE, ZTST ALWAYS
            stream.push_back(Qword{6, 0x00});                                             // PRIM: sprite
            stream.push_back(Qword{rgbaq(colours[frame]), 0x01});
            stream.push_back(Qword{xyz(0, 0), 0x05});
            stream.push_back(Qword{xyz(2, 1), 0x05});                                     // kick: 2 x 1 pixels
            stream.push_back(Qword{uint64_t{0} | (uint64_t{1} << 16), 0x50});             // BITBLTBUF: SBP 0, SBW 1, CT32
            stream.push_back(Qword{0, 0x51});                                             // TRXPOS 0, 0
            stream.push_back(Qword{uint64_t{2} | (uint64_t{1} << 32), 0x52});             // TRXREG 2 x 1
            stream.push_back(Qword{1, 0x53});                                             // TRXDIR 1: local -> host
            stream.push_back(Qword{0, 0x7F});                                             // NOP (pads the packet)
            if (!renderer.GifTransfer(1, reinterpret_cast<const uint8_t*>(stream.data()), stream.size() * sizeof(Qword))) {
                std::printf("hw_core2_delayed_downloads: GIF transfer refused\n");
                return 1;
            }
            uint32_t words[2] = {};
            const std::size_t read = renderer.ReadFifo(words, sizeof words);
            bool ok = read == sizeof words && words[0] == words[1];
            if (!delayed || frame == 0) {
                ok = ok && words[0] == colours[frame];
            } else {
                bool earlier = false;
                for (uint32_t k = 0; k < frame; ++k) earlier = earlier || words[0] == colours[k];
                ok = ok && earlier;
            }
            std::printf("  %s frame %u: downloaded %08X %08X (drawn %08X)%s\n", delayed ? "delayed" : "exact", frame,
                        words[0], words[1], colours[frame], ok ? "" : "  WRONG");
            if (!ok) ++wrong;
            renderer.Flush();  // VBlank: the frame's work is submitted
            for (int spin = 0; spin < 200 && delayed; ++spin) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const core2::Core2Counters* stats = renderer.Core2Stats();
        const uint64_t answered = stats ? stats->delayedDownloads : 0;
        std::printf("  %s: delayed answers %llu\n", delayed ? "delayed" : "exact", static_cast<unsigned long long>(answered));
        if (wrong != 0 || renderer.Failed() || (delayed ? answered != 5 : answered != 0)) ++failures;
    }
    // IN-441: with GT4_GS_HIDE_FLARES=1 a 64 x 8 CT32 download (GT4's flare visibility table) is answered as zeros
    // without a readback, and any other download keeps the drawn colour.
    {
        _putenv_s("GT4_GS_HIDE_FLARES", "1");
        _putenv_s("GT4_CORE2_DELAYED_DOWNLOADS", "0");
        _putenv_s("GT4_CORE2_CPU_SPRITES", "0");
        _putenv_s("GT4_HW_CORE", "2");
        writeback::Renderer renderer;
        _putenv_s("GT4_HW_CORE", "");
        _putenv_s("GT4_CORE2_CPU_SPRITES", "");
        _putenv_s("GT4_CORE2_DELAYED_DOWNLOADS", "");
        _putenv_s("GT4_GS_HIDE_FLARES", "");
        renderer.SetScale(3, 3.0f);
        std::string error;
        if (!renderer.Init(handles, error) || !renderer.Core2()) {
            std::printf("hw_core2_delayed_downloads: core 2 init failed: %s\n", error.c_str());
            return 1;
        }
        const uint32_t colour = 0x80336699u;
        int wrong = 0;
        for (const uint32_t height : {8u, 4u}) {  // the flare table shape, then another shape
            std::vector<Qword> stream;
            stream.push_back(PackedTag(14, true, 0, {0xE}));
            stream.push_back(Qword{uint64_t{0} | (uint64_t{1} << 16), 0x4C});
            stream.push_back(Qword{uint64_t{64} | (uint64_t{1} << 32), 0x4E});
            stream.push_back(Qword{(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40});
            stream.push_back(Qword{0, 0x18});
            stream.push_back(Qword{(uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47});
            stream.push_back(Qword{6, 0x00});
            stream.push_back(Qword{rgbaq(colour), 0x01});
            stream.push_back(Qword{xyz(0, 0), 0x05});
            stream.push_back(Qword{xyz(64, 8), 0x05});                                    // 64 x 8 pixels
            stream.push_back(Qword{uint64_t{0} | (uint64_t{1} << 16), 0x50});
            stream.push_back(Qword{0, 0x51});
            stream.push_back(Qword{uint64_t{64} | (uint64_t{height} << 32), 0x52});       // TRXREG 64 x height
            stream.push_back(Qword{1, 0x53});
            stream.push_back(Qword{0, 0x7F});
            if (!renderer.GifTransfer(1, reinterpret_cast<const uint8_t*>(stream.data()), stream.size() * sizeof(Qword))) {
                std::printf("hw_core2_delayed_downloads: GIF transfer refused\n");
                return 1;
            }
            std::vector<uint32_t> words(64 * height, 0xFFFFFFFFu);
            const std::size_t read = renderer.ReadFifo(words.data(), words.size() * 4);
            const uint32_t expected = height == 8 ? 0u : colour;
            uint32_t off = 0;
            for (uint32_t w : words) off += w != expected ? 1u : 0u;
            std::printf("  hidden flares: 64 x %u download, %u of %zu words not %08X%s\n", height, off, words.size(),
                        expected, off == 0 && read == words.size() * 4 ? "" : "  WRONG");
            if (off != 0 || read != words.size() * 4) ++wrong;
            renderer.Flush();
        }
        const core2::Core2Counters* stats = renderer.Core2Stats();
        const uint64_t hidden = stats ? stats->hiddenFlareTables : 0;
        std::printf("  hidden flares: tables hidden %llu\n", static_cast<unsigned long long>(hidden));
        if (wrong != 0 || hidden != 1 || renderer.Failed()) ++failures;
    }
    std::printf("hw_core2_delayed_downloads: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_core2_specialised (IN-437): pipelines specialised per draw state (GT4_CORE2_SPECIALIZE=sync: every batch waits
// for its variant) against the uber pipeline (=0) on the core 2 streams of the other tests: identical images and GS
// memory, and the 1x streams equal to the CPU reference. Without pixel interlock nothing is specialised.
int TestCore2Specialised(const gpu::VulkanHandles& handles) {
    int failures = 0;
    auto ran = [&handles](const char* what) {
        return [what, &handles](const Core2AbRun& off, const Core2AbRun& on) {
            std::printf("  %s: specialised batches %llu (uber run %llu)\n", what,
                        static_cast<unsigned long long>(on.stats.specialisedBatches),
                        static_cast<unsigned long long>(off.stats.specialisedBatches));
            if (off.stats.specialisedBatches != 0) return 1;
            return handles.pixelInterlock && on.stats.specialisedBatches == 0 ? 1 : 0;
        };
    };
    const char* kSwitch = "GT4_CORE2_SPECIALIZE";
    failures += CheckCore2Ab(handles, "cache stream", CacheFixtureCapture(), kCacheFixtureFrames, kSwitch, false,
                             ran("cache stream"), false, 2, "sync");
    failures += CheckCore2Ab(handles, "stream over a GPU-read render", Core2StreamOverRenderCapture(6), 6, kSwitch, true,
                             ran("stream over a GPU-read render"), false, 2, "sync");
    failures += CheckCore2Ab(handles, "GPU palettes", Core2GpuPaletteCapture(4), 4, kSwitch, true, ran("GPU palettes"),
                             false, 2, "sync");
    for (uint32_t scale : {1u, 3u}) {
        failures += CheckCore2Ab(handles, scale == 1 ? "texture from a target at 1x" : "texture from a target at 3x",
                                 Core2TargetTextureCapture(3), 3, kSwitch, scale == 1, ran("texture from a target"),
                                 false, scale, "sync");
        failures += CheckCore2Ab(handles, scale == 1 ? "mip levels at 1x" : "mip levels at 3x", Core2MipCapture(), 1,
                                 kSwitch, false, ran("mip levels"), false, scale, "sync");
    }
    std::printf("hw_core2_specialised: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// What GT4 does every VBlank: textures A and C (CT32, whole blocks), B over part of A, then A again, a T8 texture with whole
// blocks and one that ends inside a block (never remembered); sprites show them and memory is compared.
cpuref::CaptureWriter Core2UploadMemoCapture(uint32_t frames, bool spriteOverA = false) {
    cpuref::CaptureWriter capture;
    constexpr uint64_t kA = 0, kTexA = 16, kTexC = 17, kTexT8 = 18, kClut = 19;  // pages
    auto xyz = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    auto uv = [](uint32_t u, uint32_t v) { return uint64_t{u * 16} | (uint64_t{v * 16} << 16); };
    const uint64_t kSprite = 6, kTme = 1u << 4, kFst = 1u << 8;
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
    capture.Privileged(0x12000010u, uint64_t{2} << 13);
    capture.Privileged(0x12000070u, uint64_t{kA * 32} | (uint64_t{1} << 9));
    capture.Privileged(0x12000080u, (uint64_t{63} << 32) | (uint64_t{63} << 44));
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    auto upload = [&](uint64_t page, uint64_t psm, uint32_t width, uint32_t height, uint32_t seed) {
        ad({(page * 32) << 32 | (uint64_t{1} << 48) | (psm << 56), 0x50, 0, 0x51,
            uint64_t{width} | (uint64_t{height} << 32), 0x52, 0, 0x53});
        const uint32_t bytesPerPixel = psm == 0 ? 4u : 1u;
        const uint32_t qwords = (width * height * bytesPerPixel + 15u) / 16u;
        std::vector<uint64_t> data(qwords * 2);
        uint64_t state = seed;
        for (uint64_t& word : data) word = state = state * 6364136223846793005ull + 1442695040888963407ull;
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(data.size() / 2), true, false, 0, 2, {}, data));
    };
    for (uint32_t frame = 0; frame < frames; ++frame) {
        if (spriteOverA) {
            // IN-528: a small untextured sprite into A's memory outside B (drawn by the CPU into GS memory, IN-407); the next
            // frame's uploads of A, replays from the second frame on, must put A's bytes back.
            ad({kTexA | (uint64_t{1} << 16), 0x4C, kSprite, 0x00, uint64_t{0x80102030u} | (uint64_t{0x3F800000u} << 32), 0x01,
                xyz(40, 16), 0x05, xyz(56, 24), 0x05});
        }
        upload(kTexA, 0x00, 64, 32, 1);
        upload(kTexC, 0x00, 32, 16, 2);
        upload(kTexA, 0x00, 32, 16, 3);  // B over part of A
        upload(kTexA, 0x00, 64, 32, 1);  // and A again
        upload(kClut, 0x00, 16, 16, 4);
        upload(kTexT8, 0x13, 32, 32, 5);
        upload(kTexT8, 0x13, 20, 13, 6 + (frame & 1u));  // not whole blocks; one of two contents
        const uint64_t tex0A = uint64_t{kTexA * 32} | (uint64_t{1} << 14) | (uint64_t{6} << 26) | (uint64_t{5} << 30) |
                               (uint64_t{1} << 34) | (uint64_t{1} << 35);
        const uint64_t tex0T8 = uint64_t{kTexT8 * 32} | (uint64_t{1} << 14) | (uint64_t{0x13} << 20) | (uint64_t{5} << 26) |
                                (uint64_t{5} << 30) | (uint64_t{1} << 34) | (uint64_t{1} << 35) |
                                (uint64_t{kClut * 32} << 37) | (uint64_t{1} << 61);
        ad({(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18, uint64_t{1} << 17, 0x47, uint64_t{1} << 32, 0x4E,
            kA | (uint64_t{1} << 16), 0x4C, tex0A, 0x06, kSprite | kTme | kFst, 0x00, uv(0, 0), 0x03, xyz(0, 0), 0x05,
            uv(32, 16), 0x03, xyz(32, 16), 0x05, tex0T8, 0x06, kSprite | kTme | kFst, 0x00, uv(0, 0), 0x03, xyz(32, 0), 0x05,
            uv(32, 32), 0x03, xyz(64, 32), 0x05});
        capture.FrameEnd(0);
    }
    return capture;
}

// A scene target copied to a CT32 frame and to a CT16 frame by textured sprites: draws that read nothing back.
cpuref::CaptureWriter Core2CopiesCapture(uint32_t frames) {
    cpuref::CaptureWriter capture;
    constexpr uint64_t kA = 0, kB = 8, kScene = 22;  // pages
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xyz = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    auto uv = [](uint32_t u, uint32_t v) { return uint64_t{u * 16} | (uint64_t{v * 16} << 16); };
    const uint64_t kSprite = 6, kTme = 1u << 4, kFst = 1u << 8, kFbw2 = uint64_t{2} << 16, kCt16 = uint64_t{2} << 24;
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
    capture.Privileged(0x12000010u, uint64_t{2} << 13);
    capture.Privileged(0x12000070u, uint64_t{kA * 32} | (uint64_t{2} << 9));
    capture.Privileged(0x12000080u, (uint64_t{127} << 32) | (uint64_t{63} << 44));
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    const uint64_t tex0 = uint64_t{kScene * 32} | (uint64_t{2} << 14) | (uint64_t{7} << 26) | (uint64_t{6} << 30) |
                          (uint64_t{1} << 34) | (uint64_t{1} << 35);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        ad({(uint64_t{127} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18, uint64_t{1} << 17, 0x47, uint64_t{1} << 32, 0x4E,
            kScene | kFbw2, 0x4C, kSprite, 0x00, rgbaq(0x80204060u + frame * 0x00030201u), 0x01, xyz(0, 0), 0x05,
            xyz(128, 64), 0x05, rgbaq(0x80C08040u), 0x01, xyz(8, 4), 0x05, xyz(100, 28), 0x05,
            kA | kFbw2, 0x4C, tex0, 0x06, kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(0, 0),
            0x05, uv(128, 64), 0x03, xyz(128, 64), 0x05,
            kB | kFbw2 | kCt16, 0x4C, tex0, 0x06, kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03,
            xyz(0, 0), 0x05, uv(128, 64), 0x03, xyz(128, 64), 0x05});
        capture.FrameEnd(frame);
    }
    return capture;
}

// hw_core2_rop (IN-481, IN-499): draws through colour attachments against storage writes, CT32 and CT16: the same images
// and GS memory, equal to the CPU reference at 1x.
int TestCore2Rop(const gpu::VulkanHandles& handles) {
    int failures = 0;
    uint64_t ropBatches = 0;
    auto ran = [&ropBatches](const Core2AbRun& off, const Core2AbRun& on) {
        ropBatches += on.stats.ropBatches;
        return off.stats.ropBatches != 0 ? 1 : 0;
    };
    for (uint32_t scale : {1u, 2u}) {
        const std::string tag = "copies at " + std::to_string(scale) + "x";
        failures += CheckCore2Ab(handles, tag.c_str(), Core2CopiesCapture(3), 3, "GT4_CORE2_ROP", scale == 1, ran, false, scale, "1");
    }
    std::printf("  batches through colour attachments: %llu\n", static_cast<unsigned long long>(ropBatches));
    if (ropBatches == 0) ++failures;
    std::printf("hw_core2_rop: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// A 640 x 448 frame filled with half alpha, then GT4's sheen: a small target added over the screen as Cs Ad + Cd.
cpuref::CaptureWriter Core2RoadSheenCapture() {
    cpuref::CaptureWriter capture;
    constexpr uint64_t kA = 0, kSmall = 200;  // pages
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xyz = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    auto uv = [](uint32_t u, uint32_t v) { return uint64_t{u * 16} | (uint64_t{v * 16} << 16); };
    const uint64_t kSprite = 6, kTme = 1u << 4, kAbe = 1u << 6, kFst = 1u << 8;
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
    capture.Privileged(0x12000010u, uint64_t{2} << 13);
    capture.Privileged(0x12000070u, uint64_t{kA * 32} | (uint64_t{10} << 9));
    capture.Privileged(0x12000080u, (uint64_t{639} << 32) | (uint64_t{447} << 44));
    const std::vector<uint64_t> words = {
        (uint64_t{639} << 16) | (uint64_t{447} << 48), 0x40, 0, 0x18, uint64_t{1} << 17, 0x47, uint64_t{1} << 32, 0x4E,
        // the small target and the frame, half alpha
        kSmall | (uint64_t{2} << 16), 0x4C, kSprite, 0x00, rgbaq(0x80604020u), 0x01, xyz(0, 0), 0x05, xyz(128, 128), 0x05,
        kA | (uint64_t{10} << 16), 0x4C, kSprite, 0x00, rgbaq(0x40102030u), 0x01, xyz(0, 0), 0x05, xyz(640, 448), 0x05,
        rgbaq(0x00203040u), 0x01, xyz(0, 200), 0x05, xyz(640, 300), 0x05,
        // the sheen: A = Cs, B = 0, C = Ad, D = Cd
        (uint64_t{2} << 2) | (uint64_t{1} << 4) | (uint64_t{1} << 6), 0x42,
        uint64_t{kSmall * 32} | (uint64_t{2} << 14) | (uint64_t{7} << 26) | (uint64_t{7} << 30) | (uint64_t{1} << 34) | (uint64_t{1} << 35), 0x06,
        kSprite | kTme | kAbe | kFst, 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(0, 0), 0x05, uv(128, 128), 0x03, xyz(640, 448), 0x05};
    capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE}, words));
    capture.FrameEnd(0);
    return capture;
}

// hw_core2_road_sheen (IN-500): the switch skips that pass and changes the picture; it is drawn unless told otherwise.
int TestCore2RoadSheen(const gpu::VulkanHandles& handles) {
    int failures = 0;
    const auto capture = Core2RoadSheenCapture();
    Core2AbRun skipped, drawn, byDefault;
    std::string error;
    if (!ReplayCore2Ab(handles, capture, "GT4_CORE2_ROAD_SHEEN", false, skipped, error, 0, 2) ||
        !ReplayCore2Ab(handles, capture, "GT4_CORE2_ROAD_SHEEN", true, drawn, error, 0, 2, {}, "1") ||
        !ReplayCore2Ab(handles, capture, "GT4_CORE2_ROAD_SHEEN", true, byDefault, error, 0, 2)) {
        std::printf("  replay failed: %s\n", error.c_str());
        return 1;
    }
    std::printf("  passes skipped: switch 0 %llu, switch 1 %llu, default %llu\n", static_cast<unsigned long long>(skipped.stats.roadSheensSkipped),
                static_cast<unsigned long long>(drawn.stats.roadSheensSkipped), static_cast<unsigned long long>(byDefault.stats.roadSheensSkipped));
    if (skipped.stats.roadSheensSkipped == 0 || drawn.stats.roadSheensSkipped != 0 || byDefault.stats.roadSheensSkipped != 0) ++failures;
    if (skipped.images == drawn.images) {
        std::printf("  the pass made no difference\n");
        ++failures;
    }
    if (byDefault.images != drawn.images) {
        std::printf("  the default is not the full picture\n");
        ++failures;
    }
    // IN-503: the plain variant that leaves pixels without destination alpha: the same picture.
    _putenv_s("GT4_CORE2_PLAIN_DISJOINT", "1");
    _putenv_s("GT4_CORE2_SPECIALIZE", "sync");
    Core2AbRun early, late;
    if (!ReplayCore2Ab(handles, capture, "GT4_CORE2_SHEEN_EARLY", true, early, error, 0, 2) ||
        !ReplayCore2Ab(handles, capture, "GT4_CORE2_SHEEN_EARLY", false, late, error, 0, 2)) {
        std::printf("  replay failed: %s\n", error.c_str());
        ++failures;
    }
    _putenv_s("GT4_CORE2_PLAIN_DISJOINT", "");
    _putenv_s("GT4_CORE2_SPECIALIZE", "");
    std::printf("  plain sheens leaving early: on %llu, off %llu\n", static_cast<unsigned long long>(early.stats.sheensEarly),
                static_cast<unsigned long long>(late.stats.sheensEarly));
    if (handles.pixelInterlock && early.stats.sheensEarly == 0) ++failures;
    if (late.stats.sheensEarly != 0 || early.images != late.images || early.images != drawn.images) {
        std::printf("  the early variant changed the picture\n");
        ++failures;
    }
    std::printf("hw_core2_road_sheen: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_core2_whole_clears (IN-494): whole-target fills as clears against drawing them: images and memory unchanged, exact at
// 1x (the default there keeps the top byte of a Z24 word on this GPU).
int TestCore2WholeClears(const gpu::VulkanHandles& handles) {
    int failures = 0;
    auto none = [](const Core2AbRun&, const Core2AbRun&) { return 0; };
    for (uint32_t scale : {1u, 3u}) {
        const std::string tag = " at " + std::to_string(scale) + "x";
        failures += CheckCore2Ab(handles, ("cache stream" + tag).c_str(), CacheFixtureCapture(), kCacheFixtureFrames,
                                 "GT4_CORE2_CLEAR_WHOLE", false, none, false, scale);
        failures += CheckCore2Ab(handles, ("GPU palettes" + tag).c_str(), Core2GpuPaletteCapture(4), 4, "GT4_CORE2_CLEAR_WHOLE",
                                 scale == 1, none, false, scale);
    }
    std::printf("hw_core2_whole_clears: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// Words of GS memory that differ from the CPU reference after the last frame of `capture` (a CoreGS 2.0 run with `abSwitch`
// set to `value`, "" for its default).
uint64_t Core2DeviationsFromReference(const gpu::VulkanHandles& handles, const cpuref::CaptureWriter& capture, uint32_t frames,
                                      const char* abSwitch, const char* value, uint32_t scale, Core2AbRun& run) {
    std::string error;
    if (!ReplayCore2Ab(handles, capture, abSwitch, true, run, error, frames - 1, scale, {}, value)) {
        std::printf("  replay failed: %s\n", error.c_str());
        return ~uint64_t{0};
    }
    std::vector<uint8_t> reference;
    cpuref::CpuRenderer renderer;
    cpuref::ReplaySummary summary;
    if (!cpuref::ReplayCapture(capture.Bytes(), renderer, summary,
                               [&](uint64_t, const cpuref::FrameImage&) {
                                   const auto& bytes = renderer.Memory().Local().bytes();
                                   reference.assign(bytes.begin(), bytes.begin() + kGsMemoryBytes);
                               },
                               error) ||
        run.memory.empty() || run.memory.back().size() != kGsMemoryBytes) {
        std::printf("  reference replay failed: %s\n", error.c_str());
        return ~uint64_t{0};
    }
    uint64_t deviations = 0;
    for (size_t at = 0; at < kGsMemoryBytes; at += 4) {
        if (std::memcmp(run.memory.back().data() + at, reference.data() + at, 4) != 0) ++deviations;
    }
    return deviations;
}

// hw_core2_upload_memo (IN-486): uploads recognised and copied instead of swizzled: identical images and GS memory, equal
// to the CPU reference where the stream is exact, and the uploads really replayed.
int TestCore2UploadMemo(const gpu::VulkanHandles& handles) {
    int failures = 0;
    uint64_t replayed = 0;
    auto ran = [&replayed](const Core2AbRun& off, const Core2AbRun& on) {
        replayed += on.stats.uploadsReplayed;
        return off.stats.uploadsReplayed != 0 ? 1 : 0;
    };
    const char* kSwitch = "GT4_CORE2_UPLOAD_MEMO";
    failures += CheckCore2Ab(handles, "uploads over each other", Core2UploadMemoCapture(6), 6, kSwitch, true, ran);
    failures += CheckCore2Ab(handles, "a CPU sprite over a remembered upload (IN-528)", Core2UploadMemoCapture(6, true), 6, kSwitch,
                             true, ran);
    {  // IN-528: with the sprite's old identities the replay leaves the sprite's pixels in A
        Core2AbRun oldIds;
        _putenv_s("GT4_CORE2_SPRITE_IDS", "0");
        const uint64_t deviations = Core2DeviationsFromReference(handles, Core2UploadMemoCapture(6, true), 6, kSwitch, "", 2, oldIds);
        _putenv_s("GT4_CORE2_SPRITE_IDS", "");
        std::printf("  old sprite identities: words off the reference %llu, CPU sprites %llu\n",
                    static_cast<unsigned long long>(deviations), static_cast<unsigned long long>(oldIds.stats.cpuSpriteBatches));
        if (deviations == 0 || oldIds.stats.cpuSpriteBatches == 0) ++failures;
    }
    std::printf("  uploads replayed: %llu\n", static_cast<unsigned long long>(replayed));
    if (replayed == 0) ++failures;
    std::printf("hw_core2_upload_memo: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// GT4's depth clear (IN-529): a scene drawn with its depth far away, then black sprites of z 0 with FRAME at the depth buffer's
// base as CT24 and ZBUF one page after it, then a sprite with a GEQUAL test that must pass everywhere.
cpuref::CaptureWriter Core2DepthClearCapture(uint32_t frames) {
    cpuref::CaptureWriter capture;
    constexpr uint64_t kScene = 0, kDepth = 8;  // pages, both 128 pixels wide (FBW 2)
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xyz = [](uint32_t x, uint32_t y, uint32_t z) {
        return uint64_t{x * 16} | (uint64_t{y * 16} << 16) | (uint64_t{z} << 32);
    };
    const uint64_t kSprite = 6;
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
    capture.Privileged(0x12000010u, uint64_t{2} << 13);
    capture.Privileged(0x12000070u, uint64_t{kScene * 32} | (uint64_t{2} << 9));
    capture.Privileged(0x12000080u, (uint64_t{127} << 32) | (uint64_t{63} << 44));
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    const uint64_t always = (uint64_t{1} << 16) | (uint64_t{1} << 17), gequal = (uint64_t{1} << 16) | (uint64_t{2} << 17);
    const uint64_t z24 = uint64_t{1} << 24;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        ad({(uint64_t{127} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18,
            // the scene, red, with its depth far away
            kScene | (uint64_t{2} << 16), 0x4C, kDepth | z24, 0x4E, always, 0x47, kSprite, 0x00, rgbaq(0x800000FFu), 0x01,
            xyz(0, 0, 0x100000), 0x05, xyz(128, 64, 0x100000), 0x05,
            // the clear: CT24 over the depth's even page columns, Z24 one page later
            kDepth | (uint64_t{2} << 16) | (uint64_t{1} << 24), 0x4C, (kDepth + 1) | z24, 0x4E, kSprite, 0x00, rgbaq(0), 0x01,
            xyz(0, 0, 0), 0x05, xyz(64, 64, 0), 0x05,
            // green where the depth test passes
            kScene | (uint64_t{2} << 16), 0x4C, kDepth | z24, 0x4E, gequal, 0x47, kSprite, 0x00, rgbaq(0x8000FF00u), 0x01,
            xyz(0, 0, 0x10), 0x05, xyz(128, 64, 0x10), 0x05});
        capture.FrameEnd(frame);
    }
    return capture;
}

// hw_core2_depth_clear (IN-529): with the rewrite, GS memory equals the CPU reference at 1x and 2x (the depth reads 0 and the
// green sprite covers the scene); drawn as written, the depth target keeps the far depth.
int TestCore2DepthClear(const gpu::VulkanHandles& handles) {
    int failures = 0;
    for (uint32_t scale : {1u, 2u}) {
        Core2AbRun on, off;
        const uint64_t onDeviations =
            Core2DeviationsFromReference(handles, Core2DepthClearCapture(2), 2, "GT4_CORE2_DEPTH_CLEAR", "", scale, on);
        const uint64_t offDeviations =
            Core2DeviationsFromReference(handles, Core2DepthClearCapture(2), 2, "GT4_CORE2_DEPTH_CLEAR", "0", scale, off);
        std::printf("  %ux: words off the reference: rewritten %llu, as written %llu; depth clears %llu / %llu\n", scale,
                    static_cast<unsigned long long>(onDeviations), static_cast<unsigned long long>(offDeviations),
                    static_cast<unsigned long long>(on.stats.depthClears), static_cast<unsigned long long>(off.stats.depthClears));
        if (onDeviations != 0 || offDeviations == 0 || on.stats.depthClears == 0 || off.stats.depthClears != 0) ++failures;
    }
    std::printf("hw_core2_depth_clear: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// A black clear, then a white sprite with SCANMSK 2 (even rows left out) and one with SCANMSK 3 (odd rows) beside it.
cpuref::CaptureWriter Core2ScanmskCapture() {
    cpuref::CaptureWriter capture;
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xyz = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    const uint64_t kSprite = 6;
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
    capture.Privileged(0x12000010u, uint64_t{2} << 13);
    capture.Privileged(0x12000070u, uint64_t{1} << 9);
    capture.Privileged(0x12000080u, (uint64_t{63} << 32) | (uint64_t{63} << 44));
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    ad({(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18, uint64_t{1} << 17, 0x47, uint64_t{1} << 32, 0x4E,
        uint64_t{1} << 16, 0x4C, kSprite, 0x00, rgbaq(0x80000000u), 0x01, xyz(0, 0), 0x05, xyz(64, 64), 0x05,
        2, 0x22, kSprite, 0x00, rgbaq(0x80FFFFFFu), 0x01, xyz(0, 0), 0x05, xyz(32, 64), 0x05,
        3, 0x22, kSprite, 0x00, rgbaq(0x80FFFFFFu), 0x01, xyz(32, 0), 0x05, xyz(64, 64), 0x05, 0, 0x22});
    capture.FrameEnd(0);
    return capture;
}

// hw_core2_scanmsk (IN-530): SCANMSK leaves out the rows it names; GS memory equals the CPU reference at 1x.
int TestCore2Scanmsk(const gpu::VulkanHandles& handles) {
    int failures = 0;
    Core2AbRun run;
    const uint64_t deviations = Core2DeviationsFromReference(handles, Core2ScanmskCapture(), 1, "GT4_CORE2_NONE", "", 1, run);
    std::printf("  1x: words off the reference %llu\n", static_cast<unsigned long long>(deviations));
    if (deviations != 0) ++failures;
    std::printf("hw_core2_scanmsk: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// A 64 x 64 CT32 display at page 20 over memory written by an upload `bufferWidthUnits` x 64 pixels wide (1: the display's own
// width; 2: another width).
cpuref::CaptureWriter Core2DisplayMemoryCapture(uint32_t bufferWidthUnits) {
    cpuref::CaptureWriter capture;
    constexpr uint64_t kShown = 20;
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
    capture.Privileged(0x12000010u, uint64_t{2} << 13);
    capture.Privileged(0x12000070u, kShown | (uint64_t{1} << 9));  // DISPFB1: FBP in pages
    capture.Privileged(0x12000080u, (uint64_t{63} << 32) | (uint64_t{63} << 44));
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    const uint32_t width = 64 * bufferWidthUnits, height = 64 / bufferWidthUnits;
    ad({(kShown * 32) << 32 | (uint64_t{bufferWidthUnits} << 48), 0x50, 0, 0x51, uint64_t{width} | (uint64_t{height} << 32), 0x52,
        0, 0x53});
    std::vector<uint64_t> data(width * height / 2);
    uint64_t state = 7;
    for (uint64_t& word : data) word = state = state * 6364136223846793005ull + 1442695040888963407ull;
    capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(data.size() / 2), true, false, 0, 2, {}, data));
    capture.FrameEnd(0);
    return capture;
}

// IN-532: a white CT32 target at page 20, shown; with `overdraw`, a CT16 target drawn over the same memory afterwards.
cpuref::CaptureWriter Core2DisplayStaleCapture(bool overdraw) {
    cpuref::CaptureWriter capture;
    constexpr uint64_t kShown = 20;
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xyz = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    const uint64_t kSprite = 6;
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
    capture.Privileged(0x12000010u, uint64_t{2} << 13);
    capture.Privileged(0x12000070u, kShown | (uint64_t{2} << 9));
    capture.Privileged(0x12000080u, (uint64_t{63} << 32) | (uint64_t{63} << 44));
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    std::vector<uint64_t> words = {(uint64_t{127} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18, uint64_t{1} << 17, 0x47,
                                   uint64_t{1} << 32, 0x4E, kShown | (uint64_t{2} << 16), 0x4C, kSprite, 0x00,
                                   rgbaq(0x80FFFFFFu), 0x01, xyz(0, 0), 0x05, xyz(128, 64), 0x05};
    if (overdraw) {
        for (uint64_t w : {kShown | (uint64_t{2} << 16) | (uint64_t{2} << 24), uint64_t{0x4C}, kSprite, uint64_t{0x00},
                           rgbaq(0x800000FFu), uint64_t{0x01}, xyz(0, 0), uint64_t{0x05}, xyz(128, 64), uint64_t{0x05}}) {
            words.push_back(w);
        }
    }
    ad(words);
    capture.FrameEnd(0);
    return capture;
}

// hw_core2_display_memory (IN-531): a display with no target shows GS memory only when it was written as that buffer
// (GT4_CORE2_DISPLAY_MEMORY=1 shows it anyway).
int TestCore2DisplayMemory(const gpu::VulkanHandles& handles) {
    int failures = 0;
    auto shown = [&](const cpuref::CaptureWriter& capture, const char* value) {
        Core2AbRun run;
        std::string error;
        if (!ReplayCore2Ab(handles, capture, "GT4_CORE2_DISPLAY_MEMORY", true, run, error, 0, 1, {}, value) || run.images.empty()) {
            std::printf("  replay failed: %s\n", error.c_str());
            ++failures;
            return std::ptrdiff_t{-1};
        }
        return std::count_if(run.images.back().begin(), run.images.back().end(), [](uint8_t v) { return v != 0; });
    };
    const std::ptrdiff_t ownWidth = shown(Core2DisplayMemoryCapture(1), "0");
    const std::ptrdiff_t otherWidth = shown(Core2DisplayMemoryCapture(2), "0");
    const std::ptrdiff_t otherWidthShown = shown(Core2DisplayMemoryCapture(2), "1");
    std::printf("  lit bytes: written as the buffer %lld, at another width %lld, the same with the switch %lld\n",
                static_cast<long long>(ownWidth), static_cast<long long>(otherWidth), static_cast<long long>(otherWidthShown));
    if (ownWidth <= 0 || otherWidth != 0 || otherWidthShown <= 0) ++failures;
    // IN-532: a target is shown only while no target of another format drew over it later.
    auto shownTarget = [&](bool overdraw, const char* value) {
        Core2AbRun run;
        std::string error;
        if (!ReplayCore2Ab(handles, Core2DisplayStaleCapture(overdraw), "GT4_CORE2_DISPLAY_NEWEST", true, run, error, 0, 1, {}, value) ||
            run.images.empty()) {
            std::printf("  replay failed: %s\n", error.c_str());
            ++failures;
            return std::ptrdiff_t{-1};
        }
        return std::count_if(run.images.back().begin(), run.images.back().end(), [](uint8_t v) { return v != 0; });
    };
    const std::ptrdiff_t alone = shownTarget(false, ""), overdrawn = shownTarget(true, ""), overdrawnShown = shownTarget(true, "0");
    std::printf("  lit bytes: a target alone %lld, drawn over later by a CT16 target %lld, the same with the switch at 0 %lld\n",
                static_cast<long long>(alone), static_cast<long long>(overdrawn), static_cast<long long>(overdrawnShown));
    if (alone <= 0 || overdrawn != 0 || overdrawnShown <= 0) ++failures;
    std::printf("hw_core2_display_memory: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_core2_prepare_shaders (IN-507): the launcher's compile before a session: work on a new cache, none when it is ready,
// work again when the stamp or the compiled file is damaged or gone.
int TestCore2PrepareShaders(const gpu::VulkanHandles& handles) {
    namespace fs = std::filesystem;
    const fs::path folder = fs::temp_directory_path() / ("gt4_prepare_shaders_test_" + std::to_string(_getpid()));
    std::error_code ignored;
    fs::remove_all(folder, ignored);
    _putenv_s("GT4_HW_SHADER_CACHE", folder.string().c_str());
    _putenv_s("GT4_HW_SHADER_PREPARE", "1");
    int failures = 0;
    std::size_t total = 0;
    auto prepare = [&](const char* what, bool expectWork) {
        core2::Gpu renderer;
        std::string error;
        if (!renderer.Init(handles, 1, 1.0f, error)) {
            std::printf("  %s: init failed: %s\n", what, error.c_str());
            ++failures;
            return;
        }
        bool worked = false;
        std::size_t last = 0;
        const bool ready = renderer.PrepareShaders([&](std::size_t done, std::size_t all) { worked = true; last = done; total = all; }, error);
        std::printf("  %s: ready %d, worked %d, %zu of %zu\n", what, ready ? 1 : 0, worked ? 1 : 0, last, total);
        if (!ready || worked != expectWork || (worked && last != total)) ++failures;
    };
    prepare("a new cache", true);
    if (total == 0 || !fs::exists(folder / "prepared.bin", ignored)) ++failures;
    prepare("ready", false);
    fs::resize_file(folder / "prepared.bin", 3u, ignored);
    prepare("a cut stamp", true);
    {
        std::ofstream garbage(folder / "pipelines.bin", std::ios::binary | std::ios::trunc);
        garbage << "this is no pipeline cache of any driver";
    }
    prepare("a damaged cache", true);
    fs::remove(folder / "variants.bin", ignored);
    prepare("no list", true);
    prepare("ready again", false);
    _putenv_s("GT4_HW_SHADER_CACHE", "");
    _putenv_s("GT4_HW_SHADER_PREPARE", "");
    fs::remove_all(folder, ignored);
    std::printf("hw_core2_prepare_shaders: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_core2_shader_guard (TASK-170): a prepare that ends inside a variant compile (a driver that ends the process there,
// simulated with GT4_CORE2_TEST_COMPILE_ABANDON) leaves the guard; the next prepare compiles one variant at a time (a
// parallel run left several keys in flight) or marks the one key in flight a suspect (TASK-170c); the same key ending a
// second run is quarantined, and a later prepare finishes without the quarantined variants.
int TestCore2ShaderGuard(const gpu::VulkanHandles& handles) {
    namespace fs = std::filesystem;
    const fs::path folder = fs::temp_directory_path() / ("gt4_shader_guard_test_" + std::to_string(_getpid()));
    std::error_code ignored;
    fs::remove_all(folder, ignored);
    _putenv_s("GT4_HW_SHADER_CACHE", folder.string().c_str());
    _putenv_s("GT4_HW_SHADER_PREPARE", "1");
    int failures = 0;
    struct Run {
        bool ready = false;
        std::size_t total = 0;
        core2::Gpu::ShaderGuardState guard;
    };
    auto prepare = [&](const char* what, const char* abandonAt) {
        _putenv_s("GT4_CORE2_TEST_COMPILE_ABANDON", abandonAt);
        Run run;
        core2::Gpu renderer;
        std::string error;
        if (!renderer.Init(handles, 1, 1.0f, error)) {
            std::printf("  %s: init failed: %s\n", what, error.c_str());
            ++failures;
            return run;
        }
        run.guard = renderer.ShaderGuard();
        run.ready = renderer.PrepareShaders([&](std::size_t, std::size_t all) { run.total = all; }, error);
        std::printf("  %s: ready %d, %zu variants, %zu quarantined, %zu suspects, serial %d\n", what, run.ready ? 1 : 0,
                    run.total, run.guard.quarantined, run.guard.suspects, run.guard.serial ? 1 : 0);
        return run;
    };
    const Run clean = prepare("a clean prepare", "");
    if (!clean.ready || clean.total < 4 || clean.guard.quarantined != 0 || clean.guard.serial) ++failures;
    fs::remove_all(folder, ignored);

    // A run that ends inside its 4th compile leaves compiling.bin.
    const Run died = prepare("ends inside a compile", "4");
    if (died.ready || !fs::exists(folder / "compiling.bin", ignored)) ++failures;
    // The next start reads it: one prepare at a time from now on. One key in flight is only a suspect (TASK-170c), never
    // quarantined at the first end; several keys name nobody. The suspect compiles again and is cleared.
    const Run after = prepare("after it", "");
    if (!after.ready || !after.guard.serial || after.guard.quarantined != 0 || after.guard.suspects > 1 ||
        fs::exists(folder / "compiling.bin", ignored) || fs::exists(folder / "suspects.bin", ignored)) {
        ++failures;
    }
    if (after.total != clean.total) ++failures;

    // Strike one: a one-at-a-time run ends inside its 2nd compile. That key is a suspect, not quarantined...
    fs::remove(folder / "prepared.bin", ignored);
    const Run diedSerial = prepare("one at a time, ends inside a compile", "2");
    if (diedSerial.ready || !fs::exists(folder / "compiling.bin", ignored)) ++failures;
    fs::remove(folder / "prepared.bin", ignored);
    const Run suspected = prepare("after strike one", "");
    if (!suspected.ready || !suspected.guard.serial || suspected.guard.suspects != 1 || suspected.guard.quarantined != 0 ||
        suspected.total != clean.total) {
        ++failures;
    }
    // ...it was compiled again (the total is the whole list) and, compiling fine, is cleared.
    if (fs::exists(folder / "suspects.bin", ignored) || fs::exists(folder / "quarantine.bin", ignored) == false) ++failures;
    fs::remove(folder / "prepared.bin", ignored);
    const Run cleared = prepare("the suspect compiled fine", "");
    if (cleared.guard.suspects != 0 || cleared.guard.quarantined != 0 || cleared.total != clean.total) ++failures;

    // Two strikes on the same key: it ends two runs in a row (the same 2nd compile of the same list) and is quarantined.
    fs::remove(folder / "prepared.bin", ignored);
    prepare("strike one again", "2");
    fs::remove(folder / "prepared.bin", ignored);
    prepare("strike two", "2");
    fs::remove(folder / "prepared.bin", ignored);
    const Run quarantinedRun = prepare("after strike two", "");
    if (!quarantinedRun.ready || !quarantinedRun.guard.serial || quarantinedRun.guard.quarantined != 1 ||
        quarantinedRun.guard.suspects != 0 || quarantinedRun.total + quarantinedRun.guard.quarantined != clean.total ||
        !fs::exists(folder / "quarantine.bin", ignored) || fs::exists(folder / "suspects.bin", ignored)) {
        ++failures;
    }
    // rc14 files (quarantine.bin and no suspects.bin) load as they are, and the quarantine stays.
    const Run again = prepare("the next start, rc14 layout", "");
    if (again.guard.quarantined != 1 || again.guard.suspects != 0 || !again.guard.serial) ++failures;

    // A new suspect next to the quarantine; another driver's files start clean; a damaged suspects.bin is ignored.
    fs::remove(folder / "prepared.bin", ignored);
    prepare("a different key ends a run", "2");
    fs::remove(folder / "prepared.bin", ignored);
    const Run twoKinds = prepare("a suspect beside the quarantine", "1");
    if (twoKinds.guard.suspects != 1 || twoKinds.guard.quarantined != 1) ++failures;
    if (!fs::exists(folder / "suspects.bin", ignored)) ++failures;
    fs::remove(folder / "compiling.bin", ignored);  // the run that just ended inside its first compile
    auto copyGuard = [&](const char* from, const char* to) {
        std::error_code copyError;
        fs::copy_file(folder / from, folder / to, fs::copy_options::overwrite_existing, copyError);
    };
    copyGuard("suspects.bin", "suspects.keep");
    copyGuard("quarantine.bin", "quarantine.keep");
    for (const char* name : {"suspects.bin", "quarantine.bin"}) {
        std::fstream file(folder / name, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(24);  // the driver version of the header (magic 8, shader set 8, vendor 4, device 4)
        const std::uint32_t otherDriver = 0x7777u;
        file.write(reinterpret_cast<const char*>(&otherDriver), sizeof otherDriver);
    }
    fs::remove(folder / "prepared.bin", ignored);
    const Run otherDriver = prepare("another driver's files", "");
    if (!otherDriver.ready || otherDriver.guard.quarantined != 0 || otherDriver.guard.suspects != 0 || otherDriver.guard.serial ||
        otherDriver.total != clean.total) {
        ++failures;
    }
    copyGuard("suspects.keep", "suspects.bin");
    copyGuard("quarantine.keep", "quarantine.bin");
    {
        std::ofstream garbage(folder / "suspects.bin", std::ios::binary | std::ios::trunc);
        garbage << "not a guard file";
    }
    fs::remove(folder / "prepared.bin", ignored);
    const Run damagedSuspects = prepare("a damaged suspects.bin", "");
    if (!damagedSuspects.ready || damagedSuspects.guard.suspects != 0 || damagedSuspects.guard.quarantined != 1) ++failures;

    // A damaged quarantine.bin is ignored too.
    {
        std::ofstream garbage(folder / "quarantine.bin", std::ios::binary | std::ios::trunc);
        garbage << "not a guard file";
    }
    fs::remove(folder / "prepared.bin", ignored);
    const Run damaged = prepare("a damaged guard file", "");
    if (!damaged.ready || damaged.guard.quarantined != 0 || damaged.guard.serial || damaged.total != clean.total) ++failures;

    _putenv_s("GT4_CORE2_TEST_COMPILE_ABANDON", "");
    _putenv_s("GT4_HW_SHADER_CACHE", "");
    _putenv_s("GT4_HW_SHADER_PREPARE", "");
    fs::remove_all(folder, ignored);
    std::printf("hw_core2_shader_guard: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_core2_shader_cache (IN-489): GT4_HW_SHADER_CACHE keeps the variants between sessions: a second session finds them,
// the pictures do not change, and damaged files only cost the time to compile again.
int TestCore2ShaderCache(const gpu::VulkanHandles& handles) {
    namespace fs = std::filesystem;
    const fs::path folder = fs::temp_directory_path() / ("gt4_shader_cache_test_" + std::to_string(_getpid()));
    std::error_code ignored;
    fs::remove_all(folder, ignored);
    _putenv_s("GT4_HW_SHADER_CACHE", folder.string().c_str());
    int failures = 0;
    const auto capture = CacheFixtureCapture();
    auto session = [&](const char* what, Core2AbRun& run) {
        std::string error;
        if (!ReplayCore2Ab(handles, capture, "GT4_CORE2_NO_SUCH_SWITCH", true, run, error, 0, 2, {}, "")) {
            std::printf("  %s: replay failed: %s\n", what, error.c_str());
            ++failures;
        }
    };
    Core2AbRun first, second, damaged;
    session("first session", first);
    const auto variantsSize = fs::file_size(folder / "variants.bin", ignored);
    const auto compiledSize = fs::file_size(folder / "pipelines.bin", ignored);
    std::printf("  after the first session: variants.bin %llu bytes, pipelines.bin %llu bytes\n",
                static_cast<unsigned long long>(variantsSize), static_cast<unsigned long long>(compiledSize));
    if (handles.pixelInterlock && (variantsSize <= 24u || compiledSize == 0u)) ++failures;
    session("second session", second);
    if (second.images != first.images) {
        std::printf("  the second session's pictures differ\n");
        ++failures;
    }
    if (fs::file_size(folder / "variants.bin", ignored) < variantsSize) {
        std::printf("  the second session lost variants\n");
        ++failures;
    }
    // A cut list and a cache of nonsense: refused without any harm.
    fs::resize_file(folder / "variants.bin", 30u, ignored);
    {
        std::ofstream garbage(folder / "pipelines.bin", std::ios::binary | std::ios::trunc);
        garbage << "this is no pipeline cache of any driver";
    }
    session("damaged files", damaged);
    if (damaged.images != first.images) {
        std::printf("  the pictures differ after damaged files\n");
        ++failures;
    }
    _putenv_s("GT4_HW_SHADER_CACHE", "");
    fs::remove_all(folder, ignored);
    std::printf("hw_core2_shader_cache: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_core2_mipmaps (IN-428): the five sprites above at 1x and 3x, every pixel the expected level colour; with
// GT4_CORE2_MIPMAP=0 every sprite shows level 0.
int TestCore2Mipmaps(const gpu::VulkanHandles& handles) {
    int failures = 0;
    struct Expect {
        uint32_t x0, y0;
        uint32_t colour;
        const char* what;
    };
    const Expect expected[] = {{0, 0, 0x8000FF00u, "MMIN 2, LOD 1: level 1"},
                               {16, 0, 0x80FF0000u, "MMIN 2, LOD 2: level 2"},
                               {32, 0, 0x80007F7Fu, "MMIN 3, LOD 0.5: levels 0 and 1 half and half"},
                               {48, 0, 0x800000FFu, "MMIN 1: level 0"},
                               {0, 16, 0x800000FFu, "LOD -1: level 0 (magnification)"}};
    for (uint32_t scale : {1u, 3u}) {
        for (bool mips : {true, false}) {
            Core2AbRun run;
            Core2AbRun aniso;
            std::string error;
            if (!ReplayCore2Ab(handles, Core2MipCapture(), "GT4_CORE2_MIPMAP", mips, run, error, 0, scale) ||
                run.memory.empty() || run.memory.back().empty()) {
                std::printf("  mipmaps %ux: replay failed: %s\n", scale, error.c_str());
                ++failures;
                continue;
            }
            memory::LocalMemory local;
            std::memcpy(local.bytes().data(), run.memory.back().data(), kGsMemoryBytes);
            const memory::BufferView frame{0, 1, memory::Psm::Ct32};
            // IN-434: the stretched STQ sprite, without and with anisotropic filtering.
            if (mips) {
                _putenv_s("GT4_CORE2_ANISO", "16");
                const bool ok = ReplayCore2Ab(handles, Core2MipCapture(), "GT4_CORE2_MIPMAP", true, aniso, error, 0, scale) &&
                                !aniso.memory.empty() && !aniso.memory.back().empty();
                _putenv_s("GT4_CORE2_ANISO", "");
                memory::LocalMemory anisoLocal;
                if (ok) std::memcpy(anisoLocal.bytes().data(), aniso.memory.back().data(), kGsMemoryBytes);
                uint32_t plainWrong = 0, anisoWrong = 0;
                for (uint32_t y = 32; y < 34; ++y) {
                    for (uint32_t x = 1; x < 15; ++x) {  // away from the sprite's ends
                        if (local.readPixel(frame, x, y) != 0x80FF0000u) ++plainWrong;
                        if (!ok || anisoLocal.readPixel(frame, x, y) != 0x800000FFu) ++anisoWrong;
                    }
                }
                std::printf("  mipmaps %ux stretched STQ sprite: level 2 without anisotropy %u of 28 wrong, level 0 with "
                            "16x %u of 28 wrong\n", scale, plainWrong, anisoWrong);
                if (plainWrong != 0 || anisoWrong != 0) ++failures;
            }
            for (const Expect& e : expected) {
                const uint32_t want = mips ? e.colour : 0x800000FFu;
                uint32_t wrong = 0, sample = 0;
                for (uint32_t y = e.y0; y < e.y0 + 16; ++y) {
                    for (uint32_t x = e.x0; x < e.x0 + 16; ++x) {
                        const uint32_t got = local.readPixel(frame, x, y);
                        if (got != want) {
                            if (wrong == 0) sample = got;
                            ++wrong;
                        }
                    }
                }
                std::printf("  mipmaps %ux %s: %s: %u of 256 pixels wrong%s\n", scale, mips ? "on" : "off", e.what, wrong,
                            wrong ? (" (first " + std::to_string(sample) + ")").c_str() : "");
                if (wrong) ++failures;
            }
        }
    }
    std::printf("hw_core2_mipmaps: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_core2_drawn_page_test (IN-508): GT4_CORE2_DRAWN_PAGE_TEST on a write in a drawn target's page row that shares no page
// with its drawn area: no store for it, the picture and memory equal to the run that stores, at 1x and scaled. Without
// the dirty side of the test (whole rows patched from a memory that was not stored) the drawn pixels of that row would
// come back old, and the picture and memory would differ.
int TestCore2DrawnPageTest(const gpu::VulkanHandles& handles) {
    int failures = 0;
    _putenv_s("GT4_CORE2_PATCH_BITS", "0");  // this test counts the stores of the write over the drawn page
    constexpr uint32_t kFrames = 5;
    for (uint32_t scale : {1u, 2u}) {
        failures += CheckCore2Ab(handles, scale == 1 ? "write beside the drawn pages at 1x" : "write beside the drawn pages at 2x",
                                 Core2WriteBesideDrawnCapture(kFrames, true), kFrames, "GT4_CORE2_DRAWN_PAGE_TEST", true,
                                 [](const Core2AbRun& off, const Core2AbRun& on) {
                                     std::printf("  stores before write %llu / %llu, missed %llu / %llu, patched pixels %llu / %llu\n",
                                                 static_cast<unsigned long long>(off.stats.storesBeforeWrite),
                                                 static_cast<unsigned long long>(on.stats.storesBeforeWrite),
                                                 static_cast<unsigned long long>(off.stats.drawnPagesMissed),
                                                 static_cast<unsigned long long>(on.stats.drawnPagesMissed),
                                                 static_cast<unsigned long long>(off.stats.dirtyPatchPixels),
                                                 static_cast<unsigned long long>(on.stats.dirtyPatchPixels));
                                     return off.stats.storesBeforeWrite == kFrames && on.stats.storesBeforeWrite == 2 &&
                                                    off.stats.drawnPagesMissed == 0 && on.stats.drawnPagesMissed == 3 &&
                                                    on.stats.dirtyPatchPixels < off.stats.dirtyPatchPixels
                                                ? 0
                                                : 1;
                                 },
                                 true, scale);
    }
    // A drawn target nothing has read yet is not stored either way, so whole rows lost its drawn pixels (and could delete
    // the target): with the test on, memory equals the reference, and with it off it does not.
    const cpuref::CaptureWriter unread = Core2WriteBesideDrawnCapture(kFrames, false);
    std::vector<uint8_t> reference;
    {
        cpuref::CpuRenderer renderer;
        cpuref::ReplaySummary summary;
        std::string error;
        if (!cpuref::ReplayCapture(unread.Bytes(), renderer, summary,
                                   [&](uint64_t, const cpuref::FrameImage&) {
                                       const auto& bytes = renderer.Memory().Local().bytes();
                                       reference.assign(bytes.begin(), bytes.begin() + kGsMemoryBytes);
                                   },
                                   error)) {
            std::printf("  unread target: reference failed: %s\n", error.c_str());
            ++failures;
        }
    }
    for (const bool on : {false, true}) {
        Core2AbRun run;
        std::string error;
        if (!ReplayCore2Ab(handles, unread, "GT4_CORE2_DRAWN_PAGE_TEST", on, run, error, kFrames - 1, 1) ||
            run.memory.size() != kFrames || reference.size() != kGsMemoryBytes) {
            std::printf("  unread target: replay failed: %s\n", error.c_str());
            ++failures;
            continue;
        }
        uint64_t wrong = 0;
        for (size_t at = 0; at < kGsMemoryBytes; ++at) wrong += run.memory.back()[at] != reference[at];
        std::printf("  unread target, test %s: memory bytes off the reference %llu\n", on ? "on" : "off",
                    static_cast<unsigned long long>(wrong));
        if (on ? wrong != 0 : wrong == 0) ++failures;
    }
    _putenv_s("GT4_CORE2_PATCH_BITS", "");
    std::printf("hw_core2_drawn_page_test: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_core2_width_grace (IN-509): GT4_CORE2_WIDTH_GRACE_LONG on a target of another width drawn every second VBlank: it is
// neither dropped nor stored each time, with the same picture and memory, at 1x and scaled.
int TestCore2WidthGrace(const gpu::VulkanHandles& handles) {
    int failures = 0;
    constexpr uint32_t kFrames = 8;
    for (uint32_t scale : {1u, 2u}) {
        failures += CheckCore2Ab(handles, scale == 1 ? "width every second VBlank at 1x" : "width every second VBlank at 2x",
                                 Core2WidthEverySecondVblankCapture(kFrames), kFrames, "GT4_CORE2_WIDTH_GRACE_LONG", true,
                                 [](const Core2AbRun& off, const Core2AbRun& on) {
                                     std::printf("  GPU-read keeps %llu / %llu, targets replaced %llu / %llu, created %llu / %llu\n",
                                                 static_cast<unsigned long long>(off.stats.gpuReadKeeps),
                                                 static_cast<unsigned long long>(on.stats.gpuReadKeeps),
                                                 static_cast<unsigned long long>(off.stats.targetsReplaced),
                                                 static_cast<unsigned long long>(on.stats.targetsReplaced),
                                                 static_cast<unsigned long long>(off.stats.targetsCreated),
                                                 static_cast<unsigned long long>(on.stats.targetsCreated));
                                     return off.stats.gpuReadKeeps >= 2 && on.stats.gpuReadKeeps == 0 &&
                                                    on.stats.targetsCreated < off.stats.targetsCreated
                                                ? 0
                                                : 1;
                                 },
                                 true, scale);
    }
    std::printf("hw_core2_width_grace: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_core2_glm_memo (IN-510): GT4_CORE2_GLM_MEMO on a palette loaded again and again from a colour and depth pair: GLM writes
// are left out only while nothing changed, with the same picture and memory, at 1x and scaled.
int TestCore2GlmMemo(const gpu::VulkanHandles& handles) {
    int failures = 0;
    constexpr uint32_t kFrames = 3;
    for (uint32_t scale : {1u, 2u}) {
        failures += CheckCore2Ab(handles, scale == 1 ? "repeated palette loads at 1x" : "repeated palette loads at 2x",
                                 Core2MultiTargetClutCapture(kFrames, true), kFrames, "GT4_CORE2_GLM_MEMO", false,
                                 [](const Core2AbRun& off, const Core2AbRun& on) {
                                     std::printf("  GLM writes %llu / %llu, left out %llu / %llu, multi-target loads %llu / %llu\n",
                                                 static_cast<unsigned long long>(off.stats.materializations),
                                                 static_cast<unsigned long long>(on.stats.materializations),
                                                 static_cast<unsigned long long>(off.stats.materializationsLeftOut),
                                                 static_cast<unsigned long long>(on.stats.materializationsLeftOut),
                                                 static_cast<unsigned long long>(off.stats.clutMultiTargetLoads),
                                                 static_cast<unsigned long long>(on.stats.clutMultiTargetLoads));
                                     return off.stats.materializationsLeftOut == 0 && on.stats.materializationsLeftOut > 0 &&
                                                    on.stats.materializations < off.stats.materializations &&
                                                    on.stats.clutMultiTargetLoads == off.stats.clutMultiTargetLoads
                                                ? 0
                                                : 1;
                                 },
                                 true, scale);
    }
    std::printf("hw_core2_glm_memo: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_core2_patch_bits (IN-512): GT4_CORE2_PATCH_BITS on small writes in another format over a drawn page: GT4's T4 pattern
// and T4, T8 and CT16 writes over a GPU-read render: no store for them, with the same picture and memory.
int TestCore2PatchBits(const gpu::VulkanHandles& handles) {
    int failures = 0;
    constexpr uint32_t kFrames = 5;
    for (uint32_t scale : {1u, 2u}) {
        failures += CheckCore2Ab(handles, scale == 1 ? "T4 over the drawn page at 1x" : "T4 over the drawn page at 2x",
                                 Core2WriteBesideDrawnCapture(kFrames, true), kFrames, "GT4_CORE2_PATCH_BITS", true,
                                 [](const Core2AbRun& off, const Core2AbRun& on) {
                                     std::printf("  stores before write %llu / %llu, bit patches %llu / %llu\n",
                                                 static_cast<unsigned long long>(off.stats.storesBeforeWrite),
                                                 static_cast<unsigned long long>(on.stats.storesBeforeWrite),
                                                 static_cast<unsigned long long>(off.stats.bitPatches),
                                                 static_cast<unsigned long long>(on.stats.bitPatches));
                                     return off.stats.storesBeforeWrite > 0 && on.stats.storesBeforeWrite == 0 &&
                                                    off.stats.bitPatches == 0 && on.stats.bitPatches > 0
                                                ? 0
                                                : 1;
                                 },
                                 true, scale);
    }
    failures += CheckCore2Ab(handles, "stream over a GPU-read render", Core2StreamOverRenderCapture(6), 6, "GT4_CORE2_PATCH_BITS", true,
                             [](const Core2AbRun& off, const Core2AbRun& on) {
                                 std::printf("  stream over a render: stores before write %llu / %llu, bit patches %llu / %llu\n",
                                             static_cast<unsigned long long>(off.stats.storesBeforeWrite),
                                             static_cast<unsigned long long>(on.stats.storesBeforeWrite),
                                             static_cast<unsigned long long>(off.stats.bitPatches),
                                             static_cast<unsigned long long>(on.stats.bitPatches));
                                 return on.stats.storesBeforeWrite < off.stats.storesBeforeWrite ? 0 : 1;
                             });
    // The cache stream draws one target over pages that other writes touch in other formats: the page patches of the run
    // without the switch take this stream further from the reference (about 1,900 words at 1x, about 270 with it), so here
    // the check is that memory never moves further from the reference, frame by frame, and at 1x gains no new deviation.
    const cpuref::CaptureWriter stream = CacheFixtureCapture();
    std::vector<std::vector<uint8_t>> reference;
    {
        cpuref::CpuRenderer renderer;
        cpuref::ReplaySummary summary;
        std::string error;
        if (!cpuref::ReplayCapture(stream.Bytes(), renderer, summary,
                                   [&](uint64_t, const cpuref::FrameImage&) {
                                       const auto& bytes = renderer.Memory().Local().bytes();
                                       reference.emplace_back(bytes.begin(), bytes.begin() + kGsMemoryBytes);
                                   },
                                   error)) {
            std::printf("  cache stream: reference failed: %s\n", error.c_str());
            ++failures;
        }
    }
    for (uint32_t scale : {1u, 2u}) {
        Core2AbRun off, on;
        std::string error;
        if (!ReplayCore2Ab(handles, stream, "GT4_CORE2_PATCH_BITS", false, off, error, 0, scale) ||
            !ReplayCore2Ab(handles, stream, "GT4_CORE2_PATCH_BITS", true, on, error, 0, scale) ||
            off.memory.size() != kCacheFixtureFrames || on.memory.size() != kCacheFixtureFrames ||
            reference.size() != kCacheFixtureFrames) {
            std::printf("  cache stream at %ux: replay failed: %s\n", scale, error.c_str());
            ++failures;
            continue;
        }
        for (uint32_t frame = 0; frame < kCacheFixtureFrames; ++frame) {
            uint64_t offWrong = 0, onWrong = 0, newWrong = 0;
            for (size_t at = 0; at < kGsMemoryBytes; at += 4) {
                const bool a = std::memcmp(off.memory[frame].data() + at, reference[frame].data() + at, 4) == 0;
                const bool b = std::memcmp(on.memory[frame].data() + at, reference[frame].data() + at, 4) == 0;
                offWrong += a ? 0u : 1u;
                onWrong += b ? 0u : 1u;
                newWrong += a && !b ? 1u : 0u;
            }
            std::printf("  cache stream at %ux frame %u: words off the reference %llu / %llu, new %llu\n", scale, frame,
                        static_cast<unsigned long long>(offWrong), static_cast<unsigned long long>(onWrong),
                        static_cast<unsigned long long>(newWrong));
            if (onWrong > offWrong || (scale == 1 && newWrong != 0)) ++failures;
        }
    }
    std::printf("hw_core2_patch_bits: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// IN-538 (the car showroom's studio backdrop): a CT32 target at page 22 (FBW 2, 128 x 128, pages 22-29) drawn whole on the
// GPU and read there as a T8 texture, then a large T8 write in the same memory, beyond IN-512's limits:
//   frame % 2 == 0: 128 x 200 from row 0 (pages 22-25; the last 8 rows fill only part of page 25's first block row);
//   frame % 2 == 1: 128 x 150 from row 4 (part of the first and the last block rows).
// The target is then sampled 1:1 in a filled page, in page 25 (the partly written blocks) and in two pages the write never
// touches, whose drawn pixels must survive.
cpuref::CaptureWriter Core2LargeWriteOverDrawnCapture(uint32_t frames) {
    cpuref::CaptureWriter capture;
    constexpr uint64_t kA = 0, kSource = 16, kRender = 22;  // pages
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xyz = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
    auto uv = [](uint32_t u, uint32_t v) { return uint64_t{u * 16} | (uint64_t{v * 16} << 16); };
    auto frameReg = [](uint64_t page, uint64_t fbw) { return page | (fbw << 16); };  // CT32
    auto blit = [](uint64_t dbp, uint64_t dbw, uint64_t dpsm) { return (dbp << 32) | (dbw << 48) | (dpsm << 56); };
    const uint64_t kSprite = 6, kTme = 1u << 4, kFst = 1u << 8;
    auto tex0 = [](uint64_t blocks, uint64_t tbw, uint64_t psm, uint64_t log2, uint64_t tfx) {
        return blocks | (tbw << 14) | (psm << 20) | (log2 << 26) | (log2 << 30) | (uint64_t{1} << 34) | (tfx << 35);
    };
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
    capture.Privileged(0x12000010u, uint64_t{2} << 13);
    capture.Privileged(0x12000070u, uint64_t{kA * 32} | (uint64_t{1} << 9));
    capture.Privileged(0x12000080u, (uint64_t{63} << 32) | (uint64_t{63} << 44));
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    auto image = [&](const std::vector<uint64_t>& data) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(data.size() / 2), true, false, 0, 2, {}, data));
    };
    // A grey ramp palette at page 14 and a one-colour source texture at page 16 (TFX modulate gives the vertex colour).
    ad({blit(14 * 32, 1, 0x00), 0x50, 0, 0x51, uint64_t{16} | (uint64_t{16} << 32), 0x52, 0, 0x53});
    {
        std::vector<uint64_t> entries;
        for (uint32_t i = 0; i < 128; ++i) {
            const uint32_t a = 2 * i, b = 2 * i + 1;
            entries.push_back(uint64_t{0x80000000u | (a * 0x010101u)} | (uint64_t{0x80000000u | (b * 0x010101u)} << 32));
        }
        image(entries);
    }
    ad({blit(kSource * 32, 1, 0x00), 0x50, 0, 0x51, uint64_t{16} | (uint64_t{16} << 32), 0x52, 0, 0x53});
    image(std::vector<uint64_t>(128, uint64_t{0x80808080u} * 0x100000001ull));
    const uint64_t readRender = tex0(kRender * 32, 1, 0x13, 5, 0) | (uint64_t{14 * 32} << 37) | (uint64_t{1} << 61);
    const uint64_t sampleRender = tex0(kRender * 32, 2, 0x00, 7, 1);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        // the whole 128 x 128 target drawn on the GPU, then A cleared and the target read as a T8 texture
        ad({(uint64_t{127} << 16) | (uint64_t{127} << 48), 0x40, 0, 0x18, uint64_t{1} << 17, 0x47, uint64_t{1} << 32, 0x4E,
            frameReg(kRender, 2), 0x4C, tex0(kSource * 32, 1, 0x00, 4, 0), 0x06, kSprite | kTme | kFst, 0x00,
            rgbaq(0x80000000u | ((frame + 1u) * 0x00102030u)), 0x01, uv(0, 0), 0x03, xyz(0, 0), 0x05, uv(16, 16), 0x03,
            xyz(128, 128), 0x05, frameReg(kA, 1), 0x4C, kSprite, 0x00, rgbaq(0x80000000u), 0x01, xyz(0, 0), 0x05,
            xyz(64, 64), 0x05, readRender, 0x06, kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03,
            xyz(0, 0), 0x05, uv(32, 32), 0x03, xyz(32, 32), 0x05});
        // a small sprite sends the read's batch before the write
        ad({sampleRender, 0x06, kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uv(8, 8), 0x03, xyz(0, 60), 0x05,
            uv(12, 12), 0x03, xyz(4, 64), 0x05});
        const uint32_t top = frame % 2 == 0 ? 0u : 4u, rows = frame % 2 == 0 ? 200u : 150u;
        ad({blit(kRender * 32, 2, 0x13), 0x50, uint64_t{top} << 48, 0x51, uint64_t{128} | (uint64_t{rows} << 32), 0x52, 0, 0x53});
        std::vector<uint64_t> words;
        for (uint32_t i = 0; i < 128 * rows / 8; ++i) words.push_back((uint64_t{i + frame} * 0x0102030405060708ull) ^ 0x8040201008040201ull);
        image(words);
        // the target sampled 1:1: a filled page, page 25 (rows 32-63, columns 64-127), and pages 26 and 29 (untouched)
        ad({sampleRender, 0x06, kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uv(8, 8), 0x03, xyz(0, 0), 0x05,
            uv(40, 40), 0x03, xyz(32, 32), 0x05, uv(80, 32), 0x03, xyz(32, 0), 0x05, uv(112, 64), 0x03, xyz(64, 32), 0x05,
            uv(0, 64), 0x03, xyz(0, 32), 0x05, uv(32, 96), 0x03, xyz(32, 64), 0x05, uv(96, 96), 0x03, xyz(32, 32), 0x05,
            uv(128, 128), 0x03, xyz(64, 64), 0x05});
        capture.FrameEnd(frame);
    }
    return capture;
}

// IN-539 (photo mode's uploads over its Z16S depth buffer): page 22 holds a Z16S target (a depth-only sprite over the whole
// page) and a CT32 target (a colour sprite over 32 x 32), both drawn on the GPU; a CSM1 palette loaded from the page reads
// both on the GPU (IN-417, IN-537). Then whole-block writes land in the page: an 8 x 8 CT32 image five blocks in (frame
// % 2 == 0) or a 16 x 16 T8 image nine blocks in (frame % 2 == 1); a palette is loaded again from the page and used, and
// the colour target is sampled as a texture. GS memory is read at the last frame, so a depth buffer that lost the write
// (or whose other blocks were patched from older memory) shows there.
cpuref::CaptureWriter Core2WholeBlockWriteCapture(uint32_t frames) {
    cpuref::CaptureWriter capture;
    constexpr uint64_t kA = 0, kT8 = 18, kShared = 22;  // pages
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xyz = [](uint32_t x, uint32_t y, uint32_t z = 0) {
        return uint64_t{x * 16} | (uint64_t{y * 16} << 16) | (uint64_t{z} << 32);
    };
    auto uv = [](uint32_t u, uint32_t v) { return uint64_t{u * 16} | (uint64_t{v * 16} << 16); };
    auto frameReg = [](uint64_t page) { return page | (uint64_t{1} << 16); };  // FBW 1, CT32
    auto blit = [](uint64_t dbp, uint64_t dbw, uint64_t dpsm) { return (dbp << 32) | (dbw << 48) | (dpsm << 56); };
    const uint64_t kSprite = 6, kTme = 1u << 4, kFst = 1u << 8;
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));
    capture.Privileged(0x12000010u, uint64_t{2} << 13);
    capture.Privileged(0x12000070u, uint64_t{kA * 32} | (uint64_t{1} << 9));
    capture.Privileged(0x12000080u, (uint64_t{63} << 32) | (uint64_t{63} << 44));
    auto ad = [&](const std::vector<uint64_t>& words) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE},
                                                     words));
    };
    auto image = [&](const std::vector<uint64_t>& data) {
        capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(data.size() / 2), true, false, 0, 2, {}, data));
    };
    // A 16 x 16 T8 texture at page 18: every index once.
    ad({blit(kT8 * 32, 2, 0x13), 0x50, 0, 0x51, uint64_t{16} | (uint64_t{16} << 32), 0x52, 0, 0x53});
    {
        std::vector<uint64_t> texels;
        for (uint32_t i = 0; i < 32; ++i) {
            uint64_t word = 0;
            for (uint32_t b = 0; b < 8; ++b) word |= uint64_t{(i * 8 + b) & 255u} << (8 * b);
            texels.push_back(word);
        }
        image(texels);
    }
    const uint64_t zbufMasked = kShared | (uint64_t{1} << 24) | (uint64_t{1} << 32);
    const uint64_t zbufZ16S = kShared | (uint64_t{0xA} << 24);
    auto paletteSprite = [&](uint32_t x, uint32_t y, uint64_t paletteBlock) {
        const uint64_t load = uint64_t{kT8 * 32} | (uint64_t{2} << 14) | (uint64_t{0x13} << 20) | (uint64_t{4} << 26) |
                              (uint64_t{4} << 30) | (uint64_t{1} << 34) | (uint64_t{1} << 35) |
                              ((kShared * 32 + paletteBlock) << 37) | (uint64_t{1} << 61);
        ad({uint64_t{1} << 17, 0x47, zbufMasked, 0x4E, frameReg(kA), 0x4C, load, 0x06, kSprite | kTme | kFst, 0x00,
            rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(x, y), 0x05, uv(16, 16), 0x03, xyz(x + 16, y + 16), 0x05});
    };
    for (uint32_t frame = 0; frame < frames; ++frame) {
        ad({(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18,
            // the colour target: a sprite over 32 x 32 at page 22
            uint64_t{1} << 17, 0x47, zbufMasked, 0x4E, frameReg(kShared), 0x4C, kSprite, 0x00,
            rgbaq(0x80402010u + frame * 0x01030507u), 0x01, xyz(0, 0), 0x05, xyz(32, 32), 0x05,
            // the depth target: Z16S at page 22, a depth-only sprite (colour into A) over the whole page
            (uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47, zbufZ16S, 0x4E, frameReg(kA), 0x4C, kSprite, 0x00,
            rgbaq(0x80000000u), 0x01, xyz(0, 0, 0x1234u + frame * 0x0101u), 0x05, xyz(64, 64, 0x1234u + frame * 0x0101u), 0x05});
        paletteSprite(32, 0, 0);
        std::vector<uint64_t> words;
        if (frame % 2 == 0) {
            ad({blit(kShared * 32 + 5, 1, 0x00), 0x50, 0, 0x51, uint64_t{8} | (uint64_t{8} << 32), 0x52, 0, 0x53});
            for (uint32_t i = 0; i < 32; ++i) words.push_back(uint64_t{0x80C06020u + i * 0x00010203u + frame} * 0x100000001ull);
        } else {
            ad({blit(kShared * 32 + 9, 2, 0x13), 0x50, 0, 0x51, uint64_t{16} | (uint64_t{16} << 32), 0x52, 0, 0x53});
            for (uint32_t i = 0; i < 32; ++i) words.push_back((uint64_t{i + frame} * 0x0102030405060708ull) ^ 0x8040201008040201ull);
        }
        image(words);
        paletteSprite(48, 0, 4);
        paletteSprite(0, 32, 8);
        // the colour target sampled as a texture
        ad({uint64_t{kShared * 32} | (uint64_t{1} << 14) | (uint64_t{5} << 26) | (uint64_t{5} << 30) | (uint64_t{1} << 34) |
                (uint64_t{1} << 35),
            0x06, kSprite | kTme | kFst, 0x00, rgbaq(0x80808080u), 0x01, uv(0, 0), 0x03, xyz(16, 32), 0x05, uv(32, 32), 0x03,
            xyz(48, 64), 0x05});
        capture.FrameEnd(frame);
    }
    return capture;
}

// hw_core2_patch_blocks (IN-538, IN-539): GT4_CORE2_PATCH_BLOCKS on a large T8 write over a drawn, GPU-read CT32 target,
// and GT4_CORE2_WHOLE_BLOCK_WRITES on whole-block writes over a GPU-read Z16S target: no store for them, with the same
// picture and memory.
int TestCore2PatchBlocks(const gpu::VulkanHandles& handles) {
    int failures = 0;
    constexpr uint32_t kFrames = 4;
    for (uint32_t scale : {1u, 2u, 3u}) {
        const std::string name = "large T8 over a drawn target at " + std::to_string(scale) + "x";
        failures += CheckCore2Ab(handles, name.c_str(), Core2LargeWriteOverDrawnCapture(kFrames), kFrames, "GT4_CORE2_PATCH_BLOCKS", false,
                                 [&](const Core2AbRun& off, const Core2AbRun& on) {
                                     std::printf("  %s: stores before write %llu / %llu, block merges %llu / %llu (%llu rectangles)\n",
                                                 name.c_str(), static_cast<unsigned long long>(off.stats.storesBeforeWrite),
                                                 static_cast<unsigned long long>(on.stats.storesBeforeWrite),
                                                 static_cast<unsigned long long>(off.stats.blockPatchWrites),
                                                 static_cast<unsigned long long>(on.stats.blockPatchWrites),
                                                 static_cast<unsigned long long>(on.stats.blockPatchRects));
                                     return off.stats.storesBeforeWrite > 0 && on.stats.storesBeforeWrite == 0 &&
                                                    off.stats.blockPatchWrites == 0 && on.stats.blockPatchWrites == kFrames
                                                ? 0
                                                : 1;
                                 },
                                 true, scale);
    }
    // IN-539 and IN-537 are off by default (measured): both runs read the Z16S palette on the GPU, the "on" run stores
    // with IN-539.
    _putenv_s("GT4_CORE2_GPU_Z16", "1");
    for (uint32_t scale : {1u, 2u}) {
        const std::string name = "whole blocks over a Z16S target at " + std::to_string(scale) + "x";
        failures += CheckCore2Ab(handles, name.c_str(), Core2WholeBlockWriteCapture(kFrames), kFrames, "GT4_CORE2_WHOLE_BLOCK_WRITES", false,
                                 [&](const Core2AbRun& off, const Core2AbRun& on) {
                                     std::printf("  %s: stores before write %llu / %llu, GPU stores %llu / %llu\n",
                                                 name.c_str(), static_cast<unsigned long long>(off.stats.storesBeforeWrite),
                                                 static_cast<unsigned long long>(on.stats.storesBeforeWrite),
                                                 static_cast<unsigned long long>(off.stats.gpuStores),
                                                 static_cast<unsigned long long>(on.stats.gpuStores));
                                     return off.stats.storesBeforeWrite > 0 && on.stats.storesBeforeWrite == 0 &&
                                                    off.stats.gpuStores == 0 && on.stats.gpuStores > 0
                                                ? 0
                                                : 1;
                                 },
                                 true, scale, "1");
    }
    _putenv_s("GT4_CORE2_GPU_Z16", "");
    std::printf("hw_core2_patch_blocks: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_core2_over_target_memo (IN-513): GT4_CORE2_OVER_TARGET_MEMO: a decode over a drawn target is reused in its VBlank only while
// nothing under it changed, with the same picture and memory, at 1x and scaled.
int TestCore2OverTargetMemo(const gpu::VulkanHandles& handles) {
    int failures = 0;
    constexpr uint32_t kFrames = 4;
    for (uint32_t scale : {1u, 2u}) {
        failures += CheckCore2Ab(handles, scale == 1 ? "texture over a target at 1x" : "texture over a target at 2x",
                                 Core2TextureOverTargetCapture(kFrames), kFrames, "GT4_CORE2_OVER_TARGET_MEMO", true,
                                 [](const Core2AbRun& off, const Core2AbRun& on) {
                                     std::printf("  decodes over targets %llu / %llu, reuses %llu / %llu, GLM writes %llu / %llu\n",
                                                 static_cast<unsigned long long>(off.stats.decodesOverTargets),
                                                 static_cast<unsigned long long>(on.stats.decodesOverTargets),
                                                 static_cast<unsigned long long>(off.stats.overTargetReuses),
                                                 static_cast<unsigned long long>(on.stats.overTargetReuses),
                                                 static_cast<unsigned long long>(off.stats.materializations),
                                                 static_cast<unsigned long long>(on.stats.materializations));
                                     return off.stats.overTargetReuses == 0 && on.stats.overTargetReuses > 0 &&
                                                    on.stats.decodesOverTargets < off.stats.decodesOverTargets
                                                ? 0
                                                : 1;
                                 },
                                 true, scale);
    }
    std::printf("hw_core2_over_target_memo: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_core2_cpu_sprites (IN-407): GT4_CORE2_CPU_SPRITES (small untextured sprites drawn by the CPU reference into GS
// memory) on the hw_cache_equals_writeback stream (palettes behind a depth test, T8/T4 textures, uploads, moves, a
// readback) and on GT4's palette-sprite pattern, whose memory must equal the reference exactly and whose CLUT loads
// must need no readback with the switch on.
int TestCore2CpuSprites(const gpu::VulkanHandles& handles) {
    const char* kSwitch = "GT4_CORE2_CPU_SPRITES";
    int failures = CheckCore2Ab(handles, "cache stream", CacheFixtureCapture(), kCacheFixtureFrames, kSwitch, false,
                                [](const Core2AbRun& off, const Core2AbRun& on) {
                                    std::printf("  cache stream: CPU sprite batches %llu / %llu\n",
                                                static_cast<unsigned long long>(off.stats.cpuSpriteBatches),
                                                static_cast<unsigned long long>(on.stats.cpuSpriteBatches));
                                    return off.stats.cpuSpriteBatches == 0 && on.stats.cpuSpriteBatches > 0 ? 0 : 1;
                                });
    failures += CheckCore2Ab(handles, "palettes", Core2PaletteSpritesCapture(4), 4, kSwitch, true,
                             [](const Core2AbRun& off, const Core2AbRun& on) {
                                 std::printf("  palettes: CPU sprite batches %llu / %llu, CLUT readbacks %llu / %llu, "
                                             "targets created %llu / %llu\n",
                                             static_cast<unsigned long long>(off.stats.cpuSpriteBatches),
                                             static_cast<unsigned long long>(on.stats.cpuSpriteBatches),
                                             static_cast<unsigned long long>(off.stats.clutReadbacks),
                                             static_cast<unsigned long long>(on.stats.clutReadbacks),
                                             static_cast<unsigned long long>(off.stats.targetsCreated),
                                             static_cast<unsigned long long>(on.stats.targetsCreated));
                                 return off.stats.cpuSpriteBatches == 0 && on.stats.cpuSpriteBatches > 0 &&
                                                on.stats.clutReadbacks == 0 &&
                                                (off.stats.clutReadbacks + off.stats.clutTargetLoads) > 0
                                            ? 0
                                            : 1;
                             });
    std::printf("hw_core2_cpu_sprites: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_core2_gpu_clut (IN-408): GT4_CORE2_GPU_CLUT (CLUT loads from a drawn target read on the GPU) on GT4's GPU palette
// pattern and on the hw_cache_equals_writeback stream. With the switch on the palette's CLUT loads need no readback,
// they are read from the target on the GPU, the T8 sprites are expanded through the GPU CLUT (palette rows, IN-409),
// and the T4 load from memory is mirrored into it (the last T8 sprite mixes both loads).
int TestCore2GpuClut(const gpu::VulkanHandles& handles) {
    const char* kSwitch = "GT4_CORE2_GPU_CLUT";
    _putenv_s("GT4_CORE2_PATCH_BITS", "0");  // some cases count the stores of a small write over a drawn page
    int failures = CheckCore2Ab(handles, "GPU palettes", Core2GpuPaletteCapture(4), 4, kSwitch, false,
                                [](const Core2AbRun& off, const Core2AbRun& on) {
                                    std::printf("  GPU palettes: CLUT readbacks %llu / %llu, GPU CLUT loads %llu / %llu, "
                                                "palette rows %llu / %llu\n",
                                                static_cast<unsigned long long>(off.stats.clutReadbacks),
                                                static_cast<unsigned long long>(on.stats.clutReadbacks),
                                                static_cast<unsigned long long>(off.stats.clutTargetLoads),
                                                static_cast<unsigned long long>(on.stats.clutTargetLoads),
                                                static_cast<unsigned long long>(off.stats.paletteRowsWritten),
                                                static_cast<unsigned long long>(on.stats.paletteRowsWritten));
                                    return off.stats.clutReadbacks > 0 && off.stats.clutTargetLoads == 0 &&
                                                   on.stats.clutReadbacks == 0 && on.stats.clutTargetLoads > 0 &&
                                                   on.stats.paletteRowsWritten > 0
                                               ? 0
                                               : 1;
                                });
    failures += CheckCore2Ab(handles, "cache stream", CacheFixtureCapture(), kCacheFixtureFrames, kSwitch, false,
                             [](const Core2AbRun&, const Core2AbRun&) { return 0; });
    // IN-409: GT4_CORE2_PALETTE_ROWS (indexed textures kept as indices, colours looked up in palette rows at draw time)
    // must give the same images and memory as palettes expanded into every decode.
    failures += CheckCore2Ab(handles, "palette rows", Core2GpuPaletteCapture(4), 4, "GT4_CORE2_PALETTE_ROWS", false,
                             [](const Core2AbRun& off, const Core2AbRun& on) {
                                 std::printf("  palette rows: rows written %llu / %llu, row hits %llu / %llu, "
                                             "GLM writes %llu / %llu\n",
                                             static_cast<unsigned long long>(off.stats.paletteRowsWritten),
                                             static_cast<unsigned long long>(on.stats.paletteRowsWritten),
                                             static_cast<unsigned long long>(off.stats.paletteRowHits),
                                             static_cast<unsigned long long>(on.stats.paletteRowHits),
                                             static_cast<unsigned long long>(off.stats.materializations),
                                             static_cast<unsigned long long>(on.stats.materializations));
                                 return off.stats.paletteRowsWritten == 0 && on.stats.paletteRowsWritten > 0 &&
                                                on.stats.materializations > 0
                                            ? 0
                                            : 1;
                             });
    failures += CheckCore2Ab(handles, "palette rows, cache stream", CacheFixtureCapture(), kCacheFixtureFrames,
                             "GT4_CORE2_PALETTE_ROWS", false,
                             [](const Core2AbRun&, const Core2AbRun&) { return 0; });
    // IN-408 ageing: a target only read through GS memory on the GPU ages out and keeps its pixels in memory.
    failures += CheckCore2Ab(handles, "GPU-read ageing", Core2GpuReadAgeingCapture(72), 72, kSwitch, false,
                             [](const Core2AbRun& off, const Core2AbRun& on) {
                                 std::printf("  GPU-read ageing: targets aged %llu / %llu, GPU-read keeps %llu / %llu, "
                                             "GLM writes %llu / %llu\n",
                                             static_cast<unsigned long long>(off.stats.targetsAged),
                                             static_cast<unsigned long long>(on.stats.targetsAged),
                                             static_cast<unsigned long long>(off.stats.gpuReadKeeps),
                                             static_cast<unsigned long long>(on.stats.gpuReadKeeps),
                                             static_cast<unsigned long long>(off.stats.materializations),
                                             static_cast<unsigned long long>(on.stats.materializations));
                                 return on.stats.targetsAged > 0 && on.stats.gpuReadKeeps > 0 &&
                                                on.stats.materializations > 0
                                            ? 0
                                            : 1;
                             },
                             true);
    // IN-415: a write replacing every page under a GPU-read target needs no store; the half-page one still stores and
    // the page-exact one needs none either way (IN-416).
    failures += CheckCore2Ab(handles, "stream over a GPU-read render", Core2StreamOverRenderCapture(6), 6,
                             "GT4_CORE2_STORE_SKIP_WHOLE", true,
                             [](const Core2AbRun& off, const Core2AbRun& on) {
                                 std::printf("  stream over a GPU-read render: stores before write %llu / %llu, "
                                             "targets replaced %llu / %llu\n",
                                             static_cast<unsigned long long>(off.stats.storesBeforeWrite),
                                             static_cast<unsigned long long>(on.stats.storesBeforeWrite),
                                             static_cast<unsigned long long>(off.stats.targetsReplaced),
                                             static_cast<unsigned long long>(on.stats.targetsReplaced));
                                 return off.stats.storesBeforeWrite == 4 && on.stats.storesBeforeWrite == 2 ? 0 : 1;
                             });
    // IN-416: a write a whole number of pages away in the same layout patches only its pixels, page by page: no store.
    failures += CheckCore2Ab(handles, "page-exact writes over a GPU-read render", Core2StreamOverRenderCapture(6), 6,
                             "GT4_CORE2_PAGE_EXACT_DIRTY", true,
                             [](const Core2AbRun& off, const Core2AbRun& on) {
                                 std::printf("  page-exact writes: stores before write %llu / %llu, page-exact writes "
                                             "%llu / %llu\n",
                                             static_cast<unsigned long long>(off.stats.storesBeforeWrite),
                                             static_cast<unsigned long long>(on.stats.storesBeforeWrite),
                                             static_cast<unsigned long long>(off.stats.pageExactDirties),
                                             static_cast<unsigned long long>(on.stats.pageExactDirties));
                                 return off.stats.storesBeforeWrite == 4 && on.stats.storesBeforeWrite == 2 &&
                                                off.stats.pageExactDirties == 0 && on.stats.pageExactDirties > 0
                                            ? 0
                                            : 1;
                             });
    // IN-417: a CLUT load over a colour and a Z24 target is read on the GPU from the GLM, not read back.
    failures += CheckCore2Ab(handles, "palette over colour and depth targets", Core2MultiTargetClutCapture(3), 3,
                             "GT4_CORE2_GPU_MULTI_TARGET", false,
                             [](const Core2AbRun& off, const Core2AbRun& on) {
                                 std::printf("  palette over colour and depth targets: CLUT readbacks %llu / %llu, "
                                             "multi-target GPU loads %llu / %llu\n",
                                             static_cast<unsigned long long>(off.stats.clutReadbacks),
                                             static_cast<unsigned long long>(on.stats.clutReadbacks),
                                             static_cast<unsigned long long>(off.stats.clutMultiTargetLoads),
                                             static_cast<unsigned long long>(on.stats.clutMultiTargetLoads));
                                 return off.stats.clutReadbacks > 0 && on.stats.clutReadbacks == 0 &&
                                                off.stats.clutMultiTargetLoads == 0 && on.stats.clutMultiTargetLoads > 0
                                            ? 0
                                            : 1;
                             });
    // IN-537: the same over a Z16S depth target (photo mode's palettes in the memory of its depth buffer).
    failures += CheckCore2Ab(handles, "palette over a Z16S target", Core2WholeBlockWriteCapture(4), 4, "GT4_CORE2_GPU_Z16", false,
                             [](const Core2AbRun& off, const Core2AbRun& on) {
                                 std::printf("  palette over a Z16S target: CLUT readbacks %llu / %llu, multi-target GPU loads "
                                             "%llu / %llu\n",
                                             static_cast<unsigned long long>(off.stats.clutReadbacks),
                                             static_cast<unsigned long long>(on.stats.clutReadbacks),
                                             static_cast<unsigned long long>(off.stats.clutMultiTargetLoads),
                                             static_cast<unsigned long long>(on.stats.clutMultiTargetLoads));
                                 return off.stats.clutReadbacks > 0 && on.stats.clutReadbacks == 0 && on.stats.clutMultiTargetLoads > 0
                                            ? 0
                                            : 1;
                             },
                             false, 2, "1");
    // IN-419/IN-420: barriers only on dependencies and draw batches deferred into one render pass must give the same
    // images and memory as a barrier around every command and each batch recorded at once.
    failures += CheckCore2Ab(handles, "deferred draws, GPU palettes", Core2GpuPaletteCapture(4), 4, "GT4_CORE2_DEFER_DRAWS",
                             true, [](const Core2AbRun&, const Core2AbRun&) { return 0; });
    failures += CheckCore2Ab(handles, "deferred draws, cache stream", CacheFixtureCapture(), kCacheFixtureFrames,
                             "GT4_CORE2_DEFER_DRAWS", false, [](const Core2AbRun&, const Core2AbRun&) { return 0; });
    failures += CheckCore2Ab(handles, "lazy barriers, stream over a GPU-read render", Core2StreamOverRenderCapture(6), 6,
                             "GT4_CORE2_LAZY_BARRIERS", true, [](const Core2AbRun&, const Core2AbRun&) { return 0; });
    // IN-422: textures over no drawn target decoded from a CPU page window must equal the GLM decodes.
    failures += CheckCore2Ab(handles, "decode window, GPU palettes", Core2GpuPaletteCapture(4), 4, "GT4_CORE2_DECODE_WINDOW",
                             true, [](const Core2AbRun&, const Core2AbRun&) { return 0; });
    failures += CheckCore2Ab(handles, "decode window, cache stream", CacheFixtureCapture(), kCacheFixtureFrames,
                             "GT4_CORE2_DECODE_WINDOW", false, [](const Core2AbRun&, const Core2AbRun&) { return 0; });
    // S2: images placed in shared memory blocks must give the same images and memory as one allocation per image.
    failures += CheckCore2Ab(handles, "image memory blocks, cache stream", CacheFixtureCapture(), kCacheFixtureFrames,
                             "GT4_CORE2_IMAGE_BLOCKS", false, [](const Core2AbRun&, const Core2AbRun&) { return 0; });
    failures += CheckCore2Ab(handles, "image memory blocks, GPU-read ageing", Core2GpuReadAgeingCapture(72), 72,
                             "GT4_CORE2_IMAGE_BLOCKS", true, [](const Core2AbRun&, const Core2AbRun&) { return 0; }, true);
    // S4: eight submissions in flight (the switch at "0" means two) must give the same images and memory.
    failures += CheckCore2Ab(handles, "submission slots, cache stream", CacheFixtureCapture(), kCacheFixtureFrames,
                             "GT4_CORE2_SLOTS", false, [](const Core2AbRun&, const Core2AbRun&) { return 0; });
    failures += CheckCore2Ab(handles, "submission slots, GPU palettes", Core2GpuPaletteCapture(4), 4, "GT4_CORE2_SLOTS",
                             true, [](const Core2AbRun&, const Core2AbRun&) { return 0; });
    // TASK-178: the first submission after a waited one (a readback) sent after K batches, here 1, must give the same images
    // and memory as the plain cadence; the rule must have run (submissions by it with K, none off).
    auto afterWaitCounters = [](const char* what) {
        return [what](const Core2AbRun& off, const Core2AbRun& on) {
            std::printf("  %s: drains %llu / %llu, submissions after a wait %llu / %llu\n", what,
                        static_cast<unsigned long long>(off.stats.drains), static_cast<unsigned long long>(on.stats.drains),
                        static_cast<unsigned long long>(off.stats.afterWaitSubmissions),
                        static_cast<unsigned long long>(on.stats.afterWaitSubmissions));
            return off.stats.drains > 0 && off.stats.afterWaitSubmissions == 0 && on.stats.afterWaitSubmissions > 0 ? 0 : 1;
        };
    };
    failures += CheckCore2Ab(handles, "submit after a wait, stream over a GPU-read render", Core2StreamOverRenderCapture(6), 6,
                             "GT4_CORE2_SUBMIT_AFTER_WAIT", true, afterWaitCounters("stream over a GPU-read render"), false, 2,
                             "1");
    failures += CheckCore2Ab(handles, "submit after a wait, GPU-read ageing", Core2GpuReadAgeingCapture(72), 72,
                             "GT4_CORE2_SUBMIT_AFTER_WAIT", true, afterWaitCounters("GPU-read ageing"), true, 2, "1");
    // TASK-182 step 3 (O2): waiting for the newest submission's fence only must give the same images and memory as waiting
    // for every slot, and the newest-fence path must have run on the "on" side and not on the "off" side.
    auto newestFenceCounters = [](const char* what) {
        return [what](const Core2AbRun& off, const Core2AbRun& on) {
            std::printf("  %s: drains %llu / %llu, newest-fence waits %llu / %llu\n", what,
                        static_cast<unsigned long long>(off.stats.drains), static_cast<unsigned long long>(on.stats.drains),
                        static_cast<unsigned long long>(off.stats.newestFenceWaits),
                        static_cast<unsigned long long>(on.stats.newestFenceWaits));
            return off.stats.drains > 0 && off.stats.newestFenceWaits == 0 && on.stats.newestFenceWaits > 0 ? 0 : 1;
        };
    };
    failures += CheckCore2Ab(handles, "newest fence only, stream over a GPU-read render", Core2StreamOverRenderCapture(6), 6,
                             "GT4_CORE2_WAIT_NEWEST_FENCE", true, newestFenceCounters("stream over a GPU-read render"));
    failures += CheckCore2Ab(handles, "newest fence only, GPU-read ageing", Core2GpuReadAgeingCapture(72), 72,
                             "GT4_CORE2_WAIT_NEWEST_FENCE", true, newestFenceCounters("GPU-read ageing"), true);
    // TASK-182 step 3: the bounded spin before the blocking wait polls the fence (and may see it signalled) and changes
    // nothing else; it runs on the newest-fence path, so both runs have that on.
    auto spinCounters = [](const char* what) {
        return [what](const Core2AbRun& off, const Core2AbRun& on) {
            std::printf("  %s: spin polls %llu / %llu, spin hits %llu / %llu\n", what,
                        static_cast<unsigned long long>(off.stats.spinPolls),
                        static_cast<unsigned long long>(on.stats.spinPolls),
                        static_cast<unsigned long long>(off.stats.spinHits),
                        static_cast<unsigned long long>(on.stats.spinHits));
            return off.stats.spinPolls == 0 && on.stats.spinPolls > 0 ? 0 : 1;
        };
    };
    failures += CheckCore2Ab(handles, "readback spin, stream over a GPU-read render", Core2StreamOverRenderCapture(6), 6,
                             "GT4_HW_READBACK_SPIN", true, spinCounters("stream over a GPU-read render"), false, 2, "1",
                             {{"GT4_CORE2_WAIT_NEWEST_FENCE", "1"}});
    failures += CheckCore2Ab(handles, "readback spin, GPU-read ageing", Core2GpuReadAgeingCapture(72), 72,
                             "GT4_HW_READBACK_SPIN", true, spinCounters("GPU-read ageing"), true, 2, "1",
                             {{"GT4_CORE2_WAIT_NEWEST_FENCE", "1"}});
    // IN-429 / IN-430: batched readbacks and merged dirty rectangles give the same images and memory.
    auto readbackCounters = [](const char* what) {
        return [what](const Core2AbRun& off, const Core2AbRun& on) {
            std::printf("  %s: batched readbacks %llu / %llu, dirty merged %llu / %llu, readbacks %llu / %llu\n", what,
                        static_cast<unsigned long long>(off.stats.batchedReadbacks),
                        static_cast<unsigned long long>(on.stats.batchedReadbacks),
                        static_cast<unsigned long long>(off.stats.dirtyMerged),
                        static_cast<unsigned long long>(on.stats.dirtyMerged),
                        static_cast<unsigned long long>(off.stats.readbacks),
                        static_cast<unsigned long long>(on.stats.readbacks));
            return on.stats.batchedReadbacks + on.stats.dirtyMerged > 0 ? 0 : 1;  // the paths ran
        };
    };
    for (const char* abSwitch : {"GT4_CORE2_BATCH_READBACK", "GT4_CORE2_DIRTY_MERGE"}) {
        failures += CheckCore2Ab(handles, abSwitch, CacheFixtureCapture(), kCacheFixtureFrames, abSwitch, false,
                                 readbackCounters("cache stream"));
        failures += CheckCore2Ab(handles, abSwitch, Core2StreamOverRenderCapture(6), 6, abSwitch, true,
                                 readbackCounters("stream over a GPU-read render"));
        failures += CheckCore2Ab(handles, abSwitch, Core2GpuReadAgeingCapture(72), 72, abSwitch, false,
                                 readbackCounters("GPU-read ageing"));
    }
    // IN-427: a texture taken from a target at 1x gives the CPU reference's memory exactly (at 2x the scaled raster is not
    // bit-identical to the 1x reference by design: no new deviation between the runs).
    for (uint32_t scale : {1u, 2u}) {
        failures += CheckCore2Ab(handles, scale == 1 ? "texture from a target at 1x" : "texture from a target at 2x",
                                 Core2TargetTextureCapture(3), 3, "GT4_CORE2_DEFER_DRAWS", scale == 1,
                                 [](const Core2AbRun& off, const Core2AbRun& on) {
                                     std::printf("  texture from a target: from targets %llu / %llu, from memory %llu / %llu\n",
                                                 static_cast<unsigned long long>(off.stats.texturesFromTargets),
                                                 static_cast<unsigned long long>(on.stats.texturesFromTargets),
                                                 static_cast<unsigned long long>(off.stats.texturesFromMemory),
                                                 static_cast<unsigned long long>(on.stats.texturesFromMemory));
                                     return off.stats.texturesFromTargets > 0 && on.stats.texturesFromTargets > 0 ? 0 : 1;
                                 },
                                 false, scale);
    }
    _putenv_s("GT4_CORE2_PATCH_BITS", "");
    std::printf("hw_core2_gpu_clut: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_core2_sync_trace (TASK-182): GT4_CORE2_BARRIER_TRACE (counts of barriers, pass ends, flushes, submissions and descriptor
// traffic) and GT4_CORE2_GPU_CATEGORIES (a timestamp at each change of kind of work) are diagnostics only: the images and
// the GS memory of every frame must be identical with each switch on and off.
int TestCore2SyncTrace(const gpu::VulkanHandles& handles) {
    int failures = 0;
    for (const char* diagnostics : {"GT4_CORE2_BARRIER_TRACE", "GT4_CORE2_GPU_CATEGORIES"}) {
        failures += CheckCore2Ab(handles, diagnostics, CacheFixtureCapture(), kCacheFixtureFrames, diagnostics, false,
                                 [](const Core2AbRun&, const Core2AbRun&) { return 0; });
        failures += CheckCore2Ab(handles, diagnostics, Core2GpuPaletteCapture(4), 4, diagnostics, false,
                                 [](const Core2AbRun&, const Core2AbRun&) { return 0; });
    }
    std::printf("hw_core2_sync_trace: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_core2_data_paths (TASK-180 test4, STUDY-UNIFIED-MEMORY P1-C/P1-D): the per-path byte counters and the spec-required
// host-read barrier (GT4_CORE2_HOST_READ_BARRIER). The barrier adds a dependency into the host domain before a host read;
// it changes no command's data, so every frame's image and the GS memory must be identical with the switch off and on.
// The counters must see the traffic the captures really carry (vertices, uniforms, GLM or window decodes, the readbacks),
// each ring's high-water mark must stay inside its share, and the barrier must be recorded exactly on the runs that read
// back to the host.
int TestCore2DataPaths(const gpu::VulkanHandles& handles) {
    int failures = 0;
    for (uint32_t scale : {1u, 2u}) {
        auto run = [&](const char* name, const cpuref::CaptureWriter& capture, uint32_t frames) {
            Core2AbRun off, on;
            std::string error;
            if (!ReplayCore2Ab(handles, capture, "GT4_CORE2_HOST_READ_BARRIER", false, off, error, 0, scale) ||
                !ReplayCore2Ab(handles, capture, "GT4_CORE2_HOST_READ_BARRIER", true, on, error, 0, scale)) {
                std::printf("  %s at %ux: replay failed: %s\n", name, scale, error.c_str());
                ++failures;
                return;
            }
            if (off.images.size() != frames || on.images.size() != frames || off.memory.size() != on.memory.size()) {
                std::printf("  %s at %ux: frame counts %zu / %zu\n", name, scale, off.images.size(), on.images.size());
                ++failures;
                return;
            }
            uint64_t differingImages = 0, differingMemory = 0;
            for (uint32_t frame = 0; frame < frames; ++frame) {
                if (off.images[frame] != on.images[frame]) ++differingImages;
                if (frame < off.memory.size() && off.memory[frame] != on.memory[frame]) ++differingMemory;
            }
            if (differingImages != 0 || differingMemory != 0) {
                std::printf("  %s at %ux: the host-read barrier changed %llu image(s) and %llu memory frame(s)\n", name, scale,
                            static_cast<unsigned long long>(differingImages),
                            static_cast<unsigned long long>(differingMemory));
                ++failures;
            }
            // The switch-off run must record no host-domain dependency at all; the switch-on run needs one for each wait
            // that ends in a host read.
            if (off.gpu.hostReadBarriers != 0) {
                std::printf("  %s at %ux: %llu host-read barriers with the switch off\n", name, scale,
                            static_cast<unsigned long long>(off.gpu.hostReadBarriers));
                ++failures;
            }
            const bool readBack = on.stats.readbacks > 0 || on.gpu.hostReadBytes > 0;
            if (readBack && on.gpu.hostReadBarriers == 0) {
                std::printf("  %s at %ux: a readback with no host-read barrier\n", name, scale);
                ++failures;
            }
            if (!readBack && on.gpu.hostReadBarriers != 0) {
                std::printf("  %s at %ux: %llu host-read barriers without a readback\n", name, scale,
                            static_cast<unsigned long long>(on.gpu.hostReadBarriers));
                ++failures;
            }
            // What a drawn capture must have written through the CPU: vertices and uniforms on every draw, and the GLM
            // uploads or the decode windows.
            if (on.gpu.cpuVertexBytes == 0 || on.gpu.cpuUniformBytes == 0) {
                std::printf("  %s at %ux: vertices %llu bytes, uniforms %llu bytes\n", name, scale,
                            static_cast<unsigned long long>(on.gpu.cpuVertexBytes),
                            static_cast<unsigned long long>(on.gpu.cpuUniformBytes));
                ++failures;
            }
            if (on.gpu.cpuGlmBytes + on.gpu.windowBytes == 0) {
                std::printf("  %s at %ux: no GLM upload and no decode window\n", name, scale);
                ++failures;
            }
            if (on.stats.readbacks > 0 && on.gpu.hostReadBytes < 4u * on.stats.readbackPixels) {
                std::printf("  %s at %ux: host reads %llu bytes for %llu readback pixels\n", name, scale,
                            static_cast<unsigned long long>(on.gpu.hostReadBytes),
                            static_cast<unsigned long long>(on.stats.readbackPixels));
                ++failures;
            }
            if (on.gpu.ringShareStagingBytes == 0 || on.gpu.ringHighStaging > on.gpu.ringShareStagingBytes ||
                on.gpu.ringHighVertex > on.gpu.ringShareVertexBytes ||
                on.gpu.ringHighUniform > on.gpu.ringShareUniformBytes) {
                std::printf("  %s at %ux: ring high-water staging %llu/%llu, vertex %llu/%llu, uniform %llu/%llu\n", name,
                            scale, static_cast<unsigned long long>(on.gpu.ringHighStaging),
                            static_cast<unsigned long long>(on.gpu.ringShareStagingBytes),
                            static_cast<unsigned long long>(on.gpu.ringHighVertex),
                            static_cast<unsigned long long>(on.gpu.ringShareVertexBytes),
                            static_cast<unsigned long long>(on.gpu.ringHighUniform),
                            static_cast<unsigned long long>(on.gpu.ringShareUniformBytes));
                ++failures;
            }
            std::printf("  %s at %ux: images and memory identical (host-read barriers %llu on / %llu off, vertex %llu B, "
                        "uniform %llu B, glm %llu B, window %llu B, image %llu B, host reads %llu B)\n",
                        name, scale, static_cast<unsigned long long>(on.gpu.hostReadBarriers),
                        static_cast<unsigned long long>(off.gpu.hostReadBarriers),
                        static_cast<unsigned long long>(on.gpu.cpuVertexBytes),
                        static_cast<unsigned long long>(on.gpu.cpuUniformBytes),
                        static_cast<unsigned long long>(on.gpu.cpuGlmBytes),
                        static_cast<unsigned long long>(on.gpu.windowBytes),
                        static_cast<unsigned long long>(on.gpu.cpuImageBytes),
                        static_cast<unsigned long long>(on.gpu.hostReadBytes));
        };
        run("cache stream", CacheFixtureCapture(), kCacheFixtureFrames);
        run("GPU palettes", Core2GpuPaletteCapture(4), 4);
        run("stream over a GPU-read render", Core2StreamOverRenderCapture(6), 6);
    }
    std::printf("hw_core2_data_paths: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// TASK-180 test5: replays a capture under an arbitrary set of environment switches (the effect toggles), for the
// equivalence checks below. The switches are read at init, so they are cleared as soon as the renderer exists.
bool ReplayWithEnv(const gpu::VulkanHandles& handles, const cpuref::CaptureWriter& capture,
                   const std::vector<std::pair<const char*, const char*>>& env, Core2AbRun& out, std::string& error,
                   uint32_t scale = 2) {
    for (const auto& [name, value] : env) _putenv_s(name, value);
    _putenv_s("GT4_HW_CORE", "2");
    writeback::Renderer renderer;
    _putenv_s("GT4_HW_CORE", "");
    renderer.SetScale(scale, static_cast<float>(scale));
    const bool ready = renderer.Init(handles, error);
    for (const auto& [name, value] : env) {
        (void)value;
        _putenv_s(name, "");
    }
    if (!ready) return false;
    if (!renderer.Core2()) {
        error = "GT4_HW_CORE=2 did not select core 2";
        return false;
    }
    writeback::WritebackReplayTarget target(renderer);
    cpuref::ReplaySummary summary;
    const bool ok = cpuref::ReplayCapture(capture.Bytes(), target, summary,
                                          [&](uint64_t, const cpuref::FrameImage& image) {
                                              std::vector<uint8_t> rgb;
                                              for (const display::Rgba8& p : image.image.pixels) {
                                                  rgb.push_back(p.r);
                                                  rgb.push_back(p.g);
                                                  rgb.push_back(p.b);
                                              }
                                              out.images.push_back(std::move(rgb));
                                              const uint8_t* slm = renderer.Slm();
                                              out.memory.emplace_back(slm, slm + kGsMemoryBytes);
                                          },
                                          error);
    if (const core2::Core2Counters* stats = renderer.Core2Stats()) out.stats = *stats;
    if (const core2::Core2GpuStats* gpu = renderer.Core2GpuCounters()) out.gpu = *gpu;
    if (renderer.Failed()) error += " " + renderer.LastError();
    return ok && !renderer.Failed();
}

// hw_core2_effect_toggles (TASK-180 test5, owner 2026-11-05): Performance Mode is a set of independent effect switches now.
// Each new GT4_CORE2_PERF_<ITEM> switch must mean exactly what the older A/B switch of that item means (the same picture
// and the same GS memory), every effect on must be the picture of today's accurate mode, and every item together must be
// what GT4_CORE2_PERFORMANCE=1 did. The flare table is answered as zeros under the new name, on a 64 x 8 download.
int TestCore2EffectToggles(const gpu::VulkanHandles& handles) {
    int failures = 0;
    std::string error;
    const auto sheen = Core2RoadSheenCapture();
    Core2AbRun new_off, old_off, bundle, all_on;
    const bool replayed = ReplayWithEnv(handles, sheen, {{"GT4_CORE2_PERF_ROAD_SHEEN", "1"}}, new_off, error) &&
                          ReplayWithEnv(handles, sheen, {{"GT4_CORE2_ROAD_SHEEN", "0"}}, old_off, error) &&
                          ReplayWithEnv(handles, sheen,
                                        {{"GT4_CORE2_PERF_ROAD_SHEEN", "1"}, {"GT4_CORE2_PERF_FLARES", "1"}}, all_on,
                                        error) &&
                          ReplayWithEnv(handles, sheen, {{"GT4_CORE2_PERFORMANCE", "1"}}, bundle, error);
    if (!replayed) {
        std::printf("  replay failed: %s\n", error.c_str());
        ++failures;
    } else {
        // 1. The new road sheen switch is the old one.
        const bool same_old = new_off.images == old_off.images && new_off.memory == old_off.memory;
        std::printf("  road sheen: new switch skips %llu pass(es), old switch %llu, pictures %s\n",
                    static_cast<unsigned long long>(new_off.gpu.roadSheensSkipped),
                    static_cast<unsigned long long>(old_off.gpu.roadSheensSkipped), same_old ? "identical" : "DIFFER");
        if (!same_old || new_off.gpu.roadSheensSkipped == 0) ++failures;
        // 2. Every effect together is the old Performance Mode bundle.
        const bool same_bundle = all_on.images == bundle.images && all_on.memory == bundle.memory;
        std::printf("  all effects off against GT4_CORE2_PERFORMANCE=1: pictures %s\n", same_bundle ? "identical" : "DIFFER");
        if (!same_bundle) ++failures;
    }

    // 3. The flare table: GT4_CORE2_PERF_FLARES=1 answers GT4's 64 x 8 visibility table as zeros, exactly as
    // GT4_GS_HIDE_FLARES=1 (the older name and the GSdx path's switch) does.
    auto flare_answer = [&](const char* name, const char* value, std::vector<std::uint32_t>& words) {
        if (name != nullptr) _putenv_s(name, value);
        _putenv_s("GT4_HW_CORE", "2");
        writeback::Renderer renderer;
        _putenv_s("GT4_HW_CORE", "");
        if (name != nullptr) _putenv_s(name, "");
        renderer.SetScale(2, 2.0f);
        std::string init_error;
        if (!renderer.Init(handles, init_error) || !renderer.Core2()) return false;
        auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
        auto xyz = [](uint32_t x, uint32_t y) { return uint64_t{x * 16} | (uint64_t{y * 16} << 16); };
        std::vector<Qword> stream;
        stream.push_back(PackedTag(14, true, 0, {0xE}));
        stream.push_back(Qword{uint64_t{0} | (uint64_t{1} << 16), 0x4C});
        stream.push_back(Qword{uint64_t{64} | (uint64_t{1} << 32), 0x4E});
        stream.push_back(Qword{(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40});
        stream.push_back(Qword{0, 0x18});
        stream.push_back(Qword{(uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47});
        stream.push_back(Qword{6, 0x00});
        stream.push_back(Qword{rgbaq(0x80336699u), 0x01});
        stream.push_back(Qword{xyz(0, 0), 0x05});
        stream.push_back(Qword{xyz(64, 8), 0x05});
        stream.push_back(Qword{uint64_t{0} | (uint64_t{1} << 16), 0x50});
        stream.push_back(Qword{0, 0x51});
        stream.push_back(Qword{uint64_t{64} | (uint64_t{8} << 32), 0x52});
        stream.push_back(Qword{1, 0x53});
        stream.push_back(Qword{0, 0x7F});
        if (!renderer.GifTransfer(1, reinterpret_cast<const uint8_t*>(stream.data()), stream.size() * sizeof(Qword))) {
            return false;
        }
        words.assign(64 * 8, 0xFFFFFFFFu);
        return renderer.ReadFifo(words.data(), words.size() * 4) == words.size() * 4;
    };
    std::vector<std::uint32_t> by_new, by_old, by_none;
    if (!flare_answer("GT4_CORE2_PERF_FLARES", "1", by_new) || !flare_answer("GT4_GS_HIDE_FLARES", "1", by_old) ||
        !flare_answer(nullptr, "", by_none)) {
        std::printf("  flare table: the streams could not be read\n");
        ++failures;
    } else {
        const auto zeros = [](const std::vector<std::uint32_t>& words) {
            return std::all_of(words.begin(), words.end(), [](std::uint32_t word) { return word == 0u; });
        };
        const auto drawn = [](const std::vector<std::uint32_t>& words) {
            return std::all_of(words.begin(), words.end(), [](std::uint32_t word) { return word == 0x80336699u; });
        };
        std::printf("  flare table: new switch %s, old switch %s, every effect on %s\n", zeros(by_new) ? "zeros" : "NOT zeros",
                    zeros(by_old) ? "zeros" : "NOT zeros", drawn(by_none) ? "the drawn words" : "NOT the drawn words");
        if (!zeros(by_new) || by_new != by_old || !drawn(by_none)) ++failures;
    }
    std::printf("hw_core2_effect_toggles: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_postfx_fxaa (IN-376, chapter 13.5): a dark buffer with one bright flat triangle whose long edges are steep
// and shallow, composed on the GPU at 1x and 2x with and without FXAA. Edge-only FXAA must leave every pixel away
// from the edges unchanged, change pixels along them, and only produce colours between the two flat colours.
int TestPostfxFxaa(const gpu::VulkanHandles& handles) {
    auto rgbaq = [](uint32_t colour) { return uint64_t{colour} | (uint64_t{0x3F800000u} << 32); };
    auto xy = [](uint32_t x16, uint32_t y16) { return uint64_t{x16} | (uint64_t{y16} << 16); };
    const uint64_t kSprite = 6, kTriangle = 3;
    cpuref::CaptureWriter capture;
    capture.Privileged(0x12000000u, 1u | (1u << 5) | (0xFFu << 8));                // PMODE: RC1, MMOD 1, ALP 255
    capture.Privileged(0x12000010u, uint64_t{2} << 13);                             // SMODE1: NTSC
    capture.Privileged(0x12000070u, uint64_t{0} | (uint64_t{1} << 9));             // DISPFB1: page 0, FBW 1
    capture.Privileged(0x12000080u, (uint64_t{3} << 23) | (uint64_t{255} << 32) | (uint64_t{63} << 44));  // 64 x 64
    std::vector<uint64_t> words = {(uint64_t{63} << 16) | (uint64_t{63} << 48), 0x40, 0, 0x18,
                                   (uint64_t{1} << 16) | (uint64_t{1} << 17), 0x47,  // TEST: ZTE, ALWAYS
                                   uint64_t{1} << 16, 0x4C, 8 | (uint64_t{1} << 32), 0x4E,
                                   kSprite, 0x00, rgbaq(0x80101010u), 0x01, xy(0, 0), 0x05, xy(64 * 16, 64 * 16), 0x05,
                                   kTriangle, 0x00, rgbaq(0x80F0F0F0u), 0x01, xy(4 * 16 + 3, 3 * 16), 0x05,
                                   xy(60 * 16, 21 * 16 + 5), 0x05, xy(17 * 16 + 9, 61 * 16), 0x05};
    capture.Gif(1, cpuref::CaptureWriter::Packet(static_cast<uint32_t>(words.size() / 2), true, false, 0, 0, {0xE}, words));
    capture.FrameEnd(0);

    auto compose = [&](uint32_t scale, bool fxaa, std::vector<uint32_t>& pixels, uint32_t& width, std::string& error) {
        writeback::Renderer renderer;
        renderer.SetScale(scale, static_cast<float>(scale));
        if (!renderer.Init(handles, error)) return false;
        renderer.SetSyncMode(true);
        renderer.SetFxaa(fxaa);
        writeback::WritebackReplayTarget target(renderer, writeback::ComposeKind::Reference);
        cpuref::ReplaySummary summary;
        bool got = false;
        const bool ok = cpuref::ReplayCapture(capture.Bytes(), target, summary,
                                              [&](uint64_t, const cpuref::FrameImage&) {
                                                  writeback::GpuFrame frame;
                                                  got = renderer.ComposeFrameGpu(frame) && frame.fxaa == fxaa &&
                                                        renderer.ReadGpuFrame(frame, pixels);
                                                  width = frame.width;
                                              },
                                              error);
        if (renderer.Failed()) error += " " + renderer.LastError();
        return ok && got && !renderer.Failed();
    };
    int failures = 0;
    for (uint32_t scale : {1u, 2u}) {
        std::vector<uint32_t> plain, smooth;
        uint32_t width = 0, width2 = 0;
        std::string error;
        if (!compose(scale, false, plain, width, error) || !compose(scale, true, smooth, width2, error) ||
            plain.size() != smooth.size() || width != width2 || width == 0) {
            std::printf("hw_postfx_fxaa: scale %u compose failed: %s\n", scale, error.c_str());
            ++failures;
            continue;
        }
        const uint32_t height = static_cast<uint32_t>(plain.size() / width);
        uint64_t changed = 0, changedAwayFromEdges = 0, outOfRange = 0;
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const uint32_t a = plain[y * width + x] & 0xFFFFFFu;
                const uint32_t b = smooth[y * width + x] & 0xFFFFFFu;
                if (a == b) continue;
                ++changed;
                // Edge pixels: some 3 x 3 neighbour differs in the plain image.
                bool edge = false;
                for (int dy = -1; dy <= 1 && !edge; ++dy) {
                    for (int dx = -1; dx <= 1 && !edge; ++dx) {
                        const int nx = std::clamp(static_cast<int>(x) + dx, 0, static_cast<int>(width) - 1);
                        const int ny = std::clamp(static_cast<int>(y) + dy, 0, static_cast<int>(height) - 1);
                        edge = (plain[static_cast<size_t>(ny) * width + nx] & 0xFFFFFFu) != a;
                    }
                }
                if (!edge) ++changedAwayFromEdges;
                for (int c = 0; c < 3; ++c) {
                    const uint32_t v = (b >> (8 * c)) & 0xFFu;
                    if (v < 0x10u || v > 0xF0u) ++outOfRange;
                }
            }
        }
        std::printf("  scale %u: %ux%u, %llu pixels smoothed, %llu changed away from edges, %llu channels outside "
                    "the two colours\n", scale, width, height, static_cast<unsigned long long>(changed),
                    static_cast<unsigned long long>(changedAwayFromEdges), static_cast<unsigned long long>(outOfRange));
        if (changed < 20 || changedAwayFromEdges != 0 || outOfRange != 0) ++failures;
    }
    std::printf("hw_postfx_fxaa: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_glm_unswizzle (IN-350): random local memory in the GLM is loaded into a colour or depth slot at a target
// layout; the slot must then hold LocalMemory::readRect of the same rectangle (what a load from the shadow memory
// would upload). Then a GLM download of a few page ranges must give back the GLM bytes at the same offsets.
int TestGlmUnswizzle(gpu::GpuDrawer& drawer) {
    struct Case {
        memory::Psm psm;
        bool depth;
        uint32_t bp;
        uint32_t bw;
    };
    const Case cases[] = {{memory::Psm::Ct32, false, 64, 10},  {memory::Psm::Ct24, false, 320, 4},
                          {memory::Psm::Ct16, false, 1000, 3}, {memory::Psm::Ct16S, false, 37, 3},
                          {memory::Psm::Z32, true, 640, 10},   {memory::Psm::Z24, true, 2048, 4},
                          {memory::Psm::Z16, true, 5000, 3},   {memory::Psm::Z16S, true, 7001, 3}};
    uint64_t state = 0x0B5Eu;
    auto next = [&] {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(state >> 32);
    };
    int failures = 0;
    memory::LocalMemory local;
    for (const Case& c : cases) {
        for (uint32_t address = 0; address < 4u * 1024u * 1024u; address += 4) {
            const uint32_t word = next();
            for (uint32_t b = 0; b < 4; ++b) local.setByte(address + b, static_cast<uint8_t>(word >> (8 * b)));
        }
        std::string error;
        if (!drawer.UploadGlm(local.bytes().data(), {{0u, 4u * 1024u * 1024u}}, error)) {
            std::printf("hw_glm_unswizzle: upload failed: %s\n", error.c_str());
            return 1;
        }
        const int32_t x0 = 3, y0 = 5, x1 = 140, y1 = 77;
        const uint32_t width = static_cast<uint32_t>(x1 - x0 + 1);
        const uint32_t height = static_cast<uint32_t>(y1 - y0 + 1);
        gpu::GpuChunk chunk;
        chunk.rectX0 = x0;
        chunk.rectY0 = y0;
        chunk.rectX1 = x1;
        chunk.rectY1 = y1;
        chunk.colourSlot = 2;
        chunk.depthSlot = 1;
        chunk.uploadFrame = chunk.uploadDepth = false;
        chunk.readbackFrame = !c.depth;
        chunk.readbackDepth = c.depth;
        (c.depth ? chunk.depthWords : chunk.frameWords).resize(static_cast<size_t>(width) * height);
        if (!drawer.LoadSlotFromGlm(c.depth, c.depth ? 1u : 2u, c.bp, c.bw, static_cast<uint32_t>(c.psm), x0, y0, x1, y1,
                                    error) ||
            !drawer.Execute(chunk, error)) {
            std::printf("hw_glm_unswizzle: GPU error: %s\n", error.c_str());
            return 1;
        }
        std::vector<uint32_t> expected(static_cast<size_t>(width) * height);
        local.readRect(memory::BufferView{c.bp, c.bw, c.psm}, static_cast<uint32_t>(x0), static_cast<uint32_t>(y0), width,
                       height, expected.data());
        const std::vector<uint32_t>& got = c.depth ? chunk.depthWords : chunk.frameWords;
        size_t differing = 0;
        for (size_t i = 0; i < expected.size(); ++i) differing += got[i] != expected[i] ? 1u : 0u;
        std::printf("  psm %02x (%s slot): %zu of %zu pixels differ from readRect\n", static_cast<unsigned>(c.psm),
                    c.depth ? "depth" : "colour", differing, expected.size());
        if (differing != 0) ++failures;
    }
    // Download round trip: three page ranges (one at the end of memory) into a zeroed buffer.
    std::vector<uint8_t> downloaded(4u * 1024u * 1024u, 0);
    const std::vector<std::pair<uint32_t, uint32_t>> ranges = {
        {0u, 8192u}, {37u * 8192u, 3u * 8192u}, {4u * 1024u * 1024u - 8192u, 8192u}};
    std::string error;
    if (!drawer.DownloadGlm(ranges, downloaded.data(), error)) {
        std::printf("hw_glm_unswizzle: download failed: %s\n", error.c_str());
        return 1;
    }
    size_t wrong = 0;
    for (size_t i = 0; i < downloaded.size(); ++i) {
        bool inside = false;
        for (const auto& range : ranges) inside = inside || (i >= range.first && i < range.first + range.second);
        const uint8_t expected = inside ? local.bytes()[i] : uint8_t{0};
        wrong += downloaded[i] != expected ? 1u : 0u;
    }
    std::printf("  download: %zu of %zu bytes wrong\n", wrong, downloaded.size());
    if (wrong != 0) ++failures;
    std::printf("hw_glm_unswizzle: %d failing cases\n", failures);
    return failures == 0 ? 0 : 1;
}

// hw_glm_upload_merge (IN-350): random (word, value, bit mask) triples merged into a random GLM must give
// glm = (glm & ~mask) | (value & mask) for exactly those words, every other word unchanged.
int TestGlmUploadMerge(gpu::GpuDrawer& drawer) {
    uint64_t state = 0x3E26u;
    auto next = [&] {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(state >> 32);
    };
    std::vector<uint8_t> memory(4u * 1024u * 1024u);
    for (uint8_t& byte : memory) byte = static_cast<uint8_t>(next());
    std::string error;
    if (!drawer.UploadGlm(memory.data(), {{0u, 4u * 1024u * 1024u}}, error)) {
        std::printf("hw_glm_upload_merge: upload failed: %s\n", error.c_str());
        return 1;
    }
    std::vector<uint32_t> expected(memory.size() / 4);
    std::memcpy(expected.data(), memory.data(), memory.size());
    std::vector<uint32_t> triples;
    std::vector<bool> used(expected.size(), false);
    for (int i = 0; i < 20000; ++i) {
        const uint32_t word = next() % static_cast<uint32_t>(expected.size());
        if (used[word]) continue;
        used[word] = true;
        const uint32_t masks[] = {0xFFFFFFFFu, 0x0000FFFFu, 0xFFFF0000u, 0x0000000Fu, 0xF0F00F0Fu, next()};
        const uint32_t mask = masks[next() % 6u];
        const uint32_t value = next();
        triples.insert(triples.end(), {word, value, mask});
        expected[word] = (expected[word] & ~mask) | (value & mask);
    }
    std::vector<uint32_t> glm;
    if (!drawer.MergeIntoGlm(triples, error) || !drawer.ReadGlm(glm, error)) {
        std::printf("hw_glm_upload_merge: GPU error: %s\n", error.c_str());
        return 1;
    }
    size_t wrong = 0;
    for (size_t i = 0; i < expected.size(); ++i) wrong += glm[i] != expected[i] ? 1u : 0u;
    std::printf("  %zu merged words, %zu of %zu GLM words wrong\n", triples.size() / 3, wrong, expected.size());
    std::printf("hw_glm_upload_merge: %s\n", wrong == 0 ? "pass" : "FAIL");
    return wrong == 0 ? 0 : 1;
}

// hw_glm_host_upload (IN-351): EE->GS uploads written into the GLM by the upload kernel must equal
// memory::hostToLocal on the same bytes, for every transfer size (32, 24, 16, 8, 4 bits, the high-byte and
// high/low-nibble 32-bit formats, depth formats), odd origins and widths, and a partial transfer that ends inside
// the rectangle. The whole 4 MiB is compared after each upload.
int TestGlmHostUpload(gpu::GpuDrawer& drawer) {
    struct Case {
        uint32_t psm;
        uint32_t bp;
        uint32_t bw;
        uint32_t x;
        uint32_t y;
        uint32_t width;
        uint32_t height;
        bool partial;
    };
    const Case cases[] = {{0x00, 64, 10, 3, 5, 61, 17, false},   {0x01, 320, 4, 1, 2, 33, 9, false},
                          {0x02, 1000, 3, 7, 1, 25, 40, false},  {0x0A, 37, 3, 2, 3, 30, 31, true},
                          {0x13, 2048, 4, 5, 9, 77, 21, false},  {0x14, 5000, 2, 3, 7, 101, 13, false},
                          {0x1B, 7001, 2, 9, 4, 50, 19, false},  {0x24, 7200, 2, 1, 1, 63, 11, false},
                          {0x2C, 7400, 2, 4, 6, 64, 8, true},    {0x30, 9000, 10, 6, 3, 300, 12, false},
                          {0x31, 9400, 4, 2, 2, 129, 7, false},  {0x3A, 9800, 2, 3, 1, 90, 30, false}};
    uint64_t state = 0x7A11u;
    auto next = [&] {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(state >> 32);
    };
    memory::LocalMemory local;
    for (uint32_t address = 0; address < 4u * 1024u * 1024u; address += 4) {
        const uint32_t word = next();
        for (uint32_t b = 0; b < 4; ++b) local.setByte(address + b, static_cast<uint8_t>(word >> (8 * b)));
    }
    std::string error;
    if (!drawer.UploadGlm(local.bytes().data(), {{0u, 4u * 1024u * 1024u}}, error)) {
        std::printf("hw_glm_host_upload: upload failed: %s\n", error.c_str());
        return 1;
    }
    int failures = 0;
    for (const Case& c : cases) {
        const auto psm = memory::decodePsm(c.psm);
        if (!psm) {
            std::printf("  psm %02x: not a PSM\n", c.psm);
            ++failures;
            continue;
        }
        const uint32_t bpp = memory::psmInfo(*psm).transferBpp;
        size_t bytes = (static_cast<size_t>(c.width) * c.height * bpp + 7) / 8;
        if (c.partial) bytes = bytes / 2 + 3;  // ends inside the rectangle, not on a texel boundary
        std::vector<uint8_t> data(bytes);
        for (uint8_t& byte : data) byte = static_cast<uint8_t>(next());
        const uint32_t pixels = static_cast<uint32_t>(
            std::min<uint64_t>(static_cast<uint64_t>(bytes) * 8u / bpp, static_cast<uint64_t>(c.width) * c.height));
        if (!drawer.UploadHostToGlm(c.bp, c.bw, c.psm, c.x, c.y, c.width, pixels, bpp, data.data(), data.size(), error)) {
            std::printf("hw_glm_host_upload: GPU error: %s\n", error.c_str());
            return 1;
        }
        memory::hostToLocal(local, memory::BufferView{c.bp, c.bw, *psm}, memory::TransferRect{c.x, c.y, c.width, c.height},
                            std::span<const uint8_t>(data.data(), data.size()));
        std::vector<uint32_t> glm;
        if (!drawer.ReadGlm(glm, error)) {
            std::printf("hw_glm_host_upload: read failed: %s\n", error.c_str());
            return 1;
        }
        size_t differing = 0;
        for (size_t i = 0; i < glm.size(); ++i) {
            uint32_t expected = 0;
            std::memcpy(&expected, local.bytes().data() + 4 * i, 4);
            differing += glm[i] != expected ? 1u : 0u;
        }
        std::printf("  psm %02x %ux%u at (%u,%u)%s: %zu GLM words differ from hostToLocal\n", c.psm, c.width, c.height,
                    c.x, c.y, c.partial ? " partial" : "", differing);
        if (differing != 0) ++failures;
    }
    std::printf("hw_glm_host_upload: %d failing cases\n", failures);
    return failures == 0 ? 0 : 1;
}

// IN-347 GLM kernels against the CPU memory model (chapter 04.9 fixture hw_glm_equals_writeback, split in two).
// hw_glm_decode: random local memory uploaded to the GLM; for every PSM, several bases and widths, the GPU
// decode of a 96 x 80 extent equals LocalMemory::readPixel (indexed formats: through a random palette).
int TestGlmDecode(gpu::GpuDrawer& drawer) {
    memory::LocalMemory local;
    uint64_t state = 0x61A5u;
    auto next = [&] {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(state >> 32);
    };
    for (uint32_t address = 0; address < 4u * 1024u * 1024u; address += 4) {
        const uint32_t word = next();
        for (uint32_t b = 0; b < 4; ++b) local.setByte(address + b, static_cast<uint8_t>(word >> (8 * b)));
    }
    std::string error;
    if (!drawer.UploadGlm(local.bytes().data(), {{0u, 4u * 1024u * 1024u}}, error)) {
        std::printf("hw_glm_decode: upload failed: %s\n", error.c_str());
        return 1;
    }
    std::vector<uint32_t> palette(256);
    for (uint32_t& entry : palette) entry = next();
    const memory::Psm psms[] = {memory::Psm::Ct32, memory::Psm::Ct24, memory::Psm::Ct16, memory::Psm::Ct16S,
                                memory::Psm::T8,   memory::Psm::T4,   memory::Psm::T8H,  memory::Psm::T4Hl,
                                memory::Psm::T4Hh, memory::Psm::Z32,  memory::Psm::Z24,  memory::Psm::Z16,
                                memory::Psm::Z16S};
    const uint32_t bases[] = {0u, 37u, 9000u, 16370u};
    const uint32_t widths[] = {1u, 2u, 10u};
    constexpr uint32_t kW = 96, kH = 80;
    uint64_t compared = 0, mismatches = 0;
    for (memory::Psm psm : psms) {
        const bool indexed = memory::psmInfo(psm).isIndexed;
        for (uint32_t bp : bases) {
            for (uint32_t bw : widths) {
                gpu::GpuChunk chunk;
                chunk.rectX0 = chunk.rectY0 = 0;
                chunk.rectX1 = chunk.rectY1 = 0;
                chunk.uploadFrame = chunk.uploadDepth = chunk.readbackFrame = chunk.readbackDepth = false;
                chunk.hasTexture = true;
                chunk.glmTexture = true;
                chunk.glmBp = bp;
                chunk.glmBw = bw;
                chunk.glmPsm = static_cast<uint32_t>(psm);
                chunk.glmIndexed = indexed;
                chunk.palette = palette;
                chunk.texWidth = kW;
                chunk.texHeight = kH;
                std::vector<uint32_t> texels;
                if (!drawer.Execute(chunk, error) || !drawer.ReadTexels(kW, kH, texels, error)) {
                    std::printf("hw_glm_decode: GPU error: %s\n", error.c_str());
                    return 1;
                }
                const memory::BufferView view{bp, bw, psm};
                for (uint32_t y = 0; y < kH; ++y) {
                    for (uint32_t x = 0; x < kW; ++x) {
                        uint32_t expected = local.readPixel(view, x, y);
                        if (indexed) expected = palette[expected & 255u];
                        const uint32_t got = texels[static_cast<size_t>(y) * kW + x];
                        ++compared;
                        if (got != expected) {
                            if (mismatches < 4) {
                                std::printf("  mismatch psm=%02x bp=%u bw=%u at %u,%u: gpu %08X cpu %08X\n",
                                            static_cast<unsigned>(psm), bp, bw, x, y, got, expected);
                            }
                            ++mismatches;
                        }
                    }
                }
            }
        }
    }
    std::printf("hw_glm_decode: compared %llu texels over 13 PSMs, mismatches %llu\n",
                static_cast<unsigned long long>(compared), static_cast<unsigned long long>(mismatches));
    return mismatches == 0 ? 0 : 1;
}

// hw_glm_swizzle: random target words in a colour or depth slot are swizzled into the GLM at the target's own
// (BP, BW, PSM); the whole 4 MiB GLM must then equal the local memory after LocalMemory::writeRect of the same
// words (only the pixel bits change; neighbouring sub-word pixels survive).
// The rectangle stays inside the buffer width: wider rectangles wrap into the next page row, two pixels then
// share an address, and the last writer depends on the visiting order (a parallel swizzle has none).
int TestGlmSwizzle(gpu::GpuDrawer& drawer) {
    struct Case {
        memory::Psm psm;
        bool depth;
        uint32_t bp;
        uint32_t bw;
    };
    const Case cases[] = {{memory::Psm::Ct32, false, 64, 10},  {memory::Psm::Ct24, false, 320, 4},
                          {memory::Psm::Ct16, false, 1000, 3}, {memory::Psm::Ct16S, false, 37, 3},
                          {memory::Psm::Z32, true, 640, 10},   {memory::Psm::Z24, true, 2048, 4},
                          {memory::Psm::Z16, true, 5000, 3},   {memory::Psm::Z16S, true, 7001, 3}};
    uint64_t state = 0x5A11u;
    auto next = [&] {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(state >> 32);
    };
    int failures = 0;
    for (const Case& c : cases) {
        memory::LocalMemory local;
        for (uint32_t address = 0; address < 4u * 1024u * 1024u; address += 4) {
            const uint32_t word = next();
            for (uint32_t b = 0; b < 4; ++b) local.setByte(address + b, static_cast<uint8_t>(word >> (8 * b)));
        }
        std::string error;
        if (!drawer.UploadGlm(local.bytes().data(), {{0u, 4u * 1024u * 1024u}}, error)) {
            std::printf("hw_glm_swizzle: upload failed: %s\n", error.c_str());
            return 1;
        }
        const int32_t x0 = 3, y0 = 5, x1 = 140, y1 = 77;
        const size_t words = static_cast<size_t>(x1 - x0 + 1) * static_cast<size_t>(y1 - y0 + 1);
        std::vector<uint32_t> values(words);
        for (uint32_t& v : values) v = next();
        gpu::GpuChunk chunk;
        chunk.rectX0 = x0;
        chunk.rectY0 = y0;
        chunk.rectX1 = x1;
        chunk.rectY1 = y1;
        chunk.colourSlot = 1;
        chunk.depthSlot = 1;
        chunk.uploadFrame = !c.depth;
        chunk.uploadDepth = c.depth;
        chunk.readbackFrame = chunk.readbackDepth = false;
        (c.depth ? chunk.depthWords : chunk.frameWords) = values;
        std::vector<uint32_t> glm;
        if (!drawer.Execute(chunk, error) ||
            !drawer.SwizzleToGlm(c.depth, 1, c.bp, c.bw, static_cast<uint32_t>(c.psm), x0, y0, x1, y1, error) ||
            !drawer.ReadGlm(glm, error)) {
            std::printf("hw_glm_swizzle: GPU error: %s\n", error.c_str());
            return 1;
        }
        local.writeRect(memory::BufferView{c.bp, c.bw, c.psm}, static_cast<uint32_t>(x0), static_cast<uint32_t>(y0),
                        static_cast<uint32_t>(x1 - x0 + 1), static_cast<uint32_t>(y1 - y0 + 1), values.data());
        size_t differing = 0;
        for (size_t i = 0; i < glm.size(); ++i) {
            uint32_t expected = 0;
            std::memcpy(&expected, local.bytes().data() + 4 * i, 4);
            if (glm[i] != expected) ++differing;
        }
        std::printf("  psm %02x (%s slot): %zu of %zu GLM words differ from the write-back\n",
                    static_cast<unsigned>(c.psm), c.depth ? "depth" : "colour", differing, glm.size());
        if (differing != 0) ++failures;
    }
    std::printf("hw_glm_swizzle: %d failing cases\n", failures);
    return failures == 0 ? 0 : 1;
}

// IN-349 GPU CLUT (chapter 07.7) against the CPU CLUT unit: a sequence of 40 random CLUT loads (CT32, CT16,
// CT16S; CSM1 and CSM2; T8 and T4 palettes; CSA 0..31; aligned and unaligned CBP) is applied to the CPU
// clut::ClutUnit from local memory and to the GPU CLUT buffer from the GLM; after every load the 512 halfwords
// must be equal. Then a T8 and a T4 texture decoded on the GPU through the GPU CLUT (with TEXA for CT16) must
// equal the CPU decode through ClutUnit::Decode.
int TestGlmClut(gpu::GpuDrawer& drawer) {
    memory::LocalMemory local;
    uint64_t state = 0xC107u;
    auto next = [&] {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<uint32_t>(state >> 32);
    };
    for (uint32_t address = 0; address < 4u * 1024u * 1024u; address += 4) {
        const uint32_t word = next();
        for (uint32_t b = 0; b < 4; ++b) local.setByte(address + b, static_cast<uint8_t>(word >> (8 * b)));
    }
    std::string error;
    if (!drawer.UploadGlm(local.bytes().data(), {{0u, 4u * 1024u * 1024u}}, error)) {
        std::printf("hw_glm_clut: upload failed: %s\n", error.c_str());
        return 1;
    }
    CounterSet counterSet;
    clut::ClutUnit cpu(counterSet);
    if (!drawer.UploadClut(cpu.Halfwords(), error)) {
        std::printf("hw_glm_clut: CLUT upload failed: %s\n", error.c_str());
        return 1;
    }
    const uint32_t cpsms[] = {0x00u, 0x02u, 0x0Au};
    int failures = 0;
    clut::Tex0Write last;
    for (int load = 0; load < 40; ++load) {
        clut::Tex0Write write;
        write.psm = (next() & 1u) ? 0x13u : 0x14u;  // T8 or T4
        write.cpsm = cpsms[next() % 3u];
        write.cbp = (load % 3 == 0) ? (next() % 16000u) : (next() % 500u) * 32u;
        write.csm = (load % 5 == 4) ? 1u : 0u;
        write.csa = next() & 31u;
        write.cld = 1;
        write.texclutCbw = 1u + next() % 4u;
        write.texclutCou = next() % 4u;
        write.texclutCov = next() % 300u;
        cpu.ProcessWrite(write, local.bytes().data(), {});
        if (!drawer.LoadClutFromGlm(write.cbp, write.cpsm, write.csm, write.csa, write.psm, write.texclutCbw,
                                    write.texclutCou, write.texclutCov, error)) {
            std::printf("hw_glm_clut: GPU load failed: %s\n", error.c_str());
            return 1;
        }
        std::vector<uint16_t> gpu;
        if (!drawer.ReadClut(gpu, error)) {
            std::printf("hw_glm_clut: read failed: %s\n", error.c_str());
            return 1;
        }
        uint32_t differing = 0;
        for (uint32_t i = 0; i < 512; ++i) differing += gpu[i] != cpu.Halfwords()[i] ? 1u : 0u;
        if (differing != 0) {
            std::printf("  load %d (cpsm %02x csm %u csa %u psm %02x cbp %u): %u halfwords differ\n", load, write.cpsm,
                        write.csm, write.csa, write.psm, write.cbp, differing);
            ++failures;
        }
        last = write;
    }
    // Indexed decode through the GPU CLUT equals the CPU palette for both index sizes and both palette formats.
    const uint64_t texa = uint64_t{0x55} | (uint64_t{1} << 15) | (uint64_t{0xAA} << 32);  // TA0, AEM, TA1
    const struct {
        uint32_t psm;
        uint32_t cpsm;
        uint32_t csa;
    } decodes[] = {{0x13u, 0x00u, 3u}, {0x14u, 0x00u, 9u}, {0x13u, 0x02u, 7u}, {0x14u, 0x0Au, 21u}};
    for (const auto& d : decodes) {
        clut::Tex0Write write;
        write.psm = d.psm;
        write.cpsm = d.cpsm;
        write.cbp = 4096u;
        write.csa = d.csa;
        write.cld = 1;
        cpu.ProcessWrite(write, local.bytes().data(), {});
        if (!drawer.LoadClutFromGlm(write.cbp, write.cpsm, 0, write.csa, write.psm, 0, 0, 0, error)) return 1;
        const clut::Palette palette = cpu.Decode(d.psm, d.cpsm, d.csa, texa);
        const clut::TexaState texaState = clut::TexaState::FromRegister(texa);
        gpu::GpuChunk chunk;
        chunk.rectX0 = chunk.rectY0 = chunk.rectX1 = chunk.rectY1 = 0;
        chunk.uploadFrame = chunk.uploadDepth = chunk.readbackFrame = chunk.readbackDepth = false;
        chunk.hasTexture = true;
        chunk.glmTexture = true;
        chunk.glmBp = 8000u;
        chunk.glmBw = 2u;
        chunk.glmPsm = d.psm;
        chunk.glmIndexed = true;
        chunk.glmGpuClut = true;
        chunk.glmCpsm = d.cpsm;
        chunk.glmCsa = d.csa;
        chunk.glmTa0 = texaState.ta0;
        chunk.glmTa1 = texaState.ta1;
        chunk.glmAem = texaState.aem ? 1u : 0u;
        chunk.texWidth = 64;
        chunk.texHeight = 32;
        std::vector<uint32_t> texels;
        if (!drawer.Execute(chunk, error) || !drawer.ReadTexels(64, 32, texels, error)) {
            std::printf("hw_glm_clut: decode failed: %s\n", error.c_str());
            return 1;
        }
        uint32_t wrong = 0;
        const memory::BufferView view{8000u, 2u, *memory::decodePsm(d.psm)};
        for (uint32_t y = 0; y < 32; ++y) {
            for (uint32_t x = 0; x < 64; ++x) {
                const uint32_t expected = palette.entries[local.readPixel(view, x, y) & 255u];
                wrong += texels[static_cast<size_t>(y) * 64 + x] != expected ? 1u : 0u;
            }
        }
        std::printf("  decode psm %02x cpsm %02x csa %u: %u of 2048 texels differ\n", d.psm, d.cpsm, d.csa, wrong);
        if (wrong != 0) ++failures;
    }
    (void)last;
    std::printf("hw_glm_clut: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

int TestGouraudReport(gpu::GpuDrawer& drawer) {
    // Report only (IN-500): Gouraud interpolation rounding is not asserted yet.
    Setup s;
    s.iip = true;
    std::vector<Vertex> tri = {MakeVertex(0, 0, 255, 0, 0, 255), MakeVertex(40, 0, 0, 255, 0, 255),
                               MakeVertex(0, 40, 0, 0, 255, 255)};
    Batch batch = MakeBatch(s, tri, {PrimType::Triangle});
    CaseResult r = RunCase(drawer, s, batch, 16);
    Report("hw_gpu_gouraud_report", r);
    return r.gpuError ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::string name = argc > 1 ? argv[1] : "";
    if (name == "hw_flat_provoking") return TestFlatProvoking();
    if (name == "hw_sprite_corners") return TestSpriteCorners();
    if (name == "hw_present_scheduler") return TestPresentScheduler();
    if (name == "hw_present_scheduler_hold") return TestPresentSchedulerHold();
    if (name == "hw_present_scheduler_rate") return TestPresentSchedulerRate();
    if (name == "hw_aux_frame_guest_untouched") return TestAuxFrameGuestUntouched();
    if (name == "hw_pass_timings") return TestPassTimings();
    if (name == "hw_presentation_field_phase") return TestPresentationFieldPhase();

    Headless headless;
    std::string error;
    if (const char* off = std::getenv("GT4_HW_TEST_NO_INTERLOCK"); off && std::strcmp(off, "1") == 0) {
        headless.wantInterlock = false;  // IN-345: run the per-primitive path
    }
    if (!headless.Create(error)) {
        std::printf("%s: skipped (%s)\n", name.c_str(), error.c_str());
        return 77;
    }
    gpu::VulkanHandles handles;
    handles.instance = headless.instance;
    handles.physical = headless.physical;
    handles.device = headless.device;
    handles.queue = headless.queue;
    handles.queueFamily = headless.family;
    handles.pixelInterlock = headless.interlock;
    handles.independentBlend = headless.independentBlend;
    handles.dualSourceBlend = headless.dualSourceBlend;
    handles.profile = headless.profile;  // TASK-179
    gpu::GpuDrawer drawer;
    if (!drawer.Init(handles, error)) {
        std::printf("%s: GPU drawer init failed: %s\n", name.c_str(), error.c_str());
        return 1;
    }
    if (name == "hw_gpu_coverage_vectors") return TestCoverageVectors(drawer);
    if (name == "hw_gpu_equivalence_flat") return TestEquivalenceFlat(drawer);
    if (name == "hw_gpu_equivalence_texture") return TestEquivalenceTexture(drawer);
    if (name == "hw_gpu_equivalence_region") return TestEquivalenceWrapRegion(drawer);
    if (name == "hw_gpu_equivalence_bilinear") return TestEquivalenceBilinear(drawer);
    if (name == "hw_gpu_equivalence_tfx_fog") return TestEquivalenceTfxFog(drawer);
    if (name == "hw_gpu_equivalence_depth_alpha") return TestEquivalenceDepthAlpha(drawer);
    if (name == "hw_gpu_equivalence_scissor") return TestEquivalenceScissor(drawer);
    if (name == "hw_gpu_raster_blend") return TestRasterBlendModes(drawer);
    if (name == "hw_gpu_raster_pabe") return TestRasterPabe(drawer);
    if (name == "hw_gpu_raster_atest") return TestRasterAlphaTest(drawer);
    if (name == "hw_gpu_raster_date") return TestRasterDate(drawer);
    if (name == "hw_gpu_raster_frame") return TestRasterFrameWrites(drawer);
    if (name == "hw_gpu_gouraud_report") return TestGouraudReport(drawer);
    if (name == "hw_glm_decode") return TestGlmDecode(drawer);
    if (name == "hw_glm_swizzle") return TestGlmSwizzle(drawer);
    if (name == "hw_glm_unswizzle") return TestGlmUnswizzle(drawer);
    if (name == "hw_glm_upload_merge") return TestGlmUploadMerge(drawer);
    if (name == "hw_glm_host_upload") return TestGlmHostUpload(drawer);
    if (name == "hw_glm_clut") return TestGlmClut(drawer);
    if (name == "hw_gpu_writeback_aliased_depth") return TestWritebackAliasedDepth(drawer);
    if (name == "hw_readback_pending_draw") return TestReadbackObservesPendingDraw(handles);
    if (name == "hw_replay_parity_double_buffer") return TestReplayParityDoubleBuffer(handles);
    if (name == "hw_cache_equals_writeback") return TestCacheEqualsWriteback(handles);
    if (name == "hw_scale_exact") return TestScaleExact(handles);
    if (name == "hw_scale_target_extent") return TestScaleTargetExtent(handles);
    if (name == "hw_postfx_fxaa") return TestPostfxFxaa(handles);
    if (name == "hw_scale_display_shadow") return TestScaleDisplayShadow(handles);
    if (name == "hw_scale_fullscreen_passes") return TestScaleFullscreenPasses(handles);
    if (name == "hw_tone_hle_exact") return TestToneHleExact(handles);
    if (name == "hw_scale_interlaced_present") return TestScaleInterlacedPresent(handles);
    if (name == "hw_core2_interlaced_present") return TestCore2InterlacedPresent(handles);
    if (name == "hw_core2_post_field_compose") return TestCore2PostFieldCompose(handles);
    if (name == "hw_core2_cpu_sprites") return TestCore2CpuSprites(handles);
    if (name == "hw_core2_gpu_clut") return TestCore2GpuClut(handles);
    if (name == "hw_core2_palette_segments") return TestCore2PaletteSegments(handles);
    if (name == "hw_core2_tone_early_drop") return TestCore2ToneEarlyDrop(handles);
    if (name == "hw_core2_drawn_page_test") return TestCore2DrawnPageTest(handles);
    if (name == "hw_core2_width_grace") return TestCore2WidthGrace(handles);
    if (name == "hw_core2_glm_memo") return TestCore2GlmMemo(handles);
    if (name == "hw_core2_patch_bits") return TestCore2PatchBits(handles);
    if (name == "hw_core2_patch_blocks") return TestCore2PatchBlocks(handles);
    if (name == "hw_core2_over_target_memo") return TestCore2OverTargetMemo(handles);
    if (name == "hw_core2_mipmaps") return TestCore2Mipmaps(handles);
    if (name == "hw_core2_specialised") return TestCore2Specialised(handles);
    if (name == "hw_core2_upload_memo") return TestCore2UploadMemo(handles);
    if (name == "hw_core2_depth_clear") return TestCore2DepthClear(handles);
    if (name == "hw_core2_scanmsk") return TestCore2Scanmsk(handles);
    if (name == "hw_core2_display_memory") return TestCore2DisplayMemory(handles);
    if (name == "hw_core2_whole_clears") return TestCore2WholeClears(handles);
    if (name == "hw_core2_rop") return TestCore2Rop(handles);
    if (name == "hw_core2_road_sheen") return TestCore2RoadSheen(handles);
    if (name == "hw_core2_shader_cache") return TestCore2ShaderCache(handles);
    if (name == "hw_core2_prepare_shaders") return TestCore2PrepareShaders(handles);
    if (name == "hw_core2_shader_guard") return TestCore2ShaderGuard(handles);
    if (name == "hw_core2_delayed_downloads") return TestCore2DelayedDownloads(handles);
    if (name == "hw_core2_compute_fills") return TestCore2ComputeFills(handles);
    if (name == "hw_core2_self_texture") return TestCore2SelfTexture(handles);
    if (name == "hw_core2_self_copy_draw") return TestCore2SelfCopyDraw(handles);
    if (name == "hw_core2_texture_replacement") return TestCore2TextureReplacement(handles);
    if (name == "hw_core2_overlap_order") return TestCore2OverlapOrder(handles);
    if (name == "hw_core2_point_quads") return TestCore2PointQuads(handles);
    if (name == "hw_core2_sync_trace") return TestCore2SyncTrace(handles);
    if (name == "hw_core2_data_paths") return TestCore2DataPaths(handles);
    if (name == "hw_core2_effect_toggles") return TestCore2EffectToggles(handles);
    if (name == "hw_core2_present_scale") return TestCore2PresentScale(handles);
    if (name == "hw_core2_forced_bilinear") return TestCore2ForcedBilinear(handles);
    std::printf("unknown test: %s\n", name.c_str());
    return 2;
}

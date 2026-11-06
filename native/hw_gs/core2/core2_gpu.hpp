// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from docs/hw-renderer-spec/CORE2-SPEC.md and CORE2-DESIGN.md (TASK-027 core rewrite), clean room
// (decision 0070).
//
// The GPU side of CORE2. Every target is one r32ui storage image of GS words at the internal scale (colour and depth
// alike); nothing is kept at 1x. Draws use the existing vertex and uber pixel shaders (blending, depth, alpha test,
// DATE, dither and fog in the shader on storage images). Compute passes patch native rectangles from GS memory into
// a target (4.1), reduce a target to 1x words for a readback (4.2), run GT4's channel-shuffle hook (7.2) and merge the
// display (5.3). Work is recorded into one command buffer and submitted at the end of a frame, or earlier when the CPU
// needs a result. The public header stays Vulkan-free.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../writeback/hw_gs_writeback_gpu.hpp"

namespace hwgs::core2 {

// A GPU image handle (index into the image table). kNoImage: none.
using ImageId = std::int32_t;
constexpr ImageId kNoImage = -1;

// One batch of draws into a colour target (and a depth target), as CORE2's renderer hands it over.
struct DrawBatch {
    ImageId frame = kNoImage;     // colour target (required)
    ImageId depth = kNoImage;     // depth target, or kNoImage when the draw neither tests nor writes depth
    ImageId texture = kNoImage;   // CORE2-SPEC 3.2: a target sampled at scale (tex_scale = s), else kNoImage
    // A texture decoded from GS memory (3.7): a 1x image of its own, texel (0, 0) at the image origin.
    ImageId memoryTexture = kNoImage;
    gpu::GpuPixelState state;
    const std::vector<gpu::GpuVertex> *vertices = nullptr;
    const std::vector<gpu::GpuDraw> *draws = nullptr;
    std::uint32_t zMax = 0xFFFFFFFFu;
    // IN-409: memoryTexture holds CLUT indices; their colours are row `paletteRow` of image `palettes` (-1: none).
    ImageId palettes = kNoImage;
    std::int32_t paletteRow = -1;
    // TASK-168: every row of `palettes` the vertices name (GpuVertex::paletteRow - 1) when the batch has several palette
    // segments, so the draw is ordered after their writes and before any rewrite of them; nullptr: `paletteRow` alone.
    const std::vector<std::int32_t> *segmentPaletteRows = nullptr;
    // IN-428: the mip parameters of memoryTexture (0: level 0 only), packed as hw_pixel_uber.frag g_mip describes.
    std::uint32_t mipInfo = 0;
    bool forceLinear = false;  // IN-433: bilinear where the game asks for nearest (push word bit 31)
    std::uint32_t scanmsk = 0;  // IN-530: SCANMSK, 2 leaves out even native rows, 3 odd ones (push word bits 20-21)
    // IN-442: an untextured sprite batch with no blend, fog or DATE: drawn by core2_fill.comp when its sprites turn out
    // to be one constant fragment over disjoint pixel rectangles (Gpu::Draw checks the geometry).
    bool constantFill = false;
    // Spec 8.8: `texture` is the colour or depth target this batch draws into and the sampled texels meet the drawn
    // pixels. The batch then samples a copy of native rectangle [copyX0, copyX1] x [copyY0, copyY1] (inclusive) of the
    // texture taken before the batch: the GPU runs the batch's fragments in parallel, so sampling the target itself
    // would read pixels other fragments of the batch may or may not have written yet.
    bool selfCopy = false;
    std::int32_t copyX0 = 0, copyY0 = 0, copyX1 = -1, copyY1 = -1;
    // IN-470 (chapter 11): memoryTexture is a replacement image of this size (0: none), RGBA8 words. It covers the
    // native rectangle at (logicalX, logicalY) of size logicalWidth x logicalHeight: 2^TW x 2^TH or the region.
    std::uint32_t replacedWidth = 0, replacedHeight = 0;
    std::uint32_t logicalX = 0, logicalY = 0, logicalWidth = 0, logicalHeight = 0;
};

// Diagnostics for the F8 report and the replay summary.
struct Core2GpuStats {
    std::uint64_t batches = 0;
    std::uint64_t wholeClears = 0;      // IN-494: fills that became a clear of the whole image
    std::uint64_t plainBatches = 0;     // IN-496: batches of an interlock GPU drawn without the interlock
    std::uint64_t roadSheensSkipped = 0;  // IN-501
    std::uint64_t sheensEarly = 0;        // IN-503
    std::uint64_t imagesCreated = 0;    // S1: new images (not taken over from finished ones)
    std::uint64_t imagesAllocated = 0;  // S1: images that needed a device allocation of their own
    std::uint64_t imageBlocks = 0;      // S2: 64 MiB image memory blocks allocated
    std::uint64_t recycledImages = 0;  // B4: images taken over from finished ones instead of allocated
    std::uint64_t selfTextureBatches = 0;  // batches whose texture is the colour or depth image they draw into
    std::uint64_t selfCopies = 0;          // spec 8.8: of those, batches that sampled a copy taken before them
    std::uint64_t selfCopyDraws = 0;       // TASK-182 R1: of those copies, ones drawn inside the open pass (GT4_CORE2_SELF_COPY_DRAW)
    std::uint64_t selfCopyPixels = 0;      // output pixels those copies moved
    std::uint64_t draws = 0;
    std::uint64_t patches = 0;
    std::uint64_t reductions = 0;
    std::uint64_t laterReductions = 0;  // IN-439: reductions taken later (performance mode's delayed downloads)
    std::uint64_t computeFills = 0;     // IN-442: batches drawn as compute fills
    std::uint64_t fillPixels = 0;       // IN-442: output pixels they covered
    std::uint64_t tones = 0;
    std::uint64_t composes = 0;
    std::uint64_t deinterlaces = 0;
    // TASK-182 R8 (GT4_CORE2_DEINT_FIELD_COMPOSE=1): display merges written straight into the field's deinterlace history
    // image, and full-display image copies the deinterlace recorded (two per deinterlaced VBlank without the switch, none
    // with it).
    std::uint64_t fieldComposes = 0;
    std::uint64_t postCopies = 0;
    std::uint64_t fxaaFrames = 0;
    std::uint64_t presentScales = 0;  // IN-432: frames resampled to the window rectangle
    // IN-437: pipelines specialised per draw state (GT4_CORE2_SPECIALIZE).
    std::uint64_t specialisedBatches = 0;  // triangle batches drawn with their specialised pipeline
    std::uint64_t uberBatches = 0;         // triangle batches drawn with the uber pipeline while theirs compiled
    std::uint64_t specPipelines = 0;       // specialised pipelines compiled
    std::uint64_t specRequested = 0;       // draw states seen (compiled, compiling or queued)
    std::uint64_t specCompileNs = 0;       // time spent compiling them
    std::uint64_t specCompileMaxNs = 0;
    std::uint64_t specFailures = 0;
    std::uint64_t submissions = 0;
    std::uint64_t waits = 0;
    std::uint64_t atlasUploads = 0;
    std::uint64_t atlasResets = 0;
    std::uint64_t imageBytes = 0;
    std::uint64_t clutTargetLoads = 0;  // IN-408: CLUT loads read from a target on the GPU
    std::uint64_t clutWrites = 0;       // IN-408: CPU CLUT loads mirrored into the GPU CLUT
    std::uint64_t clutReads = 0;        // IN-408: GPU CLUT read back for a CPU user (a wait)
    std::uint64_t materializations = 0; // IN-408: target rectangles written into the GLM on the GPU
    std::uint64_t paletteRows = 0;      // IN-409: palette rows expanded from the GPU CLUT
    std::uint64_t waitNs = 0;           // B0: time the CPU spent waiting for GPU fences
    std::uint64_t gpuNs = 0;            // B4: GPU time of finished submissions (timestamps, first to last command)
    std::uint64_t timedSubmissions = 0;
    std::uint64_t drawPassNs = 0;
    // TASK-182 (GT4_CORE2_GPU_CATEGORIES=1): GPU time by kind of work, nanoseconds: draw passes, compose, deinterlace,
    // FXAA, present, uploads and copies, compute (fills, decodes, patches, tone), readback copies, and the start gap (the
    // recording's start to its first mark). Kept in the order of Gpu::Impl's kCat* kinds.
    std::uint64_t catNs[9] = {};
    std::uint64_t primitives = 0;          // B4: pipeline statistics of finished submissions (input primitives,
    std::uint64_t fragments = 0;           // fragment shader invocations, compute shader invocations)
    std::uint64_t computeInvocations = 0;
    std::uint64_t windowDecodes = 0;       // IN-422: texture decodes read from a CPU page window, not the GLM
    std::uint64_t windowBytes = 0;       // B4: GPU time inside the deferred draw passes (IN-420), part of gpuNs // B4: submissions whose GPU time is in gpuNs
    std::uint64_t fullBarriers = 0;      // B4: all-commands barriers (each drains the GPU)
    std::uint64_t barriersSkipped = 0;   // IN-419: commands recorded without a barrier (no dependency)
    std::uint64_t fragmentBarriers = 0;  // B4: fragment-to-fragment barriers inside the render pass
    std::uint64_t ropBatches = 0;        // IN-481: batches drawn through colour attachments
    std::uint64_t renderPasses = 0;      // B4: render pass begins (one after every compute or transfer break)
    std::uint64_t drawFlushes = 0;       // IN-420: groups of deferred draw batches recorded in one render pass
    std::uint64_t midFrameSubmissions = 0;  // IN-410: submissions after a number of draw batches
    std::uint64_t points = 0;      // TASK-179: point primitives drawn
    std::uint64_t pointQuads = 0;  // TASK-179: of those, drawn as two triangles (no largePoints)
    // TASK-178 (GT4_CORE2_SUBMIT_AFTER_WAIT): the first mid-frame submission after a waited one, sent after K batches.
    std::uint64_t afterWaitSubmissions = 0;
    // TASK-178 diagnostics: waited submissions (each drains the whole queue), and what follows them until the next submission.
    std::uint64_t drains = 0;
    std::uint64_t drainIdleNs = 0;    // GPU idle (timestamps): the drained work's last command to the next submission's first
    std::uint64_t drainIdles = 0;     // drains measured that way
    std::uint64_t drainGapCpuNs = 0;  // CPU: the end of the fence wait to the next vkQueueSubmit
    std::uint64_t drainGaps = 0;
    // TASK-180 test4 (P1-C, STUDY-UNIFIED-MEMORY 7.2): every data path in bytes, always counted (an integer add next to
    // work that already copies or records a command). Printed every 600 VBlanks as "[core2] data paths/vblank".
    // CPU -> GPU: what the CPU writes into host-visible memory the GPU reads.
    std::uint64_t cpuGlmBytes = 0;       // UploadGlm: the pages written into the GLM
    std::uint64_t cpuPaletteBytes = 0;   // DecodeTexture: a palette expanded for a decode (1024 bytes)
    std::uint64_t cpuPatchBytes = 0;     // Patch: the words written into a target
    std::uint64_t cpuClutBytes = 0;      // WriteClut: the 256 RGBA words mirrored into the GPU CLUT
    std::uint64_t cpuToneBytes = 0;      // Tone: the table upload (1024 bytes)
    std::uint64_t cpuFillStateBytes = 0; // Draw: a fill's push state
    std::uint64_t cpuVertexBytes = 0;    // Draw: the vertices pushed into the vertex ring
    std::uint64_t cpuUniformBytes = 0;   // Draw: the draw state pushed into the uniform ring (once per draw)
    std::uint64_t cpuImageBytes = 0;     // UploadAtlas, UploadImage: texels staged for the GPU
    // GPU copies: bytes a copy command moves (read plus write are two different paths, counted by direction).
    std::uint64_t gpuCopyBufferBytes = 0;          // buffer-to-buffer (GLM pages, CLUT writes, CLUT reads)
    std::uint64_t gpuCopyBufferToImageBytes = 0;   // staging to image (atlas, standalone uploads)
    std::uint64_t gpuCopyImageBytes = 0;           // image-to-image (grow, copy, self copy, deinterlace, snapshot)
    std::uint64_t gpuCopyImageToBufferBytes = 0;   // image-to-buffer (image reads)
    std::uint64_t gpuClearBytes = 0;               // clears of new images and whole-image fills
    // Bytes the CPU memcpys out of mapped GPU memory (readbacks). The fence is what makes them legal (see the host-read
    // barrier).
    std::uint64_t hostReadBytes = 0;
    // Ring high-water marks (bytes) since the last print, their shares (one recording's part of each ring), and how many
    // times a ring was full enough to submit early.
    std::uint64_t ringHighStaging = 0, ringHighVertex = 0, ringHighUniform = 0;
    std::uint64_t ringShareStagingBytes = 0, ringShareVertexBytes = 0, ringShareUniformBytes = 0;
    std::uint64_t ringFullSubmits = 0;
    // TASK-180 test4 (P1-D): the dependencies into the host domain recorded before a host read
    // (GT4_CORE2_HOST_READ_BARRIER, on by default). Not part of fullBarriers; pending sets are untouched.
    std::uint64_t hostReadBarriers = 0;
    // TASK-182 step 3 (O2): waits that took the newest-fence path, and the bounded spin before the blocking wait (polls and
    // the ones that saw the fence signalled already).
    std::uint64_t newestFenceWaits = 0;
    std::uint64_t spinPolls = 0;
    std::uint64_t spinHits = 0;
};

// IN-408: a CLUT load (07.4) whose source is a drawn 32-bit colour target, done on the GPU.
struct ClutTargetLoad {
    std::uint32_t cbp = 0, cpsm = 0, csm = 0, csa = 0;
    bool eightBit = true;
    std::uint32_t cbw = 0, cou = 0, cov = 0;  // TEXCLUT (CSM2)
    std::uint32_t tbp = 0, tbw = 0;           // the target's base (blocks) and width (pages)
    std::int32_t heldX0 = 0, heldY0 = 0, heldX1 = -1, heldY1 = -1;  // its drawn and valid area (native, inclusive)
    bool fullWord = true;                     // false: only 24-bit draws, the top byte stays in memory
};

// IN-408: an indexed texture expanded through the GPU CLUT (07.5, as clut::ClutUnit::Decode).
struct GpuPalette {
    std::uint32_t cpsm = 0, csa = 0;
    std::uint64_t texa = 0;
};

class Gpu {
public:
    Gpu();
    ~Gpu();
    Gpu(const Gpu &) = delete;
    Gpu &operator=(const Gpu &) = delete;

    // `scale` is the internal scale s; `shift` the 06.3 half-pixel shift factor of the vertex stage.
    bool Init(const gpu::VulkanHandles &handles, std::uint32_t scale, float shift, std::string &error);
    bool Ready() const;
    // IN-507: compiles what the shader cache (GT4_HW_SHADER_CACHE) lacks. Returns at once when it is ready for these shaders,
    // this card and this driver, without calling `progress`; otherwise `progress` gets (done, total), starting at 0. False
    // when there is no cache to prepare.
    using ShaderProgress = std::function<void(std::size_t done, std::size_t total)>;
    bool PrepareShaders(const ShaderProgress &progress, std::string &error);
    // TASK-170: what the compile guard read at the start (variants quarantined on this card and driver after a run died
    // inside their compile; prepares one variant at a time after a parallel prepare died).
    struct ShaderGuardState {
      std::size_t quarantined = 0;
      std::size_t suspects = 0;  // TASK-170c: keys that ended one run; compiled again, quarantined at a second end
      bool serial = false;
    };
    ShaderGuardState ShaderGuard() const;
    // IN-431: true on an integrated GPU (its images live in system memory shared with the game); the image budgets
    // are smaller there.
    bool Integrated() const;
    // TASK-180 test5: the road sheen effect is off (GT4_CORE2_PERF_ROAD_SHEEN=1), for the [core2] effects log line.
    bool RoadSheenSkipped() const;
    std::uint32_t Scale() const;
    std::uint32_t MaxNativeSide() const;  // the largest native side an image can have at this scale

    // Images of `nativeWidth` x `nativeHeight` GS pixels (the image is that size times s; `scaled` false: 1x).
    // New images are cleared to zero.
    ImageId CreateImage(std::uint32_t nativeWidth, std::uint32_t nativeHeight, bool scaled, std::string &error);
    // Grows an image (1.4): a new image of the new size gets the old content copied across at scale.
    bool GrowImage(ImageId id, std::uint32_t nativeWidth, std::uint32_t nativeHeight, std::string &error);
    void DestroyImage(ImageId id);  // released once the GPU is done with it
    void ImageSize(ImageId id, std::uint32_t &nativeWidth, std::uint32_t &nativeHeight) const;
    // Copies a native rectangle of one image into another at the same position, at scale (2.4a, same format class).
    bool CopyImage(ImageId from, ImageId to, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                   std::string &error);

    // Texture atlas (1x, decoded RGBA8 texels from GS memory). Allocate returns false when the atlas is full; the
    // caller then forgets every cached place and calls ResetAtlas.
    bool AllocateAtlas(std::uint32_t width, std::uint32_t height, std::uint32_t &x, std::uint32_t &y);
    void ResetAtlas();
    bool UploadAtlas(std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height,
                     const std::uint32_t *texels, std::string &error);
    // Writes width x height texels (row-major) at the origin of a 1x image.
    bool UploadImage(ImageId id, std::uint32_t width, std::uint32_t height, const std::uint32_t *texels,
                     std::string &error);

    bool Draw(const DrawBatch &batch, std::string &error);

    // The GPU copy of GS memory (the GLM, 4 MiB) that textures are decoded from (3.7): byte ranges of `memory`
    // (offset, size) copied into it.
    bool UploadGlm(const std::uint8_t *memory, const std::vector<std::pair<std::uint32_t, std::uint32_t>> &ranges,
                   std::string &error);
    // Decodes a texture from the GLM into a 1x image (texel (0, 0) at its origin): stored words for direct formats
    // (the pixel shader expands CT16/CT24), RGBA8 through `palette` (256 entries) for indexed ones.
    bool DecodeTexture(ImageId id, std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, std::uint32_t width,
                       std::uint32_t height, const std::uint32_t *palette, std::string &error, std::uint32_t originX = 0,
                       std::uint32_t originY = 0);
    // IN-422: the next DecodeTexture or DecodeTextureGpuClut reads GS memory pages [firstPage, firstPage + pageCount)
    // (modulo 512) from `memory` (the CPU copy, current for them) instead of the GLM. Cleared by that decode.
    void SetDecodeWindow(const std::uint8_t *memory, std::uint32_t firstPage, std::uint32_t pageCount);
    // S3: GPU cost map of the commands recorded between ProfileStart and ProfileReport (each isolated and timed).
    bool ProfileStart(std::string &error);
    bool ProfileReport(std::string &error);
    // IN-408: the same decode, indexed formats expanded through the GPU CLUT buffer.
    bool DecodeTextureGpuClut(ImageId id, std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, std::uint32_t width,
                              std::uint32_t height, const GpuPalette &palette, std::string &error,
                              std::uint32_t originX = 0, std::uint32_t originY = 0);

    // IN-408: the GPU copy of the CLUT buffer (512 halfwords, one per uint). ClutFromTarget records a CLUT load
    // read from target image `target` (GLM current for the entries outside it); WriteClut records halfwords
    // [first, first + count) from the CPU CLUT unit; ReadClut submits, waits and returns all 512.
    bool ClutFromTarget(ImageId target, const ClutTargetLoad &load, std::string &error);
    // IN-409: row `row` of 1x palette image `palettes` (256 wide) from the GPU CLUT, expanded for a texture of format
    // `psm` (16 or 256 entries) with `palette`'s CPSM, CSA and TEXA.
    bool PaletteRowFromGpuClut(ImageId palettes, std::uint32_t row, std::uint32_t psm, const GpuPalette &palette,
                               std::string &error);
    bool WriteClut(std::uint32_t first, std::uint32_t count, const std::uint16_t *halfwords, std::string &error);
    bool ReadClut(std::uint16_t halfwords[512], std::string &error);
    // IN-408: writes the native rectangle (inclusive) of 32-bit colour target `id` into the GLM at the target's own
    // CT32 addresses (base `tbp` in blocks, width `tbw` in pages); `fullWord` false keeps the GLM's top bytes.
    bool MaterializeTarget(ImageId id, std::uint32_t psm, std::uint32_t tbp, std::uint32_t tbw, std::int32_t x0, std::int32_t y0,
                           std::int32_t x1, std::int32_t y1, bool fullWord, std::string &error);

    // 4.1: native rectangle (inclusive) of stored words, row-major, written into the image at scale; `mask` selects
    // the bits written.
    bool Patch(ImageId id, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
               const std::vector<std::uint32_t> &words, std::uint32_t mask, std::string &error);
    // 4.1 from the GLM: the rectangle read on the GPU in (bp, bw, psm) addressing (UploadGlm first).
    bool PatchFromGlm(ImageId id, std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, std::int32_t x0,
                      std::int32_t y0, std::int32_t x1, std::int32_t y1, std::uint32_t mask, std::string &error);
    // 4.2: the native rectangle (inclusive) reduced to 1x stored words (submits and waits).
    bool Reduce(ImageId id, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                std::vector<std::uint32_t> &words, std::string &error);
    // IN-429: several reductions in one submission and one wait; `words[i]` receives request i. False with an empty
    // error when the requests do not fit the readback buffer together (the caller reduces them one by one).
    struct ReduceRequest {
        ImageId id = 0;
        std::int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    };
    bool ReduceMany(const std::vector<ReduceRequest> &requests, std::vector<std::vector<std::uint32_t>> &words,
                    std::string &error);
    // IN-439 (performance mode, decision 0073): a reduction recorded with the work in flight and no wait. `ticket`
    // names its result; TakeReduced copies the words once the submission that carries it has completed and returns
    // false until then. Both return false with an empty error when they cannot (no free result slot, a rectangle
    // larger than a slot, an unknown ticket); a non-empty error is a failure.
    bool ReduceLater(ImageId id, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                     std::uint32_t &ticket, std::string &error);
    bool TakeReduced(std::uint32_t ticket, std::vector<std::uint32_t> &words, std::string &error);
    // 7.2: GT4's channel-shuffle hook over the image's native rectangle [0, width) x [0, height).
    bool Tone(ImageId id, std::uint32_t nativeWidth, std::uint32_t nativeHeight, const std::uint32_t palette[256],
              std::string &error);

    // 5.3: the display merge. `words` is the compose push block (28 words, core2_compose.comp); sources are the
    // images of circuits 1 and 2 (kNoImage: not shown). The result is the display image (RGBA8 words, GENERAL), or the
    // deinterlace field image when `intoField` is set (TASK-182 R8: PrepareDeinterlace chose it for this VBlank).
    bool Compose(const std::uint32_t words[28], ImageId sourceA, ImageId sourceB, std::uint32_t outWidth,
                 std::uint32_t outHeight, bool intoField, std::string &error);
    // The image the post chain last produced: the display image, where the deinterlace writes its result over the merge
    // (with GT4_CORE2_DEINT_FIELD_COMPOSE=1 the merge itself went into a field history image). Fxaa, PresentScale and
    // ReadDisplay read it.
    std::uint64_t DisplayImage() const;  // VkImage
    // TASK-182 R8 (GT4_CORE2_DEINT_FIELD_COMPOSE=1): creates the history, woven and output images of this deinterlace
    // step at step.width x step.height and remembers the history entry this field's display merge composes into, so the
    // full-display copy of the composed image into the history is not needed. False (with an error) on a bad step.
    bool PrepareDeinterlace(const gpu::GpuDrawer::DeinterlaceStep &step, std::string &error);
    // TASK-182 R8: the display image still holds the last deinterlaced output and is about to be overwritten by a merge
    // (a progressive frame, or the kernel's rare fallback to the last output): copies it into the deinterlace output
    // image, which is what the deinterlace reads as `previous_output` without the field-compose order.
    bool SnapshotLastOutput(std::string &error);
    // 13.4 at scale (IN-405): deinterlaces the top-left step.width x step.height pixels of the display image in place
    // with the old core's kernel and step (hw_deinterlace_scaled.comp). Without `fieldComposed` the display image is
    // first copied into history entry `current`; with it the merge already wrote that entry and the last adaptive
    // output is still in the display image (no copy either way). The history images (four merge images, the woven
    // image) follow the display size; the caller keeps which entry holds which field.
    bool Deinterlace(const gpu::GpuDrawer::DeinterlaceStep &step, bool fieldComposed, std::string &error);
    // 13.5 (IN-376, IN-405 in core 2): FXAA of the display image's top-left width x height pixels into the FXAA image.
    bool Fxaa(std::uint32_t width, std::uint32_t height, std::string &error);
    // IN-432: the composed frame (`fromFxaa`: the FXAA output) of width x height resampled to outWidth x outHeight
    // (core2_present_scale.comp: Lanczos-2 per axis, or FSR1 EASU when both axes grow), then FSR1 RCAS when
    // `sharpness` > 0 (exp2(-stops)). The result is PresentImage().
    bool PresentScale(bool fromFxaa, std::uint32_t width, std::uint32_t height, std::uint32_t outWidth,
                      std::uint32_t outHeight, float sharpness, std::string &error);
    std::uint64_t PresentImage() const;  // VkImage
    std::uint64_t FxaaImage() const;  // VkImage
    // `fxaa`: read the FXAA image instead of the display image.
    bool ReadDisplay(std::uint32_t width, std::uint32_t height, std::vector<std::uint32_t> &pixels, std::string &error,
                     bool presentScaled,
                     bool fxaa = false);
    // Diagnostics: the top-left width x height words of an image (submits and waits).
    bool ReadImage(ImageId id, std::uint32_t width, std::uint32_t height, std::vector<std::uint32_t> &words,
                   std::string &error);

    bool Submit(bool wait, std::string &error);
    // TASK-178: batches per mid-frame submission, and after a waited submission (0: the rule is off).
    std::uint32_t SubmitEvery() const;
    std::uint32_t SubmitAfterWait() const;
    const Core2GpuStats &Stats() const;
    // TASK-180 test4 (P1-C): the ring high-water marks go back to zero after the data-path line printed them.
    void ResetRingHighMarks();
    // Diagnostics: cumulative full barriers as " label[/kind]=count" (the command that needed one and its hazard).
    std::string BarrierReport() const;
    // TASK-182 (GT4_CORE2_BARRIER_TRACE=1): fragment barriers, render pass ends, flushes, submissions and descriptor traffic by
    // reason, cumulative, one line per row; empty without the switch.
    std::string SyncReport(std::uint64_t vblanks) const;
    // TASK-182 step 2a (GT4_CORE2_BARRIER_TRACE=1): a batch that samples the colour or depth target it draws into and needs
    // no copy of it (the oracle's D3 path 2). `disjoint`: the sampled rectangle and the drawn one do not meet, so the batch
    // reads the target itself; `frameUntouched`: the batch keeps every lane of the frame, so its own writes change nothing
    // it reads (IN-495). Counted in the sync report; does nothing without the switch.
    enum class SelfSampleNoCopy { Disjoint, FrameUntouched };
    void NoteSelfSampleNoCopy(SelfSampleNoCopy why);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace hwgs::core2

// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from docs/hw-renderer-spec/CORE2-SPEC.md and CORE2-DESIGN.md (TASK-027 core rewrite), clean room
// (decision 0070).
//
// CORE2, the CoreGS 2.0 core rewrite (owner decision 2026-10-28): a target cache in which every target is ONE image
// at the internal scale. GS memory (the CPU shadow memory) holds what the EE uploaded; target content reaches it only
// through readbacks, reduced by point sampling (CORE2-SPEC 4.2). A target is never rebuilt from 1x memory, except
// for the rectangles the EE really uploaded into it (4.1). writeback::Renderer forwards to this class when
// GT4_HW_CORE=2.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../writeback/hw_gs_writeback.hpp"

namespace hwgs::core2 {

// Defined in core2_gpu.hpp; forward declared here (and in hw_gs_writeback.hpp) so the GPU header's include of the
// writeback one stays one-way.
struct Core2GpuStats;

// The session counters of CORE2 (replay summary and the [hw] core2 line of the F8 report).
struct Core2Counters {
    std::uint64_t ropBatches = 0;         // IN-481, from the GPU's counters
    std::uint64_t roadSheensSkipped = 0;  // IN-501, from the GPU's counters
    std::uint64_t sheensEarly = 0;        // IN-503, from the GPU's counters
    std::uint64_t targetsCreated = 0;
    std::uint64_t targetsAged = 0;        // deleted after 60 frames unused (6.2)
    std::uint64_t targetsReplaced = 0;    // replaced on a width change (2.1) or full dirty coverage (4.1)
    std::uint64_t targetsGrown = 0;
    std::uint64_t texturesFromTargets = 0;
    std::uint64_t texturesFromMemory = 0;
    std::uint64_t atlasHits = 0;
    std::uint64_t dirtyPatches = 0;
    std::uint64_t dirtyPatchPixels = 0;
    std::uint64_t readbacks = 0;
    std::uint64_t readbackPixels = 0;
    std::uint64_t toneRuns = 0;
    std::uint64_t toneDrawsSkipped = 0;
    std::uint64_t toneDropsOnArrival = 0;      // TASK-168b: of those, dropped as the front end emitted them (no OnBatch)
    std::uint64_t toneAlphaOnlyUnsupported = 0;  // 7.2 alpha-only variant: drawn normally (not implemented yet)
    std::uint64_t displayFromTargets = 0;
    std::uint64_t displayFromMemory = 0;
    std::uint64_t displayStaleTargets = 0;     // IN-532: displays left black because a newer target of another format drew over
    std::uint64_t indexedFromTargetReadbacks = 0;  // interim 3.5: a T8/T4 texture over a drawn target
    std::uint64_t directFromTargetReadbacks = 0;   // a 16/24/32-bit memory texture over a drawn target (not a hit)
    std::uint64_t clutReadbacks = 0;
    std::uint64_t delayedDownloads = 0;        // IN-439: downloads answered with the previous same download's words
    std::uint64_t hiddenFlareTables = 0;       // IN-441: flare visibility tables answered as zeros        // IN-439: downloads answered with the previous same download's words
    std::uint64_t downloadReadbacks = 0;       // EE downloads (local-to-host transfers) that read back a target
    // TASK-178: EE downloads handled (hidden flare tables included), and the targets and native pixels the readbacks of
    // downloads reduced.
    std::uint64_t downloads = 0;
    std::uint64_t downloadTargets = 0;
    std::uint64_t downloadPixels = 0;
    std::uint64_t drains = 0;                // TASK-178, from the GPU's counters: waited submissions
    std::uint64_t afterWaitSubmissions = 0;  // TASK-178, from the GPU's counters: submissions by GT4_CORE2_SUBMIT_AFTER_WAIT
    // TASK-182 step 3 (O2), from the GPU's counters: waits done on the newest fence only, and the bounded spin (polls, and the
    // ones that saw the signalled fence without blocking).
    std::uint64_t newestFenceWaits = 0;
    std::uint64_t spinPolls = 0;
    std::uint64_t spinHits = 0;
    std::uint64_t batchedReadbacks = 0;        // IN-429: target readbacks reduced together in one submission
    std::uint64_t textureBytes = 0;            // IN-431: 1x texels in the texture cache now
    std::uint64_t dirtyMerged = 0;
    std::uint64_t specialisedBatches = 0;      // IN-437: batches drawn with a pipeline specialised to their state
    std::uint64_t computeFills = 0;            // IN-442: batches drawn as compute fills
    std::uint64_t selfCopies = 0;              // spec 8.8: batches that sampled a copy of the target they draw into
    std::uint64_t selfCopyDraws = 0;           // TASK-182 R1: those copies drawn inside the open pass (GT4_CORE2_SELF_COPY_DRAW)
    // TASK-182 R8 (GT4_CORE2_DEINT_FIELD_COMPOSE=1), from the GPU's counters: display merges written into the
    // deinterlace field's history image, and full-display copies the deinterlace recorded.
    std::uint64_t postFieldComposes = 0;
    std::uint64_t postCopies = 0;
    // IN-470: texture replacement (chapter 11.7).
    std::uint64_t replacementFiles = 0;        // names registered from the pack folder
    std::uint64_t replacementKeys = 0;         // Format P keys computed (new content)
    std::uint64_t replacementRequests = 0;     // images asked of the loader
    std::uint64_t replacementLoads = 0;        // images put on the GPU
    std::uint64_t replacementFailed = 0;       // files refused
    std::uint64_t replacementHits = 0;         // batches drawn with a replacement
    std::uint64_t replacementLate = 0;         // IN-533: batches drawn with the game's texture while their replacement loaded
    std::uint64_t replacementMemoryPalettes = 0;  // IN-534: lookups that read a palette loaded on the GPU from GS memory
    std::uint64_t replacementWaitUs = 0;       // IN-533: time the GS worker waited for replacements it needed now
    std::uint64_t depthIntoColourFills = 0;    // IN-535: colour targets a depth fill at another base wrote into
    std::uint64_t depthIntoColourPixels = 0;   // IN-535: their pixels (1x)
    std::uint64_t replacementBytes = 0;        // replacement images on the GPU now
    std::uint64_t replacementDumps = 0;        // textures dumped
    std::uint64_t depthClears = 0;             // IN-529: depth clears through FRAME and ZBUF drawn as a depth write
    std::uint64_t uploadsReplayed = 0;         // IN-486: uploads recognised and copied instead of swizzled
    std::uint64_t uploadBytesReplayed = 0;
    std::uint64_t unchangedUploads = 0;        // IN-450: uploads that changed no byte and kept the page versions
    std::uint64_t contentHits = 0;             // IN-453: stale by page version, but the texture's blocks unchanged
    std::uint64_t staleBy[5] = {};             // IN-450: stale decodes by the newest page writer (upload, readback, delayed download, move, whole memory)
    std::uint64_t forcedBilinearBatches = 0;   // IN-433: textured batches sampled bilinearly by GT4_HW_FILTER             // IN-430: dirty rectangles merged into the target's last pending one
    std::uint64_t cpuSpriteBatches = 0;        // IN-407: small untextured sprite batches drawn into GS memory
    std::uint64_t clutTargetLoads = 0;         // IN-408: CLUT loads read from a drawn target on the GPU
    std::uint64_t gpuPaletteDecodes = 0;       // IN-408: indexed textures expanded through the GPU CLUT
    std::uint64_t clutReads = 0;               // IN-408: GPU CLUT read back for the CPU (the tone hook)
    std::uint64_t materializations = 0;        // IN-408: drawn target rectangles written into the GLM on the GPU
    std::uint64_t texturesOverTargets = 0;     // IN-408: textures decoded over drawn targets (through the GLM)
    std::uint64_t gpuReadKeeps = 0;            // IN-408: GPU-read targets stored to memory before being dropped
    std::uint64_t storesBeforeWrite = 0;       // IN-411: drawn targets stored before a write patches whole rows
    std::uint64_t liveTargets = 0;             // B3: targets alive at the last [core2] line
    std::uint64_t targetSlots = 0;             // B3: the size of the target table then
    std::uint64_t decodesFirst = 0;            // B2: memory texture decodes with no cached entry
    std::uint64_t decodesStale = 0;            // B2: ... whose entry's pages were written since
    std::uint64_t decodesKeyClash = 0;         // B2: ... whose hash slot held another key
    std::uint64_t decodesOverTargets = 0;      // B2: ... over drawn targets (never cached, IN-411)
    std::uint64_t paletteRowsWritten = 0;      // IN-409: palette rows filled (CPU palettes and GPU CLUT expansions)
    std::uint64_t paletteRowHits = 0;          // IN-409: draws whose palette row was already filled
    std::uint64_t paletteSegmentBatches = 0;   // TASK-168: batches drawn with several palette segments (rows per vertex)
    std::uint64_t paletteSegmentsDrawn = 0;    // TASK-168: the segments of those batches
    std::uint64_t paletteSegmentSplits = 0;    // TASK-168: segmented batches drawn segment by segment (tone hook, replacement)
    std::uint64_t paletteSegmentsWholeNoTone = 0;  // TASK-168b: segmented T8 sprite batches drawn whole, no segment taking ToneHook
    std::uint64_t paletteSegmentToneRefusals = 0;  // TASK-168b: CLUT loads that ended a T8 sprite batch during a tone run
    std::uint64_t clutMultiTargetLoads = 0;    // IN-417: GPU CLUT loads read from the GLM over several or depth targets
    std::uint64_t mipDraws = 0;           // C1: textured draws with TEX1.MXL > 0 and MMIN 2..5
    std::uint64_t trilinearDraws = 0;     // C1: ... of which MMIN 3 or 5 (mip-linear)
    std::uint64_t mipConstantLod = 0;     // C1: ... of which LCM = 1 (constant LOD K)
    std::uint64_t mipMaxLevels = 0;       // C1: the largest MXL seen
    std::uint64_t mipTextures = 0;        // IN-428: memory textures decoded with their mip levels
    std::uint64_t mipLevelsDecoded = 0;   // IN-428: mip levels (1..MXL) decoded
    std::uint64_t pageExactDirties = 0;        // IN-416: writes turned into page-exact dirty rectangles
    std::uint64_t overTargetReuses = 0;        // IN-513: decodes over targets reused in their VBlank
    std::uint64_t bitPatches = 0;              // IN-512: blocks of a small write patched bit by bit, with no store
    std::uint64_t blockPatchWrites = 0;        // IN-538: large writes merged block by block, with no store
    std::uint64_t blockPatchRects = 0;         // ... rectangles of filled blocks the targets took from the GLM
    std::uint64_t gpuStores = 0;               // IN-539: stores before whole-block writes done on the GPU
    std::uint64_t gpuStoreRows = 0;            // ... target page-row patches from the GLM they made
    std::uint64_t materializationsLeftOut = 0;  // IN-510: GLM writes left out because the GLM already held the target
    std::uint64_t drawnPagesMissed = 0;        // IN-508: stores left out because the write shares no page with the drawn area
    std::uint64_t uploadChecks = 0;            // GT4_CORE2_UPLOAD_CHECK: fast uploads compared with the generic path
    std::uint64_t uploadCheckMismatches = 0;   // ... of which differed (also printed per upload)
};

class Renderer {
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    void SetScale(std::uint32_t scale, float shift);
    std::uint32_t Scale() const;
    bool Init(const gpu::VulkanHandles &handles, std::string &error);
    bool PrepareShaders(const std::function<void(std::size_t, std::size_t)> &progress, std::string &error);  // IN-507

    bool GifTransfer(std::uint32_t path, const std::uint8_t *bytes, std::size_t byteCount);
    bool GifBusy(std::uint32_t path) const;
    void GifReset(std::uint32_t pathMask);
    bool WritePrivileged(std::uint32_t address, std::uint64_t value);
    void ResetGs();
    void ClearVram();
    std::size_t ReadFifo(void *destination, std::size_t byteCount);
    void Flush();

    bool ComposeFrameGpu(writeback::GpuFrame &out);
    bool ReadGpuFrame(const writeback::GpuFrame &frame, std::vector<std::uint32_t> &pixels);
    void ComposeFrame(writeback::FrameOutput &out);
    void SetDisplayField(bool odd);
    void SetAgeTargets(bool age);
    bool DisplayField() const;
    void SetOutputSettings(const writeback::OutputSettings &settings);
    void SetFxaa(bool enabled);  // IN-405: 13.5 FXAA of the presented frame
    // IN-432: the window rectangle the presented frame is resampled to (0 x 0: none).
    void SetPresentSize(std::uint32_t width, std::uint32_t height);
    // IN-433: 0 textures filtered as the game asks, 1 bilinear on every textured draw, 2 on all but sprites.
    void SetForcedBilinear(std::uint32_t mode);

    // The shadow memory after writing every target's drawn area back (a host read of the whole memory).
    const std::uint8_t *Slm();
    const writeback::Counters &GetCounters() const;
    const Core2Counters &Core2Stats() const;
    // TASK-180 test4 (P1-C): the GPU-side counters (data paths, ring high-water marks, host-read barriers). Read by the
    // data-path acceptance test; the F8 report and the 600-VBlank lines read them through Gpu::Stats.
    const Core2GpuStats &GpuStats() const;
    bool Failed() const;
    const std::string &FailureText() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace hwgs::core2

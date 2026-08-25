#pragma once

#include "OloEngine/Core/Base.h"

#include <glm/glm.hpp>

#include <cmath>

namespace OloEngine
{
    // ── Where the atlas PAGE index rides in a per-draw lightmap region (#868) ──
    //
    // The per-draw region travels as ONE `glm::vec4`
    // (InstanceData::LightmapScaleOffset) all the way from DrawMeshCommand
    // through CommandBucket's FrameDataBuffer stream into the instance SSBO.
    // Multi-paging needs a fifth number — the page — and every lane of that
    // vec4 is already spoken for by `uv2 * xy + zw`.
    //
    // The encoding: the page rides in the INTEGER PART OF `.z`.
    //
    //     encoded = vec4(scaleX, scaleY, offsetX + page, offsetY)
    //     page    = floor(encoded.z)
    //     offsetX = encoded.z - page
    //
    // Why this lane and not a new one: adding a fifth lane means a second
    // FrameDataBuffer stream, a second `anyNonDefault` batching probe and a
    // second field on DrawMeshCommand — new surface on the exact path
    // baked-lightmap-pipeline.md §5 warns collapses per-draw values during
    // auto-batching. This encoding touches the value in exactly two places
    // (SceneLightmapRuntime composes it, sampleLightmapIrradiance decodes it)
    // and nothing in between changes at all.
    //
    // Why it is EXACT, not approximate: a page-local `offsetX` is always
    // `regionX / atlasSize` with both operands integers and `atlasSize` a power
    // of two, so it is a binary fraction with at most log2(atlasSize) ≤ 14
    // fractional bits, in [0, 1). The page is an integer < kMaxLightmapPages
    // (3 bits). Their sum needs ≤ 17 significand bits, well inside f32's 24 —
    // so `floor()` recovers the page and the subtraction recovers the offset
    // BIT-EXACTLY, for every legal atlas size and page. `LightmapPageEncodingTest`
    // asserts that with EXPECT_EQ on floats, which is correct here precisely
    // because exactness is the contract.
    //
    // Why the "no lightmap" sentinel survives: the sentinel is the ALL-ZERO
    // vec4, and every consumer gates on `scaleOffset.x <= 0.0` — the SCALE
    // lane, which this encoding never touches. An all-zero region decodes to
    // page 0 with a zero offset, which is never reached because the scale gate
    // fires first. A real region always has `scaleX = regionSize / atlasSize > 0`.
    //
    // The GLSL twin lives in OloEditor/assets/shaders/include/LightmapSampling.glsl
    // (`decodeLightmapPage` / `decodeLightmapOffset`). Change one, change both.

    // Hard ceiling on atlas pages. Matches OLmapFormat::MaxPageCount — the
    // .olmap reader rejects anything above it, so a bake may never produce
    // one. Kept as its own constant so the encoding's exactness argument above
    // does not silently depend on a serialization header.
    inline constexpr u32 kMaxLightmapPages = 8;

    // GPU bytes per atlas texel: the runtime uploads the atlas as an RGBA16F
    // Texture2DArray (4 channels x 2 bytes). The VRAM budget below is
    // expressed against this, not against the f32 CPU-side texel data.
    inline constexpr u64 kLightmapAtlasBytesPerTexel = 8;

    // ── THE BUDGET POLICY (#868 acceptance bullet 3) ───────────────────────
    //
    // "As many pages as needed" is explicitly not a policy. What this ceiling
    // bounds is PAGING — the memory #868 newly made it possible to ask for —
    // and the page count is DERIVED from it, the same shape
    // shared-atlas-allocator.md records for the impostor retrofit (a budget
    // gate, chosen over unbounded growth). At the default 1024² atlas one page
    // is 8 MiB, so the 64 MiB ceiling yields the format's full 8 pages.
    //
    // IT IS NOT A TOTAL VRAM CEILING, and the difference is worth stating
    // plainly rather than being discovered: the floor of ONE page is
    // unconditional, so an atlas whose SINGLE page already exceeds the ceiling
    // still bakes. A 4096² page is 128 MiB and a 16384² page is 2 GiB, both
    // above the 64 MiB budget, and both are allowed — because one page is
    // exactly the pre-#868 footprint (`AtlasSize` has always been the user's
    // knob, and the old single `Texture2D` cost the same bytes), so refusing
    // would be a regression in behaviour that has nothing to do with paging.
    // `Prepare()` logs a warning when it happens so the cost is visible.
    //
    // The honest statement of the policy is therefore: **total atlas VRAM is
    // `max(one page, floor(budget / pageBytes)) * pageBytes`** — bounded by the
    // ceiling for every scene whose atlas fits it at all, and equal to the
    // pre-#868 single-page cost for every scene whose atlas does not.
    //
    // This is a CONSTANT, not a scene-authored setting, on purpose: the page
    // budget changes the atlas LAYOUT, so making it authorable would require
    // adding it to SceneLightmapRuntime::ComputeBakeKey (and to
    // SceneLightmapSettings, and its serializer) or a budget change would
    // silently leave every existing bake validating against a layout it can no
    // longer reproduce. If it ever becomes authorable, it MUST join the key.
    inline constexpr u64 kLightmapAtlasMemoryBudgetBytes = 64ull * 1024ull * 1024ull;

    // Pages the budget affords for a square atlas of `atlasSize`, clamped to
    // [1, kMaxLightmapPages]. Never returns 0 — see the unconditional
    // one-page floor above; `SinglePageExceedsLightmapBudget` is how a caller
    // asks whether that floor is what it just got.
    [[nodiscard("the derived page budget is the whole point of the call")]] inline constexpr u32
    LightmapPageBudget(u32 atlasSize) noexcept
    {
        if (atlasSize == 0)
        {
            return 1;
        }
        const u64 bytesPerPage = static_cast<u64>(atlasSize) * atlasSize * kLightmapAtlasBytesPerTexel;
        const u64 affordable = bytesPerPage == 0 ? 1 : kLightmapAtlasMemoryBudgetBytes / bytesPerPage;
        if (affordable <= 1)
        {
            return 1;
        }
        return affordable >= kMaxLightmapPages ? kMaxLightmapPages : static_cast<u32>(affordable);
    }

    // True when a SINGLE page of this atlas already exceeds the memory ceiling,
    // i.e. the one-page floor above is load-bearing and the bake is about to
    // spend more than the budget nominally allows. Not an error — see the
    // policy note — but `Prepare()` warns on it so it is never silent.
    [[nodiscard("the budget-overrun answer is the whole point of the call")]] inline constexpr bool
    SinglePageExceedsLightmapBudget(u32 atlasSize) noexcept
    {
        const u64 bytesPerPage = static_cast<u64>(atlasSize) * atlasSize * kLightmapAtlasBytesPerTexel;
        return bytesPerPage > kLightmapAtlasMemoryBudgetBytes;
    }

    // Compose the per-draw vec4 from a PAGE-LOCAL scale/offset and a page index.
    // `pageLocal` is exactly what LightmapEntityEntry::ScaleOffset stores.
    [[nodiscard("returns the encoded region; it does not modify pageLocal")]] inline glm::vec4
    EncodeLightmapRegion(const glm::vec4& pageLocal, u32 page) noexcept
    {
        return glm::vec4(pageLocal.x, pageLocal.y, pageLocal.z + static_cast<f32>(page), pageLocal.w);
    }

    // The C++ twin of the shader's decode — used by tests and by any CPU-side
    // consumer that needs to read a composed region back.
    //
    // TOTAL BY CONSTRUCTION, for any float whatsoever. A validated asset can
    // never reach this with a degenerate `.z` (LightmapAsset::Validate() bounds
    // the region to its page), but this is a public inline helper in a header:
    // the next caller is not required to have validated anything, and
    // `static_cast<u32>` of +Inf or of a finite value past UINT32_MAX is
    // UNDEFINED BEHAVIOUR, not a large number. So the range check happens
    // BEFORE the cast, and anything outside [0, kMaxLightmapPages) — negative,
    // NaN, infinite, or merely too large — reads as page 0, which is the same
    // answer the scale-lane gate would produce anyway.
    [[nodiscard("returns the decoded page; it does not modify encoded")]] inline u32
    DecodeLightmapPage(const glm::vec4& encoded) noexcept
    {
        const f32 page = std::floor(encoded.z);
        // Written as a positive range test so NaN falls through it (every NaN
        // comparison is false), rather than as a negated one that NaN passes.
        if (!(page >= 1.0f && page < static_cast<f32>(kMaxLightmapPages)))
        {
            return 0; // 0, negatives, NaN, +/-Inf, and anything past the cap
        }
        return static_cast<u32>(page);
    }

    [[nodiscard("returns the page-local region; it does not modify encoded")]] inline glm::vec4
    DecodeLightmapPageLocalRegion(const glm::vec4& encoded) noexcept
    {
        return glm::vec4(encoded.x, encoded.y, encoded.z - std::floor(encoded.z), encoded.w);
    }
} // namespace OloEngine

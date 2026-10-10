#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/BoundingVolume.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <span>

// =============================================================================
// Which virtual-shadow-map clip levels foliage invalidates, and where (#1580).
//
// Foliage moves every frame (wind, interaction), and until #1580 its whole
// footprint was submitted to the VSM as deforming every frame, so every page it
// touched in all sixteen clip levels was redrawn every frame and the page cache
// never applied to it: 19-103 ms of VSM foliage casters at 1080p against 4-10 ms
// with cascades.
//
// THE ERROR BOUND, from declared numbers only. A plant's vertices never move
// further from their rest position than its layer DECLARES: the wind bound
// FoliageWindMaximumDisplacement (the same pad the cull's instance bounds
// carry), plus, inside an interaction influence's cylinder only,
// FoliageInteractionMaximumDisplacement (the shader clamps the summed push to
// it; with no active influence the contribution is exactly zero). So:
//
//   * A clip level whose texel is FINER than the layer's declared displacement
//     can resolve the motion: the footprint is invalidated in it every frame
//     (LevelsFinerThan with the wind bound; around each influence, with wind
//     plus interaction).
//   * A level whose texel is at least that displacement keeps its cached
//     foliage pages. Its depth texels can lag the live plants by at most the
//     peak-to-peak motion, 2 x displacement <= 2 texels of that level, and a
//     coarse level is only sampled by receivers more than ~1024 texels'
//     worth of distance away (VirtualShadowMap::SelectClipLevel puts a
//     receiver at distance d on the level whose texel is d/1024..d/2048), or
//     as the one-frame fallback for a page not yet drawn.
//   * Coarse levels are still refreshed, one level every
//     kCoarseRefreshPeriodFrames frames in rotation (CoarseRefreshLevel), so
//     what the displacement bound does not describe -- a density-LOD fade, a
//     card/mesh hand-over, a card turning toward a moved eye -- cannot stay
//     stale for longer than one rotation.
//   * When the main eye moves, the levels a receiver within the layer's reach
//     selects (and the next coarser one, the marker's fallback) are
//     invalidated too: those camera-dependent changes happen within the
//     layer's view distance, so that is where they can be seen.
//
// Everything that changes the foliage's silhouette WITHOUT motion -- a
// regeneration, an edited layer, a moved terrain, foliage arriving or leaving
// -- still invalidates every level, through ShadowFamilyCache's revision and
// bounds comparison, exactly as before.
// =============================================================================

namespace OloEngine::FoliageShadowInvalidation
{
    // Which policy ShadowRenderPass applies. Policy is what ships; the other
    // two exist for the A/B that proves it (FoliageVsmCacheVisualEvidenceTest):
    // EveryFrame is the pre-#1580 behaviour (the whole footprint, every level,
    // every frame), Never invalidates nothing for motion (the negative control).
    enum class Mode : u8
    {
        Policy,
        EveryFrame,
        Never,
    };

    namespace Detail
    {
        inline std::atomic<Mode>& ModeStorage()
        {
            static std::atomic<Mode> s_Mode{ Mode::Policy };
            return s_Mode;
        }
    } // namespace Detail

    inline void SetMode(Mode mode)
    {
        Detail::ModeStorage().store(mode, std::memory_order_relaxed);
    }

    [[nodiscard]] inline Mode GetMode()
    {
        return Detail::ModeStorage().load(std::memory_order_relaxed);
    }

    // The level lane of a VirtualShadowMap invalidation: 0 is "every level",
    // which is what every pre-#1580 caller means and gets.
    inline constexpr u32 kEveryLevel = 0u;

    // One coarse level every this many frames. Four: with the committed
    // scenes' wind bounds (0.1-0.84 m) the coarse set is levels 7-15 at most,
    // nine levels, so the slowest level is refreshed every 36 frames (0.6 s at
    // 60 Hz) -- the default FoliageInteraction recovery time, the timescale on
    // which the foliage's own state visibly changes -- while the amortised
    // cost is a quarter of one level's redraw per frame. A shorter period
    // spends a whole level's redraw per frame to refresh shadows only distant
    // receivers sample; a longer one lets a moved eye's LOD hand-over sit in
    // the coarse levels for over a second.
    inline constexpr u32 kCoarseRefreshPeriodFrames = 4u;

    // Bit L set when level L's texel is strictly finer than `displacement`.
    // A non-finite or non-positive displacement resolves in no level.
    [[nodiscard]] inline u32 LevelsFinerThan(std::span<const f32> texelWorldSizes, f32 displacement)
    {
        u32 mask = 0u;
        if (!(displacement > 0.0f) || !std::isfinite(displacement))
            return 0u;
        const auto count = std::min<sizet>(texelWorldSizes.size(), 32u);
        for (sizet level = 0; level < count; ++level)
        {
            if (texelWorldSizes[level] < displacement)
                mask |= 1u << level;
        }
        return mask;
    }

    // Bits 0..level (inclusive), clamped to `levelCount` levels.
    [[nodiscard]] inline u32 LevelsUpTo(i32 level, u32 levelCount)
    {
        if (level < 0 || levelCount == 0u)
            return 0u;
        const u32 last = std::min<u32>(static_cast<u32>(level), std::min<u32>(levelCount, 32u) - 1u);
        return last >= 31u ? ~0u : ((1u << (last + 1u)) - 1u);
    }

    // The ONE coarse level the rotation refreshes on `frame`, as a bit, or 0
    // on the frames between refreshes and when there is no coarse level.
    [[nodiscard]] inline u32 CoarseRefreshLevel(u32 coarseMask, u64 frame,
                                                u32 periodFrames = kCoarseRefreshPeriodFrames)
    {
        if (coarseMask == 0u || periodFrames == 0u || frame % periodFrames != 0u)
            return 0u;
        u32 pick = static_cast<u32>((frame / periodFrames) % static_cast<u64>(std::popcount(coarseMask)));
        for (u32 bits = coarseMask; bits != 0u; bits &= bits - 1u)
        {
            if (pick-- == 0u)
                return bits & (~bits + 1u);
        }
        return 0u;
    }

    // The world box an interaction influence can bend plants inside: every
    // root the shader's cylinder test admits (FoliageInteraction.glsl: within
    // `radius` in XZ, from 1.25 radius below the centre to height + 0.25
    // radius above it, at the current OR previous centre, because the
    // velocity pass evaluates both), grown by `plantReach`, the farthest any
    // point of a plant's padded bound gets from its root.
    [[nodiscard]] inline BoundingBox InfluenceBounds(const glm::vec3& center, const glm::vec3& previousCenter,
                                                     f32 radius, f32 height, f32 plantReach)
    {
        const glm::vec3 lo = glm::min(center, previousCenter);
        const glm::vec3 hi = glm::max(center, previousCenter);
        const f32 r = std::max(radius, 0.0f);
        const f32 reach = std::max(plantReach, 0.0f);
        return BoundingBox(glm::vec3(lo.x - r - reach, lo.y - 1.25f * r - reach, lo.z - r - reach),
                           glm::vec3(hi.x + r + reach, hi.y + std::max(height, 0.0f) + 0.25f * r + reach,
                                     hi.z + r + reach));
    }

    // Two invalidations merged into one box (the queue's saturation path)
    // invalidate the union of their levels; "every level" absorbs anything.
    [[nodiscard]] inline u32 MergeLevelMasks(u32 a, u32 b)
    {
        return (a == kEveryLevel || b == kEveryLevel) ? kEveryLevel : (a | b);
    }
} // namespace OloEngine::FoliageShadowInvalidation

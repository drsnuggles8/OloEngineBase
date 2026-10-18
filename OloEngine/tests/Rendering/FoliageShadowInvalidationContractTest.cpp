// OLO_TEST_LAYER: L1
// =============================================================================
// FoliageShadowInvalidationContractTest — issue #1580.
//
// Which virtual-shadow-map clip levels foliage invalidates, from declared
// displacement bounds (FoliageShadowInvalidation.h). Pins:
//   * the level selection: a level invalidates exactly when its texel is finer
//     than the declared displacement, with the real clip texel sizes;
//   * the coarse-level rotation: one level per period, every coarse level
//     within one rotation, none of the fine ones;
//   * the influence box: every root the shader's cylinder bends, grown by the
//     plant's reach, and nothing near the rest of the footprint;
//   * the level lane through the invalidation queue, including its
//     saturation merge.
// The visual half is FoliageVsmCacheVisualEvidenceTest.
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/Shadow/FoliageShadowInvalidation.h"
#include "OloEngine/Renderer/Shadow/ShadowInvalidationQueue.h"
#include "OloEngine/Renderer/Shadow/VirtualShadowMap.h"
#include "OloEngine/Terrain/Foliage/FoliageInteraction.h"
#include "OloEngine/Terrain/Foliage/FoliageWind.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>

namespace OloEngine::Tests
{
    namespace
    {
        namespace FSI = FoliageShadowInvalidation;

        // The texel sizes VirtualShadowMap::BuildClipProjections gives each
        // level at the default Clip0HalfExtent, taken from the real builder.
        std::array<f32, VSM::kClipLevels> DefaultTexelSizes()
        {
            std::array<VSM::ClipProjection, VSM::kClipLevels> clips{};
            std::array<glm::ivec2, VSM::kClipLevels> origins{};
            std::array<glm::ivec2, VSM::kClipLevels> prev{};
            VirtualShadowMap::BuildClipProjections(glm::normalize(glm::vec3(-0.4f, -0.8f, -0.3f)), glm::vec3(0.0f),
                                                   VirtualShadowMapSettings{}, prev, true, clips, origins);
            std::array<f32, VSM::kClipLevels> out{};
            for (u32 level = 0; level < VSM::kClipLevels; ++level)
                out[level] = clips[level].TexelWorldSize;
            return out;
        }
    } // namespace

    TEST(FoliageShadowInvalidation, LevelsFinerThanTheDeclaredDisplacementAreExactlyTheInvalidatedOnes)
    {
        const auto texels = DefaultTexelSizes();
        // The real meadow grass: WindStrength 0.35, stiffness 0.15, branch 0.25.
        const f32 wind = FoliageWindMaximumDisplacement(0.35f, SanitizeFoliageWind(0.15f, 0.25f, 0.0f));
        const u32 mask = FSI::LevelsFinerThan(texels, wind);
        ASSERT_NE(mask, 0u);
        ASSERT_NE(mask, (1u << VSM::kClipLevels) - 1u) << "every level invalidated: nothing would ever be cached";
        for (u32 level = 0; level < VSM::kClipLevels; ++level)
        {
            const bool invalidated = (mask & (1u << level)) != 0u;
            EXPECT_EQ(invalidated, texels[level] < wind)
                << "level " << level << " texel " << texels[level] << " m against a " << wind << " m bound";
        }
        // Contiguous from level 0: texels double per level.
        EXPECT_EQ(std::countr_one(mask), std::popcount(mask));
        std::printf("[foliage-vsm] meadow grass: wind bound %.3f m invalidates levels 0..%d per frame\n", wind,
                    std::popcount(mask) - 1);

        // Interaction adds its own bound inside an influence: strictly more levels.
        const u32 withInteraction = FSI::LevelsFinerThan(texels, wind + FoliageInteractionMaximumDisplacement(1.0f));
        EXPECT_EQ(withInteraction & mask, mask);
        EXPECT_GT(std::popcount(withInteraction), std::popcount(mask));

        // No declared motion, no per-frame invalidation; garbage resolves nowhere.
        EXPECT_EQ(FSI::LevelsFinerThan(texels, 0.0f), 0u);
        EXPECT_EQ(FSI::LevelsFinerThan(texels, -1.0f), 0u);
        EXPECT_EQ(FSI::LevelsFinerThan(texels, std::numeric_limits<f32>::quiet_NaN()), 0u);
        EXPECT_EQ(FSI::LevelsFinerThan(texels, std::numeric_limits<f32>::infinity()), 0u);
        // A texel exactly equal to the bound cannot resolve it.
        EXPECT_EQ(FSI::LevelsFinerThan(texels, texels[4]), (1u << 4) - 1u);
    }

    TEST(FoliageShadowInvalidation, CoarseLevelsRotateOnePerPeriodAndAllWithinOneRotation)
    {
        const u32 fine = (1u << 9) - 1u;                            // levels 0..8
        const u32 coarse = ((1u << VSM::kClipLevels) - 1u) & ~fine; // levels 9..15
        const u32 period = FSI::kCoarseRefreshPeriodFrames;
        const u32 rotation = period * static_cast<u32>(std::popcount(coarse));

        u32 seen = 0u;
        for (u64 frame = 1000; frame < 1000 + rotation; ++frame)
        {
            const u32 level = FSI::CoarseRefreshLevel(coarse, frame);
            if (frame % period != 0u)
            {
                EXPECT_EQ(level, 0u) << "frame " << frame << " refreshed between periods";
                continue;
            }
            ASSERT_EQ(std::popcount(level), 1) << "frame " << frame << " must refresh exactly one level";
            EXPECT_EQ(level & fine, 0u) << "the rotation refreshed a level that is already redrawn every frame";
            EXPECT_EQ(seen & level, 0u) << "a level came round twice inside one rotation";
            seen |= level;
        }
        EXPECT_EQ(seen, coarse) << "a coarse level was never refreshed within one rotation";

        EXPECT_EQ(FSI::CoarseRefreshLevel(0u, 0u), 0u);
        EXPECT_EQ(FSI::CoarseRefreshLevel(coarse, 0u, 0u), 0u);
    }

    TEST(FoliageShadowInvalidation, InfluenceBoxHoldsEveryBentRootAndNotTheFootprint)
    {
        const glm::vec3 center(100.0f, 12.0f, -40.0f);
        const glm::vec3 previous(101.5f, 12.0f, -40.5f);
        constexpr f32 kRadius = 1.5f;
        constexpr f32 kHeight = 1.0f;
        constexpr f32 kReach = 2.0f;
        const BoundingBox box = FSI::InfluenceBounds(center, previous, kRadius, kHeight, kReach);

        // Every root the cylinder test in FoliageInteraction.glsl weights above
        // zero, at either centre, with a plant of reach kReach on it.
        for (const glm::vec3& c : { center, previous })
        {
            for (f32 angle = 0.0f; angle < 6.2831f; angle += 0.39f)
            {
                for (f32 dy : { -1.25f * kRadius, 0.0f, kHeight + 0.25f * kRadius })
                {
                    const glm::vec3 root = c + glm::vec3(std::cos(angle) * kRadius * 0.999f, dy, std::sin(angle) * kRadius * 0.999f);
                    for (const glm::vec3& tip : { root + glm::vec3(kReach, 0, 0), root - glm::vec3(kReach, 0, 0),
                                                  root + glm::vec3(0, kReach, 0), root - glm::vec3(0, kReach, 0),
                                                  root + glm::vec3(0, 0, kReach), root - glm::vec3(0, 0, kReach) })
                    {
                        EXPECT_TRUE(glm::all(glm::greaterThanEqual(tip, box.Min - 1e-4f)) &&
                                    glm::all(glm::lessThanEqual(tip, box.Max + 1e-4f)))
                            << "a bent plant's tip at (" << tip.x << ", " << tip.y << ", " << tip.z << ") is outside";
                    }
                }
            }
        }
        // Local: a few metres, nowhere near a 1 km footprint.
        const glm::vec3 extent = box.Max - box.Min;
        EXPECT_LT(extent.x, 2.0f * (kRadius + kReach) + 2.0f);
        EXPECT_LT(extent.z, 2.0f * (kRadius + kReach) + 2.0f);
    }

    TEST(FoliageShadowInvalidation, LevelLaneSurvivesTheQueueAndItsSaturationMerge)
    {
        TArray64<glm::vec4> pending;
        AppendBoundedShadowInvalidation(pending, { 0, 0, 0 }, { 1, 1, 1 }, 4, 0b0110u);
        AppendBoundedShadowInvalidation(pending, { 2, 0, 0 }, { 3, 1, 1 }, 4);
        ASSERT_EQ(pending.Num(), 4);
        EXPECT_EQ(static_cast<u32>(pending[0].w), 0b0110u);
        EXPECT_EQ(static_cast<u32>(pending[2].w), FSI::kEveryLevel) << "a caller that names no levels means every level";

        // Saturated: the merged box invalidates the union of the levels...
        TArray64<glm::vec4> full;
        AppendBoundedShadowInvalidation(full, { 0, 0, 0 }, { 1, 1, 1 }, 1, 0b0001u);
        AppendBoundedShadowInvalidation(full, { 5, 0, 0 }, { 6, 1, 1 }, 1, 0b1000u);
        ASSERT_EQ(full.Num(), 2);
        EXPECT_EQ(static_cast<u32>(full[0].w), 0b1001u);
        EXPECT_FLOAT_EQ(full[1].x, 6.0f);
        // ...and "every level" absorbs a masked one rather than narrowing it.
        AppendBoundedShadowInvalidation(full, { 9, 0, 0 }, { 9.5f, 1, 1 }, 1);
        EXPECT_EQ(static_cast<u32>(full[0].w), FSI::kEveryLevel);
        EXPECT_EQ(FSI::MergeLevelMasks(0b10u, 0b01u), 0b11u);
        EXPECT_EQ(FSI::MergeLevelMasks(FSI::kEveryLevel, 0b01u), FSI::kEveryLevel);
    }

    // #1580's card cut-over: no receiver inside the mesh reach samples a card
    // level, and every level past the coarsest one such a receiver can select
    // takes cards.
    TEST(FoliageShadowInvalidation, CardLevelsStartPastEveryLevelAReceiverInsideTheMeshReachSelects)
    {
        constexpr f32 kClip0 = 2.0f;
        constexpr f32 kBias = 1.0f;
        for (const f32 reach : { 0.5f, 12.0f, 30.0f, 35.1f, 64.0f, 400.0f })
        {
            SCOPED_TRACE(reach);
            const i32 atReach = VirtualShadowMap::SelectClipLevel(reach, kClip0, kBias);
            const u32 first = FSI::FirstCardLevel(atReach, VSM::kClipLevels);
            // Every receiver distance inside the reach selects a mesh level.
            for (f32 d = 0.05f; d <= reach; d *= 1.05f)
                EXPECT_LT(static_cast<u32>(VirtualShadowMap::SelectClipLevel(d, kClip0, kBias)), first) << d;
            // And the level right after the reach's is a card level: the cut-over
            // is as fine as that guarantee allows, not further out.
            EXPECT_EQ(first, std::min<u32>(static_cast<u32>(atReach) + 1u, VSM::kClipLevels));
        }
        // The default clip extent puts a 30 m mesh band's last level at L4 (texel
        // 1.6 cm, the levels receivers 16-32 m out select), so cards from L5.
        EXPECT_EQ(FSI::FirstCardLevel(VirtualShadowMap::SelectClipLevel(30.0f, kClip0, kBias), VSM::kClipLevels), 5u);
        EXPECT_EQ(FSI::FirstCardLevel(-1, VSM::kClipLevels), 0u) << "no mesh layer casts: every level may take cards";
        EXPECT_EQ(FSI::FirstCardLevel(static_cast<i32>(VSM::kClipLevels) - 1, VSM::kClipLevels), VSM::kClipLevels)
            << "a reach the coarsest level serves leaves no card level";
    }
} // namespace OloEngine::Tests

// =============================================================================
// HZBGeneratorResizeTest.cpp
//
// Regression coverage for the GTAO ghost-halo bug.
//
// HZBGenerator allocates a power-of-2 R32F texture sized to NextPow2(viewport).
// Its m_UVFactor (= viewport / HZB texture size) is what GTAO multiplies its
// screen-space sample UVs by to land inside the live viewport region of the
// HZB. So when the viewport changes but the rounded-up power-of-2 doesn't,
// the texture really doesn't need re-creating — but the UV scale *does* still
// change, because the live viewport-to-HZB ratio shifted.
//
// Previously, HZBGenerator::Resize() updated m_UVFactor only on the
// texture-re-creation path. A viewport drag like 4584x2515 -> 4582x2515 stays
// inside the same 8192x4096 bucket → early-return → m_UVFactor stayed at the
// value computed for the original viewport. GTAO then sampled the HZB at the
// *old* viewport-to-HZB ratio while normals sampled at the live ratio, and
// the AO mask landed offset from the geometry along whichever axis was being
// resized. Persistent for the rest of the session: every subsequent
// same-bucket resize re-hit the early-return.
//
// These tests pin the contract that m_UVFactor reflects the *current* live
// viewport-to-HZB ratio after every Resize(), regardless of whether the HZB
// texture itself had to be re-created.
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/HZBGenerator.h"

#include <gtest/gtest.h>

#include <glm/glm.hpp>

namespace OloEngine::Tests
{
    namespace
    {
        // Local mirror of HZBGenerator's private NextPowerOfTwo so the
        // expected-value math below stays self-contained / readable.
        constexpr u32 NextPow2(u32 v)
        {
            v--;
            v |= v >> 1;
            v |= v >> 2;
            v |= v >> 4;
            v |= v >> 8;
            v |= v >> 16;
            v++;
            return v;
        }

        // Expected UVFactor for a given viewport: viewport / NextPow2(viewport).
        glm::vec2 ExpectedUVFactor(u32 viewportW, u32 viewportH)
        {
            return glm::vec2(
                static_cast<f32>(viewportW) / static_cast<f32>(NextPow2(viewportW)),
                static_cast<f32>(viewportH) / static_cast<f32>(NextPow2(viewportH)));
        }
    } // namespace

    // -----------------------------------------------------------------------
    // First Resize: UVFactor should reflect the new viewport-to-HZB ratio,
    // and HZB texture dimensions should round up to the next power of 2.
    // -----------------------------------------------------------------------
    TEST(HZBGeneratorResizeTest, FirstResizeSetsUVFactorAndHZBDimensions)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        HZBGenerator gen;
        gen.Resize(4584u, 2515u);

        EXPECT_EQ(gen.GetHZBWidth(), NextPow2(4584u))
            << "HZB width must round up to next power of two.";
        EXPECT_EQ(gen.GetHZBHeight(), NextPow2(2515u))
            << "HZB height must round up to next power of two.";

        const auto expected = ExpectedUVFactor(4584u, 2515u);
        const auto actual = gen.GetUVFactor();
        EXPECT_FLOAT_EQ(actual.x, expected.x);
        EXPECT_FLOAT_EQ(actual.y, expected.y);
    }

    // -----------------------------------------------------------------------
    // Regression: a same-bucket resize must STILL refresh m_UVFactor, even
    // though the HZB texture itself can be reused. Both axes vary within the
    // same power-of-two range.
    //
    // Concrete: 4584x2515 -> 4578x2515 (both round up to 8192x4096).
    // Pre-fix: UVFactor stayed at (4584/8192, 2515/4096) instead of updating
    // to (4578/8192, 2515/4096) → GTAO sampled the HZB offset by 6 viewport
    // pixels along X, visible as a translucent halo on the right side of
    // every entity until the editor restarted.
    // -----------------------------------------------------------------------
    TEST(HZBGeneratorResizeTest, SameBucketResizeRefreshesUVFactor)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        HZBGenerator gen;

        gen.Resize(4584u, 2515u);
        const auto hzbW1 = gen.GetHZBWidth();
        const auto hzbH1 = gen.GetHZBHeight();

        gen.Resize(4578u, 2515u);

        // HZB texture dimensions should be unchanged — both viewports fall
        // inside the same 8192x4096 bucket.
        EXPECT_EQ(gen.GetHZBWidth(), hzbW1);
        EXPECT_EQ(gen.GetHZBHeight(), hzbH1);

        // But the UV factor MUST track the new viewport.
        const auto expected = ExpectedUVFactor(4578u, 2515u);
        const auto actual = gen.GetUVFactor();
        EXPECT_FLOAT_EQ(actual.x, expected.x)
            << "Same-bucket viewport-width change must refresh UVFactor.x.";
        EXPECT_FLOAT_EQ(actual.y, expected.y)
            << "Same-bucket viewport-height (unchanged here) must remain correct.";
    }

    // -----------------------------------------------------------------------
    // Y-only resize within the same bucket: matches the user-visible symptom
    // (drag the bottom viewport edge → ghost halo offset in Y). UVFactor.y
    // must follow the viewport even though hzbH is unchanged.
    // -----------------------------------------------------------------------
    TEST(HZBGeneratorResizeTest, SameBucketHeightOnlyResizeRefreshesUVFactorY)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        HZBGenerator gen;

        gen.Resize(1920u, 1080u);
        gen.Resize(1920u, 1054u); // both heights round up to 2048

        EXPECT_EQ(gen.GetHZBHeight(), NextPow2(1080u));
        EXPECT_EQ(gen.GetHZBHeight(), NextPow2(1054u))
            << "Sanity: 1080 and 1054 round up to the same power of two.";

        const auto expected = ExpectedUVFactor(1920u, 1054u);
        const auto actual = gen.GetUVFactor();
        EXPECT_FLOAT_EQ(actual.x, expected.x);
        EXPECT_FLOAT_EQ(actual.y, expected.y);
    }

    // -----------------------------------------------------------------------
    // X-only resize within the same bucket: matches the user-visible symptom
    // (drag the right viewport edge → ghost halo offset in X). Symmetric to
    // the Y-only test above.
    // -----------------------------------------------------------------------
    TEST(HZBGeneratorResizeTest, SameBucketWidthOnlyResizeRefreshesUVFactorX)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        HZBGenerator gen;

        gen.Resize(1920u, 1080u);
        gen.Resize(1820u, 1080u); // both widths round up to 2048

        EXPECT_EQ(gen.GetHZBWidth(), NextPow2(1920u));
        EXPECT_EQ(gen.GetHZBWidth(), NextPow2(1820u))
            << "Sanity: 1920 and 1820 round up to the same power of two.";

        const auto expected = ExpectedUVFactor(1820u, 1080u);
        const auto actual = gen.GetUVFactor();
        EXPECT_FLOAT_EQ(actual.x, expected.x);
        EXPECT_FLOAT_EQ(actual.y, expected.y);
    }

    // -----------------------------------------------------------------------
    // Walk a sequence of small same-bucket resizes — the realistic shape of
    // a viewport drag, which is what surfaced the bug originally. Every
    // intermediate state must have the correct UVFactor.
    // -----------------------------------------------------------------------
    TEST(HZBGeneratorResizeTest, ChainOfSameBucketResizesAllRefreshUVFactor)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        HZBGenerator gen;

        // Every value must be strictly greater than 1024 so NextPow2(h) is
        // 2048 for the whole walk — the test name says "same-bucket", and
        // 1024u is itself a power of two (NextPow2(1024) == 1024) so any
        // value ≤ 1024 would drop into a different bucket and trigger the
        // texture-re-create path instead of exercising the early-return.
        constexpr u32 kHeights[] = { 1080u, 1054u, 1040u, 1030u, 1027u, 1025u };
        constexpr u32 kExpectedBucket = NextPow2(kHeights[0]);
        for (const u32 h : kHeights)
        {
            ASSERT_EQ(NextPow2(h), kExpectedBucket)
                << "kHeights must all land in the same power-of-2 bucket; "
                   "h="
                << h << " maps to " << NextPow2(h)
                << " but the chain expects " << kExpectedBucket;

            gen.Resize(1920u, h);

            EXPECT_EQ(gen.GetHZBHeight(), kExpectedBucket)
                << "Same-bucket resize must not re-create the HZB texture; "
                   "h="
                << h;
            const auto expected = ExpectedUVFactor(1920u, h);
            const auto actual = gen.GetUVFactor();
            EXPECT_FLOAT_EQ(actual.x, expected.x) << "viewport height " << h;
            EXPECT_FLOAT_EQ(actual.y, expected.y) << "viewport height " << h;
        }
    }

    // -----------------------------------------------------------------------
    // Cross-bucket resize: behaviour should still be correct (this is the
    // non-buggy code path pre-fix). Guards against a regression where the
    // fix accidentally breaks the texture-re-create path.
    // -----------------------------------------------------------------------
    TEST(HZBGeneratorResizeTest, CrossBucketResizeUpdatesBothTextureAndUVFactor)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        HZBGenerator gen;

        gen.Resize(1920u, 1080u); // bucket 2048x2048
        const auto hzbW1 = gen.GetHZBWidth();
        const auto hzbH1 = gen.GetHZBHeight();

        gen.Resize(4584u, 2515u); // bucket 8192x4096 — different

        EXPECT_NE(gen.GetHZBWidth(), hzbW1)
            << "Cross-bucket resize must re-create the HZB texture (new width).";
        EXPECT_NE(gen.GetHZBHeight(), hzbH1)
            << "Cross-bucket resize must re-create the HZB texture (new height).";

        const auto expected = ExpectedUVFactor(4584u, 2515u);
        const auto actual = gen.GetUVFactor();
        EXPECT_FLOAT_EQ(actual.x, expected.x);
        EXPECT_FLOAT_EQ(actual.y, expected.y);
    }
} // namespace OloEngine::Tests

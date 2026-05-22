#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Math/Math.h"

#include <bit>
#include <cmath>
#include <limits>

// =============================================================================
// MathTest — contracts for the OloEngine::Math namespace helpers.
//
// Today only BitwiseEqual is covered. DecomposeTransform is exercised
// transitively by TransformComponentTest and the gizmo paths.
//
// BitwiseEqual is the canonical bit-exact comparison for floats / glm types,
// the documented replacement for ==/!= per cpp-coding-quality.md §2a. Three
// invariants matter:
//   1. Bit-equal inputs compare equal (including NaN payloads).
//   2. +0.0f and -0.0f differ (== would falsely report them equal).
//   3. Composite trivially-copyable types (glm::vec, glm::mat) work without
//      special-casing.
// =============================================================================

namespace
{
    using OloEngine::Math::BitwiseEqual;

    TEST(MathBitwiseEqualTest, EqualScalarsCompareEqual)
    {
        EXPECT_TRUE(BitwiseEqual(0.0f, 0.0f));
        EXPECT_TRUE(BitwiseEqual(1.0f, 1.0f));
        EXPECT_TRUE(BitwiseEqual(-3.14159f, -3.14159f));
        EXPECT_TRUE(BitwiseEqual(0.0, 0.0));
    }

    TEST(MathBitwiseEqualTest, DifferentScalarsCompareUnequal)
    {
        EXPECT_FALSE(BitwiseEqual(1.0f, 1.0000001f));
        EXPECT_FALSE(BitwiseEqual(0.0f, 1e-30f));
    }

    TEST(MathBitwiseEqualTest, PositiveAndNegativeZeroAreDistinct)
    {
        // The whole point of bit-exact comparison: +0 and -0 have different bit
        // patterns, so a change from one to the other is a real authored edit
        // that needs to round-trip through undo / save-game / network sync.
        // operator== falsely reports them equal.
        const f32 posZero = 0.0f;
        const f32 negZero = -0.0f;
        ASSERT_EQ(posZero, negZero); // sanity: operator== says equal
        EXPECT_FALSE(BitwiseEqual(posZero, negZero));
    }

    TEST(MathBitwiseEqualTest, NaNIsBitwiseEqualToItself)
    {
        // operator== always returns false for NaN. BitwiseEqual returns true
        // when the bit pattern is identical — required so that a serialized
        // NaN round-tripping into a component doesn't constantly look like
        // a fresh edit on every frame.
        const f32 nan = std::numeric_limits<f32>::quiet_NaN();
        ASSERT_NE(nan, nan); // sanity: operator== says not equal
        const f32 nanCopy = nan;
        EXPECT_TRUE(BitwiseEqual(nan, nanCopy));
    }

    TEST(MathBitwiseEqualTest, DifferentNaNPayloadsAreUnequal)
    {
        // Two NaNs with different bit payloads are distinct values for
        // change-detection purposes.
        const u32 quietPattern = 0x7FC00000;
        const u32 signalingPattern = 0x7FA00001;
        const f32 quietNaN = std::bit_cast<f32>(quietPattern);
        const f32 signalingNaN = std::bit_cast<f32>(signalingPattern);
        EXPECT_FALSE(BitwiseEqual(quietNaN, signalingNaN));
    }

    TEST(MathBitwiseEqualTest, GlmVecComparison)
    {
        const glm::vec3 a{ 1.0f, 2.0f, 3.0f };
        const glm::vec3 b{ 1.0f, 2.0f, 3.0f };
        const glm::vec3 c{ 1.0f, 2.0f, 3.0001f };
        EXPECT_TRUE(BitwiseEqual(a, b));
        EXPECT_FALSE(BitwiseEqual(a, c));
    }

    TEST(MathBitwiseEqualTest, GlmMat4Comparison)
    {
        const glm::mat4 identity{ 1.0f };
        const glm::mat4 identityCopy{ 1.0f };
        glm::mat4 perturbed{ 1.0f };
        perturbed[3][0] = 1e-7f;
        EXPECT_TRUE(BitwiseEqual(identity, identityCopy));
        EXPECT_FALSE(BitwiseEqual(identity, perturbed));
    }

    TEST(MathBitwiseEqualTest, IntegerTypesAlsoWork)
    {
        // The helper isn't float-only — any trivially-copyable type can use
        // it, which is convenient when a struct mixes scalars and bools.
        EXPECT_TRUE(BitwiseEqual(42, 42));
        EXPECT_FALSE(BitwiseEqual(42, 43));

        struct Trivial
        {
            f32 X;
            i32 Y;
            bool Z;
            // Pad to force a struct with implicit padding bytes. Bit-exact
            // comparison includes padding, so callers must zero-init for
            // predictable equality — same rule as std::memcmp.
        };

        const Trivial a{ 1.0f, 7, true };
        Trivial b = a;
        EXPECT_TRUE(BitwiseEqual(a, b));
        b.Y = 8;
        EXPECT_FALSE(BitwiseEqual(a, b));
    }
} // namespace

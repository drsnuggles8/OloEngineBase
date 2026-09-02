#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Math/Math.h"

#include <bit>
#include <cmath>
#include <limits>
#include <type_traits>

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
        // it, including structs of scalars (see the padding rule below).
        EXPECT_TRUE(BitwiseEqual(42, 42));
        EXPECT_FALSE(BitwiseEqual(42, 43));

        // Structs. `BitwiseEqual` is `memcmp` over `sizeof(T)`, so it compares
        // every byte of the object representation, PADDING included, and the
        // language leaves padding bytes unspecified. No initialisation idiom
        // pins them: the first version of this case copy-constructed `b` from
        // `a` and failed under GCC, whose implicit copy is member-wise and skips
        // the padding; the second version value-initialised both objects
        // (`Trivial a = Trivial();`, which does zero the whole representation)
        // and then assigned the members, and failed under GCC 14 -O3, which
        // scalarises the value-initialised local and re-materialises it with
        // fresh garbage in bytes 9-11 when the address is finally taken for the
        // memcmp. Both are conforming: after a member store the padding is
        // unspecified again, whatever it held before.
        //
        // The rule for callers therefore is: `BitwiseEqual` on a struct is only
        // deterministic when the struct has no padding, and the type system can
        // check that. `std::has_unique_object_representations_v<T>` is exactly
        // the predicate, and it is what the two layouts below differ in. (A
        // padded type still compares equal when both operands come from ONE
        // memcpy'd source, but that is the caller's guarantee, not the helper's.)
        struct Padded
        {
            f32 m_X;
            i32 m_Y;
            bool m_Z;
        };
        static_assert(sizeof(Padded) > sizeof(f32) + sizeof(i32) + sizeof(bool),
                      "Padded must actually carry padding for the static_assert below to say anything");
        static_assert(!std::has_unique_object_representations_v<Padded>,
                      "a padded struct has no unique object representation, so BitwiseEqual on it is not deterministic");

        // Integer members only: for a float member Clang answers
        // has_unique_object_representations = false (NaN payloads) where GCC
        // and MSVC answer true, and this assert must not depend on which
        // compiler is asked. Float bit patterns are covered by the cases above.
        struct Packed
        {
            i32 m_X;
            i32 m_Y;
            u32 m_Z; // a bool here would reintroduce three padding bytes
        };
        static_assert(sizeof(Packed) == sizeof(i32) + sizeof(i32) + sizeof(u32), "Packed must have no padding");
        static_assert(std::has_unique_object_representations_v<Packed>,
                      "Packed is the kind of layout BitwiseEqual is deterministic for");

        Packed a{ -3, 7, 1u };
        Packed b{ -3, 7, 1u };
        EXPECT_TRUE(BitwiseEqual(a, b));
        b.m_Y = 8;
        EXPECT_FALSE(BitwiseEqual(a, b));
        b.m_Y = 7;
        b.m_Z = 0u;
        EXPECT_FALSE(BitwiseEqual(a, b)) << "the trailing member takes part in the comparison";
    }

    // =========================================================================
    // IsFinite — the predicate behind the cpp-coding-quality §2 rule that every
    // float crossing a serialization boundary (scene YAML, save-games, network)
    // must be validated before it reaches the GPU / physics / matrix math.
    // =========================================================================
    using OloEngine::Math::IsFinite;

    TEST(MathIsFiniteTest, FiniteScalarsVectorsAndMatricesPass)
    {
        EXPECT_TRUE(IsFinite(0.0f));
        EXPECT_TRUE(IsFinite(-123.456f));
        EXPECT_TRUE(IsFinite(1e30)); // double overload
        EXPECT_TRUE(IsFinite(glm::vec2{ 1.0f, -2.0f }));
        EXPECT_TRUE(IsFinite(glm::vec3{ 1.0f, -2.0f, 3.0f }));
        EXPECT_TRUE(IsFinite(glm::vec4{ 1.0f, -2.0f, 3.0f, -4.0f }));
        EXPECT_TRUE(IsFinite(glm::mat4{ 2.5f }));
    }

    TEST(MathIsFiniteTest, NaNIsRejectedInEveryComponentSlot)
    {
        const f32 nan = std::numeric_limits<f32>::quiet_NaN();
        EXPECT_FALSE(IsFinite(nan));
        EXPECT_FALSE(IsFinite(glm::vec2{ 1.0f, nan }));
        EXPECT_FALSE(IsFinite(glm::vec3{ nan, 2.0f, 3.0f }));
        EXPECT_FALSE(IsFinite(glm::vec4{ 1.0f, 2.0f, 3.0f, nan }));

        // A single non-finite matrix element must fail the whole-matrix check —
        // any slot, not just the diagonal.
        glm::mat4 m{ 1.0f };
        m[2][1] = nan;
        EXPECT_FALSE(IsFinite(m));
    }

    TEST(MathIsFiniteTest, PositiveAndNegativeInfinityAreRejected)
    {
        const f32 inf = std::numeric_limits<f32>::infinity();
        EXPECT_FALSE(IsFinite(inf));
        EXPECT_FALSE(IsFinite(-inf));
        EXPECT_FALSE(IsFinite(glm::vec3{ 1.0f, inf, 3.0f }));
        EXPECT_FALSE(IsFinite(glm::vec4{ -inf, 2.0f, 3.0f, 4.0f }));

        glm::mat4 m{ 1.0f };
        m[0][0] = -inf;
        EXPECT_FALSE(IsFinite(m));
    }
} // namespace

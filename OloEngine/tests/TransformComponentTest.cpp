#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Scene/Components.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file

// =============================================================================
// Default State
// =============================================================================

TEST(TransformComponent, DefaultsAreIdentity)
{
    TransformComponent tc;
    EXPECT_EQ(tc.Translation, glm::vec3(0.0f));
    EXPECT_EQ(tc.GetRotationEuler(), glm::vec3(0.0f));
    EXPECT_FLOAT_EQ(tc.GetRotation().w, 1.0f);
    EXPECT_FLOAT_EQ(tc.GetRotation().x, 0.0f);
    EXPECT_FLOAT_EQ(tc.GetRotation().y, 0.0f);
    EXPECT_FLOAT_EQ(tc.GetRotation().z, 0.0f);
    EXPECT_EQ(tc.Scale, glm::vec3(1.0f));
}

// =============================================================================
// SetRotationEuler / GetRotationEuler
// =============================================================================

TEST(TransformComponent, SetRotationEulerUpdatesQuaternion)
{
    TransformComponent tc;
    glm::vec3 euler(0.1f, 0.2f, 0.3f);
    tc.SetRotationEuler(euler);

    EXPECT_EQ(tc.GetRotationEuler(), euler);

    glm::quat expected = glm::quat(euler);
    glm::quat actual = tc.GetRotation();
    EXPECT_NEAR(actual.w, expected.w, 1e-5f);
    EXPECT_NEAR(actual.x, expected.x, 1e-5f);
    EXPECT_NEAR(actual.y, expected.y, 1e-5f);
    EXPECT_NEAR(actual.z, expected.z, 1e-5f);
}

// =============================================================================
// SetRotation (quaternion) / GetRotation
// =============================================================================

TEST(TransformComponent, SetRotationQuaternionUpdatesEuler)
{
    TransformComponent tc;
    glm::vec3 euler(0.3f, 0.5f, 0.1f);
    glm::quat q = glm::quat(euler);
    tc.SetRotation(q);

    glm::quat actual = tc.GetRotation();
    EXPECT_NEAR(actual.w, q.w, 1e-5f);
    EXPECT_NEAR(actual.x, q.x, 1e-5f);
    EXPECT_NEAR(actual.y, q.y, 1e-5f);
    EXPECT_NEAR(actual.z, q.z, 1e-5f);

    // Euler should produce the same quaternion when converted back
    glm::quat fromEuler = glm::quat(tc.GetRotationEuler());
    EXPECT_NEAR(glm::dot(fromEuler, q), 1.0f, 1e-4f);
}

// =============================================================================
// Flip Prevention
// =============================================================================

TEST(TransformComponent, SetRotationPreventsFlips)
{
    TransformComponent tc;
    // Start with a known euler
    glm::vec3 startEuler(0.1f, 0.2f, 0.3f);
    tc.SetRotationEuler(startEuler);

    // Apply a very small rotation change via quaternion
    glm::vec3 slightlyDifferent(0.11f, 0.21f, 0.31f);
    glm::quat q = glm::quat(slightlyDifferent);
    tc.SetRotation(q);

    // The resulting euler should be close to the input, not flipped by pi
    glm::vec3 resultEuler = tc.GetRotationEuler();
    EXPECT_NEAR(resultEuler.x, slightlyDifferent.x, 0.1f);
    EXPECT_NEAR(resultEuler.y, slightlyDifferent.y, 0.1f);
    EXPECT_NEAR(resultEuler.z, slightlyDifferent.z, 0.1f);
}

// =============================================================================
// GetTransform / SetTransform round-trip
// =============================================================================

TEST(TransformComponent, GetTransformUsesQuaternion)
{
    TransformComponent tc;
    tc.Translation = { 1.0f, 2.0f, 3.0f };
    tc.SetRotationEuler({ 0.1f, 0.2f, 0.3f });
    tc.Scale = { 1.0f, 1.0f, 1.0f };

    glm::mat4 m = tc.GetTransform();
    glm::mat4 expected = glm::translate(glm::mat4(1.0f), tc.Translation)
                       * glm::toMat4(tc.GetRotation())
                       * glm::scale(glm::mat4(1.0f), tc.Scale);

    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            EXPECT_NEAR(m[col][row], expected[col][row], 1e-5f);
        }
    }
}

TEST(TransformComponent, SetTransformRoundTrip)
{
    TransformComponent original;
    original.Translation = { 5.0f, -3.0f, 7.0f };
    original.SetRotationEuler({ 0.4f, -0.2f, 0.6f });
    original.Scale = { 2.0f, 0.5f, 3.0f };

    glm::mat4 mat = original.GetTransform();

    TransformComponent restored;
    restored.SetTransform(mat);

    EXPECT_NEAR(restored.Translation.x, original.Translation.x, 1e-4f);
    EXPECT_NEAR(restored.Translation.y, original.Translation.y, 1e-4f);
    EXPECT_NEAR(restored.Translation.z, original.Translation.z, 1e-4f);

    EXPECT_NEAR(restored.Scale.x, original.Scale.x, 1e-4f);
    EXPECT_NEAR(restored.Scale.y, original.Scale.y, 1e-4f);
    EXPECT_NEAR(restored.Scale.z, original.Scale.z, 1e-4f);

    // Quaternions should match (or be negated equivalent)
    glm::quat q1 = original.GetRotation();
    glm::quat q2 = restored.GetRotation();
    EXPECT_NEAR(std::abs(glm::dot(q1, q2)), 1.0f, 1e-3f);
}

// =============================================================================
// Copy semantics
// =============================================================================

TEST(TransformComponent, CopyPreservesPrivateFields)
{
    TransformComponent src;
    src.Translation = { 1.0f, 2.0f, 3.0f };
    src.SetRotationEuler({ 0.5f, 0.6f, 0.7f });
    src.Scale = { 2.0f, 3.0f, 4.0f };

    TransformComponent dst = src;

    EXPECT_EQ(dst.Translation, src.Translation);
    EXPECT_EQ(dst.GetRotationEuler(), src.GetRotationEuler());
    EXPECT_EQ(dst.Scale, src.Scale);

    glm::quat q1 = src.GetRotation();
    glm::quat q2 = dst.GetRotation();
    EXPECT_FLOAT_EQ(q1.w, q2.w);
    EXPECT_FLOAT_EQ(q1.x, q2.x);
    EXPECT_FLOAT_EQ(q1.y, q2.y);
    EXPECT_FLOAT_EQ(q1.z, q2.z);
}

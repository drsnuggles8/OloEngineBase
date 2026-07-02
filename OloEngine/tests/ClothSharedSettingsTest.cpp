#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// OLO_TEST_LAYER: unit
// =============================================================================
// ClothSharedSettingsTest — pure contract test for JoltShapes::CreateClothSharedSettings
// (issue #460, soft-body first slice).
//
// Pins the grid→soft-body-settings math WITHOUT a PhysicsSystem or GL context:
//   - particle count == columns × rows, grid clamped to [2,128] per axis,
//   - the world transform is baked into every particle's rest position,
//   - the attachment mode pins exactly the right particles (inverse mass 0),
//   - free particles share the total mass (Σ 1/invMass ≈ totalMass),
//   - two faces are emitted per grid cell,
//   - non-finite input fails closed (nullptr).
// These run in CI; the on-screen drape/collision behaviour is covered by the
// ClothSimulation functional test + the editor visual pass.
// =============================================================================

#include "OloEngine/Physics3D/JoltShapes.h"
#include "OloEngine/Physics3D/Physics3DTypes.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>

#include <cmath>
#include <limits>

using namespace OloEngine;

namespace
{
    // Count particles with inverse mass 0 (pinned / immovable).
    u32 CountPinned(const JPH::SoftBodySharedSettings& s)
    {
        u32 n = 0;
        for (const auto& v : s.mVertices)
            if (v.mInvMass == 0.0f)
                ++n;
        return n;
    }
} // namespace

TEST(ClothSharedSettingsTest, ParticleAndFaceCountsMatchGrid)
{
    constexpr u32 kCols = 8, kRows = 5;
    auto settings = JoltShapes::CreateClothSharedSettings(
        kCols, kRows, 2.0f, 3.0f, 1.0f, 0.0f, 0.001f, ClothAttachment::None, glm::mat4(1.0f));
    ASSERT_TRUE(settings);

    EXPECT_EQ(settings->mVertices.size(), static_cast<sizet>(kCols) * kRows);
    // Two triangles per grid cell.
    EXPECT_EQ(settings->mFaces.size(), static_cast<sizet>(kCols - 1) * (kRows - 1) * 2);
}

TEST(ClothSharedSettingsTest, GridResolutionClampedToValidRange)
{
    // Below the minimum (needs >= 2 per axis to form a quad) is clamped up.
    auto tiny = JoltShapes::CreateClothSharedSettings(
        0, 1, 1.0f, 1.0f, 1.0f, 0.0f, 0.001f, ClothAttachment::None, glm::mat4(1.0f));
    ASSERT_TRUE(tiny);
    EXPECT_EQ(tiny->mVertices.size(), static_cast<sizet>(2) * 2);

    // Above the ceiling is clamped down to 128 per axis.
    auto huge = JoltShapes::CreateClothSharedSettings(
        4096, 4096, 1.0f, 1.0f, 1.0f, 0.0f, 0.001f, ClothAttachment::None, glm::mat4(1.0f));
    ASSERT_TRUE(huge);
    EXPECT_EQ(huge->mVertices.size(), static_cast<sizet>(128) * 128);
}

TEST(ClothSharedSettingsTest, AttachmentPinsTheRightParticles)
{
    constexpr u32 kCols = 6, kRows = 4;

    auto none = JoltShapes::CreateClothSharedSettings(kCols, kRows, 2.0f, 2.0f, 1.0f, 0.0f, 0.001f, ClothAttachment::None, glm::mat4(1.0f));
    ASSERT_TRUE(none);
    EXPECT_EQ(CountPinned(*none), 0u) << "None must leave every particle free";

    auto topEdge = JoltShapes::CreateClothSharedSettings(kCols, kRows, 2.0f, 2.0f, 1.0f, 0.0f, 0.001f, ClothAttachment::TopEdge, glm::mat4(1.0f));
    ASSERT_TRUE(topEdge);
    EXPECT_EQ(CountPinned(*topEdge), kCols) << "TopEdge pins the whole first row";

    auto corners = JoltShapes::CreateClothSharedSettings(kCols, kRows, 2.0f, 2.0f, 1.0f, 0.0f, 0.001f, ClothAttachment::TopCorners, glm::mat4(1.0f));
    ASSERT_TRUE(corners);
    EXPECT_EQ(CountPinned(*corners), 2u) << "TopCorners pins exactly the two far corners";
    // The pinned particles are indices 0 and cols-1 (first row).
    EXPECT_EQ(corners->mVertices[0].mInvMass, 0.0f);
    EXPECT_EQ(corners->mVertices[kCols - 1].mInvMass, 0.0f);

    auto leftEdge = JoltShapes::CreateClothSharedSettings(kCols, kRows, 2.0f, 2.0f, 1.0f, 0.0f, 0.001f, ClothAttachment::LeftEdge, glm::mat4(1.0f));
    ASSERT_TRUE(leftEdge);
    EXPECT_EQ(CountPinned(*leftEdge), kRows) << "LeftEdge pins the whole first column";
}

TEST(ClothSharedSettingsTest, FreeParticlesShareTotalMass)
{
    constexpr u32 kCols = 5, kRows = 5;
    constexpr f32 kMass = 4.0f;
    auto settings = JoltShapes::CreateClothSharedSettings(kCols, kRows, 2.0f, 2.0f, kMass, 0.0f, 0.001f, ClothAttachment::TopEdge, glm::mat4(1.0f));
    ASSERT_TRUE(settings);

    // Σ over free particles of (1 / invMass) == totalMass. Pinned particles (invMass 0)
    // carry no mass in this accounting.
    f32 accumulated = 0.0f;
    for (const auto& v : settings->mVertices)
        if (v.mInvMass > 0.0f)
            accumulated += 1.0f / v.mInvMass;
    EXPECT_NEAR(accumulated, kMass, 1.0e-3f);
}

TEST(ClothSharedSettingsTest, WorldTransformIsBakedIntoRestPositions)
{
    constexpr u32 kCols = 2, kRows = 2;
    constexpr f32 kW = 2.0f, kH = 4.0f;
    const glm::vec3 kOffset(10.0f, 5.0f, -3.0f);
    const glm::mat4 xform = glm::translate(glm::mat4(1.0f), kOffset);

    auto settings = JoltShapes::CreateClothSharedSettings(kCols, kRows, kW, kH, 1.0f, 0.0f, 0.001f, ClothAttachment::None, xform);
    ASSERT_TRUE(settings);
    ASSERT_EQ(settings->mVertices.size(), 4u);

    // Grid spans [-W/2, W/2] × [-H/2, H/2] in local X–Z, translated by kOffset. Vertex 0
    // is the (col 0, row 0) corner = local (-W/2, 0, -H/2).
    const auto& v0 = settings->mVertices[0].mPosition;
    EXPECT_NEAR(v0.x, kOffset.x - kW * 0.5f, 1.0e-4f);
    EXPECT_NEAR(v0.y, kOffset.y, 1.0e-4f);
    EXPECT_NEAR(v0.z, kOffset.z - kH * 0.5f, 1.0e-4f);

    // Vertex 3 is the (col 1, row 1) far corner = local (+W/2, 0, +H/2).
    const auto& v3 = settings->mVertices[3].mPosition;
    EXPECT_NEAR(v3.x, kOffset.x + kW * 0.5f, 1.0e-4f);
    EXPECT_NEAR(v3.z, kOffset.z + kH * 0.5f, 1.0e-4f);
}

TEST(ClothSharedSettingsTest, NonFiniteInputFailsClosed)
{
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 inf = std::numeric_limits<f32>::infinity();

    EXPECT_FALSE(JoltShapes::CreateClothSharedSettings(4, 4, nan, 2.0f, 1.0f, 0.0f, 0.001f, ClothAttachment::None, glm::mat4(1.0f)));
    EXPECT_FALSE(JoltShapes::CreateClothSharedSettings(4, 4, 2.0f, 2.0f, inf, 0.0f, 0.001f, ClothAttachment::None, glm::mat4(1.0f)));

    glm::mat4 badXform(1.0f);
    badXform[3][1] = nan;
    EXPECT_FALSE(JoltShapes::CreateClothSharedSettings(4, 4, 2.0f, 2.0f, 1.0f, 0.0f, 0.001f, ClothAttachment::None, badXform));
}

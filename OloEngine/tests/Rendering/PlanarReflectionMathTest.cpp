#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L1
// =============================================================================
// PlanarReflectionMathTest — L1 contract tests for the planar-reflection math.
//
// `OloEngine::PlanarReflection` (Renderer/PlanarReflection.h) is the pure math
// behind the mirrored-camera reflection pass: the Householder reflection matrix
// that mirrors the world across the water/mirror plane, and Lengyel's oblique
// near-plane clip that culls geometry on the far side of the plane so it cannot
// leak into the reflection. These run on the CPU in CI and pin the formulas
// independently of the GPU pass — a renderer change can pass every contract
// test and still look wrong on screen, so the PlanarReflectionRenderPass is
// ALSO covered by multi-angle screenshot evidence (see the PropertyTests dir).
//
// Conventions asserted here (must match the engine's GLM build): right-handed
// view, OpenGL clip volume (NDC z in [-1, 1], near plane at clip z = -w),
// column-major matrices.
// =============================================================================

#include "OloEngine/Renderer/PlanarReflection.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

using namespace OloEngine;

namespace
{
    // A representative camera looking down-forward at a water plane at y = h,
    // with the eye comfortably above the surface — the common reflection case.
    constexpr f32 kPlaneHeight = 2.0f;
    const glm::vec4 kWaterPlane{ 0.0f, 1.0f, 0.0f, -kPlaneHeight }; // y - h = 0, kept side y > h
    const glm::vec3 kCameraPos{ 0.0f, 5.0f, 10.0f };

    glm::mat4 MakeView()
    {
        return glm::lookAt(kCameraPos, glm::vec3{ 0.0f, kPlaneHeight, 0.0f }, glm::vec3{ 0.0f, 1.0f, 0.0f });
    }

    glm::mat4 MakeProjection()
    {
        return glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    }
} // namespace

// --- Reflection (Householder) matrix --------------------------------------

TEST(PlanarReflectionMathTest, ReflectionMatrixMirrorsAcrossHorizontalPlane)
{
    const glm::mat4 r = PlanarReflection::MakeReflectionMatrix(kWaterPlane);

    // A point at height 5 above a plane at height 2 reflects to 2*2 - 5 = -1.
    const glm::vec4 reflected = r * glm::vec4{ 1.0f, 5.0f, 3.0f, 1.0f };
    EXPECT_NEAR(reflected.x, 1.0f, 1e-5f);
    EXPECT_NEAR(reflected.y, -1.0f, 1e-5f);
    EXPECT_NEAR(reflected.z, 3.0f, 1e-5f);
    EXPECT_NEAR(reflected.w, 1.0f, 1e-5f);
}

TEST(PlanarReflectionMathTest, ReflectionMatrixHasNegativeUnitDeterminant)
{
    // A reflection flips handedness — det == -1. The pass relies on this to
    // know it must invert front-face culling when rendering the mirror view.
    const glm::mat4 r = PlanarReflection::MakeReflectionMatrix(kWaterPlane);
    EXPECT_NEAR(glm::determinant(r), -1.0f, 1e-5f);
}

TEST(PlanarReflectionMathTest, ReflectionIsInvolutive)
{
    // Reflecting twice returns the original point.
    const glm::mat4 r = PlanarReflection::MakeReflectionMatrix(kWaterPlane);
    const glm::vec4 p{ -3.0f, 7.5f, 4.0f, 1.0f };
    const glm::vec4 twice = r * (r * p);
    EXPECT_NEAR(twice.x, p.x, 1e-4f);
    EXPECT_NEAR(twice.y, p.y, 1e-4f);
    EXPECT_NEAR(twice.z, p.z, 1e-4f);
}

TEST(PlanarReflectionMathTest, ReflectPointMatchesMatrixAndArbitraryPlane)
{
    // A tilted, unnormalized plane — ReflectPoint must normalize internally and
    // agree with the matrix path.
    const glm::vec4 tilted{ 0.0f, 4.0f, 4.0f, -8.0f }; // unnormalized
    const glm::mat4 r = PlanarReflection::MakeReflectionMatrix(PlanarReflection::NormalizePlane(tilted));

    const glm::vec3 p{ 2.0f, 3.0f, -1.0f };
    const glm::vec3 viaFn = PlanarReflection::ReflectPoint(tilted, p);
    const glm::vec4 viaMat = r * glm::vec4{ p, 1.0f };
    EXPECT_NEAR(viaFn.x, viaMat.x, 1e-4f);
    EXPECT_NEAR(viaFn.y, viaMat.y, 1e-4f);
    EXPECT_NEAR(viaFn.z, viaMat.z, 1e-4f);
}

// --- Plane normalization ---------------------------------------------------

TEST(PlanarReflectionMathTest, NormalizePlaneScalesNormalToUnitLength)
{
    const glm::vec4 n = PlanarReflection::NormalizePlane(glm::vec4{ 0.0f, 2.0f, 0.0f, -4.0f });
    EXPECT_NEAR(n.x, 0.0f, 1e-6f);
    EXPECT_NEAR(n.y, 1.0f, 1e-6f);
    EXPECT_NEAR(n.z, 0.0f, 1e-6f);
    EXPECT_NEAR(n.w, -2.0f, 1e-6f);
}

TEST(PlanarReflectionMathTest, NormalizePlaneFallsBackForDegenerateNormal)
{
    // Zero / near-zero normal — there is nothing to normalize against, and
    // returning it unchanged would still let MakeObliqueProjection divide by a
    // zero dot-product (→ NaN/Inf in the projection matrix). Fall back to the
    // same safe horizontal plane the non-finite branch uses.
    const glm::vec4 degenerate{ 0.0f, 0.0f, 0.0f, 5.0f };
    const glm::vec4 n = PlanarReflection::NormalizePlane(degenerate);
    EXPECT_NEAR(n.x, 0.0f, 1e-6f);
    EXPECT_NEAR(n.y, 1.0f, 1e-6f);
    EXPECT_NEAR(n.z, 0.0f, 1e-6f);
    EXPECT_NEAR(n.w, 0.0f, 1e-6f);
}

// --- Mirror view & reflected camera position -------------------------------

TEST(PlanarReflectionMathTest, MirrorViewEqualsViewTimesReflection)
{
    // Rendering unmodified world geometry with MirrorView must equal viewing the
    // reflected geometry with the real view: mirrorView*p == view*(R*p).
    const glm::mat4 view = MakeView();
    const glm::mat4 mirrorView = PlanarReflection::MakeMirrorView(view, kWaterPlane);
    const glm::mat4 r = PlanarReflection::MakeReflectionMatrix(kWaterPlane);

    for (const glm::vec4 p : { glm::vec4{ 1, 4, -3, 1 }, glm::vec4{ -2, 6, 1, 1 }, glm::vec4{ 5, 2, -8, 1 } })
    {
        const glm::vec4 a = mirrorView * p;
        const glm::vec4 b = view * (r * p);
        EXPECT_NEAR(a.x, b.x, 1e-4f);
        EXPECT_NEAR(a.y, b.y, 1e-4f);
        EXPECT_NEAR(a.z, b.z, 1e-4f);
    }
}

TEST(PlanarReflectionMathTest, MirrorCameraPositionIsReflectedAcrossPlane)
{
    const auto m = PlanarReflection::BuildReflectionMatrices(MakeView(), MakeProjection(), kCameraPos, kWaterPlane);
    // Camera at y=5 over a plane at y=2 reflects to y = 2*2 - 5 = -1.
    EXPECT_NEAR(m.MirrorCameraPosition.x, 0.0f, 1e-4f);
    EXPECT_NEAR(m.MirrorCameraPosition.y, -1.0f, 1e-4f);
    EXPECT_NEAR(m.MirrorCameraPosition.z, 10.0f, 1e-4f);
}

// --- Oblique near-plane clip (Lengyel) -------------------------------------

TEST(PlanarReflectionMathTest, ObliqueClipMapsPlanePointsToNearPlane)
{
    // The defining property: a vertex exactly on the reflection plane projects
    // to clip z = -w (the near plane) under the oblique projection.
    const auto m = PlanarReflection::BuildReflectionMatrices(MakeView(), MakeProjection(), kCameraPos, kWaterPlane);

    for (const glm::vec3 onPlane : { glm::vec3{ 0, kPlaneHeight, -5 }, glm::vec3{ 3, kPlaneHeight, -7 } })
    {
        const glm::vec4 clip = m.ViewProjection * glm::vec4{ onPlane, 1.0f };
        ASSERT_GT(clip.w, 0.0f) << "test point must be in front of the mirror camera";
        EXPECT_NEAR(clip.z, -clip.w, 1e-3f * clip.w) << "on-plane vertex should land on the near plane";
    }
}

TEST(PlanarReflectionMathTest, ObliqueClipKeepsAbovePlaneAndCullsBelow)
{
    // Geometry on the kept side (above water) lands inside the frustum
    // (clip z > -w); geometry on the far side (below water) is clipped beyond
    // the near plane (clip z < -w). Same screen column, straddling the plane.
    const auto m = PlanarReflection::BuildReflectionMatrices(MakeView(), MakeProjection(), kCameraPos, kWaterPlane);

    const glm::vec4 above = m.ViewProjection * glm::vec4{ 0.0f, kPlaneHeight + 1.0f, -5.0f, 1.0f };
    const glm::vec4 below = m.ViewProjection * glm::vec4{ 0.0f, kPlaneHeight - 1.0f, -5.0f, 1.0f };

    ASSERT_GT(above.w, 0.0f);
    ASSERT_GT(below.w, 0.0f);
    EXPECT_GT(above.z, -above.w) << "above-water geometry must be kept";
    EXPECT_LT(below.z, -below.w) << "below-water geometry must be clipped";
}

TEST(PlanarReflectionMathTest, ObliqueProjectionPreservesXAndYProjection)
{
    // Lengyel only rewrites the clip-z row; the x/y projection (and thus where a
    // vertex lands horizontally on screen) must be untouched.
    const glm::mat4 proj = MakeProjection();
    const glm::vec4 viewPlane{ 0.0f, 1.0f, 0.2f, -3.0f };
    const glm::mat4 oblique = PlanarReflection::MakeObliqueProjection(proj, viewPlane);

    EXPECT_FLOAT_EQ(oblique[0][0], proj[0][0]);
    EXPECT_FLOAT_EQ(oblique[1][1], proj[1][1]);
    EXPECT_FLOAT_EQ(oblique[0][1], proj[0][1]);
    EXPECT_FLOAT_EQ(oblique[1][0], proj[1][0]);
    // The w-row (perspective divide) is also preserved.
    EXPECT_FLOAT_EQ(oblique[2][3], proj[2][3]);
    EXPECT_FLOAT_EQ(oblique[3][3], proj[3][3]);
}

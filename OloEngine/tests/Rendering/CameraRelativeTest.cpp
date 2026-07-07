// OLO_TEST_LAYER: L1
//
// CPU contract tests for camera-relative rendering (issue #429). These pin the
// origin-subtraction math independently of any GL context:
//   * ComputeRenderOrigin grid-snapping (no-op in the first cell).
//   * The algebraic identity that makes the transform invisible to shaders:
//         M_rel * (worldPos - origin) == M_world * worldPos
//     for the view, view-projection and model matrices.
//   * The whole point of the exercise — that projecting a far-from-origin vertex
//     through the *relative* path lands far closer to the double-precision truth
//     than the naive all-f32 world path (i.e. it removes the jitter source).
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/CameraRelative.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <type_traits>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

namespace
{
    // Project a local vertex through P*V*M and return NDC (clip.xyz / clip.w).
    template<typename Mat, typename Vec>
    Vec ProjectNdc(const Mat& proj, const Mat& view, const Mat& model, const Vec& localVertex)
    {
        auto clip = proj * view * model * typename Mat::col_type(localVertex, static_cast<typename Vec::value_type>(1));
        return Vec(clip) / clip.w;
    }
} // namespace

// -----------------------------------------------------------------------------
// ComputeRenderOrigin — grid snapping
// -----------------------------------------------------------------------------

TEST(CameraRelative, OriginIsZeroWithinFirstCell)
{
    // Anywhere inside +/- gridSize/2 of the world origin must snap to exactly
    // (0,0,0) so the whole feature is a byte-identical no-op near origin.
    const f32 half = kRenderOriginGridSize * 0.5f - 1.0f;
    EXPECT_EQ(ComputeRenderOrigin({ 0.0f, 0.0f, 0.0f }), glm::vec3(0.0f));
    EXPECT_EQ(ComputeRenderOrigin({ half, -half, half }), glm::vec3(0.0f));
    EXPECT_EQ(ComputeRenderOrigin({ -half, half, -half }), glm::vec3(0.0f));
}

TEST(CameraRelative, OriginSnapsToNearestGridMultiple)
{
    const glm::vec3 origin = ComputeRenderOrigin({ 40003.0f, 97.0f, -39990.0f });
    // Each component is the nearest multiple of the grid size.
    EXPECT_FLOAT_EQ(origin.x, std::round(40003.0f / kRenderOriginGridSize) * kRenderOriginGridSize);
    EXPECT_FLOAT_EQ(origin.y, std::round(97.0f / kRenderOriginGridSize) * kRenderOriginGridSize);
    EXPECT_FLOAT_EQ(origin.z, std::round(-39990.0f / kRenderOriginGridSize) * kRenderOriginGridSize);

    // The camera is guaranteed to be within half a cell of its snapped origin.
    const glm::vec3 camera(40003.0f, 97.0f, -39990.0f);
    EXPECT_LE(glm::length(camera - origin), kRenderOriginGridSize * 0.5f * std::sqrt(3.0f) + 1.0f);
}

TEST(CameraRelative, OriginSnapDisabledReturnsExactPosition)
{
    const glm::vec3 camera(40003.0f, 97.0f, -39990.0f);
    EXPECT_EQ(ComputeRenderOrigin(camera, 0.0f), camera);
}

// -----------------------------------------------------------------------------
// MakeModelRelative — translation shifted, rotation/scale preserved
// -----------------------------------------------------------------------------

TEST(CameraRelative, ModelRelativeShiftsOnlyTranslation)
{
    const glm::vec3 origin(39936.0f, 0.0f, -40960.0f);
    glm::mat4 model = glm::translate(glm::mat4(1.0f), { 40000.0f, 12.0f, -41000.0f });
    model = glm::rotate(model, glm::radians(37.0f), { 0.2f, 0.7f, 0.1f });
    model = glm::scale(model, { 2.0f, 3.0f, 0.5f });

    const glm::mat4 rel = MakeModelRelative(model, origin);

    // Rotation/scale 3x3 is byte-identical.
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            EXPECT_FLOAT_EQ(rel[c][r], model[c][r]) << "col " << c << " row " << r;

    // Translation column reduced by exactly origin.
    EXPECT_FLOAT_EQ(rel[3][0], model[3][0] - origin.x);
    EXPECT_FLOAT_EQ(rel[3][1], model[3][1] - origin.y);
    EXPECT_FLOAT_EQ(rel[3][2], model[3][2] - origin.z);
    EXPECT_FLOAT_EQ(rel[3][3], 1.0f);
}

// -----------------------------------------------------------------------------
// The core invariant: shaders that consume (matrix, worldPos) see no difference
// when both are supplied relative to a common origin.
// -----------------------------------------------------------------------------

TEST(CameraRelative, RelativeViewProjectionIsInvariantForNearbyGeometry)
{
    const glm::vec3 cameraPos(40000.0f, 100.0f, 40000.0f);
    const glm::vec3 target(40005.0f, 100.0f, 40002.0f);
    const glm::vec3 origin = ComputeRenderOrigin(cameraPos);

    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
    const glm::mat4 view = glm::lookAt(cameraPos, target, { 0.0f, 1.0f, 0.0f });
    const glm::mat4 vp = proj * view;

    const glm::mat4 viewRel = MakeViewRelative(view, origin);
    const glm::mat4 vpRel = MakeViewProjectionRelative(vp, origin);

    // A point near the camera. Relative-space point is worldPos - origin.
    const glm::vec3 worldPoint(40004.0f, 101.0f, 40001.0f);
    const glm::vec3 relPoint = MakePositionRelative(worldPoint, origin);

    // viewRel * relPoint == view * worldPoint (within f32).
    const glm::vec4 viewedWorld = view * glm::vec4(worldPoint, 1.0f);
    const glm::vec4 viewedRel = viewRel * glm::vec4(relPoint, 1.0f);
    for (int i = 0; i < 3; ++i)
        EXPECT_NEAR(viewedRel[i], viewedWorld[i], 0.05f) << "view component " << i;

    // Projected clip must agree too.
    const glm::vec4 clipWorld = vp * glm::vec4(worldPoint, 1.0f);
    const glm::vec4 clipRel = vpRel * glm::vec4(relPoint, 1.0f);
    const glm::vec3 ndcWorld = glm::vec3(clipWorld) / clipWorld.w;
    const glm::vec3 ndcRel = glm::vec3(clipRel) / clipRel.w;
    for (int i = 0; i < 3; ++i)
        EXPECT_NEAR(ndcRel[i], ndcWorld[i], 1e-3f) << "ndc component " << i;
}

// -----------------------------------------------------------------------------
// The payoff: fine local geometric detail is what f32 loses far from origin (a
// mesh's sub-ULP vertex offsets collapse — the visible jitter/deformation). The
// *absolute* position at 45 km is ULP-limited either way (the camera's own
// stored f32 position has a ~0.004 ULP there — camera-relative can't beat that),
// but the RELATIVE position of nearby vertices, computed near 0, stays crisp.
// This test pins that: the separation of two nearby vertices survives the
// relative path but is corrupted by the naive world path.
// -----------------------------------------------------------------------------

TEST(CameraRelative, RelativePathPreservesFineDetailFarFromOrigin)
{
    // A mesh ~45 km from the world origin, camera just beside it.
    const glm::dvec3 meshPosD(45000.0, 120.0, -45000.0);
    const glm::dvec3 cameraD(45010.0, 122.0, -45005.0);

    const glm::mat4 modelWorldF = glm::translate(glm::mat4(1.0f), glm::vec3(meshPosD));
    const glm::dmat4 modelWorldD = glm::translate(glm::dmat4(1.0), meshPosD);

    const glm::vec3 origin = ComputeRenderOrigin(glm::vec3(cameraD));
    const glm::mat4 modelRelF = MakeModelRelative(modelWorldF, origin);

    // Two local vertices ~1.4 cm apart — fine detail relative to the 45 km scale.
    const glm::vec3 localA(0.0f, 0.0f, 0.0f);
    const glm::vec3 localB(0.010f, 0.008f, 0.006f);

    auto world = [](const auto& m, const auto& v)
    {
        using T = typename std::remove_cvref_t<decltype(m)>::value_type;
        auto p = m * glm::vec<4, T>(v, static_cast<T>(1));
        return glm::vec<3, T>(p);
    };

    // Ground-truth vertex separation (double precision).
    const double truthSep = glm::length(world(modelWorldD, glm::dvec3(localB)) - world(modelWorldD, glm::dvec3(localA)));

    // Naive world path: world position built in f32 at ~45 km — detail quantized.
    const double worldSep = glm::length(glm::dvec3(world(modelWorldF, localB)) - glm::dvec3(world(modelWorldF, localA)));

    // Camera-relative path: world position built in f32 near 0 — detail preserved.
    const double relSep = glm::length(glm::dvec3(world(modelRelF, localB)) - glm::dvec3(world(modelRelF, localA)));

    const double worldErr = std::abs(worldSep - truthSep);
    const double relErr = std::abs(relSep - truthSep);

    // The relative path reproduces the true separation to a tiny fraction.
    EXPECT_LT(relErr, truthSep * 1e-2) << "relative path should preserve fine detail; sep=" << relSep << " truth=" << truthSep;
    // The naive world path visibly corrupts it — orders of magnitude worse.
    EXPECT_GT(worldErr, relErr * 10.0)
        << "world err=" << worldErr << " rel err=" << relErr << " truth=" << truthSep;
    // And the world-path corruption is a meaningful fraction of the detail itself.
    EXPECT_GT(worldErr, truthSep * 0.05) << "world path should visibly degrade fine detail at 45 km";
}

// -----------------------------------------------------------------------------
// Motion vectors: previous-frame VP and previous-frame model, referenced to the
// same current origin, must reproduce the same NDC — otherwise TAA/motion-blur
// velocity breaks even near origin.
// -----------------------------------------------------------------------------

TEST(CameraRelative, PrevFrameStaysConsistentUnderSameOrigin)
{
    const glm::vec3 cameraPos(38000.0f, 80.0f, 38000.0f);
    const glm::vec3 origin = ComputeRenderOrigin(cameraPos);

    const glm::mat4 proj = glm::perspective(glm::radians(50.0f), 1.6f, 0.1f, 1500.0f);
    const glm::mat4 prevView = glm::lookAt(cameraPos, cameraPos + glm::vec3(1.0f, 0.0f, 0.3f), { 0.0f, 1.0f, 0.0f });
    const glm::mat4 prevVp = proj * prevView;
    const glm::mat4 prevModel = glm::translate(glm::mat4(1.0f), { 38002.0f, 81.0f, 38001.0f });

    const glm::mat4 prevVpRel = MakeViewProjectionRelative(prevVp, origin);
    const glm::mat4 prevModelRel = MakeModelRelative(prevModel, origin);

    const glm::vec3 local(0.25f, -0.1f, 0.4f);
    const glm::vec3 ndcWorld = ProjectNdc(proj, prevView, prevModel, local);
    // Relative: prevVpRel already folds in the projection, so pass identity view.
    const glm::vec4 clipRel = prevVpRel * prevModelRel * glm::vec4(local, 1.0f);
    const glm::vec3 ndcRel = glm::vec3(clipRel) / clipRel.w;

    for (int i = 0; i < 3; ++i)
        EXPECT_NEAR(ndcRel[i], ndcWorld[i], 1e-3f) << "prev ndc component " << i;
}

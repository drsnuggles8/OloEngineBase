// OLO_TEST_LAYER: L1
//
// CPU contract tests for camera-relative rendering in the 2D sprite path
// (issue #429, 2D-sprites slice). Renderer2D bakes world vertices on the CPU
// at Draw* time (transform * QuadVertexPositions[i]) and uploads its own
// view-projection UBO, so — unlike the GPU-upload 3D path — it loses fine
// detail at *bake* time far from origin. The fix mirrors the 3D pattern with a
// per-BeginScene grid-snapped render origin O: bake every vertex relative to O
// (MakeModelRelative / MakePositionRelative) and upload VP made relative to the
// same O (MakeViewProjectionRelative). These tests pin, without a GL context,
// exactly the math the Renderer2D implementation performs:
//   * The ORTHOGRAPHIC projection case (the 3D test only covers perspective).
//   * The invariance identity VP_rel * (worldPos - O) == VP_world * worldPos for
//     both the transform-baked quad corners and the explicit-position paths.
//   * The payoff: a far-from-origin sprite's fine corner detail survives the
//     relative bake but is corrupted by the naive world-space bake.
//   * The debug-lever contract: O == (0,0,0) is a byte-identical no-op, so the
//     relative upload equals the plain world upload near origin / lever-off.
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/CameraRelative.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

namespace
{
    // The unit-quad corners Renderer2D bakes every sprite/quad/circle/rect from
    // (Renderer2D::Init seeds s_Data.QuadVertexPositions to exactly these).
    constexpr glm::vec4 kQuadVertexPositions[4] = {
        { -0.5f, -0.5f, 0.0f, 1.0f },
        { 0.5f, -0.5f, 0.0f, 1.0f },
        { 0.5f, 0.5f, 0.0f, 1.0f },
        { -0.5f, 0.5f, 0.0f, 1.0f }
    };

    // An orthographic view-projection centred on `cameraPos` — the shape
    // Renderer2D uploads for a 2D scene (OrthographicCamera / the runtime 2D
    // camera). Half-extents pick a modest world view so far-origin coordinates
    // stay well inside f32 range.
    glm::mat4 OrthoViewProjection(const glm::vec3& cameraPos)
    {
        const glm::mat4 proj = glm::ortho(-10.0f, 10.0f, -10.0f, 10.0f, -1.0f, 1.0f);
        const glm::mat4 view = glm::translate(glm::mat4(1.0f), -cameraPos);
        return proj * view;
    }
} // namespace

// -----------------------------------------------------------------------------
// No-op near origin / lever off: the relative upload is byte-identical to the
// plain world upload when O == (0,0,0). This is the safety gate — every
// existing near-origin 2D scene renders exactly as before.
// -----------------------------------------------------------------------------

TEST(Renderer2DCameraRelative, ZeroOriginUploadIsByteIdenticalToWorld)
{
    const glm::vec3 cameraPos(5.0f, -3.0f, 0.0f); // within the first grid cell
    const glm::vec3 origin = ComputeRenderOrigin(cameraPos);
    ASSERT_EQ(origin, glm::vec3(0.0f));

    const glm::mat4 vpWorld = OrthoViewProjection(cameraPos);
    const glm::mat4 vpRel = MakeViewProjectionRelative(vpWorld, origin);

    // Byte-identical: MakeViewProjectionRelative multiplies the origin (0,0,0,1)
    // through, which reproduces the world matrix's translation column exactly.
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            EXPECT_FLOAT_EQ(vpRel[c][r], vpWorld[c][r]) << "col " << c << " row " << r;

    // And a transform baked relative to O==0 equals the plain world bake.
    const glm::mat4 model = glm::translate(glm::mat4(1.0f), { 4.0f, 2.0f, 0.0f });
    const glm::mat4 modelRel = MakeModelRelative(model, origin);
    for (const glm::vec4& corner : kQuadVertexPositions)
    {
        const glm::vec4 world = model * corner;
        const glm::vec4 rel = modelRel * corner;
        for (int i = 0; i < 4; ++i)
            EXPECT_FLOAT_EQ(rel[i], world[i]);
    }
}

// -----------------------------------------------------------------------------
// Invariance: a sprite baked relative and projected through the relative ortho
// VP lands at the same clip position as the naive world path — the shift is
// invisible on screen. Covers the transform-baked quad corners.
// -----------------------------------------------------------------------------

TEST(Renderer2DCameraRelative, BakedQuadCornersProjectIdenticallyFarFromOrigin)
{
    const glm::vec3 cameraPos(45000.0f, -45000.0f, 0.0f);
    const glm::vec3 origin = ComputeRenderOrigin(cameraPos);
    ASSERT_NE(origin, glm::vec3(0.0f)); // well outside the first cell

    // A sprite a few units from the camera, as a 2D scene would place it.
    const glm::mat4 model = glm::translate(glm::mat4(1.0f), { 45003.0f, -44998.0f, 0.0f }) *
                            glm::scale(glm::mat4(1.0f), { 2.0f, 1.5f, 1.0f });

    const glm::mat4 vpWorld = OrthoViewProjection(cameraPos);
    const glm::mat4 vpRel = MakeViewProjectionRelative(vpWorld, origin);
    const glm::mat4 modelRel = MakeModelRelative(model, origin);

    for (const glm::vec4& corner : kQuadVertexPositions)
    {
        // Naive world path: bake world (in f32) then project through world VP.
        const glm::vec4 clipWorld = vpWorld * (model * corner);
        // Relative path (what Renderer2D now does): bake relative, project rel.
        const glm::vec4 clipRel = vpRel * (modelRel * corner);

        for (int i = 0; i < 4; ++i)
            EXPECT_NEAR(clipRel[i], clipWorld[i], 1e-2f) << "clip component " << i;
    }
}

// -----------------------------------------------------------------------------
// Invariance for the explicit-position paths (DrawLine / DrawPolygon /
// DrawQuadVertices): a world position shifted by -O and projected through the
// relative VP matches the world path.
// -----------------------------------------------------------------------------

TEST(Renderer2DCameraRelative, ExplicitWorldPositionsProjectIdenticallyFarFromOrigin)
{
    const glm::vec3 cameraPos(-38000.0f, 38000.0f, 0.0f);
    const glm::vec3 origin = ComputeRenderOrigin(cameraPos);

    const glm::mat4 vpWorld = OrthoViewProjection(cameraPos);
    const glm::mat4 vpRel = MakeViewProjectionRelative(vpWorld, origin);

    const glm::vec3 worldPoints[] = {
        { -38004.0f, 38002.0f, 0.0f },
        { -37997.0f, 38005.0f, 0.0f },
        { -38001.0f, 37996.0f, 0.0f }
    };

    for (const glm::vec3& p : worldPoints)
    {
        const glm::vec3 rel = MakePositionRelative(p, origin);
        const glm::vec4 clipWorld = vpWorld * glm::vec4(p, 1.0f);
        const glm::vec4 clipRel = vpRel * glm::vec4(rel, 1.0f);
        for (int i = 0; i < 4; ++i)
            EXPECT_NEAR(clipRel[i], clipWorld[i], 1e-2f) << "clip component " << i;
    }
}

// -----------------------------------------------------------------------------
// The payoff (the whole reason for the slice): a sprite ~45 km from the origin
// keeps its fine corner geometry through the relative bake, but the naive world
// bake — transform * corner evaluated in f32 at 45 km — quantizes it. Measured
// as the diagonal length of a small sprite: at 45 km an f32 ULP (~0.004) is a
// meaningful fraction of a sub-unit sprite's corner offsets.
// -----------------------------------------------------------------------------

TEST(Renderer2DCameraRelative, RelativeBakePreservesSpriteDetailFarFromOrigin)
{
    // A small sprite (2 cm) far from origin — the scale at which f32 bake error
    // at 45 km becomes a large relative error.
    const glm::dvec3 spritePosD(45000.0, -45000.0, 0.0);
    const glm::vec3 cameraPos(45001.0f, -45001.0f, 0.0f);
    const glm::vec3 origin = ComputeRenderOrigin(cameraPos);

    const glm::mat4 modelWorldF = glm::translate(glm::mat4(1.0f), glm::vec3(spritePosD)) *
                                  glm::scale(glm::mat4(1.0f), { 0.02f, 0.02f, 1.0f });
    const glm::dmat4 modelWorldD = glm::translate(glm::dmat4(1.0), spritePosD) *
                                   glm::scale(glm::dmat4(1.0), { 0.02, 0.02, 1.0 });
    const glm::mat4 modelRelF = MakeModelRelative(modelWorldF, origin);

    auto diagonal = [](const auto& m) -> double
    {
        using T = typename std::remove_cvref_t<decltype(m)>::value_type;
        const auto bl = m * glm::vec<4, T>(static_cast<T>(-0.5), static_cast<T>(-0.5), static_cast<T>(0), static_cast<T>(1));
        const auto tr = m * glm::vec<4, T>(static_cast<T>(0.5), static_cast<T>(0.5), static_cast<T>(0), static_cast<T>(1));
        return glm::length(glm::dvec2(glm::dvec4(tr) - glm::dvec4(bl)));
    };

    const double truthDiag = diagonal(modelWorldD);
    const double worldDiag = diagonal(modelWorldF);
    const double relDiag = diagonal(modelRelF);

    const double worldErr = std::abs(worldDiag - truthDiag);
    const double relErr = std::abs(relDiag - truthDiag);

    // Relative bake reproduces the true sprite diagonal to a tiny fraction.
    EXPECT_LT(relErr, truthDiag * 1e-3)
        << "relative bake should preserve sprite detail; diag=" << relDiag << " truth=" << truthDiag;
    // The naive world bake corrupts it by orders of magnitude more.
    EXPECT_GT(worldErr, relErr * 10.0)
        << "world err=" << worldErr << " rel err=" << relErr << " truth=" << truthDiag;
    // And that corruption is a meaningful fraction of the sprite itself.
    EXPECT_GT(worldErr, truthDiag * 0.02)
        << "world bake should visibly degrade a small sprite's geometry at 45 km";
}

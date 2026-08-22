// OLO_TEST_LAYER: cullinglod
// =============================================================================
// ObserverCameraTest.cpp  (#726)
//
// CPU-side contract for the observer camera: the freeze/mirror state machine and
// the frustum-corner unprojection the wireframe is drawn from.
//
// Why this layer and not only a visual test: the observer camera's whole value
// is being a GROUND TRUTH other culling work is checked against, and its
// characteristic failure is showing a plausible-but-wrong cut. A screenshot
// cannot tell "frozen at pose A" from "frozen at pose A-and-a-bit" — but the
// mirror contract can be stated exactly, so it is stated here. The full-pipeline
// half (does the frame actually keep the frozen cut when you fly away?) lives in
// PropertyTests/ObserverCameraVisualEvidenceTest.cpp.
//
// No GL context needed: every entry point exercised here is pure state or pure
// matrix maths on Renderer3D's static data.
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Renderer/Renderer3D.h"

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>

namespace OloEngine::Tests
{
    namespace
    {
        // Renderer3D's camera state is process-wide, so every test here restores
        // what it found. Without this a failing EXPECT would leave the culling
        // camera frozen for every later test in the same binary — and a frozen
        // culling camera is exactly the kind of state whose damage shows up
        // somewhere else entirely.
        struct CullingCameraRestore
        {
            bool m_Frozen = Renderer3D::IsCullingCameraFrozen();
            glm::vec3 m_ViewPos = Renderer3D::GetCullViewPosition();
            f32 m_Near = Renderer3D::GetCullNearClip();
            f32 m_Far = Renderer3D::GetCullFarClip();

            ~CullingCameraRestore()
            {
                Renderer3D::SetCullingCameraFrozen(false);
                Renderer3D::SetViewPosition(m_ViewPos);
                Renderer3D::SetCameraClipPlanes(m_Near, m_Far);
                Renderer3D::SetCullingCameraFrozen(m_Frozen);
            }
        };

        constexpr f32 kEps = 1e-4f;

        void ExpectVec3Near(const glm::vec3& actual, const glm::vec3& expected, f32 eps = kEps)
        {
            EXPECT_NEAR(actual.x, expected.x, eps);
            EXPECT_NEAR(actual.y, expected.y, eps);
            EXPECT_NEAR(actual.z, expected.z, eps);
        }
    } // namespace

    // -------------------------------------------------------------------------
    // The mirror contract
    // -------------------------------------------------------------------------

    TEST(ObserverCamera, CullingCameraFollowsTheRenderCameraWhileUnfrozen)
    {
        const CullingCameraRestore restore;

        Renderer3D::SetCullingCameraFrozen(false);
        Renderer3D::SetViewPosition({ 1.0f, 2.0f, 3.0f });
        Renderer3D::SetCameraClipPlanes(0.25f, 500.0f);

        ExpectVec3Near(Renderer3D::GetCullViewPosition(), { 1.0f, 2.0f, 3.0f });
        EXPECT_NEAR(Renderer3D::GetCullNearClip(), 0.25f, kEps);
        EXPECT_NEAR(Renderer3D::GetCullFarClip(), 500.0f, kEps);
    }

    TEST(ObserverCamera, FreezingPinsTheCullingCameraWhileTheRenderCameraMovesOn)
    {
        const CullingCameraRestore restore;

        Renderer3D::SetCullingCameraFrozen(false);
        Renderer3D::SetViewPosition({ 10.0f, 0.0f, 0.0f });
        Renderer3D::SetCameraClipPlanes(0.1f, 100.0f);

        Renderer3D::SetCullingCameraFrozen(true);
        ASSERT_TRUE(Renderer3D::IsCullingCameraFrozen());

        // Fly the observer somewhere completely different.
        Renderer3D::SetViewPosition({ -400.0f, 250.0f, 900.0f });
        Renderer3D::SetCameraClipPlanes(5.0f, 20000.0f);

        // The culling camera must not have noticed. This is the property the
        // whole feature rests on: the cut belongs to the frozen pose, not to
        // wherever the viewport has since wandered.
        ExpectVec3Near(Renderer3D::GetCullViewPosition(), { 10.0f, 0.0f, 0.0f });
        EXPECT_NEAR(Renderer3D::GetCullNearClip(), 0.1f, kEps);
        EXPECT_NEAR(Renderer3D::GetCullFarClip(), 100.0f, kEps);
    }

    TEST(ObserverCamera, UnfreezingReattachesImmediatelyWithoutWaitingForAFrame)
    {
        const CullingCameraRestore restore;

        Renderer3D::SetCullingCameraFrozen(false);
        Renderer3D::SetViewPosition({ 10.0f, 0.0f, 0.0f });
        Renderer3D::SetCullingCameraFrozen(true);
        Renderer3D::SetViewPosition({ -3.0f, 7.0f, 11.0f });

        Renderer3D::SetCullingCameraFrozen(false);

        // Not "next BeginScene": an MCP query or an editor panel can read the
        // culling camera between the unfreeze and the next frame, and a stale
        // frozen answer there reads as "unfreeze didn't work".
        EXPECT_FALSE(Renderer3D::IsCullingCameraFrozen());
        ExpectVec3Near(Renderer3D::GetCullViewPosition(), { -3.0f, 7.0f, 11.0f });
    }

    TEST(ObserverCamera, FreezeStateAndTheRendererSettingsBoolCannotDrift)
    {
        const CullingCameraRestore restore;

        // Three UIs write RendererSettings::ObserverCameraEnabled (the settings
        // panel, the F3 overlay, MCP) while Renderer3D owns the actual freeze.
        // If the two ever disagree, a checkbox lies about what the renderer is
        // doing — so the setter writes the bool back.
        Renderer3D::SetCullingCameraFrozen(true);
        EXPECT_TRUE(Renderer3D::GetRendererSettings().ObserverCameraEnabled);

        Renderer3D::SetCullingCameraFrozen(false);
        EXPECT_FALSE(Renderer3D::GetRendererSettings().ObserverCameraEnabled);
    }

    // -------------------------------------------------------------------------
    // Frustum corners — what the wireframe is drawn from
    // -------------------------------------------------------------------------

    TEST(ObserverCamera, FrustumCornersRoundTripBackToTheNDCCube)
    {
        // The corners are produced by unprojecting the NDC cube, so projecting
        // them back must land on it again. This is the cheapest statement of
        // "the wireframe is the frustum that culled", independent of whether the
        // projection was perspective, orthographic or off-centre.
        const glm::mat4 proj = glm::perspective(glm::radians(55.0f), 16.0f / 9.0f, 0.3f, 250.0f);
        const glm::mat4 view = glm::lookAt(glm::vec3(4.0f, 9.0f, -12.0f), glm::vec3(0.0f), glm::vec3(0, 1, 0));
        const glm::mat4 viewProjection = proj * view;

        const std::array<glm::vec3, 8> corners = Renderer3D::ComputeFrustumCorners(viewProjection);

        for (u32 i = 0; i < 8; ++i)
        {
            const glm::vec4 clip = viewProjection * glm::vec4(corners[i], 1.0f);
            ASSERT_GT(std::abs(clip.w), 1e-6f) << "corner " << i << " projected to a degenerate w";
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;

            // Corner index convention: bit0 = +x, bit1 = +y, bit2 = +z. The
            // debug-draw expansion recovers the 12 edges as "pairs differing in
            // exactly one bit", so getting this order wrong draws a tangle
            // rather than a frustum.
            EXPECT_NEAR(ndc.x, (i & 1u) ? 1.0f : -1.0f, 1e-3f) << "corner " << i;
            EXPECT_NEAR(ndc.y, (i & 2u) ? 1.0f : -1.0f, 1e-3f) << "corner " << i;
            EXPECT_NEAR(ndc.z, (i & 4u) ? 1.0f : -1.0f, 1e-3f) << "corner " << i;
        }
    }

    TEST(ObserverCamera, FrustumCornersMatchTheAnalyticPerspectiveFrustum)
    {
        // A camera at the origin looking down -Z, so the analytic corners are
        // writable by hand: half-height = tan(fov/2) * distance.
        constexpr f32 kFovDeg = 60.0f;
        constexpr f32 kAspect = 2.0f;
        constexpr f32 kNear = 1.0f;
        constexpr f32 kFar = 50.0f;

        const glm::mat4 viewProjection =
            glm::perspective(glm::radians(kFovDeg), kAspect, kNear, kFar) *
            glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0, 1, 0));

        const std::array<glm::vec3, 8> corners = Renderer3D::ComputeFrustumCorners(viewProjection);

        const f32 tanHalf = std::tan(glm::radians(kFovDeg) * 0.5f);
        const f32 nearHalfH = tanHalf * kNear;
        const f32 farHalfH = tanHalf * kFar;

        // index 0 = (-x, -y, near) ... index 7 = (+x, +y, far)
        ExpectVec3Near(corners[0], { -nearHalfH * kAspect, -nearHalfH, -kNear }, 1e-3f);
        ExpectVec3Near(corners[3], { nearHalfH * kAspect, nearHalfH, -kNear }, 1e-3f);
        ExpectVec3Near(corners[4], { -farHalfH * kAspect, -farHalfH, -kFar }, 1e-2f);
        ExpectVec3Near(corners[7], { farHalfH * kAspect, farHalfH, -kFar }, 1e-2f);
    }

    TEST(ObserverCamera, FrustumCornersSurviveADegenerateViewProjection)
    {
        // A zeroed / uninitialised projection reaches here on the first frame of
        // a headless or not-yet-sized renderer. An inf corner would blow the
        // debug-draw quad expansion up to cover the whole viewport, which reads
        // as "the tool is broken" rather than "there is nothing to draw".
        const std::array<glm::vec3, 8> corners = Renderer3D::ComputeFrustumCorners(glm::mat4(0.0f));
        for (const glm::vec3& corner : corners)
        {
            EXPECT_TRUE(std::isfinite(corner.x));
            EXPECT_TRUE(std::isfinite(corner.y));
            EXPECT_TRUE(std::isfinite(corner.z));
        }
    }

    // -------------------------------------------------------------------------
    // The property the feature exists for, stated at the Frustum level
    // -------------------------------------------------------------------------

    TEST(ObserverCamera, FrozenFrustumStillRejectsWhatTheObserverCanSee)
    {
        // The diagnostic only works if the two frustums genuinely disagree: an
        // object outside the FROZEN frustum but inside the OBSERVER's is what
        // shows up as a visible hole once you fly away. If a change ever made
        // culling follow the render camera again, this is the shape of the
        // property that breaks.
        const glm::mat4 proj = glm::perspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);

        const Frustum frozen(proj * glm::lookAt(glm::vec3(0.0f, 0.0f, 10.0f),
                                                glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0, 1, 0)));
        // Observer pulled back and off to the side, so it sees a much wider slice
        // of the world than the frozen camera ever did.
        const Frustum observer(proj * glm::lookAt(glm::vec3(60.0f, 20.0f, 60.0f),
                                                  glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0, 1, 0)));

        // Well off to the frozen camera's left, still in front of the observer.
        const BoundingSphere offToTheSide{ glm::vec3(-14.0f, 0.0f, 0.0f), 1.0f };
        EXPECT_FALSE(frozen.IsBoundingSphereVisible(offToTheSide))
            << "the frozen frustum should have culled this - the test scene no longer proves anything";
        EXPECT_TRUE(observer.IsBoundingSphereVisible(offToTheSide))
            << "the observer should see it, otherwise flying away shows nothing new";

        // And the thing both agree on stays drawn, so a hole is attributable.
        const BoundingSphere atTheOrigin{ glm::vec3(0.0f), 1.0f };
        EXPECT_TRUE(frozen.IsBoundingSphereVisible(atTheOrigin));
        EXPECT_TRUE(observer.IsBoundingSphereVisible(atTheOrigin));
    }
} // namespace OloEngine::Tests

// OLO_TEST_LAYER: L1
//
// CPU contract tests for EditorCamera posing (issue #931).
//
// WHAT WENT WRONG. SetPosition / SetYaw / SetPitch / SetDistance only stashed
// their member and did NOT rebuild the view matrix. Every caller that does not
// also drive OnUpdate — the editor's default 3D pose, the preferences panel's
// camera bookmarks, and any offline capture that poses a camera and renders —
// therefore got the camera it was already looking through. A visual-evidence
// test posed that way rendered its entire set from the constructor's default
// orbit view: several "different" camera angles, byte-identical frames, sky and
// water present and the subject nowhere, and no error anywhere. Worse,
// SetPosition could never work as written even with a rebuild, because
// UpdateView re-derives the eye from the orbit focal point and distance — the
// assignment to m_Position is overwritten by the very next call.
//
// These tests are the cheap half of the guard. They pin the poses at the matrix
// level with no GL context; the rendered half — that two different requested
// poses actually produce two different PICTURES — lives in
// Rendering/PropertyTests/MeshVisibilityEvidenceTest.cpp.

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"

#include <gtest/gtest.h>

#include <glm/gtc/epsilon.hpp>

#include <cmath>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr f32 kEps = 1e-4f;

        [[nodiscard]] EditorCamera MakeCamera()
        {
            EditorCamera camera(60.0f, 16.0f / 9.0f, 0.1f, 1000.0f);
            camera.SetViewportSize(1280.0f, 720.0f);
            return camera;
        }

        void ExpectVec3Near(const glm::vec3& actual, const glm::vec3& expected, const char* what)
        {
            EXPECT_NEAR(actual.x, expected.x, kEps) << what << " (x)";
            EXPECT_NEAR(actual.y, expected.y, kEps) << what << " (y)";
            EXPECT_NEAR(actual.z, expected.z, kEps) << what << " (z)";
        }

        // Max absolute element difference between two view matrices. Zero means
        // the two poses render the SAME picture — which is the failure mode
        // this file exists to make loud.
        [[nodiscard]] f32 MaxElementDelta(const glm::mat4& a, const glm::mat4& b)
        {
            f32 worst = 0.0f;
            for (int col = 0; col < 4; ++col)
            {
                for (int row = 0; row < 4; ++row)
                {
                    worst = std::max(worst, std::abs(a[col][row] - b[col][row]));
                }
            }
            return worst;
        }
    } // namespace

    // --- The pose calls -----------------------------------------------------

    TEST(EditorCameraPoseTest, SetPosePlacesTheEyeExactlyWhereAsked)
    {
        EditorCamera camera = MakeCamera();
        constexpr glm::vec3 eye{ 3.0f, 4.2f, -8.0f };
        camera.SetPose(eye, 0.7f, 0.25f);

        ExpectVec3Near(camera.GetPosition(), eye, "SetPose eye");
        EXPECT_NEAR(camera.GetYaw(), 0.7f, kEps);
        EXPECT_NEAR(camera.GetPitch(), 0.25f, kEps);
        // The view matrix must agree with the eye: transforming the eye into
        // view space puts it at the origin.
        const glm::vec3 eyeInView = glm::vec3(camera.GetViewMatrix() * glm::vec4(eye, 1.0f));
        ExpectVec3Near(eyeInView, glm::vec3(0.0f), "eye in view space");
    }

    TEST(EditorCameraPoseTest, PositivePitchLooksDown)
    {
        EditorCamera camera = MakeCamera();
        camera.SetPose(glm::vec3(0.0f, 10.0f, 0.0f), 0.0f, 0.5f);
        EXPECT_LT(camera.GetForwardDirection().y, 0.0f)
            << "positive pitch must tilt the view DOWN — the editor's default pose "
               "and every capture pose depend on this sign";
    }

    TEST(EditorCameraPoseTest, SetPoseWithOrbitDistanceKeepsTheEyeAndPivotsAhead)
    {
        EditorCamera camera = MakeCamera();
        constexpr glm::vec3 eye{ -2.0f, 6.0f, 12.0f };
        constexpr f32 orbitDistance = 15.0f;
        camera.SetPose(eye, -0.3f, 0.2f, orbitDistance);

        ExpectVec3Near(camera.GetPosition(), eye, "SetPose(4) eye");
        EXPECT_NEAR(camera.GetDistance(), orbitDistance, kEps);
        // The pivot sits `orbitDistance` straight ahead of the eye, so a later
        // orbit turns about the thing being looked at rather than the eye.
        ExpectVec3Near(camera.GetFocalPoint(), eye + camera.GetForwardDirection() * orbitDistance,
                       "SetPose(4) focal point");
    }

    // --- The #931 regression: the stashing setters ---------------------------

    TEST(EditorCameraPoseTest, SetYawRebuildsTheView)
    {
        EditorCamera camera = MakeCamera();
        const glm::mat4 before = camera.GetViewMatrix();
        camera.SetYaw(1.1f);
        EXPECT_GT(MaxElementDelta(camera.GetViewMatrix(), before), kEps)
            << "SetYaw left the view matrix untouched — the #931 silent no-op";
    }

    TEST(EditorCameraPoseTest, SetPitchRebuildsTheView)
    {
        EditorCamera camera = MakeCamera();
        const glm::mat4 before = camera.GetViewMatrix();
        camera.SetPitch(0.6f);
        EXPECT_GT(MaxElementDelta(camera.GetViewMatrix(), before), kEps)
            << "SetPitch left the view matrix untouched — the #931 silent no-op";
    }

    TEST(EditorCameraPoseTest, SetDistanceRebuildsTheView)
    {
        EditorCamera camera = MakeCamera();
        const glm::mat4 before = camera.GetViewMatrix();
        camera.SetDistance(40.0f);
        EXPECT_GT(MaxElementDelta(camera.GetViewMatrix(), before), kEps)
            << "SetDistance left the view matrix untouched — the #931 silent no-op";
    }

    TEST(EditorCameraPoseTest, SetPositionMovesTheEyeAndSurvivesTheNextRebuild)
    {
        EditorCamera camera = MakeCamera();
        constexpr glm::vec3 eye{ 0.0f, 5.0f, 10.0f };
        camera.SetPosition(eye);
        ExpectVec3Near(camera.GetPosition(), eye, "SetPosition eye");
        // The VIEW matrix, not just the reported eye: the original bug assigned
        // m_Position and stopped, so an implementation that stores the eye and
        // skips its own UpdateView would satisfy the line above while still
        // rendering from the previous pose. Transforming the eye by the view has
        // to land on the origin.
        ExpectVec3Near(glm::vec3(camera.GetViewMatrix() * glm::vec4(eye, 1.0f)), glm::vec3(0.0f),
                       "SetPosition eye in view space");

        // The trap that made the old implementation unfixable by simply adding a
        // rebuild: UpdateView re-derives the eye from the focal point, so an
        // assignment to m_Position is discarded by any later view rebuild. Force
        // one and check the eye is still where it was put.
        camera.SetYaw(camera.GetYaw());
        ExpectVec3Near(camera.GetPosition(), eye, "SetPosition eye after a later rebuild");
    }

    // --- The thing the blank captures should have failed on ------------------

    TEST(EditorCameraPoseTest, DifferentRequestedPosesProduceDifferentViews)
    {
        // Three poses of the kind a multi-angle screenshot test asks for. Before
        // #931 all three rendered the same picture, and the only symptom was a
        // set of identical PNGs that no assertion looked at.
        EditorCamera a = MakeCamera();
        a.SetPose(glm::vec3(0.0f, 4.2f, 6.0f), 0.0f, 0.06f);
        EditorCamera b = MakeCamera();
        b.SetPose(glm::vec3(9.0f, 3.0f, 4.0f), 0.9f, 0.10f);
        EditorCamera c = MakeCamera();
        c.SetPose(glm::vec3(0.0f, 12.0f, 14.0f), 0.0f, 0.55f);

        EXPECT_GT(MaxElementDelta(a.GetViewMatrix(), b.GetViewMatrix()), kEps);
        EXPECT_GT(MaxElementDelta(a.GetViewMatrix(), c.GetViewMatrix()), kEps);
        EXPECT_GT(MaxElementDelta(b.GetViewMatrix(), c.GetViewMatrix()), kEps);
    }

    TEST(EditorCameraPoseTest, CameraBookmarkRoundTripsExactly)
    {
        // What EditorPreferencesPanel stores and restores: eye position, yaw,
        // pitch, orbit distance. Restoring must reproduce the same view.
        EditorCamera source = MakeCamera();
        source.SetPose(glm::vec3(12.0f, 7.5f, -3.25f), 2.1f, -0.35f, 9.0f);

        const glm::vec3 savedPosition = source.GetPosition();
        const f32 savedYaw = source.GetYaw();
        const f32 savedPitch = source.GetPitch();
        const f32 savedDistance = source.GetDistance();

        EditorCamera restored = MakeCamera();
        restored.SetPose(savedPosition, savedYaw, savedPitch, savedDistance);

        ExpectVec3Near(restored.GetPosition(), savedPosition, "restored eye");
        EXPECT_LT(MaxElementDelta(restored.GetViewMatrix(), source.GetViewMatrix()), kEps)
            << "a restored camera bookmark must reproduce the saved view";
    }
} // namespace OloEngine::Tests

// OLO_TEST_LAYER: Functional
#include "OloEnginePCH.h"

// =============================================================================
// ParentedCameraFollowsItsParentTest — Functional Test.
//
// Cross-subsystem seam under test:
//   the transform hierarchy x Scene::RenderRuntime's primary camera. A camera
//   parented to a character -- the dog scene's runtime camera rides the dog as
//   it walks (#1533) -- must render from its WORLD pose. RenderRuntime used the
//   camera's local transform, so a parented camera drew from its
//   parent-relative offset as if it were a world position and never followed.
//
// Scenario: a parent at (5, 0, 0), a child camera 10 m behind it on +Z looking
// down -Z. After a tick the view-projection must be the one through the
// camera's world pose (15 m from where the local alone would put it), and the
// parent itself must land at the centre of the screen. Then the parent moves
// and the view must move with it.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

using namespace OloEngine;
using namespace OloEngine::Functional;

class ParentedCameraFollowsItsParentTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        m_Parent = GetScene().CreateEntity("Rider");
        m_Parent.GetComponent<TransformComponent>().Translation = glm::vec3(5.0f, 0.0f, 0.0f);

        m_Camera = GetScene().CreateEntity("RidingCamera");
        m_Camera.GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, 0.0f, 10.0f);
        auto& camera = m_Camera.AddComponent<CameraComponent>();
        camera.Primary = true;
        camera.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);
        camera.Camera.SetPerspective(glm::radians(45.0f), 0.1f, 100.0f);
        camera.Camera.SetViewportSize(1280u, 720u);
        m_Camera.SetParent(m_Parent);
    }

    // Where `world` lands in normalized device coordinates through the last
    // runtime render's view-projection.
    [[nodiscard]] glm::vec3 Ndc(const glm::vec3& world) const
    {
        const glm::vec4 clip = GetScene().GetCameraViewProjection() * glm::vec4(world, 1.0f);
        return glm::vec3(clip) / clip.w;
    }

    Entity m_Parent;
    Entity m_Camera;
};

TEST_F(ParentedCameraFollowsItsParentTest, TheViewIsTheCamerasWorldPoseAndMovesWithTheParent)
{
    RunFrames(1);

    const glm::mat4 expected = m_Camera.GetComponent<CameraComponent>().Camera.GetProjection() *
                               glm::inverse(glm::translate(glm::mat4(1.0f), glm::vec3(5.0f, 0.0f, 10.0f)));
    const glm::mat4& actual = GetScene().GetCameraViewProjection();
    for (int c = 0; c < 4; ++c)
    {
        for (int r = 0; r < 4; ++r)
        {
            EXPECT_NEAR(actual[c][r], expected[c][r], 1e-4f) << "view-projection element [" << c << "][" << r << "]";
        }
    }
    const glm::vec3 centred = Ndc(glm::vec3(5.0f, 0.0f, 0.0f));
    EXPECT_NEAR(centred.x, 0.0f, 1e-4f) << "the parent is off the screen's centre: the camera drew from its local offset";
    EXPECT_NEAR(centred.y, 0.0f, 1e-4f);

    m_Parent.GetComponent<TransformComponent>().Translation = glm::vec3(5.0f, 0.0f, -3.0f);
    RunFrames(1);
    const glm::vec3 followed = Ndc(glm::vec3(5.0f, 0.0f, -3.0f));
    EXPECT_NEAR(followed.x, 0.0f, 1e-4f) << "the camera did not follow its parent";
    EXPECT_NEAR(followed.y, 0.0f, 1e-4f);
}

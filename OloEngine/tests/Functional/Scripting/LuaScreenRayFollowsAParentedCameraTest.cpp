#include "OloEnginePCH.h"
#include "TestTempDir.h"

// OLO_TEST_LAYER: Functional

// =============================================================================
// LuaScreenRayFollowsAParentedCameraTest — Functional Test (found on #1559).
//
// Seam: Lua Camera.ScreenToWorldRay × Scene::ScreenToWorldRay × the camera's
// composed world transform × the scene viewport.
//
// Both the Lua and the C# screen ray were built from the camera's LOCAL
// transform, so a camera parented under a character (the dog scenes' runtime
// camera rides the dog) cast every ray from beside the world origin; and Lua
// normalised the cursor by the host WINDOW instead of the viewport the cursor is
// measured in, so in the editor a click landed off by the panel's share of the
// window. One implementation (Scene::ScreenToWorldRay) now serves both languages.
// =============================================================================

#include "Functional/FunctionalTest.h"

#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <filesystem>
#include <fstream>

using namespace OloEngine;
using namespace OloEngine::Functional;

class LuaScreenRayFollowsAParentedCameraTest : public FunctionalTest
{
  protected:
    void BuildScene() override
    {
        Entity parent = GetScene().CreateEntity("CameraParent");
        parent.GetComponent<TransformComponent>().Translation = { 10.0f, 2.0f, 0.0f };
        m_Camera = GetScene().CreateEntityWithUUID(UUID{ 4101 }, "Camera");
        m_Camera.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 5.0f };
        m_Camera.AddComponent<CameraComponent>().Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);
        m_Camera.SetParent(parent);
        m_Observer = GetScene().CreateEntity("Observer");
        EnableLua();
        GetScene().OnViewportResize(800, 600);
    }

    void TearDown() override
    {
        FunctionalTest::TearDown();
        std::error_code ec;
        std::filesystem::remove(m_Script, ec);
    }

    Entity m_Camera;
    Entity m_Observer;
    std::filesystem::path m_Script;
};

TEST_F(LuaScreenRayFollowsAParentedCameraTest, TheCentreRayStartsAtTheCamerasWorldPosition)
{
    m_Script = OloEngine::Tests::TempFile("olo_functional_screen_ray.lua");
    std::ofstream(m_Script, std::ios::binary | std::ios::trunc) << R"(
local s = {}
function s.OnUpdate(id, ts)
    local ray = Camera.ScreenToWorldRay(4101, vec2.new(400, 300))
    if ray ~= nil then entity_utils.set_translation(id, ray.origin) end
end
return s
)";
    RegisterLuaScript(m_Observer, m_Script);
    RunFrames(2);

    // World position: parent (10, 2, 0) + local (0, 0, 5); the centre ray
    // starts on the near plane just in front of it, looking down -Z.
    const glm::vec3 origin = m_Observer.GetComponent<TransformComponent>().Translation;
    EXPECT_NEAR(origin.x, 10.0f, 1e-2f) << "the ray was cast from the camera's LOCAL transform";
    EXPECT_NEAR(origin.y, 2.0f, 1e-2f);
    EXPECT_LT(origin.z, 5.0f);
    EXPECT_GT(origin.z, 4.0f);

    // The pixel (400, 300) of an 800x600 viewport is its centre: the shared ray
    // through uv (0.5, 0.5) is the same ray.
    glm::vec3 apiOrigin;
    glm::vec3 apiDirection;
    ASSERT_TRUE(GetScene().ScreenToWorldRay(m_Camera, { 0.5f, 0.5f }, apiOrigin, apiDirection));
    EXPECT_NEAR(apiDirection.z, -1.0f, 1e-3f);
    EXPECT_NEAR(glm::length(apiOrigin - origin), 0.0f, 1e-3f) << "Lua does not normalise the cursor by the viewport";
}

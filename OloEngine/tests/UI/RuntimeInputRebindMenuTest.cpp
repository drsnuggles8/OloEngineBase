// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// RuntimeInputRebindMenuTest — drives the in-game rebind panel through the real
// ECS UI systems (UILayoutSystem + UIInputSystem) with no GL context, proving
// the click → capture / reset wiring end-to-end. A simulated mouse press-then-
// release over a resolved button rect must reach RuntimeInputRebindMenu::OnUpdate
// and mutate the Gameplay action map via InputRebindController.
// =============================================================================

#include "OloEngine/Core/InputAction.h"
#include "OloEngine/Core/InputActionManager.h"
#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/UI/RuntimeInputRebindMenu.h"
#include "OloEngine/UI/UIInputSystem.h"
#include "OloEngine/UI/UILayoutSystem.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <string>

using namespace OloEngine; // NOLINT(google-build-using-namespace)

namespace
{
    constexpr u32 kW = 1920;
    constexpr u32 kH = 1080;

    bool JumpHasKey(KeyCode key)
    {
        const InputAction* a = InputActionManager::GetActionMap(InputContextType::Gameplay).GetAction("Jump");
        return a && std::ranges::find(a->Bindings, InputBinding::Key(key)) != a->Bindings.end();
    }
} // namespace

class RuntimeInputRebindMenuTest : public ::testing::Test
{
  protected:
    Ref<Scene> m_Scene;
    RuntimeInputRebindMenu m_Menu;

    void SetUp() override
    {
        InputActionManager::Init();
        InputActionManager::SetActionMap(InputContextType::Gameplay, CreateDefaultGameActions());
        m_Scene = Scene::Create();
        m_Menu.Open(*m_Scene, InputContextType::Gameplay, {});
    }

    void TearDown() override
    {
        m_Menu.Close();
        m_Scene = nullptr;
        InputActionManager::Shutdown();
    }

    // Locate a button by the caption of its child text label; returns its parent (the button).
    Entity FindButton(const std::string& caption)
    {
        auto view = m_Scene->GetAllEntitiesWith<UITextComponent>();
        for (const auto e : view)
        {
            if (view.get<UITextComponent>(e).m_Text == caption)
            {
                return Entity{ e, m_Scene.get() }.GetParent();
            }
        }
        return {};
    }

    // Run one UI frame: resolve layout, feed mouse, tick the menu.
    void Frame(glm::vec2 mouse, bool down, bool pressed)
    {
        UILayoutSystem::ResolveLayout(*m_Scene, kW, kH, glm::mat4(1.0f));
        UIInputSystem::ProcessInput(*m_Scene, mouse, down, pressed, 0.0f, 0.0f, {});
        m_Menu.OnUpdate();
    }

    // Simulate a full click (press then release) at the centre of a button's resolved rect.
    void ClickButton(Entity button)
    {
        ASSERT_TRUE(button);
        UILayoutSystem::ResolveLayout(*m_Scene, kW, kH, glm::mat4(1.0f));
        ASSERT_TRUE(button.HasComponent<UIResolvedRectComponent>());
        const auto& rr = button.GetComponent<UIResolvedRectComponent>();
        const glm::vec2 centre = rr.m_Position + rr.m_Size * 0.5f;
        Frame(centre, /*down=*/true, /*pressed=*/true);   // press
        Frame(centre, /*down=*/false, /*pressed=*/false); // release → click
    }
};

TEST_F(RuntimeInputRebindMenuTest, ClickingRebindButtonStartsCapture)
{
    EXPECT_FALSE(m_Menu.Controller().IsCapturing());

    Entity rebind = FindButton("Rebind");
    ASSERT_TRUE(rebind);
    ClickButton(rebind);

    EXPECT_TRUE(m_Menu.Controller().IsCapturing());
}

TEST_F(RuntimeInputRebindMenuTest, ClickingResetAllRestoresDefaultsThroughTheMenu)
{
    // Mutate Jump away from its default via the controller, then click "Reset All".
    m_Menu.Controller().BeginRebind("Jump", 0, /*gamepad=*/false);
    m_Menu.Controller().OnKeyPressed(Key::J);
    ASSERT_TRUE(JumpHasKey(Key::J));

    Entity resetAll = FindButton("Reset All to Default");
    ASSERT_TRUE(resetAll);
    ClickButton(resetAll);

    EXPECT_TRUE(JumpHasKey(Key::Space)); // default restored
    EXPECT_FALSE(JumpHasKey(Key::J));
}

// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Core/InputAction.h"
#include "OloEngine/Core/InputActionManager.h"
#include "OloEngine/Core/InputActionSerializer.h"
#include "OloEngine/Core/InputRebindController.h"
#include "OloEngine/Core/KeyCodes.h"
#include "OloEngine/Core/MouseCodes.h"

#include <algorithm>
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace OloEngine; // NOLINT(google-build-using-namespace)

namespace
{
    // True if `action` in the Gameplay map is bound to a keyboard `key`.
    bool ActionHasKey(const std::string& action, KeyCode key)
    {
        const InputAction* a = InputActionManager::GetActionMap(InputContextType::Gameplay).GetAction(action);
        if (!a)
        {
            return false;
        }
        return std::ranges::find(a->Bindings, InputBinding::Key(key)) != a->Bindings.end();
    }

    std::filesystem::path TempYamlPath(const char* stem)
    {
        auto dir = std::filesystem::temp_directory_path();
#ifdef _WIN32
        const auto pid = static_cast<unsigned>(_getpid());
#else
        const auto pid = static_cast<unsigned>(getpid());
#endif
        return dir / (std::string("olo_rebind_") + stem + "_" + std::to_string(pid) + ".yaml");
    }
} // namespace

class InputRebindControllerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        InputActionManager::Init();
        InputActionManager::SetActionMap(InputContextType::Gameplay, CreateDefaultGameActions());
        m_Ctrl.SetTargetContext(InputContextType::Gameplay);
    }

    void TearDown() override
    {
        InputActionManager::Shutdown();
    }

    InputRebindController m_Ctrl;
};

// --- Capture with no conflict applies immediately -------------------------------

TEST_F(InputRebindControllerTest, KeyboardCaptureRebindsPrimaryBinding)
{
    // Jump's primary binding is Space; rebind it to J (unused elsewhere).
    m_Ctrl.BeginRebind("Jump", 0, /*gamepad=*/false);
    EXPECT_TRUE(m_Ctrl.IsCapturing());

    const bool consumed = m_Ctrl.OnKeyPressed(Key::J);
    EXPECT_TRUE(consumed);
    EXPECT_FALSE(m_Ctrl.IsCapturing());
    EXPECT_FALSE(m_Ctrl.HasPendingConflict());

    EXPECT_TRUE(ActionHasKey("Jump", Key::J));
    EXPECT_FALSE(ActionHasKey("Jump", Key::Space)); // slot 0 (Space) was overwritten
}

TEST_F(InputRebindControllerTest, EscapeCancelsCaptureWithoutBinding)
{
    m_Ctrl.BeginRebind("Jump", 0, /*gamepad=*/false);
    EXPECT_TRUE(m_Ctrl.OnKeyPressed(Key::Escape));
    EXPECT_FALSE(m_Ctrl.IsCapturing());
    // Nothing changed — Space still bound, no J.
    EXPECT_TRUE(ActionHasKey("Jump", Key::Space));
    EXPECT_FALSE(ActionHasKey("Jump", Key::J));
}

TEST_F(InputRebindControllerTest, MouseCaptureAppendsNewBinding)
{
    m_Ctrl.BeginCaptureNew("Interact", /*gamepad=*/false);
    EXPECT_TRUE(m_Ctrl.OnMouseButtonPressed(Mouse::ButtonRight));

    const InputAction* interact = InputActionManager::GetActionMap(InputContextType::Gameplay).GetAction("Interact");
    ASSERT_NE(interact, nullptr);
    EXPECT_NE(std::ranges::find(interact->Bindings, InputBinding::MouseButton(Mouse::ButtonRight)), interact->Bindings.end());
}

// --- Conflict detection + resolution -------------------------------------------

TEST_F(InputRebindControllerTest, ConflictIsSurfacedNotAppliedUntilResolved)
{
    // Interact is bound to E; try to also bind E to Jump → conflict with Interact.
    m_Ctrl.BeginRebind("Jump", 0, /*gamepad=*/false);
    m_Ctrl.OnKeyPressed(Key::E);

    ASSERT_TRUE(m_Ctrl.HasPendingConflict());
    EXPECT_EQ(m_Ctrl.GetPendingConflict().ConflictingAction, "Interact");
    // Not applied yet: Jump slot 0 is still Space, Interact still has E.
    EXPECT_TRUE(ActionHasKey("Jump", Key::Space));
    EXPECT_TRUE(ActionHasKey("Interact", Key::E));
}

TEST_F(InputRebindControllerTest, ResolveReplaceMovesBindingToTarget)
{
    m_Ctrl.BeginRebind("Jump", 0, /*gamepad=*/false);
    m_Ctrl.OnKeyPressed(Key::E);
    ASSERT_TRUE(m_Ctrl.HasPendingConflict());

    m_Ctrl.ResolveConflict(RebindResolution::Replace);
    EXPECT_FALSE(m_Ctrl.HasPendingConflict());
    EXPECT_TRUE(ActionHasKey("Jump", Key::E));      // Jump now owns E
    EXPECT_FALSE(ActionHasKey("Interact", Key::E)); // stolen from Interact
}

TEST_F(InputRebindControllerTest, ResolveCancelLeavesEverythingUntouched)
{
    m_Ctrl.BeginRebind("Jump", 0, /*gamepad=*/false);
    m_Ctrl.OnKeyPressed(Key::E);
    m_Ctrl.ResolveConflict(RebindResolution::Cancel);

    EXPECT_FALSE(m_Ctrl.HasPendingConflict());
    EXPECT_TRUE(ActionHasKey("Jump", Key::Space)); // unchanged
    EXPECT_TRUE(ActionHasKey("Interact", Key::E)); // unchanged
}

TEST_F(InputRebindControllerTest, ResolveSwapExchangesBindings)
{
    // Rebind Jump's slot 0 (Space) to E, which Interact owns. Swap should hand Interact
    // the target's old binding (Space) and give Jump E.
    m_Ctrl.BeginRebind("Jump", 0, /*gamepad=*/false);
    m_Ctrl.OnKeyPressed(Key::E);
    ASSERT_TRUE(m_Ctrl.HasPendingConflict());

    m_Ctrl.ResolveConflict(RebindResolution::Swap);
    EXPECT_TRUE(ActionHasKey("Jump", Key::E));
    EXPECT_TRUE(ActionHasKey("Interact", Key::Space));
    EXPECT_FALSE(ActionHasKey("Interact", Key::E));
}

// --- Reset to default ----------------------------------------------------------

TEST_F(InputRebindControllerTest, ResetActionRestoresDefaultBindings)
{
    m_Ctrl.BeginRebind("Jump", 0, /*gamepad=*/false);
    m_Ctrl.OnKeyPressed(Key::J);
    ASSERT_TRUE(ActionHasKey("Jump", Key::J));

    m_Ctrl.ResetActionToDefault("Jump");
    EXPECT_TRUE(ActionHasKey("Jump", Key::Space)); // default restored
    EXPECT_FALSE(ActionHasKey("Jump", Key::J));
}

TEST_F(InputRebindControllerTest, ResetWholeMapRestoresAllDefaults)
{
    m_Ctrl.BeginRebind("Jump", 0, false);
    m_Ctrl.OnKeyPressed(Key::J);
    m_Ctrl.BeginRebind("Interact", 0, false);
    m_Ctrl.OnKeyPressed(Key::K);

    m_Ctrl.ResetTargetMapToDefault();

    const InputActionMap defaults = CreateDefaultGameActions();
    const InputActionMap& current = InputActionManager::GetActionMap(InputContextType::Gameplay);
    EXPECT_EQ(current.Actions.size(), defaults.Actions.size());
    EXPECT_TRUE(ActionHasKey("Jump", Key::Space));
    EXPECT_TRUE(ActionHasKey("Interact", Key::E));
}

// --- Persistence round-trip (survives a "restart") -----------------------------

TEST_F(InputRebindControllerTest, RebindPersistsThroughSerializerRoundTrip)
{
    // Rebind Interact to J, then save.
    m_Ctrl.BeginRebind("Interact", 0, false);
    m_Ctrl.OnKeyPressed(Key::J);
    ASSERT_TRUE(ActionHasKey("Interact", Key::J));

    const auto path = TempYamlPath("roundtrip");
    ASSERT_TRUE(InputRebindController::Save(path));

    // Simulate a restart: wipe manager state, reload from disk.
    InputActionManager::Shutdown();
    InputActionManager::Init();
    auto loaded = InputActionSerializer::DeserializeContexts(path);
    ASSERT_TRUE(loaded.has_value());
    InputActionManager::ReplaceAllContextMaps(*loaded);

    EXPECT_TRUE(ActionHasKey("Interact", Key::J)); // rebind survived

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// OLO_TEST_LAYER: unit
//
// The ON-path half of the Steamworks tests (#644).
//
// WHAT THIS IS FOR
// ----------------
// SteamManagerTest.cpp drives SteamManager through a fake ISteamBackend, so it proves the
// engine's LOGIC on every configuration — but it never touches SteamworksBackend.cpp, the one
// translation unit that actually calls Valve's API. On a public repo the real SDK can never
// reach CI (non-redistributable headers), so without this file that TU would be compiled on
// developer machines only and would bitrot silently: Trap 3 in
// docs/agent-rules/vcpkg-dependency-management.md.
//
// Building with -DOLO_WITH_STEAM_STUB_SDK=ON swaps in hand-written stub headers plus a .cpp that
// provides their symbols, so CI COMPILES, LINKS and RUNS a genuine OLO_WITH_STEAM=1 binary. These
// tests then exercise the real backend code against canned returns.
//
// WHAT THIS DOES NOT PROVE
// ------------------------
// Nothing about Valve's servers, the Steam client, or the overlay. It proves the Valve-calling
// code compiles, links, and routes correctly. Live behaviour is manually verified against App ID
// 480 — see the PR body and docs/ops/build.md.
//
// Every test SKIPS cleanly when the stub is not the active SDK, so this file is harmless in a
// normal OFF build and in a developer's real-SDK build.

#include "Platform/Steam/SteamManager.h"
#include "Platform/Steam/StubSDK/SteamStubControl.h"

#include <span>
#include <string>
#include <vector>

namespace SteamStub = OloEngine::SteamStub;
using OloEngine::SteamCloudQuota;
using OloEngine::SteamManager;
using OloEngine::SteamResult;
using OloEngine::SteamSucceeded;

namespace
{
    constexpr const char* kKnownAchievement = "ACH_STUB_KNOWN";

    [[nodiscard]] std::span<const u8> AsBytes(const std::string& text)
    {
        return std::span<const u8>{ reinterpret_cast<const u8*>(text.data()), text.size() };
    }

    class SteamStubOnPathTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            if (!SteamStub::IsActive())
            {
                GTEST_SKIP() << "stub SDK not compiled in (needs -DOLO_WITH_STEAM_STUB_SDK=ON); "
                                "the ON path cannot be exercised on this configuration";
            }

            // Reset BOTH layers. The stub holds process-wide state and the manager holds a
            // backend, and leaving either behind leaks into the next test.
            SteamManager::ResetForTesting();
            SteamStub::Reset();
            SteamStub::RegisterAchievement(kKnownAchievement);
        }

        void TearDown() override
        {
            SteamManager::ResetForTesting();
            SteamStub::Reset();
        }
    };

    // =================================================================================
    // The behaviour that must never regress, now covered in CI rather than by one manual run.
    // =================================================================================

    // The real SteamworksBackend::Initialize() calls SteamAPI_InitEx and inspects
    // ESteamAPIInitResult. This drives that code with a forced failure — the "Steam client isn't
    // running" case — and asserts the engine warns and continues instead of throwing.
    TEST_F(SteamStubOnPathTest, FailedSteamInitDoesNotThrowAndLeavesSteamUnavailable)
    {
        SteamStub::SetInitResult(2); // k_ESteamAPIInitResult_NoSteamClient

        EXPECT_NO_THROW(SteamManager::Initialize());
        EXPECT_FALSE(SteamManager::IsAvailable());

        // ...and the whole surface stays callable and inert afterwards.
        EXPECT_EQ(SteamManager::UnlockAchievement(kKnownAchievement), SteamResult::Unavailable);
        EXPECT_EQ(SteamManager::SetRichPresence("status", "x"), SteamResult::Unavailable);
        EXPECT_FALSE(SteamManager::IsOverlayActive());
        EXPECT_NO_THROW(SteamManager::RunCallbacks());
        EXPECT_NO_THROW(SteamManager::Shutdown());
    }

    TEST_F(SteamStubOnPathTest, VersionMismatchIsAlsoNonFatal)
    {
        SteamStub::SetInitResult(3); // k_ESteamAPIInitResult_VersionMismatch

        EXPECT_NO_THROW(SteamManager::Initialize());
        EXPECT_FALSE(SteamManager::IsAvailable());
    }

    TEST_F(SteamStubOnPathTest, SuccessfulInitMakesSteamAvailable)
    {
        SteamManager::Initialize();

        EXPECT_TRUE(SteamManager::IsAvailable());
        EXPECT_EQ(SteamManager::GetAppId(), 480u);
        EXPECT_FALSE(SteamManager::GetPersonaName().empty());
    }

    // Exercises the real Shutdown()/SteamAPI_Shutdown() path and proves a second call is safe —
    // which matters because Application reaches Shutdown from two different places.
    TEST_F(SteamStubOnPathTest, ShutdownIsSafeAndRepeatable)
    {
        SteamManager::Initialize();
        ASSERT_TRUE(SteamManager::IsAvailable());

        EXPECT_NO_THROW(SteamManager::Shutdown());
        EXPECT_FALSE(SteamManager::IsAvailable());
        EXPECT_NO_THROW(SteamManager::Shutdown());
    }

    // =================================================================================
    // Achievements through the REAL backend code.
    // =================================================================================

    TEST_F(SteamStubOnPathTest, UnlockAchievementReachesTheSdkAndStores)
    {
        SteamManager::Initialize();
        ASSERT_TRUE(SteamManager::IsAvailable());

        EXPECT_EQ(SteamManager::UnlockAchievement(kKnownAchievement), SteamResult::Success);
        EXPECT_TRUE(SteamStub::IsAchievementUnlocked(kKnownAchievement));
        EXPECT_EQ(SteamStub::GetStoreStatsCallCount(), 1u);
    }

    // The dedup, verified end-to-end through the real ISteamUserStats calls rather than a fake.
    TEST_F(SteamStubOnPathTest, RepeatUnlockDoesNotIssueASecondStore)
    {
        SteamManager::Initialize();
        ASSERT_EQ(SteamManager::UnlockAchievement(kKnownAchievement), SteamResult::Success);
        ASSERT_EQ(SteamStub::GetStoreStatsCallCount(), 1u);

        EXPECT_EQ(SteamManager::UnlockAchievement(kKnownAchievement), SteamResult::AlreadySet);
        EXPECT_EQ(SteamStub::GetStoreStatsCallCount(), 1u);
    }

    // The stub reports an unregistered id the same way Steam reports an undefined achievement,
    // which is what drives SteamworksBackend's NotFound mapping.
    TEST_F(SteamStubOnPathTest, UnknownAchievementIsReportedAsNotFound)
    {
        SteamManager::Initialize();

        EXPECT_EQ(SteamManager::UnlockAchievement("ACH_NEVER_REGISTERED"), SteamResult::NotFound);
        EXPECT_EQ(SteamStub::GetStoreStatsCallCount(), 0u);
    }

    TEST_F(SteamStubOnPathTest, PreUnlockedAchievementIsDetected)
    {
        SteamStub::ForceAchievementUnlocked(kKnownAchievement, true);
        SteamManager::Initialize();

        EXPECT_TRUE(SteamManager::IsAchievementUnlocked(kKnownAchievement));
        EXPECT_EQ(SteamManager::UnlockAchievement(kKnownAchievement), SteamResult::AlreadySet);
        EXPECT_EQ(SteamStub::GetStoreStatsCallCount(), 0u) << "an already-unlocked achievement issued a store";
    }

    // =================================================================================
    // Rich presence and overlay through the real backend.
    // =================================================================================

    TEST_F(SteamStubOnPathTest, RichPresenceReachesTheSdk)
    {
        SteamManager::Initialize();

        EXPECT_EQ(SteamManager::SetRichPresence("status", "In the caves"), SteamResult::Success);
        EXPECT_NO_THROW(SteamManager::ClearRichPresence());
    }

    // Overlay state must come from the GameOverlayActivated_t callback, NOT from
    // ISteamUtils::IsOverlayEnabled() — that answers "is the overlay available", so using it here
    // would report the overlay permanently active for the whole session. With no callback
    // delivered yet, the honest answer is "closed".
    TEST_F(SteamStubOnPathTest, OverlayReportsClosedWithoutACallback)
    {
        SteamManager::Initialize();
        EXPECT_FALSE(SteamManager::IsOverlayActive());
    }

    // Drives SteamworksBackend::OnGameOverlayActivated for real.
    //
    // This is the only callback-handling code in the backend, and until the stub learned to
    // dispatch it was dead in CI — registered, never invoked. The stub now delivers the event
    // through SteamAPI_RunCallbacks, so the engine's registration, the member-function
    // dispatch, and the m_OverlayActive it maintains are all executed here.
    TEST_F(SteamStubOnPathTest, OverlayStateTracksTheCallback)
    {
        SteamManager::Initialize();
        ASSERT_FALSE(SteamManager::IsOverlayActive());

        // The transition is queued, not applied: nothing changes until the frame pump runs.
        SteamStub::SetOverlayActive(true);
        EXPECT_FALSE(SteamManager::IsOverlayActive())
            << "overlay state changed without RunCallbacks — it is not going through the callback";

        SteamManager::RunCallbacks();
        EXPECT_TRUE(SteamManager::IsOverlayActive()) << "the overlay-opened callback did not reach the backend";

        SteamStub::SetOverlayActive(false);
        SteamManager::RunCallbacks();
        EXPECT_FALSE(SteamManager::IsOverlayActive()) << "the overlay-closed callback did not reach the backend";
    }

    // Shutdown must UNREGISTER the callback, so nothing can dispatch into a backend that has torn
    // down. The ordering that makes this safe is Unregister() before SteamAPI_Shutdown().
    //
    // Asserted through SteamStub::IsOverlayCallbackRegistered() rather than through behaviour,
    // because the behavioural version of this test is worthless: after Shutdown the manager has
    // no backend, so RunCallbacks returns before reaching any dispatch and IsOverlayActive() is
    // false regardless. Such a test passes whether or not Unregister() is ever called — it looks
    // like coverage while asserting nothing about the thing it names.
    TEST_F(SteamStubOnPathTest, ShutdownUnregistersTheOverlayCallback)
    {
        EXPECT_FALSE(SteamStub::IsOverlayCallbackRegistered()) << "a callback was registered before Initialize";

        SteamManager::Initialize();
        ASSERT_TRUE(SteamManager::IsAvailable());
        EXPECT_TRUE(SteamStub::IsOverlayCallbackRegistered())
            << "Initialize did not register the overlay callback — the backend cannot receive events";

        SteamManager::Shutdown();
        EXPECT_FALSE(SteamStub::IsOverlayCallbackRegistered())
            << "Shutdown left the overlay callback registered, pointing at a destroyed backend";

        // ...and pumping afterwards is still safe.
        SteamStub::SetOverlayActive(true);
        EXPECT_NO_THROW(SteamManager::RunCallbacks());
        EXPECT_FALSE(SteamManager::IsOverlayActive());
    }

    // =================================================================================
    // Cloud through the real ISteamRemoteStorage calls.
    // =================================================================================

    TEST_F(SteamStubOnPathTest, CloudRoundTripsThroughTheSdk)
    {
        SteamManager::Initialize();
        ASSERT_TRUE(SteamManager::IsCloudEnabled());

        const std::string payload = "stub-save-bytes";
        ASSERT_EQ(SteamManager::CloudWrite("slot0.sav", AsBytes(payload)), SteamResult::Success);
        EXPECT_TRUE(SteamStub::CloudHasFile("slot0.sav"));

        std::vector<u8> readBack;
        ASSERT_EQ(SteamManager::CloudRead("slot0.sav", readBack), SteamResult::Success);
        EXPECT_EQ(std::string(readBack.begin(), readBack.end()), payload);
    }

    // Both switches must be on. Exercised here through the real IsCloudEnabledForAccount() /
    // IsCloudEnabledForApp() pair, which is the part a fake backend cannot cover.
    TEST_F(SteamStubOnPathTest, EitherCloudSwitchOffDisablesCloud)
    {
        SteamManager::Initialize();

        SteamStub::SetCloudEnabled(false, true);
        EXPECT_FALSE(SteamManager::IsCloudEnabled()) << "account-level Cloud off was ignored";

        SteamStub::SetCloudEnabled(true, false);
        EXPECT_FALSE(SteamManager::IsCloudEnabled()) << "app-level Cloud off was ignored";

        SteamStub::SetCloudEnabled(true, true);
        EXPECT_TRUE(SteamManager::IsCloudEnabled());
    }

    TEST_F(SteamStubOnPathTest, CloudEnumerateAndDeleteReachTheSdk)
    {
        SteamManager::Initialize();
        ASSERT_EQ(SteamManager::CloudWrite("a.sav", AsBytes("1")), SteamResult::Success);
        ASSERT_EQ(SteamManager::CloudWrite("b.sav", AsBytes("2")), SteamResult::Success);

        EXPECT_EQ(SteamManager::CloudEnumerate().size(), 2u);

        EXPECT_EQ(SteamManager::CloudDelete("a.sav"), SteamResult::Success);
        EXPECT_FALSE(SteamStub::CloudHasFile("a.sav"));
        EXPECT_EQ(SteamManager::CloudEnumerate().size(), 1u);
    }

    TEST_F(SteamStubOnPathTest, CloudQuotaIsReadThroughTheSdk)
    {
        SteamStub::SetCloudQuota(8192, 2048);
        SteamManager::Initialize();

        SteamCloudQuota quota;
        ASSERT_EQ(SteamManager::GetCloudQuota(quota), SteamResult::Success);
        EXPECT_EQ(quota.TotalBytes, 8192u);
        EXPECT_EQ(quota.AvailableBytes, 2048u);
    }

    // A zero-length cloud file is legal but carries no save. Handing the caller an empty buffer
    // that looks like a valid save is worse than reporting it absent.
    TEST_F(SteamStubOnPathTest, EmptyCloudFileReadsAsNotFound)
    {
        SteamManager::Initialize();
        ASSERT_EQ(SteamManager::CloudWrite("empty.sav", AsBytes("")), SteamResult::Success);

        std::vector<u8> readBack;
        EXPECT_EQ(SteamManager::CloudRead("empty.sav", readBack), SteamResult::NotFound);
    }

    // =================================================================================
    // Steam Input through the real ISteamInput calls.
    // =================================================================================

    TEST_F(SteamStubOnPathTest, InputComesUpAutomaticallyWithSteamItself)
    {
        SteamManager::Initialize();
        EXPECT_TRUE(SteamManager::IsInputAvailable());
    }

    TEST_F(SteamStubOnPathTest, NoControllersConnectedIsTheOrdinaryState)
    {
        SteamManager::Initialize();
        EXPECT_TRUE(SteamManager::GetConnectedControllers().empty());
    }

    TEST_F(SteamStubOnPathTest, ConnectedControllersRoundTripThroughTheSdk)
    {
        SteamStub::SetConnectedControllers({ 1, 2 });
        SteamManager::Initialize();

        const auto controllers = SteamManager::GetConnectedControllers();
        ASSERT_EQ(controllers.size(), 2u);
        EXPECT_EQ(controllers[0], 1u);
        EXPECT_EQ(controllers[1], 2u);
    }

    TEST_F(SteamStubOnPathTest, ActivateActionSetReachesTheSdk)
    {
        SteamStub::SetConnectedControllers({ 1 });
        SteamManager::Initialize();

        const auto actionSet = SteamManager::GetActionSetHandle("Gameplay");
        SteamManager::ActivateActionSet(1, actionSet);
        EXPECT_EQ(SteamStub::GetActiveActionSetName(1), "Gameplay");
    }

    // An action with no configured state reports Active=false — "not bound", not a fabricated
    // press — which is what lets InputActionManager fall back to the engine's own bindings.
    TEST_F(SteamStubOnPathTest, UnboundDigitalActionReportsInactive)
    {
        SteamManager::Initialize();
        const auto handle = SteamManager::GetDigitalActionHandle("Jump");
        const auto state = SteamManager::GetDigitalActionState(1, handle);
        EXPECT_FALSE(state.Active);
    }

    TEST_F(SteamStubOnPathTest, DigitalAndAnalogActionStateReachTheSdk)
    {
        SteamManager::Initialize();

        const auto digitalHandle = SteamManager::GetDigitalActionHandle("Jump");
        SteamStub::SetDigitalActionState(1, "Jump", /*pressed*/ true);
        const auto digitalState = SteamManager::GetDigitalActionState(1, digitalHandle);
        EXPECT_TRUE(digitalState.Active);
        EXPECT_TRUE(digitalState.Pressed);

        const auto analogHandle = SteamManager::GetAnalogActionHandle("Move");
        SteamStub::SetAnalogActionState(1, "Move", 0.75f, -0.5f);
        const auto analogState = SteamManager::GetAnalogActionState(1, analogHandle);
        EXPECT_TRUE(analogState.Active);
        EXPECT_FLOAT_EQ(analogState.X, 0.75f);
        EXPECT_FLOAT_EQ(analogState.Y, -0.5f);
    }

    // Valve reports a Trigger-mode analog action on 0..1 (0 = released); SteamworksBackend must
    // normalize that to this engine's own GamepadAxis convention (-1..1, -1 = released — see
    // GamepadCodes.h's GamepadAxis::RightTrigger) so a trigger-shaped action reads identically
    // whether it came from Steam Input or raw XInput/DirectInput.
    TEST_F(SteamStubOnPathTest, TriggerModeAnalogActionAtRestNormalizesToMinusOne)
    {
        SteamManager::Initialize();
        const auto handle = SteamManager::GetAnalogActionHandle("Accelerate");
        SteamStub::SetAnalogActionState(1, "Accelerate", /*x*/ 0.0f, /*y*/ 0.0f, /*active*/ true, /*triggerMode*/ true);

        const auto state = SteamManager::GetAnalogActionState(1, handle);
        EXPECT_TRUE(state.Active);
        EXPECT_FLOAT_EQ(state.X, -1.0f);
    }

    TEST_F(SteamStubOnPathTest, TriggerModeAnalogActionAtFullPressNormalizesToPlusOne)
    {
        SteamManager::Initialize();
        const auto handle = SteamManager::GetAnalogActionHandle("Accelerate");
        SteamStub::SetAnalogActionState(1, "Accelerate", /*x*/ 1.0f, /*y*/ 0.0f, /*active*/ true, /*triggerMode*/ true);

        const auto state = SteamManager::GetAnalogActionState(1, handle);
        EXPECT_TRUE(state.Active);
        EXPECT_FLOAT_EQ(state.X, 1.0f);
    }

    TEST_F(SteamStubOnPathTest, TriggerModeAnalogActionAtHalfPressNormalizesToZero)
    {
        SteamManager::Initialize();
        const auto handle = SteamManager::GetAnalogActionHandle("Accelerate");
        SteamStub::SetAnalogActionState(1, "Accelerate", /*x*/ 0.5f, /*y*/ 0.0f, /*active*/ true, /*triggerMode*/ true);

        const auto state = SteamManager::GetAnalogActionState(1, handle);
        EXPECT_FLOAT_EQ(state.X, 0.0f);
    }

    // A JoystickMove-mode action (the default, and what DigitalAndAnalogActionStateReachTheSdk
    // above already exercises) is already on this engine's -1..1 convention and must NOT be
    // renormalized — the mode check must be selective, not a blanket transform.
    TEST_F(SteamStubOnPathTest, JoystickModeAnalogActionIsNotRenormalized)
    {
        SteamManager::Initialize();
        const auto handle = SteamManager::GetAnalogActionHandle("Move");
        SteamStub::SetAnalogActionState(1, "Move", /*x*/ -0.3f, /*y*/ 0.0f, /*active*/ true, /*triggerMode*/ false);

        const auto state = SteamManager::GetAnalogActionState(1, handle);
        EXPECT_FLOAT_EQ(state.X, -0.3f);
    }

    // Glyph lookup through GetDigitalActionOrigins -> GetStringForActionOrigin /
    // GetGlyphPNGForActionOrigin — the real chain a button prompt takes, not a shortcut keyed
    // straight off the action name.
    TEST_F(SteamStubOnPathTest, GlyphLookupReachesTheSdkThroughOrigins)
    {
        SteamManager::Initialize();
        SteamStub::SetGlyphForAction("Jump", "A Button", "/glyphs/a_button.png");

        // A real (non-zero) action set handle is required here: GetDigitalActionOrigins is the
        // real Valve API contract, and SteamworksBackend rejects kInvalidSteamInputActionSetHandle
        // (0) before ever reaching the SDK — the stub itself ignores the actionSet value, but the
        // backend's own guard does not, so passing the sentinel here always resolves to "no origin".
        const auto actionSet = SteamManager::GetActionSetHandle("Gameplay");
        const auto handle = SteamManager::GetDigitalActionHandle("Jump");
        EXPECT_EQ(SteamManager::GetGlyphLabelForDigitalAction(1, actionSet, handle), "A Button");
        EXPECT_EQ(SteamManager::GetGlyphPngForDigitalAction(1, actionSet, handle), "/glyphs/a_button.png");
    }

    TEST_F(SteamStubOnPathTest, GlyphLookupForAnUnconfiguredActionIsNotConfusedWithARealFailure)
    {
        SteamManager::Initialize();
        const auto actionSet = SteamManager::GetActionSetHandle("Gameplay");
        const auto handle = SteamManager::GetDigitalActionHandle("NeverConfigured");

        // The stub's fallback label is deliberately obviously-synthetic (matching the rule
        // applied to SteamAPI_InitEx's error message) rather than empty, so a test reading the
        // log can't mistake it for a real Steam response.
        EXPECT_EQ(SteamManager::GetGlyphLabelForDigitalAction(1, actionSet, handle), "stub SDK: unlabelled origin");
        EXPECT_TRUE(SteamManager::GetGlyphPngForDigitalAction(1, actionSet, handle).empty());
    }

    TEST_F(SteamStubOnPathTest, InputShutdownIsSafeAndRepeatable)
    {
        SteamManager::Initialize();
        ASSERT_TRUE(SteamManager::IsInputAvailable());

        EXPECT_NO_THROW(SteamManager::Shutdown());
        EXPECT_FALSE(SteamManager::IsInputAvailable());
        EXPECT_NO_THROW(SteamManager::Shutdown());
    }

    // Per-frame pump against the real SteamAPI_RunCallbacks symbol.
    TEST_F(SteamStubOnPathTest, RunCallbacksIsSafeBeforeAndAfterInit)
    {
        EXPECT_NO_THROW(SteamManager::RunCallbacks());

        SteamManager::Initialize();
        for (int i = 0; i < 8; ++i)
        {
            EXPECT_NO_THROW(SteamManager::RunCallbacks());
        }

        SteamManager::Shutdown();
        EXPECT_NO_THROW(SteamManager::RunCallbacks());
    }
} // namespace

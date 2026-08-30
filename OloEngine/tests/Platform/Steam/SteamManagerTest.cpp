#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// OLO_TEST_LAYER: unit
//
// Steamworks platform integration (#644).
//
// THIS FILE IS WHERE THE DURABLE VALUE OF THE FEATURE LIVES. The live Valve-server behaviour can
// only be checked by hand, on one machine, by a developer holding a partner-account SDK. What CAN
// be checked everywhere, on every push, is:
//
//   1. the DEGRADATION CONTRACT — that every entry point no-ops and reports Unavailable when
//      Steam is absent, and that Initialize() logs-and-continues rather than throwing. This is
//      what protects the fresh-clone contributor who has never heard of Steamworks, and it is
//      the behaviour whose regression would be most silent;
//   2. the LOGIC THAT ACTUALLY HAS BUGS — the already-unlocked dedup, rich-presence validation,
//      the cloud-disabled short-circuit. None of it needs a Steam client.
//
// These run on every configuration, including a build with no SDK at all, because they drive
// SteamManager through a fake ISteamBackend rather than through Valve's API. The ON-path half —
// proving the real Valve-calling TU compiles, links and behaves — lives in
// SteamStubOnPathTest.cpp and runs against the hand-written stub SDK in CI.

#include "FakeSteamBackend.h"

#include "Platform/Steam/SteamBackend.h"
#include "Platform/Steam/SteamManager.h"

#include <algorithm>
#include <set>
#include <span>
#include <string>
#include <vector>

using OloEngine::SteamCloudQuota;
using OloEngine::SteamManager;
using OloEngine::SteamResult;
using OloEngine::SteamSucceeded;
using OloEngine::Testing::FakeSteamBackend;

namespace
{
    constexpr const char* kKnownAchievement = "ACH_TEST_KNOWN";
    constexpr const char* kUnknownAchievement = "ACH_TEST_TYPO";

    [[nodiscard]] std::span<const u8> AsBytes(const std::string& text)
    {
        return std::span<const u8>{ reinterpret_cast<const u8*>(text.data()), text.size() };
    }

    class SteamManagerTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            auto backend = OloEngine::CreateScope<FakeSteamBackend>();
            m_Fake = backend.get();
            m_Fake->KnownAchievements.emplace(kKnownAchievement);
            SteamManager::SetBackendForTesting(std::move(backend));
        }

        void TearDown() override
        {
            SteamManager::ResetForTesting();
            m_Fake = nullptr;
        }

        // Bring the manager up with the fake installed.
        void InitializeManager()
        {
            SteamManager::Initialize();
        }

        FakeSteamBackend* m_Fake = nullptr;
    };

    // =================================================================================
    // The degradation contract.
    // =================================================================================

    // The single most important behaviour in the feature. Application.cpp throws
    // std::runtime_error when AudioEngine or NetworkManager fail to initialise; Steam must never
    // join that club, because "no Steam client" is an ordinary state on a dev box, a CI runner,
    // or a player's machine when the exe is launched directly.
    TEST_F(SteamManagerTest, InitializeDoesNotThrowWhenSteamIsUnavailable)
    {
        m_Fake->InitializeSucceeds = false;
        EXPECT_NO_THROW(SteamManager::Initialize());
        EXPECT_FALSE(SteamManager::IsAvailable());
    }

    TEST_F(SteamManagerTest, InitializeDoesNotThrowWhenSteamIsAvailable)
    {
        m_Fake->InitializeSucceeds = true;
        EXPECT_NO_THROW(SteamManager::Initialize());
        EXPECT_TRUE(SteamManager::IsAvailable());
    }

    // Every public entry point must be callable with Steam down and return a DEFINED
    // "unavailable" answer — never a crash, never an exception, never a half-written out-param.
    // This is what lets game code call Steam unconditionally instead of guarding every site.
    TEST_F(SteamManagerTest, EveryEntryPointNoOpsWhenUnavailable)
    {
        m_Fake->InitializeSucceeds = false;
        SteamManager::Initialize();
        ASSERT_FALSE(SteamManager::IsAvailable());

        EXPECT_EQ(SteamManager::GetAppId(), 0u);
        EXPECT_TRUE(SteamManager::GetPersonaName().empty());

        EXPECT_EQ(SteamManager::UnlockAchievement(kKnownAchievement), SteamResult::Unavailable);
        EXPECT_EQ(SteamManager::ClearAchievement(kKnownAchievement), SteamResult::Unavailable);
        EXPECT_FALSE(SteamManager::IsAchievementUnlocked(kKnownAchievement));
        EXPECT_EQ(SteamManager::StoreStats(), SteamResult::Unavailable);

        EXPECT_EQ(SteamManager::SetRichPresence("status", "testing"), SteamResult::Unavailable);
        EXPECT_NO_THROW(SteamManager::ClearRichPresence());

        EXPECT_FALSE(SteamManager::IsOverlayActive());

        EXPECT_FALSE(SteamManager::IsCloudEnabled());
        EXPECT_EQ(SteamManager::CloudWrite("save.bin", AsBytes("data")), SteamResult::Unavailable);

        std::vector<u8> readBuffer;
        EXPECT_EQ(SteamManager::CloudRead("save.bin", readBuffer), SteamResult::Unavailable);
        EXPECT_TRUE(readBuffer.empty());

        EXPECT_FALSE(SteamManager::CloudExists("save.bin"));
        EXPECT_EQ(SteamManager::CloudDelete("save.bin"), SteamResult::Unavailable);
        EXPECT_TRUE(SteamManager::CloudEnumerate().empty());

        SteamCloudQuota quota;
        EXPECT_EQ(SteamManager::GetCloudQuota(quota), SteamResult::Unavailable);

        EXPECT_NO_THROW(SteamManager::RunCallbacks());
    }

    // Reachable from BOTH of Application's shutdown paths, and from neither if startup died
    // early. Missing one leaks the Steam session, so Shutdown has to tolerate every ordering.
    TEST_F(SteamManagerTest, ShutdownIsSafeWithoutInitializeAndIsIdempotent)
    {
        EXPECT_NO_THROW(SteamManager::Shutdown());
        EXPECT_NO_THROW(SteamManager::Shutdown());
        EXPECT_FALSE(SteamManager::IsAvailable());
    }

    TEST_F(SteamManagerTest, InitializeIsIdempotent)
    {
        SteamManager::Initialize();
        SteamManager::Initialize();
        EXPECT_EQ(m_Fake->InitializeCalls, 1u) << "a second Initialize must not restart the Steam session";
    }

    TEST_F(SteamManagerTest, RunCallbacksReachesTheBackendOnlyWhenAvailable)
    {
        SteamManager::RunCallbacks();
        EXPECT_EQ(m_Fake->RunCallbacksCalls, 0u) << "callbacks pumped before Initialize";

        InitializeManager();
        SteamManager::RunCallbacks();
        SteamManager::RunCallbacks();
        EXPECT_EQ(m_Fake->RunCallbacksCalls, 2u);
    }

    // =================================================================================
    // Achievements — the dedup is the interesting logic.
    // =================================================================================

    TEST_F(SteamManagerTest, UnlockAchievementStoresStatsSoTheOverlayToastFires)
    {
        InitializeManager();

        EXPECT_EQ(SteamManager::UnlockAchievement(kKnownAchievement), SteamResult::Success);
        EXPECT_TRUE(m_Fake->UnlockedAchievements.contains(kKnownAchievement));

        // The overlay notification fires on the STORE, not the set. Without this the achievement
        // is recorded but the player sees nothing, which reads as "achievements are broken".
        EXPECT_EQ(m_Fake->StoreStatsCalls, 1u);
    }

    // The reason the dedup exists: StoreStats is a network round-trip, and a game unlocking from
    // an OnUpdate would otherwise issue one every frame for the rest of the session.
    TEST_F(SteamManagerTest, RepeatUnlockReportsAlreadySetAndDoesNotStoreAgain)
    {
        InitializeManager();
        ASSERT_EQ(SteamManager::UnlockAchievement(kKnownAchievement), SteamResult::Success);
        ASSERT_EQ(m_Fake->StoreStatsCalls, 1u);

        for (int i = 0; i < 10; ++i)
        {
            EXPECT_EQ(SteamManager::UnlockAchievement(kKnownAchievement), SteamResult::AlreadySet);
        }

        EXPECT_EQ(m_Fake->StoreStatsCalls, 1u) << "repeat unlocks issued extra network stores";
        EXPECT_EQ(m_Fake->SetAchievementCalls, 1u) << "repeat unlocks re-set the achievement";
    }

    // AlreadySet must remain distinguishable from Success — an unlock sting or a telemetry hook
    // needs to tell "just earned" from "already had it" — while still counting as success.
    TEST_F(SteamManagerTest, AlreadySetCountsAsSuccessButIsNotSuccess)
    {
        InitializeManager();
        ASSERT_EQ(SteamManager::UnlockAchievement(kKnownAchievement), SteamResult::Success);

        const SteamResult repeat = SteamManager::UnlockAchievement(kKnownAchievement);
        EXPECT_NE(repeat, SteamResult::Success);
        EXPECT_TRUE(SteamSucceeded(repeat));
    }

    // A typo'd id is silent in-game; the only place it can surface is here.
    TEST_F(SteamManagerTest, UnknownAchievementReportsNotFoundAndDoesNotWrite)
    {
        InitializeManager();

        EXPECT_EQ(SteamManager::UnlockAchievement(kUnknownAchievement), SteamResult::NotFound);
        EXPECT_EQ(m_Fake->SetAchievementCalls, 0u) << "an unknown id must not reach SetAchievement";
        EXPECT_EQ(m_Fake->StoreStatsCalls, 0u);
    }

    TEST_F(SteamManagerTest, EmptyAchievementIdIsRejectedBeforeTouchingTheBackend)
    {
        InitializeManager();

        EXPECT_EQ(SteamManager::UnlockAchievement(""), SteamResult::InvalidArgument);
        EXPECT_EQ(SteamManager::ClearAchievement(""), SteamResult::InvalidArgument);
        EXPECT_EQ(m_Fake->SetAchievementCalls, 0u);
    }

    // Failures must be surfaced, not swallowed — the caller decides whether to retry or report.
    TEST_F(SteamManagerTest, BackendFailuresAreSurfaced)
    {
        InitializeManager();

        m_Fake->SetAchievementFails = true;
        EXPECT_EQ(SteamManager::UnlockAchievement(kKnownAchievement), SteamResult::Failed);
        EXPECT_EQ(m_Fake->StoreStatsCalls, 0u) << "a failed set must not be followed by a store";

        m_Fake->SetAchievementFails = false;
        m_Fake->StoreStatsFails = true;
        EXPECT_EQ(SteamManager::UnlockAchievement(kKnownAchievement), SteamResult::Failed);
    }

    TEST_F(SteamManagerTest, IsAchievementUnlockedReflectsBackendState)
    {
        InitializeManager();

        EXPECT_FALSE(SteamManager::IsAchievementUnlocked(kKnownAchievement));
        ASSERT_EQ(SteamManager::UnlockAchievement(kKnownAchievement), SteamResult::Success);
        EXPECT_TRUE(SteamManager::IsAchievementUnlocked(kKnownAchievement));

        // An unknown id is "not unlocked" rather than an error, so callers can branch on it.
        EXPECT_FALSE(SteamManager::IsAchievementUnlocked(kUnknownAchievement));
    }

    TEST_F(SteamManagerTest, ClearAchievementStoresStats)
    {
        InitializeManager();
        ASSERT_EQ(SteamManager::UnlockAchievement(kKnownAchievement), SteamResult::Success);
        const u32 storesAfterUnlock = m_Fake->StoreStatsCalls;

        EXPECT_EQ(SteamManager::ClearAchievement(kKnownAchievement), SteamResult::Success);
        EXPECT_FALSE(m_Fake->UnlockedAchievements.contains(kKnownAchievement));
        EXPECT_EQ(m_Fake->StoreStatsCalls, storesAfterUnlock + 1);
    }

    // =================================================================================
    // Rich presence — validation happens in the manager so it is identical on every path.
    // =================================================================================

    TEST_F(SteamManagerTest, SetRichPresenceRoundTrips)
    {
        InitializeManager();

        EXPECT_EQ(SteamManager::SetRichPresence("status", "Exploring the caves"), SteamResult::Success);
        ASSERT_TRUE(m_Fake->RichPresence.contains("status"));
        EXPECT_EQ(m_Fake->RichPresence.at("status"), "Exploring the caves");
    }

    // Matches Steam's own semantics, so a script can clear one key without clearing all of them.
    TEST_F(SteamManagerTest, EmptyRichPresenceValueClearsThatKey)
    {
        InitializeManager();
        ASSERT_EQ(SteamManager::SetRichPresence("status", "something"), SteamResult::Success);

        EXPECT_EQ(SteamManager::SetRichPresence("status", ""), SteamResult::Success);
        EXPECT_FALSE(m_Fake->RichPresence.contains("status"));
    }

    TEST_F(SteamManagerTest, EmptyRichPresenceKeyIsRejected)
    {
        InitializeManager();
        EXPECT_EQ(SteamManager::SetRichPresence("", "value"), SteamResult::InvalidArgument);
    }

    // Reject rather than truncate. Steam silently drops over-long input, and a presence string
    // that quietly loses its tail is much harder to diagnose than a logged rejection.
    TEST_F(SteamManagerTest, OverlongRichPresenceIsRejectedNotTruncated)
    {
        InitializeManager();

        const std::string longKey(65, 'k');    // Steam's key limit is 64
        const std::string longValue(257, 'v'); // Steam's value limit is 256

        EXPECT_EQ(SteamManager::SetRichPresence(longKey, "ok"), SteamResult::InvalidArgument);
        EXPECT_EQ(SteamManager::SetRichPresence("status", longValue), SteamResult::InvalidArgument);
        EXPECT_TRUE(m_Fake->RichPresence.empty()) << "a rejected presence write reached the backend anyway";

        // Exactly at the limit must still be accepted — an off-by-one here silently costs a
        // character of every maximum-length presence string.
        const std::string maxKey(64, 'k');
        const std::string maxValue(256, 'v');
        EXPECT_EQ(SteamManager::SetRichPresence(maxKey, maxValue), SteamResult::Success);
    }

    TEST_F(SteamManagerTest, ClearRichPresenceReachesTheBackend)
    {
        InitializeManager();
        ASSERT_EQ(SteamManager::SetRichPresence("status", "something"), SteamResult::Success);

        SteamManager::ClearRichPresence();
        EXPECT_EQ(m_Fake->ClearRichPresenceCalls, 1u);
        EXPECT_TRUE(m_Fake->RichPresence.empty());
    }

    // =================================================================================
    // Overlay.
    // =================================================================================

    TEST_F(SteamManagerTest, OverlayStateIsReported)
    {
        InitializeManager();

        EXPECT_FALSE(SteamManager::IsOverlayActive());
        m_Fake->OverlayActive = true;
        EXPECT_TRUE(SteamManager::IsOverlayActive());
    }

    // =================================================================================
    // Cloud.
    // =================================================================================

    TEST_F(SteamManagerTest, CloudWriteReadRoundTrips)
    {
        InitializeManager();

        const std::string payload = "save-game-bytes";
        ASSERT_EQ(SteamManager::CloudWrite("slot0.sav", AsBytes(payload)), SteamResult::Success);
        EXPECT_TRUE(SteamManager::CloudExists("slot0.sav"));

        std::vector<u8> readBack;
        ASSERT_EQ(SteamManager::CloudRead("slot0.sav", readBack), SteamResult::Success);
        EXPECT_EQ(std::string(readBack.begin(), readBack.end()), payload);
    }

    // BOTH the account-wide and the per-app Cloud switch must be on. A game that ignores this
    // silently loses saves for every player who turned Cloud off, so the manager short-circuits
    // before the backend rather than relying on the backend to refuse.
    TEST_F(SteamManagerTest, CloudDisabledShortCircuitsEveryOperation)
    {
        InitializeManager();
        m_Fake->CloudEnabled = false;

        EXPECT_FALSE(SteamManager::IsCloudEnabled());
        EXPECT_EQ(SteamManager::CloudWrite("slot0.sav", AsBytes("data")), SteamResult::Unavailable);
        EXPECT_TRUE(m_Fake->CloudFiles.empty()) << "a write reached the backend with Cloud disabled";

        std::vector<u8> readBuffer;
        EXPECT_EQ(SteamManager::CloudRead("slot0.sav", readBuffer), SteamResult::Unavailable);
        EXPECT_FALSE(SteamManager::CloudExists("slot0.sav"));
        EXPECT_EQ(SteamManager::CloudDelete("slot0.sav"), SteamResult::Unavailable);
        EXPECT_TRUE(SteamManager::CloudEnumerate().empty());

        SteamCloudQuota quota;
        EXPECT_EQ(SteamManager::GetCloudQuota(quota), SteamResult::Unavailable);
    }

    TEST_F(SteamManagerTest, CloudReadOfMissingFileReportsNotFound)
    {
        InitializeManager();

        std::vector<u8> readBuffer;
        EXPECT_EQ(SteamManager::CloudRead("never-written.sav", readBuffer), SteamResult::NotFound);
    }

    // Parity pin between FakeSteamBackend and SteamworksBackend.
    //
    // A zero-length cloud file is legal but carries no save, and the REAL backend reports it as
    // NotFound rather than handing back an empty buffer that looks like a valid save. The fake
    // must agree: every test in this file is only evidence about production to the extent the two
    // behave alike, so a divergence here would quietly turn the whole suite into fiction.
    TEST_F(SteamManagerTest, EmptyCloudFileReadsAsNotFoundJustLikeTheRealBackend)
    {
        InitializeManager();
        m_Fake->CloudFiles["empty.sav"] = {};

        std::vector<u8> readBuffer;
        EXPECT_EQ(SteamManager::CloudRead("empty.sav", readBuffer), SteamResult::NotFound);
        EXPECT_TRUE(readBuffer.empty());
    }

    TEST_F(SteamManagerTest, CloudDeleteRemovesTheFile)
    {
        InitializeManager();
        ASSERT_EQ(SteamManager::CloudWrite("slot0.sav", AsBytes("data")), SteamResult::Success);

        EXPECT_EQ(SteamManager::CloudDelete("slot0.sav"), SteamResult::Success);
        EXPECT_FALSE(SteamManager::CloudExists("slot0.sav"));
        EXPECT_EQ(SteamManager::CloudDelete("slot0.sav"), SteamResult::NotFound);
    }

    TEST_F(SteamManagerTest, CloudEnumerateListsEveryWrittenFile)
    {
        InitializeManager();
        ASSERT_EQ(SteamManager::CloudWrite("a.sav", AsBytes("1")), SteamResult::Success);
        ASSERT_EQ(SteamManager::CloudWrite("b.sav", AsBytes("2")), SteamResult::Success);

        const std::vector<std::string> files = SteamManager::CloudEnumerate();
        EXPECT_EQ(files.size(), 2u);
        EXPECT_NE(std::find(files.begin(), files.end(), "a.sav"), files.end());
        EXPECT_NE(std::find(files.begin(), files.end(), "b.sav"), files.end());
    }

    TEST_F(SteamManagerTest, EmptyCloudFileNameIsRejected)
    {
        InitializeManager();

        EXPECT_EQ(SteamManager::CloudWrite("", AsBytes("data")), SteamResult::InvalidArgument);
        EXPECT_EQ(SteamManager::CloudDelete(""), SteamResult::InvalidArgument);
        EXPECT_FALSE(SteamManager::CloudExists(""));

        std::vector<u8> readBuffer;
        EXPECT_EQ(SteamManager::CloudRead("", readBuffer), SteamResult::InvalidArgument);
    }

    TEST_F(SteamManagerTest, CloudQuotaIsReported)
    {
        InitializeManager();
        m_Fake->Quota = SteamCloudQuota{ .TotalBytes = 4096, .AvailableBytes = 1024 };

        SteamCloudQuota quota;
        ASSERT_EQ(SteamManager::GetCloudQuota(quota), SteamResult::Success);
        EXPECT_EQ(quota.TotalBytes, 4096u);
        EXPECT_EQ(quota.AvailableBytes, 1024u);
        EXPECT_EQ(quota.UsedBytes(), 3072u);
    }

    // A backend reporting available > total would otherwise underflow the unsigned subtraction
    // into an enormous "used" figure.
    TEST_F(SteamManagerTest, CloudQuotaUsedBytesCannotUnderflow)
    {
        const SteamCloudQuota inconsistent{ .TotalBytes = 100, .AvailableBytes = 500 };
        EXPECT_EQ(inconsistent.UsedBytes(), 0u);
    }

    // =================================================================================
    // Identity.
    // =================================================================================

    TEST_F(SteamManagerTest, IdentityIsReportedWhenAvailable)
    {
        m_Fake->AppId = 480;
        m_Fake->PersonaName = "TestPlayer";
        InitializeManager();

        EXPECT_EQ(SteamManager::GetAppId(), 480u);
        EXPECT_EQ(SteamManager::GetPersonaName(), "TestPlayer");
    }

    // =================================================================================
    // Steam Input.
    // =================================================================================

    TEST_F(SteamManagerTest, InputIsUnavailableUntilInitializeSucceedsAndAControllerIsConnected)
    {
        // Steam itself up, Steam Input never brought up (default fake state) — degrades same as
        // no controller at all.
        InitializeManager();
        EXPECT_TRUE(SteamManager::IsAvailable());
        EXPECT_TRUE(SteamManager::IsInputAvailable()) << "InputInit() runs automatically as part of Initialize()";
        EXPECT_TRUE(SteamManager::GetConnectedControllers().empty());
    }

    TEST_F(SteamManagerTest, FailedInputInitLeavesInputUnavailableButSteamItselfFine)
    {
        m_Fake->InputInitSucceeds = false;
        InitializeManager();

        EXPECT_TRUE(SteamManager::IsAvailable()) << "a Steam Input failure must not take down the rest of Steam";
        EXPECT_FALSE(SteamManager::IsInputAvailable());
        EXPECT_TRUE(SteamManager::GetConnectedControllers().empty());
    }

    TEST_F(SteamManagerTest, ActivateActionSetReachesTheBackend)
    {
        InitializeManager();
        m_Fake->ConnectedControllers = { 1 };

        const auto controllers = SteamManager::GetConnectedControllers();
        ASSERT_EQ(controllers.size(), 1u);

        const auto actionSet = SteamManager::GetActionSetHandle("Gameplay");
        SteamManager::ActivateActionSet(controllers[0], actionSet);
        EXPECT_EQ(m_Fake->ActivateActionSetCalls, 1u);
    }

    TEST_F(SteamManagerTest, DigitalActionStatePassesThroughUnavailableWhenNotBound)
    {
        InitializeManager();
        const auto handle = SteamManager::GetDigitalActionHandle("Jump");

        // No state configured on the fake for this (controller, handle) pair — must report
        // Active=false ("not bound"), not fabricate a pressed state.
        const auto state = SteamManager::GetDigitalActionState(1, handle);
        EXPECT_FALSE(state.Active);
        EXPECT_FALSE(state.Pressed);
    }

    TEST_F(SteamManagerTest, DigitalAndAnalogActionStateReachTheBackend)
    {
        InitializeManager();
        m_Fake->DigitalActionHandles["Jump"] = 42;
        m_Fake->AnalogActionHandles["Move"] = 43;
        m_Fake->DigitalActionStates[{ 1, 42 }] = { .Pressed = true, .Active = true };
        m_Fake->AnalogActionStates[{ 1, 43 }] = { .X = 0.5f, .Y = -0.25f, .Active = true };

        const auto digitalHandle = SteamManager::GetDigitalActionHandle("Jump");
        const auto digitalState = SteamManager::GetDigitalActionState(1, digitalHandle);
        EXPECT_TRUE(digitalState.Active);
        EXPECT_TRUE(digitalState.Pressed);

        const auto analogHandle = SteamManager::GetAnalogActionHandle("Move");
        const auto analogState = SteamManager::GetAnalogActionState(1, analogHandle);
        EXPECT_TRUE(analogState.Active);
        EXPECT_FLOAT_EQ(analogState.X, 0.5f);
        EXPECT_FLOAT_EQ(analogState.Y, -0.25f);
    }

    TEST_F(SteamManagerTest, GlyphLookupReachesTheBackend)
    {
        InitializeManager();
        m_Fake->DigitalActionHandles["Jump"] = 42;
        m_Fake->GlyphLabels["Jump"] = "A Button";
        m_Fake->GlyphPngs["Jump"] = "/glyphs/a_button.png";

        const auto handle = SteamManager::GetDigitalActionHandle("Jump");
        EXPECT_EQ(SteamManager::GetGlyphLabelForDigitalAction(1, 0, handle), "A Button");
        EXPECT_EQ(SteamManager::GetGlyphPngForDigitalAction(1, 0, handle), "/glyphs/a_button.png");
    }

    TEST_F(SteamManagerTest, InputSurfaceNoOpsWhenSteamUnavailable)
    {
        // Never initialized — the whole surface must be inert, matching every other entry point.
        EXPECT_FALSE(SteamManager::IsInputAvailable());
        EXPECT_TRUE(SteamManager::GetConnectedControllers().empty());
        EXPECT_EQ(SteamManager::GetActionSetHandle("Gameplay"), OloEngine::kInvalidSteamInputActionSetHandle);
        EXPECT_EQ(SteamManager::GetDigitalActionHandle("Jump"), OloEngine::kInvalidSteamInputDigitalActionHandle);
        EXPECT_NO_THROW(SteamManager::ActivateActionSet(1, 1));
        EXPECT_TRUE(SteamManager::GetGlyphLabelForDigitalAction(1, 1, 1).empty());
    }

    // =================================================================================
    // SteamResult helpers.
    // =================================================================================

    TEST(SteamResultTest, SucceededTreatsAlreadySetAsSuccess)
    {
        EXPECT_TRUE(SteamSucceeded(SteamResult::Success));
        EXPECT_TRUE(SteamSucceeded(SteamResult::AlreadySet));

        EXPECT_FALSE(SteamSucceeded(SteamResult::Unavailable));
        EXPECT_FALSE(SteamSucceeded(SteamResult::NotFound));
        EXPECT_FALSE(SteamSucceeded(SteamResult::Failed));
        EXPECT_FALSE(SteamSucceeded(SteamResult::InvalidArgument));
    }

    TEST(SteamResultTest, EveryResultHasADistinctName)
    {
        // Guards against a new enumerator being added without a matching string, which would
        // otherwise silently log as "Unknown" in exactly the diagnostics someone is reading to
        // work out why Steam did nothing.
        const std::vector<SteamResult> all{ SteamResult::Success, SteamResult::Unavailable,
                                            SteamResult::NotFound, SteamResult::AlreadySet,
                                            SteamResult::Failed, SteamResult::InvalidArgument };

        std::set<std::string> names;
        for (const SteamResult result : all)
        {
            const std::string name = OloEngine::SteamResultToString(result);
            EXPECT_NE(name, "Unknown") << "a SteamResult is missing from SteamResultToString";
            names.insert(name);
        }
        EXPECT_EQ(names.size(), all.size()) << "two SteamResult values share a name";
    }
} // namespace

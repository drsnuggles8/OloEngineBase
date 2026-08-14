#pragma once

#include "OloEngine/Core/Base.h"

#include <string>
#include <string_view>
#include <vector>

// Control surface for the hand-written stub SDK (#644).
//
// ALWAYS INCLUDABLE AND ALWAYS LINKABLE, on every build configuration. When the stub is not the
// active SDK (a real-SDK build, or OLO_WITH_STEAM=0) every function here is a no-op and
// IsActive() returns false — so a test can include this unconditionally and skip cleanly:
//
//     if (!SteamStub::IsActive()) { GTEST_SKIP() << "stub SDK not compiled in"; }
//
// That is deliberate. Making this header conditional would push #if OLO_WITH_STEAM_STUB_SDK into
// every test file, which is exactly the sort of scattered conditional compilation that rots.
namespace OloEngine::SteamStub
{
    // True only when this build is using the hand-written stub SDK as its Steamworks
    // implementation, i.e. OLO_WITH_STEAM=1 && OLO_WITH_STEAM_STUB_SDK=1.
    [[nodiscard]] bool IsActive();

    // What the next SteamAPI_InitEx() should report. 0 == success; the non-zero values mirror
    // ESteamAPIInitResult (1 generic, 2 no client, 3 version mismatch).
    //
    // This is what lets CI exercise the single most important behaviour in the feature — that a
    // failed Steam init warns and continues rather than throwing — on a machine that has never
    // had Steam installed.
    void SetInitResult(int result);

    // Register an achievement id the stub should recognise. Ids not registered here report
    // NotFound, which is how the "unknown achievement" path gets tested.
    void RegisterAchievement(std::string_view achievementId);

    // Force an achievement to the unlocked state without going through the engine, so a test can
    // set up the "already unlocked" precondition that the dedup logic keys on.
    void ForceAchievementUnlocked(std::string_view achievementId, bool unlocked);

    [[nodiscard]] bool IsAchievementUnlocked(std::string_view achievementId);

    // How many times StoreStats() has been called. The dedup test asserts this does NOT grow on
    // a repeat unlock — the whole point of the dedup is avoiding a network store per frame.
    [[nodiscard]] u32 GetStoreStatsCallCount();

    // Overlay display state. Queues a transition; the engine does not observe it until
    // SteamAPI_RunCallbacks delivers the event, so a test must pump to see the change — which is
    // what puts the real callback path under test rather than a directly-poked flag.
    void SetOverlayActive(bool active);

    // True while the engine has an overlay callback registered with the stub.
    //
    // Exists so a test can assert REGISTRATION/UNREGISTRATION directly. Trying to observe it
    // through behaviour instead does not work: after SteamManager::Shutdown the backend is gone,
    // so RunCallbacks returns before reaching any dispatch and IsOverlayActive() is false no
    // matter what Unregister() did. A test written that way passes whether or not unregistration
    // happens at all — it looks like coverage and is worth nothing.
    [[nodiscard]] bool IsOverlayCallbackRegistered();

    // Cloud switches. Both must be true for writes to land, mirroring Steam's account-wide and
    // per-app settings.
    void SetCloudEnabled(bool accountEnabled, bool appEnabled);

    // Total/available cloud quota the stub reports.
    void SetCloudQuota(u64 totalBytes, u64 availableBytes);

    [[nodiscard]] bool CloudHasFile(std::string_view name);

    // Wipe all stub state back to defaults: init succeeds, no achievements, cloud on and empty,
    // overlay closed. Tests must call this in SetUp so they cannot leak state into each other.
    void Reset();
} // namespace OloEngine::SteamStub

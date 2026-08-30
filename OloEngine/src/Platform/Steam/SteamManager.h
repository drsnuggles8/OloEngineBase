#pragma once

#include "OloEngine/Core/Base.h"
#include "Platform/Steam/SteamTypes.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine
{
    class ISteamBackend;

    // Process-level Steam platform service (#644): achievements, rich presence, overlay state
    // and Steam Cloud.
    //
    // A STATIC MANAGER, matching NetworkManager / SaveGameManager / InputActionManager, and
    // deliberately NOT an ECS component: achievements are a property of the process and the
    // signed-in user, not of an entity. That choice is what keeps this feature clear of the
    // whole hand-maintained cross-binding touch-point set in CLAUDE.md (AllComponents tuple,
    // SceneSerializer blocks, SaveGame capture/restore, OnComponentAdded specializations). Do
    // not "promote" any of this to a component without reading that section first.
    //
    // GRACEFUL DEGRADATION IS THE CONTRACT. Two independent failures are covered:
    //
    //   Variant B, compile time  — OLO_WITH_STEAM=0. CreateSteamBackend() hands back a
    //                              NullSteamBackend, so every call below is a defined no-op.
    //   Variant A, run time      — OLO_WITH_STEAM=1 but SteamAPI_Init() returned false (client
    //                              not running, no App ID, offline). Initialize() logs an
    //                              actionable warning, sets the availability flag false, and
    //                              RETURNS — it does not throw.
    //
    // Initialize() therefore never fails in a way the caller must handle. This is on purpose and
    // is the single most important behaviour in the feature: Application.cpp throws
    // std::runtime_error when AudioEngine or NetworkManager fail to init, and Steam must never
    // join that club — a missing Steam client must not stop the engine starting. There is a test
    // asserting exactly that, and it runs in CI on both the OFF path and (via the stub SDK) the
    // ON path.
    //
    // Threading: game thread only.
    class SteamManager
    {
      public:
        // --- lifecycle ---------------------------------------------------------------------

        // Bring Steam up. Safe to call when Steam is absent, disabled or broken; never throws.
        // Idempotent — a second call without an intervening Shutdown() is a no-op.
        static void Initialize();

        // Tear Steam down. Safe to call when Initialize() was never called or failed, which
        // matters because Application has TWO shutdown paths (the exception path and the normal
        // destructor path) and both must reach this.
        static void Shutdown();

        // Drain Steam's callback queue. Call once per frame from the game loop, before the
        // frame's task drain so a callback that enqueues a game-thread task lands the same frame.
        static void RunCallbacks();

        // True when Steam is compiled in AND the session initialised successfully. Everything
        // below no-ops and returns Unavailable when this is false, so callers are not required
        // to check it first — check it only to hide UI or skip work.
        [[nodiscard]] static bool IsAvailable();

        // --- identity ----------------------------------------------------------------------

        // The running App ID (480 = Spacewar during development), or 0 when unavailable.
        [[nodiscard]] static u32 GetAppId();

        // The signed-in user's display name, or empty when unavailable.
        [[nodiscard]] static std::string GetPersonaName();

        // --- achievements ------------------------------------------------------------------

        // Unlock an achievement and flush it to Steam so the overlay toast fires.
        //
        // Returns AlreadySet (not Success) when the achievement was already unlocked, and skips
        // the store entirely in that case — Steam tolerates redundant sets, but a game that
        // unlocks from an OnUpdate would otherwise issue a network store every frame forever.
        static SteamResult UnlockAchievement(std::string_view achievementId);

        // Clear an achievement. Development aid — ship builds should not call this.
        static SteamResult ClearAchievement(std::string_view achievementId);

        [[nodiscard]] static bool IsAchievementUnlocked(std::string_view achievementId);

        // Explicitly flush pending stat changes. UnlockAchievement already does this; this is
        // for callers batching several changes.
        static SteamResult StoreStats();

        // --- rich presence -----------------------------------------------------------------

        // Set a rich-presence key. An empty value clears that key, matching Steam's own
        // semantics. Keys and values are length-capped by Steam; over-long input is rejected
        // with InvalidArgument rather than silently truncated.
        static SteamResult SetRichPresence(std::string_view key, std::string_view value);

        // Clear every rich-presence key for this user.
        static void ClearRichPresence();

        // --- overlay -----------------------------------------------------------------------

        // True while the Steam overlay is up. Games typically pause on the rising edge.
        //
        // The overlay hooks the swapchain, and this engine has TWO RHI backends. This is
        // verified on OpenGL only; Vulkan overlay behaviour is untested here and no claim is
        // made about it.
        [[nodiscard]] static bool IsOverlayActive();

        // --- cloud -------------------------------------------------------------------------

        [[nodiscard]] static bool IsCloudEnabled();

        static SteamResult CloudWrite(std::string_view name, std::span<const u8> data);
        static SteamResult CloudRead(std::string_view name, std::vector<u8>& outData);
        [[nodiscard]] static bool CloudExists(std::string_view name);
        static SteamResult CloudDelete(std::string_view name);
        [[nodiscard]] static std::vector<std::string> CloudEnumerate();
        static SteamResult GetCloudQuota(SteamCloudQuota& outQuota);

        // --- input -----------------------------------------------------------------------
        //
        // Steam Input is brought up and torn down as part of Initialize()/Shutdown() and
        // pumped as part of RunCallbacks() — see those methods — so there is no separate
        // Init/Shutdown/RunFrame here. IsInputAvailable() can be false while IsAvailable() is
        // true (Steam is up but Steam Input failed to init, or nothing is connected), so
        // callers must check it before relying on any of the below.
        //
        // See docs/agent-rules/steamworks-platform-integration.md §11 for how
        // InputActionManager drives this seam to implement "Steam Input wins when available".

        [[nodiscard]] static bool IsInputAvailable();

        [[nodiscard]] static std::vector<SteamInputHandle> GetConnectedControllers();

        [[nodiscard]] static SteamInputActionSetHandle GetActionSetHandle(std::string_view actionSetName);
        static void ActivateActionSet(SteamInputHandle controller, SteamInputActionSetHandle actionSet);

        [[nodiscard]] static SteamInputDigitalActionHandle GetDigitalActionHandle(std::string_view actionName);
        [[nodiscard]] static SteamInputDigitalActionState GetDigitalActionState(SteamInputHandle controller,
                                                                                SteamInputDigitalActionHandle action);

        [[nodiscard]] static SteamInputAnalogActionHandle GetAnalogActionHandle(std::string_view actionName);
        [[nodiscard]] static SteamInputAnalogActionState GetAnalogActionState(SteamInputHandle controller,
                                                                              SteamInputAnalogActionHandle action);

        [[nodiscard]] static std::string GetGlyphLabelForDigitalAction(SteamInputHandle controller, SteamInputActionSetHandle actionSet,
                                                                       SteamInputDigitalActionHandle action);
        [[nodiscard]] static std::string GetGlyphLabelForAnalogAction(SteamInputHandle controller, SteamInputActionSetHandle actionSet,
                                                                      SteamInputAnalogActionHandle action);
        [[nodiscard]] static std::string GetGlyphPngForDigitalAction(SteamInputHandle controller, SteamInputActionSetHandle actionSet,
                                                                     SteamInputDigitalActionHandle action);
        [[nodiscard]] static std::string GetGlyphPngForAnalogAction(SteamInputHandle controller, SteamInputActionSetHandle actionSet,
                                                                    SteamInputAnalogActionHandle action);

        // --- test seam ---------------------------------------------------------------------

        // Swap in a fake backend. Takes ownership; pass nullptr to restore the real one.
        //
        // This is what makes the interesting logic testable with no Steam client: the dedup in
        // UnlockAchievement, the validation in SetRichPresence, and (later) the cloud/local
        // reconciliation in the SaveGame hooks. Tests must pair this with ResetForTesting().
        static void SetBackendForTesting(Scope<ISteamBackend> backend);

        // Drop any test backend and return to the uninitialised state.
        static void ResetForTesting();

      private:
        SteamManager() = delete;
    };
} // namespace OloEngine

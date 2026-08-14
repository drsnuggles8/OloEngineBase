#pragma once

#include "OloEngine/Core/Base.h"
#include "Platform/Steam/SteamTypes.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine
{
    // THE seam for Steamworks (#644).
    //
    // Every Steamworks call the engine makes goes through this interface, and exactly ONE
    // translation unit — Platform/Steam/SteamworksBackend.cpp — includes a Valve header. That is
    // deliberate and load-bearing:
    //
    //   * it makes the logic that actually has bugs (already-unlocked dedup, cloud/local
    //     reconciliation ordering, save-slot union) testable against a fake with no Steam client,
    //     no App ID and no Valve account;
    //   * it confines the code CI can never compile against the real SDK to one file, which is
    //     what the hand-written stub SDK (OLO_WITH_STEAM_STUB_SDK) has to stand in for;
    //   * it keeps Valve types out of every other header, so OLO_WITH_STEAM=0 changes nothing
    //     about the engine's link surface.
    //
    // If you find yourself including <steam/...> anywhere else, that is the bug.
    //
    // Threading: all methods are called from the game thread only. Nothing here is thread-safe,
    // and nothing here should be called from a worker — Steamworks itself is not thread-safe in
    // the way an engine worker pool would need.
    class ISteamBackend
    {
      public:
        virtual ~ISteamBackend() = default;

        ISteamBackend() = default;
        ISteamBackend(const ISteamBackend&) = delete;
        ISteamBackend& operator=(const ISteamBackend&) = delete;
        ISteamBackend(ISteamBackend&&) = delete;
        ISteamBackend& operator=(ISteamBackend&&) = delete;

        // --- lifecycle ---------------------------------------------------------------------

        // Bring the backend up. MUST NOT throw and MUST NOT be fatal: returning false means
        // "Steam is not available this session", which is a completely normal outcome (client
        // not running, no App ID, offline) and must never stop the engine starting.
        [[nodiscard]] virtual bool Initialize() = 0;

        virtual void Shutdown() = 0;

        // Drain Steam's callback queue. Called once per frame from the game loop.
        virtual void RunCallbacks() = 0;

        // True once Initialize() has succeeded and the session is usable. Every other call below
        // must no-op and return Unavailable when this is false.
        [[nodiscard]] virtual bool IsAvailable() const = 0;

        // --- identity ----------------------------------------------------------------------

        [[nodiscard]] virtual u32 GetAppId() const = 0;
        [[nodiscard]] virtual std::string GetPersonaName() const = 0;

        // --- achievements / stats ----------------------------------------------------------

        // Set the achievement flag locally. NOTE this does NOT push to Steam's servers — the
        // overlay notification only fires on StoreStats(). The manager owns that sequencing;
        // backends should not call StoreStats() implicitly.
        virtual SteamResult SetAchievement(std::string_view achievementId) = 0;

        // Clear a previously-unlocked achievement. Development aid; Steam permits it.
        virtual SteamResult ClearAchievement(std::string_view achievementId) = 0;

        // Query the unlocked flag. Returns Unavailable/NotFound without touching outUnlocked.
        virtual SteamResult GetAchievementUnlocked(std::string_view achievementId, bool& outUnlocked) const = 0;

        // Flush pending achievement/stat changes to Steam. This is what makes the overlay toast
        // appear, so it must be called after SetAchievement.
        virtual SteamResult StoreStats() = 0;

        // --- rich presence -----------------------------------------------------------------

        virtual SteamResult SetRichPresence(std::string_view key, std::string_view value) = 0;
        virtual void ClearRichPresence() = 0;

        // --- overlay -----------------------------------------------------------------------

        // True while the Steam overlay is displayed over the game. Games use this to auto-pause.
        [[nodiscard]] virtual bool IsOverlayActive() const = 0;

        // --- cloud -------------------------------------------------------------------------

        // Cloud has TWO switches: the account-wide setting and the per-app one. Both must be on
        // for a write to land, and a game that ignores this silently loses saves.
        [[nodiscard]] virtual bool IsCloudEnabled() const = 0;

        virtual SteamResult CloudWrite(std::string_view name, std::span<const u8> data) = 0;
        virtual SteamResult CloudRead(std::string_view name, std::vector<u8>& outData) const = 0;
        [[nodiscard]] virtual bool CloudExists(std::string_view name) const = 0;
        virtual SteamResult CloudDelete(std::string_view name) = 0;
        [[nodiscard]] virtual std::vector<std::string> CloudEnumerate() const = 0;
        virtual SteamResult GetCloudQuota(SteamCloudQuota& outQuota) const = 0;
    };

    // The always-unavailable backend.
    //
    // Used whenever Steam cannot work: OLO_WITH_STEAM=0 (not compiled in), or an ON build whose
    // SteamAPI_Init() failed. Every call is a defined no-op returning Unavailable, which is what
    // lets the rest of the engine call Steam unconditionally without guarding every site.
    //
    // Also the base for the test fake, so the fake only overrides what a given test cares about.
    class NullSteamBackend : public ISteamBackend
    {
      public:
        [[nodiscard]] bool Initialize() override
        {
            return false;
        }
        void Shutdown() override {}
        void RunCallbacks() override {}
        [[nodiscard]] bool IsAvailable() const override
        {
            return false;
        }

        [[nodiscard]] u32 GetAppId() const override
        {
            return 0;
        }
        [[nodiscard]] std::string GetPersonaName() const override
        {
            return {};
        }

        SteamResult SetAchievement(std::string_view) override
        {
            return SteamResult::Unavailable;
        }
        SteamResult ClearAchievement(std::string_view) override
        {
            return SteamResult::Unavailable;
        }
        SteamResult GetAchievementUnlocked(std::string_view, bool&) const override
        {
            return SteamResult::Unavailable;
        }
        SteamResult StoreStats() override
        {
            return SteamResult::Unavailable;
        }

        SteamResult SetRichPresence(std::string_view, std::string_view) override
        {
            return SteamResult::Unavailable;
        }
        void ClearRichPresence() override {}

        [[nodiscard]] bool IsOverlayActive() const override
        {
            return false;
        }

        [[nodiscard]] bool IsCloudEnabled() const override
        {
            return false;
        }
        SteamResult CloudWrite(std::string_view, std::span<const u8>) override
        {
            return SteamResult::Unavailable;
        }
        SteamResult CloudRead(std::string_view, std::vector<u8>&) const override
        {
            return SteamResult::Unavailable;
        }
        [[nodiscard]] bool CloudExists(std::string_view) const override
        {
            return false;
        }
        SteamResult CloudDelete(std::string_view) override
        {
            return SteamResult::Unavailable;
        }
        [[nodiscard]] std::vector<std::string> CloudEnumerate() const override
        {
            return {};
        }
        SteamResult GetCloudQuota(SteamCloudQuota&) const override
        {
            return SteamResult::Unavailable;
        }
    };

    // Produce the backend for this build.
    //
    // Defined in SteamworksBackend.cpp on BOTH sides of the OLO_WITH_STEAM #if — the real
    // Valve-backed implementation when compiled in, a NullSteamBackend when not. That is the
    // "complete stub set" half of the two-variant degradation contract (the runtime half is
    // Initialize() returning false), and it is why the link surface is identical either way.
    //
    // Never returns null.
    [[nodiscard]] Scope<ISteamBackend> CreateSteamBackend();
} // namespace OloEngine

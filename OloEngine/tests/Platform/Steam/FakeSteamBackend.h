#pragma once

#include "Platform/Steam/SteamBackend.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace OloEngine::Testing
{
    // In-memory ISteamBackend for tests.
    //
    // This is the payoff of putting every Steamworks call behind one interface: the logic that
    // actually has bugs — the already-unlocked dedup, the rich-presence validation, cloud/local
    // reconciliation — becomes fully unit-testable with no Steam client, no App ID and no Valve
    // account, on any machine including a headless CI runner.
    //
    // Distinct from the STUB SDK, which serves a different purpose: the stub stands in for
    // Valve's headers so the one Valve-calling TU compiles and links in CI. This fake stands in
    // for the backend so the manager's logic can be driven directly. They are complementary, not
    // duplicates — see the comment at the top of SteamBackend.h.
    class FakeSteamBackend final : public ISteamBackend
    {
      public:
        // --- knobs the tests set ------------------------------------------------------------

        // When false, Initialize() reports failure — the "Steam client not running" case.
        bool InitializeSucceeds = true;

        bool CloudEnabled = true;
        bool OverlayActive = false;
        u32 AppId = 480;
        std::string PersonaName = "FakeUser";
        SteamCloudQuota Quota{ .TotalBytes = 1024 * 1024, .AvailableBytes = 1024 * 1024 };

        // Achievement ids the fake pretends Steam knows about. Anything else reports NotFound,
        // which is how the unknown-id path gets exercised.
        std::set<std::string, std::less<>> KnownAchievements;
        std::set<std::string, std::less<>> UnlockedAchievements;

        std::map<std::string, std::vector<u8>, std::less<>> CloudFiles;
        std::map<std::string, std::string, std::less<>> RichPresence;

        // --- observations the tests assert on -----------------------------------------------

        u32 InitializeCalls = 0;
        u32 ShutdownCalls = 0;
        u32 RunCallbacksCalls = 0;
        u32 SetAchievementCalls = 0;
        u32 StoreStatsCalls = 0;
        u32 ClearRichPresenceCalls = 0;

        // Make a specific operation fail, to prove the manager surfaces rather than swallows it.
        bool SetAchievementFails = false;
        bool StoreStatsFails = false;
        bool CloudWriteFails = false;

        // --- ISteamBackend ------------------------------------------------------------------

        [[nodiscard]] bool Initialize() override
        {
            ++InitializeCalls;
            m_Available = InitializeSucceeds;
            return m_Available;
        }

        void Shutdown() override
        {
            ++ShutdownCalls;
            m_Available = false;
        }

        void RunCallbacks() override
        {
            ++RunCallbacksCalls;
        }

        [[nodiscard]] bool IsAvailable() const override
        {
            return m_Available;
        }

        [[nodiscard]] u32 GetAppId() const override
        {
            return AppId;
        }
        [[nodiscard]] std::string GetPersonaName() const override
        {
            return PersonaName;
        }

        SteamResult SetAchievement(std::string_view achievementId) override
        {
            ++SetAchievementCalls;
            if (SetAchievementFails)
            {
                return SteamResult::Failed;
            }
            if (!KnownAchievements.contains(achievementId))
            {
                return SteamResult::NotFound;
            }
            UnlockedAchievements.emplace(achievementId);
            return SteamResult::Success;
        }

        SteamResult ClearAchievement(std::string_view achievementId) override
        {
            if (!KnownAchievements.contains(achievementId))
            {
                return SteamResult::NotFound;
            }
            UnlockedAchievements.erase(std::string{ achievementId });
            return SteamResult::Success;
        }

        SteamResult GetAchievementUnlocked(std::string_view achievementId, bool& outUnlocked) const override
        {
            if (!KnownAchievements.contains(achievementId))
            {
                return SteamResult::NotFound;
            }
            outUnlocked = UnlockedAchievements.contains(achievementId);
            return SteamResult::Success;
        }

        SteamResult StoreStats() override
        {
            ++StoreStatsCalls;
            return StoreStatsFails ? SteamResult::Failed : SteamResult::Success;
        }

        SteamResult SetRichPresence(std::string_view key, std::string_view value) override
        {
            if (value.empty())
            {
                RichPresence.erase(std::string{ key });
            }
            else
            {
                RichPresence[std::string{ key }] = std::string{ value };
            }
            return SteamResult::Success;
        }

        void ClearRichPresence() override
        {
            ++ClearRichPresenceCalls;
            RichPresence.clear();
        }

        [[nodiscard]] bool IsOverlayActive() const override
        {
            return OverlayActive;
        }

        [[nodiscard]] bool IsCloudEnabled() const override
        {
            return CloudEnabled;
        }

        SteamResult CloudWrite(std::string_view name, std::span<const u8> data) override
        {
            if (CloudWriteFails)
            {
                return SteamResult::Failed;
            }
            CloudFiles[std::string{ name }] = std::vector<u8>(data.begin(), data.end());
            return SteamResult::Success;
        }

        SteamResult CloudRead(std::string_view name, std::vector<u8>& outData) const override
        {
            const auto found = CloudFiles.find(name);
            if (found == CloudFiles.end())
            {
                return SteamResult::NotFound;
            }
            outData = found->second;
            return SteamResult::Success;
        }

        [[nodiscard]] bool CloudExists(std::string_view name) const override
        {
            return CloudFiles.contains(name);
        }

        SteamResult CloudDelete(std::string_view name) override
        {
            return CloudFiles.erase(std::string{ name }) > 0 ? SteamResult::Success : SteamResult::NotFound;
        }

        [[nodiscard]] std::vector<std::string> CloudEnumerate() const override
        {
            std::vector<std::string> names;
            names.reserve(CloudFiles.size());
            for (const auto& [name, unused] : CloudFiles)
            {
                names.push_back(name);
            }
            return names;
        }

        SteamResult GetCloudQuota(SteamCloudQuota& outQuota) const override
        {
            outQuota = Quota;
            return SteamResult::Success;
        }

      private:
        bool m_Available = false;
    };
} // namespace OloEngine::Testing

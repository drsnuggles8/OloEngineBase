#pragma once

#include "Platform/Steam/SteamBackend.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
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
        bool CloudDeleteFails = false;

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

            // Mirror SteamworksBackend::CloudRead: a zero-length cloud file is legal but carries
            // no save, and is reported as absent rather than as an empty-but-valid buffer.
            //
            // This parity matters more than it looks. The whole value of this fake is that tests
            // written against it also describe the real backend; anywhere the two disagree, the
            // tests quietly stop being evidence about production. Keep them in step.
            if (found->second.empty())
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
            if (CloudDeleteFails)
            {
                return SteamResult::Failed; // leaves the entry in place, as a real failure would
            }
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

        // --- Steam Input ----------------------------------------------------------------

        // When false, InputInit() reports failure — Steam is up but Steam Input isn't, the
        // same shape as a stats/achievements-only integration that skips it.
        bool InputInitSucceeds = true;
        bool InputInitialized = false;

        std::vector<SteamInputHandle> ConnectedControllers;
        std::map<std::string, SteamInputDigitalActionHandle, std::less<>> DigitalActionHandles;
        std::map<std::string, SteamInputAnalogActionHandle, std::less<>> AnalogActionHandles;
        std::map<std::pair<SteamInputHandle, SteamInputDigitalActionHandle>, SteamInputDigitalActionState> DigitalActionStates;
        std::map<std::pair<SteamInputHandle, SteamInputAnalogActionHandle>, SteamInputAnalogActionState> AnalogActionStates;
        std::map<std::pair<SteamInputHandle, SteamInputActionSetHandle>, std::string> ActiveActionSetNames;
        std::map<std::string, std::string, std::less<>> GlyphLabels;
        std::map<std::string, std::string, std::less<>> GlyphPngs;

        u32 ActivateActionSetCalls = 0;
        mutable SteamInputActionSetHandle NextHandle = 1;

        bool InputInit() override
        {
            InputInitialized = InputInitSucceeds;
            return InputInitialized;
        }
        void InputShutdown() override
        {
            InputInitialized = false;
        }
        void InputRunFrame() override {}
        [[nodiscard]] bool IsInputAvailable() const override
        {
            return InputInitialized;
        }
        [[nodiscard]] std::vector<SteamInputHandle> GetConnectedControllers() const override
        {
            return ConnectedControllers;
        }

        [[nodiscard]] SteamInputActionSetHandle GetActionSetHandle(std::string_view actionSetName) const override
        {
            // The fake doesn't validate against a manifest, matching the stub SDK's contract
            // (see StubState::ActionSetHandles) — any name gets a handle. Interned by name
            // (first-sight assignment), also matching the stub's InternHandle: a caller that
            // asks for the same action-set name twice must get the same handle back.
            if (const auto found = m_ActionSetHandles.find(actionSetName); found != m_ActionSetHandles.end())
            {
                return found->second;
            }
            const SteamInputActionSetHandle handle = NextHandle++;
            m_ActionSetHandles.emplace(std::string{ actionSetName }, handle);
            return handle;
        }
        void ActivateActionSet(SteamInputHandle controller, SteamInputActionSetHandle actionSet) override
        {
            ++ActivateActionSetCalls;
            ActiveActionSetNames[{ controller, actionSet }] = "activated";
        }

        [[nodiscard]] SteamInputDigitalActionHandle GetDigitalActionHandle(std::string_view actionName) const override
        {
            auto it = DigitalActionHandles.find(actionName);
            return it != DigitalActionHandles.end() ? it->second : kInvalidSteamInputDigitalActionHandle;
        }
        [[nodiscard]] SteamInputDigitalActionState GetDigitalActionState(SteamInputHandle controller,
                                                                         SteamInputDigitalActionHandle action) const override
        {
            auto it = DigitalActionStates.find({ controller, action });
            return it != DigitalActionStates.end() ? it->second : SteamInputDigitalActionState{};
        }

        [[nodiscard]] SteamInputAnalogActionHandle GetAnalogActionHandle(std::string_view actionName) const override
        {
            auto it = AnalogActionHandles.find(actionName);
            return it != AnalogActionHandles.end() ? it->second : kInvalidSteamInputAnalogActionHandle;
        }
        [[nodiscard]] SteamInputAnalogActionState GetAnalogActionState(SteamInputHandle controller,
                                                                       SteamInputAnalogActionHandle action) const override
        {
            auto it = AnalogActionStates.find({ controller, action });
            return it != AnalogActionStates.end() ? it->second : SteamInputAnalogActionState{};
        }

        [[nodiscard]] std::string GetGlyphLabelForDigitalAction(SteamInputHandle, SteamInputActionSetHandle,
                                                                SteamInputDigitalActionHandle action) const override
        {
            return GlyphLabelFor(FindDigitalActionName(action));
        }
        [[nodiscard]] std::string GetGlyphLabelForAnalogAction(SteamInputHandle, SteamInputActionSetHandle,
                                                               SteamInputAnalogActionHandle action) const override
        {
            return GlyphLabelFor(FindAnalogActionName(action));
        }
        [[nodiscard]] std::string GetGlyphPngForDigitalAction(SteamInputHandle, SteamInputActionSetHandle,
                                                              SteamInputDigitalActionHandle action) const override
        {
            return GlyphPngFor(FindDigitalActionName(action));
        }
        [[nodiscard]] std::string GetGlyphPngForAnalogAction(SteamInputHandle, SteamInputActionSetHandle,
                                                             SteamInputAnalogActionHandle action) const override
        {
            return GlyphPngFor(FindAnalogActionName(action));
        }

      private:
        [[nodiscard]] std::string FindDigitalActionName(SteamInputDigitalActionHandle action) const
        {
            for (const auto& [name, handle] : DigitalActionHandles)
            {
                if (handle == action)
                {
                    return name;
                }
            }
            return {};
        }
        [[nodiscard]] std::string FindAnalogActionName(SteamInputAnalogActionHandle action) const
        {
            for (const auto& [name, handle] : AnalogActionHandles)
            {
                if (handle == action)
                {
                    return name;
                }
            }
            return {};
        }
        [[nodiscard]] std::string GlyphLabelFor(const std::string& name) const
        {
            auto it = GlyphLabels.find(name);
            return it != GlyphLabels.end() ? it->second : std::string{};
        }
        [[nodiscard]] std::string GlyphPngFor(const std::string& name) const
        {
            auto it = GlyphPngs.find(name);
            return it != GlyphPngs.end() ? it->second : std::string{};
        }

        bool m_Available = false;

        // Backs GetActionSetHandle's first-sight interning above.
        mutable std::map<std::string, SteamInputActionSetHandle, std::less<>> m_ActionSetHandles;
    };
} // namespace OloEngine::Testing

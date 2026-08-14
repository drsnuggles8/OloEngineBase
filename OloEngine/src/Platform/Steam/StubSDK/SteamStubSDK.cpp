#include "OloEnginePCH.h"
#include "Platform/Steam/StubSDK/SteamStubControl.h"

// =====================================================================================
// Implementation of the hand-written stub SDK (#644).
//
// Provides the symbols declared in StubSDK/public/steam/steam_api.h so CI can COMPILE AND LINK the
// OLO_WITH_STEAM=ON path without Valve's proprietary SDK, which can never reach a public repo.
// Nothing here is derived from the SDK; it is an in-memory fake with canned behaviour.
//
// This TU is ALWAYS listed in the source list and self-guards, matching the convention used by
// the Vulkan backend and the OLO_WITH_* import translators. When the stub is not active it still
// provides the SteamStub:: control functions as no-ops, so tests link and skip cleanly on every
// configuration rather than needing #if guards of their own.
// =====================================================================================

#if OLO_WITH_STEAM && OLO_WITH_STEAM_STUB_SDK

// "public/" prefix and the mirrored public/steam/ layout are deliberate — see the long comment
// at the include in SteamworksBackend.cpp. Short version: putting a directory with a `steam/`
// child on the engine include path hijacks GameNetworkingSockets' headers engine-wide.
#include "public/steam/steam_api.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
    // All stub state. A single struct so Reset() cannot forget a field — the classic way a
    // fake leaks state between tests is an added member that the reset function never learns
    // about.
    struct StubState
    {
        int InitResult = 0; // k_ESteamAPIInitResult_OK
        bool Initialized = false;

        std::set<std::string, std::less<>> KnownAchievements;
        std::set<std::string, std::less<>> UnlockedAchievements;
        u32 StoreStatsCalls = 0;

        std::string PersonaName = "StubUser";
        u32 AppId = 480; // Spacewar, the same id development uses

        bool OverlayActive = false;

        bool CloudAccountEnabled = true;
        bool CloudAppEnabled = true;
        u64 CloudTotalBytes = 100ull * 1024ull * 1024ull;
        u64 CloudAvailableBytes = 100ull * 1024ull * 1024ull;
        std::map<std::string, std::vector<u8>, std::less<>> CloudFiles;

        // Backing storage for the const char* that GetFileNameAndSize hands back. The real API
        // returns a pointer into Steam-owned memory valid until the next call; matching that
        // contract with a member keeps callers honest about not retaining it.
        std::string LastEnumeratedName;
    };

    StubState& State()
    {
        static StubState s_State;
        return s_State;
    }

    // The interface objects are stateless handles onto State(); one static of each is enough.
    ISteamUserStats s_UserStats;
    ISteamFriends s_Friends;
    ISteamUtils s_Utils;
    ISteamRemoteStorage s_RemoteStorage;
} // namespace

// --- init / shutdown --------------------------------------------------------------------

ESteamAPIInitResult SteamAPI_InitEx(SteamErrMsg* pOutErrMsg)
{
    StubState& state = State();

    if (state.InitResult != 0)
    {
        if (pOutErrMsg)
        {
            // The engine logs this verbatim, so make it obviously synthetic — a real failure
            // message must never be confused with a stubbed one when reading a CI log.
            //
            // snprintf rather than strncpy: it always NUL-terminates (strncpy does not when the
            // source fills the buffer) and avoids MSVC's C4996 deprecation warning without
            // reaching for _CRT_SECURE_NO_WARNINGS.
            std::snprintf(*pOutErrMsg, k_cchMaxSteamErrMsg, "%s",
                          "stub SDK: SteamAPI_InitEx forced to fail by SteamStub::SetInitResult");
        }
        return static_cast<ESteamAPIInitResult>(state.InitResult);
    }

    state.Initialized = true;
    if (pOutErrMsg)
    {
        (*pOutErrMsg)[0] = '\0';
    }
    return k_ESteamAPIInitResult_OK;
}

void SteamAPI_Shutdown()
{
    State().Initialized = false;
}

void SteamAPI_RunCallbacks()
{
    // No dispatch: with no Steam client there are no events to deliver. Present so the engine's
    // per-frame pump links and runs.
}

// --- ISteamUserStats --------------------------------------------------------------------

bool ISteamUserStats::GetAchievement(const char* pchName, bool* pbAchieved)
{
    if (!pchName || !pbAchieved)
    {
        return false;
    }
    StubState& state = State();

    // An unregistered id reports false, exactly as Steam does for an achievement that is not
    // defined for the app. That is what drives the engine's NotFound path.
    if (!state.KnownAchievements.contains(pchName))
    {
        return false;
    }
    *pbAchieved = state.UnlockedAchievements.contains(pchName);
    return true;
}

bool ISteamUserStats::SetAchievement(const char* pchName)
{
    if (!pchName)
    {
        return false;
    }
    StubState& state = State();
    if (!state.KnownAchievements.contains(pchName))
    {
        return false;
    }
    state.UnlockedAchievements.emplace(pchName);
    return true;
}

bool ISteamUserStats::ClearAchievement(const char* pchName)
{
    if (!pchName)
    {
        return false;
    }
    StubState& state = State();
    if (!state.KnownAchievements.contains(pchName))
    {
        return false;
    }
    state.UnlockedAchievements.erase(std::string{ pchName });
    return true;
}

bool ISteamUserStats::StoreStats()
{
    ++State().StoreStatsCalls;
    return true;
}

// --- ISteamFriends ----------------------------------------------------------------------

const char* ISteamFriends::GetPersonaName()
{
    return State().PersonaName.c_str();
}

bool ISteamFriends::SetRichPresence(const char* pchKey, const char* pchValue)
{
    return pchKey != nullptr && pchValue != nullptr;
}

void ISteamFriends::ClearRichPresence() {}

// --- ISteamUtils ------------------------------------------------------------------------

uint32 ISteamUtils::GetAppID()
{
    return static_cast<uint32>(State().AppId);
}

bool ISteamUtils::IsOverlayEnabled()
{
    return true;
}

// --- ISteamRemoteStorage ----------------------------------------------------------------

bool ISteamRemoteStorage::FileWrite(const char* pchFile, const void* pvData, int32 cubData)
{
    if (!pchFile || cubData < 0 || (cubData > 0 && !pvData))
    {
        return false;
    }
    StubState& state = State();

    const auto* bytes = static_cast<const u8*>(pvData);
    state.CloudFiles[pchFile] = std::vector<u8>(bytes, bytes + cubData);
    return true;
}

int32 ISteamRemoteStorage::FileRead(const char* pchFile, void* pvData, int32 cubDataToRead)
{
    if (!pchFile || !pvData || cubDataToRead <= 0)
    {
        return 0;
    }
    StubState& state = State();

    const auto found = state.CloudFiles.find(pchFile);
    if (found == state.CloudFiles.end())
    {
        return 0;
    }

    const int32 toCopy =
        cubDataToRead < static_cast<int32>(found->second.size()) ? cubDataToRead : static_cast<int32>(found->second.size());
    std::memcpy(pvData, found->second.data(), static_cast<sizet>(toCopy));
    return toCopy;
}

bool ISteamRemoteStorage::FileExists(const char* pchFile)
{
    return pchFile && State().CloudFiles.contains(pchFile);
}

bool ISteamRemoteStorage::FileDelete(const char* pchFile)
{
    return pchFile && State().CloudFiles.erase(std::string{ pchFile }) > 0;
}

int32 ISteamRemoteStorage::GetFileSize(const char* pchFile)
{
    if (!pchFile)
    {
        return 0;
    }
    const auto found = State().CloudFiles.find(pchFile);
    return found == State().CloudFiles.end() ? 0 : static_cast<int32>(found->second.size());
}

int32 ISteamRemoteStorage::GetFileCount()
{
    return static_cast<int32>(State().CloudFiles.size());
}

const char* ISteamRemoteStorage::GetFileNameAndSize(int iFile, int32* pnFileSizeInBytes)
{
    StubState& state = State();
    if (iFile < 0 || static_cast<sizet>(iFile) >= state.CloudFiles.size())
    {
        return nullptr;
    }

    auto entry = state.CloudFiles.begin();
    std::advance(entry, iFile);

    state.LastEnumeratedName = entry->first;
    if (pnFileSizeInBytes)
    {
        *pnFileSizeInBytes = static_cast<int32>(entry->second.size());
    }
    return state.LastEnumeratedName.c_str();
}

bool ISteamRemoteStorage::GetQuota(uint64* pnTotalBytes, uint64* puAvailableBytes)
{
    if (!pnTotalBytes || !puAvailableBytes)
    {
        return false;
    }
    const StubState& state = State();
    *pnTotalBytes = static_cast<uint64>(state.CloudTotalBytes);
    *puAvailableBytes = static_cast<uint64>(state.CloudAvailableBytes);
    return true;
}

bool ISteamRemoteStorage::IsCloudEnabledForAccount()
{
    return State().CloudAccountEnabled;
}

bool ISteamRemoteStorage::IsCloudEnabledForApp()
{
    return State().CloudAppEnabled;
}

// --- accessors --------------------------------------------------------------------------
//
// Return nullptr before a successful init, matching the real SDK: calling SteamUserStats()
// without a live session gives you nothing, and the engine's null checks exist for that reason.

ISteamUserStats* SteamUserStats()
{
    return State().Initialized ? &s_UserStats : nullptr;
}

ISteamFriends* SteamFriends()
{
    return State().Initialized ? &s_Friends : nullptr;
}

ISteamUtils* SteamUtils()
{
    return State().Initialized ? &s_Utils : nullptr;
}

ISteamRemoteStorage* SteamRemoteStorage()
{
    return State().Initialized ? &s_RemoteStorage : nullptr;
}

// --- control surface --------------------------------------------------------------------

namespace OloEngine::SteamStub
{
    bool IsActive()
    {
        return true;
    }

    void SetInitResult(int result)
    {
        State().InitResult = result;
    }

    void RegisterAchievement(std::string_view achievementId)
    {
        State().KnownAchievements.emplace(achievementId);
    }

    void ForceAchievementUnlocked(std::string_view achievementId, bool unlocked)
    {
        StubState& state = State();
        state.KnownAchievements.emplace(achievementId);
        if (unlocked)
        {
            state.UnlockedAchievements.emplace(achievementId);
        }
        else
        {
            state.UnlockedAchievements.erase(std::string{ achievementId });
        }
    }

    bool IsAchievementUnlocked(std::string_view achievementId)
    {
        return State().UnlockedAchievements.contains(achievementId);
    }

    u32 GetStoreStatsCallCount()
    {
        return State().StoreStatsCalls;
    }

    void SetOverlayActive(bool active)
    {
        State().OverlayActive = active;
    }

    void SetCloudEnabled(bool accountEnabled, bool appEnabled)
    {
        StubState& state = State();
        state.CloudAccountEnabled = accountEnabled;
        state.CloudAppEnabled = appEnabled;
    }

    void SetCloudQuota(u64 totalBytes, u64 availableBytes)
    {
        StubState& state = State();
        state.CloudTotalBytes = totalBytes;
        state.CloudAvailableBytes = availableBytes;
    }

    bool CloudHasFile(std::string_view name)
    {
        return State().CloudFiles.contains(name);
    }

    void Reset()
    {
        // Whole-struct reassignment rather than field-by-field, so a member added later is reset
        // automatically instead of silently leaking between tests.
        State() = StubState{};
    }
} // namespace OloEngine::SteamStub

#else // stub SDK not active — control surface still links, as no-ops

namespace OloEngine::SteamStub
{
    bool IsActive()
    {
        return false;
    }

    void SetInitResult(int) {}
    void RegisterAchievement(std::string_view) {}
    void ForceAchievementUnlocked(std::string_view, bool) {}
    bool IsAchievementUnlocked(std::string_view)
    {
        return false;
    }
    u32 GetStoreStatsCallCount()
    {
        return 0;
    }
    void SetOverlayActive(bool) {}
    void SetCloudEnabled(bool, bool) {}
    void SetCloudQuota(u64, u64) {}
    bool CloudHasFile(std::string_view)
    {
        return false;
    }
    void Reset() {}
} // namespace OloEngine::SteamStub

#endif // OLO_WITH_STEAM && OLO_WITH_STEAM_STUB_SDK

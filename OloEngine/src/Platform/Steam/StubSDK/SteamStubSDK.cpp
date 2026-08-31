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
#include <utility>
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
        // Set when SetOverlayActive changes the state, cleared once RunCallbacks has delivered
        // the event. Real Steam fires GameOverlayActivated_t on TRANSITIONS, not every frame.
        bool OverlayTransitionPending = false;

        bool CloudAccountEnabled = true;
        bool CloudAppEnabled = true;
        u64 CloudTotalBytes = 100ull * 1024ull * 1024ull;
        u64 CloudAvailableBytes = 100ull * 1024ull * 1024ull;
        std::map<std::string, std::vector<u8>, std::less<>> CloudFiles;

        // Backing storage for the const char* that GetFileNameAndSize hands back. The real API
        // returns a pointer into Steam-owned memory valid until the next call; matching that
        // contract with a member keeps callers honest about not retaining it.
        std::string LastEnumeratedName;

        // --- Steam Input ---------------------------------------------------------------
        bool InputInitialized = false;
        std::vector<u64> ConnectedControllers;

        // The stub does not model an action manifest: any name handed to GetActionSetHandle /
        // GetDigitalActionHandle / GetAnalogActionHandle gets a stable handle, assigned on
        // first sight. Real Steam Input only recognises names present in the game's manifest;
        // the stub can't see one (there is no manifest in a CI checkout), so it trusts the
        // caller instead — exactly the same trust the rest of this stub places in the engine.
        std::map<std::string, u64, std::less<>> ActionSetHandles;
        std::map<std::string, u64, std::less<>> DigitalActionHandles;
        std::map<std::string, u64, std::less<>> AnalogActionHandles;
        u64 NextHandle = 1;

        std::map<u64, std::string> ActiveActionSetByController;
        // Keyed by (controller handle, action NAME) rather than the numeric handle, so
        // SteamStub::SetDigitalActionState can be called before or after the engine has
        // resolved a handle for that name — test setup order shouldn't matter.
        std::map<std::pair<u64, std::string>, InputDigitalActionData_t> DigitalActionStateByName;
        std::map<std::pair<u64, std::string>, InputAnalogActionData_t> AnalogActionStateByName;
        // Reverse lookup so GetDigitalActionData(handle) can find the name the state was keyed
        // under.
        std::map<u64, std::string> DigitalHandleToName;
        std::map<u64, std::string> AnalogHandleToName;

        std::map<std::string, std::string, std::less<>> GlyphLabelByAction;
        std::map<std::string, std::string, std::less<>> GlyphPngByAction;

        // Fabricated per-action "origin" so GetStringForActionOrigin / GetGlyphPNGForActionOrigin
        // — which the real SDK keys purely off the origin, not the action — can still find their
        // way back to the action name the origin was handed out for. Starts well above the one
        // named EInputActionOrigin constant the stub declares, so the two numbering spaces never
        // collide.
        std::map<int, std::string> OriginToActionName;
        int NextOrigin = 1000;

        // Backing storage for GetStringForActionOrigin / GetGlyphPNGForActionOrigin's returned
        // const char*, on the same "valid until the next call" contract as LastEnumeratedName
        // above — but kept as its OWN member rather than reusing that one. The two accessors
        // serve unrelated SDK surfaces (ISteamRemoteStorage's file enumeration vs ISteamInput's
        // glyph lookup), and sharing a single string would silently invalidate whichever
        // pointer was returned first the moment the other surface is called.
        std::string LastGlyphString;
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
    ISteamInput s_Input;

    // First-sight handle assignment shared by GetActionSetHandle / GetDigitalActionHandle /
    // GetAnalogActionHandle — see the comment on StubState::ActionSetHandles for why the stub
    // doesn't validate names against a manifest.
    u64 InternHandle(std::map<std::string, u64, std::less<>>& table, std::string_view name)
    {
        if (const auto found = table.find(name); found != table.end())
        {
            return found->second;
        }
        StubState& state = State();
        const u64 handle = state.NextHandle++;
        table.emplace(std::string{ name }, handle);
        return handle;
    }

    // First-sight origin assignment for an action name, and the reverse entry that lets
    // GetStringForActionOrigin / GetGlyphPNGForActionOrigin find their way back to it — see the
    // comment on StubState::OriginToActionName.
    int AssignOrigin(StubState& state, const std::string& actionName)
    {
        for (const auto& [origin, name] : state.OriginToActionName)
        {
            if (name == actionName)
            {
                return origin;
            }
        }
        const int origin = state.NextOrigin++;
        state.OriginToActionName.emplace(origin, actionName);
        return origin;
    }
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
    StubState& state = State();
    if (!state.Initialized)
    {
        return;
    }

    // Deliver a pending overlay transition to whatever the engine registered.
    //
    // This is the one place the stub genuinely DISPATCHES rather than merely linking, and it
    // exists so SteamworksBackend::OnGameOverlayActivated — the only callback-handling code in
    // the backend — is executed in CI instead of sitting dead. Fired on transitions only,
    // matching real Steam, so a test can assert the pump is what moves the state rather than a
    // poll of some always-on flag.
    if (state.OverlayTransitionPending && SteamStubInternal::g_OverlayCallback)
    {
        GameOverlayActivated_t event{};
        event.m_bActive = state.OverlayActive ? uint8{ 1 } : uint8{ 0 };
        event.m_bUserInitiated = true;
        event.m_nAppID = static_cast<AppId_t>(state.AppId);
        event.m_dwOverlayPID = 0;

        state.OverlayTransitionPending = false;
        SteamStubInternal::g_OverlayCallback(&event);
    }
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

ISteamInput* SteamInput()
{
    // Steam Input's accessor is available whenever SteamAPI itself is up — Init() below is
    // what actually brings the subsystem online, matching the real SDK's two-step contract
    // (SteamInput() can return non-null before ISteamInput::Init() has been called).
    return State().Initialized ? &s_Input : nullptr;
}

// --- ISteamInput --------------------------------------------------------------------------

bool ISteamInput::Init(bool /*bExplicitlyCallRunFrame*/)
{
    State().InputInitialized = true;
    return true;
}

bool ISteamInput::Shutdown()
{
    State().InputInitialized = false;
    return true;
}

void ISteamInput::RunFrame(bool /*bReservedValue*/) {}

int ISteamInput::GetConnectedControllers(InputHandle_t* handlesOut)
{
    if (!handlesOut)
    {
        return 0;
    }
    const StubState& state = State();
    const int count = static_cast<int>(std::min(state.ConnectedControllers.size(), static_cast<sizet>(STEAM_INPUT_MAX_COUNT)));
    for (int i = 0; i < count; ++i)
    {
        handlesOut[i] = static_cast<InputHandle_t>(state.ConnectedControllers[static_cast<sizet>(i)]);
    }
    return count;
}

InputActionSetHandle_t ISteamInput::GetActionSetHandle(const char* pszActionSetName)
{
    if (!pszActionSetName)
    {
        return 0;
    }
    return static_cast<InputActionSetHandle_t>(InternHandle(State().ActionSetHandles, pszActionSetName));
}

void ISteamInput::ActivateActionSet(InputHandle_t inputHandle, InputActionSetHandle_t actionSetHandle)
{
    StubState& state = State();
    for (const auto& [name, handle] : state.ActionSetHandles)
    {
        if (handle == static_cast<u64>(actionSetHandle))
        {
            state.ActiveActionSetByController[static_cast<u64>(inputHandle)] = name;
            return;
        }
    }
}

InputDigitalActionHandle_t ISteamInput::GetDigitalActionHandle(const char* pszActionName)
{
    if (!pszActionName)
    {
        return 0;
    }
    StubState& state = State();
    const u64 handle = InternHandle(state.DigitalActionHandles, pszActionName);
    state.DigitalHandleToName[handle] = pszActionName;
    return static_cast<InputDigitalActionHandle_t>(handle);
}

InputDigitalActionData_t ISteamInput::GetDigitalActionData(InputHandle_t inputHandle, InputDigitalActionHandle_t digitalActionHandle)
{
    const StubState& state = State();
    const auto nameIt = state.DigitalHandleToName.find(static_cast<u64>(digitalActionHandle));
    if (nameIt == state.DigitalHandleToName.end())
    {
        return {};
    }
    const auto stateIt = state.DigitalActionStateByName.find({ static_cast<u64>(inputHandle), nameIt->second });
    return stateIt != state.DigitalActionStateByName.end() ? stateIt->second : InputDigitalActionData_t{};
}

int ISteamInput::GetDigitalActionOrigins(InputHandle_t /*inputHandle*/, InputActionSetHandle_t /*actionSetHandle*/,
                                         InputDigitalActionHandle_t digitalActionHandle, EInputActionOrigin* originsOut)
{
    if (!originsOut)
    {
        return 0;
    }
    StubState& state = State();
    const auto nameIt = state.DigitalHandleToName.find(static_cast<u64>(digitalActionHandle));
    if (nameIt == state.DigitalHandleToName.end())
    {
        return 0;
    }
    originsOut[0] = static_cast<EInputActionOrigin>(AssignOrigin(state, nameIt->second));
    return 1;
}

InputAnalogActionHandle_t ISteamInput::GetAnalogActionHandle(const char* pszActionName)
{
    if (!pszActionName)
    {
        return 0;
    }
    StubState& state = State();
    const u64 handle = InternHandle(state.AnalogActionHandles, pszActionName);
    state.AnalogHandleToName[handle] = pszActionName;
    return static_cast<InputAnalogActionHandle_t>(handle);
}

InputAnalogActionData_t ISteamInput::GetAnalogActionData(InputHandle_t inputHandle, InputAnalogActionHandle_t analogActionHandle)
{
    const StubState& state = State();
    const auto nameIt = state.AnalogHandleToName.find(static_cast<u64>(analogActionHandle));
    if (nameIt == state.AnalogHandleToName.end())
    {
        return {};
    }
    const auto stateIt = state.AnalogActionStateByName.find({ static_cast<u64>(inputHandle), nameIt->second });
    return stateIt != state.AnalogActionStateByName.end() ? stateIt->second : InputAnalogActionData_t{};
}

int ISteamInput::GetAnalogActionOrigins(InputHandle_t /*inputHandle*/, InputActionSetHandle_t /*actionSetHandle*/,
                                        InputAnalogActionHandle_t analogActionHandle, EInputActionOrigin* originsOut)
{
    if (!originsOut)
    {
        return 0;
    }
    StubState& state = State();
    const auto nameIt = state.AnalogHandleToName.find(static_cast<u64>(analogActionHandle));
    if (nameIt == state.AnalogHandleToName.end())
    {
        return 0;
    }
    originsOut[0] = static_cast<EInputActionOrigin>(AssignOrigin(state, nameIt->second));
    return 1;
}

const char* ISteamInput::GetStringForActionOrigin(EInputActionOrigin eOrigin)
{
    StubState& state = State();
    const auto nameIt = state.OriginToActionName.find(static_cast<int>(eOrigin));
    const std::string* actionName = nameIt != state.OriginToActionName.end() ? &nameIt->second : nullptr;
    const auto labelIt = actionName ? state.GlyphLabelByAction.find(*actionName) : state.GlyphLabelByAction.end();
    // A test that never called SetGlyphForAction still gets a non-empty, obviously-synthetic
    // label rather than an empty string — matching the "real failure vs stubbed value must
    // never be confused" rule the rest of this stub follows for its error strings.
    state.LastGlyphString = labelIt != state.GlyphLabelByAction.end() ? labelIt->second : "stub SDK: unlabelled origin";
    return state.LastGlyphString.c_str();
}

const char* ISteamInput::GetGlyphPNGForActionOrigin(EInputActionOrigin eOrigin, ESteamInputGlyphSize /*eSize*/, uint32 /*unFlags*/)
{
    StubState& state = State();
    const auto nameIt = state.OriginToActionName.find(static_cast<int>(eOrigin));
    const std::string* actionName = nameIt != state.OriginToActionName.end() ? &nameIt->second : nullptr;
    const auto pngIt = actionName ? state.GlyphPngByAction.find(*actionName) : state.GlyphPngByAction.end();
    state.LastGlyphString = pngIt != state.GlyphPngByAction.end() ? pngIt->second : std::string{};
    return state.LastGlyphString.c_str();
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

    bool IsOverlayCallbackRegistered()
    {
        return static_cast<bool>(SteamStubInternal::g_OverlayCallback);
    }

    void SetOverlayActive(bool active)
    {
        StubState& state = State();
        if (state.OverlayActive == active)
        {
            return; // no transition, nothing for the pump to deliver
        }
        state.OverlayActive = active;

        // The engine does NOT see this until SteamAPI_RunCallbacks delivers it — which is the
        // point. A test must pump to observe the change, exactly as a frame would, so the
        // callback path is what gets exercised rather than a directly-poked flag.
        state.OverlayTransitionPending = true;
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

    void SetConnectedControllers(const std::vector<u64>& controllerHandles)
    {
        State().ConnectedControllers = controllerHandles;
    }

    void SetDigitalActionState(u64 controllerHandle, std::string_view actionName, bool pressed, bool active)
    {
        State().DigitalActionStateByName[{ controllerHandle, std::string{ actionName } }] =
            InputDigitalActionData_t{ .bState = pressed, .bActive = active };
    }

    void SetAnalogActionState(u64 controllerHandle, std::string_view actionName, f32 x, f32 y, bool active, bool triggerMode)
    {
        State().AnalogActionStateByName[{ controllerHandle, std::string{ actionName } }] = InputAnalogActionData_t{
            .eMode = triggerMode ? k_EInputSourceMode_Trigger : k_EInputSourceMode_JoystickMove, .x = x, .y = y, .bActive = active
        };
    }

    std::string GetActiveActionSetName(u64 controllerHandle)
    {
        const StubState& state = State();
        const auto found = state.ActiveActionSetByController.find(controllerHandle);
        return found != state.ActiveActionSetByController.end() ? found->second : std::string{};
    }

    void SetGlyphForAction(std::string_view actionName, std::string_view label, std::string_view pngPath)
    {
        StubState& state = State();
        state.GlyphLabelByAction[std::string{ actionName }] = std::string{ label };
        state.GlyphPngByAction[std::string{ actionName }] = std::string{ pngPath };
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
    bool IsOverlayCallbackRegistered()
    {
        return false;
    }
    void SetOverlayActive(bool) {}
    void SetCloudEnabled(bool, bool) {}
    void SetCloudQuota(u64, u64) {}
    bool CloudHasFile(std::string_view)
    {
        return false;
    }
    void SetConnectedControllers(const std::vector<u64>&) {}
    void SetDigitalActionState(u64, std::string_view, bool, bool) {}
    void SetAnalogActionState(u64, std::string_view, f32, f32, bool, bool) {}
    std::string GetActiveActionSetName(u64)
    {
        return {};
    }
    void SetGlyphForAction(std::string_view, std::string_view, std::string_view) {}
    void Reset() {}
} // namespace OloEngine::SteamStub

#endif // OLO_WITH_STEAM && OLO_WITH_STEAM_STUB_SDK

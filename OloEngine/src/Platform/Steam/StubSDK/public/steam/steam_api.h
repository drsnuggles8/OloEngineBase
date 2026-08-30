#pragma once

// =====================================================================================
// HAND-WRITTEN STUB. THIS IS NOT VALVE'S HEADER.
//
// Nothing here is copied from the Steamworks SDK. These are our own minimal declarations of
// only the entry points OloEngine actually calls, written for interoperability so that
// Platform/Steam/SteamworksBackend.cpp can be compiled and linked without the proprietary SDK.
// The symbols are provided by SteamStubSDK.cpp and return canned values.
//
// WHY THIS EXISTS
// ---------------
// OloEngineBase is a PUBLIC repository and the Steamworks SDK licence forbids redistribution,
// so the real headers can never reach CI. Without this file the OLO_WITH_STEAM=ON path would be
// compiled on developer machines only and would bitrot silently — Trap 3 in
// docs/agent-rules/vcpkg-dependency-management.md. With it, CI compiles, LINKS and RUNS a
// genuine ON build, including the SteamAPI_Init()-fails branch, which is the behaviour that must
// never regress.
//
// CONTRACT
// --------
// This file must stay in sync with what SteamworksBackend.cpp calls — nothing more. A call the
// stubs do not declare turns the CI job red, and that is the mechanism working: it means someone
// added a Steamworks call without considering that CI cannot see it. Add the declaration here
// and the canned implementation in SteamStubSDK.cpp; do NOT delete the job.
//
// Deliberately NOT a mirror of the SDK: the real v1.65 SDK is 44 headers, and reproducing it
// would be both a bigger licence question and a maintenance burden nobody would keep up with.
// =====================================================================================

#include <cstdint>
#include <functional>
#include <type_traits>

// --- SDK scalar types -------------------------------------------------------------------

using int32 = std::int32_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;
using uint8 = std::uint8_t;
using AppId_t = std::uint32_t;

// --- init / shutdown --------------------------------------------------------------------

inline constexpr int k_cchMaxSteamErrMsg = 1024;
using SteamErrMsg = char[k_cchMaxSteamErrMsg];

enum ESteamAPIInitResult
{
    k_ESteamAPIInitResult_OK = 0,
    k_ESteamAPIInitResult_FailedGeneric = 1,
    k_ESteamAPIInitResult_NoSteamClient = 2,
    k_ESteamAPIInitResult_VersionMismatch = 3,
};

ESteamAPIInitResult SteamAPI_InitEx(SteamErrMsg* pOutErrMsg);
void SteamAPI_Shutdown();
void SteamAPI_RunCallbacks();

// --- cloud constants --------------------------------------------------------------------

inline constexpr uint32 k_unMaxCloudFileChunkSize = 100u * 1024u * 1024u;

// --- callback plumbing ------------------------------------------------------------------

struct GameOverlayActivated_t;

namespace SteamStubInternal
{
    // The overlay callback the engine registered, type-erased so SteamAPI_RunCallbacks (which
    // knows nothing about the engine's class) can invoke it. Set by CCallbackManual::Register.
    inline std::function<void(GameOverlayActivated_t*)> g_OverlayCallback;
} // namespace SteamStubInternal

// Real Steamworks callbacks are dispatched by a CCallback/CCallbackManual template whose base
// registers itself with the Steam dispatch loop. The stub keeps the same shape — Register /
// Unregister plus a member-function pointer — and ACTUALLY DISPATCHES.
//
// An earlier version made Register a no-op, on the reasoning that "with no Steam client there are
// no events". That was wrong in an important way: it left SteamworksBackend::OnGameOverlayActivated
// and the m_OverlayActive it maintains completely unexercised, so the one piece of real
// callback-handling code in the backend was dead in CI. Since a test can now drive the transition
// via SteamStub::SetOverlayActive(), the stub dispatches and that code path is covered.
template<class T, class P, bool WithGameServer>
class CCallbackManual
{
  public:
    void Register(T* pObj, void (T::*func)(P*))
    {
        if constexpr (std::is_same_v<P, GameOverlayActivated_t>)
        {
            if (pObj && func)
            {
                SteamStubInternal::g_OverlayCallback = [pObj, func](GameOverlayActivated_t* param)
                {
                    (pObj->*func)(param);
                };
            }
        }
    }

    void Unregister()
    {
        if constexpr (std::is_same_v<P, GameOverlayActivated_t>)
        {
            SteamStubInternal::g_OverlayCallback = nullptr;
        }
    }
};

struct GameOverlayActivated_t
{
    uint8 m_bActive;
    bool m_bUserInitiated;
    AppId_t m_nAppID;
    uint32 m_dwOverlayPID;
};

// --- interfaces -------------------------------------------------------------------------
//
// Only the methods SteamworksBackend.cpp calls. These are plain classes rather than the real
// SDK's pure-virtual interfaces because nothing here needs a vtable — the accessors below hand
// back a process-wide singleton from SteamStubSDK.cpp.

class ISteamUserStats
{
  public:
    bool GetAchievement(const char* pchName, bool* pbAchieved);
    bool SetAchievement(const char* pchName);
    bool ClearAchievement(const char* pchName);
    bool StoreStats();
};

class ISteamFriends
{
  public:
    const char* GetPersonaName();
    bool SetRichPresence(const char* pchKey, const char* pchValue);
    void ClearRichPresence();
};

class ISteamUtils
{
  public:
    uint32 GetAppID();
    bool IsOverlayEnabled();
};

class ISteamRemoteStorage
{
  public:
    bool FileWrite(const char* pchFile, const void* pvData, int32 cubData);
    int32 FileRead(const char* pchFile, void* pvData, int32 cubDataToRead);
    bool FileExists(const char* pchFile);
    bool FileDelete(const char* pchFile);
    int32 GetFileSize(const char* pchFile);
    int32 GetFileCount();
    const char* GetFileNameAndSize(int iFile, int32* pnFileSizeInBytes);
    bool GetQuota(uint64* pnTotalBytes, uint64* puAvailableBytes);
    bool IsCloudEnabledForAccount();
    bool IsCloudEnabledForApp();
};

// --- accessors --------------------------------------------------------------------------

ISteamUserStats* SteamUserStats();
ISteamFriends* SteamFriends();
ISteamUtils* SteamUtils();
ISteamRemoteStorage* SteamRemoteStorage();

// --- Steam Input --------------------------------------------------------------------------
//
// Only the entry points SteamworksBackend.cpp calls, same rule as the rest of this file.

using InputHandle_t = uint64;
using InputActionSetHandle_t = uint64;
using InputDigitalActionHandle_t = uint64;
using InputAnalogActionHandle_t = uint64;

inline constexpr int STEAM_INPUT_MAX_COUNT = 16;
inline constexpr int STEAM_INPUT_MAX_ORIGINS = 8;

// A tiny slice of the real EInputActionOrigin enum — enough to exercise "an origin was found"
// vs "nothing is bound" (k_EInputActionOrigin_None) without reproducing Valve's ~300-entry
// per-controller-type enum.
enum EInputActionOrigin
{
    k_EInputActionOrigin_None = 0,
    k_EInputActionOrigin_XBoxOne_A = 1,
};

enum ESteamInputGlyphSize
{
    k_ESteamInputGlyphSize_Small = 0,
    k_ESteamInputGlyphSize_Medium = 1,
    k_ESteamInputGlyphSize_Large = 2,
};

struct InputDigitalActionData_t
{
    bool bState;
    bool bActive;
};

struct InputAnalogActionData_t
{
    float x;
    float y;
    bool bActive;
};

class ISteamInput
{
  public:
    bool Init(bool bExplicitlyCallRunFrame);
    bool Shutdown();
    void RunFrame(bool bReservedValue = true);

    int GetConnectedControllers(InputHandle_t* handlesOut);

    InputActionSetHandle_t GetActionSetHandle(const char* pszActionSetName);
    void ActivateActionSet(InputHandle_t inputHandle, InputActionSetHandle_t actionSetHandle);

    InputDigitalActionHandle_t GetDigitalActionHandle(const char* pszActionName);
    InputDigitalActionData_t GetDigitalActionData(InputHandle_t inputHandle, InputDigitalActionHandle_t digitalActionHandle);
    int GetDigitalActionOrigins(InputHandle_t inputHandle, InputActionSetHandle_t actionSetHandle,
                                InputDigitalActionHandle_t digitalActionHandle, EInputActionOrigin* originsOut);

    InputAnalogActionHandle_t GetAnalogActionHandle(const char* pszActionName);
    InputAnalogActionData_t GetAnalogActionData(InputHandle_t inputHandle, InputAnalogActionHandle_t analogActionHandle);
    int GetAnalogActionOrigins(InputHandle_t inputHandle, InputActionSetHandle_t actionSetHandle,
                               InputAnalogActionHandle_t analogActionHandle, EInputActionOrigin* originsOut);

    const char* GetStringForActionOrigin(EInputActionOrigin eOrigin);
    const char* GetGlyphPNGForActionOrigin(EInputActionOrigin eOrigin, ESteamInputGlyphSize eSize, uint32 unFlags);
};

ISteamInput* SteamInput();

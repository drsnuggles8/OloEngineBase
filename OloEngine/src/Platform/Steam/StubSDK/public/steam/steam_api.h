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

// Real Steamworks callbacks are dispatched by a CCallback/CCallbackManual template whose base
// registers itself with the Steam dispatch loop. The engine only needs Register/Unregister and
// the ability to name a member function, so the stub keeps the shape and drops the dispatch —
// no stubbed callback ever fires, which is correct: with no Steam client there are no events.
template<class T, class P, bool WithGameServer>
class CCallbackManual
{
  public:
    void Register(T* /*pObj*/, void (T::* /*func*/)(P*)) {}
    void Unregister() {}
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

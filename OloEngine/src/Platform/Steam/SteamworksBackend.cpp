#include "OloEnginePCH.h"
#include "Platform/Steam/SteamBackend.h"

#include "OloEngine/Core/Log.h"

// =====================================================================================
// THE ONLY TRANSLATION UNIT IN THE ENGINE THAT INCLUDES A VALVE HEADER.
//
// Keep it that way. Everything above this file talks to ISteamBackend, which means the
// interesting logic is testable against a fake with no Steam client, and this file is the only
// thing the hand-written stub SDK has to satisfy for CI. If you need a new Steamworks call, add
// it to ISteamBackend and implement it here — do not include <steam/...> anywhere else.
// =====================================================================================

#if OLO_WITH_STEAM

// Valve ships their headers with a Latin-1 (c) in the copyright banner, so under this project's
// /utf-8 every TU that includes them emits C4828 ("character not valid in the current source
// character set") — three of them, from steamhttpenums.h / isteaminventory.h / isteamvideo.h.
// Not our bug and not fixable upstream.
//
// Suppressed HERE rather than in CMake on purpose: set_source_files_properties is
// directory-scoped and silently does nothing from the wrong CMakeLists (no error, no warning —
// the flag just never reaches the compiler), and a target-wide /wd4828 would hide a genuine
// C4828 anywhere else in the engine. A push/disable around the include is local, obvious, and
// cannot be defeated by CMake scoping or a unity build.
//
// Harmless today (the project is /W4 without /WX) but it would be a hard build break the day
// warnings-as-errors is switched on.
// NOTE THE "public/" PREFIX — it is load-bearing, not cosmetic.
//
// The include directory is the SDK ROOT (…/sdk), NOT …/sdk/public. If public/ were on the
// include path, then `steam/` would be a resolvable prefix engine-wide, and every
// `#include <steam/…>` in the NETWORKING code would start finding Valve's headers instead of
// GameNetworkingSockets'. That actually happens: the Steamworks SDK ships its own
// isteamnetworkingutils.h, steamnetworkingtypes.h, isteamnetworkingsockets.h, steam_api_common.h
// and steamtypes.h. The engine's netcode includes <steam/isteamnetworkingutils.h> (which Valve
// HAS → hijacked) and <steam/steamnetworkingsockets.h> (which Valve does NOT have → still GNS),
// so a Networking TU ends up with half of each SDK and dies with 12 ×
// "C2365: 'k_iSteamNetworkingSocketsCallbacks': redefinition" pointing at a Steamworks header
// from a file that has nothing to do with Steam.
//
// Rooting the include dir one level higher makes that impossible by construction: nothing on the
// include path has a `steam/` child, so GNS's headers can never be shadowed. The stub SDK
// mirrors the same public/steam/ layout for exactly this reason.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4828)
#endif

#include "public/steam/steam_api.h"

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <array>

namespace OloEngine
{
    namespace
    {
        // Steam's own per-write limit: k_unMaxCloudFileChunkSize is 100 MiB, and FileWrite takes
        // an int32 byte count. Checking against Steam's constant rather than INT32_MAX both
        // rejects an over-large save with a message naming the real limit, and keeps the
        // narrowing cast to int32 provably safe — an unguarded huge buffer would wrap to a
        // negative length, which Steam either rejects confusingly or acts on.
        constexpr sizet kMaxCloudFileBytes = static_cast<sizet>(k_unMaxCloudFileChunkSize);

        // Steam's C API is all NUL-terminated char*, and std::string_view is not guaranteed to
        // be. Every string crossing into Valve's API goes through this.
        [[nodiscard]] std::string ToCString(std::string_view value)
        {
            return std::string{ value };
        }
    } // namespace

    // Real Steamworks-backed implementation.
    class SteamworksBackend final : public ISteamBackend
    {
      public:
        SteamworksBackend() = default;

        ~SteamworksBackend() override
        {
            // Defensive: Shutdown() is called explicitly by SteamManager from both of
            // Application's teardown paths, but if an exception unwound past both, this still
            // closes the session rather than leaking it.
            SteamworksBackend::Shutdown();
        }

        [[nodiscard]] bool Initialize() override
        {
            if (m_Initialized)
            {
                return true;
            }

            // SteamAPI_InitEx over SteamAPI_Init: it fills in a detailed, human-readable reason
            // we can put straight in the log, which is the difference between "Steam failed" and
            // "Steam isn't running, here is what to do".
            SteamErrMsg errorMessage = {};
            const ESteamAPIInitResult result = SteamAPI_InitEx(&errorMessage);

            if (result != k_ESteamAPIInitResult_OK)
            {
                // Deliberately not an error and definitely not a throw — see the contract on
                // SteamManager. Each case gets its own remediation sentence because the fixes
                // are genuinely different.
                switch (result)
                {
                    case k_ESteamAPIInitResult_NoSteamClient:
                        OLO_CORE_WARN("[Steam] No running Steam client, so Steam features are off this session. "
                                      "Start Steam and relaunch. Steam says: \"{0}\"",
                                      errorMessage);
                        break;
                    case k_ESteamAPIInitResult_VersionMismatch:
                        OLO_CORE_WARN("[Steam] The Steam client is older than this SDK (v1.65), so Steam features are "
                                      "off this session. Let Steam update itself and relaunch. Steam says: \"{0}\"",
                                      errorMessage);
                        break;
                    case k_ESteamAPIInitResult_FailedGeneric:
                    default:
                        OLO_CORE_WARN("[Steam] Steam initialisation failed, so Steam features are off this session. "
                                      "The usual cause is a missing steam_appid.txt in the working directory "
                                      "(OloEditor/) — put '480' in it to develop against Valve's public Spacewar "
                                      "test app. Steam says: \"{0}\"",
                                      errorMessage);
                        break;
                }
                return false;
            }

            m_Initialized = true;

            // Register the overlay callback only after a successful init; CCallback registration
            // before SteamAPI_Init is not valid.
            m_OverlayCallback.Register(this, &SteamworksBackend::OnGameOverlayActivated);

            return true;
        }

        void Shutdown() override
        {
            if (!m_Initialized)
            {
                return;
            }
            m_OverlayCallback.Unregister();
            SteamAPI_Shutdown();
            m_Initialized = false;
            m_OverlayActive = false;
        }

        void RunCallbacks() override
        {
            if (m_Initialized)
            {
                SteamAPI_RunCallbacks();
            }
        }

        [[nodiscard]] bool IsAvailable() const override
        {
            return m_Initialized;
        }

        // --- identity ----------------------------------------------------------------------

        [[nodiscard]] u32 GetAppId() const override
        {
            ISteamUtils* utils = SteamUtils();
            return utils ? static_cast<u32>(utils->GetAppID()) : 0u;
        }

        [[nodiscard]] std::string GetPersonaName() const override
        {
            ISteamFriends* friends = SteamFriends();
            if (!friends)
            {
                return {};
            }
            const char* name = friends->GetPersonaName();
            return name ? std::string{ name } : std::string{};
        }

        // --- achievements ------------------------------------------------------------------

        SteamResult SetAchievement(std::string_view achievementId) override
        {
            ISteamUserStats* stats = SteamUserStats();
            if (!stats)
            {
                return SteamResult::Unavailable;
            }
            const std::string id = ToCString(achievementId);
            return stats->SetAchievement(id.c_str()) ? SteamResult::Success : SteamResult::Failed;
        }

        SteamResult ClearAchievement(std::string_view achievementId) override
        {
            ISteamUserStats* stats = SteamUserStats();
            if (!stats)
            {
                return SteamResult::Unavailable;
            }
            const std::string id = ToCString(achievementId);
            return stats->ClearAchievement(id.c_str()) ? SteamResult::Success : SteamResult::Failed;
        }

        SteamResult GetAchievementUnlocked(std::string_view achievementId, bool& outUnlocked) const override
        {
            ISteamUserStats* stats = SteamUserStats();
            if (!stats)
            {
                return SteamResult::Unavailable;
            }

            const std::string id = ToCString(achievementId);
            bool achieved = false;

            // GetAchievement returns false for an id Steam doesn't know about, which is how an
            // unknown achievement is distinguished from a known-but-locked one. The manager
            // turns that into a loud warning, since it is otherwise completely silent in-game.
            if (!stats->GetAchievement(id.c_str(), &achieved))
            {
                return SteamResult::NotFound;
            }

            outUnlocked = achieved;
            return SteamResult::Success;
        }

        SteamResult StoreStats() override
        {
            ISteamUserStats* stats = SteamUserStats();
            if (!stats)
            {
                return SteamResult::Unavailable;
            }
            return stats->StoreStats() ? SteamResult::Success : SteamResult::Failed;
        }

        // --- rich presence -----------------------------------------------------------------

        SteamResult SetRichPresence(std::string_view key, std::string_view value) override
        {
            ISteamFriends* friends = SteamFriends();
            if (!friends)
            {
                return SteamResult::Unavailable;
            }
            const std::string keyString = ToCString(key);
            const std::string valueString = ToCString(value);
            return friends->SetRichPresence(keyString.c_str(), valueString.c_str()) ? SteamResult::Success
                                                                                    : SteamResult::Failed;
        }

        void ClearRichPresence() override
        {
            if (ISteamFriends* friends = SteamFriends())
            {
                friends->ClearRichPresence();
            }
        }

        // --- overlay -----------------------------------------------------------------------

        // NOTE this is the *displayed* state, tracked from the GameOverlayActivated_t callback.
        // ISteamUtils::IsOverlayEnabled() answers a different question — whether the overlay is
        // available at all — and using it here would report "active" for the whole session.
        [[nodiscard]] bool IsOverlayActive() const override
        {
            return m_OverlayActive;
        }

        // --- cloud -------------------------------------------------------------------------

        [[nodiscard]] bool IsCloudEnabled() const override
        {
            ISteamRemoteStorage* storage = SteamRemoteStorage();
            if (!storage)
            {
                return false;
            }
            // BOTH switches must be on. A game that checks only the app-level one silently loses
            // saves for every player who turned Cloud off account-wide.
            return storage->IsCloudEnabledForAccount() && storage->IsCloudEnabledForApp();
        }

        SteamResult CloudWrite(std::string_view name, std::span<const u8> data) override
        {
            ISteamRemoteStorage* storage = SteamRemoteStorage();
            if (!storage)
            {
                return SteamResult::Unavailable;
            }
            if (data.size() > kMaxCloudFileBytes)
            {
                OLO_CORE_WARN("[Steam] Cloud write of '{0}' is {1} bytes, over Steam's {2}-byte per-file limit.", name,
                              data.size(), kMaxCloudFileBytes);
                return SteamResult::InvalidArgument;
            }

            const std::string fileName = ToCString(name);
            const bool wrote =
                storage->FileWrite(fileName.c_str(), data.data(), static_cast<int32>(data.size()));
            return wrote ? SteamResult::Success : SteamResult::Failed;
        }

        SteamResult CloudRead(std::string_view name, std::vector<u8>& outData) const override
        {
            ISteamRemoteStorage* storage = SteamRemoteStorage();
            if (!storage)
            {
                return SteamResult::Unavailable;
            }

            const std::string fileName = ToCString(name);
            if (!storage->FileExists(fileName.c_str()))
            {
                return SteamResult::NotFound;
            }

            const int32 size = storage->GetFileSize(fileName.c_str());
            if (size <= 0)
            {
                // A zero-length cloud file is legal but carries no save; treat it as absent
                // rather than handing the caller an empty buffer that looks like a valid save.
                return SteamResult::NotFound;
            }

            outData.resize(static_cast<sizet>(size));
            const int32 read = storage->FileRead(fileName.c_str(), outData.data(), size);
            if (read != size)
            {
                OLO_CORE_WARN("[Steam] Cloud read of '{0}' returned {1} of {2} bytes; discarding the partial read.",
                              name, read, size);
                outData.clear();
                return SteamResult::Failed;
            }
            return SteamResult::Success;
        }

        [[nodiscard]] bool CloudExists(std::string_view name) const override
        {
            ISteamRemoteStorage* storage = SteamRemoteStorage();
            if (!storage)
            {
                return false;
            }
            const std::string fileName = ToCString(name);
            return storage->FileExists(fileName.c_str());
        }

        SteamResult CloudDelete(std::string_view name) override
        {
            ISteamRemoteStorage* storage = SteamRemoteStorage();
            if (!storage)
            {
                return SteamResult::Unavailable;
            }
            const std::string fileName = ToCString(name);
            if (!storage->FileExists(fileName.c_str()))
            {
                return SteamResult::NotFound;
            }
            // FileDelete removes it locally AND from the cloud. FileForget would only stop
            // syncing while leaving the local copy, which is not what a delete means here.
            return storage->FileDelete(fileName.c_str()) ? SteamResult::Success : SteamResult::Failed;
        }

        [[nodiscard]] std::vector<std::string> CloudEnumerate() const override
        {
            std::vector<std::string> files;
            ISteamRemoteStorage* storage = SteamRemoteStorage();
            if (!storage)
            {
                return files;
            }

            const int32 count = storage->GetFileCount();
            files.reserve(static_cast<sizet>(std::max(count, 0)));
            for (int32 i = 0; i < count; ++i)
            {
                int32 fileSize = 0;
                if (const char* fileName = storage->GetFileNameAndSize(i, &fileSize); fileName && *fileName)
                {
                    files.emplace_back(fileName);
                }
            }
            return files;
        }

        SteamResult GetCloudQuota(SteamCloudQuota& outQuota) const override
        {
            ISteamRemoteStorage* storage = SteamRemoteStorage();
            if (!storage)
            {
                return SteamResult::Unavailable;
            }

            uint64 total = 0;
            uint64 available = 0;
            if (!storage->GetQuota(&total, &available))
            {
                return SteamResult::Failed;
            }

            outQuota.TotalBytes = static_cast<u64>(total);
            outQuota.AvailableBytes = static_cast<u64>(available);
            return SteamResult::Success;
        }

        // --- input -------------------------------------------------------------------------

        bool InputInit() override
        {
            if (m_InputInitialized)
            {
                return true;
            }
            ISteamInput* input = SteamInput();
            if (!input)
            {
                return false;
            }
            // We drive the pump ourselves from RunFrame() below (called once per engine frame,
            // right after SteamAPI_RunCallbacks) rather than letting the SDK run it implicitly
            // off SteamAPI_RunCallbacks — explicit control is what the SDK recommends for a
            // game with its own fixed frame loop.
            m_InputInitialized = input->Init(true);
            return m_InputInitialized;
        }

        void InputShutdown() override
        {
            if (!m_InputInitialized)
            {
                return;
            }
            if (ISteamInput* input = SteamInput())
            {
                input->Shutdown();
            }
            m_InputInitialized = false;
        }

        void InputRunFrame() override
        {
            if (m_InputInitialized)
            {
                if (ISteamInput* input = SteamInput())
                {
                    input->RunFrame();
                }
            }
        }

        [[nodiscard]] bool IsInputAvailable() const override
        {
            return m_InputInitialized;
        }

        [[nodiscard]] std::vector<SteamInputHandle> GetConnectedControllers() const override
        {
            std::vector<SteamInputHandle> result;
            ISteamInput* input = SteamInput();
            if (!m_InputInitialized || !input)
            {
                return result;
            }

            std::array<InputHandle_t, STEAM_INPUT_MAX_COUNT> handles{};
            const int count = input->GetConnectedControllers(handles.data());
            result.reserve(static_cast<sizet>(std::max(count, 0)));
            for (int i = 0; i < count; ++i)
            {
                result.push_back(static_cast<SteamInputHandle>(handles[static_cast<sizet>(i)]));
            }
            return result;
        }

        [[nodiscard]] SteamInputActionSetHandle GetActionSetHandle(std::string_view actionSetName) const override
        {
            ISteamInput* input = SteamInput();
            if (!m_InputInitialized || !input)
            {
                return kInvalidSteamInputActionSetHandle;
            }
            const std::string name = ToCString(actionSetName);
            return static_cast<SteamInputActionSetHandle>(input->GetActionSetHandle(name.c_str()));
        }

        void ActivateActionSet(SteamInputHandle controller, SteamInputActionSetHandle actionSet) override
        {
            ISteamInput* input = SteamInput();
            if (!m_InputInitialized || !input || controller == kInvalidSteamInputHandle ||
                actionSet == kInvalidSteamInputActionSetHandle)
            {
                return;
            }
            input->ActivateActionSet(static_cast<InputHandle_t>(controller), static_cast<InputActionSetHandle_t>(actionSet));
        }

        [[nodiscard]] SteamInputDigitalActionHandle GetDigitalActionHandle(std::string_view actionName) const override
        {
            ISteamInput* input = SteamInput();
            if (!m_InputInitialized || !input)
            {
                return kInvalidSteamInputDigitalActionHandle;
            }
            const std::string name = ToCString(actionName);
            return static_cast<SteamInputDigitalActionHandle>(input->GetDigitalActionHandle(name.c_str()));
        }

        [[nodiscard]] SteamInputDigitalActionState GetDigitalActionState(SteamInputHandle controller,
                                                                         SteamInputDigitalActionHandle action) const override
        {
            ISteamInput* input = SteamInput();
            if (!m_InputInitialized || !input || controller == kInvalidSteamInputHandle ||
                action == kInvalidSteamInputDigitalActionHandle)
            {
                return {};
            }
            const InputDigitalActionData_t data =
                input->GetDigitalActionData(static_cast<InputHandle_t>(controller), static_cast<InputDigitalActionHandle_t>(action));
            return SteamInputDigitalActionState{ .Pressed = data.bState, .Active = data.bActive };
        }

        [[nodiscard]] SteamInputAnalogActionHandle GetAnalogActionHandle(std::string_view actionName) const override
        {
            ISteamInput* input = SteamInput();
            if (!m_InputInitialized || !input)
            {
                return kInvalidSteamInputAnalogActionHandle;
            }
            const std::string name = ToCString(actionName);
            return static_cast<SteamInputAnalogActionHandle>(input->GetAnalogActionHandle(name.c_str()));
        }

        [[nodiscard]] SteamInputAnalogActionState GetAnalogActionState(SteamInputHandle controller,
                                                                       SteamInputAnalogActionHandle action) const override
        {
            ISteamInput* input = SteamInput();
            if (!m_InputInitialized || !input || controller == kInvalidSteamInputHandle ||
                action == kInvalidSteamInputAnalogActionHandle)
            {
                return {};
            }
            const InputAnalogActionData_t data =
                input->GetAnalogActionData(static_cast<InputHandle_t>(controller), static_cast<InputAnalogActionHandle_t>(action));

            f32 x = data.x;
            f32 y = data.y;
            if (data.eMode == k_EInputSourceMode_Trigger)
            {
                // Valve reports a Trigger-mode analog action on 0..1 (0 = released). Normalize to
                // this engine's own GamepadAxis convention — -1..1, -1 = released (see
                // GamepadCodes.h's GamepadAxis::RightTrigger) — so a trigger-shaped action reads
                // identically whether it came from Steam Input or raw XInput/DirectInput.
                x = (x * 2.0f) - 1.0f;
                y = (y * 2.0f) - 1.0f; // unused by a Trigger action, but kept consistent.
            }
            // Every other source mode (JoystickMove and the button/dpad/mouse modes an analog
            // action can theoretically report) is already on a -1..1-or-boolean convention this
            // engine's own gamepad axes match, so nothing else needs converting.

            return SteamInputAnalogActionState{ .X = x, .Y = y, .Active = data.bActive };
        }

        [[nodiscard]] std::string GetGlyphLabelForDigitalAction(SteamInputHandle controller, SteamInputActionSetHandle actionSet,
                                                                SteamInputDigitalActionHandle action) const override
        {
            const EInputActionOrigin origin = FirstDigitalOrigin(controller, actionSet, action);
            return OriginLabel(origin);
        }

        [[nodiscard]] std::string GetGlyphLabelForAnalogAction(SteamInputHandle controller, SteamInputActionSetHandle actionSet,
                                                               SteamInputAnalogActionHandle action) const override
        {
            const EInputActionOrigin origin = FirstAnalogOrigin(controller, actionSet, action);
            return OriginLabel(origin);
        }

        [[nodiscard]] std::string GetGlyphPngForDigitalAction(SteamInputHandle controller, SteamInputActionSetHandle actionSet,
                                                              SteamInputDigitalActionHandle action) const override
        {
            const EInputActionOrigin origin = FirstDigitalOrigin(controller, actionSet, action);
            return OriginGlyphPng(origin);
        }

        [[nodiscard]] std::string GetGlyphPngForAnalogAction(SteamInputHandle controller, SteamInputActionSetHandle actionSet,
                                                             SteamInputAnalogActionHandle action) const override
        {
            const EInputActionOrigin origin = FirstAnalogOrigin(controller, actionSet, action);
            return OriginGlyphPng(origin);
        }

      private:
        [[nodiscard]] EInputActionOrigin FirstDigitalOrigin(SteamInputHandle controller, SteamInputActionSetHandle actionSet,
                                                            SteamInputDigitalActionHandle action) const
        {
            ISteamInput* input = SteamInput();
            if (!m_InputInitialized || !input || controller == kInvalidSteamInputHandle ||
                actionSet == kInvalidSteamInputActionSetHandle || action == kInvalidSteamInputDigitalActionHandle)
            {
                return k_EInputActionOrigin_None;
            }
            std::array<EInputActionOrigin, STEAM_INPUT_MAX_ORIGINS> origins{};
            const int count = input->GetDigitalActionOrigins(static_cast<InputHandle_t>(controller),
                                                             static_cast<InputActionSetHandle_t>(actionSet),
                                                             static_cast<InputDigitalActionHandle_t>(action), origins.data());
            return count > 0 ? origins[0] : k_EInputActionOrigin_None;
        }

        [[nodiscard]] EInputActionOrigin FirstAnalogOrigin(SteamInputHandle controller, SteamInputActionSetHandle actionSet,
                                                           SteamInputAnalogActionHandle action) const
        {
            ISteamInput* input = SteamInput();
            if (!m_InputInitialized || !input || controller == kInvalidSteamInputHandle ||
                actionSet == kInvalidSteamInputActionSetHandle || action == kInvalidSteamInputAnalogActionHandle)
            {
                return k_EInputActionOrigin_None;
            }
            std::array<EInputActionOrigin, STEAM_INPUT_MAX_ORIGINS> origins{};
            const int count = input->GetAnalogActionOrigins(static_cast<InputHandle_t>(controller),
                                                            static_cast<InputActionSetHandle_t>(actionSet),
                                                            static_cast<InputAnalogActionHandle_t>(action), origins.data());
            return count > 0 ? origins[0] : k_EInputActionOrigin_None;
        }

        [[nodiscard]] std::string OriginLabel(EInputActionOrigin origin) const
        {
            if (origin == k_EInputActionOrigin_None)
            {
                return {};
            }
            ISteamInput* input = SteamInput();
            if (!input)
            {
                return {};
            }
            const char* label = input->GetStringForActionOrigin(origin);
            return label ? std::string{ label } : std::string{};
        }

        [[nodiscard]] std::string OriginGlyphPng(EInputActionOrigin origin) const
        {
            if (origin == k_EInputActionOrigin_None)
            {
                return {};
            }
            ISteamInput* input = SteamInput();
            if (!input)
            {
                return {};
            }
            // Medium is a reasonable general-purpose size for an in-game prompt; callers that
            // need a different size can add a parameter later, but nothing in the engine wants
            // one today.
            const char* path = input->GetGlyphPNGForActionOrigin(origin, k_ESteamInputGlyphSize_Medium, 0);
            return path ? std::string{ path } : std::string{};
        }

        void OnGameOverlayActivated(GameOverlayActivated_t* callback)
        {
            if (!callback)
            {
                return;
            }
            m_OverlayActive = callback->m_bActive != 0;
            OLO_CORE_TRACE("[Steam] Overlay {0}.", m_OverlayActive ? "opened" : "closed");
        }

        bool m_Initialized = false;
        bool m_OverlayActive = false;
        bool m_InputInitialized = false;

        // Manual registration (CCallbackManual): a CCallback registering in the constructor
        // would run before SteamAPI_Init, which is not valid. Registered in Initialize() only on
        // success, and unregistered in Shutdown() before SteamAPI_Shutdown().
        CCallbackManual<SteamworksBackend, GameOverlayActivated_t, false> m_OverlayCallback;
    };

    Scope<ISteamBackend> CreateSteamBackend()
    {
        return CreateScope<SteamworksBackend>();
    }
} // namespace OloEngine

#else // !OLO_WITH_STEAM — compile-time degradation (Variant B)

namespace OloEngine
{
    // The complete no-op stub half of the two-variant contract. The header and link surface are
    // identical to the enabled build, which is what keeps the Lua bindings, the editor UI and
    // every test compiling unchanged with no SDK present.
    //
    // There is no second stub class here on purpose: NullSteamBackend already IS the complete
    // no-op set, and duplicating it would give the OFF path a second thing to drift from.
    Scope<ISteamBackend> CreateSteamBackend()
    {
        return CreateScope<NullSteamBackend>();
    }
} // namespace OloEngine

#endif // OLO_WITH_STEAM

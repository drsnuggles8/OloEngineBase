#include "OloEnginePCH.h"
#include "Platform/Steam/SteamManager.h"

#include "OloEngine/Core/Log.h"
#include "Platform/Steam/SteamBackend.h"

namespace OloEngine
{
    namespace
    {
        // Steam's own documented caps. Enforced here rather than in the backend so the limits
        // are testable against the fake and identical on every path.
        constexpr sizet kMaxRichPresenceKeyLength = 64;
        constexpr sizet kMaxRichPresenceValueLength = 256;

        Scope<ISteamBackend> s_Backend;
        bool s_Initialized = false;

        // Every public entry point funnels through this. Returns nullptr when Steam cannot serve
        // the call, which is the single place the "no-op and report Unavailable" contract lives.
        [[nodiscard]] ISteamBackend* AvailableBackend()
        {
            if (!s_Backend || !s_Backend->IsAvailable())
            {
                return nullptr;
            }
            return s_Backend.get();
        }
    } // namespace

    void SteamManager::Initialize()
    {
        OLO_PROFILE_FUNCTION();

        if (s_Initialized)
        {
            return;
        }
        s_Initialized = true;

        // A test may have installed a fake before Initialize(); don't stomp it.
        if (!s_Backend)
        {
            s_Backend = CreateSteamBackend();
        }

        // NOTE the deliberate absence of a throw here. Application.cpp throws
        // std::runtime_error when AudioEngine or NetworkManager fail; Steam must not, because
        // "the Steam client isn't running" is an ordinary state for a developer machine, a CI
        // runner, or a player who launched the exe directly. Warn, disable, continue.
        if (!s_Backend->Initialize())
        {
#if OLO_WITH_STEAM
            OLO_CORE_WARN("[Steam] Steamworks is compiled in but the session could not start; Steam features are "
                          "disabled for this run. Usual causes: the Steam client is not running, or there is no "
                          "steam_appid.txt next to the executable / in the working directory (OloEditor/). Put '480' "
                          "in that file to develop against Valve's public Spacewar test app. The engine continues "
                          "normally without Steam.");
#else
            OLO_CORE_TRACE("[Steam] Steamworks support is not compiled in (OLO_WITH_STEAM=0); Steam features are "
                           "unavailable. Set STEAMWORKS_SDK_ROOT and reconfigure to enable it — see "
                           "docs/ops/build.md.");
#endif
            return;
        }

        OLO_CORE_INFO("[Steam] Initialized. AppID={0}, user='{1}'.", s_Backend->GetAppId(), s_Backend->GetPersonaName());
    }

    void SteamManager::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        // Reachable from BOTH of Application's shutdown paths, and from neither if startup died
        // early — so it must tolerate a backend that was never created or never initialised.
        if (s_Backend)
        {
            s_Backend->Shutdown();
            s_Backend.reset();
        }
        s_Initialized = false;
    }

    void SteamManager::RunCallbacks()
    {
        if (ISteamBackend* backend = AvailableBackend())
        {
            backend->RunCallbacks();
        }
    }

    bool SteamManager::IsAvailable()
    {
        return AvailableBackend() != nullptr;
    }

    u32 SteamManager::GetAppId()
    {
        ISteamBackend* backend = AvailableBackend();
        return backend ? backend->GetAppId() : 0;
    }

    std::string SteamManager::GetPersonaName()
    {
        ISteamBackend* backend = AvailableBackend();
        return backend ? backend->GetPersonaName() : std::string{};
    }

    SteamResult SteamManager::UnlockAchievement(std::string_view achievementId)
    {
        OLO_PROFILE_FUNCTION();

        if (achievementId.empty())
        {
            OLO_CORE_WARN("[Steam] UnlockAchievement called with an empty achievement id.");
            return SteamResult::InvalidArgument;
        }

        ISteamBackend* backend = AvailableBackend();
        if (!backend)
        {
            return SteamResult::Unavailable;
        }

        // Dedup BEFORE writing. Steam tolerates a redundant SetAchievement, but StoreStats() is a
        // network round-trip — a game unlocking from an OnUpdate would otherwise issue one every
        // frame for the rest of the session. Returning AlreadySet (rather than Success) lets a
        // caller tell "I just earned this" from "I already had it", which is what an unlock
        // notification or a telemetry hook needs.
        bool alreadyUnlocked = false;
        const SteamResult queried = backend->GetAchievementUnlocked(achievementId, alreadyUnlocked);

        if (queried == SteamResult::Success && alreadyUnlocked)
        {
            return SteamResult::AlreadySet;
        }

        if (queried == SteamResult::NotFound)
        {
            // Almost always a typo or a missing Steamworks-partner-site definition. Worth
            // shouting about: it is silent in-game and only visible here.
            OLO_CORE_WARN("[Steam] Unknown achievement id '{0}' — Steam does not have it defined for AppID {1}. "
                          "Check the spelling and that it exists on the Steamworks partner site.",
                          achievementId, backend->GetAppId());
            return SteamResult::NotFound;
        }

        if (const SteamResult set = backend->SetAchievement(achievementId); !SteamSucceeded(set))
        {
            OLO_CORE_WARN("[Steam] Failed to set achievement '{0}': {1}.", achievementId, SteamResultToString(set));
            return set;
        }

        // The overlay toast fires on the store, not the set. Without this the achievement is
        // recorded but the player sees nothing, which reads as "achievements are broken".
        if (const SteamResult stored = backend->StoreStats(); !SteamSucceeded(stored))
        {
            OLO_CORE_WARN("[Steam] Achievement '{0}' was set locally but StoreStats failed: {1}. It should sync on a "
                          "later store or at shutdown.",
                          achievementId, SteamResultToString(stored));
            return stored;
        }

        OLO_CORE_INFO("[Steam] Achievement unlocked: '{0}'.", achievementId);
        return SteamResult::Success;
    }

    SteamResult SteamManager::ClearAchievement(std::string_view achievementId)
    {
        if (achievementId.empty())
        {
            return SteamResult::InvalidArgument;
        }

        ISteamBackend* backend = AvailableBackend();
        if (!backend)
        {
            return SteamResult::Unavailable;
        }

        if (const SteamResult cleared = backend->ClearAchievement(achievementId); !SteamSucceeded(cleared))
        {
            return cleared;
        }
        return backend->StoreStats();
    }

    bool SteamManager::IsAchievementUnlocked(std::string_view achievementId)
    {
        ISteamBackend* backend = AvailableBackend();
        if (!backend || achievementId.empty())
        {
            return false;
        }

        bool unlocked = false;
        if (backend->GetAchievementUnlocked(achievementId, unlocked) != SteamResult::Success)
        {
            return false;
        }
        return unlocked;
    }

    SteamResult SteamManager::StoreStats()
    {
        ISteamBackend* backend = AvailableBackend();
        return backend ? backend->StoreStats() : SteamResult::Unavailable;
    }

    SteamResult SteamManager::SetRichPresence(std::string_view key, std::string_view value)
    {
        if (key.empty())
        {
            OLO_CORE_WARN("[Steam] SetRichPresence called with an empty key.");
            return SteamResult::InvalidArgument;
        }

        // Reject rather than truncate. Steam silently drops over-long input, and a rich-presence
        // string that quietly loses its tail is far harder to diagnose than a logged rejection.
        if (key.size() > kMaxRichPresenceKeyLength)
        {
            OLO_CORE_WARN("[Steam] Rich-presence key '{0}' is {1} bytes; Steam's limit is {2}.", key, key.size(),
                          kMaxRichPresenceKeyLength);
            return SteamResult::InvalidArgument;
        }
        if (value.size() > kMaxRichPresenceValueLength)
        {
            OLO_CORE_WARN("[Steam] Rich-presence value for key '{0}' is {1} bytes; Steam's limit is {2}.", key,
                          value.size(), kMaxRichPresenceValueLength);
            return SteamResult::InvalidArgument;
        }

        ISteamBackend* backend = AvailableBackend();
        return backend ? backend->SetRichPresence(key, value) : SteamResult::Unavailable;
    }

    void SteamManager::ClearRichPresence()
    {
        if (ISteamBackend* backend = AvailableBackend())
        {
            backend->ClearRichPresence();
        }
    }

    bool SteamManager::IsOverlayActive()
    {
        ISteamBackend* backend = AvailableBackend();
        return backend && backend->IsOverlayActive();
    }

    bool SteamManager::IsCloudEnabled()
    {
        ISteamBackend* backend = AvailableBackend();
        return backend && backend->IsCloudEnabled();
    }

    SteamResult SteamManager::CloudWrite(std::string_view name, std::span<const u8> data)
    {
        OLO_PROFILE_FUNCTION();

        if (name.empty())
        {
            return SteamResult::InvalidArgument;
        }

        ISteamBackend* backend = AvailableBackend();
        if (!backend)
        {
            return SteamResult::Unavailable;
        }
        if (!backend->IsCloudEnabled())
        {
            // Both the account-wide and per-app Cloud switches must be on. This is a normal
            // player-chosen state, not a bug — hence trace, not warn.
            OLO_CORE_TRACE("[Steam] Cloud is disabled; skipping cloud write of '{0}'.", name);
            return SteamResult::Unavailable;
        }
        return backend->CloudWrite(name, data);
    }

    SteamResult SteamManager::CloudRead(std::string_view name, std::vector<u8>& outData)
    {
        OLO_PROFILE_FUNCTION();

        if (name.empty())
        {
            return SteamResult::InvalidArgument;
        }

        ISteamBackend* backend = AvailableBackend();
        if (!backend || !backend->IsCloudEnabled())
        {
            return SteamResult::Unavailable;
        }
        return backend->CloudRead(name, outData);
    }

    bool SteamManager::CloudExists(std::string_view name)
    {
        ISteamBackend* backend = AvailableBackend();
        return backend && !name.empty() && backend->IsCloudEnabled() && backend->CloudExists(name);
    }

    SteamResult SteamManager::CloudDelete(std::string_view name)
    {
        if (name.empty())
        {
            return SteamResult::InvalidArgument;
        }

        ISteamBackend* backend = AvailableBackend();
        if (!backend || !backend->IsCloudEnabled())
        {
            return SteamResult::Unavailable;
        }
        return backend->CloudDelete(name);
    }

    std::vector<std::string> SteamManager::CloudEnumerate()
    {
        ISteamBackend* backend = AvailableBackend();
        if (!backend || !backend->IsCloudEnabled())
        {
            return {};
        }
        return backend->CloudEnumerate();
    }

    SteamResult SteamManager::GetCloudQuota(SteamCloudQuota& outQuota)
    {
        ISteamBackend* backend = AvailableBackend();
        if (!backend || !backend->IsCloudEnabled())
        {
            return SteamResult::Unavailable;
        }
        return backend->GetCloudQuota(outQuota);
    }

    void SteamManager::SetBackendForTesting(Scope<ISteamBackend> backend)
    {
        if (s_Backend)
        {
            s_Backend->Shutdown();
        }
        s_Backend = std::move(backend);
        s_Initialized = false;
    }

    void SteamManager::ResetForTesting()
    {
        Shutdown();
    }
} // namespace OloEngine

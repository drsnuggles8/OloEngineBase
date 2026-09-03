#include "OloEngine/Gameplay/Combat/WeaponState.h"

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    namespace
    {
        [[nodiscard]] f32 SecondsPerRound(const WeaponDefinition& definition)
        {
            if (!std::isfinite(definition.RoundsPerMinute) || definition.RoundsPerMinute <= 0.0f)
            {
                return 0.0f;
            }

            return 60.0f / definition.RoundsPerMinute;
        }
    } // namespace

    WeaponState WeaponState::Loaded(const WeaponDefinition& definition)
    {
        WeaponState state;
        state.m_MagazineAmmo = definition.MagazineSize;
        state.m_ReserveAmmo = definition.ReserveAmmo;
        return state;
    }

    WeaponFireResult WeaponState::TryFire(const WeaponDefinition& definition)
    {
        if (m_IsReloading)
        {
            return WeaponFireResult::Reloading;
        }

        if (m_CadenceRemaining > 0.0f)
        {
            return WeaponFireResult::CadenceBlocked;
        }

        if (m_MagazineAmmo == 0)
        {
            return WeaponFireResult::Empty;
        }

        --m_MagazineAmmo;
        ++m_ShotsFired;
        m_CadenceRemaining += SecondsPerRound(definition);
        return WeaponFireResult::Fired;
    }

    WeaponReloadResult WeaponState::BeginReload(const WeaponDefinition& definition)
    {
        if (m_IsReloading)
        {
            return WeaponReloadResult::AlreadyReloading;
        }
        if (m_MagazineAmmo >= definition.MagazineSize)
        {
            return WeaponReloadResult::MagazineFull;
        }
        if (m_ReserveAmmo == 0)
        {
            return WeaponReloadResult::NoReserveAmmo;
        }

        m_IsReloading = true;
        m_ReloadRemaining = std::isfinite(definition.ReloadSeconds)
                                ? std::max(0.0f, definition.ReloadSeconds)
                                : 0.0f;
        return WeaponReloadResult::Started;
    }

    void WeaponState::Advance(const WeaponDefinition& definition, f32 deltaSeconds, bool fireHeld)
    {
        if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f)
        {
            return;
        }

        m_CadenceRemaining -= deltaSeconds;
        if (!fireHeld || m_IsReloading)
        {
            m_CadenceRemaining = std::max(0.0f, m_CadenceRemaining);
        }

        if (!m_IsReloading)
        {
            return;
        }

        m_ReloadRemaining = std::max(0.0f, m_ReloadRemaining - deltaSeconds);
        if (m_ReloadRemaining > 0.0f)
        {
            return;
        }

        const u32 roundsNeeded = definition.MagazineSize > m_MagazineAmmo
                                     ? definition.MagazineSize - m_MagazineAmmo
                                     : 0;
        const u32 roundsLoaded = std::min(roundsNeeded, m_ReserveAmmo);
        m_MagazineAmmo += roundsLoaded;
        m_ReserveAmmo -= roundsLoaded;
        m_IsReloading = false;
    }
} // namespace OloEngine

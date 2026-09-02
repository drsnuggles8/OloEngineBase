#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Gameplay/Combat/WeaponDefinition.h"

namespace OloEngine
{
    enum class WeaponFireResult : u8
    {
        Fired,
        CadenceBlocked,
        Empty,
        Reloading,
    };

    enum class WeaponReloadResult : u8
    {
        Started,
        AlreadyReloading,
        MagazineFull,
        NoReserveAmmo,
    };

    class WeaponState
    {
      public:
        WeaponState() = default;
        [[nodiscard]] static WeaponState Loaded(const WeaponDefinition& definition);

        [[nodiscard]] WeaponFireResult TryFire(const WeaponDefinition& definition);
        [[nodiscard]] WeaponReloadResult BeginReload(const WeaponDefinition& definition);
        void Advance(const WeaponDefinition& definition, f32 deltaSeconds);

        [[nodiscard]] u32 GetMagazineAmmo() const
        {
            return m_MagazineAmmo;
        }
        [[nodiscard]] u32 GetReserveAmmo() const
        {
            return m_ReserveAmmo;
        }
        [[nodiscard]] u32 GetShotsFired() const
        {
            return m_ShotsFired;
        }

      private:
        u32 m_MagazineAmmo = 0;
        u32 m_ReserveAmmo = 0;
        u32 m_ShotsFired = 0;
        f32 m_CadenceRemaining = 0.0f;
        f32 m_ReloadRemaining = 0.0f;
        bool m_IsReloading = false;
    };
} // namespace OloEngine

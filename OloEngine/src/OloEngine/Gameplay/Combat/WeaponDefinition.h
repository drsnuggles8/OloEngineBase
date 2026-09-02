#pragma once

#include "OloEngine/Core/Base.h"

#include <string>
#include <string_view>

namespace OloEngine
{
    enum class WeaponDelivery : u8
    {
        Hitscan,
        Projectile,
    };

    struct WeaponDefinition
    {
        WeaponDelivery Delivery = WeaponDelivery::Hitscan;
        f32 Damage = 10.0f;
        f32 Range = 100.0f;
        f32 RoundsPerMinute = 600.0f;
        u32 MagazineSize = 30;
        u32 ReserveAmmo = 90;
        f32 ReloadSeconds = 2.0f;
        f32 RecoilPitch = 1.0f;
        f32 RecoilYaw = 0.25f;
        f32 FalloffStart = 50.0f;
        f32 FalloffEnd = 100.0f;
        f32 MinimumDamageMultiplier = 0.5f;
        f32 ProjectileSpeed = 40.0f;
        f32 ProjectileRadius = 0.05f;
        f32 ProjectileLifetime = 5.0f;
        std::string DamageType = "Damage.Ballistic";
        std::string MuzzleAudioTrigger;
        std::string ImpactAudioTrigger;
        std::string HitReactionTrigger = "HitReaction";
    };

    [[nodiscard]] inline WeaponDelivery WeaponDeliveryFromString(std::string_view value)
    {
        return value == "Projectile" ? WeaponDelivery::Projectile : WeaponDelivery::Hitscan;
    }
} // namespace OloEngine

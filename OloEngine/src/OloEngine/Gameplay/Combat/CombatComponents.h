#pragma once

#include "OloEngine/Gameplay/Combat/ProjectileState.h"
#include "OloEngine/Gameplay/Combat/WeaponState.h"
#include "OloEngine/Scene/ComponentReflection.h"

#include <glm/glm.hpp>
#include <string>

namespace OloEngine
{
    struct WeaponComponent
    {
        OLO_PROPERTY()
        std::string m_WeaponItemID;
        OLO_PROPERTY()
        glm::vec3 m_MuzzleOffset{ 0.0f, 0.0f, 0.0f };
        OLO_PROPERTY()
        bool m_UseDeviceInput = true;

        // Per-tick intent. Fire is a level (supports automatic cadence); reload
        // is an edge consumed by CombatSystem.
        OLO_PROPERTY()
        OLO_SERIALIZE(Skip)
        bool m_FireInput = false;
        OLO_PROPERTY()
        OLO_SERIALIZE(Skip)
        bool m_ReloadInput = false;

        OLO_SERIALIZE(Skip)
        WeaponState m_State;
        OLO_SERIALIZE(Skip)
        std::string m_LoadedItemID;
        OLO_SERIALIZE(Skip)
        bool m_Initialized = false;
        OLO_SERIALIZE(Skip)
        bool m_ReloadKeyWasDown = false;

        auto operator==(const WeaponComponent& other) const -> bool
        {
            return m_WeaponItemID == other.m_WeaponItemID && m_MuzzleOffset == other.m_MuzzleOffset &&
                   m_UseDeviceInput == other.m_UseDeviceInput;
        }
    };

    // Runtime projectile entities carry a complete launch snapshot so item hot
    // reload cannot mutate a round already in flight.
    struct ProjectileComponent
    {
        OLO_SERIALIZE(Skip)
        ProjectileState m_State;

        auto operator==(const ProjectileComponent&) const -> bool
        {
            return true;
        }
    };

    // Opt-in player lifecycle policy. The entity is relocated in place so its
    // stable UUID, progression, inventory, and script references survive death.
    struct PlayerRespawnComponent
    {
        OLO_PROPERTY()
        glm::vec3 m_SpawnPoint{ 0.0f };
        OLO_PROPERTY()
        f32 m_SpawnYawDeg = 0.0f;
        OLO_PROPERTY()
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 3600.0f)
        f32 m_RespawnDelay = 3.0f;

        OLO_SERIALIZE(Skip)
        f32 m_TimeRemaining = 0.0f;
        OLO_SERIALIZE(Skip)
        bool m_DeathObserved = false;

        auto operator==(const PlayerRespawnComponent& other) const -> bool
        {
            return m_SpawnPoint == other.m_SpawnPoint && m_SpawnYawDeg == other.m_SpawnYawDeg &&
                   m_RespawnDelay == other.m_RespawnDelay;
        }
    };

    struct ImpactDecalComponent
    {
        OLO_SERIALIZE(Skip)
        f32 m_RemainingSeconds = 8.0f;

        auto operator==(const ImpactDecalComponent&) const -> bool
        {
            return true;
        }
    };
} // namespace OloEngine

#pragma once

#include "OloEngine/Gameplay/Combat/WeaponDefinition.h"
#include "OloEngine/Gameplay/Combat/ProjectileState.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    class Entity;
    class Scene;
    class SceneQueries;
    struct SceneQueryHit;

    class CombatSystem
    {
      public:
        [[nodiscard]] static f32 ApplyWeaponImpact(Scene* scene, Entity source, const WeaponDefinition& definition, const SceneQueryHit& hit);
        [[nodiscard]] static bool FireHitscan(Scene* scene, SceneQueries& queries, Entity source, const WeaponDefinition& definition,
                                              const glm::vec3& origin, const glm::vec3& direction);
        [[nodiscard]] static bool AdvanceProjectile(Scene* scene, SceneQueries& queries, ProjectileState& projectile, f32 deltaSeconds);
        static void OnUpdate(Scene* scene, SceneQueries* queries, f32 deltaSeconds);
    };
} // namespace OloEngine

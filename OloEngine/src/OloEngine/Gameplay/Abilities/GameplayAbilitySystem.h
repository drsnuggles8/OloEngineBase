#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Gameplay/Abilities/Tags/GameplayTag.h"
#include "OloEngine/Gameplay/Abilities/Damage/DamageEvent.h"

namespace OloEngine
{
    class Scene;
    class Entity;

    class GameplayAbilitySystem
    {
      public:
        static void OnUpdate(Scene* scene, f32 dt);
        static bool TryActivateAbility(Scene* scene, Entity owner, const GameplayTag& abilityTag);
        static void CancelAbility(Scene* scene, Entity owner, const GameplayTag& abilityTag);

        /// Apply a damage event to its target: DamageCalculation over the
        /// source/target attribute sets, Health base-value reduction, immediate
        /// State.Alive -> State.Dead tag flip on a kill, EntityKilledEvent
        /// publish, and the kill-XP bounty grant into the killer's
        /// ProgressionComponent (issue #635). This is the single damage choke
        /// point — the C#/Lua damage glue delegates here rather than
        /// duplicating the pipeline. Returns the final calculated damage
        /// (0 when the event had no valid damageable target).
        /// Game-thread only (publishes on the GameplayEventBus).
        static f32 ApplyDamage(Scene* scene, const DamageEvent& event);
    };

} // namespace OloEngine

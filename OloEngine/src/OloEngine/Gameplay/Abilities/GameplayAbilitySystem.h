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
        static void ApplyDamage(Scene* scene, const DamageEvent& event);
    };

} // namespace OloEngine

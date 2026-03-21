#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Abilities/CooldownManager.h"

namespace OloEngine
{

    void CooldownManager::StartCooldown(const GameplayTag& abilityTag, f32 duration)
    {
        if (duration <= 0.0f)
        {
            m_Cooldowns.erase(abilityTag);
            return;
        }
        m_Cooldowns[abilityTag] = { duration, duration };
    }

    void CooldownManager::Tick(f32 dt)
    {
        if (dt <= 0.0f)
        {
            return;
        }
        for (auto it = m_Cooldowns.begin(); it != m_Cooldowns.end();)
        {
            it->second.Remaining -= dt;
            if (it->second.Remaining <= 0.0f)
            {
                it = m_Cooldowns.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    bool CooldownManager::IsOnCooldown(const GameplayTag& abilityTag) const
    {
        auto it = m_Cooldowns.find(abilityTag);
        return it != m_Cooldowns.end() && it->second.Remaining > 0.0f;
    }

    f32 CooldownManager::GetRemainingCooldown(const GameplayTag& abilityTag) const
    {
        auto it = m_Cooldowns.find(abilityTag);
        if (it != m_Cooldowns.end())
        {
            return std::max(it->second.Remaining, 0.0f);
        }
        return 0.0f;
    }

    f32 CooldownManager::GetCooldownFraction(const GameplayTag& abilityTag) const
    {
        auto it = m_Cooldowns.find(abilityTag);
        if (it != m_Cooldowns.end() && it->second.Duration > 0.0f)
        {
            return std::clamp(it->second.Remaining / it->second.Duration, 0.0f, 1.0f);
        }
        return 0.0f;
    }

    void CooldownManager::ResetCooldown(const GameplayTag& abilityTag)
    {
        m_Cooldowns.erase(abilityTag);
    }

    void CooldownManager::ResetAll()
    {
        m_Cooldowns.clear();
    }

} // namespace OloEngine

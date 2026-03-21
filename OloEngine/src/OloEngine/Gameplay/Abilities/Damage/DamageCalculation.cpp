#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Abilities/Damage/DamageCalculation.h"

#include <algorithm>

namespace OloEngine
{

    DamageCalculation::CustomFormula DamageCalculation::s_CustomFormula;

    f32 DamageCalculation::Calculate(const DamageEvent& event, const AttributeSet& sourceAttribs, const AttributeSet& targetAttribs)
    {
        if (s_CustomFormula)
        {
            return s_CustomFormula(event, sourceAttribs, targetAttribs);
        }

        // Default damage pipeline
        f32 damage = event.RawDamage;

        // 1. Apply attack power bonus from source
        damage += sourceAttribs.GetCurrentValue("AttackPower");

        // 2. Apply critical hit
        if (event.IsCritical)
        {
            damage *= event.CritMultiplier;
        }

        // 3. Subtract defense/armor from target
        f32 defense = targetAttribs.GetCurrentValue("Defense");
        damage -= defense;

        // 4. Apply damage type resistance
        // Convention: resistance attribute = "Resistance.<DamageType>" (e.g., "Resistance.Fire")
        std::string resistanceAttr = "Resistance." + event.DamageType.GetTagString();
        if (targetAttribs.HasAttribute(resistanceAttr))
        {
            f32 resistance = targetAttribs.GetCurrentValue(resistanceAttr);
            // Resistance is a percentage reduction (0.0 = no reduction, 1.0 = immune)
            resistance = std::clamp(resistance, 0.0f, 1.0f);
            damage *= (1.0f - resistance);
        }

        // 5. Clamp to minimum 0 damage
        return std::max(damage, 0.0f);
    }

    void DamageCalculation::SetCustomFormula(CustomFormula formula)
    {
        s_CustomFormula = std::move(formula);
    }

    void DamageCalculation::ClearCustomFormula()
    {
        s_CustomFormula = nullptr;
    }

    DamageCalculation::ScopedFormula::ScopedFormula(CustomFormula formula)
        : m_Previous(std::move(s_CustomFormula))
    {
        s_CustomFormula = std::move(formula);
    }

    DamageCalculation::ScopedFormula::~ScopedFormula()
    {
        s_CustomFormula = std::move(m_Previous);
    }

} // namespace OloEngine

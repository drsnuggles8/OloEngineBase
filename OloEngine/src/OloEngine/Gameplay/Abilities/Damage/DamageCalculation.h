#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Gameplay/Abilities/Damage/DamageEvent.h"
#include "OloEngine/Gameplay/Abilities/Attributes/AttributeSet.h"

#include <functional>

namespace OloEngine
{

    class DamageCalculation
    {
      public:
        using CustomFormula = std::function<f32(const DamageEvent&, const AttributeSet&, const AttributeSet&)>;

        // Default pipeline: apply crit -> subtract armor -> apply resistance -> clamp to 0
        static f32 Calculate(const DamageEvent& event, const AttributeSet& sourceAttribs, const AttributeSet& targetAttribs);

        static void SetCustomFormula(CustomFormula formula);
        static void ClearCustomFormula();

        // RAII guard: restores the previous formula on scope exit
        class ScopedFormula
        {
          public:
            explicit ScopedFormula(CustomFormula formula);
            ~ScopedFormula();
            ScopedFormula(const ScopedFormula&) = delete;
            ScopedFormula& operator=(const ScopedFormula&) = delete;

          private:
            CustomFormula m_Previous;
        };

      private:
        static CustomFormula s_CustomFormula;
    };

} // namespace OloEngine

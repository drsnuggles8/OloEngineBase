#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Progression/ExperienceCurve.h"

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    i32 ExperienceCurve::GetXPForLevelUp(i32 currentLevel) const
    {
        i32 level = std::max(currentLevel, 1);

        if (m_Mode == CurveMode::Table && !m_Table.empty())
        {
            auto index = std::min(static_cast<sizet>(level - 1), m_Table.size() - 1);
            return std::max(m_Table[index], 1);
        }

        f32 baseXP = (std::isfinite(m_BaseXP) && m_BaseXP >= 1.0f) ? m_BaseXP : 100.0f;
        f32 exponent = (std::isfinite(m_Exponent) && m_Exponent >= 0.0f) ? std::min(m_Exponent, 10.0f) : 1.0f;

        f64 xp = std::round(static_cast<f64>(baseXP) * std::pow(static_cast<f64>(level), static_cast<f64>(exponent)));
        constexpr f64 kMaxXPStep = 2000000000.0; // stay inside i32
        xp = std::clamp(xp, 1.0, kMaxXPStep);
        return static_cast<i32>(xp);
    }

    i32 ExperienceCurve::DefaultXPForLevelUp(i32 currentLevel)
    {
        i32 level = std::max(currentLevel, 1);
        constexpr i32 kXPPerLevel = 100;
        // 100 XP per current level, saturating well inside i32.
        if (level > 20000000)
        {
            level = 20000000;
        }
        return kXPPerLevel * level;
    }

    void ExperienceCurve::Sanitize()
    {
        m_MaxLevel = std::clamp(m_MaxLevel, 1, 1000);

        if (!std::isfinite(m_BaseXP))
        {
            m_BaseXP = 100.0f;
        }
        m_BaseXP = std::clamp(m_BaseXP, 1.0f, 1e7f);

        if (!std::isfinite(m_Exponent))
        {
            m_Exponent = 1.5f;
        }
        m_Exponent = std::clamp(m_Exponent, 0.0f, 10.0f);

        for (auto& entry : m_Table)
        {
            entry = std::max(entry, 1);
        }
    }
} // namespace OloEngine

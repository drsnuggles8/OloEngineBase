#include "OloEnginePCH.h"
#include "OloEngine/Renderer/LOD.h"

namespace OloEngine
{
    i32 LODGroup::SelectLOD(f32 distance) const
    {
        OLO_PROFILE_FUNCTION();
        if (Levels.empty())
        {
            return -1;
        }

        f32 const safeBias = (Bias <= 0.0f) ? 1.0f : Bias;
        f32 const effectiveDistance = distance / safeBias;

        for (i32 i = 0; i < static_cast<i32>(Levels.size()); ++i)
        {
            if (effectiveDistance <= Levels[i].MaxDistance)
            {
                return i;
            }
        }

        // Beyond all thresholds — return lowest detail level (last)
        return static_cast<i32>(Levels.size()) - 1;
    }
} // namespace OloEngine

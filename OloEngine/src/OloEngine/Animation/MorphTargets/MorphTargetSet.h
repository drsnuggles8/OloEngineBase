#pragma once

#include "MorphTarget.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Ref.h"

#include <string>
#include <utility>
#include <vector>

namespace OloEngine
{
    class MorphTargetSet : public RefCounted
    {
      public:
        std::vector<MorphTarget> Targets;

        MorphTargetSet() = default;

        [[nodiscard]] i32 FindTarget(const std::string& name) const
        {
            for (i32 i = 0; i < static_cast<i32>(Targets.size()); ++i)
            {
                if (Targets[i].Name == name)
                    return i;
            }
            return -1;
        }

        [[nodiscard]] u32 GetTargetCount() const
        {
            return static_cast<u32>(Targets.size());
        }

        [[nodiscard]] u32 GetVertexCount() const
        {
            if (Targets.empty())
                return 0;
            return static_cast<u32>(Targets[0].Vertices.size());
        }

        [[nodiscard]] bool HasTarget(const std::string& name) const
        {
            return FindTarget(name) >= 0;
        }

        void AddTarget(MorphTarget target)
        {
            if (!Targets.empty() && !target.Vertices.empty() && target.Vertices.size() != Targets[0].Vertices.size())
            {
                OLO_CORE_WARN("MorphTargetSet::AddTarget: vertex count mismatch "
                              "(expected {}, got {}) for target '{}'",
                              Targets[0].Vertices.size(), target.Vertices.size(), target.Name);
            }
            Targets.push_back(std::move(target));
        }
    };
} // namespace OloEngine

#pragma once

#include "MorphTarget.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Ref.h"

#include <string>
#include <unordered_map>
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

        bool AddTarget(MorphTarget target)
        {
            if (!Targets.empty() && !target.Vertices.empty() && target.Vertices.size() != Targets[0].Vertices.size())
            {
                OLO_CORE_ERROR("MorphTargetSet::AddTarget: vertex count mismatch "
                               "(expected {}, got {}) for target '{}' — target rejected",
                               Targets[0].Vertices.size(), target.Vertices.size(), target.Name);
                return false;
            }
            m_NameIndexCache.clear(); // Invalidate cache
            Targets.push_back(std::move(target));
            return true;
        }

        // O(1) name-to-index lookup via cached map
        [[nodiscard]] i32 FindTargetCached(const std::string& name) const
        {
            BuildNameIndexCache();
            auto it = m_NameIndexCache.find(name);
            return (it != m_NameIndexCache.end()) ? it->second : -1;
        }

      private:
        void BuildNameIndexCache() const
        {
            if (!m_NameIndexCache.empty() || Targets.empty())
                return;
            for (i32 i = 0; i < static_cast<i32>(Targets.size()); ++i)
                m_NameIndexCache[Targets[i].Name] = i;
        }

        mutable std::unordered_map<std::string, i32> m_NameIndexCache;
    };
} // namespace OloEngine

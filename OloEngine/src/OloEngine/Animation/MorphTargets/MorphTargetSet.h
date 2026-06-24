#pragma once

#include "MorphTarget.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/UniqueLock.h"

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

        [[nodiscard("target index needed for weight mapping")]] i32 FindTarget(const std::string& name) const
        {
            auto count = static_cast<i32>(Targets.size());
            for (i32 i = 0; i < count; ++i)
            {
                if (Targets[i].Name == name)
                    return i;
            }
            return -1;
        }

        [[nodiscard("count needed for buffer sizing")]] u32 GetTargetCount() const
        {
            return static_cast<u32>(Targets.size());
        }

        [[nodiscard("vertex count needed for validation")]] u32 GetVertexCount() const
        {
            if (Targets.empty())
                return 0;
            return static_cast<u32>(Targets[0].Vertices.size());
        }

        [[nodiscard("existence check must be used")]] bool HasTarget(const std::string& name) const
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
            {
                TUniqueLock<FMutex> lock(m_CacheMutex);
                m_NameIndexCache.clear(); // Invalidate cache
            }
            Targets.push_back(std::move(target));
            return true;
        }

        // O(1) name-to-index lookup via cached map
        [[nodiscard("cached target index needed for weight mapping")]] i32 FindTargetCached(const std::string& name) const
        {
            TUniqueLock<FMutex> lock(m_CacheMutex);
            BuildNameIndexCacheLocked();
            auto it = m_NameIndexCache.find(name);
            return (it != m_NameIndexCache.end()) ? it->second : -1;
        }

      private:
        // Caller must hold m_CacheMutex.
        void BuildNameIndexCacheLocked() const
        {
            if (!m_NameIndexCache.empty() || Targets.empty())
                return;
            for (i32 i = 0; i < static_cast<i32>(Targets.size()); ++i)
                m_NameIndexCache[Targets[i].Name] = i;
        }

        // Guards the mutable cache so const lookups can rebuild it from multiple
        // threads without a data race (S8379).
        mutable FMutex m_CacheMutex;
        mutable std::unordered_map<std::string, i32> m_NameIndexCache;
    };
} // namespace OloEngine

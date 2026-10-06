#pragma once

#include "OloEngine/Renderer/BoundingVolume.h"

#include "OloEngine/Math/Math.h"
#include <limits>
#include <span>
#include <unordered_map>
#include <utility>

namespace OloEngine
{
    // Stable caster identity, world-space footprint and silhouette revision.
    struct ShadowFamilyFootprint
    {
        u64 Key = 0;
        BoundingBox Bounds = NoBounds;
        glm::mat4 Transform{ 1.0f };
        u64 Revision = 0;
        bool Deforming = false;
    };

    class ShadowFamilyCache
    {
      public:
        [[nodiscard]] bool HasPreviousCasters() const
        {
            return !m_Previous.empty();
        }

        template<typename Invalidate>
        void Update(std::span<const ShadowFamilyFootprint> current, Invalidate&& invalidate)
        {
            std::unordered_map<u64, ShadowFamilyFootprint> next;
            for (const auto& caster : current)
            {
                auto previous = m_Previous.find(caster.Key);
                if (previous == m_Previous.end())
                    invalidate(caster.Bounds);
                else if (caster.Deforming || previous->second.Deforming || caster.Revision != previous->second.Revision ||
                         !Math::BitwiseEqual(caster.Transform, previous->second.Transform) ||
                         (!Math::BitwiseEqual(caster.Bounds.Min, previous->second.Bounds.Min) ||
                          !Math::BitwiseEqual(caster.Bounds.Max, previous->second.Bounds.Max)))
                {
                    // NoBounds denotes unknown coverage, not an empty box.
                    // Unioning it as an empty box would hide an edited silhouette.
                    const bool unknown = caster.Bounds.Min.x >= std::numeric_limits<f32>::max() ||
                                         previous->second.Bounds.Min.x >= std::numeric_limits<f32>::max();
                    invalidate(unknown ? NoBounds : caster.Bounds.Union(previous->second.Bounds));
                }
                next.insert_or_assign(caster.Key, caster);
            }
            for (const auto& [key, previous] : m_Previous)
            {
                if (!next.contains(key))
                    invalidate(previous.Bounds);
            }
            m_Previous = std::move(next);
        }

      private:
        std::unordered_map<u64, ShadowFamilyFootprint> m_Previous;
    };
} // namespace OloEngine

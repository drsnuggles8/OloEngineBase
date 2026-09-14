#pragma once

#include "MorphTarget.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Threading/Mutex.h"
#include "OloEngine/Threading/UniqueLock.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
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

        [[nodiscard("target index needed for weight mapping")]] i32 FindTarget(std::string_view name) const
        {
            auto count = static_cast<i32>(Targets.size());
            for (i32 i = 0; i < count; ++i)
            {
                if (name == Targets[static_cast<sizet>(i)].Name)
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
                TUniqueLock lock(m_CacheMutex);
                m_NameIndexCache.clear(); // Invalidate cache
                m_MaxDisplacement.reset();
                m_Compatibility.reset();
            }
            Targets.push_back(std::move(target));
            return true;
        }

        /**
         * @brief Why a morph target set cannot deform a given mesh.
         *
         * A morph stream that does not span the mesh it is attached to is the
         * "missing morph stream" of issue #1227, and it must be nameable: the
         * failure it produces otherwise is a face that deforms at the wrong
         * vertices, which reads as a broken rig rather than as mismatched data.
         */
        enum class ECompatibility : u8
        {
            Compatible = 0,
            /// The set has no targets at all.
            Empty,
            /// A dense target's delta array is a different length from the mesh.
            DenseVertexCountMismatch,
            /// A sparse target addresses a vertex the mesh does not have.
            SparseIndexOutOfRange,
        };

        [[nodiscard("compatibility result decides whether the mesh may be morphed")]] static std::string_view
        ToString(ECompatibility compatibility)
        {
            switch (compatibility)
            {
                case ECompatibility::Compatible:
                    return "Compatible";
                case ECompatibility::Empty:
                    return "Empty";
                case ECompatibility::DenseVertexCountMismatch:
                    return "DenseVertexCountMismatch";
                case ECompatibility::SparseIndexOutOfRange:
                    return "SparseIndexOutOfRange";
            }
            return "Unknown";
        }

        /**
         * @brief Whether this set can deform a mesh of @p meshVertexCount vertices.
         *
         * Checked against the MESH, not against the set's own first target:
         * GetVertexCount() reads Targets[0].Vertices.size(), which is zero for a
         * sparse target and therefore says nothing at all about a sparse set.
         */
        [[nodiscard("an incompatible morph set must not be applied")]] ECompatibility
        CheckCompatibility(u32 meshVertexCount) const
        {
            if (Targets.empty())
                return ECompatibility::Empty;

            // Cached on the vertex count it was asked about, like GetMaxDisplacement
            // below. The deformation pass asks every frame and the answer is a
            // property of immutable imported data: a 52-blendshape facial rig with
            // sparse targets is on the order of a million delta entries, and
            // re-walking them per character per tick is pure waste.
            {
                TUniqueLock lock(m_CacheMutex);
                if (m_Compatibility.has_value() && m_CompatibilityVertexCount == meshVertexCount)
                    return *m_Compatibility;
            }

            const ECompatibility result = CheckCompatibilityUncached(meshVertexCount);

            {
                TUniqueLock lock(m_CacheMutex);
                m_Compatibility = result;
                m_CompatibilityVertexCount = meshVertexCount;
            }
            return result;
        }

      private:
        [[nodiscard]] ECompatibility CheckCompatibilityUncached(u32 meshVertexCount) const
        {
            for (const auto& target : Targets)
            {
                if (target.IsSparse)
                {
                    for (const auto& entry : target.SparseVertices)
                    {
                        if (entry.VertexIndex >= meshVertexCount)
                            return ECompatibility::SparseIndexOutOfRange;
                    }
                }
                else if (target.Vertices.size() != static_cast<sizet>(meshVertexCount))
                {
                    return ECompatibility::DenseVertexCountMismatch;
                }
            }
            return ECompatibility::Compatible;
        }

      public:
        /**
         * @brief Upper bound, in object space, on how far this set can move any vertex.
         *
         * Weights are clamped to [0, 1] and combine additively, so no vertex can be
         * displaced further than the sum over targets of that target's largest delta.
         * Conservative by construction and independent of the current weights, which
         * is what lets culling use it without re-measuring every frame (#1227
         * criterion 3: bounds must enclose combined skeletal AND morph motion).
         *
         * Cached: the set is immutable once imported, and this is an O(targets x
         * vertices) sweep over data that can run to millions of deltas.
         */
        [[nodiscard("the displacement bound must reach the culling expansion")]] f32 GetMaxDisplacement() const
        {
            TUniqueLock lock(m_CacheMutex);
            if (!m_MaxDisplacement.has_value())
            {
                f32 total = 0.0f;
                for (const auto& target : Targets)
                {
                    f32 targetMax = 0.0f;
                    const auto accumulate = [&targetMax](const glm::vec3& delta)
                    {
                        const f32 length = glm::length(delta);
                        if (std::isfinite(length))
                            targetMax = std::max(targetMax, length);
                    };
                    if (target.IsSparse)
                    {
                        for (const auto& entry : target.SparseVertices)
                            accumulate(entry.Delta.DeltaPosition);
                    }
                    else
                    {
                        for (const auto& vertex : target.Vertices)
                            accumulate(vertex.DeltaPosition);
                    }
                    total += targetMax;
                }
                m_MaxDisplacement = std::isfinite(total) ? total : 0.0f;
            }
            return *m_MaxDisplacement;
        }

        // O(1) name-to-index lookup via cached map
        [[nodiscard("cached target index needed for weight mapping")]] i32 FindTargetCached(const std::string& name) const
        {
            TUniqueLock lock(m_CacheMutex);
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
            auto targetCount = static_cast<i32>(Targets.size());
            for (i32 i = 0; i < targetCount; ++i)
                m_NameIndexCache[Targets[static_cast<sizet>(i)].Name] = i;
        }

        // Guards the mutable cache so const lookups can rebuild it from multiple
        // threads without a data race (S8379).
        mutable FMutex m_CacheMutex;
        mutable std::unordered_map<std::string, i32> m_NameIndexCache;
        // Guarded by m_CacheMutex, like the name cache above.
        mutable std::optional<f32> m_MaxDisplacement;
        mutable std::optional<ECompatibility> m_Compatibility;
        mutable u32 m_CompatibilityVertexCount = 0;
    };
} // namespace OloEngine

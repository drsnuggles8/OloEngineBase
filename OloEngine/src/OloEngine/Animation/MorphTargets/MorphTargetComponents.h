#pragma once

#include "MorphTargetSet.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Scene/ComponentReflection.h" // OLO_SERIALIZE(Skip) to mark runtime fields

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    struct MorphTargetComponent
    {
        // Morph target data for this entity's mesh (runtime — loaded from the mesh)
        OLO_SERIALIZE(Skip)
        Ref<MorphTargetSet> MorphTargets;

        // Per-target weights (target name -> weight 0.0 to 1.0) — the authored data
        std::unordered_map<std::string, f32> Weights;

        // Cached base mesh data for CPU morph evaluation (populated once from MeshSource)
        OLO_SERIALIZE(Skip)
        std::vector<glm::vec3> BasePositions;
        OLO_SERIALIZE(Skip)
        std::vector<glm::vec3> BaseNormals;

        // Tracks whether morph weights were active last frame (for transition detection).
        // NOT OLO_SERIALIZE(Skip): master exposes this plain bool in the MCP field registry,
        // and a master test (McpFieldRegistry.ListFieldsOnEmptyWeightsMapReportsNoMapEntries)
        // relies on it keeping the component listable when the Weights map is empty. Skipping
        // it would silently change shipping MCP behavior — the component is hand-written in the
        // scene serializer (kComponentsCustomSerialize), so this field is not scene-persisted
        // either way; Skip here only ever affected MCP.
        bool WasMorphActive = false;

        // --- Shared animated-surface state (#1227). Runtime only. -----------------
        //
        // The morph half of the deformation history the skeletal producer owns for
        // bones. MorphDeformationSystem writes all of it at the frame boundary; no
        // other code should.

        /// Ordered weights the deformed surface currently on the GPU was built from.
        OLO_SERIALIZE(Skip)
        std::vector<f32> AppliedWeights;
        /// Ordered weights the PREVIOUS rendered frame's surface was built from.
        OLO_SERIALIZE(Skip)
        std::vector<f32> PrevAppliedWeights;
        /// True once the morph pass has decided a surface for this entity at all.
        /// NOT `!AppliedWeights.empty()`: an entity with every weight at zero has a
        /// perfectly good surface — the undeformed one — described by the EMPTY
        /// weight vector, and conflating the two makes a face that starts neutral
        /// never acquire history and therefore never reject on its first expression.
        OLO_SERIALIZE(Skip)
        bool HasAppliedSurface = false;
        /// True when PrevAppliedWeights describes a surface this entity really drew.
        OLO_SERIALIZE(Skip)
        bool HasMorphHistory = false;
        /// True while the current frame's morphed surface is not the previous frame's
        /// and the renderer therefore cannot express the difference as a velocity.
        OLO_SERIALIZE(Skip)
        bool RejectDeformationHistory = false;

        // Identity of the data the BasePositions/BaseNormals cache was taken from.
        // Keyed on the MORPH SET rather than on the mesh source: the component holds
        // a strong Ref to the set, so that address cannot be recycled underneath the
        // key while the cache lives — a raw MeshSource pointer can be, and two LOD
        // levels produced by the index-only generator have the same vertex count, so
        // a size check alone does not separate them.
        OLO_SERIALIZE(Skip)
        const MorphTargetSet* BaseCacheKey = nullptr;
        OLO_SERIALIZE(Skip)
        u32 BaseCacheVertexCount = 0;

        /// The surface the cache was taken FROM, held strongly.
        ///
        /// The morph pass deforms a MeshSource's vertex buffer in place, so the
        /// cached rest surface and the mesh it belongs to are one fact: dropping
        /// the cache without first writing it back leaves that mesh deformed
        /// forever, and a later activation then re-caches the deformed vertices as
        /// the base surface and compounds the expression on itself.
        ///
        /// Restoring through whatever mesh happens to be current at the time is not
        /// good enough — after a mesh swap that handle is the NEW surface, and two
        /// meshes of equal vertex count would silently take each other's rest data.
        /// Holding the source makes the restore always target the right mesh, and
        /// (being strong) also makes BaseCacheKey's address non-recyclable.
        OLO_SERIALIZE(Skip)
        Ref<MeshSource> BaseCacheSource;

        /// Weights refused because they were not finite, cumulative for the session.
        OLO_SERIALIZE(Skip)
        u32 RejectedWeightCount = 0;

        /// How many unknown target names have already been reported to the engine
        /// counters. The morph pass runs every frame and the counter it feeds is a
        /// session total, so without this a single misnamed weight would add 60 per
        /// second and the total would measure wall-clock time, not bad names.
        OLO_SERIALIZE(Skip)
        u32 ReportedUnknownTargets = 0;

        /// A morph set already refused as incompatible with this entity's surface.
        /// Remembered so the refusal happens ONCE per set rather than once per
        /// frame: the mesh keeps offering the same set, and a warning that repeats
        /// sixty times a second is as unreadable as no warning at all.
        ///
        /// A strong Ref rather than a raw pointer, so the address cannot be freed
        /// and reused by a DIFFERENT set that would then be refused on the strength
        /// of a stale identity.
        OLO_SERIALIZE(Skip)
        Ref<MorphTargetSet> RefusedSet;
        /// The vertex count the refusal was decided at. A set is only still refused
        /// for a surface of the SAME size: a MorphTargetSet can be shared between
        /// meshes (MeshSource::SetMorphTargets takes any set, and an authored LOD
        /// group can point at arbitrary meshes), so one that does not span surface A
        /// may span surface B perfectly well. Keying the refusal on the set alone
        /// would blacklist it everywhere, permanently, after one bad pairing.
        OLO_SERIALIZE(Skip)
        u32 RefusedSetVertexCount = 0;

        MorphTargetComponent() = default;
        MorphTargetComponent(const MorphTargetComponent&) = default;

        /**
         * @brief Set one target's weight, refusing a value that is not finite.
         *
         * std::clamp cannot reject a NaN: every comparison against it is false, so
         * clamp returns the NaN unchanged. One NaN weight propagates through the
         * evaluator into every vertex it touches and the whole mesh vanishes, with
         * nothing logged — the weight arrives from script, from Lua, from a scene
         * file and from an MCP write, so it is untrusted on four routes.
         *
         * @return true when the weight was stored.
         */
        bool SetWeight(const std::string& targetName, f32 weight)
        {
            if (!std::isfinite(weight))
            {
                ++RejectedWeightCount;
                OLO_CORE_WARN("MorphTargetComponent::SetWeight: refused a non-finite weight for target '{}' "
                              "(refused {} so far) — the previous value is kept",
                              targetName, RejectedWeightCount);
                return false;
            }
            Weights[targetName] = std::clamp(weight, 0.0f, 1.0f);
            return true;
        }

        [[nodiscard("weight value must be used")]] f32 GetWeight(const std::string& targetName) const
        {
            auto it = Weights.find(targetName);
            return (it != Weights.end()) ? it->second : 0.0f;
        }

        void ResetAllWeights()
        {
            for (auto& [name, weight] : Weights)
            {
                weight = 0.0f;
            }
        }

        // Check if any morph target has a non-zero weight
        [[nodiscard("active weight check drives morph target updates")]] bool HasActiveWeights() const
        {
            return std::ranges::any_of(Weights, [](const auto& kv)
                                       { return kv.second > 1e-4f; });
        }

        /// Ordered weights plus what was wrong with the request, so a caller can
        /// report a weight that named a target this mesh does not have instead of
        /// dropping it where nobody can see.
        struct FOrderedWeights
        {
            std::vector<f32> Weights;
            /// Authored weights naming a target absent from the bound MorphTargetSet.
            u32 UnknownTargets = 0;
        };

        // Build a flat weight vector matching the MorphTargetSet order
        [[nodiscard("ordered weights and the unknown-target count must both be used")]] FOrderedWeights
        GetOrderedWeightsChecked() const
        {
            FOrderedWeights result;
            if (!MorphTargets)
            {
                // Every authored weight is unaddressable without a set to order it by.
                result.UnknownTargets = static_cast<u32>(Weights.size());
                return result;
            }

            result.Weights.assign(MorphTargets->GetTargetCount(), 0.0f);
            for (const auto& [name, weight] : Weights)
            {
                const i32 idx = MorphTargets->FindTargetCached(name);
                if (idx < 0)
                {
                    ++result.UnknownTargets;
                    continue;
                }
                result.Weights[static_cast<sizet>(idx)] = weight;
            }
            return result;
        }

        [[nodiscard("ordered weights needed for GPU upload")]] std::vector<f32> GetOrderedWeights() const
        {
            return GetOrderedWeightsChecked().Weights;
        }

        /// Drop the cached base surface and the morph history. Called when the data
        /// the cache was taken from is no longer the data being deformed.
        void InvalidateBaseCache()
        {
            BasePositions.clear();
            BaseNormals.clear();
            BaseCacheKey = nullptr;
            BaseCacheVertexCount = 0;
            BaseCacheSource = nullptr;
            WasMorphActive = false;
            // The unknown-target report was about the set that is going away.
            ReportedUnknownTargets = 0;
        }

        /// True when dropping the base cache would actually lose something. The
        /// caller counts the invalidation only then, so the session total reads as
        /// "a cached surface was thrown away", not "a cache that was empty anyway
        /// was cleared again".
        [[nodiscard("the answer decides whether the invalidation is counted")]] bool HasCachedBaseSurface() const
        {
            return !BasePositions.empty();
        }
    };
} // namespace OloEngine

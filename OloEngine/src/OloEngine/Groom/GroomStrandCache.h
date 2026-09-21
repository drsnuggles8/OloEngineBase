#pragma once

// =============================================================================
// GroomStrandCache.h — the per-(asset, settings) strand geometry, owned by
// neither pass. Issue #1323.
//
// WHY IT IS NOT IN GroomRenderPass ANY MORE. It was, from #1246 to #1252, and
// that placement is exactly what made a groom unable to cast a shadow:
// `ShadowRenderPass::Execute` runs BEFORE `GroomRenderPass::Execute`, so at the
// moment the shadow map is rasterised the buffers a groom caster would draw did
// not exist yet. Making a groom a caster is therefore a lift of this cache into
// something both passes can reach — not a new caster list, which is what a sixth
// caster family normally is. `docs/agent-rules/groom-coat-self-shadowing.md`
// named this as the structural obstacle when #1248 declared the scene-shadow
// half out of scope.
//
// WHO DRIVES IT. `RenderPipeline` owns one instance and calls `BeginFrame()`
// once per frame, before the graph executes; the two passes then ACQUIRE from
// it in graph order. The shadow pass acquires first and pays for the build; the
// strand pass acquires second and hits. Neither owns the other and neither has
// to run for the other to work — a frame in which the shadow pass is skipped
// still builds on the strand pass's acquire, and vice versa.
//
// THE TICK IS THE CACHE'S OWN, not GroomFrameState::FrameIndex. That one is the
// stochastic sample index and is deliberately wrapped (`& 0xFFFFF` in
// RenderPipeline), so an entry used just before the wrap reads as a million
// frames old and is evicted, while one from the previous cycle reads as newly
// used and stays. A 64-bit counter only this class advances cannot do either.
//
// ONE BUILD PER KEY PER FRAME, and that is a consequence of the lift rather
// than a tidy-up: a DEFORMED groom is rebuilt and its vertex buffer refilled
// every frame, so with two consumers acquiring the same key the build would run
// twice — twice the CPU cost and a DeformedRebuilds counter that reads double
// for a coat that is behaving. `LastUsedTick` is what makes the second acquire
// of a frame a hit.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Groom/GroomCoatShadowTechnique.h"
#include "OloEngine/Groom/GroomStrandMesh.h"
#include "OloEngine/Groom/GroomStrandRequest.h"

#include <glm/glm.hpp>

#include <unordered_map>

namespace OloEngine
{
    class IndexBuffer;
    class Texture3D;
    class VertexArray;
    class VertexBuffer;

    /// What the cache DID this frame, for the panel and the PR's evidence.
    /// Reset by BeginFrame, so every number is per frame rather than cumulative.
    struct GroomStrandCacheStats
    {
        /// Geometry builds and evictions.
        u32 CacheBuilds = 0;
        u32 CacheEvictions = 0;
        /// Dynamic vertex-buffer refills a deformed groom cost this frame. One
        /// per deformed groom is healthy; more means the geometry is being
        /// reallocated, not refilled.
        u32 DeformedRebuilds = 0;
        /// Coat-volume rebuilds (#1248).
        u32 CoatRebuilds = 0;

        void Reset() noexcept
        {
            *this = GroomStrandCacheStats{};
        }
    };

    /// What the SHADOW passes did with grooms this frame (#1323).
    ///
    /// It lives on the shared cache rather than on ShadowRenderPass because of
    /// where it is READ: the editor has exactly one groom readout, hanging off
    /// GroomRenderPass, and "why does this coat cast no shadow" has to be
    /// answerable from the same panel that asks for the shadow. The cache is
    /// the one frame-scoped object both passes already hold, so routing the
    /// tally through it costs no new accessor and no new ownership.
    struct GroomShadowCasterStats
    {
        /// Requests whose GroomSceneShadowComponent asked them to cast.
        u32 GroomsAskedToCast = 0;
        /// Of those, the ones that had geometry and were submitted as casters.
        u32 GroomsCasting = 0;
        /// Of those, the ones that produced NO geometry — an empty build, or a
        /// budget that selected nothing. Non-zero is the honest answer to a
        /// coat that renders and casts nothing, and it is a different fault
        /// from "the family was never wired into this technique".
        u32 GroomsWithoutGeometry = 0;

        /// Draws issued, per technique. THREE COUNTERS RATHER THAN ONE, and
        /// that is the point: virtual-geometry-into-a-second-shadow-technique.md
        /// says a family reaches a technique only if somebody wired it there and
        /// that NOTHING detects the gap. A zero here, next to a non-zero
        /// GroomsCasting, is what detects it.
        u32 CascadeDraws = 0;
        u32 AtlasDraws = 0;
        u32 VirtualShadowLevelDraws = 0;

        /// Which directional technique owned the sun this frame, so a zero in
        /// one of the two directional counters can be read as "not this
        /// frame's technique" rather than as a hole.
        bool VirtualShadowMapActive = false;

        void Reset() noexcept
        {
            *this = GroomShadowCasterStats{};
        }
    };

    class GroomStrandCache : public RefCounted
    {
      public:
        // One groom's GPU geometry, keyed by asset handle AND build settings.
        struct Entry
        {
            Ref<VertexArray> Array;
            Ref<VertexBuffer> Vertices;
            Ref<IndexBuffer> Indices;
            GroomStrandBuildSettings Settings;
            GroomStrandMeshStats Stats;
            u64 Bytes = 0;
            u64 LastUsedTick = 0;

            /// True when the buffers were created for repeated refills, which is
            /// what a bound groom needs: its vertices change every frame with
            /// the body's pose. A static entry's buffers are immutable and must
            /// never be handed to the refill path.
            bool Dynamic = false;

            // ── Coat self-shadowing (#1248) ─────────────────────────

            // The bake lives in the GEOMETRY cache, keyed the same way, because
            // it is a function of the same two things the geometry is: the
            // groom asset and the build settings. Keeping it anywhere else
            // would need a second eviction policy that could disagree with this
            // one about when a coat is still in use.

            /// The packed RGBA32F volume: xyz = the voxel's mean fibre
            /// direction times its coherence, w = fibre areal density. Null
            /// until the first successful bake.
            Ref<Texture3D> CoatVolume;
            /// The volume's object-space box, needed to map a shading point
            /// into it.
            glm::vec3 CoatBoundsMin{ 0.0f };
            glm::vec3 CoatBoundsMax{ 0.0f };
            /// Voxels on the longest axis of the bake actually resident.
            u32 CoatResolution = 0;
            /// The bake's REAL voxel size, carried rather than re-derived. Only
            /// the longest axis gets `CoatResolution` voxels, so
            /// extent/resolution is that axis's voxel size and nobody else's.
            f32 CoatVoxelSize = 0.0f;
            /// The width scale the bake was made at. It multiplies the cooked
            /// diameters and therefore the areal density stored, so it invalidates.
            f32 CoatWidthScale = 0.0f;
            /// GPU bytes the volume occupies.
            u64 CoatBytes = 0;
            /// The LOD step the resident bake was made at, the step the policy
            /// is currently ASKING for, and how many consecutive frames it has
            /// asked for it. The three together are the hysteresis state.
            u32 CoatLodStep = 0;
            u32 CoatRequestedLodStep = 0;
            u32 CoatLodStableFrames = 0;
            /// The cache tick the volume was last rebuilt at, so staleness is a
            /// number rather than an impression.
            u64 CoatBuiltTick = 0;

            /// The cache tick this entry's GEOMETRY bytes were last counted
            /// against a representation (#1252). Two unbound entities sharing
            /// one groom asset at one budget share ONE entry, so adding its
            /// bytes per DRAW would report the same allocation twice.
            u64 BytesCountedTick = 0;

            /// The mode the resident bake serves.
            GroomCoatShadowTechnique CoatMode = GroomCoatShadowTechnique::None;
        };

        /// Evict to budget, then advance the tick. Called ONCE per frame by
        /// RenderPipeline, before the graph executes.
        ///
        /// EVICTION HAPPENS HERE rather than at the end of a pass, and the move
        /// is what the lift made necessary: with two consumers there is no
        /// longer a single "last groom draw of the frame" to hang it on, and an
        /// entry freed between the shadow acquire and the strand draw would be
        /// freed with a recorded draw still pointing at it. At the top of a
        /// frame nothing this frame has been acquired yet, so the retention
        /// window is the only thing eviction has to respect.
        void BeginFrame();

        [[nodiscard]] u64 GetTick() const noexcept
        {
            return m_Tick;
        }

        /// Strand-geometry cache key: the asset handle AND the build settings.
        ///
        /// Public because it is a pure function and the cache's correctness
        /// rests on it: it is hashed FIELD BY FIELD rather than over the object
        /// representation, because GroomStrandBuildSettings carries
        /// uninitialised padding, and that distinction is only defensible if
        /// something tests it.
        [[nodiscard]] static u64 CacheKey(const GroomStrandRequest& request) noexcept;

        /// Whether this request's geometry is per-ENTITY rather than per-asset.
        ///
        /// A bound groom's vertices depend on a body's pose, so two entities
        /// sharing one groom asset cannot share one buffer.
        [[nodiscard]] static bool IsDeformed(const GroomStrandRequest& request) noexcept;

        /// The entry for `request`, building it if the cache does not hold one.
        /// Null when the groom produced no geometry at all, which is a
        /// legitimate state (a guides-only view of a groom with no guides) and
        /// not an error.
        [[nodiscard]] Entry* AcquireGeometry(const GroomStrandRequest& request);

        /// Rebuilds `entry`'s coat volume if the request needs one and the
        /// resident bake is not already right. Returns the decision, so the
        /// caller records the reason rather than re-deriving it.
        ///
        /// `viewportHeight` is the SHADING target's height, which is what the
        /// coat's apparent size is measured in. Only the strand pass calls this
        /// — a shadow caster needs the geometry and never the bake.
        /// The resident-volume BUDGET IS THE CACHE'S OWN, not a counter the
        /// caller carries. Carried, it desynced: the two refusal paths here
        /// release a resident volume, and so do eviction and the geometry
        /// rebuild, and none of those is on the caller's path -- so the count
        /// read high and a later coat could be denied a slot or trigger a
        /// pointless reclaim. See ReleaseCoatVolume.
        [[nodiscard]] GroomCoatShadowDecision AcquireCoatVolume(const GroomStrandRequest& request, Entry& entry,
                                                                f32 viewportHeight);

        /// Coat volumes currently held across the WHOLE cache, not just the
        /// ones drawn this frame.
        [[nodiscard]] u32 CountResidentCoatVolumes() const noexcept;

        /// Upper bound on the cache, in bytes.
        void SetBudgetBytes(u64 bytes) noexcept
        {
            m_BudgetBytes = bytes;
        }

        [[nodiscard]] u64 GetBytes() const noexcept
        {
            return m_Bytes;
        }
        [[nodiscard]] u32 GetEntryCount() const noexcept
        {
            return static_cast<u32>(m_Entries.size());
        }
        [[nodiscard]] const GroomStrandCacheStats& GetStats() const noexcept
        {
            return m_Stats;
        }

        /// This frame's shadow-caster tally. Written by ShadowRenderPass, read
        /// by GroomRenderPass for the editor's one groom readout; reset by
        /// BeginFrame with everything else.
        [[nodiscard]] GroomShadowCasterStats& MutableShadowStats() noexcept
        {
            return m_ShadowStats;
        }
        [[nodiscard]] const GroomShadowCasterStats& GetShadowStats() const noexcept
        {
            return m_ShadowStats;
        }

        /// The width scale a groom is DRAWN at: the authored lever times the
        /// representation LOD's coverage compensation (#1252).
        ///
        /// SHARED BECAUSE THE CASTER AND THE DRAW MUST AGREE. The strand pass
        /// widens its ribbons by this and the shadow pass widens its casters by
        /// the same, so a coat thickened to compensate for a spent strand
        /// budget casts the shadow of the coat that is on screen rather than of
        /// the one the budget left. Two copies of the expression would drift
        /// the moment either changed, and the symptom would be a shadow
        /// slightly the wrong size — which nobody reads as a bug.
        [[nodiscard]] static f32 EffectiveWidthScale(const GroomStrandRequest& request, const Entry& entry) noexcept;

        /// Drops every entry. The cache holds GPU buffers whose device is going
        /// away, so this runs on a renderer reset rather than leaving them to
        /// be rebuilt against a dead context.
        void Clear();

      private:
        /// Frees the least-recently-used coat volume that is NOT in use this
        /// frame. Returns false when every resident volume belongs to a groom
        /// acquired this frame, which is the honest "budget really is full" case.
        bool ReclaimLeastRecentlyUsedCoatVolume();
        void EvictToBudget();

        std::unordered_map<u64, Entry> m_Entries;
        GroomStrandCacheStats m_Stats;
        GroomShadowCasterStats m_ShadowStats;
        /// Entries holding a coat volume right now. Maintained by every site
        /// that creates or releases one, so CountResidentCoatVolumes is a read
        /// rather than a walk and the budget cannot drift.
        u32 m_ResidentCoatVolumes = 0;
        u64 m_BudgetBytes = 256ull * 1024ull * 1024ull;
        u64 m_Bytes = 0;
        u64 m_Tick = 0;
    };
} // namespace OloEngine

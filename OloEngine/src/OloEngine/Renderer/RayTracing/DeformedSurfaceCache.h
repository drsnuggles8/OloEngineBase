#pragma once

// =============================================================================
// DeformedSurfaceCache.h — the deformed-vertex producer's CPU half. Issue #1229.
//
// WHAT THIS OWNS. One persistent vertex buffer per animated surface, holding
// that surface's skinned-and-morphed vertices in OloEngine::Vertex layout, plus
// the per-frame dispatch queue that fills them and the bone palettes those
// dispatches read.
//
// WHY IT EXISTS AT ALL. Every raster consumer of a skinned surface deforms it
// inside its own vertex stage and keeps nothing (skeletal-deformation-shared-
// output.md), so before this class no deformed vertex existed anywhere in
// memory. A BLAS is built from memory, so #1228's animated GPU Scene records
// were tracing their REST pose. Materializing the surface is the whole of
// criterion 1, and it is a producer, not a second scene: the output buffer
// replaces one field of the canonical geometry record (its VertexAddress) and
// nothing else.
//
// WHY A VertexBuffer AND NOT A StorageBuffer. The output has to satisfy three
// unrelated demands at once — written by a compute dispatch, read by
// vkCmdBuildAccelerationStructuresKHR, and read by the hit shaders through a
// device address. VulkanVertexBuffer already creates with exactly that set
// (STORAGE_BUFFER | SHADER_DEVICE_ADDRESS | ACCELERATION_STRUCTURE_BUILD_INPUT_
// READ_ONLY), and the AS-build bit is already gated on the device capability
// the way VUID-VkBufferCreateInfo-None-09499 requires
// (vulkan-ray-tracing-acceleration-structures.md §8b). A StorageBuffer has no
// AS-build-input bit and cannot be handed to a build at all.
//
// VULKAN ONLY, BY CONSTRUCTION. Nothing is ever allocated unless the RT scene
// reports itself available, so an OpenGL session allocates no deformed buffers,
// dispatches nothing, and stages exactly the records it staged before this
// issue. That is what "preserve the tested non-RT rendering" means here: the
// raster path is not branched around, it is untouched.
//
// THREADING. Render thread only. Acquire() is called from inside
// Renderer3D::ExtractGPUSceneMesh during extraction; Dispatch() is called from
// the render graph node that runs immediately before RayTracingScenePass.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"

#include <glm/glm.hpp>

#include <limits>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include "OloEngine/Containers/Array.h"

namespace OloEngine
{
    class ComputeShader;
    class MeshSource;
    class StorageBuffer;
    class UniformBuffer;
    class VertexBuffer;
} // namespace OloEngine

namespace OloEngine::RayTracing
{
    // -------------------------------------------------------------------------
    // Identity
    // -------------------------------------------------------------------------

    // One deformed surface is one ENTITY deforming one REST VERTEX BUFFER.
    //
    // Per entity because the pose is per entity: two characters sharing a mesh
    // share the rest buffer and must not share the deformed one, which is the
    // single reason a per-geometry BLAS cannot serve animated surfaces.
    //
    // Per rest buffer rather than per MeshSource POINTER because a conventional
    // LOD switch hands the entity a different MeshSource, and a freed one can
    // be replaced by a new one at the same address — the same recycling trap
    // MorphTargetComponent::BasePositions keys around
    // (morph-and-lod-in-the-animated-surface.md). An RHI::ResourceHandle
    // carries a generation, so it cannot be mistaken for its predecessor.
    struct DeformedSurfaceKey
    {
        u64 EntityId = 0;
        u64 RestVertexBuffer = 0; ///< RHI::ResourceHandle, packed.

        [[nodiscard]] auto operator==(const DeformedSurfaceKey&) const -> bool = default;
    };

    struct DeformedSurfaceKeyHash
    {
        [[nodiscard]] sizet operator()(const DeformedSurfaceKey& key) const noexcept
        {
            // FNV-1a over the two lanes. Not a shift-and-xor: the entity id and
            // the handle are both dense small integers in practice, and xoring
            // them collides every (entity, buffer) pair with its transpose.
            u64 hash = 1469598103934665603ull;
            const auto mix = [&hash](u64 value)
            {
                hash ^= value;
                hash *= 1099511628211ull;
            };
            mix(key.EntityId);
            mix(key.RestVertexBuffer);
            return static_cast<sizet>(hash);
        }
    };

    // What Acquire hands back. An invalid result is the normal answer on every
    // path that is not a live animated surface on an RT device, and it means
    // "stage the rest buffer, exactly as before this issue".
    struct DeformedSurfaceBinding
    {
        RHI::ResourceHandle Handle{};
        u64 DeviceAddress = 0;
        // NO `Reallocated` lane. A reallocation moves the buffer's DEVICE
        // ADDRESS, which is already one of the fields FingerprintGeometry
        // hashes, so RayTracingScene sees it as GeometryChanged and rebuilds
        // without being told twice. A second spelling of one fact is a second
        // thing that can disagree with it.
        //
        // Increments ONLY when this surface's vertices were actually rewritten.
        // It is what tells a consumer "the geometry under your acceleration
        // structure moved", and the skeleton's deformation revision cannot
        // answer that: #1226 advances bone history once per frame for every
        // skinned entity WHETHER OR NOT IT ANIMATED, so that revision ticks
        // every frame for a character standing perfectly still. Measured on the
        // fox scene in edit mode — not playing, pose fixed — it produced a
        // refit every frame and a full rebuild every eighth.
        //
        // This counter is derived from the palette CONTENTS instead, so an idle
        // surface holds it and every consumer of it stands still too.
        u32 ContentRevision = 0;

        [[nodiscard]] bool IsValid() const
        {
            return Handle.IsValid() && DeviceAddress != 0u;
        }
    };

    // -------------------------------------------------------------------------
    // Telemetry
    // -------------------------------------------------------------------------

    // Criterion 4's "build/refit timings, memory, rebuild reasons" half that
    // belongs to the PRODUCER rather than to the acceleration structures. Kept
    // separate from RayTracing::SceneStats for the reason that block states in
    // its own header: a counter whose zero has two readings is a counter nobody
    // can act on, and "no surfaces were offered" must not read like "surfaces
    // were offered and refused".
    struct DeformedSurfaceStats
    {
        // Standing.
        u32 ResidentSurfaces = 0; ///< Deformed buffers currently allocated.
        u64 ResidentBytes = 0;    ///< What those buffers hold, bytes.
        u64 PaletteBytes = 0;     ///< The shared per-frame palette staging buffer.

        // Per frame.
        u32 SurfacesRequested = 0; ///< Acquire() calls that named a live animated surface.
        u32 Dispatched = 0;        ///< Compute dispatches recorded.
        u32 VerticesDeformed = 0;  ///< Vertices those dispatches covered.
        u32 Allocated = 0;         ///< Buffers created this frame (a new surface).
        u32 Reallocated = 0;       ///< Buffers replaced this frame (capacity grew).
        u32 Retired = 0;           ///< Buffers released this frame (despawn, LOD switch).
        // A surface whose pose did not advance since the last dispatch. It is
        // SKIPPED, not re-deformed, and the acceleration structure is not
        // refitted for it either — which is the single largest saving in a
        // scene where most characters are idle. Non-zero is healthy.
        u32 SkippedUnchanged = 0;
        // A surface that could not be produced: no bone influence buffer, no
        // device address, a vertex count of zero, or an allocation that failed.
        // It falls back to its rest buffer and is COUNTED rather than silently
        // traced at rest — a T-posed character in a reflection with nothing
        // said about it is the exact failure this issue exists to remove.
        u32 Refused = 0;

        // NO GPU-TIME FIELD HERE, deliberately. SkeletalDeformPass brackets its
        // dispatches with GPUPassTimerPool::BeginSubPass("SkeletalDeformToBuffer"),
        // so the deformation's GPU cost is reported through the same per-pass
        // channel as every other pass and needs no counter of its own.
        //
        // The alternative was tried in this very header's neighbour and does
        // not work: RayTracing::FrameCounters has BlasBuildGpuNs and
        // TlasBuildGpuNs, both declared by #978, both read by the Statistics
        // panel, and neither ever WRITTEN by anything. They read zero forever,
        // and the panel hides them behind `if (> 0)`, so nothing says so. A
        // field that is always zero is worse than an absent one — it answers
        // the question wrongly instead of sending the reader to the channel
        // that can answer it.

        void ResetFrame()
        {
            SurfacesRequested = 0;
            Dispatched = 0;
            VerticesDeformed = 0;
            Allocated = 0;
            Reallocated = 0;
            Retired = 0;
            SkippedUnchanged = 0;
            Refused = 0;
        }

        [[nodiscard]] auto operator==(const DeformedSurfaceStats&) const -> bool = default;
    };

    // -------------------------------------------------------------------------
    // The cache
    // -------------------------------------------------------------------------

    class DeformedSurfaceCache
    {
      public:
        DeformedSurfaceCache();
        // OUT OF LINE, and it has to be. The Ref<> members below name types
        // this header only forward-declares, and ~Ref needs a complete type to
        // release. Defaulting the destructor here would push that requirement
        // onto every translation unit that so much as declares one of these —
        // which, since Renderer3D.h owns one, is most of the renderer.
        ~DeformedSurfaceCache();

        DeformedSurfaceCache(const DeformedSurfaceCache&) = delete;
        DeformedSurfaceCache& operator=(const DeformedSurfaceCache&) = delete;

        // Arm or disarm the whole mechanism. Called from Renderer3D once the RT
        // scene has reported its capability. While disabled, Acquire() refuses
        // everything without counting a refusal — there is nothing to refuse,
        // the feature is off — and every resident buffer is released.
        void SetEnabled(bool enabled);

        [[nodiscard]] bool IsEnabled() const
        {
            return m_Enabled;
        }

        void Shutdown();

        // Start a frame's extraction. Clears the dispatch queue and the palette
        // staging; resident buffers survive.
        //
        // The frame counter is the cache's OWN, not GPU Scene's. All it has to
        // be is monotonic, because its only use is "was this surface offered
        // during the current extraction" — and borrowing GPU Scene's would mean
        // widening that class's public API for a value nothing else needs.
        void BeginFrame();

        // Ask for this surface's deformed stream, queueing the dispatch that
        // fills it if the pose has advanced since the last one.
        //
        // `palette` is the entity's current final bone matrices, and it is the
        // ONLY thing consulted to decide whether this surface needs re-skinning.
        // Copied, never retained: the caller owns them and rewrites them every
        // tick, so what is kept is a hash.
        //
        // Deliberately NOT SkeletonData's deformation revision, which is the
        // obvious candidate and is wrong. #1226 advances bone history once per
        // frame for every skinned entity whether or not it animated, so that
        // revision ticks every frame for a motionless character; driving the
        // skip from it makes the skip never fire. The palette bytes are the
        // pose.
        // `isAnimated` is the caller's own statement that this surface is one
        // this cache is FOR, and it is separate from the palette being non-empty
        // on purpose. Acquire is called for every staged submesh, rigid ones
        // included, so without it every rigid mesh in the scene would be
        // counted as a requested-and-refused animated surface and the one
        // counter that means "a character is missing from the TLAS" would read
        // in the thousands on a scene with no characters in it.
        // `morphStateHash` folds in everything OTHER than the bone palette that
        // changes this surface's vertices. It is not optional: morph deltas are
        // applied on the CPU straight into the rest vertex buffer
        // (Scene::EvaluateMorphTargets), so an expressing head with a fixed
        // skeleton pose rewrites the geometry while the palette does not move
        // one bit. Comparing the palette alone freezes that face at its first
        // expression in every ray-traced effect — and "an expressing head" is
        // one of the two subjects this issue names.
        [[nodiscard]] DeformedSurfaceBinding Acquire(const DeformedSurfaceKey& key, bool isAnimated,
                                                     const Ref<MeshSource>& meshSource,
                                                     std::span<const glm::mat4> palette, u64 morphStateHash);

        // The pose hash Acquire compares against. Exposed because it is the
        // testable half of the skip decision.
        [[nodiscard]] static u64 HashPalette(std::span<const glm::mat4> palette);

        // Release every buffer whose surface was not Acquired this frame, and
        // upload the frame's palettes. Called at the end of extraction, before
        // the graph runs.
        //
        // Retirement is by ABSENCE for the same reason GPU Scene's is: there is
        // no despawn signal to listen to, and a surface stops being offered the
        // frame its entity dies, its animation is removed, or its LOD switches
        // to a different rest buffer.
        void EndFrame();

        // Record this frame's dispatches into the frame command buffer. Called
        // by the graph node that runs immediately before RayTracingScenePass,
        // so that the compute writes are ordered before the builds that read
        // them. Returns the number of dispatches recorded.
        u32 Dispatch();

        [[nodiscard]] const DeformedSurfaceStats& GetStats() const
        {
            return m_Stats;
        }

        [[nodiscard]] bool HasWork() const
        {
            return !m_Queue.IsEmpty();
        }

        // --- Policy, exposed because it is the testable half -----------------

        // Bytes one surface's deformed stream needs. Named rather than
        // open-coded so the memory counter and the allocation cannot disagree
        // about what a vertex costs.
        [[nodiscard]] static u64 StreamBytes(u32 vertexCount);

        // Whether an existing buffer sized for `capacity` vertices can serve a
        // surface that now has `required`. Growth-only with no shrink: a
        // surface that loses vertices keeps its buffer, because a shrink buys
        // nothing and costs a rebuild of an acceleration structure that could
        // have refitted.
        [[nodiscard]] static bool CapacityServes(u32 capacity, u32 required);

        // What a new allocation is sized to. Rounded up so a surface that grows
        // by a vertex at a time does not reallocate — and therefore force a
        // BLAS rebuild — every frame.
        [[nodiscard]] static u32 CapacityFor(u32 vertexCount);

      private:
        struct Entry
        {
            Ref<VertexBuffer> Output;
            u32 Capacity = 0;    ///< Vertices the buffer can hold.
            u32 VertexCount = 0; ///< Vertices the surface had when last dispatched.
            u64 DeviceAddress = 0;
            u64 LastSeenFrame = 0;
            // The revision the resident contents were produced at, and whether
            // anything has ever been produced. kNeverDeformed is distinct from
            // revision 0, which is a legitimate value a freshly spawned
            // skeleton holds — treating them alike leaves a brand-new surface
            // un-deformed for exactly the frame its rest pose is most visible.
            // The palette this surface was last deformed WITH, hashed, and the
            // count of times it has actually been rewritten. Hashing 24-100
            // mat4s costs nothing next to skinning the vertices they drive, and
            // it is exact: identical palettes and an identical vertex count
            // mean identical output.
            u64 PaletteHash = 0;
            u64 MorphStateHash = 0;
            u32 ContentRevision = 0;
            bool EverDeformed = false;
        };

        struct QueuedDispatch
        {
            // The surface this dispatch belongs to. Carried so that a queue
            // which is DROPPED rather than recorded can put its entries back:
            // Acquire has already marked them deformed-at-this-pose, and a
            // surface left in that state is never re-queued, so its structure
            // would be built over vertices nothing ever wrote.
            DeformedSurfaceKey Key{};
            u64 RestAddress = 0;
            u64 InfluenceAddress = 0;
            u64 OutputAddress = 0;
            u32 PaletteOffsetBytes = 0;
            u32 BoneCount = 0;
            u32 VertexCount = 0;
        };

        void ReleaseEntry(Entry& entry);
        // Undo the pose bookkeeping Acquire committed for every queued surface,
        // so the next frame offers them again.
        void RollbackQueuedDispatches();
        bool EnsurePaletteBuffer(u64 requiredBytes);
        bool EnsureShader();

        bool m_Enabled = false;
        u64 m_FrameNumber = 0;

        std::unordered_map<DeformedSurfaceKey, Entry, DeformedSurfaceKeyHash> m_Surfaces;
        TArray<QueuedDispatch> m_Queue;
        TArray<DeformedSurfaceKey> m_PendingRetires;
        // Surfaces already counted in SurfacesRequested this frame. Acquire is
        // called once per SUBMESH and a character is many submeshes sharing one
        // deformed stream, so without this the census reports one idle fox as
        // several idle surfaces.
        std::unordered_set<DeformedSurfaceKey, DeformedSurfaceKeyHash> m_CountedThisFrame;

        // The frame's palettes, packed back to back and uploaded once. One
        // buffer for every surface rather than one per surface: a palette is
        // 100 mat4s at most and a per-surface allocation would be the dominant
        // cost of a crowd.
        TArray<glm::mat4> m_PaletteStaging;
        Ref<StorageBuffer> m_PaletteBuffer;
        u64 m_PaletteBufferBytes = 0;
        u64 m_PaletteAddress = 0;

        Ref<ComputeShader> m_Shader;
        Ref<UniformBuffer> m_Params;
        bool m_ShaderUnavailable = false;

        DeformedSurfaceStats m_Stats{};
    };
} // namespace OloEngine::RayTracing

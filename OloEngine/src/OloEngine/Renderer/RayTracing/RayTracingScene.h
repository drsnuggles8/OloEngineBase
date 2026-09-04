#pragma once

// =============================================================================
// RayTracingScene.h — the acceleration-structure scene manager. Issue #978.
//
// WHAT THIS OWNS. One BLAS per GPU Scene GEOMETRY record, one TLAS over the
// live GPU Scene INSTANCE records, the policy that decides when either is
// rebuilt, and the telemetry. It consumes the canonical GPU Scene (#977) and
// exposes a stable query contract — a TLAS device address — to passes.
//
// WHY THE SPLIT. Everything above the IRayTracingBackend line is
// backend-neutral policy: classification, the refit-vs-rebuild heuristic, the
// stale-record guard, instance packing, the counters. It compiles and is
// tested on a machine with no GPU, which is every CI runner this project has.
// Everything below the line is device work, implemented once in
// Platform/Vulkan/VulkanRayTracingBackend.cpp. The seam is the same shape
// RHI::DescriptorHeap uses for IDescriptorHeapBackend.
//
// WHAT THE SPLIT DELIBERATELY DOES NOT COVER, stated here because a
// substitution is a decision about what you stop testing
// (substituted-seams-compound.md): the neutral half cannot see a wrong
// VkAccelerationStructureInstanceKHR layout, a wrong scratch alignment, a
// compaction that retires live storage, or a missing AS-build -> AS-read
// barrier. Those are pinned ONLY by the device-backed tenants in
// tests/Rendering/RayTracing/, which SKIP where there is no RT device.
//
// THREADING. Render thread only, like every other GPU resource manager here.
// It runs inside Renderer3D::EndScene, after GPUScene::Upload and before the
// render graph is compiled.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RayTracing/RayTracingStats.h"
#include "OloEngine/Renderer/RayTracing/RayTracingTypes.h"

#include <glm/glm.hpp>

#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class GPUScene;
    struct GPUSceneGeometry;
    struct GPUSceneInstance;
    struct GPUSceneMaterial;
} // namespace OloEngine

namespace OloEngine::RayTracing
{
    // -------------------------------------------------------------------------
    // Identity
    // -------------------------------------------------------------------------

    // A BLAS is keyed by the GPU Scene GEOMETRY slot, not the instance slot —
    // many instances share one geometry, which is the entire point of a
    // two-level acceleration structure.
    //
    // The generation is carried because a GPU Scene slot is recycled two
    // frames after its record dies. Slot alone would let a rebuilt mesh
    // inherit the dead mesh's BLAS.
    struct GeometryKey
    {
        u32 Slot = std::numeric_limits<u32>::max();
        u32 Generation = 0;

        [[nodiscard]] bool IsValid() const
        {
            return Slot != std::numeric_limits<u32>::max() && Generation != 0;
        }
        [[nodiscard]] auto operator==(const GeometryKey&) const -> bool = default;
    };

    struct GeometryKeyHash
    {
        [[nodiscard]] sizet operator()(const GeometryKey& key) const noexcept
        {
            return (static_cast<sizet>(key.Slot) << 32) ^ static_cast<sizet>(key.Generation);
        }
    };

    // -------------------------------------------------------------------------
    // What the backend is asked to do
    // -------------------------------------------------------------------------

    // One BLAS build. Addresses come straight from the GPU Scene geometry
    // record, which on Vulkan already carries real device addresses for the
    // vertex and index streams.
    struct BlasBuildRequest
    {
        GeometryKey Key;
        GeometryClass Class = GeometryClass::Unsupported;
        BuildReason Reason = BuildReason::FirstBuild;

        u64 VertexAddress = 0;
        u64 IndexAddress = 0;
        u32 VertexStride = 0; ///< Bytes between vertex positions (sizeof(Vertex)).
        u32 VertexCount = 0;  ///< maxVertex + 1 for the build.
        u32 FirstIndex = 0;
        u32 IndexCount = 0;
        i32 BaseVertex = 0;

        [[nodiscard]] u32 TriangleCount() const
        {
            return IndexCount / 3u;
        }
    };

    // One TLAS instance. Mirrors what VkAccelerationStructureInstanceKHR
    // MEANS without borrowing its bitfield layout — the packing lives in the
    // Vulkan backend, and the two narrow fields here are already range-checked
    // (PackInstanceMask / FitsInstanceCustomIndex).
    struct InstanceRecord
    {
        // Row-major 3x4, exactly the GPU Scene transform encoding and exactly
        // VkTransformMatrixKHR's. Deliberately not a glm::mat4: converting
        // twice is how a transpose bug gets in.
        std::array<glm::vec4, 3> Transform{ glm::vec4{ 1.0f, 0.0f, 0.0f, 0.0f },
                                            glm::vec4{ 0.0f, 1.0f, 0.0f, 0.0f },
                                            glm::vec4{ 0.0f, 0.0f, 1.0f, 0.0f } };

        u32 CustomIndex = 0; ///< The GPU Scene instance slot. 24 bits at the device.
        u8 Mask = kInstanceMaskAll;
        bool ForceOpaque = true; ///< False for Masked geometry: the ray must stop as a candidate.
        GeometryKey Geometry;    ///< Which BLAS this instance references.

        [[nodiscard]] auto operator==(const InstanceRecord&) const -> bool = default;
    };

    // -------------------------------------------------------------------------
    // The device seam
    // -------------------------------------------------------------------------

    // Implemented once, in Platform/Vulkan/VulkanRayTracingBackend.cpp. Every
    // method may be called only from the render thread, inside a live frame
    // recording, with the exception of GetCapabilities.
    class IRayTracingBackend
    {
      public:
        virtual ~IRayTracingBackend() = default;

        [[nodiscard]] virtual Capabilities GetCapabilities() const = 0;

        // Record every request into the frame's command buffer, batched. The
        // backend owns scratch pooling and the build/compaction pipeline; it
        // must not submit, and must not wait for the device.
        //
        // Returns the number of builds actually recorded — fewer than
        // requested when a size query or an allocation failed, which the
        // caller reports rather than asserting.
        virtual u32 RecordBlasBuilds(std::span<const BlasBuildRequest> requests) = 0;

        // Retire a BLAS whose geometry record died. Frame-safe: the handle and
        // its backing storage go to deferred reclaim, never an inline destroy.
        virtual void RetireBlas(const GeometryKey& key) = 0;

        // True once this geometry has a BLAS that a TLAS build may reference.
        // A build recorded this frame counts — the TLAS build is ordered after
        // it in the same command buffer.
        [[nodiscard]] virtual bool IsBlasResident(const GeometryKey& key) const = 0;

        // Record the TLAS build/update. `rebuild` false requests an in-place
        // refit, which the backend may still promote to a rebuild (it says so
        // by returning the reason it used).
        virtual TlasBuildReason RecordTlasBuild(std::span<const InstanceRecord> instances, TlasBuildReason requested) = 0;

        // The stable query contract handed to passes and to the diagnostic:
        // the TLAS's device address, which a ray-query shader converts with
        // accelerationStructureEXT(uvec2). Zero when no TLAS has been built.
        [[nodiscard]] virtual u64 GetTlasDeviceAddress() const = 0;

        // Emit the AS-build -> AS-read barrier into the frame command buffer.
        // Separate from RecordTlasBuild so a caller that builds nothing this
        // frame still cannot read without one.
        virtual void RecordBuildToReadBarrier() = 0;

        // Fold this frame's device-side numbers (AS bytes, scratch bytes,
        // compaction savings, GPU times) into the stats block the scene owns.
        virtual void PublishStats(SceneStats& stats) const = 0;

        // Drop every device object. Called at renderer shutdown and on a
        // backend restart.
        virtual void Shutdown() = 0;
    };

    // Installed at renderer init by the neutral factory in RayTracingScene.cpp,
    // which branches on RendererAPI::GetAPI(). Returns nullptr on any backend
    // without hardware ray tracing — which is not an error, it is the
    // unsupported path.
    [[nodiscard]] std::unique_ptr<IRayTracingBackend> CreateRayTracingBackend();

    // -------------------------------------------------------------------------
    // The scene manager
    // -------------------------------------------------------------------------

    class RayTracingScene
    {
      public:
        RayTracingScene() = default;
        ~RayTracingScene() = default;

        RayTracingScene(const RayTracingScene&) = delete;
        RayTracingScene& operator=(const RayTracingScene&) = delete;

        // Install the device backend (or leave the scene in its unsupported
        // state when there is none). Idempotent.
        void Init();
        void Shutdown();

        // Test seam: install a backend directly. Not used by production code.
        void SetBackendForTesting(std::unique_ptr<IRayTracingBackend> backend);

        [[nodiscard]] Capabilities GetCapabilities() const
        {
            return m_Capabilities;
        }
        [[nodiscard]] bool IsAvailable() const
        {
            return m_Capabilities.Supported && m_Backend != nullptr;
        }

        // The whole per-frame entry point. Walks the live GPU Scene records,
        // classifies each, decides what must be built, records the builds and
        // the TLAS, and updates the counters.
        //
        // The camera-relative render origin is read from GPU Scene rather
        // than passed in: it is the origin GPU Scene ENCODED this frame's
        // transforms against, so taking it from anywhere else is how the two
        // drift and a rebase goes unnoticed. A change to it moves every
        // transform at once and forces a full TLAS rebuild.
        //
        // Safe and cheap to call when RT is unavailable: it resets the frame
        // counters and returns.
        void Update(const GPUScene& scene);

        // Emit the AS-build -> AS-read barrier. Called by the render graph
        // pass that owns the hazard, not by Update, so the barrier sits where
        // the graph says it does.
        void RecordBuildToReadBarrier();

        [[nodiscard]] u64 GetTlasDeviceAddress() const;

        [[nodiscard]] const SceneStats& GetStats() const
        {
            return m_Stats;
        }

        // --- Policy, exposed because it is the testable half -------------

        // Classify one GPU Scene record pair. `material` may be null (an
        // instance whose material slot is dead), which classifies as
        // Unsupported rather than guessing an alpha mode.
        [[nodiscard]] static GeometryClass Classify(const GPUSceneInstance& instance, const GPUSceneGeometry* geometry,
                                                    const GPUSceneMaterial* material);

        // Decide what a resident BLAS needs this frame given its class and
        // what changed. Returns nullopt when nothing must be recorded.
        //
        // `consecutiveRefits` is the run length of refits since the last full
        // rebuild — the documented deformed-geometry heuristic: refit while
        // the run is under kMaxConsecutiveRefits, then rebuild, because a
        // refitted BLAS's tree topology is the ORIGINAL geometry's and its
        // quality decays as the vertices drift from it.
        static constexpr u32 kMaxConsecutiveRefits = 8;

        [[nodiscard]] static std::optional<BuildReason> DecideBuild(GeometryClass previousClass,
                                                                    GeometryClass currentClass, bool geometryChanged,
                                                                    bool hasBlas, u32 consecutiveRefits);

        // Decide whether the TLAS can refit or must rebuild.
        [[nodiscard]] static TlasBuildReason DecideTlasBuild(u32 previousInstanceCount, u32 currentInstanceCount,
                                                             bool topologyChanged, bool renderOriginRebased,
                                                             bool everBuilt);

      private:
        // What we know about one resident BLAS between frames.
        struct BlasState
        {
            GeometryClass Class = GeometryClass::Unsupported;
            u32 ConsecutiveRefits = 0;
            u64 GeometryFingerprint = 0; ///< Bytes of the GPU Scene geometry record, hashed.
            u64 LastSeenFrame = 0;
        };

        std::unique_ptr<IRayTracingBackend> m_Backend;
        Capabilities m_Capabilities{};

        std::unordered_map<GeometryKey, BlasState, GeometryKeyHash> m_Blas;
        std::vector<InstanceRecord> m_Instances;
        std::vector<BlasBuildRequest> m_PendingBuilds;
        std::vector<GeometryKey> m_PendingRetires;

        SceneStats m_Stats{};
        u64 m_FrameNumber = 0;
        u32 m_PreviousInstanceCount = 0;
        bool m_EverBuiltTlas = false;
        bool m_HasRenderOrigin = false;
        glm::vec3 m_RenderOrigin{ 0.0f };
    };
} // namespace OloEngine::RayTracing

#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMesh.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshGpuData.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    class MeshSource;
    class StorageBuffer;

    // Debug / test routing control for the compute software rasterizer.
    enum class VirtualSwRasterMode : u8
    {
        Auto = 0,          // route small-coverage clusters to the SW rasterizer
        ForceSoftware = 1, // route every (near-plane-safe) cluster through the SW rasterizer
        Disabled = 2       // hardware MDI only
    };

    // Debug visualization written into the "VirtualGeometryDebug" capture target
    // by both raster paths (issue #629). Off = the main pass does no debug work.
    enum class VirtualDebugMode : u8
    {
        Off = 0,
        ClusterId = 1, // per-pixel cluster index hashed to a distinct colour
        Lod = 2,       // per-pixel cluster DAG level as a colour ramp
        Overdraw = 3   // per-pixel cluster fragment count as a heat ramp
    };

    // Streaming residency statistics (page pools, issue #629 slice 5).
    struct VirtualResidencyStats
    {
        u32 TotalPages = 0;
        u32 ResidentPages = 0;
        u32 PinnedPages = 0;
        u32 BudgetSlots = 0;
        u64 PageUploads = 0;
        u64 PageEvictions = 0;
    };

    // Aggregate per-frame cluster-cull statistics, summed over every instance's
    // VirtualDrawArgs (issue #629). Read back from the GPU args buffer on demand
    // for the editor stats overlay / MCP inspection (a small blocking readback,
    // so only fetched when the overlay is open).
    struct VirtualCullStats
    {
        u32 InstanceCount = 0;
        u32 TestedClusters = 0;     // clusters the cull dispatched a thread for
        u32 CutSelected = 0;        // passed the view-dependent DAG-cut rule
        u32 HardwareDraws = 0;      // survivors routed to the hardware MDI path
        u32 SoftwareRasterized = 0; // survivors routed to the compute SW rasterizer
        [[nodiscard]] u32 DrawnClusters() const
        {
            return HardwareDraws + SoftwareRasterized;
        }
    };

    // One virtual-mesh draw request for the current frame (produced by the Scene
    // VirtualMeshComponent loop via Renderer3D::SubmitVirtualMesh).
    struct VirtualMeshSubmission
    {
        AssetHandle Mesh = 0;
        glm::mat4 Transform{ 1.0f }; // absolute world transform (origin shift applied at upload)
        glm::mat4 PrevTransform{ 1.0f };
        i32 EntityID = -1;
        f32 ErrorThresholdPixels = 1.0f;
        // One FrameDataBufferManager material slot PER PART (a multi-submesh mesh is one DAG
        // per submesh, each drawn as its own instance with its own material). Parallel to the
        // registry's part list for this mesh, so index i is the material for part i.
        std::vector<u32> MaterialDataIndices;
        // Parallel to MaterialDataIndices: 1 when that part's material is alpha-masked/blended,
        // which forces its clusters onto the hardware raster path (the compute rasterizer
        // cannot run a cutout test — see VirtualInstanceGpuRecord::kFlagAlphaMasked).
        std::vector<u8> PartAlphaMasked;
        // Parallel to MaterialDataIndices: 1 when that part's material is MaterialFlag::TwoSided,
        // so the hardware draw must not backface-cull it (the classic path does this in
        // Renderer3DDrawHelpers::BuildRenderState). Sponza's foliage is two-sided single-quad
        // geometry — culling it drops half of every leaf.
        std::vector<u8> PartTwoSided;
        bool CastShadows = true;
    };

    // @brief GPU residence for every registered virtual mesh (issue #629).
    //
    // Owns the pooled cluster/group/vertex/index SSBOs shared by all virtual
    // meshes, plus the per-frame instance/command/args/visible buffers the cull
    // compute writes and the hardware MDI path consumes. Slice 2 keeps every
    // registered mesh fully resident; the streaming slice replaces the all-
    // resident pools with budgeted page pools.
    //
    // Threading: registration and GL calls must happen on the render thread with
    // a live GL context (same contract as GPUFluidSolver). CPU-only paths
    // (Register* building/packing) are context-free until PrepareFrame uploads.
    class VirtualMeshRegistry
    {
      public:
        // One PART of a registered mesh: the DAG for a single submesh. A single-submesh mesh
        // has exactly one. Parts of the same mesh are CONTIGUOUS in m_Entries (see MeshParts),
        // which is what lets the pool packing below stay completely part-agnostic — it just
        // walks m_Entries and never needs to know which mesh an entry came from.
        struct MeshEntry
        {
            VirtualMeshGpuData Packed; // mesh-local records (bases applied at pool upload)
            u32 ClusterBase = 0;
            u32 GroupBase = 0;
            u32 VertexBase = 0;
            u32 IndexBase = 0;
            u32 LevelCount = 0;
            u32 SourceTriangleCount = 0;
            u32 SubmeshIndex = 0; // submesh this part was built from
            bool Valid = false;   // false = build failed (unsupported source); submissions are skipped
        };

        // The contiguous run of MeshEntry parts belonging to one mesh asset.
        struct MeshParts
        {
            u32 FirstEntry = 0;
            u32 Count = 0;
            bool Valid = false; // at least one part built
        };

        // CPU-side instance record kept alongside the GPU upload so the draw
        // loop can bind per-instance material state and issue the MDI call.
        struct FrameInstance
        {
            VirtualInstanceGpuRecord Gpu;
            u32 MaterialDataIndex = 0;
            bool CastShadows = true;
            bool TwoSided = false; // material is TwoSided — the hardware draw must not backface-cull
        };

        static VirtualMeshRegistry& Get();

        // Builds the cluster LOD DAG for the mesh source and packs it for GPU
        // use. Safe to call repeatedly; returns whether the mesh is usable.
        bool RegisterMeshSource(AssetHandle handle, const MeshSource& source);
        [[nodiscard]] bool IsRegistered(AssetHandle handle) const;

        // The parts (one per submesh) registered for this mesh. Count == 0 when the mesh is
        // unknown or nothing built.
        [[nodiscard]] MeshParts FindParts(AssetHandle handle) const;
        [[nodiscard]] const MeshEntry& GetEntry(u32 entryIndex) const
        {
            return m_Entries[entryIndex];
        }

        // Frame lifecycle -----------------------------------------------------
        void BeginFrame();
        void Submit(const VirtualMeshSubmission& submission);
        [[nodiscard]] const std::vector<VirtualMeshSubmission>& GetSubmissions() const
        {
            return m_Submissions;
        }

        // Uploads dirty pools and stages this frame's instance records; returns
        // false when there is nothing to draw. Idempotent within a frame (the
        // shadow pass runs before the main virtual-geometry pass and both call
        // it; the first caller does the work). Requires a live GL context.
        bool PrepareFrame(const glm::vec3& renderOrigin);

        // Sizes/clears the software-raster visibility buffer for this frame's
        // main viewport (called by VirtualGeometryPass only — shadow cascades
        // never software-rasterize).
        void EnsureVisbuffer(u32 viewportWidth, u32 viewportHeight);

        // Software-raster routing control (consulted by VirtualGeometryPass;
        // ForceSoftware/Disabled power the SW-vs-HW parity test).
        void SetSwRasterMode(VirtualSwRasterMode mode)
        {
            m_SwRasterMode = mode;
        }
        [[nodiscard]] VirtualSwRasterMode GetSwRasterMode() const
        {
            return m_SwRasterMode;
        }
        [[nodiscard]] f32 GetSwRasterThresholdPixels() const
        {
            return m_SwRasterThresholdPixels;
        }
        // Auto-mode routing threshold: a cluster whose projected screen radius is
        // below this many pixels goes to the compute software rasterizer. Exposed
        // so the HW/SW split can be swept live over MCP (issue #607) instead of
        // only through the two extreme modes. Clamped to a sane range — 0 would
        // silently disable SW raster in Auto and a huge value would force it.
        void SetSwRasterThresholdPixels(f32 thresholdPixels)
        {
            if (!std::isfinite(thresholdPixels))
                return;
            m_SwRasterThresholdPixels = std::clamp(thresholdPixels, 0.0f, 4096.0f);
        }

        // Debug/test override: force the portable two-pass 2x32 software-raster
        // visibility path even on a driver that supports the single-pass 64-bit
        // atomic path (issue #629). Lets the SW-vs-HW parity test exercise BOTH
        // rasterizers on int64-capable hardware. No effect on drivers without
        // 64-bit atomics (portable is the only path there).
        void SetForcePortableSwRaster(bool force)
        {
            m_ForcePortableSwRaster = force;
        }
        [[nodiscard]] bool GetForcePortableSwRaster() const
        {
            return m_ForcePortableSwRaster;
        }

        // Debug visualization (issue #629). When the mode is not Off, both raster
        // paths write per-pixel cluster/LOD/overdraw data into the debug targets,
        // which VirtualGeometryPass imports into the render graph as
        // "VirtualGeometryDebug" (colour) for olo_render_capture_target / MCP
        // inspection. No cost when Off.
        void SetDebugMode(VirtualDebugMode mode)
        {
            m_DebugMode = mode;
        }
        [[nodiscard]] VirtualDebugMode GetDebugMode() const
        {
            return m_DebugMode;
        }

        // Sizes/clears the RGBA8 colour + R32UI count debug targets to the given
        // viewport (called by VirtualGeometryPass only when a debug mode is on).
        void EnsureDebugTargets(u32 viewportWidth, u32 viewportHeight);
        [[nodiscard]] u32 GetDebugColorTextureID() const
        {
            return m_DebugColorTexID;
        }
        [[nodiscard]] u32 GetDebugCountTextureID() const
        {
            return m_DebugCountTexID;
        }
        [[nodiscard]] u32 GetDebugWidth() const
        {
            return m_DebugWidth;
        }
        [[nodiscard]] u32 GetDebugHeight() const
        {
            return m_DebugHeight;
        }

        // Streaming residency (slice 5). Pages (one per group) live in
        // budgeted slot arenas; the cull consults per-group resident bits and
        // requests missing pages, which this processes with LRU eviction.
        // budgetSlots == 0 means "fit everything" (eager residency — the
        // default, no pop-in). Changing the budget rebuilds the pools.
        void SetPageBudgetSlots(u32 budgetSlots);
        [[nodiscard]] const VirtualResidencyStats& GetResidencyStats() const
        {
            return m_ResidencyStats;
        }

        // Blocking GPU readback of this frame's aggregate cull statistics from
        // the args buffer. Requires a live GL context; returns zeroes when there
        // is nothing to draw. For the editor stats overlay / MCP inspection.
        [[nodiscard]] VirtualCullStats ReadFrameCullStats() const;
        [[nodiscard]] const Ref<StorageBuffer>& GetGroupStatesBuffer() const
        {
            return m_GroupStatesBuffer;
        }

        // Reads back last frame's request/touch bits, uploads requested pages
        // through the persistent-mapped ring (LRU-evicting under budget
        // pressure), and republishes the resident bits. Call once per frame
        // after PrepareFrame, before the cull dispatches.
        void ProcessResidency();

        [[nodiscard]] const std::vector<FrameInstance>& GetFrameInstances() const
        {
            return m_FrameInstances;
        }
        [[nodiscard]] u32 GetTotalFrameClusterCount() const
        {
            return m_TotalFrameClusterCount;
        }

        // GL object accessors for the cull + draw passes (valid after PrepareFrame)
        [[nodiscard]] const Ref<StorageBuffer>& GetClusterBuffer() const
        {
            return m_ClusterBuffer;
        }
        [[nodiscard]] const Ref<StorageBuffer>& GetGroupBuffer() const
        {
            return m_GroupBuffer;
        }
        [[nodiscard]] const Ref<StorageBuffer>& GetVertexBuffer() const
        {
            return m_VertexBuffer;
        }
        [[nodiscard]] const Ref<StorageBuffer>& GetInstanceBuffer() const
        {
            return m_InstanceBuffer;
        }
        [[nodiscard]] const Ref<StorageBuffer>& GetCommandBuffer() const
        {
            return m_CommandBuffer;
        }
        [[nodiscard]] const Ref<StorageBuffer>& GetArgsBuffer() const
        {
            return m_ArgsBuffer;
        }
        [[nodiscard]] const Ref<StorageBuffer>& GetVisibleBuffer() const
        {
            return m_VisibleBuffer;
        }
        [[nodiscard]] const Ref<StorageBuffer>& GetSwListBuffer() const
        {
            return m_SwListBuffer;
        }
        [[nodiscard]] const Ref<StorageBuffer>& GetVisbufferBuffer() const
        {
            return m_VisbufferBuffer;
        }
        [[nodiscard]] u32 GetIndexBufferID() const
        {
            return m_IndexBufferID;
        }
        [[nodiscard]] u32 GetVaoID() const
        {
            return m_VaoID;
        }
        [[nodiscard]] u32 GetVisbufferWidth() const
        {
            return m_VisbufferWidth;
        }
        [[nodiscard]] u32 GetVisbufferHeight() const
        {
            return m_VisbufferHeight;
        }

        // Releases every GL object (called from Renderer3D::Shutdown).
        void Shutdown();

      private:
        VirtualMeshRegistry() = default;

        static constexpr u32 kNoSlot = 0xFFFFFFFFu;

        // Runtime state of one streamable page (pooled numbering).
        struct PageRuntime
        {
            VirtualPageInfo Info;   // mesh-local ranges
            u32 MeshEntryIndex = 0; // owning MeshEntry (for CPU payload access)
            u32 PooledGroup = 0;    // group index in the pooled buffers
            u32 PooledFirstCluster = 0;
            u32 SlotIndex = kNoSlot;
            u64 LastUsedFrame = 0;
            bool Pinned = false;
            bool Resident = false;
        };

        void RebuildPools();
        void EnsureFrameBuffers();
        bool LoadPage(u32 pageIndex);
        void EvictPage(u32 pageIndex);
        [[nodiscard]] bool CopyThroughRing(u32 targetBufferID, u64 targetOffset, const void* payload, u64 bytes);

        std::unordered_map<AssetHandle, MeshParts> m_EntryLookup;
        std::vector<MeshEntry> m_Entries; // stable order => deterministic pool layout
        bool m_PoolsDirty = false;

        std::vector<VirtualMeshSubmission> m_Submissions;
        std::vector<FrameInstance> m_FrameInstances;
        u32 m_TotalFrameClusterCount = 0;
        bool m_FramePrepared = false; // PrepareFrame ran this frame
        bool m_FramePreparedResult = false;
        bool m_ResidencyProcessed = false; // ProcessResidency ran this frame

        // Resident metadata pools (rebuilt when meshes register)
        Ref<StorageBuffer> m_ClusterBuffer;     // SSBO_VIRTUAL_CLUSTERS (bases rebased on page load)
        Ref<StorageBuffer> m_GroupBuffer;       // SSBO_VIRTUAL_GROUPS
        Ref<StorageBuffer> m_GroupStatesBuffer; // SSBO_VIRTUAL_GROUP_STATES (bit0 resident / bit1 request / bit2 touch)
        // Budgeted geometry slot arenas (page slots of uniform capacity)
        Ref<StorageBuffer> m_VertexBuffer; // SSBO_VIRTUAL_VERTICES arena
        u32 m_IndexBufferID = 0;           // element-buffer + SSBO_VIRTUAL_INDICES arena
        u32 m_VaoID = 0;                   // element-buffer-only VAO for the MDI path

        // Streaming bookkeeping
        std::vector<PageRuntime> m_Pages;
        std::vector<u32> m_PageOfPooledGroup;                  // pooled group -> page index
        std::vector<VirtualClusterGpuRecord> m_PooledClusters; // CPU mirror, mesh-local bases
        std::vector<u32> m_GroupStatesCpu;
        std::vector<u32> m_FreeSlots;
        u32 m_SlotVertexCapacity = 0;
        u32 m_SlotIndexCapacity = 0;
        u32 m_SlotCount = 0;
        u32 m_BudgetSlotsSetting = 0; // 0 = fit everything (eager)
        u32 m_MaxPageUploadsPerFrame = 64;
        u64 m_FrameCounter = 0;
        VirtualResidencyStats m_ResidencyStats;

        // Persistent-mapped upload ring (CPU staging -> arena copies)
        u32 m_RingBufferID = 0;
        u8* m_RingPtr = nullptr;
        u64 m_RingSize = 0;
        u64 m_RingHead = 0;

        // Per-frame buffers (grow-only)
        Ref<StorageBuffer> m_InstanceBuffer; // SSBO_VIRTUAL_INSTANCES
        Ref<StorageBuffer> m_CommandBuffer;  // SSBO_VIRTUAL_DRAW_COMMANDS (also GL_DRAW_INDIRECT_BUFFER)
        Ref<StorageBuffer> m_ArgsBuffer;     // SSBO_VIRTUAL_DRAW_ARGS (also GL_PARAMETER_BUFFER)
        // GL_DYNAMIC_READ staging copy of m_ArgsBuffer, so the CPU stats readback never
        // touches the video-memory-resident args buffer directly (see ReadFrameCullStats).
        // Mutable because that readback is const — it observes the frame, it doesn't build it.
        mutable u32 m_ArgsReadbackID = 0;
        mutable u32 m_ArgsReadbackBytes = 0;
        Ref<StorageBuffer> m_VisibleBuffer;   // SSBO_VIRTUAL_VISIBLE
        Ref<StorageBuffer> m_SwListBuffer;    // SSBO_VIRTUAL_SW_LIST (16-byte header + records)
        Ref<StorageBuffer> m_VisbufferBuffer; // SSBO_VIRTUAL_VISBUFFER (uvec2 per pixel)
        u32 m_VisbufferWidth = 0;
        u32 m_VisbufferHeight = 0;

        VirtualSwRasterMode m_SwRasterMode = VirtualSwRasterMode::Auto;
        f32 m_SwRasterThresholdPixels = 24.0f; // Auto-mode projected-radius routing threshold
        bool m_ForcePortableSwRaster = false;  // debug/test: force the two-pass 2x32 path

        // Debug visualization targets (raw GL, sized to the viewport). Colour is
        // RGBA8 (imported into the graph + captured); count is R32UI (overdraw
        // accumulation, colorized into the colour target).
        VirtualDebugMode m_DebugMode = VirtualDebugMode::Off;
        u32 m_DebugColorTexID = 0;
        u32 m_DebugCountTexID = 0;
        u32 m_DebugWidth = 0;
        u32 m_DebugHeight = 0;
    };
} // namespace OloEngine

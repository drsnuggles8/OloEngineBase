#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/StorageBuffer.h"

#include <array>
#include <cstddef>
#include <glm/glm.hpp>

namespace OloEngine
{
    class TerrainGPUQuadtree;

    // @brief GPU terrain picking with no synchronous readback (issue #717).
    //
    // Answers "where does this ray hit the terrain?" entirely against the GPU
    // heightmap, and hands the answer back one or two frames later through a
    // fenced ring — never through a `GetData()`, which is the stall the whole
    // class exists to remove.
    //
    // The CPU path it replaces (`EditorLayer::TerrainRaycast`) marches the
    // CPU-side heightmap mirror in 1-unit steps. That mirror is exactly what the
    // GPU-quadtree and GPU-painting work is trying to stop needing, and the
    // march is single-threaded work proportional to the ray's length.
    //
    // Three dispatches, all reusing the LOD descent's machinery:
    //
    //   1. `TerrainRayNodeSelect.comp`, once per tree level — the ray-guided
    //      twin of `TerrainNodeSelect.comp`, reading the SAME min/max height
    //      pyramid. A ray/AABB slab test against inflated bounds replaces the
    //      frustum test, so the descent visits O(2^level) nodes per level
    //      instead of O(4^level) and lands on the finest-level nodes the ray
    //      really passes through.
    //   2. `TerrainPickArgs.comp` (1 thread) swaps the worklist counters and
    //      writes the next `DispatchComputeIndirect` arguments, so the CPU never
    //      learns how many nodes survived a level.
    //   3. `TerrainPickResolve.comp`, one work group per candidate — clips the
    //      ray to the candidate's box, marches at heightmap-texel spacing,
    //      bisects the bracket, and `atomicMin`s the hit's `t`. The IEEE bit
    //      pattern of a non-negative float is monotonic, so that minimum IS the
    //      nearest hit with no sort and no ordering.
    //
    // WHAT MAKES IT NON-STALLING is the fence poll, not the buffer type — the
    // same conclusion `GPUReadbackStats` reached and for the same reason: the
    // RHI's persistent mapping is write-only on GL and unimplemented on Vulkan,
    // so a readback goes through a DeviceToHost buffer and a plain read, issued
    // ONLY for a slot whose `IsFenceSignaled()` already reports complete. There
    // is no `ClientWaitFence` in this class and adding one would put the stall
    // straight back.
    //
    // THE RAY IS KEPT CPU-SIDE PER RING SLOT, and the GPU publishes only `t`.
    // That is what makes the hit position exact rather than nearly exact: the
    // CPU reconstructs `origin + direction * t` from the very ray it submitted,
    // so no position is ever encoded, rounded or round-tripped through the GPU.
    // It is also what makes `RayId` meaningful — a consumer can tell whether the
    // answer it is holding belongs to the ray it last asked about.
    //
    // THREAD SAFETY: render-thread only, like the quadtree it rides on.
    class TerrainGPUPicker : public RefCounted
    {
      public:
        // Staging slots. THREE, for the reason GPUReadbackStats uses three: two
        // avoids a stall only if the GPU is never more than one frame behind,
        // and the point of a poll is that we do not get to assume that.
        static constexpr u32 kRingSlots = 3;

        // Per-level worklist capacity. A ray touches at most ~4*2^level nodes at
        // level L (the 2D DDA bound plus the inflation's slack), so 8192 covers
        // the deepest tree the pyramid allows with room to spare — two orders of
        // magnitude below the LOD descent's own cap, because a line through the
        // tree is not a sweep of it.
        static constexpr u32 kMaxNodeListEntries = 8192;
        // Finest-level nodes the ray may pass through in one query.
        static constexpr u32 kMaxCandidates = 4096;

        // The HitTBits reset value. Above every finite positive float's bit
        // pattern, so any real hit wins the atomicMin. GLSL twin:
        // OLO_TERRAIN_PICK_NO_HIT in include/TerrainPickCommon.glsl.
        static constexpr u32 kNoHitBits = 0xFFFFFFFFu;

        static constexpr u32 kOverflowNodes = 1u;
        static constexpr u32 kOverflowCandidates = 2u;
        // The resolve kernel could not step the marched segment at heightmap-
        // texel spacing within its per-lane sample budget, so the answer may be
        // coarser than the "within a texel" this feature is measured against —
        // or a thin crossing may have been stepped over entirely. Reported
        // rather than left silent, because otherwise it is indistinguishable
        // from the ray genuinely missing.
        static constexpr u32 kOverflowMarch = 4u;

        // A pick query. Terrain-LOCAL, matching the node bounds and the cull
        // inputs — see MakeTerrainLocalCullInputs in Scene.cpp.
        struct RayRequest
        {
            glm::vec3 OriginLocal{ 0.0f };
            glm::vec3 DirectionLocal{ 0.0f, -1.0f, 0.0f }; // must be normalized
            f32 MaxDistance = 2000.0f;
            // Echoed back with the answer. The caller owns the numbering; 0 is
            // as valid as any other value.
            u32 RayId = 0;
        };

        // Everything the terrain contributes to a query. Separate from the ray
        // because it changes on a sculpt, not on a mouse move.
        struct TerrainInputs
        {
            RHI::ResourceHandle Heightmap{};
            u32 HeightmapResolution = 0;
            f32 WorldSizeX = 0.0f;
            f32 WorldSizeZ = 0.0f;
            f32 HeightScale = 1.0f;
        };

        struct Result
        {
            // False until the first capture has retired — roughly frame 3 after
            // the first submitted ray. Distinguishes "not back yet" from "came
            // back a miss", which are otherwise the same bytes.
            bool Valid = false;
            bool Hit = false;
            f32 Distance = 0.0f;             // t along the submitted ray
            glm::vec3 PositionLocal{ 0.0f }; // OriginLocal + DirectionLocal * Distance
            u32 RayId = 0;
            u32 OverflowFlags = 0;
            u64 FrameIndex = 0; // the frame the query was DISPATCHED on
            u32 Latency = 0;    // frames between that dispatch and this readback
        };

        TerrainGPUPicker();
        ~TerrainGPUPicker();

        // Non-copyable, non-movable. m_Ring holds RAW GPU handles (a buffer and
        // a fence per slot) that the destructor releases, so a copy would hand
        // two objects the same handles and both would DestroyFence/DeleteBuffer
        // them. Declaring the destructor already suppresses the implicit MOVE
        // but NOT the implicit COPY, which is exactly the trap: without these
        // the class reads as safe and silently is not.
        TerrainGPUPicker(const TerrainGPUPicker&) = delete;
        TerrainGPUPicker& operator=(const TerrainGPUPicker&) = delete;
        TerrainGPUPicker(TerrainGPUPicker&&) = delete;
        TerrainGPUPicker& operator=(TerrainGPUPicker&&) = delete;

        // Queue a ray for the next Dispatch(). Overwrites any ray queued and not
        // yet dispatched this frame — a picker answers the LATEST question, and
        // a mouse that moved twice before a frame started has only one current
        // position. Refuses a non-finite or zero-length ray (the slab test's
        // reciprocal turns a NaN direction into a box that swallows the tree).
        bool SubmitRay(const RayRequest& request);

        // Advance the frame counter, retire every ring slot whose fence has
        // signalled, and publish the newest. Poll, never wait. Must be called
        // ONCE PER FRAME whether or not a ray was ever submitted — the counter
        // it advances is what `Result::Latency` is measured in.
        void Poll();

        // Run the descent + resolve for the queued ray, if there is one, and
        // capture the result into the ring. Render thread, live context.
        // Returns false when there is nothing to do or the pass is unusable, so
        // a caller can keep the CPU path.
        bool Dispatch(const TerrainGPUQuadtree& tree, const TerrainInputs& terrain);

        // Release the ring's fences and buffers. Must be called while the
        // context is still alive.
        void Shutdown();

        // The newest answer the ring has actually returned.
        [[nodiscard]] const Result& GetLatest() const
        {
            return m_Latest;
        }

        // True while a ray is queued but not yet dispatched.
        [[nodiscard]] bool HasPendingRay() const
        {
            return m_HasPendingRay;
        }

        // Ring slots still executing. A persistently full ring means the CPU is
        // running far ahead of the GPU and the answer on hand is older than
        // `Latency` last reported.
        [[nodiscard]] u32 GetSlotsInFlight() const
        {
            return m_SlotsInFlight;
        }

        // Byte offsets into the state buffer. Not implementation details: the
        // two dispatch offsets are passed straight to DispatchComputeIndirect
        // and the result offset/size bound the ring's CopyBufferSubData. GLSL
        // twin: the TerrainPickState block in include/TerrainPickCommon.glsl.
        static constexpr u32 kDescentDispatchOffset = 16;
        static constexpr u32 kResolveDispatchOffset = 32;
        static constexpr u32 kResultOffset = 48;
        static constexpr u32 kResultBytes = 16;
        static constexpr u32 kHeaderBytes = 128;

        // C++ twin of the TerrainPickState block's header — everything up to the
        // runtime-sized candidate array, which is what one SetData writes and
        // what the static_asserts below pin.
        struct PickStateHeader
        {
            u32 PendingCount = 0;
            u32 NextCount = 0;
            u32 CandidateCount = 0;
            u32 OverflowFlags = 0;
            glm::uvec3 DescentDispatch{ 0u };
            u32 _Pad0 = 0;
            glm::uvec3 ResolveDispatch{ 0u };
            u32 _Pad1 = 0;
            u32 HitTBits = kNoHitBits;
            u32 ResultFlags = 0;
            u32 RayId = 0;
            u32 _Pad2 = 0;
            glm::vec4 RayOriginAndMaxDist{ 0.0f };
            glm::vec4 RayDirAndInflate{ 0.0f };
            glm::vec4 TerrainSizeAndScale{ 0.0f };
            glm::uvec4 PickParams{ 0u };
        };

        // The 16 bytes at kResultOffset, as the ring copies them.
        struct PickResultBlock
        {
            u32 HitTBits = kNoHitBits;
            u32 ResultFlags = 0;
            u32 RayId = 0;
            u32 _Pad = 0;
        };

      private:
        struct RingSlot
        {
            RHI::ResourceHandle Buffer{};
            u64 Fence = 0;
            u64 FrameIndex = 0;
            // The ray this slot's query was dispatched with. Kept so the CPU can
            // turn the GPU's `t` back into a position against the exact ray it
            // asked about, rather than against whatever ray is current when the
            // answer lands.
            RayRequest Ray{};
            bool Pending = false;
        };

        void EnsureShaders();
        bool EnsureBuffers();
        void ReleaseRing();
        // Issue the ring copy + fence for the query just dispatched, keeping
        // `ray` with the slot so a late answer resolves against its own ray.
        void CaptureResult(const RayRequest& ray);

        Ref<ComputeShader> m_DescentShader;
        Ref<ComputeShader> m_ArgsShader;
        Ref<ComputeShader> m_ResolveShader;
        bool m_ShadersLoaded = false;
        bool m_ShaderLoadFailed = false;

        Ref<StorageBuffer> m_StateBuffer; // header + candidate list
        Ref<StorageBuffer> m_NodeListA;
        Ref<StorageBuffer> m_NodeListB;

        std::array<RingSlot, kRingSlots> m_Ring{};
        u32 m_NextSlot = 0;
        u32 m_SlotsInFlight = 0;
        u64 m_FrameIndex = 0;

        RayRequest m_PendingRay{};
        bool m_HasPendingRay = false;
        Result m_Latest{};
        bool m_OverflowWarned = false;
    };

    static_assert(sizeof(TerrainGPUPicker::PickStateHeader) == TerrainGPUPicker::kHeaderBytes,
                  "PickStateHeader must match the std430 TerrainPickState header exactly");
    static_assert(offsetof(TerrainGPUPicker::PickStateHeader, DescentDispatch) == TerrainGPUPicker::kDescentDispatchOffset,
                  "DescentDispatch offset is part of the DispatchComputeIndirect contract");
    static_assert(offsetof(TerrainGPUPicker::PickStateHeader, ResolveDispatch) == TerrainGPUPicker::kResolveDispatchOffset,
                  "ResolveDispatch offset is part of the DispatchComputeIndirect contract");
    static_assert(offsetof(TerrainGPUPicker::PickStateHeader, HitTBits) == TerrainGPUPicker::kResultOffset,
                  "the result block's offset bounds the readback ring's copy");
    static_assert(sizeof(TerrainGPUPicker::PickResultBlock) == TerrainGPUPicker::kResultBytes,
                  "PickResultBlock must be the exact range the ring copies");
} // namespace OloEngine

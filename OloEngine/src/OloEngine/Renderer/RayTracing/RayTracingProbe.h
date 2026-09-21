#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <array>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace OloEngine
{
    class GPUScene;
}

namespace OloEngine::RayTracing
{
    class RayTracingScene;

    // @brief The live-frame half of `olo_rt_trace_ray` (issue #607).
    //
    // Traces a caller-supplied batch of deterministic rays against the SCENE's
    // TLAS and reports, per ray, what it hit. The shader
    // (assets/shaders/compute/RayTracingProbe.comp) and its device tenant
    // already shipped with #978; what did not was any way to run it against the
    // editor's real scene. Without that, a live session can read the RT scene's
    // COUNTERS (`olo_rt_scene_stats`) but cannot ask what a ray actually hits —
    // which is the one question separating "the TLAS was built" from "the TLAS
    // is correct".
    //
    // VULKAN ONLY, for the shader's reason: `GL_EXT_ray_query` has no OpenGL
    // representation. On a GL context this class loads nothing, dispatches
    // nothing, and says why through GetUnavailableReason() — a diagnostic that
    // returns zeros on a backend that cannot answer is worse than one that
    // refuses.
    //
    // WHY A FENCED RING RATHER THAN A SYNCHRONOUS READBACK, and why the tool
    // above it is submit -> settle -> re-poll: the dispatch has to be RECORDED
    // INTO THE FRAME'S COMMAND STREAM, after RayTracingScenePass has built the
    // acceleration structures and emitted its build->read barrier. An MCP
    // handler runs on an HTTP worker and marshals onto the main thread between
    // frames, which is not a point where anything can be recorded — so the
    // request is queued, one later frame dispatches it, and the answer is read
    // back once its fence has signalled. That is the same shape TerrainGPUPicker
    // uses and for the same reason; the ring is deliberately a copy of it rather
    // than a second invention.
    //
    // THE RAYS ARE KEPT CPU-SIDE PER RING SLOT. A late answer then still
    // resolves against the batch it was dispatched with instead of whatever is
    // current when it lands, which is what makes `BatchId` meaningful: a caller
    // can tell whether the answer it holds belongs to the rays it asked about.
    //
    // THREAD SAFETY: render thread only, like the scene it rides on.
    class RayTracingProbe
    {
      public:
        // Ring slots. THREE, matching TerrainGPUPicker: two only avoids a stall
        // if the GPU is never more than one frame behind, and the point of a
        // poll is that we do not get to assume that.
        static constexpr u32 kRingSlots = 3;

        // Rays per batch. ONE WORKGROUP's worth (RayTracingProbe.comp declares
        // local_size_x = 64). A diagnostic answers "what does this ray hit",
        // asked a handful of rays at a time; a caller that wants a picture
        // should render one. The cap is enforced at submit, not truncated —
        // a silently dropped ray would be reported as a miss.
        static constexpr u32 kMaxRays = 64;

        // RayFlags bits. GLSL twins: OLO_RT_FLAG_* in RayTracingProbe.comp.
        static constexpr u32 kFlagCullBackFaces = 1u;
        static constexpr u32 kFlagTerminateOnFirstHit = 2u;

        struct Ray
        {
            glm::vec3 Origin{ 0.0f };
            f32 TMin = 0.0f;
            glm::vec3 Direction{ 0.0f, 0.0f, -1.0f }; // normalized at submit
            f32 TMax = 1000.0f;
        };

        struct Hit
        {
            // NOT `Hit`: a member may not share its class's name (that spelling
            // declares a constructor and does not compile).
            bool IsHit = false;
            f32 Distance = 0.0f;
            glm::vec2 Barycentrics{ 0.0f }; // (b1, b2); b0 = 1 - b1 - b2
            u32 InstanceSlot = 0;           // == instanceCustomIndex == GPU Scene instance slot
            u32 PrimitiveIndex = 0;
            u32 MaterialSlot = 0;
            u32 GeometrySlot = 0;
            glm::vec2 UV{ 0.0f };
            glm::vec3 WorldNormal{ 0.0f };
            f32 WindingSign = 1.0f; // -1 when the instance basis mirrors
            // The hit position, reconstructed CPU-side from the ray THIS batch
            // was dispatched with. Exact for the same reason TerrainGPUPicker's
            // is: no position is ever encoded, rounded or round-tripped through
            // the GPU.
            glm::vec3 Position{ 0.0f };
        };

        struct Batch
        {
            std::vector<Ray> Rays;
            u32 RayFlags = 0;
            u32 InstanceMask = 0xFFu;
            // MUST BE NON-ZERO. 0 is reserved as "no batch" by
            // GetUnavailableBatchId(), so a batch carrying it would own a
            // refusal reason that reads as ownerless — and a reader applying
            // the documented pairing strictly would then downgrade a definitive
            // refusal to "pending". SubmitBatch refuses it rather than letting
            // the default value through silently.
            u32 BatchId = 0;
        };

        struct Result
        {
            // False until a batch has actually come back. Distinguishes "not
            // back yet" from "came back all misses", which are otherwise the
            // same bytes.
            bool Valid = false;
            u32 BatchId = 0;
            std::vector<Ray> Rays; ///< the rays this answer belongs to
            std::vector<Hit> Hits;
            u32 RayFlags = 0;
            u32 InstanceMask = 0xFFu;
            u64 FrameIndex = 0; ///< the frame the batch was DISPATCHED on
            u32 Latency = 0;    ///< frames between that dispatch and this readback
        };

        RayTracingProbe();
        ~RayTracingProbe();

        // Non-copyable, non-movable: the ring holds RAW GPU handles (a buffer
        // and a fence per slot) that the destructor releases, so a copy would
        // hand two objects the same handles and both would free them.
        // Declaring the destructor suppresses the implicit MOVE but not the
        // implicit COPY — which is exactly the trap.
        RayTracingProbe(const RayTracingProbe&) = delete;
        RayTracingProbe& operator=(const RayTracingProbe&) = delete;
        RayTracingProbe(RayTracingProbe&&) = delete;
        RayTracingProbe& operator=(RayTracingProbe&&) = delete;

        // Queue a batch for the next Dispatch(). Overwrites any batch queued and
        // not yet dispatched — the probe answers the LATEST question. Refuses an
        // empty batch, one over kMaxRays, and any ray that is non-finite, has a
        // zero-length direction, or a degenerate [TMin, TMax] interval: a
        // malformed ray is UNDEFINED BEHAVIOUR at rayQueryInitializeEXT, not a
        // miss. (The shader validates too — this is the near end of the same
        // guard, so a rejected ray gets a reason instead of a silent miss.)
        // Returns false and fills `outError` on refusal.
        bool SubmitBatch(const Batch& batch, std::string& outError);

        // Advance the frame counter, retire every ring slot whose fence has
        // signalled, and publish the newest. Poll, never wait. Called ONCE PER
        // FRAME whether or not a batch was submitted — the counter it advances
        // is what Result::Latency is measured in.
        void Poll();

        // Record the trace for the queued batch, if there is one. MUST run
        // after the acceleration structures are built and the build->read
        // barrier is emitted — RayTracingScenePass::Execute is that point.
        // Returns false when there is nothing to do or the probe is unusable;
        // GetUnavailableReason() then says which.
        bool Dispatch(const RayTracingScene& scene, const GPUScene& gpuScene);

        // Release the ring's fences and buffers. Must be called while the
        // context is still alive.
        void Shutdown();

        [[nodiscard]] const Result& GetLatest() const
        {
            return m_Latest;
        }

        [[nodiscard]] bool HasPendingBatch() const
        {
            return m_HasPendingBatch;
        }

        [[nodiscard]] u32 GetPendingBatchId() const
        {
            return m_PendingBatch.BatchId;
        }

        [[nodiscard]] u32 GetSlotsInFlight() const
        {
            return m_SlotsInFlight;
        }

        // Empty while the probe is usable; otherwise why the LAST REFUSED
        // BATCH was refused. Set by Dispatch, so it describes a real attempt
        // rather than a guess made without one.
        //
        // ALWAYS READ IT BESIDE GetUnavailableBatchId(). The probe holds one
        // reason, but batches are submitted by whoever asks — so a reason read
        // without checking whose it is can be reported against someone else's
        // batch id, which is a confidently wrong diagnosis rather than a
        // missing one. The tool above serializes its calls as well; this pairing
        // is what keeps the invariant local to the probe instead of depending on
        // that lock.
        [[nodiscard]] const std::string& GetUnavailableReason() const
        {
            return m_UnavailableReason;
        }

        // The batch GetUnavailableReason() describes. 0 when there is none —
        // which is sound only because SubmitBatch refuses a zero BatchId, so no
        // real batch can ever collide with the sentinel.
        [[nodiscard]] u32 GetUnavailableBatchId() const
        {
            return m_UnavailableBatchId;
        }

        // The probe shader's hit record. MUST match OloRtProbeHit in
        // assets/shaders/compute/RayTracingProbe.comp field for field; the
        // static_assert below is the only thing standing between a layout
        // change there and a readback that decodes garbage and looks fine.
        // Deliberately the same struct RayTracingDeviceTest.cpp declares — if
        // the two ever disagree, one of them is reading the wrong bytes.
        struct GpuHit
        {
            glm::vec4 DistanceAndBarycentrics{ -1.0f, 0.0f, 0.0f, 0.0f };
            glm::uvec4 Ids{ 0u };
            glm::vec4 UVAndPad{ 0.0f };
            glm::vec4 WorldNormalAndWindingSign{ 0.0f };
            glm::vec4 LegacyDirectBasisNormal{ 0.0f };
        };

        // The probe shader's UBO. Mirrors RayTracingProbeParams, std140. uvec2
        // device addresses, for the same reason the GLSL uses them: the
        // contract stays valid without 64-bit integer extensions.
        struct GpuParams
        {
            glm::uvec2 TlasAddress{ 0u };
            glm::uvec2 RayAddress{ 0u };
            glm::uvec2 HitAddress{ 0u };
            glm::uvec2 InstanceTableAddress{ 0u };
            glm::uvec2 GeometryTableAddress{ 0u };
            glm::uvec2 MaterialTableAddress{ 0u };
            u32 RayCount = 0;
            u32 InstanceSlotCount = 0;
            u32 GeometrySlotCount = 0;
            u32 MaterialSlotCount = 0;
            u32 RayFlags = 0;
            u32 InstanceMask = 0xFFu;
            u32 Pad0 = 0;
            u32 Pad1 = 0;
        };

      private:
        struct RingSlot
        {
            RHI::ResourceHandle Buffer{};
            u64 Fence = 0;
            u64 FrameIndex = 0;
            // The batch this slot was dispatched with, kept so a late answer
            // resolves against its own rays rather than the current ones.
            Batch Dispatched;
            bool Pending = false;
        };

        bool EnsureShader();
        bool EnsureBuffers();
        void ReleaseRing();
        // False when the capture could not be staged (a full ring, or a fence
        // the driver refused). The caller must turn that into a REASON: the
        // batch is gone either way, so a silent false reads to the tool above
        // as 'still pending' forever.
        [[nodiscard]] bool CaptureResult(const Batch& batch);

        Ref<ComputeShader> m_Shader;
        bool m_ShaderLoaded = false;
        bool m_ShaderLoadFailed = false;

        Ref<StorageBuffer> m_RayBuffer;
        Ref<StorageBuffer> m_HitBuffer;
        Ref<UniformBuffer> m_ParamsUBO;

        std::array<RingSlot, kRingSlots> m_Ring{};
        u32 m_NextSlot = 0;
        u32 m_SlotsInFlight = 0;
        u64 m_FrameIndex = 0;

        Batch m_PendingBatch{};
        bool m_HasPendingBatch = false;
        Result m_Latest{};
        std::string m_UnavailableReason;
        u32 m_UnavailableBatchId = 0;
    };

    static_assert(sizeof(RayTracingProbe::GpuHit) == 80,
                  "GpuHit must match OloRtProbeHit's std430 layout (5 x 16 bytes)");
    static_assert(sizeof(RayTracingProbe::GpuParams) % 16 == 0, "std140 uniform blocks are 16-byte aligned");
} // namespace OloEngine::RayTracing

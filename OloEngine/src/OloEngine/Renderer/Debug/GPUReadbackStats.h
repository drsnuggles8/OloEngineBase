#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Debug/GPUReadbackStatsRegistry.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
// COMPLETE type, not a forward declaration: `Data` below holds a
// Ref<StorageBuffer> by value, so instantiating it instantiates
// ~Ref<StorageBuffer>, which needs the derived-to-RefCounted conversion. Clang
// rejects the header outright with only a forward declaration while MSVC
// accepts it -- the exact trap ShaderDebugDraw.h documents, and it surfaced
// there on the Linux sanitizer build rather than locally.
#include "OloEngine/Renderer/StorageBuffer.h"

#include <array>
#include <atomic>

namespace OloEngine
{
    // @brief The structured GPU readback-stats channel (issue #721).
    //
    // ONE stats SSBO any GPU-driven pass can `atomicAdd` into, copied each frame
    // into a ring of device-to-host staging buffers and read back N frames later
    // behind a fence -- never synchronously, and never with a `ClientWaitFence`.
    //
    //   Init()        allocate + bind the 144-byte stats SSBO and the ring.
    //                 Called once from Renderer3D::Init.
    //   BeginFrame()  poll the ring oldest-first, retire every slot whose fence
    //                 has signalled, publish the NEWEST retired one as
    //                 GetLatest(), then zero the live buffer for this frame.
    //                 Called from RenderPipeline::PrepareFrame, i.e. BeginScene.
    //   EndFrame()    barrier, copy the live buffer into the next free ring slot,
    //                 fence it. Called from Renderer3D::EndScene after the graph
    //                 has executed, so it sees every pass's contribution.
    //
    // THE BRACKET IS BeginScene..EndScene, NOT the render graph. That is wider
    // than it first looks necessary and the width is the point: the GPU instance
    // cull dispatches at SUBMISSION time, between the two, not inside the graph.
    // Clearing in UploadExecutionState (where the sibling ShaderDebugDraw does
    // its per-frame reset, and where this used to live) wipes the cull's counters
    // after it has already run — the channel then reports a permanent zero for
    // its first adopter, while any test that drives BeginFrame/EndFrame by hand
    // around a cull still passes.
    //
    // WHAT MAKES IT NON-STALLING, precisely. Not the buffer type -- the RHI's
    // persistent mapping is WRITE-only on GL and unimplemented on Vulkan (see
    // VulkanRendererAPI::AllocatePersistentUploadStorage), so a readback has to
    // go through a DeviceToHost buffer and a plain read. What removes the stall
    // is that the read is ONLY issued for a slot whose fence
    // `IsFenceSignaled()` already reports complete -- a poll, never a wait. A
    // full ring means every slot is still executing, and the correct response is
    // to skip the capture and keep the newest frame, not to block on the oldest.
    // This is the same shape as TerrainVirtualTexture's feedback ring; that
    // class's comments are the longer version of this paragraph.
    //
    // WHY NOT READ THE LIVE BUFFER DIRECTLY. The live buffer is GPU-written
    // every frame and must stay in video memory. A CPU read straight off it
    // makes NVIDIA log 131188 ("CPU is consuming buffer object data ...
    // inconsistent with this usage pattern") and then migrate the buffer
    // VIDEO -> HOST (131186), permanently slowing every atomic that touches it.
    // Same trap, same fix, as ShaderDebugDraw::StageStatsForReadback and
    // VirtualMeshRegistry::ReadFrameCullStats.
    //
    // THREAD SAFETY: render-thread only, with one exception -- `IsEnabled()` and
    // `GetLatest()` are readable from the UI/MCP thread, so the enable flag is
    // atomic and the published frame is a plain value swapped under the render
    // thread's own ordering (a torn read of a `u32[12]` POD is the one risk, and
    // it is accepted: the consumer is a diagnostic overlay, and the alternative
    // is a lock on the render thread's per-frame path).
    class GPUReadbackStats
    {
      public:
        // Staging slots. THREE, not two: two is enough to avoid a stall only if
        // the GPU is never more than one frame behind, and the whole point of a
        // fence poll is that we do not get to assume that. A third slot costs
        // 144 bytes and removes the "ring full, capture skipped" case from every
        // realistic frame pacing.
        static constexpr u32 kRingSlots = 3;

        static void Init();
        static void Shutdown();
        [[nodiscard]] static bool IsInitialised();

        // Master switch. Off: the GLSL helpers early-out on one scalar load, the
        // live buffer is not cleared, no copy is issued and no fence is created.
        // Takes effect at the next BeginFrame().
        static void SetEnabled(bool enabled);
        [[nodiscard]] static bool IsEnabled();

        // ---- Frame lifecycle ------------------------------------------------
        static void BeginFrame();
        static void EndFrame();

        // ---- Consumers -------------------------------------------------------
        // The newest frame that has actually come back. `Valid` is false until
        // the first slot retires (frame ~3 after enabling).
        [[nodiscard]] static const GPUReadbackStatsFrame& GetLatest();

        // The frame the LIVE block is accumulating into — i.e. the one EndFrame()
        // is about to capture. Always ahead of `GetLatest().FrameIndex` by the
        // ring latency. Exposed so a consumer can tell "the channel has not
        // caught up yet" from "the channel has stopped", which are the same
        // bytes if you only ever see the retired frame.
        [[nodiscard]] static u64 GetFrameIndex();

        // Slots still executing on the GPU right now. Diagnostic for the overlay:
        // a persistently full ring means the CPU is running far ahead and the
        // numbers on screen are older than `Latency` last reported.
        [[nodiscard]] static u32 GetSlotsInFlight();

        // ---- Producers -------------------------------------------------------
        // Re-bind the stats SSBO at ShaderBindingLayout::SSBO_GPU_STATS. Call
        // immediately before a dispatch that publishes counters.
        //
        // BeginFrame() already binds it for the frame, so this is belt-and-braces
        // -- but a pass that binds ten of its own buffers in one BindWorkingSet()
        // (VSM does exactly that) can leave the slot pointing somewhere else, and
        // the failure mode of a wrong bind here is counters landing in an
        // unrelated buffer, which reads as "the channel reports zero" with no
        // other symptom. Cheap enough to be unconditional.
        static void BindForDispatch();

      private:
        // The live SSBO's layout. GLSL twin: the OloGpuReadbackStats block in
        // include/GPUReadbackStats.glsl. std430 on a block of nothing but `uint`
        // is a plain packed array, so this is a memcpy-compatible mirror.
        struct GPUStatsBlock
        {
            u32 Flags = 0;
            u32 Enabled = 0;
            u32 FrameIndexLo = 0;
            u32 FrameIndexHi = 0;
            std::array<u32, kGPUStatCounterSlots> Counters{};
        };
        static_assert(sizeof(GPUStatsBlock) == (4 + kGPUStatCounterSlots) * sizeof(u32),
                      "GPUStatsBlock must be a tightly packed uint array to mirror the std430 block");

        struct RingSlot
        {
            RHI::ResourceHandle Buffer{};
            u64 Fence = 0;
            u64 FrameIndex = 0;
            bool Pending = false;
        };

        struct Data
        {
            Ref<StorageBuffer> StatsBuffer;
            std::array<RingSlot, kRingSlots> Ring{};
            u32 NextSlot = 0;
            u32 SlotsInFlight = 0;
            u64 FrameIndex = 0;

            GPUReadbackStatsFrame Latest{};

            std::atomic<bool> Enabled{ false };
            std::atomic<bool> Initialised{ false };
            // Was the live buffer armed for THIS frame? EndFrame refuses to copy
            // a buffer BeginFrame never cleared -- otherwise a frame that flipped
            // the toggle on mid-way would publish counters accumulated against a
            // stale header, i.e. attribute this frame's work to an older frame
            // index. Being wrong about *which frame* a number belongs to is the
            // specific failure this channel exists to prevent.
            bool Armed = false;
            // Was the channel armed on the PREVIOUS frame? Lets the disabled
            // path collapse the block exactly once instead of re-uploading it
            // every frame, which is what makes "off costs one scalar load"
            // literally true rather than nearly true.
            bool WasArmed = false;
        };

        static Data& Get();
        static void RetireCompletedSlots(Data& data);
        static void ReleaseRing(Data& data);
    };
} // namespace OloEngine

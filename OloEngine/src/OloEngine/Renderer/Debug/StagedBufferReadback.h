#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"

#include <array>

namespace OloEngine
{
    // A GPU -> CPU read that never touches the SOURCE buffer from the CPU.
    //
    // The trap this exists for: a buffer the GPU writes with atomics — or reads
    // as an indirect-argument / storage source — must stay in video memory. A
    // CPU glGetNamedBufferSubData straight off it makes NVIDIA log "CPU is
    // consuming buffer object data ... inconsistent with this usage pattern"
    // (131188) and then migrate the buffer VIDEO -> HOST (131186). The cost is
    // NOT the one read: the migration is permanent, so every subsequent frame's
    // atomics against that buffer pay for it. Throttling the read does not help
    // for the same reason — see docs/agent-rules/gpu-readback-stats-channel.md.
    //
    // So the read goes through a small DeviceToHost staging buffer that the GPU
    // copies into. Two usage shapes with two pairs of calls, and which pair is
    // right depends on whether the reader may WAIT:
    //
    //   * StageDeferred() this frame, ReadCompleted() a LATER frame: a statistic.
    //     The read never waits and never returns bytes no copy wrote. A copy is
    //     fenced when it is issued and handed out once its fence has signalled,
    //     so a value is at least one frame old and can be older when the GPU is
    //     behind. Copies are handed out one per call in the order they were
    //     staged, so a reader that polls once a frame sees EVERY frame's value,
    //     late, and a one-frame event is not skipped. Right for a per-frame
    //     counter or an overflow flag.
    //   * Stage() then Read() immediately: a human or a test asked for the
    //     CURRENT value, outside a frame. Still blocks on the copy, which is the
    //     point. The source buffer still never migrates.
    //
    // DO NOT Stage() inside one frame and Read() in the next. On OpenGL that
    // read waits for the copy, so it is exact and can stall. On Vulkan nothing
    // ties it to the copy having run: with frames in flight the previous
    // frame's submission has not necessarily executed, so the read returned the
    // copy before it, or on a new staging buffer whatever its memory held
    // (#1697: the virtual shadow map's page counters skipped the one frame that
    // drew every page, and once read 0xFF9D9D9D as a page count).
    // GPUFluidSolver's body-impulse harvest still has that shape. It is
    // simulation feedback, not a statistic, so "late and never waits" is not
    // obviously its rule (#1702).
    class StagedBufferReadback
    {
      public:
        StagedBufferReadback() = default;
        ~StagedBufferReadback();

        StagedBufferReadback(const StagedBufferReadback&) = delete;
        StagedBufferReadback& operator=(const StagedBufferReadback&) = delete;
        StagedBufferReadback(StagedBufferReadback&& other) noexcept;
        StagedBufferReadback& operator=(StagedBufferReadback&& other) noexcept;

        // Issue the GPU-side copy. `barriers` is ORed with BufferUpdate before
        // the copy: a glCopyNamedBufferSubData is a buffer-update client, so the
        // ShaderStorage barrier that ordered the writing kernel does NOT order
        // this copy against it — pass ShaderStorage as well whenever the source
        // was written by shader atomics, or the copy reads a value that is right
        // most of the time.
        //
        // That barrier is load-bearing on GL and inert on VULKAN, where calling
        // it between frames is a counted no-op: there is no command buffer for
        // it to order anything in. Nothing is lost — VulkanRendererAPI::
        // CopyBufferSubData runs the between-frames copy as a one-shot submit
        // carrying its own ALL_COMMANDS -> COPY availability barrier, and a
        // separate submission is ordered after every frame already submitted.
        // Pinned by VulkanRawBufferUpload.StagedReadbackOutsideAFrameCopiesRatherThanNoOps,
        // which also explains why the no-op must not be "fixed" away.
        void Stage(RHI::ResourceHandle source, u64 srcOffsetBytes, u64 sizeBytes,
                   MemoryBarrierFlags barriers = MemoryBarrierFlags::ShaderStorage);

        // Copy the staged bytes out. False when nothing has been staged yet (the
        // first frames of the deferred shape), so the caller keeps its previous
        // value rather than reading an uninitialised buffer.
        [[nodiscard]] bool Read(void* dest, u64 sizeBytes);

        [[nodiscard]] bool HasPendingStage() const
        {
            return m_Staged;
        }

        // ---- the deferred shape: fenced, never waits ------------------------
        static constexpr u32 kRingSlots = 4;

        // Issue the copy into a free ring slot and fence it. `barriers` as for
        // Stage(). False, copying nothing, when every slot holds a copy that
        // has not been read yet: the reader is kRingSlots copies behind, and
        // this frame's value is the one that is dropped.
        bool StageDeferred(RHI::ResourceHandle source, u64 srcOffsetBytes, u64 sizeBytes,
                           MemoryBarrierFlags barriers = MemoryBarrierFlags::ShaderStorage);

        // Copy out the OLDEST deferred copy, if its fence has signalled. False
        // when no copy has finished (the first frames, or a GPU that is
        // behind), so the caller keeps its previous value. Never waits.
        [[nodiscard]] bool ReadCompleted(void* dest, u64 sizeBytes);

        // Deferred copies issued and not read yet, finished or not.
        [[nodiscard]] u32 GetDeferredCopiesOutstanding() const;

        // Drop the staging buffer.
        //
        // CALL THIS FROM YOUR OWNER'S Shutdown()/DestroyResources(), not just at
        // destruction, whenever the owner might outlive the renderer — anything
        // reachable from a process static does. RenderCommand::s_RendererAPI is
        // never reset, only destroyed at atexit, so a null check cannot save the
        // destructor: by then the pointee is gone and DeleteBuffer dereferences
        // it. The symptom is an access violation AFTER every test has passed,
        // which is exactly how it first showed up (VirtualShadowMap lives in
        // Renderer3D::s_Data). See
        // docs/agent-rules/lazy-static-release-ownership.md.
        void Release();

      private:
        RHI::ResourceHandle m_Staging = RHI::NullResource;
        u64 m_Capacity = 0;
        bool m_Staged = false;

        struct RingSlot
        {
            RHI::ResourceHandle Buffer = RHI::NullResource;
            u64 Capacity = 0;
            u64 Bytes = 0;
            u64 Fence = 0;
            u64 Serial = 0; // order of issue; the lowest outstanding one is read first
            bool Outstanding = false;
        };
        void ReleaseRing();

        std::array<RingSlot, kRingSlots> m_Ring{};
        u64 m_NextSerial = 1;
    };
} // namespace OloEngine

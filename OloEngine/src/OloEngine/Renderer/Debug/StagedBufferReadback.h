#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"

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
    // copies into. Two usage shapes, and the difference is only WHEN you call
    // Read():
    //
    //   * Stage() this frame, Read() a LATER frame — the read never waits on
    //     anything, at the cost of the value being a frame or more old. Right
    //     for a per-frame counter or an overflow flag.
    //   * Stage() then Read() immediately — still blocks on the copy, which is
    //     the point when a human or a test asked for the CURRENT value. The
    //     source buffer still never migrates.
    //
    // Not a fenced ring: there is no IsFenceSignaled here, so `Stage` overwrites
    // whatever the previous Stage left un-read. Consumers that need "skip rather
    // than block when the GPU is behind" want GPUReadbackStats or
    // TerrainGPUPicker's ring instead.
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
    };
} // namespace OloEngine

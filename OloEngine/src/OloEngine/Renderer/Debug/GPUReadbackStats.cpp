#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Debug/GPUReadbackStats.h"

#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <algorithm>

namespace OloEngine
{
    namespace
    {
        // Mirrors GPUStatsBlock, which is private to the class. Spelled from the
        // registry constant rather than `sizeof` so that this file and the .glsl
        // are derived from the SAME number; GPUStatsBlock's own static_assert is
        // what ties the struct to it.
        constexpr u32 kStatsBlockBytes = static_cast<u32>((4 + kGPUStatCounterSlots) * sizeof(u32));
    } // namespace

    GPUReadbackStats::Data& GPUReadbackStats::Get()
    {
        static Data s_Data;
        return s_Data;
    }

    void GPUReadbackStats::Init()
    {
        auto& data = Get();
        if (data.Initialised)
            return;

        // DynamicCopy: the GPU both writes it (every pass's atomics) and is the
        // source of the per-frame copy into the ring. It must stay in video
        // memory -- see the header's note on the NVIDIA VIDEO->HOST migration.
        data.StatsBuffer = StorageBuffer::Create(kStatsBlockBytes, ShaderBindingLayout::SSBO_GPU_STATS,
                                                 StorageBufferUsage::DynamicCopy);
        if (!data.StatsBuffer)
        {
            OLO_CORE_ERROR("GPUReadbackStats: stats buffer allocation failed ({} bytes)", kStatsBlockBytes);
            return;
        }
        data.StatsBuffer->ClearData();
        // Bind once, here. The GLSL helpers open by reading `b_StatsEnabled` out
        // of this block, and reading an unbound SSBO is undefined in GL (the spec
        // explicitly permits program termination). Binding a zeroed buffer at
        // init is what makes the *disabled* path safe rather than merely cheap.
        data.StatsBuffer->Bind();

        for (auto& slot : data.Ring)
        {
            slot.Buffer = RenderCommand::CreateBufferHandle();
            RenderCommand::AllocateBufferStorage(slot.Buffer, kStatsBlockBytes, RHI::MemoryResidency::DeviceToHost);
            slot.Pending = false;
            slot.Fence = 0;
        }

        data.NextSlot = 0;
        data.SlotsInFlight = 0;
        data.FrameIndex = 0;
        data.Latest = {};
        data.Armed = false;
        data.WasArmed = false;
        data.Initialised = true;

        OLO_CORE_INFO("GPUReadbackStats: initialised at SSBO binding {} ({} counters, {} flags, {}-slot ring).",
                      ShaderBindingLayout::SSBO_GPU_STATS, kGPUStatCounterCount, kGPUStatFlagCount, kRingSlots);
    }

    void GPUReadbackStats::Shutdown()
    {
        auto& data = Get();
        if (!data.Initialised)
            return;

        ReleaseRing(data);
        data.StatsBuffer.Reset();
        data.Enabled = false;
        data.Initialised = false;
        data.Latest = {};
        data.Armed = false;
        data.WasArmed = false;
        data.SlotsInFlight = 0;
        data.FrameIndex = 0;
    }

    void GPUReadbackStats::ReleaseRing(Data& data)
    {
        for (auto& slot : data.Ring)
        {
            // Destroy the fence BEFORE the buffer, and destroy it even when the
            // slot is still pending. A pending slot's fence is a live GL sync
            // object; dropping the handle without DestroyFence leaks it for the
            // lifetime of the context, and a session that toggles the feature
            // repeatedly would leak one per toggle.
            if (slot.Fence != 0)
            {
                RenderCommand::DestroyFence(slot.Fence);
                slot.Fence = 0;
            }
            if (slot.Buffer.IsValid())
            {
                RenderCommand::DeleteBuffer(slot.Buffer);
                slot.Buffer = RHI::NullResource;
            }
            slot.Pending = false;
            slot.FrameIndex = 0;
        }
        data.NextSlot = 0;
    }

    bool GPUReadbackStats::IsInitialised()
    {
        return Get().Initialised.load(std::memory_order_relaxed);
    }

    void GPUReadbackStats::SetEnabled(bool enabled)
    {
        Get().Enabled.store(enabled, std::memory_order_relaxed);
    }

    bool GPUReadbackStats::IsEnabled()
    {
        return Get().Enabled.load(std::memory_order_relaxed);
    }

    u64 GPUReadbackStats::GetFrameIndex()
    {
        return Get().FrameIndex;
    }

    u32 GPUReadbackStats::GetSlotsInFlight()
    {
        return Get().SlotsInFlight;
    }

    const GPUReadbackStatsFrame& GPUReadbackStats::GetLatest()
    {
        return Get().Latest;
    }

    void GPUReadbackStats::BindForDispatch()
    {
        auto& data = Get();
        if (!data.Initialised || !data.StatsBuffer)
            return;
        data.StatsBuffer->Bind();
    }

    void GPUReadbackStats::RetireCompletedSlots(Data& data)
    {
        OLO_PROFILE_FUNCTION();

        u32 inFlight = 0;
        // OLDEST FIRST, not array order. `NextSlot` is the slot the next capture
        // will use, so it is also the oldest one still in flight; walking from
        // there wraps through the ring in ISSUE order. Array order would let a
        // wrapped ring publish an OLDER frame after a newer one had already
        // retired -- counters that go backwards in time, which is worse than
        // stale counters because it looks like a real change.
        for (u32 offset = 0; offset < kRingSlots; ++offset)
        {
            RingSlot& slot = data.Ring[(data.NextSlot + offset) % kRingSlots];
            if (!slot.Pending)
                continue;

            // THE WHOLE POINT: ask, never wait. `IsFenceSignaled` is a poll, so a
            // slot the GPU has not finished is left for a later frame. There is
            // no ClientWaitFence anywhere in this class and adding one would
            // reintroduce exactly the stall this design exists to remove.
            if (!RenderCommand::IsFenceSignaled(slot.Fence))
            {
                ++inFlight;
                continue;
            }

            GPUStatsBlock block{};
            RenderCommand::ReadBufferSubData(slot.Buffer, 0, kStatsBlockBytes, &block);

            RenderCommand::DestroyFence(slot.Fence);
            slot.Fence = 0;
            slot.Pending = false;

            // Publish only if this slot is NEWER than what we already hold.
            // Retiring in issue order makes that true by construction today, but
            // asserting it here is what keeps a future ring-order change from
            // silently reintroducing the backwards-in-time failure above.
            const u64 slotFrame = slot.FrameIndex;
            if (data.Latest.Valid && slotFrame <= data.Latest.FrameIndex)
                continue;

            data.Latest.Valid = true;
            data.Latest.FrameIndex = slotFrame;
            data.Latest.Latency = static_cast<u32>(data.FrameIndex - slotFrame);
            data.Latest.Flags = block.Flags;
            std::copy_n(block.Counters.begin(), kGPUStatCounterCount, data.Latest.Counters.begin());
        }
        data.SlotsInFlight = inFlight;
    }

    void GPUReadbackStats::BeginFrame()
    {
        OLO_PROFILE_FUNCTION();

        auto& data = Get();
        data.Armed = false;
        if (!data.Initialised || !data.StatsBuffer)
            return;

        ++data.FrameIndex;

        // Retire regardless of the enable flag: a slot captured on the last
        // enabled frame is still in flight when the toggle flips off, and its
        // fence must be destroyed and its buffer read (or at minimum released)
        // rather than left pending forever.
        RetireCompletedSlots(data);

        const bool enabled = data.Enabled.load(std::memory_order_relaxed);
        if (!enabled)
        {
            // Steady-state disabled costs ONE relaxed atomic load and nothing
            // else -- no upload, no clear, no bind, no copy, no fence. That is
            // the contract this header, RendererSettings and the MCP tool schema
            // all advertise, and it is the claim the tool's `enabled` argument
            // exists to let somebody A/B; uploading 144 bytes every disabled
            // frame anyway would have made all three statements false together.
            //
            // ONE-SHOT COLLAPSE on the frame the toggle flips off, so the block
            // the shaders still read reports Enabled == 0 rather than keeping
            // whatever the last enabled frame left there. `WasArmed` is what
            // makes it one-shot: without it this would be the per-frame upload
            // the paragraph above just refused.
            if (data.WasArmed)
            {
                data.WasArmed = false;
                GPUStatsBlock off{};
                data.StatsBuffer->SetData(&off, kStatsBlockBytes);
                data.StatsBuffer->Bind();
            }
            return;
        }

        GPUStatsBlock header{};
        header.Flags = 0;
        header.Enabled = 1u;
        header.FrameIndexLo = static_cast<u32>(data.FrameIndex & 0xFFFFFFFFull);
        header.FrameIndexHi = static_cast<u32>(data.FrameIndex >> 32);
        // A full-block upload, not a header-only one: this is what ZEROES the
        // counters for the new frame. A per-frame counter that is not cleared
        // accumulates across the whole session and reads as a monotonically
        // rising number nobody notices is wrong -- the counters would still be
        // "a number", which is the failure mode this channel is built to avoid.
        data.StatsBuffer->SetData(&header, kStatsBlockBytes);

        // Re-bind after the write. The bind survives the process today (nothing
        // else claims this slot), but a re-created buffer -- or a foreign
        // BindStorageBuffer at the same number -- would otherwise leave the
        // frame's atomics landing somewhere else with no diagnostic.
        data.StatsBuffer->Bind();

        data.Armed = true;
        data.WasArmed = true;
    }

    void GPUReadbackStats::EndFrame()
    {
        OLO_PROFILE_FUNCTION();

        auto& data = Get();
        if (!data.Initialised || !data.StatsBuffer || !data.Armed)
            return;
        data.Armed = false;

        RingSlot& slot = data.Ring[data.NextSlot];
        if (slot.Pending || !slot.Buffer.IsValid())
        {
            // A full ring means every slot is still executing. Skip the capture
            // and keep the newest retired frame rather than blocking on the
            // oldest -- SlotsInFlight is what tells the overlay this happened.
            return;
        }

        // The passes wrote this buffer with SHADER atomics; the copy below is a
        // buffer-update client and needs its own barrier class. ShaderStorage
        // orders the atomics themselves, BufferUpdate orders the copy against
        // them -- both, because dropping either makes the copy read a value that
        // is right *most* of the time.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::BufferUpdate);
        RenderCommand::CopyBufferSubData(data.StatsBuffer->GetRHIHandle(), slot.Buffer, 0, 0, kStatsBlockBytes);

        slot.Fence = RenderCommand::CreateFence();
        if (slot.Fence == 0)
        {
            // Fence creation failed: leave the slot free rather than pending.
            // A pending slot with a zero fence would either be polled forever
            // (IsFenceSignaled(0) is false) or -- worse, if it ever reported
            // true -- read before the copy completed, which is the torn read
            // this whole design exists to make impossible.
            OLO_CORE_WARN("GPUReadbackStats: fence creation failed; dropping this frame's capture.");
            return;
        }
        slot.Pending = true;
        slot.FrameIndex = data.FrameIndex;
        data.NextSlot = (data.NextSlot + 1u) % kRingSlots;
    }
} // namespace OloEngine

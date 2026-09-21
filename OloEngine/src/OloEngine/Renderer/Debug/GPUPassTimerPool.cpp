#include "OloEnginePCH.h"
#include "GPUPassTimerPool.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Renderer/RenderCommand.h"

namespace OloEngine
{
    GPUPassTimerPool& GPUPassTimerPool::GetInstance()
    {
        static GPUPassTimerPool instance;
        return instance;
    }

    GPUPassTimerPool::~GPUPassTimerPool()
    {
        OLO_CORE_ASSERT(!m_Initialized, "GPUPassTimerPool::~GPUPassTimerPool: Shutdown() was not called before destruction!");
    }

    void GPUPassTimerPool::Initialize(u32 maxPassesPerFrame)
    {
        OLO_PROFILE_FUNCTION();
        if (m_Initialized)
            return;

        // Timestamp queries through the facade (#691): RHI::QueryType::
        // Timestamp stamps via WriteTimestamp on both backends (glQueryCounter /
        // vkCmdWriteTimestamp), with results in nanoseconds either way. The
        // former GL-only early-out is gone with the direct glad calls it
        // guarded; staying UNinitialized remains the complete disable if a
        // backend ever refuses (every per-frame entry point gates on m_Active,
        // which only BeginFrame — itself gated on m_Initialized — can set), and
        // an uninitialized pool now reports Unavailable rather than zeros.
        m_MaxPasses = maxPassesPerFrame;

        const u32 queriesPerSlot = 2 + (2 * maxPassesPerFrame);
        for (auto& slot : m_Slots)
        {
            slot.Queries.assign(queriesPerSlot, RHI::NullResource);
            RenderCommand::CreateQueries(RHI::QueryType::Timestamp, std::span<RHI::ResourceHandle>(slot.Queries));
            slot.Stamped.assign(queriesPerSlot, 0u);
            slot.PassNames.resize(maxPassesPerFrame);
            slot.PassIsSubPass.assign(maxPassesPerFrame, 0u);
            slot.PassParentNames.resize(maxPassesPerFrame);
            slot.PassCount = 0;
            slot.UntimedPasses = 0;
            slot.FrameNumber = 0;
            slot.Pending = false;
        }

        m_WriteSlot = 0;
        m_FrameCounter = 0;
        m_Active = false;
        m_PassOpen = false;
        m_SubPassOpen = false;
        m_LastFrameSample = GpuTimingSample::Absent(GpuTimingStatus::Pending);
        m_LastResolvedFrame = 0;
        m_DroppedSlots = 0;
        m_UnstampedFrames = 0;
        m_DropWarningIssued = false;
        m_LastPassTimings.clear();
        m_Initialized = true;

        OLO_CORE_INFO("GPUPassTimerPool: Initialized with {} pass slots x {} frames in flight", maxPassesPerFrame, kSlotCount);
    }

    void GPUPassTimerPool::Shutdown()
    {
        OLO_PROFILE_FUNCTION();
        if (!m_Initialized)
            return;

        for (auto& slot : m_Slots)
        {
            if (!slot.Queries.empty())
            {
                RenderCommand::DeleteQueries(std::span<const RHI::ResourceHandle>(slot.Queries));
                slot.Queries.clear();
            }
            slot.Stamped.clear();
            slot.PassNames.clear();
            slot.PassIsSubPass.clear();
            slot.PassParentNames.clear();
            slot.Pending = false;
        }

        m_LastPassTimings.clear();
        // The published sample follows the pool down: a shut-down pool that
        // kept handing out its last real measurement would report a number for
        // a device that is no longer being timed.
        m_LastFrameSample = GpuTimingSample::Absent(GpuTimingStatus::Unavailable);
        m_Initialized = false;
        m_Active = false;
        m_PassOpen = false;
        m_SubPassOpen = false;

        OLO_CORE_INFO("GPUPassTimerPool: Shutdown");
    }

    void GPUPassTimerPool::ResetSlotForFrame(FrameSlot& slot, u64 frameNumber)
    {
        // Every pair starts the frame unstamped. A pair left over from the
        // previous ring cycle still holds that cycle's timestamps on GL, so
        // without this a bracket the backend refuses THIS frame would resolve
        // against four-frame-old values and look perfectly healthy.
        std::fill(slot.Stamped.begin(), slot.Stamped.end(), static_cast<u8>(0));
        slot.FrameNumber = frameNumber;
        slot.PassCount = 0;
        slot.UntimedPasses = 0;
        slot.Pending = false;
    }

    void GPUPassTimerPool::StampQuery(FrameSlot& slot, u32 queryIndex)
    {
        if (queryIndex >= slot.Queries.size())
            return;

        // The backend's answer is the ground truth: Vulkan refuses a timestamp
        // recorded from a RecordParallel worker (ADR 0011 amendment (92) rule
        // 7) and outside a recording bracket; GL refuses a retired handle.
        // Each of those used to produce a silent 0.0 ms interval.
        slot.Stamped[queryIndex] = RenderCommand::WriteTimestamp(slot.Queries[queryIndex]) ? 1u : 0u;
    }

    void GPUPassTimerPool::BeginFrame()
    {
        OLO_PROFILE_FUNCTION();
        if (!m_Initialized)
            return;

        // Resolve pending slots oldest-first so the published results always end
        // on the newest fully-completed frame. Walking (m_WriteSlot + i) visits
        // slots in ascending write age.
        for (u32 i = 1; i <= kSlotCount; ++i)
        {
            FrameSlot& slot = m_Slots[(m_WriteSlot + i) % kSlotCount];
            if (slot.Pending)
                TryResolveSlot(slot, false);
        }

        m_WriteSlot = (m_WriteSlot + 1) % kSlotCount;
        FrameSlot& slot = m_Slots[m_WriteSlot];

        // About to overwrite: if the GPU is somehow >kSlotCount-1 frames behind,
        // drop the stale slot rather than stalling on it.
        if (slot.Pending)
            TryResolveSlot(slot, true);

        ++m_FrameCounter;
        ResetSlotForFrame(slot, m_FrameCounter);

        StampQuery(slot, 0);
        m_Active = true;
        m_PassOpen = false;
        m_SubPassOpen = false;
    }

    void GPUPassTimerPool::EndFrame()
    {
        if (!m_Initialized || !m_Active)
            return;

        // Close any bracket left open (a Begin without its End). Every
        // allocated pair MUST have both timestamps stamped before the slot is
        // marked pending — a pair with one endpoint now resolves to NotStamped
        // rather than to a garbage duration, but closing the bracket is still
        // the honest thing to do because the work really did end here.
        EndSubPass();
        EndPass();

        FrameSlot& slot = m_Slots[m_WriteSlot];
        StampQuery(slot, 1);
        slot.Pending = true;
        m_Active = false;
    }

    void GPUPassTimerPool::BeginPass(const std::string& name)
    {
        if (!m_Active || m_PassOpen)
            return;

        FrameSlot& slot = m_Slots[m_WriteSlot];
        if (slot.PassCount >= m_MaxPasses)
        {
            // The pass runs; it just gets no bracket. Counted so the resolved
            // frame can say its pass list is short of the graph instead of
            // quietly omitting the entry.
            ++slot.UntimedPasses;
            return;
        }

        // Allocate the pair up front (rather than on EndPass) so a sub-pass
        // opened inside this bracket gets its own pair without colliding.
        m_CurrentPassIndex = slot.PassCount++;
        slot.PassNames[m_CurrentPassIndex] = name;
        slot.PassIsSubPass[m_CurrentPassIndex] = 0u;
        slot.PassParentNames[m_CurrentPassIndex].clear();
        StampQuery(slot, 2 + (2 * m_CurrentPassIndex));
        m_PassOpen = true;
    }

    void GPUPassTimerPool::EndPass()
    {
        if (!m_Active || !m_PassOpen)
            return;

        // A sub-pass left open must not outlive its parent bracket.
        EndSubPass();

        FrameSlot& slot = m_Slots[m_WriteSlot];
        StampQuery(slot, 3 + (2 * m_CurrentPassIndex));
        m_PassOpen = false;
    }

    void GPUPassTimerPool::BeginSubPass(const std::string& name)
    {
        if (!m_Active || !m_PassOpen || m_SubPassOpen)
            return;

        FrameSlot& slot = m_Slots[m_WriteSlot];
        if (slot.PassCount >= m_MaxPasses)
        {
            ++slot.UntimedPasses;
            return;
        }

        m_CurrentSubPassIndex = slot.PassCount++;
        slot.PassNames[m_CurrentSubPassIndex] = slot.PassNames[m_CurrentPassIndex] + "/" + name;
        slot.PassIsSubPass[m_CurrentSubPassIndex] = 1u;
        slot.PassParentNames[m_CurrentSubPassIndex] = slot.PassNames[m_CurrentPassIndex];
        StampQuery(slot, 2 + (2 * m_CurrentSubPassIndex));
        m_SubPassOpen = true;
    }

    void GPUPassTimerPool::EndSubPass()
    {
        if (!m_Active || !m_SubPassOpen)
            return;

        FrameSlot& slot = m_Slots[m_WriteSlot];
        StampQuery(slot, 3 + (2 * m_CurrentSubPassIndex));
        m_SubPassOpen = false;
    }

    GpuTimingSample GPUPassTimerPool::ResolvePair(const FrameSlot& slot, u32 beginIndex, u32 endIndex) const
    {
        if (beginIndex >= slot.Queries.size() || endIndex >= slot.Queries.size())
            return GpuTimingSample::Absent(GpuTimingStatus::NotTimed);

        // Gather what is observable, then let the pure decision in
        // GPUTimingStatus.h classify it. Split that way so the classification —
        // the part that used to collapse four distinct failures into 0.0 — is
        // testable without a device.
        GpuTimingPairReadout readout;

        // Did the backend take the stamps at all? A refused stamp leaves the
        // query holding whatever it held before: zero on a fresh object, a
        // four-frame-old timestamp on a reused one. Both read back cleanly.
        readout.BeginStamped = slot.Stamped[beginIndex] != 0u;
        readout.EndStamped = slot.Stamped[endIndex] != 0u;

        // Are the results actually back? Availability is probed FIRST, per pair,
        // and it is not an optimization: TryGetQueryResultU64 blocks on both
        // backends (GL's GL_QUERY_RESULT, Vulkan's WAIT read), and this pool's
        // cardinal rule is that it never stalls the render thread.
        //
        // The slot-level gate in TryResolveSlot keys on the frame-end query,
        // which covers everything submitted before it on the SAME queue. A pass
        // stamped onto another queue (async compute, #808) is not ordered
        // against it, so relying on the frame's gate alone could hand a
        // still-in-flight query to a blocking read. Each pair is probed in its
        // own right instead. Probing the pair's END is enough: both its stamps
        // go into the same command buffer, so the later one's availability
        // implies the earlier one's.
        //
        // Nanoseconds on both backends (the Vulkan arm owns the
        // timestampPeriod scaling — see RendererAPI::WriteTimestamp's
        // contract), so the subtraction downstream stays backend-blind.
        if (readout.BeginStamped && readout.EndStamped &&
            RenderCommand::IsQueryResultAvailable(slot.Queries[endIndex]))
        {
            readout.BeginReadable = RenderCommand::TryGetQueryResultU64(slot.Queries[beginIndex], readout.BeginNs);
            readout.EndReadable = RenderCommand::TryGetQueryResultU64(slot.Queries[endIndex], readout.EndNs);
        }

        return ResolveGpuTimingPair(readout);
    }

    void GPUPassTimerPool::WarnOnceAboutLostFrame(u64 frameNumber, const char* reason)
    {
        // Rate-limited to once per session on purpose: a GPU this far behind
        // loses a frame EVERY frame, and one line at 60 Hz is 35 000 lines in
        // ten minutes (the GLStateGuard lesson). The per-frame signal is
        // GetDroppedSlotCount(), which every consumer publishes.
        if (m_DropWarningIssued)
        {
            return;
        }
        m_DropWarningIssued = true;
        OLO_CORE_WARN("GPUPassTimerPool: frame {}'s GPU timings were lost — {}. Published pass timings skip this "
                      "frame; the snapshot's age and droppedSlots say by how much. Logged once per session.",
                      frameNumber, reason);
    }

    void GPUPassTimerPool::TryResolveSlot(FrameSlot& slot, bool dropIfUnavailable)
    {
        // The frame-end timestamp is the last query stamped in the slot; queries
        // complete in submission order, so its availability implies every earlier
        // timestamp on the same queue is readable too. It is the cheap gate, not
        // the per-pair proof — ResolvePair still checks each pair.
        // A frame-end stamp the backend never took means this slot can NEVER
        // resolve: there is no completion gate to read the rest behind, and
        // reading blind would block. Retire it at once with its own reason
        // rather than holding it pending for a whole ring cycle and then
        // reporting it as a ring wrap, which is a different fault.
        if (slot.Stamped[1] == 0u)
        {
            // NOT m_DroppedSlots. That counter is published everywhere as "the
            // GPU fell more than a ring behind", and a backend that declines
            // timestamps would make it climb once per frame and report a
            // backlog that never happened — one counter answering two
            // questions, which is the defect this issue exists to remove.
            ++m_UnstampedFrames;
            slot.Pending = false;
            if (m_LastResolvedFrame == 0)
            {
                m_LastFrameSample = GpuTimingSample::Absent(GpuTimingStatus::NotStamped);
            }
            WarnOnceAboutLostFrame(slot.FrameNumber, "the frame-end timestamp was never recorded by the backend");
            return;
        }

        if (!RenderCommand::IsQueryResultAvailable(slot.Queries[1]))
        {
            if (!dropIfUnavailable)
                return;

            // The ring wrapped on a slot the GPU never finished. Publishing
            // nothing would leave the previous frame's numbers in place with no
            // sign that a frame went missing; publishing 0.0 would claim the
            // frame was free. Count it, say so once, and let the age on the
            // published snapshot carry the rest.
            ++m_DroppedSlots;
            slot.Pending = false;
            // Nothing has EVER resolved and the ring is already wrapping, so
            // the published "pending" is not going to become a measurement on
            // its own. Say Dropped instead: a consumer waiting patiently for a
            // number that is never coming is the silent-failure shape again,
            // just slower. Once a real frame HAS resolved, its numbers stay
            // published (with a growing age) rather than being thrown away.
            if (m_LastResolvedFrame == 0)
            {
                m_LastFrameSample = GpuTimingSample::Absent(GpuTimingStatus::Dropped);
            }
            WarnOnceAboutLostFrame(slot.FrameNumber, "the query ring wrapped before the GPU finished them");
            return;
        }

        m_LastFrameSample = ResolvePair(slot, 0, 1);

        m_LastPassTimings.clear();
        m_LastPassTimings.reserve(slot.PassCount + slot.UntimedPasses);
        for (u32 i = 0; i < slot.PassCount; ++i)
        {
            m_LastPassTimings.push_back(PassTiming{
                .Name = slot.PassNames[i],
                .Sample = ResolvePair(slot, 2 + (2 * i), 3 + (2 * i)),
                .IsSubPass = slot.PassIsSubPass[i] != 0u,
                .ParentName = slot.PassParentNames[i],
            });
        }

        // Passes that overflowed the per-frame budget are published by count,
        // under a synthetic name, rather than omitted. A reader comparing the
        // pass list against the graph would otherwise conclude those passes did
        // not execute.
        if (slot.UntimedPasses > 0)
        {
            m_LastPassTimings.push_back(PassTiming{
                .Name = "<" + std::to_string(slot.UntimedPasses) + " pass(es) over the per-frame timer budget>",
                .Sample = GpuTimingSample::Absent(GpuTimingStatus::NotTimed),
                .IsSubPass = false,
                .ParentName = {},
            });
        }

        m_LastResolvedFrame = slot.FrameNumber;
        slot.Pending = false;
    }

    GPUPassTimerPool::FrameTimings GPUPassTimerPool::GetLastFrameTimings() const
    {
        FrameTimings out;
        if (!m_Initialized)
        {
            // Not a zero-filled snapshot: an unavailable pool says so, and a
            // consumer that prints the number gets "unavailable" rather than
            // "0.000 ms".
            out.Frame = GpuTimingSample::Absent(GpuTimingStatus::Unavailable);
            return out;
        }

        out.FrameNumber = m_LastResolvedFrame;
        out.CurrentFrameNumber = m_FrameCounter;
        out.AgeFrames = (m_LastResolvedFrame > 0 && m_FrameCounter >= m_LastResolvedFrame)
                            ? m_FrameCounter - m_LastResolvedFrame
                            : 0;
        out.Frame = m_LastFrameSample;
        out.Passes = m_LastPassTimings;
        out.DroppedSlots = m_DroppedSlots;
        out.UnstampedFrames = m_UnstampedFrames;

        // STALE IS NOT THE SAME AS UNMEASURED, and the two are deliberately not
        // merged here. A snapshot older than the ring still holds numbers that
        // a GPU really produced; they just describe a frame that is no longer
        // representative. Restating them as Dropped would throw away the best
        // available evidence to say something the age already says — and
        // `IsStale()`, `AgeFrames` and `DroppedSlots` are how a caller learns
        // not to treat them as current (the olo_perf_pass_timings staleness
        // flag from #519 is built on exactly this).
        //
        // `Dropped` is reserved for a measurement that never existed, which is
        // set where the drop happens (TryResolveSlot) rather than inferred here.
        return out;
    }

    GPUPassTimerPool::PassTotal GPUPassTimerPool::SumTopLevel(const std::vector<PassTiming>& passes)
    {
        PassTotal total;
        for (const PassTiming& pass : passes)
        {
            // A sub-pass interval is INSIDE its parent's. Adding both is the
            // double-count criterion 2 of #1337 forbids.
            if (pass.IsSubPass)
            {
                ++total.ExcludedSubPasses;
                continue;
            }
            if (pass.IsValid())
            {
                total.GpuMs += pass.Sample.GpuMs;
                ++total.ValidPasses;
            }
            else
            {
                ++total.UnmeasuredPasses;
            }
        }
        return total;
    }
} // namespace OloEngine

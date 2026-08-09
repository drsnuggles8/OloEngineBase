#pragma once

// =============================================================================
// RHIGpuFence.h — the split-barrier / timeline signal-wait primitive.
//
// Issue #691 Phase 6, ADR 0011 §6.
//
// WHAT THIS IS FOR. A split barrier is a plain GPU-memory location plus two
// operations — Signal(value, op) on the producing side and Wait(value,
// compareOp) on the consuming side — deliberately unified with the timeline
// semaphore (core Vulkan 1.2, inside the ADR 0010 floor) rather than invented
// as a second, Vulkan-events-shaped abstraction. One primitive serves BOTH
// sides of the API: a GPU-side cross-submission dependency (queue signal →
// queue wait) and a CPU-side frame-pacing check (queue signal → host wait) are
// the same fence and the same monotonically increasing counter.
//
// WHAT THIS IS NOT. It does not replace §1.5's ordinary same-command-buffer
// barrier model (`RHI::Barrier` spans through `IssueBarrierBatch`) — that
// stays exactly as decided. A GpuFence exists for the specific case where a
// blocking barrier would leave the GPU idle across a longer span (cross-pass
// tail latency, cross-frame dependencies). WHICH render-graph passes are worth
// splitting is a Phase 7 per-pass profiling decision; Phase 6 only establishes
// that the primitive exists and what shape it has. The existing
// `RendererAPI::CreateFence`/`ClientWaitFence` family (GL's one-shot binary
// `GLsync`, consumed by `FrameResourceManager`) is untouched — migrating that
// chain onto GpuFence is the §6 follow-up, not a Phase 6 requirement.
//
// VALUE DISCIPLINE. Values are monotonic. The Vulkan timeline contract
// requires every signal to strictly increase the counter, so `AtomicMax` /
// `GreaterEqual` is the native pair and the one every caller should default
// to. `Set` / `Equal` (§6's minimum op set) are representable but carry the
// same monotonicity requirement on this backend — a `Set` to a value at or
// below the current counter is a caller bug and asserts.
//
// BACKEND AVAILABILITY. Phase 6 implements the Vulkan backend only
// (`Platform/Vulkan/VulkanGpuFence`). `Create()` returns null on other
// backends — callers branch on the returned pointer the same way heap callers
// branch on `IsBindlessSupported()`: every call site must be able to take the
// other path.
//
// This header is API-neutral and stays that way (RHIBoundaryRatchetTest pins
// it): the backend reaches Vulkan through `Platform/Vulkan/`, and only the
// factory TU (RHIGpuFence.cpp) names it, under the sanctioned
// OLO_WITH_VULKAN-guarded factory-include pattern.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"

#include <algorithm>
#include <limits>

namespace OloEngine::RHI
{
    // §6's minimum op set. On a timeline-semaphore backend `Set` still obeys
    // the monotonic contract (see header comment).
    enum class FenceSignalOp : u8
    {
        Set,       ///< Write the value (monotonic on timeline backends).
        AtomicMax, ///< max(counter, value) — the timeline-native op.
    };

    enum class FenceCompareOp : u8
    {
        Equal,        ///< Satisfied when counter == value.
        GreaterEqual, ///< Satisfied when counter >= value — the timeline-native op.
    };

    // One fence = one monotonically increasing 64-bit counter, visible to both
    // the GPU timeline and the host.
    class GpuFence : public RefCounted
    {
      public:
        ~GpuFence() override = default;

        // --- GPU side (queue-attached) ---------------------------------------
        // Queue ops do not execute immediately: they are staged against the
        // fence and consumed by the NEXT queue submission the backend makes
        // (the render loop's submit, or a test fixture's). Signal attaches to
        // the producing submission's signal list, Wait to the consuming
        // submission's wait list.
        virtual void QueueSignal(u64 value, FenceSignalOp op = FenceSignalOp::AtomicMax) = 0;
        virtual void QueueWait(u64 value, FenceCompareOp compareOp = FenceCompareOp::GreaterEqual) = 0;

        // --- CPU side --------------------------------------------------------
        virtual void HostSignal(u64 value, FenceSignalOp op = FenceSignalOp::AtomicMax) = 0;
        // Blocks until the compare is satisfied or the timeout elapses.
        // Returns true when satisfied, false on timeout.
        [[nodiscard]] virtual bool HostWait(u64 value, u64 timeoutNanoseconds,
                                            FenceCompareOp compareOp = FenceCompareOp::GreaterEqual) = 0;
        // The counter's current value as observed by the host (never blocks).
        [[nodiscard]] virtual u64 CompletedValue() const = 0;

        // Monotonic value dispenser for callers that treat the fence as a
        // frame/step timeline: each call returns the next value to signal.
        // Anchored on max(reservation floor, completed): the floor covers
        // every value this object has dispensed OR seen pass through a signal
        // call (NoteSignalValue below — a QueueSignal(100) that has not
        // submitted yet is invisible to the live counter but must still
        // reserve 100), so the returned value is STRICTLY above both the live
        // counter and every staged signal (the timeline contract).
        //
        // UINT64_MAX is RESERVED as the exhaustion/poison sentinel, never
        // dispensed as an incremented result: a wrap to 0 would silently
        // violate monotonicity, so the dispenser saturates instead
        // (unreachable by counting — 2^64 increments — but reachable by a
        // caller hand-signalling a huge value first, and deliberately
        // produced by a backend whose counter READ failed — see
        // VulkanGpuFence::CompletedValue).
        [[nodiscard]] u64 NextValue()
        {
            const u64 base = std::max(m_ReservedValueFloor, CompletedValue());
            if (base >= std::numeric_limits<u64>::max() - 1)
            {
                OLO_CORE_ERROR("RHI::GpuFence: timeline value space exhausted (or the counter read failed) — "
                               "saturating at UINT64_MAX");
                m_ReservedValueFloor = std::numeric_limits<u64>::max();
                return m_ReservedValueFloor;
            }
            m_ReservedValueFloor = base + 1;
            return m_ReservedValueFloor;
        }
        [[nodiscard]] u64 LastDispensedValue() const
        {
            return m_ReservedValueFloor;
        }

        // Null on backends without an implementation (currently everything but
        // Vulkan) — callers must branch, not assume.
        [[nodiscard]] static Ref<GpuFence> Create(u64 initialValue = 0);

      protected:
        explicit GpuFence(u64 initialValue) : m_ReservedValueFloor(initialValue)
        {
        }

        // Backends call this from EVERY signal path (queue-staged and host)
        // so a hand-picked signal value raises the dispenser's reservation
        // floor — a staged-but-unsubmitted QueueSignal(100) must prevent
        // NextValue() from dispensing anything at or below 100.
        void NoteSignalValue(u64 value)
        {
            m_ReservedValueFloor = std::max(m_ReservedValueFloor, value);
        }

      private:
        // Doubles as the dispenser state and the reservation floor for
        // hand-picked signal values (see NoteSignalValue).
        u64 m_ReservedValueFloor = 0;
    };
} // namespace OloEngine::RHI

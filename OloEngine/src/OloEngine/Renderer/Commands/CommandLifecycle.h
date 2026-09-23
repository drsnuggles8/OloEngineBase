#pragma once

#include "OloEngine/Core/Base.h"

/*
 * The command lifecycle (issue #1335): PREPARE, then FREEZE, then REPLAY, then RETIRE.
 *
 *   Prepare  Packets are built, their bone offsets remapped from worker-local to
 *            global, sorted and batched. Everything may be written: the packet's
 *            inline command, its metadata, the bucket's arrays, and the frame's
 *            FrameDataBuffer payloads.
 *   Freeze   The first replay of a bucket (or of a loose packet span) freezes it.
 *            From here on a packet's bytes and a bucket's order are fixed, and the
 *            FrameDataBuffer publishes every range allocated so far. Replay may run
 *            on several workers at once and reads all three without locks, so this
 *            is the point after which a write is a data race rather than an edit.
 *   Replay   Any number of times, serial or parallel. Replay reads only; its
 *            statistics and dispatcher state live in the caller, never in a packet.
 *   Retire   CommandBucket::Clear/Reset and FrameDataBuffer::Reset return both to
 *            Prepare for the next frame. Packet memory is reclaimed by the
 *            allocator reset, so a packet is never un-frozen: a new one is built.
 *
 * What is enforced, and how:
 *   - Always: every mutating packet accessor, every mutating bucket operation and
 *     every write into a published FrameDataBuffer range checks the state. A
 *     violation is counted, logged, asserted in Debug, and REFUSED wherever the
 *     API can refuse (bucket operations, payload writes). A packet accessor that
 *     hands out a pointer cannot refuse, so it is counted and logged.
 *   - With validation on (Debug by default, the test suite always): freezing a
 *     packet also records a digest of its bytes, and every replay re-checks it
 *     before any worker starts and again after they finish. A mismatch before the
 *     replay refuses the replay; a mismatch after it reports a write that raced
 *     the replay. This is what catches a write through a pointer taken during
 *     preparation, which no accessor can see.
 *
 * A pass that needs a different version of a frozen packet copies it
 * (CommandPacket::Clone) and edits the copy; the copy starts in Prepare.
 */

namespace OloEngine::CommandLifecycle
{
    enum class Violation : u8
    {
        PacketMutatedAfterFreeze,   // a mutating CommandPacket accessor on a frozen packet
        PacketChangedAfterFreeze,   // validation: frozen bytes differ before a replay
        PacketChangedDuringReplay,  // validation: frozen bytes differ after a replay
        BucketMutatedAfterFreeze,   // Submit / Sort / Batch / Remap / Merge on a frozen bucket
        ReplayOfUnfrozenBucket,     // a const replay entry reached a bucket nobody froze
        PayloadWrittenAfterPublish, // a FrameDataBuffer write into a published range
        Count
    };

    [[nodiscard]] const char* ToString(Violation violation);

    // Counts, logs (the 1st, 2nd, 4th, 8th... occurrence of each kind, so a
    // per-frame fault stays visible without flooding the log) and asserts in
    // Debug unless a ScopedExpectedViolations is live. Thread-safe.
    void ReportViolation(Violation violation, const char* where);

    [[nodiscard]] u64 GetViolationCount(Violation violation);
    [[nodiscard]] u64 GetTotalViolationCount();

    // Content validation. On by default in Debug builds, off in Release, where
    // the per-packet digest is the measurable cost of the lifecycle (see
    // CommandLifecycleCostTest). The OLO_COMMAND_LIFECYCLE_VALIDATION lever
    // overrides the default either way ("1" on, "0" off), including at run
    // time through olo_debug_levers_set. The test binary turns it on for
    // every case.
    [[nodiscard]] bool IsValidationEnabled();
    void SetValidationEnabled(bool enabled);

    // For negative-control tests: violations raised while one of these is
    // alive are still counted and logged, but do not break into the debugger
    // in a Debug build, and are not charged to the test by the suite's
    // lifecycle listener.
    class ScopedExpectedViolations
    {
      public:
        ScopedExpectedViolations();
        ~ScopedExpectedViolations();
        ScopedExpectedViolations(const ScopedExpectedViolations&) = delete;
        ScopedExpectedViolations& operator=(const ScopedExpectedViolations&) = delete;
        ScopedExpectedViolations(ScopedExpectedViolations&&) = delete;
        ScopedExpectedViolations& operator=(ScopedExpectedViolations&&) = delete;
    };

    // Violations raised while NO ScopedExpectedViolations was alive. The
    // suite listener compares this, not the total, so a negative control
    // declares exactly the violations it provokes and nothing else is waived.
    [[nodiscard]] u64 GetUnexpectedViolationCount();
} // namespace OloEngine::CommandLifecycle

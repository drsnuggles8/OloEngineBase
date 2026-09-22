#pragma once

// =============================================================================
// GPUTimingStatus.h — the validity vocabulary every GPU timing carries. #1337.
//
// A timing that could not be measured must say so. It must never arrive as
// 0.0, because a consumer cannot tell that apart from a pass that genuinely
// cost nothing — and every consumer downstream then reports the guess as fact.
// The checked-in parallel-recording study (docs/analysis/
// vulkan-parallel-recording-1013.md) records exactly this: "GPU timestamp
// samples were sometimes stale or zero; no GPU speedup is claimed."
//
// So the sentinel is gone. A measurement is a VALUE PLUS A STATUS, the status
// names which of the several distinct "no number" causes applied, and a
// consumer that wants the number has to look at the status to get it.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <string_view>

namespace OloEngine
{
    /// @brief Why a GPU timing does or does not carry a number.
    ///
    /// Ordered loosely from "fine" to "broken". Only `Valid` means the
    /// accompanying millisecond value describes real GPU work; every other
    /// value means the number is absent, and the enumerator says why so a
    /// reader can act on it instead of guessing.
    enum class GpuTimingStatus : u8
    {
        /// Measured. The value is this interval's GPU time, on the frame the
        /// record names. The only status whose number may be read.
        Valid = 0,

        /// Stamped, results not back from the device yet. The steady-state
        /// resolve latency is 1-3 frames, so this is the normal status for the
        /// first frames of a session and after a device reset — not a fault.
        Pending,

        /// The ring wrapped before the GPU finished this slot, so it was
        /// discarded to keep the pool non-blocking. Never measured, and never
        /// will be: the queries have been reissued for a later frame.
        ///
        /// NOT the same as "stale". A stale reading is a real measurement of an
        /// older frame and keeps its `Valid` status; how out of date it is, is
        /// reported by the snapshot's age instead. `Dropped` means there is no
        /// number, so a consumer waiting for one should stop waiting.
        Dropped,

        /// The backend refused or ignored the timestamp write, so the interval
        /// has no endpoints. Reached when a pass brackets from a parallel
        /// recording worker (Vulkan refuses those by ADR 0011 amendment (92)
        /// rule 7), outside a recording bracket, or on a retired query handle.
        NotStamped,

        /// Both endpoints read back, but the end stamp is BEFORE the begin
        /// stamp. Timestamps from two queue families do not share a timebase,
        /// so an async-compute bracket can close before it opened. There is no
        /// duration to derive — and it is emphatically not zero.
        ///
        /// A tick-IDENTICAL pair is not this. See the note on the zero case in
        /// ResolveGpuTimingPair: an empty bracket is a measurement of zero, not
        /// a fault, and calling it one sends a reader hunting for a queue bug
        /// that is not there.
        OutOfOrder,

        /// The interval ran but no bracket was allocated for it: the per-frame
        /// pass budget was already full. The work happened and is inside the
        /// frame total; it just has no attribution of its own.
        NotTimed,

        /// The timer pool is not running — the device declined timestamp
        /// queries, or GPU timing is switched off for this session. Nothing was
        /// measured and nothing will be until that changes.
        Unavailable,
    };

    /// @brief The stable wire/UI spelling of a status. Used by the MCP tools,
    /// the benchmark export and the editor panels so one vocabulary reaches
    /// every consumer.
    [[nodiscard]] constexpr std::string_view ToString(GpuTimingStatus status) noexcept
    {
        switch (status)
        {
            case GpuTimingStatus::Valid:
                return "valid";
            case GpuTimingStatus::Pending:
                return "pending";
            case GpuTimingStatus::Dropped:
                return "dropped";
            case GpuTimingStatus::NotStamped:
                return "notStamped";
            case GpuTimingStatus::OutOfOrder:
                return "outOfOrder";
            case GpuTimingStatus::NotTimed:
                return "notTimed";
            case GpuTimingStatus::Unavailable:
                return "unavailable";
        }
        return "unavailable";
    }

    /// @brief A one-line reason a reader can show verbatim. Deliberately says
    /// what it means for the NUMBER, not what happened in the driver.
    [[nodiscard]] constexpr std::string_view DescribeGpuTimingStatus(GpuTimingStatus status) noexcept
    {
        switch (status)
        {
            case GpuTimingStatus::Valid:
                return "measured";
            case GpuTimingStatus::Pending:
                return "results have not come back from the device yet";
            case GpuTimingStatus::Dropped:
                return "the query ring wrapped before the GPU finished; never measured";
            case GpuTimingStatus::NotStamped:
                return "the backend did not record the timestamp pair";
            case GpuTimingStatus::OutOfOrder:
                return "the end stamp does not follow the begin stamp; no duration exists";
            case GpuTimingStatus::NotTimed:
                return "the per-frame pass budget was full, so this interval got no bracket";
            case GpuTimingStatus::Unavailable:
                return "GPU timing is not running on this device or session";
        }
        return "GPU timing is not running on this device or session";
    }

    /// @brief One GPU interval: a duration that exists only when `Status` says
    /// it does.
    ///
    /// `GpuMs` is deliberately not the whole answer. Callers check `IsValid()`
    /// first, or go through `ValueOr()` and supply the substitute themselves so
    /// the choice is visible in the source rather than baked into the producer.
    /// A raw accessor exists for the serializers that must emit both fields,
    /// and it is named so that reading it without the status looks wrong at the
    /// call site.
    struct GpuTimingSample
    {
        f64 GpuMs = 0.0;
        GpuTimingStatus Status = GpuTimingStatus::Unavailable;

        [[nodiscard]] constexpr bool IsValid() const noexcept
        {
            return Status == GpuTimingStatus::Valid;
        }

        /// @brief The measurement, or nothing. The only sanctioned read.
        [[nodiscard]] constexpr f64 ValueOr(f64 fallback) const noexcept
        {
            return IsValid() ? GpuMs : fallback;
        }

        /// @brief The raw number regardless of status — for serializers that
        /// emit the status beside it. Never for arithmetic.
        [[nodiscard]] constexpr f64 RawMsForSerialization() const noexcept
        {
            return GpuMs;
        }

        [[nodiscard]] static constexpr GpuTimingSample Measured(f64 ms) noexcept
        {
            return GpuTimingSample{ ms, GpuTimingStatus::Valid };
        }

        [[nodiscard]] static constexpr GpuTimingSample Absent(GpuTimingStatus why) noexcept
        {
            return GpuTimingSample{ 0.0, why };
        }

        [[nodiscard]] auto operator==(const GpuTimingSample&) const -> bool = default;
    };

    /// @brief Everything known about one begin/end timestamp pair, as plain
    /// data. The inputs to the validity decision, with no renderer in them.
    struct GpuTimingPairReadout
    {
        /// Did the backend accept the begin/end timestamp writes? A refusal is
        /// routine under parallel recording and leaves the query holding
        /// whatever it held before — zero, or a stamp from four frames ago.
        bool BeginStamped = false;
        bool EndStamped = false;

        /// Did the result actually come back? Distinct from `Stamped`: a stamp
        /// the backend took may still be in flight, and a stamp on another
        /// queue is not ordered against the frame's own end stamp.
        bool BeginReadable = false;
        bool EndReadable = false;

        /// The readings, nanoseconds, meaningful only where Readable.
        u64 BeginNs = 0;
        u64 EndNs = 0;
    };

    /// @brief The whole validity decision for one timestamp pair, as a pure
    /// function of what was observed. #1337.
    ///
    /// Free-standing and engine-free on purpose: this is the logic that used to
    /// be a `cond ? value : 0.0` buried in the resolve loop, and it is where
    /// every silent zero came from. Out here it unit-tests exhaustively against
    /// synthetic readouts — including the ones no GPU will reliably produce on
    /// demand, like a backwards pair — instead of only on hardware that happens
    /// to misbehave.
    ///
    /// The order of the checks is the order of the causes, most specific first:
    /// a pair that was never stamped is not "pending", and a pair that came
    /// back backwards is not "zero".
    [[nodiscard]] constexpr GpuTimingSample ResolveGpuTimingPair(const GpuTimingPairReadout& readout) noexcept
    {
        if (!readout.BeginStamped || !readout.EndStamped)
        {
            return GpuTimingSample::Absent(GpuTimingStatus::NotStamped);
        }
        if (!readout.BeginReadable || !readout.EndReadable)
        {
            return GpuTimingSample::Absent(GpuTimingStatus::Pending);
        }
        if (readout.EndNs < readout.BeginNs)
        {
            // The bracket closed BEFORE it opened. Two queue families share no
            // timebase, so this is what an async-compute pass looks like when
            // its stamps are compared against the graphics queue's. There is no
            // duration in it.
            return GpuTimingSample::Absent(GpuTimingStatus::OutOfOrder);
        }
        // EQUAL stamps are a MEASUREMENT OF ZERO, not a failure — and getting
        // this wrong is easy, because #1337 is otherwise entirely about zeros
        // being lies. The distinction is where the zero comes from. A pass that
        // issued no GPU commands (no skeletal meshes in the scene, no fluid)
        // legitimately opens and closes its bracket on the same GPU tick, and
        // reporting that as a fault would (a) send a reader hunting a queue bug
        // that is not there, and (b) mark the frame's pass total incomplete on
        // almost every real frame, which would make `passGpuTotalIsComplete`
        // and `unattributedGpuMs` useless. Measured on an NVIDIA GL path where
        // the tick is ~1024 ns: 2 of 17 passes per frame land here.
        //
        // The zeros this issue exists to delete are the ones with no
        // measurement behind them, and those are already gone above: NotStamped
        // (the backend refused), Pending (nothing came back), Dropped (the ring
        // wrapped), Unavailable (no instrument). A sub-tick zero is none of
        // them, and it is distinguishable from all of them by its status.
        return GpuTimingSample::Measured(static_cast<f64>(readout.EndNs - readout.BeginNs) / 1'000'000.0);
    }
} // namespace OloEngine

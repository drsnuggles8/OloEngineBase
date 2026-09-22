#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Debug/GPUTimingStatus.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"

#include <array>
#include <string>
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/String.h"

namespace OloEngine
{
    /// @brief One timed interval of a GPUPassTimerPool resolved frame. Lives at
    /// namespace scope so the relocation trait below can name it; the pool
    /// re-exports it as GPUPassTimerPool::PassTiming.
    struct GPUPassTiming
    {
        FString Name;

        /// The measurement and its validity. `Sample.GpuMs` is meaningful
        /// only when `Sample.IsValid()`.
        GpuTimingSample Sample{};

        /// True for a bracket opened with BeginSubPass: its interval sits
        /// INSIDE its parent's and is published as "<Parent>/<name>".
        ///
        /// Carried as a flag rather than re-derived from a '/' in the name,
        /// which is what consumers used to do — a pass whose own name
        /// contains a slash would be misread as somebody's sub-pass, and
        /// its time then silently dropped from the frame total. Criterion 2
        /// of #1337 is precisely "duplicate/nested intervals are not summed
        /// as elapsed frame time", so the nesting is a fact the producer
        /// states, not one the consumer infers.
        bool IsSubPass = false;

        /// The name of the enclosing pass when IsSubPass; empty otherwise.
        FString ParentName;

        [[nodiscard]] bool IsValid() const
        {
            return Sample.IsValid();
        }
    };

    // Names own independent string storage; the sample and the flag are scalars.
    template<>
    struct TIsTriviallyRelocatable<GPUPassTiming>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(GPUPassTiming::Name)> &&
                                      TIsTriviallyRelocatable_V<decltype(GPUPassTiming::Sample)> &&
                                      TIsTriviallyRelocatable_V<decltype(GPUPassTiming::IsSubPass)> &&
                                      TIsTriviallyRelocatable_V<decltype(GPUPassTiming::ParentName)>;
    };

    /// @brief Always-on GPU timing for the whole frame and each render-graph pass.
    ///
    /// Ring-buffered timestamp query pairs (RHI::QueryType::Timestamp through the
    /// RenderCommand facade — backend-neutral since #691): frame N stamps
    /// begin/end timestamps around the frame's render and around every executed
    /// pass; the results are resolved a few frames later via a non-blocking
    /// availability check, so the published numbers always describe the most
    /// recent fully-completed frame.
    ///
    /// Deliberately uses scope-free timestamps (WriteTimestamp) instead of
    /// TimeElapsed brackets: elapsed-time query scopes must not nest, and the
    /// frame-capture path (GPUTimerQueryPool) already owns per-draw TimeElapsed
    /// scopes *inside* the pass brackets this pool times. Timestamps coexist.
    ///
    /// **Every published number carries a GpuTimingStatus (#1337).** The pool
    /// used to publish 0.0 for a dropped slot, an unstamped bracket and a
    /// backwards timestamp pair alike, which is indistinguishable from a pass
    /// that cost nothing. It now tracks, per query, whether the backend
    /// accepted the stamp and whether the result came back, and reports the
    /// specific reason a number is missing. See GPUTimingStatus.h.
    class GPUPassTimerPool
    {
      public:
        /// @brief One timed interval of the resolved frame (see GPUPassTiming).
        using PassTiming = GPUPassTiming;

        /// @brief Everything the pool published for one resolved frame, as one
        /// internally consistent snapshot.
        ///
        /// The frame identity travels WITH the numbers. Reading the passes and
        /// the age through two separate calls let a consumer pair a pass list
        /// with an age from a different frame; one struct cannot.
        struct FrameTimings
        {
            /// The frame counter value these timings describe. 0 means nothing
            /// has ever resolved, in which case every sample is non-Valid.
            u64 FrameNumber = 0;

            /// The frame counter value when this snapshot was taken.
            u64 CurrentFrameNumber = 0;

            /// How many frames old the numbers are. 1-3 is the designed resolve
            /// latency; kSlotCount or more means the ring wrapped and nothing
            /// newer could be resolved.
            u64 AgeFrames = 0;

            /// Whole-frame GPU time (frame-begin to frame-end timestamp span).
            GpuTimingSample Frame{};

            /// Per-pass times in execution order, sub-pass entries included and
            /// flagged.
            TArray<PassTiming> Passes;

            /// Slots discarded since Initialize() because the GPU fell more
            /// than a ring behind. Non-zero means some frames were never
            /// measured at all — the published numbers skip them.
            u32 DroppedSlots = 0;

            /// Frames the backend declined to stamp. A separate count from
            /// DroppedSlots because they are separate faults: this one says the
            /// instrument is not working, that one says the GPU is behind.
            u32 UnstampedFrames = 0;

            /// True when the numbers are older than the ring can explain.
            [[nodiscard]] bool IsStale() const
            {
                return FrameNumber == 0 || AgeFrames >= kSlotCount;
            }
        };

        /// @brief The result of totalling a pass list, with its own holes named.
        ///
        /// A bare sum silently treats an unmeasured pass as a free one, so the
        /// count of what could not be added travels with the total.
        struct PassTotal
        {
            /// Sum of the VALID top-level intervals only.
            f64 GpuMs = 0.0;
            /// Top-level passes that contributed.
            u32 ValidPasses = 0;
            /// Top-level passes that carried no number, so are missing from
            /// GpuMs. Non-zero means the total is a LOWER BOUND.
            u32 UnmeasuredPasses = 0;
            /// Sub-pass intervals deliberately excluded: their time is already
            /// inside a parent's bracket and adding it would double-count.
            u32 ExcludedSubPasses = 0;

            /// @brief True when every top-level pass contributed, so the total
            /// is complete rather than a floor.
            [[nodiscard]] bool IsComplete() const
            {
                return UnmeasuredPasses == 0;
            }
        };

        // 4 slots: results are read back 1-3 frames after issue without ever
        // blocking; a slot still pending when its turn comes again (GPU >3
        // frames behind — pathological) is dropped instead of waited on. The
        // drop is COUNTED and the affected frame is reported as Dropped rather
        // than as a zero (#1337).
        // Public so callers (e.g. the MCP olo_perf_pass_timings staleness
        // flag) can statically pin their own "results are stale" threshold
        // to this ring size instead of duplicating the number.
        static constexpr u32 kSlotCount = 4;

        static GPUPassTimerPool& GetInstance();

        /// @brief Allocate query objects. Call once after the graphics device is live.
        void Initialize(u32 maxPassesPerFrame = 96);

        /// @brief Delete all query objects.
        void Shutdown();

        /// @brief Advance to the next ring slot, resolve any completed older
        /// slots (non-blocking), and stamp the frame-begin timestamp.
        void BeginFrame();

        /// @brief Stamp the frame-end timestamp and mark the slot for readback.
        void EndFrame();

        /// @brief Stamp the begin timestamp for the named pass. Top-level passes
        /// must not overlap; a Begin without a matching End is closed at EndFrame.
        void BeginPass(const std::string& name);

        /// @brief Stamp the end timestamp for the pass opened by BeginPass.
        void EndPass();

        /// @brief Stamp the begin timestamp for a sub-pass INSIDE the currently
        /// open pass (e.g. the ScenePass depth-prepass vs color split, #316).
        /// The sub-pass is published as "<ParentName>/<name>"; timestamps are
        /// scope-free so the pair coexists with (and overlaps) the parent's
        /// bracket. One level only: a sub-pass inside a sub-pass is dropped, as
        /// is a sub-pass with no pass open.
        void BeginSubPass(const std::string& name);

        /// @brief Stamp the end timestamp for the sub-pass opened by BeginSubPass.
        void EndSubPass();

        [[nodiscard]] bool IsInitialized() const
        {
            return m_Initialized;
        }

        /// @brief The most recently resolved frame's timings, frame identity and
        /// age, as one consistent snapshot. Returns a copy so callers reading
        /// via a main-thread marshal (e.g. the MCP diagnostics server) get a
        /// stable view.
        ///
        /// An uninitialized pool returns a snapshot whose samples are all
        /// `Unavailable` and whose pass list is empty — never a zero-filled one.
        [[nodiscard]] FrameTimings GetLastFrameTimings() const;

        /// @brief Sum the valid top-level intervals of a pass list, excluding
        /// a sub-pass whose parent is present (its time is inside the parent's
        /// bracket), counting an orphan sub-pass, and naming what could not
        /// be added.
        ///
        /// Free-standing and static so every consumer totals a pass list the
        /// same way: the MCP shaping (McpPassTimings.h) calls it rather than
        /// keeping a loop of its own, and the evidence tests call it directly.
        [[nodiscard]] static PassTotal SumTopLevel(const TArray<PassTiming>& passes);

        /// @brief Whole-frame GPU time of the most recently resolved frame, with
        /// its validity. Prefer GetLastFrameTimings() when the passes or the age
        /// are wanted too.
        [[nodiscard]] GpuTimingSample GetLastFrameGpuSample() const
        {
            return m_Initialized ? m_LastFrameSample : GpuTimingSample::Absent(GpuTimingStatus::Unavailable);
        }

        /// @brief Frame counter value of the most recently resolved frame (0 when
        /// nothing has resolved yet). Compare against GetCurrentFrameNumber() to
        /// see how many frames the published results lag.
        [[nodiscard]] u64 GetLastResolvedFrameNumber() const
        {
            return m_LastResolvedFrame;
        }

        [[nodiscard]] u64 GetCurrentFrameNumber() const
        {
            return m_FrameCounter;
        }

        /// @brief How many slots have been discarded because the GPU fell more
        /// than a ring behind. Each one is a frame that was never measured.
        [[nodiscard]] u32 GetDroppedSlotCount() const
        {
            return m_DroppedSlots;
        }

        /// @brief How many frames the backend declined to stamp at all, which
        /// is a different fault from a ring wrap and deliberately a different
        /// counter: a device that refuses timestamps would otherwise report a
        /// permanent, growing GPU backlog that never happened.
        [[nodiscard]] u32 GetUnstampedFrameCount() const
        {
            return m_UnstampedFrames;
        }

      private:
        GPUPassTimerPool() = default;
        ~GPUPassTimerPool();
        GPUPassTimerPool(const GPUPassTimerPool&) = delete;
        GPUPassTimerPool& operator=(const GPUPassTimerPool&) = delete;

        struct FrameSlot
        {
            // [0] = frame begin, [1] = frame end, then per-pass begin/end pairs
            // at [2 + 2*i] / [3 + 2*i]. Pass and sub-pass brackets share the
            // pair array: each Begin{Sub,}Pass allocates the next pair, so a
            // sub-pass pair sits between its parent's begin and end stamps
            // (parent-first allocation order is what the MCP shaping relies on
            // to attach "Parent/Sub" entries to their parent).
            TArray<RHI::ResourceHandle> Queries;

            // Per-query record of whether the BACKEND accepted the stamp, as
            // reported by RenderCommand::WriteTimestamp. Without it a query
            // that was never stamped reads back as 0 and subtracts to a
            // perfectly plausible 0.0 ms — the #1337 defect. GL in particular
            // reports a never-used query object's result as AVAILABLE with
            // value 0, so availability alone cannot stand in for this.
            // u8 rather than bool so the storage is addressable per element.
            TArray<u8> Stamped;

            TArray<FString> PassNames;
            TArray<u8> PassIsSubPass;
            TArray<FString> PassParentNames;
            u32 PassCount = 0;
            u64 FrameNumber = 0;
            bool Pending = false; // stamped and awaiting readback
            // Passes that executed while the per-frame pair budget was already
            // full. They are published as NotTimed rather than omitted, so a
            // reader sees that the pass list is short of the graph.
            u32 UntimedPasses = 0;
        };

        // Reads the slot's results if the GPU has finished them (checked via the
        // frame-end query — the last one stamped) and publishes them. When
        // `dropIfUnavailable` is set the slot is cleared even if unresolvable,
        // and the drop is counted and published rather than left as silence.
        void TryResolveSlot(FrameSlot& slot, bool dropIfUnavailable);

        // Marks every query pair in `slot` unstamped and resets its per-frame
        // bookkeeping. Called when a slot starts a new frame.
        void ResetSlotForFrame(FrameSlot& slot, u64 frameNumber);

        // Stamps `queryIndex` and records whether the backend accepted it.
        void StampQuery(FrameSlot& slot, u32 queryIndex);

        // Resolves one begin/end query pair into a sample. Probes availability
        // before reading, so it can never block the render thread.
        [[nodiscard]] GpuTimingSample ResolvePair(const FrameSlot& slot, u32 beginIndex, u32 endIndex) const;

        // Says once per session that a frame's timings were lost, and why. The
        // per-frame signal is GetDroppedSlotCount(), not the log.
        void WarnOnceAboutLostFrame(u64 frameNumber, const char* reason);

        std::array<FrameSlot, kSlotCount> m_Slots;
        u32 m_WriteSlot = 0;
        u32 m_MaxPasses = 0;
        u64 m_FrameCounter = 0;
        bool m_Initialized = false;
        bool m_Active = false;      // between BeginFrame/EndFrame
        bool m_PassOpen = false;    // between BeginPass/EndPass
        bool m_SubPassOpen = false; // between BeginSubPass/EndSubPass
        u32 m_CurrentPassIndex = 0; // pair index of the open pass
        u32 m_CurrentSubPassIndex = 0;

        // Published results (most recently resolved frame).
        TArray<PassTiming> m_LastPassTimings;
        GpuTimingSample m_LastFrameSample{};
        u64 m_LastResolvedFrame = 0;
        u32 m_DroppedSlots = 0;    // the ring wrapped on an unfinished slot
        u32 m_UnstampedFrames = 0; // the backend never recorded the frame-end stamp
        // Rate-limits the drop warning: a GPU this far behind drops every
        // frame, and one line per frame is 60 lines a second.
        bool m_DropWarningIssued = false;
    };
} // namespace OloEngine

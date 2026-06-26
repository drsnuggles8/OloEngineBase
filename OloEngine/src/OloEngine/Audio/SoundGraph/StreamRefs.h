#pragma once

#include "OloEngine/Core/Base.h"

#include <type_traits>
#include <vector>

namespace OloEngine::Audio::SoundGraph
{
    //==============================================================================
    // Phase 2 typed connections (docs/soundgraph-metasounds-refactor.md).
    //
    // A connection is a raw pointer from a consumer's input ref into a producer's
    // output storage, patched once at wire time. The audio thread reads through the
    // pointer with no hash lookups, no virtual dispatch, and no choc::value
    // indirection — that is what lets SoundGraph::Process call each node once per
    // block instead of once per sample.
    //
    // Pointer stability contract: producers' output storage must never move after
    // wiring. NodeProcessor is non-copyable/non-movable and AudioBuffer allocates
    // its block storage once at construction (fixed capacity, never resized), so
    // every pointer captured by a ref stays valid for the lifetime of the graph.
    //==============================================================================

    /// Hard upper bound on frames per Process(numFrames) chunk. SoundGraph splits
    /// larger requests into chunks of this size, so per-output buffers can be
    /// allocated once at construction and never resized (pointer stability).
    inline constexpr u32 kMaxAudioBlockFrames = 4096;

    /// Stream/endpoint type tag used to validate connections at wire time.
    enum class EStreamType : u8
    {
        Audio, // block-rate f32 buffer (kMaxAudioBlockFrames capacity)
        Float, // control-rate f32 scalar
        Int32,
        Int64,
        Bool,
    };

    //==============================================================================
    /// Per-output audio block buffer. Fixed capacity (kMaxAudioBlockFrames) so Data()
    /// is stable for the node's lifetime — consumers capture the raw pointer at wire
    /// time. The producer writes the first numFrames samples each Process call.
    ///
    /// Storage modes (Phase 3 contiguous pool, docs/soundgraph-metasounds-refactor.md):
    ///   * Default — self-owned fallback vector. Used by graph ramp buffers, directly
    ///     constructed nodes (tests), and any buffer the graph chooses not to pool.
    ///   * Pooled — after AdoptPoolStorage(slot), Data() points into the graph's one
    ///     contiguous m_NodeOutputPool allocation and the fallback vector is released.
    /// Either way Data() returns a pointer that never moves for the buffer's lifetime.
    struct AudioBuffer
    {
        AudioBuffer() : m_Storage(kMaxAudioBlockFrames, 0.0f), m_Data(m_Storage.data()) {}

        AudioBuffer(const AudioBuffer&) = delete;
        AudioBuffer& operator=(const AudioBuffer&) = delete;
        AudioBuffer(AudioBuffer&&) = delete;
        AudioBuffer& operator=(AudioBuffer&&) = delete;

        [[nodiscard("buffer pointer must be used")]] f32* Data() noexcept
        {
            return m_Data;
        }
        [[nodiscard("buffer pointer must be used")]] const f32* Data() const noexcept
        {
            return m_Data;
        }

        /// Repoint this buffer at graph-owned contiguous pool storage and release the
        /// self-owned fallback. `slot` must have >= kMaxAudioBlockFrames capacity and
        /// must outlive this buffer (the pool is sized once and never resized).
        ///
        /// CONTRACT: call exactly once, off the audio thread, BEFORE any consumer
        /// captures Data() — i.e. before graph wiring. Repointing after a consumer has
        /// cached the old (fallback) pointer would dangle it. See SoundGraph::
        /// AllocateNodeOutputPool, which is the only caller and runs pre-wiring.
        void AdoptPoolStorage(f32* slot) noexcept
        {
            m_Data = slot;
            m_Storage = std::vector<f32>(); // release the fallback; m_Data now points at the pool
        }

      private:
        std::vector<f32> m_Storage; // self-owned fallback (empty once pooled)
        f32* m_Data;                // stable view: &m_Storage[0] or a pool slot
    };

    //==============================================================================
    /// Audio-rate input reference. Binds to either a producer's AudioBuffer
    /// (per-frame samples, stride 1) or a scalar f32 cell broadcast across the
    /// block (stride 0) — graph parameters, control-rate node outputs, and the
    /// unconnected default all use the scalar form. Sample() is branch-free.
    struct AudioBufferRef
    {
        AudioBufferRef() noexcept : m_Data(&m_Default) {}

        // Self-referential default pointer: copying would alias another ref's
        // default cell, so forbid it. Refs live as node members and are never
        // copied after construction.
        AudioBufferRef(const AudioBufferRef&) = delete;
        AudioBufferRef& operator=(const AudioBufferRef&) = delete;
        AudioBufferRef(AudioBufferRef&&) = delete;
        AudioBufferRef& operator=(AudioBufferRef&&) = delete;

        [[nodiscard("sampled value must be used")]] f32 Sample(u32 frame) const noexcept
        {
            return m_Data[frame * m_Stride];
        }
        [[nodiscard("query result must be used")]] bool IsBuffer() const noexcept
        {
            return m_Stride == 1;
        }

        void BindBuffer(const f32* blockData) noexcept
        {
            m_Data = blockData;
            m_Stride = 1;
        }
        void BindScalar(const f32* cell) noexcept
        {
            m_Data = cell;
            m_Stride = 0;
        }
        void SetDefault(f32 value) noexcept
        {
            m_Default = value;
        }

        const f32* m_Data;
        u32 m_Stride = 0;
        f32 m_Default = 0.0f;
    };

    //==============================================================================
    /// Concept gate for control-rate stream value types — the primitives a typed
    /// connection can carry.
    template<typename T>
    concept StreamValue = std::is_same_v<T, f32> || std::is_same_v<T, i32> ||
                          std::is_same_v<T, i64> || std::is_same_v<T, bool>;

    /// Control-rate input reference: `const T*` into a producer's scalar output,
    /// a graph parameter cell, or (unconnected) its own inline default. Always
    /// valid to read — there is no null state.
    template<StreamValue T>
    struct ValueRef
    {
        ValueRef() noexcept : m_Ptr(&m_Default) {}

        ValueRef(const ValueRef&) = delete;
        ValueRef& operator=(const ValueRef&) = delete;
        ValueRef(ValueRef&&) = delete;
        ValueRef& operator=(ValueRef&&) = delete;

        [[nodiscard("stream value must be used")]] T Get() const noexcept
        {
            return *m_Ptr;
        }

        void Bind(const T* cell) noexcept
        {
            m_Ptr = cell;
        }
        void SetDefault(T value) noexcept
        {
            m_Default = value;
        }

        const T* m_Ptr;
        T m_Default{};
    };

    using FloatRef = ValueRef<f32>;
    using IntRef = ValueRef<i32>;
    using Int64Ref = ValueRef<i64>;
    using BoolRef = ValueRef<bool>;

    /// Maps a stream value type to its EStreamType tag.
    template<StreamValue T>
    inline constexpr EStreamType StreamTypeFor = std::is_same_v<T, f32>   ? EStreamType::Float
                                                 : std::is_same_v<T, i32> ? EStreamType::Int32
                                                 : std::is_same_v<T, i64> ? EStreamType::Int64
                                                                          : EStreamType::Bool;

    //==============================================================================
    /// Sample-accurate trigger (Phase 4 — docs/soundgraph-metasounds-refactor.md).
    ///
    /// Carries the frame offset within the current block at which a trigger fires.
    /// kNotFired (-1) means "did not fire this block". Trigger-consuming nodes
    /// (WavePlayer Play/Stop, envelope retrigger/release) read the offset in
    /// Process() and split their per-frame loop at that frame, so an event that
    /// arrives mid-block takes effect at the exact sample instead of being
    /// quantised to the block boundary.
    ///
    /// Single-fire-per-block model: a node holds one Trigger per trigger input.
    /// Fire() records the *earliest* pending offset for the block — multiple fires
    /// in one block collapse to the first, matching the old Flag/dirty semantics
    /// (N SetDirty()s = one action) while keeping the earliest moment requested.
    /// Genuine sub-block multi-fire needs the full event-queue scheduling that is
    /// out of Phase 4's scope.
    struct Trigger
    {
        static constexpr i32 kNotFired = -1;

        i32 m_SampleOffset = kNotFired; // [0, numFrames) when fired; kNotFired otherwise

        [[nodiscard("query result must be used")]] bool IsFired() const noexcept
        {
            return m_SampleOffset != kNotFired;
        }
        [[nodiscard("trigger offset must be used")]] i32 Offset() const noexcept
        {
            return m_SampleOffset;
        }

        /// Record a fire at `sampleOffset` (negative offsets — e.g. a legacy
        /// block-boundary event with no frame info — clamp to frame 0). If the
        /// trigger already fired earlier this block, keep the earlier offset so
        /// the node acts at the earliest requested frame (first-fire-wins).
        void Fire(i32 sampleOffset) noexcept
        {
            const i32 offset = sampleOffset < 0 ? 0 : sampleOffset;
            if (m_SampleOffset == kNotFired || offset < m_SampleOffset)
                m_SampleOffset = offset;
        }

        void Reset() noexcept
        {
            m_SampleOffset = kNotFired;
        }

        /// Read-and-clear: returns the pending offset (or kNotFired) and resets.
        [[nodiscard("Consume() read-clears the trigger; the returned offset must be used")]] i32 Consume() noexcept
        {
            const i32 offset = m_SampleOffset;
            m_SampleOffset = kNotFired;
            return offset;
        }
    };

    //==============================================================================
    /// Trigger input reference (Phase 4). Mirrors AudioBufferRef / ValueRef: it
    /// owns an inline default Trigger and points at it until Bind() repoints it at
    /// a producer's Trigger. Trigger-consuming nodes own a TriggerRef per trigger
    /// input; the node's InputEvent handler Fire()s it with the event's sample
    /// offset, and Process() Consume()s the offset to split its per-frame loop.
    ///
    /// Bind() is the seam for future typed trigger connections (the doc's "snapshot
    /// TriggerRef pointers"); nothing wires them yet, so today every node uses its
    /// inline default. On a bound ref, prefer the non-destructive IsFired()/Offset()
    /// reads (Consume() read-clears the shared producer trigger).
    struct TriggerRef
    {
        TriggerRef() noexcept : m_Trigger(&m_Default) {}

        // Self-referential default pointer: copying would alias another ref's
        // default cell, so forbid it (same contract as the other refs).
        TriggerRef(const TriggerRef&) = delete;
        TriggerRef& operator=(const TriggerRef&) = delete;
        TriggerRef(TriggerRef&&) = delete;
        TriggerRef& operator=(TriggerRef&&) = delete;

        void Fire(i32 sampleOffset) noexcept
        {
            m_Trigger->Fire(sampleOffset);
        }
        [[nodiscard("query result must be used")]] bool IsFired() const noexcept
        {
            return m_Trigger->IsFired();
        }
        [[nodiscard("trigger offset must be used")]] i32 Offset() const noexcept
        {
            return m_Trigger->Offset();
        }
        [[nodiscard("Consume() read-clears the trigger; the returned offset must be used")]] i32 Consume() noexcept
        {
            return m_Trigger->Consume();
        }

        void Bind(Trigger* producer) noexcept
        {
            m_Trigger = producer;
        }

        Trigger* m_Trigger;
        Trigger m_Default;
    };

} // namespace OloEngine::Audio::SoundGraph

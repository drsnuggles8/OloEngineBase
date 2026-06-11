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
    /// Owning per-output audio block buffer. Fixed capacity so Data() is stable for
    /// the node's lifetime (consumers capture the raw pointer at wire time). The
    /// producer writes the first numFrames samples each Process call.
    struct AudioBuffer
    {
        AudioBuffer() : m_Data(kMaxAudioBlockFrames, 0.0f) {}

        AudioBuffer(const AudioBuffer&) = delete;
        AudioBuffer& operator=(const AudioBuffer&) = delete;
        AudioBuffer(AudioBuffer&&) = delete;
        AudioBuffer& operator=(AudioBuffer&&) = delete;

        [[nodiscard]] f32* Data() noexcept { return m_Data.data(); }
        [[nodiscard]] const f32* Data() const noexcept { return m_Data.data(); }

      private:
        std::vector<f32> m_Data;
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

        [[nodiscard]] f32 Sample(u32 frame) const noexcept { return m_Data[frame * m_Stride]; }
        [[nodiscard]] bool IsBuffer() const noexcept { return m_Stride == 1; }

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
        void SetDefault(f32 value) noexcept { m_Default = value; }

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

        [[nodiscard]] T Get() const noexcept { return *m_Ptr; }

        void Bind(const T* cell) noexcept { m_Ptr = cell; }
        void SetDefault(T value) noexcept { m_Default = value; }

        const T* m_Ptr;
        T m_Default{};
    };

    using FloatRef = ValueRef<f32>;
    using IntRef = ValueRef<i32>;
    using Int64Ref = ValueRef<i64>;
    using BoolRef = ValueRef<bool>;

    /// Maps a stream value type to its EStreamType tag.
    template<StreamValue T>
    inline constexpr EStreamType StreamTypeFor = std::is_same_v<T, f32>  ? EStreamType::Float
                                                 : std::is_same_v<T, i32> ? EStreamType::Int32
                                                 : std::is_same_v<T, i64> ? EStreamType::Int64
                                                                          : EStreamType::Bool;

    //==============================================================================
    /// Placeholder for Phase 4 sample-accurate triggers. Until then trigger events
    /// fire at block boundaries through the InputEvent/OutputEvent system.
    struct TriggerRef
    {
        i32 m_SampleOffset = -1; // frame offset within the block; -1 = not fired
    };

} // namespace OloEngine::Audio::SoundGraph

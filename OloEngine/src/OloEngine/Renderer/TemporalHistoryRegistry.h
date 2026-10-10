#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Texture.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/String.h"

namespace OloEngine
{
    enum class TemporalHistoryEffect : u8
    {
        TAA,
        SSGI,
        SSR,
        Cloudscape,
        RayTracedShadow, ///< Hybrid ray-traced shadow visibility mask (issue #1056)
        PathTracer,      ///< GPU reference path tracer's progressive accumulation (issue #1055)
        ReSTIRDI,        ///< ReSTIR DI screen-space reservoirs (issue #1140)
        ReSTIRGI,        ///< ReSTIR GI screen-space reservoirs (issue #1169)
        // ReSTIR PT's path records live in the pass's own buffers; the registry
        // holds their LINEAGE only (an external entry, #1348), so a camera cut
        // or a scene reset reaches them like every other history.
        ReSTIRPT,
    };

    enum class TemporalHistoryPlane : u8
    {
        Signal,
        SurfaceDepth,
        SurfaceGeometry,
        SurfaceIdentity,
        MomentsFirst,
        MomentsSecond,
        Diagnostics,
        Albedo, ///< First-hit albedo AOV accumulation (issue #1055)
        // The three planes one packed ReSTIR DI reservoir occupies (issue
        // #1140). Named rather than folded into Signal/Diagnostics because
        // the LayoutVersion on their descriptor is what stops a reservoir
        // written by an older packing from being reinterpreted as a newer
        // one — which is not a crash, it is a plausible wrong image.
        ReservoirSample,   ///< xyz = emitter point / direction, w = packed kind + light index
        ReservoirRadiance, ///< xyz = emitter radiance, w = oct-packed emitter normal
        ReservoirState,    ///< x = W, y = M, z = target pdf, w = packed diagnostics
    };

    enum class TemporalHistoryResolution : u8
    {
        Display,
        Scene,
        Half,
        Quarter,
    };

    enum class TemporalHistoryBackend : u8
    {
        Unknown,
        OpenGL,
        Vulkan,
    };

    struct TemporalHistoryKey
    {
        TemporalHistoryEffect Effect = TemporalHistoryEffect::TAA;
        u64 View = 0;
        TemporalHistoryResolution Resolution = TemporalHistoryResolution::Scene;
        TemporalHistoryPlane Plane = TemporalHistoryPlane::Signal;

        auto operator==(const TemporalHistoryKey&) const -> bool = default;
    };

    struct TemporalHistoryKeyHash
    {
        [[nodiscard]] std::size_t operator()(const TemporalHistoryKey& key) const noexcept;
    };

    struct TemporalHistoryDescriptor
    {
        u32 Width = 0;
        u32 Height = 0;
        ImageFormat Format = ImageFormat::None;
        u32 MipLevels = 1;
        u32 Samples = 1;
        u32 LayoutVersion = 1;
        TemporalHistoryBackend Backend = TemporalHistoryBackend::Unknown;

        [[nodiscard]] bool IsUsable() const
        {
            return Width > 0 && Height > 0 && Format != ImageFormat::None && Samples > 0 && LayoutVersion > 0;
        }

        auto operator==(const TemporalHistoryDescriptor&) const -> bool = default;
    };

    enum class TemporalHistoryDependency : u32
    {
        None = 0,
        ViewTransform = 1u << 0u,
        Projection = 1u << 1u,
        Viewport = 1u << 2u,
        RenderScale = 1u << 3u,
        Scene = 1u << 4u,
        Backend = 1u << 5u,
        FeatureState = 1u << 6u,
        Jitter = 1u << 7u,
        // The scene's CONTENT changed under a still camera: a GPU Scene record
        // (an instance transform, a material factor, a light, the environment)
        // committed with different bytes. Declared only by a history that
        // cannot reproject (the path tracer's accumulation); a reprojecting
        // history survives a moving object by design and does not declare it.
        SceneContent = 1u << 8u,
    };

    [[nodiscard]] constexpr TemporalHistoryDependency operator|(
        TemporalHistoryDependency lhs, TemporalHistoryDependency rhs)
    {
        return static_cast<TemporalHistoryDependency>(std::to_underlying(lhs) | std::to_underlying(rhs));
    }

    [[nodiscard]] constexpr TemporalHistoryDependency operator&(
        TemporalHistoryDependency lhs, TemporalHistoryDependency rhs)
    {
        return static_cast<TemporalHistoryDependency>(std::to_underlying(lhs) & std::to_underlying(rhs));
    }

    enum class TemporalHistoryInvalidationCause : u8
    {
        None,
        FirstUse,
        DescriptorChanged,
        CameraCut,
        ProjectionChanged,
        ViewportResized,
        DynamicResolutionChanged,
        SceneReset,
        FeatureToggled,
        BackendChanged,
        JitterReset,
        CopyFailed,
        Manual,
        // A record in the GPU Scene changed this frame (an instance moved, a
        // material factor was edited, a light changed) without the scene being
        // reloaded. Maps to the SceneContent dependency, which only a history
        // that cannot reproject declares — a reprojecting history survives a
        // moving object by design (issue #1055).
        SceneMutated,
        // Renderer3D::ResetFrameSequences restarted every sampling sequence (the
        // stochastic frame index, the jitter phase, the pass-local indices).
        // Every history was accumulated from the old sequence, whatever it
        // declares, so this reaches every entry, like Manual (#1348).
        SamplingSequenceReset,
    };

    struct TemporalHistoryToken
    {
        u32 Index = 0;
        u32 Generation = 0;

        [[nodiscard]] bool IsValid() const
        {
            return Generation != 0;
        }

        auto operator==(const TemporalHistoryToken&) const -> bool = default;
    };

    [[nodiscard]] constexpr u32 NextTemporalHistoryGeneration(u32 generation)
    {
        ++generation;
        return generation == 0 ? 1 : generation;
    }

    struct TemporalHistoryAcquireResult
    {
        TemporalHistoryToken Token{};
        bool Created = false;
        bool DescriptorChanged = false;
    };

    struct TemporalHistorySnapshot
    {
        TemporalHistoryKey Key{};
        TemporalHistoryDescriptor Descriptor{};
        TemporalHistoryToken Token{};
        TemporalHistoryDependency Dependencies = TemporalHistoryDependency::None;
        TemporalHistoryInvalidationCause LastInvalidation = TemporalHistoryInvalidationCause::None;
        // The lineage AOV (#1348). Age counts the frames the current history
        // has accumulated: 1 on the frame it was produced with nothing to read,
        // +1 on every later frame that read it and wrote it back. LineageCause
        // is what started that lineage and survives MarkProduced, where
        // LastInvalidation is cleared. A reset policy is read off these two: a
        // camera cut that kept Age climbing kept a stale history.
        u32 Age = 0;
        TemporalHistoryInvalidationCause LineageCause = TemporalHistoryInvalidationCause::FirstUse;
        bool Valid = false;
        bool HasTexture = false;
        FString DebugName;
    };

    struct TemporalHistoryEntry
    {
        TemporalHistoryKey Key{};
        TemporalHistoryDescriptor Descriptor{};
        TemporalHistoryDependency Dependencies = TemporalHistoryDependency::None;
        TemporalHistoryInvalidationCause LastInvalidation = TemporalHistoryInvalidationCause::FirstUse;
        // A break waiting for the next MarkProduced to start a new lineage
        // with; None while the lineage continues.
        TemporalHistoryInvalidationCause PendingLineageBreak = TemporalHistoryInvalidationCause::FirstUse;
        TemporalHistoryInvalidationCause LineageCause = TemporalHistoryInvalidationCause::FirstUse;
        u32 Generation = 1;
        u32 Age = 0;
        bool Valid = false;
        // Whether the history was valid when this frame's extraction began,
        // that is whether the frame read it. MarkCopyFailed latches it.
        bool ValidAtExtraction = false;
        // A lineage whose storage a pass owns (AcquireExternal): no texture,
        // never imported, so it stays out of the validity key.
        bool External = false;
        Ref<Texture2D> Texture;
        FString DebugName;
    };

    // Descriptor/key are scalar value records; Ref and FString own external storage without self-pointers.
    template<>
    struct TIsTriviallyRelocatable<TemporalHistoryEntry>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(TemporalHistoryEntry::Key)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TemporalHistoryEntry::Descriptor)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TemporalHistoryEntry::Dependencies)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TemporalHistoryEntry::LastInvalidation)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TemporalHistoryEntry::PendingLineageBreak)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TemporalHistoryEntry::LineageCause)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TemporalHistoryEntry::Generation)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TemporalHistoryEntry::Age)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TemporalHistoryEntry::Valid)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TemporalHistoryEntry::ValidAtExtraction)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TemporalHistoryEntry::External)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TemporalHistoryEntry::Texture)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TemporalHistoryEntry::DebugName)>::Value;
    };

    // Same scalar metadata as Entry with an owned string and no texture reference.
    template<>
    struct TIsTriviallyRelocatable<TemporalHistorySnapshot>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(TemporalHistorySnapshot::Key)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TemporalHistorySnapshot::Descriptor)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TemporalHistorySnapshot::Token)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TemporalHistorySnapshot::Dependencies)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TemporalHistorySnapshot::LastInvalidation)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TemporalHistorySnapshot::Age)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TemporalHistorySnapshot::LineageCause)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TemporalHistorySnapshot::Valid)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TemporalHistorySnapshot::HasTexture)>::Value &&
                                      TIsTriviallyRelocatable<decltype(TemporalHistorySnapshot::DebugName)>::Value;
    };

    // Persistent, graph-owned temporal state. RenderGraph handles are deliberately
    // absent: transient resources may feed an extraction, but can never become the
    // next frame's backing store through alias reuse.
    class TemporalHistoryRegistry
    {
      public:
        [[nodiscard]] TemporalHistoryAcquireResult Acquire(
            const TemporalHistoryKey& key,
            const TemporalHistoryDescriptor& descriptor,
            TemporalHistoryDependency dependencies,
            std::string debugName = {});

        // A history whose storage a pass keeps itself (#1348: ReSTIR PT's path
        // records): the registry owns its validity and lineage, nothing else.
        // MarkProduced accepts it without a texture; ComputeValidityKey skips it.
        [[nodiscard]] TemporalHistoryAcquireResult AcquireExternal(
            const TemporalHistoryKey& key,
            const TemporalHistoryDescriptor& descriptor,
            TemporalHistoryDependency dependencies,
            std::string debugName = {});

        [[nodiscard]] bool IsCurrent(TemporalHistoryToken token) const;
        [[nodiscard]] bool IsValid(TemporalHistoryToken token) const;
        [[nodiscard]] TemporalHistoryToken Find(const TemporalHistoryKey& key) const;
        // The same slot at its CURRENT generation, or an invalid token when the
        // slot no longer exists. For a holder that latched a token when a
        // history was acquired and must follow later invalidations, which bump
        // the generation without changing the texture.
        [[nodiscard]] TemporalHistoryToken Current(TemporalHistoryToken token) const;
        [[nodiscard]] const TemporalHistoryDescriptor* GetDescriptor(TemporalHistoryToken token) const;
        [[nodiscard]] std::string_view GetDebugName(TemporalHistoryToken token) const;
        [[nodiscard]] Ref<Texture2D> GetTexture(TemporalHistoryToken token) const;
        bool SetTexture(TemporalHistoryToken token, Ref<Texture2D> texture);
        bool MarkProduced(TemporalHistoryToken token);
        bool MarkCopyFailed(TemporalHistoryToken token);

        u32 Invalidate(TemporalHistoryInvalidationCause cause,
                       std::optional<TemporalHistoryEffect> effect = std::nullopt);
        // Drop one history's texture as well as its validity: for an effect
        // that stopped declaring its history, so it neither holds the memory
        // nor resumes the old lineage when it comes back. Unlike Invalidate it
        // ignores the stale-history fault, which is about reset policy, not
        // storage. Returns false when there was nothing to release.
        bool Release(const TemporalHistoryKey& key, TemporalHistoryInvalidationCause cause);
        // Whether any history of `effect` holds a texture or a valid frame:
        // the cheap test before a release that would otherwise snapshot.
        [[nodiscard]] bool HoldsAny(TemporalHistoryEffect effect) const;
        void Clear();

        [[nodiscard]] TArray<TemporalHistorySnapshot> Snapshot() const;

        // Which histories exist and which of them hold a usable previous frame,
        // as one key (issue #1333). That is what decides whether a history is
        // IMPORTED and read, so it is a render-graph declaration input. The
        // generation is deliberately left out: Invalidate() bumps it on every
        // matching entry, already-invalid ones included, so hashing it would
        // rebuild the frame graph on every frame an object or the camera moves
        // while declaring nothing different.
        [[nodiscard]] u64 ComputeValidityKey() const;
        [[nodiscard]] static TemporalHistoryDependency DependencyForCause(TemporalHistoryInvalidationCause cause);

      private:
        using Entry = TemporalHistoryEntry;

        [[nodiscard]] Entry* Resolve(TemporalHistoryToken token);
        [[nodiscard]] const Entry* Resolve(TemporalHistoryToken token) const;

        std::unordered_map<TemporalHistoryKey, u32, TemporalHistoryKeyHash> m_Indices;
        std::unordered_map<std::string, TemporalHistoryKey> m_DebugNameOwners;
        TArray<Entry> m_Entries;
    };
} // namespace OloEngine

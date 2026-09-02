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
#include <vector>

namespace OloEngine
{
    enum class TemporalHistoryEffect : u8
    {
        TAA,
        SSGI,
        SSR,
        Cloudscape,
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
        bool Valid = false;
        bool HasTexture = false;
        std::string DebugName;
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

        [[nodiscard]] bool IsCurrent(TemporalHistoryToken token) const;
        [[nodiscard]] bool IsValid(TemporalHistoryToken token) const;
        [[nodiscard]] TemporalHistoryToken Find(const TemporalHistoryKey& key) const;
        [[nodiscard]] const TemporalHistoryDescriptor* GetDescriptor(TemporalHistoryToken token) const;
        [[nodiscard]] std::string_view GetDebugName(TemporalHistoryToken token) const;
        [[nodiscard]] Ref<Texture2D> GetTexture(TemporalHistoryToken token) const;
        bool SetTexture(TemporalHistoryToken token, Ref<Texture2D> texture);
        bool MarkProduced(TemporalHistoryToken token);
        bool MarkCopyFailed(TemporalHistoryToken token);

        u32 Invalidate(TemporalHistoryInvalidationCause cause,
                       std::optional<TemporalHistoryEffect> effect = std::nullopt);
        void Clear();

        [[nodiscard]] std::vector<TemporalHistorySnapshot> Snapshot() const;
        [[nodiscard]] static TemporalHistoryDependency DependencyForCause(TemporalHistoryInvalidationCause cause);

      private:
        struct Entry
        {
            TemporalHistoryKey Key{};
            TemporalHistoryDescriptor Descriptor{};
            TemporalHistoryDependency Dependencies = TemporalHistoryDependency::None;
            TemporalHistoryInvalidationCause LastInvalidation = TemporalHistoryInvalidationCause::FirstUse;
            u32 Generation = 1;
            bool Valid = false;
            Ref<Texture2D> Texture;
            std::string DebugName;
        };

        [[nodiscard]] Entry* Resolve(TemporalHistoryToken token);
        [[nodiscard]] const Entry* Resolve(TemporalHistoryToken token) const;

        std::unordered_map<TemporalHistoryKey, u32, TemporalHistoryKeyHash> m_Indices;
        std::vector<Entry> m_Entries;
    };
} // namespace OloEngine

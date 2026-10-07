#pragma once

#include "OloEngine/Asset/AssetSystem/RepresentationStreaming.h"
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/String.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Vertex.h"

#include <filesystem>

namespace OloEngine
{
    struct FFoliageStreamingPart
    {
        u32 BaseIndex = 0;
        u32 IndexCount = 0;
        FString AlbedoPath;
        u64 AlbedoGpuBytes = 0;
        u64 AlbedoUploadBytes = 0;
        u64 AlbedoSourceBytes = 0;
        u64 AlbedoDecodeBytes = 0;
        u32 AlbedoWidth = 0;
        u32 AlbedoHeight = 0;
    };

    template<>
    struct TIsTriviallyRelocatable<FFoliageStreamingPart>
    {
        static constexpr bool Value = TIsTriviallyRelocatable<decltype(FFoliageStreamingPart::BaseIndex)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FFoliageStreamingPart::IndexCount)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FFoliageStreamingPart::AlbedoPath)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FFoliageStreamingPart::AlbedoGpuBytes)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FFoliageStreamingPart::AlbedoUploadBytes)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FFoliageStreamingPart::AlbedoSourceBytes)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FFoliageStreamingPart::AlbedoDecodeBytes)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FFoliageStreamingPart::AlbedoWidth)>::Value &&
                                      TIsTriviallyRelocatable<decltype(FFoliageStreamingPart::AlbedoHeight)>::Value;
    };

    // Worker-owned preparation. No Model, MeshSource, Texture or GPU object may
    // be constructed here: their constructors can upload on the calling thread.
    class FFoliageStreamingPayload final : public FRepresentationPayload
    {
      public:
        TArray<Vertex> Vertices;
        TArray<u32> Indices;
        TArray<FFoliageStreamingPart> Parts;
        BoundingBox Bounds;
        FRepresentationIOStats IOStats;

        [[nodiscard]] u64 GetCpuBytes() const noexcept override;
        [[nodiscard]] std::optional<FRepresentationIOStats> GetIOStats() const noexcept override
        {
            return IOStats;
        }
        [[nodiscard]] u64 GetGpuBytes() const noexcept;
        [[nodiscard]] u64 GetUploadBytes(f32 alphaCutoff = 0.0f) const noexcept;
    };

    // The decoder reserves its working set before reading or allocating image
    // bytes. Its ticket stays charged until the consuming upload drops it.
    class FFoliageTextureStaging final : public RefCounted
    {
      public:
        ~FFoliageTextureStaging() override;
        [[nodiscard]] static Ref<FFoliageTextureStaging> Reserve(u64 bytes);
        [[nodiscard]] u64 Capacity() const
        {
            return m_Capacity;
        }

      private:
        FFoliageTextureStaging(u64 capacity, u64 ticket) : m_Capacity(capacity), m_Ticket(ticket) {}
        u64 m_Capacity = 0;
        u64 m_Ticket = 0;
    };

    class FFoliageStreamingImage final : public FRepresentationPayload
    {
      public:
        ~FFoliageStreamingImage() override;
        TArray<u8> Pixels;
        u32 Width = 0;
        u32 Height = 0;
        Ref<FFoliageTextureStaging> Staging;
        [[nodiscard]] u8 AlphaAt(glm::vec2 uv) const noexcept;
        [[nodiscard]] u64 GetCpuBytes() const noexcept override
        {
            return sizeof(*this) + Pixels.GetAllocatedSize();
        }
    };

    [[nodiscard]] Ref<FFoliageStreamingImage> DecodeFoliageStreamingAlbedo(const FFoliageStreamingPart& part,
                                                                           Ref<FFoliageTextureStaging> staging = {});

    [[nodiscard]] Ref<FFoliageStreamingPayload> LoadFoliageStreamingPayload(const std::filesystem::path& absolutePath);
} // namespace OloEngine

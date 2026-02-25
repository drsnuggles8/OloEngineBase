#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/RendererResource.h"

#include <string>
#include <vector>

namespace OloEngine
{
    enum class ImageFormat
    {
        None = 0,
        R8,
        RGB8,
        RGBA8,
        RGBA32F, // Unsupported
        R32F,    // Unsupported
        RG32F,   // Unsupported
        RGB32F,  // Unsupported
        DEPTH24STENCIL8
    };

    struct TextureSpecification
    {
        u32 Width = 1;
        u32 Height = 1;
        ImageFormat Format = ImageFormat::RGBA8;
        bool GenerateMips = true;
    };

    class Texture : public RendererResource
    {
      public:
        virtual ~Texture() = default;

        virtual const TextureSpecification& GetSpecification() const = 0;

        [[nodiscard("Store this!")]] virtual u32 GetWidth() const = 0;
        [[nodiscard("Store this!")]] virtual u32 GetHeight() const = 0;
        [[nodiscard("Store this!")]] virtual u32 GetRendererID() const = 0;
        [[nodiscard("Store this!")]] virtual const std::string& GetPath() const = 0;

        virtual void SetData(void* data, u32 size) = 0;
        virtual void Invalidate(std::string_view path, u32 width, u32 height, const void* data, u32 channels) = 0;

        virtual void Bind(u32 slot) const = 0;

        [[nodiscard("Store this!")]] virtual bool IsLoaded() const = 0;

        [[nodiscard("Use for transparency")]] virtual bool HasAlphaChannel() const = 0;

        /**
         * @brief Read texture data back from GPU
         *
         * @param outData Vector to receive the texture data
         * @param mipLevel Mipmap level to read (0 = base level)
         * @return true if readback succeeded
         */
        virtual bool GetData(std::vector<u8>& outData, u32 mipLevel = 0) const = 0;

        bool operator==(const Texture& other) const
        {
            return GetRendererID() == other.GetRendererID();
        }

        // Asset interface
        static constexpr AssetType GetStaticType() noexcept
        {
            return AssetType::None;
        }
        virtual AssetType GetAssetType() const override = 0;
    };

    class Texture2D : public Texture
    {
      public:
        virtual void SubImage(u32 x, u32 y, u32 width, u32 height, const void* data, u32 dataSize) = 0;

        static Ref<Texture2D> Create(const TextureSpecification& specification);
        static Ref<Texture2D> Create(const std::string& path);

        // Asset interface
        static constexpr AssetType GetStaticType() noexcept
        {
            return AssetType::Texture2D;
        }
        virtual AssetType GetAssetType() const override
        {
            return GetStaticType();
        }
    };
} // namespace OloEngine

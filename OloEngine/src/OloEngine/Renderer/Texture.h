#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/RendererResource.h"

#include <string>
#include <string_view>
#include <vector>

namespace OloEngine
{
    // Defined in Renderer/TextureCompression.h — a mip chain of BCn block bytes.
    struct CompressedTextureImage;

    enum class ImageFormat
    {
        None = 0,
        R8,
        R8UI,
        R16UI,
        RG16UI,
        RGB8,
        RGBA8,
        RGBA16F,
        RGBA32F, // Unsupported
        R32F,    // Unsupported
        RG32F,   // Unsupported
        RGB32F,  // Unsupported
        DEPTH24STENCIL8,
        RG16F, // Keep appended to preserve legacy serialized enum values
        R32I,
        RG8, // 2-channel 8-bit (normal-map xy, two-channel masks). Appended to
             // preserve legacy serialised enum integer values used in asset packs.
        // Block-compressed formats (#440). Uploaded via glCompressedTextureSubImage2D
        // rather than a client pixel format; the sRGB variant of BC7 is selected from
        // TextureSpecification::SRGB. Appended to preserve legacy serialised values.
        BC7, // BPTC RGBA — base color / albedo / emissive
        BC5  // RGTC2 two-channel — tangent-space normal xy
    };

    // True for the block-compressed ImageFormat values, which take the
    // glCompressedTextureSubImage2D upload path instead of a client pixel format.
    [[nodiscard]] constexpr bool IsCompressedFormat(ImageFormat format) noexcept
    {
        return format == ImageFormat::BC7 || format == ImageFormat::BC5;
    }

    struct TextureSpecification
    {
        u32 Width = 1;
        u32 Height = 1;
        ImageFormat Format = ImageFormat::RGBA8;
        bool GenerateMips = true;
        // Explicit mip level count. 0 = auto (1 if !GenerateMips, full chain if GenerateMips).
        u32 MipLevels = 0;
        // Sample count. Values > 1 create multisample storage and force a
        // single mip level.
        u32 Samples = 1;
        // True for color textures (albedo, emissive, UI) — selects an sRGB
        // internal format so the GPU automatically converts sample data from
        // sRGB to linear. Leave false for data textures (normal, metallic,
        // roughness, AO) where the bytes already encode linear values.
        // Only meaningful for 8-bit color formats (RGB8 / RGBA8); ignored
        // for float / integer / depth formats.
        bool SRGB = false;
        // True for textures whose pixels are replaced every frame from the CPU
        // (e.g. streamed video frames). The OpenGL backend then uploads through a
        // double-buffered Pixel Buffer Object ring so the copy + DMA do not stall
        // the render thread. Ignored for multisample textures.
        bool Streaming = false;
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
        AssetType GetAssetType() const override = 0;
    };

    class Texture2D : public Texture
    {
      public:
        virtual void SubImage(u32 x, u32 y, u32 width, u32 height, const void* data, u32 dataSize) = 0;

        [[nodiscard("Store this!")]] virtual u32 GetMipLevelCount() const = 0;

        // Recreate the texture with new dimensions (same spec otherwise).
        // Needed because glTextureStorage2D allocates immutable storage.
        virtual void Resize(u32 width, u32 height) = 0;

        static Ref<Texture2D> Create(const TextureSpecification& specification);
        // Create a GPU texture from an offline block-compressed (BC7/BC5) mip chain,
        // uploaded via glCompressedTextureSubImage2D. On hardware lacking the required
        // BPTC/RGTC support the blocks are decompressed on the CPU and uploaded as an
        // uncompressed RGBA8 texture so nothing breaks (#440).
        static Ref<Texture2D> Create(const CompressedTextureImage& compressedImage);
        // Load a texture from disk. Pass srgb=true for color textures (albedo,
        // emissive, UI) so the GPU converts samples from sRGB to linear on
        // read. Leave srgb=false (default) for data textures (normal map,
        // metallic-roughness, AO, heightmap) where bytes are already linear.
        static Ref<Texture2D> Create(const std::string& path, bool srgb = false);

        // Asset interface
        static constexpr AssetType GetStaticType() noexcept
        {
            return AssetType::Texture2D;
        }
        AssetType GetAssetType() const override
        {
            return GetStaticType();
        }
    };
} // namespace OloEngine

#pragma once
#include "OloEngine/Renderer/Texture.h"

#include <glad/gl.h>

namespace OloEngine
{

    class OpenGLTexture2D : public Texture2D
    {
      public:
        explicit OpenGLTexture2D(const TextureSpecification& specification);
        explicit OpenGLTexture2D(const std::string& path, bool srgb = false);
        // Upload an offline block-compressed (BC7/BC5) mip chain (#440). Falls back to
        // CPU-decompressed RGBA8 when the driver lacks BPTC/RGTC support.
        explicit OpenGLTexture2D(const CompressedTextureImage& compressedImage);
        ~OpenGLTexture2D() override;

        const TextureSpecification& GetSpecification() const override
        {
            return m_Specification;
        }

        [[nodiscard("Store this!")]] u32 GetWidth() const override
        {
            return m_Width;
        }
        [[nodiscard("Store this!")]] u32 GetHeight() const override
        {
            return m_Height;
        }
        [[nodiscard("Store this!")]] u32 GetRendererID() const override
        {
            return m_RendererID;
        }
        [[nodiscard("Store this!")]] const std::string& GetPath() const override
        {
            return m_Path;
        }

        void SetData(void* data, u32 size) override;
        void SubImage(u32 x, u32 y, u32 width, u32 height, const void* data, u32 dataSize) override;
        void Invalidate(std::string_view path, u32 width, u32 height, const void* data, u32 channels) override;

        void Bind(u32 slot) const override;

        [[nodiscard("Store this!")]] bool IsLoaded() const override
        {
            return m_IsLoaded;
        }

        [[nodiscard("Use for transparency")]] bool HasAlphaChannel() const override
        {
            return m_DataFormat == GL_RGBA || m_Specification.Format == ImageFormat::RGBA8 ||
                   m_Specification.Format == ImageFormat::RGBA32F || m_Specification.Format == ImageFormat::BC7;
        }

        bool GetData(std::vector<u8>& outData, u32 mipLevel = 0) const override;

        [[nodiscard("Store this!")]] u32 GetMipLevelCount() const override
        {
            return m_MipLevels;
        }

        void Resize(u32 width, u32 height) override;

      private:
        void InvalidateImpl(std::string_view path, u32 width, u32 height, const void* data, u32 channels);
        // CPU-decompress a block-compressed image and upload it as an uncompressed
        // RGBA8 texture (used when the driver lacks BPTC/RGTC support).
        void UploadDecompressedFallback(const CompressedTextureImage& image);
        void CreateStorage();
        [[nodiscard]] static u32 CalculateFullMipCount(u32 width, u32 height);

      private:
        TextureSpecification m_Specification;

        std::string m_Path;
        bool m_IsLoaded = false;
        u32 m_Width{};
        u32 m_Height{};
        u32 m_MipLevels = 1;
        u32 m_RendererID{};
        GLenum m_InternalFormat{};
        GLenum m_DataFormat{};

        // Double-buffered Pixel Buffer Objects for streaming uploads (see
        // TextureSpecification::Streaming). Created lazily on the first SetData and
        // alternated each call so the CPU copy never waits on the previous DMA.
        GLuint m_PBO[2]{};
        u32 m_PBOIndex = 0;
        u32 m_PBOCapacity = 0;
    };

} // namespace OloEngine

#pragma once
#include "OloEngine/Renderer/Texture.h"

#include <glad/gl.h>

namespace OloEngine
{

    class OpenGLTexture2D : public Texture2D
    {
      public:
        explicit OpenGLTexture2D(const TextureSpecification& specification);
        explicit OpenGLTexture2D(const std::string& path);
        ~OpenGLTexture2D() override;

        virtual const TextureSpecification& GetSpecification() const override
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
        void Invalidate(std::string_view path, u32 width, u32 height, const void* data, u32 channels) override;

        void Bind(u32 slot) const override;

        [[nodiscard("Store this!")]] bool IsLoaded() const override
        {
            return m_IsLoaded;
        }

        [[nodiscard("Use for transparency")]] bool HasAlphaChannel() const override
        {
            return m_DataFormat == GL_RGBA || m_Specification.Format == ImageFormat::RGBA8 || m_Specification.Format == ImageFormat::RGBA32F;
        }

      private:
        void InvalidateImpl(std::string_view path, u32 width, u32 height, const void* data, u32 channels);

      private:
        TextureSpecification m_Specification;

        std::string m_Path;
        bool m_IsLoaded = false;
        u32 m_Width{};
        u32 m_Height{};
        u32 m_RendererID{};
        GLenum m_InternalFormat{};
        GLenum m_DataFormat{};
    };

} // namespace OloEngine

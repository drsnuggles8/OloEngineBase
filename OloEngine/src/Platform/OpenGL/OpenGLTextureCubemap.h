#pragma once

#include "OloEngine/Renderer/TextureCubemap.h"

#include <glad/gl.h>
#include <vector>

namespace OloEngine
{
    class OpenGLTextureCubemap : public TextureCubemap
    {
    public:
        explicit OpenGLTextureCubemap(const CubemapSpecification& specification);
        explicit OpenGLTextureCubemap(const std::vector<std::string>& facePaths);
        ~OpenGLTextureCubemap() override;

        // Texture interface implementation
        const TextureSpecification& GetSpecification() const override { return m_Specification; }
        
        [[nodiscard("Store this!")]] u32 GetWidth() const override { return m_Width; }
        [[nodiscard("Store this!")]] u32 GetHeight() const override { return m_Height; }
        [[nodiscard("Store this!")]] u32 GetRendererID() const override { return m_RendererID; }
        [[nodiscard("Store this!")]] const std::string& GetPath() const override { return m_Path; }

        void SetData(void* data, u32 size) override;
        void Invalidate(std::string_view path, u32 width, u32 height, const void* data, u32 channels) override;

        void Bind(u32 slot) const override;

        [[nodiscard("Store this!")]] bool IsLoaded() const override { return m_IsLoaded; }
		[[nodiscard]] bool HasAlphaChannel() const override { return m_HasAlphaChannel; }

        // TextureCubemap specific methods
        const CubemapSpecification& GetCubemapSpecification() const override { return m_CubemapSpecification; }
        void SetFaceData(u32 faceIndex, void* data, u32 size) override;

    private:
        void LoadFaces(const std::vector<std::string>& facePaths);
        
    private:
        TextureSpecification m_Specification;
        CubemapSpecification m_CubemapSpecification;
        
        std::string m_Path;
        bool m_IsLoaded = false;
        u32 m_Width{};
        u32 m_Height{};
        u32 m_RendererID{};
        GLenum m_InternalFormat{};
        GLenum m_DataFormat{};
		bool m_HasAlphaChannel = false;
    };
}
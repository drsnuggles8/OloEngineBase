#pragma once

#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "OloEngine/Renderer/TextureCubemapArray.h"

#include <glad/gl.h>
#include <vector>

namespace OloEngine
{
    class OpenGLTextureCubemapArray : public TextureCubemapArray
    {
      public:
        explicit OpenGLTextureCubemapArray(const CubemapArraySpecification& specification);
        ~OpenGLTextureCubemapArray() override;

        // Texture interface
        const TextureSpecification& GetSpecification() const override
        {
            return m_Specification;
        }
        [[nodiscard("Store this!")]] u32 GetWidth() const override
        {
            return m_ArraySpecification.Resolution;
        }
        [[nodiscard("Store this!")]] u32 GetHeight() const override
        {
            return m_ArraySpecification.Resolution;
        }
        [[nodiscard("Store this!")]] u32 GetRendererID() const override
        {
            return m_RendererID;
        }
        [[nodiscard]] RHI::ResourceHandle GetRHIHandle() const override
        {
            return m_RHIHandle.Get();
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
        [[nodiscard]] bool HasAlphaChannel() const override
        {
            return false;
        }
        bool GetData(std::vector<u8>& outData, u32 mipLevel = 0) const override;

        // TextureCubemapArray interface
        [[nodiscard]] const CubemapArraySpecification& GetArraySpecification() const override
        {
            return m_ArraySpecification;
        }
        [[nodiscard("Store this!")]] u32 GetMipLevelCount() const override;
        bool SetLayerMipData(u32 layer, u32 mip, const void* data, sizet sizeBytes) override;
        bool CopyLayerFromCubemap(u32 layer, const TextureCubemap& source) override;

      private:
        [[nodiscard]] sizet CalculateMemory(u32 bytesPerPixel, u32 mipLevels) const;

        TextureSpecification m_Specification;
        CubemapArraySpecification m_ArraySpecification;

        std::string m_Path;
        bool m_IsLoaded = false;
        u32 m_RendererID{};
        // Generation-checked identity for m_RendererID, kept in lockstep by
        // m_RHIHandle.Sync() wherever the native name is assigned (issue #691).
        RHI::ScopedResourceHandle m_RHIHandle;
        GLenum m_InternalFormat{};
        GLenum m_DataFormat{};
        GLenum m_DataType{};
        u32 m_BytesPerPixel = 0;
    };
} // namespace OloEngine

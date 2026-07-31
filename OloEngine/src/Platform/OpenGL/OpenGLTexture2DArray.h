#pragma once

#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "OloEngine/Renderer/Texture2DArray.h"

namespace OloEngine
{
    class OpenGLTexture2DArray : public Texture2DArray
    {
      public:
        explicit OpenGLTexture2DArray(const Texture2DArraySpecification& spec);
        ~OpenGLTexture2DArray() override;

        [[nodiscard]] u32 GetWidth() const override
        {
            return m_Width;
        }
        [[nodiscard]] u32 GetHeight() const override
        {
            return m_Height;
        }
        [[nodiscard]] u32 GetLayers() const override
        {
            return m_Layers;
        }
        [[nodiscard]] u32 GetRendererID() const override
        {
            return m_RendererID;
        }

        [[nodiscard]] RHI::ResourceHandle GetRHIHandle() const override
        {
            return m_RHIHandle.Get();
        }
        [[nodiscard]] const Texture2DArraySpecification& GetSpecification() const override
        {
            return m_Specification;
        }

        void Bind(u32 slot) const override;

        void SetLayerData(u32 layer, const void* data, u32 width, u32 height) override;
        void GenerateMipmaps() override;

      private:
        u32 m_RendererID = 0;
        // Generation-checked identity for m_RendererID above, kept in
        // lockstep by m_RHIHandle.Sync() at every site that assigns the
        // native name. RAII retires the entry, so a handle to a destroyed
        // object can never resolve to a recycled GL name (issue #691).
        RHI::ScopedResourceHandle m_RHIHandle;
        u32 m_Width = 0;
        u32 m_Height = 0;
        u32 m_Layers = 0;
        Texture2DArraySpecification m_Specification;
    };
} // namespace OloEngine

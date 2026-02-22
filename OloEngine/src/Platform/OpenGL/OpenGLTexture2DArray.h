#pragma once

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
        [[nodiscard]] const Texture2DArraySpecification& GetSpecification() const override
        {
            return m_Specification;
        }

        void Bind(u32 slot) const override;

      private:
        u32 m_RendererID = 0;
        u32 m_Width = 0;
        u32 m_Height = 0;
        u32 m_Layers = 0;
        Texture2DArraySpecification m_Specification;
    };
} // namespace OloEngine

#pragma once

#include "OloEngine/Renderer/RHI/RHIResourceRegistry.h"
#include "OloEngine/Renderer/Texture3D.h"

namespace OloEngine
{
    class OpenGLTexture3D : public Texture3D
    {
      public:
        explicit OpenGLTexture3D(const Texture3DSpecification& spec);
        ~OpenGLTexture3D() override;

        [[nodiscard]] u32 GetWidth() const override
        {
            return m_Width;
        }
        [[nodiscard]] u32 GetHeight() const override
        {
            return m_Height;
        }
        [[nodiscard]] u32 GetDepth() const override
        {
            return m_Depth;
        }
        [[nodiscard]] u32 GetRendererID() const override
        {
            return m_RendererID;
        }

        [[nodiscard]] RHI::ResourceHandle GetRHIHandle() const override
        {
            return m_RHIHandle.Get();
        }
        [[nodiscard]] const Texture3DSpecification& GetSpecification() const override
        {
            return m_Specification;
        }

        void Bind(u32 slot) const override;
        void SetData(const void* data, u32 size) override;

      private:
        u32 m_RendererID = 0;
        // Generation-checked identity for m_RendererID above, kept in
        // lockstep by m_RHIHandle.Sync() at every site that assigns the
        // native name. RAII retires the entry, so a handle to a destroyed
        // object can never resolve to a recycled GL name (issue #691).
        RHI::ScopedResourceHandle m_RHIHandle;
        u32 m_Width = 0;
        u32 m_Height = 0;
        u32 m_Depth = 0;
        Texture3DSpecification m_Specification;
    };
} // namespace OloEngine

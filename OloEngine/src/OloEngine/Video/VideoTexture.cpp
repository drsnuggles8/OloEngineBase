#include "OloEnginePCH.h"
#include "OloEngine/Video/VideoTexture.h"

namespace OloEngine
{
    void VideoTexture::Initialize(u32 width, u32 height)
    {
        if (width == 0 || height == 0)
            return;
        if (m_Texture && m_Width == width && m_Height == height)
            return;

        TextureSpecification spec;
        spec.Width = width;
        spec.Height = height;
        spec.Format = ImageFormat::RGBA8;
        spec.GenerateMips = false;
        spec.SRGB = false;
        spec.Streaming = true; // Replaced every frame — upload via the PBO ring.
        m_Texture = Texture2D::Create(spec);
        m_Width = width;
        m_Height = height;
    }

    void VideoTexture::Destroy()
    {
        m_Texture = nullptr;
        m_Width = 0;
        m_Height = 0;
    }

    void VideoTexture::UpdateFrame(const u8* rgbaData, u32 width, u32 height)
    {
        if (!rgbaData || width == 0 || height == 0)
            return;

        if (!m_Texture || m_Width != width || m_Height != height)
            Initialize(width, height);
        if (!m_Texture)
            return;

        const u32 size = width * height * 4u;
        m_Texture->SetData(const_cast<u8*>(rgbaData), size);
    }

    void VideoTexture::Bind(u32 slot) const
    {
        if (m_Texture)
            m_Texture->Bind(slot);
    }

    u32 VideoTexture::GetRendererID() const
    {
        return m_Texture ? m_Texture->GetRendererID() : 0u;
    }
} // namespace OloEngine

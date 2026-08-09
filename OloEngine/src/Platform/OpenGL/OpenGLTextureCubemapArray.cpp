#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLTextureCubemapArray.h"
#include "Platform/OpenGL/OpenGLUtilities.h"
#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/Debug/GPUResourceInspector.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/TextureCubemap.h"

#include <algorithm>

namespace OloEngine
{
    namespace
    {
        struct ArrayFormatInfo
        {
            GLenum InternalFormat = 0;
            GLenum DataFormat = 0;
            GLenum DataType = 0;
            u32 BytesPerPixel = 0; // 0 = unsupported
        };

        // Only the formats the probe arrays (and plausible future arrays)
        // actually use — extend when a caller needs more.
        [[nodiscard]] ArrayFormatInfo GetArrayFormatInfo(ImageFormat format)
        {
            switch (format)
            {
                case ImageFormat::R8:
                    return { GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1 };
                case ImageFormat::RGBA8:
                    return { GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4 };
                case ImageFormat::R32F:
                    return { GL_R32F, GL_RED, GL_FLOAT, 4 };
                case ImageFormat::RG32F:
                    return { GL_RG32F, GL_RG, GL_FLOAT, 8 };
                case ImageFormat::RGBA16F:
                    // Client data is uploaded as full floats; the driver packs.
                    return { GL_RGBA16F, GL_RGBA, GL_FLOAT, 16 };
                case ImageFormat::RGBA32F:
                    return { GL_RGBA32F, GL_RGBA, GL_FLOAT, 16 };
                default:
                    return {};
            }
        }
    } // namespace

    OpenGLTextureCubemapArray::OpenGLTextureCubemapArray(const CubemapArraySpecification& specification)
        : m_ArraySpecification(specification)
    {
        OLO_PROFILE_FUNCTION();

        m_Specification.Width = m_ArraySpecification.Resolution;
        m_Specification.Height = m_ArraySpecification.Resolution;
        m_Specification.Format = m_ArraySpecification.Format;
        m_Specification.GenerateMips = m_ArraySpecification.MipLevels != 1;
        m_Specification.MipLevels = m_ArraySpecification.MipLevels;
        m_Path = "Generated CubemapArray";

        auto const info = GetArrayFormatInfo(m_ArraySpecification.Format);
        if (info.BytesPerPixel == 0 || m_ArraySpecification.Resolution == 0 || m_ArraySpecification.Layers == 0)
        {
            OLO_CORE_ERROR("OpenGLTextureCubemapArray: unsupported format {} or empty dimensions ({}x{} layers)",
                           static_cast<u32>(m_ArraySpecification.Format), m_ArraySpecification.Resolution,
                           m_ArraySpecification.Layers);
            m_RHIHandle.Sync(RHI::ResourceKind::Texture, m_RendererID, RHI::Backend::OpenGL);
            return;
        }
        m_InternalFormat = info.InternalFormat;
        m_DataFormat = info.DataFormat;
        m_DataType = info.DataType;
        m_BytesPerPixel = info.BytesPerPixel;

        glCreateTextures(GL_TEXTURE_CUBE_MAP_ARRAY, 1, &m_RendererID);
        m_RHIHandle.Sync(RHI::ResourceKind::Texture, m_RendererID, RHI::Backend::OpenGL);

        u32 const mipLevels = GetMipLevelCount();
        glTextureStorage3D(m_RendererID, static_cast<GLsizei>(mipLevels), m_InternalFormat,
                           static_cast<GLsizei>(m_ArraySpecification.Resolution),
                           static_cast<GLsizei>(m_ArraySpecification.Resolution),
                           static_cast<GLsizei>(m_ArraySpecification.Layers * 6u));

        glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, mipLevels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

        OLO_TRACK_GPU_ALLOC(this,
                            CalculateMemory(m_BytesPerPixel, mipLevels),
                            RendererMemoryTracker::ResourceType::TextureCubemap,
                            "OpenGL TextureCubemapArray");
        GPUResourceInspector::GetInstance().RegisterTextureCubemap(m_RendererID, m_Path, "TextureCubemapArray");

        m_IsLoaded = true;
    }

    OpenGLTextureCubemapArray::~OpenGLTextureCubemapArray()
    {
        OLO_PROFILE_FUNCTION();

        OLO_TRACK_DEALLOC(this);
        GPUResourceInspector::GetInstance().UnregisterResource(m_RendererID);

        // Same skip-bind / dangling-descriptor hazard as every other texture
        // type when the recycled name is later reused (issue #691 Phase 3).
        Utils::RetireTextureViews(m_RHIHandle.Get());

        u32 const id = m_RendererID;
        FrameResourceManager::Get().SubmitForDeletion([id]()
                                                      { glDeleteTextures(1, &id); });
    }

    void OpenGLTextureCubemapArray::SetData(void* /*data*/, u32 /*size*/)
    {
        OLO_CORE_ERROR("SetData is not supported for cubemap arrays; use SetLayerMipData");
    }

    void OpenGLTextureCubemapArray::Invalidate(std::string_view /*path*/, u32 /*width*/, u32 /*height*/,
                                               const void* /*data*/, u32 /*channels*/)
    {
        OLO_CORE_ERROR("Invalidate is not supported for cubemap arrays");
    }

    void OpenGLTextureCubemapArray::Bind(u32 slot) const
    {
        OLO_PROFILE_FUNCTION();

        glBindTextureUnit(slot, m_RendererID);
    }

    u32 OpenGLTextureCubemapArray::GetMipLevelCount() const
    {
        if (m_ArraySpecification.MipLevels > 0)
        {
            return m_ArraySpecification.MipLevels;
        }
        u32 levels = 1;
        for (u32 dim = m_ArraySpecification.Resolution; dim > 1; dim >>= 1)
        {
            ++levels;
        }
        return levels;
    }

    bool OpenGLTextureCubemapArray::SetLayerMipData(u32 layer, u32 mip, const void* data, sizet sizeBytes)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_IsLoaded)
        {
            return false;
        }
        if (layer >= m_ArraySpecification.Layers || mip >= GetMipLevelCount())
        {
            OLO_CORE_ERROR("SetLayerMipData: layer {} / mip {} out of range ({} layers, {} mips)",
                           layer, mip, m_ArraySpecification.Layers, GetMipLevelCount());
            return false;
        }

        u32 const mipRes = std::max(1u, m_ArraySpecification.Resolution >> mip);
        sizet const expected = static_cast<sizet>(mipRes) * mipRes * 6u * m_BytesPerPixel;
        if (sizeBytes != expected)
        {
            OLO_CORE_ERROR("SetLayerMipData: size mismatch for layer {} mip {} (expected {}, got {})",
                           layer, mip, expected, sizeBytes);
            return false;
        }

        // R32F rows are 4-byte multiples at every mip, but R8 (or a future
        // 3-channel format) may not be — force tight unpacking like the
        // cubemap upload path does.
        GLint prevAlignment = 4;
        bool const needAlignment = (static_cast<sizet>(mipRes) * m_BytesPerPixel) % 4u != 0u;
        if (needAlignment)
        {
            glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlignment);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        }

        glTextureSubImage3D(m_RendererID, static_cast<GLint>(mip),
                            0, 0, static_cast<GLint>(layer * 6u),
                            static_cast<GLsizei>(mipRes), static_cast<GLsizei>(mipRes), 6,
                            m_DataFormat, m_DataType, data);

        if (needAlignment)
        {
            glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlignment);
        }
        return true;
    }

    bool OpenGLTextureCubemapArray::CopyLayerFromCubemap(u32 layer, const TextureCubemap& source)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_IsLoaded)
        {
            return false;
        }
        if (layer >= m_ArraySpecification.Layers)
        {
            OLO_CORE_ERROR("CopyLayerFromCubemap: layer {} out of range ({} layers)", layer, m_ArraySpecification.Layers);
            return false;
        }
        auto const& srcSpec = source.GetCubemapSpecification();
        if (srcSpec.Width != m_ArraySpecification.Resolution || srcSpec.Format != m_ArraySpecification.Format)
        {
            // glCopyImageSubData needs identical texel layouts; a silent size
            // or format mismatch would be GL_INVALID_OPERATION at copy time.
            OLO_CORE_ERROR("CopyLayerFromCubemap: source {}x{} format {} does not match array {}x{} format {}",
                           srcSpec.Width, srcSpec.Height, static_cast<u32>(srcSpec.Format),
                           m_ArraySpecification.Resolution, m_ArraySpecification.Resolution,
                           static_cast<u32>(m_ArraySpecification.Format));
            return false;
        }

        u32 const mips = std::min(GetMipLevelCount(), source.GetMipLevelCount());
        for (u32 mip = 0; mip < mips; ++mip)
        {
            u32 const mipRes = std::max(1u, m_ArraySpecification.Resolution >> mip);
            glCopyImageSubData(source.GetRendererID(), GL_TEXTURE_CUBE_MAP, static_cast<GLint>(mip), 0, 0, 0,
                               m_RendererID, GL_TEXTURE_CUBE_MAP_ARRAY, static_cast<GLint>(mip),
                               0, 0, static_cast<GLint>(layer * 6u),
                               static_cast<GLsizei>(mipRes), static_cast<GLsizei>(mipRes), 6);
        }
        return true;
    }

    bool OpenGLTextureCubemapArray::GetData(std::vector<u8>& outData, u32 mipLevel) const
    {
        OLO_PROFILE_FUNCTION();

        if (!m_IsLoaded || mipLevel >= GetMipLevelCount())
        {
            return false;
        }
        u32 const mipRes = std::max(1u, m_ArraySpecification.Resolution >> mipLevel);
        sizet const totalBytes = static_cast<sizet>(mipRes) * mipRes * 6u * m_ArraySpecification.Layers * m_BytesPerPixel;
        outData.resize(totalBytes);

        Utils::DrainGLErrors();
        glGetTextureImage(m_RendererID, static_cast<GLint>(mipLevel), m_DataFormat, m_DataType,
                          static_cast<GLsizei>(totalBytes), outData.data());
        if (GLenum const error = glGetError(); error != GL_NO_ERROR)
        {
            OLO_CORE_ERROR("OpenGLTextureCubemapArray::GetData: GL error {}", error);
            return false;
        }
        return true;
    }

    sizet OpenGLTextureCubemapArray::CalculateMemory(u32 bytesPerPixel, u32 mipLevels) const
    {
        sizet total = 0;
        u32 mipRes = m_ArraySpecification.Resolution;
        for (u32 mip = 0; mip < mipLevels; ++mip)
        {
            total += static_cast<sizet>(mipRes) * mipRes * bytesPerPixel * 6u * m_ArraySpecification.Layers;
            mipRes = std::max(1u, mipRes >> 1);
        }
        return total;
    }
} // namespace OloEngine

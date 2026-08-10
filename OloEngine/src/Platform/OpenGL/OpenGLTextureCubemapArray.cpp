#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLTextureCubemapArray.h"
#include "Platform/OpenGL/OpenGLUtilities.h"
#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/Debug/GPUResourceInspector.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/TextureCubemap.h"

#include <algorithm>
#include <limits>

namespace OloEngine
{
    namespace
    {
        struct ArrayFormatInfo
        {
            GLenum InternalFormat = 0;
            GLenum DataFormat = 0;
            GLenum DataType = 0;
            u32 BytesPerPixel = 0;    // client upload stride (0 = unsupported)
            u32 GPUBytesPerTexel = 0; // resident GPU size — differs for RGBA16F
        };

        // Only the formats the probe arrays (and plausible future arrays)
        // actually use — extend when a caller needs more.
        [[nodiscard]] ArrayFormatInfo GetArrayFormatInfo(ImageFormat format)
        {
            switch (format)
            {
                case ImageFormat::R8:
                    return { GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1, 1 };
                case ImageFormat::RGBA8:
                    return { GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, 4 };
                case ImageFormat::R32F:
                    return { GL_R32F, GL_RED, GL_FLOAT, 4, 4 };
                case ImageFormat::RG32F:
                    return { GL_RG32F, GL_RG, GL_FLOAT, 8, 8 };
                case ImageFormat::RGBA16F:
                    // Client data is uploaded as full floats (16 B/texel); the
                    // driver packs to half on upload, so only 8 B/texel are
                    // resident — the memory tracker must count the latter.
                    return { GL_RGBA16F, GL_RGBA, GL_FLOAT, 16, 8 };
                case ImageFormat::RGBA32F:
                    return { GL_RGBA32F, GL_RGBA, GL_FLOAT, 16, 16 };
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

        // 6 * Layers must fit the driver's array-layer limit (GL 4.6
        // guarantees >= 2048; the probe arrays use at most 192) — a silent
        // over-ask would leave a broken texture behind an IsLoaded() true.
        GLint maxArrayLayers = 0;
        glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &maxArrayLayers);
        if (static_cast<GLint>(m_ArraySpecification.Layers * 6u) > maxArrayLayers)
        {
            OLO_CORE_ERROR("OpenGLTextureCubemapArray: {} layers ({} array slices) exceed GL_MAX_ARRAY_TEXTURE_LAYERS {}",
                           m_ArraySpecification.Layers, m_ArraySpecification.Layers * 6u, maxArrayLayers);
            m_RHIHandle.Sync(RHI::ResourceKind::Texture, m_RendererID, RHI::Backend::OpenGL);
            return;
        }

        // Attribute any allocation-time error to THIS storage call, not a
        // leaked flag from an earlier caller.
        Utils::DrainGLErrors();

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

        if (GLenum const error = glGetError(); error != GL_NO_ERROR)
        {
            OLO_CORE_ERROR("OpenGLTextureCubemapArray: storage allocation failed (GL error {}, {} layers @ {}^2)",
                           error, m_ArraySpecification.Layers, m_ArraySpecification.Resolution);
            glDeleteTextures(1, &m_RendererID);
            m_RendererID = 0;
            m_RHIHandle.Sync(RHI::ResourceKind::Texture, m_RendererID, RHI::Backend::OpenGL);
            return; // m_IsLoaded stays false — EnsureArrays treats this as a failed create
        }

        OLO_TRACK_GPU_ALLOC(this,
                            CalculateMemory(info.GPUBytesPerTexel, mipLevels),
                            RendererMemoryTracker::ResourceType::TextureCubemap,
                            "OpenGL TextureCubemapArray");
        GPUResourceInspector::GetInstance().RegisterTextureCubemap(m_RendererID, m_Path, "TextureCubemapArray");

        m_IsLoaded = true;
    }

    OpenGLTextureCubemapArray::~OpenGLTextureCubemapArray()
    {
        OLO_PROFILE_FUNCTION();

        // Tracker/inspector registration and m_IsLoaded flip together on the
        // constructor's one success path, so a failed construction (no GL
        // object, nothing tracked) tears down as a no-op — untracking an
        // address the tracker never saw would log a bogus mismatch.
        if (!m_IsLoaded)
        {
            return;
        }

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

        // Drain first so the post-call check describes THIS upload (the GL
        // error flag is a sticky global), then report failure to the caller —
        // ReflectionProbeArray skips the probe rather than shading from a
        // half-uploaded layer.
        Utils::DrainGLErrors();
        glTextureSubImage3D(m_RendererID, static_cast<GLint>(mip),
                            0, 0, static_cast<GLint>(layer * 6u),
                            static_cast<GLsizei>(mipRes), static_cast<GLsizei>(mipRes), 6,
                            m_DataFormat, m_DataType, data);

        if (needAlignment)
        {
            glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlignment);
        }
        if (GLenum const error = glGetError(); error != GL_NO_ERROR)
        {
            OLO_CORE_ERROR("SetLayerMipData: GL error {} uploading layer {} mip {}", error, layer, mip);
            return false;
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

        // Drain first so the post-loop check describes THESE copies; a copy
        // that errored (e.g. an internal-format class mismatch this class's
        // spec check could not see) must fail the layer, not shade garbage.
        Utils::DrainGLErrors();
        u32 const mips = std::min(GetMipLevelCount(), source.GetMipLevelCount());
        for (u32 mip = 0; mip < mips; ++mip)
        {
            u32 const mipRes = std::max(1u, m_ArraySpecification.Resolution >> mip);
            glCopyImageSubData(source.GetRendererID(), GL_TEXTURE_CUBE_MAP, static_cast<GLint>(mip), 0, 0, 0,
                               m_RendererID, GL_TEXTURE_CUBE_MAP_ARRAY, static_cast<GLint>(mip),
                               0, 0, static_cast<GLint>(layer * 6u),
                               static_cast<GLsizei>(mipRes), static_cast<GLsizei>(mipRes), 6);
        }
        if (GLenum const error = glGetError(); error != GL_NO_ERROR)
        {
            OLO_CORE_ERROR("CopyLayerFromCubemap: GL error {} copying into layer {}", error, layer);
            return false;
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
        if (totalBytes > static_cast<sizet>(std::numeric_limits<GLsizei>::max()))
        {
            OLO_CORE_ERROR("OpenGLTextureCubemapArray::GetData: {} bytes exceeds the GLsizei readback limit", totalBytes);
            return false;
        }
        outData.resize(totalBytes);

        // Tight packing to match the tightly-packed buffer the caller gets —
        // the default 4-byte row alignment would skew R8 rows at odd widths
        // (mirror of SetLayerMipData's unpack handling).
        GLint prevAlignment = 4;
        bool const needAlignment = (static_cast<sizet>(mipRes) * m_BytesPerPixel) % 4u != 0u;
        if (needAlignment)
        {
            glGetIntegerv(GL_PACK_ALIGNMENT, &prevAlignment);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
        }

        Utils::DrainGLErrors();
        glGetTextureImage(m_RendererID, static_cast<GLint>(mipLevel), m_DataFormat, m_DataType,
                          static_cast<GLsizei>(totalBytes), outData.data());

        if (needAlignment)
        {
            glPixelStorei(GL_PACK_ALIGNMENT, prevAlignment);
        }
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

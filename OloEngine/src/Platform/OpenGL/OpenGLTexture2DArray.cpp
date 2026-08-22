#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLTexture2DArray.h"
#include "Platform/OpenGL/OpenGLUtilities.h"
#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/RendererAPI.h"

#if OLO_WITH_VULKAN
// OLO_WITH_VULKAN-guarded factory TU may see Platform/Vulkan/ headers (the
// sanctioned factory-include pattern, rhi-abstraction-boundary.md).
#include "Platform/Vulkan/VulkanDevice.h"
#include "Platform/Vulkan/VulkanTransientResources.h"
#endif

#include <glad/gl.h>

namespace OloEngine
{
    namespace
    {
        auto Texture2DArrayFormatToGL(Texture2DArrayFormat format) -> GLenum
        {
            switch (format)
            {
                case Texture2DArrayFormat::DEPTH_COMPONENT32F:
                    return GL_DEPTH_COMPONENT32F;
                case Texture2DArrayFormat::RGBA8:
                    return GL_RGBA8;
                case Texture2DArrayFormat::RGBA16F:
                    return GL_RGBA16F;
                case Texture2DArrayFormat::RGBA32F:
                    return GL_RGBA32F;
                case Texture2DArrayFormat::RGBA32UI:
                    return GL_RGBA32UI;
                case Texture2DArrayFormat::BC7:
                    // The LINEAR variant, deliberately — VT cache texels are
                    // transcoded payload, not sRGB-authored colour.
                    return GL_COMPRESSED_RGBA_BPTC_UNORM;
            }
            OLO_CORE_ASSERT(false, "Unknown Texture2DArrayFormat");
            return 0;
        }
    } // namespace

    OpenGLTexture2DArray::OpenGLTexture2DArray(const Texture2DArraySpecification& spec)
        : m_Width(spec.Width), m_Height(spec.Height), m_Layers(spec.Layers), m_Specification(spec)
    {
        OLO_PROFILE_FUNCTION();

        GLenum internalFormat = Texture2DArrayFormatToGL(spec.Format);

        glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &m_RendererID);
        m_RHIHandle.Sync(RHI::ResourceKind::Texture, m_RendererID, RHI::Backend::OpenGL);

        // Calculate mip levels
        GLsizei mipLevels = 1;
        if (spec.GenerateMipmaps)
        {
            mipLevels = static_cast<GLsizei>(std::floor(std::log2(std::max(m_Width, m_Height)))) + 1;
        }
        glTextureStorage3D(m_RendererID, mipLevels, internalFormat, static_cast<GLsizei>(m_Width), static_cast<GLsizei>(m_Height), static_cast<GLsizei>(m_Layers));

        // Integer textures are incomplete under LINEAR filtering — sampling one
        // returns undefined results, so RGBA32UI must be NEAREST on both
        // filters. BC7 keeps the linear path below like every other colour
        // format.
        if (spec.Format == Texture2DArrayFormat::RGBA32UI)
        {
            glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
        else
        {
            if (spec.GenerateMipmaps)
            {
                glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            }
            else
            {
                glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            }
            glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }

        const bool isDepthFormat = (spec.Format == Texture2DArrayFormat::DEPTH_COMPONENT32F);
        const GLenum wrapMode = isDepthFormat ? GL_CLAMP_TO_BORDER : GL_CLAMP_TO_EDGE;
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_S, static_cast<GLint>(wrapMode));
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_T, static_cast<GLint>(wrapMode));
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_R, static_cast<GLint>(wrapMode));

        if (isDepthFormat)
        {
            // White border so areas outside the shadow map read as "no shadow"
            constexpr std::array<float, 4> borderColor = { 1.0f, 1.0f, 1.0f, 1.0f };
            glTextureParameterfv(m_RendererID, GL_TEXTURE_BORDER_COLOR, borderColor.data());
        }

        if (isDepthFormat && spec.DepthComparisonMode)
        {
            glTextureParameteri(m_RendererID, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
            glTextureParameteri(m_RendererID, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        }

        // Track GPU memory allocation
        sizet bytesPerPixel = 4; // DEPTH_COMPONENT32F = 4, RGBA8 = 4
        if (spec.Format == Texture2DArrayFormat::RGBA16F)
        {
            bytesPerPixel = 8;
        }
        else if (spec.Format == Texture2DArrayFormat::RGBA32F ||
                 spec.Format == Texture2DArrayFormat::RGBA32UI)
        {
            bytesPerPixel = 16;
        }
        else if (spec.Format == Texture2DArrayFormat::BC7)
        {
            bytesPerPixel = 1; // 16 bytes per 4x4 block
        }
        else
        {
            // No additional handling required.
        }
        sizet textureMemory = static_cast<sizet>(m_Width) * m_Height * m_Layers * bytesPerPixel;
        OLO_TRACK_GPU_ALLOC(this,
                            textureMemory,
                            RendererMemoryTracker::ResourceType::Texture2D,
                            "OpenGL Texture2DArray");
    }

    OpenGLTexture2DArray::~OpenGLTexture2DArray()
    {
        OLO_PROFILE_FUNCTION();
        OLO_TRACK_DEALLOC(this);

        // Every texture type that mints an RHI handle owes this — see
        // Utils::RetireTextureViews. A Texture2DArray is the shadow-map array
        // and the ocean FFT ping-pong pair, the latter bound as a storage-image
        // descriptor through the heap, so destroying one used to leave a
        // resident image handle on a texture that was about to be deleted
        // (issue #691).
        Utils::RetireTextureViews(m_RHIHandle.Get());

        u32 id = m_RendererID;
        FrameResourceManager::Get().SubmitForDeletion([id]()
                                                      { glDeleteTextures(1, &id); });
    }

    void OpenGLTexture2DArray::Bind(u32 slot) const
    {
        OLO_PROFILE_FUNCTION();
        glBindTextureUnit(slot, m_RendererID);
    }

    void OpenGLTexture2DArray::SetLayerData(u32 layer, const void* data, u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_ASSERT(layer < m_Layers, "Layer index out of bounds");
        OLO_CORE_ASSERT(width == m_Width && height == m_Height, "Layer data dimensions must match array dimensions");

        GLenum dataFormat = GL_RGBA;
        GLenum dataType = GL_UNSIGNED_BYTE;

        switch (m_Specification.Format)
        {
            case Texture2DArrayFormat::RGBA8:
                dataFormat = GL_RGBA;
                dataType = GL_UNSIGNED_BYTE;
                break;
            case Texture2DArrayFormat::RGBA16F:
            case Texture2DArrayFormat::RGBA32F:
                dataFormat = GL_RGBA;
                dataType = (m_Specification.Format == Texture2DArrayFormat::RGBA16F) ? GL_HALF_FLOAT : GL_FLOAT;
                break;
            case Texture2DArrayFormat::RGBA32UI:
                dataFormat = GL_RGBA_INTEGER;
                dataType = GL_UNSIGNED_INT;
                break;
            case Texture2DArrayFormat::BC7:
                // glTextureSubImage3D on block-compressed storage is
                // GL_INVALID_OPERATION; BC7 layers are populated GPU-side via
                // CopyImageSubDataFull (issue #715).
                OLO_CORE_ASSERT(false, "SetLayerData not supported for block-compressed formats");
                return;
            default:
                OLO_CORE_ASSERT(false, "SetLayerData not supported for depth formats");
                return;
        }

        glTextureSubImage3D(
            m_RendererID,
            0,                               // mip level 0
            0, 0, static_cast<GLint>(layer), // x, y, layer offset
            static_cast<GLsizei>(width),
            static_cast<GLsizei>(height),
            1, // depth = 1 layer
            dataFormat,
            dataType,
            data);
    }

    void OpenGLTexture2DArray::GenerateMipmaps()
    {
        OLO_PROFILE_FUNCTION();
        // Compressed VT caches never mip, and glGenerateTextureMipmap on a
        // block-compressed or integer internal format is GL_INVALID_OPERATION.
        if (m_Specification.Format == Texture2DArrayFormat::BC7 ||
            m_Specification.Format == Texture2DArrayFormat::RGBA32UI)
        {
            OLO_CORE_ASSERT(false, "GenerateMipmaps not supported for block-compressed or integer formats");
            return;
        }
        glGenerateTextureMipmap(m_RendererID);
    }

    // Factory. Backend-switched (issue #691): this was the
    // one texture factory with no Vulkan arm, so ShadowMap's lazily-created
    // CSM/atlas placeholder — the first Texture2DArray any Vulkan frame
    // touches, via VolumetricFogPass::Execute — constructed a GL object
    // whose glCreateTextures call is an access violation in any process
    // that never loaded a GL context (VulkanPassSuiteTest isolated runs).
    Ref<Texture2DArray> Texture2DArray::Create(const Texture2DArraySpecification& spec)
    {
        OLO_PROFILE_FUNCTION();
        switch (RendererAPI::GetAPI())
        {
            case RendererAPI::API::None:
            {
                OLO_CORE_ASSERT(false, "RendererAPI::None is currently not supported!");
                return nullptr;
            }
            case RendererAPI::API::Vulkan:
            {
#if OLO_WITH_VULKAN
                // A Vulkan resource cannot exist without a device, so fall
                // through to the assert when none is up.
                if (VulkanDevice::Get() != nullptr)
                {
                    return Ref<VulkanTexture2DArray>::Create(spec);
                }
#endif
                OLO_CORE_ASSERT(false, "RendererAPI::Vulkan: no VulkanDevice is up (or OLO_WITH_VULKAN is compiled out) — cannot create a Vulkan 2D texture array!");
                return nullptr;
            }
            case RendererAPI::API::OpenGL:
                return Ref<OpenGLTexture2DArray>::Create(spec);
        }

        OLO_CORE_ASSERT(false, "Unknown RendererAPI!");
        return nullptr;
    }
} // namespace OloEngine

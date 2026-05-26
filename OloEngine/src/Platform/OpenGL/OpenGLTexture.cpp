#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLTexture.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Debug/GPUResourceInspector.h"

#define STB_IMAGE_IMPLEMENTATION
// stb_image's SSE2 JPEG decoder passes non-constant values to _mm_shufflelo_epi16
// when GCC inlines aggressively with AVX2 flags, causing "not an 8-bit immediate" errors.
#if defined(__GNUC__) && !defined(__clang__)
#define STBI_NO_SIMD
#endif
#include <stb_image/stb_image.h>

namespace OloEngine
{
    namespace Utils
    {

        [[nodiscard("Store this!")]] static GLenum OloEngineImageFormatToGLDataFormat(ImageFormat format)
        {
            switch (format)
            {
                case ImageFormat::R8:
                    return GL_RED;
                case ImageFormat::R8UI:
                    return GL_RED_INTEGER;
                case ImageFormat::R16UI:
                    return GL_RED_INTEGER;
                case ImageFormat::RG16UI:
                    return GL_RG_INTEGER;
                case ImageFormat::RG16F:
                    return GL_RG;
                case ImageFormat::RG8:
                    return GL_RG;
                case ImageFormat::RGB8:
                    return GL_RGB;
                case ImageFormat::RGBA8:
                    return GL_RGBA;
                case ImageFormat::RGBA16F:
                    return GL_RGBA;
                case ImageFormat::R32F:
                    return GL_RED;
                case ImageFormat::R32I:
                    return GL_RED_INTEGER;
                case ImageFormat::RG32F:
                    return GL_RG;
                case ImageFormat::RGB32F:
                    return GL_RGB;
                case ImageFormat::RGBA32F:
                    return GL_RGBA;
                case ImageFormat::DEPTH24STENCIL8:
                    return GL_DEPTH_STENCIL;
            }

            OLO_CORE_ASSERT(false, "Unknown ImageFormat!");
            return 0;
        }

        [[nodiscard("Store this!")]] static GLenum OloEngineImageFormatToGLInternalFormat(ImageFormat format, bool srgb = false)
        {
            switch (format)
            {
                case ImageFormat::R8:
                    return GL_R8;
                case ImageFormat::R8UI:
                    return GL_R8UI;
                case ImageFormat::R16UI:
                    return GL_R16UI;
                case ImageFormat::RG16UI:
                    return GL_RG16UI;
                case ImageFormat::RG16F:
                    return GL_RG16F;
                case ImageFormat::RG8:
                    // sRGB doesn't apply to 2-channel data textures.
                    return GL_RG8;
                case ImageFormat::RGB8:
                    // sRGB only applies to 8-bit color formats; the GPU converts
                    // sample reads from sRGB to linear automatically.
                    return srgb ? GL_SRGB8 : GL_RGB8;
                case ImageFormat::RGBA8:
                    return srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
                case ImageFormat::RGBA16F:
                    return GL_RGBA16F;
                case ImageFormat::R32F:
                    return GL_R32F;
                case ImageFormat::R32I:
                    return GL_R32I;
                case ImageFormat::RG32F:
                    return GL_RG32F;
                case ImageFormat::RGB32F:
                    return GL_RGB32F;
                case ImageFormat::RGBA32F:
                    return GL_RGBA32F;
                case ImageFormat::DEPTH24STENCIL8:
                    return GL_DEPTH24_STENCIL8;
            }

            OLO_CORE_ASSERT(false, "Unknown ImageFormat!");
            return 0;
        }

    } // namespace Utils

    u32 OpenGLTexture2D::CalculateFullMipCount(u32 width, u32 height)
    {
        return static_cast<u32>(std::floor(std::log2(static_cast<f64>(std::max(width, height))))) + 1;
    }

    void OpenGLTexture2D::CreateStorage()
    {
        const auto sampleCount = std::max(m_Specification.Samples, 1u);
        const bool multisampled = sampleCount > 1u;

        glCreateTextures(multisampled ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D, 1, &m_RendererID);

        if (multisampled)
        {
            glTextureStorage2DMultisample(m_RendererID,
                                          static_cast<GLsizei>(sampleCount),
                                          m_InternalFormat,
                                          static_cast<GLsizei>(m_Width),
                                          static_cast<GLsizei>(m_Height),
                                          GL_FALSE);
        }
        else
        {
            glTextureStorage2D(m_RendererID,
                               static_cast<GLsizei>(m_MipLevels),
                               m_InternalFormat,
                               static_cast<GLsizei>(m_Width),
                               static_cast<GLsizei>(m_Height));
        }
    }

    OpenGLTexture2D::OpenGLTexture2D(const TextureSpecification& specification)
        : m_Specification(specification), m_Width(m_Specification.Width), m_Height(m_Specification.Height)
    {
        OLO_PROFILE_FUNCTION();

        m_Specification.Samples = std::max(m_Specification.Samples, 1u);
        m_InternalFormat = Utils::OloEngineImageFormatToGLInternalFormat(m_Specification.Format, m_Specification.SRGB);
        m_DataFormat = Utils::OloEngineImageFormatToGLDataFormat(m_Specification.Format);

        if (m_Specification.Samples > 1u)
        {
            m_Specification.GenerateMips = false;
            m_Specification.MipLevels = 1u;
            m_MipLevels = 1u;
        }
        else if (m_Specification.MipLevels > 0)
        {
            m_MipLevels = m_Specification.MipLevels;
        }
        else if (m_Specification.GenerateMips)
        {
            m_MipLevels = CalculateFullMipCount(m_Width, m_Height);
        }
        else
        {
            m_MipLevels = 1;
        }

        CreateStorage();

        // Calculate memory usage based on format and dimensions
        u32 bytesPerPixel = 4; // Default to RGBA
        switch (m_Specification.Format)
        {
            case ImageFormat::R8:
            case ImageFormat::R8UI:
                bytesPerPixel = 1;
                break;
            case ImageFormat::R16UI:
                bytesPerPixel = 2;
                break;
            case ImageFormat::RG8:
                bytesPerPixel = 2;
                break;
            case ImageFormat::RG16UI:
                bytesPerPixel = 4;
                break;
            case ImageFormat::RG16F:
                bytesPerPixel = 4;
                break;
            case ImageFormat::RGB8:
                bytesPerPixel = 3;
                break;
            case ImageFormat::RGBA8:
                bytesPerPixel = 4;
                break;
            case ImageFormat::RGBA16F:
                bytesPerPixel = 8;
                break;
            case ImageFormat::R32F:
                bytesPerPixel = 4;
                break;
            case ImageFormat::R32I:
                bytesPerPixel = 4;
                break;
            case ImageFormat::RG32F:
                bytesPerPixel = 8;
                break;
            case ImageFormat::RGB32F:
                bytesPerPixel = 12;
                break;
            case ImageFormat::RGBA32F:
                bytesPerPixel = 16;
                break;
            case ImageFormat::DEPTH24STENCIL8:
                bytesPerPixel = 4;
                break;
        }
        auto textureMemory = static_cast<sizet>(m_Width) * static_cast<sizet>(m_Height) *
                             static_cast<sizet>(bytesPerPixel) * static_cast<sizet>(m_Specification.Samples);
        // Track GPU memory allocation
        OLO_TRACK_GPU_ALLOC(this,
                            textureMemory,
                            RendererMemoryTracker::ResourceType::Texture2D,
                            "OpenGL Texture2D (spec)");

        // Register with GPU Resource Inspector
        GPUResourceInspector::GetInstance().RegisterTexture(m_RendererID, "Texture2D (spec)", "Texture2D");

        if (m_Specification.Samples == 1u)
        {
            // NOTE: Texture Wrapping
            glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

            // NOTE: Texture Filtering
            glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_T, GL_REPEAT);
        }

        m_IsLoaded = true;
    }

    OpenGLTexture2D::OpenGLTexture2D(const std::string& path, bool srgb)
    {
        OLO_PROFILE_FUNCTION();

        // Stash the sRGB choice on the spec so InvalidateImpl picks the right
        // GL internal format. Reusing m_Specification.SRGB keeps the path-load
        // and spec constructors converging on a single source of truth.
        m_Specification.SRGB = srgb;

        int width = 0;
        int height = 0;
        int channels = 0;
        // Use the THREAD-LOCAL flip setter. Once stbi_set_flip_vertically_on_load_thread
        // has been called anywhere in this thread (e.g. from CreatePackedMetallicRoughnessTexture's
        // LoadSingleChannelImage), the thread-local override is permanently latched and the
        // *global* setter via stbi_set_flip_vertically_on_load() is silently ignored — which
        // produced the long-running "second Model load has un-flipped textures" bug. Setting
        // the thread-local explicitly here makes the load deterministic regardless of any
        // prior calls.
        ::stbi_set_flip_vertically_on_load_thread(1);
        stbi_uc* data = nullptr;
        {
            OLO_PROFILE_SCOPE("stbi_load - OpenGLTexture2D::OpenGLTexture2D(const std::string&)");
            data = ::stbi_load(path.c_str(), &width, &height, &channels, 0);
        }
        ::stbi_set_flip_vertically_on_load_thread(0); // reset thread-local flag to avoid polluting later stbi calls

        if (!data)
        {
            OLO_CORE_ERROR("Image texture data is null! Failed to load: '{}'", path);
            ::stbi_image_free(data);
            return;
        }

        InvalidateImpl(path, static_cast<u32>(width), static_cast<u32>(height), data, static_cast<u32>(channels));

        ::stbi_image_free(data);
    }
    OpenGLTexture2D::~OpenGLTexture2D()
    {
        OLO_PROFILE_FUNCTION();
        // Track GPU memory deallocation
        OLO_TRACK_DEALLOC(this);

        // Unregister from GPU Resource Inspector
        GPUResourceInspector::GetInstance().UnregisterResource(m_RendererID);

        // Drop any cached "this slot already has this texture bound" entries so a
        // future bind with a recycled GL ID isn't skipped against stale tracking.
        CommandDispatch::InvalidateTextureBinding(m_RendererID);

        u32 id = m_RendererID;
        FrameResourceManager::Get().SubmitForDeletion([id]()
                                                      { glDeleteTextures(1, &id); });
    }

    void OpenGLTexture2D::Resize(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("OpenGLTexture2D::Resize: Invalid dimensions {}x{}", width, height);
            return;
        }

        // Dealloc old
        OLO_TRACK_DEALLOC(this);
        GPUResourceInspector::GetInstance().UnregisterResource(m_RendererID);
        CommandDispatch::InvalidateTextureBinding(m_RendererID);

        u32 oldId = m_RendererID;
        FrameResourceManager::Get().SubmitForDeletion([oldId]()
                                                      { glDeleteTextures(1, &oldId); });

        m_Width = width;
        m_Height = height;
        m_Specification.Width = width;
        m_Specification.Height = height;

        // Recalculate mip count if auto
        if (m_Specification.Samples > 1u)
        {
            m_Specification.GenerateMips = false;
            m_Specification.MipLevels = 1u;
            m_MipLevels = 1u;
        }
        else if (m_Specification.MipLevels > 0)
        {
            m_MipLevels = m_Specification.MipLevels;
        }
        else if (m_Specification.GenerateMips)
        {
            m_MipLevels = CalculateFullMipCount(m_Width, m_Height);
        }
        else
        {
            m_MipLevels = 1;
        }

        CreateStorage();

        // Re-track
        u32 bytesPerPixel = 4;
        switch (m_Specification.Format)
        {
            case ImageFormat::R8:
            case ImageFormat::R8UI:
                bytesPerPixel = 1;
                break;
            case ImageFormat::R16UI:
                bytesPerPixel = 2;
                break;
            case ImageFormat::RG8:
                bytesPerPixel = 2;
                break;
            case ImageFormat::RG16UI:
                bytesPerPixel = 4;
                break;
            case ImageFormat::RG16F:
                bytesPerPixel = 4;
                break;
            case ImageFormat::R32F:
                bytesPerPixel = 4;
                break;
            case ImageFormat::RG32F:
                bytesPerPixel = 8;
                break;
            case ImageFormat::RGB8:
                bytesPerPixel = 3;
                break;
            case ImageFormat::RGBA8:
                bytesPerPixel = 4;
                break;
            case ImageFormat::RGBA16F:
                bytesPerPixel = 8;
                break;
            case ImageFormat::RGB32F:
                bytesPerPixel = 12;
                break;
            case ImageFormat::RGBA32F:
                bytesPerPixel = 16;
                break;
            case ImageFormat::DEPTH24STENCIL8:
                bytesPerPixel = 4;
                break;
            default:
                break;
        }
        auto textureMemory = static_cast<sizet>(m_Width) * static_cast<sizet>(m_Height) *
                             static_cast<sizet>(bytesPerPixel) * static_cast<sizet>(m_Specification.Samples);
        OLO_TRACK_GPU_ALLOC(this, textureMemory, RendererMemoryTracker::ResourceType::Texture2D, "OpenGL Texture2D (resized)");
        GPUResourceInspector::GetInstance().RegisterTexture(m_RendererID, "Texture2D (resized)", "Texture2D");

        if (m_Specification.Samples == 1u)
        {
            // Reapply sampler state
            glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, m_MipLevels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
            glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_T, GL_REPEAT);
        }

        m_IsLoaded = true;
    }

    void OpenGLTexture2D::SetData(void* const data, const u32 size)
    {
        OLO_PROFILE_FUNCTION();

        if (m_Specification.Samples > 1u)
        {
            OLO_CORE_ERROR("OpenGLTexture2D::SetData: multisample textures do not support data uploads");
            return;
        }

        // Calculate bytes per pixel based on the texture's image format
        u32 bpp = 4; // Default to RGBA8
        GLenum dataType = GL_UNSIGNED_BYTE;

        switch (m_Specification.Format)
        {
            case ImageFormat::R8:
                bpp = 1;
                dataType = GL_UNSIGNED_BYTE;
                break;
            case ImageFormat::R8UI:
                bpp = 1;
                dataType = GL_UNSIGNED_BYTE;
                break;
            case ImageFormat::R16UI:
                bpp = 2;
                dataType = GL_UNSIGNED_SHORT;
                break;
            case ImageFormat::RG8:
                bpp = 2;
                dataType = GL_UNSIGNED_BYTE;
                break;
            case ImageFormat::RG16UI:
                bpp = 4;
                dataType = GL_UNSIGNED_SHORT;
                break;
            case ImageFormat::RG16F:
                bpp = 8; // Upload as GL_FLOAT (2 * 4 bytes), OpenGL converts to half-float
                dataType = GL_FLOAT;
                break;
            case ImageFormat::RGB8:
                bpp = 3;
                dataType = GL_UNSIGNED_BYTE;
                break;
            case ImageFormat::RGBA8:
                bpp = 4;
                dataType = GL_UNSIGNED_BYTE;
                break;
            case ImageFormat::RGBA16F:
                bpp = 16; // Upload as GL_FLOAT (4 * 4 bytes), OpenGL converts to half-float
                dataType = GL_FLOAT;
                break;
            case ImageFormat::R32F:
                bpp = 4;
                dataType = GL_FLOAT;
                break;
            case ImageFormat::RG32F:
                bpp = 8;
                dataType = GL_FLOAT;
                break;
            case ImageFormat::RGB32F:
                bpp = 12;
                dataType = GL_FLOAT;
                break;
            case ImageFormat::RGBA32F:
                bpp = 16;
                dataType = GL_FLOAT;
                break;
            default:
                break;
        }

        OLO_CORE_ASSERT(size == m_Width * m_Height * bpp, "Data must be entire texture! Expected: {}, Got: {}", m_Width * m_Height * bpp, size);
        glTextureSubImage2D(m_RendererID, 0, 0, 0, static_cast<int>(m_Width), static_cast<int>(m_Height), m_DataFormat, dataType, data);
    }

    void OpenGLTexture2D::SubImage(u32 x, u32 y, u32 width, u32 height, const void* data, [[maybe_unused]] u32 dataSize)
    {
        OLO_PROFILE_FUNCTION();

        if (m_Specification.Samples > 1u)
        {
            OLO_CORE_ERROR("OpenGLTexture2D::SubImage: multisample textures do not support sub-image uploads");
            return;
        }

        if (!data || width == 0 || height == 0)
            return;

        if (width > m_Width || height > m_Height || x > m_Width - width || y > m_Height - height)
        {
            OLO_CORE_ERROR("OpenGLTexture2D::SubImage - Region ({},{} {}x{}) exceeds texture bounds ({}x{})",
                           x, y, width, height, m_Width, m_Height);
            return;
        }

        GLenum dataType = GL_UNSIGNED_BYTE;
        switch (m_Specification.Format)
        {
            case ImageFormat::R16UI:
                dataType = GL_UNSIGNED_SHORT;
                break;
            case ImageFormat::RG16UI:
                dataType = GL_UNSIGNED_SHORT;
                break;
            case ImageFormat::RG16F:
                dataType = GL_FLOAT;
                break;
            case ImageFormat::R32F:
            case ImageFormat::RG32F:
            case ImageFormat::RGB32F:
            case ImageFormat::RGBA32F:
                dataType = GL_FLOAT;
                break;
            case ImageFormat::RGBA16F:
                dataType = GL_FLOAT;
                break;
            default:
                break;
        }

        glTextureSubImage2D(
            m_RendererID, 0,
            static_cast<GLint>(x), static_cast<GLint>(y),
            static_cast<GLsizei>(width), static_cast<GLsizei>(height),
            m_DataFormat, dataType, data);
    }

    void OpenGLTexture2D::Invalidate(std::string_view path, u32 width, u32 height, const void* data, u32 channels)
    {
        OLO_PROFILE_FUNCTION();

        InvalidateImpl(path, width, height, data, channels);
    }

    void OpenGLTexture2D::InvalidateImpl(std::string_view path, u32 width, u32 height, const void* data, u32 channels)
    {
        OLO_PROFILE_FUNCTION();

        m_Path = path;
        OLO_CORE_TRACE("Loading texture from path: {}", m_Path);
        m_IsLoaded = true;
        m_Width = width;
        m_Height = height;
        // Mirror the decoded dimensions onto the spec so GetSpecification()
        // reports the real bitmap, not the default-constructed 1x1 spec.
        m_Specification.Width = width;
        m_Specification.Height = height;

        GLenum internalFormat = 0;
        GLenum dataFormat = 0;
        // SRGB internal formats are only defined for 3- and 4-channel color
        // textures. Single- and dual-channel data textures (heightmaps, mask
        // channels) remain linear regardless of the flag.
        const bool useSrgb = m_Specification.SRGB && (channels == 3 || channels == 4);
        switch (channels)
        {
            case 1:
                internalFormat = GL_R8;
                dataFormat = GL_RED;
                m_Specification.Format = ImageFormat::R8;
                OLO_CORE_TRACE("Texture channel count is 1. Internal format is: {}. Data Format is: {}.", internalFormat, dataFormat);
                break;
            case 2:
                internalFormat = GL_RG8;
                dataFormat = GL_RG;
                m_Specification.Format = ImageFormat::RG8;
                OLO_CORE_TRACE("Texture channel count is 2. Internal format is: {}. Data Format is: {}.", internalFormat, dataFormat);
                break;
            case 3:
                internalFormat = useSrgb ? GL_SRGB8 : GL_RGB8;
                dataFormat = GL_RGB;
                m_Specification.Format = ImageFormat::RGB8;
                OLO_CORE_TRACE("Texture channel count is 3. Internal format is: {}. Data Format is: {}.", internalFormat, dataFormat);
                break;
            case 4:
                internalFormat = useSrgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
                dataFormat = GL_RGBA;
                m_Specification.Format = ImageFormat::RGBA8;
                OLO_CORE_TRACE("Texture channel count is 4. Internal format is: {}. Data Format is: {}.", internalFormat, dataFormat);
                break;
            default:
                OLO_CORE_ERROR("Texture channel count is not within (1-4) range. Channel count: {}", channels);
                return;
        }

        m_InternalFormat = internalFormat;
        m_DataFormat = dataFormat;

        OLO_CORE_ASSERT(internalFormat & dataFormat, "Format not supported!");
        glCreateTextures(GL_TEXTURE_2D, 1, &m_RendererID);
        glTextureStorage2D(m_RendererID, 1, internalFormat, static_cast<int>(m_Width), static_cast<int>(m_Height));

        // Calculate memory usage based on channels and dimensions
        sizet textureMemory = static_cast<sizet>(m_Width) * m_Height * channels;
        // Track GPU memory allocation
        std::string textureName = "OpenGL Texture2D: " + std::string(path);
        OLO_TRACK_GPU_ALLOC(this,
                            textureMemory,
                            RendererMemoryTracker::ResourceType::Texture2D,
                            textureName);

        // Register with GPU Resource Inspector
        GPUResourceInspector::GetInstance().RegisterTexture(m_RendererID, std::string(path), textureName);

        // NOTE: Texture Wrapping
        glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        // NOTE: Texture Filtering
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_T, GL_REPEAT);

        glTextureSubImage2D(m_RendererID, 0, 0, 0, static_cast<int>(m_Width), static_cast<int>(m_Height), dataFormat, GL_UNSIGNED_BYTE, data);
        glGenerateTextureMipmap(m_RendererID);
    }
    void OpenGLTexture2D::Bind(const u32 slot) const
    {
        OLO_PROFILE_FUNCTION();

        glBindTextureUnit(slot, m_RendererID);

        // Update profiler counters
        RendererProfiler::GetInstance().IncrementCounter(RendererProfiler::MetricType::TextureBinds, 1);
    }

    bool OpenGLTexture2D::GetData(std::vector<u8>& outData, u32 mipLevel) const
    {
        OLO_PROFILE_FUNCTION();

        if (!m_IsLoaded || m_RendererID == 0)
        {
            OLO_CORE_ERROR("OpenGLTexture2D::GetData: Texture not loaded");
            return false;
        }

        if (m_Specification.Samples > 1u)
        {
            OLO_CORE_ERROR("OpenGLTexture2D::GetData: multisample texture readback is not supported");
            return false;
        }

        // Calculate mip dimensions
        u32 mipWidth = std::max(1u, m_Width >> mipLevel);
        u32 mipHeight = std::max(1u, m_Height >> mipLevel);

        // Determine bytes per pixel based on format
        u32 bytesPerPixel = 4;
        GLenum dataType = GL_UNSIGNED_BYTE;

        switch (m_Specification.Format)
        {
            case ImageFormat::R8:
            case ImageFormat::R8UI:
                bytesPerPixel = 1;
                dataType = GL_UNSIGNED_BYTE;
                break;
            case ImageFormat::R16UI:
                bytesPerPixel = 2;
                dataType = GL_UNSIGNED_SHORT;
                break;
            case ImageFormat::RG8:
                bytesPerPixel = 2;
                dataType = GL_UNSIGNED_BYTE;
                break;
            case ImageFormat::RG16UI:
                bytesPerPixel = 4;
                dataType = GL_UNSIGNED_SHORT;
                break;
            case ImageFormat::RG16F:
                bytesPerPixel = 8;
                dataType = GL_FLOAT;
                break;
            case ImageFormat::RGB8:
                bytesPerPixel = 3;
                dataType = GL_UNSIGNED_BYTE;
                break;
            case ImageFormat::RGBA8:
                bytesPerPixel = 4;
                dataType = GL_UNSIGNED_BYTE;
                break;
            case ImageFormat::R32F:
                bytesPerPixel = 4;
                dataType = GL_FLOAT;
                break;
            case ImageFormat::RG32F:
                bytesPerPixel = 8;
                dataType = GL_FLOAT;
                break;
            case ImageFormat::RGB32F:
                bytesPerPixel = 12;
                dataType = GL_FLOAT;
                break;
            case ImageFormat::RGBA32F:
                bytesPerPixel = 16;
                dataType = GL_FLOAT;
                break;
            case ImageFormat::RGBA16F:
                bytesPerPixel = 16;
                dataType = GL_FLOAT;
                break;
            default:
                OLO_CORE_ERROR("OpenGLTexture2D::GetData: Unsupported format for readback");
                return false;
        }

        sizet dataSize = static_cast<sizet>(mipWidth) * mipHeight * bytesPerPixel;
        outData.resize(dataSize);

        // Use DSA glGetTextureImage for readback
        glGetTextureImage(m_RendererID, static_cast<GLint>(mipLevel), m_DataFormat, dataType,
                          static_cast<GLsizei>(dataSize), outData.data());

        GLenum error = glGetError();
        if (error != GL_NO_ERROR)
        {
            OLO_CORE_ERROR("OpenGLTexture2D::GetData: GL error {}", error);
            return false;
        }

        return true;
    }
} // namespace OloEngine

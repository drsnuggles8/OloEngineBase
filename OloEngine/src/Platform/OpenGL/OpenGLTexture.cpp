#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLTexture.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Debug/GPUResourceInspector.h"

#define STB_IMAGE_IMPLEMENTATION
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
                case ImageFormat::RGB8:
                    return GL_RGB;
                case ImageFormat::RGBA8:
                    return GL_RGBA;
                case ImageFormat::R32F:
                    return GL_RED;
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

        [[nodiscard("Store this!")]] static GLenum OloEngineImageFormatToGLInternalFormat(ImageFormat format)
        {
            switch (format)
            {
                case ImageFormat::R8:
                    return GL_R8;
                case ImageFormat::RGB8:
                    return GL_RGB8;
                case ImageFormat::RGBA8:
                    return GL_RGBA8;
                case ImageFormat::R32F:
                    return GL_R32F;
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

    OpenGLTexture2D::OpenGLTexture2D(const TextureSpecification& specification)
        : m_Specification(specification), m_Width(m_Specification.Width), m_Height(m_Specification.Height)
    {
        OLO_PROFILE_FUNCTION();

        m_InternalFormat = Utils::OloEngineImageFormatToGLInternalFormat(m_Specification.Format);
        m_DataFormat = Utils::OloEngineImageFormatToGLDataFormat(m_Specification.Format);
        glCreateTextures(GL_TEXTURE_2D, 1, &m_RendererID);
        glTextureStorage2D(m_RendererID, 1, m_InternalFormat, static_cast<int>(m_Width), static_cast<int>(m_Height));

        // Calculate memory usage based on format and dimensions
        u32 bytesPerPixel = 4; // Default to RGBA
        switch (m_Specification.Format)
        {
            case ImageFormat::R8:
                bytesPerPixel = 1;
                break;
            case ImageFormat::RGB8:
                bytesPerPixel = 3;
                break;
            case ImageFormat::RGBA8:
                bytesPerPixel = 4;
                break;
            case ImageFormat::R32F:
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
        sizet textureMemory = static_cast<sizet>(m_Width) * m_Height * bytesPerPixel;
        // Track GPU memory allocation
        OLO_TRACK_GPU_ALLOC(this,
                            textureMemory,
                            RendererMemoryTracker::ResourceType::Texture2D,
                            "OpenGL Texture2D (spec)");

        // Register with GPU Resource Inspector
        GPUResourceInspector::GetInstance().RegisterTexture(m_RendererID, "Texture2D (spec)", "Texture2D");

        // NOTE: Texture Wrapping
        glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

        // NOTE: Texture Filtering
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }

    OpenGLTexture2D::OpenGLTexture2D(const std::string& path)
    {
        OLO_PROFILE_FUNCTION();

        int width = 0;
        int height = 0;
        int channels = 0;
        ::stbi_set_flip_vertically_on_load(1);
        stbi_uc* data = nullptr;
        {
            OLO_PROFILE_SCOPE("stbi_load - OpenGLTexture2D::OpenGLTexture2D(const std::string&)");
            data = ::stbi_load(path.c_str(), &width, &height, &channels, 0);
        }

        if (!data)
        {
            OLO_CORE_ERROR("Image texture data is null!");
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

        glDeleteTextures(1, &m_RendererID);
    }

    void OpenGLTexture2D::SetData(void* const data, const u32 size)
    {
        OLO_PROFILE_FUNCTION();

        // Calculate bytes per pixel based on the texture's image format
        u32 bpp = 4; // Default to RGBA8
        GLenum dataType = GL_UNSIGNED_BYTE;

        switch (m_Specification.Format)
        {
            case ImageFormat::R8:
                bpp = 1;
                dataType = GL_UNSIGNED_BYTE;
                break;
            case ImageFormat::RGB8:
                bpp = 3;
                dataType = GL_UNSIGNED_BYTE;
                break;
            case ImageFormat::RGBA8:
                bpp = 4;
                dataType = GL_UNSIGNED_BYTE;
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
            case ImageFormat::R32F:
            case ImageFormat::RG32F:
            case ImageFormat::RGB32F:
            case ImageFormat::RGBA32F:
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

        GLenum internalFormat = 0;
        GLenum dataFormat = 0;
        switch (channels)
        {
            case 1:
                internalFormat = GL_R8;
                dataFormat = GL_RED;
                OLO_CORE_TRACE("Texture channel count is 1. Internal format is: {}. Data Format is: {}.", internalFormat, dataFormat);
                break;
            case 2:
                internalFormat = GL_RG8;
                dataFormat = GL_RG;
                OLO_CORE_TRACE("Texture channel count is 2. Internal format is: {}. Data Format is: {}.", internalFormat, dataFormat);
                break;
            case 3:
                internalFormat = GL_RGB8;
                dataFormat = GL_RGB;
                OLO_CORE_TRACE("Texture channel count is 3. Internal format is: {}. Data Format is: {}.", internalFormat, dataFormat);
                break;
            case 4:
                internalFormat = GL_RGBA8;
                dataFormat = GL_RGBA;
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
        OLO_TRACK_GPU_ALLOC(reinterpret_cast<void*>(static_cast<uintptr_t>(m_RendererID)),
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

        // Calculate mip dimensions
        u32 mipWidth = std::max(1u, m_Width >> mipLevel);
        u32 mipHeight = std::max(1u, m_Height >> mipLevel);

        // Determine bytes per pixel based on format
        u32 bytesPerPixel = 4;
        GLenum dataType = GL_UNSIGNED_BYTE;

        switch (m_Specification.Format)
        {
            case ImageFormat::R8:
                bytesPerPixel = 1;
                dataType = GL_UNSIGNED_BYTE;
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

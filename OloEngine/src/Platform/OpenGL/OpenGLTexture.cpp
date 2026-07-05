#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLTexture.h"
#include "Platform/OpenGL/OpenGLUtilities.h"
#include "OloEngine/Renderer/TextureCompression.h"
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
                case ImageFormat::BC7:
                case ImageFormat::BC5:
                    // Compressed formats upload via glCompressedTextureSubImage2D with no
                    // client pixel format; this value is unused for those paths.
                    return 0;
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
                case ImageFormat::BC7:
                    // BPTC. sRGB variant for colour data; UNORM for linear.
                    return srgb ? GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM : GL_COMPRESSED_RGBA_BPTC_UNORM;
                case ImageFormat::BC5:
                    // RGTC2 two-channel; always linear.
                    return GL_COMPRESSED_RG_RGTC2;
            }

            OLO_CORE_ASSERT(false, "Unknown ImageFormat!");
            return 0;
        }

        // Whether the current GL context can accept the given compressed format.
        // BPTC (BC7) is core since GL 4.2, RGTC (BC5) since GL 3.0 — both guaranteed on
        // the engine's required 4.6 context. Still queried so a degraded context (or a
        // future lower-GL backend) falls back to uncompressed instead of erroring.
        [[nodiscard("Store this!")]] static bool IsCompressionSupported(TextureCompressionFormat format)
        {
            switch (format)
            {
                case TextureCompressionFormat::BC7:
                    return (GLAD_GL_VERSION_4_2 != 0) || (GLAD_GL_ARB_texture_compression_bptc != 0);
                case TextureCompressionFormat::BC5:
                    return (GLAD_GL_VERSION_3_0 != 0) || (GLAD_GL_ARB_texture_compression_rgtc != 0);
                case TextureCompressionFormat::None:
                    return false;
            }
            return false;
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

        // Block-compressed formats have no population path through the spec ctor: it would
        // allocate empty compressed storage and report IsLoaded, producing a garbage
        // texture. They MUST be created via the CompressedTextureImage overload.
        if (IsCompressedFormat(m_Specification.Format))
        {
            OLO_CORE_ERROR("OpenGLTexture2D: block-compressed format {} cannot be created from a TextureSpecification — use the CompressedTextureImage overload",
                           static_cast<u32>(m_Specification.Format));
            m_RendererID = 0;
            m_IsLoaded = false;
            return;
        }

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
            case ImageFormat::BC7:
            case ImageFormat::BC5:
                // Block-compressed textures are not created through the spec ctor (use
                // the CompressedTextureImage ctor, which tracks block bytes exactly).
                // ~1 byte/texel is a coarse placeholder purely for this tracking line.
                bytesPerPixel = 1;
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

    bool OpenGLTexture2D::Reload()
    {
        OLO_PROFILE_FUNCTION();

        // Cooked block-compressed containers (.olotex) are build artifacts, not
        // hand-edited live assets, and InvalidateImpl only knows the uncompressed
        // upload path — refuse so the caller replaces the object instead.
        if (IsCompressedFormat(m_Specification.Format))
            return false;

        if (m_Path.empty())
            return false;

        int width = 0;
        int height = 0;
        int channels = 0;
        // Match the path constructor's thread-local flip handling exactly (see the
        // long comment there) so a reloaded texture isn't vertically mirrored.
        ::stbi_set_flip_vertically_on_load_thread(1);
        stbi_uc* data = nullptr;
        {
            OLO_PROFILE_SCOPE("stbi_load - OpenGLTexture2D::Reload");
            data = ::stbi_load(m_Path.c_str(), &width, &height, &channels, 0);
        }
        ::stbi_set_flip_vertically_on_load_thread(0);

        if (!data)
        {
            OLO_CORE_ERROR("OpenGLTexture2D::Reload: failed to re-read texture '{}'", m_Path);
            return false;
        }

        // InvalidateImpl tears down the previous GL texture (its re-entrancy guard) and
        // uploads the new data onto this same object. m_RendererID changes, but the
        // Ref<Texture2D> does not — every consumer picks up the new pixels on next bind.
        InvalidateImpl(m_Path, static_cast<u32>(width), static_cast<u32>(height), data, static_cast<u32>(channels));

        ::stbi_image_free(data);
        return true;
    }

    OpenGLTexture2D::OpenGLTexture2D(const CompressedTextureImage& image)
    {
        OLO_PROFILE_FUNCTION();

        if (!image.IsValid())
        {
            OLO_CORE_ERROR("OpenGLTexture2D: invalid CompressedTextureImage");
            return;
        }

        m_Width = image.Width;
        m_Height = image.Height;
        m_Path = image.SourcePath; // so GetPath() works (pack serializer re-reads the .olotex)
        m_Specification.Width = image.Width;
        m_Specification.Height = image.Height;
        m_Specification.SRGB = image.SRGB;
        m_Specification.Format = (image.Format == TextureCompressionFormat::BC5) ? ImageFormat::BC5 : ImageFormat::BC7;
        m_CompressedHasAlpha = image.HasAlpha;
        m_MipLevels = image.MipLevels();
        m_Specification.MipLevels = m_MipLevels;
        m_Specification.GenerateMips = m_MipLevels > 1u;

        // Fallback: on a context without the required compressed-format support, decode
        // the mip chain on the CPU and upload uncompressed so rendering still works.
        if (!Utils::IsCompressionSupported(image.Format))
        {
            OLO_CORE_WARN("OpenGLTexture2D: driver lacks {} support — falling back to uncompressed RGBA8",
                          image.Format == TextureCompressionFormat::BC5 ? "BC5/RGTC" : "BC7/BPTC");
            UploadDecompressedFallback(image);
            return;
        }

        // Reuse the single ImageFormat->GL internal-format mapping (m_Specification.Format
        // is already BC7/BC5) rather than a second parallel switch.
        m_InternalFormat = Utils::OloEngineImageFormatToGLInternalFormat(m_Specification.Format, image.SRGB);
        m_DataFormat = 0; // compressed: no client pixel format

        glCreateTextures(GL_TEXTURE_2D, 1, &m_RendererID);
        glTextureStorage2D(m_RendererID, static_cast<GLsizei>(m_MipLevels), m_InternalFormat,
                           static_cast<GLsizei>(m_Width), static_cast<GLsizei>(m_Height));

        sizet totalBytes = 0;
        bool baseLevelUploaded = false;
        for (u32 level = 0; level < m_MipLevels; ++level)
        {
            const u32 mipWidth = std::max(1u, m_Width >> level);
            const u32 mipHeight = std::max(1u, m_Height >> level);
            const std::vector<u8>& blocks = image.Mips[level];

            const sizet expected = TextureCompression::MipByteSize(image.Format, mipWidth, mipHeight);
            if (blocks.size() != expected)
            {
                OLO_CORE_ERROR("OpenGLTexture2D: mip {} size {} != expected {} — skipping upload", level, blocks.size(), expected);
                continue;
            }

            glCompressedTextureSubImage2D(m_RendererID, static_cast<GLint>(level), 0, 0,
                                          static_cast<GLsizei>(mipWidth), static_cast<GLsizei>(mipHeight),
                                          m_InternalFormat, static_cast<GLsizei>(blocks.size()), blocks.data());
            totalBytes += blocks.size();
            if (level == 0)
                baseLevelUploaded = true;
        }

        OLO_TRACK_GPU_ALLOC(this, totalBytes, RendererMemoryTracker::ResourceType::Texture2D, "OpenGL Texture2D (compressed)");
        GPUResourceInspector::GetInstance().RegisterTexture(m_RendererID, "Texture2D (compressed)", "Texture2D");

        glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, m_MipLevels > 1u ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_T, GL_REPEAT);

        // Require the base level (mip 0) specifically — a texture whose full-resolution
        // level never uploaded is unusable even if a smaller mip happened to succeed, so a
        // truncated/mismatched blob must not masquerade as a successfully loaded texture.
        m_IsLoaded = baseLevelUploaded;
        if (!m_IsLoaded)
            OLO_CORE_ERROR("OpenGLTexture2D: compressed upload did not produce a valid base mip level");
    }

    void OpenGLTexture2D::UploadDecompressedFallback(const CompressedTextureImage& image)
    {
        // Decode base mip to RGBA8 and upload as a normal texture; regenerate mips on GPU.
        std::vector<u8> rgba;
        u32 w = 0;
        u32 h = 0;
        if (!TextureCompression::DecodeToRGBA8(image, 0, rgba, w, h) || rgba.empty())
        {
            OLO_CORE_ERROR("OpenGLTexture2D: fallback decode failed");
            return;
        }

        const bool wantMips = image.MipLevels() > 1u;
        m_MipLevels = wantMips ? CalculateFullMipCount(w, h) : 1u;
        // Keep m_Specification.Format as the compressed (BC7/BC5) identity — the physical
        // GL upload format lives in m_InternalFormat/m_DataFormat below. Overwriting it
        // with RGBA8 would defeat HasAlphaChannel() (an opaque BC7 / BC5 normal would then
        // report alpha and mis-sort into the transparent pass), unguard Resize/SetData/
        // SubImage, and make the asset-pack serializer treat the texture as uncompressed.
        m_Specification.MipLevels = m_MipLevels;
        m_Specification.GenerateMips = wantMips;
        m_InternalFormat = image.SRGB ? GL_SRGB8_ALPHA8 : GL_RGBA8;
        m_DataFormat = GL_RGBA;

        glCreateTextures(GL_TEXTURE_2D, 1, &m_RendererID);
        glTextureStorage2D(m_RendererID, static_cast<GLsizei>(m_MipLevels), m_InternalFormat,
                           static_cast<GLsizei>(w), static_cast<GLsizei>(h));
        glTextureSubImage2D(m_RendererID, 0, 0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h),
                            GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        if (m_MipLevels > 1u)
            glGenerateTextureMipmap(m_RendererID);

        OLO_TRACK_GPU_ALLOC(this, rgba.size(), RendererMemoryTracker::ResourceType::Texture2D, "OpenGL Texture2D (compressed-fallback)");
        GPUResourceInspector::GetInstance().RegisterTexture(m_RendererID, "Texture2D (compressed-fallback)", "Texture2D");

        glTextureParameteri(m_RendererID, GL_TEXTURE_MIN_FILTER, m_MipLevels > 1u ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTextureParameteri(m_RendererID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTextureParameteri(m_RendererID, GL_TEXTURE_WRAP_T, GL_REPEAT);

        m_IsLoaded = true;
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

        if (m_PBO[0] != 0u)
        {
            GLuint pbo0 = m_PBO[0];
            GLuint pbo1 = m_PBO[1];
            FrameResourceManager::Get().SubmitForDeletion([pbo0, pbo1]()
                                                          { GLuint pbos[2] = { pbo0, pbo1 }; glDeleteBuffers(2, pbos); });
        }
    }

    void OpenGLTexture2D::Resize(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        // Block-compressed textures carry no re-uploadable client data — a resize would
        // recreate empty compressed storage (undefined mips). Reject rather than corrupt.
        if (IsCompressedFormat(m_Specification.Format))
        {
            OLO_CORE_ERROR("OpenGLTexture2D::Resize: not supported for block-compressed textures");
            return;
        }

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

        if (IsCompressedFormat(m_Specification.Format))
        {
            OLO_CORE_ERROR("OpenGLTexture2D::SetData: not supported for block-compressed textures (upload via the CompressedTextureImage ctor)");
            return;
        }

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

        // Streaming path: route the upload through a double-buffered PBO ring so the
        // CPU copy and the GPU DMA overlap instead of stalling the render thread.
        if (m_Specification.Streaming && m_Specification.Samples <= 1u && data)
        {
            if (m_PBO[0] == 0u || m_PBOCapacity != size)
            {
                if (m_PBO[0] == 0u)
                    glCreateBuffers(2, m_PBO);
                glNamedBufferData(m_PBO[0], static_cast<GLsizeiptr>(size), nullptr, GL_STREAM_DRAW);
                glNamedBufferData(m_PBO[1], static_cast<GLsizeiptr>(size), nullptr, GL_STREAM_DRAW);
                m_PBOCapacity = size;
            }

            m_PBOIndex = (m_PBOIndex + 1u) & 1u;
            const GLuint pbo = m_PBO[m_PBOIndex];

            // Orphan + map UNSYNCHRONIZED so the driver hands back fresh storage
            // without waiting on any in-flight DMA from the previous frame.
            glNamedBufferData(pbo, static_cast<GLsizeiptr>(size), nullptr, GL_STREAM_DRAW);
            if (void* ptr = glMapNamedBufferRange(pbo, 0, static_cast<GLsizeiptr>(size),
                                                  GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_UNSYNCHRONIZED_BIT))
            {
                std::memcpy(ptr, data, size);
                glUnmapNamedBuffer(pbo);

                // glTextureSubImage2D sources from the bound GL_PIXEL_UNPACK_BUFFER when
                // the data pointer is a (null) buffer offset. Restore the binding to 0 after.
                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
                glTextureSubImage2D(m_RendererID, 0, 0, 0, static_cast<int>(m_Width), static_cast<int>(m_Height), m_DataFormat, dataType, nullptr);
                glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
                return;
            }
            // Map failed — fall through to the direct (client-memory) upload below.
        }

        glTextureSubImage2D(m_RendererID, 0, 0, 0, static_cast<int>(m_Width), static_cast<int>(m_Height), m_DataFormat, dataType, data);
    }

    void OpenGLTexture2D::SubImage(u32 x, u32 y, u32 width, u32 height, const void* data, [[maybe_unused]] u32 dataSize)
    {
        OLO_PROFILE_FUNCTION();

        if (IsCompressedFormat(m_Specification.Format))
        {
            OLO_CORE_ERROR("OpenGLTexture2D::SubImage: not supported for block-compressed textures");
            return;
        }

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

        // Re-entrancy: InvalidateImpl runs again on an in-place reload (see Reload()).
        // glCreateTextures below overwrites m_RendererID, so release the previous
        // texture first — otherwise every reload leaks a GL texture, its tracker entry,
        // its inspector registration, and leaves a stale slot-binding cache entry.
        // Guarded on a non-zero id so the first-time path (m_RendererID == 0) is a no-op.
        if (m_RendererID != 0)
        {
            OLO_TRACK_DEALLOC(this);
            GPUResourceInspector::GetInstance().UnregisterResource(m_RendererID);
            CommandDispatch::InvalidateTextureBinding(m_RendererID);
            u32 oldId = m_RendererID;
            FrameResourceManager::Get().SubmitForDeletion([oldId]()
                                                          { glDeleteTextures(1, &oldId); });
            m_RendererID = 0;
        }

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

        // Drain leaked GL errors so the check below reflects only this readback
        // (see OpenGLTextureCubemap::GetFaceData for the spurious-failure this prevents).
        Utils::DrainGLErrors();

        // Use DSA glGetTextureImage for readback
        glGetTextureImage(m_RendererID, static_cast<GLint>(mipLevel), m_DataFormat, dataType,
                          static_cast<GLsizei>(dataSize), outData.data());

        if (GLenum error = glGetError(); error != GL_NO_ERROR)
        {
            OLO_CORE_ERROR("OpenGLTexture2D::GetData: GL error {}", error);
            return false;
        }

        return true;
    }
} // namespace OloEngine

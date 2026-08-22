#include "OloEnginePCH.h"
#include "Platform/OpenGL/OpenGLResourceInspectorBackend.h"
#include "OloEngine/Core/Log.h"

#include <glad/gl.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <sstream>

namespace OloEngine
{
    namespace
    {
        // Map a GL format token to the number of channels stb_image_write should receive.
        // Returns 0 for unsupported formats (depth/stencil packed types, etc.). Callers should
        // log and bail when 0 is returned. The RG case is widened to RGB for PNG output because
        // libpng-style 2-channel encoding isn't universally readable by external image tools.
        i32 ChannelsFromGLFormat(GLenum glFormat)
        {
            switch (glFormat)
            {
                case GL_RED:
                case GL_DEPTH_COMPONENT:
                    return 1;
                case GL_RG:
                    return 2;
                case GL_RGB:
                case GL_BGR:
                    return 3;
                case GL_RGBA:
                case GL_BGRA:
                    return 4;
                default:
                    return 0;
            }
        }
    } // namespace

    // ---- Introspection -----------------------------------------------------

    void OpenGLResourceInspectorBackend::QueryTexture(u64 nativeTextureIdWide, bool isCubemap, TextureQuery& outInfo)
    {
        // The interface carries u64 native ids so a VkImage survives the trip
        // (#810); a GL name always fits a GLuint, so narrow once, here.
        const auto nativeTextureId = static_cast<GLuint>(nativeTextureIdWide);
        // Modern OpenGL 4.5+ DSA approach - no texture binding required
        // Initialized: glGetTextureLevelParameteriv leaves its out-param
        // untouched when the call errors (a deleted or wrong-target name), and
        // an uninitialized width/height would propagate as a garbage extent
        // into every consumer of this struct (review finding).
        GLint width = 0;
        GLint height = 0;
        GLint internalFormat = 0;
        // For cubemaps, query the positive X face (they're all the same size)
        glGetTextureLevelParameteriv(nativeTextureId, 0, GL_TEXTURE_WIDTH, &width);
        glGetTextureLevelParameteriv(nativeTextureId, 0, GL_TEXTURE_HEIGHT, &height);
        glGetTextureLevelParameteriv(nativeTextureId, 0, GL_TEXTURE_INTERNAL_FORMAT, &internalFormat);

        outInfo.Width = static_cast<u32>(width);
        outInfo.Height = static_cast<u32>(height);
        outInfo.InternalFormat = static_cast<u32>(internalFormat);

        GLenum pixelFormat = GL_RGBA;
        GLenum dataType = GL_UNSIGNED_BYTE;
        if (!isCubemap)
        {
            // Determine format and type based on internal format
            switch (internalFormat)
            {
                case GL_RGBA8:
                case GL_SRGB8_ALPHA8:
                    pixelFormat = GL_RGBA;
                    dataType = GL_UNSIGNED_BYTE;
                    break;
                case GL_RGB8:
                case GL_SRGB8:
                    pixelFormat = GL_RGB;
                    dataType = GL_UNSIGNED_BYTE;
                    break;
                case GL_RG8:
                    pixelFormat = GL_RG;
                    dataType = GL_UNSIGNED_BYTE;
                    break;
                case GL_R8:
                    pixelFormat = GL_RED;
                    dataType = GL_UNSIGNED_BYTE;
                    break;
                case GL_RGBA16F:
                    pixelFormat = GL_RGBA;
                    dataType = GL_HALF_FLOAT;
                    break;
                case GL_RGB16F:
                    pixelFormat = GL_RGB;
                    dataType = GL_HALF_FLOAT;
                    break;
                case GL_RG16F:
                    pixelFormat = GL_RG;
                    dataType = GL_HALF_FLOAT;
                    break;
                case GL_R16F:
                    pixelFormat = GL_RED;
                    dataType = GL_HALF_FLOAT;
                    break;
                case GL_RGBA32F:
                    pixelFormat = GL_RGBA;
                    dataType = GL_FLOAT;
                    break;
                case GL_RGB32F:
                    pixelFormat = GL_RGB;
                    dataType = GL_FLOAT;
                    break;
                case GL_RG32F:
                    pixelFormat = GL_RG;
                    dataType = GL_FLOAT;
                    break;
                case GL_R32F:
                    pixelFormat = GL_RED;
                    dataType = GL_FLOAT;
                    break;
                case GL_DEPTH_COMPONENT16:
                case GL_DEPTH_COMPONENT24:
                case GL_DEPTH_COMPONENT32:
                    pixelFormat = GL_DEPTH_COMPONENT;
                    dataType = GL_UNSIGNED_INT;
                    break;
                case GL_DEPTH_COMPONENT32F:
                    pixelFormat = GL_DEPTH_COMPONENT;
                    dataType = GL_FLOAT;
                    break;
                case GL_DEPTH24_STENCIL8:
                    pixelFormat = GL_DEPTH_STENCIL;
                    dataType = GL_UNSIGNED_INT_24_8;
                    break;
                case GL_DEPTH32F_STENCIL8:
                    pixelFormat = GL_DEPTH_STENCIL;
                    dataType = GL_FLOAT_32_UNSIGNED_INT_24_8_REV;
                    break;
                default:
                    pixelFormat = GL_RGBA;
                    dataType = GL_UNSIGNED_BYTE;
                    break;
            }
        }
        else
        {
            // Determine format and type based on internal format (same as texture 2D)
            switch (internalFormat)
            {
                case GL_RGBA8:
                case GL_SRGB8_ALPHA8:
                    pixelFormat = GL_RGBA;
                    dataType = GL_UNSIGNED_BYTE;
                    break;
                case GL_RGB8:
                case GL_SRGB8:
                    pixelFormat = GL_RGB;
                    dataType = GL_UNSIGNED_BYTE;
                    break;
                case GL_RG8:
                    pixelFormat = GL_RG;
                    dataType = GL_UNSIGNED_BYTE;
                    break;
                case GL_R8:
                    pixelFormat = GL_RED;
                    dataType = GL_UNSIGNED_BYTE;
                    break;
                case GL_RGBA16F:
                    pixelFormat = GL_RGBA;
                    dataType = GL_HALF_FLOAT;
                    break;
                case GL_RGB16F:
                    pixelFormat = GL_RGB;
                    dataType = GL_HALF_FLOAT;
                    break;
                case GL_RG16F:
                    pixelFormat = GL_RG;
                    dataType = GL_HALF_FLOAT;
                    break;
                case GL_R16F:
                    pixelFormat = GL_RED;
                    dataType = GL_HALF_FLOAT;
                    break;
                case GL_RGBA32F:
                    pixelFormat = GL_RGBA;
                    dataType = GL_FLOAT;
                    break;
                case GL_RGB32F:
                    pixelFormat = GL_RGB;
                    dataType = GL_FLOAT;
                    break;
                case GL_RG32F:
                    pixelFormat = GL_RG;
                    dataType = GL_FLOAT;
                    break;
                case GL_R32F:
                    pixelFormat = GL_RED;
                    dataType = GL_FLOAT;
                    break;
                default:
                    pixelFormat = GL_RGBA;
                    dataType = GL_UNSIGNED_BYTE;
                    break;
            }
        }
        outInfo.PixelFormat = static_cast<u32>(pixelFormat);
        outInfo.DataType = static_cast<u32>(dataType);

        // bytesPerPixel and switch-case removed (now unused)

        // How many mip levels are ALLOCATED, not how many may be sampled.
        // GL_TEXTURE_MAX_LEVEL is the sampling ceiling and defaults to 1000, so
        // reading the level count out of it reported 1001 levels and
        // HasMips == true for every texture in the engine — including the
        // single-level render targets — and then fed that 1001 to
        // CalculateAccurateTextureMemoryUsage, which sums one level per count
        // (review finding).
        GLint allocatedLevels = 0;
        GLint immutableFormat = GL_FALSE;
        glGetTextureParameteriv(nativeTextureId, GL_TEXTURE_IMMUTABLE_FORMAT, &immutableFormat);
        if (immutableFormat != GL_FALSE)
        {
            // Immutable storage (glTextureStorage*) reports its level count exactly.
            glGetTextureParameteriv(nativeTextureId, GL_TEXTURE_IMMUTABLE_LEVELS, &allocatedLevels);
        }
        else
        {
            // Mutable storage has no such query — probe until a level has no
            // image. The walk is bounded by the full chain the base extent
            // could hold as well as by MAX_LEVEL, so the default ceiling of
            // 1000 cannot turn this into a thousand driver round-trips.
            GLint maxLevel = 0;
            glGetTextureParameteriv(nativeTextureId, GL_TEXTURE_MAX_LEVEL, &maxLevel);
            GLint levelLimit = 0;
            for (GLint extent = std::max(width, height); extent > 1; extent /= 2)
            {
                ++levelLimit;
            }
            levelLimit = std::min(maxLevel, levelLimit);
            for (GLint level = 0; level <= levelLimit; ++level)
            {
                GLint levelWidth = 0;
                glGetTextureLevelParameteriv(nativeTextureId, level, GL_TEXTURE_WIDTH, &levelWidth);
                if (levelWidth <= 0)
                {
                    break;
                }
                ++allocatedLevels;
            }
        }
        // A texture whose level 0 query failed (deleted name, wrong target)
        // still owns one nominal level; zero would make the memory estimate 0.
        outInfo.MipLevels = static_cast<u32>(std::max(allocatedLevels, 1));
        outInfo.HasMips = allocatedLevels > 1;

        // Calculate accurate memory usage including compression and mip levels
        // (cubemaps: 6 faces)
        outInfo.MemoryUsage = CalculateAccurateTextureMemoryUsage(nativeTextureId,
                                                                  isCubemap ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D,
                                                                  outInfo.InternalFormat,
                                                                  outInfo.Width, outInfo.Height,
                                                                  outInfo.MipLevels);
    }

    void OpenGLResourceInspectorBackend::QueryBuffer(u64 nativeBufferIdWide, u32 nativeTarget, BufferQuery& outInfo)
    {
        // The interface carries u64 native ids so a VkImage survives the trip
        // (#810); a GL name always fits a GLuint, so narrow once, here.
        const auto nativeBufferId = static_cast<GLuint>(nativeBufferIdWide);
        // Save current buffer binding for this target
        GLint previousBinding = 0;
        const GLenum bindingQuery = GetBufferBindingQuery(nativeTarget);
        glGetIntegerv(bindingQuery, &previousBinding);

        // Bind the buffer temporarily to query its properties
        glBindBuffer(nativeTarget, nativeBufferId);

        GLint size, usage;
        glGetBufferParameteriv(nativeTarget, GL_BUFFER_SIZE, &size);
        glGetBufferParameteriv(nativeTarget, GL_BUFFER_USAGE, &usage);

        outInfo.Size = static_cast<u32>(size);
        outInfo.Usage = static_cast<u32>(usage);
        outInfo.MemoryUsage = static_cast<sizet>(size);

        // Restore previous buffer binding
        glBindBuffer(nativeTarget, previousBinding);
    }

    void OpenGLResourceInspectorBackend::QueryFramebuffer(u64 nativeFramebufferIdWide, FramebufferQuery& outInfo)
    {
        // The interface carries u64 native ids so a VkImage survives the trip
        // (#810); a GL name always fits a GLuint, so narrow once, here.
        const auto nativeFramebufferId = static_cast<GLuint>(nativeFramebufferIdWide);
        // Save current framebuffer binding
        GLint previousBinding;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previousBinding);

        // Bind the framebuffer temporarily to query its properties
        glBindFramebuffer(GL_FRAMEBUFFER, nativeFramebufferId);

        // Check framebuffer status
        outInfo.Status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

        // Query color attachments
        outInfo.ColorAttachmentCount = 0;
        outInfo.ColorAttachmentFormats.clear();

        for (u32 i = 0; i < 8; ++i)
        {
            GLint attachmentType;
            glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i,
                                                  GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &attachmentType);

            if (attachmentType != GL_NONE)
            {
                ++outInfo.ColorAttachmentCount;

                GLint internalFormat;
                glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i,
                                                      GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &internalFormat);
                outInfo.ColorAttachmentFormats.push_back(static_cast<u32>(internalFormat));
            }
        }

        // Check depth attachment
        GLint depthAttachmentType;
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                              GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &depthAttachmentType);
        outInfo.HasDepthAttachment = (depthAttachmentType != GL_NONE);

        if (outInfo.HasDepthAttachment)
        {
            GLint depthFormat;
            glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                                  GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &depthFormat);
            outInfo.DepthAttachmentFormat = static_cast<u32>(depthFormat);
        }

        // Check stencil attachment
        GLint stencilAttachmentType;
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                                              GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &stencilAttachmentType);
        outInfo.HasStencilAttachment = (stencilAttachmentType != GL_NONE);

        if (outInfo.HasStencilAttachment)
        {
            GLint stencilFormat;
            glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                                                  GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &stencilFormat);
            outInfo.StencilAttachmentFormat = static_cast<u32>(stencilFormat);
        }

        // Estimate memory usage (simplified)
        if (outInfo.ColorAttachmentCount > 0)
        {
            // Get dimensions from first color attachment if available
            GLint textureID;
            glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                                  GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &textureID);
            if (textureID != 0)
            {
                GLint width, height;
                glGetTextureLevelParameteriv(textureID, 0, GL_TEXTURE_WIDTH, &width);
                glGetTextureLevelParameteriv(textureID, 0, GL_TEXTURE_HEIGHT, &height);

                outInfo.Width = static_cast<u32>(width);
                outInfo.Height = static_cast<u32>(height);

                // Estimate memory usage (simplified calculation). Widened
                // BEFORE multiplying, not after (review finding): the operands
                // are GLint, so the products were formed in 32 bits and only
                // then cast — a large MRT target can overflow that on its own,
                // and the cast would faithfully preserve the wrapped value.
                const auto pixels = static_cast<sizet>(width) * static_cast<sizet>(height);
                outInfo.MemoryUsage = pixels * 4u * static_cast<sizet>(outInfo.ColorAttachmentCount);
                if (outInfo.HasDepthAttachment)
                    outInfo.MemoryUsage += pixels * 4u;
                if (outInfo.HasStencilAttachment)
                    outInfo.MemoryUsage += pixels;
            }
        }

        // Restore previous framebuffer binding
        glBindFramebuffer(GL_FRAMEBUFFER, previousBinding);
    }

    IResourceInspectorBackend::BufferKind OpenGLResourceInspectorBackend::ClassifyBufferTarget(u32 nativeTarget) const
    {
        // Determine resource type based on target
        switch (nativeTarget)
        {
            case GL_ARRAY_BUFFER:
                return BufferKind::Vertex;
            case GL_ELEMENT_ARRAY_BUFFER:
                return BufferKind::Index;
            case GL_UNIFORM_BUFFER:
                return BufferKind::Uniform;
            default:
                return BufferKind::Vertex; // Default fallback
        }
    }

    u64 OpenGLResourceInspectorBackend::GetBoundTexture2D() const
    {
        GLint currentTexture = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &currentTexture);
        return static_cast<u64>(currentTexture);
    }

    void OpenGLResourceInspectorBackend::GetTextureLevelSize(u64 nativeTextureIdWide, u32 mipLevel, u32& outWidth, u32& outHeight)
    {
        // The interface carries u64 native ids so a VkImage survives the trip
        // (#810); a GL name always fits a GLuint, so narrow once, here.
        const auto nativeTextureId = static_cast<GLuint>(nativeTextureIdWide);
        GLint width = 0;
        GLint height = 0;
        glGetTextureLevelParameteriv(nativeTextureId, static_cast<GLint>(mipLevel), GL_TEXTURE_WIDTH, &width);
        glGetTextureLevelParameteriv(nativeTextureId, static_cast<GLint>(mipLevel), GL_TEXTURE_HEIGHT, &height);
        outWidth = width > 0 ? static_cast<u32>(width) : 0u;
        outHeight = height > 0 ? static_cast<u32>(height) : 0u;
    }

    // ---- Native-enum vocabulary --------------------------------------------

    i32 OpenGLResourceInspectorBackend::ChannelCountForPixelFormat(u32 nativePixelFormat) const
    {
        return ChannelsFromGLFormat(static_cast<GLenum>(nativePixelFormat));
    }

    IResourceInspectorBackend::PixelPrecision OpenGLResourceInspectorBackend::ClassifyPixelDataType(u32 nativeDataType) const
    {
        // Pick the readback precision from the source's native data type rather
        // than hardcoding GL_UNSIGNED_BYTE for everything non-float — that
        // would silently quantise depth (GL_UNSIGNED_INT) to 8 bits and is
        // outright invalid against true integer internal formats.
        //
        // Strategy: 8-bit normalised → read as u8; everything else float-ish
        // or wider-than-byte → read as GL_FLOAT (drivers promote depth/half/
        // 24-bit-depth to normalised float). A Float classification triggers
        // the caller's float→u8 clamp path for PNG output.
        switch (nativeDataType)
        {
            case GL_UNSIGNED_BYTE:
                return PixelPrecision::UnsignedByte;
            case GL_HALF_FLOAT:
            case GL_FLOAT:
            case GL_UNSIGNED_INT: // depth-as-uint → promote to normalised float
                return PixelPrecision::Float;
            default:
                // Packed depth-stencil (GL_UNSIGNED_INT_24_8 /
                // GL_FLOAT_32_UNSIGNED_INT_24_8_REV) is already rejected by
                // ChannelCountForPixelFormat. Anything else here is an integer
                // texture format (R8I, RGBA16UI, …) which QueryTexture
                // doesn't currently classify — bail rather than guess.
                return PixelPrecision::Unsupported;
        }
    }

    bool OpenGLResourceInspectorBackend::IsFloatPixelDataType(u32 nativeDataType) const
    {
        return nativeDataType == GL_FLOAT || nativeDataType == GL_HALF_FLOAT;
    }

    std::string OpenGLResourceInspectorBackend::FormatTextureFormatName(u32 nativeInternalFormat) const
    {
        switch (nativeInternalFormat)
        {
            // 8-bit formats
            case GL_RGBA8:
                return "RGBA8";
            case GL_RGB8:
                return "RGB8";
            case GL_RG8:
                return "RG8";
            case GL_R8:
                return "R8";
            case GL_RGBA8_SNORM:
                return "RGBA8_SNORM";
            case GL_RGB8_SNORM:
                return "RGB8_SNORM";
            case GL_RG8_SNORM:
                return "RG8_SNORM";
            case GL_R8_SNORM:
                return "R8_SNORM";

            // 16-bit formats
            case GL_RGBA16:
                return "RGBA16";
            case GL_RGB16:
                return "RGB16";
            case GL_RG16:
                return "RG16";
            case GL_R16:
                return "R16";
            case GL_RGBA16_SNORM:
                return "RGBA16_SNORM";
            case GL_RGB16_SNORM:
                return "RGB16_SNORM";
            case GL_RG16_SNORM:
                return "RG16_SNORM";
            case GL_R16_SNORM:
                return "R16_SNORM";

            // 32-bit float formats
            case GL_RGBA32F:
                return "RGBA32F";
            case GL_RGB32F:
                return "RGB32F";
            case GL_RG32F:
                return "RG32F";
            case GL_R32F:
                return "R32F";

            // 16-bit float formats
            case GL_RGBA16F:
                return "RGBA16F";
            case GL_RGB16F:
                return "RGB16F";
            case GL_RG16F:
                return "RG16F";
            case GL_R16F:
                return "R16F";

            // Integer formats
            case GL_RGBA32I:
                return "RGBA32I";
            case GL_RGB32I:
                return "RGB32I";
            case GL_RG32I:
                return "RG32I";
            case GL_R32I:
                return "R32I";
            case GL_RGBA16I:
                return "RGBA16I";
            case GL_RGB16I:
                return "RGB16I";
            case GL_RG16I:
                return "RG16I";
            case GL_R16I:
                return "R16I";
            case GL_RGBA8I:
                return "RGBA8I";
            case GL_RGB8I:
                return "RGB8I";
            case GL_RG8I:
                return "RG8I";
            case GL_R8I:
                return "R8I";

            // Unsigned integer formats
            case GL_RGBA32UI:
                return "RGBA32UI";
            case GL_RGB32UI:
                return "RGB32UI";
            case GL_RG32UI:
                return "RG32UI";
            case GL_R32UI:
                return "R32UI";
            case GL_RGBA16UI:
                return "RGBA16UI";
            case GL_RGB16UI:
                return "RGB16UI";
            case GL_RG16UI:
                return "RG16UI";
            case GL_R16UI:
                return "R16UI";
            case GL_RGBA8UI:
                return "RGBA8UI";
            case GL_RGB8UI:
                return "RGB8UI";
            case GL_RG8UI:
                return "RG8UI";
            case GL_R8UI:
                return "R8UI";

            // Depth/stencil formats
            case GL_DEPTH_COMPONENT16:
                return "DEPTH16";
            case GL_DEPTH_COMPONENT24:
                return "DEPTH24";
            case GL_DEPTH_COMPONENT32:
                return "DEPTH32";
            case GL_DEPTH_COMPONENT32F:
                return "DEPTH32F";
            case GL_DEPTH24_STENCIL8:
                return "DEPTH24_STENCIL8";
            case GL_DEPTH32F_STENCIL8:
                return "DEPTH32F_STENCIL8";
            case GL_STENCIL_INDEX8:
                return "STENCIL8";

            // Compressed formats
            case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
                return "DXT1_RGB";
            case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
                return "DXT1_RGBA";
            case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
                return "DXT3";
            case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
                return "DXT5";

            // sRGB formats
            case GL_SRGB8:
                return "sRGB8";
            case GL_SRGB8_ALPHA8:
                return "sRGBA8";

            default:
            {
                std::stringstream ss;
                ss << "Unknown (0x" << std::uppercase << std::hex << nativeInternalFormat << ")";
                return ss.str();
            }
        }
    }

    std::string OpenGLResourceInspectorBackend::FormatBufferUsageName(u32 nativeUsage) const
    {
        switch (nativeUsage)
        {
            case GL_STATIC_DRAW:
                return "STATIC_DRAW";
            case GL_DYNAMIC_DRAW:
                return "DYNAMIC_DRAW";
            case GL_STREAM_DRAW:
                return "STREAM_DRAW";
            case GL_STATIC_READ:
                return "STATIC_READ";
            case GL_DYNAMIC_READ:
                return "DYNAMIC_READ";
            case GL_STREAM_READ:
                return "STREAM_READ";
            case GL_STATIC_COPY:
                return "STATIC_COPY";
            case GL_DYNAMIC_COPY:
                return "DYNAMIC_COPY";
            case GL_STREAM_COPY:
                return "STREAM_COPY";
            default:
            {
                std::stringstream ss;
                ss << "Unknown (0x" << std::hex << nativeUsage << ")";
                return ss.str();
            }
        }
    }

    const char* OpenGLResourceInspectorBackend::GetBufferTargetName(u32 nativeTarget) const
    {
        switch (nativeTarget)
        {
            case GL_ARRAY_BUFFER:
                return "Array Buffer";
            case GL_ELEMENT_ARRAY_BUFFER:
                return "Element Array Buffer";
            case GL_UNIFORM_BUFFER:
                return "Uniform Buffer";
            case GL_SHADER_STORAGE_BUFFER:
                return "Shader Storage Buffer";
            case GL_TRANSFORM_FEEDBACK_BUFFER:
                return "Transform Feedback Buffer";
            case GL_COPY_READ_BUFFER:
                return "Copy Read Buffer";
            case GL_COPY_WRITE_BUFFER:
                return "Copy Write Buffer";
            case GL_PIXEL_PACK_BUFFER:
                return "Pixel Pack Buffer";
            case GL_PIXEL_UNPACK_BUFFER:
                return "Pixel Unpack Buffer";
            case GL_TEXTURE_BUFFER:
                return "Texture Buffer";
            case GL_DRAW_INDIRECT_BUFFER:
                return "Draw Indirect Buffer";
            case GL_DISPATCH_INDIRECT_BUFFER:
                return "Dispatch Indirect Buffer";
            default:
                return "Unknown";
        }
    }

    const char* OpenGLResourceInspectorBackend::FramebufferStatusName(u32 nativeStatus, FramebufferStatusClass& outClass) const
    {
        switch (nativeStatus)
        {
            case GL_FRAMEBUFFER_COMPLETE:
                outClass = FramebufferStatusClass::Complete;
                return "Complete";
            case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
                outClass = FramebufferStatusClass::Incomplete;
                return "Incomplete Attachment";
            case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
                outClass = FramebufferStatusClass::Incomplete;
                return "Missing Attachment";
            case GL_FRAMEBUFFER_UNSUPPORTED:
                outClass = FramebufferStatusClass::Unsupported;
                return "Unsupported";
            default:
                outClass = FramebufferStatusClass::Unknown;
                return "Unknown";
        }
    }

    // ---- Synchronous readback ----------------------------------------------

    bool OpenGLResourceInspectorBackend::ReadTextureLevel(u64 nativeTextureIdWide, bool isCubemap, u32 mipLevel, u32 faceIndex,
                                                          u32 width, u32 height, u32 nativePixelFormat, bool readAsFloat,
                                                          void* dest, sizet destBytes, std::string& outError)
    {
        // The interface carries u64 native ids so a VkImage survives the trip
        // (#810); a GL name always fits a GLuint, so narrow once, here.
        const auto nativeTextureId = static_cast<GLuint>(nativeTextureIdWide);
        const GLenum readType = readAsFloat ? GL_FLOAT : GL_UNSIGNED_BYTE;

        // DSA glGetTextureSubImage: for cubemaps the layer (z) selects the face in the
        // order +X, -X, +Y, -Y, +Z, -Z — same as TEXTURE_CUBE_MAP_POSITIVE_X..NEGATIVE_Z.
        const GLint zOffset = isCubemap ? static_cast<GLint>(faceIndex) : 0;
        // Tight packing — glPixelStore alignment defaults to 4 which would pad odd-width
        // 3-channel rows; force 1 so the buffer matches our row stride calculation.
        GLint prevPackAlignment = 4;
        glGetIntegerv(GL_PACK_ALIGNMENT, &prevPackAlignment);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);

        // Unbind any GL_PIXEL_PACK_BUFFER while reading into a CPU pointer.
        // If a PBO is bound (e.g. mid-RequestTextureDownload), the readBuffer
        // pointer would be reinterpreted as a byte offset into the PBO instead
        // of an address, producing garbage or a crash. Save/restore mirrors the
        // pattern used for PACK_ALIGNMENT above.
        GLint prevPackPBO = 0;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prevPackPBO);
        if (prevPackPBO != 0)
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

        glGetTextureSubImage(nativeTextureId,
                             static_cast<GLint>(mipLevel),
                             0, 0, zOffset,
                             static_cast<GLsizei>(width),
                             static_cast<GLsizei>(height),
                             1,
                             static_cast<GLenum>(nativePixelFormat),
                             readType,
                             static_cast<GLsizei>(destBytes),
                             dest);

        if (prevPackPBO != 0)
            glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(prevPackPBO));
        glPixelStorei(GL_PACK_ALIGNMENT, prevPackAlignment);

        if (GLenum err = glGetError(); err != GL_NO_ERROR)
        {
            outError = std::format("glGetTextureSubImage failed (GL 0x{:X})", err);
            return false;
        }
        return true;
    }

    bool OpenGLResourceInspectorBackend::ReadBufferRange(u64 nativeBufferIdWide, u32 nativeTarget, u32 offset, u32 size, void* dest)
    {
        // The interface carries u64 native ids so a VkImage survives the trip
        // (#810); a GL name always fits a GLuint, so narrow once, here.
        const auto nativeBufferId = static_cast<GLuint>(nativeBufferIdWide);
        // Save current buffer binding for this target
        GLint previousBinding = 0;
        const GLenum bindingQuery = GetBufferBindingQuery(nativeTarget);
        glGetIntegerv(bindingQuery, &previousBinding);

        // Bind buffer and map data
        glBindBuffer(nativeTarget, nativeBufferId);

        bool success = false;
        // The whole buffer is mapped, so [offset, offset+size) must be proven
        // inside it BEFORE the memcpy (review finding): an out-of-range
        // request here is a read past the mapping — a heap fault, not a GL
        // error, and no GL call would report it. Checked in u64 so the sum
        // cannot wrap.
        GLint64 bufferSize = 0;
        glGetBufferParameteri64v(nativeTarget, GL_BUFFER_SIZE, &bufferSize);
        const u64 end = static_cast<u64>(offset) + static_cast<u64>(size);
        if (bufferSize <= 0 || end > static_cast<u64>(bufferSize))
        {
            OLO_CORE_WARN("[GPUResourceInspector] buffer read {}+{} exceeds buffer {} ({} bytes) — refused", offset,
                          size, nativeBufferId, bufferSize);
            glBindBuffer(nativeTarget, previousBinding);
            return false;
        }

        // Map buffer and copy data
        if (const void* data = glMapBuffer(nativeTarget, GL_READ_ONLY); data)
        {
            if (size > 0)
                std::memcpy(dest, static_cast<const u8*>(data) + offset, size);
            glUnmapBuffer(nativeTarget);
            success = true;
        }

        // Restore previous buffer binding
        glBindBuffer(nativeTarget, previousBinding);
        return success;
    }

    // ---- Texture capture ---------------------------------------------------

    bool OpenGLResourceInspectorBackend::QueryCaptureSource(u64 nativeTextureIdWide, u32 mipLevel, CaptureSource& outSource)
    {
        // The interface carries u64 native ids so a VkImage survives the trip
        // (#810); a GL name always fits a GLuint, so narrow once, here.
        const auto nativeTextureId = static_cast<GLuint>(nativeTextureIdWide);
        if (nativeTextureId == 0 || glIsTexture(nativeTextureId) == GL_FALSE)
        {
            outSource.Error = "invalid texture id";
            return false;
        }

        // GL 4.5 DSA: a texture object knows its own target.
        GLint target = 0;
        glGetTextureParameteriv(nativeTextureId, GL_TEXTURE_TARGET, &target);
        // Cube-map arrays encode the glGetTextureSubImage z offset as
        // layer * 6 + face — a single faceOrLayer parameter can't express that
        // contract unambiguously, so reject rather than guess. No engine render
        // target uses cube-map arrays today; split the parameter if one appears.
        if (target == GL_TEXTURE_CUBE_MAP_ARRAY)
        {
            outSource.Error = "cube-map-array textures are not supported (faceOrLayer cannot express layer+face)";
            return false;
        }
        outSource.Layered = target == GL_TEXTURE_CUBE_MAP ||
                            target == GL_TEXTURE_2D_ARRAY || target == GL_TEXTURE_3D;

        GLint width = 0;
        GLint height = 0;
        GLint internalFormat = 0;
        glGetTextureLevelParameteriv(nativeTextureId, static_cast<GLint>(mipLevel), GL_TEXTURE_WIDTH, &width);
        glGetTextureLevelParameteriv(nativeTextureId, static_cast<GLint>(mipLevel), GL_TEXTURE_HEIGHT, &height);
        glGetTextureLevelParameteriv(nativeTextureId, static_cast<GLint>(mipLevel), GL_TEXTURE_INTERNAL_FORMAT, &internalFormat);
        if (width <= 0 || height <= 0)
        {
            outSource.Error = "texture has no storage at the requested mip level";
            return false;
        }
        outSource.FullWidth = static_cast<u32>(width);
        outSource.FullHeight = static_cast<u32>(height);

        // Map the internal format to a readback (format, type). Same table as
        // QueryTexture, except packed depth-stencil reads as depth-only
        // float — glGetTextureSubImage(GL_DEPTH_COMPONENT) legally extracts the
        // depth plane, which is exactly what an inspection capture wants.
        GLenum format = GL_NONE;
        GLenum dataType = GL_NONE;
        bool isDepth = false;
        switch (internalFormat)
        {
            case GL_RGBA8:
            case GL_SRGB8_ALPHA8:
                format = GL_RGBA;
                dataType = GL_UNSIGNED_BYTE;
                break;
            case GL_RGB8:
            case GL_SRGB8:
                format = GL_RGB;
                dataType = GL_UNSIGNED_BYTE;
                break;
            case GL_RG8:
                format = GL_RG;
                dataType = GL_UNSIGNED_BYTE;
                break;
            case GL_R8:
                format = GL_RED;
                dataType = GL_UNSIGNED_BYTE;
                break;
            case GL_RGBA16F:
            case GL_RGBA32F:
                format = GL_RGBA;
                dataType = GL_FLOAT;
                break;
            case GL_RGB16F:
            case GL_RGB32F:
            case GL_R11F_G11F_B10F:
                format = GL_RGB;
                dataType = GL_FLOAT;
                break;
            case GL_RG16F:
            case GL_RG32F:
                format = GL_RG;
                dataType = GL_FLOAT;
                break;
            case GL_R16F:
            case GL_R32F:
                format = GL_RED;
                dataType = GL_FLOAT;
                break;
            case GL_DEPTH_COMPONENT16:
            case GL_DEPTH_COMPONENT24:
            case GL_DEPTH_COMPONENT32:
            case GL_DEPTH_COMPONENT32F:
            case GL_DEPTH24_STENCIL8:
            case GL_DEPTH32F_STENCIL8:
                format = GL_DEPTH_COMPONENT;
                dataType = GL_FLOAT;
                isDepth = true;
                break;
            default:
                outSource.Error = "unsupported internal format 0x" + std::format("{:X}", static_cast<u32>(internalFormat));
                return false;
        }

        outSource.Channels = ChannelsFromGLFormat(format);
        outSource.IsFloat = (dataType == GL_FLOAT);
        outSource.IsDepth = isDepth;
        outSource.PixelFormat = static_cast<u32>(format);
        outSource.FormatName = FormatTextureFormatName(static_cast<u32>(internalFormat));
        return true;
    }

    bool OpenGLResourceInspectorBackend::ReadCaptureRegion(u64 nativeTextureIdWide, u32 mipLevel, u32 faceOrLayer,
                                                           const CaptureSource& source, u32 regionX, u32 regionY,
                                                           u32 regionWidth, u32 regionHeight,
                                                           void* dest, sizet destBytes, std::string& outError)
    {
        // The interface carries u64 native ids so a VkImage survives the trip
        // (#810); a GL name always fits a GLuint, so narrow once, here.
        const auto nativeTextureId = static_cast<GLuint>(nativeTextureIdWide);
        // The rect arrives in top-left-origin coords; GL rows run bottom-up, so
        // rows [Y, Y+H) from the top are GL rows [fullHeight - Y - H, ...).
        // All three are u32, so a region hanging off the bottom underflows to
        // ~4 billion and casts to a garbage GLint (review finding). Refuse
        // instead: a caller asking for rows outside the image has a bug, and
        // silently reading some other part of the texture hides it.
        // Widened to u64 BEFORE the comparison, and both axes checked (review
        // finding — the first cut of this guard compared in u32, so a large
        // regionY could wrap the sum and pass the very check meant to stop it).
        const u64 endY = static_cast<u64>(regionY) + static_cast<u64>(regionHeight);
        const u64 endX = static_cast<u64>(regionX) + static_cast<u64>(regionWidth);
        if (endY > static_cast<u64>(source.FullHeight) || endX > static_cast<u64>(source.FullWidth))
        {
            outError = "capture region x[" + std::to_string(regionX) + ", " + std::to_string(endX) + ") y[" +
                       std::to_string(regionY) + ", " + std::to_string(endY) + ") exceeds " +
                       std::to_string(source.FullWidth) + "x" + std::to_string(source.FullHeight);
            return false;
        }
        const auto glRegionY = static_cast<GLint>(source.FullHeight - regionY - regionHeight);

        // Tight packing + PBO unbind guard — same rationale as ReadTextureLevel
        // (SaveTextureToFile's readback arm).
        GLint prevPackAlignment = 4;
        glGetIntegerv(GL_PACK_ALIGNMENT, &prevPackAlignment);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        GLint prevPackPBO = 0;
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prevPackPBO);
        if (prevPackPBO != 0)
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

        glGetTextureSubImage(nativeTextureId,
                             static_cast<GLint>(mipLevel),
                             static_cast<GLint>(regionX), glRegionY,
                             source.Layered ? static_cast<GLint>(faceOrLayer) : 0,
                             static_cast<GLsizei>(regionWidth), static_cast<GLsizei>(regionHeight), 1,
                             static_cast<GLenum>(source.PixelFormat),
                             source.IsFloat ? GL_FLOAT : GL_UNSIGNED_BYTE,
                             static_cast<GLsizei>(destBytes), dest);

        if (prevPackPBO != 0)
            glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(prevPackPBO));
        glPixelStorei(GL_PACK_ALIGNMENT, prevPackAlignment);

        if (GLenum err = glGetError(); err != GL_NO_ERROR)
        {
            outError = "glGetTextureSubImage failed (GL 0x" + std::format("{:X}", err) + ")";
            return false;
        }
        return true;
    }

    bool OpenGLResourceInspectorBackend::CaptureRowsAreBottomUp() const
    {
        // GL's bottom-left row order is an implementation detail handled inside
        // CaptureTexturePng: the neutral shell flips to PNG top-down
        // orientation after quantisation iff the backend reads bottom-up.
        return true;
    }

    // ---- Async download engine ---------------------------------------------

    bool OpenGLResourceInspectorBackend::BeginTextureDownload(u64 nativeTextureIdWide, bool isCubemap, u32 mipLevel, u32 faceIndex,
                                                              u32 width, u32 height, sizet dataSize, DownloadTicket& outTicket)
    {
        // The interface carries u64 native ids so a VkImage survives the trip
        // (#810); a GL name always fits a GLuint, so narrow once, here.
        const auto nativeTextureId = static_cast<GLuint>(nativeTextureIdWide);
        GLuint pbo;
        glGenBuffers(1, &pbo);

        if (pbo == 0)
        {
            OLO_CORE_WARN("Failed to create PBO for texture download");
            return false;
        }
        // Modern OpenGL 4.5+ approach: Use immutable buffer storage + DSA
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);

        // Use modern immutable buffer storage (OpenGL 4.4+)
        glBufferStorage(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(dataSize), nullptr, GL_MAP_READ_BIT | GL_DYNAMIC_STORAGE_BIT);

        // Modern OpenGL 4.5+ DSA: Direct texture access without state changes.
        // For cubemaps the z-offset selects the face (0..5 = +X,-X,+Y,-Y,+Z,-Z);
        // for 2D textures it must be 0. SaveTextureToFile uses the same convention.
        const GLint zOffset = isCubemap ? static_cast<GLint>(faceIndex) : 0;
        glGetTextureSubImage(nativeTextureId, static_cast<GLint>(mipLevel), 0, 0, zOffset,
                             static_cast<GLsizei>(width), static_cast<GLsizei>(height), 1,
                             GL_RGBA, GL_UNSIGNED_BYTE, static_cast<GLsizei>(dataSize), nullptr);

        // Unbind PBO
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

        // Create modern sync object for better async completion detection (OpenGL 3.2+)
        GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (fence == nullptr)
        {
            OLO_CORE_WARN("Failed to create sync fence for texture download");
            glDeleteBuffers(1, &pbo);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            return false;
        }

        // Restore PBO binding
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

        outTicket.NativeBuffer = static_cast<u32>(pbo);
        outTicket.Fence = fence;
        return true;
    }

    IResourceInspectorBackend::DownloadStatus OpenGLResourceInspectorBackend::PollDownload(const DownloadTicket& ticket)
    {
        // Modern OpenGL 4.5+ approach: Use sync objects for non-blocking completion detection
        // Check fence status without blocking
        const GLenum result = glClientWaitSync(static_cast<GLsync>(ticket.Fence), 0, 0); // 0 timeout = non-blocking

        if (result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED)
        {
            // Download is complete!
            return DownloadStatus::Complete;
        }
        if (result == GL_WAIT_FAILED)
        {
            // Sync object failed - this shouldn't happen but handle gracefully
            return DownloadStatus::Failed;
        }
        // GL_TIMEOUT_EXPIRED means not ready yet - continue to next frame
        return DownloadStatus::Pending;
    }

    const void* OpenGLResourceInspectorBackend::MapDownloadData(const DownloadTicket& ticket, sizet dataSize)
    {
        // Map the PBO to get the downloaded data
        glBindBuffer(GL_PIXEL_PACK_BUFFER, ticket.NativeBuffer);
        // Map the buffer to read the data
        const void* data = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(dataSize), GL_MAP_READ_BIT);
        if (data == nullptr)
        {
            // Clean up
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        }
        return data;
    }

    void OpenGLResourceInspectorBackend::UnmapDownloadData(const DownloadTicket& ticket)
    {
        // Unmap the ticket's OWN buffer, not whatever currently occupies the
        // PIXEL_PACK binding (review finding): map and unmap are separate
        // calls with caller code in between, and any of it may bind another
        // PBO — unmapping the wrong buffer leaves this one mapped forever and
        // corrupts the other one's state. Re-binding is what makes the pair
        // symmetric; the ticket carries the name precisely for this.
        glBindBuffer(GL_PIXEL_PACK_BUFFER, ticket.NativeBuffer);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }

    void OpenGLResourceInspectorBackend::ReleaseDownload(const DownloadTicket& ticket)
    {
        if (ticket.Fence != nullptr)
        {
            glDeleteSync(static_cast<GLsync>(ticket.Fence));
        }
        if (ticket.NativeBuffer != 0)
        {
            GLuint pbo = ticket.NativeBuffer;
            glDeleteBuffers(1, &pbo);
        }
    }

    // ---- ImGui binding -----------------------------------------------------

    u64 OpenGLResourceInspectorBackend::GetImGuiTextureID(u64 nativeTextureIdWide) const
    {
        // The interface carries u64 native ids so a VkImage survives the trip
        // (#810); a GL name always fits a GLuint, so narrow once, here.
        const auto nativeTextureId = static_cast<GLuint>(nativeTextureIdWide);
        // On GL the raw texture name IS the ImTextureID the ImGui GL3 renderer
        // backend consumes — the same seam ImGuiLayer::GetTextureID's GL arm
        // uses. The cast to ImTextureID happens in the neutral shell; no ImGui
        // header here.
        return nativeTextureId;
    }

    // ---- Buffer binding utility --------------------------------------------

    u32 OpenGLResourceInspectorBackend::GetBufferBindingQuery(u32 target)
    {
        switch (target)
        {
            case GL_ARRAY_BUFFER:
                return GL_ARRAY_BUFFER_BINDING;
            case GL_ELEMENT_ARRAY_BUFFER:
                return GL_ELEMENT_ARRAY_BUFFER_BINDING;
            case GL_UNIFORM_BUFFER:
                return GL_UNIFORM_BUFFER_BINDING;
            case GL_SHADER_STORAGE_BUFFER:
                return GL_SHADER_STORAGE_BUFFER_BINDING;
            case GL_TRANSFORM_FEEDBACK_BUFFER:
                return GL_TRANSFORM_FEEDBACK_BUFFER_BINDING;
            case GL_ATOMIC_COUNTER_BUFFER:
                return GL_ATOMIC_COUNTER_BUFFER_BINDING;
            case GL_COPY_READ_BUFFER:
                return GL_COPY_READ_BUFFER_BINDING;
            case GL_COPY_WRITE_BUFFER:
                return GL_COPY_WRITE_BUFFER_BINDING;
            case GL_DISPATCH_INDIRECT_BUFFER:
                return GL_DISPATCH_INDIRECT_BUFFER_BINDING;
            case GL_DRAW_INDIRECT_BUFFER:
                return GL_DRAW_INDIRECT_BUFFER_BINDING;
            case GL_PIXEL_PACK_BUFFER:
                return GL_PIXEL_PACK_BUFFER_BINDING;
            case GL_PIXEL_UNPACK_BUFFER:
                return GL_PIXEL_UNPACK_BUFFER_BINDING;
            case GL_QUERY_BUFFER:
                return GL_QUERY_BUFFER_BINDING;
            case GL_TEXTURE_BUFFER:
                return GL_TEXTURE_BUFFER_BINDING;
            default:
                OLO_CORE_WARN("Unknown buffer target 0x{0:X}, falling back to GL_ARRAY_BUFFER_BINDING", target);
                return GL_ARRAY_BUFFER_BINDING;
        }
    }

    // ---- Texture memory calculation utilities ------------------------------

    sizet OpenGLResourceInspectorBackend::CalculateAccurateTextureMemoryUsage(u32 textureId, u32 target,
                                                                              u32 internalFormat, u32 width,
                                                                              u32 height, u32 mipLevels) const
    {
        sizet totalMemory = 0;

        // Check if format is compressed
        GLint isCompressed = GL_FALSE;
        glGetInternalformativ(target, internalFormat, GL_TEXTURE_COMPRESSED, 1, &isCompressed);

        if (isCompressed == GL_TRUE)
        {
            // Handle compressed textures - calculate based on block sizes
            totalMemory = CalculateCompressedTextureMemory(textureId, target, internalFormat, width, height, mipLevels);
        }
        else
        {
            // Handle uncompressed textures - calculate based on bytes per pixel
            u32 bytesPerPixel = GetUncompressedBytesPerPixel(internalFormat);
            totalMemory = CalculateUncompressedTextureMemory(width, height, bytesPerPixel, mipLevels);

            // For cubemaps, multiply by 6 faces
            if (target == GL_TEXTURE_CUBE_MAP)
            {
                totalMemory *= 6;
            }
        }

        return totalMemory;
    }

    sizet OpenGLResourceInspectorBackend::CalculateCompressedTextureMemory(u32 textureId, u32 target,
                                                                           u32 internalFormat, u32 /*width*/,
                                                                           u32 /*height*/, u32 mipLevels) const
    {
        sizet totalMemory = 0;
        u32 blockSize = GetCompressedBlockSize(internalFormat);

        // Determine number of faces
        u32 faceCount = (target == GL_TEXTURE_CUBE_MAP) ? 6 : 1;

        for (u32 face = 0; face < faceCount; ++face)
        {
            for (u32 level = 0; level < mipLevels; ++level)
            {
                // Get actual dimensions for this mip level and face
                // Initialized: a failed query leaves its out-param untouched,
                // and an uninitialized extent would be added to the total.
                GLint levelWidth = 0;
                GLint levelHeight = 0;
                GLint compressedSize = 0;

                // One query path for both targets. It used to be an
                // if/else whose two arms were textually identical (a
                // cubemap arm that intended per-face queries and never
                // implemented them, then lost its faceTarget) — flagged by
                // both the PR review and cpp:S3923. DSA queries address the
                // texture OBJECT, so a cube's faces all report the same
                // level extents anyway; the face LOOP above is what
                // multiplies the total by six.
                glGetTextureLevelParameteriv(textureId, static_cast<GLint>(level), GL_TEXTURE_WIDTH, &levelWidth);
                glGetTextureLevelParameteriv(textureId, static_cast<GLint>(level), GL_TEXTURE_HEIGHT, &levelHeight);
                glGetTextureLevelParameteriv(textureId, static_cast<GLint>(level), GL_TEXTURE_COMPRESSED_IMAGE_SIZE,
                                             &compressedSize);

                if (levelWidth > 0 && levelHeight > 0)
                {
                    // Use actual compressed size if available, otherwise calculate
                    if (compressedSize > 0)
                    {
                        totalMemory += static_cast<sizet>(compressedSize);
                    }
                    else
                    {
                        // Calculate based on block compression
                        u32 blocksX = (static_cast<u32>(levelWidth) + 3) / 4;
                        u32 blocksY = (static_cast<u32>(levelHeight) + 3) / 4;
                        // Same widening rule as the uncompressed path below.
                        totalMemory += static_cast<sizet>(blocksX) * static_cast<sizet>(blocksY) *
                                       static_cast<sizet>(blockSize);
                    }
                }
            }
        }

        return totalMemory;
    }

    sizet OpenGLResourceInspectorBackend::CalculateUncompressedTextureMemory(u32 width, u32 height,
                                                                             u32 bytesPerPixel, u32 mipLevels) const
    {
        sizet totalMemory = 0;
        u32 currentWidth = width;
        u32 currentHeight = height;

        for (u32 level = 0; level < mipLevels; ++level)
        {
            // Widened before multiplying, not after (review finding): the
            // operands are u32, so an 8K RGBA32F mip 0 (7680*4320*16) already
            // needs 40 bits and the cast would faithfully store the wrapped
            // product.
            totalMemory += static_cast<sizet>(currentWidth) * static_cast<sizet>(currentHeight) *
                           static_cast<sizet>(bytesPerPixel);

            // Calculate next mip level dimensions
            currentWidth = std::max(1u, currentWidth / 2);
            currentHeight = std::max(1u, currentHeight / 2);
        }

        return totalMemory;
    }

    u32 OpenGLResourceInspectorBackend::GetUncompressedBytesPerPixel(u32 internalFormat) const
    {
        switch (internalFormat)
        {
            // 8-bit single channel
            case GL_R8:
            case GL_R8_SNORM:
            case GL_R8I:
            case GL_R8UI:
                return 1;

            // 16-bit single channel or 8-bit dual channel
            case GL_RG8:
            case GL_RG8_SNORM:
            case GL_RG8I:
            case GL_RG8UI:
            case GL_R16:
            case GL_R16F:
            case GL_R16I:
            case GL_R16UI:
            case GL_DEPTH_COMPONENT16:
                return 2;

            // 24-bit RGB
            case GL_RGB8:
            case GL_RGB8_SNORM:
            case GL_RGB8I:
            case GL_RGB8UI:
            case GL_SRGB8:
            case GL_DEPTH_COMPONENT24:
                return 3;

            // 32-bit formats (RGBA8, RG16, R32, depth32)
            case GL_RGBA8:
            case GL_RGBA8_SNORM:
            case GL_RGBA8I:
            case GL_RGBA8UI:
            case GL_SRGB8_ALPHA8:
            case GL_RG16:
            case GL_RG16F:
            case GL_RG16I:
            case GL_RG16UI:
            case GL_R32F:
            case GL_R32I:
            case GL_R32UI:
            case GL_DEPTH_COMPONENT32:
            case GL_DEPTH_COMPONENT32F:
            case GL_DEPTH24_STENCIL8:
                return 4;

            // 48-bit RGB16
            case GL_RGB16:
            case GL_RGB16F:
            case GL_RGB16I:
            case GL_RGB16UI:
                return 6;

            // 64-bit formats (RGBA16, RG32, depth32f+stencil8)
            case GL_RGBA16:
            case GL_RGBA16F:
            case GL_RGBA16I:
            case GL_RGBA16UI:
            case GL_RG32F:
            case GL_RG32I:
            case GL_RG32UI:
            case GL_DEPTH32F_STENCIL8:
                return 8;

            // 96-bit RGB32
            case GL_RGB32F:
            case GL_RGB32I:
            case GL_RGB32UI:
                return 12;

            // 128-bit RGBA32
            case GL_RGBA32F:
            case GL_RGBA32I:
            case GL_RGBA32UI:
                return 16;

            default:
                OLO_CORE_WARN("GPUResourceInspector: Unknown texture format 0x{:X}, assuming 4 bytes per pixel", internalFormat);
                return 4;
        }
    }

    u32 OpenGLResourceInspectorBackend::GetCompressedBlockSize(u32 internalFormat) const
    {
        switch (internalFormat)
        {
            // DXT1/BC1 - 4x4 blocks, 8 bytes per block (RGB or RGBA with 1-bit alpha)
            case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
            case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
            case GL_COMPRESSED_SRGB_S3TC_DXT1_EXT:
            case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT:
                return 8;

            // DXT3/BC2 - 4x4 blocks, 16 bytes per block (RGBA with explicit alpha)
            case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
            case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT:
                return 16;

            // DXT5/BC3 - 4x4 blocks, 16 bytes per block (RGBA with interpolated alpha)
            case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
            case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT:
                return 16;

            // BC4/ATI1 - 4x4 blocks, 8 bytes per block (single channel)
            case GL_COMPRESSED_RED_RGTC1:
            case GL_COMPRESSED_SIGNED_RED_RGTC1:
                return 8;

            // BC5/ATI2 - 4x4 blocks, 16 bytes per block (dual channel)
            case GL_COMPRESSED_RG_RGTC2:
            case GL_COMPRESSED_SIGNED_RG_RGTC2:
                return 16;

            // BC6H - 4x4 blocks, 16 bytes per block (HDR RGB)
            case GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT:
            case GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT:
                return 16;

            // BC7 - 4x4 blocks, 16 bytes per block (high quality RGBA)
            case GL_COMPRESSED_RGBA_BPTC_UNORM:
            case GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM:
                return 16;

            // ETC2 formats - 4x4 blocks
            case GL_COMPRESSED_RGB8_ETC2:
            case GL_COMPRESSED_SRGB8_ETC2:
            case GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2:
            case GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2:
                return 8;

            case GL_COMPRESSED_RGBA8_ETC2_EAC:
            case GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC:
                return 16;

            // EAC formats - 4x4 blocks
            case GL_COMPRESSED_R11_EAC:
            case GL_COMPRESSED_SIGNED_R11_EAC:
                return 8;

            case GL_COMPRESSED_RG11_EAC:
            case GL_COMPRESSED_SIGNED_RG11_EAC:
                return 16;

            // ASTC formats - variable block sizes (using 4x4 as most common)
            case GL_COMPRESSED_RGBA_ASTC_4x4_KHR:
            case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4_KHR:
                return 16;

            default:
                OLO_CORE_WARN("GPUResourceInspector: Unknown compressed format 0x{:X}, assuming 16 bytes per block", internalFormat);
                return 16;
        }
    }
} // namespace OloEngine

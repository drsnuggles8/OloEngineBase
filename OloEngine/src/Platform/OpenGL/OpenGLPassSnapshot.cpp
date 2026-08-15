// =============================================================================
// OpenGLPassSnapshot.cpp
//
// GL definitions for the RenderGraphPassSnapshot clone engine declared in
// Renderer/Debug/PassSnapshotBackend.h (#691 Phase 9, ADR 0011 §1.6). The
// orchestration — request lifecycle, scratch-slot cache, result bookkeeping —
// stays in Renderer/Debug/RenderGraphPassSnapshot.cpp; every glad call of the
// bitwise clone lives here, in native u32 currency end to end (see the seam
// header for why no handle can exist on this path).
// =============================================================================

#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Debug/PassSnapshotBackend.h"

#include "Platform/OpenGL/OpenGLUtilities.h"

#include <glad/gl.h>

#include <algorithm>

namespace OloEngine::Detail
{
    namespace
    {
        // Mip count of a texture object: immutable storage reports it
        // directly; mutable storage is probed level-by-level (an unallocated
        // level reports width 0).
        u32 QueryMipLevels(const u32 textureId)
        {
            GLint immutableLevels = 0;
            glGetTextureParameteriv(textureId, GL_TEXTURE_IMMUTABLE_LEVELS, &immutableLevels);
            if (immutableLevels > 0)
                return static_cast<u32>(immutableLevels);

            u32 levels = 0;
            for (u32 level = 0; level < 16u; ++level)
            {
                GLint levelWidth = 0;
                glGetTextureLevelParameteriv(textureId, static_cast<GLint>(level), GL_TEXTURE_WIDTH, &levelWidth);
                if (levelWidth <= 0)
                    break;
                ++levels;
            }
            return std::max(levels, 1u);
        }
    } // namespace

    NativeTextureCloneInfo QueryNativeTextureCloneInfo(u32 textureId)
    {
        NativeTextureCloneInfo info;
        if (textureId == 0u || glIsTexture(textureId) == GL_FALSE)
        {
            return info; // IsTexture stays false
        }
        info.IsTexture = true;

        GLint glTarget = 0;
        glGetTextureParameteriv(textureId, GL_TEXTURE_TARGET, &glTarget);
        info.Target = static_cast<u32>(glTarget);

        GLint samples = 0;
        glGetTextureLevelParameteriv(textureId, 0, GL_TEXTURE_SAMPLES, &samples);
        info.Samples = static_cast<i32>(samples);
        if (samples > 1)
        {
            // Multisample sources cannot be cloned by this path; leave the
            // storage description defaulted — the caller rejects on Samples.
            return info;
        }

        GLint width = 0;
        GLint height = 0;
        GLint depth = 0;
        GLint internalFormat = 0;
        glGetTextureLevelParameteriv(textureId, 0, GL_TEXTURE_WIDTH, &width);
        glGetTextureLevelParameteriv(textureId, 0, GL_TEXTURE_HEIGHT, &height);
        glGetTextureLevelParameteriv(textureId, 0, GL_TEXTURE_DEPTH, &depth);
        glGetTextureLevelParameteriv(textureId, 0, GL_TEXTURE_INTERNAL_FORMAT, &internalFormat);
        info.Width = static_cast<u32>(std::max(width, 0));
        info.Height = static_cast<u32>(std::max(height, 0));
        info.InternalFormat = static_cast<u32>(internalFormat);

        // glCopyImageSubData addresses a cube map as 6 array layers.
        u32 depthOrLayers = std::max(static_cast<u32>(std::max(depth, 0)), 1u);
        if (glTarget == GL_TEXTURE_CUBE_MAP)
            depthOrLayers = 6u;
        info.DepthOrLayers = depthOrLayers;

        info.MipLevels = QueryMipLevels(textureId);
        return info;
    }

    u32 CreateNativeScratchTexture(u32 target, u32 internalFormat,
                                   u32 width, u32 height, u32 depthOrLayers,
                                   u32 mipLevels)
    {
        u32 texture = 0;
        glCreateTextures(static_cast<GLenum>(target), 1, &texture);
        switch (target)
        {
            case GL_TEXTURE_2D:
            case GL_TEXTURE_CUBE_MAP:
                glTextureStorage2D(texture, static_cast<GLsizei>(mipLevels), static_cast<GLenum>(internalFormat),
                                   static_cast<GLsizei>(width), static_cast<GLsizei>(height));
                break;
            case GL_TEXTURE_2D_ARRAY:
            case GL_TEXTURE_3D:
            case GL_TEXTURE_CUBE_MAP_ARRAY:
                glTextureStorage3D(texture, static_cast<GLsizei>(mipLevels), static_cast<GLenum>(internalFormat),
                                   static_cast<GLsizei>(width), static_cast<GLsizei>(height),
                                   static_cast<GLsizei>(depthOrLayers));
                break;
            default:
                glDeleteTextures(1, &texture);
                return 0;
        }

        // NEAREST filters: an INTEGER-format texture (the R32I entity-id
        // buffer) is texture-INcomplete under the default LINEAR filters
        // (GL 4.6 §8.17), and glCopyImageSubData mandates INVALID_OPERATION
        // on an incomplete texture (§18.3.2) — NVIDIA is lenient, other
        // drivers are not. Harmless for every other format; the scratch is
        // never shader-sampled (readback-only).
        glTextureParameteri(texture, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTextureParameteri(texture, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        if (glGetError() != GL_NO_ERROR)
        {
            glDeleteTextures(1, &texture);
            return 0;
        }

        return texture;
    }

    void DeleteNativeTexture(u32 textureId)
    {
        if (textureId != 0)
            glDeleteTextures(1, &textureId);
    }

    u32 CopyNativeTextureAllMips(u32 sourceId, u32 scratchId, u32 target,
                                 u32 width, u32 height, u32 depthOrLayers,
                                 u32 mipLevels)
    {
        for (u32 mip = 0; mip < mipLevels; ++mip)
        {
            const auto mipWidth = std::max(width >> mip, 1u);
            const auto mipHeight = std::max(height >> mip, 1u);
            // A 3D volume's depth halves per mip; array layers / cube faces
            // stay constant.
            const u32 mipDepth = (target == GL_TEXTURE_3D) ? std::max(depthOrLayers >> mip, 1u) : depthOrLayers;
            glCopyImageSubData(sourceId, static_cast<GLenum>(target), static_cast<GLint>(mip), 0, 0, 0,
                               scratchId, static_cast<GLenum>(target), static_cast<GLint>(mip), 0, 0, 0,
                               static_cast<GLsizei>(mipWidth), static_cast<GLsizei>(mipHeight),
                               static_cast<GLsizei>(mipDepth));
        }

        return static_cast<u32>(glGetError());
    }

    void DrainNativeErrors()
    {
        Utils::DrainGLErrors();
    }
} // namespace OloEngine::Detail

#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Debug/RenderGraphPassSnapshot.h"

#include "OloEngine/Renderer/RenderGraph.h"

#include <glad/gl.h>

#include <algorithm>
#include <format>

namespace OloEngine
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

    RenderGraphPassSnapshot::~RenderGraphPassSnapshot()
    {
        // Deliberately no GL calls: the instance lives as a process-static
        // (RenderGraphDebugRuntime), destroyed after the GL context. The real
        // cleanup site is ReleaseScratch(), invoked from renderer shutdown via
        // RenderGraphDebugRuntime::SetActiveGraph(nullptr).
    }

    void RenderGraphPassSnapshot::Arm(RenderGraph* graph, std::string passName, std::vector<Request> requests)
    {
        if (m_InstalledGraph && m_InstalledGraph != graph)
            m_InstalledGraph->RemovePostPassHook(kPostPassHookKey);

        m_InstalledGraph = graph;
        m_PassName = std::move(passName);
        m_Requests = std::move(requests);
        m_Results.clear();
        m_Pending = graph != nullptr && !m_Requests.empty();

        if (graph)
        {
            graph->AddPostPassHook(kPostPassHookKey,
                                   [this](const std::string& executedPass, RenderGraph& g)
                                   { this->OnPassExecuted(executedPass, g); });
        }
    }

    void RenderGraphPassSnapshot::Disarm()
    {
        if (m_InstalledGraph)
            m_InstalledGraph->RemovePostPassHook(kPostPassHookKey);
        m_InstalledGraph = nullptr;
        m_Pending = false;
        m_Requests.clear();
    }

    void RenderGraphPassSnapshot::ReleaseScratch()
    {
        for (auto& slot : m_Scratch)
        {
            if (slot.Texture != 0)
                glDeleteTextures(1, &slot.Texture);
        }
        m_Scratch.clear();
        m_Results.clear();
    }

    u32 RenderGraphPassSnapshot::AcquireScratch(const sizet slot, const u32 glTarget, const u32 glInternalFormat,
                                                const u32 width, const u32 height, const u32 depthOrLayers,
                                                const u32 mipLevels)
    {
        if (slot >= m_Scratch.size())
            m_Scratch.resize(slot + 1u);

        ScratchSlot& scratch = m_Scratch[slot];
        if (scratch.Texture != 0 && scratch.Target == glTarget && scratch.Format == glInternalFormat &&
            scratch.Width == width && scratch.Height == height && scratch.Depth == depthOrLayers &&
            scratch.Mips == mipLevels)
        {
            return scratch.Texture;
        }

        if (scratch.Texture != 0)
        {
            glDeleteTextures(1, &scratch.Texture);
            scratch = {};
        }

        u32 texture = 0;
        glCreateTextures(glTarget, 1, &texture);
        switch (glTarget)
        {
            case GL_TEXTURE_2D:
            case GL_TEXTURE_CUBE_MAP:
                glTextureStorage2D(texture, static_cast<GLsizei>(mipLevels), glInternalFormat,
                                   static_cast<GLsizei>(width), static_cast<GLsizei>(height));
                break;
            case GL_TEXTURE_2D_ARRAY:
            case GL_TEXTURE_3D:
            case GL_TEXTURE_CUBE_MAP_ARRAY:
                glTextureStorage3D(texture, static_cast<GLsizei>(mipLevels), glInternalFormat,
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

        scratch.Texture = texture;
        scratch.Target = glTarget;
        scratch.Format = glInternalFormat;
        scratch.Width = width;
        scratch.Height = height;
        scratch.Depth = depthOrLayers;
        scratch.Mips = mipLevels;
        return texture;
    }

    void RenderGraphPassSnapshot::CaptureOne(const sizet slot, const Request& request, Result& out)
    {
        out.ResourceName = request.ResourceName;

        const u32 sourceId = request.Resolve ? request.Resolve() : 0u;
        if (sourceId == 0u || glIsTexture(sourceId) == GL_FALSE)
        {
            out.Error = "Resource '" + request.ResourceName + "' did not resolve to a GL texture after pass '" +
                        m_PassName + "' (wrong rendering path, effect disabled, or no GPU backing).";
            return;
        }
        out.SourceTextureID = sourceId;

        GLint glTarget = 0;
        glGetTextureParameteriv(sourceId, GL_TEXTURE_TARGET, &glTarget);
        GLint samples = 0;
        glGetTextureLevelParameteriv(sourceId, 0, GL_TEXTURE_SAMPLES, &samples);
        if (samples > 1)
        {
            out.Error = "'" + request.ResourceName +
                        "' is multisampled; afterPass snapshots support single-sample textures only.";
            return;
        }

        GLint width = 0;
        GLint height = 0;
        GLint depth = 0;
        GLint internalFormat = 0;
        glGetTextureLevelParameteriv(sourceId, 0, GL_TEXTURE_WIDTH, &width);
        glGetTextureLevelParameteriv(sourceId, 0, GL_TEXTURE_HEIGHT, &height);
        glGetTextureLevelParameteriv(sourceId, 0, GL_TEXTURE_DEPTH, &depth);
        glGetTextureLevelParameteriv(sourceId, 0, GL_TEXTURE_INTERNAL_FORMAT, &internalFormat);
        if (width <= 0 || height <= 0)
        {
            out.Error = "'" + request.ResourceName + "' has no storage at mip 0.";
            return;
        }

        // glCopyImageSubData addresses a cube map as 6 array layers.
        u32 depthOrLayers = std::max(static_cast<u32>(depth), 1u);
        if (glTarget == GL_TEXTURE_CUBE_MAP)
            depthOrLayers = 6u;

        const u32 mipLevels = QueryMipLevels(sourceId);
        const u32 scratch = AcquireScratch(slot, static_cast<u32>(glTarget), static_cast<u32>(internalFormat),
                                           static_cast<u32>(width), static_cast<u32>(height), depthOrLayers,
                                           mipLevels);
        if (scratch == 0u)
        {
            out.Error = "Could not allocate a snapshot clone for '" + request.ResourceName + "' (GL target 0x" +
                        std::format("{:X}", static_cast<u32>(glTarget)) + ").";
            return;
        }

        for (u32 mip = 0; mip < mipLevels; ++mip)
        {
            const auto mipWidth = std::max(static_cast<u32>(width) >> mip, 1u);
            const auto mipHeight = std::max(static_cast<u32>(height) >> mip, 1u);
            // A 3D volume's depth halves per mip; array layers / cube faces
            // stay constant.
            const u32 mipDepth = (glTarget == GL_TEXTURE_3D) ? std::max(depthOrLayers >> mip, 1u) : depthOrLayers;
            glCopyImageSubData(sourceId, static_cast<GLenum>(glTarget), static_cast<GLint>(mip), 0, 0, 0,
                               scratch, static_cast<GLenum>(glTarget), static_cast<GLint>(mip), 0, 0, 0,
                               static_cast<GLsizei>(mipWidth), static_cast<GLsizei>(mipHeight),
                               static_cast<GLsizei>(mipDepth));
        }

        if (const GLenum error = glGetError(); error != GL_NO_ERROR)
        {
            out.Error = "glCopyImageSubData on '" + request.ResourceName + "' failed (GL 0x" +
                        std::format("{:X}", static_cast<u32>(error)) + ").";
            return;
        }

        out.Captured = true;
        out.TextureID = scratch;
        out.Width = static_cast<u32>(width);
        out.Height = static_cast<u32>(height);
        out.DepthOrLayers = depthOrLayers;
        out.MipLevels = mipLevels;
        out.GLInternalFormat = static_cast<u32>(internalFormat);
        out.GLTarget = static_cast<u32>(glTarget);
    }

    void RenderGraphPassSnapshot::OnPassExecuted(const std::string& passName, RenderGraph& /*graph*/)
    {
        if (!m_Pending || passName != m_PassName)
            return;

        // One-shot: whatever happens below, this request is consumed.
        m_Pending = false;
        m_Results.clear();
        m_Results.resize(m_Requests.size());

        // Drain any stale error so the checks in CaptureOne attribute
        // failures to THIS copy, not to whatever the executing pass left.
        while (glGetError() != GL_NO_ERROR)
        {
        }

        for (sizet i = 0; i < m_Requests.size(); ++i)
            CaptureOne(i, m_Requests[i], m_Results[i]);
    }
} // namespace OloEngine

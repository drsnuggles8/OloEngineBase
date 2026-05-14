#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Debug/RenderGraphFrameCapture.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/GBuffer.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include <algorithm>
#include <array>
#include <glad/gl.h>
#include <limits>
#include <string_view>

namespace OloEngine
{
    namespace
    {
        // Single shared blit destination FBO — re-used across all
        // captures by re-attaching the per-capture texture as
        // COLOR_ATTACHMENT0 before each blit.
        u32 s_BlitDstFBO = 0;
        // Single shared blit source FBO — used to wrap the source FB's
        // color attachment when only the renderer ID is available.
        u32 s_BlitSrcFBO = 0;

        u32 EnsureBlitFBO(u32& fbo)
        {
            if (fbo == 0)
            {
                glCreateFramebuffers(1, &fbo);
            }
            return fbo;
        }

        bool IsPresentationLikeSource(const RenderGraphFrameCapture::Source source)
        {
            using S = RenderGraphFrameCapture::Source;
            switch (source)
            {
                case S::SceneColor:
                case S::SSSColor:
                case S::OITResolveColor:
                case S::AOApplyColor:
                case S::BloomColor:
                case S::DOFColor:
                case S::MotionBlurColor:
                case S::TAAColor:
                case S::PrecipitationColor:
                case S::FogColor:
                case S::ChromAbColor:
                case S::ColorGradingColor:
                case S::ToneMapColor:
                case S::VignetteColor:
                case S::FXAAColor:
                case S::SelectionOutlineColor:
                case S::UIComposite:
                case S::Backbuffer:
                    return true;
                default:
                    return false;
            }
        }

        bool IsDepthFormat(FramebufferTextureFormat format)
        {
            return format == FramebufferTextureFormat::DEPTH24STENCIL8 ||
                   format == FramebufferTextureFormat::DEPTH_COMPONENT32F;
        }

        u32 CountColorAttachments(const FramebufferSpecification& spec)
        {
            u32 count = 0;
            for (const auto& attachment : spec.Attachments.Attachments)
            {
                if (!IsDepthFormat(attachment.TextureFormat))
                {
                    ++count;
                }
            }
            return count;
        }
    } // namespace

    RenderGraphFrameCapture::~RenderGraphFrameCapture()
    {
        ClearCaptures();
        if (s_BlitDstFBO != 0)
        {
            glDeleteFramebuffers(1, &s_BlitDstFBO);
            s_BlitDstFBO = 0;
        }
        if (s_BlitSrcFBO != 0)
        {
            glDeleteFramebuffers(1, &s_BlitSrcFBO);
            s_BlitSrcFBO = 0;
        }
    }

    const char* RenderGraphFrameCapture::SourceName(Source s)
    {
        switch (s)
        {
            case Source::SceneColor:
                return "SceneColor";
            case Source::GBufferAlbedo:
                return "GBufferAlbedo";
            case Source::GBufferNormal:
                return "GBufferNormal";
            case Source::GBufferEmissive:
                return "GBufferEmissive";
            case Source::Velocity:
                return "Velocity";
            case Source::SceneNormals:
                return "SceneNormals";
            case Source::HZBDepth:
                return "HZBDepth";
            case Source::SSSColor:
                return "SSSColor";
            case Source::OITResolveColor:
                return "OITResolveColor";
            case Source::AOTexture:
                return "AOTexture";
            case Source::AOApplyColor:
                return "AOApplyColor";
            case Source::BloomColor:
                return "BloomColor";
            case Source::DOFColor:
                return "DOFColor";
            case Source::MotionBlurColor:
                return "MotionBlurColor";
            case Source::TAAColor:
                return "TAAColor";
            case Source::PrecipitationColor:
                return "PrecipitationColor";
            case Source::FogColor:
                return "FogColor";
            case Source::ChromAbColor:
                return "ChromAbColor";
            case Source::ColorGradingColor:
                return "ColorGradingColor";
            case Source::ToneMapColor:
                return "ToneMapColor";
            case Source::VignetteColor:
                return "VignetteColor";
            case Source::FXAAColor:
                return "FXAAColor";
            case Source::SelectionOutlineColor:
                return "SelectionOutline";
            case Source::UIComposite:
                return "UIComposite";
            case Source::Backbuffer:
                return "Backbuffer";
            case Source::COUNT:
            default:
                return "Unknown";
        }
    }

    void RenderGraphFrameCapture::InstallHook(RenderGraph* graph)
    {
        if (m_InstalledGraph == graph)
        {
            return;
        }

        if (m_InstalledGraph)
        {
            m_InstalledGraph->SetPostPassHook({});
        }

        m_InstalledGraph = graph;

        if (graph)
        {
            graph->SetPostPassHook([this](const std::string& passName, RenderGraph& g)
                                   { this->OnPassExecuted(passName, g); });
        }
    }

    void RenderGraphFrameCapture::ClearCaptures()
    {
        for (auto& [key, entry] : m_TextureCache)
        {
            if (entry.TextureID != 0)
            {
                glDeleteTextures(1, &entry.TextureID);
                entry.TextureID = 0;
            }
        }
        m_TextureCache.clear();
        m_Captures.clear();
    }

    u32 RenderGraphFrameCapture::AcquireTexture(const std::string& passName, Source source, u32 width, u32 height)
    {
        const CacheKey key{ passName, source };
        auto it = m_TextureCache.find(key);
        if (it != m_TextureCache.end())
        {
            // Reuse if dimensions match, otherwise reallocate.
            if (it->second.Width == width && it->second.Height == height && it->second.TextureID != 0)
            {
                return it->second.TextureID;
            }
            glDeleteTextures(1, &it->second.TextureID);
            it->second.TextureID = 0;
        }

        u32 tex = 0;
        glCreateTextures(GL_TEXTURE_2D, 1, &tex);
        glTextureStorage2D(tex, 1, GL_RGBA8, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
        glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // Force alpha = 1 on sample. Most scene framebuffers store alpha = 0
        // (cleared to vec4(0,0,0,0); only opaque shaders may set it). ImGui
        // renders our debug thumbnails with blending, so a 0 alpha makes the
        // entire image appear transparent (i.e. grey panel background).
        glTextureParameteri(tex, GL_TEXTURE_SWIZZLE_A, GL_ONE);

        m_TextureCache[key] = CachedTexture{ tex, width, height };
        return tex;
    }

    void RenderGraphFrameCapture::CaptureFramebuffer(const std::string& passName, Source source, u32 sourceTextureID, u32 width, u32 height,
                                                     std::string_view resourceName, u32 sourceFramebufferID, const GraphMetadata& metadata)
    {
        if (sourceTextureID == 0 || width == 0 || height == 0)
        {
            return;
        }

        const u32 dstTexture = AcquireTexture(passName, source, width, height);
        if (dstTexture == 0)
        {
            return;
        }

        // Wrap src texture in our shared source FBO and dst texture in
        // our shared dst FBO, then blit. This handles arbitrary format
        // conversion (the source may be RGBA16F HDR while we capture to
        // RGBA8 for cheap display).
        const u32 srcFBO = EnsureBlitFBO(s_BlitSrcFBO);
        const u32 dstFBO = EnsureBlitFBO(s_BlitDstFBO);

        glNamedFramebufferTexture(srcFBO, GL_COLOR_ATTACHMENT0, sourceTextureID, 0);
        glNamedFramebufferTexture(dstFBO, GL_COLOR_ATTACHMENT0, dstTexture, 0);
        glNamedFramebufferReadBuffer(srcFBO, GL_COLOR_ATTACHMENT0);
        glNamedFramebufferDrawBuffer(dstFBO, GL_COLOR_ATTACHMENT0);

        // Sanity-check completeness — attaching a depth-only or zero
        // texture would otherwise produce GL_INVALID_FRAMEBUFFER_OPERATION
        // from the blit.
        const GLenum srcStatus = glCheckNamedFramebufferStatus(srcFBO, GL_READ_FRAMEBUFFER);
        const GLenum dstStatus = glCheckNamedFramebufferStatus(dstFBO, GL_DRAW_FRAMEBUFFER);
        if (srcStatus != GL_FRAMEBUFFER_COMPLETE || dstStatus != GL_FRAMEBUFFER_COMPLETE)
        {
            OLO_CORE_WARN("RenderGraphFrameCapture[{}|{}:{}]: FBO incomplete (src=0x{:x} dst=0x{:x})",
                          passName, SourceName(source), resourceName, srcStatus, dstStatus);
            return;
        }

        // glBlitFramebuffer honors GL_COLOR_WRITEMASK, GL_SCISSOR_TEST, and
        // GL_FRAMEBUFFER_SRGB. Various passes leave color writes disabled
        // (depth prepass), scissor enabled to a sub-region (shadow tiles),
        // or sRGB-encode on, all of which would silently produce a black /
        // partial capture. Save current state, neutralize, blit, restore.
        GLboolean prevColorMask[4]{};
        glGetBooleanv(GL_COLOR_WRITEMASK, prevColorMask);
        const GLboolean prevScissor = glIsEnabled(GL_SCISSOR_TEST);
        const GLboolean prevSrgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
        const GLboolean prevRasterizerDiscard = glIsEnabled(GL_RASTERIZER_DISCARD);

        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        if (prevScissor)
        {
            glDisable(GL_SCISSOR_TEST);
        }
        if (prevSrgb)
        {
            glDisable(GL_FRAMEBUFFER_SRGB);
        }
        if (prevRasterizerDiscard)
        {
            glDisable(GL_RASTERIZER_DISCARD);
        }

        glBlitNamedFramebuffer(srcFBO, dstFBO,
                               0, 0, static_cast<GLint>(width), static_cast<GLint>(height),
                               0, 0, static_cast<GLint>(width), static_cast<GLint>(height),
                               GL_COLOR_BUFFER_BIT, GL_NEAREST);

        const GLenum blitErr = glGetError();
        if (blitErr != GL_NO_ERROR)
        {
            OLO_CORE_WARN("RenderGraphFrameCapture[{}|{}:{}]: glBlitNamedFramebuffer GL error 0x{:x} (src tex {}, {}x{})",
                          passName, SourceName(source), resourceName, blitErr, sourceTextureID, width, height);
        }
        else
        {
            RecordCapture(passName, source, resourceName, sourceTextureID, sourceFramebufferID,
                          dstTexture, width, height, metadata);
        }

        // Restore prior global state so we don't perturb the next pass.
        glColorMask(prevColorMask[0], prevColorMask[1], prevColorMask[2], prevColorMask[3]);
        if (prevScissor)
        {
            glEnable(GL_SCISSOR_TEST);
        }
        if (prevSrgb)
        {
            glEnable(GL_FRAMEBUFFER_SRGB);
        }
        if (prevRasterizerDiscard)
        {
            glEnable(GL_RASTERIZER_DISCARD);
        }

        // Detach so we don't keep stale references.
        glNamedFramebufferTexture(srcFBO, GL_COLOR_ATTACHMENT0, 0, 0);
        glNamedFramebufferTexture(dstFBO, GL_COLOR_ATTACHMENT0, 0, 0);

        // CaptureEntry is appended above on successful blit so probe stats are retained.
    }

    void RenderGraphFrameCapture::CaptureDefaultFramebuffer(const std::string& passName, Source source, u32 width, u32 height,
                                                            std::string_view resourceName, const GraphMetadata& metadata)
    {
        if (width == 0 || height == 0)
        {
            return;
        }

        const u32 dstTexture = AcquireTexture(passName, source, width, height);
        if (dstTexture == 0)
        {
            return;
        }

        const u32 dstFBO = EnsureBlitFBO(s_BlitDstFBO);
        glNamedFramebufferTexture(dstFBO, GL_COLOR_ATTACHMENT0, dstTexture, 0);
        glNamedFramebufferDrawBuffer(dstFBO, GL_COLOR_ATTACHMENT0);

        const GLenum dstStatus = glCheckNamedFramebufferStatus(dstFBO, GL_DRAW_FRAMEBUFFER);
        if (dstStatus != GL_FRAMEBUFFER_COMPLETE)
        {
            OLO_CORE_WARN("RenderGraphFrameCapture[{}|{}:{}]: default-FB capture destination incomplete (dst=0x{:x})",
                          passName, SourceName(source), resourceName, dstStatus);
            glNamedFramebufferTexture(dstFBO, GL_COLOR_ATTACHMENT0, 0, 0);
            return;
        }

        GLint prevReadFramebuffer = 0;
        GLint prevDrawFramebuffer = 0;
        GLint prevReadBuffer = GL_BACK;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFramebuffer);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFramebuffer);
        glGetIntegerv(GL_READ_BUFFER, &prevReadBuffer);

        GLboolean prevColorMask[4]{};
        glGetBooleanv(GL_COLOR_WRITEMASK, prevColorMask);
        const GLboolean prevScissor = glIsEnabled(GL_SCISSOR_TEST);
        const GLboolean prevSrgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
        const GLboolean prevRasterizerDiscard = glIsEnabled(GL_RASTERIZER_DISCARD);

        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        if (prevScissor)
        {
            glDisable(GL_SCISSOR_TEST);
        }
        if (prevSrgb)
        {
            glDisable(GL_FRAMEBUFFER_SRGB);
        }
        if (prevRasterizerDiscard)
        {
            glDisable(GL_RASTERIZER_DISCARD);
        }

        glBlitNamedFramebuffer(0, dstFBO,
                               0, 0, static_cast<GLint>(width), static_cast<GLint>(height),
                               0, 0, static_cast<GLint>(width), static_cast<GLint>(height),
                               GL_COLOR_BUFFER_BIT, GL_NEAREST);

        const GLenum blitErr = glGetError();
        if (blitErr != GL_NO_ERROR)
        {
            OLO_CORE_WARN("RenderGraphFrameCapture[{}|{}:{}]: default-FB blit GL error 0x{:x} ({}x{})",
                          passName, SourceName(source), resourceName, blitErr, width, height);
        }
        else
        {
            RecordCapture(passName, source, resourceName, 0, 0, dstTexture, width, height, metadata);
        }

        glColorMask(prevColorMask[0], prevColorMask[1], prevColorMask[2], prevColorMask[3]);
        if (prevScissor)
        {
            glEnable(GL_SCISSOR_TEST);
        }
        if (prevSrgb)
        {
            glEnable(GL_FRAMEBUFFER_SRGB);
        }
        if (prevRasterizerDiscard)
        {
            glEnable(GL_RASTERIZER_DISCARD);
        }

        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevReadFramebuffer));
        glReadBuffer(static_cast<GLenum>(prevReadBuffer));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prevDrawFramebuffer));
        glNamedFramebufferTexture(dstFBO, GL_COLOR_ATTACHMENT0, 0, 0);
    }

    void RenderGraphFrameCapture::RecordCapture(const std::string& passName, Source source, std::string_view resourceName,
                                                u32 sourceTextureID, u32 sourceFramebufferID, u32 dstTexture,
                                                u32 width, u32 height, const GraphMetadata& metadata)
    {
        std::array<std::array<u8, 4>, 9> probes{};
        const GLint probeX[3] = {
            0,
            std::max<GLint>(0, static_cast<GLint>(width) / 2),
            std::max<GLint>(0, static_cast<GLint>(width) - 1)
        };
        const GLint probeY[3] = {
            0,
            std::max<GLint>(0, static_cast<GLint>(height) / 2),
            std::max<GLint>(0, static_cast<GLint>(height) - 1)
        };

        u32 nonBlackSamples = 0;
        u32 nonTransparentSamples = 0;
        sizet probeIndex = 0;
        for (const GLint y : probeY)
        {
            for (const GLint x : probeX)
            {
                std::array<u8, 4> rgba{ 0, 0, 0, 0 };
                glGetTextureSubImage(dstTexture, 0, x, y, 0, 1, 1, 1,
                                     GL_RGBA, GL_UNSIGNED_BYTE,
                                     static_cast<GLsizei>(rgba.size()), rgba.data());
                probes[probeIndex] = rgba;

                if (rgba[0] != 0 || rgba[1] != 0 || rgba[2] != 0)
                    ++nonBlackSamples;
                if (rgba[3] != 0)
                    ++nonTransparentSamples;

                ++probeIndex;
            }
        }

        const auto& center = probes[4];
        if (nonBlackSamples == 0)
        {
            OLO_CORE_WARN("RenderGraphFrameCapture[{}|{}:{}]: BLACK capture (src tex {} fb {} -> dst tex {}, {}x{}, nonBlack={}/9, nonTransparent={}/9, center=({},{},{},{}))",
                          passName, SourceName(source), resourceName, sourceTextureID, sourceFramebufferID, dstTexture, width, height,
                          nonBlackSamples, nonTransparentSamples,
                          center[0], center[1], center[2], center[3]);
        }
        else if (IsPresentationLikeSource(source) && nonTransparentSamples == 0)
        {
            OLO_CORE_WARN("RenderGraphFrameCapture[{}|{}:{}]: TRANSPARENT capture (src tex {} fb {} -> dst tex {}, {}x{}, nonBlack={}/9, nonTransparent={}/9, center=({},{},{},{}))",
                          passName, SourceName(source), resourceName, sourceTextureID, sourceFramebufferID, dstTexture, width, height,
                          nonBlackSamples, nonTransparentSamples,
                          center[0], center[1], center[2], center[3]);
        }
        else
        {
            OLO_CORE_TRACE("RenderGraphFrameCapture[{}|{}:{}]: blit OK src tex {} fb {} -> dst tex {} ({}x{}, nonBlack={}/9, nonTransparent={}/9, center=({},{},{},{}))",
                           passName, SourceName(source), resourceName, sourceTextureID, sourceFramebufferID, dstTexture, width, height,
                           nonBlackSamples, nonTransparentSamples,
                           center[0], center[1], center[2], center[3]);
        }

        m_Captures.push_back(CaptureEntry{
            .PassName = passName,
            .ResourceName = std::string(resourceName),
            .SourceKind = source,
            .TextureID = dstTexture,
            .SourceTextureID = sourceTextureID,
            .SourceFramebufferID = sourceFramebufferID,
            .Width = width,
            .Height = height,
            .PassOrderIndex = metadata.PassOrderIndex,
            .CulledPassCount = metadata.CulledPassCount,
            .PlannedBarrierCount = metadata.PlannedBarrierCount,
            .ResourceCount = metadata.ResourceCount,
            .NonBlackSamples = nonBlackSamples,
            .NonTransparentSamples = nonTransparentSamples,
            .CenterRGBA = center,
        });
    }

    void RenderGraphFrameCapture::OnPassExecuted(const std::string& passName, RenderGraph& graph)
    {
        if (!m_PendingCapture && !m_CapturingActive)
        {
            return;
        }

        // First pass of a requested capture — start fresh.
        if (m_PendingCapture)
        {
            m_PendingCapture = false;
            m_CapturingActive = true;
            m_DiagLogged = false;
            m_Captures.clear();
            m_PassesSeenThisCapture.clear();
        }

        // Detect end-of-frame: if we've already captured this pass during
        // the current capture window, the graph has wrapped to the next
        // frame — finalize and stop capturing.
        if (m_PassesSeenThisCapture.contains(passName))
        {
            m_CapturingActive = false;
            return;
        }
        m_PassesSeenThisCapture.insert(passName);

        GraphMetadata metadata;
        const auto& passOrder = graph.GetExecutionOrder();
        if (const auto it = std::find(passOrder.begin(), passOrder.end(), passName); it != passOrder.end())
        {
            metadata.PassOrderIndex = static_cast<u32>(std::distance(passOrder.begin(), it));
        }
        metadata.CulledPassCount = static_cast<u32>(graph.GetCulledPasses().size());
        metadata.PlannedBarrierCount = static_cast<u32>(graph.GetPlannedBarriers().size());
        metadata.ResourceCount = static_cast<u32>(graph.GetRegisteredResources().size());

        const auto passIndexOf = [&passOrder](std::string_view name) -> u32
        {
            const auto it = std::find_if(passOrder.begin(), passOrder.end(),
                                         [name](const std::string& candidate)
                                         { return std::string_view(candidate) == name; });
            if (it == passOrder.end())
                return std::numeric_limits<u32>::max();
            return static_cast<u32>(std::distance(passOrder.begin(), it));
        };

        const u32 scenePassIndex = passIndexOf("ScenePass");
        const bool sceneTimelineStarted = metadata.PassOrderIndex != std::numeric_limits<u32>::max() &&
                                          scenePassIndex != std::numeric_limits<u32>::max() &&
                                          metadata.PassOrderIndex >= scenePassIndex;

        bool emittedDiag = false;

        const auto captureFB = [&](Source kind, std::string_view resourceName, const Ref<Framebuffer>& fb)
        {
            if (!fb)
                return;

            const auto& spec = fb->GetSpecification();
            if (CountColorAttachments(spec) == 0)
                return;

            const u32 colorID = fb->GetColorAttachmentRendererID(0);
            if (colorID == 0)
            {
                return;
            }

            // One-shot diagnostic per capture so we can verify the live FB
            // matches what the pass actually rendered into.
            if (!m_DiagLogged)
            {
                const sizet attachmentCount = spec.Attachments.Attachments.size();
                OLO_CORE_INFO("RenderGraphFrameCapture[live {}:{}]: fbGL={} attachments={} colorTex0={} ({}x{})",
                              SourceName(kind), resourceName, fb->GetRendererID(), attachmentCount, colorID, spec.Width, spec.Height);
                emittedDiag = true;
            }

            CaptureFramebuffer(passName, kind, colorID, spec.Width, spec.Height,
                               resourceName, fb->GetRendererID(), metadata);
        };

        const auto captureTexture = [&](Source kind, std::string_view resourceName, u32 textureID, u32 width, u32 height, u32 sourceFramebufferID = 0)
        {
            if (textureID == 0 || width == 0 || height == 0)
                return;
            if (!m_DiagLogged)
            {
                OLO_CORE_INFO("RenderGraphFrameCapture[live {}:{}]: tex={} fb={} ({}x{})",
                              SourceName(kind), resourceName, textureID, sourceFramebufferID, width, height);
                emittedDiag = true;
            }
            CaptureFramebuffer(passName, kind, textureID, width, height, resourceName, sourceFramebufferID, metadata);
        };

        const auto captureGraphTexture = [&](Source kind, std::string_view resourceName)
        {
            const u32 textureID = graph.ResolveTexture(graph.GetTextureHandle(resourceName));
            if (textureID == 0)
            {
                return;
            }

            GLint width = 0;
            GLint height = 0;
            glGetTextureLevelParameteriv(textureID, 0, GL_TEXTURE_WIDTH, &width);
            glGetTextureLevelParameteriv(textureID, 0, GL_TEXTURE_HEIGHT, &height);
            if (width <= 0 || height <= 0)
            {
                return;
            }

            captureTexture(kind, resourceName, textureID, static_cast<u32>(width), static_cast<u32>(height));
        };

        // SceneColor is the primary timeline surface. Capture it after every
        // executed pass once ScenePass has produced the first scene image.
        if (sceneTimelineStarted)
        {
            if (const auto sceneFramebuffer = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor); sceneFramebuffer)
            {
                captureFB(Source::SceneColor, ResourceNames::SceneColor, sceneFramebuffer);
            }
        }

        if (passName == "ScenePass" || passName == "DeferredOpaqueDecalPass")
        {
            captureGraphTexture(Source::SceneNormals, ResourceNames::SceneNormals);
            captureGraphTexture(Source::GBufferAlbedo, ResourceNames::GBufferAlbedo);
            captureGraphTexture(Source::GBufferNormal, ResourceNames::GBufferNormal);
            captureGraphTexture(Source::GBufferEmissive, ResourceNames::GBufferEmissive);
            captureGraphTexture(Source::Velocity, ResourceNames::Velocity);
        }

        if (passName == "SSSPass")
            captureFB(Source::SSSColor, ResourceNames::SSSColor, Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SSSColor));
        if (passName == "AOApplyPass")
            captureFB(Source::AOApplyColor, ResourceNames::AOApplyColor, Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::AOApplyColor));
        if (passName == "BloomPass")
            captureFB(Source::BloomColor, ResourceNames::BloomColor, Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::BloomColor));
        if (passName == "DOFPass")
            captureFB(Source::DOFColor, ResourceNames::DOFColor, Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::DOFColor));
        if (passName == "MotionBlurPass")
            captureFB(Source::MotionBlurColor, ResourceNames::MotionBlurColor, Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::MotionBlurColor));
        if (passName == "TAAPass")
            captureFB(Source::TAAColor, ResourceNames::TAAColor, Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::TAAColor));
        if (passName == "PrecipitationPass")
            captureFB(Source::PrecipitationColor, ResourceNames::PrecipitationColor, Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::PrecipitationColor));
        if (passName == "FogPass")
            captureFB(Source::FogColor, ResourceNames::FogColor, Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::FogColor));
        if (passName == "ChromAberrationPass")
            captureFB(Source::ChromAbColor, ResourceNames::ChromAbColor, Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ChromAbColor));
        if (passName == "ColorGradingPass")
            captureFB(Source::ColorGradingColor, ResourceNames::ColorGradingColor, Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ColorGradingColor));
        if (passName == "ToneMapPass")
            captureFB(Source::ToneMapColor, ResourceNames::ToneMapColor, Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor));
        if (passName == "VignettePass")
            captureFB(Source::VignetteColor, ResourceNames::VignetteColor, Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::VignetteColor));
        if (passName == "FXAAPass")
            captureFB(Source::FXAAColor, ResourceNames::FXAAColor, Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::FXAAColor));
        if (passName == "SelectionOutlinePass")
            captureFB(Source::SelectionOutlineColor, ResourceNames::SelectionOutlineColor, Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SelectionOutlineColor));
        if (passName == "UICompositePass")
            captureFB(Source::UIComposite, ResourceNames::UIComposite, Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite));

        if (passName == "SSAOPass")
            captureGraphTexture(Source::AOTexture, ResourceNames::AOBuffer);

        if (passName == "GTAOPass")
        {
            captureGraphTexture(Source::AOTexture, ResourceNames::AOBuffer);
            captureGraphTexture(Source::HZBDepth, ResourceNames::HZBDepth);
        }

        if (passName == "FinalPass")
        {
            bool capturedPresentedImage = false;
            if (const auto uiFramebuffer = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite); uiFramebuffer)
            {
                captureFB(Source::Backbuffer, ResourceNames::Backbuffer, uiFramebuffer);
                capturedPresentedImage = true;
            }

            if (!capturedPresentedImage)
            {
                u32 width = 0;
                u32 height = 0;
                if (const auto sceneFramebuffer = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor); sceneFramebuffer)
                {
                    const auto& spec = sceneFramebuffer->GetSpecification();
                    width = spec.Width;
                    height = spec.Height;
                }
                CaptureDefaultFramebuffer(passName, Source::Backbuffer, width, height, ResourceNames::Backbuffer, metadata);
            }
        }

        m_DiagLogged = m_DiagLogged || emittedDiag;
    }
} // namespace OloEngine

#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Debug/RenderGraphFrameCapture.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/Passes/PostProcessRenderPass.h"
#include "OloEngine/Renderer/Passes/SelectionOutlineRenderPass.h"
#include "OloEngine/Renderer/Passes/UICompositeRenderPass.h"
#include "OloEngine/Renderer/Passes/SSSRenderPass.h"
#include "OloEngine/Renderer/Passes/OITResolveRenderPass.h"
#include <glad/gl.h>

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
            case Source::PostProcessColor:
                return "PostProcessColor";
            case Source::SelectionOutlineColor:
                return "SelectionOutline";
            case Source::UIComposite:
                return "UIComposite";
            case Source::SceneColorViaBlackboard:
                return "SceneColorViaBlackboard";
            case Source::PostProcessColorViaBlackboard:
                return "PostProcessColorViaBlackboard";
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

    void RenderGraphFrameCapture::CaptureFramebuffer(const std::string& passName, Source source, u32 sourceTextureID, u32 width, u32 height)
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
            OLO_CORE_WARN("RenderGraphFrameCapture[{}|{}]: FBO incomplete (src=0x{:x} dst=0x{:x})",
                          passName, SourceName(source), srcStatus, dstStatus);
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
            OLO_CORE_WARN("RenderGraphFrameCapture[{}|{}]: glBlitNamedFramebuffer GL error 0x{:x} (src tex {}, {}x{})",
                          passName, SourceName(source), blitErr, sourceTextureID, width, height);
        }
        else
        {
            // Probe the center pixel of the just-written destination so we can
            // tell whether the blit actually wrote color data.
            u8 probe[4] = { 0, 0, 0, 0 };
            const GLint cx = static_cast<GLint>(width) / 2;
            const GLint cy = static_cast<GLint>(height) / 2;
            glGetTextureSubImage(dstTexture, 0, cx, cy, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, sizeof(probe), probe);

            OLO_CORE_TRACE("RenderGraphFrameCapture[{}|{}]: blit OK src tex {} -> dst tex {} ({}x{}) center=({},{},{},{})",
                           passName, SourceName(source), sourceTextureID, dstTexture, width, height,
                           probe[0], probe[1], probe[2], probe[3]);
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

        m_Captures.push_back(CaptureEntry{
            .PassName = passName,
            .SourceKind = source,
            .TextureID = dstTexture,
            .Width = width,
            .Height = height,
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

        // Bypass the RenderGraph blackboard entirely — query Renderer3D's
        // live pass framebuffers directly. The blackboard handle layer has
        // known issues where slots can be corrupted by transient resource
        // planning that shares names with imported resources. Reading
        // ScenePass->GetTarget() etc. is the source of truth: it returns
        // the EXACT framebuffer the pass just rendered into.
        struct SourceBinding
        {
            Source Kind;
            Ref<Framebuffer> FB;
        };

        SourceBinding sources[6]{};
        sizet sourceCount = 0;

        if (const auto& scenePass = Renderer3D::GetScenePass(); scenePass && scenePass->GetTarget())
        {
            sources[sourceCount++] = { Source::SceneColor, scenePass->GetTarget() };
        }
        if (const auto& sssPass = Renderer3D::GetSSSPass(); sssPass && sssPass->GetTarget())
        {
            sources[sourceCount++] = { Source::SSSColor, sssPass->GetTarget() };
        }
        if (const auto& oitPass = Renderer3D::GetOITResolvePass(); oitPass && oitPass->GetTarget())
        {
            sources[sourceCount++] = { Source::OITResolveColor, oitPass->GetTarget() };
        }
        if (const auto& postPass = Renderer3D::GetPostProcessPass(); postPass && postPass->GetTarget())
        {
            sources[sourceCount++] = { Source::PostProcessColor, postPass->GetTarget() };
        }
        if (const auto& selPass = Renderer3D::GetSelectionOutlinePass(); selPass && selPass->GetTarget())
        {
            sources[sourceCount++] = { Source::SelectionOutlineColor, selPass->GetTarget() };
        }
        if (const auto& uiPass = Renderer3D::GetUICompositePass(); uiPass && uiPass->GetTarget())
        {
            sources[sourceCount++] = { Source::UIComposite, uiPass->GetTarget() };
        }

        for (sizet i = 0; i < sourceCount; ++i)
        {
            const auto& src = sources[i];
            const auto& fb = src.FB;

            const auto& spec = fb->GetSpecification();
            const u32 colorID = fb->GetColorAttachmentRendererID(0);
            if (colorID == 0)
            {
                continue;
            }

            // One-shot diagnostic per capture so we can verify the live FB
            // matches what the pass actually rendered into.
            if (!m_DiagLogged)
            {
                const sizet attachmentCount = spec.Attachments.Attachments.size();
                OLO_CORE_INFO("RenderGraphFrameCapture[live {}]: fbGL={} attachments={} colorTex0={} ({}x{})",
                              SourceName(src.Kind), fb->GetRendererID(), attachmentCount, colorID, spec.Width, spec.Height);
            }

            CaptureFramebuffer(passName, src.Kind, colorID, spec.Width, spec.Height);
        }

        // Capture the GTAO AO texture directly. This is a stand-alone R8
        // texture (not a framebuffer attachment), so feed its renderer ID
        // straight into the blit. Useful for diagnosing whether the AO
        // output itself contains corruption / scene fragments rather than
        // pure occlusion data.
        if (const auto& gtaoPass = Renderer3D::GetGTAOPass(); gtaoPass)
        {
            const u32 aoID = gtaoPass->GetGTAOTextureID();
            const u32 aoW = gtaoPass->GetWidth();
            const u32 aoH = gtaoPass->GetHeight();
            if (aoID != 0 && aoW > 0 && aoH > 0)
            {
                if (!m_DiagLogged)
                {
                    OLO_CORE_INFO("RenderGraphFrameCapture[live AOTexture]: aoTex={} ({}x{})", aoID, aoW, aoH);
                }
                CaptureFramebuffer(passName, Source::AOTexture, aoID, aoW, aoH);
            }

            // HZB depth (mip 0) — what GTAO actually samples. If this contains
            // ghost geometry, depth is corrupt. If clean, AO bug is inside
            // the GTAO compute itself.
            const auto& hzb = gtaoPass->GetHZBGenerator();
            const u32 hzbID = hzb.GetHZBTextureID();
            const u32 hzbW = hzb.GetHZBWidth();
            const u32 hzbH = hzb.GetHZBHeight();
            if (hzbID != 0 && hzbW > 0 && hzbH > 0)
            {
                if (!m_DiagLogged)
                {
                    OLO_CORE_INFO("RenderGraphFrameCapture[live HZB]: hzbTex={} ({}x{})", hzbID, hzbW, hzbH);
                }
                CaptureFramebuffer(passName, Source::HZBDepth, hzbID, hzbW, hzbH);
            }
        }

        // SceneNormals = scene FB color attachment 2 (RG16F octahedral
        // view-space normals). GTAO compute reads this via texelFetch at
        // pixCoord — if dimensions don't match m_Width/m_Height of GTAO,
        // GTAO reads wrong-pixel normals and produces shifted AO.
        if (const auto& scenePass = Renderer3D::GetScenePass(); scenePass && scenePass->GetTarget())
        {
            const auto& sceneFB = scenePass->GetTarget();
            const auto& spec = sceneFB->GetSpecification();
            // Attachment index 2 is normals per Renderer3D scene FB layout
            const u32 normalsID = sceneFB->GetColorAttachmentRendererID(2);
            if (normalsID != 0)
            {
                if (!m_DiagLogged)
                {
                    OLO_CORE_INFO("RenderGraphFrameCapture[live SceneNormals]: normalsTex={} ({}x{})",
                                  normalsID, spec.Width, spec.Height);
                }
                CaptureFramebuffer(passName, Source::SceneNormals, normalsID, spec.Width, spec.Height);
            }
        }

        // Diagnostic: ALSO capture what the RenderGraph blackboard resolves
        // SceneColor / PostProcessColor to. If these differ from the "live"
        // versions above, the blackboard's physical-slot bookkeeping is
        // corrupt. PostProcessPass and other consumers read through the
        // blackboard, so a mismatch here directly explains rendering bugs
        // (ghosting, stale content, wrong tonemap input, etc.).
        struct BlackboardBinding
        {
            Source Kind;
            RGFramebufferHandle Handle;
        };

        const auto& blackboard = graph.GetBlackboard();
        const BlackboardBinding bbSources[] = {
            { Source::SceneColorViaBlackboard, blackboard.SceneColor },
            { Source::PostProcessColorViaBlackboard, blackboard.PostProcessColor },
        };

        for (const auto& src : bbSources)
        {
            if (!src.Handle.IsValid())
            {
                continue;
            }
            const Ref<Framebuffer> fb = graph.ResolveFramebuffer(src.Handle);
            if (!fb)
            {
                continue;
            }

            const auto& spec = fb->GetSpecification();
            const u32 colorID = fb->GetColorAttachmentRendererID(0);
            if (colorID == 0)
            {
                continue;
            }

            if (!m_DiagLogged)
            {
                const std::string_view rname = graph.GetResourceName(src.Handle);
                const sizet attachmentCount = spec.Attachments.Attachments.size();
                OLO_CORE_INFO("RenderGraphFrameCapture[blackboard {}]: handle=(idx={}, gen={}) name='{}' fbGL={} attachments={} colorTex0={} ({}x{})",
                              SourceName(src.Kind), src.Handle.Index, src.Handle.Generation,
                              rname, fb->GetRendererID(), attachmentCount, colorID, spec.Width, spec.Height);
            }

            CaptureFramebuffer(passName, src.Kind, colorID, spec.Width, spec.Height);
        }

        m_DiagLogged = true;

        // Suppress unused-parameter warning — we no longer query the graph
        // directly but keep the parameter for API compatibility with the
        // hook signature.
        (void)graph;
    }
} // namespace OloEngine

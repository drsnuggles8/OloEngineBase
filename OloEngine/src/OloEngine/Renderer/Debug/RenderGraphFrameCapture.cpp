#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Debug/RenderGraphFrameCapture.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Renderer/Debug/FrameCaptureBackend.h"
#include "OloEngine/Renderer/Debug/RenderGraphResourceIdentity.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/GBuffer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include <algorithm>
#include <array>
#include <limits>
#include <string_view>

namespace OloEngine
{
    namespace
    {
        // Single shared blit destination FBO — re-used across all
        // captures by re-attaching the per-capture texture as
        // attachment 0 before each blit.
        RHI::ResourceHandle s_BlitDstFBO;
        // Single shared blit source FBO — used to wrap the source FB's
        // color attachment when only the texture identity is available.
        RHI::ResourceHandle s_BlitSrcFBO;

        RHI::ResourceHandle EnsureBlitFBO(RHI::ResourceHandle& fbo)
        {
            if (!fbo.IsValid())
            {
                fbo = RenderCommand::CreateFramebufferHandle();
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
                // The colour-vision adaptation output (issue #458) is the LAST
                // stage before present when a mode is active, so it is the most
                // presentation-like source there is — a transparent capture here
                // must raise the same diagnostic as its neighbours.
                case S::ColorBlindColor:
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
        if (s_BlitDstFBO.IsValid())
        {
            RenderCommand::DeleteFramebuffer(s_BlitDstFBO);
            s_BlitDstFBO = RHI::NullResource;
        }
        if (s_BlitSrcFBO.IsValid())
        {
            RenderCommand::DeleteFramebuffer(s_BlitSrcFBO);
            s_BlitSrcFBO = RHI::NullResource;
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
            case Source::ColorBlindColor:
                return "ColorBlindColor";
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
            m_InstalledGraph->RemovePostPassHook(kPostPassHookKey);
        }

        m_InstalledGraph = graph;

        if (graph)
        {
            // Keyed registration (issue #607): the MCP afterPass snapshot can
            // hold its own post-pass hook on the same graph without either
            // tool clobbering the other.
            graph->AddPostPassHook(kPostPassHookKey,
                                   [this](const std::string& passName, RenderGraph& g)
                                   { this->OnPassExecuted(passName, g); });
        }
    }

    void RenderGraphFrameCapture::ClearCaptures()
    {
        for (auto& [key, entry] : m_TextureCache)
        {
            if (entry.Texture.IsValid())
            {
                RenderCommand::DeleteTexture(entry.Texture);
                entry.Texture = RHI::NullResource;
            }
        }
        m_TextureCache.clear();
        m_Captures.clear();
    }

    RHI::ResourceHandle RenderGraphFrameCapture::AcquireTexture(const std::string& passName, Source source, u32 width, u32 height)
    {
        const CacheKey key{ passName, source };
        if (auto it = m_TextureCache.find(key); it != m_TextureCache.end())
        {
            // Reuse if dimensions match, otherwise reallocate.
            if (it->second.Width == width && it->second.Height == height && it->second.Texture.IsValid())
            {
                return it->second.Texture;
            }
            RenderCommand::DeleteTexture(it->second.Texture);
            it->second.Texture = RHI::NullResource;
        }

        const RHI::ResourceHandle tex = RenderCommand::CreateTexture2DHandle(width, height, RHI::Format::RGBA8UNorm);
        if (!tex.IsValid())
        {
            return RHI::NullResource;
        }
        RenderCommand::SetTextureFilter(tex, RHI::Filter::Linear, RHI::Filter::Linear);
        RenderCommand::SetTextureWrap(tex, RHI::AddressMode::ClampToEdge);
        // Force alpha = 1 on sample. Most scene framebuffers store alpha = 0
        // (cleared to vec4(0,0,0,0); only opaque shaders may set it). ImGui
        // renders our debug thumbnails with blending, so a 0 alpha makes the
        // entire image appear transparent (i.e. grey panel background).
        // Behind the seam (#691): texture swizzle has no RendererAPI
        // equivalent — see FrameCaptureBackend.h.
        Detail::SetTextureAlphaSwizzleOne(tex);

        m_TextureCache[key] = CachedTexture{ tex, width, height };
        return tex;
    }

    void RenderGraphFrameCapture::CaptureFramebuffer(const std::string& passName, Source source, RHI::ResourceHandle sourceTexture,
                                                     u32 width, u32 height,
                                                     std::string_view resourceName, u32 sourceFramebufferID, const GraphMetadata& metadata)
    {
        if (!sourceTexture.IsValid() || width == 0 || height == 0)
        {
            return;
        }

        const RHI::ResourceHandle dstTexture = AcquireTexture(passName, source, width, height);
        if (!dstTexture.IsValid())
        {
            return;
        }

        // Wrap src texture in our shared source FBO and dst texture in
        // our shared dst FBO, then blit. This handles arbitrary format
        // conversion (the source may be RGBA16F HDR while we capture to
        // RGBA8 for cheap display).
        const RHI::ResourceHandle srcFBO = EnsureBlitFBO(s_BlitSrcFBO);
        const RHI::ResourceHandle dstFBO = EnsureBlitFBO(s_BlitDstFBO);

        RenderCommand::AttachFramebufferColorTexture(srcFBO, 0, sourceTexture, 0);
        RenderCommand::AttachFramebufferColorTexture(dstFBO, 0, dstTexture, 0);
        RenderCommand::SetFramebufferReadAttachment(srcFBO, 0);
        constexpr std::array<u32, 1> kAttachment0 = { 0u };
        RenderCommand::SetFramebufferDrawAttachments(dstFBO, kAttachment0);

        // Sanity-check completeness — attaching a depth-only or zero
        // texture would otherwise produce GL_INVALID_FRAMEBUFFER_OPERATION
        // from the blit.
        const bool srcComplete = RenderCommand::IsFramebufferComplete(srcFBO);
        const bool dstComplete = RenderCommand::IsFramebufferComplete(dstFBO);
        if (!srcComplete || !dstComplete)
        {
            OLO_CORE_WARN("RenderGraphFrameCapture[{}|{}:{}]: FBO incomplete (srcComplete={} dstComplete={})",
                          passName, SourceName(source), resourceName, srcComplete, dstComplete);
            // Detach on THIS path too (review finding): both FBOs are
            // process-wide shared objects, so bailing with the textures still
            // attached leaves a live reference on a framebuffer nobody owns
            // AND leaks this capture's attachments into the next caller's
            // completeness check — which is exactly the state that produced
            // the incompleteness being reported.
            RenderCommand::AttachFramebufferColorTexture(srcFBO, 0, RHI::NullResource, 0);
            RenderCommand::AttachFramebufferColorTexture(dstFBO, 0, RHI::NullResource, 0);
            return;
        }

        // The blit honors GL_COLOR_WRITEMASK, GL_SCISSOR_TEST, and
        // GL_FRAMEBUFFER_SRGB. Various passes leave color writes disabled
        // (depth prepass), scissor enabled to a sub-region (shadow tiles),
        // or sRGB-encode on, all of which would silently produce a black /
        // partial capture. The seam saves that state, neutralizes it, blits,
        // and restores it — the required state QUERIES and the SRGB /
        // RASTERIZER_DISCARD toggles have no facade equivalent, and neither
        // does the folded error attribution (ADR 0011 amendment (7)); see
        // FrameCaptureBackend.h.
        if (const u32 blitErr = Detail::BlitWithGLTransferSemantics(srcFBO, dstFBO, width, height); blitErr != 0)
        {
            OLO_CORE_WARN("RenderGraphFrameCapture[{}|{}:{}]: blit GL error 0x{:x} (src tex {}, {}x{})",
                          passName, SourceName(source), resourceName, blitErr, sourceTexture, width, height);
        }
        else
        {
            RecordCapture(passName, source, resourceName, sourceTexture, sourceFramebufferID,
                          dstTexture, width, height, metadata);
        }

        // Detach so we don't keep stale references.
        RenderCommand::AttachFramebufferColorTexture(srcFBO, 0, RHI::NullResource, 0);
        RenderCommand::AttachFramebufferColorTexture(dstFBO, 0, RHI::NullResource, 0);

        // CaptureEntry is appended above on successful blit so probe stats are retained.
    }

    void RenderGraphFrameCapture::CaptureDefaultFramebuffer(const std::string& passName, Source source, u32 width, u32 height,
                                                            std::string_view resourceName, const GraphMetadata& metadata)
    {
        if (width == 0 || height == 0)
        {
            return;
        }

        const RHI::ResourceHandle dstTexture = AcquireTexture(passName, source, width, height);
        if (!dstTexture.IsValid())
        {
            return;
        }

        const RHI::ResourceHandle dstFBO = EnsureBlitFBO(s_BlitDstFBO);
        RenderCommand::AttachFramebufferColorTexture(dstFBO, 0, dstTexture, 0);
        constexpr std::array<u32, 1> kAttachment0 = { 0u };
        RenderCommand::SetFramebufferDrawAttachments(dstFBO, kAttachment0);

        if (!RenderCommand::IsFramebufferComplete(dstFBO))
        {
            OLO_CORE_WARN("RenderGraphFrameCapture[{}|{}:{}]: default-FB capture destination incomplete",
                          passName, SourceName(source), resourceName);
            RenderCommand::AttachFramebufferColorTexture(dstFBO, 0, RHI::NullResource, 0);
            return;
        }

        // Behind the seam (#691): saving the read/draw framebuffer
        // bindings and the read-buffer selection, and pointing the DEFAULT
        // framebuffer's read buffer at the backbuffer, have no RendererAPI
        // equivalent — the facade has no state-query family, no split
        // read/draw framebuffer bind, and SetFramebufferReadAttachment only
        // names color attachments. See FrameCaptureBackend.h.
        const Detail::DefaultFramebufferReadState readSource = Detail::SelectDefaultFramebufferReadSource();

        // RHI::NullResource as the blit source names the DEFAULT framebuffer;
        // the seam neutralizes/restores the blit-affecting state and folds the
        // error attribution (same contract as CaptureFramebuffer above).
        if (const u32 blitErr = Detail::BlitWithGLTransferSemantics(RHI::NullResource, dstFBO, width, height); blitErr != 0)
        {
            OLO_CORE_WARN("RenderGraphFrameCapture[{}|{}:{}]: default-FB blit GL error 0x{:x} ({}x{})",
                          passName, SourceName(source), resourceName, blitErr, width, height);
        }
        else
        {
            RecordCapture(passName, source, resourceName, RHI::NullResource, 0, dstTexture, width, height, metadata);
        }

        Detail::RestoreDefaultFramebufferReadSource(readSource);
        RenderCommand::AttachFramebufferColorTexture(dstFBO, 0, RHI::NullResource, 0);
    }

    void RenderGraphFrameCapture::RecordCapture(const std::string& passName, Source source, std::string_view resourceName,
                                                RHI::ResourceHandle sourceTexture, u32 sourceFramebufferID, RHI::ResourceHandle dstTexture,
                                                u32 width, u32 height, const GraphMetadata& metadata)
    {
        std::array<std::array<u8, 4>, 9> probes{};
        const i32 probeX[3] = {
            0,
            std::max<i32>(0, static_cast<i32>(width) / 2),
            std::max<i32>(0, static_cast<i32>(width) - 1)
        };
        const i32 probeY[3] = {
            0,
            std::max<i32>(0, static_cast<i32>(height) / 2),
            std::max<i32>(0, static_cast<i32>(height) - 1)
        };

        u32 nonBlackSamples = 0;
        u32 nonTransparentSamples = 0;
        sizet probeIndex = 0;
        for (const i32 y : probeY)
        {
            for (const i32 x : probeX)
            {
                std::array<u8, 4> rgba{ 0, 0, 0, 0 };
                if (!RenderCommand::ReadTextureSubImage(dstTexture, 0, x, y, 0, 1, 1, 1,
                                                        RHI::Format::RGBA8UNorm,
                                                        rgba.size(), rgba.data()))
                {
                    // Keep the zero-initialised sample — the same value the
                    // raw readback produced on error.
                    rgba = { 0, 0, 0, 0 };
                }
                probes[probeIndex] = rgba;

                if (rgba[0] != 0 || rgba[1] != 0 || rgba[2] != 0)
                    ++nonBlackSamples;
                if (rgba[3] != 0)
                    ++nonTransparentSamples;

                ++probeIndex;
            }
        }

        // Native names for the diagnostics below and for CaptureEntry (the
        // debugger feeds TextureID straight to ImGui as an ImTextureID).
        const u32 sourceTextureID = Debug::NativeTextureIdForDiagnostics(sourceTexture);
        const u32 dstTextureID = Debug::NativeTextureIdForDiagnostics(dstTexture);

        const auto& center = probes[4];
        if (nonBlackSamples == 0)
        {
            OLO_CORE_WARN("RenderGraphFrameCapture[{}|{}:{}]: BLACK capture (src tex {} fb {} -> dst tex {}, {}x{}, nonBlack={}/9, nonTransparent={}/9, center=({},{},{},{}))",
                          passName, SourceName(source), resourceName, sourceTextureID, sourceFramebufferID, dstTextureID, width, height,
                          nonBlackSamples, nonTransparentSamples,
                          center[0], center[1], center[2], center[3]);
        }
        else if (IsPresentationLikeSource(source) && nonTransparentSamples == 0)
        {
            OLO_CORE_WARN("RenderGraphFrameCapture[{}|{}:{}]: TRANSPARENT capture (src tex {} fb {} -> dst tex {}, {}x{}, nonBlack={}/9, nonTransparent={}/9, center=({},{},{},{}))",
                          passName, SourceName(source), resourceName, sourceTextureID, sourceFramebufferID, dstTextureID, width, height,
                          nonBlackSamples, nonTransparentSamples,
                          center[0], center[1], center[2], center[3]);
        }
        else
        {
            OLO_CORE_TRACE("RenderGraphFrameCapture[{}|{}:{}]: blit OK src tex {} fb {} -> dst tex {} ({}x{}, nonBlack={}/9, nonTransparent={}/9, center=({},{},{},{}))",
                           passName, SourceName(source), resourceName, sourceTextureID, sourceFramebufferID, dstTextureID, width, height,
                           nonBlackSamples, nonTransparentSamples,
                           center[0], center[1], center[2], center[3]);
        }

        m_Captures.push_back(CaptureEntry{
            .PassName = passName,
            .ResourceName = std::string(resourceName),
            .SourceKind = source,
            .TextureID = dstTextureID,
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
        if (const auto it = std::ranges::find(passOrder, passName); it != passOrder.end())
        {
            metadata.PassOrderIndex = static_cast<u32>(std::distance(passOrder.begin(), it));
        }
        metadata.CulledPassCount = static_cast<u32>(graph.GetCulledPasses().size());
        metadata.PlannedBarrierCount = static_cast<u32>(graph.GetPlannedBarriers().size());
        metadata.ResourceCount = static_cast<u32>(graph.GetRegisteredResources().size());

        const auto passIndexOf = [&passOrder](std::string_view name) -> u32
        {
            const auto it = std::ranges::find_if(passOrder,
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

        const auto captureFB = [this, &emittedDiag, &passName, &metadata](Source kind, std::string_view resourceName, const Ref<Framebuffer>& fb)
        {
            if (!fb)
                return;

            const auto& spec = fb->GetSpecification();
            if (CountColorAttachments(spec) == 0)
                return;

            const RHI::ResourceHandle colorAttachment = fb->GetColorAttachmentHandle(0);
            if (!colorAttachment.IsValid())
            {
                return;
            }

            // One-shot diagnostic per capture so we can verify the live FB
            // matches what the pass actually rendered into.
            if (!m_DiagLogged)
            {
                const sizet attachmentCount = spec.Attachments.Attachments.size();
                OLO_CORE_INFO("RenderGraphFrameCapture[live {}:{}]: fbGL={} attachments={} colorTex0={} ({}x{})",
                              SourceName(kind), resourceName, fb->GetRendererID(), attachmentCount,
                              Debug::NativeTextureIdForDiagnostics(colorAttachment), spec.Width, spec.Height);
                emittedDiag = true;
            }

            CaptureFramebuffer(passName, kind, colorAttachment, spec.Width, spec.Height,
                               resourceName, fb->GetRendererID(), metadata);
        };

        const auto captureTexture = [this, &emittedDiag, &passName, &metadata](Source kind, std::string_view resourceName, RHI::ResourceHandle texture, u32 width, u32 height, u32 sourceFramebufferID = 0)
        {
            if (!texture.IsValid() || width == 0 || height == 0)
                return;
            if (!m_DiagLogged)
            {
                OLO_CORE_INFO("RenderGraphFrameCapture[live {}:{}]: tex={} fb={} ({}x{})",
                              SourceName(kind), resourceName, texture, sourceFramebufferID, width, height);
                emittedDiag = true;
            }
            CaptureFramebuffer(passName, kind, texture, width, height, resourceName, sourceFramebufferID, metadata);
        };

        const auto captureGraphTexture = [&graph, &captureTexture](Source kind, std::string_view resourceName)
        {
            const RGTextureHandle rgHandle = graph.GetTextureHandle(resourceName);
            const RHI::ResourceHandle texture = graph.ResolveTextureHandle(rgHandle);
            if (!texture.IsValid())
            {
                // Dual-currency guard (see RenderGraphResourceIdentity.h): a
                // resource still imported as a raw native id has no identity
                // to give, and the facade capture path cannot reach it. Warn
                // instead of silently dropping it from the capture set — the
                // #732 SSAO-noise regression was exactly this shape.
                if (Debug::NativeTextureIdForDiagnostics(graph, rgHandle) != 0)
                {
                    OLO_CORE_WARN("RenderGraphFrameCapture[{}]: resource carries only a native id and "
                                  "cannot be captured through the facade path",
                                  resourceName);
                }
                return;
            }

            u32 width = 0;
            u32 height = 0;
            RenderCommand::GetTextureDimensions(texture, 0, width, height);
            if (width == 0 || height == 0)
            {
                return;
            }

            captureTexture(kind, resourceName, texture, width, height);
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
        if (passName == "ColorBlindPass")
            captureFB(Source::ColorBlindColor, ResourceNames::ColorBlindColor, Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ColorBlindColor));

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
            // ColorBlindColor first (issue #458): the accessibility stage runs after
            // UICompositePass, so it — not UIComposite — is what FinalPass presents
            // whenever a mode is active. Source::Backbuffer means "the image the
            // player saw", and reading UIComposite here would quietly answer with the
            // frame from one stage earlier.
            if (const auto colorBlindFramebuffer = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ColorBlindColor); colorBlindFramebuffer)
            {
                captureFB(Source::Backbuffer, ResourceNames::Backbuffer, colorBlindFramebuffer);
                capturedPresentedImage = true;
            }
            else if (const auto uiFramebuffer = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite); uiFramebuffer)
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

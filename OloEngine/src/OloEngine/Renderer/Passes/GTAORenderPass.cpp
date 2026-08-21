#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RGBuilder.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/Passes/AOTargetIdentity.h"
#include "OloEngine/Renderer/Passes/GTAORenderPass.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

namespace OloEngine
{
    // Hilbert curve LUT: maps (x,y) in a 64×64 grid to a 1D index.
    // Used for spatiotemporal noise in GTAO to decorrelate samples.
    static constexpr u32 HILBERT_SIZE = 64;
    static constexpr u32 GTAO_HZB_TEXTURE_SLOT = 3;
    static constexpr u32 GTAO_NORMALS_TEXTURE_SLOT = 4;
    static constexpr u32 GTAO_HILBERT_TEXTURE_SLOT = 5;

    // Generate Hilbert curve index for a given (x, y) coordinate
    static u16 HilbertIndex(u32 x, u32 y)
    {
        u32 d = 0;
        for (u32 s = HILBERT_SIZE / 2; s > 0; s /= 2)
        {
            u32 rx = (x & s) > 0 ? 1 : 0;
            u32 ry = (y & s) > 0 ? 1 : 0;
            d += s * s * ((3 * rx) ^ ry);
            // Rotate
            if (ry == 0)
            {
                if (rx == 1)
                {
                    x = s - 1 - x;
                    y = s - 1 - y;
                }
                std::swap(x, y);
            }
        }
        return static_cast<u16>(d & 0xFFFF);
    }

    GTAORenderPass::GTAORenderPass()
    {
        SetName("GTAOPass");
        // Phase G slice 1 — compute-only; HZB + GTAO main pass + denoise all dispatch compute.
        // Candidate for async-compute overlap once multi-queue scheduling is added (Phase G.2).
        SetPassWorkType(PassWorkType::Compute);
        SetAsyncComputeCandidate(true);
    }

    void GTAORenderPass::Setup(RGBuilder& builder, FrameBlackboard& blackboard)
    {
        RenderGraphNode::Setup(builder, blackboard);
        m_SelectedSceneDepthTexture = {};
        m_SelectedSceneNormalsTexture = {};
        m_SceneNormalsAreViewSpace = false;
        m_SelectedAOOutputTexture = {};
        m_SelectedEdgeTexture = {};
        m_SelectedHZBDepthTexture = {};
        m_SelectedDenoisePingTexture = {};
        m_SelectedDenoisePongTexture = {};

        if (!m_Settings.GTAOEnabled || m_Settings.ActiveAOTechnique != AOTechnique::GTAO)
            return;

        const bool willDispatchDenoise = m_Settings.GTAODenoiseEnabled && m_Settings.GTAODenoisePasses > 0;

        if (blackboard.Scene.SceneDepth.IsValid())
        {
            m_SelectedSceneDepthTexture = blackboard.Scene.SceneDepth;
            [[maybe_unused]] const auto sceneDepthRead = builder.Read(blackboard.Scene.SceneDepth, RGReadUsage::ShaderSample);
        }
        if (blackboard.Scene.SceneNormals.IsValid())
        {
            m_SelectedSceneNormalsTexture = blackboard.Scene.SceneNormals;
            // GTAO.comp converts its input to view space with u_ViewMatrix. The
            // deferred G-Buffer stores world-space normals, so that conversion is
            // correct there — but the forward path hands us normals the PBR shader
            // already put in view space, and transforming those again produced
            // garbage that read as full occlusion on every surface.
            m_SceneNormalsAreViewSpace = blackboard.Scene.SceneNormalsAreViewSpace;
            [[maybe_unused]] const auto sceneNormalsRead = builder.Read(blackboard.Scene.SceneNormals, RGReadUsage::ShaderSample);
        }
        if (blackboard.AO.AOBuffer.IsValid())
        {
            m_SelectedAOOutputTexture = blackboard.AO.AOBuffer;
            // AOBuffer is written via glCopyImageSubData from the final denoise
            // ping-pong slot, not a compute image-store. The dispatch path
            // writes to GTAODenoisePing/Pong (declared as ShaderImage above).
            builder.Write(blackboard.AO.AOBuffer, RGWriteUsage::TransferDest);
        }

        if (m_Width == 0u || m_Height == 0u)
            return;

        const auto nextPow2 = [](u32 value)
        {
            u32 result = 1u;
            while (result < value)
                result <<= 1u;
            return result;
        };

        const auto hzbW = nextPow2(m_Width);
        const auto hzbH = nextPow2(m_Height);
        u32 mipCount = 1u;
        for (u32 mipW = hzbW, mipH = hzbH; mipW > 1u || mipH > 1u; ++mipCount)
        {
            mipW = mipW > 1u ? (mipW / 2u) : 1u;
            mipH = mipH > 1u ? (mipH / 2u) : 1u;
        }

        if (blackboard.Scratch.HZBDepth.IsValid())
        {
            m_SelectedHZBDepthTexture = blackboard.Scratch.HZBDepth;
            // Intra-pass HZB mip-chain reduction: each compute dispatch reads
            // mip[i-1] and writes mip[i] of the same texture in sequence.
            builder.AllowSamePassReadWrite(blackboard.Scratch.HZBDepth);

            bool canUseMipViews = mipCount <= static_cast<u32>(blackboard.Scratch.HZBDepthMipViews.size());
            if (canUseMipViews)
            {
                for (u32 mip = 0u; mip < mipCount; ++mip)
                {
                    if (!blackboard.Scratch.HZBDepthMipViews[mip].IsValid())
                    {
                        canUseMipViews = false;
                        break;
                    }
                }
            }

            if (canUseMipViews)
            {
                builder.Write(blackboard.Scratch.HZBDepthMipViews[0u], RGWriteUsage::ShaderImage);
                for (u32 mip = 1u; mip < mipCount; ++mip)
                {
                    [[maybe_unused]] const auto hzbMipRead =
                        builder.Read(blackboard.Scratch.HZBDepthMipViews[mip - 1u], RGReadUsage::ShaderSample);
                    builder.Write(blackboard.Scratch.HZBDepthMipViews[mip], RGWriteUsage::ShaderImage);
                }
            }
            else
            {
                builder.Write(blackboard.Scratch.HZBDepth, RGWriteUsage::ShaderImage, RGSubresourceRange::Mip(0u));
                for (u32 mip = 1u; mip < mipCount; ++mip)
                {
                    [[maybe_unused]] const auto hzbMipRead =
                        builder.Read(blackboard.Scratch.HZBDepth, RGReadUsage::ShaderSample, RGSubresourceRange::Mip(mip - 1u));
                    builder.Write(blackboard.Scratch.HZBDepth, RGWriteUsage::ShaderImage, RGSubresourceRange::Mip(mip));
                }
            }

            [[maybe_unused]] const auto hzbRead = builder.Read(blackboard.Scratch.HZBDepth, RGReadUsage::ShaderSample);
        }

        if (blackboard.Scratch.GTAOEdge.IsValid())
        {
            m_SelectedEdgeTexture = blackboard.Scratch.GTAOEdge;
            // Intra-pass write-then-imageLoad: the GTAO main dispatch writes
            // GTAOEdge, then the denoise dispatch reads it back via imageLoad.
            builder.AllowSamePassReadWrite(blackboard.Scratch.GTAOEdge);
            builder.Write(blackboard.Scratch.GTAOEdge, RGWriteUsage::ShaderImage);
            [[maybe_unused]] const auto edgeRead = builder.Read(blackboard.Scratch.GTAOEdge, RGReadUsage::ShaderImage);
        }

        if (blackboard.Scratch.GTAODenoisePing.IsValid())
        {
            m_SelectedDenoisePingTexture = blackboard.Scratch.GTAODenoisePing;
            if (willDispatchDenoise)
            {
                // Intra-pass denoise ping-pong: each denoise iteration reads
                // one and writes the other; the chain alternates inside this
                // single Execute.
                builder.AllowSamePassReadWrite(blackboard.Scratch.GTAODenoisePing);
                builder.Write(blackboard.Scratch.GTAODenoisePing, RGWriteUsage::ShaderImage);
                [[maybe_unused]] const auto pingRead = builder.Read(blackboard.Scratch.GTAODenoisePing, RGReadUsage::ShaderImage);
            }
            else
            {
                builder.Write(blackboard.Scratch.GTAODenoisePing, RGWriteUsage::ShaderImage);
            }
        }

        if (willDispatchDenoise && blackboard.Scratch.GTAODenoisePong.IsValid())
        {
            m_SelectedDenoisePongTexture = blackboard.Scratch.GTAODenoisePong;
            // Intra-pass denoise ping-pong (see GTAODenoisePing above).
            builder.AllowSamePassReadWrite(blackboard.Scratch.GTAODenoisePong);
            builder.Write(blackboard.Scratch.GTAODenoisePong, RGWriteUsage::ShaderImage);
            [[maybe_unused]] const auto pongRead = builder.Read(blackboard.Scratch.GTAODenoisePong, RGReadUsage::ShaderImage);
        }
    }

    GTAORenderPass::~GTAORenderPass() = default;

    void GTAORenderPass::Init(const FramebufferSpecification& spec)
    {
        OLO_PROFILE_FUNCTION();

        m_FramebufferSpec = spec;
        m_Width = spec.Width;
        m_Height = spec.Height;

        // Initialize HZB generator
        m_HZBGenerator.Initialize();

        // Load compute shaders
        m_GTAOShader = ComputeShader::Create("assets/shaders/compute/GTAO.comp");
        m_DenoiseShader = ComputeShader::Create("assets/shaders/compute/GTAO_Denoise.comp");

        // Generate Hilbert LUT
        GenerateHilbertLUT();

        // Initialize HZB for current viewport
        m_HZBGenerator.Resize(m_Width, m_Height);

        OLO_CORE_INFO("GTAORenderPass: Initialized at {}x{}", m_Width, m_Height);
    }

    void GTAORenderPass::GenerateHilbertLUT()
    {
        OLO_PROFILE_FUNCTION();

        TextureSpecification lutSpec;
        lutSpec.Width = HILBERT_SIZE;
        lutSpec.Height = HILBERT_SIZE;
        lutSpec.Format = ImageFormat::R16UI;
        lutSpec.GenerateMips = false;
        lutSpec.MipLevels = 1;

        m_HilbertLUT = Texture2D::Create(lutSpec);

        // Generate LUT data
        std::vector<u16> data(HILBERT_SIZE * HILBERT_SIZE);
        for (u32 y = 0; y < HILBERT_SIZE; ++y)
        {
            for (u32 x = 0; x < HILBERT_SIZE; ++x)
            {
                data[y * HILBERT_SIZE + x] = HilbertIndex(x, y);
            }
        }

        m_HilbertLUT->SetData(data.data(), static_cast<u32>(data.size() * sizeof(u16)));
    }

    void GTAORenderPass::Execute(RGCommandContext& context)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Settings.GTAOEnabled || m_Settings.ActiveAOTechnique != AOTechnique::GTAO || !IsReadyForExecution())
        {
            return;
        }

        // Phase D / H follow-up: resolve the GTAO edge scratch texture from
        // the transient pool only. The execute path no longer falls back to
        // an owned edge texture.
        //
        // Resolved BEFORE the depth/normals guard below, deliberately: that
        // guard is one of the early returns that has to leave the AO target in
        // the "no occlusion" state, so it needs the handle in hand (#771).
        RHI::ResourceHandle edgeTexID{};
        RHI::ResourceHandle aoOutputTexID{};
        RHI::ResourceHandle denoisePingTexID{};
        RHI::ResourceHandle denoisePongTexID{};
        if (m_SelectedAOOutputTexture.IsValid())
            aoOutputTexID = context.ResolveTextureHandle(m_SelectedAOOutputTexture);
        if (m_SelectedEdgeTexture.IsValid())
            edgeTexID = context.ResolveTextureHandle(m_SelectedEdgeTexture);
        if (m_SelectedDenoisePingTexture.IsValid())
            denoisePingTexID = context.ResolveTextureHandle(m_SelectedDenoisePingTexture);

        const bool willDispatchDenoise = m_Settings.GTAODenoiseEnabled && m_Settings.GTAODenoisePasses > 0;
        if (willDispatchDenoise && m_SelectedDenoisePongTexture.IsValid())
            denoisePongTexID = context.ResolveTextureHandle(m_SelectedDenoisePongTexture);

        // Phase F slice 37 — self-resolving SceneDepth and SceneNormals: look
        // up directly from the render graph blackboard so no per-frame
        // side-channel setter calls are needed from EndScene().
        RHI::ResourceHandle depthID{};
        RHI::ResourceHandle normalsID{};
        if (m_SelectedSceneDepthTexture.IsValid())
            depthID = context.ResolveTextureHandle(m_SelectedSceneDepthTexture);
        if (m_SelectedSceneNormalsTexture.IsValid())
            normalsID = context.ResolveTextureHandle(m_SelectedSceneNormalsTexture);
        if (!depthID.IsValid() || !normalsID.IsValid())
        {
            PublishNoOcclusion(aoOutputTexID, "scene depth/normals did not resolve");
            return;
        }

        // From here on the AO target is RESOLVED, which means AOApplyPass will
        // sample it this frame whether or not we manage to produce anything.
        // Every remaining early return therefore has to leave it holding the
        // "no occlusion" identity (1.0) instead of whatever the transient pool
        // handed us — see PublishNoOcclusion (issue #771).
        if (!edgeTexID.IsValid() || !aoOutputTexID.IsValid() || !denoisePingTexID.IsValid())
        {
            PublishNoOcclusion(aoOutputTexID, "edge/AO/denoise-ping scratch did not resolve");
            return;
        }
        if (willDispatchDenoise && !denoisePongTexID.IsValid())
        {
            PublishNoOcclusion(aoOutputTexID, "denoise-pong scratch did not resolve");
            return;
        }

        // Phase D / H follow-up: resolve transient HZB scratch from the render
        // graph and require it to exist for execution.
        RHI::ResourceHandle transientHZBID{};
        if (m_SelectedHZBDepthTexture.IsValid())
            transientHZBID = context.ResolveTextureHandle(m_SelectedHZBDepthTexture);
        if (!transientHZBID.IsValid())
        {
            m_HZBGenerator.ClearExternalHZBTexture();
            PublishNoOcclusion(aoOutputTexID, "HZB scratch did not resolve");
            return;
        }
        m_HZBGenerator.SetExternalHZBTexture(transientHZBID, m_HZBGenerator.GetMipCount());

        // The structural event (issue #771). A render-band change — the FSR1
        // `Upscale` toggle is the one that bites — resizes every GTAO scratch
        // target, and RenderPipeline::PopulateBlackboard evicts the transient
        // pool on the same edge (#563), so this frame's AO chain lands on
        // FRESHLY ALLOCATED storage. Fresh VRAM is not specified, and when the
        // driver hands back zeroes the AO target reads as AO = 0 — *maximum*
        // occlusion — which PostProcess_SSAOApply multiplies straight through
        // to an exactly-black frame. Recycled (dirty, non-zero) pages hid it,
        // which is why the failure tracked allocation history rather than any
        // code path. Seed the identity before the dispatch so the band-change
        // frame can never present uninitialised storage as full occlusion.
        // Only the AO TARGET is seeded here, not the ping/pong/edge scratch:
        // GTAO.comp and GTAO_Denoise.comp both bounds-check against
        // u_ScreenWidth/Height and write every texel inside it, and the denoise
        // reads nothing outside that region — so the scratch chain has no
        // read-before-write to protect. The AO target does: the final
        // CopyImageSubData covers only m_Width x m_Height of it.
        if (m_Width != m_LastExecutedWidth || m_Height != m_LastExecutedHeight)
        {
            PublishNoOcclusion(aoOutputTexID, nullptr);
            m_LastExecutedWidth = m_Width;
            m_LastExecutedHeight = m_Height;
        }

        // (The previous "log on input change" diagnostic was dropped: the AO
        // output is double-buffered, so the texture ID flips every frame and
        // the dedup never holds — it fired ~60 times per second. If you need
        // to trace AO inputs again, drop a one-shot OLO_CORE_TRACE here.)

        // Step 1: Generate HZB from scene depth
        //
        // Sub-pass brackets (issue #720) isolate each dispatch's GPU-ms under
        // GPUPassTimerPool's "GTAOPass" bracket instead of folding HZB, GTAO
        // and denoise into one undifferentiated number. This is what let the
        // per-shader thread-group swizzle measurement find that GTAO.comp and
        // HZB.comp regress under the swizzle (see ThreadGroupSwizzle.glsl) —
        // both dispatches below are therefore UNSWIZZLED; only GTAO_Denoise
        // adopted it. The brackets stay regardless, for future profiling.
        auto& gpuSubTimers = GPUPassTimerPool::GetInstance();
        gpuSubTimers.BeginSubPass("HZB");
        m_HZBGenerator.Generate(depthID);
        gpuSubTimers.EndSubPass();

        // Step 2: Upload GTAO uniforms
        UploadGTAOUniforms();

        // Step 3: Dispatch GTAO main pass
        gpuSubTimers.BeginSubPass("GTAO");
        DispatchGTAO(denoisePingTexID, normalsID, edgeTexID);
        gpuSubTimers.EndSubPass();

        // Step 4: Denoise (if enabled)
        if (willDispatchDenoise)
        {
            gpuSubTimers.BeginSubPass("GTAO_Denoise");
            DispatchDenoise(edgeTexID, denoisePingTexID, denoisePongTexID);
            gpuSubTimers.EndSubPass();
        }

        const RHI::ResourceHandle finalAOTextureID = (willDispatchDenoise && (m_Settings.GTAODenoisePasses % 2 != 0))
                                                         ? denoisePongTexID
                                                         : denoisePingTexID;
        if (finalAOTextureID.IsValid() && finalAOTextureID != aoOutputTexID)
        {
            RenderCommand::MemoryBarrier(
                MemoryBarrierFlags::ShaderImageAccess |
                MemoryBarrierFlags::TextureFetch |
                MemoryBarrierFlags::TextureUpdate);

            RenderCommand::CopyImageSubData(finalAOTextureID, RendererAPI::TextureTargetType::Texture2D,
                                            aoOutputTexID, RendererAPI::TextureTargetType::Texture2D,
                                            m_Width, m_Height);
        }
    }

    void GTAORenderPass::PublishNoOcclusion(RHI::ResourceHandle aoOutputTexture, const char* skipReason)
    {
        if (!aoOutputTexture.IsValid())
            return;

        // 1.0 == fully visible. AOApplyPass computes `sceneColor * mix(1, ao,
        // intensity)`, so this is the only value that makes "GTAO produced
        // nothing this frame" a no-op rather than a black frame. GTAO.comp
        // itself never emits below 0.03 (its visibility clamp), so an AO texel
        // of exactly 0 reaching the apply pass always means unwritten storage.
        PublishAOTargetAsFullyVisible(aoOutputTexture);

        // Every skip that reaches here used to be a bare `return;`, which is
        // what let issue #771 read as an order-dependent flake for weeks.
        // Rate-limited: a persistent resolve failure would otherwise log once
        // per frame.
        if (skipReason != nullptr)
        {
            if (static u32 s_SkipWarnings = 0; s_SkipWarnings++ < 10)
            {
                OLO_CORE_WARN("GTAORenderPass: skipped this frame ({}); AO target published as "
                              "fully visible so the apply pass does not multiply the scene by "
                              "uninitialised storage.",
                              skipReason);
            }
        }
    }

    void GTAORenderPass::UploadGTAOUniforms()
    {
        if (!m_GTAOUBO || !m_GPUData)
        {
            return;
        }

        f32 projScale00 = m_Projection[0][0];
        f32 projScale11 = m_Projection[1][1];

        // NDCToView: unproject from normalized screen [0,1] to view-space XY.
        //
        // GL convention on BOTH axes: (2u - 1) / proj00 and (2v - 1) / proj11.
        // The XeGTAO reference negates the Y pair (-2/proj11, +1/proj11)
        // because D3D texture coordinates put v = 0 at the TOP row; this port
        // consumes GL-convention inputs everywhere (the compute's pixCoord
        // row 0 is the framebuffer BOTTOM: the HZB is a 1:1 texelFetch copy
        // of the scene depth and the view-normals texture is fetched with the
        // same coordinates), so keeping the D3D flip NEGATED view-space Y for
        // every reconstructed sample position while the decoded surface
        // normals stayed correct. All horizon angles were reflected about the
        // horizontal plane: invisible looking straight down (symmetric), a
        // full-frame visibility collapse to the 0.03 floor at grazing views
        // (the sea / quay "goosebumps" weave rode on that collapsed AO).
        m_GPUData->NDCToViewMul = glm::vec2(2.0f / projScale00, 2.0f / projScale11);
        m_GPUData->NDCToViewAdd = glm::vec2(-1.0f / projScale00, -1.0f / projScale11);

        f32 pixelSizeX = 1.0f / static_cast<f32>(m_Width);
        f32 pixelSizeY = 1.0f / static_cast<f32>(m_Height);
        m_GPUData->NDCToViewMul_x_PixelSize = m_GPUData->NDCToViewMul * glm::vec2(pixelSizeX, pixelSizeY);

        m_GPUData->EffectRadius = m_Settings.GTAORadius;
        m_GPUData->FinalValuePower = m_Settings.GTAOPower;
        m_GPUData->EffectFalloffRange = m_Settings.GTAOFalloffRange;
        m_GPUData->SampleDistributionPower = m_Settings.GTAOSampleDistribution;
        m_GPUData->ThinOccluderCompensation = m_Settings.GTAOThinCompensation;
        m_GPUData->DepthMIPSamplingOffset = m_Settings.GTAODepthMipOffset;
        m_GPUData->DenoiseBlurBeta = m_Settings.GTAODenoiseBeta;

        // Depth linearization: proj[2][2] and proj[3][2]
        m_GPUData->DepthLinearizeA = m_Projection[2][2];
        m_GPUData->DepthLinearizeB = m_Projection[3][2];

        m_GPUData->HZBUVFactor = m_HZBGenerator.GetUVFactor();
        m_GPUData->ScreenWidth = static_cast<i32>(m_Width);
        m_GPUData->ScreenHeight = static_cast<i32>(m_Height);

        m_GPUData->DenoiseEnabled = m_Settings.GTAODenoiseEnabled ? 1 : 0;
        m_GPUData->DenoisePasses = m_Settings.GTAODenoisePasses;
        m_GPUData->DebugView = m_Settings.GTAODebugView ? 1 : 0;

        // Transforms world-space G-Buffer normals to view space. When the source is
        // ALREADY view space (the forward path's scene-colour RT2, written as
        // octEncode(mat3(u_View) * N) by PBR_MultiLight.glsl) this must be identity
        // instead — applying the view matrix a second time rotates every normal out
        // of the hemisphere the horizon search assumes and drives AO to zero
        // everywhere, swimming as the camera turns.
        m_GPUData->ViewMatrix = m_SceneNormalsAreViewSpace ? glm::mat4(1.0f) : m_ViewMatrix;

        m_GTAOUBO->SetData(m_GPUData, UBOStructures::GTAOUBO::GetSize());
        m_GTAOUBO->Bind();
    }

    void GTAORenderPass::DispatchGTAO(RHI::ResourceHandle aoOutputTextureID, RHI::ResourceHandle normalsTextureID,
                                      RHI::ResourceHandle edgeTexID)
    {
        OLO_PROFILE_FUNCTION();

        m_GTAOShader->Bind();

        // Bind output images.
        //
        // FrameTransient: every one of these comes from context.ResolveTextureHandle,
        // i.e. the render graph's transient pool, so their descriptors must come from
        // the per-frame ring. A Persistent view of a pooled target would memoise an
        // offset onto an object the pool can hand to a different logical resource next
        // frame — the aliasing hazard the two lifetime classes exist to separate.
        HeapBinding::BindImageOrOffset(0, aoOutputTextureID, 0, false, 0, RHI::Access::StorageWrite,
                                       RHI::Format::R8UNorm, RHI::HeapSlotLifetime::FrameTransient);
        HeapBinding::BindImageOrOffset(1, edgeTexID, 0, false, 0, RHI::Access::StorageWrite,
                                       RHI::Format::R8UNorm, RHI::HeapSlotLifetime::FrameTransient);

        // Bind inputs. The lifetimes differ per input and are not interchangeable,
        // and the HZB's is NOT a property of this pass — it must be asked for.
        //
        // GetHZBTexture() returns the generator's own pyramid only when nothing has
        // called SetExternalHZBTexture. The GTAO path routinely supplies the
        // graph-pooled Scratch.HZBDepth instead, and a Persistent view of a pooled
        // target memoises an offset onto an object the planner may reassign next
        // frame — so GetHZBLifetime() answers FrameTransient in exactly that case.
        // An earlier version of this comment called the HZB "pass-owned
        // (memoisable, Persistent)", which is true only for the generator-owned
        // half and was the reasoning behind the wrong hard-coded lifetime here.
        //
        // The Hilbert LUT genuinely is pass-owned and stays Persistent; the view
        // normals come from the transient pool and take a per-frame ring slot
        // (issue #691 Phase 3).
        const RHI::ResourceHandle hzbID = m_HZBGenerator.GetHZBTexture();
        HeapBinding::BindTextureOrOffset(GTAO_HZB_TEXTURE_SLOT, hzbID,
                                         m_HZBGenerator.GetHZBLifetime());

        HeapBinding::BindTextureOrOffset(GTAO_NORMALS_TEXTURE_SLOT, normalsTextureID,
                                         RHI::HeapSlotLifetime::FrameTransient);

        HeapBinding::BindTextureOrOffset(GTAO_HILBERT_TEXTURE_SLOT, m_HilbertLUT->GetRHIHandle(),
                                         RHI::HeapSlotLifetime::Persistent);

        // Dispatch 16×16 workgroups
        u32 groupsX = (m_Width + 15) / 16;
        u32 groupsY = (m_Height + 15) / 16;
        HeapBinding::FlushOffsets();
        RenderCommand::DispatchCompute(groupsX, groupsY, 1);

        // Barrier: AO + edges must be visible for denoise
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch);

        m_GTAOShader->Unbind();
    }

    void GTAORenderPass::DispatchDenoise(RHI::ResourceHandle edgeTexID, RHI::ResourceHandle pingTextureID,
                                         RHI::ResourceHandle pongTextureID)
    {
        OLO_PROFILE_FUNCTION();

        m_DenoiseShader->Bind();

        u32 groupsX = (m_Width + 7) / 8;
        u32 groupsY = (m_Height + 7) / 8;

        // Edge texture is always read-only.
        //
        // NOTE this is the SAME texture DispatchGTAO just bound for WRITING. GL folds
        // the two to one image handle (glGetImageHandleARB takes no access), so the
        // backend widens its residency to READ_WRITE rather than transitioning it
        // twice — reading a WRITE_ONLY-resident handle is undefined, and a second
        // residency transition is an INVALID_OPERATION.
        HeapBinding::BindImageOrOffset(2, edgeTexID, 0, false, 0, RHI::Access::StorageRead,
                                       RHI::Format::R8UNorm, RHI::HeapSlotLifetime::FrameTransient);

        i32 passes = m_Settings.GTAODenoisePasses;
        bool readFromTex0 = true;

        // Former bare uniform via ComputeShader::SetInt — a no-op on the
        // Vulkan route (issue #691 Phase 7). Refilled per pass: each SetData
        // is a fresh per-dispatch address on the arena-versioned backend.
        if (!m_DenoiseUBO)
        {
            m_DenoiseUBO = UniformBuffer::Create(UBOStructures::GTAODenoiseUBO::GetSize(),
                                                 ShaderBindingLayout::UBO_GTAO_DENOISE);
        }

        for (i32 pass = 0; pass < passes; ++pass)
        {
            // Alternate horizontal/vertical
            UBOStructures::GTAODenoiseUBO denoiseParams{};
            denoiseParams.BlurHorizontal = (pass % 2 == 0) ? 1 : 0;
            m_DenoiseUBO->SetData(&denoiseParams, sizeof(denoiseParams));
            m_DenoiseUBO->Bind();

            if (readFromTex0)
            {
                HeapBinding::BindImageOrOffset(0, pingTextureID, 0, false, 0, RHI::Access::StorageRead,
                                               RHI::Format::R8UNorm, RHI::HeapSlotLifetime::FrameTransient);
                HeapBinding::BindImageOrOffset(1, pongTextureID, 0, false, 0, RHI::Access::StorageWrite,
                                               RHI::Format::R8UNorm, RHI::HeapSlotLifetime::FrameTransient);
            }
            else
            {
                HeapBinding::BindImageOrOffset(0, pongTextureID, 0, false, 0, RHI::Access::StorageRead,
                                               RHI::Format::R8UNorm, RHI::HeapSlotLifetime::FrameTransient);
                HeapBinding::BindImageOrOffset(1, pingTextureID, 0, false, 0, RHI::Access::StorageWrite,
                                               RHI::Format::R8UNorm, RHI::HeapSlotLifetime::FrameTransient);
            }

            // Per ITERATION: ping and pong swap roles every pass, so a flush hoisted
            // out of the loop would publish only the final pass's pair.
            HeapBinding::FlushOffsets();
            RenderCommand::DispatchCompute(groupsX, groupsY, 1);
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch);

            readFromTex0 = !readFromTex0;
        }

        m_DenoiseShader->Unbind();
    }
    void GTAORenderPass::SetupFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            return;
        }

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        m_Width = width;
        m_Height = height;

        m_HZBGenerator.Resize(width, height);
    }

    void GTAORenderPass::ResizeFramebuffer(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();

        if (width == 0 || height == 0)
        {
            return;
        }

        m_FramebufferSpec.Width = width;
        m_FramebufferSpec.Height = height;
        m_Width = width;
        m_Height = height;

        m_HZBGenerator.Resize(width, height);

        OLO_CORE_INFO("GTAORenderPass: Resized to {}x{}", width, height);
    }

    void GTAORenderPass::OnReset()
    {
        // Advance the spatio-temporal noise phase ONLY when TAA is enabled.
        // XeGTAO's animated noise index exists to be temporally resolved by
        // TAA; without TAA the R1/Hilbert pattern visibly boils every frame —
        // the "goosebumps" weave over the water and distant quay. With a
        // fixed phase the residual pattern is static and the (edge-aware)
        // denoise output is temporally stable, matching reference XeGTAO
        // behavior for the no-TAA configuration.
        if (m_GPUData && m_Settings.TAAEnabled)
        {
            m_GPUData->NoiseIndex = (m_GPUData->NoiseIndex + 1) % 256;
        }
        m_SelectedSceneDepthTexture = {};
        m_SelectedSceneNormalsTexture = {};
        m_SceneNormalsAreViewSpace = false;
        m_SelectedAOOutputTexture = {};
        m_SelectedEdgeTexture = {};
        m_SelectedHZBDepthTexture = {};
        m_SelectedDenoisePingTexture = {};
        m_SelectedDenoisePongTexture = {};
    }
} // namespace OloEngine

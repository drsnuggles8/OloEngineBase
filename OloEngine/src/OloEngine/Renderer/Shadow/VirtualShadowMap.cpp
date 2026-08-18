#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Shadow/VirtualShadowMap.h"

#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/Passes/ShadowRenderPass.h"
#include "OloEngine/Renderer/RHI/RHIProjectionSeam.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace OloEngine
{
    namespace
    {
        // Positive modulo. The frustum origin in page units goes negative as soon
        // as the camera crosses the render origin, and C++'s % would then hand the
        // wrap maths a negative offset — which addresses the page table out of
        // bounds instead of wrapping it.
        [[nodiscard]] constexpr i32 PositiveMod(i32 value, i32 modulus)
        {
            const i32 rem = value % modulus;
            return (rem < 0) ? (rem + modulus) : rem;
        }

        [[nodiscard]] constexpr u32 DivideRoundUp(u32 value, u32 divisor)
        {
            return (value + divisor - 1) / divisor;
        }

        constexpr u32 kFreeListHeaderUints = 4;
        constexpr u32 kRequestHeaderUints = 4;
        constexpr u32 kInvalidationHeaderVec4s = 1;
        constexpr u32 kMaxInvalidations = 1024;

        // The local-light buffer's std430 layout, mirrored from the VSMLocalLights
        // block in include/VirtualShadowResources.glsl. Three uint[kMaxLocalLayers]
        // header arrays, then the unsized LocalLight tail — that order is forced,
        // because std430 allows only the LAST member to be unsized.
        constexpr u32 kLocalRasterMipOffset = 0;
        constexpr u32 kLocalHeadOffset = VSM::kMaxLocalLayers * sizeof(u32);
        constexpr u32 kLocalInvalidateOffset = kLocalHeadOffset + VSM::kMaxLocalLayers * sizeof(u32);
        constexpr u32 kLocalLayersOffset = kLocalInvalidateOffset + VSM::kMaxLocalLayers * sizeof(u32);
        constexpr u32 kLocalBufferBytes = kLocalLayersOffset + VSM::kMaxLocalLayers * sizeof(VSM::LocalLight);
        static_assert(kLocalLayersOffset % 16 == 0,
                      "the LocalLight tail must start 16-byte aligned or std430 pads the header");

        // Face basis for a point light's six layers, in the +X,-X,+Y,-Y,+Z,-Z
        // order vsmCubeFace / atlasCubeFace select by dominant axis. Copied from
        // ShadowMap::BuildPointLightFaceMatrices rather than shared through it,
        // because these are built in RENDER-RELATIVE space — but the axes and the
        // up vectors must stay identical, which VirtualShadowMapLocalTest pins.
        constexpr std::array<glm::vec3, 6> kCubeFaceForward{ {
            { 1.0f, 0.0f, 0.0f },
            { -1.0f, 0.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f },
            { 0.0f, -1.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f },
            { 0.0f, 0.0f, -1.0f },
        } };
        constexpr std::array<glm::vec3, 6> kCubeFaceUp{ {
            { 0.0f, -1.0f, 0.0f },
            { 0.0f, -1.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f },
            { 0.0f, 0.0f, -1.0f },
            { 0.0f, -1.0f, 0.0f },
            { 0.0f, -1.0f, 0.0f },
        } };

        // The near plane every local face uses. Matches the atlas path's
        // hard-coded 0.1 so the two techniques clip the same geometry, but
        // additionally floored against the range: a 0.5 m range light with a
        // 0.1 m near plane has a depth distribution the bias conversion cannot
        // rescue.
        [[nodiscard]] constexpr f32 LocalNearPlane(f32 range)
        {
            const f32 fromRange = range * 0.01f;
            return (fromRange < 0.1f) ? std::max(fromRange, 1.0e-3f) : 0.1f;
        }
    } // namespace

    VirtualShadowMap::~VirtualShadowMap()
    {
        // Textures and framebuffers are RHI handles, not Ref<>s, so they do not
        // release themselves. Shutdown() is idempotent.
        Shutdown();
    }

    void VirtualShadowMap::Init(const VirtualShadowMapSettings& settings)
    {
        OLO_PROFILE_FUNCTION();

        m_Settings = settings;
        if (!m_Settings.Enabled)
            return;

        m_PhysicalResolution = VirtualShadowMap::SanitizeResolution(m_Settings.PhysicalResolution);
        m_PhysicalPageTableResolution = m_PhysicalResolution / VSM::kPageSize;

        if (!LoadShaders())
        {
            OLO_CORE_ERROR("VirtualShadowMap: shader load failed — falling back to CSM");
            DestroyResources();
            m_Settings.Enabled = false;
            return;
        }

        CreateResources();

        m_FullInvalidate = true;
        m_Initialized = true;

        OLO_CORE_INFO("VirtualShadowMap initialized: {} clip levels of {}^2 virtual texels, {}^2 physical pool "
                      "({} pages of {}^2), {:.1f} MB",
                      VSM::kClipLevels, VSM::kVirtualResolution, m_PhysicalResolution,
                      m_PhysicalPageTableResolution * m_PhysicalPageTableResolution, VSM::kPageSize,
                      static_cast<f64>(GetVRAMBytes()) / (1024.0 * 1024.0));
    }

    bool VirtualShadowMap::LoadShaders()
    {
        OLO_PROFILE_FUNCTION();

        m_FreeWrappedShader = ComputeShader::Create("assets/shaders/compute/VSM_FreeWrappedPages.comp");
        m_MarkShader = ComputeShader::Create("assets/shaders/compute/VSM_MarkRequiredPages.comp");
        m_InvalidateShader = ComputeShader::Create("assets/shaders/compute/VSM_InvalidatePages.comp");
        m_FindFreeShader = ComputeShader::Create("assets/shaders/compute/VSM_FindFreePages.comp");
        m_AllocateShader = ComputeShader::Create("assets/shaders/compute/VSM_AllocatePages.comp");
        m_ClearPagesShader = ComputeShader::Create("assets/shaders/compute/VSM_ClearDirtyPages.comp");
        m_BuildHPBShader = ComputeShader::Create("assets/shaders/compute/VSM_BuildHPB.comp");
        m_CullShader = ComputeShader::Create("assets/shaders/compute/VSM_CullCasters.comp");
        m_CullLocalShader = ComputeShader::Create("assets/shaders/compute/VSM_CullLocalCasters.comp");
        m_EndFrameShader = ComputeShader::Create("assets/shaders/compute/VSM_EndFrame.comp");
        m_DepthShader = Shader::Create("assets/shaders/VSM_Depth.glsl");
        m_DepthSkinnedShader = Shader::Create("assets/shaders/VSM_DepthSkinned.glsl");
        m_DepthLocalShader = Shader::Create("assets/shaders/VSM_DepthLocal.glsl");
        m_DepthLocalSkinnedShader = Shader::Create("assets/shaders/VSM_DepthLocalSkinned.glsl");

        const bool computeOk = m_FreeWrappedShader && m_FreeWrappedShader->IsValid() &&
                               m_MarkShader && m_MarkShader->IsValid() &&
                               m_InvalidateShader && m_InvalidateShader->IsValid() &&
                               m_FindFreeShader && m_FindFreeShader->IsValid() &&
                               m_AllocateShader && m_AllocateShader->IsValid() &&
                               m_ClearPagesShader && m_ClearPagesShader->IsValid() &&
                               m_BuildHPBShader && m_BuildHPBShader->IsValid() &&
                               m_CullShader && m_CullShader->IsValid() &&
                               m_CullLocalShader && m_CullLocalShader->IsValid() &&
                               m_EndFrameShader && m_EndFrameShader->IsValid();
        // IsReady(), not just non-null: Shader::Create can return a live object
        // whose compilation FAILED, and VSM staying "active" with a dead depth
        // raster is precisely the unshadowed-frame-with-no-error state this
        // whole function exists to refuse (the caller falls back to CSM).
        //
        // The two LOCAL rasters are in the same conjunction as the directional
        // pair, not treated as optional: a failed local raster would leave the
        // local lights' pages allocated, dirty and never written, which reads as
        // fully-lit local lights rather than as a missing feature. Degrading the
        // whole system to CSM + atlas is the honest failure.
        return computeOk && m_DepthShader && m_DepthShader->IsReady() &&
               m_DepthSkinnedShader && m_DepthSkinnedShader->IsReady() &&
               m_DepthLocalShader && m_DepthLocalShader->IsReady() &&
               m_DepthLocalSkinnedShader && m_DepthLocalSkinnedShader->IsReady();
    }

    void VirtualShadowMap::CreateResources()
    {
        OLO_PROFILE_FUNCTION();

        const u32 physicalPageCount = m_PhysicalPageTableResolution * m_PhysicalPageTableResolution;

        // R32UI, not a depth format: the raster resolves visibility with
        // imageAtomicMin (which has no float form) because the page indirection
        // happens in the fragment shader and there is no fixed-function depth test
        // to lean on.
        m_PhysicalPool = RenderCommand::CreateTexture2DHandle(m_PhysicalResolution, m_PhysicalResolution,
                                                              RHI::Format::R32UInt);
        RenderCommand::ClearTextureUInt(m_PhysicalPool, 0, 0xFFFFFFFFu);

        // The raster's render SCOPE. GL needs a complete framebuffer to define the
        // render area, and that area must be the VIRTUAL resolution — the physical
        // pool cannot serve, because it is a different (and configurable) size, and
        // a smaller attachment would clip away three quarters of every clip level
        // without any diagnostic. R8UInt is the cheapest renderable format, draw
        // buffers are set to NONE, and nothing is ever written through it: the pass
        // writes only through the image unit.
        m_RasterScope = RenderCommand::CreateTexture2DHandle(VSM::kVirtualResolution, VSM::kVirtualResolution,
                                                             RHI::Format::R8UInt);
        m_RasterFramebuffer = RenderCommand::CreateFramebufferHandle();
        RenderCommand::AttachFramebufferColorTexture(m_RasterFramebuffer, 0, m_RasterScope, 0);
        RenderCommand::SetFramebufferDrawAttachments(m_RasterFramebuffer, {});
        if (!RenderCommand::IsFramebufferComplete(m_RasterFramebuffer) && !m_LoggedRasterIncomplete)
        {
            OLO_CORE_ERROR("VirtualShadowMap: raster framebuffer incomplete — the depth raster will not run");
            m_LoggedRasterIncomplete = true;
        }

        using SB = StorageBuffer;
        constexpr auto kGPUWritten = StorageBufferUsage::DynamicCopy;

        // glMultiDrawElementsIndirectCount sources its draw count from a GPU
        // buffer. Every VSM batch issues exactly one command, so this holds a
        // constant 1 — the count is fixed but the INSTANCE count inside the
        // command is not, and that is the number the cull writes. It reuses
        // SSBO_VSM_DRAW_COMMANDS' binding number but is only ever consumed as
        // an indirect PARAMETER buffer (never bound by BindWorkingSet), so it is
        // created FIRST: StorageBuffer::Create issues an initial bind at its
        // binding point, and creating it after m_DrawCommands would displace the
        // real working-set buffer until the next BindWorkingSet re-bound it —
        // harmless under the current call order, and exactly the kind of
        // ordering dependency that breaks silently when someone reorders a
        // frame.
        m_DrawCountBuffer = SB::Create(sizeof(u32), ShaderBindingLayout::SSBO_VSM_DRAW_COMMANDS);
        const u32 oneDraw = 1u;
        m_DrawCountBuffer->SetData(&oneDraw, sizeof(u32));

        // The directional clip levels and the local layers share ONE table (and
        // therefore one allocator, one free list and one eviction policy) — the
        // sharing issue #703 asked for. The directional region keeps indices
        // [0, kTotalVirtualPages) so nothing about the clip-level addressing
        // moved.
        m_PageTable = SB::Create(VSM::kTotalPageTableEntries * sizeof(u32),
                                 ShaderBindingLayout::SSBO_VSM_PAGE_TABLE, kGPUWritten);
        m_MetaTable = SB::Create(physicalPageCount * sizeof(u32),
                                 ShaderBindingLayout::SSBO_VSM_META_TABLE, kGPUWritten);
        m_HPB = SB::Create(VSM::kTotalHPBEntries * sizeof(u32),
                           ShaderBindingLayout::SSBO_VSM_HPB, kGPUWritten);
        m_Requests = SB::Create((kRequestHeaderUints + 4 * VSM::kMaxRequests) * sizeof(u32),
                                ShaderBindingLayout::SSBO_VSM_REQUESTS, kGPUWritten);
        m_FreePages = SB::Create((kFreeListHeaderUints + 2 * physicalPageCount) * sizeof(u32),
                                 ShaderBindingLayout::SSBO_VSM_FREE_PAGES, kGPUWritten);
        m_Invalidations = SB::Create((kInvalidationHeaderVec4s + 2 * kMaxInvalidations) * sizeof(glm::vec4),
                                     ShaderBindingLayout::SSBO_VSM_INVALIDATIONS);
        m_CullInstances = SB::Create(VSM::kMaxCasters * sizeof(VSM::CullInstance),
                                     ShaderBindingLayout::SSBO_VSM_CULL_INSTANCES);
        m_DrawInstances = SB::Create(VSM::kMaxDrawInstances * sizeof(VSM::DrawInstance),
                                     ShaderBindingLayout::SSBO_VSM_DRAW_INSTANCES, kGPUWritten);
        m_DrawCommands = SB::Create(VSM::kMaxBatches * sizeof(VSM::DrawCommand),
                                    ShaderBindingLayout::SSBO_VSM_DRAW_COMMANDS, kGPUWritten);
        for (auto& stats : m_StatsBuffers)
            stats = SB::Create(sizeof(VSM::Statistics), ShaderBindingLayout::SSBO_VSM_STATS, kGPUWritten);

        // The one NEW binding issue #703 takes (SSBO 78). It could not ride an
        // existing buffer: the per-layer projections are 40 KB, which is past the
        // GL 4.6 minimum UBO size (16 KB), and every VSM SSBO already holds a
        // structurally different thing. DynamicCopy rather than the CPU default
        // because the header's raster-mip array is GPU-written (atomicMin from
        // the local HPB build) even though the rest of the buffer is uploaded.
        m_LocalLights = SB::Create(kLocalBufferBytes, ShaderBindingLayout::SSBO_VSM_LOCAL_LIGHTS, kGPUWritten);

        m_GlobalsUBO = UniformBuffer::Create(VSM::GlobalsUBO::GetSize(), ShaderBindingLayout::UBO_VIRTUAL_SHADOW);
        m_PassUBO = UniformBuffer::Create(VSM::PassUBO::GetSize(), ShaderBindingLayout::UBO_VIRTUAL_SHADOW_DRAW);

        ResetPageState();
    }

    void VirtualShadowMap::ResetPageState()
    {
        if (m_PageTable)
            m_PageTable->ClearData();
        if (m_MetaTable)
            m_MetaTable->ClearData();
        if (m_HPB)
            m_HPB->ClearData();
        if (m_Requests)
            m_Requests->ClearData();
        if (m_FreePages)
            m_FreePages->ClearData();
        if (m_LocalLights)
            m_LocalLights->ClearData();
        for (auto& stats : m_StatsBuffers)
        {
            if (stats)
                stats->ClearData();
        }
        m_Statistics = {};

        // The layer pool goes with the pages it addresses. Keeping the slot map
        // across a reset would hand a light back a layer whose page-table entries
        // have just been zeroed — the layer would read as cached and never be
        // redrawn, which is a permanently blank shadow for that light.
        m_LocalPool.Clear();
        m_LocalLayers.fill(VSM::LocalLight{});
        m_LocalHeads.clear();
        m_LocalLayerHighWater = 0;
        m_LocalLightsStarved = 0;
    }

    void VirtualShadowMap::DestroyResources()
    {
        if (m_RasterFramebuffer.IsValid())
        {
            RenderCommand::DeleteFramebuffer(m_RasterFramebuffer);
            m_RasterFramebuffer = {};
        }
        if (m_RasterScope.IsValid())
        {
            RenderCommand::DeleteTexture(m_RasterScope);
            m_RasterScope = {};
        }
        if (m_PhysicalPool.IsValid())
        {
            RenderCommand::DeleteTexture(m_PhysicalPool);
            m_PhysicalPool = {};
        }

        m_PageTable.Reset();
        m_MetaTable.Reset();
        m_HPB.Reset();
        m_Requests.Reset();
        m_FreePages.Reset();
        m_Invalidations.Reset();
        m_CullInstances.Reset();
        m_DrawInstances.Reset();
        m_DrawCommands.Reset();
        m_DrawCountBuffer.Reset();
        m_LocalLights.Reset();
        for (auto& stats : m_StatsBuffers)
            stats.Reset();

        m_GlobalsUBO.Reset();
        m_PassUBO.Reset();

        m_FreeWrappedShader.Reset();
        m_MarkShader.Reset();
        m_InvalidateShader.Reset();
        m_FindFreeShader.Reset();
        m_AllocateShader.Reset();
        m_ClearPagesShader.Reset();
        m_BuildHPBShader.Reset();
        m_CullShader.Reset();
        m_CullLocalShader.Reset();
        m_EndFrameShader.Reset();
        m_DepthShader.Reset();
        m_DepthSkinnedShader.Reset();
        m_DepthLocalShader.Reset();
        m_DepthLocalSkinnedShader.Reset();
    }

    void VirtualShadowMap::Shutdown()
    {
        OLO_PROFILE_FUNCTION();
        DestroyResources();
        m_Initialized = false;
        m_FullInvalidate = true;
    }

    void VirtualShadowMap::SetSettings(const VirtualShadowMapSettings& settings)
    {
        OLO_PROFILE_FUNCTION();

        const bool wasActive = IsActive();
        const bool needsRecreate = settings.Enabled != m_Settings.Enabled ||
                                   VirtualShadowMap::SanitizeResolution(settings.PhysicalResolution) != m_PhysicalResolution;

        // Anything that changes the shape of light space invalidates every cached
        // page: the stored depths and the page-to-world mapping were produced
        // under the old numbers, and a partially-refreshed table would blend the
        // two silently.
        // Epsilon compare, not ==/!= (cpp-coding-quality.md par.2): these arrive
        // from UI sliders and (eventually) serialized settings, and a last-bit
        // round-trip difference must not torch every cached page.
        const auto differs = [](f32 a, f32 b)
        { return std::fabs(a - b) > 1.0e-6f; };
        const bool invalidates = differs(settings.Clip0HalfExtent, m_Settings.Clip0HalfExtent) ||
                                 differs(settings.DepthRange, m_Settings.DepthRange) ||
                                 differs(settings.ClipSelectionBias, m_Settings.ClipSelectionBias) ||
                                 // LocalDetailBias is in the SAME class as
                                 // ClipSelectionBias, not a quality dial: it is an
                                 // input to the mip heuristic BOTH the marker and
                                 // the sampler run, so a cached page produced under
                                 // the old value is addressed by a mip the new one
                                 // no longer picks (page-cache doc §1).
                                 differs(settings.LocalDetailBias, m_Settings.LocalDetailBias) ||
                                 settings.LocalLights != m_Settings.LocalLights;

        m_Settings = settings;

        if (needsRecreate)
        {
            if (wasActive || m_Initialized)
                Shutdown();
            Init(m_Settings);
            return;
        }

        if (invalidates)
            m_FullInvalidate = true;
    }

    u64 VirtualShadowMap::GetVRAMBytes() const
    {
        if (!m_Initialized)
            return 0;

        const u64 physicalPageCount = static_cast<u64>(m_PhysicalPageTableResolution) * m_PhysicalPageTableResolution;
        u64 bytes = 0;
        bytes += static_cast<u64>(m_PhysicalResolution) * m_PhysicalResolution * sizeof(u32); // R32UI pool
        bytes += static_cast<u64>(VSM::kVirtualResolution) * VSM::kVirtualResolution;         // R8UI raster scope
        bytes += static_cast<u64>(VSM::kTotalPageTableEntries) * sizeof(u32);                 // page table
        bytes += physicalPageCount * sizeof(u32);                                             // meta table
        bytes += static_cast<u64>(VSM::kTotalHPBEntries) * sizeof(u32);                       // HPB
        bytes += static_cast<u64>(kRequestHeaderUints + 4 * VSM::kMaxRequests) * sizeof(u32);
        bytes += (kFreeListHeaderUints + 2 * physicalPageCount) * sizeof(u32);
        bytes += static_cast<u64>(VSM::kMaxCasters) * sizeof(VSM::CullInstance);
        bytes += static_cast<u64>(VSM::kMaxDrawInstances) * sizeof(VSM::DrawInstance);
        bytes += kLocalBufferBytes; // local-light layer buffer (issue #703)
        return bytes;
    }

    // -------------------------------------------------------------------------
    // Step 0 — clip projections
    // -------------------------------------------------------------------------

    u32 VirtualShadowMap::SanitizeResolution(u32 requested)
    {
        // A pool that is not a whole number of pages leaves a partial page at the
        // edge, and the allocator has no way to express "half a page" -- it would
        // hand out a physical page whose texels run off the end of the texture.
        constexpr u32 kMin = 1024;
        constexpr u32 kMax = 8192;
        const u32 clamped = std::clamp(requested, kMin, kMax);
        return (clamped / VSM::kPageSize) * VSM::kPageSize;
    }

    i32 VirtualShadowMap::SelectClipLevel(f32 distanceToCamera, f32 clip0HalfExtent, f32 bias)
    {
        // GLSL twin: vsmClipLevelForDistance in include/VirtualShadowCommon.glsl.
        // Level L covers clip0HalfExtent * 2^L, so the level whose texels are about
        // a screen pixel at distance d is ceil(log2(d / clip0HalfExtent)).
        const f32 scaled = std::max(distanceToCamera * std::max(bias, 1e-4f), 1e-4f) /
                           std::max(clip0HalfExtent, 1e-4f);
        const auto level = static_cast<i32>(std::ceil(std::log2(std::max(scaled, 1e-4f))));
        return std::clamp(level, 0, static_cast<i32>(VSM::kClipLevels) - 1);
    }

    bool VirtualShadowMap::WorldPointToWrappedPage(const VSM::ClipProjection& clip,
                                                   const glm::vec3& worldPosRelative,
                                                   glm::ivec2& outWrappedPage)
    {
        const glm::vec4 clipPos = clip.ViewProjection * glm::vec4(worldPosRelative, 1.0f);
        const glm::vec3 ndc = glm::vec3(clipPos) / clipPos.w;
        const glm::vec2 uv = glm::vec2(ndc) * 0.5f + 0.5f;
        if (uv.x < 0.0f || uv.x >= 1.0f || uv.y < 0.0f || uv.y >= 1.0f || ndc.z < -1.0f || ndc.z > 1.0f)
            return false;

        constexpr i32 kRes = static_cast<i32>(VSM::kPageTableResolution);
        const glm::ivec2 virtualPage = glm::clamp(glm::ivec2(glm::floor(uv * static_cast<f32>(kRes))),
                                                  glm::ivec2(0), glm::ivec2(kRes - 1));
        outWrappedPage = (virtualPage + clip.PageOffset) & glm::ivec2(kRes - 1);
        return true;
    }

    void VirtualShadowMap::BuildClipProjections(const glm::vec3& lightDirection,
                                                const glm::vec3& cameraPositionRelative,
                                                const VirtualShadowMapSettings& settings,
                                                const std::array<glm::ivec2, VSM::kClipLevels>& prevOrigins,
                                                bool fullInvalidate,
                                                std::array<VSM::ClipProjection, VSM::kClipLevels>& outClips,
                                                std::array<glm::ivec2, VSM::kClipLevels>& outOrigins)
    {
        const glm::vec3 forward = glm::normalize(lightDirection);

        // A basis derived deterministically from the light direction ALONE. If it
        // depended on anything per-frame, every cached page would shear under it.
        const glm::vec3 upHint = (std::abs(forward.y) > 0.99f) ? glm::vec3(1.0f, 0.0f, 0.0f)
                                                               : glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::mat4 lightView = glm::lookAt(glm::vec3(0.0f), forward, upHint);
        const glm::vec3 lightRight(lightView[0][0], lightView[1][0], lightView[2][0]);
        const glm::vec3 lightUp(lightView[0][1], lightView[1][1], lightView[2][1]);

        // Light-space XY of the camera. Z is deliberately unused: the depth range
        // is anchored to the render ORIGIN, not to the camera. That is what keeps a
        // cached page's stored depth meaningful after the camera moves along the
        // light axis, and what makes every clip level agree on a world point's
        // depth -- which is what stops a seam forming at a clip-level boundary.
        const f32 cameraLightX = glm::dot(lightRight, cameraPositionRelative);
        const f32 cameraLightY = glm::dot(lightUp, cameraPositionRelative);

        const f32 depthRange = std::max(settings.DepthRange, 1.0f);
        const f32 clip0HalfExtent = std::max(settings.Clip0HalfExtent, 0.01f);
        constexpr i32 kRes = static_cast<i32>(VSM::kPageTableResolution);

        for (u32 level = 0; level < VSM::kClipLevels; ++level)
        {
            const f32 halfExtent = clip0HalfExtent * std::exp2(static_cast<f32>(level));
            const f32 extent = halfExtent * 2.0f;
            // Exactly kPageTableResolution pages span the level, so snapping the
            // frustum's lower-left corner to a page multiple snaps every page
            // boundary with it -- the invariant the whole cache rests on.
            const f32 pageWorldSize = extent / static_cast<f32>(kRes);

            const auto originX = static_cast<i32>(std::floor((cameraLightX - halfExtent) / pageWorldSize));
            const auto originY = static_cast<i32>(std::floor((cameraLightY - halfExtent) / pageWorldSize));

            const f32 minX = static_cast<f32>(originX) * pageWorldSize;
            const f32 minY = static_cast<f32>(originY) * pageWorldSize;

            auto& clip = outClips[level];
            clip.ViewProjection =
                glm::ortho(minX, minX + extent, minY, minY + extent, -depthRange, depthRange) * lightView;
            clip.HalfExtent = halfExtent;
            clip.TexelWorldSize = extent / static_cast<f32>(VSM::kVirtualResolution);

            const glm::ivec2 origin(originX, originY);
            const glm::ivec2 previous = fullInvalidate ? origin : prevOrigins[level];

            clip.PageOffset = glm::ivec2(PositiveMod(originX, kRes), PositiveMod(originY, kRes));
            clip.PrevPageOffset = glm::ivec2(PositiveMod(previous.x, kRes), PositiveMod(previous.y, kRes));
            // Saturated: a move of a whole table or more frees everything anyway,
            // and clamping keeps the shader's per-axis comparison in i32 range.
            clip.PageDelta = glm::clamp(origin - previous, glm::ivec2(-kRes), glm::ivec2(kRes));

            outOrigins[level] = origin;
        }
    }

    void VirtualShadowMap::BeginFrame(const glm::vec3& lightDirection, const glm::vec3& cameraPositionRelative,
                                      const glm::vec3& renderOrigin, bool directionalEnabled)
    {
        OLO_PROFILE_FUNCTION();

        if (!IsActive())
            return;

        // Normalizing a zero vector yields NaN, and a NaN light direction poisons
        // every clip projection and therefore every page the marker requests.
        // A scene with local lights but NO directional one reaches here now
        // (issue #703 relaxed the caller's gate so the layers still get built),
        // so the degenerate input is a real path rather than a defensive branch.
        const f32 directionLength = glm::length(lightDirection);
        const glm::vec3 forward = (directionLength > 1.0e-6f) ? (lightDirection / directionLength)
                                                              : glm::vec3(0.0f, -1.0f, 0.0f);

        // A change to either re-anchors light space, so every cached page is stale.
        constexpr f32 kDirectionEpsilon = 0.99999f;
        if (glm::dot(forward, m_PrevLightDirection) < kDirectionEpsilon ||
            glm::any(glm::notEqual(renderOrigin, m_PrevRenderOrigin, 0.0f)))
        {
            m_FullInvalidate = true;
        }

        BuildClipProjections(forward, cameraPositionRelative, m_Settings, m_PrevOrigins, m_FullInvalidate,
                             m_Globals.Clips, m_CurrOrigins);

        // The rasterizer flavour of each clip projection (ADR 0011 (59)). Derived
        // HERE and not inside BuildClipProjections deliberately: that function is
        // the one the contract tests drive, and reaching into the backend seam
        // from it would make its output depend on which RHI happens to be up.
        for (auto& clip : m_Globals.Clips)
            clip.ViewProjectionRaster = RHI::AdjustProjectionForBackend(clip.ViewProjection);

        const f32 depthRange = std::max(m_Settings.DepthRange, 1.0f);
        m_Globals.LightDirection = glm::vec4(forward, 0.0f);
        m_Globals.CameraPosition = glm::vec4(cameraPositionRelative, 0.0f);
        m_Globals.Params0 = glm::vec4(std::max(m_Settings.Clip0HalfExtent, 0.01f),
                                      std::max(m_Settings.ClipSelectionBias, 0.01f),
                                      // Metres -> the [0,1] depth the ortho range maps to, so the
                                      // authored value stays physically meaningful when the range
                                      // is retuned.
                                      m_Settings.DepthBiasMeters / (2.0f * depthRange),
                                      m_Settings.NormalBias);
        // Softness / max distance are owned by ShadowSettings and arrive through
        // SetSamplingParams; preserve whatever it last wrote.
        m_Globals.Params1 = glm::vec4(m_Globals.Params1.x, m_Globals.Params1.y,
                                      static_cast<f32>(m_PhysicalResolution),
                                      static_cast<f32>(m_PhysicalPageTableResolution));
        m_Globals.Params2 = glm::ivec4(1, m_Settings.DebugMode, m_FullInvalidate ? 1 : 0,
                                       m_Globals.Params2.w + 1);
        // Local-light counts are published by EndLocalLights (which runs earlier
        // in the frame, during shadow setup); only the two flags and the biases
        // belong to this frame's globals rebuild.
        m_Globals.Params4 = glm::ivec4(AreLocalLightsActive() ? 1 : 0, m_Globals.Params4.y,
                                       m_Globals.Params4.z, directionalEnabled ? 1 : 0);
        m_Globals.Params5 = glm::vec4(std::max(m_Settings.LocalDetailBias, 0.01f),
                                      std::max(m_Settings.LocalDepthBiasMeters, 0.0f), 0.0f, 0.0f);

        m_PrevLightDirection = forward;
        m_PrevRenderOrigin = renderOrigin;
    }

    void VirtualShadowMap::SetSamplingParams(f32 softness, f32 maxShadowDistance)
    {
        m_Globals.Params1.x = softness;
        m_Globals.Params1.y = maxShadowDistance;
    }

    // -------------------------------------------------------------------------
    // Local lights — the layer pool (issue #703)
    // -------------------------------------------------------------------------

    i32 VirtualShadowMap::SelectLocalMip(f32 distanceToCamera, f32 distanceToLight, f32 bias)
    {
        // GLSL twin: vsmLocalMipForDistances in include/VirtualShadowCommon.glsl.
        // The derivation is written out there; the short version is that it makes
        // a local face's texel the same world size as a directional texel at the
        // same distance from the camera, so the two techniques' sharpness matches
        // and neither needs to know the screen resolution.
        // The bias MULTIPLIES the numerator, matching SelectClipLevel's direction:
        // > 1 is coarser and cheaper. See the GLSL twin for why that is worth
        // spelling out.
        const f32 ratio = (static_cast<f32>(VSM::kLocalVirtualResolution) *
                           std::max(distanceToCamera, 1.0e-4f) * std::max(bias, 1.0e-4f)) /
                          (static_cast<f32>(VSM::kVirtualResolution) * std::max(distanceToLight, 1.0e-4f));
        const auto mip = static_cast<i32>(std::ceil(std::log2(std::max(ratio, 1.0e-4f))));
        return std::clamp(mip, 0, static_cast<i32>(VSM::kLocalMipCount) - 1);
    }

    u32 VirtualShadowMap::SelectCubeFace(const glm::vec3& direction)
    {
        // Mirrors vsmCubeFace / PBRCommon's atlasCubeFace. All three must agree:
        // the marker picks the face to request, the sampler picks the face to
        // read, and a disagreement puts the sample on a face nobody backed —
        // which reads as an unshadowed quadrant, not as an error.
        const glm::vec3 a = glm::abs(direction);
        if (a.x >= a.y && a.x >= a.z)
            return (direction.x > 0.0f) ? 0u : 1u;
        if (a.y >= a.z)
            return (direction.y > 0.0f) ? 2u : 3u;
        return (direction.z > 0.0f) ? 4u : 5u;
    }

    u32 VirtualShadowMap::LocalMipOffset(u32 mip)
    {
        // GLSL twin: vsmLocalMipOffset. Same loop-free table treatment as the
        // directional HPB offsets, and for the same reason.
        constexpr std::array<u32, VSM::kLocalMipCount> kOffsets{ 0u, 1024u, 1280u, 1344u, 1360u, 1364u };
        return kOffsets[std::min(mip, VSM::kLocalMipCount - 1u)];
    }

    u32 VirtualShadowMap::LocalPageIndex(u32 layer, u32 mip, glm::uvec2 page)
    {
        const u32 clampedMip = std::min(mip, VSM::kLocalMipCount - 1u);
        const u32 mipRes = VSM::kLocalPageTableResolution >> clampedMip;
        return VSM::kTotalVirtualPages + layer * VSM::kLocalPagesPerLayer + LocalMipOffset(clampedMip) +
               page.y * mipRes + page.x;
    }

    void VirtualShadowMap::BuildLocalLightProjections(const LocalLightDesc& desc,
                                                      std::array<glm::mat4, 6>& outViewProjections,
                                                      f32& outNear, f32& outFar)
    {
        const f32 range = std::max(desc.Range, 0.01f);
        const f32 nearPlane = LocalNearPlane(range);
        outNear = nearPlane;
        outFar = range;

        if (desc.IsPoint)
        {
            // 90° cube faces, same axes and same up vectors as
            // ShadowMap::BuildPointLightFaceMatrices — so a light's shadow lands
            // in the same place whichever technique draws it
            // (light-path-photometric-parity.md). Built here in RENDER-RELATIVE
            // space rather than shifted afterwards: the VSM works entirely
            // relative, and a shift applied to a perspective view-projection is
            // not the same operation as building it from a shifted eye.
            const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, nearPlane, range);
            for (u32 face = 0; face < 6; ++face)
            {
                outViewProjections[face] =
                    proj * glm::lookAt(desc.PositionRelative, desc.PositionRelative + kCubeFaceForward[face],
                                       kCubeFaceUp[face]);
            }
            return;
        }

        glm::vec3 direction = desc.Direction;
        const f32 length = glm::length(direction);
        direction = (length > 1.0e-6f) ? (direction / length) : glm::vec3(0.0f, 0.0f, -1.0f);

        glm::vec3 up(0.0f, 1.0f, 0.0f);
        if (std::abs(glm::dot(direction, up)) > 0.99f)
            up = glm::vec3(1.0f, 0.0f, 0.0f);

        // Clamped, unlike the atlas builder: a cutoff at or past 90° asks for a
        // >=180° field of view, and glm::perspective answers that with a matrix
        // whose x/y scale is zero or negative. The atlas path produces a garbage
        // tile for the same input; here it would poison a layer's cached pages,
        // so the clamp is worth the one-degree difference at the extreme.
        const f32 fov = glm::radians(std::clamp(desc.OuterCutoffDegrees, 1.0f, 89.0f) * 2.0f);
        outViewProjections[0] = glm::perspective(fov, 1.0f, nearPlane, range) *
                                glm::lookAt(desc.PositionRelative, desc.PositionRelative + direction, up);
        for (u32 face = 1; face < 6; ++face)
            outViewProjections[face] = glm::mat4(1.0f);
    }

    void VirtualShadowMap::LocalLayerPool::BeginFrame()
    {
        ++FrameCounter;
        // Cleared HERE, not at the end of the frame: registration is what sets
        // these, and the GPU consumer (VSM_FreeWrappedPages, via UpdatePages)
        // runs between registration and the next BeginFrame.
        Invalidate.fill(0);
        for (auto& slot : Slots)
            slot.InUse = false;
    }

    void VirtualShadowMap::LocalLayerPool::InvalidateSlot(u32 slotIndex)
    {
        if (slotIndex >= Slots.size())
            return;
        const Slot& slot = Slots[slotIndex];
        for (u32 i = 0; i < slot.Count && (slot.Base + i) < VSM::kMaxLocalLayers; ++i)
            Invalidate[slot.Base + i] = 1u;
    }

    void VirtualShadowMap::LocalLayerPool::Release(u32 slotIndex)
    {
        if (slotIndex >= Slots.size())
            return;

        // Flag the pages BEFORE the layers stop pointing at the slot — the flush
        // is what stops the next owner inheriting a table full of allocated,
        // clean, WRONG pages, which reads as a light whose shadow never updates.
        InvalidateSlot(slotIndex);

        Slot& slot = Slots[slotIndex];
        for (u32 i = 0; i < slot.Count && (slot.Base + i) < VSM::kMaxLocalLayers; ++i)
            Owner[slot.Base + i] = 0;
        if (slot.LightId != 0)
            ByLight.erase(slot.LightId);

        // Not erased from the vector — see the declaration.
        slot = Slot{};
    }

    u32 VirtualShadowMap::LocalLayerPool::Acquire(const LocalLightDesc& desc, bool& outMoved)
    {
        const u32 count = desc.IsPoint ? 6u : 1u;

        u32 slotIndex = kNoLocalSlot;
        if (desc.LightId != 0)
        {
            if (const auto found = ByLight.find(desc.LightId); found != ByLight.end())
            {
                // A light that changed CLASS (spot <-> point) needs a different
                // number of layers, so its run is released and re-taken rather
                // than resized in place — a resized run would overlap whatever
                // sits after it.
                if (Slots[found->second].Count == count)
                    slotIndex = found->second;
                else
                    Release(found->second);
            }
        }

        if (slotIndex == kNoLocalSlot)
        {
            slotIndex = AcquireFreshSlot(desc.LightId, count);
            if (slotIndex == kNoLocalSlot)
            {
                outMoved = false;
                return kNoLocalSlot;
            }
        }

        Slot& slot = Slots[slotIndex];

        // A pose change re-anchors the light's frusta, so every cached page under
        // this slot now means a different world position — the local twin of the
        // directional path's light-direction / render-origin check. Epsilon
        // compares, not ==: these arrive from a transform recomputed every frame,
        // and a last-bit difference must not flush a light's whole cache
        // (cpp-coding-quality.md §2).
        constexpr f32 kPoseEpsilon = 1.0e-4f;
        outMoved = glm::any(glm::greaterThan(glm::abs(slot.Position - desc.PositionRelative),
                                             glm::vec3(kPoseEpsilon))) ||
                   glm::any(glm::greaterThan(glm::abs(slot.Direction - desc.Direction),
                                             glm::vec3(kPoseEpsilon))) ||
                   std::fabs(slot.Range - desc.Range) > kPoseEpsilon ||
                   std::fabs(slot.Cutoff - desc.OuterCutoffDegrees) > kPoseEpsilon ||
                   slot.IsPoint != desc.IsPoint;

        if (outMoved)
        {
            InvalidateSlot(slotIndex);
            slot.Position = desc.PositionRelative;
            slot.Direction = desc.Direction;
            slot.Range = desc.Range;
            slot.Cutoff = desc.OuterCutoffDegrees;
            slot.IsPoint = desc.IsPoint;
        }

        slot.InUse = true;
        slot.LastUsedFrame = FrameCounter;
        return slotIndex;
    }

    u32 VirtualShadowMap::LocalLayerPool::LayerHighWater() const
    {
        u32 highWater = 0;
        for (const auto& slot : Slots)
        {
            if (slot.Count != 0)
                highWater = std::max(highWater, slot.Base + slot.Count);
        }
        return highWater;
    }

    void VirtualShadowMap::LocalLayerPool::Clear()
    {
        Slots.clear();
        ByLight.clear();
        Owner.fill(0);
        Invalidate.fill(0);
    }

    u32 VirtualShadowMap::LocalLayerPool::AcquireFreshSlot(u64 lightId, u32 count)
    {
        const auto findRun = [this, count]() -> u32
        {
            u32 run = 0;
            for (u32 layer = 0; layer < VSM::kMaxLocalLayers; ++layer)
            {
                run = (Owner[layer] == 0) ? (run + 1) : 0u;
                if (run >= count)
                    return layer + 1 - count;
            }
            return kNoLocalSlot;
        };

        u32 base = findRun();
        if (base == kNoLocalSlot)
        {
            // Evict the least-recently-used slot that is NOT serving a light this
            // frame, then retry once. Only one: evicting a batch would be a
            // bigger hammer than the pressure justifies, and a caller that still
            // cannot fit gets a reportable failure rather than a silently
            // reshuffled pool.
            u32 victim = kNoLocalSlot;
            u64 oldest = ~0ull;
            for (u32 i = 0; i < Slots.size(); ++i)
            {
                const Slot& slot = Slots[i];
                if (slot.Count == 0 || slot.InUse)
                    continue;
                if (slot.LastUsedFrame < oldest)
                {
                    oldest = slot.LastUsedFrame;
                    victim = i;
                }
            }
            if (victim == kNoLocalSlot)
                return kNoLocalSlot;
            Release(victim);
            base = findRun();
            if (base == kNoLocalSlot)
                return kNoLocalSlot; // the freed run was too short and not adjacent
        }

        u32 slotIndex = kNoLocalSlot;
        for (u32 i = 0; i < Slots.size(); ++i)
        {
            if (Slots[i].Count == 0)
            {
                slotIndex = i;
                break;
            }
        }
        if (slotIndex == kNoLocalSlot)
        {
            slotIndex = static_cast<u32>(Slots.size());
            Slots.emplace_back();
        }

        Slot& slot = Slots[slotIndex];
        slot = Slot{};
        slot.LightId = lightId;
        slot.Base = base;
        slot.Count = count;
        // Deliberately NOT the caller's pose: Acquire compares against this to
        // decide whether the light moved, and a FRESH slot must compare as moved
        // so its projections get written and its recycled pages get flushed.
        slot.Range = -1.0f;

        for (u32 i = 0; i < count; ++i)
            Owner[base + i] = static_cast<u16>(slotIndex + 1);
        if (lightId != 0)
            ByLight[lightId] = slotIndex;

        // A recycled run may still hold the previous owner's allocated pages.
        for (u32 i = 0; i < count; ++i)
            Invalidate[base + i] = 1u;

        return slotIndex;
    }

    void VirtualShadowMap::BeginLocalLights()
    {
        if (!IsActive())
            return;

        m_LocalPool.BeginFrame();
        m_LocalHeads.clear();
        m_LocalLightsStarved = 0;
    }

    i32 VirtualShadowMap::RegisterLocalLight(const LocalLightDesc& desc)
    {
        if (!AreLocalLightsActive() || desc.Range <= 0.0f)
            return -1;

        const u32 count = desc.IsPoint ? 6u : 1u;

        bool moved = false;
        const u32 slotIndex = m_LocalPool.Acquire(desc, moved);
        if (slotIndex == kNoLocalSlot)
        {
            ++m_LocalLightsStarved;
            if (!m_LoggedLocalPoolExhausted)
            {
                OLO_CORE_WARN("VirtualShadowMap: local layer pool exhausted ({} layers) — a light this frame "
                              "gets no shadow. Raise VSM::kMaxLocalLayers or reduce shadow-casting local lights",
                              VSM::kMaxLocalLayers);
                m_LoggedLocalPoolExhausted = true;
            }
            return -1;
        }

        const LocalLayerPool::Slot& slot = m_LocalPool.Slots[slotIndex];

        if (moved)
        {
            std::array<glm::mat4, 6> viewProjections{};
            f32 nearPlane = 0.0f;
            f32 farPlane = 0.0f;
            BuildLocalLightProjections(desc, viewProjections, nearPlane, farPlane);

            for (u32 i = 0; i < count; ++i)
            {
                VSM::LocalLight& layer = m_LocalLayers[slot.Base + i];
                layer.ViewProjection = viewProjections[i];
                // Derived here and not inside BuildLocalLightProjections for the
                // same reason the clip projections are: that function is what the
                // contract tests drive, and reaching into the backend seam from it
                // would make its output depend on which RHI happens to be up.
                layer.ViewProjectionRaster = RHI::AdjustProjectionForBackend(viewProjections[i]);
                layer.PositionRange = glm::vec4(desc.PositionRelative, desc.Range);
                layer.Params = glm::vec4(nearPlane, farPlane, static_cast<f32>(i),
                                         desc.IsPoint ? VSM::kLocalKindPoint : VSM::kLocalKindSpot);
            }
        }

        m_LocalHeads.push_back((slot.Base << 1) | (desc.IsPoint ? 1u : 0u));
        return static_cast<i32>(slot.Base);
    }

    void VirtualShadowMap::EndLocalLights()
    {
        if (!IsActive())
            return;

        // A light that stopped casting keeps its slot (so it keeps its cached
        // pages if it starts again) but must stop being marked, culled and
        // rasterized. Zeroing the layer's KIND is what takes it out of all three:
        // the marker walks b_LocalLightHead, which no longer names it, and the
        // cull skips a layer whose kind is unused.
        for (const auto& slot : m_LocalPool.Slots)
        {
            if (slot.Count == 0)
                continue;
            const f32 kind = !slot.InUse    ? VSM::kLocalKindUnused
                             : slot.IsPoint ? VSM::kLocalKindPoint
                                            : VSM::kLocalKindSpot;
            for (u32 i = 0; i < slot.Count && (slot.Base + i) < VSM::kMaxLocalLayers; ++i)
                m_LocalLayers[slot.Base + i].Params.w = kind;
        }

        m_LocalLayerHighWater = m_LocalPool.LayerHighWater();
        m_Globals.Params4.y = static_cast<i32>(m_LocalHeads.size());
        m_Globals.Params4.z = static_cast<i32>(m_LocalLayerHighWater);

        UploadLocalLights();
    }

    void VirtualShadowMap::UploadLocalLights()
    {
        if (!m_LocalLights)
            return;

        // The raster mip is GPU-written (atomicMin from the local HPB build) and
        // must start at "coarser than any real mip" for the min to mean anything.
        // Resetting it here — in the upload that runs during shadow SETUP — is
        // what puts it before UpdatePages' HPB build in the frame, which is the
        // only ordering that works: reset it after and every layer rasterizes at
        // mip 0, reset it never and a layer's raster mip decays to whatever the
        // first frame chose.
        std::array<u32, VSM::kMaxLocalLayers> rasterMip{};
        rasterMip.fill(VSM::kLocalMipCount);
        m_LocalLights->SetData(rasterMip.data(), static_cast<u32>(rasterMip.size() * sizeof(u32)),
                               kLocalRasterMipOffset);

        std::array<u32, VSM::kMaxLocalLayers> heads{};
        const auto headCount = static_cast<u32>(std::min<sizet>(m_LocalHeads.size(), VSM::kMaxLocalLayers));
        std::copy_n(m_LocalHeads.begin(), headCount, heads.begin());
        m_LocalLights->SetData(heads.data(), static_cast<u32>(heads.size() * sizeof(u32)), kLocalHeadOffset);

        m_LocalLights->SetData(m_LocalPool.Invalidate.data(),
                               static_cast<u32>(m_LocalPool.Invalidate.size() * sizeof(u32)),
                               kLocalInvalidateOffset);

        m_LocalLights->SetData(m_LocalLayers.data(),
                               static_cast<u32>(m_LocalLayers.size() * sizeof(VSM::LocalLight)),
                               kLocalLayersOffset);
    }

    void VirtualShadowMap::AddDynamicInvalidation(const glm::vec3& boundsMin, const glm::vec3& boundsMax)
    {
        if (!IsActive() || m_PendingInvalidations.size() >= 2 * kMaxInvalidations)
            return;
        m_PendingInvalidations.emplace_back(boundsMin, 0.0f);
        m_PendingInvalidations.emplace_back(boundsMax, 0.0f);
    }

    void VirtualShadowMap::SubmitDynamicInvalidations(const std::vector<ShadowMeshCaster>& meshCasters,
                                                      const std::vector<ShadowSkinnedCaster>& skinnedCasters,
                                                      const glm::vec3& renderOrigin)
    {
        OLO_PROFILE_FUNCTION();

        if (!IsActive())
            return;

        const auto boundsOf = [&renderOrigin](const BoundingBox& worldBounds, glm::vec3& outMin, glm::vec3& outMax)
        {
            if (worldBounds.Min.x >= std::numeric_limits<f32>::max())
                return false;
            outMin = worldBounds.Min - renderOrigin;
            outMax = worldBounds.Max - renderOrigin;
            return true;
        };

        // Skinned casters animate by definition — always invalidate.
        for (const auto& caster : skinnedCasters)
        {
            glm::vec3 boundsMin{};
            glm::vec3 boundsMax{};
            if (boundsOf(caster.WorldBounds, boundsMin, boundsMax))
                AddDynamicInvalidation(boundsMin, boundsMax);
        }

        // Static casters: a pose that differs from the same index last frame is a
        // mover. The SWEPT bounds (previous UNION current) are what gets
        // invalidated — invalidating only the new position leaves the object's old
        // silhouette baked into a page nobody will redraw, which is the classic
        // "the shadow stayed behind" artefact.
        for (sizet i = 0; i < meshCasters.size(); ++i)
        {
            const auto& caster = meshCasters[i];
            glm::vec3 boundsMin{};
            glm::vec3 boundsMax{};
            const bool hasBounds = boundsOf(caster.WorldBounds, boundsMin, boundsMax);

            const bool isNew = i >= m_PrevCasterPoses.size();
            const bool moved = isNew || std::memcmp(&m_PrevCasterPoses[i].Transform, &caster.transform,
                                                    sizeof(glm::mat4)) != 0;
            if (moved && hasBounds)
            {
                glm::vec3 sweptMin = boundsMin;
                glm::vec3 sweptMax = boundsMax;
                if (!isNew && m_PrevCasterPoses[i].HasBounds)
                {
                    sweptMin = glm::min(sweptMin, m_PrevCasterPoses[i].BoundsMin);
                    sweptMax = glm::max(sweptMax, m_PrevCasterPoses[i].BoundsMax);
                }
                AddDynamicInvalidation(sweptMin, sweptMax);
            }
        }

        m_PrevCasterPoses.resize(meshCasters.size());
        for (sizet i = 0; i < meshCasters.size(); ++i)
        {
            auto& pose = m_PrevCasterPoses[i];
            pose.Transform = meshCasters[i].transform;
            pose.HasBounds = boundsOf(meshCasters[i].WorldBounds, pose.BoundsMin, pose.BoundsMax);
        }
    }

    // -------------------------------------------------------------------------
    // Steps 1-6 — page management
    // -------------------------------------------------------------------------

    void VirtualShadowMap::BindWorkingSet()
    {
        m_GlobalsUBO->SetData(&m_Globals, VSM::GlobalsUBO::GetSize());
        m_GlobalsUBO->Bind();

        m_PageTable->Bind();
        m_MetaTable->Bind();
        m_HPB->Bind();
        m_Requests->Bind();
        m_FreePages->Bind();
        m_Invalidations->Bind();
        m_CullInstances->Bind();
        m_DrawInstances->Bind();
        m_DrawCommands->Bind();
        m_LocalLights->Bind();
        m_StatsBuffers[m_StatsWriteIndex]->Bind();
    }

    void VirtualShadowMap::BindPhysicalPoolImage() const
    {
        // Image unit 0, through the heap seam rather than RenderCommand directly
        // (issue #691 Phase 3 — the RHI boundary ratchet counts raw facade bind
        // sites, and this is the sanctioned spelling).
        //
        // MUST BE CALLED WITH THE CONSUMING SHADER ALREADY BOUND.
        // BindImageOrOffset forks on Shader::IsBoundProgramBindless() — the
        // program currently in flight — and every VSM shader declares the pool
        // slot-based (`layout(r32ui, binding = 0) uniform coherent uimage2D`). So
        // the fork must be allowed to see a VSM program, take the fallback, and
        // issue a real bind. Called with some other program bound it would ask
        // about that one instead, and a bindless answer would stage an offset and
        // bind NOTHING — every imageAtomicMin in the pass silently discarded, with
        // the pool left full of the far sentinel and the frame simply unshadowed.
        HeapBinding::BindImageOrOffset(0, m_PhysicalPool, 0, false, 0, RHI::Access::StorageReadWrite,
                                       RHI::Format::R32UInt, RHI::HeapSlotLifetime::Persistent);
    }

    void VirtualShadowMap::DispatchKernel(const Ref<ComputeShader>& shader, u32 threadCount, u32 groupSize) const
    {
        if (!shader || threadCount == 0)
            return;
        shader->Bind();
        RenderCommand::DispatchCompute(DivideRoundUp(threadCount, groupSize), 1, 1);
        // Every VSM kernel feeds the next one through SSBOs, and several also
        // touch the physical pool image, so the barrier is the same everywhere.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::ShaderImageAccess);
    }

    void VirtualShadowMap::UpdatePages()
    {
        OLO_PROFILE_FUNCTION();

        if (!IsActive())
            return;

        // The buffer being READ this frame is the one written last frame, so the
        // readback never waits on work still in flight.
        ReadbackStatistics();
        m_StatsWriteIndex ^= 1u;
        m_StatsBuffers[m_StatsWriteIndex]->ClearData();

        // Dynamic-caster bounds gathered since the last shadow pass.
        const auto invalidationCount =
            static_cast<u32>(std::min<sizet>(m_PendingInvalidations.size() / 2, kMaxInvalidations));
        {
            const glm::uvec4 header(invalidationCount, 0u, 0u, 0u);
            m_Invalidations->SetData(&header, sizeof(header), 0);
            if (invalidationCount > 0)
            {
                m_Invalidations->SetData(m_PendingInvalidations.data(),
                                         invalidationCount * 2 * static_cast<u32>(sizeof(glm::vec4)),
                                         static_cast<u32>(sizeof(glm::vec4)));
            }
        }
        m_PendingInvalidations.clear();

        BindWorkingSet();

        // 1. Release the slots the clip frusta scrolled onto, plus every layer a
        //    light moved out from under. One dispatch over the WHOLE table:
        //    the kernel forks on the index, so local pages cannot be forgotten
        //    here the way a second dispatch could be.
        DispatchKernel(m_FreeWrappedShader, VSM::kTotalPageTableEntries, 64);

        // 2. Re-dirty resident pages a dynamic caster moved through.
        //
        // The pass block is filled EXPLICITLY here, unlike before issue #703.
        // This dispatch runs before the HPB loop, so whatever the block held was
        // left by the PREVIOUS frame's cull or raster — which was harmless while
        // the kernel read nothing from it, and is a garbage domain selector now
        // that it does.
        if (invalidationCount > 0 && m_InvalidateShader)
        {
            VSM::PassUBO pass{};
            pass.Params.y = 0u; // directional domain
            m_PassUBO->SetData(&pass, VSM::PassUBO::GetSize());
            m_PassUBO->Bind();

            m_InvalidateShader->Bind();
            RenderCommand::DispatchCompute(invalidationCount * VSM::kClipLevels, 1, 1);
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);

            // 2b. The same, over the local layers a mover passed through. Without
            //     it a lamp's cached pages keep the object's OLD silhouette and
            //     its shadow trails behind it — the "shadow stayed behind"
            //     artefact, which reads as an animation or transform-sync bug.
            if (AreLocalLightsActive() && m_LocalLayerHighWater > 0)
            {
                pass.Params.y = 1u; // local domain
                pass.Params.z = m_LocalLayerHighWater;
                m_PassUBO->SetData(&pass, VSM::PassUBO::GetSize());
                m_PassUBO->Bind();

                m_InvalidateShader->Bind();
                // TWO DIMENSIONS, not one flattened count. The product is up to
                // kMaxInvalidations x kMaxLocalLayers = 262,144 work groups, and
                // the GL 4.6 / Vulkan guaranteed minimum for a single dimension of
                // GL_MAX_COMPUTE_WORK_GROUP_COUNT is 65,535. Flattened, a
                // conforming implementation at the minimum REJECTS the dispatch —
                // and the failure is silent in the frame: every local invalidation
                // that frame is dropped, so movers trail their shadows again, on
                // that hardware only. This box (NVIDIA, ~2^31 per axis) would
                // never have shown it.
                //
                // Split, each axis is bounded by its own budget (1024 and 256) and
                // both are two orders of magnitude inside the guarantee. The
                // directional twin above needs no such care: it is bounded by
                // kClipLevels = 16.
                RenderCommand::DispatchCompute(invalidationCount, m_LocalLayerHighWater, 1);
                RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
            }
        }

        const u32 physicalPageCount = m_PhysicalPageTableResolution * m_PhysicalPageTableResolution;

        // 3./4. Build the free lists, then back every pending request.
        DispatchKernel(m_FindFreeShader, physicalPageCount, 64);
        DispatchKernel(m_AllocateShader, VSM::kMaxRequests, 64);

        // 5. Clear only the pages that will be redrawn — one workgroup per page.
        if (m_ClearPagesShader)
        {
            // Shader FIRST, then the image. HeapBinding::BindImageOrOffset forks on
            // the program IN FLIGHT, so binding the image before the shader would
            // ask the question of whichever program happened to be bound last —
            // and if that one were bindless the seam would stage an offset and
            // issue no bind, silently dropping every write. Bound in this order
            // the fork sees a VSM shader, which is slot-declared
            // (`layout(r32ui, binding = 0)`), takes the fallback and really binds.
            m_ClearPagesShader->Bind();
            BindPhysicalPoolImage();
            RenderCommand::DispatchCompute(physicalPageCount, 1, 1);
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::ShaderImageAccess);
        }

        // 6. The dirty-flag pyramid, one dispatch per mip (a cross-workgroup
        //    reduction needs a real barrier between levels).
        for (u32 mip = 0; mip < VSM::kHPBMipCount; ++mip)
        {
            VSM::PassUBO pass{};
            pass.Params.x = mip;
            pass.Params.y = 0u; // directional domain
            m_PassUBO->SetData(&pass, VSM::PassUBO::GetSize());
            m_PassUBO->Bind();

            const u32 mipRes = VSM::kPageTableResolution >> mip;
            DispatchKernel(m_BuildHPBShader, mipRes * mipRes * VSM::kClipLevels, 64);
        }

        // 6b. The same pyramid per local layer (issue #703). Its mip 0 does more
        //     than the directional one — it GATHERS across the layer's six
        //     page-table mips and, in the same pass, atomicMins the layer's raster
        //     mip. Both products come out of the one sweep because both are
        //     answers to "what is dirty in this layer", and computing them apart
        //     would be two reads of the same 1365 entries.
        if (AreLocalLightsActive() && m_LocalLayerHighWater > 0)
        {
            for (u32 mip = 0; mip < VSM::kLocalMipCount; ++mip)
            {
                VSM::PassUBO pass{};
                pass.Params.x = mip;
                pass.Params.y = 1u; // local domain
                pass.Params.z = m_LocalLayerHighWater;
                m_PassUBO->SetData(&pass, VSM::PassUBO::GetSize());
                m_PassUBO->Bind();

                const u32 mipRes = VSM::kLocalPageTableResolution >> mip;
                DispatchKernel(m_BuildHPBShader, mipRes * mipRes * m_LocalLayerHighWater, 64);
            }
        }
    }

    // -------------------------------------------------------------------------
    // Step 7 — cull + raster
    // -------------------------------------------------------------------------

    bool VirtualShadowMap::RenderCasters(const std::vector<ShadowMeshCaster>& meshCasters,
                                         const std::vector<ShadowSkinnedCaster>& skinnedCasters,
                                         const glm::vec3& renderOrigin,
                                         const BoneUploader& uploadBones)
    {
        OLO_PROFILE_FUNCTION();

        if (!IsActive() || !m_RasterFramebuffer.IsValid())
            return false;
        if (meshCasters.empty() && skinnedCasters.empty())
            return false;

        // ---- Batch the static casters by (VAO, index range, cull mode) --------
        // Same key as ShadowRenderPass's CSM path: casters sharing it read the
        // same submesh, so they collapse into one instanced indirect draw.
        m_Batches.clear();
        m_CullInput.clear();
        m_BatchLookup.clear();

        const auto batchKeyOf = [](const ShadowMeshCaster& caster, RHI::ResourceHandle drawVao)
        {
            return BatchKey{ (static_cast<u64>(drawVao.Generation) << 32) | drawVao.Index,
                             caster.indexCount, caster.baseIndex, caster.twoSided };
        };

        // With local lights on, each batch needs TWO indirect commands out of the
        // one kMaxBatches-long command buffer — the directional one at index b
        // and the local one at batchCount + b. Halving the batch cap is what
        // keeps that in bounds without growing the buffer for the far commoner
        // directional-only case.
        const bool localActive = AreLocalLightsActive() && m_LocalLayerHighWater > 0;
        const u32 batchCap = localActive ? (VSM::kMaxBatches / 2) : VSM::kMaxBatches;

        for (const auto& caster : meshCasters)
        {
            const RHI::ResourceHandle drawVao = caster.shadowVaoID.IsValid() ? caster.shadowVaoID : caster.vaoID;
            if (!drawVao.IsValid() || caster.indexCount == 0)
                continue;

            const auto [slot, inserted] =
                m_BatchLookup.try_emplace(batchKeyOf(caster, drawVao), static_cast<u32>(m_Batches.size()));
            if (inserted)
            {
                if (m_Batches.size() >= batchCap)
                {
                    // Over budget: forget the tentative mapping so the second
                    // pass skips this caster, exactly as the old scan did.
                    m_BatchLookup.erase(slot->first);
                    continue;
                }
                m_Batches.push_back({ drawVao, caster.indexCount, caster.baseIndex, caster.twoSided, 0, 0 });
            }
            m_Batches[slot->second].CasterCount += 1;
        }

        if (m_Batches.empty() && skinnedCasters.empty())
            return false;

        // Each batch's compacted run is sized EXACTLY casterCount * clipLevels, so
        // the cull's per-batch counter cannot overflow it — which is why the
        // shader's overflow branch is a tripwire rather than a real path.
        u32 runCursor = 0;
        for (auto& batch : m_Batches)
        {
            batch.RunBase = runCursor;
            runCursor += batch.CasterCount * VSM::kClipLevels;
        }

        // Skinned casters are CPU-scheduled (their bone palette is per-caster, so
        // they cannot share an instanced batch) and take the tail of the same
        // buffer. Reserving from the end keeps the cull's region contiguous.
        const u32 skinnedReserve = static_cast<u32>(skinnedCasters.size()) * VSM::kClipLevels;
        if (runCursor + skinnedReserve > VSM::kMaxDrawInstances)
        {
            if (!m_LoggedDrawBudgetExhausted)
            {
                OLO_CORE_WARN("VirtualShadowMap: draw-instance budget exhausted ({} needed, {} available) — "
                              "raise VSM::kMaxDrawInstances or reduce shadow casters",
                              runCursor + skinnedReserve, VSM::kMaxDrawInstances);
                m_LoggedDrawBudgetExhausted = true;
            }
            return false;
        }
        const u32 skinnedBase = runCursor;
        runCursor += skinnedReserve;

        // ---- Local-light runs, after the directional ones ---------------------
        //
        // The two rasters share m_DrawInstances, and the directional half is laid
        // out first because it is the one that must never be dropped: it replaces
        // the CSM outright, whereas dropping the local half costs an unshadowed
        // lamp. When the rest of the buffer cannot hold the local runs the local
        // raster is skipped for the frame and says so — the same "a budget may
        // drop work, but it says so" contract the three budgets above keep.
        bool localFits = localActive;
        if (localActive)
        {
            u32 localCursor = runCursor;
            for (auto& batch : m_Batches)
            {
                batch.LocalRunBase = localCursor;
                batch.LocalRunCapacity = batch.CasterCount * VSM::kLocalLayersPerCaster;
                localCursor += batch.LocalRunCapacity;
            }
            const u32 localSkinnedReserve =
                static_cast<u32>(skinnedCasters.size()) * VSM::kLocalLayersPerCaster;
            if (localCursor + localSkinnedReserve > VSM::kMaxDrawInstances)
            {
                if (!m_LoggedLocalDrawBudgetExhausted)
                {
                    OLO_CORE_WARN("VirtualShadowMap: local draw-instance budget exhausted ({} needed, {} "
                                  "available) — local-light shadows are skipped this frame. Raise "
                                  "VSM::kMaxDrawInstances or lower VSM::kLocalLayersPerCaster",
                                  localCursor + localSkinnedReserve, VSM::kMaxDrawInstances);
                    m_LoggedLocalDrawBudgetExhausted = true;
                }
                localFits = false;
            }
            else
            {
                m_LocalInstanceBase = localCursor;
                m_LocalInstanceCapacity = localSkinnedReserve;
            }
        }
        if (!localFits)
        {
            m_LocalInstanceBase = 0;
            m_LocalInstanceCapacity = 0;
            for (auto& batch : m_Batches)
            {
                batch.LocalRunBase = 0;
                batch.LocalRunCapacity = 0;
            }
        }

        // ---- Cull input + indirect commands ---------------------------------
        //
        // Two commands per batch when local lights are on: [0, batchCount) are
        // the directional draws and [batchCount, 2*batchCount) the local ones.
        // One buffer, disjoint ranges — the cull kernels never write each other's.
        const auto batchCount = static_cast<u32>(m_Batches.size());
        const u32 commandCount = localFits ? (batchCount * 2u) : batchCount;
        m_CullInput.reserve(meshCasters.size());
        m_DrawCommandStaging.assign(commandCount, VSM::DrawCommand{});
        for (u32 i = 0; i < batchCount; ++i)
        {
            m_DrawCommandStaging[i].IndexCount = m_Batches[i].IndexCount;
            m_DrawCommandStaging[i].InstanceCount = 0; // the cull writes this
            m_DrawCommandStaging[i].FirstIndex = m_Batches[i].BaseIndex;
            m_DrawCommandStaging[i].BaseVertex = 0;
            m_DrawCommandStaging[i].BaseInstance = 0;
            if (localFits)
                m_DrawCommandStaging[batchCount + i] = m_DrawCommandStaging[i];
        }

        for (const auto& caster : meshCasters)
        {
            const RHI::ResourceHandle drawVao = caster.shadowVaoID.IsValid() ? caster.shadowVaoID : caster.vaoID;
            if (!drawVao.IsValid() || caster.indexCount == 0)
                continue;

            const auto found = m_BatchLookup.find(batchKeyOf(caster, drawVao));
            if (found == m_BatchLookup.end())
                continue; // dropped by the batch budget in the first pass

            const u32 batchIndex = found->second;
            const Batch& batch = m_Batches[batchIndex];

            VSM::CullInstance entry{};
            entry.Transform = MakeModelRelative(caster.transform, renderOrigin);
            const bool unbounded = caster.WorldBounds.Min.x >= std::numeric_limits<f32>::max();
            if (!unbounded)
            {
                entry.BoundsMin = glm::vec4(caster.WorldBounds.Min - renderOrigin, 0.0f);
                entry.BoundsMax = glm::vec4(caster.WorldBounds.Max - renderOrigin, 0.0f);
            }
            entry.Batch = glm::uvec4(batchIndex, batch.RunBase, batch.CasterCount * VSM::kClipLevels,
                                     unbounded ? 1u : 0u);
            // The local cull's command index is the batch's, shifted past the
            // directional block — see the two-commands-per-batch note above.
            entry.LocalBatch = glm::uvec4(batchCount + batchIndex, batch.LocalRunBase,
                                          batch.LocalRunCapacity, 0u);
            m_CullInput.push_back(entry);
        }

        if (m_CullInput.size() > VSM::kMaxCasters)
        {
            // Same contract as the batch and draw-instance budgets above: a
            // budget may drop work, but it says so. Silent truncation here is a
            // caster whose shadow vanishes with nothing to grep for.
            if (!m_LoggedCasterBudgetExhausted)
            {
                OLO_CORE_WARN("VirtualShadowMap: caster budget exhausted ({} submitted, {} kept) — "
                              "raise VSM::kMaxCasters or reduce shadow casters",
                              m_CullInput.size(), VSM::kMaxCasters);
                m_LoggedCasterBudgetExhausted = true;
            }
            m_CullInput.resize(VSM::kMaxCasters);
        }

        if (!m_CullInput.empty())
        {
            m_CullInstances->SetData(m_CullInput.data(),
                                     static_cast<u32>(m_CullInput.size() * sizeof(VSM::CullInstance)));
        }
        if (!m_DrawCommandStaging.empty())
        {
            m_DrawCommands->SetData(m_DrawCommandStaging.data(),
                                    static_cast<u32>(m_DrawCommandStaging.size() * sizeof(VSM::DrawCommand)));
        }

        BindWorkingSet();

        // ---- The cull ---------------------------------------------------------
        if (!m_CullInput.empty() && m_CullShader)
        {
            VSM::PassUBO pass{};
            pass.Params.x = static_cast<u32>(m_CullInput.size());
            m_PassUBO->SetData(&pass, VSM::PassUBO::GetSize());
            m_PassUBO->Bind();

            m_CullShader->Bind();
            RenderCommand::DispatchCompute(
                DivideRoundUp(static_cast<u32>(m_CullInput.size()) * VSM::kClipLevels, 64), 1, 1);
            // The two culls write disjoint commands and disjoint instance runs,
            // so they only need to be ordered against the DRAWS, not against each
            // other — but they do share b_CullInstances and b_DrawCommands, so
            // the local one is dispatched here rather than after the directional
            // raster, where it would race the draw reading those commands.
            if (localFits && m_CullLocalShader)
            {
                VSM::PassUBO localPass{};
                localPass.Params.x = static_cast<u32>(m_CullInput.size());
                localPass.Params.z = m_LocalLayerHighWater;
                m_PassUBO->SetData(&localPass, VSM::PassUBO::GetSize());
                m_PassUBO->Bind();

                m_CullLocalShader->Bind();
                RenderCommand::DispatchCompute(
                    DivideRoundUp(static_cast<u32>(m_CullInput.size()) * m_LocalLayerHighWater, 64), 1, 1);
            }
            // The indirect commands the cull just wrote are consumed by the draw
            // path, so the barrier has to cover Command as well as ShaderStorage —
            // dropping Command is the classic "the draw used last frame's counts"
            // bug, and it looks like flickering geometry, not like a missing barrier.
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::Command);
        }

        // ---- Raster ----------------------------------------------------------
        const auto previousViewport = RenderCommand::GetViewport();

        RenderCommand::BindFramebuffer(m_RasterFramebuffer);
        RenderCommand::SetViewport(0, 0, VSM::kVirtualResolution, VSM::kVirtualResolution);
        RenderCommand::DisableScissorTest();

        // No depth attachment and no colour writes: visibility is resolved by
        // imageAtomicMin inside the fragment stage (see
        // include/VirtualShadowRasterStage.glsl).
        RenderCommand::SetDepthTest(false);
        RenderCommand::SetDepthMask(false);
        RenderCommand::SetColorMask(false, false, false, false);
        RenderCommand::EnableCulling();
        RenderCommand::FrontCull();

        u32 drawnBatches = 0;
        if (m_DepthShader && !m_Batches.empty())
        {
            m_DepthShader->Bind();
            // After the shader bind, not before — see BindPhysicalPoolImage().
            BindPhysicalPoolImage();
            bool cullingDisabled = false;
            for (sizet i = 0; i < m_Batches.size(); ++i)
            {
                const auto& batch = m_Batches[i];
                if (batch.TwoSided != cullingDisabled)
                {
                    if (batch.TwoSided)
                        RenderCommand::DisableCulling();
                    else
                    {
                        RenderCommand::EnableCulling();
                        RenderCommand::FrontCull();
                    }
                    cullingDisabled = batch.TwoSided;
                }

                VSM::PassUBO pass{};
                pass.Params.x = batch.RunBase;
                m_PassUBO->SetData(&pass, VSM::PassUBO::GetSize());
                m_PassUBO->Bind();

                // One command per batch, its instance count GPU-written. The count
                // source is a constant 1; what varies is the instance count inside
                // the command, which is exactly what must not round-trip the CPU.
                RenderCommand::MultiDrawElementsIndirectCountRaw(
                    batch.Vao, m_DrawCommands->GetRHIHandle(),
                    static_cast<u32>(i * sizeof(VSM::DrawCommand)),
                    m_DrawCountBuffer->GetRHIHandle(), 0, 1, sizeof(VSM::DrawCommand));
                ++drawnBatches;
            }
            if (cullingDisabled)
            {
                RenderCommand::EnableCulling();
                RenderCommand::FrontCull();
            }
        }

        drawnBatches += RenderSkinnedCasters(skinnedCasters, renderOrigin, skinnedBase, uploadBones);

        // ---- Local-light raster (issue #703) ---------------------------------
        //
        // A second pass over the same casters into the same pool, at the LOCAL
        // virtual resolution. It cannot share the directional draws: a different
        // projection source, a different viewport and a per-instance mip scale.
        if (localFits)
        {
            RenderCommand::SetViewport(0, 0, VSM::kLocalVirtualResolution, VSM::kLocalVirtualResolution);
            drawnBatches += RenderLocalCasters(skinnedCasters, renderOrigin, m_LocalInstanceBase, uploadBones);
        }

        // Restore. The physical pool is read by the lit pass as a texture, so the
        // image writes above must be visible to a texture fetch, not just to
        // another image access.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch);

        RenderCommand::SetColorMask(true, true, true, true);
        RenderCommand::SetDepthTest(true);
        RenderCommand::SetDepthMask(true);
        RenderCommand::BackCull();
        RenderCommand::BindDefaultFramebuffer();
        // Restore only a REAL viewport. GetViewport() answers {0,0,0,0} when
        // nothing has set one yet — VSM runs at the very top of the frame, so
        // that is the normal case on the first frame and in any harness that
        // drives it directly. GL shrugs at glViewport(0,0,0,0); Vulkan does not:
        // vkCmdSetViewport with width 0 is VUID-VkViewport-width-01770, one
        // validation error per frame, and in a suite that gates on zero errors
        // it fails whatever else the pass did correctly.
        if (previousViewport.width > 0 && previousViewport.height > 0)
        {
            RenderCommand::SetViewport(previousViewport.x, previousViewport.y,
                                       previousViewport.width, previousViewport.height);
        }

        return drawnBatches > 0;
    }

    u32 VirtualShadowMap::RenderSkinnedCasters(const std::vector<ShadowSkinnedCaster>& skinnedCasters,
                                               const glm::vec3& renderOrigin, u32 instanceBase,
                                               const BoneUploader& uploadBones)
    {
        if (skinnedCasters.empty() || !m_DepthSkinnedShader)
            return 0;

        // Deliberately no HPB test here — see the header comment on
        // VSM_DepthSkinned.glsl. Correctness is preserved by the fragment stage's
        // not-dirty early-out; what a skinned caster loses is only the chance to
        // skip its vertex work.
        m_DepthSkinnedShader->Bind();
        // Again after the shader bind, and again unconditionally rather than
        // relying on the static path having run: a scene whose only casters are
        // skinned never enters that branch, and an unbound image unit makes every
        // imageAtomicMin here a no-op with no diagnostic.
        BindPhysicalPoolImage();

        u32 drawn = 0;
        u32 cursor = instanceBase;
        m_SkinnedInstanceStaging.clear();

        for (const auto& caster : skinnedCasters)
        {
            if (!caster.vaoID.IsValid() || caster.indexCount == 0)
                continue;

            const glm::mat4 transform = MakeModelRelative(caster.transform, renderOrigin);
            const bool unbounded = caster.WorldBounds.Min.x >= std::numeric_limits<f32>::max();

            m_SkinnedInstanceStaging.clear();
            for (u32 level = 0; level < VSM::kClipLevels; ++level)
            {
                if (!unbounded)
                {
                    // Cheap CPU frustum reject against this level's ortho box.
                    const glm::mat4& viewProj = m_Globals.Clips[level].ViewProjection;
                    glm::vec3 ndcMin(std::numeric_limits<f32>::max());
                    glm::vec3 ndcMax(std::numeric_limits<f32>::lowest());
                    const glm::vec3 boundsMin = caster.WorldBounds.Min - renderOrigin;
                    const glm::vec3 boundsMax = caster.WorldBounds.Max - renderOrigin;
                    for (u32 corner = 0; corner < 8; ++corner)
                    {
                        const glm::vec3 point((corner & 1u) != 0u ? boundsMax.x : boundsMin.x,
                                              (corner & 2u) != 0u ? boundsMax.y : boundsMin.y,
                                              (corner & 4u) != 0u ? boundsMax.z : boundsMin.z);
                        const glm::vec4 clipPos = viewProj * glm::vec4(point, 1.0f);
                        const glm::vec3 ndc = glm::vec3(clipPos) / clipPos.w;
                        ndcMin = glm::min(ndcMin, ndc);
                        ndcMax = glm::max(ndcMax, ndc);
                    }
                    if (glm::any(glm::greaterThan(ndcMin, glm::vec3(1.0f))) ||
                        glm::any(glm::lessThan(ndcMax, glm::vec3(-1.0f))))
                    {
                        continue;
                    }
                }

                VSM::DrawInstance record{};
                record.Transform = transform;
                record.ClipLevel = level;
                m_SkinnedInstanceStaging.push_back(record);
            }

            if (m_SkinnedInstanceStaging.empty())
                continue;

            const auto instanceCount = static_cast<u32>(m_SkinnedInstanceStaging.size());
            if (cursor + instanceCount > VSM::kMaxDrawInstances)
                break;

            m_DrawInstances->SetData(m_SkinnedInstanceStaging.data(),
                                     instanceCount * static_cast<u32>(sizeof(VSM::DrawInstance)),
                                     cursor * static_cast<u32>(sizeof(VSM::DrawInstance)));

            VSM::PassUBO pass{};
            pass.Params.x = cursor;
            m_PassUBO->SetData(&pass, VSM::PassUBO::GetSize());
            m_PassUBO->Bind();

            if (uploadBones)
                uploadBones(caster);

            RenderCommand::DrawIndexedInstancedRaw(caster.vaoID, caster.indexCount, caster.baseIndex, instanceCount);
            cursor += instanceCount;
            ++drawn;
        }

        return drawn;
    }

    u32 VirtualShadowMap::RenderLocalCasters(const std::vector<ShadowSkinnedCaster>& skinnedCasters,
                                             const glm::vec3& renderOrigin, u32 instanceBase,
                                             const BoneUploader& uploadBones)
    {
        u32 drawn = 0;
        const auto batchCount = static_cast<u32>(m_Batches.size());

        if (m_DepthLocalShader && batchCount > 0)
        {
            m_DepthLocalShader->Bind();
            // After the shader bind, not before — see BindPhysicalPoolImage().
            BindPhysicalPoolImage();

            bool cullingDisabled = false;
            for (u32 i = 0; i < batchCount; ++i)
            {
                const auto& batch = m_Batches[i];
                if (batch.LocalRunCapacity == 0)
                    continue;

                if (batch.TwoSided != cullingDisabled)
                {
                    if (batch.TwoSided)
                        RenderCommand::DisableCulling();
                    else
                    {
                        RenderCommand::EnableCulling();
                        RenderCommand::FrontCull();
                    }
                    cullingDisabled = batch.TwoSided;
                }

                VSM::PassUBO pass{};
                pass.Params.x = batch.LocalRunBase;
                m_PassUBO->SetData(&pass, VSM::PassUBO::GetSize());
                m_PassUBO->Bind();

                RenderCommand::MultiDrawElementsIndirectCountRaw(
                    batch.Vao, m_DrawCommands->GetRHIHandle(),
                    static_cast<u32>((batchCount + i) * sizeof(VSM::DrawCommand)),
                    m_DrawCountBuffer->GetRHIHandle(), 0, 1, sizeof(VSM::DrawCommand));
                ++drawn;
            }
            if (cullingDisabled)
            {
                RenderCommand::EnableCulling();
                RenderCommand::FrontCull();
            }
        }

        // Skinned casters, CPU-scheduled exactly as in RenderSkinnedCasters: a
        // per-caster bone palette cannot share an instanced batch. The CPU test
        // is a sphere overlap rather than the directional path's eight-corner
        // NDC reject, because a local layer's frustum is a perspective and
        // projecting an AABB corner behind the near plane divides by a negative w
        // — the classic wrong-side-of-the-camera false reject, which here would
        // drop a character's shadow only when it stood close to the lamp.
        if (m_DepthLocalSkinnedShader && !skinnedCasters.empty() && m_LocalInstanceCapacity > 0)
        {
            m_DepthLocalSkinnedShader->Bind();
            BindPhysicalPoolImage();

            u32 cursor = instanceBase;
            for (const auto& caster : skinnedCasters)
            {
                if (!caster.vaoID.IsValid() || caster.indexCount == 0)
                    continue;

                const glm::mat4 transform = MakeModelRelative(caster.transform, renderOrigin);
                const bool unbounded = caster.WorldBounds.Min.x >= std::numeric_limits<f32>::max();
                const glm::vec3 boundsMin = caster.WorldBounds.Min - renderOrigin;
                const glm::vec3 boundsMax = caster.WorldBounds.Max - renderOrigin;

                m_LocalSkinnedStaging.clear();
                for (u32 layer = 0; layer < m_LocalLayerHighWater; ++layer)
                {
                    const VSM::LocalLight& light = m_LocalLayers[layer];
                    if (light.Params.w < 0.5f)
                        continue; // layer not serving a light this frame

                    if (!unbounded)
                    {
                        // Closest point of the AABB to the light centre, against
                        // the light's range.
                        const glm::vec3 lightPos(light.PositionRange);
                        const glm::vec3 closest = glm::clamp(lightPos, boundsMin, boundsMax);
                        const glm::vec3 delta = closest - lightPos;
                        if (glm::dot(delta, delta) > light.PositionRange.w * light.PositionRange.w)
                            continue;
                    }

                    VSM::DrawInstance record{};
                    record.Transform = transform;
                    record.LocalLayer = layer;
                    m_LocalSkinnedStaging.push_back(record);
                    if (m_LocalSkinnedStaging.size() >= VSM::kLocalLayersPerCaster)
                        break;
                }

                if (m_LocalSkinnedStaging.empty())
                    continue;

                const auto instanceCount = static_cast<u32>(m_LocalSkinnedStaging.size());
                if (cursor + instanceCount > instanceBase + m_LocalInstanceCapacity)
                    break;

                m_DrawInstances->SetData(m_LocalSkinnedStaging.data(),
                                         instanceCount * static_cast<u32>(sizeof(VSM::DrawInstance)),
                                         cursor * static_cast<u32>(sizeof(VSM::DrawInstance)));

                VSM::PassUBO pass{};
                pass.Params.x = cursor;
                m_PassUBO->SetData(&pass, VSM::PassUBO::GetSize());
                m_PassUBO->Bind();

                if (uploadBones)
                    uploadBones(caster);

                RenderCommand::DrawIndexedInstancedRaw(caster.vaoID, caster.indexCount, caster.baseIndex,
                                                       instanceCount);
                cursor += instanceCount;
                ++drawn;
            }
        }

        return drawn;
    }

    // -------------------------------------------------------------------------
    // Step 8 — end of the shadow pass
    // -------------------------------------------------------------------------

    void VirtualShadowMap::EndFrame()
    {
        OLO_PROFILE_FUNCTION();

        if (!IsActive())
            return;

        BindWorkingSet();
        DispatchKernel(m_EndFrameShader, VSM::kTotalPageTableEntries, 64);

        m_PrevOrigins = m_CurrOrigins;
        m_FullInvalidate = false;
    }

    // -------------------------------------------------------------------------
    // Step 9 — page marking, from the late graph node
    // -------------------------------------------------------------------------

    void VirtualShadowMap::MarkRequiredPages(RHI::ResourceHandle sceneDepth, u32 depthWidth, u32 depthHeight,
                                             const glm::mat4& inverseViewProjection,
                                             const glm::vec3& cameraPositionRelative)
    {
        OLO_PROFILE_FUNCTION();

        if (!IsActive() || !m_MarkShader || !sceneDepth.IsValid() || depthWidth == 0 || depthHeight == 0)
            return;

        // Stride 2: one mark per 2x2 depth quad. A page covers roughly its own
        // size in screen pixels at the level the heuristic picks, so halving the
        // sample rate on each axis still puts hundreds of samples inside every
        // page — it reduces the atomics, not the coverage.
        constexpr i32 kMarkStride = 2;

        m_Globals.InverseViewProjection = inverseViewProjection;
        m_Globals.CameraPosition = glm::vec4(cameraPositionRelative, 0.0f);
        m_Globals.Params3 = glm::ivec4(static_cast<i32>(depthWidth), static_cast<i32>(depthHeight), kMarkStride, 0);

        BindWorkingSet();

        // Publish-AND-bind, not the forking BindTextureOrOffset: this runs before
        // the mark shader is bound, so the fork would answer for whatever program
        // was last in flight. PublishTextureOffsetAndBind is route-agnostic — it
        // stages the offset AND issues the bind — so the order does not matter and
        // the slot-declared `layout(binding = 19) uniform sampler2D` in
        // VSM_MarkRequiredPages.comp is served either way. Same call, same reason,
        // as BindForSampling's publish of the physical pool.
        // The default SamplerDesc is deliberate here (unlike BindForSampling's
        // explicit one): it means INHERIT the texture object's own state, which is
        // exactly what the plain bind this replaced did, so the depth buffer is
        // still sampled with whatever the G-Buffer gave it.
        HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_POSTPROCESS_DEPTH, sceneDepth,
                                                 RHI::HeapSlotLifetime::FrameTransient);

        m_MarkShader->Bind();
        const u32 groupsX = DivideRoundUp(DivideRoundUp(depthWidth, static_cast<u32>(kMarkStride)), 8u);
        const u32 groupsY = DivideRoundUp(DivideRoundUp(depthHeight, static_cast<u32>(kMarkStride)), 8u);
        RenderCommand::DispatchCompute(groupsX, groupsY, 1);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
    }

    // -------------------------------------------------------------------------
    // Binding + diagnostics
    // -------------------------------------------------------------------------

    void VirtualShadowMap::BindForSampling()
    {
        if (!m_Initialized)
            return;

        // Params2.x is what the shader branches on, so an inactive VSM uploads a
        // disabled block rather than relying on every consumer remembering to ask.
        auto globals = m_Globals;
        globals.Params2.x = IsActive() ? 1 : 0;
        globals.Params4.x = (IsActive() && m_Settings.LocalLights) ? 1 : 0;
        m_GlobalsUBO->SetData(&globals, VSM::GlobalsUBO::GetSize());
        m_GlobalsUBO->Bind();

        m_PageTable->Bind();
        // The lit pass needs the layer projections to sample a local light, so
        // this rides the same publish as the page table. Bound even when local
        // lights are off: Params4.x is what the shader branches on, and a shader
        // that declares the block still needs SOMETHING bound at 78 or the
        // read is undefined rather than merely unused.
        m_LocalLights->Bind();

        // PUBLISH AND BIND, not one or the other. VirtualShadowSampling.glsl is a
        // shared header, so converting its declaration would drag every includer
        // onto the raw-GLSL route (glsl-shaders.md §5e, first row) — but its
        // includers are already MIXED: DeferredLighting takes the bindless route
        // while DeferredLighting_MSAA does not. Staging an offset AND issuing a
        // real bind is what serves both from one declaration, and it is the
        // mechanism BindlessShaderPipelineTest's allowlist entry points at.
        //
        // Persistent, not FrameTransient: the pool is owned by this system for the
        // lifetime of the settings, never by the graph's transient pool, so the
        // memoised offset cannot come to name someone else's target.
        //
        // Nearest + ClampToEdge: the stored value is a float BIT PATTERN, so any
        // filtering at all would interpolate between exponents and produce a depth
        // that is not merely inaccurate but unrelated to either neighbour.
        RHI::SamplerDesc poolSampler{};
        // Explicit, not the default: a default-constructed SamplerDesc is a request
        // to INHERIT the texture object's state, so the fields below would be
        // ignored (issue #691 Phase 3) and the pool could end up sampled with
        // whatever GL's texture default happens to be.
        poolSampler.Source = RHI::SamplerSource::Explicit;
        poolSampler.LinearMipFilter = false;
        poolSampler.MinFilter = RHI::Filter::Nearest;
        poolSampler.MagFilter = RHI::Filter::Nearest;
        poolSampler.AddressU = RHI::AddressMode::ClampToEdge;
        poolSampler.AddressV = RHI::AddressMode::ClampToEdge;
        HeapBinding::PublishTextureOffsetAndBind(ShaderBindingLayout::TEX_VSM_PHYSICAL, m_PhysicalPool,
                                                 RHI::HeapSlotLifetime::Persistent, poolSampler);
    }

    void VirtualShadowMap::ReadbackStatistics()
    {
        // Called BEFORE the write index flips, so m_StatsWriteIndex still names
        // the buffer the PREVIOUS frame wrote — which the GPU finished with a
        // whole frame ago. The counters therefore cost one stale frame rather
        // than a pipeline stall, and acceptance criterion #2 needs PagesDrawn
        // observable every frame, so this is not gated behind a debug flag.
        const auto& source = m_StatsBuffers[m_StatsWriteIndex];
        if (!source)
            return;
        // The kernels that wrote this buffer were fenced with ShaderStorage
        // barriers, which order SHADER reads — a CPU GetData is a buffer-update
        // client and needs its own barrier class. The frame-late read makes the
        // race close to unhittable in practice, which is exactly why it would
        // ship: it costs one barrier to make the ordering guaranteed instead of
        // probable.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::BufferUpdate);
        VSM::Statistics readback{};
        source->GetData(&readback, sizeof(VSM::Statistics));
        m_Statistics = readback;
    }
} // namespace OloEngine

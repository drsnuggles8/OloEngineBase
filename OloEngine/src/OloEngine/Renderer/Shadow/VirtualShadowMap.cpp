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
        m_EndFrameShader = ComputeShader::Create("assets/shaders/compute/VSM_EndFrame.comp");
        m_DepthShader = Shader::Create("assets/shaders/VSM_Depth.glsl");
        m_DepthSkinnedShader = Shader::Create("assets/shaders/VSM_DepthSkinned.glsl");

        const bool computeOk = m_FreeWrappedShader && m_FreeWrappedShader->IsValid() &&
                               m_MarkShader && m_MarkShader->IsValid() &&
                               m_InvalidateShader && m_InvalidateShader->IsValid() &&
                               m_FindFreeShader && m_FindFreeShader->IsValid() &&
                               m_AllocateShader && m_AllocateShader->IsValid() &&
                               m_ClearPagesShader && m_ClearPagesShader->IsValid() &&
                               m_BuildHPBShader && m_BuildHPBShader->IsValid() &&
                               m_CullShader && m_CullShader->IsValid() &&
                               m_EndFrameShader && m_EndFrameShader->IsValid();
        return computeOk && m_DepthShader && m_DepthSkinnedShader;
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

        m_PageTable = SB::Create(VSM::kTotalVirtualPages * sizeof(u32),
                                 ShaderBindingLayout::SSBO_VSM_PAGE_TABLE, kGPUWritten);
        m_MetaTable = SB::Create(physicalPageCount * sizeof(u32),
                                 ShaderBindingLayout::SSBO_VSM_META_TABLE, kGPUWritten);
        m_HPB = SB::Create(VSM::kHPBTotalEntries * sizeof(u32),
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

        // glMultiDrawElementsIndirectCount sources its draw count from a GPU
        // buffer. Every VSM batch issues exactly one command, so this holds a
        // constant 1 — the count is fixed but the INSTANCE count inside the
        // command is not, and that is the number the cull writes.
        m_DrawCountBuffer = SB::Create(sizeof(u32), ShaderBindingLayout::SSBO_VSM_DRAW_COMMANDS);
        const u32 oneDraw = 1u;
        m_DrawCountBuffer->SetData(&oneDraw, sizeof(u32));

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
        for (auto& stats : m_StatsBuffers)
        {
            if (stats)
                stats->ClearData();
        }
        m_Statistics = {};
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
        m_EndFrameShader.Reset();
        m_DepthShader.Reset();
        m_DepthSkinnedShader.Reset();
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
        const bool invalidates = settings.Clip0HalfExtent != m_Settings.Clip0HalfExtent ||
                                 settings.DepthRange != m_Settings.DepthRange ||
                                 settings.ClipSelectionBias != m_Settings.ClipSelectionBias;

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
        bytes += static_cast<u64>(VSM::kTotalVirtualPages) * sizeof(u32);                     // page table
        bytes += physicalPageCount * sizeof(u32);                                             // meta table
        bytes += static_cast<u64>(VSM::kHPBTotalEntries) * sizeof(u32);                       // HPB
        bytes += static_cast<u64>(kRequestHeaderUints + 4 * VSM::kMaxRequests) * sizeof(u32);
        bytes += (kFreeListHeaderUints + 2 * physicalPageCount) * sizeof(u32);
        bytes += static_cast<u64>(VSM::kMaxCasters) * sizeof(VSM::CullInstance);
        bytes += static_cast<u64>(VSM::kMaxDrawInstances) * sizeof(VSM::DrawInstance);
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
                                      const glm::vec3& renderOrigin)
    {
        OLO_PROFILE_FUNCTION();

        if (!IsActive())
            return;

        const glm::vec3 forward = glm::normalize(lightDirection);

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

        m_PrevLightDirection = forward;
        m_PrevRenderOrigin = renderOrigin;
    }

    void VirtualShadowMap::SetSamplingParams(f32 softness, f32 maxShadowDistance)
    {
        m_Globals.Params1.x = softness;
        m_Globals.Params1.y = maxShadowDistance;
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

        // 1. Release the slots the clip frusta scrolled onto.
        DispatchKernel(m_FreeWrappedShader, VSM::kTotalVirtualPages, 64);

        // 2. Re-dirty resident pages a dynamic caster moved through.
        if (invalidationCount > 0 && m_InvalidateShader)
        {
            m_InvalidateShader->Bind();
            RenderCommand::DispatchCompute(invalidationCount * VSM::kClipLevels, 1, 1);
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
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
            m_PassUBO->SetData(&pass, VSM::PassUBO::GetSize());
            m_PassUBO->Bind();

            const u32 mipRes = VSM::kPageTableResolution >> mip;
            DispatchKernel(m_BuildHPBShader, mipRes * mipRes * VSM::kClipLevels, 64);
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

        for (const auto& caster : meshCasters)
        {
            const RHI::ResourceHandle drawVao = caster.shadowVaoID.IsValid() ? caster.shadowVaoID : caster.vaoID;
            if (!drawVao.IsValid() || caster.indexCount == 0)
                continue;

            auto it = std::ranges::find_if(m_Batches,
                                           [&](const Batch& b)
                                           {
                                               return b.Vao == drawVao && b.IndexCount == caster.indexCount &&
                                                      b.BaseIndex == caster.baseIndex && b.TwoSided == caster.twoSided;
                                           });
            if (it == m_Batches.end())
            {
                if (m_Batches.size() >= VSM::kMaxBatches)
                    continue;
                m_Batches.push_back({ drawVao, caster.indexCount, caster.baseIndex, caster.twoSided, 0, 0 });
                it = std::prev(m_Batches.end());
            }
            it->CasterCount += 1;
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

        // ---- Cull input + indirect commands ---------------------------------
        m_CullInput.reserve(meshCasters.size());
        m_DrawCommandStaging.assign(m_Batches.size(), VSM::DrawCommand{});
        for (sizet i = 0; i < m_Batches.size(); ++i)
        {
            m_DrawCommandStaging[i].IndexCount = m_Batches[i].IndexCount;
            m_DrawCommandStaging[i].InstanceCount = 0; // the cull writes this
            m_DrawCommandStaging[i].FirstIndex = m_Batches[i].BaseIndex;
            m_DrawCommandStaging[i].BaseVertex = 0;
            m_DrawCommandStaging[i].BaseInstance = 0;
        }

        for (const auto& caster : meshCasters)
        {
            const RHI::ResourceHandle drawVao = caster.shadowVaoID.IsValid() ? caster.shadowVaoID : caster.vaoID;
            if (!drawVao.IsValid() || caster.indexCount == 0)
                continue;

            const auto it = std::ranges::find_if(m_Batches,
                                                 [&](const Batch& b)
                                                 {
                                                     return b.Vao == drawVao && b.IndexCount == caster.indexCount &&
                                                            b.BaseIndex == caster.baseIndex &&
                                                            b.TwoSided == caster.twoSided;
                                                 });
            if (it == m_Batches.end())
                continue;

            const auto batchIndex = static_cast<u32>(std::distance(m_Batches.begin(), it));

            VSM::CullInstance entry{};
            entry.Transform = MakeModelRelative(caster.transform, renderOrigin);
            const bool unbounded = caster.WorldBounds.Min.x >= std::numeric_limits<f32>::max();
            if (!unbounded)
            {
                entry.BoundsMin = glm::vec4(caster.WorldBounds.Min - renderOrigin, 0.0f);
                entry.BoundsMax = glm::vec4(caster.WorldBounds.Max - renderOrigin, 0.0f);
            }
            entry.Batch = glm::uvec4(batchIndex, it->RunBase, it->CasterCount * VSM::kClipLevels,
                                     unbounded ? 1u : 0u);
            m_CullInput.push_back(entry);
        }

        if (m_CullInput.size() > VSM::kMaxCasters)
            m_CullInput.resize(VSM::kMaxCasters);

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

        // Restore. The physical pool is read by the lit pass as a texture, so the
        // image writes above must be visible to a texture fetch, not just to
        // another image access.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch);

        RenderCommand::SetColorMask(true, true, true, true);
        RenderCommand::SetDepthTest(true);
        RenderCommand::SetDepthMask(true);
        RenderCommand::BackCull();
        RenderCommand::BindDefaultFramebuffer();
        RenderCommand::SetViewport(previousViewport.x, previousViewport.y,
                                   previousViewport.width, previousViewport.height);

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

    // -------------------------------------------------------------------------
    // Step 8 — end of the shadow pass
    // -------------------------------------------------------------------------

    void VirtualShadowMap::EndFrame()
    {
        OLO_PROFILE_FUNCTION();

        if (!IsActive())
            return;

        BindWorkingSet();
        DispatchKernel(m_EndFrameShader, VSM::kTotalVirtualPages, 64);

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
        m_GlobalsUBO->SetData(&globals, VSM::GlobalsUBO::GetSize());
        m_GlobalsUBO->Bind();

        m_PageTable->Bind();

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
        VSM::Statistics readback{};
        source->GetData(&readback, sizeof(VSM::Statistics));
        m_Statistics = readback;
    }
} // namespace OloEngine

#include "OloEnginePCH.h"
#include "OloEngine/Renderer/CloudShadowMap.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/RenderCommand.h"

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    CloudShadowMap::CloudShadowMapData CloudShadowMap::s_Data;

    namespace
    {
        // Must match local_size_x/y in CloudShadow_Generate.comp
        constexpr u32 kLocalSize = 8;
    } // namespace

    void CloudShadowMap::Update(const CloudscapeRenderState& state, const glm::vec3& cameraPosAbsolute)
    {
        OLO_PROFILE_FUNCTION();

        if (s_Data.m_CreationFailed)
        {
            return; // already logged; Shutdown() clears the latch
        }

        // Lazy-create GPU resources on the first call (raw-id handling
        // mirrors SSAORenderPass::CreateNoiseTexture; the render pipeline
        // owns the call site so a live GL context is guaranteed).
        if (!s_Data.m_Texture.IsValid() || !s_Data.m_GenerateShader)
        {
            if (!s_Data.m_Texture.IsValid())
            {
                s_Data.m_Texture = RenderCommand::CreateTexture2DHandle(kShadowResolution, kShadowResolution, RHI::Format::R8UNorm);
                if (s_Data.m_Texture.IsValid())
                {
                    RenderCommand::SetTextureFilter(s_Data.m_Texture, RHI::Filter::Linear, RHI::Filter::Linear);
                    RenderCommand::SetTextureWrap(s_Data.m_Texture, RHI::AddressMode::ClampToEdge);
                }
            }
            if (!s_Data.m_GenerateShader)
            {
                s_Data.m_GenerateShader = ComputeShader::Create("assets/shaders/compute/CloudShadow_Generate.comp");
            }

            const bool textureValid = s_Data.m_Texture.IsValid();
            const bool shaderValid = s_Data.m_GenerateShader && s_Data.m_GenerateShader->IsValid();
            if (!textureValid || !shaderValid)
            {
                OLO_CORE_ERROR("CloudShadowMap::Update failed — {}",
                               !shaderValid ? "CloudShadow_Generate.comp could not be loaded/compiled"
                                            : "R8 shadow texture could not be created");
                if (s_Data.m_Texture.IsValid())
                {
                    RenderCommand::DeleteTexture(s_Data.m_Texture);
                    s_Data.m_Texture = {};
                }
                s_Data.m_GenerateShader = nullptr;
                s_Data.m_CreationFailed = true;
                return;
            }
        }

        // Texel-snap the map center to the camera so panning re-samples the
        // exact same world positions instead of swimming the shadow pattern.
        const f32 worldSize = std::max(state.ShadowWorldSize, 1.0f);
        const f32 texelSize = worldSize / static_cast<f32>(kShadowResolution);
        const glm::vec2 center(std::floor(cameraPosAbsolute.x / texelSize) * texelSize,
                               std::floor(cameraPosAbsolute.z / texelSize) * texelSize);

        // NOTE: the compute also reads the CloudscapeData UBO (binding 52)
        // and the noise samplers (59/60/61) — the caller uploaded/bound those
        // BEFORE this call (see the class comment in CloudShadowMap.h).
        s_Data.m_GenerateShader->Bind();
        s_Data.m_GenerateShader->SetFloat2("u_ShadowCenter", center);
        s_Data.m_GenerateShader->SetFloat("u_ShadowWorldSize", worldSize);
        s_Data.m_GenerateShader->SetInt("u_ShadowResolution", static_cast<int>(kShadowResolution));

        // Persistent: the shadow map is owned by this system for the process's
        // life, not by the frame graph, so its descriptor is memoised once rather
        // than re-minted from the transient ring every frame.
        HeapBinding::BindImageOrOffset(0, s_Data.m_Texture, 0, false, 0, RHI::Access::StorageWrite,
                                       RHI::Format::R8UNorm, RHI::HeapSlotLifetime::Persistent);
        HeapBinding::FlushOffsets();
        constexpr u32 kGroups = (kShadowResolution + kLocalSize - 1) / kLocalSize;
        RenderCommand::DispatchCompute(kGroups, kGroups, 1);

        // The PBR surface shaders sample the map as a texture; image stores
        // must land first.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureFetch);
        s_Data.m_GenerateShader->Unbind();

        s_Data.m_Center = center;
        s_Data.m_WorldSize = worldSize;
        s_Data.m_Ready = true;
    }

    void CloudShadowMap::Shutdown()
    {
        OLO_PROFILE_FUNCTION();

        const bool hadState = s_Data.m_Texture.IsValid() || s_Data.m_GenerateShader || s_Data.m_CreationFailed;

        if (s_Data.m_Texture.IsValid())
        {
            // The shadow map is bound through the PBR mesh dispatch's TRACKED
            // path (CommandDispatch::SetCloudShadowTextureID), so drop any
            // cached "slot already has this texture" entry before the ID is
            // deleted — a future bind with a recycled GL ID must not be
            // skipped against stale tracking (the same contract the
            // OpenGLTexture2D destructor honors).
            CommandDispatch::InvalidateTextureBinding(s_Data.m_Texture);
            RenderCommand::DeleteTexture(s_Data.m_Texture);
        }
        s_Data.m_GenerateShader = nullptr;
        s_Data.m_Texture = {};
        s_Data.m_Center = glm::vec2(0.0f, 0.0f);
        s_Data.m_WorldSize = 0.0f;
        s_Data.m_Ready = false;
        s_Data.m_CreationFailed = false;

        if (hadState)
        {
            OLO_CORE_INFO("CloudShadowMap shut down");
        }
    }

    bool CloudShadowMap::IsReady()
    {
        return s_Data.m_Ready;
    }

    RHI::ResourceHandle CloudShadowMap::GetTextureHandle()
    {
        return s_Data.m_Ready ? s_Data.m_Texture : RHI::NullResource;
    }

    glm::vec2 CloudShadowMap::GetCenter()
    {
        return s_Data.m_Center;
    }

    f32 CloudShadowMap::GetWorldSize()
    {
        return s_Data.m_WorldSize;
    }
} // namespace OloEngine

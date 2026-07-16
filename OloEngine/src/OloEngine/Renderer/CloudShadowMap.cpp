#include "OloEnginePCH.h"
#include "OloEngine/Renderer/CloudShadowMap.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/RenderCommand.h"

#include <glad/gl.h>

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
        if (s_Data.m_TextureID == 0 || !s_Data.m_GenerateShader)
        {
            if (s_Data.m_TextureID == 0)
            {
                s_Data.m_TextureID = RenderCommand::CreateTexture2D(kShadowResolution, kShadowResolution, GL_R8);
                if (s_Data.m_TextureID != 0)
                {
                    RenderCommand::SetTextureParameter(s_Data.m_TextureID, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    RenderCommand::SetTextureParameter(s_Data.m_TextureID, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    RenderCommand::SetTextureParameter(s_Data.m_TextureID, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    RenderCommand::SetTextureParameter(s_Data.m_TextureID, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                }
            }
            if (!s_Data.m_GenerateShader)
            {
                s_Data.m_GenerateShader = ComputeShader::Create("assets/shaders/compute/CloudShadow_Generate.comp");
            }

            const bool textureValid = s_Data.m_TextureID != 0;
            const bool shaderValid = s_Data.m_GenerateShader && s_Data.m_GenerateShader->IsValid();
            if (!textureValid || !shaderValid)
            {
                OLO_CORE_ERROR("CloudShadowMap::Update failed — {}",
                               !shaderValid ? "CloudShadow_Generate.comp could not be loaded/compiled"
                                            : "R8 shadow texture could not be created");
                if (s_Data.m_TextureID != 0)
                {
                    RenderCommand::DeleteTexture(s_Data.m_TextureID);
                    s_Data.m_TextureID = 0;
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
        // and the noise samplers (56/57/58) — the caller uploaded/bound those
        // BEFORE this call (see the class comment in CloudShadowMap.h).
        s_Data.m_GenerateShader->Bind();
        s_Data.m_GenerateShader->SetFloat2("u_ShadowCenter", center);
        s_Data.m_GenerateShader->SetFloat("u_ShadowWorldSize", worldSize);
        s_Data.m_GenerateShader->SetInt("u_ShadowResolution", static_cast<int>(kShadowResolution));

        RenderCommand::BindImageTexture(0, s_Data.m_TextureID, 0, false, 0, GL_WRITE_ONLY, GL_R8);
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

        const bool hadState = s_Data.m_TextureID != 0 || s_Data.m_GenerateShader || s_Data.m_CreationFailed;

        if (s_Data.m_TextureID != 0)
        {
            RenderCommand::DeleteTexture(s_Data.m_TextureID);
        }
        s_Data.m_GenerateShader = nullptr;
        s_Data.m_TextureID = 0;
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

    u32 CloudShadowMap::GetTextureID()
    {
        return s_Data.m_Ready ? s_Data.m_TextureID : 0;
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

#include "OloEnginePCH.h"
#include "OloEngine/Renderer/LightCulling/LightCullingBuffer.h"

namespace OloEngine
{
    void LightCullingBuffer::Initialize(u32 maxPointLights, u32 maxSpotLights)
    {
        OLO_PROFILE_FUNCTION();

        m_MaxPointLights = maxPointLights;
        m_MaxSpotLights = maxSpotLights;

        m_PointLightSSBO = StorageBuffer::Create(
            maxPointLights * sizeof(GPUPointLight),
            ShaderBindingLayout::SSBO_FPLUS_POINT_LIGHTS,
            StorageBufferUsage::DynamicDraw);

        m_SpotLightSSBO = StorageBuffer::Create(
            maxSpotLights * sizeof(GPUSpotLight),
            ShaderBindingLayout::SSBO_FPLUS_SPOT_LIGHTS,
            StorageBufferUsage::DynamicDraw);

        m_Initialized = true;

        OLO_CORE_INFO("LightCullingBuffer: Initialized for {} point + {} spot lights",
                      maxPointLights, maxSpotLights);
    }

    void LightCullingBuffer::Update(const std::vector<GPUPointLight>& pointLights,
                                    const std::vector<GPUSpotLight>& spotLights)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Initialized)
        {
            return;
        }

        m_PointLightCount = static_cast<u32>(pointLights.size());
        m_SpotLightCount = static_cast<u32>(spotLights.size());

        if (m_PointLightCount > 0 && m_PointLightSSBO)
        {
            // Resize if needed
            const u32 requiredSize = m_PointLightCount * sizeof(GPUPointLight);
            if (requiredSize > m_PointLightSSBO->GetSize())
            {
                m_PointLightSSBO->Resize(requiredSize);
            }
            m_PointLightSSBO->SetData(pointLights.data(), requiredSize);
        }

        if (m_SpotLightCount > 0 && m_SpotLightSSBO)
        {
            const u32 requiredSize = m_SpotLightCount * sizeof(GPUSpotLight);
            if (requiredSize > m_SpotLightSSBO->GetSize())
            {
                m_SpotLightSSBO->Resize(requiredSize);
            }
            m_SpotLightSSBO->SetData(spotLights.data(), requiredSize);
        }
    }

    void LightCullingBuffer::Bind() const
    {
        if (m_PointLightSSBO)
            m_PointLightSSBO->Bind();
        if (m_SpotLightSSBO)
            m_SpotLightSSBO->Bind();
    }

    void LightCullingBuffer::Unbind() const
    {
        if (m_PointLightSSBO)
            m_PointLightSSBO->Unbind();
        if (m_SpotLightSSBO)
            m_SpotLightSSBO->Unbind();
    }
} // namespace OloEngine

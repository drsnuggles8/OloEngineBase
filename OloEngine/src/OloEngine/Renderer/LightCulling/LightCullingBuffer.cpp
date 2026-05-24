#include "OloEnginePCH.h"
#include "OloEngine/Renderer/LightCulling/LightCullingBuffer.h"

namespace OloEngine
{
    void LightCullingBuffer::Initialize(u32 maxPointLights, u32 maxSpotLights, u32 maxSphereAreaLights)
    {
        OLO_PROFILE_FUNCTION();

        m_MaxPointLights = maxPointLights;
        m_MaxSpotLights = maxSpotLights;
        m_MaxSphereAreaLights = maxSphereAreaLights;

        m_PointLightSSBO = StorageBuffer::Create(
            maxPointLights * sizeof(GPUPointLight),
            ShaderBindingLayout::SSBO_FPLUS_POINT_LIGHTS,
            StorageBufferUsage::DynamicDraw);

        m_SpotLightSSBO = StorageBuffer::Create(
            maxSpotLights * sizeof(GPUSpotLight),
            ShaderBindingLayout::SSBO_FPLUS_SPOT_LIGHTS,
            StorageBufferUsage::DynamicDraw);

        m_SphereAreaLightSSBO = StorageBuffer::Create(
            maxSphereAreaLights * sizeof(GPUSphereAreaLight),
            ShaderBindingLayout::SSBO_FPLUS_SPHERE_AREA_LIGHTS,
            StorageBufferUsage::DynamicDraw);

        if (!m_PointLightSSBO || !m_SpotLightSSBO || !m_SphereAreaLightSSBO)
        {
            OLO_CORE_ERROR("LightCullingBuffer: Failed to create one or more SSBOs");
            m_PointLightSSBO.Reset();
            m_SpotLightSSBO.Reset();
            m_SphereAreaLightSSBO.Reset();
            m_Initialized = false;
            return;
        }

        m_Initialized = true;

        OLO_CORE_INFO("LightCullingBuffer: Initialized for {} point + {} spot + {} sphere-area lights",
                      maxPointLights, maxSpotLights, maxSphereAreaLights);
    }

    void LightCullingBuffer::Shutdown()
    {
        m_PointLightSSBO.Reset();
        m_SpotLightSSBO.Reset();
        m_SphereAreaLightSSBO.Reset();
        m_PointLightCount = 0;
        m_SpotLightCount = 0;
        m_SphereAreaLightCount = 0;
        m_Initialized = false;
    }

    void LightCullingBuffer::Update(const std::vector<GPUPointLight>& pointLights,
                                    const std::vector<GPUSpotLight>& spotLights,
                                    const std::vector<GPUSphereAreaLight>& sphereAreaLights)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Initialized)
        {
            return;
        }

        m_PointLightCount = static_cast<u32>(pointLights.size());
        m_SpotLightCount = static_cast<u32>(spotLights.size());
        m_SphereAreaLightCount = static_cast<u32>(sphereAreaLights.size());

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

        if (m_SphereAreaLightCount > 0 && m_SphereAreaLightSSBO)
        {
            const u32 requiredSize = m_SphereAreaLightCount * sizeof(GPUSphereAreaLight);
            if (requiredSize > m_SphereAreaLightSSBO->GetSize())
            {
                m_SphereAreaLightSSBO->Resize(requiredSize);
            }
            m_SphereAreaLightSSBO->SetData(sphereAreaLights.data(), requiredSize);
        }
    }

    void LightCullingBuffer::Bind() const
    {
        if (m_PointLightSSBO)
            m_PointLightSSBO->Bind();
        if (m_SpotLightSSBO)
            m_SpotLightSSBO->Bind();
        if (m_SphereAreaLightSSBO)
            m_SphereAreaLightSSBO->Bind();
    }

    void LightCullingBuffer::Unbind() const
    {
        if (m_PointLightSSBO)
            m_PointLightSSBO->Unbind();
        if (m_SpotLightSSBO)
            m_SpotLightSSBO->Unbind();
        if (m_SphereAreaLightSSBO)
            m_SphereAreaLightSSBO->Unbind();
    }
} // namespace OloEngine

#include "OloEnginePCH.h"
#include "OloEngine/Renderer/LightBuffer.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

namespace OloEngine
{
    LightBuffer::LightBuffer()
    {
        // Create UBO for light data
        m_UBO = UniformBuffer::Create(LightBufferUBO::GetSize(), ShaderBindingLayout::UBO_LIGHTS);
        Clear();
    }

    bool LightBuffer::AddLight(const Light& light)
    {
        if (IsFull())
        {
            OLO_CORE_WARN("LightBuffer::AddLight: Light buffer is full, cannot add more lights");
            return false;
        }

        ConvertLightToData(light, m_BufferData.Lights[m_LightCount]);
        m_LightCount++;
        m_BufferData.LightCount = static_cast<i32>(m_LightCount);

        return true;
    }

    void LightBuffer::RemoveLight(u32 index)
    {
        if (index >= m_LightCount)
        {
            OLO_CORE_WARN("LightBuffer::RemoveLight: Invalid light index {}", index);
            return;
        }

        // Shift lights down to fill the gap
        for (u32 i = index; i < m_LightCount - 1; ++i)
        {
            m_BufferData.Lights[i] = m_BufferData.Lights[i + 1];
        }

        m_LightCount--;
        m_BufferData.LightCount = static_cast<i32>(m_LightCount);
    }

    void LightBuffer::Clear()
    {
        m_LightCount = 0;
        m_BufferData.LightCount = 0;

        // Clear all light data
        memset(m_BufferData.Lights, 0, sizeof(m_BufferData.Lights));
    }

    void LightBuffer::UpdateLight(u32 index, const Light& light)
    {
        if (index >= m_LightCount)
        {
            OLO_CORE_WARN("LightBuffer::UpdateLight: Invalid light index {}", index);
            return;
        }

        ConvertLightToData(light, m_BufferData.Lights[index]);
    }

    void LightBuffer::UploadToGPU()
    {
        if (m_UBO)
        {
            m_UBO->SetData(&m_BufferData, LightBufferUBO::GetSize());
        }
    }

    void LightBuffer::Bind()
    {
        if (m_UBO)
        {
            // UBO is automatically bound to the correct binding point during creation
            // Just ensure data is up to date
            UploadToGPU();
        }
    }

    const LightBuffer::LightData& LightBuffer::GetLightData(u32 index) const
    {
        if (index >= m_LightCount)
        {
            OLO_CORE_ERROR("LightBuffer::GetLightData: Invalid light index {}", index);
            static LightData dummy = {};
            return dummy;
        }

        return m_BufferData.Lights[index];
    }

    void LightBuffer::ConvertLightToData(const Light& light, LightData& data)
    {
        // Convert light type and position
        switch (light.Type)
        {
            case LightType::Directional:
                data.Position = glm::vec4(light.Direction, 0.0f); // w=0 for directional
                data.Direction = glm::vec4(light.Direction, 0.0f);
                data.AttenuationParams = glm::vec4(1.0f, 0.0f, 0.0f, 0.0f); // No attenuation for directional
                data.SpotParams = glm::vec4(0.0f, 0.0f, 0.0f, static_cast<f32>(ShaderConstants::DIRECTIONAL_LIGHT));
                break;

            case LightType::Point:
                data.Position = glm::vec4(light.Position, 1.0f); // w=1 for point
                data.Direction = glm::vec4(0.0f, -1.0f, 0.0f, 0.0f);
                data.AttenuationParams = glm::vec4(light.Constant, light.Linear, light.Quadratic, light.Range);
                data.SpotParams = glm::vec4(0.0f, 0.0f, 0.0f, static_cast<f32>(ShaderConstants::POINT_LIGHT));
                break;

            case LightType::Spot:
                data.Position = glm::vec4(light.Position, 1.0f); // w=1 for spot
                data.Direction = glm::vec4(light.Direction, 0.0f);
                data.AttenuationParams = glm::vec4(light.Constant, light.Linear, light.Quadratic, light.Range);
                data.SpotParams = glm::vec4(light.CutOff, light.OuterCutOff, light.Falloff, static_cast<f32>(ShaderConstants::SPOT_LIGHT));
                break;

            default:
                OLO_CORE_WARN("LightBuffer::ConvertLightToData: Unknown light type");
                break;
        }

        // Set color and intensity
        data.Color = glm::vec4(light.Color, light.Intensity);
    }

    // MultiLightRenderer implementation
    MultiLightRenderer::MultiLightRenderer()
    {
    }

    void MultiLightRenderer::Initialize()
    {
        if (m_Initialized)
        {
            OLO_CORE_WARN("MultiLightRenderer::Initialize: Already initialized");
            return;
        }

        // Light buffer is initialized in its constructor
        m_Initialized = true;
        OLO_CORE_INFO("MultiLightRenderer initialized with support for {} lights", ShaderConstants::MAX_LIGHTS);
    }

    i32 MultiLightRenderer::AddLight(const Light& light)
    {
        if (!m_Initialized)
        {
            OLO_CORE_ERROR("MultiLightRenderer::AddLight: Renderer not initialized");
            return -1;
        }

        u32 oldCount = m_LightBuffer.GetLightCount();
        if (m_LightBuffer.AddLight(light))
        {
            return static_cast<i32>(oldCount);
        }

        return -1;
    }

    void MultiLightRenderer::RemoveLight(u32 index)
    {
        if (!m_Initialized)
        {
            OLO_CORE_ERROR("MultiLightRenderer::RemoveLight: Renderer not initialized");
            return;
        }

        m_LightBuffer.RemoveLight(index);
    }

    void MultiLightRenderer::UpdateLight(u32 index, const Light& light)
    {
        if (!m_Initialized)
        {
            OLO_CORE_ERROR("MultiLightRenderer::UpdateLight: Renderer not initialized");
            return;
        }

        m_LightBuffer.UpdateLight(index, light);
    }

    void MultiLightRenderer::ClearLights()
    {
        if (!m_Initialized)
        {
            OLO_CORE_ERROR("MultiLightRenderer::ClearLights: Renderer not initialized");
            return;
        }

        m_LightBuffer.Clear();
    }

    void MultiLightRenderer::BeginRender()
    {
        if (!m_Initialized)
        {
            OLO_CORE_ERROR("MultiLightRenderer::BeginRender: Renderer not initialized");
            return;
        }

        // Bind light buffer
        m_LightBuffer.Bind();
    }

    void MultiLightRenderer::EndRender()
    {
        if (!m_Initialized)
        {
            OLO_CORE_ERROR("MultiLightRenderer::EndRender: Renderer not initialized");
            return;
        }

        // Upload any pending changes
        m_LightBuffer.UploadToGPU();
    }
} // namespace OloEngine

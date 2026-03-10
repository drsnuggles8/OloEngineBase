#include "OloEnginePCH.h"
#include "OcclusionCuller.h"
#include "OcclusionQueryPool.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"

#include <glm/gtc/matrix_transform.hpp>

namespace OloEngine
{
    OcclusionCuller& OcclusionCuller::GetInstance()
    {
        static OcclusionCuller instance;
        return instance;
    }

    void OcclusionCuller::Initialize()
    {
        OLO_PROFILE_FUNCTION();
        if (m_Initialized)
            return;

        m_ProxyCube = MeshPrimitives::CreateCube();
        if (!m_ProxyCube)
        {
            OLO_CORE_ERROR("OcclusionCuller: Failed to create proxy cube mesh");
            return;
        }

        m_Initialized = true;
        OLO_CORE_INFO("OcclusionCuller: Initialized with proxy cube mesh");
    }

    void OcclusionCuller::Shutdown()
    {
        OLO_PROFILE_FUNCTION();
        m_ProxyCube.Reset();
        m_PendingQueries.clear();
        m_Initialized = false;
    }

    void OcclusionCuller::QueueBoundingBox(u32 queryIndex, const BoundingBox& worldBounds)
    {
        OLO_PROFILE_FUNCTION();
        m_PendingQueries.push_back({ queryIndex, worldBounds });
    }

    void OcclusionCuller::FlushQueuedQueries()
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Initialized || !m_ProxyCube || m_PendingQueries.empty())
            return;

        auto& queryPool = OcclusionQueryPool::GetInstance();
        if (!queryPool.IsActive())
        {
            m_PendingQueries.clear();
            return;
        }

        auto& api = RenderCommand::GetRendererAPI();

        // Set up deterministic render state for query pass
        api.SetColorMask(false, false, false, false);
        api.SetDepthMask(false);
        api.SetDepthTest(true);
        api.SetDepthFunc(GL_LEQUAL);
        api.SetBlendState(false);
        api.DisableCulling();
        api.DisableStencilTest();
        api.DisableScissorTest();

        // Bind the occlusion proxy shader
        auto proxyShader = Renderer3D::GetShaderLibrary().Get("OcclusionProxy");
        if (!proxyShader)
        {
            OLO_CORE_WARN("OcclusionCuller: OcclusionProxy shader not found, skipping queries");
            m_PendingQueries.clear();
            return;
        }

        auto modelUBO = Renderer3D::GetModelMatrixUBO();
        if (!modelUBO)
        {
            OLO_CORE_WARN("OcclusionCuller: Model UBO not available, skipping queries");
            m_PendingQueries.clear();
            return;
        }

        proxyShader->Bind();

        for (const auto& pending : m_PendingQueries)
        {
            // Compute model matrix to scale/translate unit cube [-0.5,0.5]^3 to match AABB
            glm::vec3 center = (pending.Bounds.Min + pending.Bounds.Max) * 0.5f;
            glm::vec3 extent = pending.Bounds.Max - pending.Bounds.Min;

            glm::mat4 model = glm::translate(glm::mat4(1.0f), center);
            model = glm::scale(model, extent);

            ShaderBindingLayout::ModelUBO modelData{};
            modelData.Model = model;
            modelData.Normal = glm::transpose(glm::inverse(model));
            modelData.EntityID = -1;
            modelUBO->SetData(&modelData, ShaderBindingLayout::ModelUBO::GetSize());
            modelUBO->Bind();
            CommandDispatch::InvalidateUBOCache(ShaderBindingLayout::UBO_MODEL);

            // Issue the query: draw the box between Begin/EndQuery
            queryPool.BeginQuery(pending.QueryIndex);
            RenderCommand::DrawIndexedRaw(
                m_ProxyCube->GetVertexArray()->GetRendererID(),
                m_ProxyCube->GetIndexCount());
            queryPool.EndQuery(pending.QueryIndex);
        }

        // Restore default state
        api.SetColorMask(true, true, true, true);
        api.SetDepthMask(true);
        api.SetDepthFunc(GL_LESS);

        m_PendingQueries.clear();
    }
} // namespace OloEngine

#include "OloEnginePCH.h"
#include "OcclusionCuller.h"
#include "OcclusionQueryPool.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/Instancing/InstanceBuffer.h"
#include "OloEngine/Renderer/Instancing/InstanceData.h"
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

        auto instanceBuffer = Renderer3D::GetModelInstanceBuffer();
        if (!instanceBuffer)
        {
            OLO_CORE_WARN("OcclusionCuller: ModelInstanceBuffer not available, skipping queries");
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

            // Camera-relative (issue #429): the proxy is drawn with the shared
            // (now relative) camera UBO against the relative-rendered scene depth
            // buffer, so its world model matrix must be shifted by the render
            // origin or the query tests occlusion at the wrong depth far from
            // origin. No-op at the origin.
            model = MakeModelRelative(model, Renderer3D::GetRenderOrigin());

            // OcclusionProxy shader reads u_Model from the InstanceBuffer SSBO
            // at binding 15 via InstanceBlock_Vertex.glsl. Push a single
            // instance per query; proxies have no motion history so
            // PrevTransform aliases Transform (zero velocity).
            InstanceData inst;
            inst.Transform = model;
            inst.Normal = glm::transpose(glm::inverse(model));
            inst.PrevTransform = model;
            inst.EntityID = -1;
            const std::span<const InstanceData> oneInstance(&inst, 1);
            instanceBuffer->Upload(oneInstance);
            instanceBuffer->Bind();

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

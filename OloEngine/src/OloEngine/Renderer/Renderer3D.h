#pragma once

#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/Camera/PerspectiveCamera.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Light.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/Passes/FinalRenderPass.h"
#include "OloEngine/Core/Timestep.h"

// Forward declarations
namespace OloEngine {
    class Mesh;
    class Texture2D;
    class RenderCommand;
    class UniformBuffer;
    class SceneRenderPass;
    class FinalRenderPass;
    class CommandBucket;
}

namespace OloEngine
{
	class ShaderLibrary;

	class Renderer3D
	{
	public:
		struct Statistics
		{
			u32 TotalMeshes = 0;
			u32 CulledMeshes = 0;
			u32 DrawCalls = 0;
			u32 ShaderBinds = 0;
			u32 TextureBinds = 0;
			
			void Reset() { TotalMeshes = 0; CulledMeshes = 0; DrawCalls = 0; ShaderBinds = 0; TextureBinds = 0; }
		};

	public:
		static void Init();
		static void Shutdown();

		static void BeginScene(const PerspectiveCamera& camera);
		static void EndScene();

		static CommandPacket* DrawMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material, bool isStatic = true);
		static CommandPacket* DrawQuad(const glm::mat4& modelMatrix, const Ref<Texture2D>& texture);
		static CommandPacket* DrawMeshInstanced(const Ref<Mesh>& mesh, const std::vector<glm::mat4>& transforms, const Material& material, bool isStatic = true);
		static CommandPacket* DrawLightCube(const glm::mat4& modelMatrix);
		static CommandPacket* DrawCube(const glm::mat4& modelMatrix, const Material& material, bool isStatic = true);
	
		static void SetLight(const Light& light);
		static void SetViewPosition(const glm::vec3& position);
		
		// Culling methods
		static void EnableFrustumCulling(bool enable);
		static bool IsFrustumCullingEnabled();
		static void EnableDynamicCulling(bool enable);
		static bool IsDynamicCullingEnabled();
		static const Frustum& GetViewFrustum();
		static bool IsVisibleInFrustum(const Ref<Mesh>& mesh, const glm::mat4& transform);
		static bool IsVisibleInFrustum(const BoundingSphere& sphere);
		static bool IsVisibleInFrustum(const BoundingBox& box);
		
		// Statistics and debug methods
		static Statistics GetStats();
		static void ResetStats();
		
		// Debug access to command bucket for debugging tools
		static const CommandBucket* GetCommandBucket() { return s_Data.ScenePass ? &s_Data.ScenePass->GetCommandBucket() : nullptr; }
		
		// Window resize handling
		static void OnWindowResize(u32 width, u32 height);
		static const Ref<RenderGraph>& GetRenderGraph() { return s_Data.RGraph; }

		template<typename T>
		static CommandPacket* CreateDrawCall()
		{
			OLO_PROFILE_FUNCTION();
			return s_Data.ScenePass->GetCommandBucket().CreateDrawCall<T>();
		}

		static void SubmitPacket(CommandPacket* packet)
		{
			OLO_PROFILE_FUNCTION();
			if (!packet)
			{
				OLO_CORE_WARN("Renderer3D::SubmitPacket: Attempted to submit a null CommandPacket pointer!");
				return;
			}
			s_Data.ScenePass->SubmitPacket(packet);
		}

	private:
		static void UpdateCameraMatricesUBO(const glm::mat4& view, const glm::mat4& projection);
		static void SetupRenderGraph(u32 width, u32 height);

	private:
		struct Renderer3DData
		{
			Ref<Mesh> CubeMesh;
			Ref<Mesh> QuadMesh;
			Ref<Shader> LightCubeShader;
			Ref<Shader> LightingShader;
			Ref<Shader> QuadShader;
			
			Ref<UniformBuffer> TransformUBO;
			Ref<UniformBuffer> MaterialUBO;
			Ref<UniformBuffer> TextureFlagUBO;
			Ref<UniformBuffer> CameraMatricesBuffer;
			Ref<UniformBuffer> LightPropertiesUBO;

			glm::mat4 ViewProjectionMatrix = glm::mat4(1.0f);
			glm::mat4 ViewMatrix = glm::mat4(1.0f);
			glm::mat4 ProjectionMatrix = glm::mat4(1.0f);

			Frustum ViewFrustum;
			bool FrustumCullingEnabled = true;
			bool DynamicCullingEnabled = true;

			Light SceneLight;
			glm::vec3 ViewPos;
			
			Statistics Stats;
			u32 CommandCounter = 0;
			
			Ref<RenderGraph> RGraph;
			Ref<SceneRenderPass> ScenePass;
            Ref<FinalRenderPass> FinalPass;
		};

		static Renderer3DData s_Data;
		static ShaderLibrary m_ShaderLibrary;
	};
}
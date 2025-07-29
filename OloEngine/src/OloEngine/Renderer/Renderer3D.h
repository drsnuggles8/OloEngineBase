#pragma once

#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/Camera/PerspectiveCamera.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/SkinnedMesh.h"
#include "OloEngine/Renderer/Light.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/Passes/FinalRenderPass.h"
#include "OloEngine/Core/Timestep.h"
#include "OloEngine/Renderer/ShaderResourceRegistry.h"

// Forward declarations
namespace OloEngine {
    class Texture2D;
    class RenderCommand;
    class UniformBuffer;
    class CommandBucket;
    class Scene;
    class Entity;
}

namespace OloEngine
{
    /**
     * @brief High-level 3D rendering API with scene and material management
     * 
     * @warning Thread Safety: This class is NOT thread-safe. All methods should be called
     * from the main rendering thread only. The static data members (s_Data, m_ShaderLibrary)
     * are accessed without synchronization and concurrent access will lead to undefined behavior.
     * If multi-threaded rendering is required, external synchronization must be provided.
     */
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
			u32 TotalAnimatedMeshes = 0;
			u32 RenderedAnimatedMeshes = 0;
			u32 SkippedAnimatedMeshes = 0;
			
			void Reset() 
			{ 
				TotalMeshes = 0; 
				CulledMeshes = 0; 
				DrawCalls = 0; 
				ShaderBinds = 0; 
				TextureBinds = 0; 
				TotalAnimatedMeshes = 0; 
				RenderedAnimatedMeshes = 0; 
				SkippedAnimatedMeshes = 0; 
			}
		};

	public:
		static void Init();
		static void Shutdown();

		static void BeginScene(const PerspectiveCamera& camera);
		static void EndScene();
		static CommandPacket* DrawMesh(const AssetRef<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material, bool isStatic = true);
		static CommandPacket* DrawSkinnedMesh(const Ref<SkinnedMesh>& mesh, const glm::mat4& modelMatrix, const Material& material, const std::vector<glm::mat4>& boneMatrices, bool isStatic = true);
		static CommandPacket* DrawQuad(const glm::mat4& modelMatrix, const Ref<Texture2D>& texture);
		static CommandPacket* DrawMeshInstanced(const AssetRef<Mesh>& mesh, const std::vector<glm::mat4>& transforms, const Material& material, bool isStatic = true);
		static CommandPacket* DrawLightCube(const glm::mat4& modelMatrix);
		static CommandPacket* DrawCube(const glm::mat4& modelMatrix, const Material& material, bool isStatic = true);
		static CommandPacket* DrawSkybox(const Ref<TextureCubemap>& skyboxTexture);
		
		// Skeleton visualization
		static void DrawSkeleton(const Skeleton& skeleton, const glm::mat4& modelMatrix, 
								 bool showBones = true, bool showJoints = true, 
								 f32 jointSize = 0.02f, f32 boneThickness = 2.0f);
		static CommandPacket* DrawLine(const glm::vec3& start, const glm::vec3& end, 
									   const glm::vec3& color = glm::vec3(1.0f), f32 thickness = 1.0f);
		static CommandPacket* DrawSphere(const glm::vec3& position, f32 radius, 
										  const glm::vec3& color = glm::vec3(1.0f));
		
		// ECS Animated Mesh Rendering
		static void RenderAnimatedMeshes(const Ref<Scene>& scene, const Material& defaultMaterial);
		static void RenderAnimatedMesh(Entity entity, const Material& defaultMaterial);
	
		static void SetLight(const Light& light);
		static void SetViewPosition(const glm::vec3& position);
		
		// Culling methods
		static void EnableFrustumCulling(bool enable);
		static bool IsFrustumCullingEnabled();		static void EnableDynamicCulling(bool enable);
		static bool IsDynamicCullingEnabled();
		static const Frustum& GetViewFrustum();
		static bool IsVisibleInFrustum(const AssetRef<Mesh>& mesh, const glm::mat4& transform);
		static bool IsVisibleInFrustum(const Ref<SkinnedMesh>& mesh, const glm::mat4& transform);
		static bool IsVisibleInFrustum(const BoundingSphere& sphere);
		static bool IsVisibleInFrustum(const BoundingBox& box);
		
		// Debug culling methods
		static void SetForceDisableCulling(bool disable);
		static bool IsForceDisableCulling();
		
		// Statistics and debug methods
		static Statistics GetStats();
		static void ResetStats();
		
		// Global resource management for scene-wide resources
		static ShaderResourceRegistry& GetGlobalResourceRegistry() { return s_Data.GlobalResourceRegistry; }
		template<typename T>
		static bool SetGlobalResource(const std::string& name, const Ref<T>& resource)
		{
			return s_Data.GlobalResourceRegistry.SetResource(name, resource);
		}
		static void ApplyGlobalResources();
		
		// Shader registry management
		static ShaderResourceRegistry* GetShaderRegistry(u32 shaderID);
		static void RegisterShaderRegistry(u32 shaderID, ShaderResourceRegistry* registry);
		static void UnregisterShaderRegistry(u32 shaderID);
		static const std::unordered_map<u32, ShaderResourceRegistry*>& GetShaderRegistries();
		
		// High-level resource setting methods
		template<typename T>
		static bool SetShaderResource(u32 shaderID, const std::string& name, const Ref<T>& resource)
		{
			auto* registry = GetShaderRegistry(shaderID);
			if (registry)
			{
				return registry->SetResource(name, resource);
			}
			return false;
		}
		static void ApplyResourceBindings(u32 shaderID);
		
		// Debug access to command bucket for debugging tools
		static const CommandBucket* GetCommandBucket() { return s_Data.ScenePass ? &s_Data.ScenePass->GetCommandBucket() : nullptr; }
		
		// Window resize handling
		static void OnWindowResize(u32 width, u32 height);
		static const Ref<RenderGraph>& GetRenderGraph() { return s_Data.RGraph; }

		// Shader library access for PBR material shader selection
		static ShaderLibrary& GetShaderLibrary();

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
		static void UpdateLightPropertiesUBO();
		static void SetupRenderGraph(u32 width, u32 height);

	private:
		struct Renderer3DData
		{
			AssetRef<Mesh> CubeMesh;
			Ref<Mesh> QuadMesh;
			Ref<Mesh> SkyboxMesh;
			Ref<Shader> LightCubeShader;
			Ref<Shader> LightingShader;
			Ref<Shader> SkinnedLightingShader;
			Ref<Shader> QuadShader;
			Ref<Shader> PBRShader;
			Ref<Shader> PBRSkinnedShader;
			Ref<Shader> PBRMultiLightShader;
			Ref<Shader> PBRMultiLightSkinnedShader;
			Ref<Shader> SkyboxShader;
					Ref<UniformBuffer> CameraUBO;
			Ref<UniformBuffer> MaterialUBO;
			Ref<UniformBuffer> LightPropertiesUBO;
			Ref<UniformBuffer> MultiLightBuffer;
			Ref<UniformBuffer> BoneMatricesUBO;
			Ref<UniformBuffer> ModelMatrixUBO;

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
			
			// Global resource registry for scene-wide resources like environment maps, shadows, etc.
			ShaderResourceRegistry GlobalResourceRegistry;
			
			// Shader registry management
			std::unordered_map<u32, ShaderResourceRegistry*> ShaderRegistries;
			
			Ref<RenderGraph> RGraph;
			Ref<SceneRenderPass> ScenePass;
            Ref<FinalRenderPass> FinalPass;
		};

		static Renderer3DData s_Data;
		static ShaderLibrary m_ShaderLibrary;
	};
}
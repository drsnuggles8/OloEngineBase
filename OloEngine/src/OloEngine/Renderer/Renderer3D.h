#pragma once

#include "OloEngine/Renderer/Camera/Camera.h"
#include "OloEngine/Renderer/Camera/PerspectiveCamera.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Light.h"
#include "OloEngine/Renderer/RenderQueue.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/TextureCubemap.h"

namespace OloEngine
{
	// Forward declarations
	class Mesh;

	class Renderer3D
	{
	public:
		static void Init();
		static void Shutdown();

		static void BeginScene(const PerspectiveCamera& camera);
		static void EndScene();

		// Draw methods
		static void DrawMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material, bool isStatic = false);
		static void DrawCube(const glm::mat4& modelMatrix, const Material& material, bool isStatic = false);
		static void DrawLightCube(const glm::mat4& modelMatrix);
		
		// Simple textured quad rendering
		static void DrawQuad(const glm::mat4& modelMatrix, const Ref<Texture2D>& texture);
		
		// Light and view setters
		static void SetLight(const Light& light);
		static void SetViewPosition(const glm::vec3& position);

		// Frustum culling
		static void EnableFrustumCulling(bool enable);
		static bool IsFrustumCullingEnabled();
		static void EnableDynamicCulling(bool enable);
		static bool IsDynamicCullingEnabled();
		static const Frustum& GetViewFrustum();
		
		// Frustum culling visibility checks
		static bool IsVisibleInFrustum(const BoundingSphere& sphere);
		static bool IsVisibleInFrustum(const BoundingBox& box);
		
		// Statistics
		struct Statistics
		{
			u32 TotalMeshes = 0;
			u32 CulledMeshes = 0;
			
			void Reset() { TotalMeshes = 0; CulledMeshes = 0; }
		};
		
		static Statistics GetStats();
		static void ResetStats();

		// Internal rendering methods (used by RenderQueue)
		static void RenderMeshInternal(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material);
		static void RenderMeshInstanced(const Ref<Mesh>& mesh, const std::vector<glm::mat4>& transforms, const Material& material);
		static void RenderQuadInternal(const glm::mat4& modelMatrix, const Ref<Texture2D>& texture);
		
		// RenderGraph methods
		static void OnWindowResize(u32 width, u32 height);
		static Ref<RenderGraph> GetRenderGraph() { return s_Data.RGraph; }

	private:
		static void UpdateTransformUBO(const glm::mat4& modelMatrix);
		static void UpdateTransformsUBO(const std::vector<glm::mat4>& transforms);
		static void UpdateLightPropertiesUBO(const Material& material);
		static void UpdateTextureFlag(const Material& material);
		static void UpdateCameraMatricesUBO(const glm::mat4& view, const glm::mat4& projection);
		
		// Frustum culling helper
		static bool IsVisibleInFrustum(const Ref<Mesh>& mesh, const glm::mat4& transform);
		
		// Setup the render graph
		static void SetupRenderGraph(u32 width, u32 height);

		static ShaderLibrary m_ShaderLibrary;
		
		struct Renderer3DData
		{
			Ref<Mesh> CubeMesh;
			Ref<Mesh> QuadMesh;
			Ref<Shader> LightCubeShader;
			Ref<Shader> LightingShader;
			Ref<Shader> QuadShader;
			Ref<UniformBuffer> UBO;
			Ref<UniformBuffer> LightPropertiesBuffer;
			Ref<UniformBuffer> TextureFlagBuffer;
			Ref<UniformBuffer> CameraMatricesBuffer;

			glm::mat4 ViewProjectionMatrix;
			glm::mat4 ViewMatrix;
			glm::mat4 ProjectionMatrix;

			Frustum ViewFrustum;
			bool FrustumCullingEnabled = true;
			bool DynamicCullingEnabled = true;

			Light SceneLight;
			glm::vec3 ViewPos;
			
			Statistics Stats;
			
			Ref<RenderGraph> RGraph;
		};
		
		static Renderer3DData s_Data;
	};
}

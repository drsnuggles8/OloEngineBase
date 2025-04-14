#pragma once

#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/Camera/PerspectiveCamera.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Light.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Core/Timestep.h"

// Forward declarations
namespace OloEngine {
    class Mesh;
    class Texture2D;
    class RenderCommand;
    class UniformBuffer;
    class SceneRenderPass;
    class FinalRenderPass;
}

namespace OloEngine
{
	class ShaderLibrary;

	class StatelessRenderer3D
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

		static void DrawCube(const glm::mat4& modelMatrix, const Material& material, bool isStatic = true);
		static void DrawQuad(const glm::mat4& modelMatrix, const Ref<Texture2D>& texture);
		static void DrawMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material, bool isStatic = true);
		static void DrawMeshInstanced(const Ref<Mesh>& mesh, const std::vector<glm::mat4>& transforms, const Material& material, bool isStatic = true);
		static void DrawLightCube(const glm::mat4& modelMatrix);

		// State management functions for compatibility with RenderCommand
		static void SetPolygonMode(unsigned int face, unsigned int mode);
		static void SetLineWidth(float width);
		static void EnableBlending();
		static void DisableBlending();
		static void SetBlendFunc(unsigned int src, unsigned int dest);
		static void SetBlendEquation(unsigned int mode);
		static void SetColorMask(bool red, bool green, bool blue, bool alpha);
		static void SetDepthMask(bool enabled);
		static void EnableDepthTest();
		static void DisableDepthTest();
		
		// Stencil operations
		static void EnableStencilTest();
		static void DisableStencilTest();
		static void SetStencilFunc(unsigned int func, int ref, unsigned int mask);
		static void SetStencilMask(unsigned int mask);
		static void SetStencilOp(unsigned int sfail, unsigned int dpfail, unsigned int dppass);
		static void ClearStencil();
		
		// Culling operations
		static void SetCulling(bool enabled);
		static void SetCullFace(unsigned int face);
		
		// Polygon offset operations
		static void SetPolygonOffset(float factor, float units);

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
		
		// Window resize handling
		static void OnWindowResize(u32 width, u32 height);
		static const Ref<RenderGraph>& GetRenderGraph() { return s_Data.RGraph; }
        
        // Basic rendering methods
        static void SetClearColor(const glm::vec4& color);
        static void Clear();

	private:
		static void UpdateCameraMatricesUBO(const glm::mat4& view, const glm::mat4& projection);
		static void SetupRenderGraph(u32 width, u32 height);

	private:
		struct StatelessRenderer3DData
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

		static StatelessRenderer3DData s_Data;
		static ShaderLibrary m_ShaderLibrary;
	};
}
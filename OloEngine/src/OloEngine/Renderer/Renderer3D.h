#pragma once

#include "OloEngine/Renderer/Camera/Camera.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Light.h"

namespace OloEngine
{
	// Forward declarations
	class Mesh;

	class Renderer3D
	{
	public:
		static void Init();
		static void Shutdown();

		static void BeginScene(const glm::mat4& viewProjectionMatrix);
		static void EndScene();

		// Draw methods
		static void DrawMesh(const Ref<Mesh>& mesh, const glm::mat4& modelMatrix, const Material& material);
		static void DrawCube(const glm::mat4& modelMatrix, const Material& material);
		static void DrawLightCube(const glm::mat4& modelMatrix);
		
		// Simple textured quad rendering
		static void DrawQuad(const glm::mat4& modelMatrix, const Ref<Texture2D>& texture);
		
		// Light and view setters
		static void SetLight(const Light& light);
		static void SetViewPosition(const glm::vec3& position);

	private:
		static void UpdateTransformUBO(const glm::mat4& modelMatrix);
		static void UpdateLightPropertiesUBO(const Material& material);
		static void UpdateTextureFlag(const Material& material);

		static ShaderLibrary m_ShaderLibrary;
	};
}

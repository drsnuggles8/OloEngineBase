#pragma once

#include "OloEngine/Renderer/Camera/Camera.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Renderer/Shader.h"

namespace OloEngine
{
	// Material properties for 3D lighting
	struct Material
	{
		glm::vec3 Ambient = { 0.1f, 0.1f, 0.1f };
		glm::vec3 Diffuse = { 0.7f, 0.7f, 0.7f };
		glm::vec3 Specular = { 0.5f, 0.5f, 0.5f };
		float Shininess = 32.0f;
		
		// Texture maps for PBR-style rendering
		Ref<Texture2D> DiffuseMap = nullptr;
		Ref<Texture2D> SpecularMap = nullptr;
		
		// Controls whether to use texture maps or solid colors
		bool UseTextureMaps = false;
	};

	// Light parameters for Phong lighting model
	struct Light
	{
		glm::vec3 Position = { 1.2f, 1.0f, 2.0f };

		glm::vec3 Ambient = { 0.2f, 0.2f, 0.2f };
		glm::vec3 Diffuse = { 0.5f, 0.5f, 0.5f };
		glm::vec3 Specular = { 1.0f, 1.0f, 1.0f };
	};

	class Renderer3D
	{
	public:
		static void Init();
		static void Shutdown();

		static void BeginScene(const glm::mat4& viewProjectionMatrix);
		static void EndScene();

		static void DrawCube(const glm::mat4& modelMatrix, const Material& material);
		static void DrawLightCube(const glm::mat4& modelMatrix);

		// Light and view setters
		static void SetLight(const Light& light);
		static void SetViewPosition(const glm::vec3& position);

	private:
		static ShaderLibrary m_ShaderLibrary;
	};
}

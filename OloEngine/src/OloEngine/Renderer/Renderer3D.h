#pragma once

#include "OloEngine/Renderer/Camera/Camera.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Renderer/Shader.h"

namespace OloEngine
{
	class Renderer3D
	{
	public:
		static void Init();
		static void Shutdown();

		static void BeginScene(const glm::mat4& viewProjectionMatrix);
		static void EndScene();

		static void DrawCube(const glm::mat4& modelMatrix, const glm::vec3& objectColor, const glm::vec3& lightColor);
		static void DrawLightCube(const glm::mat4& modelMatrix);

		// Phong lighting model setters
		static void SetLightPosition(const glm::vec3& position);
		static void SetViewPosition(const glm::vec3& position);
		static void SetLightingParameters(float ambientStrength, float specularStrength, float shininess);
	private:
		static ShaderLibrary m_ShaderLibrary;
	};
}

#pragma once

#include "OloEngine/Renderer/Camera.h"
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

		// Draw methods
		static void Draw(const glm::mat4& modelMatrix);
		static void Draw(const glm::mat4& modelMatrix, const Ref<Texture2D>& texture);

	private:
		static ShaderLibrary m_ShaderLibrary;
	};
}

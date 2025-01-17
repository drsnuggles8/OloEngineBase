#pragma once

#include "OloEngine/Renderer/RenderCommand.h"

#include "OloEngine/Renderer/OrthographicCamera.h"
#include "OloEngine/Renderer/Shader.h"

namespace OloEngine
{
	class Renderer
	{
	public:
		static void Init();
		static void Shutdown();

		static void OnWindowResize(u32 width, u32 height);

		static void BeginScene(OrthographicCamera const& camera);
		static void EndScene();

		static void Submit(const Ref<Shader>& shader, const Ref<VertexArray>& vertexArray, const glm::mat4& transform = glm::mat4(1.0f));

		[[nodiscard("Store this!")]] static RendererAPI::API GetAPI() { return RendererAPI::GetAPI(); }
	private:
		struct SceneData
		{
			glm::mat4 ViewProjectionMatrix;
		};

		static Scope<SceneData> s_SceneData;
	};
}

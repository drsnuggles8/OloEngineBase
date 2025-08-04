#pragma once

#include "OloEngine/Renderer/RenderCommand.h"

#include "OloEngine/Renderer/Camera/OrthographicCamera.h"
#include "OloEngine/Renderer/Shader.h"

namespace OloEngine
{
	enum class RendererType
	{
		Renderer2D,
		Renderer3D,
	};

	class Renderer
	{
	public:
		static void Init(RendererType type);
		static void Shutdown();

		static void OnWindowResize(u32 width, u32 height);

		static void BeginScene(OrthographicCamera const& camera);
		static void EndScene();

		static void Submit(const AssetRef<Shader>& shader, const AssetRef<VertexArray>& vertexArray, const glm::mat4& transform = glm::mat4(1.0f));

		[[nodiscard("Store this!")]] static RendererAPI::API GetAPI() { return RendererAPI::GetAPI(); }
	private:
		struct SceneData
		{
			glm::mat4 ViewProjectionMatrix;
		};

		static Scope<SceneData> s_SceneData;
		static RendererType s_RendererType;
	};
}

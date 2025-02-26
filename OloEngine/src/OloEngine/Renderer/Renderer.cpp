#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer.h"

#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Renderer/Renderer3D.h"


namespace OloEngine
{
	Scope<Renderer::SceneData> Renderer::s_SceneData = CreateScope<Renderer::SceneData>();
	RendererType Renderer::s_RendererType;

	void Renderer::Init(RendererType type)
	{
		OLO_PROFILE_FUNCTION();

		RenderCommand::Init();
		s_RendererType = type;
		switch (type)
		{
			case RendererType::Renderer2D:
				Renderer2D::Init();
				break;
			case RendererType::Renderer3D:
				Renderer3D::Init();
				break;
		}
	}

	void Renderer::Shutdown()
	{
		switch (s_RendererType)
		{
			case RendererType::Renderer2D:
				Renderer2D::Shutdown();
				break;
			case RendererType::Renderer3D:
				Renderer3D::Shutdown();
				break;
		}
	}

	void Renderer::OnWindowResize(const u32 width, const u32 height)
	{
		RenderCommand::SetViewport(0, 0, width, height);
	}

	void Renderer::BeginScene(OrthographicCamera const& camera)
	{
		s_SceneData->ViewProjectionMatrix = camera.GetViewProjectionMatrix();
	}

	void Renderer::EndScene()
	{
	}

	void Renderer::Submit(const Ref<Shader>& shader, const Ref<VertexArray>& vertexArray, const glm::mat4& transform)
	{
		shader->Bind();
		shader->SetMat4("u_ViewProjection", s_SceneData->ViewProjectionMatrix);
		shader->SetMat4("u_Transform", transform);

		vertexArray->Bind();
		RenderCommand::DrawIndexed(vertexArray);
	}
}

#pragma once

#include "OloEngine/Renderer/RendererAPI.h"

namespace OloEngine {

	class RenderCommand
	{
	public:
		static void Init()
		{
			s_RendererAPI->Init();
		}

		static void SetViewport(const uint32_t x, const uint32_t y, const uint32_t width, const uint32_t height)
		{
			s_RendererAPI->SetViewport(x, y, width, height);
		}

		static void SetClearColor(const glm::vec4& color)
		{
			s_RendererAPI->SetClearColor(color);
		}

		static void Clear()
		{
			s_RendererAPI->Clear();
		}

		static void DrawIndexed(const Ref<VertexArray>& vertexArray, const uint32_t count = 0)
		{
			s_RendererAPI->DrawIndexed(vertexArray, count);
		}
	private:
		static Scope<RendererAPI> s_RendererAPI;
	};

}

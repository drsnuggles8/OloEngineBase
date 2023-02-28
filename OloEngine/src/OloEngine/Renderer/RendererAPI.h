#pragma once

#include "OloEngine/Renderer/VertexArray.h"

#include <glm/glm.hpp>

namespace OloEngine
{

	class RendererAPI
	{
	public:
		enum class API
		{
			None = 0, OpenGL = 1
		};
	public:
		virtual ~RendererAPI() = default;

		virtual void Init() = 0;
		virtual void SetViewport(u32 x, u32 y, u32 width, u32 height) = 0;
		virtual void SetClearColor(const glm::vec4& color) = 0;
		virtual void Clear() = 0;

		virtual void DrawIndexed(const Ref<VertexArray>& vertexArray, u32 indexCount) = 0;
		virtual void DrawLines(const Ref<VertexArray>& vertexArray, u32 vertexCount) = 0;

		virtual void SetLineWidth(f32 width) = 0;

		virtual void EnableCulling() = 0;
		virtual void DisableCulling() = 0;
		virtual void FrontCull() = 0;
		virtual void BackCull() = 0;
		virtual void SetDepthMask(bool value) = 0;
		virtual void SetDepthTest(bool value) = 0;
		virtual void SetBlendState(bool value) = 0;

		[[nodiscard("Store this!")]] static API GetAPI() { return s_API; }
		static Scope<RendererAPI> Create();

	private:
		static API s_API;
	};

}

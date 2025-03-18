#pragma once

#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/RenderQueue.h"
#include "OloEngine/Renderer/RenderState.h"

#include <glad/gl.h>

namespace OloEngine
{
	class RenderCommand
	{
	public:
		static void Init()
		{
			s_RendererAPI->Init();
		}

		static void SetViewport(const u32 x, const u32 y, const u32 width, const u32 height)
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

		static void DrawArrays(const Ref<VertexArray>& vertexArray, u32 vertexCount)
		{
			s_RendererAPI->DrawArrays(vertexArray, vertexCount);
		}

		static void DrawIndexed(const Ref<VertexArray>& vertexArray, const u32 indexCount = 0)
		{
			s_RendererAPI->DrawIndexed(vertexArray, indexCount);
		}

		static void DrawIndexedInstanced(const Ref<VertexArray>& vertexArray, const u32 indexCount = 0, const u32 instanceCount = 1)
		{
			s_RendererAPI->DrawIndexedInstanced(vertexArray, indexCount, instanceCount);
		}

		static void DrawLines(const Ref<VertexArray>& vertexArray, const u32 vertexCount)
		{
			s_RendererAPI->DrawLines(vertexArray, vertexCount);
		}

		static void SetLineWidth(const f32 width)
		{
			LineWidthState state;
			state.Width = width;
			RenderQueue::SubmitStateChange(state);
		}

		static void EnableCulling()
		{
			CullingState state;
			state.Enabled = true;
			state.Face = GL_BACK; // Default back face culling
			RenderQueue::SubmitStateChange(state);
		}

		static void DisableCulling()
		{
			CullingState state;
			state.Enabled = false;
			RenderQueue::SubmitStateChange(state);
		}

		static void FrontCull()
		{
			CullingState state;
			state.Enabled = true;
			state.Face = GL_FRONT;
			RenderQueue::SubmitStateChange(state);
		}

		static void BackCull()
		{
			CullingState state;
			state.Enabled = true;
			state.Face = GL_BACK;
			RenderQueue::SubmitStateChange(state);
		}

		// Depth
		static void SetDepthMask(bool value)
		{
			DepthState state = GetCurrentDepthState();
			state.WriteMask = value;
			RenderQueue::SubmitStateChange(state);
		}

		static void SetDepthTest(bool value)
		{
			DepthState state = GetCurrentDepthState();
			state.TestEnabled = value;
			RenderQueue::SubmitStateChange(state);
		}

		static void SetDepthFunc(GLenum func)
		{
			DepthState state = GetCurrentDepthState();
			state.Function = func;
			RenderQueue::SubmitStateChange(state);
		}

		// Blending
		static void EnableBlending()
		{
			BlendState state = GetCurrentBlendState();
			state.Enabled = true;
			RenderQueue::SubmitStateChange(state);
		}
		
		static void DisableBlending()
		{
			BlendState state = GetCurrentBlendState();
			state.Enabled = false;
			RenderQueue::SubmitStateChange(state);
		}

		static void SetBlendState(bool value)
		{
			BlendState state = GetCurrentBlendState();
			state.Enabled = value;
			RenderQueue::SubmitStateChange(state);
		}

		static void SetBlendFunc(GLenum sfactor, GLenum dfactor)
		{
			BlendState state = GetCurrentBlendState();
			state.SrcFactor = sfactor;
			state.DstFactor = dfactor;
			RenderQueue::SubmitStateChange(state);
		}

		static void SetBlendEquation(GLenum mode)
		{
			BlendState state = GetCurrentBlendState();
			state.Equation = mode;
			RenderQueue::SubmitStateChange(state);
		}

		// Stencil
		static void EnableStencilTest()
		{
			StencilState state = GetCurrentStencilState();
			state.Enabled = true;
			RenderQueue::SubmitStateChange(state);
		}

		static void DisableStencilTest()
		{
			StencilState state = GetCurrentStencilState();
			state.Enabled = false;
			RenderQueue::SubmitStateChange(state);
		}

		static void SetStencilFunc(GLenum func, GLint ref, GLuint mask)
		{
			StencilState state = GetCurrentStencilState();
			state.Function = func;
			state.Reference = ref;
			state.ReadMask = mask;
			RenderQueue::SubmitStateChange(state);
		}

		static void SetStencilOp(GLenum sfail, GLenum dpfail, GLenum dppass)
		{
			StencilState state = GetCurrentStencilState();
			state.StencilFail = sfail;
			state.DepthFail = dpfail;
			state.DepthPass = dppass;
			RenderQueue::SubmitStateChange(state);
		}
		
		static void SetStencilMask(GLuint mask)
		{
			StencilState state = GetCurrentStencilState();
			state.WriteMask = mask;
			RenderQueue::SubmitStateChange(state);
		}

		static void ClearStencil()
		{
			s_RendererAPI->ClearStencil();
		}

		static void SetPolygonMode(GLenum face, GLenum mode)
		{
			PolygonModeState state;
			state.Face = face;
			state.Mode = mode;
			RenderQueue::SubmitStateChange(state);
		}

		static void EnableScissorTest()
		{
			ScissorState state = GetCurrentScissorState();
			state.Enabled = true;
			RenderQueue::SubmitStateChange(state);
		}

		static void DisableScissorTest()
		{
			ScissorState state = GetCurrentScissorState();
			state.Enabled = false;
			RenderQueue::SubmitStateChange(state);
		}

		static void SetScissorBox(GLint x, GLint y, GLsizei width, GLsizei height)
		{
			ScissorState state = GetCurrentScissorState();
			state.Enabled = true;
			state.X = x;
			state.Y = y;
			state.Width = width;
			state.Height = height;
			RenderQueue::SubmitStateChange(state);
		}
		
		static void BindDefaultFramebuffer()
		{
			s_RendererAPI->BindDefaultFramebuffer();
		}
		
		static void BindTexture(u32 slot, u32 textureID)
		{
			s_RendererAPI->BindTexture(slot, textureID);
		}

		static void SetPolygonOffset(f32 factor, f32 units)
		{
			PolygonOffsetState state;
			state.Enabled = true;
			state.Factor = factor;
			state.Units = units;
			RenderQueue::SubmitStateChange(state);
		}

		static void EnableMultisampling()
		{
			MultisamplingState state;
			state.Enabled = true;
			RenderQueue::SubmitStateChange(state);
		}

		static void DisableMultisampling()
		{
			MultisamplingState state;
			state.Enabled = false;
			RenderQueue::SubmitStateChange(state);
		}

		static void SetColorMask(bool red, bool green, bool blue, bool alpha)
		{
			ColorMaskState state;
			state.Red = red;
			state.Green = green;
			state.Blue = blue;
			state.Alpha = alpha;
			RenderQueue::SubmitStateChange(state);
		}
		
		// For internal use by the renderer
		static RendererAPI& GetRendererAPI() { return *s_RendererAPI; }

	private:
		// Helper functions to get current state
		static DepthState GetCurrentDepthState()
		{
			return RenderQueue::GetCurrentState().Depth;
		}
		
		static BlendState GetCurrentBlendState()
		{
			return RenderQueue::GetCurrentState().Blend;
		}
		
		static StencilState GetCurrentStencilState()
		{
			return RenderQueue::GetCurrentState().Stencil;
		}
		
		static ScissorState GetCurrentScissorState()
		{
			return RenderQueue::GetCurrentState().Scissor;
		}

		static Scope<RendererAPI> s_RendererAPI;
	};
}

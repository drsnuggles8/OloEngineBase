#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Passes/UICompositeRenderPass.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/Renderer.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/MeshPrimitives.h"

namespace OloEngine
{
	UICompositeRenderPass::UICompositeRenderPass()
	{
		SetName("UICompositePass");
		OLO_CORE_INFO("Creating UICompositeRenderPass.");
	}

	void UICompositeRenderPass::Init(const FramebufferSpecification& spec)
	{
		OLO_PROFILE_FUNCTION();

		m_FramebufferSpec = spec;

		m_BlitShader = Shader::Create("assets/shaders/FullscreenBlit.glsl");

		CreateFramebuffer(spec.Width, spec.Height);

		OLO_CORE_INFO("UICompositeRenderPass: Initialized {}x{}", spec.Width, spec.Height);
	}

	void UICompositeRenderPass::CreateFramebuffer(u32 width, u32 height)
	{
		FramebufferSpecification fbSpec;
		fbSpec.Width = width;
		fbSpec.Height = height;
		fbSpec.Samples = 1;
		// Match the ScenePass MRT layout so Renderer2D shaders (which output to 3
		// locations: color, entity ID, view-normal) don't trigger NVIDIA driver
		// shader recompilation when the draw-buffer configuration changes.
		fbSpec.Attachments = {
			FramebufferTextureFormat::RGBA8,       // [0] LDR color (composited scene + UI)
			FramebufferTextureFormat::RED_INTEGER,  // [1] Entity ID (for editor picking)
			FramebufferTextureFormat::RG16F         // [2] View-normal (unused, but keeps MRT layout stable)
		};

		m_Target = Framebuffer::Create(fbSpec);
	}

	void UICompositeRenderPass::Execute()
	{
		OLO_PROFILE_FUNCTION();

		if (!m_Target)
		{
			return;
		}

		// Bind our FBO and clear all attachments (handles mixed integer/float types)
		m_Target->Bind();
		RenderCommand::SetViewport(0, 0, m_FramebufferSpec.Width, m_FramebufferSpec.Height);
		m_Target->ClearAllAttachments({ 0.0f, 0.0f, 0.0f, 1.0f }, -1);

		// Blit the post-processed scene as background
		if (m_InputFramebuffer && m_BlitShader)
		{
			RenderCommand::SetBlendState(false);
			RenderCommand::SetDepthTest(false);

			m_BlitShader->Bind();
			u32 colorAttachment = m_InputFramebuffer->GetColorAttachmentRendererID(0);
			RenderCommand::BindTexture(0, colorAttachment);
			m_BlitShader->SetInt("u_Texture", 0);

			auto va = MeshPrimitives::GetFullscreenTriangle();
			va->Bind();
			RenderCommand::DrawIndexed(va);
		}
		else
		{
			OLO_CORE_WARN("UICompositePass: No input framebuffer ({}) or blit shader ({}) — scene background will be black",
						  m_InputFramebuffer != nullptr, m_BlitShader != nullptr);
		}

		// Render 2D overlays and screen-space UI via the per-frame callback
		if (m_RenderCallback)
		{
			RenderCommand::SetBlendState(true);
			RenderCommand::SetBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			RenderCommand::SetDepthTest(false);
			RenderCommand::SetDepthMask(false);

			m_RenderCallback();

			// One-shot: clear for next frame
			m_RenderCallback = nullptr;
		}
		else
		{
			OLO_CORE_WARN("UICompositePass: No render callback set — UI will not render this frame");
		}
	}

	Ref<Framebuffer> UICompositeRenderPass::GetTarget() const
	{
		return m_Target;
	}

	void UICompositeRenderPass::SetupFramebuffer(u32 width, u32 height)
	{
		OLO_PROFILE_FUNCTION();

		m_FramebufferSpec.Width = width;
		m_FramebufferSpec.Height = height;

		CreateFramebuffer(width, height);

		OLO_CORE_INFO("UICompositeRenderPass: Setup {}x{}", width, height);
	}

	void UICompositeRenderPass::ResizeFramebuffer(u32 width, u32 height)
	{
		OLO_PROFILE_FUNCTION();

		if (width == 0 || height == 0)
		{
			OLO_CORE_WARN("UICompositeRenderPass::ResizeFramebuffer: Invalid dimensions {}x{}", width, height);
			return;
		}

		m_FramebufferSpec.Width = width;
		m_FramebufferSpec.Height = height;

		if (m_Target)
		{
			m_Target->Resize(width, height);
		}

		OLO_CORE_INFO("UICompositeRenderPass: Resized to {}x{}", width, height);
	}

	void UICompositeRenderPass::OnReset()
	{
		OLO_PROFILE_FUNCTION();

		if (m_FramebufferSpec.Width > 0 && m_FramebufferSpec.Height > 0)
		{
			CreateFramebuffer(m_FramebufferSpec.Width, m_FramebufferSpec.Height);
		}
	}

	void UICompositeRenderPass::SetInputFramebuffer(const Ref<Framebuffer>& input)
	{
		m_InputFramebuffer = input;
	}

	void UICompositeRenderPass::SetRenderCallback(RenderCallback callback)
	{
		m_RenderCallback = std::move(callback);
	}
} // namespace OloEngine

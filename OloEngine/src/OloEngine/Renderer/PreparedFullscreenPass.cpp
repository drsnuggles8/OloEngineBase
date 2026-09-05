#include "OloEnginePCH.h"
#include "OloEngine/Renderer/PreparedFullscreenPass.h"

#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderCommand.h"

namespace OloEngine
{
    RGPreparedPass PrepareFullscreenPass(Ref<Framebuffer> target, Ref<Shader> shader,
                                         std::vector<FullscreenTextureInput> textures,
                                         std::vector<Ref<UniformBuffer>> uniforms, bool clear,
                                         std::vector<u32> drawBuffers, glm::vec4 clearColor)
    {
        if (!target || !shader)
            return {};

        RGPreparedPass prepared;
        for (const auto& texture : textures)
            prepared.Resources.push_back({ texture.Texture, false });
        for (auto& uniform : uniforms)
            if (uniform)
            {
                uniform->PrepareForParallelRead();
                prepared.Resources.push_back({ uniform->GetRHIHandle(), false });
            }
        u32 color = 0;
        for (const auto& attachment : target->GetSpecification().Attachments.Attachments)
        {
            if (attachment.TextureFormat == FramebufferTextureFormat::None)
                continue;
            const bool depth = attachment.TextureFormat == FramebufferTextureFormat::DEPTH24STENCIL8 ||
                               attachment.TextureFormat == FramebufferTextureFormat::DEPTH_COMPONENT32F;
            prepared.Resources.push_back({ depth ? target->GetDepthAttachmentHandle() : target->GetColorAttachmentHandle(color++), true });
        }

        // The shared primitive is lazy; create it before any worker can ask.
        auto triangle = MeshPrimitives::GetFullscreenTriangle();
        prepared.Record = [target = std::move(target), shader = std::move(shader), triangle = std::move(triangle),
                           textures = std::move(textures), uniforms = std::move(uniforms), clear, drawBuffers = std::move(drawBuffers), clearColor](RGCommandContext& context) mutable
        {
            for (const auto& uniform : uniforms)
                if (uniform)
                    uniform->Bind();
            target->Bind();
            const auto& spec = target->GetSpecification();
            context.SetViewport(0, 0, spec.Width, spec.Height);
            context.SetDepthTest(false);
            context.SetDepthMask(false);
            context.SetBlendState(false);
            context.SetCulling(false);
            RenderCommand::DisableStencilTest();
            RenderCommand::DisableScissorTest();
            RenderCommand::SetPolygonMode(RHI::PolygonMode::Fill);
            RenderCommand::SetColorMask(true, true, true, true);
            context.SetDrawBuffers(drawBuffers);
            if (clear)
            {
                context.SetClearColor(clearColor);
                context.Clear();
            }
            shader->Bind();
            for (const auto& texture : textures)
            {
                context.BindTextureOrHeapOffset(texture.Slot, texture.Texture, texture.Lifetime);
                if (!texture.SamplerUniform.empty())
                    shader->SetInt(std::string(texture.SamplerUniform), static_cast<i32>(texture.Slot));
            }
            triangle->Bind();
            context.FlushHeapOffsets();
            context.DrawIndexed(triangle);
            context.SetDepthMask(true);
            target->Unbind();
        };
        return prepared;
    }
} // namespace OloEngine

#include "OloEnginePCH.h"
#include "OloEngine/Renderer/SkyCubemapBake.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RHI/RHIProjectionSeam.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace OloEngine::SkyBake
{
    namespace
    {
        // The six look directions + 90-degree projection used to capture each
        // cubemap face. Same convention as IBLPrecompute::RenderToCubemap so the
        // baked cubemap is sampled consistently by the rest of the IBL path.
        struct CaptureMatrices
        {
            glm::mat4 Views[6];
            glm::mat4 Projection;
        };

        const CaptureMatrices& GetCaptureMatrices()
        {
            static const CaptureMatrices kMatrices = []
            {
                CaptureMatrices m{};
                m.Views[0] = glm::lookAt(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f));
                m.Views[1] = glm::lookAt(glm::vec3(0.0f), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f));
                m.Views[2] = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
                m.Views[3] = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
                m.Views[4] = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, -1.0f, 0.0f));
                m.Views[5] = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f));
                m.Projection = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
                return m;
            }();
            return kMatrices;
        }
    } // namespace

    bool RenderSkyToCubemap(Ref<TextureCubemap> cubemap,
                            Ref<Shader> shader,
                            Ref<UniformBuffer> cameraUBO,
                            Ref<UniformBuffer> skyUBO)
    {
        OLO_PROFILE_FUNCTION();

        if (!cubemap || !shader || !cameraUBO || !skyUBO)
        {
            OLO_CORE_ERROR("SkyBake::RenderSkyToCubemap: null argument");
            return false;
        }

        const u32 face = cubemap->GetWidth();
        const auto& mats = GetCaptureMatrices();

        const bool wasStencilEnabled = RenderCommand::IsStencilTestEnabled();
        if (wasStencilEnabled)
            RenderCommand::DisableStencilTest();

        shader->Bind();
        skyUBO->Bind();

        FramebufferSpecification fbSpec;
        fbSpec.Width = face;
        fbSpec.Height = face;
        fbSpec.Attachments = { FramebufferTextureFormat::RGBA32F, FramebufferTextureFormat::Depth };
        auto framebuffer = Framebuffer::Create(fbSpec);
        if (!framebuffer)
        {
            OLO_CORE_ERROR("SkyBake::RenderSkyToCubemap: failed to allocate framebuffer");
            if (wasStencilEnabled)
                RenderCommand::EnableStencilTest();
            return false;
        }

        auto cubeMesh = MeshPrimitives::CreateSkyboxCube();
        if (!cubeMesh)
        {
            if (wasStencilEnabled)
                RenderCommand::EnableStencilTest();
            return false;
        }

        // Hoisted out of the face loop: the framebuffer is not recreated between
        // faces, so its attachment identity is constant for the whole bake
        // (matches ReflectionProbeBaker::CaptureSceneCubemap).
        const RHI::ResourceHandle fbColor = framebuffer->GetColorAttachmentHandle(0);

        for (u32 i = 0; i < 6; ++i)
        {
            OLO_PROFILE_SCOPE("SkyBake::Face");

            // Update CameraUBO with this face's view/projection. The shader's
            // vertex stage picks up u_ViewProjection just like every other
            // skybox-style shader, turning the cube vertex into a sample dir.
            // A8 seam, CAPTURE flavour (z remap, no y flip) — covers the three
            // procedural sky bakes (Atmosphere/Preetham/StarNest) that route
            // through this one face loop. These faces are addressed by
            // DIRECTION, not screen uv, so the y flip would only store each
            // face row-mirrored relative to the GL bake while direction->texel
            // addressing stayed API-identical. Dormant today (no sky shader
            // has a pull branch yet) but the correct flavour is free to use
            // now and nothing would fail later if it were wrong.
            ShaderBindingLayout::CameraUBO data;
            data.ViewProjection = RHI::AdjustCaptureProjectionForBackend(mats.Projection * mats.Views[i]);
            data.View = mats.Views[i];
            data.Projection = RHI::AdjustCaptureProjectionForBackend(mats.Projection);
            data.Position = glm::vec3(0.0f);
            data.Pad0 = 0.0f;
            // Reconstruction sibling of the CAPTURE flavour is the RAW matrix
            // (#691): no y flip (capture never flips), no z remap (a
            // reconstructing shader applies its own).
            data.ProjectionForReconstruction = mats.Projection;
            cameraUBO->SetData(&data, ShaderBindingLayout::CameraUBO::GetSize());
            cameraUBO->Bind();

            framebuffer->Bind();
            RenderCommand::SetViewport(0, 0, face, face);
            RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 1.0f });
            RenderCommand::ClearColorAndDepth();

            auto vao = cubeMesh->GetVertexArray();
            vao->Bind();
            RenderCommand::DrawIndexed(vao);

            RenderCommand::CopyImageSubDataFull(
                fbColor, RendererAPI::TextureTargetType::Texture2D, 0, 0,
                cubemap->GetRHIHandle(), RendererAPI::TextureTargetType::TextureCubeMap, 0, static_cast<i32>(i),
                face, face);
        }

        framebuffer->Unbind();

        // Build the mip chain for the freshly baked faces (#943). Every face was
        // written through CopyImageSubDataFull above, which touches level 0 only,
        // so without this the chain is allocated and stale — and a coarse mip of
        // last frame's sky is worse than no mip at all.
        //
        // Why the chain exists: the water surface is a near-mirror that samples
        // this cubemap along `reflect(-viewDir, normal)`. On a rippled sea at a
        // grazing angle that direction sweeps a wide solid angle inside a single
        // pixel, and a single-level cubemap gives the hardware nothing to filter
        // with — the reflection aliases into hard-edged flats (issue #943, and
        // the "plateaus" of #898). Water.glsl now samples it with textureGrad so
        // the footprint picks a mip. Consumers that must keep reading the base
        // level ask for it explicitly (IBLPrefilter.glsl, IrradianceConvolution
        // .glsl); the skybox draw is unaffected because it samples with no
        // gradient spread.
        //
        // No-op for a cubemap created with GenerateMips = false, so the three
        // sky bakes that route through here opt in via their own spec.
        cubemap->GenerateMipmaps();

        if (wasStencilEnabled)
            RenderCommand::EnableStencilTest();

        return true;
    }
} // namespace OloEngine::SkyBake

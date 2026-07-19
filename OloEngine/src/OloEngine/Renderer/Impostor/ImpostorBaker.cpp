#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Impostor/ImpostorBaker.h"
#include "OloEngine/Renderer/Impostor/OctahedralImpostor.h"

#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/VertexArray.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    namespace
    {
        // std140 layout — must match Impostor_Bake.glsl's ImpostorBakeParams.
        struct ImpostorBakeUBO
        {
            glm::mat4 ViewProjection;
            glm::vec4 CenterRadius; // xyz = centre, w = radius
            glm::vec4 DirCutoff;    // xyz = view dir (obj -> camera), w = alpha cutoff
            glm::vec4 Tint;         // rgb = tint, a = unused
        };
        static_assert(sizeof(ImpostorBakeUBO) == 112, "ImpostorBakeUBO must match GLSL std140 (112 bytes)");

        Ref<Texture2D> GetWhiteFallback()
        {
            static Ref<Texture2D> s_White;
            if (!s_White)
            {
                TextureSpecification spec;
                spec.Width = 1;
                spec.Height = 1;
                spec.Format = ImageFormat::RGBA8;
                spec.GenerateMips = false;
                s_White = Texture2D::Create(spec);
                u32 whitePixel = 0xFFFFFFFFu;
                s_White->SetData(&whitePixel, sizeof(whitePixel));
            }
            return s_White;
        }

        Ref<Texture2D> CreateAtlasTexture(u32 size)
        {
            TextureSpecification spec;
            spec.Width = size;
            spec.Height = size;
            spec.Format = ImageFormat::RGBA8;
            spec.GenerateMips = false; // single mip — mip chains bleed across tile borders
            spec.SRGB = false;
            return Texture2D::Create(spec);
        }
    } // namespace

    ImpostorAtlas ImpostorBaker::Bake(
        const Ref<Mesh>& mesh,
        const Ref<Texture2D>& albedoTexture,
        const glm::vec3& tint,
        u32 framesPerAxis,
        u32 atlasResolution,
        bool hemi,
        f32 alphaCutoff)
    {
        OLO_PROFILE_FUNCTION();

        ImpostorAtlas atlas;

        if (!mesh || !mesh->GetVertexArray())
        {
            OLO_CORE_WARN("ImpostorBaker::Bake: null mesh / vertex array — skipping bake");
            return atlas;
        }

        const u32 N = std::clamp(framesPerAxis, 2u, 32u);
        const u32 requestedRes = std::clamp(atlasResolution, N * 16u, 4096u);
        const u32 tileRes = std::max(requestedRes / N, 16u);
        const u32 atlasSize = tileRes * N; // exact multiple of N so tiles divide evenly

        auto bakeShader = Renderer3D::GetShaderLibrary().Get("Impostor_Bake");
        if (!bakeShader)
        {
            OLO_CORE_ERROR("ImpostorBaker::Bake: Impostor_Bake shader not available");
            return atlas;
        }

        // Frame the WHOLE MeshSource, not just submesh 0: DrawIndexed(vao) below
        // renders the full shared index buffer (every submesh), so framing to the
        // per-submesh bounds would clip a multi-submesh model's other parts.
        const BoundingBox srcBox = mesh->GetMeshSource() ? mesh->GetMeshSource()->GetBoundingBox() : mesh->GetBoundingBox();
        const glm::vec3 center = (srcBox.Min + srcBox.Max) * 0.5f;
        const f32 radius = std::max(glm::length(srcBox.Max - center), 1e-3f);

        // Offscreen MRT: RT0 = albedo+coverage, RT1 = normal+depth, + depth buffer.
        FramebufferSpecification fbSpec;
        fbSpec.Width = atlasSize;
        fbSpec.Height = atlasSize;
        fbSpec.Attachments = {
            FramebufferTextureFormat::RGBA8,
            FramebufferTextureFormat::RGBA8,
            FramebufferTextureFormat::Depth
        };
        auto framebuffer = Framebuffer::Create(fbSpec);

        auto bakeUBO = UniformBuffer::Create(sizeof(ImpostorBakeUBO), ShaderBindingLayout::UBO_IMPOSTOR_BAKE);

        const Ref<Texture2D> albedo = albedoTexture ? albedoTexture : GetWhiteFallback();

        // Preserve stencil state (mirrors IBLPrecompute / SkyCubemapBake).
        const bool wasStencil = RenderCommand::IsStencilTestEnabled();
        if (wasStencil)
            RenderCommand::DisableStencilTest();
        // Depth test + write on so per-tile silhouettes resolve correctly. The
        // scene's command dispatch re-applies its own PODRenderState per draw
        // next frame, so no explicit restore is needed here.
        RenderCommand::SetDepthTest(true);
        RenderCommand::SetDepthMask(true);

        framebuffer->Bind();

        // Bind the bake program BEFORE clearing so the clear revalidates against
        // the atlas FBO with the matching program bound (gl-clear-program-
        // revalidation.md) — no GLClearProgramGuard needed for this shape.
        bakeShader->Bind();

        RenderCommand::SetViewport(0, 0, atlasSize, atlasSize);
        RenderCommand::SetClearColor({ 0.0f, 0.0f, 0.0f, 0.0f });
        RenderCommand::ClearColorAndDepth();

        albedo->Bind(ShaderBindingLayout::TEX_DIFFUSE);

        auto vao = mesh->GetVertexArray();

        for (u32 fy = 0; fy < N; ++fy)
        {
            for (u32 fx = 0; fx < N; ++fx)
            {
                const glm::ivec2 frame(static_cast<i32>(fx), static_cast<i32>(fy));
                const glm::vec3 dir = Impostor::FrameToDirection(frame, N, hemi); // object -> camera

                const glm::vec3 eye = center + dir * (radius * 2.5f);
                const glm::vec3 up = (std::abs(dir.y) > 0.999f) ? glm::vec3(0.0f, 0.0f, -1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
                const glm::mat4 view = glm::lookAt(eye, center, up);
                const glm::mat4 proj = glm::ortho(-radius, radius, -radius, radius, radius * 0.5f, radius * 5.0f);

                ImpostorBakeUBO ubo{};
                ubo.ViewProjection = proj * view;
                ubo.CenterRadius = glm::vec4(center, radius);
                ubo.DirCutoff = glm::vec4(dir, alphaCutoff);
                ubo.Tint = glm::vec4(tint, 0.0f);
                bakeUBO->SetData(&ubo, sizeof(ImpostorBakeUBO));
                bakeUBO->Bind();

                RenderCommand::SetViewport(fx * tileRes, fy * tileRes, tileRes, tileRes);

                vao->Bind();
                RenderCommand::DrawIndexed(vao);
            }
        }

        framebuffer->Unbind();

        if (wasStencil)
            RenderCommand::EnableStencilTest();

        // Copy the two color attachments into persistent atlas textures.
        atlas.Albedo = CreateAtlasTexture(atlasSize);
        atlas.NormalDepth = CreateAtlasTexture(atlasSize);

        RenderCommand::CopyImageSubDataFull(
            framebuffer->GetColorAttachmentRendererID(0), RendererAPI::TextureTargetType::Texture2D, 0, 0,
            atlas.Albedo->GetRendererID(), RendererAPI::TextureTargetType::Texture2D, 0, 0,
            atlasSize, atlasSize);
        RenderCommand::CopyImageSubDataFull(
            framebuffer->GetColorAttachmentRendererID(1), RendererAPI::TextureTargetType::Texture2D, 0, 0,
            atlas.NormalDepth->GetRendererID(), RendererAPI::TextureTargetType::Texture2D, 0, 0,
            atlasSize, atlasSize);

        atlas.FramesPerAxis = N;
        atlas.Hemi = hemi;
        atlas.Radius = radius;
        atlas.Center = center;

        OLO_CORE_INFO("ImpostorBaker: baked {}x{} octahedral atlas ({} tiles @ {}px, radius {:.2f})",
                      N, N, N * N, tileRes, radius);
        return atlas;
    }
} // namespace OloEngine

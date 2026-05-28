#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Preview/AssetPreviewRenderer.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MaterialAsset.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/VertexArray.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace OloEngine
{
    Ref<Framebuffer> AssetPreviewRenderer::s_Framebuffer;
    Ref<Shader> AssetPreviewRenderer::s_Shader;
    Ref<Mesh> AssetPreviewRenderer::s_SphereMesh;
    Ref<UniformBuffer> AssetPreviewRenderer::s_PreviewUBO;
    Ref<Texture2D> AssetPreviewRenderer::s_DefaultWhite;
    bool AssetPreviewRenderer::s_Initialised = false;

    // Camera + transform for a single preview render. Bundled so the
    // sphere and arbitrary-mesh paths can share `RenderInto`.
    struct PreviewCameraRig
    {
        glm::mat4 Model{ 1.0f };
        glm::vec3 EyePos{ 0.0f, 0.0f, 2.6f };
        glm::vec3 LookAt{ 0.0f };
        f32 FovYDeg{ 35.0f };
        f32 NearClip{ 0.05f };
        f32 FarClip{ 25.0f };
    };

    namespace
    {
        // Default camera rig for the sphere preview. Pulled back along +Z
        // so the unit-radius icosphere fills ~60% of the framebuffer.
        constexpr f32 kSphereCameraDistance = 2.6f;
        constexpr f32 kCameraFovYDeg = 35.0f;
        constexpr f32 kNearClip = 0.05f;
        constexpr f32 kFarClip = 25.0f;

        PreviewCameraRig MakeSphereRig()
        {
            PreviewCameraRig rig;
            rig.Model = glm::mat4(1.0f);
            rig.EyePos = glm::vec3(0.0f, 0.0f, kSphereCameraDistance);
            rig.LookAt = glm::vec3(0.0f);
            rig.FovYDeg = kCameraFovYDeg;
            rig.NearClip = kNearClip;
            rig.FarClip = kFarClip;
            return rig;
        }

        // Frame a mesh: centre the AABB at the origin, pull the camera
        // back along +Z by a distance derived from the bounding sphere
        // radius so the model occupies roughly the same screen fraction
        // as the sphere preview.
        PreviewCameraRig MakeMeshRig(const Ref<Mesh>& mesh)
        {
            PreviewCameraRig rig;
            rig.FovYDeg = kCameraFovYDeg;
            rig.LookAt = glm::vec3(0.0f);

            if (!mesh || !mesh->IsValid())
            {
                rig.Model = glm::mat4(1.0f);
                rig.EyePos = glm::vec3(0.0f, 0.0f, kSphereCameraDistance);
                rig.NearClip = kNearClip;
                rig.FarClip = kFarClip;
                return rig;
            }

            BoundingBox aabb = mesh->GetBoundingBox();
            const glm::vec3 center = aabb.GetCenter();
            const glm::vec3 extents = aabb.GetExtents();
            const f32 radius = glm::max(glm::length(extents), 0.001f);

            // Translate mesh so its center sits at the origin.
            rig.Model = glm::translate(glm::mat4(1.0f), -center);

            // Distance so the bounding sphere subtends ~60% of the FOV.
            // tan(fov/2) * dist = radius / 0.6  →  dist = radius / (0.6 * tan(fov/2))
            const f32 halfFovTan = glm::tan(glm::radians(rig.FovYDeg) * 0.5f);
            const f32 dist = radius / (0.6f * halfFovTan) + radius;
            // Pull along +Z, slightly elevated for a more flattering view
            // (3/4 angle instead of straight-on).
            rig.EyePos = glm::vec3(0.0f, radius * 0.35f, dist);
            rig.NearClip = glm::max(0.01f, dist - 4.0f * radius);
            rig.FarClip = dist + 4.0f * radius;
            return rig;
        }
    } // namespace

    const std::string& AssetPreviewRenderer::GetDefaultShaderPath()
    {
        // Static-local so the function can serve as a `const&` default
        // argument without needing a string allocation per call.
        static const std::string s_Path = "assets/shaders/MaterialPreview.glsl";
        return s_Path;
    }

    void AssetPreviewRenderer::Initialize(const std::string& shaderPath)
    {
        if (s_Initialised)
            return;

        OLO_PROFILE_FUNCTION();

        FramebufferSpecification fbSpec;
        fbSpec.Width = kThumbnailSize;
        fbSpec.Height = kThumbnailSize;
        fbSpec.Attachments = { FramebufferTextureFormat::RGBA8, FramebufferTextureFormat::Depth };
        s_Framebuffer = Framebuffer::Create(fbSpec);
        if (!s_Framebuffer)
        {
            OLO_CORE_ERROR("AssetPreviewRenderer: failed to create framebuffer");
            return;
        }

        s_Shader = Shader::Create(shaderPath);
        if (!s_Shader)
        {
            OLO_CORE_ERROR("AssetPreviewRenderer: failed to load shader from '{}'", shaderPath);
            s_Framebuffer = nullptr;
            return;
        }

        s_PreviewUBO = UniformBuffer::Create(sizeof(PreviewBlock), ShaderBindingLayout::UBO_PREVIEW);
        if (!s_PreviewUBO)
        {
            OLO_CORE_ERROR("AssetPreviewRenderer: failed to create preview UBO");
            s_Framebuffer = nullptr;
            s_Shader = nullptr;
            return;
        }

        if (!EnsureSphereMesh())
        {
            OLO_CORE_ERROR("AssetPreviewRenderer: failed to build unit sphere mesh");
            s_Framebuffer = nullptr;
            s_Shader = nullptr;
            s_PreviewUBO = nullptr;
            return;
        }
        if (!EnsureDefaultWhiteTexture())
        {
            OLO_CORE_ERROR("AssetPreviewRenderer: failed to build default white texture");
            s_Framebuffer = nullptr;
            s_Shader = nullptr;
            s_PreviewUBO = nullptr;
            s_SphereMesh = nullptr;
            return;
        }
        s_Initialised = true;
    }

    void AssetPreviewRenderer::Shutdown()
    {
        s_Framebuffer = nullptr;
        s_Shader = nullptr;
        s_SphereMesh = nullptr;
        s_PreviewUBO = nullptr;
        s_DefaultWhite = nullptr;
        s_Initialised = false;
    }

    bool AssetPreviewRenderer::IsInitialized()
    {
        return s_Initialised;
    }

    bool AssetPreviewRenderer::EnsureSphereMesh()
    {
        if (s_SphereMesh && s_SphereMesh->IsValid())
            return true;
        // Renderer3D's shared icosphere lives behind a static API but is
        // only valid after Renderer3D::Init, which a host may or may not
        // have done by the time a thumbnail is first requested. Build our
        // own unit icosphere up front so the preview path is independent
        // of the main renderer's lifecycle.
        s_SphereMesh = MeshPrimitives::CreateIcosphere(1.0f, 2);
        return s_SphereMesh && s_SphereMesh->IsValid();
    }

    bool AssetPreviewRenderer::EnsureDefaultWhiteTexture()
    {
        if (s_DefaultWhite)
            return true;
        TextureSpecification spec;
        spec.Width = 1;
        spec.Height = 1;
        spec.Format = ImageFormat::RGBA8;
        spec.GenerateMips = false;
        spec.MipLevels = 1;
        s_DefaultWhite = Texture2D::Create(spec);
        if (!s_DefaultWhite)
            return false;
        u8 white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
        s_DefaultWhite->SetData(white, sizeof(white));
        return true;
    }

    Ref<Texture2D> AssetPreviewRenderer::CreateTargetTexture()
    {
        TextureSpecification spec;
        spec.Width = kThumbnailSize;
        spec.Height = kThumbnailSize;
        spec.Format = ImageFormat::RGBA8;
        spec.GenerateMips = false;
        spec.MipLevels = 1;
        return Texture2D::Create(spec);
    }

    void AssetPreviewRenderer::FillDefaultMaterialBlock(PreviewBlock& block)
    {
        block.AlbedoFactor = glm::vec3(0.7f, 0.7f, 0.72f);
        block.MetallicFactor = 0.0f;
        block.RoughnessFactor = 0.45f;
        block.EmissiveFactor = 0.0f;
        block.UseAlbedoMap = 0;
        block.UseMetalnessMap = 0;
        block.UseRoughnessMap = 0;

        // Even when the shader's `u_Use*` flags are false the texture
        // units must still have a complete texture bound — many drivers
        // crash or sample garbage from a slot with no texture, even if
        // the shader's branch wouldn't read it. The 1×1 white texture
        // is the cheap "definitely complete" stand-in.
        //
        // Slot numbers match `ShaderBindingLayout`:
        //   0 = TEX_DIFFUSE   (albedo)
        //   6 = TEX_ROUGHNESS (roughness)
        //   7 = TEX_METALLIC  (metallic)
        if (s_DefaultWhite)
        {
            s_DefaultWhite->Bind(ShaderBindingLayout::TEX_DIFFUSE);
            s_DefaultWhite->Bind(ShaderBindingLayout::TEX_ROUGHNESS);
            s_DefaultWhite->Bind(ShaderBindingLayout::TEX_METALLIC);
        }
    }

    void AssetPreviewRenderer::FillMaterialBlock(const Ref<MaterialAsset>& material, PreviewBlock& block)
    {
        if (!material)
        {
            FillDefaultMaterialBlock(block);
            return;
        }

        // Factors come from `MaterialAsset`'s public accessor (the official
        // asset API; uses the underlying Material's generic uniform map).
        //
        // Each factor is validated with `std::isfinite` before going into
        // the UBO so a NaN/Inf left behind by a malformed .olomaterial
        // or a scripting glitch can't poison the shader math (cpp-coding-
        // quality §2). The fallback values match `FillDefaultMaterialBlock`
        // (neutral grey, non-metallic, mid-roughness, no emission), so a
        // dirty material renders as the standard default sphere rather
        // than a NaN-coloured blob.
        const auto safeF = [](f32 v, f32 fallback) noexcept
        {
            return std::isfinite(v) ? v : fallback;
        };
        const auto safeV3 = [&safeF](const glm::vec3& v, const glm::vec3& fallback) noexcept
        {
            return glm::vec3(safeF(v.x, fallback.x), safeF(v.y, fallback.y), safeF(v.z, fallback.z));
        };

        block.AlbedoFactor = safeV3(material->GetAlbedoColor(), glm::vec3(0.7f, 0.7f, 0.72f));
        block.MetallicFactor = glm::clamp(safeF(material->GetMetalness(), 0.0f), 0.0f, 1.0f);
        block.RoughnessFactor = glm::clamp(safeF(material->GetRoughness(), 0.45f), 0.0f, 1.0f);
        block.EmissiveFactor = glm::max(0.0f, safeF(material->GetEmission(), 0.0f));

        // Textures are pulled directly off the underlying `Material` rather
        // than through `MaterialAsset::GetAlbedoMap()` and friends. The
        // asset accessors fall back to `AssetManager::GetPlaceholderAsset`
        // → `AssetManager::GetAsset` whenever a texture slot is empty, and
        // `GetAsset` asserts when no project is active (the headless test
        // rig in particular). `Material::TryGetTexture2D` returns null
        // cleanly on the same "no texture set" path.
        Ref<Material> mat = material->GetMaterial();
        Ref<Texture2D> albedoMap = mat ? mat->TryGetTexture2D("u_AlbedoTexture") : nullptr;
        Ref<Texture2D> metalMap = mat ? mat->TryGetTexture2D("u_MetalnessTexture") : nullptr;
        Ref<Texture2D> roughMap = mat ? mat->TryGetTexture2D("u_RoughnessTexture") : nullptr;

        block.UseAlbedoMap = albedoMap ? 1 : 0;
        block.UseMetalnessMap = metalMap ? 1 : 0;
        block.UseRoughnessMap = roughMap ? 1 : 0;

        // Bind a real texture on every sampler slot. For unused slots
        // we fall back to the 1×1 white default so the GL state machine
        // always sees a complete texture there — see FillDefaultMaterialBlock
        // for the rationale. Slot numbers match `ShaderBindingLayout`'s
        // documented PBR layout.
        const u32 albedoSlot = ShaderBindingLayout::TEX_DIFFUSE;
        const u32 roughSlot = ShaderBindingLayout::TEX_ROUGHNESS;
        const u32 metalSlot = ShaderBindingLayout::TEX_METALLIC;
        if (albedoMap)
            albedoMap->Bind(albedoSlot);
        else if (s_DefaultWhite)
            s_DefaultWhite->Bind(albedoSlot);
        if (metalMap)
            metalMap->Bind(metalSlot);
        else if (s_DefaultWhite)
            s_DefaultWhite->Bind(metalSlot);
        if (roughMap)
            roughMap->Bind(roughSlot);
        else if (s_DefaultWhite)
            s_DefaultWhite->Bind(roughSlot);
    }

    Ref<Texture2D> AssetPreviewRenderer::RenderInto(const Ref<Mesh>& mesh,
                                                    const PreviewCameraRig& rig,
                                                    PreviewBlock& block)
    {
        OLO_PROFILE_FUNCTION();

        if (!s_Initialised || !s_Shader || !s_Framebuffer || !s_PreviewUBO)
            return nullptr;
        if (!mesh || !mesh->IsValid())
        {
            OLO_CORE_WARN("AssetPreviewRenderer::RenderInto: mesh invalid");
            return nullptr;
        }

        // Fill the matrix half of the UBO; the material half was already
        // filled by FillMaterialBlock before RenderInto was called.
        block.Model = rig.Model;
        block.View = glm::lookAt(rig.EyePos, rig.LookAt, glm::vec3(0.0f, 1.0f, 0.0f));
        block.Projection = glm::perspective(glm::radians(rig.FovYDeg), 1.0f, rig.NearClip, rig.FarClip);
        block.CameraPosition = rig.EyePos;

        // Save the host's stencil-test state. The host renderer manages
        // its own depth + cull modes per draw so we don't need to restore
        // those, but stencil state is sticky and we don't want to leak it.
        const bool wasStencilTestEnabled = RenderCommand::IsStencilTestEnabled();
        if (wasStencilTestEnabled)
            RenderCommand::DisableStencilTest();
        RenderCommand::SetDepthTest(true);
        RenderCommand::EnableCulling();
        RenderCommand::BackCull();

        s_Framebuffer->Bind();
        RenderCommand::SetViewport(0, 0, kThumbnailSize, kThumbnailSize);
        RenderCommand::SetClearColor({ 0.15f, 0.16f, 0.18f, 1.0f });
        RenderCommand::ClearColorAndDepth();

        s_PreviewUBO->SetData(&block, sizeof(PreviewBlock));
        s_PreviewUBO->Bind();

        s_Shader->Bind();

        auto vao = mesh->GetVertexArray();
        if (!vao)
        {
            OLO_CORE_WARN("AssetPreviewRenderer::RenderInto: mesh has no VAO");
            s_Framebuffer->Unbind();
            if (wasStencilTestEnabled)
                RenderCommand::EnableStencilTest();
            return nullptr;
        }
        vao->Bind();
        RenderCommand::DrawIndexed(vao);

        Ref<Texture2D> target = CreateTargetTexture();
        if (target)
        {
            const u32 fbColor = s_Framebuffer->GetColorAttachmentRendererID(0);
            RenderCommand::CopyImageSubDataFull(
                fbColor, RendererAPI::TextureTargetType::Texture2D, 0, 0,
                target->GetRendererID(), RendererAPI::TextureTargetType::Texture2D, 0, 0,
                kThumbnailSize, kThumbnailSize);
        }
        else
        {
            OLO_CORE_ERROR("AssetPreviewRenderer::RenderInto: could not allocate target Texture2D");
        }

        s_Framebuffer->Unbind();
        if (wasStencilTestEnabled)
            RenderCommand::EnableStencilTest();

        return target;
    }

    Ref<Texture2D> AssetPreviewRenderer::RenderMaterialPreview(const Ref<MaterialAsset>& material)
    {
        if (!s_Initialised)
        {
            OLO_CORE_WARN("AssetPreviewRenderer::RenderMaterialPreview: not initialised");
            return nullptr;
        }
        if (!material)
            return nullptr;
        if (!EnsureSphereMesh())
            return nullptr;

        PreviewBlock block;
        FillMaterialBlock(material, block);
        return RenderInto(s_SphereMesh, MakeSphereRig(), block);
    }

    Ref<Texture2D> AssetPreviewRenderer::RenderMeshPreview(const Ref<Mesh>& mesh,
                                                           const Ref<MaterialAsset>& material)
    {
        if (!s_Initialised)
        {
            OLO_CORE_WARN("AssetPreviewRenderer::RenderMeshPreview: not initialised");
            return nullptr;
        }
        if (!mesh || !mesh->IsValid())
            return nullptr;

        PreviewBlock block;
        FillMaterialBlock(material, block);
        return RenderInto(mesh, MakeMeshRig(mesh), block);
    }
} // namespace OloEngine

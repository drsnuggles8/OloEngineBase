#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glm/glm.hpp>

#include <string>

namespace OloEngine
{
    class MaterialAsset;

    // -----------------------------------------------------------------------
    // AssetPreviewRenderer
    //
    // Renders thumbnails of `MaterialAsset` / `Mesh` assets into RGBA8
    // `Texture2D` images. The renderer owns a persistent framebuffer and a
    // dedicated `MaterialPreview.glsl` shader so it can be driven outside
    // the Renderer3D frame graph (the full pipeline owns its own output FB
    // and is too heavyweight for tiny thumbnails).
    //
    // Lives in the engine rather than the editor because the editor
    // material/mesh inspectors will eventually want the same offscreen
    // preview, and because keeping it engine-side lets the renderer tests
    // exercise it under `RendererAttachedTest`.
    //
    // Lifetime: Initialize() must be called after `Renderer::Init` has
    // brought up a GL context. Shutdown() releases the FB / shader / sphere.
    // Both are idempotent.
    //
    // Shader path: Initialize() takes an optional shader filepath so the
    // engine doesn't bake in the editor's `assets/shaders/...` layout. The
    // default is the path Renderer3D itself uses (cwd-relative), which
    // works when the host cwd is `OloEditor/` (editor + tests both arrange
    // for that — see `RenderPropertyFixture::ChangeToOloEditorDir`).
    // -----------------------------------------------------------------------
    class AssetPreviewRenderer
    {
      public:
        // Default thumbnail size. The Content Browser only ever asks for
        // one size today, so this is hardcoded instead of parameterised
        // per call — keeps the cached framebuffer reusable across requests.
        static constexpr u32 kThumbnailSize = 256;

        // Shader filepath used by default. Relative to cwd, which is
        // assumed to be `OloEditor/` at runtime.
        static const std::string& GetDefaultShaderPath();

        static void Initialize(const std::string& shaderPath = GetDefaultShaderPath());
        static void Shutdown();
        [[nodiscard]] static bool IsInitialized();

        // Render a sphere preview of the given material into a fresh
        // RGBA8 `Texture2D` and return it. Returns null if the renderer
        // is not initialised, the material is null, or the underlying
        // GL pipeline cannot produce an image (no GPU available, shader
        // failed to compile, etc.).
        //
        // The returned texture is owned by the caller — typically the
        // thumbnail cache, which keeps it alive for the lifetime of the
        // cache entry.
        [[nodiscard]] static Ref<Texture2D> RenderMaterialPreview(const Ref<MaterialAsset>& material);

        // Render an arbitrary mesh under a supplied material. When
        // `material` is null the mesh is rendered with a neutral default
        // material (mid-grey, mid-roughness, non-metallic) — useful for
        // mesh-asset thumbnails where the source file does not bundle a
        // material reference.
        //
        // Camera framing is computed from the mesh's local-space AABB so
        // the model fills the same ~60% screen-fraction as the sphere
        // preview, regardless of source units.
        [[nodiscard]] static Ref<Texture2D> RenderMeshPreview(const Ref<Mesh>& mesh,
                                                              const Ref<MaterialAsset>& material = nullptr);

      private:
        struct PreviewBlock
        {
            // std140 layout mirrors `MaterialPreview.glsl::PreviewBlock`.
            // Padding is explicit so MSVC's struct layout matches the
            // GPU side regardless of compiler heuristics.
            glm::mat4 Model{ 1.0f };
            glm::mat4 View{ 1.0f };
            glm::mat4 Projection{ 1.0f };
            glm::vec3 CameraPosition{ 0.0f };
            f32 _pad0{ 0.0f };
            glm::vec3 AlbedoFactor{ 0.7f };
            f32 MetallicFactor{ 0.0f };
            f32 RoughnessFactor{ 0.45f };
            f32 EmissiveFactor{ 0.0f };
            i32 UseAlbedoMap{ 0 };
            i32 UseMetalnessMap{ 0 };
            i32 UseRoughnessMap{ 0 };
            i32 _pad1{ 0 };
            i32 _pad2{ 0 };
            i32 _pad3{ 0 };
        };
        static_assert(sizeof(PreviewBlock) % 16 == 0, "PreviewBlock must be 16-byte aligned for std140");

        // Build the unit-icosphere and 1×1 white default-texture. Return
        // true on success so `Initialize()` can refuse to mark the
        // renderer as ready when a fallback resource failed to allocate.
        [[nodiscard]] static bool EnsureSphereMesh();
        [[nodiscard]] static bool EnsureDefaultWhiteTexture();
        static Ref<Texture2D> CreateTargetTexture();
        static void FillMaterialBlock(const Ref<MaterialAsset>& material, PreviewBlock& block);
        static void FillDefaultMaterialBlock(PreviewBlock& block);
        // Render an arbitrary mesh into the persistent FB with a camera /
        // model placement chosen for `mesh`. Materials are bound by the
        // caller (so sphere-vs-arbitrary-mesh paths reuse the same code).
        [[nodiscard]] static Ref<Texture2D> RenderInto(const Ref<Mesh>& mesh,
                                                       const struct PreviewCameraRig& rig,
                                                       PreviewBlock& block);

      private:
        static Ref<Framebuffer> s_Framebuffer;
        static Ref<Shader> s_Shader;
        static Ref<Mesh> s_SphereMesh;
        static Ref<UniformBuffer> s_PreviewUBO;
        // 1×1 white RGBA8. Bound to every sampler slot the shader exposes
        // before a render so unused samplers always have *some* complete
        // texture attached — required by GL on many drivers even when the
        // shader's branch wouldn't read the sampler.
        static Ref<Texture2D> s_DefaultWhite;
        static bool s_Initialised;
    };
} // namespace OloEngine

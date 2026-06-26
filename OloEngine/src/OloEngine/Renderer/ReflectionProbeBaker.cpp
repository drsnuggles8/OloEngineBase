#include "OloEnginePCH.h"
#include "OloEngine/Renderer/ReflectionProbeBaker.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Instrumentor.h"
#include "OloEngine/Renderer/Camera/Camera.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Scene.h"

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <limits>
#include <vector>

namespace OloEngine
{
    i32 SelectDominantReflectionProbe(const glm::vec3& cameraPosition,
                                      std::span<const ReflectionProbeRef> probes)
    {
        f32 bestDistSq = std::numeric_limits<f32>::max();
        i32 bestIndex = -1;
        for (sizet i = 0; i < probes.size(); ++i)
        {
            auto const& p = probes[i];
            glm::vec3 const delta = cameraPosition - p.Position;
            f32 const distSq = glm::dot(delta, delta);
            if (f32 const radiusSq = p.InfluenceRadius * p.InfluenceRadius; distSq > radiusSq)
            {
                continue;
            }
            if (distSq < bestDistSq)
            {
                bestDistSq = distSq;
                bestIndex = static_cast<i32>(i);
            }
        }
        return bestIndex;
    }

    namespace
    {
        // Cubemap face look directions and up vectors. Order matches
        // GL_TEXTURE_CUBE_MAP_POSITIVE_X .. NEGATIVE_Z and the bindings used in
        // IBLPrecompute::RenderToCubemap, so prefilter / irradiance generation
        // see the same orientation as our captured faces.
        constexpr glm::vec3 s_FaceTargets[6] = {
            { 1.0f, 0.0f, 0.0f },
            { -1.0f, 0.0f, 0.0f },
            { 0.0f, 1.0f, 0.0f },
            { 0.0f, -1.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f },
            { 0.0f, 0.0f, -1.0f },
        };

        constexpr glm::vec3 s_FaceUps[6] = {
            { 0.0f, -1.0f, 0.0f },
            { 0.0f, -1.0f, 0.0f },
            { 0.0f, 0.0f, 1.0f },
            { 0.0f, 0.0f, -1.0f },
            { 0.0f, -1.0f, 0.0f },
            { 0.0f, -1.0f, 0.0f },
        };
    } // namespace

    Ref<TextureCubemap> ReflectionProbeBaker::CaptureSceneCubemap(Ref<Scene>& scene,
                                                                  const glm::vec3& position,
                                                                  u32 resolution)
    {
        OLO_PROFILE_FUNCTION();

        // Defense in depth: BakeProbe already clamps, but this is private and
        // any future caller could pass 0 (GL undefined) or huge values (OOM).
        // Literal bounds match BakeProbe and SceneSerializer's deserialize clamp.
        u32 const clampedResolution = std::clamp(resolution, 16u, 2048u);
        if (clampedResolution != resolution)
        {
            OLO_CORE_WARN("ReflectionProbeBaker::CaptureSceneCubemap: resolution {} clamped to {}", resolution, clampedResolution);
        }

        CubemapSpecification cubemapSpec;
        cubemapSpec.Width = clampedResolution;
        cubemapSpec.Height = clampedResolution;
        cubemapSpec.Format = ImageFormat::RGBA32F;
        cubemapSpec.GenerateMips = true;
        auto cubemap = TextureCubemap::Create(cubemapSpec);
        if (!cubemap)
        {
            OLO_CORE_ERROR("ReflectionProbeBaker: failed to allocate cubemap ({}x{} RGBA32F)", clampedResolution, clampedResolution);
            return nullptr;
        }

        // The scene is rendered through the full Renderer3D render graph so the
        // probe captures the same lighting / skybox / emissive surfaces as the
        // live view. The graph writes into its OWN targets — never an
        // externally-bound framebuffer — so we resize the graph to the probe
        // resolution, render each face, and read back the graph's HDR SceneColor
        // (the MRT-root scene-pass output: linear radiance BEFORE tone mapping
        // and post/TAA, which is exactly what the IBL convolution wants).
        //
        // The previous approach bound a local capture FBO and read it back; that
        // FBO was never a render target (RenderScene3D targets the graph), so the
        // bake captured only the cleared (black) FBO — every probe baked black.
        u32 savedWidth = 0;
        u32 savedHeight = 0;
        if (auto const preBakeSceneFb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor))
        {
            savedWidth = preBakeSceneFb->GetSpecification().Width;
            savedHeight = preBakeSceneFb->GetSpecification().Height;
        }

        // Square render targets so the 90° FOV per face covers the whole image.
        Renderer3D::OnWindowResize(clampedResolution, clampedResolution);

        // 90° FOV per face — 6 faces cover the full sphere with no overlap.
        // Far plane is generous; near plane small enough to capture nearby
        // walls when the probe sits inside an enclosed room.
        Camera const captureCamera(glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 1000.0f));

        // Prime the render graph at the new size before capturing: the first
        // render(s) after a graph resize come back black (the graph needs a
        // frame to settle into the reallocated targets). Without this every
        // captured face is black even though RenderScene3D works fine at the
        // unchanged live-view size.
        {
            glm::mat4 const warmView = glm::inverse(glm::lookAt(position, position + s_FaceTargets[0], s_FaceUps[0]));
            scene->RenderScene3D(captureCamera, warmView);
        }

        // Preallocate readback buffer — 4 floats per pixel. The destination
        // cubemap face is RGBA32F; SceneColor RT0 is read back as RGBA float
        // regardless of its own internal format.
        sizet const faceBytes = static_cast<sizet>(clampedResolution) * clampedResolution * 4u * sizeof(f32);
        std::vector<f32> pixelBuffer(faceBytes / sizeof(f32));

        bool captureOk = true;
        for (u32 face = 0; face < 6; ++face)
        {
            glm::mat4 const view = glm::lookAt(position, position + s_FaceTargets[face], s_FaceUps[face]);
            glm::mat4 const transform = glm::inverse(view);

            scene->RenderScene3D(captureCamera, transform);

            // Read back this face's lit HDR radiance from the graph's SceneColor
            // RT0. glGetTextureImage reads the texture directly (no FBO-bound
            // restriction) and lets the driver pick its preferred path.
            auto const sceneFb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            if (!sceneFb)
            {
                OLO_CORE_ERROR("ReflectionProbeBaker: SceneColor framebuffer unavailable while capturing face {}", face);
                captureOk = false;
                break;
            }

            u32 const colorAttachmentID = sceneFb->GetColorAttachmentRendererID(0);
            glGetTextureImage(colorAttachmentID, 0, GL_RGBA, GL_FLOAT,
                              static_cast<GLsizei>(faceBytes),
                              pixelBuffer.data());

            // Upload into the cubemap face. SetFaceData triggers
            // glGenerateTextureMipmap on every call; redundant on faces
            // 0-4 but harmless and keeps the API surface small. Bake is an
            // editor-time interactive action so the cost is acceptable.
            cubemap->SetFaceData(face, pixelBuffer.data(), static_cast<u32>(faceBytes));
        }

        // Restore the render-graph size for the live view (the bake squashed it
        // down to the probe resolution above).
        if (savedWidth != 0 && savedHeight != 0)
        {
            Renderer3D::OnWindowResize(savedWidth, savedHeight);
        }

        if (!captureOk)
        {
            return nullptr;
        }

        return cubemap;
    }

    bool ReflectionProbeBaker::BakeProbe(Ref<Scene>& scene,
                                         const glm::vec3& position,
                                         ReflectionProbeComponent& probe)
    {
        OLO_PROFILE_FUNCTION();

        if (!scene)
        {
            OLO_CORE_ERROR("ReflectionProbeBaker::BakeProbe: scene is null");
            return false;
        }

        // Clamp resolution defensively — the component's serializer already
        // clamps loaded values, but in-memory edits may bypass that path.
        u32 const resolution = std::clamp(probe.m_Resolution, 16u, 2048u);

        auto cubemap = CaptureSceneCubemap(scene, position, resolution);
        if (!cubemap)
        {
            return false;
        }

        // Hand off to EnvironmentMap which runs the IBL chain (irradiance +
        // prefiltered specular + BRDF LUT). The same plumbing the global
        // EnvironmentMapComponent uses — Renderer3D::SetGlobalIBL can consume
        // it without modification.
        //
        // Opt OUT of the IBL disk cache. A captured probe cubemap is a render
        // target whose pixels depend on the scene, probe position and time, but
        // it carries a constant debug name ("Generated Cubemap"), so the cache
        // key (name + dimensions) collides across every bake at the same
        // resolution. A cache hit would serve a PREVIOUS bake's IBL for freshly
        // captured pixels — re-baking, baking a second probe, or baking after a
        // scene change would all silently reuse stale (even all-black) maps.
        // Procedural/StarNest skies opt out for the identical reason.
        IBLConfiguration iblConfig;
        iblConfig.UseDiskCache = false;
        auto environment = EnvironmentMap::CreateFromCubemap(cubemap, iblConfig);
        if (!environment || !environment->HasIBL())
        {
            OLO_CORE_ERROR("ReflectionProbeBaker::BakeProbe: IBL generation failed");
            return false;
        }

        probe.m_BakedEnvironment = environment;
        probe.m_NeedsBake = false;

        OLO_CORE_INFO("Baked reflection probe at ({}, {}, {}) — cubemap {}x{}",
                      position.x, position.y, position.z, resolution, resolution);
        return true;
    }
} // namespace OloEngine

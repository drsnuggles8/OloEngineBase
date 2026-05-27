#include "OloEngine/Renderer/ReflectionProbeBaker.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Instrumentor.h"
#include "OloEngine/Renderer/Camera/Camera.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/RenderCommand.h"
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
            f32 const radiusSq = p.InfluenceRadius * p.InfluenceRadius;
            if (distSq > radiusSq)
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
            OLO_CORE_ERROR("ReflectionProbeBaker: failed to allocate cubemap ({}x{} RGBA32F)", resolution, resolution);
            return nullptr;
        }

        FramebufferSpecification fbSpec;
        fbSpec.Width = clampedResolution;
        fbSpec.Height = clampedResolution;
        fbSpec.Attachments = { FramebufferTextureFormat::RGBA32F, FramebufferTextureFormat::Depth };
        auto fbo = Framebuffer::Create(fbSpec);
        if (!fbo)
        {
            OLO_CORE_ERROR("ReflectionProbeBaker: failed to allocate capture framebuffer");
            return nullptr;
        }

        // 90° FOV per face — 6 faces cover the full sphere with no overlap.
        // Far plane is generous; near plane small enough to capture nearby
        // walls when the probe sits inside an enclosed room.
        Camera const captureCamera(glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 1000.0f));

        // Preallocate readback buffer — 4 floats per pixel (RGBA32F).
        sizet const faceBytes = static_cast<sizet>(clampedResolution) * clampedResolution * 4u * sizeof(f32);
        std::vector<f32> pixelBuffer(faceBytes / sizeof(f32));

        for (u32 face = 0; face < 6; ++face)
        {
            glm::mat4 const view = glm::lookAt(position, position + s_FaceTargets[face], s_FaceUps[face]);
            glm::mat4 const transform = glm::inverse(view);

            fbo->Bind();
            RenderCommand::SetViewport(0, 0, clampedResolution, clampedResolution);
            fbo->ClearAllAttachments(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));

            scene->RenderScene3D(captureCamera, transform);

            // Read back the rendered face. Using glGetTextureImage avoids
            // the FBO-bound restriction of glReadPixels and lets the driver
            // pick its preferred path.
            u32 const colorAttachmentID = fbo->GetColorAttachmentRendererID(0);
            glGetTextureImage(colorAttachmentID, 0, GL_RGBA, GL_FLOAT,
                              static_cast<GLsizei>(faceBytes),
                              pixelBuffer.data());

            fbo->Unbind();

            // Upload into the cubemap face. SetFaceData triggers
            // glGenerateTextureMipmap on every call; redundant on faces
            // 0-4 but harmless and keeps the API surface small. Bake is an
            // editor-time interactive action so the cost is acceptable.
            cubemap->SetFaceData(face, pixelBuffer.data(), static_cast<u32>(faceBytes));
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
        auto environment = EnvironmentMap::CreateFromCubemap(cubemap);
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

#include "OloEngine/Renderer/LightProbeBaker.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Renderer/Camera/Camera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Debug/Instrumentor.h"

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

namespace OloEngine
{
    // Cubemap face directions: +X, -X, +Y, -Y, +Z, -Z
    static const glm::vec3 s_CubemapTargets[6] = {
        { 1.0f, 0.0f, 0.0f },
        { -1.0f, 0.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f },
        { 0.0f, -1.0f, 0.0f },
        { 0.0f, 0.0f, 1.0f },
        { 0.0f, 0.0f, -1.0f }
    };

    static const glm::vec3 s_CubemapUps[6] = {
        { 0.0f, -1.0f, 0.0f },
        { 0.0f, -1.0f, 0.0f },
        { 0.0f, 0.0f, 1.0f },
        { 0.0f, 0.0f, -1.0f },
        { 0.0f, -1.0f, 0.0f },
        { 0.0f, -1.0f, 0.0f }
    };

    void LightProbeBaker::RenderCubemapAtPosition(
        Ref<Scene>& scene,
        const glm::vec3& position,
        u32 resolution,
        std::vector<glm::vec3>& outPixels)
    {
        OLO_PROFILE_FUNCTION();

        auto const totalPixels = static_cast<size_t>(resolution) * resolution * 6;
        outPixels.resize(totalPixels);

        FramebufferSpecification spec;
        spec.Width = resolution;
        spec.Height = resolution;
        spec.Attachments = { FramebufferTextureFormat::RGBA16F, FramebufferTextureFormat::Depth };
        auto fbo = Framebuffer::Create(spec);

        Camera const captureCamera(glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 1000.0f));

        // Temporary buffer for RGBA16F readback (4 floats per pixel)
        std::vector<f32> rgbaBuffer(static_cast<size_t>(resolution) * resolution * 4);

        for (u32 face = 0; face < 6; ++face)
        {
            glm::mat4 const view = glm::lookAt(position, position + s_CubemapTargets[face], s_CubemapUps[face]);
            glm::mat4 const transform = glm::inverse(view);

            fbo->Bind();
            RenderCommand::SetViewport(0, 0, resolution, resolution);
            fbo->ClearAllAttachments(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));

            // Render the full scene from this cubemap face's perspective
            scene->RenderScene3D(captureCamera, transform);

            // Read back RGBA16F pixel data from the color attachment
            u32 const colorAttachmentID = fbo->GetColorAttachmentRendererID(0);
            glGetTextureImage(colorAttachmentID, 0, GL_RGBA, GL_FLOAT,
                              static_cast<GLsizei>(rgbaBuffer.size() * sizeof(f32)),
                              rgbaBuffer.data());

            fbo->Unbind();

            // Convert RGBA to RGB and store
            auto const faceOffset = static_cast<size_t>(face) * resolution * resolution;
            for (size_t i = 0; i < static_cast<size_t>(resolution) * resolution; ++i)
            {
                outPixels[faceOffset + i] = glm::vec3(
                    rgbaBuffer[i * 4 + 0],
                    rgbaBuffer[i * 4 + 1],
                    rgbaBuffer[i * 4 + 2]);
            }
        }
    }

    SHCoefficients LightProbeBaker::ProjectToSH(
        const std::vector<glm::vec3>& cubemapPixels,
        u32 resolution)
    {
        OLO_PROFILE_FUNCTION();

        SHCoefficients result;
        result.Zero();

        if (resolution == 0)
        {
            OLO_CORE_ERROR("LightProbeBaker::ProjectToSH: resolution must be > 0");
            return result;
        }

        auto const expectedPixels = static_cast<size_t>(6) * resolution * resolution;
        if (cubemapPixels.size() < expectedPixels)
        {
            OLO_CORE_ERROR("LightProbeBaker::ProjectToSH: cubemapPixels size {} < expected {}", cubemapPixels.size(), expectedPixels);
            return result;
        }

        f32 totalWeight = 0.0f;
        f32 const texelSize = 2.0f / static_cast<f32>(resolution);

        for (u32 face = 0; face < 6; ++face)
        {
            for (u32 y = 0; y < resolution; ++y)
            {
                for (u32 x = 0; x < resolution; ++x)
                {
                    // Map texel to [-1, 1] range
                    f32 const u = (static_cast<f32>(x) + 0.5f) * texelSize - 1.0f;
                    f32 const v = (static_cast<f32>(y) + 0.5f) * texelSize - 1.0f;

                    // Convert to world direction based on face
                    glm::vec3 dir(0.0f);
                    switch (face)
                    {
                        case 0:
                            dir = glm::vec3(1.0f, -v, -u);
                            break; // +X
                        case 1:
                            dir = glm::vec3(-1.0f, -v, u);
                            break; // -X
                        case 2:
                            dir = glm::vec3(u, 1.0f, v);
                            break; // +Y
                        case 3:
                            dir = glm::vec3(u, -1.0f, -v);
                            break; // -Y
                        case 4:
                            dir = glm::vec3(u, -v, 1.0f);
                            break; // +Z
                        case 5:
                            dir = glm::vec3(-u, -v, -1.0f);
                            break; // -Z
                    }
                    dir = glm::normalize(dir);

                    // Solid angle approximation for cubemap texel
                    f32 const distSq = u * u + v * v + 1.0f;
                    f32 const weight = 4.0f / (std::sqrt(distSq) * distSq);

                    auto const idx = static_cast<size_t>(face) * resolution * resolution + static_cast<size_t>(y) * resolution + x;
                    glm::vec3 const color = cubemapPixels[idx];

                    // Evaluate SH basis and accumulate
                    auto basis = SHBasis::Evaluate(dir);
                    for (u32 i = 0; i < SH_COEFFICIENT_COUNT; ++i)
                    {
                        result.Coefficients[i] += color * (basis[i] * weight);
                    }
                    totalWeight += weight;
                }
            }
        }

        // Normalize
        if (totalWeight > 0.0f)
        {
            f32 const norm = (4.0f * glm::pi<f32>()) / totalWeight;
            result.Scale(norm);
        }

        return result;
    }

    SHCoefficients LightProbeBaker::BakeProbeAtPosition(
        Ref<Scene>& scene,
        const glm::vec3& position,
        u32 cubemapResolution,
        bool* outValid)
    {
        OLO_PROFILE_FUNCTION();

        std::vector<glm::vec3> pixels;
        RenderCubemapAtPosition(scene, position, cubemapResolution, pixels);

        SHCoefficients sh = ProjectToSH(pixels, cubemapResolution);

        if (outValid)
        {
            // Simple heuristic: if the probe captures mostly black (inside geometry),
            // mark as invalid
            f32 energy = glm::dot(sh.Coefficients[0], glm::vec3(1.0f));
            *outValid = energy > 0.001f;
        }

        return sh;
    }

    void LightProbeBaker::BakeVolume(
        Ref<Scene>& scene,
        LightProbeVolumeComponent& volume,
        Ref<LightProbeVolumeAsset>& asset,
        u32 cubemapResolution,
        const ProbeBakeProgressCallback& progress)
    {
        OLO_PROFILE_FUNCTION();

        if (!asset)
        {
            return;
        }

        // Sync asset parameters from component
        asset->BoundsMin = volume.m_BoundsMin;
        asset->BoundsMax = volume.m_BoundsMax;
        asset->Resolution = volume.m_Resolution;
        asset->Spacing = volume.m_Spacing;
        asset->AllocateCoefficients();

        i32 const totalProbes = volume.GetTotalProbeCount();
        glm::vec3 const extent = volume.m_BoundsMax - volume.m_BoundsMin;

        for (i32 z = 0; z < volume.m_Resolution.z; ++z)
        {
            for (i32 y = 0; y < volume.m_Resolution.y; ++y)
            {
                for (i32 x = 0; x < volume.m_Resolution.x; ++x)
                {
                    // Calculate probe world position
                    glm::vec3 t(0.0f);
                    if (volume.m_Resolution.x > 1)
                        t.x = static_cast<f32>(x) / static_cast<f32>(volume.m_Resolution.x - 1);
                    if (volume.m_Resolution.y > 1)
                        t.y = static_cast<f32>(y) / static_cast<f32>(volume.m_Resolution.y - 1);
                    if (volume.m_Resolution.z > 1)
                        t.z = static_cast<f32>(z) / static_cast<f32>(volume.m_Resolution.z - 1);

                    glm::vec3 const probePos = volume.m_BoundsMin + extent * t;

                    bool valid = true;
                    SHCoefficients sh = BakeProbeAtPosition(scene, probePos, cubemapResolution, &valid);

                    i32 const linearIdx = volume.GridIndex(x, y, z);
                    asset->SetProbeData(linearIdx, sh, valid ? 1.0f : 0.0f);

                    if (progress)
                    {
                        progress(linearIdx + 1, totalProbes);
                    }
                }
            }
        }

        volume.m_Dirty = true;
    }
} // namespace OloEngine

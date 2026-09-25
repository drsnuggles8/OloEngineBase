// OLO_TEST_LAYER: L2
// =============================================================================
// PointLightEvaluatorParityGpuTest.cpp — issue #1457.
//
// Forward shades a point light through the light LOOP (oloSkinLightContribution
// Split over the MultiLightUBO entry); Forward+ and Deferred shade it through
// the clustered TILE evaluator (fplusEvaluateTileLightsSplit over the Forward+
// SSBO). The probe (assets/shaders/tests/ShaderUnit_PointLightEvaluatorParity
// .glsl) runs both on the same surface, with both lights packed from ONE
// canonical record through the production adapters — so a drift in either the
// shader math or the UBO/SSBO packing shows up as a number here.
//
// #1457 reported Deferred shading DDGITest's red point light differently from
// Forward. Case 0 is that light. The evaluators agree; the live gap was
// screen-space AO, which Forward multiplies into the composed colour and
// Deferred applies to the ambient term alone (#1452) — see
// DeferredPointLightParityEvidenceTest.cpp for the frame-level half.
//
// The last case is the NEGATIVE CONTROL: the SSBO copy of the light carries a
// different attenuation coefficient, and the comparison must see it. Without it
// "the two rows agree" would also be what a probe that evaluated one function
// twice produces.
//
// Skips cleanly without a GL 4.6 context, like every probe fixture here.
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneLightAdapter.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneTypes.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // Mirrors ProbeSurface in the probe (std430, four vec4s).
        struct ProbeSurface
        {
            glm::vec4 PositionAndRoughness;
            glm::vec4 NormalAndMetallic;
            glm::vec4 CameraAndModel;
            glm::vec4 Albedo;
        };

        struct ProbeCase
        {
            const char* Name;
            GPUSceneLightInput Light;
            ProbeSurface Surface;
            bool ExpectLit;
        };

        constexpr f32 kLegacy = 0.0f;
        constexpr f32 kClosureV2 = 1.0f;

        GPUSceneLightInput PointLight(const glm::vec3& position, const glm::vec3& color, f32 intensity, f32 range,
                                      f32 attenuation)
        {
            GPUSceneLightInput input;
            input.m_Type = std::to_underlying(GPUSceneLightType::Point);
            input.m_Position = position;
            input.m_Color = color;
            input.m_Intensity = intensity;
            input.m_Range = range;
            input.m_Attenuation = attenuation;
            return input;
        }

        ProbeSurface Surface(const glm::vec3& position, const glm::vec3& normal, const glm::vec3& camera,
                             const glm::vec3& albedo, f32 roughness, f32 metallic, f32 model)
        {
            return ProbeSurface{ glm::vec4(position, roughness), glm::vec4(normal, metallic), glm::vec4(camera, model),
                                 glm::vec4(albedo, 1.0f) };
        }

        std::vector<ProbeCase> Cases()
        {
            const glm::vec3 grey(0.5f);
            return {
                // DDGITest.olo's red point light on its floor, seen from the scene camera.
                { "DDGITest red light on the floor",
                  PointLight({ -5.0f, 3.0f, 0.0f }, { 1.0f, 0.2f, 0.1f }, 8.0f, 15.0f, 1.0f),
                  Surface({ -3.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 5.0f, 15.0f }, grey, 0.9f, 0.0f, kLegacy),
                  true },
                { "default attenuation, grazing wall",
                  PointLight({ 0.0f, 1.0f, 0.3f }, { 1.0f, 1.0f, 1.0f }, 12.0f, 10.0f, 2.0f),
                  Surface({ 2.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.5f, 6.0f }, { 0.8f, 0.7f, 0.6f }, 0.5f,
                          0.0f, kLegacy),
                  true },
                { "just inside the range window",
                  PointLight({ 0.0f, 2.0f, 0.0f }, { 0.3f, 0.6f, 1.0f }, 20.0f, 5.0f, 1.0f),
                  Surface({ 4.8f, 2.0f, 0.0f }, { -1.0f, 0.0f, 0.0f }, { 2.0f, 3.0f, 4.0f }, grey, 0.6f, 0.0f, kLegacy),
                  true },
                { "just outside the range window",
                  PointLight({ 0.0f, 2.0f, 0.0f }, { 0.3f, 0.6f, 1.0f }, 20.0f, 5.0f, 1.0f),
                  Surface({ 5.2f, 2.0f, 0.0f }, { -1.0f, 0.0f, 0.0f }, { 2.0f, 3.0f, 4.0f }, grey, 0.6f, 0.0f, kLegacy),
                  false },
                { "surface facing away",
                  PointLight({ 0.0f, 3.0f, 0.0f }, { 1.0f, 1.0f, 1.0f }, 10.0f, 10.0f, 2.0f),
                  Surface({ 0.0f, 0.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, -2.0f, 3.0f }, grey, 0.5f, 0.0f, kLegacy),
                  false },
                { "glossy highlight in the mirror direction",
                  PointLight({ -2.0f, 2.0f, 0.0f }, { 1.0f, 0.9f, 0.8f }, 15.0f, 12.0f, 2.0f),
                  Surface({ 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 2.0f, 2.0f, 0.0f }, { 0.2f, 0.2f, 0.25f }, 0.15f,
                          0.0f, kLegacy),
                  true },
                { "metal",
                  PointLight({ 1.0f, 2.0f, 1.0f }, { 1.0f, 1.0f, 1.0f }, 9.0f, 8.0f, 2.0f),
                  Surface({ 0.0f, 0.0f, 0.0f }, { 0.2f, 1.0f, 0.1f }, { -1.0f, 2.0f, 2.0f }, { 1.0f, 0.78f, 0.34f }, 0.35f,
                          1.0f, kLegacy),
                  true },
                { "closure V2",
                  PointLight({ -1.0f, 2.5f, 1.5f }, { 0.9f, 1.0f, 0.8f }, 11.0f, 9.0f, 2.0f),
                  Surface({ 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 1.5f, 2.0f, 3.0f }, { 0.6f, 0.4f, 0.3f }, 0.4f,
                          0.0f, kClosureV2),
                  true },
                // NEGATIVE CONTROL — packed below with a different SSBO attenuation.
                { "control: SSBO attenuation differs from the UBO",
                  PointLight({ -5.0f, 3.0f, 0.0f }, { 1.0f, 0.2f, 0.1f }, 8.0f, 15.0f, 1.0f),
                  Surface({ -3.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 5.0f, 15.0f }, grey, 0.9f, 0.0f, kLegacy),
                  true },
            };
        }

        u32 CreateBuffer(GLenum target, u32 binding, const void* data, sizet bytes)
        {
            u32 buffer = 0;
            ::glCreateBuffers(1, &buffer);
            ::glNamedBufferData(buffer, static_cast<GLsizeiptr>(bytes), data, GL_STATIC_DRAW);
            ::glBindBufferBase(target, binding, buffer);
            return buffer;
        }
    } // namespace

    TEST(PointLightEvaluatorParityGpu, LightLoopAndClusteredTilesShadeOnePointLightAlike)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const std::vector<ProbeCase> cases = Cases();
        const auto columns = static_cast<u32>(cases.size());
        const u32 controlColumn = columns - 1u;

        // Both rows' lights from ONE canonical record, through the adapters the
        // scene uses (Scene::ProcessScene3DSharedLogic).
        std::vector<UBOStructures::MultiLightData> loopLights;
        std::vector<GPUPointLight> tileLights;
        std::vector<ProbeSurface> surfaces;
        for (const ProbeCase& c : cases)
        {
            const GPUSceneLight canonical = EncodeGPUSceneLight(c.Light, glm::vec3(0.0f));
            loopLights.push_back(GPUSceneLightAdapter::ToMultiLightData(canonical));
            tileLights.push_back(GPUSceneLightAdapter::ToForwardPlusPoint(canonical));
            surfaces.push_back(c.Surface);
        }
        const f32 packedQuadratic = tileLights[controlColumn].ShadowAndAttenuation.y;
        tileLights[controlColumn].ShadowAndAttenuation.y = packedQuadratic * 2.0f + 0.5f;

        // One single-light cluster per column: countX = columns, one row of
        // tiles, one depth slice (scale = bias = 0).
        std::vector<glm::uvec2> grid;
        std::vector<u32> indices;
        for (u32 i = 0; i < columns; ++i)
        {
            grid.emplace_back(i, 1u);
            indices.push_back(i); // type tag 0 = point
        }
        UBOStructures::ForwardPlusUBO params{};
        params.Params = glm::uvec4(columns, 1u, 1u, 1u);
        params.TileScale = glm::vec4(1.0f, 0.5f, static_cast<f32>(columns), 2.0f);
        params.DepthSlicing = glm::vec4(0.0f, 0.0f, 0.1f, 100.0f);

        const std::array<GPUSpotLight, 1> noSpots{};
        const std::array<GPUSphereAreaLight, 1> noSpheres{};

        FramebufferSpecification spec{};
        spec.Width = columns;
        spec.Height = 2;
        spec.Attachments = { FramebufferTextureFormat::RGBA32F };
        Ref<Framebuffer> output = Framebuffer::Create(spec);
        Ref<Shader> shader = Shader::Create("assets/shaders/tests/ShaderUnit_PointLightEvaluatorParity.glsl");
        ASSERT_TRUE(shader) << "the probe shader was not created at all";
        ASSERT_TRUE(shader->IsReady())
            << "ShaderUnit_PointLightEvaluatorParity.glsl did not compile — check OloEngine.log";

        std::vector<f32> pixels;
        {
            GLStateGuard guard("PointLightEvaluatorParity", GLStateGuard::Policy::Restore);
            const std::array<u32, 8> buffers = {
                CreateBuffer(GL_SHADER_STORAGE_BUFFER, 9, tileLights.data(), tileLights.size() * sizeof(GPUPointLight)),
                CreateBuffer(GL_SHADER_STORAGE_BUFFER, 10, noSpots.data(), sizeof(noSpots)),
                CreateBuffer(GL_SHADER_STORAGE_BUFFER, 11, indices.data(), indices.size() * sizeof(u32)),
                CreateBuffer(GL_SHADER_STORAGE_BUFFER, 12, grid.data(), grid.size() * sizeof(glm::uvec2)),
                CreateBuffer(GL_SHADER_STORAGE_BUFFER, 18, noSpheres.data(), sizeof(noSpheres)),
                CreateBuffer(GL_SHADER_STORAGE_BUFFER, 40, loopLights.data(),
                             loopLights.size() * sizeof(UBOStructures::MultiLightData)),
                CreateBuffer(GL_SHADER_STORAGE_BUFFER, 41, surfaces.data(), surfaces.size() * sizeof(ProbeSurface)),
                CreateBuffer(GL_UNIFORM_BUFFER, 25, &params, sizeof(params)),
            };

            output->Bind();
            ::glViewport(0, 0, static_cast<GLsizei>(columns), 2);
            ::glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            ::glDisable(GL_SCISSOR_TEST); // a leaked scissor would clip the probe, not the lighting
            ::glClear(GL_COLOR_BUFFER_BIT);
            ::glDisable(GL_BLEND);
            ::glDisable(GL_DEPTH_TEST);
            ::glDisable(GL_CULL_FACE);
            shader->Bind();
            FullscreenPass pass;
            pass.Draw(0);
            ::glFinish();
            output->Unbind();

            for (const u32 binding : { 9u, 10u, 11u, 12u, 18u, 40u, 41u })
                ::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, 0);
            ::glBindBufferBase(GL_UNIFORM_BUFFER, 25, 0);
            ::glDeleteBuffers(static_cast<GLsizei>(buffers.size()), buffers.data());
        }
        ReadbackRgbaFloat(output->GetColorAttachmentRendererID(0), columns, 2, pixels);
        ASSERT_EQ(pixels.size(), static_cast<sizet>(columns) * 2u * 4u);

        const auto texel = [&](u32 column, u32 row)
        {
            const sizet base = (static_cast<sizet>(row) * columns + column) * 4u;
            EXPECT_EQ(pixels[base + 3u], 1.0f) << "texel (" << column << ", " << row << ") was never written";
            return glm::vec3(pixels[base], pixels[base + 1u], pixels[base + 2u]);
        };

        for (u32 i = 0; i < columns; ++i)
        {
            const glm::vec3 loop = texel(i, 0);
            const glm::vec3 tiles = texel(i, 1);
            for (int c = 0; c < 3; ++c)
            {
                ASSERT_TRUE(std::isfinite(loop[c]) && std::isfinite(tiles[c])) << cases[i].Name;
            }
            const f32 peak = std::max({ loop.r, loop.g, loop.b, tiles.r, tiles.g, tiles.b });
            const f32 worst = std::max({ std::abs(loop.r - tiles.r), std::abs(loop.g - tiles.g), std::abs(loop.b - tiles.b) });

            if (i == controlColumn)
            {
                EXPECT_GT(worst, 0.05f * peak)
                    << "NEGATIVE CONTROL: the tile light carries attenuation " << tileLights[i].ShadowAndAttenuation.y
                    << " against the loop's " << packedQuadratic << ", yet the two rows agree (loop " << loop.r << ", "
                    << loop.g << ", " << loop.b << "). The probe is not comparing two evaluators, so every "
                    << "agreement above proves nothing.";
                continue;
            }

            if (cases[i].ExpectLit)
                EXPECT_GT(peak, 1e-3f) << cases[i].Name << ": unlit — the case proves nothing";
            else
                EXPECT_EQ(peak, 0.0f) << cases[i].Name << ": expected no light on either row";

            EXPECT_LE(worst, 1e-5f * std::max(1.0f, peak))
                << cases[i].Name << ": the light loop (Forward) returns (" << loop.r << ", " << loop.g << ", " << loop.b
                << ") and the clustered tiles (Forward+ / Deferred) return (" << tiles.r << ", " << tiles.g << ", "
                << tiles.b << ") for the same light and surface (issue #1457)";
        }
    }
} // namespace OloEngine::Tests

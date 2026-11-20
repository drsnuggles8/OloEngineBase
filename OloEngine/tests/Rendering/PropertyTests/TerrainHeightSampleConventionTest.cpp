// OLO_TEST_LAYER: L8
// =============================================================================
// TerrainHeightSampleConventionTest.cpp — the ground is drawn where the CPU
// says it is (issue #1721).
//
// A height map's N texels are N SAMPLES, N - 1 intervals apart, the first and
// the last on the tile's edges. TerrainData::GetHeightAt reads it that way, and
// so does everything built from it: the Jolt collision shape, the chunk meshes,
// foliage placement, the picker. The render path handed the tile UV straight to
// textureLod, which reads a texel as an AREA centred half a texel in from the
// edge, so the ground was drawn (uv - 0.5) texels away from the ground things
// stand on: nothing at the middle of a tile, half a texel at its edges.
//
// THE SCENE. A 64 m terrain from a 17 px height map the test writes (4 m between
// samples, so half a texel is 2 m), with a different height step at every
// sample along both axes. The camera looks straight down at the quarter of the
// tile next to the origin, where the two rules disagree by 0.1 to 0.45 texels.
//
// WHAT IS ASSERTED, per render path, in this process's backend: the world
// point under every terrain pixel, unprojected from the frame's depth, has the
// height GetHeightAt gives at that point's (x, z) — and does NOT fit the old
// rule, evaluated on the CPU for the same pixels, which is what makes the first
// number mean something. And, without a frame: every fetch of the height map
// by a tile UV in the shader tree goes through oloTerrainHeightUV, counted.
//
// Classification: L8 (full pipeline through the real Scene render path, depth
// readback). SKIPs cleanly without a GPU context. Runs in the --olo-rhi=vulkan
// process too.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "../../TestTempDir.h"
#include "../ReSTIR/ShaderSourceScan.h"

#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Terrain/TerrainData.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kFrame = 256;
        constexpr u32 kSamples = 17; // 16 intervals of 4 m
        constexpr f32 kWorldSize = 64.0f;
        constexpr f32 kHeightScale = 16.0f;

        [[nodiscard]] const char* PathName(RenderingPath path)
        {
            switch (path)
            {
                case RenderingPath::Forward:
                    return "Forward";
                case RenderingPath::ForwardPlus:
                    return "ForwardPlus";
                case RenderingPath::Deferred:
                    return "Deferred";
                default:
                    return "Other";
            }
        }

        // The height of sample (x, z), 0..255: a step profile along X plus
        // another along Z. Between four samples that is a plane, so ANY
        // triangulation of the cell draws the CPU's bilinear lerp exactly, and
        // the comparison does not depend on how finely a node is tessellated.
        [[nodiscard]] u8 SampleValue(u32 x, u32 z)
        {
            constexpr std::array<u8, kSamples> alongX{ 10, 60, 25, 90, 40, 110, 55, 20, 75, 30, 100, 45, 15, 85, 35, 65, 5 };
            constexpr std::array<u8, kSamples> alongZ{ 0, 45, 15, 70, 30, 95, 20, 60, 10, 80, 35, 105, 25, 55, 5, 90, 40 };
            return static_cast<u8>(alongX[x] + alongZ[z]);
        }

        // What a bare textureLod(heightmap, uv) reads: texel i is an area
        // centred at (i + 0.5) / N, clamped at the edges. The rule the render
        // path used; `at` reads one sample.
        template<typename SampleAt>
        [[nodiscard]] f32 AreaRuleHeight(const f32 u, const f32 v, const SampleAt& at)
        {
            const auto axis = [](const f32 t, u32& i0, u32& i1, f32& frac)
            {
                const f32 f = std::clamp(t * static_cast<f32>(kSamples) - 0.5f, 0.0f, static_cast<f32>(kSamples - 1u));
                i0 = static_cast<u32>(f);
                i1 = std::min(i0 + 1u, kSamples - 1u);
                frac = f - static_cast<f32>(i0);
            };
            u32 x0 = 0;
            u32 x1 = 0;
            u32 z0 = 0;
            u32 z1 = 0;
            f32 fx = 0.0f;
            f32 fz = 0.0f;
            axis(u, x0, x1, fx);
            axis(v, z0, z1, fz);
            return glm::mix(glm::mix(at(x0, z0), at(x1, z0), fx), glm::mix(at(x0, z1), at(x1, z1), fx), fz);
        }
    } // namespace

    class TerrainHeightSampleConvention : public RendererAttachedTest, public ::testing::WithParamInterface<RenderingPath>
    {
      protected:
        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return true;
        }

        void BuildScene() override
        {
            EnableRendering(kFrame, kFrame);

            auto& rs = Renderer3D::GetRendererSettings();
            rs.Path = GetParam();
            rs.ShowGrid = false;
            Renderer3D::ApplyRendererSettings();

            const fs::path heightmap = TempDir("terrain-height-samples") / "height.png";
            std::vector<u8> pixels(static_cast<sizet>(kSamples) * kSamples);
            for (u32 z = 0; z < kSamples; ++z)
                for (u32 x = 0; x < kSamples; ++x)
                    pixels[static_cast<sizet>(z) * kSamples + x] = SampleValue(x, z);
            // The write-side flip is process-wide and has no getter; state it.
            ::stbi_flip_vertically_on_write(0);
            ASSERT_NE(::stbi_write_png(heightmap.string().c_str(), static_cast<int>(kSamples), static_cast<int>(kSamples), 1,
                                       pixels.data(), static_cast<int>(kSamples)),
                      0)
                << "could not write " << heightmap.string();

            m_Camera = GetScene().CreateEntity("Camera");
            auto& cam = m_Camera.AddComponent<CameraComponent>();
            cam.Primary = true;
            cam.Camera.SetViewportSize(kFrame, kFrame);
            // Straight down over (17, 17) from 60 m: the frame is the ground
            // between 2.5 and 31.5 m on both axes, tile UV 0.04 to 0.49.
            auto& transform = m_Camera.GetComponent<TransformComponent>();
            transform.Translation = { 17.0f, 60.0f, 17.0f };
            transform.SetRotationEuler({ -glm::half_pi<f32>(), 0.0f, 0.0f });
            cam.Camera.SetPerspective(glm::radians(30.0f), 0.5f, 200.0f);

            Entity sun = GetScene().CreateEntity("Sun");
            auto& dl = sun.AddComponent<DirectionalLightComponent>();
            dl.m_Direction = glm::normalize(glm::vec3(0.3f, -0.9f, 0.2f));
            dl.m_Intensity = 2.0f;

            m_Terrain = GetScene().CreateEntity("Terrain");
            auto& terrain = m_Terrain.AddComponent<TerrainComponent>();
            terrain.m_HeightmapPath = heightmap.generic_string();
            terrain.m_WorldSizeX = kWorldSize;
            terrain.m_WorldSizeZ = kWorldSize;
            terrain.m_HeightScale = kHeightScale;
            terrain.m_CollisionEnabled = false;
        }

        Entity m_Camera;
        Entity m_Terrain;
    };

    TEST_P(TerrainHeightSampleConvention, TheDrawnGroundIsWhereTheCpuHeightIs)
    {
        RunFrames(6);

        const auto& terrain = m_Terrain.GetComponent<TerrainComponent>();
        ASSERT_TRUE(terrain.m_TerrainData) << "the render path built no height field";
        const TerrainData& data = *terrain.m_TerrainData;
        ASSERT_EQ(data.GetResolution(), kSamples);

        // The samples as the engine decoded them, read back through the CPU's
        // own accessor at the positions it puts them.
        const auto sampleAt = [&data](const u32 x, const u32 z)
        {
            return data.GetHeightAt(static_cast<f32>(x) / static_cast<f32>(kSamples - 1u),
                                    static_cast<f32>(z) / static_cast<f32>(kSamples - 1u)) *
                   kHeightScale;
        };
        ASSERT_NEAR(sampleAt(3, 5), static_cast<f32>(SampleValue(3, 5)) / 255.0f * kHeightScale, 0.01f)
            << "the decoded height map is not the one this test wrote";

        auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
        ASSERT_TRUE(fb) << "no scene framebuffer to read the depth of";
        const u32 width = fb->GetSpecification().Width;
        const u32 height = fb->GetSpecification().Height;
        ASSERT_EQ(width, kFrame);
        ASSERT_EQ(height, kFrame);
        std::vector<f32> depth(static_cast<sizet>(width) * height);
        ReadbackDepth32F(fb->GetDepthAttachmentHandle(), width, height, depth);

        const glm::mat4 world = m_Camera.GetComponent<TransformComponent>().GetTransform();
        const glm::mat4 inverseViewProjection =
            glm::inverse(m_Camera.GetComponent<CameraComponent>().Camera.GetProjection() * glm::inverse(world));

        u32 compared = 0;
        f64 squaredSampleRule = 0.0;
        f64 squaredAreaRule = 0.0;
        f32 worstSampleRule = 0.0f;
        for (u32 y = 0; y < height; ++y)
        {
            for (u32 x = 0; x < width; ++x)
            {
                const f32 d = depth[static_cast<sizet>(y) * width + x];
                if (!(d < 1.0f))
                    continue;
                // glm::perspective is RH_NO: depth [0, 1] is NDC z [-1, 1].
                const glm::vec4 ndc{ ((static_cast<f32>(x) + 0.5f) / static_cast<f32>(width)) * 2.0f - 1.0f,
                                     ((static_cast<f32>(y) + 0.5f) / static_cast<f32>(height)) * 2.0f - 1.0f, d * 2.0f - 1.0f,
                                     1.0f };
                const glm::vec4 unprojected = inverseViewProjection * ndc;
                const glm::vec3 point = glm::vec3(unprojected) / unprojected.w;
                const f32 u = point.x / kWorldSize;
                const f32 v = point.z / kWorldSize;
                if (u < 0.02f || u > 0.5f || v < 0.02f || v > 0.5f)
                    continue;
                const f32 sampleRule = data.GetHeightAt(u, v) * kHeightScale;
                const f32 areaRule = AreaRuleHeight(u, v, sampleAt);
                ++compared;
                squaredSampleRule += static_cast<f64>(point.y - sampleRule) * (point.y - sampleRule);
                squaredAreaRule += static_cast<f64>(point.y - areaRule) * (point.y - areaRule);
                worstSampleRule = std::max(worstSampleRule, std::abs(point.y - sampleRule));
            }
        }
        // The frame is all terrain: a count far below it means the depth read
        // back is not the terrain's, and the numbers below are about nothing.
        ASSERT_GT(compared, kFrame * kFrame * 3u / 4u) << "the frame's depth does not show the terrain";

        const f64 rmsSampleRule = std::sqrt(squaredSampleRule / compared);
        const f64 rmsAreaRule = std::sqrt(squaredAreaRule / compared);
        std::cout << "[TerrainHeightSampleConvention] " << PathName(GetParam()) << ": " << compared
                  << " px; drawn ground vs GetHeightAt rms " << rmsSampleRule << " m (worst " << worstSampleRule
                  << " m); vs the texel-area rule rms " << rmsAreaRule << " m\n";

        // Measured: rms 0.2 mm on OpenGL and 0.6 mm on Vulkan, worst 2 mm, which
        // is the depth buffer's precision at 60 m; and with the conversion
        // removed, 1.06 m against GetHeightAt and 0.05 m against the area rule.
        EXPECT_LT(rmsSampleRule, 0.01) << "the drawn ground is not at the height the CPU gives for the same (x, z)";
        EXPECT_LT(worstSampleRule, 0.05f) << "somewhere the drawn ground leaves the CPU's";
        EXPECT_GT(rmsAreaRule, 0.5)
            << "the old rule fits this frame as well: the scene no longer tells the two conventions apart";
    }

    INSTANTIATE_TEST_SUITE_P(AllPaths, TerrainHeightSampleConvention,
                             ::testing::Values(RenderingPath::Forward, RenderingPath::ForwardPlus, RenderingPath::Deferred),
                             [](const ::testing::TestParamInfo<RenderingPath>& info)
                             { return std::string(PathName(info.param)); });

    // Every fetch of the height map by a tile UV converts the UV first. Counted
    // per file: a fetch added without the conversion makes the two numbers
    // differ, and a file that stops fetching at all makes the first one zero.
    TEST(TerrainHeightSampleConventionSource, EveryUvFetchOfTheHeightMapConvertsItsUv)
    {
        struct File
        {
            const char* Path;
            u32 Fetches;
        };
        // One in the evaluation stages' shared helper, four in the per-pixel
        // normal, and five in the virtual-texture bake (four for the slope, one
        // for the position).
        constexpr std::array<File, 3> files{ File{ "include/TerrainHeightSampling.glsl", 1 },
                                             File{ "include/TerrainPixelNormal.glsl", 4 },
                                             File{ "compute/TerrainVTTileBake.comp", 5 } };
        const auto count = [](const std::string& text, const std::string& needle)
        {
            u32 n = 0;
            for (sizet at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
                ++n;
            return n;
        };
        for (const File& file : files)
        {
            const fs::path path = ShaderScan::ResolveShaderPath(file.Path);
            ASSERT_TRUE(fs::exists(path)) << path.string();
            // Whitespace out, so a fetch wrapped over two lines still counts.
            std::string source = ShaderScan::ReadTextFile(path);
            std::erase_if(source, [](const char c)
                          { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; });
            EXPECT_EQ(count(source, "textureLod(u_TerrainHeightmap,"), file.Fetches) << file.Path;
            EXPECT_EQ(count(source, "textureLod(u_TerrainHeightmap,oloTerrainHeightUV("), file.Fetches)
                << file.Path << " fetches the height map by a tile UV it did not convert (include/TerrainHeightUV.glsl)";
            EXPECT_EQ(count(source, "texture(u_TerrainHeightmap,"), 0u)
                << file.Path << " fetches the height map with implicit derivatives";
        }

        // No terrain surface or depth shader fetches it any other way: they all
        // go through the helper above.
        for (const char* shader : { "Terrain_PBR.glsl", "Terrain_GBuffer.glsl", "include/TerrainDepthTess_EvaluationStage.glsl" })
        {
            const fs::path path = ShaderScan::ResolveShaderPath(shader);
            ASSERT_TRUE(fs::exists(path)) << path.string();
            std::string source = ShaderScan::ReadTextFile(path);
            std::erase_if(source, [](const char c)
                          { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; });
            EXPECT_EQ(count(source, "textureLod(u_TerrainHeightmap,"), 0u) << shader;
            EXPECT_EQ(count(source, "texture(u_TerrainHeightmap,"), 0u) << shader;
            EXPECT_GE(count(source, "oloTerrainFilteredHeight("), 1u) << shader << " no longer displaces from the height map";
        }
    }
} // namespace OloEngine::Tests

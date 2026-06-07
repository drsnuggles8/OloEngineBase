// =============================================================================
// StarNestSkyVisualTest.cpp
//
// Visual evidence (PNG) for the Star Nest nebula sky (issue #292). Bakes the
// nebula to a cubemap via StarNestSky::Generate, reads the six faces back,
// applies the editor's default tonemap (TONEMAP_NONE: clamp + gamma 2.2) and
// writes a two-row face grid (skybox + irradiance) to
//   OloEditor/assets/tests/visual/StarNestSky_VisualEvidence.png
// so a reviewer (and the author) can confirm the bake actually looks like a
// nebula — dark space with coloured cloud structure and bright star cores —
// and that the irradiance row (what reflects/lights surfaces) is a sane,
// comparable brightness rather than blown white.
//
// The PNG is always written, pass or fail. Assertions are deliberately loose
// (GPU/driver varies); the must-hold contracts are pinned numerically by
// StarNestSkyBakeTest (non-black, finite, sane ceiling) and StarNestSkyMathTest
// (the marching formula). This test exists for the human-readable artifact.
//
// Classification: L8 / integration (full Renderer3D bring-up + GPU bake +
// cubemap face readback + PNG output).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/StarNestSky.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/TextureCubemap.h"

#include <glm/glm.hpp>
#include <stb_image/stb_image_write.h>

#include <cstring>
#include <filesystem>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        // Mirror the editor's default post-process transform (TONEMAP_NONE):
        // clamp to [0,1] then gamma 2.2.
        glm::vec3 TonemapEditorNone(glm::vec3 x)
        {
            glm::vec3 mapped = glm::clamp(x, glm::vec3(0.0f), glm::vec3(1.0f));
            return glm::pow(mapped, glm::vec3(1.0f / 2.2f));
        }

        u8 ToByte(f32 v)
        {
            return static_cast<u8>(glm::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    } // namespace

    class StarNestSkyVisualTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override {}
    };

    TEST_F(StarNestSkyVisualTest, WritesFaceGridPng)
    {
        StarNestParameters params;
        // A slightly brighter, more saturated nebula reads better in the grid.
        params.Intensity = 3.0f;
        params.Saturation = 0.9f;

        constexpr u32 kFace = 128;
        auto envMap = StarNestSky::Generate(params, kFace);
        ASSERT_TRUE(envMap && envMap->GetEnvironmentMap());
        ASSERT_TRUE(envMap->GetIrradianceMap());

        // Top row: the skybox cubemap (what the camera sees behind geometry).
        // Bottom row: the irradiance cubemap (what lights/reflects via diffuse
        // IBL), upscaled to the same cell size.
        struct Source
        {
            Ref<TextureCubemap> Cubemap;
            const char* Label;
        };
        const Source sources[2] = {
            { envMap->GetEnvironmentMap(), "skybox" },
            { envMap->GetIrradianceMap(), "irradiance" },
        };

        constexpr u32 kCols = 6; // 6 faces in a row
        constexpr u32 kCell = 128;
        const u32 imgW = kCols * kCell;
        const u32 imgH = 2 * kCell; // skybox row + irradiance row
        std::vector<u8> img(static_cast<sizet>(imgW) * imgH * 4, 0);

        f32 meanLuma = 0.0f;
        u32 sampleCount = 0;

        for (u32 s = 0; s < 2; ++s)
        {
            auto cubemap = sources[s].Cubemap;
            const u32 face = cubemap->GetWidth();
            const auto& spec = cubemap->GetCubemapSpecification();
            ASSERT_EQ(spec.Format, ImageFormat::RGBA32F);

            for (u32 faceIdx = 0; faceIdx < 6; ++faceIdx)
            {
                std::vector<u8> bytes;
                ASSERT_TRUE(cubemap->GetFaceData(faceIdx, bytes, 0))
                    << "GetFaceData failed for " << sources[s].Label << " face " << faceIdx;
                ASSERT_GE(bytes.size(), static_cast<sizet>(face) * face * 16);

                const u32 ox = faceIdx * kCell;
                const u32 oy = s * kCell;

                for (u32 y = 0; y < kCell; ++y)
                {
                    for (u32 x = 0; x < kCell; ++x)
                    {
                        const u32 sx = (x * face) / kCell;
                        const u32 sy = (y * face) / kCell;
                        f32 rgb[4];
                        std::memcpy(rgb, bytes.data() + (static_cast<sizet>(sy) * face + sx) * 16, sizeof(rgb));
                        glm::vec3 linear(rgb[0], rgb[1], rgb[2]);
                        meanLuma += (linear.r + linear.g + linear.b) / 3.0f;
                        ++sampleCount;

                        glm::vec3 ldr = TonemapEditorNone(linear);
                        const sizet dst = (static_cast<sizet>(oy + y) * imgW + (ox + x)) * 4;
                        img[dst + 0] = ToByte(ldr.r);
                        img[dst + 1] = ToByte(ldr.g);
                        img[dst + 2] = ToByte(ldr.b);
                        img[dst + 3] = 255;
                    }
                }
            }
        }

        const fs::path outDir = fs::path("assets") / "tests" / "visual";
        std::error_code ec;
        fs::create_directories(outDir, ec);
        const fs::path outPath = outDir / "StarNestSky_VisualEvidence.png";

        const int ok = stbi_write_png(outPath.string().c_str(),
                                      static_cast<int>(imgW), static_cast<int>(imgH), 4,
                                      img.data(), static_cast<int>(imgW * 4));
        EXPECT_NE(ok, 0) << "Failed to write " << outPath.string();
        OLO_CORE_INFO("StarNestSky visual evidence written to {}", fs::absolute(outPath).string());

        // Loose sanity: the average linear radiance lands in a displayable range
        // (not black, not blown white).
        const f32 mean = sampleCount ? meanLuma / static_cast<f32>(sampleCount) : 0.0f;
        EXPECT_GT(mean, 0.0005f) << "Nebula is essentially black.";
        EXPECT_LT(mean, 20.0f) << "Nebula is implausibly bright (tonemaps to white).";
    }
} // namespace OloEngine::Tests

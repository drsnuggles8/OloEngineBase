// =============================================================================
// ProceduralSkyVisualTest.cpp
//
// Visual evidence (PNG) for the Preetham procedural sky. Bakes the sky to a
// cubemap via ProceduralSky::Generate, reads the six faces back, applies the
// editor's default tonemap (TONEMAP_NONE: clamp + gamma 2.2) and writes a
// two-row face grid (skybox + irradiance) to
//   OloEditor/assets/tests/visual/ProceduralSky_VisualEvidence.png
// so a reviewer (and the author) can confirm the sky actually looks like a
// sky — blue-ish gradient, brighter toward the horizon, with the sun disk
// visible on whichever horizontal face contains the sun direction.
//
// The PNG is always written, pass or fail. Assertions are deliberately
// loose (the GPU/driver varies); the contracts that *must* hold regardless
// of driver are pinned numerically by ProceduralSkyBakeTest (non-black,
// finite, sane brightness ceiling) and ProceduralSkyMathTest (analytic
// model). This test exists for the human-readable artifact.
//
// Runs from OloEditor/ (project convention). On repo-root invocation the
// PNG lands under <cwd>/assets/tests/visual/ instead — same as the other
// visual tests.
//
// Classification: L8 / integration (full Renderer3D bring-up + GPU bake +
// cubemap face readback + PNG output).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/ProceduralSky.h"
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

        // Mirror the editor's *actual* default post-process transform for the
        // ProceduralSkyTest scene: TonemapOperator 0 (TONEMAP_NONE) = clamp to
        // [0,1] then gamma 2.2 (see PostProcess_ToneMap.glsl). Using ACES here
        // instead would under-represent the result — ACES darkens and
        // desaturates midtones, whereas the gamma curve brightens them.
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

    class ProceduralSkyVisualTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override {}
    };

    TEST_F(ProceduralSkyVisualTest, WritesFaceGridPng)
    {
        PreethamParameters params;
        params.SunDirection = glm::normalize(glm::vec3(0.35f, 0.55f, 0.75f));
        params.Turbidity = 3.0f;
        params.Exposure = 0.1f;
        params.SunIntensity = 1.0f;
        params.ShowSunDisk = true;

        constexpr u32 kFace = 128;
        auto envMap = ProceduralSky::Generate(params, kFace);
        ASSERT_TRUE(envMap && envMap->GetEnvironmentMap());
        ASSERT_TRUE(envMap->GetIrradianceMap());

        // Top row: the skybox cubemap (what the camera sees behind geometry).
        // Bottom row: the irradiance cubemap (what lights every surface via
        // diffuse IBL), upscaled to the same cell size. The two rows MUST look
        // comparably bright after tonemapping — if the irradiance row is white
        // while the skybox row is a sky, that's the "white objects, correct
        // sky" bug (stale/over-bright IBL). The irradiance map is intrinsically
        // blurry (it's a cosine-convolved low-res cubemap), so expect smooth
        // colour blobs, not detail.
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
                        // Nearest-sample upscale from face res to kCell.
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
        const fs::path outPath = outDir / "ProceduralSky_VisualEvidence.png";

        const int ok = stbi_write_png(outPath.string().c_str(),
                                      static_cast<int>(imgW), static_cast<int>(imgH), 4,
                                      img.data(), static_cast<int>(imgW * 4));
        EXPECT_NE(ok, 0) << "Failed to write " << outPath.string();
        OLO_CORE_INFO("ProceduralSky visual evidence written to {}", fs::absolute(outPath).string());

        // Loose sanity: the average linear sky radiance lands in a displayable
        // range (not black, not the pre-fix hundreds-bright white-out).
        const f32 mean = sampleCount ? meanLuma / static_cast<f32>(sampleCount) : 0.0f;
        EXPECT_GT(mean, 0.001f) << "Sky is essentially black.";
        EXPECT_LT(mean, 20.0f) << "Sky is implausibly bright (tonemaps to white).";
    }
} // namespace OloEngine::Tests

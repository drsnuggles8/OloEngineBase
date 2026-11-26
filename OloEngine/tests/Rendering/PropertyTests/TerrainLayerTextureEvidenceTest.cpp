// OLO_TEST_LAYER: L8
// =============================================================================
// TerrainLayerTextureEvidenceTest.cpp — textured terrain layers reach the frame
// (issue #1721).
//
// No shipped scene gave a terrain layer a texture, and the path was defective
// in ways only a textured layer shows:
//
//   * the layer arrays were CLAMPED while the shaders sample uv * tiling, so a
//     textured layer drew one tile in a corner and smeared its border texels
//     over the rest of the terrain;
//   * the side projections on a slope multiplied a world position in METRES by
//     the tiling the flat ground multiplies a [0,1] UV by, and ran texture V
//     along a horizontal axis;
//   * there was no per-terrain colour variation, and no way to hide a tiled
//     layer's repetition at range.
//
// THE SCENE. A 64 m terrain from a heightmap the test writes: a flat plain
// (x < 40 m), a cliff rising 24 m over the next 8 m, a plateau behind it that
// falls back to the plain's height by the far edge. Layer 0, a black/white
// checker, is painted on the plain; layer 1, horizontal stripes, on the cliff
// and the plateau. Both tile 8 times across the terrain.
// Every texture — heightmap, splatmap, layers, macro maps — is written by the
// test into its temp directory.
//
// WHAT IS ASSERTED, per render path, in this process's backend:
//   (a) the checker REPEATS: a region of the plain 11 to 29 m from the corner
//       the first tile sits in shows the checker's contrast (the clamp gave a
//       flat smear there), and the control with the textures removed is flat;
//   (b) a macro map that halves red halves the region's red and leaves green,
//       and a neutral macro map changes nothing;
//   (c) a wet macro map darkens the region;
//   (d) the stripes are HORIZONTAL on the cliff: rows of the image differ,
//       columns do not;
//   (e) from 220 m the checker's cells are several times larger than from
//       30 m: the far sampling scale has replaced the near one.
// through the virtual texture (TerrainLayerTextureVirtualTexture, GL process):
//   (h) the tile bake puts the same textured plain and the same horizontally
//       banded cliff into the cache that the per-pixel path draws;
// and, without a frame (TerrainLayerTextureMaterial):
//   (f) two materials with identical layers hold the same arrays;
//   (g) a splatmap loaded from a file fills the CPU mirror with the weights the
//       GPU samples, and a foliage layer masked on an empty channel places
//       nothing.
//
// Evidence: OloEditor/assets/tests/visual/TerrainLayerTexture_<Backend>_<Path>_<Angle>.png
// and the controls TerrainLayerTextureOff_<Backend>_<Path>_<Angle>.png. Runs in
// the --olo-rhi=vulkan process too.
//
// Classification: L8 (full pipeline through the real Scene render path,
// readback + PNG). SKIPs cleanly without a GPU context.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "TestRenderBackend.h"
#include "../../TestTempDir.h"

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Physics3D/JoltScene.h"
#include "OloEngine/Physics3D/SceneQueries.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/Texture2DArray.h"
#include "OloEngine/Renderer/Water/WaterShoreDepthSystem.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/TerrainGround.h"
#include "OloEngine/Terrain/Foliage/FoliageLayer.h"
#include "OloEngine/Terrain/Foliage/FoliageRenderer.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Terrain/TerrainStreamer.h"
#include "OloEngine/Terrain/TerrainSurfaceShading.h"
#include "OloEngine/Terrain/VirtualTexture/TerrainVirtualTexture.h"
#include "OloEngine/Utils/PlatformUtils.h"

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
#include <utility>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kSize = 256;
        constexpr u32 kFramesPerCapture = 6;
        constexpr f32 kCaptureTime = 4.0f;

        // The terrain. World metres and heightmap texels coincide along X.
        constexpr f32 kWorldSize = 64.0f;
        constexpr f32 kHeightScale = 24.0f;
        constexpr u32 kHeightmapSize = 65;
        constexpr u32 kCliffStart = 40; // metres along X where the plain ends
        constexpr u32 kCliffEnd = 48;   // ... and the plateau begins
        constexpr u32 kPlateauEnd = 56; // ... and ends, falling back to zero at the far edge
        constexpr u32 kSplatSize = 64;
        constexpr u32 kPaintedSplit = 36; // splat texels below this are layer 0 (checker), the rest layer 1 (stripes)
        constexpr f32 kLayerTiling = 8.0f;
        constexpr u32 kLayerTextureSize = 64;
        constexpr u8 kDark = 20;
        constexpr u8 kLight = 235;

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

        // stb's write-side vertical flip is a PROCESS-WIDE switch with no getter,
        // and a suite that runs earlier in the same process can leave it on
        // (ShadowFamilyVisualEvidenceTest's frame writer does). Every image this
        // file writes is an input whose row order matters or evidence that must
        // be the right way up, so each write states the order it means.
        void WritePng(const fs::path& file, u32 width, u32 height, int channels, const std::vector<u8>& pixels)
        {
            ::stbi_flip_vertically_on_write(0);
            ASSERT_NE(::stbi_write_png(file.string().c_str(), static_cast<int>(width), static_cast<int>(height), channels,
                                       pixels.data(), static_cast<int>(width) * channels),
                      0)
                << "could not write " << file.string();
        }

        // A uniform RGBA image.
        void WriteSolid(const fs::path& file, u32 size, u8 r, u8 g, u8 b, u8 a)
        {
            std::vector<u8> pixels(static_cast<sizet>(size) * size * 4u);
            for (sizet at = 0; at < pixels.size(); at += 4)
            {
                pixels[at] = r;
                pixels[at + 1] = g;
                pixels[at + 2] = b;
                pixels[at + 3] = a;
            }
            WritePng(file, size, size, 4, pixels);
        }

        // 2x2 cells of dark and light.
        void WriteChecker(const fs::path& file)
        {
            std::vector<u8> pixels(static_cast<sizet>(kLayerTextureSize) * kLayerTextureSize * 4u);
            const u32 cell = kLayerTextureSize / 2u;
            for (u32 y = 0; y < kLayerTextureSize; ++y)
            {
                for (u32 x = 0; x < kLayerTextureSize; ++x)
                {
                    const u8 value = (((x / cell) + (y / cell)) % 2u) == 0u ? kDark : kLight;
                    const sizet at = (static_cast<sizet>(y) * kLayerTextureSize + x) * 4u;
                    pixels[at] = pixels[at + 1] = pixels[at + 2] = value;
                    pixels[at + 3] = 255;
                }
            }
            WritePng(file, kLayerTextureSize, kLayerTextureSize, 4, pixels);
        }

        // Bands across the image: every texel of a ROW has one value, and the
        // value alternates every 8 rows. Texture V is the only axis it varies on.
        void WriteHorizontalStripes(const fs::path& file)
        {
            std::vector<u8> pixels(static_cast<sizet>(kLayerTextureSize) * kLayerTextureSize * 4u);
            for (u32 y = 0; y < kLayerTextureSize; ++y)
            {
                const u8 value = ((y / 8u) % 2u) == 0u ? kDark : kLight;
                for (u32 x = 0; x < kLayerTextureSize; ++x)
                {
                    const sizet at = (static_cast<sizet>(y) * kLayerTextureSize + x) * 4u;
                    pixels[at] = pixels[at + 1] = pixels[at + 2] = value;
                    pixels[at + 3] = 255;
                }
            }
            WritePng(file, kLayerTextureSize, kLayerTextureSize, 4, pixels);
        }

        // Plain, cliff, plateau, and a fall back to the plain's height along X;
        // constant along Z.
        //
        // The two X edges are the same height. When this suite was written the
        // GPU heightmap was a repeat-wrapped texture read at the tile UV, so the
        // vertices on the u = 0 edge filtered in the texels of the u = 1 edge,
        // and a plateau running to the far edge raised a 12 m wall along x = 0
        // whose shadow covered the first 10 m of the plain. Both halves of that
        // are fixed (TerrainData::UploadToGPU clamps; the shaders read the edge
        // SAMPLE at the edge, include/TerrainHeightUV.glsl), and
        // TerrainHeightSampleConventionTest pins the second. The profile stays
        // as it is because the assertions below are measured on it.
        void WriteHeightmap(const fs::path& file)
        {
            std::vector<u8> pixels(static_cast<sizet>(kHeightmapSize) * kHeightmapSize);
            for (u32 y = 0; y < kHeightmapSize; ++y)
            {
                for (u32 x = 0; x < kHeightmapSize; ++x)
                {
                    const f32 rise = std::clamp((static_cast<f32>(x) - static_cast<f32>(kCliffStart)) /
                                                    static_cast<f32>(kCliffEnd - kCliffStart),
                                                0.0f, 1.0f);
                    const f32 fall = std::clamp((static_cast<f32>(kHeightmapSize - 1u) - static_cast<f32>(x)) /
                                                    static_cast<f32>(kHeightmapSize - 1u - kPlateauEnd),
                                                0.0f, 1.0f);
                    pixels[static_cast<sizet>(y) * kHeightmapSize + x] =
                        static_cast<u8>(std::lround(std::min(rise, fall) * 255.0f));
                }
            }
            WritePng(file, kHeightmapSize, kHeightmapSize, 1, pixels);
        }

        // Layer 0 (red) below kPaintedSplit along X, layer 1 (green) from there.
        void WriteSplatmap(const fs::path& file)
        {
            std::vector<u8> pixels(static_cast<sizet>(kSplatSize) * kSplatSize * 4u, 0);
            for (u32 y = 0; y < kSplatSize; ++y)
            {
                for (u32 x = 0; x < kSplatSize; ++x)
                {
                    const sizet at = (static_cast<sizet>(y) * kSplatSize + x) * 4u;
                    pixels[at + (x < kPaintedSplit ? 0 : 1)] = 255;
                }
            }
            WritePng(file, kSplatSize, kSplatSize, 4, pixels);
        }

        // Rows arrive bottom-up; the PNG is written top-down.
        void WriteEvidence(const std::string& name, const std::vector<u8>& rgba)
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string file = (dir / (name + ".png")).string();
            const sizet rowBytes = static_cast<sizet>(kSize) * 4u;
            std::vector<u8> topDown(rgba.size());
            for (u32 y = 0; y < kSize; ++y)
                std::copy_n(rgba.data() + static_cast<sizet>(kSize - 1u - y) * rowBytes, rowBytes,
                            topDown.data() + static_cast<sizet>(y) * rowBytes);
            ::stbi_flip_vertically_on_write(0);
            EXPECT_NE(::stbi_write_png(file.c_str(), static_cast<int>(kSize), static_cast<int>(kSize), 4, topDown.data(),
                                       static_cast<int>(rowBytes)),
                      0)
                << "could not write " << file;
        }

        [[nodiscard]] f64 SrgbToLinear(u8 value)
        {
            const f64 c = static_cast<f64>(value) / 255.0;
            return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
        }

        [[nodiscard]] f64 Luma(const std::vector<u8>& rgba, u32 x, u32 y)
        {
            const sizet at = (static_cast<sizet>(y) * kSize + x) * 4u;
            return 0.2126 * rgba[at] + 0.7152 * rgba[at + 1] + 0.0722 * rgba[at + 2];
        }

        // A square centred on the frame: `half` pixels either side of the
        // middle. Centred, so no statistic below depends on which way up the
        // rows of a readback are.
        struct Region
        {
            u32 Min;
            u32 Max; // exclusive
        };
        [[nodiscard]] constexpr Region Centre(u32 half)
        {
            return Region{ kSize / 2u - half, kSize / 2u + half };
        }

        // Mean of the DISPLAY-LINEAR colour over the region: the frame's bytes
        // through the sRGB decode, so a ratio of two means is a ratio of light.
        [[nodiscard]] glm::dvec3 MeanLinearColour(const std::vector<u8>& rgba, Region region)
        {
            glm::dvec3 sum(0.0);
            u32 count = 0;
            for (u32 y = region.Min; y < region.Max; ++y)
            {
                for (u32 x = region.Min; x < region.Max; ++x)
                {
                    const sizet at = (static_cast<sizet>(y) * kSize + x) * 4u;
                    sum += glm::dvec3(SrgbToLinear(rgba[at]), SrgbToLinear(rgba[at + 1]), SrgbToLinear(rgba[at + 2]));
                    ++count;
                }
            }
            return sum / static_cast<f64>(count);
        }

        [[nodiscard]] f64 StdDev(const std::vector<f64>& values)
        {
            f64 mean = 0.0;
            for (const f64 v : values)
                mean += v;
            mean /= static_cast<f64>(values.size());
            f64 variance = 0.0;
            for (const f64 v : values)
                variance += (v - mean) * (v - mean);
            return std::sqrt(variance / static_cast<f64>(values.size()));
        }

        // Standard deviation of the 8-bit luma over the region.
        [[nodiscard]] f64 LumaContrast(const std::vector<u8>& rgba, Region region)
        {
            std::vector<f64> values;
            for (u32 y = region.Min; y < region.Max; ++y)
                for (u32 x = region.Min; x < region.Max; ++x)
                    values.push_back(Luma(rgba, x, y));
            return StdDev(values);
        }

        // How much the region's ROW means differ from each other, and its
        // COLUMN means. Horizontal stripes move the first and not the second.
        struct Banding
        {
            f64 AcrossRows = 0.0;
            f64 AcrossColumns = 0.0;
        };
        [[nodiscard]] Banding MeasureBanding(const std::vector<u8>& rgba, Region region)
        {
            const u32 extent = region.Max - region.Min;
            std::vector<f64> rows(extent, 0.0);
            std::vector<f64> columns(extent, 0.0);
            for (u32 y = region.Min; y < region.Max; ++y)
            {
                for (u32 x = region.Min; x < region.Max; ++x)
                {
                    const f64 luma = Luma(rgba, x, y);
                    rows[y - region.Min] += luma / static_cast<f64>(extent);
                    columns[x - region.Min] += luma / static_cast<f64>(extent);
                }
            }
            return Banding{ .AcrossRows = StdDev(rows), .AcrossColumns = StdDev(columns) };
        }

        // Along the frame's middle row between two columns: the luma range, and
        // the longest run of pixels on one side of its middle — the width of
        // the widest checker cell the row crosses, in pixels. The range is what
        // says the run measures a checker: on a flat row it is noise and the
        // run means nothing.
        struct RowCells
        {
            f64 LumaRange = 0.0;
            u32 LongestRun = 0;
        };
        [[nodiscard]] RowCells MeasureMiddleRow(const std::vector<u8>& rgba, u32 firstColumn, u32 lastColumn)
        {
            const u32 row = kSize / 2u;
            f64 lo = 255.0;
            f64 hi = 0.0;
            for (u32 x = firstColumn; x < lastColumn; ++x)
            {
                lo = std::min(lo, Luma(rgba, x, row));
                hi = std::max(hi, Luma(rgba, x, row));
            }
            const f64 mid = 0.5 * (lo + hi);
            u32 longest = 0;
            u32 run = 0;
            bool above = Luma(rgba, firstColumn, row) > mid;
            for (u32 x = firstColumn; x < lastColumn; ++x)
            {
                const bool nowAbove = Luma(rgba, x, row) > mid;
                run = (nowAbove == above) ? run + 1u : 1u;
                above = nowAbove;
                longest = std::max(longest, run);
            }
            return RowCells{ .LumaRange = hi - lo, .LongestRun = longest };
        }

        [[nodiscard]] u32 MaxChannelDifference(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            u32 worst = 0;
            for (sizet at = 0; at + 3 < std::min(a.size(), b.size()); at += 4)
                for (sizet c = 0; c < 3; ++c)
                    worst = std::max(worst, static_cast<u32>(std::abs(static_cast<int>(a[at + c]) - static_cast<int>(b[at + c]))));
            return worst;
        }

        [[nodiscard]] std::string Text(const glm::dvec3& v)
        {
            return "(" + std::to_string(v.x) + ", " + std::to_string(v.y) + ", " + std::to_string(v.z) + ")";
        }

        // Pins the engine clock for a capture and puts back whatever was there.
        struct ScopedMockTime
        {
            explicit ScopedMockTime(f32 t)
                : m_HadMock(Time::HasMockTime()), m_Previous(Time::GetMockTime())
            {
                Time::SetMockTime(t);
            }
            ~ScopedMockTime()
            {
                if (m_HadMock)
                    Time::SetMockTime(m_Previous);
                else
                    Time::ClearMockTime();
            }
            ScopedMockTime(const ScopedMockTime&) = delete;
            ScopedMockTime& operator=(const ScopedMockTime&) = delete;

          private:
            bool m_HadMock;
            f32 m_Previous;
        };

        [[nodiscard]] TerrainLayer MakeLayer(const char* name, const fs::path& albedo)
        {
            TerrainLayer layer;
            layer.Name = name;
            layer.AlbedoPath = albedo.generic_string();
            layer.TilingScale = kLayerTiling;
            // Fully rough and non-metallic: the frame is then diffuse light,
            // which is proportional to the albedo the assertions predict from.
            layer.Roughness = 1.0f;
            layer.Metallic = 0.0f;
            layer.BaseColor = glm::vec3(0.5f);
            return layer;
        }
    } // namespace

    // ── The rendered half ────────────────────────────────────────────────────

    class TerrainLayerTextureEvidence : public RendererAttachedTest, public ::testing::WithParamInterface<RenderingPath>
    {
      protected:
        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return true;
        }

        void BuildScene() override
        {
            EnableRendering(kSize, kSize);

            auto& rs = Renderer3D::GetRendererSettings();
            rs.Path = GetParam();
            rs.ShowGrid = false;
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.AutoExposureEnabled = false;
            pp.Exposure = 1.0f;
            // No tone curve: the frame is then the lit colour through the
            // display encode alone, so ratios of decoded pixels are ratios of
            // light.
            pp.Tonemap = TonemapOperator::None;
            Renderer3D::ApplyRendererSettings();

            m_Directory = TempDir("terrain-layer-texture");
            const fs::path heightmap = m_Directory / "height.png";
            const fs::path splatmap = m_Directory / "splat0.png";
            m_Checker = m_Directory / "checker.png";
            m_Stripes = m_Directory / "stripes.png";
            ASSERT_NO_FATAL_FAILURE(WriteHeightmap(heightmap));
            ASSERT_NO_FATAL_FAILURE(WriteSplatmap(splatmap));
            ASSERT_NO_FATAL_FAILURE(WriteChecker(m_Checker));
            ASSERT_NO_FATAL_FAILURE(WriteHorizontalStripes(m_Stripes));

            m_Camera = GetScene().CreateEntity("Camera");
            auto& cam = m_Camera.AddComponent<CameraComponent>();
            cam.Primary = true;
            cam.Camera.SetViewportSize(kSize, kSize);

            // From the plain's side and above: lights the plain, the plateau
            // and the cliff face, which looks back along -X.
            Entity sun = GetScene().CreateEntity("Sun");
            auto& dl = sun.AddComponent<DirectionalLightComponent>();
            dl.m_Direction = glm::normalize(glm::vec3(0.6f, -0.7f, 0.2f));
            dl.m_Color = glm::vec3(1.0f);
            dl.m_Intensity = 2.0f;

            m_Terrain = GetScene().CreateEntity("Terrain");
            auto& terrain = m_Terrain.AddComponent<TerrainComponent>();
            terrain.m_HeightmapPath = heightmap.generic_string();
            terrain.m_WorldSizeX = kWorldSize;
            terrain.m_WorldSizeZ = kWorldSize;
            terrain.m_HeightScale = kHeightScale;
            terrain.m_CollisionEnabled = false;
            terrain.m_Material = Ref<TerrainMaterial>::Create();
            terrain.m_Material->AddLayer(MakeLayer("Checker", m_Checker));
            terrain.m_Material->AddLayer(MakeLayer("Stripes", m_Stripes));
            terrain.m_Material->SetSplatmapPath(0, splatmap.generic_string());
            terrain.m_Material->SetLayerResolution(kLayerTextureSize);
            terrain.m_MaterialNeedsRebuild = true;
        }

        [[nodiscard]] TerrainMaterial& Material()
        {
            return *m_Terrain.GetComponent<TerrainComponent>().m_Material;
        }

        void RebuildMaterial()
        {
            m_Terrain.GetComponent<TerrainComponent>().m_MaterialNeedsRebuild = true;
        }

        void SetMacroMap(const fs::path& file)
        {
            Material().SetMacroMapPath(file.empty() ? FString() : FString(file.generic_string()));
            RebuildMaterial();
        }

        // Looking straight down from `height` over (x, z). Image right is +X,
        // image up is -Z.
        void LookDown(f32 x, f32 z, f32 height, f32 fovDegrees)
        {
            auto& transform = m_Camera.GetComponent<TransformComponent>();
            transform.Translation = { x, height, z };
            transform.SetRotationEuler({ -glm::half_pi<f32>(), 0.0f, 0.0f });
            m_Camera.GetComponent<CameraComponent>().Camera.SetPerspective(glm::radians(fovDegrees), 0.5f, 1000.0f);
        }

        // Level, looking along +X at the cliff face.
        void LookAtCliff()
        {
            auto& transform = m_Camera.GetComponent<TransformComponent>();
            transform.Translation = { 18.0f, 12.0f, 32.0f };
            transform.SetRotationEuler({ 0.0f, -glm::half_pi<f32>(), 0.0f });
            m_Camera.GetComponent<CameraComponent>().Camera.SetPerspective(glm::radians(60.0f), 0.5f, 1000.0f);
        }

        // 30 m above the plain: the middle of the frame is the plain between
        // x = 11 and 29 m, z = 23 and 41 m — all checker, all nearer than the
        // 40 m where the far sampling scale starts, and two tiles away from the
        // corner tile a clamped array would have drawn.
        void LookAtThePlain()
        {
            LookDown(20.0f, 32.0f, 30.0f, 60.0f);
        }

        void Capture(std::vector<u8>& out)
        {
            RunFrames(kFramesPerCapture);
            u32 w = 0;
            u32 h = 0;
            ASSERT_TRUE(ReadbackComposite(out, w, h)) << "no composited frame to read back";
            ASSERT_EQ(w, kSize);
            ASSERT_EQ(h, kSize);
        }

        // Fraction of pixels whose RGB changed by more than four display levels.
        [[nodiscard]] static f64 ChangedFraction(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            u32 changed = 0;
            for (sizet at = 0; at < a.size(); at += 4)
            {
                const auto delta = [&](sizet channel)
                { return std::abs(a[at + channel] - b[at + channel]); };
                if (std::max({ delta(0), delta(1), delta(2) }) > 4)
                    ++changed;
            }
            return static_cast<f64>(changed) / (kSize * kSize);
        }

        [[nodiscard]] std::string EvidenceName(const char* feature, const char* angle) const
        {
            return TestRenderBackend::EvidenceName(feature) + PathName(GetParam()) + "_" + angle;
        }

        fs::path m_Directory;
        fs::path m_Checker;
        fs::path m_Stripes;
        Entity m_Camera;
        Entity m_Terrain;
    };

    TEST_P(TerrainLayerTextureEvidence, StreamedTileAndShoreDepthFollowTheCarrier)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ASSERT_FALSE(HasFatalFailure());
        ScopedMockTime mockTime(kCaptureTime);
        auto& terrain = m_Terrain.GetComponent<TerrainComponent>();
        terrain.m_StreamingEnabled = true;
        terrain.m_TileWorldSize = kWorldSize;
        terrain.m_TileResolution = kHeightmapSize;
        terrain.m_StreamingLoadRadius = 0;
        terrain.m_StreamingMaxTiles = 1;
        terrain.m_TileDirectory = m_Directory.generic_string();
        terrain.m_TileFilePattern = "tile_%d_%d.png";
        ASSERT_NO_FATAL_FAILURE(WriteHeightmap(m_Directory / "tile_1_-1.png"));

        Entity rig = GetScene().CreateEntity("TerrainCarrier");
        auto& parent = rig.GetComponent<TransformComponent>();
        parent.Translation = { 240, 3, -170 };
        parent.SetRotationEuler({ 0, glm::half_pi<f32>(), 0 });
        parent.Scale = { 1.5f, 1.25f, 1.0f };
        const glm::vec3 cameraLocal(kWorldSize + 20, 0, -kWorldSize + 32);
        const glm::vec3 target(parent.GetTransform() * glm::vec4(cameraLocal, 1));
        LookDown(target.x, target.z, target.y + 30, 55.0f);

        // Two representations of exactly the same terrain, including a nonzero
        // resident tile index. A translated-only camera conversion selects a
        // different tile even for the root twin.
        m_Terrain.GetComponent<TransformComponent>() = parent;
        std::vector<u8> root, child;
        ASSERT_NO_FATAL_FAILURE(Capture(root));
        ASSERT_TRUE(terrain.m_Streamer);
        ASSERT_TRUE(terrain.m_Streamer->GetTile(1, -1));
        ASSERT_EQ(terrain.m_Streamer->GetLoadedTileCount(), 1u);
        ASSERT_GT(LumaContrast(root, Centre(kSize / 4)), 25.0);
        m_Terrain.SetParent(rig);
        m_Terrain.GetComponent<TransformComponent>() = TransformComponent{};
        ASSERT_NO_FATAL_FAILURE(Capture(child));
        WriteEvidence(EvidenceName("ParentedTerrainOff", "StreamedTile"), root);
        WriteEvidence(EvidenceName("ParentedTerrain", "StreamedTile"), child);
        const f64 difference = ChangedFraction(root, child);
        EXPECT_LT(difference, 0.015);
        std::cout << "[ParentedTerrain] " << EvidenceName("ParentedTerrain", "StreamedTile")
                  << " tile (1,-1), checker contrast " << LumaContrast(child, Centre(kSize / 4))
                  << ", changed fraction vs root " << difference << '\n';

        Entity ocean = GetScene().CreateEntity("ShoreAboveStreamedTile");
        const f32 seaLevel = target.y + 40;
        ocean.GetComponent<TransformComponent>().Translation = { target.x, seaLevel, target.z };
        const glm::vec3 plateau(parent.GetTransform() * glm::vec4(kWorldSize + 52, kHeightScale, -kWorldSize + 32, 1));
        auto& water = ocean.AddComponent<WaterComponent>();
        water.m_ShoreWavesEnabled = true;
        water.m_WorldSizeX = water.m_WorldSizeZ = 128;
        water.m_GridResolutionX = water.m_GridResolutionZ = 8;
        terrain.m_CollisionEnabled = true;
        GetScene().OnPhysics3DStart();
        struct StopPhysics
        {
            Scene& TestScene;
            ~StopPhysics()
            {
                TestScene.OnPhysics3DStop();
            }
        } const stopPhysics{ GetScene() };
        const auto checkShore = [&](glm::vec3 location)
        {
            const auto ground = SampleTerrainGroundHeight(GetScene(), location.x, location.z);
            ASSERT_TRUE(ground.has_value());
            EXPECT_NEAR(*ground, location.y, 0.02f);
            const auto sample = WaterShoreDepthSystem::SampleWorld({ location.x, location.z });
            ASSERT_TRUE(sample.Enabled);
            EXPECT_NEAR(sample.Depth, seaLevel - location.y, 0.05f);
            RayCastInfo ray;
            ray.m_Origin = location + glm::vec3(0, 60, 0);
            ray.m_Direction = { 0, -1, 0 };
            ray.m_MaxDistance = 100;
            SceneQueryHit hit;
            ASSERT_TRUE(GetScene().GetPhysicsScene()->CastRay(ray, hit));
            EXPECT_EQ(hit.m_HitEntity, m_Terrain.GetUUID());
            EXPECT_NEAR(hit.m_Position.y, location.y, 0.05f);
        };
        RunFrames(3, 0.0f);
        ASSERT_NO_FATAL_FAILURE(checkShore(target));
        ASSERT_NO_FATAL_FAILURE(checkShore(plateau));
        m_Terrain.SetParent({});
        m_Terrain.GetComponent<TransformComponent>() = parent;
        RunFrames(2, 0.0f);
        ASSERT_NO_FATAL_FAILURE(checkShore(target));
        ASSERT_NO_FATAL_FAILURE(checkShore(plateau));
        m_Terrain.SetParent(rig);
        m_Terrain.GetComponent<TransformComponent>() = TransformComponent{};
        parent.Translation.y += 2;
        // The old tile stays loaded. The moving parent changes its base height.
        RunFrames(3, 0.0f);
        ASSERT_NO_FATAL_FAILURE(checkShore(target + glm::vec3(0, 2, 0)));
        ASSERT_NO_FATAL_FAILURE(checkShore(plateau + glm::vec3(0, 2, 0)));

        // The same world camera now sees tile (2,-1). The ready set must evict
        // (1,-1) and the analytic ground query must see the new, flat substitute.
        parent.Translation -= glm::vec3(parent.GetTransform()[0]) * kWorldSize;
        RunFrames(kFramesPerCapture, 0.0f);
        ASSERT_TRUE(terrain.m_Streamer->GetTile(2, -1));
        EXPECT_FALSE(terrain.m_Streamer->GetTile(1, -1));
        EXPECT_EQ(terrain.m_Streamer->GetLoadedTileCount(), 1u);
        EXPECT_FALSE(GetScene().GetPhysicsScene()->HasTerrainTileBody(m_Terrain.GetUUID(), 1, -1));
        EXPECT_TRUE(GetScene().GetPhysicsScene()->HasTerrainTileBody(m_Terrain.GetUUID(), 2, -1));
        ASSERT_NO_FATAL_FAILURE(checkShore(target + glm::vec3(0, 2, 0)));
    }

    TEST_P(TerrainLayerTextureEvidence, FoliageAndTerrainDrawAtTheRootTwinsPose)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ASSERT_FALSE(HasFatalFailure());
        ScopedMockTime mockTime(kCaptureTime);
        Entity rig = GetScene().CreateEntity("FoliageCarrier");
        auto& parent = rig.GetComponent<TransformComponent>();
        parent.Translation = { 90, 4, -65 };
        parent.SetRotationEuler({ 0, 0.45f, 0 });
        auto& foliage = m_Terrain.AddComponent<FoliageComponent>();
        FoliageLayer layer;
        layer.Density = 0.3f;
        layer.MinHeight = layer.MaxHeight = 2.0f;
        layer.AlbedoPath = "assets/textures/grass.png";
        foliage.m_Layers.Add(layer);
        m_Terrain.GetComponent<TransformComponent>() = parent;
        const glm::vec3 target(parent.GetTransform() * glm::vec4(20, 0, 32, 1));
        const glm::vec3 offset(0, 16, 16);
        auto& camera = m_Camera.GetComponent<TransformComponent>();
        camera.Translation = target + offset;
        camera.SetRotationEuler({ -glm::quarter_pi<f32>(), 0, 0 });
        std::vector<u8> root, child, bare;
        ASSERT_NO_FATAL_FAILURE(Capture(root));
        ASSERT_TRUE(foliage.m_Renderer);
        ASSERT_GT(foliage.m_Renderer->GetTotalInstanceCount(), 0u);
        m_Terrain.SetParent(rig);
        m_Terrain.GetComponent<TransformComponent>() = TransformComponent{};
        ASSERT_NO_FATAL_FAILURE(Capture(child));
        const auto& groups = foliage.m_Renderer->GetInstanceRegistry().GetGroups();
        ASSERT_FALSE(groups.IsEmpty());
        for (const auto& group : groups)
        {
            const auto expected = group.m_LocalBounds.Transform(parent.GetTransform());
            EXPECT_LT(glm::length(expected.Min - group.m_WorldBounds.Min), 1e-4f);
            EXPECT_LT(glm::length(expected.Max - group.m_WorldBounds.Max), 1e-4f);
        }
        WriteEvidence(EvidenceName("ParentedTerrainOff", "Foliage"), root);
        WriteEvidence(EvidenceName("ParentedTerrain", "Foliage"), child);
        const f64 twinDiff = ChangedFraction(root, child);
        EXPECT_LT(twinDiff, 0.015);
        foliage.m_Enabled = false;
        ASSERT_NO_FATAL_FAILURE(Capture(bare));
        const f64 coverage = ChangedFraction(child, bare);
        EXPECT_GT(coverage, 0.001) << "removing the grass should change visible terrain pixels";
        std::cout << "[ParentedTerrain] " << EvidenceName("ParentedTerrain", "Foliage")
                  << " foliage coverage fraction " << coverage << ", changed fraction vs root " << twinDiff << '\n';
    }

    // (a) and (e).
    TEST_P(TerrainLayerTextureEvidence, ALayerTextureRepeatsAcrossTheTerrainAndCoarsensWithDistance)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ASSERT_FALSE(HasFatalFailure());
        ScopedMockTime mockTime(kCaptureTime);
        const std::string path = PathName(GetParam());
        const Region middle = Centre(kSize / 4u);

        LookAtThePlain();
        std::vector<u8> textured;
        Capture(textured);
        ASSERT_FALSE(HasFatalFailure());
        WriteEvidence(EvidenceName("TerrainLayerTexture", "Top"), textured);

        const f64 texturedContrast = LumaContrast(textured, middle);
        // The widest run of one checker cell along the middle row. A cell is
        // 64 m / 8 tiles / 2 cells = 4 m, and the frame is 34.6 m across, so a
        // cell is about 30 px.
        const u32 nearCell = MeasureMiddleRow(textured, middle.Min, middle.Max).LongestRun;
        std::cout << "[TerrainLayerTexture] " << TestRenderBackend::EvidenceBackendPrefix() << path
                  << " plain: luma stddev " << texturedContrast << ", widest checker run " << nearCell << " px\n";

        EXPECT_GT(texturedContrast, 30.0)
            << "on " << path << ", the plain 11 to 29 m from the terrain's corner shows no checker. The layer arrays "
            << "tile: sampled past the first repeat they must repeat. A clamped array smears its border texels there.";
        EXPECT_GE(nearCell, 15u) << "on " << path << ", the checker's cells are smaller than the 4 m the tiling gives";
        EXPECT_LE(nearCell, 45u) << "on " << path << ", the checker's cells are larger than the 4 m the tiling gives";

        // The control: the same layers with no texture are their BaseColor, and
        // the same region is then flat. If it is not, the contrast above is the
        // light's or the geometry's, not the texture's.
        Material().GetLayer(0).AlbedoPath = FString();
        Material().GetLayer(1).AlbedoPath = FString();
        RebuildMaterial();
        std::vector<u8> solid;
        Capture(solid);
        ASSERT_FALSE(HasFatalFailure());
        WriteEvidence(EvidenceName("TerrainLayerTextureOff", "Top"), solid);
        const f64 solidContrast = LumaContrast(solid, middle);
        std::cout << "[TerrainLayerTexture] " << TestRenderBackend::EvidenceBackendPrefix() << path
                  << " plain, textures removed: luma stddev " << solidContrast << "\n";
        EXPECT_LT(solidContrast, 3.0) << "on " << path << ", the untextured plain is not flat";

        // (e) From 220 m every pixel is past the 160 m where only the far scale
        // is sampled. The far tiling is 8 * 0.137 = 1.096 repeats, so a cell is
        // 29 m of a frame 77.6 m across: about 96 px. At the near tiling it
        // would be 4 m: 13 px.
        Material().GetLayer(0).AlbedoPath = FString(m_Checker.generic_string());
        Material().GetLayer(1).AlbedoPath = FString(m_Stripes.generic_string());
        RebuildMaterial();
        LookDown(32.0f, 32.0f, 220.0f, 20.0f);
        std::vector<u8> distant;
        Capture(distant);
        ASSERT_FALSE(HasFatalFailure());
        WriteEvidence(EvidenceName("TerrainLayerTexture", "Far"), distant);
        // The plain's columns: the terrain is 64 m of the 77.6 m frame, and the
        // plain is its first 36 m.
        const f64 pixelsPerMetre = static_cast<f64>(kSize) / (2.0 * 220.0 * std::tan(glm::radians(10.0)));
        const u32 plainFirst = static_cast<u32>(kSize / 2.0 - 30.0 * pixelsPerMetre);
        const u32 plainLast = static_cast<u32>(kSize / 2.0 + 2.0 * pixelsPerMetre);
        const RowCells farRow = MeasureMiddleRow(distant, plainFirst, plainLast);
        const u32 farCell = farRow.LongestRun;
        std::cout << "[TerrainLayerTexture] " << TestRenderBackend::EvidenceBackendPrefix() << path
                  << " from 220 m: widest checker run " << farCell << " px, luma range " << farRow.LumaRange
                  << ", over columns " << plainFirst << ".." << plainLast << " (near-scale cells would be "
                  << 4.0 * pixelsPerMetre << " px)\n";
        // Both tones on the row and a boundary between them inside it: on a
        // flat row the run below is the row's own width, which would pass the
        // "larger than a near-scale cell" test having shown no checker.
        EXPECT_GT(farRow.LumaRange, 60.0) << "on " << path << ", from 220 m the plain shows no checker at all";
        EXPECT_LT(farCell, plainLast - plainFirst)
            << "on " << path << ", from 220 m the plain's middle row never crosses from one checker cell to another";
        EXPECT_GT(static_cast<f64>(farCell), 3.0 * 4.0 * pixelsPerMetre)
            << "on " << path << ", from 220 m the checker still has cells the size the near tiling gives. Beyond "
            << TerrainSurfaceShading::kFarDistance << " m a layer is sampled at " << TerrainSurfaceShading::kFarTilingRatio
            << " of its tiling.";
    }

    // (b) and (c).
    TEST_P(TerrainLayerTextureEvidence, TheMacroMapTintsAndWetsTheBlendedSurface)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ASSERT_FALSE(HasFatalFailure());
        ScopedMockTime mockTime(kCaptureTime);
        const std::string path = PathName(GetParam());
        const Region middle = Centre(kSize / 4u);

        const fs::path neutral = m_Directory / "macro_neutral.png";
        const fs::path tint = m_Directory / "macro_tint.png";
        const fs::path wet = m_Directory / "macro_wet.png";
        // Byte 128 is a multiplier of one; alpha 255 is dry.
        ASSERT_NO_FATAL_FAILURE(WriteSolid(neutral, 16, 128, 128, 128, 255));
        // Red at 64: half. Green and blue untouched.
        ASSERT_NO_FATAL_FAILURE(WriteSolid(tint, 16, 64, 128, 128, 255));
        // Neutral tint, alpha 0: fully wet.
        ASSERT_NO_FATAL_FAILURE(WriteSolid(wet, 16, 128, 128, 128, 0));

        LookAtThePlain();
        std::vector<u8> plain;
        Capture(plain);
        ASSERT_FALSE(HasFatalFailure());
        WriteEvidence(EvidenceName("TerrainLayerTextureOff", "TopMacro"), plain);

        // Determinism control: the same capture again. If this differs, the
        // comparisons below cannot attribute a difference to the macro map.
        std::vector<u8> again;
        Capture(again);
        ASSERT_FALSE(HasFatalFailure());
        const u32 noise = MaxChannelDifference(plain, again);
        ASSERT_LE(noise, 1u) << "on " << path << ", two captures with nothing changed differ by " << noise;

        SetMacroMap(neutral);
        std::vector<u8> withNeutral;
        Capture(withNeutral);
        ASSERT_FALSE(HasFatalFailure());
        WriteEvidence(EvidenceName("TerrainLayerTexture", "TopMacroNeutral"), withNeutral);
        ASSERT_TRUE(Material().GetMacroMap()) << "the neutral macro map did not load";
        const u32 neutralDifference = MaxChannelDifference(plain, withNeutral);

        SetMacroMap(tint);
        std::vector<u8> withTint;
        Capture(withTint);
        ASSERT_FALSE(HasFatalFailure());
        WriteEvidence(EvidenceName("TerrainLayerTexture", "TopMacroTint"), withTint);

        SetMacroMap(wet);
        std::vector<u8> withWet;
        Capture(withWet);
        ASSERT_FALSE(HasFatalFailure());
        WriteEvidence(EvidenceName("TerrainLayerTexture", "TopMacroWet"), withWet);

        const glm::dvec3 base = MeanLinearColour(plain, middle);
        const glm::dvec3 tinted = MeanLinearColour(withTint, middle);
        const glm::dvec3 wetted = MeanLinearColour(withWet, middle);
        const glm::dvec3 tintRatio = tinted / base;
        const glm::dvec3 wetRatio = wetted / base;
        std::cout << "[TerrainLayerTexture] " << TestRenderBackend::EvidenceBackendPrefix() << path << " macro: base "
                  << Text(base) << ", neutral max channel difference " << neutralDifference << ", tint ratio "
                  << Text(tintRatio) << ", wet ratio " << Text(wetRatio) << "\n";

        EXPECT_LE(neutralDifference, 2u)
            << "on " << path << ", a macro map of (128, 128, 128, 255) changed the frame. 128 is a tint of one and "
            << "255 is dry, so that map is the identity.";

        // The tint multiplies the albedo, and on a fully rough dielectric the
        // light leaving the surface is the albedo's diffuse reflection plus a
        // few percent of untinted specular: red at 64/128 is a little over half.
        EXPECT_GT(tintRatio.r, 0.42) << "on " << path;
        EXPECT_LT(tintRatio.r, 0.62) << "on " << path << ", the macro map's red 64 did not halve the surface's red";
        EXPECT_NEAR(tintRatio.g, 1.0, 0.05) << "on " << path << ", the macro map's green 128 moved the green";
        EXPECT_NEAR(tintRatio.b, 1.0, 0.05) << "on " << path << ", the macro map's blue 128 moved the blue";

        // Wet: albedo * 0.55, roughness 0.12. The camera is not in the sun's
        // mirror direction, so the smoother surface adds no highlight here and
        // the frame is the darker albedo.
        EXPECT_GT(MaxChannelDifference(plain, withWet), 8u) << "on " << path << ", the wet frame is the dry one";
        EXPECT_LT(wetRatio.g, 0.75) << "on " << path << ", full wetness did not darken the surface";
        EXPECT_GT(wetRatio.g, 0.40) << "on " << path << ", full wetness darkened the surface below albedo * 0.55";

        // And the map comes off again.
        SetMacroMap({});
        std::vector<u8> removed;
        Capture(removed);
        ASSERT_FALSE(HasFatalFailure());
        EXPECT_FALSE(Material().GetMacroMap());
        EXPECT_LE(MaxChannelDifference(plain, removed), noise + 1u)
            << "on " << path << ", removing the macro map did not give the first frame back";
    }

    // (d).
    TEST_P(TerrainLayerTextureEvidence, StrataRunHorizontallyOnACliff)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ASSERT_FALSE(HasFatalFailure());
        ScopedMockTime mockTime(kCaptureTime);
        const std::string path = PathName(GetParam());

        LookAtCliff();
        std::vector<u8> cliff;
        Capture(cliff);
        ASSERT_FALSE(HasFatalFailure());
        WriteEvidence(EvidenceName("TerrainLayerTexture", "Cliff"), cliff);

        // The middle of the frame is the cliff face between about 7 and 17 m up.
        // The stripes layer varies along texture V only, and on a side
        // projection V runs down world Y: bands 1 m tall, 8 to 9 px here.
        const Region face = Centre(kSize / 6u);
        const Banding banding = MeasureBanding(cliff, face);
        std::cout << "[TerrainLayerTexture] " << TestRenderBackend::EvidenceBackendPrefix() << path
                  << " cliff: row-mean stddev " << banding.AcrossRows << ", column-mean stddev "
                  << banding.AcrossColumns << "\n";
        EXPECT_GT(banding.AcrossRows, 15.0)
            << "on " << path << ", the cliff face shows no bands down the image: the stripes layer is not reaching it";
        EXPECT_GT(banding.AcrossRows, 4.0 * banding.AcrossColumns)
            << "on " << path << ", the stripes on the cliff are not horizontal. An X-facing face samples "
            << "(world Z, world Y): texture V has to run along world Y, or bedding planes stand on end.";
    }

    INSTANTIATE_TEST_SUITE_P(AllPaths, TerrainLayerTextureEvidence,
                             ::testing::Values(RenderingPath::Forward, RenderingPath::ForwardPlus,
                                               RenderingPath::Deferred),
                             [](const ::testing::TestParamInfo<RenderingPath>& info)
                             { return std::string(PathName(info.param)); });

    // ── The same surface through the virtual texture ─────────────────────────
    //
    // The tile bake (compute/TerrainVTTileBake.comp) is the third evaluator of
    // the layer blend, with its own sampling: it has no derivatives. It has to
    // put the SAME textured surface into the cache the fragment paths draw
    // per pixel — the same tiling, and the same side projections on a cliff.
    //
    // Not in the --olo-rhi=vulkan process: like TerrainVirtualTextureVisualEvidenceTest,
    // the feedback loop this waits on has not been ported to the windowless
    // Vulkan fixture.
    class TerrainLayerTextureVirtualTexture : public TerrainLayerTextureEvidence
    {
      protected:
        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return false;
        }

        static constexpr u32 kConvergeFrames = 48;

        // Small, fixed images and an uncompressed cache: the comparison is about
        // what the bake samples, not about the adaptive or BC7 machinery.
        void EnableVirtualTexture()
        {
            auto& terrain = m_Terrain.GetComponent<TerrainComponent>();
            terrain.m_VirtualTextureEnabled = true;
            terrain.m_VTVirtualPagesWide = 32;
            terrain.m_VTPageTexels = 64;
            terrain.m_VTBorderTexels = 4;
            terrain.m_VTCacheTilesWide = 12;
            terrain.m_VTMaxTileBakesPerFrame = 16;
            terrain.m_VTAdaptiveEnabled = false;
            terrain.m_VTTrilinearEnabled = false;
            terrain.m_VTCompressedCache = false;
        }

        void CaptureConverged(std::vector<u8>& out)
        {
            RunFrames(kConvergeFrames);
            u32 w = 0;
            u32 h = 0;
            ASSERT_TRUE(ReadbackComposite(out, w, h)) << "no composited frame to read back";
            ASSERT_EQ(w, kSize);
            ASSERT_EQ(h, kSize);
            const auto& terrain = m_Terrain.GetComponent<TerrainComponent>();
            ASSERT_TRUE(terrain.m_VirtualTexture && terrain.m_VirtualTexture->IsReadyForShading())
                << "the virtual texture never became ready, so this frame is the per-pixel path again";
            ASSERT_GT(terrain.m_VirtualTexture->GetStats().m_TilesBakedTotal, 0u) << "no tile was ever baked";
        }

        // Mean absolute 8-bit difference per channel over the region.
        [[nodiscard]] static f64 MeanAbsoluteDifference(const std::vector<u8>& a, const std::vector<u8>& b, Region region)
        {
            f64 sum = 0.0;
            u32 count = 0;
            for (u32 y = region.Min; y < region.Max; ++y)
            {
                for (u32 x = region.Min; x < region.Max; ++x)
                {
                    const sizet at = (static_cast<sizet>(y) * kSize + x) * 4u;
                    for (sizet c = 0; c < 3; ++c)
                    {
                        sum += std::abs(static_cast<int>(a[at + c]) - static_cast<int>(b[at + c]));
                        ++count;
                    }
                }
            }
            return sum / static_cast<f64>(count);
        }
    };

    TEST_P(TerrainLayerTextureVirtualTexture, TheTileBakeDrawsTheSameTexturedSurfaceAsThePerPixelPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ASSERT_FALSE(HasFatalFailure());
        ScopedMockTime mockTime(kCaptureTime);
        const std::string path = PathName(GetParam());
        const Region middle = Centre(kSize / 4u);
        const Region face = Centre(kSize / 6u);

        // The per-pixel references, before the virtual texture exists.
        LookAtThePlain();
        std::vector<u8> plainPerPixel;
        Capture(plainPerPixel);
        ASSERT_FALSE(HasFatalFailure());
        LookAtCliff();
        std::vector<u8> cliffPerPixel;
        Capture(cliffPerPixel);
        ASSERT_FALSE(HasFatalFailure());

        EnableVirtualTexture();
        LookAtThePlain();
        std::vector<u8> plainBaked;
        CaptureConverged(plainBaked);
        ASSERT_FALSE(HasFatalFailure());
        WriteEvidence(EvidenceName("TerrainLayerTexture", "TopVT"), plainBaked);
        LookAtCliff();
        std::vector<u8> cliffBaked;
        CaptureConverged(cliffBaked);
        ASSERT_FALSE(HasFatalFailure());
        WriteEvidence(EvidenceName("TerrainLayerTexture", "CliffVT"), cliffBaked);

        const f64 plainContrast = LumaContrast(plainBaked, middle);
        const u32 plainCell = MeasureMiddleRow(plainBaked, middle.Min, middle.Max).LongestRun;
        const f64 plainDifference = MeanAbsoluteDifference(plainBaked, plainPerPixel, middle);
        const Banding cliffBanding = MeasureBanding(cliffBaked, face);
        const f64 cliffDifference = MeanAbsoluteDifference(cliffBaked, cliffPerPixel, face);
        std::cout << "[TerrainLayerTexture] " << TestRenderBackend::EvidenceBackendPrefix() << path
                  << " virtual texture: plain luma stddev " << plainContrast << ", widest checker run " << plainCell
                  << " px, mean |baked - per-pixel| " << plainDifference << "/255; cliff row-mean stddev "
                  << cliffBanding.AcrossRows << ", column-mean stddev " << cliffBanding.AcrossColumns
                  << ", mean |baked - per-pixel| " << cliffDifference << "/255\n";

        // The plain: the checker is in the cache, at the tiling the per-pixel
        // path draws it at, and in the same place.
        EXPECT_GT(plainContrast, 30.0) << "on " << path << ", the baked plain shows no checker";
        EXPECT_GE(plainCell, 15u) << "on " << path << ", the baked checker's cells are not the per-pixel path's 4 m";
        EXPECT_LE(plainCell, 45u) << "on " << path << ", the baked checker's cells are not the per-pixel path's 4 m";
        // A checker displaced by half a cell differs by about half the tone
        // range everywhere (over 50/255 here); the same checker through a cache
        // of a different texel density differs only along its edges.
        EXPECT_LT(plainDifference, 16.0)
            << "on " << path << ", the baked plain is not the per-pixel plain: the bake samples the layers at "
            << "another tiling or another offset";

        // The cliff: the bake's side projections run texture V down world Y
        // too, at the same scale.
        EXPECT_GT(cliffBanding.AcrossRows, 15.0) << "on " << path << ", the baked cliff shows no bands";
        EXPECT_GT(cliffBanding.AcrossRows, 4.0 * cliffBanding.AcrossColumns)
            << "on " << path << ", the stripes on the baked cliff are not horizontal";
        EXPECT_LT(cliffDifference, 24.0)
            << "on " << path << ", the baked cliff is not the per-pixel cliff: the bake's side projection differs "
            << "from include/TerrainLayerSampling.glsl's";
    }

    INSTANTIATE_TEST_SUITE_P(AllPaths, TerrainLayerTextureVirtualTexture,
                             ::testing::Values(RenderingPath::Forward, RenderingPath::ForwardPlus,
                                               RenderingPath::Deferred),
                             [](const ::testing::TestParamInfo<RenderingPath>& info)
                             { return std::string(PathName(info.param)); });

    // ── The material half: GPU objects, no frame ─────────────────────────────

    class TerrainLayerTextureMaterial : public RendererAttachedTest
    {
      protected:
        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return true;
        }

        void BuildScene() override {}
    };

    // (f).
    TEST_F(TerrainLayerTextureMaterial, MaterialsWithIdenticalLayersShareOneSetOfArrays)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const fs::path directory = TempDir("shared-arrays");
        const fs::path checker = directory / "checker.png";
        ASSERT_NO_FATAL_FAILURE(WriteChecker(checker));

        const auto build = [&checker](TerrainMaterial& material)
        {
            material.AddLayer(MakeLayer("Textured", checker));
            TerrainLayer solid;
            solid.Name = "Solid";
            solid.BaseColor = glm::vec3(0.2f, 0.4f, 0.1f);
            material.AddLayer(solid);
        };

        TerrainMaterial first;
        build(first);
        first.BuildTextureArrays(128);
        ASSERT_TRUE(first.IsBuilt());

        // What the arrays are: they tile, the albedo decodes sRGB, the data
        // arrays do not, and all three are anisotropically filtered.
        const auto& albedoSpec = first.GetAlbedoArray()->GetSpecification();
        EXPECT_EQ(albedoSpec.Format, Texture2DArrayFormat::RGBA8_SRGB);
        EXPECT_TRUE(albedoSpec.RepeatWrap);
        EXPECT_FLOAT_EQ(albedoSpec.MaxAnisotropy, TerrainMaterial::kLayerAnisotropy);
        EXPECT_EQ(albedoSpec.Width, 128u);
        EXPECT_EQ(first.GetNormalArray()->GetSpecification().Format, Texture2DArrayFormat::RGBA8);
        EXPECT_TRUE(first.GetNormalArray()->GetSpecification().RepeatWrap);
        EXPECT_EQ(first.GetARMArray()->GetSpecification().Format, Texture2DArrayFormat::RGBA8);
        EXPECT_TRUE(first.GetARMArray()->GetSpecification().RepeatWrap);

        TerrainMaterial second;
        build(second);
        // Tiling and blend sharpness are per-terrain UBO values: two terrains
        // that differ only in them still sample the same arrays.
        second.GetLayer(0).TilingScale = 31.0f;
        second.GetLayer(1).HeightBlendSharpness = 9.0f;
        second.BuildTextureArrays(128);
        ASSERT_TRUE(second.IsBuilt());
        EXPECT_EQ(first.GetLayerArrays().get(), second.GetLayerArrays().get())
            << "two materials with the same layer files, fallback values and resolution built two sets of arrays";
        EXPECT_EQ(first.GetAlbedoArray()->GetRHIHandle(), second.GetAlbedoArray()->GetRHIHandle());
        EXPECT_EQ(first.GetNormalArray()->GetRHIHandle(), second.GetNormalArray()->GetRHIHandle());
        EXPECT_EQ(first.GetARMArray()->GetRHIHandle(), second.GetARMArray()->GetRHIHandle());

        // Anything the texels are built from separates them.
        TerrainMaterial otherColour;
        build(otherColour);
        otherColour.GetLayer(1).BaseColor = glm::vec3(0.2f, 0.4f, 0.11f);
        otherColour.BuildTextureArrays(128);
        ASSERT_TRUE(otherColour.IsBuilt());
        EXPECT_NE(first.GetLayerArrays().get(), otherColour.GetLayerArrays().get())
            << "a different fallback colour shared the arrays of another material";

        TerrainMaterial otherResolution;
        build(otherResolution);
        otherResolution.BuildTextureArrays(64);
        ASSERT_TRUE(otherResolution.IsBuilt());
        EXPECT_NE(first.GetLayerArrays().get(), otherResolution.GetLayerArrays().get())
            << "a different layer resolution shared the arrays of another material";
        EXPECT_EQ(otherResolution.GetAlbedoArray()->GetSpecification().Width, 64u);

        TerrainMaterial otherFile;
        otherFile.AddLayer(MakeLayer("Textured", checker));
        otherFile.GetLayer(0).AlbedoPath = FString();
        TerrainLayer solid;
        solid.Name = "Solid";
        solid.BaseColor = glm::vec3(0.2f, 0.4f, 0.1f);
        otherFile.AddLayer(solid);
        otherFile.BuildTextureArrays(128);
        ASSERT_TRUE(otherFile.IsBuilt());
        EXPECT_NE(first.GetLayerArrays().get(), otherFile.GetLayerArrays().get())
            << "a layer without its albedo file shared the arrays of the material that has it";
    }

    // (g).
    TEST_F(TerrainLayerTextureMaterial, ASplatmapFileFillsTheCpuMirrorTheMasksRead)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const fs::path directory = TempDir("splat-mirror");

        // Every texel distinct and every ROW distinct, so a flipped or
        // transposed mirror cannot pass. Red rises along X, green along Y,
        // blue is empty, alpha is full.
        constexpr u32 kGrid = 8;
        std::vector<u8> written(static_cast<sizet>(kGrid) * kGrid * 4u);
        for (u32 y = 0; y < kGrid; ++y)
        {
            for (u32 x = 0; x < kGrid; ++x)
            {
                const sizet at = (static_cast<sizet>(y) * kGrid + x) * 4u;
                written[at] = static_cast<u8>(x * 30u);
                written[at + 1] = static_cast<u8>(y * 30u);
                written[at + 2] = 0;
                written[at + 3] = 255;
            }
        }
        const fs::path plane0 = directory / "splat0.png";
        ASSERT_NO_FATAL_FAILURE(WritePng(plane0, kGrid, kGrid, 4, written));

        TerrainMaterial material;
        material.AddLayer(TerrainLayer{});
        ASSERT_FALSE(material.HasCPUSplatmaps());
        material.SetSplatmapPath(0, plane0.generic_string());
        material.LoadSplatmaps();

        ASSERT_TRUE(material.HasCPUSplatmaps()) << "a splatmap loaded from a file left the CPU mirror empty";
        EXPECT_EQ(material.GetSplatmapResolution(), kGrid);
        EXPECT_FALSE(material.AreCPUSplatmapsStale());
        const TArray<u8>& mirror0 = std::as_const(material).GetSplatmapData(0);
        ASSERT_EQ(static_cast<sizet>(mirror0.Num()), written.size());
        EXPECT_TRUE(std::equal(written.begin(), written.end(), mirror0.GetData()))
            << "the CPU mirror does not hold the file's weights, row 0 first";

        // The plane with no file is all zeros, and exists on the GPU too: its
        // four layers have no weight, not whatever an unbound slot holds.
        const TArray<u8>& mirror1 = std::as_const(material).GetSplatmapData(1);
        ASSERT_EQ(static_cast<sizet>(mirror1.Num()), written.size());
        EXPECT_TRUE(std::all_of(mirror1.GetData(), mirror1.GetData() + mirror1.Num(), [](u8 v)
                                { return v == 0; }));
        ASSERT_TRUE(material.GetSplatmap(0));
        ASSERT_TRUE(material.GetSplatmap(1));

        // The GPU texture holds the same bytes: it is a linear RGBA8 upload of
        // the mirror, so nothing decodes the weights on the way.
        EXPECT_FALSE(material.GetSplatmap(0)->GetSpecification().SRGB);
        TArray64<u8> onGpu;
        ASSERT_TRUE(material.GetSplatmap(0)->GetData(onGpu));
        ASSERT_EQ(static_cast<sizet>(onGpu.Num()), written.size());
        EXPECT_TRUE(std::equal(written.begin(), written.end(), onGpu.GetData()))
            << "the GPU splatmap does not hold the weights the CPU mirror reports";

        // Two planes of different sizes are one weight field on two grids:
        // refused, with the material left as it was.
        const u64 revision = material.GetSplatmapRevision();
        const fs::path plane1 = directory / "splat1.png";
        ASSERT_NO_FATAL_FAILURE(WriteSolid(plane1, 16, 255, 0, 0, 0));
        material.SetSplatmapPath(1, plane1.generic_string());
        material.LoadSplatmaps();
        EXPECT_EQ(material.GetSplatmapRevision(), revision) << "a 16x16 plane 1 was accepted beside an 8x8 plane 0";
        EXPECT_EQ(material.GetSplatmapResolution(), kGrid);
        EXPECT_TRUE(std::equal(written.begin(), written.end(), std::as_const(material).GetSplatmapData(0).GetData()));
        material.SetSplatmapPath(1, FString());

        // What the mirror is for: a foliage layer masked on a splat channel.
        // Channel 2 (blue) is zero everywhere, channel 3 (alpha) is full.
        TerrainData terrain;
        terrain.CreateFlat(16u);
        const auto place = [&](i32 channel)
        {
            TArray<FoliageLayer> layers;
            layers.Add(FoliageLayer{});
            layers[0].SplatmapChannel = channel;
            auto plants = Ref<FoliageRenderer>::Create();
            plants->GenerateInstances(layers, terrain, &material, 8.0f, 8.0f, 1.0f);
            return plants->GetTotalInstanceCount();
        };
        const auto onFullChannel = place(3);
        const auto onEmptyChannel = place(2);
        std::cout << "[TerrainLayerTexture] foliage on a file-loaded splatmap: " << onFullChannel
                  << " instances on the full channel, " << onEmptyChannel << " on the empty one\n";
        EXPECT_GT(onFullChannel, 0u) << "a layer masked on a channel that is 255 everywhere placed nothing";
        EXPECT_EQ(onEmptyChannel, 0u)
            << "a layer masked on a channel that is zero everywhere placed plants: the mask was dropped";
    }
} // namespace OloEngine::Tests

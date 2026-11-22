// OLO_TEST_LAYER: unit
// =============================================================================
// TerrainLayerTextureContractTest.cpp — the CPU half of textured terrain layers
// (issue #1721).
//
// Three contracts, none of which needs a GPU:
//
//   * THE RESAMPLING FILTER. A layer file of another size than the material's
//     layer resolution used to be POINT-sampled: a 2048 px scan built at 512
//     kept one texel in sixteen. TerrainMaterial::ResampleRGBA8 averages by
//     area when an axis shrinks and interpolates between texel centres (with
//     wrap-around, a layer tiles) when it grows.
//   * THE sRGB ENCODE. The albedo array is sRGB, and a layer with no albedo
//     file is a solid colour from TerrainLayer::BaseColor, which is LINEAR. It
//     is encoded on the way in so the sampler decodes it back to what was
//     authored.
//   * THE MACRO MAP AND THE TWO-SCALE BLEND as TerrainSurfaceShading.h states
//     them: the values the shaders are fed from, and the CPU twins of the two
//     functions in include/TerrainLayerBlend.glsl.
//
// The rendered half is Rendering/PropertyTests/TerrainLayerTextureEvidenceTest.cpp.
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Terrain/TerrainSurfaceShading.h"

#include "../Rendering/ReSTIR/ShaderSourceScan.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <span>
#include <string>

namespace OloEngine::Tests
{
    namespace
    {
        [[nodiscard]] std::span<const u8> Bytes(const TArray<u8>& pixels)
        {
            return { pixels.GetData(), static_cast<sizet>(pixels.Num()) };
        }

        // An image whose every channel of texel (x, y) is values[y * width + x],
        // with alpha 255.
        [[nodiscard]] TArray<u8> Grey(std::span<const u8> values)
        {
            TArray<u8> pixels;
            for (const u8 value : values)
            {
                pixels.Add(value);
                pixels.Add(value);
                pixels.Add(value);
                pixels.Add(255);
            }
            return pixels;
        }

        // The sRGB encode by its definition, for the table-driven one to be
        // compared with.
        [[nodiscard]] u8 ReferenceEncode(const f64 linear)
        {
            const f64 clamped = std::clamp(linear, 0.0, 1.0);
            const f64 encoded = clamped <= 0.0031308 ? clamped * 12.92 : 1.055 * std::pow(clamped, 1.0 / 2.4) - 0.055;
            return static_cast<u8>(std::lround(encoded * 255.0));
        }
    } // namespace

    TEST(TerrainLayerTextureContract, AMinifiedCheckerIsItsMeanNotOneOfItsCorners)
    {
        // 2x2 black/white checker -> 1x1. A point sample returns 0 or 255.
        const std::array<u8, 4> checker{ 0, 255, 255, 0 };
        const TArray<u8> source = Grey(checker);

        TArray<u8> linearData;
        TerrainMaterial::ResampleRGBA8(Bytes(source), 2, 2, 1, 1, false, linearData);
        ASSERT_EQ(linearData.Num(), 4);
        // Data (a normal map, an ARM map) averages its bytes: 127.5, rounded.
        EXPECT_EQ(linearData[0], 128);
        EXPECT_EQ(linearData[1], 128);
        EXPECT_EQ(linearData[2], 128);
        EXPECT_EQ(linearData[3], 255);

        TArray<u8> colour;
        TerrainMaterial::ResampleRGBA8(Bytes(source), 2, 2, 1, 1, true, colour);
        ASSERT_EQ(colour.Num(), 4);
        // Colour averages LIGHT: half of white is linear 0.5, which sRGB stores
        // as 188. Averaging the bytes instead (128) is linear 0.216 — a minified
        // high-contrast albedo that came out less than half as bright.
        EXPECT_EQ(colour[0], 188);
        EXPECT_EQ(colour[0], ReferenceEncode(0.5));
        EXPECT_EQ(colour[3], 255);
    }

    TEST(TerrainLayerTextureContract, EveryDestinationTexelAveragesTheSourceAreaItCovers)
    {
        // 4x4 -> 2x2: each destination texel is the mean of its own 2x2 block.
        const std::array<u8, 16> values{ 10, 20, 100, 120, //
                                         30, 40, 140, 160, //
                                         200, 210, 0, 0,   //
                                         220, 230, 0, 4 };
        TArray<u8> out;
        TerrainMaterial::ResampleRGBA8(Bytes(Grey(values)), 4, 4, 2, 2, false, out);
        ASSERT_EQ(out.Num(), 16);
        EXPECT_EQ(out[0], 25);  // (10 + 20 + 30 + 40) / 4
        EXPECT_EQ(out[4], 130); // (100 + 120 + 140 + 160) / 4
        EXPECT_EQ(out[8], 215); // (200 + 210 + 220 + 230) / 4
        EXPECT_EQ(out[12], 1);  // (0 + 0 + 0 + 4) / 4

        // 3 -> 2 along X: a destination texel covers 1.5 source texels, so the
        // middle one is shared half and half.
        const std::array<u8, 3> row{ 0, 90, 240 };
        TerrainMaterial::ResampleRGBA8(Bytes(Grey(row)), 3, 1, 2, 1, false, out);
        ASSERT_EQ(out.Num(), 8);
        EXPECT_EQ(out[0], 30);  // (0 * 1 + 90 * 0.5) / 1.5
        EXPECT_EQ(out[4], 190); // (90 * 0.5 + 240 * 1) / 1.5
    }

    TEST(TerrainLayerTextureContract, AMagnifiedImageIsInterpolatedAndWrapsAtItsEdges)
    {
        // 2 -> 4 along X. Destination texel centres fall at source positions
        // -0.25, 0.25, 0.75, 1.25, and a layer tiles, so -0.25 lies between the
        // LAST source texel and the first.
        const std::array<u8, 2> row{ 0, 200 };
        TArray<u8> out;
        TerrainMaterial::ResampleRGBA8(Bytes(Grey(row)), 2, 1, 4, 1, false, out);
        ASSERT_EQ(out.Num(), 16);
        EXPECT_EQ(out[0], 50);   // 0.75 * 0 + 0.25 * 200 (wrapped)
        EXPECT_EQ(out[4], 50);   // 0.75 * 0 + 0.25 * 200
        EXPECT_EQ(out[8], 150);  // 0.25 * 0 + 0.75 * 200
        EXPECT_EQ(out[12], 150); // 0.75 * 200 + 0.25 * 0 (wrapped)
    }

    TEST(TerrainLayerTextureContract, ResamplingToTheSameSizeChangesNothing)
    {
        const std::array<u8, 4> values{ 1, 77, 201, 255 };
        const TArray<u8> source = Grey(values);
        TArray<u8> out;
        TerrainMaterial::ResampleRGBA8(Bytes(source), 2, 2, 2, 2, false, out);
        EXPECT_EQ(out, source);
        // The colour path decodes and re-encodes every channel; that round trip
        // must be the identity on bytes or an unresampled albedo would drift.
        TerrainMaterial::ResampleRGBA8(Bytes(source), 2, 2, 2, 2, true, out);
        EXPECT_EQ(out, source);
    }

    TEST(TerrainLayerTextureContract, TheSRGBEncodeIsTheStandardCurveAndInvertsTheDecode)
    {
        for (u32 byte = 0; byte < 256; ++byte)
        {
            EXPECT_EQ(TerrainMaterial::LinearToSRGB8(TerrainMaterial::SRGB8ToLinear(static_cast<u8>(byte))), byte)
                << "sRGB byte " << byte << " does not survive decode + encode";
        }

        // Against the definition, at values that are not byte centres.
        for (u32 step = 0; step <= 4000; ++step)
        {
            const f32 linear = static_cast<f32>(step) / 4000.0f;
            const int encoded = TerrainMaterial::LinearToSRGB8(linear);
            const int reference = ReferenceEncode(static_cast<f64>(linear));
            // Equal, except within float rounding of a byte boundary.
            EXPECT_LE(std::abs(encoded - reference), 1) << "linear " << linear;
        }

        EXPECT_EQ(TerrainMaterial::LinearToSRGB8(0.0f), 0);
        EXPECT_EQ(TerrainMaterial::LinearToSRGB8(-1.0f), 0);
        EXPECT_EQ(TerrainMaterial::LinearToSRGB8(1.0f), 255);
        EXPECT_EQ(TerrainMaterial::LinearToSRGB8(7.0f), 255);
        EXPECT_EQ(TerrainMaterial::LinearToSRGB8(std::numeric_limits<f32>::quiet_NaN()), 0);
    }

    TEST(TerrainLayerTextureContract, ASolidLayerColourSurvivesTheSRGBArray)
    {
        // What a layer without an albedo file goes through: BaseColor (linear)
        // -> LinearToSRGB8 -> the sRGB array -> the sampler's decode. The value
        // that comes back is within half an sRGB step of the authored one, and
        // an sRGB step is at most 2.4 / 1.055 / 255 of linear range (at white).
        constexpr f32 kWorstHalfStep = 0.5f * 2.4f / 1.055f / 255.0f + 1e-4f;
        for (u32 step = 0; step <= 1000; ++step)
        {
            const f32 authored = static_cast<f32>(step) / 1000.0f;
            const f32 sampled = TerrainMaterial::SRGB8ToLinear(TerrainMaterial::LinearToSRGB8(authored));
            EXPECT_NEAR(sampled, authored, kWorstHalfStep) << "authored linear " << authored;
        }
        // The default layer colour, 0.5: stored as 188, sampled as 0.5029.
        EXPECT_EQ(TerrainMaterial::LinearToSRGB8(0.5f), 188);
        EXPECT_NEAR(TerrainMaterial::SRGB8ToLinear(188), 0.5029f, 5e-4f);
    }

    TEST(TerrainLayerTextureContract, TheLayerResolutionIsAPowerOfTwoInItsRange)
    {
        EXPECT_EQ(TerrainMaterial::SanitizeLayerResolution(0), 64u);
        EXPECT_EQ(TerrainMaterial::SanitizeLayerResolution(63), 64u);
        EXPECT_EQ(TerrainMaterial::SanitizeLayerResolution(64), 64u);
        EXPECT_EQ(TerrainMaterial::SanitizeLayerResolution(100), 64u);
        EXPECT_EQ(TerrainMaterial::SanitizeLayerResolution(512), 512u);
        EXPECT_EQ(TerrainMaterial::SanitizeLayerResolution(1000), 512u);
        EXPECT_EQ(TerrainMaterial::SanitizeLayerResolution(2048), 2048u);
        EXPECT_EQ(TerrainMaterial::SanitizeLayerResolution(4096), 4096u);
        EXPECT_EQ(TerrainMaterial::SanitizeLayerResolution(5000), 4096u);
        EXPECT_EQ(TerrainMaterial::SanitizeLayerResolution(0xFFFFFFFFu), 4096u);

        // A material is 512 until told otherwise: what every scene was built at.
        TerrainMaterial material;
        EXPECT_EQ(material.GetLayerResolution(), 512u);
        material.SetLayerResolution(1500);
        EXPECT_EQ(material.GetLayerResolution(), 1024u);
    }

    TEST(TerrainLayerTextureContract, ANeutralMacroTexelLeavesTheSurfaceAsItWas)
    {
        using namespace TerrainSurfaceShading;
        const glm::vec3 albedo(0.31f, 0.47f, 0.12f);
        const f32 roughness = 0.83f;

        // Byte 128 in RGB, 255 in alpha: what an untouched macro map holds.
        const glm::vec4 neutral(static_cast<f32>(kMacroNeutralTint) / 255.0f, static_cast<f32>(kMacroNeutralTint) / 255.0f,
                                static_cast<f32>(kMacroNeutralTint) / 255.0f, static_cast<f32>(kMacroDryAlpha) / 255.0f);
        const MacroResult result = ApplyMacro(albedo, roughness, neutral);
        EXPECT_NEAR(result.Albedo.r, albedo.r, 1e-6f);
        EXPECT_NEAR(result.Albedo.g, albedo.g, 1e-6f);
        EXPECT_NEAR(result.Albedo.b, albedo.b, 1e-6f);
        EXPECT_FLOAT_EQ(result.Roughness, roughness);
    }

    TEST(TerrainLayerTextureContract, TheMacroTintScalesAndWetnessDarkensAndSmooths)
    {
        using namespace TerrainSurfaceShading;
        const glm::vec3 albedo(0.4f, 0.4f, 0.4f);
        const f32 roughness = 0.9f;

        // Tint: 64 halves a channel, 255 nearly doubles it, 128 leaves it.
        const MacroResult tinted =
            ApplyMacro(albedo, roughness, glm::vec4(64.0f / 255.0f, 128.0f / 255.0f, 255.0f / 255.0f, 1.0f));
        EXPECT_NEAR(tinted.Albedo.r, 0.4f * 64.0f / 128.0f, 1e-6f);
        EXPECT_NEAR(tinted.Albedo.g, 0.4f, 1e-6f);
        EXPECT_NEAR(tinted.Albedo.b, 0.4f * 255.0f / 128.0f, 1e-6f);
        EXPECT_FLOAT_EQ(tinted.Roughness, roughness);

        // Fully wet (alpha 0): albedo * 0.55, roughness 0.12.
        const MacroResult wet = ApplyMacro(albedo, roughness, glm::vec4(glm::vec3(128.0f / 255.0f), 0.0f));
        EXPECT_NEAR(wet.Albedo.r, 0.4f * 0.55f, 1e-6f);
        EXPECT_NEAR(wet.Roughness, 0.12f, 1e-6f);

        // Half wet: half way on both.
        const MacroResult damp = ApplyMacro(albedo, roughness, glm::vec4(glm::vec3(128.0f / 255.0f), 0.5f));
        EXPECT_NEAR(damp.Albedo.r, 0.4f * 0.775f, 1e-6f);
        EXPECT_NEAR(damp.Roughness, 0.51f, 1e-6f);
    }

    TEST(TerrainLayerTextureContract, TheFarScaleFadesInBetweenTheTwoDistances)
    {
        using namespace TerrainSurfaceShading;
        EXPECT_FLOAT_EQ(FarScaleBlend(0.0f), 0.0f);
        EXPECT_FLOAT_EQ(FarScaleBlend(kNearDistance), 0.0f);
        EXPECT_FLOAT_EQ(FarScaleBlend(0.5f * (kNearDistance + kFarDistance)), 0.5f);
        EXPECT_FLOAT_EQ(FarScaleBlend(kFarDistance), 1.0f);
        EXPECT_FLOAT_EQ(FarScaleBlend(10000.0f), 1.0f);
        f32 previous = 0.0f;
        for (f32 distance = kNearDistance; distance <= kFarDistance; distance += 1.0f)
        {
            const f32 blend = FarScaleBlend(distance);
            EXPECT_GE(blend, previous) << "the far scale faded back out at " << distance << " m";
            previous = blend;
        }

        // The contract values, pinned: a scene's content is authored against
        // them.
        EXPECT_FLOAT_EQ(kFarTilingRatio, 0.137f);
        EXPECT_FLOAT_EQ(kNearDistance, 40.0f);
        EXPECT_FLOAT_EQ(kFarDistance, 160.0f);
        // Not a divisor of one: an integer ratio would line the two grids up.
        EXPECT_GT(std::abs(1.0f / kFarTilingRatio - std::round(1.0f / kFarTilingRatio)), 0.2f);
    }

    TEST(TerrainLayerTextureContract, TheUniformLanesAreZeroWhenTheFeatureIsOff)
    {
        using namespace TerrainSurfaceShading;
        // Zero is what the shaders test for: no far scale, no macro step.
        const glm::vec4 singleScale = PackLayerScale(false);
        EXPECT_FLOAT_EQ(singleScale.x, 0.0f);
        const glm::vec4 noMacro = PackMacroParams(false);
        EXPECT_FLOAT_EQ(noMacro.x, 0.0f);

        const glm::vec4 twoScale = PackLayerScale(true);
        EXPECT_FLOAT_EQ(twoScale.x, kFarTilingRatio);
        EXPECT_FLOAT_EQ(twoScale.y, kNearDistance);
        EXPECT_FLOAT_EQ(twoScale.z, kFarDistance);
        const glm::vec4 macro = PackMacroParams(true);
        EXPECT_FLOAT_EQ(macro.x, 1.0f);
        EXPECT_FLOAT_EQ(macro.y, 255.0f / 128.0f);
        EXPECT_FLOAT_EQ(macro.z, 0.55f);
        EXPECT_FLOAT_EQ(macro.w, 0.12f);
    }

    // A voxel surface samples a world position in metres, and the tile a
    // layer's TilingScale counts repeats across need not be square: each of the
    // three projections takes the scale of the axis the heightfield's own
    // projection runs along (include/TerrainVoxelTiling.glsl). Counted in all
    // four voxel surface shaders, so one written back to a single scalar fails.
    TEST(TerrainLayerTextureContract, AVoxelWallTakesTheTilesScalePerAxis)
    {
        const auto count = [](const std::string& text, const std::string& needle)
        {
            u32 n = 0;
            for (sizet at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
                ++n;
            return n;
        };
        for (const char* shader : { "Terrain_Voxel.glsl", "Terrain_Voxel_GBuffer.glsl", "Terrain_VoxelGreedy.glsl",
                                    "Terrain_VoxelGreedy_GBuffer.glsl" })
        {
            const std::filesystem::path path = ShaderScan::ResolveShaderPath(shader);
            ASSERT_TRUE(std::filesystem::exists(path)) << path.string();
            std::string source = ShaderScan::ReadTextFile(path);
            std::erase_if(source, [](const char c)
                          { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; });
            EXPECT_EQ(count(source, "#include\"include/TerrainVoxelTiling.glsl\""), 1u) << shader;
            // Albedo, ARM and normal, once per projection.
            EXPECT_EQ(count(source, "vec3(wpTri.yz*tiling.y,"), 3u) << shader << ": X-facing faces take the Z axis' scale";
            EXPECT_EQ(count(source, "vec3(wpTri.xz*tiling,"), 3u) << shader << ": the top projection takes both";
            EXPECT_EQ(count(source, "vec3(wpTri.xy*tiling.x,"), 3u) << shader << ": Z-facing faces take the X axis' scale";
            EXPECT_EQ(count(source, "wpTri."), 9u) << shader << " samples a projection this test does not know";
        }

        // The helper divides by the tile, per axis, and keeps the old default
        // when a draw uploads no tile or no tiling.
        std::string helper = ShaderScan::ReadTextFile(ShaderScan::ResolveShaderPath("include/TerrainVoxelTiling.glsl"));
        std::erase_if(helper, [](const char c)
                      { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; });
        EXPECT_EQ(count(helper, "returnvec2(repeatsAcrossTile)/tileSize;"), 1u);
        EXPECT_EQ(count(helper, "vec2tileSize=u_WorldSizeAndHeightScale.xy;"), 1u);
        EXPECT_EQ(count(helper, "returnvec2(0.1);"), 1u);
    }

    // The scene file. Drift.olo's island blocks are written by a Python tool,
    // so the engine's own WRITE of the material keys is exercised here and
    // nowhere else: a material goes out through SceneSerializer and comes back
    // with its maps, its layer-array resolution and its layers as authored.
    TEST(TerrainLayerTextureContract, AMaterialRoundTripsThroughTheSceneFile)
    {
        auto scene = Scene::Create();
        Entity entity = scene->CreateEntity("Terrain");
        const UUID uuid = entity.GetUUID();
        {
            auto& terrain = entity.AddComponent<TerrainComponent>();
            terrain.m_HeightmapPath = "Assets/Terrain/Test/Height.png";
            terrain.m_Material = Ref<TerrainMaterial>::Create();
            terrain.m_Material->SetSplatmapPath(0, "Assets/Terrain/Test/Splat0.png");
            terrain.m_Material->SetSplatmapPath(1, "Assets/Terrain/Test/Splat1.png");
            terrain.m_Material->SetMacroMapPath("Assets/Terrain/Test/Macro.png");
            terrain.m_Material->SetLayerResolution(1024);
            TerrainLayer rock;
            rock.Name = "Rock";
            rock.AlbedoPath = "Assets/Terrain/Test/Rock_Color.jpg";
            rock.NormalPath = "Assets/Terrain/Test/Rock_Normal.jpg";
            rock.ARMPath = "Assets/Terrain/Test/Rock_ARM.png";
            rock.TilingScale = 175.0f;
            rock.BaseColor = glm::vec3(0.21f, 0.2f, 0.19f);
            terrain.m_Material->AddLayer(rock);
            TerrainLayer turf;
            turf.Name = "Turf";
            turf.TilingScale = 120.0f;
            terrain.m_Material->AddLayer(turf);
        }

        const std::string yaml = SceneSerializer(scene).SerializeToYAML();
        auto loaded = Scene::Create();
        ASSERT_TRUE(SceneSerializer(loaded).DeserializeFromYAML(yaml));
        Entity reloaded = loaded->GetEntityByUUID(uuid);
        ASSERT_TRUE(reloaded);
        const auto& terrain = reloaded.GetComponent<TerrainComponent>();
        ASSERT_TRUE(terrain.m_Material);
        const TerrainMaterial& material = *terrain.m_Material;
        EXPECT_EQ(terrain.m_HeightmapPath, "Assets/Terrain/Test/Height.png");
        EXPECT_EQ(material.GetSplatmapPath(0).ToStdString(), "Assets/Terrain/Test/Splat0.png");
        EXPECT_EQ(material.GetSplatmapPath(1).ToStdString(), "Assets/Terrain/Test/Splat1.png");
        EXPECT_EQ(material.GetMacroMapPath().ToStdString(), "Assets/Terrain/Test/Macro.png");
        EXPECT_EQ(material.GetLayerResolution(), 1024u);
        ASSERT_EQ(material.GetLayerCount(), 2u);
        EXPECT_EQ(material.GetLayer(0).Name.ToStdString(), "Rock");
        EXPECT_EQ(material.GetLayer(0).AlbedoPath.ToStdString(), "Assets/Terrain/Test/Rock_Color.jpg");
        EXPECT_EQ(material.GetLayer(0).NormalPath.ToStdString(), "Assets/Terrain/Test/Rock_Normal.jpg");
        EXPECT_EQ(material.GetLayer(0).ARMPath.ToStdString(), "Assets/Terrain/Test/Rock_ARM.png");
        EXPECT_FLOAT_EQ(material.GetLayer(0).TilingScale, 175.0f);
        EXPECT_FLOAT_EQ(material.GetLayer(0).BaseColor.g, 0.2f);
        EXPECT_FLOAT_EQ(material.GetLayer(1).TilingScale, 120.0f);

        // Saved again, the text is the same: nothing is dropped or defaulted on
        // the way round.
        EXPECT_EQ(SceneSerializer(loaded).SerializeToYAML(), yaml);

        // A scene written before the two keys existed has no macro map and the
        // resolution every material was built at then.
        YAML::Node document = YAML::Load(yaml);
        bool removed = false;
        for (auto entityNode : document["Entities"])
        {
            YAML::Node terrainNode = entityNode["TerrainComponent"];
            if (!terrainNode)
                continue;
            ASSERT_TRUE(terrainNode["MacroMapPath"]) << "MacroMapPath is not written";
            ASSERT_TRUE(terrainNode["LayerResolution"]) << "LayerResolution is not written";
            terrainNode.remove("MacroMapPath");
            terrainNode.remove("LayerResolution");
            removed = true;
        }
        ASSERT_TRUE(removed) << "the saved scene has no TerrainComponent";
        const std::string older = YAML::Dump(document);
        auto fromOlder = Scene::Create();
        ASSERT_TRUE(SceneSerializer(fromOlder).DeserializeFromYAML(older));
        Entity olderEntity = fromOlder->GetEntityByUUID(uuid);
        ASSERT_TRUE(olderEntity);
        const auto& olderTerrain = olderEntity.GetComponent<TerrainComponent>();
        ASSERT_TRUE(olderTerrain.m_Material);
        EXPECT_TRUE(olderTerrain.m_Material->GetMacroMapPath().ToStdString().empty());
        EXPECT_EQ(olderTerrain.m_Material->GetLayerResolution(), TerrainMaterial::kDefaultLayerResolution);
        EXPECT_EQ(olderTerrain.m_Material->GetLayerCount(), 2u);
    }
} // namespace OloEngine::Tests

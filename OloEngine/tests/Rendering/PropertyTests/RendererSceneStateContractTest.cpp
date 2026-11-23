// OLO_TEST_LAYER: meta
//
// What a scene switch drops from the renderer (issues #1689, #1665; ADR 0025).
//
// Renderer3D::ResetSceneState is the renderer's scene switch. Everything the
// previous world published into the renderer, and every cache a pass built
// from that world's content, must be gone after it, or the next world draws
// the previous one's fog, wind or probe light for its first frames. In the
// test binary the same carry-over moves one test's numbers by which test ran
// before it (DDGIVisualEvidenceTest.LightLeakRegression's dark band, #1665).
//
// The rule is a classification, so these tests read the declarations that
// carry it:
//
//   * every by-reference Get*Settings() accessor returns either a
//     RendererSceneState member (scene publication, reset by assignment) or
//     a struct listed below as configuration, with the reason;
//   * a reset really does return every scene-published struct to its default;
//   * every pass with a frame sequence of its own (a ResetFrameSequence
//     override: the passes that keep state from one frame to the next) says
//     whether that state is a world cache, by overriding DropWorldCache or by
//     an entry below that names what it keeps instead.
//
// The table behind the classification:
// docs/agent-rules/cross-test-renderer-state-classification.md.
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/Renderer3D.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <string>

namespace fs = std::filesystem;

namespace OloEngine::Tests
{
    namespace
    {
        fs::path RepoRoot()
        {
            return fs::path(OLO_TEST_EDITOR_ROOT).parent_path();
        }

        std::string ReadText(const fs::path& path)
        {
            std::ifstream in(path, std::ios::binary);
            return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
        }

        // Configuration: written by the user, the editor panels, the MCP tools
        // or quality tiering, and kept across a scene switch, a view reset and
        // a renderer restart. Everything else a Get*Settings() accessor returns
        // is the scene's.
        const std::map<std::string, std::string, std::less<>>& ConfigurationAccessors()
        {
            static const std::map<std::string, std::string, std::less<>> list = {
                { "GetRendererSettings",
                  "the view's renderer configuration: activation publishes the scene's path, and quality tiering and "
                  "the user's panel choices go on top and must survive a scene reset" },
                { "GetPostProcessSettings",
                  "the view's post-process stack (#1352): activation publishes the authored stack, and tiering and "
                  "the user's debug taps go on top of it" },
            };
            return list;
        }

        // Passes with a frame sequence of their own whose cross-frame state is
        // not built from the world's content.
        const std::map<std::string, std::string, std::less<>>& PassesWithoutWorldCache()
        {
            static const std::map<std::string, std::string, std::less<>> list = {
                { "FSR2RenderPass", "screen-space upscaler history, invalidated through TemporalHistoryRegistry" },
                { "GTAORenderPass", "a screen-space temporal rotation index" },
                { "ReSTIRGIPass", "screen-space reservoirs, invalidated through TemporalHistoryRegistry" },
                { "ReSTIRPTPass", "screen-space reservoir ping-pong, invalidated through TemporalHistoryRegistry" },
                { "ToneMapRenderPass", "the view's exposure adaptation, which follows the camera, not the world" },
                { "VolumetricFogPass", "camera-froxel history, invalidated through TemporalHistoryRegistry" },
            };
            return list;
        }
    } // namespace

    TEST(RendererSceneStateContract, EverySettingsAccessorIsSceneStateOrListedConfiguration)
    {
        const std::string renderer = ReadText(RepoRoot() / "OloEngine/src/OloEngine/Renderer/Renderer3D.h");
        ASSERT_FALSE(renderer.empty());

        const std::regex accessor(R"(static\s+[\w:]+&\s+(Get\w+Settings)\(\)\s*\{\s*return\s+s_Data\.([\w.]+)\s*;)");
        sizet scanned = 0;
        for (std::sregex_iterator it(renderer.begin(), renderer.end(), accessor), end; it != end; ++it)
        {
            ++scanned;
            const std::string name = (*it)[1].str();
            const std::string member = (*it)[2].str();
            const bool sceneState = member.starts_with("Scene.");
            const bool configuration = ConfigurationAccessors().contains(name);
            EXPECT_NE(sceneState, configuration)
                << "Renderer3D::" << name << "() returns s_Data." << member << ". "
                << (sceneState ? "It is scene state AND listed as configuration: drop one."
                               : "A value a scene writes belongs in RendererSceneState, where ResetSceneState resets "
                                 "it by assignment; one only the user, the panels or tiering write is configuration "
                                 "and goes in ConfigurationAccessors() with the reason. See "
                                 "docs/agent-rules/cross-test-renderer-state-classification.md.");
        }
        // Twelve on the day this was written. Fewer means the pattern stopped
        // matching and the loop above checked nothing.
        EXPECT_GE(scanned, 12u) << "the accessor scan of Renderer3D.h found too few";
    }

    TEST(RendererSceneStateContract, AResetReturnsEveryScenePublicationToItsDefault)
    {
        // Every scene-published settings struct, moved off its default, then
        // the scene switch. Restored afterwards so this test leaks nothing.
        struct Saved
        {
            SnowSettings Snow = Renderer3D::GetSnowSettings();
            SkinDiffusionSettings SkinDiffusion = Renderer3D::GetSkinDiffusionSettings();
            FogSettings Fog = Renderer3D::GetFogSettings();
            WindSettings Wind = Renderer3D::GetWindSettings();
            SnowAccumulationSettings SnowAccumulation = Renderer3D::GetSnowAccumulationSettings();
            SnowEjectaSettings SnowEjecta = Renderer3D::GetSnowEjectaSettings();
            PrecipitationSettings Precipitation = Renderer3D::GetPrecipitationSettings();
            ~Saved()
            {
                Renderer3D::GetSnowSettings() = Snow;
                Renderer3D::GetSkinDiffusionSettings() = SkinDiffusion;
                Renderer3D::GetFogSettings() = Fog;
                Renderer3D::GetWindSettings() = Wind;
                Renderer3D::GetSnowAccumulationSettings() = SnowAccumulation;
                Renderer3D::GetSnowEjectaSettings() = SnowEjecta;
                Renderer3D::GetPrecipitationSettings() = Precipitation;
            }
        } saved;

        Renderer3D::GetSnowSettings().Enabled = !SnowSettings{}.Enabled;
        Renderer3D::GetSkinDiffusionSettings().Enabled = !SkinDiffusionSettings{}.Enabled;
        Renderer3D::GetFogSettings().Enabled = !FogSettings{}.Enabled;
        Renderer3D::GetFogSettings().Density = FogSettings{}.Density + 0.5f;
        Renderer3D::GetWindSettings().Enabled = !WindSettings{}.Enabled;
        Renderer3D::GetSnowAccumulationSettings().Enabled = !SnowAccumulationSettings{}.Enabled;
        Renderer3D::GetSnowEjectaSettings().Enabled = !SnowEjectaSettings{}.Enabled;
        Renderer3D::GetPrecipitationSettings().Enabled = !PrecipitationSettings{}.Enabled;

        Renderer3D::ResetSceneState();

        EXPECT_EQ(Renderer3D::GetSnowSettings().Enabled, SnowSettings{}.Enabled);
        EXPECT_EQ(Renderer3D::GetSkinDiffusionSettings().Enabled, SkinDiffusionSettings{}.Enabled);
        EXPECT_EQ(Renderer3D::GetFogSettings().Enabled, FogSettings{}.Enabled);
        EXPECT_NEAR(Renderer3D::GetFogSettings().Density, FogSettings{}.Density, 1e-6f)
            << "a scene that never publishes fog inherits the previous scene's";
        EXPECT_EQ(Renderer3D::GetWindSettings().Enabled, WindSettings{}.Enabled);
        EXPECT_EQ(Renderer3D::GetSnowAccumulationSettings().Enabled, SnowAccumulationSettings{}.Enabled);
        EXPECT_EQ(Renderer3D::GetSnowEjectaSettings().Enabled, SnowEjectaSettings{}.Enabled);
        EXPECT_EQ(Renderer3D::GetPrecipitationSettings().Enabled, PrecipitationSettings{}.Enabled);
    }

    TEST(RendererSceneStateContract, EveryPassWithAFrameSequenceDeclaresItsWorldCache)
    {
        const std::regex type(R"(\bclass\s+(?:OLO_API\s+)?(\w+)\s*(?:final\s*)?:\s*public\s+\w+)");
        const std::regex sequence(R"(\bvoid\s+ResetFrameSequence\s*\([^)]*\)\s*override)");
        const std::regex dropCache(R"(\bvoid\s+DropWorldCache\s*\(\s*\)\s*override)");

        std::set<std::string, std::less<>> withSequence;
        std::set<std::string, std::less<>> withDrop;
        for (const auto& entry : fs::recursive_directory_iterator(RepoRoot() / "OloEngine/src/OloEngine/Renderer"))
        {
            if (!entry.is_regular_file() || entry.path().extension() != ".h")
                continue;
            const std::string text = ReadText(entry.path());
            const auto ownerAt = [&text, &type](sizet position)
            {
                const std::string before = text.substr(0, position);
                std::string owner;
                for (std::sregex_iterator t(before.begin(), before.end(), type), tend; t != tend; ++t)
                    owner = (*t)[1].str();
                return owner;
            };
            for (std::sregex_iterator it(text.begin(), text.end(), sequence), end; it != end; ++it)
                withSequence.insert(ownerAt(static_cast<sizet>(it->position())));
            for (std::sregex_iterator it(text.begin(), text.end(), dropCache), end; it != end; ++it)
                withDrop.insert(ownerAt(static_cast<sizet>(it->position())));
        }
        // Seven on the day this was written; fewer means the scan broke.
        ASSERT_GE(withSequence.size(), 7u) << "the ResetFrameSequence scan found too few passes";
        EXPECT_TRUE(withDrop.contains("DDGIProbeUpdatePass")) << "the scan no longer sees the DDGI probe volume's drop";

        for (const std::string& pass : withSequence)
        {
            const bool drops = withDrop.contains(pass);
            const bool listed = PassesWithoutWorldCache().contains(pass);
            EXPECT_NE(drops, listed)
                << pass << " keeps state from one frame to the next (it overrides ResetFrameSequence). "
                << (drops ? "It drops a world cache AND is listed as holding none: drop one."
                          : "If any of that state is built from the scene's content (a probe field, a page cache, an "
                            "accumulation), override DropWorldCache so Renderer3D::ResetSceneState drops it; "
                            "otherwise list it in PassesWithoutWorldCache() with what it keeps instead.");
        }
    }
} // namespace OloEngine::Tests

// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// GBufferBakedGIContractTest.cpp
//
// The G-Buffer's sixth render target (issue #865) carries baked lightmap
// irradiance E in .rgb and COVERAGE in .a, so that the deferred lighting pass —
// which has neither UV2 nor instance identity by the time it runs — can enter
// the ambient ladder at the same rung the forward path does.
//
// That target has three contracts a compiler cannot check, and each of them
// fails SILENTLY and PLAUSIBLY:
//
//   1. EVERY G-Buffer writer must write RT5. An MRT output a fragment shader
//      never assigns is UNDEFINED in that attachment — not zero. RT5's .a is a
//      coverage flag, so undefined there reads as "this pixel has baked GI" and
//      the lighting pass shades it from whatever the target happened to hold.
//      A new G-Buffer shader that forgets the line renders correctly on a scene
//      with no bake and wrong on one with a bake, which is the worst possible
//      distribution of when the bug appears.
//
//   2. The sampler slot is a mirror. `DeferredLighting.glsl` spells the binding
//      as an integer literal; `ShaderBindingLayout::TEX_GBUFFER_BAKEDGI` is what
//      DeferredLightingPass binds. Two numbers, no shared symbol — the #691/#702
//      shape of bug, where the shader reads whatever texture is at the stale slot.
//
//   3. The two paths' top ambient rung must stay the same rung.
//      `include/AmbientLadder.glsl` (forward) and
//      `include/DeferredLightingShared.glsl` (deferred) each define it once, and
//      a photometric divergence between them is invisible in either path alone —
//      you only see it by rendering the same scene twice, which is what
//      LightmapVisualEvidenceTest.BakedBleedSurvivesTheDeferredPath costs a GPU
//      to do. These text checks are the cheap headless guard in front of it.
//
// Text scans, deliberately: the thing under test is what the shader SOURCE says,
// and that is checkable with no GL context, so it runs everywhere.
//
// Classification: shaderpipe (shader source / binding contract, headless).
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        [[nodiscard]] fs::path ShaderRoot()
        {
            return fs::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders";
        }

        [[nodiscard]] std::string ReadWholeFile(const fs::path& path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                return {};
            }
            std::ostringstream buffer;
            buffer << file.rdbuf();
            return buffer.str();
        }

        // Every .glsl under assets/shaders (including include/), recursively.
        [[nodiscard]] std::vector<fs::path> AllShaderFiles()
        {
            std::vector<fs::path> files;
            std::error_code ec;
            for (const auto& entry : fs::recursive_directory_iterator(ShaderRoot(), ec))
            {
                if (entry.is_regular_file(ec) && entry.path().extension() == ".glsl")
                {
                    files.push_back(entry.path());
                }
            }
            return files;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // Contract 1 — every G-Buffer writer writes RT5.
    //
    // "G-Buffer writer" is identified by the entity-ID output at location 4:
    // that attachment exists only in the deferred G-Buffer layout, so declaring
    // it is exactly the statement "this fragment shader targets the G-Buffer".
    // A shader that writes a SUBSET of the attachments through an explicit
    // draw-buffer mask (the Decal_GBuffer_* family) does not declare it and is
    // correctly out of scope — its masked-off slots are GL_NONE, not undefined.
    // -------------------------------------------------------------------------
    TEST(GBufferBakedGIContract, EveryGBufferWriterDeclaresAndWritesTheBakedGITarget)
    {
        const std::regex declRe(R"(layout\(location\s*=\s*5\)\s*out\s+vec4\s+o_GBufferBakedGI\s*;)");
        const std::regex writeRe(R"(o_GBufferBakedGI\s*=)");

        u32 writersChecked = 0;
        for (const auto& path : AllShaderFiles())
        {
            const std::string src = ReadWholeFile(path);
            if (src.find("o_GBufferEntityID") == std::string::npos)
            {
                continue;
            }
            ++writersChecked;
            const std::string name = path.filename().string();

            EXPECT_TRUE(std::regex_search(src, declRe))
                << name << " writes the G-Buffer (it declares o_GBufferEntityID) but does not declare "
                << "`layout(location = 5) out vec4 o_GBufferBakedGI;`. An MRT output the shader never "
                << "declares leaves attachment 5 UNDEFINED for its pixels, and that attachment's alpha "
                << "is a coverage flag — the deferred ambient ladder will read undefined memory as "
                << "\"this pixel has baked GI\" (issue #865).";

            EXPECT_TRUE(std::regex_search(src, writeRe))
                << name << " declares o_GBufferBakedGI but never assigns it. Declaring an output does "
                << "not initialise it; write vec4(0.0) if this surface receives no baked lightmap.";
        }

        // Both directions: if the marker stops finding the writers, the test above
        // silently passes over an empty set. Ten is the count at the time of
        // writing (PBR + skinned, foliage, grid, light cube, skybox, three terrain
        // variants, the virtual-geometry fragment include and its resolve pass);
        // the bound is a floor, not a pin, so adding a writer does not fail here —
        // it fails on the two EXPECTs above if it forgets the target.
        EXPECT_GE(writersChecked, 10u)
            << "found only " << writersChecked << " G-Buffer writers — the o_GBufferEntityID marker "
            << "no longer identifies them, so this test is checking nothing";
    }

    // -------------------------------------------------------------------------
    // Contract 2 — the sampler slot mirrors the binding layout.
    // -------------------------------------------------------------------------
    TEST(GBufferBakedGIContract, TheBakedGISamplerSlotMatchesTheBindingLayout)
    {
        const std::regex bindingRe(R"(layout\(binding\s*=\s*(\d+)\)\s*uniform\s+sampler2D(?:MS)?\s+u_GBufferBakedGI\s*;)");

        struct Consumer
        {
            const char* File;
            const char* Why;
        };
        const Consumer kConsumers[] = {
            { "DeferredLighting.glsl", "the single-sample deferred lighting pass" },
            { "DeferredLighting_MSAA.glsl", "the per-sample MSAA deferred lighting pass" },
        };

        for (const auto& consumer : kConsumers)
        {
            const std::string src = ReadWholeFile(ShaderRoot() / consumer.File);
            ASSERT_FALSE(src.empty()) << "could not read " << consumer.File;

            std::smatch match;
            ASSERT_TRUE(std::regex_search(src, match, bindingRe))
                << consumer.File << " (" << consumer.Why
                << ") declares no u_GBufferBakedGI sampler — the deferred path cannot read the baked "
                << "lightmap irradiance the G-Buffer pass wrote (issue #865)";

            EXPECT_EQ(static_cast<u32>(std::stoul(match[1].str())), ShaderBindingLayout::TEX_GBUFFER_BAKEDGI)
                << consumer.File << " binds u_GBufferBakedGI to slot " << match[1].str()
                << " but DeferredLightingPass binds the attachment at "
                << ShaderBindingLayout::TEX_GBUFFER_BAKEDGI
                << ". The two numbers are separate literals with no shared symbol; when they drift the "
                << "shader samples whatever texture occupies the stale slot, which is a plausible "
                << "picture rather than a black one.";
        }
    }

    // -------------------------------------------------------------------------
    // Contract 3a — the G-Buffer pass is where the atlas fetch happens.
    //
    // This is the whole design decision of #865 in one assertion: the deferred
    // path carries IRRADIANCE, not the atlas UV, because it is the geometry pass
    // that still has UV2 and the per-draw region — and because an MSAA resolve
    // averaging two charts' UVs across a silhouette reads an unrelated texel,
    // while averaging irradiance and coverage is the alpha-weighted blend the
    // sampler already documents.
    // -------------------------------------------------------------------------
    TEST(GBufferBakedGIContract, PBRGBufferSamplesTheAtlasAndWritesIrradiance)
    {
        const std::string src = ReadWholeFile(ShaderRoot() / "PBR_GBuffer.glsl");
        ASSERT_FALSE(src.empty());

        EXPECT_NE(src.find("include/LightmapSampling.glsl"), std::string::npos)
            << "PBR_GBuffer.glsl does not include the lightmap sampler — the deferred path has no other "
               "stage that still holds UV2 and the per-draw atlas region";

        EXPECT_TRUE(std::regex_search(src, std::regex(R"(layout\(location\s*=\s*3\)\s*in\s+vec2\s+a_TexCoord2\s*;)")))
            << "PBR_GBuffer.glsl does not read the UV2 attribute at location 3. MeshSource::Build pins "
               "the lightmap stream (and its stride-0 constant stub for unbaked static meshes) at that "
               "location precisely so every static VAO exposes one identical layout.";

        EXPECT_TRUE(std::regex_search(src, std::regex(R"(o_GBufferBakedGI\s*=\s*sampleLightmapIrradiance\()")))
            << "PBR_GBuffer.glsl does not write the atlas sample into RT5. Storing anything else there — "
               "the atlas UV, or a pre-shaded ambient term — breaks either the MSAA resolve (averaged "
               "UVs address a foreign chart) or the decal path (albedo is still being modified after "
               "this pass runs).";

        // The Vulkan vertex-pull route must gate the UV2 read on the same
        // per-instance signal the fragment sampler uses. An unconditional pull on
        // an unbaked static mesh resolves to the frame arena's fixed-size null
        // block and runs off the end of it — a buffer-device-address read has no
        // bounds, so that is a device loss (ADR 0011 amendment (89)), and it is
        // the common case rather than an edge one.
        EXPECT_TRUE(std::regex_search(
            src, std::regex(R"(LightmapScaleOffset\.x\s*>\s*0\.0[\s\S]{0,200}b_LightmapUV\.v\[)")))
            << "PBR_GBuffer.glsl's Vulkan pull of the UV2 stream is not guarded by "
               "`instances[...].LightmapScaleOffset.x > 0.0` — an unbaked static mesh has no stream-1 "
               "buffer and the unguarded read is a device loss, not a clamped read";
    }

    // -------------------------------------------------------------------------
    // Contract 2c — the VIRTUAL raster paths sample the atlas too, on BOTH of
    // their rasterizers, and both guard the arena read.
    //
    // Virtual geometry has three ways onto the screen — hardware MDI, the
    // VK_EXT_mesh_shader path, and the compute software rasterizer resolved
    // through VirtualVisibilityResolve.glsl. All three write RT5, so all three
    // have to sample, or a cluster changes colour when it crosses the
    // software-raster size threshold. That is a per-cluster discontinuity in a
    // frame, which is far harder to spot than a whole surface going unlit.
    //
    // These are text scans for the same reason LightmapPageEncodingTest's is:
    // the only test that renders a paged virtual atlas is behind
    // OLO_ENSURE_GPU_OR_SKIP(), so on a machine with no GPU nothing else would
    // notice these reverting.
    // -------------------------------------------------------------------------
    TEST(GBufferBakedGIContract, VirtualRasterPathsSampleTheAtlas)
    {
        struct VirtualConsumer
        {
            const char* File;
            const char* What;
        };
        // The hardware raster's fragment stage is shared verbatim by the MDI and
        // mesh-shader pipelines, so covering the include covers both.
        constexpr VirtualConsumer kConsumers[] = {
            { "include/VirtualGBufferFragment.glsl", "the hardware raster (MDI + mesh shader)" },
            { "VirtualVisibilityResolve.glsl", "the software raster's material resolve" },
        };

        for (const VirtualConsumer& consumer : kConsumers)
        {
            const std::string src = ReadWholeFile(ShaderRoot() / consumer.File);
            ASSERT_FALSE(src.empty()) << consumer.File;

            EXPECT_NE(src.find("LightmapSampling.glsl"), std::string::npos)
                << consumer.File << " (" << consumer.What
                << ") does not include the lightmap sampler, so it cannot decode a region";

            EXPECT_TRUE(std::regex_search(src, std::regex(R"(o_GBufferBakedGI\s*=\s*sampleLightmapIrradiance\()"))) << consumer.File << " (" << consumer.What
                                                                                                                    << ") writes something other than the atlas sample into RT5. If it writes vec4(0) again, "
                                                                                                                       "virtual geometry silently stops receiving baked GI on that rasterizer while the other "
                                                                                                                       "one keeps it — the two disagree per cluster, not per object.";
        }
    }

    // The arena read itself must be guarded, twice over. This is the virtual
    // path's version of the amendment-(89) device-loss guard: the uv2 rides the
    // vertex arena's packed tail, so an index computed with no tail present (or
    // for an instance with no region) lands past the buffer — and a
    // buffer-device-address read has no bounds.
    TEST(GBufferBakedGIContract, VirtualLightmapUVFetchIsGuarded)
    {
        const std::string stage = ReadWholeFile(ShaderRoot() / "include/VirtualGBufferVertexStage.glsl");
        ASSERT_FALSE(stage.empty());
        EXPECT_TRUE(std::regex_search(
            stage, std::regex(R"(u_VirtualLightmapUVBase\s*==\s*0u[\s\S]{0,120}LightmapScaleOffset\.x\s*<=\s*0\.0)")))
            << "FetchVirtualLightmapUV does not check BOTH that the arena carries a uv2 tail and that "
               "this instance has a region. Either alone is insufficient: no tail means the element "
               "index is past the buffer, and no region means the mesh's cook may predate its unwrap so "
               "the tail holds another mesh's charts.";

        const std::string resolve = ReadWholeFile(ShaderRoot() / "VirtualVisibilityResolve.glsl");
        ASSERT_FALSE(resolve.empty());
        EXPECT_TRUE(std::regex_search(
            resolve, std::regex(R"(u_VirtualLightmapUVBase\s*!=\s*0u[\s\S]{0,120}LightmapScaleOffset\.x\s*>\s*0\.0)")))
            << "the software-raster resolve fetches the uv2 tail without the same two guards the "
               "hardware stage uses";
    }

    // -------------------------------------------------------------------------
    // Contract 3b — forward and deferred enter the ladder at the same rung, with
    // the same gate and the same helpers.
    //
    // The gate is the assertion that matters. Branching on the sampled COLOUR
    // instead of the coverage alpha collapses "validly baked pure black" (an
    // enclosed surface no indirect light reaches — must keep its darkness) into
    // "never baked" (must fall through), and the enclosed room glows with sky
    // IBL. That is the exact leak the bake exists to kill, and it was the first
    // draft's bug on the forward path.
    // -------------------------------------------------------------------------
    TEST(GBufferBakedGIContract, BothPathsGateTheLightmapRungOnCoverageAndUseTheSameHelpers)
    {
        const std::string forward = ReadWholeFile(ShaderRoot() / "include" / "AmbientLadder.glsl");
        const std::string deferred = ReadWholeFile(ShaderRoot() / "include" / "DeferredLightingShared.glsl");
        ASSERT_FALSE(forward.empty());
        ASSERT_FALSE(deferred.empty());

        EXPECT_TRUE(std::regex_search(forward, std::regex(R"(lightmapSample\.a\s*>\s*0\.5)")))
            << "the forward ambient ladder no longer gates its lightmap rung on the sample's COVERAGE "
               "alpha";
        EXPECT_TRUE(std::regex_search(deferred, std::regex(R"(bakedGI\.a\s*>\s*0\.5)")))
            << "the deferred ambient ladder does not gate its lightmap rung on RT5's COVERAGE alpha. A "
               "colour-based gate (dot(rgb, rgb) > 0) makes a validly baked black texel indistinguishable "
               "from an unbaked one, and the enclosed room glows with sky IBL.";

        // Both rungs replace the DIFFUSE ambient with the baked irradiance and
        // keep IBL specular — same two helpers, chosen the same way.
        for (const auto* helper : { "calculateCombinedAmbientPrefiltered", "calculateLightProbeAmbient" })
        {
            EXPECT_NE(forward.find(helper), std::string::npos)
                << "AmbientLadder.glsl no longer calls " << helper;
            EXPECT_NE(deferred.find(helper), std::string::npos)
                << "DeferredLightingShared.glsl no longer calls " << helper
                << " — the two paths' ambient rungs have drifted apart, which is invisible in either "
                   "path alone";
        }
    }
} // namespace OloEngine::Tests

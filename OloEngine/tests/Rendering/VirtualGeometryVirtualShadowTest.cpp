// OLO_TEST_LAYER: L1
//
// Virtual geometry through the Virtual Shadow Map — contract tests (issue #1149).
//
// The route this pins is the one the issue describes as failing quietly: "a
// shadow regression renders a plausible frame with subtly wrong contact
// shadows." Every check here is for a change that would leave the frame looking
// reasonable and the shadows wrong or missing, and none of them needs a GPU:
//
//   * the dirty-page gate DEFAULTS OFF. It rides the cluster-cull block that
//     every virtual-geometry view fills, including the main camera's and the
//     classic CSM cascades'. A gate whose "off" value were 0 would read as
//     "clip level 0" on every one of those and reject their geometry against a
//     page pyramid that has nothing to do with them — no error, no warning, no
//     virtual geometry.
//   * the shader block and the C++ mirror agree on where that gate lives. A
//     drift writes the gate into CullProjParams' bytes instead, which is a
//     silently wrong near plane on the main view.
//   * BOTH culls ask the footprint question through the SAME code. They were one
//     copy each before this issue; two copies of a wrapped-page rectangle drift,
//     and the drift shows up as one caster family drawing into pages the other
//     already considers finished.
//   * the raster resolves through the page table, with the RASTERIZER flavour of
//     the projection. The math flavour there renders the whole shadow map upside
//     down on Vulkan and is identical on GL, so a GL-only check would pass.
//   * the clip levels a caster reaches are found, and a caster is never dropped
//     from a level it does touch — that one IS the missing shadow.
//
// Pixels are covered separately:
// VirtualGeometryVisualEvidence.VirtualMeshCastsThroughTheVirtualShadowMapPages.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/Shadow/VirtualShadowMap.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

using namespace OloEngine;

namespace
{
    // Same three-candidate walk the other shader-contract tests use: the suite
    // runs from the repo root, but a packaged run may sit beside the assets.
    [[nodiscard]] std::string ReadShader(const char* relative)
    {
        namespace fs = std::filesystem;
        const std::array<fs::path, 3> candidates{
            fs::path("OloEditor/assets/shaders") / relative,
            fs::path("assets/shaders") / relative,
            fs::path("../OloEditor/assets/shaders") / relative,
        };
        for (const auto& path : candidates)
        {
            std::ifstream in(path);
            if (!in)
                continue;
            std::ostringstream buffer;
            buffer << in.rdbuf();
            return buffer.str();
        }
        return {};
    }

    // The settings a virtual-geometry scene would actually run: the shipped
    // defaults, so a change to them fails these tests rather than sliding past.
    [[nodiscard]] VirtualShadowMapSettings DefaultSettings()
    {
        VirtualShadowMapSettings settings;
        settings.Enabled = true;
        return settings;
    }

    // This frame's clip projections for a sun at 45 degrees and a camera at the
    // render origin — BuildClipProjections is the same function the live path
    // calls, so the levels under test are the levels that ship.
    [[nodiscard]] std::array<VSM::ClipProjection, VSM::kClipLevels> BuildClips()
    {
        std::array<VSM::ClipProjection, VSM::kClipLevels> clips{};
        std::array<glm::ivec2, VSM::kClipLevels> origins{};
        const std::array<glm::ivec2, VSM::kClipLevels> prevOrigins{};
        VirtualShadowMap::BuildClipProjections(glm::normalize(glm::vec3(0.4f, -1.0f, 0.3f)),
                                               glm::vec3(0.0f), DefaultSettings(), prevOrigins,
                                               /*fullInvalidate=*/true, clips, origins);
        return clips;
    }
} // namespace

// ---------------------------------------------------------------------------
// The gate
// ---------------------------------------------------------------------------

TEST(VirtualGeometryVirtualShadow, TheDirtyPageGateIsOffInAZeroInitialisedCullBlock)
{
    // THE guard for this whole feature. Every other filler of this block — the
    // main camera's virtual-geometry pass, the CSM cascades, the atlas faces,
    // the parity test — value-initialises it and sets only the fields it cares
    // about, which is the documented contract of the block. So "off" cannot be
    // the zero value.
    const UBOStructures::VirtualClusterCullUBO zeroInitialised{};
    EXPECT_LT(zeroInitialised.VsmPageGate.x, 0)
        << "a zero-initialised cull block must read as GATE OFF; at 0 it names clip level 0 and "
           "every non-VSM view silently rejects its geometry against the VSM page pyramid";

    // And the shader must agree on which sign means off, not just the C++ side.
    const std::string cull = ReadShader("compute/VirtualClusterCull.comp");
    ASSERT_FALSE(cull.empty()) << "VirtualClusterCull.comp not found";
    EXPECT_NE(cull.find("u_VSMPageGate.x >= 0"), std::string::npos)
        << "the shader's gate predicate must admit only a non-negative clip level";
}

TEST(VirtualGeometryVirtualShadow, TheGateSitsWhereTheShaderBlockPutsIt)
{
    // std140 offsets, checked against the block's own comment column — the same
    // technique the rest of the UBO mirrors use, because restating the number
    // here would just be a third copy.
    EXPECT_EQ(offsetof(UBOStructures::VirtualClusterCullUBO, VsmPageGate), 240u);
    EXPECT_EQ(sizeof(UBOStructures::VirtualClusterCullUBO), 256u);

    const std::string cull = ReadShader("compute/VirtualClusterCull.comp");
    ASSERT_FALSE(cull.empty());
    const std::regex declaration(R"(ivec4\s+u_VSMPageGate;\s*//\s*(\d+))");
    std::smatch match;
    ASSERT_TRUE(std::regex_search(cull, match, declaration))
        << "VirtualClusterCullParams must declare `ivec4 u_VSMPageGate; // <offset>`";
    EXPECT_EQ(std::stoul(match[1].str()),
              static_cast<unsigned long>(offsetof(UBOStructures::VirtualClusterCullUBO, VsmPageGate)));
}

TEST(VirtualGeometryVirtualShadow, TheClusterCullReadsThePyramidAtItsEngineWideSlot)
{
    const std::string cull = ReadShader("compute/VirtualClusterCull.comp");
    const std::string buffers = ReadShader("include/VirtualShadowBuffers.glsl");
    ASSERT_FALSE(cull.empty());
    ASSERT_FALSE(buffers.empty());

    // Same binding number as the VSM kernels, and the same block AND member
    // name — the shared footprint include addresses `b_HPB`, so a rename on
    // either side stops compiling rather than reading the wrong buffer.
    const std::regex hpbBinding(R"(binding\s*=\s*(\d+)\s*\)\s*readonly\s+buffer\s+VSMHierarchicalPageBuffer\s*\{\s*uint\s+b_HPB)");
    std::smatch match;
    ASSERT_TRUE(std::regex_search(cull, match, hpbBinding))
        << "the virtual-geometry cull must declare VSMHierarchicalPageBuffer { uint b_HPB[]; }";
    EXPECT_EQ(std::stoul(match[1].str()), ShaderBindingLayout::SSBO_VSM_HPB);
    EXPECT_NE(buffers.find("VSMHierarchicalPageBuffer { uint b_HPB[]; }"), std::string::npos)
        << "VirtualShadowBuffers.glsl must still spell the pyramid the same way";
}

TEST(VirtualGeometryVirtualShadow, BothCullsAskTheFootprintQuestionThroughOneCopy)
{
    const std::string virtualCull = ReadShader("compute/VirtualClusterCull.comp");
    const std::string meshCull = ReadShader("compute/VSM_CullCasters.comp");
    const std::string footprint = ReadShader("include/VirtualShadowPageFootprint.glsl");
    ASSERT_FALSE(virtualCull.empty());
    ASSERT_FALSE(meshCull.empty());
    ASSERT_FALSE(footprint.empty());

    for (const std::string* source : { &virtualCull, &meshCull })
    {
        EXPECT_NE(source->find("VirtualShadowPageFootprint.glsl"), std::string::npos)
            << "both culls must include the shared footprint helper";
        // A second definition would silently shadow the include's — the exact
        // drift this file exists to prevent.
        EXPECT_EQ(source->find("bool vsmFootprintHasDirtyPage("), std::string::npos)
            << "the footprint test must not be redefined locally";
    }
    EXPECT_NE(footprint.find("bool vsmFootprintHasDirtyPage("), std::string::npos);
    EXPECT_NE(footprint.find("void vsmSphereToPageRect("), std::string::npos);
}

// ---------------------------------------------------------------------------
// The raster
// ---------------------------------------------------------------------------

TEST(VirtualGeometryVirtualShadow, TheRasterResolvesThroughThePageTableWithTheRasterizerMatrix)
{
    const std::string raster = ReadShader("VSM_VirtualMeshDepth.glsl");
    ASSERT_FALSE(raster.empty()) << "VSM_VirtualMeshDepth.glsl not found";

    // Visibility must come from the page indirection, not a depth attachment:
    // the VSM raster scope has no depth buffer at all, so a stage that wrote
    // gl_FragDepth would render a completely unshadowed frame.
    EXPECT_NE(raster.find("include/VirtualShadowRasterStage.glsl"), std::string::npos);

    // gl_Position takes the RASTERIZER flavour. On GL the two matrices are
    // identical, so getting this wrong is invisible until the frame is rendered
    // on Vulkan — where it flips the whole shadow map.
    const std::regex position(R"(gl_Position\s*=\s*u_VSMClips\[\w+\]\.ViewProjectionRaster)");
    EXPECT_TRUE(std::regex_search(raster, position))
        << "gl_Position must use ViewProjectionRaster, never the math-flavour matrix";

    // And the level travels per draw, in the pass block, because the cluster
    // command stream carries clusters rather than (caster, level) records.
    EXPECT_NE(raster.find("u_VSMPassParams.x"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Which levels a caster reaches
// ---------------------------------------------------------------------------

TEST(VirtualGeometryVirtualShadow, ACasterAtTheCameraReachesTheFinestClipLevel)
{
    const auto clips = BuildClips();
    // Well inside clip 0's 2 m half extent.
    EXPECT_TRUE(VirtualShadowMap::BoundsReachClipLevel(clips[0].ViewProjection,
                                                       glm::vec3(-0.25f), glm::vec3(0.25f)));
}

TEST(VirtualGeometryVirtualShadow, ACasterBeyondALevelIsDroppedThereAndKeptOnACoarserOne)
{
    const auto clips = BuildClips();
    const auto settings = DefaultSettings();

    // 200 m out: far outside clip 0 (2 m half extent), comfortably inside the
    // level whose half extent passes it.
    const glm::vec3 farMin(190.0f, -1.0f, 190.0f);
    const glm::vec3 farMax(210.0f, 1.0f, 210.0f);

    EXPECT_FALSE(VirtualShadowMap::BoundsReachClipLevel(clips[0].ViewProjection, farMin, farMax))
        << "a caster 200 m away must not cost clip level 0 a cluster-cull dispatch";

    // Some level must still claim it, or the caster casts no shadow at all —
    // which is the silent hole this whole route has to avoid.
    bool reachedSomewhere = false;
    for (u32 level = 0; level < VSM::kClipLevels; ++level)
        reachedSomewhere = reachedSomewhere || VirtualShadowMap::BoundsReachClipLevel(clips[level].ViewProjection, farMin, farMax);
    EXPECT_TRUE(reachedSomewhere)
        << "a caster inside the configured shadow range must reach at least one clip level; "
           "clip 0 half extent = "
        << settings.Clip0HalfExtent;
}

TEST(VirtualGeometryVirtualShadow, TheLevelThatOwnsACasterIsNeverColderThanTheOneBelowIt)
{
    // Coverage grows by 2x per level, so once a level claims a caster every
    // coarser level claims it too. The property matters because the route drops
    // levels it believes no caster reaches: a hole in the middle of the reached
    // set would be a level that renders nothing while its neighbours do, which
    // on screen is a shadow that vanishes at one distance band and returns at
    // the next.
    const auto clips = BuildClips();
    for (f32 distance : { 1.0f, 9.0f, 60.0f, 400.0f, 3000.0f })
    {
        const glm::vec3 boundsMin(distance - 0.5f, -0.5f, -0.5f);
        const glm::vec3 boundsMax(distance + 0.5f, 0.5f, 0.5f);

        bool seenReached = false;
        for (u32 level = 0; level < VSM::kClipLevels; ++level)
        {
            const bool reached = VirtualShadowMap::BoundsReachClipLevel(clips[level].ViewProjection,
                                                                        boundsMin, boundsMax);
            if (reached)
            {
                seenReached = true;
                continue;
            }
            EXPECT_FALSE(seenReached)
                << "clip level " << level << " dropped a caster at " << distance
                << " m that a FINER level kept — the reached set must not have holes";
        }
        EXPECT_TRUE(seenReached) << "no clip level reaches a caster at " << distance << " m";
    }
}

TEST(VirtualGeometryVirtualShadow, ACasterStraddlingALevelBoundaryIsKept)
{
    const auto clips = BuildClips();
    const auto settings = DefaultSettings();
    // A box whose near end sits inside clip 0 and whose far end runs well past
    // it. Rejecting this is the classic conservative-bounds bug: the part inside
    // the level still needs its pages drawn.
    const glm::vec3 boundsMin(0.0f, -0.1f, -0.1f);
    const glm::vec3 boundsMax(settings.Clip0HalfExtent * 40.0f, 0.1f, 0.1f);
    EXPECT_TRUE(VirtualShadowMap::BoundsReachClipLevel(clips[0].ViewProjection, boundsMin, boundsMax));
}

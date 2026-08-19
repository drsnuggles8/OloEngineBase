// OLO_TEST_LAYER: L1
//
// Virtual Shadow Map LOCAL-LIGHT contract tests (issue #703).
//
// Same charter as VirtualShadowMapTest.cpp, one domain over: everything here is
// CPU-only and runs in headless CI, because every invariant it pins fails
// SILENTLY on screen.
//
//   * a C++/GLSL constant drift reinterprets every local page entry;
//   * a meta-encoding overlap makes an eviction release somebody else's page;
//   * a marker/sampler disagreement about the MIP lands the sample on a page
//     nobody backed, and an unbacked page reads as fully LIT — so the symptom is
//     "this lamp casts no shadow", which looks like a light setting;
//   * a layer pool that reassigns layers every frame is CORRECT and throws away
//     the entire cache, which looks like the raster being slow.
//
// The pixels are covered separately — see VirtualShadowMapVisualEvidenceTest and
// the multi-angle capture in the PR description.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// ShadowMap.h, not just VirtualShadowMap.h: two of the tests below compare the
// VSM's per-face projections against the ATLAS builders' — that parity is the
// point (light-path-photometric-parity.md), so the two must be visible together.
// It also brings in ShadowAtlas.h for the 16-light budget this beats.
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Renderer/Shadow/VirtualShadowMap.h"
// Also for MIN_GUARANTEED_BUFFER_BINDINGS — the 84 this file used to spell as a
// literal. It reached into Platform/Vulkan/VulkanBindingState.h for that constant
// until #811, which does not compile under OLO_WITH_VULKAN=OFF (the whole header is
// behind the guard) — and nothing built that configuration, so it went unnoticed.
// The bound is a GL binding-number property, so it now lives in the neutral header.
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <regex>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>

using namespace OloEngine;

namespace
{
    [[nodiscard]] std::string ReadShaderText(const char* relative)
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

    // Extracts `#define <name> <integer literal>`. Returns false for a define
    // spelled as an expression — those are checked by their evaluated result.
    [[nodiscard]] bool ParseIntDefine(const std::string& source, const std::string& name, i64& out)
    {
        const std::regex pattern("#define\\s+" + name + "\\s+(-?[0-9]+)\\s");
        std::smatch match;
        if (!std::regex_search(source, match, pattern))
            return false;
        out = std::stoll(match[1].str());
        return true;
    }

    [[nodiscard]] u32 CountOccurrences(const std::string& haystack, const std::string& needle)
    {
        u32 count = 0;
        for (sizet pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + 1))
            ++count;
        return count;
    }

    [[nodiscard]] VirtualShadowMap::LocalLightDesc MakePointLight(u64 id, const glm::vec3& position, f32 range)
    {
        VirtualShadowMap::LocalLightDesc desc;
        desc.LightId = id;
        desc.PositionRelative = position;
        desc.Range = range;
        desc.IsPoint = true;
        return desc;
    }

    [[nodiscard]] VirtualShadowMap::LocalLightDesc MakeSpotLight(u64 id, const glm::vec3& position,
                                                                 const glm::vec3& direction, f32 range,
                                                                 f32 cutoffDegrees)
    {
        VirtualShadowMap::LocalLightDesc desc;
        desc.LightId = id;
        desc.PositionRelative = position;
        desc.Direction = direction;
        desc.Range = range;
        desc.OuterCutoffDegrees = cutoffDegrees;
        desc.IsPoint = false;
        return desc;
    }
} // namespace

// =============================================================================
// 1. The C++/GLSL constant mirror
// =============================================================================

TEST(VirtualShadowMapLocal, ConstantsMirrorTheShaderContract)
{
    const std::string common = ReadShaderText("include/VirtualShadowCommon.glsl");
    ASSERT_FALSE(common.empty())
        << "VirtualShadowCommon.glsl not found — the mirror test cannot run and would pass vacuously";

    struct Mirror
    {
        const char* Define;
        u32 Cpp;
    };
    const std::array<Mirror, 5> mirrors{ {
        { "VSM_LOCAL_VIRTUAL_RESOLUTION", VSM::kLocalVirtualResolution },
        { "VSM_LOCAL_PAGE_TABLE_RES_LOG2", 5u },
        { "VSM_LOCAL_MIP_COUNT", VSM::kLocalMipCount },
        { "VSM_LOCAL_PAGES_PER_LAYER", VSM::kLocalPagesPerLayer },
        { "VSM_MAX_LOCAL_LAYERS", VSM::kMaxLocalLayers },
    } };

    for (const auto& mirror : mirrors)
    {
        i64 shaderValue = 0;
        ASSERT_TRUE(ParseIntDefine(common, mirror.Define, shaderValue))
            << mirror.Define << " is missing from VirtualShadowCommon.glsl";
        EXPECT_EQ(static_cast<i64>(mirror.Cpp), shaderValue)
            << mirror.Define << " drifted between C++ and GLSL — every local page entry is now reinterpreted";
    }

    // The derived constants, by value rather than by re-parsing the expressions.
    EXPECT_EQ(VSM::kLocalPageTableResolution, VSM::kLocalVirtualResolution / VSM::kPageSize);
    EXPECT_EQ(VSM::kLocalPageTableResolution, 1u << 5);
    EXPECT_EQ(VSM::kTotalLocalPages, VSM::kLocalPagesPerLayer * VSM::kMaxLocalLayers);
    EXPECT_EQ(VSM::kTotalPageTableEntries, VSM::kTotalVirtualPages + VSM::kTotalLocalPages);
    EXPECT_EQ(VSM::kTotalHPBEntries, VSM::kHPBTotalEntries + VSM::kLocalHPBTotalEntries);

    // The pyramid must bottom out at exactly one texel, or the cull's coarsest
    // lookup reads past the end of a layer's region and into the next layer's.
    EXPECT_EQ(1u, VSM::kLocalPageTableResolution >> (VSM::kLocalMipCount - 1));

    u32 expectedPages = 0;
    for (u32 mip = 0; mip < VSM::kLocalMipCount; ++mip)
    {
        const u32 res = VSM::kLocalPageTableResolution >> mip;
        expectedPages += res * res;
    }
    EXPECT_EQ(VSM::kLocalPagesPerLayer, expectedPages);

    // The one new SSBO binding. Pinned against the reserved Vulkan vertex-pull
    // streams and the GL 4.6 minimum, the same two checks #702's bindings got.
    EXPECT_NE(ShaderBindingLayout::SSBO_VSM_LOCAL_LIGHTS, ShaderBindingLayout::SSBO_VERTEX_PULL);
    EXPECT_NE(ShaderBindingLayout::SSBO_VSM_LOCAL_LIGHTS, ShaderBindingLayout::SSBO_BONE_PULL);
    EXPECT_LT(ShaderBindingLayout::SSBO_VSM_LOCAL_LIGHTS, ShaderBindingLayout::MIN_GUARANTEED_BUFFER_BINDINGS)
        << "past the GL_MAX_UNIFORM_BUFFER_BINDINGS minimum guarantee (84), which is the bound the "
           "Vulkan backend sizes its binding tables to — not a shader-storage limit, which is where "
           "the previous message pointed";

    const std::string resources = ReadShaderText("include/VirtualShadowResources.glsl");
    ASSERT_FALSE(resources.empty());
    std::smatch match;
    ASSERT_TRUE(std::regex_search(
        resources, match,
        std::regex(R"(layout\(std430,\s*binding\s*=\s*(\d+)\)\s*VSM_LOCAL_LIGHTS_QUALIFIER\s+buffer\s+VSMLocalLights)")))
        << "the VSMLocalLights block declaration was not found in VirtualShadowResources.glsl";
    EXPECT_EQ(static_cast<u32>(std::stoul(match[1].str())), ShaderBindingLayout::SSBO_VSM_LOCAL_LIGHTS)
        << "VSMLocalLights' binding literal drifted from SSBO_VSM_LOCAL_LIGHTS — the local raster would read "
           "whatever buffer that slot holds";

    // ONE declaration, not two #ifdef'd copies. They existed as two for about an
    // hour during #703 and had already drifted by a member, which is a silent
    // std430 layout change for whichever consumer took the shorter branch.
    EXPECT_EQ(1u, CountOccurrences(resources, "buffer VSMLocalLights"))
        << "VSMLocalLights is declared more than once — the copies will drift";
}

TEST(VirtualShadowMapLocal, LocalMipOffsetsMatchTheShaderTable)
{
    const std::string common = ReadShaderText("include/VirtualShadowCommon.glsl");
    ASSERT_FALSE(common.empty());

    // The shader spells the offsets as a literal int[], the same treatment (and
    // for the same reason) as the directional HPB table. Recompute them here.
    std::array<u32, VSM::kLocalMipCount> expected{};
    u32 running = 0;
    for (u32 mip = 0; mip < VSM::kLocalMipCount; ++mip)
    {
        expected[mip] = running;
        const u32 res = VSM::kLocalPageTableResolution >> mip;
        running += res * res;
    }

    for (u32 mip = 0; mip < VSM::kLocalMipCount; ++mip)
        EXPECT_EQ(expected[mip], VirtualShadowMap::LocalMipOffset(mip)) << "mip " << mip;

    std::smatch match;
    ASSERT_TRUE(std::regex_search(
        common, match,
        std::regex(R"(int\[VSM_LOCAL_MIP_COUNT\]\(([^)]*)\))")))
        << "vsmLocalMipOffset's literal table was not found in VirtualShadowCommon.glsl";

    std::stringstream stream(match[1].str());
    std::string token;
    u32 index = 0;
    while (std::getline(stream, token, ','))
    {
        ASSERT_LT(index, VSM::kLocalMipCount) << "the shader table is longer than VSM_LOCAL_MIP_COUNT";
        EXPECT_EQ(expected[index], static_cast<u32>(std::stoul(token)))
            << "vsmLocalMipOffset entry " << index << " drifted — every page past this mip is misaddressed";
        ++index;
    }
    EXPECT_EQ(VSM::kLocalMipCount, index);
}

TEST(VirtualShadowMapLocal, GPUStructLayoutsMatchTheirShaderTwins)
{
    // std430: mat4 + mat4 + vec4 + vec4.
    EXPECT_EQ(160u, sizeof(VSM::LocalLight));
    EXPECT_EQ(0u, offsetof(VSM::LocalLight, ViewProjection));
    EXPECT_EQ(64u, offsetof(VSM::LocalLight, ViewProjectionRaster));
    EXPECT_EQ(128u, offsetof(VSM::LocalLight, PositionRange));
    EXPECT_EQ(144u, offsetof(VSM::LocalLight, Params));

    // The two flavours must sit at DIFFERENT offsets. Collapsing them into one
    // "because they're the same" is only true on GL — the same trap
    // VirtualShadowMap.GPUStructLayoutsMatchTheirShaderTwins guards for the clip
    // projections.
    EXPECT_NE(offsetof(VSM::LocalLight, ViewProjection), offsetof(VSM::LocalLight, ViewProjectionRaster));

    // The cull record grew a second batch descriptor for the local run.
    EXPECT_EQ(128u, sizeof(VSM::CullInstance));
    EXPECT_EQ(96u, offsetof(VSM::CullInstance, Batch));
    EXPECT_EQ(112u, offsetof(VSM::CullInstance, LocalBatch));

    // The draw record did NOT grow: the local layer took a pad word, so the
    // directional path's stride is untouched.
    EXPECT_EQ(80u, sizeof(VSM::DrawInstance));
    EXPECT_EQ(64u, offsetof(VSM::DrawInstance, ClipLevel));
    EXPECT_EQ(68u, offsetof(VSM::DrawInstance, LocalLayer));

    // The globals block grew by two vec4s and nothing else moved.
    EXPECT_EQ(2752u, sizeof(VSM::GlobalsUBO));
    EXPECT_EQ(2720u, offsetof(VSM::GlobalsUBO, Params4));
    EXPECT_EQ(2736u, offsetof(VSM::GlobalsUBO, Params5));
    EXPECT_EQ(0u, sizeof(VSM::GlobalsUBO) % 16);
}

// =============================================================================
// 2. The meta-table encoding — one 32-bit word, two owner kinds
// =============================================================================

TEST(VirtualShadowMapLocal, MetaLocalOwnerRoundTripsAndCannotAliasADirectionalOwner)
{
    // The LOCAL bit must not overlap ALLOCATED or VISITED, or an allocated page
    // would read as local and its eviction would zero an unrelated page-table
    // entry — releasing one shadow and corrupting another, from one mistake.
    EXPECT_EQ(0u, VSM::kMetaLocalBit & VSM::kMetaAllocatedBit);
    EXPECT_EQ(0u, VSM::kMetaLocalBit & VSM::kMetaVisitedBit);

    const std::string common = ReadShaderText("include/VirtualShadowCommon.glsl");
    ASSERT_FALSE(common.empty());
    for (const auto& [name, value] : std::array<std::pair<const char*, u32>, 3>{
             { { "VSM_META_ALLOCATED_BIT", VSM::kMetaAllocatedBit },
               { "VSM_META_VISITED_BIT", VSM::kMetaVisitedBit },
               { "VSM_META_LOCAL_BIT", VSM::kMetaLocalBit } } })
    {
        std::smatch match;
        ASSERT_TRUE(std::regex_search(common, match, std::regex(std::string("#define\\s+") + name + R"(\s+\(1u\s*<<\s*(\d+)\))")))
            << name << " is missing from VirtualShadowCommon.glsl";
        EXPECT_EQ(value, 1u << std::stoul(match[1].str())) << name << " drifted between C++ and GLSL";
    }

    // The local owner payload — layer, mip, page x/y — has to fit strictly below
    // the LOCAL bit, or a large layer index would set it and the entry would
    // decode as some other kind of owner entirely.
    constexpr u32 kMaxLayer = VSM::kMaxLocalLayers - 1;
    constexpr u32 kMaxMip = VSM::kLocalMipCount - 1;
    constexpr u32 kMaxPage = VSM::kLocalPageTableResolution - 1;
    const u32 payload = (kMaxLayer << 15) | (kMaxMip << 12) | (kMaxPage << 6) | kMaxPage;
    EXPECT_LT(payload, VSM::kMetaLocalBit)
        << "the widest local owner payload overlaps the LOCAL bit";

    // And the shader's own packer, re-derived here, must agree field for field.
    for (u32 layer : { 0u, 1u, 42u, kMaxLayer })
    {
        for (u32 mip = 0; mip < VSM::kLocalMipCount; ++mip)
        {
            for (u32 page : { 0u, 7u, kMaxPage })
            {
                const u32 packed = VSM::kMetaLocalBit | (layer << 15) | (mip << 12) | (page << 6) | page;
                EXPECT_EQ(layer, (packed >> 15) & 0xFFu);
                EXPECT_EQ(mip, (packed >> 12) & 0x7u);
                EXPECT_EQ(page, (packed >> 6) & 0x3Fu);
                EXPECT_EQ(page, packed & 0x3Fu);
                EXPECT_NE(0u, packed & VSM::kMetaLocalBit);
            }
        }
    }

    // The request flag word lives in a different buffer but has the same job: `.w`
    // carries the LOCAL bit and, beneath it, the page-table mip. The two must not
    // overlap — a mip that reached the flag bit would make the allocator replay a
    // local request as a directional one against a layer index that is really a
    // clip level, and vice versa.
    //
    // (The previous version of this asserted `0u & bit == 0u`, which is true of
    // every possible bit and therefore said nothing.)
    EXPECT_EQ(0u, VSM::kRequestLocalBit & VSM::kRequestMipMask)
        << "the local flag overlaps the mip field — a request cannot carry both";
    EXPECT_GE(VSM::kRequestMipMask, VSM::kLocalMipCount - 1u)
        << "the mip field is too narrow to hold the coarsest mip, so a request for it "
           "would decode as a different mip entirely";
    // And the flag must be a real bit, so a DIRECTIONAL request's zero `.w` stays
    // distinguishable from a local one — that is what lets the pre-#703 encoding
    // keep working unchanged.
    EXPECT_NE(0u, VSM::kRequestLocalBit);
}

// =============================================================================
// 3. Page addressing
// =============================================================================

TEST(VirtualShadowMapLocal, LocalPagesAreInjectiveAndLiveAfterTheDirectionalRegion)
{
    // Exhaustive over the first few layers and every mip: any collision here maps
    // two different faces onto one page entry, which is two lights sharing (and
    // overwriting) one shadow.
    std::set<u32> seen;
    constexpr u32 kLayersUnderTest = 4;
    for (u32 layer = 0; layer < kLayersUnderTest; ++layer)
    {
        for (u32 mip = 0; mip < VSM::kLocalMipCount; ++mip)
        {
            const u32 mipRes = VSM::kLocalPageTableResolution >> mip;
            for (u32 y = 0; y < mipRes; ++y)
            {
                for (u32 x = 0; x < mipRes; ++x)
                {
                    const u32 index = VirtualShadowMap::LocalPageIndex(layer, mip, { x, y });
                    EXPECT_GE(index, VSM::kTotalVirtualPages)
                        << "a local page landed inside the directional region — it would alias a clip level";
                    EXPECT_LT(index, VSM::kTotalPageTableEntries);
                    EXPECT_TRUE(seen.insert(index).second)
                        << "collision at layer " << layer << " mip " << mip << " (" << x << "," << y << ")";
                }
            }
        }
    }
    EXPECT_EQ(kLayersUnderTest * VSM::kLocalPagesPerLayer, seen.size());

    // The last page of the last layer must be the last entry of the table — one
    // off either way and the final layer either overruns the buffer or wastes it.
    const u32 lastMip = VSM::kLocalMipCount - 1;
    EXPECT_EQ(VSM::kTotalPageTableEntries - 1,
              VirtualShadowMap::LocalPageIndex(VSM::kMaxLocalLayers - 1, lastMip, { 0u, 0u }));
}

// =============================================================================
// 4. The mip heuristic — the producer/consumer contract
// =============================================================================

TEST(VirtualShadowMapLocal, LocalMipIsMonotonicInBothDistances)
{
    constexpr f32 kBias = 1.0f;

    // Further from the CAMERA => a coarser (higher) mip: the same surface is
    // worth fewer screen pixels, so it is worth fewer shadow texels.
    i32 previous = -1;
    for (f32 cameraDistance : { 0.5f, 1.0f, 2.0f, 5.0f, 20.0f, 100.0f, 500.0f })
    {
        const i32 mip = VirtualShadowMap::SelectLocalMip(cameraDistance, 2.0f, kBias);
        EXPECT_GE(mip, previous) << "camera distance " << cameraDistance;
        EXPECT_GE(mip, 0);
        EXPECT_LT(mip, static_cast<i32>(VSM::kLocalMipCount));
        previous = mip;
    }

    // Further from the LIGHT => a finer (lower) mip: a face texel already covers
    // more world at that distance, so more of them are needed to match a pixel.
    previous = static_cast<i32>(VSM::kLocalMipCount);
    for (f32 lightDistance : { 0.1f, 0.5f, 1.0f, 5.0f, 20.0f, 100.0f })
    {
        const i32 mip = VirtualShadowMap::SelectLocalMip(50.0f, lightDistance, kBias);
        EXPECT_LE(mip, previous) << "light distance " << lightDistance;
        previous = mip;
    }

    // The bias behaves like ClipSelectionBias: > 1 pushes to cheaper (coarser)
    // mips, < 1 to sharper ones.
    //
    // STRICT inequalities at an operating point where NOTHING CLAMPS, and that is
    // the point of choosing 10 m / 2 m rather than something further out. The
    // first version of this test used a point whose high-bias answer saturated at
    // mip 5, so `>=` held even though the formula divided by the bias instead of
    // multiplying — i.e. the slider sharpened where it said it blurred, and the
    // clamp hid it. That inversion is what this test actually caught.
    const i32 mipSharp = VirtualShadowMap::SelectLocalMip(10.0f, 2.0f, 0.25f);
    const i32 mipNeutral = VirtualShadowMap::SelectLocalMip(10.0f, 2.0f, 1.0f);
    const i32 mipCoarse = VirtualShadowMap::SelectLocalMip(10.0f, 2.0f, 4.0f);
    EXPECT_GT(mipCoarse, mipNeutral) << "a bias > 1 must select a COARSER mip (" << mipCoarse << " vs "
                                     << mipNeutral << ") — the knob is inverted";
    EXPECT_LT(mipSharp, mipNeutral) << "a bias < 1 must select a FINER mip (" << mipSharp << " vs "
                                    << mipNeutral << ") — the knob is inverted";
    EXPECT_GT(mipCoarse, 0);
    EXPECT_LT(mipCoarse, static_cast<i32>(VSM::kLocalMipCount) - 1)
        << "the coarse probe saturated, so this comparison can no longer detect an inversion — move the "
           "operating point rather than relaxing the assertion";

    // Degenerate inputs must clamp, not produce a NaN mip that indexes the page
    // table with garbage.
    EXPECT_EQ(0, VirtualShadowMap::SelectLocalMip(0.0f, 0.0f, 1.0f));
    EXPECT_EQ(0, VirtualShadowMap::SelectLocalMip(0.0f, 1000.0f, 1.0f));
    EXPECT_EQ(static_cast<i32>(VSM::kLocalMipCount) - 1,
              VirtualShadowMap::SelectLocalMip(1.0e6f, 0.01f, 1.0f));
}

TEST(VirtualShadowMapLocal, DetailScalesWithScreenFootprintNotWithAnAuthoredRank)
{
    // The issue's SECOND acceptance criterion, as an arithmetic statement: two
    // lights identical in every authored property, differing only in how far the
    // camera is from them, must resolve to different mips — and the far one to
    // the cheaper mip. Under the atlas this was decided by a priority RANK, so
    // two identical lights could only differ by their scores' ordering.
    constexpr f32 kRange = 5.0f;
    constexpr f32 kBias = 1.0f;

    const i32 nearMip = VirtualShadowMap::SelectLocalMip(8.0f, kRange * 0.5f, kBias);
    const i32 farMip = VirtualShadowMap::SelectLocalMip(240.0f, kRange * 0.5f, kBias);

    EXPECT_LT(nearMip, farMip) << "a light 30x further away resolves to the same mip — detail is not "
                                  "following the screen footprint";

    // And the far one costs proportionally fewer pages. A mip-m face is
    // (32 >> m)² pages, so the ratio is what the VRAM claim rests on.
    const u32 nearPages = (VSM::kLocalPageTableResolution >> nearMip) * (VSM::kLocalPageTableResolution >> nearMip);
    const u32 farPages = (VSM::kLocalPageTableResolution >> farMip) * (VSM::kLocalPageTableResolution >> farMip);
    EXPECT_LT(farPages, nearPages);
    EXPECT_GE(nearPages / std::max(farPages, 1u), 16u)
        << "the far light should cost at least 16x fewer pages for a full face";
}

TEST(VirtualShadowMapLocal, TheMarkerAndTheSamplerCallOneMipHeuristic)
{
    // agent-rules/virtual-shadow-map-page-cache.md §1, restated for local lights:
    // the producer and the consumer must pick the SAME mip. The mechanism is that
    // there is only one implementation and both call it — so what this test
    // checks is that neither side grew a local copy.
    const std::string common = ReadShaderText("include/VirtualShadowCommon.glsl");
    const std::string marker = ReadShaderText("compute/VSM_MarkRequiredPages.comp");
    const std::string sampling = ReadShaderText("include/VirtualShadowSampling.glsl");
    ASSERT_FALSE(common.empty());
    ASSERT_FALSE(marker.empty());
    ASSERT_FALSE(sampling.empty());

    EXPECT_EQ(1u, CountOccurrences(common, "int vsmLocalMipForDistances("))
        << "vsmLocalMipForDistances is defined more than once";
    EXPECT_GE(CountOccurrences(marker, "vsmLocalMipForDistances("), 1u)
        << "the page marker no longer calls the shared mip heuristic";
    EXPECT_GE(CountOccurrences(sampling, "vsmLocalMipForDistances("), 1u)
        << "the sampler no longer calls the shared mip heuristic";

    // The same for the cube-face selector: three implementations exist by design
    // (GLSL for VSM, GLSL for the atlas, C++), and they must agree — so the VSM
    // ones must both route through vsmCubeFace rather than inlining the axes.
    EXPECT_EQ(1u, CountOccurrences(common, "int vsmCubeFace("));
    EXPECT_GE(CountOccurrences(marker, "vsmCubeFace("), 1u);
    EXPECT_GE(CountOccurrences(sampling, "vsmCubeFace("), 1u);
}

TEST(VirtualShadowMapLocal, CubeFaceSelectionMatchesTheAtlasFaceOrder)
{
    // +X,-X,+Y,-Y,+Z,-Z — the order ShadowMap::BuildPointLightFaceMatrices builds
    // and PBRCommon's atlasCubeFace selects. All three must agree or a light's
    // shadow appears on the wrong side of it.
    EXPECT_EQ(0u, VirtualShadowMap::SelectCubeFace({ 1.0f, 0.0f, 0.0f }));
    EXPECT_EQ(1u, VirtualShadowMap::SelectCubeFace({ -1.0f, 0.0f, 0.0f }));
    EXPECT_EQ(2u, VirtualShadowMap::SelectCubeFace({ 0.0f, 1.0f, 0.0f }));
    EXPECT_EQ(3u, VirtualShadowMap::SelectCubeFace({ 0.0f, -1.0f, 0.0f }));
    EXPECT_EQ(4u, VirtualShadowMap::SelectCubeFace({ 0.0f, 0.0f, 1.0f }));
    EXPECT_EQ(5u, VirtualShadowMap::SelectCubeFace({ 0.0f, 0.0f, -1.0f }));

    // Dominant axis wins, including at the diagonals where the tie-break decides.
    EXPECT_EQ(0u, VirtualShadowMap::SelectCubeFace({ 2.0f, 1.0f, 1.0f }));
    EXPECT_EQ(3u, VirtualShadowMap::SelectCubeFace({ 0.5f, -2.0f, 1.0f }));
    EXPECT_EQ(4u, VirtualShadowMap::SelectCubeFace({ 0.5f, 0.5f, 2.0f }));
}

// =============================================================================
// 5. The per-face projections
// =============================================================================

TEST(VirtualShadowMapLocal, EveryDirectionLandsInsideTheFaceItsSelectorPicks)
{
    // THE seam property for a cube: the face chosen by dominant axis must be the
    // face whose frustum actually contains the point. If the two ever disagree,
    // the marker requests one face's page and the sampler reads another's — and
    // an unbacked page reads LIT, so the artefact is an unshadowed wedge along a
    // cube edge, which looks like a light-leak bug.
    const glm::vec3 lightPosition(3.0f, 2.0f, -1.0f);
    const auto desc = MakePointLight(1, lightPosition, 20.0f);

    std::array<glm::mat4, 6> viewProjections{};
    f32 nearPlane = 0.0f;
    f32 farPlane = 0.0f;
    VirtualShadowMap::BuildLocalLightProjections(desc, viewProjections, nearPlane, farPlane);

    EXPECT_GT(nearPlane, 0.0f);
    EXPECT_FLOAT_EQ(20.0f, farPlane);

    // A dense sweep of directions, including the diagonals where the dominant
    // axis flips.
    u32 tested = 0;
    for (i32 iy = -8; iy <= 8; ++iy)
    {
        for (i32 ix = -8; ix <= 8; ++ix)
        {
            for (i32 iz = -8; iz <= 8; ++iz)
            {
                const glm::vec3 direction(static_cast<f32>(ix), static_cast<f32>(iy), static_cast<f32>(iz));
                if (glm::length(direction) < 0.5f)
                    continue;

                const glm::vec3 point = lightPosition + glm::normalize(direction) * 5.0f;
                const u32 face = VirtualShadowMap::SelectCubeFace(point - lightPosition);

                const glm::vec4 clipPos = viewProjections[face] * glm::vec4(point, 1.0f);
                ASSERT_GT(clipPos.w, 0.0f) << "face " << face << " projected the point behind its near plane";

                const glm::vec3 ndc = glm::vec3(clipPos) / clipPos.w;
                // A generous epsilon: the point sits exactly on the face boundary
                // at the diagonals, and floating point decides which side. The
                // shader's projection helper CLAMPS the uv for exactly this
                // reason — what must not happen is a point landing meaningfully
                // outside, which is a wrong face rather than a rounding question.
                constexpr f32 kEdgeEpsilon = 1.0e-3f;
                EXPECT_LE(std::abs(ndc.x), 1.0f + kEdgeEpsilon) << "face " << face;
                EXPECT_LE(std::abs(ndc.y), 1.0f + kEdgeEpsilon) << "face " << face;
                EXPECT_GE(ndc.z, -1.0f - kEdgeEpsilon) << "face " << face;
                EXPECT_LE(ndc.z, 1.0f + kEdgeEpsilon) << "face " << face;
                ++tested;
            }
        }
    }
    EXPECT_GT(tested, 4000u) << "the sweep degenerated — it is not testing what it claims";
}

TEST(VirtualShadowMapLocal, AllMipsOfALayerAgreeOnTheDepthOfAWorldPoint)
{
    // The local analogue of AllClipLevelsAgreeOnTheDepthOfAWorldPoint, and it
    // holds for a structural reason worth pinning: every mip of a layer shares
    // ONE projection — only the page grid's resolution changes — so a fragment
    // that falls back from mip m to mip m+1 compares against the same stored
    // number. If a mip ever gained its own projection this would break, and the
    // symptom would be a ring-shaped seam at each mip boundary.
    const glm::vec3 lightPosition(0.0f, 4.0f, 0.0f);
    const auto desc = MakeSpotLight(7, lightPosition, glm::vec3(0.0f, -1.0f, 0.0f), 30.0f, 40.0f);

    std::array<glm::mat4, 6> viewProjections{};
    f32 nearPlane = 0.0f;
    f32 farPlane = 0.0f;
    VirtualShadowMap::BuildLocalLightProjections(desc, viewProjections, nearPlane, farPlane);

    // The projection a layer's mips share is index 0 for a spot.
    const glm::vec3 point(1.5f, -2.0f, 0.75f);
    const glm::vec4 clipPos = viewProjections[0] * glm::vec4(point, 1.0f);
    ASSERT_GT(clipPos.w, 0.0f);
    const f32 depth = (clipPos.z / clipPos.w) * 0.5f + 0.5f;
    EXPECT_GE(depth, 0.0f);
    EXPECT_LE(depth, 1.0f);

    // Only the page grid changes with the mip. Re-derive the page a UV lands in
    // at each mip and check they nest — a point in page (x,y) at mip m must be in
    // page (x>>1, y>>1) at mip m+1, which is what makes the coarser mip a valid
    // fallback rather than a different address space.
    const glm::vec2 uv = glm::vec2(clipPos) / clipPos.w * 0.5f + 0.5f;
    ASSERT_GE(uv.x, 0.0f);
    ASSERT_LT(uv.x, 1.0f);
    ASSERT_GE(uv.y, 0.0f);
    ASSERT_LT(uv.y, 1.0f);

    glm::uvec2 previous{};
    for (u32 mip = 0; mip < VSM::kLocalMipCount; ++mip)
    {
        const u32 mipRes = VSM::kLocalPageTableResolution >> mip;
        const glm::uvec2 page{ static_cast<u32>(uv.x * static_cast<f32>(mipRes)),
                               static_cast<u32>(uv.y * static_cast<f32>(mipRes)) };
        ASSERT_LT(page.x, mipRes);
        ASSERT_LT(page.y, mipRes);
        if (mip > 0)
        {
            EXPECT_EQ(previous.x >> 1, page.x) << "mip " << mip;
            EXPECT_EQ(previous.y >> 1, page.y) << "mip " << mip;
        }
        previous = page;
    }
}

TEST(VirtualShadowMapLocal, SpotProjectionsClampADegenerateConeInsteadOfInverting)
{
    // A cutoff at or past 90 degrees asks glm::perspective for a >=180 degree
    // field of view, whose x/y scale is zero or negative. The atlas path produces
    // a garbage tile for that input; here it would poison a layer's CACHED pages,
    // so the builder clamps.
    for (f32 cutoff : { -5.0f, 0.0f, 89.5f, 90.0f, 175.0f })
    {
        const auto desc = MakeSpotLight(9, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), 10.0f, cutoff);
        std::array<glm::mat4, 6> viewProjections{};
        f32 nearPlane = 0.0f;
        f32 farPlane = 0.0f;
        VirtualShadowMap::BuildLocalLightProjections(desc, viewProjections, nearPlane, farPlane);

        const glm::mat4& viewProjection = viewProjections[0];
        for (i32 column = 0; column < 4; ++column)
        {
            for (i32 row = 0; row < 4; ++row)
                EXPECT_TRUE(std::isfinite(viewProjection[column][row])) << "cutoff " << cutoff;
        }
        // A point straight down the cone must project in front of the light and
        // inside the frustum, whatever the authored cutoff was.
        const glm::vec4 clipPos = viewProjection * glm::vec4(0.0f, 0.0f, -5.0f, 1.0f);
        EXPECT_GT(clipPos.w, 0.0f) << "cutoff " << cutoff;
        EXPECT_LE(std::abs(clipPos.x / clipPos.w), 1.0f) << "cutoff " << cutoff;
        EXPECT_LE(std::abs(clipPos.y / clipPos.w), 1.0f) << "cutoff " << cutoff;
    }

    // A zero direction must not produce a NaN view matrix — it arrives from a
    // component whose default is whatever the user last typed.
    const auto degenerate = MakeSpotLight(11, glm::vec3(1.0f), glm::vec3(0.0f), 10.0f, 35.0f);
    std::array<glm::mat4, 6> viewProjections{};
    f32 nearPlane = 0.0f;
    f32 farPlane = 0.0f;
    VirtualShadowMap::BuildLocalLightProjections(degenerate, viewProjections, nearPlane, farPlane);
    for (i32 column = 0; column < 4; ++column)
    {
        for (i32 row = 0; row < 4; ++row)
            EXPECT_TRUE(std::isfinite(viewProjections[0][column][row]));
    }
}

TEST(VirtualShadowMapLocal, PointFacesMatchTheAtlasBuildersAxesAndUpVectors)
{
    // light-path-photometric-parity.md: the two shadow techniques must place a
    // light's frustum identically, or switching between them MOVES the shadow.
    // The atlas builder works in absolute world space and this one in
    // render-relative space, so they are compared at a render origin of zero,
    // where the two spaces coincide.
    const glm::vec3 lightPosition(2.0f, 5.0f, -3.0f);
    constexpr f32 kRange = 25.0f;

    const auto atlasFaces = ShadowMap::BuildPointLightFaceMatrices(lightPosition, kRange);

    std::array<glm::mat4, 6> vsmFaces{};
    f32 nearPlane = 0.0f;
    f32 farPlane = 0.0f;
    VirtualShadowMap::BuildLocalLightProjections(MakePointLight(3, lightPosition, kRange), vsmFaces, nearPlane,
                                                 farPlane);

    // Not matrix equality: the near planes differ by design (the VSM floors its
    // near plane against the range). What must match is WHERE each face looks —
    // so a probe point on each axis must land in the same face, at the same UV.
    for (u32 face = 0; face < 6; ++face)
    {
        const glm::vec3 axis = glm::vec3(face == 0 ? 1.0f : face == 1 ? -1.0f
                                                                      : 0.0f,
                                         face == 2 ? 1.0f : face == 3 ? -1.0f
                                                                      : 0.0f,
                                         face == 4 ? 1.0f : face == 5 ? -1.0f
                                                                      : 0.0f);
        // Off-axis on purpose: an on-axis probe lands at the face centre and
        // would pass even if the up vectors were rotated.
        //
        // Driven by the FACE INDEX, not by comparing the axis components against
        // 0.0f — floating-point equality is forbidden here (cpp-coding-quality.md
        // par.2) and the old spelling additionally had a dead `? 0.0f : 0.0f`
        // ternary on z. Both offsets stay well under the axis' own 6.0, so the
        // dominant component is unchanged and SelectCubeFace still answers `face`.
        const bool xDominant = (face < 2);
        const bool yDominant = (face >= 2 && face < 4);
        const glm::vec3 offAxis(xDominant ? 0.0f : 1.3f, yDominant ? 0.0f : 0.9f, 0.0f);
        const glm::vec3 probe = lightPosition + axis * 6.0f + offAxis;

        ASSERT_EQ(face, VirtualShadowMap::SelectCubeFace(probe - lightPosition));

        const glm::vec4 atlasClip = atlasFaces[face] * glm::vec4(probe, 1.0f);
        const glm::vec4 vsmClip = vsmFaces[face] * glm::vec4(probe, 1.0f);
        ASSERT_GT(atlasClip.w, 0.0f) << "face " << face;
        ASSERT_GT(vsmClip.w, 0.0f) << "face " << face;

        const glm::vec2 atlasUV = glm::vec2(atlasClip) / atlasClip.w;
        const glm::vec2 vsmUV = glm::vec2(vsmClip) / vsmClip.w;
        EXPECT_NEAR(atlasUV.x, vsmUV.x, 1.0e-4f) << "face " << face << " x";
        EXPECT_NEAR(atlasUV.y, vsmUV.y, 1.0e-4f) << "face " << face << " y";
    }
}

// =============================================================================
// 6. The layer pool
// =============================================================================

TEST(VirtualShadowMapLocal, ALightKeepsItsLayersWhileItStandsStill)
{
    // The caching invariant. Layers are what a light's cached pages are addressed
    // by, so reassigning them every frame is CORRECT and redraws the entire local
    // pool every frame — which looks like the raster being slow, not like the
    // cache being off.
    VirtualShadowMap::LocalLayerPool pool;

    const auto lamp = MakePointLight(101, glm::vec3(1.0f, 2.0f, 3.0f), 10.0f);

    pool.BeginFrame();
    bool moved = false;
    const u32 first = pool.Acquire(lamp, moved);
    ASSERT_NE(VirtualShadowMap::kNoLocalSlot, first);
    EXPECT_TRUE(moved) << "a fresh slot must report as moved so its projections get written";
    const u32 firstBase = pool.Slots[first].Base;
    EXPECT_TRUE(pool.Invalidate[firstBase]) << "a recycled layer must be flushed before its new owner uses it";

    for (u32 frame = 0; frame < 5; ++frame)
    {
        pool.BeginFrame();
        moved = true;
        const u32 slot = pool.Acquire(lamp, moved);
        ASSERT_NE(VirtualShadowMap::kNoLocalSlot, slot);
        EXPECT_EQ(first, slot) << "frame " << frame << ": the light was handed a different slot";
        EXPECT_EQ(firstBase, pool.Slots[slot].Base) << "frame " << frame;
        EXPECT_FALSE(moved) << "frame " << frame << ": a stationary light reported as moved";
        for (u32 layer = firstBase; layer < firstBase + 6; ++layer)
            EXPECT_FALSE(pool.Invalidate[layer]) << "frame " << frame << " layer " << layer;
    }
}

TEST(VirtualShadowMapLocal, AMovedLightFlushesExactlyItsOwnLayers)
{
    VirtualShadowMap::LocalLayerPool pool;

    const auto stationary = MakePointLight(1, glm::vec3(0.0f), 10.0f);
    auto mover = MakePointLight(2, glm::vec3(20.0f, 0.0f, 0.0f), 10.0f);

    pool.BeginFrame();
    bool moved = false;
    const u32 stationarySlot = pool.Acquire(stationary, moved);
    const u32 moverSlot = pool.Acquire(mover, moved);
    ASSERT_NE(VirtualShadowMap::kNoLocalSlot, stationarySlot);
    ASSERT_NE(VirtualShadowMap::kNoLocalSlot, moverSlot);
    const u32 stationaryBase = pool.Slots[stationarySlot].Base;
    const u32 moverBase = pool.Slots[moverSlot].Base;
    ASSERT_NE(stationaryBase, moverBase);

    pool.BeginFrame();
    mover.PositionRelative.x += 3.0f;
    moved = false;
    pool.Acquire(stationary, moved);
    EXPECT_FALSE(moved);
    moved = false;
    pool.Acquire(mover, moved);
    EXPECT_TRUE(moved) << "a light that moved must rebuild its projections";

    for (u32 i = 0; i < 6; ++i)
    {
        EXPECT_TRUE(pool.Invalidate[moverBase + i]) << "the mover's layer " << i << " was not flushed";
        EXPECT_FALSE(pool.Invalidate[stationaryBase + i])
            << "one light moving cost another light its cache — layer " << i;
    }

    // A sub-epsilon jitter must NOT flush: these poses are recomputed from a
    // transform every frame, and a last-bit difference torching a light's whole
    // cache is the same class of mistake SetSettings' epsilon compare avoids.
    pool.BeginFrame();
    mover.PositionRelative.x += 1.0e-7f;
    moved = false;
    pool.Acquire(mover, moved);
    EXPECT_FALSE(moved) << "floating-point jitter flushed a stationary light";
}

TEST(VirtualShadowMapLocal, PointAndSpotTakeContiguousRunsOfTheRightLength)
{
    VirtualShadowMap::LocalLayerPool pool;
    pool.BeginFrame();

    bool moved = false;
    const u32 spot = pool.Acquire(MakeSpotLight(1, glm::vec3(0.0f), glm::vec3(0, 0, -1), 8.0f, 30.0f), moved);
    const u32 point = pool.Acquire(MakePointLight(2, glm::vec3(5.0f), 8.0f), moved);
    const u32 spot2 = pool.Acquire(MakeSpotLight(3, glm::vec3(9.0f), glm::vec3(0, -1, 0), 8.0f, 30.0f), moved);

    ASSERT_NE(VirtualShadowMap::kNoLocalSlot, spot);
    ASSERT_NE(VirtualShadowMap::kNoLocalSlot, point);
    ASSERT_NE(VirtualShadowMap::kNoLocalSlot, spot2);

    EXPECT_EQ(1u, pool.Slots[spot].Count);
    EXPECT_EQ(6u, pool.Slots[point].Count);
    EXPECT_EQ(1u, pool.Slots[spot2].Count);

    // A point light's six faces MUST be contiguous: the sampler reaches face f as
    // `layerBase + f`, so a gap would send it to another light's layer.
    const u32 pointBase = pool.Slots[point].Base;
    for (u32 i = 0; i < 6; ++i)
        EXPECT_EQ(point + 1, pool.Owner[pointBase + i]) << "face " << i << " is not owned by the point light";

    // Every allocated layer names a LIVE slot, and every slot owns exactly as
    // many layers as its Count says. That is what catches two slots overlapping a
    // layer, or a slot whose run was resized without its ownership following.
    //
    // (The previous version inserted the LOOP VARIABLE into a set and asserted the
    // insert succeeded — unique by construction, so it could not fail.)
    std::map<u32, u32> layersPerSlot;
    u32 ownedLayers = 0;
    for (u32 layer = 0; layer < VSM::kMaxLocalLayers; ++layer)
    {
        const u16 owner = pool.Owner[layer];
        if (owner == 0)
            continue;
        ++ownedLayers;
        const u32 slotIndex = static_cast<u32>(owner) - 1u;
        ASSERT_LT(slotIndex, pool.Slots.size()) << "layer " << layer << " names a slot that does not exist";
        EXPECT_NE(0u, pool.Slots[slotIndex].Count)
            << "layer " << layer << " is owned by a FREED slot — the run was released without its "
                                    "ownership entries";
        ++layersPerSlot[slotIndex];
    }
    for (const auto& [slotIndex, layerCount] : layersPerSlot)
    {
        EXPECT_EQ(pool.Slots[slotIndex].Count, layerCount)
            << "slot " << slotIndex << " claims " << pool.Slots[slotIndex].Count << " layers but owns "
            << layerCount << " — the runs overlap";
    }
    EXPECT_EQ(3u, layersPerSlot.size()) << "expected exactly the three lights allocated above";
    EXPECT_EQ(8u, ownedLayers);
    EXPECT_EQ(8u, pool.LayerHighWater());
}

TEST(VirtualShadowMapLocal, TheLayerPoolBeatsTheAtlasBudgetAndReportsExhaustion)
{
    // The issue's FIRST acceptance criterion, as a pool-level statement: the
    // atlas capped local shadows at 16 lights / 32 entries by construction. The
    // layer pool takes 42 point lights (6 layers each) or 256 spots, from the
    // same 256 layers, at no VRAM cost per layer.
    VirtualShadowMap::LocalLayerPool pool;
    pool.BeginFrame();

    u32 accepted = 0;
    bool moved = false;
    for (u32 i = 0; i < 64; ++i)
    {
        const auto light = MakePointLight(1000 + i, glm::vec3(static_cast<f32>(i) * 4.0f, 0.0f, 0.0f), 5.0f);
        if (pool.Acquire(light, moved) != VirtualShadowMap::kNoLocalSlot)
            ++accepted;
    }

    EXPECT_EQ(VSM::kMaxLocalLayers / 6, accepted)
        << "the pool did not pack point lights six layers at a time";
    EXPECT_GT(accepted, ShadowAtlas::kMaxShadowedLights)
        << "the layer pool takes no more shadowed point lights than the atlas did — criterion 1 unmet";

    // Spot lights are one layer each, so the same pool takes all 256.
    VirtualShadowMap::LocalLayerPool spotPool;
    spotPool.BeginFrame();
    accepted = 0;
    for (u32 i = 0; i < VSM::kMaxLocalLayers + 8; ++i)
    {
        const auto light = MakeSpotLight(2000 + i, glm::vec3(static_cast<f32>(i), 0.0f, 0.0f),
                                         glm::vec3(0.0f, -1.0f, 0.0f), 5.0f, 30.0f);
        if (spotPool.Acquire(light, moved) != VirtualShadowMap::kNoLocalSlot)
            ++accepted;
    }
    EXPECT_EQ(VSM::kMaxLocalLayers, accepted);

    // And exhaustion is a reported -1, never a silently reused layer: two lights
    // sharing a layer would overwrite each other's pages every frame.
    EXPECT_EQ(VirtualShadowMap::kNoLocalSlot,
              spotPool.Acquire(MakeSpotLight(99999, glm::vec3(0.0f), glm::vec3(0, -1, 0), 5.0f, 30.0f), moved));
}

TEST(VirtualShadowMapLocal, EvictionTakesTheLeastRecentlyUsedIdleSlotAndNeverAnActiveOne)
{
    VirtualShadowMap::LocalLayerPool pool;

    // Fill the pool with spots, then let two of them go idle for different
    // numbers of frames.
    bool moved = false;
    pool.BeginFrame();
    for (u32 i = 0; i < VSM::kMaxLocalLayers; ++i)
    {
        ASSERT_NE(VirtualShadowMap::kNoLocalSlot,
                  pool.Acquire(MakeSpotLight(i + 1, glm::vec3(static_cast<f32>(i)), glm::vec3(0, -1, 0), 5.0f,
                                             30.0f),
                               moved));
    }

    const u32 oldestId = 1;
    const u32 lessOldId = 2;
    const u32 oldestBase = pool.Slots[pool.ByLight.at(oldestId)].Base;

    // Frame 2: everyone but the oldest renews.
    pool.BeginFrame();
    for (u32 i = 1; i < VSM::kMaxLocalLayers; ++i)
    {
        pool.Acquire(MakeSpotLight(i + 1, glm::vec3(static_cast<f32>(i)), glm::vec3(0, -1, 0), 5.0f, 30.0f),
                     moved);
    }

    // Frame 3: the second-oldest drops out too, so the two idle slots have
    // different LastUsedFrame values and the tie-break is real.
    pool.BeginFrame();
    for (u32 i = 2; i < VSM::kMaxLocalLayers; ++i)
    {
        pool.Acquire(MakeSpotLight(i + 1, glm::vec3(static_cast<f32>(i)), glm::vec3(0, -1, 0), 5.0f, 30.0f),
                     moved);
    }

    // A new light must evict the OLDEST idle one, not the merely-idle one and
    // certainly not one still in use this frame.
    const u32 fresh = pool.Acquire(MakeSpotLight(50000, glm::vec3(-1.0f), glm::vec3(0, -1, 0), 5.0f, 30.0f),
                                   moved);
    ASSERT_NE(VirtualShadowMap::kNoLocalSlot, fresh);
    EXPECT_EQ(oldestBase, pool.Slots[fresh].Base) << "eviction did not pick the least-recently-used slot";
    EXPECT_EQ(0u, pool.ByLight.count(oldestId)) << "the evicted light kept its mapping";
    EXPECT_EQ(1u, pool.ByLight.count(lessOldId)) << "a newer idle slot was evicted instead";
    EXPECT_TRUE(pool.Invalidate[oldestBase])
        << "the evicted slot's pages were not flushed — its layers would read as cached and never redraw";
}

TEST(VirtualShadowMapLocal, ALightThatChangesClassGivesUpItsRunRatherThanResizingIt)
{
    // A spot that becomes a point needs six layers where it had one. Resizing in
    // place would overlap whatever sits after it, so the run is released and
    // re-taken — and the old layers must be flushed on the way out.
    VirtualShadowMap::LocalLayerPool pool;
    pool.BeginFrame();

    bool moved = false;
    const u32 spotSlot =
        pool.Acquire(MakeSpotLight(1, glm::vec3(0.0f), glm::vec3(0, 0, -1), 8.0f, 30.0f), moved);
    ASSERT_NE(VirtualShadowMap::kNoLocalSlot, spotSlot);
    const u32 spotBase = pool.Slots[spotSlot].Base;
    const u32 neighbour = pool.Acquire(MakeSpotLight(2, glm::vec3(4.0f), glm::vec3(0, 0, -1), 8.0f, 30.0f), moved);
    ASSERT_NE(VirtualShadowMap::kNoLocalSlot, neighbour);
    const u32 neighbourBase = pool.Slots[neighbour].Base;

    pool.BeginFrame();
    const u32 pointSlot = pool.Acquire(MakePointLight(1, glm::vec3(0.0f), 8.0f), moved);
    ASSERT_NE(VirtualShadowMap::kNoLocalSlot, pointSlot);
    EXPECT_EQ(6u, pool.Slots[pointSlot].Count);
    EXPECT_TRUE(moved);
    EXPECT_TRUE(pool.Invalidate[spotBase]) << "the abandoned spot layer was not flushed";

    // The neighbour must be untouched — a resize would have run over it.
    ASSERT_EQ(1u, pool.ByLight.count(2u));
    EXPECT_EQ(neighbourBase, pool.Slots[pool.ByLight.at(2u)].Base);
    EXPECT_EQ(neighbour + 1, pool.Owner[neighbourBase]);

    // And the point light's new run must not overlap the neighbour.
    const u32 pointBase = pool.Slots[pointSlot].Base;
    for (u32 i = 0; i < 6; ++i)
        EXPECT_NE(neighbourBase, pointBase + i);
}

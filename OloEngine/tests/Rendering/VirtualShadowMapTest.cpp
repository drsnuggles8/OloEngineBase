// OLO_TEST_LAYER: L1
//
// Virtual Shadow Map contract tests (issue #702).
//
// Everything here is CPU-only and runs in headless CI. That is deliberate: the
// three invariants VSM rests on are all expressible without a GPU, and every one
// of them fails SILENTLY on screen — a wrong wrap makes shadows swim, a
// per-level depth difference draws a seam at a clip boundary, and a C++/GLSL
// constant drift reinterprets every page entry into plausible garbage. A
// screenshot can show you that something is wrong; only these can tell you which.
//
// The pixels are covered separately: see the multi-angle evidence capture in the
// PR description and docs/agent-rules/notes-renderer.md.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/Shadow/VirtualShadowMap.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

using namespace OloEngine;

namespace
{
    // The shader half of every constant this system shares. Parsed rather than
    // duplicated: a test that restates the number is a second copy to keep in
    // sync, which is the failure it exists to prevent.
    [[nodiscard]] std::string ReadShaderFile(const char* relative)
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

    [[nodiscard]] std::string ReadShaderInclude(const char* name)
    {
        namespace fs = std::filesystem;
        const std::array<fs::path, 3> candidates{
            fs::path("OloEditor/assets/shaders/include") / name,
            fs::path("assets/shaders/include") / name,
            fs::path("../OloEditor/assets/shaders/include") / name,
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
    // whose body is an expression (those are checked by their evaluated result
    // through a sibling define instead).
    [[nodiscard]] bool ParseIntDefine(const std::string& source, const std::string& name, i64& out)
    {
        const std::regex pattern("#define\\s+" + name + "\\s+(-?[0-9]+)\\s");
        std::smatch match;
        if (!std::regex_search(source, match, pattern))
            return false;
        out = std::stoll(match[1].str());
        return true;
    }

    // A page's centre in world space, for the wrap tests below. Uses the same
    // basis BuildClipProjections does, so a change to one is visible here.
    [[nodiscard]] glm::vec3 SunDirection()
    {
        return glm::normalize(glm::vec3(0.35f, -1.0f, 0.22f));
    }
} // namespace

// =============================================================================
// 1. The C++/GLSL constant mirror
// =============================================================================

TEST(VirtualShadowMap, ConstantsMirrorTheShaderContract)
{
    const std::string common = ReadShaderInclude("VirtualShadowCommon.glsl");
    ASSERT_FALSE(common.empty())
        << "VirtualShadowCommon.glsl not found — the mirror test cannot run and would pass vacuously";

    struct Mirror
    {
        const char* Define;
        u32 Cpp;
    };
    const std::array<Mirror, 8> mirrors{ {
        { "VSM_VIRTUAL_RESOLUTION", VSM::kVirtualResolution },
        { "VSM_PAGE_SIZE", VSM::kPageSize },
        { "VSM_PAGE_SIZE_LOG2", VSM::kPageSizeLog2 },
        { "VSM_CLIP_LEVELS", VSM::kClipLevels },
        { "VSM_HPB_MIP_COUNT", VSM::kHPBMipCount },
        { "VSM_MAX_REQUESTS", VSM::kMaxRequests },
        // The cull validates every CPU-supplied batch record against these two
        // before touching its buffers — a drift here turns that tripwire into
        // either a scribble (GLSL smaller) or a false overflow (GLSL larger).
        { "VSM_MAX_BATCHES", VSM::kMaxBatches },
        { "VSM_MAX_DRAW_INSTANCES", VSM::kMaxDrawInstances },
    } };

    for (const auto& mirror : mirrors)
    {
        i64 shaderValue = 0;
        ASSERT_TRUE(ParseIntDefine(common, mirror.Define, shaderValue))
            << mirror.Define << " is missing from VirtualShadowCommon.glsl";
        EXPECT_EQ(static_cast<i64>(mirror.Cpp), shaderValue)
            << mirror.Define << " drifted between C++ and GLSL — every page entry is now reinterpreted";
    }

    // The derived constants, checked by value rather than by re-parsing the
    // expressions the shader spells them with.
    EXPECT_EQ(VSM::kPageTableResolution, VSM::kVirtualResolution / VSM::kPageSize);
    EXPECT_EQ(VSM::kPageSize, 1u << VSM::kPageSizeLog2);
    EXPECT_EQ(VSM::kPagesPerClipLevel, VSM::kPageTableResolution * VSM::kPageTableResolution);
    EXPECT_EQ(VSM::kTotalVirtualPages, VSM::kPagesPerClipLevel * VSM::kClipLevels);

    // Two sampler bindings are hand-written LITERALS in the shaders (GLSL on
    // this compile route cannot read C++ constants), each next to a comment
    // naming the constant it mirrors. The comment is not a mechanism; this is.
    // A drift makes the kernel/sampler read whatever texture the stale slot
    // holds — a wrong-resource read, not an error.
    {
        const std::string markKernel = ReadShaderFile("compute/VSM_MarkRequiredPages.comp");
        ASSERT_FALSE(markKernel.empty()) << "VSM_MarkRequiredPages.comp not found";
        std::smatch match;
        ASSERT_TRUE(std::regex_search(
            markKernel, match,
            std::regex(R"(layout\(binding\s*=\s*(\d+)\)\s*uniform\s+sampler2D\s+u_VSMSceneDepth)")))
            << "u_VSMSceneDepth declaration not found in VSM_MarkRequiredPages.comp";
        EXPECT_EQ(static_cast<u32>(std::stoul(match[1].str())), ShaderBindingLayout::TEX_POSTPROCESS_DEPTH)
            << "u_VSMSceneDepth's binding literal drifted from TEX_POSTPROCESS_DEPTH — the marker now "
               "unprojects whatever texture that slot holds and marks pages for a scene that does not "
               "exist";

        const std::string sampling = ReadShaderInclude("VirtualShadowSampling.glsl");
        ASSERT_FALSE(sampling.empty()) << "VirtualShadowSampling.glsl not found";
        ASSERT_TRUE(std::regex_search(
            sampling, match,
            std::regex(R"(layout\(binding\s*=\s*(\d+)\)\s*uniform\s+usampler2D\s+u_VSMPhysicalPool)")))
            << "u_VSMPhysicalPool declaration not found in VirtualShadowSampling.glsl";
        EXPECT_EQ(static_cast<u32>(std::stoul(match[1].str())), ShaderBindingLayout::TEX_VSM_PHYSICAL)
            << "u_VSMPhysicalPool's binding literal drifted from TEX_VSM_PHYSICAL";
    }

    // The shader shifts by "the page TABLE's log2" in the cull's seam test. That
    // it currently equals the page size's log2 is a coincidence of 4096/64 = 64,
    // and code that relied on the coincidence would break silently on a retune.
    i64 tableLog2 = 0;
    ASSERT_TRUE(ParseIntDefine(common, "VSM_PAGE_TABLE_RES_LOG2", tableLog2));
    EXPECT_EQ(VSM::kPageTableResolution, 1u << tableLog2);

    // The HPB pyramid size, which the shader hard-codes as a table.
    u32 expectedEntries = 0;
    for (u32 mip = 0; mip < VSM::kHPBMipCount; ++mip)
    {
        const u32 res = VSM::kPageTableResolution >> mip;
        expectedEntries += res * res;
    }
    EXPECT_EQ(VSM::kHPBEntriesPerLevel, expectedEntries);
    EXPECT_EQ(1u, VSM::kPageTableResolution >> (VSM::kHPBMipCount - 1))
        << "the pyramid must bottom out at exactly one texel per clip level";
}

TEST(VirtualShadowMap, HPBMipOffsetsMatchTheShaderTable)
{
    const std::string common = ReadShaderInclude("VirtualShadowCommon.glsl");
    ASSERT_FALSE(common.empty());

    // The shader spells the offsets as a literal int[] because a 7-deep table is
    // cheaper (and provably right) than the closed form. Recompute them here.
    std::array<u32, VSM::kHPBMipCount> expected{};
    u32 running = 0;
    for (u32 mip = 0; mip < VSM::kHPBMipCount; ++mip)
    {
        expected[mip] = running;
        const u32 res = VSM::kPageTableResolution >> mip;
        running += res * res;
    }

    const std::regex tablePattern(R"(int\[VSM_HPB_MIP_COUNT\]\(([^)]*)\))");
    std::smatch match;
    ASSERT_TRUE(std::regex_search(common, match, tablePattern))
        << "the HPB mip-offset table is no longer an int[] literal — update this test with it";

    std::stringstream values(match[1].str());
    std::string token;
    u32 index = 0;
    while (std::getline(values, token, ','))
    {
        ASSERT_LT(index, VSM::kHPBMipCount) << "the shader table has more entries than VSM_HPB_MIP_COUNT";
        EXPECT_EQ(expected[index], static_cast<u32>(std::stoul(token)))
            << "HPB mip " << index << " offset drifted — the cull would read another mip's dirty flags";
        ++index;
    }
    EXPECT_EQ(VSM::kHPBMipCount, index);
}

// =============================================================================
// 2. Page-state encoding
// =============================================================================

TEST(VirtualShadowMap, PageStateBitsAreDisjointAndLeaveRoomForTheCoords)
{
    const u32 flagBits = VSM::kPageAllocatedBit | VSM::kPageRequestsAllocBit | VSM::kPageDirtyBit |
                         VSM::kPageVisitedBit | VSM::kPageAllocFailedBit;

    // Five distinct bits, no overlap.
    EXPECT_EQ(5, std::popcount(flagBits));

    // The low 16 bits carry the physical page coordinates and must not collide
    // with any flag — a collision would make a page at (255, y) read as dirty.
    constexpr u32 kCoordMask = 0x0000FFFFu;
    EXPECT_EQ(0u, flagBits & kCoordMask)
        << "a page-state flag overlaps the physical-coordinate field";

    // The coordinate field must address the largest pool the settings allow.
    // 8 bits per axis => 256 pages => 256 * 64 = 16384 texels, comfortably past
    // the 8192 clamp in SanitizePhysicalResolution.
    EXPECT_GE(256u * VSM::kPageSize, 8192u);

    // Meta entries use the top two bits and 20 bits of owner (8 + 8 + 4).
    const u32 metaFlags = VSM::kMetaAllocatedBit | VSM::kMetaVisitedBit;
    EXPECT_EQ(2, std::popcount(metaFlags));
    EXPECT_EQ(0u, metaFlags & 0x000FFFFFu) << "a meta flag overlaps the owner field";
    EXPECT_LE(VSM::kClipLevels, 16u) << "the meta owner field carries only 4 bits of clip level";
    EXPECT_LE(VSM::kPageTableResolution, 256u) << "the meta owner field carries only 8 bits per page axis";
}

TEST(VirtualShadowMap, GPUStructLayoutsMatchTheirShaderTwins)
{
    // std140/std430 sizes. A drift here does not fail to compile; it shifts every
    // subsequent member and produces plausible-looking garbage.
    EXPECT_EQ(160u, sizeof(VSM::ClipProjection));
    EXPECT_EQ(2720u, sizeof(VSM::GlobalsUBO));
    EXPECT_EQ(16u, sizeof(VSM::PassUBO));
    EXPECT_EQ(112u, sizeof(VSM::CullInstance));
    EXPECT_EQ(80u, sizeof(VSM::DrawInstance));
    // Must match GL's DrawElementsIndirectCommand exactly — the driver reads it.
    EXPECT_EQ(20u, sizeof(VSM::DrawCommand));
    EXPECT_EQ(32u, sizeof(VSM::Statistics));

    EXPECT_EQ(0u, sizeof(VSM::GlobalsUBO) % 16u);
    EXPECT_EQ(sizeof(VSM::ClipProjection) * VSM::kClipLevels + 64u + 6u * 16u, sizeof(VSM::GlobalsUBO));
}

TEST(VirtualShadowMap, BindingSlotsDoNotCollideWithTheReservedVertexPullStreams)
{
    // SSBO 57 and 63 are reserved engine-wide for the Vulkan vertex-pull streams,
    // and TEX 63/64 are kept out of the sampler namespace for the same reason
    // (ShaderBindingLayout's A2 note). Claiming one is a within-shader collision
    // on Vulkan's single-set model, not a compile error.
    const std::array<u32, 10> vsmStorage{ {
        ShaderBindingLayout::SSBO_VSM_PAGE_TABLE,
        ShaderBindingLayout::SSBO_VSM_META_TABLE,
        ShaderBindingLayout::SSBO_VSM_HPB,
        ShaderBindingLayout::SSBO_VSM_REQUESTS,
        ShaderBindingLayout::SSBO_VSM_FREE_PAGES,
        ShaderBindingLayout::SSBO_VSM_INVALIDATIONS,
        ShaderBindingLayout::SSBO_VSM_CULL_INSTANCES,
        ShaderBindingLayout::SSBO_VSM_DRAW_INSTANCES,
        ShaderBindingLayout::SSBO_VSM_DRAW_COMMANDS,
        ShaderBindingLayout::SSBO_VSM_STATS,
    } };
    for (const u32 binding : vsmStorage)
    {
        EXPECT_NE(ShaderBindingLayout::SSBO_VERTEX_PULL, binding);
        EXPECT_NE(ShaderBindingLayout::SSBO_BONE_PULL, binding);
    }

    // No two VSM storage bindings may share a slot either.
    for (sizet i = 0; i < vsmStorage.size(); ++i)
    {
        for (sizet j = i + 1; j < vsmStorage.size(); ++j)
            EXPECT_NE(vsmStorage[i], vsmStorage[j]) << "two VSM SSBOs share binding " << vsmStorage[i];
    }

    EXPECT_NE(ShaderBindingLayout::SSBO_VERTEX_PULL, ShaderBindingLayout::TEX_VSM_PHYSICAL)
        << "the VSM pool sampler collides with the vertex-pull stream — the exact A2 collision";
    EXPECT_LT(ShaderBindingLayout::TEX_VSM_PHYSICAL, ShaderBindingLayout::TEX_SHADER_GRAPH_0)
        << "engine sampler slots must stay below the shader-graph user base";
    EXPECT_LT(ShaderBindingLayout::UBO_VIRTUAL_SHADOW_DRAW, ShaderBindingLayout::UBO_BINDING_LIMIT);
}

// =============================================================================
// 3. Clip-level selection — the producer/consumer agreement
// =============================================================================

TEST(VirtualShadowMap, ClipLevelSelectionIsMonotonicAndCoversTheConfiguredRange)
{
    constexpr f32 kClip0 = 2.0f;
    constexpr f32 kBias = 1.0f;

    i32 previous = -1;
    for (f32 distance = 0.1f; distance < 100000.0f; distance *= 1.3f)
    {
        const i32 level = VirtualShadowMap::SelectClipLevel(distance, kClip0, kBias);
        EXPECT_GE(level, 0);
        EXPECT_LT(level, static_cast<i32>(VSM::kClipLevels));
        EXPECT_GE(level, previous) << "clip level went BACKWARDS as distance grew — rings would interleave";
        previous = level;
    }

    // The far end must actually be reachable, or the configured range is a lie:
    // level 15 covers kClip0 * 2^15 = 65 km, which is the "shadow distance well
    // past the 200 m default" the issue asks for.
    EXPECT_EQ(static_cast<i32>(VSM::kClipLevels) - 1, VirtualShadowMap::SelectClipLevel(60000.0f, kClip0, kBias));
    EXPECT_EQ(0, VirtualShadowMap::SelectClipLevel(0.5f, kClip0, kBias));

    // The bias moves work between levels in the documented direction.
    const i32 unbiased = VirtualShadowMap::SelectClipLevel(100.0f, kClip0, 1.0f);
    EXPECT_GE(VirtualShadowMap::SelectClipLevel(100.0f, kClip0, 4.0f), unbiased) << "bias > 1 must coarsen";
    EXPECT_LE(VirtualShadowMap::SelectClipLevel(100.0f, kClip0, 0.25f), unbiased) << "bias < 1 must sharpen";
}

// =============================================================================
// 4. The caching invariant — a world point keeps its page slot as the camera moves
// =============================================================================

TEST(VirtualShadowMap, AWorldPointKeepsItsWrappedPageAcrossCameraTranslation)
{
    // THE invariant the whole system rests on. Clip frusta move in whole-page
    // increments and the wrapped slot of a page is decided by its WORLD page
    // index, so a static world point must land in the same page-table slot before
    // and after the camera moves. If it does not, every cached texel is being
    // reused for the wrong piece of the world — which reads on screen as shadows
    // that swim as you walk, not as anything obviously broken.
    VirtualShadowMapSettings settings{};
    settings.Clip0HalfExtent = 2.0f;
    settings.DepthRange = 4096.0f;

    const glm::vec3 lightDir = SunDirection();
    const glm::vec3 worldPoint(3.0f, 0.5f, -7.0f);

    std::array<VSM::ClipProjection, VSM::kClipLevels> before{};
    std::array<VSM::ClipProjection, VSM::kClipLevels> after{};
    std::array<glm::ivec2, VSM::kClipLevels> originsBefore{};
    std::array<glm::ivec2, VSM::kClipLevels> originsAfter{};

    VirtualShadowMap::BuildClipProjections(lightDir, glm::vec3(0.0f), settings, {}, true, before, originsBefore);
    // A move large enough to shift the near levels by many pages and the far ones
    // by none — both halves of the wrap have to hold.
    VirtualShadowMap::BuildClipProjections(lightDir, glm::vec3(17.3f, 2.0f, -9.1f), settings, originsBefore, false,
                                           after, originsAfter);

    u32 levelsChecked = 0;
    for (u32 level = 0; level < VSM::kClipLevels; ++level)
    {
        glm::ivec2 pageBefore{};
        glm::ivec2 pageAfter{};
        if (!VirtualShadowMap::WorldPointToWrappedPage(before[level], worldPoint, pageBefore))
            continue; // outside this level's frustum — nothing to compare
        if (!VirtualShadowMap::WorldPointToWrappedPage(after[level], worldPoint, pageAfter))
            continue;

        EXPECT_EQ(pageBefore, pageAfter)
            << "clip level " << level << ": a static world point changed page slot when the camera moved";
        ++levelsChecked;
    }
    EXPECT_GT(levelsChecked, 8u) << "the point fell outside almost every level — the test proved nothing";
}

TEST(VirtualShadowMap, PageOffsetAndDeltaDescribeTheSameMove)
{
    VirtualShadowMapSettings settings{};
    settings.Clip0HalfExtent = 2.0f;

    std::array<VSM::ClipProjection, VSM::kClipLevels> first{};
    std::array<VSM::ClipProjection, VSM::kClipLevels> second{};
    std::array<glm::ivec2, VSM::kClipLevels> originsFirst{};
    std::array<glm::ivec2, VSM::kClipLevels> originsSecond{};

    VirtualShadowMap::BuildClipProjections(SunDirection(), glm::vec3(0.0f), settings, {}, true, first, originsFirst);
    VirtualShadowMap::BuildClipProjections(SunDirection(), glm::vec3(5.0f, 0.0f, 3.0f), settings, originsFirst, false,
                                           second, originsSecond);

    constexpr i32 kRes = static_cast<i32>(VSM::kPageTableResolution);
    u32 saturatedLevels = 0;

    for (u32 level = 0; level < VSM::kClipLevels; ++level)
    {
        // PageDelta is SATURATED at +-kPageTableResolution, not the raw difference.
        // That is the contract, not a rounding artefact: the fine levels scroll far
        // more than a whole table on any real camera move (level 0's pages are
        // ~6 cm, so a 5 m step is 86 pages), and past one table width every slot
        // has changed owner anyway — so the shader's per-axis keep test resolves to
        // "free everything" at exactly +-kRes and clamping keeps it in i32 range.
        const glm::ivec2 rawDelta = originsSecond[level] - originsFirst[level];
        EXPECT_EQ(glm::clamp(rawDelta, glm::ivec2(-kRes), glm::ivec2(kRes)), second[level].PageDelta)
            << "level " << level << ": PageDelta is not the saturated origin difference";

        if (std::abs(rawDelta.x) >= kRes || std::abs(rawDelta.y) >= kRes)
        {
            ++saturatedLevels;
            // A saturated axis must make the shader's keep predicate false for
            // EVERY slot — otherwise a page that scrolled clean out of the frustum
            // would keep its cached texels under a new owner.
            const i32 d = second[level].PageDelta.x;
            if (std::abs(rawDelta.x) >= kRes)
            {
                for (i32 slot = 0; slot < kRes; ++slot)
                {
                    const i32 r = (slot - second[level].PrevPageOffset.x) & (kRes - 1);
                    const bool keep = (d >= 0) ? (r >= d) : (r < kRes + d);
                    EXPECT_FALSE(keep) << "level " << level << " slot " << slot
                                       << ": a saturated delta must free every slot";
                }
            }
        }

        // The offsets are the origins modulo the table resolution, and must be
        // NON-NEGATIVE: a negative offset (which C++'s % happily produces once the
        // camera crosses the render origin) indexes the page table out of bounds.
        EXPECT_GE(second[level].PageOffset.x, 0);
        EXPECT_GE(second[level].PageOffset.y, 0);
        EXPECT_LT(second[level].PageOffset.x, kRes);
        EXPECT_LT(second[level].PageOffset.y, kRes);
        EXPECT_EQ(first[level].PageOffset, second[level].PrevPageOffset);
    }

    // Both halves of the contract have to be exercised, or a change that always
    // saturated (or never did) would still pass.
    EXPECT_GT(saturatedLevels, 0u) << "no level scrolled past a whole table — saturation went untested";
    EXPECT_LT(saturatedLevels, VSM::kClipLevels)
        << "every level saturated — the exact-delta half of the contract went untested";
}

TEST(VirtualShadowMap, NegativeCameraCoordinatesStillProduceInBoundsPageOffsets)
{
    // The regression this pins: origins go negative as soon as the camera crosses
    // the render origin, and a `%` that keeps the sign turns the wrap into an
    // out-of-bounds read of the page table.
    VirtualShadowMapSettings settings{};
    settings.Clip0HalfExtent = 2.0f;

    std::array<VSM::ClipProjection, VSM::kClipLevels> clips{};
    std::array<glm::ivec2, VSM::kClipLevels> origins{};
    VirtualShadowMap::BuildClipProjections(SunDirection(), glm::vec3(-4213.7f, -80.0f, -9911.25f), settings, {}, true,
                                           clips, origins);

    for (u32 level = 0; level < VSM::kClipLevels; ++level)
    {
        EXPECT_GE(clips[level].PageOffset.x, 0) << "level " << level;
        EXPECT_GE(clips[level].PageOffset.y, 0) << "level " << level;
        EXPECT_LT(clips[level].PageOffset.x, static_cast<i32>(VSM::kPageTableResolution));
        EXPECT_LT(clips[level].PageOffset.y, static_cast<i32>(VSM::kPageTableResolution));
    }
}

// =============================================================================
// 5. The no-seam invariant — every clip level stores the same depth
// =============================================================================

TEST(VirtualShadowMap, AllClipLevelsAgreeOnTheDepthOfAWorldPoint)
{
    // "No visible seams at clip-level boundaries" is an acceptance criterion, and
    // this is the property that delivers it: the levels differ only in XY extent,
    // never in the world -> light-space-depth mapping. A fragment that switches
    // level between frames, or between neighbouring pixels, therefore compares
    // against the SAME stored number.
    //
    // It is also why VirtualShadowMapSettings::DepthRange is camera-independent:
    // a range that tracked the camera would rescale depth per frame and per level.
    VirtualShadowMapSettings settings{};
    settings.Clip0HalfExtent = 2.0f;
    settings.DepthRange = 4096.0f;

    std::array<VSM::ClipProjection, VSM::kClipLevels> clips{};
    std::array<glm::ivec2, VSM::kClipLevels> origins{};
    VirtualShadowMap::BuildClipProjections(SunDirection(), glm::vec3(12.0f, 3.0f, -4.0f), settings, {}, true, clips,
                                           origins);

    const std::array<glm::vec3, 4> probes{ {
        glm::vec3(12.5f, 1.0f, -4.5f),
        glm::vec3(20.0f, -3.0f, 10.0f),
        glm::vec3(-30.0f, 12.0f, -25.0f),
        glm::vec3(140.0f, 0.0f, 60.0f),
    } };

    for (const glm::vec3& probe : probes)
    {
        f32 reference = -1.0f;
        u32 levelsSeen = 0;
        for (u32 level = 0; level < VSM::kClipLevels; ++level)
        {
            const glm::vec4 clipPos = clips[level].ViewProjection * glm::vec4(probe, 1.0f);
            const f32 depth = (clipPos.z / clipPos.w) * 0.5f + 0.5f;
            if (depth < 0.0f || depth > 1.0f)
                continue;
            if (reference < 0.0f)
                reference = depth;
            else
            {
                EXPECT_NEAR(reference, depth, 1e-5f)
                    << "clip level " << level << " stores a different depth for the same world point — "
                    << "this is exactly what draws a seam at a clip-level boundary";
            }
            ++levelsSeen;
        }
        EXPECT_GT(levelsSeen, 4u) << "the probe was outside almost every level; the comparison proved nothing";
    }
}

TEST(VirtualShadowMap, EachClipLevelCoversTwiceTheExtentOfTheOneBelow)
{
    VirtualShadowMapSettings settings{};
    settings.Clip0HalfExtent = 3.0f;

    std::array<VSM::ClipProjection, VSM::kClipLevels> clips{};
    std::array<glm::ivec2, VSM::kClipLevels> origins{};
    VirtualShadowMap::BuildClipProjections(SunDirection(), glm::vec3(0.0f), settings, {}, true, clips, origins);

    EXPECT_NEAR(settings.Clip0HalfExtent, clips[0].HalfExtent, 1e-4f);
    for (u32 level = 1; level < VSM::kClipLevels; ++level)
    {
        EXPECT_NEAR(clips[level - 1].HalfExtent * 2.0f, clips[level].HalfExtent, clips[level].HalfExtent * 1e-5f)
            << "level " << level << " does not double the previous extent";
        EXPECT_NEAR(clips[level - 1].TexelWorldSize * 2.0f, clips[level].TexelWorldSize,
                    clips[level].TexelWorldSize * 1e-5f);
    }

    // A page must be exactly 1/kPageTableResolution of the level, or snapping the
    // frustum origin to a page multiple does not snap the page grid with it.
    for (u32 level = 0; level < VSM::kClipLevels; ++level)
    {
        const f32 pageWorldSize = clips[level].TexelWorldSize * static_cast<f32>(VSM::kPageSize);
        EXPECT_NEAR(clips[level].HalfExtent * 2.0f,
                    pageWorldSize * static_cast<f32>(VSM::kPageTableResolution),
                    clips[level].HalfExtent * 1e-4f);
    }
}

// =============================================================================
// 6. The wraparound free predicate
// =============================================================================

TEST(VirtualShadowMap, WrapFreePredicateReleasesExactlyTheSlotsThatChangedOwner)
{
    // The CPU twin of VSM_FreeWrappedPages.comp's per-axis test. A slot must be
    // freed iff the world page it holds changed — free too few and an incoming
    // page inherits the outgoing page's texels; free too many and the cache is
    // pointless.
    constexpr i32 kRes = static_cast<i32>(VSM::kPageTableResolution);

    for (i32 originOld : { -3 * kRes - 5, -7, 0, 11, 5 * kRes + 2 })
    {
        for (i32 delta : { -kRes - 1, -kRes, -kRes + 1, -9, -1, 0, 1, 9, kRes - 1, kRes, kRes + 1 })
        {
            const i32 originNew = originOld + delta;
            for (i32 slot = 0; slot < kRes; ++slot)
            {
                // Ground truth: which world page does this slot hold, before and after?
                const auto worldPage = [&](i32 origin)
                {
                    i32 rem = (slot - origin) % kRes;
                    if (rem < 0)
                        rem += kRes;
                    return origin + rem;
                };
                const bool changedOwner = worldPage(originOld) != worldPage(originNew);

                // The shader's formulation, using only the wrapped previous offset
                // and the (saturated) delta — which is all it has.
                i32 prevOffset = originOld % kRes;
                if (prevOffset < 0)
                    prevOffset += kRes;
                const i32 saturated = std::clamp(delta, -kRes, kRes);
                const i32 r = (slot - prevOffset) & (kRes - 1);
                const bool keep = (saturated >= 0) ? (r >= saturated) : (r < kRes + saturated);

                EXPECT_EQ(changedOwner, !keep)
                    << "originOld=" << originOld << " delta=" << delta << " slot=" << slot;
            }
        }
    }
}

// =============================================================================
// 7. Settings sanitisation
// =============================================================================

TEST(VirtualShadowMap, PhysicalResolutionIsClampedToAWholeNumberOfPages)
{
    // A pool that is not a page multiple leaves a partial page at the edge, and
    // the allocator has no way to express "half a page" — it would hand out a
    // physical page whose texels run off the end of the texture.
    for (const u32 requested : { 0u, 100u, 1023u, 1024u, 1500u, 2048u, 4096u, 8192u, 100000u })
    {
        const u32 sanitized = VirtualShadowMap::SanitizeResolution(requested);
        EXPECT_EQ(0u, sanitized % VSM::kPageSize) << "requested " << requested;
        EXPECT_GE(sanitized, 1024u);
        EXPECT_LE(sanitized, 8192u);
    }

    // The default is what the acceptance criterion's VRAM comparison is quoted
    // against, so a silent change to it must show up here.
    const VirtualShadowMapSettings defaults{};
    EXPECT_FALSE(defaults.Enabled) << "VSM must stay opt-in: it does not yet cover terrain / foliage / "
                                      "voxel / virtual-geometry casters";

    // The two projection flavours must be SEPARATE members. Collapsing them to
    // one would work on GL (where the backend adjustment is identity) and flip
    // every page on Vulkan — the archetypal seam bug this layout prevents.
    EXPECT_NE(offsetof(VSM::ClipProjection, ViewProjection),
              offsetof(VSM::ClipProjection, ViewProjectionRaster));
    EXPECT_EQ(0u, offsetof(VSM::ClipProjection, ViewProjection));
    EXPECT_EQ(64u, offsetof(VSM::ClipProjection, ViewProjectionRaster));
    EXPECT_EQ(4096u, defaults.PhysicalResolution);
    EXPECT_GT(defaults.DepthRange, 0.0f);
    EXPECT_GT(defaults.Clip0HalfExtent, 0.0f);
}

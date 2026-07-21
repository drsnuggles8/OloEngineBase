#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// OLO_TEST_LAYER: unit

// Unit tests for the pure shaping + compare math behind olo_render_validate
// (issue #607): base-name grouping of versioned resources, the consumed-but-
// unbacked flag, the bit-exact float-buffer compare (the "HZB mip0 == scene
// depth bitwise" instrument), and the reply JSON shape. The GL readbacks and
// the live graph sweep live in McpToolsRender.cpp's handler (deliberately NOT
// compiled here).
#include "MCP/McpRenderValidate.h"

#include <bit>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
    using namespace OloEngine;
    using namespace OloEngine::MCP::RenderValidate;
} // namespace

TEST(McpRenderValidate, BaseNameStripsVersionQualifier)
{
    EXPECT_EQ("SceneColor", BaseName("SceneColor@ParticlePass"));
    EXPECT_EQ("SceneColor", BaseName("SceneColor"));
    EXPECT_EQ("", BaseName("@LeadingAt"));
}

TEST(McpRenderValidate, ConsumedButUnbackedFlag)
{
    ResourceIdentity consumedUnbacked;
    consumedUnbacked.HasConsumers = true;
    EXPECT_TRUE(IsUnbackedConsumed(consumedUnbacked));

    ResourceIdentity consumedBacked = consumedUnbacked;
    consumedBacked.GLTextureId = 5;
    EXPECT_FALSE(IsUnbackedConsumed(consumedBacked));

    ResourceIdentity unconsumed;
    unconsumed.HasConsumers = false;
    EXPECT_FALSE(IsUnbackedConsumed(unconsumed)); // nobody reads it — not a hazard
}

TEST(McpRenderValidate, VersionGroupsReportDistinctPhysicalIds)
{
    std::vector<ResourceIdentity> identities;
    ResourceIdentity base;
    base.Name = "SceneColor";
    base.GLTextureId = 10;
    base.LastWriter = "Lighting";
    identities.push_back(base);
    ResourceIdentity version;
    version.Name = "SceneColor@ParticlePass";
    version.GLTextureId = 11; // copy-on-write: a DIFFERENT physical texture
    version.LastWriter = "ParticlePass";
    identities.push_back(version);
    ResourceIdentity lone;
    lone.Name = "SceneDepth"; // single version — no group emitted
    lone.GLTextureId = 12;
    identities.push_back(lone);

    const Json groups = VersionGroupsJson(identities);
    ASSERT_EQ(1u, groups.size());
    EXPECT_EQ("SceneColor", groups[0]["baseName"].get<std::string>());
    EXPECT_TRUE(groups[0]["multiplePhysicalIds"].get<bool>());
    ASSERT_EQ(2u, groups[0]["versions"].size());
    EXPECT_EQ("Lighting", groups[0]["versions"][0]["lastWriter"].get<std::string>());
}

TEST(McpRenderValidate, VersionGroupSharedPhysicalIdIsNotFlagged)
{
    std::vector<ResourceIdentity> identities;
    for (const char* name : { "SceneDepth", "SceneDepth@ParticlePass" })
    {
        ResourceIdentity identity;
        identity.Name = name;
        identity.GLTextureId = 33; // SSA versions aliasing ONE physical texture
        identities.push_back(identity);
    }
    const Json groups = VersionGroupsJson(identities);
    ASSERT_EQ(1u, groups.size());
    EXPECT_FALSE(groups[0]["multiplePhysicalIds"].get<bool>());
}

TEST(McpRenderValidate, CompareIdenticalBuffersIsBitwiseEqual)
{
    const std::vector<f32> a{ 0.25f, 0.5f, 0.75f, 1.0f };
    const CompareResult result = CompareFloatBuffers(a, 2, 2, a, 2, 2);

    EXPECT_TRUE(result.Error.empty());
    EXPECT_TRUE(result.BitwiseEqual);
    EXPECT_EQ(4u, result.ComparedTexels);
    EXPECT_EQ(0u, result.DifferingTexels);
}

TEST(McpRenderValidate, CompareFindsOneUlpDifference)
{
    // The exact corruption class the tool exists for: a value one ULP off.
    std::vector<f32> a{ 1.0f, 1.0f, 1.0f, 1.0f };
    std::vector<f32> b = a;
    b[2] = std::nextafter(1.0f, 0.0f);
    const CompareResult result = CompareFloatBuffers(a, 2, 2, b, 2, 2);

    EXPECT_FALSE(result.BitwiseEqual);
    EXPECT_EQ(1u, result.DifferingTexels);
    ASSERT_EQ(1u, result.FirstDiffs.size());
    EXPECT_EQ(0u, result.FirstDiffs[0].X); // row-major: index 2 = (0, 1)
    EXPECT_EQ(1u, result.FirstDiffs[0].Y);
    EXPECT_GT(result.MaxAbsDiff, 0.0);
    EXPECT_LT(result.MaxAbsDiff, 1.0e-6);
}

TEST(McpRenderValidate, CompareOverlapsDifferentlySizedBuffers)
{
    // A 3x2 vs 2x2: only the overlapping top-left 2x2 is compared, with each
    // buffer indexed by its OWN row stride.
    const std::vector<f32> a{ 1.0f, 2.0f, 9.0f,
                              3.0f, 4.0f, 9.0f };
    const std::vector<f32> b{ 1.0f, 2.0f,
                              3.0f, 4.0f };
    const CompareResult result = CompareFloatBuffers(a, 3, 2, b, 2, 2);

    EXPECT_EQ(2u, result.Width);
    EXPECT_EQ(2u, result.Height);
    EXPECT_TRUE(result.BitwiseEqual);
}

TEST(McpRenderValidate, CompareIdenticalNaNsAreEqualDifferingNaNIsNot)
{
    const f32 quietNaN = std::numeric_limits<f32>::quiet_NaN();
    const std::vector<f32> a{ quietNaN, 1.0f };
    const std::vector<f32> sameNaN{ quietNaN, 1.0f };
    EXPECT_TRUE(CompareFloatBuffers(a, 2, 1, sameNaN, 2, 1).BitwiseEqual);

    const std::vector<f32> noNaN{ 1.0f, 1.0f };
    const CompareResult result = CompareFloatBuffers(a, 2, 1, noNaN, 2, 1);
    EXPECT_FALSE(result.BitwiseEqual);
    EXPECT_EQ(1u, result.DifferingTexels);
    // A NaN-vs-finite diff must not poison MaxAbsDiff.
    EXPECT_DOUBLE_EQ(0.0, result.MaxAbsDiff);
}

TEST(McpRenderValidate, BuildValidateJsonOkOnlyWhenClean)
{
    const Json clean = BuildValidateJson({}, {}, {}, {}, {});
    EXPECT_TRUE(clean["ok"].get<bool>());
    EXPECT_EQ(0u, clean["hazardCount"].get<u32>());

    std::vector<HazardInfo> hazards;
    hazards.push_back(HazardInfo{ "ReadAfterWrite", "SceneDepth", "GTAOPass", "ParticlePass", "reader precedes writer" });
    const Json dirty = BuildValidateJson(hazards, {}, {}, {}, {});
    EXPECT_FALSE(dirty["ok"].get<bool>());
    EXPECT_EQ(1u, dirty["hazardCount"].get<u32>());
    EXPECT_EQ("ReadAfterWrite", dirty["hazards"][0]["kind"].get<std::string>());

    std::vector<ResourceIdentity> identities;
    ResourceIdentity unbacked;
    unbacked.Name = "GhostBuffer";
    unbacked.HasConsumers = true;
    identities.push_back(unbacked);
    const Json ghost = BuildValidateJson({}, {}, {}, {}, identities);
    EXPECT_FALSE(ghost["ok"].get<bool>());
    ASSERT_EQ(1u, ghost["consumedButUnbacked"].size());
    EXPECT_EQ("GhostBuffer", ghost["consumedButUnbacked"][0].get<std::string>());
}

TEST(McpRenderValidate, CompareResultJsonShape)
{
    CompareRequest request;
    request.A = "SceneDepth";
    request.B = "HZB";
    request.MipB = 0;
    request.AfterPass = "GTAOPass";

    std::vector<f32> a{ 0.5f };
    std::vector<f32> b{ 0.25f };
    CompareResult result = CompareFloatBuffers(a, 1, 1, b, 1, 1);
    result.FormatA = "DEPTH24_STENCIL8";
    result.FormatB = "R32F";

    const Json j = CompareResultJson(request, result);
    EXPECT_EQ("SceneDepth", j["a"]["name"].get<std::string>());
    EXPECT_EQ("HZB", j["b"]["name"].get<std::string>());
    EXPECT_EQ("GTAOPass", j["afterPass"].get<std::string>());
    EXPECT_FALSE(j["bitwiseEqual"].get<bool>());
    EXPECT_EQ(1u, j["differingTexels"].get<u64>());
    ASSERT_EQ(1u, j["firstDiffs"].size());
    EXPECT_DOUBLE_EQ(0.25, j["maxAbsDiff"].get<f64>());
    EXPECT_EQ(std::bit_cast<u32>(0.5f), j["firstDiffs"][0]["aBits"].get<u32>());
}

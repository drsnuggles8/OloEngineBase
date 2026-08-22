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
    consumedBacked.NativeTextureHandle = 5;
    EXPECT_FALSE(IsUnbackedConsumed(consumedBacked));

    ResourceIdentity unconsumed;
    unconsumed.HasConsumers = false;
    EXPECT_FALSE(IsUnbackedConsumed(unconsumed)); // nobody reads it — not a hazard
}

// The #890 regression, in the exact shape the live Vulkan editor produced it.
//
// A framebuffer-backed resource under Vulkan reports native handle 0 for every
// attachment — VulkanFramebuffer::GetColorAttachmentRendererID returns 0 by
// design, "no GL name exists here". The old predicate read only that number, so
// `olo_render_validate` returned a false `ok: false` naming 11 resources that
// olo_render_capture_target read real HDR scene content out of in the same
// session. Whatever else changes, a resource carrying an identity and real
// storage must never be called unbacked.
TEST(McpRenderValidate, VulkanShapedResourceWithNoNativeHandleIsNotUnbacked)
{
    ResourceIdentity sceneColor;
    sceneColor.Name = "SceneColor";
    sceneColor.HasConsumers = true;
    sceneColor.NativeTextureHandle = 0; // the Vulkan framebuffer attachment
    sceneColor.TextureIdentity = 0x0000000100000007ull;
    sceneColor.TextureHasStorage = true;

    EXPECT_TRUE(HasTextureBacking(sceneColor));
    EXPECT_FALSE(IsUnbackedConsumed(sceneColor))
        << "a native handle of 0 is legitimate on Vulkan and must never be read as 'unbacked'";
}

// The mirror image, and the one that a green test suite let through once
// already: a resource can carry a perfectly VALID identity and still have no
// storage this frame (a transient the planner never allocated). The identity
// means the backend could be ASKED — it is not itself an answer, and it must
// never override a negative storage query.
//
// Caught live: after the first cut of this fix, Vulkan reported `ok: true`
// with an empty list while `olo_render_target_stats` still answered "has no
// storage at mip 0" for GTAOEdge. The unit test passed because it encoded the
// assumption (identity == 0) instead of the real shape.
TEST(McpRenderValidate, AValidIdentityDoesNotOverrideANegativeStorageAnswer)
{
    ResourceIdentity gtaoEdge;
    gtaoEdge.Name = "GTAOEdge";
    gtaoEdge.HasConsumers = true;
    gtaoEdge.TextureIdentity = 0x000000010000003Cull; // a live handle...
    gtaoEdge.TextureHasStorage = false;               // ...with nothing behind it
    gtaoEdge.NativeTextureHandle = 60;                // and a recycled native name

    EXPECT_FALSE(HasTextureBacking(gtaoEdge));
    EXPECT_TRUE(IsUnbackedConsumed(gtaoEdge))
        << "the storage query is final in BOTH directions; neither a live identity nor a "
           "recycled native name may talk it out of a negative answer";
}

// The native handle is consulted ONLY when there is no identity to ask about
// — a resource imported as a bare native id. There it can confirm backing,
// and it still can never deny it.
TEST(McpRenderValidate, NativeHandleOnlyAnswersWhenThereIsNoIdentity)
{
    ResourceIdentity nativeImport;
    nativeImport.HasConsumers = true;
    nativeImport.TextureIdentity = 0; // nothing to interrogate
    nativeImport.TextureHasStorage = false;
    nativeImport.NativeTextureHandle = 42;
    EXPECT_TRUE(HasTextureBacking(nativeImport));
    EXPECT_FALSE(IsUnbackedConsumed(nativeImport));

    ResourceIdentity nothing;
    nothing.HasConsumers = true;
    EXPECT_FALSE(HasTextureBacking(nothing));
    EXPECT_TRUE(IsUnbackedConsumed(nothing));
}

// The other half of the same live result, and the reason the fix is a storage
// query rather than "the identity is non-null". Two of the thirteen resources
// the tool flagged on Vulkan — GTAOEdge and GTAODenoisePong — were TRUE
// positives: olo_render_target_stats answered "has no storage at mip 0" for
// both. A fix that only stopped trusting the native id would have turned a
// noisy tool into a silent one.
TEST(McpRenderValidate, GenuinelyUnbackedConsumedResourceIsStillFlagged)
{
    ResourceIdentity gtaoEdge;
    gtaoEdge.Name = "GTAOEdge";
    gtaoEdge.HasConsumers = true;
    gtaoEdge.NativeTextureHandle = 0;
    gtaoEdge.TextureIdentity = 0; // nothing resolved, in either currency
    gtaoEdge.TextureHasStorage = false;

    EXPECT_FALSE(HasTextureBacking(gtaoEdge));
    EXPECT_TRUE(IsUnbackedConsumed(gtaoEdge));
}

// A buffer-backed resource has the same asymmetry: every Vulkan buffer class
// answers 0 to GetRendererID(), so the identity is the only currency that can
// say "there is an object here".
TEST(McpRenderValidate, BufferBackingIsDecidedFromTheIdentityToo)
{
    ResourceIdentity buffer;
    buffer.Name = "ClusterLightGrid";
    buffer.HasConsumers = true;
    buffer.NativeBufferHandle = 0;
    buffer.BufferIdentity = 0x0000000200000011ull;

    EXPECT_TRUE(HasBufferBacking(buffer));
    EXPECT_FALSE(IsUnbackedConsumed(buffer));
}

// PhysicalKey prefers the identity, so two versions that share one object are
// recognised as sharing it even when neither carries a native handle. Keying on
// the native value instead collapsed every Vulkan framebuffer attachment onto
// 0 — which reads as "they all share one texture" rather than "we cannot tell".
TEST(McpRenderValidate, PhysicalKeyPrefersIdentityOverNativeHandle)
{
    ResourceIdentity a;
    a.TextureIdentity = 0x0000000100000007ull;
    a.NativeTextureHandle = 999;
    EXPECT_EQ(0x0000000100000007ull, PhysicalKey(a));

    ResourceIdentity nativeOnly; // imported as a bare native id: no identity to give
    nativeOnly.NativeTextureHandle = 42;
    EXPECT_EQ(42ull, PhysicalKey(nativeOnly));

    ResourceIdentity unbacked;
    EXPECT_EQ(0ull, PhysicalKey(unbacked)) << "unbacked is not a physical object everything shares";
}

// The token spelling matches the engine's own fmt formatter for RHI::Handle,
// so a value copied out of an MCP reply matches what a log line prints.
TEST(McpRenderValidate, IdentityTokenMatchesTheEngineSpelling)
{
    EXPECT_EQ("#7:1", OloEngine::MCP::IdentityToken(0x0000000100000007ull));
    EXPECT_EQ("", OloEngine::MCP::IdentityToken(0)) << "0 is 'no identity', not '#0:0'";
    EXPECT_EQ("0x1F", OloEngine::MCP::NativeHandleHex(31));
    // The value that used to be truncated: a 64-bit handle survives intact.
    EXPECT_EQ("0x1D2C3B4A5F6E7", OloEngine::MCP::NativeHandleHex(0x1D2C3B4A5F6E7ull));
}

TEST(McpRenderValidate, VersionGroupsReportDistinctPhysicalIds)
{
    std::vector<ResourceIdentity> identities;
    ResourceIdentity base;
    base.Name = "SceneColor";
    base.TextureIdentity = 10;
    base.LastWriter = "Lighting";
    identities.push_back(base);
    ResourceIdentity version;
    version.Name = "SceneColor@ParticlePass";
    version.TextureIdentity = 11; // copy-on-write: a DIFFERENT physical resource
    version.LastWriter = "ParticlePass";
    identities.push_back(version);
    ResourceIdentity lone;
    lone.Name = "SceneDepth"; // single version — no group emitted
    lone.TextureIdentity = 12;
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
        identity.TextureIdentity = 33; // SSA versions aliasing ONE physical texture
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

// OLO_TEST_LAYER: unit
//
// #1345 — the device-free half of PSO precaching:
//
//   1. PipelineRequestLedger: every draw-time miss is classified by the state
//      it found (never requested / requested too late, queued or in flight /
//      retried after a failure), a prepared first use is the only outcome with
//      no synchronous creation, failures and stale (invalidated) results are
//      counted apart, and a never-requested miss on a shader declared ready is
//      a ReadyClaimViolation rather than an ordinary miss.
//   2. PipelineManifest: the record of complete keys survives a round trip,
//      keeps a shader's classic and mesh-stage identities apart, puts the
//      selected configuration's entries first, and refuses a malformed file
//      without refusing a well-formed file's valid entries.
//   3. VulkanPipelineBuilder::DeriveGraphicsKey (compiled with Vulkan, no device
//      needed): incompatible configurations never derive the same key —
//      sample count, attachment formats, depth format, attachment count,
//      tessellation patch size, root-data layout, shader identity (a skinned or
//      material variant is its own shader) and baked blend each separate two
//      keys, while the render resolution is deliberately not an input.
//
// The device half — real pipelines through the same paths — is in
// VulkanShaderPipelineTest.cpp.

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/PipelineManifest.h"
#include "OloEngine/Renderer/PipelineRequestLedger.h"

#if OLO_WITH_VULKAN
#include "Platform/Vulkan/VulkanPipelineBuilder.h"
#endif

#include <gtest/gtest.h>

#include <functional>
#include <span>
#include <string>

using namespace OloEngine;

namespace
{
    struct IntHash
    {
        sizet operator()(const int key) const
        {
            return std::hash<int>{}(key);
        }
    };
    using Ledger = PipelineRequestLedger<int, IntHash, std::string>;

    constexpr u64 kShaderA = 11;
    constexpr u64 kShaderB = 22;
} // namespace

// -----------------------------------------------------------------------------
// 1. The ledger
// -----------------------------------------------------------------------------

TEST(PipelineRequestLedger, APreparedRequestIsTheOnlyFirstUseWithoutASynchronousCreation)
{
    Ledger ledger;
    ASSERT_TRUE(ledger.Request(1, kShaderA, "inputs"));
    const auto job = ledger.TakeNextQueued();
    ASSERT_TRUE(job.has_value());
    EXPECT_EQ(job->RequestKey, 1);
    EXPECT_EQ(job->Data, "inputs") << "The worker gets the snapshot the request carried";
    EXPECT_EQ(ledger.GetState(1), Ledger::State::InFlight);

    EXPECT_TRUE(ledger.Complete(1, job->Ticket, /*success=*/true, /*blocking=*/false, 12.5,
                                PipelineDriverCacheResult::Miss));
    EXPECT_EQ(ledger.GetState(1), Ledger::State::Ready);
    EXPECT_TRUE(ledger.NoteFirstUse(1));

    const auto& c = ledger.GetCounters();
    EXPECT_EQ(c.PreparedFirstUses, 1u);
    EXPECT_EQ(c.Misses(), 0u);
    EXPECT_EQ(c.PreparedCreations, 1u);
    EXPECT_EQ(c.BlockingCreations, 0u);
    EXPECT_DOUBLE_EQ(c.PreparedCreateMs, 12.5);
    EXPECT_DOUBLE_EQ(c.BlockingCreateMs, 0.0) << "Worker time is not draw-thread time";
    EXPECT_EQ(c.DriverCacheMisses, 1u);
}

TEST(PipelineRequestLedger, MissesAreClassifiedByTheStateTheDrawFound)
{
    Ledger ledger;

    // Never requested: the draw creates it, and its first use is not a hit.
    auto miss = ledger.ClaimMiss(1, kShaderA);
    EXPECT_EQ(miss.Action, Ledger::MissAction::CreateNow);
    EXPECT_EQ(miss.Kind, PipelineMissKind::NeverRequested);
    EXPECT_FALSE(miss.ReadyClaimViolation);
    EXPECT_TRUE(ledger.Complete(1, miss.Ticket, true, /*blocking=*/true, 30.0, PipelineDriverCacheResult::Hit));
    EXPECT_FALSE(ledger.NoteFirstUse(1)) << "A pipeline the draw created is not a prepared hit";

    // Requested, still queued: the draw takes the request over.
    ASSERT_TRUE(ledger.Request(2, kShaderA, "queued"));
    miss = ledger.ClaimMiss(2, kShaderA);
    EXPECT_EQ(miss.Action, Ledger::MissAction::CreateNow);
    EXPECT_EQ(miss.Kind, PipelineMissKind::TooLateQueued);
    EXPECT_FALSE(ledger.TakeNextQueued().has_value()) << "A taken-over request must not be created twice";
    EXPECT_TRUE(ledger.Complete(2, miss.Ticket, true, true, 10.0));

    // Requested, in flight: the draw waits for the worker instead of compiling.
    ASSERT_TRUE(ledger.Request(3, kShaderA, "flying"));
    const auto flying = ledger.TakeNextQueued();
    ASSERT_TRUE(flying.has_value());
    miss = ledger.ClaimMiss(3, kShaderA);
    EXPECT_EQ(miss.Action, Ledger::MissAction::Wait);
    EXPECT_EQ(miss.Kind, PipelineMissKind::TooLateInFlight);
    ledger.NoteBlockingWait(4.0);
    EXPECT_TRUE(ledger.Complete(3, flying->Ticket, true, /*blocking=*/false, 9.0));

    const auto& c = ledger.GetCounters();
    EXPECT_EQ(c.NeverRequested, 1u);
    EXPECT_EQ(c.TooLateQueued, 1u);
    EXPECT_EQ(c.TooLateInFlight, 1u);
    EXPECT_EQ(c.Misses(), 3u);
    EXPECT_EQ(c.BlockingCreations, 2u);
    EXPECT_EQ(c.PreparedCreations, 1u);
    EXPECT_DOUBLE_EQ(c.BlockingCreateMs, 30.0 + 10.0 + 4.0) << "A wait is blocking time too";
    EXPECT_DOUBLE_EQ(c.BlockingCreateMaxMs, 30.0);
    EXPECT_EQ(c.DriverCacheHits, 1u);
    EXPECT_EQ(c.DriverCacheUnknown, 2u);
}

TEST(PipelineRequestLedger, ADrawThatWaitedAndThenHasToCreateIsStillOneMiss)
{
    Ledger ledger;
    ASSERT_TRUE(ledger.Request(5, kShaderA, "flying"));
    const auto flying = ledger.TakeNextQueued();
    ASSERT_TRUE(flying.has_value());
    EXPECT_EQ(ledger.ClaimMiss(5, kShaderA).Action, Ledger::MissAction::Wait);
    // The worker's creation fails while the draw waits; the draw asks again
    // after the wait, without counting a second miss.
    EXPECT_TRUE(ledger.Complete(5, flying->Ticket, /*success=*/false, false, 3.0));
    const auto retry = ledger.ClaimMiss(5, kShaderA, /*countMiss=*/false);
    EXPECT_EQ(retry.Action, Ledger::MissAction::CreateNow);
    EXPECT_TRUE(ledger.Complete(5, retry.Ticket, true, true, 6.0));

    const auto& c = ledger.GetCounters();
    EXPECT_EQ(c.Misses(), 1u) << "One hitching draw is one miss";
    EXPECT_EQ(c.TooLateInFlight, 1u);
    EXPECT_EQ(c.RetriedAfterFailure, 0u);
    EXPECT_EQ(c.FailedCreations, 1u);
}

TEST(PipelineRequestLedger, AFailedCreationIsCountedAndTheDrawRetriesIt)
{
    Ledger ledger;
    ASSERT_TRUE(ledger.Request(7, kShaderA, "x"));
    const auto job = ledger.TakeNextQueued();
    ASSERT_TRUE(job.has_value());
    EXPECT_TRUE(ledger.Complete(7, job->Ticket, /*success=*/false, /*blocking=*/false, 1.0));
    EXPECT_EQ(ledger.GetState(7), Ledger::State::Failed);
    EXPECT_EQ(ledger.GetCounters().FailedCreations, 1u);

    const auto miss = ledger.ClaimMiss(7, kShaderA);
    EXPECT_EQ(miss.Action, Ledger::MissAction::CreateNow);
    EXPECT_EQ(miss.Kind, PipelineMissKind::RetriedAfterFailure);
    EXPECT_TRUE(ledger.Complete(7, miss.Ticket, true, true, 2.0));
    EXPECT_EQ(ledger.GetCounters().RetriedAfterFailure, 1u);
    EXPECT_EQ(ledger.GetState(7), Ledger::State::Ready);

    // A failed key may be requested again; a ready one is a duplicate.
    EXPECT_FALSE(ledger.Request(7, kShaderA, "again"));
    EXPECT_EQ(ledger.GetCounters().DuplicateRequests, 1u);
}

TEST(PipelineRequestLedger, ANeverRequestedMissOnAShaderDeclaredReadyIsAClaimViolation)
{
    Ledger ledger;
    ledger.DeclareShaderReady(kShaderA);
    const auto onClaimed = ledger.ClaimMiss(1, kShaderA);
    EXPECT_TRUE(onClaimed.ReadyClaimViolation);
    const auto onUnclaimed = ledger.ClaimMiss(2, kShaderB);
    EXPECT_FALSE(onUnclaimed.ReadyClaimViolation);
    EXPECT_EQ(ledger.GetCounters().NeverRequested, 2u);
    EXPECT_EQ(ledger.GetCounters().ReadyClaimViolations, 1u);

    // Invalidation withdraws the claim with the entries.
    (void)ledger.InvalidateShader(kShaderA);
    EXPECT_FALSE(ledger.IsShaderDeclaredReady(kShaderA));
}

TEST(PipelineRequestLedger, InvalidationReturnsTheKeysToRequestAgainAndDiscardsWhatWasInFlight)
{
    Ledger ledger;
    ASSERT_TRUE(ledger.Request(1, kShaderA, "ready"));
    const auto first = ledger.TakeNextQueued();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(ledger.Complete(1, first->Ticket, true, false, 1.0));
    ASSERT_TRUE(ledger.Request(2, kShaderA, "flying"));
    ASSERT_TRUE(ledger.Request(3, kShaderA, "queued"));
    ASSERT_TRUE(ledger.Request(4, kShaderB, "other shader"));
    const auto inFlight = ledger.TakeNextQueued();
    ASSERT_TRUE(inFlight.has_value());
    ASSERT_EQ(inFlight->RequestKey, 2);

    const TArray<int> invalidated = ledger.InvalidateShader(kShaderA);
    EXPECT_EQ(invalidated.Num(), 2) << "The ready and the queued key are re-requested; the in-flight one is discarded";
    EXPECT_TRUE(invalidated.Contains(1));
    EXPECT_TRUE(invalidated.Contains(3));

    EXPECT_FALSE(ledger.Complete(2, inFlight->Ticket, true, false, 5.0))
        << "A result built from the old modules must be discarded";
    EXPECT_EQ(ledger.GetCounters().DiscardedStale, 1u);
    EXPECT_FALSE(ledger.GetState(2).has_value());

    const auto next = ledger.TakeNextQueued();
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->RequestKey, 4) << "Only the other shader's request is still queued";
}

TEST(PipelineRequestLedger, AnOldCreationCannotPublishOverTheClaimThatReplacedIt)
{
    // A worker compiles from the old modules; a reload invalidates the key and,
    // before the worker returns, a draw claims the key again for the new
    // modules. The worker's late result must be discarded, and the draw's
    // accepted, whichever finishes first.
    Ledger ledger;
    ASSERT_TRUE(ledger.Request(9, kShaderA, "old modules"));
    const auto worker = ledger.TakeNextQueued();
    ASSERT_TRUE(worker.has_value());
    (void)ledger.InvalidateShader(kShaderA);
    const auto draw = ledger.ClaimMiss(9, kShaderA);
    ASSERT_EQ(draw.Action, Ledger::MissAction::CreateNow);

    EXPECT_FALSE(ledger.Complete(9, worker->Ticket, true, false, 8.0)) << "The pre-reload pipeline was published";
    EXPECT_EQ(ledger.GetState(9), Ledger::State::InFlight) << "The draw's claim must survive the stale result";
    EXPECT_TRUE(ledger.Complete(9, draw.Ticket, true, true, 7.0));
    EXPECT_EQ(ledger.GetState(9), Ledger::State::Ready);
    EXPECT_EQ(ledger.GetCounters().DiscardedStale, 1u);
}

TEST(PipelineRequestLedger, TheSelectedConfigurationIsPreparedFirstAndPromotesASpeculativeRequestOnce)
{
    Ledger ledger;
    ASSERT_TRUE(ledger.Request(1, kShaderA, "msaa4", PipelineRequestPriority::Speculative));
    ASSERT_TRUE(ledger.Request(2, kShaderA, "msaa1", PipelineRequestPriority::Selected));
    EXPECT_EQ(ledger.TakeNextQueued()->RequestKey, 2);

    // The configuration switched: key 1 is now selected and jumps the queue
    // ahead of a speculative request made before it. Asking again (the
    // renderer re-requests on every switch) does not queue it again.
    ASSERT_TRUE(ledger.Request(3, kShaderB, "other", PipelineRequestPriority::Speculative));
    EXPECT_FALSE(ledger.Request(1, kShaderA, "msaa4", PipelineRequestPriority::Selected));
    EXPECT_FALSE(ledger.Request(1, kShaderA, "msaa4", PipelineRequestPriority::Selected));
    EXPECT_FALSE(ledger.Request(1, kShaderA, "msaa4", PipelineRequestPriority::Selected));
    EXPECT_EQ(ledger.TakeNextQueued()->RequestKey, 1);
    EXPECT_EQ(ledger.TakeNextQueued()->RequestKey, 3);
    EXPECT_FALSE(ledger.TakeNextQueued().has_value()) << "The promoted key's old slot is skipped, not created twice";
}

// -----------------------------------------------------------------------------
// 2. The manifest
// -----------------------------------------------------------------------------

namespace
{
    PipelineSignature Signature(const u32 colorCount, const u32 format, const u32 depth, const u32 samples,
                                const u32 patch = 0)
    {
        PipelineSignature signature;
        signature.ColorCount = colorCount;
        for (u32 i = 0; i < colorCount; ++i)
        {
            signature.ColorFormats[i] = format + i;
        }
        signature.DepthFormat = depth;
        signature.Samples = samples;
        signature.PatchControlPoints = patch;
        return signature;
    }

    constexpr u32 kClassicStages = 0x01 | 0x10; // vertex | fragment
    constexpr u32 kMeshStages = 0x80 | 0x10;    // mesh | fragment
} // namespace

TEST(PipelineManifest, RecordsCompleteKeysPerShaderIdentityAndConfiguration)
{
    PipelineManifest manifest;
    manifest.BeginSession();
    const auto gbuffer = Signature(5, 97, 126, 1);
    const auto gbufferMsaa = Signature(5, 97, 126, 4);
    EXPECT_TRUE(manifest.Record("PBR_GBuffer", kClassicStages, gbuffer, "Deferred/msaa1/native/oit1"));
    EXPECT_FALSE(manifest.Record("PBR_GBuffer", kClassicStages, gbuffer, "Deferred/msaa1/native/oit1"))
        << "A key already recorded under this configuration changes nothing";
    EXPECT_TRUE(manifest.Record("PBR_GBuffer", kClassicStages, gbuffer, "Deferred/msaa1/upscaled/oit1"));
    EXPECT_TRUE(manifest.Record("PBR_GBuffer", kClassicStages, gbufferMsaa, "Deferred/msaa4/native/oit1"));
    EXPECT_EQ(manifest.GetEntryCount(), 2u);

    // The selected configuration's entries come first.
    const auto msaaFirst = manifest.EntriesFor("PBR_GBuffer", kClassicStages, "Deferred/msaa4/native/oit1");
    ASSERT_EQ(msaaFirst.Num(), 2);
    EXPECT_EQ(msaaFirst[0]->Signature.Samples, 4u);
    const auto plainFirst = manifest.EntriesFor("PBR_GBuffer", kClassicStages, "Deferred/msaa1/native/oit1");
    EXPECT_EQ(plainFirst[0]->Signature.Samples, 1u);
    EXPECT_EQ(plainFirst[0]->Configurations.Num(), 2);
}

TEST(PipelineManifest, TheEmptyTagIsAConfigurationAndSurvivesARoundTrip)
{
    // A session that never publishes a tag (2D only) draws under the empty
    // one; its entries must be selectable under it next session.
    PipelineManifest manifest;
    manifest.BeginSession();
    EXPECT_TRUE(manifest.Record("Renderer2D_Quad", kClassicStages, Signature(1, 37, 0, 1), ""));
    EXPECT_FALSE(manifest.Record("Renderer2D_Quad", kClassicStages, Signature(1, 37, 0, 1), ""))
        << "Recording the same key under the same tag again changes nothing";

    const auto parsed = PipelineManifest::Parse(manifest.Serialize());
    ASSERT_TRUE(parsed.has_value());
    const auto entries = parsed->EntriesFor("Renderer2D_Quad", kClassicStages, "");
    ASSERT_EQ(entries.Num(), 1);
    EXPECT_TRUE(entries[0]->SeenUnder(""));
    EXPECT_FALSE(entries[0]->SeenUnder("Forward/msaa1/native/oit0"));
}

TEST(PipelineManifest, AClassicAndAMeshStageShaderOfOneNameNeverShareEntries)
{
    PipelineManifest manifest;
    ASSERT_TRUE(manifest.Record("Variant", kClassicStages, Signature(1, 37, 0, 1), "cfg"));
    EXPECT_TRUE(manifest.EntriesFor("Variant", kMeshStages, "cfg").IsEmpty())
        << "An edit that adds a mesh stage must not inherit the classic shader's keys";
    EXPECT_EQ(manifest.EntriesFor("Variant", kClassicStages, "cfg").Num(), 1);
}

TEST(PipelineManifest, SurvivesARoundTripByteForByte)
{
    PipelineManifest manifest;
    manifest.BeginSession();
    (void)manifest.Record("Terrain_PBR", kClassicStages | 0x02 | 0x04, Signature(2, 97, 126, 1, /*patch=*/4),
                          "Forward/msaa1/native/oit0");
    (void)manifest.Record("PBR", kClassicStages, Signature(2, 97, 126, 1), "Forward/msaa1/native/oit0");
    (void)manifest.Record("FroxelFogScatter", 0x20, PipelineSignature{}, "Deferred/msaa1/native/oit1");

    const std::string text = manifest.Serialize();
    std::string error;
    sizet rejected = 7;
    const auto parsed = PipelineManifest::Parse(text, &error, &rejected);
    ASSERT_TRUE(parsed.has_value()) << error;
    EXPECT_EQ(rejected, 0u);
    EXPECT_EQ(parsed->GetEntryCount(), 3u);
    EXPECT_EQ(parsed->Serialize(), text) << "Deterministic: a save of an unchanged manifest is byte-identical";
    const auto terrain = parsed->EntriesFor("Terrain_PBR", kClassicStages | 0x02 | 0x04, "");
    ASSERT_EQ(terrain.Num(), 1);
    EXPECT_EQ(terrain[0]->Signature.PatchControlPoints, 4u) << "The patch size is part of the key";
}

TEST(PipelineManifest, RefusesAMalformedFileButKeepsTheValidEntriesOfAWellFormedOne)
{
    std::string error;
    EXPECT_FALSE(PipelineManifest::Parse("not: [valid", &error).has_value());
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(PipelineManifest::Parse("PipelineManifest:\n  Version: 99\n", &error).has_value())
        << "A future format version is refused, not misread";

    const std::string partlyBad =
        "PipelineManifest:\n"
        "  Version: 1\n"
        "  Entries:\n"
        "    - {Shader: Good, Stages: 17, Color: [37], Depth: 0, Samples: 1}\n"
        "    - {Shader: BadSamples, Stages: 17, Color: [37], Depth: 0, Samples: 3}\n"
        "    - {Shader: TooManyTargets, Stages: 17, Color: [1, 2, 3, 4, 5, 6, 7, 8, 9], Depth: 0, Samples: 1}\n"
        "    - {Stages: 17, Color: [37], Depth: 0, Samples: 1}\n";
    sizet rejected = 0;
    const auto parsed = PipelineManifest::Parse(partlyBad, &error, &rejected);
    ASSERT_TRUE(parsed.has_value()) << error;
    EXPECT_EQ(parsed->GetEntryCount(), 1u);
    EXPECT_EQ(rejected, 3u);
}

TEST(PipelineManifest, MergeUnionsConfigurationsAndTrimDropsTheLeastRecentlySeen)
{
    PipelineManifest local;
    local.BeginSession(); // session 1
    (void)local.Record("Old", kClassicStages, Signature(1, 37, 0, 1), "a");
    local.BeginSession(); // session 2
    (void)local.Record("New", kClassicStages, Signature(1, 37, 0, 1), "a");

    PipelineManifest seed;
    (void)seed.Record("New", kClassicStages, Signature(1, 37, 0, 1), "b");
    local.Merge(seed);
    const auto merged = local.EntriesFor("New", kClassicStages, "b");
    ASSERT_EQ(merged.Num(), 1);
    EXPECT_TRUE(merged[0]->SeenUnder("a"));
    EXPECT_TRUE(merged[0]->SeenUnder("b"));

    local.Trim(1);
    EXPECT_EQ(local.GetEntryCount(), 1u);
    EXPECT_TRUE(local.EntriesFor("Old", kClassicStages, "a").IsEmpty());
    EXPECT_EQ(local.EntriesFor("New", kClassicStages, "a").Num(), 1);
}

TEST(PipelineManifest, TrimNeverKeepsAnOlderEntryOverANewerOne)
{
    // Sessions {1, 2, 2}, keep one: whatever order the shaders hash in, the
    // session-1 entry goes and exactly one session-2 entry stays.
    for (int round = 0; round < 8; ++round)
    {
        PipelineManifest manifest;
        manifest.BeginSession(); // 1
        (void)manifest.Record("Oldest" + std::to_string(round), kClassicStages, Signature(1, 37, 0, 1), "a");
        manifest.BeginSession(); // 2
        (void)manifest.Record("NewA" + std::to_string(round), kClassicStages, Signature(1, 37, 0, 1), "a");
        (void)manifest.Record("NewA" + std::to_string(round), kClassicStages, Signature(1, 44, 0, 1), "a");
        manifest.Trim(1);
        EXPECT_EQ(manifest.GetEntryCount(), 1u);
        EXPECT_TRUE(manifest.EntriesFor("Oldest" + std::to_string(round), kClassicStages, "a").IsEmpty())
            << "round " << round;
    }
}

TEST(PipelineManifest, TrimBreaksTiesByIdentityNotByHashOrder)
{
    // Five shaders, all seen in the same session; keep two. The survivors are
    // the same on every standard library: the last two identities in order.
    PipelineManifest manifest;
    manifest.BeginSession();
    for (const char* name : { "Echo", "Alpha", "Delta", "Charlie", "Bravo" })
    {
        ASSERT_TRUE(manifest.Record(name, kClassicStages, Signature(1, 37, 0, 1), "a"));
    }
    manifest.Trim(2);
    EXPECT_EQ(manifest.GetEntryCount(), 2u);
    EXPECT_EQ(manifest.EntriesFor("Delta", kClassicStages, "a").Num(), 1);
    EXPECT_EQ(manifest.EntriesFor("Echo", kClassicStages, "a").Num(), 1);
}

TEST(PipelineManifest, TheConfigurationTagNamesEveryInputThatChangesAPassTarget)
{
    const std::string base = MakePipelineConfigurationTag("Deferred", 1, "native", false, true);
    EXPECT_EQ(base, "Deferred/msaa1/native/oit1");
    EXPECT_NE(MakePipelineConfigurationTag("Deferred", 4, "native", false, true), base);
    EXPECT_NE(MakePipelineConfigurationTag("Forward", 1, "native", false, true), base);
    EXPECT_NE(MakePipelineConfigurationTag("Deferred", 1, "upscaled", false, true), base);
    EXPECT_NE(MakePipelineConfigurationTag("Deferred", 1, "upscaled", true, true),
              MakePipelineConfigurationTag("Deferred", 1, "upscaled", false, true));
    EXPECT_NE(MakePipelineConfigurationTag("Deferred", 1, "native", false, false), base);
}

// -----------------------------------------------------------------------------
// 3. The key
// -----------------------------------------------------------------------------

#if OLO_WITH_VULKAN
namespace
{
    using Key = VulkanPipelineBuilder::Key;

    VulkanRenderTargetDesc Targets(const u32 count, const VkFormat format, const VkFormat depth, const u32 samples)
    {
        VulkanRenderTargetDesc targets;
        targets.ColorCount = count;
        for (u32 i = 0; i < count; ++i)
        {
            targets.ColorFormats[i] = format;
        }
        targets.DepthFormat = depth;
        targets.Samples = samples;
        return targets;
    }

    VulkanRootDataLayout Layout(const u32 binding)
    {
        VulkanShaderBinding ubo;
        ubo.Set = 0;
        ubo.Binding = binding;
        ubo.BindingKind = VulkanShaderBinding::Kind::UniformBuffer;
        return VulkanRootDataLayout::Build(std::span{ &ubo, 1 });
    }

    Key Derive(const u64 shader, const VulkanRenderTargetDesc& targets, const VulkanRootDataLayout& layout,
               const bool tessellated = false, const u32 patch = 3, const bool dynamicBlend = true,
               const VulkanRecordedPipelineState* stateOverride = nullptr)
    {
        VulkanRecordedPipelineState state = stateOverride != nullptr ? *stateOverride : VulkanRecordedPipelineState{};
        state.PatchVertexCount = patch;
        return VulkanPipelineBuilder::DeriveGraphicsKey(shader, layout, tessellated, dynamicBlend, state, targets);
    }

    void ExpectApart(const Key& a, const Key& b, const char* why)
    {
        EXPECT_FALSE(a == b) << why;
        EXPECT_NE(VulkanPipelineBuilder::KeyHash{}(a), VulkanPipelineBuilder::KeyHash{}(b)) << why << " (hash)";
    }
} // namespace

TEST(VulkanPipelineKey, IncompatibleConfigurationsNeverDeriveTheSameKey)
{
    const auto layout = Layout(31);
    const auto base = Targets(5, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, 1);
    const Key reference = Derive(1, base, layout);

    ExpectApart(reference, Derive(1, Targets(5, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, 4), layout),
                "sample count (MSAA on/off)");
    ExpectApart(reference, Derive(1, Targets(5, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT, 1), layout),
                "colour attachment format");
    ExpectApart(reference,
                Derive(1, Targets(5, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, 1), layout),
                "depth format");
    ExpectApart(reference, Derive(1, Targets(4, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_D32_SFLOAT, 1), layout),
                "attachment count");
    ExpectApart(reference, Derive(2, base, layout),
                "shader identity: a skinned, mesh-stage or material variant is a different shader");
    ExpectApart(reference, Derive(1, base, Layout(32)), "root-data layout (the baked binding mappings)");

    // Tessellation: a patch pipeline is apart from a classic one, and two
    // patch sizes are apart from each other; a classic pipeline ignores the
    // recorded patch size entirely.
    const Key tess3 = Derive(1, base, layout, /*tessellated=*/true, 3);
    ExpectApart(reference, tess3, "tessellation stage present");
    ExpectApart(tess3, Derive(1, base, layout, true, 4), "tessellation patch size");
    EXPECT_TRUE(reference == Derive(1, base, layout, false, 4)) << "Patch size is not an axis without tessellation";

    // Blend is dynamic on the ADR 0010 floor (EDS3) and therefore no axis; where
    // it is baked, two blend states must not share a pipeline.
    VulkanRecordedPipelineState blended;
    blended.Blend = true;
    EXPECT_TRUE(reference == Derive(1, base, layout, false, 3, /*dynamicBlend=*/true, &blended));
    ExpectApart(Derive(1, base, layout, false, 3, /*dynamicBlend=*/false),
                Derive(1, base, layout, false, 3, /*dynamicBlend=*/false, &blended), "baked blend state");
}

TEST(VulkanPipelineKey, ResolutionAndStaleFormatsPastTheAttachmentCountAreNotInputs)
{
    // VulkanRenderTargetDesc carries no extent at all: viewport and scissor are
    // VIEWPORT/SCISSOR_WITH_COUNT dynamic state, so one pipeline serves every
    // render resolution (the non-native-resolution axis is not a PSO axis).
    const auto layout = Layout(31);
    const auto tidy = Targets(2, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED, 1);
    auto stale = tidy;
    stale.ColorFormats[5] = VK_FORMAT_R32_SINT; // left over from a wider target
    EXPECT_TRUE(Derive(1, tidy, layout) == Derive(1, stale, layout));
    EXPECT_EQ(VulkanPipelineBuilder::KeyHash{}(Derive(1, tidy, layout)),
              VulkanPipelineBuilder::KeyHash{}(Derive(1, stale, layout)));
}

TEST(VulkanPipelineKey, ComputeKeysNeverCollideWithGraphicsKeysOfTheSameShaderIndex)
{
    const auto layout = Layout(31);
    const Key compute = VulkanPipelineBuilder::DeriveComputeKey(1, layout);
    const Key graphics = Derive(1, Targets(1, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED, 1), layout);
    EXPECT_FALSE(compute == graphics);
}
#endif

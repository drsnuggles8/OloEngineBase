// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"

#include "GroomStrandFixture.h"
#include "GroomBindingFixture.h"
#include "../TestAsyncLoadHooks.h"
#include "OloEngine/Groom/GroomLodBuilder.h"
#include "OloEngine/Groom/GroomBindingBuilder.h"
#include "OloEngine/Groom/GroomStreaming.h"
#include "OloEngine/Renderer/Passes/GroomRenderPass.h"
#include "OloEngine/Renderer/RGCommandContext.h"

#include <gtest/gtest.h>
#include <chrono>
#include <cstring>
#include <thread>
#include <limits>

using namespace OloEngine; // NOLINT(google-build-using-namespace)

namespace
{
    GroomStrandRequest Request()
    {
        GroomStrandRequest request;
        request.Groom = Tests::GroomStrandFixture::MakePelt(384, 4).Groom;
        request.Handle = 1257;
        request.EntityID = 73;
        request.Build.MaxStrands = 384;
        request.Coat.Enabled = true;
        request.Coat.LengthJitter = 0.3f;
        request.Coat.ShadeJitter = 0.2f;
        request.Build.CoatDigest = GroomCoatDigest(request.Coat);
        return request;
    }

    class GroomStreamingTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            Tests::EnsureTaskSchedulerStarted();
            RepresentationStreaming::Get().Reset();
            RepresentationStreaming::Get().SetStagingBudget(16u * 1024u * 1024u);
        }
        void TearDown() override
        {
            EXPECT_EQ(RepresentationStreaming::Get().GetStats().StagingCpuBytes, 0u);
            RepresentationStreaming::Get().Reset();
            RepresentationStreaming::Get().SetStagingBudget(0);
        }
    };
} // namespace

TEST_F(GroomStreamingTest, UncookedFloorPreservesSourceIdentityAndBoundsWork)
{
    auto request = Request();
    ASSERT_TRUE(request.Groom);
    const auto groom = request.Groom;
    const auto original = request.Coat.Seed;
    ApplyGroomStreamingFloor(request, 24);
    EXPECT_TRUE(request.StreamingFloor);
    EXPECT_EQ(request.Groom, groom);
    EXPECT_EQ(request.Handle, AssetHandle(1257));
    EXPECT_EQ(request.EntityID, 73);
    EXPECT_EQ(request.Coat.Seed, original);
    EXPECT_EQ(request.Lod.Representation, GroomRepresentation::Strand);
    EXPECT_EQ(request.Build.MaxStrands, 24u);
    EXPECT_LE(request.Build.MaxSegments, 24u * 64u);
    auto first = PrepareGroomGeometry(groom, nullptr, nullptr, request.Build, request.Coat);
    auto reloaded = PrepareGroomGeometry(groom, nullptr, nullptr, request.Build, request.Coat);
    ASSERT_TRUE(first);
    ASSERT_TRUE(reloaded);
    EXPECT_GT(first->Stats.StrandsSelected, 0u);
    EXPECT_LE(first->Stats.StrandsSelected, 24u);
    ASSERT_EQ(first->Vertices.Num(), reloaded->Vertices.Num());
    EXPECT_EQ(std::memcmp(first->Vertices.GetData(), reloaded->Vertices.GetData(),
                          static_cast<sizet>(first->Vertices.Num()) * sizeof(GroomStrandVertex)),
              0);
    EXPECT_LE(first->GetCpuBytes(), DescribeGroomGeometry(request).CpuBytes.GetBytes().value());
}

TEST_F(GroomStreamingTest, CookedCardFloorRetainsTheCompleteTierAndBaseCurveMap)
{
    auto request = Request();
    // Card clump addressing requires a bounded root chart. The strand-only
    // pelt fixture deliberately has unwrapped UVs; use the bound coat here.
    request.Groom = GroomBindingTest::MakeCoat(384, 4);
    ASSERT_TRUE(request.Groom);
    GroomLodLevel cards;
    GroomCardSettings settings;
    settings.CellSize = 0.2f;
    std::string reason;
    ASSERT_TRUE(GroomCooker::Canonicalize(*request.Groom, reason)) << reason;
    ASSERT_TRUE(GroomLodBuilder::BuildCardLevel(*request.Groom, settings, cards, reason)) << reason;
    std::vector<GroomLodLevel> levels;
    levels.push_back(std::move(cards));
    ASSERT_TRUE(GroomLodBuilder::AttachLodLevels(*request.Groom, std::move(levels), reason)) << reason;
    const auto* level = request.Groom->FindLodLevel(GroomRepresentation::Card);
    ASSERT_NE(level, nullptr);
    const auto sourceMap = level->SourceCurves;
    ApplyGroomStreamingFloor(request, 1);
    EXPECT_EQ(request.Lod.Representation, GroomRepresentation::Card);
    EXPECT_EQ(request.LodLevel, level);
    EXPECT_EQ(request.Build.MaxStrands, level->GetCurveCount());
    EXPECT_EQ(request.BuildSource().BaseCurveCount, request.Groom->GetCurveCount());
    EXPECT_EQ(level->SourceCurves, sourceMap);
}

TEST_F(GroomStreamingTest, DisabledResidencyLeavesTheAuthoredRequestUntouched)
{
    GroomRenderPass pass;
    auto request = Request();
    const auto build = request.Build;
    pass.BeginStreamingFrame(1, false, 1);
    pass.ResolveStreamingRequest(request);
    EXPECT_EQ(request.Build, build);
    EXPECT_FALSE(request.StreamingEnabled);
    EXPECT_FALSE(request.StreamingFloor);
    EXPECT_EQ(pass.GetStreamingStats().Requested, 0u);
}

TEST_F(GroomStreamingTest, QueuedPreparationDrawsFloorAndReadmitsTheSameIdentity)
{
    Tasks::FTaskEvent gate("GroomStreamingPending");
    auto& budget = RepresentationStreaming::Get();
    budget.BeginFrame(1, 8u * 1024u * 1024u, 8u * 1024u * 1024u);
    GroomRenderPass pass;
    pass.SetStreamingStartGate(gate);
    pass.BeginStreamingFrame(1, true, 24);
    auto pending = Request();
    const auto groom = pending.Groom;
    pass.ResolveStreamingRequest(pending);
    EXPECT_TRUE(pending.StreamingPending);
    EXPECT_TRUE(pending.StreamingFloor);
    EXPECT_EQ(pending.StreamingFallback, EGroomStreamingFallback::Pending);
    EXPECT_GT(budget.GetStats().StagingCpuBytes, 0u);
    gate.Trigger();
    pass.SetStreamingStartGate(std::nullopt);
    const auto deadline = std::chrono::steady_clock::now() + Tests::kLoadHookFailAfter;
    bool admitted = false;
    u64 frame = 2;
    while (!admitted && std::chrono::steady_clock::now() < deadline)
    {
        budget.BeginFrame(frame, 8u * 1024u * 1024u, 8u * 1024u * 1024u);
        pass.BeginStreamingFrame(frame++, true, 24);
        auto request = Request();
        request.Groom = groom;
        pass.ResolveStreamingRequest(request);
        admitted = !request.StreamingFloor;
        EXPECT_EQ(request.EntityID, pending.EntityID);
        EXPECT_EQ(request.Handle, pending.Handle);
        EXPECT_EQ(request.Coat.Seed, pending.Coat.Seed);
        std::this_thread::yield();
    }
    EXPECT_TRUE(admitted);
    EXPECT_GT(budget.GetStats().OptionalResidentGpuBytes, 0u);
    // Ready CPU geometry stays charged until real GPU integration or teardown.
    EXPECT_GT(budget.GetStats().StagingCpuBytes, 0u);
}

TEST_F(GroomStreamingTest, MemoryPressureKeepsMandatoryFloorWhileDetailIsReady)
{
    auto& budget = RepresentationStreaming::Get();
    budget.BeginFrame(1, 1, 8u * 1024u * 1024u);
    GroomRenderPass pass;
    pass.BeginStreamingFrame(1, true, 24);
    auto request = Request();
    const auto groom = request.Groom;
    pass.ResolveStreamingRequest(request);
    const auto deadline = std::chrono::steady_clock::now() + Tests::kLoadHookFailAfter;
    u64 frame = 2;
    do
    {
        budget.BeginFrame(frame, 1, 8u * 1024u * 1024u);
        pass.BeginStreamingFrame(frame++, true, 24);
        request = Request();
        request.Groom = groom;
        pass.ResolveStreamingRequest(request);
        std::this_thread::yield();
    } while (request.StreamingPending && std::chrono::steady_clock::now() < deadline);
    EXPECT_FALSE(request.StreamingPending);
    EXPECT_TRUE(request.StreamingFloor);
    EXPECT_EQ(request.StreamingFallback, EGroomStreamingFallback::ResidentBudget);
    EXPECT_EQ(budget.GetStats().OptionalResidentGpuBytes, 0u);
    EXPECT_GT(budget.GetStats().PinnedResidentGpuBytes, 0u);
}

TEST_F(GroomStreamingTest, LastGroomDepartureCancelsPreparationWithoutARenderTarget)
{
    Tasks::FTaskEvent gate("GroomStreamingDeparture");
    auto& budget = RepresentationStreaming::Get();
    budget.BeginFrame(1, 8u * 1024u * 1024u, 8u * 1024u * 1024u);
    GroomRenderPass pass;
    pass.SetStreamingStartGate(gate);
    pass.BeginStreamingFrame(1, true, 24);
    auto request = Request();
    pass.ResolveStreamingRequest(request);
    ASSERT_TRUE(request.StreamingPending);
    pass.BeginStreamingFrame(2, true, 24);
    RGCommandContext context;
    pass.Execute(context); // the real empty-pass boundary owns retirement
    EXPECT_EQ(pass.GetStreamingStats().Cancelled, 1u);
    EXPECT_EQ(budget.GetStats().PinnedResidentGpuBytes, 0u);
    EXPECT_EQ(budget.GetStats().OptionalResidentGpuBytes, 0u);
    EXPECT_EQ(budget.GetStats().StagingCpuBytes, 0u);
    gate.Trigger();
}

TEST_F(GroomStreamingTest, SharedBoundGeometryChargesEntityStateAndRefusesUnbudgetedUploads)
{
    const auto surface = GroomBindingTest::MakeGrid(8);
    auto original = Request();
    original.Groom = GroomBindingTest::MakeCoat(384);
    GroomBindingBuildStats bindingStats;
    std::string reason;
    ASSERT_TRUE(GroomBindingBuilder::Build(*original.Groom, surface.View(), "StreamingBody",
                                           GroomBindingBuildSettings{}, original.Binding, bindingStats, reason))
        << reason;
    original.StreamingGuideSlots = 2;
    original.StreamingDisplacementPoints = 8;
    auto& budget = RepresentationStreaming::Get();
    GroomRenderPass pass;
    pass.SetGpuDeformationEnabled(true);
    budget.BeginFrame(1, 8u * 1024u * 1024u, 8u * 1024u * 1024u);
    pass.BeginStreamingFrame(1, true, 24);
    auto request = original;
    pass.ResolveStreamingRequest(request);
    ASSERT_TRUE(request.StreamingFloor);
    const auto deadline = std::chrono::steady_clock::now() + Tests::kLoadHookFailAfter;
    u64 frame = 2;
    do
    {
        budget.BeginFrame(frame, 8u * 1024u * 1024u, 8u * 1024u * 1024u);
        pass.BeginStreamingFrame(frame++, true, 24);
        request = original;
        pass.ResolveStreamingRequest(request);
        std::this_thread::yield();
    } while (request.StreamingFloor && std::chrono::steady_clock::now() < deadline);
    ASSERT_FALSE(request.StreamingFloor);
    ASSERT_NE(request.StreamingEntityKey, 0u);
    const auto first = budget.GetStats().OptionalResidentGpuBytes;
    auto second = original;
    ++second.EntityID;
    pass.ResolveStreamingRequest(second);
    ASSERT_FALSE(second.StreamingFloor);
    EXPECT_EQ(second.StreamingKey, request.StreamingKey);
    EXPECT_NE(second.StreamingEntityKey, request.StreamingEntityKey);
    const auto entityCapacity = GroomDeformBufferLayout::Make(384, 2, 8).TotalBytes() + 65536u;
    EXPECT_EQ(budget.GetStats().OptionalResidentGpuBytes - first, entityCapacity);

    // A prepared immutable stream alone cannot authorize entity initialization.
    budget.BeginFrame(frame, 8u * 1024u * 1024u, 1);
    pass.BeginStreamingFrame(frame, true, 24);
    auto refused = original;
    pass.ResolveStreamingRequest(refused);
    EXPECT_TRUE(refused.StreamingFloor);
    EXPECT_EQ(refused.StreamingFallback, EGroomStreamingFallback::UploadBudget);
    EXPECT_GT(budget.GetStats().PinnedResidentGpuBytes, 0u);
}

TEST_F(GroomStreamingTest, DeferredSharedIntegrationRequiresTheCurrentFramesUploadAllowance)
{
    const auto surface = GroomBindingTest::MakeGrid(8);
    auto original = Request();
    original.Groom = GroomBindingTest::MakeCoat(384);
    GroomBindingBuildStats bindingStats;
    std::string reason;
    ASSERT_TRUE(GroomBindingBuilder::Build(*original.Groom, surface.View(), "StreamingBody",
                                           GroomBindingBuildSettings{}, original.Binding, bindingStats, reason))
        << reason;
    const auto prepared = PrepareGroomGeometry(original.Groom, original.Binding, nullptr, original.Build, original.Coat);
    ASSERT_TRUE(prepared);
    const u64 sharedUpload = prepared->Stats.VertexBytes + prepared->Stats.IndexBytes * 2u;
    const u64 entityUpload = GroomDeformBufferLayout::Make(static_cast<u32>(prepared->RootCurves.Num()), 0, 0).TotalBytes();
    ASSERT_GT(sharedUpload, entityUpload);
    auto& budget = RepresentationStreaming::Get();
    GroomRenderPass pass;
    pass.SetGpuDeformationEnabled(true);
    budget.BeginFrame(1, 1, 8u * 1024u * 1024u);
    pass.BeginStreamingFrame(1, true, 24);
    auto request = original;
    pass.ResolveStreamingRequest(request);
    const auto deadline = std::chrono::steady_clock::now() + Tests::kLoadHookFailAfter;
    u64 frame = 2;
    do
    {
        // Keep the ready stream out of resident admission until preparation
        // finishes, so the test controls the first possible integration frame.
        budget.BeginFrame(frame, 1, 8u * 1024u * 1024u);
        pass.BeginStreamingFrame(frame++, true, 24);
        request = original;
        pass.ResolveStreamingRequest(request);
        std::this_thread::yield();
    } while (request.StreamingPending && std::chrono::steady_clock::now() < deadline);
    ASSERT_FALSE(request.StreamingPending);
    ASSERT_EQ(request.StreamingFallback, EGroomStreamingFallback::ResidentBudget);

    budget.BeginFrame(frame, 8u * 1024u * 1024u, sharedUpload);
    pass.BeginStreamingFrame(frame++, true, 24);
    request = original;
    pass.ResolveStreamingRequest(request);
    ASSERT_TRUE(request.StreamingFloor);
    EXPECT_EQ(request.StreamingFallback, EGroomStreamingFallback::UploadBudget);
    EXPECT_GT(budget.GetStats().OptionalResidentGpuBytes, 0u);
    EXPECT_GT(budget.GetStats().StagingCpuBytes, 0u);

    // This allowance would initialize the entity if last frame's shared
    // reservation were incorrectly reused, but cannot upload the shared stream.
    budget.BeginFrame(frame, 8u * 1024u * 1024u, entityUpload);
    pass.BeginStreamingFrame(frame++, true, 24);
    request = original;
    pass.ResolveStreamingRequest(request);
    EXPECT_TRUE(request.StreamingFloor) << "unintegrated shared geometry must not reach the detail upload path";
    EXPECT_EQ(request.StreamingFallback, EGroomStreamingFallback::UploadBudget);
    EXPECT_EQ(pass.GetStreamingStats().DetailDraws, 0u);

    budget.BeginFrame(frame, 8u * 1024u * 1024u, sharedUpload + entityUpload);
    pass.BeginStreamingFrame(frame, true, 24);
    request = original;
    pass.ResolveStreamingRequest(request);
    ASSERT_FALSE(request.StreamingFloor);
    EXPECT_NE(request.StreamingEntityKey, 0u);
    EXPECT_EQ(budget.GetStats().UploadedBytesThisFrame, sharedUpload + entityUpload);
    const u64 admittedBytes = budget.GetStats().UploadedBytesThisFrame;
    auto repeated = original;
    pass.ResolveStreamingRequest(repeated);
    EXPECT_FALSE(repeated.StreamingFloor);
    EXPECT_EQ(budget.GetStats().UploadedBytesThisFrame, admittedBytes) << "shadow and beauty must share one transfer admission";
}

TEST_F(GroomStreamingTest, StagingPressureLeavesAnExplicitFloorWithoutStartingWork)
{
    auto& budget = RepresentationStreaming::Get();
    budget.SetStagingBudget(1);
    budget.BeginFrame(1, 8u * 1024u * 1024u, 8u * 1024u * 1024u);
    GroomRenderPass pass;
    pass.BeginStreamingFrame(1, true, 24);
    auto request = Request();
    pass.ResolveStreamingRequest(request);
    EXPECT_TRUE(request.StreamingFloor);
    EXPECT_FALSE(request.StreamingPending);
    EXPECT_EQ(request.StreamingFallback, EGroomStreamingFallback::StagingBudget);
    EXPECT_EQ(budget.GetStats().StagingCpuBytes, 0u);
}

TEST_F(GroomStreamingTest, ExtremeRetiringBackingReportsResidentPressureWithoutWrapping)
{
    auto& budget = RepresentationStreaming::Get();
    budget.SetRetiringGpuBytes(std::numeric_limits<u64>::max());
    budget.BeginFrame(1, 8u * 1024u * 1024u, 8u * 1024u * 1024u);
    GroomRenderPass pass;
    pass.BeginStreamingFrame(1, true, 24);
    auto original = Request();
    auto request = original;
    pass.ResolveStreamingRequest(request);
    const auto deadline = std::chrono::steady_clock::now() + Tests::kLoadHookFailAfter;
    u64 frame = 2;
    do
    {
        budget.BeginFrame(frame, 8u * 1024u * 1024u, 8u * 1024u * 1024u);
        pass.BeginStreamingFrame(frame++, true, 24);
        request = original;
        pass.ResolveStreamingRequest(request);
        std::this_thread::yield();
    } while (request.StreamingPending && std::chrono::steady_clock::now() < deadline);
    EXPECT_FALSE(request.StreamingPending);
    EXPECT_TRUE(request.StreamingFloor);
    EXPECT_EQ(request.StreamingFallback, EGroomStreamingFallback::ResidentBudget);
    EXPECT_EQ(budget.GetStats().OptionalResidentGpuBytes, 0u);
}

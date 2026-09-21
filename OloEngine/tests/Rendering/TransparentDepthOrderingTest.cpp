#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "RenderingTestUtils.h"
#include "OloEngine/Renderer/Commands/CommandAllocator.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include "OloEngine/Renderer/Commands/CommandPacket.h"
#include "OloEngine/Renderer/Commands/DrawKey.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"

#include <algorithm>
#include <array>
#include <memory>
#include <numeric>
#include <random>
#include <vector>

// OLO_TEST_LAYER: plumbing

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

// =============================================================================
// Issue #1327 — conventional alpha-blended draws must sort depth-major.
//
// `DrawKey::CreateTransparent` inverts depth so that back-to-front falls out of
// an ascending raw-key sort, but depth used to sit in the LEAST significant
// field, below shader and material. Inverting it therefore only ordered draws
// WITHIN one shader+material bucket; across buckets the blend order was
// material-ID order. Two overlapping 50% surfaces with different materials
// composited in whichever order their material IDs happened to fall.
//
// These tests drive the real CommandBucket radix sort (not `operator<`) and
// composite the sorted sequence with the source-over operator the GL/Vulkan
// blend state implements, so they assert the framebuffer claim, not the key
// claim. The pixel-level counterpart is
// PropertyTests/TransparentBlendOrderVisualEvidenceTest.cpp.
// =============================================================================

namespace
{
    // One alpha-blended surface, fully covering the pixel under test.
    struct Layer
    {
        glm::vec3 m_Color{ 0.0f };
        f32 m_Alpha = 1.0f;
        u32 m_Depth = 0;    // Camera-space depth key: larger == farther.
        u32 m_ShaderID = 1; // Distinct shaders/materials are the whole point.
        u32 m_MaterialID = 1;
        i32 m_EntityID = 0; // Identity carried through sort/batch.
    };

    constexpr glm::vec3 kBlack{ 0.0f, 0.0f, 0.0f };
    constexpr glm::vec3 kRed{ 1.0f, 0.0f, 0.0f };
    constexpr glm::vec3 kBlue{ 0.0f, 0.0f, 1.0f };

    // GL_SRC_ALPHA / GL_ONE_MINUS_SRC_ALPHA, applied in the given draw order.
    glm::vec3 CompositeOver(const std::vector<Layer>& drawOrder, const glm::vec3& background)
    {
        glm::vec3 dst = background;
        for (const Layer& layer : drawOrder)
        {
            dst = layer.m_Alpha * layer.m_Color + (1.0f - layer.m_Alpha) * dst;
        }
        return dst;
    }

    // GL_ONE / GL_ONE — accumulation, no dependence on draw order.
    glm::vec3 CompositeAdd(const std::vector<Layer>& drawOrder, const glm::vec3& background)
    {
        glm::vec3 dst = background;
        for (const Layer& layer : drawOrder)
        {
            dst += layer.m_Alpha * layer.m_Color;
        }
        return dst;
    }

    DrawMeshCommand MakeBlendedDraw(const Layer& layer)
    {
        DrawMeshCommand cmd = MakeSyntheticDrawMeshCommand(
            layer.m_ShaderID, layer.m_MaterialID, 0.5f, layer.m_EntityID);
        // Per-instance attributes that must survive any later grouping step.
        cmd.color = glm::vec4(layer.m_Color, layer.m_Alpha);
        cmd.custom = static_cast<f32>(layer.m_EntityID);
        return cmd;
    }

    PacketMetadata MakeTransparentMetadata(const Layer& layer)
    {
        PacketMetadata meta;
        meta.m_SortKey = DrawKey::CreateTransparent(
            0, ViewLayerType::ThreeD, layer.m_ShaderID, layer.m_MaterialID, layer.m_Depth);
        return meta;
    }

    // Recover the draw order a sorted bucket would execute, as Layers, by
    // matching each packet's entityID back to the submitted set. Instanced
    // packets expand to their per-instance entityIDs in instance order, which
    // is exactly the order the GPU blends them.
    std::vector<Layer> ExecutionOrder(const CommandBucket& bucket, const std::vector<Layer>& submitted)
    {
        auto layerFor = [&submitted](i32 entityID) -> const Layer&
        {
            const auto it = std::ranges::find_if(submitted, [entityID](const Layer& l)
                                                 { return l.m_EntityID == entityID; });
            OLO_CORE_ASSERT(it != submitted.end(), "Unknown entityID in execution order");
            return *it;
        };

        std::vector<Layer> order;
        for (const CommandPacket* packet : bucket.GetSortedCommands())
        {
            if (!packet)
                continue;
            if (packet->GetCommandType() == CommandType::DrawMeshInstanced)
            {
                const auto* icmd = packet->GetCommandData<DrawMeshInstancedCommand>();
                FrameDataBuffer& frameBuffer = FrameDataBufferManager::Get();
                for (u32 i = 0; i < icmd->instanceCount; ++i)
                {
                    const i32* id = icmd->entityIDBufferOffset == UINT32_MAX
                                        ? nullptr
                                        : frameBuffer.GetEntityIDPtr(icmd->entityIDBufferOffset + i);
                    OLO_CORE_ASSERT(id, "Batched transparent draw lost its per-instance entityIDs");
                    order.push_back(layerFor(*id));
                }
                continue;
            }
            const auto* cmd = packet->GetCommandData<DrawMeshCommand>();
            order.push_back(layerFor(cmd->entityID));
        }
        return order;
    }

    std::string DescribeOrder(const std::vector<Layer>& order)
    {
        std::ostringstream oss;
        oss << "[";
        for (sizet i = 0; i < order.size(); ++i)
        {
            oss << (i ? ", " : "") << "entity=" << order[i].m_EntityID
                << " depth=" << order[i].m_Depth
                << " mat=" << order[i].m_MaterialID
                << " shader=" << order[i].m_ShaderID;
        }
        oss << "]";
        return oss.str();
    }

    // The worked example from issue #1327: red 50% in front of blue 50% over
    // black must give (0.5, 0, 0.25). Reversed it gives (0.25, 0, 0.5).
    constexpr glm::vec3 kExpectedComposite{ 0.5f, 0.0f, 0.25f };
    constexpr glm::vec3 kReversedComposite{ 0.25f, 0.0f, 0.5f };
    constexpr f32 kBlendTolerance = 1e-5f;

    void ExpectColorNear(const glm::vec3& actual, const glm::vec3& expected, const std::string& context)
    {
        EXPECT_NEAR(actual.r, expected.r, kBlendTolerance) << context;
        EXPECT_NEAR(actual.g, expected.g, kBlendTolerance) << context;
        EXPECT_NEAR(actual.b, expected.b, kBlendTolerance) << context;
    }
} // namespace

// Batching and instanced expansion both need FrameDataBufferManager.
class TransparentDepthOrderingTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Allocator = std::make_unique<CommandAllocator>();
        m_OwnsFrameData = !FrameDataBufferManager::IsInitialized();
        if (m_OwnsFrameData)
            FrameDataBufferManager::Init();
        FrameDataBufferManager::Get().Reset();
    }

    void TearDown() override
    {
        if (m_OwnsFrameData)
            FrameDataBufferManager::Shutdown();
        m_Allocator.reset();
    }

    // Submit `layers` in the given order, run the bucket's real sort (and
    // optionally its batching step), and return the resulting draw order.
    std::vector<Layer> SortedOrder(const std::vector<Layer>& layers, bool enableBatching)
    {
        CommandBucketConfig config;
        config.EnableSorting = true;
        config.EnableBatching = enableBatching;
        CommandBucket bucket(config);

        for (const Layer& layer : layers)
        {
            bucket.Submit(MakeBlendedDraw(layer), MakeTransparentMetadata(layer), m_Allocator.get());
        }

        if (enableBatching)
        {
            bucket.BatchCommands(*m_Allocator);
            if (!bucket.IsSorted())
                bucket.SortCommands();
        }
        else
        {
            bucket.SortCommands();
        }
        return ExecutionOrder(bucket, layers);
    }

    std::unique_ptr<CommandAllocator> m_Allocator;

  private:
    bool m_OwnsFrameData = false;
};

// =============================================================================
// Negative control — the composite really is order-sensitive
// =============================================================================

TEST_F(TransparentDepthOrderingTest, SourceOverIsOrderSensitive)
{
    // Without this, every ordering assertion below could pass on a compositor
    // that ignored order entirely.
    const Layer red{ kRed, 0.5f, 100, 1, 1, 0 };
    const Layer blue{ kBlue, 0.5f, 900, 2, 2, 1 };

    ExpectColorNear(CompositeOver({ blue, red }, kBlack), kExpectedComposite, "blue then red (correct)");
    ExpectColorNear(CompositeOver({ red, blue }, kBlack), kReversedComposite, "red then blue (reversed)");
    EXPECT_GT(glm::distance(kExpectedComposite, kReversedComposite), 0.1f)
        << "The two orders must be distinguishable for the ordering tests to mean anything";
}

// =============================================================================
// AC1 — overlapping transparents with different shaders/materials
// =============================================================================

TEST_F(TransparentDepthOrderingTest, RedOverBlueCompositesDepthOrderedAcrossMaterials)
{
    // Red is NEARER (smaller depth key) and must therefore be drawn LAST.
    // Material and shader IDs are chosen so the pre-fix state-major key sorts
    // red first: it owns the lower shader and the lower material ID.
    const Layer redNear{ kRed, 0.5f, 100, /*shader*/ 1, /*material*/ 1, 0 };
    const Layer blueFar{ kBlue, 0.5f, 900, /*shader*/ 2, /*material*/ 2, 1 };

    const std::vector<Layer> order = SortedOrder({ redNear, blueFar }, /*enableBatching*/ false);
    ASSERT_EQ(order.size(), 2u);
    ExpectColorNear(CompositeOver(order, kBlack), kExpectedComposite, DescribeOrder(order));
}

TEST_F(TransparentDepthOrderingTest, CompositeSurvivesEveryMaterialAndSubmissionPermutation)
{
    // AC1 in full: permute which surface owns the lower material ID, which owns
    // the lower shader ID, and the submission order. The composite must not
    // move — depth alone decides.
    for (u32 permutation = 0; permutation < 8u; ++permutation)
    {
        const bool redOwnsLowerMaterial = (permutation & 0x1u) != 0;
        const bool redOwnsLowerShader = (permutation & 0x2u) != 0;
        const bool submitRedFirst = (permutation & 0x4u) != 0;

        const Layer redNear{ kRed, 0.5f, 100,
                             redOwnsLowerShader ? 1u : 2u,
                             redOwnsLowerMaterial ? 1u : 2u, 0 };
        const Layer blueFar{ kBlue, 0.5f, 900,
                             redOwnsLowerShader ? 2u : 1u,
                             redOwnsLowerMaterial ? 2u : 1u, 1 };

        std::vector<Layer> submitted;
        if (submitRedFirst)
            submitted = { redNear, blueFar };
        else
            submitted = { blueFar, redNear };

        const std::vector<Layer> order = SortedOrder(submitted, /*enableBatching*/ false);
        ASSERT_EQ(order.size(), 2u) << "permutation " << permutation;
        ExpectColorNear(CompositeOver(order, kBlack), kExpectedComposite,
                        "permutation " + std::to_string(permutation) + " -> " + DescribeOrder(order));
    }
}

TEST_F(TransparentDepthOrderingTest, ManyLayersSortStrictlyBackToFrontRegardlessOfMaterial)
{
    // A deeper stack: depth and material ID are deliberately ANTI-correlated,
    // so a material-major sort produces exactly the reverse of the right answer.
    constexpr u32 kLayerCount = 16;
    std::vector<Layer> layers;
    layers.reserve(kLayerCount);
    for (u32 i = 0; i < kLayerCount; ++i)
    {
        const u32 depth = 1000u + i * 1000u;              // ascending: i=0 nearest
        const u32 materialID = kLayerCount - i;           // descending
        const u32 shaderID = 1u + (kLayerCount - i) % 4u; // also anti-correlated
        layers.push_back(Layer{ glm::vec3(static_cast<f32>(i) / kLayerCount, 0.25f, 0.5f),
                                0.4f, depth, shaderID, materialID, static_cast<i32>(i) });
    }

    // Shuffle the submission order too — nothing about submission may leak in.
    auto rng = MakeTestRNG();
    std::ranges::shuffle(layers, rng);

    const std::vector<Layer> order = SortedOrder(layers, /*enableBatching*/ false);
    ASSERT_EQ(order.size(), kLayerCount);
    for (sizet i = 1; i < order.size(); ++i)
    {
        EXPECT_GE(order[i - 1].m_Depth, order[i].m_Depth)
            << "Draw " << i << " is farther than its predecessor — not back-to-front.\n"
            << DescribeOrder(order);
    }
}

// =============================================================================
// AC2 — batching, submission mode and per-instance attributes
// =============================================================================

TEST_F(TransparentDepthOrderingTest, BatchingDoesNotChangeTheBlendOrder)
{
    // Same mesh + material for every layer, so every draw is a batching
    // candidate, but the depths differ — collapsing them into one instanced
    // draw at the first packet's key would erase the ordering.
    std::vector<Layer> layers;
    for (u32 i = 0; i < 6; ++i)
    {
        layers.push_back(Layer{ glm::vec3(0.2f * static_cast<f32>(i), 0.3f, 0.7f),
                                0.5f, 500u + i * 700u, /*shader*/ 3, /*material*/ 7,
                                static_cast<i32>(i) });
    }
    auto rng = MakeTestRNG();
    std::ranges::shuffle(layers, rng);

    const std::vector<Layer> unbatched = SortedOrder(layers, /*enableBatching*/ false);
    const std::vector<Layer> batched = SortedOrder(layers, /*enableBatching*/ true);

    ASSERT_EQ(unbatched.size(), layers.size());
    ASSERT_EQ(batched.size(), layers.size()) << "Batching dropped a transparent draw: " << DescribeOrder(batched);
    ExpectColorNear(CompositeOver(batched, kBlack), CompositeOver(unbatched, kBlack),
                    "batched " + DescribeOrder(batched) + "\nunbatched " + DescribeOrder(unbatched));

    for (sizet i = 0; i < batched.size(); ++i)
    {
        EXPECT_EQ(batched[i].m_EntityID, unbatched[i].m_EntityID)
            << "Batching reordered draw " << i << "\nbatched   " << DescribeOrder(batched)
            << "\nunbatched " << DescribeOrder(unbatched);
    }
}

TEST_F(TransparentDepthOrderingTest, CoincidentTransparentsStillBatchAndKeepSubmissionOrder)
{
    // Draws that share a complete sort key were already an unordered tie run,
    // so collapsing them changes nothing — batching must still be allowed to
    // do it, and must lay the instances out in submission order.
    constexpr u32 kCount = 5;
    std::vector<Layer> layers;
    for (u32 i = 0; i < kCount; ++i)
    {
        layers.push_back(Layer{ glm::vec3(0.1f * static_cast<f32>(i)), 0.5f,
                                /*depth*/ 4242, /*shader*/ 2, /*material*/ 9, static_cast<i32>(i) });
    }

    CommandBucketConfig config;
    config.EnableSorting = true;
    config.EnableBatching = true;
    CommandBucket bucket(config);
    for (const Layer& layer : layers)
    {
        DrawMeshCommand cmd = MakeBlendedDraw(layer);
        cmd.vertexArrayID = TestHandle(100u);
        bucket.Submit(cmd, MakeTransparentMetadata(layer), m_Allocator.get());
    }
    bucket.BatchCommands(*m_Allocator);

    ASSERT_EQ(bucket.GetSortedCommands().size(), 1u)
        << "Coincident transparent draws should still collapse into one instanced draw";
    const CommandPacket* packet = bucket.GetSortedCommands()[0];
    ASSERT_EQ(packet->GetCommandType(), CommandType::DrawMeshInstanced);

    // Per-instance attributes must survive the reorder, in submission order.
    const auto* icmd = packet->GetCommandData<DrawMeshInstancedCommand>();
    ASSERT_EQ(icmd->instanceCount, kCount);
    ASSERT_NE(icmd->entityIDBufferOffset, UINT32_MAX);
    ASSERT_NE(icmd->colorBufferOffset, UINT32_MAX) << "Per-instance tint stream was dropped";
    FrameDataBuffer& frameBuffer = FrameDataBufferManager::Get();
    for (u32 i = 0; i < kCount; ++i)
    {
        const i32* id = frameBuffer.GetEntityIDPtr(icmd->entityIDBufferOffset + i);
        ASSERT_NE(id, nullptr);
        EXPECT_EQ(*id, static_cast<i32>(i)) << "Instance " << i << " is not in submission order";

        const glm::vec4* tint = frameBuffer.GetColorPtr(icmd->colorBufferOffset + i);
        ASSERT_NE(tint, nullptr);
        EXPECT_NEAR(tint->r, layers[i].m_Color.r, 1e-6f) << "Per-instance tint " << i << " was reassigned";
    }
}

TEST_F(TransparentDepthOrderingTest, ParallelSubmissionMatchesSerialSubmission)
{
    std::vector<Layer> layers;
    for (u32 i = 0; i < 12; ++i)
    {
        layers.push_back(Layer{ glm::vec3(0.05f * static_cast<f32>(i), 0.4f, 0.6f), 0.35f,
                                300u + i * 911u, 1u + (i % 3u), 1u + ((11u - i) % 5u),
                                static_cast<i32>(i) });
    }

    const std::vector<Layer> serial = SortedOrder(layers, /*enableBatching*/ false);

    // Same draws, handed to the bucket through the worker-slot path instead.
    CommandBucketConfig config;
    config.InitialCapacity = 256;
    CommandBucket bucket(config);
    bucket.SetAllocator(m_Allocator.get());
    bucket.PrepareForParallelSubmission();
    constexpr u32 kWorkers = 4;
    for (sizet i = 0; i < layers.size(); ++i)
    {
        PacketMetadata meta = MakeTransparentMetadata(layers[i]);
        CommandPacket* packet = m_Allocator->CreateCommandPacket(MakeBlendedDraw(layers[i]), meta);
        ASSERT_NE(packet, nullptr);
        bucket.SubmitPacketParallel(packet, static_cast<u32>(i) % kWorkers);
    }
    bucket.MergeThreadLocalCommands();
    bucket.SortCommands();
    const std::vector<Layer> parallel = ExecutionOrder(bucket, layers);

    ASSERT_EQ(parallel.size(), serial.size());
    for (sizet i = 0; i < parallel.size(); ++i)
    {
        EXPECT_EQ(parallel[i].m_Depth, serial[i].m_Depth)
            << "Parallel submission produced a different depth order at " << i
            << "\nserial   " << DescribeOrder(serial)
            << "\nparallel " << DescribeOrder(parallel);
    }
    ExpectColorNear(CompositeOver(parallel, kBlack), CompositeOver(serial, kBlack), "parallel vs serial composite");
}

// =============================================================================
// AC3 — conventional alpha, additive and OIT are different claims
// =============================================================================

TEST_F(TransparentDepthOrderingTest, AdditiveAccumulationIsOrderIndependentByConstruction)
{
    // Additive draws need no depth-major key: GL_ONE/GL_ONE accumulation
    // commutes. This is stated as a property of the operator, not tested as
    // though the sort had to fix it — and it is why RenderMode::Additive and
    // RenderMode::Subtractive keep the state-major layout.
    std::vector<Layer> layers;
    for (u32 i = 0; i < 5; ++i)
        layers.push_back(Layer{ glm::vec3(0.1f * static_cast<f32>(i), 0.2f, 0.3f), 0.5f, i * 100u, 1, 1, static_cast<i32>(i) });

    const glm::vec3 forward = CompositeAdd(layers, kBlack);
    std::vector<Layer> reversed(layers.rbegin(), layers.rend());
    ExpectColorNear(CompositeAdd(reversed, kBlack), forward, "additive must not depend on order");

    // …and the same permutation genuinely moves a source-over composite, so the
    // claim above is about the operator and not about the data being trivial.
    EXPECT_GT(glm::distance(CompositeOver(layers, kBlack), CompositeOver(reversed, kBlack)), 1e-3f);
}

TEST_F(TransparentDepthOrderingTest, ConventionalAlphaIsOrderedWhetherOrNotOITExists)
{
    // Blended meshes submitted to the scene bucket are conventional alpha
    // draws: the weighted-blended OIT targets are written only by the decal,
    // particle and groom passes (RendererSettings::OITEnabled), never by
    // SceneRenderPass. Their correctness is pinned separately in
    // PropertyTests/OITPropertyTests.cpp and cannot stand in for this one.
    const Layer redNear{ kRed, 0.5f, 100, 1, 1, 0 };
    const Layer blueFar{ kBlue, 0.5f, 900, 2, 2, 1 };

    const std::vector<Layer> order = SortedOrder({ redNear, blueFar }, /*enableBatching*/ false);
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order.front().m_EntityID, blueFar.m_EntityID) << DescribeOrder(order);
    ExpectColorNear(CompositeOver(order, kBlack), kExpectedComposite, DescribeOrder(order));

    // The key itself must say "conventional alpha", not "some blended thing".
    const DrawKey key = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, 1, 1, 100);
    EXPECT_EQ(key.GetRenderMode(), RenderMode::Transparent);
}

// =============================================================================
// AC4 — opaque stays state-major, dependencies stay valid, ties are defined
// =============================================================================

TEST_F(TransparentDepthOrderingTest, BothPayloadLayoutsArePinnedBitExactly)
{
    // The strongest statement available about "opaque sorting did not regress"
    // is that the sort sees BYTE-IDENTICAL keys and runs untouched code. A
    // timing on a Debug build cannot say that; this can. Hand-computed, so a
    // future layout change has to disagree with arithmetic rather than with a
    // value read back out of the same accessors that produced it.
    //
    //   [63:61] viewport 5 -> 0xA000'0000'0000'0000
    //   [60:58] layer                                  (TwoD = 1 / ThreeD = 0)
    //   [57:56] mode                                   (Opaque = 0 / Transparent = 1)
    const DrawKey opaque = DrawKey::CreateOpaque(5, ViewLayerType::TwoD, 0x1234, 0xABCD, 0x567890);
    EXPECT_EQ(opaque.GetKey(), 0xA41234ABCD567890ULL)
        << "The state-major payload moved: shader must stay at [55:40], material at [39:24], "
           "depth at [23:0].\n"
        << PrintKeyBits(opaque);

    // Transparent: depth (inverted: 0xFFFFFF - 100 = 0xFFFF9B) at [55:32],
    // shader at [31:16], material at [15:0].
    const DrawKey transparent = DrawKey::CreateTransparent(5, ViewLayerType::ThreeD, 0x1234, 0xABCD, 100);
    EXPECT_EQ(transparent.GetKey(), 0xA1FFFF9B1234ABCDULL)
        << "The depth-major payload moved.\n"
        << PrintKeyBits(transparent);

    // CreateCustom rides the opaque layout with priority in the depth field.
    EXPECT_EQ(DrawKey::CreateCustom(5, ViewLayerType::TwoD, 0x567890).GetKey(), 0xA400000000567890ULL);
}

TEST_F(TransparentDepthOrderingTest, SetRenderModeRepacksThePayload)
{
    // Changing the mode changes what the payload bits MEAN, so the mutators
    // would be order-independent only by accident without the re-pack.
    DrawKey key;
    key.SetShaderID(0x1234);
    key.SetMaterialID(0xABCD);
    key.SetDepth(0x567890);
    key.SetRenderMode(RenderMode::Transparent); // …set LAST, on purpose.

    EXPECT_EQ(key.GetRenderMode(), RenderMode::Transparent);
    EXPECT_EQ(key.GetShaderID(), 0x1234u) << PrintKeyBits(key);
    EXPECT_EQ(key.GetMaterialID(), 0xABCDu) << PrintKeyBits(key);
    EXPECT_EQ(key.GetDepth(), 0x567890u) << PrintKeyBits(key);
    // …and the bits really are in the transparent positions, not left behind.
    EXPECT_EQ(key.GetKey(), DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, 0x1234, 0xABCD,
                                                       0xFFFFFFu - 0x567890u)
                                .GetKey())
        << PrintKeyBits(key);

    // And back again, losing nothing.
    key.SetRenderMode(RenderMode::Opaque);
    EXPECT_EQ(key.GetShaderID(), 0x1234u) << PrintKeyBits(key);
    EXPECT_EQ(key.GetMaterialID(), 0xABCDu) << PrintKeyBits(key);
    EXPECT_EQ(key.GetDepth(), 0x567890u) << PrintKeyBits(key);
}

TEST_F(TransparentDepthOrderingTest, OpaqueSortingRemainsStateMajor)
{
    // The opaque half of the key layout must not move: opaque draws group by
    // shader, then material, and only then run front-to-back.
    std::vector<DrawKey> keys;
    auto rng = MakeTestRNG();
    std::uniform_int_distribution<u32> depthDist(0, 0xFFFFFF);
    for (u32 shader = 1; shader <= 4; ++shader)
        for (u32 material = 1; material <= 4; ++material)
            for (u32 i = 0; i < 4; ++i)
                keys.push_back(DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, shader, material, depthDist(rng)));
    std::ranges::shuffle(keys, rng);
    std::sort(keys.begin(), keys.end());

    for (sizet i = 1; i < keys.size(); ++i)
    {
        const bool shaderNonDecreasing = keys[i - 1].GetShaderID() <= keys[i].GetShaderID();
        EXPECT_TRUE(shaderNonDecreasing) << "Opaque keys are no longer shader-major at " << i;
        if (keys[i - 1].GetShaderID() == keys[i].GetShaderID())
        {
            EXPECT_LE(keys[i - 1].GetMaterialID(), keys[i].GetMaterialID())
                << "Opaque keys are no longer material-major within a shader at " << i;
            if (keys[i - 1].GetMaterialID() == keys[i].GetMaterialID())
            {
                EXPECT_LE(keys[i - 1].GetDepth(), keys[i].GetDepth())
                    << "Opaque draws are no longer front-to-back within a material at " << i;
            }
        }
    }
}

TEST_F(TransparentDepthOrderingTest, OpaqueAlwaysPrecedesTransparent)
{
    // Whatever the payload layout, the RenderMode bits still separate the two
    // partitions — this is what lets each half carry its own field order.
    const DrawKey opaqueFarthest = DrawKey::CreateOpaque(0, ViewLayerType::ThreeD, 0xFFFF, 0xFFFF, 0xFFFFFF);
    const DrawKey transparentNearest = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, 0, 0, 0xFFFFFF);
    EXPECT_LT(opaqueFarthest.GetKey(), transparentNearest.GetKey());
}

TEST_F(TransparentDepthOrderingTest, EqualDepthTiesBreakOnShaderThenMaterialThenSubmission)
{
    // "Deterministic ties" means specified, not merely repeatable.
    const DrawKey a = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, 2, 9, 500);
    const DrawKey b = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, 3, 1, 500);
    const DrawKey c = DrawKey::CreateTransparent(0, ViewLayerType::ThreeD, 2, 10, 500);
    EXPECT_LT(a.GetKey(), b.GetKey()) << "Equal depth must break on shader first";
    EXPECT_LT(a.GetKey(), c.GetKey()) << "Equal depth and shader must break on material";

    // Fully equal keys keep submission order: the bucket's radix sort is stable.
    std::vector<Layer> layers;
    for (u32 i = 0; i < 6; ++i)
        layers.push_back(Layer{ glm::vec3(0.1f), 0.5f, 777, 4, 4, static_cast<i32>(i) });
    const std::vector<Layer> order = SortedOrder(layers, /*enableBatching*/ false);
    ASSERT_EQ(order.size(), layers.size());
    for (sizet i = 0; i < order.size(); ++i)
        EXPECT_EQ(order[i].m_EntityID, static_cast<i32>(i)) << "Equal keys did not keep submission order";
}

TEST_F(TransparentDepthOrderingTest, DependencyConstrainedTransparentsAreNotReordered)
{
    // A packet that declares m_DependsOnPrevious pins itself behind its
    // predecessor; the sort must respect that even though depth says otherwise.
    const Layer first{ kBlue, 0.5f, 100, 1, 1, 0 }; // nearer
    const Layer second{ kRed, 0.5f, 900, 2, 2, 1 }; // farther, but constrained

    CommandBucketConfig config;
    config.EnableSorting = true;
    config.EnableBatching = false;
    CommandBucket bucket(config);
    bucket.Submit(MakeBlendedDraw(first), MakeTransparentMetadata(first), m_Allocator.get());
    PacketMetadata constrained = MakeTransparentMetadata(second);
    constrained.m_DependsOnPrevious = true;
    bucket.Submit(MakeBlendedDraw(second), constrained, m_Allocator.get());
    bucket.SortCommands();

    const std::vector<Layer> order = ExecutionOrder(bucket, { first, second });
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0].m_EntityID, first.m_EntityID)
        << "A declared dependency outranks depth order: " << DescribeOrder(order);
    EXPECT_EQ(order[1].m_EntityID, second.m_EntityID) << DescribeOrder(order);
}

// =============================================================================
// Object-level sorting cannot resolve intersecting geometry — documented, not
// solved (issue #1327 asks for the limitation to be stated).
// =============================================================================

TEST_F(TransparentDepthOrderingTest, IntersectingGeometryIsOutsideTheGuarantee)
{
    // Two surfaces whose bounding-sphere centres order one way while their
    // per-pixel depths order the other. The key sorts by object centre, so the
    // guarantee is per-object, not per-fragment; nothing here claims otherwise.
    const Layer nearCentreFarPixel{ kRed, 0.5f, 100, 1, 1, 0 };
    const Layer farCentreNearPixel{ kBlue, 0.5f, 900, 2, 2, 1 };

    const std::vector<Layer> order = SortedOrder({ nearCentreFarPixel, farCentreNearPixel }, false);
    ASSERT_EQ(order.size(), 2u);
    // Sorted strictly by the object-level depth key…
    EXPECT_EQ(order[0].m_EntityID, farCentreNearPixel.m_EntityID);
    // …which is the documented limit: a pixel where the two meshes interpenetrate
    // still blends in object order. Per-fragment resolution needs OIT.
    SUCCEED() << "Object-level sorting resolves object order only; see DrawKey.h.";
}

// OLO_TEST_LAYER: plumbing
#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "FrameDataBufferFixture.h"
#include "MockRendererAPI.h"
#include "RenderingTestUtils.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/Commands/CommandAllocator.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include "OloEngine/Renderer/Commands/CommandLifecycle.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstring>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

// The command lifecycle of issue #1335: prepare, freeze, replay, retire.
//
// Three groups of tests, one per acceptance criterion they back:
//   * Lifecycle: legitimate preparation (parallel submission, bone-offset
//     remapping, batching) passes untouched, and the first replay freezes.
//   * Negative controls: every way of mutating a frozen packet, a frozen bucket
//     or a published frame payload is reported, and where it happens before a
//     replay, that replay does not consume it.
//   * Replay: what the dispatcher CONSUMES after batching and freezing, captured
//     per packet, equals the unbatched sources field for field, and a parallel
//     replay on real threads consumes exactly what a serial one does.
//
// The capture replaces the dispatch function (through the packet resolver the
// engine installs at startup), so what is compared is precisely what
// CommandDispatch would read: the frozen command bytes plus the FrameDataBuffer
// ranges they point at, read through the same const accessors.

using namespace OloEngine;          // NOLINT(google-build-using-namespace) — test file
using namespace OloEngine::Testing; // NOLINT(google-build-using-namespace) — test file

namespace
{
    using Violation = CommandLifecycle::Violation;

    template<typename T>
    [[nodiscard]] bool SameBytes(const std::vector<T>& a, const std::vector<T>& b)
    {
        return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
    }

    // ——— What one replayed packet handed to the dispatcher ———

    struct ConsumedDraw
    {
        CommandType Type = CommandType::Invalid;
        RHI::ResourceHandle VertexArray{};
        u32 IndexCount = 0;
        u32 BaseIndex = 0;
        u16 MaterialDataIndex = 0;
        u16 RenderStateIndex = 0;
        u32 BoneCount = 0;
        std::vector<glm::mat4> Transforms;
        std::vector<glm::mat4> PrevTransforms;
        std::vector<i32> EntityIDs;
        std::vector<glm::vec4> Colors;
        std::vector<f32> Customs;
        std::vector<glm::vec4> LightmapRegions;
        std::vector<glm::mat4> Bones;
        std::vector<glm::mat4> PrevBones;
        u32 Calls = 0;

        [[nodiscard]] bool SameAs(const ConsumedDraw& o) const
        {
            return Type == o.Type && VertexArray == o.VertexArray && IndexCount == o.IndexCount &&
                   BaseIndex == o.BaseIndex && MaterialDataIndex == o.MaterialDataIndex &&
                   RenderStateIndex == o.RenderStateIndex && BoneCount == o.BoneCount &&
                   SameBytes(Transforms, o.Transforms) && SameBytes(PrevTransforms, o.PrevTransforms) &&
                   SameBytes(EntityIDs, o.EntityIDs) && SameBytes(Colors, o.Colors) && SameBytes(Customs, o.Customs) &&
                   SameBytes(LightmapRegions, o.LightmapRegions) && SameBytes(Bones, o.Bones) &&
                   SameBytes(PrevBones, o.PrevBones) && Calls == o.Calls;
        }
    };

    // One instance as the shader would see it, for comparing a batched replay
    // against the unbatched sources.
    struct ConsumedInstance
    {
        i32 EntityID = -1;
        glm::mat4 Transform{ 1.0f };
        glm::mat4 PrevTransform{ 1.0f };
        glm::vec4 Color{ 1.0f };
        f32 Custom = 0.0f;
        glm::vec4 LightmapRegion{ 0.0f };
        u16 MaterialDataIndex = 0;
        u16 RenderStateIndex = 0;
        RHI::ResourceHandle VertexArray{};
        std::vector<glm::mat4> Bones;
        std::vector<glm::mat4> PrevBones;
    };

    // The capture's slot table: packet data address -> slot. Built on the
    // test thread before a replay and only read during it.
    struct CaptureTable
    {
        std::unordered_map<const void*, sizet> SlotOf;
        std::vector<ConsumedDraw> Slots;
        std::atomic<u32> UnknownPackets{ 0 };
    };
    CaptureTable* s_Capture = nullptr;

    void AppendPalette(std::vector<glm::mat4>& out, u32 offset, u32 count)
    {
        if (count == 0)
            return;
        const glm::mat4* palette = FrameDataBufferManager::Get().GetBoneMatrixRange(offset, count);
        if (palette)
            out.assign(palette, palette + count);
    }

    void CaptureDrawMesh(const void* data, RendererAPI& /*api*/)
    {
        const auto it = s_Capture->SlotOf.find(data);
        if (it == s_Capture->SlotOf.end())
        {
            s_Capture->UnknownPackets.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const auto* cmd = static_cast<const DrawMeshCommand*>(data);
        ConsumedDraw& out = s_Capture->Slots[it->second];
        ++out.Calls;
        out.Type = CommandType::DrawMesh;
        out.VertexArray = cmd->vertexArrayID;
        out.IndexCount = cmd->indexCount;
        out.BaseIndex = cmd->baseIndex;
        out.MaterialDataIndex = cmd->materialDataIndex;
        out.RenderStateIndex = cmd->renderStateIndex;
        out.Transforms = { cmd->transform };
        out.PrevTransforms = { cmd->prevTransform };
        out.EntityIDs = { cmd->entityID };
        out.Colors = { cmd->color };
        out.Customs = { cmd->custom };
        out.LightmapRegions = { cmd->lightmapScaleOffset };
        if (cmd->isAnimatedMesh)
        {
            out.BoneCount = cmd->boneCount;
            AppendPalette(out.Bones, cmd->boneBufferOffset, cmd->boneCount);
            // UINT32_MAX aliases the current palette, as UploadBoneMatrices does.
            AppendPalette(out.PrevBones, cmd->prevBoneBufferOffset == UINT32_MAX ? cmd->boneBufferOffset : cmd->prevBoneBufferOffset,
                          cmd->boneCount);
        }
    }

    void CaptureDrawMeshInstanced(const void* data, RendererAPI& /*api*/)
    {
        const auto it = s_Capture->SlotOf.find(data);
        if (it == s_Capture->SlotOf.end())
        {
            s_Capture->UnknownPackets.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const auto* cmd = static_cast<const DrawMeshInstancedCommand*>(data);
        const FrameDataBuffer& frame = FrameDataBufferManager::Get();
        ConsumedDraw& out = s_Capture->Slots[it->second];
        ++out.Calls;
        out.Type = CommandType::DrawMeshInstanced;
        out.VertexArray = cmd->vertexArrayID;
        out.IndexCount = cmd->indexCount;
        out.BaseIndex = cmd->baseIndex;
        out.MaterialDataIndex = cmd->materialDataIndex;
        out.RenderStateIndex = cmd->renderStateIndex;

        // The dispatcher's own defaults for an absent stream (see
        // CommandDispatch::DrawMeshInstanced): prev aliases current, entity -1,
        // tint 1, custom 0, lightmap 0.
        const u32 n = cmd->transformCount;
        const glm::mat4* transforms = frame.GetTransformPtr(cmd->transformBufferOffset);
        const glm::mat4* prev = cmd->prevTransformBufferOffset != UINT32_MAX ? frame.GetTransformPtr(cmd->prevTransformBufferOffset) : transforms;
        const i32* ids = cmd->entityIDBufferOffset != UINT32_MAX ? frame.GetEntityIDPtr(cmd->entityIDBufferOffset) : nullptr;
        const glm::vec4* colors = cmd->colorBufferOffset != UINT32_MAX ? frame.GetColorPtr(cmd->colorBufferOffset) : nullptr;
        const f32* customs = cmd->customBufferOffset != UINT32_MAX ? frame.GetCustomPtr(cmd->customBufferOffset) : nullptr;
        const glm::vec4* lightmaps = cmd->lightmapRegionBufferOffset != UINT32_MAX ? frame.GetColorPtr(cmd->lightmapRegionBufferOffset) : nullptr;
        out.Transforms.clear();
        out.PrevTransforms.clear();
        out.EntityIDs.clear();
        out.Colors.clear();
        out.Customs.clear();
        out.LightmapRegions.clear();
        for (u32 i = 0; i < n && transforms; ++i)
        {
            out.Transforms.push_back(transforms[i]);
            out.PrevTransforms.push_back(prev ? prev[i] : transforms[i]);
            out.EntityIDs.push_back(ids ? ids[i] : -1);
            out.Colors.push_back(colors ? colors[i] : glm::vec4(1.0f));
            out.Customs.push_back(customs ? customs[i] : 0.0f);
            out.LightmapRegions.push_back(lightmaps ? lightmaps[i] : glm::vec4(0.0f));
        }
        if (cmd->isAnimatedMesh)
        {
            out.BoneCount = cmd->boneCountPerInstance;
            AppendPalette(out.Bones, cmd->boneBufferOffset, cmd->boneCountPerInstance);
            AppendPalette(out.PrevBones, cmd->prevBoneBufferOffset == UINT32_MAX ? cmd->boneBufferOffset : cmd->prevBoneBufferOffset,
                          cmd->boneCountPerInstance);
        }
    }

    CommandDispatchFn CaptureResolver(CommandType type)
    {
        switch (type)
        {
            case CommandType::DrawMesh:
                return &CaptureDrawMesh;
            case CommandType::DrawMeshInstanced:
                return &CaptureDrawMeshInstanced;
            default:
                return nullptr;
        }
    }

    // Every draw is safe to replay on a worker in this synthetic frame.
    u32 CaptureClassifier(const CommandPacket& packet)
    {
        if (packet.GetCommandType() == CommandType::DrawMeshInstanced)
            return std::max(1u, packet.GetCommandData<DrawMeshInstancedCommand>()->transformCount);
        return 1u;
    }

    // Installs the capture as the process-wide resolver and classifier for the
    // scope, restoring whatever the engine (or an earlier test) had installed.
    class ScopedCaptureHooks
    {
      public:
        explicit ScopedCaptureHooks(CaptureTable& table)
            : m_PrevResolver(CommandPacket::GetDispatchResolver()), m_PrevClassifier(CommandBucket::GetParallelReplayClassifier())
        {
            s_Capture = &table;
            CommandPacket::SetDispatchResolver(&CaptureResolver);
            CommandBucket::SetParallelReplayClassifier(&CaptureClassifier);
        }
        ~ScopedCaptureHooks()
        {
            CommandPacket::SetDispatchResolver(m_PrevResolver);
            CommandBucket::SetParallelReplayClassifier(m_PrevClassifier);
            s_Capture = nullptr;
        }
        ScopedCaptureHooks(const ScopedCaptureHooks&) = delete;
        ScopedCaptureHooks& operator=(const ScopedCaptureHooks&) = delete;
        ScopedCaptureHooks(ScopedCaptureHooks&&) = delete;
        ScopedCaptureHooks& operator=(ScopedCaptureHooks&&) = delete;

      private:
        CommandPacket::DispatchResolverFn m_PrevResolver;
        CommandBucket::ParallelReplayClassifier m_PrevClassifier;
    };

    // RecordParallel on real threads, one per item, all released together —
    // the shape of the Vulkan backend's parallel recording without a device.
    class ThreadedReplayAPI final : public MockRendererAPI
    {
      public:
        void RecordParallel(u32 itemCount, const std::function<void(u32 item)>& body, u32 /*instanceCapacity*/) override
        {
            ItemsLastRegion = itemCount;
            if (itemCount == 0)
                return;
            std::barrier start(static_cast<std::ptrdiff_t>(itemCount));
            std::vector<std::jthread> workers;
            workers.reserve(itemCount);
            for (u32 item = 0; item < itemCount; ++item)
            {
                workers.emplace_back([&, item]
                                     {
                    start.arrive_and_wait();
                    body(item); });
            }
        }
        u32 ItemsLastRegion = 0;
    };

    // Build the capture table for a frozen span: one slot per packet, keyed
    // by the address of its (frozen) command bytes.
    void PrepareCapture(CaptureTable& table, std::span<const CommandPacket* const> packets)
    {
        table.SlotOf.clear();
        table.Slots.assign(packets.size(), ConsumedDraw{});
        table.UnknownPackets.store(0);
        for (sizet i = 0; i < packets.size(); ++i)
            table.SlotOf.emplace(packets[i]->GetRawCommandData(), i);
    }

    // Flatten what a replay consumed into per-instance records, ordered by
    // entity id so a batched and an unbatched replay of the same frame line up.
    std::vector<ConsumedInstance> FlattenInstances(const std::vector<ConsumedDraw>& draws)
    {
        std::vector<ConsumedInstance> out;
        for (const ConsumedDraw& draw : draws)
        {
            for (sizet i = 0; i < draw.Transforms.size(); ++i)
            {
                ConsumedInstance inst;
                inst.EntityID = draw.EntityIDs[i];
                inst.Transform = draw.Transforms[i];
                inst.PrevTransform = draw.PrevTransforms[i];
                inst.Color = draw.Colors[i];
                inst.Custom = draw.Customs[i];
                inst.LightmapRegion = draw.LightmapRegions[i];
                inst.MaterialDataIndex = draw.MaterialDataIndex;
                inst.RenderStateIndex = draw.RenderStateIndex;
                inst.VertexArray = draw.VertexArray;
                inst.Bones = draw.Bones;
                inst.PrevBones = draw.PrevBones;
                out.push_back(std::move(inst));
            }
        }
        std::ranges::sort(out, {}, &ConsumedInstance::EntityID);
        return out;
    }

    // ——— The synthetic frame ———
    //
    // Submitted from REAL worker threads through the same parallel path
    // Renderer3D uses (worker-local bone scratch, TLS bucket slots, merge,
    // bone-offset remap), so the lifecycle is exercised on the preparation it
    // has to allow, not on a hand-assembled bucket.

    constexpr u32 kWorkers = 4;
    constexpr u32 kStaticGroups = 24;   // batchable geometry/material groups...
    constexpr u32 kStaticPerGroup = 12; // ...of this many draws each
    constexpr u32 kPoses = 3;           // skinned draws share one of three poses...
    constexpr u32 kSkinnedPerPose = 6;  // ...six actors each
    constexpr u32 kSingles = 150;       // unique geometry: never batched
    constexpr u32 kBones = 6;

    [[nodiscard]] glm::mat4 PoseBone(u32 pose, u32 bone, bool previous)
    {
        return glm::translate(glm::mat4(1.0f), glm::vec3(static_cast<f32>(pose) + (previous ? 0.25f : 0.0f), static_cast<f32>(bone), 3.0f));
    }

    struct FrameSource
    {
        DrawMeshCommand Command{};
        PacketMetadata Meta;
        bool Skinned = false;
        u32 Pose = 0;
    };

    std::vector<FrameSource> MakeFrameSources()
    {
        std::vector<FrameSource> sources;
        i32 entity = 1000;
        const auto base = [&](u32 vao, u16 material, u16 state)
        {
            FrameSource src;
            src.Command = MakeSyntheticDrawMeshCommand(1, material, 0.5f, entity);
            src.Command.vertexArrayID = TestHandle(vao);
            src.Command.materialDataIndex = material;
            src.Command.renderStateIndex = state;
            const f32 e = static_cast<f32>(entity);
            src.Command.transform = glm::translate(glm::mat4(1.0f), glm::vec3(e, 1.0f, 2.0f));
            src.Command.prevTransform = glm::translate(glm::mat4(1.0f), glm::vec3(e - 0.5f, 1.0f, 2.0f));
            src.Meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 1u + material % 7u, material, static_cast<u32>(entity) * 7u % 0xFFFFFFu);
            ++entity;
            return src;
        };

        for (u32 g = 0; g < kStaticGroups; ++g)
        {
            for (u32 i = 0; i < kStaticPerGroup; ++i)
            {
                FrameSource src = base(100u + g, static_cast<u16>(10 + g), static_cast<u16>(g % 3));
                // Optional per-instance data on some sources only, so a batch
                // mixes set and default values in the same stream.
                if (i % 3 == 0)
                    src.Command.color = glm::vec4(0.1f * static_cast<f32>(i), 0.5f, 0.25f, 1.0f);
                if (i % 4 == 1)
                    src.Command.custom = 0.125f * static_cast<f32>(i);
                if (i % 5 == 2)
                    src.Command.lightmapScaleOffset = glm::vec4(0.5f, 0.5f, 0.01f * static_cast<f32>(i), 0.25f);
                sources.push_back(src);
            }
        }
        for (u32 pose = 0; pose < kPoses; ++pose)
        {
            for (u32 i = 0; i < kSkinnedPerPose; ++i)
            {
                FrameSource src = base(500u, 90, 1);
                src.Skinned = true;
                src.Pose = pose;
                sources.push_back(src);
            }
        }
        for (u32 i = 0; i < kSingles; ++i)
            sources.push_back(base(1000u + i, static_cast<u16>(200 + i), 0));
        return sources;
    }

    // Submit every source through the parallel path from kWorkers threads,
    // then run the main-thread half of EndParallelSubmission.
    void SubmitFrameInParallel(CommandBucket& bucket, std::span<const FrameSource> sources,
                               std::vector<std::unique_ptr<CommandAllocator>>& workerAllocators)
    {
        FrameDataBuffer& frame = FrameDataBufferManager::Get();
        frame.PrepareForParallelSubmission();
        bucket.PrepareForParallelSubmission();

        workerAllocators.clear();
        for (u32 w = 0; w < kWorkers; ++w)
            workerAllocators.push_back(std::make_unique<CommandAllocator>());

        std::barrier start(static_cast<std::ptrdiff_t>(kWorkers));
        {
            std::vector<std::jthread> workers;
            for (u32 w = 0; w < kWorkers; ++w)
            {
                workers.emplace_back([&, w]
                                     {
                    start.arrive_and_wait();
                    CommandAllocator& allocator = *workerAllocators[w];
                    for (sizet s = w; s < sources.size(); s += kWorkers)
                    {
                        DrawMeshCommand cmd = sources[s].Command;
                        if (sources[s].Skinned)
                        {
                            std::array<glm::mat4, kBones> current{};
                            std::array<glm::mat4, kBones> previous{};
                            for (u32 b = 0; b < kBones; ++b)
                            {
                                current[b] = PoseBone(sources[s].Pose, b, false);
                                previous[b] = PoseBone(sources[s].Pose, b, true);
                            }
                            const u32 cur = frame.AllocateBoneMatricesParallel(w, kBones);
                            frame.WriteBoneMatricesParallel(w, cur, current.data(), kBones);
                            const u32 prev = frame.AllocateBoneMatricesParallel(w, kBones);
                            frame.WriteBoneMatricesParallel(w, prev, previous.data(), kBones);
                            cmd.isAnimatedMesh = true;
                            cmd.boneCount = kBones;
                            cmd.boneBufferOffset = cur;
                            cmd.prevBoneBufferOffset = prev;
                            cmd.workerIndex = static_cast<u8>(w);
                            cmd.needsBoneOffsetRemap = true;
                        }
                        CommandPacket* packet = allocator.CreateCommandPacket(cmd, sources[s].Meta);
                        bucket.SubmitPacketParallel(packet, w);
                    } });
            }
        }

        // EndParallelSubmission's order: scratch merge, command merge, remap.
        frame.MergeScratchBuffers();
        bucket.MergeThreadLocalCommands();
        bucket.RemapBoneOffsets(frame);
    }
} // namespace

// =============================================================================
// Fixture: validation on, violation counters observed per test
// =============================================================================

class CommandLifecycleTest : public FrameDataBufferFixture
{
  protected:
    void SetUp() override
    {
        FrameDataBufferFixture::SetUp();
        m_PrevValidation = CommandLifecycle::IsValidationEnabled();
        CommandLifecycle::SetValidationEnabled(true);
        for (u32 k = 0; k < static_cast<u32>(Violation::Count); ++k)
            m_Baseline[k] = CommandLifecycle::GetViolationCount(static_cast<Violation>(k));
    }

    void TearDown() override
    {
        CommandLifecycle::SetValidationEnabled(m_PrevValidation);
        FrameDataBufferFixture::TearDown();
    }

    [[nodiscard]] u64 Raised(Violation v) const
    {
        return CommandLifecycle::GetViolationCount(v) - m_Baseline[static_cast<u32>(v)];
    }

    [[nodiscard]] u64 RaisedTotal() const
    {
        u64 total = 0;
        for (u32 k = 0; k < static_cast<u32>(Violation::Count); ++k)
            total += Raised(static_cast<Violation>(k));
        return total;
    }

    // One ordinary DrawMesh submitted to `bucket`, with the packet returned
    // so a test can hold on to it past the freeze. It carries its own no-op
    // dispatch function: the process-wide resolver may be the real
    // CommandDispatch, installed by an earlier renderer test in this process.
    CommandPacket* SubmitOne(CommandBucket& bucket, i32 entity = 7)
    {
        auto cmd = MakeSyntheticDrawMeshCommand(1, 1, 0.5f, entity);
        PacketMetadata meta;
        meta.m_SortKey = MakeSyntheticOpaqueKey(0, ViewLayerType::ThreeD, 1, 1, static_cast<u32>(entity));
        CommandPacket* packet = bucket.Submit(cmd, meta, &m_Allocator);
        if (packet)
            packet->SetDispatchFunction([](const void*, RendererAPI&) {});
        return packet;
    }

    CommandAllocator m_Allocator;
    MockRendererAPI m_API;

  private:
    bool m_PrevValidation = false;
    std::array<u64, static_cast<sizet>(Violation::Count)> m_Baseline{};
};

// =============================================================================
// Lifecycle: legitimate preparation is untouched, the first replay freezes
// =============================================================================

TEST_F(CommandLifecycleTest, ParallelSubmissionRemapAndBatchingAreLegalPreparation)
{
    CaptureTable table;
    ScopedCaptureHooks hooks(table);

    CommandBucket bucket;
    bucket.SetAllocator(&m_Allocator);
    const auto sources = MakeFrameSources();
    std::vector<std::unique_ptr<CommandAllocator>> workerAllocators;
    SubmitFrameInParallel(bucket, sources, workerAllocators);
    ASSERT_EQ(bucket.GetCommandCount(), sources.size());

    bucket.BatchCommands(m_Allocator);
    EXPECT_FALSE(bucket.IsFrozen()) << "Batching is preparation: it must not freeze the bucket";
    EXPECT_EQ(bucket.GetStatistics().SkinnedBatchUnremapped, 0u) << "RemapBoneOffsets did not reach every skinned draw";
    EXPECT_EQ(bucket.GetStatistics().SkinnedBatchGroups, kPoses);
    for (const CommandPacket* packet : bucket.GetPackets())
        EXPECT_FALSE(packet->IsFrozen());

    PrepareCapture(table, bucket.GetPackets());
    bucket.ExecuteParallel(m_API);

    EXPECT_TRUE(bucket.IsFrozen());
    for (const CommandPacket* packet : bucket.GetPackets())
        EXPECT_TRUE(packet->IsFrozen());
    EXPECT_EQ(table.UnknownPackets.load(), 0u);
    EXPECT_EQ(RaisedTotal(), 0u) << "Legitimate preparation raised a lifecycle violation";
    EXPECT_EQ(FrameDataBufferManager::Get().GetPublishedTransformCount(), FrameDataBufferManager::Get().GetTransformCount())
        << "The first replay must publish every transform allocated before it";
}

TEST_F(CommandLifecycleTest, ReplayingAFrozenBucketAgainIsLegal)
{
    CommandBucket bucket;
    bucket.SetAllocator(&m_Allocator);
    for (i32 i = 0; i < 8; ++i)
        (void)SubmitOne(bucket, i);
    bucket.SortCommands();

    // Depth prepass + colour pass + a reflection or overdraw replay: the
    // same bucket, several times, with a no-op sort request in between.
    bucket.Execute(m_API);
    bucket.SortCommands();
    bucket.ExecuteParallel(m_API);
    bucket.Execute(m_API);

    EXPECT_TRUE(bucket.IsFrozen());
    EXPECT_EQ(RaisedTotal(), 0u);
}

TEST_F(CommandLifecycleTest, ClearRetiresTheBucketForTheNextFrame)
{
    CommandBucket bucket;
    bucket.SetAllocator(&m_Allocator);
    (void)SubmitOne(bucket);
    bucket.Execute(m_API);
    ASSERT_TRUE(bucket.IsFrozen());

    bucket.Reset(m_Allocator);
    FrameDataBufferManager::Get().Reset();
    EXPECT_FALSE(bucket.IsFrozen());
    EXPECT_EQ(FrameDataBufferManager::Get().GetPublishedTransformCount(), 0u);

    CommandPacket* next = SubmitOne(bucket);
    EXPECT_NE(next, nullptr) << "A retired bucket must accept the next frame's packets";
    EXPECT_FALSE(next->IsFrozen());
    EXPECT_EQ(bucket.GetCommandCount(), 1u);
    EXPECT_EQ(RaisedTotal(), 0u);
}

TEST_F(CommandLifecycleTest, ACloneOfAFrozenPacketIsPreparationAndLeavesTheOriginalAlone)
{
    CommandBucket bucket;
    bucket.SetAllocator(&m_Allocator);
    CommandPacket* original = SubmitOne(bucket);
    bucket.Execute(m_API);
    ASSERT_TRUE(original->IsFrozen());

    // DecalRenderPass's OIT variant: copy the frozen packet, edit the copy.
    const CommandPacket* frozen = original;
    CommandPacket* variant = frozen->Clone(m_Allocator);
    ASSERT_NE(variant, nullptr);
    EXPECT_FALSE(variant->IsFrozen());
    variant->GetCommandData<DrawMeshCommand>()->entityID = 99;

    EXPECT_EQ(frozen->GetCommandData<DrawMeshCommand>()->entityID, 7);
    EXPECT_TRUE(frozen->MatchesFrozenContent());
    EXPECT_EQ(RaisedTotal(), 0u);
}

// =============================================================================
// Negative controls: a mutation after freeze is detected before replay uses it
// =============================================================================

TEST_F(CommandLifecycleTest, NegativeControl_MutableAccessorOnAFrozenPacketIsReported)
{
    CommandBucket bucket;
    bucket.SetAllocator(&m_Allocator);
    CommandPacket* packet = SubmitOne(bucket);
    bucket.Freeze();

    const CommandLifecycle::ScopedExpectedViolations expected;
    (void)packet->GetCommandData<DrawMeshCommand>();
    (void)packet->GetRawCommandData();
    packet->SetMetadata(packet->GetMetadata());
    EXPECT_EQ(Raised(Violation::PacketMutatedAfterFreeze), 3u);

    // The const view reads without a report.
    const CommandPacket* view = packet;
    (void)view->GetCommandData<DrawMeshCommand>();
    EXPECT_EQ(Raised(Violation::PacketMutatedAfterFreeze), 3u);
}

// The write the accessor check cannot see: a pointer taken during preparation
// and written through after the freeze. Validation's digest catches it on the
// calling thread, and the parallel replay that would have read it never starts.
TEST_F(CommandLifecycleTest, NegativeControl_WriteThroughAPreparationPointerIsCaughtBeforeParallelReplay)
{
    CaptureTable table;
    ScopedCaptureHooks hooks(table);

    CommandBucket bucket;
    bucket.SetAllocator(&m_Allocator);
    std::vector<DrawMeshCommand*> held;
    for (i32 i = 0; i < 96; ++i)
        held.push_back(SubmitOne(bucket, i)->GetCommandData<DrawMeshCommand>());
    bucket.SortCommands();
    bucket.Freeze();

    held[40]->transform[3][0] = 1.0e6f; // the stale write

    PrepareCapture(table, bucket.GetPackets());
    ThreadedReplayAPI threaded;
    {
        const CommandLifecycle::ScopedExpectedViolations expected;
        bucket.ExecuteParallel(threaded);
    }

    EXPECT_EQ(Raised(Violation::PacketChangedAfterFreeze), 1u);
    EXPECT_EQ(threaded.ItemsLastRegion, 0u) << "No worker may start once a frozen packet is found changed";
    for (const ConsumedDraw& draw : table.Slots)
        EXPECT_EQ(draw.Calls, 0u) << "A packet was replayed although the frozen span failed validation";
    EXPECT_EQ(bucket.GetStatistics().DrawCalls, 0u);
}

// A write that lands WHILE a replay runs cannot be stopped before the fact;
// it is reported as soon as every worker has finished.
TEST_F(CommandLifecycleTest, NegativeControl_WriteDuringReplayIsReportedWhenTheReplayEnds)
{
    CommandBucket bucket;
    bucket.SetAllocator(&m_Allocator);
    CommandPacket* victim = SubmitOne(bucket, 1);
    static DrawMeshCommand* s_VictimCommand = nullptr;
    s_VictimCommand = victim->GetCommandData<DrawMeshCommand>();

    // The first packet's dispatch writes into the second packet — a racing
    // writer, made deterministic by running in the serial replay.
    CommandPacket* writer = SubmitOne(bucket, 0);
    writer->SetDispatchFunction([](const void*, RendererAPI&)
                                { s_VictimCommand->custom = 42.0f; });
    victim->SetDispatchFunction([](const void*, RendererAPI&) {});

    {
        const CommandLifecycle::ScopedExpectedViolations expected;
        bucket.Execute(m_API);
    }
    EXPECT_EQ(Raised(Violation::PacketChangedDuringReplay), 1u);
    s_VictimCommand = nullptr;
}

TEST_F(CommandLifecycleTest, NegativeControl_BucketOperationsAfterFreezeAreRefused)
{
    CommandBucket bucket;
    bucket.SetAllocator(&m_Allocator);
    CommandPacket* loose = m_Allocator.CreateCommandPacket(MakeSyntheticDrawMeshCommand(), PacketMetadata{});
    for (i32 i = 0; i < 4; ++i)
        (void)SubmitOne(bucket, i);
    bucket.Freeze();

    const CommandLifecycle::ScopedExpectedViolations expected;
    EXPECT_EQ(SubmitOne(bucket), nullptr);
    bucket.AddCommand(loose);
    bucket.SubmitPacket(loose);
    bucket.SortCommands(); // unsorted and frozen: reordering would be an edit
    bucket.BatchCommands(m_Allocator);
    bucket.RemapBoneOffsets(FrameDataBufferManager::Get());
    bucket.PrepareForParallelSubmission();
    bucket.MergeThreadLocalCommands();

    EXPECT_EQ(Raised(Violation::BucketMutatedAfterFreeze), 8u);
    EXPECT_EQ(bucket.GetCommandCount(), 4u) << "A refused submission still changed the bucket";
    EXPECT_FALSE(bucket.IsSorted());
    EXPECT_FALSE(bucket.IsBatched());
}

TEST_F(CommandLifecycleTest, NegativeControl_ConstReplayOfAnUnfrozenBucketIsRefused)
{
    CommandBucket bucket;
    bucket.SetAllocator(&m_Allocator);
    (void)SubmitOne(bucket);

    const CommandLifecycle::ScopedExpectedViolations expected;
    const auto stats = bucket.ReplayRange(m_API, 0, 1);
    EXPECT_EQ(stats.DrawCalls, 0u);
    EXPECT_EQ(Raised(Violation::ReplayOfUnfrozenBucket), 1u);
}

TEST_F(CommandLifecycleTest, NegativeControl_WriteIntoAPublishedFramePayloadIsRefused)
{
    FrameDataBuffer& frame = FrameDataBufferManager::Get();
    const glm::mat4 original = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 3.0f));
    const u32 offset = frame.AllocateTransforms(1);
    ASSERT_NE(offset, UINT32_MAX);
    frame.WriteTransforms(offset, &original, 1);
    const u32 ids = frame.AllocateEntityIDs(1);
    const i32 id = 5;
    frame.WriteEntityIDs(ids, &id, 1);

    // Any replay publishes the frame's payloads.
    CommandBucket bucket;
    bucket.SetAllocator(&m_Allocator);
    (void)SubmitOne(bucket);
    bucket.Execute(m_API);
    ASSERT_GT(frame.GetPublishedTransformCount(), offset);

    {
        const CommandLifecycle::ScopedExpectedViolations expected;
        const glm::mat4 clobber(0.0f);
        const i32 otherID = -3;
        frame.WriteTransforms(offset, &clobber, 1);
        frame.WriteEntityIDs(ids, &otherID, 1);
    }
    EXPECT_EQ(Raised(Violation::PayloadWrittenAfterPublish), 2u);
    EXPECT_TRUE(Math::BitwiseEqual(*frame.GetTransformPtr(offset), original)) << "A refused write still reached the payload";
    EXPECT_EQ(*frame.GetEntityIDPtr(ids), id);

    // Appending after the publication is how a later pass batches: legal.
    const u32 fresh = frame.AllocateTransforms(1);
    ASSERT_NE(fresh, UINT32_MAX);
    const glm::mat4 appended = glm::translate(glm::mat4(1.0f), glm::vec3(9.0f));
    frame.WriteTransforms(fresh, &appended, 1);
    EXPECT_TRUE(Math::BitwiseEqual(*frame.GetTransformPtr(fresh), appended));
    EXPECT_EQ(Raised(Violation::PayloadWrittenAfterPublish), 2u);
}

// What Release catches with validation off: every accessor-level mutation, and
// no content check. Pinned so the difference between the two modes is stated
// by a test rather than only by a comment.
TEST_F(CommandLifecycleTest, WithValidationOffOnlyAccessorLevelMutationIsSeen)
{
    CommandLifecycle::SetValidationEnabled(false);
    CommandBucket bucket;
    bucket.SetAllocator(&m_Allocator);
    CommandPacket* packet = SubmitOne(bucket);
    DrawMeshCommand* held = packet->GetCommandData<DrawMeshCommand>();
    bucket.Freeze();

    held->custom = 3.0f;
    EXPECT_TRUE(packet->MatchesFrozenContent()) << "With validation off no digest is recorded, so none can fail";

    const CommandLifecycle::ScopedExpectedViolations expected;
    (void)packet->GetCommandData<DrawMeshCommand>();
    EXPECT_EQ(Raised(Violation::PacketMutatedAfterFreeze), 1u);
}

// =============================================================================
// Replay: batching preserves every stream, parallel replay equals serial
// =============================================================================

// Batching collapses N sources into one instanced packet. After it is frozen
// and replayed, what the dispatcher consumes per instance must be each
// source's own current and previous transform, picking id, material and
// render state, bone palettes, and optional tint / custom / lightmap region —
// exactly what an unbatched replay of the same frame consumes.
TEST_F(CommandLifecycleTest, BatchingPreservesEveryPerInstanceStreamThroughFreezeAndReplay)
{
    CaptureTable table;
    ScopedCaptureHooks hooks(table);
    const auto sources = MakeFrameSources();

    const auto replayFrame = [&](bool batching)
    {
        FrameDataBufferManager::Get().Reset();
        CommandAllocator allocator;
        CommandBucketConfig config;
        config.EnableBatching = batching;
        CommandBucket bucket(config);
        bucket.SetAllocator(&allocator);
        std::vector<std::unique_ptr<CommandAllocator>> workerAllocators;
        SubmitFrameInParallel(bucket, sources, workerAllocators);
        bucket.BatchCommands(allocator);
        bucket.SortCommands();
        bucket.Freeze();
        PrepareCapture(table, bucket.GetPackets());
        bucket.Execute(m_API);
        EXPECT_EQ(table.UnknownPackets.load(), 0u);
        return std::pair{ FlattenInstances(table.Slots), bucket.GetCommandCount() };
    };

    const auto [unbatched, unbatchedPackets] = replayFrame(false);
    const auto [batched, batchedPackets] = replayFrame(true);

    ASSERT_EQ(unbatchedPackets, sources.size());
    // Every static group and every pose collapses to one packet; singles stay.
    EXPECT_EQ(batchedPackets, static_cast<sizet>(kStaticGroups + kPoses + kSingles));
    ASSERT_EQ(batched.size(), unbatched.size()) << "Batching lost or duplicated instances";

    for (sizet i = 0; i < unbatched.size(); ++i)
    {
        const ConsumedInstance& want = unbatched[i];
        const ConsumedInstance& got = batched[i];
        ASSERT_EQ(got.EntityID, want.EntityID) << "picking id";
        EXPECT_TRUE(Math::BitwiseEqual(got.Transform, want.Transform)) << "current transform, entity " << want.EntityID;
        EXPECT_TRUE(Math::BitwiseEqual(got.PrevTransform, want.PrevTransform)) << "previous transform, entity " << want.EntityID;
        EXPECT_TRUE(Math::BitwiseEqual(got.Color, want.Color)) << "tint, entity " << want.EntityID;
        EXPECT_TRUE(Math::BitwiseEqual(got.Custom, want.Custom)) << "custom, entity " << want.EntityID;
        EXPECT_TRUE(Math::BitwiseEqual(got.LightmapRegion, want.LightmapRegion)) << "lightmap region, entity " << want.EntityID;
        EXPECT_EQ(got.MaterialDataIndex, want.MaterialDataIndex) << "material, entity " << want.EntityID;
        EXPECT_EQ(got.RenderStateIndex, want.RenderStateIndex) << "render state, entity " << want.EntityID;
        EXPECT_EQ(got.VertexArray, want.VertexArray) << "geometry, entity " << want.EntityID;
        EXPECT_TRUE(SameBytes(got.Bones, want.Bones)) << "current bone palette, entity " << want.EntityID;
        EXPECT_TRUE(SameBytes(got.PrevBones, want.PrevBones)) << "previous bone palette, entity " << want.EntityID;
    }

    // The skinned sources must actually carry their pose, or the comparison
    // above would pass on two empty palettes.
    const auto skinned = std::ranges::count_if(batched, [](const ConsumedInstance& inst)
                                               { return inst.Bones.size() == kBones && inst.PrevBones.size() == kBones; });
    EXPECT_EQ(static_cast<u32>(skinned), kPoses * kSkinnedPerPose);
    EXPECT_EQ(RaisedTotal(), 0u);
}

// The same frozen frame replayed serially and on real worker threads (the
// ThreadedReplayAPI forks one thread per item, as Vulkan parallel recording
// does) consumes identical bytes, each packet exactly once. Run under the
// Sanitizers workflow's TSan job this is also the data-race check on replay.
TEST_F(CommandLifecycleTest, SerialAndParallelReplayOfAFrozenFrameAreEquivalent)
{
    CaptureTable table;
    ScopedCaptureHooks hooks(table);
    const auto sources = MakeFrameSources();

    CommandBucket bucket;
    bucket.SetAllocator(&m_Allocator);
    std::vector<std::unique_ptr<CommandAllocator>> workerAllocators;
    SubmitFrameInParallel(bucket, sources, workerAllocators);
    bucket.BatchCommands(m_Allocator);
    bucket.Freeze();
    const auto packets = bucket.GetPackets();
    PrepareCapture(table, packets);

    // Serial reference.
    bucket.Execute(m_API);
    const std::vector<ConsumedDraw> serial = table.Slots;
    const auto serialStats = bucket.GetStatistics();

    for (u32 run = 0; run < 3; ++run)
    {
        table.Slots.assign(packets.size(), ConsumedDraw{});
        ThreadedReplayAPI threaded;
        bucket.ExecuteParallel(threaded, 8u);
        ASSERT_GE(threaded.ItemsLastRegion, 2u) << "The parallel replay did not fork; the comparison would be serial against serial";

        EXPECT_EQ(table.UnknownPackets.load(), 0u);
        EXPECT_EQ(bucket.GetStatistics().DrawCalls, serialStats.DrawCalls);
        for (sizet i = 0; i < packets.size(); ++i)
        {
            EXPECT_EQ(serial[i].Calls, 1u) << "serial replay, packet " << i;
            EXPECT_TRUE(table.Slots[i].SameAs(serial[i])) << "parallel run " << run << " consumed different bytes for packet " << i;
        }
    }
    EXPECT_EQ(RaisedTotal(), 0u);
}

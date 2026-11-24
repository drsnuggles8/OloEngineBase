// OLO_TEST_LAYER: L4
#include "OloEnginePCH.h"
#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/Crowd/CrowdCull.h"
#include "OloEngine/Renderer/Crowd/CrowdGeometry.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <array>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        class CrowdCullTest : public RendererAttachedTest
        {
          protected:
            bool RunsInVulkanProcess() const override
            {
                return true;
            }

            void BuildScene() override
            {
                EnableRendering(256, 256);
                auto camera = GetScene().CreateEntity("Orthographic crowd camera");
                auto& lens = camera.AddComponent<CameraComponent>();
                lens.Primary = true;
                lens.Camera.SetOrthographic(32.0f, 0.1f, 100.0f);
            }
        };

        InstanceData Placed(u64 id, f32 scale, f32 depth, f32 phase)
        {
            InstanceData row;
            row.StableID = id;
            row.Custom = phase;
            row.Transform = glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -depth)), glm::vec3(scale));
            row.PrevTransform = row.Transform;
            return row;
        }

        InstanceData ReadMember(const CrowdCull& culler, u32 tier, u32 row = 0)
        {
            InstanceData member;
            RenderCommand::ReadBufferSubData(culler.Instances(tier), sizeof(member) * row, sizeof(member), &member);
            return member;
        }

        void ExpectCount(const CrowdCull& culler, u32 tier, CrowdDrawRange range, u32 count)
        {
            std::array<u32, 5> args{};
            RenderCommand::ReadBufferSubData(culler.Indirect(tier), 0u, sizeof(args), args.data());
            EXPECT_EQ(args, (std::array<u32, 5>{ range.Count, count, range.FirstIndex, 0u, 0u })) << "tier " << tier;
        }
    } // namespace

    TEST_F(CrowdCullTest, OrthographicCohortsRetainTheirPreviousPoseAcrossTierChanges)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        CrowdCull culler;
        CrowdClip clip;
        clip.FrameCount = 17;
        clip.BoneCount = 1;
        clip.Atlas.SetNum(17);
        for (u32 frame = 0; frame < clip.FrameCount; ++frame)
            clip.Atlas[frame].Transform[3].x = static_cast<f32>(frame);

        const std::array<CrowdDrawRange, 3> ranges{ { { 81, 6 }, { 45, 9 }, { 3, 12 } } };
        const glm::vec4 sphere{ 0.0f, 0.0f, 0.0f, 1.0f };
        std::array<InstanceData, 4> members{
            Placed(10, 3.0f, 45.0f, 0.0f), Placed(20, 1.0f, 5.0f, 0.0f),
            Placed(30, 0.5f, 45.0f, 0.5f), Placed(40, 3.0f, -8.0f, 0.0f)
        };
        constexpr f32 first = 0.21875f; // full frame 3.5, coarse frame 1.75
        auto dispatch = [&](f32 now, f32 previous, bool reset, bool loop = true)
        {
            RunFrames(1); // establishes the culling camera and advances physical frame serial
            bool ran = false;
            RunInFrame([&]
                       { ran = culler.Dispatch(members, clip, now, previous, loop, reset, {}, sphere, sphere, ranges); });
            return ran;
        };
        ASSERT_TRUE(dispatch(first, first, true));
        // Radius one gives 16 px anywhere in this orthographic view. The
        // larger member stays near even though it is nine times farther away.
        constexpr u32 fullBase = 5u; // four instance slots + encoded one
        const u32 coarseBase = fullBase + clip.FrameCount;
        const std::array<u32, 3> addresses{ fullBase + 3u, coarseBase + 1u, coarseBase + 5u };
        const std::array<u32, 3> sourceFrames{ 3u, 2u, 10u };
        for (u32 tier = 0; tier < 3; ++tier)
        {
            ExpectCount(culler, tier, ranges[tier], 1u);
            const auto member = ReadMember(culler, tier);
            EXPECT_EQ(member.StableID, members[tier].StableID);
            const u32 address = addresses[tier];
            EXPECT_EQ(member.CrowdPose.x, address);
            EXPECT_EQ(member.CrowdPose.y, address);
            EXPECT_EQ(member.CrowdPose.w, 1u);
            EXPECT_EQ(member.CrowdPose.z & 0xffffu, tier == 0u ? 32768u : 49151u);
            EXPECT_EQ(member.CrowdPose.z >> 16u, member.CrowdPose.z & 0xffffu);
            EXPECT_FLOAT_EQ(ReadMember(culler, tier, address - 1u).Transform[3].x, static_cast<f32>(sourceFrames[tier]));
            const u32 nextSource = sourceFrames[tier] + (tier == 0u ? 1u : 2u);
            EXPECT_FLOAT_EQ(ReadMember(culler, tier, address).Transform[3].x, static_cast<f32>(nextSource));
        }

        // The former near actor crosses both tier thresholds in one frame.
        // The pose that generated its previous pixels used interpolation.
        members[0] = Placed(10, 0.4f, 45.0f, 0.25f);
        members[2] = Placed(30, 3.0f, 45.0f, 0.5f); // far to near, with a coarse previous pose
        // Change only the high half of this slot's identity. The newcomer
        // must have zero bone velocity even at the same position.
        members[1].StableID += (u64{ 1 } << 32u);
        members[1].PrevTransform[3].x = 2.5f;
        members[1].CrowdPose.y = 1u; // arrived from another clip, with genuine owner-level model history
        ASSERT_TRUE(dispatch(0.25f, first, false));
        ExpectCount(culler, 0, ranges[0], 1u);
        ExpectCount(culler, 1, ranges[1], 1u);
        ExpectCount(culler, 2, ranges[2], 1u);
        const auto newcomer = ReadMember(culler, 1);
        EXPECT_EQ(newcomer.CrowdPose.x, newcomer.CrowdPose.y);
        EXPECT_FLOAT_EQ(newcomer.PrevTransform[3].x, 2.5f);
        const auto switched = ReadMember(culler, 2);
        ASSERT_EQ(switched.StableID, 10u);
        EXPECT_EQ(switched.CrowdPose.x, coarseBase + 4u);
        EXPECT_EQ(switched.CrowdPose.y, fullBase + 3u);
        EXPECT_EQ(switched.CrowdPose.z >> 16u, 32768u);
        const auto approaching = ReadMember(culler, 0);
        EXPECT_EQ(approaching.CrowdPose.x, fullBase + 12u);
        EXPECT_EQ(approaching.CrowdPose.y, coarseBase + 5u);
        EXPECT_EQ(approaching.CrowdPose.z >> 16u, 49151u);

        // Every tier must hold the authored last pose of a stopped, non-looping clip.
        ASSERT_TRUE(dispatch(1.0f, 1.0f, true, false));
        for (u32 tier = 0; tier < 3; ++tier)
        {
            const auto member = ReadMember(culler, tier);
            const u32 terminal = tier == 0u ? fullBase + 16u : coarseBase + 8u;
            EXPECT_EQ(member.CrowdPose.x, terminal);
            EXPECT_EQ(member.CrowdPose.y, terminal);
            EXPECT_EQ(member.CrowdPose.z, 0u);
            EXPECT_FLOAT_EQ(ReadMember(culler, tier, terminal - 1u).Transform[3].x, 16.0f);
        }
    }

    TEST_F(CrowdCullTest, BakedVerticesUseThePriorSamplingRateAcrossTierChanges)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        CrowdCull culler;
        CrowdClip clip;
        clip.BoneCount = 1;
        clip.FrameCount = 17;
        clip.Atlas.SetNum(17);
        std::array<CrowdVertexFrames, 3> surfaces;
        for (u32 tier = 0; tier < surfaces.size(); ++tier)
        {
            auto& surface = surfaces[tier];
            surface.RecordsPerFrame = 2;
            surface.Records.SetNum(2 * (tier == 0 ? 17 : 9));
            for (i32 row = 0; row < surface.Records.Num(); row += 2)
                surface.Records[row].CrowdPose = { 40u + tier, 2u, 0u, 0u };
        }
        const std::array<CrowdDrawRange, 3> ranges{ { { 3, 0 }, { 3, 0 }, { 3, 0 } } };
        const glm::vec4 sphere{ 0.0f, 0.0f, 0.0f, 1.0f };
        std::array members{ Placed(10, 3.0f, 45.0f, 0.0f) };
        auto dispatch = [&](bool reset)
        {
            RunFrames(1);
            bool ran = false;
            RunInFrame([&]
                       { ran = culler.Dispatch(members, clip, 0.25f, 0.25f, true, reset, {}, sphere, sphere, ranges, surfaces); });
            return ran;
        };
        ASSERT_TRUE(dispatch(true));
        constexpr u32 tag = 0x80000000u;
        const auto near = ReadMember(culler, 0);
        ASSERT_NE(near.CrowdPose.x & tag, 0u);
        EXPECT_EQ(ReadMember(culler, 0, (near.CrowdPose.x & ~tag) - 1u).CrowdPose, glm::uvec4(40, 2, 0, 0));
        members[0] = Placed(10, 0.4f, 45.0f, 0.0f);
        ASSERT_TRUE(dispatch(false));
        const auto far = ReadMember(culler, 2);
        ASSERT_NE(far.CrowdPose.x & tag, 0u);
        EXPECT_EQ(ReadMember(culler, 2, (far.CrowdPose.x & ~tag) - 1u).CrowdPose, glm::uvec4(42, 2, 0, 0));
        // The previous pose came from the full-rate near palette: four
        // frames after the single input record, plus the address sentinel.
        EXPECT_EQ(far.CrowdPose.y, 6u);
        ASSERT_TRUE(dispatch(false));
        EXPECT_EQ(ReadMember(culler, 2).CrowdPose.y, far.CrowdPose.x);
    }

    TEST_F(CrowdCullTest, RetainedMemberKeepsHistoryWhenClipGroupRowsChange)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        CrowdCull culler;
        CrowdClip clip;
        clip.FrameCount = 17;
        clip.BoneCount = 1;
        clip.Atlas.SetNum(17);
        const std::array<CrowdDrawRange, 3> ranges{ { { 81, 6 }, { 45, 9 }, { 3, 12 } } };
        const glm::vec4 sphere{ 0.0f, 0.0f, 0.0f, 1.0f };
        const auto a = Placed(101, 2.25f, 10.0f, 0.125f);
        const auto b = Placed(202, 0.5f, 15.0f, 0.0f);
        const auto c = Placed(303, 0.5f, 20.0f, 0.0f);
        const auto d = Placed(404, 0.5f, 25.0f, 0.0f);
        auto dispatch = [&](const std::vector<InstanceData>& members, f32 now, f32 previous)
        {
            RunFrames(1);
            bool ran = false;
            RunInFrame([&]
                       { ran = culler.Dispatch(members, clip, now, previous, true, false, {}, sphere, sphere, ranges); });
            return ran;
        };
        constexpr f32 first = 0.21875f;
        ASSERT_TRUE(dispatch({ a, b, c }, first, first));
        ExpectCount(culler, 0, ranges[0], 1u);

        // A moves into the hysteresis band (30.4 px). Its prior near tier,
        // phase offset and transform must survive both growth and row 0 -> 2.
        auto moved = Placed(101, 1.9f, 10.0f, 0.25f);
        moved.PrevTransform = a.Transform;
        moved.Transform[3].x = 3.0f;
        ASSERT_TRUE(dispatch({ c, d, moved, b }, 0.25f, first));
        ExpectCount(culler, 0, ranges[0], 1u);
        const auto afterGrowth = ReadMember(culler, 0);
        ASSERT_EQ(afterGrowth.StableID, moved.StableID);
        constexpr u32 fullBase = 6u + 1u;                  // growth from three prefix records to six
        EXPECT_EQ(afterGrowth.CrowdPose.y, fullBase + 5u); // (first + A's old offset) * 16 = 5.5
        EXPECT_EQ(afterGrowth.CrowdPose.z >> 16u, 32768u);
        EXPECT_FLOAT_EQ(afterGrowth.PrevTransform[3].x, 0.0f);
        EXPECT_FLOAT_EQ(afterGrowth.Transform[3].x, 3.0f);

        // Two actors leave the clip and the survivors swap order. A has to
        // read old row 2 while old row 0 still belongs to C.
        moved.PrevTransform = moved.Transform;
        moved.Transform[3].x = 5.0f;
        ASSERT_TRUE(dispatch({ moved, c }, 0.375f, 0.25f));
        ExpectCount(culler, 0, ranges[0], 1u);
        const auto afterRemoval = ReadMember(culler, 0);
        ASSERT_EQ(afterRemoval.StableID, moved.StableID);
        EXPECT_EQ(afterRemoval.CrowdPose.y, fullBase + 8u);
        EXPECT_EQ(afterRemoval.CrowdPose.x, fullBase + 10u);
        EXPECT_FLOAT_EQ(afterRemoval.PrevTransform[3].x, 3.0f);
        EXPECT_FLOAT_EQ(afterRemoval.Transform[3].x, 5.0f);

        RunFrames(1); // A missed physical frame invalidates even matching IDs.
        ASSERT_TRUE(dispatch({ moved, c }, 0.5f, 0.375f));
        ExpectCount(culler, 0, ranges[0], 0u);
        ExpectCount(culler, 1, ranges[1], 1u);
        const auto afterGap = ReadMember(culler, 1);
        EXPECT_EQ(afterGap.CrowdPose.y, afterGap.CrowdPose.x);
        EXPECT_FLOAT_EQ(afterGap.PrevTransform[3].x, afterGap.Transform[3].x);
    }

    TEST_F(CrowdCullTest, TwoFrameCoarseClipInterpolatesEveryEighthAndUsesThePreviousLoopMode)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        CrowdClip clip;
        clip.FrameCount = 2;
        clip.BoneCount = 4; // all three matrix lanes plus a second record
        const u32 stride = clip.RecordsPerFrame();
        clip.Atlas.SetNum(clip.FrameCount * stride);
        clip.Atlas[stride].Transform[3].x = 8.0f;
        clip.Atlas[stride].Normal[3].y = -16.0f;
        clip.Atlas[stride].PrevTransform[3].z = 4.0f;
        clip.Atlas[stride + 1u].Transform[3].x = 24.0f;
        CrowdCull culler;
        const std::array members{ Placed(202, 1.0f, 15.0f, 0.75f) };
        const std::array<CrowdDrawRange, 3> ranges{ { { 81, 6 }, { 45, 9 }, { 3, 12 } } };
        const glm::vec4 sphere{ 0.0f, 0.0f, 0.0f, 1.0f };
        auto dispatch = [&](bool loop)
        {
            RunFrames(1);
            bool ran = false;
            RunInFrame([&]
                       { ran = culler.Dispatch(members, clip, 0.25f, 0.25f, loop, false, {}, sphere, sphere, ranges); });
            return ran;
        };
        ASSERT_TRUE(dispatch(true));
        ExpectCount(culler, 1, ranges[1], 1u);
        constexpr u32 coarseBase = 6u; // one member + four full-rate records + encoded one
        for (u32 i = 0; i <= 8u; ++i)
        {
            const u32 row = coarseBase - 1u + i * stride;
            const auto sample = ReadMember(culler, 1, row);
            const f32 eighth = static_cast<f32>(i);
            EXPECT_FLOAT_EQ(sample.Transform[3].x, eighth);
            EXPECT_FLOAT_EQ(sample.Normal[3].y, -2.0f * eighth);
            EXPECT_FLOAT_EQ(sample.PrevTransform[3].z, 0.5f * eighth);
            EXPECT_FLOAT_EQ(ReadMember(culler, 1, row + 1u).Transform[3].x, 3.0f * eighth);
        }
        constexpr u32 terminal = coarseBase + 8u * 2u;
        // Clock + offset = one cycle. Loop mode alone decides whether the
        // vertices use the first or last pose, independently in each frame.
        EXPECT_EQ(ReadMember(culler, 1).CrowdPose.x, coarseBase);
        ASSERT_TRUE(dispatch(false));
        const auto held = ReadMember(culler, 1);
        EXPECT_EQ(held.CrowdPose.x, terminal);
        EXPECT_EQ(held.CrowdPose.y, coarseBase);
        ASSERT_TRUE(dispatch(true));
        const auto wrapped = ReadMember(culler, 1);
        EXPECT_EQ(wrapped.CrowdPose.x, coarseBase);
        EXPECT_EQ(wrapped.CrowdPose.y, terminal);
    }
} // namespace OloEngine::Tests

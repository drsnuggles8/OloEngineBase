// OLO_TEST_LAYER: L4
#include "OloEnginePCH.h"
#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/Crowd/CrowdCull.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <array>
#include <glm/gtc/matrix_transform.hpp>

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
        ASSERT_TRUE(dispatch(0.25f, first, false));
        ExpectCount(culler, 0, ranges[0], 1u);
        ExpectCount(culler, 1, ranges[1], 1u);
        ExpectCount(culler, 2, ranges[2], 1u);
        const auto newcomer = ReadMember(culler, 1);
        EXPECT_EQ(newcomer.CrowdPose.x, newcomer.CrowdPose.y);
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
} // namespace OloEngine::Tests

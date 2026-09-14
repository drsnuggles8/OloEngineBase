// =============================================================================
// SkinnedCrowdBatchingVisualEvidenceTest.cpp
//
// Visual evidence for skinned auto-batching (issue #1031): a crowd of skinned
// actors that share a pose must collapse into one instanced draw AND render the
// same pixels as the one-draw-per-actor path it replaces.
//
// The scene is six copies of MeshPrimitives::CreateMultiBoneAnimatedCube() in a
// row, all playing one shared swing clip. Each carries its OWN Skeleton
// instance — not a shared Ref — because batching is decided on palette CONTENT,
// and giving every actor its own skeleton object is what proves that rather
// than accidental pointer identity. Three actors run the clip from its origin
// and three from a third of the way in, so the frame contains two pose groups
// and the partitioning is exercised rather than merely the happy path.
//
// Three renders, not two, and that is the point:
//
//   unbatched          — the reference image
//   unbatched (again)  — the CONTROL, which measures how much this pipeline
//                        moves between two runs of identical input
//   batched            — the arm under test
//
// The assertion is that batched-vs-reference differs no more than
// control-vs-reference. A two-render A/B would have to invent a tolerance and
// would then be measuring the tolerance; with a control, the noise floor is
// whatever the renderer actually produces on the day.
//
// The crowd is captured MID-MOTION, and it has to be driven by a playing CLIP
// to be so. Scene::OnUpdateRuntime advances the bone history at the top of the
// frame, before anything renders, so a pose written from the test between
// frames is already in `prev` by the time the draw happens and every arm emits
// exactly zero velocity — on which the motion-vector comparison would pass
// even if batching dropped the previous palette outright. A clip is sampled
// inside the tick, after the history advance, so prev and current genuinely
// differ and the comparison has something to compare.
//
// PNGs land in OloEditor/assets/tests/visual/SkinnedCrowd_*.png as evidence for
// a human to look at. Like the SkeletalDeform_* captures they are deliberately
// not in the RMSE golden set: the contract worth regressing on is the
// batched == unbatched relationship asserted here, not the exact pixels of a
// procedural cube.
//
// Classification: L8 / integration (full GL pipeline + readback + PNG).
//
// OLO_TEST_LAYER: integration
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include "OloEngine/Renderer/GBuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 800;
        constexpr u32 kHeight = 400;

        // Three actors per pose group, two groups. Small enough to stay a fast
        // test, large enough that a batch is unambiguously a batch.
        constexpr u32 kGroupSize = 3;
        constexpr u32 kActorCount = kGroupSize * 2;

        // The crowd plays ONE clip. Group A starts at the clip's origin and
        // group B a third of the way in, so the two groups are in different
        // poses while each group stays exactly in phase with itself — which is
        // what a crowd of background actors on a shared cycle looks like, and
        // what the batcher is supposed to exploit.
        constexpr f32 kClipDuration = 4.0f;
        constexpr f32 kPhaseA = 0.0f;
        constexpr f32 kPhaseB = kClipDuration / 3.0f;

        // Frames each arm runs after re-seeding the clock. Enough that the bone
        // history has advanced past the deliberate reset and every skeleton is
        // in steady motion when the last frame is captured.
        constexpr u32 kSettleFrames = 4;

        // A rotation clip on the animated cube's upper bone. Built inline
        // rather than taken from Functional/Helpers/AnimationFixtures.h: that
        // header is for helpers shared by three or more Functional files, and
        // this one is bound to the bone names of
        // MeshPrimitives::CreateMultiBoneAnimatedCube ("Lower" / "Upper").
        [[nodiscard]] Ref<AnimationClip> MakeUpperBoneSwingClip()
        {
            auto clip = Ref<AnimationClip>::Create();
            clip->Name = "SkinnedCrowd_UpperSwing";
            clip->Duration = kClipDuration;

            BoneAnimation bone;
            bone.BoneName = "Upper";
            // Three rotation keys so the bone is moving at EVERY sampled time
            // rather than pausing at a turning point — a frame captured at a
            // stationary instant would emit zero velocity and make the motion
            // comparison vacuous again.
            bone.RotationKeys.push_back({ 0.0, glm::angleAxis(-0.6f, glm::vec3(0.0f, 0.0f, 1.0f)) });
            bone.RotationKeys.push_back({ static_cast<f64>(kClipDuration) * 0.5, glm::angleAxis(0.6f, glm::vec3(0.0f, 0.0f, 1.0f)) });
            bone.RotationKeys.push_back({ static_cast<f64>(kClipDuration), glm::angleAxis(-0.6f, glm::vec3(0.0f, 0.0f, 1.0f)) });
            bone.PositionKeys.push_back({ 0.0, glm::vec3(0.0f) });
            bone.PositionKeys.push_back({ static_cast<f64>(kClipDuration), glm::vec3(0.0f) });
            bone.ScaleKeys.push_back({ 0.0, glm::vec3(1.0f) });
            bone.ScaleKeys.push_back({ static_cast<f64>(kClipDuration), glm::vec3(1.0f) });
            clip->BoneAnimations.push_back(std::move(bone));
            clip->InitializeBoneCache();
            return clip;
        }

        // Mean absolute per-channel difference between two RGBA8 images.
        [[nodiscard]] f64 MeanAbsDifference(const std::vector<u8>& lhs, const std::vector<u8>& rhs)
        {
            if (lhs.size() != rhs.size() || lhs.empty())
                return std::numeric_limits<f64>::infinity();

            f64 total = 0.0;
            for (std::size_t i = 0; i < lhs.size(); ++i)
                total += std::abs(static_cast<f64>(lhs[i]) - static_cast<f64>(rhs[i]));
            return total / static_cast<f64>(lhs.size());
        }

        // Mean absolute difference over the VELOCITY lanes (.rg) of two RGBA
        // float readbacks of an RG16F target. See PeakVelocity for why .ba are
        // skipped rather than averaged in.
        [[nodiscard]] f64 MeanAbsVelocityDifference(const std::vector<f32>& lhs, const std::vector<f32>& rhs)
        {
            if (lhs.size() != rhs.size() || lhs.empty())
                return std::numeric_limits<f64>::infinity();

            f64 total = 0.0;
            std::size_t counted = 0;
            for (std::size_t texel = 0; texel * 4u + 1u < lhs.size(); ++texel)
            {
                for (std::size_t channel = 0; channel < 2u; ++channel)
                {
                    const std::size_t i = texel * 4u + channel;
                    total += std::abs(static_cast<f64>(lhs[i]) - static_cast<f64>(rhs[i]));
                    ++counted;
                }
            }
            return counted == 0 ? std::numeric_limits<f64>::infinity() : total / static_cast<f64>(counted);
        }

        // The velocity target is RG16F but comes back through an RGBA float
        // readback, so every texel carries a synthesized B of 0 and A of 1.
        // Those two lanes are not velocity: scanning them would make the
        // "something moved" guard below return >= 1.0 on an all-zero buffer and
        // never fail, which is exactly the vacuous check this test exists to
        // avoid. Only .rg is read, here and in the comparison above.
        [[nodiscard]] f32 PeakVelocity(const std::vector<f32>& rgba)
        {
            f32 peak = 0.0f;
            for (std::size_t texel = 0; texel * 4u + 1u < rgba.size(); ++texel)
            {
                for (std::size_t channel = 0; channel < 2u; ++channel)
                {
                    const f32 value = rgba[texel * 4u + channel];
                    if (std::isfinite(value))
                        peak = std::max(peak, std::abs(value));
                }
            }
            return peak;
        }
    } // namespace

    class SkinnedCrowdBatchingScene : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            {
                Entity camera = scene.CreateEntity("Camera");
                camera.GetComponent<TransformComponent>().Translation = { 0.0f, 1.5f, 14.0f };
                auto& cameraComp = camera.AddComponent<CameraComponent>();
                cameraComp.Primary = true;
                cameraComp.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);
            }

            {
                Entity sun = scene.CreateEntity("Sun");
                auto& light = sun.AddComponent<DirectionalLightComponent>();
                light.m_Direction = glm::normalize(glm::vec3(0.35f, -0.75f, -0.55f));
                light.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                light.m_Intensity = 4.0f;
                light.m_CastShadows = false;
            }

            m_SkinnedMesh = MeshPrimitives::CreateMultiBoneAnimatedCube();
            ASSERT_TRUE(m_SkinnedMesh) << "the skinned primitive failed to build";
            const Skeleton* sourceSkeleton = m_SkinnedMesh->GetMeshSource()->GetSkeleton();
            ASSERT_TRUE(sourceSkeleton) << "the skinned primitive carries no skeleton";

            // ONE clip shared by the whole crowd. Two actors at the same clip
            // time then sample byte-identical palettes without anything
            // declaring that they should -- which is the content-addressed
            // sharing this feature is built on, not an authored archetype.
            m_Clip = MakeUpperBoneSwingClip();

            for (u32 actor = 0; actor < kActorCount; ++actor)
            {
                Entity entity = scene.CreateEntity("CrowdActor" + std::to_string(actor));
                auto& transform = entity.GetComponent<TransformComponent>();
                transform.Translation = { (static_cast<f32>(actor) - 2.5f) * 2.6f, 0.0f, 0.0f };

                entity.AddComponent<MeshComponent>(m_SkinnedMesh->GetMeshSource());

                // One material for the whole crowd. Batching keys on the
                // material data index, so a per-actor material would separate
                // them before the pose ever mattered.
                auto& material = entity.AddComponent<MaterialComponent>();
                material.m_Material.SetBaseColorFactor(glm::vec4(0.85f, 0.3f, 0.2f, 1.0f));

                // A skeleton OBJECT per actor, holding identical matrices for
                // the actors in one group. Sharing one Ref across the group
                // would let a pointer comparison pass this test.
                Ref<Skeleton> skeleton = Ref<Skeleton>::Create(sourceSkeleton->m_LocalTransforms.size());
                static_cast<SkeletonData&>(*skeleton) = static_cast<const SkeletonData&>(*sourceSkeleton);
                entity.AddComponent<SkeletonComponent>(skeleton);
                m_Skeletons.push_back(skeleton);

                // The pose MUST be produced inside the tick, not written from
                // the test between frames. Scene::OnUpdateRuntime advances the
                // bone history at the top of the frame, before anything renders,
                // so a palette written from outside is already in `prev` by the
                // time the draw happens and the frame emits exactly zero
                // velocity -- which would make the motion-vector half of this
                // test pass on two empty buffers. A playing clip is sampled
                // after the history advance, so prev and current genuinely
                // differ.
                auto& animation = entity.AddComponent<AnimationStateComponent>();
                animation.m_CurrentClip = m_Clip;
                animation.m_CurrentTime = (actor < kGroupSize) ? kPhaseA : kPhaseB;
                animation.m_IsPlaying = true;
                m_Animations.push_back(entity);
            }
        }

        /// Put every actor's clip clock back on its group's phase and drop the
        /// bone history, so each arm starts from exactly the same state.
        void RewindCrowd()
        {
            for (u32 actor = 0; actor < m_Animations.size(); ++actor)
            {
                auto& animation = m_Animations[actor].GetComponent<AnimationStateComponent>();
                animation.m_CurrentTime = (actor < kGroupSize) ? kPhaseA : kPhaseB;
                animation.m_IsPlaying = true;
                m_Skeletons[actor]->ResetBoneHistory();
            }
        }

        [[nodiscard]] static SceneRenderPass* GeometryPass()
        {
            // The geometry render-stream node IS the SceneRenderPass — the pass
            // that owns the bucket BatchCommands runs on, and the G-Buffer the
            // velocity target lives in (Renderer3DInternal::GetRenderStreamNode).
            return static_cast<SceneRenderPass*>(Renderer3D::GetRenderStreamNode(Renderer3D::RenderStreamType::Geometry));
        }

        struct ArmResult
        {
            std::vector<u8> m_Color;
            std::vector<f32> m_Velocity;
            CommandBucket::Statistics m_BucketStats;
        };

        /// Render the crowd once, with the geometry bucket's batcher forced on
        /// or off, and capture what the frame produced.
        ///
        /// Every arm rewinds the clip clocks to the same phases and runs the
        /// same number of frames at the harness's fixed timestep, so the three
        /// arms end on the same clip time and any difference between them is
        /// the batcher's doing and nothing else.
        void RenderArm(bool enableBatching, ArmResult& out)
        {
            SceneRenderPass* geometry = GeometryPass();
            ASSERT_TRUE(geometry) << "no geometry render-stream node";

            CommandBucket& bucket = geometry->GetCommandBucket();
            if (!m_SavedBucketConfig)
                m_SavedBucketConfig = bucket.GetConfig();

            CommandBucketConfig config = bucket.GetConfig();
            config.EnableBatching = enableBatching;
            bucket.SetConfig(config);

            RewindCrowd();
            RunFrames(kSettleFrames);

            out.m_BucketStats = bucket.GetStatistics();

            u32 width = 0;
            u32 height = 0;
            ASSERT_TRUE(ReadbackComposite(out.m_Color, width, height)) << "ReadbackComposite failed";
            ASSERT_EQ(out.m_Color.size(), static_cast<std::size_t>(width) * height * 4u);

            const Ref<GBuffer>& gbuffer = geometry->GetGBuffer();
            ASSERT_TRUE(gbuffer) << "the deferred G-Buffer is absent; velocity cannot be inspected";
            const u32 velocityID = gbuffer->GetColorAttachmentID(GBuffer::Velocity);
            ASSERT_NE(velocityID, 0u) << "the G-Buffer carries no velocity attachment";
            ReadbackRgbaFloat(velocityID, width, height, out.m_Velocity);
        }

        void WritePng(const std::string& tag, const std::vector<u8>& pixels) const
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / ("SkinnedCrowd_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight),
                                               4, pixels.data(), static_cast<int>(kWidth) * 4);
            EXPECT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";
        }

        void TearDown() override
        {
            // The bucket belongs to the renderer singleton and
            // CommandBucket::Reset does not clear its config, so an arm that
            // dies on a fatal assertion would otherwise leave auto-batching
            // disabled for every later renderer test in this binary.
            if (m_SavedBucketConfig)
            {
                if (SceneRenderPass* geometry = GeometryPass())
                    geometry->GetCommandBucket().SetConfig(*m_SavedBucketConfig);
                m_SavedBucketConfig.reset();
            }
            RendererAttachedTest::TearDown();
        }

        std::optional<CommandBucketConfig> m_SavedBucketConfig;
        Ref<Mesh> m_SkinnedMesh;
        Ref<AnimationClip> m_Clip;
        std::vector<Ref<Skeleton>> m_Skeletons;
        std::vector<Entity> m_Animations;
    };

    TEST_F(SkinnedCrowdBatchingScene, SamePoseCrowdCollapsesAndRendersIdentically)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Deferred, because the velocity target this test reads is the
        // G-Buffer's RT3. The fixture restores RendererSettings in TearDown.
        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        ArmResult reference;
        ASSERT_NO_FATAL_FAILURE(RenderArm(/*enableBatching=*/false, reference));

        ArmResult control;
        ASSERT_NO_FATAL_FAILURE(RenderArm(/*enableBatching=*/false, control));

        ArmResult batched;
        ASSERT_NO_FATAL_FAILURE(RenderArm(/*enableBatching=*/true, batched));

        WritePng("Unbatched", reference.m_Color);
        WritePng("Batched", batched.m_Color);

        // The measurement, printed rather than only asserted: this is the
        // number the issue's "collapse identical-pose actors into one draw"
        // criterion is about, and a reader of the test log should be able to
        // see it without re-deriving it from the assertions. The bucket's
        // statistics are per-batching-pass, so these are single-frame figures.
        std::cout << "[ SKINNED  ] crowd of " << kActorCount << " in 2 poses - geometry-bucket draw calls: "
                  << reference.m_BucketStats.DrawCalls << " unbatched vs "
                  << batched.m_BucketStats.DrawCalls << " batched; "
                  << batched.m_BucketStats.SkinnedBatchGroups << " pose group(s), "
                  << batched.m_BucketStats.SkinnedBatchedCommands << " source draw(s) collapsed" << std::endl;

        EXPECT_LT(batched.m_BucketStats.DrawCalls, reference.m_BucketStats.DrawCalls)
            << "batching the crowd did not reduce the geometry bucket's draw calls";

        // 1. The crowd actually batched, and into TWO groups — one per pose.
        //    The unbatched arm must show none, or the toggle did nothing and
        //    every comparison below is vacuous.
        EXPECT_EQ(reference.m_BucketStats.SkinnedBatchedCommands, 0u)
            << "the batcher was disabled yet skinned draws still collapsed";
        EXPECT_GE(batched.m_BucketStats.SkinnedBatchGroups, 2u)
            << "the two pose groups did not each form a batch";
        EXPECT_GE(batched.m_BucketStats.SkinnedBatchedCommands, kActorCount - 2u)
            << "fewer skinned draws collapsed than the crowd has duplicates";
        EXPECT_EQ(batched.m_BucketStats.SkinnedBatchUnremapped, 0u)
            << "a skinned draw reached the batcher with worker-local bone offsets";

        // 2. Non-vacuity: something rendered, and it was moving.
        const f32 referenceVelocityPeak = PeakVelocity(reference.m_Velocity);
        const f32 batchedVelocityPeak = PeakVelocity(batched.m_Velocity);
        ASSERT_GT(referenceVelocityPeak, 0.0f)
            << "the unbatched reference emitted no motion at all; the capture is not mid-motion "
               "and the ghosting check below would pass on two empty buffers";

        // 3. The images agree, to within what this pipeline does to itself
        //    between two identical runs.
        const f64 controlColorDelta = MeanAbsDifference(reference.m_Color, control.m_Color);
        const f64 batchedColorDelta = MeanAbsDifference(reference.m_Color, batched.m_Color);
        EXPECT_LE(batchedColorDelta, controlColorDelta + 1.0)
            << "batched crowd renders differently from the one-draw-per-actor path "
            << "(batched delta " << batchedColorDelta << " vs control " << controlColorDelta
            << "); compare SkinnedCrowd_Batched.png against SkinnedCrowd_Unbatched.png";

        // 4. And so do the motion vectors. This is the ghosting guard: a batch
        //    that lost its previous-pose palette emits zero velocity, which
        //    shows up here as a delta near the reference's own peak while the
        //    control stays near zero.
        const f64 controlVelocityDelta = MeanAbsVelocityDifference(reference.m_Velocity, control.m_Velocity);
        const f64 batchedVelocityDelta = MeanAbsVelocityDifference(reference.m_Velocity, batched.m_Velocity);
        EXPECT_LE(batchedVelocityDelta, controlVelocityDelta + 1e-4)
            << "batched skinned draws emit different motion vectors from unbatched ones "
            << "(batched delta " << batchedVelocityDelta << " vs control " << controlVelocityDelta
            << ", reference peak " << referenceVelocityPeak << ", batched peak " << batchedVelocityPeak
            << ") — this is what ghosting under TAA looks like before it reaches the screen";
        EXPECT_GT(batchedVelocityPeak, 0.0f)
            << "the batched crowd emitted no motion at all: the previous-pose palette was dropped";
    }
} // namespace OloEngine::Tests

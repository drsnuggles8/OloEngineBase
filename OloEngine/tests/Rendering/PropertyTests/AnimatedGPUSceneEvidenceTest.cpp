// =============================================================================
// AnimatedGPUSceneEvidenceTest.cpp
//
// Visual and telemetry evidence for issue #1228: an animated mesh rendered
// through the canonical GPU Scene records, captured from four camera angles and
// across a MOVING pose into
//   OloEditor/assets/tests/visual/AnimatedGPUScene_<tag>.png
//
// Why the PNGs alone would not be evidence
// ----------------------------------------
// Like the raster migration before it (#994), this change is expected to be
// pixel-identical: the record's transforms are the same render-relative
// matrices the legacy branch uploaded, and the deformation itself still happens
// in the vertex stage from the same palette. A capture that looks right
// therefore proves nothing on its own -- it would look exactly the same if
// every animated draw had silently fallen back to the legacy branch.
//
// So the pictures are accompanied by three claims the pictures cannot make:
//
//   1. Animated draws actually CONSUMED records this frame. It has to be the
//      dispatcher's count, not extraction's: a link that resolved says nothing
//      about whether anything read it, so asserting on extraction alone would
//      stay green if the link were never handed to DrawAnimatedMesh at all.
//   2. The census says what the records took, and the Skinned category has
//      stopped counting surfaces the records DID take -- while still being able
//      to count the ones they did not.
//   3. The deformation revisions advance while the subject animates, and stop
//      claiming continuity across a declared discontinuity. This is the half
//      that only a MOVING subject can show: a stale previous revision is a
//      velocity across a seam, and a still frame has no velocity to be wrong
//      about.
//
// What the pictures DO catch that no CPU test can: a transposed or
// double-shifted transform now that the animated path takes its matrices from
// the record rather than assembling them. Those render something plausible from
// one angle, which is why the captures orbit the subject instead of taking one
// shot.
//
// Classification: L8 / integration (full GL pipeline + RGBA8 readback + PNG).
//
// OLO_TEST_LAYER: integration
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 960;
        constexpr u32 kHeight = 540;
        constexpr glm::vec3 kSubjectCentre{ 0.0f, 1.9f, 0.0f };

        [[nodiscard]] f32 MeanLuminance(const std::vector<u8>& rgba)
        {
            if (rgba.empty())
            {
                return 0.0f;
            }
            f64 total = 0.0;
            const std::size_t pixels = rgba.size() / 4u;
            for (std::size_t i = 0; i < pixels; ++i)
            {
                const std::size_t base = i * 4u;
                total += 0.2126 * rgba[base + 0] + 0.7152 * rgba[base + 1] + 0.0722 * rgba[base + 2];
            }
            return static_cast<f32>(total / (255.0 * static_cast<f64>(pixels)));
        }
    } // namespace

    class AnimatedGPUSceneScene : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 12.0f, 0.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(0.50f, -0.70f, -0.50f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 5.0f;
                dl.m_CastShadows = true;
            }

            // A ground plane, so the subject is read against something rather
            // than against the void (single-mesh-visual-test-lighting.md).
            {
                Entity floor = scene.CreateEntity("Floor");
                auto& tc = floor.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, -0.5f, 0.0f };
                tc.Scale = { 40.0f, 1.0f, 40.0f };
                auto& mc = floor.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Plane;
                if (Ref<Mesh> plane = MeshPrimitives::CreatePlane())
                {
                    mc.m_MeshSource = plane->GetMeshSource();
                }
                auto& mat = floor.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.82f, 0.82f, 0.84f, 1.0f));
            }

            m_SkinnedMesh = MeshPrimitives::CreateMultiBoneAnimatedCube();
            if (!m_SkinnedMesh)
            {
                return;
            }

            m_Subject = scene.CreateEntity("AnimatedSubject");
            auto& tc = m_Subject.GetComponent<TransformComponent>();
            tc.Translation = kSubjectCentre;
            tc.Scale = { 2.0f, 2.0f, 2.0f };
            m_Subject.AddComponent<MeshComponent>().m_MeshSource = m_SkinnedMesh->GetMeshSource();
            m_Subject.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor(
                glm::vec4(0.9f, 0.25f, 0.15f, 1.0f));

            // Own the skeleton on the entity so the pose can be driven directly
            // without a clip: this test is about what the renderer does with a
            // pose, not about how the pose was sampled.
            const Skeleton* sourceSkeleton = m_SkinnedMesh->GetMeshSource()->GetSkeleton();
            ASSERT_TRUE(sourceSkeleton) << "the skinned primitive carries no skeleton";
            m_Skeleton = Ref<Skeleton>::Create(sourceSkeleton->m_LocalTransforms.size());
            // Only the SkeletonData half: Skeleton also inherits RefCounted,
            // whose bookkeeping must not be assigned over.
            static_cast<SkeletonData&>(*m_Skeleton) = static_cast<const SkeletonData&>(*sourceSkeleton);
            m_Subject.AddComponent<SkeletonComponent>(m_Skeleton);

            SetUpperBoneBend(0.0f);
        }

        /// Rotate the upper bone about Z and recompute the palette, mirroring
        /// AnimationSystem's hierarchy walk.
        void SetUpperBoneBend(f32 radians)
        {
            if (!m_Skeleton)
            {
                return;
            }
            SkeletonData& skeleton = *m_Skeleton;
            skeleton.m_LocalTransforms[0] = glm::mat4(1.0f);
            skeleton.m_LocalTransforms[1] = glm::rotate(glm::mat4(1.0f), radians, glm::vec3(0.0f, 0.0f, 1.0f));

            const sizet boneCount = skeleton.m_LocalTransforms.size();
            for (sizet bone = 0; bone < boneCount; ++bone)
            {
                const i32 parent = skeleton.m_ParentIndices[bone];
                skeleton.m_GlobalTransforms[bone] =
                    (parent >= 0)
                        ? skeleton.m_GlobalTransforms[static_cast<sizet>(parent)] * skeleton.m_LocalTransforms[bone]
                        : skeleton.m_LocalTransforms[bone];
            }
            for (sizet bone = 0; bone < boneCount; ++bone)
            {
                skeleton.m_FinalBoneMatrices[bone] =
                    skeleton.m_GlobalTransforms[bone] * skeleton.m_InverseBindPoses[bone];
            }
        }

        void Capture(const std::string& tag, f32 yaw, f32 pitch, std::vector<u8>& outPixels)
        {
            EditorCamera camera(55.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 500.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            // Focus, not SetPose: it orbits a point, so every pose is guaranteed
            // to be LOOKING AT the subject whatever the yaw/pitch conventions
            // are, which is how a sibling test avoided capturing empty sky.
            camera.Focus(kSubjectCentre, 9.0f, yaw, pitch);

            RunEditorFrames(camera, 2);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
            {
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            }
            if (!fb)
            {
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            }
            ASSERT_TRUE(fb) << "no composited framebuffer for capture '" << tag << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            // GL readback is bottom-up; PNG treats row 0 as the top.
            const std::size_t rowBytes = static_cast<std::size_t>(kWidth) * 4u;
            std::vector<u8> scratch(rowBytes);
            for (u32 y = 0; y < kHeight / 2u; ++y)
            {
                u8* top = outPixels.data() + static_cast<std::size_t>(y) * rowBytes;
                u8* bottom = outPixels.data() + static_cast<std::size_t>(kHeight - 1u - y) * rowBytes;
                std::memcpy(scratch.data(), top, rowBytes);
                std::memcpy(top, bottom, rowBytes);
                std::memcpy(bottom, scratch.data(), rowBytes);
            }

            // Anchored, not relative to whatever the cwd happens to be when the
            // capture runs. Renderer init resolves its assets relative to
            // OloEditor/ and leaves the process there, so a bare
            // fs::path("OloEditor") written AFTER the first frame lands in
            // OloEditor/OloEditor/... -- which is a real directory, so the test
            // passes, writes its evidence somewhere nobody looks, and the
            // reviewer concludes the captures were never taken.
            const fs::path cwd = fs::current_path();
            const fs::path dir = (cwd.filename() == "OloEditor" ? cwd : cwd / "OloEditor") / "assets" / "tests" /
                                 "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const fs::path path = dir / ("AnimatedGPUScene_" + tag + ".png");
            stbi_write_png(path.string().c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                           outPixels.data(), static_cast<int>(rowBytes));
        }

        Ref<Mesh> m_SkinnedMesh;
        Ref<Skeleton> m_Skeleton;
        Entity m_Subject;
    };

    // AC 1 + AC 3 + AC 4, from several angles: the animated subject reaches the
    // canonical records, draws consume them, and the diagnostics say so
    // accurately rather than reporting the surface as unsupported.
    TEST_F(AnimatedGPUSceneScene, AnimatedSubjectRendersThroughTheRecordsFromEveryAngle)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ASSERT_TRUE(m_SkinnedMesh) << "the skinned primitive failed to build";

        // Deferred: the path where PBR_GBuffer reads the canonical material
        // record, i.e. where both halves of the link are live.
        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        struct Pose
        {
            const char* Name;
            f32 Yaw;
            f32 Pitch;
        };
        const std::array<Pose, 4> poses = { {
            { "Front", 0.0f, 0.18f },
            { "Left", 1.20f, 0.22f },
            { "Right", -1.20f, 0.22f },
            { "Above", 0.0f, 0.95f },
        } };

        for (const Pose& pose : poses)
        {
            std::vector<u8> pixels;
            Capture(pose.Name, pose.Yaw, pose.Pitch, pixels);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }

            EXPECT_GT(MeanLuminance(pixels), 0.02f) << "pose '" << pose.Name << "' rendered (near-)black";

            const GPUSceneFrameStats stats = Renderer3D::GetGPUSceneStats();

            // The claim the pictures cannot make.
            EXPECT_GT(stats.m_Animated.m_CanonicalInstances, 0u)
                << "pose '" << pose.Name
                << "': no animated submesh reached a canonical record, so the surface is still outside GPU Scene";
            EXPECT_GT(stats.m_Animated.m_CanonicalEntities, 0u);
            EXPECT_EQ(stats.m_Animated.m_CanonicalInstances,
                      stats.m_Animated.m_SurfacesWithHistory + stats.m_Animated.m_SurfacesWithoutHistory)
                << "the census does not partition its own canonical instances, which is a producer bug rather "
                   "than a property of the scene";
            EXPECT_EQ(stats.m_Animated.m_UnsupportedInstances, 0u)
                << "pose '" << pose.Name << "': an animated submesh was offered to the records and refused";

            // AC 3, the part most easily faked: the category has to be able to
            // still report. It is 0 here because this scene's animated entity
            // IS representable -- not because nothing counts any more.
            EXPECT_EQ(stats.m_UnsupportedCounts[static_cast<sizet>(GPUSceneUnsupportedCategory::Skinned)], 0u)
                << "pose '" << pose.Name
                << "': a supported animated surface is still counted as an unsupported Skinned one";

            // AC 4: a draw actually READ a record. Extraction resolving a link
            // says nothing about consumption, which is why this is the
            // dispatcher's counter.
            EXPECT_GT(CommandDispatch::GetGPUSceneConsumedDrawCount(), 0u)
                << "pose '" << pose.Name << "': no draw consumed a canonical record this frame";
        }
    }

    // AC 1 + AC 2 with MOTION, which is the only state a stale previous
    // revision can be wrong in. A still subject has no velocity to get wrong;
    // an animating one does, and the record has to track it frame by frame.
    TEST_F(AnimatedGPUSceneScene, DeformationRevisionsTrackAMovingPoseAndStopAtADiscontinuity)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ASSERT_TRUE(m_Skeleton) << "the subject carries no skeleton";

        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        std::vector<u8> pixels;
        std::vector<u32> revisions;
        std::vector<f32> luminance;

        // Five frames of a bone actually bending, captured so a human can see
        // the subject deform rather than being asked to trust a counter.
        for (u32 frame = 0; frame < 5u; ++frame)
        {
            SetUpperBoneBend(0.12f * static_cast<f32>(frame));
            Capture("Bend" + std::to_string(frame), 0.0f, 0.18f, pixels);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }
            luminance.push_back(MeanLuminance(pixels));
            revisions.push_back(m_Skeleton->m_DeformationRevision);

            // While the subject animates continuously its record must claim a
            // usable previous pose -- that is what lets the temporal filters
            // reproject it instead of rejecting it every frame.
            EXPECT_TRUE(m_Skeleton->HasContinuousDeformation())
                << "frame " << frame
                << ": the surface reports no deformation history while animating continuously, so every frame of "
                   "this bend is refused a velocity";
            EXPECT_EQ(m_Skeleton->HasBoneHistory(), m_Skeleton->HasContinuousDeformation())
                << "frame " << frame << ": the record's verdict and the bone palettes' verdict have drifted";
        }

        for (std::size_t i = 1; i < revisions.size(); ++i)
        {
            EXPECT_GT(revisions[i], revisions[i - 1])
                << "the deformation revision did not advance between rendered frames, so a consumer asking "
                   "'has this surface deformed since I last built from it' would answer no while it bent";
        }

        // The picture changed too. A counter that advances over five frames of
        // an IDENTICAL image would mean the bend never reached the GPU.
        const auto [minLuma, maxLuma] = std::minmax_element(luminance.begin(), luminance.end());
        EXPECT_GT(*maxLuma - *minLuma, 1e-4f)
            << "five frames of a bending bone produced a visually identical image, so the pose is not reaching "
               "the vertex stage and the revision counter is measuring nothing";

        // The discontinuity half: after a declared reset the surface must stop
        // claiming a previous pose, even though it is mid-animation and its
        // palettes are perfectly well formed. This is the case that produces a
        // plausible wrong image rather than a failure.
        m_Skeleton->ResetBoneHistory();
        EXPECT_FALSE(m_Skeleton->HasContinuousDeformation())
            << "a declared discontinuity left the surface claiming a previous pose it was never in";

        SetUpperBoneBend(0.8f);
        Capture("AfterDiscontinuity", 0.0f, 0.18f, pixels);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }
        EXPECT_GT(MeanLuminance(pixels), 0.02f)
            << "the frame after a discontinuity rendered black: a dropped history must cost the surface its "
               "velocity, never its geometry";
    }

    // The feature applies to all three rendering paths, and for different
    // reasons in each, so a Deferred-only proof would be a proof about one
    // third of it:
    //
    //   * the TRANSFORM half of a link is consumed at
    //     CommandDispatch::UploadModelInstance, the single-instance choke point
    //     every path and both backends share -- so it applies everywhere;
    //   * the MATERIAL half needs the canonical material table bound, which is
    //     the Deferred G-buffer's read.
    //
    // A path that quietly stopped carrying links would still draw, because an
    // unresolved link falls back by design, so the only thing that catches it
    // is the consumed-draw counter asserted per path.
    TEST_F(AnimatedGPUSceneScene, EveryRenderingPathConsumesTheSameAnimatedRecords)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ASSERT_TRUE(m_SkinnedMesh) << "the skinned primitive failed to build";

        struct PathCase
        {
            const char* Name;
            RenderingPath Path;
        };
        const std::array<PathCase, 3> paths = { {
            { "Forward", RenderingPath::Forward },
            { "ForwardPlus", RenderingPath::ForwardPlus },
            { "Deferred", RenderingPath::Deferred },
        } };

        for (const PathCase& pathCase : paths)
        {
            Renderer3D::GetRendererSettings().Path = pathCase.Path;
            Renderer3D::ApplyRendererSettings();

            std::vector<u8> pixels;
            Capture(std::string("Path_") + pathCase.Name, 0.0f, 0.18f, pixels);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }

            EXPECT_GT(MeanLuminance(pixels), 0.02f)
                << "path '" << pathCase.Name << "' rendered (near-)black";

            const GPUSceneFrameStats stats = Renderer3D::GetGPUSceneStats();
            EXPECT_GT(stats.m_Animated.m_CanonicalInstances, 0u)
                << "path '" << pathCase.Name << "': the animated surface did not reach a canonical record";
            EXPECT_EQ(stats.m_UnsupportedCounts[static_cast<sizet>(GPUSceneUnsupportedCategory::Skinned)], 0u)
                << "path '" << pathCase.Name << "': a supported animated surface is counted as unsupported";
            EXPECT_GT(CommandDispatch::GetGPUSceneConsumedDrawCount(), 0u)
                << "path '" << pathCase.Name
                << "': no draw consumed a canonical record, so this path silently kept the legacy branch while "
                   "the other two migrated";
        }
    }
} // namespace OloEngine::Tests

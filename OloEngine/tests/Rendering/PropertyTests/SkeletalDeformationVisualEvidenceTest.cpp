// =============================================================================
// SkeletalDeformationVisualEvidenceTest.cpp
//
// Visual evidence and the cross-consumer contract for the shared skeletal
// deformation output (issue #1226, criterion 1): a skinned subject must render
// the SAME deformed surface in colour, in the depth prepass and in the raster
// shadow pass.
//
// The subject is MeshPrimitives::CreateMultiBoneAnimatedCube(), whose upper
// half is bound to a second bone. Rotating that bone bends the cube visibly and
// asymmetrically — and, crucially, it moves geometry that the colour pass and
// the shadow pass each have to deform for themselves.
//
// The assertion is not "the picture looks right". It is that the subject's
// silhouette in the lit image and its shadow on the floor move TOGETHER when
// the bone rotates:
//
//   * the lit silhouette's horizontal centroid shifts with the bend, and
//   * the floor shadow's horizontal centroid shifts the same way.
//
// A shadow pass deforming the vertex differently from the colour pass produces
// a shadow that does not track the bend — the caster leans one way and its
// shadow stays put, or leans by a different amount. That is exactly the class
// of divergence #1226 removed by giving all seven skinned shaders one producer,
// and it is invisible to any test that renders only a beauty frame.
//
// PNGs land in OloEditor/assets/tests/visual/SkeletalDeform_*.png as evidence
// for a human to look at. They are written every run, like the shadow-atlas
// captures — they are deliberately NOT added to the RMSE golden set, because
// the contract worth regressing on is the centroid relationship asserted here,
// not the exact pixels of a procedural cube.
//
// Classification: L8 / integration (full GL pipeline + RGBA8 readback + PNG).
//
// OLO_TEST_LAYER: integration
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

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

        // World-space centre of the skinned subject; every camera pose orbits it.
        constexpr glm::vec3 kSubjectCentre{ 0.0f, 1.9f, 0.0f };

        [[nodiscard]] f32 Luma(const std::vector<u8>& rgba, std::size_t pixelIndex)
        {
            const std::size_t base = pixelIndex * 4u;
            return 0.2126f * static_cast<f32>(rgba[base + 0]) +
                   0.7152f * static_cast<f32>(rgba[base + 1]) +
                   0.0722f * static_cast<f32>(rgba[base + 2]);
        }
    } // namespace

    class SkeletalDeformationVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            // Sun, angled so the cube casts a shadow sideways onto the floor
            // rather than straight down underneath itself — a shadow that lands
            // beside the caster is one whose horizontal position can actually be
            // measured independently of the caster's own silhouette.
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

            // Matte floor to catch the shadow.
            {
                Entity floor = scene.CreateEntity("Floor");
                auto& tc = floor.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, -0.5f, 0.0f };
                tc.Scale = { 40.0f, 1.0f, 40.0f };
                auto& mc = floor.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Plane;
                if (Ref<Mesh> plane = MeshPrimitives::CreatePlane())
                    mc.m_MeshSource = plane->GetMeshSource();
                auto& mat = floor.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.82f, 0.82f, 0.84f, 1.0f));
            }

            // The skinned subject.
            m_SkinnedMesh = MeshPrimitives::CreateMultiBoneAnimatedCube();
            if (!m_SkinnedMesh)
                return;

            m_Subject = scene.CreateEntity("SkinnedSubject");
            auto& tc = m_Subject.GetComponent<TransformComponent>();
            tc.Translation = { 0.0f, 1.9f, 0.0f };
            tc.Scale = { 2.0f, 2.0f, 2.0f };

            auto& mc = m_Subject.AddComponent<MeshComponent>();
            mc.m_MeshSource = m_SkinnedMesh->GetMeshSource();

            auto& mat = m_Subject.AddComponent<MaterialComponent>();
            mat.m_Material.SetBaseColorFactor(glm::vec4(0.9f, 0.25f, 0.15f, 1.0f));

            // Own the skeleton on the entity so the pose can be driven directly,
            // without a clip: this test is about what the renderer does with a
            // pose, not about how the pose was sampled.
            const Skeleton* sourceSkeleton = m_SkinnedMesh->GetMeshSource()->GetSkeleton();
            ASSERT_TRUE(sourceSkeleton) << "the skinned primitive carries no skeleton";
            m_Skeleton = Ref<Skeleton>::Create(sourceSkeleton->m_LocalTransforms.size());
            // Copy only the SkeletonData half — Skeleton also inherits RefCounted,
            // whose bookkeeping must not be assigned over.
            static_cast<SkeletonData&>(*m_Skeleton) = static_cast<const SkeletonData&>(*sourceSkeleton);
            m_Subject.AddComponent<SkeletonComponent>(m_Skeleton);

            SetUpperBoneBend(0.0f);
        }

        /// Rotate the upper bone about Z by `radians` and recompute the palette,
        /// mirroring AnimationSystem's hierarchy walk.
        void SetUpperBoneBend(f32 radians)
        {
            if (!m_Skeleton)
                return;

            SkeletonData& skeleton = *m_Skeleton;
            skeleton.m_LocalTransforms[0] = glm::mat4(1.0f);
            skeleton.m_LocalTransforms[1] =
                glm::rotate(glm::mat4(1.0f), radians, glm::vec3(0.0f, 0.0f, 1.0f));

            const sizet boneCount = skeleton.m_LocalTransforms.size();
            for (sizet bone = 0; bone < boneCount; ++bone)
            {
                const i32 parent = skeleton.m_ParentIndices[bone];
                skeleton.m_GlobalTransforms[bone] =
                    (parent >= 0) ? skeleton.m_GlobalTransforms[static_cast<sizet>(parent)] * skeleton.m_LocalTransforms[bone]
                                  : skeleton.m_LocalTransforms[bone];
            }
            for (sizet bone = 0; bone < boneCount; ++bone)
            {
                skeleton.m_FinalBoneMatrices[bone] =
                    skeleton.m_GlobalTransforms[bone] * skeleton.m_InverseBindPoses[bone];
            }
        }

        void Capture(const std::string& tag, std::vector<u8>& outPixels)
        {
            // The global shadow toggle is off by default in this fixture, and a
            // frame with no shadow map at all would make the cross-consumer
            // assertion below vacuous rather than failing.
            ShadowSettings shadowSettings = Renderer3D::GetShadowMap().GetSettings();
            shadowSettings.Enabled = true;
            Renderer3D::GetShadowMap().SetSettings(shadowSettings);

            EditorCamera camera(55.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 500.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            // Focus, not SetPose: it orbits a point, so every pose is guaranteed
            // to be LOOKING AT the subject whatever the yaw/pitch conventions
            // are. Posing an eye by hand and trusting a remembered pitch sign is
            // how the first version of this test ended up capturing empty sky.
            // Positive pitch tilts the view down.
            camera.Focus(kSubjectCentre, m_CameraDistance, m_CameraYaw, m_CameraPitch);

            RunEditorFrames(camera, 2);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "no composited framebuffer for capture '" << tag << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            // GL readback is bottom-up; PNG and the row sampling below both treat
            // row 0 as the top.
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

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / ("SkeletalDeform_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight),
                                               4, outPixels.data(), static_cast<int>(kWidth) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";
        }

        /// Horizontal centroid, in normalised 0..1 screen X, of the pixels in the
        /// given row band that are RED-dominant — the subject's own material. The
        /// floor is neutral grey and the sky is not, so this isolates the lit
        /// silhouette without depending on exact shading.
        [[nodiscard]] static bool SubjectCentroidX(const std::vector<u8>& rgba, f32 y0, f32 y1, f32& outX)
        {
            f64 weighted = 0.0;
            f64 total = 0.0;
            const u32 rowStart = static_cast<u32>(y0 * static_cast<f32>(kHeight));
            const u32 rowEnd = static_cast<u32>(y1 * static_cast<f32>(kHeight));
            for (u32 y = rowStart; y < rowEnd && y < kHeight; ++y)
            {
                for (u32 x = 0; x < kWidth; ++x)
                {
                    const std::size_t index = static_cast<std::size_t>(y) * kWidth + x;
                    const std::size_t base = index * 4u;
                    const f32 r = static_cast<f32>(rgba[base + 0]);
                    const f32 g = static_cast<f32>(rgba[base + 1]);
                    const f32 b = static_cast<f32>(rgba[base + 2]);
                    if (r > 24.0f && r > g + 12.0f && r > b + 12.0f)
                    {
                        weighted += static_cast<f64>(x);
                        total += 1.0;
                    }
                }
            }
            if (total < 50.0)
                return false;
            outX = static_cast<f32>(weighted / total) / static_cast<f32>(kWidth);
            return true;
        }

        /// Horizontal centroid of the pixels in the given band that got DARKER
        /// between the two frames — the shadow that moved. Comparing two frames
        /// rather than thresholding one absolute luminance keeps this independent
        /// of the floor's shading and of the shadow technique in use.
        [[nodiscard]] static bool DarkenedCentroidX(const std::vector<u8>& before, const std::vector<u8>& after,
                                                    f32 y0, f32 y1, f32 minDelta, f32& outX, f64& outArea)
        {
            f64 weighted = 0.0;
            f64 total = 0.0;
            const u32 rowStart = static_cast<u32>(y0 * static_cast<f32>(kHeight));
            const u32 rowEnd = static_cast<u32>(y1 * static_cast<f32>(kHeight));
            for (u32 y = rowStart; y < rowEnd && y < kHeight; ++y)
            {
                for (u32 x = 0; x < kWidth; ++x)
                {
                    const std::size_t index = static_cast<std::size_t>(y) * kWidth + x;
                    const f32 delta = Luma(before, index) - Luma(after, index);
                    if (delta > minDelta)
                    {
                        weighted += static_cast<f64>(x);
                        total += 1.0;
                    }
                }
            }
            outArea = total;
            if (total < 50.0)
                return false;
            outX = static_cast<f32>(weighted / total) / static_cast<f32>(kWidth);
            return true;
        }

        f32 m_CameraDistance = 9.0f;
        f32 m_CameraYaw = 0.0f;
        f32 m_CameraPitch = 0.22f;

        Ref<Mesh> m_SkinnedMesh;
        Ref<Skeleton> m_Skeleton;
        Entity m_Subject;
    };

    // The evidence capture: several camera angles plus the bent pose, so a human
    // can look at the deformed subject and its shadow from more than one side.
    TEST_F(SkeletalDeformationVisualEvidenceTest, CapturesDeformedSubjectFromSeveralAngles)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ASSERT_TRUE(m_SkinnedMesh) << "the skinned test primitive could not be built";

        struct Pose
        {
            const char* Tag;
            f32 Distance;
            f32 Yaw;
            f32 Pitch;
        };
        const Pose poses[] = {
            { "Front", 9.0f, 0.0f, 0.22f },
            { "Side", 9.0f, 1.20f, 0.22f },
            { "Quarter", 9.0f, -0.70f, 0.28f },
            { "Above", 9.0f, 0.0f, 0.95f },
        };

        for (const Pose& pose : poses)
        {
            m_CameraDistance = pose.Distance;
            m_CameraYaw = pose.Yaw;
            m_CameraPitch = pose.Pitch;

            SetUpperBoneBend(0.0f);
            std::vector<u8> rest;
            Capture(std::string(pose.Tag) + "_Rest", rest);
            ASSERT_FALSE(::testing::Test::HasFatalFailure());

            SetUpperBoneBend(0.85f);
            std::vector<u8> bent;
            Capture(std::string(pose.Tag) + "_Bent", bent);
            ASSERT_FALSE(::testing::Test::HasFatalFailure());

            // The bend must change the image at all: a subject whose pose does
            // not reach the renderer would produce two identical frames, and
            // every downstream assertion would then be vacuous.
            f64 changed = 0.0;
            for (std::size_t index = 0; index < static_cast<std::size_t>(kWidth) * kHeight; ++index)
            {
                if (std::fabs(Luma(rest, index) - Luma(bent, index)) > 6.0f)
                    changed += 1.0;
            }
            EXPECT_GT(changed, 500.0)
                << pose.Tag << ": bending the upper bone changed almost nothing on screen — "
                            << "the skinned pose is not reaching the render path";
        }
    }

    TEST_F(SkeletalDeformationVisualEvidenceTest, ShadowTracksTheSameBendAsTheLitSurface)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ASSERT_TRUE(m_SkinnedMesh) << "the skinned test primitive could not be built";

        m_CameraDistance = 9.5f;
        m_CameraYaw = 0.0f;
        m_CameraPitch = 0.30f;

        SetUpperBoneBend(0.0f);
        std::vector<u8> rest;
        Capture("Contract_Rest", rest);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        SetUpperBoneBend(0.95f);
        std::vector<u8> bent;
        Capture("Contract_Bent", bent);
        ASSERT_FALSE(::testing::Test::HasFatalFailure());

        // Upper band of the frame: the subject itself, lit by the colour pass.
        f32 subjectRestX = 0.0f;
        f32 subjectBentX = 0.0f;
        ASSERT_TRUE(SubjectCentroidX(rest, 0.05f, 0.62f, subjectRestX))
            << "could not find the subject in the rest frame — see "
               "assets/tests/visual/SkeletalDeform_Contract_Rest.png";
        ASSERT_TRUE(SubjectCentroidX(bent, 0.05f, 0.62f, subjectBentX))
            << "could not find the subject in the bent frame";

        const f32 subjectShift = subjectBentX - subjectRestX;
        ASSERT_GT(std::fabs(subjectShift), 0.004f)
            << "the lit silhouette did not move when the bone rotated (" << subjectRestX << " -> "
            << subjectBentX << "); the colour pass is not applying the deformation";

        // Lower band: the floor. Pixels that got DARKER going rest -> bent are
        // where the shadow arrived; pixels that got BRIGHTER are where it left.
        f32 shadowArrivedX = 0.0f;
        f32 shadowLeftX = 0.0f;
        f64 arrivedArea = 0.0;
        f64 leftArea = 0.0;
        const bool arrived = DarkenedCentroidX(rest, bent, 0.64f, 0.99f, 8.0f, shadowArrivedX, arrivedArea);
        const bool left = DarkenedCentroidX(bent, rest, 0.64f, 0.99f, 8.0f, shadowLeftX, leftArea);

        ASSERT_TRUE(arrived && left)
            << "the floor shadow did not move when the bone rotated (arrived area " << arrivedArea
            << ", vacated area " << leftArea
            << "). A shadow that stays put while the caster bends is the shadow pass deforming "
               "the vertex differently from the colour pass — the exact divergence #1226 removed "
               "by putting all seven skinned shaders on one producer. See "
               "assets/tests/visual/SkeletalDeform_Contract_{Rest,Bent}.png";

        // Same direction as the lit silhouette: the shadow arrives on the side
        // the subject leaned towards and vacates the side it left.
        const f32 shadowShift = shadowArrivedX - shadowLeftX;
        EXPECT_GT(shadowShift * subjectShift, 0.0f)
            << "the shadow moved the OPPOSITE way to the lit surface (subject shift "
            << subjectShift << ", shadow shift " << shadowShift
            << "): the two passes agree that the vertex moved but not about where to";
    }
} // namespace OloEngine::Tests

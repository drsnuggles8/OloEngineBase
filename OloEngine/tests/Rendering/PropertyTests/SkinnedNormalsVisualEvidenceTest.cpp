// =============================================================================
// SkinnedNormalsVisualEvidenceTest.cpp
//
// Visual evidence that a rigged source without normals renders SMOOTH: the
// animated import generates shared smooth normals (aiProcess_GenSmoothNormals,
// 66 degree crease limit) like the static import has since #653, where it used
// to generate flat per-face ones.
//
// The subject is a normal-less, skinned 16x10 UV sphere (NormallessSkinnedSphere)
// imported through AnimatedModel and drawn as a skinned MeshComponent, lit from
// one side. Flat normals shade every facet uniformly, so the sphere shows hard
// steps in brightness along each facet edge; smooth normals give a gradient.
//
// The assertion counts those steps: the share of the sphere's interior pixels
// whose brightness differs from a neighbour by more than kStepLuma. It runs on
// Forward, Forward+ and Deferred, from three camera angles each, and in the
// --olo-rhi=vulkan process too. PNGs land in OloEditor/assets/tests/visual/
// SkinnedNormals_<Backend>_<Path>_<Angle>.png for a human to look at; they are
// not RMSE goldens, the step share is the contract. Measured: 0.13-0.19% with
// smooth normals, 2.4-2.6% with the old flat ones, on every path.
//
// What it cannot see: a normal that is shared but points the wrong way, as
// aiProcess_GenSmoothNormals alone leaves on an indexed mesh (one face's normal
// per vertex). That still interpolates without steps, so it passes here; the
// radial-normal check in AnimatedModelImportSettingsTest catches it.
//
// Classification: L8 / integration (full pipeline + RGBA8 readback + PNG).
//
// OLO_TEST_LAYER: integration
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"
#include "TestRenderBackend.h"

#include "Rendering/NormallessSkinnedMeshFixture.h"
#include "TestTempDir.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
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

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 640;
        constexpr u32 kHeight = 480;

        constexpr glm::vec3 kSubjectCentre{ 0.0f, 1.5f, 0.0f };
        constexpr f32 kSubjectRadius = 1.5f;

        // A brightness difference between neighbouring pixels larger than this is a
        // step, not a gradient. A lit 1.5 m sphere filling ~300 px changes by a few
        // luma levels per pixel at most; a flat facet edge jumps by tens.
        constexpr f32 kStepLuma = 8.0f;

        [[nodiscard]] f32 Luma(const std::vector<u8>& rgba, sizet pixel)
        {
            const sizet base = pixel * 4u;
            return 0.2126f * static_cast<f32>(rgba[base + 0]) + 0.7152f * static_cast<f32>(rgba[base + 1]) +
                   0.0722f * static_cast<f32>(rgba[base + 2]);
        }

        // The subject's own red material; the sky is not red-dominant.
        [[nodiscard]] bool IsSubject(const std::vector<u8>& rgba, u32 x, u32 y)
        {
            const sizet base = (static_cast<sizet>(y) * kWidth + x) * 4u;
            const f32 r = rgba[base + 0];
            const f32 g = rgba[base + 1];
            const f32 b = rgba[base + 2];
            return r > 20.0f && r > g * 1.4f + 4.0f && r > b * 1.4f + 4.0f;
        }

        struct StepCount
        {
            sizet Interior = 0;
            sizet Steps = 0;
        };

        // Interior pixels are subject pixels whose whole 5x5 neighbourhood is subject,
        // which keeps the silhouette edge (subject against sky) out of the count.
        [[nodiscard]] StepCount CountShadingSteps(const std::vector<u8>& rgba)
        {
            StepCount count;
            constexpr u32 kMargin = 2;
            for (u32 y = kMargin; y + kMargin < kHeight; ++y)
            {
                for (u32 x = kMargin; x + kMargin < kWidth; ++x)
                {
                    bool interior = true;
                    for (u32 dy = 0; dy <= 2 * kMargin && interior; ++dy)
                    {
                        for (u32 dx = 0; dx <= 2 * kMargin && interior; ++dx)
                        {
                            interior = IsSubject(rgba, x + dx - kMargin, y + dy - kMargin);
                        }
                    }
                    if (!interior)
                        continue;

                    ++count.Interior;
                    const sizet pixel = static_cast<sizet>(y) * kWidth + x;
                    const f32 luma = Luma(rgba, pixel);
                    if (std::abs(luma - Luma(rgba, pixel + 1)) > kStepLuma ||
                        std::abs(luma - Luma(rgba, pixel + kWidth)) > kStepLuma)
                    {
                        ++count.Steps;
                    }
                }
            }
            return count;
        }
    } // namespace

    class SkinnedNormalsVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return true;
        }

        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            // A sun from the upper left and front, so the terminator crosses the sphere
            // in view. No shadows: self-shadowing acne would add steps of its own.
            {
                Entity light = scene.CreateEntity("Sun");
                light.GetComponent<TransformComponent>().Translation = { 0.0f, 12.0f, 0.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(0.7f, -0.45f, -0.55f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 4.0f;
                dl.m_CastShadows = false;
            }

            const fs::path gltf = NormallessSkinnedSphere::Write(TempDir("sphere"));
            m_Model = Ref<AnimatedModel>::Create(gltf.string());
            ASSERT_TRUE(m_Model);
            ASSERT_EQ(m_Model->GetMeshes().size(), 1u);
            ASSERT_TRUE(m_Model->GetSkeleton()) << "the fixture is rigged and must take the animated route";

            m_Subject = scene.CreateEntity("SkinnedSphere");
            auto& tc = m_Subject.GetComponent<TransformComponent>();
            tc.Translation = kSubjectCentre;
            tc.Scale = glm::vec3(kSubjectRadius);

            m_Subject.AddComponent<MeshComponent>().m_MeshSource = m_Model->GetMeshes()[0];

            // Rough dielectric, so the shading is a diffuse gradient with no sharp highlight.
            auto& mat = m_Subject.AddComponent<MaterialComponent>();
            mat.m_Material.SetBaseColorFactor(glm::vec4(0.85f, 0.18f, 0.12f, 1.0f));
            mat.m_Material.SetMetallicFactor(0.0f);
            mat.m_Material.SetRoughnessFactor(0.9f);

            // Own a copy of the bind pose, with its palette computed, so the draw is skinned.
            const Ref<Skeleton>& source = m_Model->GetSkeleton();
            auto skeleton = Ref<Skeleton>::Create(source->m_LocalTransforms.size());
            static_cast<SkeletonData&>(*skeleton) = static_cast<const SkeletonData&>(*source);
            for (sizet bone = 0; bone < skeleton->m_LocalTransforms.size(); ++bone)
            {
                const i32 parent = skeleton->m_ParentIndices[bone];
                skeleton->m_GlobalTransforms[bone] =
                    (parent >= 0) ? skeleton->m_GlobalTransforms[static_cast<sizet>(parent)] * skeleton->m_LocalTransforms[bone]
                                  : skeleton->m_LocalTransforms[bone];
                skeleton->m_FinalBoneMatrices[bone] = skeleton->m_GlobalTransforms[bone] * skeleton->m_InverseBindPoses[bone];
            }
            m_Subject.AddComponent<SkeletonComponent>(skeleton);
        }

        void Capture(const std::string& tag, f32 yaw, f32 pitch, std::vector<u8>& outPixels)
        {
            EditorCamera camera(45.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 200.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.Focus(kSubjectCentre, 6.0f, yaw, pitch);
            RunEditorFrames(camera, 3);

            u32 width = 0;
            u32 height = 0;
            ASSERT_TRUE(ReadbackComposite(outPixels, width, height)) << "no composited frame for '" << tag << "'";
            ASSERT_EQ(width, kWidth);
            ASSERT_EQ(height, kHeight);

            // Readback is bottom-up; the PNG wants row 0 at the top.
            const sizet rowBytes = static_cast<sizet>(kWidth) * 4u;
            std::vector<u8> scratch(rowBytes);
            for (u32 y = 0; y < kHeight / 2u; ++y)
            {
                u8* top = outPixels.data() + static_cast<sizet>(y) * rowBytes;
                u8* bottom = outPixels.data() + static_cast<sizet>(kHeight - 1u - y) * rowBytes;
                std::memcpy(scratch.data(), top, rowBytes);
                std::memcpy(top, bottom, rowBytes);
                std::memcpy(bottom, scratch.data(), rowBytes);
            }

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / (TestRenderBackend::EvidenceName("SkinnedNormals") + tag + ".png")).string();
            ASSERT_NE(::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                                       outPixels.data(), static_cast<int>(rowBytes)),
                      0)
                << "stbi_write_png failed for '" << path << "'";
        }

        Ref<AnimatedModel> m_Model;
        Entity m_Subject;
    };

    TEST_F(SkinnedNormalsVisualEvidenceTest, ANormallessRiggedSphereShadesSmoothOnEveryPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const struct
        {
            RenderingPath Path;
            const char* Name;
        } paths[] = { { RenderingPath::Forward, "Forward" },
                      { RenderingPath::ForwardPlus, "ForwardPlus" },
                      { RenderingPath::Deferred, "Deferred" } };
        const struct
        {
            const char* Name;
            f32 Yaw;
            f32 Pitch;
        } angles[] = { { "Front", 0.0f, 0.15f }, { "Left", -0.9f, 0.45f }, { "Below", 0.7f, -0.35f } };

        for (const auto& path : paths)
        {
            Renderer3D::GetRendererSettings().Path = path.Path;
            Renderer3D::ApplyRendererSettings();
            for (const auto& angle : angles)
            {
                const std::string tag = std::string(path.Name) + "_" + angle.Name;
                SCOPED_TRACE(tag);
                std::vector<u8> pixels;
                ASSERT_NO_FATAL_FAILURE(Capture(tag, angle.Yaw, angle.Pitch, pixels));

                const StepCount count = CountShadingSteps(pixels);
                // The sphere covers ~300 px across; a frame that shows much less of it
                // missed the subject and would pass vacuously.
                ASSERT_GT(count.Interior, 20000u) << "the sphere is not in the frame";
                const f64 stepShare = static_cast<f64>(count.Steps) / static_cast<f64>(count.Interior);
                std::printf("[SkinnedNormals] %s: %zu interior px, %zu steps (%.3f%%)\n", tag.c_str(),
                            count.Interior, count.Steps, stepShare * 100.0);
                EXPECT_LT(stepShare, 0.005) << "faceted shading: " << count.Steps << " of " << count.Interior
                                            << " interior pixels sit on a brightness step";
            }
        }
    }
} // namespace OloEngine::Tests

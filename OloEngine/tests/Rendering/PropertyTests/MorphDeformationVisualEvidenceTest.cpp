// =============================================================================
// MorphDeformationVisualEvidenceTest.cpp
//
// Visual evidence (PNG) for the morph half of the shared animated surface
// (issue #1227). Renders a procedurally built "expressing head" through the FULL
// editor pipeline at three expression states and from two angles, and writes
// each frame to
//   OloEditor/assets/tests/visual/MorphDeformation_<pose>.png
//
// Why a picture is needed here at all. Every CPU contract in
// MorphDeformationHistoryTest can pass while the screen is wrong: the morph pass
// writes its result into the mesh's vertex buffer and re-uploads it, so a
// deformation that is correct in the TArray<Vertex> and never reaches the GPU
// buffer is invisible to every one of those tests and to nothing else. The same
// is true in the other direction for the LOD half — a level that lost its bone
// stream is a mesh whose weights are all zero, which the shared producer treats
// as an unskinned vertex and draws at its rest pose, silently.
//
// The subject is procedural on purpose. The issue names "an expressing reference
// head", and there is no morph-target model in this repository to use as one;
// adding a licensed glTF for a screenshot would put an asset in the tree that
// only this test reads. A sphere with two authored expressions ("Smile" pushes
// the lower front outwards, "Brow" lifts the upper band) deforms visibly from
// every angle these captures use, and is reproducible without any asset at all.
// The real reference head and the real animal changing LOD are the LIVE editor
// half of the verification, not this one.
//
// Golden-image model, exactly as WaterVisualEvidenceTest: the render is frozen
// so each pose is deterministic, a normal run COMPARES against the committed PNG
// (RMSE) and writes nothing, and --olo-golden-rebase (re)writes them after a
// deliberate visual change. Run from OloEditor/ so assets resolve.
//
// Classification: L8 / integration (full GL pipeline + RGBA8 readback + PNG).
//
// OLO_TEST_LAYER: L8
// =============================================================================

#include "OloEnginePCH.h"
#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"
#include "TestTempDir.h"

#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetComponents.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/LOD.h"
#include "OloEngine/Renderer/MeshOptimization.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 960;
        constexpr u32 kHeight = 540;
        constexpr f64 kGoldenRmseThreshold = 6.0;

        [[nodiscard]] f64 Rgba8Rmse(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
                return std::numeric_limits<f64>::max();
            f64 sumSq = 0.0;
            std::size_t count = 0;
            for (std::size_t i = 0; i + 3 < a.size(); i += 4)
            {
                for (int c = 0; c < 3; ++c)
                {
                    const f64 d = static_cast<f64>(a[i + c]) - static_cast<f64>(b[i + c]);
                    sumSq += d * d;
                    ++count;
                }
            }
            return count ? std::sqrt(sumSq / static_cast<f64>(count)) : 0.0;
        }

        // Two expressions over a sphere, both large enough to read at a glance from
        // any of the capture angles. "Smile" pushes the lower front hemisphere
        // forward and down; "Brow" lifts a band across the upper half. Deliberately
        // asymmetric in x so a left/right mix-up in the delta indexing is visible
        // rather than cancelling out.
        [[nodiscard]] Ref<MorphTargetSet> BuildExpressions(const MeshSource& source)
        {
            const auto& vertices = source.GetVertices();
            const auto vertexCount = static_cast<u32>(vertices.Num());

            MorphTarget smile("Smile", vertexCount);
            MorphTarget brow("Brow", vertexCount);
            for (u32 i = 0; i < vertexCount; ++i)
            {
                const glm::vec3 p = vertices[static_cast<i32>(i)].Position;

                if (p.y < 0.05f && p.z > 0.0f)
                {
                    const f32 reach = glm::clamp(p.z, 0.0f, 1.0f);
                    smile.Vertices[i].DeltaPosition =
                        glm::vec3(0.35f * reach * (p.x >= 0.0f ? 1.0f : 0.6f), -0.45f * reach, 0.55f * reach);
                }
                if (p.y > 0.35f && p.y < 0.8f)
                {
                    brow.Vertices[i].DeltaPosition = glm::vec3(0.0f, 0.4f, 0.25f);
                }
            }

            auto set = Ref<MorphTargetSet>::Create();
            set->AddTarget(smile);
            set->AddTarget(brow);
            return set;
        }

        [[nodiscard]] bool GoldenRebaseRequested()
        {
            return OloEngine::Tests::Options().GoldenRebase;
        }
    } // namespace

    class MorphDeformationVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 6.0f, 4.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.6f, -0.7f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 3.0f;
            }

            // The head. A high enough segment count that the expressions read as a
            // smooth deformation rather than as a handful of displaced facets.
            {
                m_Head = scene.CreateEntity("ExpressingHead");
                Ref<Mesh> sphere = MeshPrimitives::CreateSphere(1.0f, 48);
                ASSERT_TRUE(sphere) << "sphere primitive unavailable";

                auto& mc = m_Head.AddComponent<MeshComponent>();
                mc.m_MeshSource = sphere->GetMeshSource();
                mc.m_MeshSource->SetMorphTargets(BuildExpressions(*mc.m_MeshSource));

                auto& mat = m_Head.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.82f, 0.66f, 0.58f, 1.0f));

                // No weights set here: each capture below drives them, so a single
                // built scene produces the whole expression sweep.
                m_Head.AddComponent<MorphTargetComponent>();
            }

            // A neutral reference object beside the head. It must look IDENTICAL in
            // every capture: the morph pass writes into a mesh's vertex buffer, so a
            // deformation that reached the wrong MeshSource would show up here as a
            // cube that moves when the face does.
            {
                Entity reference = scene.CreateEntity("NeutralReference");
                auto& tc = reference.GetComponent<TransformComponent>();
                tc.Translation = { 2.6f, 0.0f, 0.0f };
                tc.Scale = { 0.8f, 0.8f, 0.8f };
                auto& mc = reference.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Cube;
                if (Ref<Mesh> cube = MeshPrimitives::CreateCube(); cube)
                    mc.m_MeshSource = cube->GetMeshSource();
                auto& mat = reference.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.25f, 0.45f, 0.75f, 1.0f));
            }
        }

        void SetExpression(f32 smile, f32 brow)
        {
            auto& morph = m_Head.GetComponent<MorphTargetComponent>();
            morph.SetWeight("Smile", smile);
            morph.SetWeight("Brow", brow);
        }

        // Framed on the head rather than posed from an eye point. EditorCamera's
        // pitch is POSITIVE-IS-DOWN, and hand-computing an eye + yaw/pitch that
        // actually looks at the subject got the sign wrong and put the deforming
        // lower-front of the head off the bottom of every frame — with the sphere
        // still visible, so the capture looked plausible and the expression sweep
        // measured nothing. Focus() takes the point to look AT, which cannot be
        // wrong in that way.
        void Capture(const std::string& poseName, f32 yawDegrees, f32 pitchDegrees, f32 distance)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 100.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.Focus(glm::vec3(0.0f, 0.0f, 0.0f), distance, glm::radians(yawDegrees), glm::radians(pitchDegrees));

            // Several frames, not one: the morph pass and the frame-boundary history
            // advance are different phases of the frame, and a capture taken on the
            // first tick would show the surface before the deformation reached the
            // GPU buffer — which is the state this test exists to tell apart.
            RunEditorFrames(camera, 3);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for pose '" << poseName << "'";

            std::vector<u8> pixels;
            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, pixels);
            ASSERT_EQ(pixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            // GL hands back rows bottom-up; the PNG and any row-indexed check treat
            // row 0 as the top.
            {
                const std::size_t rowBytes = static_cast<std::size_t>(kWidth) * 4u;
                std::vector<u8> tmp(rowBytes);
                for (u32 y = 0; y < kHeight / 2u; ++y)
                {
                    u8* top = pixels.data() + static_cast<std::size_t>(y) * rowBytes;
                    u8* bot = pixels.data() + static_cast<std::size_t>(kHeight - 1u - y) * rowBytes;
                    std::memcpy(tmp.data(), top, rowBytes);
                    std::memcpy(top, bot, rowBytes);
                    std::memcpy(bot, tmp.data(), rowBytes);
                }
            }

            m_LastCapture = pixels;

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            const std::string path = (dir / ("MorphDeformation_" + poseName + ".png")).string();

            if (GoldenRebaseRequested())
            {
                std::error_code ec;
                fs::create_directories(dir, ec);
                ASSERT_FALSE(ec) << "Failed to create golden dir '" << dir.string() << "': " << ec.message();
                const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth),
                                                   static_cast<int>(kHeight), 4, pixels.data(),
                                                   static_cast<int>(kWidth) * 4);
                ASSERT_NE(wrote, 0) << "stbi_write_png failed to write golden '" << path << "'";
                return;
            }

            int gw = 0, gh = 0, gch = 0;
            stbi_uc* golden = ::stbi_load(path.c_str(), &gw, &gh, &gch, 4);
            ASSERT_NE(golden, nullptr)
                << "Missing golden '" << path << "' — rerun with --olo-golden-rebase to create it.";
            const bool sizeMatches = (gw == static_cast<int>(kWidth) && gh == static_cast<int>(kHeight));
            std::vector<u8> goldenPixels;
            if (sizeMatches)
                goldenPixels.assign(golden, golden + static_cast<std::size_t>(kWidth) * kHeight * 4u);
            ::stbi_image_free(golden);
            ASSERT_TRUE(sizeMatches) << "Golden '" << path << "' is " << gw << "x" << gh << ", expected "
                                     << kWidth << "x" << kHeight << " — rerun with --olo-golden-rebase.";

            const f64 rmse = Rgba8Rmse(pixels, goldenPixels);
            EXPECT_LE(rmse, kGoldenRmseThreshold)
                << "Pose '" << poseName << "' diverged from golden (RMSE " << rmse << " > "
                << kGoldenRmseThreshold << "). If this is an intended visual change, rerun with "
                << "--olo-golden-rebase to update " << path;
        }

        Entity m_Head;
        std::vector<u8> m_LastCapture;
    };

    TEST_F(MorphDeformationVisualEvidenceTest, CaptureExpressionSweepFromMultipleAngles)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Front three-quarter, where a smile displaces the silhouette most; and a
        // profile, where the brow lift does. One angle alone would miss half of
        // each expression — the reason the issue asks for several.
        constexpr f32 kDistance = 3.6f;
        constexpr f32 kThreeQuarterYaw = -35.0f;
        constexpr f32 kThreeQuarterPitch = 12.0f;
        // +90, not -90: the neutral reference cube sits at +x, and the -90 profile
        // looks straight down that axis with the cube filling the frame in front of
        // the head. Viewing from the far side keeps the cube behind the subject and
        // still in shot.
        constexpr f32 kProfileYaw = 90.0f;
        constexpr f32 kProfilePitch = 4.0f;

        SetExpression(0.0f, 0.0f);
        Capture("Neutral_ThreeQuarter", kThreeQuarterYaw, kThreeQuarterPitch, kDistance);
        const std::vector<u8> neutral = m_LastCapture;

        SetExpression(1.0f, 0.0f);
        Capture("Smile_ThreeQuarter", kThreeQuarterYaw, kThreeQuarterPitch, kDistance);
        const std::vector<u8> smiling = m_LastCapture;

        SetExpression(1.0f, 1.0f);
        Capture("SmileBrow_ThreeQuarter", kThreeQuarterYaw, kThreeQuarterPitch, kDistance);

        SetExpression(1.0f, 1.0f);
        Capture("SmileBrow_Profile", kProfileYaw, kProfilePitch, kDistance);

        SetExpression(0.0f, 0.0f);
        Capture("Neutral_Profile", kProfileYaw, kProfilePitch, kDistance);

        // Driver-independent, golden-independent: the expression must actually
        // reach the screen. A morph that is correct in the CPU vertex array and
        // never re-uploaded renders an identical frame, and every CPU test in
        // MorphDeformationHistoryTest still passes.
        ASSERT_EQ(neutral.size(), smiling.size());
        EXPECT_GT(Rgba8Rmse(neutral, smiling), kGoldenRmseThreshold)
            << "the neutral and smiling frames are the same image — the morph never reached "
               "the GPU vertex buffer, which no CPU-side test can see";
    }

    // =============================================================================
    // The LOD half of the same surface, on the GPU.
    //
    // The CPU tests in AnimatedSurfaceLODTest prove a skinned + morphing source now
    // GETS an LOD chain, that every level keeps its bone influences and morph
    // deltas, that the selected level coarsens with distance, and that the switch is
    // an attributed history rejection. What none of them can prove is that the
    // coarse level RASTERISES: the bone influence buffer is rebuilt per level from
    // an array CopyDeformationStreams resized, and a level whose stream ended up the
    // wrong length draws a mesh whose weights the shared producer reads as
    // unskinned — the rest pose, not an error.
    //
    // The level is forced through the renderer's pixel-error threshold rather than
    // by moving the subject away, so both captures frame the subject identically and
    // the only difference between them is which level was drawn. Moving it instead
    // would shrink it to a few pixels at the coarse level, where nothing about its
    // correctness is judgeable.
    // =============================================================================

    namespace
    {
        // A skinned sphere. Built from the primitive's geometry into a FRESH
        // MeshSource because MeshSource::Build() latches: the skeleton and the
        // influences have to be in place before the first build or the bone influence
        // buffer is never created.
        [[nodiscard]] Ref<MeshSource> MakeSkinnedSphere()
        {
            Ref<Mesh> sphere = MeshPrimitives::CreateSphere(1.0f, 48);
            if (!sphere || !sphere->GetMeshSource())
                return nullptr;

            const auto& srcVerts = sphere->GetMeshSource()->GetVertices();
            const auto& srcIndices = sphere->GetMeshSource()->GetIndices();

            std::vector<Vertex> vertices(srcVerts.GetData(), srcVerts.GetData() + srcVerts.Num());
            std::vector<u32> indices(srcIndices.GetData(), srcIndices.GetData() + srcIndices.Num());

            auto source = Ref<MeshSource>::Create(std::move(vertices), std::move(indices));

            auto skeleton = Ref<Skeleton>::Create(1);
            skeleton->m_BoneNames = { "Root" };
            skeleton->m_ParentIndices = { -1 };
            skeleton->m_LocalTransforms = { glm::mat4(1.0f) };
            skeleton->m_BonePreTransforms = { glm::mat4(1.0f) };
            skeleton->m_GlobalTransforms[0] = glm::mat4(1.0f);
            skeleton->SetBindPose();
            source->SetSkeleton(skeleton);

            auto& bones = source->GetBoneInfluences();
            bones.SetNum(source->GetVertices().Num());
            for (i32 i = 0; i < bones.Num(); ++i)
            {
                BoneInfluence influence;
                influence.m_BoneIDs[0] = 0u;
                influence.m_Weights[0] = 1.0f;
                bones[i] = influence;
            }

            Submesh submesh;
            submesh.m_BaseVertex = 0;
            submesh.m_BaseIndex = 0;
            submesh.m_VertexCount = static_cast<u32>(source->GetVertices().Num());
            submesh.m_IndexCount = static_cast<u32>(source->GetIndices().Num());
            submesh.m_MaterialIndex = 0;
            submesh.m_IsRigged = true;
            source->AddSubmesh(submesh);

            source->Build();
            return source;
        }
    } // namespace

    class SkinnedLODVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        // A throwaway project so AssetManager::AddMemoryOnlyAsset has somewhere to put
        // the generated LOD meshes. RendererAttachedTest brings up the renderer, not
        // the asset system — without this, GenerateAutoLODGroup dereferences a null
        // active manager and the fixture dies in SetUp with an access violation.
        // Same idiom as AutoMeshLODVisualEvidenceTest.
        void SetUpAssetManager()
        {
            m_TempDir = OloEngine::Tests::TempDir("skinnedlod");
            std::error_code ec;
            fs::remove_all(m_TempDir, ec);
            fs::create_directories(m_TempDir / "Assets", ec);
            ASSERT_FALSE(ec) << "failed to create temp project dir: " << ec.message();

            const fs::path projectFile = m_TempDir / "SkinnedLOD.oloproj";
            {
                std::ofstream proj(projectFile);
                proj << "Project:\n"
                        "  Name: SkinnedLOD\n"
                        "  StartScene: \"\"\n"
                        "  AssetDirectory: \"Assets\"\n"
                        "  ScriptModulePath: \"\"\n";
            }
            ASSERT_TRUE(Project::Load(projectFile)) << "Project::Load failed for " << m_TempDir.string();

            m_AssetManager = Ref<EditorAssetManager>::Create();
            m_AssetManager->Initialize(/*startFileWatcher=*/false);
            Project::SetAssetManager(m_AssetManager);
        }

        void TearDown() override
        {
            RendererAttachedTest::TearDown();
            m_AssetManager.Reset();
            std::error_code ec;
            fs::remove_all(m_TempDir, ec);
        }

        void BuildScene() override
        {
            SetUpAssetManager();

            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 6.0f, 4.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.6f, -0.7f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 3.0f;
            }

            Ref<MeshSource> source = MakeSkinnedSphere();
            ASSERT_TRUE(source) << "could not build a skinned subject";

            m_Subject = scene.CreateEntity("SkinnedSubject");
            m_Subject.AddComponent<MeshComponent>(source);
            m_Subject.AddComponent<SkeletonComponent>(source->GetSkeletonRef());

            auto& mat = m_Subject.AddComponent<MaterialComponent>();
            mat.m_Material.SetBaseColorFactor(glm::vec4(0.75f, 0.55f, 0.3f, 1.0f));

            auto baseMesh = Ref<Mesh>::Create(source, 0);
            const AssetHandle baseHandle = AssetManager::AddMemoryOnlyAsset(baseMesh);
            auto& lod = m_Subject.AddComponent<LODGroupComponent>();
            lod.m_LODGroup = MeshOptimization::GenerateAutoLODGroup(*source, baseHandle);
            lod.m_AutoGenerated = true;
        }

        [[nodiscard]] i32 ActiveLevel()
        {
            return m_Subject.GetComponent<LODGroupComponent>().m_ActiveAnimatedLOD;
        }

        void Capture(const std::string& poseName, f32 pixelErrorThreshold, std::vector<u8>& outPixels)
        {
            Renderer3D::GetRendererSettings().LODPixelErrorThreshold = pixelErrorThreshold;

            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 200.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.Focus(glm::vec3(0.0f), 3.4f, glm::radians(-35.0f), glm::radians(12.0f));

            // Enough frames for the frame-boundary selection to see the new threshold
            // AND for the deformation pass and submission to agree on the result.
            RunEditorFrames(camera, 4);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for '" << poseName << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            const std::size_t rowBytes = static_cast<std::size_t>(kWidth) * 4u;
            std::vector<u8> tmp(rowBytes);
            for (u32 y = 0; y < kHeight / 2u; ++y)
            {
                u8* top = outPixels.data() + static_cast<std::size_t>(y) * rowBytes;
                u8* bot = outPixels.data() + static_cast<std::size_t>(kHeight - 1u - y) * rowBytes;
                std::memcpy(tmp.data(), top, rowBytes);
                std::memcpy(top, bot, rowBytes);
                std::memcpy(bot, tmp.data(), rowBytes);
            }

            // Evidence, not a golden: what this test ASSERTS is the selected level and
            // that the two frames differ, both of which are numbers. A committed PNG
            // here would be an RMSE golden over a silhouette that moves with the
            // simplifier's output, which is not a contract worth pinning.
            if (GoldenRebaseRequested())
            {
                const fs::path dir = fs::path("assets") / "tests" / "visual";
                std::error_code ec;
                fs::create_directories(dir, ec);
                const std::string path = (dir / ("SkinnedLOD_" + poseName + ".png")).string();
                (void)::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                                       outPixels.data(), static_cast<int>(kWidth) * 4);
            }
        }

        Entity m_Subject;
        Ref<EditorAssetManager> m_AssetManager;
        fs::path m_TempDir;
    };

    TEST_F(SkinnedLODVisualEvidenceTest, ACoarseLevelOfASkinnedMeshRasterises)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const f32 restoreThreshold = Renderer3D::GetRendererSettings().LODPixelErrorThreshold;

        ASSERT_GT(m_Subject.GetComponent<LODGroupComponent>().m_LODGroup.Levels.size(), 1u)
            << "no LOD chain for a skinned source — the generators still refuse one";

        std::vector<u8> fine;
        Capture("Fine", 1.0f, fine);
        const i32 fineLevel = ActiveLevel();

        std::vector<u8> coarse;
        Capture("Coarse", 100000.0f, coarse);
        const i32 coarseLevel = ActiveLevel();

        Renderer3D::GetRendererSettings().LODPixelErrorThreshold = restoreThreshold;

        EXPECT_GE(fineLevel, 0) << "no level was resolved for the animated surface at all";
        EXPECT_GT(coarseLevel, fineLevel)
            << "raising the pixel-error threshold did not coarsen the level (fine " << fineLevel
            << ", coarse " << coarseLevel << ")";

        // The coarse level drew, and drew something DIFFERENT from the fine one. A
        // level that silently fell back to LOD 0 would produce an identical frame;
        // one that drew nothing would produce an empty one.
        EXPECT_GT(Rgba8Rmse(fine, coarse), 0.5)
            << "the coarse level rendered the same image as the fine one — the selected level is not "
               "reaching the draw, so the whole chain is decorative";

        const bool coarseHasSubject = std::ranges::any_of(
            coarse, [](u8 channel)
            { return channel > 0u; });
        EXPECT_TRUE(coarseHasSubject) << "the coarse frame is empty — the level drew nothing at all";
    }

} // namespace OloEngine::Tests

// OLO_TEST_LAYER: L8
// =============================================================================
// AutoMeshLODVisualEvidenceTest.cpp  (#711)
//
// Full-pipeline evidence for automatic mesh LOD generation + pixel-error LOD
// selection, driving the REAL Scene pipeline (`Scene::OnUpdateRuntime` ->
// Renderer3D render graph) over a dense field of generated LOD groups.
//
// The three acceptance criteria this file covers are all OBSERVATIONS of the
// running renderer, which is why they live here and not in the CPU tests:
//
//   1. LOD DOES NOT SWITCH WHEN THE CAMERA ROTATES IN PLACE.
//      `LODOrbitDoesNotSwitchLevels` orbits the camera around a static subject
//      and asserts the per-level object histogram is byte-identical at every
//      azimuth. This is the sharpest test of whether the projection plane is
//      right: a camera-plane projection passes every value test in LODTest and
//      fails HERE, because the subject's screen position changes as it swings.
//
//   2. 1080p AND 4K SELECT DIFFERENT LEVELS WITH NO THRESHOLD RETUNING.
//      `HigherResolutionRendersMoreTriangles` renders the identical scene at
//      1920x1080 and 3840x2160 with the same pixel-error threshold and compares
//      the histograms. Numbers only — the two frames are supposed to look the
//      same, so an image proves nothing here.
//
//   3. TRIANGLE COUNT DROPS AT VISUALLY EQUAL OUTPUT.
//      `PixelErrorLODSavesTrianglesAtVisuallyEqualOutput` renders the same
//      scene with LOD disabled and enabled, counts the submitted triangles on
//      both sides, and diffs the two composited frames.
//
// Scene lighting follows docs/agent-rules/single-mesh-visual-test-lighting.md:
// a ground plane plus a key light, or the subjects render near-black and every
// image assertion becomes vacuous.
//
// Evidence written: AutoMeshLOD_Off.png / AutoMeshLOD_On.png — the full-detail
// and LOD-selected frames of criterion 3, the pair a human has to compare.
//
// Classification: L8 / integration (full GL pipeline through the real Scene
// render path, RGBA8 readback + PNG). SKIPs cleanly without a GL 4.6 context.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/LOD.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshOptimization.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "TestTempDir.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth1080p = 1920;
        constexpr u32 kHeight1080p = 1080;
        constexpr u32 kWidth4K = 3840;
        constexpr u32 kHeight4K = 2160;

        // Triangles actually submitted this frame, derived from the per-level
        // object histogram and the per-level triangle counts of the one shared
        // LOD group every subject uses.
        u64 SubmittedTriangles(const std::vector<u32>& objectsPerLevel, const LODGroup& group)
        {
            u64 total = 0;
            for (sizet level = 0; level < objectsPerLevel.size() && level < group.Levels.size(); ++level)
            {
                total += static_cast<u64>(objectsPerLevel[level]) * group.Levels[level].TriangleCount;
            }
            return total;
        }

        std::string HistogramString(const std::vector<u32>& objectsPerLevel)
        {
            std::string s = "[";
            for (sizet i = 0; i < objectsPerLevel.size(); ++i)
            {
                if (i != 0)
                    s += ", ";
                s += std::to_string(objectsPerLevel[i]);
            }
            return s + "]";
        }

        // Fraction of pixels whose per-channel delta exceeds `threshold`.
        f32 FractionChanged(const std::vector<u8>& a, const std::vector<u8>& b, u8 threshold)
        {
            const std::size_t n = std::min(a.size(), b.size());
            if (n == 0 || a.size() != b.size())
                return 1.0f;
            std::size_t changed = 0;
            std::size_t pixels = 0;
            for (std::size_t i = 0; i + 3 < n; i += 4)
            {
                const int dr = std::abs(static_cast<int>(a[i + 0]) - static_cast<int>(b[i + 0]));
                const int dg = std::abs(static_cast<int>(a[i + 1]) - static_cast<int>(b[i + 1]));
                const int db = std::abs(static_cast<int>(a[i + 2]) - static_cast<int>(b[i + 2]));
                if (dr > threshold || dg > threshold || db > threshold)
                    ++changed;
                ++pixels;
            }
            return pixels ? static_cast<f32>(changed) / static_cast<f32>(pixels) : 1.0f;
        }

        f32 LuminanceSpread(const std::vector<u8>& px)
        {
            f32 mn = 1.0f;
            f32 mx = 0.0f;
            for (std::size_t i = 0; i + 3 < px.size(); i += 4)
            {
                const f32 l = (0.2126f * px[i] + 0.7152f * px[i + 1] + 0.0722f * px[i + 2]) / 255.0f;
                mn = std::min(mn, l);
                mx = std::max(mx, l);
            }
            return mx - mn;
        }

        void WriteEvidence(const std::string& name, const std::vector<u8>& rgba, u32 w, u32 h)
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const fs::path out = dir / (name + ".png");
            const int wrote = ::stbi_write_png(out.string().c_str(), static_cast<int>(w), static_cast<int>(h),
                                               4, rgba.data(), static_cast<int>(w) * 4);
            EXPECT_NE(wrote, 0) << "failed to write visual evidence PNG to " << out.string();
        }
    } // namespace

    // -------------------------------------------------------------------------
    // AutoMeshLODScene — a field of identical spheres, each carrying the SAME
    // generated LOD chain, spread over a wide depth range so the field spans
    // several levels at once.
    // -------------------------------------------------------------------------
    class AutoMeshLODScene : public RendererAttachedTest
    {
      protected:
        // A throwaway project so AssetManager::AddMemoryOnlyAsset has somewhere to
        // put the generated LOD meshes. RendererAttachedTest brings up the
        // renderer, not the asset system.
        void SetUpAssetManager()
        {
            m_TempDir = OloEngine::Tests::TempDir("automeshlod");
            std::error_code ec;
            fs::remove_all(m_TempDir, ec);
            fs::create_directories(m_TempDir / "Assets", ec);
            ASSERT_FALSE(ec) << "failed to create temp project dir: " << ec.message();

            const fs::path projectFile = m_TempDir / "AutoMeshLOD.oloproj";
            {
                std::ofstream proj(projectFile);
                proj << "Project:\n"
                        "  Name: AutoMeshLOD\n"
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

            // Camera at the origin looking down -Z (OloEngine convention).
            Entity camera = GetScene().CreateEntity("Camera");
            camera.GetComponent<TransformComponent>().Translation = { 0.0f, 2.0f, 0.0f };
            auto& cam = camera.AddComponent<CameraComponent>();
            cam.Primary = true;
            cam.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);
            cam.Camera.SetPerspectiveFarClip(2000.0f);

            // Ground plane + key light: a sparse scene renders the subjects
            // near-black otherwise (single-mesh-visual-test-lighting.md).
            Entity ground = GetScene().CreateEntity("Ground");
            ground.AddComponent<MeshComponent>(MeshPrimitives::CreateCube()->GetMeshSource());
            auto& gt = ground.GetComponent<TransformComponent>();
            gt.Translation = { 0.0f, -3.0f, -200.0f };
            gt.Scale = { 400.0f, 0.5f, 400.0f };

            Entity light = GetScene().CreateEntity("KeyLight");
            light.GetComponent<TransformComponent>().Translation = { 0.0f, 40.0f, 0.0f };
            auto& dl = light.AddComponent<DirectionalLightComponent>();
            dl.m_Color = { 1.0f, 0.97f, 0.92f };
            dl.m_Intensity = 3.0f;
            dl.m_Direction = glm::normalize(glm::vec3(0.3f, -1.0f, -0.4f));

            // The subject: one dense icosphere shared by every instance, with one
            // generated LOD chain shared by every instance too. Sharing keeps the
            // cook to a single call and makes the histogram directly comparable
            // across configurations.
            Ref<Mesh> sphere = MeshPrimitives::CreateIcosphere(1.0f, 4); // 5120 triangles
            m_SubjectMeshSource = sphere->GetMeshSource();
            ASSERT_TRUE(m_SubjectMeshSource);

            AssetHandle const baseHandle = AssetManager::AddMemoryOnlyAsset(sphere);
            ASSERT_NE(static_cast<u64>(baseHandle), 0ULL);

            m_SharedGroup = MeshOptimization::GenerateAutoLODGroup(*m_SubjectMeshSource, baseHandle);
            ASSERT_GE(m_SharedGroup.Levels.size(), 3u)
                << "the subject mesh produced no usable LOD chain — every assertion below would be vacuous";
            ASSERT_TRUE(m_SharedGroup.HasErrorData());

            // A field receding into the distance so several levels are live at once.
            constexpr i32 kColumns = 7;
            constexpr i32 kRows = 12;
            for (i32 row = 0; row < kRows; ++row)
            {
                const f32 z = -20.0f - static_cast<f32>(row) * 28.0f;
                for (i32 col = 0; col < kColumns; ++col)
                {
                    const f32 x = (static_cast<f32>(col) - static_cast<f32>(kColumns - 1) * 0.5f) * 6.0f;
                    Entity subject = GetScene().CreateEntity("Subject");
                    subject.AddComponent<MeshComponent>(m_SubjectMeshSource);
                    subject.GetComponent<TransformComponent>().Translation = { x, 0.0f, z };

                    auto& lod = subject.AddComponent<LODGroupComponent>();
                    lod.m_LODGroup = m_SharedGroup;
                    ++m_SubjectCount;
                }
            }

            EnableRendering(kWidth1080p, kHeight1080p);
        }

        // Ticks a few frames and returns this frame's per-level object histogram,
        // with the ground plane's LOD-less draw excluded (it has no LOD group, so
        // it never appears in the histogram at all).
        std::vector<u32> CaptureHistogram(u32 frames = 3)
        {
            RunFrames(frames);
            return Renderer3D::GetStats().ObjectsPerLODLevel;
        }

        void SetAllGroupsEnabled(bool enabled)
        {
            for (auto e : GetScene().GetAllEntitiesWith<LODGroupComponent>())
            {
                Entity{ e, &GetScene() }.GetComponent<LODGroupComponent>().m_Enabled = enabled;
            }
        }

        // Places the camera on a circle around the field's centre at a fixed
        // radius, looking inward. Only the POSITION and the yaw change — the
        // distance to every subject is what a correct implementation must ignore
        // changes in orientation for, so the radius is held exactly.
        void PlaceCameraOnOrbit(f32 azimuthRadians, f32 radius)
        {
            for (auto e : GetScene().GetAllEntitiesWith<CameraComponent>())
            {
                Entity cameraEntity{ e, &GetScene() };
                auto& transform = cameraEntity.GetComponent<TransformComponent>();
                transform.Translation = m_OrbitCentre + glm::vec3(radius * std::sin(azimuthRadians), 2.0f,
                                                                  radius * std::cos(azimuthRadians));
                // Yaw to face the centre. Selection must not react to this at all.
                transform.SetRotationEuler(glm::vec3(0.0f, azimuthRadians, 0.0f));
            }
        }

        Ref<EditorAssetManager> m_AssetManager;
        fs::path m_TempDir;
        Ref<MeshSource> m_SubjectMeshSource;
        LODGroup m_SharedGroup;
        u32 m_SubjectCount = 0;
        glm::vec3 m_OrbitCentre{ 0.0f, 0.0f, -180.0f };
    };

    // Acceptance criterion: LOD does not switch when the camera rotates in place.
    //
    // The camera is moved around a circle of FIXED radius about the field centre
    // and yawed to face it, so every subject's DISTANCE from the camera changes
    // by nothing that a rotation could explain — only the direction it is seen
    // from does. A projection onto the camera's own image plane would move the
    // subjects across the screen (and off it) and shift the histogram; the
    // mesh-facing plane cannot see the difference.
    TEST_F(AutoMeshLODScene, LODOrbitDoesNotSwitchLevels)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Disable every field subject's group so exactly ONE object appears in the
        // histogram. The field cannot be used for this: a fixed-radius orbit about
        // the field centre genuinely changes the distance to each individual member,
        // so a correct implementation would move their levels too. The subjects keep
        // drawing (at LOD 0) as scene background; a disabled group is simply not
        // handed to the selector, so it contributes nothing to the histogram.
        SetAllGroupsEnabled(false);

        Entity subject = GetScene().CreateEntity("OrbitSubject");
        subject.AddComponent<MeshComponent>(m_SubjectMeshSource);
        subject.GetComponent<TransformComponent>().Translation = m_OrbitCentre;
        auto& subjectLOD = subject.AddComponent<LODGroupComponent>();
        subjectLOD.m_LODGroup = m_SharedGroup;

        // Find a radius that puts the subject in the MIDDLE of the chain. Pinned at
        // LOD 0 or at the coarsest level it would hold its histogram no matter how
        // wrong the projection is, and the whole test would be vacuous.
        const auto lastLevel = static_cast<sizet>(m_SharedGroup.Levels.size() - 1);
        f32 orbitRadius = 0.0f;
        std::vector<u32> reference;
        sizet referenceLevel = 0;
        for (const f32 candidate : { 25.0f, 40.0f, 60.0f, 90.0f, 130.0f, 190.0f, 280.0f, 400.0f })
        {
            PlaceCameraOnOrbit(0.0f, candidate);
            std::vector<u32> histogram = CaptureHistogram();
            if (std::accumulate(histogram.begin(), histogram.end(), 0u) != 1u)
                continue;
            const auto level = static_cast<sizet>(
                std::distance(histogram.begin(), std::ranges::find_if(histogram, [](u32 n)
                                                                      { return n > 0; })));
            if (level > 0 && level < lastLevel)
            {
                orbitRadius = candidate;
                reference = std::move(histogram);
                referenceLevel = level;
                break;
            }
        }
        std::cout << "[ LOD orbit ] radius " << orbitRadius << ", level " << referenceLevel << " of "
                  << lastLevel << ", histogram " << HistogramString(reference) << "\n";

        ASSERT_GT(orbitRadius, 0.0f)
            << "no orbit radius put the subject mid-chain (" << (lastLevel + 1)
            << " levels) — the orbit assertion below would be vacuous at either end of the chain";

        for (i32 step = 1; step < 16; ++step)
        {
            const f32 azimuth = glm::radians(static_cast<f32>(step) * 22.5f);
            PlaceCameraOnOrbit(azimuth, orbitRadius);
            const std::vector<u32> histogram = CaptureHistogram();
            EXPECT_EQ(histogram, reference)
                << "LOD selection changed at azimuth " << (static_cast<f32>(step) * 22.5f) << " deg: "
                << HistogramString(histogram) << " vs " << HistogramString(reference)
                << " (reference level " << referenceLevel << ", radius " << orbitRadius
                << ") — selection is reacting to camera orientation, which is exactly the popping "
                   "pixel-error selection exists to remove";
        }
    }

    // Acceptance criterion: the same scene at 1080p and 4K selects appropriately
    // different LODs with NO threshold retuning.
    TEST_F(AutoMeshLODScene, HigherResolutionRendersMoreTriangles)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        ResizeRenderTarget(kWidth1080p, kHeight1080p);
        const std::vector<u32> at1080p = CaptureHistogram();
        const u64 triangles1080p = SubmittedTriangles(at1080p, m_SharedGroup);

        // Both resolutions are read back — a render that silently produced nothing
        // at 4K would otherwise pass on the histogram alone — but neither frame is
        // written out. This criterion's evidence is the LEVEL DISTRIBUTION, not the
        // pixels: the two frames are supposed to look the same, so the images say
        // nothing a human can act on, and a 3840x2160 PNG is 1.2 MB of binary that
        // every suite run would re-churn. The image half of the story is the
        // LOD-off / LOD-on pair the next test writes.
        std::vector<u8> frame1080p;
        u32 w = 0;
        u32 h = 0;
        ASSERT_TRUE(ReadbackComposite(frame1080p, w, h));
        EXPECT_EQ(w, kWidth1080p);
        EXPECT_GT(LuminanceSpread(frame1080p), 0.05f) << "the 1080p frame is nearly flat — nothing drew";

        ResizeRenderTarget(kWidth4K, kHeight4K);
        const std::vector<u32> at4K = CaptureHistogram();
        const u64 triangles4K = SubmittedTriangles(at4K, m_SharedGroup);

        std::vector<u8> frame4K;
        ASSERT_TRUE(ReadbackComposite(frame4K, w, h));
        EXPECT_EQ(w, kWidth4K);
        EXPECT_GT(LuminanceSpread(frame4K), 0.05f) << "the 4K frame is nearly flat — nothing drew";

        // Report unconditionally: these are the acceptance-criterion measurements,
        // and a number only printed on failure is a number nobody can quote.
        std::cout << "[ LOD 1080p ] " << HistogramString(at1080p) << " = " << triangles1080p << " triangles\n"
                  << "[ LOD   4K  ] " << HistogramString(at4K) << " = " << triangles4K << " triangles\n";

        ASSERT_GT(triangles1080p, 0u) << "nothing was LOD-selected at 1080p";
        EXPECT_NE(at4K, at1080p)
            << "4K selected the identical level distribution as 1080p — the pixel estimate is "
               "not scaling with render height, so one threshold cannot serve both";
        EXPECT_GT(triangles4K, triangles1080p)
            << "4K " << HistogramString(at4K) << " = " << triangles4K << " tris; "
            << "1080p " << HistogramString(at1080p) << " = " << triangles1080p << " tris";
    }

    // Acceptance criterion: triangle count in a dense scene drops measurably at
    // visually equal output.
    TEST_F(AutoMeshLODScene, PixelErrorLODSavesTrianglesAtVisuallyEqualOutput)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // LOD off: every subject draws LOD 0.
        SetAllGroupsEnabled(false);
        RunFrames(3);
        std::vector<u8> fullDetail;
        u32 w = 0;
        u32 h = 0;
        ASSERT_TRUE(ReadbackComposite(fullDetail, w, h));
        WriteEvidence("AutoMeshLOD_Off", fullDetail, w, h);

        const u64 trianglesOff = static_cast<u64>(m_SubjectCount) * m_SharedGroup.Levels.front().TriangleCount;
        ASSERT_GT(trianglesOff, 0u);
        EXPECT_GT(LuminanceSpread(fullDetail), 0.05f)
            << "the LOD-off frame is nearly flat — the subjects may not have drawn, which would "
               "make the image comparison below vacuous";

        // LOD on at the default 1 px budget.
        SetAllGroupsEnabled(true);
        const std::vector<u32> histogram = CaptureHistogram();
        const u64 trianglesOn = SubmittedTriangles(histogram, m_SharedGroup);

        std::vector<u8> loddedFrame;
        ASSERT_TRUE(ReadbackComposite(loddedFrame, w, h));
        WriteEvidence("AutoMeshLOD_On", loddedFrame, w, h);

        std::cout << "[ LOD  off  ] " << m_SubjectCount << " subjects x "
                  << m_SharedGroup.Levels.front().TriangleCount << " tris = " << trianglesOff << " triangles\n"
                  << "[ LOD  on   ] " << HistogramString(histogram) << " = " << trianglesOn << " triangles ("
                  << (100.0 - 100.0 * static_cast<f64>(trianglesOn) / static_cast<f64>(trianglesOff))
                  << "% fewer)\n";

        EXPECT_LT(trianglesOn, trianglesOff)
            << "LOD on submitted " << trianglesOn << " triangles vs " << trianglesOff
            << " off — histogram " << HistogramString(histogram);

        // "Visually equal": at a 1 px error budget the two frames must differ on
        // only a small fraction of pixels, and only at silhouettes. The slack is
        // deliberately generous — a sphere silhouette is a long thin region and
        // TAA jitter moves it — but a wholesale detail collapse would blow past it.
        const f32 changed = FractionChanged(loddedFrame, fullDetail, /*threshold*/ 48u);
        EXPECT_LT(changed, 0.05f)
            << "LOD changed " << (changed * 100.0f)
            << "% of pixels against the full-detail frame at a 1 px error budget";
    }
    // A generated chain is unpersistable however the intent flag reads, so the
    // serializer has to decide from the LEVEL HANDLES. This is the case a hand-edit
    // in the inspector used to produce: the flag said "authored" while the levels
    // were still memory-only, and the save wrote references nothing could resolve —
    // the archetype in cache-stored-unresolvable-reference.md, whose failure only
    // shows on the SECOND load.
    //
    // It lives here rather than in ComponentRoundTripTest because it needs a real
    // AssetManager and a real generated chain; a hand-built group cannot exercise
    // AssetManager::IsMemoryAsset.
    TEST_F(AutoMeshLODScene, GeneratedLevelsAreNeverWrittenToTheSceneFile)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        ASSERT_GE(m_SharedGroup.Levels.size(), 3u);

        // Every generated level past LOD 0 must really be a memory-only asset, or
        // the assertion below is vacuous.
        for (sizet i = 1; i < m_SharedGroup.Levels.size(); ++i)
        {
            ASSERT_TRUE(AssetManager::IsMemoryAsset(m_SharedGroup.Levels[i].MeshHandle))
                << "level " << i << " is not a memory-only asset — this test is not measuring anything";
        }

        // Claim the group is authored. The serializer must not believe it.
        for (auto e : GetScene().GetAllEntitiesWith<LODGroupComponent>())
        {
            Entity{ e, &GetScene() }.GetComponent<LODGroupComponent>().m_AutoGenerated = false;
        }

        const std::string yaml = SceneSerializer(GetSceneRef()).SerializeToYAML();
        ASSERT_FALSE(yaml.empty());

        for (sizet i = 1; i < m_SharedGroup.Levels.size(); ++i)
        {
            const std::string handle = std::to_string(static_cast<u64>(m_SharedGroup.Levels[i].MeshHandle));
            EXPECT_EQ(yaml.find(handle), std::string::npos)
                << "memory-only handle for level " << i << " reached the scene file — the serializer "
                                                           "trusted m_AutoGenerated instead of checking the handle";
        }
        EXPECT_NE(yaml.find("AutoGenerated: true"), std::string::npos)
            << "the group must be demoted to derived data so the loader regenerates it";
    }
} // namespace OloEngine::Tests

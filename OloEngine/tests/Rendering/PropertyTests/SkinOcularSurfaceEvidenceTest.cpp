// OLO_TEST_LAYER: L8
// =============================================================================
// SkinOcularSurfaceEvidenceTest.cpp — a pair of eyes under a pinned rig, on all
// three raster paths, with the refraction on and off. Issue #1244.
//
// WHAT THIS FIXTURE IS FOR, stated as the thing it is guarding against. A
// corneal refraction is the most screenshot-proof feature in this engine: at a
// head-on view the refracted and painted iris points COINCIDE BY SYMMETRY, so
// a front-on capture of a broken eye and a correct one are the same image.
// Issue #1244's third acceptance criterion says so in the words "animated gaze
// ... and close-up motion remain stable", and this file is built around that:
//
//   * every angle is captured TWICE, once refracting and once painted, and the
//     pair is diffed. A claim about a single frame could not tell the two apart;
//   * the FRONT angle is captured too, and its diff is expected to be SMALL.
//     That row is not padding — it is the control that proves the oblique rows
//     are measuring the refraction and not a global brightness change;
//   * a GAZE angle rotates the eye entities rather than the camera, which is
//     the motion the criterion names and the one a static rig cannot produce.
//
// THE FILENAME IS THE CELL. `EyeCornea_<Backend>_<Path>_<Angle>.png` with its
// `EyeCorneaOff_` control, per docs/process/task-loop.md §2a — so an
// artefact-backed cell that was not run is a file missing from the diff. Every
// Vulkan cell is live-only by construction (this fixture needs a real GL 4.6
// context and skips without one), which is why `GL` is in every name here.
//
// BUILT FROM PRIMITIVES, and that is a stated limitation rather than a shortcut
// nobody mentions. The engine's supplied head is Suzanne, which has eyeball
// geometry but no separate iris, no cornea and no UV chart for one — and
// Benchmark/ReferenceHead.olo's own header records "there is no eye shader" as
// the gap this fixture fills. What version 5 needs from a mesh is a UNIFORMLY
// SCALED SPHERE and an optical axis, which a sphere primitive with a transform
// gives exactly, deterministically, and without a licensed asset. The shading
// claims below are claims about the MATERIAL, which does not know what mesh it
// is on; the live editor run in the PR body is where the same profiles meet
// real head geometry.
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"
#include "RendererAttachedTest.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/SkinOcularSurface.h"
#include "OloEngine/Renderer/SkinProfile.h"
#include "OloEngine/Renderer/SkinProfileTable.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr u32 kSize = 384;

        // The two eyes' half-separation, in the fixture's units. Head-scale
        // proportions: the globes are 0.26 across and 0.34 apart, which is
        // roughly a real interpupillary distance against a real globe.
        constexpr f32 kEyeRadius = 0.13f;
        constexpr f32 kEyeSeparation = 0.17f;

        struct Capture
        {
            std::vector<u8> Pixels;
            u32 Width = 0;
            u32 Height = 0;

            [[nodiscard]] std::size_t Index(u32 x, u32 y) const
            {
                return (static_cast<std::size_t>(y) * Width + x) * 4u;
            }
        };

        struct Difference
        {
            u64 ChangedPixels = 0;
            u32 MaxDelta = 0;
        };

        [[nodiscard]] Difference Diff(const Capture& a, const Capture& b)
        {
            Difference d{};
            const std::size_t count = std::min(a.Pixels.size(), b.Pixels.size());
            for (std::size_t i = 0; i + 3 < count; i += 4)
            {
                u32 worst = 0;
                for (u32 c = 0; c < 3; ++c)
                {
                    const auto delta = static_cast<u32>(
                        std::abs(static_cast<i32>(a.Pixels[i + c]) - static_cast<i32>(b.Pixels[i + c])));
                    worst = std::max(worst, delta);
                }
                if (worst > 0)
                {
                    ++d.ChangedPixels;
                    d.MaxDelta = std::max(d.MaxDelta, worst);
                }
            }
            return d;
        }

        [[nodiscard]] fs::path VisualOutputPath(const std::string& name)
        {
            fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir / (name + ".png");
        }

        [[nodiscard]] const char* PathName(RenderingPath path)
        {
            switch (path)
            {
                case RenderingPath::Forward:
                    return "Forward";
                case RenderingPath::ForwardPlus:
                    return "ForwardPlus";
                case RenderingPath::Deferred:
                    return "Deferred";
                default:
                    break;
            }
            return "Unknown";
        }

        // THE FOUR ANGLES, and each one is in the matrix for a stated reason.
        struct AngleSetup
        {
            const char* Name;
            glm::vec3 CameraPosition;
            glm::vec3 CameraEuler;
            // How far the EYES are rotated, in degrees about their own vertical
            // axis. Non-zero only for the gaze row.
            f32 GazeDegrees;
        };

        // FRONT is the CONTROL, not a viewpoint: it is the capture where the
        // refracted and painted models agree by symmetry, and its small diff is
        // what proves the other three rows are measuring the refraction rather
        // than a global change.
        //
        // THREEQUARTER and OBLIQUE move the CAMERA, which is what a viewer
        // walking around a head does.
        //
        // GAZE keeps the camera still and rotates the EYES, which is the motion
        // issue #1244's third criterion names. It is the only row a static rig
        // could not produce, and the only one that isolates the eye's own
        // movement from the head's.
        constexpr AngleSetup kAngles[] = {
            { "Front", { 0.0f, 0.0f, 0.95f }, { 0.0f, 0.0f, 0.0f }, 0.0f },
            { "ThreeQuarter", { 0.62f, 0.10f, 0.72f }, { -6.0f, 40.0f, 0.0f }, 0.0f },
            { "Oblique", { 0.83f, 0.26f, 0.44f }, { -16.0f, 62.0f, 0.0f }, 0.0f },
            { "Gaze", { 0.0f, 0.0f, 0.95f }, { 0.0f, 0.0f, 0.0f }, 26.0f },
        };

        constexpr RenderingPath kPaths[] = { RenderingPath::Forward, RenderingPath::ForwardPlus,
                                             RenderingPath::Deferred };
    } // namespace

    // =========================================================================
    // The scene
    // =========================================================================

    class SkinOcularSurfaceScene : public RendererAttachedTest
    {
      protected:
        Entity m_LeftEye;
        Entity m_RightEye;
        Entity m_Face;

        // ONE PROFILE FOR BOTH EYES — see TheTwoEyesShareOneProfile. The second
        // handle is the PAINTED arm of the same eye, which is the A/B control
        // and differs in exactly one field.
        AssetHandle m_RefractingProfile{};
        // ITS TWIN — identical parameters, different handle. Every floor in
        // this file is measured across TWO HANDLES rather than across two
        // captures of one, because the arms being compared are two handles and
        // a slot change is worth about 5 levels on its own. See
        // m_VersionFourTwin for the full argument.
        AssetHandle m_RefractingTwin{};
        AssetHandle m_PaintedProfile{};
        // Transport version 4 with no ocular block at all: the identity arm.
        AssetHandle m_VersionFourProfile{};
        // A SECOND HANDLE WITH IDENTICAL PARAMETERS. Not a duplicate by
        // oversight — it is the floor the identity claim is measured against,
        // and #1245's SkinOralSurfaceEvidenceTest keeps one for the same
        // reason. Two DIFFERENT profile assets take two different slots in the
        // per-frame profile table, and the frame is not byte-identical across a
        // slot change: measured, 5 levels out of 255 on all three paths.
        //
        // So a floor measured by capturing ONE handle twice is the wrong floor
        // — it measures temporal jitter and not slot jitter, and the identity
        // claim below is a claim across two handles. Using it anyway is how
        // this test first reported a 5-level "regression" that was the fixture.
        AssetHandle m_VersionFourTwin{};
        // Version 5 with the master at 0: what a profile that was BUMPED but
        // not authored must shade as, which is the claim that makes the version
        // bump safe.
        AssetHandle m_NeutralFiveProfile{};

        Ref<EditorAssetManager> m_AssetManager;
        fs::path m_ProjectDir;

        void BuildScene() override
        {
            SetUpScratchProject();
            if (HasFatalFailure())
                return;

            m_RefractingProfile = MakeEyeProfile("EyeRefracting", SkinEvaluationModel::OcularSurface, 1.0f, 1.0f);
            m_RefractingTwin = MakeEyeProfile("EyeRefractingTwin", SkinEvaluationModel::OcularSurface, 1.0f, 1.0f);
            m_PaintedProfile = MakeEyeProfile("EyePainted", SkinEvaluationModel::OcularSurface, 1.0f, 0.0f);
            m_NeutralFiveProfile = MakeEyeProfile("EyeNeutralFive", SkinEvaluationModel::OcularSurface, 0.0f, 1.0f);
            m_VersionFourProfile = MakeEyeProfile("EyeVersionFour", SkinEvaluationModel::OralSurface, 0.0f, 1.0f);
            m_VersionFourTwin = MakeEyeProfile("EyeVersionFourTwin", SkinEvaluationModel::OralSurface, 0.0f, 1.0f);

            Entity camera = GetScene().CreateEntity("Camera");
            auto& cameraComp = camera.AddComponent<CameraComponent>();
            cameraComp.Primary = true;
            cameraComp.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);

            Entity sun = GetScene().CreateEntity("Sun");
            auto& dirLight = sun.AddComponent<DirectionalLightComponent>();
            // OFF THE OPTICAL AXIS on purpose. A key light along the axis puts
            // the corneal highlight exactly on the pupil, where it sits on top
            // of the darkest part of the iris and hides every claim about
            // either. A three-quarter key is also what any real eye reference
            // is lit with.
            dirLight.m_Direction = glm::normalize(glm::vec3(-0.45f, -0.55f, -0.70f));
            dirLight.m_Color = { 1.0f, 0.98f, 0.95f };
            dirLight.m_Intensity = 3.2f;
            dirLight.m_CastShadows = false;

            // A FACE PLANE behind the eyes, so the eyes are seen against skin
            // rather than against the clear colour. Without it the limbus would
            // be a silhouette edge against background, and "the sclera is still
            // a skin term" would be a claim about nothing.
            m_Face = MakeSkinPart("Face", MeshPrimitives::CreateCube(), { 0.0f, 0.0f, -0.16f },
                                  { 0.72f, 0.46f, 0.10f }, m_VersionFourProfile,
                                  glm::vec3(0.72f, 0.54f, 0.47f));

            // THE TWO EYES, both naming the SAME profile handle and differing
            // only by their transform. That IS the left/right convention issue
            // #1244's first criterion asks for, built into the fixture so a
            // change that broke it would break these captures.
            m_LeftEye = MakeEye("LeftEye", -kEyeSeparation);
            m_RightEye = MakeEye("RightEye", kEyeSeparation);

            EnableRendering(kSize, kSize);
        }

        void TearDown() override
        {
            // BEFORE anything that can fail. An ASSERT_* returns from the test
            // body, so a reset written there never runs and leaves a
            // renderer-wide switch armed for every later fixture in the process.
            Renderer3D::GetSkinDiffusionSettings() = SkinDiffusionSettings{};
            Renderer3D::GetSkinProfileTable().Reset();

            m_AssetManager.Reset();
            Project::Unload();
            std::error_code ec;
            if (!m_ProjectDir.empty())
                fs::remove_all(m_ProjectDir, ec);
            RendererAttachedTest::TearDown();
        }

        // ---- Fixture construction ------------------------------------------

        [[nodiscard]] Entity MakeSkinPart(const char* name, const Ref<Mesh>& mesh, const glm::vec3& translation,
                                          const glm::vec3& scale, AssetHandle profile, const glm::vec3& albedo)
        {
            Entity entity = GetScene().CreateEntity(name);
            entity.AddComponent<MeshComponent>(mesh->GetMeshSource());
            auto& transform = entity.GetComponent<TransformComponent>();
            transform.Translation = translation;
            transform.Scale = scale;

            auto& materialComp = entity.AddComponent<MaterialComponent>();
            Material& material = materialComp.m_Material;
            material.SetBaseColorFactor(glm::vec4(albedo, 1.0f));
            material.SetMetallicFactor(0.0f);
            material.SetRoughnessFactor(0.40f);
            material.SetMaterialKind(MaterialKind::Skin);
            material.SetSkinProfileHandle(profile);
            material.SetThicknessFactor(0.004f);
            return entity;
        }

        // An eye: a UNIFORMLY SCALED sphere, which is the convention version 5
        // requires. The scale is one number in all three axes deliberately — a
        // non-uniform one makes the interpolated normal stop being the globe's
        // radial direction and the whole eye-local frame goes with it.
        [[nodiscard]] Entity MakeEye(const char* name, f32 x)
        {
            Entity entity = MakeSkinPart(name, MeshPrimitives::CreateSphere(1.0f, 64), { x, 0.03f, 0.0f },
                                         glm::vec3(kEyeRadius), m_RefractingProfile,
                                         // The SCLERA's albedo. Not white: an
                                         // eye white is warm, scattering
                                         // collagen, and painting it 1.0 would
                                         // clip every claim about the limbus.
                                         glm::vec3(0.78f, 0.74f, 0.71f));
            // The cornea is SMOOTH — that is what makes an eye read as wet, and
            // it is the roughness the corneal highlight is shaped by.
            entity.GetComponent<MaterialComponent>().m_Material.SetRoughnessFactor(0.09f);
            return entity;
        }

        // The eye profile. ONE function for every arm, so the arms differ in
        // exactly the fields named in the signature and a stray third
        // difference cannot creep into an A/B.
        [[nodiscard]] AssetHandle MakeEyeProfile(const char* name, SkinEvaluationModel model,
                                                 f32 ocularStrength, f32 refractionStrength)
        {
            auto profile = Ref<SkinProfile>::Create();
            profile->SetName(name);
            SkinProfileParameters parameters = SkinProfile::DefaultParameters();
            parameters.EvaluationModel = model;
            // Version 5 is CUMULATIVE over 4, so the layered and oral fields are
            // set on EVERY arm including the controls — otherwise the A/B would
            // be measuring the lobes and the tear film as well as the cornea.
            parameters.Specular.LobeMix = 0.2f;
            parameters.Specular.LobeRoughnessScale = 2.0f;
            parameters.Specular.NormalVarianceStrength = 0.0f;
            // THE TEAR FILM, which is #1245's coat with the index of tears
            // rather than saliva — 1.337 against 1.330, an F0 of 0.0208 against
            // 0.0201. Authored, not branched; see include/SkinOcularSurface.glsl.
            parameters.Oral.CoatStrength = 0.5f;
            parameters.Oral.CoatRoughness = 0.04f;
            parameters.Oral.CoatIor = 1.337f;

            parameters.Ocular.OcularStrength = ocularStrength;
            parameters.Ocular.RefractionStrength = refractionStrength;
            parameters.Ocular.LimbalRingStrength = 0.75f;
            parameters.Ocular.IrisConcavity = 0.25f;
            // A MID BLUE-GREY IRIS. Without a colour the iris is exactly the
            // albedo of the sclera around it and the eye reads as a sphere with
            // a dot — which is how the first capture of this feature came out,
            // and is worse than the "flat painted eye" criterion 2 forbids.
            parameters.Ocular.IrisColor = { 0.3f, 0.44f, 0.52f };
            EXPECT_TRUE(profile->SetParameters(parameters))
                << name << ": the probe profile needed correcting — a bound moved";
            const AssetHandle handle = AssetManager::AddMemoryOnlyAsset<SkinProfile>(profile);
            EXPECT_NE(static_cast<u64>(handle), 0ULL) << name << ": AddMemoryOnlyAsset returned a zero handle";
            return handle;
        }

        void SetUpScratchProject()
        {
            std::error_code ec;
            m_ProjectDir = OloEngine::Tests::TempDir("skin-ocular-surface-project");
            fs::create_directories(m_ProjectDir / "Assets", ec);
            ASSERT_FALSE(ec) << "failed to create the scratch project dir at " << m_ProjectDir.string();

            const fs::path projectFile = m_ProjectDir / "SkinOcularSurface.oloproj";
            {
                std::ofstream proj(projectFile);
                proj << "Project:\n"
                        "  Name: SkinOcularSurface\n"
                        "  StartScene: \"\"\n"
                        "  AssetDirectory: \"Assets\"\n"
                        "  ScriptModulePath: \"\"\n";
            }
            ASSERT_TRUE(Project::Load(projectFile)) << "Project::Load failed for " << projectFile.string();

            m_AssetManager = Ref<EditorAssetManager>::Create();
            m_AssetManager->Initialize(/*startFileWatcher=*/false);
            Project::SetAssetManager(m_AssetManager);
        }

        // ---- Posing ---------------------------------------------------------

        // GAZE. Both eyes turn together about their own vertical axis, which is
        // what a gaze change is — and, because the optical axis IS the entity
        // transform's +Z, it is also the only thing that has to move for the
        // refraction to follow. Nothing else in the scene changes.
        void SetGaze(f32 degrees)
        {
            for (Entity eye : { m_LeftEye, m_RightEye })
            {
                if (!eye)
                    continue;
                eye.GetComponent<TransformComponent>().SetRotationEuler(
                    glm::vec3(0.0f, glm::radians(degrees), 0.0f));
            }
        }

        void SetEyeProfile(AssetHandle handle)
        {
            for (Entity eye : { m_LeftEye, m_RightEye })
            {
                if (eye)
                    eye.GetComponent<MaterialComponent>().m_Material.SetSkinProfileHandle(handle);
            }
        }

        void SetCamera(const glm::vec3& position, const glm::vec3& eulerDegrees)
        {
            Entity camera = GetScene().FindEntityByName("Camera");
            ASSERT_TRUE(static_cast<bool>(camera)) << "the probe scene lost its camera";
            auto& transform = camera.GetComponent<TransformComponent>();
            transform.Translation = position;
            transform.SetRotationEuler(glm::radians(eulerDegrees));
        }

        // ---- Capture ---------------------------------------------------------

        [[nodiscard]] bool CaptureFrame(RenderingPath path, AssetHandle profile, const AngleSetup& angle,
                                        const std::string& name, Capture& out,
                                        f64* millisecondsPerFrame = nullptr)
        {
            SetEyeProfile(profile);
            SetGaze(angle.GazeDegrees);
            SetCamera(angle.CameraPosition, angle.CameraEuler);
            // The diffusion pass stays ON in EVERY arm: every profile here is at
            // version 4 or 5 and both diffuse, so leaving it on is what isolates
            // the cornea.
            Renderer3D::GetSkinDiffusionSettings().Enabled = true;
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::ApplyRendererSettings();

            // Two frames: the first settles the graph rebuild a path or profile
            // change forces, the second is the one measured.
            RunFrames(2);

            if (millisecondsPerFrame != nullptr)
            {
                // THE TIMING RECORD the fourth criterion asks for. WALL CLOCK
                // over several settled frames, NOT a GPU timer query: this
                // fixture renders 384x384 offscreen on a box that also hosts CI
                // runners, so the absolute number is not a benchmark and is not
                // reported as one. What it IS good for is the RELATIVE cost of
                // the refracting and painted arms measured back to back under
                // identical conditions, which is the question the criterion
                // actually asks.
                constexpr i32 kTimedFrames = 24;
                const auto start = std::chrono::steady_clock::now();
                RunFrames(kTimedFrames);
                const auto end = std::chrono::steady_clock::now();
                *millisecondsPerFrame =
                    std::chrono::duration<f64, std::milli>(end - start).count() / static_cast<f64>(kTimedFrames);
            }

            if (!ReadbackComposite(out.Pixels, out.Width, out.Height))
                return false;
            if (out.Pixels.size() != static_cast<std::size_t>(out.Width) * out.Height * 4u)
                return false;

            if (!name.empty())
            {
                const fs::path file = VisualOutputPath(name);
                const int wrote = ::stbi_write_png(file.string().c_str(), static_cast<int>(out.Width),
                                                   static_cast<int>(out.Height), 4, out.Pixels.data(),
                                                   static_cast<int>(out.Width) * 4);
                EXPECT_NE(wrote, 0) << "failed to write " << file.string();
            }
            return true;
        }

        // THE SAME-MODE CONTROL, and every claim in this fixture is measured
        // against it rather than against zero. Two captures of the SAME profile
        // are not guaranteed byte-identical — the renderer carries temporal
        // state across frames — so asserting "nothing changed AT ALL" would be
        // asserting something about the renderer's determinism rather than
        // about this feature.
        [[nodiscard]] f32 MeasureHandleFloor(RenderingPath path, AssetHandle a, AssetHandle twin,
                                             const AngleSetup& angle)
        {
            Capture capA;
            Capture capB;
            if (!CaptureFrame(path, a, angle, std::string(), capA))
                return -1.0f;
            if (!CaptureFrame(path, twin, angle, std::string(), capB))
                return -1.0f;
            return static_cast<f32>(Diff(capA, capB).MaxDelta);
        }
    };

    // =========================================================================
    // CRITERION 1 — the authoring convention, on the CPU
    // =========================================================================

    // RUNS WITHOUT A GPU, deliberately: the first acceptance criterion is about
    // what the two eyes are ASSIGNED, and that is answerable without rendering
    // anything. A criterion whose only evidence needed a GL 4.6 context would be
    // unverifiable on the Linux CI this repo runs.
    TEST(SkinOcularSurfaceAssignment, TheTwoEyesShareOneProfileAndDifferOnlyByTransform)
    {
        // THE CONVENTION, asserted as the thing it actually is: there is no
        // per-eye field to get wrong, because the only per-eye quantity — the
        // optical axis — is read from the entity transform in the shader.
        //
        // This test would fail the day somebody added an `IsLeftEye` or a
        // mirrored `IrisOffset` to SkinOcularParameters, which is the point: the
        // convention is "predictable" only for as long as nothing per-entity
        // lives in the per-asset record.
        SkinProfileParameters eye = SkinProfile::DefaultParameters();
        eye.EvaluationModel = SkinEvaluationModel::OcularSurface;
        eye.Ocular.OcularStrength = 1.0f;
        ASSERT_TRUE(eye.Sanitize());

        const glm::vec4 cornea = SkinOcularCorneaLane(eye);
        const glm::vec4 iris = SkinOcularIrisLane(eye);
        const glm::vec4 response = SkinOcularResponseLane(eye);

        // Nothing in the three lanes is a direction, a side or a position —
        // every component is a scalar ratio. A lane component that encoded a
        // side would have to be signed, so the check is that none of the twelve
        // is negative for a legal profile.
        for (i32 i = 0; i < 4; ++i)
        {
            EXPECT_GE(cornea[i], 0.0f) << "cornea lane component " << i << " is signed";
            EXPECT_GE(iris[i], 0.0f) << "iris lane component " << i << " is signed";
            EXPECT_GE(response[i], 0.0f) << "response lane component " << i << " is signed";
        }

        // And the two arms of every A/B below differ in exactly ONE field, so a
        // measured difference is attributable.
        SkinProfileParameters painted = eye;
        painted.Ocular.RefractionStrength = 0.0f;
        ASSERT_TRUE(painted.Sanitize());
        EXPECT_FALSE(eye == painted);
        EXPECT_EQ(SkinOcularCorneaLane(eye), SkinOcularCorneaLane(painted))
            << "the painted control differs from the refracting arm in more than RefractionStrength";
        EXPECT_EQ(SkinOcularIrisLane(eye), SkinOcularIrisLane(painted));
    }

    // =========================================================================
    // The identity arm — a version bump that changes nothing
    // =========================================================================

    class SkinOcularIdentityTest : public SkinOcularSurfaceScene
    {
    };

    TEST_F(SkinOcularIdentityTest, ANeutralVersionFiveProfileIsTheVersionFourFrame)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // The claim that makes the version bump safe to land: moving a profile
        // from transport version 4 to version 5 with the ocular block left at
        // its defaults changes NOTHING. Every head already in this project sits
        // at version 4 or below, so if this were false the PR would restate
        // every one of them.
        for (const RenderingPath path : kPaths)
        {
            // THE FLOOR IS MEASURED ACROSS TWO HANDLES, not two captures of one
            // — see m_VersionFourTwin. The question this test asks is "does
            // changing the profile change the frame?", so the control has to be
            // "changing to an IDENTICAL profile", which is a different and
            // larger floor than "capturing the same profile twice".
            Capture floorA;
            Capture floorB;
            ASSERT_TRUE(CaptureFrame(path, m_VersionFourProfile, kAngles[0], std::string(), floorA));
            ASSERT_TRUE(CaptureFrame(path, m_VersionFourTwin, kAngles[0], std::string(), floorB));
            const auto floor = static_cast<f32>(Diff(floorA, floorB).MaxDelta);

            Capture four;
            Capture five;
            ASSERT_TRUE(CaptureFrame(path, m_VersionFourProfile, kAngles[0], std::string(), four));
            ASSERT_TRUE(CaptureFrame(path, m_NeutralFiveProfile, kAngles[0],
                                     std::string("EyeCornea_GL_") + PathName(path) + "_NeutralIdentity", five));

            const Difference d = Diff(four, five);
            EXPECT_LE(static_cast<f32>(d.MaxDelta), floor)
                << PathName(path)
                << ": a version-5 profile with OcularStrength 0 does NOT shade as version 4 "
                   "(max delta "
                << d.MaxDelta << " against a repeat floor of " << floor
                << "). Every existing head would be restated by this PR.";
        }
    }

    // =========================================================================
    // CRITERION 3 — the refraction, and why a head-on capture cannot show it
    // =========================================================================

    class SkinOcularRefractionTest : public SkinOcularSurfaceScene
    {
    };

    TEST_F(SkinOcularRefractionTest, TheCorneaMovesTheIrisAndOnlyMotionRevealsIt)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // THE CAPTURE MATRIX. Four angles x three paths x two arms, every file
        // named for its cell, so an unrun cell is a missing file in the diff.
        struct Row
        {
            const char* Path;
            const char* Angle;
            u64 ChangedPixels;
            u32 MaxDelta;
            f32 Floor;
        };
        std::vector<Row> rows;

        for (const RenderingPath path : kPaths)
        {
            for (const AngleSetup& angle : kAngles)
            {
                const std::string stem = std::string("_GL_") + PathName(path) + "_" + angle.Name;

                const f32 floor = MeasureHandleFloor(path, m_RefractingProfile, m_RefractingTwin, angle);
                ASSERT_GE(floor, 0.0f) << stem << ": the repeat-floor capture failed";

                Capture refracting;
                Capture painted;
                ASSERT_TRUE(CaptureFrame(path, m_RefractingProfile, angle, "EyeCornea" + stem, refracting));
                ASSERT_TRUE(CaptureFrame(path, m_PaintedProfile, angle, "EyeCorneaOff" + stem, painted));

                const Difference d = Diff(refracting, painted);
                rows.push_back({ PathName(path), angle.Name, d.ChangedPixels, d.MaxDelta, floor });
            }
        }

        // ---- The claim, read out of the matrix -----------------------------
        //
        // TWO ASSERTIONS AND THEY POINT IN OPPOSITE DIRECTIONS, which is what
        // makes this a measurement of the refraction rather than of anything
        // else that might have changed between two captures.
        //
        // FRONT IS NOT A ZERO-PARALLAX CONTROL, and the first version of this
        // test assumed it was and went red at 138 levels. The
        // zero-by-symmetry statement is about ONE eye on the camera's OWN axis;
        // this scene has TWO globes 0.23 apart, so at a head-on camera each is
        // already several degrees off its own optical axis and its parallax is
        // real. The exact statement is pinned where it can be stated exactly —
        // SkinOcularSurfaceTest.RefractionChangesNothingHeadOnAndEverythingObliquely,
        // which asks a single surface point at the corneal apex directly.
        //
        // What Front IS here is the SMALLEST cell, and the ORDERING across the
        // four angles is the measurable form of "this is a parallax".
        for (const Row& row : rows)
        {
            const bool isFront = std::string(row.Angle) == "Front";
            if (isFront)
            {
                // FRONT IS STILL A CELL, and it still has to show the feature:
                // the globes are off the camera's axis even here, so there is
                // real parallax. What it must NOT be is the largest — see the
                // ordering assertion below.
                EXPECT_GT(static_cast<f32>(row.MaxDelta), row.Floor + 8.0f)
                    << row.Path << "/Front: even the head-on capture should carry some parallax, "
                    << "because the two globes are 0.23 apart and neither is on the camera's axis "
                    << "(" << row.MaxDelta << " vs a floor of " << row.Floor << ")";
            }
            else
            {
                // THE FEATURE. Off-axis the two models must visibly disagree,
                // and by more than two captures of the same thing disagree.
                EXPECT_GT(static_cast<f32>(row.MaxDelta), row.Floor + 8.0f)
                    << row.Path << "/" << row.Angle << ": refracting and painted are within the "
                    << "repeat floor (" << row.MaxDelta << " vs " << row.Floor
                    << "). The cornea is not moving the iris on this path.";
                EXPECT_GT(row.ChangedPixels, 200u)
                    << row.Path << "/" << row.Angle << ": only " << row.ChangedPixels
                    << " pixels moved — too few to be an iris, so whatever changed is not the eye.";
            }
        }

        // AND THE ORDERING, which is criterion 3 in one line: the effect grows
        // with how far off-axis you are. A model that produced a constant
        // difference at every angle would pass both assertions above and would
        // not be a refraction.
        const auto findRow = [&rows](const char* path, const char* angle) -> const Row*
        {
            for (const Row& r : rows)
            {
                if (std::string(r.Path) == path && std::string(r.Angle) == angle)
                    return &r;
            }
            return nullptr;
        };
        for (const RenderingPath path : kPaths)
        {
            const Row* front = findRow(PathName(path), "Front");
            const Row* oblique = findRow(PathName(path), "Oblique");
            ASSERT_NE(front, nullptr);
            ASSERT_NE(oblique, nullptr);
            EXPECT_GT(oblique->MaxDelta, front->MaxDelta)
                << PathName(path)
                << ": the refraction does not grow with view angle, so it is not a parallax";
        }

        // THE GAZE ROW, called out separately because it is the one the
        // criterion names in words. The camera did not move; the EYES did. A
        // model whose iris is welded to the surface produces nothing here.
        for (const RenderingPath path : kPaths)
        {
            const Row* gaze = findRow(PathName(path), "Gaze");
            ASSERT_NE(gaze, nullptr);
            EXPECT_GT(static_cast<f32>(gaze->MaxDelta), gaze->Floor + 8.0f)
                << PathName(path)
                << "/Gaze: rotating the eyes under a fixed camera produced no refraction "
                   "difference. This is the animated-gaze case issue #1244's third acceptance "
                   "criterion is about, and it is the one a static front-on capture cannot show.";
        }
    }

    // =========================================================================
    // CRITERION 4 — the three paths, and what the feature costs
    // =========================================================================

    class SkinOcularPathTest : public SkinOcularSurfaceScene
    {
    };

    TEST_F(SkinOcularPathTest, TheThreeRasterPathsShadeTheSameEye)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // THE STRUCTURAL CLAIM, measured. Renderer/SkinOcularSurface.h argues
        // that the three paths agree BY CONSTRUCTION because the refraction runs
        // in the material stage rather than the lighting stage. That is an
        // argument about where code is; this is the measurement that would
        // notice if it stopped being true.
        //
        // NOT A PIXEL-EQUALITY TEST. Forward, Forward+ and Deferred differ from
        // each other on any material — different light culling, a G-Buffer round
        // trip, a different ambient ladder — so comparing them directly would
        // fail on things that have nothing to do with eyes. What IS comparable
        // is the DIFFERENCE the feature makes on each path: if the refraction is
        // one implementation, refracting-minus-painted must be about the same
        // size everywhere.
        const AngleSetup& angle = kAngles[2]; // Oblique: where the effect is largest

        std::vector<u32> deltas;
        for (const RenderingPath path : kPaths)
        {
            Capture refracting;
            Capture painted;
            ASSERT_TRUE(CaptureFrame(path, m_RefractingProfile, angle, std::string(), refracting));
            ASSERT_TRUE(CaptureFrame(path, m_PaintedProfile, angle, std::string(), painted));
            deltas.push_back(Diff(refracting, painted).MaxDelta);
        }

        ASSERT_EQ(deltas.size(), 3u);
        const u32 smallest = *std::min_element(deltas.begin(), deltas.end());
        const u32 largest = *std::max_element(deltas.begin(), deltas.end());
        ASSERT_GT(smallest, 0u) << "one of the three paths produced no refraction at all";

        // A factor of two, which is generous on purpose: the three paths tone
        // map and resolve differently, so the same radiance difference does not
        // land on the same 8-bit level. What this catches is a path where the
        // feature is MISSING or half-wired, which shows up as an order of
        // magnitude rather than as a factor of two.
        EXPECT_LT(static_cast<f32>(largest) / static_cast<f32>(smallest), 2.0f)
            << "the refraction is a different size on different paths (Forward " << deltas[0]
            << ", Forward+ " << deltas[1] << ", Deferred " << deltas[2]
            << "). It is supposed to be one implementation in the material stage.";
    }

    TEST_F(SkinOcularPathTest, TheCostOfTheEyeIsRecorded)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Criterion 4's first half: what does it cost? Written to a file beside
        // the PNGs, because a number in a test log is a number nobody reads
        // twice.
        std::string report;
        report += "EyeCornea_Timing — issue #1244, 384x384 offscreen, wall clock per frame.\n";
        report += "NOT A BENCHMARK: this box also hosts CI runners for another repo, so the\n";
        report += "absolute figures move. The RELATIVE column is the measurement — the two arms\n";
        report += "are captured back to back under identical conditions.\n\n";

        for (const RenderingPath path : kPaths)
        {
            Capture scratch;
            f64 refractingMs = 0.0;
            f64 paintedMs = 0.0;
            f64 neutralMs = 0.0;
            ASSERT_TRUE(CaptureFrame(path, m_RefractingProfile, kAngles[2], std::string(), scratch,
                                     &refractingMs));
            ASSERT_TRUE(CaptureFrame(path, m_PaintedProfile, kAngles[2], std::string(), scratch, &paintedMs));
            ASSERT_TRUE(CaptureFrame(path, m_NeutralFiveProfile, kAngles[2], std::string(), scratch,
                                     &neutralMs));

            char line[320];
            std::snprintf(line, sizeof(line),
                          "%-12s  refracting %7.3f ms   painted %7.3f ms   ocular-off %7.3f ms   "
                          "(refraction costs %+.1f%% over ocular-off)\n",
                          PathName(path), refractingMs, paintedMs, neutralMs,
                          neutralMs > 0.0 ? (refractingMs / neutralMs - 1.0) * 100.0 : 0.0);
            report += line;

            // THE ONLY ASSERTION, and it is deliberately loose: a per-pixel
            // refraction with no texture fetch cannot plausibly double a frame,
            // and anything that did would be a bug rather than a cost. A tight
            // bound here would be a flake on a shared box, which is worse than
            // no bound.
            EXPECT_LT(refractingMs, neutralMs * 2.0 + 2.0)
                << PathName(path) << ": the eye more than doubled the frame time";
        }

        const fs::path timingFile = VisualOutputPath("EyeCornea_Timing").replace_extension(".txt");
        std::ofstream out(timingFile);
        out << report;
        EXPECT_TRUE(out.good()) << "failed to write " << timingFile.string();
    }

    // =========================================================================
    // CRITERION 2 — not flat, and not sorted wrongly
    // =========================================================================

    class SkinOcularDepthTest : public SkinOcularSurfaceScene
    {
    };

    TEST_F(SkinOcularDepthTest, TheIrisRespondsToTheLightAndTheCornealHighlightDoesNot)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // THE LAYER-SORTING CLAIM, made measurable. "Cornea in front of iris"
        // has a specific consequence: the specular highlight belongs to the
        // CORNEA and must not move when the IRIS's depth response changes. A
        // shader that replaced the shading normal with the iris-plane normal —
        // the obvious way to give an iris depth — would slide that highlight
        // across the eye, which is the single most recognisable way for an eye
        // to look wrong and is exactly a layer in the wrong order.
        //
        // So: capture with the iris dish flat and with it deep, and assert that
        // the frame changed (the iris responded) while the BRIGHTEST pixel — the
        // corneal highlight — stayed where it was.
        auto flatProfile = Ref<SkinProfile>::Create();
        {
            SkinProfileParameters p = SkinProfile::DefaultParameters();
            p.EvaluationModel = SkinEvaluationModel::OcularSurface;
            p.Specular.LobeMix = 0.2f;
            p.Specular.LobeRoughnessScale = 2.0f;
            p.Specular.NormalVarianceStrength = 0.0f;
            p.Oral.CoatStrength = 0.5f;
            p.Oral.CoatRoughness = 0.04f;
            p.Oral.CoatIor = 1.337f;
            p.Ocular.OcularStrength = 1.0f;
            p.Ocular.RefractionStrength = 1.0f;
            p.Ocular.LimbalRingStrength = 0.75f;
            p.Ocular.IrisColor = { 0.3f, 0.44f, 0.52f };
            p.Ocular.IrisConcavity = 0.0f; // the ONLY difference from m_RefractingProfile
            ASSERT_TRUE(flatProfile->SetParameters(p));
        }
        const AssetHandle flat = AssetManager::AddMemoryOnlyAsset<SkinProfile>(flatProfile);
        ASSERT_NE(static_cast<u64>(flat), 0ULL);

        const AngleSetup& angle = kAngles[1]; // ThreeQuarter: the key light rakes the eye here

        const f32 floor = MeasureHandleFloor(RenderingPath::Forward, m_RefractingProfile, m_RefractingTwin, angle);
        ASSERT_GE(floor, 0.0f);

        Capture flatCapture;
        Capture deepCapture;
        ASSERT_TRUE(CaptureFrame(RenderingPath::Forward, flat, angle,
                                 "EyeCorneaFlatIris_GL_Forward_ThreeQuarter", flatCapture));
        ASSERT_TRUE(CaptureFrame(RenderingPath::Forward, m_RefractingProfile, angle,
                                 "EyeCorneaDeepIris_GL_Forward_ThreeQuarter", deepCapture));

        const Difference d = Diff(flatCapture, deepCapture);
        EXPECT_GT(static_cast<f32>(d.MaxDelta), floor + 4.0f)
            << "the iris dish changed nothing: the depth response issue #1244's second criterion "
               "asks for is not reaching the frame (max delta "
            << d.MaxDelta << ", floor " << floor << ")";

        // The brightest pixel — the corneal highlight. Located in both frames
        // and required to be in the same place.
        const auto brightest = [](const Capture& c) -> std::pair<u32, u32>
        {
            u32 bestX = 0;
            u32 bestY = 0;
            i32 best = -1;
            for (u32 y = 0; y < c.Height; ++y)
            {
                for (u32 x = 0; x < c.Width; ++x)
                {
                    const std::size_t i = c.Index(x, y);
                    const i32 luma = static_cast<i32>(c.Pixels[i]) + static_cast<i32>(c.Pixels[i + 1]) +
                                     static_cast<i32>(c.Pixels[i + 2]);
                    if (luma > best)
                    {
                        best = luma;
                        bestX = x;
                        bestY = y;
                    }
                }
            }
            return { bestX, bestY };
        };

        const auto [flatX, flatY] = brightest(flatCapture);
        const auto [deepX, deepY] = brightest(deepCapture);
        const f32 moved = std::sqrt(static_cast<f32>((flatX - deepX) * (flatX - deepX) +
                                                     (flatY - deepY) * (flatY - deepY)));
        // Four pixels at 384: enough to absorb a tie between two adjacent
        // near-equal texels, far too little to absorb a highlight sliding with
        // the iris tilt, which would move it across the whole cornea.
        EXPECT_LT(moved, 4.0f)
            << "the corneal highlight MOVED by " << moved << " px when only the iris dish changed "
            << "(" << flatX << "," << flatY << " -> " << deepX << "," << deepY << "). The specular "
                                                                                  "belongs to the cornea; if the iris tilt is dragging it, the shading normal has "
                                                                                  "been replaced rather than perturbed and the layers are in the wrong order.";
    }

} // namespace OloEngine::Tests

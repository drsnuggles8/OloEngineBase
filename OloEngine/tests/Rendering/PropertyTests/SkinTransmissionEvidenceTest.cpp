// OLO_TEST_LAYER: L8
// =============================================================================
// SkinTransmissionEvidenceTest.cpp — a BACKLIT thin region must glow, a
// FRONT-LIT one must not, and an OCCLUDED one must stop. Issue #1242.
//
// WHY THIS TEST SHAPE. The issue's second acceptance criterion is three claims
// in one sentence — "front lighting, backlighting and occlusion vary the
// contribution correctly without turning the head uniformly emissive" — and
// each of them fails into a DIFFERENT wrong frame, two of which look fine:
//
//   * a transmission that does nothing (the thickness never reached the
//     G-Buffer, the lane decoded wrong, the version gate rejected the profile)
//     renders exactly the pre-#1242 frame, which is a correct-looking head;
//   * a transmission that fires REGARDLESS OF LIGHT DIRECTION renders the
//     uniformly emissive head the criterion names. From the front it is simply
//     a slightly brighter head, which a screenshot reviewer accepts;
//   * a transmission that ignores the shadow term renders an ear that glows
//     through a hand held in front of the sun — invisible unless something is
//     actually occluding it in the captured frame.
//
// So the test measures each claim A against B on the SAME scene, with the ONLY
// difference being the authored transport VERSION:
//
//   1. BACKLIT GLOW. With the light behind, the thin region must get BRIGHTER
//      at version 2 than at version 1. That is the term doing something at all.
//   2. FRONT-LIT IDENTITY. With the light in front, version 2 must render the
//      version-1 frame — the term vanishes, so the two captures agree within
//      the noise floor. This is the claim that rules out the emissive head, and
//      it is the one no single-lighting-condition capture can make.
//   3. WARM TRANSMISSION. The backlit glow must be REDDER than the frame it is
//      added to: red's mean free path is three times blue's, so it is red that
//      survives the trip. A grey glow is the difference between skin and wax.
//   4. OCCLUSION. A blocker between the light and the thin region must collapse
//      the glow back toward the version-1 frame.
//
// THE A/B CONTROL IS THE TRANSPORT VERSION, NOT A RENDERER SWITCH, and that is
// deliberate: there is no renderer-level kill switch for this term, because
// ADR 0024 makes turning it on an authoring act per profile. Using the version
// as the control therefore tests the version gate at the same time — a profile
// at version 1 must render the #1241 frame even though the renderer is fully
// capable of transmitting.
//
// Evidence PNGs, named <Feature>_<Backend>_<Path>[_<Angle>].png so an unrun
// cell is a missing FILE (docs/process/task-loop.md 2a). GL only — this fixture
// needs a real GL 4.6 context and SKIPs without one, so the Vulkan cells come
// from the live editor and can never come from here:
//   OloEditor/assets/tests/visual/SkinTransmission{,Off}_GL_{Forward,ForwardPlus,Deferred}_Backlit.png
//   OloEditor/assets/tests/visual/SkinTransmission{,Off}_GL_{Forward,ForwardPlus,Deferred}_Frontlit.png
//   OloEditor/assets/tests/visual/SkinTransmission{,Off}_GL_Deferred_Oblique.png
//   OloEditor/assets/tests/visual/SkinTransmission{,Off}_GL_Deferred_Occluded.png
//
// Classification: L8 (full Scene pipeline on all three raster paths, RGBA8
// readback + PNG; SKIPs cleanly without a GL 4.6 context).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/SkinProfile.h"
#include "OloEngine/Renderer/SkinProfileTable.h"
#include "OloEngine/Renderer/SkinTransmission.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>

#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kSize = 384;

        // AN EXAGGERATED THICKNESS AND RADII, DELIBERATELY — the same argument
        // SkinDiffusionEvidenceTest makes about its radii. A contract test
        // measures whether the mechanism is CONNECTED, not whether it is
        // beautiful: at a real ear's 2 mm against a 1.55 mm mean free path the
        // transmitted term on a unit sphere is a fraction of an 8-bit level, and
        // a tolerance wide enough to see it would also pass with the feature off.
        //
        // These put the optical depth near 1 over a sphere-sized region, which
        // is a measurable number of levels. The per-channel RATIO is the
        // authored one (red about three times blue) because claim 3 is about
        // that ratio and nothing else.
        constexpr glm::vec3 kScatterRadiusMM{ 62.0f, 32.0f, 22.0f };
        constexpr glm::vec3 kScatterColor{ 0.90f, 0.55f, 0.40f };
        // 0.05 m x 1000 = 50 mm, comparable to the radii above.
        constexpr f32 kThicknessFactorMetres = 0.05f;

        [[nodiscard]] f32 Channel(const std::vector<u8>& px, std::size_t idx, u32 channel)
        {
            return static_cast<f32>(px[idx + channel]) / 255.0f;
        }

        [[nodiscard]] f32 LumaAt(const std::vector<u8>& px, std::size_t idx)
        {
            return 0.2126f * Channel(px, idx, 0) + 0.7152f * Channel(px, idx, 1) + 0.0722f * Channel(px, idx, 2);
        }

        // Red's share of the pixel's total energy. Scale-free, so it does not
        // move when one capture is a little brighter overall — only when the
        // colour balance changes, which is exactly claim 3.
        [[nodiscard]] f32 RedFractionAt(const std::vector<u8>& px, std::size_t idx)
        {
            const f32 r = Channel(px, idx, 0);
            const f32 g = Channel(px, idx, 1);
            const f32 b = Channel(px, idx, 2);
            return r / std::max(r + g + b, 1.0e-4f);
        }

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

        struct FrameStats
        {
            f32 MeanLuma = 0.0f;
            f32 MeanRedFraction = 0.0f;
            f32 PeakLuma = 0.0f;
            u64 Samples = 0;
        };

        // Measured over the CENTRAL DISC of the frame, which is where the probe
        // sphere is — not over the whole image, which is mostly background and
        // would dilute every measurement toward zero.
        [[nodiscard]] FrameStats MeasureSubject(const Capture& capture)
        {
            FrameStats stats{};
            f64 lumaSum = 0.0;
            f64 redSum = 0.0;
            const f32 cx = 0.5f * static_cast<f32>(capture.Width);
            const f32 cy = 0.5f * static_cast<f32>(capture.Height);
            const f32 radius = 0.30f * static_cast<f32>(capture.Width);

            for (u32 y = 0; y < capture.Height; ++y)
            {
                for (u32 x = 0; x < capture.Width; ++x)
                {
                    const f32 dx = static_cast<f32>(x) - cx;
                    const f32 dy = static_cast<f32>(y) - cy;
                    if ((dx * dx) + (dy * dy) > radius * radius)
                        continue;

                    const std::size_t idx = capture.Index(x, y);
                    const f32 luma = LumaAt(capture.Pixels, idx);
                    lumaSum += static_cast<f64>(luma);
                    redSum += static_cast<f64>(RedFractionAt(capture.Pixels, idx));
                    stats.PeakLuma = std::max(stats.PeakLuma, luma);
                    ++stats.Samples;
                }
            }
            if (stats.Samples > 0)
            {
                stats.MeanLuma = static_cast<f32>(lumaSum / static_cast<f64>(stats.Samples));
                stats.MeanRedFraction = static_cast<f32>(redSum / static_cast<f64>(stats.Samples));
            }
            return stats;
        }

        // The mean luma of the LEFT and RIGHT halves of the subject disc.
        //
        // WHY HALVES. The fixture's thickness MAP splits the sphere down the
        // middle — thin on the left, thick on the right — so a localised
        // transmission shows up as a DIFFERENCE BETWEEN THE HALVES of one
        // capture. That is a much stronger claim than "the frame got brighter",
        // because a term that ignored the thickness map entirely would brighten
        // both halves equally and pass every whole-frame measurement.
        //
        // A LEFT/RIGHT split rather than top/bottom, deliberately: a texture
        // built in memory is vertically mirrored relative to the same image
        // loaded from a file (docs/agent-rules/notes-renderer.md), so a
        // horizontal split would make this test depend on which way up the
        // upload landed. Left and right are unaffected by that flip.
        struct HalfStats
        {
            f32 LeftMeanLuma = 0.0f;
            f32 RightMeanLuma = 0.0f;
            u64 LeftSamples = 0;
            u64 RightSamples = 0;
        };

        [[nodiscard]] HalfStats MeasureHalves(const Capture& capture)
        {
            HalfStats stats{};
            f64 leftSum = 0.0;
            f64 rightSum = 0.0;
            const f32 cx = 0.5f * static_cast<f32>(capture.Width);
            const f32 cy = 0.5f * static_cast<f32>(capture.Height);
            // Tighter than MeasureSubject's disc, and inset from the seam: the
            // map's midline is a hard texel edge, and a band either side of it
            // is bilinearly blended. Measuring across it would average the two
            // answers this test is trying to tell apart.
            const f32 radius = 0.22f * static_cast<f32>(capture.Width);
            const f32 seamGuard = 0.06f * static_cast<f32>(capture.Width);

            for (u32 y = 0; y < capture.Height; ++y)
            {
                for (u32 x = 0; x < capture.Width; ++x)
                {
                    const f32 dx = static_cast<f32>(x) - cx;
                    const f32 dy = static_cast<f32>(y) - cy;
                    if ((dx * dx) + (dy * dy) > radius * radius)
                        continue;
                    if (std::abs(dx) < seamGuard)
                        continue;

                    const f32 luma = LumaAt(capture.Pixels, capture.Index(x, y));
                    if (dx < 0.0f)
                    {
                        leftSum += static_cast<f64>(luma);
                        ++stats.LeftSamples;
                    }
                    else
                    {
                        rightSum += static_cast<f64>(luma);
                        ++stats.RightSamples;
                    }
                }
            }
            if (stats.LeftSamples > 0)
                stats.LeftMeanLuma = static_cast<f32>(leftSum / static_cast<f64>(stats.LeftSamples));
            if (stats.RightSamples > 0)
                stats.RightMeanLuma = static_cast<f32>(rightSum / static_cast<f64>(stats.RightSamples));
            return stats;
        }

        // How many pixels differ, and by how much. The measured A/B the task
        // loop asks for ("61 000 pixels differ, max delta 89/255" is a result;
        // "it looks about the same" is not).
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
    } // namespace

    class SkinTransmissionScene : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            SetUpScratchProject();

            m_TransmittingProfile = MakeProfile("TransmissionProbe", SkinEvaluationModel::ThicknessTransmission);
            // THE CONTROL ARM. Version 1 diffuses but does not transmit, so the
            // A/B isolates the transmission term ALONE rather than the whole
            // skin stack — an "off" arm at version 0 would also switch the
            // #1241 diffusion off and the difference would be two features.
            m_DiffusionOnlyProfile = MakeProfile("DiffusionOnlyControl", SkinEvaluationModel::ScreenSpaceDiffusion);

            Entity camera = GetScene().CreateEntity("Camera");
            camera.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 4.0f };
            auto& cameraComp = camera.AddComponent<CameraComponent>();
            cameraComp.Primary = true;
            cameraComp.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);

            // THE KEY LIGHT, whose DIRECTION is what the test moves. Shadows on,
            // because claim 4 is about occlusion and a light that casts none
            // cannot demonstrate it.
            Entity sun = GetScene().CreateEntity("Sun");
            auto& dirLight = sun.AddComponent<DirectionalLightComponent>();
            dirLight.m_Direction = { 0.0f, 0.0f, 1.0f }; // set per capture
            dirLight.m_Color = { 1.0f, 1.0f, 1.0f };
            dirLight.m_Intensity = 3.0f;
            dirLight.m_CastShadows = true;

            const Ref<Mesh> sphere = MeshPrimitives::CreateSphere(1.0f, 48);
            Entity entity = GetScene().CreateEntity("SkinSphere");
            entity.AddComponent<MeshComponent>(sphere->GetMeshSource());
            auto& materialComp = entity.AddComponent<MaterialComponent>();
            materialComp.m_Material.SetBaseColorFactor(glm::vec4(0.62f, 0.48f, 0.42f, 1.0f));
            materialComp.m_Material.SetMetallicFactor(0.0f);
            materialComp.m_Material.SetRoughnessFactor(0.40f);
            materialComp.m_Material.SetMaterialKind(MaterialKind::Skin);
            materialComp.m_Material.SetSkinProfileHandle(m_TransmittingProfile);
            // THE AUTHORED THICKNESS, in METRES per glTF KHR_materials_volume.
            // Without it the term is disabled and counted as
            // SkinTransmissionFallbackReason::NoThickness — which the
            // ItReportsAMissingThickness test below relies on.
            materialComp.m_Material.SetThicknessFactor(kThicknessFactorMetres);
            // THE THICKNESS MAP — thin on the left, nearly opaque on the right.
            //
            // THIS IS WHAT MAKES THE EVIDENCE MEAN ANYTHING. A uniformly thick
            // sphere backlit dead-on glows all over, which is visually
            // indistinguishable from the uniformly emissive head the issue's
            // second criterion forbids — a reviewer cannot tell a correct frame
            // from the failure. With a map, the SAME frame shows one half
            // glowing and the other dark, which is the actual claim: thin
            // regions transmit and thick ones do not.
            //
            // It is also the only headless coverage of the map SAMPLING path —
            // the factor-only path would pass with u_UseThicknessMap ignored.
            materialComp.m_Material.SetThicknessMap(MakeSplitThicknessMap());
            m_Sphere = entity;

            // THE BLOCKER for claim 4, parked out of the way until that capture
            // moves it between the light and the sphere. A second entity rather
            // than a moved light, so the LIGHTING is identical and only the
            // OCCLUSION differs — moving the light would change two things.
            const Ref<Mesh> blockerMesh = MeshPrimitives::CreateCube();
            Entity blocker = GetScene().CreateEntity("Blocker");
            blocker.AddComponent<MeshComponent>(blockerMesh->GetMeshSource());
            auto& blockerTransform = blocker.GetComponent<TransformComponent>();
            blockerTransform.Translation = { 0.0f, 0.0f, -1000.0f };
            blockerTransform.Scale = { 4.0f, 4.0f, 0.2f };
            blocker.AddComponent<MaterialComponent>();
            m_Blocker = blocker;

            EnableRendering(kSize, kSize);
        }

        void TearDown() override
        {
            // BEFORE anything that can fail. An ASSERT_* returns from the test
            // body, so a reset written there never runs and leaves a
            // renderer-wide switch armed for every later fixture in the process
            // (cross-test-renderer-state.md).
            Renderer3D::GetSkinDiffusionSettings() = SkinDiffusionSettings{};
            Renderer3D::GetPostProcessSettings().MaterialDebug = MaterialDebugView::None;
            Renderer3D::GetSkinProfileTable().Reset();

            m_AssetManager.Reset();
            Project::Unload();
            std::error_code ec;
            if (!m_ProjectDir.empty())
                fs::remove_all(m_ProjectDir, ec);
            RendererAttachedTest::TearDown();
        }

        // A 64x64 RGBA8 thickness map whose split BISECTS THE VISIBLE
        // HEMISPHERE of MeshPrimitives::CreateSphere.
        //
        // THE SPLIT IS AT u = 0.25, NOT u = 0.5, AND THAT IS THE WHOLE POINT OF
        // THIS COMMENT. CreateSphere lays u around the full 360 degrees of
        // longitude (`texCoord = (s * S, r * R)`), and its z is
        // `sin(2*pi*u) * sin(pi*v)` — so the hemisphere facing a camera on +z is
        // exactly u in (0, 0.5). A texture split down its own middle therefore
        // puts the ENTIRE visible face inside ONE half, and the sphere renders
        // at one uniform thickness.
        //
        // That is not a hypothetical: this fixture's first version split at 0.5,
        // rendered a uniformly glowing ball, and the resulting failure message
        // said "the thickness MAP is not reaching the transmission term" — about
        // a map that was reaching it perfectly and being sampled at one value.
        // A test fixture can be wrong in a way that accuses the code.
        //
        // Splitting at 0.25 puts u in (0, 0.25) and u in [0.25, 0.5) — both
        // visible — on opposite sides of the seam, so the rendered sphere shows
        // both thicknesses at once.
        //
        // RGBA8 and NOT sRGB: the thickness map is DATA, not colour. An sRGB
        // upload would put a gamma curve through the optical depth and make the
        // thick half read thinner than it is — the same trap the editor's
        // drag-drop path guards against, tested here by construction.
        [[nodiscard]] Ref<Texture2D> MakeSplitThicknessMap()
        {
            constexpr u32 kDim = 64;
            TextureSpecification spec{};
            spec.Width = kDim;
            spec.Height = kDim;
            spec.Format = ImageFormat::RGBA8;
            spec.GenerateMips = false;
            spec.SRGB = false;

            Ref<Texture2D> texture = Texture2D::Create(spec);
            // `Texture2D::Create` NEVER returns null (notes-renderer.md), so the
            // Ref proves nothing — the RHI handle is the only thing that says
            // whether a GL texture actually exists. Without this the fixture
            // would attach a map the renderer resolves to "no map", fall back to
            // a uniform thickness, and render a plausible glowing ball.
            EXPECT_TRUE(static_cast<bool>(texture)) << "the thickness map texture was not created";
            EXPECT_TRUE(texture->GetRHIHandle().IsValid())
                << "the thickness map has no RHI handle — Texture2D::Create handed back a live Ref for a texture that "
                   "was never created on the GPU, so u_UseThicknessMap will be 0 and the map is silently ignored.";

            std::vector<u8> pixels(static_cast<std::size_t>(kDim) * kDim * 4u, 0u);
            for (u32 y = 0; y < kDim; ++y)
            {
                for (u32 x = 0; x < kDim; ++x)
                {
                    // 38 -> 15% of the authored thickness, which at this
                    // fixture's radii is optically thin enough to glow clearly;
                    // 255 -> the full authored thickness, which is optically
                    // deep enough to stay dark. So THIN is the 38 side.
                    //
                    // The seam sits at u = 0.25 (see the header): the half-texel
                    // offset makes the comparison land on texel CENTRES, so the
                    // seam is exactly on a texel boundary rather than through
                    // the middle of one.
                    const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kDim);
                    const u8 thickness = (u < 0.25f) ? 255u : 38u;
                    const std::size_t base = ((static_cast<std::size_t>(y) * kDim) + x) * 4u;
                    pixels[base + 0u] = thickness; // RED is the channel the shader reads
                    pixels[base + 1u] = thickness;
                    pixels[base + 2u] = thickness;
                    pixels[base + 3u] = 255u;
                }
            }
            texture->SetData(pixels.data(), static_cast<u32>(pixels.size()));
            return texture;
        }

        [[nodiscard]] AssetHandle MakeProfile(const char* name, SkinEvaluationModel model)
        {
            auto profile = Ref<SkinProfile>::Create();
            profile->SetName(name);
            SkinProfileParameters parameters = SkinProfile::DefaultParameters();
            parameters.EvaluationModel = model;
            parameters.ScatterColor = kScatterColor;
            parameters.ScatterRadiusMM = kScatterRadiusMM;
            parameters.Transmission.Strength = 1.0f;
            EXPECT_TRUE(profile->SetParameters(parameters))
                << name << ": the probe profile needed correcting — a bound moved";
            const AssetHandle handle = AssetManager::AddMemoryOnlyAsset<SkinProfile>(profile);
            EXPECT_NE(static_cast<u64>(handle), 0ULL) << name << ": AddMemoryOnlyAsset returned a zero handle";
            return handle;
        }

        void SetUpScratchProject()
        {
            std::error_code ec;
            m_ProjectDir = OloEngine::Tests::TempDir("skin-transmission-project");
            fs::create_directories(m_ProjectDir / "Assets", ec);
            ASSERT_FALSE(ec) << "failed to create the scratch project dir at " << m_ProjectDir.string();

            const fs::path projectFile = m_ProjectDir / "SkinTransmission.oloproj";
            {
                std::ofstream proj(projectFile);
                proj << "Project:\n"
                        "  Name: SkinTransmission\n"
                        "  StartScene: \"\"\n"
                        "  AssetDirectory: \"Assets\"\n"
                        "  ScriptModulePath: \"\"\n";
            }
            ASSERT_TRUE(Project::Load(projectFile)) << "Project::Load failed for " << projectFile.string();

            m_AssetManager = Ref<EditorAssetManager>::Create();
            m_AssetManager->Initialize(/*startFileWatcher=*/false);
            Project::SetAssetManager(m_AssetManager);
        }

        void SetLightDirection(const glm::vec3& direction)
        {
            Entity sun = GetScene().FindEntityByName("Sun");
            ASSERT_TRUE(static_cast<bool>(sun)) << "the probe scene lost its sun";
            sun.GetComponent<DirectionalLightComponent>().m_Direction = direction;
        }

        void SetBlockerEngaged(bool engaged)
        {
            ASSERT_TRUE(static_cast<bool>(m_Blocker));
            // Parked a kilometre away rather than removed, so the scene's entity
            // set — and therefore the render graph and every slot assignment —
            // is identical between the two captures.
            m_Blocker.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, engaged ? -3.0f : -1000.0f };
        }

        void SetProfile(AssetHandle handle)
        {
            ASSERT_TRUE(static_cast<bool>(m_Sphere));
            m_Sphere.GetComponent<MaterialComponent>().m_Material.SetSkinProfileHandle(handle);
        }

        void SetCameraPose(const glm::vec3& translation, const glm::vec3& rotationRadians)
        {
            Entity camera = GetScene().FindEntityByName("Camera");
            ASSERT_TRUE(static_cast<bool>(camera)) << "the probe scene lost its camera";
            auto& transform = camera.GetComponent<TransformComponent>();
            transform.Translation = translation;
            transform.SetRotationEuler(rotationRadians);
        }

        // Render one capture and write its evidence PNG. The PROFILE is the only
        // thing that differs between the A and B captures of a pair.
        [[nodiscard]] bool CaptureFrame(RenderingPath path, bool transmitting, const std::string& name, Capture& out)
        {
            SetProfile(transmitting ? m_TransmittingProfile : m_DiffusionOnlyProfile);
            // The diffusion pass stays ON in BOTH arms: the control is version 1,
            // which diffuses, so leaving it on is what isolates transmission.
            Renderer3D::GetSkinDiffusionSettings().Enabled = true;
            Renderer3D::GetSkinDiffusionSettings().Quality = SkinDiffusionQuality::High;
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::ApplyRendererSettings();

            // Two frames: the first settles the graph rebuild that a path or
            // profile change forces, the second is the one measured.
            RunFrames(2);

            if (!ReadbackComposite(out.Pixels, out.Width, out.Height))
                return false;
            if (out.Pixels.size() != static_cast<std::size_t>(out.Width) * out.Height * 4u)
                return false;

            const fs::path file = VisualOutputPath(name);
            const int wrote = ::stbi_write_png(file.string().c_str(), static_cast<int>(out.Width),
                                               static_cast<int>(out.Height), 4, out.Pixels.data(),
                                               static_cast<int>(out.Width) * 4);
            EXPECT_NE(wrote, 0) << "failed to write " << file.string();
            return true;
        }

        // The whole three-claim battery on one raster path.
        void ExpectTransmissionOnPath(RenderingPath path, const char* pathName)
        {
            // ---- CLAIM 1 + 3: BACKLIT --------------------------------------
            // The light points AWAY from the camera, so it is behind the sphere.
            SetLightDirection({ 0.0f, 0.0f, 1.0f });
            SetBlockerEngaged(false);

            Capture backlitOff;
            Capture backlitOn;
            ASSERT_TRUE(CaptureFrame(path, false, std::string("SkinTransmissionOff_GL_") + pathName + "_Backlit",
                                     backlitOff))
                << pathName << ": readback failed";
            ASSERT_TRUE(CaptureFrame(path, true, std::string("SkinTransmission_GL_") + pathName + "_Backlit",
                                     backlitOn))
                << pathName << ": readback failed";
            ASSERT_EQ(backlitOff.Width, backlitOn.Width);
            ASSERT_EQ(backlitOff.Height, backlitOn.Height);

            const FrameStats offStats = MeasureSubject(backlitOff);
            const FrameStats onStats = MeasureSubject(backlitOn);
            ASSERT_GT(offStats.Samples, 0u) << pathName << ": the measurement disc found no pixels";

            const Difference backlitDiff = Diff(backlitOff, backlitOn);

            // CLAIM 1. The backlit thin region must get brighter. A strict
            // inequality on the MEAN over the subject, not on a single pixel:
            // one pixel is noise, the mean over the sphere is the term.
            EXPECT_GT(onStats.MeanLuma, offStats.MeanLuma)
                << pathName << ": backlighting a version-2 profile produced no extra light — the transmission term "
                               "never reached the frame. off mean="
                << offStats.MeanLuma
                << " on mean=" << onStats.MeanLuma << ", " << backlitDiff.ChangedPixels << " px differ, max delta "
                << backlitDiff.MaxDelta;
            EXPECT_GT(backlitDiff.ChangedPixels, 0u) << pathName << ": the two captures are byte-identical";

            // CLAIM 1b — THE TRANSMISSION IS LOCALISED BY THE THICKNESS MAP.
            //
            // The strongest single assertion in this fixture, because it is the
            // one that separates a working feature from the failure the issue
            // names: a term that ignored the map, or read a constant, would
            // brighten both halves equally and satisfy every measurement above.
            // Here the THIN half must outglow the THICK half in the SAME frame.
            //
            // And the control must NOT show the split, which is what proves the
            // asymmetry comes from the transmission term rather than from the
            // scene's own lighting or from the map leaking into another channel.
            const HalfStats onHalves = MeasureHalves(backlitOn);
            const HalfStats offHalves = MeasureHalves(backlitOff);
            ASSERT_GT(onHalves.LeftSamples, 0u);
            ASSERT_GT(onHalves.RightSamples, 0u);

            // DIRECTION-AGNOSTIC, deliberately. Which SCREEN side the thin
            // band lands on is a property of CreateSphere's UV winding and of
            // the camera's handedness — neither of which is this test's
            // contract, and both of which would make it fail for a reason that
            // has nothing to do with transmission. What IS the contract is that
            // the two thicknesses shade DIFFERENTLY, and that the control shows
            // no such split.
            const f32 onBrighter = std::max(onHalves.LeftMeanLuma, onHalves.RightMeanLuma);
            const f32 onDarker = std::min(onHalves.LeftMeanLuma, onHalves.RightMeanLuma);

            EXPECT_GT(onBrighter, onDarker * 1.25f)
                << pathName << ": the two thicknesses shade almost identically (" << onHalves.LeftMeanLuma
                << " left vs " << onHalves.RightMeanLuma
                << ") — the thickness MAP is not modulating the transmission term, so the feature is transmitting a "
                   "constant and would glow through a skull as readily as through an ear.";

            EXPECT_LT(std::abs(offHalves.LeftMeanLuma - offHalves.RightMeanLuma),
                      0.05f * std::max(offHalves.LeftMeanLuma, 1.0e-4f))
                << pathName << ": the CONTROL capture already differs across the midline (" << offHalves.LeftMeanLuma
                << " vs " << offHalves.RightMeanLuma
                << "), so the split measured above is not attributable to the transmission term.";

            // CLAIM 3. And the extra light must be WARM. Measured as a shift in
            // red's share, which is scale-free — so this is about colour
            // balance and not merely about claim 1 again.
            EXPECT_GT(onStats.MeanRedFraction, offStats.MeanRedFraction)
                << pathName << ": the transmitted light is not redder than the frame it joined — a grey glow is wax, "
                               "not skin. off="
                << offStats.MeanRedFraction << " on=" << onStats.MeanRedFraction;

            // ---- CLAIM 2: FRONT-LIT IDENTITY -------------------------------
            // THE CLAIM THAT RULES OUT THE UNIFORMLY EMISSIVE HEAD, and the one
            // that a single lighting condition cannot make.
            SetLightDirection({ 0.0f, 0.0f, -1.0f });

            Capture frontlitOff;
            Capture frontlitOn;
            ASSERT_TRUE(CaptureFrame(path, false, std::string("SkinTransmissionOff_GL_") + pathName + "_Frontlit",
                                     frontlitOff))
                << pathName << ": readback failed";
            ASSERT_TRUE(CaptureFrame(path, true, std::string("SkinTransmission_GL_") + pathName + "_Frontlit",
                                     frontlitOn))
                << pathName << ": readback failed";

            const Difference frontlitDiff = Diff(frontlitOff, frontlitOn);
            const FrameStats frontOff = MeasureSubject(frontlitOff);
            const FrameStats frontOn = MeasureSubject(frontlitOn);

            // ---- THE NOISE FLOOR, MEASURED RATHER THAN ASSUMED -------------
            //
            // Claim 2 is an "these two frames agree" claim, and that cannot be
            // asserted against an absolute number on this renderer: capturing
            // the SAME configuration twice does not give byte-identical frames.
            // Several passes in the composite carry frame-to-frame state (the
            // temporal history #1241's diffusion feeds, the jittered sampling
            // above it), so an absolute tolerance is really a claim about how
            // noisy the renderer is — which is not what this test is about, and
            // which would make the test fail for reasons that have nothing to do
            // with transmission.
            //
            // So the floor is MEASURED: the version-2 arm is captured twice with
            // nothing changed between them, and claim 2 asks that the front-lit
            // A/B sits inside that same variation. That is the "same-mode
            // control" discipline, and it makes the assertion say what it means
            // — "front lighting changes the frame no more than re-rendering it
            // does" — instead of encoding today's noise as a constant.
            //
            // The floor is NOT allowed to swallow the claim: it is compared
            // against the BACKLIT delta below, so a renderer that became noisy
            // enough to hide the term fails there rather than passing here.
            Capture frontlitRepeat;
            ASSERT_TRUE(CaptureFrame(path, true, std::string("SkinTransmission_GL_") + pathName + "_FrontlitRepeat",
                                     frontlitRepeat))
                << pathName << ": readback failed";
            const Difference noiseFloor = Diff(frontlitOn, frontlitRepeat);

            EXPECT_LE(frontlitDiff.MaxDelta, std::max(noiseFloor.MaxDelta, 1u))
                << pathName << ": FRONT lighting changed the frame by " << frontlitDiff.MaxDelta << "/255 across "
                << frontlitDiff.ChangedPixels << " px, while re-rendering the SAME configuration changed it by only "
                << noiseFloor.MaxDelta << "/255 across " << noiseFloor.ChangedPixels
                << " px — so the difference is the transmission term, not renderer noise. The term does not vanish "
                   "toward front lighting, which is the uniformly emissive head the issue forbids. front off mean="
                << frontOff.MeanLuma << " on mean=" << frontOn.MeanLuma;

            // The MEAN is the other half of claim 2, and it is the half an
            // absolute per-pixel tolerance cannot make: a term that fired under
            // front lighting would ADD light over the whole subject, so the mean
            // would move in ONE direction. Noise moves it either way and by far
            // less. 0.5% of the control's own mean is an order of magnitude
            // below the backlit lift claim 1 measures.
            EXPECT_LT(std::abs(frontOn.MeanLuma - frontOff.MeanLuma), 0.005f * frontOff.MeanLuma)
                << pathName << ": front-lit mean luma moved from " << frontOff.MeanLuma << " to " << frontOn.MeanLuma
                << " — a term that vanishes toward front lighting cannot shift the subject's mean brightness.";

            // And the control that keeps claim 2 from being vacuous: the backlit
            // pair must differ by MUCH more than BOTH the front-lit pair and the
            // measured noise floor, or the captures could be measuring a term
            // that never fires, or a renderer too noisy to measure at all.
            EXPECT_GT(backlitDiff.MaxDelta, frontlitDiff.MaxDelta + 4u)
                << pathName << ": the backlit and front-lit A/Bs differ by a similar amount (" << backlitDiff.MaxDelta
                << " vs " << frontlitDiff.MaxDelta << "), so this test is not distinguishing direction at all";
            EXPECT_GT(backlitDiff.MaxDelta, noiseFloor.MaxDelta + 4u)
                << pathName << ": the backlit A/B (" << backlitDiff.MaxDelta
                << "/255) is within the renderer's own frame-to-frame variation (" << noiseFloor.MaxDelta
                << "/255), so claim 1 above is measuring noise and this whole fixture is blind.";
        }

        fs::path m_ProjectDir;
        Ref<EditorAssetManager> m_AssetManager;
        AssetHandle m_TransmittingProfile = 0;
        AssetHandle m_DiffusionOnlyProfile = 0;
        Entity m_Sphere;
        Entity m_Blocker;
    };

    // ---- The three raster paths, one cell each ------------------------------

    TEST_F(SkinTransmissionScene, ForwardTransmitsWhenBacklitAndVanishesWhenFrontLit)
    {
        ExpectTransmissionOnPath(RenderingPath::Forward, "Forward");
    }

    TEST_F(SkinTransmissionScene, ForwardPlusTransmitsWhenBacklitAndVanishesWhenFrontLit)
    {
        ExpectTransmissionOnPath(RenderingPath::ForwardPlus, "ForwardPlus");
    }

    TEST_F(SkinTransmissionScene, DeferredTransmitsWhenBacklitAndVanishesWhenFrontLit)
    {
        ExpectTransmissionOnPath(RenderingPath::Deferred, "Deferred");
    }

    // ---- CLAIM 4: occlusion -------------------------------------------------

    // The shared-visibility claim, which is the one that separates this term
    // from an "unshadowed ambient constant" the issue rules out. Deferred only:
    // the visibility factor is computed by the same shared body on all three
    // paths, and one demonstration of the gate is what the criterion asks for.
    TEST_F(SkinTransmissionScene, AnOccluderCollapsesTheTransmittedGlow)
    {
        SetLightDirection({ 0.0f, 0.0f, 1.0f });

        SetBlockerEngaged(false);
        Capture unoccluded;
        ASSERT_TRUE(CaptureFrame(RenderingPath::Deferred, true, "SkinTransmission_GL_Deferred_Oblique", unoccluded));

        SetBlockerEngaged(true);
        Capture occluded;
        ASSERT_TRUE(CaptureFrame(RenderingPath::Deferred, true, "SkinTransmission_GL_Deferred_Occluded", occluded));

        // The version-1 floor: with the blocker in place, the transmitted term
        // should be gone, so the frame should sit at or below what the control
        // profile renders unoccluded.
        SetBlockerEngaged(true);
        Capture occludedControl;
        ASSERT_TRUE(
            CaptureFrame(RenderingPath::Deferred, false, "SkinTransmissionOff_GL_Deferred_Occluded", occludedControl));
        SetBlockerEngaged(false);
        Capture unoccludedControl;
        ASSERT_TRUE(
            CaptureFrame(RenderingPath::Deferred, false, "SkinTransmissionOff_GL_Deferred_Oblique", unoccludedControl));

        const FrameStats unoccludedStats = MeasureSubject(unoccluded);
        const FrameStats occludedStats = MeasureSubject(occluded);

        EXPECT_LT(occludedStats.MeanLuma, unoccludedStats.MeanLuma)
            << "a blocker between the sun and the thin region did not reduce the glow — the transmission term is not "
               "gated by the shared visibility factor, so a shadowed ear still transmits. unoccluded="
            << unoccludedStats.MeanLuma << " occluded=" << occludedStats.MeanLuma;

        // And the A/B against the control under the SAME occlusion: with the
        // blocker in, version 2 and version 1 must be close, because the term
        // that distinguished them has been shadowed away.
        const Difference occludedDiff = Diff(occludedControl, occluded);
        const Difference unoccludedDiff = Diff(unoccludedControl, unoccluded);
        EXPECT_LT(occludedDiff.MaxDelta, unoccludedDiff.MaxDelta)
            << "occluding the light did not bring the two transport versions closer together (" << occludedDiff.MaxDelta
            << " vs " << unoccludedDiff.MaxDelta << ")";
    }

    // ---- The conservative fallback, observed rather than asserted in prose --

    // A version-2 profile on a material with NO authored thickness must render
    // the control frame AND say so in the counters. Both halves matter: the
    // first is the conservative fallback, the second is the house rule that a
    // path which cannot do its job says so countably.
    TEST_F(SkinTransmissionScene, AMissingThicknessTransmitsNothingAndIsCounted)
    {
        SetLightDirection({ 0.0f, 0.0f, 1.0f });
        SetBlockerEngaged(false);

        Renderer3D::GetSkinProfileTable().Reset();
        ASSERT_TRUE(static_cast<bool>(m_Sphere));
        m_Sphere.GetComponent<MaterialComponent>().m_Material.SetThicknessFactor(0.0f);

        Capture noThickness;
        ASSERT_TRUE(CaptureFrame(RenderingPath::Deferred, true, "SkinTransmission_GL_Deferred_NoThickness",
                                 noThickness));

        EXPECT_GT(Renderer3D::GetSkinProfileTable().GetTransmissionFallbackCount(
                      SkinTransmissionFallbackReason::NoThickness),
                  0ULL)
            << "a version-2 profile on a material with no thickness must be COUNTED — a head that quietly stopped "
               "transmitting looks exactly like a head that never should have";

        // Restore, then confirm the same scene DOES transmit with a thickness —
        // so the zero above is the fallback and not simply a dark frame.
        m_Sphere.GetComponent<MaterialComponent>().m_Material.SetThicknessFactor(kThicknessFactorMetres);
        Capture withThickness;
        ASSERT_TRUE(CaptureFrame(RenderingPath::Deferred, true, "SkinTransmission_GL_Deferred_WithThickness",
                                 withThickness));

        EXPECT_GT(MeasureSubject(withThickness).MeanLuma, MeasureSubject(noThickness).MeanLuma)
            << "authoring a thickness made no difference, so the fallback test above proves nothing";
    }

} // namespace OloEngine::Tests

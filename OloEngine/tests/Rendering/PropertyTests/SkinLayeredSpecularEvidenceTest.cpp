// OLO_TEST_LAYER: L8
// =============================================================================
// SkinLayeredSpecularEvidenceTest.cpp — the second lobe must not touch the
// diffusion, the mixture must not manufacture energy, and the filtered
// roughness must stop the pores sparkling. Issue #1243.
//
// WHY THIS TEST SHAPE. The issue's first three acceptance criteria each fail
// into a DIFFERENT wrong frame, and two of the three look fine:
//
//   * a layered specular that also moved the DIFFUSE half renders a head whose
//     subsurface scattering has been quietly re-weighted. It looks like a
//     slightly different — often nicer — skin tone, and nothing about it says
//     "the specular change leaked".
//   * an ADDITIVE second lobe renders a brighter head. That reads as an
//     improvement right up until someone checks a furnace test, which is why
//     the convexity is measured on real frames here and not only swept on the
//     CPU.
//   * a filter that does nothing renders a perfectly good STILL. Sparkle is a
//     property of a sequence, so no single capture can fail on it, and a
//     screenshot reviewer cannot see it at all.
//
// So each claim is measured A against B on the SAME scene, with the only
// difference being the authored profile — and each is measured against this
// fixture's own SAME-MODE REPEAT FLOOR rather than against zero, because two
// captures of an unchanged scene are not byte-identical here (see
// MeasureRepeatFloor):
//
//   1. THE DIFFUSE AOV MOVES NO MORE THAN A REPEAT CAPTURE DOES between
//      transport version 2 and a version-3 profile whose only change is a lobe
//      mixture. This is the first acceptance criterion — "does not erase the
//      underlying diffusion" — and it should hold by construction, because
//      #1231 put the two halves in separate outputs and the mixture only ever
//      touches one.
//   2. THE SPECULAR AOV MOVES MUCH MORE, so claim 1 is not passing because the
//      feature is inert or because the floor swallowed it.
//   3. A NEUTRAL VERSION-3 PROFILE RENDERS THE VERSION-2 FRAME, on every raster
//      path. The same claim as 1 from the other side, and the only one the
//      forward paths can make — MaterialDebugView is deferred-only.
//   4. ENERGY, PER PIXEL. The w = 0.5 capture lies between the w = 0 and w = 1
//      captures at EVERY pixel. Not as a mean: widening a lobe redistributes
//      energy rather than changing the total, so the three means agree to within
//      half a level and the mean cannot discriminate at all. The per-pixel
//      statement is also the stronger one — an additive mixture violates it
//      everywhere at once.
//   5. SPARKLE, measured on a ROTATING SPHERE. A sphere's silhouette does not
//      move when it spins, so every pixel's surface content resamples while the
//      shape stays put — which isolates the high-frequency shading change from
//      the enormous frame-to-frame delta any real camera move would contribute.
//      The filtered arm must show strictly less frame-to-frame change.
//   6. THE DETAIL BAND. A profile at detail strength +2 and its -1 control must
//      differ — the "baseline / detail-only AOV comparison" the fourth criterion
//      asks for, as a number.
//
// THE A/B CONTROL IS THE TRANSPORT VERSION OR AN AUTHORED FIELD, never a
// renderer switch, and deliberately: ADR 0024 makes turning this on an
// authoring act per profile, so there is no renderer-level kill switch to use.
// Using the version as the control tests the version gate at the same time.
//
// Evidence PNGs, named <Feature>_<Backend>_<Path>_<Cell>.png so an unrun cell is
// a missing FILE (docs/process/task-loop.md 2a). GL only — this fixture needs a
// real GL 4.6 context and SKIPs without one, so the Vulkan rows of the
// verification grid come from the live editor and can never come from here.
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
#include "OloEngine/Renderer/SkinLayeredSpecular.h"
#include "OloEngine/Renderer/SkinProfile.h"
#include "OloEngine/Renderer/SkinProfileTable.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
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

        // The surface's own roughness. Low enough that the lobe is narrow
        // against the pore normals' spread — which is the regime where variance
        // filtering matters at all, and the regime a face's oily highlight is
        // actually in. At roughness 0.8 every claim in this file would still
        // hold and none of them would mean anything.
        constexpr f32 kRoughness = 0.35f;

        // THE MIXTURE THE LAYERED ARM AUTHORS. 0.5 rather than something subtle,
        // for the reason SkinTransmissionEvidenceTest exaggerates its radii: a
        // contract test measures whether the mechanism is CONNECTED, and a
        // tolerance wide enough to see a tasteful mixture would also pass with
        // the feature off.
        constexpr f32 kLobeMix = 0.5f;
        constexpr f32 kLobeRoughnessScale = 2.5f;
        constexpr f32 kVarianceStrength = 1.0f;

        // Three skin tones, linear Rec.709 base colours — the issue's fourth
        // criterion asks for "several skin tones", and a specular claim that
        // only holds for one albedo is a claim about that albedo.
        constexpr glm::vec3 kTones[] = {
            { 0.78f, 0.60f, 0.52f }, // light
            { 0.46f, 0.31f, 0.24f }, // mid
            { 0.17f, 0.11f, 0.08f }, // deep
        };

        struct LightSetup
        {
            const char* Name;
            glm::vec3 Direction;
            f32 Intensity;
        };

        // SOFT, HARD and GRAZING, which is what the criterion names. The three
        // differ in the two things a specular lobe responds to: how much of the
        // hemisphere the light covers (intensity stands in for solid angle here,
        // since a directional light has none) and how oblique it is.
        constexpr LightSetup kLights[] = {
            { "Soft", { -0.35f, -0.55f, -0.75f }, 1.2f },
            { "Hard", { -0.20f, -0.30f, -0.93f }, 6.0f },
            // Almost in the image plane: the grazing row, where a single GGX
            // lobe is worst and where the second one earns its place.
            { "Grazing", { -0.97f, -0.10f, -0.20f }, 5.0f },
        };

        [[nodiscard]] f32 Channel(const std::vector<u8>& px, std::size_t idx, u32 channel)
        {
            return static_cast<f32>(px[idx + channel]) / 255.0f;
        }

        [[nodiscard]] f32 LumaAt(const std::vector<u8>& px, std::size_t idx)
        {
            return 0.2126f * Channel(px, idx, 0) + 0.7152f * Channel(px, idx, 1) + 0.0722f * Channel(px, idx, 2);
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

        // Measured over the CENTRAL DISC, which is where the probe sphere is —
        // not over the whole image, which is mostly background and would dilute
        // every measurement toward zero.
        [[nodiscard]] f32 MeanSubjectLuma(const Capture& capture, u64* samplesOut = nullptr)
        {
            f64 sum = 0.0;
            u64 samples = 0;
            const f32 cx = 0.5f * static_cast<f32>(capture.Width);
            const f32 cy = 0.5f * static_cast<f32>(capture.Height);
            const f32 radius = 0.28f * static_cast<f32>(capture.Width);

            for (u32 y = 0; y < capture.Height; ++y)
            {
                for (u32 x = 0; x < capture.Width; ++x)
                {
                    const f32 dx = static_cast<f32>(x) - cx;
                    const f32 dy = static_cast<f32>(y) - cy;
                    if ((dx * dx) + (dy * dy) > radius * radius)
                        continue;
                    sum += static_cast<f64>(LumaAt(capture.Pixels, capture.Index(x, y)));
                    ++samples;
                }
            }
            if (samplesOut != nullptr)
                *samplesOut = samples;
            return samples > 0 ? static_cast<f32>(sum / static_cast<f64>(samples)) : 0.0f;
        }

        struct Difference
        {
            u64 ChangedPixels = 0;
            u32 MaxDelta = 0;
            // The mean absolute luma difference over the SUBJECT DISC. This is
            // the sparkle number: a whole-frame mean would be diluted by the
            // background, which never changes and would make every arm look
            // equally stable.
            f32 MeanSubjectDelta = 0.0f;
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

            f64 sum = 0.0;
            u64 samples = 0;
            const f32 cx = 0.5f * static_cast<f32>(a.Width);
            const f32 cy = 0.5f * static_cast<f32>(a.Height);
            const f32 radius = 0.28f * static_cast<f32>(a.Width);
            for (u32 y = 0; y < a.Height && y < b.Height; ++y)
            {
                for (u32 x = 0; x < a.Width && x < b.Width; ++x)
                {
                    const f32 dx = static_cast<f32>(x) - cx;
                    const f32 dy = static_cast<f32>(y) - cy;
                    if ((dx * dx) + (dy * dy) > radius * radius)
                        continue;
                    sum += static_cast<f64>(
                        std::abs(LumaAt(a.Pixels, a.Index(x, y)) - LumaAt(b.Pixels, b.Index(x, y))));
                    ++samples;
                }
            }
            if (samples > 0)
                d.MeanSubjectDelta = static_cast<f32>(sum / static_cast<f64>(samples));
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
    } // namespace

    class SkinLayeredSpecularScene : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            SetUpScratchProject();

            // THE CONTROL ARM is transport version 2, not version 0, and that is
            // what makes the A/B isolate THIS feature. An "off" arm at version 0
            // would also switch off #1241's diffusion and #1242's transmission,
            // and the difference between the two captures would be three
            // features rather than one.
            m_ControlProfile = MakeProfile("LayeredOffControl", SkinEvaluationModel::ThicknessTransmission,
                                           /*lobeMix=*/0.0f, /*variance=*/0.0f, /*detail=*/0.0f);

            // The lobe arm. Variance filtering OFF, so claim 1's byte-identity
            // is a statement about the MIXTURE alone — the filter moves the
            // roughness, and the ambient term's kD is a function of roughness,
            // so leaving it on would couple a few levels into the diffuse half
            // through a route that has nothing to do with lobe layering.
            m_LobeOnlyProfile = MakeProfile("LobeOnly", SkinEvaluationModel::LayeredSpecular, kLobeMix,
                                            /*variance=*/0.0f, /*detail=*/0.0f);
            m_NarrowOnlyProfile = MakeProfile("NarrowOnly", SkinEvaluationModel::LayeredSpecular, 0.0f,
                                              /*variance=*/0.0f, /*detail=*/0.0f);
            m_BroadOnlyProfile = MakeProfile("BroadOnly", SkinEvaluationModel::LayeredSpecular, 1.0f,
                                             /*variance=*/0.0f, /*detail=*/0.0f);

            // The filter arms, for claim 4.
            m_FilteredProfile = MakeProfile("Filtered", SkinEvaluationModel::LayeredSpecular, kLobeMix,
                                            kVarianceStrength, /*detail=*/0.0f);
            m_UnfilteredProfile = MakeProfile("Unfiltered", SkinEvaluationModel::LayeredSpecular, kLobeMix,
                                              /*variance=*/0.0f, /*detail=*/0.0f);

            // The detail arms, for claim 5. -1 is the EXACT "detail off" control
            // — it removes precisely the band +2 deepens, rather than being a
            // second authored value that happens to look flatter.
            m_DetailOnProfile = MakeProfile("DetailOn", SkinEvaluationModel::LayeredSpecular, kLobeMix,
                                            kVarianceStrength, /*detail=*/2.0f);
            m_DetailOffProfile = MakeProfile("DetailOff", SkinEvaluationModel::LayeredSpecular, kLobeMix,
                                             kVarianceStrength, /*detail=*/-1.0f);

            Entity camera = GetScene().CreateEntity("Camera");
            camera.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 3.2f };
            auto& cameraComp = camera.AddComponent<CameraComponent>();
            cameraComp.Primary = true;
            cameraComp.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);

            Entity sun = GetScene().CreateEntity("Sun");
            auto& dirLight = sun.AddComponent<DirectionalLightComponent>();
            dirLight.m_Direction = kLights[0].Direction;
            dirLight.m_Color = { 1.0f, 1.0f, 1.0f };
            dirLight.m_Intensity = kLights[0].Intensity;
            dirLight.m_CastShadows = false;

            const Ref<Mesh> sphere = MeshPrimitives::CreateSphere(1.0f, 64);
            Entity entity = GetScene().CreateEntity("SkinSphere");
            entity.AddComponent<MeshComponent>(sphere->GetMeshSource());
            auto& materialComp = entity.AddComponent<MaterialComponent>();
            materialComp.m_Material.SetBaseColorFactor(glm::vec4(kTones[1], 1.0f));
            materialComp.m_Material.SetMetallicFactor(0.0f);
            materialComp.m_Material.SetRoughnessFactor(kRoughness);
            materialComp.m_Material.SetMaterialKind(MaterialKind::Skin);
            materialComp.m_Material.SetSkinProfileHandle(m_ControlProfile);
            // THE PORE NORMAL MAP, and without it this entire fixture measures
            // nothing. The variance filter widens the lobe by the spread of the
            // SHADING normal across a pixel; a smooth sphere has none, so every
            // filtered arm would be byte-identical to its unfiltered control and
            // claim 4 would pass by being uniformly zero. The detail band is
            // taken out of this same map, so claim 5 needs it too.
            materialComp.m_Material.SetNormalMap(MakePoreNormalMap());
            materialComp.m_Material.SetNormalScale(1.0f);
            m_Sphere = entity;

            EnableRendering(kSize, kSize);
        }

        void TearDown() override
        {
            // BEFORE anything that can fail. An ASSERT_* returns from the test
            // body, so a reset written there never runs and leaves a
            // renderer-wide switch armed for every later fixture in the process.
            Renderer3D::GetPostProcessSettings().MaterialDebug = MaterialDebugView::None;
            Renderer3D::GetSkinDiffusionSettings() = SkinDiffusionSettings{};
            Renderer3D::GetSkinProfileTable().Reset();

            m_AssetManager.Reset();
            Project::Unload();
            std::error_code ec;
            if (!m_ProjectDir.empty())
                fs::remove_all(m_ProjectDir, ec);
            RendererAttachedTest::TearDown();
        }

        // A 256x256 RGBA8 tangent-space normal map of pores: a high-frequency
        // lattice of dimples over a lower-frequency furrow pattern, which is the
        // two bands a real skin normal map carries and the two the detail
        // extraction separates.
        //
        // MIPS ON, deliberately and load-bearingly. The pore band is taken as
        // the difference between the fragment's mip and the same map two mips
        // coarser (OLO_SKIN_DETAIL_LOD_OFFSET); without a mip chain the coarse
        // tap returns the base level, the difference is exactly zero, and the
        // detail band silently does nothing for every strength an author sets.
        [[nodiscard]] Ref<Texture2D> MakePoreNormalMap()
        {
            constexpr u32 kDim = 256;
            TextureSpecification spec{};
            spec.Width = kDim;
            spec.Height = kDim;
            spec.Format = ImageFormat::RGBA8;
            spec.GenerateMips = true;
            // NOT sRGB: a normal map is a direction, not a colour. An sRGB
            // upload would bend every xy through a gamma curve and tilt the
            // whole surface.
            spec.SRGB = false;

            Ref<Texture2D> texture = Texture2D::Create(spec);
            // `Texture2D::Create` NEVER returns null, so the Ref proves nothing
            // — the RHI handle is the only thing that says whether a GL texture
            // exists. Without this the fixture would attach a map the renderer
            // resolves to "no map", shade a smooth sphere, and pass claim 1
            // while measuring nothing.
            EXPECT_TRUE(static_cast<bool>(texture)) << "the pore normal map texture was not created";
            EXPECT_TRUE(texture->GetRHIHandle().IsValid())
                << "the pore normal map has no RHI handle — Texture2D::Create handed back a live Ref for a texture "
                   "that was never created on the GPU, so u_UseNormalMap will be 0 and this fixture shades a smooth "
                   "sphere with nothing to filter.";

            std::vector<u8> pixels(static_cast<std::size_t>(kDim) * kDim * 4u, 0u);
            for (u32 y = 0; y < kDim; ++y)
            {
                for (u32 x = 0; x < kDim; ++x)
                {
                    const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kDim);
                    const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(kDim);

                    // PORES: a high-frequency lattice, near the texel rate, so
                    // the mip chain really does throw it away and the filter has
                    // something measurable to recover.
                    constexpr f32 kTwoPi = 6.2831853f;
                    const f32 poreU = std::sin(u * kTwoPi * 48.0f) * std::cos(v * kTwoPi * 48.0f);
                    const f32 poreV = std::cos(u * kTwoPi * 48.0f) * std::sin(v * kTwoPi * 48.0f);
                    // FURROWS: a low-frequency band that survives every mip, so
                    // the difference between two mips isolates the pores rather
                    // than the whole normal.
                    const f32 furrowU = std::sin(u * kTwoPi * 6.0f) * 0.5f;
                    const f32 furrowV = std::sin(v * kTwoPi * 6.0f + 1.1f) * 0.5f;

                    const f32 nx = std::clamp(0.55f * poreU + 0.30f * furrowU, -1.0f, 1.0f);
                    const f32 ny = std::clamp(0.55f * poreV + 0.30f * furrowV, -1.0f, 1.0f);

                    const std::size_t base = ((static_cast<std::size_t>(y) * kDim) + x) * 4u;
                    pixels[base + 0u] = static_cast<u8>(std::lround((nx * 0.5f + 0.5f) * 255.0f));
                    pixels[base + 1u] = static_cast<u8>(std::lround((ny * 0.5f + 0.5f) * 255.0f));
                    // Blue is reconstructed by decodeTangentNormal, never read.
                    pixels[base + 2u] = 255u;
                    pixels[base + 3u] = 255u;
                }
            }
            texture->SetData(pixels.data(), static_cast<u32>(pixels.size()));
            return texture;
        }

        [[nodiscard]] AssetHandle MakeProfile(const char* name, SkinEvaluationModel model, f32 lobeMix,
                                              f32 varianceStrength, f32 detailStrength)
        {
            auto profile = Ref<SkinProfile>::Create();
            profile->SetName(name);
            SkinProfileParameters parameters = SkinProfile::DefaultParameters();
            parameters.EvaluationModel = model;
            parameters.Specular.LobeMix = lobeMix;
            parameters.Specular.LobeRoughnessScale = kLobeRoughnessScale;
            parameters.Specular.NormalVarianceStrength = varianceStrength;
            parameters.Specular.DetailStrength = detailStrength;
            EXPECT_TRUE(profile->SetParameters(parameters))
                << name << ": the probe profile needed correcting — a bound moved";
            const AssetHandle handle = AssetManager::AddMemoryOnlyAsset<SkinProfile>(profile);
            EXPECT_NE(static_cast<u64>(handle), 0ULL) << name << ": AddMemoryOnlyAsset returned a zero handle";
            return handle;
        }

        void SetUpScratchProject()
        {
            std::error_code ec;
            m_ProjectDir = OloEngine::Tests::TempDir("skin-layered-specular-project");
            fs::create_directories(m_ProjectDir / "Assets", ec);
            ASSERT_FALSE(ec) << "failed to create the scratch project dir at " << m_ProjectDir.string();

            const fs::path projectFile = m_ProjectDir / "SkinLayeredSpecular.oloproj";
            {
                std::ofstream proj(projectFile);
                proj << "Project:\n"
                        "  Name: SkinLayeredSpecular\n"
                        "  StartScene: \"\"\n"
                        "  AssetDirectory: \"Assets\"\n"
                        "  ScriptModulePath: \"\"\n";
            }
            ASSERT_TRUE(Project::Load(projectFile)) << "Project::Load failed for " << projectFile.string();

            m_AssetManager = Ref<EditorAssetManager>::Create();
            m_AssetManager->Initialize(/*startFileWatcher=*/false);
            Project::SetAssetManager(m_AssetManager);
        }

        void SetProfile(AssetHandle handle)
        {
            ASSERT_TRUE(static_cast<bool>(m_Sphere));
            m_Sphere.GetComponent<MaterialComponent>().m_Material.SetSkinProfileHandle(handle);
        }

        void SetTone(const glm::vec3& tone)
        {
            ASSERT_TRUE(static_cast<bool>(m_Sphere));
            m_Sphere.GetComponent<MaterialComponent>().m_Material.SetBaseColorFactor(glm::vec4(tone, 1.0f));
        }

        void SetLight(const LightSetup& light)
        {
            Entity sun = GetScene().FindEntityByName("Sun");
            ASSERT_TRUE(static_cast<bool>(sun)) << "the probe scene lost its sun";
            auto& dirLight = sun.GetComponent<DirectionalLightComponent>();
            dirLight.m_Direction = light.Direction;
            dirLight.m_Intensity = light.Intensity;
        }

        void SetSphereYaw(f32 radians)
        {
            ASSERT_TRUE(static_cast<bool>(m_Sphere));
            m_Sphere.GetComponent<TransformComponent>().SetRotationEuler({ 0.0f, radians, 0.0f });
        }

        // Render one capture and write its evidence PNG. The PROFILE (or the
        // debug view) is the only thing that differs between the A and B
        // captures of a pair.
        [[nodiscard]] bool CaptureFrame(RenderingPath path, AssetHandle profile, MaterialDebugView view,
                                        const std::string& name, Capture& out)
        {
            SetProfile(profile);
            Renderer3D::GetPostProcessSettings().MaterialDebug = view;
            // The diffusion pass stays ON in EVERY arm: both the control and the
            // layered profiles diffuse (version 2 and version 3 both do), so
            // leaving it on is what isolates the specular layering.
            Renderer3D::GetSkinDiffusionSettings().Enabled = true;
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::ApplyRendererSettings();

            // Two frames: the first settles the graph rebuild a path or profile
            // change forces, the second is the one measured.
            RunFrames(2);

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

        // ---- The claim batteries, one per raster path ----------------------

        // THE SAME-MODE CONTROL, and every claim in this fixture is measured
        // against it rather than against zero.
        //
        // Two captures of the SAME profile are not guaranteed byte-identical:
        // the renderer carries temporal state across frames, so a repeat capture
        // moves a little even though nothing about the scene changed. Asserting
        // "the diffuse did not change AT ALL" would therefore be asserting
        // something about the renderer's determinism rather than about this
        // feature, and it would go red for a reason that has nothing to do with
        // the code under test.
        //
        // So the fixture measures its own floor first and states every claim
        // relative to it: the diffuse must move no more than a repeat capture
        // does, and the specular must move more. BOTH halves are needed — the
        // first alone would pass if the floor were enormous.
        [[nodiscard]] f32 MeasureRepeatFloor(RenderingPath path, AssetHandle profile, MaterialDebugView view)
        {
            Capture a;
            Capture b;
            if (!CaptureFrame(path, profile, view, std::string(), a))
                return -1.0f;
            if (!CaptureFrame(path, profile, view, std::string(), b))
                return -1.0f;
            return static_cast<f32>(Diff(a, b).MaxDelta);
        }

        // DEFERRED ONLY, and that is a property of the ENGINE rather than a gap
        // in this test: MaterialDebugView is evaluated in
        // include/DeferredLightingShared.glsl and the forward paths do not
        // implement it at all. Asking for the Diffuse AOV on Forward returns the
        // ordinary composite, which of course moves when the specular does. The
        // first version of this fixture did exactly that and produced a
        // confident failure message about a leak that was not happening.
        //
        // The forward paths make the same claim a different way, through the
        // exact-identity test below, which needs no AOV.
        void ExpectTheDiffusionSurvivesTheSecondLobe()
        {
            constexpr RenderingPath path = RenderingPath::Deferred;
            const char* p = PathName(path);
            SetLight(kLights[1]);

            const f32 floor = MeasureRepeatFloor(path, m_ControlProfile, MaterialDebugView::Diffuse);
            ASSERT_GE(floor, 0.0f) << p << ": readback failed measuring the repeat floor";

            Capture controlDiffuse;
            Capture layeredDiffuse;
            ASSERT_TRUE(CaptureFrame(path, m_ControlProfile, MaterialDebugView::Diffuse,
                                     std::string("SkinLayeredSpecOff_GL_") + p + "_DiffuseAOV", controlDiffuse))
                << p << ": readback failed";
            ASSERT_TRUE(CaptureFrame(path, m_LobeOnlyProfile, MaterialDebugView::Diffuse,
                                     std::string("SkinLayeredSpec_GL_") + p + "_DiffuseAOV", layeredDiffuse))
                << p << ": readback failed";

            const Difference diffuseDiff = Diff(controlDiffuse, layeredDiffuse);

            // CLAIM 1. Turning the second lobe on moves the DIFFUSE half no more
            // than capturing the same frame twice does.
            EXPECT_LE(static_cast<f32>(diffuseDiff.MaxDelta), floor)
                << p
                << ": THE SECOND SPECULAR LOBE MOVED THE DIFFUSE HALF. This is issue #1243's first acceptance "
                   "criterion, 'do not erase the underlying diffusion', and it is supposed to hold by "
                   "CONSTRUCTION: #1231 put the two halves in separate outputs and the mixture only ever writes to "
                   "one of them. A failure here means the layering is leaking into the term #1241's diffusion pass "
                   "blurs, so every head's subsurface scattering has been quietly re-weighted. diffuse max delta "
                << diffuseDiff.MaxDelta << " against a same-mode repeat floor of " << floor << " ("
                << diffuseDiff.ChangedPixels << " px differ)";

            // CLAIM 2. And the specular DID move, by more than the floor, so
            // claim 1 is not passing because the feature is inert or because the
            // floor swallowed everything.
            Capture controlSpecular;
            Capture layeredSpecular;
            ASSERT_TRUE(CaptureFrame(path, m_ControlProfile, MaterialDebugView::Specular,
                                     std::string("SkinLayeredSpecOff_GL_") + p + "_SpecularAOV", controlSpecular))
                << p << ": readback failed";
            ASSERT_TRUE(CaptureFrame(path, m_LobeOnlyProfile, MaterialDebugView::Specular,
                                     std::string("SkinLayeredSpec_GL_") + p + "_SpecularAOV", layeredSpecular))
                << p << ": readback failed";

            const Difference specularDiff = Diff(controlSpecular, layeredSpecular);
            EXPECT_GT(static_cast<f32>(specularDiff.MaxDelta), floor)
                << p
                << ": the layered and unlayered SPECULAR AOVs differ by no more than the repeat floor, so the "
                   "diffuse assertion above passed because the whole feature is inert rather than because it is "
                   "correctly confined. The profile never reached the shader: check the transport version gate and "
                   "the lane upload. specular max delta "
                << specularDiff.MaxDelta << ", floor " << floor;
            EXPECT_GT(specularDiff.ChangedPixels, 0u) << p << ": the two specular captures are byte-identical";
        }

        // THE NEUTRAL-IDENTITY CLAIM, which every raster path can make because
        // it needs no AOV: a transport-version-3 profile whose authored fields
        // are all NEUTRAL must render the version-2 frame.
        //
        // This is what makes "layering off" a true control rather than a third
        // variant. The acceptance criteria are demonstrated by subtracting these
        // two frames, and a baseline that is merely similar puts its own error
        // into every difference image built on it. It also covers the forward
        // paths' version gate, which the deferred AOV claim cannot.
        void ExpectANeutralVersionThreeMatchesVersionTwo(RenderingPath path)
        {
            const char* p = PathName(path);
            SetLight(kLights[1]);

            const f32 floor = MeasureRepeatFloor(path, m_ControlProfile, MaterialDebugView::None);
            ASSERT_GE(floor, 0.0f) << p << ": readback failed measuring the repeat floor";

            Capture version2;
            Capture neutralVersion3;
            ASSERT_TRUE(CaptureFrame(path, m_ControlProfile, MaterialDebugView::None,
                                     std::string("SkinLayeredSpecOff_GL_") + p + "_NeutralIdentity", version2))
                << p << ": readback failed";
            ASSERT_TRUE(CaptureFrame(path, m_NarrowOnlyProfile, MaterialDebugView::None,
                                     std::string("SkinLayeredSpec_GL_") + p + "_NeutralIdentity", neutralVersion3))
                << p << ": readback failed";

            const Difference d = Diff(version2, neutralVersion3);
            EXPECT_LE(static_cast<f32>(d.MaxDelta), floor)
                << p
                << ": A NEUTRAL VERSION-3 PROFILE DID NOT RENDER THE VERSION-2 FRAME. Every default this feature "
                   "adds is neutral, so moving a profile forward one transport version without touching any of the "
                   "new fields must change nothing. A failure here means every skin material in every scene moves "
                   "the moment its author bumps the version, which is exactly what the versioning exists to "
                   "prevent. max delta "
                << d.MaxDelta << " against a same-mode repeat floor of " << floor;
        }

        void ExpectTheMixtureIsConvexOnScreen(RenderingPath path)
        {
            const char* p = PathName(path);
            SetLight(kLights[1]);

            Capture narrow;
            Capture mixed;
            Capture broad;
            ASSERT_TRUE(CaptureFrame(path, m_NarrowOnlyProfile, MaterialDebugView::Specular,
                                     std::string("SkinLayeredSpec_GL_") + p + "_LobeMix000", narrow))
                << p << ": readback failed";
            ASSERT_TRUE(CaptureFrame(path, m_LobeOnlyProfile, MaterialDebugView::Specular,
                                     std::string("SkinLayeredSpec_GL_") + p + "_LobeMix050", mixed))
                << p << ": readback failed";
            ASSERT_TRUE(CaptureFrame(path, m_BroadOnlyProfile, MaterialDebugView::Specular,
                                     std::string("SkinLayeredSpec_GL_") + p + "_LobeMix100", broad))
                << p << ": readback failed";

            // PER PIXEL, NOT PER FRAME, and the first version of this test got
            // that wrong in an instructive way.
            //
            // It compared the MEAN luma of the three captures and asserted the
            // w = 0.5 mean fell between the other two. On Forward the three
            // means agreed to within half an 8-bit level, and the test failed
            // its own "the interval must have width" guard.
            //
            // That was not a bug: it is the feature working. Widening a GGX lobe
            // REDISTRIBUTES its energy rather than changing the total — which is
            // the whole point of the convex mixture — so the hemispherical mean
            // is very nearly conserved by construction and is close to useless
            // as a discriminator. What actually moves is WHERE the energy is:
            // the narrow arm has a tight bright core, the broad arm a dim wide
            // one. So the claim has to be made where it lives, at each pixel.
            //
            // A convex combination lies between its endpoints AT EVERY PIXEL, so
            // that is what is counted here — and it is a far stronger statement
            // than the mean version ever was, because an additive mixture
            // violates it everywhere at once.
            u64 outside = 0;
            u64 samples = 0;
            u32 worstExcess = 0;
            u32 endpointSpread = 0;
            const f32 cx = 0.5f * static_cast<f32>(mixed.Width);
            const f32 cy = 0.5f * static_cast<f32>(mixed.Height);
            const f32 radius = 0.28f * static_cast<f32>(mixed.Width);

            for (u32 y = 0; y < mixed.Height; ++y)
            {
                for (u32 x = 0; x < mixed.Width; ++x)
                {
                    const f32 dx = static_cast<f32>(x) - cx;
                    const f32 dy = static_cast<f32>(y) - cy;
                    if ((dx * dx) + (dy * dy) > radius * radius)
                        continue;

                    const std::size_t idx = mixed.Index(x, y);
                    for (u32 c = 0; c < 3; ++c)
                    {
                        const auto n = static_cast<i32>(narrow.Pixels[idx + c]);
                        const auto m = static_cast<i32>(mixed.Pixels[idx + c]);
                        const auto b = static_cast<i32>(broad.Pixels[idx + c]);
                        const i32 lo = std::min(n, b);
                        const i32 hi = std::max(n, b);
                        endpointSpread = std::max(endpointSpread, static_cast<u32>(hi - lo));
                        ++samples;

                        // One 8-bit level of slack: the endpoints are read back
                        // quantised, so a mixture landing exactly on one of them
                        // can round a hair outside without anything being wrong.
                        const i32 excess = std::max(lo - 1 - m, m - (hi + 1));
                        if (excess > 0)
                        {
                            ++outside;
                            worstExcess = std::max(worstExcess, static_cast<u32>(excess));
                        }
                    }
                }
            }
            ASSERT_GT(samples, 0u) << p << ": the measurement disc found no pixels";

            // CLAIM 3. A convex combination cannot leave the interval its two
            // endpoints span. An ADDITIVE second lobe leaves it at every pixel
            // at once, and in the direction that reads as "the new skin shader
            // looks better".
            EXPECT_EQ(outside, 0u)
                << p << ": THE MIXTURE LEFT THE INTERVAL ITS TWO LOBES SPAN at " << outside << " of " << samples
                << " channel samples, worst excess " << worstExcess
                << "/255. A convex combination is bounded by its endpoints at every pixel; an ADDITIVE one is not, "
                   "and an additive second lobe brightens every head in the game while looking like an "
                   "improvement. This is issue #1243's 'consistent energy' criterion measured on real frames.";

            // And the two endpoints really are different somewhere, or the
            // interval is a point and the assertion above holds trivially.
            EXPECT_GT(endpointSpread, 2u)
                << p << ": the w=0 and w=1 frames are the same picture, so the convexity assertion above is about "
                        "an interval of zero width. The broad lobe's roughness scale is not reaching the shader.";
        }

        // `view` is Specular on the deferred path and None on the forward ones,
        // because MaterialDebugView is a deferred-path feature. Measuring the
        // COMPOSITE on forward is not a weaker test than measuring the specular
        // AOV: the diffuse half of a skin pixel is smooth by construction — it
        // has just been through #1241's diffusion blur — so essentially all of
        // the frame-to-frame change a spinning sphere produces is specular
        // anyway. It does mean the forward numbers are not comparable with the
        // deferred ones, which is why each arm is only ever compared with its
        // own control.
        void ExpectTheFilterStopsTheSparkle(RenderingPath path, MaterialDebugView view)
        {
            const char* p = PathName(path);
            SetLight(kLights[2]); // grazing — where a narrow lobe sparkles most

            // A ROTATING SPHERE, and the choice is the whole design of this
            // measurement. A sphere's SILHOUETTE does not move when it spins, so
            // between two frames the shape on screen is identical and only the
            // surface content under each pixel has resampled. Dollying the
            // camera instead — the motion the criterion literally names — moves
            // the silhouette by tens of pixels, and the frame-to-frame delta is
            // then dominated by edge coverage rather than by shading. The
            // distance sweep is captured as PNGs below for the eye; this is the
            // number.
            constexpr i32 kSteps = 6;
            constexpr f32 kStepRadians = 0.010f; // ~0.6 degrees

            const auto measureSparkle = [&](AssetHandle profile, const char* label) -> f32
            {
                f64 total = 0.0;
                Capture previous;
                for (i32 i = 0; i < kSteps; ++i)
                {
                    SetSphereYaw(static_cast<f32>(i) * kStepRadians);
                    Capture current;
                    // Only the first and last step of each arm are written as
                    // evidence; six near-identical PNGs per arm is noise in the
                    // directory rather than evidence.
                    const std::string name = (i == 0 || i == kSteps - 1)
                                                 ? std::string("SkinLayeredSpec_GL_") + p + "_" + label + "_Spin" +
                                                       std::to_string(i)
                                                 : std::string();
                    if (!CaptureFrame(path, profile, view, name, current))
                        return -1.0f;
                    if (i > 0)
                        total += static_cast<f64>(Diff(previous, current).MeanSubjectDelta);
                    previous = std::move(current);
                }
                return static_cast<f32>(total / static_cast<f64>(kSteps - 1));
            };

            const f32 unfiltered = measureSparkle(m_UnfilteredProfile, "Unfiltered");
            ASSERT_GE(unfiltered, 0.0f) << p << ": readback failed in the unfiltered arm";
            const f32 filtered = measureSparkle(m_FilteredProfile, "Filtered");
            ASSERT_GE(filtered, 0.0f) << p << ": readback failed in the filtered arm";

            SetSphereYaw(0.0f);

            // The unfiltered arm has to actually sparkle, or the comparison is
            // between two zeros. This is the assertion that catches a normal map
            // that never bound, which is the way this fixture most plausibly
            // becomes a test of nothing.
            ASSERT_GT(unfiltered, 1.0e-4f)
                << p << ": the UNFILTERED arm shows no frame-to-frame change at all, so there is no sparkle to "
                        "remove. Either the pore normal map never reached the shader or the sphere is not rotating "
                        "— in both cases the comparison below is between two zeros.";

            // CLAIM 4.
            EXPECT_LT(filtered, unfiltered)
                << p
                << ": THE VARIANCE FILTER DID NOT REDUCE THE SPARKLE. Issue #1243's second acceptance criterion is "
                   "that pore/normal filtering avoids sparkling under motion, and this is that criterion as a "
                   "number: mean frame-to-frame luma change over a spinning sphere, filtered="
                << filtered << " unfiltered=" << unfiltered
                << ". Equal values mean the strength never reached the roughness — check that "
                   "oloSkinFilteredRoughness is called AFTER the normal map, on the final shading normal.";
        }

        // `view` for the reason the sparkle battery takes one.
        void ExpectTheDetailBandRespondsToItsControl(RenderingPath path, MaterialDebugView view)
        {
            const char* p = PathName(path);
            SetLight(kLights[1]);

            Capture detailOn;
            Capture detailOff;
            ASSERT_TRUE(CaptureFrame(path, m_DetailOnProfile, view,
                                     std::string("SkinLayeredSpec_GL_") + p + "_DetailOn", detailOn))
                << p << ": readback failed";
            ASSERT_TRUE(CaptureFrame(path, m_DetailOffProfile, view,
                                     std::string("SkinLayeredSpecOff_GL_") + p + "_DetailOff", detailOff))
                << p << ": readback failed";

            const Difference detailDiff = Diff(detailOn, detailOff);

            // CLAIM 5. The baseline / detail-only comparison the fourth
            // acceptance criterion asks for, as a number rather than as two
            // pictures somebody has to interpret.
            EXPECT_GT(detailDiff.ChangedPixels, 0u)
                << p
                << ": the DETAIL-ON and DETAIL-OFF frames are byte-identical, so the pore band is doing nothing. "
                   "The most likely cause is a normal map with no mip chain: the band is the difference between "
                   "the fragment's mip and the same map two mips coarser, and without mips those are the same tap "
                   "and the difference is exactly zero.";
            EXPECT_GT(detailDiff.MaxDelta, 1u)
                << p << ": the detail band moves pixels by at most one 8-bit level, which is indistinguishable "
                        "from dither. detail-on/off max delta "
                << detailDiff.MaxDelta;
        }

        // The tone x lighting matrix the fourth acceptance criterion asks for.
        // Captures only — the numeric claims are made above; what this adds is
        // the artefact for every cell, so an unrun combination is a missing file.
        void CaptureToneAndLightingMatrix(RenderingPath path)
        {
            const char* p = PathName(path);
            for (u32 t = 0; t < std::size(kTones); ++t)
            {
                SetTone(kTones[t]);
                for (const LightSetup& light : kLights)
                {
                    SetLight(light);
                    const std::string suffix =
                        std::string("_") + light.Name + "_Tone" + std::to_string(t);

                    Capture on;
                    Capture off;
                    ASSERT_TRUE(CaptureFrame(path, m_DetailOnProfile, MaterialDebugView::None,
                                             std::string("SkinLayeredSpec_GL_") + p + suffix, on))
                        << p << suffix << ": readback failed";
                    ASSERT_TRUE(CaptureFrame(path, m_ControlProfile, MaterialDebugView::None,
                                             std::string("SkinLayeredSpecOff_GL_") + p + suffix, off))
                        << p << suffix << ": readback failed";

                    // Every cell must be a real frame, not a black one. A silent
                    // readback failure would otherwise leave a directory full of
                    // convincing-looking evidence that shows nothing.
                    EXPECT_GT(MeanSubjectLuma(on), 0.0f)
                        << p << suffix << ": the layered capture is black — the subject did not render";
                    EXPECT_GT(Diff(on, off).ChangedPixels, 0u)
                        << p << suffix
                        << ": the layered and control frames are identical for this tone and light, so this cell "
                           "is evidence of nothing";
                }
            }
            SetTone(kTones[1]);
        }

        Ref<EditorAssetManager> m_AssetManager;
        fs::path m_ProjectDir;
        Entity m_Sphere;
        AssetHandle m_ControlProfile = 0;
        AssetHandle m_LobeOnlyProfile = 0;
        AssetHandle m_NarrowOnlyProfile = 0;
        AssetHandle m_BroadOnlyProfile = 0;
        AssetHandle m_FilteredProfile = 0;
        AssetHandle m_UnfilteredProfile = 0;
        AssetHandle m_DetailOnProfile = 0;
        AssetHandle m_DetailOffProfile = 0;
    };

    // ---- Criterion 1: the diffusion underneath is untouched ----------------

    // Deferred only: MaterialDebugView is a deferred-path feature, so this is
    // the one path on which the two halves can be inspected apart at all.
    TEST_F(SkinLayeredSpecularScene, DeferredKeepsTheDiffusionUnderTheSecondLobe)
    {
        ExpectTheDiffusionSurvivesTheSecondLobe();
    }

    // The same claim from the other side, on every path and without an AOV: a
    // neutral version-3 profile renders the version-2 frame.
    TEST_F(SkinLayeredSpecularScene, ForwardNeutralVersionThreeMatchesVersionTwo)
    {
        ExpectANeutralVersionThreeMatchesVersionTwo(RenderingPath::Forward);
    }

    TEST_F(SkinLayeredSpecularScene, ForwardPlusNeutralVersionThreeMatchesVersionTwo)
    {
        ExpectANeutralVersionThreeMatchesVersionTwo(RenderingPath::ForwardPlus);
    }

    TEST_F(SkinLayeredSpecularScene, DeferredNeutralVersionThreeMatchesVersionTwo)
    {
        ExpectANeutralVersionThreeMatchesVersionTwo(RenderingPath::Deferred);
    }

    // ---- Criterion 1: consistent energy ------------------------------------

    // Deferred only, for the AOV reason above: on a forward path the "specular"
    // capture is the whole composite, and the diffuse half in it would dominate
    // the convexity measurement with a term the mixture never touches.
    TEST_F(SkinLayeredSpecularScene, DeferredMixtureStaysBetweenItsTwoLobes)
    {
        ExpectTheMixtureIsConvexOnScreen(RenderingPath::Deferred);
    }

    // ---- Criterion 2: no sparkle -------------------------------------------

    TEST_F(SkinLayeredSpecularScene, DeferredFilteringReducesFrameToFrameSparkle)
    {
        ExpectTheFilterStopsTheSparkle(RenderingPath::Deferred, MaterialDebugView::Specular);
    }

    TEST_F(SkinLayeredSpecularScene, ForwardFilteringReducesFrameToFrameSparkle)
    {
        ExpectTheFilterStopsTheSparkle(RenderingPath::Forward, MaterialDebugView::None);
    }

    TEST_F(SkinLayeredSpecularScene, ForwardPlusFilteringReducesFrameToFrameSparkle)
    {
        ExpectTheFilterStopsTheSparkle(RenderingPath::ForwardPlus, MaterialDebugView::None);
    }

    // ---- Criterion 3 / 4: the detail band and its control ------------------

    TEST_F(SkinLayeredSpecularScene, DeferredDetailBandRespondsToItsAuthoredStrength)
    {
        ExpectTheDetailBandRespondsToItsControl(RenderingPath::Deferred, MaterialDebugView::Specular);
    }

    TEST_F(SkinLayeredSpecularScene, ForwardDetailBandRespondsToItsAuthoredStrength)
    {
        ExpectTheDetailBandRespondsToItsControl(RenderingPath::Forward, MaterialDebugView::None);
    }

    // ---- Criterion 4: several tones under soft, hard and grazing light -----

    TEST_F(SkinLayeredSpecularScene, DeferredCapturesEverySkinToneAndLightingCell)
    {
        CaptureToneAndLightingMatrix(RenderingPath::Deferred);
    }

    TEST_F(SkinLayeredSpecularScene, ForwardPlusCapturesEverySkinToneAndLightingCell)
    {
        CaptureToneAndLightingMatrix(RenderingPath::ForwardPlus);
    }

} // namespace OloEngine::Tests

// OLO_TEST_LAYER: L8
// =============================================================================
// SkinOralSurfaceEvidenceTest.cpp — lips, gums, tongue and teeth on an opening
// and closing mouth: the coat must not feed the diffusion, the cavity must stop
// the interior glowing, and enamel must not shade like mucosa. Issue #1245.
//
// WHY THIS TEST SHAPE. The issue's acceptance criteria are phrased as ABSENCES
// — "without glowing interiors or light leaks", "wet specular stays distinct
// from diffusion", "teeth and mucosa are not assigned identical skin response"
// — and an absence is exactly what a screenshot cannot show. Each of the three
// fails into a frame that looks plausible:
//
//   * a coat that ADDED to the diffuse half renders a mouth whose interior is
//     softly luminous. It reads as "nice subsurface" until someone notices the
//     inside of a closed mouth is brighter than the lips around it.
//   * a cavity term that never fires renders a perfectly good OPEN mouth. The
//     failure is only visible in the CLOSED pose, which is the pose nobody
//     screenshots, and only against the open one.
//   * teeth sharing the mucosa profile render a mouth that is merely a bit
//     waxy. Nothing about it says two materials were meant to be different.
//
// So each claim is measured A against B on the SAME scene, with the only
// difference being an authored value — and each against this fixture's own
// SAME-MODE REPEAT FLOOR rather than against zero, because two captures of an
// unchanged scene are not byte-identical here (see MeasureRepeatFloor).
//
//   1. MATERIAL ASSIGNMENT, on the CPU. The four oral parts resolve to profiles
//      that differ, in distinct slots, with different coat indices. That is the
//      first acceptance criterion as an assertion rather than as a screenshot,
//      and it is the one claim that cannot be satisfied by tuning.
//   2. THE COAT NEVER BRIGHTENS THE DIFFUSE AOV, pixel by pixel. This is "wet
//      specular stays distinct from diffusion" in its structural form: the
//      diffuse half is precisely what oloSkinDiffusionOutput hands the
//      screen-space blur, so a coat that could add to it would put its highlight
//      through the diffusion kernel.
//   3. AND THE SPECULAR AOV MOVES MUCH MORE, so claim 2 is not passing because
//      the coat is inert.
//   4. THE CAVITY DARKENS THE MOUTH INTERIOR, monotonically in the authored
//      strength, in the CLOSED pose under BACKLIGHT — which is the exact
//      configuration the thin-region transmission of issue #1242 lights an
//      interior it cannot see, and therefore the exact configuration the
//      "glowing interiors" failure lives in.
//   5. A NEUTRAL VERSION-4 PROFILE RENDERS THE VERSION-3 FRAME, on every raster
//      path. The identity arm: moving a profile forward one version must change
//      nothing until a field is authored. This is the case that has caught a
//      missed cumulative version list TWICE before (see Renderer/SkinDiffusion.cpp).
//   6. ENAMEL DOES NOT SHADE LIKE MUCOSA — swapping the teeth' profile for the
//      lips' one moves the frame far beyond the floor.
//
// AND THE OPEN/CLOSED PAIR IS CAPTURED under side and back lighting from three
// camera angles, with the component AOVs and a per-frame timing record, which is
// the fourth criterion. Those are EVIDENCE rather than assertions: "believable"
// is not a number, and a test that claimed it was would be lying about what it
// checked. The numbers this file does assert are the ones that can be wrong
// without anybody noticing.
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
#include "OloEngine/Renderer/SkinOralSurface.h"
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
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kSize = 384;

        // The authored oral values the "wet" arms use. Exaggerated against what
        // an artist would pick, for the reason SkinTransmissionEvidenceTest
        // exaggerates its radii: a contract test measures whether the mechanism
        // is CONNECTED, and a tolerance wide enough to see a tasteful coat would
        // also pass with the coat off.
        constexpr f32 kCoatStrength = 0.8f;
        constexpr f32 kCoatRoughness = 0.06f;
        constexpr f32 kCavityOcclusion = 1.0f;

        // The two indices that make teeth not-mucosa. Saliva is essentially
        // water; enamel is the real published figure, and the two differ by
        // roughly a factor of three in reflectance at normal incidence.
        constexpr f32 kSalivaIor = 1.33f;
        constexpr f32 kEnamelIor = 1.63f;

        // THE THICKNESS that makes the transmission term fire at all. Without a
        // non-zero thickness the whole cavity claim would pass by the term never
        // running — a green test that measured nothing, which is the failure
        // mode Renderer/SkinTransmission.h's NoThickness diagnostic exists for.
        constexpr f32 kThicknessMetres = 0.004f; // 4 mm of lip

        struct LightSetup
        {
            const char* Name;
            glm::vec3 Direction;
            f32 Intensity;
        };

        // SIDE and BACK, which is what the fourth criterion names, plus a front
        // key so the wet highlight has something to be a highlight of.
        //
        // THE BACKLIT ROW IS THE ONE THAT MATTERS for the cavity claim: the
        // thin-region term is driven by saturate(-dot(N, L)), so it only fires
        // where the light is BEHIND the surface — which, for a closed mouth, is
        // precisely the interior nobody can see and the shadow map cannot reach.
        constexpr LightSetup kLights[] = {
            { "Key", { -0.30f, -0.45f, -0.84f }, 4.0f },
            { "Side", { -0.94f, -0.20f, -0.28f }, 4.0f },
            { "Back", { 0.10f, -0.15f, 0.98f }, 5.0f },
        };

        struct CameraSetup
        {
            const char* Name;
            glm::vec3 Position;
            glm::vec3 RotationEuler;
        };

        // THREE ANGLES, because a wet highlight is a view-dependent term and a
        // single camera can miss it entirely: the Fresnel that makes a film read
        // as wet rises toward grazing, so the three-quarter and low views are
        // where the effect is largest and the front view is where it is
        // smallest. A capture from one angle would be a capture of one number.
        constexpr CameraSetup kCameras[] = {
            // The DISTANCES frame the whole assembly INCLUDING the dropped jaw:
            // the open pose spans about 1.25 world units vertically, and a 45-degree
            // vertical FOV shows 1.2 of that at z = 1.45. The first version of this
            // table cropped the lower lip off every open frame.
            { "Front", { 0.0f, -0.12f, 2.30f }, { 0.0f, 0.0f, 0.0f } },
            { "ThreeQuarter", { 1.42f, 0.16f, 1.86f }, { -0.08f, 0.65f, 0.0f } },
            { "Low", { 0.0f, -1.02f, 2.02f }, { 0.42f, 0.0f, 0.0f } },
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

        // THE MOUTH-INTERIOR RECT, in normalized image coordinates: the band
        // between the lips, where the tongue and the back of the cavity are.
        //
        // A RECT AND NOT THE WHOLE FRAME, because the whole frame is mostly lip
        // and background: a glow confined to the interior would be diluted by a
        // factor of ten in a full-frame mean and would sit under every tolerance
        // in this file. The rect is deliberately generous in x and tight in y —
        // the mouth is wide and the aperture is not tall.
        constexpr f32 kInteriorX0 = 0.34f;
        constexpr f32 kInteriorX1 = 0.66f;
        constexpr f32 kInteriorY0 = 0.42f;
        constexpr f32 kInteriorY1 = 0.60f;

        [[nodiscard]] f32 MeanRectLuma(const Capture& capture, f32 x0, f32 x1, f32 y0, f32 y1,
                                       u64* samplesOut = nullptr)
        {
            f64 sum = 0.0;
            u64 samples = 0;
            const auto px0 = static_cast<u32>(x0 * static_cast<f32>(capture.Width));
            const auto px1 = static_cast<u32>(x1 * static_cast<f32>(capture.Width));
            const auto py0 = static_cast<u32>(y0 * static_cast<f32>(capture.Height));
            const auto py1 = static_cast<u32>(y1 * static_cast<f32>(capture.Height));

            for (u32 y = py0; y < py1 && y < capture.Height; ++y)
            {
                for (u32 x = px0; x < px1 && x < capture.Width; ++x)
                {
                    sum += static_cast<f64>(LumaAt(capture.Pixels, capture.Index(x, y)));
                    ++samples;
                }
            }
            if (samplesOut != nullptr)
                *samplesOut = samples;
            return samples > 0 ? static_cast<f32>(sum / static_cast<f64>(samples)) : 0.0f;
        }

        [[nodiscard]] f32 MeanInteriorLuma(const Capture& capture, u64* samplesOut = nullptr)
        {
            return MeanRectLuma(capture, kInteriorX0, kInteriorX1, kInteriorY0, kInteriorY1, samplesOut);
        }

        struct Difference
        {
            u64 ChangedPixels = 0;
            u32 MaxDelta = 0;
            f32 MeanInteriorDelta = 0.0f;
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
            d.MeanInteriorDelta = std::abs(MeanInteriorLuma(a) - MeanInteriorLuma(b));
            return d;
        }

        // How many pixels of `candidate` are BRIGHTER than `reference` by more
        // than `slack` levels. The pixel-wise form of claim 2: a mean can hide a
        // bright interior behind a darkened lip, and it is the interior that
        // matters.
        [[nodiscard]] u64 CountBrighterPixels(const Capture& reference, const Capture& candidate, i32 slack)
        {
            u64 brighter = 0;
            const std::size_t count = std::min(reference.Pixels.size(), candidate.Pixels.size());
            for (std::size_t i = 0; i + 3 < count; i += 4)
            {
                for (u32 c = 0; c < 3; ++c)
                {
                    const i32 delta =
                        static_cast<i32>(candidate.Pixels[i + c]) - static_cast<i32>(reference.Pixels[i + c]);
                    if (delta > slack)
                    {
                        ++brighter;
                        break;
                    }
                }
            }
            return brighter;
        }

        // The LARGEST upward move of any channel of any pixel. The counting
        // helper above answers "did anything get brighter?"; this one answers
        // "by how much?", and the two together are what separate the coat's two
        // halves: it must brighten the specular a great deal and the diffuse not
        // at all.
        [[nodiscard]] i32 MaxBrightening(const Capture& reference, const Capture& candidate)
        {
            i32 worst = 0;
            const std::size_t count = std::min(reference.Pixels.size(), candidate.Pixels.size());
            for (std::size_t i = 0; i + 3 < count; i += 4)
            {
                for (u32 c = 0; c < 3; ++c)
                {
                    const i32 delta =
                        static_cast<i32>(candidate.Pixels[i + c]) - static_cast<i32>(reference.Pixels[i + c]);
                    worst = std::max(worst, delta);
                }
            }
            return worst;
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

    class SkinOralSurfaceScene : public RendererAttachedTest
    {
      protected:
        // The four oral parts, each with its own material and its own profile.
        // NAMED rather than indexed, because the whole first acceptance
        // criterion is about them being four distinguishable things.
        Entity m_UpperLip;
        Entity m_LowerLip;
        Entity m_UpperGum;
        Entity m_LowerGum;
        Entity m_Tongue;
        Entity m_UpperTeeth;
        Entity m_LowerTeeth;
        Entity m_Throat;

        AssetHandle m_DryControlProfile{}; // transport version 3 — the A/B control
        // A SECOND HANDLE WITH IDENTICAL PARAMETERS. Not a duplicate by
        // oversight — it is the floor the identity claim is measured against.
        // See where it is built.
        AssetHandle m_DryControlTwin{};
        AssetHandle m_NeutralV4Profile{}; // version 4, every oral field at its default
        AssetHandle m_LipProfile{};       // wet mucosa, cavity off
        // THE ONE-VARIABLE CONTROL for the coat claims: byte-for-byte the lip
        // profile with CoatStrength alone at 0. See the comment where it is
        // built for why m_DryControlProfile cannot serve.
        AssetHandle m_LipDryProfile{};
        AssetHandle m_LipCavityProfile{}; // wet mucosa, cavity fully spent
        AssetHandle m_LipHalfCavityProfile{};
        AssetHandle m_TongueProfile{};
        AssetHandle m_GumProfile{};
        AssetHandle m_EnamelProfile{};

        Ref<EditorAssetManager> m_AssetManager;
        fs::path m_ProjectDir;

        void BuildScene() override
        {
            SetUpScratchProject();

            // THE CONTROL ARM is transport version 3, not version 0, and that is
            // what makes the A/B isolate THIS feature. An "off" arm at version 0
            // would also switch off #1241's diffusion, #1242's transmission and
            // #1243's lobes, and the difference between the two captures would
            // be four features rather than one.
            m_DryControlProfile = MakeProfile("OralOffControl", SkinEvaluationModel::LayeredSpecular,
                                              /*coat=*/0.0f, kSalivaIor, /*cavity=*/0.0f);

            // THE IDENTITY CLAIM'S FLOOR, and it has to be a SECOND HANDLE
            // rather than the same one twice.
            //
            // A same-handle repeat measures the renderer's frame-to-frame
            // determinism, which on this fixture is exact — floor 0. But the
            // identity claim swaps one profile ASSET for another, and that
            // changes more than the transport version: a different handle takes
            // a different skin-profile SLOT and sorts into a different
            // submission order. This assembly ENCLOSES A VOLUME, so its parts
            // interpenetrate, and along an intersection curve the depth test is
            // a tie that submission order breaks. Two pixels flipped, by six
            // levels, on Forward and Forward+ — which is what a z-fight looks
            // like and is nothing to do with the skin transport.
            //
            // So the floor is measured between two profiles that differ ONLY in
            // their handle: same version, same every field. Whatever the handle
            // change costs is then in the floor, and what is left for the claim
            // to catch is the version. A widened tolerance would have hidden the
            // same amount of a real regression; this hides none of it.
            m_DryControlTwin = MakeProfile("OralOffControlTwin", SkinEvaluationModel::LayeredSpecular,
                                           /*coat=*/0.0f, kSalivaIor, /*cavity=*/0.0f);

            // Version 4 with NOTHING authored. Claim 5's identity arm.
            m_NeutralV4Profile = MakeProfile("NeutralV4", SkinEvaluationModel::OralSurface,
                                             SkinOralParameters{}.CoatStrength, SkinOralParameters{}.CoatIor,
                                             SkinOralParameters{}.CavityOcclusion);

            m_LipProfile = MakeProfile("Lip", SkinEvaluationModel::OralSurface, kCoatStrength, kSalivaIor, 0.0f);
            // THE COAT'S OWN CONTROL, and it is NOT m_DryControlProfile.
            //
            // A transport-version A/B is the right control for the IDENTITY
            // claim, where the whole point is that moving the version changes
            // nothing. It is the WRONG control for the coat claims, and the
            // difference cost a confident, wrong failure message before it was
            // noticed: the version-3 arm and the version-4 arm were also being
            // given DIFFERENT PROFILES on the teeth, and a different profile
            // means a different ScatterColor and ScatterRadiusMM, which means a
            // DIFFERENT SCREEN-SPACE DIFFUSION KERNEL.
            //
            // The diffusion pass adds `blur(aux) - aux`. Two different kernels
            // redistribute the same diffuse energy differently, so a pixel next
            // to a brighter neighbour can come out brighter under the narrower
            // one — which looks exactly like the coat leaking into the diffuse
            // half, and is not. 160 pixels did precisely that, all of them in
            // the bright band where the teeth are, and all of them a HUE shift
            // (red up, blue down) rather than a gain.
            //
            // So the coat's control is this: the same profile, the same scatter
            // parameters, the same kernel, the same transport version, and
            // CoatStrength alone at 0.
            m_LipDryProfile = MakeProfile("LipDry", SkinEvaluationModel::OralSurface, 0.0f, kSalivaIor, 0.0f);
            m_LipCavityProfile =
                MakeProfile("LipCavity", SkinEvaluationModel::OralSurface, kCoatStrength, kSalivaIor, kCavityOcclusion);
            m_LipHalfCavityProfile =
                MakeProfile("LipHalfCavity", SkinEvaluationModel::OralSurface, kCoatStrength, kSalivaIor, 0.5f);
            // A tongue is the wettest surface in a mouth and the softest — a
            // higher coat over a rougher tissue.
            m_TongueProfile = MakeProfile("Tongue", SkinEvaluationModel::OralSurface, 0.95f, kSalivaIor, kCavityOcclusion);
            m_GumProfile = MakeProfile("Gum", SkinEvaluationModel::OralSurface, 0.55f, kSalivaIor, kCavityOcclusion);
            // ENAMEL. The one field that makes teeth not mucosa, plus a short,
            // near-achromatic scattering — dentin under enamel does not carry
            // red the way a dermis does.
            m_EnamelProfile = MakeEnamelProfile("Enamel");

            Entity camera = GetScene().CreateEntity("Camera");
            camera.GetComponent<TransformComponent>().Translation = kCameras[0].Position;
            auto& cameraComp = camera.AddComponent<CameraComponent>();
            cameraComp.Primary = true;
            cameraComp.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);

            Entity sun = GetScene().CreateEntity("Sun");
            auto& dirLight = sun.AddComponent<DirectionalLightComponent>();
            dirLight.m_Direction = kLights[0].Direction;
            dirLight.m_Color = { 1.0f, 1.0f, 1.0f };
            dirLight.m_Intensity = kLights[0].Intensity;
            dirLight.m_CastShadows = false;

            // ---- THE ORAL ASSEMBLY -----------------------------------------
            //
            // BUILT FROM PRIMITIVES, and that is a stated limitation rather than
            // a shortcut nobody mentions. The issue's scope boundary is "use
            // supplied anatomy"; the engine's supplied head is Suzanne, whose
            // mouth is a closed slit with no interior, no gums and no teeth, so
            // there is no authored oral geometry to use. What THIS fixture needs
            // is not anatomy but a cavity: a set of surfaces that enclose a
            // volume, so that "the inside of a closed mouth" is a place a pixel
            // can be. Primitives give that exactly, deterministically, and
            // without a licensed asset — and the shading claims below are
            // claims about the material, which does not know what mesh it is on.
            //
            // The LIVE EDITOR run in the PR body is where the same profiles meet
            // real head geometry.
            //
            // THE PROPORTIONS ARE NOT ARBITRARY: the parts OVERLAP in depth so
            // that the assembly ENCLOSES A VOLUME in the closed pose. A set of
            // separated slabs would leave the "interior" open to the camera and
            // to every light, and the cavity claim would then be measuring a
            // surface nobody was occluding. The first version of this fixture
            // had exactly that — floating discs with air between them — and it
            // read as a stack of plates rather than a mouth.
            m_Throat = MakePart("Throat", MeshPrimitives::CreateSphere(1.0f, 32), { 0.0f, -0.02f, -0.62f },
                                { 0.40f, 0.30f, 0.30f }, m_TongueProfile, glm::vec3(0.34f, 0.12f, 0.11f));
            m_Tongue = MakePart("Tongue", MeshPrimitives::CreateSphere(1.0f, 40), { 0.0f, -0.11f, -0.24f },
                                { 0.34f, 0.10f, 0.34f }, m_TongueProfile, glm::vec3(0.55f, 0.22f, 0.21f));
            m_UpperGum = MakePart("UpperGum", MeshPrimitives::CreateCube(), { 0.0f, 0.155f, -0.26f },
                                  { 0.46f, 0.11f, 0.24f }, m_GumProfile, glm::vec3(0.52f, 0.20f, 0.20f));
            m_LowerGum = MakePart("LowerGum", MeshPrimitives::CreateCube(), { 0.0f, -0.155f, -0.26f },
                                  { 0.46f, 0.11f, 0.24f }, m_GumProfile, glm::vec3(0.52f, 0.20f, 0.20f));
            m_UpperTeeth = MakePart("UpperTeeth", MeshPrimitives::CreateCube(), { 0.0f, 0.068f, -0.20f },
                                    { 0.42f, 0.09f, 0.15f }, m_EnamelProfile, glm::vec3(0.86f, 0.84f, 0.79f));
            m_LowerTeeth = MakePart("LowerTeeth", MeshPrimitives::CreateCube(), { 0.0f, -0.068f, -0.20f },
                                    { 0.42f, 0.09f, 0.15f }, m_EnamelProfile, glm::vec3(0.86f, 0.84f, 0.79f));
            m_UpperLip = MakePart("UpperLip", MeshPrimitives::CreateSphere(1.0f, 40), { 0.0f, 0.245f, 0.02f },
                                  { 0.58f, 0.14f, 0.22f }, m_LipProfile, glm::vec3(0.55f, 0.24f, 0.24f));
            m_LowerLip = MakePart("LowerLip", MeshPrimitives::CreateSphere(1.0f, 40), { 0.0f, -0.245f, 0.02f },
                                  { 0.58f, 0.155f, 0.24f }, m_LipProfile, glm::vec3(0.58f, 0.26f, 0.26f));

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

        // ---- Fixture construction -----------------------------------------

        [[nodiscard]] Entity MakePart(const char* name, const Ref<Mesh>& mesh, const glm::vec3& translation,
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
            material.SetRoughnessFactor(0.42f);
            material.SetMaterialKind(MaterialKind::Skin);
            material.SetSkinProfileHandle(profile);
            // THE AUTHORED THICKNESS, without which the transmission term never
            // fires and the cavity claim would pass by measuring nothing. See
            // SkinTransmissionFallbackReason::NoThickness.
            material.SetThicknessFactor(kThicknessMetres);
            // THE AO MAP, and it is what the cavity weight spends. Without it
            // OLO_MAT_AO returns a flat 1.0, mix(1, 1, anything) is 1, and every
            // cavity arm would be byte-identical to its control — claim 4 would
            // pass by being uniformly zero, which is the exact shape of a test
            // that checks nothing.
            material.SetAOMap(MakeCavityAOMap());
            material.SetOcclusionStrength(1.0f);
            return entity;
        }

        // A 128x128 R8 occlusion map that is DARK TOWARD THE CENTRE — which for
        // every part of this assembly is the side facing into the cavity.
        //
        // A RADIAL FALLOFF rather than a hand-painted mask, because the claim
        // this map supports is about the MECHANISM (does an authored occlusion
        // reach the transmitted term?), not about a particular head's UVs. A
        // real head's AO map has the same shape around a mouth for the same
        // reason: the aperture occludes itself.
        [[nodiscard]] Ref<Texture2D> MakeCavityAOMap()
        {
            if (m_CavityAO)
                return m_CavityAO;

            constexpr u32 kDim = 128;
            TextureSpecification spec{};
            spec.Width = kDim;
            spec.Height = kDim;
            spec.Format = ImageFormat::RGBA8;
            spec.GenerateMips = false;
            // NOT sRGB: an occlusion factor is a scalar, not a colour.
            spec.SRGB = false;

            Ref<Texture2D> texture = Texture2D::Create(spec);
            // `Texture2D::Create` NEVER returns null, so the Ref proves nothing
            // — the RHI handle is the only thing that says whether a GL texture
            // exists. Without this the fixture would attach a map the renderer
            // resolves to "no map", shade at a flat AO of 1, and pass claim 4
            // while measuring nothing.
            EXPECT_TRUE(static_cast<bool>(texture)) << "the cavity AO map texture was not created";
            EXPECT_TRUE(texture->GetRHIHandle().IsValid())
                << "the cavity AO map has no RHI handle — Texture2D::Create handed back a live Ref for a texture "
                   "that was never created on the GPU, so u_UseAOMap will be 0 and the cavity term has nothing to "
                   "spend.";

            std::vector<u8> pixels(static_cast<std::size_t>(kDim) * kDim * 4u, 0u);
            for (u32 y = 0; y < kDim; ++y)
            {
                for (u32 x = 0; x < kDim; ++x)
                {
                    const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kDim) - 0.5f;
                    const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(kDim) - 0.5f;
                    const f32 r = std::sqrt(u * u + v * v) * 2.0f;
                    // 0.05 at the centre of the cavity, 1.0 at the outer rim.
                    const f32 ao = std::clamp(0.05f + 0.95f * r, 0.0f, 1.0f);
                    const auto level = static_cast<u8>(std::lround(ao * 255.0f));

                    const std::size_t base = ((static_cast<std::size_t>(y) * kDim) + x) * 4u;
                    // RED is the channel sampleAO reads; the rest carry the same
                    // value so an inspector opening the PNG sees a grey map
                    // rather than a red one.
                    pixels[base + 0u] = level;
                    pixels[base + 1u] = level;
                    pixels[base + 2u] = level;
                    pixels[base + 3u] = 255u;
                }
            }
            texture->SetData(pixels.data(), static_cast<u32>(pixels.size()));
            m_CavityAO = texture;
            return texture;
        }

        [[nodiscard]] AssetHandle MakeProfile(const char* name, SkinEvaluationModel model, f32 coatStrength,
                                              f32 coatIor, f32 cavityOcclusion)
        {
            auto profile = Ref<SkinProfile>::Create();
            profile->SetName(name);
            SkinProfileParameters parameters = SkinProfile::DefaultParameters();
            parameters.EvaluationModel = model;
            // Version 4 is CUMULATIVE over 3, so the layered fields are set on
            // every arm including the control — otherwise the A/B would be
            // measuring the lobes as well as the coat.
            parameters.Specular.LobeMix = 0.35f;
            parameters.Specular.LobeRoughnessScale = 2.0f;
            parameters.Specular.NormalVarianceStrength = 0.0f;
            parameters.Oral.CoatStrength = coatStrength;
            parameters.Oral.CoatRoughness = kCoatRoughness;
            parameters.Oral.CoatIor = coatIor;
            parameters.Oral.CavityOcclusion = cavityOcclusion;
            EXPECT_TRUE(profile->SetParameters(parameters))
                << name << ": the probe profile needed correcting — a bound moved";
            const AssetHandle handle = AssetManager::AddMemoryOnlyAsset<SkinProfile>(profile);
            EXPECT_NE(static_cast<u64>(handle), 0ULL) << name << ": AddMemoryOnlyAsset returned a zero handle";
            return handle;
        }

        // Enamel is not mucosa with a different number in one field: it is a
        // different tissue, and the profile says so in three places — the coat's
        // index, the transport albedo and the mean free path. THAT is what
        // "teeth and mucosa are not assigned identical skin response" means; a
        // single differing field would be a tuning difference wearing the
        // criterion's clothes.
        [[nodiscard]] AssetHandle MakeEnamelProfile(const char* name)
        {
            auto profile = Ref<SkinProfile>::Create();
            profile->SetName(name);
            SkinProfileParameters parameters = SkinProfile::DefaultParameters();
            parameters.EvaluationModel = SkinEvaluationModel::OralSurface;
            // Near-achromatic and SHORT. A dermis's red-dominant 1.55 mm is what
            // makes skin read as skin; a tooth scattering that way reads as a
            // gum with a highlight.
            parameters.ScatterColor = { 0.92f, 0.90f, 0.88f };
            parameters.ScatterRadiusMM = { 0.42f, 0.40f, 0.38f };
            parameters.SpecularTint = { 1.0f, 1.0f, 0.98f };
            parameters.Specular.LobeMix = 0.1f;
            parameters.Specular.LobeRoughnessScale = 1.6f;
            parameters.Specular.NormalVarianceStrength = 0.0f;
            parameters.Oral.CoatStrength = 0.45f;
            parameters.Oral.CoatRoughness = 0.04f;
            parameters.Oral.CoatIor = kEnamelIor;
            parameters.Oral.CavityOcclusion = kCavityOcclusion;
            EXPECT_TRUE(profile->SetParameters(parameters))
                << name << ": the enamel profile needed correcting — a bound moved";
            const AssetHandle handle = AssetManager::AddMemoryOnlyAsset<SkinProfile>(profile);
            EXPECT_NE(static_cast<u64>(handle), 0ULL) << name << ": AddMemoryOnlyAsset returned a zero handle";
            return handle;
        }

        void SetUpScratchProject()
        {
            std::error_code ec;
            m_ProjectDir = OloEngine::Tests::TempDir("skin-oral-surface-project");
            fs::create_directories(m_ProjectDir / "Assets", ec);
            ASSERT_FALSE(ec) << "failed to create the scratch project dir at " << m_ProjectDir.string();

            const fs::path projectFile = m_ProjectDir / "SkinOralSurface.oloproj";
            {
                std::ofstream proj(projectFile);
                proj << "Project:\n"
                        "  Name: SkinOralSurface\n"
                        "  StartScene: \"\"\n"
                        "  AssetDirectory: \"Assets\"\n"
                        "  ScriptModulePath: \"\"\n";
            }
            ASSERT_TRUE(Project::Load(projectFile)) << "Project::Load failed for " << projectFile.string();

            m_AssetManager = Ref<EditorAssetManager>::Create();
            m_AssetManager->Initialize(/*startFileWatcher=*/false);
            Project::SetAssetManager(m_AssetManager);
        }

        // ---- Posing --------------------------------------------------------

        // THE JAW. Everything below the bite line moves as one, which is what a
        // jaw is; the upper assembly and the throat stay put.
        //
        // `openness` 0 is a closed mouth (the two lips meet) and 1 is a wide
        // one. The CLOSED pose is the one the cavity claim is measured in,
        // because it is the pose where the interior is invisible and therefore
        // the pose where a glow is a bug rather than a view of the tongue.
        void SetJawOpenness(f32 openness)
        {
            const f32 drop = 0.46f * std::clamp(openness, 0.0f, 1.0f);
            const auto move = [drop](Entity entity, f32 baseY)
            {
                if (!entity)
                    return;
                entity.GetComponent<TransformComponent>().Translation.y = baseY - drop;
            };
            move(m_LowerLip, -0.245f);
            move(m_LowerGum, -0.155f);
            move(m_LowerTeeth, -0.068f);
            // THE TONGUE RIDES THE JAW but not all the way: it stays higher in
            // the open mouth than the teeth it sits behind, which is what makes
            // it visible in the open pose and hidden in the closed one. A tongue
            // that dropped with the jaw exactly would be invisible in both.
            move(m_Tongue, -0.11f + 0.14f * std::clamp(openness, 0.0f, 1.0f));
        }

        void SetOralProfiles(AssetHandle mucosa, AssetHandle teeth)
        {
            const auto assign = [](Entity entity, AssetHandle handle)
            {
                if (entity)
                    entity.GetComponent<MaterialComponent>().m_Material.SetSkinProfileHandle(handle);
            };
            assign(m_UpperLip, mucosa);
            assign(m_LowerLip, mucosa);
            assign(m_UpperGum, mucosa);
            assign(m_LowerGum, mucosa);
            assign(m_Tongue, mucosa);
            assign(m_Throat, mucosa);
            assign(m_UpperTeeth, teeth);
            assign(m_LowerTeeth, teeth);
        }

        void SetLight(const LightSetup& light)
        {
            Entity sun = GetScene().FindEntityByName("Sun");
            ASSERT_TRUE(static_cast<bool>(sun)) << "the probe scene lost its sun";
            auto& dirLight = sun.GetComponent<DirectionalLightComponent>();
            dirLight.m_Direction = light.Direction;
            dirLight.m_Intensity = light.Intensity;
        }

        void SetCamera(const CameraSetup& setup)
        {
            Entity camera = GetScene().FindEntityByName("Camera");
            ASSERT_TRUE(static_cast<bool>(camera)) << "the probe scene lost its camera";
            auto& transform = camera.GetComponent<TransformComponent>();
            transform.Translation = setup.Position;
            transform.SetRotationEuler(setup.RotationEuler);
        }

        // ---- Capture -------------------------------------------------------

        // Render one capture and write its evidence PNG. The PROFILE (or the
        // pose, or the debug view) is the only thing that differs between the A
        // and B captures of a pair.
        [[nodiscard]] bool CaptureFrame(RenderingPath path, AssetHandle mucosa, AssetHandle teeth,
                                        MaterialDebugView view, const std::string& name, Capture& out,
                                        f64* millisecondsPerFrame = nullptr)
        {
            SetOralProfiles(mucosa, teeth);
            Renderer3D::GetPostProcessSettings().MaterialDebug = view;
            // The diffusion pass stays ON in EVERY arm: both the control and the
            // oral profiles diffuse (version 3 and version 4 both do), so
            // leaving it on is what isolates the coat.
            Renderer3D::GetSkinDiffusionSettings().Enabled = true;
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::ApplyRendererSettings();

            // Two frames: the first settles the graph rebuild a path or profile
            // change forces, the second is the one measured.
            RunFrames(2);

            if (millisecondsPerFrame != nullptr)
            {
                // THE TIMING RECORD the fourth criterion asks for. WALL CLOCK
                // over several frames after the graph has already settled, NOT a
                // GPU timer query: this fixture renders 384x384 offscreen on a
                // box that also hosts CI runners, so the absolute number is not
                // a benchmark and is not reported as one. What it IS good for is
                // the RELATIVE cost of the dry and wet arms measured back to
                // back under identical conditions, which is the question the
                // criterion actually asks — "what did this feature cost?".
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
        // against it rather than against zero.
        //
        // Two captures of the SAME profile are not guaranteed byte-identical:
        // the renderer carries temporal state across frames, so a repeat capture
        // moves a little even though nothing about the scene changed. Asserting
        // "the diffuse did not change AT ALL" would therefore be asserting
        // something about the renderer's determinism rather than about this
        // feature, and it would go red for a reason that has nothing to do with
        // the code under test.
        [[nodiscard]] f32 MeasureRepeatFloor(RenderingPath path, AssetHandle mucosa, AssetHandle teeth,
                                             MaterialDebugView view)
        {
            Capture a;
            Capture b;
            if (!CaptureFrame(path, mucosa, teeth, view, std::string(), a))
                return -1.0f;
            if (!CaptureFrame(path, mucosa, teeth, view, std::string(), b))
                return -1.0f;
            return static_cast<f32>(Diff(a, b).MaxDelta);
        }

      private:
        Ref<Texture2D> m_CavityAO;
    };

    // =========================================================================
    // CLAIM 1 — the material assignment, on the CPU
    // =========================================================================

    // RUNS WITHOUT A GPU, deliberately: the first acceptance criterion is about
    // what the four parts are ASSIGNED, and that is answerable without rendering
    // anything. A criterion whose only evidence needed a GL 4.6 context would be
    // unverifiable on the Linux CI this repo runs.
    TEST(SkinOralSurfaceAssignment, TheFourOralPartsDoNotShareOneResponse)
    {
        SkinProfileParameters lip = SkinProfile::DefaultParameters();
        lip.EvaluationModel = SkinEvaluationModel::OralSurface;
        lip.Oral.CoatStrength = 0.8f;
        lip.Oral.CoatIor = kSalivaIor;
        ASSERT_TRUE(lip.Sanitize());

        SkinProfileParameters tongue = lip;
        tongue.Oral.CoatStrength = 0.95f;
        tongue.Oral.CavityOcclusion = 1.0f;

        SkinProfileParameters gum = lip;
        gum.Oral.CoatStrength = 0.55f;
        gum.Oral.CavityOcclusion = 1.0f;

        SkinProfileParameters enamel = SkinProfile::DefaultParameters();
        enamel.EvaluationModel = SkinEvaluationModel::OralSurface;
        enamel.ScatterColor = { 0.92f, 0.90f, 0.88f };
        enamel.ScatterRadiusMM = { 0.42f, 0.40f, 0.38f };
        enamel.Oral.CoatStrength = 0.45f;
        enamel.Oral.CoatIor = kEnamelIor;
        ASSERT_TRUE(enamel.Sanitize());

        // FOUR DISTINCT RECORDS. `operator==` here is bit-exact identity, which
        // is the right question: "did somebody assign the same profile to two
        // parts and call it authoring?".
        EXPECT_FALSE(lip == tongue);
        EXPECT_FALSE(lip == gum);
        EXPECT_FALSE(lip == enamel);
        EXPECT_FALSE(tongue == gum);
        EXPECT_FALSE(tongue == enamel);
        EXPECT_FALSE(gum == enamel);

        // AND THE MUCOSA/ENAMEL SPLIT IS PHYSICAL, not just different. The three
        // places it differs are the three that decide what a surface looks like:
        // how much light comes back off the film, how far what got in travels,
        // and what colour survives the trip.
        const glm::vec4 lipLane = SkinOralLane(lip);
        const glm::vec4 enamelLane = SkinOralLane(enamel);
        EXPECT_GT(enamelLane.z, 2.5f * lipLane.z)
            << "TEETH AND MUCOSA HAVE THE SAME COAT REFLECTANCE. Enamel's index is 1.63 and saliva's is 1.33; if "
               "these agree, somebody left the teeth profile at the mucosa default and the third acceptance "
               "criterion is not met however good the frame looks.";
        EXPECT_LT(enamel.ScatterRadiusMM.r, 0.5f * lip.ScatterRadiusMM.r)
            << "the teeth are scattering as far as a dermis does — that is a gum with a highlight, not a tooth";
        EXPECT_LT(enamel.ScatterColor.r - enamel.ScatterColor.b, 0.1f)
            << "the teeth' transport albedo is red-dominant like skin's; enamel is near-achromatic";
    }

    // =========================================================================
    // CLAIMS 2, 3 — the wet specular stays out of the diffusion
    // =========================================================================

    class SkinOralSurfaceWetTest : public SkinOralSurfaceScene
    {
    };

    // DEFERRED ONLY, and that is a property of the ENGINE rather than a gap in
    // this test: MaterialDebugView is evaluated in
    // include/DeferredLightingShared.glsl and the forward paths do not implement
    // it at all. Asking for the Diffuse AOV on Forward returns the ordinary
    // composite, which of course moves when the specular does.
    //
    // The forward paths make the related claim a different way, through the
    // exact-identity test below, which needs no AOV.
    TEST_F(SkinOralSurfaceWetTest, TheCoatNeverFeedsTheDiffusionAndTheSpecularMovesInstead)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr RenderingPath path = RenderingPath::Deferred;
        const char* p = PathName(path);
        SetLight(kLights[0]);
        SetCamera(kCameras[0]);
        SetJawOpenness(1.0f);

        // ONE PROFILE ON BOTH SLOTS IN BOTH ARMS, so the ONLY thing that differs
        // between the captures below is CoatStrength. Assigning the enamel
        // profile to the teeth here would also change their diffusion kernel —
        // see where m_LipDryProfile is built. Enamel gets its own test.
        const f32 floor =
            MeasureRepeatFloor(path, m_LipDryProfile, m_LipDryProfile, MaterialDebugView::Diffuse);
        ASSERT_GE(floor, 0.0f) << p << ": readback failed measuring the repeat floor";

        Capture dryDiffuse;
        Capture wetDiffuse;
        Capture drySpecular;
        Capture wetSpecular;

        ASSERT_TRUE(CaptureFrame(path, m_LipDryProfile, m_LipDryProfile, MaterialDebugView::Diffuse,
                                 std::string("HeadOralOff_GL_") + p + "_DiffuseAOV", dryDiffuse))
            << p << ": readback failed";
        ASSERT_TRUE(CaptureFrame(path, m_LipProfile, m_LipProfile, MaterialDebugView::Diffuse,
                                 std::string("HeadOral_GL_") + p + "_DiffuseAOV", wetDiffuse))
            << p << ": readback failed";
        ASSERT_TRUE(CaptureFrame(path, m_LipDryProfile, m_LipDryProfile, MaterialDebugView::Specular,
                                 std::string("HeadOralOff_GL_") + p + "_SpecularAOV", drySpecular))
            << p << ": readback failed";
        ASSERT_TRUE(CaptureFrame(path, m_LipProfile, m_LipProfile, MaterialDebugView::Specular,
                                 std::string("HeadOral_GL_") + p + "_SpecularAOV", wetSpecular))
            << p << ": readback failed";

        // CLAIM 2. The coat may DARKEN the diffuse half — it takes energy from
        // the tissue, which is the whole of the energy argument — but it must
        // never ADD to it. Stated pixel-wise.
        //
        // THE SLACK IS THE MEASURED REPEAT FLOOR AND NOTHING ADDED TO IT. On
        // this fixture that floor is 0 — two captures of an unchanged scene are
        // byte-identical here — so the claim below is EXACT, which is what the
        // partition in Renderer/SkinOralSurface.h entitles it to be. Rounding
        // the slack up to 1 "to be safe" would have hidden a one-level leak on
        // every pixel of a head, which is precisely the size of leak nobody ever
        // notices in a frame. If a future renderer change makes the floor
        // non-zero, this widens by exactly that much and no more.
        const auto slack = static_cast<i32>(floor);
        const u64 brighter = CountBrighterPixels(dryDiffuse, wetDiffuse, slack);
        EXPECT_EQ(brighter, 0ULL)
            << p << ": THE COAT BRIGHTENED THE DIFFUSE HALF at " << brighter << " pixels (slack " << slack
            << " levels). That half is exactly what oloSkinDiffusionOutput hands the screen-space blur, so any "
               "energy the coat puts there is blurred across the surface and re-added — which is how a wet lip "
               "ends up making the tissue under it glow. Renderer/SkinOralSurface.h states the coat is a "
               "PARTITION: it attenuates the diffuse and adds ONLY to the specular.";

        // CLAIM 3. And the specular got BRIGHTER — so claim 2 is not passing
        // because the coat is inert.
        //
        // THE BAR IS THE REPEAT FLOOR, NOT A MULTIPLE OF IT, and the history is
        // the reason. This claim used to demand 4x the floor. When #1245 landed,
        // the largest upward move here was 17 levels. b783876d6 (the coat's
        // Fresnel takes dot(V, H), not dot(N, H)) correctly shrank the head-on
        // highlight to 7. And 4 of those 7 were never the coat: until #1394 the
        // skin diffusion pass added its high-pass of the DIFFUSE half on top of
        // every material debug view, and the coat changes the diffuse half.
        // With that leak fixed (RenderPipeline.cpp, SkinDiffusionRunsThisFrame)
        // the coat's clean gain in this AOV is 3 levels, on 2-3 pixels, at this
        // pose and at Side/ThreeQuarter alike.
        //
        // The floor on this fixture is 0 (two captures are byte-identical), so
        // "above the floor" is still an exact statement that the coat reached
        // the specular half. How LARGE the wet highlight should be is a
        // separate question, tracked as #1421 (a follow-up to the closed
        // #1245) rather than answered by a threshold chosen to make this pass.
        //
        // THE INSTRUMENT IS THE LARGEST UPWARD MOVE, NOT A PIXEL COUNT, and the
        // first version of this test got that wrong in a way worth recording.
        // It asserted that the coat changed more specular PIXELS than diffuse
        // ones, on the intuition that a highlight is concentrated. The opposite
        // is true and is correct: the coat attenuates the diffuse EVERYWHERE the
        // film is, so nearly every lit pixel moves by a level or two (47 235 of
        // them, by at most 21 levels down), while the specular it adds is
        // concentrated where the half-vector is (32 233 pixels, by up to 20
        // levels UP, when this was written — see the history above for what
        // those figures are today). Counting pixels measures how WIDE each change is; the
        // claim is about how BIG it is, and the two answers point in opposite
        // directions.
        const Difference specularDiff = Diff(drySpecular, wetSpecular);
        const i32 specularUp = MaxBrightening(drySpecular, wetSpecular);
        EXPECT_GT(static_cast<f32>(specularUp), floor)
            << p << ": THE SPECULAR AOV GAINED ALMOST NOTHING (" << specularUp
            << " levels against a floor of " << floor
            << "). The coat is not reaching the frame at all, which makes every other claim in this file pass "
               "vacuously — claim 2 in particular, since a coat that does nothing brightens nothing. Check the "
               "lane packing at submission and the version gate in oloSkinOralLaneFor.";
        EXPECT_GT(static_cast<f32>(specularDiff.MaxDelta), floor)
            << p << ": the specular AOV barely moved in either direction";
    }

    // =========================================================================
    // CLAIM 4 — the cavity stops the interior glowing
    // =========================================================================

    class SkinOralSurfaceCavityTest : public SkinOralSurfaceScene
    {
    };

    TEST_F(SkinOralSurfaceCavityTest, TheCavityDarkensAClosedMouthInteriorMonotonically)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr RenderingPath path = RenderingPath::Deferred;
        const char* p = PathName(path);

        // THE BACKLIT, CLOSED CONFIGURATION — the only one in which this claim
        // means anything. The thin-region term is driven by saturate(-dot(N,L)),
        // so it fires where the light is BEHIND the surface; a closed mouth's
        // interior is lit by nothing else, and no shadow map at any resolution
        // this engine runs reaches inside it.
        SetLight(kLights[2]);
        SetCamera(kCameras[0]);
        SetJawOpenness(0.0f);

        const f32 floor = MeasureRepeatFloor(path, m_LipProfile, m_EnamelProfile, MaterialDebugView::None);
        ASSERT_GE(floor, 0.0f) << p << ": readback failed measuring the repeat floor";

        Capture none;
        Capture half;
        Capture full;
        ASSERT_TRUE(CaptureFrame(path, m_LipProfile, m_EnamelProfile, MaterialDebugView::None,
                                 std::string("HeadOralOff_GL_") + p + "_ClosedBacklit", none))
            << p << ": readback failed";
        ASSERT_TRUE(CaptureFrame(path, m_LipHalfCavityProfile, m_EnamelProfile, MaterialDebugView::None,
                                 std::string("HeadOral_GL_") + p + "_ClosedBacklit_CavityHalf", half))
            << p << ": readback failed";
        ASSERT_TRUE(CaptureFrame(path, m_LipCavityProfile, m_EnamelProfile, MaterialDebugView::None,
                                 std::string("HeadOral_GL_") + p + "_ClosedBacklit_CavityFull", full))
            << p << ": readback failed";

        u64 samples = 0;
        const f32 lumaNone = MeanInteriorLuma(none, &samples);
        const f32 lumaHalf = MeanInteriorLuma(half);
        const f32 lumaFull = MeanInteriorLuma(full);

        ASSERT_GT(samples, 256ULL)
            << "the interior rect sampled almost nothing — the measurement window and the geometry no longer "
               "overlap, which would make every number below meaningless";

        // The floor expressed in the same units as the luma means, so the
        // comparison below is against this fixture's own noise and not against a
        // constant somebody picked.
        const f32 lumaFloor = std::max(floor, 1.0f) / 255.0f;

        // CLAIM 4a. Spending the occlusion darkens the interior, beyond noise.
        EXPECT_LT(lumaFull, lumaNone - lumaFloor)
            << p << ": THE CAVITY DID NOT DARKEN A CLOSED MOUTH'S INTERIOR (" << lumaFull << " against "
            << lumaNone << ", floor " << lumaFloor
            << "). The thin-region transmission of issue #1242 is the ONLY term here with no visibility of its "
               "own: the direct lobes carry lightVisibility and the ambient ladder carries the AO. If the cavity "
               "weight is not reaching it, a closed mouth is lit from inside by a light behind the head — which "
               "is the 'glowing interiors' failure the second acceptance criterion names in those words.";

        // CLAIM 4b. MONOTONE. A half-spent occlusion sits between the two, which
        // is what makes the effect a dial an author can trust rather than a
        // switch that happens to point the right way at its endpoints.
        EXPECT_LE(lumaHalf, lumaNone + lumaFloor) << p << ": half the occlusion brightened the interior";
        EXPECT_GE(lumaHalf, lumaFull - lumaFloor) << p << ": half the occlusion darkened MORE than all of it";

        // CLAIM 4c. AND THE OPEN POSE IS CAPTURED BESIDE IT, as the criterion
        // asks — with no assertion attached, because "believable" is not a
        // number and a test that claimed it was would be lying about what it
        // checked. What IS asserted is that the two poses are genuinely
        // different frames, so the pair is evidence of a mouth that opens rather
        // than two captures of the same still.
        SetJawOpenness(1.0f);
        Capture openFull;
        ASSERT_TRUE(CaptureFrame(path, m_LipCavityProfile, m_EnamelProfile, MaterialDebugView::None,
                                 std::string("HeadOral_GL_") + p + "_OpenBacklit_CavityFull", openFull))
            << p << ": readback failed";
        const Difference poseDiff = Diff(full, openFull);
        EXPECT_GT(static_cast<f32>(poseDiff.MaxDelta), 8.0f * std::max(floor, 1.0f))
            << p << ": the open and closed poses rendered the same frame — the jaw is not moving, so the "
                    "open/closed pair is not evidence of anything.";
    }

    // =========================================================================
    // CLAIM 5 — the identity arm, on every raster path
    // =========================================================================

    class SkinOralSurfaceIdentityTest : public SkinOralSurfaceScene
    {
      protected:
        void ExpectNeutralVersionFourIsTheVersionThreeFrame(RenderingPath path)
        {
            const char* p = PathName(path);
            SetLight(kLights[0]);
            SetCamera(kCameras[0]);
            SetJawOpenness(1.0f);

            // THE FLOOR IS A HANDLE SWAP, not a repeat — see m_DryControlTwin.
            Capture floorA;
            Capture floorB;
            ASSERT_TRUE(CaptureFrame(path, m_DryControlProfile, m_DryControlProfile, MaterialDebugView::None,
                                     std::string(), floorA))
                << p << ": readback failed measuring the handle-swap floor";
            ASSERT_TRUE(CaptureFrame(path, m_DryControlTwin, m_DryControlTwin, MaterialDebugView::None,
                                     std::string(), floorB))
                << p << ": readback failed measuring the handle-swap floor";
            const auto floor = static_cast<f32>(Diff(floorA, floorB).MaxDelta);

            Capture v3;
            Capture v4;
            ASSERT_TRUE(CaptureFrame(path, m_DryControlProfile, m_DryControlProfile, MaterialDebugView::None,
                                     std::string("HeadOralOff_GL_") + p + "_NeutralIdentity", v3))
                << p << ": readback failed";
            ASSERT_TRUE(CaptureFrame(path, m_NeutralV4Profile, m_NeutralV4Profile, MaterialDebugView::None,
                                     std::string("HeadOral_GL_") + p + "_NeutralIdentity", v4))
                << p << ": readback failed";

            const Difference identity = Diff(v3, v4);
            EXPECT_LE(static_cast<f32>(identity.MaxDelta), floor)
                << p << ": MOVING A PROFILE FROM TRANSPORT VERSION 3 TO 4 CHANGED THE FRAME by "
                << identity.MaxDelta << " levels across " << identity.ChangedPixels
                << " pixels, against a handle-swap floor of " << floor
                << ". Version 4's oral fields all default to neutral, so a version-4 profile with nothing "
                   "authored must render the version-3 frame to within what swapping one profile handle for an "
                   "IDENTICAL one costs — which is what that floor is.\n"
                   "\n"
                   "THIS IS THE ASSERTION THAT HAS CAUGHT A MISSED CUMULATIVE VERSION LIST TWICE ALREADY — see "
                   "the comment in Renderer/SkinDiffusion.cpp. The versions are cumulative and every list that "
                   "tests them is written out explicitly, so appending version 4 means editing each one; the "
                   "symptom of missing the diffusion kernel's list is exactly this: a head that silently loses "
                   "its subsurface scattering the moment its author moves the version forward.\n"
                   "\n"
                   "Grep for `LayeredSpecular` across .cpp, .h AND .glsl — the shader lists are the ones a "
                   "`*.glsl` grep finds, and they are not all of them.";
        }
    };

    TEST_F(SkinOralSurfaceIdentityTest, ANeutralVersionFourProfileRendersTheVersionThreeFrameOnForward)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ExpectNeutralVersionFourIsTheVersionThreeFrame(RenderingPath::Forward);
    }

    TEST_F(SkinOralSurfaceIdentityTest, ANeutralVersionFourProfileRendersTheVersionThreeFrameOnForwardPlus)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ExpectNeutralVersionFourIsTheVersionThreeFrame(RenderingPath::ForwardPlus);
    }

    TEST_F(SkinOralSurfaceIdentityTest, ANeutralVersionFourProfileRendersTheVersionThreeFrameOnDeferred)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ExpectNeutralVersionFourIsTheVersionThreeFrame(RenderingPath::Deferred);
    }

    // =========================================================================
    // CLAIM 6 — enamel does not shade like mucosa
    // =========================================================================

    class SkinOralSurfaceEnamelTest : public SkinOralSurfaceScene
    {
    };

    TEST_F(SkinOralSurfaceEnamelTest, TeethShadedAsMucosaAreADifferentFrame)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        constexpr RenderingPath path = RenderingPath::Deferred;
        const char* p = PathName(path);
        // SIDE LIGHT and an open mouth: the teeth have to be visible and lit at
        // an angle where a coat's Fresnel separates the two indices. A front key
        // on a closed mouth would compare two invisible surfaces.
        SetLight(kLights[1]);
        SetCamera(kCameras[1]);
        SetJawOpenness(1.0f);

        const f32 floor = MeasureRepeatFloor(path, m_LipCavityProfile, m_EnamelProfile, MaterialDebugView::None);
        ASSERT_GE(floor, 0.0f) << p << ": readback failed measuring the repeat floor";

        Capture enamelTeeth;
        Capture mucosaTeeth;
        ASSERT_TRUE(CaptureFrame(path, m_LipCavityProfile, m_EnamelProfile, MaterialDebugView::None,
                                 std::string("HeadOral_GL_") + p + "_Enamel", enamelTeeth))
            << p << ": readback failed";
        // THE CONTROL IS THE MUCOSA PROFILE ON THE TEETH — the exact failure the
        // criterion names, reproduced deliberately so the frame that does NOT
        // have it can be compared against something real rather than against
        // nothing.
        ASSERT_TRUE(CaptureFrame(path, m_LipCavityProfile, m_LipCavityProfile, MaterialDebugView::None,
                                 std::string("HeadOralOff_GL_") + p + "_TeethAsMucosa", mucosaTeeth))
            << p << ": readback failed";

        const Difference d = Diff(enamelTeeth, mucosaTeeth);
        EXPECT_GT(static_cast<f32>(d.MaxDelta), 6.0f * std::max(floor, 1.0f))
            << p << ": TEETH AND MUCOSA RENDERED THE SAME (" << d.MaxDelta << " levels against a floor of "
            << floor
            << "). The third acceptance criterion is that they are not assigned an identical skin response; if "
               "swapping one profile for the other does not move the frame, then whatever distinguishes them on "
               "disk is not reaching the shader.";
        EXPECT_GT(d.ChangedPixels, 512ULL)
            << p << ": only " << d.ChangedPixels
            << " pixels moved — the teeth are barely visible from this angle, so the comparison above is about a "
               "handful of texels rather than about two materials.";
    }

    // =========================================================================
    // THE EVIDENCE SWEEP — angles, poses, lighting, and the timing record
    // =========================================================================

    class SkinOralSurfaceSweepTest : public SkinOralSurfaceScene
    {
    };

    // THE FOURTH ACCEPTANCE CRITERION, which asks for close-up animation under
    // side/backlighting captured with component AOVs and timing. There is one
    // assertion here — that the captures were WRITTEN — because everything else
    // this produces is for a human to look at, and a test that dressed "looks
    // believable" up as a tolerance would be claiming to check something it
    // cannot.
    TEST_F(SkinOralSurfaceSweepTest, CaptureTheOpenClosedPairFromEveryAngleUnderSideAndBackLight)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        struct Timing
        {
            std::string Cell;
            f64 DryMs;
            f64 WetMs;
        };
        std::vector<Timing> timings;

        for (const RenderingPath path : { RenderingPath::Forward, RenderingPath::ForwardPlus,
                                          RenderingPath::Deferred })
        {
            const char* p = PathName(path);
            for (const LightSetup& light : kLights)
            {
                SetLight(light);
                for (const CameraSetup& cam : kCameras)
                {
                    SetCamera(cam);
                    for (const auto& pose : { std::pair<const char*, f32>{ "Closed", 0.0f },
                                              std::pair<const char*, f32>{ "Open", 1.0f } })
                    {
                        SetJawOpenness(pose.second);

                        const std::string cell =
                            std::string(p) + "_" + light.Name + "_" + cam.Name + "_" + pose.first;

                        // THE TIMED PAIR is taken on ONE cell per path rather
                        // than on all eighteen: a wall-clock measurement on a
                        // box that also hosts CI runners is noisy enough that
                        // eighteen of them would be eighteen different answers,
                        // and the question is what the coat costs, not what the
                        // machine was doing.
                        const bool timeThis = (light.Name == kLights[0].Name) && (cam.Name == kCameras[0].Name) &&
                                              (pose.second > 0.5f);
                        f64 dryMs = 0.0;
                        f64 wetMs = 0.0;

                        Capture dry;
                        Capture wet;
                        ASSERT_TRUE(CaptureFrame(path, m_DryControlProfile, m_DryControlProfile,
                                                 MaterialDebugView::None, "HeadOralOff_GL_" + cell, dry,
                                                 timeThis ? &dryMs : nullptr))
                            << cell << ": readback failed";
                        ASSERT_TRUE(CaptureFrame(path, m_LipCavityProfile, m_EnamelProfile,
                                                 MaterialDebugView::None, "HeadOral_GL_" + cell, wet,
                                                 timeThis ? &wetMs : nullptr))
                            << cell << ": readback failed";

                        if (timeThis)
                            timings.push_back({ cell, dryMs, wetMs });

                        // THE ONE ASSERTION: the two arms are different frames.
                        // Without it an entire eighteen-cell sweep could be
                        // eighteen copies of the dry frame and every PNG would
                        // still be on disk for a reviewer to nod at.
                        const Difference d = Diff(dry, wet);
                        EXPECT_GT(d.ChangedPixels, 256ULL)
                            << cell << ": the wet and dry arms rendered the same frame, so this cell's evidence "
                                       "shows nothing.";
                    }
                }
            }
        }

        // THE TIMING RECORD, written beside the PNGs so the PR body can cite a
        // file rather than a number somebody typed. Not a golden: it is a
        // measurement on named hardware at a named resolution, and the header
        // inside it says so.
        const fs::path timingFile = VisualOutputPath("HeadOral_Timing").replace_extension(".txt");
        std::ofstream out(timingFile);
        ASSERT_TRUE(out.is_open()) << "failed to open " << timingFile.string();
        out << "# SkinOralSurfaceEvidenceTest timing, issue #1245\n"
            << "# Offscreen " << kSize << "x" << kSize << ", wall clock over 24 settled frames, dry (transport\n"
            << "# version 3) against wet (version 4, coat " << kCoatStrength << ", cavity " << kCavityOcclusion
            << ").\n"
            << "# NOT A BENCHMARK: this box hosts CI runners for another repo, so the absolute figures move.\n"
            << "# What the pair is good for is the RELATIVE cost, measured back to back.\n"
            << "# cell, dry_ms_per_frame, wet_ms_per_frame, delta_ms, delta_pct\n";
        for (const Timing& t : timings)
        {
            const f64 delta = t.WetMs - t.DryMs;
            const f64 pct = (t.DryMs > 0.0) ? (100.0 * delta / t.DryMs) : 0.0;
            out << t.Cell << ", " << t.DryMs << ", " << t.WetMs << ", " << delta << ", " << pct << "\n";
        }
        out.close();

        EXPECT_EQ(timings.size(), 3U) << "one timed cell per raster path was expected";
    }
} // namespace OloEngine::Tests

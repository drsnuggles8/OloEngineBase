#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L8
// =============================================================================
// FurCoatAuthoringVisualEvidenceTest — issue #1251, acceptance criterion 4.
//
// "Capture both animals under controlled front/side/backlight; include
//  whisker/long-hair groups where present and avoid a uniformly fuzzy coat."
//
// Writes OloEditor/assets/tests/visual/FurCoat<Animal>_GL_<Path>_<Light>.png
// and the matching FurCoatGroupsOff_* controls. The filename carries the
// {backend} x {path} cell it covers, deliberately including the backend even
// though it is always GL here: a reader counting files then cannot mistake a
// complete set of OpenGL captures for a complete verification matrix. Every
// Vulkan cell is live-only — these fixtures need a real GL 4.6 context and skip
// without one — and is evidenced in the PR body instead.
//
// WHAT IS ASSERTED, as opposed to merely captured. "Avoid a uniformly fuzzy
// coat" is a judgement, and a judgement is not a test; a coat that is wrong in
// every way this feature can be wrong still produces a picture of a coat. So
// the criterion is decomposed into the claims that can be false while the
// picture stays plausible, and each is measured:
//
//   1. THE COAT AUTHORING REACHES THE SCREEN, on all three rendering paths.
//      A tint lane the shader never reads, a per-role stride that resolves to
//      one, a coat digest missing from the cache key — all three render exactly
//      like the control, and none is visible in a screenshot.
//   2. THE SILHOUETTE SURVIVES A BUDGET THAT THE COAT DOES NOT. Turning the
//      strand budget down must cost the frame far less of its OUTLINE than of
//      its interior, because the guard hairs are thinned last. That is
//      criterion 1's claim measured on pixels rather than on counters.
//   3. THE LAYERS ARE SEPARABLE. Hiding the undercoat and hiding the guard coat
//      must produce two DIFFERENT frames, and neither may be the full coat. A
//      visibility mask that reached the counters but not the geometry passes
//      GroomCoatAuthoringTest and fails here.
//   4. THE COAT IS NOT UNIFORM. Measured as the spatial variance of the coat's
//      own luminance: an authored coat with regional maps, clumping and
//      per-strand variation has visibly more structure than the same groom with
//      the coat switched off. This is the closest a number gets to criterion 4's
//      judgement, and the PNGs exist so a reviewer can make the judgement.
//   5. WHISKERS SURVIVE EVERYTHING. The whisker group is twelve strands; it must
//      be present at a budget that has thinned the undercoat by an order of
//      magnitude, because three missing whiskers read as damage.
//   6. MSAA, UPSCALING AND A NON-NATIVE RESOLUTION do not change what the coat
//      IS. Strands are sub-pixel (groom-strand-visibility.md), so a density
//      change is exactly the kind of thing that looks right at 1x and breaks on
//      resolve.
//
// Classification: L8 / golden image (full GL pipeline + RGBA8 readback + PNG).
// These are EVIDENCE, not SSIM goldens — the composition tier underneath can be
// stochastic by design (#1246) — so the contracts are the assertions and the
// PNGs exist so a reviewer can look at what they describe.
// =============================================================================

#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCoat.h"
#include "OloEngine/Groom/GroomCooker.h"
#include "OloEngine/Groom/GroomFibreScattering.h"
#include "OloEngine/Groom/GroomVisibility.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;

        constexpr int kChangedPixelThreshold = 8;

        [[nodiscard]] u32 CountDifferingPixels(const std::vector<u8>& frame, const std::vector<u8>& baseline)
        {
            if (frame.size() != baseline.size())
            {
                return 0;
            }
            u32 count = 0;
            for (sizet i = 0; i + 3 < frame.size(); i += 4)
            {
                for (int c = 0; c < 3; ++c)
                {
                    if (std::abs(static_cast<int>(frame[i + c]) - static_cast<int>(baseline[i + c])) >
                        kChangedPixelThreshold)
                    {
                        ++count;
                        break;
                    }
                }
            }
            return count;
        }

        // Pixels that differ from `empty` — the frame with no coat at all. This
        // is the coat's own FOOTPRINT, and its count is what a silhouette
        // measurement is made of.
        [[nodiscard]] u32 CountCoatPixels(const std::vector<u8>& frame, const std::vector<u8>& empty)
        {
            return CountDifferingPixels(frame, empty);
        }

        // The coat footprint restricted to pixels OUTSIDE the body — the OUTLINE
        // rather than the interior.
        //
        // WHY THE BODY IS THE MASK AND NOT A CONTOUR TRACE. The scene is a dark
        // sphere under a coat, so "outside the body" is exactly the set of pixels
        // where the coat is all there is: the guard hairs that stand off the
        // surface and make the animal's shape. A contour trace would have to
        // guess where the body ends; the strandless capture KNOWS, because the
        // body is the only thing in it.
        [[nodiscard]] u32 CountSilhouettePixels(const std::vector<u8>& frame, const std::vector<u8>& empty,
                                                const std::vector<u8>& background)
        {
            if (frame.size() != empty.size() || frame.size() != background.size())
            {
                return 0;
            }
            u32 count = 0;
            for (sizet i = 0; i + 3 < frame.size(); i += 4)
            {
                // A pixel where the BODY is absent (empty == background) but the
                // coat is present.
                bool bodyHere = false;
                bool coatHere = false;
                for (int c = 0; c < 3; ++c)
                {
                    bodyHere = bodyHere || std::abs(static_cast<int>(empty[i + c]) -
                                                    static_cast<int>(background[i + c])) > kChangedPixelThreshold;
                    coatHere = coatHere || std::abs(static_cast<int>(frame[i + c]) - static_cast<int>(empty[i + c])) >
                                               kChangedPixelThreshold;
                }
                if (!bodyHere && coatHere)
                {
                    ++count;
                }
            }
            return count;
        }

        // The RELATIVE spread — standard deviation over mean — of the coat's
        // luminance, across the pixels the coat occupies. "Uniformly fuzzy" is a
        // narrow spread: every pixel the same slightly-lifted grey.
        //
        // RELATIVE, NOT THE PLAIN VARIANCE, and the difference is the whole
        // measurement. A per-strand tint is a MULTIPLICATIVE perturbation, and an
        // absolute variance scales with the square of the mean — so turning shade
        // variation up, which darkens the coat overall because a tint cannot push
        // an albedo above the authored one, LOWERS the absolute variance while
        // visibly increasing the variation. Measured that way the statistic moves
        // the wrong way for a real effect; measured relative to the mean it moves
        // the way the mechanism predicts.
        //
        // Over the coat's OWN pixels rather than the whole frame, so the
        // background does not dominate it. It is a spread within ONE frame, never
        // a ratio of two tone-mapped means, which groom-fibre-scattering.md
        // rule 13 forbids.
        [[nodiscard]] f64 CoatLuminanceRelativeSpread(const std::vector<u8>& frame, const std::vector<u8>& empty)
        {
            if (frame.size() != empty.size())
            {
                return 0.0;
            }
            f64 sum = 0.0;
            f64 sumSquares = 0.0;
            u64 samples = 0;
            for (sizet i = 0; i + 3 < frame.size(); i += 4)
            {
                bool coatHere = false;
                for (int c = 0; c < 3; ++c)
                {
                    coatHere = coatHere || std::abs(static_cast<int>(frame[i + c]) - static_cast<int>(empty[i + c])) >
                                               kChangedPixelThreshold;
                }
                if (!coatHere)
                {
                    continue;
                }
                const f64 luma = (0.2126 * frame[i]) + (0.7152 * frame[i + 1]) + (0.0722 * frame[i + 2]);
                sum += luma;
                sumSquares += luma * luma;
                ++samples;
            }
            if (samples < 2)
            {
                return 0.0;
            }
            const f64 mean = sum / static_cast<f64>(samples);
            if (mean <= 1e-6)
            {
                return 0.0;
            }
            const f64 variance = std::max(0.0, (sumSquares / static_cast<f64>(samples)) - (mean * mean));
            return std::sqrt(variance) / mean;
        }
    } // namespace

    class FurCoatAuthoringVisualEvidenceTest : public RendererAttachedTest
    {
      public:
        AssetHandle m_ShortCoatHandle = 0;
        AssetHandle m_LongCoatHandle = 0;
        Entity m_GroomEntity;
        Entity m_BodyEntity;
        Entity m_LightEntity;

        void BuildScene() override
        {
            if (!Project::GetActive() || !Project::HasAssetManager())
            {
                std::error_code ec;
                const fs::path projectDir = TempDir("project");
                fs::create_directories(projectDir / "Assets", ec);
                ASSERT_FALSE(ec) << "failed to create temp project dir";
                {
                    std::ofstream proj(projectDir / "Evidence.oloproj");
                    proj << "Project:\n"
                            "  Name: FurCoatAuthoringEvidence\n"
                            "  StartScene: \"\"\n"
                            "  AssetDirectory: \"Assets\"\n"
                            "  ScriptModulePath: \"\"\n";
                }
                ASSERT_TRUE(Project::Load(projectDir / "Evidence.oloproj"));
                auto assetManager = Ref<EditorAssetManager>::Create();
                assetManager->Initialize(false);
                Project::SetAssetManager(assetManager);
            }

            EnableRendering(kWidth, kHeight);
            Scene& scene = GetScene();

            auto& rendererSettings = Renderer3D::GetRendererSettings();
            rendererSettings.EditorDebugDrawsEnabled = true;
            rendererSettings.ShowComponentGizmos = false;
            rendererSettings.ShowGrid = false;
            rendererSettings.ShowWorldAxisHelper = false;

            // ONE directional light, repointed between captures: the coat's
            // appearance is then a function of a single angle, which is the axis
            // criterion 4's "front/side/backlight" is stated in.
            {
                m_LightEntity = scene.CreateEntity("Sun");
                auto& tc = m_LightEntity.GetComponent<TransformComponent>();
                tc.Translation = glm::vec3(0.0f, 6.0f, 4.0f);
                auto& dl = m_LightEntity.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(0.0f, -0.25f, -1.0f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.93f);
                dl.m_Intensity = 3.0f;
            }

            // A DARK body, so the coat is what the frame is measuring, and so a
            // pixel outside it that is not background is a guard hair.
            {
                m_BodyEntity = scene.CreateEntity("Body");
                auto& tc = m_BodyEntity.GetComponent<TransformComponent>();
                tc.Scale = glm::vec3(0.98f);
                auto& mc = m_BodyEntity.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Sphere;
                if (Ref<Mesh> mesh = MeshPrimitives::CreateSphere())
                {
                    mc.m_MeshSource = mesh->GetMeshSource();
                }
                auto& mat = m_BodyEntity.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.06f, 0.055f, 0.05f, 1.0f));
            }

            Ref<GroomAsset> shortCoat = BuildAnimalGroom(/*longCoat*/ false);
            Ref<GroomAsset> longCoat = BuildAnimalGroom(/*longCoat*/ true);
            ASSERT_TRUE(shortCoat);
            ASSERT_TRUE(longCoat);
            m_ShortCoatHandle = AssetManager::AddMemoryOnlyAsset<GroomAsset>(shortCoat);
            m_LongCoatHandle = AssetManager::AddMemoryOnlyAsset<GroomAsset>(longCoat);
            ASSERT_NE(static_cast<u64>(m_ShortCoatHandle), 0u);
            ASSERT_NE(static_cast<u64>(m_LongCoatHandle), 0u);

            m_GroomEntity = scene.CreateEntity("Groom");
            auto& groomComponent = m_GroomEntity.AddComponent<GroomComponent>();
            groomComponent.m_Groom = m_ShortCoatHandle;
            groomComponent.m_ShowPreview = false;
            groomComponent.m_RenderStrands = true;
            groomComponent.m_MaxRenderStrands = 12000;
            // The DETERMINISTIC tier, for the reason #1247's and #1248's evidence
            // use it: the stochastic one is noise by design, so a pixel A/B would
            // be measuring the dither rather than the coat.
            groomComponent.m_CompositionMode = static_cast<u8>(GroomCompositionMode::OpaqueRibbon);
            groomComponent.m_StrandColor = glm::vec3(0.72f, 0.60f, 0.46f);

            // A fibre material, so the coat is LIT — a tint on an unlit ramp is
            // still visible, but a coat judged for "uniformly fuzzy" has to be
            // the coat that ships.
            auto& fibre = m_GroomEntity.AddComponent<GroomFibreComponent>();
            fibre.m_Enabled = true;
            fibre.m_PigmentMode = static_cast<u8>(GroomFibrePigmentMode::Melanin);
            fibre.m_Eumelanin = 0.35f;
            fibre.m_Pheomelanin = 0.15f;
            fibre.m_Intensity = 10.0f;

            auto& coat = m_GroomEntity.AddComponent<GroomCoatComponent>();
            coat.m_Enabled = true;
            coat.m_VariationSeed = 4711u;
            // An authored coat, not the defaults: the defaults are identity, so a
            // capture of them would be a capture of the control.
            coat.m_UndercoatDensity = 0.85f;
            coat.m_UndercoatLength = 0.9f;
            coat.m_UndercoatWidth = 0.9f;
            coat.m_UndercoatClump = 1.0f;
            coat.m_GuardDensity = 1.0f;
            coat.m_GuardLength = 1.25f;
            coat.m_GuardWidth = 1.3f;
            coat.m_GuardClump = 0.6f;
            coat.m_LengthJitter = 0.35f;
            coat.m_WidthJitter = 0.25f;
            coat.m_ShadeJitter = 0.30f;
            coat.m_ClumpCellSize = 0.045f;
        }

        // A reference animal as a groom on the unit sphere: an undercoat, a
        // guard coat, whiskers, and — on the long-coated one — a mane.
        //
        // AUTHORED HERE RATHER THAN IMPORTED for the reason GroomAlembicFixture.h
        // gives: a committed binary can only exercise the one shape whoever
        // exported it happened to make, and this fixture needs its GROUPS to be
        // exactly the ones criterion 1 names. It is the same arithmetic the
        // Alembic fixture's MakeShortCoatAnimal / MakeLongCoatAnimal use, laid on
        // a sphere instead of a capsule so it sits on the primitive body above.
        static Ref<GroomAsset> BuildAnimalGroom(bool longCoat)
        {
            GroomBuilder builder;
            std::string reason;

            struct LayerPlan
            {
                const char* Name;
                u32 Count;
                f32 Length;
                f32 Width;
                // The v band this layer's roots occupy, so a regional map can
                // address the regions separately.
                f32 VFrom;
                f32 VTo;
                f32 ThetaFrom; // polar band on the sphere, 0 = pole
                f32 ThetaTo;
            };

            std::vector<LayerPlan> layers;
            if (longCoat)
            {
                layers = { { "body_undercoat", 7000, 0.35f, 0.0055f, 0.00f, 0.50f, 0.15f, 0.95f },
                           { "body_guard", 1600, 0.85f, 0.0090f, 0.00f, 0.50f, 0.15f, 0.95f },
                           { "head_undercoat", 900, 0.18f, 0.0045f, 0.50f, 0.70f, 0.00f, 0.20f },
                           { "head_guard", 240, 0.32f, 0.0085f, 0.50f, 0.70f, 0.00f, 0.20f },
                           { "mane_longhair", 900, 1.05f, 0.0095f, 0.70f, 0.85f, 0.10f, 0.30f },
                           { "tail_plume_longhair", 500, 1.20f, 0.0090f, 0.85f, 1.00f, 0.90f, 1.00f },
                           { "muzzle_whiskers", 24, 1.50f, 0.0180f, 0.50f, 0.52f, 0.00f, 0.06f } };
            }
            else
            {
                layers = { { "body_undercoat", 7000, 0.10f, 0.0050f, 0.00f, 0.50f, 0.15f, 0.95f },
                           { "body_guard", 1600, 0.22f, 0.0085f, 0.00f, 0.50f, 0.15f, 0.95f },
                           { "head_undercoat", 900, 0.07f, 0.0042f, 0.50f, 0.70f, 0.00f, 0.20f },
                           { "head_guard", 240, 0.14f, 0.0080f, 0.50f, 0.70f, 0.00f, 0.20f },
                           { "ears_guard", 300, 0.20f, 0.0080f, 0.70f, 0.85f, 0.06f, 0.14f },
                           { "tail_guard", 400, 0.30f, 0.0085f, 0.85f, 1.00f, 0.90f, 1.00f },
                           { "muzzle_whiskers", 24, 1.30f, 0.0180f, 0.50f, 0.52f, 0.00f, 0.06f } };
            }

            constexpr f32 kRadius = 1.0f;
            constexpr f32 kGoldenAngle = 2.39996323f;
            constexpr u32 kPoints = 8;

            for (u32 layer = 0; layer < layers.size(); ++layer)
            {
                const LayerPlan& plan = layers[layer];
                u16 groupId = 0;
                if (!builder.AddGroup(plan.Name, groupId, reason))
                {
                    ADD_FAILURE() << "AddGroup('" << plan.Name << "'): " << reason;
                    return nullptr;
                }

                for (u32 s = 0; s < plan.Count; ++s)
                {
                    const f32 t = (static_cast<f32>(s) + 0.5f) / static_cast<f32>(plan.Count);
                    // A Fibonacci band on the sphere: even coverage of the
                    // layer's own polar band, from the index alone.
                    const f32 theta = plan.ThetaFrom + (t * (plan.ThetaTo - plan.ThetaFrom));
                    const f32 cosTheta = 1.0f - (2.0f * theta);
                    const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - (cosTheta * cosTheta)));
                    // The golden angle OFFSET PER LAYER, so the guard hairs do
                    // not grow out of the undercoat's exact roots — perfectly
                    // correlated layers would make every per-role measurement
                    // below a measurement of one layer.
                    const f32 phi = (kGoldenAngle * static_cast<f32>(s)) + (static_cast<f32>(layer) * 0.7f);
                    const glm::vec3 normal(sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi));
                    const glm::vec3 root = normal * kRadius;

                    std::vector<glm::vec3> points;
                    std::vector<f32> widths;
                    points.reserve(kPoints);
                    widths.reserve(kPoints);
                    for (u32 p = 0; p < kPoints; ++p)
                    {
                        const f32 along = static_cast<f32>(p) / static_cast<f32>(kPoints - 1);
                        glm::vec3 point = root + (normal * (plan.Length * along));
                        // Drooping, so a strand has a real shape for the length
                        // scale and the clump to act on. A straight spike is its
                        // own clump target and neither would be visible.
                        point.y -= plan.Length * 0.55f * along * along;
                        points.push_back(point);
                        widths.push_back(plan.Width * (1.0f - (0.75f * along)));
                    }

                    GroomCurveInput input;
                    input.Points = points;
                    input.Widths = widths;
                    // u around the body, v inside this layer's OWN band — the
                    // disjoint bands are what make a regional map regional.
                    const f32 u = std::fmod(phi / (2.0f * 3.14159265f), 1.0f);
                    input.RootUV = { u < 0.0f ? u + 1.0f : u, plan.VFrom + (t * (plan.VTo - plan.VFrom)) };
                    input.GroupId = groupId;
                    input.IsGuide = (s % 23u) == 0u;
                    if (!builder.AddCurve(input, reason))
                    {
                        ADD_FAILURE() << "AddCurve: " << reason;
                        return nullptr;
                    }
                }
            }

            builder.SetName(longCoat ? "LongCoatAnimal" : "ShortCoatAnimal");
            Ref<GroomAsset> groom = builder.Build(reason);
            EXPECT_TRUE(groom) << reason;
            if (groom)
            {
                // CANONICALISED, which is the cook's structural half — so what is
                // captured is the coat as it comes out of the cooker rather than
                // as the builder happened to order it. Criterion 1's "preserving
                // density and silhouette during cooking" is about this step.
                EXPECT_TRUE(GroomCooker::Canonicalize(*groom, reason)) << reason;
            }
            return groom;
        }

        void Capture(const std::string& saveAs, const glm::vec3& position, f32 yaw, f32 pitch,
                     std::vector<u8>& outPixels, u32 width = kWidth, u32 height = kHeight)
        {
            EditorCamera camera(60.0f, static_cast<f32>(width) / static_cast<f32>(height), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(width), static_cast<f32>(height));
            camera.SetPose(position, yaw, pitch);

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
            ASSERT_TRUE(fb) << "No composited framebuffer for '" << saveAs << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), width, height, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<sizet>(width) * height * 4u);

            {
                const sizet rowBytes = static_cast<sizet>(width) * 4u;
                std::vector<u8> tmp(rowBytes);
                for (u32 y = 0; y < height / 2u; ++y)
                {
                    u8* top = outPixels.data() + (static_cast<sizet>(y) * rowBytes);
                    u8* bot = outPixels.data() + (static_cast<sizet>(height - 1u - y) * rowBytes);
                    std::memcpy(tmp.data(), top, rowBytes);
                    std::memcpy(top, bot, rowBytes);
                    std::memcpy(bot, tmp.data(), rowBytes);
                }
            }

            if (!saveAs.empty())
            {
                WriteEvidenceAt(saveAs, width, height, outPixels);
            }
        }

        static void WriteEvidenceAt(const std::string& name, u32 width, u32 height, const std::vector<u8>& pixels)
        {
            // RELATIVE, and that is load-bearing: the renderer's initialisation
            // leaves the process cwd inside OloEditor/, so a path that looks like
            // it lands in the repo root actually lands in the editor's asset tree
            // — which is where these belong.
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ASSERT_FALSE(ec) << "Failed to create evidence dir '" << dir.string() << "': " << ec.message();
            const std::string path = (dir / (name + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4,
                                               pixels.data(), static_cast<int>(width) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed to write '" << path << "'";
        }

        GroomCoatComponent& Coat()
        {
            return m_GroomEntity.GetComponent<GroomCoatComponent>();
        }

        GroomComponent& Groom()
        {
            return m_GroomEntity.GetComponent<GroomComponent>();
        }

        void UseAnimal(bool longCoat)
        {
            Groom().m_Groom = longCoat ? m_LongCoatHandle : m_ShortCoatHandle;
        }

        void SetLightDirection(const glm::vec3& direction)
        {
            m_LightEntity.GetComponent<DirectionalLightComponent>().m_Direction = glm::normalize(direction);
        }
    };

    // ── 1 + 4. Both animals, three lights, three paths ─────────────────────

    TEST_F(FurCoatAuthoringVisualEvidenceTest, BothAnimalsUnderControlledFrontSideAndBacklight)
    {
        struct LightCase
        {
            const char* Name;
            glm::vec3 Direction;
        };
        // FRONT is the light travelling away from the camera (the coat is lit
        // from the viewer's side); BACK is the reverse, which is where a coat's
        // rim and its transmission live and where a uniform coat looks most
        // obviously uniform; SIDE is the raking angle that shows clumping.
        const std::array<LightCase, 3> lights = { {
            { "Front", glm::vec3(0.0f, -0.25f, -1.0f) },
            { "Side", glm::vec3(-1.0f, -0.20f, 0.0f) },
            { "Back", glm::vec3(0.0f, -0.25f, 1.0f) },
        } };

        struct AnimalCase
        {
            const char* Name;
            bool LongCoat;
        };
        const std::array<AnimalCase, 2> animals = { { { "ShortCoat", false }, { "LongCoat", true } } };

        const glm::vec3 eye{ 0.0f, 0.6f, 5.2f };

        for (const AnimalCase& animal : animals)
        {
            UseAnimal(animal.LongCoat);
            for (const LightCase& light : lights)
            {
                SetLightDirection(light.Direction);

                // A/B against the SAME camera, scene, light and groom, with only
                // the coat authoring switched off. Off FIRST, so the baseline
                // cannot be contaminated by geometry built for the previous case.
                Coat().m_Enabled = false;
                std::vector<u8> plain;
                Capture(std::string("FurCoatGroupsOff_GL_Forward_") + animal.Name + "_" + light.Name, eye, 0.0f, 0.05f,
                        plain);
                if (::testing::Test::HasFatalFailure())
                {
                    return;
                }

                Coat().m_Enabled = true;
                std::vector<u8> authored;
                Capture(std::string("FurCoat") + animal.Name + "_GL_Forward_" + light.Name, eye, 0.0f, 0.05f, authored);
                if (::testing::Test::HasFatalFailure())
                {
                    return;
                }

                // The coat's own footprint, measured against a frame with no
                // strands at all — so a variance below is a variance of the coat
                // and not of the body behind it.
                Groom().m_RenderStrands = false;
                std::vector<u8> strandless;
                Capture("", eye, 0.0f, 0.05f, strandless);
                Groom().m_RenderStrands = true;
                if (::testing::Test::HasFatalFailure())
                {
                    return;
                }

                const u32 differing = CountDifferingPixels(authored, plain);
                const f64 authoredSpread = CoatLuminanceRelativeSpread(authored, strandless);
                const f64 plainSpread = CoatLuminanceRelativeSpread(plain, strandless);
                std::printf("[fur-coat] %-9s %-5s  %u px differ, coat relative spread authored %.3f vs plain %.3f\n",
                            animal.Name, light.Name, differing, authoredSpread, plainSpread);

                // 1. IT REACHES THE SCREEN. A tint lane the shader never reads,
                //    a per-role stride that resolved to one, or a coat digest
                //    missing from the cache key all render exactly like `plain`.
                EXPECT_GT(differing, 2000u)
                    << animal.Name << "/" << light.Name
                    << ": enabling the coat authoring changed almost nothing, so it is not reaching the geometry "
                       "or the shader";

                // THE SPREAD IS PRINTED, NOT ASSERTED, and that is deliberate.
                //
                // The obvious assertion — "the authored coat is less uniform than
                // the flat one" — measures over the set of pixels each frame's
                // own coat occupies, and the authored coat occupies a DIFFERENT
                // set: its guard hairs are longer and thicker, so it covers more
                // of the dark background. The statistic then moves for a reason
                // that has nothing to do with uniformity, and it went both ways
                // across these six cells for exactly that reason. It is a
                // selection effect, not a measurement.
                //
                // The claim it was standing in for is made properly, with a
                // controlled A/B, by ShadeJitterWidensTheCoatLuminanceSpread
                // below, where the two arms draw identical geometry and differ
                // only in the per-strand tint. Criterion 4's judgement itself is
                // what the PNGs are for.
            }
        }
    }

    TEST_F(FurCoatAuthoringVisualEvidenceTest, TheCoatAuthoringReachesEveryRenderingPath)
    {
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

        const glm::vec3 eye{ 0.0f, 0.6f, 5.2f };
        UseAnimal(/*longCoat*/ true);
        SetLightDirection(glm::vec3(-1.0f, -0.20f, 0.0f));

        for (const PathCase& pathCase : paths)
        {
            Renderer3D::GetRendererSettings().Path = pathCase.Path;
            Renderer3D::ApplyRendererSettings();

            Coat().m_Enabled = false;
            std::vector<u8> plain;
            Capture(std::string("FurCoatGroupsOff_GL_") + pathCase.Name, eye, 0.0f, 0.05f, plain);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }

            Coat().m_Enabled = true;
            std::vector<u8> authored;
            Capture(std::string("FurCoatGroups_GL_") + pathCase.Name, eye, 0.0f, 0.05f, authored);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }

            const u32 differing = CountDifferingPixels(authored, plain);
            std::printf("[fur-coat] path %-11s  %u px differ\n", pathCase.Name, differing);
            EXPECT_GT(differing, 2000u)
                << pathCase.Name
                << ": the coat authoring did not reach this path. The geometry is shared across the three, so a "
                   "failure HERE and not on Forward means the strand pass is not running on this path at all";
        }

        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
    }

    // ── 2. The budget takes the interior before the outline ────────────────

    TEST_F(FurCoatAuthoringVisualEvidenceTest, PerRoleBudgetingKeepsMoreOfTheSilhouetteThanAGlobalStride)
    {
        // Criterion 1's "preserving density and silhouette", measured on pixels.
        //
        // THE CONTROL ARM IS A GLOBAL STRIDE, NOT A FULL COAT, and finding the
        // right control took a failed attempt worth recording. The first version
        // compared the outline's retained fraction against the whole coat's under
        // one budget cut, expecting the outline to survive better. It does not,
        // and not because the weighting is broken: the coat's INTERIOR is
        // massively overdrawn — thousands of undercoat strands land on the same
        // pixels — so removing half of them barely moves the interior pixel
        // count, while the sparse, non-overlapping outline responds almost
        // linearly. The interior count is saturated; the comparison was measuring
        // that saturation.
        //
        // The clean experiment is the one this feature actually changed: with the
        // coat DISABLED every curve is role Unassigned, so the budget falls back
        // to the single global stride the build used before #1251; with it
        // ENABLED at identity settings, the same budget is spent per role. Same
        // groom, same geometry at full budget, same saturation — only the
        // ALLOCATION differs.
        const glm::vec3 eye{ 0.0f, 0.6f, 5.2f };
        UseAnimal(/*longCoat*/ true);
        SetLightDirection(glm::vec3(-1.0f, -0.20f, 0.0f));

        // Identity overrides: the arms must differ only in HOW the budget is
        // spent, not in how long or how thick the strands are.
        GroomCoatComponent& coat = Coat();
        coat = GroomCoatComponent{};
        coat.m_Enabled = true;

        // The frame with a body and no coat, and the frame with neither — the two
        // masks the silhouette measurement needs.
        Groom().m_RenderStrands = false;
        std::vector<u8> strandless;
        Capture("", eye, 0.0f, 0.05f, strandless);
        Groom().m_RenderStrands = true;
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        // MOVED OUT OF FRAME rather than hidden: MeshComponent carries no
        // visibility flag, and removing the component would tear down its mesh
        // resources and rebuild them for the next capture — which is a different
        // frame for a reason that has nothing to do with the coat.
        auto& bodyTransform = m_BodyEntity.GetComponent<TransformComponent>();
        const glm::vec3 bodyHome = bodyTransform.Translation;
        bodyTransform.Translation = glm::vec3(0.0f, -1000.0f, 0.0f);
        Groom().m_RenderStrands = false;
        std::vector<u8> background;
        Capture("", eye, 0.0f, 0.05f, background);
        bodyTransform.Translation = bodyHome;
        Groom().m_RenderStrands = true;
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        constexpr u32 kFullBudget = 12000;
        constexpr u32 kTightBudget = 1500;

        const auto captureOutline = [&](const char* name, bool coatEnabled, u32 budget)
        {
            Coat().m_Enabled = coatEnabled;
            Groom().m_MaxRenderStrands = budget;
            std::vector<u8> frame;
            Capture(name, eye, 0.0f, 0.05f, frame);
            return CountSilhouettePixels(frame, strandless, background);
        };

        const u32 perRoleFull = captureOutline("FurCoatBudgetFull_GL_Forward", true, kFullBudget);
        const u32 perRoleTight = captureOutline("FurCoatBudgetThin_GL_Forward", true, kTightBudget);
        const u32 globalFull = captureOutline("FurCoatBudgetFullGlobalStride_GL_Forward", false, kFullBudget);
        const u32 globalTight = captureOutline("FurCoatBudgetThinGlobalStride_GL_Forward", false, kTightBudget);

        Coat().m_Enabled = true;
        Groom().m_MaxRenderStrands = kFullBudget;
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        ASSERT_GT(perRoleFull, 0u) << "the guard hairs must stand off the body, or there is no silhouette to measure";
        ASSERT_GT(globalFull, 0u);

        const f64 perRoleRetained = static_cast<f64>(perRoleTight) / static_cast<f64>(perRoleFull);
        const f64 globalRetained = static_cast<f64>(globalTight) / static_cast<f64>(globalFull);
        std::printf("[fur-coat] outline under a budget cut to %u: per-role %u -> %u (%.3f), global stride %u -> %u "
                    "(%.3f)\n",
                    kTightBudget, perRoleFull, perRoleTight, perRoleRetained, globalFull, globalTight,
                    globalRetained);

        // At the FULL budget the two arms must be the same picture: identity
        // settings, every stride 1, nothing to allocate. If they differ, the
        // comparison below is measuring something other than the allocation.
        EXPECT_NEAR(static_cast<f64>(perRoleFull), static_cast<f64>(globalFull), static_cast<f64>(globalFull) * 0.02)
            << "the two arms do not start from the same coat, so the budget comparison is not controlled";

        EXPECT_LT(globalRetained, 0.95) << "the budget did not actually bite, so this test asserts nothing";
        EXPECT_GT(perRoleRetained, globalRetained * 1.05)
            << "spending the budget per role kept no more of the animal's outline than a single global stride, so "
               "the guard hairs are not being thinned last";
    }

    TEST_F(FurCoatAuthoringVisualEvidenceTest, ShadeJitterWidensTheCoatLuminanceSpread)
    {
        // Criterion 4's "avoid a uniformly fuzzy coat", as the one part of it a
        // number can carry: per-strand shade variation must widen the coat's
        // luminance distribution.
        //
        // A CONTROLLED A/B, unlike the variance printed in the capture test
        // above: ShadeJitter changes no geometry at all, so both arms draw the
        // SAME strands in the same places and the variance is measured over the
        // same pixels. The only difference is the tint each strand carries —
        // which is exactly the mechanism under test, including its transport
        // through the packed vertex lane and the shader's unpack.
        const glm::vec3 eye{ 0.0f, 0.6f, 5.2f };
        UseAnimal(/*longCoat*/ true);
        SetLightDirection(glm::vec3(-1.0f, -0.20f, 0.0f));

        GroomCoatComponent& coat = Coat();
        coat = GroomCoatComponent{};
        coat.m_Enabled = true;
        coat.m_VariationSeed = 4711u;

        Groom().m_RenderStrands = false;
        std::vector<u8> strandless;
        Capture("", eye, 0.0f, 0.05f, strandless);
        Groom().m_RenderStrands = true;
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        Coat().m_ShadeJitter = 0.0f;
        std::vector<u8> flat;
        Capture("FurCoatShadeFlat_GL_Forward", eye, 0.0f, 0.05f, flat);

        // THE MAXIMUM, not a middling value. Half of a symmetric shade jitter
        // clips at white (a tint cannot raise an albedo above the authored one —
        // GroomCoat.h says so at the field), so only the darkening half moves the
        // measurement, and a small jitter puts the effect down among the
        // lighting variation that dominates the spread of any lit coat. At 1.0
        // the effect is unambiguous, which is what an evidence assertion needs.
        Coat().m_ShadeJitter = 1.0f;
        std::vector<u8> varied;
        Capture("FurCoatShadeVaried_GL_Forward", eye, 0.0f, 0.05f, varied);
        Coat().m_ShadeJitter = 0.0f;
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        const f64 flatSpread = CoatLuminanceRelativeSpread(flat, strandless);
        const f64 variedSpread = CoatLuminanceRelativeSpread(varied, strandless);
        const u32 differing = CountDifferingPixels(varied, flat);
        std::printf("[fur-coat] shade jitter 0.0 -> 1.0: relative spread %.3f -> %.3f, %u px differ\n", flatSpread,
                    variedSpread, differing);

        EXPECT_GT(differing, 2000u)
            << "shade jitter changed nothing on screen: the per-strand tint is not reaching the shader, which is "
               "what a dropped vertex lane or a missing unpack looks like";
        ASSERT_GT(flatSpread, 0.0);
        EXPECT_GT(variedSpread, flatSpread * 1.05)
            << "shade jitter did not widen the coat's relative luminance spread, so every strand is still the same "
               "colour";
    }

    // ── 3 + 5. The layers are separable, and whiskers survive ──────────────

    TEST_F(FurCoatAuthoringVisualEvidenceTest, HidingEachLayerProducesADifferentCoat)
    {
        const glm::vec3 eye{ 0.0f, 0.6f, 5.2f };
        UseAnimal(/*longCoat*/ true);
        SetLightDirection(glm::vec3(-1.0f, -0.20f, 0.0f));

        constexpr u32 kAll = (1u << GroomCoatRoleCount) - 1u;

        Coat().m_RoleVisibilityMask = kAll;
        std::vector<u8> everything;
        Capture("FurCoatLayersAll_GL_Forward", eye, 0.0f, 0.05f, everything);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        Coat().m_RoleVisibilityMask = kAll & ~(1u << static_cast<u32>(GroomCoatRole::Undercoat));
        std::vector<u8> noUndercoat;
        Capture("FurCoatLayersGuardOnly_GL_Forward", eye, 0.0f, 0.05f, noUndercoat);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        Coat().m_RoleVisibilityMask = kAll & ~(1u << static_cast<u32>(GroomCoatRole::GuardHair));
        std::vector<u8> noGuard;
        Capture("FurCoatLayersUndercoatOnly_GL_Forward", eye, 0.0f, 0.05f, noGuard);
        Coat().m_RoleVisibilityMask = kAll;
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        const u32 undercoatDelta = CountDifferingPixels(noUndercoat, everything);
        const u32 guardDelta = CountDifferingPixels(noGuard, everything);
        const u32 betweenDelta = CountDifferingPixels(noUndercoat, noGuard);
        std::printf("[fur-coat] hide undercoat %u px, hide guard %u px, the two differ by %u px\n", undercoatDelta,
                    guardDelta, betweenDelta);

        EXPECT_GT(undercoatDelta, 2000u) << "hiding the undercoat changed nothing on screen";
        EXPECT_GT(guardDelta, 2000u) << "hiding the guard coat changed nothing on screen";
        // AND THE TWO ARE DIFFERENT FRAMES. A mask that reached the counters but
        // removed the same strands either way would pass both assertions above
        // and fail this one.
        EXPECT_GT(betweenDelta, 2000u)
            << "hiding the undercoat and hiding the guard coat produced the same picture, so the mask is not "
               "selecting by role";
    }

    TEST_F(FurCoatAuthoringVisualEvidenceTest, TheWhiskersSurviveABudgetThatGutsTheUndercoat)
    {
        const glm::vec3 eye{ 0.0f, 0.6f, 5.2f };
        UseAnimal(/*longCoat*/ false);
        SetLightDirection(glm::vec3(-1.0f, -0.20f, 0.0f));

        constexpr u32 kAll = (1u << GroomCoatRoleCount) - 1u;
        constexpr u32 kWhiskersOnly = 1u << static_cast<u32>(GroomCoatRole::Whisker);

        // A brutal budget: 400 strands against a groom of nearly ten thousand.
        Groom().m_MaxRenderStrands = 400;

        Coat().m_RoleVisibilityMask = kWhiskersOnly;
        std::vector<u8> whiskersOnly;
        Capture("FurCoatWhiskers_GL_Forward", eye, 0.0f, 0.05f, whiskersOnly);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        Coat().m_RoleVisibilityMask = kAll & ~kWhiskersOnly;
        std::vector<u8> withoutWhiskers;
        Capture("FurCoatWhiskersHidden_GL_Forward", eye, 0.0f, 0.05f, withoutWhiskers);

        Coat().m_RoleVisibilityMask = kAll;
        std::vector<u8> everything;
        Capture("FurCoatWhiskersPresent_GL_Forward", eye, 0.0f, 0.05f, everything);

        Groom().m_MaxRenderStrands = 12000;
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        // The whiskers are DRAWN at this budget: hiding them changes the frame.
        // Twenty-four very thick strands against a coat of thousands is a small
        // number of pixels, so the threshold is small — but it is not zero, and
        // zero is exactly what a budget that decimated them would give.
        const u32 whiskerDelta = CountDifferingPixels(everything, withoutWhiskers);
        std::printf("[fur-coat] whiskers contribute %u px at a budget of 400 strands\n", whiskerDelta);
        EXPECT_GT(whiskerDelta, 200u)
            << "the whiskers vanished under a tight budget: three missing whiskers read as damage, which is why "
               "the Whisker role is exempt from the density draw";
    }

    // ── 6. Sub-pixel: MSAA, upscaling and a non-native resolution ──────────

    TEST_F(FurCoatAuthoringVisualEvidenceTest, TheCoatSurvivesMsaa)
    {
        // Strands are sub-pixel (groom-strand-visibility.md), so the coverage a
        // density change produces and the MSAA resolve interact directly: this is
        // the likeliest place for a coat that looks right at 1x to break.
        //
        // MSAASampleCount lives on DeferredSettings, not RendererSettings — MSAA
        // is a G-Buffer setting on the deferred path only — so this cell runs on
        // Deferred. That is where it belongs rather than an omission.
        //
        // EACH SAMPLE COUNT IS ITS OWN A/B. Comparing the two MSAA frames to each
        // other and asserting they are not black would pass with the coat
        // authoring never applied at all.
        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        auto& deferred = Renderer3D::GetRendererSettings().Deferred;
        const u32 restoreSamples = deferred.MSAASampleCount;

        const glm::vec3 eye{ 0.0f, 0.6f, 5.2f };
        UseAnimal(/*longCoat*/ true);
        SetLightDirection(glm::vec3(-1.0f, -0.20f, 0.0f));

        struct Variant
        {
            const char* Name;
            u32 Samples;
        };
        const std::array<Variant, 2> variants = { { { "NoMsaa", 1u }, { "Msaa4", 4u } } };

        std::array<u32, 2> differing{};

        for (sizet v = 0; v < variants.size(); ++v)
        {
            deferred.MSAASampleCount = variants[v].Samples;
            Renderer3D::ApplyRendererSettings();

            Coat().m_Enabled = false;
            std::vector<u8> plain;
            Capture(std::string("FurCoatGroupsOff_GL_Deferred_") + variants[v].Name, eye, 0.0f, 0.05f, plain);
            if (::testing::Test::HasFatalFailure())
            {
                deferred.MSAASampleCount = restoreSamples;
                Renderer3D::ApplyRendererSettings();
                return;
            }

            Coat().m_Enabled = true;
            std::vector<u8> authored;
            Capture(std::string("FurCoatGroups_GL_Deferred_") + variants[v].Name, eye, 0.0f, 0.05f, authored);
            if (::testing::Test::HasFatalFailure())
            {
                deferred.MSAASampleCount = restoreSamples;
                Renderer3D::ApplyRendererSettings();
                return;
            }

            differing[v] = CountDifferingPixels(authored, plain);
            std::printf("[fur-coat] MSAA %u: %u px differ\n", variants[v].Samples, differing[v]);
            EXPECT_GT(differing[v], 2000u)
                << variants[v].Name << ": the coat authoring changed almost nothing at this sample count";
        }

        deferred.MSAASampleCount = restoreSamples;
        Renderer3D::ApplyRendererSettings();
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();

        // And of the SAME ORDER at both counts. MSAA legitimately moves edge
        // pixels; it must not halve or double the coat's footprint, which is what
        // a coverage term evaluated per sample instead of per fragment would look
        // like.
        ASSERT_GT(differing[0], 0u);
        const f64 ratio = static_cast<f64>(differing[1]) / static_cast<f64>(differing[0]);
        std::printf("[fur-coat] MSAA footprint ratio 4x/1x: %.3f\n", ratio);
        EXPECT_GT(ratio, 0.5) << "the coat's footprint moved with the sample count";
        EXPECT_LT(ratio, 2.0) << "the coat's footprint moved with the sample count";
    }

    TEST_F(FurCoatAuthoringVisualEvidenceTest, TheCoatSurvivesANonNativeResolutionAndUpscaling)
    {
        // Coat density PER PIXEL is resolution-dependent — a coat tuned at native
        // that goes bald or solid at another resolution is what this cell exists
        // to catch — and the upscale path is the same question asked by the
        // renderer rather than by the window.
        //
        // Both are one cell because the mechanism is one: the strand build's
        // output is a function of the ASSET and the coat, and neither may see the
        // render target. What the target changes is how many pixels each ribbon
        // covers, which is the resolve's business.
        const glm::vec3 eye{ 0.0f, 0.6f, 5.2f };
        UseAnimal(/*longCoat*/ true);
        SetLightDirection(glm::vec3(-1.0f, -0.20f, 0.0f));

        Coat().m_Enabled = false;
        std::vector<u8> nativeOff;
        Capture("FurCoatGroupsOff_GL_Forward_Native", eye, 0.0f, 0.05f, nativeOff);
        Coat().m_Enabled = true;
        std::vector<u8> native;
        Capture("FurCoatGroups_GL_Forward_Native", eye, 0.0f, 0.05f, native);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }
        const u32 nativeDiffering = CountDifferingPixels(native, nativeOff);
        ASSERT_GT(nativeDiffering, 2000u) << "no coat effect at native resolution to compare against";

        constexpr u32 kSmallWidth = 960;
        constexpr u32 kSmallHeight = 540;
        // ResizeRenderTarget, not a second EnableRendering: the fixture owns the
        // render target's lifetime, and re-enabling would build a new one
        // underneath the frame graph mid-test.
        ResizeRenderTarget(kSmallWidth, kSmallHeight);

        // THE SCALED CELL IS AN A/B TOO. Without its own control it would pass
        // whenever a coat merely rendered at 960x540, which is true even with the
        // authoring never applied.
        Coat().m_Enabled = false;
        std::vector<u8> scaledOff;
        Capture("FurCoatGroupsOff_GL_Forward_Scaled", eye, 0.0f, 0.05f, scaledOff, kSmallWidth, kSmallHeight);
        Coat().m_Enabled = true;
        std::vector<u8> scaled;
        Capture("FurCoatGroups_GL_Forward_Scaled", eye, 0.0f, 0.05f, scaled, kSmallWidth, kSmallHeight);
        const u32 scaledDiffering = CountDifferingPixels(scaled, scaledOff);

        // Restored BEFORE any assertion can return early, so a failure here
        // cannot leave the fixture at the small size for whatever runs next.
        ResizeRenderTarget(kWidth, kHeight);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        const f64 nativeFraction = static_cast<f64>(nativeDiffering) / static_cast<f64>(kWidth * kHeight);
        const f64 scaledFraction = static_cast<f64>(scaledDiffering) / static_cast<f64>(kSmallWidth * kSmallHeight);
        std::printf("[fur-coat] coat footprint: native %.4f of frame, scaled %.4f\n", nativeFraction, scaledFraction);

        EXPECT_GT(scaledDiffering, 500u) << "the coat authoring vanished at a non-native resolution";
        // THE FRACTION OF THE FRAME, not the pixel count: the coat covers the
        // same part of the animal at any resolution, so a coat that went bald or
        // solid would show as the fraction moving. The band is wide because
        // sub-pixel strands legitimately lose and gain coverage on resolve; it is
        // narrow enough to catch a coat that halved or doubled.
        EXPECT_GT(scaledFraction, nativeFraction * 0.5) << "the coat thinned out badly at a smaller render target";
        EXPECT_LT(scaledFraction, nativeFraction * 2.0) << "the coat went solid at a smaller render target";
    }
} // namespace OloEngine::Tests

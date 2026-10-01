#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L8
// =============================================================================
// GroomSceneShadowVisualEvidenceTest — issue #1323, the pixels.
//
// Writes OloEditor/assets/tests/visual/GroomSceneShadow_GL_<Path>[_<Case>].png
// and the matching GroomSceneShadowOff_* controls. The filename carries the
// {backend} x {path} cell it covers, deliberately including the backend even
// though it is always GL here: a reader counting files then cannot mistake a
// complete set of OpenGL captures for a complete verification matrix. Every
// Vulkan cell is live-only — these fixtures need a real GL 4.6 context and skip
// without one — and is evidenced in the PR body instead.
//
// THE MASKS ARE DERIVED, NOT HAND-PLACED, and that is the design decision the
// file rests on. "The coat's shadow fell on the ground" and "the ground's
// shadow fell on the coat" are claims about two DISJOINT screen regions, and a
// hand-typed rectangle that drifts off the coat turns every assertion into a
// measurement of the background. So the coat's mask is obtained by toggling
// GroomComponent::m_RenderStrands and taking the pixels that moved — the coat
// IS where the coat is — and everything else is the scene.
//
// WHAT IS ASSERTED, as opposed to merely captured:
//
//   1. HAIR -> SCENE. With casting on, the SCENE region darkens. A caster
//      family that was never wired into the frame's directional technique
//      renders exactly like the control, and virtual-geometry-into-a-second-
//      shadow-technique.md is explicit that nothing else detects that.
//   2. SCENE -> HAIR. With receiving on, the COAT region darkens under an
//      occluder. A shader that declares the samplers and never reads them
//      renders exactly like the control too.
//   3. BOTH IN THE SAME FRAME. Criterion 4 says two separate captures do not
//      satisfy it, so the both-on capture is asserted to move BOTH masks —
//      against the same control, from the same camera.
//   4. THE ONE-TEXEL FLOOR IS WHAT MAKES IT CAST. With the floor at zero the
//      coat casts essentially nothing, because a 70 um strand crosses a cascade
//      texel centre essentially never. That is the measurement behind the
//      mechanism rather than an assertion that it is on.
//   5. A CASTER IS NOT SHADOWED BY ITS OWN CAST. Once this groom's widened
//      ribbons are in the cascade map, a strand sampling that map at its own
//      position is occluded by its own coat — and a shadow map is a BINARY
//      test on something that is not binary, so the coat goes BLACK rather
//      than merely darker. Measured at 0.22 mean luma against a 44.98 control
//      before the receiver offset was gated on CASTING rather than on the
//      density volume. A black coat is indistinguishable from a correct
//      silhouette (groom-coat-self-shadowing.md rule 10), which is why this is
//      an assertion and not a look.
//   6. BOTH DIRECTIONS SURVIVE MSAA, at the same order of magnitude. Hair is
//      sub-pixel geometry and the width floor is a coverage argument, so a
//      per-SAMPLE evaluation anywhere would move the effect with the sample
//      count. Measured on the deferred path, where MSAASampleCount actually
//      applies.
//   7. THE VIRTUAL SHADOW MAP CELL (#1523). The coat casts through the VSM's
//      clip levels, and a coat that MOVES leaves no silhouette behind in the
//      VSM's cached pages: the moved frame matches a from-scratch render.
//   8. THE BODY SHADOWS ITS OWN FUR, AND THE FUR STILL NOT ITSELF (#1533).
//      Lit from behind, the coat the camera sees is on the body's far side.
//      The strands sample the OPAQUE cascades at themselves, so the body
//      darkens them; the light-exit receiver they replace, re-created by a
//      fault lever, does not -- and with the body not casting, routing both
//      directions leaves the coat as lit as no routing at all.
//
// Classification: L8 / golden image (full GL pipeline + RGBA8 readback + PNG).
// These are EVIDENCE, not SSIM goldens; the contracts are the assertions and
// the PNGs exist so a reviewer can look at what they describe.
// =============================================================================

#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCoatShadow.h"
#include "OloEngine/Groom/GroomCoatShadowTechnique.h"
#include "OloEngine/Groom/GroomCooker.h"
#include "OloEngine/Groom/GroomVisibility.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Passes/GroomRenderPass.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
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
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;

        // A pixel is "on the coat" when toggling the strands moved it by more
        // than this. Well above the frame's own noise floor and well below the
        // change a strand makes against a mid-grey background.
        constexpr f64 kMaskThreshold = 6.0;

        [[nodiscard]] f64 Luma(const std::vector<u8>& frame, sizet i)
        {
            // Rec. 709 luma on the tone-mapped frame. Every claim below is a
            // RELATIVE comparison between two frames of the same scene from the
            // same camera, so the transfer function cancels well enough for a
            // direction and an ordering — which is all that is asserted. No
            // ratio of two tone-mapped means is taken.
            return (0.2126 * frame[i]) + (0.7152 * frame[i + 1]) + (0.0722 * frame[i + 2]);
        }

        /// Pixels the coat occupies, as a per-pixel mask over the frame.
        [[nodiscard]] std::vector<u8> DeriveMask(const std::vector<u8>& withCoat, const std::vector<u8>& without)
        {
            std::vector<u8> mask(withCoat.size() / 4u, 0u);
            if (withCoat.size() != without.size())
            {
                return mask;
            }
            for (sizet i = 0, p = 0; i + 3 < withCoat.size(); i += 4, ++p)
            {
                mask[p] = std::abs(Luma(withCoat, i) - Luma(without, i)) > kMaskThreshold ? 1u : 0u;
            }
            return mask;
        }

        [[nodiscard]] u32 CountMask(const std::vector<u8>& mask)
        {
            u32 count = 0;
            for (const u8 m : mask)
            {
                count += m;
            }
            return count;
        }

        /// Pixels the two frames disagree about, inside `mask` or outside it.
        ///
        /// BESIDE the mean rather than instead of it: the scene region is
        /// mostly untouched background, so a coat shadow that is unmistakable
        /// on screen moves the region MEAN by about one luma step. The count
        /// is what says how much of the ground actually changed, and the two
        /// together are what distinguish "a shadow landed" from "the exposure
        /// drifted".
        [[nodiscard]] u32 CountDifferingIn(const std::vector<u8>& frame, const std::vector<u8>& baseline,
                                           const std::vector<u8>& mask, bool inside)
        {
            if (frame.size() != baseline.size())
            {
                return 0;
            }
            u32 count = 0;
            for (sizet i = 0, p = 0; i + 3 < frame.size() && p < mask.size(); i += 4, ++p)
            {
                if ((mask[p] != 0u) != inside)
                {
                    continue;
                }
                if (std::abs(Luma(frame, i) - Luma(baseline, i)) > kMaskThreshold)
                {
                    ++count;
                }
            }
            return count;
        }

        /// Mean luma over the pixels `mask` selects (or over its complement).
        [[nodiscard]] f64 MeanLumaIn(const std::vector<u8>& frame, const std::vector<u8>& mask, bool inside)
        {
            f64 total = 0.0;
            u32 count = 0;
            for (sizet i = 0, p = 0; i + 3 < frame.size() && p < mask.size(); i += 4, ++p)
            {
                if ((mask[p] != 0u) == inside)
                {
                    total += Luma(frame, i);
                    ++count;
                }
            }
            return count > 0u ? total / static_cast<f64>(count) : 0.0;
        }
    } // namespace

    class GroomSceneShadowVisualEvidenceTest : public RendererAttachedTest
    {
      public:
        AssetHandle m_GroomHandle = 0;
        Entity m_GroomEntity;
        Entity m_LightEntity;
        Entity m_OccluderEntity;
        Entity m_BodyEntity;

        // THE RENDERING PATH IS PROCESS-GLOBAL, and four cases here move it.
        // RendererStateListener restores it before the next test runs, but it
        // RECORDS the leak and --olo-strict-renderer-state fails on it — so a
        // fixture that changes it owns putting it back.
        RenderingPath m_RestorePath = RenderingPath::Forward;

        void SetUp() override
        {
            RendererAttachedTest::SetUp();
            m_RestorePath = Renderer3D::GetRendererSettings().Path;
        }

        void TearDown() override
        {
            Renderer3D::GetRendererSettings().Path = m_RestorePath;
            Renderer3D::ApplyRendererSettings();
            RendererAttachedTest::TearDown();
        }

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
                            "  Name: GroomSceneShadowEvidence\n"
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

            // ONE shadow-casting sun, from above and to the side, so both
            // directions land somewhere the camera can see: the coat's shadow
            // falls on the ground away from the camera, and the occluder's
            // falls across the coat's near side.
            {
                m_LightEntity = scene.CreateEntity("Sun");
                auto& tc = m_LightEntity.GetComponent<TransformComponent>();
                tc.Translation = glm::vec3(4.0f, 8.0f, 4.0f);
                auto& dl = m_LightEntity.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.45f, -0.78f, -0.44f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.93f);
                dl.m_Intensity = 3.0f;
                dl.m_CastShadows = true;
            }

            // A PALE ground, because it is the surface the coat's shadow has to
            // be visible on. A dark floor would satisfy "the frame changed"
            // while showing nothing.
            {
                Entity ground = scene.CreateEntity("Ground");
                auto& tc = ground.GetComponent<TransformComponent>();
                tc.Translation = glm::vec3(0.0f, 0.0f, 0.0f);
                tc.Scale = glm::vec3(24.0f, 1.0f, 24.0f);
                auto& mc = ground.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Plane;
                if (Ref<Mesh> mesh = MeshPrimitives::CreatePlane())
                {
                    mc.m_MeshSource = mesh->GetMeshSource();
                }
                auto& mat = ground.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.62f, 0.62f, 0.64f, 1.0f));
            }

            // A DARK body under the coat, so the coat is what the frame is
            // measuring rather than the sphere it grows on.
            {
                m_BodyEntity = scene.CreateEntity("Body");
                Entity body = m_BodyEntity;
                auto& tc = body.GetComponent<TransformComponent>();
                tc.Translation = glm::vec3(0.0f, 1.45f, 0.0f);
                tc.Scale = glm::vec3(0.98f);
                auto& mc = body.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Sphere;
                if (Ref<Mesh> mesh = MeshPrimitives::CreateSphere())
                {
                    mc.m_MeshSource = mesh->GetMeshSource();
                }
                auto& mat = body.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.06f, 0.055f, 0.05f, 1.0f));
            }

            // THE OCCLUDER, which is the scene -> hair half. A slab between the
            // sun and the coat, so a band of the coat is in shade while the rest
            // of it is in the open — which is what makes "a coat lit as if it
            // were in the open" a measurable claim rather than an impression.
            {
                m_OccluderEntity = scene.CreateEntity("Occluder");
                auto& tc = m_OccluderEntity.GetComponent<TransformComponent>();
                // SMALL AND HIGH, so it shades a BAND across the coat's upper
                // half and drops its own ground shadow behind the body, out of
                // the camera's view. A slab wide enough to shade the whole coat
                // also blankets the ground the coat's own shadow has to land
                // on, and a difference measured on ground that is already fully
                // shadowed saturates at zero -- which is how a working caster
                // reads as a broken one. Measured: at 3.2 x 3.2 the occluder's
                // square swallowed the coat's cast and left it a 1.09 mean-luma
                // sliver.
                tc.Translation = glm::vec3(1.15f, 4.6f, 1.15f);
                tc.Scale = glm::vec3(2.1f, 0.15f, 2.1f);
                auto& mc = m_OccluderEntity.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Cube;
                if (Ref<Mesh> mesh = MeshPrimitives::CreateCube())
                {
                    mc.m_MeshSource = mesh->GetMeshSource();
                }
                auto& mat = m_OccluderEntity.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.25f, 0.25f, 0.28f, 1.0f));
            }

            Ref<GroomAsset> groom = BuildEvidenceGroom();
            ASSERT_TRUE(groom);
            m_GroomHandle = AssetManager::AddMemoryOnlyAsset<GroomAsset>(groom);
            ASSERT_NE(static_cast<u64>(m_GroomHandle), 0u);

            m_GroomEntity = scene.CreateEntity("Groom");
            {
                auto& tc = m_GroomEntity.GetComponent<TransformComponent>();
                tc.Translation = glm::vec3(0.0f, 1.45f, 0.0f);
            }
            auto& groomComponent = m_GroomEntity.AddComponent<GroomComponent>();
            groomComponent.m_Groom = m_GroomHandle;
            groomComponent.m_ShowPreview = false;
            groomComponent.m_RenderStrands = true;
            groomComponent.m_MaxRenderStrands = 6000;
            // The DETERMINISTIC tier: the stochastic one is noise by design, so
            // a pixel A/B would be measuring the dither rather than the shadow.
            groomComponent.m_CompositionMode = static_cast<u8>(GroomCompositionMode::OpaqueRibbon);
            groomComponent.m_StrandColor = glm::vec3(0.78f, 0.68f, 0.54f);

            // A fibre material, because the scene shadow attenuates LIGHTING
            // and #1246's neutral ramp has no lighting to attenuate.
            auto& fibre = m_GroomEntity.AddComponent<GroomFibreComponent>();
            fibre.m_Enabled = true;
            fibre.m_PigmentMode = static_cast<u8>(GroomFibrePigmentMode::Melanin);
            fibre.m_Eumelanin = 0.1f;
            fibre.m_Pheomelanin = 0.05f;
            // Bright enough that most of the coat clears the mask threshold
            // against the dark body underneath it. At 12 only the coat's lit
            // fringe registered and the derived mask was 2.3% of the frame.
            fibre.m_Intensity = 20.0f;

            // The coat's INTERNAL term (#1248), off by default here. The cases
            // that need it turn it on, because it is the other half of the
            // double-count boundary and mixing it into every capture would make
            // the two indistinguishable.
            auto& coat = m_GroomEntity.AddComponent<GroomCoatShadowComponent>();
            coat.m_Enabled = false;
            coat.m_Mode = static_cast<u8>(GroomCoatShadow::CoatShadowMode::AnisotropicDensityVolume);
            coat.m_Resolution = 64;
            coat.m_StepVoxels = 3.0f;

            // The scene-shadow routing itself (#1323), added with BOTH
            // directions off. Off is the control every capture is measured
            // against, and the component's own default is on — so this is the
            // fixture choosing its baseline, not the component's default.
            auto& sceneShadow = m_GroomEntity.AddComponent<GroomSceneShadowComponent>();
            sceneShadow.m_CastShadows = false;
            sceneShadow.m_ReceiveShadows = false;
            sceneShadow.m_ShadowWidthTexels = 1.0f;
        }

        static Ref<GroomAsset> BuildEvidenceGroom()
        {
            GroomBuilder builder;
            std::string reason;
            u16 group = 0;
            EXPECT_TRUE(builder.AddGroup("coat", group, reason)) << reason;

            constexpr u32 kStrands = 6000;
            constexpr u32 kPoints = 8;
            constexpr f32 kRadius = 1.0f;
            constexpr f32 kLength = 1.1f;
            constexpr f32 kGoldenAngle = 2.39996323f;

            for (u32 s = 0; s < kStrands; ++s)
            {
                const f32 t = (static_cast<f32>(s) + 0.5f) / static_cast<f32>(kStrands);
                const f32 cosTheta = 1.0f - t;
                const f32 sinTheta = std::sqrt(std::max(0.0f, 1.0f - (cosTheta * cosTheta)));
                const f32 phi = kGoldenAngle * static_cast<f32>(s);
                const glm::vec3 normal(sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi));
                const glm::vec3 root = normal * kRadius;

                std::vector<glm::vec3> points;
                std::vector<f32> widths;
                points.reserve(kPoints);
                widths.reserve(kPoints);
                for (u32 p = 0; p < kPoints; ++p)
                {
                    const f32 along = static_cast<f32>(p) / static_cast<f32>(kPoints - 1);
                    glm::vec3 point = root + (normal * (kLength * along));
                    point.y -= kLength * 0.75f * along * along;
                    points.push_back(point);
                    widths.push_back(0.006f * (1.0f - (0.8f * along)));
                }

                GroomCurveInput input;
                input.Points = points;
                input.Widths = widths;
                input.RootUV = { std::fmod(phi / (2.0f * 3.14159265f), 1.0f), t };
                input.GroupId = group;
                input.IsGuide = (s % 25u) == 0;
                EXPECT_TRUE(builder.AddCurve(input, reason)) << reason;
            }

            builder.SetName("SceneShadowEvidenceGroom");
            Ref<GroomAsset> groom = builder.Build(reason);
            EXPECT_TRUE(groom) << reason;
            if (groom)
            {
                EXPECT_TRUE(GroomCooker::Canonicalize(*groom, reason)) << reason;
            }
            return groom;
        }

        // ONE POSE for every capture in this file. Every claim here is a
        // difference between two frames, and backend-ab-needs-an-identical-camera
        // is the memory of inventing a defect by comparing two poses.
        [[nodiscard]] static EditorCamera MakeCamera(u32 width = kWidth, u32 height = kHeight)
        {
            constexpr glm::vec3 kEye{ 0.0f, 2.35f, 6.4f };
            EditorCamera camera(60.0f, static_cast<f32>(width) / static_cast<f32>(height), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(width), static_cast<f32>(height));
            camera.SetPose(kEye, 0.0f, 0.22f);
            return camera;
        }

        /// Frames from the capture pose with nothing read back, for a case that
        /// has to read a per-frame counter on a particular frame.
        void Step(u32 frames)
        {
            EditorCamera camera = MakeCamera();
            RunEditorFrames(camera, frames);
        }

        void Capture(const std::string& saveAs, std::vector<u8>& outPixels, u32 width = kWidth,
                     u32 height = kHeight)
        {
            EditorCamera camera = MakeCamera(width, height);
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
            // leaves the process cwd inside OloEditor/, so a path that looks
            // like it lands in the repo root actually lands in the editor's
            // asset tree — which is where these belong.
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ASSERT_FALSE(ec) << "Failed to create evidence dir '" << dir.string() << "': " << ec.message();
            const std::string path = (dir / (name + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4,
                                               pixels.data(), static_cast<int>(width) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed to write '" << path << "'";
        }

        GroomSceneShadowComponent& Routing()
        {
            return m_GroomEntity.GetComponent<GroomSceneShadowComponent>();
        }
        GroomCoatShadowComponent& Coat()
        {
            return m_GroomEntity.GetComponent<GroomCoatShadowComponent>();
        }
        GroomComponent& Groom()
        {
            return m_GroomEntity.GetComponent<GroomComponent>();
        }
        /// Whether the sphere under the coat casts. Since #1533 it shadows the
        /// fur that grows on it, so a case measuring anything else stops it.
        void SetBodyCasts(bool casts)
        {
            m_BodyEntity.GetComponent<MaterialComponent>().m_Material.SetFlag(MaterialFlag::DisableShadowCasting,
                                                                              !casts);
        }

        // CASE 8's measurement under whichever light placeLight puts behind
        // the coat: the backlit frames with no routing, routed with the body
        // out of the map, routed with it, and through the coat-exit receiver
        // (the fault lever). opaqueMap names the copy that light samples.
        void RunBacklitBodyCase(const char* lightName, const std::string& tag, ShadowMap::OpaqueCopy opaqueMap,
                                const std::function<void()>& placeLight)
        {
            Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
            Renderer3D::ApplyRendererSettings();

            GetScene().DestroyEntity(m_OccluderEntity);
            m_OccluderEntity = {};
            placeLight();

            Routing().m_CastShadows = false;
            Routing().m_ReceiveShadows = false;
            const std::vector<u8> coatMask = DeriveCoatMask();
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }
            ASSERT_GT(CountMask(coatMask), 20000u) << "the backlit coat barely registers against the frame";

            // The no-shadow control, with the body not casting either.
            SetBodyCasts(false);
            std::vector<u8> control;
            Capture({}, control);

            Routing().m_CastShadows = true;
            Routing().m_ReceiveShadows = true;
            std::vector<u8> noBodyCaster;
            Capture(tag + "NoBodyCaster", noBodyCaster);
            const GroomRenderPass* const groomPass = Renderer3D::GetGroomRenderPass();
            const u32 opaqueGrooms = groomPass == nullptr                      ? 0u
                                     : opaqueMap == ShadowMap::OpaqueCopy::Atlas ? groomPass->GetStats().GroomsShadowedByOpaqueAtlas
                                                                                 : groomPass->GetStats().GroomsShadowedByOpaqueCascades;

            SetBodyCasts(true);
            std::vector<u8> fixed;
            Capture(tag + "Body", fixed);

            // THE EQUIVALENCE: a coat that receives and does not cast samples
            // the FULL map at the strand -- a map with the body and no fur in
            // it, which is exactly what the opaque copy has to be. So the fixed
            // frame must match it on every coat pixel; a copy that missed the
            // texels the strands read, or held the fur, would not.
            Routing().m_CastShadows = false;
            std::vector<u8> receiveOnly;
            Capture({}, receiveOnly);
            Routing().m_CastShadows = true;

            // THE NEGATIVE CONTROL: the same frame through the old receiver.
            struct FaultRestore
            {
                ~FaultRestore()
                {
                    Levers::SetFaultGroomShadowAtCoatExit(false);
                }
            } faultRestore;
            Levers::SetFaultGroomShadowAtCoatExit(true);
            std::vector<u8> atExit;
            Capture(tag + "BodyAtCoatExit", atExit);
            Levers::SetFaultGroomShadowAtCoatExit(false);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }

            // PER PIXEL, not the coat's mean: the coat grows on the upper half of
            // the sphere and its crown faces the sun, so only a band of it is on
            // the body's far side, and the engine's soft shadow spreads that band's
            // edge over a wide penumbra that the tone curve compresses further. The
            // body's shadow is three claims about the pixels it touches: it only
            // ever REMOVES light, where it is full the fur goes BLACK (nothing else
            // lights this scene), and over its whole footprint the fur keeps well
            // under the light it had. Measured at the time of writing: 7832 coat
            // pixels changed, 3 of them brighter; 1355 went from lit to black; the
            // changed pixels kept 0.66 of their luma.
            u32 bodyTouched = 0;
            u32 bodyBrightened = 0;
            u32 litToBlack = 0;
            f64 touchedFixed = 0.0;
            f64 touchedAtExit = 0.0;
            for (sizet i = 0, px = 0; i + 3 < fixed.size() && px < coatMask.size(); i += 4, ++px)
            {
                if (coatMask[px] == 0u)
                {
                    continue;
                }
                const f64 withFix = Luma(fixed, i);
                const f64 throughExit = Luma(atExit, i);
                if (std::abs(withFix - throughExit) <= kMaskThreshold)
                {
                    continue;
                }
                ++bodyTouched;
                bodyBrightened += withFix > throughExit ? 1u : 0u;
                litToBlack += (withFix < 10.0 && throughExit > 50.0) ? 1u : 0u;
                touchedFixed += withFix;
                touchedAtExit += throughExit;
            }
            const f64 touchedRatio = touchedAtExit > 0.0 ? touchedFixed / touchedAtExit : 1.0;
            const u32 coatPixels = CountMask(coatMask);
            // The two frames the negative control and the double-count check
            // compare, per pixel for the same reason.
            const u32 exitMovedByBody = CountDifferingIn(atExit, noBodyCaster, coatMask, /*inside=*/true);
            const u32 routingMovedCoat = CountDifferingIn(noBodyCaster, control, coatMask, /*inside=*/true);
            const u32 copyDiffersFromFurlessMap = CountDifferingIn(fixed, receiveOnly, coatMask, /*inside=*/true);
            std::printf("[groom-scene-shadow] %-9s %u coat px | the body touched %u (%u brighter, %u lit -> black), "
                        "keeping %.3f of their luma | through the coat exit the body moved %u px (negative control) | "
                        "routing with no body caster moved %u px | %u groom(s) on the opaque copy\n",
                        lightName, coatPixels, bodyTouched, bodyBrightened, litToBlack, touchedRatio, exitMovedByBody,
                        routingMovedCoat, opaqueGrooms);
            std::printf("[groom-scene-shadow] %-9s the opaque copy against the furless full map: %u coat px differ\n",
                        lightName, copyDiffersFromFurlessMap);

            ASSERT_GT(MeanLumaIn(control, coatMask, true), 10.0)
                << "the backlit control coat is already almost black, so this case cannot measure a shadow on it";
            EXPECT_GT(opaqueGrooms, 0u) << "no groom sampled the opaque copy of the " << lightName
                                        << "'s map, so the frames below measured the light-exit receiver twice";

            // THE SENSITIVITY CHECK FIRST: the old receiver must NOT see the body,
            // or the darkening asserted next could come from something else.
            ASSERT_LT(exitMovedByBody, coatPixels / 100u)
                << "through the light-exit receiver the body casting moved the coat anyway, so this case cannot tell "
                   "the fix from the defect it replaces";

            EXPECT_GT(bodyTouched, coatPixels / 5u)
                << "the body's shadow reached too little of the coat to be the far side of the body";
            EXPECT_LT(bodyBrightened, bodyTouched / 100u)
                << "the body BRIGHTENED coat pixels: an occluder can only remove light";
            EXPECT_GT(litToBlack, coatPixels / 50u)
                << "no coat pixel in the body's full shadow went black: the body still lets its sun through";
            EXPECT_LT(touchedRatio, 0.8)
                << "where the body's shadow falls the coat kept almost all of its light";

            // AND THE FUR STILL NOT ITSELF: with the body out of the map, routing
            // both directions must leave the coat exactly as lit as no routing. Its
            // own strands in the map would take it to black (case 5's 0.22 luma).
            EXPECT_LT(copyDiffersFromFurlessMap, coatPixels / 100u)
                << "the coat sampling the opaque copy is not the coat sampling a map with no fur in it: the copy missed "
                   "the texels the strands read, or holds the fur";
            EXPECT_LT(routingMovedCoat, coatPixels / 100u)
                << "with nothing but its own strands to shadow it, routing moved the coat: the opaque cascades hold "
                   "the fur, which counts the coat twice";
        }

        [[nodiscard]] static const GroomShadowCasterStats& CasterStats()
        {
            static const GroomShadowCasterStats kEmpty{};
            const auto* pass = Renderer3D::GetGroomRenderPass();
            return pass != nullptr ? pass->GetStats().SceneShadow : kEmpty;
        }

        /// The coat's screen footprint, derived by toggling the strands off and
        /// taking the pixels that moved. Leaves the routing untouched.
        std::vector<u8> DeriveCoatMask()
        {
            std::vector<u8> withCoat;
            Capture({}, withCoat);
            Groom().m_RenderStrands = false;
            std::vector<u8> without;
            Capture({}, without);
            Groom().m_RenderStrands = true;
            return DeriveMask(withCoat, without);
        }
    };

    // ── 1-3. Both directions, in the same frame, on every rendering path ────

    TEST_F(GroomSceneShadowVisualEvidenceTest, BothDirectionsAreVisibleInTheSameFrameOnEveryRenderingPath)
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

        for (const PathCase& pathCase : paths)
        {
            Renderer3D::GetRendererSettings().Path = pathCase.Path;
            Renderer3D::ApplyRendererSettings();

            Routing().m_CastShadows = false;
            Routing().m_ReceiveShadows = false;

            const std::vector<u8> coatMask = DeriveCoatMask();
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }
            const u32 coatPixels = CountMask(coatMask);
            ASSERT_GT(coatPixels, 12000u)
                << pathCase.Name
                << ": the coat covers too little of the frame for the masks to mean anything. A difference "
                   "assertion cannot catch an empty frame, so this is the coverage floor that can.";
            ASSERT_LT(coatPixels, kWidth * kHeight * 3u / 4u)
                << pathCase.Name << ": the coat mask swallowed the frame, so the 'scene' region is not the scene";

            // THE CONTROL: routed neither way.
            std::vector<u8> control;
            Capture(std::string("GroomSceneShadowOff_GL_") + pathCase.Name, control);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }

            // HAIR -> SCENE alone.
            Routing().m_CastShadows = true;
            Routing().m_ReceiveShadows = false;
            std::vector<u8> castOnly;
            Capture(std::string("GroomSceneShadow_GL_") + pathCase.Name + "_CastOnly", castOnly);

            // SCENE -> HAIR alone.
            Routing().m_CastShadows = false;
            Routing().m_ReceiveShadows = true;
            std::vector<u8> receiveOnly;
            Capture(std::string("GroomSceneShadow_GL_") + pathCase.Name + "_ReceiveOnly", receiveOnly);

            // BOTH, in ONE frame — acceptance criterion 4, which two separate
            // captures explicitly do not satisfy.
            Routing().m_CastShadows = true;
            Routing().m_ReceiveShadows = true;
            std::vector<u8> both;
            Capture(std::string("GroomSceneShadow_GL_") + pathCase.Name, both);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }

            const f64 sceneControl = MeanLumaIn(control, coatMask, /*inside=*/false);
            const f64 sceneCast = MeanLumaIn(castOnly, coatMask, false);
            const f64 sceneBoth = MeanLumaIn(both, coatMask, false);
            const f64 coatControl = MeanLumaIn(control, coatMask, /*inside=*/true);
            const f64 coatReceive = MeanLumaIn(receiveOnly, coatMask, true);
            const f64 coatBoth = MeanLumaIn(both, coatMask, true);

            const u32 sceneCastPixels = CountDifferingIn(castOnly, control, coatMask, false);
            const u32 sceneBothPixels = CountDifferingIn(both, control, coatMask, false);
            const u32 coatReceivePixels = CountDifferingIn(receiveOnly, control, coatMask, true);
            const u32 coatBothPixels = CountDifferingIn(both, control, coatMask, true);

            std::printf("[groom-scene-shadow] %-11s coat px %6u | scene luma %.3f -> cast %.3f (%u px),"
                        " both %.3f (%u px) | coat luma %.3f -> receive %.3f (%u px), both %.3f (%u px)\n",
                        pathCase.Name, coatPixels, sceneControl, sceneCast, sceneCastPixels, sceneBoth,
                        sceneBothPixels, coatControl, coatReceive, coatReceivePixels, coatBoth, coatBothPixels);

            // 1. HAIR -> SCENE. The coat's own shadow landing on the ground.
            EXPECT_LT(sceneCast, sceneControl - 0.25)
                << pathCase.Name
                << ": turning casting on did not darken the scene around the coat. A caster family that was "
                   "never routed into the frame's directional technique renders exactly like the control, and "
                   "nothing else detects that.";

            // 2. SCENE -> HAIR. The occluder's shadow landing on the coat.
            EXPECT_LT(coatReceive, coatControl - 0.25)
                << pathCase.Name
                << ": turning receiving on did not darken the coat under the occluder, so GroomStrand.glsl is "
                   "declaring the shadow samplers and not reading them";

            // 3. BOTH, IN THE SAME FRAME.
            EXPECT_LT(sceneBoth, sceneControl - 0.25) << pathCase.Name << ": the both-on frame lost the cast half";
            EXPECT_LT(coatBoth, coatControl - 0.25) << pathCase.Name << ": the both-on frame lost the receive half";
            EXPECT_GT(sceneBothPixels, 2000u)
                << pathCase.Name
                << ": too little of the scene changed in the both-on frame for the coat's shadow to be on it";
            EXPECT_GT(coatBothPixels, 2000u)
                << pathCase.Name << ": too little of the coat changed in the both-on frame";

            // AND THE COAT IS STILL LIT. A coat that is both a caster and a
            // receiver is occluded by its own strands in the map, and a shadow
            // map is a BINARY test on something that is not binary -- so
            // without the receiver offset every strand behind the outermost
            // widened ribbon reads as fully shadowed and the coat goes BLACK.
            // Measured at 0.22 mean luma against a 44.98 control before the
            // offset was regated onto CASTING rather than onto the density
            // volume. A black coat is indistinguishable from a correct
            // silhouette, which is why this floor is an assertion and not a
            // look (groom-coat-self-shadowing.md rule 10).
            EXPECT_GT(coatBoth, coatControl * 0.15)
                << pathCase.Name
                << ": the coat went black with both directions on. That is the coat occluding ITSELF through "
                   "the shadow map -- the receiver is supposed to be offset past this groom's own strands "
                   "whenever it casts.";

            // And the counters agree with the pixels. A frame where the pixels
            // moved and the counters read zero is a frame whose difference came
            // from somewhere else.
            const GroomShadowCasterStats& stats = CasterStats();
            EXPECT_EQ(stats.GroomsAskedToCast, 1u) << pathCase.Name;
            EXPECT_EQ(stats.GroomsCasting, 1u) << pathCase.Name;
            EXPECT_EQ(stats.GroomsWithoutGeometry, 0u) << pathCase.Name;
            EXPECT_GT(stats.CascadeDraws, 0u)
                << pathCase.Name
                << ": the coat is a caster and the CSM cascades drew none of it — the technique is not wired";
            EXPECT_FALSE(stats.VirtualShadowMapActive)
                << pathCase.Name << ": this fixture is the CSM cell; the VSM cell is evidenced separately";
        }
    }

    // ── 4. The one-texel floor is what makes a coat cast at all ─────────────

    TEST_F(GroomSceneShadowVisualEvidenceTest, TheOneTexelWidthFloorIsWhatMakesTheCoatCastAtAll)
    {
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();

        Routing().m_CastShadows = false;
        Routing().m_ReceiveShadows = false;
        const std::vector<u8> coatMask = DeriveCoatMask();
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        std::vector<u8> control;
        Capture({}, control);

        Routing().m_CastShadows = true;

        // THE FLOOR OFF. A 70 um strand against a cascade texel of a few
        // centimetres is about a thousandth of a texel wide, so rasterised
        // honestly it crosses a texel centre essentially never.
        Routing().m_ShadowWidthTexels = 0.0f;
        std::vector<u8> honest;
        Capture("GroomSceneShadow_GL_Forward_NoWidthFloor", honest);

        Routing().m_ShadowWidthTexels = 1.0f;
        std::vector<u8> widened;
        Capture({}, widened);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        const f64 sceneControl = MeanLumaIn(control, coatMask, false);
        const f64 sceneHonest = MeanLumaIn(honest, coatMask, false);
        const f64 sceneWidened = MeanLumaIn(widened, coatMask, false);

        const f64 honestDarkening = sceneControl - sceneHonest;
        const f64 widenedDarkening = sceneControl - sceneWidened;
        std::printf("[groom-scene-shadow] width floor 0 -> %.3f darkening, 1 texel -> %.3f darkening\n",
                    honestDarkening, widenedDarkening);

        EXPECT_GT(widenedDarkening, 0.25) << "the widened coat cast nothing measurable";

        // A LOWER BOUND, and the reason is a property of this fixture rather
        // than of the mechanism. These strands are 6 mm across so that the
        // coat is legible in a capture at all -- roughly a cascade texel -- so
        // they already cast something without the floor, and the ratio here
        // comes out near 2. Real hair is 70 um, where the same arithmetic
        // gives 139x and the honest arm casts NOTHING; that figure is a
        // measurement and it lives in
        // GroomShadowWidening.AHairIsFarBelowACascadeTexelSoTheFloorIsWhatMakesItCast,
        // because a fixture whose coat is invisible cannot carry it.
        //
        // What this case is for is that the floor is LIVE and monotone on real
        // pixels, which the CPU test cannot say.
        EXPECT_GT(widenedDarkening, honestDarkening * 1.5)
            << "widening the strands to a shadow texel bought less than half again the occlusion that "
               "rasterising them honestly did, so the floor is not reaching the raster";
    }

    // ── 6. MSAA ────────────────────────────────────────────────────────────

    TEST_F(GroomSceneShadowVisualEvidenceTest, BothDirectionsSurviveMsaa)
    {
        // MSAA IS A CELL for the reason it is one in #1248's fixture: hair is
        // sub-pixel geometry, and the light-space width floor this feature
        // rests on is a coverage argument. If anything here were evaluated per
        // SAMPLE rather than per fragment, the sample count would move it.
        //
        // MSAASampleCount lives on DeferredSettings, not RendererSettings — it
        // is a G-Buffer setting on the deferred path only. That is measured
        // here rather than assumed, and it is also why a live sweep of the MCP
        // 'msaa' token on the FORWARD path reports the frame unchanged: the
        // setting is genuinely inert there.
        //
        // EACH SAMPLE COUNT IS ITS OWN A/B, never the two frames against each
        // other: comparing the two MSAA frames would pass with the routing
        // never sampled at all, which is the "an assertion that passes on a
        // frame with nothing in it" failure.
        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        auto& deferred = Renderer3D::GetRendererSettings().Deferred;
        const u32 restoreSamples = deferred.MSAASampleCount;

        struct Variant
        {
            const char* Name;
            u32 Samples;
        };
        const std::array<Variant, 2> variants = { { { "NoMsaa", 1u }, { "Msaa4", 4u } } };

        f64 sceneEffect[2] = { 0.0, 0.0 };
        f64 coatEffect[2] = { 0.0, 0.0 };

        for (sizet v = 0; v < variants.size(); ++v)
        {
            deferred.MSAASampleCount = variants[v].Samples;
            Renderer3D::ApplyRendererSettings();

            Routing().m_CastShadows = false;
            Routing().m_ReceiveShadows = false;
            const std::vector<u8> coatMask = DeriveCoatMask();
            if (::testing::Test::HasFatalFailure())
            {
                deferred.MSAASampleCount = restoreSamples;
                Renderer3D::ApplyRendererSettings();
                return;
            }

            std::vector<u8> control;
            Capture(std::string("GroomSceneShadowOff_GL_Deferred_") + variants[v].Name, control);

            Routing().m_CastShadows = true;
            Routing().m_ReceiveShadows = true;
            std::vector<u8> routed;
            Capture(std::string("GroomSceneShadow_GL_Deferred_") + variants[v].Name, routed);
            if (::testing::Test::HasFatalFailure())
            {
                deferred.MSAASampleCount = restoreSamples;
                Renderer3D::ApplyRendererSettings();
                return;
            }

            sceneEffect[v] = MeanLumaIn(control, coatMask, false) - MeanLumaIn(routed, coatMask, false);
            coatEffect[v] = MeanLumaIn(control, coatMask, true) - MeanLumaIn(routed, coatMask, true);
            std::printf("[groom-scene-shadow] MSAA %u: scene darkening %.3f, coat darkening %.3f\n",
                        variants[v].Samples, sceneEffect[v], coatEffect[v]);

            EXPECT_GT(sceneEffect[v], 0.25)
                << variants[v].Name << ": the coat cast nothing measurable at this sample count";
            EXPECT_GT(coatEffect[v], 0.25)
                << variants[v].Name << ": the coat received nothing measurable at this sample count";
        }

        deferred.MSAASampleCount = restoreSamples;
        Renderer3D::ApplyRendererSettings();

        // And of the SAME ORDER at both counts. MSAA legitimately moves edge
        // pixels — a coat is almost all edge — but it must not halve or double
        // what the routing does, which is what a per-sample evaluation would
        // look like.
        const f64 sceneRatio = sceneEffect[1] / sceneEffect[0];
        const f64 coatRatio = coatEffect[1] / coatEffect[0];
        std::printf("[groom-scene-shadow] MSAA 4x/1x effect ratio: scene %.3f, coat %.3f\n", sceneRatio,
                    coatRatio);
        EXPECT_GT(sceneRatio, 0.5);
        EXPECT_LT(sceneRatio, 2.0);
        EXPECT_GT(coatRatio, 0.5);
        EXPECT_LT(coatRatio, 2.0);
    }

    // ── 5. A caster is not shadowed by its own cast ────────────────────────

    TEST_F(GroomSceneShadowVisualEvidenceTest, ACoatThatCastsIsNotShadowedByItsOwnStrands)
    {
        // THE REGRESSION THIS EXISTS FOR, as a number. With the receiver offset
        // gated on the density VOLUME rather than on CASTING, a coat with both
        // directions routed and no volume fell from 44.98 mean luma to 0.22 —
        // black. A shadow map is a BINARY visibility test and a coat is not
        // binary, so once this groom's own widened ribbons are in the map every
        // strand behind the outermost layer reads as fully shadowed.
        //
        // An ABSOLUTE assertion rather than a ratio, and that is the lesson of
        // the version this replaces: it compared the volume-on and volume-off
        // arms on the assumption that only one of them was offset, and once the
        // gate moved to casting both read 0.000 and the ratio became 0/0. What
        // is actually being claimed is that a coat nothing else shadows stays
        // LIT, and that is a claim about one number.
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();

        // NOTHING ELSE MAY SHADOW THE COAT, or the measurement cannot tell the
        // coat's own cast from the occluder's -- and since #1533 that includes
        // the body, which now shadows the fur on its far side from the sun.
        GetScene().DestroyEntity(m_OccluderEntity);
        m_OccluderEntity = {};
        SetBodyCasts(false);

        Routing().m_CastShadows = false;
        Routing().m_ReceiveShadows = false;
        const std::vector<u8> coatMask = DeriveCoatMask();
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        const auto measure = [&](bool volumeActive, const char* saveAs)
        {
            Coat().m_Enabled = volumeActive;

            Routing().m_CastShadows = false;
            Routing().m_ReceiveShadows = false;
            std::vector<u8> control;
            Capture({}, control);

            Routing().m_CastShadows = true;
            Routing().m_ReceiveShadows = true;
            std::vector<u8> routed;
            Capture(saveAs, routed);

            const f64 controlLuma = MeanLumaIn(control, coatMask, true);
            const f64 routedLuma = MeanLumaIn(routed, coatMask, true);
            std::printf("[groom-scene-shadow] %-9s volume: coat luma %.3f -> %.3f with both directions routed\n",
                        volumeActive ? "with" : "without", controlLuma, routedLuma);
            return std::pair<f64, f64>{ controlLuma, routedLuma };
        };

        // BOTH VOLUME STATES, because the offset is gated on CASTING and
        // therefore must hold in both — and the one that broke was the arm
        // WITHOUT a volume, which is the commonest authoring.
        const auto [noVolControl, noVolRouted] = measure(false, "GroomSceneShadow_GL_Forward_NoCoatVolume");
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }
        const auto [volControl, volRouted] = measure(true, "GroomSceneShadow_GL_Forward_CoatVolume");
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        ASSERT_GT(noVolControl, 10.0) << "the control coat is already almost black, so this case cannot measure "
                                         "whether routing darkened it";

        EXPECT_GT(noVolRouted, noVolControl * 0.8)
            << "with NO density volume and nothing else shadowing it, routing both directions darkened the coat "
               "by more than a fifth. That is the coat occluding ITSELF through the shadow map: the receiver is "
               "supposed to be offset past this groom's own strands whenever it casts, and gating that offset on "
               "the volume instead is what took this coat to 0.22 luma against a 44.98 control.";

        EXPECT_GT(volRouted, volControl * 0.8)
            << "with the density volume active, routing both directions darkened the coat by more than a fifth "
               "on top of what the volume already charges it — which is the double count the light-exit receiver "
               "offset exists to remove";
    }

    // ── 7. The Virtual Shadow Map cell, and a moving coat's cached pages ────
    //
    // #1380 routed grooms into the VSM's clip levels and never invalidated the
    // pages they had been drawn into (#1523). The VSM caches a page until
    // something marks it dirty, so a coat that MOVES leaves its old silhouette
    // in every cached page it crossed -- a shadow that stays behind, which
    // reads as a lighting bug two subsystems away. The comparison that detects
    // it is against a FROM-SCRATCH render of the same state: the VSM is
    // restarted (which drops every cached page) and the moved coat captured
    // again. A frame that matches the fresh one has no stale silhouette in it.
    TEST_F(GroomSceneShadowVisualEvidenceTest, TheVirtualShadowMapCastsAndFollowsAMovingCoat)
    {
        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        // Process-wide shadow settings: restored on EVERY exit, the way the VSM
        // evidence test restores them, or a failure here leaves VSM on for every
        // later test in the process.
        const auto setVsm = [](bool enabled)
        {
            auto& shadowMap = Renderer3D::GetShadowMap();
            ShadowSettings settings = shadowMap.GetSettings();
            settings.VSM.Enabled = enabled;
            shadowMap.SetSettings(settings);
            return shadowMap.IsVirtualShadowMapActive() == enabled;
        };
        struct VsmRestore
        {
            std::function<bool(bool)> Set;
            ~VsmRestore()
            {
                (void)Set(false);
            }
        } restore{ setVsm };
        if (!setVsm(true))
        {
            GTEST_SKIP() << "the Virtual Shadow Map could not start on this device (VirtualShadowMap::Init refused)";
        }

        Routing().m_CastShadows = false;
        Routing().m_ReceiveShadows = false;
        const std::vector<u8> coatMask = DeriveCoatMask();
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }
        std::vector<u8> control;
        Capture("GroomSceneShadowOff_GL_Deferred_Vsm", control);

        Routing().m_CastShadows = true;
        std::vector<u8> cast;
        Capture("GroomSceneShadow_GL_Deferred_Vsm", cast);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }
        const GroomShadowCasterStats castStats = CasterStats();
        const f64 sceneControl = MeanLumaIn(control, coatMask, /*inside=*/false);
        const f64 sceneCast = MeanLumaIn(cast, coatMask, false);
        const u32 castPixels = CountDifferingIn(cast, control, coatMask, false);
        std::printf("[groom-scene-shadow] VSM         scene luma %.3f -> cast %.3f (%u px) | level draws %u, "
                    "cascade draws %u\n",
                    sceneControl, sceneCast, castPixels, castStats.VirtualShadowLevelDraws, castStats.CascadeDraws);

        EXPECT_TRUE(castStats.VirtualShadowMapActive) << "this is the VSM cell; the frame ran the CSM cascades";
        EXPECT_GT(castStats.VirtualShadowLevelDraws, 0u)
            << "the coat is a caster and no VSM clip level drew it -- the family is not wired into the VSM";
        EXPECT_LT(sceneCast, sceneControl - 0.25) << "casting through the VSM did not darken the scene";
        EXPECT_GT(castPixels, 2000u) << "too little of the scene changed for the coat's VSM shadow to be on it";

        // Move the coat sideways. The body stays: it is a separate caster, and
        // its shadow is the same in every frame compared below.
        auto& groomTransform = m_GroomEntity.GetComponent<TransformComponent>();
        const glm::vec3 home = groomTransform.Translation;
        const glm::vec3 away = home + glm::vec3(-1.3f, 0.0f, 0.4f);
        const auto restartVsm = [&setVsm]()
        {
            // Restarting the VSM drops every cached page, so the next frame is a
            // from-scratch render with nothing in it that could be stale.
            return setVsm(false) && setVsm(true);
        };

        // THE NEGATIVE CONTROL FIRST: the same move with the groom invalidation
        // switched off. It must leave the old silhouette behind, or the
        // stale-page check below could not see staleness at all -- a VSM that
        // happened to redraw every page every frame would pass it for free.
        struct FaultRestore
        {
            ~FaultRestore()
            {
                Levers::SetFaultSkipGroomVsmInvalidation(false);
            }
        } faultRestore;
        Levers::SetFaultSkipGroomVsmInvalidation(true);
        groomTransform.Translation = away;
        std::vector<u8> movedWithoutInvalidation;
        Capture("GroomSceneShadow_GL_Deferred_VsmMovedNoInvalidation", movedWithoutInvalidation);
        Levers::SetFaultSkipGroomVsmInvalidation(false);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        // The reference: the coat at `away`, rendered from scratch.
        ASSERT_TRUE(restartVsm());
        std::vector<u8> fresh;
        Capture("GroomSceneShadow_GL_Deferred_VsmMovedFresh", fresh);
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        // The real move: warm the cache at home from scratch, then move once and
        // read the counter on THAT frame -- the frame the move happens in is the
        // only one that has anything to invalidate for an unbound coat.
        groomTransform.Translation = home;
        ASSERT_TRUE(restartVsm());
        Step(3);
        groomTransform.Translation = away;
        Step(1);
        const u32 invalidations = CasterStats().VirtualShadowInvalidations;
        std::vector<u8> moved;
        Capture("GroomSceneShadow_GL_Deferred_VsmMoved", moved);
        groomTransform.Translation = home;
        if (::testing::Test::HasFatalFailure())
        {
            return;
        }

        const std::vector<u8> none(coatMask.size(), 0u);
        const u32 stalePixels = CountDifferingIn(moved, fresh, none, /*inside=*/false);
        const u32 staleWithoutInvalidation = CountDifferingIn(movedWithoutInvalidation, fresh, none, false);
        const u32 shadowMovedPixels = CountDifferingIn(cast, fresh, none, false);
        std::printf("[groom-scene-shadow] VSM moved   invalidations %u | moved vs fresh %u px, without invalidation "
                    "%u px (negative control) | home vs fresh %u px (the shadow moved)\n",
                    invalidations, stalePixels, staleWithoutInvalidation, shadowMovedPixels);

        // THE SENSITIVITY CHECKS FIRST: the comparison can see a moved shadow at
        // all, and it can see a stale one.
        ASSERT_GT(shadowMovedPixels, 2000u)
            << "moving the coat did not move anything the comparison can see, so the stale-page check below "
               "would pass on a frame with no groom shadow in it";
        ASSERT_GT(staleWithoutInvalidation, 2000u)
            << "with the groom invalidation switched off the moved frame still matched a from-scratch render, "
               "so this check cannot see a stale page and its pass below would mean nothing";
        EXPECT_GT(invalidations, 0u) << "the coat moved with the VSM on and no page was invalidated for it";
        EXPECT_LT(stalePixels, 250u)
            << "the moved coat's frame differs from a from-scratch render of the same state by " << stalePixels
            << " pixels: its old silhouette is still in the VSM's cached pages";
    }

    // ── 8. The body shadows its own fur, and the fur still not itself ────────
    //
    // #1323 moved every strand's lookup to where the light LEAVES the coat's
    // box, so its own strands in the map could not shadow it twice. That point
    // is outside the body, so the map answered only for occluders beyond the
    // coat: the body the fur grows on shadowed none of it, and a coat lit from
    // behind was lit THROUGH the animal. Since #1533 the strands sample the
    // OPAQUE cascades -- copied before any groom cast -- at themselves.
    //
    // Three frames settle it, all with both directions routed and the sun
    // behind the coat, so the fur the camera sees is on the body's far side:
    //
    //   * the old receiver, re-created by a fault lever: the coat must stay
    //     LIT, or this case could not see the defect at all;
    //   * the fix: the body darkens the coat it shadows;
    //   * the fix with the body not casting: nothing else shadows the coat, so
    //     it must be as lit as with no routing -- its own strands are not in
    //     the copy, which is the double count the exit point existed to stop.
    TEST_F(GroomSceneShadowVisualEvidenceTest, TheBodyShadowsItsOwnFurAndTheFurStillNotItself)
    {
        // BEHIND AND ABOVE, towards the camera: the camera faces the body's
        // shadowed side, and the coat's own cast falls in front of the body.
        RunBacklitBodyCase("sun", "GroomSceneShadow_GL_Forward_Backlit", ShadowMap::OpaqueCopy::Cascades,
                           [this]
                           {
                               m_LightEntity.GetComponent<DirectionalLightComponent>().m_Direction =
                                   glm::normalize(glm::vec3(0.25f, -0.55f, 0.80f));
                           });
    }

    // The same under a SPOT light, whose shadow is a tile of the local-light
    // atlas: the atlas is split and copied as the cascades are, and a strand
    // samples the opaque atlas at itself. The sun goes, so the spot is the only
    // light and every darkened pixel is its shadow.
    TEST_F(GroomSceneShadowVisualEvidenceTest, TheBodyShadowsItsOwnFurUnderASpotLight)
    {
        RunBacklitBodyCase("spot", "GroomSceneShadow_GL_Forward_SpotBacklit", ShadowMap::OpaqueCopy::Atlas,
                           [this]
                           {
                               GetScene().DestroyEntity(m_LightEntity);
                               m_LightEntity = GetScene().CreateEntity("Spot");
                               const glm::vec3 travels = glm::normalize(glm::vec3(0.25f, -0.55f, 0.80f));
                               // Six metres back along the sun's direction, its
                               // cone just wider than the coat seen from there.
                               m_LightEntity.GetComponent<TransformComponent>().Translation =
                                   glm::vec3(0.0f, 1.45f, 0.0f) - (travels * 6.0f);
                               auto& spot = m_LightEntity.AddComponent<SpotLightComponent>();
                               spot.m_Direction = travels;
                               spot.m_Color = glm::vec3(1.0f, 0.97f, 0.93f);
                               spot.m_Intensity = 60.0f;
                               spot.m_Range = 15.0f;
                               spot.m_InnerCutoff = 20.0f;
                               spot.m_OuterCutoff = 26.0f;
                               spot.m_CastShadows = true;
                           });
    }
} // namespace OloEngine::Tests

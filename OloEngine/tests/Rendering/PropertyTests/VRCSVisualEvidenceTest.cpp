// =============================================================================
// VRCSVisualEvidenceTest.cpp
//
// OLO_TEST_LAYER: L8
//
// Variable Rate Compute Shading (issue #683), through the FULL deferred
// pipeline, from two poses, as PNG evidence plus a golden-free differential
// contract.
//
// This test exists because VRCS's acceptance criterion is not a number the unit
// layer can produce. VRCSContractTest proves the leader/broadcast partition is
// sound and VRCSClassifierGpuTest proves the classifier refuses silhouettes on
// synthetic input — and BOTH can pass while the composited frame grows visible
// 8-pixel blocks, because neither of them ever looks at a frame. The criterion
// is "visually equal output, image-diff within threshold vs. full rate, no tile
// seams in smooth gradients", and that is what is asserted here.
//
// Same scene as GTAOVisualEvidenceTest — grey cube on a grey floor under an
// overhead white sun, in front of a skybox — so the two are directly
// comparable, and so the contact crease (the feature VRCS must not smear) is in
// a known place.
//
// Evidence written to OloEditor/assets/tests/visual/:
//   VRCS_AO_Off_<pose>.png    AO buffer, GTAO at full rate
//   VRCS_AO_On_<pose>.png     AO buffer, GTAO consuming shading rates
//   VRCS_Rate_<pose>.png      the rate heatmap (white 1x1, mid-grey 2x2)
//   VRCS_Lit_Off_<pose>.png   lit composite, full rate
//   VRCS_Lit_On_<pose>.png    lit composite, variable rate
//
// THE THREE ASSERTIONS THAT MATTER, and why each is shaped the way it is:
//
//   1. IMAGE DIFF. Mean absolute luma difference between the full-rate and
//      variable-rate AO buffers, and between the two lit composites. A mean is
//      the right statistic for "visually equal" and the wrong one for "no
//      seams" — a one-pixel-wide artefact vanishes into a mean over a million
//      pixels — which is why it is not the only check.
//
//   2. TILE SEAMS IN A SMOOTH GRADIENT. Measured as a RATIO, on the open floor,
//      where the AO signal is smooth by construction: the mean absolute step
//      between horizontally adjacent pixels that STRADDLE an 8-pixel tile
//      boundary, over the same quantity for pairs that do not. A coarsening
//      artefact is periodic at the tile pitch, so it inflates the numerator and
//      not the denominator; a global change in contrast moves both and cancels.
//      The same ratio is computed on the FULL-RATE frame as the control, so the
//      claim is "VRCS did not make boundary steps worse", not "boundary steps
//      are small" — which would be a claim about GTAO, not about VRCS.
//
//   3. EDGE PRESERVATION, on the output rather than on the classifier. The
//      contact crease must still be clearly darker than the open floor WITH
//      VRCS on, and the crease's own darkness must not have washed out relative
//      to full rate. This is the assertion that would fail if the classifier
//      were quietly coarsening silhouettes — the failure mode
//      VRCSClassifierGpuTest can only catch on inputs it invented.
//
// TAA stability is checked separately below by re-rendering the same static
// pose and comparing consecutive frames: if VRCS made the AO term flicker, the
// frame-to-frame delta would rise against the full-rate control.
//
// Runs in the normal suite and SKIPs (not fails) when no GL 4.6 context exists,
// matching every other *VisualEvidenceTest.
//
// Classification: L8 (full GL pipeline + RGBA8 readback + PNG evidence).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 1024;
        constexpr u32 kHeight = 768;
        constexpr u32 kTilePitch = 8u; // ShadingRateClassifier::kTileSize, pinned by VRCSContractTest
        constexpr f32 kCaptureTime = 4.0f;

        [[nodiscard]] f64 Luma(const std::vector<u8>& px, u32 x, u32 y)
        {
            const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
            if (idx + 2 >= px.size())
                return 0.0;
            return 0.2126 * px[idx + 0] + 0.7152 * px[idx + 1] + 0.0722 * px[idx + 2];
        }

        [[nodiscard]] f64 BandLuma(const std::vector<u8>& px, f32 x0, f32 x1, f32 y0, f32 y1)
        {
            const u32 ix0 = static_cast<u32>(x0 * kWidth);
            const u32 ix1 = static_cast<u32>(x1 * kWidth);
            const u32 iy0 = static_cast<u32>(y0 * kHeight);
            const u32 iy1 = static_cast<u32>(y1 * kHeight);
            f64 sum = 0.0;
            u64 count = 0;
            for (u32 y = iy0; y < iy1; ++y)
            {
                for (u32 x = ix0; x < ix1; ++x)
                {
                    sum += Luma(px, x, y);
                    ++count;
                }
            }
            return count ? sum / static_cast<f64>(count) : 0.0;
        }

        [[nodiscard]] f64 MeanAbsLumaDiff(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
                return 1e9;
            f64 sum = 0.0;
            u64 count = 0;
            for (u32 y = 0; y < kHeight; ++y)
            {
                for (u32 x = 0; x < kWidth; ++x)
                {
                    sum += std::abs(Luma(a, x, y) - Luma(b, x, y));
                    ++count;
                }
            }
            return count ? sum / static_cast<f64>(count) : 1e9;
        }

        // The seam metric. Walks a UV region and separates horizontally adjacent
        // pixel pairs into two populations: those whose right-hand member starts
        // a new 8-pixel tile column, and all the others. A coarsening artefact
        // lives ONLY in the first population, because that is where one leader's
        // broadcast meets the next leader's; anything that moves both equally
        // (contrast, exposure, noise) cancels in the ratio.
        struct SeamMeasure
        {
            f64 AcrossBoundary = 0.0;
            f64 WithinTile = 0.0;

            [[nodiscard]] f64 Ratio() const
            {
                return WithinTile > 1e-6 ? AcrossBoundary / WithinTile : 0.0;
            }
        };

        [[nodiscard]] SeamMeasure MeasureTileSeams(const std::vector<u8>& px, f32 x0, f32 x1, f32 y0, f32 y1)
        {
            const u32 ix0 = static_cast<u32>(x0 * kWidth);
            const u32 ix1 = static_cast<u32>(x1 * kWidth);
            const u32 iy0 = static_cast<u32>(y0 * kHeight);
            const u32 iy1 = static_cast<u32>(y1 * kHeight);

            f64 acrossSum = 0.0;
            u64 acrossCount = 0;
            f64 withinSum = 0.0;
            u64 withinCount = 0;

            for (u32 y = iy0; y < iy1; ++y)
            {
                for (u32 x = ix0; x + 1u < ix1; ++x)
                {
                    const f64 step = std::abs(Luma(px, x + 1u, y) - Luma(px, x, y));
                    if (((x + 1u) % kTilePitch) == 0u)
                    {
                        acrossSum += step;
                        ++acrossCount;
                    }
                    else
                    {
                        withinSum += step;
                        ++withinCount;
                    }
                }
            }

            SeamMeasure m;
            m.AcrossBoundary = acrossCount ? acrossSum / static_cast<f64>(acrossCount) : 0.0;
            m.WithinTile = withinCount ? withinSum / static_cast<f64>(withinCount) : 0.0;
            return m;
        }

        [[nodiscard]] f64 DarkestCellLuma(const std::vector<u8>& px, f32 x0, f32 x1, f32 y0, f32 y1,
                                          u32 cellsX, u32 cellsY)
        {
            f64 darkest = 1e9;
            const f32 dx = (x1 - x0) / static_cast<f32>(cellsX);
            const f32 dy = (y1 - y0) / static_cast<f32>(cellsY);
            for (u32 cy = 0; cy < cellsY; ++cy)
            {
                for (u32 cx = 0; cx < cellsX; ++cx)
                {
                    const f32 cellX0 = x0 + dx * static_cast<f32>(cx);
                    const f32 cellY0 = y0 + dy * static_cast<f32>(cy);
                    darkest = std::min(darkest, BandLuma(px, cellX0, cellX0 + dx, cellY0, cellY0 + dy));
                }
            }
            return darkest;
        }
    } // namespace

    class VRCSVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();

            EnableRendering(kWidth, kHeight);

            Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
            Renderer3D::ApplyRendererSettings();

            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 20.0f, 0.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(0.25f, -0.92f, 0.3f));
                dl.m_Color = glm::vec3(1.0f, 1.0f, 1.0f);
                dl.m_Intensity = 1.5f;
                dl.m_CastShadows = false; // isolate the AO contribution, as GTAOVisualEvidenceTest does
            }

            {
                Entity sky = scene.CreateEntity("Skybox");
                auto& env = sky.AddComponent<EnvironmentMapComponent>();
                env.m_FilePath = "assets/textures/Skybox";
                env.m_IsCubemapFolder = true;
                env.m_EnableSkybox = true;
                env.m_EnableIBL = false;
            }

            auto addMesh = [&scene](const char* name, MeshPrimitive prim, const glm::vec3& pos,
                                    const glm::vec3& scale)
            {
                Entity e = scene.CreateEntity(name);
                auto& tc = e.GetComponent<TransformComponent>();
                tc.Translation = pos;
                tc.Scale = scale;
                auto& mc = e.AddComponent<MeshComponent>();
                mc.m_Primitive = prim;
                Ref<Mesh> mesh = (prim == MeshPrimitive::Plane) ? MeshPrimitives::CreatePlane()
                                                                : MeshPrimitives::CreateCube();
                if (mesh)
                    mc.m_MeshSource = mesh->GetMeshSource();
                return e;
            };

            {
                Entity floor = addMesh("GreyFloor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f },
                                       { 40.0f, 1.0f, 40.0f });
                auto& mat = floor.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.7f, 0.7f, 0.7f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
            }

            {
                Entity cube = addMesh("Occluder", MeshPrimitive::Cube, { 0.0f, 2.5f, 0.0f },
                                      { 5.0f, 5.0f, 5.0f });
                auto& mat = cube.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.6f, 0.6f, 0.6f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
            }
        }

        void Capture(const std::string& tag, const glm::vec3& position, f32 yaw, f32 pitch,
                     std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, yaw, pitch);
            RunEditorFrames(camera, 2);
            ReadAndWrite(tag, outPixels);
        }

        void ReadAndWrite(const std::string& tag, std::vector<u8>& outPixels)
        {
            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for VRCS capture '" << tag << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            // GL readback is bottom-up; flip so row 0 is the top of the frame —
            // and so the tile-pitch arithmetic in MeasureTileSeams operates on
            // the same grid the classifier used (its tiles are anchored at the
            // image origin, and a flip would offset them by kHeight % 8).
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

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ASSERT_FALSE(ec) << "Failed to create evidence dir: " << ec.message();

            const std::string path = (dir / ("VRCS_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth),
                                               static_cast<int>(kHeight), 4, outPixels.data(),
                                               static_cast<int>(kWidth) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";
        }

        // GTAO at production defaults. Every VRCS knob is set explicitly so a
        // change to the shipped defaults cannot silently change what is tested.
        static void ApplyGTAO(bool vrcsEnabled, bool debugOverlay, bool aoDebugView)
        {
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.ActiveAOTechnique = AOTechnique::GTAO;
            pp.GTAOEnabled = true;
            pp.GTAORadius = 1.5f;
            pp.GTAOPower = 2.2f;
            pp.GTAOFalloffRange = 0.615f;
            pp.GTAOSampleDistribution = 2.0f;
            pp.GTAOThinCompensation = 0.0f;
            pp.GTAODepthMipOffset = 3.3f;
            pp.GTAODenoiseEnabled = true;
            pp.GTAODenoisePasses = 4;
            pp.GTAODenoiseBeta = 1.2f;
            pp.GTAODebugView = aoDebugView;

            pp.VRCSEnabled = vrcsEnabled;
            pp.VRCSGTAO = true;
            pp.VRCSAllow4x4 = false; // 2x2 only: the shipped default, and the tier this contract covers
            pp.VRCSDepthThreshold = 0.01f;
            pp.VRCSNormalThreshold = 0.02f;
            pp.VRCSLumaThreshold = 0.25f;
            pp.VRCS4x4ToleranceScale = 0.25f;
            pp.VRCSDebugOverlay = debugOverlay;

            // Both VRCS gates are hashed into the blackboard fingerprint and
            // GTAORenderPass::Setup branches on them, so every flip needs an
            // apply before the next capture — the same rule GTAOVisualEvidence
            // records for GTAOEnabled / GTAODebugView.
            Renderer3D::ApplyRendererSettings();
        }
    };

    TEST_F(VRCSVisualEvidenceTest, VariableRateMatchesFullRateWithoutTileSeams)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        struct ScopedMockTime
        {
            explicit ScopedMockTime(f32 t)
            {
                Time::SetMockTime(t);
            }
            ~ScopedMockTime()
            {
                Time::ClearMockTime();
            }
        } scopedMockTime(kCaptureTime);

        struct Pose
        {
            const char* Name;
            glm::vec3 Position;
            f32 Yaw;
            f32 Pitch;
        };

        // The same two grazing poses GTAOVisualEvidenceTest uses: sky across the
        // top, lit floor and cube below, and the floor tilted rather than
        // face-on — which is where a coarsened AO term has the most room to go
        // wrong and where the depth range inside a tile is largest.
        const std::array<Pose, 2> poses = { {
            { "Angled", { 0.0f, 6.0f, 22.0f }, 0.0f, 0.30f },
            { "Higher", { 0.0f, 8.0f, 19.0f }, 0.0f, 0.42f },
        } };

        // Open floor, clear of both the cube and the contact crease — smooth by
        // construction, which is what makes it the right place to look for a
        // seam. Crease band straddles the cube base across the centre.
        constexpr f32 kFloorX0 = 0.08f, kFloorX1 = 0.30f, kFloorY0 = 0.60f, kFloorY1 = 0.76f;
        constexpr f32 kCreaseX0 = 0.28f, kCreaseX1 = 0.72f, kCreaseY0 = 0.40f, kCreaseY1 = 0.62f;

        for (const Pose& pose : poses)
        {
            SCOPED_TRACE(pose.Name);

            ApplyGTAO(/*vrcsEnabled*/ false, /*debugOverlay*/ false, /*aoDebugView*/ true);
            std::vector<u8> aoOff;
            Capture(std::string("AO_Off_") + pose.Name, pose.Position, pose.Yaw, pose.Pitch, aoOff);
            if (::testing::Test::HasFatalFailure())
                return;

            ApplyGTAO(/*vrcsEnabled*/ true, /*debugOverlay*/ false, /*aoDebugView*/ true);
            std::vector<u8> aoOn;
            Capture(std::string("AO_On_") + pose.Name, pose.Position, pose.Yaw, pose.Pitch, aoOn);
            if (::testing::Test::HasFatalFailure())
                return;

            ApplyGTAO(/*vrcsEnabled*/ true, /*debugOverlay*/ true, /*aoDebugView*/ true);
            std::vector<u8> heat;
            Capture(std::string("Rate_") + pose.Name, pose.Position, pose.Yaw, pose.Pitch, heat);
            if (::testing::Test::HasFatalFailure())
                return;

            ApplyGTAO(/*vrcsEnabled*/ false, /*debugOverlay*/ false, /*aoDebugView*/ false);
            std::vector<u8> litOff;
            Capture(std::string("Lit_Off_") + pose.Name, pose.Position, pose.Yaw, pose.Pitch, litOff);
            if (::testing::Test::HasFatalFailure())
                return;

            ApplyGTAO(/*vrcsEnabled*/ true, /*debugOverlay*/ false, /*aoDebugView*/ false);
            std::vector<u8> litOn;
            Capture(std::string("Lit_On_") + pose.Name, pose.Position, pose.Yaw, pose.Pitch, litOn);
            if (::testing::Test::HasFatalFailure())
                return;

            // ---- 0) The frames drew at all. Every threshold below is satisfied
            //         by two black images, so this comes first.
            EXPECT_GT(BandLuma(litOff, kFloorX0, kFloorX1, kFloorY0, kFloorY1), 20.0)
                << "full-rate lit floor rendered (near-)black";
            EXPECT_GT(BandLuma(litOn, kFloorX0, kFloorX1, kFloorY0, kFloorY1), 20.0)
                << "variable-rate lit floor rendered (near-)black. See VRCS_Lit_On_" << pose.Name << ".png";

            // ---- 1) The heatmap proves VRCS actually engaged. Without this the
            //         "matches full rate" assertions below are trivially
            //         satisfied by a feature that silently did nothing — which
            //         is exactly what a wrong enable gate looks like.
            //         Read differentially rather than against an absolute luma:
            //         the heatmap goes through the AO-apply and tonemap chain
            //         like any other AO value, so "0.5 means 2x2" is not a
            //         pixel value this test can predict. What it CAN predict is
            //         the ordering — the open floor coarsens, the cube's
            //         contact silhouette does not, so the floor must come out
            //         darker than the crease band.
            const f64 heatFloor = BandLuma(heat, kFloorX0, kFloorX1, kFloorY0, kFloorY1);
            const f64 heatCrease = BandLuma(heat, kCreaseX0, kCreaseX1, kCreaseY0, kCreaseY1);
            EXPECT_LT(heatFloor, heatCrease)
                << "the heatmap shows the open floor (" << heatFloor << ") no coarser than the contact "
                << "silhouette (" << heatCrease
                << ") — either VRCS coarsened nothing, in which case the diff assertions below prove "
                   "nothing, or it coarsened the silhouette, which is the bug. See VRCS_Rate_"
                << pose.Name << ".png";

            // ---- 2) IMAGE DIFF. The AO buffer is where a coarsened term shows
            //         first; the lit composite is what a player sees.
            const f64 aoDiff = MeanAbsLumaDiff(aoOff, aoOn);
            const f64 litDiff = MeanAbsLumaDiff(litOff, litOn);
            EXPECT_LT(aoDiff, 8.0) << "variable-rate AO differs from full-rate AO by " << aoDiff
                                   << "/255 mean absolute luma. See VRCS_AO_Off_" << pose.Name
                                   << ".png vs VRCS_AO_On_" << pose.Name << ".png";
            EXPECT_LT(litDiff, 5.0) << "variable-rate composite differs from full-rate by " << litDiff
                                    << "/255 mean absolute luma. See VRCS_Lit_Off_" << pose.Name
                                    << ".png vs VRCS_Lit_On_" << pose.Name << ".png";

            // ---- 3) TILE SEAMS IN A SMOOTH GRADIENT, as a ratio against the
            //         full-rate control. A mean image diff cannot see this: a
            //         one-pixel step repeated every eight columns is a rounding
            //         error in the mean and an obvious lattice on screen.
            const SeamMeasure seamOff = MeasureTileSeams(aoOff, kFloorX0, kFloorX1, kFloorY0, kFloorY1);
            const SeamMeasure seamOn = MeasureTileSeams(aoOn, kFloorX0, kFloorX1, kFloorY0, kFloorY1);
            EXPECT_GT(seamOff.WithinTile, 0.0) << "the control frame has no within-tile variation at all, so "
                                                  "the seam ratio is undefined — is the floor flat-shaded?";
            EXPECT_LT(seamOn.Ratio(), seamOff.Ratio() + 0.6)
                << "steps ACROSS 8-pixel tile boundaries grew relative to steps within a tile: "
                << seamOff.Ratio() << " -> " << seamOn.Ratio()
                << ". That is the signature of a visible coarsening lattice on smooth ground. See VRCS_AO_On_"
                << pose.Name << ".png";

            // ---- 4) EDGE PRESERVATION, measured on the output. The contact
            //         crease is the feature the classifier is supposed to
            //         protect; if it were coarsening silhouettes the crease
            //         would fill in from its neighbours.
            //         Stated as a CONTRAST RATIO differential, not as an
            //         absolute margin. How much darker the crease is than the
            //         floor is a property of GTAO's contrast curve in this
            //         scene — an absolute factor here would be a hand-picked
            //         constant describing GTAO, and it would fail or pass for
            //         reasons that have nothing to do with shading rate. What
            //         belongs to VRCS is whether that contrast SURVIVED, so the
            //         full-rate frame supplies the reference.
            const f64 floorAOOff = BandLuma(aoOff, kFloorX0, kFloorX1, kFloorY0, kFloorY1);
            const f64 floorAOOn = BandLuma(aoOn, kFloorX0, kFloorX1, kFloorY0, kFloorY1);
            const f64 creaseAOOff = DarkestCellLuma(aoOff, kCreaseX0, kCreaseX1, kCreaseY0, kCreaseY1, 16, 12);
            const f64 creaseAOOn = DarkestCellLuma(aoOn, kCreaseX0, kCreaseX1, kCreaseY0, kCreaseY1, 16, 12);

            ASSERT_GT(floorAOOff, 1.0);
            ASSERT_GT(floorAOOn, 1.0);
            const f64 contrastOff = creaseAOOff / floorAOOff;
            const f64 contrastOn = creaseAOOn / floorAOOn;

            // There is contact occlusion to preserve in the first place. Guards
            // the case where GTAO produced nothing and both ratios are ~1, in
            // which case the differential below is vacuously satisfied.
            EXPECT_LT(contrastOff, 0.95)
                << "the full-rate frame has no measurable contact occlusion (crease/floor = " << contrastOff
                << "), so the preservation check below cannot mean anything";

            // 0.03 of headroom on a ratio in [0,1]: enough for the denoiser's
            // and TAA's frame-to-frame wobble, far less than the fill-in a
            // coarsened silhouette would produce.
            EXPECT_LT(contrastOn, contrastOff + 0.03)
                << "contact occlusion washed out: crease/floor went " << contrastOff << " -> " << contrastOn
                << " (crease " << creaseAOOff << " -> " << creaseAOOn << ", floor " << floorAOOff << " -> "
                << floorAOOn << "). The classifier is coarsening a silhouette. See VRCS_AO_On_" << pose.Name
                << ".png";
        }
    }

    // =========================================================================
    // TAA stability. VRCS decides per frame which invocation shades, so a
    // classification that flickers between 1x1 and 2x2 on a static view would
    // make the AO term shimmer — invisible in any single screenshot and
    // extremely visible in motion.
    //
    // Measured as a frame-to-frame delta at a FIXED pose, against the full-rate
    // control. An absolute threshold here would be a claim about GTAO's own
    // temporal noise (its spatio-temporal Hilbert/R2 dither is deliberately
    // animated under TAA); the comparison against the control is a claim about
    // VRCS, which is what this test is for.
    // =========================================================================
    TEST_F(VRCSVisualEvidenceTest, VariableRateDoesNotDestabiliseTheFrameUnderTAA)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        struct ScopedMockTime
        {
            explicit ScopedMockTime(f32 t)
            {
                Time::SetMockTime(t);
            }
            ~ScopedMockTime()
            {
                Time::ClearMockTime();
            }
        } scopedMockTime(kCaptureTime);

        Renderer3D::GetPostProcessSettings().TAAEnabled = true;

        const glm::vec3 position{ 0.0f, 6.0f, 22.0f };
        constexpr f32 kYaw = 0.0f;
        constexpr f32 kPitch = 0.30f;

        const auto consecutiveFrameDelta = [&](bool vrcsEnabled, const char* tag) -> f64
        {
            ApplyGTAO(vrcsEnabled, /*debugOverlay*/ false, /*aoDebugView*/ false);

            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, kYaw, kPitch);

            // Settle first: TAA needs several frames to converge from a cold
            // history, and comparing two frames during convergence measures the
            // accumulation ramp rather than any residual instability.
            RunEditorFrames(camera, 12);

            std::vector<u8> first;
            ReadAndWrite(std::string("TAA_") + tag + "_A", first);
            if (::testing::Test::HasFatalFailure())
                return -1.0;

            RunEditorFrames(camera, 1);
            std::vector<u8> second;
            ReadAndWrite(std::string("TAA_") + tag + "_B", second);
            if (::testing::Test::HasFatalFailure())
                return -1.0;

            return MeanAbsLumaDiff(first, second);
        };

        const f64 fullRateDelta = consecutiveFrameDelta(false, "FullRate");
        ASSERT_GE(fullRateDelta, 0.0);
        const f64 variableRateDelta = consecutiveFrameDelta(true, "VariableRate");
        ASSERT_GE(variableRateDelta, 0.0);

        // Absolute headroom on top of the ratio so a full-rate control that
        // settles to ~0 does not make the comparison infinitely strict — the
        // question is whether VRCS introduced shimmer, not whether the renderer
        // is bit-stable.
        EXPECT_LT(variableRateDelta, fullRateDelta * 2.0 + 1.5)
            << "consecutive settled frames differ by " << variableRateDelta
            << "/255 with VRCS on against " << fullRateDelta
            << " at full rate — the classification is flickering on a static view";
    }
} // namespace OloEngine::Tests

// OLO_TEST_LAYER: L8
// =============================================================================
// FoliageLeafTransmissionEvidenceTest.cpp
//
// Visual evidence for issue #1234: a backlit clump and a shrub, rendered
// through the real editor pipeline on a live GL context, across the
// {Forward, Forward+, Deferred} paths and the {front, side, back, shadowed-back}
// lighting directions the issue's demonstrating case needs.
//
// WHAT IS MEASURED, AND WHY IT IS NOT A THRESHOLD ON ONE FRAME.
//
// Every claim here is an A/B against a CONTROL: the same scene, same camera,
// same clock, rendered with the layer's TransmissionStrength as authored and
// again with it at 0 — which is the documented off switch, so the control arm
// is byte-for-byte the pre-#1234 material. The quantity is the MEAN ABSOLUTE
// DIFFERENCE between the two frames, and this file calls it the layer's
// TRANSMISSION CONTRIBUTION.
//
// That framing is what makes the cross-path comparison possible at all. The
// forward and deferred paths legitimately differ on a foliage frame for a dozen
// reasons that have nothing to do with this material — different ambient tiers,
// SSAO only on deferred, a different composite order. A pixel A/B *between
// paths* would measure all of that and call it a finding. An A/B of
// (path with the material) against (the same path without it) isolates exactly
// the term #1234 added, and the three isolated terms CAN be compared.
//
// The claims, each falsifiable in both directions:
//
//   BACKLIT > NOISE       — the term is on screen at all. The renderer's own
//                           run-to-run variance is measured by capturing one
//                           arm twice, so "bigger than noise" is an absolute
//                           bound and not a guess.
//   FRONT-LIT << BACKLIT  — this is a forward-scattering lobe. A term that was
//                           as strong from the front would be an ambient fill,
//                           and every screenshot of a backlit plant would look
//                           identical to one of a front-lit plant.
//   SHADOWED-BACK << BACK — #1234's second criterion in pixels. The same
//                           backlit camera, with a cube between the sun and the
//                           plants: the term must collapse. An unshadowed
//                           ambient constant — the wrong answer the issue names
//                           — would be UNCHANGED here, and that is the single
//                           most valuable number in this file.
//   ALL THREE PATHS CARRY IT — forward, forward+ and deferred each produce a
//                           contribution above the noise floor, within a band
//                           of each other. The band is deliberately loose (3x)
//                           and the reason is stated at the assertion: the
//                           INDIRECT half of the term reads the ambient
//                           environment, and the three paths reach the
//                           environment through different ladders. What a loose
//                           band still catches is the failure that actually
//                           happens — one path dropping the term entirely, or
//                           evaluating it at a different order of magnitude
//                           because its lobe parameters never arrived.
//   THE BACK FACE TRANSMITS TOO — seen from the far side, the leaf still
//                           transmits. The two-sided normal rule is pinned
//                           exactly by FoliageLeafTransmissionTest; this checks
//                           the pixels agree.
//
// The PNGs are written on EVERY run, not only under --olo-golden-rebase, and
// their names encode the cell (<Feature>[Off]_<Backend>_<Path>_<Angle>.png) so
// a cell nobody ran is a file missing from the diff.
//
// KNOWN-UNREACHABLE HERE: every VULKAN cell. This fixture needs a real GL 4.6
// context (it skips cleanly without one) and cannot select the Vulkan backend,
// so Vulkan is verified live in the editor and reported in the PR's matrix, not
// here. Saying so in the file is the point — a reader counting PNGs must not
// mistake a full set of GL captures for a full matrix.
// =============================================================================

#include "OloEnginePCH.h"
#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Terrain/Foliage/FoliageRenderer.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
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

        constexpr u32 kWidth = 960;
        constexpr u32 kHeight = 540;
        // A fixed clock: wind is disabled on these layers anyway, but the
        // capture time also seeds the LOD dither, and an A/B whose two arms
        // dithered differently would measure the dither.
        constexpr f32 kCaptureTime = 3.0f;

        // The Drift conifer — the same asset FoliageAuthoredMeshEvidenceTest and
        // Woodland.olo use. Relative to OloEditor/, the suite's working
        // directory.
        constexpr const char* kPineMesh = "SandboxProject/Assets/Models/Vegetation/pine.obj";
        constexpr const char* kFoliageAlbedo = "assets/textures/grass.png";
        // The authored leaf maps that go with that cutout — generated by
        // assets/textures/generate_leaf_maps.py, committed, and deterministic.
        // Set on BOTH layers so the captures exercise the MAPPED surface, not
        // only the constants: criterion 1 is about authorable maps and criterion
        // 4 about mapped detail, and neither is demonstrated by a uniform leaf.
        constexpr const char* kLeafNormal = "assets/textures/leaf_normal.png";
        constexpr const char* kLeafRoughness = "assets/textures/leaf_roughness.png";
        constexpr const char* kLeafThickness = "assets/textures/leaf_thickness.png";

        // Mean absolute RGB difference per pixel, in 0..255 units. A MEAN, not a
        // count: the question here is "how much energy did this term add",
        // which is an amount, where #1233's silhouette question was a count.
        [[nodiscard]] f64 MeanAbsDifference(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
                return 0.0;
            f64 total = 0.0;
            std::size_t pixels = 0;
            for (std::size_t i = 0; i + 3 < a.size(); i += 4)
            {
                total += std::abs(static_cast<int>(a[i + 0]) - static_cast<int>(b[i + 0]));
                total += std::abs(static_cast<int>(a[i + 1]) - static_cast<int>(b[i + 1]));
                total += std::abs(static_cast<int>(a[i + 2]) - static_cast<int>(b[i + 2]));
                ++pixels;
            }
            return pixels == 0 ? 0.0 : total / (3.0 * static_cast<f64>(pixels));
        }

        struct CameraPose
        {
            glm::vec3 Eye;
            f32 Yaw;
            f32 Pitch;
        };

        // The demonstrating view: standing among the plants at eye height,
        // looking along -Z into the clump. The sun is placed BEHIND the plants
        // for the backlit arm, so this pose is the issue's named case.
        constexpr CameraPose kFrontPose{ { 128.0f, 9.0f, 172.0f }, 0.0f, 0.06f };
        // The same clump from the opposite side — the SURFACE-SIDE dimension.
        // The leaves the first pose saw front-on are seen from behind here.
        constexpr CameraPose kBackPose{ { 128.0f, 9.0f, 104.0f }, 3.14159265f, 0.06f };
        // Far enough that the card / impostor band owns the plants.
        constexpr CameraPose kFarPose{ { 128.0f, 42.0f, 290.0f }, 0.0f, 0.22f };
    } // namespace

    class FoliageLeafTransmissionEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = { 128.0f, 60.0f, 128.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                // Backlit by default: the light TRAVELS towards +Z, so it
                // arrives from -Z — behind the plants as seen from kFrontPose.
                dl.m_Direction = glm::normalize(glm::vec3(0.0f, -0.35f, 1.0f));
                dl.m_Color = glm::vec3(1.0f, 0.96f, 0.88f);
                dl.m_Intensity = 4.0f;
                dl.m_CastShadows = true;
                m_SunEntity = light;
            }

            // The occluder for the shadowed-backlit cell. Parked far away and
            // scaled to nothing until the test moves it into place, so it is
            // absent from every other capture rather than being a second
            // variable in all of them.
            {
                Entity cube = scene.CreateEntity("SunOccluder");
                auto& tc = cube.GetComponent<TransformComponent>();
                tc.Translation = { 128.0f, -500.0f, 128.0f };
                tc.Scale = { 1.0f, 1.0f, 1.0f };
                auto& mc = cube.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Cube;
                if (Ref<Mesh> mesh = MeshPrimitives::CreateCube())
                    mc.m_MeshSource = mesh->GetMeshSource();
                auto& mat = cube.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.5f, 0.5f, 0.5f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
                m_OccluderEntity = cube;
            }

            m_TerrainEntity = scene.CreateEntity("Terrain");
            {
                auto& terrain = m_TerrainEntity.AddComponent<TerrainComponent>();
                terrain.m_ProceduralEnabled = true;
                terrain.m_ProceduralSeed = 7;
                terrain.m_ProceduralResolution = 128;
                terrain.m_ProceduralOctaves = 3;
                terrain.m_ProceduralFrequency = 1.2f;
                terrain.m_WorldSizeX = 256.0f;
                terrain.m_WorldSizeZ = 256.0f;
                // Nearly flat: the hillside must not shadow the clump, or the
                // shadowed-backlit cell would stop being a controlled change.
                terrain.m_HeightScale = 3.0f;
                terrain.m_TessellationEnabled = false;
                terrain.m_Material = Ref<TerrainMaterial>::Create();
                for (const auto& layer : TerrainGenerator::MakeDefaultLayers())
                    terrain.m_Material->AddLayer(layer);

                auto& foliage = m_TerrainEntity.AddComponent<FoliageComponent>();
                foliage.m_Enabled = true;

                // THE BACKLIT CLUMP — authored plant meshes (issue #1233's
                // geometry), with the leaf material on.
                {
                    FoliageLayer clump;
                    clump.Name = "BacklitClump";
                    clump.MeshPath = kPineMesh;
                    clump.AlbedoPath = kFoliageAlbedo;
                    clump.Density = 0.03f;
                    clump.SplatmapChannel = -1;
                    clump.MaxSlopeAngle = 70.0f;
                    clump.MinScale = 1.0f;
                    clump.MaxScale = 1.0f;
                    clump.MinHeight = 7.0f;
                    clump.MaxHeight = 10.0f;
                    clump.ViewDistance = 400.0f;
                    clump.FadeStartDistance = 360.0f;
                    clump.UseAuthoredMesh = true;
                    clump.MeshViewDistance = 90.0f;
                    clump.MeshFadeStartDistance = 70.0f;
                    clump.AlphaCutoff = 0.25f;
                    clump.WindStrength = 0.0f; // a deterministic silhouette across the A/B
                    clump.BaseColor = glm::vec3(0.20f, 0.44f, 0.15f);
                    clump.Roughness = 0.75f;
                    // The leaf material, maps and all.
                    clump.NormalMapPath = kLeafNormal;
                    clump.RoughnessMapPath = kLeafRoughness;
                    clump.ThicknessMapPath = kLeafThickness;
                    clump.NormalStrength = 0.8f;
                    clump.TransmissionStrength = 1.0f;
                    clump.TransmissionColor = glm::vec3(0.45f, 0.70f, 0.18f);
                    clump.Thickness = 0.85f;
                    clump.TransmissionDistortion = 0.30f;
                    clump.TransmissionPower = 3.0f;
                    clump.TransmissionWrap = 0.55f;
                    clump.TransmissionAmbient = 0.40f;
                    foliage.m_Layers.push_back(clump);
                }

                // THE SHRUB — flat cards only, a different leaf material, so the
                // evidence covers both geometry representations the material
                // has to mean the same thing on.
                {
                    FoliageLayer shrub;
                    shrub.Name = "Shrub";
                    shrub.AlbedoPath = kFoliageAlbedo;
                    shrub.Density = 0.25f;
                    shrub.SplatmapChannel = -1;
                    shrub.MaxSlopeAngle = 70.0f;
                    shrub.MinScale = 1.4f;
                    shrub.MaxScale = 1.8f;
                    shrub.MinHeight = 1.6f;
                    shrub.MaxHeight = 2.4f;
                    shrub.ViewDistance = 260.0f;
                    shrub.FadeStartDistance = 220.0f;
                    shrub.UseAuthoredMesh = false;
                    shrub.AlphaCutoff = 0.3f;
                    shrub.WindStrength = 0.0f;
                    shrub.BaseColor = glm::vec3(0.26f, 0.50f, 0.16f);
                    shrub.Roughness = 0.85f;
                    shrub.NormalMapPath = kLeafNormal;
                    shrub.RoughnessMapPath = kLeafRoughness;
                    shrub.ThicknessMapPath = kLeafThickness;
                    shrub.NormalStrength = 1.0f;
                    shrub.TransmissionStrength = 0.8f;
                    shrub.TransmissionColor = glm::vec3(0.52f, 0.72f, 0.22f);
                    shrub.Thickness = 0.70f;
                    shrub.TransmissionDistortion = 0.40f;
                    shrub.TransmissionPower = 5.0f;
                    shrub.TransmissionWrap = 0.45f;
                    shrub.TransmissionAmbient = 0.35f;
                    foliage.m_Layers.push_back(shrub);
                }

                foliage.m_NeedsRebuild = true;

                // Remember the authored strengths so the control arm can be the
                // exact same scene with ONLY this value changed.
                for (const auto& l : foliage.m_Layers)
                    m_AuthoredStrengths.push_back(l.TransmissionStrength);
            }
        }

        void SetTransmission(bool on)
        {
            auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
            ASSERT_EQ(foliage.m_Layers.size(), m_AuthoredStrengths.size());
            for (std::size_t i = 0; i < foliage.m_Layers.size(); ++i)
                foliage.m_Layers[i].TransmissionStrength = on ? m_AuthoredStrengths[i] : 0.0f;
            // The layer's leaf material is re-read on rebuild — the maps are
            // cached by path and nothing else here changed, so this is cheap.
            foliage.m_NeedsRebuild = true;
        }

        void SetSunDirection(const glm::vec3& dir)
        {
            m_SunEntity.GetComponent<DirectionalLightComponent>().m_Direction = glm::normalize(dir);
        }

        // Moves the occluder cube between the sun and the clump, or parks it
        // out of the world. A wide, thin slab high above the plants: it must
        // shadow them without appearing in frame, or the pixel difference would
        // include the cube itself.
        void SetOccluder(bool on)
        {
            auto& tc = m_OccluderEntity.GetComponent<TransformComponent>();
            if (on)
            {
                tc.Translation = { 128.0f, 34.0f, 90.0f };
                tc.Scale = { 220.0f, 2.0f, 60.0f };
            }
            else
            {
                tc.Translation = { 128.0f, -500.0f, 128.0f };
                tc.Scale = { 1.0f, 1.0f, 1.0f };
            }
        }

        // Switching the path three times in one test is safe here without a
        // restore of its own: RendererAttachedTest::TearDown already snapshots
        // and restores the process-wide RendererSettings for exactly this case
        // (see its m_SavedRendererSettings). Re-doing it here would be a second
        // owner of the same invariant.
        void SetPath(RenderingPath path)
        {
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::ApplyRendererSettings();
        }

        void Capture(const CameraPose& pose, std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 2000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(pose.Eye, pose.Yaw, pose.Pitch);
            // Several frames: the foliage rebuild, the shadow cascades and the
            // temporal history all need to settle, and a first frame would
            // measure the settling.
            RunEditorFrames(camera, 5);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No SceneColor framebuffer";
            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);
        }

        // One cell: capture the authored arm and the control arm, write both
        // PNGs under names that spell the cell out, and return the mean
        // absolute difference between them.
        f64 MeasureCell(const char* backend, const char* path, const char* angle, const CameraPose& pose)
        {
            std::vector<u8> on;
            std::vector<u8> off;

            SetTransmission(true);
            Capture(pose, on);
            SetTransmission(false);
            Capture(pose, off);

            WritePng(std::string("LeafTransmission_") + backend + "_" + path + "_" + angle + ".png", on);
            WritePng(std::string("LeafTransmissionOff_") + backend + "_" + path + "_" + angle + ".png", off);
            return MeanAbsDifference(on, off);
        }

        static void WritePng(const std::string& name, const std::vector<u8>& px)
        {
            std::vector<u8> flipped(px); // GL readback is bottom-up
            const std::size_t rowBytes = static_cast<std::size_t>(kWidth) * 4u;
            for (u32 y = 0; y < kHeight / 2u; ++y)
            {
                u8* a = flipped.data() + static_cast<std::size_t>(y) * rowBytes;
                u8* b = flipped.data() + static_cast<std::size_t>(kHeight - 1u - y) * rowBytes;
                std::vector<u8> tmp(a, a + rowBytes);
                std::memcpy(a, b, rowBytes);
                std::memcpy(b, tmp.data(), rowBytes);
            }
            // RELATIVE on purpose: renderer init leaves the process CWD in
            // OloEditor/, so an absolute-looking "OloEditor/assets/..." would
            // land in OloEditor/OloEditor/ and the test would still pass while
            // writing nothing a reviewer can find.
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ::stbi_write_png((dir / name).string().c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                             flipped.data(), static_cast<int>(kWidth) * 4);
        }

        Entity m_TerrainEntity;
        Entity m_SunEntity;
        Entity m_OccluderEntity;
        std::vector<f32> m_AuthoredStrengths;
    };

    TEST_F(FoliageLeafTransmissionEvidenceTest, BacklitLeavesTransmitShadowedOnesDoNotAndEveryPathAgrees)
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

        // ── The renderer's own noise floor ──────────────────────────────────
        // The SAME arm, the same pose, twice. Everything measured below has to
        // clear this, and it is measured rather than assumed because a foliage
        // frame carries a stochastic LOD dither and a temporal history.
        SetPath(RenderingPath::Deferred);
        SetTransmission(true);
        std::vector<u8> repeatA;
        std::vector<u8> repeatB;
        Capture(kFrontPose, repeatA);
        Capture(kFrontPose, repeatB);
        const f64 noiseFloor = MeanAbsDifference(repeatA, repeatB);

        // ── The camera-independent gate ─────────────────────────────────────
        // AFTER the first capture, not before, and that ordering is a fact
        // about the engine rather than a style choice: FoliageComponent's
        // m_Renderer is constructed by the first Scene tick that sees the
        // component, so a gate placed before any frame runs asserts on a null
        // renderer and reports a broken fixture for a scene that is fine.
        //
        // What it buys: a zero pixel difference below would otherwise be
        // ambiguous between "the shading is wrong" and "the material never left
        // the component". This separates the two before any pixel is looked at.
        ASSERT_TRUE(m_TerrainEntity && m_TerrainEntity.HasComponent<FoliageComponent>());
        auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
        ASSERT_TRUE(foliage.m_Renderer) << "the foliage renderer was never constructed, even after a frame ran";
        ASSERT_GT(foliage.m_Renderer->GetTotalInstanceCount(), 0u)
            << "nothing was scattered — the fixture is broken and every measurement below is of an empty frame";

        const auto draws = foliage.m_Renderer->GetActiveLayerDrawInfo();
        ASSERT_FALSE(draws.empty());
        bool anyLeafDraw = false;
        for (const auto& d : draws)
        {
            if (d.LeafTransmissionStrength > 0.0f)
            {
                anyLeafDraw = true;
                EXPECT_GT(d.LeafThickness, 0.0f) << "a leaf draw carries zero thickness and cannot transmit";
                EXPECT_GE(d.LeafTransmissionPower, 1.0f);
                EXPECT_TRUE(d.LeafThicknessTextureID.IsValid())
                    << "the authored thickness MAP did not reach the draw — check OloEngine.log for the "
                       "FoliageRenderer load diagnostic; the layer would shade at its constant instead";
                EXPECT_TRUE(d.LeafNormalTextureID.IsValid()) << "the authored normal map did not reach the draw";
            }
        }
        EXPECT_TRUE(anyLeafDraw) << "no draw carries a leaf material — the layer's authored transmission did not "
                                    "survive FoliageRenderer, so the pixels below cannot show it";

        // A floor with a little headroom: a measurement barely above the noise
        // is not evidence of anything.
        const f64 minimumSignal = std::max(noiseFloor * 4.0, 0.25);

        // ── Lighting-direction dimension, on the deferred path ──────────────
        SetSunDirection({ 0.0f, -0.35f, 1.0f }); // from behind the plants
        const f64 backlit = MeasureCell("GL", "Deferred", "Backlit", kFrontPose);

        SetSunDirection({ 0.0f, -0.35f, -1.0f }); // from the camera's side
        const f64 frontLit = MeasureCell("GL", "Deferred", "FrontLit", kFrontPose);

        SetSunDirection({ 1.0f, -0.35f, 0.0f }); // across
        const f64 sideLit = MeasureCell("GL", "Deferred", "SideLit", kFrontPose);

        // The one that proves the term is not an unshadowed constant.
        SetSunDirection({ 0.0f, -0.35f, 1.0f });
        SetOccluder(true);
        const f64 shadowedBacklit = MeasureCell("GL", "Deferred", "ShadowedBacklit", kFrontPose);
        SetOccluder(false);

        // ── Surface-side and distance dimensions ────────────────────────────
        // THE SUN IS FLIPPED WITH THE CAMERA, and that is the whole point of
        // this cell. kBackPose views the same plants from the opposite side, so
        // leaving the sun where it was would make them FRONT-lit and the cell
        // would measure the front-lit case a second time under a name that says
        // otherwise. Backlighting is a relationship between the light and the
        // VIEWER, so the demonstrating configuration from the far side needs
        // the light on the far side too — and the leaves the first pose saw
        // front-on are then seen from their OTHER FACE, still transmitting.
        // That is what "both sides respond" means, and it is the pixel
        // counterpart of the two-sided normal rule FoliageLeafTransmissionTest
        // pins exactly.
        SetSunDirection({ 0.0f, -0.35f, -1.0f });
        const f64 backFace = MeasureCell("GL", "Deferred", "BackFace", kBackPose);

        SetSunDirection({ 0.0f, -0.35f, 1.0f });
        // NOT `far`: <windows.h> still #defines `near` and `far` as empty
        // macros for 16-bit compatibility, and a local named either one expands
        // to nothing and fails with "expected unqualified-id" a hundred lines
        // from anything that looks relevant.
        const f64 farField = MeasureCell("GL", "Deferred", "Far", kFarPose);

        // ── The path dimension ──────────────────────────────────────────────
        SetPath(RenderingPath::Forward);
        const f64 forwardBacklit = MeasureCell("GL", "Forward", "Backlit", kFrontPose);
        SetPath(RenderingPath::ForwardPlus);
        const f64 forwardPlusBacklit = MeasureCell("GL", "ForwardPlus", "Backlit", kFrontPose);
        SetPath(RenderingPath::Deferred);

        // =====================================================================
        // The claims.
        // =====================================================================

        EXPECT_GT(backlit, minimumSignal)
            << "A backlit canopy renders IDENTICALLY with the leaf material on and off (mean abs difference "
            << backlit << " against a measured noise floor of " << noiseFloor
            << "). The transmission term is not reaching the screen at all — check OloEngine.log for shader "
               "errors, and FoliageLeafTransmissionTest for whether the lobe itself is alive.";

        EXPECT_LT(frontLit, backlit * 0.6)
            << "A FRONT-lit canopy gained as much from the leaf material as a backlit one (" << frontLit
            << " vs " << backlit
            << "). This is supposed to be a forward-scattering lobe; a term that pays out equally from both "
               "sides is an ambient fill, and the issue's demonstrating case would be indistinguishable from "
               "its control.";

        EXPECT_LT(shadowedBacklit, backlit * 0.5)
            << "TRANSMISSION SURVIVED AN OCCLUDER BETWEEN THE SUN AND THE PLANTS (" << shadowedBacklit
            << " shadowed vs " << backlit
            << " unshadowed). This is #1234's second acceptance criterion failing in exactly the way it names: "
               "the term is behaving as an unshadowed ambient constant. Check that the shadow factor still "
               "multiplies the DIRECT half in DeferredLightingShared.glsl, and that oloFoliageShadowNormal is "
               "biasing the lookup along the lit-side normal.";

        EXPECT_GT(backFace, minimumSignal)
            << "Seen from the other side, the leaves stopped transmitting (" << backFace
            << "). Both sides must respond — the two-sided normal rule is the mechanism, and "
               "FoliageLeafTransmissionTest pins it directly if this fails.";

        // Side-lit and far are captured for the reviewer and asserted only for
        // finiteness and non-negativity: neither has a value a threshold could
        // be honestly pinned to. Side-lit sits between the two bounds above by
        // construction, and the far field is dominated by the card/impostor
        // hand-over, whose magnitude is a property of the LOD band rather than
        // of this material.
        EXPECT_TRUE(std::isfinite(sideLit) && sideLit >= 0.0);
        EXPECT_TRUE(std::isfinite(farField) && farField >= 0.0);

        // ── Every path carries the term ─────────────────────────────────────
        EXPECT_GT(forwardBacklit, minimumSignal)
            << "The FORWARD path renders a backlit canopy identically with the material on and off ("
            << forwardBacklit << "). Forward and deferred must preserve the same material meaning (#1234's "
                                 "third criterion) and forward has dropped it.";
        EXPECT_GT(forwardPlusBacklit, minimumSignal)
            << "The FORWARD+ path renders a backlit canopy identically with the material on and off ("
            << forwardPlusBacklit << ").";

        const f64 lo = std::min({ backlit, forwardBacklit, forwardPlusBacklit });
        const f64 hi = std::max({ backlit, forwardBacklit, forwardPlusBacklit });
        ASSERT_GT(lo, 0.0);
        // A LOOSE band, deliberately, and the looseness is a measurement
        // property rather than slack: the INDIRECT half of the term reads the
        // ambient environment, and the three paths reach the environment
        // through different ladders (deferred has probes and baked GI rungs the
        // forward foliage shader does not). A tight bound here would be a
        // pinned coincidence that the next ambient change breaks. What 3x still
        // catches is the failure that actually happens — a path evaluating the
        // term at the wrong order of magnitude because its lobe parameters
        // never arrived, or reaching it through a different code path that
        // decays separately.
        EXPECT_LT(hi / lo, 3.0)
            << "The three render paths disagree about HOW MUCH a backlit leaf transmits by more than 3x "
               "(deferred "
            << backlit << ", forward " << forwardBacklit << ", forward+ "
            << forwardPlusBacklit
            << "). They share one evaluation (include/FoliageSurface.glsl) and one set of authored parameters, "
               "so a gap this size means one of them is reading different parameters — check the leaf-profile "
               "slot the G-Buffer carries against the FoliageUBO lanes the forward path reads.";

        // Forward and forward+ run the SAME foliage program (see
        // SelectFoliageRenderStream), so these two are not an approximation of
        // each other — they are the same shader over the same light set, and a
        // real gap means the forward+ path changed something about the foliage
        // draw itself.
        EXPECT_LT(std::abs(forwardBacklit - forwardPlusBacklit), std::max(forwardBacklit * 0.25, 0.5))
            << "Forward and Forward+ disagree (" << forwardBacklit << " vs " << forwardPlusBacklit
            << ") although foliage runs the SAME program on both — SelectFoliageRenderStream routes them to "
               "one shader over one light set, so this gap is not a lighting-model difference.";
    }

} // namespace OloEngine::Tests

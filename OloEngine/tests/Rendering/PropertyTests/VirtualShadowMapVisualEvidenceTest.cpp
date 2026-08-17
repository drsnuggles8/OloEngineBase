// OLO_TEST_LAYER: L8
// =============================================================================
// VirtualShadowMapVisualEvidenceTest.cpp
//
// Visual evidence (PNG) + a driver-independent contract for Virtual Shadow Maps
// (issue #702): does the sparse page table actually cast a directional shadow,
// and does it land where the CSM it replaces puts it?
//
// WHY THIS EXISTS AS A TEST RATHER THAN A SCREENSHOT SESSION. Verifying VSM by
// opening a sandbox scene failed three times in a row, and never because of VSM:
// one scene disables directional shadows on purpose, another never submits its
// props as casters (its cascade contains only the ground plane), and a third
// puts the sun overhead so every object hides its own shadow. A scene this test
// BUILDS is a scene whose caster set, sun angle and floor are known — which is
// the whole point of single-mesh-visual-test-lighting.md's "add a ground plane,
// then look at the PNG".
//
// The scene is a neutral grey floor with a cube resting on it under a grazing
// sun whose cascaded shadow is ENABLED. Rendered twice through the full
// Renderer3D pipeline from the same pose — once on CSM, once on VSM — and both
// frames are written to
//   OloEditor/assets/tests/visual/VirtualShadowMap_<technique>_<pose>.png
//
// The contract is GOLDEN-FREE and differential, so it survives a GPU change and
// needs no committed reference image:
//
//   1. CSM darkens the floor on the cube's lee side  (the baseline is real)
//   2. VSM darkens it too                            (VSM casts at all)
//   3. both darkenings have the same CENTROID        (VSM agrees with CSM about
//      WHERE the shadow is — this is what catches a wrong clip level, a wrong
//      page wrap, or a flipped raster origin, each of which still produces "a
//      shadow", just not in the right place)
//   4. both carry a comparable amount of darkening   (same footprint, not a
//      correctly-placed fragment of one — the centroid alone cannot tell those
//      apart, since a dot inside the real footprint has the right centroid)
//   5. the lit control band stays bright under both  (neither dims globally)
//
// (3) is the load-bearing one, with (4) closing its blind spot. (1) and (2) alone
// would pass for a system that shadows the entire floor.
//
// Position is measured as a darkness-weighted CENTROID, not as the darkest cell.
// The darkest-cell version of this test reported the two techniques as seven
// cells apart on frames that were, to the eye, identical: the cast shadow fills
// most of the lee band, so "darkest cell" was an argmin over a nearly flat field.
// A metric whose answer is decided by the noise in a flat region is worse than no
// metric, because it fails loudly and points somewhere real.
//
// Runs in the normal suite and SKIPs (not fails) when no GL 4.6 context exists,
// matching ContactShadowVisualEvidenceTest. The page-table MATHS contracts (wrap
// predicate, clip-level agreement, per-level depth equality) are CPU-only and
// live in Rendering/VirtualShadowMapTest.cpp.
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
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
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
        constexpr f32 kCaptureTime = 4.0f; // freeze the clock for deterministic frames

        // VSM consumes pages the PREVIOUS frame marked (the marking pass needs the
        // scene depth buffer, which does not exist when the shadow pass runs), so a
        // capture after one frame would photograph an empty page table. Several
        // frames also let the allocator settle and the caching claim take effect.
        constexpr u32 kSettleFrames = 6;

        struct BandStats
        {
            f64 R = 0.0;
            f64 G = 0.0;
            f64 B = 0.0;

            [[nodiscard]] f64 Luma() const
            {
                return 0.2126 * R + 0.7152 * G + 0.0722 * B;
            }
        };

        // Mean RGB over a rectangular band (UV fractions), rows top-down.
        [[nodiscard]] BandStats SampleBand(const std::vector<u8>& px, f32 x0, f32 x1, f32 y0, f32 y1)
        {
            const u32 ix0 = static_cast<u32>(x0 * kWidth);
            const u32 ix1 = static_cast<u32>(x1 * kWidth);
            const u32 iy0 = static_cast<u32>(y0 * kHeight);
            const u32 iy1 = static_cast<u32>(y1 * kHeight);
            u64 sumR = 0, sumG = 0, sumB = 0, count = 0;
            for (u32 y = iy0; y < iy1; ++y)
            {
                for (u32 x = ix0; x < ix1; ++x)
                {
                    const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                    if (idx + 2 >= px.size())
                        continue;
                    sumR += px[idx + 0];
                    sumG += px[idx + 1];
                    sumB += px[idx + 2];
                    ++count;
                }
            }
            if (count == 0)
                return {};
            return { static_cast<f64>(sumR) / count, static_cast<f64>(sumG) / count,
                     static_cast<f64>(sumB) / count };
        }

        // WHERE the shadow is and HOW MUCH of it there is, as a first moment of
        // "darkness" over a scanned region.
        //
        // This replaced a darkest-CELL scan, and the reason is worth keeping: the
        // cast shadow fills most of the lee band, so the darkest cell was an argmin
        // over a nearly flat field and landed wherever the grid texture happened to
        // put a pixel. It reported CSM and VSM as seven cells apart while the two
        // frames were, to the eye, the same shadow in the same place. A centroid
        // has no such failure — every shadowed pixel votes, so it moves only when
        // the shadow actually moves, which is precisely the regression the test
        // exists to catch (a wrong clip level, a wrong page wrap or a flipped
        // raster origin all displace the whole footprint).
        //
        // Weighting by (lit - luma) rather than counting thresholded pixels keeps
        // it continuous: a penumbra pixel contributes in proportion to how dark it
        // is, so the two techniques' different edge softness shifts the centroid by
        // a fraction of the penumbra rather than by a whole cell.
        struct DarkMoment
        {
            f64 Mass = 0.0; // summed darkening — proportional to shadow area x depth
            f64 Cx = 0.0;   // centroid, in frame UV
            f64 Cy = 0.0;
            bool Found = false;
        };

        // Only pixels at least this much darker than the lit control band count.
        // Grid lines are BRIGHTER than the floor so they weigh nothing either way;
        // this rejects shading gradient and readback noise.
        constexpr f64 kDarkNoiseFloor = 8.0;

        [[nodiscard]] DarkMoment DarknessCentroid(const std::vector<u8>& px, f32 x0, f32 x1, f32 y0,
                                                  f32 y1, f64 litLuma)
        {
            const u32 ix0 = static_cast<u32>(x0 * kWidth);
            const u32 ix1 = std::min(static_cast<u32>(x1 * kWidth), kWidth);
            const u32 iy0 = static_cast<u32>(y0 * kHeight);
            const u32 iy1 = std::min(static_cast<u32>(y1 * kHeight), kHeight);

            f64 mass = 0.0;
            f64 sumX = 0.0;
            f64 sumY = 0.0;
            for (u32 y = iy0; y < iy1; ++y)
            {
                for (u32 x = ix0; x < ix1; ++x)
                {
                    const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                    if (idx + 2 >= px.size())
                        continue;
                    const f64 luma = 0.2126 * px[idx + 0] + 0.7152 * px[idx + 1] + 0.0722 * px[idx + 2];
                    const f64 weight = litLuma - luma;
                    if (weight <= kDarkNoiseFloor)
                        continue;
                    mass += weight;
                    sumX += weight * (static_cast<f64>(x) / kWidth);
                    sumY += weight * (static_cast<f64>(y) / kHeight);
                }
            }
            if (mass <= 0.0)
                return {};
            return { mass, sumX / mass, sumY / mass, true };
        }
    } // namespace

    class VirtualShadowMapVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        // SetVirtualShadowMaps mutates the PROCESS-WIDE shadow settings, and each
        // test body restores CSM only on its success path — an ASSERT_* firing
        // inside Capture leaves VSM enabled for every later test in the process,
        // which is exactly the cross-test bleed GLStateGuard exists to stop for
        // GL state. Restore unconditionally; the call is idempotent when the
        // body already did it.
        void TearDown() override
        {
            (void)SetVirtualShadowMaps(false);
            RendererAttachedTest::TearDown();
        }

        void BuildScene() override
        {
            Scene& scene = GetScene();

            EnableRendering(kWidth, kHeight);

            // Deferred: the lit path VSM's sampling is wired into first, and the
            // one the editor defaults to.
            Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
            Renderer3D::ApplyRendererSettings();

            // A GRAZING sun, and that is deliberate. An overhead sun puts each
            // object's shadow directly beneath it, where the object itself hides
            // it from every camera that can also see the object — which is exactly
            // how three sandbox scenes looked "shadowless" while their cascades
            // held real depth. Travelling toward +X throws the cube's shadow onto
            // open floor on the screen-right side.
            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 20.0f, 0.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(0.7f, -0.45f, 0.0f));
                dl.m_Color = glm::vec3(1.0f, 1.0f, 1.0f);
                dl.m_Intensity = 1.5f;
                dl.m_CastShadows = true; // the whole point — both techniques need it on
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

            // Neutral grey floor at y = 0 — the shadow RECEIVER. Without it the
            // cast shadow has nothing to land on and the frame looks identical
            // whichever technique is running (single-mesh-visual-test-lighting.md).
            {
                Entity floor = addMesh("GreyFloor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f },
                                       { 80.0f, 1.0f, 80.0f });
                auto& mat = floor.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.7f, 0.7f, 0.7f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
            }

            // The caster: a cube resting on the floor (a unit cube spans -0.5..0.5,
            // so a scale-5 cube centred at y=2.5 has its base exactly on y=0).
            // A static MeshComponent is precisely the caster type VSM covers.
            {
                Entity cube = addMesh("Occluder", MeshPrimitive::Cube, { 0.0f, 2.5f, 0.0f },
                                      { 5.0f, 5.0f, 5.0f });
                auto& mat = cube.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.6f, 0.6f, 0.6f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
            }
        }

        // Switch the directional technique. Returns false when VSM refused to come
        // up (wrong backend, shader load failure) — VirtualShadowMap::Init clears
        // its own Enabled flag in that case, so asking the shadow map is the only
        // honest way to know whether the frame that follows is really VSM.
        [[nodiscard]] static bool SetVirtualShadowMaps(bool enabled, i32 debugMode = 0)
        {
            auto& shadowMap = Renderer3D::GetShadowMap();
            ShadowSettings settings = shadowMap.GetSettings();
            settings.VSM.Enabled = enabled;
            settings.VSM.DebugMode = debugMode;
            shadowMap.SetSettings(settings);
            return shadowMap.IsVirtualShadowMapActive() == enabled;
        }

        void Capture(const std::string& tag, const glm::vec3& position, f32 yaw, f32 pitch,
                     std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, yaw, pitch);

            RunEditorFrames(camera, kSettleFrames);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for VSM capture '" << tag << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            // GL readback is bottom-up; flip so row 0 is the top of the frame.
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
            ASSERT_FALSE(ec) << "Failed to create evidence dir '" << dir.generic_string()
                             << "': " << ec.message();

            // Stamped next to every capture. When the comparison below fails, the
            // first question is always "was the page table even populated for
            // THIS frame sequence" — and re-running to find out costs a full GL
            // fixture spin-up, so it is recorded unconditionally.
            {
                const auto& shadowMap = Renderer3D::GetShadowMap();
                const VSM::Statistics s = shadowMap.GetVirtualShadowMap().GetStatistics();
                GTEST_LOG_(INFO) << "[" << tag << "] vsmActive=" << shadowMap.IsVirtualShadowMapActive()
                                 << " resident=" << s.PagesResident << " drawn=" << s.PagesDrawn
                                 << " requested=" << s.PagesRequested << " allocated=" << s.PagesAllocated
                                 << " failed=" << s.PagesFailed << " drawInstances=" << s.DrawInstances;
            }

            const std::string path = (dir / ("VirtualShadowMap_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth),
                                               static_cast<int>(kHeight), 4, outPixels.data(),
                                               static_cast<int>(kWidth) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";
        }
    };

    // CSM vs VSM on the same scene, same pose, same light: both must cast, and
    // both must cast in the SAME PLACE. SKIPs without a GL 4.6 context.
    TEST_F(VirtualShadowMapVisualEvidenceTest, VirtualShadowMapCastsTheSameFloorShadowAsCSM)
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

        // The cube stands at the origin (base y=0, top y=5), screen-centred, looked
        // at down -Z. The sun travels +X, so the cast shadow falls on the +X
        // (screen-RIGHT) floor and the -X (screen-LEFT) floor stays fully lit. Two
        // poses so a view-dependent regression is caught.
        const std::array<Pose, 2> poses = { {
            { "Angled", { 0.0f, 9.0f, 16.0f }, 0.0f, 0.55f },
            { "Higher", { 0.0f, 11.0f, 15.0f }, 0.0f, 0.7f },
        } };

        // Lee = screen-right of the cube (shadow side); Lit = mirrored control.
        // The lee window starts just clear of the cube's right edge (~0.62) so the
        // cube's own unlit faces cannot contribute to the centroid, and runs to the
        // frame edge so the whole cast footprint is inside it — a window that
        // clipped the shadow would move the centroid by however much it clipped.
        constexpr f32 kLeeX0 = 0.635f, kLeeX1 = 0.99f;
        constexpr f32 kLitX0 = 0.12f, kLitX1 = 0.38f;
        constexpr f32 kScanY0 = 0.32f, kScanY1 = 0.78f;

        for (const Pose& pose : poses)
        {
            SCOPED_TRACE(pose.Name);

            // ---- CSM baseline -------------------------------------------------
            ASSERT_TRUE(SetVirtualShadowMaps(false)) << "could not return to the CSM path";
            std::vector<u8> csm;
            Capture(std::string("CSM_") + pose.Name, pose.Position, pose.Yaw, pose.Pitch, csm);
            if (::testing::Test::HasFatalFailure())
                return;

            // ---- VSM ----------------------------------------------------------
            if (!SetVirtualShadowMaps(true))
            {
                GTEST_SKIP() << "Virtual Shadow Maps refused to initialise on this backend/driver "
                                "(VirtualShadowMap::Init clears its own Enabled flag and falls back "
                                "to CSM) — nothing to compare.";
            }
            std::vector<u8> vsm;
            Capture(std::string("VSM_") + pose.Name, pose.Position, pose.Yaw, pose.Pitch, vsm);
            if (::testing::Test::HasFatalFailure())
                return;

            // DebugMode 7 renders the shadow factor the lit pass RECEIVES. It is
            // captured unconditionally, right here, because the two ways this
            // test fails need opposite fixes and the beauty frame cannot tell
            // them apart: a shadow visible here but not above means the term is
            // computed correctly and then dropped by the caller; no shadow here
            // means the page table or the raster is at fault. DebugMode does not
            // recreate any VSM resource (VirtualShadowMap::SetSettings only
            // recreates on Enabled/resolution), so this frame sees exactly the
            // page table the beauty frame saw.
            ASSERT_TRUE(SetVirtualShadowMaps(true, 7));
            std::vector<u8> factor;
            Capture(std::string("VSM_") + pose.Name + "_ShadowFactor", pose.Position, pose.Yaw, pose.Pitch,
                    factor);
            const bool factorFatal = ::testing::Test::HasFatalFailure();
            ASSERT_TRUE(SetVirtualShadowMaps(false)) << "could not restore the CSM path";
            if (factorFatal)
                return;

            const BandStats csmLit = SampleBand(csm, kLitX0, kLitX1, kScanY0, kScanY1);
            const BandStats vsmLit = SampleBand(vsm, kLitX0, kLitX1, kScanY0, kScanY1);

            // The floor must actually be lit somewhere, or every comparison below
            // is between two blacks and passes vacuously. Asserted before the
            // moments are taken, because the lit luma is their reference level.
            ASSERT_GT(csmLit.Luma(), 25.0) << "the CSM control band is not lit — the scene is too dark "
                                              "to draw any conclusion from";
            ASSERT_GT(vsmLit.Luma(), 25.0) << "the VSM control band is not lit";

            const DarkMoment csmDark = DarknessCentroid(csm, kLeeX0, kLeeX1, kScanY0, kScanY1, csmLit.Luma());
            const DarkMoment vsmDark = DarknessCentroid(vsm, kLeeX0, kLeeX1, kScanY0, kScanY1, vsmLit.Luma());

            GTEST_LOG_(INFO) << "lee darkness — CSM mass=" << csmDark.Mass << " at (" << csmDark.Cx << ", "
                             << csmDark.Cy << ")  VSM mass=" << vsmDark.Mass << " at (" << vsmDark.Cx
                             << ", " << vsmDark.Cy << ")";

            // 1. CSM casts a shadow at all (the baseline is real, not another
            //    shadowless scene).
            ASSERT_TRUE(csmDark.Found)
                << "CSM did not darken the lee floor at all (lit band " << csmLit.Luma()
                << ") — the baseline casts nothing, so this test cannot say anything about VSM";

            // 2. VSM casts a shadow at all.
            EXPECT_TRUE(vsmDark.Found)
                << "VSM did not darken the lee floor at all (lit band " << vsmLit.Luma()
                << ") — every lee pixel is within " << kDarkNoiseFloor << " luma of the lit control";
            if (!vsmDark.Found)
                continue;

            // 3. THE LOAD-BEARING ONE: both put it in the same place. A wrong clip
            //    level, a wrong page wrap or a flipped raster origin all still
            //    produce "a shadow" — just not here. The tolerance is 2.5% of the
            //    frame, which is a few pixels wider than the two techniques'
            //    penumbra difference and far tighter than any of those faults.
            constexpr f64 kCentroidTolerance = 0.025;
            EXPECT_NEAR(vsmDark.Cx, csmDark.Cx, kCentroidTolerance)
                << "VSM's cast shadow sits " << std::abs(vsmDark.Cx - csmDark.Cx)
                << " of the frame width from CSM's — it exists but is in the wrong place";
            EXPECT_NEAR(vsmDark.Cy, csmDark.Cy, kCentroidTolerance)
                << "VSM's cast shadow sits " << std::abs(vsmDark.Cy - csmDark.Cy)
                << " of the frame height from CSM's — it exists but is in the wrong place";

            // 4. …and it is the same SIZE, to within the sharpness difference the
            //    two techniques legitimately have. Without this, a VSM that
            //    shadowed a single correct pixel would satisfy (2) and (3): the
            //    centroid of a dot inside the real footprint is still in the right
            //    place. The band is generous on purpose — VSM's finer clip levels
            //    genuinely peter-pan less than CSM's cascade, so its footprint runs
            //    slightly larger, and pinning this tighter would fail on a real
            //    improvement.
            EXPECT_GT(vsmDark.Mass, csmDark.Mass * 0.5)
                << "VSM's shadow carries only " << (vsmDark.Mass / csmDark.Mass)
                << "x CSM's darkening — it is casting a fragment of the footprint, not the footprint";
            EXPECT_LT(vsmDark.Mass, csmDark.Mass * 2.0)
                << "VSM's shadow carries " << (vsmDark.Mass / csmDark.Mass)
                << "x CSM's darkening — it is over-shadowing (acne, or a bias that lost its sign)";

            // 5. Neither technique globally dims the frame: the lit control band
            //    must agree between them.
            EXPECT_NEAR(vsmLit.Luma(), csmLit.Luma(), 18.0)
                << "the lit control band changed between CSM and VSM — one of them is dimming the "
                   "whole frame rather than casting a localised shadow";
        }
    }

    // Is the lit pass running the VSM sampling code AT ALL?
    //
    // This separates the two failure modes that look identical on screen — "VSM
    // is sampling but finds no resident page" and "the lit shader never enters
    // the VSM branch" — and they need completely different fixes. The debug tint
    // lives inside that branch, so a frame it does not change is a branch that
    // did not run, whatever the CPU-side IsActive() says.
    TEST_F(VirtualShadowMapVisualEvidenceTest, LitPassActuallyEntersTheVirtualShadowMapBranch)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        if (!SetVirtualShadowMaps(true))
        {
            GTEST_SKIP() << "Virtual Shadow Maps refused to initialise on this backend/driver.";
        }

        const glm::vec3 pos{ 0.0f, 9.0f, 16.0f };
        std::vector<u8> plain;
        Capture("Probe_Plain", pos, 0.0f, 0.55f, plain);
        if (::testing::Test::HasFatalFailure())
            return;

        // DebugMode 3 = residency (green resident, red requested-but-unbacked).
        ASSERT_TRUE(SetVirtualShadowMaps(true, 3));
        std::vector<u8> tinted;
        Capture("Probe_Residency", pos, 0.0f, 0.55f, tinted);
        const bool fatal = ::testing::Test::HasFatalFailure();
        ASSERT_TRUE(SetVirtualShadowMaps(false)) << "could not restore the CSM path";
        if (fatal)
            return;

        ASSERT_EQ(plain.size(), tinted.size());
        std::size_t differing = 0;
        for (std::size_t i = 0; i < plain.size(); i += 4)
        {
            if (plain[i] != tinted[i] || plain[i + 1] != tinted[i + 1] || plain[i + 2] != tinted[i + 2])
                ++differing;
        }
        const f64 fraction = static_cast<f64>(differing) / static_cast<f64>(plain.size() / 4);
        GTEST_LOG_(INFO) << "VSM debug tint changed " << differing << " pixels (" << (fraction * 100.0)
                         << "% of the frame)";

        EXPECT_GT(fraction, 0.25)
            << "turning on the VSM residency debug view changed " << (fraction * 100.0)
            << "% of the frame — the lit pass is NOT entering the VSM branch, so no amount of "
               "page-table debugging will help until the globals block (VSM_ENABLED) reaches the "
               "shader";
    }

    // Acceptance criterion #2: a static scene with a static camera must settle to
    // redrawing (almost) no pages. This is the caching claim, and it is the one
    // property that separates VSM from "a very elaborate way to draw 16 cascades".
    TEST_F(VirtualShadowMapVisualEvidenceTest, StaticSceneSettlesToRedrawingAlmostNoPages)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        if (!SetVirtualShadowMaps(true))
        {
            GTEST_SKIP() << "Virtual Shadow Maps refused to initialise on this backend/driver.";
        }

        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        camera.SetPose({ 0.0f, 9.0f, 16.0f }, 0.0f, 0.55f);

        // Warm up ONE FRAME AT A TIME, recording the peak. "Never drawn" and
        // "drawn once then cached" both settle to PagesDrawn == 0, so a test that
        // only samples the end state passes just as happily when the raster is
        // completely broken — which is exactly the state this suite found VSM in.
        // The peak is what separates them.
        const auto& shadowMap = Renderer3D::GetShadowMap();
        u32 peakDrawn = 0;
        u32 peakResident = 0;
        u32 peakDrawInstances = 0;
        for (u32 i = 0; i < 10; ++i)
        {
            RunEditorFrames(camera, 1);
            const VSM::Statistics s = shadowMap.GetVirtualShadowMap().GetStatistics();
            peakDrawn = std::max(peakDrawn, s.PagesDrawn);
            peakResident = std::max(peakResident, s.PagesResident);
            peakDrawInstances = std::max(peakDrawInstances, s.DrawInstances);
        }
        const u32 residentAfterWarmup = peakResident;

        // Then hold everything still and watch what it costs.
        RunEditorFrames(camera, 6);
        const VSM::Statistics settled = shadowMap.GetVirtualShadowMap().GetStatistics();

        // …and did it write anything? Read the pool BEFORE switching VSM off:
        // SetSettings(false) tears the physical pool down, and the readback then
        // fails on a dead handle rather than telling you anything.
        //
        // …and did it write anything? PagesDrawn counts pages the CLEAR kernel
        // reset, which happens whether or not a single triangle then lands on
        // them. So read the pool itself: a page that was cleared and never
        // rasterized still holds the far sentinel, and a pool that is ENTIRELY
        // sentinel means the raster is running and writing nothing — which
        // presents identically to "VSM is off" and is the single most expensive
        // thing to misdiagnose in this system.
        {
            const auto& vsm = shadowMap.GetVirtualShadowMap();
            const u32 res = vsm.GetPhysicalResolution();
            std::vector<u32> pool(static_cast<std::size_t>(res) * res, 0u);
            const bool read = RenderCommand::ReadTextureImage(vsm.GetPhysicalPoolHandle(), 0,
                                                              RHI::Format::R32UInt,
                                                              pool.size() * sizeof(u32), pool.data());
            ASSERT_TRUE(read) << "could not read back the VSM physical pool";

            std::size_t written = 0;
            for (const u32 texel : pool)
            {
                if (texel != 0xFFFFFFFFu)
                    ++written;
            }
            // Which PAGES got geometry, not just how many texels. A raster that
            // works but only ran for a few clip levels fills whole pages; one
            // whose viewport or page indirection is wrong fills a corner of many.
            // The two need different fixes and the texel count cannot tell them
            // apart.
            const u32 pageRes = res / VSM::kPageSize;
            std::vector<u32> perPage(static_cast<std::size_t>(pageRes) * pageRes, 0u);
            for (u32 y = 0; y < res; ++y)
            {
                for (u32 x = 0; x < res; ++x)
                {
                    if (pool[static_cast<std::size_t>(y) * res + x] != 0xFFFFFFFFu)
                        ++perPage[static_cast<std::size_t>(y / VSM::kPageSize) * pageRes + (x / VSM::kPageSize)];
                }
            }
            u32 pagesTouched = 0, pagesFull = 0;
            constexpr u32 kTexelsPerPage = VSM::kPageSize * VSM::kPageSize;
            for (const u32 c : perPage)
            {
                if (c > 0)
                    ++pagesTouched;
                if (c > (kTexelsPerPage * 9u) / 10u)
                    ++pagesFull;
            }
            GTEST_LOG_(INFO) << "VSM physical pool: " << written << " / " << pool.size()
                             << " texels hold depth (rest are the far sentinel); "
                             << pagesTouched << " pages touched, " << pagesFull << " of them >90% filled";
            EXPECT_GT(written, 0u)
                << "every texel of the physical pool is still the far sentinel — pages are being "
                   "allocated and cleared, but the depth raster wrote nothing into them";
        }

        ASSERT_TRUE(SetVirtualShadowMaps(false)) << "could not restore the CSM path";

        // Reported unconditionally: when one of the assertions below fails these
        // four numbers say WHICH stage broke, and re-running to find out costs a
        // full GL fixture spin-up.
        GTEST_LOG_(INFO) << "VSM warmup peak: resident=" << peakResident << " drawn=" << peakDrawn
                         << " drawInstances=" << peakDrawInstances
                         << " | settled: resident=" << settled.PagesResident
                         << " drawn=" << settled.PagesDrawn << " requested=" << settled.PagesRequested
                         << " allocated=" << settled.PagesAllocated << " failed=" << settled.PagesFailed
                         << " freed=" << settled.PagesFreed << " drawInstances=" << settled.DrawInstances;

        // Residency: did the allocator ever back anything?
        EXPECT_GT(residentAfterWarmup, 0u)
            << "no pages were ever resident — the allocator never ran, so 'drew nothing' proves "
               "nothing about caching";

        // Did the RASTER ever run? Without this the caching assertion below is
        // satisfied by a system that draws nothing at all, forever.
        EXPECT_GT(peakDrawn, 0u)
            << "no page was EVER redrawn across warmup — the page table is being allocated but the "
               "depth raster is not filling it, so every resident page still reads 'far' and the "
               "sampler reports everything lit";

        // The claim itself. A handful of redraws is fine (the allocator may still
        // be settling an edge page); redrawing a large fraction of the resident
        // set every frame means the cache is not caching — the likeliest cause is
        // the VISITED bit being cleared on the wrong side of the marking pass
        // (see agent-rules/virtual-shadow-map-page-cache.md §4).
        EXPECT_LT(settled.PagesDrawn, std::max(8u, residentAfterWarmup / 4u))
            << "a static scene with a static camera redrew " << settled.PagesDrawn << " of "
            << settled.PagesResident << " resident pages — caching is not working";

        // The pool must not be thrashing either: allocation failures mean the
        // physical pool is undersized for this view and shadows are silently
        // degrading to coarser clip levels.
        EXPECT_EQ(settled.PagesFailed, 0u)
            << "the physical pool could not satisfy " << settled.PagesFailed
            << " page requests — raise VirtualShadowMapSettings::PhysicalResolution";
        EXPECT_EQ(settled.CullOverflows, 0u) << "the cull dropped draw records — a CPU-side sizing bug";
    }
} // namespace OloEngine::Tests

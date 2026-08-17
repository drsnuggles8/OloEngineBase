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
//   3. both darkenings land in the SAME cell region  (VSM agrees with CSM about
//      WHERE the shadow is — this is what catches a wrong clip level, a wrong
//      page wrap, or a flipped raster origin, each of which still produces "a
//      shadow", just not in the right place)
//   4. the lit control band stays bright under both  (neither dims globally)
//
// (3) is the load-bearing one. (1) and (2) alone would pass for a system that
// shadows the entire floor.
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

        // The darkest cell of a scanned region, and where it sits. `Cell` is the
        // grid index so two techniques can be compared on POSITION, not just on
        // "something got darker".
        struct DarkCell
        {
            f64 Luma = 1e9;
            u32 CellX = 0;
            u32 CellY = 0;
            bool Found = false;
        };

        [[nodiscard]] DarkCell DarkestCell(const std::vector<u8>& px, f32 x0, f32 x1, f32 y0, f32 y1,
                                           u32 cellsX, u32 cellsY)
        {
            DarkCell best;
            const f32 dx = (x1 - x0) / static_cast<f32>(cellsX);
            const f32 dy = (y1 - y0) / static_cast<f32>(cellsY);
            for (u32 cy = 0; cy < cellsY; ++cy)
            {
                for (u32 cx = 0; cx < cellsX; ++cx)
                {
                    const f32 cellX0 = x0 + dx * static_cast<f32>(cx);
                    const f32 cellY0 = y0 + dy * static_cast<f32>(cy);
                    const f64 luma = SampleBand(px, cellX0, cellX0 + dx, cellY0, cellY0 + dy).Luma();
                    if (luma < best.Luma)
                        best = { luma, cx, cy, true };
                }
            }
            return best;
        }
    } // namespace

    class VirtualShadowMapVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
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
        constexpr f32 kLeeX0 = 0.62f, kLeeX1 = 0.88f;
        constexpr f32 kLitX0 = 0.12f, kLitX1 = 0.38f;
        constexpr f32 kScanY0 = 0.35f, kScanY1 = 0.75f;
        constexpr u32 kCellsX = 12, kCellsY = 12;

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
            ASSERT_TRUE(SetVirtualShadowMaps(false)) << "could not restore the CSM path";

            const BandStats csmLit = SampleBand(csm, kLitX0, kLitX1, kScanY0, kScanY1);
            const BandStats vsmLit = SampleBand(vsm, kLitX0, kLitX1, kScanY0, kScanY1);
            const DarkCell csmDark = DarkestCell(csm, kLeeX0, kLeeX1, kScanY0, kScanY1, kCellsX, kCellsY);
            const DarkCell vsmDark = DarkestCell(vsm, kLeeX0, kLeeX1, kScanY0, kScanY1, kCellsX, kCellsY);

            ASSERT_TRUE(csmDark.Found && vsmDark.Found) << "the lee scan region was empty";

            // The floor must actually be lit somewhere, or every comparison below
            // is between two blacks and passes vacuously.
            EXPECT_GT(csmLit.Luma(), 25.0) << "the CSM control band is not lit — the scene is too dark "
                                              "to draw any conclusion from";
            EXPECT_GT(vsmLit.Luma(), 25.0) << "the VSM control band is not lit";

            // 1. CSM casts a shadow at all (the baseline is real, not another
            //    shadowless scene).
            EXPECT_LT(csmDark.Luma, csmLit.Luma() - 8.0)
                << "CSM did not darken the lee floor (darkest lee cell " << csmDark.Luma
                << " vs lit band " << csmLit.Luma() << ") — the baseline casts nothing, so this "
                << "test cannot say anything about VSM";

            // 2. VSM casts a shadow at all.
            EXPECT_LT(vsmDark.Luma, vsmLit.Luma() - 8.0)
                << "VSM did not darken the lee floor (darkest lee cell " << vsmDark.Luma
                << " vs lit band " << vsmLit.Luma() << ")";

            // 3. THE LOAD-BEARING ONE: both put it in the same place. A wrong clip
            //    level, a wrong page wrap or a flipped raster origin all still
            //    produce "a shadow" — just not here.
            const i32 dx = static_cast<i32>(vsmDark.CellX) - static_cast<i32>(csmDark.CellX);
            const i32 dy = static_cast<i32>(vsmDark.CellY) - static_cast<i32>(csmDark.CellY);
            EXPECT_LE(std::abs(dx), 2) << "VSM's darkest lee cell is " << dx
                                       << " cells from CSM's in X — the shadow exists but is in the "
                                          "wrong place";
            EXPECT_LE(std::abs(dy), 2) << "VSM's darkest lee cell is " << dy
                                       << " cells from CSM's in Y — the shadow exists but is in the "
                                          "wrong place";

            // 4. Neither technique globally dims the frame: the lit control band
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

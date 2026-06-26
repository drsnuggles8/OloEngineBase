// OLO_TEST_LAYER: L8
// =============================================================================
// ReflectionProbeVisualEvidenceTest.cpp
//
// Visual evidence (PNG) + a driver-independent, golden-free contract for local
// reflection probes (ReflectionProbeBaker / ReflectionProbeComponent /
// Scene::ApplyReflectionProbeOverride — issue #230).
//
// A shiny metallic sphere sits at the centre of a closed, emissive-RED room. A
// ReflectionProbeComponent is BAKED at a point inside the room (the real
// editor "Bake" path: ReflectionProbeBaker::BakeProbe renders the scene from
// the probe into a cubemap + IBL chain). The same scene is then rendered
// several times through the FULL Renderer3D editor pipeline from the same pose,
// toggling only the probe, and each frame is written to
//   OloEditor/assets/tests/visual/ReflectionProbe_<state>.png
//
// States captured (all from the head-on pose unless noted):
//   * Off        — probe inactive: no IBL override, the metal stays neutral.
//   * On         — probe active + in range: the metal reflects the baked RED
//                  environment.
//   * OutOfRange — probe active but the camera sits OUTSIDE its influence
//                  radius: Scene::ApplyReflectionProbeOverride must NOT apply
//                  it, so the metal looks like Off again. This exercises the
//                  live selection/influence gate, which the pure-math
//                  ReflectionProbeSelectionTest cannot (it tests the geometry
//                  function in isolation, not the scene-integration path that
//                  builds the probe list and calls Renderer3D::SetGlobalIBL).
//   * Oblique    — On, from a second camera angle, so the reflection is shown
//                  to pick up the environment regardless of view direction.
//
// The contract is GOLDEN-FREE and differential (robust across GPUs, no
// committed reference image — and deliberately decoupled from the IBL-bake
// internals so a precompute change can't silently red this test):
//   1. The On frame rendered non-trivially (the scene actually drew).
//   2. The sphere centre reads RED-dominant with the probe On — the metal
//      reflects the room's dominant environment colour.
//   3. The sphere centre is NOT red-dominant with the probe Off — the redness
//      is supplied by the probe, not by the (red) background showing through
//      the opaque sphere.
//   4. The probe measurably changes the sphere centre (On is much redder than
//      Off) — the override is what produced the reflection.
//   5. With the camera outside the probe's influence radius the sphere centre
//      matches Off (not red-dominant) — the influence gate works end-to-end.
//   6. From a second angle the On reflection is still RED-dominant.
//
// The cheap probe-selection *math* contract lives in
// ReflectionProbeSelectionTest.cpp (SelectDominantReflectionProbe geometry).
// Per the CLAUDE.md rendering rule, that proves the formula; this test proves
// the rendered frame looks right.
//
// Runs in the normal suite and SKIPs (not fails) when no GL 4.6 context exists,
// matching WaterVisualEvidenceTest / BloomVisualEvidenceTest.
//
// Classification: L8 (full GL pipeline + cubemap bake + RGBA8 readback + PNG
// evidence).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/ReflectionProbeBaker.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glad/gl.h>
#include <glm/glm.hpp>
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

        // Mean luminance over the whole frame — a "did the render run at all"
        // floor that doesn't depend on any one region.
        [[nodiscard]] f64 MeanLuma(const std::vector<u8>& px)
        {
            f64 sum = 0.0;
            u64 count = 0;
            for (std::size_t i = 0; i + 3 < px.size(); i += 4)
            {
                sum += 0.2126 * px[i + 0] + 0.7152 * px[i + 1] + 0.0722 * px[i + 2];
                ++count;
            }
            return count ? sum / static_cast<f64>(count) : 0.0;
        }
    } // namespace

    class ReflectionProbeVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        Entity m_Sphere; // reflective subject at the origin
        Entity m_Probe;  // reflection probe (baked in the test body)

        void BuildScene() override
        {
            Scene& scene = GetScene();

            EnableRendering(kWidth, kHeight);

            // No lights and no global EnvironmentMap: the only light in the scene
            // is the emissive room, and the only IBL is the baked probe. That
            // makes the Off baseline a clean, near-neutral metal (0.03 ambient)
            // and isolates the probe as the sole source of the red reflection.

            // Closed emissive-RED room built from six thin cube slabs. Slabs (not
            // a single inverted box) so each wall presents a front face toward the
            // interior regardless of back-face culling, and emissive (not lit) so
            // the room is reliably RED in the bake without depending on any light.
            // Moderate emissive (≈0.9) so it tone-maps to a clear red, not a
            // blown-out white.
            const auto addWall = [&scene](const char* name, const glm::vec3& pos, const glm::vec3& scale)
            {
                Entity e = scene.CreateEntity(name);
                auto& tc = e.GetComponent<TransformComponent>();
                tc.Translation = pos;
                tc.Scale = scale;
                auto& mc = e.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Cube;
                if (Ref<Mesh> mesh = MeshPrimitives::CreateCube())
                    mc.m_MeshSource = mesh->GetMeshSource();
                auto& mat = e.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.05f, 0.0f, 0.0f, 1.0f));
                mat.m_Material.SetEmissiveFactor(glm::vec4(0.9f, 0.05f, 0.05f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
            };

            constexpr f32 kRoom = 10.0f;  // half-extent of the room
            constexpr f32 kThick = 0.25f; // slab half-thickness
            const f32 span = 2.0f * kRoom;
            addWall("Floor", { 0.0f, -kRoom, 0.0f }, { span, kThick, span });
            addWall("Ceiling", { 0.0f, kRoom, 0.0f }, { span, kThick, span });
            addWall("Wall-X", { -kRoom, 0.0f, 0.0f }, { kThick, span, span });
            addWall("Wall+X", { kRoom, 0.0f, 0.0f }, { kThick, span, span });
            addWall("Wall-Z", { 0.0f, 0.0f, -kRoom }, { span, span, kThick });
            addWall("Wall+Z", { 0.0f, 0.0f, kRoom }, { span, span, kThick });

            // Shiny metallic sphere at the origin: metallic=1 + white base colour
            // so the specular F0 is white and the metal reflects the environment
            // colour faithfully; a mid roughness blurs the reflection toward the
            // environment's average (robustly uniform red) rather than a sharp,
            // view-dependent mirror image.
            m_Sphere = scene.CreateEntity("ReflectiveSphere");
            m_Sphere.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 0.0f };
            {
                auto& mc = m_Sphere.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Sphere;
                if (Ref<Mesh> mesh = MeshPrimitives::CreateSphere(1.5f, 48))
                    mc.m_MeshSource = mesh->GetMeshSource();
                auto& mat = m_Sphere.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
                mat.m_Material.SetMetallicFactor(1.0f);
                mat.m_Material.SetRoughnessFactor(0.4f);
            }

            // Reflection probe offset above the sphere so the bake's view of the
            // room isn't occluded by the sphere itself. Influence radius is set
            // per-capture in the test body.
            m_Probe = scene.CreateEntity("ReflectionProbe");
            m_Probe.GetComponent<TransformComponent>().Translation = { 0.0f, 3.5f, 0.0f };
            {
                auto& probe = m_Probe.AddComponent<ReflectionProbeComponent>();
                probe.m_InfluenceRadius = 40.0f;
                probe.m_Resolution = 256;
                probe.m_Intensity = 1.0f;
                probe.m_Active = false; // start inactive; the test toggles it
            }
        }

        // Render the current scene/probe state from the given pose, read back the
        // composited frame (top-down rows), save it as PNG evidence, and verify
        // the PNG round-trips (write succeeded + reloads bit-identical).
        void Capture(const std::string& tag, const glm::vec3& position, f32 yaw, f32 pitch,
                     std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, yaw, pitch);

            RunEditorFrames(camera, 2);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for capture '" << tag << "'";

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

            const std::string path = (dir / ("ReflectionProbe_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth),
                                               static_cast<int>(kHeight), 4, outPixels.data(),
                                               static_cast<int>(kWidth) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";

            int w = 0, h = 0, ch = 0;
            stbi_uc* loaded = ::stbi_load(path.c_str(), &w, &h, &ch, 4);
            ASSERT_NE(loaded, nullptr) << "Failed to reload written PNG '" << path << "'";
            EXPECT_EQ(w, static_cast<int>(kWidth));
            EXPECT_EQ(h, static_cast<int>(kHeight));
            if (w == static_cast<int>(kWidth) && h == static_cast<int>(kHeight))
            {
                EXPECT_EQ(std::memcmp(loaded, outPixels.data(),
                                      static_cast<std::size_t>(kWidth) * kHeight * 4u),
                          0)
                    << "Reloaded PNG pixels differ from the written buffer: " << path;
            }
            ::stbi_image_free(loaded);
        }
    };

    // A baked reflection probe must make a reflective metal pick up the room's
    // colour, only when the camera is inside the probe's influence. SKIPs without
    // a GL 4.6 context (see file header).
    TEST_F(ReflectionProbeVisualEvidenceTest, ReflectiveSpherePicksUpBakedEnvironment)
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

        // --- Bake the probe (the real editor "Bake" path) ---
        // BakeProbe renders the scene from the probe position into a cubemap and
        // builds the IBL chain. The probe is still inactive here, so the bake's
        // own RenderScene3D pass sees no probe override (m_BakedEnvironment is
        // null) — no self-reference.
        Ref<Scene> sceneRef = GetSceneRef();
        auto& probe = m_Probe.GetComponent<ReflectionProbeComponent>();
        const glm::vec3 probePos = m_Probe.GetComponent<TransformComponent>().Translation;

        // BakeProbe drives several full-pipeline renders directly (not via the
        // GLStateGuard-wrapped RunEditorFrames helper), so contain the global GL
        // state it leaves behind — otherwise this test could poison later GPU
        // tests in the same process (the issue #258 hazard).
        bool baked = false;
        {
            GLStateGuard bakeGuard("ReflectionProbeVisualEvidence::Bake", GLStateGuard::Policy::Restore);
            baked = ReflectionProbeBaker::BakeProbe(sceneRef, probePos, probe);
        }
        ASSERT_TRUE(baked) << "ReflectionProbeBaker::BakeProbe failed";
        ASSERT_TRUE(probe.m_BakedEnvironment && probe.m_BakedEnvironment->HasIBL())
            << "Bake produced no usable IBL environment";

        // BakeProbe squashes the render graph down to the probe resolution and
        // restores it on the way out; re-assert the editor-capture size anyway so
        // the composited captures below deterministically render at kWidth x
        // kHeight regardless of the bake's internal sizing.
        Renderer3D::OnWindowResize(kWidth, kHeight);
        SetViewport(kWidth, kHeight);

        // The reflective sphere is at screen centre in every pose, so sample a
        // small band there.
        constexpr f32 cx0 = 0.45f, cx1 = 0.55f, cy0 = 0.45f, cy1 = 0.55f;

        // Head-on pose: camera on +Z looking toward the sphere at the origin.
        const glm::vec3 frontPos{ 0.0f, 0.0f, 6.0f };
        constexpr f32 frontYaw = 0.0f;
        constexpr f32 frontPitch = 0.0f;

        // --- Off: probe inactive (no override) ---
        probe.m_Active = false;
        probe.m_InfluenceRadius = 40.0f;
        std::vector<u8> offPixels;
        Capture("Off", frontPos, frontYaw, frontPitch, offPixels);
        if (::testing::Test::HasFatalFailure())
            return;

        // --- On: probe active, camera well inside the influence radius ---
        probe.m_Active = true;
        probe.m_InfluenceRadius = 40.0f;
        std::vector<u8> onPixels;
        Capture("On", frontPos, frontYaw, frontPitch, onPixels);
        if (::testing::Test::HasFatalFailure())
            return;

        // --- OutOfRange: probe active but the camera sits OUTSIDE its influence
        //     radius (probe at (0,3.5,0), camera ~6.95 units away; radius 1.0). The
        //     live selection gate must decline to apply it. ---
        probe.m_Active = true;
        probe.m_InfluenceRadius = 1.0f;
        std::vector<u8> outOfRangePixels;
        Capture("OutOfRange", frontPos, frontYaw, frontPitch, outOfRangePixels);
        if (::testing::Test::HasFatalFailure())
            return;

        // --- Oblique: On, from a second angle (orbited 45° around +Y). ---
        probe.m_Active = true;
        probe.m_InfluenceRadius = 40.0f;
        const glm::vec3 obliquePos{ 5.0f, 0.0f, 5.0f };
        constexpr f32 obliqueYaw = -0.785398f; // -45°, aims (5,0,5) at the origin
        constexpr f32 obliquePitch = 0.0f;
        std::vector<u8> obliquePixels;
        Capture("Oblique", obliquePos, obliqueYaw, obliquePitch, obliquePixels);
        if (::testing::Test::HasFatalFailure())
            return;

        const BandStats centerOff = SampleBand(offPixels, cx0, cx1, cy0, cy1);
        const BandStats centerOn = SampleBand(onPixels, cx0, cx1, cy0, cy1);
        const BandStats centerOut = SampleBand(outOfRangePixels, cx0, cx1, cy0, cy1);
        const BandStats centerObl = SampleBand(obliquePixels, cx0, cx1, cy0, cy1);

        // (1) The On frame actually rendered (not a black/failed frame).
        EXPECT_GT(MeanLuma(onPixels), 5.0)
            << "On frame is (near-)black — the scene did not render. See ReflectionProbe_On.png";

        // (2) Probe On: the sphere centre reflects the room's RED environment.
        EXPECT_GT(centerOn.R, centerOn.G + 12.0)
            << "Sphere centre is not red-dominant with the probe On (R=" << centerOn.R
            << " G=" << centerOn.G << " B=" << centerOn.B << ") — the metal is not reflecting the "
            << "baked environment. See ReflectionProbe_On.png";
        EXPECT_GT(centerOn.R, centerOn.B + 12.0)
            << "Sphere centre is not red-dominant over blue with the probe On (R=" << centerOn.R
            << " G=" << centerOn.G << " B=" << centerOn.B << "). See ReflectionProbe_On.png";

        // (3) Probe Off: the sphere centre is NOT red-dominant. The opaque sphere
        //     covers screen centre in both frames, so this rules out "the red
        //     background just shows through" — the redness in On must come from
        //     the probe reflection, not the (identical) red walls behind.
        EXPECT_LE(centerOff.R, centerOff.G + 12.0)
            << "Sphere centre is already red-dominant with the probe Off (R=" << centerOff.R
            << " G=" << centerOff.G << " B=" << centerOff.B << ") — the baseline is contaminated "
            << "(is the sphere covering screen centre?). See ReflectionProbe_Off.png";

        // (4) The probe is what changed the sphere: On is markedly redder than Off.
        EXPECT_GT(centerOn.R, centerOff.R + 20.0)
            << "Enabling the probe did not measurably redden the sphere centre (off.R=" << centerOff.R
            << " on.R=" << centerOn.R << "). See ReflectionProbe_Off.png / ReflectionProbe_On.png";

        // (5) Influence gate (the live selection path, not covered by the math
        //     test): camera outside the influence radius => no override => the
        //     sphere centre looks like Off, not red.
        EXPECT_LE(centerOut.R, centerOut.G + 12.0)
            << "Sphere centre is red-dominant even though the camera is OUTSIDE the probe's "
            << "influence radius (R=" << centerOut.R << " G=" << centerOut.G << " B=" << centerOut.B
            << ") — Scene::ApplyReflectionProbeOverride applied an out-of-range probe. "
            << "See ReflectionProbe_OutOfRange.png";
        EXPECT_LT(std::abs(centerOut.R - centerOff.R), 20.0)
            << "Out-of-range capture (R=" << centerOut.R << ") does not match the Off baseline (R="
            << centerOff.R << ") — the influence gate is not behaving like 'no probe'. "
            << "See ReflectionProbe_OutOfRange.png";

        // (6) The reflection holds from a second view angle.
        EXPECT_GT(centerObl.R, centerObl.G + 12.0)
            << "Oblique-angle sphere centre is not red-dominant (R=" << centerObl.R
            << " G=" << centerObl.G << " B=" << centerObl.B << ") — reflection is view-dependent in "
            << "a way it should not be. See ReflectionProbe_Oblique.png";
    }
} // namespace OloEngine::Tests

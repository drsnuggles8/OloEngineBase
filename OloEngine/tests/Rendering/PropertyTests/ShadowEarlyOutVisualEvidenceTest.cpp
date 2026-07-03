// OLO_TEST_LAYER: L8
// =============================================================================
// ShadowEarlyOutVisualEvidenceTest.cpp
//
// Visual regression guard for issue #522 — ShadowRenderPass early-out when no
// light casts shadows. The optimization gates caster submission (Scene) and the
// whole pass (ShadowRenderPass::Execute) on ShadowMap::AnyShadowsRequested().
// Two silent failure modes have to be ruled out on real pixels, not just math:
//
//   1. Too-aggressive early-out KILLS real shadows — the change must NOT stop a
//      light that DOES cast shadows from rendering them. (The main risk.)
//   2. A stale-matrix leak — when shadows are OFF the pass must produce a
//      cleanly-lit frame, not a garbage shadow projected from last frame's /
//      identity CSM matrices.
//
// The scene is the same shadow-centric layout as PCSSVisualEvidenceTest (flat
// ground receiver + a tall pole + a floating cube) lit by a low-angle
// directional sun. We render it TWICE from each of two camera angles — once
// with the sun's m_CastShadows = true, once = false — and compare the ground:
//
//   * CastShadows = true  -> a meaningful band of shadowed (darkened) ground.
//   * CastShadows = false -> that band is gone; the ground reads uniformly lit,
//     AND the frame is still a real lit render (not black / not flat).
//
// Driver-independent (no committed goldens): the contract is the RELATIVE drop
// in shadowed-ground pixels between the two renders, not an absolute image.
//
// Runs in the normal suite; SKIPs cleanly when no GL 4.6 context exists,
// matching the issue #258 RendererAttachedTest discipline.
//
// Classification: L8 / integration (full GL pipeline through the real Scene
// render path, RGBA8 readback + PNG evidence).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Utils/PlatformUtils.h" // Time::SetMockTime / ClearMockTime

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 640;
        constexpr u32 kHeight = 360;
        constexpr f32 kCaptureTime = 5.0f; // frozen clock -> deterministic frames

        [[nodiscard]] f64 Luma(const u8* p)
        {
            return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
        }

        fs::path VisualDir()
        {
            fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir;
        }
    } // namespace

    class ShadowEarlyOutVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        Entity m_Sun;

        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            // Low-angle sun pointing toward +Z and down, so the pole throws a long
            // shadow toward the camera (which sits on +Z). Cast shadows ON here; the
            // test toggles it per-capture.
            m_Sun = scene.CreateEntity("Sun");
            {
                auto& tc = m_Sun.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 25.0f, -25.0f };
                auto& dl = m_Sun.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(0.18f, -0.55f, 0.55f));
                dl.m_Color = glm::vec3(1.0f, 0.98f, 0.95f);
                dl.m_Intensity = 3.5f;
                dl.m_CastShadows = true;
            }

            auto addPrimitive = [&scene](const char* name, MeshPrimitive prim, const glm::vec3& pos,
                                         const glm::vec3& scale, const glm::vec3& albedo)
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
                auto& mat = e.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(albedo, 1.0f));
            };

            // Flat light-grey ground (the receiver) — large so the long shadow fits.
            addPrimitive("Ground", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f }, { 40.0f, 1.0f, 40.0f },
                         { 0.72f, 0.72f, 0.74f });
            // Tall thin pole STANDING on the ground at z = -2.
            addPrimitive("Pole", MeshPrimitive::Cube, { 0.0f, 6.0f, -2.0f }, { 0.6f, 12.0f, 0.6f },
                         { 0.55f, 0.55f, 0.58f });
            // Cube floating ABOVE the ground — a second cast shadow.
            addPrimitive("FloatCube", MeshPrimitive::Cube, { 6.0f, 4.0f, 1.0f }, { 2.0f, 2.0f, 2.0f },
                         { 0.6f, 0.3f, 0.25f });
        }

        // Render one pose with the sun casting (or not) and read back the frame,
        // top-flipped. Also writes a PNG so a reviewer always has the evidence.
        void Capture(const std::string& poseName, bool castShadows, const EditorCamera& camera,
                     std::vector<u8>& outPixels)
        {
            // Ensure global shadow mapping is enabled — we're isolating the PER-LIGHT
            // cast flag, not the global toggle.
            ShadowSettings s = Renderer3D::GetShadowMap().GetSettings();
            s.Enabled = true;
            Renderer3D::GetShadowMap().SetSettings(s);

            m_Sun.GetComponent<DirectionalLightComponent>().m_CastShadows = castShadows;

            RunEditorFrames(camera, 2);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for pose '" << poseName << "'";

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            // GL readback is bottom-up; flip so row 0 is the TOP of the frame.
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

            const std::string path =
                (VisualDir() / ("ShadowEarlyOut_" + poseName + "_" + (castShadows ? "on" : "off") + ".png")).string();
            ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                             outPixels.data(), static_cast<int>(kWidth) * 4);
        }
    };

    // With a directional light casting shadows, real shadows must render; with the
    // SAME light's m_CastShadows toggled off (the early-out path), the shadows must
    // cleanly disappear while the frame stays a real lit render. SKIPs without GPU.
    TEST_F(ShadowEarlyOutVisualEvidenceTest, ShadowsRenderWhenCastingAndVanishWhenNot)
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

        // A cast shadow is proven by the per-pixel DIFFERENCE between the two
        // frames: enabling shadows can only DARKEN lit ground, never brighten it,
        // and never change the geometry / background. So we compare on-vs-off:
        //   * onDarkens  — pixels lit in OFF that turn dark in ON  => the shadow.
        //   * offDarkens — pixels lit in ON  that turn dark in OFF => a dark patch
        //     present ONLY when shadows are "off", i.e. exactly the stale-matrix
        //     garbage the bug produced. Correct behaviour: ~0.
        // This is robust to the grey pole/cube geometry and the far checkerboard
        // floor (identical mid-luma in both frames -> tiny |diff| -> not counted),
        // and needs no committed golden.
        constexpr f64 kLitLuma = 140.0;    // lit grey ground reads well above this
        constexpr f64 kDarkenDelta = 35.0; // meaningful shadow darkening
        const auto diffClassify = [](const std::vector<u8>& on, const std::vector<u8>& off,
                                     std::size_t& onDarkens, std::size_t& offDarkens,
                                     std::size_t& litOff, std::size_t& total)
        {
            onDarkens = offDarkens = litOff = total = 0;
            const u32 yStart = static_cast<u32>(static_cast<f32>(kHeight) * 0.45f);
            for (u32 y = yStart; y < kHeight; ++y)
            {
                for (u32 x = 0; x < kWidth; ++x)
                {
                    const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                    const f64 lOn = Luma(on.data() + idx);
                    const f64 lOff = Luma(off.data() + idx);
                    ++total;
                    if (lOff > kLitLuma)
                        ++litOff; // lit ground in the no-shadow frame
                    if (lOff > kLitLuma && lOn < lOff - kDarkenDelta)
                        ++onDarkens; // shadow cast by enabling shadows
                    if (lOn > kLitLuma && lOff < lOn - kDarkenDelta)
                        ++offDarkens; // spurious darkening ONLY when shadows off (the bug)
                }
            }
        };

        const auto verifyPose = [&](const std::string& poseName, const EditorCamera& camera)
        {
            std::vector<u8> onPixels;
            Capture(poseName, /*castShadows=*/true, camera, onPixels);
            if (::testing::Test::HasFatalFailure())
                return;
            std::vector<u8> offPixels;
            Capture(poseName, /*castShadows=*/false, camera, offPixels);
            if (::testing::Test::HasFatalFailure())
                return;

            std::size_t onDarkens, offDarkens, litOff, total;
            diffClassify(onPixels, offPixels, onDarkens, offDarkens, litOff, total);
            ASSERT_GT(total, 0u);

            // (1) Regression guard — the MAIN risk. A casting light must actually
            // paint a shadow: enabling shadows darkens a non-trivial patch of ground.
            EXPECT_GT(onDarkens, total / 200)
                << poseName << ": expected a rendered shadow when the sun casts shadows "
                << "(ground pixels darkened by enabling shadows=" << onDarkens << " of " << total << ")";

            // (2) The no-shadow frame is still a real lit render — not black (that
            // would mean the early-out broke the whole scene, not just shadows).
            EXPECT_GT(litOff, total / 20)
                << poseName << ": the no-shadow frame must still show lit ground "
                << "(lit px=" << litOff << " of " << total << ")";

            // (3) Clean early-out: with cast-shadows OFF, no ground pixel is darker
            // than in the ON frame. A stale-matrix leak (the bug) would project a
            // garbage shadow only in the OFF frame, lighting up offDarkens.
            EXPECT_LT(offDarkens, onDarkens / 10)
                << poseName << ": the no-shadow frame has dark patches the shadowed frame lacks "
                << "(offDarkens=" << offDarkens << " vs onDarkens=" << onDarkens
                << ") — suggests the pass rendered against stale CSM matrices instead of early-outing";
        };

        const f32 aspect = static_cast<f32>(kWidth) / static_cast<f32>(kHeight);

        // Pose A — 3/4 view from +Z, angled down: the pole's long shadow fills the
        // lower frame. SetPose: yaw 0 looks toward -Z, positive pitch tilts down.
        EditorCamera threeQuarter(60.0f, aspect, 0.05f, 1000.0f);
        threeQuarter.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        threeQuarter.SetPose({ 0.0f, 11.0f, 17.0f }, 0.0f, 0.55f);
        verifyPose("ThreeQuarter", threeQuarter);
        if (::testing::Test::HasFatalFailure())
            return;

        // Pose B — higher, X-offset, steeper oblique view. A different angle changes
        // the cascade coverage and the on-screen shadow projection, guarding against
        // view-dependent regressions.
        EditorCamera oblique(60.0f, aspect, 0.05f, 1000.0f);
        oblique.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        oblique.SetPose({ 8.0f, 14.0f, 13.0f }, 0.0f, 0.72f);
        verifyPose("Oblique", oblique);
    }
} // namespace OloEngine::Tests

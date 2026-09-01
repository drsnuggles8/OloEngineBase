// OLO_TEST_LAYER: L8
// =============================================================================
// GBufferFlagsResolveEvidenceTest.cpp — the G-Buffer flags lane must survive an
// MSAA resolve (issue #996).
//
// THE BUG THIS PINS. G-Buffer RT2 is vec4(emissive.rgb, materialFlags). Three
// of those channels are radiometry and want the hardware's averaging resolve;
// the fourth is a BITFIELD — bit 0 unlit, the PBR closure model in bits 1..,
// see oloEncodeGBufferPbrFlags in include/PBRCommon.glsl. In the resolved-MSAA
// deferred mode (MSAASampleCount > 1 with PerSampleLighting OFF — both live,
// non-default, session-local toggles in the Renderer Settings panel)
// GBuffer::Resolve() average-blitted every colour attachment, so a silhouette
// pixel half-covered by a ClosureV2 surface (a = 2.0) over Legacy geometry
// (a = 0.0) resolved to exactly 1.0, which decodes as UNLIT: the lighting pass
// returned that pixel's raw emissive — black for an ordinary material — and
// every ClosureV2 object grew a dark fringe. Against the cleared background
// (a = 1.0) the same average landed on 1.5, where GLSL round()'s half-way
// behaviour is implementation-defined, so the frame was not even stable across
// vendors or between GL and Vulkan.
//
// WHY A PIXEL TEST. The defect leaves every CPU/contract test green: the encode
// is right, the decode is right, and the corruption happens in between, inside
// a fixed-function blit. The only place it is observable is the frame.
//
// WHAT IS ASSERTED. A dark FRINGE is a pixel much darker than the lit pixels a
// few columns to either side of it — the signature of a shaded surface meeting
// a shaded background through a mis-decoded silhouette. The count is NOT
// asserted against zero: a lit sphere has a real terminator, and this scene's
// own limb trips the detector on a handful of pixels no matter what the resolve
// does. The MSAA = 1 render of the same scene is therefore the control — it
// resolves nothing, so its count IS the noise floor — and the assertion is that
// turning MSAA on with per-sample lighting off does not raise it.
//
// Measured here on 2026-09-01 (NVIDIA, GL) via the OLO_GBUFFER_NO_FLAGS_RESOLVE
// lever, which switches off exactly this fix and nothing else:
//
//                    control (MSAA 1)   MSAA 4, fix OFF   MSAA 4, fix ON
//   ClosureV2 subject       14                62                 3
//   Legacy subject          14                 3                 3
//
// The ClosureV2 row is the bug and the assertion's load-bearing case: 62 > 14
// fails, 3 <= 14 passes. Frame-diffing those two MSAA frames put the difference
// at 86 pixels along the sphere's outline, mean luma 27 (min 0 — literally
// black) becoming mean 151. The Legacy row is the regression guard for the
// other half of the issue's acceptance criteria: the fix touches RT2 on every
// MSAA frame, Legacy or not, so a Legacy silhouette must not acquire a fringe
// either.
//
// Both NON-MSAA frames came back byte-identical (matching MD5s) with the lever
// on and off, which is the "Legacy-only scenes are unaffected" claim measured
// rather than assumed. Under MSAA the Legacy frame does move, on 23 pixels,
// every one of them BRIGHTER (luma 76 -> ~102): those are the milder variant of
// the same defect that ADR 0016 §5 recorded against the pre-#975 encoding — a
// Legacy silhouette averaging with the unlit background to a half-way value —
// so they were already wrong, and are now right.
//
// Evidence PNGs (written before any assertion):
//   OloEditor/assets/tests/visual/GBufferFlagsResolve_<case>.png
//
// Classification: L8 (full Scene pipeline, RGBA8 readback + PNG; SKIPs cleanly
// without a GL 4.6 context).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/PBRModel.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>

#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kSize = 320;

        // How far to either side a "fringe" candidate looks for its lit
        // neighbours. 3 px clears the 1-2 px an MSAA silhouette legitimately
        // spans, so a correctly anti-aliased edge is never the thing measured.
        constexpr i32 kNeighbourSpan = 3;

        [[nodiscard]] f32 LuminanceAt(const std::vector<u8>& px, std::size_t idx)
        {
            const f32 r = static_cast<f32>(px[idx + 0]) / 255.0f;
            const f32 g = static_cast<f32>(px[idx + 1]) / 255.0f;
            const f32 b = static_cast<f32>(px[idx + 2]) / 255.0f;
            return 0.2126f * r + 0.7152f * g + 0.0722f * b;
        }

        // Count pixels much darker than the pixels kNeighbourSpan columns away
        // on BOTH sides, where both of those are clearly lit. That shape is
        // what a mis-decoded silhouette makes and what an ordinary dark feature
        // (a shadow, a dark material, the frame border) does not: a real dark
        // region is dark on at least one side too.
        [[nodiscard]] u32 CountDarkFringePixels(const std::vector<u8>& px, u32 w, u32 h)
        {
            // The floor sits well below anything this deliberately bright scene
            // shades to, and well above the black the unlit early-out returns
            // for a material with no emissive.
            constexpr f32 kLitFloor = 0.12f;
            constexpr f32 kFringeRatio = 0.45f;

            u32 count = 0;
            for (u32 y = 0; y < h; ++y)
            {
                const std::size_t row = static_cast<std::size_t>(y) * w;
                for (i32 x = kNeighbourSpan; x < static_cast<i32>(w) - kNeighbourSpan; ++x)
                {
                    const f32 here = LuminanceAt(px, (row + static_cast<std::size_t>(x)) * 4);
                    const f32 left = LuminanceAt(px, (row + static_cast<std::size_t>(x - kNeighbourSpan)) * 4);
                    const f32 right = LuminanceAt(px, (row + static_cast<std::size_t>(x + kNeighbourSpan)) * 4);
                    if (left < kLitFloor || right < kLitFloor)
                        continue;
                    if (here < std::min(left, right) * kFringeRatio)
                        ++count;
                }
            }
            return count;
        }

        [[nodiscard]] f32 PeakLuminance(const std::vector<u8>& px, u32 w, u32 h)
        {
            f32 peak = 0.0f;
            for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i)
                peak = std::max(peak, LuminanceAt(px, i * 4));
            return peak;
        }

        [[nodiscard]] fs::path VisualOutputPath(const char* caseName)
        {
            fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir / (std::string("GBufferFlagsResolve_") + caseName + ".png");
        }
    } // namespace

    // The scene makes the two silhouettes the issue names both present in one
    // frame, and everything else in the frame bright:
    //   * a large Legacy-material wall fills the middle of the view, so the
    //     lower part of the sphere's outline is ClosureV2 (a = 2.0) against
    //     Legacy (a = 0.0) — the average that landed exactly on the UNLIT code;
    //   * the top of the sphere clears the wall and is outlined against the
    //     cleared G-Buffer background (a = 1.0) — the average that landed on
    //     1.5, the implementation-defined round() case.
    // No shadows, and the key light points down the view axis, so no pixel in
    // the frame is legitimately dark and the fringe detector has nothing else
    // to find.
    class GBufferFlagsResolveScene : public RendererAttachedTest
    {
      protected:
        // Set by the two fixtures below before BuildScene runs.
        bool m_SubjectUsesClosureV2 = true;

        void BuildScene() override
        {
            Entity camera = GetScene().CreateEntity("Camera");
            camera.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 6.0f };
            auto& cameraComp = camera.AddComponent<CameraComponent>();
            cameraComp.Primary = true;
            cameraComp.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);

            Entity sun = GetScene().CreateEntity("Sun");
            auto& dirLight = sun.AddComponent<DirectionalLightComponent>();
            dirLight.m_Direction = { -0.15f, -0.25f, -1.0f };
            dirLight.m_Color = { 1.0f, 1.0f, 1.0f };
            dirLight.m_Intensity = 4.0f;
            dirLight.m_CastShadows = false;

            // The LEGACY backdrop the lower silhouette is measured against: a
            // cube scaled into a wall, deliberately left at the constructor
            // default model (Legacy, a = 0.0 in the flags lane).
            {
                Entity wall = GetScene().CreateEntity("LegacyWall");
                wall.AddComponent<MeshComponent>(MeshPrimitives::CreateCube()->GetMeshSource());
                auto& transform = wall.GetComponent<TransformComponent>();
                transform.Translation = { 0.0f, -1.6f, -2.0f };
                transform.Scale = { 14.0f, 4.0f, 0.5f };
                auto& materialComp = wall.AddComponent<MaterialComponent>();
                materialComp.m_Material.SetBaseColorFactor(glm::vec4(0.75f, 0.72f, 0.70f, 1.0f));
                materialComp.m_Material.SetMetallicFactor(0.0f);
                materialComp.m_Material.SetRoughnessFactor(0.6f);
            }

            // The SUBJECT. Straddles the wall's top edge, so its outline is
            // half against Legacy geometry and half against the background.
            {
                Entity subject = GetScene().CreateEntity("Subject");
                subject.AddComponent<MeshComponent>(MeshPrimitives::CreateSphere(1.0f, 32)->GetMeshSource());
                subject.GetComponent<TransformComponent>().Translation = { 0.0f, 0.35f, 0.0f };
                auto& materialComp = subject.AddComponent<MaterialComponent>();
                materialComp.m_Material.SetBaseColorFactor(glm::vec4(0.85f, 0.80f, 0.75f, 1.0f));
                materialComp.m_Material.SetMetallicFactor(0.0f);
                materialComp.m_Material.SetRoughnessFactor(0.35f);
                if (m_SubjectUsesClosureV2)
                    materialComp.m_Material.SetPBRModel(PBRModel::ClosureV2);
            }

            EnableRendering(kSize, kSize);
        }

        // Renders the scene on the Deferred path with the given MSAA
        // configuration, writes the evidence PNG, and returns the frame. The
        // fixture's TearDown restores RendererSettings, so neither toggle can
        // leak into the next test.
        void RenderDeferred(u32 sampleCount, bool perSampleLighting, const char* caseName,
                            std::vector<u8>& outPx, u32& outWidth, u32& outHeight)
        {
            auto& settings = Renderer3D::GetRendererSettings();
            settings.Path = RenderingPath::Deferred;
            settings.Deferred.MSAASampleCount = sampleCount;
            settings.Deferred.PerSampleLighting = perSampleLighting;
            Renderer3D::ApplyRendererSettings();

            RunFrames(2);

            ASSERT_TRUE(ReadbackComposite(outPx, outWidth, outHeight))
                << caseName << ": ReadbackComposite failed";
            ASSERT_EQ(outPx.size(), static_cast<std::size_t>(outWidth) * outHeight * 4u);

            const fs::path out = VisualOutputPath(caseName);
            const int wrote = ::stbi_write_png(out.string().c_str(),
                                               static_cast<int>(outWidth), static_cast<int>(outHeight),
                                               4, outPx.data(), static_cast<int>(outWidth) * 4);
            EXPECT_NE(wrote, 0) << "failed to write " << out.string();
        }
    };

    class GBufferFlagsResolveClosureV2Scene : public GBufferFlagsResolveScene
    {
      protected:
        void SetUp() override
        {
            m_SubjectUsesClosureV2 = true;
            RendererAttachedTest::SetUp();
        }
    };

    class GBufferFlagsResolveLegacyScene : public GBufferFlagsResolveScene
    {
      protected:
        void SetUp() override
        {
            m_SubjectUsesClosureV2 = false;
            RendererAttachedTest::SetUp();
        }
    };

    // THE acceptance criterion: MSAA > 1 with per-sample lighting off must not
    // misclassify a ClosureV2 silhouette as unlit — against Legacy geometry
    // (the exact-1.0 case) or against the background (the 1.5 case).
    TEST_F(GBufferFlagsResolveClosureV2Scene, ResolvedMSAAKeepsClosureV2SilhouettesLit)
    {
        std::vector<u8> controlPx;
        std::vector<u8> resolvedPx;
        u32 w = 0;
        u32 h = 0;

        // Control first: no MSAA, so no resolve, so no averaged bitfield. Its
        // fringe count is this scene's noise floor and is what makes the
        // resolved count below mean anything.
        RenderDeferred(1u, false, "ClosureV2_NoMSAA", controlPx, w, h);
        if (::testing::Test::HasFatalFailure())
            return;
        const u32 controlFringe = CountDarkFringePixels(controlPx, w, h);

        // The reported-broken configuration, exactly: MSAA on, per-sample off.
        RenderDeferred(4u, false, "ClosureV2_MSAA4_Resolved", resolvedPx, w, h);
        if (::testing::Test::HasFatalFailure())
            return;
        const u32 resolvedFringe = CountDarkFringePixels(resolvedPx, w, h);

        // Non-vacuity: the frame has to contain a lit subject, or a black frame
        // would trivially have no fringes.
        ASSERT_GT(PeakLuminance(resolvedPx, w, h), 0.2f)
            << "the MSAA frame looks empty; nothing was measured. See "
            << VisualOutputPath("ClosureV2_MSAA4_Resolved").string();

        // The comparison, not a constant. Measured 62 against a 14 control with
        // the fix off and 3 with it on, so the margin is wide in both
        // directions and there is no threshold for anyone to tune.
        EXPECT_LE(resolvedFringe, controlFringe)
            << "MSAA 4x with per-sample lighting OFF produced " << resolvedFringe
            << " dark-fringe pixels against a non-MSAA control of " << controlFringe
            << " — the ClosureV2 silhouette is decoding as UNLIT and returning raw emissive. "
               "That is the averaged flags bitfield (issue #996): the resolve must exclude RT2's "
               "alpha, see GBuffer::ResolveFlagsLane / GBufferFlagsResolve.glsl. Re-run with "
               "OLO_GBUFFER_NO_FLAGS_RESOLVE=1 to see the defect on purpose. Evidence: "
            << VisualOutputPath("ClosureV2_MSAA4_Resolved").string() << " vs "
            << VisualOutputPath("ClosureV2_NoMSAA").string();
    }

    // The other half of the acceptance criteria: a Legacy-only scene must be
    // unaffected. A fix that excludes the lane from the resolve touches RT2 on
    // every MSAA frame, Legacy or not, so this is the regression this change is
    // most likely to cause.
    TEST_F(GBufferFlagsResolveLegacyScene, LegacyOnlySceneIsUnaffectedByTheFlagsResolve)
    {
        std::vector<u8> controlPx;
        std::vector<u8> resolvedPx;
        u32 w = 0;
        u32 h = 0;

        RenderDeferred(1u, false, "Legacy_NoMSAA", controlPx, w, h);
        if (::testing::Test::HasFatalFailure())
            return;
        const u32 controlFringe = CountDarkFringePixels(controlPx, w, h);

        RenderDeferred(4u, false, "Legacy_MSAA4_Resolved", resolvedPx, w, h);
        if (::testing::Test::HasFatalFailure())
            return;
        const u32 resolvedFringe = CountDarkFringePixels(resolvedPx, w, h);

        ASSERT_GT(PeakLuminance(resolvedPx, w, h), 0.2f)
            << "the Legacy MSAA frame looks empty; nothing was measured. See "
            << VisualOutputPath("Legacy_MSAA4_Resolved").string();

        EXPECT_LE(resolvedFringe, controlFringe)
            << "a Legacy-only scene grew dark fringes under MSAA (" << resolvedFringe
            << " against a non-MSAA control of " << controlFringe
            << ") — the flags-lane resolve (GBuffer::ResolveFlagsLane, issue #996) is writing "
               "something other than a single sample's flags, or is not restoring the colour "
               "mask. Evidence: "
            << VisualOutputPath("Legacy_MSAA4_Resolved").string();
    }
} // namespace OloEngine::Tests

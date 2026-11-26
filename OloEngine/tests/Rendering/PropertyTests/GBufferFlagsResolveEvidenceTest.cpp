// OLO_TEST_LAYER: L8
// Evidence PNGs (written before any assertion):
//   OloEditor/assets/tests/visual/GBufferFlagsResolve_<case>.png
//
// Classification: L8 (full Scene pipeline, RGBA8 readback + PNG; SKIPs cleanly
// without a GL 4.6 context).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/MaterialKind.h"
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

    // The upper silhouette meets the unlit clear. The lower meets Generic geometry.
    // No shadows, and the key light points down the view axis, so no pixel in
    // the frame is legitimately dark and the fringe detector has nothing else
    // to find.
    class GBufferFlagsResolveScene : public RendererAttachedTest
    {
      protected:
        // Set by the two fixtures below before BuildScene runs.
        bool m_SubjectUsesSnowKind = true;

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

            {
                Entity wall = GetScene().CreateEntity("GenericWall");
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
            // half against Generic geometry and half against the background.
            {
                Entity subject = GetScene().CreateEntity("Subject");
                subject.AddComponent<MeshComponent>(MeshPrimitives::CreateSphere(1.0f, 32)->GetMeshSource());
                subject.GetComponent<TransformComponent>().Translation = { 0.0f, 0.35f, 0.0f };
                auto& materialComp = subject.AddComponent<MaterialComponent>();
                materialComp.m_Material.SetBaseColorFactor(glm::vec4(0.85f, 0.80f, 0.75f, 1.0f));
                materialComp.m_Material.SetMetallicFactor(0.0f);
                materialComp.m_Material.SetRoughnessFactor(0.35f);
                if (m_SubjectUsesSnowKind)
                    materialComp.m_Material.SetMaterialKind(MaterialKind::Snow);
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

    class GBufferFlagsResolveSnowScene : public GBufferFlagsResolveScene
    {
      protected:
        void SetUp() override
        {
            m_SubjectUsesSnowKind = true;
            RendererAttachedTest::SetUp();
        }
    };

    class GBufferFlagsResolveGenericScene : public GBufferFlagsResolveScene
    {
      protected:
        void SetUp() override
        {
            m_SubjectUsesSnowKind = false;
            RendererAttachedTest::SetUp();
        }
    };

    TEST_F(GBufferFlagsResolveSnowScene, ResolvedMSAAKeepsSnowSilhouettesLit)
    {
        std::vector<u8> controlPx;
        std::vector<u8> resolvedPx;
        u32 w = 0;
        u32 h = 0;

        // Control first: no MSAA, so no resolve, so no averaged bitfield. Its
        // fringe count is this scene's noise floor and is what makes the
        // resolved count below mean anything.
        RenderDeferred(1u, false, "Snow_NoMSAA", controlPx, w, h);
        if (::testing::Test::HasFatalFailure())
            return;
        const u32 controlFringe = CountDarkFringePixels(controlPx, w, h);

        // The reported-broken configuration, exactly: MSAA on, per-sample off.
        RenderDeferred(4u, false, "Snow_MSAA4_Resolved", resolvedPx, w, h);
        if (::testing::Test::HasFatalFailure())
            return;
        const u32 resolvedFringe = CountDarkFringePixels(resolvedPx, w, h);

        // Non-vacuity: the frame has to contain a lit subject, or a black frame
        // would trivially have no fringes.
        ASSERT_GT(PeakLuminance(resolvedPx, w, h), 0.2f)
            << "the MSAA frame looks empty; nothing was measured. See "
            << VisualOutputPath("Snow_MSAA4_Resolved").string();

        // An averaged Snow/Generic edge can flip the low (unlit) bit.
        EXPECT_LE(resolvedFringe, controlFringe)
            << "MSAA 4x with per-sample lighting OFF produced " << resolvedFringe
            << " dark-fringe pixels against a non-MSAA control of " << controlFringe
            << " — the Snow silhouette is decoding as UNLIT and returning raw emissive. "
               "That is the averaged flags bitfield (issue #996): the resolve must exclude RT2's "
               "alpha, see GBuffer::ResolveFlagsLane / GBufferFlagsResolve.glsl. Re-run with "
               "OLO_GBUFFER_NO_FLAGS_RESOLVE=1 to see the defect on purpose. Evidence: "
            << VisualOutputPath("Snow_MSAA4_Resolved").string() << " vs "
            << VisualOutputPath("Snow_NoMSAA").string();
    }

    TEST_F(GBufferFlagsResolveGenericScene, GenericOnlySceneIsUnaffectedByTheFlagsResolve)
    {
        std::vector<u8> controlPx;
        std::vector<u8> resolvedPx;
        u32 w = 0;
        u32 h = 0;

        RenderDeferred(1u, false, "Generic_NoMSAA", controlPx, w, h);
        if (::testing::Test::HasFatalFailure())
            return;
        const u32 controlFringe = CountDarkFringePixels(controlPx, w, h);

        RenderDeferred(4u, false, "Generic_MSAA4_Resolved", resolvedPx, w, h);
        if (::testing::Test::HasFatalFailure())
            return;
        const u32 resolvedFringe = CountDarkFringePixels(resolvedPx, w, h);

        ASSERT_GT(PeakLuminance(resolvedPx, w, h), 0.2f)
            << "the Generic MSAA frame looks empty; nothing was measured. See "
            << VisualOutputPath("Generic_MSAA4_Resolved").string();

        EXPECT_LE(resolvedFringe, controlFringe)
            << "a Generic-only scene grew dark fringes under MSAA (" << resolvedFringe
            << " against a non-MSAA control of " << controlFringe
            << ") — the flags-lane resolve (GBuffer::ResolveFlagsLane, issue #996) is writing "
               "something other than a single sample's flags, or is not restoring the colour "
               "mask. Evidence: "
            << VisualOutputPath("Generic_MSAA4_Resolved").string();
    }
} // namespace OloEngine::Tests

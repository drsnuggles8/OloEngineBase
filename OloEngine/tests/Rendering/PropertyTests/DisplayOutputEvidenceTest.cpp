// OLO_TEST_LAYER: L8
// =============================================================================
// DisplayOutputEvidenceTest.cpp — the scene-to-display chain on the real
// pipeline (#1353), on both backends (the default process and
// `--olo-rhi=vulkan`), on every render path.
//
// A row of emissive patches of KNOWN radiance (black albedo, no lights, so a
// patch's scene value is its emissive factor) is rendered through the full
// Renderer3D pipeline with every spatial and temporal post effect off. Then:
//
//  * SDR cells: the composite is written as DisplayOutputSdr_<Backend>_<Path>.png
//    and its FNV-1a hash printed; the SDR chain must be bit-identical to the
//    pre-#1353 renderer, which the PR proves by re-running these cells with
//    master's two changed shaders swapped in (the C++ side of SDR declares
//    the same targets and only appends UBO bytes no SDR shader reads).
//  * HDR cells: every composite pixel must be the analytic CPU mapping of its
//    scene pixel (ToneMapForHdrDisplay -> NitsToComposition), and each known
//    patch must come out at its luminance below the BT.2390 knee and at most
//    at the display peak above it.
//  * Vulkan only: the PRESENTED image of the windowless target (HDR10 PQ in
//    A2B10G10R10, scRGB in RGBA16F) must be the CPU display encode of the
//    composite pixel by pixel, and each patch decodes back to its nits.
//
// OpenGL presents SDR only (OpenGLContext publishes the reason); its HDR cells
// publish an HDR selection by hand, which exercises the same tone-map shader
// and post-chain formats on GL as a parity check. That route is not one a GL
// user can reach, and the matrix says so.
// =============================================================================
#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"
#include "TestRenderBackend.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Accessibility/AccessibilitySettings.h"
#include "OloEngine/Renderer/DisplayOutput.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#if OLO_WITH_VULKAN
#include "Platform/Vulkan/VulkanContext.h"
#endif

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // The windowless Vulkan target is this size, and FinalRenderPass blits
        // the composite to it 1:1 only when the two agree.
        constexpr u32 kSize = TestRenderBackend::kWindowlessTargetSize;
        constexpr f32 kGamma = 2.2f;
        constexpr f32 kPaperWhite = 203.0f;
        constexpr f32 kUiWhite = 160.0f;
        constexpr f32 kPeak = 600.0f;

        // Scene radiance of each patch, in units of reference white: deep
        // shadow, mid-tones, paper white, and highlights up to 60x it. The last
        // two are colours, for the gamut and hue cases.
        struct Patch
        {
            glm::vec3 Emissive;
            const char* Name;
        };
        constexpr std::array<Patch, 10> kPatches{ {
            { glm::vec3(0.02f), "grey 0.02" },
            { glm::vec3(0.1f), "grey 0.1" },
            { glm::vec3(0.35f), "grey 0.35" },
            { glm::vec3(1.0f), "paper white" },
            { glm::vec3(1.6f), "grey 1.6" },
            { glm::vec3(4.0f), "grey 4" },
            { glm::vec3(12.0f), "grey 12" },
            { glm::vec3(60.0f), "grey 60" },
            { glm::vec3(6.0f, 0.0f, 0.0f), "red 6" },
            { glm::vec3(0.0f, 2.5f, 2.5f), "cyan 2.5" },
        } };
        // The first eight patches are grey: Rec.2020 grey is grey, so their
        // green channel is their luminance in either container.
        constexpr u32 kGreyPatchCount = 8u;
        constexpr f32 kSpacing = 0.9f;
        constexpr f32 kPatchSize = 0.8f;
        constexpr f32 kCameraDistance = 9.0f;
        constexpr f32 kFovDegrees = 60.0f;

        [[nodiscard]] f32 PatchX(u32 i)
        {
            return (static_cast<f32>(i) - 0.5f * static_cast<f32>(kPatches.size() - 1u)) * kSpacing;
        }

        // The pixel at a patch's centre, in GL (bottom-up) row order, which is
        // what the RHI-handle readbacks return on both backends.
        [[nodiscard]] std::pair<u32, u32> PatchPixel(u32 i)
        {
            // The patch's front face sits at z = +size/2, nearer than z = 0.
            const f32 depth = kCameraDistance - 0.5f * kPatchSize;
            const f32 halfWidth = depth * std::tan(glm::radians(kFovDegrees) * 0.5f);
            const f32 ndcX = PatchX(i) / halfWidth;
            return { static_cast<u32>((ndcX * 0.5f + 0.5f) * static_cast<f32>(kSize)), kSize / 2u };
        }

        [[nodiscard]] glm::vec3 At(const std::vector<f32>& rgba, u32 x, u32 y)
        {
            const sizet i = (static_cast<sizet>(y) * kSize + x) * 4u;
            return { rgba[i], rgba[i + 1], rgba[i + 2] };
        }

        [[nodiscard]] f32 MaxAbs(const glm::vec3& v)
        {
            return std::max({ std::abs(v.r), std::abs(v.g), std::abs(v.b) });
        }

        [[nodiscard]] u64 Fnv1a(const std::vector<u8>& bytes)
        {
            u64 h = 1469598103934665603ull;
            for (const u8 b : bytes)
            {
                h ^= b;
                h *= 1099511628211ull;
            }
            return h;
        }

        [[nodiscard]] std::string PathName(RenderingPath path)
        {
            switch (path)
            {
                case RenderingPath::Forward:
                    return "Forward";
                case RenderingPath::ForwardPlus:
                    return "ForwardPlus";
                case RenderingPath::Deferred:
                    return "Deferred";
                default:
                    return "Other";
            }
        }

        // Rows come back bottom-up; the PNG is written top row first.
        void WritePng(const std::string& name, const std::vector<u8>& rgba8)
        {
            const std::filesystem::path dir = "assets/tests/visual";
            std::filesystem::create_directories(dir);
            const auto file = dir / (TestRenderBackend::EvidenceName(name) + ".png");
            const int stride = static_cast<int>(kSize * 4u);
            const u8* topRow = rgba8.data() + static_cast<sizet>(kSize - 1u) * stride;
            ASSERT_NE(stbi_write_png(file.string().c_str(), static_cast<int>(kSize), static_cast<int>(kSize), 4, topRow, -stride), 0)
                << file.string();
        }

        // A composite as an 8-bit image for looking at: composition clipped to
        // [0, 1] (UI white), i.e. what an SDR view of the HDR frame shows.
        [[nodiscard]] std::vector<u8> ToRgba8(const std::vector<f32>& rgba)
        {
            std::vector<u8> out(rgba.size());
            for (sizet i = 0; i < rgba.size(); ++i)
                out[i] = static_cast<u8>(std::lround(std::clamp(rgba[i], 0.0f, 1.0f) * 255.0f));
            return out;
        }
    } // namespace

    class DisplayOutputEvidence : public RendererAttachedTest
    {
      protected:
        bool RunsInVulkanProcess() const override
        {
            return true;
        }

        void BuildScene() override
        {
            EnableRendering(kSize, kSize);
            Scene& scene = GetScene();
            scene.SetGridVisible(false);
            scene.SetWorldAxisHelperVisible(false);
            scene.SetLightGizmosVisible(false);
            scene.SetCameraFrustumsVisible(false);
            for (u32 i = 0; i < kPatches.size(); ++i)
            {
                Entity e = scene.CreateEntity(kPatches[i].Name);
                auto& tc = e.GetComponent<TransformComponent>();
                tc.Translation = { PatchX(i), 0.0f, 0.0f };
                tc.Scale = glm::vec3(kPatchSize);
                RunInFrame([&]
                           { e.AddComponent<MeshComponent>(MeshPrimitives::CreateCube()->GetMeshSource()); });
                auto& mat = e.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
                mat.m_Material.SetEmissiveFactor(glm::vec4(kPatches[i].Emissive, 1.0f));
            }

            // Every effect that moves a pixel by its neighbours or its history
            // off, so the composite is a per-pixel function of the scene.
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.Tonemap = TonemapOperator::Reinhard;
            pp.Exposure = 1.0f;
            pp.Gamma = kGamma;
            pp.AutoExposureEnabled = false;
            pp.BloomEnabled = false;
            pp.VignetteEnabled = false;
            pp.FXAAEnabled = false;
            pp.TAAEnabled = false;
            pp.DOFEnabled = false;
            pp.MotionBlurEnabled = false;
            pp.ChromaticAberrationEnabled = false;
            pp.ColorGradingEnabled = false;
            pp.SSAOEnabled = false;
            pp.GTAOEnabled = false;
            pp.SSREnabled = false;
            pp.SSGIEnabled = false;
            pp.CASEnabled = false;
        }

        void TearDown() override
        {
            // Process-global: an HDR mode left active would put every later
            // renderer test's post chain in RGBA16F.
            DisplayOutput::Reset();
            Accessibility::Reset();
            Renderer3D::GetPostProcessSettings().CASEnabled = false;
            RendererAttachedTest::TearDown();
        }

        // Selects the output mode the way each backend gets one: Vulkan through
        // the preference its windowless context re-selects on (the production
        // path), OpenGL by publishing the selection an HDR backend would make.
        void SelectMode(DisplayOutputMode mode)
        {
            DisplayOutputSettings s;
            s.Preference = mode == DisplayOutputMode::HDR10   ? DisplayOutputPreference::HDR10
                           : mode == DisplayOutputMode::ScRGB ? DisplayOutputPreference::ScRGB
                                                              : DisplayOutputPreference::SDR;
            s.PaperWhiteNits = kPaperWhite;
            s.UIWhiteNits = kUiWhite;
            s.PeakNitsOverride = kPeak;
            DisplayOutput::SetSettings(s);
            if (!TestRenderBackend::IsVulkanProcess())
            {
                DisplayCapabilities caps;
                caps.BackendHasHdrRoute = true;
                caps.OffersHDR10 = caps.OffersScRGB = true;
                DisplayOutputSelection selection;
                selection.Mode = mode;
                selection.Reason = "published by DisplayOutputEvidence (GL parity)";
                DisplayOutput::PublishActive(selection, caps);
            }
        }

        struct Capture
        {
            std::vector<f32> Scene;
            std::vector<f32> Composite;
        };

        // The display-range stages after the tone map, on: CAS sharpening and
        // colour-vision adaptation. Both used to clamp to 1.0 and must not in HDR.
        void EnableDisplayRangeStages()
        {
            Renderer3D::GetPostProcessSettings().CASEnabled = true;
            AccessibilitySettings a = Accessibility::Get();
            a.ColorBlind = ColorBlindMode::Deuteranopia;
            a.ColorBlindMethod = ColorBlindAdaptation::Correct;
            a.ColorBlindSeverity = 1.0f;
            Accessibility::Set(a);
        }

        // As Render, with CAS and colour-vision adaptation running: the
        // presented composite is ColorBlindColor, the last stage before the
        // backbuffer. Returns the final composite only.
        void RenderWithDisplayRangeStages(RenderingPath path, std::vector<f32>& scene, std::vector<f32>& finalComposite)
        {
            auto& renderer = Renderer3D::GetRendererSettings();
            renderer.Path = path;
            Renderer3D::ApplyRendererSettings();
            EditorCamera camera(kFovDegrees, 1.0f, 0.05f, 100.0f);
            camera.SetViewportSize(static_cast<f32>(kSize), static_cast<f32>(kSize));
            camera.SetPose({ 0.0f, 0.0f, kCameraDistance }, 0.0f, 0.0f);
            RunEditorFrames(camera, 3);
            // Resolving a name proves nothing about THIS frame (a name stays
            // resolvable from an earlier graph); the callers prove the stages ran
            // from the pixels instead (StagePixelsChanged).
            const auto adapted = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ColorBlindColor);
            const auto sceneFb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(adapted && sceneFb);
            ReadbackRgbaFloat(sceneFb->GetColorAttachmentHandle(0), kSize, kSize, scene);
            ReadbackRgbaFloat(adapted->GetColorAttachmentHandle(0), kSize, kSize, finalComposite);
            ASSERT_EQ(finalComposite.size(), static_cast<sizet>(kSize) * kSize * 4u);
        }

        // Pixels the display-range stages moved off the no-stage mapping: CAS
        // sharpens every patch edge and the adaptation remaps the colour
        // patches, so a frame where they ran this time has thousands.
        [[nodiscard]] static u32 StagePixelsChanged(const std::vector<f32>& scene, const std::vector<f32>& composite,
                                                    const DisplayMapping& mapping)
        {
            u32 changed = 0;
            for (u32 y = 0; y < kSize; ++y)
                for (u32 x = 0; x < kSize; ++x)
                {
                    const glm::vec3 s = glm::max(At(scene, x, y), glm::vec3(0.0f));
                    const glm::vec3 expected = mapping.IsHdr()
                                                   ? NitsToComposition(ToneMapForHdrDisplay(s, mapping, false), mapping, kGamma)
                                                   : glm::pow(s / (s + glm::vec3(1.0f)), glm::vec3(1.0f / kGamma));
                    changed += MaxAbs(At(composite, x, y) - expected) > 4.0f / 255.0f ? 1u : 0u;
                }
            return changed;
        }

        void Render(RenderingPath path, Capture& out)
        {
            // The per-pixel cells state their own stages: an earlier cell in
            // the process may have left sharpening or adaptation on.
            Renderer3D::GetPostProcessSettings().CASEnabled = false;
            Accessibility::Reset();
            auto& renderer = Renderer3D::GetRendererSettings();
            renderer.Path = path;
            Renderer3D::ApplyRendererSettings();
            EditorCamera camera(kFovDegrees, 1.0f, 0.05f, 100.0f);
            camera.SetViewportSize(static_cast<f32>(kSize), static_cast<f32>(kSize));
            camera.SetPose({ 0.0f, 0.0f, kCameraDistance }, 0.0f, 0.0f);
            RunEditorFrames(camera, 3);

            for (const auto name : { ResourceNames::BloomColor, ResourceNames::FXAAColor, ResourceNames::VignetteColor,
                                     ResourceNames::TAAColor })
                ASSERT_FALSE(Renderer3D::ResolveFrameGraphFramebuffer(name)) << name << " ran; the chain is not per-pixel";
            // No UpscalerColor check: after a cell that ran CAS that NAME keeps
            // resolving from the earlier graph. A sharpen that did run here
            // fails the SDR/HDR cells' per-pixel relations instead.

            const auto scene = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            const auto composite = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            ASSERT_TRUE(scene && composite);
            ReadbackRgbaFloat(scene->GetColorAttachmentHandle(0), kSize, kSize, out.Scene);
            ReadbackRgbaFloat(composite->GetColorAttachmentHandle(0), kSize, kSize, out.Composite);
            ASSERT_EQ(out.Composite.size(), static_cast<sizet>(kSize) * kSize * 4u);
        }

        // Each patch's centre carries exactly its emissive radiance: no light,
        // a black albedo, and nothing between the scene and the tone map.
        // A NaN pixel fails no `err > tolerance` comparison, so every image a
        // cell checks is first required to be finite.
        void ExpectAllFinite(const std::vector<f32>& rgba, const char* what)
        {
            u32 bad = 0;
            for (const f32 v : rgba)
                bad += std::isfinite(v) ? 0u : 1u;
            EXPECT_EQ(bad, 0u) << what << " holds non-finite channels";
        }

        void ExpectKnownScene(const Capture& c)
        {
            for (u32 i = 0; i < kPatches.size(); ++i)
            {
                const auto [x, y] = PatchPixel(i);
                const glm::vec3 scene = At(c.Scene, x, y);
                EXPECT_LE(MaxAbs(scene - kPatches[i].Emissive), 0.01f * MaxAbs(kPatches[i].Emissive) + 2.0e-3f)
                    << kPatches[i].Name << " at (" << x << "," << y << ") reads " << scene.r << "," << scene.g << "," << scene.b;
            }
        }

        void SdrCell(RenderingPath path)
        {
            SelectMode(DisplayOutputMode::SDR);
            Capture c;
            ASSERT_NO_FATAL_FAILURE(Render(path, c));
            ASSERT_FALSE(DisplayOutput::GetActiveMapping().IsHdr());
            ExpectAllFinite(c.Composite, "the SDR composite");
            ExpectKnownScene(c);
            // SDR: Reinhard, then 1/gamma, quantised to RGBA8 (+-0.5 LSB dither).
            u32 off = 0;
            for (u32 y = 0; y < kSize; ++y)
                for (u32 x = 0; x < kSize; ++x)
                {
                    const glm::vec3 s = glm::max(At(c.Scene, x, y), glm::vec3(0.0f));
                    const glm::vec3 expected = glm::pow(s / (s + glm::vec3(1.0f)), glm::vec3(1.0f / kGamma));
                    off += MaxAbs(At(c.Composite, x, y) - expected) > 1.6f / 255.0f ? 1u : 0u;
                }
            EXPECT_EQ(off, 0u) << "SDR composite pixels off the Reinhard curve";
            const std::vector<u8> rgba8 = ToRgba8(c.Composite);
            ASSERT_NO_FATAL_FAILURE(WritePng("DisplayOutputSdr_" + PathName(path), rgba8));
            std::cout << "[ DISPLAY ] " << TestRenderBackend::EvidenceName("DisplayOutputSdr_" + PathName(path))
                      << " fnv1a=" << std::hex << Fnv1a(rgba8) << std::dec << '\n';
        }

        void HdrCell(RenderingPath path, DisplayOutputMode mode)
        {
            SelectMode(mode);
            Capture c;
            ASSERT_NO_FATAL_FAILURE(Render(path, c));
            const DisplayMapping mapping = DisplayOutput::GetActiveMapping();
            ASSERT_EQ(mapping.Mode, mode) << "the backend did not select " << ToString(mode) << ": "
                                          << *DisplayOutput::GetActiveSelection().Reason;
            ASSERT_FLOAT_EQ(mapping.PeakNits, kPeak);
            ExpectAllFinite(c.Composite, "the HDR composite");
            ExpectKnownScene(c);

            // Every composite pixel is the CPU mapping of its scene pixel. The
            // tolerance is the deband dither (half an 8-bit LSB, in composition
            // units) plus RGBA16F's 11-bit mantissa.
            u32 off = 0;
            f32 worst = 0.0f;
            for (u32 y = 0; y < kSize; ++y)
                for (u32 x = 0; x < kSize; ++x)
                {
                    const glm::vec3 nits = ToneMapForHdrDisplay(At(c.Scene, x, y), mapping, false);
                    const glm::vec3 expected = NitsToComposition(nits, mapping, kGamma);
                    const f32 err = MaxAbs(At(c.Composite, x, y) - expected);
                    worst = std::max(worst, err);
                    off += err > 1.2f / 255.0f + 2.0e-3f * MaxAbs(expected) ? 1u : 0u;
                }
            EXPECT_EQ(off, 0u) << "HDR composite pixels off the CPU mapping (worst " << worst << ")";

            // The known-nit table: each patch's display luminance.
            const f32 kneeNits = PqDecodeToNits(1.5f * PqEncodeNits(kPeak) - 0.5f);
            const std::string cell = std::string("DisplayOutput") + ToString(mode) + "_" + PathName(path);
            for (u32 i = 0; i < kPatches.size(); ++i)
            {
                const auto [x, y] = PatchPixel(i);
                const glm::vec3 linear = glm::pow(glm::max(At(c.Composite, x, y), glm::vec3(0.0f)), glm::vec3(kGamma)) * kUiWhite;
                const f32 target = MaxAbs(kPatches[i].Emissive) * kPaperWhite;
                const f32 out = MaxAbs(linear);
                std::cout << "[ DISPLAY ] " << TestRenderBackend::EvidenceName(cell) << " patch '" << kPatches[i].Name
                          << "' scene " << target << " nits -> composite " << out << " nits\n";
                EXPECT_LE(out, kPeak * 1.01f) << kPatches[i].Name;
                if (target < kneeNits * 0.95f)
                    EXPECT_NEAR(out, target, target * 0.01f + 0.05f) << kPatches[i].Name << " is below the knee";
            }
            ASSERT_NO_FATAL_FAILURE(WritePng(cell, ToRgba8(c.Composite)));

#if OLO_WITH_VULKAN
            if (TestRenderBackend::IsVulkanProcess())
                ExpectPresentedEncode(c, mapping, cell);
#endif
        }

#if OLO_WITH_VULKAN
        // The image the display is sent: the windowless target's presentable
        // image, written by the display encode from the composition image.
        void ExpectPresentedEncode(const Capture& c, const DisplayMapping& mapping, const std::string& cell)
        {
            VulkanContext* context = VulkanContext::Get();
            ASSERT_NE(context, nullptr);
            std::vector<f32> presented;
            ReadbackRgbaFloat(context->GetPresentImageHandle(0), kSize, kSize, presented);
            ASSERT_EQ(presented.size(), c.Composite.size());
            ExpectAllFinite(presented, "the presented image");
            // HDR10 stores 10-bit PQ; scRGB stores half floats.
            const bool hdr10 = mapping.Mode == DisplayOutputMode::HDR10;
            u32 off = 0;
            f32 worst = 0.0f;
            for (u32 y = 0; y < kSize; ++y)
                for (u32 x = 0; x < kSize; ++x)
                {
                    const glm::vec3 expected = EncodeCompositionForDisplay(At(c.Composite, x, y), mapping, kGamma);
                    const f32 err = MaxAbs(At(presented, x, y) - expected);
                    worst = std::max(worst, err);
                    const f32 tolerance = hdr10 ? 1.0f / 1023.0f : 2.0e-3f * MaxAbs(expected) + 1.0e-3f;
                    off += err > tolerance ? 1u : 0u;
                }
            EXPECT_EQ(off, 0u) << "presented pixels off the CPU display encode (worst " << worst << ")";

            for (u32 i = 0; i < kPatches.size(); ++i)
            {
                const auto [x, y] = PatchPixel(i);
                const glm::vec3 v = At(presented, x, y);
                // Back to Rec.709 nits for the grey patches (Rec.2020 grey is grey).
                const f32 nits = hdr10 ? PqDecodeToNits(v.g) : v.g * kScRgbReferenceNits;
                const f32 composite = std::pow(std::max(At(c.Composite, x, y).g, 0.0f), kGamma) * kUiWhite;
                std::cout << "[ DISPLAY ] " << TestRenderBackend::EvidenceName(cell) << " presented patch '"
                          << kPatches[i].Name << "' green " << nits << " nits (composite " << composite << ")\n";
                if (i < kGreyPatchCount)
                    EXPECT_NEAR(nits, composite, composite * 0.01f + 0.05f) << kPatches[i].Name;
            }
            EXPECT_EQ(context->GetPresentFormat(), hdr10 ? 64u /*VK_FORMAT_A2B10G10R10_UNORM_PACK32*/ : 97u /*R16G16B16A16_SFLOAT*/);
        }
#endif
    };

    TEST_F(DisplayOutputEvidence, SdrForward)
    {
        SdrCell(RenderingPath::Forward);
    }
    TEST_F(DisplayOutputEvidence, SdrForwardPlus)
    {
        SdrCell(RenderingPath::ForwardPlus);
    }
    TEST_F(DisplayOutputEvidence, SdrDeferred)
    {
        SdrCell(RenderingPath::Deferred);
    }
    TEST_F(DisplayOutputEvidence, Hdr10Forward)
    {
        HdrCell(RenderingPath::Forward, DisplayOutputMode::HDR10);
    }
    TEST_F(DisplayOutputEvidence, Hdr10ForwardPlus)
    {
        HdrCell(RenderingPath::ForwardPlus, DisplayOutputMode::HDR10);
    }
    TEST_F(DisplayOutputEvidence, Hdr10Deferred)
    {
        HdrCell(RenderingPath::Deferred, DisplayOutputMode::HDR10);
    }
    TEST_F(DisplayOutputEvidence, ScRgbForward)
    {
        HdrCell(RenderingPath::Forward, DisplayOutputMode::ScRGB);
    }
    TEST_F(DisplayOutputEvidence, ScRgbForwardPlus)
    {
        HdrCell(RenderingPath::ForwardPlus, DisplayOutputMode::ScRGB);
    }
    TEST_F(DisplayOutputEvidence, ScRgbDeferred)
    {
        HdrCell(RenderingPath::Deferred, DisplayOutputMode::ScRGB);
    }

    // SDR with the display-range stages on: hashed for the master-shader A/B
    // (CAS, RCAS, colour-blind and tone map swapped back), so the stages this
    // branch touched are proven bit-identical in SDR too.
    TEST_F(DisplayOutputEvidence, SdrWithSharpenAndColourBlind)
    {
        SelectMode(DisplayOutputMode::SDR);
        EnableDisplayRangeStages();
        std::vector<f32> scene;
        std::vector<f32> composite;
        ASSERT_NO_FATAL_FAILURE(RenderWithDisplayRangeStages(RenderingPath::Forward, scene, composite));
        ExpectAllFinite(composite, "the stages composite");
        EXPECT_GT(StagePixelsChanged(scene, composite, DisplayOutput::GetActiveMapping()), 1000u)
            << "CAS and the adaptation did not run this frame";
        const std::vector<u8> rgba8 = ToRgba8(composite);
        ASSERT_NO_FATAL_FAILURE(WritePng("DisplayOutputSdrStages_Forward", rgba8));
        std::cout << "[ DISPLAY ] " << TestRenderBackend::EvidenceName("DisplayOutputSdrStages_Forward") << " fnv1a=" << std::hex
                  << Fnv1a(rgba8) << std::dec << '\n';
    }

    // HDR10 with CAS and colour-vision adaptation on: a grey highlight far
    // above UI white must reach the presented composite above UI white, still
    // grey. Both stages clamped to 1.0 before #1353, which cut it to UI white.
    TEST_F(DisplayOutputEvidence, Hdr10HighlightsSurviveSharpenAndColourBlind)
    {
        SelectMode(DisplayOutputMode::HDR10);
        EnableDisplayRangeStages();
        std::vector<f32> scene;
        std::vector<f32> composite;
        ASSERT_NO_FATAL_FAILURE(RenderWithDisplayRangeStages(RenderingPath::Forward, scene, composite));
        ExpectAllFinite(composite, "the stages composite");
        ASSERT_TRUE(DisplayOutput::GetActiveMapping().IsHdr());
        const DisplayMapping mapping = DisplayOutput::GetActiveMapping();
        EXPECT_GT(StagePixelsChanged(scene, composite, mapping), 1000u) << "CAS and the adaptation did not run this frame";
        // grey 12 (2436 scene nits) maps to ~569 nits: composition ~1.78.
        const auto [x, y] = PatchPixel(6);
        const glm::vec3 c = At(composite, x, y);
        const f32 expected = NitsToComposition(ToneMapForHdrDisplay(kPatches[6].Emissive, mapping, false), mapping, kGamma).g;
        std::cout << "[ DISPLAY ] " << TestRenderBackend::EvidenceName("DisplayOutputHDR10Stages_Forward")
                  << " grey-12 composite " << c.r << "," << c.g << "," << c.b << " (no stages: " << expected << ")\n";
        EXPECT_GT(std::min({ c.r, c.g, c.b }), 1.5f) << "a highlight was cut to UI white by a display-range stage";
        EXPECT_NEAR(c.g, expected, 0.02f * expected) << "CAS on a flat patch and the adaptation of a grey are the identity";
        EXPECT_LT(MaxAbs(c - glm::vec3(c.g)), 0.02f * c.g) << "the adaptation tinted a grey highlight";
        ASSERT_NO_FATAL_FAILURE(WritePng("DisplayOutputHDR10Stages_Forward", ToRgba8(composite)));
    }

    // A mode switch must not change scene transport: the scene-referred
    // radiance is identical with the output in SDR and in HDR10, and the
    // switch back restores the SDR composite exactly.
    TEST_F(DisplayOutputEvidence, ModeSwitchLeavesSceneTransportAlone)
    {
        SelectMode(DisplayOutputMode::SDR);
        Capture sdr;
        ASSERT_NO_FATAL_FAILURE(Render(RenderingPath::Forward, sdr));
        SelectMode(DisplayOutputMode::HDR10);
        Capture hdr;
        ASSERT_NO_FATAL_FAILURE(Render(RenderingPath::Forward, hdr));
        ASSERT_TRUE(DisplayOutput::GetActiveMapping().IsHdr());
        SelectMode(DisplayOutputMode::SDR);
        Capture back;
        ASSERT_NO_FATAL_FAILURE(Render(RenderingPath::Forward, back));
        ASSERT_FALSE(DisplayOutput::GetActiveMapping().IsHdr());

        u32 sceneDiffers = 0;
        u32 compositeDiffers = 0;
        for (u32 y = 0; y < kSize; ++y)
            for (u32 x = 0; x < kSize; ++x)
            {
                sceneDiffers += MaxAbs(At(sdr.Scene, x, y) - At(hdr.Scene, x, y)) > 0.0f ? 1u : 0u;
                compositeDiffers += MaxAbs(At(sdr.Composite, x, y) - At(back.Composite, x, y)) > 0.0f ? 1u : 0u;
            }
        EXPECT_EQ(sceneDiffers, 0u) << "the output mode reached the scene-referred radiance";
        EXPECT_EQ(compositeDiffers, 0u) << "returning to SDR did not restore the SDR composite";
    }
} // namespace OloEngine::Tests

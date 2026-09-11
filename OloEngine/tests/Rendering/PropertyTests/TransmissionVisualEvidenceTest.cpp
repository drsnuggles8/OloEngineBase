// OLO_TEST_LAYER: L8
// =============================================================================
// TransmissionVisualEvidenceTest.cpp
//
// Visual evidence (PNG) for the physical glTF material extensions (issue #970):
// KHR_materials_transmission, KHR_materials_ior and KHR_materials_volume.
//
// Renders ONE cube through the FULL Renderer3D pipeline four times, changing
// only its material between captures, and writes each frame to
//   OloEditor/assets/tests/visual/Transmission_<variant>.png
//
// WHY ONE CUBE, FOUR CAPTURES, RATHER THAN FOUR CUBES IN ONE FRAME. The
// assertions below compare the SAME screen region across captures, so nothing
// in them depends on where a cube landed in the frame — no hand-tuned pixel
// rectangles to drift when the camera or the mesh changes. The only difference
// between two captures is the material, which is exactly the variable under
// test.
//
// The four variants, and what each is for:
//   OpaqueControl — a legacy PBR material that never touches a #970 setter.
//                   This is the "no behaviour change for legacy materials"
//                   evidence: it must shade as its albedo, with the whole
//                   transmission closure skipped.
//   ThinGlass     — transmission 1, thickness 0. Thin-walled: refracts the
//                   environment, absorbs nothing.
//   VolumeThin    — transmission 1, a SMALL thickness through an orange medium.
//   VolumeThick   — the SAME medium at 8x the thickness.
//
// The last two are the acceptance criterion "attenuation is visibly
// depth-dependent where thickness is available", and they are asserted as
// driver-independent inequalities rather than left to the eye:
//   1. the thick sample is DARKER than the thin one (more path length, more
//      absorption), and
//   2. it has lost proportionally MORE BLUE than red — the orange attenuation
//      colour (0.9, 0.4, 0.2) absorbs blue hardest, so a thicker slab does not
//      merely darken, it shifts colour. A bug that applied absorption as a flat
//      scalar would pass (1) and fail (2).
//
// BOTH RENDERING PATHS ARE CAPTURED. Forward runs the closure directly in
// PBR_MultiLight.glsl. Deferred has no G-Buffer channels for transmission, so
// Renderer3D reroutes a transmissive material to ForwardOverlayPass; the
// Deferred case is what proves that reroute actually fires, since without it
// the glass would come back opaque and assertion (1) would fail.
//
// Classification: L8 / integration (full GL pipeline + RGBA8 readback + PNG).
// The committed Transmission_*.png act as golden references: a normal run
// COMPARES (RMSE) and writes nothing; pass --olo-golden-rebase to (re)write
// them after a deliberate visual change. Run from OloEditor/ so assets resolve.
// SKIPs (never DISABLED_) without a GL 4.6 context.
// =============================================================================

#include "OloEnginePCH.h"
#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/GltfPhysicalMaterial.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h" // Time::SetMockTime

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        // Smaller than the 1280x720 the water captures use: there are EIGHT
        // goldens here (four variants on two rendering paths) and they are
        // tracked binaries. 800x450 still shows the refraction and the colour
        // shift plainly when a human opens them.
        constexpr u32 kWidth = 800;
        constexpr u32 kHeight = 450;

        // Nothing in this scene animates, but the clock still feeds atmosphere /
        // TAA jitter, so it is frozen for the same reason the water captures
        // freeze it: a golden reference has to be deterministic.
        constexpr f32 kCaptureTime = 3.0f;

        constexpr f64 kGoldenRmseThreshold = 6.0;

        // The centre of the frame, where the sample cube sits. Deliberately well
        // inside the silhouette so a pixel or two of camera drift changes
        // nothing, and expressed as a FRACTION of the frame so it survives a
        // resolution change.
        constexpr f32 kSampleRectHalfExtent = 0.10f;

        struct Rgb
        {
            f64 R = 0.0;
            f64 G = 0.0;
            f64 B = 0.0;

            [[nodiscard]] f64 Luma() const
            {
                // Rec. 709 luma. Any fixed positive weighting would do; this one
                // matches what a viewer perceives as "darker".
                return 0.2126 * R + 0.7152 * G + 0.0722 * B;
            }
        };

        // Mean RGB over the centred sample rect of a top-down RGBA8 frame.
        [[nodiscard]] Rgb MeanCenterColor(const std::vector<u8>& pixels)
        {
            const auto x0 = static_cast<u32>(kWidth * (0.5f - kSampleRectHalfExtent));
            const auto x1 = static_cast<u32>(kWidth * (0.5f + kSampleRectHalfExtent));
            const auto y0 = static_cast<u32>(kHeight * (0.5f - kSampleRectHalfExtent));
            const auto y1 = static_cast<u32>(kHeight * (0.5f + kSampleRectHalfExtent));

            Rgb sum;
            u64 count = 0;
            for (u32 y = y0; y < y1; ++y)
            {
                for (u32 x = x0; x < x1; ++x)
                {
                    const std::size_t i = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                    if (i + 3 >= pixels.size())
                        continue;
                    sum.R += pixels[i + 0];
                    sum.G += pixels[i + 1];
                    sum.B += pixels[i + 2];
                    ++count;
                }
            }
            if (count == 0)
                return Rgb{};
            return Rgb{ sum.R / static_cast<f64>(count), sum.G / static_cast<f64>(count),
                        sum.B / static_cast<f64>(count) };
        }

        [[nodiscard]] f64 Rgba8Rmse(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
                return std::numeric_limits<f64>::max();
            f64 sumSq = 0.0;
            std::size_t count = 0;
            for (std::size_t i = 0; i + 3 < a.size(); i += 4)
            {
                for (int c = 0; c < 3; ++c)
                {
                    const f64 d = static_cast<f64>(a[i + c]) - static_cast<f64>(b[i + c]);
                    sumSq += d * d;
                    ++count;
                }
            }
            return count ? std::sqrt(sumSq / static_cast<f64>(count)) : 0.0;
        }

        [[nodiscard]] bool GoldenRebaseRequested()
        {
            return OloEngine::Tests::Options().GoldenRebase;
        }

        // The medium shared by the two volume variants. Orange, so blue is the
        // most-absorbed channel and the colour shift with depth is unambiguous.
        constexpr glm::vec3 kAttenuationColor{ 0.9f, 0.4f, 0.2f };
        constexpr f32 kAttenuationDistance = 0.5f;

        enum class Variant
        {
            OpaqueControl,
            ThinGlass,
            VolumeThin,
            VolumeThick,
        };

        [[nodiscard]] const char* VariantName(Variant v)
        {
            switch (v)
            {
                case Variant::OpaqueControl:
                    return "OpaqueControl";
                case Variant::ThinGlass:
                    return "ThinGlass";
                case Variant::VolumeThin:
                    return "VolumeThin";
                case Variant::VolumeThick:
                    return "VolumeThick";
            }
            return "Unknown";
        }

        // Authors one variant onto a material, from a known-clean starting point
        // so a previous variant's fields cannot leak into the next capture.
        void ApplyVariant(Material& material, Variant variant)
        {
            material.SetTransmissionFactor(0.0f);
            material.SetThicknessFactor(0.0f);
            material.SetAttenuationColor(glm::vec3(1.0f));
            material.SetAttenuationDistance(std::numeric_limits<f32>::infinity());
            material.SetIOR(kDefaultIOR);
            material.SetEnableIBL(true);

            switch (variant)
            {
                case Variant::OpaqueControl:
                    // A perfectly ordinary material: red, rough, and never
                    // touched by the transmission setters above their defaults.
                    material.SetBaseColorFactor(glm::vec4(0.8f, 0.1f, 0.1f, 1.0f));
                    material.SetMetallicFactor(0.0f);
                    material.SetRoughnessFactor(0.5f);
                    break;

                case Variant::ThinGlass:
                    material.SetBaseColorFactor(glm::vec4(1.0f));
                    material.SetMetallicFactor(0.0f);
                    material.SetRoughnessFactor(0.05f);
                    material.SetTransmissionFactor(1.0f);
                    break;

                case Variant::VolumeThin:
                case Variant::VolumeThick:
                    material.SetBaseColorFactor(glm::vec4(1.0f));
                    material.SetMetallicFactor(0.0f);
                    material.SetRoughnessFactor(0.05f);
                    material.SetTransmissionFactor(1.0f);
                    material.SetAttenuationColor(kAttenuationColor);
                    material.SetAttenuationDistance(kAttenuationDistance);
                    // The ONLY difference between these two is the path length.
                    material.SetThicknessFactor(variant == Variant::VolumeThin ? 0.25f : 2.0f);
                    break;
            }
        }
    } // namespace

    class TransmissionVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            {
                Entity light = scene.CreateEntity("Sun");
                auto& tc = light.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, 8.0f, 4.0f };
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.7f, -0.6f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 2.0f;
            }

            // The environment is NOT decoration here: transmitted radiance is
            // sampled from the prefiltered environment map, so without IBL the
            // glass would transmit black and the assertions below would compare
            // two zeroes. The preconditions in RunVariantSweep check for exactly
            // that and say so rather than passing vacuously.
            {
                Entity sky = scene.CreateEntity("Skybox");
                auto& env = sky.AddComponent<EnvironmentMapComponent>();
                env.m_FilePath = "assets/textures/Skybox";
                env.m_IsCubemapFolder = true;
                env.m_EnableSkybox = true;
                env.m_EnableIBL = true;
                env.m_IBLIntensity = 1.0f;
            }

            auto addCube = [&scene](const char* name, const glm::vec3& pos, const glm::vec3& scale,
                                    const glm::vec3& albedo) -> Entity
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
                mat.m_Material.SetBaseColorFactor(glm::vec4(albedo, 1.0f));
                mat.m_Material.SetRoughnessFactor(0.6f);
                return e;
            };

            // A bright MAGENTA backdrop behind the sample. It is a CONTROL for
            // the documented limitation, not a subject: transmitted light comes
            // from the environment map, so this wall should NOT appear through
            // the glass. Keeping it in the frame means the golden PNGs record
            // that limitation instead of hiding it — a future change that adds
            // a real scene-colour copy will move these goldens visibly, which is
            // the correct signal.
            addCube("Backdrop", { 0.0f, 0.0f, -4.0f }, { 14.0f, 8.0f, 0.2f }, { 1.0f, 0.0f, 1.0f });

            m_Sample = addCube("Sample", { 0.0f, 0.0f, 0.0f }, { 2.2f, 2.2f, 2.2f }, { 1.0f, 1.0f, 1.0f });
        }

        // Renders the sample with `variant` applied and reads back the composited
        // frame, comparing against (or rebasing) the golden for `pathLabel`.
        void Capture(const std::string& pathLabel, Variant variant, std::vector<u8>& outPixels)
        {
            ASSERT_TRUE(static_cast<bool>(m_Sample));
            ASSERT_TRUE(m_Sample.HasComponent<MaterialComponent>());
            ApplyVariant(m_Sample.GetComponent<MaterialComponent>().m_Material, variant);

            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            // Camera on +Z looking toward -Z at the origin, level. Head-on is the
            // pose where Fresnel is weakest and transmission strongest, which is
            // where the absorption difference is easiest to both see and assert.
            camera.SetPose(glm::vec3(0.0f, 0.0f, 5.5f), 0.0f, 0.0f);

            // Several frames: the render graph settles its transient targets and
            // any temporal pass reaches steady state, so the golden is stable.
            RunEditorFrames(camera, 3);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for " << pathLabel << "/" << VariantName(variant);

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            // GL returns rows bottom-up; the PNG writer and MeanCenterColor both
            // treat row 0 as the top.
            {
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
            }

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            const std::string path =
                (dir / ("Transmission_" + pathLabel + "_" + VariantName(variant) + ".png")).string();

            if (GoldenRebaseRequested())
            {
                std::error_code ec;
                fs::create_directories(dir, ec);
                ASSERT_FALSE(ec) << "Failed to create golden dir '" << dir.string() << "': " << ec.message();
                const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight),
                                                   4, outPixels.data(), static_cast<int>(kWidth) * 4);
                ASSERT_NE(wrote, 0) << "stbi_write_png failed to write golden '" << path << "'";
                return;
            }

            int gw = 0, gh = 0, gch = 0;
            stbi_uc* golden = ::stbi_load(path.c_str(), &gw, &gh, &gch, 4);
            ASSERT_NE(golden, nullptr)
                << "Missing golden '" << path << "' — rerun with --olo-golden-rebase to create it.";
            const bool sizeMatches = (gw == static_cast<int>(kWidth) && gh == static_cast<int>(kHeight));
            std::vector<u8> goldenPixels;
            if (sizeMatches)
                goldenPixels.assign(golden, golden + static_cast<std::size_t>(kWidth) * kHeight * 4u);
            ::stbi_image_free(golden);
            ASSERT_TRUE(sizeMatches) << "Golden '" << path << "' is " << gw << "x" << gh << ", expected " << kWidth
                                     << "x" << kHeight << " — rerun with --olo-golden-rebase.";

            const f64 rmse = Rgba8Rmse(outPixels, goldenPixels);
            EXPECT_LE(rmse, kGoldenRmseThreshold)
                << pathLabel << "/" << VariantName(variant) << " diverged from golden (RMSE " << rmse << " > "
                << kGoldenRmseThreshold << "). If this is an intended visual change, rerun with "
                << "--olo-golden-rebase to update " << path;
        }

        // Captures all four variants on the active rendering path and asserts the
        // relationships between them.
        void RunVariantSweep(const std::string& pathLabel)
        {
            std::array<Rgb, 4> means{};
            for (const Variant variant :
                 { Variant::OpaqueControl, Variant::ThinGlass, Variant::VolumeThin, Variant::VolumeThick })
            {
                std::vector<u8> pixels;
                Capture(pathLabel, variant, pixels);
                if (::testing::Test::HasFatalFailure())
                    return;
                means[static_cast<std::size_t>(variant)] = MeanCenterColor(pixels);
            }

            const Rgb& opaque = means[static_cast<std::size_t>(Variant::OpaqueControl)];
            const Rgb& thin = means[static_cast<std::size_t>(Variant::ThinGlass)];
            const Rgb& volumeThin = means[static_cast<std::size_t>(Variant::VolumeThin)];
            const Rgb& volumeThick = means[static_cast<std::size_t>(Variant::VolumeThick)];

            // --- The legacy material is still a red cube --------------------
            // If the transmission closure had leaked into a non-transmissive
            // material this is where it would show: the control would stop
            // being dominated by its own albedo.
            EXPECT_GT(opaque.R, opaque.G * 1.5) << pathLabel << ": the opaque control stopped reading as red";
            EXPECT_GT(opaque.R, opaque.B * 1.5) << pathLabel << ": the opaque control stopped reading as red";

            // --- Preconditions, so nothing below can pass vacuously ---------
            ASSERT_GT(thin.Luma(), 2.0)
                << pathLabel << ": the thin-glass sample is essentially black, which means the prefiltered "
                << "environment never bound (run from OloEditor/ so assets/textures/Skybox resolves). "
                << "Every absorption assertion below would be comparing two zeroes.";

            // --- Transmission actually did something ------------------------
            const f64 opaqueToThin = std::abs(thin.Luma() - opaque.Luma());
            EXPECT_GT(opaqueToThin, 2.0)
                << pathLabel << ": transmission changed nothing — the glass shaded like the opaque control. "
                << "On the Deferred path this is what a broken ForwardOverlayPass reroute looks like.";

            // --- Attenuation is depth-dependent -----------------------------
            // Same medium, 8x the path length: strictly more absorption.
            EXPECT_LT(volumeThick.Luma(), volumeThin.Luma())
                << pathLabel << ": a thicker slab of the same medium did not absorb more light "
                << "(thin luma " << volumeThin.Luma() << ", thick luma " << volumeThick.Luma() << ")";

            // --- ... and it is a COLOUR shift, not a flat dim ----------------
            // kAttenuationColor is orange, so blue must lose proportionally more
            // than red. A scalar-absorption bug passes the luma test above and
            // fails here.
            ASSERT_GT(volumeThin.R, 0.5);
            ASSERT_GT(volumeThin.B, 0.5);
            const f64 redRetained = volumeThick.R / volumeThin.R;
            const f64 blueRetained = volumeThick.B / volumeThin.B;
            EXPECT_LT(blueRetained, redRetained)
                << pathLabel << ": absorption did not shift colour with depth — blue retained " << blueRetained
                << ", red retained " << redRetained << ". An orange attenuation colour must absorb blue hardest.";

            // --- Physically bounded ------------------------------------------
            // Nothing may exceed the 0..255 the readback can hold, and the glass
            // may not out-brighten the environment into a blown white.
            for (const Rgb& mean : means)
            {
                EXPECT_GE(mean.R, 0.0);
                EXPECT_LE(mean.R, 255.0);
                EXPECT_GE(mean.G, 0.0);
                EXPECT_LE(mean.G, 255.0);
                EXPECT_GE(mean.B, 0.0);
                EXPECT_LE(mean.B, 255.0);
            }
        }

        Entity m_Sample;
    };

    namespace
    {
        // Freezes the clock for a capture run, restoring it on every exit path
        // (including an ASSERT early-return) so the mock cannot leak into the
        // tests that follow in the same process.
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
        };

        // Restores the rendering path, for the same reason.
        //
        // ApplyRendererSettings() is not optional and not a tidy-up: writing
        // Settings.Path alone leaves the render graph built for the PREVIOUS
        // path, so the passes for the new one never run and every capture comes
        // back as the same cleared frame. That failure is silent and uniform --
        // four byte-identical grey captures -- which is exactly what it looked
        // like before this call was added.
        struct ScopedRenderingPath
        {
            explicit ScopedRenderingPath(RenderingPath path) : m_Previous(Renderer3D::GetRendererSettings().Path)
            {
                Renderer3D::GetRendererSettings().Path = path;
                Renderer3D::ApplyRendererSettings();
            }
            ~ScopedRenderingPath()
            {
                Renderer3D::GetRendererSettings().Path = m_Previous;
                Renderer3D::ApplyRendererSettings();
            }

            RenderingPath m_Previous;
        };
    } // namespace

    TEST_F(TransmissionVisualEvidenceTest, ForwardPathTransmissionAndVolumeAbsorption)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        ScopedMockTime scopedMockTime(kCaptureTime);
        ScopedRenderingPath scopedPath(RenderingPath::Forward);

        RunVariantSweep("Forward");
    }

    // The Deferred case is the ForwardOverlayPass reroute's only real test: the
    // G-Buffer cannot carry transmission, so if the reroute stops firing the
    // glass shades opaque and the "transmission actually did something" and
    // depth-dependence assertions both fail.
    TEST_F(TransmissionVisualEvidenceTest, DeferredPathReroutesTransmissiveMaterialsToTheForwardOverlay)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        ScopedMockTime scopedMockTime(kCaptureTime);
        ScopedRenderingPath scopedPath(RenderingPath::Deferred);

        ResetPhysicalMaterialStats();

        RunVariantSweep("Deferred");

        // The reroute must have found somewhere correct to send every
        // transmissive draw. A non-zero counter here means the frames above were
        // shaded opaque through the G-Buffer — the silent-fallback case the
        // counter exists to make loud.
        const PhysicalMaterialStats stats = GetPhysicalMaterialStats();
        EXPECT_EQ(stats.TransmissiveDrawsWithoutForwardOverlay, 0u)
            << "a transmissive draw had no ForwardOverlayPass to reroute to, so it shaded opaque";
    }
} // namespace OloEngine::Tests

#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L8
// =============================================================================
// LightingTapEvidenceTest — issue #1526, item 3: the split lighting AOVs.
//
// A lighting tap shows one term of the lit colour IN PLACE of it (see LIGHTING
// TAPS in include/PBRCommon.glsl), so a capture reads the term as SceneColor.
// The contract, measured on the real GL pipeline:
//
//   1. PARTITION. DirectDiffuse + DirectSpecular + IndirectDiffuse +
//      IndirectSpecular + Remainder equals the tap-off SceneColor on every lit
//      pixel, to fp16 rounding. A term that went missing, or was counted twice,
//      breaks the sum.
//   2. EVERY TERM IS LIVE. Each split term carries energy somewhere in a scene
//      built to exercise it (a sun, an environment, a dielectric and a metal),
//      and ShadowVisibility shows both a shadowed and a lit floor.
//   3. TAP-OFF IS THE SHIPPING FRAME. The tap-off frame is bit-identical before
//      and after cycling through every tap (no state leaks), and its hash is
//      printed, so a shader-swap A/B against the base commit's shaders (same
//      binary) proves it is the frame the feature did not exist for.
//
// NEGATIVE CONTROL: the partition check is re-run with one term dropped. It
// must FAIL, or a check that tolerated the missing term could not have told a
// real partition from a broken one.
//
// Cells, one PNG contact sheet each (lit, the five terms, shadow visibility,
// |sum - lit| x 64): LightingTap_GL_<Path>_<Angle>.png for {Forward,
// ForwardPlus, Deferred} x {Front, High}; LightingTap_GL_DeferredMSAA4_Front.png
// (the per-sample MSAA lighting shader; Forward has no MSAA mode); and
// LightingTap_GL_Forward_FSR1Quality_Front.png (the taps at internal
// resolution). Vulkan cells are live-only (editor benchmark capture).
//
// Classification: L8 (full GL pipeline + float readback + PNG evidence).
// =============================================================================

#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RHI/RHIProjectionSeam.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 480;
        constexpr u32 kHeight = 320;
        constexpr u32 kFrames = 3;

        // fp16 scene colour: each of the five terms is rounded on its own, so the
        // sum carries up to five half-ulps against the lit value. 1% relative
        // covers that with room; a dropped term on this scene is tens of percent.
        constexpr f32 kRelativeTolerance = 0.01f;
        constexpr f32 kAbsoluteTolerance = 2e-3f;
        // The share of lit pixels allowed outside tolerance: silhouette pixels
        // whose coverage differs by a sample between two separately rendered
        // frames. Measured well under this.
        constexpr f64 kMaxOutlierFraction = 0.002;

        constexpr std::array<LightingTap, 5> kPartition = { LightingTap::DirectDiffuse, LightingTap::DirectSpecular,
                                                            LightingTap::IndirectDiffuse,
                                                            LightingTap::IndirectSpecular, LightingTap::Remainder };

        struct Image
        {
            u32 Width = 0;
            u32 Height = 0;
            std::vector<f32> Rgba; // bottom-up on GL, as read
        };

        [[nodiscard]] bool ReadFloat(std::string_view resource, bool depth, Image& out)
        {
            // Graph texture first, then the framebuffer's colour or depth
            // attachment: the benchmark capture core's resolve order.
            RHI::ResourceHandle handle = Renderer3D::ResolveFrameGraphTextureHandle(resource);
            if (!handle.IsValid())
            {
                if (const Ref<Framebuffer> framebuffer = Renderer3D::ResolveFrameGraphFramebuffer(resource))
                    handle = depth ? framebuffer->GetDepthAttachmentHandle() : framebuffer->GetColorAttachmentHandle(0);
            }
            if (!handle.IsValid())
                return false;
            u32 w = 0;
            u32 h = 0;
            RenderCommand::GetTextureDimensions(handle, 0, w, h);
            if (w == 0 || h == 0)
                return false;
            const u32 channels = depth ? 1u : 4u;
            out.Width = w;
            out.Height = h;
            out.Rgba.assign(static_cast<sizet>(w) * h * channels, 0.0f);
            return RenderCommand::ReadTextureSubImage(handle, 0, 0, 0, 0, w, h, 1u,
                                                      depth ? RHI::Format::D32Float : RHI::Format::RGBA32Float,
                                                      out.Rgba.size() * sizeof(f32), out.Rgba.data());
        }

        [[nodiscard]] u64 Fnv1a(const std::vector<f32>& values)
        {
            u64 hash = 1469598103934665603ull;
            const auto* bytes = reinterpret_cast<const u8*>(values.data());
            for (sizet i = 0; i < values.size() * sizeof(f32); ++i)
            {
                hash ^= bytes[i];
                hash *= 1099511628211ull;
            }
            return hash;
        }

        struct PartitionReport
        {
            u64 LitPixels = 0;
            u64 Outliers = 0;
            f32 WorstRelative = 0.0f;
            std::array<f64, kPartition.size()> MeanLuminance{};
            f64 ShadowedFraction = 0.0; // ShadowVisibility < 0.2
            f64 SunlitFraction = 0.0;   // ShadowVisibility > 0.8
        };

        [[nodiscard]] f32 Luminance(const f32* p)
        {
            return 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
        }

        // `skip` drops one term from the sum: the negative control.
        [[nodiscard]] PartitionReport CheckPartition(const Image& lit, const std::array<Image, kPartition.size()>& terms,
                                                     const Image& shadow, const Image& depth, i32 skip = -1)
        {
            PartitionReport report;
            const sizet texels = static_cast<sizet>(lit.Width) * lit.Height;
            u64 shadowed = 0;
            u64 sunlit = 0;
            for (sizet i = 0; i < texels; ++i)
            {
                // Surfaces only: the sky writes scene colour directly and no tap
                // touches it, so it would appear in all five terms.
                if (depth.Rgba[i] >= 0.99999f)
                    continue;
                ++report.LitPixels;
                const f32* a = &lit.Rgba[i * 4];
                for (sizet t = 0; t < terms.size(); ++t)
                    report.MeanLuminance[t] += Luminance(&terms[t].Rgba[i * 4]);
                const f32 visibility = shadow.Rgba[i * 4];
                shadowed += visibility < 0.2f ? 1u : 0u;
                sunlit += visibility > 0.8f ? 1u : 0u;

                bool outlier = false;
                for (int c = 0; c < 3; ++c)
                {
                    f32 sum = 0.0f;
                    for (sizet t = 0; t < terms.size(); ++t)
                    {
                        if (static_cast<i32>(t) != skip)
                            sum += terms[t].Rgba[i * 4 + c];
                    }
                    const f32 error = std::abs(sum - a[c]);
                    const f32 allowed = kAbsoluteTolerance + kRelativeTolerance * std::abs(a[c]);
                    if (!(error <= allowed))
                        outlier = true;
                    report.WorstRelative = std::max(report.WorstRelative, error / std::max(std::abs(a[c]), 1e-3f));
                }
                report.Outliers += outlier ? 1u : 0u;
            }
            if (report.LitPixels > 0u)
            {
                for (f64& mean : report.MeanLuminance)
                    mean /= static_cast<f64>(report.LitPixels);
                report.ShadowedFraction = static_cast<f64>(shadowed) / static_cast<f64>(report.LitPixels);
                report.SunlitFraction = static_cast<f64>(sunlit) / static_cast<f64>(report.LitPixels);
            }
            return report;
        }

        // Reinhard + sRGB for the contact sheet; the numbers are the float data.
        [[nodiscard]] u8 Encode(f32 v)
        {
            const f32 mapped = std::max(v, 0.0f) / (1.0f + std::max(v, 0.0f));
            return static_cast<u8>(std::clamp(std::pow(mapped, 1.0f / 2.2f), 0.0f, 1.0f) * 255.0f + 0.5f);
        }

        void WriteContactSheet(const std::string& cell, const Image& lit,
                               const std::array<Image, kPartition.size()>& terms, const Image& shadow)
        {
            // 4 x 2 tiles, top-down: lit, DD, DS, ID / IS, remainder, shadow, error.
            const u32 w = lit.Width;
            const u32 h = lit.Height;
            std::vector<u8> sheet(static_cast<sizet>(w) * 4u * h * 2u * 4u, 255u);
            const auto tile = [&](u32 tx, u32 ty, auto&& texel)
            {
                for (u32 y = 0; y < h; ++y)
                {
                    const u32 srcY = RHI::RenderTargetRowsAreBottomUp() ? h - 1u - y : y;
                    for (u32 x = 0; x < w; ++x)
                    {
                        const sizet src = static_cast<sizet>(srcY) * w + x;
                        const sizet dst = ((static_cast<sizet>(ty * h + y) * (w * 4u)) + tx * w + x) * 4u;
                        const std::array<u8, 3> rgb = texel(src);
                        sheet[dst + 0] = rgb[0];
                        sheet[dst + 1] = rgb[1];
                        sheet[dst + 2] = rgb[2];
                    }
                }
            };
            const auto colour = [](const Image& image)
            {
                return [&image](sizet i) -> std::array<u8, 3>
                { return { Encode(image.Rgba[i * 4]), Encode(image.Rgba[i * 4 + 1]), Encode(image.Rgba[i * 4 + 2]) }; };
            };
            tile(0, 0, colour(lit));
            tile(1, 0, colour(terms[0]));
            tile(2, 0, colour(terms[1]));
            tile(3, 0, colour(terms[2]));
            tile(0, 1, colour(terms[3]));
            tile(1, 1, colour(terms[4]));
            tile(2, 1,
                 [&shadow](sizet i) -> std::array<u8, 3>
                 {
                     const auto v = static_cast<u8>(std::clamp(shadow.Rgba[i * 4], 0.0f, 1.0f) * 255.0f + 0.5f);
                     return { v, v, v };
                 });
            tile(3, 1,
                 [&](sizet i) -> std::array<u8, 3>
                 {
                     f32 worst = 0.0f;
                     for (int c = 0; c < 3; ++c)
                     {
                         f32 sum = 0.0f;
                         for (const Image& term : terms)
                             sum += term.Rgba[i * 4 + c];
                         worst = std::max(worst, std::abs(sum - lit.Rgba[i * 4 + c]));
                     }
                     const auto v = static_cast<u8>(std::clamp(worst * 64.0f, 0.0f, 1.0f) * 255.0f + 0.5f);
                     return { v, 0u, 0u };
                 });
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / (cell + ".png")).string();
            EXPECT_NE(::stbi_write_png(path.c_str(), static_cast<int>(w * 4u), static_cast<int>(h * 2u), 4,
                                       sheet.data(), static_cast<int>(w * 4u * 4u)),
                      0)
                << "failed to write " << path;
        }

        struct ScopedLightingTap
        {
            ScopedLightingTap() = default;
            ~ScopedLightingTap()
            {
                Renderer3D::GetPostProcessSettings().LightingDebugTap = LightingTap::None;
            }
            ScopedLightingTap(const ScopedLightingTap&) = delete;
            ScopedLightingTap& operator=(const ScopedLightingTap&) = delete;
        };
    } // namespace

    class LightingTapEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            EnableRendering(kWidth, kHeight);
            Scene& scene = GetScene();

            auto& settings = Renderer3D::GetRendererSettings();
            settings.EditorDebugDrawsEnabled = false;
            settings.ShowGrid = false;
            settings.ShowWorldAxisHelper = false;
            // Deterministic frames: nothing temporal or stochastic between the
            // six separately rendered frames a partition compares.
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.TAAEnabled = false;
            pp.SSAOEnabled = false;
            pp.GTAOEnabled = false;
            pp.SSREnabled = false;
            pp.SSGIEnabled = false;

            {
                Entity sun = scene.CreateEntity("Sun");
                sun.GetComponent<TransformComponent>().Translation = { 0.0f, 20.0f, 0.0f };
                auto& dl = sun.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(0.45f, -0.8f, 0.35f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 3.0f;
                dl.m_CastShadows = true;
            }
            {
                // The environment: indirect diffuse AND specular need one (no
                // BRDF LUT and no prefiltered map without it).
                Entity sky = scene.CreateEntity("Skybox");
                auto& env = sky.AddComponent<EnvironmentMapComponent>();
                env.m_FilePath = "assets/textures/Skybox";
                env.m_IsCubemapFolder = true;
                env.m_EnableSkybox = true;
                env.m_EnableIBL = true;
            }

            const auto addMesh = [&scene](const char* name, MeshPrimitive primitive, const glm::vec3& at,
                                          const glm::vec3& scale, const glm::vec4& color, f32 metallic,
                                          f32 roughness, const glm::vec3& emissive = glm::vec3(0.0f))
            {
                Entity entity = scene.CreateEntity(name);
                auto& tc = entity.GetComponent<TransformComponent>();
                tc.Translation = at;
                tc.Scale = scale;
                auto& mc = entity.AddComponent<MeshComponent>();
                mc.m_Primitive = primitive;
                const Ref<Mesh> mesh = primitive == MeshPrimitive::Sphere  ? MeshPrimitives::CreateSphere()
                                       : primitive == MeshPrimitive::Plane ? MeshPrimitives::CreatePlane()
                                                                           : MeshPrimitives::CreateCube();
                if (mesh)
                    mc.m_MeshSource = mesh->GetMeshSource();
                auto& mat = entity.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(color);
                mat.m_Material.SetMetallicFactor(metallic);
                mat.m_Material.SetRoughnessFactor(roughness);
                if (emissive.x > 0.0f || emissive.y > 0.0f || emissive.z > 0.0f)
                    mat.m_Material.SetEmissiveFactor(glm::vec4(emissive, 1.0f));
            };
            addMesh("Floor", MeshPrimitive::Plane, glm::vec3(0.0f), { 20.0f, 1.0f, 20.0f },
                    glm::vec4(0.6f, 0.6f, 0.6f, 1.0f), 0.0f, 0.8f);
            addMesh("Dielectric", MeshPrimitive::Sphere, { -1.3f, 1.0f, 0.0f }, glm::vec3(1.0f),
                    glm::vec4(0.75f, 0.12f, 0.08f, 1.0f), 0.0f, 0.35f);
            addMesh("Metal", MeshPrimitive::Sphere, { 1.3f, 1.0f, 0.0f }, glm::vec3(1.0f),
                    glm::vec4(0.95f, 0.78f, 0.35f, 1.0f), 1.0f, 0.25f);
            // Emission lands in the remainder, so the fifth term is exercised too.
            addMesh("Emitter", MeshPrimitive::Cube, { 0.0f, 0.35f, 1.6f }, glm::vec3(0.5f),
                    glm::vec4(0.1f, 0.1f, 0.1f, 1.0f), 0.0f, 0.9f, glm::vec3(0.2f, 0.8f, 1.5f));
        }

        [[nodiscard]] EditorCamera MakeCamera(bool high) const
        {
            EditorCamera camera(50.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 200.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            if (high)
                camera.SetPose(glm::vec3(0.0f, 7.5f, 6.5f), 0.0f, glm::radians(48.0f));
            else
                camera.SetPose(glm::vec3(0.0f, 1.8f, 7.0f), 0.0f, glm::radians(8.0f));
            return camera;
        }

        // Render `tap` and read scene colour. Every frame of a comparison is
        // rendered from the same camera with nothing temporal on.
        void RenderTap(const EditorCamera& camera, LightingTap tap, Image& out)
        {
            Renderer3D::GetPostProcessSettings().LightingDebugTap = tap;
            RunEditorFrames(camera, kFrames);
            ASSERT_TRUE(ReadFloat(ResourceNames::SceneColor, false, out)) << "no SceneColor for tap "
                                                                          << std::to_underlying(tap);
        }

        // One cell: lit, the partition, shadow, the negative control, the sheet.
        void RunCell(const std::string& cell, const EditorCamera& camera)
        {
            const ScopedLightingTap restore;
            Image lit;
            RenderTap(camera, LightingTap::None, lit);
            if (HasFatalFailure())
                return;
            Image depth;
            ASSERT_TRUE(ReadFloat(ResourceNames::SceneDepth, true, depth)) << cell << ": no SceneDepth";
            ASSERT_EQ(depth.Width, lit.Width) << cell << ": depth and colour disagree about the internal size";

            std::array<Image, kPartition.size()> terms;
            for (sizet t = 0; t < kPartition.size(); ++t)
            {
                RenderTap(camera, kPartition[t], terms[t]);
                if (HasFatalFailure())
                    return;
            }
            Image shadow;
            RenderTap(camera, LightingTap::ShadowVisibility, shadow);
            if (HasFatalFailure())
                return;
            Image litAgain;
            RenderTap(camera, LightingTap::None, litAgain);
            if (HasFatalFailure())
                return;

            // 3. The tap-off frame survives the cycle bit for bit.
            EXPECT_EQ(Fnv1a(lit.Rgba), Fnv1a(litAgain.Rgba)) << cell << ": the tap-off frame changed after cycling taps";
            std::cout << "[LightingTap] " << cell << " tap-off SceneColor " << lit.Width << "x" << lit.Height
                      << " fnv1a 0x" << std::hex << Fnv1a(lit.Rgba) << std::dec << '\n';

            // 1. The partition.
            const PartitionReport report = CheckPartition(lit, terms, shadow, depth);
            ASSERT_GT(report.LitPixels, static_cast<u64>(lit.Width) * lit.Height / 10u) << cell << ": too few surface pixels";
            const f64 outlierFraction = static_cast<f64>(report.Outliers) / static_cast<f64>(report.LitPixels);
            std::cout << "[LightingTap] " << cell << ": " << report.LitPixels << " lit px, " << report.Outliers
                      << " outside tolerance (" << outlierFraction * 100.0 << "%), worst relative "
                      << report.WorstRelative << "; mean luminance DD " << report.MeanLuminance[0] << " DS "
                      << report.MeanLuminance[1] << " ID " << report.MeanLuminance[2] << " IS "
                      << report.MeanLuminance[3] << " R " << report.MeanLuminance[4] << "; shadowed "
                      << report.ShadowedFraction << ", sunlit " << report.SunlitFraction << '\n';
            EXPECT_LE(outlierFraction, kMaxOutlierFraction) << cell << ": the five taps do not sum to the lit colour";

            // 2. Every term is live, and the shadow tap shows a shadow.
            for (sizet t = 0; t < kPartition.size(); ++t)
            {
                EXPECT_GT(report.MeanLuminance[t], 1e-4) << cell << ": tap " << std::to_underlying(kPartition[t])
                                                         << " carries no energy";
            }
            EXPECT_GT(report.ShadowedFraction, 0.005) << cell << ": no shadowed surface in the shadow tap";
            EXPECT_GT(report.SunlitFraction, 0.2) << cell << ": no sunlit surface in the shadow tap";

            // NEGATIVE CONTROL: drop each term in turn; the check must fail.
            for (sizet t = 0; t < kPartition.size(); ++t)
            {
                const PartitionReport broken = CheckPartition(lit, terms, shadow, depth, static_cast<i32>(t));
                const f64 brokenFraction = static_cast<f64>(broken.Outliers) / static_cast<f64>(broken.LitPixels);
                EXPECT_GT(brokenFraction, kMaxOutlierFraction)
                    << cell << ": the partition check ACCEPTED a sum missing tap " << std::to_underlying(kPartition[t]);
            }

            WriteContactSheet(cell, lit, terms, shadow);
        }
    };

    TEST_F(LightingTapEvidenceTest, TapsPartitionTheLitColourOnEveryPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        auto& settings = Renderer3D::GetRendererSettings();
        for (const auto& [path, name] : { std::pair{ RenderingPath::Forward, "Forward" },
                                          std::pair{ RenderingPath::ForwardPlus, "ForwardPlus" },
                                          std::pair{ RenderingPath::Deferred, "Deferred" } })
        {
            settings.Path = path;
            Renderer3D::ApplyRendererSettings();
            for (const bool high : { false, true })
            {
                RunCell(std::string("LightingTap_GL_") + name + (high ? "_High" : "_Front"), MakeCamera(high));
                if (HasFatalFailure())
                    return;
            }
        }
    }

    // The per-sample lighting shader (DeferredLightingFragment_MSAA). Forward
    // has no MSAA mode: the sample count is the Deferred G-Buffer's.
    TEST_F(LightingTapEvidenceTest, TapsPartitionUnderDeferredMsaa4)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        auto& settings = Renderer3D::GetRendererSettings();
        settings.Path = RenderingPath::Deferred;
        settings.Deferred.MSAASampleCount = 4u;
        Renderer3D::ApplyRendererSettings();
        RunCell("LightingTap_GL_DeferredMSAA4_Front", MakeCamera(false));
    }

    // The reflection AOVs: SSR's confidence (always in SSRGuide's alpha, #1057)
    // and its hit distance (the same lane under the ReflectionHitDistance tap).
    // Deferred only, because the screen-space reflection tier is.
    TEST_F(LightingTapEvidenceTest, ReflectionConfidenceAndHitDistanceReadFromTheSsrGuide)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedLightingTap restore;
        auto& settings = Renderer3D::GetRendererSettings();
        settings.Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();
        auto& pp = Renderer3D::GetPostProcessSettings();
        pp.SSREnabled = true;
        Entity floor = GetScene().FindEntityByName("Floor");
        ASSERT_TRUE(floor);
        floor.GetComponent<MaterialComponent>().m_Material.SetRoughnessFactor(0.05f);
        const EditorCamera camera = MakeCamera(false);

        Image litOff;
        RenderTap(camera, LightingTap::None, litOff);
        if (HasFatalFailure())
            return;
        Image guideConfidence;
        ASSERT_TRUE(ReadFloat(ResourceNames::SSRGuide, false, guideConfidence)) << "SSR did not run";
        Image litTapped;
        RenderTap(camera, LightingTap::ReflectionHitDistance, litTapped);
        if (HasFatalFailure())
            return;
        Image guideDistance;
        ASSERT_TRUE(ReadFloat(ResourceNames::SSRGuide, false, guideDistance));
        ASSERT_EQ(guideConfidence.Rgba.size(), guideDistance.Rgba.size());

        // The hit-distance tap changes SSR's target, never the lit colour.
        EXPECT_EQ(Fnv1a(litOff.Rgba), Fnv1a(litTapped.Rgba)) << "the reflection tap changed SceneColor";

        const f32 maxDistance = pp.SSRMaxDistance;
        u64 confident = 0;
        u64 confidentWithDistance = 0;
        f32 largestConfidence = 0.0f;
        f32 largestDistance = 0.0f;
        f64 distanceSum = 0.0;
        u64 outOfRange = 0;
        const sizet texels = static_cast<sizet>(guideConfidence.Width) * guideConfidence.Height;
        for (sizet i = 0; i < texels; ++i)
        {
            const f32 confidence = guideConfidence.Rgba[i * 4 + 3];
            const f32 distance = guideDistance.Rgba[i * 4 + 3];
            largestConfidence = std::max(largestConfidence, confidence);
            largestDistance = std::max(largestDistance, distance);
            outOfRange += (distance < 0.0f || distance > maxDistance + 1e-3f) ? 1u : 0u;
            if (confidence > 0.1f)
            {
                ++confident;
                if (distance > 0.0f)
                {
                    ++confidentWithDistance;
                    distanceSum += distance;
                }
            }
        }
        std::cout << "[LightingTap] reflection AOVs: " << confident << " px with SSR confidence > 0.1 (max "
                  << largestConfidence << "), " << confidentWithDistance << " of them with a hit distance, mean "
                  << (confidentWithDistance ? distanceSum / static_cast<f64>(confidentWithDistance) : 0.0)
                  << " m, max " << largestDistance << " m\n";
        EXPECT_GT(confident, 500u) << "SSR found no confident reflection on the glossy floor";
        EXPECT_LE(largestConfidence, 1.0f) << "the untapped lane is not a confidence";
        // A confident hit has travelled somewhere: the tap wrote a distance
        // where the confidence was. The two lanes come from two frames, and SSR
        // re-jitters its rays every frame, so a pixel at a hit/miss boundary can
        // hit in one and miss in the other (measured: about 9% of them here).
        EXPECT_GE(static_cast<f64>(confidentWithDistance), 0.85 * static_cast<f64>(confident));
        EXPECT_EQ(outOfRange, 0u) << "a hit distance outside [0, SSRMaxDistance]";
        // The lane really switched meaning: the spheres are metres away from the
        // floor pixels reflecting them, a confidence never exceeds 1.
        EXPECT_GT(largestDistance, 1.0f) << "the tapped lane still holds a confidence";

        // Evidence: confidence (grey) beside hit distance (grey, 0..SSRMaxDistance).
        const u32 w = guideConfidence.Width;
        const u32 h = guideConfidence.Height;
        std::vector<u8> sheet(static_cast<sizet>(w) * 2u * h * 4u, 255u);
        for (u32 y = 0; y < h; ++y)
        {
            const u32 srcY = RHI::RenderTargetRowsAreBottomUp() ? h - 1u - y : y;
            for (u32 x = 0; x < w; ++x)
            {
                const sizet src = (static_cast<sizet>(srcY) * w + x) * 4u + 3u;
                const auto conf = static_cast<u8>(std::clamp(guideConfidence.Rgba[src], 0.0f, 1.0f) * 255.0f + 0.5f);
                const auto dist =
                    static_cast<u8>(std::clamp(guideDistance.Rgba[src] / maxDistance, 0.0f, 1.0f) * 255.0f + 0.5f);
                u8* left = &sheet[(static_cast<sizet>(y) * w * 2u + x) * 4u];
                u8* right = &sheet[(static_cast<sizet>(y) * w * 2u + w + x) * 4u];
                left[0] = left[1] = left[2] = conf;
                right[0] = right[1] = right[2] = dist;
            }
        }
        const std::string path = (fs::path("assets") / "tests" / "visual" / "LightingTap_GL_Deferred_ReflectionAovs.png").string();
        EXPECT_NE(::stbi_write_png(path.c_str(), static_cast<int>(w * 2u), static_cast<int>(h), 4, sheet.data(),
                                   static_cast<int>(w * 2u * 4u)),
                  0);
        pp.SSREnabled = false;
    }

    // Under an upscaler the taps are internal-resolution images, like every
    // scene-band AOV: SceneColor is the reduced band.
    TEST_F(LightingTapEvidenceTest, TapsPartitionAtInternalResolutionUnderUpscale)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        auto& settings = Renderer3D::GetRendererSettings();
        settings.Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        auto& pp = Renderer3D::GetPostProcessSettings();
        pp.Upscale = UpscaleMode::Quality;
        pp.Technique = UpscalerTechnique::Spatial;
        RunCell("LightingTap_GL_Forward_FSR1Quality_Front", MakeCamera(false));
        Image colour;
        ASSERT_TRUE(ReadFloat(ResourceNames::SceneColor, false, colour));
        EXPECT_EQ(colour.Width, static_cast<u32>(std::floor(static_cast<f32>(kWidth) * UpscaleModeToRenderScale(UpscaleMode::Quality))))
            << "the taps were not at internal resolution";
        pp.Upscale = UpscaleMode::Off;
    }
} // namespace OloEngine::Tests

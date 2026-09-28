// OLO_TEST_LAYER: L8
// =============================================================================
// ReflectionTierTermEvidenceTest.cpp -- issue #1325 on the production path.
//
// A reflection tier replaces the indirect SPECULAR TERM, not the frame colour
// (ADR 0020 section 1a). The old SSR composite was mix(C, L * tint, blend) over
// the whole lit colour, so a black reflected object dimmed the diffuse, the
// emission and the direct light under it by the same factor.
//
// The scene: a bright, sunlit, slightly emissive DIELECTRIC floor -- its colour
// is almost all diffuse, and its indirect specular term S is small -- under a
// BLACK block (no albedo, no emission). SSR finds the block in the floor and
// its radiance there is zero. Through the full GL deferred pipeline, in ONE
// frame, the test reads SSR's input colour, SSR's output and the S the lighting
// exported, over the floor band the block's reflection lands in. There:
//
//   * what SSR removes is at most S -- the unrelated terms keep their value;
//   * and it removes a real part of S -- the reflection did land.
//
// The old composite fails the first by the floor's whole diffuse term times
// its blend. PNGs from three poses go to assets/tests/visual/.
//
// Transient aliasing is off for the fixture: a post-frame read of a mid-graph
// target would otherwise see a later pass's image under the same texture.
// SKIPs without a GL 4.6 context.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Core/DebugLevers.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 640;
        constexpr u32 kHeight = 480;
        constexpr f32 kCaptureTime = 4.0f;

        struct LinearFrame
        {
            std::vector<f32> Pixels; // RGBA32F, GL row order (bottom-up)
            u32 Width = 0;
            u32 Height = 0;
        };

        [[nodiscard]] bool ReadAttachment(std::string_view resource, u32 attachment, LinearFrame& out)
        {
            const Ref<Framebuffer> fb = Renderer3D::ResolveFrameGraphFramebuffer(resource);
            if (!fb)
                return false;
            out.Width = fb->GetSpecification().Width;
            out.Height = fb->GetSpecification().Height;
            ReadbackRgbaFloat(fb->GetColorAttachmentRendererID(attachment), out.Width, out.Height, out.Pixels);
            return out.Pixels.size() == static_cast<sizet>(out.Width) * out.Height * 4u;
        }

        [[nodiscard]] f64 Luma(const LinearFrame& f, sizet pixel)
        {
            return 0.2126 * f.Pixels[pixel * 4u] + 0.7152 * f.Pixels[pixel * 4u + 1u] +
                   0.0722 * f.Pixels[pixel * 4u + 2u];
        }
    } // namespace

    class ReflectionTierTermEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);
            m_AliasingWas = Levers::DisableTransientAliasing();
            Levers::SetDisableTransientAliasing(true);
            // Scene only: the editor grid and gizmos are drawn into the colour too.
            auto& settings = Renderer3D::GetRendererSettings();
            m_GridWas = settings.ShowGrid;
            m_GizmosWas = settings.ShowComponentGizmos;
            settings.ShowGrid = false;
            settings.ShowComponentGizmos = false;

            // SSR runs on the deferred path only.
            Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
            Renderer3D::ApplyRendererSettings();

            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.3f, -0.9f, -0.2f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 3.0f;
            }

            // An environment, so the lighting HAS an indirect specular term for
            // SSR to replace: with none, S is zero and a black hit changes nothing.
            {
                Entity sky = scene.CreateEntity("Skybox");
                auto& env = sky.AddComponent<EnvironmentMapComponent>();
                env.m_FilePath = "assets/textures/Skybox";
                env.m_IsCubemapFolder = true;
                env.m_EnableSkybox = true;
                env.m_EnableIBL = true;
            }

            const auto addMesh = [&scene](const char* name, bool plane, const glm::vec3& pos, const glm::vec3& scale)
            {
                Entity e = scene.CreateEntity(name);
                auto& tc = e.GetComponent<TransformComponent>();
                tc.Translation = pos;
                tc.Scale = scale;
                auto& mc = e.AddComponent<MeshComponent>();
                mc.m_Primitive = plane ? MeshPrimitive::Plane : MeshPrimitive::Cube;
                const Ref<Mesh> mesh = plane ? MeshPrimitives::CreatePlane() : MeshPrimitives::CreateCube();
                if (mesh)
                    mc.m_MeshSource = mesh->GetMeshSource();
                return e;
            };

            // The floor: diffuse-dominated, glossy enough for SSR, with a little
            // emission so the emission term is in the colour too.
            {
                Entity floor = addMesh("Floor", true, { 0.0f, 0.0f, 0.0f }, { 60.0f, 1.0f, 60.0f });
                auto& mat = floor.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.8f, 0.8f, 0.8f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(0.05f);
                mat.m_Material.SetEmissiveFactor(glm::vec4(0.05f, 0.05f, 0.05f, 1.0f));
            }
            // The black block: reflects no light at all.
            {
                Entity block = addMesh("BlackBlock", false, { 0.0f, 2.0f, -7.0f }, { 6.0f, 4.0f, 1.0f });
                auto& mat = block.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
                mat.m_Material.SetEmissiveFactor(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
            }
        }

        void TearDown() override
        {
            Levers::SetDisableTransientAliasing(m_AliasingWas);
            Renderer3D::GetRendererSettings().ShowGrid = m_GridWas;
            Renderer3D::GetRendererSettings().ShowComponentGizmos = m_GizmosWas;
            RendererAttachedTest::TearDown();
        }

        struct Band
        {
            f64 Input = 0.0;    // SSR's input colour, luma
            f64 Output = 0.0;   // SSR's output colour, luma
            f64 Specular = 0.0; // S the lighting exported, luma
            u32 Pixels = 0;
            f64 WorstExcess = 0.0; // max over pixels of (removed - S) / input
        };

        // One pose: converge the temporal resolve, then read the frame's SSR input,
        // SSR output and exported S over the band [y0, y1] x [x0, x1] (UV, top-down).
        void Measure(const char* pose, const glm::vec3& eye, f32 pitch, f32 x0, f32 x1, f32 y0, f32 y1, Band& band)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(eye, 0.0f, pitch);
            RunEditorFrames(camera, 24);

            LinearFrame output;
            LinearFrame specular;
            ASSERT_TRUE(ReadAttachment(ResourceNames::SSRColor, 0u, output)) << "SSR did not run";
            ASSERT_TRUE(ReadAttachment(ResourceNames::IndirectSpecular, 0u, specular))
                << "the lighting did not export the indirect specular term";
            // SSR's input, by the pass's own fallback order.
            LinearFrame input;
            bool haveInput = false;
            for (const std::string_view name :
                 { ResourceNames::RTReflectionColor, ResourceNames::SSGIColor, ResourceNames::AOApplyColor,
                   ResourceNames::SceneColor })
            {
                if (ReadAttachment(name, 0u, input))
                {
                    haveInput = true;
                    break;
                }
            }
            ASSERT_TRUE(haveInput);
            ASSERT_EQ(input.Width, output.Width);
            ASSERT_EQ(specular.Width, output.Width);

            const u32 w = output.Width;
            const u32 h = output.Height;
            for (u32 row = static_cast<u32>(y0 * h); row < static_cast<u32>(y1 * h); ++row)
            {
                const u32 glRow = h - 1u - row;
                for (u32 x = static_cast<u32>(x0 * w); x < static_cast<u32>(x1 * w); ++x)
                {
                    const sizet p = static_cast<sizet>(glRow) * w + x;
                    const f64 in = Luma(input, p);
                    const f64 out = Luma(output, p);
                    const f64 s = Luma(specular, p);
                    band.Input += in;
                    band.Output += out;
                    band.Specular += s;
                    ++band.Pixels;
                    if (in > 1.0e-3)
                        band.WorstExcess = std::max(band.WorstExcess, ((in - out) - s) / in);
                }
            }

            // The composited frame, for looking at.
            auto composite = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!composite)
                composite = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (composite)
            {
                std::vector<u8> ldr;
                ReadbackRgba8(composite->GetColorAttachmentRendererID(0), kWidth, kHeight, ldr);
                const fs::path dir = fs::path("assets") / "tests" / "visual";
                std::error_code ec;
                fs::create_directories(dir, ec);
                const std::string path = (dir / (std::string("ReflectionTierTerm_GL_Deferred_") + pose + ".png")).string();
                (void)::stbi_flip_vertically_on_write(1);
                (void)::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4, ldr.data(),
                                       static_cast<int>(kWidth) * 4);
                (void)::stbi_flip_vertically_on_write(0);
            }
        }

        bool m_AliasingWas = false;
        bool m_GridWas = true;
        bool m_GizmosWas = true;
    };

    TEST_F(ReflectionTierTermEvidenceTest, SSRReplacesTheSpecularTermAndLeavesTheRestOfTheColour)
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

        auto& pp = Renderer3D::GetPostProcessSettings();
        pp.SSREnabled = true;
        pp.SSRIntensity = 1.0f;
        pp.SSRMaxRoughness = 0.6f;
        pp.SSRMaxDistance = 60.0f;
        pp.SSRThickness = 1.0f;
        pp.SSRStride = 0.25f;
        pp.SSRMaxSteps = 96;
        pp.SSRBinarySearchSteps = 6;
        pp.SSREdgeFade = 0.1f;

        struct Pose
        {
            const char* Name;
            glm::vec3 Eye;
            f32 Pitch;
            f32 BandY0;
            f32 BandY1;
        };
        // The block's reflection lands in the floor between the camera and the
        // block, lower in the frame as the view flattens.
        const std::array<Pose, 3> poses = { {
            { "Frontal", { 0.0f, 4.0f, 6.0f }, 0.42f, 0.42f, 0.55f },
            { "Low", { 0.0f, 2.2f, 8.5f }, 0.22f, 0.45f, 0.62f },
            { "Grazing", { 0.0f, 1.0f, 10.0f }, 0.08f, 0.50f, 0.67f },
        } };

        for (const Pose& pose : poses)
        {
            SCOPED_TRACE(pose.Name);
            Band band;
            Measure(pose.Name, pose.Eye, pose.Pitch, 0.40f, 0.60f, pose.BandY0, pose.BandY1, band);
            ASSERT_FALSE(HasFatalFailure());
            ASSERT_GT(band.Pixels, 100u);
            const f64 n = static_cast<f64>(band.Pixels);
            const f64 input = band.Input / n;
            const f64 output = band.Output / n;
            const f64 specular = band.Specular / n;
            const f64 removed = input - output;
            std::printf("[reflection-tier-term] %s: input %.5f output %.5f removed %.5f S %.5f (removed/S %.3f, "
                        "removed/input %.4f), worst per-pixel excess %.4f of the input\n",
                        pose.Name, input, output, removed, specular, specular > 0.0 ? removed / specular : 0.0,
                        input > 0.0 ? removed / input : 0.0, band.WorstExcess);
            std::fflush(stdout);

            ASSERT_GT(input, 0.05) << "the floor must be lit to measure what SSR leaves of it";
            ASSERT_GT(specular, 1.0e-4) << "the lighting exported no specular term here; there is nothing to replace";
            // What SSR removes is at most the specular term: the diffuse, the
            // emission and the direct light under a black reflection keep every
            // bit of their value. 2% of S for the temporal resolve's noise.
            EXPECT_LE(removed, specular * 1.02 + 1.0e-5)
                << "SSR removed more than the indirect specular term from under a black reflection: it dimmed "
                   "the diffuse, emission or direct light too (issue #1325)";
            // Pixel by pixel, too, where the old blend is unmistakable: it took up
            // to three quarters of a pixel's colour beyond that pixel's S.
            EXPECT_LE(band.WorstExcess, 0.02)
                << "at some pixel SSR removed more than 2% of the colour beyond its specular term (issue #1325)";
            // And the reflection did land: a black hit removes a real share of S.
            EXPECT_GT(removed, specular * 0.2) << "SSR's black reflection removed almost none of the specular term";
        }
    }
} // namespace OloEngine::Tests

// OLO_TEST_LAYER: L8
// =============================================================================
// SecondSunShadowVisualEvidenceTest.cpp
//
// The directional shadow cascades belong to the FIRST directional light alone.
// Scene.cpp builds them for UBO index 0 ("only one light gets the cascades");
// every lit shader nevertheless applied them to EVERY directional light, so a
// second sun — a rim or fill light, the staple of any lit showcase — was
// multiplied by the first sun's shadow map. It went dark exactly where the
// first sun is blocked, which is where a rim light is meant to read. Found on
// the #1533 dog showcase, whose rim light never lit the far side of the dog.
//
// THE CLAIM, on real pixels: a second, non-casting directional light adds the
// same light to ground INSIDE the first sun's shadow as to ground outside it.
// The scene is the ShadowEarlyOut layout — flat ground, a pole, a floating cube
// — under a casting sun, captured with and without a second sun from the other
// side. The second sun's contribution is the per-pixel difference, measured
// separately over the first sun's shadow and over its lit ground. With the
// cascades applied to both lights it is near zero inside the shadow; applied to
// the first alone it is at least as large inside as outside (tone mapping
// compresses the brighter lit ground more, so inside reads LARGER).
//
// AND the cascades must reach the casting sun at all. They go to one light,
// which Scene.cpp used to pick as whichever directional light its EnTT view
// yielded first — the NEWEST — so a sun created before its rim light cast no
// shadow anywhere. This scene creates them in exactly that order.
//
// Every rendering path the lit shaders serve: Forward (PBR_MultiLight),
// Forward+ (the same shader over the clustered path) and Deferred
// (DeferredLightingShared.glsl) each carry their own copy of the gate.
//
// Driver-independent (no committed goldens); SKIPs without a GL 4.6 context.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <array>
#include <cstring>
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
        constexpr f32 kCaptureTime = 5.0f;

        [[nodiscard]] f64 Luma(const u8* p)
        {
            return (0.2126 * p[0]) + (0.7152 * p[1]) + (0.0722 * p[2]);
        }

        fs::path VisualDir()
        {
            fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir;
        }
    } // namespace

    class SecondSunShadowVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        Entity m_Sun;
        Entity m_SecondSun;

        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            // The casting sun: low, from behind the pole, so the pole throws a
            // long shadow towards the camera on +Z. It is created FIRST, so it is
            // directional light 0 and owns the cascades.
            m_Sun = scene.CreateEntity("Sun");
            {
                auto& dl = m_Sun.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(0.18f, -0.55f, 0.55f));
                dl.m_Color = glm::vec3(1.0f, 0.98f, 0.95f);
                dl.m_Intensity = 3.5f;
                dl.m_CastShadows = true;
            }
            // The second sun: from the other side and higher, NOT casting. Its
            // own light reaches the first sun's shadow on the ground unobstructed
            // (the pole's shadow for THIS direction falls elsewhere, and it casts
            // none anyway).
            m_SecondSun = scene.CreateEntity("SecondSun");
            {
                auto& dl = m_SecondSun.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.35f, -0.8f, -0.3f));
                dl.m_Color = glm::vec3(0.8f, 0.9f, 1.0f);
                dl.m_Intensity = 0.0f; // toggled per capture
                dl.m_CastShadows = false;
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
                Ref<Mesh> mesh =
                    (prim == MeshPrimitive::Plane) ? MeshPrimitives::CreatePlane() : MeshPrimitives::CreateCube();
                if (mesh)
                {
                    mc.m_MeshSource = mesh->GetMeshSource();
                }
                auto& mat = e.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(albedo, 1.0f));
            };
            addPrimitive("Ground", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f }, { 40.0f, 1.0f, 40.0f },
                         { 0.72f, 0.72f, 0.74f });
            addPrimitive("Pole", MeshPrimitive::Cube, { 0.0f, 6.0f, -2.0f }, { 0.6f, 12.0f, 0.6f },
                         { 0.55f, 0.55f, 0.58f });
            addPrimitive("FloatCube", MeshPrimitive::Cube, { 6.0f, 4.0f, 1.0f }, { 2.0f, 2.0f, 2.0f },
                         { 0.6f, 0.3f, 0.25f });
        }

        void Capture(const std::string& name, RenderingPath path, bool sunCasts, f32 secondIntensity,
                     const EditorCamera& camera, std::vector<u8>& out)
        {
            ShadowSettings s = Renderer3D::GetShadowMap().GetSettings();
            s.Enabled = true;
            Renderer3D::GetShadowMap().SetSettings(s);
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::ApplyRendererSettings();

            m_Sun.GetComponent<DirectionalLightComponent>().m_CastShadows = sunCasts;
            m_SecondSun.GetComponent<DirectionalLightComponent>().m_Intensity = secondIntensity;
            RunEditorFrames(camera, 2);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
            {
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            }
            ASSERT_TRUE(fb) << "no composited framebuffer for '" << name << "'";
            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, out);
            ASSERT_EQ(out.size(), static_cast<sizet>(kWidth) * kHeight * 4u);

            const sizet rowBytes = static_cast<sizet>(kWidth) * 4u;
            std::vector<u8> tmp(rowBytes);
            for (u32 y = 0; y < kHeight / 2u; ++y)
            {
                u8* top = out.data() + (static_cast<sizet>(y) * rowBytes);
                u8* bottom = out.data() + (static_cast<sizet>(kHeight - 1u - y) * rowBytes);
                std::memcpy(tmp.data(), top, rowBytes);
                std::memcpy(top, bottom, rowBytes);
                std::memcpy(bottom, tmp.data(), rowBytes);
            }
            const std::string file = (VisualDir() / ("SecondSun_" + name + ".png")).string();
            ::stbi_write_png(file.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4, out.data(),
                             static_cast<int>(kWidth) * 4);
        }
    };

    TEST_F(SecondSunShadowVisualEvidenceTest, ASecondSunIsNotShadowedByTheFirstSunsCascades)
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

        // ShadowEarlyOut's three-quarter pose: the pole's long shadow fills the
        // lower frame. Yaw 0 looks toward -Z, positive pitch tilts down.
        EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        camera.SetPose({ 0.0f, 11.0f, 17.0f }, 0.0f, 0.55f);

        struct PathCase
        {
            const char* Name;
            RenderingPath Path;
        };
        const std::array<PathCase, 3> paths{ { { "Forward", RenderingPath::Forward },
                                               { "ForwardPlus", RenderingPath::ForwardPlus },
                                               { "Deferred", RenderingPath::Deferred } } };

        for (const PathCase& pathCase : paths)
        {
            // THE FIRST SUN'S SHADOW, found rather than assumed: ground that
            // darkens when the sun starts casting.
            std::vector<u8> sunLit;
            std::vector<u8> sunShadowed;
            std::vector<u8> bothSuns;
            const std::string base = std::string(pathCase.Name);
            Capture(base + "_SunNoShadow", pathCase.Path, false, 0.0f, camera, sunLit);
            ASSERT_FALSE(HasFatalFailure());
            Capture(base + "_Sun", pathCase.Path, true, 0.0f, camera, sunShadowed);
            ASSERT_FALSE(HasFatalFailure());
            Capture(base + "_SunAndSecondSun", pathCase.Path, true, 2.0f, camera, bothSuns);
            ASSERT_FALSE(HasFatalFailure());

            f64 insideGain = 0.0;
            f64 outsideGain = 0.0;
            u32 inside = 0;
            u32 outside = 0;
            const u32 yStart = static_cast<u32>(static_cast<f32>(kHeight) * 0.45f);
            for (u32 y = yStart; y < kHeight; ++y)
            {
                for (u32 x = 0; x < kWidth; ++x)
                {
                    const sizet i = ((static_cast<sizet>(y) * kWidth) + x) * 4u;
                    const f64 lit = Luma(sunLit.data() + i);
                    const f64 shadowed = Luma(sunShadowed.data() + i);
                    const f64 gain = Luma(bothSuns.data() + i) - shadowed;
                    if (lit > 120.0 && shadowed < lit - 40.0)
                    {
                        insideGain += gain;
                        ++inside;
                    }
                    else if (lit > 120.0 && std::abs(shadowed - lit) < 3.0)
                    {
                        outsideGain += gain;
                        ++outside;
                    }
                }
            }
            // The sun is created BEFORE the second sun, and EnTT yields the newest
            // entity first: before the cascades went to the brightest CASTING
            // light, this scene handed them to the non-casting second sun and
            // nothing cast at all. So this is the ownership check as well.
            ASSERT_GT(inside, 500u) << pathCase.Name
                                    << ": the first sun cast no shadow -- the cascades went to a light that does not cast";
            ASSERT_GT(outside, 5000u) << pathCase.Name << ": too little sunlit ground to measure against";
            const f64 meanInside = insideGain / static_cast<f64>(inside);
            const f64 meanOutside = outsideGain / static_cast<f64>(outside);
            std::printf("[second-sun] %s: gain inside the first sun's shadow %.2f over %u px, outside %.2f over %u px\n",
                        pathCase.Name, meanInside, inside, meanOutside, outside);
            EXPECT_GT(meanOutside, 5.0) << pathCase.Name << ": the second sun lit nothing at all";
            // With the first sun's cascades applied to the second sun, the gain
            // inside its shadow is a few luma levels of soft edge at most.
            EXPECT_GT(meanInside, 0.8 * meanOutside)
                << pathCase.Name << ": the second sun is darkened inside the FIRST sun's shadow, so it is still "
                << "being multiplied by cascades that were built for another light";
        }
    }
} // namespace OloEngine::Tests

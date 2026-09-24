// OLO_TEST_LAYER: integration
// =============================================================================
// LightingSignalCompositionEvidenceTest.cpp — enable/disable tests on the REAL
// deferred pipeline for the lighting-signal ownership rules of issue #1336.
//
// Each case toggles one technique and asserts what the contract
// (docs/agent-rules/lighting-signal-contract.md) says the toggle may and may
// not change:
//
//   1. SCREEN-SPACE AO is visibility for the AMBIENT term. A scene lit only by
//      emission has no ambient term, so turning GTAO on must leave the frame
//      unchanged — while GTAO's own debug view proves it found real occlusion
//      in that scene. Before #1336 PostProcess_SSAOApply multiplied the whole
//      frame and darkened the emission in every crease.
//   2. CONTACT SHADOWS are visibility for the SUN. The left half of the scene is
//      emission only (black albedo, so the sun reflects nothing from it) and the
//      right half is a sunlit white floor: turning contact shadows on must
//      darken the right half's lee and leave the left half untouched. Before
//      #1336 the post pass multiplied the whole frame, emission included.
//   3. SSGI replaces the ambient ladder over the directions it resolves; it
//      does not add to it. A grey floor inside an enclosure whose walls leave
//      exactly the radiance the ladder already assumed (the flat fill, 0.03)
//      must not change when SSGI is turned on — the white-furnace argument
//      applied to a screen-space estimator. The same scene with brighter walls
//      is the positive control that SSGI ran. Before #1336 SSGI added the walls'
//      light on top of the ladder that already counted those directions.
//
// Every case writes an OFF / ON pair of PNGs named per the task loop's
// forcing function — <Feature>[Off]_GL_Deferred.png — under
// OloEditor/assets/tests/visual/. GL only: the headless fixtures need a GL 4.6
// context and skip without one; the Vulkan cells are live-verified.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

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

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
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

        constexpr u32 kWidth = 640;
        constexpr u32 kHeight = 480;
        constexpr f32 kCaptureTime = 4.0f;
        constexpr u32 kFramesPerCapture = 8; // lets the temporal resolves settle on a static pose

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

        struct RegionDiff
        {
            f64 MeanOff = 0.0; // mean luma, OFF frame
            f64 MeanOn = 0.0;  // mean luma, ON frame
            f64 MeanAbsDiff = 0.0;
            u32 MaxAbsDiff = 0;
            u64 Darkened = 0; // pixels whose luma dropped by more than 6 levels OFF -> ON
            u64 Count = 0;
        };

        [[nodiscard]] f64 Luma(const u8* p)
        {
            return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
        }

        // Luma statistics over a UV rectangle (rows top-down) of two frames.
        [[nodiscard]] RegionDiff CompareRegion(const std::vector<u8>& off, const std::vector<u8>& on, f32 x0, f32 x1,
                                               f32 y0, f32 y1)
        {
            RegionDiff d;
            const u32 ix0 = static_cast<u32>(x0 * kWidth);
            const u32 ix1 = static_cast<u32>(x1 * kWidth);
            const u32 iy0 = static_cast<u32>(y0 * kHeight);
            const u32 iy1 = static_cast<u32>(y1 * kHeight);
            for (u32 y = iy0; y < iy1; ++y)
            {
                for (u32 x = ix0; x < ix1; ++x)
                {
                    const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                    const f64 a = Luma(&off[idx]);
                    const f64 b = Luma(&on[idx]);
                    d.MeanOff += a;
                    d.MeanOn += b;
                    d.MeanAbsDiff += std::abs(a - b);
                    if (a - b > 6.0)
                        ++d.Darkened;
                    for (int c = 0; c < 3; ++c)
                    {
                        const u32 diff = static_cast<u32>(std::abs(static_cast<int>(off[idx + c]) - static_cast<int>(on[idx + c])));
                        d.MaxAbsDiff = std::max(d.MaxAbsDiff, diff);
                    }
                    ++d.Count;
                }
            }
            if (d.Count > 0)
            {
                d.MeanOff /= static_cast<f64>(d.Count);
                d.MeanOn /= static_cast<f64>(d.Count);
                d.MeanAbsDiff /= static_cast<f64>(d.Count);
            }
            return d;
        }
    } // namespace

    class LightingSignalCompositionTest : public RendererAttachedTest
    {
      protected:
        // An EMISSION-ONLY surface is black AND metallic: black albedo alone is a
        // dielectric with F0 = 0.04, which still reflects ~4% of the sun as
        // specular — a real direct term a contact shadow may legitimately
        // darken. Metallic with black albedo has F0 = 0 and no body lobe, so its
        // outgoing radiance is its emission and nothing else.
        Entity AddMesh(const char* name, MeshPrimitive primitive, const glm::vec3& position, const glm::vec3& scale,
                       const glm::vec3& albedo, const glm::vec3& emissive)
        {
            const bool emissionOnly = albedo.x <= 0.0f && albedo.y <= 0.0f && albedo.z <= 0.0f;
            Scene& scene = GetScene();
            Entity e = scene.CreateEntity(name);
            auto& tc = e.GetComponent<TransformComponent>();
            tc.Translation = position;
            tc.Scale = scale;
            auto& mc = e.AddComponent<MeshComponent>();
            mc.m_Primitive = primitive;
            Ref<Mesh> mesh = (primitive == MeshPrimitive::Plane) ? MeshPrimitives::CreatePlane() : MeshPrimitives::CreateCube();
            if (mesh)
                mc.m_MeshSource = mesh->GetMeshSource();
            auto& mat = e.AddComponent<MaterialComponent>();
            mat.m_Material.SetBaseColorFactor(glm::vec4(albedo, 1.0f));
            mat.m_Material.SetEmissiveFactor(glm::vec4(emissive, 1.0f));
            mat.m_Material.SetMetallicFactor(emissionOnly ? 1.0f : 0.0f);
            mat.m_Material.SetRoughnessFactor(1.0f);
            return e;
        }

        void UseDeferredWithQuietPost(f32 exposure)
        {
            Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
            Renderer3D::ApplyRendererSettings();
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.BloomEnabled = false;
            pp.AutoExposureEnabled = false;
            pp.Exposure = exposure;
        }

        // Render from the pose, read the composited frame back (rows top-down)
        // and write it as `<name>.png` evidence.
        void Capture(const std::string& name, const glm::vec3& position, f32 yaw, f32 pitch, std::vector<u8>& out)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 500.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, yaw, pitch);
            RunEditorFrames(camera, kFramesPerCapture);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            ASSERT_TRUE(fb) << "no composited framebuffer for '" << name << "'";
            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, out);
            ASSERT_EQ(out.size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);

            const std::size_t rowBytes = static_cast<std::size_t>(kWidth) * 4u;
            std::vector<u8> tmp(rowBytes);
            for (u32 y = 0; y < kHeight / 2u; ++y)
            {
                u8* top = out.data() + static_cast<std::size_t>(y) * rowBytes;
                u8* bottom = out.data() + static_cast<std::size_t>(kHeight - 1u - y) * rowBytes;
                std::memcpy(tmp.data(), top, rowBytes);
                std::memcpy(top, bottom, rowBytes);
                std::memcpy(bottom, tmp.data(), rowBytes);
            }

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ASSERT_FALSE(ec) << "cannot create " << dir.generic_string();
            const std::string path = (dir / (name + ".png")).string();
            ASSERT_NE(::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4, out.data(),
                                       static_cast<int>(rowBytes)),
                      0)
                << "stbi_write_png failed for " << path;
        }
    };

    // -------------------------------------------------------------------------
    // 1. Screen-space AO does not touch emission.
    // -------------------------------------------------------------------------
    class LightingSignalAOTest : public LightingSignalCompositionTest
    {
      protected:
        void BuildScene() override
        {
            EnableRendering(kWidth, kHeight);
            UseDeferredWithQuietPost(1.0f);
            // No light, no environment: the only light in the frame is EMISSION,
            // on black-albedo surfaces, so the ambient term (the flat fill times
            // albedo) is zero everywhere. Two boxes on a floor make the creases
            // GTAO darkens.
            AddMesh("Floor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f }, { 30.0f, 1.0f, 30.0f }, glm::vec3(0.0f),
                    glm::vec3(0.55f));
            AddMesh("BoxA", MeshPrimitive::Cube, { -1.0f, 1.0f, 0.0f }, { 2.0f, 2.0f, 2.0f }, glm::vec3(0.0f),
                    glm::vec3(0.25f, 0.5f, 0.8f));
            AddMesh("BoxB", MeshPrimitive::Cube, { 1.2f, 0.75f, -0.6f }, { 1.5f, 1.5f, 1.5f }, glm::vec3(0.0f),
                    glm::vec3(0.8f, 0.45f, 0.2f));
        }
    };

    TEST_F(LightingSignalAOTest, ScreenSpaceAOLeavesEmissionUntouchedOnTheDeferredPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime mockTime(kCaptureTime);
        auto& pp = Renderer3D::GetPostProcessSettings();
        const glm::vec3 pose(0.0f, 3.5f, 6.5f);
        constexpr f32 yaw = 0.0f;
        constexpr f32 pitch = 0.45f;

        pp.GTAOEnabled = false;
        pp.SSAOEnabled = false;
        Renderer3D::ApplyRendererSettings();
        std::vector<u8> off;
        Capture("LightingSignalAOOff_GL_Deferred", pose, yaw, pitch, off);
        ASSERT_FALSE(HasFatalFailure());

        pp.ActiveAOTechnique = AOTechnique::GTAO;
        pp.GTAOEnabled = true;
        pp.GTAORadius = 1.5f;
        Renderer3D::ApplyRendererSettings();

        // Positive control FIRST: the AO buffer of this very scene holds real
        // occlusion. Without it an unchanged frame would also be what a GTAO
        // pass that never ran produces.
        pp.GTAODebugView = true;
        std::vector<u8> aoView;
        Capture("LightingSignalAODebug_GL_Deferred", pose, yaw, pitch, aoView);
        ASSERT_FALSE(HasFatalFailure());
        pp.GTAODebugView = false;
        f64 darkest = 255.0;
        for (std::size_t i = 0; i < aoView.size(); i += 4u)
            darkest = std::min(darkest, Luma(&aoView[i]));
        EXPECT_LT(darkest, 150.0) << "GTAO found no occlusion in the crease scene, so the assertion below would be "
                                     "vacuous — see LightingSignalAODebug_GL_Deferred.png";

        std::vector<u8> on;
        Capture("LightingSignalAO_GL_Deferred", pose, yaw, pitch, on);
        ASSERT_FALSE(HasFatalFailure());

        const RegionDiff whole = CompareRegion(off, on, 0.0f, 1.0f, 0.0f, 1.0f);
        EXPECT_GT(whole.MeanOff, 20.0) << "the emission-only frame rendered (near-)black";
        EXPECT_LT(whole.MeanAbsDiff, 0.5)
            << "TURNING GTAO ON CHANGED AN EMISSION-ONLY FRAME (mean |d luma| " << whole.MeanAbsDiff << ", max "
            << whole.MaxAbsDiff << "). AO is visibility for the ambient term, and this scene has none — the AO was "
            << "multiplied into emission. Compare LightingSignalAOOff_GL_Deferred.png / LightingSignalAO_GL_Deferred.png.";
        EXPECT_LE(whole.MaxAbsDiff, 3u) << "a crease pixel darkened by " << whole.MaxAbsDiff << " levels";
    }

    // -------------------------------------------------------------------------
    // 2. Contact shadows darken the sun and nothing else.
    // -------------------------------------------------------------------------
    class LightingSignalContactShadowTest : public LightingSignalCompositionTest
    {
      protected:
        void BuildScene() override
        {
            EnableRendering(kWidth, kHeight);
            UseDeferredWithQuietPost(1.0f);
            Scene& scene = GetScene();
            // A low sun from behind the camera's right shoulder, so each box
            // throws its contact shadow onto the floor toward the camera's left.
            Entity sun = scene.CreateEntity("Sun");
            auto& dl = sun.AddComponent<DirectionalLightComponent>();
            dl.m_Direction = glm::normalize(glm::vec3(-0.55f, -0.45f, -0.7f));
            dl.m_Color = glm::vec3(1.0f);
            dl.m_Intensity = 3.0f;

            // LEFT: emission only — black albedo reflects none of the sun.
            AddMesh("EmissiveFloor", MeshPrimitive::Plane, { -6.0f, 0.0f, 0.0f }, { 12.0f, 1.0f, 30.0f },
                    glm::vec3(0.0f), glm::vec3(0.45f));
            AddMesh("EmissiveBox", MeshPrimitive::Cube, { -3.0f, 0.6f, 0.0f }, { 1.2f, 1.2f, 1.2f }, glm::vec3(0.0f),
                    glm::vec3(0.3f, 0.3f, 0.6f));
            // RIGHT: a sunlit white floor and box — the positive control.
            AddMesh("LitFloor", MeshPrimitive::Plane, { 6.0f, 0.0f, 0.0f }, { 12.0f, 1.0f, 30.0f }, glm::vec3(0.8f),
                    glm::vec3(0.0f));
            AddMesh("LitBox", MeshPrimitive::Cube, { 3.0f, 0.6f, 0.0f }, { 1.2f, 1.2f, 1.2f }, glm::vec3(0.8f),
                    glm::vec3(0.0f));
        }
    };

    TEST_F(LightingSignalContactShadowTest, ContactShadowsDarkenTheSunlitHalfAndNotTheEmissiveHalf)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime mockTime(kCaptureTime);
        auto& pp = Renderer3D::GetPostProcessSettings();
        const glm::vec3 pose(0.0f, 4.0f, 9.0f);
        constexpr f32 yaw = 0.0f;
        constexpr f32 pitch = 0.4f;

        pp.ContactShadowEnabled = false;
        std::vector<u8> off;
        Capture("LightingSignalContactOff_GL_Deferred", pose, yaw, pitch, off);
        ASSERT_FALSE(HasFatalFailure());

        pp.ContactShadowEnabled = true;
        pp.ContactShadowIntensity = 1.0f;
        pp.ContactShadowMaxDistance = 1.5f;
        std::vector<u8> on;
        Capture("LightingSignalContact_GL_Deferred", pose, yaw, pitch, on);
        ASSERT_FALSE(HasFatalFailure());

        // The two halves of the frame, with a margin around the seam.
        const RegionDiff emissiveHalf = CompareRegion(off, on, 0.02f, 0.45f, 0.35f, 0.98f);
        const RegionDiff litHalf = CompareRegion(off, on, 0.55f, 0.98f, 0.35f, 0.98f);
        EXPECT_GT(emissiveHalf.MeanOff, 20.0) << "the emissive half rendered (near-)black";
        EXPECT_GT(litHalf.MeanOff, 20.0) << "the sunlit half rendered (near-)black";

        // Positive control: contact shadows ran and darkened sunlight. Counted,
        // not averaged: the contact band hugs the box's base and is a small
        // fraction of the half-frame, so its mean moves by a tenth of a level.
        EXPECT_GT(litHalf.Darkened, 150u)
            << "contact shadows visibly darkened only " << litHalf.Darkened << " sunlit pixels, so the assertion "
            << "below would be vacuous — see LightingSignalContact_GL_Deferred.png";
        EXPECT_GT(litHalf.MaxAbsDiff, 12u) << "no sunlit pixel darkened visibly under contact shadows";

        EXPECT_LT(emissiveHalf.MeanAbsDiff, 0.3)
            << "CONTACT SHADOWS DARKENED EMISSION (mean |d luma| " << emissiveHalf.MeanAbsDiff << ", max "
            << emissiveHalf.MaxAbsDiff << "). They are visibility for the sun, and black albedo reflects none of "
            << "it. Compare LightingSignalContactOff_GL_Deferred.png / LightingSignalContact_GL_Deferred.png.";
        EXPECT_LE(emissiveHalf.MaxAbsDiff, 3u);
    }

    // -------------------------------------------------------------------------
    // 3. SSGI replaces the ladder over its resolved directions — a furnace.
    // -------------------------------------------------------------------------
    class LightingSignalSSGITest : public LightingSignalCompositionTest
    {
      protected:
        void BuildScene() override
        {
            EnableRendering(kWidth, kHeight);
            // Exposure 8: the flat fill is 0.03, and the double count this case
            // exists to catch is a few percent of it in linear HDR. Amplified it
            // is tens of display levels; unamplified it would hide in 8 bits.
            UseDeferredWithQuietPost(8.0f);
            // No light, no environment: the ambient ladder answers with its flat
            // fill (OLO_AMBIENT_FLAT_FILL, 0.03 * albedo, no Fresnel split) on
            // every pixel. The floor is the receiver.
            AddMesh("Floor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f }, { 40.0f, 1.0f, 40.0f }, glm::vec3(0.8f),
                    glm::vec3(0.0f));
            // The enclosure: black albedo, so its outgoing radiance is exactly its
            // emission — set per case by SetEnclosureRadiance.
            m_Enclosure.push_back(AddMesh("BackWall", MeshPrimitive::Cube, { 0.0f, 2.0f, -3.0f }, { 12.0f, 4.0f, 0.5f },
                                          glm::vec3(0.0f), glm::vec3(0.0f)));
            m_Enclosure.push_back(AddMesh("LeftWall", MeshPrimitive::Cube, { -3.0f, 2.0f, 0.0f }, { 0.5f, 4.0f, 8.0f },
                                          glm::vec3(0.0f), glm::vec3(0.0f)));
            m_Enclosure.push_back(AddMesh("RightWall", MeshPrimitive::Cube, { 3.0f, 2.0f, 0.0f }, { 0.5f, 4.0f, 8.0f },
                                          glm::vec3(0.0f), glm::vec3(0.0f)));
        }

        void SetEnclosureRadiance(f32 radiance)
        {
            for (Entity& e : m_Enclosure)
            {
                e.GetComponent<MaterialComponent>().m_Material.SetEmissiveFactor(glm::vec4(glm::vec3(radiance), 1.0f));
            }
        }

        void ApplySSGI(bool enabled)
        {
            auto& pp = Renderer3D::GetPostProcessSettings();
            pp.SSGIEnabled = enabled;
            pp.SSGIIntensity = 1.0f;
            pp.SSGIMaxDistance = 12.0f;
            pp.SSGIThickness = 1.5f;
            pp.SSGIStride = 0.25f;
            pp.SSGIMaxSteps = 48;
            pp.SSGIRayCount = 16;
            pp.SSGIEdgeFade = 0.05f;
        }

        std::vector<Entity> m_Enclosure;
    };

    TEST_F(LightingSignalSSGITest, SSGIAddsNothingWhereTheResolvedRadianceIsWhatTheLadderAssumed)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ScopedMockTime mockTime(kCaptureTime);
        const glm::vec3 pose(0.0f, 2.2f, 4.5f);
        constexpr f32 yaw = 0.0f;
        constexpr f32 pitch = 0.35f;
        // The floor between the walls, in front of the back wall.
        constexpr f32 fx0 = 0.30f, fx1 = 0.70f, fy0 = 0.62f, fy1 = 0.92f;

        // ---- the furnace: enclosure radiance == the flat fill ---------------
        SetEnclosureRadiance(0.03f);
        ApplySSGI(false);
        std::vector<u8> furnaceOff;
        Capture("LightingSignalSSGIFurnaceOff_GL_Deferred", pose, yaw, pitch, furnaceOff);
        ASSERT_FALSE(HasFatalFailure());
        ApplySSGI(true);
        std::vector<u8> furnaceOn;
        Capture("LightingSignalSSGIFurnace_GL_Deferred", pose, yaw, pitch, furnaceOn);
        ASSERT_FALSE(HasFatalFailure());

        // ---- the positive control: enclosure 10x brighter than the fill -----
        SetEnclosureRadiance(0.3f);
        ApplySSGI(false);
        std::vector<u8> brightOff;
        Capture("LightingSignalSSGIBrightOff_GL_Deferred", pose, yaw, pitch, brightOff);
        ASSERT_FALSE(HasFatalFailure());
        ApplySSGI(true);
        std::vector<u8> brightOn;
        Capture("LightingSignalSSGIBright_GL_Deferred", pose, yaw, pitch, brightOn);
        ASSERT_FALSE(HasFatalFailure());

        const RegionDiff furnace = CompareRegion(furnaceOff, furnaceOn, fx0, fx1, fy0, fy1);
        const RegionDiff bright = CompareRegion(brightOff, brightOn, fx0, fx1, fy0, fy1);
        EXPECT_GT(furnace.MeanOff, 15.0) << "the furnace floor rendered (near-)black";

        EXPECT_GT(bright.MeanOn - bright.MeanOff, 8.0)
            << "SSGI did not brighten the floor under walls ten times brighter than the ladder assumed (off "
            << bright.MeanOff << ", on " << bright.MeanOn << "), so the furnace assertion below would be vacuous";

        EXPECT_LT(std::abs(furnace.MeanOn - furnace.MeanOff), 1.5)
            << "SSGI CHANGED THE FURNACE FLOOR (off " << furnace.MeanOff << ", on " << furnace.MeanOn
            << "). Every direction SSGI resolved leaves exactly the radiance the ambient ladder already counted, "
            << "so replacing the ladder over those directions changes nothing — adding to it double-counts them. "
            << "See LightingSignalSSGIFurnaceOff_GL_Deferred.png / LightingSignalSSGIFurnace_GL_Deferred.png.";
    }
} // namespace OloEngine::Tests

// OLO_TEST_LAYER: L8
//
// Virtual shadow pages for POINT and SPOT lights — the pixel evidence (#703).
//
// The L1 sibling (VirtualShadowMapLocalTest) pins the maths. This one answers the
// two questions only a rendered frame can:
//
//   1. DOES THE STARVATION CASE ACTUALLY GO AWAY? The budgeted atlas caps local
//      shadows at 16 lights / 32 entries, and a point light costs six entries. So
//      a scene with more casting local lights than that has, under the atlas,
//      lights whose `AtlasCasterRecord::Allocated` is FALSE — they light their
//      surroundings and cast nothing. The test builds exactly such a scene,
//      renders it both ways, and asserts:
//        * atlas: at least one caster starved, and the far lamp's floor is UNLIT
//          of shadow;
//        * VSM:   zero starved, and that same floor now carries a cast shadow.
//      That pair IS acceptance criterion 1, measured rather than argued.
//
//   2. DO THE CUBE FACES AGREE WITH EACH OTHER? A point light is six independent
//      perspective faces sharing one page pool, and the classic failure is one
//      face out of six being wrong — a flipped raster origin, an off-by-one in
//      the face selector, a page addressed in the neighbour's region. From a
//      single camera that is invisible: five faces look right and the sixth is
//      off-screen. So the occluder is orbited and captured from four azimuths,
//      and each capture must find a shadow.
//
// Both SKIP cleanly without a GL 4.6 context, like every other test in this
// directory, so headless CI stays green while a GPU run gates on them.
//
// The PNGs land in OloEditor/assets/tests/visual/ and are meant to be LOOKED AT:
// the assertions below can tell you a shadow exists and roughly where, and they
// cannot tell you it has a seam through it.

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
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

        // Local pages are marked from the PREVIOUS frame's depth buffer, exactly
        // like the directional ones, so a capture after one frame photographs an
        // empty page table. Several frames also let the allocator settle.
        constexpr u32 kSettleFrames = 8;

        // More casting local lights than the atlas can ever serve. 20 point lights
        // is 120 atlas entries against a 32-entry budget, so the atlas MUST starve
        // some of them — which is the control this whole file rests on.
        //
        // The count also has to clear the atlas' 16-LIGHT cap after culling: a
        // light whose range sphere is outside the camera frustum scores 0 and is
        // never a candidate in either path, and the first version of this scene
        // put enough of them out of frustum that only 15 competed — which reads as
        // "VSM serves fewer lights than the atlas cap" when it is really "this
        // scene never had that many candidates".
        constexpr u32 kPointLightCount = 20;
        constexpr u32 kSpotLightCount = 8;

        // POSITIVE PITCH TILTS THE VIEW DOWN (EditorCamera::SetPose). Spelled out
        // because the first version of this file used a negative pitch "to look
        // down", pointed the camera at empty sky, and produced a uniformly grey
        // frame that every assertion here read as "the shadow is missing" — the
        // exact mistake live-verification-noise-floor.md is about.
        constexpr f32 kLookDownPitch = 0.55f;

        [[nodiscard]] f64 MeanLuma(const std::vector<u8>& px, f32 x0, f32 x1, f32 y0, f32 y1)
        {
            const u32 ix0 = static_cast<u32>(x0 * kWidth);
            const u32 ix1 = std::min(static_cast<u32>(x1 * kWidth), kWidth);
            const u32 iy0 = static_cast<u32>(y0 * kHeight);
            const u32 iy1 = std::min(static_cast<u32>(y1 * kHeight), kHeight);

            f64 sum = 0.0;
            u64 count = 0;
            for (u32 y = iy0; y < iy1; ++y)
            {
                for (u32 x = ix0; x < ix1; ++x)
                {
                    const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                    if (idx + 2 >= px.size())
                        continue;
                    sum += 0.2126 * px[idx + 0] + 0.7152 * px[idx + 1] + 0.0722 * px[idx + 2];
                    ++count;
                }
            }
            return (count == 0) ? 0.0 : (sum / static_cast<f64>(count));
        }

        // How much of a region is meaningfully darker than its own bright end.
        // Expressed as a CONTRAST rather than an absolute level because the two
        // techniques do not have to agree on exposure — only on whether there is
        // a shadow in the frame at all.
        [[nodiscard]] f64 DarkFraction(const std::vector<u8>& px, f32 x0, f32 x1, f32 y0, f32 y1,
                                       f64 litReference)
        {
            const u32 ix0 = static_cast<u32>(x0 * kWidth);
            const u32 ix1 = std::min(static_cast<u32>(x1 * kWidth), kWidth);
            const u32 iy0 = static_cast<u32>(y0 * kHeight);
            const u32 iy1 = std::min(static_cast<u32>(y1 * kHeight), kHeight);

            u64 dark = 0;
            u64 count = 0;
            for (u32 y = iy0; y < iy1; ++y)
            {
                for (u32 x = ix0; x < ix1; ++x)
                {
                    const std::size_t idx = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
                    if (idx + 2 >= px.size())
                        continue;
                    const f64 luma = 0.2126 * px[idx + 0] + 0.7152 * px[idx + 1] + 0.0722 * px[idx + 2];
                    // 25% below the lit reference: comfortably past readback noise
                    // and past the cosine falloff across a flat floor, so what it
                    // counts is a cast shadow rather than a shading gradient.
                    if (luma < litReference * 0.75)
                        ++dark;
                    ++count;
                }
            }
            return (count == 0) ? 0.0 : (static_cast<f64>(dark) / static_cast<f64>(count));
        }

        // Mean absolute luma difference between two frames of the same scene and
        // pose, over the whole image.
        //
        // THIS IS THE MEASUREMENT THAT DOES NOT NEED A RECTANGLE, and that is why
        // it replaced one. A hand-placed "where the shadow should be" band is a
        // magic number that goes stale the moment the scene is retuned — and the
        // first version of this file had one that sat mostly on floor the lamp
        // never reached, so it measured unlit distance, scored 79% dark in BOTH
        // techniques, and passed while proving nothing.
        //
        // Comparing whole frames asks the question directly instead: turn the
        // shadows off and the frame changes by X; switch technique and it changes
        // by Y. If Y is small next to X, the two techniques drew the same shadow.
        [[nodiscard]] f64 MeanAbsLumaDiff(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
                return 0.0;
            f64 sum = 0.0;
            u64 count = 0;
            for (std::size_t idx = 0; idx + 2 < a.size(); idx += 4)
            {
                const f64 la = 0.2126 * a[idx + 0] + 0.7152 * a[idx + 1] + 0.0722 * a[idx + 2];
                const f64 lb = 0.2126 * b[idx + 0] + 0.7152 * b[idx + 1] + 0.0722 * b[idx + 2];
                sum += std::abs(la - lb);
                ++count;
            }
            return (count == 0) ? 0.0 : (sum / static_cast<f64>(count));
        }

        // Fraction of pixels whose luma moved by more than `delta` between two
        // frames of the same scene and pose.
        //
        // Preferred over the whole-frame MEAN for anything shaped like a shadow.
        // A cast shadow is a large local change over a small part of the image, so
        // averaging it across every unaffected pixel buries it: the criterion-1
        // pair below reads 0.44 as a mean, which is indistinguishable from noise
        // by inspection even though the two frames plainly differ. As a fraction
        // it says what it means -- "this much of the frame changed" -- and the
        // threshold can be reasoned about instead of guessed.
        [[nodiscard]] f64 ChangedFraction(const std::vector<u8>& a, const std::vector<u8>& b, f64 delta)
        {
            if (a.size() != b.size() || a.empty())
                return 0.0;
            u64 changed = 0;
            u64 count = 0;
            for (std::size_t idx = 0; idx + 2 < a.size(); idx += 4)
            {
                const f64 la = 0.2126 * a[idx + 0] + 0.7152 * a[idx + 1] + 0.0722 * a[idx + 2];
                const f64 lb = 0.2126 * b[idx + 0] + 0.7152 * b[idx + 1] + 0.0722 * b[idx + 2];
                if (std::abs(la - lb) > delta)
                    ++changed;
                ++count;
            }
            return (count == 0) ? 0.0 : (static_cast<f64>(changed) / static_cast<f64>(count));
        }
    } // namespace

    class VirtualShadowMapLocalVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        // Same unconditional restore as the directional evidence test: these
        // settings are PROCESS-WIDE, and an ASSERT_* firing mid-body would
        // otherwise leave VSM on for every later test in the binary.
        void TearDown() override
        {
            (void)SetVirtualShadowMaps(false);
            RendererAttachedTest::TearDown();
        }

        void BuildScene() override
        {
            Scene& scene = GetScene();

            EnableRendering(kWidth, kHeight);

            Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
            Renderer3D::ApplyRendererSettings();

            auto addMesh = [&scene](const char* name, MeshPrimitive prim, const glm::vec3& pos,
                                    const glm::vec3& scale)
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
                return e;
            };

            // The receiver. Without it the cast shadow has nothing to land on and
            // the frame looks the same whichever technique is running
            // (single-mesh-visual-test-lighting.md).
            {
                Entity floor = addMesh("Floor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f },
                                       { 200.0f, 1.0f, 200.0f });
                auto& mat = floor.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.75f, 0.75f, 0.75f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
            }

            // THE SUBJECT: one box at the origin with a lamp beside it. This is
            // the pair every assertion below reads — the box occludes the lamp and
            // throws a shadow onto open floor.
            {
                Entity occluder = addMesh("Occluder", MeshPrimitive::Cube, { 0.0f, 1.5f, 0.0f },
                                          { 3.0f, 3.0f, 3.0f });
                auto& mat = occluder.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.6f, 0.6f, 0.6f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
            }

            // `attenuation` is the QUADRATIC coefficient, and it has to be passed
            // explicitly: the component default is 2.0, which is 1/(1 + 2d^2) —
            // half a percent of the light at 10 m. A scene built on the default
            // renders lit by ambient alone, and then "no shadow" and "no light"
            // are the same picture.
            //
            // It is also the one lighting knob ShadowAtlas::ComputeScore does NOT
            // read, which is what lets the subject lamp below be BRIGHT and still
            // lose the atlas competition.
            auto addPointLight = [&scene](const char* name, const glm::vec3& pos, f32 intensity, f32 range,
                                          f32 attenuation)
            {
                Entity e = scene.CreateEntity(name);
                e.GetComponent<TransformComponent>().Translation = pos;
                auto& pl = e.AddComponent<PointLightComponent>();
                pl.m_Color = glm::vec3(1.0f);
                pl.m_Intensity = intensity;
                pl.m_Range = range;
                pl.m_Attenuation = attenuation;
                pl.m_CastShadows = true;
                return e;
            };

            // THE SUBJECT LAMP, deliberately the DIMMEST of the set.
            //
            // ShadowAtlas::ComputeScore ranks by intensity-weighted angular size,
            // and the atlas allocates in descending score order — so making this
            // one the faintest is what guarantees it loses the atlas competition
            // and wins nothing but a starved record. Under VSM there is no
            // competition to lose, and that difference is the entire experiment.
            // Placed at -X so its shadow of the occluder falls toward +X, which
            // from the camera pose below is open floor on the screen-RIGHT. A lamp
            // on the camera's side would put the shadow behind the box, hidden by
            // the box itself — how three sandbox scenes managed to look shadowless.
            //
            // Its UUID is kept because every assertion in the first test is about
            // THIS light: "some light was starved" is a much weaker claim than
            // "the light we deliberately made unaffordable was starved, and now
            // is not", and only the identity makes the second one sayable.
            // CLOSE to the occluder and SHORT-RANGED, which is what makes its
            // ShadowAtlas score low — the score is (range/cameraDistance)^2 scaled
            // by intensity, so a small range at a middling distance loses to the
            // ring below however bright the lamp actually is.
            m_SubjectLampId = addPointLight("SubjectLamp", { -4.5f, 3.0f, 0.0f }, 2.0f, 11.0f, 0.02f)
                                  .GetComponent<IDComponent>()
                                  .ID;

            // The competition: brighter lights in a ring, all casting. Two jobs,
            // and the second one is what makes the pixel comparison decisive —
            // they consume the atlas budget, AND their ring radius exceeds their
            // own range, so NONE of them reaches the occluder. The subject lamp is
            // therefore the only light that can light the subject's floor, which
            // means a shadow there is attributable to it alone.
            for (u32 i = 1; i < kPointLightCount; ++i)
            {
                const f32 angle = (static_cast<f32>(i) / static_cast<f32>(kPointLightCount)) * 6.2831853f;
                const glm::vec3 pos{ std::cos(angle) * 38.0f, 5.0f, std::sin(angle) * 38.0f };
                // Default attenuation ON PURPOSE: these must SCORE high (big
                // range) while CONTRIBUTING almost no light to the subject floor,
                // or they would wash out the very shadow being measured.
                addPointLight(("FillLamp" + std::to_string(i)).c_str(), pos, 9.0f, 30.0f, 2.0f);
            }

            for (u32 i = 0; i < kSpotLightCount; ++i)
            {
                const f32 angle = (static_cast<f32>(i) / static_cast<f32>(kSpotLightCount)) * 6.2831853f;
                Entity e = scene.CreateEntity(("FillSpot" + std::to_string(i)).c_str());
                e.GetComponent<TransformComponent>().Translation =
                    glm::vec3{ std::cos(angle) * 50.0f, 9.0f, std::sin(angle) * 50.0f };
                auto& sl = e.AddComponent<SpotLightComponent>();
                sl.m_Direction = glm::normalize(glm::vec3(-std::cos(angle), -1.0f, -std::sin(angle)));
                sl.m_Color = glm::vec3(1.0f);
                sl.m_Intensity = 9.0f;
                sl.m_Range = 40.0f;
                sl.m_Attenuation = 2.0f;
                sl.m_InnerCutoff = 14.0f;
                sl.m_OuterCutoff = 26.0f;
                sl.m_CastShadows = true;
            }
        }

        // Returns false when VSM refused to come up (wrong backend, shader load
        // failure): VirtualShadowMap::Init clears its own Enabled flag, so asking
        // the shadow map is the only honest way to know what the next frame ran.
        [[nodiscard]] static bool SetVirtualShadowMaps(bool enabled)
        {
            auto& shadowMap = Renderer3D::GetShadowMap();
            ShadowSettings settings = shadowMap.GetSettings();
            settings.VSM.Enabled = enabled;
            settings.VSM.LocalLights = true;
            shadowMap.SetSettings(settings);
            return shadowMap.IsVirtualShadowMapActive() == enabled;
        }

        struct CaptureStats
        {
            bool SubjectAllocated = false;
            bool SubjectSeen = false;
            u32 StarvedCasters = 0;
            u32 AllocatedCasters = 0;
            u32 LocalLights = 0;
            u32 LocalLayers = 0;
            u32 LocalPagesResident = 0;
            u32 LocalPagesDrawn = 0;
        };

        void Capture(const std::string& tag, const glm::vec3& position, f32 yaw, f32 pitch,
                     std::vector<u8>& outPixels, CaptureStats& outStats)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, yaw, pitch);

            RunEditorFrames(camera, kSettleFrames);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No composited framebuffer for local-light capture '" << tag << "'";

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

            // THE ACCEPTANCE-CRITERION COUNTERS, recorded next to every capture.
            // AtlasCasterRecord is filled by BOTH paths — the VSM one reuses it
            // precisely so "was anything starved" stays answerable with the same
            // question whichever technique is running.
            {
                const auto& shadowMap = Renderer3D::GetShadowMap();
                for (const auto& record : shadowMap.GetAtlasLayout())
                {
                    if (record.LightEntity == m_SubjectLampId && record.Score > 0.0f)
                    {
                        outStats.SubjectSeen = true;
                        outStats.SubjectAllocated = record.Allocated;
                    }
                    if (record.Score <= 0.0f)
                        continue; // culled before allocation — never a candidate
                    if (record.Allocated)
                        ++outStats.AllocatedCasters;
                    else
                        ++outStats.StarvedCasters;
                }

                const auto& vsm = shadowMap.GetVirtualShadowMap();
                outStats.LocalLights = vsm.GetLocalLightCount();
                outStats.LocalLayers = vsm.GetLocalLayerCount();
                const VSM::Statistics stats = vsm.GetStatistics();
                outStats.LocalPagesResident = stats.LocalPagesResident;
                outStats.LocalPagesDrawn = stats.LocalPagesDrawn;

                GTEST_LOG_(INFO) << "[" << tag << "] vsmActive=" << shadowMap.IsVirtualShadowMapActive()
                                 << " allocated=" << outStats.AllocatedCasters
                                 << " starved=" << outStats.StarvedCasters
                                 << " localLights=" << outStats.LocalLights
                                 << " localLayers=" << outStats.LocalLayers
                                 << " localResident=" << outStats.LocalPagesResident
                                 << " localDrawn=" << outStats.LocalPagesDrawn
                                 << " pagesResident=" << stats.PagesResident
                                 << " pagesFailed=" << stats.PagesFailed
                                 << " cullOverflows=" << stats.CullOverflows
                                 << " subjectSeen=" << outStats.SubjectSeen
                                 << " subjectAllocated=" << outStats.SubjectAllocated;
            }

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ASSERT_FALSE(ec) << "Failed to create evidence dir '" << dir.generic_string()
                             << "': " << ec.message();

            const std::string path = (dir / ("VirtualShadowMapLocal_" + tag + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth),
                                               static_cast<int>(kHeight), 4, outPixels.data(),
                                               static_cast<int>(kWidth) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed for '" << path << "'";
        }

        u64 m_SubjectLampId = 0;
    };

    // ACCEPTANCE CRITERION 1, as a rendered pair.
    TEST_F(VirtualShadowMapLocalVisualEvidenceTest, TheAtlasStarvesLightsAndTheLayerPoolDoesNot)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // The proven pose family from the directional evidence test: on +Z looking
        // back at the origin, POSITIVE pitch tilting down onto the floor.
        const glm::vec3 eye{ 0.0f, 9.0f, 16.0f };
        constexpr f32 kYaw = 0.0f;

        // ---- Control: the budgeted atlas -----------------------------------
        ASSERT_TRUE(SetVirtualShadowMaps(false)) << "could not return the shadow map to the atlas path";

        std::vector<u8> atlasPixels;
        CaptureStats atlasStats;
        Capture("Atlas", eye, kYaw, kLookDownPitch, atlasPixels, atlasStats);

        // The control has to actually BE the starved case, or the comparison below
        // proves nothing.
        ASSERT_TRUE(atlasStats.SubjectSeen)
            << "the subject lamp was not a shadow candidate at all — it is outside the camera frustum or "
               "not casting, so this scene cannot demonstrate anything";
        ASSERT_FALSE(atlasStats.SubjectAllocated)
            << "the atlas served the subject lamp, so this scene is NOT the starvation case and the VSM "
               "half below would prove nothing. Make the subject dimmer or add more competitors.";
        ASSERT_GT(atlasStats.StarvedCasters, 0u);
        ASSERT_EQ(atlasStats.LocalLights, 0u) << "the atlas control somehow registered VSM layers";

        // The floor has to be LIT in the control, or "no shadow" below is
        // indistinguishable from "no light" (single-mesh-visual-test-lighting.md).
        constexpr f32 kFloorY0 = 0.55f;
        constexpr f32 kFloorY1 = 0.98f;
        const f64 atlasLit = MeanLuma(atlasPixels, 0.0f, 1.0f, kFloorY0, kFloorY1);
        ASSERT_GT(atlasLit, 10.0)
            << "the floor is not lit in the atlas control (mean luma " << atlasLit
            << ") — look at assets/tests/visual/VirtualShadowMapLocal_Atlas.png before trusting anything "
               "below it";

        // ---- The page table --------------------------------------------------
        if (!SetVirtualShadowMaps(true))
        {
            GTEST_SKIP() << "Virtual Shadow Maps refused to initialise on this backend/driver — nothing "
                            "to compare against";
        }

        std::vector<u8> vsmPixels;
        CaptureStats vsmStats;
        Capture("Pages", eye, kYaw, kLookDownPitch, vsmPixels, vsmStats);

        // CRITERION 1, the allocation half.
        EXPECT_TRUE(vsmStats.SubjectAllocated)
            << "the layer pool did not serve the very light the atlas starved";
        EXPECT_EQ(vsmStats.StarvedCasters, 0u)
            << "the layer pool starved " << vsmStats.StarvedCasters
            << " local light(s) — the whole point of issue #703 is that this is zero for a scene the "
               "atlas could not serve";
        EXPECT_GT(vsmStats.AllocatedCasters, atlasStats.AllocatedCasters)
            << "VSM allocated " << vsmStats.AllocatedCasters << " local casters against the atlas "
            << atlasStats.AllocatedCasters << " — it must serve strictly more, not the same";
        EXPECT_GT(vsmStats.LocalLights, ShadowAtlas::kMaxShadowedLights)
            << "only " << vsmStats.LocalLights << " shadowed local lights — at or below the atlas own "
            << ShadowAtlas::kMaxShadowedLights << "-light cap, so the headline claim is unproven";

        // The pool must be BACKING those lights, not merely counting them. A layer
        // with no resident page is a light with no shadow, and every assertion
        // above would still pass.
        EXPECT_GT(vsmStats.LocalPagesResident, 0u)
            << "no local page is resident, so no local light has a shadow map at all — the layers were "
               "handed out and never backed";

        // CRITERION 1, the PIXEL half.
        //
        // Measured as the DIFFERENCE between the two frames, not as dark pixels in
        // a band. The first version counted dark pixels and scored 49% in both
        // techniques — because half that band is floor no lamp reaches, which is
        // equally dark whichever shadow technique is running, so the measurement
        // was dominated by something neither technique affects.
        //
        // The claim here is specific: VSM renders the shadows of 15 lights the
        // atlas starved. Two frames of the same scene from the same pose that
        // differ ONLY in that respect must therefore differ visibly, and by much
        // more than the frame-to-frame noise floor of a static scene.
        // WHAT THIS ACTUALLY MEASURES, stated because the number is smaller than
        // it first looks like it should be: the ring lights are deliberately given
        // the default 1/(1+2d^2) attenuation so they score high without washing out
        // the subject floor, which means they contribute almost no LIGHT and their
        // newly-available shadows are therefore almost invisible. The change
        // between these two frames is essentially the SUBJECT lamp shadow alone --
        // the one light this scene was built to have the atlas starve.
        //
        // A cast shadow is a big change over a small area, so it is measured as an
        // AREA, not as a whole-frame mean. The mean for this pair is ~0.44 luma,
        // which reads like noise; the same difference is ~2% of the frame moving
        // by more than 8 luma, which reads like what it is.
        const f64 vsmVsAtlas = MeanAbsLumaDiff(vsmPixels, atlasPixels);
        const f64 changed = ChangedFraction(vsmPixels, atlasPixels, 8.0);
        EXPECT_GT(changed, 0.005)
            << "only " << (changed * 100.0)
            << "% of the frame changed when the page table took over from the atlas, yet it is supposed "
               "to be casting the shadow of the subject lamp the atlas starved — the allocation numbers "
               "above are bookkeeping rather than pixels. Compare "
               "assets/tests/visual/VirtualShadowMapLocal_{Atlas,Pages}.png";

        // …and the extra darkness has to be SHADOW, not the whole frame dimming:
        // the lit region must survive. A page table that darkened everything
        // uniformly would pass the test above and be obviously wrong on screen.
        const f64 vsmLit = MeanLuma(vsmPixels, 0.0f, 1.0f, kFloorY0, kFloorY1);
        EXPECT_GT(vsmLit, atlasLit * 0.6)
            << "the floor lost " << (100.0 * (1.0 - vsmLit / std::max(atlasLit, 1.0e-6))) << "% of its brightness when the page table came on — that is a "
                                                                                             "frame going dark, not shadows appearing";

        GTEST_LOG_(INFO) << "atlas: allocated=" << atlasStats.AllocatedCasters
                         << " starved=" << atlasStats.StarvedCasters
                         << " subjectAllocated=" << atlasStats.SubjectAllocated
                         << " | vsm: allocated=" << vsmStats.AllocatedCasters
                         << " starved=" << vsmStats.StarvedCasters
                         << " subjectAllocated=" << vsmStats.SubjectAllocated
                         << " lights=" << vsmStats.LocalLights << " layers=" << vsmStats.LocalLayers
                         << " localResident=" << vsmStats.LocalPagesResident
                         << " | frameDiff=" << vsmVsAtlas << " changedFraction=" << changed << " atlasLit=" << atlasLit
                         << " vsmLit=" << vsmLit;
    }

    // ONE LIGHT, ONE BOX, ONE FLOOR — the isolation test, and the one to read
    // first when anything else in this file fails.
    //
    // Every other test here mixes two questions: "does a local light cast a
    // shadow through the page table at all" and "does the pool hold up under N
    // lights". A frame with no shadow answers both at once and distinguishes
    // neither — which is exactly what happened during bring-up, where a scene
    // packed with 28 lamps produced no shadow and the page counters said the pool
    // was exhausted, so the pipeline and the capacity were indistinguishable
    // suspects.
    //
    // This scene cannot be capacity-limited: one light is six layers, and its
    // pages are a rounding error against a 4096-page pool. So a missing shadow
    // here is the PIPELINE — the marker, the allocator, the raster, or the
    // sampler — and a present one clears all four in a single frame.
    //
    // It is also the local twin of the directional evidence test's headline
    // comparison: the atlas serves one light trivially, so both techniques must
    // cast, and they must cast in the SAME PLACE.
    class VirtualShadowMapLocalSingleLightTest : public RendererAttachedTest
    {
      protected:
        void TearDown() override
        {
            (void)SetVirtualShadowMaps(false);
            RendererAttachedTest::TearDown();
        }

        [[nodiscard]] static bool SetVirtualShadowMaps(bool enabled)
        {
            auto& shadowMap = Renderer3D::GetShadowMap();
            ShadowSettings settings = shadowMap.GetSettings();
            settings.VSM.Enabled = enabled;
            settings.VSM.LocalLights = true;
            shadowMap.SetSettings(settings);
            return shadowMap.IsVirtualShadowMapActive() == enabled;
        }

        // Shared by both tests on this fixture. A method rather than a lambda in
        // one of them because the orbit test needs the identical capture path —
        // same settle count, same flip, same PNG naming — or its per-azimuth
        // no-shadow control would not be comparable to its shadowed frame.
        [[nodiscard]] bool CapturePose(const std::string& tag, const glm::vec3& eye, f32 yaw,
                                       std::vector<u8>& pixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 1000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(eye, yaw, kLookDownPitch);
            RunEditorFrames(camera, kSettleFrames);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            if (!fb)
                fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            if (!fb)
                return false;

            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, pixels);
            const std::size_t rowBytes = static_cast<std::size_t>(kWidth) * 4u;
            std::vector<u8> tmp(rowBytes);
            for (u32 y = 0; y < kHeight / 2u; ++y)
            {
                u8* top = pixels.data() + static_cast<std::size_t>(y) * rowBytes;
                u8* bot = pixels.data() + static_cast<std::size_t>(kHeight - 1u - y) * rowBytes;
                std::memcpy(tmp.data(), top, rowBytes);
                std::memcpy(top, bot, rowBytes);
                std::memcpy(bot, tmp.data(), rowBytes);
            }

            const auto& shadowMap = Renderer3D::GetShadowMap();
            const VSM::Statistics stats = shadowMap.GetVirtualShadowMap().GetStatistics();
            GTEST_LOG_(INFO) << "[" << tag << "] vsmActive=" << shadowMap.IsVirtualShadowMapActive()
                             << " localLights=" << shadowMap.GetVirtualShadowMap().GetLocalLightCount()
                             << " localLayers=" << shadowMap.GetVirtualShadowMap().GetLocalLayerCount()
                             << " localResident=" << stats.LocalPagesResident
                             << " localDrawn=" << stats.LocalPagesDrawn << " failed=" << stats.PagesFailed;

            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const std::string path = (dir / ("VirtualShadowMapLocalSingle_" + tag + ".png")).string();
            ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                             pixels.data(), static_cast<int>(kWidth) * 4);
            return true;
        }

        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);
            Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
            Renderer3D::ApplyRendererSettings();

            auto addMesh = [&scene](const char* name, MeshPrimitive prim, const glm::vec3& pos,
                                    const glm::vec3& scale)
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
                return e;
            };

            // A SURROUND, not a single box beside the lamp — because the point
            // of this fixture is that a point light is SIX independent faces and
            // the classic failure is exactly one of them being wrong.
            //
            // Floor below (-Y), ceiling above (+Y), and four boxes ringing the
            // lamp (+X,-X,+Z,-Z). Every face therefore has an occluder, and every
            // azimuth of the orbit test sees at least two of the resulting
            // shadows radiating outward across the lit floor. With one box beside
            // the lamp instead, two of the four azimuths saw no shadow at all and
            // the test was reporting a scene limitation as a renderer fault.
            {
                Entity floor = addMesh("Floor", MeshPrimitive::Plane, { 0.0f, 0.0f, 0.0f },
                                       { 60.0f, 1.0f, 60.0f });
                auto& mat = floor.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.75f, 0.75f, 0.75f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
            }
            {
                Entity ceiling = addMesh("Ceiling", MeshPrimitive::Cube, { 0.0f, 7.0f, 0.0f },
                                         { 15.0f, 0.4f, 15.0f });
                auto& mat = ceiling.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.5f, 0.5f, 0.5f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
            }

            constexpr f32 kRingRadius = 5.0f;
            const std::array<glm::vec3, 4> ring{ {
                { kRingRadius, 1.4f, 0.0f },
                { -kRingRadius, 1.4f, 0.0f },
                { 0.0f, 1.4f, kRingRadius },
                { 0.0f, 1.4f, -kRingRadius },
            } };
            for (u32 i = 0; i < ring.size(); ++i)
            {
                Entity box = addMesh(("RingBox" + std::to_string(i)).c_str(), MeshPrimitive::Cube, ring[i],
                                     { 2.6f, 2.8f, 2.6f });
                auto& mat = box.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.62f, 0.62f, 0.66f, 1.0f));
                mat.m_Material.SetMetallicFactor(0.0f);
                mat.m_Material.SetRoughnessFactor(1.0f);
            }

            // Centred and low, between the ring boxes: the shadows fan OUTWARD
            // across open floor in every direction rather than landing under the
            // box that made them.
            {
                Entity lamp = scene.CreateEntity("Lamp");
                lamp.GetComponent<TransformComponent>().Translation = { 0.0f, 2.6f, 0.0f };
                auto& pl = lamp.AddComponent<PointLightComponent>();
                pl.m_Color = glm::vec3(1.0f);
                pl.m_Intensity = 4.0f;
                pl.m_Range = 34.0f;
                // See the note on the other fixture: the 2.0 component default is
                // a 1/(1+2d^2) falloff that leaves the floor lit by ambient alone.
                pl.m_Attenuation = 0.02f;
                pl.m_CastShadows = true;
            }
        }
    };

    TEST_F(VirtualShadowMapLocalSingleLightTest, APointLightCastsTheSameFloorShadowThroughEitherTechnique)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const glm::vec3 eye{ 0.0f, 9.0f, 16.0f };
        constexpr f32 kYaw = 0.0f;

        auto& shadowMap = Renderer3D::GetShadowMap();

        // ---- Control A: shadows OFF entirely --------------------------------
        // The reference the other two are measured against. Without it, "the
        // frame contains dark pixels" is not evidence of a shadow — the floor is
        // dark wherever the lamp does not reach, in every one of these frames.
        ASSERT_TRUE(SetVirtualShadowMaps(false));
        shadowMap.SetEnabled(false);
        std::vector<u8> noShadowPixels;
        ASSERT_TRUE(CapturePose("NoShadow", eye, kYaw, noShadowPixels));
        shadowMap.SetEnabled(true);

        // ---- Control B: the atlas -------------------------------------------
        std::vector<u8> atlasPixels;
        ASSERT_TRUE(CapturePose("Atlas", eye, kYaw, atlasPixels));

        const f64 atlasVsNone = MeanAbsLumaDiff(atlasPixels, noShadowPixels);
        ASSERT_GT(atlasVsNone, 1.0)
            << "turning shadows on changed the frame by only " << atlasVsNone
            << " luma on average — the ATLAS baseline is not casting a point-light shadow, so this test "
               "cannot say anything about the page table. Look at "
               "assets/tests/visual/VirtualShadowMapLocalSingle_{NoShadow,Atlas}.png";

        // ---- The page table --------------------------------------------------
        if (!SetVirtualShadowMaps(true))
            GTEST_SKIP() << "Virtual Shadow Maps refused to initialise on this backend/driver";

        std::vector<u8> vsmPixels;
        ASSERT_TRUE(CapturePose("Pages", eye, kYaw, vsmPixels));

        const f64 vsmVsNone = MeanAbsLumaDiff(vsmPixels, noShadowPixels);
        const f64 vsmVsAtlas = MeanAbsLumaDiff(vsmPixels, atlasPixels);

        GTEST_LOG_(INFO) << "single light: atlasVsNone=" << atlasVsNone << " vsmVsNone=" << vsmVsNone
                         << " vsmVsAtlas=" << vsmVsAtlas;

        // 1. The page table casts SOMETHING. One light, six layers, an empty
        //    pool — a failure here is the local pipeline itself (marker,
        //    allocator, raster or sampler), not capacity.
        EXPECT_GT(vsmVsNone, 1.0)
            << "enabling the page table changed the frame by only " << vsmVsNone
            << " luma against the no-shadow control, while the atlas changed it by " << atlasVsNone
            << " — the local light casts no shadow at all";

        // 2. It casts the SAME shadow. The two techniques must differ from each
        //    other by much less than either differs from having no shadow;
        //    otherwise VSM is drawing a shadow somewhere the atlas is not.
        EXPECT_LT(vsmVsAtlas, vsmVsNone * 0.5)
            << "the page table's frame differs from the atlas' by " << vsmVsAtlas
            << " luma but from the no-shadow control by only " << vsmVsNone
            << " — the two techniques are not drawing the same shadow. Compare "
               "assets/tests/visual/VirtualShadowMapLocalSingle_{Atlas,Pages}.png";

        // 3. And it is not merely a DARKER frame — the shadow has to be in the
        //    same place, which a whole-frame mean cannot see. The band right of
        //    the box is where a lamp at -X throws it.
        const f64 atlasLit = MeanLuma(atlasPixels, 0.05f, 0.35f, 0.45f, 0.75f);
        const f64 vsmLit = MeanLuma(vsmPixels, 0.05f, 0.35f, 0.45f, 0.75f);
        ASSERT_GT(atlasLit, 20.0) << "the lit pool beside the lamp is dark in the atlas frame";
        EXPECT_NEAR(vsmLit, atlasLit, atlasLit * 0.25)
            << "the lit region beside the lamp differs between techniques (" << vsmLit << " vs "
            << atlasLit << ") — one of them is dimming light it should not touch";
    }

    // ALL SIX CUBE FACES, from four azimuths.
    //
    // A point light is six independent perspective faces sharing one page pool,
    // and the classic failure is exactly one of them being wrong — a flipped
    // raster origin, an off-by-one in the face selector, a page addressed in a
    // neighbour's region. From a single camera that is invisible: the five good
    // faces carry the frame and the broken one is off-screen or behind you.
    //
    // Each azimuth is measured against its OWN no-shadow control, so the test
    // never has to know where on screen that azimuth's shadows land.
    TEST_F(VirtualShadowMapLocalSingleLightTest, EveryAzimuthAroundAPointLightFindsACastShadow)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        struct Pose
        {
            const char* Tag;
            glm::vec3 Eye;
            f32 Yaw;
        };

        constexpr f32 kRadius = 17.0f;
        constexpr f32 kEyeHeight = 9.0f;
        const std::array<Pose, 4> poses{ {
            { "Orbit000", { 0.0f, kEyeHeight, kRadius }, 0.0f },
            { "Orbit090", { kRadius, kEyeHeight, 0.0f }, 1.5707963f },
            { "Orbit180", { 0.0f, kEyeHeight, -kRadius }, 3.1415927f },
            { "Orbit270", { -kRadius, kEyeHeight, 0.0f }, -1.5707963f },
        } };

        auto& shadowMap = Renderer3D::GetShadowMap();

        u32 azimuthsWithShadow = 0;
        for (const Pose& pose : poses)
        {
            std::vector<u8> noShadow;
            std::vector<u8> withShadow;

            ASSERT_TRUE(SetVirtualShadowMaps(false));
            shadowMap.SetEnabled(false);
            ASSERT_TRUE(CapturePose(std::string(pose.Tag) + "_NoShadow", pose.Eye, pose.Yaw, noShadow));
            shadowMap.SetEnabled(true);

            if (!SetVirtualShadowMaps(true))
                GTEST_SKIP() << "Virtual Shadow Maps refused to initialise on this backend/driver";
            ASSERT_TRUE(CapturePose(pose.Tag, pose.Eye, pose.Yaw, withShadow));

            const f64 diff = MeanAbsLumaDiff(withShadow, noShadow);
            const f64 lit = MeanLuma(withShadow, 0.2f, 0.8f, 0.5f, 0.9f);
            GTEST_LOG_(INFO) << pose.Tag << ": litCentre=" << lit << " vsShadowlessDiff=" << diff;

            if (lit <= 10.0)
            {
                ADD_FAILURE() << pose.Tag << ": the floor around the lamp is not lit (mean luma " << lit
                              << ") — this azimuth cannot say anything about shadows";
                continue;
            }
            if (diff > 1.0)
                ++azimuthsWithShadow;
        }

        EXPECT_EQ(azimuthsWithShadow, poses.size())
            << "only " << azimuthsWithShadow << " of " << poses.size()
            << " azimuths changed when shadows were switched on. A point light is six faces sharing one "
               "page pool and this is how ONE broken face shows up — read the four "
               "assets/tests/visual/VirtualShadowMapLocalSingle_Orbit*.png against their _NoShadow "
               "counterparts and compare which quadrant is identical, then map it onto the "
               "+X,-X,+Y,-Y,+Z,-Z face order";
    }

} // namespace OloEngine::Tests

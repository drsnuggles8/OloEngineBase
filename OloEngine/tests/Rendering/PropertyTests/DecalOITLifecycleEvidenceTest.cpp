// OLO_TEST_LAYER: L8
// =============================================================================
// DecalOITLifecycleEvidenceTest.cpp  (#1335)
//
// The weighted-blended OIT decal path must draw its decals WITHOUT editing the
// packets it replays. DecalRenderPass installs the Decal_OIT program override
// per decal; it used to write it into every queued decal packet. In Deferred,
// DecalRenderPass::ExecuteOnGBuffer replays that same bucket first (the opaque
// decals), so by the time the OIT path ran, the packets it wrote had already
// been frozen for replay — the write the command lifecycle forbids, because a
// bucket replayed on workers is read without a lock. The pass now installs the
// override on clones.
//
// Real Scene pipeline, one transparent albedo decal over a grey floor, in
// Deferred (where the bucket is frozen before the OIT replay) and in Forward
// (where the OIT replay is the first). Per path there are two arms. OIT ON is the path #1335 changed: no
// lifecycle violation, and the decal-visibility diagnostic must report the
// OIT variant's draw issued and its fragments written. OIT OFF is the
// control: the same decal is visible in the composite, so the scene and the
// measurement work. The OIT composite itself is NOT asserted — it is empty
// before and after #1335, issue #1417.
//
// The Forward arm also pins a second fix: render-stream passes used to reset
// the shared frame allocator, freeing the decal packet before it replayed, and
// the OIT variant was then allocated on top of it (no draw issued).
//
// Writes DecalOIT_GL_<Path>.png (OIT on), DecalOITOff_GL_<Path>.png (OIT off)
// and a DecalOIT[Off]Control_GL_<Path>.png with the decal removed for each, to
// OloEditor/assets/tests/visual/. SKIPs cleanly without a GL 4.6 context.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/Commands/CommandLifecycle.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <stb_image/stb_image_write.h>

#include <cmath>
#include <iostream>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;
        constexpr u32 kSize = 256;

        void WritePng(const std::string& name, const std::vector<u8>& px, u32 w, u32 h)
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            const fs::path out = dir / name;
            EXPECT_NE(::stbi_write_png(out.string().c_str(), static_cast<int>(w), static_cast<int>(h), 4, px.data(),
                                       static_cast<int>(w) * 4),
                      0)
                << "failed to write " << out.string();
        }

        struct RegionStats
        {
            f32 MeanLuminance = 0.0f;
            f32 MeanRedMinusGreen = 0.0f;
        };

        RegionStats Measure(const std::vector<u8>& px, u32 w, u32 cx, u32 cy, u32 half)
        {
            f64 lum = 0.0;
            f64 rmg = 0.0;
            u32 n = 0;
            for (u32 y = cy - half; y < cy + half; ++y)
            {
                for (u32 x = cx - half; x < cx + half; ++x)
                {
                    const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
                    const f32 r = px[i + 0] / 255.0f;
                    const f32 g = px[i + 1] / 255.0f;
                    const f32 b = px[i + 2] / 255.0f;
                    lum += 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    rmg += r - g;
                    ++n;
                }
            }
            return { static_cast<f32>(lum / n), static_cast<f32>(rmg / n) };
        }
    } // namespace

    class DecalOITScene : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kSize, kSize);

            // Looking down at the floor from above and in front.
            Entity camera = scene.CreateEntity("Camera");
            auto& ctc = camera.GetComponent<TransformComponent>();
            ctc.Translation = { 0.0f, 9.0f, 6.0f };
            ctc.SetRotationEuler({ glm::radians(-56.0f), 0.0f, 0.0f });
            auto& cam = camera.AddComponent<CameraComponent>();
            cam.Primary = true;
            cam.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);

            Entity sun = scene.CreateEntity("Sun");
            auto& dl = sun.AddComponent<DirectionalLightComponent>();
            dl.m_Direction = glm::normalize(glm::vec3(0.2f, -1.0f, -0.3f));
            dl.m_Intensity = 2.0f;
            dl.m_CastShadows = false;

            Entity floor = scene.CreateEntity("Floor");
            floor.GetComponent<TransformComponent>().Scale = { 30.0f, 1.0f, 30.0f };
            auto& mc = floor.AddComponent<MeshComponent>();
            mc.m_Primitive = MeshPrimitive::Plane;
            if (Ref<Mesh> plane = MeshPrimitives::CreatePlane())
                mc.m_MeshSource = plane->GetMeshSource();
            auto& mat = floor.AddComponent<MaterialComponent>();
            mat.m_Material.SetBaseColorFactor(glm::vec4(0.6f, 0.6f, 0.6f, 1.0f));
            mat.m_Material.SetMetallicFactor(0.0f);
            mat.m_Material.SetRoughnessFactor(1.0f);

            m_Decal = scene.CreateEntity("TransparentDecal");
            m_Decal.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 0.0f };
            AddDecal();
        }

        void AddDecal()
        {
            auto& decal = m_Decal.AddComponent<DecalComponent>();
            decal.m_Size = { 4.0f, 2.0f, 4.0f };
            decal.m_Color = { 1.0f, 0.05f, 0.05f, 0.9f };
            decal.m_Mode = DecalMode::Albedo;
            decal.m_Transparent = true;
        }

        // One frame sequence at `path` with OIT on or off. Returns what the
        // decal-visibility diagnostic saw for the decal entity, and the red
        // excess of the decal region over the same frame without the decal.
        struct ArmResult
        {
            Renderer3D::DecalVisibilityObservation Observation;
            f32 RedExcess = 0.0f;
            f32 ControlLuminance = 0.0f;
        };

        ArmResult RenderArm(RenderingPath path, bool oit, const std::string& tag)
        {
            ArmResult result;
            auto& settings = Renderer3D::GetRendererSettings();
            settings.Path = path;
            settings.OITEnabled = oit;
            Renderer3D::ApplyRendererSettings();
            if (!m_Decal.HasComponent<DecalComponent>())
                AddDecal();

            // Arm the per-entity diagnostic, render, then read the completed
            // sample. It is double-buffered, so the frames after arming carry it.
            const i32 entityID = static_cast<i32>(static_cast<entt::entity>(m_Decal));
            (void)Renderer3D::ObserveDecalVisibility(entityID);
            RunFrames(4);
            result.Observation = Renderer3D::ObserveDecalVisibility(entityID);

            std::vector<u8> on;
            u32 w = 0;
            u32 h = 0;
            EXPECT_TRUE(ReadbackComposite(on, w, h)) << tag;
            WritePng("DecalOIT" + std::string(oit ? "" : "Off") + "_GL_" + tag + ".png", on, w, h);

            m_Decal.RemoveComponent<DecalComponent>();
            RunFrames(4);
            std::vector<u8> off;
            EXPECT_TRUE(ReadbackComposite(off, w, h)) << tag;
            WritePng("DecalOIT" + std::string(oit ? "" : "Off") + "Control_GL_" + tag + ".png", off, w, h);
            if (on.size() != static_cast<sizet>(kSize) * kSize * 4 || off.size() != on.size())
                return result;

            const RegionStats floorOnly = Measure(off, w, w / 2, h / 2, 16);
            const RegionStats withDecal = Measure(on, w, w / 2, h / 2, 16);
            result.RedExcess = withDecal.MeanRedMinusGreen - floorOnly.MeanRedMinusGreen;
            result.ControlLuminance = floorOnly.MeanLuminance;
            return result;
        }

        void RenderAndCheck(RenderingPath path, const char* pathName)
        {
            // OIT ON — the path #1335 changed. The decal must be replayed
            // through its OIT variant (draw issued, fragments written) and no
            // lifecycle violation may be raised on the way.
            const u64 violationsBefore = CommandLifecycle::GetTotalViolationCount();
            const ArmResult oit = RenderArm(path, true, pathName);
            EXPECT_EQ(CommandLifecycle::GetTotalViolationCount(), violationsBefore)
                << pathName << ": the OIT decal replay wrote a frozen packet, bucket or frame payload";
            EXPECT_TRUE(oit.Observation.HasSample) << pathName << ": no decal-visibility sample was completed";
            EXPECT_TRUE(oit.Observation.Submitted) << pathName << ": the decal was not submitted";
            EXPECT_TRUE(oit.Observation.DrawIssued)
                << pathName << ": the decal's OIT variant was never replayed (a skipped or refused replay)";
            EXPECT_TRUE(oit.Observation.FragmentResultKnown && oit.Observation.FragmentsSurvived)
                << pathName << ": the OIT decal draw wrote no fragments";
            // Not asserted: the OIT decal does not reach the COMPOSITE on this
            // path, before this change and after it — a pre-existing defect,
            // issue #1417, which owns this assertion. Printed so the number
            // travels with the log.
            std::cout << "[DecalOIT] " << pathName << " OIT composite red excess " << oit.RedExcess << '\n';

            // OIT OFF — the control arm: the same transparent decal through the
            // forward overlay is visible, so the scene and the measurement work.
            const ArmResult plain = RenderArm(path, false, pathName);
            EXPECT_GT(plain.ControlLuminance, 0.05f)
                << pathName << ": the control frame is empty, so the comparison below would mean nothing";
            EXPECT_GT(plain.RedExcess, 0.15f)
                << pathName << ": the transparent red decal is not visible over the floor without OIT";
        }

        Entity m_Decal;
    };

    TEST_F(DecalOITScene, TransparentDecalDrawsThroughOITWithoutWritingFrozenPackets)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // Path and OIT are process-global renderer settings; leave them as found.
        struct RestoreSettings
        {
            RendererSettings Saved = Renderer3D::GetRendererSettings();
            ~RestoreSettings()
            {
                Renderer3D::GetRendererSettings() = Saved;
                Renderer3D::ApplyRendererSettings();
            }
        } restoreSettings;

        // Deferred first: ExecuteOnGBuffer freezes the decal bucket before the
        // OIT replay, which is the case the lifecycle change is about.
        RenderAndCheck(RenderingPath::Deferred, "Deferred");
        RenderAndCheck(RenderingPath::Forward, "Forward");
    }
} // namespace OloEngine::Tests

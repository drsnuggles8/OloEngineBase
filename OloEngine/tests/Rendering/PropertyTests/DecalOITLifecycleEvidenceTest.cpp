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
// Deferred (where the bucket is frozen before the OIT replay), Forward (where
// the OIT replay is the first) and Forward+. Per path there are two arms. OIT ON is the path #1335 changed: no
// lifecycle violation, and the decal-visibility diagnostic must report the
// OIT variant's draw issued and its fragments written. OIT OFF is the
// control: the same decal is visible in the composite, so the scene and the
// measurement work. Since #1417 the OIT arm's composite is asserted too: the
// decal is visible over the floor against the decal-removed control.
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

#include "OloEngine/Particle/ParticleSystem.h"
#include "OloEngine/Renderer/Commands/CommandLifecycle.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
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
            m_Floor = floor;
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
            // The same, measured while the visibility diagnostic is armed.
            f32 ArmedRedExcess = 0.0f;
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
            if (oit)
                ExpectOITWriterOrder(tag);

            u32 w = 0;
            u32 h = 0;
            std::vector<u8> armed;
            EXPECT_TRUE(ReadbackComposite(armed, w, h)) << tag;

            // The frame a USER sees has no diagnostic armed. The diagnostic
            // re-applies the packet's render state after its own draw, which is
            // a second place the OIT blend was lost (#1417), so both frames are
            // measured. Re-targeting it at an entity with no decal disarms it
            // for this decal.
            (void)Renderer3D::ObserveDecalVisibility(static_cast<i32>(static_cast<entt::entity>(m_Floor)));
            RunFrames(4);
            std::vector<u8> on;
            EXPECT_TRUE(ReadbackComposite(on, w, h)) << tag;
            WritePng("DecalOIT" + std::string(oit ? "" : "Off") + "_GL_" + tag + ".png", on, w, h);

            m_Decal.RemoveComponent<DecalComponent>();
            RunFrames(4);
            std::vector<u8> off;
            EXPECT_TRUE(ReadbackComposite(off, w, h)) << tag;
            WritePng("DecalOIT" + std::string(oit ? "" : "Off") + "Control_GL_" + tag + ".png", off, w, h);
            if (on.size() != static_cast<sizet>(kSize) * kSize * 4 || off.size() != on.size() ||
                armed.size() != on.size())
                return result;

            const RegionStats floorOnly = Measure(off, w, w / 2, h / 2, 16);
            const RegionStats withDecal = Measure(on, w, w / 2, h / 2, 16);
            result.RedExcess = withDecal.MeanRedMinusGreen - floorOnly.MeanRedMinusGreen;
            result.ArmedRedExcess = Measure(armed, w, w / 2, h / 2, 16).MeanRedMinusGreen - floorOnly.MeanRedMinusGreen;
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
            // #1417: the OIT decal reaches the COMPOSITE. It used to be drawn,
            // its fragments surviving, and still leave the frame byte-identical
            // to the decal-removed control (red excess 0.000): the packet's own
            // render state set one global blend function over both OIT targets,
            // so the accumulation overflowed and the revealage stayed at 1.
            std::cout << "[DecalOIT] " << pathName << " OIT composite red excess " << oit.RedExcess << '\n';

            // OIT OFF — the control arm: the same transparent decal through the
            // forward overlay is visible, so the scene and the measurement work.
            EXPECT_GT(oit.RedExcess, 0.15f)
                << pathName << ": the transparent red decal drawn through OIT is not visible in the composite";
            // Arming the diagnostic must not change what is drawn.
            EXPECT_NEAR(oit.ArmedRedExcess, oit.RedExcess, 0.01f)
                << pathName << ": the OIT decal composites differently while the visibility diagnostic is armed";

            const ArmResult plain = RenderArm(path, false, pathName);
            EXPECT_GT(plain.ControlLuminance, 0.05f)
                << pathName << ": the control frame is empty, so the comparison below would mean nothing";
            EXPECT_GT(plain.RedExcess, 0.15f)
                << pathName << ": the transparent red decal is not visible over the floor without OIT";
        }

        // The order the WB-OIT chain needs: the targets are cleared, the decal
        // accumulates into them, and only then are they resolved. Measured while
        // diagnosing #1417 -- the order was right and the loss was the blend --
        // and pinned so a registration change cannot move the decal outside it.
        static void ExpectOITWriterOrder(const std::string& tag)
        {
            const auto& graph = RenderGraphDebugRuntime::GetActiveGraph();
            ASSERT_TRUE(graph) << tag << ": no active render graph";
            const auto order = graph->GetExecutionOrder();
            const auto indexOf = [&order](std::string_view name) -> std::ptrdiff_t
            {
                const auto it = std::ranges::find_if(order, [name](const FString& entry)
                                                     { return entry.ToView() == name; });
                return it == order.end() ? -1 : it - order.begin();
            };
            const std::ptrdiff_t prepare = indexOf("OITPreparePass");
            const std::ptrdiff_t decal = indexOf("DecalPass");
            const std::ptrdiff_t resolve = indexOf("OITResolvePass");
            ASSERT_GE(prepare, 0) << tag;
            ASSERT_GE(decal, 0) << tag;
            ASSERT_GE(resolve, 0) << tag;
            EXPECT_LT(prepare, decal) << tag << ": the decal accumulates before the OIT targets are cleared";
            EXPECT_LT(decal, resolve) << tag << ": the OIT targets are resolved before the decal accumulates";
        }

        Entity m_Decal;
        Entity m_Floor;
        Entity m_Particles;
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
        RenderAndCheck(RenderingPath::ForwardPlus, "ForwardPlus");
    }

    // #1417's third question: do PARTICLES through OIT reach the composite? They
    // share the OIT targets and the resolve with the decal, and neither kind did:
    // CPU billboards lost the per-attachment blend to the per-emitter blend-mode
    // helper (the same global SetBlendFunc as the decal packets), and GPU
    // billboards had no OIT shader at all, so they wrote nothing to the revealage
    // target. Measured on both: red billboards over the floor, against the same
    // frame with the particles removed, with OIT on and off.
    TEST_F(DecalOITScene, TransparentParticlesThroughOITReachTheComposite)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        struct RestoreSettings
        {
            RendererSettings Saved = Renderer3D::GetRendererSettings();
            ~RestoreSettings()
            {
                Renderer3D::GetRendererSettings() = Saved;
                Renderer3D::ApplyRendererSettings();
            }
        } restoreSettings;

        m_Decal.RemoveComponent<DecalComponent>();

        const auto addParticles = [this](bool gpu)
        {
            m_Particles = GetScene().CreateEntity("TransparentParticles");
            auto& system = m_Particles.AddComponent<ParticleSystemComponent>().System;
            system.RenderMode = ParticleRenderMode::Billboard;
            system.GravityModule.Enabled = false;
            system.DragModule.Enabled = false;
            if (gpu)
            {
                // GPU particles are emitted by the emitter and simulated on the
                // device, so the sheet is a still stack at the emitter. About a
                // dozen layers: WB-OIT's weight clamp lets roughly two dozen
                // same-pixel layers overflow the RGBA16F accumulator (#1468),
                // and that is not what this measures.
                m_Particles.GetComponent<TransformComponent>().Translation = { 0.0f, 0.3f, 0.0f };
                system.UseGPU = true;
                system.Emitter.RateOverTime = 60.0f;
                system.Emitter.InitialSpeed = 0.0f;
                system.Emitter.LifetimeMin = system.Emitter.LifetimeMax = 1000.0f;
                system.Emitter.InitialSize = 1.6f;
                system.Emitter.InitialColor = { 1.0f, 0.05f, 0.05f, 0.9f };
                return;
            }
            system.Emitter.RateOverTime = 0;
            auto& pool = system.GetPool();
            constexpr u32 kCount = 25;
            ASSERT_EQ(pool.Emit(kCount), kCount);
            for (u32 i = 0; i < kCount; ++i)
            {
                // A 5x5 sheet of overlapping quads just above the floor centre,
                // so the measured box is covered by several layers.
                pool.m_Positions[i] = { -0.6f + 0.3f * static_cast<f32>(i % 5), 0.3f,
                                        -0.6f + 0.3f * static_cast<f32>(i / 5) };
                pool.m_PrevPositions[i] = pool.m_Positions[i];
                pool.m_Colors[i] = pool.m_InitialColors[i] = { 1.0f, 0.05f, 0.05f, 0.9f };
                pool.m_Sizes[i] = pool.m_PrevSizes[i] = pool.m_InitialSizes[i] = 0.8f;
                pool.m_Lifetimes[i] = pool.m_MaxLifetimes[i] = 1000.0f;
            }
            system.Update(0.001f, glm::vec3(0.0f));
        };

        const auto measure = [this, &addParticles](RenderingPath path, bool oit, bool gpu, const std::string& tag) -> f32
        {
            auto& settings = Renderer3D::GetRendererSettings();
            settings.Path = path;
            settings.OITEnabled = oit;
            Renderer3D::ApplyRendererSettings();

            addParticles(gpu);
            RunFrames(gpu ? 12 : 4);
            std::vector<u8> on;
            u32 w = 0;
            u32 h = 0;
            EXPECT_TRUE(ReadbackComposite(on, w, h)) << tag;
            WritePng("Particle" + std::string(gpu ? "Gpu" : "") + "OIT" + std::string(oit ? "" : "Off") + "_GL_" + tag +
                         ".png",
                     on, w, h);

            GetScene().DestroyEntity(m_Particles);
            RunFrames(4);
            std::vector<u8> off;
            EXPECT_TRUE(ReadbackComposite(off, w, h)) << tag;
            if (on.size() != static_cast<sizet>(kSize) * kSize * 4 || off.size() != on.size())
                return 0.0f;
            return Measure(on, w, w / 2, h / 2, 16).MeanRedMinusGreen -
                   Measure(off, w, w / 2, h / 2, 16).MeanRedMinusGreen;
        };

        for (const auto& [path, name] : { std::pair{ RenderingPath::Deferred, "Deferred" },
                                          std::pair{ RenderingPath::Forward, "Forward" },
                                          std::pair{ RenderingPath::ForwardPlus, "ForwardPlus" } })
        {
            for (const bool gpu : { false, true })
            {
                const char* kind = gpu ? "GPU" : "CPU";
                const f32 oitExcess = measure(path, true, gpu, name);
                const f32 plainExcess = measure(path, false, gpu, name);
                std::cout << "[ParticleOIT] " << name << ' ' << kind << " red excess: OIT " << oitExcess << ", OIT off "
                          << plainExcess << '\n';
                EXPECT_GT(plainExcess, 0.15f) << name << ' ' << kind
                                              << ": the red particles are not visible without OIT either, so the scene "
                                                 "does not measure anything";
                EXPECT_GT(oitExcess, 0.15f) << name << ' ' << kind
                                            << ": the red particles drawn through OIT are not visible in the composite";
            }
        }
    }
} // namespace OloEngine::Tests

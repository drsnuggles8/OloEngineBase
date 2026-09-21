// OLO_TEST_LAYER: L8
// =============================================================================
// GBufferDebugFinalWriterEvidenceTest.cpp — a G-Buffer debug view must show the
// FINAL G-Buffer, not the first writer's (issue #1329).
//
// THE BUG THIS PINS. The deferred G-Buffer has four writers and ScenePass is
// only the first of them: VirtualGeometryPass, DeferredGPUOcclusionPass and
// DeferredOpaqueDecalPass are separate graph nodes the scheduler runs AFTER
// ScenePass::Execute has returned. The debug blit used to sit at the tail of
// that Execute, so an "Albedo" view showed the surface before the decals were
// projected onto it and a "Normal" view showed it before the virtual-geometry
// clusters landed — a picture of a G-Buffer no lighting consumer ever saw, in
// the one mode whose entire purpose is to be trusted while diagnosing
// something else. The extraction now lives in GBufferDebugPass, registered
// after every late writer and immediately before DeferredLightingPass.
//
// WHY A DECAL IS THE LATE WRITER. The issue's fourth acceptance criterion
// rules out a pre-filled texture or a debug-only substitute, so these tests
// drive a real DecalComponent through Scene::OnUpdateRuntime: the same
// submission path, the same DecalRenderPass bucket and the same
// Decal_GBuffer.glsl draw the editor runs. The A/B is the decal entity's
// presence, nothing else — same camera, same wall, same channel.
//
// WHAT IS ASSERTED, per acceptance criterion:
//
//   1. The debug image contains the decal in all three MSAA configurations
//      (off, resolve-before-lighting, per-sample). Measured as strongly-red
//      pixels against a deliberately grey wall, with the decal-free render of
//      the identical scene as the control — so the number the assertion
//      compares against is measured, not chosen.
//   2. The capture identifies its frame and the G-Buffer content version it
//      read, and a frame that extracts nothing retires the record instead of
//      leaving the last one to be read as current.
//   3. Entity IDs and packed material flags survive the MSAA resolve as
//      values something actually wrote. Asserted as a SUBSET rule against the
//      non-MSAA render of the same scene: any value present after the resolve
//      that is absent without it was invented by an average.
//   4. Covered by construction — see "why a decal is the late writer".
//
// Evidence PNGs (written before any assertion):
//   OloEditor/assets/tests/visual/GBufferDebugFinalWriter_GL_Deferred_*.png
//
// Classification: L8 (full Scene pipeline, RGBA8 readback + PNG; SKIPs cleanly
// without a GL 4.6 context).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/Commands/FrameResourceManager.h"
#include "OloEngine/Renderer/Debug/DebugViewProvenance.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/GBuffer.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/PBRModel.h"
#include "OloEngine/Renderer/Passes/SceneRenderPass.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>

#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iterator>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kSize = 320;

        // DeferredSettings::DebugChannel values used here.
        constexpr u32 kChannelOff = 0;
        constexpr u32 kChannelAlbedo = 1;

        // "Strongly red" — the decal's colour against a neutral grey wall. The
        // margin is wide because the debug blit's RGBA8 albedo goes through the
        // whole post chain (tone map, gamma) before the composite readback, and
        // the test is about presence, not about an exact value surviving that.
        [[nodiscard]] bool IsDecalRed(const std::vector<u8>& px, std::size_t i)
        {
            const int r = px[i + 0];
            const int g = px[i + 1];
            const int b = px[i + 2];
            return r > 90 && r - g > 45 && r - b > 45;
        }

        [[nodiscard]] u32 CountDecalRedPixels(const std::vector<u8>& px, u32 w, u32 h)
        {
            u32 count = 0;
            for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i)
            {
                if (IsDecalRed(px, i * 4))
                    ++count;
            }
            return count;
        }

        // Non-vacuity guard: a frame with nothing in it has no red pixels
        // either, so "the control has none" would otherwise pass on a black
        // screen. Every assertion below is paired with this.
        [[nodiscard]] u32 CountNonBackgroundPixels(const std::vector<u8>& px, u32 w, u32 h)
        {
            u32 count = 0;
            for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i)
            {
                const std::size_t idx = i * 4;
                if (px[idx] > 24 || px[idx + 1] > 24 || px[idx + 2] > 24)
                    ++count;
            }
            return count;
        }

        [[nodiscard]] fs::path VisualOutputPath(const char* caseName)
        {
            fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir / (std::string("GBufferDebugFinalWriter_GL_Deferred_") + caseName + ".png");
        }

        // Quantise a flags-lane float to a comparable key. RT2's alpha is a
        // packed BITFIELD written as a small whole number through an RGBA16F
        // attachment, so equality at 1/1024 is exact for every value a shader
        // writes and still tolerant of the half round-trip.
        [[nodiscard]] i32 FlagsKey(f32 value)
        {
            return static_cast<i32>(std::lround(static_cast<f64>(value) * 1024.0));
        }

        [[nodiscard]] SceneRenderPass* GeometryPass()
        {
            return static_cast<SceneRenderPass*>(
                Renderer3D::GetRenderStreamNode(Renderer3D::RenderStreamType::Geometry));
        }
    } // namespace

    // A flat grey wall filling the view with a red albedo decal projected onto
    // the middle of it. Nothing else: the frame has exactly one red thing in
    // it, and it arrives through the late writer.
    class GBufferDebugFinalWriterScene : public RendererAttachedTest
    {
      protected:
        // Set by a fixture before BuildScene runs.
        bool m_SpawnDecal = true;

        void BuildScene() override
        {
            Entity camera = GetScene().CreateEntity("Camera");
            camera.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 5.0f };
            auto& cameraComp = camera.AddComponent<CameraComponent>();
            cameraComp.Primary = true;
            cameraComp.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);

            Entity sun = GetScene().CreateEntity("Sun");
            auto& dirLight = sun.AddComponent<DirectionalLightComponent>();
            dirLight.m_Direction = { -0.1f, -0.2f, -1.0f };
            dirLight.m_Color = { 1.0f, 1.0f, 1.0f };
            dirLight.m_Intensity = 3.0f;
            dirLight.m_CastShadows = false;

            // The wall. Big enough to fill the frame so no cleared background
            // is visible — the flags-lane subset assertion below depends on
            // that (a cleared texel carries a flags value nothing authored).
            {
                Entity wall = GetScene().CreateEntity("Wall");
                wall.AddComponent<MeshComponent>(MeshPrimitives::CreateCube()->GetMeshSource());
                auto& transform = wall.GetComponent<TransformComponent>();
                transform.Translation = { 0.0f, 0.0f, -1.0f };
                transform.Scale = { 40.0f, 40.0f, 0.5f };
                auto& materialComp = wall.AddComponent<MaterialComponent>();
                materialComp.m_Material.SetBaseColorFactor(glm::vec4(0.72f, 0.72f, 0.72f, 1.0f));
                materialComp.m_Material.SetMetallicFactor(0.0f);
                materialComp.m_Material.SetRoughnessFactor(0.6f);
            }

            // A second, closer subject on a DIFFERENT PBR model, so the frame
            // carries two distinct entity IDs and two distinct packed-flag
            // values across a silhouette — the pair the MSAA resolve has to
            // keep intact rather than average.
            {
                Entity subject = GetScene().CreateEntity("ClosureSubject");
                subject.AddComponent<MeshComponent>(MeshPrimitives::CreateSphere(1.1f, 32)->GetMeshSource());
                subject.GetComponent<TransformComponent>().Translation = { -1.4f, 0.6f, 1.0f };
                auto& materialComp = subject.AddComponent<MaterialComponent>();
                materialComp.m_Material.SetBaseColorFactor(glm::vec4(0.80f, 0.80f, 0.80f, 1.0f));
                materialComp.m_Material.SetMetallicFactor(0.0f);
                materialComp.m_Material.SetRoughnessFactor(0.3f);
                materialComp.m_Material.SetPBRModel(PBRModel::ClosureV2);
            }

            if (m_SpawnDecal)
            {
                // THE LATE WRITER. An opaque deferred decal: submitted by
                // Scene::OnUpdateRuntime into the DecalRenderPass bucket and
                // drained into the G-Buffer by DeferredOpaqueDecalPass, which
                // the scheduler runs long after ScenePass returned.
                Entity decal = GetScene().CreateEntity("RedDecal");
                auto& transform = decal.GetComponent<TransformComponent>();
                // The wall's front face sits at z = -0.75; the projection box
                // straddles it so the whole patch lands on the wall.
                transform.Translation = { 0.9f, -0.4f, -0.75f };
                auto& decalComp = decal.AddComponent<DecalComponent>();
                decalComp.m_Color = { 1.0f, 0.03f, 0.03f, 1.0f };
                decalComp.m_Size = { 2.4f, 2.4f, 2.0f };
                decalComp.m_FadeDistance = 0.05f;
                decalComp.m_Transparent = false;
            }

            EnableRendering(kSize, kSize);
        }

        // Render the scene on the Deferred path with the given MSAA and debug
        // configuration, write the evidence PNG, and hand back the composite.
        void RenderDeferred(u32 sampleCount, bool perSampleLighting, u32 debugChannel,
                            const char* caseName, std::vector<u8>& outPx, u32& outWidth, u32& outHeight)
        {
            auto& settings = Renderer3D::GetRendererSettings();
            settings.Path = RenderingPath::Deferred;
            settings.Deferred.MSAASampleCount = sampleCount;
            settings.Deferred.PerSampleLighting = perSampleLighting;
            settings.Deferred.DebugChannel = debugChannel;
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

    class GBufferDebugWithDecalScene : public GBufferDebugFinalWriterScene
    {
      protected:
        void SetUp() override
        {
            m_SpawnDecal = true;
            RendererAttachedTest::SetUp();
        }
    };

    class GBufferDebugWithoutDecalScene : public GBufferDebugFinalWriterScene
    {
      protected:
        void SetUp() override
        {
            m_SpawnDecal = false;
            RendererAttachedTest::SetUp();
        }
    };

    // ---- Criterion 1 + 4 --------------------------------------------------
    // The decal is in the albedo debug view in every MSAA configuration. Each
    // arm is a separate test so a failure names the configuration.

    TEST_F(GBufferDebugWithDecalScene, AlbedoDebugViewShowsTheDecalWithoutMSAA)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        std::vector<u8> px;
        u32 w = 0;
        u32 h = 0;
        RenderDeferred(1u, false, kChannelAlbedo, "Albedo_MSAA1_Decal", px, w, h);
        if (::testing::Test::HasFatalFailure())
            return;

        ASSERT_GT(CountNonBackgroundPixels(px, w, h), (w * h) / 4u)
            << "the debug frame looks empty; nothing was measured. See "
            << VisualOutputPath("Albedo_MSAA1_Decal").string();
        EXPECT_GT(CountDecalRedPixels(px, w, h), 200u)
            << "the albedo debug view has no decal in it. DeferredOpaqueDecalPass runs AFTER "
               "ScenePass, so an extraction inside ScenePass::Execute cannot see it (issue #1329) "
               "— check that GBufferDebugPass is registered after the decal node. Evidence: "
            << VisualOutputPath("Albedo_MSAA1_Decal").string();
    }

    TEST_F(GBufferDebugWithDecalScene, AlbedoDebugViewShowsTheDecalUnderResolvedMSAA)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        std::vector<u8> px;
        u32 w = 0;
        u32 h = 0;
        RenderDeferred(4u, false, kChannelAlbedo, "Albedo_MSAA4_Resolved_Decal", px, w, h);
        if (::testing::Test::HasFatalFailure())
            return;

        ASSERT_GT(CountNonBackgroundPixels(px, w, h), (w * h) / 4u)
            << "the debug frame looks empty; nothing was measured.";
        EXPECT_GT(CountDecalRedPixels(px, w, h), 200u)
            << "resolve-before-lighting MSAA: the albedo debug view has no decal in it. Evidence: "
            << VisualOutputPath("Albedo_MSAA4_Resolved_Decal").string();
    }

    TEST_F(GBufferDebugWithDecalScene, AlbedoDebugViewShowsTheDecalUnderPerSampleMSAA)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        std::vector<u8> px;
        u32 w = 0;
        u32 h = 0;
        RenderDeferred(4u, true, kChannelAlbedo, "Albedo_MSAA4_PerSample_Decal", px, w, h);
        if (::testing::Test::HasFatalFailure())
            return;

        ASSERT_GT(CountNonBackgroundPixels(px, w, h), (w * h) / 4u)
            << "the debug frame looks empty; nothing was measured.";
        EXPECT_GT(CountDecalRedPixels(px, w, h), 200u)
            << "per-sample MSAA: the albedo debug view has no decal in it. This arm additionally "
               "needs the COLOUR resolve to happen after the decals — GBufferDebugPass::Execute "
               "does it itself for exactly this reason. Evidence: "
            << VisualOutputPath("Albedo_MSAA4_PerSample_Decal").string();
    }

    // The control that makes the three assertions above mean something: the
    // identical scene with no decal entity has no red in it at all, on every
    // MSAA configuration. Without this, a red-tinted post-process stage would
    // satisfy every arm above.
    TEST_F(GBufferDebugWithoutDecalScene, AlbedoDebugViewHasNoRedWithoutTheDecal)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        struct Arm
        {
            u32 SampleCount;
            bool PerSample;
            const char* Name;
        };
        constexpr std::array<Arm, 3> arms = { Arm{ 1u, false, "Albedo_MSAA1_NoDecal" },
                                              Arm{ 4u, false, "Albedo_MSAA4_Resolved_NoDecal" },
                                              Arm{ 4u, true, "Albedo_MSAA4_PerSample_NoDecal" } };

        for (const auto& arm : arms)
        {
            std::vector<u8> px;
            u32 w = 0;
            u32 h = 0;
            RenderDeferred(arm.SampleCount, arm.PerSample, kChannelAlbedo, arm.Name, px, w, h);
            if (::testing::Test::HasFatalFailure())
                return;

            ASSERT_GT(CountNonBackgroundPixels(px, w, h), (w * h) / 4u)
                << arm.Name << ": the control frame looks empty, so its zero red count proves nothing.";
            EXPECT_LT(CountDecalRedPixels(px, w, h), 20u)
                << arm.Name << ": the decal-free control already has red pixels in it, so the "
                               "decal arms are not measuring the decal. Evidence: "
                << VisualOutputPath(arm.Name).string();
        }
    }

    // ---- Criterion 2: the capture says which frame and which version -------

    TEST_F(GBufferDebugWithDecalScene, CaptureIdentifiesItsFrameAndTheFinalGBufferVersion)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        std::vector<u8> px;
        u32 w = 0;
        u32 h = 0;
        RenderDeferred(1u, false, kChannelAlbedo, "Provenance_MSAA1_Decal", px, w, h);
        if (::testing::Test::HasFatalFailure())
            return;

        const u64 currentFrame = FrameResourceManager::Get().GetTotalFrameCount();
        const DebugViewProvenance record = DebugViewProvenanceRegistry::Get();

        ASSERT_TRUE(record.Valid) << "no debug capture was recorded although a channel is selected";
        // The producer stamps the in-flight frame and this runs after EndFrame
        // incremented the completed count, so one frame of daylight is the
        // expected reading and two is the staleness the record is here to name.
        EXPECT_GE(record.Frame + 1u, currentFrame)
            << "the capture record names frame " << record.Frame << " while the renderer has completed "
            << currentFrame << " — a stale image is being presented as current.";
        EXPECT_TRUE(DebugViewProvenanceRegistry::IsCurrent(currentFrame));
        EXPECT_EQ(record.Channel, kChannelAlbedo);
        EXPECT_EQ(record.Pass, "GBufferDebugPass");

        // The load-bearing one. The G-Buffer's content version is bumped by
        // every writer; the version the capture read must be the one still
        // standing at the end of the frame. Under the pre-#1329 arrangement
        // the capture ran at ScenePass's version and the decal pass bumped it
        // afterwards, so these two numbers disagreed by one.
        SceneRenderPass* geometry = GeometryPass();
        ASSERT_TRUE(geometry) << "no geometry render-stream node";
        const Ref<GBuffer>& gbuffer = geometry->GetGBuffer();
        ASSERT_TRUE(gbuffer) << "the deferred G-Buffer is absent";
        EXPECT_EQ(record.GBufferWriteVersion, gbuffer->GetWriteVersion())
            << "the debug view was extracted at G-Buffer version " << record.GBufferWriteVersion
            << " but the frame ended at version " << gbuffer->GetWriteVersion()
            << " — a late writer changed the attachments after the capture (issue #1329).";
        // Not a restatement of the line above: GBufferFinalVersion is raised by
        // GBuffer::MarkWritten, never by the extracting pass, so these two
        // agreeing means no writer landed afterwards — the same fact reported
        // by the mechanism that would notice if one had.
        EXPECT_EQ(record.GBufferWriteVersion, record.GBufferFinalVersion)
            << "a G-Buffer write landed after the extraction published; the capture is of an "
               "intermediate version (last writer reported: "
            << record.LastGBufferWriter << ").";
        EXPECT_EQ(record.Stage, DebugViewStage::FinalGBuffer);
        EXPECT_STREQ(gbuffer->GetLastWriter(), "DeferredOpaqueDecalPass")
            << "the decal pass was expected to be the last G-Buffer writer in this scene; it "
               "reports '"
            << gbuffer->GetLastWriter()
            << "'. If that is a deliberate reordering, the capture assertions above move with it.";

        // The freshness verdict is stated in words, not left to the reader.
        EXPECT_NE(DebugViewProvenanceRegistry::Describe(currentFrame).find("current (frame"),
                  std::string::npos);
    }

    TEST_F(GBufferDebugWithDecalScene, NoCaptureIsRecordedWhileTheDebugViewIsOff)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        std::vector<u8> onPx;
        u32 w = 0;
        u32 h = 0;
        RenderDeferred(1u, false, kChannelAlbedo, "Provenance_On", onPx, w, h);
        if (::testing::Test::HasFatalFailure())
            return;
        ASSERT_TRUE(DebugViewProvenanceRegistry::Get().Valid);

        // Switching the channel off must RETIRE the record. Leaving the last
        // one behind is the "stale data presented as current" half of the
        // criterion: the pixels on screen are now the lit frame, and a record
        // claiming an albedo capture would describe something that is no
        // longer there.
        std::vector<u8> offPx;
        RenderDeferred(1u, false, kChannelOff, "Provenance_Off_BeautyFrame", offPx, w, h);
        if (::testing::Test::HasFatalFailure())
            return;

        const DebugViewProvenance record = DebugViewProvenanceRegistry::Get();
        EXPECT_FALSE(record.Valid)
            << "the debug channel is off but a capture record survives, naming frame " << record.Frame;
        EXPECT_EQ(DebugViewProvenanceRegistry::Describe(FrameResourceManager::Get().GetTotalFrameCount()),
                  "no debug view extracted");

        // ...and the beauty frame is a LIT frame, not the leftover blit. The
        // claim is a difference, so it is measured as one: the debug view is
        // raw albedo and the beauty frame is that albedo through the whole
        // lighting and post chain, so most of the frame has to move. A count,
        // not an eyeball -- "it looks about the same" would pass on a debug
        // blit that simply never got overwritten.
        ASSERT_GT(CountNonBackgroundPixels(offPx, w, h), (w * h) / 4u)
            << "the lit frame looks empty. Evidence: "
            << VisualOutputPath("Provenance_Off_BeautyFrame").string();
        ASSERT_EQ(onPx.size(), offPx.size());
        u32 movedPixels = 0;
        for (std::size_t i = 0; i < offPx.size(); i += 4)
        {
            const int dr = std::abs(static_cast<int>(onPx[i]) - static_cast<int>(offPx[i]));
            const int dg = std::abs(static_cast<int>(onPx[i + 1]) - static_cast<int>(offPx[i + 1]));
            const int db = std::abs(static_cast<int>(onPx[i + 2]) - static_cast<int>(offPx[i + 2]));
            if (dr + dg + db > 12)
                ++movedPixels;
        }
        // Measured 2026-09-21 (NVIDIA, GL): 43 301 of 102 400 pixels, 42%. The
        // rest of the frame is the flat wall, whose lit grey happens to land
        // close to its albedo grey -- which is exactly why the threshold is a
        // quarter of the frame and not "most of it".
        EXPECT_GT(movedPixels, (w * h) / 4u)
            << "only " << movedPixels << " of " << (w * h)
            << " pixels differ between the albedo debug view and the lit frame (42% is the measured "
               "figure) — the debug blit ran with the channel off, or DeferredLightingPass did not. "
               "Evidence: "
            << VisualOutputPath("Provenance_Off_BeautyFrame").string() << " vs "
            << VisualOutputPath("Provenance_On").string();
    }

    // ---- Criterion 1 (graph ordering) -------------------------------------

    TEST_F(GBufferDebugWithDecalScene, DebugExtractionIsScheduledAfterEveryLateGBufferWriter)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        std::vector<u8> px;
        u32 w = 0;
        u32 h = 0;
        RenderDeferred(1u, false, kChannelAlbedo, "Ordering_MSAA1_Decal", px, w, h);
        if (::testing::Test::HasFatalFailure())
            return;

        const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph();
        ASSERT_TRUE(graph) << "no active render graph";
        const auto& order = graph->GetExecutionOrder();

        const auto indexOf = [&order](std::string_view name) -> std::ptrdiff_t
        {
            const auto it = std::ranges::find(order, name);
            return it == order.end() ? -1 : std::distance(order.begin(), it);
        };

        const auto debugIdx = indexOf("GBufferDebugPass");
        ASSERT_GE(debugIdx, 0)
            << "GBufferDebugPass is not in the deferred execution order — it was culled, or never "
               "registered. Its Setup declares its reads unconditionally precisely so that cannot "
               "happen behind a runtime toggle (issue #1315).";

        // Every G-Buffer writer that is present must come first. A writer that
        // is not registered in this configuration is not asserted about — the
        // test states which ones it actually checked.
        for (const char* writer : { "ScenePass", "VirtualGeometryPass", "DeferredGPUOcclusionPass",
                                    "DeferredOpaqueDecalPass" })
        {
            const auto writerIdx = indexOf(writer);
            if (writerIdx < 0)
                continue;
            EXPECT_LT(writerIdx, debugIdx)
                << writer << " runs AFTER GBufferDebugPass, so the debug image predates its writes "
                             "(issue #1329).";
        }
        EXPECT_GE(indexOf("DeferredOpaqueDecalPass"), 0)
            << "the decal node is absent from the deferred graph, so this ordering test checked "
               "nothing about the late writer the issue names.";

        const auto lightingIdx = indexOf("DeferredLightingPass");
        ASSERT_GE(lightingIdx, 0) << "DeferredLightingPass is absent from the deferred graph";
        EXPECT_LT(debugIdx, lightingIdx)
            << "the debug extraction runs after lighting, which would read a G-Buffer lighting has "
               "already consumed and put the blit on top of the lit frame by accident rather than "
               "by design.";
    }

    // ---- Criterion 3: integer IDs and packed flags are never averaged ------
    //
    // THE SUBSET RULE. An MSAA resolve of a continuous attribute is supposed to
    // produce values that were never written — that is what averaging IS, and
    // for albedo or a normal it is correct. For RT4's picking entity ID and
    // RT2's packed material-flag bitfield it is a defect with no symptom: the
    // average of two valid codes is a plausible number that names an entity
    // nobody clicked or a material nobody authored. So the assertion is not a
    // threshold; it is that the SET of values present after the resolve is a
    // subset of the set present without it, measured on the identical scene.
    //
    // The scene is built for it: a ClosureV2 sphere over a Legacy wall puts two
    // different entity IDs and two different flag codes on either side of a
    // silhouette, at every MSAA sample, and the wall fills the frame so no
    // cleared texel contributes a value no writer produced.
    class GBufferResolveLaneScene : public GBufferDebugFinalWriterScene
    {
      protected:
        void SetUp() override
        {
            m_SpawnDecal = true;
            RendererAttachedTest::SetUp();
        }

        struct Lanes
        {
            std::set<i32> EntityIDs;
            std::set<i32> FlagCodes;
            u32 SampledPixels = 0;
        };

        // Render one configuration and read the RESOLVED G-Buffer's integer
        // and flags lanes back. Reads the single-sample sampling framebuffer —
        // the one every non-per-sample consumer and the picking blit use.
        void CaptureLanes(u32 sampleCount, bool perSampleLighting, u32 debugChannel,
                          const char* caseName, Lanes& out)
        {
            std::vector<u8> px;
            u32 w = 0;
            u32 h = 0;
            RenderDeferred(sampleCount, perSampleLighting, debugChannel, caseName, px, w, h);
            if (::testing::Test::HasFatalFailure())
                return;

            SceneRenderPass* geometry = GeometryPass();
            ASSERT_TRUE(geometry) << "no geometry render-stream node";
            const Ref<GBuffer>& gbuffer = geometry->GetGBuffer();
            ASSERT_TRUE(gbuffer) << "the deferred G-Buffer is absent";

            const u32 gw = gbuffer->GetWidth();
            const u32 gh = gbuffer->GetHeight();
            ASSERT_GT(gw, 0u);
            ASSERT_GT(gh, 0u);

            const u32 entityTex = gbuffer->GetColorAttachmentID(GBuffer::EntityID);
            const u32 emissiveTex = gbuffer->GetColorAttachmentID(GBuffer::Emissive);
            ASSERT_NE(entityTex, 0u) << "the G-Buffer carries no entity-ID attachment";
            ASSERT_NE(emissiveTex, 0u) << "the G-Buffer carries no emissive/flags attachment";

            std::vector<i32> ids;
            ReadbackRedInteger(entityTex, gw, gh, ids);
            std::vector<f32> emissive;
            ReadbackRgbaFloat(emissiveTex, gw, gh, emissive);
            ASSERT_EQ(ids.size(), static_cast<std::size_t>(gw) * gh);
            ASSERT_EQ(emissive.size(), static_cast<std::size_t>(gw) * gh * 4u);

            out = {};
            for (std::size_t i = 0; i < ids.size(); ++i)
            {
                out.EntityIDs.insert(ids[i]);
                out.FlagCodes.insert(FlagsKey(emissive[i * 4u + 3u]));
                ++out.SampledPixels;
            }
        }
    };

    TEST_F(GBufferResolveLaneScene, ResolvedMSAAInventsNoEntityIDAndNoMaterialFlagCode)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // The reference: no MSAA, so no resolve, so every value in the buffer
        // was written by a fragment shader. This set is the whole assertion.
        Lanes reference;
        CaptureLanes(1u, false, kChannelOff, "Lanes_MSAA1_Reference", reference);
        if (::testing::Test::HasFatalFailure())
            return;
        ASSERT_GE(reference.EntityIDs.size(), 2u)
            << "the reference frame holds fewer than two entity IDs, so a resolve could not "
               "average two of them and this test would pass on an empty scene.";
        ASSERT_GE(reference.FlagCodes.size(), 2u)
            << "the reference frame holds fewer than two distinct material-flag codes; the "
               "ClosureV2 sphere over the Legacy wall is what is supposed to provide them.";

        struct Arm
        {
            u32 SampleCount;
            bool PerSample;
            u32 Channel;
            const char* Name;
        };
        // Both MSAA sub-modes the issue names. The per-sample arm runs with the
        // debug channel ON because that is what makes the colour resolve happen
        // at all on that path — and it is the arm #1329 moved.
        const std::array<Arm, 2> arms = {
            Arm{ 4u, false, kChannelOff, "Lanes_MSAA4_Resolved" },
            Arm{ 4u, true, kChannelAlbedo, "Lanes_MSAA4_PerSample" },
        };

        for (const auto& arm : arms)
        {
            Lanes resolved;
            CaptureLanes(arm.SampleCount, arm.PerSample, arm.Channel, arm.Name, resolved);
            if (::testing::Test::HasFatalFailure())
                return;

            ASSERT_GT(resolved.SampledPixels, 0u) << arm.Name << ": nothing was read back";

            std::vector<i32> inventedIDs;
            std::ranges::set_difference(resolved.EntityIDs, reference.EntityIDs,
                                        std::back_inserter(inventedIDs));
            std::string idList;
            for (const i32 id : inventedIDs)
                idList += (idList.empty() ? "" : ", ") + std::to_string(id);
            EXPECT_TRUE(inventedIDs.empty())
                << arm.Name << ": the MSAA resolve produced entity ID(s) no fragment wrote: " << idList
                << ". An averaged picking ID selects an entity nobody clicked (issue #1329, "
                   "criterion 3). The reference (no MSAA) frame holds "
                << reference.EntityIDs.size() << " distinct IDs.";

            std::vector<i32> inventedFlags;
            std::ranges::set_difference(resolved.FlagCodes, reference.FlagCodes,
                                        std::back_inserter(inventedFlags));
            std::string flagList;
            for (const i32 code : inventedFlags)
                flagList += (flagList.empty() ? "" : ", ") + std::to_string(code / 1024.0f);
            EXPECT_TRUE(inventedFlags.empty())
                << arm.Name << ": the MSAA resolve produced packed material-flag value(s) nothing "
                               "authored: "
                << flagList
                << ". That bitfield decodes to a material combination no one wrote — see "
                   "GBuffer::ResolveFlagsLane / GBufferFlagsResolve.glsl (issue #996), which this "
                   "test is the value-level guard for.";
        }
    }

    // The other half of criterion 3: picking. The entity-ID lane is what the
    // editor's selection reads, so a change to the debug path that moved,
    // cleared or re-ordered it would show up here — the IDs present in the
    // beauty frame must be exactly the ones the scene's meshes carry.
    TEST_F(GBufferResolveLaneScene, PickingIDsAreUnchangedByTheDebugPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        Lanes beauty;
        CaptureLanes(1u, false, kChannelOff, "Picking_MSAA1_Beauty", beauty);
        if (::testing::Test::HasFatalFailure())
            return;

        Lanes withDebugView;
        CaptureLanes(1u, false, kChannelAlbedo, "Picking_MSAA1_DebugView", withDebugView);
        if (::testing::Test::HasFatalFailure())
            return;

        EXPECT_EQ(beauty.EntityIDs, withDebugView.EntityIDs)
            << "selecting a G-Buffer debug channel changed the set of picking entity IDs in the "
               "G-Buffer. The debug extraction writes the SCENE colour target, never RT4, so this "
               "is a regression in the extraction's draw-buffer or read-buffer restore.";
    }
} // namespace OloEngine::Tests

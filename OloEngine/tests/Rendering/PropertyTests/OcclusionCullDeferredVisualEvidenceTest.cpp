// OLO_TEST_LAYER: L8
// =============================================================================
// OcclusionCullDeferredVisualEvidenceTest.cpp  (#486)
//
// Full-pipeline visual evidence for the DEFERRED two-phase GPU Hi-Z occlusion
// cull (#486 extends #431's Forward/Forward+ scheme to the Deferred path). It
// drives the REAL Scene pipeline (`Scene::OnUpdateRuntime` -> Renderer3D render
// graph) with `RenderingPath::Deferred` active, a large occluder wall in front
// of a DENSE instanced cube field (> the GPU-cull threshold, so the batch routes
// through `Renderer3D::SubmitGPUCulledInstanced` and now the deferred two-phase
// route: phase 1 through the ScenePass G-Buffer bucket, phase 2 through
// `DeferredGPUOcclusionPass`), then reads the composited frame back.
//
// The sibling forward test is OcclusionCullVisualEvidenceTest. This one proves
// the SAME "no false hole / no corruption" contract holds through the deferred
// G-Buffer pipeline with the new phase-2 pass registered and executing:
//   * enabling deferred two-phase occlusion must not flip a meaningful fraction
//     of pixels vs the occlusion-OFF baseline (a false cull of visible geometry
//     would punch a large-delta hole), and
//   * the frame stays healthy (not flat / not blanked) — the new phase-2 HZB
//     rebuild + G-Buffer re-export must leave the deferred pipeline intact.
//
// Correct occlusion is invisible in the final image (culled instances were
// already hidden behind the wall); the cull's positive behaviour is proven on
// the CPU by DeferredTwoPhaseOcclusionTest / GPUOcclusionCullParityTest.
//
// The ON frame is always written to
//   OloEditor/assets/tests/visual/OcclusionCull_Deferred_VisualEvidence.png
//
// Classification: L8 / integration (full deferred GL pipeline through the real
// Scene render path, RGBA8 readback + PNG). SKIPs cleanly without a GL 4.6
// context.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/Instancing/InstanceData.h"
#include "OloEngine/Renderer/Instancing/InstancedMeshComponent.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;
        constexpr u32 kSize = 256;

        f32 FractionChanged(const std::vector<u8>& a, const std::vector<u8>& b, u8 threshold)
        {
            const std::size_t n = std::min(a.size(), b.size());
            if (n == 0)
                return 1.0f;
            std::size_t changed = 0;
            std::size_t pixels = 0;
            for (std::size_t i = 0; i + 3 < n; i += 4)
            {
                const int dr = std::abs(static_cast<int>(a[i + 0]) - static_cast<int>(b[i + 0]));
                const int dg = std::abs(static_cast<int>(a[i + 1]) - static_cast<int>(b[i + 1]));
                const int db = std::abs(static_cast<int>(a[i + 2]) - static_cast<int>(b[i + 2]));
                if (dr > threshold || dg > threshold || db > threshold)
                    ++changed;
                ++pixels;
            }
            return pixels ? static_cast<f32>(changed) / static_cast<f32>(pixels) : 1.0f;
        }

        f32 LuminanceSpread(const std::vector<u8>& px)
        {
            f32 mn = 1.0f, mx = 0.0f;
            for (std::size_t i = 0; i + 3 < px.size(); i += 4)
            {
                const f32 l = (0.2126f * px[i] + 0.7152f * px[i + 1] + 0.0722f * px[i + 2]) / 255.0f;
                mn = std::min(mn, l);
                mx = std::max(mx, l);
            }
            return mx - mn;
        }

        fs::path VisualOutputPath()
        {
            fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir / "OcclusionCull_Deferred_VisualEvidence.png";
        }
    } // namespace

    // -------------------------------------------------------------------------
    // Same occluded instance field as the forward test: a wall in front of >1024
    // instanced cubes, plus visible sentinels IN FRONT of the wall that must
    // survive occlusion (so a false cull shows up as a hole in the diff).
    // -------------------------------------------------------------------------
    class DeferredOccludedInstanceFieldScene : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Entity camera = GetScene().CreateEntity("Camera");
            camera.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 10.0f };
            auto& cam = camera.AddComponent<CameraComponent>();
            cam.Primary = true;
            cam.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);

            Entity light = GetScene().CreateEntity("KeyLight");
            light.GetComponent<TransformComponent>().Translation = { 3.0f, 4.0f, 6.0f };
            auto& pl = light.AddComponent<PointLightComponent>();
            pl.m_Color = { 1.0f, 1.0f, 1.0f };
            pl.m_Intensity = 60.0f;
            pl.m_Range = 60.0f;

            Ref<Mesh> cube = MeshPrimitives::CreateCube();

            // Near occluder wall (non-instanced MeshComponent → deferred PBR
            // G-Buffer shader) covering the screen centre at z = +2.
            Entity wall = GetScene().CreateEntity("Occluder");
            wall.AddComponent<MeshComponent>(cube->GetMeshSource());
            auto& wt = wall.GetComponent<TransformComponent>();
            wt.Translation = { 0.0f, 0.0f, 2.0f };
            wt.Scale = { 7.0f, 7.0f, 0.5f };

            // Dense instanced cube field far BEHIND the wall (z = -20), > the GPU
            // cull threshold (1024) so it routes through SubmitGPUCulledInstanced.
            Entity field = GetScene().CreateEntity("InstancedField");
            auto& imc = field.AddComponent<InstancedMeshComponent>();
            imc.MeshSource = cube->GetMeshSource();
            imc.CastShadows = false;
            constexpr i32 kGrid = 34; // 34*34 = 1156 > 1024
            imc.Instances.reserve(static_cast<sizet>(kGrid) * kGrid);
            for (i32 gy = 0; gy < kGrid; ++gy)
            {
                for (i32 gx = 0; gx < kGrid; ++gx)
                {
                    const f32 x = (static_cast<f32>(gx) / static_cast<f32>(kGrid - 1) - 0.5f) * 11.0f;
                    const f32 y = (static_cast<f32>(gy) / static_cast<f32>(kGrid - 1) - 0.5f) * 11.0f;
                    InstanceData inst;
                    inst.Transform = glm::scale(
                        glm::translate(glm::mat4(1.0f), glm::vec3(x, y, -20.0f)),
                        glm::vec3(0.15f));
                    inst.PrevTransform = inst.Transform;
                    imc.Instances.push_back(inst);
                }
            }

            // Sentinels IN FRONT of the wall (z = +5) that are genuinely visible
            // and MUST survive occlusion — a false cull punches a hole the diff sees.
            for (i32 sy = 0; sy < 4; ++sy)
            {
                for (i32 sx = 0; sx < 4; ++sx)
                {
                    const f32 x = (static_cast<f32>(sx) / 3.0f - 0.5f) * 4.0f;
                    const f32 y = (static_cast<f32>(sy) / 3.0f - 0.5f) * 4.0f;
                    InstanceData inst;
                    inst.Transform = glm::scale(
                        glm::translate(glm::mat4(1.0f), glm::vec3(x, y, 5.0f)),
                        glm::vec3(0.3f));
                    inst.PrevTransform = inst.Transform;
                    inst.Color = glm::vec4(1.0f, 0.3f, 0.2f, 1.0f);
                    imc.Instances.push_back(inst);
                }
            }

            EnableRendering(kSize, kSize);
        }
    };

    // RAII: restore the process-wide RenderingPath + HZB occlusion toggle on
    // scope exit so a failing ASSERT can't leave Deferred / occlusion enabled for
    // later tests in the same process.
    struct DeferredHZBRestore
    {
        RenderingPath m_PrevPath = Renderer3D::GetRendererSettings().Path;
        bool m_PrevHZB = Renderer3D::IsHZBOcclusionCullingEnabled();
        ~DeferredHZBRestore()
        {
            Renderer3D::GetRendererSettings().Path = m_PrevPath;
            Renderer3D::EnableHZBOcclusionCulling(m_PrevHZB);
            Renderer3D::ApplyRendererSettings();
        }
    };

    TEST_F(DeferredOccludedInstanceFieldScene, DeferredTwoPhaseOcclusionDoesNotCorruptOrHoleTheFrame)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const DeferredHZBRestore restore;

        // Switch to the Deferred render path (rebuilds the graph so
        // DeferredGPUOcclusionPass is registered). MSAA stays at the default 1.
        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        // Baseline: HZB occlusion OFF (deferred single-phase frustum cull). Warm
        // TAA / velocity history so the image is stable.
        Renderer3D::EnableHZBOcclusionCulling(false);
        RunFrames(4);
        std::vector<u8> baseline;
        u32 w = 0, h = 0;
        ASSERT_TRUE(ReadbackComposite(baseline, w, h)) << "ReadbackComposite failed (deferred, occlusion off)";
        EXPECT_EQ(w, kSize);
        EXPECT_EQ(h, kSize);
        EXPECT_GT(LuminanceSpread(baseline), 0.05f)
            << "deferred baseline frame is nearly flat — the occluder wall may not have drawn in the G-Buffer";

        // Enable deferred two-phase HZB occlusion. Frame 1 populates the retained
        // pyramid; subsequent frames run phase 1 (ScenePass G-Buffer) + phase 2
        // (DeferredGPUOcclusionPass) and cull the instances behind the wall.
        Renderer3D::EnableHZBOcclusionCulling(true);
        RunFrames(4);
        std::vector<u8> occluded;
        ASSERT_TRUE(ReadbackComposite(occluded, w, h)) << "ReadbackComposite failed (deferred, occlusion on)";

        const fs::path out = VisualOutputPath();
        const int wrote = ::stbi_write_png(out.string().c_str(),
                                           static_cast<int>(w), static_cast<int>(h),
                                           4, occluded.data(), static_cast<int>(w) * 4);
        EXPECT_NE(wrote, 0) << "failed to write deferred visual evidence PNG to " << out.string();

        // No-hole / no-corruption contract: enabling deferred two-phase occlusion
        // must not flip a meaningful fraction of pixels vs the occlusion-off
        // baseline. Same slack as the forward test (delta > 48/255 on < 2% of px).
        const f32 changed = FractionChanged(baseline, occluded, /*threshold*/ 48u);
        EXPECT_LT(changed, 0.02f)
            << "enabling deferred two-phase HZB occlusion changed " << (changed * 100.0f)
            << "% of pixels vs the occlusion-off baseline — a false cull may be punching a "
               "hole in visible geometry (phase-2 disocclusion recovery or G-Buffer re-export "
               "may be wrong); see "
            << out.string();

        EXPECT_GT(LuminanceSpread(occluded), 0.05f)
            << "deferred occlusion-on frame went flat — the phase-2 pass may have broken the "
               "G-Buffer / lighting; see "
            << out.string();
    }

    // -------------------------------------------------------------------------
    // Issue #530 regression: re-entering the Deferred path a second time in one
    // process must not cull the entire graph.
    //
    // ConfigureRenderGraph() -> RenderGraph::ResetTopology() wipes the graph's
    // blackboard + imported-resource maps on every path switch, but the
    // RenderPipeline's blackboard-populate cache is keyed on a fingerprint that
    // (before the fix) hashed only scene/settings inputs — identical for the
    // same Deferred scene across two entries. So the second entry recomputed a
    // matching fingerprint, short-circuited PopulateBlackboard, and left the
    // just-wiped blackboard empty; every pass's Setup() then read empty handles,
    // RGBuilder dropped every declaration, and the whole 37-pass graph culled
    // (reads=0/writes=0) — a blank composite with no GL error.
    //
    // The repro is a reconfigure -> reconfigure sequence with NO frame rendered
    // in between (mirrors the fixture TearDown path-restore followed by the next
    // test's SetUp path-switch), which is what leaves the cache primed with the
    // prior Deferred fingerprint. The fix hashes RenderGraph::GetTopologyGeneration()
    // — bumped by every ResetTopology() — so the second entry's fingerprint can
    // never match the first's cached one. A blank second frame (LuminanceSpread
    // collapses to ~0) is the observable symptom of the cull.
    // -------------------------------------------------------------------------
    TEST_F(DeferredOccludedInstanceFieldScene, ReenteringDeferredPathDoesNotCullEntireGraph_Issue530)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const DeferredHZBRestore restore;

        // First Deferred entry: configure + render so the blackboard is
        // populated and the populate-cache fingerprint is primed.
        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();
        RunFrames(2);
        std::vector<u8> firstFrame;
        u32 w = 0, h = 0;
        ASSERT_TRUE(ReadbackComposite(firstFrame, w, h)) << "first Deferred entry: ReadbackComposite failed";
        ASSERT_GT(LuminanceSpread(firstFrame), 0.05f)
            << "first Deferred entry already rendered a flat frame — test scene is broken";

        // Reconfigure AWAY from Deferred and back with no frame in between. The
        // blackboard is wiped twice by ResetTopology, but the populate cache
        // still holds the Deferred fingerprint from the first entry.
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        // Second Deferred entry: identical scene/settings, so every fingerprint
        // input EXCEPT the topology generation matches the first entry. Without
        // the #530 fix the populate cache hits, the blackboard stays empty, and
        // this frame is blank.
        RunFrames(2);
        std::vector<u8> secondFrame;
        ASSERT_TRUE(ReadbackComposite(secondFrame, w, h))
            << "second Deferred entry: ReadbackComposite failed — the graph was likely fully culled";
        EXPECT_GT(LuminanceSpread(secondFrame), 0.05f)
            << "issue #530: re-entering the Deferred path produced a blank frame — the whole render "
               "graph was culled because PopulateBlackboard's fingerprint cache short-circuited past "
               "the ResetTopology() blackboard wipe (topology-generation hashing missing/broken)";
    }
} // namespace OloEngine::Tests

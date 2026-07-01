// OLO_TEST_LAYER: L8
// =============================================================================
// OcclusionCullVisualEvidenceTest.cpp  (#431)
//
// Full-pipeline visual evidence for GPU Hi-Z occlusion culling. Drives the REAL
// Scene pipeline (`Scene::OnUpdateRuntime` -> Renderer3D render graph) with a
// large occluder wall in front of a DENSE instanced cube field (> the GPU-cull
// threshold, so the batch routes through `Renderer3D::SubmitGPUCulledInstanced`
// and the new InstanceOcclusionCull.comp), then reads the composited frame back.
//
// Why this complements the other two tests:
//   * GPUOcclusionCullParityTest      — pins the cull math on the CPU.
//   * GPUOcclusionCullGPUTest         — runs the real shader on the GPU and
//                                       reads back the survivor count.
//   * THIS                            — proves that, end-to-end through the real
//                                       render graph, enabling occlusion does
//                                       NOT corrupt the frame or punch holes in
//                                       visible geometry (no false culls), and
//                                       that the new post-Execute HZB-generation
//                                       pass leaves the pipeline healthy.
//
// Correct occlusion is, by construction, invisible in the final image — the
// culled instances were already hidden behind the wall. So the contract is a
// "no regression" one: the occlusion-ON frame must match the occlusion-OFF
// baseline (within TAA jitter). A false cull of *visible* geometry would show
// up as a large-delta region and fail. The cull's *positive* behaviour (it
// really drops survivors) is proven by GPUOcclusionCullGPUTest.
//
// The ON frame is always written to
//   OloEditor/assets/tests/visual/OcclusionCull_VisualEvidence.png
//
// Classification: L8 / integration (full GL pipeline through the real Scene
// render path, RGBA8 readback + PNG). SKIPs cleanly without a GL 4.6 context.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
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

        // Fraction of pixels whose per-channel delta exceeds `threshold`.
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
            return dir / "OcclusionCull_VisualEvidence.png";
        }
    } // namespace

    // -------------------------------------------------------------------------
    // OccludedInstanceFieldScene — a wall in front of >1024 instanced cubes.
    // -------------------------------------------------------------------------
    class OccludedInstanceFieldScene : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            // Camera at +Z looking down -Z (OloEngine convention).
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

            // A near occluder wall covering the screen centre at z = +2. Sized
            // to cover the instanced field's screen footprint with margin while
            // leaving the screen corners as visible background (so the frame has
            // real wall-vs-background contrast for the spread assertion).
            Entity wall = GetScene().CreateEntity("Occluder");
            wall.AddComponent<MeshComponent>(cube->GetMeshSource());
            auto& wt = wall.GetComponent<TransformComponent>();
            wt.Translation = { 0.0f, 0.0f, 2.0f };
            wt.Scale = { 7.0f, 7.0f, 0.5f };

            // A dense instanced cube field far BEHIND the wall (z = -20),
            // spread across the screen footprint so every instance lands behind
            // the occluder. >1024 instances so the batch routes through the GPU
            // cull path (threshold is 1024).
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
                    inst.PrevTransform = inst.Transform; // static — prev == current
                    imc.Instances.push_back(inst);
                }
            }

            // Sentinels: a few instances IN FRONT of the wall (z = +5, between
            // the camera at z=10 and the wall at z=2) that are genuinely visible
            // and MUST survive occlusion. Without them every instance is hidden
            // behind the wall, so even a (wrong) full cull would leave the
            // visible frame unchanged and the diff below would not detect a false
            // cull. With them, a false cull punches a visible hole the diff sees.
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
                    inst.Color = glm::vec4(1.0f, 0.3f, 0.2f, 1.0f); // distinct from the wall
                    imc.Instances.push_back(inst);
                }
            }

            EnableRendering(kSize, kSize);
        }
    };

    // RAII: restore the process-wide HZB occlusion toggle on scope exit so a
    // failing ASSERT (which returns out of the test mid-flow) can't leave it
    // enabled for later tests in the same process.
    struct HZBOcclusionRestore
    {
        bool m_Prev = Renderer3D::IsHZBOcclusionCullingEnabled();
        ~HZBOcclusionRestore()
        {
            Renderer3D::EnableHZBOcclusionCulling(m_Prev);
        }
    };

    TEST_F(OccludedInstanceFieldScene, OcclusionDoesNotCorruptOrHoleTheFrame)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const HZBOcclusionRestore hzbRestore;

        // Baseline: HZB occlusion OFF. Frustum culling stays on (the instanced
        // field still routes through the GPU frustum cull). A few frames warm
        // the TAA / velocity history so the image is stable.
        Renderer3D::EnableHZBOcclusionCulling(false);
        RunFrames(4);
        std::vector<u8> baseline;
        u32 w = 0, h = 0;
        ASSERT_TRUE(ReadbackComposite(baseline, w, h)) << "ReadbackComposite failed (occlusion off)";
        EXPECT_EQ(w, kSize);
        EXPECT_EQ(h, kSize);

        // The baseline must be a real rendered frame (wall + lit background),
        // otherwise the comparison below is meaningless.
        EXPECT_GT(LuminanceSpread(baseline), 0.05f)
            << "baseline frame is nearly flat — the occluder wall may not have drawn";

        // Now enable HZB occlusion. Frame 1 populates the retained pyramid;
        // subsequent frames cull the instances hidden behind the wall. The
        // visible image (dominated by the wall) must stay essentially identical.
        Renderer3D::EnableHZBOcclusionCulling(true);
        RunFrames(4);
        std::vector<u8> occluded;
        ASSERT_TRUE(ReadbackComposite(occluded, w, h)) << "ReadbackComposite failed (occlusion on)";

        // Always write the evidence PNG first.
        const fs::path out = VisualOutputPath();
        const int wrote = ::stbi_write_png(out.string().c_str(),
                                           static_cast<int>(w), static_cast<int>(h),
                                           4, occluded.data(), static_cast<int>(w) * 4);
        EXPECT_NE(wrote, 0) << "failed to write visual evidence PNG to " << out.string();

        // No-hole / no-corruption contract: enabling occlusion must not flip a
        // meaningful fraction of pixels. A false cull of visible geometry would
        // change a large region; TAA jitter only nudges a few pixels by a few
        // levels. Allow a small slack for jitter (delta > 48/255 on < 2% of px).
        const f32 changed = FractionChanged(baseline, occluded, /*threshold*/ 48u);
        EXPECT_LT(changed, 0.02f)
            << "enabling HZB occlusion changed " << (changed * 100.0f)
            << "% of pixels vs the occlusion-off baseline — a false cull may be "
               "punching a hole in visible geometry; see "
            << out.string();

        // The occluded-frame must still be a real frame (the wall is still
        // there — occlusion only drops the hidden instances behind it).
        EXPECT_GT(LuminanceSpread(occluded), 0.05f)
            << "occlusion-on frame went flat — the pipeline may have broken; see " << out.string();

        // (HZB occlusion toggle restored by hzbRestore on scope exit.)
    }
} // namespace OloEngine::Tests

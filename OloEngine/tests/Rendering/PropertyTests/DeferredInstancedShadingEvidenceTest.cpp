// OLO_TEST_LAYER: L8
// =============================================================================
// DeferredInstancedShadingEvidenceTest.cpp  (#515)
//
// Full-pipeline visual evidence that instanced statics are correctly lit when
// routed through the Deferred G-Buffer pipeline. Before #515, both instanced
// submission paths — `Renderer3D::DrawMeshInstanced` (CPU frustum-cull path,
// below `GPUCullThreshold`) and `Renderer3D::SubmitGPUCulledInstanced` (GPU
// compute-cull path, at/above `GPUCullThreshold`) — always picked
// `DefaultForwardShader` for a material with no explicit shader override,
// even when `RendererSettings::Path == Deferred`. `DefaultForwardShader`
// writes the forward MRT outputs (o_Color / o_EntityID / o_ViewNormal /
// o_Velocity), which alias onto the G-Buffer attachment slots
// (Albedo / Normal / Emissive / Velocity) — so instanced surfaces wrote
// garbage into the G-Buffer and DeferredLightingPass composited them as dark,
// unlit squares (see the #486 PR's `OcclusionCull_Deferred_VisualEvidence.png`
// evidence, which first surfaced this).
//
// This test drives the REAL Scene pipeline (`Scene::OnUpdateRuntime` ->
// Renderer3D render graph) with `RenderingPath::Deferred` active and TWO
// side-by-side `InstancedMeshComponent` fields using the default PBR material
// (no `MaterialComponent` / `OverrideMaterial` — the common case), so both
// instanced submission paths run in the SAME frame of the SAME scene:
//   * Left  field (< GPUCullThreshold)  -> Renderer3D::DrawMeshInstanced (CPU cull).
//   * Right field (>= GPUCullThreshold) -> Renderer3D::SubmitGPUCulledInstanced
//     (GPU compute cull).
// Both fields in one scene/one RunFrames sequence — rather than two separate
// TEST_Fs that each flip RendererSettings::Path to Deferred in turn — sidesteps
// an unrelated pre-existing RenderGraph/test-fixture ordering fragility found
// while authoring this test (now tracked as issue #530): a *second* consecutive
// RendererAttachedTest that switches to Deferred (immediately after a prior one
// already did) could see its whole graph compile with zero declared reads/writes
// (every pass culled, ReadbackComposite fails) depending on run order. That is
// orthogonal to the #515 shader-routing bug this test targets, so it is dodged
// here rather than chased down as part of this fix.
//
// Contract: with a point light illuminating both fields, each field's region
// must be measurably brighter than an unlit background corner and above an
// absolute brightness floor — a regression back to the forward-aliased
// G-Buffer write would leave the field(s) dark/unlit, close to the background
// luminance, and this test would fail exactly as the visual evidence in #515
// showed. The #515 shader-routing fix itself was additionally verified by
// direct inspection of the runtime shader-selection decision (temporary
// logging during authoring confirmed `PBRGBufferShader` is selected, not
// `DefaultForwardShader`, for both submission paths) — the PNG this test
// writes is corroborating visual evidence, not the sole proof.
//
// The frame is always written to
//   OloEditor/assets/tests/visual/DeferredInstancedShading_VisualEvidence.png
//
// Classification: L8 / integration (full deferred GL pipeline through the
// real Scene render path, RGBA8 readback + PNG). SKIPs cleanly without a GL
// 4.6 context.
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

        f32 LuminanceAt(const std::vector<u8>& px, std::size_t idx)
        {
            const f32 r = static_cast<f32>(px[idx + 0]) / 255.0f;
            const f32 g = static_cast<f32>(px[idx + 1]) / 255.0f;
            const f32 b = static_cast<f32>(px[idx + 2]) / 255.0f;
            return 0.2126f * r + 0.7152f * g + 0.0722f * b;
        }

        f32 MeanLuminanceInRegion(const std::vector<u8>& px, u32 w, u32 h,
                                  u32 cx, u32 cy, u32 halfExtent)
        {
            const u32 x0 = (cx > halfExtent) ? cx - halfExtent : 0u;
            const u32 y0 = (cy > halfExtent) ? cy - halfExtent : 0u;
            const u32 x1 = std::min(cx + halfExtent, w);
            const u32 y1 = std::min(cy + halfExtent, h);
            f64 sum = 0.0;
            u32 count = 0;
            for (u32 y = y0; y < y1; ++y)
            {
                for (u32 x = x0; x < x1; ++x)
                {
                    sum += LuminanceAt(px, (static_cast<std::size_t>(y) * w + x) * 4);
                    ++count;
                }
            }
            return count ? static_cast<f32>(sum / count) : 0.0f;
        }

        fs::path VisualOutputPath()
        {
            fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir / "DeferredInstancedShading_VisualEvidence.png";
        }

        // Fills `imc.Instances` with a gridDim x gridDim grid of unit cubes,
        // centred at (centreX, 0, 0), spanning `span` world units per axis.
        void FillInstanceGrid(InstancedMeshComponent& imc, i32 gridDim, f32 centreX, f32 span)
        {
            imc.Instances.reserve(imc.Instances.size() + static_cast<sizet>(gridDim) * gridDim);
            for (i32 gy = 0; gy < gridDim; ++gy)
            {
                for (i32 gx = 0; gx < gridDim; ++gx)
                {
                    const f32 x = centreX + (static_cast<f32>(gx) / static_cast<f32>(gridDim - 1) - 0.5f) * span;
                    const f32 y = (static_cast<f32>(gy) / static_cast<f32>(gridDim - 1) - 0.5f) * span;
                    InstanceData inst;
                    inst.Transform = glm::scale(
                        glm::translate(glm::mat4(1.0f), glm::vec3(x, y, 0.0f)),
                        glm::vec3(span / static_cast<f32>(gridDim) * 0.85f));
                    inst.PrevTransform = inst.Transform;
                    imc.Instances.push_back(inst);
                }
            }
        }
    } // namespace

    // -------------------------------------------------------------------------
    // TwoInstancedFieldsLitScene — a left field (below GPUCullThreshold, CPU
    // frustum-cull path) and a right field (at/above GPUCullThreshold, GPU
    // compute-cull path), side by side so neither overlaps nor occludes the
    // other, lit by an off-axis point light, both using the default PBR
    // material (no MaterialComponent / OverrideMaterial — the common case
    // #515 describes).
    // -------------------------------------------------------------------------
    class TwoInstancedFieldsLitScene : public RendererAttachedTest
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
            light.GetComponent<TransformComponent>().Translation = { 0.0f, 1.0f, 8.0f };
            auto& pl = light.AddComponent<PointLightComponent>();
            pl.m_Color = { 1.0f, 1.0f, 1.0f };
            pl.m_Intensity = 800.0f;
            pl.m_Range = 100.0f;

            Ref<Mesh> cube = MeshPrimitives::CreateCube();

            // Left field: below Renderer3D's default GPUCullThreshold (1024)
            // -> routes through the CPU frustum-cull path
            // (Renderer3D::DrawMeshInstanced).
            Entity leftField = GetScene().CreateEntity("LeftField");
            auto& leftImc = leftField.AddComponent<InstancedMeshComponent>();
            leftImc.MeshSource = cube->GetMeshSource();
            leftImc.CastShadows = false;
            FillInstanceGrid(leftImc, /*gridDim*/ 8, /*centreX*/ -2.0f, /*span*/ 3.6f); // 64 instances

            // Right field: at/above the default GPUCullThreshold (1024) ->
            // routes through the GPU compute-cull path
            // (Renderer3D::SubmitGPUCulledInstanced).
            Entity rightField = GetScene().CreateEntity("RightField");
            auto& rightImc = rightField.AddComponent<InstancedMeshComponent>();
            rightImc.MeshSource = cube->GetMeshSource();
            rightImc.CastShadows = false;
            FillInstanceGrid(rightImc, /*gridDim*/ 34, /*centreX*/ 2.0f, /*span*/ 3.6f); // 1156 instances

            EnableRendering(kSize, kSize);
        }
    };

    TEST_F(TwoInstancedFieldsLitScene, BothInstancedSubmissionPathsAreLitInDeferred)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        // A few frames to stabilise TAA / velocity history.
        RunFrames(4);

        std::vector<u8> px;
        u32 w = 0, h = 0;
        ASSERT_TRUE(ReadbackComposite(px, w, h)) << "ReadbackComposite failed (deferred instanced shading)";
        EXPECT_EQ(w, kSize);
        EXPECT_EQ(h, kSize);

        const fs::path out = VisualOutputPath();
        const int wrote = ::stbi_write_png(out.string().c_str(),
                                           static_cast<int>(w), static_cast<int>(h),
                                           4, px.data(), static_cast<int>(w) * 4);
        EXPECT_NE(wrote, 0) << "failed to write visual evidence PNG to " << out.string();

        // Left field (CPU cull path) sits in the left third of the frame;
        // right field (GPU cull path) in the right third. A thin strip along
        // the very top stays background (both fields' world-space grids sit
        // well below the camera's top view-frustum edge at this distance).
        const f32 leftField = MeanLuminanceInRegion(px, w, h, w / 4, h / 2, 40);
        const f32 rightField = MeanLuminanceInRegion(px, w, h, 3 * w / 4, h / 2, 40);
        const f32 background = MeanLuminanceInRegion(px, w, h, w / 2, 8, 8);

        // Absolute brightness floor: a dark/unlit field (the pre-#515 bug —
        // forward MRT outputs aliased onto G-Buffer slots, composited as
        // near-black garbage) would fail this outright.
        EXPECT_GT(leftField, 0.10f)
            << "deferred CPU-cull-path instanced field is dark/unlit (luminance = " << leftField
            << ") — DrawMeshInstanced may still be routing to a forward-only shader on the "
               "Deferred path instead of PBRGBufferShader; see "
            << out.string();
        EXPECT_GT(rightField, 0.10f)
            << "deferred GPU-cull-path instanced field is dark/unlit (luminance = " << rightField
            << ") — SubmitGPUCulledInstanced may still be routing to a forward-only shader on the "
               "Deferred path instead of PBRGBufferShader; see "
            << out.string();

        // Contrast check: both lit fields must differ from the unlit background.
        EXPECT_GT(std::abs(leftField - background), 0.02f)
            << "deferred CPU-cull-path instanced field (" << leftField << ") does not differ from "
                                                                          "the background ("
            << background << ") — the field may not be visible; see "
            << out.string();
        EXPECT_GT(std::abs(rightField - background), 0.02f)
            << "deferred GPU-cull-path instanced field (" << rightField << ") does not differ from "
                                                                           "the background ("
            << background << ") — the field may not be visible; see "
            << out.string();
    }
} // namespace OloEngine::Tests

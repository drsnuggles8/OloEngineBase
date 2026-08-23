// OLO_TEST_LAYER: L3
//
// =============================================================================
// GPU terrain picking on a real GPU — issue #717, both acceptance criteria.
//
// The layout test next door proves the C++ and GLSL sides of the state block
// agree. It cannot prove the pass ANSWERS CORRECTLY, and "answers a plausible
// position" is exactly how this feature would ship broken: a brush cursor that
// sits a metre under the ground, or two frames behind the mouse, or on the
// wrong side of a ridge, looks like a feel problem rather than a bug.
//
// Every assertion here is cross-checked against something derived independently
// of the pass:
//
//   * THE SURFACE RESIDUAL. The returned point is fed back through
//     `TerrainData::GetHeightAt` — the CPU heightmap, untouched by any of this
//     — and must lie ON the surface. This is the assertion that does not care
//     whether the CPU raycast is right, only whether the answer is a point of
//     the terrain.
//   * THE CPU PATH. The shipped `EditorLayer::TerrainRaycastCPU` algorithm is
//     transcribed below and compared against, within one heightmap texel, which
//     is the issue's acceptance criterion in its own words.
//   * THE RING'S LATENCY. `Latency > 0` proves the answer came back through the
//     fenced ring at least one frame after the dispatch that produced it. A
//     synchronous readback would publish at latency 0, so this is the assertion
//     that "no synchronous readback on the interaction path" is a fact about
//     the code rather than a claim about it.
//   * THE TESSELLATION GATE. One case runs with `m_TessellationEnabled` FALSE.
//     The LOD descent is gated on that flag and no shipped scene set it for
//     months, which is how the GPU quadtree ended up with zero runtime coverage
//     (docs/agent-rules/terrain-gpu-lod-quadtree.md §1). Picking must not
//     inherit that gate, and this is the test that says so.
//
// SKIPs cleanly without a GL 4.6 context, like every other GPU test here.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Terrain/TerrainChunkManager.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainGPUPicker.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainMaterial.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>
#include <limits>
#include <utility>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kWidth = 640;
        constexpr u32 kHeight = 360;

        constexpr f32 kWorldSize = 1024.0f;
        constexpr f32 kHeightScale = 120.0f;
        constexpr u32 kHeightmapResolution = 256;

        // One heightmap texel in world units — the tolerance the issue names.
        constexpr f32 kTexelWorld = kWorldSize / static_cast<f32>(kHeightmapResolution - 1);

        // `EditorLayer::TerrainRaycastCPU`, transcribed: a 1-unit march looking
        // for the first above->below crossing, then 8 bisection steps. Kept
        // line-for-line with the shipped path rather than "improved", because
        // what is being measured is agreement WITH IT, not with a better march.
        //
        // The terrain here sits at the origin with an identity transform, so the
        // world/terrain-local distinction the real path handles collapses.
        bool CpuTerrainRaycast(const TerrainData& data, const glm::vec3& origin, const glm::vec3& dir,
                               glm::vec3& outHit)
        {
            constexpr f32 stepSize = 1.0f;
            constexpr f32 maxDist = 2000.0f;
            constexpr i32 refinementSteps = 8;

            auto heightAt = [&data](const glm::vec3& p)
            {
                const f32 nx = std::clamp(p.x / kWorldSize, 0.0f, 1.0f);
                const f32 nz = std::clamp(p.z / kWorldSize, 0.0f, 1.0f);
                return data.GetHeightAt(nx, nz) * kHeightScale;
            };

            bool wasAbove = true;
            for (f32 t = 0.0f; t < maxDist; t += stepSize)
            {
                const glm::vec3 p = origin + dir * t;
                const f32 normX = p.x / kWorldSize;
                const f32 normZ = p.z / kWorldSize;
                if (normX < 0.0f || normX > 1.0f || normZ < 0.0f || normZ > 1.0f)
                {
                    continue;
                }

                const bool isAbove = p.y > heightAt(p);
                if (!isAbove && wasAbove)
                {
                    f32 lo = t - stepSize;
                    f32 hi = t;
                    for (i32 i = 0; i < refinementSteps; ++i)
                    {
                        const f32 mid = (lo + hi) * 0.5f;
                        if (const glm::vec3 mp = origin + dir * mid; mp.y > heightAt(mp))
                        {
                            lo = mid;
                        }
                        else
                        {
                            hi = mid;
                        }
                    }
                    outHit = origin + dir * ((lo + hi) * 0.5f);
                    return true;
                }
                wasAbove = isAbove;
            }
            return false;
        }
    } // namespace

    class TerrainGPUPickEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.3f));
                dl.m_Intensity = 3.0f;
            }

            m_TerrainEntity = scene.CreateEntity("Terrain");
            auto& terrain = m_TerrainEntity.AddComponent<TerrainComponent>();
            terrain.m_ProceduralEnabled = true;
            terrain.m_ProceduralSeed = 717717;
            terrain.m_ProceduralResolution = kHeightmapResolution;
            terrain.m_ProceduralOctaves = 5;
            terrain.m_ProceduralFrequency = 3.0f;
            terrain.m_WorldSizeX = kWorldSize;
            terrain.m_WorldSizeZ = kWorldSize;
            terrain.m_HeightScale = kHeightScale;
            // DEFAULT (false) on purpose — see the header comment. Individual
            // cases flip it where they mean to.
            terrain.m_TessellationEnabled = false;

            terrain.m_AutoMaterial = true;
            terrain.m_SplatmapGenResolution = 256;
            terrain.m_Material = Ref<TerrainMaterial>::Create();
            for (const auto& layer : TerrainGenerator::MakeDefaultLayers())
            {
                terrain.m_Material->AddLayer(layer);
            }
            terrain.m_LayerRules = TerrainGenerator::MakeDefaultRules();
            terrain.m_MaterialNeedsRebuild = true;
            terrain.m_AutoSplatNeedsRebuild = true;
        }

        // A test presents nothing, so nothing flushes on its own and the ring's
        // fences would never submit. A ClientWaitFence HERE is fine — the rule
        // the feature has to keep is that the ENGINE never waits, and the test
        // waiting is what lets it observe the engine's poll succeeding.
        static void FlushAndWaitForGPU()
        {
            const u64 fence = RenderCommand::CreateFence();
            if (fence == 0)
            {
                return;
            }
            [[maybe_unused]] const auto status = RenderCommand::ClientWaitFence(fence, 2'000'000'000ull);
            RenderCommand::DestroyFence(fence);
        }

        EditorCamera MakeCamera() const
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 6000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(glm::vec3(512.0f, 500.0f, 512.0f), 0.0f, 1.50f);
            return camera;
        }

        // Bring the terrain, its chunks and both quadtrees up, then hand back the
        // picker. Null when the terrain never built — the caller asserts.
        Ref<TerrainGPUPicker> WarmUpAndGetPicker(const EditorCamera& camera)
        {
            RunEditorFrames(camera, 5);
            auto& terrain = m_TerrainEntity.GetComponent<TerrainComponent>();
            if (!terrain.m_ChunkManager || !terrain.m_ChunkManager->IsBuilt())
            {
                return nullptr;
            }
            return terrain.m_ChunkManager->GetOrCreateGPUPicker();
        }

        // Submit `request` every frame until the ring returns THAT ray's answer.
        //
        // Waiting on the RAY ID, not merely on `Valid`: an answer from a previous
        // query is already valid, and asserting against it would measure the
        // wrong ray while looking exactly like a pass. Returns false on timeout,
        // which the caller must treat as a failure rather than a miss.
        bool AwaitAnswer(Ref<TerrainGPUPicker>& picker, const EditorCamera& camera,
                         const TerrainGPUPicker::RayRequest& request, TerrainGPUPicker::Result& outResult)
        {
            constexpr u32 kMaxFrames = 20;
            for (u32 frame = 0; frame < kMaxFrames; ++frame)
            {
                EXPECT_TRUE(picker->SubmitRay(request));
                RunEditorFrames(camera, 1);
                FlushAndWaitForGPU();
                // Deliberately NOT calling Poll() here. Poll advances the frame
                // counter Latency is measured in, and an extra call would make
                // an answer look one frame older than it is — flattering exactly
                // the assertion this test uses to prove the read is async.
                if (const auto& latest = picker->GetLatest(); latest.Valid && latest.RayId == request.RayId)
                {
                    outResult = latest;
                    return true;
                }
            }
            return false;
        }

        Entity m_TerrainEntity;
    };

    // The core claim: a GPU pick lands on the terrain, and lands where the CPU
    // march says it should.
    TEST_F(TerrainGPUPickEvidenceTest, GpuPickMatchesTheCpuMarchWithinOneTexel)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const EditorCamera camera = MakeCamera();
        Ref<TerrainGPUPicker> picker = WarmUpAndGetPicker(camera);
        ASSERT_TRUE(picker) << "the terrain never built, so there was nothing to pick against";

        auto& terrain = m_TerrainEntity.GetComponent<TerrainComponent>();
        ASSERT_TRUE(terrain.m_TerrainData);
        ASSERT_EQ(terrain.m_TerrainData->GetResolution(), kHeightmapResolution);

        // Four rays fired at different parts of the terrain from different
        // heights and angles, all steep enough that the CPU reference's 1-unit
        // march cannot tunnel through a ridge — the comparison is against that
        // march, so a case where IT is wrong would measure nothing useful.
        const std::vector<std::pair<glm::vec3, glm::vec3>> rays = {
            { glm::vec3(512.0f, 600.0f, 512.0f), glm::normalize(glm::vec3(0.0f, -1.0f, 0.0f)) },
            { glm::vec3(200.0f, 500.0f, 300.0f), glm::normalize(glm::vec3(0.25f, -1.0f, 0.15f)) },
            { glm::vec3(800.0f, 450.0f, 700.0f), glm::normalize(glm::vec3(-0.2f, -1.0f, -0.3f)) },
            { glm::vec3(120.0f, 700.0f, 880.0f), glm::normalize(glm::vec3(0.35f, -1.0f, -0.35f)) },
        };

        u32 rayId = 0;
        u32 comparedRays = 0;
        for (const auto& [origin, direction] : rays)
        {
            glm::vec3 cpuHit{ 0.0f };
            ASSERT_TRUE(CpuTerrainRaycast(*terrain.m_TerrainData, origin, direction, cpuHit))
                << "the CPU reference found no hit for a ray aimed straight at the terrain";

            TerrainGPUPicker::RayRequest request;
            request.OriginLocal = origin;
            request.DirectionLocal = direction;
            request.MaxDistance = 2000.0f;
            request.RayId = ++rayId;

            TerrainGPUPicker::Result result;
            ASSERT_TRUE(AwaitAnswer(picker, camera, request, result))
                << "ray " << rayId << " never came back through the ring";
            ASSERT_TRUE(result.Hit) << "ray " << rayId << " reported a miss where the CPU march found ground";
            // Any of the three overflow causes (worklist, candidate list, or a
            // march that could not reach texel spacing) means the pass answered
            // under duress, so the accuracy assertions below would be measuring
            // something other than the path this test is about. Validated by
            // forcing the march flag on in TerrainPickResolve.comp: the value
            // arrives here as 4 and this line goes red.
            EXPECT_EQ(result.OverflowFlags, 0u)
                << "ray " << rayId << " came back with overflow flags " << result.OverflowFlags
                << " (1 = worklist, 2 = candidate list, 4 = march budget), so this answer is not the one under test";

            // ---- cross-check 1: the point is ON the surface ------------------
            // Independent of the CPU raycast entirely: only the heightmap is
            // consulted. Half a texel of vertical slack covers the difference
            // between the GPU's bilinear fetch and the CPU's, which are the same
            // expression on the same data but not the same rounding.
            const f32 surfaceY =
                terrain.m_TerrainData->GetHeightAt(result.PositionLocal.x / kWorldSize,
                                                   result.PositionLocal.z / kWorldSize) *
                kHeightScale;
            EXPECT_NEAR(result.PositionLocal.y, surfaceY, kTexelWorld * 0.5f)
                << "ray " << rayId << " returned a point that is not on the heightmap";

            // ---- cross-check 2: agreement with the shipped CPU path ----------
            EXPECT_LE(glm::length(result.PositionLocal - cpuHit), kTexelWorld)
                << "ray " << rayId << ": GPU " << result.PositionLocal.x << "," << result.PositionLocal.y << ","
                << result.PositionLocal.z << " vs CPU " << cpuHit.x << "," << cpuHit.y << "," << cpuHit.z
                << " — further apart than one texel (" << kTexelWorld << " units)";

            // ---- cross-check 3: it came back through the ring ----------------
            // A synchronous read would publish the answer on the frame that
            // produced it. This is the acceptance criterion "no synchronous
            // readback" expressed as something observable.
            EXPECT_GT(result.Latency, 0u) << "ray " << rayId << " was published with zero frames of ring latency, "
                                                                "which is what a synchronous readback looks like";
            ++comparedRays;
        }

        // Anti-vacuous: a loop that compared nothing would otherwise pass.
        ASSERT_EQ(comparedRays, rays.size());
    }

    // The gating-flag trap, made into an assertion. `m_TessellationEnabled` is
    // false in BuildScene, so this whole file already runs on the ungated path —
    // but a future change that quietly hangs picking off that flag would still
    // pass every other case here if they enabled it, so one case states the
    // requirement outright.
    TEST_F(TerrainGPUPickEvidenceTest, PickingWorksWithTessellationDisabled)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const EditorCamera camera = MakeCamera();
        Ref<TerrainGPUPicker> picker = WarmUpAndGetPicker(camera);
        ASSERT_TRUE(picker);

        auto& terrain = m_TerrainEntity.GetComponent<TerrainComponent>();
        ASSERT_FALSE(terrain.m_TessellationEnabled)
            << "this case is only meaningful while the LOD gate is OFF";
        ASSERT_TRUE(terrain.m_ChunkManager->GetGPUQuadtree());
        ASSERT_TRUE(terrain.m_ChunkManager->GetGPUQuadtree()->IsBuilt())
            << "the node pyramid must be built regardless of the tessellation flag — picking reads it";

        TerrainGPUPicker::RayRequest request;
        request.OriginLocal = glm::vec3(512.0f, 600.0f, 512.0f);
        request.DirectionLocal = glm::vec3(0.0f, -1.0f, 0.0f);
        request.RayId = 4242;

        TerrainGPUPicker::Result result;
        ASSERT_TRUE(AwaitAnswer(picker, camera, request, result));
        EXPECT_TRUE(result.Hit) << "picking is gated on the tessellation flag — it must not be";
    }

    // A ray aimed away from the terrain must come back a MISS, not a hit at some
    // clamped edge height. The heightmap sampler clamps its UV, so a march that
    // wandered outside the footprint would find a plausible surface there; the
    // node bounds are what stop it, and this is the case that proves they do.
    TEST_F(TerrainGPUPickEvidenceTest, RayAimedAwayFromTheTerrainReportsAMiss)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const EditorCamera camera = MakeCamera();
        Ref<TerrainGPUPicker> picker = WarmUpAndGetPicker(camera);
        ASSERT_TRUE(picker);

        TerrainGPUPicker::RayRequest request;
        request.OriginLocal = glm::vec3(512.0f, 600.0f, 512.0f);
        // Straight up, from above the terrain: nothing to hit in that direction.
        request.DirectionLocal = glm::vec3(0.0f, 1.0f, 0.0f);
        request.RayId = 9001;

        TerrainGPUPicker::Result result;
        ASSERT_TRUE(AwaitAnswer(picker, camera, request, result));
        EXPECT_FALSE(result.Hit) << "an upward ray hit something at distance " << result.Distance;

        // And the same origin looking down DOES hit — the anti-vacuous half. A
        // picker that answered "miss" unconditionally would pass the assertion
        // above and nothing else here would notice.
        request.DirectionLocal = glm::vec3(0.0f, -1.0f, 0.0f);
        request.RayId = 9002;
        ASSERT_TRUE(AwaitAnswer(picker, camera, request, result));
        EXPECT_TRUE(result.Hit) << "the downward control ray missed too, so the miss above proved nothing";
    }

    // A ray the picker refuses must not be queued at all: a NaN direction would
    // make every slab comparison false, so the node test degenerates to "hit"
    // and the descent splits the entire tree.
    TEST_F(TerrainGPUPickEvidenceTest, NonFiniteAndDegenerateRaysAreRefused)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const EditorCamera camera = MakeCamera();
        Ref<TerrainGPUPicker> picker = WarmUpAndGetPicker(camera);
        ASSERT_TRUE(picker);

        TerrainGPUPicker::RayRequest bad;
        bad.OriginLocal = glm::vec3(512.0f, 600.0f, 512.0f);
        bad.DirectionLocal = glm::vec3(std::numeric_limits<f32>::quiet_NaN(), -1.0f, 0.0f);
        EXPECT_FALSE(picker->SubmitRay(bad));
        EXPECT_FALSE(picker->HasPendingRay());

        bad.DirectionLocal = glm::vec3(0.0f);
        EXPECT_FALSE(picker->SubmitRay(bad)) << "a zero-length direction has no `t` to report";
        EXPECT_FALSE(picker->HasPendingRay());

        bad.DirectionLocal = glm::vec3(0.0f, -1.0f, 0.0f);
        bad.MaxDistance = -1.0f;
        EXPECT_FALSE(picker->SubmitRay(bad));
        EXPECT_FALSE(picker->HasPendingRay());

        // Anti-vacuous: a SubmitRay that returned false for everything would
        // pass all three above.
        bad.MaxDistance = 2000.0f;
        EXPECT_TRUE(picker->SubmitRay(bad));
        EXPECT_TRUE(picker->HasPendingRay());
    }
} // namespace OloEngine::Tests

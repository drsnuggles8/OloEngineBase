// OLO_TEST_LAYER: L4
//
// =============================================================================
// GPUReadbackStatsEvidenceTest.cpp — issue #721, both acceptance criteria, on a
// real GPU.
//
// The layout test next door proves the C++ and GLSL registries agree. It cannot
// prove the channel REPORTS THE RIGHT NUMBER, and "reports a number" is the
// failure mode a diagnostic channel is most likely to ship with: a counter that
// is right on average, or right for the wrong frame, or right except when the
// pass it instruments took a different branch, produces a plausible value and no
// symptom.
//
// So every assertion here is a CROSS-CHECK against something derived
// independently of the channel:
//
//   * `InstanceCullInput` is counted by the SHADER, one atomicAdd per surviving
//     invocation, and compared against the instance count the CPU uploaded. A
//     counter the CPU wrote would have agreed by construction and proved
//     nothing about whether the dispatch ran at all.
//   * `Drawn` is compared against `indirect.instanceCount`, which is the value
//     the actual draw would consume — a completely separate buffer, written by
//     a different atomic, read back through a different path.
//   * `Drawn + FrustumRejected == Input` is a conservation identity. Any single
//     counter can be wrong and plausible; three that must sum cannot all be
//     wrong the same way.
//   * The overflow case forces a REAL truncation — the shader genuinely refuses
//     the append and the draw count genuinely stops at the capacity — rather
//     than setting a flag by hand, which is what acceptance criterion #2
//     actually asks for.
//
// Classification L4: it drives a real compute dispatch and reads real GPU
// buffers, with no scene and no pixels.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/Debug/GPUReadbackStats.h"
#include "OloEngine/Renderer/Instancing/GPUFrustumCuller.h"
#include "OloEngine/Renderer/Instancing/InstanceData.h"
#include "OloEngine/Renderer/Instancing/InstancedMeshComponent.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kInstanceCount = 512;
        // Half the batch is placed behind the camera, so the frustum test has
        // something real to reject. Exact, not approximate: the split is what
        // makes the conservation identity below a meaningful check rather than
        // "some number plus some other number".
        constexpr u32 kVisibleInstances = kInstanceCount / 2;

        // Flush the GL command queue and wait for it to drain.
        //
        // GPUReadbackStats::BeginFrame only ever POLLS its fences
        // (`IsFenceSignaled` -> glGetSynciv, no flush bit), which is exactly what
        // makes it non-stalling in the engine — where SwapBuffers flushes every
        // frame anyway. A test presents nothing, so without this the ring's
        // fences may never be submitted and the poll would spin forever. The
        // fence is created AFTER the channel's own copy, so waiting on it implies
        // the copy completed too.
        void FlushAndWaitForGPU()
        {
            const u64 fence = RenderCommand::CreateFence();
            if (fence == 0)
                return;
            // 2 s: generous enough that a slow driver is not a flake, short
            // enough that a genuine hang fails the test instead of the suite.
            [[maybe_unused]] const auto status = RenderCommand::ClientWaitFence(fence, 2'000'000'000ull);
            RenderCommand::DestroyFence(fence);
        }

        // Retire every slot the ring is still holding.
        //
        // Not hygiene — a precondition. `EndFrame` SKIPS the capture when no slot
        // is free (that is what makes it non-blocking), so a ring left full by an
        // earlier test in this process would silently take no snapshot of the
        // frame under test, and the wait below would then time out on a frame
        // that was never captured. `BeginFrame` alone retires without consuming a
        // slot, so this drains without needing the GPU to do any new work.
        void DrainRing()
        {
            for (u32 i = 0; i < GPUReadbackStats::kRingSlots + 1; ++i)
            {
                FlushAndWaitForGPU();
                GPUReadbackStats::BeginFrame();
            }
        }

        // Drive the channel until frame `target` (or newer) comes back.
        //
        // WAITING FOR A SPECIFIC FRAME INDEX, not merely for `Valid`. `Valid`
        // latches true at the first retirement and never clears, so a
        // "pump until valid" loop returns the PREVIOUS test's counters
        // instantly — and those counters are plausible, differently wrong, and
        // would make this whole file assert against the wrong frame. That is the
        // exact failure mode the channel exists to expose, so it would be a poor
        // joke to build it into the test that proves the channel works.
        GPUReadbackStatsFrame PumpUntilRetired(u64 target, u32 maxFrames = 16)
        {
            for (u32 i = 0; i < maxFrames; ++i)
            {
                FlushAndWaitForGPU();
                GPUReadbackStats::BeginFrame();
                if (const auto& frame = GPUReadbackStats::GetLatest(); frame.Valid && frame.FrameIndex >= target)
                    return frame;
                GPUReadbackStats::EndFrame();
            }
            return {};
        }

        std::vector<InstanceData> MakeSplitBatch()
        {
            std::vector<InstanceData> instances(kInstanceCount);
            for (u32 i = 0; i < kInstanceCount; ++i)
            {
                // -Z is in front of a glm::lookAt camera looking down -Z; +Z is
                // behind it and outside the near plane.
                const f32 z = (i < kVisibleInstances) ? -20.0f : 60.0f;
                const auto x = static_cast<f32>(static_cast<i32>(i % 8u) - 4);
                instances[i].Transform = glm::translate(glm::mat4(1.0f), glm::vec3(x, 0.0f, z));
                instances[i].PrevTransform = instances[i].Transform;
            }
            return instances;
        }

        // The `instanceCount` field of the DrawElementsIndirectCommand — the
        // value the real draw consumes. Independent of the stats channel in
        // every respect: different buffer, different atomic, different readback.
        u32 ReadIndirectInstanceCount(const Ref<StorageBuffer>& indirect)
        {
            u32 words[2]{};
            indirect->GetData(words, static_cast<u32>(sizeof(words)), 0);
            return words[1];
        }

        class GPUReadbackStatsEvidence : public RendererAttachedTest
        {
          protected:
            // No entities: this fixture is used purely for its GL context and
            // initialised Renderer3D. The cull is driven directly.
            void BuildScene() override {}

            void TearDown() override
            {
                // Release the camera UBO while GL is alive. A function-local
                // `static Ref<UniformBuffer>` here — the obvious way to write it —
                // outlives Renderer::Shutdown() and shows up in the teardown
                // report as a surviving GPU allocation, which is exactly the
                // pattern docs/agent-rules/lazy-static-release-ownership.md is
                // about. Cheap to get right; noisy to leave wrong, because the
                // report is the thing that catches the real ones.
                m_CameraUBO.Reset();
                RendererAttachedTest::TearDown();
            }

            // The camera the cull tests against. GPUFrustumCuller reads
            // u_ViewProjection out of the shared camera UBO rather than taking it
            // as an argument, so the test has to supply one — a leftover matrix
            // from an earlier test would make the survivor split
            // nondeterministic.
            void UploadCamera(const glm::mat4& viewProjection)
            {
                if (!m_CameraUBO)
                {
                    m_CameraUBO = UniformBuffer::Create(UBOStructures::CameraUBO::GetSize(),
                                                        ShaderBindingLayout::UBO_CAMERA);
                }
                UBOStructures::CameraUBO camera{};
                camera.ViewProjection = viewProjection;
                camera.View = glm::mat4(1.0f);
                camera.Projection = viewProjection;
                camera.Position = glm::vec3(0.0f);
                camera.PrevViewProjection = viewProjection;
                m_CameraUBO->SetData(&camera, UBOStructures::CameraUBO::GetSize());
                m_CameraUBO->Bind();
            }

            // The view-projection every test in this file culls against: a 60°
            // perspective looking down -Z, far plane at 50, so the batch's
            // z = -20 half is inside and its z = +60 half is not.
            [[nodiscard]] static glm::mat4 TestViewProjection()
            {
                return glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 50.0f) *
                       glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            }

          private:
            Ref<UniformBuffer> m_CameraUBO;
        };
    } // namespace

    // Criterion 1: a GPU-driven pass publishes counters with no synchronous
    // readback, and the counters agree with independently derived values.
    TEST_F(GPUReadbackStatsEvidence, InstanceCullCountersMatchIndependentlyDerivedValues)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ASSERT_TRUE(GPUReadbackStats::IsInitialised()) << "Renderer3D::Init did not bring the channel up";

        GPUReadbackStats::SetEnabled(true);
        DrainRing();
        GPUReadbackStats::BeginFrame();

        UploadCamera(TestViewProjection());

        auto culler = Ref<GPUFrustumCuller>::Create();
        culler->EnsureInitialised();
        culler->BeginFrame();

        const auto instances = MakeSplitBatch();
        const auto result = culler->Cull(instances, 36, 0, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), 1.0f);
        ASSERT_TRUE(result.IndirectBuffer);

        const u64 target = GPUReadbackStats::GetFrameIndex();
        GPUReadbackStats::EndFrame();
        const auto frame = PumpUntilRetired(target);
        ASSERT_TRUE(frame.Valid) << "the readback ring never retired a slot — the channel produced nothing at all";

        const u32 input = frame.Get(GPUStatCounter::InstanceCullInput);
        const u32 drawn = frame.Get(GPUStatCounter::InstanceCullDrawn);
        const u32 rejected = frame.Get(GPUStatCounter::InstanceCullFrustumRejected);

        // Cross-check 1 — the shader's own input count against what the CPU
        // uploaded. This is the check that would fail if the dispatch never ran,
        // if the workgroup count were wrong, or if the stats block were bound at
        // the wrong slot (all three of which produce zero, not a wrong number).
        EXPECT_EQ(input, kInstanceCount) << "the shader counted a different number of invocations than the CPU "
                                            "submitted instances";

        // Cross-check 2 — conservation. Every counted invocation must have
        // either drawn or been rejected; nothing may vanish.
        //
        // This two-term form holds because `culler->BeginFrame()` cleared the
        // occlusion inputs, so `Cull()` dispatched the FRUSTUM-ONLY shader and
        // there is no third exit. With a Hi-Z pyramid supplied the identity
        // gains an `+ OcclusionRejected` term; it is deliberately not exercised
        // here, because a test that needs a populated depth pyramid to state its
        // invariant is a test about the pyramid.
        EXPECT_EQ(drawn + rejected, input) << "drawn(" << drawn << ") + rejected(" << rejected << ") != input("
                                           << input << ") — a counter is being missed on some branch";

        // Cross-check 3 — the channel's `Drawn` against the value the ACTUAL
        // DRAW would use. These two numbers come from different buffers written
        // by different atomics; agreeing by accident is not a realistic failure.
        EXPECT_EQ(drawn, ReadIndirectInstanceCount(result.IndirectBuffer))
            << "the stats channel and the indirect draw command disagree about how many instances survived";

        // And the split is the one the batch was built for. Not a tautology: it
        // is what tells the three checks above apart from "everything survived",
        // under which conservation holds trivially and the frustum test could be
        // entirely broken.
        EXPECT_EQ(drawn, kVisibleInstances);
        EXPECT_EQ(rejected, kInstanceCount - kVisibleInstances);

        // No overflow was forced, so nothing may claim one.
        EXPECT_FALSE(frame.AnyOverflow()) << "an overflow flag fired on a batch that fits comfortably";

        // The counters belong to a frame that has already finished, which is the
        // whole no-stall claim: a synchronous readback would have reported the
        // CURRENT frame index.
        EXPECT_GT(frame.Latency, 0u) << "the readback reported zero frames of latency, which means it was read "
                                        "synchronously rather than through the ring";
    }

    // Criterion 2: an intentionally overflowed buffer surfaces as a VISIBLE
    // OVERFLOW FLAG rather than silent truncation.
    //
    // The truncation is real. `SetDebugOutputCapacity` shrinks the bound the
    // shader checks against, so survivors past it are genuinely refused, the
    // indirect draw count is genuinely rolled back, and the frame would genuinely
    // render fewer instances. Nothing here fakes the flag.
    TEST_F(GPUReadbackStatsEvidence, ForcedOverflowRaisesAFlagAndCountsTheDrops)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ASSERT_TRUE(GPUReadbackStats::IsInitialised());

        constexpr u32 kSqueezedCapacity = 32;
        static_assert(kSqueezedCapacity < kVisibleInstances, "the cap has to be below the survivor count to truncate");

        GPUReadbackStats::SetEnabled(true);
        DrainRing();
        GPUReadbackStats::BeginFrame();

        UploadCamera(TestViewProjection());

        auto culler = Ref<GPUFrustumCuller>::Create();
        culler->EnsureInitialised();
        culler->BeginFrame();
        culler->SetDebugOutputCapacity(kSqueezedCapacity);

        const auto instances = MakeSplitBatch();
        const auto result = culler->Cull(instances, 36, 0, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), 1.0f);
        ASSERT_TRUE(result.IndirectBuffer);

        const u64 target = GPUReadbackStats::GetFrameIndex();
        GPUReadbackStats::EndFrame();
        const auto frame = PumpUntilRetired(target);
        ASSERT_TRUE(frame.Valid);

        // THE ACCEPTANCE CRITERION.
        EXPECT_TRUE(frame.Overflowed(GPUStatFlag::InstanceCullOutput))
            << "the cull output truncated and NOTHING said so — this is exactly the silent-truncation failure "
               "issue #721 exists to remove";

        const u32 drawn = frame.Get(GPUStatCounter::InstanceCullDrawn);
        const u32 dropped = frame.Get(GPUStatCounter::InstanceCullDropped);

        EXPECT_EQ(drawn, kSqueezedCapacity) << "more instances were written than the capacity allowed";
        EXPECT_EQ(dropped, kVisibleInstances - kSqueezedCapacity)
            << "the dropped counter does not account for every refused survivor";
        // Conservation again, now across the truncation: nothing may be lost
        // between "survived the frustum" and "drawn or explicitly dropped".
        EXPECT_EQ(drawn + dropped, kVisibleInstances);

        // The rollback, cross-checked against the draw command. Without it the
        // indirect draw would still ask for the refused instances and read
        // whatever slot they claimed — a silent drop traded for visible garbage.
        EXPECT_EQ(ReadIndirectInstanceCount(result.IndirectBuffer), kSqueezedCapacity)
            << "the indirect draw count exceeds what was actually written, so the draw will read slots the "
               "cull never filled";

        // Only the flag that actually fired.
        EXPECT_FALSE(frame.Overflowed(GPUStatFlag::VSMRequestRing));
        EXPECT_FALSE(frame.Overflowed(GPUStatFlag::VSMPhysicalPool));
    }

    // Disabling the channel must genuinely stop it, not merely hide it: the
    // helpers early-out on `b_StatsEnabled`, so a disabled frame publishes
    // nothing. Worth pinning because the gate is a runtime value rather than a
    // preprocessor fork, which is the thing that keeps the instrumented and
    // shipped shader binaries identical.
    TEST_F(GPUReadbackStatsEvidence, DisabledChannelPublishesNothing)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ASSERT_TRUE(GPUReadbackStats::IsInitialised());

        // Drain anything the previous tests left in flight so the assertion
        // below cannot be satisfied by a stale retired frame.
        GPUReadbackStats::SetEnabled(false);
        for (u32 i = 0; i < GPUReadbackStats::kRingSlots + 2; ++i)
        {
            FlushAndWaitForGPU();
            GPUReadbackStats::BeginFrame();
            GPUReadbackStats::EndFrame();
        }

        UploadCamera(TestViewProjection());

        GPUReadbackStats::BeginFrame();
        const u64 disabledFrame = GPUReadbackStats::GetFrameIndex();
        auto culler = Ref<GPUFrustumCuller>::Create();
        culler->EnsureInitialised();
        culler->BeginFrame();
        const auto instances = MakeSplitBatch();
        const auto result = culler->Cull(instances, 36, 0, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), 1.0f);
        GPUReadbackStats::EndFrame();

        // Give the ring every chance to retire that frame if it captured one.
        for (u32 i = 0; i < GPUReadbackStats::kRingSlots + 2; ++i)
        {
            FlushAndWaitForGPU();
            GPUReadbackStats::BeginFrame();
        }

        // Nothing may have been captured for a disabled frame. Checked by frame
        // INDEX rather than by `Valid`, which latches true for the whole process
        // once the first frame retires and would make this assertion vacuous.
        const auto& latest = GPUReadbackStats::GetLatest();
        EXPECT_FALSE(latest.Valid && latest.FrameIndex >= disabledFrame)
            << "a frame captured while the channel was disabled came back anyway (frame " << latest.FrameIndex
            << " >= " << disabledFrame << ")";

        // The cull itself is unaffected — the gate must not change what renders.
        EXPECT_EQ(ReadIndirectInstanceCount(result.IndirectBuffer), kVisibleInstances)
            << "turning the diagnostic off changed the cull's output, which makes the instrumented build a "
               "different program from the shipped one";

        GPUReadbackStats::SetEnabled(true);
    }

    // -------------------------------------------------------------------------
    // The end-to-end guard, and the reason it exists.
    //
    // Every test above drives `BeginFrame -> cull -> EndFrame` BY HAND, which
    // means none of them can tell you whether the engine calls them in that
    // order. It does not, if the hooks are placed the obvious way: the sibling
    // ShaderDebugDraw resets itself in `RenderPipeline::UploadExecutionState`,
    // so this channel was written there too — and `UploadExecutionState` runs
    // inside `EndScene`, whereas the GPU instance cull dispatches at SUBMISSION
    // time, before it. The per-frame clear landed AFTER the cull had already
    // published, so adopter #1 reported a permanent zero in the real engine
    // while every hand-ordered test above stayed green.
    //
    // This test drives a REAL frame — `Scene::OnUpdateRuntime` through
    // `RunFrames`, the same path the editor takes — with an instance field large
    // enough to route through the GPU cull. It fails if the bracket ever stops
    // enclosing submission.
    // -------------------------------------------------------------------------
    namespace
    {
        // Renderer3DData::GPUCullThreshold is 1024; comfortably past it so the
        // submission routes to SubmitGPUCulledInstanced rather than the CPU loop.
        constexpr i32 kCullGridDim = 34; // 34^2 = 1156
        constexpr u32 kCullGridInstances = static_cast<u32>(kCullGridDim) * kCullGridDim;

        class GPUReadbackStatsFullFrame : public RendererAttachedTest
        {
          protected:
            void BuildScene() override
            {
                EnableRendering(256, 256);

                Entity camera = GetScene().CreateEntity("Camera");
                camera.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 12.0f };
                auto& cam = camera.AddComponent<CameraComponent>();
                cam.Primary = true;
                cam.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);

                Ref<Mesh> cube = MeshPrimitives::CreateCube();
                Entity field = GetScene().CreateEntity("InstanceField");
                auto& imc = field.AddComponent<InstancedMeshComponent>();
                imc.MeshSource = cube->GetMeshSource();
                imc.CastShadows = false;
                imc.Instances.reserve(kCullGridInstances);
                for (i32 gy = 0; gy < kCullGridDim; ++gy)
                {
                    for (i32 gx = 0; gx < kCullGridDim; ++gx)
                    {
                        const f32 u = static_cast<f32>(gx) / static_cast<f32>(kCullGridDim - 1) - 0.5f;
                        const f32 v = static_cast<f32>(gy) / static_cast<f32>(kCullGridDim - 1) - 0.5f;
                        InstanceData inst;
                        inst.Transform =
                            glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(u * 12.0f, v * 12.0f, 0.0f)),
                                       glm::vec3(0.15f));
                        inst.PrevTransform = inst.Transform;
                        imc.Instances.push_back(inst);
                    }
                }
            }
        };
    } // namespace

    TEST_F(GPUReadbackStatsFullFrame, RealFrameFeedsTheInstanceCullCounters)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ASSERT_TRUE(GPUReadbackStats::IsInitialised());

        // Enough frames for the ring to retire one that saw the cull: the
        // channel is read a few frames late by design.
        RunFrames(8);

        const auto& frame = GPUReadbackStats::GetLatest();
        ASSERT_TRUE(frame.Valid) << "no frame came back from the ring during a real render";

        // THE ASSERTION THAT CATCHES A MISPLACED HOOK. Zero here does not mean
        // "the cull did nothing" — a 1156-instance field in front of the camera
        // is well past Renderer3DData::GPUCullThreshold (1024), so the GPU cull
        // certainly ran. It means the counters were cleared after it ran.
        EXPECT_GT(frame.Get(GPUStatCounter::InstanceCullInput), 0u)
            << "the GPU instance cull published nothing during a real frame. The channel's BeginFrame/EndFrame "
               "bracket must enclose SUBMISSION (BeginScene..EndScene), not just the render graph — the cull "
               "dispatches before the graph runs.";

        // And the counters describe THIS batch rather than some leftover: the
        // shader counts one invocation per submitted instance.
        EXPECT_GE(frame.Get(GPUStatCounter::InstanceCullInput), kCullGridInstances)
            << "fewer invocations were counted than the field has instances";

        // A real frame with a comfortably-sized buffer must not report a
        // truncation. This is the other half of the forced-overflow test above:
        // that one proves the flag CAN fire, this one proves it does not fire
        // spuriously on the ordinary path — a flag that is always on is exactly
        // as useless as one that never is.
        EXPECT_FALSE(frame.Overflowed(GPUStatFlag::InstanceCullOutput))
            << "the cull reported a truncation on a batch that fits";
    }
} // namespace OloEngine::Tests

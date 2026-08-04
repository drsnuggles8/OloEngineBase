// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// ShaderDebugDrawGpuPushTest.cpp — issue #725
//
// Drives the REAL GLSL append helper on the GPU. `ShaderDebugDrawContractTest`
// pins the two-atomic append protocol by transcribing it into C++, which proves
// the algorithm but not that the shader implements it — the transcription and
// the GLSL could drift, and a drift in the atomic ORDER is exactly the kind of
// bug that produces plausible-looking output (a slightly-too-large instance
// count, drawing one uninitialised entry) rather than a failure.
//
// So this test dispatches `assets/shaders/tests/DebugDrawPushProbe.comp`, which
// includes the production `include/DebugDrawCommon.glsl` and calls
// `OloDebugDrawAABB` from real hardware, then reads the channel header back
// through the engine's own sanctioned readback path and checks:
//
//   * an under-capacity push draws exactly what it asked for;
//   * an over-capacity push reports the FULL requested count while drawing only
//     `capacity` — the overflow flag, on hardware;
//   * a disabled channel (capacity 0) accepts nothing AND leaves the counters
//     at zero, so the disabled path cannot manufacture a phantom overflow.
//
// The readback goes through ShaderDebugDraw::StageStatsForReadback() +
// BeginFrame() rather than a direct GetData on the channel, deliberately: the
// channel is GL_DYNAMIC_COPY and is read every frame as a GL_DRAW_INDIRECT_BUFFER,
// so a CPU read straight off it makes NVIDIA migrate it VIDEO->HOST and
// permanently slow the indirect draw (the VirtualMeshRegistry trap). A test that
// took the shortcut would be the template someone copies.
//
// FIXTURE CHOICE. This uses RendererAttachedTest rather than a bare
// OLO_ENSURE_GPU_OR_SKIP TEST, because it drives the real ShaderDebugDraw facade
// and therefore needs RenderCommand's RendererAPI instance — which the bare GPU
// fixture does NOT create (it only makes a GL context; the sibling GPU tests get
// away with it by using raw GL throughout). It does not render a scene.
//
// Classification: shaderpipe (single compute shader on the GPU).
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"
#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/Debug/ShaderDebugDraw.h"
#include "OloEngine/Renderer/Debug/ShaderDebugDrawTypes.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"

#include <gtest/gtest.h>

#include <utility>

namespace OloEngine::Tests
{
    namespace
    {
        // Dispatch the probe with `pushCount` requested appends, then drain the
        // channel header. Returns the AABB channel's stats.
        //
        // The staging copy is issued and then read on the NEXT BeginFrame, which
        // is the production ordering (one frame of latency, no stall), so the
        // sequence here is: reset -> dispatch -> stage -> drain.
        [[nodiscard]] ShaderDebugDrawChannelStats RunProbe(ComputeShader& probe, u32 capacity, u32 pushCount)
        {
            ShaderDebugDraw::SetChannelCapacity(capacity);
            ShaderDebugDraw::BeginFrame(); // sizes the channels + zeroes the counters

            probe.Bind();
            probe.SetUint("u_PushCount", pushCount);
            RenderCommand::DispatchCompute((pushCount + 63u) / 64u, 1, 1);
            // The CPU is about to read what the shader's atomics wrote.
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::BufferUpdate);

            ShaderDebugDraw::StageStatsForReadback();
            ShaderDebugDraw::BeginFrame(); // drains the staged headers into GetStats()

            const auto& stats = ShaderDebugDraw::GetStats();
            return stats.Channels[static_cast<u32>(std::to_underlying(ShaderDebugDrawPrimitive::AABB))];
        }

        // Leaves the feature exactly as it was found — this fixture shares a
        // process with every other GPU test.
        struct ScopedShaderDebugDraw
        {
            bool WasEnabled;
            u32 WasCapacity;

            ScopedShaderDebugDraw() : WasEnabled(ShaderDebugDraw::IsEnabled()),
                                      WasCapacity(ShaderDebugDraw::GetChannelCapacity())
            {
                ShaderDebugDraw::Init(); // idempotent
                ShaderDebugDraw::SetEnabled(true);
            }
            ~ScopedShaderDebugDraw()
            {
                ShaderDebugDraw::SetChannelCapacity(WasCapacity);
                ShaderDebugDraw::SetEnabled(WasEnabled);
                ShaderDebugDraw::BeginFrame();
            }
        };
    } // namespace

    // No scene, no rendering — the fixture is here purely for the process-wide
    // Renderer::Init that gives RenderCommand a live RendererAPI.
    class ShaderDebugDrawGpuPushTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override {}
    };

    TEST_F(ShaderDebugDrawGpuPushTest, AnUnderCapacityPushDrawsEverythingItAskedFor)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        auto probe = ComputeShader::Create("assets/shaders/tests/DebugDrawPushProbe.comp");
        ASSERT_TRUE(probe && probe->IsValid())
            << "DebugDrawPushProbe.comp failed to compile/link — include/DebugDrawCommon.glsl does not "
               "compile as part of a real compute shader.";

        const ScopedShaderDebugDraw scoped;

        const auto stats = RunProbe(*probe, /*capacity*/ 256, /*pushCount*/ 100);
        EXPECT_EQ(stats.Capacity, 256u);
        EXPECT_EQ(stats.Requested, 100u) << "The unclamped request counter did not see every push";
        EXPECT_EQ(stats.Drawn, 100u) << "The indirect draw's instance count does not match the accepted pushes";
        EXPECT_FALSE(stats.Overflowed());
        EXPECT_EQ(stats.Dropped(), 0u);
    }

    TEST_F(ShaderDebugDrawGpuPushTest, AnOverCapacityPushReportsTheOverflowInsteadOfSilentlyDropping)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        auto probe = ComputeShader::Create("assets/shaders/tests/DebugDrawPushProbe.comp");
        ASSERT_TRUE(probe && probe->IsValid());

        const ScopedShaderDebugDraw scoped;

        constexpr u32 capacity = 64;
        constexpr u32 pushes = 500;
        const auto stats = RunProbe(*probe, capacity, pushes);

        // This is the acceptance criterion "overflowing a channel raises a
        // visible flag rather than silently dropping draws", checked against the
        // driver rather than a simulation of it.
        EXPECT_EQ(stats.Capacity, capacity);
        EXPECT_EQ(stats.Requested, pushes) << "RequestCount must stay UNCLAMPED — clamping it is what makes an "
                                              "overflow indistinguishable from a full channel";
        EXPECT_EQ(stats.Drawn, capacity) << "The indirect draw must cover exactly the entries that were written";
        EXPECT_TRUE(stats.Overflowed());
        EXPECT_EQ(stats.Dropped(), pushes - capacity);
    }

    TEST_F(ShaderDebugDrawGpuPushTest, ADisabledChannelAcceptsNothingAndReportsNoOverflow)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        auto probe = ComputeShader::Create("assets/shaders/tests/DebugDrawPushProbe.comp");
        ASSERT_TRUE(probe && probe->IsValid());

        const ScopedShaderDebugDraw scoped;

        // Capacity 0 is how the engine spells "feature off". The helper's opening
        // `Capacity == 0u` guard must stop the push BEFORE the atomic — if it did
        // not, a disabled frame would report a huge phantom overflow every time,
        // and the flag would be worthless exactly when it is meant to be quiet.
        ShaderDebugDraw::SetChannelCapacity(0);
        ShaderDebugDraw::BeginFrame();

        probe->Bind();
        probe->SetUint("u_PushCount", 500u);
        RenderCommand::DispatchCompute(8, 1, 1);
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::BufferUpdate);

        ShaderDebugDraw::StageStatsForReadback();
        ShaderDebugDraw::BeginFrame();

        const auto& stats = ShaderDebugDraw::GetStats()
                                .Channels[static_cast<u32>(std::to_underlying(ShaderDebugDrawPrimitive::AABB))];
        EXPECT_EQ(stats.Capacity, 0u);
        EXPECT_EQ(stats.Drawn, 0u);
        EXPECT_EQ(stats.Requested, 0u)
            << "The disabled guard let the push reach the atomic — the disabled path is neither free nor quiet";
        EXPECT_FALSE(stats.Overflowed());
    }
} // namespace OloEngine::Tests

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// SoundGraphTypedConnectionTest — Phase 2 typed connections
// (docs/design/soundgraph-metasounds.md)
//
// Phase 2 replaced the choc::value::ValueView wiring (which never actually
// delivered node-to-node values at runtime — nodes read ParameterWrapper
// copies, not the re-aliased views) with typed refs: AudioBufferRef carries a
// whole block of f32 samples from a producer's AudioBuffer, FloatRef/IntRef/
// Int64Ref/BoolRef carry control-rate scalars, and SoundGraph::Process calls
// each node exactly once per block in producer-before-consumer order.
//
// This file pins:
//   * Node→node audio connections actually deliver per-sample data end to end
//     (oscillator → multiply → graph output).
//   * Asset-authored default plugs reach the node refs (amplitude property).
//   * SoundGraph::Process invokes each node once per block, not per sample.
//   * Type-mismatched connections are rejected at wire time instead of being
//     silently mis-aliased.
//   * Control-rate scalar connections flow between nodes.
//   * Graph float parameters ramp per-sample through their ramp buffer when
//     set with interpolation, and broadcast scalar updates otherwise.
//   * The whole-graph Debug-build cost is block-rate: one second of audio
//     processes far faster than real time (the Phase 1 bug was 5.27x slower).
// =============================================================================

#include "OloEngine/Asset/SoundGraphAsset.h"
#include "OloEngine/Audio/SoundGraph/GraphGeneration.h"
#include "OloEngine/Audio/SoundGraph/SoundGraph.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphFactory.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphPrototype.h"
#include "OloEngine/Audio/SoundGraph/Nodes/GeneratorNodes.h"
#include "OloEngine/Audio/SoundGraph/Nodes/MathNodes.h"
#include "OloEngine/Core/UUID.h"

#include <choc/containers/choc_Value.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace)
namespace sg = OloEngine::Audio::SoundGraph;

namespace
{
    void AddConn(SoundGraphAsset& asset, UUID srcID, const std::string& srcEndpoint,
                 UUID dstID, const std::string& dstEndpoint, bool isEvent)
    {
        SoundGraphConnection c;
        c.m_SourceNodeID = srcID;
        c.m_SourceEndpoint = srcEndpoint;
        c.m_TargetNodeID = dstID;
        c.m_TargetEndpoint = dstEndpoint;
        c.m_IsEvent = isEvent;
        asset.AddConnection(c);
    }

    /// Test-only node: counts Process calls and records the numFrames it was
    /// handed. Pins the once-per-block contract.
    struct CountingNode : public sg::NodeProcessor
    {
        explicit CountingNode(UUID id) : NodeProcessor("CountingNode", id) {}

        u32 m_ProcessCalls = 0;
        u32 m_LastNumFrames = 0;

        void Process(u32 numFrames) final
        {
            ++m_ProcessCalls;
            m_LastNumFrames = numFrames;
        }
    };

    /// Test-only node: captures the per-frame values seen on an audio-rate
    /// input ref. Pins graph-parameter ramp behavior.
    struct CaptureNode : public sg::NodeProcessor
    {
        explicit CaptureNode(UUID id) : NodeProcessor("CaptureNode", id)
        {
            AddAudioInRef(Identifier("In"), m_In);
        }

        sg::AudioBufferRef m_In;
        std::vector<f32> m_Captured;

        void Process(u32 numFrames) final
        {
            m_Captured.clear();
            for (u32 frame = 0; frame < numFrames; ++frame)
                m_Captured.push_back(m_In.Sample(frame));
        }
    };
} // namespace

TEST(SoundGraphTypedConnections, NodeToNodeAudioConnectionDeliversSamples)
{
    // SineOscillator -> Multiply<float> -> graph OutLeft/OutRight. Under the old
    // ValueView wiring the multiply node read its ParameterWrapper copy (always
    // zero) instead of the oscillator output — node-to-node value connections
    // were dead. This pins the typed AudioBufferRef path end to end.
    SoundGraphAsset asset;
    asset.SetName("SineTimesHalf");

    SoundGraphNodeData sine;
    sine.m_ID = UUID();
    sine.m_Type = "SineOscillator";
    sine.m_Name = "Sine";
    sine.m_Properties["Frequency"] = "440";
    sine.m_Properties["Amplitude"] = "1.0";
    asset.AddNode(sine);

    SoundGraphNodeData multiply;
    multiply.m_ID = UUID();
    multiply.m_Type = "Multiply<float>";
    multiply.m_Name = "Volume";
    multiply.m_Properties["Multiplier"] = "0.5";
    asset.AddNode(multiply);

    AddConn(asset, sine.m_ID, "OutValue", multiply.m_ID, "Value", /*isEvent=*/false);
    AddConn(asset, multiply.m_ID, "Out", UUID(0), "OutLeft", /*isEvent=*/false);
    AddConn(asset, multiply.m_ID, "Out", UUID(0), "OutRight", /*isEvent=*/false);

    Ref<sg::Prototype> prototype = sg::CompileAssetToPrototype(asset);
    ASSERT_NE(prototype, nullptr);
    Ref<sg::SoundGraph> instance = sg::CreateInstance(prototype);
    ASSERT_NE(instance, nullptr);

    constexpr u32 kBlock = 480;
    instance->Process(kBlock);

    auto* sineNode = dynamic_cast<sg::SineOscillator*>(instance->FindNodeByID(sine.m_ID));
    ASSERT_NE(sineNode, nullptr);
    ASSERT_GE(instance->m_OutputBuffers.size(), 2u);

    const f32* sineOut = sineNode->m_OutValue.Data();
    f32 maxAbs = 0.0f;
    for (u32 frame = 0; frame < kBlock; ++frame)
    {
        EXPECT_NEAR(instance->m_OutputBuffers[0][frame], sineOut[frame] * 0.5f, 1e-6f)
            << "frame " << frame << ": graph output must be the multiply of the sine block";
        EXPECT_NEAR(instance->m_OutputBuffers[1][frame], sineOut[frame] * 0.5f, 1e-6f);
        maxAbs = std::max(maxAbs, std::abs(instance->m_OutputBuffers[0][frame]));
    }

    // 4.4 cycles of 440 Hz at 48 kHz in 480 frames — the peak must approach
    // amplitude * multiplier. Silence here means the connection didn't flow.
    EXPECT_GT(maxAbs, 0.45f) << "node-to-node audio connection delivered no signal";
    EXPECT_LE(maxAbs, 0.5f + 1e-5f);
}

TEST(SoundGraphTypedConnections, AssetDefaultPlugsReachNodeInputs)
{
    // Amplitude authored on the asset must arrive at the oscillator's input ref
    // (the old pipeline needed a parameter-storage overlay hack for this).
    SoundGraphAsset asset;
    asset.SetName("QuietSine");

    SoundGraphNodeData sine;
    sine.m_ID = UUID();
    sine.m_Type = "SineOscillator";
    sine.m_Name = "Sine";
    sine.m_Properties["Frequency"] = "440";
    sine.m_Properties["Amplitude"] = "0.25";
    asset.AddNode(sine);

    AddConn(asset, sine.m_ID, "OutValue", UUID(0), "OutLeft", /*isEvent=*/false);
    AddConn(asset, sine.m_ID, "OutValue", UUID(0), "OutRight", /*isEvent=*/false);

    Ref<sg::Prototype> prototype = sg::CompileAssetToPrototype(asset);
    ASSERT_NE(prototype, nullptr);
    Ref<sg::SoundGraph> instance = sg::CreateInstance(prototype);
    ASSERT_NE(instance, nullptr);

    constexpr u32 kBlock = 480;
    instance->Process(kBlock);

    f32 maxAbs = 0.0f;
    for (u32 frame = 0; frame < kBlock; ++frame)
        maxAbs = std::max(maxAbs, std::abs(instance->m_OutputBuffers[0][frame]));

    EXPECT_NEAR(maxAbs, 0.25f, 0.002f) << "asset-authored Amplitude must reach the oscillator";
}

TEST(SoundGraphTypedConnections, EachNodeIsProcessedOncePerBlock)
{
    auto graph = Ref<sg::SoundGraph>::Create("OncePerBlock", UUID());

    auto counting = CreateScope<CountingNode>(UUID());
    auto* countingPtr = counting.get();
    graph->AddNode(std::move(counting));
    graph->Init();

    graph->Process(480);
    EXPECT_EQ(countingPtr->m_ProcessCalls, 1u) << "Process must be called once per block, not per sample";
    EXPECT_EQ(countingPtr->m_LastNumFrames, 480u);

    graph->Process(127);
    EXPECT_EQ(countingPtr->m_ProcessCalls, 2u);
    EXPECT_EQ(countingPtr->m_LastNumFrames, 127u);
}

TEST(SoundGraphTypedConnections, TypeMismatchedConnectionIsRejected)
{
    auto sine = sg::Factory::Create(Identifier("SineOscillator"), UUID());
    auto noise = sg::Factory::Create(Identifier("Noise"), UUID());
    ASSERT_NE(sine, nullptr);
    ASSERT_NE(noise, nullptr);

    // Audio f32 output -> Int32 input: must be rejected at wire time. The old
    // ValueView re-aliasing accepted any pairing and silently produced garbage.
    EXPECT_FALSE(sg::NodeProcessor::ConnectStreams(*sine, Identifier("OutValue"),
                                                   *noise, Identifier("Seed")));

    // Audio f32 output -> audio f32 input: accepted.
    EXPECT_TRUE(sg::NodeProcessor::ConnectStreams(*sine, Identifier("OutValue"),
                                                  *noise, Identifier("Amplitude")));

    // Unknown endpoints: rejected, not thrown.
    EXPECT_FALSE(sg::NodeProcessor::ConnectStreams(*sine, Identifier("NoSuchOut"),
                                                   *noise, Identifier("Amplitude")));
    EXPECT_FALSE(sg::NodeProcessor::ConnectStreams(*sine, Identifier("OutValue"),
                                                   *noise, Identifier("NoSuchIn")));
}

TEST(SoundGraphTypedConnections, ScalarConnectionFlowsBetweenNodes)
{
    auto first = sg::Factory::Create(Identifier("Multiply<int>"), UUID());
    auto second = sg::Factory::Create(Identifier("Multiply<int>"), UUID());
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    EXPECT_TRUE(first->SetInputDefault(Identifier("Value"), choc::value::createInt32(6)));
    EXPECT_TRUE(first->SetInputDefault(Identifier("Multiplier"), choc::value::createInt32(7)));
    EXPECT_TRUE(second->SetInputDefault(Identifier("Multiplier"), choc::value::createInt32(2)));

    // first.Out (i32) -> second.Value (i32)
    EXPECT_TRUE(sg::NodeProcessor::ConnectStreams(*first, Identifier("Out"),
                                                  *second, Identifier("Value")));

    first->Process(1);
    second->Process(1);

    auto* secondTyped = dynamic_cast<sg::Multiply<i32>*>(second.get());
    ASSERT_NE(secondTyped, nullptr);
    EXPECT_EQ(secondTyped->m_Out, 6 * 7 * 2) << "scalar value must flow through the typed connection";
}

TEST(SoundGraphTypedConnections, GraphFloatParameterRampsPerSample)
{
    auto graph = Ref<sg::SoundGraph>::Create("ParamRamp", UUID());

    const Identifier volumeID("Volume");
    graph->AddGraphInputStream(volumeID, choc::value::Value(0.0f));

    auto capture = CreateScope<CaptureNode>(UUID());
    auto* capturePtr = capture.get();
    const UUID captureID = capture->m_ID;
    graph->AddNode(std::move(capture));

    ASSERT_TRUE(graph->AddInputValueRoute(volumeID, captureID, Identifier("In")));
    graph->Init();

    constexpr u32 kBlock = 480;

    // Initial value: constant 0 across the block.
    graph->Process(kBlock);
    ASSERT_EQ(capturePtr->m_Captured.size(), kBlock);
    EXPECT_NEAR(capturePtr->m_Captured.front(), 0.0f, 1e-6f);
    EXPECT_NEAR(capturePtr->m_Captured.back(), 0.0f, 1e-6f);

    // Interpolated set: 10 ms ramp = 480 steps; the block must be per-sample
    // smooth (strictly increasing) and land on the target.
    ASSERT_TRUE(graph->SendInputValue(static_cast<u32>(volumeID), choc::value::createFloat32(1.0f), /*interpolate=*/true));
    graph->Process(kBlock);
    ASSERT_EQ(capturePtr->m_Captured.size(), kBlock);
    for (u32 frame = 1; frame < kBlock; ++frame)
    {
        EXPECT_GT(capturePtr->m_Captured[frame], capturePtr->m_Captured[frame - 1] - 1e-7f)
            << "ramp must be monotonically increasing at frame " << frame;
    }
    EXPECT_NEAR(capturePtr->m_Captured.back(), 1.0f, 1e-4f);

    // Block after the ramp: settled at the target everywhere.
    graph->Process(kBlock);
    EXPECT_NEAR(capturePtr->m_Captured.front(), 1.0f, 1e-5f);
    EXPECT_NEAR(capturePtr->m_Captured.back(), 1.0f, 1e-5f);

    // Non-interpolated set: next block reads the new value from frame 0.
    ASSERT_TRUE(graph->SendInputValue(static_cast<u32>(volumeID), choc::value::createFloat32(0.25f), /*interpolate=*/false));
    graph->Process(kBlock);
    EXPECT_NEAR(capturePtr->m_Captured.front(), 0.25f, 1e-6f);
    EXPECT_NEAR(capturePtr->m_Captured.back(), 0.25f, 1e-6f);
}

// =============================================================================
// Phase 3 — compiled execution plan (docs/design/soundgraph-metasounds.md)
//
// Phase 3 lowers the topological node order to a flat array of operator handles
// ({devirtualized thunk, node} pairs) the audio thread walks directly, and pools
// every node's audio-output buffer into one contiguous allocation. The behavioural
// contract is unchanged — the typed-connection tests above already run through the
// compiled plan + pool and pin output correctness. These pin the Phase 3 mechanics:
//   * Factory-created nodes carry a devirtualized process thunk (not the vtable
//     fallback) and that thunk actually invokes the node's Process.
//   * CreateInstance relocates node output buffers into the contiguous pool while
//     preserving the data they produce.
// =============================================================================

TEST(SoundGraphCompiledPlan, FactoryNodeThunkIsDevirtualizedAndInvokesProcess)
{
    auto sine = sg::Factory::Create(Identifier("SineOscillator"), UUID());
    ASSERT_NE(sine, nullptr);

    // Factory::MakeNode patches m_ProcessFn to the concrete ProcessThunk<T>; only
    // nodes built outside the factory keep the vtable fallback.
    EXPECT_NE(sine->m_ProcessFn, &sg::NodeProcessor::VtableProcessThunk)
        << "Factory-created node must carry a devirtualized process thunk";

    auto* sineTyped = dynamic_cast<sg::SineOscillator*>(sine.get());
    ASSERT_NE(sineTyped, nullptr);
    sineTyped->m_Frequency.SetDefault(440.0f);
    sineTyped->m_Amplitude.SetDefault(1.0f);
    sine->SetSampleRate(48000.0f);
    sine->Init();

    // Dispatch through the stored thunk exactly as the compiled plan does. It must
    // reach SineOscillator::Process and fill the output buffer with signal.
    constexpr u32 kBlock = 128;
    sine->m_ProcessFn(sine.get(), kBlock);

    const f32* out = sineTyped->m_OutValue.Data();
    f32 maxAbs = 0.0f;
    for (u32 frame = 0; frame < kBlock; ++frame)
        maxAbs = std::max(maxAbs, std::abs(out[frame]));
    EXPECT_GT(maxAbs, 0.1f) << "compiled-plan thunk must invoke the node's Process";
}

TEST(SoundGraphCompiledPlan, NodeOutputBuffersArePooledContiguously)
{
    // sine -> multiply -> graph out, built via the asset path so CreateInstance runs
    // AllocateNodeOutputPool. Both nodes' audio outputs must land inside the graph's
    // single contiguous pool, on distinct kMaxAudioBlockFrames-spaced slots.
    SoundGraphAsset asset;
    asset.SetName("PooledOutputs");

    SoundGraphNodeData sine;
    sine.m_ID = UUID();
    sine.m_Type = "SineOscillator";
    sine.m_Name = "Sine";
    sine.m_Properties["Frequency"] = "440";
    sine.m_Properties["Amplitude"] = "1.0";
    asset.AddNode(sine);

    SoundGraphNodeData multiply;
    multiply.m_ID = UUID();
    multiply.m_Type = "Multiply<float>";
    multiply.m_Name = "Volume";
    multiply.m_Properties["Multiplier"] = "0.5";
    asset.AddNode(multiply);

    AddConn(asset, sine.m_ID, "OutValue", multiply.m_ID, "Value", /*isEvent=*/false);
    AddConn(asset, multiply.m_ID, "Out", UUID(0), "OutLeft", /*isEvent=*/false);
    AddConn(asset, multiply.m_ID, "Out", UUID(0), "OutRight", /*isEvent=*/false);

    Ref<sg::Prototype> prototype = sg::CompileAssetToPrototype(asset);
    ASSERT_NE(prototype, nullptr);
    Ref<sg::SoundGraph> instance = sg::CreateInstance(prototype);
    ASSERT_NE(instance, nullptr);

    auto [poolBegin, poolEnd] = instance->GetNodeOutputPoolRange();
    ASSERT_NE(poolBegin, nullptr) << "CreateInstance must pool node output buffers";

    auto* sineNode = dynamic_cast<sg::SineOscillator*>(instance->FindNodeByID(sine.m_ID));
    auto* mulNode = dynamic_cast<sg::Multiply<f32>*>(instance->FindNodeByID(multiply.m_ID));
    ASSERT_NE(sineNode, nullptr);
    ASSERT_NE(mulNode, nullptr);

    const f32* sineOut = sineNode->m_OutValue.Data();
    const f32* mulOut = mulNode->m_Out.Data();

    // Both outputs live inside the one contiguous pool allocation...
    EXPECT_GE(sineOut, poolBegin);
    EXPECT_LT(sineOut, poolEnd);
    EXPECT_GE(mulOut, poolBegin);
    EXPECT_LT(mulOut, poolEnd);
    // ...on distinct slots, spaced by whole kMaxAudioBlockFrames strides.
    EXPECT_NE(sineOut, mulOut);
    const std::ptrdiff_t slotDiff = mulOut > sineOut ? (mulOut - sineOut) : (sineOut - mulOut);
    EXPECT_EQ(slotDiff % static_cast<std::ptrdiff_t>(sg::kMaxAudioBlockFrames), 0)
        << "pooled outputs must sit on kMaxAudioBlockFrames-aligned slots";

    // Pooling preserved behaviour: the graph still produces the sine*0.5 signal.
    constexpr u32 kBlock = 480;
    instance->Process(kBlock);
    ASSERT_GE(instance->m_OutputBuffers.size(), 1u);
    f32 maxAbs = 0.0f;
    for (u32 frame = 0; frame < kBlock; ++frame)
        maxAbs = std::max(maxAbs, std::abs(instance->m_OutputBuffers[0][frame]));
    EXPECT_GT(maxAbs, 0.45f) << "pooled graph produced no signal";
    EXPECT_LE(maxAbs, 0.5f + 1e-5f);
}

TEST(SoundGraphTypedConnections, DebugBlockProcessingKeepsRealTimeHeadroom)
{
    // Regression net for the Phase 1 perf bug: one second of audio (100 blocks
    // of 480 frames) through an oscillator → multiply → output graph processed
    // in a small fraction of real time even in Debug (pre-Phase-2 the per-sample
    // node walk took ~5.3 seconds per second of audio). The functional part —
    // the workload runs and produces signal — always asserts; the wall-clock
    // budget only asserts when OLOENGINE_SOUNDGRAPH_PERF=1 is set (perf opt-in),
    // because unit runs must not fail on host timing under load. The measured
    // time is always printed so a regression is still visible in CI logs.
    SoundGraphAsset asset;
    asset.SetName("PerfNet");

    SoundGraphNodeData sine;
    sine.m_ID = UUID();
    sine.m_Type = "SineOscillator";
    sine.m_Name = "Sine";
    sine.m_Properties["Frequency"] = "440";
    sine.m_Properties["Amplitude"] = "1.0";
    asset.AddNode(sine);

    SoundGraphNodeData multiply;
    multiply.m_ID = UUID();
    multiply.m_Type = "Multiply<float>";
    multiply.m_Name = "Volume";
    multiply.m_Properties["Multiplier"] = "0.5";
    asset.AddNode(multiply);

    AddConn(asset, sine.m_ID, "OutValue", multiply.m_ID, "Value", /*isEvent=*/false);
    AddConn(asset, multiply.m_ID, "Out", UUID(0), "OutLeft", /*isEvent=*/false);
    AddConn(asset, multiply.m_ID, "Out", UUID(0), "OutRight", /*isEvent=*/false);

    Ref<sg::Prototype> prototype = sg::CompileAssetToPrototype(asset);
    ASSERT_NE(prototype, nullptr);
    Ref<sg::SoundGraph> instance = sg::CreateInstance(prototype);
    ASSERT_NE(instance, nullptr);

    constexpr u32 kBlock = 480;
    constexpr u32 kBlocks = 100; // 100 * 480 frames = 1 second at 48 kHz

    const auto start = std::chrono::steady_clock::now();
    for (u32 i = 0; i < kBlocks; ++i)
        instance->Process(kBlock);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    // Functional check (always on): the workload produced a signal, not silence.
    ASSERT_GE(instance->m_OutputBuffers.size(), 1u);
    f32 maxAbs = 0.0f;
    for (u32 frame = 0; frame < kBlock; ++frame)
        maxAbs = std::max(maxAbs, std::abs(instance->m_OutputBuffers[0][frame]));
    EXPECT_GT(maxAbs, 0.1f) << "perf-net graph produced silence — wiring broke";

    const f64 elapsedMs = std::chrono::duration<f64, std::milli>(elapsed).count();
    std::cout << "[SoundGraphPerf] 1 s of audio (100x480 frames) processed in "
              << elapsedMs << " ms\n";

    const char* perfOptIn = std::getenv("OLOENGINE_SOUNDGRAPH_PERF");
    if (perfOptIn && *perfOptIn && std::string_view(perfOptIn) != "0")
    {
        EXPECT_LT(elapsedMs, 1000.0)
            << "1 s of audio took " << elapsedMs << " ms — block-rate processing has regressed toward per-sample cost";
    }
}

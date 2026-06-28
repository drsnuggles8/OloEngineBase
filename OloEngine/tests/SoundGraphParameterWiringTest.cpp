#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// SoundGraphParameterWiringTest — runtime parameter / preset wiring
// (docs/design/soundgraph-metasounds.md, runtime completion of Phases 2-4)
//
// SoundGraphSource exposed three parameter methods that used to be silent
// no-op stubs: UpdateParameterSet (built no handles), ApplyParameterPresetInternal
// (returned success but applied nothing), and UpdateChangedParameters (empty body,
// never even called). A user setting a parameter or applying a preset on a playing
// SoundGraph got no effect and no error.
//
// This file pins the wired-up behaviour, driving the real audio path
// (SoundGraphSource::ProcessSamples) against a hand-built graph whose single
// exposed input parameter routes into a capture node:
//   * UpdateParameterSet discovers the graph's real exposed parameters (the FNV
//     hash of each endpoint name), exactly the ones the graph exposes — no more,
//     no less.
//   * A live SetParameter write reaches the graph and the value is observed by a
//     downstream node on the next processed block.
//   * ApplyParameterPreset flattens its registered descriptors and the audio
//     thread applies them — the value the preset declared shows up in the graph.
//   * A preset parameter that doesn't resolve to a graph endpoint is rejected
//     (reported, not silently swallowed) and leaves real parameters untouched.
//
// The source is exercised without a miniaudio device: ProcessSamples only needs a
// graph and an output bus buffer, both of which we provide directly. This mirrors
// the device-free graph drive in SoundGraphInstantiationTest / the typed-connection
// tests.
// =============================================================================

#include "OloEngine/Audio/SoundGraph/SoundGraph.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphSource.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphPatchPreset.h"
#include "OloEngine/Core/Hash.h"
#include "OloEngine/Core/Identifier.h"
#include "OloEngine/Core/UUID.h"

#include <choc/containers/choc_Value.h>

#include <array>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace)
namespace sg = OloEngine::Audio::SoundGraph;

namespace
{
    constexpr u32 kBlock = 64;
    constexpr u32 kChannels = 2; // SoundGraphSource default output channel count

    /// Test-only node: records the per-frame values seen on an audio-rate input ref,
    /// so a graph-parameter write can be observed end to end. Same shape as the
    /// CaptureNode in SoundGraphTypedConnectionTest.
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

    /// Build a graph that exposes `paramNames` as graph input streams and routes the
    /// first one into a CaptureNode's "In" input. Returns the initialised graph and
    /// the capture node (owned by the graph) for value inspection.
    struct WiredGraph
    {
        Ref<sg::SoundGraph> m_Graph;
        CaptureNode* m_Capture = nullptr;
    };

    WiredGraph MakeGraph(const std::vector<std::string>& paramNames, const std::string& routedParam)
    {
        WiredGraph wired;
        wired.m_Graph = Ref<sg::SoundGraph>::Create("ParamWiring", UUID());

        for (const std::string& name : paramNames)
            wired.m_Graph->AddGraphInputStream(Identifier(name), choc::value::Value(0.0f));

        auto capture = CreateScope<CaptureNode>(UUID());
        wired.m_Capture = capture.get();
        const UUID captureID = capture->m_ID;
        wired.m_Graph->AddNode(std::move(capture));

        wired.m_Graph->AddInputValueRoute(Identifier(routedParam), captureID, Identifier("In"));
        wired.m_Graph->Init();
        return wired;
    }

    /// Drive one block through the source, writing into a scratch interleaved bus.
    void ProcessOneBlock(sg::SoundGraphSource& source)
    {
        std::array<f32, kBlock * kChannels> bus{};
        f32* busPtr = bus.data();
        source.ProcessSamples(&busPtr, kBlock);
    }
} // namespace

// UpdateParameterSet must discover exactly the graph's exposed parameters — the
// stub left m_ParameterHandles empty, so HasParameter always answered false and the
// preset path had nothing to resolve against.
TEST(SoundGraphParameterWiring, UpdateParameterSetDiscoversExposedParameters)
{
    WiredGraph wired = MakeGraph({ "Cutoff", "Resonance" }, "Cutoff");

    sg::SoundGraphSource source;
    source.ReplaceGraph(wired.m_Graph);

    EXPECT_EQ(source.GetParameterCount(), 2u) << "both exposed input streams must be discovered";
    EXPECT_TRUE(source.HasParameter("Cutoff"));
    EXPECT_TRUE(source.HasParameter("Resonance"));
    EXPECT_EQ(source.HasParameter(static_cast<u32>(Identifier("Cutoff"))), true)
        << "hashed-ID lookup must match the name lookup";
    EXPECT_FALSE(source.HasParameter("NotAParameter")) << "unknown names must not resolve";
}

// A live SetParameter write must actually reach the graph and be observed downstream.
TEST(SoundGraphParameterWiring, SetParameterReachesGraph)
{
    WiredGraph wired = MakeGraph({ "Cutoff" }, "Cutoff");

    sg::SoundGraphSource source;
    source.ReplaceGraph(wired.m_Graph);

    // Unknown parameter is rejected now that the handle map is populated.
    EXPECT_FALSE(source.SetParameter("Nope", choc::value::createFloat32(1.0f)));

    constexpr f32 kValue = 0.7f;
    ASSERT_TRUE(source.SetParameter("Cutoff", choc::value::createFloat32(kValue)));

    ProcessOneBlock(source);

    ASSERT_EQ(wired.m_Capture->m_Captured.size(), kBlock);
    EXPECT_NEAR(wired.m_Capture->m_Captured.front(), kValue, 1e-6f)
        << "graph must observe the parameter from the first frame of the block";
    EXPECT_NEAR(wired.m_Capture->m_Captured.back(), kValue, 1e-6f);
}

// ApplyParameterPreset must flatten its descriptors and the audio thread must apply
// them — the declared default value has to show up in the graph.
TEST(SoundGraphParameterWiring, ApplyParameterPresetAppliesDescriptorDefaults)
{
    WiredGraph wired = MakeGraph({ "Cutoff" }, "Cutoff");

    sg::SoundGraphSource source;
    source.ReplaceGraph(wired.m_Graph);

    constexpr f32 kPresetValue = 0.33f;
    sg::SoundGraphPatchPreset preset;
    preset.SetName("CutoffPreset");
    sg::ParameterDescriptor cutoff;
    cutoff.ID = static_cast<u32>(Identifier("Cutoff")); // descriptor IDs are graph endpoint hashes
    cutoff.Name = "Cutoff";
    cutoff.DefaultValue = kPresetValue;
    preset.RegisterParameter(cutoff);

    ASSERT_TRUE(source.ApplyParameterPreset(preset)) << "at least one descriptor resolved";

    // The queued write is drained and applied on the first processed block.
    ProcessOneBlock(source);

    ASSERT_EQ(wired.m_Capture->m_Captured.size(), kBlock);
    EXPECT_NEAR(wired.m_Capture->m_Captured.front(), kPresetValue, 1e-6f)
        << "preset descriptor default must reach the graph endpoint";
    EXPECT_NEAR(wired.m_Capture->m_Captured.back(), kPresetValue, 1e-6f);
}

// A preset whose descriptor IDs don't correspond to any graph endpoint must be
// rejected (returns false / is reported), not silently treated as applied.
TEST(SoundGraphParameterWiring, ApplyParameterPresetRejectsUnresolvedParameters)
{
    WiredGraph wired = MakeGraph({ "Cutoff" }, "Cutoff");

    sg::SoundGraphSource source;
    source.ReplaceGraph(wired.m_Graph);

    // Seed a known value via the live path so we can prove the bad preset left it alone.
    constexpr f32 kSeed = 0.2f;
    ASSERT_TRUE(source.SetParameter("Cutoff", choc::value::createFloat32(kSeed)));

    sg::SoundGraphPatchPreset preset;
    sg::ParameterDescriptor bogus;
    bogus.ID = static_cast<u32>(Identifier("DoesNotExist"));
    bogus.Name = "DoesNotExist";
    bogus.DefaultValue = 0.99f;
    preset.RegisterParameter(bogus);

    EXPECT_FALSE(source.ApplyParameterPreset(preset))
        << "a preset with no resolvable parameters must report failure";

    ProcessOneBlock(source);

    ASSERT_EQ(wired.m_Capture->m_Captured.size(), kBlock);
    EXPECT_NEAR(wired.m_Capture->m_Captured.front(), kSeed, 1e-6f)
        << "the unresolved preset must not have perturbed a real parameter";
}

// A graph swap must discard parameter writes still queued for the previous graph — a value
// validated against the old graph's endpoints must not leak onto the new graph when the audio
// thread next drains. ReplaceGraph clears the hand-off queue under the suspend protocol.
TEST(SoundGraphParameterWiring, GraphSwapDiscardsQueuedUpdatesForOldGraph)
{
    WiredGraph graphA = MakeGraph({ "Cutoff" }, "Cutoff");
    WiredGraph graphB = MakeGraph({ "Cutoff" }, "Cutoff");

    sg::SoundGraphSource source;
    source.ReplaceGraph(graphA.m_Graph);

    // Queue a preset write against graph A but leave it in the queue (don't process yet).
    constexpr f32 kStale = 0.9f;
    sg::SoundGraphPatchPreset preset;
    sg::ParameterDescriptor cutoff;
    cutoff.ID = static_cast<u32>(Identifier("Cutoff"));
    cutoff.Name = "Cutoff";
    cutoff.DefaultValue = kStale;
    preset.RegisterParameter(cutoff);
    ASSERT_TRUE(source.ApplyParameterPreset(preset));

    // Swap to graph B before the audio thread ever drains the queue.
    source.ReplaceGraph(graphB.m_Graph);

    ProcessOneBlock(source);

    ASSERT_EQ(graphB.m_Capture->m_Captured.size(), kBlock);
    EXPECT_NEAR(graphB.m_Capture->m_Captured.front(), 0.0f, 1e-6f)
        << "graph B must keep its default — the stale graph-A preset write must not survive the swap";
}

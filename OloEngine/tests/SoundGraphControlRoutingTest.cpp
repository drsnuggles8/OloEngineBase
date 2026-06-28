#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// OLO_TEST_LAYER: unit
//
// =============================================================================
// SoundGraphControlRoutingTest — high-level Sound controls reach the live graph
//
// SoundGraphSound's SetVolume / SetPitch / SetLooping / SetLowPassFilter /
// SetHighPassFilter used to only store a member ("Actual ... control would be
// implemented via SoundGraph parameters") — so a scene's authored VolumeMultiplier /
// PitchMultiplier / Looping (forwarded by Scene::InitializeAudioSoundGraph) was
// silently dropped before it ever reached the audio graph.
//
// They now route the (clamped, finite-checked) value into the running SoundGraph as a
// conventionally-named graph input parameter (Volume / Pitch / Loop / LowPass /
// HighPass), reusing the proven SoundGraphSource::SetParameter path. This file pins
// that behaviour end to end, driving the real audio path
// (SoundGraphSource::ProcessSamples) without a miniaudio device — exactly the
// device-free graph drive used in SoundGraphParameterWiringTest. A SoundGraphSound is
// given a *detached* source (InitializeDetachedSource: a SoundGraphSource that is never
// attached to an ma_engine but still processes its graph), so the routing path is
// exercised with no audio hardware.
//
// Covered:
//   * each float control reaches its graph input cell with the clamped value, and a
//     routed control is observed per-frame by a downstream node;
//   * looping routes as a bool to the "Loop" endpoint;
//   * values are clamped before routing (volume [0,1], pitch [0.1,4], filters [0,1]);
//   * a non-finite value is rejected (member + graph left unchanged);
//   * a control set BEFORE the graph is installed is applied on InitializeFromGraph;
//   * a graph that doesn't expose a control's endpoint ignores the write without
//     crashing, and the member is still stored (so the getter stays correct).
// =============================================================================

#include "OloEngine/Audio/SoundGraph/SoundGraph.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphSound.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphSource.h"
#include "OloEngine/Core/Identifier.h"
#include "OloEngine/Core/UUID.h"

#include <choc/containers/choc_Value.h>

#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace)
namespace sg = OloEngine::Audio::SoundGraph;

namespace
{
    constexpr u32 kBlock = 64;
    constexpr u32 kChannels = 2; // SoundGraphSource default output channel count

    // The conventional names SoundGraphSound routes its high-level controls to. Kept in
    // the test as literals (not shared with the .cpp constants) so a rename of either
    // side surfaces as a test failure rather than silently agreeing.
    constexpr const char* kVolume = "Volume";
    constexpr const char* kPitch = "Pitch";
    constexpr const char* kLoop = "Loop";
    constexpr const char* kLowPass = "LowPass";
    constexpr const char* kHighPass = "HighPass";

    /// Records the per-frame values seen on an audio-rate input ref, so a graph-parameter
    /// write can be observed end to end (same shape as SoundGraphParameterWiringTest's).
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

    struct ControlRig
    {
        sg::SoundGraphSound m_Sound;
        Ref<sg::SoundGraph> m_Graph;
        CaptureNode* m_Capture = nullptr; // owned by m_Graph

        f32 FloatCell(const char* name) const
        {
            return m_Graph->m_EndpointInputStreams.at(Identifier(name)).m_Float;
        }
        bool BoolCell(const char* name) const
        {
            return m_Graph->m_EndpointInputStreams.at(Identifier(name)).m_Bool;
        }
    };

    /// Build a graph exposing the conventional control endpoints. `floatParams` are added
    /// as float input streams (the first is routed into a CaptureNode's audio-rate "In"),
    /// `boolParams` as bool input streams. A non-null `routedFloat` overrides which float
    /// is wired to the capture node.
    Ref<sg::SoundGraph> MakeControlGraph(const std::vector<std::string>& floatParams,
                                         const std::vector<std::string>& boolParams,
                                         const std::string& routedFloat,
                                         CaptureNode** outCapture)
    {
        auto graph = Ref<sg::SoundGraph>::Create("ControlRouting", UUID());

        for (const std::string& name : floatParams)
            graph->AddGraphInputStream(Identifier(name), choc::value::createFloat32(0.0f));
        for (const std::string& name : boolParams)
            graph->AddGraphInputStream(Identifier(name), choc::value::createBool(false));

        auto capture = CreateScope<CaptureNode>(UUID());
        *outCapture = capture.get();
        const UUID captureID = capture->m_ID;
        graph->AddNode(std::move(capture));

        if (!routedFloat.empty())
            graph->AddInputValueRoute(Identifier(routedFloat), captureID, Identifier("In"));

        graph->Init();
        return graph;
    }

    /// Stand up a SoundGraphSound on a detached (device-free) source with the given graph
    /// installed. SyncControlParametersToGraph runs inside InitializeFromGraph, so the
    /// rig's cells start at the Sound's default control values.
    void InstallGraph(ControlRig& rig, const Ref<sg::SoundGraph>& graph)
    {
        ASSERT_TRUE(rig.m_Sound.InitializeDetachedSource());
        ASSERT_TRUE(rig.m_Sound.InitializeFromGraph(graph));
    }

    /// Drive one block through the Sound's source, writing into a scratch interleaved bus.
    void ProcessOneBlock(ControlRig& rig)
    {
        std::array<f32, kBlock * kChannels> bus{};
        f32* busPtr = bus.data();
        auto* source = rig.m_Sound.GetSource();
        ASSERT_NE(source, nullptr);
        source->ProcessSamples(&busPtr, kBlock);
    }
} // namespace

// A routed float control must reach its graph input cell AND be observed per-frame by a
// downstream node — proving SetVolume actually drives the graph, not just a member.
TEST(SoundGraphControlRouting, VolumeReachesGraphAndIsObservedDownstream)
{
    ControlRig rig;
    rig.m_Graph = MakeControlGraph({ kVolume }, {}, kVolume, &rig.m_Capture);
    InstallGraph(rig, rig.m_Graph);

    rig.m_Sound.SetVolume(0.5f);
    EXPECT_FLOAT_EQ(rig.m_Sound.GetVolume(), 0.5f);
    EXPECT_FLOAT_EQ(rig.FloatCell(kVolume), 0.5f) << "SetVolume must write the graph's Volume cell";

    ProcessOneBlock(rig);

    ASSERT_EQ(rig.m_Capture->m_Captured.size(), kBlock);
    EXPECT_FLOAT_EQ(rig.m_Capture->m_Captured.front(), 0.5f)
        << "the routed Volume value must be seen from the first frame";
    EXPECT_FLOAT_EQ(rig.m_Capture->m_Captured.back(), 0.5f);
}

// Pitch and the two filters each route to their own conventional endpoint cell.
TEST(SoundGraphControlRouting, PitchAndFiltersReachTheirCells)
{
    ControlRig rig;
    rig.m_Graph = MakeControlGraph({ kVolume, kPitch, kLowPass, kHighPass }, {}, kVolume, &rig.m_Capture);
    InstallGraph(rig, rig.m_Graph);

    rig.m_Sound.SetPitch(2.0f);
    rig.m_Sound.SetLowPassFilter(0.25f);
    rig.m_Sound.SetHighPassFilter(0.75f);

    EXPECT_FLOAT_EQ(rig.FloatCell(kPitch), 2.0f);
    EXPECT_FLOAT_EQ(rig.FloatCell(kLowPass), 0.25f);
    EXPECT_FLOAT_EQ(rig.FloatCell(kHighPass), 0.75f);
}

// Looping routes as a bool to the "Loop" endpoint.
TEST(SoundGraphControlRouting, LoopingRoutesAsBool)
{
    ControlRig rig;
    rig.m_Graph = MakeControlGraph({}, { kLoop }, "", &rig.m_Capture);
    InstallGraph(rig, rig.m_Graph);

    rig.m_Sound.SetLooping(true);
    EXPECT_TRUE(rig.BoolCell(kLoop));

    rig.m_Sound.SetLooping(false);
    EXPECT_FALSE(rig.BoolCell(kLoop));
}

// Out-of-range controls are clamped BEFORE routing, so the graph never sees a value
// outside the documented range.
TEST(SoundGraphControlRouting, ControlsAreClampedBeforeRouting)
{
    ControlRig rig;
    rig.m_Graph = MakeControlGraph({ kVolume, kPitch, kLowPass, kHighPass }, {}, kVolume, &rig.m_Capture);
    InstallGraph(rig, rig.m_Graph);

    rig.m_Sound.SetVolume(2.0f); // > 1.0
    EXPECT_FLOAT_EQ(rig.m_Sound.GetVolume(), 1.0f);
    EXPECT_FLOAT_EQ(rig.FloatCell(kVolume), 1.0f);

    rig.m_Sound.SetVolume(-1.0f); // < 0.0
    EXPECT_FLOAT_EQ(rig.FloatCell(kVolume), 0.0f);

    rig.m_Sound.SetPitch(10.0f); // > 4.0
    EXPECT_FLOAT_EQ(rig.m_Sound.GetPitch(), 4.0f);
    EXPECT_FLOAT_EQ(rig.FloatCell(kPitch), 4.0f);

    rig.m_Sound.SetPitch(0.0f); // < 0.1
    EXPECT_FLOAT_EQ(rig.m_Sound.GetPitch(), 0.1f);
    EXPECT_FLOAT_EQ(rig.FloatCell(kPitch), 0.1f);

    rig.m_Sound.SetLowPassFilter(5.0f);
    EXPECT_FLOAT_EQ(rig.FloatCell(kLowPass), 1.0f);
    rig.m_Sound.SetHighPassFilter(-5.0f);
    EXPECT_FLOAT_EQ(rig.FloatCell(kHighPass), 0.0f);
}

// A non-finite value is rejected: the member keeps its previous value and the graph cell
// is left untouched (NaN must not slip through std::clamp into the audio graph).
TEST(SoundGraphControlRouting, NonFiniteControlIsRejected)
{
    ControlRig rig;
    rig.m_Graph = MakeControlGraph({ kVolume, kPitch }, {}, kVolume, &rig.m_Capture);
    InstallGraph(rig, rig.m_Graph);

    rig.m_Sound.SetVolume(0.5f);
    rig.m_Sound.SetPitch(1.5f);

    rig.m_Sound.SetVolume(std::numeric_limits<f32>::quiet_NaN());
    rig.m_Sound.SetPitch(std::numeric_limits<f32>::infinity());

    EXPECT_FLOAT_EQ(rig.m_Sound.GetVolume(), 0.5f) << "NaN volume must be ignored";
    EXPECT_FLOAT_EQ(rig.m_Sound.GetPitch(), 1.5f) << "Inf pitch must be ignored";
    EXPECT_FLOAT_EQ(rig.FloatCell(kVolume), 0.5f);
    EXPECT_FLOAT_EQ(rig.FloatCell(kPitch), 1.5f);
}

// A control set BEFORE a graph is installed must still take effect once it is — the
// SetX-then-InitializeFromGraph ordering Scene doesn't use today but the public
// IPlayableAudio interface allows.
TEST(SoundGraphControlRouting, ControlsSetBeforeGraphInstallApplyOnInit)
{
    ControlRig rig;
    ASSERT_TRUE(rig.m_Sound.InitializeDetachedSource());

    // No graph yet: these store members and route into nothing.
    rig.m_Sound.SetVolume(0.3f);
    rig.m_Sound.SetPitch(2.5f);
    rig.m_Sound.SetLooping(true);

    rig.m_Graph = MakeControlGraph({ kVolume, kPitch }, { kLoop }, kVolume, &rig.m_Capture);
    ASSERT_TRUE(rig.m_Sound.InitializeFromGraph(rig.m_Graph));

    EXPECT_FLOAT_EQ(rig.FloatCell(kVolume), 0.3f) << "pre-install volume must be synced on init";
    EXPECT_FLOAT_EQ(rig.FloatCell(kPitch), 2.5f);
    EXPECT_TRUE(rig.BoolCell(kLoop));
}

// A graph that doesn't expose a control's endpoint must ignore the write without
// crashing, and the member must still be stored so the getter stays correct.
TEST(SoundGraphControlRouting, GraphWithoutEndpointIgnoresControlGracefully)
{
    ControlRig rig;
    // Graph exposes only an unrelated parameter — no Volume/Pitch/etc.
    rig.m_Graph = MakeControlGraph({ "Unrelated" }, {}, "Unrelated", &rig.m_Capture);
    InstallGraph(rig, rig.m_Graph);

    rig.m_Sound.SetVolume(0.42f);
    rig.m_Sound.SetPitch(3.0f);
    rig.m_Sound.SetLooping(true);

    // No throw, no effect on the unrelated graph, and the members are still stored.
    EXPECT_FLOAT_EQ(rig.m_Sound.GetVolume(), 0.42f);
    EXPECT_FLOAT_EQ(rig.m_Sound.GetPitch(), 3.0f);
    EXPECT_FALSE(rig.m_Graph->m_EndpointInputStreams.contains(Identifier(kVolume)));
}

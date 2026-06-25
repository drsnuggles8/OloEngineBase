#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// SoundGraphSampleAccurateTriggerTest — Phase 4 sample-accurate triggers
// (docs/soundgraph-metasounds-refactor.md)
//
// Phase 4 makes trigger-consuming nodes split their per-frame Process() at the
// frame offset a trigger carries, so an event that arrives mid-block takes
// effect at the exact sample instead of being quantised to the block boundary
// (the Phase 1-3 behavior). The mechanism:
//   * Trigger / TriggerRef (StreamRefs.h) carry an i32 frame offset.
//   * InputEvent / OutputEvent thread an i32 sampleOffset through the event
//     system (NodeProcessor.h) so a producer firing mid-block tells consumers
//     the exact frame.
//   * WavePlayer (Play/Stop) and the AD / ADSR envelopes (trigger/release)
//     apply their state change at that frame and propagate the offset on their
//     own output events.
//
// These tests pin (no GL / no audio asset needed):
//   * The Trigger value type: fire/consume/clamp/first-fire-wins.
//   * Offset plumbing through OutputEvent -> InputEvent.
//   * AD envelope: a trigger at offset N shifts the whole attack by exactly N
//     frames; offset 0 matches the legacy block-boundary start.
//   * ADSR envelope: trigger and release each take effect at their exact frame.
//   * End-to-end: a producer firing mid-block retriggers a wired envelope at
//     that frame; RepeatTrigger emits sample-accurate offsets.
//   * WavePlayer: Play/Stop are consumed at their exact frame (observed through
//     the OnStop / OnFinished output-event offset, since audio output needs a
//     loaded asset which a unit test does not mount).
//
// The follow-on increment (external, game-code triggers) is pinned here too:
//   * SoundGraph::SendInputEvent forwards a sample offset through a graph-input
//     route to the consuming node's trigger (the last intra-graph hop).
//   * SoundGraphSource::ClampInputEventOffset validates an externally supplied
//     offset into [0, blockSize).
//   * End-to-end through the real audio path: a trigger handed to
//     SoundGraphSource::SendInputEvent with a mid-block offset is drained by
//     ProcessSamples and retriggers a wired envelope at that exact frame —
//     i.e. an external footstep is sample-accurate, not block-quantised.
// =============================================================================

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/SoundGraph.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphSource.h"
#include "OloEngine/Audio/SoundGraph/StreamRefs.h"
#include "OloEngine/Audio/SoundGraph/Nodes/EnvelopeNodes.h"
#include "OloEngine/Audio/SoundGraph/Nodes/TriggerNodes.h"
#include "OloEngine/Audio/SoundGraph/Nodes/WavePlayer.h"
#include "OloEngine/Core/Identifier.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"

#include <choc/containers/choc_Value.h>

#include <array>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace)
namespace sg = OloEngine::Audio::SoundGraph;

namespace
{
    /// Captures every (value, sampleOffset) delivered to an offset-aware input
    /// event. Used to observe the offset a producer's OutputEvent forwards.
    struct OffsetCaptureNode : public sg::NodeProcessor
    {
        explicit OffsetCaptureNode(UUID id) : NodeProcessor("OffsetCapture", id)
        {
            AddInEvent(Identifier("In"), [this](f32 value, i32 sampleOffset)
                       {
                           m_Values.push_back(value);
                           m_Offsets.push_back(sampleOffset); });
        }

        std::vector<f32> m_Values;
        std::vector<i32> m_Offsets;
    };

    /// Test-only producer: fires its trigger output once per Process at a fixed
    /// frame offset. Exercises the OutputEvent -> InputEvent offset path end to end.
    struct FireAtOffsetNode : public sg::NodeProcessor
    {
        explicit FireAtOffsetNode(UUID id) : NodeProcessor("FireAtOffset", id) {}

        i32 m_Offset = 0;
        OutputEvent m_Out{ *this };

        void Process(u32 numFrames) final
        {
            (void)numFrames;
            m_Out(1.0f, m_Offset);
        }
    };

    constexpr u32 kBlock = 480;
    constexpr f32 kSampleRate = 48000.0f;

    std::vector<f32> RunADEnvelope(i32 triggerOffset)
    {
        sg::ADEnvelope env("AD", UUID());
        // 50 ms attack / decay: ~2400 samples each, so the envelope is still
        // rising for the whole 480-frame block — the per-frame shape is easy to
        // compare against a shifted copy.
        env.m_AttackTime.SetDefault(0.05f);
        env.m_DecayTime.SetDefault(0.05f);
        env.SetSampleRate(kSampleRate);
        env.Init();

        env.InEvent(sg::ADEnvelope::IDs::s_Trigger)(1.0f, triggerOffset);
        env.Process(kBlock);

        const f32* out = env.m_OutEnvelope.Data();
        return std::vector<f32>(out, out + kBlock);
    }
} // namespace

class SoundGraphSampleAccurateTriggerTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        Log::Initialize(); // WavePlayer's no-asset bail logs a warning.
    }
};

// -----------------------------------------------------------------------------
// Trigger value type
// -----------------------------------------------------------------------------

TEST_F(SoundGraphSampleAccurateTriggerTest, TriggerStartsUnfired)
{
    sg::Trigger t;
    EXPECT_FALSE(t.IsFired());
    EXPECT_EQ(t.Offset(), sg::Trigger::kNotFired);
    EXPECT_EQ(t.Consume(), sg::Trigger::kNotFired);
}

TEST_F(SoundGraphSampleAccurateTriggerTest, TriggerFireRecordsOffsetAndConsumeClears)
{
    sg::Trigger t;
    t.Fire(137);
    EXPECT_TRUE(t.IsFired());
    EXPECT_EQ(t.Offset(), 137);

    EXPECT_EQ(t.Consume(), 137);
    // Consume read-and-clears.
    EXPECT_FALSE(t.IsFired());
    EXPECT_EQ(t.Consume(), sg::Trigger::kNotFired);
}

TEST_F(SoundGraphSampleAccurateTriggerTest, TriggerFirstFireWinsWithinABlock)
{
    sg::Trigger t;
    t.Fire(200);
    t.Fire(50);  // earlier — should win
    t.Fire(300); // later — ignored
    EXPECT_EQ(t.Offset(), 50);
}

TEST_F(SoundGraphSampleAccurateTriggerTest, TriggerNegativeOffsetClampsToBlockStart)
{
    sg::Trigger t;
    t.Fire(-1); // a legacy block-boundary event with no frame info
    EXPECT_TRUE(t.IsFired());
    EXPECT_EQ(t.Offset(), 0);
}

// -----------------------------------------------------------------------------
// Event-system offset plumbing
// -----------------------------------------------------------------------------

TEST_F(SoundGraphSampleAccurateTriggerTest, OutputEventForwardsSampleOffsetToInputEvent)
{
    FireAtOffsetNode producer{ UUID() };
    producer.m_Offset = 271;

    OffsetCaptureNode capture{ UUID() };
    producer.m_Out.AddDestination(capture.GetInputEvent(Identifier("In")));

    producer.Process(kBlock);

    ASSERT_EQ(capture.m_Offsets.size(), 1u);
    EXPECT_EQ(capture.m_Offsets[0], 271);
    EXPECT_FLOAT_EQ(capture.m_Values[0], 1.0f);
}

TEST_F(SoundGraphSampleAccurateTriggerTest, LegacySingleArgFireDeliversBlockBoundaryOffset)
{
    OffsetCaptureNode capture{ UUID() };
    sg::NodeProcessor::OutputEvent out{ capture }; // owner is irrelevant for the fire
    out.AddDestination(capture.GetInputEvent(Identifier("In")));

    out(1.0f); // single-argument legacy fire

    ASSERT_EQ(capture.m_Offsets.size(), 1u);
    EXPECT_EQ(capture.m_Offsets[0], 0) << "a value-only fire must land at the block start";
}

// -----------------------------------------------------------------------------
// AD envelope split
// -----------------------------------------------------------------------------

TEST_F(SoundGraphSampleAccurateTriggerTest, ADEnvelopeTriggerOffsetShiftsAttackByExactlyThatManyFrames)
{
    constexpr i32 kOffset = 137;

    const std::vector<f32> base = RunADEnvelope(0);
    const std::vector<f32> shifted = RunADEnvelope(kOffset);

    // The attack actually produced rising signal in the baseline.
    ASSERT_GT(base[0], 0.0f) << "attack must start at frame 0 for offset 0";
    EXPECT_GT(base[100], base[0]) << "attack must be rising";

    // Frames before the trigger are pure silence (Idle).
    for (i32 f = 0; f < kOffset; ++f)
        EXPECT_FLOAT_EQ(shifted[static_cast<u32>(f)], 0.0f)
            << "frame " << f << " before the trigger must be silent";

    // The envelope shape is identical, just delayed by exactly kOffset frames.
    for (u32 f = 0; f + static_cast<u32>(kOffset) < kBlock; ++f)
        EXPECT_NEAR(shifted[f + static_cast<u32>(kOffset)], base[f], 1e-6f)
            << "shifted output must equal the baseline at frame " << f;
}

TEST_F(SoundGraphSampleAccurateTriggerTest, ADEnvelopeOffsetZeroStartsAtFrameZero)
{
    const std::vector<f32> out = RunADEnvelope(0);
    EXPECT_GT(out[0], 0.0f) << "offset 0 must apply the attack at frame 0 (legacy block-boundary timing)";
    EXPECT_GT(out[1], out[0]) << "attack must already be rising from frame 0";
}

TEST_F(SoundGraphSampleAccurateTriggerTest, ADEnvelopeWithoutTriggerStaysSilent)
{
    sg::ADEnvelope env("AD", UUID());
    env.m_AttackTime.SetDefault(0.05f);
    env.m_DecayTime.SetDefault(0.05f);
    env.SetSampleRate(kSampleRate);
    env.Init();

    env.Process(kBlock); // no trigger fired

    const f32* out = env.m_OutEnvelope.Data();
    for (u32 f = 0; f < kBlock; ++f)
        EXPECT_FLOAT_EQ(out[f], 0.0f) << "an un-triggered envelope must be silent at frame " << f;
}

// -----------------------------------------------------------------------------
// ADSR envelope split
// -----------------------------------------------------------------------------

TEST_F(SoundGraphSampleAccurateTriggerTest, ADSREnvelopeReleaseTakesEffectAtItsExactFrame)
{
    constexpr i32 kReleaseOffset = 200;
    constexpr f32 kSustain = 0.5f;

    sg::ADSREnvelope env("ADSR", UUID());
    env.m_AttackTime.SetDefault(0.0f); // immediate attack
    env.m_DecayTime.SetDefault(0.0f);  // immediate decay -> straight to sustain
    env.m_SustainLevel.SetDefault(kSustain);
    env.m_ReleaseTime.SetDefault(0.05f); // ~2400-sample release
    env.SetSampleRate(kSampleRate);
    env.Init();

    // Trigger at the block start, release mid-block.
    env.InEvent(sg::ADSREnvelope::IDs::s_Trigger)(1.0f, 0);
    env.InEvent(sg::ADSREnvelope::IDs::s_Release)(1.0f, kReleaseOffset);
    env.Process(kBlock);

    const f32* out = env.m_OutEnvelope.Data();

    // Immediate attack+decay settle onto the sustain level within a couple of frames.
    EXPECT_NEAR(out[10], kSustain, 1e-6f) << "envelope must hold the sustain level before release";
    EXPECT_NEAR(out[static_cast<u32>(kReleaseOffset) - 1], kSustain, 1e-6f)
        << "sustain must hold right up to the frame before release";

    // Release begins exactly at kReleaseOffset and declines afterward.
    EXPECT_LT(out[static_cast<u32>(kReleaseOffset)], kSustain) << "release must begin at its trigger frame";
    EXPECT_LT(out[static_cast<u32>(kReleaseOffset) + 100], out[static_cast<u32>(kReleaseOffset)])
        << "release must keep declining";
}

TEST_F(SoundGraphSampleAccurateTriggerTest, ADSREnvelopeTriggerOffsetShiftsAttackByExactlyThatManyFrames)
{
    constexpr i32 kOffset = 90;

    auto run = [](i32 triggerOffset)
    {
        sg::ADSREnvelope env("ADSR", UUID());
        env.m_AttackTime.SetDefault(0.05f);
        env.m_DecayTime.SetDefault(0.05f);
        env.m_SustainLevel.SetDefault(0.5f);
        env.m_ReleaseTime.SetDefault(0.05f);
        env.SetSampleRate(kSampleRate);
        env.Init();
        env.InEvent(sg::ADSREnvelope::IDs::s_Trigger)(1.0f, triggerOffset);
        env.Process(kBlock);
        const f32* out = env.m_OutEnvelope.Data();
        return std::vector<f32>(out, out + kBlock);
    };

    const std::vector<f32> base = run(0);
    const std::vector<f32> shifted = run(kOffset);

    ASSERT_GT(base[0], 0.0f);
    for (i32 f = 0; f < kOffset; ++f)
        EXPECT_FLOAT_EQ(shifted[static_cast<u32>(f)], 0.0f) << "frame " << f << " before trigger must be silent";
    for (u32 f = 0; f + static_cast<u32>(kOffset) < kBlock; ++f)
        EXPECT_NEAR(shifted[f + static_cast<u32>(kOffset)], base[f], 1e-6f) << "shifted frame " << f;
}

// -----------------------------------------------------------------------------
// End-to-end: producer fires mid-block, wired consumer reacts at that frame
// -----------------------------------------------------------------------------

TEST_F(SoundGraphSampleAccurateTriggerTest, ProducerFiringMidBlockRetriggersWiredEnvelopeAtThatFrame)
{
    constexpr i32 kOffset = 222;

    FireAtOffsetNode producer{ UUID() };
    producer.m_Offset = kOffset;

    sg::ADEnvelope env("AD", UUID());
    env.m_AttackTime.SetDefault(0.05f);
    env.m_DecayTime.SetDefault(0.05f);
    env.SetSampleRate(kSampleRate);
    env.Init();

    // Wire producer's trigger output into the envelope's trigger input.
    producer.m_Out.AddDestination(env.GetInputEvent(sg::ADEnvelope::IDs::s_Trigger));

    // Producer before consumer, exactly as SoundGraph's process order would run them.
    producer.Process(kBlock);
    env.Process(kBlock);

    const f32* out = env.m_OutEnvelope.Data();
    for (i32 f = 0; f < kOffset; ++f)
        EXPECT_FLOAT_EQ(out[static_cast<u32>(f)], 0.0f) << "envelope must be silent before the producer fired";
    EXPECT_GT(out[static_cast<u32>(kOffset)], 0.0f) << "envelope must start the attack at the producer's fire frame";
    EXPECT_GT(out[static_cast<u32>(kOffset) + 50], out[static_cast<u32>(kOffset)]) << "attack must be rising";
}

TEST_F(SoundGraphSampleAccurateTriggerTest, RepeatTriggerEmitsSampleAccurateOffsets)
{
    sg::RepeatTrigger rt("Repeat", UUID());
    rt.m_Period.SetDefault(100.0f / kSampleRate); // fire roughly every 100 samples
    rt.SetSampleRate(kSampleRate);
    rt.Init();

    OffsetCaptureNode capture{ UUID() };
    rt.m_OutTrigger.AddDestination(capture.GetInputEvent(Identifier("In")));

    rt.InEvent(sg::RepeatTrigger::IDs::s_Start)(1.0f);
    rt.Process(kBlock);

    // Start fires once at the block boundary, then periodic fires land mid-block.
    ASSERT_GE(capture.m_Offsets.size(), 3u) << "a 480-frame block at ~100-sample period must fire several times";

    // Offsets are non-decreasing and span the block — i.e. the periodic fires are
    // NOT all collapsed to the block boundary (that was the pre-Phase-4 behavior).
    for (sizet i = 1; i < capture.m_Offsets.size(); ++i)
        EXPECT_GE(capture.m_Offsets[i], capture.m_Offsets[i - 1]) << "offsets must be non-decreasing";
    EXPECT_GT(capture.m_Offsets.back(), 0) << "later fires must carry non-zero frame offsets";
    EXPECT_LT(capture.m_Offsets.back(), static_cast<i32>(kBlock)) << "offsets stay within the block";
}

// -----------------------------------------------------------------------------
// WavePlayer: Play/Stop consumed at their exact frame (observed via output events)
// -----------------------------------------------------------------------------

TEST_F(SoundGraphSampleAccurateTriggerTest, WavePlayerStopTriggerFiresOnStopAtItsExactFrame)
{
    constexpr i32 kStopOffset = 305;

    sg::WavePlayer wp("WavePlayer", UUID());
    wp.SetSampleRate(kSampleRate);
    wp.Init();

    OffsetCaptureNode capture{ UUID() };
    wp.m_OnStop.AddDestination(capture.GetInputEvent(Identifier("In")));

    wp.InEvent(sg::WavePlayer::IDs::Stop)(1.0f, kStopOffset);
    wp.Process(kBlock);

    ASSERT_EQ(capture.m_Offsets.size(), 1u) << "Stop must fire OnStop exactly once";
    EXPECT_EQ(capture.m_Offsets[0], kStopOffset) << "OnStop must carry the Stop trigger's frame offset";
}

TEST_F(SoundGraphSampleAccurateTriggerTest, WavePlayerPlayWithoutAssetIsConsumedAtItsExactFrame)
{
    constexpr i32 kPlayOffset = 150;

    sg::WavePlayer wp("WavePlayer", UUID());
    wp.SetSampleRate(kSampleRate);
    wp.Init();

    OffsetCaptureNode capture{ UUID() };
    // With no wave asset bound, StartPlayback bails into StopPlayback -> OnStop,
    // carrying the Play trigger's frame offset. That proves the Play trigger was
    // consumed at frame kPlayOffset, not at the block boundary.
    wp.m_OnStop.AddDestination(capture.GetInputEvent(Identifier("In")));

    wp.InEvent(sg::WavePlayer::IDs::Play)(1.0f, kPlayOffset);
    wp.Process(kBlock);

    ASSERT_EQ(capture.m_Offsets.size(), 1u);
    EXPECT_EQ(capture.m_Offsets[0], kPlayOffset) << "Play must be consumed at its trigger frame";
}

TEST_F(SoundGraphSampleAccurateTriggerTest, WavePlayerWithoutTriggersFiresNoEvents)
{
    sg::WavePlayer wp("WavePlayer", UUID());
    wp.SetSampleRate(kSampleRate);
    wp.Init();

    OffsetCaptureNode capture{ UUID() };
    wp.m_OnStop.AddDestination(capture.GetInputEvent(Identifier("In")));
    wp.m_OnPlay.AddDestination(capture.GetInputEvent(Identifier("In")));

    wp.Process(kBlock);

    EXPECT_TRUE(capture.m_Offsets.empty()) << "no trigger fired -> no Play/Stop events";
}

// -----------------------------------------------------------------------------
// External (game-code) trigger path: offset survives the graph-input route, the
// source clamps it, and an externally scheduled trigger lands on the exact frame.
// -----------------------------------------------------------------------------

namespace
{
    /// Build a graph exposing input event `eventName`, routed to the (already
    /// constructed) `node`'s `nodeEvent` input. The graph takes ownership of `node`;
    /// the caller keeps a raw pointer (node.get()) for inspection. Sample rate is set
    /// before Init so node rates compute.
    Ref<sg::SoundGraph> MakeEventRoutedGraph(Scope<sg::NodeProcessor> node, UUID nodeID,
                                             Identifier eventName, Identifier nodeEvent)
    {
        auto graph = Ref<sg::SoundGraph>::Create("ExternalTrigger", UUID());
        graph->AddInEvent(eventName); // graph input-event endpoint
        graph->AddNode(std::move(node));

        EXPECT_TRUE(graph->AddInputEventsRoute(eventName, nodeID, nodeEvent))
            << "MakeEventRoutedGraph: failed to route the graph input event to the node";
        graph->SetSampleRate(kSampleRate);
        graph->Init();
        return graph;
    }
} // namespace

TEST_F(SoundGraphSampleAccurateTriggerTest, ClampInputEventOffsetKeepsOffsetsWithinTheBlock)
{
    using sg::SoundGraphSource;
    // Negative (no frame info) clamps to the block start.
    EXPECT_EQ(SoundGraphSource::ClampInputEventOffset(-1, 480), 0);
    EXPECT_EQ(SoundGraphSource::ClampInputEventOffset(-1000, 480), 0);
    // In-range offsets pass through untouched.
    EXPECT_EQ(SoundGraphSource::ClampInputEventOffset(0, 480), 0);
    EXPECT_EQ(SoundGraphSource::ClampInputEventOffset(137, 480), 137);
    EXPECT_EQ(SoundGraphSource::ClampInputEventOffset(479, 480), 479);
    // At/over the block end clamps to the last frame so the event still fires this block.
    EXPECT_EQ(SoundGraphSource::ClampInputEventOffset(480, 480), 479);
    EXPECT_EQ(SoundGraphSource::ClampInputEventOffset(99999, 480), 479);
    // A zero-length block has no frame to land on.
    EXPECT_EQ(SoundGraphSource::ClampInputEventOffset(10, 0), 0);
}

TEST_F(SoundGraphSampleAccurateTriggerTest, GraphSendInputEventForwardsSampleOffsetThroughRoute)
{
    constexpr i32 kOffset = 271;

    auto capture = CreateScope<OffsetCaptureNode>(UUID());
    auto* capturePtr = capture.get();
    const UUID captureID = capture->m_ID;
    Ref<sg::SoundGraph> graph = MakeEventRoutedGraph(std::move(capture), captureID, Identifier("Footstep"), Identifier("In"));

    // Fire the graph input event with a mid-block offset — the increment under test
    // is that the offset reaches the consumer instead of being snapped to frame 0.
    ASSERT_TRUE(graph->SendInputEvent(Identifier("Footstep"), choc::value::createFloat32(1.0f), kOffset));

    ASSERT_EQ(capturePtr->m_Offsets.size(), 1u);
    EXPECT_EQ(capturePtr->m_Offsets[0], kOffset) << "external trigger offset must survive the graph-input route";
    EXPECT_FLOAT_EQ(capturePtr->m_Values[0], 1.0f);
}

TEST_F(SoundGraphSampleAccurateTriggerTest, GraphSendInputEventDefaultsToBlockStart)
{
    auto capture = CreateScope<OffsetCaptureNode>(UUID());
    auto* capturePtr = capture.get();
    const UUID captureID = capture->m_ID;
    Ref<sg::SoundGraph> graph = MakeEventRoutedGraph(std::move(capture), captureID, Identifier("Footstep"), Identifier("In"));

    // No offset argument -> legacy block-boundary timing (frame 0).
    ASSERT_TRUE(graph->SendInputEvent(Identifier("Footstep"), choc::value::createFloat32(1.0f)));

    ASSERT_EQ(capturePtr->m_Offsets.size(), 1u);
    EXPECT_EQ(capturePtr->m_Offsets[0], 0) << "a value-only external fire must land at the block start";
}

TEST_F(SoundGraphSampleAccurateTriggerTest, GraphSendInputEventOnUnknownEndpointReturnsFalse)
{
    auto capture = CreateScope<OffsetCaptureNode>(UUID());
    auto* capturePtr = capture.get();
    const UUID captureID = capture->m_ID;
    Ref<sg::SoundGraph> graph = MakeEventRoutedGraph(std::move(capture), captureID, Identifier("Footstep"), Identifier("In"));

    EXPECT_FALSE(graph->SendInputEvent(Identifier("NoSuchEvent"), choc::value::createFloat32(1.0f), 100));
    EXPECT_TRUE(capturePtr->m_Offsets.empty()) << "an unknown endpoint must fire nothing";
}

TEST_F(SoundGraphSampleAccurateTriggerTest, ExternalTriggerThroughSourceRetriggersEnvelopeAtExactFrame)
{
    constexpr i32 kOffset = 222;
    constexpr u32 kChannels = 2; // SoundGraphSource default output channel count

    // AD envelope wired to a graph input event named "Footstep".
    auto env = CreateScope<sg::ADEnvelope>("AD", UUID());
    env->m_AttackTime.SetDefault(0.05f); // ~2400-sample attack, still rising across the block
    env->m_DecayTime.SetDefault(0.05f);
    auto* envPtr = env.get();
    const UUID envID = env->m_ID;
    Ref<sg::SoundGraph> graph = MakeEventRoutedGraph(std::move(env), envID, Identifier("Footstep"), sg::ADEnvelope::IDs::s_Trigger);

    sg::SoundGraphSource source;
    source.ReplaceGraph(graph);

    // Game code schedules a footstep at a mid-block sample offset (the external path).
    ASSERT_TRUE(source.SendInputEvent("Footstep", 1.0f, kOffset));

    // Drive one real audio block through the source; it drains the queue and fires the
    // trigger at its exact frame before SoundGraph::Process runs (device-free, same as
    // SoundGraphParameterWiringTest).
    std::array<f32, kBlock * kChannels> bus{};
    f32* busPtr = bus.data();
    source.ProcessSamples(&busPtr, kBlock);

    const f32* out = envPtr->m_OutEnvelope.Data();
    for (i32 f = 0; f < kOffset; ++f)
        EXPECT_FLOAT_EQ(out[static_cast<u32>(f)], 0.0f)
            << "envelope must be silent before the external trigger's frame " << f;
    EXPECT_GT(out[static_cast<u32>(kOffset)], 0.0f)
        << "attack must start at the external trigger's exact frame, not the block boundary";
    EXPECT_GT(out[static_cast<u32>(kOffset) + 50], out[static_cast<u32>(kOffset)])
        << "attack must be rising after the external trigger";
}

TEST_F(SoundGraphSampleAccurateTriggerTest, ExternalTriggerThroughSourceClampsOutOfRangeOffsetIntoTheBlock)
{
    constexpr u32 kChannels = 2;

    auto env = CreateScope<sg::ADEnvelope>("AD", UUID());
    env->m_AttackTime.SetDefault(0.05f);
    env->m_DecayTime.SetDefault(0.05f);
    auto* envPtr = env.get();
    const UUID envID = env->m_ID;
    Ref<sg::SoundGraph> graph = MakeEventRoutedGraph(std::move(env), envID, Identifier("Footstep"), sg::ADEnvelope::IDs::s_Trigger);

    sg::SoundGraphSource source;
    source.ReplaceGraph(graph);

    // An offset past the block end must clamp to the last frame and still fire this block,
    // not vanish — so only the final frame shows the attack onset.
    ASSERT_TRUE(source.SendInputEvent("Footstep", 1.0f, /*sampleOffset=*/100000));

    std::array<f32, kBlock * kChannels> bus{};
    f32* busPtr = bus.data();
    source.ProcessSamples(&busPtr, kBlock);

    const f32* out = envPtr->m_OutEnvelope.Data();
    for (u32 f = 0; f < kBlock - 1; ++f)
        EXPECT_FLOAT_EQ(out[f], 0.0f) << "clamped trigger must stay silent until the last frame, was at " << f;
    EXPECT_GT(out[kBlock - 1], 0.0f) << "an out-of-range offset must clamp to the block's last frame";
}

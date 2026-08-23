// OLO_TEST_LAYER: unit
//
// =============================================================================
// SoundGraphVoiceSuspendTest — voice-budget suspension of a SoundGraph voice
// (issue #745, follow-up to the concurrent-voice budget in #730)
//
// Before #745 a virtualized graph voice was only MUTED: the graph kept being
// stepped every block, so the budget bounded what you heard but not what the CPU
// did. `SoundGraphSource::SetVoiceSuspended` freezes the runtime instead — while
// it is set, ProcessSamples emits silence and does not step the graph at all.
//
// Every assertion below is written against the suspend behaviour and FAILS on
// the old muting behaviour, which is the point issue #745 makes explicitly: a
// phase test alone passes trivially on a graph that never stopped running, so it
// proves nothing on its own. The two halves are asserted together:
//
//   * CPU IS RECLAIMED — a counting node's Process() call count does not advance
//     across a suspension, and the graph's own frame counter is frozen with it.
//     (On the muting behaviour the count keeps climbing.)
//   * RESUME IS A CONTINUATION, NOT A RESTART — a suspended-and-resumed source
//     emits exactly the sample stream a continuously-running twin emits at the
//     same *processed-block* ordinal, and specifically NOT the graph's opening
//     block. That is #730 acceptance criterion 2, which the tempting
//     "stop the graph and re-raise SendPlayEvent" fix would break.
//
// What is deliberately NOT asserted, because it is unreachable rather than
// unimplemented: that a resumed voice lands at the position it would have
// reached had it kept running. For a procedural graph, advancing to frame N is
// exactly the DSP work of frames 0..N, so reclaiming the CPU and preserving that
// phase are the same computation. See docs/agent-rules/audio-voice-budget.md §8.
//
// All of it runs headless: no ma_engine, no audio device, no mounted asset.
// SoundGraphSource::ProcessSamples is driven directly (the pattern
// SoundGraphSampleAccurateTriggerTest established), and SoundGraphSound is built
// through InitializeDetachedSource, which allocates a source without attaching it
// to miniaudio.
// =============================================================================

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Asset/SoundGraphAsset.h"
#include "OloEngine/Audio/SoundGraph/GraphGeneration.h"
#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/SoundGraph.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphFactory.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphPrototype.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphSound.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphSource.h"
#include "OloEngine/Audio/VoiceManager.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Core/UUID.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace)
namespace sg = OloEngine::Audio::SoundGraph;

namespace
{
    constexpr u32 kBlock = 480;  // 10 ms at 48 kHz, a typical miniaudio block
    constexpr u32 kChannels = 2; // SoundGraphSource's default output channel count

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

    /// A 440 Hz sine wired straight to the graph's stereo output. An oscillator is
    /// the right probe here because its output is phase-bearing: a restart is
    /// visible as a return to the opening block, which a constant or a noise
    /// source could not distinguish.
    Ref<sg::SoundGraph> MakeSineGraph()
    {
        SoundGraphAsset asset;
        asset.SetName("SuspendProbeSine");

        SoundGraphNodeData sine;
        sine.m_ID = UUID();
        sine.m_Type = "SineOscillator";
        sine.m_Name = "Sine";
        sine.m_Properties["Frequency"] = "440";
        sine.m_Properties["Amplitude"] = "1.0";
        asset.AddNode(sine);

        AddConn(asset, sine.m_ID, "OutValue", UUID(0), "OutLeft", /*isEvent=*/false);
        AddConn(asset, sine.m_ID, "OutValue", UUID(0), "OutRight", /*isEvent=*/false);

        Ref<sg::Prototype> prototype = sg::CompileAssetToPrototype(asset);
        EXPECT_NE(prototype, nullptr) << "MakeSineGraph: asset failed to compile";
        Ref<sg::SoundGraph> instance = sg::CreateInstance(prototype);
        EXPECT_NE(instance, nullptr) << "MakeSineGraph: prototype failed to instantiate";
        return instance;
    }

    /// Counts Process() calls. This is the DSP-cost probe: "did the graph run?"
    /// is exactly the question #745 is about, and a call count answers it without
    /// depending on a wall-clock measurement that would flake under CI load.
    struct CountingNode final : public sg::NodeProcessor
    {
        explicit CountingNode(UUID id) : NodeProcessor("CountingNode", id)
        {
            // A bindable in-event, so a graph-input route can target this node. Without a
            // bound handler SoundGraph::SendInputEvent returns false and the source never
            // flips m_IsPlaying - see MakePlayRoutedCountingGraph.
            AddInEvent(Identifier("In"), [this](f32 value, i32 sampleOffset)
                       { (void)value; (void)sampleOffset; ++m_EventsReceived; });
        }

        u32 m_EventsReceived = 0;

        u32 m_ProcessCalls = 0;
        u64 m_FramesProcessed = 0;

        void Process(u32 numFrames) final
        {
            ++m_ProcessCalls;
            m_FramesProcessed += numFrames;
        }
    };

    /// Pull `blockCount` blocks through the source and return the interleaved
    /// samples of every block concatenated, so callers can compare streams.
    std::vector<f32> PumpBlocks(sg::SoundGraphSource& source, u32 blockCount)
    {
        std::vector<f32> captured;
        captured.reserve(static_cast<sizet>(blockCount) * kBlock * kChannels);

        std::vector<f32> bus(static_cast<sizet>(kBlock) * kChannels, 0.0f);
        for (u32 block = 0; block < blockCount; ++block)
        {
            std::ranges::fill(bus, 0.0f);
            f32* busPtr = bus.data();
            source.ProcessSamples(&busPtr, kBlock);
            captured.insert(captured.end(), bus.begin(), bus.end());
        }
        return captured;
    }

    f32 PeakAbs(const std::vector<f32>& samples)
    {
        f32 peak = 0.0f;
        for (const f32 sample : samples)
            peak = std::max(peak, std::abs(sample));
        return peak;
    }
} // namespace

class SoundGraphVoiceSuspendTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        Log::Initialize();
        // VoiceManager::Get() is process-wide, so leave it exactly as found — a leaked
        // voice or an altered cap would surface as an unrelated failure much later in
        // the run, in whichever audio test happens to go next.
        m_SavedMaxVoices = OloEngine::Audio::VoiceManager::Get().GetMaxVoices();
        OloEngine::Audio::VoiceManager::Get().Reset();
    }

    void TearDown() override
    {
        OloEngine::Audio::VoiceManager::Get().Reset();
        OloEngine::Audio::VoiceManager::Get().SetMaxVoices(m_SavedMaxVoices);
    }

  private:
    u32 m_SavedMaxVoices = 0;
};

// ===========================================================================
// SoundGraphSource — the transport itself
// ===========================================================================

TEST_F(SoundGraphVoiceSuspendTest, SuspendedSourceDoesNotStepTheGraph)
{
    auto counting = CreateScope<CountingNode>(UUID());
    auto* countingPtr = counting.get();

    auto graph = Ref<sg::SoundGraph>::Create("CountingGraph", UUID());
    graph->AddNode(std::move(counting));
    graph->SetSampleRate(48000.0f);
    graph->Init();

    sg::SoundGraphSource source;
    ASSERT_TRUE(source.ReplaceGraph(graph));
    EXPECT_FALSE(source.IsVoiceSuspended()) << "a fresh source must start running";

    PumpBlocks(source, 3);
    const u32 callsBeforeSuspend = countingPtr->m_ProcessCalls;
    ASSERT_EQ(callsBeforeSuspend, 3u) << "the graph must be stepped once per block while running";

    // The whole of issue #745 in one assertion: blocks pulled through a suspended
    // source cost nothing. On the pre-#745 muting behaviour this count would be 6.
    source.SetVoiceSuspended(true);
    EXPECT_TRUE(source.IsVoiceSuspended());
    PumpBlocks(source, 3);
    EXPECT_EQ(countingPtr->m_ProcessCalls, callsBeforeSuspend)
        << "a suspended voice must not step the graph — that is the DSP cost being reclaimed";

    source.SetVoiceSuspended(false);
    PumpBlocks(source, 2);
    EXPECT_EQ(countingPtr->m_ProcessCalls, callsBeforeSuspend + 2u)
        << "a thawed voice must be stepped again";
}

TEST_F(SoundGraphVoiceSuspendTest, SuspendedSourceEmitsSilenceAndHoldsItsFrameCounter)
{
    sg::SoundGraphSource source;
    ASSERT_TRUE(source.ReplaceGraph(MakeSineGraph()));

    const std::vector<f32> audible = PumpBlocks(source, 2);
    ASSERT_GT(PeakAbs(audible), 0.5f) << "the probe graph must produce signal while running";
    const u64 frameAtSuspend = source.GetCurrentFrame();
    ASSERT_EQ(frameAtSuspend, static_cast<u64>(kBlock) * 2u);

    source.SetVoiceSuspended(true);
    const std::vector<f32> suspended = PumpBlocks(source, 4);
    EXPECT_FLOAT_EQ(PeakAbs(suspended), 0.0f) << "a suspended voice must emit silence, not a stale block";

    // The frame counter is what OnVoiceQueryPosition reports, so freezing it is what
    // keeps the budget's record anchored to where the graph really is rather than to
    // a position it never computed.
    EXPECT_EQ(source.GetCurrentFrame(), frameAtSuspend)
        << "a frozen graph advanced no frames, and must not claim to have";
}

TEST_F(SoundGraphVoiceSuspendTest, ResumedSourceContinuesTheSampleStreamInsteadOfRestarting)
{
    // Two identical graphs. `continuous` runs six blocks straight through;
    // `suspended` runs three, sleeps through four, then runs three more. If the
    // suspension is a freeze, the second source's blocks 4-6 are bit-identical to
    // the first source's blocks 4-6 — same node state, same sample stream, merely
    // delayed in wall time.
    sg::SoundGraphSource continuous;
    ASSERT_TRUE(continuous.ReplaceGraph(MakeSineGraph()));
    sg::SoundGraphSource suspended;
    ASSERT_TRUE(suspended.ReplaceGraph(MakeSineGraph()));

    const std::vector<f32> continuousHead = PumpBlocks(continuous, 3);
    const std::vector<f32> continuousTail = PumpBlocks(continuous, 3);

    const std::vector<f32> suspendedHead = PumpBlocks(suspended, 3);
    ASSERT_EQ(suspendedHead, continuousHead) << "the two probe graphs must start identically";

    suspended.SetVoiceSuspended(true);
    PumpBlocks(suspended, 4);
    suspended.SetVoiceSuspended(false);
    const std::vector<f32> suspendedTail = PumpBlocks(suspended, 3);

    ASSERT_EQ(suspendedTail.size(), continuousTail.size());
    for (sizet i = 0; i < suspendedTail.size(); ++i)
    {
        ASSERT_FLOAT_EQ(suspendedTail[i], continuousTail[i])
            << "sample " << i << ": a resumed voice must continue the stream it was frozen on";
    }

    // ...and specifically it must NOT be the graph's opening block, which is what
    // "stop the graph and re-raise SendPlayEvent" would have produced — the
    // restart-instead-of-resume bug virtualization exists to prevent (#730 AC2).
    // 440 Hz at 48 kHz does not complete a whole number of cycles in 480 frames, so
    // block 4 and block 1 are genuinely different signal.
    const std::vector<f32> openingBlock(continuousHead.begin(),
                                        continuousHead.begin() + static_cast<sizet>(kBlock) * kChannels);
    const std::vector<f32> resumedBlock(suspendedTail.begin(),
                                        suspendedTail.begin() + static_cast<sizet>(kBlock) * kChannels);
    EXPECT_NE(resumedBlock, openingBlock) << "a resume must not replay the graph's initial state";
}

// ===========================================================================
// SoundGraphSound — the IVoiceHost seam the budget actually drives
// ===========================================================================

namespace
{
    /// A device-free SoundGraphSound wrapping the sine probe. InitializeDetachedSource
    /// allocates the source without attaching it to ma_engine, so the graph can be
    /// stepped by hand from the test thread.
    Scope<sg::SoundGraphSound> MakeDetachedSound()
    {
        auto sound = CreateScope<sg::SoundGraphSound>();
        EXPECT_TRUE(sound->InitializeDetachedSource());
        EXPECT_TRUE(sound->InitializeFromGraph(MakeSineGraph()));
        return sound;
    }
} // namespace

TEST_F(SoundGraphVoiceSuspendTest, VirtualizingAGraphVoiceSuspendsItsSourceAndDevirtualizingResumesIt)
{
    auto& manager = OloEngine::Audio::VoiceManager::Get();
    manager.SetMaxVoices(1);

    auto loud = MakeDetachedSound();
    auto quiet = MakeDetachedSound();
    // Priority is miniaudio-flavoured here (0 = highest), and BuildVoiceParams inverts it.
    // 200 rather than 255 deliberately: 255 normalises to a score of exactly 0, and a
    // zero-scoring voice is a separate policy case from "loses on merit".
    loud->SetPriority(0);
    quiet->SetPriority(200);

    ASSERT_TRUE(quiet->Play());
    ASSERT_TRUE(quiet->GetSource() != nullptr);
    EXPECT_FALSE(quiet->GetSource()->IsVoiceSuspended())
        << "the only voice in a one-slot budget must be running";

    // The higher-priority voice takes the slot; the loser is virtualized, and after
    // #745 that means its graph is actually frozen rather than merely muted.
    ASSERT_TRUE(loud->Play());
    ASSERT_TRUE(quiet->IsVirtualized());
    EXPECT_TRUE(quiet->GetSource()->IsVoiceSuspended())
        << "a virtualized graph voice must be suspended, not just muted (issue #745)";
    EXPECT_FALSE(loud->GetSource()->IsVoiceSuspended()) << "the winning voice must be running";

    // Give the slot back and the loser must thaw.
    ASSERT_TRUE(loud->Stop());
    manager.Update(0.016f);
    ASSERT_FALSE(quiet->IsVirtualized());
    EXPECT_FALSE(quiet->GetSource()->IsVoiceSuspended())
        << "a devirtualized graph voice must be thawed again";
    // A stopped voice stays frozen: it is silent, and now it is free as well.
    EXPECT_TRUE(loud->GetSource()->IsVoiceSuspended())
        << "a stopped graph voice must not keep paying DSP cost";
}

TEST_F(SoundGraphVoiceSuspendTest, AGraphVoiceThatNeverWinsASlotStartsSuspended)
{
    // Acquire only emits transitions for state CHANGES and every voice ENTERS virtual,
    // so a voice that starts over budget is never handed to OnVoiceStop. Before #745
    // nothing else applied the virtualized state on that path and the voice played at
    // full gain, over the cap.
    auto& manager = OloEngine::Audio::VoiceManager::Get();
    manager.SetMaxVoices(1);

    auto incumbent = MakeDetachedSound();
    incumbent->SetPriority(0);
    ASSERT_TRUE(incumbent->Play());

    auto latecomer = MakeDetachedSound();
    latecomer->SetPriority(200);
    ASSERT_TRUE(latecomer->Play());

    ASSERT_TRUE(latecomer->IsVirtualized());
    EXPECT_TRUE(latecomer->GetSource()->IsVoiceSuspended())
        << "a voice that loses the budget at Play() time must not run at all";
}

TEST_F(SoundGraphVoiceSuspendTest, AVoiceSuspendedByTheBudgetProducesNoSamples)
{
    // The seam test above checks the flag; this one checks the flag is load-bearing,
    // by pumping real blocks through the virtualized voice's source and asserting the
    // graph never ran. That is the assertion the pre-#745 mute could not pass.
    auto& manager = OloEngine::Audio::VoiceManager::Get();
    manager.SetMaxVoices(1);

    auto incumbent = MakeDetachedSound();
    incumbent->SetPriority(0);
    ASSERT_TRUE(incumbent->Play());

    auto stolen = MakeDetachedSound();
    stolen->SetPriority(200);
    ASSERT_TRUE(stolen->Play());
    ASSERT_TRUE(stolen->IsVirtualized());

    const std::vector<f32> whileVirtual = PumpBlocks(*stolen->GetSource(), 4);
    EXPECT_FLOAT_EQ(PeakAbs(whileVirtual), 0.0f);
    EXPECT_EQ(stolen->GetSource()->GetCurrentFrame(), 0u)
        << "a virtualized graph voice must not have advanced a single frame";

    // And once it wins the slot back it produces signal from frame 0 — its latched
    // Play request fires on the first block after the thaw.
    ASSERT_TRUE(incumbent->Stop());
    manager.Update(0.016f);
    ASSERT_FALSE(stolen->IsVirtualized());

    const std::vector<f32> afterThaw = PumpBlocks(*stolen->GetSource(), 2);
    EXPECT_GT(PeakAbs(afterThaw), 0.5f) << "a thawed voice must produce audio again";
    EXPECT_EQ(stolen->GetSource()->GetCurrentFrame(), static_cast<u64>(kBlock) * 2u);
}

namespace
{
    /// A graph whose `Play` input event is actually ROUTED. This matters: `SoundGraph`'s
    /// constructor adds the `Play` endpoint but binds no handler (`InitializeEndpoints()`
    /// has no call sites anywhere in the engine), so on an unrouted graph
    /// `SendInputEvent(IDs::Play)` returns false and `SoundGraphSource::m_IsPlaying` never
    /// becomes true - which makes the natural-completion path unreachable. A real authored
    /// one-shot routes Play to its WavePlayer, so it does reach it.
    Ref<sg::SoundGraph> MakePlayRoutedCountingGraph(CountingNode** outNode)
    {
        auto counting = CreateScope<CountingNode>(UUID());
        *outNode = counting.get();
        const UUID countingID = counting->m_ID;

        auto graph = Ref<sg::SoundGraph>::Create("PlayRoutedCounting", UUID());
        graph->AddNode(std::move(counting));
        EXPECT_TRUE(graph->AddInputEventsRoute(sg::SoundGraph::IDs::Play, countingID, Identifier("In")));
        graph->SetSampleRate(48000.0f);
        graph->Init();
        return graph;
    }
} // namespace

TEST_F(SoundGraphVoiceSuspendTest, AGraphSoundCanBeReplayedAfterItRunsToItsEnd)
{
    // Natural completion parks the source on the OTHER suspension axis:
    // SoundGraphSource::Update calls SuspendProcessing(true) once the graph reports
    // finished. Nothing but a graph swap ever cleared that, so a sound that ran to its end
    // could never be played again - it stayed in the graph-swap silence path forever while
    // the wrapper cheerfully reported it playing. Play is the one point where resetting the
    // source's playback counters is correct, because SendPlayEvent restarts the stream.
    CountingNode* counting = nullptr;
    auto sound = CreateScope<sg::SoundGraphSound>();
    ASSERT_TRUE(sound->InitializeDetachedSource());
    ASSERT_TRUE(sound->InitializeFromGraph(MakePlayRoutedCountingGraph(&counting)));
    auto* source = sound->GetSource();
    ASSERT_NE(source, nullptr);

    ASSERT_TRUE(sound->Play());
    PumpBlocks(*source, 1); // fires the latched Play request; this graph has no wave
                            // sources, so AreAllDataSourcesAtEnd() reports finished at once
    ASSERT_EQ(counting->m_EventsReceived, 1u) << "the Play route must actually be bound";

    sound->Update(0.016f);  // source Update -> SuspendProcessing(true)
    PumpBlocks(*source, 1); // audio thread acks the suspend
    sound->Update(0.016f);  // wrapper picks up the finish and retires the voice
    ASSERT_TRUE(sound->IsFinished()) << "the probe must actually reach natural completion";

    const u32 callsAtCompletion = counting->m_ProcessCalls;
    ASSERT_TRUE(sound->Play());
    PumpBlocks(*source, 2);
    EXPECT_GT(counting->m_ProcessCalls, callsAtCompletion)
        << "a graph sound must be steppable again after it ended";
    EXPECT_EQ(counting->m_EventsReceived, 2u)
        << "the replay's Play trigger must reach the graph";
}

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// OLO_TEST_LAYER: unit
//
// =============================================================================
// AudioSoundGraphSpatializerTest — SoundGraph voices route through the 3D spatializer
//
// Issue #424: a SoundGraphSource attaches a *bare* ma_node straight to the engine
// endpoint, but Audio::DSP::Spatializer::InitSource was built around an ma_engine_node
// (it reads `resampler.channels` and writes the engine node's built-in
// `spatializer.dopplerPitch`). This pins the new bare-node path end to end:
//
//   * SoundGraphSource::RegisterSpatializer inserts a per-voice spatializer node
//     between its SoundGraph node and the endpoint (the crux — a bare ma_node, not an
//     ma_engine_node), assigning a unique sourceID;
//   * UpdateSpatialPosition drives Spatializer::UpdateSourcePosition so the source's
//     distance / distance-attenuation track its world position relative to the listener;
//   * UnregisterSpatializer / Shutdown release the source cleanly (re-routing the
//     SoundGraph node straight back to the endpoint).
//
// Everything runs against a device-free ma_engine (config.noDevice = MA_TRUE): the node
// graph + resource manager exist (so node insertion + the VBAP / distance math run), but
// no audio hardware is opened — so this gates a normal headless CI run. The assertions
// use the spatializer's CPU-side getters (GetCurrentDistance / GetCurrentDistanceAttenuation
// / GetCurrentConeAngleAttenuation), which are pure functions of the geometry.
// =============================================================================

#include "OloEngine/Audio/AudioSource.h"
#include "OloEngine/Audio/AudioTransform.h"
#include "OloEngine/Audio/DSP/Spatializer/Spatializer.h"
#include "OloEngine/Audio/SoundGraph/SoundGraphSource.h"

#include <glm/glm.hpp>
#include <miniaudio.h>

#include <cmath>

using namespace OloEngine; // NOLINT(google-build-using-namespace)
namespace sg = OloEngine::Audio::SoundGraph;
namespace dsp = OloEngine::Audio::DSP;

namespace
{
    constexpr u32 kSampleRate = 48000;
    constexpr u32 kBlockSize = 1024;
    constexpr u32 kChannels = 2;

    // Brings up a device-free ma_engine + a Spatializer + a real SoundGraphSource whose node
    // is attached to the engine endpoint — i.e. exactly the topology a SoundGraph voice has,
    // minus the audio device. Skips cleanly if the engine cannot be created (it has no device
    // dependency, so this should always succeed, but be defensive like the GL visual tests).
    class SoundGraphSpatializerTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            ma_engine_config config = ::ma_engine_config_init();
            config.noDevice = MA_TRUE;   // offline / manual processing — no hardware opened
            config.channels = kChannels; // required when noDevice (nothing to infer from)
            config.sampleRate = kSampleRate;
            config.listenerCount = 1;

            if (::ma_engine_init(&config, &m_Engine) != MA_SUCCESS)
            {
                GTEST_SKIP() << "Device-free ma_engine could not be created on this host";
            }
            m_EngineReady = true;

            ASSERT_TRUE(m_Spatializer.Initialize(&m_Engine));
            ASSERT_TRUE(m_Source.Initialize(&m_Engine, kSampleRate, kBlockSize, kChannels));
        }

        void TearDown() override
        {
            if (m_EngineReady)
            {
                // Source first: Shutdown unregisters its spatializer node (re-routing the
                // SoundGraph node back to the endpoint), then the spatializer, then the engine.
                m_Source.Shutdown();
                m_Spatializer.Uninitialize();
                ::ma_engine_uninit(&m_Engine);
            }
        }

        ma_engine m_Engine{};
        bool m_EngineReady = false;
        dsp::Spatializer m_Spatializer;
        sg::SoundGraphSource m_Source;
    };
} // namespace

// The bare SoundGraph node can host a spatializer node and gets a valid, unique sourceID.
TEST_F(SoundGraphSpatializerTest, RegisterInsertsSpatializerNodeAndAssignsSourceID)
{
    AudioSourceConfig config;
    config.Spatialization = true;

    EXPECT_FALSE(m_Source.IsSpatialized());
    ASSERT_TRUE(m_Source.RegisterSpatializer(&m_Spatializer, config))
        << "InitSource must accept a bare ma_node (SoundGraphSource), not just an ma_engine_node";

    EXPECT_TRUE(m_Source.IsSpatialized());
    const u32 id = m_Source.GetSpatializerSourceID();
    EXPECT_NE(id, 0u) << "a registered voice gets a non-zero (allocated) sourceID";
    EXPECT_TRUE(m_Spatializer.IsInitialized(id));

    // Idempotent: a second register while already registered is a success no-op (same ID).
    EXPECT_TRUE(m_Source.RegisterSpatializer(&m_Spatializer, config));
    EXPECT_EQ(m_Source.GetSpatializerSourceID(), id);
}

// Distance reported by the spatializer matches the geometric distance to the listener, and
// distance-attenuation is monotonic: a nearer source is louder than a farther one.
TEST_F(SoundGraphSpatializerTest, PositionDrivesDistanceAndAttenuation)
{
    AudioSourceConfig config;
    config.Spatialization = true; // default Inverse model, MinDistance 0.3, MaxDistance 1000
    ASSERT_TRUE(m_Source.RegisterSpatializer(&m_Spatializer, config));
    const u32 id = m_Source.GetSpatializerSourceID();

    // Listener at the origin, looking down -Z (Audio::Transform defaults).
    m_Spatializer.UpdateListener(Audio::Transform{});

    // A source 2 units directly in front of the listener.
    Audio::Transform nearPose;
    nearPose.Position = { 0.0f, 0.0f, -2.0f };
    m_Source.UpdateSpatialPosition(nearPose);
    EXPECT_NEAR(m_Spatializer.GetCurrentDistance(id), 2.0f, 1e-3f);
    const float nearAtten = m_Spatializer.GetCurrentDistanceAttenuation(id);

    // The same source moved far away.
    Audio::Transform farPose;
    farPose.Position = { 0.0f, 0.0f, -50.0f };
    m_Source.UpdateSpatialPosition(farPose);
    EXPECT_NEAR(m_Spatializer.GetCurrentDistance(id), 50.0f, 1e-3f);
    const float farAtten = m_Spatializer.GetCurrentDistanceAttenuation(id);

    EXPECT_GT(nearAtten, farAtten) << "a nearer source must attenuate less (be louder)";
    EXPECT_GT(nearAtten, 0.0f);
    EXPECT_LE(nearAtten, 1.0f);
    EXPECT_GT(farAtten, 0.0f);

    // With the default omnidirectional (360°) cone, there is no angular attenuation: the cone
    // factor is neutral (the spatializer folds any cone gain into the distance factor).
    EXPECT_NEAR(m_Spatializer.GetCurrentConeAngleAttenuation(id), 1.0f, 1e-4f);
}

// A source at (clamped to) the listener position is at full gain; moving it out attenuates.
TEST_F(SoundGraphSpatializerTest, AtListenerIsFullGain)
{
    AudioSourceConfig config;
    config.Spatialization = true;
    ASSERT_TRUE(m_Source.RegisterSpatializer(&m_Spatializer, config));
    const u32 id = m_Source.GetSpatializerSourceID();

    m_Spatializer.UpdateListener(Audio::Transform{});

    // Co-located with the listener: distance clamps to MinDistance, so attenuation is ~1.0.
    Audio::Transform atListener; // position {0,0,0}
    m_Source.UpdateSpatialPosition(atListener);
    EXPECT_NEAR(m_Spatializer.GetCurrentDistanceAttenuation(id), 1.0f, 1e-3f);
}

// Moving the listener (not the source) also recomputes the source's relative distance —
// proving the listener-update path feeds the same math.
TEST_F(SoundGraphSpatializerTest, MovingListenerRecomputesDistance)
{
    AudioSourceConfig config;
    config.Spatialization = true;
    ASSERT_TRUE(m_Source.RegisterSpatializer(&m_Spatializer, config));
    const u32 id = m_Source.GetSpatializerSourceID();

    m_Spatializer.UpdateListener(Audio::Transform{});

    Audio::Transform sourcePose;
    sourcePose.Position = { 0.0f, 0.0f, -10.0f };
    m_Source.UpdateSpatialPosition(sourcePose);
    EXPECT_NEAR(m_Spatializer.GetCurrentDistance(id), 10.0f, 1e-3f);

    // Move the listener 6 units toward the source (down -Z): distance should drop to ~4.
    Audio::Transform listenerPose;
    listenerPose.Position = { 0.0f, 0.0f, -6.0f };
    m_Spatializer.UpdateListener(listenerPose);
    EXPECT_NEAR(m_Spatializer.GetCurrentDistance(id), 4.0f, 1e-3f);
}

// Unregistering releases the spatializer source and re-routes the node — the sourceID is
// freed and IsSpatialized goes false. (Shutdown does the same; this exercises the explicit path.)
TEST_F(SoundGraphSpatializerTest, UnregisterReleasesSource)
{
    AudioSourceConfig config;
    config.Spatialization = true;
    ASSERT_TRUE(m_Source.RegisterSpatializer(&m_Spatializer, config));
    const u32 id = m_Source.GetSpatializerSourceID();
    ASSERT_TRUE(m_Spatializer.IsInitialized(id));

    m_Source.UnregisterSpatializer();

    EXPECT_FALSE(m_Source.IsSpatialized());
    EXPECT_EQ(m_Source.GetSpatializerSourceID(), 0u);
    EXPECT_FALSE(m_Spatializer.IsInitialized(id)) << "the released sourceID is gone from the map";

    // Re-registering after release works and yields a fresh sourceID — distinct from the
    // one we just released (id), so the allocator can't silently recycle it.
    ASSERT_TRUE(m_Source.RegisterSpatializer(&m_Spatializer, config));
    const u32 reusedID = m_Source.GetSpatializerSourceID();
    EXPECT_NE(reusedID, 0u);
    EXPECT_NE(reusedID, id) << "the re-registered source must not reuse the released sourceID";
}

// Regression (primary): the Inverse distance-attenuation model is minDistance /
// (minDistance + rolloff*(clamp(d, min, max) - minDistance)). With MinDistance == 0 (a legal
// config value — the scene deserializer sanitizer permits [0, 1e6]) and a source co-located
// with the listener (distance ~ 0), that is 0 / 0 == NaN. std::clamp(NaN, MinGain, MaxGain)
// propagates the NaN, which flows through VBAP and AddAndApplyGainRamp straight into the master
// mix (all audio corrupted, potential speaker damage). The Inverse case must guard MinDistance
// <= 0 exactly like the Exponential case already does.
TEST_F(SoundGraphSpatializerTest, InverseModelZeroMinDistanceCoLocatedStaysFinite)
{
    AudioSourceConfig config;
    config.Spatialization = true;
    config.AttenuationModel = AttenuationModelType::Inverse; // the default, spelled out for clarity
    config.MinDistance = 0.0f;                               // the trigger
    config.MaxDistance = 1000.0f;
    ASSERT_TRUE(m_Source.RegisterSpatializer(&m_Spatializer, config));
    const u32 id = m_Source.GetSpatializerSourceID();

    m_Spatializer.UpdateListener(Audio::Transform{});

    // Source sitting exactly on the listener → distance ~ 0 → the 0/0 case.
    Audio::Transform atListener; // position {0,0,0}
    m_Source.UpdateSpatialPosition(atListener);

    const float atten = m_Spatializer.GetCurrentDistanceAttenuation(id);
    EXPECT_TRUE(std::isfinite(atten)) << "distance attenuation must never be NaN/Inf (got " << atten << ")";
    EXPECT_GE(atten, 0.0f);
    EXPECT_LE(atten, 1.0f);
}

// The same MinDistance == 0 Inverse config with the source at a real distance. Before the guard
// this returned 0 / (rolloff*distance) == 0 for every distance > 0 — a permanently silent voice
// (the degenerate all-silent behaviour). The guard now returns full gain, matching the
// Exponential case's minDistance<=0 short-circuit.
TEST_F(SoundGraphSpatializerTest, InverseModelZeroMinDistanceDistantStaysAudible)
{
    AudioSourceConfig config;
    config.Spatialization = true;
    config.AttenuationModel = AttenuationModelType::Inverse;
    config.MinDistance = 0.0f;
    config.MaxDistance = 1000.0f;
    ASSERT_TRUE(m_Source.RegisterSpatializer(&m_Spatializer, config));
    const u32 id = m_Source.GetSpatializerSourceID();

    m_Spatializer.UpdateListener(Audio::Transform{});

    Audio::Transform pose;
    pose.Position = { 0.0f, 0.0f, -5.0f };
    m_Source.UpdateSpatialPosition(pose);

    const float atten = m_Spatializer.GetCurrentDistanceAttenuation(id);
    EXPECT_TRUE(std::isfinite(atten));
    EXPECT_GT(atten, 0.0f) << "MinDistance == 0 must not silence the voice at distance > 0";
    EXPECT_LE(atten, 1.0f);
}

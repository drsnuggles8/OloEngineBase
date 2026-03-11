#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Scene/Streaming/StreamingSettings.h"
#include "OloEngine/Scene/Streaming/StreamingRegion.h"
#include "OloEngine/Scene/Streaming/StreamingVolumeComponent.h"
#include "OloEngine/Scene/Streaming/StreamingRegionSerializer.h"
#include "OloEngine/Scene/Streaming/SceneStreamer.h"

#include <glm/glm.hpp>

using namespace OloEngine;

// ============================================================
// StreamingSettings Tests
// ============================================================

TEST(StreamingSettings, DefaultValues)
{
    StreamingSettings ss;

    EXPECT_FALSE(ss.Enabled);
    EXPECT_GT(ss.DefaultLoadRadius, 0.0f);
    EXPECT_GT(ss.DefaultUnloadRadius, ss.DefaultLoadRadius);
    EXPECT_GT(ss.MaxLoadedRegions, 0u);
    EXPECT_TRUE(ss.RegionDirectory.empty());
}

TEST(StreamingSettings, HysteresisGuarantee)
{
    StreamingSettings ss;
    EXPECT_GT(ss.DefaultUnloadRadius, ss.DefaultLoadRadius)
        << "Unload radius must exceed load radius for hysteresis";
}

// ============================================================
// StreamingVolumeComponent Tests
// ============================================================

TEST(StreamingVolumeComponent, DefaultValues)
{
    StreamingVolumeComponent svc;

    EXPECT_EQ(svc.RegionAssetHandle, 0u);
    EXPECT_EQ(svc.ActivationMode, StreamingActivationMode::Proximity);
    EXPECT_GT(svc.LoadRadius, 0.0f);
    EXPECT_GT(svc.UnloadRadius, svc.LoadRadius);
    EXPECT_FALSE(svc.IsLoaded);
}

TEST(StreamingVolumeComponent, ManualMode)
{
    StreamingVolumeComponent svc;
    svc.ActivationMode = StreamingActivationMode::Manual;
    EXPECT_EQ(svc.ActivationMode, StreamingActivationMode::Manual);
}

// ============================================================
// StreamingRegion Tests
// ============================================================

TEST(StreamingRegion, DefaultState)
{
    StreamingRegion region;
    EXPECT_EQ(region.m_State, StreamingRegion::State::Unloaded);
    EXPECT_TRUE(region.m_EntityUUIDs.empty());
    EXPECT_TRUE(region.m_Name.empty());
}

TEST(StreamingRegion, StateTransitions)
{
    StreamingRegion region;

    region.m_State = StreamingRegion::State::Loading;
    EXPECT_EQ(region.m_State, StreamingRegion::State::Loading);

    region.m_State = StreamingRegion::State::Loaded;
    EXPECT_EQ(region.m_State, StreamingRegion::State::Loaded);

    region.m_State = StreamingRegion::State::Ready;
    EXPECT_EQ(region.m_State, StreamingRegion::State::Ready);

    region.m_State = StreamingRegion::State::Unloading;
    EXPECT_EQ(region.m_State, StreamingRegion::State::Unloading);

    region.m_State = StreamingRegion::State::Unloaded;
    EXPECT_EQ(region.m_State, StreamingRegion::State::Unloaded);
}

TEST(StreamingRegion, BoundsStorage)
{
    StreamingRegion region;
    region.m_BoundsMin = glm::vec3(-100.0f, 0.0f, -100.0f);
    region.m_BoundsMax = glm::vec3(100.0f, 50.0f, 100.0f);

    EXPECT_FLOAT_EQ(region.m_BoundsMin.x, -100.0f);
    EXPECT_FLOAT_EQ(region.m_BoundsMax.x, 100.0f);
    EXPECT_FLOAT_EQ(region.m_BoundsMax.y, 50.0f);
}

TEST(StreamingRegion, LRUFrameTracking)
{
    StreamingRegion region;
    EXPECT_EQ(region.m_LastUsedFrame, 0u);

    region.m_LastUsedFrame = 42;
    EXPECT_EQ(region.m_LastUsedFrame, 42u);
}

TEST(StreamingRegion, EntityUUIDs)
{
    StreamingRegion region;
    region.m_EntityUUIDs.emplace_back(UUID(123));
    region.m_EntityUUIDs.emplace_back(UUID(456));

    EXPECT_EQ(region.m_EntityUUIDs.size(), 2u);
}

// ============================================================
// SceneStreamerConfig Tests
// ============================================================

TEST(SceneStreamerConfig, DefaultValues)
{
    SceneStreamerConfig cfg;

    EXPECT_GT(cfg.LoadRadius, 0.0f);
    EXPECT_GT(cfg.UnloadRadius, cfg.LoadRadius);
    EXPECT_GT(cfg.MaxLoadedRegions, 0u);
    EXPECT_TRUE(cfg.RegionDirectory.empty());
}

TEST(SceneStreamerConfig, CustomValues)
{
    SceneStreamerConfig cfg;
    cfg.LoadRadius = 500.0f;
    cfg.UnloadRadius = 600.0f;
    cfg.MaxLoadedRegions = 32;
    cfg.RegionDirectory = "regions/";

    EXPECT_FLOAT_EQ(cfg.LoadRadius, 500.0f);
    EXPECT_FLOAT_EQ(cfg.UnloadRadius, 600.0f);
    EXPECT_EQ(cfg.MaxLoadedRegions, 32u);
    EXPECT_EQ(cfg.RegionDirectory, "regions/");
}

// ============================================================
// StreamingRegionSerializer Tests
// ============================================================

TEST(StreamingRegionSerializer, ParseInvalidYAML)
{
    auto result = StreamingRegionSerializer::ParseRegionFile("nonexistent_path.oloregion");
    EXPECT_FALSE(result["Region"]);
}

TEST(RegionMetadata, DefaultValues)
{
    StreamingRegionSerializer::RegionMetadata meta;
    EXPECT_EQ(meta.RegionID, UUID(0));
    EXPECT_TRUE(meta.Name.empty());
}

// ============================================================
// SceneStreamer Tests (no scene context)
// ============================================================

TEST(SceneStreamer, DefaultConstruction)
{
    SceneStreamer streamer;
    EXPECT_EQ(streamer.GetLoadedRegionCount(), 0u);
    EXPECT_EQ(streamer.GetPendingLoadCount(), 0u);
}

TEST(SceneStreamer, InitWithoutScene)
{
    SceneStreamer streamer;
    SceneStreamerConfig cfg;
    cfg.RegionDirectory = "nonexistent/";
    // Should not crash when initialized with null scene
    streamer.Initialize(nullptr, cfg);
    streamer.Shutdown();
}

TEST(SceneStreamer, ConfigAccessors)
{
    SceneStreamer streamer;
    auto& cfg = streamer.GetConfig();
    cfg.LoadRadius = 300.0f;
    EXPECT_FLOAT_EQ(streamer.GetConfig().LoadRadius, 300.0f);

    const auto& constRef = static_cast<const SceneStreamer&>(streamer);
    EXPECT_FLOAT_EQ(constRef.GetConfig().LoadRadius, 300.0f);
}

// ============================================================
// Activation Mode Enum Tests
// ============================================================

TEST(StreamingActivationMode, EnumValues)
{
    EXPECT_EQ(static_cast<u8>(StreamingActivationMode::Proximity), 0u);
    EXPECT_EQ(static_cast<u8>(StreamingActivationMode::Manual), 1u);
}

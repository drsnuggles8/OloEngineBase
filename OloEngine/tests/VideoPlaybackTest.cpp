#include <gtest/gtest.h>

#include "OloEngine/Video/VideoDecoder.h"
#include "OloEngine/Video/VideoPlayer.h"
#include "OloEngine/Video/VideoTexture.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

using namespace OloEngine;

// These tests run in CI with no GL context and no video fixture: they cover the
// decoder failure paths, the VideoPlayer transport state machine + input clamping,
// and the VideoTexture no-op-until-initialized contract — none of which touch GL.
//
// A real decode is exercised only when a fixture is supplied via the OLO_TEST_VIDEO
// environment variable (or one of the probed default paths); otherwise that test
// GTEST_SKIPs cleanly, mirroring the engine's "skip when no GL context" pattern.

namespace
{
    // Resolve an MPEG-1 fixture path, or "" if none is available.
    std::string FindTestVideo()
    {
        if (const char* env = std::getenv("OLO_TEST_VIDEO"))
        {
            if (std::filesystem::exists(env))
                return env;
        }
        const char* candidates[] = {
            "OloEditor/assets/tests/video/test.mpg",
            "OloEditor/SandboxProject/Assets/videos/test.mpg",
        };
        for (const char* c : candidates)
        {
            if (std::filesystem::exists(c))
                return c;
        }
        return {};
    }
} // namespace

//==============================================================================
// VideoDecoder — failure paths (no fixture / no GL required)
//==============================================================================

TEST(VideoDecoder, DefaultIsClosed)
{
    VideoDecoder decoder;
    EXPECT_FALSE(decoder.IsOpen());
    EXPECT_EQ(decoder.GetWidth(), 0u);
    EXPECT_EQ(decoder.GetHeight(), 0u);
    EXPECT_FALSE(decoder.IsEndOfStream());
}

TEST(VideoDecoder, OpenEmptyPathFails)
{
    VideoDecoder decoder;
    EXPECT_FALSE(decoder.Open(""));
    EXPECT_FALSE(decoder.IsOpen());
}

TEST(VideoDecoder, OpenMissingFileFails)
{
    VideoDecoder decoder;
    EXPECT_FALSE(decoder.Open("this/file/does/not/exist.mpg"));
    EXPECT_FALSE(decoder.IsOpen());
}

TEST(VideoDecoder, DecodeWithoutOpenReturnsFalse)
{
    VideoDecoder decoder;
    std::vector<u8> rgba;
    VideoFrameInfo info;
    EXPECT_FALSE(decoder.DecodeNextFrame(rgba, info));
    EXPECT_FALSE(decoder.Seek(1.0));
}

//==============================================================================
// VideoFrameInfo — default contract
//==============================================================================

TEST(VideoFrameInfo, DefaultsAreZero)
{
    VideoFrameInfo info;
    EXPECT_EQ(info.Width, 0u);
    EXPECT_EQ(info.Height, 0u);
    EXPECT_DOUBLE_EQ(info.Timestamp, 0.0);
    EXPECT_DOUBLE_EQ(info.Duration, 0.0);
}

//==============================================================================
// VideoPlayer — transport state machine (no media required)
//==============================================================================

TEST(VideoPlayer, DefaultState)
{
    auto player = Ref<VideoPlayer>::Create();
    EXPECT_FALSE(player->IsLoaded());
    EXPECT_TRUE(player->IsStopped());
    EXPECT_FALSE(player->IsPlaying());
    EXPECT_FALSE(player->IsPaused());
    EXPECT_FALSE(player->IsFinished());
    EXPECT_EQ(player->GetState(), VideoPlaybackState::Stopped);
    EXPECT_DOUBLE_EQ(player->GetCurrentTime(), 0.0);
    EXPECT_FLOAT_EQ(player->GetPlaybackSpeed(), 1.0f);
    EXPECT_FLOAT_EQ(player->GetVolume(), 1.0f);
    EXPECT_FALSE(player->IsLooping());
}

TEST(VideoPlayer, LoadMissingFileFails)
{
    auto player = Ref<VideoPlayer>::Create();
    EXPECT_FALSE(player->Load("nonexistent.mpg"));
    EXPECT_FALSE(player->IsLoaded());
}

TEST(VideoPlayer, PlayPauseStopTransitions)
{
    auto player = Ref<VideoPlayer>::Create();

    player->Play();
    EXPECT_TRUE(player->IsPlaying());

    player->Pause();
    EXPECT_TRUE(player->IsPaused());
    EXPECT_FALSE(player->IsPlaying());

    // Pause only acts from Playing — a second Pause is a no-op.
    player->Pause();
    EXPECT_TRUE(player->IsPaused());

    player->Play();
    EXPECT_TRUE(player->IsPlaying());

    player->Stop();
    EXPECT_TRUE(player->IsStopped());
    EXPECT_FALSE(player->IsPlaying());

    // Pause from Stopped does not move to Paused.
    player->Pause();
    EXPECT_TRUE(player->IsStopped());
}

TEST(VideoPlayer, SeekClampsToNonNegative)
{
    auto player = Ref<VideoPlayer>::Create();

    player->Seek(5.0);
    EXPECT_DOUBLE_EQ(player->GetCurrentTime(), 5.0); // No duration known -> no upper clamp.

    player->Seek(-3.0);
    EXPECT_DOUBLE_EQ(player->GetCurrentTime(), 0.0);

    player->Seek(std::numeric_limits<f64>::quiet_NaN());
    EXPECT_DOUBLE_EQ(player->GetCurrentTime(), 0.0);
}

TEST(VideoPlayer, PlaybackSpeedClamping)
{
    auto player = Ref<VideoPlayer>::Create();

    player->SetPlaybackSpeed(2.0f);
    EXPECT_FLOAT_EQ(player->GetPlaybackSpeed(), 2.0f);

    player->SetPlaybackSpeed(0.0f); // non-positive -> reset to 1.0
    EXPECT_FLOAT_EQ(player->GetPlaybackSpeed(), 1.0f);

    player->SetPlaybackSpeed(-4.0f); // negative -> reset to 1.0
    EXPECT_FLOAT_EQ(player->GetPlaybackSpeed(), 1.0f);

    player->SetPlaybackSpeed(1000.0f); // clamped to upper bound
    EXPECT_FLOAT_EQ(player->GetPlaybackSpeed(), 8.0f);

    player->SetPlaybackSpeed(std::numeric_limits<f32>::quiet_NaN());
    EXPECT_FLOAT_EQ(player->GetPlaybackSpeed(), 1.0f);
}

TEST(VideoPlayer, VolumeClamping)
{
    auto player = Ref<VideoPlayer>::Create();

    player->SetVolume(0.5f);
    EXPECT_FLOAT_EQ(player->GetVolume(), 0.5f);

    player->SetVolume(-1.0f);
    EXPECT_FLOAT_EQ(player->GetVolume(), 0.0f);

    player->SetVolume(3.0f);
    EXPECT_FLOAT_EQ(player->GetVolume(), 1.0f);

    player->SetVolume(std::numeric_limits<f32>::quiet_NaN());
    EXPECT_FLOAT_EQ(player->GetVolume(), 1.0f);
}

TEST(VideoPlayer, LoopingToggles)
{
    auto player = Ref<VideoPlayer>::Create();
    EXPECT_FALSE(player->IsLooping());
    player->SetLooping(true);
    EXPECT_TRUE(player->IsLooping());
    player->SetLooping(false);
    EXPECT_FALSE(player->IsLooping());
}

TEST(VideoPlayer, UpdateOnUnloadedPlayerIsInert)
{
    auto player = Ref<VideoPlayer>::Create();
    player->Play();               // Playing, but nothing loaded.
    player->Update(1.0f / 60.0f); // Should not advance the clock or crash.
    EXPECT_DOUBLE_EQ(player->GetCurrentTime(), 0.0);
    EXPECT_FALSE(player->IsFinished());
}

//==============================================================================
// VideoTexture — no-op until initialized (no GL context required)
//==============================================================================

TEST(VideoTexture, UninitializedContract)
{
    VideoTexture tex;
    EXPECT_FALSE(tex.IsInitialized());
    EXPECT_EQ(tex.GetRendererID(), 0u);
    EXPECT_EQ(tex.GetWidth(), 0u);
    EXPECT_EQ(tex.GetHeight(), 0u);

    // None of these should touch GL or crash when uninitialized.
    EXPECT_NO_FATAL_FAILURE(tex.Bind(0));
    EXPECT_NO_FATAL_FAILURE(tex.UpdateFrame(nullptr, 0, 0));
    EXPECT_NO_FATAL_FAILURE(tex.Destroy());
    EXPECT_FALSE(tex.IsInitialized());
}

//==============================================================================
// Real decode — only when an MPEG-1 fixture is available
//==============================================================================

TEST(VideoDecoderFixture, DecodesFrameWhenFixtureAvailable)
{
    const std::string path = FindTestVideo();
    if (path.empty())
        GTEST_SKIP() << "No MPEG-1 fixture (set OLO_TEST_VIDEO to a .mpg to exercise decoding).";

    VideoDecoder decoder;
    ASSERT_TRUE(decoder.Open(path));
    EXPECT_TRUE(decoder.IsOpen());
    EXPECT_GT(decoder.GetWidth(), 0u);
    EXPECT_GT(decoder.GetHeight(), 0u);

    std::vector<u8> rgba;
    VideoFrameInfo info;
    ASSERT_TRUE(decoder.DecodeNextFrame(rgba, info));
    EXPECT_EQ(rgba.size(), static_cast<sizet>(info.Width) * info.Height * 4u);
    EXPECT_GT(info.Width, 0u);
    EXPECT_GT(info.Height, 0u);

    // Seeking back to the start should keep the decoder open and decodable.
    EXPECT_TRUE(decoder.Seek(0.0));
    EXPECT_TRUE(decoder.DecodeNextFrame(rgba, info));
}

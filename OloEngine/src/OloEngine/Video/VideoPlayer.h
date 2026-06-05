#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Video/VideoAudioStream.h"
#include "OloEngine/Video/VideoDecoder.h"
#include "OloEngine/Video/VideoTexture.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace OloEngine
{
    enum class VideoPlaybackState : u8
    {
        Stopped = 0,
        Playing,
        Paused
    };

    /// Playback controller for a single video. Owns a VideoDecoder driven on a
    /// background thread that fills a bounded ring buffer of decoded RGBA frames; the
    /// main thread advances a presentation clock in Update(dt) and selects the frame to
    /// show. GPU upload is deferred to UpdateTexture() so the clock/selection logic is
    /// unit-testable with no GL context.
    ///
    /// Threading contract:
    ///   - Load / Unload / Update / UpdateTexture / Play / Pause / Stop / Seek and all
    ///     getters/setters are called from the owner (main) thread.
    ///   - The VideoDecoder is only touched by the decode thread while running (seeks are
    ///     requested via an atomic and serviced on the decode thread), so there is no
    ///     decoder data race.
    ///
    /// Audio / A/V sync: if the file carries an MP2 track, the decode thread also feeds a
    /// VideoAudioStream (miniaudio) and the audio playback position becomes the master clock
    /// — video frames are presented against it so picture locks to audible sound. With no
    /// audio track the clock falls back to wall-time accumulation in Update(dt). (Playback
    /// speed != 1 is honoured only in the video-only fallback; audio always plays at 1x.)
    class VideoPlayer : public RefCounted
    {
      public:
        VideoPlayer() = default;
        ~VideoPlayer();

        VideoPlayer(const VideoPlayer&) = delete;
        VideoPlayer& operator=(const VideoPlayer&) = delete;

        /// Open @p filePath and start the decode thread. Leaves the player Stopped.
        /// @return false if the file could not be opened / has no video stream.
        bool Load(const std::string& filePath);
        void Unload();

        // --- Transport. These set the playback state unconditionally so the state machine
        //     is testable; actual frame advancement in Update() requires a loaded video. ---
        void Play();
        void Pause();
        void Stop();
        void Seek(f64 timeSeconds);
        void SetLooping(bool loop)
        {
            m_Looping = loop;
        }
        void SetPlaybackSpeed(f32 speed);
        void SetVolume(f32 volume);

        /// Advance the presentation clock by @p dt (scaled by playback speed) and pick the
        /// current frame from the decode queue. CPU-only — no GL calls. Fires OnFinished
        /// (once) when a non-looping video reaches the end.
        void Update(f32 dt);

        /// Upload the most recently selected frame to the GPU texture. Must run on the
        /// renderer's GL thread. No-op when there is no new frame.
        void UpdateTexture();

        /// Display a single static RGBA8 image (width*height*4 bytes) through the player,
        /// with no decoder or decode thread — for splash screens / studio logos / a poster
        /// frame shown via the same overlay path as video. Replaces any loaded video.
        void PresentImage(const u8* rgba, u32 width, u32 height);

        [[nodiscard]] bool IsLoaded() const
        {
            return m_Loaded;
        }
        [[nodiscard]] bool IsPlaying() const
        {
            return m_State == VideoPlaybackState::Playing;
        }
        [[nodiscard]] bool IsPaused() const
        {
            return m_State == VideoPlaybackState::Paused;
        }
        [[nodiscard]] bool IsStopped() const
        {
            return m_State == VideoPlaybackState::Stopped;
        }
        [[nodiscard]] bool IsFinished() const
        {
            return m_Finished;
        }
        [[nodiscard]] VideoPlaybackState GetState() const
        {
            return m_State;
        }

        [[nodiscard]] f64 GetCurrentTime() const
        {
            return m_CurrentTime.load();
        }
        [[nodiscard]] f64 GetDuration() const
        {
            return m_Duration;
        }
        [[nodiscard]] f32 GetPlaybackSpeed() const
        {
            return m_PlaybackSpeed;
        }
        [[nodiscard]] f32 GetVolume() const
        {
            return m_Volume;
        }
        [[nodiscard]] bool IsLooping() const
        {
            return m_Looping;
        }
        [[nodiscard]] u32 GetWidth() const
        {
            return m_Width;
        }
        [[nodiscard]] u32 GetHeight() const
        {
            return m_Height;
        }
        [[nodiscard]] f64 GetFrameRate() const
        {
            return m_FrameRate;
        }

        [[nodiscard]] const VideoTexture& GetTexture() const
        {
            return m_Texture;
        }
        [[nodiscard]] VideoTexture& GetTexture()
        {
            return m_Texture;
        }

        /// Fired from Update() on the owner thread when a non-looping video finishes.
        std::function<void()> OnFinished;

      private:
        struct DecodedFrame
        {
            std::vector<u8> Data; // Tightly packed RGBA8.
            f64 Timestamp = 0.0;
            u32 Width = 0;
            u32 Height = 0;
        };

        void DecodeThreadFunc();
        void RestartFromBeginning();

      private:
        static constexpr sizet s_MaxQueuedFrames = 8;

        VideoDecoder m_Decoder;
        VideoTexture m_Texture;
        Scope<VideoAudioStream> m_AudioStream; // null when the video has no audio track

        // Decode thread + bounded frame queue.
        std::thread m_DecodeThread;
        std::atomic<bool> m_ThreadRunning{ false };
        std::atomic<bool> m_DecoderAtEnd{ false };
        std::atomic<bool> m_SeekRequested{ false };
        std::atomic<f64> m_SeekTarget{ 0.0 };
        std::mutex m_QueueMutex;
        std::condition_variable m_QueueCV;
        std::deque<DecodedFrame> m_FrameQueue;

        // Current frame held on the main thread (selected in Update, uploaded in UpdateTexture).
        DecodedFrame m_CurrentFrame;
        bool m_FrameDirty = false;

        // Transport / metadata (owner-thread state; atomics where cross-thread reads occur).
        std::atomic<VideoPlaybackState> m_State{ VideoPlaybackState::Stopped };
        std::atomic<f64> m_CurrentTime{ 0.0 };
        bool m_Loaded = false;
        bool m_Finished = false;
        bool m_Looping = false;
        f32 m_PlaybackSpeed = 1.0f;
        f32 m_Volume = 1.0f;
        f64 m_Duration = 0.0;
        f64 m_FrameRate = 0.0;
        u32 m_Width = 0;
        u32 m_Height = 0;
    };
} // namespace OloEngine

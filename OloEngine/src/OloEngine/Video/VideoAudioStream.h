#pragma once

#include "OloEngine/Core/Base.h"

namespace OloEngine
{
    /// Streams decoded interleaved-stereo float audio from a video's MP2 track into the
    /// engine's miniaudio graph, and exposes the playback position as the A/V master clock.
    ///
    /// A custom `ma_data_source` (backed by a lock-free `ma_pcm_rb`) is attached to a
    /// `ma_sound` on the shared `ma_engine`. The decode thread calls Write(); miniaudio's
    /// audio thread pulls via the data source. The clock is driven by how many frames the
    /// audio thread has actually consumed (so video presentation locks to audible playback).
    ///
    /// All miniaudio types are confined to the .cpp (pImpl) so this header — reachable from
    /// VideoPlayer.h / Components.h — stays light.
    class VideoAudioStream
    {
      public:
        VideoAudioStream();
        ~VideoAudioStream();

        VideoAudioStream(const VideoAudioStream&) = delete;
        VideoAudioStream& operator=(const VideoAudioStream&) = delete;

        /// Create the ring buffer + sound. Returns false if audio is unavailable.
        bool Initialize(u32 sampleRate, u32 channels);
        void Shutdown();

        void Start(); // begin/resume playback
        void Stop();  // pause playback (does not flush)
        void SetVolume(f32 volume);

        /// Flush buffered audio and rebase the clock to @p timeSeconds (after a seek).
        void Flush(f64 timeSeconds);

        /// Write interleaved float frames from the decode thread.
        /// @return frames actually written (may be < frameCount if the ring is full).
        sizet Write(const f32* interleaved, sizet frameCount);

        /// Free space available to Write right now, in frames.
        [[nodiscard("writable frame count must be used")]] sizet WritableFrames() const;

        /// Master clock: seconds of audio the device has consumed (+ last seek base).
        [[nodiscard("clock value must be used")]] f64 ClockSeconds() const;

        [[nodiscard("active state must be used")]] bool IsActive() const
        {
            return m_Active;
        }
        [[nodiscard("sample rate must be used")]] u32 GetSampleRate() const
        {
            return m_SampleRate;
        }

        // Opaque implementation — all miniaudio types live in the .cpp. Public only so the
        // C-style ma_data_source callbacks can name it; callers never construct or use it.
        struct Impl;

      private:
        Scope<Impl> m_Impl;
        bool m_Active = false;
        u32 m_SampleRate = 0;
        u32 m_Channels = 0;
    };
} // namespace OloEngine

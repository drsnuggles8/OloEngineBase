#pragma once

#include "OloEngine/Core/Base.h"

#include <string>
#include <vector>

namespace OloEngine
{
    /// Metadata describing a single decoded video frame.
    struct VideoFrameInfo
    {
        u32 Width = 0;
        u32 Height = 0;
        f64 Timestamp = 0.0; // Presentation timestamp, seconds from stream start.
        f64 Duration = 0.0;  // Nominal frame duration, seconds (1 / framerate).
    };

    /// Decodes video (and optionally audio) frames from a media file.
    ///
    /// Backed by pl_mpeg (MPEG-1 video + MP2 audio). The backend is hidden behind a
    /// pImpl so it can be swapped for FFmpeg/libav later without touching VideoPlayer,
    /// VideoTexture, or the ECS layer. Decoded video frames are returned as tightly
    /// packed RGBA8 (width * height * 4 bytes).
    ///
    /// NOT thread-safe: a given instance must only be accessed from one thread at a
    /// time. VideoPlayer confines every decode/seek call to its single decode thread.
    class VideoDecoder
    {
      public:
        VideoDecoder();
        ~VideoDecoder();

        VideoDecoder(const VideoDecoder&) = delete;
        VideoDecoder& operator=(const VideoDecoder&) = delete;
        VideoDecoder(VideoDecoder&& other) noexcept;
        VideoDecoder& operator=(VideoDecoder&& other) noexcept;

        /// Open a media file for decoding.
        /// @param filePath  Absolute or working-directory-relative path to the file.
        /// @param decodeAudio  When false (default) the audio stream is disabled so the
        ///                     demuxer skips it (avoids unbounded audio-buffer growth when
        ///                     only video is consumed). Set true once audio output is wired.
        /// @return true on success; false if the file is missing or has no video stream.
        bool Open(const std::string& filePath, bool decodeAudio = false);
        void Close();

        [[nodiscard("open state must be used")]] bool IsOpen() const;
        [[nodiscard("duration must be used")]] f64 GetDuration() const; // Total seconds (0 if unknown).
        [[nodiscard("width must be used")]] u32 GetWidth() const;
        [[nodiscard("height must be used")]] u32 GetHeight() const;
        [[nodiscard("frame rate must be used")]] f64 GetFrameRate() const;        // Frames per second.
        [[nodiscard("audio-presence flag must be used")]] bool HasAudio() const;  // True if the file carries an audio stream.
        [[nodiscard("sample rate must be used")]] u32 GetAudioSampleRate() const; // Hz (0 if no audio).
        [[nodiscard("channel count must be used")]] u32 GetAudioChannels() const; // Output channel count (backends output stereo: 2).

        /// Decode the next video frame into @p outRGBA (resized to width*height*4).
        /// @return false at end-of-stream or when no decoder is open.
        bool DecodeNextFrame(std::vector<u8>& outRGBA, VideoFrameInfo& outInfo);

        /// Decode the next block of interleaved-stereo float audio samples.
        /// @param outTimestamp  Presentation time of the first sample, seconds.
        /// @return false at end-of-stream, when audio decoding is disabled, or no decoder.
        bool DecodeAudioSamples(std::vector<f32>& outSamples, f64& outTimestamp);

        /// Seek to @p timeSeconds. Decodes the keyframe at/just before the target so the
        /// next DecodeNextFrame returns a frame at or after the requested time.
        /// @return true on success.
        bool Seek(f64 timeSeconds);

        /// True once the end of the video stream has been reached.
        [[nodiscard("end-of-stream flag must be used")]] bool IsEndOfStream() const;

      private:
        struct Impl;
        Scope<Impl> m_Impl;
    };
} // namespace OloEngine

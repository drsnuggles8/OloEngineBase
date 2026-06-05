#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Video/VideoDecoder.h" // VideoFrameInfo

#include <string>
#include <vector>

namespace OloEngine
{
    /// Internal decode-backend interface behind VideoDecoder's pImpl. Each backend wraps a
    /// concrete decoder (pl_mpeg for MPEG-1; FFmpeg/libav for H.264/HEVC/VP9 in MP4/MOV/MKV).
    /// Decoded video frames are returned as tightly packed RGBA8; audio as interleaved-stereo
    /// float. Not thread-safe — the owner confines all calls to one thread.
    class IVideoDecoderBackend
    {
      public:
        virtual ~IVideoDecoderBackend() = default;

        virtual bool Open(const std::string& filePath, bool decodeAudio) = 0;

        [[nodiscard]] virtual u32 GetWidth() const = 0;
        [[nodiscard]] virtual u32 GetHeight() const = 0;
        [[nodiscard]] virtual f64 GetDuration() const = 0;
        [[nodiscard]] virtual f64 GetFrameRate() const = 0;
        [[nodiscard]] virtual bool HasAudio() const = 0;
        [[nodiscard]] virtual u32 GetAudioSampleRate() const = 0;
        [[nodiscard]] virtual u32 GetAudioChannels() const = 0; // output channels (always 2)

        virtual bool DecodeNextFrame(std::vector<u8>& outRGBA, VideoFrameInfo& outInfo) = 0;
        virtual bool DecodeAudioSamples(std::vector<f32>& outSamples, f64& outTimestamp) = 0;
        virtual bool Seek(f64 timeSeconds) = 0;
        [[nodiscard]] virtual bool IsEndOfStream() const = 0;
    };

    /// pl_mpeg backend (MPEG-1 video + MP2 audio). Always available.
    [[nodiscard]] Scope<IVideoDecoderBackend> CreatePlMpegBackend();

#if defined(OLO_VIDEO_FFMPEG)
    /// FFmpeg/libav backend (H.264/HEVC/VP9/… in MP4/MOV/MKV/…). Built from source when the
    /// OLO_VIDEO_FFMPEG CMake option is on.
    [[nodiscard]] Scope<IVideoDecoderBackend> CreateFFmpegBackend();
#endif
} // namespace OloEngine

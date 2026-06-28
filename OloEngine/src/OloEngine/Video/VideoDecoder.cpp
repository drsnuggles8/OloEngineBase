#include "OloEnginePCH.h"
#include "OloEngine/Video/VideoDecoder.h"
#include "OloEngine/Video/VideoDecoderBackend.h"

#include <filesystem>
#include <utility>

namespace OloEngine
{
    struct VideoDecoder::Impl
    {
        Scope<IVideoDecoderBackend> Backend;
    };

    VideoDecoder::VideoDecoder()
        : m_Impl(CreateScope<Impl>())
    {
    }

    VideoDecoder::~VideoDecoder() = default;
    VideoDecoder::VideoDecoder(VideoDecoder&&) noexcept = default;
    VideoDecoder& VideoDecoder::operator=(VideoDecoder&&) noexcept = default;

    namespace
    {
        bool IsMpeg1Extension(const std::string& filePath)
        {
            std::string ext = std::filesystem::path(filePath).extension().string();
            // ASCII-lowercase in place; file extensions are ASCII, so this avoids the
            // locale-dependent <cctype> functions (and their narrow-char UB pitfalls).
            for (char& ch : ext)
            {
                if (ch >= 'A' && ch <= 'Z')
                    ch = static_cast<char>(ch - 'A' + 'a');
            }
            return ext == ".mpg" || ext == ".mpeg" || ext == ".m1v" || ext == ".mpg1";
        }
    } // namespace

    bool VideoDecoder::Open(const std::string& filePath, bool decodeAudio)
    {
        OLO_PROFILE_FUNCTION();

        Close();

        if (filePath.empty())
        {
            OLO_CORE_WARN("VideoDecoder::Open - empty file path");
            return false;
        }

        // pl_mpeg handles MPEG-1 (.mpg) with zero extra dependencies; the FFmpeg backend
        // (when built via OLO_VIDEO_FFMPEG) handles everything else — H.264/HEVC/VP9 in
        // MP4/MOV/MKV, etc.
        Scope<IVideoDecoderBackend> backend;
        if (IsMpeg1Extension(filePath))
        {
            backend = CreatePlMpegBackend();
        }
        else
        {
#if defined(OLO_VIDEO_FFMPEG)
            backend = CreateFFmpegBackend();
#else
            OLO_CORE_WARN("VideoDecoder::Open - '{}' requires the FFmpeg backend (build with "
                          "OLO_VIDEO_FFMPEG); only MPEG-1 (.mpg/.mpeg/.m1v) is supported in this build",
                          filePath);
            return false;
#endif
        }

        if (!backend || !backend->Open(filePath, decodeAudio))
            return false;

        m_Impl->Backend = std::move(backend);
        return true;
    }

    void VideoDecoder::Close()
    {
        m_Impl->Backend.reset();
    }

    bool VideoDecoder::IsOpen() const
    {
        return m_Impl->Backend != nullptr;
    }
    f64 VideoDecoder::GetDuration() const
    {
        return m_Impl->Backend ? m_Impl->Backend->GetDuration() : 0.0;
    }
    u32 VideoDecoder::GetWidth() const
    {
        return m_Impl->Backend ? m_Impl->Backend->GetWidth() : 0u;
    }
    u32 VideoDecoder::GetHeight() const
    {
        return m_Impl->Backend ? m_Impl->Backend->GetHeight() : 0u;
    }
    f64 VideoDecoder::GetFrameRate() const
    {
        return m_Impl->Backend ? m_Impl->Backend->GetFrameRate() : 0.0;
    }
    bool VideoDecoder::HasAudio() const
    {
        return m_Impl->Backend && m_Impl->Backend->HasAudio();
    }
    u32 VideoDecoder::GetAudioSampleRate() const
    {
        return m_Impl->Backend ? m_Impl->Backend->GetAudioSampleRate() : 0u;
    }
    u32 VideoDecoder::GetAudioChannels() const
    {
        return m_Impl->Backend ? m_Impl->Backend->GetAudioChannels() : 0u;
    }
    bool VideoDecoder::IsEndOfStream() const
    {
        return m_Impl->Backend && m_Impl->Backend->IsEndOfStream();
    }

    bool VideoDecoder::DecodeNextFrame(std::vector<u8>& outRGBA, VideoFrameInfo& outInfo)
    {
        return m_Impl->Backend && m_Impl->Backend->DecodeNextFrame(outRGBA, outInfo);
    }

    bool VideoDecoder::DecodeAudioSamples(std::vector<f32>& outSamples, f64& outTimestamp)
    {
        return m_Impl->Backend && m_Impl->Backend->DecodeAudioSamples(outSamples, outTimestamp);
    }

    bool VideoDecoder::Seek(f64 timeSeconds)
    {
        OLO_PROFILE_FUNCTION();
        return m_Impl->Backend && m_Impl->Backend->Seek(timeSeconds);
    }
} // namespace OloEngine

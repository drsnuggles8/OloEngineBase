#include "OloEnginePCH.h"
#include "OloEngine/Video/VideoDecoderBackend.h"

// Declarations only — the pl_mpeg implementation is compiled once in PlMpeg.cpp.
#include <pl_mpeg.h>

namespace OloEngine
{
    namespace
    {
        // MPEG-1 video + MP2 audio backend. Wraps a single plm_t decode session.
        class PlMpegBackend final : public IVideoDecoderBackend
        {
          public:
            PlMpegBackend() = default;
            ~PlMpegBackend() override
            {
                if (m_Plm)
                    plm_destroy(m_Plm);
            }

            // Owns a raw plm_t* — non-copyable and non-movable (it is only ever held by a Scope).
            PlMpegBackend(const PlMpegBackend&) = delete;
            PlMpegBackend& operator=(const PlMpegBackend&) = delete;
            PlMpegBackend(PlMpegBackend&&) = delete;
            PlMpegBackend& operator=(PlMpegBackend&&) = delete;

            bool Open(const std::string& filePath, bool decodeAudio) override
            {
                plm_t* plm = plm_create_with_filename(filePath.c_str());
                if (!plm)
                {
                    OLO_CORE_WARN("PlMpegBackend::Open - failed to open '{}' (missing file or unreadable)", filePath);
                    return false;
                }

                const int width = plm_get_width(plm);
                const int height = plm_get_height(plm);
                if (plm_get_num_video_streams(plm) <= 0 || width <= 0 || height <= 0)
                {
                    OLO_CORE_WARN("PlMpegBackend::Open - '{}' has no decodable MPEG-1 video stream", filePath);
                    plm_destroy(plm);
                    return false;
                }

                plm_set_loop(plm, 0); // VideoPlayer drives looping

                m_HasAudioStream = plm_get_num_audio_streams(plm) > 0;
                m_DecodeAudio = decodeAudio && m_HasAudioStream;
                if (m_DecodeAudio)
                {
                    plm_set_audio_enabled(plm, 1);
                    plm_set_audio_stream(plm, 0);
                    m_SampleRate = static_cast<u32>(plm_get_samplerate(plm));
                }
                else
                {
                    plm_set_audio_enabled(plm, 0);
                }

                m_Plm = plm;
                m_Width = static_cast<u32>(width);
                m_Height = static_cast<u32>(height);
                m_FrameRate = plm_get_framerate(plm);
                m_Duration = plm_get_duration(plm);
                m_EndOfStream = false;

                OLO_CORE_INFO("PlMpegBackend::Open - '{}' {}x{} @ {:.2f}fps, {:.2f}s{}",
                              filePath, m_Width, m_Height, m_FrameRate, m_Duration,
                              m_HasAudioStream ? " (+audio)" : "");
                return true;
            }

            u32 GetWidth() const override
            {
                return m_Width;
            }
            u32 GetHeight() const override
            {
                return m_Height;
            }
            f64 GetDuration() const override
            {
                return m_Duration;
            }
            f64 GetFrameRate() const override
            {
                return m_FrameRate;
            }
            bool HasAudio() const override
            {
                return m_HasAudioStream;
            }
            u32 GetAudioSampleRate() const override
            {
                return m_SampleRate;
            }
            u32 GetAudioChannels() const override
            {
                return m_HasAudioStream ? 2u : 0u;
            }
            bool IsEndOfStream() const override
            {
                return m_EndOfStream;
            }

            bool DecodeNextFrame(std::vector<u8>& outRGBA, VideoFrameInfo& outInfo) override
            {
                if (!m_Plm)
                    return false;

                plm_frame_t* frame = plm_decode_video(m_Plm);
                if (!frame)
                {
                    m_EndOfStream = true;
                    return false;
                }

                const u32 w = frame->width;
                const u32 h = frame->height;
                if (const sizet byteCount = static_cast<sizet>(w) * h * 4u; outRGBA.size() != byteCount)
                    outRGBA.resize(byteCount);

                plm_frame_to_rgba(frame, outRGBA.data(), static_cast<int>(w * 4u));

                outInfo.Width = w;
                outInfo.Height = h;
                outInfo.Timestamp = frame->time;
                outInfo.Duration = (m_FrameRate > 0.0) ? (1.0 / m_FrameRate) : 0.0;
                return true;
            }

            bool DecodeAudioSamples(std::vector<f32>& outSamples, f64& outTimestamp) override
            {
                if (!m_Plm || !m_DecodeAudio)
                    return false;

                plm_samples_t* samples = plm_decode_audio(m_Plm);
                if (!samples)
                    return false;

                const sizet floatCount = static_cast<sizet>(samples->count) * 2u; // interleaved stereo
                outSamples.assign(samples->interleaved, samples->interleaved + floatCount);
                outTimestamp = samples->time;
                return true;
            }

            bool Seek(f64 timeSeconds) override
            {
                if (!m_Plm)
                    return false;
                constexpr int seekExact = 1;
                const int ok = plm_seek(m_Plm, timeSeconds, seekExact);
                if (ok)
                    m_EndOfStream = false;
                return ok != 0;
            }

          private:
            plm_t* m_Plm = nullptr;
            u32 m_Width = 0;
            u32 m_Height = 0;
            f64 m_FrameRate = 0.0;
            f64 m_Duration = 0.0;
            u32 m_SampleRate = 0;
            bool m_HasAudioStream = false;
            bool m_DecodeAudio = false;
            bool m_EndOfStream = false;
        };
    } // namespace

    Scope<IVideoDecoderBackend> CreatePlMpegBackend()
    {
        return CreateScope<PlMpegBackend>();
    }
} // namespace OloEngine

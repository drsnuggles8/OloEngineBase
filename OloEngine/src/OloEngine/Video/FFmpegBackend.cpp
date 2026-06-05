#include "OloEnginePCH.h"
#include "OloEngine/Video/VideoDecoderBackend.h"

#if defined(OLO_VIDEO_FFMPEG)

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace OloEngine
{
    namespace
    {
        // FFmpeg/libav backend. Reads interleaved packets once and routes decoded video frames
        // and resampled-stereo-float audio into separate queues so VideoDecoder's independent
        // DecodeNextFrame / DecodeAudioSamples calls each pull from the right one.
        class FFmpegBackend final : public IVideoDecoderBackend
        {
          public:
            ~FFmpegBackend() override
            {
                Cleanup();
            }

            bool Open(const std::string& filePath, bool decodeAudio) override
            {
                if (avformat_open_input(&m_Fmt, filePath.c_str(), nullptr, nullptr) != 0)
                {
                    OLO_CORE_WARN("FFmpegBackend::Open - could not open '{}'", filePath);
                    return false;
                }
                if (avformat_find_stream_info(m_Fmt, nullptr) < 0)
                {
                    OLO_CORE_WARN("FFmpegBackend::Open - no stream info in '{}'", filePath);
                    Cleanup();
                    return false;
                }

                m_VideoStream = av_find_best_stream(m_Fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
                if (m_VideoStream < 0 || !OpenCodec(m_VideoStream, &m_VideoCtx))
                {
                    OLO_CORE_WARN("FFmpegBackend::Open - no decodable video stream in '{}'", filePath);
                    Cleanup();
                    return false;
                }

                m_Width = static_cast<u32>(m_VideoCtx->width);
                m_Height = static_cast<u32>(m_VideoCtx->height);

                const AVRational fr = av_guess_frame_rate(m_Fmt, m_Fmt->streams[m_VideoStream], nullptr);
                m_FrameRate = (fr.num && fr.den) ? av_q2d(fr) : 0.0;
                if (m_Fmt->duration != AV_NOPTS_VALUE)
                    m_Duration = static_cast<f64>(m_Fmt->duration) / AV_TIME_BASE;

                // Detect (and optionally decode) an audio stream.
                const int audioStream = av_find_best_stream(m_Fmt, AVMEDIA_TYPE_AUDIO, -1, m_VideoStream, nullptr, 0);
                m_HasAudio = (audioStream >= 0);
                if (decodeAudio && m_HasAudio && OpenCodec(audioStream, &m_AudioCtx))
                {
                    m_AudioStream = audioStream;
                    m_SampleRate = static_cast<u32>(m_AudioCtx->sample_rate);
                    if (!SetupResampler())
                    {
                        // Audio resampler failed — fall back to video-only.
                        avcodec_free_context(&m_AudioCtx);
                        m_AudioStream = -1;
                    }
                }

                m_Packet = av_packet_alloc();
                m_Frame = av_frame_alloc();
                m_EndOfStream = false;

                OLO_CORE_INFO("FFmpegBackend::Open - '{}' {}x{} @ {:.2f}fps, {:.2f}s{}",
                              filePath, m_Width, m_Height, m_FrameRate, m_Duration, m_HasAudio ? " (+audio)" : "");
                return m_Packet && m_Frame;
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
                return m_HasAudio;
            }
            u32 GetAudioSampleRate() const override
            {
                return m_SampleRate;
            }
            u32 GetAudioChannels() const override
            {
                return (m_AudioStream >= 0) ? 2u : 0u;
            }
            bool IsEndOfStream() const override
            {
                return m_EndOfStream;
            }

            bool DecodeNextFrame(std::vector<u8>& outRGBA, VideoFrameInfo& outInfo) override
            {
                while (m_VideoFrames.empty() && !m_DemuxDone)
                    ReadAndRoute();

                if (m_VideoFrames.empty())
                {
                    m_EndOfStream = true;
                    return false;
                }

                AVFrame* frame = m_VideoFrames.front();
                m_VideoFrames.pop_front();

                const u32 w = static_cast<u32>(frame->width);
                const u32 h = static_cast<u32>(frame->height);
                const sizet byteCount = static_cast<sizet>(w) * h * 4u;
                if (outRGBA.size() != byteCount)
                    outRGBA.resize(byteCount);

                m_Sws = sws_getCachedContext(m_Sws, frame->width, frame->height,
                                             static_cast<AVPixelFormat>(frame->format), static_cast<int>(w), static_cast<int>(h),
                                             AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (!m_Sws)
                {
                    // Could not build a colour converter — fail rather than return uninitialised pixels.
                    OLO_CORE_WARN("FFmpegBackend::DecodeNextFrame - sws_getCachedContext failed");
                    av_frame_free(&frame);
                    return false;
                }

                uint8_t* dst[4] = { outRGBA.data(), nullptr, nullptr, nullptr };
                int dstStride[4] = { static_cast<int>(w * 4u), 0, 0, 0 };
                sws_scale(m_Sws, frame->data, frame->linesize, 0, frame->height, dst, dstStride);

                outInfo.Width = w;
                outInfo.Height = h;
                outInfo.Timestamp = FramePtsSeconds(frame);
                outInfo.Duration = (m_FrameRate > 0.0) ? (1.0 / m_FrameRate) : 0.0;
                av_frame_free(&frame);
                return true;
            }

            bool DecodeAudioSamples(std::vector<f32>& outSamples, f64& outTimestamp) override
            {
                if (m_AudioStream < 0)
                    return false;

                while (m_AudioBlocks.empty() && !m_DemuxDone)
                    ReadAndRoute();

                if (m_AudioBlocks.empty())
                    return false;

                outSamples = std::move(m_AudioBlocks.front().Samples);
                outTimestamp = m_AudioBlocks.front().Timestamp;
                m_AudioBlocks.pop_front();
                return true;
            }

            bool Seek(f64 timeSeconds) override
            {
                if (!m_Fmt)
                    return false;

                const int64_t ts = static_cast<int64_t>(timeSeconds * AV_TIME_BASE);
                if (av_seek_frame(m_Fmt, -1, ts, AVSEEK_FLAG_BACKWARD) < 0)
                    return false;

                if (m_VideoCtx)
                    avcodec_flush_buffers(m_VideoCtx);
                if (m_AudioCtx)
                    avcodec_flush_buffers(m_AudioCtx);

                ClearQueues();
                m_DemuxDone = false;
                m_EndOfStream = false;
                return true;
            }

          private:
            struct AudioBlock
            {
                std::vector<f32> Samples; // interleaved stereo float
                f64 Timestamp = 0.0;
            };

            bool OpenCodec(int streamIndex, AVCodecContext** outCtx)
            {
                AVStream* stream = m_Fmt->streams[streamIndex];
                const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
                if (!codec)
                    return false;
                AVCodecContext* ctx = avcodec_alloc_context3(codec);
                if (!ctx)
                    return false;
                if (avcodec_parameters_to_context(ctx, stream->codecpar) < 0 || avcodec_open2(ctx, codec, nullptr) < 0)
                {
                    avcodec_free_context(&ctx);
                    return false;
                }
                *outCtx = ctx;
                return true;
            }

            bool SetupResampler()
            {
                AVChannelLayout outLayout;
                av_channel_layout_default(&outLayout, 2); // stereo

                AVChannelLayout inLayout;
                if (m_AudioCtx->ch_layout.nb_channels > 0)
                    av_channel_layout_copy(&inLayout, &m_AudioCtx->ch_layout);
                else
                    av_channel_layout_default(&inLayout, 2);

                const int rc = swr_alloc_set_opts2(&m_Swr,
                                                   &outLayout, AV_SAMPLE_FMT_FLT, m_AudioCtx->sample_rate,
                                                   &inLayout, m_AudioCtx->sample_fmt, m_AudioCtx->sample_rate,
                                                   0, nullptr);
                av_channel_layout_uninit(&outLayout);
                av_channel_layout_uninit(&inLayout);

                if (rc < 0 || !m_Swr || swr_init(m_Swr) < 0)
                {
                    if (m_Swr)
                        swr_free(&m_Swr);
                    return false;
                }
                return true;
            }

            f64 StreamTimeSeconds(int streamIndex, int64_t pts) const
            {
                if (pts == AV_NOPTS_VALUE)
                    return 0.0;
                return static_cast<f64>(pts) * av_q2d(m_Fmt->streams[streamIndex]->time_base);
            }

            f64 FramePtsSeconds(const AVFrame* frame) const
            {
                int64_t pts = frame->best_effort_timestamp;
                if (pts == AV_NOPTS_VALUE)
                    pts = frame->pts;
                return StreamTimeSeconds(m_VideoStream, pts);
            }

            // Read one packet and route the frames it yields into the video / audio queues.
            // On end-of-file, flush both decoders once.
            void ReadAndRoute()
            {
                const int rc = av_read_frame(m_Fmt, m_Packet);
                if (rc < 0)
                {
                    // Flush decoders (drain buffered frames), then we're done demuxing.
                    DrainDecoder(m_VideoCtx, m_VideoStream);
                    if (m_AudioCtx)
                        DrainDecoder(m_AudioCtx, m_AudioStream);
                    m_DemuxDone = true;
                    return;
                }

                if (m_Packet->stream_index == m_VideoStream)
                {
                    SendAndReceive(m_VideoCtx, m_VideoStream);
                }
                else if (m_AudioCtx && m_Packet->stream_index == m_AudioStream)
                {
                    SendAndReceive(m_AudioCtx, m_AudioStream);
                }
                av_packet_unref(m_Packet);
            }

            void SendAndReceive(AVCodecContext* ctx, int streamIndex)
            {
                if (avcodec_send_packet(ctx, m_Packet) < 0)
                    return;
                ReceiveFrames(ctx, streamIndex);
            }

            void DrainDecoder(AVCodecContext* ctx, int streamIndex)
            {
                if (!ctx)
                    return;
                avcodec_send_packet(ctx, nullptr); // enter draining mode
                ReceiveFrames(ctx, streamIndex);
            }

            void ReceiveFrames(AVCodecContext* ctx, int streamIndex)
            {
                while (avcodec_receive_frame(ctx, m_Frame) == 0)
                {
                    if (streamIndex == m_VideoStream)
                    {
                        m_VideoFrames.push_back(av_frame_clone(m_Frame));
                    }
                    else
                    {
                        ResampleAudio(m_Frame, streamIndex);
                    }
                    av_frame_unref(m_Frame);
                }
            }

            void ResampleAudio(AVFrame* frame, int streamIndex)
            {
                if (!m_Swr)
                    return;

                // Upper bound on output samples per channel after rate match (rate is equal).
                const int maxOut = swr_get_out_samples(m_Swr, frame->nb_samples);
                if (maxOut <= 0)
                    return;

                AudioBlock block;
                block.Samples.resize(static_cast<sizet>(maxOut) * 2u); // interleaved stereo
                uint8_t* out[1] = { reinterpret_cast<uint8_t*>(block.Samples.data()) };
                const int produced = swr_convert(m_Swr, out, maxOut,
                                                 const_cast<const uint8_t**>(frame->data), frame->nb_samples);
                if (produced <= 0)
                    return;
                block.Samples.resize(static_cast<sizet>(produced) * 2u);
                block.Timestamp = StreamTimeSeconds(streamIndex, frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts);
                m_AudioBlocks.push_back(std::move(block));
            }

            void ClearQueues()
            {
                for (AVFrame* f : m_VideoFrames)
                {
                    AVFrame* tmp = f;
                    av_frame_free(&tmp);
                }
                m_VideoFrames.clear();
                m_AudioBlocks.clear();
            }

            void Cleanup()
            {
                ClearQueues();
                if (m_Sws)
                {
                    sws_freeContext(m_Sws);
                    m_Sws = nullptr;
                }
                if (m_Swr)
                {
                    swr_free(&m_Swr);
                }
                if (m_Frame)
                {
                    av_frame_free(&m_Frame);
                }
                if (m_Packet)
                {
                    av_packet_free(&m_Packet);
                }
                if (m_VideoCtx)
                {
                    avcodec_free_context(&m_VideoCtx);
                }
                if (m_AudioCtx)
                {
                    avcodec_free_context(&m_AudioCtx);
                }
                if (m_Fmt)
                {
                    avformat_close_input(&m_Fmt);
                }
                m_VideoStream = -1;
                m_AudioStream = -1;
            }

          private:
            AVFormatContext* m_Fmt = nullptr;
            AVCodecContext* m_VideoCtx = nullptr;
            AVCodecContext* m_AudioCtx = nullptr;
            SwsContext* m_Sws = nullptr;
            SwrContext* m_Swr = nullptr;
            AVPacket* m_Packet = nullptr;
            AVFrame* m_Frame = nullptr;

            int m_VideoStream = -1;
            int m_AudioStream = -1;

            std::deque<AVFrame*> m_VideoFrames;
            std::deque<AudioBlock> m_AudioBlocks;

            u32 m_Width = 0;
            u32 m_Height = 0;
            f64 m_FrameRate = 0.0;
            f64 m_Duration = 0.0;
            u32 m_SampleRate = 0;
            bool m_HasAudio = false;
            bool m_DemuxDone = false;
            bool m_EndOfStream = false;
        };
    } // namespace

    Scope<IVideoDecoderBackend> CreateFFmpegBackend()
    {
        return CreateScope<FFmpegBackend>();
    }
} // namespace OloEngine

#endif // OLO_VIDEO_FFMPEG

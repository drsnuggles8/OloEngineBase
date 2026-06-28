#include "OloEnginePCH.h"
#include "OloEngine/Video/VideoAudioStream.h"
#include "OloEngine/Audio/AudioEngine.h"

#include <miniaudio.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace OloEngine
{
    // The ma_data_source_base MUST be the first member so a ma_data_source* handed to the
    // vtable callbacks (it is really a void*) can be static_cast straight back to this struct.
    struct VideoAudioStream::Impl
    {
        ma_data_source_base DataSource{};
        ma_pcm_rb RingBuffer{};
        ma_sound Sound{};
        bool RingInit = false;
        bool SoundInit = false;
        u32 Channels = 2;
        u32 SampleRate = 0;
        std::atomic<u64> FramesConsumed{ 0 };
        std::atomic<double> SeekBase{ 0.0 };
    };

    namespace
    {
        VideoAudioStream::Impl* ImplFrom(ma_data_source* pDataSource)
        {
            return static_cast<VideoAudioStream::Impl*>(pDataSource);
        }

        ma_result VideoDS_OnRead(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead)
        {
            auto* impl = ImplFrom(pDataSource);
            const u32 ch = impl->Channels;
            float* out = static_cast<float*>(pFramesOut);

            ma_uint64 totalRead = 0;
            if (impl->RingInit)
            {
                while (totalRead < frameCount)
                {
                    ma_uint32 want = static_cast<ma_uint32>(std::min<ma_uint64>(frameCount - totalRead, 0xFFFFFFFFu));
                    void* src = nullptr;
                    if (ma_pcm_rb_acquire_read(&impl->RingBuffer, &want, &src) != MA_SUCCESS || want == 0)
                        break;
                    std::memcpy(out + totalRead * ch, src, static_cast<size_t>(want) * ch * sizeof(float));
                    ma_pcm_rb_commit_read(&impl->RingBuffer, want);
                    totalRead += want;
                }
            }

            // Fill any underrun with silence so the device — and thus the master clock —
            // keeps advancing rather than stalling video presentation.
            if (totalRead < frameCount)
                std::memset(out + totalRead * ch, 0, static_cast<size_t>(frameCount - totalRead) * ch * sizeof(float));

            impl->FramesConsumed.fetch_add(frameCount, std::memory_order_relaxed);
            if (pFramesRead)
                *pFramesRead = frameCount;
            return MA_SUCCESS;
        }

        ma_result VideoDS_OnSeek(ma_data_source*, ma_uint64)
        {
            // Live stream — seeking is handled by VideoAudioStream::Flush, not here.
            return MA_SUCCESS;
        }

        ma_result VideoDS_OnGetDataFormat(ma_data_source* pDataSource, ma_format* pFormat, ma_uint32* pChannels, ma_uint32* pSampleRate, ma_channel* pChannelMap, size_t channelMapCap)
        {
            const auto* impl = ImplFrom(pDataSource);
            if (pFormat)
                *pFormat = ma_format_f32;
            if (pChannels)
                *pChannels = impl->Channels;
            if (pSampleRate)
                *pSampleRate = impl->SampleRate;
            if (pChannelMap)
                ma_channel_map_init_standard(ma_standard_channel_map_default, pChannelMap, channelMapCap, impl->Channels);
            return MA_SUCCESS;
        }

        ma_result VideoDS_OnGetCursor(ma_data_source* pDataSource, ma_uint64* pCursor)
        {
            const auto* impl = ImplFrom(pDataSource);
            if (pCursor)
                *pCursor = impl->FramesConsumed.load(std::memory_order_relaxed);
            return MA_SUCCESS;
        }

        const ma_data_source_vtable g_VideoDataSourceVtable = {
            VideoDS_OnRead,
            VideoDS_OnSeek,
            VideoDS_OnGetDataFormat,
            VideoDS_OnGetCursor,
            nullptr, // onGetLength: unknown / infinite
            nullptr, // onSetLooping
            0        // flags
        };
    } // namespace

    VideoAudioStream::VideoAudioStream() = default;

    VideoAudioStream::~VideoAudioStream()
    {
        Shutdown();
    }

    bool VideoAudioStream::Initialize(u32 sampleRate, u32 channels)
    {
        Shutdown();

        if (sampleRate == 0)
            return false;

        auto* engine = static_cast<ma_engine*>(AudioEngine::GetEngine());
        if (!engine)
            return false;

        m_Impl = CreateScope<Impl>();
        m_Impl->Channels = (channels == 0) ? 2u : channels;
        m_Impl->SampleRate = sampleRate;

        // ~1 second of decoded audio buffered ahead.
        if (ma_pcm_rb_init(ma_format_f32, m_Impl->Channels, sampleRate, nullptr, nullptr, &m_Impl->RingBuffer) != MA_SUCCESS)
        {
            m_Impl.reset();
            return false;
        }
        m_Impl->RingInit = true;
        ma_pcm_rb_set_sample_rate(&m_Impl->RingBuffer, sampleRate);

        ma_data_source_config dsConfig = ma_data_source_config_init();
        dsConfig.vtable = &g_VideoDataSourceVtable;
        if (ma_data_source_init(&dsConfig, &m_Impl->DataSource) != MA_SUCCESS)
        {
            ma_pcm_rb_uninit(&m_Impl->RingBuffer);
            m_Impl.reset();
            return false;
        }

        if (ma_sound_init_from_data_source(engine, &m_Impl->DataSource, MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, &m_Impl->Sound) != MA_SUCCESS)
        {
            ma_data_source_uninit(&m_Impl->DataSource);
            ma_pcm_rb_uninit(&m_Impl->RingBuffer);
            m_Impl.reset();
            return false;
        }
        m_Impl->SoundInit = true;

        m_SampleRate = sampleRate;
        m_Channels = m_Impl->Channels;
        m_Active = true;
        return true;
    }

    void VideoAudioStream::Shutdown()
    {
        if (!m_Impl)
        {
            m_Active = false;
            return;
        }

        if (m_Impl->SoundInit)
        {
            ma_sound_stop(&m_Impl->Sound);
            ma_sound_uninit(&m_Impl->Sound); // detaches from the engine graph first
            m_Impl->SoundInit = false;
        }
        ma_data_source_uninit(&m_Impl->DataSource);
        if (m_Impl->RingInit)
        {
            ma_pcm_rb_uninit(&m_Impl->RingBuffer);
            m_Impl->RingInit = false;
        }

        m_Impl.reset();
        m_Active = false;
        m_SampleRate = 0;
        m_Channels = 0;
    }

    void VideoAudioStream::Start()
    {
        if (m_Impl && m_Impl->SoundInit)
            ma_sound_start(&m_Impl->Sound);
    }

    void VideoAudioStream::Stop()
    {
        if (m_Impl && m_Impl->SoundInit)
            ma_sound_stop(&m_Impl->Sound);
    }

    void VideoAudioStream::SetVolume(f32 volume)
    {
        if (m_Impl && m_Impl->SoundInit)
            ma_sound_set_volume(&m_Impl->Sound, volume);
    }

    void VideoAudioStream::Flush(f64 timeSeconds)
    {
        if (!m_Impl)
            return;
        if (m_Impl->RingInit)
            ma_pcm_rb_reset(&m_Impl->RingBuffer);
        m_Impl->FramesConsumed.store(0, std::memory_order_relaxed);
        m_Impl->SeekBase.store(timeSeconds, std::memory_order_relaxed);
    }

    sizet VideoAudioStream::Write(const f32* interleaved, sizet frameCount)
    {
        const bool ready = m_Active && m_Impl && m_Impl->RingInit;
        if (!ready || !interleaved || frameCount == 0)
            return 0;

        sizet written = 0;
        while (written < frameCount)
        {
            ma_uint32 want = static_cast<ma_uint32>(std::min<sizet>(frameCount - written, 0xFFFFFFFFu));
            void* dst = nullptr;
            if (ma_pcm_rb_acquire_write(&m_Impl->RingBuffer, &want, &dst) != MA_SUCCESS || want == 0)
                break;
            std::memcpy(dst, interleaved + written * m_Impl->Channels, static_cast<size_t>(want) * m_Impl->Channels * sizeof(float));
            ma_pcm_rb_commit_write(&m_Impl->RingBuffer, want);
            written += want;
        }
        return written;
    }

    sizet VideoAudioStream::WritableFrames() const
    {
        if (!m_Active || !m_Impl || !m_Impl->RingInit)
            return 0;
        return ma_pcm_rb_available_write(&m_Impl->RingBuffer);
    }

    f64 VideoAudioStream::ClockSeconds() const
    {
        if (!m_Active || !m_Impl || m_SampleRate == 0)
            return 0.0;
        const u64 frames = m_Impl->FramesConsumed.load(std::memory_order_relaxed);
        return m_Impl->SeekBase.load(std::memory_order_relaxed) + static_cast<f64>(frames) / static_cast<f64>(m_SampleRate);
    }
} // namespace OloEngine

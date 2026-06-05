#include "OloEnginePCH.h"
#include "OloEngine/Video/VideoPlayer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace OloEngine
{
    VideoPlayer::~VideoPlayer()
    {
        Unload();
    }

    bool VideoPlayer::Load(const std::string& filePath)
    {
        OLO_PROFILE_FUNCTION();

        Unload();

        // Request audio decoding so we can drive A/V sync; the decoder reports whether the
        // file actually carries an audio stream.
        if (!m_Decoder.Open(filePath, /*decodeAudio=*/true))
            return false;

        m_Width = m_Decoder.GetWidth();
        m_Height = m_Decoder.GetHeight();
        m_Duration = m_Decoder.GetDuration();
        m_FrameRate = m_Decoder.GetFrameRate();

        // Set up audio output + master clock when the file has an audio track.
        if (m_Decoder.HasAudio())
        {
            auto stream = CreateScope<VideoAudioStream>();
            if (stream->Initialize(m_Decoder.GetAudioSampleRate(), m_Decoder.GetAudioChannels()))
            {
                stream->SetVolume(m_Volume);
                m_AudioStream = std::move(stream);
            }
            else
            {
                OLO_CORE_WARN("VideoPlayer::Load - audio unavailable; '{}' will play video-only", filePath);
            }
        }

        m_Loaded = true;
        m_Finished = false;
        m_State = VideoPlaybackState::Stopped;
        m_CurrentTime = 0.0;
        m_DecoderAtEnd = false;
        m_SeekRequested = false;

        // Launch the decode thread; it begins filling the ring buffer immediately so the
        // first frame is ready by the time Play() is called.
        m_ThreadRunning = true;
        m_DecodeThread = std::thread(&VideoPlayer::DecodeThreadFunc, this);
        return true;
    }

    void VideoPlayer::Unload()
    {
        // Signal and join the decode thread before touching the decoder it owns.
        if (m_ThreadRunning.exchange(false))
        {
            m_QueueCV.notify_all();
            if (m_DecodeThread.joinable())
                m_DecodeThread.join();
        }

        m_Decoder.Close();

        // The decode thread is joined above, so it is safe to tear down the audio stream
        // (the only other writer) here.
        if (m_AudioStream)
        {
            m_AudioStream->Shutdown();
            m_AudioStream.reset();
        }

        {
            std::scoped_lock lock(m_QueueMutex);
            m_FrameQueue.clear();
        }
        m_CurrentFrame = DecodedFrame{};
        m_FrameDirty = false;
        m_Texture.Destroy();

        m_Loaded = false;
        m_Finished = false;
        m_State = VideoPlaybackState::Stopped;
        m_CurrentTime = 0.0;
        m_Duration = 0.0;
        m_FrameRate = 0.0;
        m_Width = 0;
        m_Height = 0;
        m_DecoderAtEnd = false;
        m_SeekRequested = false;
    }

    void VideoPlayer::Play()
    {
        // Replaying a finished video restarts from the beginning.
        if (m_Finished)
            RestartFromBeginning();
        m_State = VideoPlaybackState::Playing;
        if (m_AudioStream)
            m_AudioStream->Start();
    }

    void VideoPlayer::Pause()
    {
        if (m_State == VideoPlaybackState::Playing)
        {
            m_State = VideoPlaybackState::Paused;
            if (m_AudioStream)
                m_AudioStream->Stop();
        }
    }

    void VideoPlayer::Stop()
    {
        m_State = VideoPlaybackState::Stopped;
        m_Finished = false;
        if (m_AudioStream)
            m_AudioStream->Stop();
        Seek(0.0);
    }

    void VideoPlayer::Seek(f64 timeSeconds)
    {
        f64 target = timeSeconds;
        if (!std::isfinite(target) || target < 0.0)
            target = 0.0;
        if (m_Duration > 0.0 && target > m_Duration)
            target = m_Duration;

        m_CurrentTime = target;
        m_Finished = false;

        if (m_Loaded)
        {
            // The decode thread performs the actual seek (the decoder is single-threaded).
            m_SeekTarget = target;
            m_SeekRequested = true;
            m_DecoderAtEnd = false;
            m_QueueCV.notify_all();
        }
    }

    void VideoPlayer::SetPlaybackSpeed(f32 speed)
    {
        if (!std::isfinite(speed) || speed <= 0.0f)
            speed = 1.0f;
        m_PlaybackSpeed = std::clamp(speed, 0.05f, 8.0f);
    }

    void VideoPlayer::SetVolume(f32 volume)
    {
        if (!std::isfinite(volume))
            volume = 1.0f;
        m_Volume = std::clamp(volume, 0.0f, 1.0f);
        if (m_AudioStream)
            m_AudioStream->SetVolume(m_Volume);
    }

    void VideoPlayer::RestartFromBeginning()
    {
        m_Finished = false;
        Seek(0.0);
    }

    void VideoPlayer::Update(f32 dt)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_Loaded || m_State != VideoPlaybackState::Playing)
            return;

        // A/V sync: when the file has audio, the audio playback position is the master clock
        // so the picture locks to audible sound. Otherwise advance by wall time * speed.
        if (m_AudioStream && m_AudioStream->IsActive())
            m_CurrentTime = m_AudioStream->ClockSeconds();
        else if (std::isfinite(dt) && dt > 0.0f)
            m_CurrentTime = m_CurrentTime.load() + static_cast<f64>(dt) * static_cast<f64>(m_PlaybackSpeed);

        const f64 now = m_CurrentTime.load();

        // Drain every frame whose presentation time has arrived, keeping the newest. This
        // also drops frames if we fell behind (e.g. after a long stall), preventing drift.
        bool selected = false;
        {
            std::scoped_lock lock(m_QueueMutex);
            while (!m_FrameQueue.empty() && m_FrameQueue.front().Timestamp <= now)
            {
                m_CurrentFrame = std::move(m_FrameQueue.front());
                m_FrameQueue.pop_front();
                selected = true;
            }
        }

        if (selected)
        {
            m_FrameDirty = true;
            // Wake the decode thread — we just freed queue capacity.
            m_QueueCV.notify_all();
        }

        // End-of-stream handling: decoder exhausted and every decoded frame consumed.
        bool queueEmpty;
        {
            std::scoped_lock lock(m_QueueMutex);
            queueEmpty = m_FrameQueue.empty();
        }

        if (m_DecoderAtEnd.load() && queueEmpty)
        {
            // With audio, also wait for the audio clock to reach the end so the soundtrack
            // isn't cut off when the last video frame is shown.
            const bool audioDrained = !m_AudioStream || !m_AudioStream->IsActive() || m_Duration <= 0.0 || now >= m_Duration - 0.1;

            if (audioDrained)
            {
                if (m_Looping)
                {
                    RestartFromBeginning();
                }
                else if (!m_Finished)
                {
                    m_Finished = true;
                    m_State = VideoPlaybackState::Stopped;
                    if (m_AudioStream)
                        m_AudioStream->Stop();
                    if (OnFinished)
                        OnFinished();
                }
            }
        }
    }

    void VideoPlayer::UpdateTexture()
    {
        if (!m_FrameDirty || m_CurrentFrame.Data.empty())
            return;

        m_Texture.UpdateFrame(m_CurrentFrame.Data.data(), m_CurrentFrame.Width, m_CurrentFrame.Height);
        m_FrameDirty = false;
    }

    void VideoPlayer::PresentImage(const u8* rgba, u32 width, u32 height)
    {
        Unload();
        if (!rgba || width == 0 || height == 0)
            return;

        m_Width = width;
        m_Height = height;
        m_Duration = 0.0;
        m_CurrentFrame.Data.assign(rgba, rgba + static_cast<sizet>(width) * height * 4u);
        m_CurrentFrame.Width = width;
        m_CurrentFrame.Height = height;
        m_CurrentFrame.Timestamp = 0.0;
        m_FrameDirty = true;

        // Marked loaded + Playing so VideoSystem ticks UpdateTexture() (uploading the image);
        // there is no decode thread and the queue stays empty, so it never "finishes".
        m_Loaded = true;
        m_State = VideoPlaybackState::Playing;
    }

    void VideoPlayer::DecodeThreadFunc()
    {
        VideoFrameInfo info;
        DecodedFrame scratch;
        std::vector<f32> audioSamples;
        f64 audioTimestamp = 0.0;

        // pl_mpeg emits MP2 audio in fixed 1152-frame blocks; only decode one when that many
        // frames of ring space are free, so a write never partially fails (no pending buffer).
        constexpr sizet kAudioBlockFrames = 1152;

        while (m_ThreadRunning.load())
        {
            // 1. Service a pending seek before producing more.
            if (m_SeekRequested.exchange(false))
            {
                const f64 target = m_SeekTarget.load();
                m_Decoder.Seek(target);
                {
                    std::scoped_lock lock(m_QueueMutex);
                    m_FrameQueue.clear();
                }
                if (m_AudioStream)
                    m_AudioStream->Flush(target);
                m_DecoderAtEnd = false;
            }

            bool didWork = false;

            // 2. Keep the audio ring topped up — its playback position is the master clock.
            if (m_AudioStream && m_AudioStream->IsActive())
            {
                while (m_AudioStream->WritableFrames() >= kAudioBlockFrames)
                {
                    if (!m_Decoder.DecodeAudioSamples(audioSamples, audioTimestamp) || audioSamples.empty())
                        break;
                    m_AudioStream->Write(audioSamples.data(), audioSamples.size() / 2u); // interleaved stereo
                    didWork = true;
                }
            }

            // 3. Keep the video frame queue topped up.
            bool videoRoom;
            {
                std::scoped_lock lock(m_QueueMutex);
                videoRoom = m_FrameQueue.size() < s_MaxQueuedFrames;
            }
            if (!m_DecoderAtEnd.load() && videoRoom)
            {
                if (m_Decoder.DecodeNextFrame(scratch.Data, info))
                {
                    scratch.Timestamp = info.Timestamp;
                    scratch.Width = info.Width;
                    scratch.Height = info.Height;
                    {
                        std::scoped_lock lock(m_QueueMutex);
                        m_FrameQueue.push_back(std::move(scratch));
                    }
                    scratch = DecodedFrame{};
                    didWork = true;
                }
                else
                {
                    m_DecoderAtEnd = true;
                }
            }

            // 4. Nothing produced this pass — wait briefly for the consumer, a seek, or stop.
            if (!didWork)
            {
                std::unique_lock lock(m_QueueMutex);
                m_QueueCV.wait_for(lock, std::chrono::milliseconds(5), [this]
                                   { return !m_ThreadRunning.load() || m_SeekRequested.load(); });
            }
        }
    }
} // namespace OloEngine

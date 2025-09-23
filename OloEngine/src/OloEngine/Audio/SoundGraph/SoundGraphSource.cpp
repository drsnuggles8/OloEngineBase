#include "OloEnginePCH.h"
#include "SoundGraphSource.h"

namespace OloEngine::Audio::SoundGraph
{
	// Static vtable initialization
	ma_data_source_vtable SoundGraphSource::s_DataSourceVTable = {};

	SoundGraphSource::SoundGraphSource(Ref<SoundGraph> soundGraph)
		: m_SoundGraph(soundGraph)
	{
		// Initialize the vtable if not already done
		if (s_DataSourceVTable.onRead == nullptr)
		{
			InitializeVTable();
		}

		// Initialize the data source base
		ma_data_source_config dataSourceConfig = ma_data_source_config_init();
		dataSourceConfig.vtable = &s_DataSourceVTable;
		ma_data_source_init(&dataSourceConfig, &m_DataSourceBase);

		// Set up channel buffers
		m_ChannelBuffers.resize(m_Channels);
	}

	SoundGraphSource::~SoundGraphSource()
	{
		Uninitialize();
	}

	void SoundGraphSource::InitializeVTable()
	{
		s_DataSourceVTable.onRead = ReadPCMFrames;
		s_DataSourceVTable.onSeek = Seek;
		s_DataSourceVTable.onGetDataFormat = GetDataFormat;
		s_DataSourceVTable.onGetCursor = GetCursor;
		s_DataSourceVTable.onGetLength = GetLength;
	}

	ma_result SoundGraphSource::Initialize(ma_engine* engine)
	{
		if (m_IsInitialized)
		{
			OLO_CORE_WARN("[SoundGraphSource] Already initialized");
			return MA_INVALID_OPERATION;
		}

		if (!engine)
		{
			OLO_CORE_ERROR("[SoundGraphSource] Invalid engine");
			return MA_INVALID_ARGS;
		}

		m_Engine = engine;
		m_SampleRate = ma_engine_get_sample_rate(engine);

		// Initialize the sound graph with the engine's sample rate
		if (m_SoundGraph)
		{
			m_SoundGraph->Initialize(static_cast<f64>(m_SampleRate), 512);
		}

		// Initialize the sound object with our custom data source
		ma_sound_config soundConfig = ma_sound_config_init();
		soundConfig.pDataSource = reinterpret_cast<ma_data_source*>(this);
		soundConfig.flags = MA_SOUND_FLAG_NO_SPATIALIZATION; // For now, disable spatialization

		ma_result result = ma_sound_init_ex(m_Engine, &soundConfig, &m_Sound);
		if (result != MA_SUCCESS)
		{
			OLO_CORE_ERROR("[SoundGraphSource] Failed to initialize sound: {}", static_cast<int>(result));
			return result;
		}

		m_IsInitialized = true;
		OLO_CORE_TRACE("[SoundGraphSource] Initialized successfully");
		return MA_SUCCESS;
	}

	void SoundGraphSource::Uninitialize()
	{
		if (!m_IsInitialized)
			return;

		if (m_IsPlaying)
		{
			Stop();
		}

		ma_sound_uninit(&m_Sound);
		m_IsInitialized = false;
		m_Engine = nullptr;

		OLO_CORE_TRACE("[SoundGraphSource] Uninitialized");
	}

	ma_result SoundGraphSource::Play()
	{
		if (!m_IsInitialized)
		{
			OLO_CORE_ERROR("[SoundGraphSource] Not initialized");
			return MA_INVALID_OPERATION;
		}

		if (m_SoundGraph)
		{
			m_SoundGraph->Play();
		}

		ma_result result = ma_sound_start(&m_Sound);
		if (result == MA_SUCCESS)
		{
			m_IsPlaying = true;
			OLO_CORE_TRACE("[SoundGraphSource] Started playback");
		}
		else
		{
			OLO_CORE_ERROR("[SoundGraphSource] Failed to start playback: {}", static_cast<int>(result));
		}

		return result;
	}

	ma_result SoundGraphSource::Stop()
	{
		if (!m_IsInitialized)
		{
			OLO_CORE_ERROR("[SoundGraphSource] Not initialized");
			return MA_INVALID_OPERATION;
		}

		if (m_SoundGraph)
		{
			m_SoundGraph->Stop();
		}

		ma_result result = ma_sound_stop(&m_Sound);
		if (result == MA_SUCCESS)
		{
			m_IsPlaying = false;
			m_CurrentFrame = 0;
			OLO_CORE_TRACE("[SoundGraphSource] Stopped playback");
		}
		else
		{
			OLO_CORE_ERROR("[SoundGraphSource] Failed to stop playback: {}", static_cast<int>(result));
		}

		return result;
	}

	ma_result SoundGraphSource::Pause()
	{
		if (!m_IsInitialized)
		{
			OLO_CORE_ERROR("[SoundGraphSource] Not initialized");
			return MA_INVALID_OPERATION;
		}

		ma_result result = ma_sound_stop(&m_Sound);
		if (result == MA_SUCCESS)
		{
			m_IsPlaying = false;
			OLO_CORE_TRACE("[SoundGraphSource] Paused playback");
		}
		else
		{
			OLO_CORE_ERROR("[SoundGraphSource] Failed to pause playback: {}", static_cast<int>(result));
		}

		return result;
	}

	bool SoundGraphSource::IsPlaying() const
	{
		return m_IsPlaying && m_IsInitialized;
	}

	//==============================================================================
	// MiniaAudio data source callbacks

	ma_result SoundGraphSource::ReadPCMFrames(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead)
	{
		SoundGraphSource* source = reinterpret_cast<SoundGraphSource*>(pDataSource);
		
		if (!source || !source->m_SoundGraph || !pFramesOut)
		{
			if (pFramesRead) *pFramesRead = 0;
			return MA_INVALID_ARGS;
		}

		// Ensure we have enough buffer space
		u32 totalSamples = static_cast<u32>(frameCount * source->m_Channels);
		if (source->m_TempBuffer.size() < totalSamples)
		{
			source->m_TempBuffer.resize(totalSamples);
		}

		// Set up channel pointers
		for (u32 ch = 0; ch < source->m_Channels; ++ch)
		{
			source->m_ChannelBuffers[ch] = &source->m_TempBuffer[ch * frameCount];
		}

		// Process the sound graph
		source->m_SoundGraph->Process(
			source->m_ChannelBuffers.data(), 
			source->m_ChannelBuffers.data(), 
			static_cast<u32>(frameCount)
		);

		// Interleave the audio data for MiniaAudio
		f32* output = static_cast<f32*>(pFramesOut);
		for (u64 frame = 0; frame < frameCount; ++frame)
		{
			for (u32 ch = 0; ch < source->m_Channels; ++ch)
			{
				output[frame * source->m_Channels + ch] = source->m_ChannelBuffers[ch][frame];
			}
		}

		source->m_CurrentFrame += frameCount;
		
		if (pFramesRead)
		{
			*pFramesRead = frameCount;
		}

		return MA_SUCCESS;
	}

	ma_result SoundGraphSource::Seek(ma_data_source* pDataSource, ma_uint64 frameIndex)
	{
		SoundGraphSource* source = reinterpret_cast<SoundGraphSource*>(pDataSource);
		
		if (!source)
		{
			return MA_INVALID_ARGS;
		}

		// For sound graphs, seeking might not make sense in many cases
		// For now, just reset to the beginning if frameIndex is 0
		if (frameIndex == 0)
		{
			source->m_CurrentFrame = 0;
			// Could trigger a reset event on the sound graph here
			return MA_SUCCESS;
		}

		// For non-zero seeks, we'll return not implemented for now
		// This could be extended based on specific sound graph needs
		return MA_NOT_IMPLEMENTED;
	}

	ma_result SoundGraphSource::GetDataFormat(ma_data_source* pDataSource, ma_format* pFormat, ma_uint32* pChannels, ma_uint32* pSampleRate, ma_channel* pChannelMap, size_t channelMapCap)
	{
		SoundGraphSource* source = reinterpret_cast<SoundGraphSource*>(pDataSource);
		
		if (!source)
		{
			return MA_INVALID_ARGS;
		}

		if (pFormat)
		{
			*pFormat = source->m_Format;
		}

		if (pChannels)
		{
			*pChannels = source->m_Channels;
		}

		if (pSampleRate)
		{
			*pSampleRate = source->m_SampleRate;
		}

		if (pChannelMap && channelMapCap >= source->m_Channels)
		{
			// Set up standard stereo channel mapping
			if (source->m_Channels >= 1) pChannelMap[0] = MA_CHANNEL_LEFT;
			if (source->m_Channels >= 2) pChannelMap[1] = MA_CHANNEL_RIGHT;
		}

		return MA_SUCCESS;
	}

	ma_result SoundGraphSource::GetCursor(ma_data_source* pDataSource, ma_uint64* pCursor)
	{
		SoundGraphSource* source = reinterpret_cast<SoundGraphSource*>(pDataSource);
		
		if (!source || !pCursor)
		{
			return MA_INVALID_ARGS;
		}

		*pCursor = source->m_CurrentFrame;
		return MA_SUCCESS;
	}

	ma_result SoundGraphSource::GetLength(ma_data_source* pDataSource, ma_uint64* pLength)
	{
		// Sound graphs typically have indefinite length (they generate audio)
		// Return unknown length
		if (pLength)
		{
			*pLength = 0; // 0 indicates unknown/infinite length
		}
		
		return MA_SUCCESS;
	}

}
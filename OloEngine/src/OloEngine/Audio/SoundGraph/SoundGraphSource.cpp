#include "OloEnginePCH.h"#include "OloEnginePCH.h"

#include "SoundGraphSource.h"#include "SoundGraphSource.h"



namespace OloEngine::Audio::SoundGraphnamespace OloEngine::Audio::SoundGraph

{{

    SoundGraphSource::SoundGraphSource()	// Static vtable initialization

    {	ma_data_source_vtable SoundGraphSource::s_DataSourceVTable = {};

    }

	SoundGraphSource::SoundGraphSource(Ref<SoundGraph> soundGraph)

    SoundGraphSource::~SoundGraphSource()		: m_SoundGraph(soundGraph)

    {	{

        ReleaseResources();		// Initialize the vtable if not already done

    }		if (s_DataSourceVTable.onRead == nullptr)

		{

    //==============================================================================			InitializeVTable();

    bool SoundGraphSource::Init(u32 sampleRate, u32 maxBlockSize)		}

    {

        m_SampleRate = sampleRate;		// Initialize the data source base

        m_BlockSize = maxBlockSize;		ma_data_source_config dataSourceConfig = ma_data_source_config_init();

        		dataSourceConfig.vtable = &s_DataSourceVTable;

        return true;		ma_data_source_init(&dataSourceConfig, &m_DataSourceBase);

    }

		// Set up channel buffers

    void SoundGraphSource::ProcessBlock(const f32** inputFrames, f32** outputFrames, u32 frameCount)		m_ChannelBuffers.resize(m_Channels);

    {	}

        if (m_Suspended.load() || !m_Graph)

        {	SoundGraphSource::~SoundGraphSource()

            // Output silence	{

            for (u32 channel = 0; channel < 2; ++channel)		Uninitialize();

            {	}

                std::memset(outputFrames[channel], 0, frameCount * sizeof(f32));

            }	void SoundGraphSource::InitializeVTable()

            return;	{

        }		s_DataSourceVTable.onRead = ReadPCMFrames;

		s_DataSourceVTable.onSeek = Seek;

        // Handle play/stop requests		s_DataSourceVTable.onGetDataFormat = GetDataFormat;

        if (m_PlayRequested.load())		s_DataSourceVTable.onGetCursor = GetCursor;

        {		s_DataSourceVTable.onGetLength = GetLength;

            m_IsPlaying = true;	}

            m_PlayRequested = false;

            	ma_result SoundGraphSource::Initialize(ma_engine* engine)

            // Trigger play event on graph	{

            if (m_Graph)		if (m_IsInitialized)

            {		{

                m_Graph->Play();			OLO_CORE_WARN("[SoundGraphSource] Already initialized");

            }			return MA_INVALID_OPERATION;

        }		}



        if (m_StopRequested.load())		if (!engine)

        {		{

            m_IsPlaying = false;			OLO_CORE_ERROR("[SoundGraphSource] Invalid engine");

            m_StopRequested = false;			return MA_INVALID_ARGS;

            		}

            // Trigger stop event on graph

            if (m_Graph)		m_Engine = engine;

            {		m_SampleRate = ma_engine_get_sample_rate(engine);

                m_Graph->Stop();

            }		// Initialize the sound graph with the engine's sample rate

        }		if (m_SoundGraph)

		{

        // Update parameters if needed			m_SoundGraph->Initialize(static_cast<f64>(m_SampleRate), 512);

        UpdateChangedParameters();		}



        // Process the sound graph		// Initialize the sound object with our custom data source

        if (m_IsPlaying.load() && m_Graph)		ma_sound_config soundConfig = ma_sound_config_init();

        {		soundConfig.pDataSource = reinterpret_cast<ma_data_source*>(this);

            m_Graph->Process(const_cast<f32**>(inputFrames), outputFrames, frameCount);		soundConfig.flags = MA_SOUND_FLAG_NO_SPATIALIZATION; // For now, disable spatialization

        }

        else		ma_result result = ma_sound_init_ex(m_Engine, &soundConfig, &m_Sound);

        {		if (result != MA_SUCCESS)

            // Output silence		{

            for (u32 channel = 0; channel < 2; ++channel)			OLO_CORE_ERROR("[SoundGraphSource] Failed to initialize sound: {}", static_cast<int>(result));

            {			return result;

                std::memset(outputFrames[channel], 0, frameCount * sizeof(f32));		}

            }

        }		m_IsInitialized = true;

		OLO_CORE_TRACE("[SoundGraphSource] Initialized successfully");

        // Update frame counter		return MA_SUCCESS;

        m_CurrentFrame += frameCount;	}

    }

	void SoundGraphSource::Uninitialize()

    void SoundGraphSource::ReleaseResources()	{

    {		if (!m_IsInitialized)

        m_Graph = nullptr;			return;

        m_IsPlaying = false;

        m_IsFinished = false;		if (m_IsPlaying)

        m_CurrentFrame = 0;		{

    }			Stop();

		}

    void SoundGraphSource::SuspendProcessing(bool shouldBeSuspended)

    {		ma_sound_uninit(&m_Sound);

        m_Suspended = shouldBeSuspended;		m_IsInitialized = false;

    }		m_Engine = nullptr;



    //==============================================================================		OLO_CORE_TRACE("[SoundGraphSource] Uninitialized");

    void SoundGraphSource::Update(f32 deltaTime)	}

    {

        // Handle outgoing events from the graph	ma_result SoundGraphSource::Play()

        HandleOutgoingEvents();	{

        		if (!m_IsInitialized)

        // Check if graph has finished playing		{

        if (m_Graph && m_IsPlaying.load() && !m_Graph->IsPlaying())			OLO_CORE_ERROR("[SoundGraphSource] Not initialized");

        {			return MA_INVALID_OPERATION;

            m_IsPlaying = false;		}

            m_IsFinished = true;

        }		if (m_SoundGraph)

    }		{

			m_SoundGraph->Play();

    //==============================================================================		}

    bool SoundGraphSource::InitializeDataSources(const std::vector<AssetHandle>& dataSources)

    {		ma_result result = ma_sound_start(&m_Sound);

        // TODO: Initialize wave sources and other data sources for the graph		if (result == MA_SUCCESS)

        OLO_CORE_INFO("SoundGraphSource: Initialized {} data sources", dataSources.size());		{

        return true;			m_IsPlaying = true;

    }			OLO_CORE_TRACE("[SoundGraphSource] Started playback");

		}

    void SoundGraphSource::UninitializeDataSources()		else

    {		{

        // TODO: Clean up data sources			OLO_CORE_ERROR("[SoundGraphSource] Failed to start playback: {}", static_cast<int>(result));

        OLO_CORE_INFO("SoundGraphSource: Uninitialized data sources");		}

    }

		return result;

    void SoundGraphSource::ReplaceGraph(const Ref<SoundGraph>& newGraph)	}

    {

        m_Graph = newGraph;	ma_result SoundGraphSource::Stop()

        	{

        if (m_Graph)		if (!m_IsInitialized)

        {		{

            m_Graph->Initialize(m_SampleRate, m_BlockSize);			OLO_CORE_ERROR("[SoundGraphSource] Not initialized");

            UpdateParameterSet();			return MA_INVALID_OPERATION;

        }		}

    }

		if (m_SoundGraph)

    //==============================================================================		{

    bool SoundGraphSource::SetParameter(const std::string& parameterName, const ValueView& value)			m_SoundGraph->Stop();

    {		}

        auto it = m_ParameterNameToHandle.find(parameterName);

        if (it == m_ParameterNameToHandle.end())		ma_result result = ma_sound_stop(&m_Sound);

        {		if (result == MA_SUCCESS)

            OLO_CORE_WARN("SoundGraphSource: Parameter '{}' not found", parameterName);		{

            return false;			m_IsPlaying = false;

        }			m_CurrentFrame = 0;

			OLO_CORE_TRACE("[SoundGraphSource] Stopped playback");

        return SetParameter(it->second, value);		}

    }		else

		{

    bool SoundGraphSource::SetParameter(u32 parameterID, const ValueView& value)			OLO_CORE_ERROR("[SoundGraphSource] Failed to stop playback: {}", static_cast<int>(result));

    {		}

        if (!m_Graph)

        {		return result;

            OLO_CORE_WARN("SoundGraphSource: Cannot set parameter - no graph loaded");	}

            return false;

        }	ma_result SoundGraphSource::Pause()

	{

        // TODO: Queue parameter change for audio thread		if (!m_IsInitialized)

        // For now, apply directly (not thread-safe but functional)		{

        			OLO_CORE_ERROR("[SoundGraphSource] Not initialized");

        std::lock_guard<std::mutex> lock(m_PresetMutex);			return MA_INVALID_OPERATION;

        // TODO: Update parameter in preset and mark as pending		}

        m_PresetUpdatePending = true;

		ma_result result = ma_sound_stop(&m_Sound);

        return true;		if (result == MA_SUCCESS)

    }		{

			m_IsPlaying = false;

    bool SoundGraphSource::ApplyParameterPreset(const SoundGraphPatchPreset& preset)			OLO_CORE_TRACE("[SoundGraphSource] Paused playback");

    {		}

        std::lock_guard<std::mutex> lock(m_PresetMutex);		else

        m_PendingPreset = preset;		{

        m_PresetUpdatePending = true;			OLO_CORE_ERROR("[SoundGraphSource] Failed to pause playback: {}", static_cast<int>(result));

        		}

        return true;

    }		return result;

	}

    //==============================================================================

    bool SoundGraphSource::SendPlayEvent()	bool SoundGraphSource::IsPlaying() const

    {	{

        m_PlayRequested = true;		return m_IsPlaying && m_IsInitialized;

        return true;	}

    }

	//==============================================================================

    bool SoundGraphSource::SendStopEvent()	// MiniaAudio data source callbacks

    {

        m_StopRequested = true;	ma_result SoundGraphSource::ReadPCMFrames(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead)

        return true;	{

    }		SoundGraphSource* source = reinterpret_cast<SoundGraphSource*>(pDataSource);

		

    //==============================================================================		if (!source || !source->m_SoundGraph || !pFramesOut)

    void SoundGraphSource::UpdateParameterSet()		{

    {			if (pFramesRead) *pFramesRead = 0;

        if (!m_Graph)			return MA_INVALID_ARGS;

            return;		}



        m_ParameterHandles.clear();		// Ensure we have enough buffer space

        m_ParameterNameToHandle.clear();		u32 totalSamples = static_cast<u32>(frameCount * source->m_Channels);

		if (source->m_TempBuffer.size() < totalSamples)

        // TODO: Scan graph for input parameters and build mapping		{

        // This would examine the graph's input endpoints and create			source->m_TempBuffer.resize(totalSamples);

        // parameter handles for runtime modification		}

    }

		// Set up channel pointers

    void SoundGraphSource::UpdateChangedParameters()		for (u32 ch = 0; ch < source->m_Channels; ++ch)

    {		{

        if (!m_PresetUpdatePending.load())			source->m_ChannelBuffers[ch] = &source->m_TempBuffer[ch * frameCount];

            return;		}



        std::lock_guard<std::mutex> lock(m_PresetMutex);		// Process the sound graph

        if (!m_PresetUpdatePending)		source->m_SoundGraph->Process(

            return;			source->m_ChannelBuffers.data(), 

			source->m_ChannelBuffers.data(), 

        // TODO: Apply pending parameter changes to the graph			static_cast<u32>(frameCount)

        // This should happen on the audio thread safely		);



        m_PresetUpdatePending = false;		// Interleave the audio data for MiniaAudio

        m_PresetInitialized = true;		f32* output = static_cast<f32*>(pFramesOut);

    }		for (u64 frame = 0; frame < frameCount; ++frame)

		{

    void SoundGraphSource::HandleOutgoingEvents()			for (u32 ch = 0; ch < source->m_Channels; ++ch)

    {			{

        std::lock_guard<std::mutex> lock(m_EventQueueMutex);				output[frame * source->m_Channels + ch] = source->m_ChannelBuffers[ch][frame];

        			}

        // Process all pending events		}

        for (auto& event : m_PendingEvents)

        {		source->m_CurrentFrame += frameCount;

            event();		

        }		if (pFramesRead)

        m_PendingEvents.clear();		{

    }			*pFramesRead = frameCount;

		}

} // namespace OloEngine::Audio::SoundGraph
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
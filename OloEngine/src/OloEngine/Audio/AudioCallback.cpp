#include "OloEnginePCH.h"
#include "AudioCallback.h"

namespace OloEngine::Audio
{
	static void processing_node_process_pcm_frames(ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut, ma_uint32* pFrameCountOut)
	{
		AudioCallback::processing_node* node = (AudioCallback::processing_node*)pNode;

		AudioCallback* callback = node->callback;
		if (callback && !callback->IsSuspended())
		{
			ma_silence_pcm_frames(ppFramesOut[0], *pFrameCountOut, ma_format_f32, 2);
			callback->ProcessBlockBase(ppFramesIn, pFrameCountIn, ppFramesOut, pFrameCountOut);
		}
		else
		{
			ma_silence_pcm_frames(ppFramesOut[0], *pFrameCountOut, ma_format_f32, 2);
		}
	}

	bool AudioCallback::Initialize(ma_engine* engine, const BusConfig& busConfig)
	{
		m_Engine = engine;
		m_BusConfig = busConfig;

		u32 sampleRate = engine->pDevice->sampleRate;
		u32 blockSize = engine->pDevice->playback.internalPeriodSizeInFrames;

		OLO_CORE_ASSERT(!busConfig.InputBuses.empty() && !busConfig.OutputBuses.empty(), "Bus config must have input and output buses");
		for (const auto& bus : busConfig.InputBuses)  
			OLO_CORE_ASSERT(bus > 0, "Input bus channel count must be > 0");
		for (const auto& bus : busConfig.OutputBuses) 
			OLO_CORE_ASSERT(bus > 0, "Output bus channel count must be > 0");

		if (m_Node.bInitialized)
		{
			ma_node_set_state(&m_Node, ma_node_state_stopped);
			ma_node_uninit(&m_Node, nullptr);
			m_Node.bInitialized = false;
		}

		m_Node.callback = this;
		m_Node.pEngine = m_Engine;

		// Initialize node with required layout
		processing_node_vtable.onProcess = processing_node_process_pcm_frames;
		processing_node_vtable.inputBusCount = (ma_uint8)busConfig.InputBuses.size();
		processing_node_vtable.outputBusCount = (ma_uint8)busConfig.OutputBuses.size();
		
		ma_result result;

		ma_node_config nodeConfig = ma_node_config_init();
		nodeConfig.initialState = ma_node_state_stopped;

		nodeConfig.pInputChannels = busConfig.InputBuses.data();
		nodeConfig.pOutputChannels = busConfig.OutputBuses.data();
		nodeConfig.vtable = &processing_node_vtable;
		result = ma_node_init(&engine->nodeGraph, &nodeConfig, nullptr, &m_Node);

		OLO_CORE_ASSERT(result == MA_SUCCESS, "Failed to initialize miniaudio node");
		m_Node.bInitialized = true;

		result = ma_node_attach_output_bus(&m_Node, 0u, &engine->nodeGraph.endpoint, 0u);
		OLO_CORE_ASSERT(result == MA_SUCCESS, "Failed to attach output bus");

		m_IsInitialized = true;
		return InitBase(sampleRate, blockSize, busConfig);
	}

	void AudioCallback::Uninitialize()
	{
		if (m_Node.bInitialized)
		{
			ma_node_set_state(&m_Node, ma_node_state_stopped);
			ma_node_uninit(&m_Node, nullptr);
			m_Node.bInitialized = false;
			m_Node.pEngine = nullptr;
			m_Node.callback = nullptr;
		}

		m_IsInitialized = false;
		ReleaseResources();
	}

	bool AudioCallback::StartNode()
	{
		if (m_Node.bInitialized)
			ma_node_set_state(&m_Node, ma_node_state_started);

		return m_Node.bInitialized;
	}

} // namespace OloEngine::Audio
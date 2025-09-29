#include "OloEnginePCH.h"
#include "SoundGraphSource.h"
#include "OloEngine/Audio/AudioLoader.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Core/Hash.h"
#include <chrono>
#include <thread>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// DataSourceContext Implementation

	bool DataSourceContext::InitializeWaveSource(AssetHandle handle)
	{
		if (WaveSources.find(handle) != WaveSources.end())
			return true; // Already initialized

		// Load the audio asset
		auto audioAsset = AssetManager::GetAsset<AudioFile>(handle);
		if (!audioAsset)
		{
			OLO_CORE_ERROR("[SoundGraphSource] Failed to load audio asset: {0}", handle);
			return false;
		}

		Audio::WaveSource waveSource{};
		waveSource.WaveHandle = handle;
		waveSource.WaveName = ""; // audioAsset->GetPath().c_str(); // For debugging - needs proper API
		
		// Extract metadata from the audio asset to set up the wave source properly
		// This prevents AreAllSourcesAtEnd() from immediately reporting finished due to TotalFrames = 0
		const double duration = audioAsset->GetDuration();
		const u32 sampleRate = audioAsset->GetSamplingRate();
		const u16 numChannels = audioAsset->GetNumChannels();
		
		// Calculate total frames from duration and sample rate
		waveSource.TotalFrames = static_cast<i64>(duration * sampleRate);
		
		// Store additional metadata that may be needed for audio processing
		// Note: WaveSource doesn't have explicit fields for these, but they're available in the AudioFile
		// If needed later, these can be accessed via AssetManager::GetAsset<AudioFile>(handle)
		
		// Set up refill callback - we'll handle this through the SoundGraphSource
		// The actual refill will be managed by the parent class
		
		WaveSources[handle] = std::move(waveSource);
		return true;
	}

	void DataSourceContext::UninitializeWaveSource(AssetHandle handle)
	{
		auto it = WaveSources.find(handle);
		if (it != WaveSources.end())
		{
			it->second.Clear();
			WaveSources.erase(it);
		}
	}

	void DataSourceContext::UninitializeAll()
	{
		for (auto& [handle, source] : WaveSources)
		{
			source.Clear();
		}
		WaveSources.clear();
	}

	bool DataSourceContext::AreAllSourcesAtEnd() const
	{
		for (const auto& [handle, source] : WaveSources)
		{
			if (source.ReadPosition < source.TotalFrames)
				return false;
		}
		return true;
	}

	//==============================================================================
	/// SoundGraphSource Implementation

	SoundGraphSource::SoundGraphSource()
	{
		// Initialize event queues
		EventMessage defaultMsg{ 0, nullptr };
		m_EventQueue.reset(256, defaultMsg);
		m_MessageQueue.reset(256, defaultMsg);
	}

	SoundGraphSource::~SoundGraphSource()
	{
		Shutdown();
	}

	bool SoundGraphSource::Initialize(ma_engine* engine, u32 sampleRate, u32 maxBlockSize)
	{
		if (m_IsInitialized)
		{
			OLO_CORE_WARN("[SoundGraphSource] Already initialized");
			return false;
		}

		if (!engine)
		{
			OLO_CORE_ERROR("[SoundGraphSource] Invalid audio engine");
			return false;
		}

		m_Engine = engine;
		m_SampleRate = sampleRate;
		m_BlockSize = maxBlockSize;

		// Set up miniaudio engine node
		ma_engine_node_config nodeConfig = ma_engine_node_config_init(
			engine, 
			ma_engine_node_type_group, 
			MA_SOUND_FLAG_NO_SPATIALIZATION
		);

		// Standard stereo configuration
		nodeConfig.channelsIn = 2;
		nodeConfig.channelsOut = 2;

		ma_result result = ma_engine_node_init(&nodeConfig, nullptr, &m_EngineNode);
		if (result != MA_SUCCESS)
		{
			OLO_CORE_ERROR("[SoundGraphSource] Failed to initialize engine node: {0}", (int)result);
			return false;
		}

		// Store reference to this instance in the node for callback access
		// Note: This approach needs to be integrated with a proper miniaudio data source
		// For now, we'll handle processing through external calls

		// Attach to the engine's endpoint for output
		result = ma_node_attach_output_bus(&m_EngineNode, 0, &engine->nodeGraph.endpoint, 0);
		if (result != MA_SUCCESS)
		{
			OLO_CORE_ERROR("[SoundGraphSource] Failed to attach output bus: {0}", (int)result);
			ma_engine_node_uninit(&m_EngineNode, nullptr);
			return false;
		}

		m_IsInitialized = true;

		OLO_CORE_INFO("[SoundGraphSource] Initialized with sample rate: {0}, block size: {1}", sampleRate, maxBlockSize);
		return true;
	}

	void SoundGraphSource::Shutdown()
	{
		if (!m_IsInitialized)
			return;

		SuspendProcessing(true);
		
		// Wait for audio thread to acknowledge suspension before tearing down state
		// This prevents race conditions during destruction
		constexpr auto timeout = std::chrono::milliseconds(100);
		auto startTime = std::chrono::steady_clock::now();
		
		while (!m_Suspended.load() && 
			   (std::chrono::steady_clock::now() - startTime) < timeout)
		{
			std::this_thread::sleep_for(std::chrono::microseconds(100));
		}
		
		// If timeout occurred, log a warning but continue with shutdown
		if (!m_Suspended.load())
		{
			OLO_CORE_WARN("[SoundGraphSource] Timeout waiting for audio thread suspension acknowledgment");
		}
		
		UninitializeDataSources();

		if (m_IsInitialized)
		{
			ma_engine_node_uninit(&m_EngineNode, nullptr);
		}

		m_Engine = nullptr;
		m_Graph = nullptr;
		m_IsInitialized = false;
		m_PresetIsInitialized = false;

		OLO_CORE_INFO("[SoundGraphSource] Shutdown complete");
	}

	void SoundGraphSource::SuspendProcessing(bool shouldBeSuspended)
	{
		if (shouldBeSuspended)
		{
			m_SuspendFlag.SetDirty();
		}
		else
		{
			// Resuming - reset state
			m_IsPlaying.store(false);
			m_CurrentFrame.store(0);
			m_IsFinished.store(false, std::memory_order_relaxed);

			// Clear any pending events by consuming them all
			EventMessage msg;
			while (m_EventQueue.pop(msg));  // Consume all pending events
			
			EventMessage msgData;
			while (m_MessageQueue.pop(msgData)); // Consume all pending messages

			// Reset the suspend flag
			m_SuspendFlag.CheckAndResetIfDirty();
			m_Suspended.store(false);
		}
	}

	void SoundGraphSource::Update(f64 deltaTime)
	{
		(void)deltaTime;
		// Process events from the audio thread
		EventMessage eventMsg;
		while (m_EventQueue.pop(eventMsg))
		{
			if (eventMsg.Callback)
				eventMsg.Callback();
		}

		// Process messages from the audio thread  
		EventMessage messageMsg;
		while (m_MessageQueue.pop(messageMsg))
		{
			if (messageMsg.Callback)
				messageMsg.Callback();
		}

		// Handle automatic suspension when finished
		if (m_IsFinished.load(std::memory_order_relaxed) && m_IsPlaying.load())
		{
			SuspendProcessing(true);
		}
	}

	//==============================================================================
	/// SoundGraph Interface

	bool SoundGraphSource::InitializeDataSources(const std::vector<AssetHandle>& dataSources)
	{
		bool success = true;

		for (AssetHandle handle : dataSources)
		{
			if (!m_DataSources.InitializeWaveSource(handle))
			{
				OLO_CORE_ERROR("[SoundGraphSource] Failed to initialize data source: {0}", handle);
				success = false;
			}
		}

		if (success)
		{
			OLO_CORE_INFO("[SoundGraphSource] Initialized {0} data sources", dataSources.size());
		}

		return success;
	}

	void SoundGraphSource::UninitializeDataSources()
	{
		m_DataSources.UninitializeAll();
	}

	void SoundGraphSource::ReplaceGraph(const Ref<SoundGraph>& newGraph)
	{
		if (newGraph == m_Graph)
			return;

		m_Graph = newGraph;

		if (m_Graph)
		{
			// Set up wave source refill callback
			// Note: This is a simplified approach - in a full implementation you'd need
			// to properly integrate with the sound graph's wave player system
			
			// Initialize the sound graph with our sample rate
			m_Graph->SetSampleRate(static_cast<f32>(m_SampleRate));

			// Update parameter mappings
			UpdateParameterSet();

			OLO_CORE_INFO("[SoundGraphSource] Replaced sound graph");
		}
	}

	//==============================================================================
	/// Parameter Interface

	bool SoundGraphSource::SetParameter(std::string_view parameterName, const choc::value::Value& value)
	{
		if (!m_Graph || parameterName.empty())
			return false;

		// Hash the parameter name for faster lookup
		u32 parameterID = OloEngine::Hash::GenerateFNVHash(parameterName);
		return SetParameter(parameterID, value);
	}

	bool SoundGraphSource::SetParameter(u32 parameterID, const choc::value::Value& value)
	{
		(void)parameterID; (void)value;
		auto it = m_ParameterHandles.find(parameterID);
		if (it == m_ParameterHandles.end())
		{
			OLO_CORE_WARN("[SoundGraphSource] Parameter ID {0} not found", parameterID);
			return false;
		}

		// For now, we'll apply parameters immediately
		// In a full implementation, you'd want thread-safe parameter updates
		if (m_Graph)
		{
			// This would need to be adapted to OloEngine's parameter system
			// return m_Graph->SetParameterValue(it->second.Handle, value);
			
			// Placeholder - needs integration with sound graph parameter system
			OLO_CORE_TRACE("[SoundGraphSource] Setting parameter {0} to value", it->second.Name);
			return true;
		}

		return false;
	}

	bool SoundGraphSource::ApplyParameterPreset(const SoundGraphPatchPreset& preset)
	{
		if (!m_Graph)
		{
			OLO_CORE_ERROR("[SoundGraphSource] No sound graph loaded");
			return false;
		}

		// Set the preset for thread-safe access from audio thread
		m_ThreadSafePreset.SetPreset(preset);

		OLO_CORE_TRACE("[SoundGraphSource] Applied parameter preset with {0} parameters", 
			preset.GetParameterCount());
		return true;
	}

	//==============================================================================
	/// Playback Interface

	int SoundGraphSource::GetNumDataSources() const
	{
		return m_DataSources.GetSourceCount();
	}

	bool SoundGraphSource::AreAllDataSourcesAtEnd()
	{
		return m_DataSources.AreAllSourcesAtEnd();
	}

	bool SoundGraphSource::SendPlayEvent()
	{
		if (!m_Graph)
			return false;

		m_PlayRequestFlag.SetDirty();
		return true;
	}

	//==============================================================================
	/// Internal Methods

	void SoundGraphSource::UpdateParameterSet()
	{
		if (!m_Graph)
			return;

		m_ParameterHandles.clear();

		// Get parameter endpoints from the sound graph
		// This would need to be adapted to OloEngine's sound graph parameter system
		// auto parameters = m_Graph->GetParameterEndpoints();
		
		// For now, create placeholder parameter mapping
		// In a real implementation, you'd iterate through the sound graph's exposed parameters
		
		OLO_CORE_TRACE("[SoundGraphSource] Updated parameter set with {0} parameters", 
			m_ParameterHandles.size());
	}

	bool SoundGraphSource::ApplyParameterPresetInternal()
	{
		if (!m_Graph)
			return false;

		// For now, we'll skip the preset application since it needs proper integration
		// In a real implementation, you'd:
		// 1. Get the preset from the thread-safe container
		// 2. Apply each parameter to the sound graph
		// 3. Track which parameters were successfully set

		// Placeholder - just mark as initialized
		return true;
	}

	void SoundGraphSource::UpdateChangedParameters()
	{
		// Handle any parameter changes that occurred since last audio block
		// This would integrate with OloEngine's parameter change detection system
	}

	//==============================================================================
	/// Audio Processing

	void SoundGraphSource::ProcessSamples(float** ppFramesOut, u32 frameCount)
	{
		// Handle suspension
		if (m_SuspendFlag.CheckAndResetIfDirty())
		{
			m_Suspended.store(true);
			m_IsPlaying.store(false);
		}

		if (!m_Graph || IsSuspended())
		{
			// Silence each channel independently for planar buffers
			for (u32 channel = 0; channel < 2; ++channel)
			{
				if (ppFramesOut[channel])
				{
					ma_silence_pcm_frames(ppFramesOut[channel], frameCount, ma_format_f32, 1);
				}
			}
			return;
		}

		// Apply parameter presets if needed
		if (!m_PresetIsInitialized)
		{
			if (ApplyParameterPresetInternal())
			{
				m_PresetIsInitialized = true;
			}
			else
			{
				// If preset application failed, output silence
				// Silence each channel independently for planar buffers
				for (u32 channel = 0; channel < 2; ++channel)
				{
					if (ppFramesOut[channel])
					{
						ma_silence_pcm_frames(ppFramesOut[channel], frameCount, ma_format_f32, 1);
					}
				}
				return;
			}
		}

		// Handle play requests
		if (m_PlayRequestFlag.CheckAndResetIfDirty())
		{
			// Send play event to sound graph
			if (m_Graph->SendInputEvent(SoundGraph::IDs::Play, choc::value::createFloat32(1.0f)))
			{
				m_CurrentFrame.store(0);
				m_IsPlaying.store(true);
				m_IsFinished.store(false, std::memory_order_relaxed);
			}
		}

		// Process the sound graph
		if (m_PresetIsInitialized && m_Graph->IsPlayable())
		{
			// Begin processing block (refill wave player buffers)
			m_Graph->BeginProcessBlock();

			// Process each frame
			for (u32 frame = 0; frame < frameCount; ++frame)
			{
				// Process the graph for this frame
				m_Graph->Process();

				// Copy output channels to the output buffer
				u32 outputChannels = std::min(2u, (u32)m_Graph->out_Channels.size());
				for (u32 channel = 0; channel < outputChannels; ++channel)
				{
					ppFramesOut[channel][frame] = m_Graph->out_Channels[channel];
				}

				// Handle mono to stereo conversion if needed
				if (outputChannels == 1 && ppFramesOut[1])
				{
					ppFramesOut[1][frame] = ppFramesOut[0][frame];
				}
			}

			// Handle outgoing events and messages
			m_Graph->HandleOutgoingEvents(this, HandleGraphEvent, HandleGraphMessage);

			// Update frame counter
			m_CurrentFrame.fetch_add(frameCount, std::memory_order_relaxed);

			// Check if playback should finish
			if (AreAllDataSourcesAtEnd())
			{
				m_IsFinished.store(true, std::memory_order_relaxed);
			}
		}
		else
		{
			// Silence each channel independently for planar buffers
			for (u32 channel = 0; channel < 2; ++channel)
			{
				if (ppFramesOut[channel])
				{
					ma_silence_pcm_frames(ppFramesOut[channel], frameCount, ma_format_f32, 1);
				}
			}
		}
	}

	//==============================================================================
	/// Static Callbacks

	void SoundGraphSource::HandleGraphEvent(void* context, u64 frameIndex, Identifier endpointID, const choc::value::ValueView& eventData)
	{
		auto* source = static_cast<SoundGraphSource*>(context);
		if (!source)
			return;

		// Queue the event for processing on the main thread
		if (source->m_OnGraphEvent)
		{
			// Allocate/copy the payload into an owned Value on the audio thread for thread safety
			// ValueView points to scratch storage that will be invalid on the main thread
			choc::value::Value ownedEventData = choc::value::Value(eventData);
			
			EventMessage msg;
			msg.FrameIndex = frameIndex;
			msg.Callback = [source, frameIndex, endpointID, ownedEventData = std::move(ownedEventData)]()
			{
				// Convert to u32 for the callback (legacy compatibility)
				u32 endpointIDu32 = static_cast<u32>(endpointID);
				
				// Use the owned, thread-safe copy of the event data
				source->m_OnGraphEvent(frameIndex, endpointIDu32, ownedEventData);
			};

			source->m_EventQueue.push(std::move(msg));
		}
	}

	void SoundGraphSource::HandleGraphMessage(void* context, u64 frameIndex, const char* message)
	{
		auto* source = static_cast<SoundGraphSource*>(context);
		if (!source || !message)
			return;

		// Queue the message for processing on the main thread
		if (source->m_OnGraphMessage)
		{
			// Create a copy of the message for thread safety
			std::string messageCopy(message);
			EventMessage msg;
			msg.FrameIndex = frameIndex;
			msg.Callback = [source, frameIndex, messageCopy]()
			{
				source->m_OnGraphMessage(frameIndex, messageCopy.c_str());
			};

			source->m_MessageQueue.push(std::move(msg));
		}
	}

	bool SoundGraphSource::RefillWaveSourceCallback(Audio::WaveSource& waveSource, void* userData)
	{
		auto* source = static_cast<SoundGraphSource*>(userData);
		if (!source)
			return false;

		// This would need integration with OloEngine's audio loading system
		// to refill the wave source buffer from the asset
		
		if (waveSource.WaveHandle == 0)
			return false;

		// Placeholder - in a real implementation you'd:
		// 1. Get the audio asset from the handle
		// 2. Read the next block of samples
		// 3. Fill the waveSource.Channels buffer
		// 4. Update waveSource.ReadPosition

		return true;
	}

} // namespace OloEngine::Audio::SoundGraph
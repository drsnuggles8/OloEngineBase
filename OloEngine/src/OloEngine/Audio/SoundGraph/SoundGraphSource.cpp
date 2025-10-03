#include "OloEnginePCH.h"
#include "SoundGraphSource.h"
#include "OloEngine/Audio/AudioLoader.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Core/Hash.h"
#include "OloEngine/Project/Project.h"
#include <chrono>
#include <thread>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// DataSourceContext Implementation

	bool DataSourceContext::InitializeWaveSource(AssetHandle handle)
	{
		OLO_PROFILE_FUNCTION();
		
		if (m_WaveSources.find(handle) != m_WaveSources.end())
			return true; // Already initialized

		// Load the audio asset
		auto audioAsset = AssetManager::GetAsset<AudioFile>(handle);
		if (!audioAsset)
		{
			OLO_CORE_ERROR("[SoundGraphSource] Failed to load audio asset: {0}", handle);
			return false;
		}

		Audio::WaveSource waveSource{};
		waveSource.m_WaveHandle = handle;
		waveSource.m_WaveName = ""; // audioAsset->GetPath().c_str(); // For debugging - needs proper API
		
		// Extract metadata from the audio asset to set up the wave source properly
		// This prevents AreAllSourcesAtEnd() from immediately reporting finished due to TotalFrames = 0
		const double duration = audioAsset->GetDuration();
		const u32 sampleRate = audioAsset->GetSamplingRate();
		const u16 numChannels = audioAsset->GetNumChannels();
		
		// Calculate total frames from duration and sample rate
		waveSource.m_TotalFrames = static_cast<i64>(duration * sampleRate);
		
		// Store additional metadata that may be needed for audio processing
		// Note: WaveSource doesn't have explicit fields for these, but they're available in the AudioFile
		// If needed later, these can be accessed via AssetManager::GetAsset<AudioFile>(handle)
		
		// Set up refill callback - we'll handle this through the SoundGraphSource
		// The actual refill will be managed by the parent class
		
		m_WaveSources[handle] = std::move(waveSource);
		return true;
	}

	void DataSourceContext::UninitializeWaveSource(AssetHandle handle)
	{
		auto it = m_WaveSources.find(handle);
		if (it != m_WaveSources.end())
		{
			it->second.Clear();
			m_WaveSources.erase(it);
		}
	}

	void DataSourceContext::UninitializeAll()
	{
		for (auto& [handle, source] : m_WaveSources)
		{
			source.Clear();
		}
		m_WaveSources.clear();
	}

	bool DataSourceContext::AreAllSourcesAtEnd() const
	{
		for (const auto& [handle, source] : m_WaveSources)
		{
			if (source.m_ReadPosition < source.m_TotalFrames)
				return false;
		}
		return true;
	}

	//==============================================================================
	/// SoundGraphSource Implementation

	u64 SoundGraphSource::GetMaxTotalFrames() const
	{
		u64 maxFrames = 0;
		for (const auto& [handle, source] : m_DataSources.m_WaveSources)
		{
			if (source.m_TotalFrames > static_cast<i64>(maxFrames))
				maxFrames = static_cast<u64>(source.m_TotalFrames);
		}
		return maxFrames;
	}

	SoundGraphSource::SoundGraphSource()
	{
		// Event queues are pre-allocated in their constructors - no initialization needed
		// The lock-free queues have no dynamic allocation
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

		ma_engine_node_uninit(&m_EngineNode, nullptr);

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
			m_IsPlaying.store(false, std::memory_order_relaxed);
			m_CurrentFrame.store(0, std::memory_order_relaxed);
			m_IsFinished.store(false, std::memory_order_relaxed);

			// Clear any pending events by consuming them all
			Audio::AudioThreadEvent event;
			while (m_EventQueue.Pop(event));  // Consume all pending events
			
			Audio::AudioThreadMessage msg;
			while (m_MessageQueue.Pop(msg)); // Consume all pending messages

			// Reset the suspend flag
			m_SuspendFlag.CheckAndResetIfDirty();
			m_Suspended.store(false, std::memory_order_relaxed);
		}
	}

	void SoundGraphSource::Update(f64 deltaTime)
	{
		(void)deltaTime;
		// Process events from the audio thread
		Audio::AudioThreadEvent event;
		while (m_EventQueue.Pop(event))
		{
			// Call the event callback with the pre-allocated value data
			if (m_OnGraphEvent)
			{
				// We're on the main thread now - safe to allocate
				// Convert ValueView to Value for the callback
				choc::value::Value value = event.ValueData.GetValue();
				m_OnGraphEvent(event.FrameIndex, event.EndpointID, value);
			}
		}

		// Process messages from the audio thread  
		Audio::AudioThreadMessage msg;
		while (m_MessageQueue.Pop(msg))
		{
			// Call the message callback with the pre-allocated text
			if (m_OnGraphMessage)
			{
				m_OnGraphMessage(msg.FrameIndex, msg.Text);
			}
		}

		// Handle automatic suspension when finished
		if (m_IsFinished.load(std::memory_order_relaxed) && m_IsPlaying.load(std::memory_order_relaxed))
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
		OLO_PROFILE_FUNCTION();

		// Handle suspension
		if (m_SuspendFlag.CheckAndResetIfDirty())
		{
			m_Suspended.store(true, std::memory_order_release);
			m_IsPlaying.store(false, std::memory_order_relaxed);
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

		// Queue the event for processing on the main thread using lock-free pre-allocated storage
		// This is real-time safe - no allocations, no locks, no blocking
		Audio::AudioThreadEvent event;
		event.FrameIndex = frameIndex;
		event.EndpointID = static_cast<u32>(endpointID);
		
		// Copy the event data into pre-allocated inline storage
		// This avoids heap allocation while remaining thread-safe
		if (!event.ValueData.CopyFrom(eventData))
		{
			// Value was too large for inline storage - this is rare for audio events
			// Log a warning but don't allocate (keep it real-time safe)
			// The event will be dropped, which is better than causing audio glitches
			return;
		}
		
		// Push to the lock-free queue (wait-free operation)
		if (!source->m_EventQueue.Push(event))
		{
			// Queue is full - event dropped
			// This is preferable to blocking or allocating in the audio thread
		}
	}

	void SoundGraphSource::HandleGraphMessage(void* context, u64 frameIndex, const char* message)
	{
		auto* source = static_cast<SoundGraphSource*>(context);
		if (!source || !message)
			return;

		// Queue the message for processing on the main thread using lock-free pre-allocated storage
		// This is real-time safe - no allocations, no locks, no blocking
		Audio::AudioThreadMessage msg;
		msg.FrameIndex = frameIndex;
		msg.SetText(message);  // Copies into pre-allocated buffer
		
		// Push to the lock-free queue (wait-free operation)
		if (!source->m_MessageQueue.Push(msg))
		{
			// Queue is full - message dropped
			// This is preferable to blocking or allocating in the audio thread
		}
	}

	bool SoundGraphSource::RefillWaveSourceCallback(Audio::WaveSource& waveSource, void* userData)
	{
		auto* source = static_cast<SoundGraphSource*>(userData);
		if (!source)
			return false;

		if (waveSource.m_WaveHandle == 0)
			return false;

		// Get the audio asset metadata
		AssetMetadata metadata = AssetManager::GetAssetMetadata(waveSource.m_WaveHandle);
		if (!metadata.IsValid())
		{
			OLO_CORE_ERROR("[SoundGraphSource] Invalid asset metadata for handle: {}", waveSource.m_WaveHandle);
			return false;
		}

		// Build the full file path
		std::filesystem::path filePath = Project::GetAssetDirectory() / metadata.FilePath;
		if (!std::filesystem::exists(filePath))
		{
			OLO_CORE_ERROR("[SoundGraphSource] Audio file does not exist: {}", filePath.string());
			return false;
		}

		// Load audio data from file
		// Note: This loads the entire file which is not ideal for large files
		// A production implementation would use streaming decoders or a cache
		AudioData audioData;
		if (!AudioLoader::LoadAudioFile(filePath, audioData))
		{
			OLO_CORE_ERROR("[SoundGraphSource] Failed to load audio file: {}", filePath.string());
			return false;
		}

		// Calculate how many frames we can refill
		const i64 remainingFrames = waveSource.m_TotalFrames - waveSource.m_ReadPosition;
		if (remainingFrames <= 0)
			return false; // Already at end of audio

		// Determine how many frames to push into the circular buffer
		// We'll push up to the buffer capacity or what's remaining, whichever is smaller
		constexpr sizet bufferFrameCapacity = 1920; // MonoCircularBuffer<f32, 1920 * 2> / 2 channels
		const i64 framesToPush = std::min(static_cast<i64>(bufferFrameCapacity), remainingFrames);

		// Validate we don't exceed the audio data bounds
		if (waveSource.m_ReadPosition + framesToPush > static_cast<i64>(audioData.m_NumFrames))
		{
			OLO_CORE_ERROR("[SoundGraphSource] Read position out of bounds");
			return false;
		}

		// Copy samples from audio data into the circular buffer
		// Audio data is interleaved (L,R,L,R...), same as the circular buffer expects
		const i64 startSampleIndex = waveSource.m_ReadPosition * audioData.m_NumChannels;
		const i64 numSamplesToPush = framesToPush * audioData.m_NumChannels;

		// Push samples into the circular buffer
		waveSource.m_Channels.PushMultiple(&audioData.m_Samples[startSampleIndex], static_cast<int>(numSamplesToPush));

		// Update read position
		waveSource.m_ReadPosition += framesToPush;

		return true;
	}

} // namespace OloEngine::Audio::SoundGraph
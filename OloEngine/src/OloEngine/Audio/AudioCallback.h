#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Audio/SampleBufferOperations.h"
#include <miniaudio.h>
#include <choc/audio/choc_SampleBuffers.h>
#include <choc/audio/choc_SampleBufferUtilities.h>

#include <vector>
#include <functional>
#include <atomic>
#include <cstring> // for std::memset

namespace OloEngine::Audio
{
	/// Basic audio callback interface for OloEngine
	/// Simplified version of Hazel's AudioCallback adapted for OloEngine's architecture
	class AudioCallback
	{
	public:
		AudioCallback() = default;
		virtual ~AudioCallback() = default;
		AudioCallback(AudioCallback&&) = default;
		AudioCallback& operator=(AudioCallback&&) = default;

		AudioCallback(const AudioCallback&) = delete;
		AudioCallback& operator=(const AudioCallback&) = delete;

		struct BusConfig
		{
			std::vector<u32> m_InputBuses;
			std::vector<u32> m_OutputBuses;
		};

		ma_node_base* GetNode() { return &m_Node.m_Base; }

		bool Initialize(ma_engine* engine, const BusConfig& busConfig);
		void Uninitialize();
		bool StartNode();

		virtual void SuspendProcessing(bool shouldBeSuspended) = 0;
		virtual bool IsSuspended() = 0;

	protected:
		virtual bool InitBase(u32 sampleRate, u32 maxBlockSize, const BusConfig& busConfig) = 0;
		virtual void ProcessBlockBase(const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut, ma_uint32* pFrameCountOut) = 0;
		virtual void ReleaseResources() = 0;

		BusConfig m_BusConfig;
		ma_engine* m_Engine = nullptr;
		u32 m_MaxBlockSize = 0; // Maximum frames per block, set during initialization

	private:
		bool m_IsInitialized = false;
		ma_node_vtable m_VTable = {};

		friend void processing_node_process_pcm_frames(ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn,
			float** ppFramesOut, ma_uint32* pFrameCountOut);

		struct processing_node
		{
			// Magic constant for runtime type validation
			static constexpr u32 s_MagicTypeId = 0x414F4C4F; // "AOLO" in hex (Audio OLO)
			
			ma_node_base m_Base;
			u32 m_TypeId = 0; // Will be set to s_MagicTypeId when properly initialized
			ma_engine* m_PEngine;
			bool m_BInitialized = false;
			AudioCallback* m_Callback;
		} m_Node;
	};

	/// CRTP template for interleaved audio processing
	template<typename T>
	class AudioCallbackInterleaved : public AudioCallback
	{
	public:
		virtual ~AudioCallbackInterleaved() = default;

	protected:
		bool InitBase(u32 sampleRate, u32 maxBlockSize, const BusConfig& busConfig) final 
		{ 
			return Underlying().Init(sampleRate, maxBlockSize, busConfig); 
		}

	private:
		// CRTP interface
		AudioCallbackInterleaved() = default;
		friend T;
		T& Underlying() { return static_cast<T&>(*this); }
		const T& Underlying() const { return static_cast<T const&>(*this); }

		void ProcessBlockBase(const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut, ma_uint32* pFrameCountOut) final
		{
			Underlying().ProcessBlock(ppFramesIn, pFrameCountIn, ppFramesOut, pFrameCountOut);
		}
	};

	/// CRTP template for deinterleaved audio processing
	template<typename T>
	class AudioCallbackDeinterleaved : public AudioCallback
	{
	public:
		virtual ~AudioCallbackDeinterleaved() = default;

	protected:
		bool InitBase(u32 sampleRate, u32 maxBlockSize, const BusConfig& busConfig) final
		{
			m_InDeinterleavedBuses.resize(busConfig.m_InputBuses.size());
			for (sizet i = 0; i < m_InDeinterleavedBuses.size(); ++i)
			{
				m_InDeinterleavedBuses[i].resize({ busConfig.m_InputBuses[i], maxBlockSize });
				m_InDeinterleavedBuses[i].clear();
			}

			m_OutDeinterleavedBuses.resize(busConfig.m_OutputBuses.size());
			for (sizet i = 0; i < m_OutDeinterleavedBuses.size(); ++i)
			{
				m_OutDeinterleavedBuses[i].resize({ busConfig.m_OutputBuses[i], maxBlockSize });
				m_OutDeinterleavedBuses[i].clear();
			}

			return Underlying().Init(sampleRate, maxBlockSize, busConfig);
		}

		void ClearBuffer()
		{
			m_InDeinterleavedBuses.clear();
			m_OutDeinterleavedBuses.clear();
		}

	private:
		// CRTP interface
		AudioCallbackDeinterleaved() = default;
		friend T;
		T& Underlying() { return static_cast<T&>(*this); }
		const T& Underlying() const { return static_cast<T const&>(*this); }

	void ProcessBlockBase(const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut, ma_uint32* pFrameCountOut) final
	{
		OLO_PROFILE_FUNCTION();
		
		// Cache a safe frame count - miniaudio can pass pFrameCountIn as nullptr for source-style nodes
		ma_uint32 frameCount = pFrameCountIn ? *pFrameCountIn : (pFrameCountOut ? *pFrameCountOut : 0);
		
		// Real-time safety: Verify frameCount doesn't exceed pre-allocated buffer size
		// If this assertion fires, the audio device is requesting more frames than we allocated in InitBase
		OLO_CORE_ASSERT(frameCount <= m_MaxBlockSize, 
			"Audio callback requested {} frames but only {} were pre-allocated. This should never happen.", 
			frameCount, m_MaxBlockSize);
		
		// Early return if frame count is invalid to prevent buffer overruns
		if (frameCount > m_MaxBlockSize || frameCount == 0)
		{
			if (pFrameCountOut) *pFrameCountOut = 0;
			return;
		}
		
		if (ppFramesIn != nullptr)
		{
			// Use actual deinterleaved buffer count for safety, not m_BusConfig size
			for (sizet i = 0; i < m_InDeinterleavedBuses.size(); ++i)
			{
				// Verify buffer was pre-allocated to correct size (no resize in real-time thread!)
				OLO_CORE_ASSERT(m_InDeinterleavedBuses[i].getSize().numFrames >= frameCount,
					"Input buffer {} has {} frames but {} requested. Buffer should be pre-allocated to maxBlockSize.",
					i, m_InDeinterleavedBuses[i].getSize().numFrames, frameCount);
				
				// Only deinterleave if the input pointer is non-null
				if (ppFramesIn[i] != nullptr)
				{
					// Deinterleave only the requested frames (buffer is pre-allocated to maxBlockSize)
					SampleBufferOperations::Deinterleave(m_InDeinterleavedBuses[i], ppFramesIn[i], frameCount);
				}
				else
				{
					// Clear only the frames we're using (real-time safe)
					auto numChannels = m_InDeinterleavedBuses[i].getNumChannels();
					auto* channelPtrs = m_InDeinterleavedBuses[i].getView().data.channels;
					for (u32 ch = 0; ch < numChannels; ++ch)
						std::memset(channelPtrs[ch], 0, frameCount * sizeof(float));
				}
			}
		}
		else
		{
			// Clear all input buses when no input provided (only active frames)
			for (sizet i = 0; i < m_InDeinterleavedBuses.size(); ++i)
			{
				auto numChannels = m_InDeinterleavedBuses[i].getNumChannels();
				auto* channelPtrs = m_InDeinterleavedBuses[i].getView().data.channels;
				for (u32 ch = 0; ch < numChannels; ++ch)
					std::memset(channelPtrs[ch], 0, frameCount * sizeof(float));
			}
		}

		// Verify output buffers are pre-allocated (no resize in real-time thread!)
		for (sizet i = 0; i < m_OutDeinterleavedBuses.size(); ++i)
		{
			OLO_CORE_ASSERT(m_OutDeinterleavedBuses[i].getSize().numFrames >= frameCount,
				"Output buffer {} has {} frames but {} requested. Buffer should be pre-allocated to maxBlockSize.",
				i, m_OutDeinterleavedBuses[i].getSize().numFrames, frameCount);
		}

		Underlying().ProcessBlock(m_InDeinterleavedBuses, m_OutDeinterleavedBuses, frameCount);

		// Use actual deinterleaved buffer count and check for null output pointers
		if (ppFramesOut != nullptr)
		{
			for (sizet i = 0; i < m_OutDeinterleavedBuses.size(); ++i)
			{
				// Only interleave if the output pointer is non-null
				if (ppFramesOut[i] != nullptr)
				{
					// Interleave only the requested frames (buffer is pre-allocated to maxBlockSize)
					SampleBufferOperations::Interleave(ppFramesOut[i], m_OutDeinterleavedBuses[i], frameCount);
				}
			}
		}
		
		// Set output frame count
		if (pFrameCountOut != nullptr)
		{
			*pFrameCountOut = frameCount;
		}
	}
	
	private:
		std::vector<choc::buffer::ChannelArrayBuffer<float>> m_InDeinterleavedBuses;
		std::vector<choc::buffer::ChannelArrayBuffer<float>> m_OutDeinterleavedBuses;
	};

	/// Function-based deinterleaved callback
	class CallbackBindedDeinterleaved : public AudioCallbackDeinterleaved<CallbackBindedDeinterleaved>
	{
	public:
		CallbackBindedDeinterleaved() = default;
		virtual ~CallbackBindedDeinterleaved() = default;

		void SuspendProcessing(bool shouldBeSuspended) final { m_Suspended.store(shouldBeSuspended); }
		bool IsSuspended() final { return m_Suspended.load(); }

		std::function<void(const std::vector<choc::buffer::ChannelArrayBuffer<float>>& inBuffer,
			std::vector<choc::buffer::ChannelArrayBuffer<float>>& outBuffer)> onAudioCallback;

	private:
		friend AudioCallbackDeinterleaved<CallbackBindedDeinterleaved>;

		virtual void ReleaseResources() override {}
		
		// CRTP interface requirement: Init() must be implemented to satisfy the base class contract.
		// Returns true immediately as configuration happens via onAudioCallback member.
		// Parameters unused because this callback type delegates all processing to the bound function.
		bool Init([[maybe_unused]] u32 sampleRate, [[maybe_unused]] u32 maxBlockSize, [[maybe_unused]] const BusConfig& config)
		{
			return true;
		}
		
		void ProcessBlock(const std::vector<choc::buffer::ChannelArrayBuffer<float>>& inBuffer, 
			std::vector<choc::buffer::ChannelArrayBuffer<float>>& outBuffer, u32 numFramesRequested)
		{
			if (onAudioCallback && !m_Suspended.load())
			{
				onAudioCallback(inBuffer, outBuffer);
			}
			else
			{
				// Clear output buffer to prevent stale samples when callback is null or suspended
				// Real-time safety: Use pre-allocated buffers and clear only the needed frames
				for (auto& channelBuffer : outBuffer)
				{
					// Verify buffer was pre-allocated to sufficient size
					OLO_CORE_ASSERT(channelBuffer.getSize().numFrames >= numFramesRequested,
						"Output buffer has {} frames but {} requested. Should be pre-allocated to maxBlockSize.",
						channelBuffer.getSize().numFrames, numFramesRequested);
					
					// Clear only the frames we need (real-time safe, no allocation)
					auto numChannels = channelBuffer.getNumChannels();
					auto* channelPtrs = channelBuffer.getView().data.channels;
					for (u32 ch = 0; ch < numChannels; ++ch)
						std::memset(channelPtrs[ch], 0, numFramesRequested * sizeof(float));
				}
			}
		}

	private:
		std::atomic<bool> m_Suspended = false;
	};

	/// Function-based interleaved callback
	class CallbackBindedInterleaved : public AudioCallbackInterleaved<CallbackBindedInterleaved>
	{
	public:
		CallbackBindedInterleaved() = default;
		virtual ~CallbackBindedInterleaved() = default;

		void SuspendProcessing(bool shouldBeSuspended) final { m_Suspended.store(shouldBeSuspended); }
		bool IsSuspended() final { return m_Suspended.load(); }

		std::function<void(const float** ppFramesIn, ma_uint32* pFrameCountIn,
			float** ppFramesOut, ma_uint32* pFrameCountOut, const BusConfig& busConfig)> onAudioCallback;

	private:
		friend AudioCallbackInterleaved<CallbackBindedInterleaved>;

		virtual void ReleaseResources() override {}
		bool Init(u32 sampleRate, u32 maxBlockSize, const BusConfig& config)
		{
			(void)sampleRate;
			(void)maxBlockSize;
			(void)config;
			return true;
		}

		void ProcessBlock(const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut, ma_uint32* pFrameCountOut)
		{
			OLO_PROFILE_FUNCTION();

			// Derive requested frame count (0 if pFrameCountIn is null)
			ma_uint32 requestedFrames = (pFrameCountIn != nullptr) ? *pFrameCountIn : 0;
			
			// Always set output frame count first
			if (pFrameCountOut != nullptr)
			{
				*pFrameCountOut = requestedFrames;
			}
			
			if (onAudioCallback && !m_Suspended.load())
			{
				// Pass pFrameCountOut to callback so it can set the produced frame count
				onAudioCallback(ppFramesIn, pFrameCountIn, ppFramesOut, pFrameCountOut, m_BusConfig);
			}
			else
			{
				// Clear output buffers to prevent stale samples when callback is null or suspended
				if (ppFramesOut != nullptr && requestedFrames > 0)
				{
					// Zero all output channels for the requested frames
					for (u32 busIndex = 0; busIndex < m_BusConfig.m_OutputBuses.size(); ++busIndex)
					{
						if (ppFramesOut[busIndex] != nullptr)
						{
							u32 channelCount = m_BusConfig.m_OutputBuses[busIndex];
							u32 totalSamples = channelCount * requestedFrames;
							std::memset(ppFramesOut[busIndex], 0, totalSamples * sizeof(float));
						}
					}
				}
			}
		}

	private:
		std::atomic<bool> m_Suspended = false;
	};

} // namespace OloEngine::Audio
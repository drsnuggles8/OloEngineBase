#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Audio/SampleBufferOperations.h"
#include <miniaudio.h>
#include <choc/audio/choc_SampleBuffers.h>
#include <choc/audio/choc_SampleBufferUtilities.h>

#include <vector>
#include <functional>
#include <atomic>

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
			std::vector<u32> InputBuses;
			std::vector<u32> OutputBuses;
		};

		ma_node_base* GetNode() { return &m_Node.base; }

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

	private:
		bool m_IsInitialized = false;
		ma_node_vtable m_VTable = {};

		friend void processing_node_process_pcm_frames(ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn,
			float** ppFramesOut, ma_uint32* pFrameCountOut);

		struct processing_node
		{
			ma_node_base base;
			ma_engine* pEngine;
			bool bInitialized = false;
			AudioCallback* callback;
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
			m_InDeinterleavedBuses.resize(busConfig.InputBuses.size());
			for (size_t i = 0; i < m_InDeinterleavedBuses.size(); ++i)
			{
				m_InDeinterleavedBuses[i].resize({ busConfig.InputBuses[i], maxBlockSize });
				m_InDeinterleavedBuses[i].clear();
			}

			m_OutDeinterleavedBuses.resize(busConfig.OutputBuses.size());
			for (size_t i = 0; i < m_OutDeinterleavedBuses.size(); ++i)
			{
				m_OutDeinterleavedBuses[i].resize({ busConfig.OutputBuses[i], maxBlockSize });
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
		// Cache a safe frame count - miniaudio can pass pFrameCountIn as nullptr for source-style nodes
		ma_uint32 frameCount = pFrameCountIn ? *pFrameCountIn : (pFrameCountOut ? *pFrameCountOut : 0);
		
		if (ppFramesIn != nullptr)
		{
			// Use actual deinterleaved buffer count for safety, not m_BusConfig size
			for (size_t i = 0; i < m_InDeinterleavedBuses.size(); ++i)
			{
				// Ensure deinterleaved buffer is sized to frameCount
				if (m_InDeinterleavedBuses[i].getSize().numFrames != frameCount)
				{
					m_InDeinterleavedBuses[i].resize({ m_InDeinterleavedBuses[i].getSize().numChannels, frameCount });
				}
				
				// Only deinterleave if the input pointer is non-null
				if (ppFramesIn[i] != nullptr)
				{
					// Deinterleave input audio from interleaved format
					// Buffer is already resized to correct frame count above
					SampleBufferOperations::Deinterleave(m_InDeinterleavedBuses[i], ppFramesIn[i]);
				}
				else
				{
					m_InDeinterleavedBuses[i].clear();
				}
			}
		}
		else
		{
			// Clear all input buses when no input provided
			for (size_t i = 0; i < m_InDeinterleavedBuses.size(); ++i)
				m_InDeinterleavedBuses[i].clear();
		}

		// Ensure output buffers are properly sized before processing
		for (size_t i = 0; i < m_OutDeinterleavedBuses.size(); ++i)
		{
			if (m_OutDeinterleavedBuses[i].getSize().numFrames != frameCount)
			{
				m_OutDeinterleavedBuses[i].resize({ m_OutDeinterleavedBuses[i].getSize().numChannels, frameCount });
			}
		}

		Underlying().ProcessBlock(m_InDeinterleavedBuses, m_OutDeinterleavedBuses, frameCount);

		// Use actual deinterleaved buffer count and check for null output pointers
		if (ppFramesOut != nullptr)
		{
			for (size_t i = 0; i < m_OutDeinterleavedBuses.size(); ++i)
			{
				// Only interleave if the output pointer is non-null
				if (ppFramesOut[i] != nullptr)
				{
					// Interleave output audio to interleaved format
					// Buffer already has correct frame count from processing
					SampleBufferOperations::Interleave(ppFramesOut[i], m_OutDeinterleavedBuses[i]);
				}
			}
		}
		
		// Set output frame count
		if (pFrameCountOut != nullptr)
		{
			*pFrameCountOut = frameCount;
		}
	}	private:
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
		bool Init(u32 sampleRate, u32 maxBlockSize, const BusConfig& config) { 
			(void)sampleRate; (void)maxBlockSize; (void)config; // Suppress unreferenced parameter warnings
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
				for (auto& channelBuffer : outBuffer)
				{
					// Set frame count and zero the samples
					channelBuffer.resize({ channelBuffer.getNumChannels(), numFramesRequested });
					channelBuffer.clear();
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
		bool Init(u32 sampleRate, u32 maxBlockSize, const BusConfig& config) { (void)sampleRate; (void)maxBlockSize; (void)config; return true; }
		void ProcessBlock(const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut, ma_uint32* pFrameCountOut)
		{
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
					for (u32 busIndex = 0; busIndex < m_BusConfig.OutputBuses.size(); ++busIndex)
					{
						if (ppFramesOut[busIndex] != nullptr)
						{
							u32 channelCount = m_BusConfig.OutputBuses[busIndex];
							u32 totalSamples = channelCount * requestedFrames;
							memset(ppFramesOut[busIndex], 0, totalSamples * sizeof(float));
						}
					}
				}
			}
		}

	private:
		std::atomic<bool> m_Suspended = false;
	};

} // namespace OloEngine::Audio
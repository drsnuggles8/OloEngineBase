#pragma once

#include "OloEngine/Core/Base.h"
#include "SoundGraph.h"
#include <miniaudio.h>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// SoundGraphSource - MiniaAudio data source that processes sound graphs
	class SoundGraphSource
	{
	public:
		explicit SoundGraphSource(Ref<SoundGraph> soundGraph);
		~SoundGraphSource();

		// MiniaAudio data source interface
		static ma_result ReadPCMFrames(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead);
		static ma_result Seek(ma_data_source* pDataSource, ma_uint64 frameIndex);
		static ma_result GetDataFormat(ma_data_source* pDataSource, ma_format* pFormat, ma_uint32* pChannels, ma_uint32* pSampleRate, ma_channel* pChannelMap, size_t channelMapCap);
		static ma_result GetCursor(ma_data_source* pDataSource, ma_uint64* pCursor);
		static ma_result GetLength(ma_data_source* pDataSource, ma_uint64* pLength);

		// Initialize this source for use with MiniaAudio
		ma_result Initialize(ma_engine* engine);
		void Uninitialize();

		// Playback control
		ma_result Play();
		ma_result Stop();
		ma_result Pause();
		bool IsPlaying() const;

		// Get the underlying sound graph
		Ref<SoundGraph> GetSoundGraph() const { return m_SoundGraph; }

		// Get the MiniaAudio sound handle
		ma_sound* GetSound() { return &m_Sound; }

	private:
		// MiniaAudio data source vtable
		static ma_data_source_vtable s_DataSourceVTable;

		// SoundGraph being processed
		Ref<SoundGraph> m_SoundGraph;

		// MiniaAudio objects
		ma_data_source_base m_DataSourceBase;
		ma_sound m_Sound;
		ma_engine* m_Engine = nullptr;

		// Audio format
		ma_format m_Format = ma_format_f32;
		u32 m_Channels = 2;
		u32 m_SampleRate = 48000;

		// State
		bool m_IsInitialized = false;
		bool m_IsPlaying = false;
		u64 m_CurrentFrame = 0;

		// Audio buffers for processing
		std::vector<f32> m_TempBuffer;
		std::vector<f32*> m_ChannelBuffers;

		// Initialize the data source vtable
		static void InitializeVTable();
	};

}
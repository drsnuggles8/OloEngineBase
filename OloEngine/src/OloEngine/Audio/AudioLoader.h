#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Asset/Asset.h"
#include <vector>
#include <string>
#include <filesystem>
#include <cmath>

namespace OloEngine::Audio
{
	//==============================================================================
	/// Audio data structure for loaded audio files
	struct AudioData
	{
		std::vector<f32> samples;       // Interleaved audio samples (L, R, L, R...)
		u32 numChannels = 0;            // Number of audio channels
		u32 numFrames = 0;              // Total number of frames (not samples)
		f64 sampleRate = 0.0;           // Sample rate in Hz
		f64 duration = 0.0;             // Duration in seconds
		u64 fileSize = 0;               // Original file size in bytes
		
		/// Clear all audio data
		void Clear()
		{
			samples.clear();
			numChannels = 0;
			numFrames = 0;
			sampleRate = 0.0;
			duration = 0.0;
			fileSize = 0;
		}
		
		/// Check if audio data is valid
		bool IsValid() const 
		{ 
			if (samples.empty() || numChannels == 0 || numFrames == 0 || !std::isfinite(sampleRate) || sampleRate <= 0.0)
				return false;
				
			// Verify that samples buffer size matches expected sample count
			const size_t expectedSampleCount = size_t(numFrames) * size_t(numChannels);
			return samples.size() == expectedSampleCount;
		}
		
		/// Get sample at specific frame and channel
		f32 GetSample(u64 frame, u32 channel) const
		{
			if (frame >= numFrames || channel >= numChannels)
				return 0.0f;
			
			u64 sampleIndex = frame * numChannels + channel;
			return (sampleIndex < samples.size()) ? samples[sampleIndex] : 0.0f;
		}
		
		/// Get total number of samples (all channels)
		u64 GetTotalSamples() const { return static_cast<u64>(samples.size()); }
	};

	//==============================================================================
	/// Audio file loader utility using miniaudio
	class AudioLoader
	{
	public:
		/// Load audio file from filesystem path
		/// @param filePath Path to the audio file
		/// @param outAudioData Structure to receive loaded audio data
		/// @return true if loading succeeded, false otherwise
		static bool LoadAudioFile(const std::filesystem::path& filePath, AudioData& outAudioData);
		
		/// Load audio file from memory buffer (future use)
		/// @param data Memory buffer containing audio file data
		/// @param dataSize Size of the memory buffer
		/// @param outAudioData Structure to receive loaded audio data
		/// @return true if loading succeeded, false otherwise
		static bool LoadAudioFromMemory(const void* data, u64 dataSize, AudioData& outAudioData);
		
		/// Get basic audio file information without loading full data
		/// @param filePath Path to the audio file
		/// @param outNumChannels Number of channels (output)
		/// @param outNumFrames Number of frames (output)
		/// @param outSampleRate Sample rate (output)
		/// @param outDuration Duration in seconds (output)
		/// @return true if successful, false otherwise
		static bool GetAudioFileInfo(const std::filesystem::path& filePath, 
			u32& outNumChannels, u32& outNumFrames, f64& outSampleRate, f64& outDuration);
		
		/// Get comprehensive audio file information including bit depth
		/// @param filePath Path to the audio file
		/// @param outNumChannels Number of channels (output)
		/// @param outNumFrames Number of frames (output)
		/// @param outSampleRate Sample rate (output)
		/// @param outDuration Duration in seconds (output)
		/// @param outBitDepth Original bit depth (output)
		/// @return true if successful, false otherwise
		static bool GetAudioFileInfo(const std::filesystem::path& filePath, 
			u32& outNumChannels, u32& outNumFrames, f64& outSampleRate, f64& outDuration, u16& outBitDepth);
		
		/// Check if a file extension is supported
		/// @param extension File extension (e.g., ".wav", ".mp3", ".ogg")
		/// @return true if supported, false otherwise
		static bool IsExtensionSupported(const std::string& extension);
		
		/// Get list of supported file extensions
		/// @return Vector of supported extensions (including the dot)
		static std::vector<std::string> GetSupportedExtensions();

	private:
		AudioLoader() = delete; // Static utility class
	};

} // namespace OloEngine::Audio
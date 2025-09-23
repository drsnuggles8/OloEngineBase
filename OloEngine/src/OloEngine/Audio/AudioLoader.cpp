#include "OloEnginePCH.h"
#include "AudioLoader.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Instrumentor.h"

#include <miniaudio.h>
#include <algorithm>
#include <fstream>

namespace OloEngine::Audio
{
	bool AudioLoader::LoadAudioFile(const std::filesystem::path& filePath, AudioData& outAudioData)
	{
		OLO_PROFILE_FUNCTION();

		// Clear output data
		outAudioData.Clear();

		if (!std::filesystem::exists(filePath))
		{
			OLO_CORE_ERROR("[AudioLoader] File does not exist: {}", filePath.string());
			return false;
		}

		// Get file size
		std::error_code ec;
		outAudioData.fileSize = std::filesystem::file_size(filePath, ec);
		if (ec)
		{
			OLO_CORE_WARN("[AudioLoader] Could not get file size for: {}", filePath.string());
			outAudioData.fileSize = 0;
		}

		// Initialize miniaudio decoder
		ma_decoder decoder;
		ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0); // Let miniaudio determine channels and sample rate
		
		ma_result result = ma_decoder_init_file(filePath.string().c_str(), &config, &decoder);
		if (result != MA_SUCCESS)
		{
			OLO_CORE_ERROR("[AudioLoader] Failed to initialize decoder for file: {} (error: {})", 
				filePath.string(), static_cast<int>(result));
			return false;
		}

		// Get audio file information
		ma_uint64 totalFrames;
		result = ma_decoder_get_length_in_pcm_frames(&decoder, &totalFrames);
		if (result != MA_SUCCESS)
		{
			OLO_CORE_ERROR("[AudioLoader] Failed to get frame count for file: {} (error: {})", 
				filePath.string(), static_cast<int>(result));
			ma_decoder_uninit(&decoder);
			return false;
		}

		// Store audio properties
		outAudioData.numChannels = decoder.outputChannels;
		outAudioData.numFrames = static_cast<u32>(totalFrames);
		outAudioData.sampleRate = static_cast<f64>(decoder.outputSampleRate);
		outAudioData.duration = static_cast<f64>(totalFrames) / outAudioData.sampleRate;

		// Validate audio properties
		if (outAudioData.numChannels == 0 || outAudioData.numFrames == 0 || outAudioData.sampleRate <= 0.0)
		{
			OLO_CORE_ERROR("[AudioLoader] Invalid audio properties for file: {} (channels: {}, frames: {}, sampleRate: {})", 
				filePath.string(), outAudioData.numChannels, outAudioData.numFrames, outAudioData.sampleRate);
			ma_decoder_uninit(&decoder);
			return false;
		}

		// Allocate buffer for audio data
		const u64 totalSamples = static_cast<u64>(outAudioData.numFrames) * outAudioData.numChannels;
		outAudioData.samples.resize(totalSamples);

		// Read audio data
		ma_uint64 framesRead;
		result = ma_decoder_read_pcm_frames(&decoder, outAudioData.samples.data(), totalFrames, &framesRead);
		
		// Clean up decoder
		ma_decoder_uninit(&decoder);

		if (result != MA_SUCCESS || framesRead != totalFrames)
		{
			OLO_CORE_ERROR("[AudioLoader] Failed to read audio data from file: {} (error: {}, frames read: {}/{})", 
				filePath.string(), static_cast<int>(result), framesRead, totalFrames);
			outAudioData.Clear();
			return false;
		}

		OLO_CORE_TRACE("[AudioLoader] Successfully loaded audio file '{}': {} frames, {} channels, {:.2f}s duration, {:.1f}MB", 
			filePath.string(), outAudioData.numFrames, outAudioData.numChannels, outAudioData.duration,
			static_cast<f64>(outAudioData.fileSize) / (1024 * 1024));

		return true;
	}

	bool AudioLoader::LoadAudioFromMemory(const void* data, u64 dataSize, AudioData& outAudioData)
	{
		OLO_PROFILE_FUNCTION();

		// Clear output data
		outAudioData.Clear();

		if (!data || dataSize == 0)
		{
			OLO_CORE_ERROR("[AudioLoader] Invalid memory buffer provided");
			return false;
		}

		outAudioData.fileSize = dataSize;

		// Initialize miniaudio decoder from memory
		ma_decoder decoder;
		ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
		
		ma_result result = ma_decoder_init_memory(data, dataSize, &config, &decoder);
		if (result != MA_SUCCESS)
		{
			OLO_CORE_ERROR("[AudioLoader] Failed to initialize decoder from memory (error: {})", static_cast<int>(result));
			return false;
		}

		// Get audio file information
		ma_uint64 totalFrames;
		result = ma_decoder_get_length_in_pcm_frames(&decoder, &totalFrames);
		if (result != MA_SUCCESS)
		{
			OLO_CORE_ERROR("[AudioLoader] Failed to get frame count from memory (error: {})", static_cast<int>(result));
			ma_decoder_uninit(&decoder);
			return false;
		}

		// Store audio properties
		outAudioData.numChannels = decoder.outputChannels;
		outAudioData.numFrames = static_cast<u32>(totalFrames);
		outAudioData.sampleRate = static_cast<f64>(decoder.outputSampleRate);
		outAudioData.duration = static_cast<f64>(totalFrames) / outAudioData.sampleRate;

		// Validate audio properties
		if (outAudioData.numChannels == 0 || outAudioData.numFrames == 0 || outAudioData.sampleRate <= 0.0)
		{
			OLO_CORE_ERROR("[AudioLoader] Invalid audio properties from memory (channels: {}, frames: {}, sampleRate: {})", 
				outAudioData.numChannels, outAudioData.numFrames, outAudioData.sampleRate);
			ma_decoder_uninit(&decoder);
			return false;
		}

		// Allocate buffer for audio data
		const u64 totalSamples = static_cast<u64>(outAudioData.numFrames) * outAudioData.numChannels;
		outAudioData.samples.resize(totalSamples);

		// Read audio data
		ma_uint64 framesRead;
		result = ma_decoder_read_pcm_frames(&decoder, outAudioData.samples.data(), totalFrames, &framesRead);
		
		// Clean up decoder
		ma_decoder_uninit(&decoder);

		if (result != MA_SUCCESS || framesRead != totalFrames)
		{
			OLO_CORE_ERROR("[AudioLoader] Failed to read audio data from memory (error: {}, frames read: {}/{})", 
				static_cast<int>(result), framesRead, totalFrames);
			outAudioData.Clear();
			return false;
		}

		OLO_CORE_TRACE("[AudioLoader] Successfully loaded audio from memory: {} frames, {} channels, {:.2f}s duration, {:.1f}MB", 
			outAudioData.numFrames, outAudioData.numChannels, outAudioData.duration,
			static_cast<f64>(dataSize) / (1024 * 1024));

		return true;
	}

	bool AudioLoader::GetAudioFileInfo(const std::filesystem::path& filePath, u32& outNumChannels, u32& outNumFrames, f64& outSampleRate, f64& outDuration)
	{
		OLO_PROFILE_FUNCTION();

		if (!std::filesystem::exists(filePath))
		{
			OLO_CORE_ERROR("[AudioLoader] File does not exist: {}", filePath.string());
			return false;
		}

		// Initialize miniaudio decoder
		ma_decoder decoder;
		ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
		
		ma_result result = ma_decoder_init_file(filePath.string().c_str(), &config, &decoder);
		if (result != MA_SUCCESS)
		{
			OLO_CORE_ERROR("[AudioLoader] Failed to initialize decoder for info query: {} (error: {})", 
				filePath.string(), static_cast<int>(result));
			return false;
		}

		// Get audio file information
		ma_uint64 totalFrames;
		result = ma_decoder_get_length_in_pcm_frames(&decoder, &totalFrames);
		if (result != MA_SUCCESS)
		{
			OLO_CORE_ERROR("[AudioLoader] Failed to get frame count for info query: {} (error: {})", 
				filePath.string(), static_cast<int>(result));
			ma_decoder_uninit(&decoder);
			return false;
		}

		// Store audio properties
		outNumChannels = decoder.outputChannels;
		outNumFrames = static_cast<u32>(totalFrames);
		outSampleRate = static_cast<f64>(decoder.outputSampleRate);
		outDuration = static_cast<f64>(totalFrames) / outSampleRate;

		// Clean up decoder
		ma_decoder_uninit(&decoder);

		// Validate audio properties
		if (outNumChannels == 0 || outNumFrames == 0 || outSampleRate <= 0.0)
		{
			OLO_CORE_ERROR("[AudioLoader] Invalid audio properties for file: {} (channels: {}, frames: {}, sampleRate: {})", 
				filePath.string(), outNumChannels, outNumFrames, outSampleRate);
			return false;
		}

		return true;
	}

	bool AudioLoader::IsExtensionSupported(const std::string& extension)
	{
		// Convert to lowercase for case-insensitive comparison
		std::string lowerExt = extension;
		std::transform(lowerExt.begin(), lowerExt.end(), lowerExt.begin(), ::tolower);

		// Common audio formats supported by miniaudio
		static const std::vector<std::string> supportedExtensions = {
			".wav", ".mp3", ".flac", ".ogg", ".m4a", ".aac", ".wma"
		};

		return std::find(supportedExtensions.begin(), supportedExtensions.end(), lowerExt) != supportedExtensions.end();
	}

	std::vector<std::string> AudioLoader::GetSupportedExtensions()
	{
		return {".wav", ".mp3", ".flac", ".ogg", ".m4a", ".aac", ".wma"};
	}

} // namespace OloEngine::Audio
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
		
		auto u8str = filePath.u8string();
		std::string utf8Path(reinterpret_cast<const char*>(u8str.c_str()), u8str.size());
		ma_result result = ma_decoder_init_file(utf8Path.c_str(), &config, &decoder);
		if (result != MA_SUCCESS)
		{
			OLO_CORE_ERROR("[AudioLoader] Failed to initialize decoder for file: {} (error: {})", 
				utf8Path, static_cast<int>(result));
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

		// Check for frame count overflow before casting to u32
		if (totalFrames > UINT32_MAX)
		{
			OLO_CORE_ERROR("[AudioLoader] File has too many frames: {} ({} frames > {} max)", 
				utf8Path, totalFrames, UINT32_MAX);
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
		const u64 maxSamples = static_cast<u64>(outAudioData.samples.max_size());
		if (totalSamples > maxSamples)
		{
			OLO_CORE_ERROR("[AudioLoader] Audio buffer too large for file: {} (samples: {}, max: {})",
				filePath.string(), totalSamples, maxSamples);
			ma_decoder_uninit(&decoder);
			return false;
		}
		outAudioData.samples.resize(static_cast<size_t>(totalSamples));

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

		// Check for frame count overflow before casting to u32
		if (totalFrames > UINT32_MAX)
		{
			OLO_CORE_ERROR("[AudioLoader] Memory audio has too many frames: {} frames > {} max", 
				totalFrames, UINT32_MAX);
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
		const u64 maxSamples = static_cast<u64>(outAudioData.samples.max_size());
		if (totalSamples > maxSamples)
		{
			OLO_CORE_ERROR("[AudioLoader] Audio buffer too large for memory decode (samples: {}, max: {})",
				totalSamples, maxSamples);
			ma_decoder_uninit(&decoder);
			return false;
		}
		outAudioData.samples.resize(static_cast<size_t>(totalSamples));

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
		u16 bitDepth; // Unused in this overload
		return GetAudioFileInfo(filePath, outNumChannels, outNumFrames, outSampleRate, outDuration, bitDepth);
	}

	bool AudioLoader::GetAudioFileInfo(const std::filesystem::path& filePath, u32& outNumChannels, u32& outNumFrames, f64& outSampleRate, f64& outDuration, u16& outBitDepth)
	{
		OLO_PROFILE_FUNCTION();

		if (!std::filesystem::exists(filePath))
		{
			OLO_CORE_ERROR("[AudioLoader] File does not exist: {}", filePath.string());
			return false;
		}

		// Initialize miniaudio decoder for format detection
		ma_decoder decoder;
		ma_decoder_config config = ma_decoder_config_init(ma_format_unknown, 0, 0); // Don't force format conversion
		
		auto u8str = filePath.u8string();
		std::string utf8Path(reinterpret_cast<const char*>(u8str.c_str()), u8str.size());
		ma_result result = ma_decoder_init_file(utf8Path.c_str(), &config, &decoder);
		if (result != MA_SUCCESS)
		{
			OLO_CORE_ERROR("[AudioLoader] Failed to initialize decoder for info query: {} (error: {})", 
				utf8Path, static_cast<int>(result));
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

		// Check for frame count overflow before casting to u32
		if (totalFrames > UINT32_MAX)
		{
			OLO_CORE_ERROR("[AudioLoader] Info query: file has too many frames: {} ({} frames > {} max)", 
				utf8Path, totalFrames, UINT32_MAX);
			ma_decoder_uninit(&decoder);
			return false;
		}

		// Store audio properties
		outNumChannels = decoder.outputChannels;
		outNumFrames = static_cast<u32>(totalFrames);
		outSampleRate = static_cast<f64>(decoder.outputSampleRate);
		outDuration = static_cast<f64>(totalFrames) / outSampleRate;
		
		// Get original bit depth from the decoder format
		switch (decoder.outputFormat)
		{
			case ma_format_u8:  outBitDepth = 8;  break;
			case ma_format_s16: outBitDepth = 16; break;
			case ma_format_s24: outBitDepth = 24; break;
			case ma_format_s32: outBitDepth = 32; break;
			case ma_format_f32: outBitDepth = 32; break; // 32-bit float
			default:
				OLO_CORE_WARN("[AudioLoader] Unknown format for file: {}, defaulting to 16-bit", filePath.string());
				outBitDepth = 16;
				break;
		}

		// Clean up decoder
		ma_decoder_uninit(&decoder);

		// Validate audio properties
		if (outNumChannels == 0 || outNumFrames == 0 || outSampleRate <= 0.0 || outBitDepth == 0)
		{
			OLO_CORE_ERROR("[AudioLoader] Invalid audio properties for file: {} (channels: {}, frames: {}, sampleRate: {}, bitDepth: {})", 
				filePath.string(), outNumChannels, outNumFrames, outSampleRate, outBitDepth);
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
#include "OloEnginePCH.h"
#include "AudioLoader.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Instrumentor.h"

#include <miniaudio.h>
#include <algorithm>

namespace OloEngine::Audio
{
    // Common audio formats supported by miniaudio
    static const std::vector<std::string> s_SupportedExtensions = {
        ".wav", ".mp3", ".flac", ".ogg"
    };

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
        outAudioData.m_FileSize = std::filesystem::file_size(filePath, ec);
        if (ec)
        {
            OLO_CORE_WARN("[AudioLoader] Could not get file size for: {}", filePath.string());
            outAudioData.m_FileSize = 0;
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

        // Decode audio data using common helper
        bool success = DecodeAudioData(decoder, outAudioData, utf8Path);
        
        // Clean up decoder
        ma_decoder_uninit(&decoder);
        
        if (!success)
        {
            outAudioData.Clear();
            return false;
        }

        OLO_CORE_TRACE("[AudioLoader] Successfully loaded audio file '{}': {:.1f}MB file size", 
            filePath.string(), static_cast<f64>(outAudioData.m_FileSize) / (1024 * 1024));

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

        outAudioData.m_FileSize = dataSize;

        // Initialize miniaudio decoder from memory
        ma_decoder decoder;
        ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
        
        ma_result result = ma_decoder_init_memory(data, dataSize, &config, &decoder);
        if (result != MA_SUCCESS)
        {
            OLO_CORE_ERROR("[AudioLoader] Failed to initialize decoder from memory (error: {})", static_cast<int>(result));
            return false;
        }

        // Decode audio data using common helper
        bool success = DecodeAudioData(decoder, outAudioData, "memory");
        
        // Clean up decoder
        ma_decoder_uninit(&decoder);
        
        if (!success)
        {
            outAudioData.Clear();
            return false;
        }

        OLO_CORE_TRACE("[AudioLoader] Successfully loaded audio from memory: {:.1f}MB buffer size", 
            static_cast<f64>(dataSize) / (1024 * 1024));

        return true;
    }

    bool AudioLoader::DecodeAudioData(ma_decoder& decoder, AudioData& outAudioData, const std::string& sourceDescription)
    {
        OLO_PROFILE_FUNCTION();

        // Get audio file information
        ma_uint64 totalFrames = 0;
        ma_result result = ma_decoder_get_length_in_pcm_frames(&decoder, &totalFrames);
        
        bool knownLength = (result == MA_SUCCESS && totalFrames > 0);
        bool streamingMode = (result == MA_NOT_IMPLEMENTED || totalFrames == 0);
        
        if (!knownLength && !streamingMode)
        {
            // Genuine error occurred
            OLO_CORE_ERROR("[AudioLoader] Failed to get frame count for {}: error {}", 
                sourceDescription, static_cast<int>(result));
            return false;
        }

        // Store basic audio properties
        outAudioData.m_NumChannels = decoder.outputChannels;
        outAudioData.m_SampleRate = static_cast<f64>(decoder.outputSampleRate);

        // Validate basic audio properties
        if (outAudioData.m_NumChannels == 0 || outAudioData.m_SampleRate <= 0.0)
        {
            OLO_CORE_ERROR("[AudioLoader] Invalid basic audio properties for {}: channels {}, sampleRate {}", 
                sourceDescription, outAudioData.m_NumChannels, outAudioData.m_SampleRate);
            return false;
        }

        // Handle known length vs streaming mode
        if (knownLength)
        {
            // Known length path - single read
            // Check for frame count overflow before casting to u32
            if (totalFrames > UINT32_MAX)
            {
                OLO_CORE_ERROR("[AudioLoader] {} has too many frames: {} > {} max", 
                    sourceDescription, totalFrames, UINT32_MAX);
                return false;
            }

            outAudioData.m_NumFrames = static_cast<u32>(totalFrames);
            outAudioData.m_Duration = static_cast<f64>(totalFrames) / outAudioData.m_SampleRate;

            // Allocate buffer for audio data
            const u64 totalSamples = static_cast<u64>(outAudioData.m_NumFrames) * outAudioData.m_NumChannels;
            const u64 maxSamples = static_cast<u64>(outAudioData.m_Samples.max_size());
            if (totalSamples > maxSamples)
            {
                OLO_CORE_ERROR("[AudioLoader] Audio buffer too large for {}: samples {} > max {}",
                    sourceDescription, totalSamples, maxSamples);
                return false;
            }
            outAudioData.m_Samples.resize(static_cast<sizet>(totalSamples));

            // Read audio data in one go
            ma_uint64 framesRead;
            result = ma_decoder_read_pcm_frames(&decoder, outAudioData.m_Samples.data(), totalFrames, &framesRead);
            
            if (result != MA_SUCCESS || framesRead != totalFrames)
            {
                OLO_CORE_ERROR("[AudioLoader] Failed to read audio data from {}: error {}, frames read {}/{}", 
                    sourceDescription, static_cast<int>(result), framesRead, totalFrames);
                outAudioData.Clear();
                return false;
            }
        }
        else
        {
            // Streaming mode - read in chunks until MA_AT_END
            OLO_CORE_WARN("[AudioLoader] Using streaming read for {}: format may not support length queries", 
                sourceDescription);
            
            const u32 chunkFrames = 4096; // Read 4096 frames at a time
            const u32 chunkSamples = chunkFrames * outAudioData.m_NumChannels;
            std::vector<f32> chunkBuffer(chunkSamples);
            
            ma_uint64 totalFramesRead = 0;
            
            while (true)
            {
                ma_uint64 framesRead;
                result = ma_decoder_read_pcm_frames(&decoder, chunkBuffer.data(), chunkFrames, &framesRead);
                
                if (result != MA_SUCCESS && result != MA_AT_END)
                {
                    OLO_CORE_ERROR("[AudioLoader] Error during streaming read from {}: error {}", 
                        sourceDescription, static_cast<int>(result));
                    outAudioData.Clear();
                    return false;
                }
                
                if (framesRead == 0)
                    break; // End of stream
                
                // Check for frame count overflow
                if (totalFramesRead + framesRead > UINT32_MAX)
                {
                    OLO_CORE_ERROR("[AudioLoader] Streaming read exceeded maximum frame count for {}", 
                        sourceDescription);
                    outAudioData.Clear();
                    return false;
                }
                
                // Append samples to output
                const u64 samplesToAppend = framesRead * outAudioData.m_NumChannels;
                const u64 currentSize = outAudioData.m_Samples.size();
                const u64 newSize = currentSize + samplesToAppend;
                
                // Check for sample buffer overflow
                if (newSize > outAudioData.m_Samples.max_size())
                {
                    OLO_CORE_ERROR("[AudioLoader] Sample buffer overflow during streaming read from {}", 
                        sourceDescription);
                    outAudioData.Clear();
                    return false;
                }
                
                outAudioData.m_Samples.resize(static_cast<sizet>(newSize));
                std::memcpy(outAudioData.m_Samples.data() + currentSize, chunkBuffer.data(), samplesToAppend * sizeof(f32));
                
                totalFramesRead += framesRead;
                
                if (result == MA_AT_END)
                    break;
            }
            
            // Set final frame count and duration from streaming read
            outAudioData.m_NumFrames = static_cast<u32>(totalFramesRead);
            outAudioData.m_Duration = static_cast<f64>(totalFramesRead) / outAudioData.m_SampleRate;
        }

        // Final validation
        if (outAudioData.m_NumFrames == 0)
        {
            OLO_CORE_ERROR("[AudioLoader] No audio frames loaded from {}", sourceDescription);
            outAudioData.Clear();
            return false;
        }

        OLO_CORE_TRACE("[AudioLoader] Successfully decoded audio from '{}': {} frames, {} channels, {:.2f}s duration", 
            sourceDescription, outAudioData.m_NumFrames, outAudioData.m_NumChannels, outAudioData.m_Duration);

        return true;
    }

    bool AudioLoader::GetAudioFileInfo(const std::filesystem::path& filePath, u32& outNumChannels, u32& outNumFrames, f64& outSampleRate, f64& outDuration)
    {
        [[maybe_unused]]u16 bitDepth; // Unused in this overload
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
        
        // Handle frame length query results
        if (result == MA_SUCCESS && totalFrames > 0)
        {
            // Check for frame count overflow before casting to u32
            if (totalFrames > UINT32_MAX)
            {
                OLO_CORE_ERROR("[AudioLoader] Info query: file has too many frames: {} ({} frames > {} max)", 
                    utf8Path, totalFrames, UINT32_MAX);
                ma_decoder_uninit(&decoder);
                return false;
            }
            
            outNumFrames = static_cast<u32>(totalFrames);
            outDuration = static_cast<f64>(totalFrames) / static_cast<f64>(decoder.outputSampleRate);
        }
        else if (result == MA_NOT_IMPLEMENTED || totalFrames == 0)
        {
            // Some formats (like Vorbis) don't support length queries or may have zero length
            OLO_CORE_WARN("[AudioLoader] Cannot determine frame count for file: {} (format may not support length queries)", 
                utf8Path);
            outNumFrames = 0;
            outDuration = 0.0;
        }
        else
        {
            // Genuine error occurred
            OLO_CORE_ERROR("[AudioLoader] Failed to get frame count for info query: {} (error: {})", 
                filePath.string(), static_cast<int>(result));
            ma_decoder_uninit(&decoder);
            return false;
        }

        // Store audio properties
        outNumChannels = decoder.outputChannels;
        outSampleRate = static_cast<f64>(decoder.outputSampleRate);
        
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

        // Validate essential audio properties (frame count can be 0 for streaming formats)
        if (outNumChannels == 0 || outSampleRate <= 0.0 || outBitDepth == 0)
        {
            OLO_CORE_ERROR("[AudioLoader] Invalid essential audio properties for file: {} (channels: {}, sampleRate: {}, bitDepth: {})", 
                filePath.string(), outNumChannels, outSampleRate, outBitDepth);
            return false;
        }

        return true;
    }

    bool AudioLoader::IsExtensionSupported(const std::string& extension)
    {
        // Convert to lowercase for case-insensitive comparison
        std::string lowerExt = extension;
        std::transform(lowerExt.begin(), lowerExt.end(), lowerExt.begin(), [](unsigned char c)
        {
            return static_cast<char>(std::tolower(c));
        });

        return std::find(s_SupportedExtensions.begin(), s_SupportedExtensions.end(), lowerExt) != s_SupportedExtensions.end();
    }

    const std::vector<std::string>& AudioLoader::GetSupportedExtensions()
    {
        return s_SupportedExtensions;
    }

} // namespace OloEngine::Audio
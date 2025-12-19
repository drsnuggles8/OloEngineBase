#pragma once

#include "OloEngine/Core/Base.h"
#include <vector>
#include <string>
#include <filesystem>
#include <cmath>

// Forward declaration for miniaudio decoder type
typedef struct ma_decoder ma_decoder;

namespace OloEngine::Audio
{
    //==============================================================================
    /// Audio data structure for loaded audio files
    struct AudioData
    {
        std::vector<f32> m_Samples;       // Interleaved audio samples (L, R, L, R...)
        u32 m_NumChannels = 0;            // Number of audio channels
        u32 m_NumFrames = 0;              // Total number of frames (not samples)
        f64 m_SampleRate = 0.0;           // Sample rate in Hz
        f64 m_Duration = 0.0;             // Duration in seconds
        u64 m_FileSize = 0;               // Original file size in bytes
        
        /// Clear all audio data
        void Clear()
        {
            m_Samples.clear();
            m_NumChannels = 0;
            m_NumFrames = 0;
            m_SampleRate = 0.0;
            m_Duration = 0.0;
            m_FileSize = 0;
        }
        
        /// Check if audio data is valid
        bool IsValid() const 
        {
            OLO_PROFILE_FUNCTION();
            
            if (m_Samples.empty() || m_NumChannels == 0 || m_NumFrames == 0 || !std::isfinite(m_SampleRate) || m_SampleRate <= 0.0)
                return false;

            // Validate duration consistency
            const f64 expectedDuration = static_cast<f64>(m_NumFrames) / m_SampleRate;
            if (!std::isfinite(m_Duration) || std::abs(m_Duration - expectedDuration) > 0.001)
                return false;
                
            const sizet expectedSampleCount = static_cast<sizet>(m_NumFrames) * static_cast<sizet>(m_NumChannels);
            return m_Samples.size() == expectedSampleCount;
        }
        
        /// Get sample at specific frame and channel
        f32 GetSample(u32 frame, u32 channel) const
        {
            OLO_PROFILE_FUNCTION();

            OLO_CORE_ASSERT(IsValid(), "AudioData must be valid before accessing samples");
            OLO_CORE_ASSERT(frame < m_NumFrames && channel < m_NumChannels, "Frame/channel out of range");
            
            if (frame >= m_NumFrames || channel >= m_NumChannels)
                return 0.0f;
            
            // Compute sample index with explicit casting to avoid overflow on 32-bit builds
            sizet sampleIndex = static_cast<sizet>(frame) * static_cast<sizet>(m_NumChannels) + static_cast<sizet>(channel);
            return m_Samples[sampleIndex];
        }
        
        /// Get total number of samples (all channels)
        u64 GetTotalSamples() const
        {
            return static_cast<u64>(m_Samples.size());
        }
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
        static const std::vector<std::string>& GetSupportedExtensions();

    private:
        AudioLoader() = delete; // Static utility class
        
        /// Helper function to decode audio data from an initialized decoder
        /// @param decoder Initialized ma_decoder ready for reading
        /// @param outAudioData Structure to receive loaded audio data
        /// @param sourceDescription Description of the source for error messages (e.g., file path or "memory")
        /// @return true if decoding succeeded, false otherwise
        static bool DecodeAudioData(ma_decoder& decoder, AudioData& outAudioData, const std::string& sourceDescription);
    };

} // namespace OloEngine::Audio

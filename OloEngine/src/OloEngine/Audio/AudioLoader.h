#pragma once

#include "OloEngine/Core/Base.h"
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
            if (m_Samples.empty() || m_NumChannels == 0 || m_NumFrames == 0 || !std::isfinite(m_SampleRate) || m_SampleRate <= 0.0)
                return false;
                
            const sizet expectedSampleCount = sizet(m_NumFrames) * sizet(m_NumChannels);
            return m_Samples.size() == expectedSampleCount;
        }
        
        /// Get sample at specific frame and channel
        f32 GetSample(u64 frame, u32 channel) const
        {
            if (frame >= m_NumFrames || channel >= m_NumChannels)
                return 0.0f;
            
            u64 sampleIndex = frame * m_NumChannels + channel;
            return (sampleIndex < m_Samples.size()) ? m_Samples[sampleIndex] : 0.0f;
        }
        
        /// Get total number of samples (all channels)
        u64 GetTotalSamples() const { return static_cast<u64>(m_Samples.size()); }
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
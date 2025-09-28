#pragma once

#include <cmath>
#include <cstring>

#include "OloEngine/Core/Base.h"
#include <choc/audio/choc_SampleBuffers.h>

namespace OloEngine::Audio
{
    /// Audio sample buffer operations for OloEngine
    /// Provides utilities for audio processing including interleaving, deinterleaving, and gain operations
    /// Based on Hazel's SampleBufferOperations but adapted for OloEngine architecture
    class SampleBufferOperations
    {
    public:
        /// Apply a gain ramp to an interleaved buffer
        static inline void ApplyGainRamp(f32* data, u32 numSamples, u32 numChannels, f32 gainStart, f32 gainEnd)
        {
            // Guard against division by zero
            if (numSamples == 0)
                return;
            
            // Special case for single sample: apply gainEnd directly
            if (numSamples == 1)
            {
                for (u32 ch = 0; ch < numChannels; ++ch)
                    data[ch] *= gainEnd;
                return;
            }
            
            // For multiple samples, use (numSamples - 1) to ensure last sample equals gainEnd
            const f32 delta = (gainEnd - gainStart) / static_cast<f32>(numSamples - 1);
            for (u32 i = 0; i < numSamples; ++i)
            {
                for (u32 ch = 0; ch < numChannels; ++ch)
                    data[i * numChannels + ch] *= gainStart + delta * static_cast<f32>(i);
            }
        }

        /// Apply a gain ramp to a single channel in an interleaved buffer
        static inline void ApplyGainRampToSingleChannel(f32* data, u32 numSamples, u32 numChannels, u32 channel, f32 gainStart, f32 gainEnd)
        {
            // Guard against division by zero
            if (numSamples == 0)
                return;
            
            // Special case for single sample: apply gainEnd directly
            if (numSamples == 1)
            {
                data[channel] *= gainEnd;
                return;
            }
            
            // For multiple samples, use (numSamples - 1) to ensure last sample equals gainEnd
            const f32 delta = (gainEnd - gainStart) / static_cast<f32>(numSamples - 1);
            for (u32 i = 0; i < numSamples; ++i)
            {
                data[i * numChannels + channel] *= gainStart + delta * static_cast<f32>(i);
            }
        }

        /// Add source to destination with gain ramp, handling channel routing
        static inline void AddAndApplyGainRamp(f32* dest, const f32* source, u32 destChannel, u32 sourceChannel,
                                             u32 destNumChannels, u32 sourceNumChannels, u32 numSamples, f32 gainStart, f32 gainEnd)
        {
            // Guard against empty range
            if (numSamples == 0)
                return;
                
            if (gainEnd == gainStart)
            {
                for (u32 i = 0; i < numSamples; ++i)
                    dest[i * destNumChannels + destChannel] += source[i * sourceNumChannels + sourceChannel] * gainStart;
            }
            else if (numSamples == 1)
            {
                // Special case for single sample: apply gainEnd directly
                dest[destChannel] += source[sourceChannel] * gainEnd;
            }
            else
            {
                // For multiple samples, use (numSamples - 1) to ensure last sample equals gainEnd
                const f32 delta = (gainEnd - gainStart) / static_cast<f32>(numSamples - 1);
                for (u32 i = 0; i < numSamples; ++i)
                {
                    f32 gain = gainStart + delta * static_cast<f32>(i);
                    dest[i * destNumChannels + destChannel] += source[i * sourceNumChannels + sourceChannel] * gain;
                }
            }
        }

        /// Add source to destination with constant gain, handling channel routing
        static inline void AddAndApplyGain(f32* dest, const f32* source, u32 destChannel, u32 sourceChannel,
                                         u32 destNumChannels, u32 sourceNumChannels, u32 numSamples, f32 gain)
        {
            for (u32 i = 0; i < numSamples; ++i)
                dest[i * destNumChannels + destChannel] += source[i * sourceNumChannels + sourceChannel] * gain;
        }

        /// Compare buffer contents for equality
        static bool ContentMatches(const f32* buffer1, const f32* buffer2, u32 frameCount, u32 numChannels)
        {
            for (u32 frame = 0; frame < frameCount; ++frame)
            {
                for (u32 chan = 0; chan < numChannels; ++chan)
                {
                    const auto pos = frame * numChannels + chan;
                    if (buffer1[pos] != buffer2[pos])
                        return false;
                }
            }

            return true;
        }

        /// Add two deinterleaved buffers together
        static inline void AddDeinterleaved(f32* dest, const f32* source, u32 numSamples)
        {
            for (u32 i = 0; i < numSamples; ++i)
                dest[i] += source[i];
        }

        /// Convert interleaved audio to deinterleaved format using choc buffers
        template<typename SampleType>
        static void Deinterleave(choc::buffer::ChannelArrayBuffer<SampleType>& dest, const f32* source)
        {
            auto numChannels = dest.getNumChannels();
            auto destData = dest.getView().data.channels;

            for (u32 ch = 0; ch < dest.getNumChannels(); ++ch)
            {
                for (u32 s = 0; s < dest.getNumFrames(); ++s)
                {
                    destData[ch][s] = source[s * numChannels + ch];
                }
            }
        }

        /// Convert deinterleaved audio to interleaved format using choc buffers
        template <typename SampleType>
        static void Interleave(f32* dest, const choc::buffer::ChannelArrayBuffer<SampleType>& source)
        {
            auto numChannels = source.getNumChannels();
            auto sourceData = source.getView().data.channels;

            for (u32 ch = 0; ch < source.getNumChannels(); ++ch)
            {
                for (u32 s = 0; s < source.getNumFrames(); ++s)
                {
                    dest[s * numChannels + ch] = sourceData[ch][s];
                }
            }
        }

        /// Convert interleaved audio to deinterleaved format using raw pointers
        static void Deinterleave(f32* const* dest, const f32* source, u32 numChannels, u32 numSamples)
        {
            for (u32 ch = 0; ch < numChannels; ++ch)
            {
                for (u32 s = 0; s < numSamples; ++s)
                {
                    dest[ch][s] = source[s * numChannels + ch];
                }
            }
        }

        /// Convert deinterleaved audio to interleaved format using raw pointers
        static void Interleave(f32* dest, const f32* const* source, u32 numChannels, u32 numSamples)
        {
            for (u32 ch = 0; ch < numChannels; ++ch)
            {
                for (u32 s = 0; s < numSamples; ++s)
                {
                    dest[s * numChannels + ch] = source[ch][s];
                }
            }
        }

        /// Calculate magnitude of a buffer section
        template<typename T, template<typename> typename LayoutType>
        static T GetMagnitude(const choc::buffer::BufferView<T, LayoutType>& buffer, i32 startSample, i32 numSamples)
        {
            // Early return for invalid parameters
            if (numSamples <= 0 || startSample < 0 || startSample >= static_cast<i32>(buffer.getNumFrames()))
                return T(0);
            
            // Clamp the sample window to buffer bounds
            i32 endSample = std::min(startSample + numSamples, static_cast<i32>(buffer.getNumFrames()));
            i32 actualSamplesCount = endSample - startSample;
            
            // Return zero if no samples to process
            if (actualSamplesCount <= 0)
                return T(0);
            
            T magnitude = T(0);
            
            for (u32 ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                for (i32 s = startSample; s < endSample; ++s)
                {
                    T sample = buffer.getSample(ch, s);
                    magnitude += sample * sample;
                }
            }
            
            // Use actual samples processed for accurate mean calculation
            return std::sqrt(magnitude / (buffer.getNumChannels() * actualSamplesCount));
        }

        /// Clear a buffer with zeros
        template<typename SampleType>
        static void Clear(choc::buffer::ChannelArrayBuffer<SampleType>& buffer)
        {
            buffer.clear();
        }

        /// Clear an interleaved buffer with zeros
        static void Clear(f32* data, u32 numSamples, u32 numChannels)
        {
            std::memset(data, 0, numSamples * numChannels * sizeof(f32));
        }
    };

} // namespace OloEngine::Audio
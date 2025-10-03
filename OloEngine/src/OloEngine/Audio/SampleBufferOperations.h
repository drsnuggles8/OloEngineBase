#pragma once

#include <cmath>
#include <cstring>
#include <algorithm>

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
            OLO_PROFILE_FUNCTION();

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
            OLO_PROFILE_FUNCTION();

            // Guard against invalid parameters
            if (numSamples == 0 || numChannels == 0 || channel >= numChannels)
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
            OLO_PROFILE_FUNCTION();

            // Guard against invalid parameters
            if (destNumChannels == 0 || sourceNumChannels == 0 || destChannel >= destNumChannels || sourceChannel >= sourceNumChannels)
                return;
                
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
            OLO_PROFILE_FUNCTION();

            // Guard against invalid parameters
            if (destNumChannels == 0 || sourceNumChannels == 0 || destChannel >= destNumChannels || sourceChannel >= sourceNumChannels)
                return;
                
            for (u32 i = 0; i < numSamples; ++i)
                dest[i * destNumChannels + destChannel] += source[i * sourceNumChannels + sourceChannel] * gain;
        }

        /// Compare buffer contents for bit-exact equality
        /// NOTE: Uses exact floating-point comparison (==). This only returns true if buffers
        /// are bit-for-bit identical (e.g., exact memory copies). Due to floating-point precision,
        /// mathematically equivalent values may differ slightly after arithmetic operations.
        /// For approximate equality testing (e.g., after audio processing), use ContentMatchesApprox instead.
        static bool ContentMatches(const f32* buffer1, const f32* buffer2, u32 frameCount, u32 numChannels)
        {
            OLO_PROFILE_FUNCTION();
            
            // Optimization: use memcmp for bit-exact comparison
            const sizet totalSamples = static_cast<sizet>(frameCount) * static_cast<sizet>(numChannels);
            return std::memcmp(buffer1, buffer2, totalSamples * sizeof(f32)) == 0;
        }

        /// Compare buffer contents for approximate equality within epsilon tolerance
        /// This is typically more appropriate for audio processing where floating-point
        /// precision errors are expected after arithmetic operations.
        static bool ContentMatchesApprox(const f32* buffer1, const f32* buffer2, u32 frameCount, u32 numChannels, f32 epsilon = 1e-6f)
        {
            OLO_PROFILE_FUNCTION();
            
            for (u32 frame = 0; frame < frameCount; ++frame)
            {
                for (u32 chan = 0; chan < numChannels; ++chan)
                {
                    const auto pos = frame * numChannels + chan;
                    if (std::abs(buffer1[pos] - buffer2[pos]) > epsilon)
                        return false;
                }
            }

            return true;
        }

        /// Add two deinterleaved buffers together
        static inline void AddDeinterleaved(f32* dest, const f32* source, u32 numSamples)
        {
            OLO_PROFILE_FUNCTION();

            for (u32 i = 0; i < numSamples; ++i)
                dest[i] += source[i];
        }

        /// Convert interleaved audio to deinterleaved format using choc buffers
        template<typename SampleType>
        static void Deinterleave(choc::buffer::ChannelArrayBuffer<SampleType>& dest, const f32* source, u32 numFrames = 0)
        {
            OLO_PROFILE_FUNCTION();

            auto numChannels = dest.getNumChannels();
            auto destData = dest.getView().data.channels;
            auto framesToProcess = (numFrames == 0) ? dest.getNumFrames() : std::min(numFrames, dest.getNumFrames());

            for (u32 s = 0; s < framesToProcess; ++s)
            {
                for (u32 ch = 0; ch < numChannels; ++ch)
                {
                    destData[ch][s] = source[s * numChannels + ch];
                }
            }
        }

        /// Convert deinterleaved audio to interleaved format using choc buffers
        template <typename SampleType>
        static void Interleave(f32* dest, const choc::buffer::ChannelArrayBuffer<SampleType>& source, u32 numFrames = 0)
        {
            OLO_PROFILE_FUNCTION();

            auto numChannels = source.getNumChannels();
            auto sourceData = source.getView().data.channels;
            auto framesToProcess = (numFrames == 0) ? source.getNumFrames() : std::min(numFrames, source.getNumFrames());

            for (u32 s = 0; s < framesToProcess; ++s)
            {
                for (u32 ch = 0; ch < numChannels; ++ch)
                {
                    dest[s * numChannels + ch] = sourceData[ch][s];
                }
            }
        }

        /// Convert interleaved audio to deinterleaved format using raw pointers
        static void Deinterleave(f32* const* dest, const f32* source, u32 numChannels, u32 numSamples)
        {
            OLO_PROFILE_FUNCTION();

            for (u32 s = 0; s < numSamples; ++s)
            {
                for (u32 ch = 0; ch < numChannels; ++ch)
                {
                    dest[ch][s] = source[s * numChannels + ch];
                }
            }
        }

        /// Convert deinterleaved audio to interleaved format using raw pointers
        static void Interleave(f32* dest, const f32* const* source, u32 numChannels, u32 numSamples)
        {
            OLO_PROFILE_FUNCTION();

            for (u32 s = 0; s < numSamples; ++s)
            {
                for (u32 ch = 0; ch < numChannels; ++ch)
                {
                    dest[s * numChannels + ch] = source[ch][s];
                }
            }
        }

        /// Calculate magnitude of a buffer section
        template<typename T, template<typename> typename LayoutType>
        static double GetMagnitude(const choc::buffer::BufferView<T, LayoutType>& buffer, i32 startSample, i32 numSamples)
        {
            OLO_PROFILE_FUNCTION();

            // Early return for invalid parameters
            if (numSamples <= 0 || startSample < 0 || startSample >= static_cast<i32>(buffer.getNumFrames()))
                return 0.0;
            
            // Cache channel count and check for zero channels to prevent division by zero
            const u32 cachedChannels = buffer.getNumChannels();
            if (cachedChannels == 0)
                return 0.0;
            
            // Clamp the sample window to buffer bounds
            i32 endSample = std::min(startSample + numSamples, static_cast<i32>(buffer.getNumFrames()));
            i32 actualSamplesCount = endSample - startSample;
            
            // Return zero if no samples to process
            if (actualSamplesCount <= 0)
                return 0.0;
            
            double magnitude = 0.0;
            
            for (u32 ch = 0; ch < cachedChannels; ++ch)
            {
                for (i32 s = startSample; s < endSample; ++s)
                {
                    T sample = buffer.getSample(ch, s);
                    double doubleSample = static_cast<double>(sample);
                    magnitude += doubleSample * doubleSample;
                }
            }
            
            // Use cached channel count and actual samples processed for accurate mean calculation
            double meanSquare = magnitude / (static_cast<double>(cachedChannels) * static_cast<double>(actualSamplesCount));
            return std::sqrt(meanSquare);
        }

        /// Clear a buffer with zeros
        template<typename SampleType>
        static void Clear(choc::buffer::ChannelArrayBuffer<SampleType>& buffer)
        {
            OLO_PROFILE_FUNCTION();

            buffer.clear();
        }

        /// Clear an interleaved buffer with zeros
        static void Clear(f32* data, u32 numSamples, u32 numChannels)
        {
            OLO_PROFILE_FUNCTION();

            std::memset(data, 0, numSamples * numChannels * sizeof(f32));
        }
    };

} // namespace OloEngine::Audio
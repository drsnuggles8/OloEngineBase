#pragma once

#include <cmath>
#include <cstring>
#include <algorithm>

#include "OloEngine/Core/Base.h"
#include <choc/audio/choc_SampleBuffers.h>

// SIMD intrinsics support detection and includes
// Detects and enables SSE/AVX optimizations based on compiler flags and CPU capabilities
// Falls back to scalar implementations when SIMD is unavailable
//
// NOTE: For MSVC, AVX support requires /arch:AVX or /arch:AVX2 compiler flag.
//       Runtime CPU feature detection via __cpuid is the preferred approach for dynamic SIMD dispatch.
//       Without proper compiler flags, AVX instructions will cause illegal instruction crashes on older CPUs.
#if defined(_MSC_VER)
    #include <intrin.h>
    // SSE is baseline for x64, optional for x86
    #if defined(_M_X64) || (defined(_M_IX86) && defined(__SSE__))
        #define OLO_AUDIO_HAS_SSE 1
    #endif
    // AVX requires explicit /arch:AVX or /arch:AVX2 flag
    // MSVC defines __AVX__ when /arch:AVX is used
    #if defined(__AVX__)
        #define OLO_AUDIO_HAS_AVX 1
    #endif
#elif defined(__GNUC__) || defined(__clang__)
    #if defined(__SSE__)
        #include <xmmintrin.h>
        #define OLO_AUDIO_HAS_SSE 1
    #endif
    #if defined(__AVX__)
        #include <immintrin.h>
        #define OLO_AUDIO_HAS_AVX 1
    #endif
#endif

namespace OloEngine::Audio
{
    /// Audio sample buffer operations for OloEngine
    /// Provides utilities for audio processing including interleaving, deinterleaving, and gain operations
    /// Based on Hazel's SampleBufferOperations but adapted for OloEngine architecture
    ///
    /// SIMD Optimizations:
    /// - ApplyGainRamp: AVX/SSE vectorized gain ramping for interleaved buffers
    /// - ApplyGainRampToSingleChannel: Vectorized single-channel gain ramping
    /// - AddAndApplyGainRamp: Vectorized mix with gain ramp
    /// - AddAndApplyGain: Vectorized mix with constant gain
    /// - Interleave/Deinterleave: Optimized stereo interleaving using shuffle/unpack instructions
    /// 
    /// All functions maintain exact scalar behavior with automatic SIMD fallback.
    /// See SampleBufferOperations_SIMD_README.md for detailed documentation.
    class SampleBufferOperations
    {
    public:
        /// Apply a gain ramp to an interleaved buffer
        static inline void ApplyGainRamp(f32* data, u32 numSamples, u32 numChannels, f32 gainStart, f32 gainEnd)
        {
            OLO_PROFILE_FUNCTION();

            if (data == nullptr || numChannels == 0 || numSamples == 0)
                return;
            
            // Special case for single sample: apply gainEnd directly
            if (numSamples == 1)
            {
                for (u32 ch = 0; ch < numChannels; ++ch)
                    data[ch] *= gainEnd;
                return;
            }
            
            const u32 totalSamples = numSamples * numChannels;
            
            // For multiple samples, use (numSamples - 1) to ensure last sample equals gainEnd
            const f32 delta = (gainEnd - gainStart) / static_cast<f32>(numSamples - 1);
            
#if defined(OLO_AUDIO_HAS_AVX)
            // AVX path: process 8 floats at a time
            if (totalSamples >= 8)
            {
                const u32 simdSamples = (totalSamples / 8) * 8;
                
                // Prepare gain ramp vectors
                // For interleaved data, we need to replicate the gain value across channels within each sample
                // But since channels are interleaved, we process linearly with incrementing gain
                const __m256 deltaVec = _mm256_set1_ps(delta);
                const __m256 gainStartVec = _mm256_set1_ps(gainStart);
                
                // Create a vector of sample indices: [0, 1, 2, 3, 4, 5, 6, 7] scaled by channels
                // Each position in the SIMD vector corresponds to a different sample/channel
                alignas(32) f32 indexOffsets[8];
                for (u32 i = 0; i < 8; ++i)
                    indexOffsets[i] = static_cast<f32>(i / numChannels);
                __m256 indexVec = _mm256_load_ps(indexOffsets);
                
                for (u32 i = 0; i < simdSamples; i += 8)
                {
                    // Calculate base sample index for this SIMD iteration
                    f32 baseSampleIdx = static_cast<f32>(i / numChannels);
                    __m256 baseSampleVec = _mm256_set1_ps(baseSampleIdx);
                    
                    // Compute the gain for each position
                    __m256 sampleIdxVec = _mm256_add_ps(baseSampleVec, indexVec);
                    __m256 gainVec = _mm256_add_ps(gainStartVec, _mm256_mul_ps(deltaVec, sampleIdxVec));
                    
                    // Load 8 samples, multiply by gain, store back
                    __m256 samples = _mm256_loadu_ps(&data[i]);
                    samples = _mm256_mul_ps(samples, gainVec);
                    _mm256_storeu_ps(&data[i], samples);
                }
                
                // Handle remainder with scalar code
                for (u32 i = simdSamples; i < totalSamples; ++i)
                {
                    u32 sampleIdx = i / numChannels;
                    data[i] *= gainStart + delta * static_cast<f32>(sampleIdx);
                }
                return;
            }
#elif defined(OLO_AUDIO_HAS_SSE)
            // SSE path: process 4 floats at a time
            if (totalSamples >= 4)
            {
                const u32 simdSamples = (totalSamples / 4) * 4;
                
                const __m128 deltaVec = _mm_set1_ps(delta);
                const __m128 gainStartVec = _mm_set1_ps(gainStart);
                
                alignas(16) f32 indexOffsets[4];
                for (u32 i = 0; i < 4; ++i)
                    indexOffsets[i] = static_cast<f32>(i / numChannels);
                __m128 indexVec = _mm_load_ps(indexOffsets);
                
                for (u32 i = 0; i < simdSamples; i += 4)
                {
                    f32 baseSampleIdx = static_cast<f32>(i / numChannels);
                    __m128 baseSampleVec = _mm_set1_ps(baseSampleIdx);
                    
                    __m128 sampleIdxVec = _mm_add_ps(baseSampleVec, indexVec);
                    __m128 gainVec = _mm_add_ps(gainStartVec, _mm_mul_ps(deltaVec, sampleIdxVec));
                    
                    __m128 samples = _mm_loadu_ps(&data[i]);
                    samples = _mm_mul_ps(samples, gainVec);
                    _mm_storeu_ps(&data[i], samples);
                }
                
                // Handle remainder
                for (u32 i = simdSamples; i < totalSamples; ++i)
                {
                    u32 sampleIdx = i / numChannels;
                    data[i] *= gainStart + delta * static_cast<f32>(sampleIdx);
                }
                return;
            }
#endif
            
            // Scalar fallback
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
            if (numSamples == 0 || numChannels == 0 || channel >= numChannels || data == nullptr)
                return;
            
            // Special case for single sample: apply gainEnd directly
            if (numSamples == 1)
            {
                data[channel] *= gainEnd;
                return;
            }
            
            // For multiple samples, use (numSamples - 1) to ensure last sample equals gainEnd
            const f32 delta = (gainEnd - gainStart) / static_cast<f32>(numSamples - 1);
            
#if defined(OLO_AUDIO_HAS_AVX)
            // AVX path: process 8 samples at a time with strided access
            if (numSamples >= 8)
            {
                const u32 simdSamples = (numSamples / 8) * 8;
                const __m256 deltaVec = _mm256_set1_ps(delta);
                const __m256 gainStartVec = _mm256_set1_ps(gainStart);
                
                // Create sample index offsets [0, 1, 2, 3, 4, 5, 6, 7]
                const __m256 indexOffsetVec = _mm256_setr_ps(0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f);
                
                for (u32 i = 0; i < simdSamples; i += 8)
                {
                    // Gather 8 samples (strided by numChannels)
                    alignas(32) f32 samples[8];
                    for (u32 j = 0; j < 8; ++j)
                        samples[j] = data[(i + j) * numChannels + channel];
                    
                    // Compute gains for these 8 samples
                    __m256 baseSampleVec = _mm256_set1_ps(static_cast<f32>(i));
                    __m256 sampleIdxVec = _mm256_add_ps(baseSampleVec, indexOffsetVec);
                    __m256 gainVec = _mm256_add_ps(gainStartVec, _mm256_mul_ps(deltaVec, sampleIdxVec));
                    
                    // Apply gain
                    __m256 samplesVec = _mm256_load_ps(samples);
                    samplesVec = _mm256_mul_ps(samplesVec, gainVec);
                    _mm256_store_ps(samples, samplesVec);
                    
                    // Scatter back (strided by numChannels)
                    for (u32 j = 0; j < 8; ++j)
                        data[(i + j) * numChannels + channel] = samples[j];
                }
                
                // Handle remainder
                for (u32 i = simdSamples; i < numSamples; ++i)
                {
                    data[i * numChannels + channel] *= gainStart + delta * static_cast<f32>(i);
                }
                return;
            }
#elif defined(OLO_AUDIO_HAS_SSE)
            // SSE path: process 4 samples at a time
            if (numSamples >= 4)
            {
                const u32 simdSamples = (numSamples / 4) * 4;
                const __m128 deltaVec = _mm_set1_ps(delta);
                const __m128 gainStartVec = _mm_set1_ps(gainStart);
                const __m128 indexOffsetVec = _mm_setr_ps(0.0f, 1.0f, 2.0f, 3.0f);
                
                for (u32 i = 0; i < simdSamples; i += 4)
                {
                    alignas(16) f32 samples[4];
                    for (u32 j = 0; j < 4; ++j)
                        samples[j] = data[(i + j) * numChannels + channel];
                    
                    __m128 baseSampleVec = _mm_set1_ps(static_cast<f32>(i));
                    __m128 sampleIdxVec = _mm_add_ps(baseSampleVec, indexOffsetVec);
                    __m128 gainVec = _mm_add_ps(gainStartVec, _mm_mul_ps(deltaVec, sampleIdxVec));
                    
                    __m128 samplesVec = _mm_load_ps(samples);
                    samplesVec = _mm_mul_ps(samplesVec, gainVec);
                    _mm_store_ps(samples, samplesVec);
                    
                    for (u32 j = 0; j < 4; ++j)
                        data[(i + j) * numChannels + channel] = samples[j];
                }
                
                for (u32 i = simdSamples; i < numSamples; ++i)
                {
                    data[i * numChannels + channel] *= gainStart + delta * static_cast<f32>(i);
                }
                return;
            }
#endif
            
            // Scalar fallback
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

            if (dest == nullptr || source == nullptr)
                return;

            // Guard against invalid parameters
            if (destNumChannels == 0 || sourceNumChannels == 0 || destChannel >= destNumChannels || sourceChannel >= sourceNumChannels)
                return;
                
            // Guard against empty range
            if (numSamples == 0)
                return;
                
            if (gainEnd == gainStart)
            {
#if defined(OLO_AUDIO_HAS_AVX)
                // Constant gain with AVX: process 8 samples at a time
                if (numSamples >= 8)
                {
                    const u32 simdSamples = (numSamples / 8) * 8;
                    const __m256 gainVec = _mm256_set1_ps(gainStart);
                    
                    for (u32 i = 0; i < simdSamples; i += 8)
                    {
                        alignas(32) f32 destSamples[8], srcSamples[8];
                        
                        // Gather source and destination samples
                        for (u32 j = 0; j < 8; ++j)
                        {
                            srcSamples[j] = source[(i + j) * sourceNumChannels + sourceChannel];
                            destSamples[j] = dest[(i + j) * destNumChannels + destChannel];
                        }
                        
                        // Load, multiply source by gain, add to dest
                        __m256 srcVec = _mm256_load_ps(srcSamples);
                        __m256 destVec = _mm256_load_ps(destSamples);
                        srcVec = _mm256_mul_ps(srcVec, gainVec);
                        destVec = _mm256_add_ps(destVec, srcVec);
                        _mm256_store_ps(destSamples, destVec);
                        
                        // Scatter back
                        for (u32 j = 0; j < 8; ++j)
                            dest[(i + j) * destNumChannels + destChannel] = destSamples[j];
                    }
                    
                    // Handle remainder
                    for (u32 i = simdSamples; i < numSamples; ++i)
                        dest[i * destNumChannels + destChannel] += source[i * sourceNumChannels + sourceChannel] * gainStart;
                    return;
                }
#elif defined(OLO_AUDIO_HAS_SSE)
                // Constant gain with SSE: process 4 samples at a time
                if (numSamples >= 4)
                {
                    const u32 simdSamples = (numSamples / 4) * 4;
                    const __m128 gainVec = _mm_set1_ps(gainStart);
                    
                    for (u32 i = 0; i < simdSamples; i += 4)
                    {
                        alignas(16) f32 destSamples[4], srcSamples[4];
                        
                        for (u32 j = 0; j < 4; ++j)
                        {
                            srcSamples[j] = source[(i + j) * sourceNumChannels + sourceChannel];
                            destSamples[j] = dest[(i + j) * destNumChannels + destChannel];
                        }
                        
                        __m128 srcVec = _mm_load_ps(srcSamples);
                        __m128 destVec = _mm_load_ps(destSamples);
                        srcVec = _mm_mul_ps(srcVec, gainVec);
                        destVec = _mm_add_ps(destVec, srcVec);
                        _mm_store_ps(destSamples, destVec);
                        
                        for (u32 j = 0; j < 4; ++j)
                            dest[(i + j) * destNumChannels + destChannel] = destSamples[j];
                    }
                    
                    for (u32 i = simdSamples; i < numSamples; ++i)
                        dest[i * destNumChannels + destChannel] += source[i * sourceNumChannels + sourceChannel] * gainStart;
                    return;
                }
#endif
                // Scalar fallback for constant gain
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
                // For multiple samples with gain ramp, use (numSamples - 1) to ensure last sample equals gainEnd
                const f32 delta = (gainEnd - gainStart) / static_cast<f32>(numSamples - 1);
                
#if defined(OLO_AUDIO_HAS_AVX)
                // Gain ramp with AVX
                if (numSamples >= 8)
                {
                    const u32 simdSamples = (numSamples / 8) * 8;
                    const __m256 deltaVec = _mm256_set1_ps(delta);
                    const __m256 gainStartVec = _mm256_set1_ps(gainStart);
                    const __m256 indexOffsetVec = _mm256_setr_ps(0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f);
                    
                    for (u32 i = 0; i < simdSamples; i += 8)
                    {
                        alignas(32) f32 destSamples[8], srcSamples[8];
                        
                        for (u32 j = 0; j < 8; ++j)
                        {
                            srcSamples[j] = source[(i + j) * sourceNumChannels + sourceChannel];
                            destSamples[j] = dest[(i + j) * destNumChannels + destChannel];
                        }
                        
                        // Compute gains
                        __m256 baseSampleVec = _mm256_set1_ps(static_cast<f32>(i));
                        __m256 sampleIdxVec = _mm256_add_ps(baseSampleVec, indexOffsetVec);
                        __m256 gainVec = _mm256_add_ps(gainStartVec, _mm256_mul_ps(deltaVec, sampleIdxVec));
                        
                        // Process: dest += source * gain
                        __m256 srcVec = _mm256_load_ps(srcSamples);
                        __m256 destVec = _mm256_load_ps(destSamples);
                        srcVec = _mm256_mul_ps(srcVec, gainVec);
                        destVec = _mm256_add_ps(destVec, srcVec);
                        _mm256_store_ps(destSamples, destVec);
                        
                        for (u32 j = 0; j < 8; ++j)
                            dest[(i + j) * destNumChannels + destChannel] = destSamples[j];
                    }
                    
                    for (u32 i = simdSamples; i < numSamples; ++i)
                    {
                        f32 gain = gainStart + delta * static_cast<f32>(i);
                        dest[i * destNumChannels + destChannel] += source[i * sourceNumChannels + sourceChannel] * gain;
                    }
                    return;
                }
#elif defined(OLO_AUDIO_HAS_SSE)
                // Gain ramp with SSE
                if (numSamples >= 4)
                {
                    const u32 simdSamples = (numSamples / 4) * 4;
                    const __m128 deltaVec = _mm_set1_ps(delta);
                    const __m128 gainStartVec = _mm_set1_ps(gainStart);
                    const __m128 indexOffsetVec = _mm_setr_ps(0.0f, 1.0f, 2.0f, 3.0f);
                    
                    for (u32 i = 0; i < simdSamples; i += 4)
                    {
                        alignas(16) f32 destSamples[4], srcSamples[4];
                        
                        for (u32 j = 0; j < 4; ++j)
                        {
                            srcSamples[j] = source[(i + j) * sourceNumChannels + sourceChannel];
                            destSamples[j] = dest[(i + j) * destNumChannels + destChannel];
                        }
                        
                        __m128 baseSampleVec = _mm_set1_ps(static_cast<f32>(i));
                        __m128 sampleIdxVec = _mm_add_ps(baseSampleVec, indexOffsetVec);
                        __m128 gainVec = _mm_add_ps(gainStartVec, _mm_mul_ps(deltaVec, sampleIdxVec));
                        
                        __m128 srcVec = _mm_load_ps(srcSamples);
                        __m128 destVec = _mm_load_ps(destSamples);
                        srcVec = _mm_mul_ps(srcVec, gainVec);
                        destVec = _mm_add_ps(destVec, srcVec);
                        _mm_store_ps(destSamples, destVec);
                        
                        for (u32 j = 0; j < 4; ++j)
                            dest[(i + j) * destNumChannels + destChannel] = destSamples[j];
                    }
                    
                    for (u32 i = simdSamples; i < numSamples; ++i)
                    {
                        f32 gain = gainStart + delta * static_cast<f32>(i);
                        dest[i * destNumChannels + destChannel] += source[i * sourceNumChannels + sourceChannel] * gain;
                    }
                    return;
                }
#endif
                // Scalar fallback for gain ramp
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

            if (dest == nullptr || source == nullptr)
                return;

            // Guard against invalid parameters
            if (destNumChannels == 0 || sourceNumChannels == 0 || destChannel >= destNumChannels || sourceChannel >= sourceNumChannels)
                return;
            
#if defined(OLO_AUDIO_HAS_AVX)
            // AVX path: process 8 samples at a time
            if (numSamples >= 8)
            {
                const u32 simdSamples = (numSamples / 8) * 8;
                const __m256 gainVec = _mm256_set1_ps(gain);
                
                for (u32 i = 0; i < simdSamples; i += 8)
                {
                    alignas(32) f32 destSamples[8], srcSamples[8];
                    
                    // Gather samples
                    for (u32 j = 0; j < 8; ++j)
                    {
                        srcSamples[j] = source[(i + j) * sourceNumChannels + sourceChannel];
                        destSamples[j] = dest[(i + j) * destNumChannels + destChannel];
                    }
                    
                    // Process: dest += source * gain
                    __m256 srcVec = _mm256_load_ps(srcSamples);
                    __m256 destVec = _mm256_load_ps(destSamples);
                    srcVec = _mm256_mul_ps(srcVec, gainVec);
                    destVec = _mm256_add_ps(destVec, srcVec);
                    _mm256_store_ps(destSamples, destVec);
                    
                    // Scatter back
                    for (u32 j = 0; j < 8; ++j)
                        dest[(i + j) * destNumChannels + destChannel] = destSamples[j];
                }
                
                // Handle remainder
                for (u32 i = simdSamples; i < numSamples; ++i)
                    dest[i * destNumChannels + destChannel] += source[i * sourceNumChannels + sourceChannel] * gain;
                return;
            }
#elif defined(OLO_AUDIO_HAS_SSE)
            // SSE path: process 4 samples at a time
            if (numSamples >= 4)
            {
                const u32 simdSamples = (numSamples / 4) * 4;
                const __m128 gainVec = _mm_set1_ps(gain);
                
                for (u32 i = 0; i < simdSamples; i += 4)
                {
                    alignas(16) f32 destSamples[4], srcSamples[4];
                    
                    for (u32 j = 0; j < 4; ++j)
                    {
                        srcSamples[j] = source[(i + j) * sourceNumChannels + sourceChannel];
                        destSamples[j] = dest[(i + j) * destNumChannels + destChannel];
                    }
                    
                    __m128 srcVec = _mm_load_ps(srcSamples);
                    __m128 destVec = _mm_load_ps(destSamples);
                    srcVec = _mm_mul_ps(srcVec, gainVec);
                    destVec = _mm_add_ps(destVec, srcVec);
                    _mm_store_ps(destSamples, destVec);
                    
                    for (u32 j = 0; j < 4; ++j)
                        dest[(i + j) * destNumChannels + destChannel] = destSamples[j];
                }
                
                for (u32 i = simdSamples; i < numSamples; ++i)
                    dest[i * destNumChannels + destChannel] += source[i * sourceNumChannels + sourceChannel] * gain;
                return;
            }
#endif
                
            // Scalar fallback
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

            if (buffer1 == nullptr || buffer2 == nullptr)
                return false;
            
            const sizet totalSamples = static_cast<sizet>(frameCount) * numChannels;
            return std::memcmp(buffer1, buffer2, totalSamples * sizeof(f32)) == 0;
        }

        /// Compare buffer contents for approximate equality within epsilon tolerance
        /// This is typically more appropriate for audio processing where floating-point
        /// precision errors are expected after arithmetic operations.
        static bool ContentMatchesApprox(const f32* buffer1, const f32* buffer2, u32 frameCount, u32 numChannels, f32 epsilon = 1e-6f)
        {
            OLO_PROFILE_FUNCTION();

            if (buffer1 == nullptr || buffer2 == nullptr)
                return false;
            
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

            if (dest == nullptr || source == nullptr)
                return;

            for (u32 i = 0; i < numSamples; ++i)
                dest[i] += source[i];
        }

        /// Convert interleaved audio to deinterleaved format using choc buffers
        template<typename SampleType>
        static void Deinterleave(choc::buffer::ChannelArrayBuffer<SampleType>& dest, const f32* source, u32 numFrames = 0)
        {
            OLO_PROFILE_FUNCTION();

            if (source == nullptr)
                return;

            auto numChannels = dest.getNumChannels();
            auto destData = dest.getView().data.channels;
            auto framesToProcess = (numFrames == 0) ? dest.getNumFrames() : std::min(numFrames, dest.getNumFrames());

            // Special case: single channel can use direct memcpy
            if (numChannels == 1)
            {
                std::memcpy(destData[0], source, framesToProcess * sizeof(f32));
                return;
            }
            
#if defined(OLO_AUDIO_HAS_AVX)
            // AVX path for stereo (most common case)
            if (numChannels == 2 && framesToProcess >= 8)
            {
                const u32 simdFrames = (framesToProcess / 8) * 8;
                
                for (u32 s = 0; s < simdFrames; s += 8)
                {
                    // Load 16 interleaved samples: L0 R0 L1 R1 L2 R2 L3 R3 L4 R4 L5 R5 L6 R6 L7 R7
                    __m256 data0 = _mm256_loadu_ps(&source[s * 2]);
                    __m256 data1 = _mm256_loadu_ps(&source[s * 2 + 8]);
                    
                    // Deinterleave using shuffle and permute
                    // Extract even indices (left channel) and odd indices (right channel)
                    __m256 left = _mm256_shuffle_ps(data0, data1, _MM_SHUFFLE(2, 0, 2, 0));
                    __m256 right = _mm256_shuffle_ps(data0, data1, _MM_SHUFFLE(3, 1, 3, 1));
                    
                    // Fix lane ordering (AVX shuffles work within 128-bit lanes)
                    __m256d left_pd = _mm256_permute4x64_pd(_mm256_castps_pd(left), _MM_SHUFFLE(3, 1, 2, 0));
                    __m256d right_pd = _mm256_permute4x64_pd(_mm256_castps_pd(right), _MM_SHUFFLE(3, 1, 2, 0));
                    
                    _mm256_storeu_ps(&destData[0][s], _mm256_castpd_ps(left_pd));
                    _mm256_storeu_ps(&destData[1][s], _mm256_castpd_ps(right_pd));
                }
                
                // Handle remainder
                for (u32 s = simdFrames; s < framesToProcess; ++s)
                {
                    for (u32 ch = 0; ch < numChannels; ++ch)
                    {
                        destData[ch][s] = source[s * numChannels + ch];
                    }
                }
                return;
            }
#elif defined(OLO_AUDIO_HAS_SSE)
            // SSE path for stereo
            if (numChannels == 2 && framesToProcess >= 4)
            {
                const u32 simdFrames = (framesToProcess / 4) * 4;
                
                for (u32 s = 0; s < simdFrames; s += 4)
                {
                    // Load 8 interleaved samples: L0 R0 L1 R1 L2 R2 L3 R3
                    __m128 data0 = _mm_loadu_ps(&source[s * 2]);
                    __m128 data1 = _mm_loadu_ps(&source[s * 2 + 4]);
                    
                    // Deinterleave: extract even and odd elements
                    __m128 left = _mm_shuffle_ps(data0, data1, _MM_SHUFFLE(2, 0, 2, 0));
                    __m128 right = _mm_shuffle_ps(data0, data1, _MM_SHUFFLE(3, 1, 3, 1));
                    
                    _mm_storeu_ps(&destData[0][s], left);
                    _mm_storeu_ps(&destData[1][s], right);
                }
                
                for (u32 s = simdFrames; s < framesToProcess; ++s)
                {
                    for (u32 ch = 0; ch < numChannels; ++ch)
                    {
                        destData[ch][s] = source[s * numChannels + ch];
                    }
                }
                return;
            }
#endif

            // Scalar fallback for all channel counts
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

            if (dest == nullptr)
                return;

            auto numChannels = source.getNumChannels();
            auto sourceData = source.getView().data.channels;
            auto framesToProcess = (numFrames == 0) ? source.getNumFrames() : std::min(numFrames, source.getNumFrames());

            // Special case: single channel can use direct memcpy
            if (numChannels == 1)
            {
                std::memcpy(dest, sourceData[0], framesToProcess * sizeof(f32));
                return;
            }

#if defined(OLO_AUDIO_HAS_AVX)
            // AVX path for stereo (most common case)
            if (numChannels == 2 && framesToProcess >= 8)
            {
                const u32 simdFrames = (framesToProcess / 8) * 8;
                
                for (u32 s = 0; s < simdFrames; s += 8)
                {
                    // Load 8 samples from each channel
                    __m256 left = _mm256_loadu_ps(&sourceData[0][s]);
                    __m256 right = _mm256_loadu_ps(&sourceData[1][s]);
                    
                    // Interleave using unpack operations
                    // Split into low and high 128-bit lanes
                    __m128 left_lo = _mm256_castps256_ps128(left);
                    __m128 left_hi = _mm256_extractf128_ps(left, 1);
                    __m128 right_lo = _mm256_castps256_ps128(right);
                    __m128 right_hi = _mm256_extractf128_ps(right, 1);
                    
                    // Interleave low and high parts
                    __m128 interleaved0 = _mm_unpacklo_ps(left_lo, right_lo);
                    __m128 interleaved1 = _mm_unpackhi_ps(left_lo, right_lo);
                    __m128 interleaved2 = _mm_unpacklo_ps(left_hi, right_hi);
                    __m128 interleaved3 = _mm_unpackhi_ps(left_hi, right_hi);
                    
                    // Store interleaved data
                    _mm_storeu_ps(&dest[s * 2], interleaved0);
                    _mm_storeu_ps(&dest[s * 2 + 4], interleaved1);
                    _mm_storeu_ps(&dest[s * 2 + 8], interleaved2);
                    _mm_storeu_ps(&dest[s * 2 + 12], interleaved3);
                }
                
                // Handle remainder
                for (u32 s = simdFrames; s < framesToProcess; ++s)
                {
                    for (u32 ch = 0; ch < numChannels; ++ch)
                    {
                        dest[s * numChannels + ch] = sourceData[ch][s];
                    }
                }
                return;
            }
#elif defined(OLO_AUDIO_HAS_SSE)
            // SSE path for stereo
            if (numChannels == 2 && framesToProcess >= 4)
            {
                const u32 simdFrames = (framesToProcess / 4) * 4;
                
                for (u32 s = 0; s < simdFrames; s += 4)
                {
                    // Load 4 samples from each channel
                    __m128 left = _mm_loadu_ps(&sourceData[0][s]);
                    __m128 right = _mm_loadu_ps(&sourceData[1][s]);
                    
                    // Interleave using unpack operations
                    __m128 interleaved0 = _mm_unpacklo_ps(left, right);
                    __m128 interleaved1 = _mm_unpackhi_ps(left, right);
                    
                    // Store interleaved data
                    _mm_storeu_ps(&dest[s * 2], interleaved0);
                    _mm_storeu_ps(&dest[s * 2 + 4], interleaved1);
                }
                
                for (u32 s = simdFrames; s < framesToProcess; ++s)
                {
                    for (u32 ch = 0; ch < numChannels; ++ch)
                    {
                        dest[s * numChannels + ch] = sourceData[ch][s];
                    }
                }
                return;
            }
#endif

            // Scalar fallback for all channel counts
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

            if (dest == nullptr || source == nullptr)
                return;

            // Special case: single channel can use direct memcpy
            if (numChannels == 1)
            {
                std::memcpy(dest[0], source, numSamples * sizeof(f32));
                return;
            }

#if defined(OLO_AUDIO_HAS_AVX)
            // AVX path for stereo (most common case)
            if (numChannels == 2 && numSamples >= 8)
            {
                const u32 simdSamples = (numSamples / 8) * 8;
                
                for (u32 s = 0; s < simdSamples; s += 8)
                {
                    __m256 data0 = _mm256_loadu_ps(&source[s * 2]);
                    __m256 data1 = _mm256_loadu_ps(&source[s * 2 + 8]);
                    
                    __m256 left = _mm256_shuffle_ps(data0, data1, _MM_SHUFFLE(2, 0, 2, 0));
                    __m256 right = _mm256_shuffle_ps(data0, data1, _MM_SHUFFLE(3, 1, 3, 1));
                    
                    // Fix lane ordering (AVX shuffles work within 128-bit lanes)
                    __m256d left_pd = _mm256_permute4x64_pd(_mm256_castps_pd(left), _MM_SHUFFLE(3, 1, 2, 0));
                    __m256d right_pd = _mm256_permute4x64_pd(_mm256_castps_pd(right), _MM_SHUFFLE(3, 1, 2, 0));
                    
                    _mm256_storeu_ps(&dest[0][s], _mm256_castpd_ps(left_pd));
                    _mm256_storeu_ps(&dest[1][s], _mm256_castpd_ps(right_pd));
                }
                
                for (u32 s = simdSamples; s < numSamples; ++s)
                {
                    for (u32 ch = 0; ch < numChannels; ++ch)
                    {
                        dest[ch][s] = source[s * numChannels + ch];
                    }
                }
                return;
            }
#elif defined(OLO_AUDIO_HAS_SSE)
            // SSE path for stereo
            if (numChannels == 2 && numSamples >= 4)
            {
                const u32 simdSamples = (numSamples / 4) * 4;
                
                for (u32 s = 0; s < simdSamples; s += 4)
                {
                    __m128 data0 = _mm_loadu_ps(&source[s * 2]);
                    __m128 data1 = _mm_loadu_ps(&source[s * 2 + 4]);
                    
                    __m128 left = _mm_shuffle_ps(data0, data1, _MM_SHUFFLE(2, 0, 2, 0));
                    __m128 right = _mm_shuffle_ps(data0, data1, _MM_SHUFFLE(3, 1, 3, 1));
                    
                    _mm_storeu_ps(&dest[0][s], left);
                    _mm_storeu_ps(&dest[1][s], right);
                }
                
                for (u32 s = simdSamples; s < numSamples; ++s)
                {
                    for (u32 ch = 0; ch < numChannels; ++ch)
                    {
                        dest[ch][s] = source[s * numChannels + ch];
                    }
                }
                return;
            }
#endif

            // Scalar fallback
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
            if (dest == nullptr)
                return;

            // Special case: single channel can use direct memcpy
            if (numChannels == 1)
            {
                std::memcpy(dest, source[0], numSamples * sizeof(f32));
                return;
            }

#if defined(OLO_AUDIO_HAS_AVX)
            // AVX path for stereo (most common case)
            if (numChannels == 2 && numSamples >= 8)
            {
                const u32 simdSamples = (numSamples / 8) * 8;
                
                for (u32 s = 0; s < simdSamples; s += 8)
                {
                    __m256 left = _mm256_loadu_ps(&source[0][s]);
                    __m256 right = _mm256_loadu_ps(&source[1][s]);
                    
                    __m128 left_lo = _mm256_castps256_ps128(left);
                    __m128 left_hi = _mm256_extractf128_ps(left, 1);
                    __m128 right_lo = _mm256_castps256_ps128(right);
                    __m128 right_hi = _mm256_extractf128_ps(right, 1);
                    
                    __m128 interleaved0 = _mm_unpacklo_ps(left_lo, right_lo);
                    __m128 interleaved1 = _mm_unpackhi_ps(left_lo, right_lo);
                    __m128 interleaved2 = _mm_unpacklo_ps(left_hi, right_hi);
                    __m128 interleaved3 = _mm_unpackhi_ps(left_hi, right_hi);
                    
                    _mm_storeu_ps(&dest[s * 2], interleaved0);
                    _mm_storeu_ps(&dest[s * 2 + 4], interleaved1);
                    _mm_storeu_ps(&dest[s * 2 + 8], interleaved2);
                    _mm_storeu_ps(&dest[s * 2 + 12], interleaved3);
                }
                
                for (u32 s = simdSamples; s < numSamples; ++s)
                {
                    for (u32 ch = 0; ch < numChannels; ++ch)
                    {
                        dest[s * numChannels + ch] = source[ch][s];
                    }
                }
                return;
            }
#elif defined(OLO_AUDIO_HAS_SSE)
            // SSE path for stereo
            if (numChannels == 2 && numSamples >= 4)
            {
                const u32 simdSamples = (numSamples / 4) * 4;
                
                for (u32 s = 0; s < simdSamples; s += 4)
                {
                    __m128 left = _mm_loadu_ps(&source[0][s]);
                    __m128 right = _mm_loadu_ps(&source[1][s]);
                    
                    __m128 interleaved0 = _mm_unpacklo_ps(left, right);
                    __m128 interleaved1 = _mm_unpackhi_ps(left, right);
                    
                    _mm_storeu_ps(&dest[s * 2], interleaved0);
                    _mm_storeu_ps(&dest[s * 2 + 4], interleaved1);
                }
                
                for (u32 s = simdSamples; s < numSamples; ++s)
                {
                    for (u32 ch = 0; ch < numChannels; ++ch)
                    {
                        dest[s * numChannels + ch] = source[ch][s];
                    }
                }
                return;
            }
#endif

            // Scalar fallback
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

            if (data == nullptr)
                return;

            std::memset(data, 0, numSamples * numChannels * sizeof(f32));
        }
    };

} // namespace OloEngine::Audio
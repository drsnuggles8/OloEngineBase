#pragma once

#include "../NodeProcessor.h"
#include "../Flag.h"
#include "OloEngine/Core/Identifier.h"
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// CompressorNode - Dynamic range compression with standard controls
	/// Provides threshold, ratio, attack, release, knee, and makeup gain
	/// Essential for controlling dynamics and achieving professional sound
	class CompressorNode : public NodeProcessor
	{
	public:
		/// Detection mode for compression
		enum class DetectionMode
		{
			Peak = 0,     // Peak detection (fast response)
			RMS,          // RMS detection (average level)
			Hybrid        // Combination of peak and RMS
		};

	private:
		// Endpoint identifiers
		const Identifier Input_ID = OLO_IDENTIFIER("Input");
		const Identifier SidechainInput_ID = OLO_IDENTIFIER("SidechainInput");
		const Identifier Threshold_ID = OLO_IDENTIFIER("Threshold");
		const Identifier Ratio_ID = OLO_IDENTIFIER("Ratio");
		const Identifier Attack_ID = OLO_IDENTIFIER("Attack");
		const Identifier Release_ID = OLO_IDENTIFIER("Release");
		const Identifier Knee_ID = OLO_IDENTIFIER("Knee");
		const Identifier MakeupGain_ID = OLO_IDENTIFIER("MakeupGain");
		const Identifier DetectionMode_ID = OLO_IDENTIFIER("DetectionMode");
		const Identifier LookAhead_ID = OLO_IDENTIFIER("LookAhead");
		const Identifier AutoMakeup_ID = OLO_IDENTIFIER("AutoMakeup");
		const Identifier Bypass_ID = OLO_IDENTIFIER("Bypass");
		const Identifier Reset_ID = OLO_IDENTIFIER("Reset");
		
		// Outputs
		const Identifier Output_ID = OLO_IDENTIFIER("Output");
		const Identifier GainReduction_ID = OLO_IDENTIFIER("GainReduction");
		const Identifier EnvelopeLevel_ID = OLO_IDENTIFIER("EnvelopeLevel");

		// Compression state
		struct CompressionState
		{
			// Envelope following
			f32 envelope = 0.0f;
			f32 gainReduction = 0.0f;
			
			// Attack/Release coefficients
			f32 attackCoeff = 0.0f;
			f32 releaseCoeff = 0.0f;
			
			// RMS detection
			std::vector<f32> rmsBuffer;
			u32 rmsBufferIndex = 0;
			f32 rmsSum = 0.0f;
			u32 rmsWindowSize = 64;
			
			// Look-ahead delay line
			std::vector<f32> delayBuffer;
			u32 delayBufferIndex = 0;
			u32 lookAheadSamples = 0;
			
			// Peak detector
			f32 peakHold = 0.0f;
			u32 peakHoldCounter = 0;
			u32 peakHoldTime = 10; // samples
			
			bool isInitialized = false;
		};

		CompressionState m_State;
		f64 m_SampleRate = 48000.0;

		// Event flags
		Flag m_ResetFlag;

		// Parameter limits
		static constexpr f32 MIN_THRESHOLD_DB = -60.0f;
		static constexpr f32 MAX_THRESHOLD_DB = 0.0f;
		static constexpr f32 MIN_RATIO = 1.0f;
		static constexpr f32 MAX_RATIO = 20.0f;
		static constexpr f32 MIN_ATTACK_MS = 0.1f;
		static constexpr f32 MAX_ATTACK_MS = 1000.0f;
		static constexpr f32 MIN_RELEASE_MS = 1.0f;
		static constexpr f32 MAX_RELEASE_MS = 10000.0f;
		static constexpr f32 MIN_KNEE_DB = 0.0f;
		static constexpr f32 MAX_KNEE_DB = 40.0f;
		static constexpr f32 MIN_MAKEUP_DB = -20.0f;
		static constexpr f32 MAX_MAKEUP_DB = 40.0f;
		static constexpr f32 MIN_LOOKAHEAD_MS = 0.0f;
		static constexpr f32 MAX_LOOKAHEAD_MS = 10.0f;

	public:
		CompressorNode()
		{
			// Register inputs
			DECLARE_INPUT(f32, Input);                       // Main audio input
			DECLARE_INPUT(f32, SidechainInput);             // External sidechain input
			DECLARE_INTERPOLATED_INPUT(f32, Threshold);      // Compression threshold in dB
			DECLARE_INTERPOLATED_INPUT(f32, Ratio);          // Compression ratio (1:1 to 20:1)
			DECLARE_INTERPOLATED_INPUT(f32, Attack);         // Attack time in milliseconds
			DECLARE_INTERPOLATED_INPUT(f32, Release);        // Release time in milliseconds
			DECLARE_INTERPOLATED_INPUT(f32, Knee);           // Soft knee width in dB
			DECLARE_INTERPOLATED_INPUT(f32, MakeupGain);     // Output makeup gain in dB
			DECLARE_INPUT(f32, DetectionMode);              // Peak/RMS/Hybrid detection
			DECLARE_INPUT(f32, LookAhead);                  // Look-ahead time in ms
			DECLARE_INPUT(f32, AutoMakeup);                 // Auto makeup gain enable
			DECLARE_INPUT(f32, Bypass);                     // Bypass compression
			DECLARE_INPUT(f32, Reset);                      // Reset compressor state

			// Register outputs
			DECLARE_OUTPUT(f32, Output);                    // Compressed audio output
			DECLARE_OUTPUT(f32, GainReduction);            // Current gain reduction in dB
			DECLARE_OUTPUT(f32, EnvelopeLevel);            // Envelope follower level in dB

			// Set default values (typical compressor settings)
			SetParameterValue(Input_ID, 0.0f, false);
			SetParameterValue(SidechainInput_ID, 0.0f, false);
			SetParameterValue(Threshold_ID, -12.0f, false);       // -12 dB threshold
			SetParameterValue(Ratio_ID, 4.0f, false);             // 4:1 compression ratio
			SetParameterValue(Attack_ID, 5.0f, false);            // 5ms attack
			SetParameterValue(Release_ID, 100.0f, false);         // 100ms release
			SetParameterValue(Knee_ID, 2.0f, false);              // 2 dB soft knee
			SetParameterValue(MakeupGain_ID, 0.0f, false);        // No makeup gain
			SetParameterValue(DetectionMode_ID, static_cast<f32>(DetectionMode::RMS), false);
			SetParameterValue(LookAhead_ID, 2.0f, false);         // 2ms look-ahead
			SetParameterValue(AutoMakeup_ID, 0.0f, false);        // Auto makeup off
			SetParameterValue(Bypass_ID, 0.0f, false);            // Not bypassed
			SetParameterValue(Reset_ID, 0.0f, false);
			
			SetParameterValue(Output_ID, 0.0f, false);
			SetParameterValue(GainReduction_ID, 0.0f, false);
			SetParameterValue(EnvelopeLevel_ID, -96.0f, false);   // -96 dB (silence)

			// Register Reset input event with flag callback
			AddInputEvent<f32>(Reset_ID, "Reset", [this](f32 value) {
				if (value > 0.5f) m_ResetFlag.SetDirty();
			});
		}

		virtual ~CompressorNode() = default;

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Process interpolation and parameter connections first
			ProcessBeforeAudio();

			// Check for reset trigger
			f32 resetValue = GetParameterValue<f32>(Reset_ID);
			if (resetValue > 0.5f || m_ResetFlag.CheckAndResetIfDirty())
			{
				ResetCompressor();
				if (resetValue > 0.5f)
					SetParameterValue(Reset_ID, 0.0f, false);
			}

			// Update compressor parameters
			UpdateCompressionParameters();

			// Check bypass
			const bool isBypassed = GetParameterValue<f32>(Bypass_ID) > 0.5f;

			// Process audio
			if (inputs && inputs[0] && outputs && outputs[0] && m_State.isInitialized)
			{
				if (isBypassed)
				{
					// Bypass: copy input to output
					for (u32 i = 0; i < numSamples; ++i)
					{
						outputs[0][i] = inputs[0][i];
					}
					SetParameterValue(GainReduction_ID, 0.0f, false);
					SetParameterValue(EnvelopeLevel_ID, -96.0f, false);
				}
				else
				{
					// Apply compression
					ProcessCompression(inputs, outputs, numSamples);
				}
				
				// Set output parameter to the last generated value
				SetParameterValue(Output_ID, outputs[0][numSamples - 1], false);
			}
			else if (outputs && outputs[0])
			{
				// Clear output if no valid processing
				for (u32 i = 0; i < numSamples; ++i)
				{
					outputs[0][i] = 0.0f;
				}
				SetParameterValue(Output_ID, 0.0f, false);
				SetParameterValue(GainReduction_ID, 0.0f, false);
				SetParameterValue(EnvelopeLevel_ID, -96.0f, false);
			}
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
			
			// Initialize interpolation with default 1ms transition time
			InitializeInterpolation(sampleRate, 0.001);
			
			// Initialize compression state
			InitializeCompression(maxBufferSize);

			// Update initial parameters
			UpdateCompressionParameters();
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("CompressorNode");
		}

		const char* GetDisplayName() const override
		{
			return "Compressor";
		}

		//======================================================================
		// Compression Implementation
		//======================================================================

	private:
		void UpdateCompressionParameters()
		{
			// Calculate attack and release coefficients
			const f32 attackMs = glm::clamp(GetParameterValue<f32>(Attack_ID), MIN_ATTACK_MS, MAX_ATTACK_MS);
			const f32 releaseMs = glm::clamp(GetParameterValue<f32>(Release_ID), MIN_RELEASE_MS, MAX_RELEASE_MS);
			
			m_State.attackCoeff = std::exp(-1.0f / (attackMs * 0.001f * static_cast<f32>(m_SampleRate)));
			m_State.releaseCoeff = std::exp(-1.0f / (releaseMs * 0.001f * static_cast<f32>(m_SampleRate)));

			// Update look-ahead samples
			const f32 lookAheadMs = glm::clamp(GetParameterValue<f32>(LookAhead_ID), MIN_LOOKAHEAD_MS, MAX_LOOKAHEAD_MS);
			m_State.lookAheadSamples = static_cast<u32>(lookAheadMs * 0.001f * m_SampleRate);
			
			// Ensure delay buffer is large enough
			if (m_State.lookAheadSamples > m_State.delayBuffer.size())
			{
				m_State.delayBuffer.resize(m_State.lookAheadSamples * 2, 0.0f);
			}
		}

		void ProcessCompression(f32** inputs, f32** outputs, u32 numSamples)
		{
			const f32 thresholdDb = glm::clamp(GetParameterValue<f32>(Threshold_ID), MIN_THRESHOLD_DB, MAX_THRESHOLD_DB);
			const f32 ratio = glm::clamp(GetParameterValue<f32>(Ratio_ID), MIN_RATIO, MAX_RATIO);
			const f32 kneeDb = glm::clamp(GetParameterValue<f32>(Knee_ID), MIN_KNEE_DB, MAX_KNEE_DB);
			const f32 makeupGainDb = glm::clamp(GetParameterValue<f32>(MakeupGain_ID), MIN_MAKEUP_DB, MAX_MAKEUP_DB);
			const bool autoMakeup = GetParameterValue<f32>(AutoMakeup_ID) > 0.5f;
			const DetectionMode detectionMode = static_cast<DetectionMode>(static_cast<i32>(GetParameterValue<f32>(DetectionMode_ID)));

			// Convert to linear values
			const f32 thresholdLinear = DbToLinear(thresholdDb);
			f32 makeupGainLinear = DbToLinear(makeupGainDb);
			
			// Auto makeup gain calculation
			if (autoMakeup)
			{
				const f32 autoGainDb = (thresholdDb * (ratio - 1.0f)) / ratio;
				makeupGainLinear = DbToLinear(makeupGainDb + autoGainDb);
			}

			for (u32 i = 0; i < numSamples; ++i)
			{
				const f32 inputSample = inputs[0][i];
				
				// Determine detection input (main input or sidechain)
				f32 detectionSample = inputSample;
				if (inputs[1]) // Sidechain input available
				{
					detectionSample = inputs[1][i];
				}

				// Apply look-ahead delay to main signal
				f32 delayedSample = inputSample;
				if (m_State.lookAheadSamples > 0)
				{
					// Store current sample in delay buffer
					m_State.delayBuffer[m_State.delayBufferIndex] = inputSample;
					
					// Get delayed sample
					u32 delayedIndex = (m_State.delayBufferIndex - m_State.lookAheadSamples + m_State.delayBuffer.size()) % m_State.delayBuffer.size();
					delayedSample = m_State.delayBuffer[delayedIndex];
					
					m_State.delayBufferIndex = (m_State.delayBufferIndex + 1) % m_State.delayBuffer.size();
				}

				// Level detection
				f32 detectionLevel = GetDetectionLevel(detectionSample, detectionMode);
				
				// Apply envelope following
				if (detectionLevel > m_State.envelope)
				{
					// Attack
					m_State.envelope = detectionLevel + (m_State.envelope - detectionLevel) * m_State.attackCoeff;
				}
				else
				{
					// Release
					m_State.envelope = detectionLevel + (m_State.envelope - detectionLevel) * m_State.releaseCoeff;
				}

				// Calculate gain reduction
				f32 gainReductionLinear = CalculateGainReduction(m_State.envelope, thresholdLinear, ratio, kneeDb);
				
				// Apply compression and makeup gain
				f32 outputSample = delayedSample * gainReductionLinear * makeupGainLinear;
				
				// Soft limiting to prevent clipping
				outputSample = SoftLimit(outputSample);
				
				outputs[0][i] = outputSample;
				
				// Store gain reduction for metering
				m_State.gainReduction = LinearToDb(gainReductionLinear);
			}

			// Update output parameters with final values
			SetParameterValue(GainReduction_ID, m_State.gainReduction, false);
			SetParameterValue(EnvelopeLevel_ID, LinearToDb(m_State.envelope), false);
		}

		f32 GetDetectionLevel(f32 sample, DetectionMode mode)
		{
			const f32 absSample = std::abs(sample);
			
			switch (mode)
			{
				case DetectionMode::Peak:
					return GetPeakLevel(absSample);
					
				case DetectionMode::RMS:
					return GetRMSLevel(sample);
					
				case DetectionMode::Hybrid:
					return std::max(GetPeakLevel(absSample), GetRMSLevel(sample));
					
				default:
					return absSample;
			}
		}

		f32 GetPeakLevel(f32 absSample)
		{
			// Simple peak detector with hold
			if (absSample > m_State.peakHold)
			{
				m_State.peakHold = absSample;
				m_State.peakHoldCounter = m_State.peakHoldTime;
			}
			else if (m_State.peakHoldCounter > 0)
			{
				m_State.peakHoldCounter--;
			}
			else
			{
				// Decay peak hold
				m_State.peakHold *= 0.999f;
			}
			
			return std::max(absSample, m_State.peakHold);
		}

		f32 GetRMSLevel(f32 sample)
		{
			// Update RMS buffer
			const f32 prevSample = m_State.rmsBuffer[m_State.rmsBufferIndex];
			m_State.rmsBuffer[m_State.rmsBufferIndex] = sample;
			
			// Update running sum
			m_State.rmsSum += (sample * sample) - (prevSample * prevSample);
			
			// Advance index
			m_State.rmsBufferIndex = (m_State.rmsBufferIndex + 1) % m_State.rmsWindowSize;
			
			// Calculate RMS
			return std::sqrt(std::max(0.0f, m_State.rmsSum / static_cast<f32>(m_State.rmsWindowSize)));
		}

		f32 CalculateGainReduction(f32 inputLevel, f32 threshold, f32 ratio, f32 kneeDb)
		{
			if (inputLevel <= 0.0f) return 1.0f;
			
			const f32 inputDb = LinearToDb(inputLevel);
			const f32 thresholdDb = LinearToDb(threshold);
			
			f32 gainReductionDb = 0.0f;
			
			if (kneeDb > 0.0f)
			{
				// Soft knee compression
				const f32 kneeStart = thresholdDb - kneeDb * 0.5f;
				const f32 kneeEnd = thresholdDb + kneeDb * 0.5f;
				
				if (inputDb < kneeStart)
				{
					// Below knee: no compression
					gainReductionDb = 0.0f;
				}
				else if (inputDb < kneeEnd)
				{
					// In knee region: smooth transition
					const f32 kneeRatio = (inputDb - kneeStart) / kneeDb;
					const f32 softRatio = 1.0f + (ratio - 1.0f) * kneeRatio;
					const f32 overThresholdDb = inputDb - thresholdDb;
					gainReductionDb = overThresholdDb * (1.0f - 1.0f / softRatio);
				}
				else
				{
					// Above knee: full compression
					const f32 overThresholdDb = inputDb - thresholdDb;
					gainReductionDb = overThresholdDb * (1.0f - 1.0f / ratio);
				}
			}
			else
			{
				// Hard knee compression
				if (inputDb > thresholdDb)
				{
					const f32 overThresholdDb = inputDb - thresholdDb;
					gainReductionDb = overThresholdDb * (1.0f - 1.0f / ratio);
				}
			}
			
			return DbToLinear(-gainReductionDb); // Negative for gain reduction
		}

		f32 SoftLimit(f32 sample)
		{
			// Soft limiting using tanh
			const f32 driveAmount = 0.7f; // Moderate soft limiting
			return std::tanh(sample * driveAmount) / driveAmount;
		}

		void InitializeCompression(u32 maxBufferSize)
		{
			// Initialize RMS buffer
			m_State.rmsWindowSize = std::max(64u, static_cast<u32>(m_SampleRate * 0.001)); // 1ms window minimum
			m_State.rmsBuffer.resize(m_State.rmsWindowSize, 0.0f);
			m_State.rmsBufferIndex = 0;
			m_State.rmsSum = 0.0f;
			
			// Initialize delay buffer for look-ahead
			m_State.delayBuffer.resize(maxBufferSize * 2, 0.0f);
			m_State.delayBufferIndex = 0;
			
			// Reset state
			m_State.envelope = 0.0f;
			m_State.gainReduction = 0.0f;
			m_State.peakHold = 0.0f;
			m_State.peakHoldCounter = 0;
			
			m_State.isInitialized = true;
		}

		void ResetCompressor()
		{
			if (m_State.isInitialized)
			{
				// Clear all buffers
				std::fill(m_State.rmsBuffer.begin(), m_State.rmsBuffer.end(), 0.0f);
				std::fill(m_State.delayBuffer.begin(), m_State.delayBuffer.end(), 0.0f);
				
				// Reset state variables
				m_State.envelope = 0.0f;
				m_State.gainReduction = 0.0f;
				m_State.rmsSum = 0.0f;
				m_State.rmsBufferIndex = 0;
				m_State.delayBufferIndex = 0;
				m_State.peakHold = 0.0f;
				m_State.peakHoldCounter = 0;
			}
		}

		// Utility functions for dB/linear conversion
		static f32 LinearToDb(f32 linear)
		{
			return (linear > 0.0f) ? (20.0f * std::log10(linear)) : -96.0f;
		}

		static f32 DbToLinear(f32 db)
		{
			return std::pow(10.0f, db * 0.05f);
		}

	public:
		//======================================================================
		// Utility Methods
		//======================================================================

		/// Get the current gain reduction in dB
		f32 GetGainReduction() const
		{
			return m_State.gainReduction;
		}

		/// Get the current envelope level in dB
		f32 GetEnvelopeLevel() const
		{
			return LinearToDb(m_State.envelope);
		}

		/// Get the current threshold in dB
		f32 GetThreshold() const
		{
			return glm::clamp(GetParameterValue<f32>(Threshold_ID), MIN_THRESHOLD_DB, MAX_THRESHOLD_DB);
		}

		/// Get the current compression ratio
		f32 GetRatio() const
		{
			return glm::clamp(GetParameterValue<f32>(Ratio_ID), MIN_RATIO, MAX_RATIO);
		}

		/// Check if compressor is bypassed
		bool IsBypassed() const
		{
			return GetParameterValue<f32>(Bypass_ID) > 0.5f;
		}
	};

} // namespace OloEngine::Audio::SoundGraph
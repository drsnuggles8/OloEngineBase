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
	/// DistortionNode - Waveshaping distortion with multiple algorithms
	/// Provides soft clipping, hard clipping, tube saturation, and bit crushing
	/// Essential for adding harmonic content and character to audio signals
	class DistortionNode : public NodeProcessor
	{
	public:
		/// Distortion algorithm types
		enum class DistortionType
		{
			SoftClip = 0,     // Smooth saturation curve
			HardClip,         // Hard limiting
			TubeSaturation,   // Tube amplifier simulation
			BitCrushing,      // Digital bit reduction
			Wavefolder,       // Wave folding distortion
			Fuzz,             // Aggressive fuzz distortion
			Overdrive         // Soft overdrive curve
		};

	private:
		// Endpoint identifiers
		const Identifier Input_ID = OLO_IDENTIFIER("Input");
		const Identifier DistortionType_ID = OLO_IDENTIFIER("DistortionType");
		const Identifier Drive_ID = OLO_IDENTIFIER("Drive");
		const Identifier Tone_ID = OLO_IDENTIFIER("Tone");
		const Identifier OutputLevel_ID = OLO_IDENTIFIER("OutputLevel");
		const Identifier WetDryMix_ID = OLO_IDENTIFIER("WetDryMix");
		
		// Bit crushing parameters
		const Identifier BitDepth_ID = OLO_IDENTIFIER("BitDepth");
		const Identifier SampleRateReduction_ID = OLO_IDENTIFIER("SampleRateReduction");
		
		// Tube saturation parameters
		const Identifier WarmthAmount_ID = OLO_IDENTIFIER("WarmthAmount");
		const Identifier AsymmetryAmount_ID = OLO_IDENTIFIER("AsymmetryAmount");
		
		// Control parameters
		const Identifier Bypass_ID = OLO_IDENTIFIER("Bypass");
		const Identifier Reset_ID = OLO_IDENTIFIER("Reset");
		
		// Outputs
		const Identifier Output_ID = OLO_IDENTIFIER("Output");
		const Identifier HarmonicContent_ID = OLO_IDENTIFIER("HarmonicContent");

		// Distortion state
		struct DistortionState
		{
			// Tone filtering (simple high/low shelf)
			f32 toneFilterState = 0.0f;
			f32 toneFilterCoeff = 0.0f;
			
			// Bit crushing state
			f32 bitCrushHold = 0.0f;
			u32 bitCrushCounter = 0;
			u32 bitCrushPeriod = 1;
			
			// Tube saturation state
			f32 tubeStatePos = 0.0f;
			f32 tubeStateNeg = 0.0f;
			f32 tubeBias = 0.0f;
			
			// DC blocking filter
			f32 dcBlockerX = 0.0f;
			f32 dcBlockerY = 0.0f;
			
			// Harmonic analysis
			f32 harmonicContent = 0.0f;
			f32 inputRMS = 0.0f;
			f32 outputRMS = 0.0f;
			
			bool isInitialized = false;
		};

		DistortionState m_State;
		f64 m_SampleRate = 48000.0;

		// Event flags
		Flag m_ResetFlag;

		// Parameter limits
		static constexpr f32 MIN_DRIVE_DB = 0.0f;
		static constexpr f32 MAX_DRIVE_DB = 40.0f;
		static constexpr f32 MIN_TONE = -1.0f;        // -1 = dark, +1 = bright
		static constexpr f32 MAX_TONE = 1.0f;
		static constexpr f32 MIN_OUTPUT_DB = -40.0f;
		static constexpr f32 MAX_OUTPUT_DB = 20.0f;
		static constexpr f32 MIN_MIX = 0.0f;          // 0 = dry, 1 = wet
		static constexpr f32 MAX_MIX = 1.0f;
		static constexpr f32 MIN_BIT_DEPTH = 1.0f;
		static constexpr f32 MAX_BIT_DEPTH = 16.0f;
		static constexpr f32 MIN_SAMPLE_RATE_REDUCTION = 1.0f;
		static constexpr f32 MAX_SAMPLE_RATE_REDUCTION = 50.0f;
		static constexpr f32 MIN_WARMTH = 0.0f;
		static constexpr f32 MAX_WARMTH = 2.0f;
		static constexpr f32 MIN_ASYMMETRY = -1.0f;
		static constexpr f32 MAX_ASYMMETRY = 1.0f;

	public:
		DistortionNode()
		{
			// Register inputs
			DECLARE_INPUT(f32, Input);                       // Audio input
			DECLARE_INPUT(f32, DistortionType);             // Distortion algorithm
			DECLARE_INTERPOLATED_INPUT(f32, Drive);          // Input gain/drive amount
			DECLARE_INTERPOLATED_INPUT(f32, Tone);           // Tone shaping (-1 to +1)
			DECLARE_INTERPOLATED_INPUT(f32, OutputLevel);    // Output level compensation
			DECLARE_INTERPOLATED_INPUT(f32, WetDryMix);      // Wet/dry mix (0-1)
			
			// Bit crushing parameters
			DECLARE_INPUT(f32, BitDepth);                   // Bit depth reduction
			DECLARE_INPUT(f32, SampleRateReduction);        // Sample rate reduction factor
			
			// Tube saturation parameters
			DECLARE_INTERPOLATED_INPUT(f32, WarmthAmount);   // Tube warmth/saturation
			DECLARE_INTERPOLATED_INPUT(f32, AsymmetryAmount); // Asymmetric clipping
			
			// Control parameters
			DECLARE_INPUT(f32, Bypass);                     // Bypass distortion
			DECLARE_INPUT(f32, Reset);                      // Reset distortion state

			// Register outputs
			DECLARE_OUTPUT(f32, Output);                    // Distorted audio output
			DECLARE_OUTPUT(f32, HarmonicContent);          // Harmonic content estimate

			// Set default values
			SetParameterValue(Input_ID, 0.0f, false);
			SetParameterValue(DistortionType_ID, static_cast<f32>(DistortionType::SoftClip), false);
			SetParameterValue(Drive_ID, 10.0f, false);         // 10 dB drive
			SetParameterValue(Tone_ID, 0.0f, false);           // Neutral tone
			SetParameterValue(OutputLevel_ID, -6.0f, false);   // -6 dB output compensation
			SetParameterValue(WetDryMix_ID, 1.0f, false);      // 100% wet
			
			SetParameterValue(BitDepth_ID, 8.0f, false);       // 8-bit crushing
			SetParameterValue(SampleRateReduction_ID, 4.0f, false); // 4x sample rate reduction
			
			SetParameterValue(WarmthAmount_ID, 0.5f, false);   // Moderate warmth
			SetParameterValue(AsymmetryAmount_ID, 0.1f, false); // Slight asymmetry
			
			SetParameterValue(Bypass_ID, 0.0f, false);         // Not bypassed
			SetParameterValue(Reset_ID, 0.0f, false);
			
			SetParameterValue(Output_ID, 0.0f, false);
			SetParameterValue(HarmonicContent_ID, 0.0f, false);

			// Register Reset input event with flag callback
			AddInputEvent<f32>(Reset_ID, "Reset", [this](f32 value) {
				if (value > 0.5f) m_ResetFlag.SetDirty();
			});
		}

		virtual ~DistortionNode() = default;

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
				ResetDistortion();
				if (resetValue > 0.5f)
					SetParameterValue(Reset_ID, 0.0f, false);
			}

			// Update distortion parameters
			UpdateDistortionParameters();

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
					SetParameterValue(HarmonicContent_ID, 0.0f, false);
				}
				else
				{
					// Apply distortion
					ProcessDistortion(inputs[0], outputs[0], numSamples);
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
				SetParameterValue(HarmonicContent_ID, 0.0f, false);
			}
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
			
			// Initialize interpolation with default 1ms transition time
			InitializeInterpolation(sampleRate, 0.001);
			
			// Initialize distortion state
			InitializeDistortion();

			// Update initial parameters
			UpdateDistortionParameters();
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("DistortionNode");
		}

		const char* GetDisplayName() const override
		{
			return "Distortion";
		}

		//======================================================================
		// Distortion Implementation
		//======================================================================

	private:
		void UpdateDistortionParameters()
		{
			// Update tone filter coefficient
			const f32 toneValue = glm::clamp(GetParameterValue<f32>(Tone_ID), MIN_TONE, MAX_TONE);
			const f32 cutoffFreq = 1000.0f + toneValue * 2000.0f; // 1kHz center, ±2kHz range
			const f32 normalizedFreq = cutoffFreq / static_cast<f32>(m_SampleRate);
			m_State.toneFilterCoeff = std::exp(-2.0f * glm::pi<f32>() * normalizedFreq);

			// Update bit crushing period
			const f32 sampleRateReduction = glm::clamp(GetParameterValue<f32>(SampleRateReduction_ID), 
													   MIN_SAMPLE_RATE_REDUCTION, MAX_SAMPLE_RATE_REDUCTION);
			m_State.bitCrushPeriod = static_cast<u32>(sampleRateReduction);
		}

		void ProcessDistortion(const f32* input, f32* output, u32 numSamples)
		{
			const DistortionType distType = static_cast<DistortionType>(static_cast<i32>(GetParameterValue<f32>(DistortionType_ID)));
			const f32 driveDb = glm::clamp(GetParameterValue<f32>(Drive_ID), MIN_DRIVE_DB, MAX_DRIVE_DB);
			const f32 driveLinear = DbToLinear(driveDb);
			const f32 outputDb = glm::clamp(GetParameterValue<f32>(OutputLevel_ID), MIN_OUTPUT_DB, MAX_OUTPUT_DB);
			const f32 outputLinear = DbToLinear(outputDb);
			const f32 wetMix = glm::clamp(GetParameterValue<f32>(WetDryMix_ID), MIN_MIX, MAX_MIX);
			const f32 dryMix = 1.0f - wetMix;

			// Accumulate RMS values for harmonic content analysis
			f32 inputRMSAccum = 0.0f;
			f32 outputRMSAccum = 0.0f;

			for (u32 i = 0; i < numSamples; ++i)
			{
				const f32 inputSample = input[i];
				
				// Apply input drive
				f32 drivenSample = inputSample * driveLinear;
				
				// Apply distortion algorithm
				f32 distortedSample = ApplyDistortion(drivenSample, distType);
				
				// Apply tone shaping
				distortedSample = ApplyToneShaping(distortedSample);
				
				// Apply output level compensation
				distortedSample *= outputLinear;
				
				// DC blocking filter
				distortedSample = ApplyDCBlocking(distortedSample);
				
				// Mix wet and dry signals
				f32 finalSample = (distortedSample * wetMix) + (inputSample * dryMix);
				
				output[i] = finalSample;

				// Accumulate RMS for harmonic analysis
				inputRMSAccum += inputSample * inputSample;
				outputRMSAccum += finalSample * finalSample;
			}

			// Update harmonic content estimate
			UpdateHarmonicContent(inputRMSAccum, outputRMSAccum, numSamples);
		}

		f32 ApplyDistortion(f32 sample, DistortionType type)
		{
			switch (type)
			{
				case DistortionType::SoftClip:
					return ApplySoftClipping(sample);
					
				case DistortionType::HardClip:
					return ApplyHardClipping(sample);
					
				case DistortionType::TubeSaturation:
					return ApplyTubeSaturation(sample);
					
				case DistortionType::BitCrushing:
					return ApplyBitCrushing(sample);
					
				case DistortionType::Wavefolder:
					return ApplyWavefolding(sample);
					
				case DistortionType::Fuzz:
					return ApplyFuzzDistortion(sample);
					
				case DistortionType::Overdrive:
					return ApplyOverdrive(sample);
					
				default:
					return sample;
			}
		}

		f32 ApplySoftClipping(f32 sample)
		{
			// Smooth saturation using tanh
			return std::tanh(sample * 2.0f) * 0.5f;
		}

		f32 ApplyHardClipping(f32 sample)
		{
			// Hard limiting at ±1.0
			return std::clamp(sample, -1.0f, 1.0f);
		}

		f32 ApplyTubeSaturation(f32 sample)
		{
			const f32 warmth = glm::clamp(GetParameterValue<f32>(WarmthAmount_ID), MIN_WARMTH, MAX_WARMTH);
			const f32 asymmetry = glm::clamp(GetParameterValue<f32>(AsymmetryAmount_ID), MIN_ASYMMETRY, MAX_ASYMMETRY);
			
			// Asymmetric tube-style saturation
			f32 result;
			if (sample >= 0.0f)
			{
				// Positive half-cycle
				const f32 drive = 1.0f + warmth + asymmetry;
				result = sample / (1.0f + std::abs(sample * drive));
				m_State.tubeStatePos = result * 0.1f + m_State.tubeStatePos * 0.9f; // Smoothing
			}
			else
			{
				// Negative half-cycle
				const f32 drive = 1.0f + warmth - asymmetry;
				result = sample / (1.0f + std::abs(sample * drive));
				m_State.tubeStateNeg = result * 0.1f + m_State.tubeStateNeg * 0.9f; // Smoothing
			}
			
			// Add slight bias for warmth
			result += m_State.tubeBias * 0.02f;
			m_State.tubeBias = m_State.tubeBias * 0.999f + result * 0.001f;
			
			return result;
		}

		f32 ApplyBitCrushing(f32 sample)
		{
			const f32 bitDepth = glm::clamp(GetParameterValue<f32>(BitDepth_ID), MIN_BIT_DEPTH, MAX_BIT_DEPTH);
			
			// Sample rate reduction
			if (m_State.bitCrushCounter++ >= m_State.bitCrushPeriod)
			{
				m_State.bitCrushCounter = 0;
				m_State.bitCrushHold = sample;
			}
			
			// Bit depth reduction
			const f32 levels = std::pow(2.0f, bitDepth);
			const f32 quantized = std::round(m_State.bitCrushHold * levels) / levels;
			
			return quantized;
		}

		f32 ApplyWavefolding(f32 sample)
		{
			// Wave folding - reflects signal when it exceeds ±1
			f32 folded = sample;
			const f32 threshold = 0.7f;
			
			while (std::abs(folded) > threshold)
			{
				if (folded > threshold)
				{
					folded = 2.0f * threshold - folded;
				}
				else if (folded < -threshold)
				{
					folded = -2.0f * threshold - folded;
				}
			}
			
			return folded;
		}

		f32 ApplyFuzzDistortion(f32 sample)
		{
			// Aggressive fuzz using sign and square root
			if (std::abs(sample) < 0.001f) return sample; // Avoid issues with small signals
			
			const f32 sign = (sample >= 0.0f) ? 1.0f : -1.0f;
			const f32 magnitude = std::abs(sample);
			const f32 fuzzed = sign * std::sqrt(magnitude) * 1.2f;
			
			return std::clamp(fuzzed, -1.0f, 1.0f);
		}

		f32 ApplyOverdrive(f32 sample)
		{
			// Soft overdrive with warm saturation
			const f32 drive = 1.5f;
			const f32 driven = sample * drive;
			
			// Soft saturation with smooth transition
			f32 result;
			if (std::abs(driven) < 0.33f)
			{
				result = driven * 2.0f;
			}
			else if (std::abs(driven) < 0.66f)
			{
				const f32 sign = (driven >= 0.0f) ? 1.0f : -1.0f;
				result = sign * (3.0f - std::pow(2.0f - 3.0f * std::abs(driven), 2.0f)) / 3.0f;
			}
			else
			{
				const f32 sign = (driven >= 0.0f) ? 1.0f : -1.0f;
				result = sign;
			}
			
			return result * 0.7f; // Scale down to prevent clipping
		}

		f32 ApplyToneShaping(f32 sample)
		{
			// Simple tone control using first-order filter
			m_State.toneFilterState = sample + (m_State.toneFilterState - sample) * m_State.toneFilterCoeff;
			
			const f32 toneValue = glm::clamp(GetParameterValue<f32>(Tone_ID), MIN_TONE, MAX_TONE);
			
			// Mix between low-pass filtered (dark) and original (bright)
			const f32 mix = (toneValue + 1.0f) * 0.5f; // Convert -1..1 to 0..1
			return m_State.toneFilterState * (1.0f - mix) + sample * mix;
		}

		f32 ApplyDCBlocking(f32 sample)
		{
			// High-pass filter to remove DC bias
			const f32 dcBlockCoeff = 0.995f;
			m_State.dcBlockerY = sample - m_State.dcBlockerX + dcBlockCoeff * m_State.dcBlockerY;
			m_State.dcBlockerX = sample;
			return m_State.dcBlockerY;
		}

		void UpdateHarmonicContent(f32 inputRMSAccum, f32 outputRMSAccum, u32 numSamples)
		{
			const f32 inputRMS = std::sqrt(inputRMSAccum / static_cast<f32>(numSamples));
			const f32 outputRMS = std::sqrt(outputRMSAccum / static_cast<f32>(numSamples));
			
			// Smooth the RMS values
			m_State.inputRMS = m_State.inputRMS * 0.9f + inputRMS * 0.1f;
			m_State.outputRMS = m_State.outputRMS * 0.9f + outputRMS * 0.1f;
			
			// Estimate harmonic content as the difference between output and input energy
			if (m_State.inputRMS > 0.001f)
			{
				const f32 energyRatio = m_State.outputRMS / m_State.inputRMS;
				m_State.harmonicContent = std::max(0.0f, energyRatio - 1.0f);
			}
			else
			{
				m_State.harmonicContent = 0.0f;
			}
			
			SetParameterValue(HarmonicContent_ID, m_State.harmonicContent, false);
		}

		void InitializeDistortion()
		{
			// Reset all state variables
			m_State.toneFilterState = 0.0f;
			m_State.toneFilterCoeff = 0.5f;
			m_State.bitCrushHold = 0.0f;
			m_State.bitCrushCounter = 0;
			m_State.bitCrushPeriod = 1;
			m_State.tubeStatePos = 0.0f;
			m_State.tubeStateNeg = 0.0f;
			m_State.tubeBias = 0.0f;
			m_State.dcBlockerX = 0.0f;
			m_State.dcBlockerY = 0.0f;
			m_State.harmonicContent = 0.0f;
			m_State.inputRMS = 0.0f;
			m_State.outputRMS = 0.0f;
			
			m_State.isInitialized = true;
		}

		void ResetDistortion()
		{
			if (m_State.isInitialized)
			{
				InitializeDistortion();
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

		/// Get the current distortion type
		DistortionType GetDistortionType() const
		{
			return static_cast<DistortionType>(static_cast<i32>(GetParameterValue<f32>(DistortionType_ID)));
		}

		/// Get current drive amount in dB
		f32 GetDrive() const
		{
			return glm::clamp(GetParameterValue<f32>(Drive_ID), MIN_DRIVE_DB, MAX_DRIVE_DB);
		}

		/// Get current tone setting
		f32 GetTone() const
		{
			return glm::clamp(GetParameterValue<f32>(Tone_ID), MIN_TONE, MAX_TONE);
		}

		/// Get current harmonic content estimate
		f32 GetHarmonicContent() const
		{
			return m_State.harmonicContent;
		}

		/// Check if distortion is bypassed
		bool IsBypassed() const
		{
			return GetParameterValue<f32>(Bypass_ID) > 0.5f;
		}

		/// Get current wet/dry mix
		f32 GetWetDryMix() const
		{
			return glm::clamp(GetParameterValue<f32>(WetDryMix_ID), MIN_MIX, MAX_MIX);
		}
	};

} // namespace OloEngine::Audio::SoundGraph
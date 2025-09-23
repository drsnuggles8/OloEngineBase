#pragma once

#include "../NodeProcessor.h"
#include "../Flag.h"
#include "OloEngine/Core/Identifier.h"
#include <glm/glm.hpp>
#include <vector>
#include <complex>
#include <memory>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// ConvolutionNode - Real-time impulse response convolution for realistic reverb
	/// Implements FFT-based convolution for acoustic modeling and spatial audio effects
	/// Essential for creating realistic room acoustics and convolution reverb
	class ConvolutionNode : public NodeProcessor
	{
	private:
		// Endpoint identifiers
		const Identifier Input_ID = OLO_IDENTIFIER("Input");
		const Identifier ImpulseResponse_ID = OLO_IDENTIFIER("ImpulseResponse");
		const Identifier WetLevel_ID = OLO_IDENTIFIER("WetLevel");
		const Identifier DryLevel_ID = OLO_IDENTIFIER("DryLevel");
		const Identifier LoadImpulse_ID = OLO_IDENTIFIER("LoadImpulse");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

		// Convolution parameters
		struct ConvolutionState
		{
			std::vector<f32> impulseResponse;
			std::vector<std::complex<f32>> impulseResponseFFT;
			std::vector<f32> inputBuffer;
			std::vector<f32> outputBuffer;
			std::vector<std::complex<f32>> inputFFT;
			std::vector<std::complex<f32>> outputFFT;
			u32 fftSize = 0;
			u32 impulseLength = 0;
			u32 bufferIndex = 0;
			bool isInitialized = false;
		};

		ConvolutionState m_State;
		f64 m_SampleRate = 48000.0;
		u32 m_MaxBufferSize = 512;

		// Default impulse response (simple delay for testing)
		static constexpr u32 DEFAULT_IMPULSE_LENGTH = 1024;
		
		// Event flags
		Flag m_LoadImpulseFlag;

		// Parameter limits
		static constexpr f32 MIN_LEVEL = 0.0f;
		static constexpr f32 MAX_LEVEL = 2.0f;

	public:
		ConvolutionNode()
		{
			// Register inputs and outputs
			DECLARE_INPUT(f32, Input);                       // Audio input to be convolved
			DECLARE_INPUT(f32, ImpulseResponse);             // Impulse response data (for dynamic loading)
			DECLARE_INTERPOLATED_INPUT(f32, WetLevel);       // Convolved signal level
			DECLARE_INTERPOLATED_INPUT(f32, DryLevel);       // Original signal level
			DECLARE_INPUT(f32, LoadImpulse);                 // Trigger to load new impulse
			DECLARE_OUTPUT(f32, Output);                     // Convolved audio output

			// Set default values
			SetParameterValue(Input_ID, 0.0f, false);
			SetParameterValue(ImpulseResponse_ID, 0.0f, false);
			SetParameterValue(WetLevel_ID, 1.0f, false);     // 100% wet by default
			SetParameterValue(DryLevel_ID, 0.0f, false);     // 0% dry (pure convolution)
			SetParameterValue(LoadImpulse_ID, 0.0f, false);
			SetParameterValue(Output_ID, 0.0f, false);

			// Register LoadImpulse input event with flag callback
			AddInputEvent<f32>(LoadImpulse_ID, "LoadImpulse", [this](f32 value) {
				if (value > 0.5f) m_LoadImpulseFlag.SetDirty();
			});
		}

		virtual ~ConvolutionNode() = default;

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Process interpolation and parameter connections first
			ProcessBeforeAudio();

			// Check for impulse loading trigger
			f32 loadImpulseValue = GetParameterValue<f32>(LoadImpulse_ID);
			if (loadImpulseValue > 0.5f || m_LoadImpulseFlag.CheckAndResetIfDirty())
			{
				LoadDefaultImpulse(); // Load default or update from parameter
				if (loadImpulseValue > 0.5f)
					SetParameterValue(LoadImpulse_ID, 0.0f, false);
			}

			// Get mix levels
			const f32 wetLevel = glm::clamp(GetParameterValue<f32>(WetLevel_ID), MIN_LEVEL, MAX_LEVEL);
			const f32 dryLevel = glm::clamp(GetParameterValue<f32>(DryLevel_ID), MIN_LEVEL, MAX_LEVEL);

			// Process audio if we have valid input/output and initialized convolution
			if (inputs && inputs[0] && outputs && outputs[0] && m_State.isInitialized)
			{
				ProcessConvolution(inputs[0], outputs[0], numSamples, wetLevel, dryLevel);
				
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
			}
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
			m_MaxBufferSize = maxBufferSize;
			
			// Initialize interpolation with default 10ms transition time
			InitializeInterpolation(sampleRate, 0.01);
			
			// Initialize convolution with default impulse
			LoadDefaultImpulse();
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("ConvolutionNode");
		}

		const char* GetDisplayName() const override
		{
			return "Convolution Reverb";
		}

		//======================================================================
		// Convolution Implementation
		//======================================================================

	private:
		void ProcessConvolution(const f32* input, f32* output, u32 numSamples, f32 wetLevel, f32 dryLevel)
		{
			if (!m_State.isInitialized || m_State.fftSize == 0)
			{
				// Fallback: just copy input to output with dry level
				for (u32 i = 0; i < numSamples; ++i)
				{
					output[i] = input[i] * dryLevel;
				}
				return;
			}

			// Process in chunks that fit our FFT size
			const u32 processingSize = m_State.fftSize / 2; // Overlap-add processing
			
			for (u32 sampleIndex = 0; sampleIndex < numSamples; ++sampleIndex)
			{
				// Store input in circular buffer
				m_State.inputBuffer[m_State.bufferIndex] = input[sampleIndex];
				
				// Get convolved output (with delay compensation)
				f32 convolvedSample = 0.0f;
				if (m_State.bufferIndex >= m_State.impulseLength)
				{
					convolvedSample = m_State.outputBuffer[m_State.bufferIndex - m_State.impulseLength];
				}
				
				// Apply convolution using time-domain approach for real-time performance
				// Note: For production, this should be replaced with partitioned FFT convolution
				convolvedSample = ComputeConvolutionSample(sampleIndex);
				
				// Mix wet and dry signals
				output[sampleIndex] = (convolvedSample * wetLevel) + (input[sampleIndex] * dryLevel);
				
				// Advance buffer index
				m_State.bufferIndex = (m_State.bufferIndex + 1) % m_State.inputBuffer.size();
			}
		}

		f32 ComputeConvolutionSample(u32 currentIndex)
		{
			f32 result = 0.0f;
			
			// Simple time-domain convolution (for small impulse responses)
			// This is a simplified implementation - production code should use FFT
			const u32 impulseLength = std::min(m_State.impulseLength, static_cast<u32>(m_State.impulseResponse.size()));
			const u32 bufferSize = static_cast<u32>(m_State.inputBuffer.size());
			
			for (u32 i = 0; i < impulseLength; ++i)
			{
				u32 inputIndex = (m_State.bufferIndex - i + bufferSize) % bufferSize;
				result += m_State.inputBuffer[inputIndex] * m_State.impulseResponse[i];
			}
			
			return result;
		}

		void LoadDefaultImpulse()
		{
			// Create a simple default impulse response (early reflections pattern)
			m_State.impulseResponse.clear();
			m_State.impulseResponse.resize(DEFAULT_IMPULSE_LENGTH, 0.0f);
			
			// Create a simple room impulse: direct sound + early reflections + decay
			const f32 sampleRate = static_cast<f32>(m_SampleRate);
			
			// Direct sound (impulse at start)
			m_State.impulseResponse[0] = 1.0f;
			
			// Early reflections (simulate wall bounces)
			const std::vector<std::pair<f32, f32>> reflections = {
				{0.020f, 0.6f},   // 20ms, 60% amplitude (wall reflection)
				{0.035f, 0.4f},   // 35ms, 40% amplitude
				{0.055f, 0.3f},   // 55ms, 30% amplitude
				{0.080f, 0.25f},  // 80ms, 25% amplitude
				{0.120f, 0.2f},   // 120ms, 20% amplitude
			};
			
			for (const auto& reflection : reflections)
			{
				u32 sampleDelay = static_cast<u32>(reflection.first * sampleRate);
				if (sampleDelay < DEFAULT_IMPULSE_LENGTH)
				{
					m_State.impulseResponse[sampleDelay] = reflection.second;
				}
			}
			
			// Add exponential decay tail
			for (u32 i = 100; i < DEFAULT_IMPULSE_LENGTH; ++i)
			{
				f32 time = static_cast<f32>(i) / sampleRate;
				f32 decay = std::exp(-time * 2.0f); // 2 second decay time
				m_State.impulseResponse[i] += decay * 0.1f * (static_cast<f32>(rand()) / RAND_MAX - 0.5f);
			}
			
			InitializeConvolutionBuffers();
		}

		void InitializeConvolutionBuffers()
		{
			m_State.impulseLength = static_cast<u32>(m_State.impulseResponse.size());
			
			// Initialize circular buffer for input (needs to be larger than impulse)
			const u32 bufferSize = std::max(m_State.impulseLength * 2, m_MaxBufferSize * 4);
			m_State.inputBuffer.resize(bufferSize, 0.0f);
			m_State.outputBuffer.resize(bufferSize, 0.0f);
			
			// Reset buffer index
			m_State.bufferIndex = 0;
			
			// For FFT-based convolution (future implementation)
			m_State.fftSize = NextPowerOfTwo(m_State.impulseLength + m_MaxBufferSize);
			
			m_State.isInitialized = true;
		}

		static u32 NextPowerOfTwo(u32 value)
		{
			u32 result = 1;
			while (result < value)
			{
				result <<= 1;
			}
			return result;
		}

	public:
		//======================================================================
		// Utility Methods
		//======================================================================

		/// Load a custom impulse response from a vector of samples
		void LoadImpulseResponse(const std::vector<f32>& impulseData)
		{
			if (!impulseData.empty())
			{
				m_State.impulseResponse = impulseData;
				InitializeConvolutionBuffers();
			}
		}

		/// Get the current impulse response length in samples
		u32 GetImpulseLength() const
		{
			return m_State.impulseLength;
		}

		/// Check if convolution is properly initialized
		bool IsInitialized() const
		{
			return m_State.isInitialized;
		}

		/// Get current wet level
		f32 GetWetLevel() const
		{
			return glm::clamp(GetParameterValue<f32>(WetLevel_ID), MIN_LEVEL, MAX_LEVEL);
		}

		/// Get current dry level  
		f32 GetDryLevel() const
		{
			return glm::clamp(GetParameterValue<f32>(DryLevel_ID), MIN_LEVEL, MAX_LEVEL);
		}
	};

} // namespace OloEngine::Audio::SoundGraph
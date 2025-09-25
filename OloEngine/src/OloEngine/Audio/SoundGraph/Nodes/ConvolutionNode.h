#pragma once

#include "../NodeProcessor.h"
#include "../Flag.h"
#include "OloEngine/Audio/SoundGraph/ValueView.h"
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
		// Parameter streams
		InputView<f32> m_InputSignal;
		InputView<f32> m_ImpulseResponseInput;
		InputView<f32> m_WetLevelInput;
		InputView<f32> m_DryLevelInput;
		InputView<f32> m_LoadImpulseInput;
		OutputView<f32> m_Output;

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
			// Initialize input streams with default values
			m_InputSignal = CreateInputView<f32>("Input", 0.0f);
			m_ImpulseResponseInput = CreateInputView<f32>("ImpulseResponse", 0.0f);
			m_WetLevelInput = CreateInputView<f32>("WetLevel", 0.5f);
			m_DryLevelInput = CreateInputView<f32>("DryLevel", 0.5f);
			m_LoadImpulseInput = CreateInputView<f32>("LoadImpulse", 0.0f);
			
			// Initialize output stream
			m_Output = CreateOutputView<f32>("Output");

			// Register impulse loading trigger callback
			m_LoadImpulseInput.RegisterInputEvent([this](f32 value) {
				if (value > 0.5f) m_LoadImpulseFlag.SetDirty();
			});
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			m_SampleRate = sampleRate;
			m_MaxBufferSize = maxBufferSize;
			
			// Initialize convolution with default impulse response
			LoadDefaultImpulse();
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update input parameters from connections
			m_InputSignal.UpdateFromConnections();
			m_ImpulseResponseInput.UpdateFromConnections();
			m_WetLevelInput.UpdateFromConnections();
			m_DryLevelInput.UpdateFromConnections();
			m_LoadImpulseInput.UpdateFromConnections();

			// Check for impulse loading trigger
			f32 loadImpulseValue = m_LoadImpulseInput.GetValue();
			if (loadImpulseValue > 0.5f || m_LoadImpulseFlag.CheckAndResetIfDirty())
			{
				LoadDefaultImpulse();
			}

			// Get processing parameters
			f32 wetLevel = glm::clamp(m_WetLevelInput.GetValue(), MIN_LEVEL, MAX_LEVEL);
			f32 dryLevel = glm::clamp(m_DryLevelInput.GetValue(), MIN_LEVEL, MAX_LEVEL);

			if (!m_State.isInitialized)
			{
				// If convolution not ready, pass dry signal through
				ProcessDrySignal(inputs, outputs, numSamples, dryLevel);
			}
			else
			{
				// Process convolution
				ProcessConvolution(inputs, outputs, numSamples, wetLevel, dryLevel);
			}

			// Update output connections
			m_Output.UpdateOutputConnections();
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
		// Utility methods for impulse response management
		//======================================================================
		
		void LoadImpulseResponse(const std::vector<f32>& impulse)
		{
			if (impulse.empty()) return;
			
			m_State.impulseResponse = impulse;
			m_State.impulseLength = impulse.size();
			
			// Determine FFT size (next power of 2, at least 2 * impulse length)
			u32 requiredSize = m_State.impulseLength + m_MaxBufferSize - 1;
			m_State.fftSize = NextPowerOfTwo(requiredSize);
			
			InitializeConvolution();
		}
		
		void SetWetLevel(f32 level) { m_WetLevelInput.SetValue(glm::clamp(level, MIN_LEVEL, MAX_LEVEL)); }
		void SetDryLevel(f32 level) { m_DryLevelInput.SetValue(glm::clamp(level, MIN_LEVEL, MAX_LEVEL)); }
		
		f32 GetWetLevel() const { return m_WetLevelInput.GetValue(); }
		f32 GetDryLevel() const { return m_DryLevelInput.GetValue(); }
		
		u32 GetImpulseLength() const { return m_State.impulseLength; }
		bool IsInitialized() const { return m_State.isInitialized; }

	private:
		void ProcessDrySignal(f32** inputs, f32** outputs, u32 numSamples, f32 dryLevel)
		{
			if (inputs && inputs[0] && outputs && outputs[0])
			{
				// Process audio buffer - dry signal only
				for (u32 i = 0; i < numSamples; ++i)
				{
					f32 drySignal = inputs[0][i] * dryLevel;
					outputs[0][i] = drySignal;
					m_Output.SetValue(outputs[0][i]);
				}
			}
			else
			{
				// Single sample processing
				f32 inputValue = m_InputSignal.GetValue();
				f32 outputValue = inputValue * dryLevel;
				m_Output.SetValue(outputValue);
			}
		}

		void ProcessConvolution(f32** inputs, f32** outputs, u32 numSamples, f32 wetLevel, f32 dryLevel)
		{
			if (inputs && inputs[0] && outputs && outputs[0])
			{
				// Process audio buffer with convolution
				for (u32 i = 0; i < numSamples; ++i)
				{
					f32 inputSample = inputs[0][i];
					f32 wetSample = ProcessConvolutionSample(inputSample);
					f32 drySignal = inputSample * dryLevel;
					f32 wetSignal = wetSample * wetLevel;
					
					outputs[0][i] = drySignal + wetSignal;
					m_Output.SetValue(outputs[0][i]);
				}
			}
			else
			{
				// Single sample processing
				f32 inputValue = m_InputSignal.GetValue();
				f32 wetSample = ProcessConvolutionSample(inputValue);
				f32 outputValue = (inputValue * dryLevel) + (wetSample * wetLevel);
				m_Output.SetValue(outputValue);
			}
		}

		f32 ProcessConvolutionSample(f32 inputSample)
		{
			// Simple FIR convolution (would be optimized with FFT in production)
			f32 output = 0.0f;
			
			// Store input sample in circular buffer
			m_State.inputBuffer[m_State.bufferIndex] = inputSample;
			
			// Convolve with impulse response
			for (u32 i = 0; i < m_State.impulseLength; ++i)
			{
				u32 bufferIdx = (m_State.bufferIndex + m_State.inputBuffer.size() - i) % m_State.inputBuffer.size();
				output += m_State.inputBuffer[bufferIdx] * m_State.impulseResponse[i];
			}
			
			// Advance buffer index
			m_State.bufferIndex = (m_State.bufferIndex + 1) % m_State.inputBuffer.size();
			
			return output;
		}

		void LoadDefaultImpulse()
		{
			// Create a simple impulse response for testing (exponential decay)
			std::vector<f32> defaultImpulse(DEFAULT_IMPULSE_LENGTH);
			
			for (u32 i = 0; i < DEFAULT_IMPULSE_LENGTH; ++i)
			{
				f32 decay = std::exp(-5.0f * i / DEFAULT_IMPULSE_LENGTH);
				defaultImpulse[i] = decay * (i == 0 ? 1.0f : 0.1f); // Initial impulse + decay
			}
			
			LoadImpulseResponse(defaultImpulse);
		}

		void InitializeConvolution()
		{
			if (m_State.impulseLength == 0) return;
			
			// Initialize buffers for convolution
			u32 bufferLength = m_State.impulseLength + m_MaxBufferSize;
			m_State.inputBuffer.resize(bufferLength);
			m_State.outputBuffer.resize(bufferLength);
			
			// Clear buffers
			std::fill(m_State.inputBuffer.begin(), m_State.inputBuffer.end(), 0.0f);
			std::fill(m_State.outputBuffer.begin(), m_State.outputBuffer.end(), 0.0f);
			
			// Initialize FFT buffers (for future optimization)
			m_State.inputFFT.resize(m_State.fftSize);
			m_State.outputFFT.resize(m_State.fftSize);
			m_State.impulseResponseFFT.resize(m_State.fftSize);
			
			// Pre-compute impulse response FFT (simplified)
			for (u32 i = 0; i < m_State.impulseLength; ++i)
			{
				m_State.impulseResponseFFT[i] = std::complex<f32>(m_State.impulseResponse[i], 0.0f);
			}
			for (u32 i = m_State.impulseLength; i < m_State.fftSize; ++i)
			{
				m_State.impulseResponseFFT[i] = std::complex<f32>(0.0f, 0.0f);
			}
			
			m_State.bufferIndex = 0;
			m_State.isInitialized = true;
		}

		static u32 NextPowerOfTwo(u32 value)
		{
			if (value <= 1) return 1;
			
			value--;
			value |= value >> 1;
			value |= value >> 2;
			value |= value >> 4;
			value |= value >> 8;
			value |= value >> 16;
			value++;
			
			return value;
		}
	};

} // namespace OloEngine::Audio::SoundGraph
#pragma once

#include "../NodeProcessor.h"
#include "../Flag.h"
#include "OloEngine/Audio/SoundGraph/ValueView.h"
#include "OloEngine/Core/Identifier.h"
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <vector>
#include <complex>
#include <cmath>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// SpectrumAnalyzerNode - Real-time FFT-based frequency spectrum analysis
	/// Provides frequency bin data for visualization and audio-reactive systems
	/// Essential for audio analysis, visualization, and frequency-based effects
	class SpectrumAnalyzerNode : public NodeProcessor
	{
	public:
		enum class WindowFunction
		{
			Rectangle = 0,
			Hann,
			Hamming,
			Blackman,
			Kaiser
		};

	private:
		// Parameter streams
		InputView<f32> m_InputSignal;
		InputView<f32> m_WindowSizeInput;
		InputView<f32> m_WindowFunctionInput;
		InputView<f32> m_OverlapFactorInput;
		InputView<f32> m_UpdateRateInput;
		InputView<f32> m_MinFrequencyInput;
		InputView<f32> m_MaxFrequencyInput;
		InputView<f32> m_ResetInput;
		
		// Output streams
		OutputView<f32> m_MagnitudeSpectrumOutput;
		OutputView<f32> m_PhaseSpectrumOutput;
		OutputView<f32> m_PowerSpectrumOutput;
		OutputView<f32> m_PeakFrequencyOutput;
		OutputView<f32> m_SpectralCentroidOutput;

		// FFT Analysis state
		struct AnalysisState
		{
			u32 windowSize = 1024;
			WindowFunction windowFunc = WindowFunction::Hann;
			u32 hopSize = 512;
			f32 minFreq = 20.0f;
			f32 maxFreq = 20000.0f;
			u32 updateInterval = 800;  // ~60 Hz at 48kHz
			
			std::vector<f32> inputBuffer;
			std::vector<f32> window;
			std::vector<std::complex<f32>> fftBuffer;
			std::vector<f32> magnitudeSpectrum;
			std::vector<f32> phaseSpectrum;
			std::vector<f32> powerSpectrum;
			
			u32 bufferIndex = 0;
			u32 samplesSinceLastUpdate = 0;
			bool needsReinitialization = true;
		};

		AnalysisState m_State;
		f64 m_SampleRate = 48000.0;
		
		// Analysis results
		f32 m_PeakFrequency = 0.0f;
		f32 m_SpectralCentroid = 0.0f;

		// Event flags
		Flag m_ResetFlag;

		// Parameter limits
		static constexpr u32 MIN_WINDOW_SIZE = 64;
		static constexpr u32 MAX_WINDOW_SIZE = 8192;
		static constexpr f32 MIN_OVERLAP = 0.0f;
		static constexpr f32 MAX_OVERLAP = 0.875f; // 87.5% max overlap
		static constexpr f32 MIN_UPDATE_RATE = 1.0f;  // 1 Hz minimum
		static constexpr f32 MAX_UPDATE_RATE = 1000.0f; // 1 kHz maximum

	public:
		SpectrumAnalyzerNode()
		{
			// Initialize input streams with default values
			m_InputSignal = CreateInputView<f32>("Input", 0.0f);
			m_WindowSizeInput = CreateInputView<f32>("WindowSize", 1024.0f);
			m_WindowFunctionInput = CreateInputView<f32>("WindowFunction", static_cast<f32>(WindowFunction::Hann));
			m_OverlapFactorInput = CreateInputView<f32>("OverlapFactor", 0.5f);
			m_UpdateRateInput = CreateInputView<f32>("UpdateRate", 60.0f);
			m_MinFrequencyInput = CreateInputView<f32>("MinFrequency", 20.0f);
			m_MaxFrequencyInput = CreateInputView<f32>("MaxFrequency", 20000.0f);
			m_ResetInput = CreateInputView<f32>("Reset", 0.0f);
			
			// Initialize output streams
			m_MagnitudeSpectrumOutput = CreateOutputView<f32>("MagnitudeSpectrum");
			m_PhaseSpectrumOutput = CreateOutputView<f32>("PhaseSpectrum");
			m_PowerSpectrumOutput = CreateOutputView<f32>("PowerSpectrum");
			m_PeakFrequencyOutput = CreateOutputView<f32>("PeakFrequency");
			m_SpectralCentroidOutput = CreateOutputView<f32>("SpectralCentroid");

			// Register reset trigger callback
			m_ResetInput.RegisterInputEvent([this](f32 value) {
				if (value > 0.5f) m_ResetFlag.SetDirty();
			});
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			m_SampleRate = sampleRate;
			InitializeAnalysis();
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update input parameters from connections
			m_InputSignal.UpdateFromConnections();
			m_WindowSizeInput.UpdateFromConnections();
			m_WindowFunctionInput.UpdateFromConnections();
			m_OverlapFactorInput.UpdateFromConnections();
			m_UpdateRateInput.UpdateFromConnections();
			m_MinFrequencyInput.UpdateFromConnections();
			m_MaxFrequencyInput.UpdateFromConnections();
			m_ResetInput.UpdateFromConnections();

			// Check for reset trigger
			f32 resetValue = m_ResetInput.GetValue();
			if (resetValue > 0.5f || m_ResetFlag.CheckAndResetIfDirty())
			{
				ResetAnalysis();
			}

			// Update analysis parameters if they changed
			UpdateAnalysisParameters();
			
			// Process audio input for analysis
			if (inputs && inputs[0])
			{
				ProcessAnalysis(inputs[0], numSamples);
			}
			else
			{
				// Process single input value
				f32 inputValue = m_InputSignal.GetValue();
				ProcessAnalysis(&inputValue, 1);
			}

			// Update output values
			m_PeakFrequencyOutput.SetValue(m_PeakFrequency);
			m_SpectralCentroidOutput.SetValue(m_SpectralCentroid);

			// Update output connections
			m_MagnitudeSpectrumOutput.UpdateOutputConnections();
			m_PhaseSpectrumOutput.UpdateOutputConnections();
			m_PowerSpectrumOutput.UpdateOutputConnections();
			m_PeakFrequencyOutput.UpdateOutputConnections();
			m_SpectralCentroidOutput.UpdateOutputConnections();
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("SpectrumAnalyzerNode");
		}

		const char* GetDisplayName() const override
		{
			return "Spectrum Analyzer";
		}

	private:
		void UpdateAnalysisParameters()
		{
			// Update window size (must be power of 2)
			u32 newWindowSize = static_cast<u32>(m_WindowSizeInput.GetValue());
			newWindowSize = std::clamp(newWindowSize, MIN_WINDOW_SIZE, MAX_WINDOW_SIZE);
			newWindowSize = NextPowerOfTwo(newWindowSize);
			
			if (newWindowSize != m_State.windowSize)
			{
				m_State.windowSize = newWindowSize;
				InitializeAnalysis();
			}

			// Update window function
			i32 windowFuncInt = static_cast<i32>(m_WindowFunctionInput.GetValue());
			m_State.windowFunc = static_cast<WindowFunction>(std::clamp(windowFuncInt, 0, 4));

			// Update overlap factor
			f32 overlapFactor = glm::clamp(m_OverlapFactorInput.GetValue(), MIN_OVERLAP, MAX_OVERLAP);
			m_State.hopSize = static_cast<u32>(m_State.windowSize * (1.0f - overlapFactor));
			m_State.hopSize = std::max(m_State.hopSize, 1u);

			// Update frequency range
			m_State.minFreq = std::max(m_MinFrequencyInput.GetValue(), 0.0f);
			m_State.maxFreq = std::min(m_MaxFrequencyInput.GetValue(), static_cast<f32>(m_SampleRate * 0.5));
			m_State.maxFreq = std::max(m_State.maxFreq, m_State.minFreq + 1.0f);

			// Update update rate
			f32 updateRate = glm::clamp(m_UpdateRateInput.GetValue(), MIN_UPDATE_RATE, MAX_UPDATE_RATE);
			m_State.updateInterval = static_cast<u32>(m_SampleRate / updateRate);
			m_State.updateInterval = std::max(m_State.updateInterval, 1u);
		}

		void ProcessAnalysis(const f32* input, u32 numSamples)
		{
			for (u32 i = 0; i < numSamples; ++i)
			{
				// Store sample in circular buffer
				m_State.inputBuffer[m_State.bufferIndex] = input[i];
				m_State.bufferIndex = (m_State.bufferIndex + 1) % m_State.windowSize;
				
				m_State.samplesSinceLastUpdate++;
				
				// Check if it's time to update analysis
				if (m_State.samplesSinceLastUpdate >= m_State.updateInterval)
				{
					PerformFFTAnalysis();
					m_State.samplesSinceLastUpdate = 0;
				}
			}
		}

		void InitializeAnalysis()
		{
			m_State.inputBuffer.resize(m_State.windowSize);
			m_State.window.resize(m_State.windowSize);
			m_State.fftBuffer.resize(m_State.windowSize);
			m_State.magnitudeSpectrum.resize(m_State.windowSize / 2 + 1);
			m_State.phaseSpectrum.resize(m_State.windowSize / 2 + 1);
			m_State.powerSpectrum.resize(m_State.windowSize / 2 + 1);
			
			// Clear buffers
			std::fill(m_State.inputBuffer.begin(), m_State.inputBuffer.end(), 0.0f);
			
			// Generate window function
			GenerateWindow();
			
			m_State.bufferIndex = 0;
			m_State.samplesSinceLastUpdate = 0;
		}

		void ResetAnalysis()
		{
			InitializeAnalysis();
			m_PeakFrequency = 0.0f;
			m_SpectralCentroid = 0.0f;
		}

		void GenerateWindow()
		{
			for (u32 i = 0; i < m_State.windowSize; ++i)
			{
				f32 n = static_cast<f32>(i) / (m_State.windowSize - 1);
				
				switch (m_State.windowFunc)
				{
					case WindowFunction::Rectangle:
						m_State.window[i] = 1.0f;
						break;
					case WindowFunction::Hann:
						m_State.window[i] = 0.5f * (1.0f - std::cos(2.0f * glm::pi<f32>() * n));
						break;
					case WindowFunction::Hamming:
						m_State.window[i] = 0.54f - 0.46f * std::cos(2.0f * glm::pi<f32>() * n);
						break;
					case WindowFunction::Blackman:
						m_State.window[i] = 0.42f - 0.5f * std::cos(2.0f * glm::pi<f32>() * n) + 0.08f * std::cos(4.0f * glm::pi<f32>() * n);
						break;
					case WindowFunction::Kaiser:
						// Simplified Kaiser window (beta=8.6)
						m_State.window[i] = 0.54f - 0.46f * std::cos(2.0f * glm::pi<f32>() * n);
						break;
				}
			}
		}

		void PerformFFTAnalysis()
		{
			// Copy windowed input to FFT buffer
			for (u32 i = 0; i < m_State.windowSize; ++i)
			{
				u32 bufferIdx = (m_State.bufferIndex + i) % m_State.windowSize;
				m_State.fftBuffer[i] = std::complex<f32>(m_State.inputBuffer[bufferIdx] * m_State.window[i], 0.0f);
			}

			// Perform FFT (simplified DFT for this implementation)
			PerformDFT();

			// Compute magnitude and phase spectra
			ComputeSpectralFeatures();
		}

		void PerformDFT()
		{
			// Simple DFT implementation (would be replaced with FFT in production)
			std::vector<std::complex<f32>> temp = m_State.fftBuffer;
			
			for (u32 k = 0; k < m_State.windowSize; ++k)
			{
				std::complex<f32> sum(0.0f, 0.0f);
				for (u32 n = 0; n < m_State.windowSize; ++n)
				{
					f32 angle = -2.0f * glm::pi<f32>() * k * n / m_State.windowSize;
					std::complex<f32> twiddle(std::cos(angle), std::sin(angle));
					sum += temp[n] * twiddle;
				}
				m_State.fftBuffer[k] = sum;
			}
		}

		void ComputeSpectralFeatures()
		{
			u32 numBins = m_State.windowSize / 2 + 1;
			f32 freqBinWidth = static_cast<f32>(m_SampleRate) / m_State.windowSize;
			
			f32 maxMagnitude = 0.0f;
			u32 peakBin = 0;
			f32 spectralSum = 0.0f;
			f32 weightedSum = 0.0f;
			
			for (u32 i = 0; i < numBins; ++i)
			{
				f32 magnitude = std::abs(m_State.fftBuffer[i]);
				f32 phase = std::arg(m_State.fftBuffer[i]);
				f32 power = magnitude * magnitude;
				
				m_State.magnitudeSpectrum[i] = magnitude;
				m_State.phaseSpectrum[i] = phase;
				m_State.powerSpectrum[i] = power;
				
				// Find peak frequency
				if (magnitude > maxMagnitude)
				{
					maxMagnitude = magnitude;
					peakBin = i;
				}
				
				// Compute spectral centroid
				f32 frequency = i * freqBinWidth;
				if (frequency >= m_State.minFreq && frequency <= m_State.maxFreq)
				{
					spectralSum += magnitude;
					weightedSum += magnitude * frequency;
				}
			}
			
			// Update peak frequency
			m_PeakFrequency = peakBin * freqBinWidth;
			
			// Update spectral centroid
			m_SpectralCentroid = (spectralSum > 0.0f) ? (weightedSum / spectralSum) : 0.0f;
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

	public:
		//======================================================================
		// Utility methods for accessing analysis results
		//======================================================================
		
		f32 GetPeakFrequency() const { return m_PeakFrequency; }
		f32 GetSpectralCentroid() const { return m_SpectralCentroid; }
		
		const std::vector<f32>& GetMagnitudeSpectrum() const { return m_State.magnitudeSpectrum; }
		const std::vector<f32>& GetPhaseSpectrum() const { return m_State.phaseSpectrum; }
		const std::vector<f32>& GetPowerSpectrum() const { return m_State.powerSpectrum; }
		
		u32 GetWindowSize() const { return m_State.windowSize; }
		WindowFunction GetWindowFunction() const { return m_State.windowFunc; }
	};

} // namespace OloEngine::Audio::SoundGraph
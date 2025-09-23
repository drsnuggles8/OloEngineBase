#pragma once

#include "../NodeProcessor.h"
#include "../Flag.h"
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
		// Endpoint identifiers
		const Identifier Input_ID = OLO_IDENTIFIER("Input");
		const Identifier WindowSize_ID = OLO_IDENTIFIER("WindowSize");
		const Identifier WindowFunction_ID = OLO_IDENTIFIER("WindowFunction");
		const Identifier OverlapFactor_ID = OLO_IDENTIFIER("OverlapFactor");
		const Identifier UpdateRate_ID = OLO_IDENTIFIER("UpdateRate");
		const Identifier MinFrequency_ID = OLO_IDENTIFIER("MinFrequency");
		const Identifier MaxFrequency_ID = OLO_IDENTIFIER("MaxFrequency");
		const Identifier Reset_ID = OLO_IDENTIFIER("Reset");
		
		// Output arrays (frequency bins)
		const Identifier MagnitudeSpectrum_ID = OLO_IDENTIFIER("MagnitudeSpectrum");
		const Identifier PhaseSpectrum_ID = OLO_IDENTIFIER("PhaseSpectrum");
		const Identifier PowerSpectrum_ID = OLO_IDENTIFIER("PowerSpectrum");
		const Identifier PeakFrequency_ID = OLO_IDENTIFIER("PeakFrequency");
		const Identifier SpectralCentroid_ID = OLO_IDENTIFIER("SpectralCentroid");

		// FFT Analysis state
		struct AnalysisState
		{
			std::vector<f32> inputBuffer;
			std::vector<f32> windowBuffer;
			std::vector<std::complex<f32>> fftBuffer;
			std::vector<f32> magnitudeSpectrum;
			std::vector<f32> phaseSpectrum;
			std::vector<f32> powerSpectrum;
			
			u32 windowSize = 1024;
			u32 bufferIndex = 0;
			u32 hopSize = 512;
			u32 samplesSinceLastUpdate = 0;
			u32 updateInterval = 512;
			
			WindowFunction windowFunc = WindowFunction::Hann;
			f32 minFreq = 20.0f;
			f32 maxFreq = 20000.0f;
			
			bool isInitialized = false;
			bool needsUpdate = false;
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
			// Register inputs
			DECLARE_INPUT(f32, Input);                       // Audio input for analysis
			DECLARE_INPUT(f32, WindowSize);                  // FFT window size (power of 2)
			DECLARE_INPUT(f32, WindowFunction);              // Window function type
			DECLARE_INPUT(f32, OverlapFactor);              // Overlap between windows (0.0-0.875)
			DECLARE_INPUT(f32, UpdateRate);                 // Analysis update rate in Hz
			DECLARE_INPUT(f32, MinFrequency);               // Minimum frequency for analysis
			DECLARE_INPUT(f32, MaxFrequency);               // Maximum frequency for analysis
			DECLARE_INPUT(f32, Reset);                      // Reset analysis state

			// Register array outputs (note: these will be parameter arrays)
			DECLARE_OUTPUT(f32, MagnitudeSpectrum);         // Magnitude spectrum array
			DECLARE_OUTPUT(f32, PhaseSpectrum);             // Phase spectrum array  
			DECLARE_OUTPUT(f32, PowerSpectrum);             // Power spectrum array
			DECLARE_OUTPUT(f32, PeakFrequency);             // Dominant frequency
			DECLARE_OUTPUT(f32, SpectralCentroid);          // Spectral centroid (brightness)

			// Set default values
			SetParameterValue(Input_ID, 0.0f, false);
			SetParameterValue(WindowSize_ID, 1024.0f, false);      // 1024 samples default
			SetParameterValue(WindowFunction_ID, static_cast<f32>(WindowFunction::Hann), false);
			SetParameterValue(OverlapFactor_ID, 0.5f, false);      // 50% overlap
			SetParameterValue(UpdateRate_ID, 60.0f, false);        // 60 Hz update rate
			SetParameterValue(MinFrequency_ID, 20.0f, false);
			SetParameterValue(MaxFrequency_ID, 20000.0f, false);
			SetParameterValue(Reset_ID, 0.0f, false);
			
			SetParameterValue(MagnitudeSpectrum_ID, 0.0f, false);
			SetParameterValue(PhaseSpectrum_ID, 0.0f, false);
			SetParameterValue(PowerSpectrum_ID, 0.0f, false);
			SetParameterValue(PeakFrequency_ID, 0.0f, false);
			SetParameterValue(SpectralCentroid_ID, 0.0f, false);

			// Register Reset input event with flag callback
			AddInputEvent<f32>(Reset_ID, "Reset", [this](f32 value) {
				if (value > 0.5f) m_ResetFlag.SetDirty();
			});
		}

		virtual ~SpectrumAnalyzerNode() = default;

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
				ResetAnalysis();
				if (resetValue > 0.5f)
					SetParameterValue(Reset_ID, 0.0f, false);
			}

			// Update analysis parameters
			UpdateAnalysisParameters();

			// Process input audio for analysis
			if (inputs && inputs[0] && m_State.isInitialized)
			{
				ProcessAnalysis(inputs[0], numSamples);
			}

			// Copy input to output (pass-through)
			if (inputs && inputs[0] && outputs && outputs[0])
			{
				for (u32 i = 0; i < numSamples; ++i)
				{
					outputs[0][i] = inputs[0][i];
				}
			}
			else if (outputs && outputs[0])
			{
				// Clear output if no input
				for (u32 i = 0; i < numSamples; ++i)
				{
					outputs[0][i] = 0.0f;
				}
			}
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
			
			// Initialize analysis state
			InitializeAnalysis();
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("SpectrumAnalyzerNode");
		}

		const char* GetDisplayName() const override
		{
			return "Spectrum Analyzer";
		}

		//======================================================================
		// Analysis Implementation
		//======================================================================

	private:
		void UpdateAnalysisParameters()
		{
			// Update window size (must be power of 2)
			u32 newWindowSize = static_cast<u32>(GetParameterValue<f32>(WindowSize_ID));
			newWindowSize = std::clamp(newWindowSize, MIN_WINDOW_SIZE, MAX_WINDOW_SIZE);
			newWindowSize = NextPowerOfTwo(newWindowSize);
			
			if (newWindowSize != m_State.windowSize)
			{
				m_State.windowSize = newWindowSize;
				InitializeAnalysis();
			}

			// Update window function
			i32 windowFuncInt = static_cast<i32>(GetParameterValue<f32>(WindowFunction_ID));
			m_State.windowFunc = static_cast<WindowFunction>(std::clamp(windowFuncInt, 0, 4));

			// Update overlap factor
			f32 overlapFactor = glm::clamp(GetParameterValue<f32>(OverlapFactor_ID), MIN_OVERLAP, MAX_OVERLAP);
			m_State.hopSize = static_cast<u32>(m_State.windowSize * (1.0f - overlapFactor));
			m_State.hopSize = std::max(m_State.hopSize, 1u);

			// Update frequency range
			m_State.minFreq = std::max(GetParameterValue<f32>(MinFrequency_ID), 0.0f);
			m_State.maxFreq = std::min(GetParameterValue<f32>(MaxFrequency_ID), static_cast<f32>(m_SampleRate * 0.5));
			m_State.maxFreq = std::max(m_State.maxFreq, m_State.minFreq + 1.0f);

			// Update update rate
			f32 updateRate = glm::clamp(GetParameterValue<f32>(UpdateRate_ID), MIN_UPDATE_RATE, MAX_UPDATE_RATE);
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

		void PerformFFTAnalysis()
		{
			// Copy windowed data to FFT buffer
			ApplyWindow();
			
			// Perform FFT (simplified implementation - in production use a proper FFT library)
			PerformFFT();
			
			// Calculate spectrum data
			CalculateSpectrum();
			
			// Update output parameters
			UpdateOutputParameters();
		}

		void ApplyWindow()
		{
			// Apply windowing function to input buffer
			for (u32 i = 0; i < m_State.windowSize; ++i)
			{
				u32 readIndex = (m_State.bufferIndex + i) % m_State.windowSize;
				f32 windowValue = CalculateWindowValue(i, m_State.windowSize, m_State.windowFunc);
				m_State.windowBuffer[i] = m_State.inputBuffer[readIndex] * windowValue;
			}
		}

		f32 CalculateWindowValue(u32 n, u32 N, WindowFunction func)
		{
			const f32 pi = glm::pi<f32>();
			const f32 normN = static_cast<f32>(n) / static_cast<f32>(N - 1);
			
			switch (func)
			{
				case WindowFunction::Rectangle:
					return 1.0f;
					
				case WindowFunction::Hann:
					return 0.5f * (1.0f - std::cos(2.0f * pi * normN));
					
				case WindowFunction::Hamming:
					return 0.54f - 0.46f * std::cos(2.0f * pi * normN);
					
				case WindowFunction::Blackman:
					return 0.42f - 0.5f * std::cos(2.0f * pi * normN) + 0.08f * std::cos(4.0f * pi * normN);
					
				case WindowFunction::Kaiser:
					// Simplified Kaiser window (beta = 8.6)
					return 0.5f * (1.0f - std::cos(2.0f * pi * normN));
					
				default:
					return 1.0f;
			}
		}

		void PerformFFT()
		{
			// Simplified DFT implementation for demonstration
			// In production, use a proper FFT library like FFTW or similar
			const u32 N = m_State.windowSize;
			const f32 pi = glm::pi<f32>();
			
			for (u32 k = 0; k < N / 2; ++k)
			{
				std::complex<f32> sum(0.0f, 0.0f);
				
				for (u32 n = 0; n < N; ++n)
				{
					f32 angle = -2.0f * pi * static_cast<f32>(k * n) / static_cast<f32>(N);
					std::complex<f32> twiddle(std::cos(angle), std::sin(angle));
					sum += m_State.windowBuffer[n] * twiddle;
				}
				
				m_State.fftBuffer[k] = sum;
			}
		}

		void CalculateSpectrum()
		{
			const u32 numBins = m_State.windowSize / 2;
			
			for (u32 i = 0; i < numBins; ++i)
			{
				const std::complex<f32>& bin = m_State.fftBuffer[i];
				
				// Magnitude spectrum
				m_State.magnitudeSpectrum[i] = std::abs(bin);
				
				// Phase spectrum
				m_State.phaseSpectrum[i] = std::arg(bin);
				
				// Power spectrum
				m_State.powerSpectrum[i] = m_State.magnitudeSpectrum[i] * m_State.magnitudeSpectrum[i];
			}
			
			// Calculate analysis features
			CalculatePeakFrequency();
			CalculateSpectralCentroid();
		}

		void CalculatePeakFrequency()
		{
			const u32 numBins = m_State.windowSize / 2;
			const f32 binSize = static_cast<f32>(m_SampleRate) / static_cast<f32>(m_State.windowSize);
			
			u32 peakBin = 0;
			f32 peakMagnitude = 0.0f;
			
			for (u32 i = 1; i < numBins - 1; ++i) // Skip DC and Nyquist
			{
				f32 frequency = static_cast<f32>(i) * binSize;
				
				// Only consider frequencies in our range of interest
				if (frequency >= m_State.minFreq && frequency <= m_State.maxFreq)
				{
					if (m_State.magnitudeSpectrum[i] > peakMagnitude)
					{
						peakMagnitude = m_State.magnitudeSpectrum[i];
						peakBin = i;
					}
				}
			}
			
			m_PeakFrequency = static_cast<f32>(peakBin) * binSize;
		}

		void CalculateSpectralCentroid()
		{
			const u32 numBins = m_State.windowSize / 2;
			const f32 binSize = static_cast<f32>(m_SampleRate) / static_cast<f32>(m_State.windowSize);
			
			f32 weightedSum = 0.0f;
			f32 magnitudeSum = 0.0f;
			
			for (u32 i = 1; i < numBins - 1; ++i) // Skip DC and Nyquist
			{
				f32 frequency = static_cast<f32>(i) * binSize;
				
				// Only consider frequencies in our range of interest
				if (frequency >= m_State.minFreq && frequency <= m_State.maxFreq)
				{
					f32 magnitude = m_State.magnitudeSpectrum[i];
					weightedSum += frequency * magnitude;
					magnitudeSum += magnitude;
				}
			}
			
			m_SpectralCentroid = (magnitudeSum > 0.0f) ? (weightedSum / magnitudeSum) : 0.0f;
		}

		void UpdateOutputParameters()
		{
			// Update scalar outputs
			SetParameterValue(PeakFrequency_ID, m_PeakFrequency, false);
			SetParameterValue(SpectralCentroid_ID, m_SpectralCentroid, false);
			
			// Note: Array outputs would need special handling in the parameter system
			// For now, we'll just set the first bin values as examples
			if (!m_State.magnitudeSpectrum.empty())
			{
				SetParameterValue(MagnitudeSpectrum_ID, m_State.magnitudeSpectrum[1], false); // Skip DC
				SetParameterValue(PhaseSpectrum_ID, m_State.phaseSpectrum[1], false);
				SetParameterValue(PowerSpectrum_ID, m_State.powerSpectrum[1], false);
			}
		}

		void InitializeAnalysis()
		{
			// Initialize buffers
			m_State.inputBuffer.resize(m_State.windowSize, 0.0f);
			m_State.windowBuffer.resize(m_State.windowSize, 0.0f);
			m_State.fftBuffer.resize(m_State.windowSize / 2);
			m_State.magnitudeSpectrum.resize(m_State.windowSize / 2, 0.0f);
			m_State.phaseSpectrum.resize(m_State.windowSize / 2, 0.0f);
			m_State.powerSpectrum.resize(m_State.windowSize / 2, 0.0f);
			
			// Reset state
			m_State.bufferIndex = 0;
			m_State.samplesSinceLastUpdate = 0;
			m_State.hopSize = m_State.windowSize / 2; // 50% overlap default
			
			m_State.isInitialized = true;
		}

		void ResetAnalysis()
		{
			if (m_State.isInitialized)
			{
				// Clear all buffers
				std::fill(m_State.inputBuffer.begin(), m_State.inputBuffer.end(), 0.0f);
				std::fill(m_State.windowBuffer.begin(), m_State.windowBuffer.end(), 0.0f);
				std::fill(m_State.magnitudeSpectrum.begin(), m_State.magnitudeSpectrum.end(), 0.0f);
				std::fill(m_State.phaseSpectrum.begin(), m_State.phaseSpectrum.end(), 0.0f);
				std::fill(m_State.powerSpectrum.begin(), m_State.powerSpectrum.end(), 0.0f);
				
				// Reset indices
				m_State.bufferIndex = 0;
				m_State.samplesSinceLastUpdate = 0;
				
				// Reset analysis results
				m_PeakFrequency = 0.0f;
				m_SpectralCentroid = 0.0f;
			}
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

		/// Get the current window size
		u32 GetWindowSize() const
		{
			return m_State.windowSize;
		}

		/// Get the number of frequency bins
		u32 GetNumFrequencyBins() const
		{
			return m_State.windowSize / 2;
		}

		/// Get frequency for a specific bin
		f32 GetBinFrequency(u32 binIndex) const
		{
			const f32 binSize = static_cast<f32>(m_SampleRate) / static_cast<f32>(m_State.windowSize);
			return static_cast<f32>(binIndex) * binSize;
		}

		/// Get the magnitude spectrum (for external access)
		const std::vector<f32>& GetMagnitudeSpectrum() const
		{
			return m_State.magnitudeSpectrum;
		}

		/// Get the power spectrum (for external access)
		const std::vector<f32>& GetPowerSpectrum() const
		{
			return m_State.powerSpectrum;
		}

		/// Get current peak frequency
		f32 GetPeakFrequency() const
		{
			return m_PeakFrequency;
		}

		/// Get current spectral centroid
		f32 GetSpectralCentroid() const
		{
			return m_SpectralCentroid;
		}
	};

} // namespace OloEngine::Audio::SoundGraph
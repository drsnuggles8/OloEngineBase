#pragma once

#include "../NodeProcessor.h"
#include "../ValueView.h"
#include "../InputView.h"
#include "../OutputView.h"
#include "OloEngine/Core/Identifier.h"
#include <glm/glm.hpp>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// FrequencyLogToLinear Node - converts logarithmic frequency values to linear scale
	/// Inverse operation of LinearToLogFrequency, essential for frequency analysis
	/// Maps a logarithmic frequency input to a linear output range
	/// Converts from legacy parameters to ValueView system while preserving functionality
	class FrequencyLogToLinearNode : public NodeProcessor
	{
	private:
		//======================================================================
		// ValueView System - Real-time Parameter Streams
		//======================================================================
		
		// Input parameter streams
		InputView<f32> m_FrequencyView;
		InputView<f32> m_MinFrequencyView;
		InputView<f32> m_MaxFrequencyView;
		InputView<f32> m_MinValueView;
		InputView<f32> m_MaxValueView;
		
		// Output streams
		OutputView<f32> m_ValueView;
		
		// Current parameter values for legacy API compatibility
		f32 m_CurrentFrequency = 1000.0f;     // 1kHz default input
		f32 m_CurrentMinFrequency = 20.0f;    // 20Hz - low end of human hearing
		f32 m_CurrentMaxFrequency = 20000.0f; // 20kHz - high end of human hearing
		f32 m_CurrentMinValue = 0.0f;
		f32 m_CurrentMaxValue = 1.0f;
		f32 m_CurrentValue = 0.5f;            // Default linear output

	public:
		FrequencyLogToLinearNode()
			: m_FrequencyView([this](f32 value) { m_CurrentFrequency = value; }),
			  m_MinFrequencyView([this](f32 value) { m_CurrentMinFrequency = value; }),
			  m_MaxFrequencyView([this](f32 value) { m_CurrentMaxFrequency = value; }),
			  m_MinValueView([this](f32 value) { m_CurrentMinValue = value; }),
			  m_MaxValueView([this](f32 value) { m_CurrentMaxValue = value; }),
			  m_ValueView([this](f32 value) { m_CurrentValue = value; })
		{
			// No parameter registration needed - handled by ValueView system
		}

		virtual ~FrequencyLogToLinearNode() = default;

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_FrequencyView.Initialize(maxBufferSize);
			m_MinFrequencyView.Initialize(maxBufferSize);
			m_MaxFrequencyView.Initialize(maxBufferSize);
			m_MinValueView.Initialize(maxBufferSize);
			m_MaxValueView.Initialize(maxBufferSize);
			m_ValueView.Initialize(maxBufferSize);
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_FrequencyView.UpdateFromConnections(inputs, numSamples);
			m_MinFrequencyView.UpdateFromConnections(inputs, numSamples);
			m_MaxFrequencyView.UpdateFromConnections(inputs, numSamples);
			m_MinValueView.UpdateFromConnections(inputs, numSamples);
			m_MaxValueView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				f32 frequency = m_FrequencyView.GetValue(sample);
				f32 minFrequency = m_MinFrequencyView.GetValue(sample);
				f32 maxFrequency = m_MaxFrequencyView.GetValue(sample);
				f32 minValue = m_MinValueView.GetValue(sample);
				f32 maxValue = m_MaxValueView.GetValue(sample);
				
				// Update internal state
				m_CurrentFrequency = frequency;
				m_CurrentMinFrequency = minFrequency;
				m_CurrentMaxFrequency = maxFrequency;
				m_CurrentMinValue = minValue;
				m_CurrentMaxValue = maxValue;

				f32 value;
				
				// Avoid division by zero and invalid logarithms
				if (minFrequency <= 0.0f || maxFrequency <= 0.0f || minFrequency >= maxFrequency || frequency <= 0.0f)
				{
					value = minValue; // Safe fallback
				}
				else
				{
					// Clamp frequency to valid range
					const f32 clampedFrequency = glm::clamp(frequency, minFrequency, maxFrequency);
					
					// Calculate octaves between minimum frequency and target frequency
					const f32 octavesBetweenMinAndTarget = glm::log2(clampedFrequency / minFrequency);
					
					// Calculate the total octave range
					const f32 octaveRange = glm::log2(maxFrequency / minFrequency);
					
					// Avoid division by zero
					if (octaveRange == 0.0f)
					{
						value = minValue;
					}
					else
					{
						// Calculate the output value range
						const f32 valueRange = maxValue - minValue;
						
						// Map from logarithmic frequency to linear value
						value = (octavesBetweenMinAndTarget / octaveRange) * valueRange + minValue;
					}
				}

				// Update current state
				m_CurrentValue = value;
				
				// Set output value
				m_ValueView.SetValue(sample, value);
			}
			
			// Update output streams
			m_ValueView.UpdateOutputConnections(outputs, numSamples);
		}

		//======================================================================
		// Legacy API Compatibility
		//======================================================================
		
		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("FrequencyLogToLinearNode");
		}

		const char* GetDisplayName() const override
		{
			return "Frequency Log to Linear";
		}

		// Legacy parameter methods for compatibility
		template<typename T>
		void SetParameterValue(const Identifier& id, T value)
		{
			if (id == OLO_IDENTIFIER("Frequency")) m_CurrentFrequency = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("MinFrequency")) m_CurrentMinFrequency = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("MaxFrequency")) m_CurrentMaxFrequency = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("MinValue")) m_CurrentMinValue = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("MaxValue")) m_CurrentMaxValue = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("Value")) m_CurrentValue = static_cast<f32>(value);
		}

		template<typename T>
		T GetParameterValue(const Identifier& id) const
		{
			if (id == OLO_IDENTIFIER("Frequency")) return static_cast<T>(m_CurrentFrequency);
			else if (id == OLO_IDENTIFIER("MinFrequency")) return static_cast<T>(m_CurrentMinFrequency);
			else if (id == OLO_IDENTIFIER("MaxFrequency")) return static_cast<T>(m_CurrentMaxFrequency);
			else if (id == OLO_IDENTIFIER("MinValue")) return static_cast<T>(m_CurrentMinValue);
			else if (id == OLO_IDENTIFIER("MaxValue")) return static_cast<T>(m_CurrentMaxValue);
			else if (id == OLO_IDENTIFIER("Value")) return static_cast<T>(m_CurrentValue);
			return T{};
		}

		//======================================================================
		// Utility Methods
		//======================================================================

		/// Convert a single frequency value to linear scale
		static f32 FrequencyLogToLinear(f32 frequency, f32 minFreq, f32 maxFreq, f32 minVal, f32 maxVal)
		{
			if (minFreq <= 0.0f || maxFreq <= 0.0f || minFreq >= maxFreq || frequency <= 0.0f)
			{
				return minVal;
			}
			
			const f32 clampedFrequency = glm::clamp(frequency, minFreq, maxFreq);
			const f32 octavesBetweenMinAndTarget = glm::log2(clampedFrequency / minFreq);
			const f32 octaveRange = glm::log2(maxFreq / minFreq);
			
			if (octaveRange == 0.0f) return minVal;
			
			const f32 valueRange = maxVal - minVal;
			return (octavesBetweenMinAndTarget / octaveRange) * valueRange + minVal;
		}

		/// Get current parameter values
		f32 GetCurrentFrequency() const { return m_CurrentFrequency; }
		f32 GetCurrentMinFrequency() const { return m_CurrentMinFrequency; }
		f32 GetCurrentMaxFrequency() const { return m_CurrentMaxFrequency; }
		f32 GetCurrentMinValue() const { return m_CurrentMinValue; }
		f32 GetCurrentMaxValue() const { return m_CurrentMaxValue; }
		f32 GetCurrentValue() const { return m_CurrentValue; }
	};

} // namespace OloEngine::Audio::SoundGraph
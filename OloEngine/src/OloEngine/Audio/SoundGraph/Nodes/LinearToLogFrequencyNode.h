#pragma once

#include "../NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/ValueView.h"
#include "OloEngine/Audio/SoundGraph/InputView.h"
#include "OloEngine/Audio/SoundGraph/OutputView.h"
#include "OloEngine/Core/Identifier.h"
#include <glm/glm.hpp>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// LinearToLogFrequency Node - converts linear values to logarithmic frequency scale
	/// Essential for audio applications where frequency perception is logarithmic
	/// Maps a linear input range to a logarithmic frequency range (e.g., for frequency controls)
	/// Converts from legacy parameters to ValueView system while preserving functionality
	class LinearToLogFrequencyNode : public NodeProcessor
	{
	private:
		//======================================================================
		// ValueView System - Real-time Parameter Streams
		//======================================================================
		
		InputView<f32> m_ValueView;
		InputView<f32> m_MinValueView;
		InputView<f32> m_MaxValueView;
		InputView<f32> m_MinFrequencyView;
		InputView<f32> m_MaxFrequencyView;
		OutputView<f32> m_FrequencyView;
		
		// Current parameter values for legacy API compatibility
		f32 m_CurrentValue = 0.5f;
		f32 m_CurrentMinValue = 0.0f;
		f32 m_CurrentMaxValue = 1.0f;
		f32 m_CurrentMinFrequency = 20.0f;   // 20Hz - low end of human hearing
		f32 m_CurrentMaxFrequency = 20000.0f; // 20kHz - high end of human hearing
		f32 m_CurrentFrequency = 1000.0f;    // 1kHz default output

	public:
		LinearToLogFrequencyNode()
			: m_ValueView([this](f32 value) { m_CurrentValue = value; }),
			  m_MinValueView([this](f32 value) { m_CurrentMinValue = value; }),
			  m_MaxValueView([this](f32 value) { m_CurrentMaxValue = value; }),
			  m_MinFrequencyView([this](f32 value) { m_CurrentMinFrequency = value; }),
			  m_MaxFrequencyView([this](f32 value) { m_CurrentMaxFrequency = value; }),
			  m_FrequencyView([this](f32 value) { m_CurrentFrequency = value; })
		{
			// No parameter registration needed - handled by ValueView system
		}

		virtual ~LinearToLogFrequencyNode() = default;

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_ValueView.Initialize(maxBufferSize);
			m_MinValueView.Initialize(maxBufferSize);
			m_MaxValueView.Initialize(maxBufferSize);
			m_MinFrequencyView.Initialize(maxBufferSize);
			m_MaxFrequencyView.Initialize(maxBufferSize);
			m_FrequencyView.Initialize(maxBufferSize);
		}

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_ValueView.UpdateFromConnections(inputs, numSamples);
			m_MinValueView.UpdateFromConnections(inputs, numSamples);
			m_MaxValueView.UpdateFromConnections(inputs, numSamples);
			m_MinFrequencyView.UpdateFromConnections(inputs, numSamples);
			m_MaxFrequencyView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				f32 value = m_ValueView.GetValue(sample);
				f32 minValue = m_MinValueView.GetValue(sample);
				f32 maxValue = m_MaxValueView.GetValue(sample);
				f32 minFrequency = m_MinFrequencyView.GetValue(sample);
				f32 maxFrequency = m_MaxFrequencyView.GetValue(sample);
				
				// Update internal state
				m_CurrentValue = value;
				m_CurrentMinValue = minValue;
				m_CurrentMaxValue = maxValue;
				m_CurrentMinFrequency = minFrequency;
				m_CurrentMaxFrequency = maxFrequency;

				f32 frequency;
				
				// Avoid division by zero and invalid logarithms
				if (maxValue == minValue || minFrequency <= 0.0f || maxFrequency <= 0.0f || minFrequency >= maxFrequency)
				{
					frequency = minFrequency; // Safe fallback
				}
				else
				{
					// Normalize the input value to 0-1 range
					const f32 normalizedValue = glm::clamp((value - minValue) / (maxValue - minValue), 0.0f, 1.0f);

					// Apply logarithmic scaling: log(min) + t * (log(max) - log(min))
					// where t is the normalized value
					const f32 logMin = glm::log(minFrequency);
					const f32 logMax = glm::log(maxFrequency);
					const f32 logFreq = logMin + normalizedValue * (logMax - logMin);

					// Convert back to linear frequency
					frequency = glm::exp(logFreq);
				}
				
				// Update output
				m_CurrentFrequency = frequency;
				m_FrequencyView.SetValue(sample, frequency);
			}
			
			// Update output streams
			m_FrequencyView.UpdateOutputConnections(outputs, numSamples);
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("LinearToLogFrequencyNode");
		}

		const char* GetDisplayName() const override
		{
			return "Linear To Log Frequency";
		}
		
		//======================================================================
		// Legacy API Compatibility Methods
		//======================================================================
		
		template<typename T>
		void SetParameterValue(const Identifier& id, T value)
		{
			if (id == OLO_IDENTIFIER("Value")) m_CurrentValue = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("MinValue")) m_CurrentMinValue = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("MaxValue")) m_CurrentMaxValue = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("MinFrequency")) m_CurrentMinFrequency = static_cast<f32>(value);
			else if (id == OLO_IDENTIFIER("MaxFrequency")) m_CurrentMaxFrequency = static_cast<f32>(value);
		}

		template<typename T>
		T GetParameterValue(const Identifier& id) const
		{
			if (id == OLO_IDENTIFIER("Value")) return static_cast<T>(m_CurrentValue);
			else if (id == OLO_IDENTIFIER("MinValue")) return static_cast<T>(m_CurrentMinValue);
			else if (id == OLO_IDENTIFIER("MaxValue")) return static_cast<T>(m_CurrentMaxValue);
			else if (id == OLO_IDENTIFIER("MinFrequency")) return static_cast<T>(m_CurrentMinFrequency);
			else if (id == OLO_IDENTIFIER("MaxFrequency")) return static_cast<T>(m_CurrentMaxFrequency);
			else if (id == OLO_IDENTIFIER("Frequency")) return static_cast<T>(m_CurrentFrequency);
			return T{};
		}

		//======================================================================
		// Utility Methods
		//======================================================================

		/// Calculate logarithmic frequency mapping with given parameters
		static f32 LinearToLogMapping(f32 value, f32 minValue, f32 maxValue, f32 minFreq, f32 maxFreq)
		{
			if (maxValue == minValue || minFreq <= 0.0f || maxFreq <= 0.0f || minFreq >= maxFreq)
				return minFreq;

			f32 normalizedValue = glm::clamp((value - minValue) / (maxValue - minValue), 0.0f, 1.0f);
			f32 logMin = glm::log(minFreq);
			f32 logMax = glm::log(maxFreq);
			f32 logFreq = logMin + normalizedValue * (logMax - logMin);
			return glm::exp(logFreq);
		}

		/// Get the current normalized value (0-1 range)
		f32 GetNormalizedValue() const
		{
			if (m_CurrentMaxValue == m_CurrentMinValue) return 0.0f;
			return glm::clamp((m_CurrentValue - m_CurrentMinValue) / (m_CurrentMaxValue - m_CurrentMinValue), 0.0f, 1.0f);
		}

		/// Set value from normalized input (0-1)
		void SetNormalizedValue(f32 normalizedValue)
		{
			normalizedValue = glm::clamp(normalizedValue, 0.0f, 1.0f);
			m_CurrentValue = m_CurrentMinValue + normalizedValue * (m_CurrentMaxValue - m_CurrentMinValue);
		}

		/// Get the frequency range ratio (max/min)
		f32 GetFrequencyRatio() const
		{
			if (m_CurrentMinFrequency <= 0.0f) return 1.0f;
			return m_CurrentMaxFrequency / m_CurrentMinFrequency;
		}
	};

} // namespace OloEngine::Audio::SoundGraph
#pragma once

#include "../NodeProcessor.h"
#include "OloEngine/Core/Identifier.h"
#include <glm/glm.hpp>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// MapRange Node - maps a value from one range to another range
	/// Optionally clamps the input to the input range before mapping
	/// Very useful for audio parameter mapping and signal conditioning
	template<typename T>
	class MapRangeNode : public NodeProcessor
	{
		static_assert(std::is_arithmetic_v<T>, "MapRangeNode can only be of arithmetic type");

	private:
		//======================================================================
		// ValueView Streams for Real-Time Processing
		//======================================================================
		
		ValueView<T> m_InputView;
		ValueView<T> m_InRangeMinView;
		ValueView<T> m_InRangeMaxView;
		ValueView<T> m_OutRangeMinView;
		ValueView<T> m_OutRangeMaxView;
		ValueView<bool> m_ClampedView;
		ValueView<T> m_ResultView;

		//======================================================================
		// Current Parameter Values (from streams)
		//======================================================================
		
		T m_CurrentInput = T(0);
		T m_CurrentInRangeMin = T(0);
		T m_CurrentInRangeMax = T(1);
		T m_CurrentOutRangeMin = T(0);
		T m_CurrentOutRangeMax = T(1);
		bool m_CurrentClamped = false;

	public:
		//======================================================================
		// Constructor & Destructor
		//======================================================================
		
		explicit MapRangeNode(NodeDatabase& database, NodeID nodeID)
			: NodeProcessor(database, nodeID)
			, m_InputView("Input", T(0))
			, m_InRangeMinView("InRangeMin", T(0))
			, m_InRangeMaxView("InRangeMax", T(1))
			, m_OutRangeMinView("OutRangeMin", T(0))
			, m_OutRangeMaxView("OutRangeMax", T(1))
			, m_ClampedView("Clamped", false)
			, m_ResultView("Output", T(0))
		{
			// Create Input/Output events
			RegisterInputEvent<T>("Input", [this](const T& value) { m_CurrentInput = value; });
			RegisterInputEvent<T>("InRangeMin", [this](const T& value) { m_CurrentInRangeMin = value; });
			RegisterInputEvent<T>("InRangeMax", [this](const T& value) { m_CurrentInRangeMax = value; });
			RegisterInputEvent<T>("OutRangeMin", [this](const T& value) { m_CurrentOutRangeMin = value; });
			RegisterInputEvent<T>("OutRangeMax", [this](const T& value) { m_CurrentOutRangeMax = value; });
			RegisterInputEvent<bool>("Clamped", [this](const bool& value) { m_CurrentClamped = value; });
			
			RegisterOutputEvent<T>("Output");
		}

		virtual ~MapRangeNode() = default;

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_InputView.UpdateFromConnections(inputs, numSamples);
			m_InRangeMinView.UpdateFromConnections(inputs, numSamples);
			m_InRangeMaxView.UpdateFromConnections(inputs, numSamples);
			m_OutRangeMinView.UpdateFromConnections(inputs, numSamples);
			m_OutRangeMaxView.UpdateFromConnections(inputs, numSamples);
			m_ClampedView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				T inputValue = m_InputView.GetValue(sample);
				T inRangeMin = m_InRangeMinView.GetValue(sample);
				T inRangeMax = m_InRangeMaxView.GetValue(sample);
				T outRangeMin = m_OutRangeMinView.GetValue(sample);
				T outRangeMax = m_OutRangeMaxView.GetValue(sample);
				bool clamped = m_ClampedView.GetValue(sample);
				
				// Update internal state if changed
				if (inputValue != m_CurrentInput) m_CurrentInput = inputValue;
				if (inRangeMin != m_CurrentInRangeMin) m_CurrentInRangeMin = inRangeMin;
				if (inRangeMax != m_CurrentInRangeMax) m_CurrentInRangeMax = inRangeMax;
				if (outRangeMin != m_CurrentOutRangeMin) m_CurrentOutRangeMin = outRangeMin;
				if (outRangeMax != m_CurrentOutRangeMax) m_CurrentOutRangeMax = outRangeMax;
				if (clamped != m_CurrentClamped) m_CurrentClamped = clamped;

				T result;
				if constexpr (std::is_same_v<T, f32>)
				{
					// Optionally clamp input to input range
					const f32 value = clamped ? glm::clamp(inputValue, inRangeMin, inRangeMax) : inputValue;
					
					// Avoid division by zero
					const f32 inputRange = inRangeMax - inRangeMin;
					if (glm::abs(inputRange) < 1e-6f)
					{
						result = outRangeMin;
					}
					else
					{
						// Map from input range to output range
						const f32 normalizedValue = (value - inRangeMin) / inputRange;
						result = outRangeMin + normalizedValue * (outRangeMax - outRangeMin);
					}
				}
				else if constexpr (std::is_same_v<T, i32>)
				{
					// For integers, use floating point arithmetic then convert back
					const f32 value = clamped ? glm::clamp(static_cast<f32>(inputValue), static_cast<f32>(inRangeMin), static_cast<f32>(inRangeMax)) : static_cast<f32>(inputValue);
					
					const f32 inputRange = static_cast<f32>(inRangeMax - inRangeMin);
					if (glm::abs(inputRange) < 1e-6f)
					{
						result = outRangeMin;
					}
					else
					{
						const f32 normalizedValue = (value - static_cast<f32>(inRangeMin)) / inputRange;
						result = static_cast<i32>(static_cast<f32>(outRangeMin) + normalizedValue * static_cast<f32>(outRangeMax - outRangeMin));
					}
				}
				else
				{
					// Generic fallback using floating point conversion
					const f32 value = clamped ? glm::clamp(static_cast<f32>(inputValue), static_cast<f32>(inRangeMin), static_cast<f32>(inRangeMax)) : static_cast<f32>(inputValue);
					
					const f32 inputRange = static_cast<f32>(inRangeMax - inRangeMin);
					if (glm::abs(inputRange) < 1e-6f)
					{
						result = outRangeMin;
					}
					else
					{
						const f32 normalizedValue = (value - static_cast<f32>(inRangeMin)) / inputRange;
						result = static_cast<T>(static_cast<f32>(outRangeMin) + normalizedValue * static_cast<f32>(outRangeMax - outRangeMin));
					}
				}

				// Set output value
				m_ResultView.SetValue(sample, result);
			}
			
			// Update output streams
			m_ResultView.UpdateOutputConnections(outputs, numSamples);
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_InputView.Initialize(maxBufferSize);
			m_InRangeMinView.Initialize(maxBufferSize);
			m_InRangeMaxView.Initialize(maxBufferSize);
			m_OutRangeMinView.Initialize(maxBufferSize);
			m_OutRangeMaxView.Initialize(maxBufferSize);
			m_ClampedView.Initialize(maxBufferSize);
			m_ResultView.Initialize(maxBufferSize);
		}

		//======================================================================
		// Type Information
		//======================================================================
		
		Identifier GetTypeID() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return OLO_IDENTIFIER("MapRangeNodeF32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("MapRangeNodeI32");
			else
				return OLO_IDENTIFIER("MapRangeNode");
		}

		const char* GetDisplayName() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return "Map Range (f32)";
			else if constexpr (std::is_same_v<T, i32>)
				return "Map Range (i32)";
			else
				return "Map Range";
		}

		//======================================================================
		// Legacy API Compatibility Methods
		//======================================================================
		
		T GetInput() const { return m_CurrentInput; }
		void SetInput(const T& value) { m_CurrentInput = value; }
		
		T GetInRangeMin() const { return m_CurrentInRangeMin; }
		void SetInRangeMin(const T& value) { m_CurrentInRangeMin = value; }
		
		T GetInRangeMax() const { return m_CurrentInRangeMax; }
		void SetInRangeMax(const T& value) { m_CurrentInRangeMax = value; }
		
		T GetOutRangeMin() const { return m_CurrentOutRangeMin; }
		void SetOutRangeMin(const T& value) { m_CurrentOutRangeMin = value; }
		
		T GetOutRangeMax() const { return m_CurrentOutRangeMax; }
		void SetOutRangeMax(const T& value) { m_CurrentOutRangeMax = value; }
		
		bool GetClamped() const { return m_CurrentClamped; }
		void SetClamped(const bool& value) { m_CurrentClamped = value; }
		
		T GetOutput() const 
		{
			// Calculate mapping result
			const T value = m_CurrentClamped ? 
				(std::is_same_v<T, f32> ? glm::clamp(m_CurrentInput, m_CurrentInRangeMin, m_CurrentInRangeMax) : 
				 std::clamp(m_CurrentInput, m_CurrentInRangeMin, m_CurrentInRangeMax)) : 
				m_CurrentInput;
			
			if constexpr (std::is_same_v<T, f32>)
			{
				const f32 inputRange = m_CurrentInRangeMax - m_CurrentInRangeMin;
				if (glm::abs(inputRange) < 1e-6f)
					return m_CurrentOutRangeMin;
				else
					return m_CurrentOutRangeMin + ((value - m_CurrentInRangeMin) / inputRange) * (m_CurrentOutRangeMax - m_CurrentOutRangeMin);
			}
			else
			{
				const f32 inputRange = static_cast<f32>(m_CurrentInRangeMax - m_CurrentInRangeMin);
				if (glm::abs(inputRange) < 1e-6f)
					return m_CurrentOutRangeMin;
				else
					return static_cast<T>(static_cast<f32>(m_CurrentOutRangeMin) + ((static_cast<f32>(value) - static_cast<f32>(m_CurrentInRangeMin)) / inputRange) * static_cast<f32>(m_CurrentOutRangeMax - m_CurrentOutRangeMin));
			}
		}
	};

	// Type aliases for common usage
	using MapRangeNodeF32 = MapRangeNode<f32>;
	using MapRangeNodeI32 = MapRangeNode<i32>;

} // namespace OloEngine::Audio::SoundGraph
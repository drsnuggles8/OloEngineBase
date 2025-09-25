#pragma once

#include "../NodeProcessor.h"
#include "OloEngine/Core/Identifier.h"
#include <glm/glm.hpp>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Power Node - raises base to the power of exponent (base^exponent)
	/// Supports both f32 and i32 variants using template specialization
	template<typename T>
	class PowerNode : public NodeProcessor
	{
		static_assert(std::is_arithmetic_v<T>, "PowerNode can only be of arithmetic type");

	private:
		//======================================================================
		// ValueView Streams for Real-Time Processing
		//======================================================================
		
		ValueView<T> m_BaseView;
		ValueView<T> m_ExponentView;
		ValueView<T> m_ResultView;

		//======================================================================
		// Current Parameter Values (from streams)
		//======================================================================
		
		T m_CurrentBase = T(2);
		T m_CurrentExponent = T(2);

	public:
		//======================================================================
		// Constructor & Destructor
		//======================================================================
		
		explicit PowerNode(NodeDatabase& database, NodeID nodeID)
			: NodeProcessor(database, nodeID)
			, m_BaseView("Base", T(2))
			, m_ExponentView("Exponent", T(2))
			, m_ResultView("Result", T(0))
		{
			// Create Input/Output events
			RegisterInputEvent<T>("Base", [this](const T& value) { m_CurrentBase = value; });
			RegisterInputEvent<T>("Exponent", [this](const T& value) { m_CurrentExponent = value; });
			
			RegisterOutputEvent<T>("Result");
		}

		virtual ~PowerNode() = default;

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_BaseView.UpdateFromConnections(inputs, numSamples);
			m_ExponentView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				T base = m_BaseView.GetValue(sample);
				T exponent = m_ExponentView.GetValue(sample);
				
				// Update internal state if changed
				if (base != m_CurrentBase) m_CurrentBase = base;
				if (exponent != m_CurrentExponent) m_CurrentExponent = exponent;
				
				// Calculate result
				T result;
				if constexpr (std::is_same_v<T, f32>)
				{
					result = glm::pow(base, exponent);
				}
				else if constexpr (std::is_same_v<T, i32>)
				{
					// For integer power, use integer arithmetic for better precision
					result = static_cast<i32>(glm::pow(static_cast<f32>(base), static_cast<f32>(exponent)));
				}
				else
				{
					result = static_cast<T>(glm::pow(static_cast<f32>(base), static_cast<f32>(exponent)));
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
			m_BaseView.Initialize(maxBufferSize);
			m_ExponentView.Initialize(maxBufferSize);
			m_ResultView.Initialize(maxBufferSize);
		}

		//======================================================================
		// Type Information
		//======================================================================
		
		Identifier GetTypeID() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return OLO_IDENTIFIER("PowerNodeF32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("PowerNodeI32");
			else
				return OLO_IDENTIFIER("PowerNode");
		}

		const char* GetDisplayName() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return "Power (f32)";
			else if constexpr (std::is_same_v<T, i32>)
				return "Power (i32)";
			else
				return "Power";
		}
		
		//======================================================================
		// Legacy API Compatibility Methods
		//======================================================================
		
		T GetBase() const { return m_CurrentBase; }
		void SetBase(const T& value) { m_CurrentBase = value; }
		
		T GetExponent() const { return m_CurrentExponent; }
		void SetExponent(const T& value) { m_CurrentExponent = value; }
		
		T GetResult() const 
		{ 
			if constexpr (std::is_same_v<T, f32>)
				return glm::pow(m_CurrentBase, m_CurrentExponent);
			else if constexpr (std::is_same_v<T, i32>)
				return static_cast<i32>(glm::pow(static_cast<f32>(m_CurrentBase), static_cast<f32>(m_CurrentExponent)));
			else
				return static_cast<T>(glm::pow(static_cast<f32>(m_CurrentBase), static_cast<f32>(m_CurrentExponent)));
		}
	};

	// Type aliases for common usage
	using PowerNodeF32 = PowerNode<f32>;
	using PowerNodeI32 = PowerNode<i32>;

} // namespace OloEngine::Audio::SoundGraph
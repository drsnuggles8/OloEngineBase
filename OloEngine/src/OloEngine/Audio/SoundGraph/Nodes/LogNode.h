#pragma once

#include "../NodeProcessor.h"
#include "OloEngine/Core/Identifier.h"
#include <glm/glm.hpp>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtx/log_base.hpp>
#undef GLM_ENABLE_EXPERIMENTAL

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Log Node - calculates logarithm of value with specified base (log_base(value))
	/// Supports both f32 and i32 variants using template specialization
	template<typename T>
	class LogNode : public NodeProcessor
	{
		static_assert(std::is_arithmetic_v<T>, "LogNode can only be of arithmetic type");

	private:
		//======================================================================
		// ValueView Streams for Real-Time Processing
		//======================================================================
		
		ValueView<T> m_BaseView;
		ValueView<T> m_ValueView;
		ValueView<T> m_ResultView;

		//======================================================================
		// Current Parameter Values (from streams)
		//======================================================================
		
		T m_CurrentBase = T(10);
		T m_CurrentValue = T(1);

	public:
		//======================================================================
		// Constructor & Destructor
		//======================================================================
		
		explicit LogNode(NodeDatabase& database, NodeID nodeID)
			: NodeProcessor(database, nodeID)
			, m_BaseView("Base", T(10))  // Default to base 10
			, m_ValueView("Value", T(1))
			, m_ResultView("Result", T(0))
		{
			// Create Input/Output events
			RegisterInputEvent<T>("Base", [this](const T& value) { m_CurrentBase = value; });
			RegisterInputEvent<T>("Value", [this](const T& value) { m_CurrentValue = value; });
			
			RegisterOutputEvent<T>("Result");
		}

		virtual ~LogNode() = default;

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_BaseView.UpdateFromConnections(inputs, numSamples);
			m_ValueView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				T base = m_BaseView.GetValue(sample);
				T value = m_ValueView.GetValue(sample);
				
				// Update internal state if changed
				if (base != m_CurrentBase) m_CurrentBase = base;
				if (value != m_CurrentValue) m_CurrentValue = value;
				
				// Calculate logarithm result with safety checks
				T result;
				if constexpr (std::is_same_v<T, f32>)
				{
					// Prevent logarithm of zero or negative numbers
					if (value <= 0.0f || base <= 0.0f || base == 1.0f)
					{
						result = 0.0f; // Safe fallback value
					}
					else
					{
						result = glm::log(value, base);
					}
				}
				else if constexpr (std::is_same_v<T, i32>)
				{
					// For integers, convert to float, compute, then convert back
					if (value <= 0 || base <= 0 || base == 1)
					{
						result = 0; // Safe fallback value
					}
					else
					{
						result = static_cast<i32>(glm::log(static_cast<f32>(value), static_cast<f32>(base)));
					}
				}
				else
				{
					// Generic fallback
					if (value <= T(0) || base <= T(0) || base == T(1))
					{
						result = T(0);
					}
					else
					{
						result = static_cast<T>(glm::log(static_cast<f32>(value), static_cast<f32>(base)));
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
			m_BaseView.Initialize(maxBufferSize);
			m_ValueView.Initialize(maxBufferSize);
			m_ResultView.Initialize(maxBufferSize);
		}

		//======================================================================
		// Type Information
		//======================================================================
		
		Identifier GetTypeID() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return OLO_IDENTIFIER("LogNodeF32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("LogNodeI32");
			else
				return OLO_IDENTIFIER("LogNode");
		}

		const char* GetDisplayName() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return "Log (f32)";
			else if constexpr (std::is_same_v<T, i32>)
				return "Log (i32)";
			else
				return "Log";
		}
		
		//======================================================================
		// Legacy API Compatibility Methods
		//======================================================================
		
		T GetBase() const { return m_CurrentBase; }
		void SetBase(const T& value) { m_CurrentBase = value; }
		
		T GetValue() const { return m_CurrentValue; }
		void SetValue(const T& value) { m_CurrentValue = value; }
		
		T GetResult() const 
		{ 
			// Calculate result with safety checks
			if constexpr (std::is_same_v<T, f32>)
			{
				if (m_CurrentValue <= 0.0f || m_CurrentBase <= 0.0f || m_CurrentBase == 1.0f)
					return 0.0f;
				else
					return glm::log(m_CurrentValue, m_CurrentBase);
			}
			else if constexpr (std::is_same_v<T, i32>)
			{
				if (m_CurrentValue <= 0 || m_CurrentBase <= 0 || m_CurrentBase == 1)
					return 0;
				else
					return static_cast<i32>(glm::log(static_cast<f32>(m_CurrentValue), static_cast<f32>(m_CurrentBase)));
			}
			else
			{
				if (m_CurrentValue <= T(0) || m_CurrentBase <= T(0) || m_CurrentBase == T(1))
					return T(0);
				else
					return static_cast<T>(glm::log(static_cast<f32>(m_CurrentValue), static_cast<f32>(m_CurrentBase)));
			}
		}
	};

	// Type aliases for common usage
	using LogNodeF32 = LogNode<f32>;
	using LogNodeI32 = LogNode<i32>;

} // namespace OloEngine::Audio::SoundGraph
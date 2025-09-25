#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include <algorithm>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Clamp node that constrains Value between Min and Max
	template<typename T>
	class ClampNode : public NodeProcessor
	{
	private:
		//======================================================================
		// ValueView Streams for Real-Time Processing
		//======================================================================
		
		ValueView<T> m_ValueView;
		ValueView<T> m_MinView;
		ValueView<T> m_MaxView;
		ValueView<T> m_OutputView;

		//======================================================================
		// Current Parameter Values (from streams)
		//======================================================================
		
		T m_CurrentValue = T{};
		T m_CurrentMin = T{};
		T m_CurrentMax = T{};

	public:
		ClampNode()
		{
			//==================================================================
			// Initialize ValueView streams and setup Input/Output events
			//==================================================================
			
			// Initialize streams
			m_ValueView.Set(m_CurrentValue);
			m_MinView.Set(m_CurrentMin);
			m_MaxView.Set(m_CurrentMax);
			m_OutputView.Set(T{});

			//==================================================================
			// Setup Input Events
			//==================================================================
			
			AddInputEvent<T>("Value", "Value to clamp", 
				[this](T value) { 
					m_CurrentValue = value; 
					m_ValueView.Set(value); 
				});
			
			AddInputEvent<T>("Min", "Minimum value",
				[this](T value) { 
					m_CurrentMin = value; 
					m_MinView.Set(value); 
				});
			
			AddInputEvent<T>("Max", "Maximum value",
				[this](T value) { 
					m_CurrentMax = value; 
					m_MaxView.Set(value); 
				});

			//==================================================================
			// Setup Output Events  
			//==================================================================
			
			AddOutputEvent<T>("Output", "Clamped value output",
				[this]() -> T { return m_OutputView.Get(); });
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			//==================================================================
			// Update ValueView streams from current parameter values
			//==================================================================
			
			m_ValueView.Set(m_CurrentValue);
			m_MinView.Set(m_CurrentMin);
			m_MaxView.Set(m_CurrentMax);
			
			// Perform clamp operation - ensure min <= max for proper clamping
			T result = std::clamp(m_CurrentValue, std::min(m_CurrentMin, m_CurrentMax), std::max(m_CurrentMin, m_CurrentMax));
			
			// Update output stream
			m_OutputView.Set(result);
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
		}

		Identifier GetTypeID() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return OLO_IDENTIFIER("ClampNode_f32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("ClampNode_i32");
			else
				return OLO_IDENTIFIER("ClampNode_unknown");
		}

		const char* GetDisplayName() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return "Clamp (f32)";
			else if constexpr (std::is_same_v<T, i32>)
				return "Clamp (i32)";
			else
				return "Clamp (unknown)";
		}

		//======================================================================
		// Legacy API compatibility methods
		//======================================================================
		
		T GetValue() const { return m_CurrentValue; }
		T GetMin() const { return m_CurrentMin; }
		T GetMax() const { return m_CurrentMax; }
		T GetOutput() const { return m_OutputView.Get(); }

	private:
		// Parameter IDs are available as members for backwards compatibility
		// Value_ID, Min_ID, Max_ID, Output_ID concepts preserved
	};

	// Common type aliases
	using ClampNodeF32 = ClampNode<f32>;
	using ClampNodeI32 = ClampNode<i32>;

} // namespace OloEngine::Audio::SoundGraph
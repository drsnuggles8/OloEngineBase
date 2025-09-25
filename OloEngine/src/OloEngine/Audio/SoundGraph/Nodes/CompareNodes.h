#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/ValueView.h"
#include "OloEngine/Audio/SoundGraph/InputView.h"
#include "OloEngine/Audio/SoundGraph/OutputView.h"
#include "OloEngine/Core/Base.h"

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// EqualNode - Template node for equality comparison
	/// Converts from legacy parameters to ValueView system while preserving functionality
	template<typename T>
	class EqualNode : public NodeProcessor
	{
		static_assert(std::is_arithmetic_v<T> || std::is_same_v<T, bool>, "EqualNode can only be used with arithmetic types or bool");

	private:
		//======================================================================
		// ValueView System - Real-time Parameter Streams
		//======================================================================
		
		InputView<T> m_LeftInputView;
		InputView<T> m_RightInputView;
		OutputView<f32> m_OutputView;  // Output is always f32 (0.0f or 1.0f)
		
		// Current parameter values for legacy API compatibility
		T m_CurrentLeftInput = T{};
		T m_CurrentRightInput = T{};
		f32 m_CurrentOutput = 0.0f;

	public:
		EqualNode()
			: m_LeftInputView([this](T value) { m_CurrentLeftInput = value; }),
			  m_RightInputView([this](T value) { m_CurrentRightInput = value; }),
			  m_OutputView([this](f32 value) { m_CurrentOutput = value; })
		{
			// No parameter registration needed - handled by ValueView system
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_LeftInputView.Initialize(maxBufferSize);
			m_RightInputView.Initialize(maxBufferSize);
			m_OutputView.Initialize(maxBufferSize);
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_LeftInputView.UpdateFromConnections(inputs, numSamples);
			m_RightInputView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				T leftValue = m_LeftInputView.GetValue(sample);
				T rightValue = m_RightInputView.GetValue(sample);
				
				// Update internal state
				m_CurrentLeftInput = leftValue;
				m_CurrentRightInput = rightValue;
				
				// Perform comparison
				f32 result;
				if constexpr (std::is_same_v<T, f32>)
				{
					// For floating point, use small epsilon for comparison
					constexpr f32 epsilon = 1e-6f;
					result = (glm::abs(leftValue - rightValue) < epsilon) ? 1.0f : 0.0f;
				}
				else
				{
					// For integers and bools, use exact comparison
					result = (leftValue == rightValue) ? 1.0f : 0.0f;
				}
				
				// Set output value
				m_CurrentOutput = result;
				m_OutputView.SetValue(sample, result);
			}
			
			// Update output streams
			m_OutputView.UpdateOutputConnections(outputs, numSamples);
		}

		Identifier GetTypeID() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return OLO_IDENTIFIER("EqualNodeF32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("EqualNodeI32");
			else if constexpr (std::is_same_v<T, bool>)
				return OLO_IDENTIFIER("EqualNodeBool");
			else
				return OLO_IDENTIFIER("EqualNodeUnknown");
		}

		const char* GetDisplayName() const override { return "Equal"; }
		
		//======================================================================
		// Legacy API Compatibility Methods
		//======================================================================
		
		template<typename U>
		void SetParameterValue(const Identifier& id, U value)
		{
			if (id == OLO_IDENTIFIER("LeftInput")) m_CurrentLeftInput = static_cast<T>(value);
			else if (id == OLO_IDENTIFIER("RightInput")) m_CurrentRightInput = static_cast<T>(value);
		}

		template<typename U>
		U GetParameterValue(const Identifier& id) const
		{
			if (id == OLO_IDENTIFIER("LeftInput")) return static_cast<U>(m_CurrentLeftInput);
			else if (id == OLO_IDENTIFIER("RightInput")) return static_cast<U>(m_CurrentRightInput);
			else if (id == OLO_IDENTIFIER("Output")) return static_cast<U>(m_CurrentOutput);
			return U{};
		}
	};

	//==============================================================================
	/// GreaterThanNode - Template node for greater than comparison
	template<typename T>
	class GreaterThanNode : public NodeProcessor
	{
		static_assert(std::is_arithmetic_v<T>, "GreaterThanNode can only be used with arithmetic types");

	private:
		//======================================================================
		// ValueView System - Real-time Parameter Streams
		//======================================================================
		
		InputView<T> m_LeftInputView;
		InputView<T> m_RightInputView;
		OutputView<f32> m_OutputView;  // Output is always f32 (0.0f or 1.0f)
		
		// Current parameter values for legacy API compatibility
		T m_CurrentLeftInput = T{};
		T m_CurrentRightInput = T{};
		f32 m_CurrentOutput = 0.0f;

	public:
		GreaterThanNode()
			: m_LeftInputView([this](T value) { m_CurrentLeftInput = value; }),
			  m_RightInputView([this](T value) { m_CurrentRightInput = value; }),
			  m_OutputView([this](f32 value) { m_CurrentOutput = value; })
		{
			// No parameter registration needed - handled by ValueView system
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_LeftInputView.Initialize(maxBufferSize);
			m_RightInputView.Initialize(maxBufferSize);
			m_OutputView.Initialize(maxBufferSize);
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_LeftInputView.UpdateFromConnections(inputs, numSamples);
			m_RightInputView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				T leftValue = m_LeftInputView.GetValue(sample);
				T rightValue = m_RightInputView.GetValue(sample);
				
				// Update internal state
				m_CurrentLeftInput = leftValue;
				m_CurrentRightInput = rightValue;
				
				// Perform comparison
				f32 result = (leftValue > rightValue) ? 1.0f : 0.0f;
				
				// Set output value
				m_CurrentOutput = result;
				m_OutputView.SetValue(sample, result);
			}
			
			// Update output streams
			m_OutputView.UpdateOutputConnections(outputs, numSamples);
		}

		Identifier GetTypeID() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return OLO_IDENTIFIER("GreaterThanNodeF32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("GreaterThanNodeI32");
			else
				return OLO_IDENTIFIER("GreaterThanNodeUnknown");
		}

		const char* GetDisplayName() const override { return "Greater Than"; }
		
		//======================================================================
		// Legacy API Compatibility Methods
		//======================================================================
		
		template<typename U>
		void SetParameterValue(const Identifier& id, U value)
		{
			if (id == OLO_IDENTIFIER("LeftInput")) m_CurrentLeftInput = static_cast<T>(value);
			else if (id == OLO_IDENTIFIER("RightInput")) m_CurrentRightInput = static_cast<T>(value);
		}

		template<typename U>
		U GetParameterValue(const Identifier& id) const
		{
			if (id == OLO_IDENTIFIER("LeftInput")) return static_cast<U>(m_CurrentLeftInput);
			else if (id == OLO_IDENTIFIER("RightInput")) return static_cast<U>(m_CurrentRightInput);
			else if (id == OLO_IDENTIFIER("Output")) return static_cast<U>(m_CurrentOutput);
			return U{};
		}
	};

	//==============================================================================
	/// LessThanNode - Template node for less than comparison
	template<typename T>
	class LessThanNode : public NodeProcessor
	{
		static_assert(std::is_arithmetic_v<T>, "LessThanNode can only be used with arithmetic types");

	private:
		//======================================================================
		// ValueView System - Real-time Parameter Streams
		//======================================================================
		
		InputView<T> m_LeftInputView;
		InputView<T> m_RightInputView;
		OutputView<f32> m_OutputView;  // Output is always f32 (0.0f or 1.0f)
		
		// Current parameter values for legacy API compatibility
		T m_CurrentLeftInput = T{};
		T m_CurrentRightInput = T{};
		f32 m_CurrentOutput = 0.0f;

	public:
		LessThanNode()
			: m_LeftInputView([this](T value) { m_CurrentLeftInput = value; }),
			  m_RightInputView([this](T value) { m_CurrentRightInput = value; }),
			  m_OutputView([this](f32 value) { m_CurrentOutput = value; })
		{
			// No parameter registration needed - handled by ValueView system
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_LeftInputView.Initialize(maxBufferSize);
			m_RightInputView.Initialize(maxBufferSize);
			m_OutputView.Initialize(maxBufferSize);
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_LeftInputView.UpdateFromConnections(inputs, numSamples);
			m_RightInputView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				T leftValue = m_LeftInputView.GetValue(sample);
				T rightValue = m_RightInputView.GetValue(sample);
				
				// Update internal state
				m_CurrentLeftInput = leftValue;
				m_CurrentRightInput = rightValue;
				
				// Perform comparison
				f32 result = (leftValue < rightValue) ? 1.0f : 0.0f;
				
				// Set output value
				m_CurrentOutput = result;
				m_OutputView.SetValue(sample, result);
			}
			
			// Update output streams
			m_OutputView.UpdateOutputConnections(outputs, numSamples);
		}

		Identifier GetTypeID() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return OLO_IDENTIFIER("LessThanNodeF32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("LessThanNodeI32");
			else
				return OLO_IDENTIFIER("LessThanNodeUnknown");
		}

		const char* GetDisplayName() const override { return "Less Than"; }
		
		//======================================================================
		// Legacy API Compatibility Methods
		//======================================================================
		
		template<typename U>
		void SetParameterValue(const Identifier& id, U value)
		{
			if (id == OLO_IDENTIFIER("LeftInput")) m_CurrentLeftInput = static_cast<T>(value);
			else if (id == OLO_IDENTIFIER("RightInput")) m_CurrentRightInput = static_cast<T>(value);
		}

		template<typename U>
		U GetParameterValue(const Identifier& id) const
		{
			if (id == OLO_IDENTIFIER("LeftInput")) return static_cast<U>(m_CurrentLeftInput);
			else if (id == OLO_IDENTIFIER("RightInput")) return static_cast<U>(m_CurrentRightInput);
			else if (id == OLO_IDENTIFIER("Output")) return static_cast<U>(m_CurrentOutput);
			return U{};
		}
	};

	//==============================================================================
	/// GreaterThanOrEqualNode - Template node for greater than or equal comparison
	template<typename T>
	class GreaterThanOrEqualNode : public NodeProcessor
	{
		static_assert(std::is_arithmetic_v<T>, "GreaterThanOrEqualNode can only be used with arithmetic types");

	private:
		//======================================================================
		// ValueView System - Real-time Parameter Streams
		//======================================================================
		
		InputView<T> m_LeftInputView;
		InputView<T> m_RightInputView;
		OutputView<f32> m_OutputView;  // Output is always f32 (0.0f or 1.0f)
		
		// Current parameter values for legacy API compatibility
		T m_CurrentLeftInput = T{};
		T m_CurrentRightInput = T{};
		f32 m_CurrentOutput = 0.0f;

	public:
		GreaterThanOrEqualNode()
			: m_LeftInputView([this](T value) { m_CurrentLeftInput = value; }),
			  m_RightInputView([this](T value) { m_CurrentRightInput = value; }),
			  m_OutputView([this](f32 value) { m_CurrentOutput = value; })
		{
			// No parameter registration needed - handled by ValueView system
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_LeftInputView.Initialize(maxBufferSize);
			m_RightInputView.Initialize(maxBufferSize);
			m_OutputView.Initialize(maxBufferSize);
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_LeftInputView.UpdateFromConnections(inputs, numSamples);
			m_RightInputView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				T leftValue = m_LeftInputView.GetValue(sample);
				T rightValue = m_RightInputView.GetValue(sample);
				
				// Update internal state
				m_CurrentLeftInput = leftValue;
				m_CurrentRightInput = rightValue;
				
				// Perform comparison
				f32 result = (leftValue >= rightValue) ? 1.0f : 0.0f;
				
				// Set output value
				m_CurrentOutput = result;
				m_OutputView.SetValue(sample, result);
			}
			
			// Update output streams
			m_OutputView.UpdateOutputConnections(outputs, numSamples);
		}

		Identifier GetTypeID() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return OLO_IDENTIFIER("GreaterThanOrEqualNodeF32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("GreaterThanOrEqualNodeI32");
			else
				return OLO_IDENTIFIER("GreaterThanOrEqualNodeUnknown");
		}

		const char* GetDisplayName() const override { return "Greater Than Or Equal"; }
		
		//======================================================================
		// Legacy API Compatibility Methods
		//======================================================================
		
		template<typename U>
		void SetParameterValue(const Identifier& id, U value)
		{
			if (id == OLO_IDENTIFIER("LeftInput")) m_CurrentLeftInput = static_cast<T>(value);
			else if (id == OLO_IDENTIFIER("RightInput")) m_CurrentRightInput = static_cast<T>(value);
		}

		template<typename U>
		U GetParameterValue(const Identifier& id) const
		{
			if (id == OLO_IDENTIFIER("LeftInput")) return static_cast<U>(m_CurrentLeftInput);
			else if (id == OLO_IDENTIFIER("RightInput")) return static_cast<U>(m_CurrentRightInput);
			else if (id == OLO_IDENTIFIER("Output")) return static_cast<U>(m_CurrentOutput);
			return U{};
		}
	};

	//==============================================================================
	/// LessThanOrEqualNode - Template node for less than or equal comparison
	template<typename T>
	class LessThanOrEqualNode : public NodeProcessor
	{
		static_assert(std::is_arithmetic_v<T>, "LessThanOrEqualNode can only be used with arithmetic types");

	private:
		//======================================================================
		// ValueView System - Real-time Parameter Streams
		//======================================================================
		
		InputView<T> m_LeftInputView;
		InputView<T> m_RightInputView;
		OutputView<f32> m_OutputView;  // Output is always f32 (0.0f or 1.0f)
		
		// Current parameter values for legacy API compatibility
		T m_CurrentLeftInput = T{};
		T m_CurrentRightInput = T{};
		f32 m_CurrentOutput = 0.0f;

	public:
		LessThanOrEqualNode()
			: m_LeftInputView([this](T value) { m_CurrentLeftInput = value; }),
			  m_RightInputView([this](T value) { m_CurrentRightInput = value; }),
			  m_OutputView([this](f32 value) { m_CurrentOutput = value; })
		{
			// No parameter registration needed - handled by ValueView system
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_LeftInputView.Initialize(maxBufferSize);
			m_RightInputView.Initialize(maxBufferSize);
			m_OutputView.Initialize(maxBufferSize);
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_LeftInputView.UpdateFromConnections(inputs, numSamples);
			m_RightInputView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				T leftValue = m_LeftInputView.GetValue(sample);
				T rightValue = m_RightInputView.GetValue(sample);
				
				// Update internal state
				m_CurrentLeftInput = leftValue;
				m_CurrentRightInput = rightValue;
				
				// Perform comparison
				f32 result = (leftValue <= rightValue) ? 1.0f : 0.0f;
				
				// Set output value
				m_CurrentOutput = result;
				m_OutputView.SetValue(sample, result);
			}
			
			// Update output streams
			m_OutputView.UpdateOutputConnections(outputs, numSamples);
		}

		Identifier GetTypeID() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return OLO_IDENTIFIER("LessThanOrEqualNodeF32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("LessThanOrEqualNodeI32");
			else
				return OLO_IDENTIFIER("LessThanOrEqualNodeUnknown");
		}

		const char* GetDisplayName() const override { return "Less Than Or Equal"; }
		
		//======================================================================
		// Legacy API Compatibility Methods
		//======================================================================
		
		template<typename U>
		void SetParameterValue(const Identifier& id, U value)
		{
			if (id == OLO_IDENTIFIER("LeftInput")) m_CurrentLeftInput = static_cast<T>(value);
			else if (id == OLO_IDENTIFIER("RightInput")) m_CurrentRightInput = static_cast<T>(value);
		}

		template<typename U>
		U GetParameterValue(const Identifier& id) const
		{
			if (id == OLO_IDENTIFIER("LeftInput")) return static_cast<U>(m_CurrentLeftInput);
			else if (id == OLO_IDENTIFIER("RightInput")) return static_cast<U>(m_CurrentRightInput);
			else if (id == OLO_IDENTIFIER("Output")) return static_cast<U>(m_CurrentOutput);
			return U{};
		}
	};

	//==============================================================================
	/// NotEqualNode - Template node for inequality comparison
	template<typename T>
	class NotEqualNode : public NodeProcessor
	{
		static_assert(std::is_arithmetic_v<T>, "NotEqualNode can only be used with arithmetic types");

	private:
		//======================================================================
		// ValueView System - Real-time Parameter Streams
		//======================================================================
		
		InputView<T> m_LeftInputView;
		InputView<T> m_RightInputView;
		OutputView<f32> m_OutputView;  // Output is always f32 (0.0f or 1.0f)
		
		// Current parameter values for legacy API compatibility
		T m_CurrentLeftInput = T{};
		T m_CurrentRightInput = T{};
		f32 m_CurrentOutput = 0.0f;

	public:
		NotEqualNode()
			: m_LeftInputView([this](T value) { m_CurrentLeftInput = value; }),
			  m_RightInputView([this](T value) { m_CurrentRightInput = value; }),
			  m_OutputView([this](f32 value) { m_CurrentOutput = value; })
		{
			// No parameter registration needed - handled by ValueView system
		}

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			NodeProcessor::Initialize(sampleRate, maxBufferSize);
			
			// Initialize ValueView streams
			m_LeftInputView.Initialize(maxBufferSize);
			m_RightInputView.Initialize(maxBufferSize);
			m_OutputView.Initialize(maxBufferSize);
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Update ValueView streams from inputs
			m_LeftInputView.UpdateFromConnections(inputs, numSamples);
			m_RightInputView.UpdateFromConnections(inputs, numSamples);
			
			for (u32 sample = 0; sample < numSamples; ++sample)
			{
				// Get current values from streams
				T leftValue = m_LeftInputView.GetValue(sample);
				T rightValue = m_RightInputView.GetValue(sample);
				
				// Update internal state
				m_CurrentLeftInput = leftValue;
				m_CurrentRightInput = rightValue;
				
				// Perform comparison
				f32 result;
				if constexpr (std::is_same_v<T, f32>)
				{
					// For floating point, use small epsilon for comparison
					constexpr f32 epsilon = 1e-6f;
					result = (glm::abs(leftValue - rightValue) >= epsilon) ? 1.0f : 0.0f;
				}
				else
				{
					// For integers, use exact comparison
					result = (leftValue != rightValue) ? 1.0f : 0.0f;
				}
				
				// Set output value
				m_CurrentOutput = result;
				m_OutputView.SetValue(sample, result);
			}
			
			// Update output streams
			m_OutputView.UpdateOutputConnections(outputs, numSamples);
		}

		Identifier GetTypeID() const override
		{
			if constexpr (std::is_same_v<T, f32>)
				return OLO_IDENTIFIER("NotEqualNodeF32");
			else if constexpr (std::is_same_v<T, i32>)
				return OLO_IDENTIFIER("NotEqualNodeI32");
			else
				return OLO_IDENTIFIER("NotEqualNodeUnknown");
		}

		const char* GetDisplayName() const override { return "Not Equal"; }
		
		//======================================================================
		// Legacy API Compatibility Methods
		//======================================================================
		
		template<typename U>
		void SetParameterValue(const Identifier& id, U value)
		{
			if (id == OLO_IDENTIFIER("LeftInput")) m_CurrentLeftInput = static_cast<T>(value);
			else if (id == OLO_IDENTIFIER("RightInput")) m_CurrentRightInput = static_cast<T>(value);
		}

		template<typename U>
		U GetParameterValue(const Identifier& id) const
		{
			if (id == OLO_IDENTIFIER("LeftInput")) return static_cast<U>(m_CurrentLeftInput);
			else if (id == OLO_IDENTIFIER("RightInput")) return static_cast<U>(m_CurrentRightInput);
			else if (id == OLO_IDENTIFIER("Output")) return static_cast<U>(m_CurrentOutput);
			return U{};
		}
	};

	// Type aliases for convenience
	using EqualNodeF32 = EqualNode<f32>;
	using EqualNodeI32 = EqualNode<i32>;
	using EqualNodeBool = EqualNode<bool>;
	using GreaterThanNodeF32 = GreaterThanNode<f32>;
	using GreaterThanNodeI32 = GreaterThanNode<i32>;
	using LessThanNodeF32 = LessThanNode<f32>;
	using LessThanNodeI32 = LessThanNode<i32>;
	using GreaterThanOrEqualNodeF32 = GreaterThanOrEqualNode<f32>;
	using GreaterThanOrEqualNodeI32 = GreaterThanOrEqualNode<i32>;
	using LessThanOrEqualNodeF32 = LessThanOrEqualNode<f32>;
	using LessThanOrEqualNodeI32 = LessThanOrEqualNode<i32>;
	using NotEqualNodeF32 = NotEqualNode<f32>;
	using NotEqualNodeI32 = NotEqualNode<i32>;

} // namespace OloEngine::Audio::SoundGraph
#pragma once

#include "OloEngine/Audio/SoundGraph/NodeProcessor.h"
#include "OloEngine/Audio/SoundGraph/Value.h"
#include "OloEngine/Audio/SoundGraph/Flag.h"
#include "OloEngine/Audio/SoundGraph/Events.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Identifier.h"
#include <cmath>
#include <glm/glm.hpp>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// AREnvelope - Attack-Release envelope generator using new Hazel-style foundation
	/// Provides a simple two-phase envelope ideal for percussive sounds
	/// Features retrigger capability and velocity scaling
	class AREnvelope : public NodeProcessor
	{
	public:
		enum class State
		{
			Idle,
			Attack,
			Release
		};

	private:
		// Input streams for connected values
		ValueView<f32> m_AttackTimeStream;
		ValueView<f32> m_ReleaseTimeStream;
		ValueView<f32> m_AttackCurveStream;
		ValueView<f32> m_ReleaseCurveStream;
		ValueView<f32> m_PeakStream;
		ValueView<f32> m_VelocityStream;
		
		// Output stream for envelope values
		ValueView<f32> m_OutputStream;
		ValueView<bool> m_CompletedStream;

		// Current parameter values (for single-value processing)
		f32 m_CurrentAttackTime = 0.01f;     // seconds
		f32 m_CurrentReleaseTime = 0.2f;     // seconds
		f32 m_CurrentAttackCurve = 1.0f;     // curve factor
		f32 m_CurrentReleaseCurve = 1.0f;    // curve factor
		f32 m_CurrentPeak = 1.0f;            // peak level
		f32 m_CurrentVelocity = 1.0f;        // note velocity
		bool m_CurrentRetrigger = true;      // allow retrigger
		f32 m_CurrentOutput = 0.0f;
		bool m_CurrentCompleted = false;

		// Envelope state
		State m_CurrentState = State::Idle;
		f32 m_CurrentValue = 0.0f;
		u32 m_AttackSamples = 0;
		u32 m_ReleaseSamples = 0;
		u32 m_CurrentSample = 0;
		f64 m_SampleRate = 44100.0;

		// Events and flags
		Flag m_TriggerFlag;

		// Parameter limits
		static constexpr f32 MIN_TIME_SECONDS = 0.001f;  // 1ms minimum
		static constexpr f32 MAX_TIME_SECONDS = 10.0f;   // 10s maximum
		static constexpr f32 MIN_LEVEL = 0.0f;
		static constexpr f32 MAX_LEVEL = 1.0f;
		static constexpr f32 MIN_CURVE = 0.1f;
		static constexpr f32 MAX_CURVE = 10.0f;

	public:
		AREnvelope()
		{
			// Create input events for receiving parameter values
			auto attackTimeEvent = std::make_shared<InputEvent>("AttackTime", [this](const Value& value) {
				if (value.GetType() == ValueType::Float32)
				{
					m_CurrentAttackTime = glm::clamp(value.Get<f32>(), MIN_TIME_SECONDS, MAX_TIME_SECONDS);
				}
			});

			auto releaseTimeEvent = std::make_shared<InputEvent>("ReleaseTime", [this](const Value& value) {
				if (value.GetType() == ValueType::Float32)
				{
					m_CurrentReleaseTime = glm::clamp(value.Get<f32>(), MIN_TIME_SECONDS, MAX_TIME_SECONDS);
				}
			});

			auto attackCurveEvent = std::make_shared<InputEvent>("AttackCurve", [this](const Value& value) {
				if (value.GetType() == ValueType::Float32)
				{
					m_CurrentAttackCurve = glm::clamp(value.Get<f32>(), MIN_CURVE, MAX_CURVE);
				}
			});

			auto releaseCurveEvent = std::make_shared<InputEvent>("ReleaseCurve", [this](const Value& value) {
				if (value.GetType() == ValueType::Float32)
				{
					m_CurrentReleaseCurve = glm::clamp(value.Get<f32>(), MIN_CURVE, MAX_CURVE);
				}
			});

			auto peakEvent = std::make_shared<InputEvent>("Peak", [this](const Value& value) {
				if (value.GetType() == ValueType::Float32)
				{
					m_CurrentPeak = glm::clamp(value.Get<f32>(), MIN_LEVEL, MAX_LEVEL);
				}
			});

			auto velocityEvent = std::make_shared<InputEvent>("Velocity", [this](const Value& value) {
				if (value.GetType() == ValueType::Float32)
				{
					m_CurrentVelocity = glm::clamp(value.Get<f32>(), MIN_LEVEL, MAX_LEVEL);
				}
			});

			auto retriggerEvent = std::make_shared<InputEvent>("Retrigger", [this](const Value& value) {
				if (value.GetType() == ValueType::Bool)
				{
					m_CurrentRetrigger = value.Get<bool>();
				}
			});

			auto triggerEvent = std::make_shared<InputEvent>("Trigger", [this](const Value& value) {
				if (value.GetType() == ValueType::Bool && value.Get<bool>())
				{
					TriggerEnvelope();
				}
			});

			// Register all input events
			AddInputEvent(attackTimeEvent);
			AddInputEvent(releaseTimeEvent);
			AddInputEvent(attackCurveEvent);
			AddInputEvent(releaseCurveEvent);
			AddInputEvent(peakEvent);
			AddInputEvent(velocityEvent);
			AddInputEvent(retriggerEvent);
			AddInputEvent(triggerEvent);

			// Create output events for sending envelope values
			auto outputEvent = std::make_shared<OutputEvent>("Output");
			auto completedEvent = std::make_shared<OutputEvent>("Completed");
			AddOutputEvent(outputEvent);
			AddOutputEvent(completedEvent);
		}

		virtual ~AREnvelope() = default;

		//======================================================================
		// NodeProcessor Implementation
		//======================================================================

		void Initialize(f64 sampleRate, u32 maxBufferSize) override
		{
			m_SampleRate = sampleRate;
			
			// Initialize ValueView streams with proper capacity
			m_AttackTimeStream = ValueView<f32>(maxBufferSize);
			m_ReleaseTimeStream = ValueView<f32>(maxBufferSize);
			m_AttackCurveStream = ValueView<f32>(maxBufferSize);
			m_ReleaseCurveStream = ValueView<f32>(maxBufferSize);
			m_PeakStream = ValueView<f32>(maxBufferSize);
			m_VelocityStream = ValueView<f32>(maxBufferSize);
			m_OutputStream = ValueView<f32>(maxBufferSize);
			m_CompletedStream = ValueView<bool>(maxBufferSize);

			// Reset envelope state
			m_CurrentState = State::Idle;
			m_CurrentValue = 0.0f;
			m_CurrentSample = 0;
		}

		void Process(f32** inputs, f32** outputs, u32 numSamples) override
		{
			// Handle stream processing mode (real-time audio)
			if (outputs && outputs[0])
			{
				// Process envelope for each sample
				for (u32 sample = 0; sample < numSamples; ++sample)
				{
					// Check for trigger event (flag set from input event)
					if (m_TriggerFlag.CheckAndResetIfDirty())
					{
						TriggerEnvelope();
					}

					// Update envelope based on current state
					bool justCompleted = UpdateEnvelope();

					// Fill output stream and audio buffer
					m_OutputStream[sample] = m_CurrentValue;
					m_CompletedStream[sample] = justCompleted;
					outputs[0][sample] = m_CurrentValue;
				}

				// Update current output values with last sample
				m_CurrentOutput = m_OutputStream[numSamples - 1];
				m_CurrentCompleted = m_CompletedStream[numSamples - 1];

				// Send outputs via event system
				auto outputEvent = GetOutputEvent("Output");
				auto completedEvent = GetOutputEvent("Completed");
				if (outputEvent)
				{
					outputEvent->SendValue(Value(m_CurrentOutput));
				}
				if (completedEvent && m_CurrentCompleted)
				{
					completedEvent->SendValue(Value(true));
				}
			}
			else
			{
				// Handle single-value processing mode (control parameters)
				// Check for trigger event
				if (m_TriggerFlag.CheckAndResetIfDirty())
				{
					TriggerEnvelope();
				}

				// Update envelope for single sample
				bool justCompleted = UpdateEnvelope();

				m_CurrentOutput = m_CurrentValue;
				m_CurrentCompleted = justCompleted;

				// Send outputs via event system
				auto outputEvent = GetOutputEvent("Output");
				auto completedEvent = GetOutputEvent("Completed");
				if (outputEvent)
				{
					outputEvent->SendValue(Value(m_CurrentOutput));
				}
				if (completedEvent && m_CurrentCompleted)
				{
					completedEvent->SendValue(Value(true));
				}
			}
		}

		Identifier GetTypeID() const override
		{
			return OLO_IDENTIFIER("AREnvelope");
		}

		const char* GetDisplayName() const override
		{
			return "AR Envelope";
		}

		//======================================================================
		// Envelope Specific Methods
		//======================================================================

		void TriggerEnvelope()
		{
			// Check retrigger capability
			if (m_CurrentState != State::Idle && m_CurrentRetrigger < 0.5f)
			{
				return; // Cannot retrigger
			}

			// Calculate envelope parameters
			f32 attackTime = glm::max(0.001f, m_CurrentAttackTime);
			f32 releaseTime = glm::max(0.001f, m_CurrentReleaseTime);

			m_AttackSamples = static_cast<u32>(attackTime * m_SampleRate);
			m_ReleaseSamples = static_cast<u32>(releaseTime * m_SampleRate);

			// Start envelope
			m_CurrentState = State::Attack;
			m_CurrentSample = 0;
		}

		void UpdateEnvelope()
		{
			const f32 peak = m_CurrentPeak * m_CurrentVelocity;
			const f32 attackCurve = glm::clamp(m_CurrentAttackCurve, 0.1f, 10.0f);
			const f32 releaseCurve = glm::clamp(m_CurrentReleaseCurve, 0.1f, 10.0f);

			switch (m_CurrentState)
			{
			case State::Attack:
				if (m_CurrentSample < m_AttackSamples)
				{
					f32 progress = static_cast<f32>(m_CurrentSample) / m_AttackSamples;
					f32 curvedProgress = glm::pow(progress, 1.0f / attackCurve);
					m_CurrentValue = curvedProgress * peak;
					m_CurrentSample++;
				}
				else
				{
					m_CurrentValue = peak;
					m_CurrentState = State::Release;
					m_CurrentSample = 0;
				}
				break;

			case State::Release:
				if (m_CurrentSample < m_ReleaseSamples)
				{
					f32 progress = static_cast<f32>(m_CurrentSample) / m_ReleaseSamples;
					f32 curvedProgress = glm::pow(progress, releaseCurve);
					m_CurrentValue = peak * (1.0f - curvedProgress);
					m_CurrentSample++;
				}
				else
				{
					m_CurrentValue = 0.0f;
					m_CurrentState = State::Idle;
					
					// Fire completion event
					if (m_CompletedEvent)
					{
						(*m_CompletedEvent)(1.0f);
					}
				}
				break;

			case State::Idle:
				m_CurrentValue = 0.0f;
				break;
			}
		}

		void ResetEnvelope()
		{
			m_CurrentState = State::Idle;
			m_CurrentValue = 0.0f;
			m_CurrentSample = 0;
			m_CurrentVelocity = 1.0f;
		}

		// Utility methods for external access
		State GetCurrentState() const { return m_CurrentState; }
		f32 GetCurrentValue() const { return m_CurrentValue; }
		f32 GetCurrentVelocity() const { return m_CurrentVelocity; }
		bool IsActive() const { return m_CurrentState != State::Idle; }
		bool CanRetrigger() const { return m_CurrentRetrigger > 0.5f; }
	};

} // namespace OloEngine::Audio::SoundGraph
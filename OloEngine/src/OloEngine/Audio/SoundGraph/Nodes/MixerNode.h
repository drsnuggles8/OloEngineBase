#pragma once

#include "../NodeProcessor.h"
#include <vector>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Mixer Node - mixes multiple audio inputs into one output
	class MixerNode : public NodeProcessor
	{
	public:
		// Endpoint identifiers
		struct EndpointIDs
		{
			static constexpr const char* OutputLeft = "OutLeft";
			static constexpr const char* OutputRight = "OutRight";
			static constexpr const char* MasterVolume = "MasterVolume";

			// Dynamic input endpoints (Input1Left, Input1Right, Input1Volume, etc.)
			static std::string GetInputLeftName(u32 index) { return "Input" + std::to_string(index + 1) + "Left"; }
			static std::string GetInputRightName(u32 index) { return "Input" + std::to_string(index + 1) + "Right"; }
			static std::string GetInputVolumeName(u32 index) { return "Input" + std::to_string(index + 1) + "Volume"; }
			static std::string GetInputMuteName(u32 index) { return "Input" + std::to_string(index + 1) + "Mute"; }
		};

		explicit MixerNode(std::string_view debugName, UUID id, u32 numInputs = 4);
		virtual ~MixerNode() = default;

		// NodeProcessor overrides
		void Process(f32* leftChannel, f32* rightChannel, u32 numSamples) override;
		void Update(f32 deltaTime) override;
		void Initialize(f64 sampleRate) override;
		void Reset() override;

		//==============================================================================
		/// Configuration

		// Set the number of input channels
		void SetNumInputs(u32 numInputs);
		u32 GetNumInputs() const { return m_NumInputs; }

		// Set volume for a specific input
		void SetInputVolume(u32 inputIndex, f32 volume);
		f32 GetInputVolume(u32 inputIndex) const;

		// Mute/unmute a specific input
		void SetInputMute(u32 inputIndex, bool mute);
		bool IsInputMuted(u32 inputIndex) const;

		// Set master volume
		void SetMasterVolume(f32 volume) { m_MasterVolume = volume; }
		f32 GetMasterVolume() const { return m_MasterVolume; }

		// Add input endpoint (for serialization)
		void AddInputEndpoint() { SetNumInputs(m_NumInputs + 1); }
		void AddInputEndpoint(const std::string& /*name*/) { SetNumInputs(m_NumInputs + 1); }

	private:
		struct InputChannel
		{
			f32 LeftInput = 0.0f;
			f32 RightInput = 0.0f;
			f32 Volume = 1.0f;
			bool Muted = false;
		};

		u32 m_NumInputs = 4;
		std::vector<InputChannel> m_Inputs;

		// Master controls
		f32 m_MasterVolume = 1.0f;

		// Output values
		f32 m_OutputLeft = 0.0f;
		f32 m_OutputRight = 0.0f;

		//==============================================================================
		/// Internal methods

		void InitializeEndpoints();
		void ResizeInputs(u32 newSize);
	};

	//==============================================================================
	/// Gain Node - simple volume control
	class GainNode : public NodeProcessor
	{
	public:
		// Endpoint identifiers
		struct EndpointIDs
		{
			static constexpr const char* InputLeft = "InLeft";
			static constexpr const char* InputRight = "InRight";
			static constexpr const char* OutputLeft = "OutLeft";
			static constexpr const char* OutputRight = "OutRight";
			static constexpr const char* Gain = "Gain";
			static constexpr const char* Mute = "Mute";
		};

		explicit GainNode(std::string_view debugName, UUID id);
		virtual ~GainNode() = default;

		// NodeProcessor overrides
		void Process(f32* leftChannel, f32* rightChannel, u32 numSamples) override;
		void Update(f32 deltaTime) override;
		void Initialize(f64 sampleRate) override;
		void Reset() override;

		//==============================================================================
		/// Configuration

		void SetGain(f32 gain) { m_Gain = gain; }
		f32 GetGain() const { return m_Gain; }

		// Alias for serialization compatibility
		void SetVolume(f32 volume) { SetGain(volume); }
		f32 GetVolume() const { return GetGain(); }

		void SetMute(bool mute) { m_Mute = mute; }
		bool IsMuted() const { return m_Mute; }

	private:
		// Input values
		f32 m_InputLeft = 0.0f;
		f32 m_InputRight = 0.0f;

		// Parameters
		f32 m_Gain = 1.0f;
		bool m_Mute = false;

		// Output values
		f32 m_OutputLeft = 0.0f;
		f32 m_OutputRight = 0.0f;

		//==============================================================================
		/// Internal methods

		void InitializeEndpoints();
	};
}
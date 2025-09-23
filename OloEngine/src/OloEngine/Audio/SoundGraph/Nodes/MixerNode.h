#pragma once

#include "../NodeProcessor.h"
#include "OloEngine/Core/Identifier.h"
#include <vector>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// MixerNode - Mixes multiple audio inputs into one output
	/// Supports configurable number of inputs with individual volume and mute controls
	/// Essential for combining multiple audio sources in sound graphs
	class MixerNode : public NodeProcessor
	{
	private:
		// Parameter identifiers
		const Identifier MasterVolume_ID = OLO_IDENTIFIER("MasterVolume");
		const Identifier NumInputs_ID = OLO_IDENTIFIER("NumInputs");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

		// Dynamic input parameter prefixes
		static constexpr const char* INPUT_PREFIX = "Input";
		static constexpr const char* VOLUME_PREFIX = "Volume";
		static constexpr const char* MUTE_PREFIX = "Mute";

		// State
		u32 m_NumInputs = 4;
		f64 m_SampleRate = 48000.0;

		// Helper methods
		Identifier GetInputID(u32 index) const;
		Identifier GetVolumeID(u32 index) const;
		Identifier GetMuteID(u32 index) const;
		void UpdateParameterCount();

	public:
		MixerNode();
		~MixerNode() override = default;

		// NodeProcessor interface
		void Process(f32** inputs, f32** outputs, u32 numSamples) override;
		void Initialize(f64 sampleRate, u32 samplesPerBlock) override;
		void Reset();

		// Node identification
		[[nodiscard]] Identifier GetTypeID() const override { return OLO_IDENTIFIER("MixerNode"); }
		[[nodiscard]] const char* GetDisplayName() const override { return "Mixer"; }

		// Configuration
		void SetNumInputs(u32 numInputs);
		u32 GetNumInputs() const { return m_NumInputs; }

		// Individual input controls
		void SetInputVolume(u32 inputIndex, f32 volume);
		f32 GetInputVolume(u32 inputIndex) const;
		void SetInputMute(u32 inputIndex, bool mute);
		bool IsInputMuted(u32 inputIndex) const;

		// Master controls
		void SetMasterVolume(f32 volume);
		f32 GetMasterVolume() const;
	};

	//==============================================================================
	/// GainNode - Simple volume control for audio signals
	/// Single input/output with gain and mute control
	class GainNode : public NodeProcessor
	{
	private:
		// Parameter identifiers
		const Identifier Input_ID = OLO_IDENTIFIER("Input");
		const Identifier Gain_ID = OLO_IDENTIFIER("Gain");
		const Identifier Mute_ID = OLO_IDENTIFIER("Mute");
		const Identifier Output_ID = OLO_IDENTIFIER("Output");

		f64 m_SampleRate = 48000.0;

	public:
		GainNode();
		~GainNode() override = default;

		// NodeProcessor interface
		void Process(f32** inputs, f32** outputs, u32 numSamples) override;
		void Initialize(f64 sampleRate, u32 samplesPerBlock) override;
		void Reset();

		// Node identification
		[[nodiscard]] Identifier GetTypeID() const override { return OLO_IDENTIFIER("GainNode"); }
		[[nodiscard]] const char* GetDisplayName() const override { return "Gain"; }

		// Controls
		void SetGain(f32 gain);
		f32 GetGain() const;
		void SetMute(bool mute);
		bool IsMuted() const;
	};

} // namespace OloEngine::Audio::SoundGraph
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
		//======================================================================
		// ValueView Streams for Real-Time Audio Processing
		//======================================================================
		
		// Fixed control streams
		ValueView<f32> m_MasterVolumeView;
		ValueView<f32> m_NumInputsView;
		ValueView<f32> m_OutputView;
		
		// Dynamic input streams - will be resized based on NumInputs
		std::vector<ValueView<f32>> m_InputViews;
		std::vector<ValueView<f32>> m_VolumeViews;
		std::vector<ValueView<f32>> m_MuteViews;

		//======================================================================
		// Current Parameter Values (from streams)
		//======================================================================
		
		f32 m_CurrentMasterVolume = 1.0f;
		f32 m_CurrentNumInputs = 4.0f;
		std::vector<f32> m_CurrentInputs;
		std::vector<f32> m_CurrentVolumes;
		std::vector<f32> m_CurrentMutes;

		// State
		u32 m_NumInputs = 4;
		f64 m_SampleRate = 48000.0;

	public:
		MixerNode()
		{
			//==================================================================
			// Initialize ValueView streams and setup Input/Output events
			//==================================================================
			
			// Initialize fixed control streams
			m_MasterVolumeView.Set(m_CurrentMasterVolume);
			m_NumInputsView.Set(m_CurrentNumInputs);
			m_OutputView.Set(0.0f);
			
			// Initialize dynamic input arrays  
			SetNumInputs(static_cast<u32>(m_CurrentNumInputs));

			//==================================================================
			// Setup Fixed Input Events
			//==================================================================
			
			AddInputEvent<f32>("MasterVolume", "Master volume control",
				[this](f32 value) { 
					m_CurrentMasterVolume = glm::clamp(value, 0.0f, 2.0f);
					m_MasterVolumeView.Set(m_CurrentMasterVolume); 
				});
			
			AddInputEvent<f32>("NumInputs", "Number of input channels",
				[this](f32 value) { 
					const u32 newNumInputs = static_cast<u32>(glm::clamp(value, 1.0f, 16.0f));
					if (newNumInputs != m_NumInputs)
					{
						SetNumInputs(newNumInputs);
					}
					m_CurrentNumInputs = static_cast<f32>(newNumInputs);
					m_NumInputsView.Set(m_CurrentNumInputs); 
				});

			//==================================================================
			// Setup Output Events  
			//==================================================================
			
			AddOutputEvent<f32>("Output", "Mixed audio output",
				[this]() -> f32 { return m_OutputView.Get(); });
		}
		~MixerNode() override = default;

		// NodeProcessor interface
		void Process(f32** inputs, f32** outputs, u32 numSamples) override;
		void Initialize(f64 sampleRate, u32 samplesPerBlock) override;
		void Reset();

		// Node identification
		[[nodiscard]] Identifier GetTypeID() const override { return OLO_IDENTIFIER("MixerNode"); }
		[[nodiscard]] const char* GetDisplayName() const override { return "Mixer"; }

		// Configuration
		void SetNumInputs(u32 numInputs)
		{
			if (numInputs == m_NumInputs)
				return;
				
			m_NumInputs = numInputs;
			
			// Resize arrays
			m_InputViews.resize(numInputs);
			m_VolumeViews.resize(numInputs);
			m_MuteViews.resize(numInputs);
			m_CurrentInputs.resize(numInputs, 0.0f);
			m_CurrentVolumes.resize(numInputs, 1.0f);
			m_CurrentMutes.resize(numInputs, 0.0f);
			
			// Setup dynamic input/output events for new inputs
			for (u32 i = 0; i < numInputs; ++i)
			{
				// Initialize streams
				m_InputViews[i].Set(m_CurrentInputs[i]);
				m_VolumeViews[i].Set(m_CurrentVolumes[i]);
				m_MuteViews[i].Set(m_CurrentMutes[i]);
				
				// Add input events (this will replace existing ones if called multiple times)
				const std::string inputName = "Input" + std::to_string(i);
				const std::string volumeName = "Volume" + std::to_string(i); 
				const std::string muteName = "Mute" + std::to_string(i);
				
				AddInputEvent<f32>(inputName, "Audio input " + std::to_string(i),
					[this, i](f32 value) { 
						if (i < m_CurrentInputs.size()) {
							m_CurrentInputs[i] = value; 
							m_InputViews[i].Set(value); 
						}
					});
				
				AddInputEvent<f32>(volumeName, "Volume control for input " + std::to_string(i),
					[this, i](f32 value) { 
						if (i < m_CurrentVolumes.size()) {
							m_CurrentVolumes[i] = glm::clamp(value, 0.0f, 2.0f); 
							m_VolumeViews[i].Set(m_CurrentVolumes[i]); 
						}
					});
				
				AddInputEvent<f32>(muteName, "Mute control for input " + std::to_string(i),
					[this, i](f32 value) { 
						if (i < m_CurrentMutes.size()) {
							m_CurrentMutes[i] = value; 
							m_MuteViews[i].Set(m_CurrentMutes[i]); 
						}
					});
			}
		}
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
		//======================================================================
		// ValueView Streams for Real-Time Audio Processing
		//======================================================================
		
		ValueView<f32> m_InputView;
		ValueView<f32> m_GainView;
		ValueView<f32> m_MuteView;
		ValueView<f32> m_OutputView;

		//======================================================================
		// Current Parameter Values (from streams)
		//======================================================================
		
		f32 m_CurrentInput = 0.0f;
		f32 m_CurrentGain = 1.0f;
		f32 m_CurrentMute = 0.0f;

		f64 m_SampleRate = 48000.0;

	public:
		GainNode()
		{
			//==================================================================
			// Initialize ValueView streams and setup Input/Output events
			//==================================================================
			
			// Initialize streams
			m_InputView.Set(m_CurrentInput);
			m_GainView.Set(m_CurrentGain);
			m_MuteView.Set(m_CurrentMute);
			m_OutputView.Set(0.0f);

			//==================================================================
			// Setup Input Events
			//==================================================================
			
			AddInputEvent<f32>("Input", "Audio input", 
				[this](f32 value) { 
					m_CurrentInput = value; 
					m_InputView.Set(value); 
				});
			
			AddInputEvent<f32>("Gain", "Volume gain control",
				[this](f32 value) { 
					m_CurrentGain = glm::clamp(value, 0.0f, 4.0f);
					m_GainView.Set(m_CurrentGain); 
				});
			
			AddInputEvent<f32>("Mute", "Mute control",
				[this](f32 value) { 
					m_CurrentMute = value;
					m_MuteView.Set(m_CurrentMute); 
				});

			//==================================================================
			// Setup Output Events  
			//==================================================================
			
			AddOutputEvent<f32>("Output", "Gained audio output",
				[this]() -> f32 { return m_OutputView.Get(); });
		}
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
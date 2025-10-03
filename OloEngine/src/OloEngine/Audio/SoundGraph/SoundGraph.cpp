#include "OloEnginePCH.h"
#include "SoundGraph.h"
#include "OloEngine/Asset/SoundGraphAsset.h"

namespace OloEngine::Audio::SoundGraph
{
	void SoundGraph::Play()
	{
		OLO_PROFILE_FUNCTION();

		if (!m_IsPlaying)
		{
			OnPlay(1.0f);
		}
	}

	void SoundGraph::Stop()
	{
		OLO_PROFILE_FUNCTION();

		if (m_IsPlaying)
		{
			OnStop(0.0f);
		}
	}

	void SoundGraph::TriggerGraphEvent(std::string_view eventName, f32 value)
	{
		OLO_PROFILE_FUNCTION();

		// Add event to queue for processing in audio thread
		Audio::AudioThreadEvent event;
		event.m_FrameIndex = m_CurrentFrame;
		event.m_EndpointID = static_cast<u32>(Identifier(eventName));
		
		choc::value::Value valueData = choc::value::createFloat32(value);
		event.m_ValueData.CopyFrom(valueData);
		
		m_OutgoingEvents.Push(event);

		// Also trigger immediately if it's a graph-level event
		if (eventName == "play")
		{
			Play();
		}
		else if (eventName == "stop")
		{
			Stop();
		}
	}

	void SoundGraph::InitializeEndpoints()
	{
		OLO_PROFILE_FUNCTION();

		// Set up event endpoints
		AddInEvent(IDs::Play, [this](f32 value) { OnPlay(value); });
		AddInEvent(IDs::Stop, [this](f32 value) { OnStop(value); });
	}

	void SoundGraph::ProcessEvents()
	{
		// TODO(olbu): Process any internal events
	}

	void SoundGraph::ProcessConnections()
	{
		// TODO(olbu): Process all connections in the graph
	}

	void SoundGraph::OnPlay(f32 value)
	{
		OLO_PROFILE_FUNCTION();

		(void)value;
		m_IsPlaying = true;
		m_HasFinished = false;
		OLO_CORE_TRACE("[SoundGraph] Started playing sound graph");
	}

	void SoundGraph::OnStop(f32 value)
	{
		OLO_PROFILE_FUNCTION();

		(void)value;
		m_IsPlaying = false;
		m_HasFinished = false;
		OLO_CORE_TRACE("[SoundGraph] Stopped sound graph");
	}

	void SoundGraph::OnFinished(f32 value)
	{
		OLO_PROFILE_FUNCTION();

		(void)value;
		m_HasFinished = true;
		m_IsPlaying = false;
		OLO_CORE_TRACE("[SoundGraph] Sound graph finished");
	}

	// TODO(olbu): This doesn't do anything yet, implement proper version
	void SoundGraph::UpdateFromAssetData([[maybe_unused]] const SoundGraphAsset& asset)
	{
		OLO_PROFILE_FUNCTION();
		
		// Clear existing state
		m_IsPlaying = false;
		m_HasFinished = false;
		m_OutgoingEvents.Clear();
		m_OutgoingMessages.Clear();
		OLO_CORE_INFO("Updated sound graph from asset data");
	}

} // namespace OloEngine::Audio::SoundGraph

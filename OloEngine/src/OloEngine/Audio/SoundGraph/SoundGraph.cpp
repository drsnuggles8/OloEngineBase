#include "OloEnginePCH.h"
#include "SoundGraph.h"
#include "OloEngine/Asset/SoundGraphAsset.h"

namespace OloEngine::Audio::SoundGraph
{
	void SoundGraph::Play()
	{
		if (!m_IsPlaying)
		{
			m_IsPlaying = true;
			m_HasFinished = false;
			OnPlay(1.0f);
			OLO_CORE_TRACE("[SoundGraph] Started playing sound graph");
		}
	}

	void SoundGraph::Stop()
	{
		if (m_IsPlaying)
		{
			m_IsPlaying = false;
			OnStop(0.0f);
			OLO_CORE_TRACE("[SoundGraph] Stopped sound graph");
		}
	}

	void SoundGraph::TriggerGraphEvent(const std::string& eventName, f32 value)
	{
		// Add event to queue for processing in audio thread
		GraphEvent event;
		event.frameIndex = m_CurrentFrame;
		event.endpointID = Identifier(eventName);
		event.value = choc::value::createFloat32(value);
		event.message = eventName;
		m_OutgoingEvents.push(event);

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
		// Set up event endpoints
		AddInEvent(IDs::Play, [this](f32 value) { OnPlay(value); });
	}

	void SoundGraph::ProcessEvents()
	{
		// Process any internal events
	}

	void SoundGraph::ProcessConnections()
	{
		// Process all connections in the graph
	}

	void SoundGraph::OnPlay(f32 value)
	{
		(void)value;
		m_IsPlaying = true;
		OLO_CORE_TRACE("[SoundGraph] Started playing sound graph");
	}

	void SoundGraph::OnStop(f32 value)
	{
		(void)value;
		m_IsPlaying = false;
		m_HasFinished = false;
		OLO_CORE_TRACE("[SoundGraph] Stopped sound graph");
	}

	void SoundGraph::OnFinished(f32 value)
	{
		(void)value;
		m_HasFinished = true;
		m_IsPlaying = false;
		OLO_CORE_TRACE("[SoundGraph] Sound graph finished");
	}

	void SoundGraph::UpdateFromAssetData([[maybe_unused]] const SoundGraphAsset& asset)
	{
		// Clear existing state
		m_IsPlaying = false;
		m_HasFinished = false;
		OLO_CORE_INFO("Updated sound graph from asset data");
	}

} // namespace OloEngine::Audio::SoundGraph

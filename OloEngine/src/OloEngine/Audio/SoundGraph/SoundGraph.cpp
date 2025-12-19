#include "OloEnginePCH.h"
#include "SoundGraph.h"
#include "SoundGraphPrototype.h" // Must include before SoundGraphAsset.h to complete Prototype type
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

        // Queue event for processing in audio thread
        // All events are handled consistently through the event queue to avoid race conditions
        Audio::AudioThreadEvent event;
        event.m_FrameIndex = m_CurrentFrame;
        event.m_EndpointID = static_cast<u32>(Identifier(eventName));

        choc::value::Value valueData = choc::value::createFloat32(value);
        event.m_ValueData.CopyFrom(valueData);

        m_OutgoingEvents.Push(event);

        // Note: Events are processed by the audio thread through InitializeEndpoints callbacks.
        // We do NOT call Play()/Stop() directly here to avoid race conditions on m_IsPlaying.
    }

    void SoundGraph::InitializeEndpoints()
    {
        OLO_PROFILE_FUNCTION();

        // Set up event endpoints
        AddInEvent(IDs::Play, [this](f32 value)
                   { OnPlay(value); });
        AddInEvent(IDs::Stop, [this](f32 value)
                   { OnStop(value); });
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

    bool SoundGraph::AddValueConnection(UUID sourceNodeID, const std::string& sourceEndpoint,
                                        UUID targetNodeID, const std::string& targetEndpoint)
    {
        return AddValueConnection(sourceNodeID, Identifier(sourceEndpoint),
                                  targetNodeID, Identifier(targetEndpoint));
    }

    bool SoundGraph::AddEventConnection(UUID sourceNodeID, const std::string& sourceEndpoint,
                                        UUID targetNodeID, const std::string& targetEndpoint)
    {
        return AddEventConnection(sourceNodeID, Identifier(sourceEndpoint),
                                  targetNodeID, Identifier(targetEndpoint));
    }

} // namespace OloEngine::Audio::SoundGraph

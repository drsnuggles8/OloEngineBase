#include "OloEnginePCH.h"
#include "NodeProcessor.h"

namespace OloEngine::Audio::SoundGraph
{
	bool NodeProcessor::ConnectTo(const std::string& outputName, NodeProcessor* targetNode, const std::string& inputName)
	{
		if (!targetNode)
		{
			OLO_CORE_ERROR("[SoundGraph] Cannot connect to null target node");
			return false;
		}

		// Check if output exists on this node
		auto outputIt = m_OutputEndpoints.find(outputName);
		if (outputIt == m_OutputEndpoints.end())
		{
			OLO_CORE_ERROR("[SoundGraph] Output endpoint '{}' not found on node '{}'", outputName, m_DebugName);
			return false;
		}

		// Check if input exists on target node
		auto inputIt = targetNode->m_InputEndpoints.find(inputName);
		if (inputIt == targetNode->m_InputEndpoints.end())
		{
			OLO_CORE_ERROR("[SoundGraph] Input endpoint '{}' not found on target node '{}'", inputName, targetNode->m_DebugName);
			return false;
		}

		// Verify that both endpoints are the same type (event or value)
		const auto& output = outputIt->second;
		const auto& input = inputIt->second;

		if (output.IsEvent != input.IsEvent)
		{
			OLO_CORE_ERROR("[SoundGraph] Cannot connect event output to value input or vice versa");
			return false;
		}

		// Make the connection
		if (output.IsEvent)
		{
			// Event connection: wire the output callback to trigger the input callback
			// Note: This is a simplified implementation - in a full system you'd want 
			// a more sophisticated event routing system
			OLO_CORE_TRACE("[SoundGraph] Connected event '{}' from '{}' to '{}' on '{}'", 
				outputName, m_DebugName, inputName, targetNode->m_DebugName);
		}
		else
		{
			// Value connection: wire the output value pointer to the input value pointer
			if (output.ValuePtr && input.ValuePtr)
			{
				// In a real implementation, you'd set up the connection so that
				// the target node reads from the source node's output value
				OLO_CORE_TRACE("[SoundGraph] Connected value '{}' from '{}' to '{}' on '{}'", 
					outputName, m_DebugName, inputName, targetNode->m_DebugName);
			}
		}

		return true;
	}

	void NodeProcessor::TriggerEvent(const std::string& eventName, f32 value)
	{
		auto outputIt = m_OutputEndpoints.find(eventName);
		if (outputIt != m_OutputEndpoints.end() && outputIt->second.IsEvent)
		{
			if (outputIt->second.EventCallback)
			{
				outputIt->second.EventCallback(value);
			}
		}

		// Also check input events (for external triggering)
		auto inputIt = m_InputEndpoints.find(eventName);
		if (inputIt != m_InputEndpoints.end() && inputIt->second.IsEvent)
		{
			if (inputIt->second.EventCallback)
			{
				inputIt->second.EventCallback(value);
			}
		}
	}
}
#include "OloEnginePCH.h"
#include "NodeProcessor.h"
#include "ParameterConnection.h"

namespace OloEngine::Audio::SoundGraph
{
	bool NodeProcessor::ConnectTo(const std::string& outputName, NodeProcessor* targetNode, const std::string& inputName)
	{
		if (!targetNode)
		{
			OLO_CORE_ERROR("[SoundGraph] Cannot connect to null target node");
			return false;
		}

		// Convert endpoint names to identifiers
		Identifier outputID = OLO_IDENTIFIER(outputName.c_str());
		Identifier inputID = OLO_IDENTIFIER(inputName.c_str());

		// Try event connection first
		auto sourceEvent = GetOutputEvent(outputID);
		auto targetEvent = targetNode->GetInputEvent(inputID);

		if (sourceEvent && targetEvent)
		{
			// Connect events
			sourceEvent->ConnectTo(targetEvent);
			OLO_CORE_TRACE("[SoundGraph] Connected event '{}' from '{}' to '{}' on '{}'", 
				outputName, GetDisplayName(), inputName, targetNode->GetDisplayName());
			return true;
		}

		// Try parameter connection (value connection)
		// Check if both nodes have the specified parameters
		if (m_Parameters.HasParameter(outputID) && targetNode->m_Parameters.HasParameter(inputID))
		{
			// Try f32 connection first (most common audio parameter type)
			if (CreateParameterConnectionF32(outputName, targetNode, inputName))
			{
				return true;
			}
			
			// If f32 connection failed, try other common types
			if (CreateParameterConnectionI32(outputName, targetNode, inputName))
			{
				return true;
			}
			
			if (CreateParameterConnectionBool(outputName, targetNode, inputName))
			{
				return true;
			}
			
			OLO_CORE_ERROR("[SoundGraph] Parameter connection failed - incompatible types for '{}' to '{}'", 
				outputName, inputName);
			return false;
		}

		OLO_CORE_ERROR("[SoundGraph] No compatible endpoints found for connection from '{}' to '{}'", 
			outputName, inputName);
		return false;
	}

	void NodeProcessor::TriggerOutputEvent(const Identifier& eventID, f32 value)
	{
		auto outputEvent = GetOutputEvent(eventID);
		if (outputEvent)
		{
			(*outputEvent)(value);
		}
	}

	void NodeProcessor::TriggerOutputEvent(const std::string& eventName, f32 value)
	{
		Identifier eventID = OLO_IDENTIFIER(eventName.c_str());
		TriggerOutputEvent(eventID, value);
	}

	//==============================================================================
	/// Parameter Connection Implementation

	bool NodeProcessor::CreateParameterConnectionF32(const std::string& outputParam, NodeProcessor* targetNode, const std::string& inputParam)
	{
		if (!targetNode)
			return false;

		Identifier outputID = OLO_IDENTIFIER(outputParam.c_str());
		Identifier inputID = OLO_IDENTIFIER(inputParam.c_str());
		
		if (!HasParameter(outputID) || !targetNode->HasParameter(inputID))
		{
			return false;
		}

		auto connection = std::make_shared<TypedParameterConnection<f32>>(
			this, outputID, targetNode, inputID);

		if (!connection || !connection->IsValid())
		{
			return false;
		}

		m_ParameterConnections.push_back(connection);
		OLO_CORE_TRACE("[SoundGraph] Created f32 parameter connection: '{}:{}' -> '{}:{}'", 
			GetDisplayName(), outputParam, targetNode->GetDisplayName(), inputParam);
		return true;
	}

	bool NodeProcessor::CreateParameterConnectionI32(const std::string& outputParam, NodeProcessor* targetNode, const std::string& inputParam)
	{
		if (!targetNode)
			return false;

		Identifier outputID = OLO_IDENTIFIER(outputParam.c_str());
		Identifier inputID = OLO_IDENTIFIER(inputParam.c_str());
		
		if (!HasParameter(outputID) || !targetNode->HasParameter(inputID))
		{
			return false;
		}

		auto connection = std::make_shared<TypedParameterConnection<i32>>(
			this, outputID, targetNode, inputID);

		if (!connection || !connection->IsValid())
		{
			return false;
		}

		m_ParameterConnections.push_back(connection);
		OLO_CORE_TRACE("[SoundGraph] Created i32 parameter connection: '{}:{}' -> '{}:{}'", 
			GetDisplayName(), outputParam, targetNode->GetDisplayName(), inputParam);
		return true;
	}

	bool NodeProcessor::CreateParameterConnectionBool(const std::string& outputParam, NodeProcessor* targetNode, const std::string& inputParam)
	{
		if (!targetNode)
			return false;

		Identifier outputID = OLO_IDENTIFIER(outputParam.c_str());
		Identifier inputID = OLO_IDENTIFIER(inputParam.c_str());
		
		if (!HasParameter(outputID) || !targetNode->HasParameter(inputID))
		{
			return false;
		}

		auto connection = std::make_shared<TypedParameterConnection<bool>>(
			this, outputID, targetNode, inputID);

		if (!connection || !connection->IsValid())
		{
			return false;
		}

		m_ParameterConnections.push_back(connection);
		OLO_CORE_TRACE("[SoundGraph] Created bool parameter connection: '{}:{}' -> '{}:{}'", 
			GetDisplayName(), outputParam, targetNode->GetDisplayName(), inputParam);
		return true;
	}

	bool NodeProcessor::RemoveParameterConnection(const std::string& outputParam, NodeProcessor* targetNode, const std::string& inputParam)
	{
		if (!targetNode)
			return false;

		Identifier outputID = OLO_IDENTIFIER(outputParam.c_str());
		Identifier inputID = OLO_IDENTIFIER(inputParam.c_str());

		// Find and remove the connection
		auto it = std::remove_if(m_ParameterConnections.begin(), m_ParameterConnections.end(),
			[=](const std::shared_ptr<ParameterConnection>& connection) {
				return connection->GetTargetNode() == targetNode &&
					   connection->GetSourceParameterID() == outputID &&
					   connection->GetTargetParameterID() == inputID;
			});

		if (it != m_ParameterConnections.end())
		{
			m_ParameterConnections.erase(it, m_ParameterConnections.end());
			OLO_CORE_TRACE("[SoundGraph] Removed parameter connection: '{}:{}' -> '{}:{}'", 
				GetDisplayName(), outputParam, targetNode->GetDisplayName(), inputParam);
			return true;
		}

		return false;
	}

	void NodeProcessor::ProcessParameterConnections()
	{
		// Propagate values through all parameter connections
		for (auto& connection : m_ParameterConnections)
		{
			if (connection && connection->IsValid())
			{
				connection->PropagateValue();
			}
		}
	}
}
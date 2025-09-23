#include "OloEnginePCH.h"
#include "ParameterConnection.h"
#include "NodeProcessor.h"

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// ParameterConnection Implementation

	ParameterConnection::ParameterConnection(NodeProcessor* sourceNode, const Identifier& sourceParam,
											 NodeProcessor* targetNode, const Identifier& targetParam)
		: m_SourceNode(sourceNode)
		, m_TargetNode(targetNode)
		, m_SourceParameterID(sourceParam)
		, m_TargetParameterID(targetParam)
	{
	}

	bool ParameterConnection::IsValid() const
	{
		// Check that both nodes exist
		if (!m_SourceNode || !m_TargetNode)
		{
			return false;
		}

		// Check that both parameters exist
		if (!m_SourceNode->HasParameter(m_SourceParameterID) || 
			!m_TargetNode->HasParameter(m_TargetParameterID))
		{
			return false;
		}

		// Cannot connect a node to itself (would create immediate feedback)
		if (m_SourceNode == m_TargetNode)
		{
			return false;
		}

		return true;
	}
}
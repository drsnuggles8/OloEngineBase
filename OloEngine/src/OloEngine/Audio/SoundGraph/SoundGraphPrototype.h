#pragma once

#include "OloEngine/Core/UUID.h"
#include "OloEngine/Core/Identifier.h"
#include "OloEngine/Core/Ref.h"
#include <choc/containers/choc_Value.h>

#include <vector>
#include <string>
#include <utility>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// SoundGraph Prototype, used to construct instances of SoundGraph for playback
	struct Prototype : public RefCounted
	{
		std::string DebugName;
		UUID ID;

		struct Endpoint
		{
			Identifier EndpointID;
			choc::value::Value DefaultValue;

			Endpoint(Identifier id, const choc::value::Value& defaultValue)
				: EndpointID(id), DefaultValue(defaultValue) {}

			Endpoint()
				: EndpointID(0), DefaultValue() {}
		};

		//==============================================================================
		/// Graph IO
		std::vector<Endpoint> Inputs, Outputs;

		std::vector<Endpoint> LocalVariablePlugs;

		//==============================================================================
		/// Nodes
		struct Node
		{
			Identifier NodeTypeID; // Used to call Factory to create the right node type
			UUID ID;
			std::vector<Endpoint> DefaultValuePlugs;

			Node(Identifier typeID, UUID uniqueID) : NodeTypeID(typeID), ID(uniqueID) {}
			Node() : NodeTypeID(0), ID(0) {}
		};

		std::vector<Node> Nodes;

		//==============================================================================
		/// Connections
		struct Connection
		{
			enum EType
			{
				NodeValue_NodeValue = 0,
				NodeEvent_NodeEvent = 1,
				GraphValue_NodeValue = 2,
				GraphEvent_NodeEvent = 3,
				NodeValue_GraphValue = 4,
				NodeEvent_GraphEvent = 5,
				LocalVariable_NodeValue = 6,
			};

			struct EndpointRef
			{
				UUID NodeID;
				Identifier EndpointID;
			};

			EndpointRef Source, Destination;
			EType Type;

			Connection(EndpointRef&& source, EndpointRef&& destination, EType connectionType)
				: Source(std::move(source)), Destination(std::move(destination)), Type(connectionType)
			{}

			Connection()
				: Source{ UUID(), Identifier(0) }, Destination{ UUID(), Identifier(0) }, Type(NodeValue_NodeValue) {}
		};

		// Used to create a copy of the graph
		std::vector<Connection> Connections;
	};

} // namespace OloEngine::Audio::SoundGraph
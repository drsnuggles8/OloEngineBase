#include "OloEnginePCH.h"
#include "SoundGraphPrototype.h"
#include "OloEngine/Serialization/StreamWriter.h"
#include "OloEngine/Serialization/StreamReader.h"

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	// ValueSerializer Helper for choc::value serialization
	struct ValueSerializer
	{
		std::vector<u8> Data;
		
		void write(const void* data, sizet size)
		{
			const u8* byteData = static_cast<const u8*>(data);
			Data.insert(Data.end(), byteData, byteData + size);
		}
	};

	//==============================================================================
	// Prototype::Endpoint Serialization
	
	void Prototype::Endpoint::Serialize(StreamWriter* writer, const Endpoint& endpoint)
	{
		// Write the endpoint ID as u32
		writer->WriteRaw(static_cast<u32>(endpoint.EndpointID));

		// Serialize the choc::value::Value using choc's built-in serialization
		ValueSerializer wrapper;
		endpoint.DefaultValue.serialise(wrapper);
		writer->WriteArray(wrapper.Data);
	}

	void Prototype::Endpoint::Deserialize(StreamReader* reader, Endpoint& endpoint)
	{
		// Read the endpoint ID
		u32 id;
		reader->ReadRaw(id);
		endpoint.EndpointID = Identifier(id);

		// Deserialize the choc::value::Value
		std::vector<u8> data;
		reader->ReadArray(data);
		choc::value::InputData inputData{ data.data(), data.data() + data.size() };
		endpoint.DefaultValue = choc::value::Value::deserialise(inputData);
	}

	//==============================================================================
	// Prototype::Node Serialization
	
	void Prototype::Node::Serialize(StreamWriter* writer, const Node& node)
	{
		// Write node type ID
		writer->WriteRaw(static_cast<u32>(node.NodeTypeID));
		
		// Write node UUID
		writer->WriteRaw(static_cast<u64>(node.ID));
		
		// Write default value plugs array
		writer->WriteArray(node.DefaultValuePlugs);
	}

	void Prototype::Node::Deserialize(StreamReader* reader, Node& node)
	{
		// Read node type ID
		u32 typeID;
		reader->ReadRaw(typeID);
		node.NodeTypeID = Identifier(typeID);
		
		// Read node UUID
		u64 id;
		reader->ReadRaw(id);
		node.ID = UUID(id);
		
		// Read default value plugs array
		reader->ReadArray(node.DefaultValuePlugs);
	}

	//==============================================================================
	// Prototype::Connection Serialization
	
	void Prototype::Connection::Serialize(StreamWriter* writer, const Connection& connection)
	{
		// Write source endpoint
		writer->WriteRaw(static_cast<u64>(connection.Source.NodeID));
		writer->WriteRaw(static_cast<u32>(connection.Source.EndpointID));
		
		// Write destination endpoint
		writer->WriteRaw(static_cast<u64>(connection.Destination.NodeID));
		writer->WriteRaw(static_cast<u32>(connection.Destination.EndpointID));
		
		// Write connection type
		writer->WriteRaw(static_cast<u32>(connection.Type));
	}

	void Prototype::Connection::Deserialize(StreamReader* reader, Connection& connection)
	{
		// Read source endpoint
		u64 sourceNodeID;
		u32 sourceEndpointID;
		reader->ReadRaw(sourceNodeID);
		reader->ReadRaw(sourceEndpointID);
		connection.Source.NodeID = UUID(sourceNodeID);
		connection.Source.EndpointID = Identifier(sourceEndpointID);
		
		// Read destination endpoint
		u64 destNodeID;
		u32 destEndpointID;
		reader->ReadRaw(destNodeID);
		reader->ReadRaw(destEndpointID);
		connection.Destination.NodeID = UUID(destNodeID);
		connection.Destination.EndpointID = Identifier(destEndpointID);
		
		// Read connection type
		u32 type;
		reader->ReadRaw(type);
		connection.Type = static_cast<Connection::EType>(type);
	}

	//==============================================================================
	// Prototype Serialization
	
	void Prototype::Serialize(StreamWriter* writer, const Prototype& prototype)
	{
		// Write debug name
		writer->WriteString(prototype.DebugName);
		
		// Write prototype ID
		writer->WriteRaw(static_cast<u64>(prototype.ID));
		
		// Write inputs array
		writer->WriteArray(prototype.Inputs);
		
		// Write outputs array
		writer->WriteArray(prototype.Outputs);
		
		// Write local variable plugs array
		writer->WriteArray(prototype.LocalVariablePlugs);
		
		// Write nodes array
		writer->WriteArray(prototype.Nodes);
		
		// Write connections array
		writer->WriteArray(prototype.Connections);
	}

	void Prototype::Deserialize(StreamReader* reader, Prototype& prototype)
	{
		// Read debug name
		reader->ReadString(prototype.DebugName);
		
		// Read prototype ID
		u64 id;
		reader->ReadRaw(id);
		prototype.ID = UUID(id);
		
		// Read inputs array
		reader->ReadArray(prototype.Inputs);
		
		// Read outputs array
		reader->ReadArray(prototype.Outputs);
		
		// Read local variable plugs array
		reader->ReadArray(prototype.LocalVariablePlugs);
		
		// Read nodes array
		reader->ReadArray(prototype.Nodes);
		
		// Read connections array
		reader->ReadArray(prototype.Connections);
	}

} // namespace OloEngine::Audio::SoundGraph

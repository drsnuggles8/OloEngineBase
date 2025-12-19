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
        std::vector<u8> m_Data;

        void Write(const void* data, sizet size)
        {
            OLO_PROFILE_FUNCTION();
            const u8* byteData = static_cast<const u8*>(data);
            m_Data.insert(m_Data.end(), byteData, byteData + size);
        }

        // Adapter for choc library - delegates to Write()
        void write(const void* data, sizet size)
        {
            Write(data, size);
        }
    };

    //==============================================================================
    // Prototype::Endpoint Serialization

    void Prototype::Endpoint::Serialize(StreamWriter* writer, const Endpoint& endpoint)
    {
        OLO_PROFILE_FUNCTION();

        // Write the endpoint ID as u32
        writer->WriteRaw(static_cast<u32>(endpoint.m_EndpointID));

        // Serialize the choc::value::Value using choc's built-in serialization
        ValueSerializer wrapper;
        endpoint.m_DefaultValue.serialise(wrapper);
        writer->WriteArray(wrapper.m_Data);
    }

    void Prototype::Endpoint::Deserialize(StreamReader* reader, Endpoint& endpoint)
    {
        OLO_PROFILE_FUNCTION();

        // Read the endpoint ID
        u32 id;
        reader->ReadRaw(id);
        endpoint.m_EndpointID = Identifier(id);

        // Deserialize the choc::value::Value
        std::vector<u8> data;
        reader->ReadArray(data);

        // Validate input buffer before deserialization
        if (data.empty())
        {
            OLO_CORE_ERROR("[SoundGraphPrototype] Empty data buffer for endpoint ID {0} - using default void value", id);
            endpoint.m_DefaultValue = choc::value::Value();
            return;
        }

        try
        {
            choc::value::InputData inputData{ data.data(), data.data() + data.size() };
            endpoint.m_DefaultValue = choc::value::Value::deserialise(inputData);
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("[SoundGraphPrototype] Failed to deserialize endpoint ID {0} (data size: {1}): {2} - using default void value",
                           id, data.size(), e.what());
            endpoint.m_DefaultValue = choc::value::Value();
        }
        catch (...)
        {
            OLO_CORE_ERROR("[SoundGraphPrototype] Unknown error deserializing endpoint ID {0} (data size: {1}) - using default void value",
                           id, data.size());
            endpoint.m_DefaultValue = choc::value::Value();
        }
    }

    //==============================================================================
    // Prototype::Node Serialization

    void Prototype::Node::Serialize(StreamWriter* writer, const Node& node)
    {
        OLO_PROFILE_FUNCTION();

        // Write node type ID
        writer->WriteRaw(static_cast<u32>(node.m_NodeTypeID));

        // Write node UUID
        writer->WriteRaw(static_cast<u64>(node.m_ID));

        // Write default value plugs array
        writer->WriteArray(node.m_DefaultValuePlugs);
    }

    void Prototype::Node::Deserialize(StreamReader* reader, Node& node)
    {
        OLO_PROFILE_FUNCTION();

        // Read node type ID
        u32 typeID;
        reader->ReadRaw(typeID);
        node.m_NodeTypeID = Identifier(typeID);

        // Read node UUID
        u64 id;
        reader->ReadRaw(id);
        node.m_ID = UUID(id);

        // Read default value plugs array
        reader->ReadArray(node.m_DefaultValuePlugs);
    }

    //==============================================================================
    // Prototype::Connection Serialization

    void Prototype::Connection::Serialize(StreamWriter* writer, const Connection& connection)
    {
        OLO_PROFILE_FUNCTION();

        // Write source endpoint
        writer->WriteRaw(static_cast<u64>(connection.m_Source.m_NodeID));
        writer->WriteRaw(static_cast<u32>(connection.m_Source.m_EndpointID));

        // Write destination endpoint
        writer->WriteRaw(static_cast<u64>(connection.m_Destination.m_NodeID));
        writer->WriteRaw(static_cast<u32>(connection.m_Destination.m_EndpointID));

        // Write connection type
        writer->WriteRaw(static_cast<u32>(connection.m_Type));
    }

    void Prototype::Connection::Deserialize(StreamReader* reader, Connection& connection)
    {
        OLO_PROFILE_FUNCTION();

        // Read source endpoint
        u64 sourceNodeID;
        u32 sourceEndpointID;
        reader->ReadRaw(sourceNodeID);
        reader->ReadRaw(sourceEndpointID);
        connection.m_Source.m_NodeID = UUID(sourceNodeID);
        connection.m_Source.m_EndpointID = Identifier(sourceEndpointID);

        // Read destination endpoint
        u64 destNodeID;
        u32 destEndpointID;
        reader->ReadRaw(destNodeID);
        reader->ReadRaw(destEndpointID);
        connection.m_Destination.m_NodeID = UUID(destNodeID);
        connection.m_Destination.m_EndpointID = Identifier(destEndpointID);

        // Read connection type
        u32 type;
        reader->ReadRaw(type);

        // Validate the connection type before casting
        // Valid range: NodeValue_NodeValue (0) to LocalVariable_NodeValue (6)
        constexpr u32 minValidType = static_cast<u32>(Connection::EType::NodeValue_NodeValue);
        constexpr u32 maxValidType = static_cast<u32>(Connection::EType::LocalVariable_NodeValue);

        if (type < minValidType || type > maxValidType)
        {
            OLO_CORE_ERROR("[SoundGraphPrototype] Invalid connection type {0} (valid range: {1}-{2}) for connection from node {3} to node {4} - defaulting to NodeValue_NodeValue",
                           type, minValidType, maxValidType, sourceNodeID, destNodeID);
            connection.m_Type = Connection::EType::NodeValue_NodeValue;
        }
        else
        {
            connection.m_Type = static_cast<Connection::EType>(type);
        }
    }

    //==============================================================================
    // Prototype Serialization

    void Prototype::Serialize(StreamWriter* writer, const Prototype& prototype)
    {
        OLO_PROFILE_FUNCTION();

        // Write debug name
        writer->WriteString(prototype.m_DebugName);

        // Write prototype ID
        writer->WriteRaw(static_cast<u64>(prototype.m_ID));

        // Write inputs array
        writer->WriteArray(prototype.m_Inputs);

        // Write outputs array
        writer->WriteArray(prototype.m_Outputs);

        // Write local variable plugs array
        writer->WriteArray(prototype.m_LocalVariablePlugs);

        // Write nodes array
        writer->WriteArray(prototype.m_Nodes);

        // Write connections array
        writer->WriteArray(prototype.m_Connections);
    }

    void Prototype::Deserialize(StreamReader* reader, Prototype& prototype)
    {
        OLO_PROFILE_FUNCTION();

        // Read debug name
        reader->ReadString(prototype.m_DebugName);

        // Read prototype ID
        u64 id;
        reader->ReadRaw(id);
        prototype.m_ID = UUID(id);

        // Read inputs array
        reader->ReadArray(prototype.m_Inputs);

        // Read outputs array
        reader->ReadArray(prototype.m_Outputs);

        // Read local variable plugs array
        reader->ReadArray(prototype.m_LocalVariablePlugs);

        // Read nodes array
        reader->ReadArray(prototype.m_Nodes);

        // Read connections array
        reader->ReadArray(prototype.m_Connections);
    }

} // namespace OloEngine::Audio::SoundGraph

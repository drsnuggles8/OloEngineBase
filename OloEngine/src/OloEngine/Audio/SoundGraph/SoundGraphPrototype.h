#pragma once

#include "OloEngine/Core/UUID.h"
#include "OloEngine/Core/Identifier.h"
#include "OloEngine/Core/Ref.h"
#include <choc/containers/choc_Value.h>

#include <vector>
#include <string>
#include <utility>

namespace OloEngine
{
    class StreamWriter;
    class StreamReader;
} // namespace OloEngine

namespace OloEngine::Audio::SoundGraph
{
    //==============================================================================
    /// SoundGraph Prototype, used to construct instances of SoundGraph for playback
    struct Prototype : public RefCounted
    {
        std::string m_DebugName;
        UUID m_ID;

        struct Endpoint
        {
            Identifier m_EndpointID;
            choc::value::Value m_DefaultValue;

            Endpoint(Identifier id, const choc::value::Value& defaultValue)
                : m_EndpointID(id), m_DefaultValue(defaultValue) {}

            Endpoint()
                : m_EndpointID(0), m_DefaultValue() {}

            static void Serialize(OloEngine::StreamWriter* writer, const Endpoint& endpoint);
            static void Deserialize(OloEngine::StreamReader* reader, Endpoint& endpoint);
        };

        //==============================================================================
        /// Graph IO
        std::vector<Endpoint> m_Inputs;
        std::vector<Endpoint> m_Outputs;
        std::vector<Endpoint> m_LocalVariablePlugs;

        //==============================================================================
        /// Nodes
        struct Node
        {
            Identifier m_NodeTypeID; // Used to call Factory to create the right node type
            UUID m_ID;
            std::vector<Endpoint> m_DefaultValuePlugs;

            Node(Identifier typeID, UUID uniqueID) : m_NodeTypeID(typeID), m_ID(uniqueID) {}
            Node() : m_NodeTypeID(0), m_ID(0) {}

            static void Serialize(OloEngine::StreamWriter* writer, const Node& node);
            static void Deserialize(OloEngine::StreamReader* reader, Node& node);
        };

        std::vector<Node> m_Nodes;

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
                UUID m_NodeID;
                Identifier m_EndpointID;
            };

            EndpointRef m_Source;
            EndpointRef m_Destination;
            EType m_Type;

            Connection(EndpointRef source, EndpointRef destination, EType connectionType)
                : m_Source(std::move(source)), m_Destination(std::move(destination)), m_Type(connectionType) {}

            Connection()
                : m_Source{ UUID(0), Identifier(0) }, m_Destination{ UUID(0), Identifier(0) }, m_Type(NodeValue_NodeValue) {}

            static void Serialize(OloEngine::StreamWriter* writer, const Connection& connection);
            static void Deserialize(OloEngine::StreamReader* reader, Connection& connection);
        };

        // Used to create a copy of the graph
        std::vector<Connection> m_Connections;

        static void Serialize(OloEngine::StreamWriter* writer, const Prototype& prototype);
        static void Deserialize(OloEngine::StreamReader* reader, Prototype& prototype);
    };

} // namespace OloEngine::Audio::SoundGraph

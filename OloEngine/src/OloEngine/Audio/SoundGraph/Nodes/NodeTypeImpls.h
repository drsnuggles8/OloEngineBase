#pragma once

#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"
#include "NodeTypes.h"

namespace OloEngine::Audio::SoundGraph 
{
    //==============================================================================
    // Template implementations for nodes - following Hazel's NodeTypeImpls.h pattern
    //==============================================================================

    /// @brief Macro to generate template implementations for node types
    /// @param NodeType The node type class name (e.g., Add, Multiply)
    /// Generates:
    /// - Constructor: NodeType<T>::NodeType(const char* dbgName, UUID id)
    /// - Init method: void NodeType<T>::Init()
    #define IMPLEMENT_NODE_TYPE(NodeType)                                          \
        template<typename T>                                                       \
        NodeType<T>::NodeType(const char* dbgName, UUID id)                        \
            : NodeProcessor(dbgName, id)                                           \
        {                                                                          \
            EndpointUtilities::RegisterEndpoints(this);                            \
        }                                                                          \
                                                                                   \
        template<typename T>                                                       \
        void NodeType<T>::Init()                                                   \
        {                                                                          \
            EndpointUtilities::InitializeInputs(this);                             \
        }

    //==============================================================================
    // Math Nodes Template Implementations
    //==============================================================================
    IMPLEMENT_NODE_TYPE(Add)
    IMPLEMENT_NODE_TYPE(Subtract)
    IMPLEMENT_NODE_TYPE(Multiply)
    IMPLEMENT_NODE_TYPE(Divide)
    IMPLEMENT_NODE_TYPE(Min)
    IMPLEMENT_NODE_TYPE(Max)
    IMPLEMENT_NODE_TYPE(Clamp)
    IMPLEMENT_NODE_TYPE(MapRange)
    IMPLEMENT_NODE_TYPE(Power)
    IMPLEMENT_NODE_TYPE(Abs)

    #undef IMPLEMENT_NODE_TYPE

} // namespace OloEngine::Audio::SoundGraph
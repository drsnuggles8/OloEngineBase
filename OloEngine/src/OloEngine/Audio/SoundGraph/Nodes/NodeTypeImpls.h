#pragma once

#include "OloEngine/Audio/SoundGraph/NodeDescriptors.h"
#include "NodeTypes.h"

namespace OloEngine::Audio::SoundGraph 
{
    //==============================================================================
    // Template implementations for nodes - following Hazel's NodeTypeImpls.h pattern
    //==============================================================================

    //==============================================================================
    // Macro to reduce boilerplate for standard node template implementations
    // Generates both constructor and Init() method for a given node type
    //==============================================================================
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
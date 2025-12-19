#pragma once

#include "NodeProcessor.h"
#include "OloEngine/Core/Reflection/Reflection.h"
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

namespace OloEngine::Audio::SoundGraph
{

    //==============================================================================
    /// Tags for distinguishing input/output descriptions
    struct TagInputs
    {
    };
    struct TagOutputs
    {
    };

    //==============================================================================
    /// Node description template (specialized by macros)
    template<typename NodeType>
    struct NodeDescription;

    //==============================================================================
    /// Check if a node has description specializations
    /// Detects NodeDescription specializations by checking for Inputs/Outputs nested types
    template<typename T, typename = void>
    struct IsDescribedNode : std::false_type
    {
    };

    template<typename T>
    struct IsDescribedNode<T, std::void_t<
                                  typename NodeDescription<std::remove_cvref_t<T>>::Inputs,
                                  typename NodeDescription<std::remove_cvref_t<T>>::Outputs>> : std::true_type
    {
    };

    template<typename T>
    constexpr bool IsDescribedNode_v = IsDescribedNode<T>::value;

    //==============================================================================
    /// Endpoint utilities for automatic registration
    namespace EndpointUtilities
    {

        namespace Impl
        {

            /// Register input endpoints from member descriptions
            template<typename TNodeType>
            bool RegisterEndpointInputs(TNodeType* node)
            {
                OLO_PROFILE_FUNCTION();

                if constexpr (IsDescribedNode_v<TNodeType>)
                {
                    using InputsDescription = typename NodeDescription<std::remove_cvref_t<TNodeType>>::Inputs;

                    return InputsDescription::MemberListType::ApplyToStaticType([node](const auto&... members)
                                                                                {
                        sizet memberIndex = 0;
                        auto registerInput = [node, &memberIndex](auto memberPtr)
                        {
                            using TMember = std::remove_reference_t<decltype(memberPtr)>;
                            constexpr bool isInputEvent = std::is_member_function_pointer_v<TMember>;

                            const std::string_view memberName = InputsDescription::s_MemberNames[memberIndex++];
                            const std::string cleanName = std::string(Core::Reflection::StringUtils::RemovePrefixAndSuffix(memberName));
                            if constexpr (isInputEvent)
                            {
                                // Handle input events (member functions)
                                // Create an InputEvent and bind it to the member function
                                using FunctionType = TMember;

                                // Create InputEvent bound to the member function
                                auto inputEvent = std::make_shared<NodeProcessor::InputEvent>(*node,
                                    [node, memberPtr](float value) {
                                        // Call the member function with the event value using std::invoke
                                        if constexpr (std::is_invocable_v<FunctionType, decltype(*node)>)
                                        {
                                            // No-parameter event function
                                            std::invoke(memberPtr, node);
                                        }
                                        else if constexpr (std::is_invocable_v<FunctionType, decltype(*node), float>)
                                        {
                                            // Single float parameter event function
                                            std::invoke(memberPtr, node, value);
                                        }
                                        // TODO: Add support for other event parameter types as needed
                                        });

                                    // Register the input event with the node
                                    // InEvents stores shared_ptr<InputEvent> for shared ownership semantics
                                    node->InEvents[OloEngine::Identifier(cleanName.c_str())] = inputEvent;

                                    return true;
                            }
                            else
                            {
                                // Handle input parameters (member variables)
                                using ValueType = typename Core::Reflection::MemberPointer::ReturnType<TMember>::Type;

                                if constexpr (std::is_pointer_v<ValueType>)
                                {
                                    // Pointer members - these will be connected to input streams
                                    using UnderlyingType = std::remove_pointer_t<ValueType>;
                                    node->template AddParameter<UnderlyingType>(OloEngine::Identifier(cleanName.c_str()), cleanName, UnderlyingType{});
                                    return true;
                                }
                                else
                                {
                                    // Direct value members
                                    node->template AddParameter<ValueType>(OloEngine::Identifier(cleanName.c_str()), cleanName, node->*memberPtr);
                                    return true;
                                }
                            }
                            };

                            return (registerInput(members) && ...); });
                }
                else
                {
                    // Node is not described - cannot auto-register
                    return false;
                }
            }

            /// Register output endpoints from member descriptions
            template<typename TNodeType>
            bool RegisterEndpointOutputs(TNodeType* node)
            {
                OLO_PROFILE_FUNCTION();

                if constexpr (IsDescribedNode_v<TNodeType>)
                {
                    using OutputsDescription = typename NodeDescription<std::remove_cvref_t<TNodeType>>::Outputs;

                    return OutputsDescription::MemberListType::ApplyToStaticType([node](const auto&... members)
                                                                                 {
                        sizet memberIndex = 0;
                        auto registerOutput = [node, &memberIndex](auto memberPtr)
                        {
                            using TMember = typename Core::Reflection::MemberPointer::ReturnType<decltype(memberPtr)>::Type;
                            constexpr bool isOutputEvent = std::is_same_v<TMember, NodeProcessor::OutputEvent>;

                            const std::string_view memberName = OutputsDescription::s_MemberNames[memberIndex++];
                            const std::string cleanName = std::string(Core::Reflection::StringUtils::RemovePrefixAndSuffix(memberName));

                            if constexpr (isOutputEvent)
                            {
                                // Handle output events
                                // Output events are OutputEvent members that can trigger other events

                                // Get reference to the OutputEvent member
                                NodeProcessor::OutputEvent& outputEvent = node->*memberPtr;

                                // Register the output event with the node
                                // OutEvents stores reference_wrapper<OutputEvent> to avoid copying
                                node->OutEvents[OloEngine::Identifier(cleanName.c_str())] = std::ref(outputEvent);

                                return true;
                            }
                            else
                            {
                                // Handle output parameters (always direct values for outputs)
                                using ValueType = typename Core::Reflection::MemberPointer::ReturnType<decltype(memberPtr)>::Type;
                                // Output values are registered but not added as parameters since they're computed
                                // The processing function will set their values directly
                                return true;
                            }
                    };

                    return (registerOutput(members) && ...); });
                }
                else
                {
                    // Node is not described - cannot auto-register
                    return false;
                }
            }

            /// Initialize input pointers to connect with parameter system
            template<typename TNodeType>
            bool InitializeInputs(TNodeType* node)
            {
                OLO_PROFILE_FUNCTION();

                if constexpr (IsDescribedNode_v<TNodeType>)
                {
                    using InputsDescription = typename NodeDescription<std::remove_cvref_t<TNodeType>>::Inputs;

                    return InputsDescription::MemberListType::ApplyToStaticType([node](const auto&... members)
                                                                                {
                        auto initializeInput = [node, memberIndex = 0](auto memberPtr) mutable
                        {
                            using TMember = std::remove_reference_t<decltype(memberPtr)>;
                            constexpr bool isInputEvent = std::is_member_function_pointer_v<TMember>;

                            const std::string_view memberName = InputsDescription::s_MemberNames[memberIndex++];
                            const std::string cleanName = std::string(Core::Reflection::StringUtils::RemovePrefixAndSuffix(memberName));

                            if constexpr (!isInputEvent)
                            {
                                using ValueType = typename Core::Reflection::MemberPointer::ReturnType<TMember>::Type;

                                if constexpr (std::is_pointer_v<ValueType>)
                                {
                                    // Connect pointer members to parameter system
                                    using UnderlyingType = std::remove_pointer_t<ValueType>;
                                    auto param = node->template GetParameter<UnderlyingType>(OloEngine::Identifier(cleanName.c_str()));
                                    if (param)
                                    {
                                        node->*memberPtr = &param->m_Value;
                                    }
                                    else
                                    {
                                        // Clear pointer when parameter lookup fails to avoid dangling references
                                        node->*memberPtr = nullptr;
                                    }
                                }
                            }

                            return true;
                        };

                        return (initializeInput(members) && ...); });
                }
                else
                {
                    return false;
                }
            }
        } // namespace Impl

        /// Register all endpoints for a described node
        template<typename TNodeType>
        bool RegisterEndpoints(TNodeType* node)
        {
            // static_assert(IsDescribedNode_v<TNodeType>, "Node must have NodeDescription specialization");

            bool success = true;
            success &= Impl::RegisterEndpointInputs(node);
            success &= Impl::RegisterEndpointOutputs(node);

            return success;
        }

        /// Initialize input pointers for a described node
        template<typename TNodeType>
        bool InitializeInputs(TNodeType* node)
        {
            // static_assert(IsDescribedNode_v<TNodeType>, "Node must have NodeDescription specialization");

            return Impl::InitializeInputs(node);
        }
    } // namespace EndpointUtilities

} // namespace OloEngine::Audio::SoundGraph

//==============================================================================
/// CONVENIENCE MACROS FOR NODE DESCRIPTION

#ifndef NODE_INPUTS
#define NODE_INPUTS(...) __VA_ARGS__
#endif

#ifndef NODE_OUTPUTS
#define NODE_OUTPUTS(...) __VA_ARGS__
#endif

/**
 * Describe a sound graph node with its inputs and outputs
 * @param NodeType - The node class to describe
 * @param InputList - NODE_INPUTS(...) with member pointers to input parameters
 * @param OutputList - NODE_OUTPUTS(...) with member pointers to output parameters
 */
#ifndef DESCRIBE_NODE
#define DESCRIBE_NODE(NodeType, InputList, OutputList)                                                                \
    OLO_DESCRIBE_TAGGED(NodeType, OloEngine::Audio::SoundGraph::TagInputs, InputList)                                 \
    OLO_DESCRIBE_TAGGED(NodeType, OloEngine::Audio::SoundGraph::TagOutputs, OutputList)                               \
                                                                                                                      \
    template<>                                                                                                        \
    struct OloEngine::Audio::SoundGraph::NodeDescription<NodeType>                                                    \
    {                                                                                                                 \
        using Inputs = OloEngine::Core::Reflection::Description<NodeType, OloEngine::Audio::SoundGraph::TagInputs>;   \
        using Outputs = OloEngine::Core::Reflection::Description<NodeType, OloEngine::Audio::SoundGraph::TagOutputs>; \
    };
#endif // !DESCRIBE_NODE

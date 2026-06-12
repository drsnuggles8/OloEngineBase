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
    /// Detects ValueRef<T> members (any instantiation).
    template<typename T>
    struct IsValueRef : std::false_type
    {
    };

    template<StreamValue T>
    struct IsValueRef<ValueRef<T>> : std::true_type
    {
    };

    template<typename T>
    constexpr bool IsValueRef_v = IsValueRef<T>::value;

    //==============================================================================
    /// Endpoint utilities for automatic registration
    ///
    /// Phase 2: endpoint registration dispatches on the *declared member type* —
    /// that is where the typing of "typed connections" lives:
    ///   - AudioBufferRef member  -> audio-rate input endpoint
    ///   - ValueRef<T> member     -> control-rate input endpoint of type T
    ///   - member function        -> input event
    ///   - AudioBuffer member     -> audio-rate output endpoint
    ///   - arithmetic member      -> control-rate output endpoint
    ///   - OutputEvent member     -> output event
    /// Anything else (e.g. the Array nodes' `std::vector<T>*` plugs) is left
    /// unregistered, matching the pre-existing behavior; the node null-checks it.
    ///
    /// The old InitializeInputs step is gone: refs are born pointing at their
    /// inline default cells, and connections re-point them at producers, so there
    /// is no second "resolve parameters" pass (and nothing for Init to undo).
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
                                        else
                                        {
                                            // No additional handling required.
                                        }
                                        });

                                    // Register the input event with the node
                                    // InEvents stores shared_ptr<InputEvent> for shared ownership semantics
                                    node->InEvents[OloEngine::Identifier(cleanName.c_str())] = inputEvent;

                                    return true;
                            }
                            else
                            {
                                using ValueType = typename Core::Reflection::MemberPointer::ReturnType<TMember>::Type;

                                if constexpr (std::is_same_v<ValueType, AudioBufferRef>)
                                {
                                    node->AddAudioInRef(OloEngine::Identifier(cleanName.c_str()), node->*memberPtr);
                                }
                                else if constexpr (IsValueRef_v<ValueType>)
                                {
                                    node->AddValueInRef(OloEngine::Identifier(cleanName.c_str()), node->*memberPtr);
                                }
                                else
                                {
                                    // Non-stream member (e.g. `std::vector<T>* m_Array`) —
                                    // not expressible as a typed connection; stays
                                    // unregistered and the node is responsible for
                                    // null-checking, matching pre-existing behavior.
                                }
                                return true;
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

                                // Register the output event with the node. OutEvents stores
                                // reference_wrapper<OutputEvent> which has no default ctor, so
                                // `operator[]` won't compile — try_emplace constructs the
                                // value in place from the std::ref argument.
                                node->OutEvents.try_emplace(OloEngine::Identifier(cleanName.c_str()),
                                                            std::ref(outputEvent));

                                return true;
                            }
                            else if constexpr (std::is_same_v<TMember, AudioBuffer>)
                            {
                                node->AddAudioOutSource(OloEngine::Identifier(cleanName.c_str()), node->*memberPtr);
                                return true;
                            }
                            else if constexpr (StreamValue<TMember>)
                            {
                                node->AddValueOutSource(OloEngine::Identifier(cleanName.c_str()), node->*memberPtr);
                                return true;
                            }
                            else
                            {
                                static_assert(std::is_same_v<TMember, AudioBuffer> || StreamValue<TMember> || isOutputEvent,
                                              "Described node output must be AudioBuffer, a stream value type, or OutputEvent");
                                return false;
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
        } // namespace Impl

        /// Register all endpoints for a described node
        template<typename TNodeType>
        bool RegisterEndpoints(TNodeType* node)
        {
            bool success = true;
            success &= Impl::RegisterEndpointInputs(node);
            success &= Impl::RegisterEndpointOutputs(node);

            return success;
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

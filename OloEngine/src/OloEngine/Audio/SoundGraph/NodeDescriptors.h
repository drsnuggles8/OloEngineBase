#pragma once

#include "NodeProcessor.h"
#include "OloEngine/Core/Reflection/Reflection.h"
#include <functional>

namespace OloEngine::Audio::SoundGraph {

	//==============================================================================
	/// Tags for distinguishing input/output descriptions
	struct TagInputs {};
	struct TagOutputs {};

	//==============================================================================
	/// Node description template (specialized by macros)
	template<typename NodeType>
	struct NodeDescription;

	//==============================================================================
	/// Check if a node has description specializations
	/// Detects NodeDescription specializations by checking for Inputs/Outputs nested types
	template<typename T, typename = void>
	struct IsDescribedNode : std::false_type {};

	template<typename T>
	struct IsDescribedNode<T, std::void_t<
		typename NodeDescription<std::remove_cvref_t<T>>::Inputs,
		typename NodeDescription<std::remove_cvref_t<T>>::Outputs
	>> : std::true_type {};

	template<typename T>
	constexpr bool IsDescribedNode_v = IsDescribedNode<T>::value;

	//==============================================================================
	/// Endpoint utilities for automatic registration
	namespace EndpointUtilities {
		
		namespace Impl {
			
			/// Register input endpoints from member descriptions
			template<typename TNodeType>
			bool RegisterEndpointInputs(TNodeType* node)
			{
				if constexpr (IsDescribedNode_v<TNodeType>)
				{
					using InputsDescription = typename NodeDescription<TNodeType>::Inputs;
					
					return InputsDescription::MemberListType::ApplyToStaticType([node](const auto&... members)
					{
						auto registerInput = [node, memberIndex = 0](auto memberPtr) mutable
						{
							using TMember = std::remove_reference_t<decltype(memberPtr)>;
							constexpr bool isInputEvent = std::is_member_function_pointer_v<TMember>;
							
							const std::string_view memberName = InputsDescription::MemberNames[memberIndex++];
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
										std::invoke(memberPtr, node.get());
									}
									else if constexpr (std::is_invocable_v<FunctionType, decltype(*node), float>)
									{
										// Single float parameter event function
										std::invoke(memberPtr, node.get(), value);
									}
									// TODO: Add support for other event parameter types as needed
									});
								
								// Register the event with the node
								node->InEvs[OLO_IDENTIFIER(cleanName.c_str())] = *inputEvent;
								
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
									node->AddParameter<UnderlyingType>(OLO_IDENTIFIER(cleanName.c_str()), cleanName, UnderlyingType{});
									return true;
								}
								else
								{
									// Direct value members
									node->AddParameter<ValueType>(OLO_IDENTIFIER(cleanName.c_str()), cleanName, node->*memberPtr);
									return true;
								}
							}
						};
						
						return (registerInput(members) && ...);
					});
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
				if constexpr (IsDescribedNode_v<TNodeType>)
				{
					using OutputsDescription = typename NodeDescription<TNodeType>::Outputs;
					
					return OutputsDescription::MemberListType::ApplyToStaticType([node](const auto&... members)
					{
						auto registerOutput = [node, memberIndex = 0](auto memberPtr) mutable
						{
							using TMember = typename Core::Reflection::MemberPointer::ReturnType<decltype(memberPtr)>::Type;
							constexpr bool isOutputEvent = std::is_same_v<TMember, NodeProcessor::OutputEvent>;
							
							const std::string_view memberName = OutputsDescription::MemberNames[memberIndex++];
							const std::string cleanName = std::string(Core::Reflection::StringUtils::RemovePrefixAndSuffix(memberName));
							
							if constexpr (isOutputEvent)
							{
								// Handle output events
								// Output events are OutputEvent members that can trigger other events
								
								// Get reference to the OutputEvent member
								NodeProcessor::OutputEvent& outputEvent = node->*memberPtr;
								
								// Register the output event with the node
								node->OutEvs[OLO_IDENTIFIER(cleanName.c_str())] = outputEvent;
								
								return true;
							}
							else
							{
								// Handle output parameters (always direct values for outputs)
								using ValueType = typename Core::Reflection::MemberPointer::ReturnType<TMember>::Type;
								// Output values are registered but not added as parameters since they're computed
								// The processing function will set their values directly
								return true;
							}
						};
						
						return (registerOutput(members) && ...);
					});
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
				if constexpr (IsDescribedNode_v<TNodeType>)
				{
					using InputsDescription = typename NodeDescription<TNodeType>::Inputs;
					
					return InputsDescription::MemberListType::ApplyToStaticType([node](const auto&... members)
					{
						auto initializeInput = [node, memberIndex = 0](auto memberPtr) mutable
						{
							using TMember = std::remove_reference_t<decltype(memberPtr)>;
							constexpr bool isInputEvent = std::is_member_function_pointer_v<TMember>;
							
							const std::string_view memberName = InputsDescription::MemberNames[memberIndex++];
							const std::string cleanName = std::string(Core::Reflection::StringUtils::RemovePrefixAndSuffix(memberName));
							
							if constexpr (!isInputEvent)
							{
								using ValueType = typename Core::Reflection::MemberPointer::ReturnType<TMember>::Type;
								
								if constexpr (std::is_pointer_v<ValueType>)
								{
									// Connect pointer members to parameter system
									using UnderlyingType = std::remove_pointer_t<ValueType>;
									auto param = node->template GetParameter<UnderlyingType>(OLO_IDENTIFIER(cleanName.c_str()));
									if (param)
									{
										node->*memberPtr = &param->Value;
									}
								}
							}
							
							return true;
						};
						
						return (initializeInput(members) && ...);
					});
				}
				else
				{
					return false;
				}
			}
		}
		
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
	}

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
#define DESCRIBE_NODE(NodeType, InputList, OutputList)									\
	OLO_DESCRIBE_TAGGED(NodeType, OloEngine::Audio::SoundGraph::TagInputs, InputList)	\
	OLO_DESCRIBE_TAGGED(NodeType, OloEngine::Audio::SoundGraph::TagOutputs, OutputList)\
																						\
	template<> struct OloEngine::Audio::SoundGraph::NodeDescription<NodeType>			\
	{																					\
		using Inputs = OloEngine::Core::Reflection::Description<NodeType, OloEngine::Audio::SoundGraph::TagInputs>;	\
		using Outputs = OloEngine::Core::Reflection::Description<NodeType, OloEngine::Audio::SoundGraph::TagOutputs>;	\
	};
#endif // !DESCRIBE_NODE
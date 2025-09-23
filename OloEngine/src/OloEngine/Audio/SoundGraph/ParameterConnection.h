#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Identifier.h"
#include <memory>
#include <functional>

namespace OloEngine::Audio::SoundGraph
{
	// Forward declarations
	class NodeProcessor;
	
	//==============================================================================
	/// Represents a connection between two parameters (output -> input)
	class ParameterConnection
	{
	public:
		ParameterConnection() = default;
		
		/// Constructor for creating a parameter connection
		/// @param sourceNode Node that outputs the parameter value
		/// @param sourceParam ID of the output parameter
		/// @param targetNode Node that receives the parameter value
		/// @param targetParam ID of the input parameter
		ParameterConnection(NodeProcessor* sourceNode, const Identifier& sourceParam,
							NodeProcessor* targetNode, const Identifier& targetParam);

		virtual ~ParameterConnection() = default;

		//==============================================================================
		/// Connection Properties

		/// Check if this connection is valid
		bool IsValid() const;

		/// Get the source node
		NodeProcessor* GetSourceNode() const { return m_SourceNode; }

		/// Get the target node
		NodeProcessor* GetTargetNode() const { return m_TargetNode; }

		/// Get the source parameter ID
		const Identifier& GetSourceParameterID() const { return m_SourceParameterID; }

		/// Get the target parameter ID  
		const Identifier& GetTargetParameterID() const { return m_TargetParameterID; }

		//==============================================================================
		/// Value Propagation

		/// Update the target parameter with the current source value
		/// This is called during audio processing to propagate parameter values
		virtual void PropagateValue() = 0;

		/// Get connection type name for debugging
		virtual const char* GetTypeName() const = 0;

	protected:
		NodeProcessor* m_SourceNode = nullptr;
		NodeProcessor* m_TargetNode = nullptr;
		Identifier m_SourceParameterID;
		Identifier m_TargetParameterID;
	};

	//==============================================================================
	/// Typed parameter connection for specific data types
	template<typename T>
	class TypedParameterConnection : public ParameterConnection
	{
	public:
		TypedParameterConnection(NodeProcessor* sourceNode, const Identifier& sourceParam,
								NodeProcessor* targetNode, const Identifier& targetParam);

		/// Propagate the typed value from source to target
		void PropagateValue() override;

		/// Get type name for debugging
		const char* GetTypeName() const override;

	private:
		/// Optional transformation function to apply during propagation
		std::function<T(T)> m_Transform;

	public:
		/// Set a transformation function to modify the value during propagation
		/// Example: connection.SetTransform([](f32 x) { return x * 2.0f; }); // Double the value
		void SetTransform(std::function<T(T)> transform) { m_Transform = std::move(transform); }

		/// Clear any transformation function
		void ClearTransform() { m_Transform = nullptr; }
	};

	//==============================================================================
	/// Connection factory functions

	/// Create a parameter connection between two nodes
	/// @param sourceNode Node that outputs the parameter value
	/// @param sourceParam Name of the output parameter
	/// @param targetNode Node that receives the parameter value
	/// @param targetParam Name of the input parameter
	/// @returns Shared pointer to the connection, or nullptr if invalid
	template<typename T>
	std::shared_ptr<TypedParameterConnection<T>> CreateParameterConnection(
		NodeProcessor* sourceNode, const std::string& sourceParam,
		NodeProcessor* targetNode, const std::string& targetParam);

	/// Create a parameter connection using identifiers
	template<typename T>
	std::shared_ptr<TypedParameterConnection<T>> CreateParameterConnection(
		NodeProcessor* sourceNode, const Identifier& sourceParam,
		NodeProcessor* targetNode, const Identifier& targetParam);

	//==============================================================================
	/// Implementation details

	template<typename T>
	TypedParameterConnection<T>::TypedParameterConnection(NodeProcessor* sourceNode, const Identifier& sourceParam,
														  NodeProcessor* targetNode, const Identifier& targetParam)
		: ParameterConnection(sourceNode, sourceParam, targetNode, targetParam)
	{
	}

	template<typename T>
	void TypedParameterConnection<T>::PropagateValue()
	{
		if (!IsValid())
			return;

		// Get value from source parameter
		T sourceValue = m_SourceNode->GetParameterValue<T>(m_SourceParameterID, T{});

		// Apply transformation if present
		if (m_Transform)
		{
			sourceValue = m_Transform(sourceValue);
		}

		// Set value on target parameter
		m_TargetNode->SetParameterValue(m_TargetParameterID, sourceValue);
	}

	template<typename T>
	const char* TypedParameterConnection<T>::GetTypeName() const
	{
		if constexpr (std::is_same_v<T, f32>)
			return "f32";
		else if constexpr (std::is_same_v<T, i32>)
			return "i32";
		else if constexpr (std::is_same_v<T, bool>)
			return "bool";
		else
			return "unknown";
	}

	template<typename T>
	std::shared_ptr<TypedParameterConnection<T>> CreateParameterConnection(
		NodeProcessor* sourceNode, const std::string& sourceParam,
		NodeProcessor* targetNode, const std::string& targetParam)
	{
		if (!sourceNode || !targetNode)
			return nullptr;

		Identifier sourceID = OLO_IDENTIFIER(sourceParam.c_str());
		Identifier targetID = OLO_IDENTIFIER(targetParam.c_str());

		return CreateParameterConnection<T>(sourceNode, sourceID, targetNode, targetID);
	}

	template<typename T>
	std::shared_ptr<TypedParameterConnection<T>> CreateParameterConnection(
		NodeProcessor* sourceNode, const Identifier& sourceParam,
		NodeProcessor* targetNode, const Identifier& targetParam)
	{
		if (!sourceNode || !targetNode)
			return nullptr;

		// Verify that both parameters exist and are of the correct type
		if (!sourceNode->HasParameter(sourceParam) || !targetNode->HasParameter(targetParam))
			return nullptr;

		auto connection = std::make_shared<TypedParameterConnection<T>>(
			sourceNode, sourceParam, targetNode, targetParam);

		return connection->IsValid() ? connection : nullptr;
	}
}
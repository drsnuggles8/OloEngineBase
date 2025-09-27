#pragma once

#include "OloEngine/Audio/AudioCallback.h"
#include "OloEngine/Core/Identifier.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Core/Ref.h"
#include <choc/containers/choc_Value.h>

#include <string>
#include <functional>
#include <complex>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <memory>

#include <glm/gtc/constants.hpp>
#include <glm/glm.hpp>
#include <atomic>

#define LOG_DBG_MESSAGES 0

//==============================================================================
/// Simple flag utilities (from Hazel's Base.h)

namespace OloEngine::Audio::SoundGraph
{
	// Use Flag utilities from Base.h
	using ::Flag;
	using ::AtomicFlag;

} // namespace OloEngine::Audio::SoundGraph

#if LOG_DBG_MESSAGES
#define DBG(...) OLO_CORE_WARN(__VA_ARGS__)
#else
#define DBG(...)
#endif

namespace OloEngine::Audio::SoundGraph
{
	struct StreamWriter;

	//==============================================================================
	/// NodeProcessor - Base class for all sound graph nodes
	struct NodeProcessor
	{
		explicit NodeProcessor(const char* dbgName, UUID id) noexcept
			: DebugName(dbgName), ID(id)
		{
		}

		virtual ~NodeProcessor() = default;

		std::string DebugName;
		UUID ID;

	protected:
		f32 m_SampleRate = 48000.0f;

	public:
		//==============================================================================
		/// Base Input/Output structures

		struct Input
		{
			Input() { OLO_CORE_ASSERT(false, "Must not construct Input using default constructor"); }
			explicit Input(NodeProcessor& owner) noexcept : Node(&owner) {}
			NodeProcessor* Node = nullptr;
		};

		struct Output
		{
			Output() { OLO_CORE_ASSERT(false, "Must not construct Output using default constructor"); }
			Output(NodeProcessor& owner) noexcept : Node(&owner) {}
			NodeProcessor* Node = nullptr;
		};

		//==============================================================================
		/// Event handling system

		struct InputEvent : public Input
		{
			using EventFunction = std::function<void(float)>;

			explicit InputEvent(NodeProcessor& owner, EventFunction ev) noexcept
				: Input(owner), Event(std::move(ev))
			{
			}

			inline virtual void operator()(float value) noexcept
			{
				if (Event)
					Event(value);
			}

			// Should be bound to NodeProcessor member method
			EventFunction Event;
		};

		struct OutputEvent : public Output
		{
			explicit OutputEvent(NodeProcessor& owner) noexcept
				: Output(owner)
			{
			}

			inline void operator()(float value) noexcept
			{
				for (auto& out : DestinationEvents)
					(*out)(value);
			}

			void AddDestination(const std::shared_ptr<InputEvent>& dest)
			{
				DestinationEvents.push_back(dest);
			}

			// OutputEvent should always have destination Input to call
			std::vector<std::shared_ptr<InputEvent>> DestinationEvents;
		};

		//==============================================================================
		/// Endpoint management

		InputEvent& AddInEvent(Identifier id, InputEvent::EventFunction function = nullptr)
		{
			const auto& [element, inserted] = InEvents.try_emplace(id, *this, function);
			OLO_CORE_ASSERT(inserted, "Input event with this ID already exists");
			return element->second;
		}

		void AddOutEvent(Identifier id, OutputEvent& out)
		{
			const auto& [element, inserted] = OutEvents.insert({ id, out });
			OLO_CORE_ASSERT(inserted, "Output event with this ID already exists");
		}

		choc::value::ValueView& AddInStream(Identifier id, choc::value::ValueView* source = nullptr)
		{
			const auto& [element, inserted] = InputStreams.try_emplace(id);
			if (source)
				element->second = *source;

			OLO_CORE_ASSERT(inserted, "Input stream with this ID already exists");
			return element->second;
		}

		template<typename T>
		choc::value::ValueView& AddOutStream(Identifier id, T& memberVariable)
		{
			const auto& [element, inserted] = OutputStreams.try_emplace(id,
				choc::value::ValueView(choc::value::Type::createPrimitive<T>(),
					&memberVariable,
					nullptr));
			
			OLO_CORE_ASSERT(inserted, "Output stream with this ID already exists");
			return element->second;
		}

		//==============================================================================
		/// Virtual interface - must be implemented by derived nodes

		virtual void SetSampleRate(f32 sampleRate) { m_SampleRate = sampleRate; }
		virtual void Init() {}
		virtual void Process() {}

		//==============================================================================
		/// Endpoint access

		std::unordered_map<Identifier, InputEvent> InEvents;
		std::unordered_map<Identifier, OutputEvent&> OutEvents;

		std::unordered_map<Identifier, choc::value::ValueView> InputStreams;
		std::unordered_map<Identifier, choc::value::ValueView> OutputStreams;

		/// Temporary storage for default value plugs when nothing is connected to an input
		std::vector<std::shared_ptr<StreamWriter>> DefaultValuePlugs;

		//==============================================================================
		/// Convenience accessors

		inline choc::value::ValueView& InValue(const Identifier& id) { return InputStreams.at(id); }
		inline choc::value::ValueView& OutValue(const Identifier& id) { return OutputStreams.at(id); }
		inline InputEvent& InEvent(const Identifier& id) { return InEvents.at(id); }
		inline OutputEvent& OutEvent(const Identifier& id) { return OutEvents.at(id); }

		//==============================================================================
		/// Parameter system (OloEngine enhancement over Hazel)

		template<typename T>
		void AddParameter(Identifier id, std::string_view debugName, const T& defaultValue)
		{
			// Add input stream for this parameter
			auto& stream = AddInStream(id);
			
			// Create default value plug
			auto defaultPlug = std::make_shared<StreamWriter>(stream, T(defaultValue), id);
			DefaultValuePlugs.push_back(defaultPlug);
			
			// Store parameter info for debugging
			ParameterInfo info;
			info.ID = id;
			info.DebugName = std::string(debugName);
			info.TypeName = typeid(T).name();
			ParameterInfos[id] = std::move(info);
		}

	private:
		struct ParameterInfo
		{
			Identifier ID;
			std::string DebugName;
			std::string TypeName;
		};
		std::unordered_map<Identifier, ParameterInfo> ParameterInfos;
	};

	//==============================================================================
	/// StreamWriter - Utility for writing values to streams
	struct StreamWriter : public NodeProcessor
	{
		template<typename T>
		explicit StreamWriter(choc::value::ValueView& destination, T&& externalObjectOrDefaultValue, Identifier destinationID, UUID id = UUID()) noexcept
			: NodeProcessor("Stream Writer", id)
			, DestinationView(destination)
			, OutputValue(std::forward<T>(externalObjectOrDefaultValue))
			, DestinationID(destinationID)
		{
			DestinationView = OutputValue.getViewReference();
		}

		inline void operator<<(float value) noexcept
		{
			*(float*)OutputValue.getRawData() = value;
		}

		template<typename T>
		inline void operator<<(T value) noexcept
		{
			*(T*)OutputValue.getRawData() = value;
		}

		Identifier DestinationID;
		choc::value::Value OutputValue;
		choc::value::ValueView& DestinationView;
	};

	//==============================================================================
	/// Template specializations for StreamWriter

	template<>
	inline void StreamWriter::operator<<(const choc::value::ValueView& value) noexcept
	{
		OutputValue = value;
	}

	template<>
	inline void StreamWriter::operator<<(choc::value::ValueView value) noexcept
	{
		OutputValue = value;
	}

} // namespace OloEngine::Audio::SoundGraph

#undef LOG_DBG_MESSAGES
#undef DBG
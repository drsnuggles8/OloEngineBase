#pragma once

#include "OloEngine/Audio/AudioCallback.h"
#include "OloEngine/Core/Identifier.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Core/Ref.h"
#include <choc/containers/choc_Value.h>

#include <complex>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <utility>

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

		// Explicitly delete copy and move operations to prevent dangling pointers
		NodeProcessor(const NodeProcessor&) = delete;
		NodeProcessor& operator=(const NodeProcessor&) = delete;
		NodeProcessor(NodeProcessor&&) = delete;
		NodeProcessor& operator=(NodeProcessor&&) = delete;

		std::string DebugName;
		UUID ID;

	protected:
		f32 m_SampleRate = 48000.0f;

	public:
		//==============================================================================
		/// Base Input/Output structures

		struct Input
		{
			Input() = delete;
			explicit Input(NodeProcessor& owner) noexcept : Node(&owner) {}
			NodeProcessor* Node = nullptr;
		};

		struct Output
		{
			Output() = delete;
			explicit Output(NodeProcessor& owner) noexcept : Node(&owner) {}
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
				// Iterate through weak_ptr connections, cleaning up expired ones
				auto it = DestinationEvents.begin();
				while (it != DestinationEvents.end())
				{
					if (auto dest = it->lock()) // Check if still valid
					{
						(*dest)(value);
						++it;
					}
					else
					{
						// Remove expired weak_ptr
						it = DestinationEvents.erase(it);
					}
				}
			}

			void AddDestination(std::shared_ptr<InputEvent> dest)
			{
				if (dest)
					DestinationEvents.push_back(std::weak_ptr<InputEvent>(dest));
			}

			// Safe connection management using weak_ptr to prevent dangling pointers
			// InputEvent instances are now owned by shared_ptr in InEvents
			std::vector<std::weak_ptr<InputEvent>> DestinationEvents;
		};

		//==============================================================================
		/// Endpoint management

		InputEvent& AddInEvent(Identifier id, InputEvent::EventFunction function = nullptr)
		{
			auto inputEvent = std::make_shared<InputEvent>(*this, function);
			const auto& [element, inserted] = InEvents.try_emplace(id, inputEvent);
			OLO_CORE_ASSERT(inserted, "Input event with this ID already exists");
			return *element->second;
		}

		void AddOutEvent(Identifier id, OutputEvent& out)
		{
			const auto& [element, inserted] = OutEvents.insert({ id, std::ref(out) });
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

		std::unordered_map<Identifier, std::shared_ptr<InputEvent>> InEvents;
		std::unordered_map<Identifier, std::reference_wrapper<OutputEvent>> OutEvents;

		std::unordered_map<Identifier, choc::value::ValueView> InputStreams;
		std::unordered_map<Identifier, choc::value::ValueView> OutputStreams;

		/// Temporary storage for default value plugs when nothing is connected to an input
		std::vector<std::shared_ptr<StreamWriter>> DefaultValuePlugs;

		//==============================================================================
		/// Convenience accessors

		inline choc::value::ValueView& InValue(const Identifier& id) { return InputStreams.at(id); }
		inline choc::value::ValueView& OutValue(const Identifier& id) { return OutputStreams.at(id); }
		inline InputEvent& InEvent(const Identifier& id) { return *InEvents.at(id); }
		inline OutputEvent& OutEvent(const Identifier& id) { return OutEvents.at(id).get(); }

		//==============================================================================
		/// Parameter system (OloEngine enhancement over Hazel)

		//==============================================================================
		/// Parameter debugging/introspection (publicly accessible)
		
		struct ParameterInfo
		{
			Identifier ID;
			std::string DebugName;
			std::string TypeName;
		};
		std::unordered_map<Identifier, ParameterInfo> ParameterInfos;

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
	};

	//==============================================================================
	/// StreamWriter - Utility for writing values to streams
	struct StreamWriter : public NodeProcessor
	{
	template<typename T>
	explicit StreamWriter(const choc::value::ValueView& destination, T&& externalObjectOrDefaultValue, Identifier destinationID, UUID id = UUID()) noexcept
		: NodeProcessor("Stream Writer", id)
		, DestinationID(destinationID)
		, OutputValue(std::forward<T>(externalObjectOrDefaultValue))
		, DestinationView(OutputValue.getViewReference())
	{
		// DestinationView is now owned by value and initialized from OutputValue
	}

	// Explicitly delete copy and move operations to prevent dangling references
	StreamWriter(const StreamWriter&) = delete;
	StreamWriter& operator=(const StreamWriter&) = delete;
	StreamWriter(StreamWriter&&) = delete;
	StreamWriter& operator=(StreamWriter&&) = delete;

	inline void operator<<(float value) noexcept
		{
			OutputValue = choc::value::Value(value);
			DestinationView = OutputValue.getViewReference();
		}

		template<typename T>
		inline void operator<<(T value) noexcept
		{
			OutputValue = choc::value::Value(value);
			DestinationView = OutputValue.getViewReference();
		}

		Identifier DestinationID;
		choc::value::Value OutputValue;
		choc::value::ValueView DestinationView;
	};

	//==============================================================================
	/// Template specializations for StreamWriter

	template<>
	inline void StreamWriter::operator<<(const choc::value::ValueView& value) noexcept
	{
		OutputValue = value;
		DestinationView = OutputValue.getViewReference();
	}

	template<>
	inline void StreamWriter::operator<<(choc::value::ValueView value) noexcept
	{
		OutputValue = value;
		DestinationView = OutputValue.getViewReference();
	}

} // namespace OloEngine::Audio::SoundGraph

#undef LOG_DBG_MESSAGES
#undef DBG
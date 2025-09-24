#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Identifier.h"
#include "OloEngine/Core/UUID.h"
#include "Value.h"

#include <string>
#include <functional>
#include <complex>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <memory>

#define LOG_DBG_MESSAGES 0

#if LOG_DBG_MESSAGES
#define DBG(...) OLO_CORE_WARN(__VA_ARGS__)
#else
#define DBG(...)
#endif

namespace OloEngine::Audio::SoundGraph
{
	struct StreamWriter;

	//==============================================================================
	/// NodeProcessor - Full Hazel-style implementation with our own Value system
	struct NodeProcessor
	{
		explicit NodeProcessor(const char* dbgName, UUID id) noexcept
			: dbgName(dbgName), ID(id)
		{
		}

		std::string dbgName;
		UUID ID;

		//? this base struct is unnecessary
		struct Input
		{
			Input() { throw std::invalid_argument("Must not construct Input using default constructor"); }
			explicit Input(NodeProcessor& owner) noexcept : node(&owner) {}
			NodeProcessor* node = nullptr;
		};

		//? this base struct is unnecessary
		struct Output
		{
			Output() { throw std::invalid_argument("Must not construct Output using default constructor"); }
			Output(NodeProcessor& owner) noexcept : node(&owner) {}
			NodeProcessor* node = nullptr;
		};

		struct InputEvent : public Input
		{
			using TEvent = std::function<void(f32)>;

			explicit InputEvent(NodeProcessor& owner, TEvent ev) noexcept
				: Input(owner), Event(std::move(ev))
			{
			}

			inline virtual void operator()(f32 value) noexcept
			{
				Event(value);
			}

			// Should be bound to NodeProcessor member method
			TEvent Event;
		};

		struct OutputEvent : public Output
		{
			explicit OutputEvent(NodeProcessor& owner) noexcept
				: Output(owner)
			{
			}

			inline void operator()(f32 value) noexcept
			{
				for (auto& out : DestinationEvs)
					(*out)(value);
			}

			void AddDestination(const std::shared_ptr<InputEvent>& dest)
			{
				DestinationEvs.push_back(dest);
			}

			// OutputEvent should always have destination Input to call
			std::vector<std::shared_ptr<InputEvent>> DestinationEvs;
		};

		InputEvent& AddInEvent(Identifier id, InputEvent::TEvent function = nullptr)
		{
			const auto& [element, inserted] = InEvs.try_emplace(id, *this, function);
			OLO_CORE_ASSERT(inserted);
			return element->second;
		}

		void AddOutEvent(Identifier id, OutputEvent& out)
		{
			const auto& [element, inserted] = OutEvs.insert({ id, out });
			OLO_CORE_ASSERT(inserted);
		}

		ValueView& AddInStream(Identifier id, ValueView* source = nullptr)
		{
			const auto& [element, inserted] = Ins.try_emplace(id);
			if (source)
				element->second = *source;

			OLO_CORE_ASSERT(inserted);
			return element->second;
		}

		template<typename T>
		ValueView& AddOutStream(Identifier id, T& memberVariable)
		{
			const auto& [element, inserted] = Outs.try_emplace(id,
															   ValueView(ValueType::CreatePrimitive<T>(),
																		 &memberVariable,
																		 nullptr));
			
			OLO_CORE_ASSERT(inserted);
			return element->second;
		}

		virtual void Init() {}
		virtual void Process() {}

		std::unordered_map<Identifier, InputEvent> InEvs;
		std::unordered_map<Identifier, OutputEvent&> OutEvs;

		std::unordered_map<Identifier, ValueView> Ins;
		std::unordered_map<Identifier, ValueView> Outs;

		//? TEMP. If nothing is connected to an input, need to initialize it with owned default value,
		//?		 storing them like this for now
		std::vector<std::shared_ptr<StreamWriter>> DefaultValuePlugs;

		inline ValueView& InValue(const Identifier& id) { return Ins.at(id); }
		inline ValueView& OutValue(const Identifier& id) { return Outs.at(id); }
		inline InputEvent& InEvent(const Identifier& id) { return InEvs.at(id); }
		inline OutputEvent& OutEvent(const Identifier& id) { return OutEvs.at(id); }
	};

	//==============================================================================
	/// StreamWriter
	struct StreamWriter : public NodeProcessor
	{
		template<typename T>
		explicit StreamWriter(ValueView& destination, T&& externalObjectOrDefaultValue, Identifier destinationID, UUID id = UUID()) noexcept
			: NodeProcessor("Stream Writer", id)
			, DestinationV(destination)
			, outV(std::move(externalObjectOrDefaultValue))
			, DestinationID(destinationID)
		{
			DestinationV = outV; //? does this make `destination` constructor argument unnecessary?
		}

		inline void operator<<(f32 value) noexcept
		{
			*(f32*)outV.GetRawData() = value;
		}

		template<typename T>
		inline void operator<<(T value) noexcept
		{
			*(T*)outV.GetRawData() = value;
		}

		Identifier DestinationID;
		Value outV;
		ValueView& DestinationV;
	};

	template<>
	inline void StreamWriter::operator<<(const ValueView& value) noexcept
	{
		outV = Value(); // Reset and copy from view
		// Note: This is a simplified implementation - full version would need proper value copying
	}

} // namespace OloEngine::Audio::SoundGraph

#undef LOG_DBG_MESSAGES
#undef DBG
#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Identifier.h"
#include <functional>
#include <vector>
#include <memory>

#define OLO_FORCE_INLINE OLO_FINLINE

namespace OloEngine::Audio::SoundGraph
{
	// Forward declarations
	struct NodeProcessor;

	//==============================================================================
	/// Flag system for event-driven processing
	/// Used to mark when events need processing without locks
	struct Flag
	{
		/// Mark the flag as dirty (needs processing)
		OLO_FORCE_INLINE void SetDirty() noexcept { m_Flag = true; }
		
		/// Check if dirty and reset atomically
		/// Returns true if flag was dirty, false otherwise
		OLO_FORCE_INLINE bool CheckAndResetIfDirty() noexcept
		{
			if (m_Flag)
			{
				m_Flag = false;
				return true;
			}
			return false;
		}
		
		/// Check if flag is dirty without resetting
		OLO_FORCE_INLINE bool IsDirty() const noexcept { return m_Flag; }

	private:
		bool m_Flag = false;
	};

	//==============================================================================
	/// Input Event - receives events from other nodes
	struct InputEvent
	{
		using EventCallback = std::function<void(f32)>;

		explicit InputEvent(NodeProcessor& owner, EventCallback callback) noexcept
			: m_Owner(&owner), m_Callback(std::move(callback))
		{
		}

		/// Trigger the event with a value
		OLO_FORCE_INLINE void operator()(f32 value) noexcept
		{
			if (m_Callback)
				m_Callback(value);
		}

		NodeProcessor* m_Owner = nullptr;
		EventCallback m_Callback;
	};

	//==============================================================================
	/// Output Event - sends events to connected input events
	struct OutputEvent
	{
		explicit OutputEvent(NodeProcessor& owner) noexcept
			: m_Owner(&owner)
		{
		}

		/// Trigger all connected input events
		OLO_FORCE_INLINE void operator()(f32 value) noexcept
		{
			for (auto& destination : m_Destinations)
			{
				if (destination)
					(*destination)(value);
			}
		}

		/// Connect this output to an input event
		void ConnectTo(const std::shared_ptr<InputEvent>& inputEvent)
		{
			m_Destinations.push_back(inputEvent);
		}

		NodeProcessor* m_Owner = nullptr;
		std::vector<std::shared_ptr<InputEvent>> m_Destinations;
	};

	//==============================================================================
	/// Stream writer for value interpolation
	struct StreamWriter
	{
		f32 m_Value = 0.0f;
		std::string m_Name;

		StreamWriter(f32 initialValue, const std::string& name)
			: m_Value(initialValue), m_Name(name) {}

		StreamWriter& operator<<(f32 value)
		{
			m_Value = value;
			return *this;
		}

		operator f32() const { return m_Value; }
	};

} // namespace OloEngine::Audio::SoundGraph
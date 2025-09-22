#pragma once

#include "OloEngine/Core/Base.h"
#include "Events.h"
#include <functional>
#include <unordered_map>

namespace OloEngine::Audio::SoundGraph
{
	//==============================================================================
	/// Enhanced Flag utilities and management system
	/// Builds on the basic Flag from Events.h to provide higher-level functionality

	//==============================================================================
	/// Utility class for managing multiple flags with callbacks
	class FlagManager
	{
	public:
		using FlagCallback = std::function<void()>;

		/// Add a flag with an optional callback
		void AddFlag(const std::string& name, FlagCallback callback = nullptr)
		{
			m_Flags[name] = Flag{};
			if (callback)
			{
				m_Callbacks[name] = std::move(callback);
			}
		}

		/// Set a flag as dirty and trigger its callback
		void SetFlag(const std::string& name)
		{
			auto flagIt = m_Flags.find(name);
			if (flagIt != m_Flags.end())
			{
				flagIt->second.SetDirty();
				
				// Execute callback if present
				auto callbackIt = m_Callbacks.find(name);
				if (callbackIt != m_Callbacks.end())
				{
					callbackIt->second();
				}
			}
		}

		/// Check if a flag is dirty
		bool IsFlagSet(const std::string& name) const
		{
			auto it = m_Flags.find(name);
			return it != m_Flags.end() && it->second.IsDirty();
		}

		/// Clear a specific flag
		void ClearFlag(const std::string& name)
		{
			auto it = m_Flags.find(name);
			if (it != m_Flags.end())
			{
				it->second.CheckAndResetIfDirty(); // Use the atomic reset
			}
		}

		/// Clear all flags
		void ClearAllFlags()
		{
			for (auto& [name, flag] : m_Flags)
			{
				flag.CheckAndResetIfDirty(); // Use the atomic reset
			}
		}

		/// Get flag by name (non-const)
		Flag* GetFlag(const std::string& name)
		{
			auto it = m_Flags.find(name);
			return it != m_Flags.end() ? &it->second : nullptr;
		}

		/// Get flag by name (const)
		const Flag* GetFlag(const std::string& name) const
		{
			auto it = m_Flags.find(name);
			return it != m_Flags.end() ? &it->second : nullptr;
		}

		/// Process all dirty flags and execute their callbacks
		void ProcessDirtyFlags()
		{
			for (auto& [name, flag] : m_Flags)
			{
				if (flag.CheckAndResetIfDirty())
				{
					auto callbackIt = m_Callbacks.find(name);
					if (callbackIt != m_Callbacks.end())
					{
						callbackIt->second();
					}
				}
			}
		}

	private:
		std::unordered_map<std::string, Flag> m_Flags;
		std::unordered_map<std::string, FlagCallback> m_Callbacks;
	};

	//==============================================================================
	/// Event routing utilities for connecting graph nodes
	class EventRouter
	{
	public:
		/// Connect an output event to an input event
		static void ConnectEvents(std::shared_ptr<OutputEvent> output, 
								 std::shared_ptr<InputEvent> input)
		{
			if (output && input)
			{
				output->ConnectTo(input);
			}
		}

		/// Create a trigger event that sets a flag when called
		static std::function<void(f32)> CreateFlagTrigger(Flag& flag)
		{
			return [&flag](f32 value) { flag.SetDirty(); };
		}

		/// Create a callback that triggers an output event
		static std::function<void(f32)> CreateEventTrigger(std::shared_ptr<OutputEvent> output)
		{
			return [output](f32 value) 
			{ 
				if (output) (*output)(value); 
			};
		}
	};

} // namespace OloEngine::Audio::SoundGraph
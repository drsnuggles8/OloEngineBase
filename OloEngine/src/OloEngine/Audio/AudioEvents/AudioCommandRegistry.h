#pragma once

#include "OloEngine/Audio/AudioEvents/AudioCommands.h"

#include <filesystem>
#include <unordered_map>

namespace OloEngine::Audio
{
    /// Stores all audio event definitions (trigger commands).
    /// Loaded from YAML at project startup.
    class AudioCommandRegistry
    {
      public:
        /// Add a new trigger command. Returns its CommandID.
        [[nodiscard]] CommandID AddTrigger(const std::string& name);

        /// Remove a trigger command by CommandID.
        bool RemoveTrigger(CommandID id);

        /// Add an action to an existing trigger. Returns false if trigger not found.
        bool AddAction(CommandID triggerID, const TriggerAction& action);

        /// Remove an action from an existing trigger by index. Returns false on out-of-range.
        bool RemoveAction(CommandID triggerID, u32 actionIndex);

        /// Look up a trigger command (mutable). Returns nullptr if not found.
        [[nodiscard]] TriggerCommand* GetTrigger(CommandID id);

        /// Look up a trigger command (const). Returns nullptr if not found.
        [[nodiscard]] const TriggerCommand* GetTrigger(CommandID id) const;

        /// Get all trigger commands.
        [[nodiscard]] const auto& GetAllTriggers() const
        {
            return m_Triggers;
        }

        /// Check whether a trigger with the given ID exists.
        [[nodiscard]] bool Contains(CommandID id) const;

        /// Get number of registered triggers.
        [[nodiscard]] sizet GetTriggerCount() const
        {
            return m_Triggers.size();
        }

        /// Serialize the entire registry to a YAML file.
        void Serialize(const std::filesystem::path& filepath) const;

        /// Deserialize from a YAML file. Returns true on success.
        bool Deserialize(const std::filesystem::path& filepath);

        /// Remove all trigger commands.
        void Clear();

      private:
        std::unordered_map<u32, TriggerCommand> m_Triggers; // Key = CommandID::ID
    };

} // namespace OloEngine::Audio

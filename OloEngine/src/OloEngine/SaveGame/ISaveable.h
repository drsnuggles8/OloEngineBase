#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Serialization/Archive.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    // Callback types for C++ save/load hooks
    // The FArchive is configured for saving or loading — same function handles both directions.
    // The UUID identifies the entity whose state is being saved/loaded.
    using SaveLoadCallback = std::function<void(FArchive&, UUID)>;

    // Registry for per-class custom save/load callbacks.
    // Scripts (C#/Lua) and native code can register callbacks here.
    class SaveableRegistry
    {
      public:
        // Register a native C++ save/load callback for a class name
        static void Register(const std::string& className, SaveLoadCallback callback);

        // Unregister a callback
        static void Unregister(const std::string& className);

        // Check if a class has registered callbacks
        static bool Has(const std::string& className);

        // Invoke save/load callback for a class (no-op if not registered)
        static void Invoke(const std::string& className, FArchive& ar, UUID entityID);

        // Clear all registrations
        static void Clear();

      private:
        static std::unordered_map<std::string, SaveLoadCallback> s_Registry;
    };

    // Convenience: collect ISaveable data for an entity's script during save.
    // Returns serialized custom data blob, or empty if class has no ISaveable.
    // Returns false if serialization encountered an error.
    bool CollectSaveableData(UUID entityID, const std::string& className, std::vector<u8>& outData);

    // Convenience: restore ISaveable data for an entity's script during load.
    // Returns false if deserialization encountered an error.
    bool RestoreSaveableData(UUID entityID, const std::string& className, const std::vector<u8>& data);

} // namespace OloEngine

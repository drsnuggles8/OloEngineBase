#include "OloEnginePCH.h"
#include "ISaveable.h"
#include "OloEngine/Serialization/ArchiveExtensions.h"

namespace OloEngine
{
    std::unordered_map<std::string, SaveLoadCallback> SaveableRegistry::s_Registry;

    void SaveableRegistry::Register(const std::string& className, SaveLoadCallback callback)
    {
        s_Registry[className] = std::move(callback);
    }

    void SaveableRegistry::Unregister(const std::string& className)
    {
        s_Registry.erase(className);
    }

    bool SaveableRegistry::Has(const std::string& className)
    {
        return s_Registry.contains(className);
    }

    void SaveableRegistry::Invoke(const std::string& className, FArchive& ar)
    {
        auto it = s_Registry.find(className);
        if (it != s_Registry.end())
        {
            it->second(ar);
        }
    }

    void SaveableRegistry::Clear()
    {
        s_Registry.clear();
    }

    std::vector<u8> CollectSaveableData(UUID entityID, const std::string& className)
    {
        if (!SaveableRegistry::Has(className))
        {
            return {};
        }

        std::vector<u8> buffer;
        FMemoryWriter writer(buffer);
        writer.ArIsSaveGame = true;

        // Store entity ID for context
        writer << entityID;

        SaveableRegistry::Invoke(className, writer);
        return buffer;
    }

    void RestoreSaveableData(UUID entityID, const std::string& className, const std::vector<u8>& data)
    {
        if (data.empty() || !SaveableRegistry::Has(className))
        {
            return;
        }

        auto dataCopy = data;
        FMemoryReader reader(dataCopy);
        reader.ArIsSaveGame = true;

        // Read entity ID (for verification)
        UUID storedID;
        reader << storedID;

        SaveableRegistry::Invoke(className, reader);
    }

} // namespace OloEngine

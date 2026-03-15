#include "OloEnginePCH.h"
#include "ISaveable.h"
#include "OloEngine/Serialization/ArchiveExtensions.h"

namespace OloEngine
{
    std::unordered_map<std::string, SaveLoadCallback> SaveableRegistry::s_Registry;

    void SaveableRegistry::Register(const std::string& className, SaveLoadCallback callback)
    {
        OLO_PROFILE_FUNCTION();
        if (!callback)
        {
            OLO_CORE_WARN("[SaveableRegistry] Ignoring empty callback for '{}'", className);
            return;
        }
        s_Registry[className] = std::move(callback);
    }

    void SaveableRegistry::Unregister(const std::string& className)
    {
        OLO_PROFILE_FUNCTION();
        s_Registry.erase(className);
    }

    bool SaveableRegistry::Has(const std::string& className)
    {
        OLO_PROFILE_FUNCTION();
        return s_Registry.contains(className);
    }

    void SaveableRegistry::Invoke(const std::string& className, FArchive& ar, UUID entityID)
    {
        OLO_PROFILE_FUNCTION();
        auto it = s_Registry.find(className);
        if (it != s_Registry.end() && it->second)
        {
            it->second(ar, entityID);
        }
    }

    void SaveableRegistry::Clear()
    {
        OLO_PROFILE_FUNCTION();
        s_Registry.clear();
    }

    bool CollectSaveableData(UUID entityID, const std::string& className, std::vector<u8>& outData)
    {
        OLO_PROFILE_FUNCTION();

        outData.clear();

        if (!SaveableRegistry::Has(className))
        {
            return true; // No callback = no data, not an error
        }

        FMemoryWriter writer(outData);
        writer.ArIsSaveGame = true;

        SaveableRegistry::Invoke(className, writer, entityID);

        if (writer.IsError())
        {
            OLO_CORE_ERROR("[ISaveable] CollectSaveableData failed for '{}' (entityID={})", className, static_cast<u64>(entityID));
            outData.clear();
            return false;
        }

        return true;
    }

    bool RestoreSaveableData(UUID entityID, const std::string& className, const std::vector<u8>& data)
    {
        OLO_PROFILE_FUNCTION();

        if (data.empty() || !SaveableRegistry::Has(className))
        {
            return true; // Nothing to restore, not an error
        }

        auto dataCopy = data;
        FMemoryReader reader(dataCopy);
        reader.ArIsSaveGame = true;

        SaveableRegistry::Invoke(className, reader, entityID);

        if (reader.IsError())
        {
            OLO_CORE_ERROR("[ISaveable] RestoreSaveableData failed for '{}' (entityID={})", className, static_cast<u64>(entityID));
            return false;
        }

        return true;
    }

} // namespace OloEngine

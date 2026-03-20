#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Inventory/ItemDatabase.h"
#include "OloEngine/Core/Log.h"

#include <yaml-cpp/yaml.h>
#include <filesystem>

namespace OloEngine
{
    std::unordered_map<std::string, ItemDefinition>& ItemDatabase::GetItems()
    {
        static std::unordered_map<std::string, ItemDefinition> s_Items;
        return s_Items;
    }

    void ItemDatabase::LoadFromDirectory(const std::string& path)
    {
        OLO_PROFILE_FUNCTION();

        if (!std::filesystem::exists(path))
        {
            OLO_CORE_WARN("[ItemDatabase] Directory does not exist: {}", path);
            return;
        }

        for (auto const& entry : std::filesystem::recursive_directory_iterator(path))
        {
            if (entry.path().extension() == ".oloitem")
            {
                try
                {
                    YAML::Node data = YAML::LoadFile(entry.path().string());

                    auto itemNode = data["ItemDefinition"];
                    if (!itemNode)
                    {
                        OLO_CORE_WARN("[ItemDatabase] Missing 'ItemDefinition' root in {}", entry.path().string());
                        continue;
                    }

                    ItemDefinition def;
                    def.ItemID = itemNode["ItemID"].as<std::string>();
                    def.DisplayName = itemNode["DisplayName"].as<std::string>(def.ItemID);
                    def.Description = itemNode["Description"].as<std::string>("");
                    def.IconPath = itemNode["IconPath"].as<std::string>("");
                    def.MeshAsset = itemNode["MeshAsset"].as<std::string>("");

                    def.Category = ItemCategoryFromString(itemNode["Category"].as<std::string>("Misc"));
                    def.Rarity = ItemRarityFromString(itemNode["Rarity"].as<std::string>("Common"));

                    def.MaxStackSize = itemNode["MaxStackSize"].as<i32>(1);
                    def.Weight = itemNode["Weight"].as<f32>(0.0f);
                    def.BuyPrice = itemNode["BuyPrice"].as<i32>(0);
                    def.SellPrice = itemNode["SellPrice"].as<i32>(0);

                    def.IsQuestItem = itemNode["IsQuestItem"].as<bool>(false);
                    def.IsConsumable = itemNode["IsConsumable"].as<bool>(false);

                    if (auto modifiers = itemNode["AttributeModifiers"]; modifiers && modifiers.IsSequence())
                    {
                        for (auto const& mod : modifiers)
                        {
                            def.AttributeModifiers.emplace_back(
                                mod["Attribute"].as<std::string>(),
                                mod["Value"].as<f32>(0.0f));
                        }
                    }

                    if (auto tags = itemNode["Tags"]; tags && tags.IsSequence())
                    {
                        for (auto const& tag : tags)
                        {
                            def.Tags.push_back(tag.as<std::string>());
                        }
                    }

                    Register(def);
                    OLO_CORE_INFO("[ItemDatabase] Loaded item: {}", def.ItemID);
                }
                catch (const YAML::Exception& e)
                {
                    OLO_CORE_ERROR("[ItemDatabase] Failed to load {}: {}", entry.path().string(), e.what());
                }
            }
        }
    }

    void ItemDatabase::Register(const ItemDefinition& definition)
    {
        GetItems()[definition.ItemID] = definition;
    }

    const ItemDefinition* ItemDatabase::Get(const std::string& itemId)
    {
        auto& items = GetItems();
        auto it = items.find(itemId);
        if (it != items.end())
        {
            return &it->second;
        }
        return nullptr;
    }

    std::vector<const ItemDefinition*> ItemDatabase::GetByCategory(ItemCategory category)
    {
        std::vector<const ItemDefinition*> result;
        for (auto const& [id, def] : GetItems())
        {
            if (def.Category == category)
            {
                result.push_back(&def);
            }
        }
        return result;
    }

    std::vector<const ItemDefinition*> ItemDatabase::GetByTag(const std::string& tag)
    {
        std::vector<const ItemDefinition*> result;
        for (auto const& [id, def] : GetItems())
        {
            for (auto const& t : def.Tags)
            {
                if (t == tag)
                {
                    result.push_back(&def);
                    break;
                }
            }
        }
        return result;
    }

    std::vector<const ItemDefinition*> ItemDatabase::GetAll()
    {
        std::vector<const ItemDefinition*> result;
        for (auto const& [id, def] : GetItems())
        {
            result.push_back(&def);
        }
        return result;
    }

    void ItemDatabase::Clear()
    {
        GetItems().clear();
    }

} // namespace OloEngine

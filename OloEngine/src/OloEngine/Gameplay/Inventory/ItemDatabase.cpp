#include "OloEnginePCH.h"
#include "OloEngine/Gameplay/Inventory/ItemDatabase.h"
#include "OloEngine/Core/Log.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string_view>

namespace OloEngine
{
    namespace
    {
        // Validate a float read from an .oloitem YAML file (cpp-coding-quality §2b).
        // A corrupt or hand-edited asset can carry NaN/±inf; substitute a safe fallback
        // so it never reaches inventory weight / attribute math.
        [[nodiscard("sanitized value must be used")]] f32 SanitizeFinite(f32 value, f32 fallback, std::string_view field, const std::string& itemId)
        {
            if (!std::isfinite(value))
            {
                OLO_CORE_WARN("[ItemDatabase] Item '{}' has non-finite {} ({}); using {}", itemId, field, value, fallback);
                return fallback;
            }
            return value;
        }

        [[nodiscard]] f32 ReadNonNegative(const YAML::Node& node, const char* field, f32 fallback, const std::string& itemId)
        {
            const f32 value = SanitizeFinite(node[field].as<f32>(fallback), fallback, field, itemId);
            if (value >= 0.0f)
            {
                return value;
            }

            OLO_CORE_WARN("[ItemDatabase] Item '{}' has negative {} ({}); using {}", itemId, field, value, fallback);
            return fallback;
        }

        [[nodiscard]] u32 ReadAmmoCount(const YAML::Node& node, const char* field, u32 fallback, const std::string& itemId)
        {
            const i64 value = node[field].as<i64>(fallback);
            if (value >= 0 && static_cast<u64>(value) <= std::numeric_limits<u32>::max())
            {
                return static_cast<u32>(value);
            }

            OLO_CORE_WARN("[ItemDatabase] Item '{}' has invalid {} ({}); using {}", itemId, field, value, fallback);
            return fallback;
        }
    } // namespace

    std::unordered_map<std::string, ItemDefinition, StringHash, StringEqual>& ItemDatabase::GetItems()
    {
        static std::unordered_map<std::string, ItemDefinition, StringHash, StringEqual> s_Items;
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

        Clear();

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
                    // Weight feeds carry-capacity math (Inventory::GetTotalWeight); reject NaN/±inf and negatives.
                    def.Weight = SanitizeFinite(itemNode["Weight"].as<f32>(0.0f), 0.0f, "Weight", def.ItemID);
                    if (def.Weight < 0.0f)
                    {
                        OLO_CORE_WARN("[ItemDatabase] Item '{}' has negative Weight ({}); using 0", def.ItemID, def.Weight);
                        def.Weight = 0.0f;
                    }
                    def.BuyPrice = itemNode["BuyPrice"].as<i32>(0);
                    def.SellPrice = itemNode["SellPrice"].as<i32>(0);

                    def.IsQuestItem = itemNode["IsQuestItem"].as<bool>(false);
                    def.IsConsumable = itemNode["IsConsumable"].as<bool>(false);

                    if (const auto weaponNode = itemNode["Weapon"]; weaponNode && weaponNode.IsMap())
                    {
                        WeaponDefinition weapon;
                        weapon.Delivery = WeaponDeliveryFromString(weaponNode["Delivery"].as<std::string>("Hitscan"));
                        weapon.Damage = ReadNonNegative(weaponNode, "Damage", weapon.Damage, def.ItemID);
                        weapon.Range = ReadNonNegative(weaponNode, "Range", weapon.Range, def.ItemID);
                        weapon.RoundsPerMinute = ReadNonNegative(weaponNode, "RoundsPerMinute", weapon.RoundsPerMinute, def.ItemID);
                        weapon.MagazineSize = ReadAmmoCount(weaponNode, "MagazineSize", weapon.MagazineSize, def.ItemID);
                        weapon.ReserveAmmo = ReadAmmoCount(weaponNode, "ReserveAmmo", weapon.ReserveAmmo, def.ItemID);
                        weapon.ReloadSeconds = ReadNonNegative(weaponNode, "ReloadSeconds", weapon.ReloadSeconds, def.ItemID);
                        weapon.RecoilPitch = ReadNonNegative(weaponNode, "RecoilPitch", weapon.RecoilPitch, def.ItemID);
                        weapon.RecoilYaw = ReadNonNegative(weaponNode, "RecoilYaw", weapon.RecoilYaw, def.ItemID);
                        weapon.FalloffStart = ReadNonNegative(weaponNode, "FalloffStart", weapon.FalloffStart, def.ItemID);
                        weapon.FalloffEnd = ReadNonNegative(weaponNode, "FalloffEnd", weapon.FalloffEnd, def.ItemID);
                        weapon.MinimumDamageMultiplier = std::clamp(
                            ReadNonNegative(weaponNode, "MinimumDamageMultiplier", weapon.MinimumDamageMultiplier, def.ItemID), 0.0f, 1.0f);
                        weapon.ProjectileSpeed = ReadNonNegative(weaponNode, "ProjectileSpeed", weapon.ProjectileSpeed, def.ItemID);
                        weapon.ProjectileRadius = ReadNonNegative(weaponNode, "ProjectileRadius", weapon.ProjectileRadius, def.ItemID);
                        weapon.ProjectileLifetime = ReadNonNegative(weaponNode, "ProjectileLifetime", weapon.ProjectileLifetime, def.ItemID);
                        weapon.DamageType = weaponNode["DamageType"].as<std::string>(weapon.DamageType);
                        weapon.MuzzleAudioTrigger = weaponNode["MuzzleAudioTrigger"].as<std::string>("");
                        weapon.ImpactAudioTrigger = weaponNode["ImpactAudioTrigger"].as<std::string>("");
                        weapon.HitReactionTrigger = weaponNode["HitReactionTrigger"].as<std::string>(weapon.HitReactionTrigger);
                        def.Weapon = std::move(weapon);
                    }

                    if (auto modifiers = itemNode["AttributeModifiers"]; modifiers && modifiers.IsSequence())
                    {
                        for (auto const& mod : modifiers)
                        {
                            // Negative modifiers are legitimate (debuffs); only reject NaN/±inf.
                            def.AttributeModifiers.emplace_back(
                                mod["Attribute"].as<std::string>(),
                                SanitizeFinite(mod["Value"].as<f32>(0.0f), 0.0f, "AttributeModifier Value", def.ItemID));
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
        auto& items = GetItems();
        if (items.contains(definition.ItemID))
        {
            OLO_CORE_WARN("[ItemDatabase] Duplicate item ID '{}' — registration ignored", definition.ItemID);
            return;
        }
        items[definition.ItemID] = definition;
    }

    const ItemDefinition* ItemDatabase::Get(const std::string& itemId)
    {
        auto& items = GetItems();
        if (auto it = items.find(itemId); it != items.end())
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

    std::vector<const ItemDefinition*> ItemDatabase::GetByTag(std::string_view tag)
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

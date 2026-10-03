#include "OloEnginePCH.h"
#include "OloEngine/Renderer/MaterialOverride.h"

#include "OloEngine/Renderer/Material.h"

#include <algorithm>
#include <cmath>

namespace OloEngine
{
    namespace
    {
        [[nodiscard]] bool IsFiniteColor(const glm::vec4& color) noexcept
        {
            return std::isfinite(color.r) && std::isfinite(color.g) && std::isfinite(color.b) && std::isfinite(color.a);
        }

        [[nodiscard]] bool SameOverrides(const TArray<MaterialOverride>& built, std::span<const MaterialOverride> current)
        {
            if (static_cast<sizet>(built.Num()) != current.size())
            {
                return false;
            }
            for (sizet i = 0; i < current.size(); ++i)
            {
                if (!(built[static_cast<i32>(i)] == current[i]))
                {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool TableHas(std::span<const Ref<Material>> table, const Material* material) noexcept
        {
            return std::ranges::any_of(table, [material](const Ref<Material>& entry)
                                       { return entry.Raw() == material; });
        }
    } // namespace

    bool ApplyMaterialOverride(Material& material, const MaterialOverride& patch)
    {
        bool everythingApplied = true;

        // The kind is a discriminated value: an unknown one becomes Generic
        // rather than indexing the shader's kind switch with a number it has no
        // branch for (the rule every MaterialKind deserializer follows).
        if (IsValidMaterialKind(static_cast<i32>(std::to_underlying(patch.Kind))))
        {
            material.SetMaterialKind(patch.Kind);
        }
        else
        {
            material.SetMaterialKind(MaterialKind::Generic);
            everythingApplied = false;
        }
        material.SetSkinProfileHandle(patch.SkinProfile);
        if (std::isfinite(patch.ThicknessFactor))
        {
            // The setter owns the >= 0 floor.
            material.SetThicknessFactor(patch.ThicknessFactor);
        }
        else
        {
            everythingApplied = false;
        }

        if (patch.OverrideBaseColor)
        {
            if (IsFiniteColor(patch.BaseColor))
            {
                material.SetBaseColorFactor(patch.BaseColor);
            }
            else
            {
                everythingApplied = false;
            }
        }
        if (patch.OverrideRoughness)
        {
            if (std::isfinite(patch.Roughness))
            {
                material.SetRoughnessFactor(std::clamp(patch.Roughness, 0.0f, 1.0f));
            }
            else
            {
                everythingApplied = false;
            }
        }
        if (patch.OverrideMetallic)
        {
            if (std::isfinite(patch.Metallic))
            {
                material.SetMetallicFactor(std::clamp(patch.Metallic, 0.0f, 1.0f));
            }
            else
            {
                everythingApplied = false;
            }
        }
        return everythingApplied;
    }

    const MaterialOverride* FindMaterialOverride(std::span<const MaterialOverride> overrides, std::string_view materialName)
    {
        if (materialName.empty())
        {
            return nullptr;
        }
        for (const MaterialOverride& patch : overrides)
        {
            if (patch.MaterialName.ToView() == materialName)
            {
                return &patch;
            }
        }
        return nullptr;
    }

    MaterialOverrideStatus ClassifyMaterialOverride(std::span<const MaterialOverride> overrides, sizet index,
                                                    std::span<const Ref<Material>> importedTable)
    {
        if (index >= overrides.size() || overrides[index].MaterialName.IsEmpty())
        {
            return MaterialOverrideStatus::EmptyName;
        }
        const std::string_view name = overrides[index].MaterialName.ToView();
        for (sizet earlier = 0; earlier < index; ++earlier)
        {
            if (overrides[earlier].MaterialName.ToView() == name)
            {
                return MaterialOverrideStatus::DuplicateName;
            }
        }
        const bool matches = std::ranges::any_of(importedTable, [name](const Ref<Material>& imported)
                                                 { return imported && imported->GetName().ToView() == name; });
        return matches ? MaterialOverrideStatus::Applies : MaterialOverrideStatus::NoMatchingMaterial;
    }

    const char* DescribeMaterialOverrideStatus(MaterialOverrideStatus status) noexcept
    {
        switch (status)
        {
            case MaterialOverrideStatus::Applies:
                return "patches the imported material of that name";
            case MaterialOverrideStatus::EmptyName:
                return "no material name is set, so it patches nothing";
            case MaterialOverrideStatus::NoMatchingMaterial:
                return "the mesh has no imported material of that name, so it patches nothing";
            case MaterialOverrideStatus::DuplicateName:
                return "an earlier override already names that material, so this one is ignored";
        }
        return "unknown";
    }

    bool MaterialOverrideCache::Prepare(std::span<const MaterialOverride> overrides,
                                        std::span<const Ref<Material>> importedTable)
    {
        bool listChanged = false;
        if (!m_Bound || !SameOverrides(m_BuiltFrom, overrides))
        {
            // Every patch was built from the old list, so all of them go: one
            // edited field would otherwise leave a stale copy shading the entity.
            m_Entries.Reset();
            m_BuiltFrom.Reset();
            m_BuiltFrom.Reserve(static_cast<i32>(overrides.size()));
            for (const MaterialOverride& patch : overrides)
            {
                m_BuiltFrom.Add(patch);
            }
            m_RejectedPatches = 0;
            m_Bound = true;
            listChanged = true;
        }

        for (const Ref<Material>& imported : importedTable)
        {
            if (!imported)
            {
                continue;
            }
            // Positive or negative, an existing entry already answers for this
            // material under the current list.
            bool known = false;
            for (const MaterialOverridePatch& entry : m_Entries)
            {
                if (entry.Key == imported.Raw())
                {
                    known = true;
                    break;
                }
            }
            if (known)
            {
                continue;
            }

            MaterialOverridePatch entry;
            entry.Key = imported.Raw();
            entry.Imported = imported;
            const std::span<const MaterialOverride> builtFrom{ m_BuiltFrom.GetData(), static_cast<sizet>(m_BuiltFrom.Num()) };
            if (const MaterialOverride* patch = FindMaterialOverride(builtFrom, imported->GetName().ToView()))
            {
                // A COPY: the imported material is shared by every entity using
                // this mesh source, and patching it in place would re-skin all of
                // them.
                entry.Patched = Ref<Material>::Create(*imported);
                if (!ApplyMaterialOverride(*entry.Patched, *patch))
                {
                    ++m_RejectedPatches;
                }
            }
            m_Entries.Add(std::move(entry));
        }

        // Bound the cache. Entries for materials that are no longer in the
        // prepared table only accumulate across mesh swaps and LOD tables; drop
        // them once they outnumber the live table several times over, which
        // tolerates a few alternating LOD tables without rebuilding every frame.
        if (static_cast<sizet>(m_Entries.Num()) > importedTable.size() * 4 + 16)
        {
            for (i32 i = m_Entries.Num() - 1; i >= 0; --i)
            {
                if (!TableHas(importedTable, m_Entries[i].Key))
                {
                    m_Entries.RemoveAt(i);
                }
            }
        }
        return listChanged;
    }

    const Material* MaterialOverrideCache::Find(const Material* imported) const noexcept
    {
        if (imported == nullptr)
        {
            return nullptr;
        }
        for (const MaterialOverridePatch& entry : m_Entries)
        {
            if (entry.Key == imported)
            {
                return entry.Patched.Raw();
            }
        }
        return nullptr;
    }

    void MaterialOverrideCache::Reset()
    {
        m_Entries.Reset();
        m_BuiltFrom.Reset();
        m_RejectedPatches = 0;
        m_Bound = false;
    }

    u32 MaterialOverrideCache::GetPatchCount() const noexcept
    {
        u32 count = 0;
        for (const MaterialOverridePatch& entry : m_Entries)
        {
            if (entry.Patched)
            {
                ++count;
            }
        }
        return count;
    }
} // namespace OloEngine

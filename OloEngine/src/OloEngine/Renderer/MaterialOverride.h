#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Containers/Array.h"
#include "OloEngine/Containers/String.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/Material.h" // complete: the cache holds and destroys Ref<Material>
#include "OloEngine/Renderer/MaterialKind.h"
#include "OloEngine/Scene/ComponentReflection.h" // OLO_SERIALIZE — expands to nothing; read by OloHeaderTool

#include <glm/glm.hpp>

#include <span>
#include <string_view>

// =============================================================================
// MaterialOverride.h — per-entity patches of IMPORTED materials (issue #1533).
//
// A MaterialComponent replaces the material of EVERY submesh; an imported model
// (glTF through Assimp) cannot carry the engine's skin fields at all. A furred
// dog's nose, tongue, gums and lips are submeshes of one skinned body mesh and
// each needs its own skin profile, so neither was enough. A MaterialOverride
// names one imported material and patches a COPY of it — never the shared
// original, which every entity using the same mesh source shares through the
// asset/animated-model caches.
//
// Precedence on every path that shades or records a submesh for an entity
// (SubmeshMaterialResolve.h):
//   MaterialComponent (every submesh) -> a matching MaterialOverride patch of
//   the submesh's imported material -> the imported material -> engine default.
// =============================================================================

namespace OloEngine
{
    // One authored patch. All-trivial on purpose, so MaterialOverridesComponent's
    // scene YAML and binary blocks are OloHeaderTool-generated; the OLO_SERIALIZE
    // bounds mirror the setters' own rules, and ApplyMaterialOverride re-validates
    // every field anyway, so a value that reached the struct some other way is
    // refused at the point it would reach a material.
    struct MaterialOverride
    {
        // The IMPORTED material this patches, by Material::GetName() — the name
        // the importer gave it. Case-sensitive; an empty name patches nothing.
        FString MaterialName;
        // What the surface IS. Reject, not Clamp: a corrupt 7 must not saturate
        // onto Foliage (3), a different valid kind — same rule as the
        // MaterialComponent block in SceneSerializer.cpp.
        OLO_SERIALIZE(Reject, Min = 0, Max = 3)
        MaterialKind Kind = MaterialKind::Generic;
        // The SkinProfile asset (0 = none). Stored whatever the kind, as
        // Material does; only a Skin material reads it.
        AssetHandle SkinProfile = 0;
        // Metres, KHR_materials_volume, same unit and floor as
        // Material::SetThicknessFactor.
        OLO_SERIALIZE(Clamp, Min = 0.0f)
        f32 ThicknessFactor = 0.0f;
        // The three factors are OPTIONAL patches: false keeps the imported value.
        bool OverrideBaseColor = false;
        glm::vec4 BaseColor{ 1.0f };
        bool OverrideRoughness = false;
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 1.0f)
        f32 Roughness = 0.5f;
        bool OverrideMetallic = false;
        OLO_SERIALIZE(Clamp, Min = 0.0f, Max = 1.0f)
        f32 Metallic = 0.0f;

        // Bit-exact on the floats (cpp-coding-quality §2a: undo change detection),
        // u64 on the handle (UUID's implicit conversion makes == ambiguous).
        auto operator==(const MaterialOverride& other) const -> bool
        {
            return MaterialName == other.MaterialName && Kind == other.Kind &&
                   static_cast<u64>(SkinProfile) == static_cast<u64>(other.SkinProfile) &&
                   Math::BitwiseEqual(ThicknessFactor, other.ThicknessFactor) &&
                   OverrideBaseColor == other.OverrideBaseColor && Math::BitwiseEqual(BaseColor, other.BaseColor) &&
                   OverrideRoughness == other.OverrideRoughness && Math::BitwiseEqual(Roughness, other.Roughness) &&
                   OverrideMetallic == other.OverrideMetallic && Math::BitwiseEqual(Metallic, other.Metallic);
        }
    };

    // The Reject bound above is a literal because OloHeaderTool emits it into
    // three generated consumers; this keeps it honest when a kind is added.
    static_assert(kMaterialKindCount - 1 == 3, "MaterialOverride::Kind's OLO_SERIALIZE(Reject, Max = 3) must track kMaterialKindCount");

    // Heap-backed FString plus scalars; no member points into the struct.
    template<>
    struct TIsTriviallyRelocatable<MaterialOverride>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(MaterialOverride::MaterialName)> &&
                                      TIsTriviallyRelocatable_V<decltype(MaterialOverride::Kind)> &&
                                      TIsTriviallyRelocatable_V<decltype(MaterialOverride::SkinProfile)> &&
                                      TIsTriviallyRelocatable_V<decltype(MaterialOverride::ThicknessFactor)> &&
                                      TIsTriviallyRelocatable_V<decltype(MaterialOverride::OverrideBaseColor)> &&
                                      TIsTriviallyRelocatable_V<decltype(MaterialOverride::BaseColor)> &&
                                      TIsTriviallyRelocatable_V<decltype(MaterialOverride::OverrideRoughness)> &&
                                      TIsTriviallyRelocatable_V<decltype(MaterialOverride::Roughness)> &&
                                      TIsTriviallyRelocatable_V<decltype(MaterialOverride::OverrideMetallic)> &&
                                      TIsTriviallyRelocatable_V<decltype(MaterialOverride::Metallic)>;
    };

    // Applies one patch to a material COPY. Every field is re-validated here —
    // an out-of-range kind becomes Generic, a non-finite factor is not applied —
    // and the return value says whether anything was refused, so the caller can
    // report it instead of shading with half a patch in silence.
    [[nodiscard]] bool ApplyMaterialOverride(Material& material, const MaterialOverride& patch);

    // The FIRST override naming `materialName` (later duplicates are ignored and
    // reported as such by ClassifyMaterialOverride), or null.
    [[nodiscard]] const MaterialOverride* FindMaterialOverride(std::span<const MaterialOverride> overrides,
                                                               std::string_view materialName);

    // What one authored override does against an imported material table. For
    // the inspector's per-row status and the Scene's load-time report.
    enum class MaterialOverrideStatus : u8
    {
        // Names a material the table has, and is the first to name it.
        Applies = 0,
        // MaterialName is empty.
        EmptyName,
        // No material of the table has that name (a typo, or the wrong mesh).
        NoMatchingMaterial,
        // An earlier override already names the same material; this one is ignored.
        DuplicateName,
    };

    [[nodiscard]] MaterialOverrideStatus ClassifyMaterialOverride(std::span<const MaterialOverride> overrides, sizet index,
                                                                  std::span<const Ref<Material>> importedTable);
    [[nodiscard]] const char* DescribeMaterialOverrideStatus(MaterialOverrideStatus status) noexcept;

    // One patched copy, keyed by the imported material it was built from.
    struct MaterialOverridePatch
    {
        // Identity key, == Imported.Raw(); kept beside it so a lookup touches no
        // refcount.
        const Material* Key = nullptr;
        // Holds the imported material alive while its patch exists. Without it a
        // freed material's address could be reused by a new one and the lookup
        // would hand back a patch of the old.
        Ref<Material> Imported;
        // The patched copy, or null when no override names the imported
        // material — a negative entry, so the name scan is not repeated per frame.
        Ref<Material> Patched;
    };

    template<>
    struct TIsTriviallyRelocatable<MaterialOverridePatch>
    {
        static constexpr bool Value = TIsTriviallyRelocatable_V<decltype(MaterialOverridePatch::Key)> &&
                                      TIsTriviallyRelocatable_V<decltype(MaterialOverridePatch::Imported)> &&
                                      TIsTriviallyRelocatable_V<decltype(MaterialOverridePatch::Patched)>;
    };

    // The per-entity patched copies (runtime-only state of MaterialOverridesComponent).
    // Game thread only: Scene::PrepareMaterialOverrides fills it and every
    // submission path reads it in the same frame.
    class MaterialOverrideCache
    {
      public:
        MaterialOverrideCache() = default;
        // A copy starts EMPTY. The patched materials are derived state, rebuilt
        // lazily from the override list, and a copied cache would share Material
        // objects between two entities — so an edit to one would shade the other.
        MaterialOverrideCache(const MaterialOverrideCache& /*other*/) noexcept {}
        MaterialOverrideCache& operator=(const MaterialOverrideCache& other) noexcept
        {
            if (this != &other)
            {
                Reset();
            }
            return *this;
        }
        MaterialOverrideCache(MaterialOverrideCache&&) noexcept = default;
        MaterialOverrideCache& operator=(MaterialOverrideCache&&) noexcept = default;
        ~MaterialOverrideCache() = default;

        // Brings the cache in line with `overrides` — every patch is dropped when
        // the list differs from the one they were built from — and makes sure
        // every material of `importedTable` has an entry, building the patched
        // copy of each one an override names. Returns true when the list changed
        // since the last call (including the first call), which is when the Scene
        // reports what the list does against this table.
        bool Prepare(std::span<const MaterialOverride> overrides, std::span<const Ref<Material>> importedTable);

        // The patched copy this entity shades `imported` with, or null when no
        // override names it (or it was never prepared).
        [[nodiscard]] const Material* Find(const Material* imported) const noexcept;

        // Drops every patch and the list they were built from.
        void Reset();

        // Entries that carry a patch, and patches whose fields were refused by
        // ApplyMaterialOverride (both for diagnostics).
        [[nodiscard]] u32 GetPatchCount() const noexcept;
        [[nodiscard]] u32 GetRejectedPatchCount() const noexcept
        {
            return m_RejectedPatches;
        }

      private:
        TArray<MaterialOverride> m_BuiltFrom;
        TArray<MaterialOverridePatch> m_Entries;
        u32 m_RejectedPatches = 0;
        bool m_Bound = false;
    };
} // namespace OloEngine

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Terrain/Foliage/FoliageLayer.h"

#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace OloEngine
{
    // Canonical identity for foliage instances (issue #1230).
    //
    // Before this, a plant WAS its row index in the per-layer instance VBO:
    // GenerateInstances rebuilt every layer wholesale, so every regeneration
    // renumbered every plant. Nothing downstream could say "this plant" across
    // two frames, which is what visibility and residency work (#1240, #1233)
    // needs to hang state off.
    //
    // The registry gives each plant a stable id that is INDEPENDENT of the
    // instance-buffer row order. The GPU row stays a projection of the record;
    // the record is the truth.
    //
    // ── What identity is keyed on ─────────────────────────────────────────
    //
    // A plant's identity is its PLACEMENT SLOT, not its position. The
    // generator is deterministic: layer L, grid cell (ix, iz) and the handful
    // of parameters that map a cell to an XZ position decide entirely whether
    // a plant appears there and where. So the key is
    //
    //     (layer identity, placement signature, cell x, cell z)
    //
    // and the consequences are deliberate:
    //
    //   * A terrain sculpt that moves a plant's ground height keeps its id —
    //     same plant, new Y. The record's position and bounds update.
    //   * A sculpt that makes a cell too steep DROPS that plant: the cell no
    //     longer emits, so its id retires. That falls out of the reconcile
    //     rather than needing a slope-specific rule.
    //   * An edit that changes the cell -> XZ mapping (density, world size, or
    //     the layer's generator seed, which is its physical index) changes the
    //     placement signature, so every id in that layer retires and fresh ones
    //     are issued. Those really are different plants in different places;
    //     keeping the ids would be the silent lie this class exists to prevent.
    //   * An edit that only changes ATTRIBUTES — tint, scale range, wind, view
    //     distance, impostor settings — keeps every id.
    //
    // ── Ids are never reused ──────────────────────────────────────────────
    //
    // Ids come from a monotonic counter, so a retired id can never be handed to
    // a different plant. That also makes "is this id retired?" free: an id is
    // retired iff it was issued and is not live. No retired-set to grow without
    // bound.
    using FoliageInstanceId = u64;

    inline constexpr FoliageInstanceId kInvalidFoliageInstanceId = 0;

    // How the renderer represents a live instance. Explicit metadata, per the
    // issue's first criterion — a consumer must not have to re-derive it from
    // layer flags.
    enum class FoliageRepresentation : u8
    {
        // The flat billboard card the raster path has always drawn.
        MeshCard = 0,
        // Octahedral impostor atlas (#433): beyond ImpostorStartDistance the
        // card cross-fades into a view-dependent impostor.
        Impostor,
        // Canonical, but the current raster path draws nothing for it — the
        // layer produced placements and has no card geometry.
        //
        // Today this only occurs if the billboard VAO could not be created,
        // because every enabled layer gets one: with the card path alone,
        // every placed plant IS drawable, so this count is legitimately 0 in a
        // healthy scene and m_UnsupportedVariants is the counter that actually
        // moves. It is here as the honest name for "canonical but undrawn",
        // which the representations #1240 / #1233 add can genuinely be.
        Unsupported,
        Count,
    };

    inline constexpr sizet FoliageRepresentationCount =
        static_cast<sizet>(FoliageRepresentation::Count);

    [[nodiscard]] constexpr const char* GetFoliageRepresentationName(FoliageRepresentation rep)
    {
        switch (rep)
        {
            case FoliageRepresentation::MeshCard:
                return "Mesh card";
            case FoliageRepresentation::Impostor:
                return "Impostor";
            case FoliageRepresentation::Unsupported:
                return "Unsupported";
            case FoliageRepresentation::Count:
                break;
        }
        return "Unknown";
    }

    // The generator inputs an instance's identity is keyed on. See the class
    // comment for why each part is here.
    struct FoliagePlacementKey
    {
        // Registry-assigned, stable across regenerations for the same layer.
        u32 m_LayerKey = 0;
        // Hash of everything that maps a grid cell to an XZ position.
        u64 m_PlacementSignature = 0;
        u32 m_CellX = 0;
        u32 m_CellZ = 0;

        [[nodiscard]] auto operator==(const FoliagePlacementKey&) const -> bool = default;
    };

    struct FoliagePlacementKeyHash
    {
        [[nodiscard]] sizet operator()(const FoliagePlacementKey& k) const noexcept
        {
            // Collisions are harmless: the map compares full keys on top of
            // this, so two colliding placements stay two placements.
            u64 h = k.m_PlacementSignature;
            h ^= static_cast<u64>(k.m_LayerKey) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
            h ^= static_cast<u64>(k.m_CellX) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
            h ^= static_cast<u64>(k.m_CellZ) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
            return static_cast<sizet>(h);
        }
    };

    // The material an instance is associated with. Interned by the registry so
    // thousands of instances share one descriptor and carry a 4-byte key.
    //
    // Paths rather than AssetHandles because that is what FoliageLayer authors
    // today; when foliage layers move to handles this struct is the one place
    // that changes.
    struct FoliageMaterialDesc
    {
        std::string m_MeshPath;
        std::string m_AlbedoPath;
        glm::vec3 m_BaseColor{ 1.0f };
        f32 m_Roughness = 0.8f;
        f32 m_AlphaCutoff = 0.5f;

        // Floats compared bitwise per cpp-coding-quality §2a — never ==.
        [[nodiscard]] auto operator==(const FoliageMaterialDesc& other) const -> bool
        {
            return m_MeshPath == other.m_MeshPath && m_AlbedoPath == other.m_AlbedoPath && Math::BitwiseEqual(m_BaseColor, other.m_BaseColor) && Math::BitwiseEqual(m_Roughness, other.m_Roughness) && Math::BitwiseEqual(m_AlphaCutoff, other.m_AlphaCutoff);
        }
    };

    using FoliageMaterialKey = u32;
    inline constexpr FoliageMaterialKey kInvalidFoliageMaterialKey = ~0u;

    // One canonical plant.
    struct FoliageInstanceRecord
    {
        FoliageInstanceId m_Id = kInvalidFoliageInstanceId;
        FoliagePlacementKey m_Key;

        // Physical layer slot at generation time. Convenience for submission;
        // NOT identity — m_Key.m_LayerKey is.
        u32 m_LayerIndex = 0;
        // Row in the layer's instance VBO. A PROJECTION of the record, not
        // identity: it changes freely between regenerations and the id does
        // not. Nothing may key state on it.
        u32 m_BufferIndex = 0;
        u32 m_GroupIndex = 0;

        FoliageMaterialKey m_MaterialKey = kInvalidFoliageMaterialKey;

        // Transform, in TERRAIN-LOCAL space — the same space the instance VBO
        // rows are in (see FoliageRenderer::SetTerrainTransform). World space
        // is this through the owning terrain's transform.
        glm::vec3 m_Position{ 0.0f };
        f32 m_Scale = 1.0f;
        f32 m_Rotation = 0.0f; // Y-axis, radians
        f32 m_Height = 1.0f;

        // Terrain-local AABB for this one plant.
        BoundingBox m_LocalBounds;

        FoliageRepresentation m_Representation = FoliageRepresentation::MeshCard;
    };

    // A spatial bucket of instances within one layer.
    //
    // This slice only has to make the groups exist, be queryable and carry
    // correct bounds — #1240 / #1233 hang visibility and residency off them.
    // No culling decision is made here.
    //
    // Groups do not span layers: submission is per layer (one draw call per
    // layer's instance stream), so a group that spanned layers could never be
    // the unit a draw is culled or compacted against.
    struct FoliageSpatialGroup
    {
        u32 m_LayerIndex = 0;
        glm::ivec2 m_Cell{ 0 };
        BoundingBox m_LocalBounds;
        // m_LocalBounds through the terrain transform. Recomputed when the
        // transform changes; no instance is invalidated by a terrain move.
        BoundingBox m_WorldBounds;
        std::vector<FoliageInstanceId> m_Instances;
        u32 m_RepresentedCount = 0;
        u32 m_UnsupportedCount = 0;
    };

    // What one reconcile did. The counts are always maintained; the id lists
    // make the contract assertable (and are what a test reads).
    struct FoliageRegistryDelta
    {
        std::vector<FoliageInstanceId> m_Added;
        std::vector<FoliageInstanceId> m_Retired;
        u32 m_Survived = 0;
    };

    // Per-generation census, surfaced through GPU Scene diagnostics so foliage
    // that the canonical records cannot yet represent is visible rather than
    // absent from the scene inventory (issue #1230, third criterion).
    struct FoliageCensus
    {
        u32 m_CanonicalInstances = 0;
        u32 m_MeshCardInstances = 0;
        u32 m_ImpostorInstances = 0;
        // See FoliageRepresentation::Unsupported: 0 in a healthy scene today.
        u32 m_UnsupportedInstances = 0;
        // Layers whose AUTHORED representation could not be provided — today
        // that is UseImpostor with an atlas that failed to bake, which silently
        // fell back to a flat card. The instances still draw (as cards), so
        // they are not Unsupported; the VARIANT is.
        u32 m_UnsupportedVariants = 0;
        u32 m_SpatialGroups = 0;

        [[nodiscard]] auto operator==(const FoliageCensus&) const -> bool = default;
    };

    class FoliageInstanceRegistry
    {
      public:
        // Group edge length in terrain-local units. 16 m buckets a 100 m
        // island into ~40 groups per layer — coarse enough that the per-group
        // bookkeeping is negligible, fine enough to be a useful visibility
        // unit later. A constant rather than an authored field so this slice
        // adds no serialized state to FoliageComponent; #1240 can promote it
        // when it has a measurement to tune against.
        static constexpr f32 kGroupSize = 16.0f;

        // ── Population, driven by FoliageRenderer::GenerateInstances ──────
        //
        // BeginGeneration / (BeginLayer AddInstance* EndLayer)* / EndGeneration.
        // A layer that is skipped (disabled, zero density, removed) simply gets
        // no BeginLayer, and EndGeneration retires its instances.

        // Takes the FULL layer list, including layers that will not emit:
        // a layer's stable key is (name, ordinal-among-same-name), and
        // counting the ordinal over only the emitting layers made disabling
        // one of two same-named layers shift the other onto a different key,
        // silently retiring and reissuing every id it owned.
        void BeginGeneration(const std::vector<FoliageLayer>& layers);

        // spacing, worldSizeX and worldSizeZ are the cell -> XZ mapping;
        // placementSeed is the generator seed (today derived from the layer's
        // physical index). Together they are the placement signature.
        // layerIndex indexes the list handed to BeginGeneration.
        void BeginLayer(u32 layerIndex, const FoliageLayer& layer,
                        u32 placementSeed, f32 spacing, f32 worldSizeX, f32 worldSizeZ,
                        FoliageRepresentation representation, bool impostorUnavailable);

        // bufferIndex is the row this instance occupies in the layer's VBO.
        void AddInstance(u32 cellX, u32 cellZ, const FoliageInstanceData& row, u32 bufferIndex);

        void EndLayer();

        // Reconciles against the previous generation, builds spatial groups and
        // publishes the census and delta.
        void EndGeneration();

        // Every instance goes. Used when the owning component or renderer is
        // torn down, or a scene unloads. Ids are NOT reused afterwards.
        void Clear();

        // The owning terrain moved. Records are terrain-local, so nothing is
        // invalidated — only the cached world bounds change. A no-op when the
        // matrix is unchanged, because Scene calls this every frame.
        void SetTerrainTransform(const glm::mat4& transform);

        [[nodiscard]] const glm::mat4& GetTerrainTransform() const
        {
            return m_TerrainTransform;
        }

        // ── Queries ──────────────────────────────────────────────────────

        [[nodiscard]] const FoliageInstanceRecord* Find(FoliageInstanceId id) const;

        [[nodiscard]] bool IsLive(FoliageInstanceId id) const
        {
            return m_ById.contains(id);
        }

        // Issued once and no longer live. Cheap because ids are monotonic, so
        // no retired-set has to be kept.
        [[nodiscard]] bool IsRetired(FoliageInstanceId id) const
        {
            return id != kInvalidFoliageInstanceId && id < m_NextId && !IsLive(id);
        }

        // True for an id this registry has never issued.
        [[nodiscard]] bool WasIssued(FoliageInstanceId id) const
        {
            return id != kInvalidFoliageInstanceId && id < m_NextId;
        }

        [[nodiscard]] FoliageInstanceId FindByPlacement(const FoliagePlacementKey& key) const;

        // The id occupying a row of a layer's instance VBO, or invalid. The
        // reverse direction of m_BufferIndex — the only sanctioned way to get
        // from a buffer row back to identity.
        [[nodiscard]] FoliageInstanceId GetIdForBufferRow(u32 layerIndex, u32 row) const;

        [[nodiscard]] const std::vector<FoliageInstanceRecord>& GetRecords() const
        {
            return m_Records;
        }

        [[nodiscard]] const std::vector<FoliageSpatialGroup>& GetGroups() const
        {
            return m_Groups;
        }

        // Groups whose WORLD bounds intersect the query box. The hook #1240 /
        // #1233 will drive visibility and residency from.
        [[nodiscard]] std::vector<u32> FindGroupsInWorldBounds(const BoundingBox& query) const;

        [[nodiscard]] const FoliageMaterialDesc* GetMaterial(FoliageMaterialKey key) const;

        [[nodiscard]] const std::vector<FoliageMaterialDesc>& GetMaterials() const
        {
            return m_Materials;
        }

        [[nodiscard]] const FoliageCensus& GetCensus() const
        {
            return m_Census;
        }

        [[nodiscard]] const FoliageRegistryDelta& GetLastDelta() const
        {
            return m_LastDelta;
        }

        // Monotonic; bumped on every reconcile that changed anything.
        [[nodiscard]] u64 GetGeneration() const
        {
            return m_Generation;
        }

      private:
        // A spatial group's identity. A STRUCT rather than bits packed into a
        // u64: the packed form overlapped its own fields (a 32-bit cell index
        // shifted by 21 runs into the layer's bits), so two different cells
        // could collapse into one group. Full-key equality cannot.
        struct GroupKey
        {
            u32 m_LayerIndex = 0;
            i32 m_CellX = 0;
            i32 m_CellZ = 0;

            [[nodiscard]] auto operator==(const GroupKey&) const -> bool = default;
        };

        struct GroupKeyHash
        {
            [[nodiscard]] sizet operator()(const GroupKey& k) const noexcept
            {
                u64 h = k.m_LayerIndex;
                h ^= static_cast<u64>(static_cast<u32>(k.m_CellX)) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
                h ^= static_cast<u64>(static_cast<u32>(k.m_CellZ)) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
                return static_cast<sizet>(h);
            }
        };

        [[nodiscard]] u32 AcquireLayerKey(const std::string& name, u32 nameOrdinal);
        [[nodiscard]] FoliageMaterialKey InternMaterial(const FoliageLayer& layer);
        void RebuildGroups();
        void RecomputeWorldBounds();

        std::vector<FoliageInstanceRecord> m_Records;
        std::unordered_map<FoliageInstanceId, u32> m_ById;                                   // id -> index into m_Records
        std::unordered_map<FoliagePlacementKey, u32, FoliagePlacementKeyHash> m_ByPlacement; // -> index into m_Records
        std::vector<FoliageSpatialGroup> m_Groups;
        std::vector<FoliageMaterialDesc> m_Materials;

        // Row -> id, per physical layer slot.
        std::vector<std::vector<FoliageInstanceId>> m_BufferRows;

        // Stable layer keys, keyed on (name, ordinal-among-same-name) so a
        // reordered-but-unrenamed layer keeps its key while its placement
        // signature changes. Persists across generations.
        std::unordered_map<std::string, std::vector<u32>> m_LayerKeysByName;
        u32 m_NextLayerKey = 1;

        FoliageInstanceId m_NextId = 1; // 0 is kInvalidFoliageInstanceId
        u64 m_Generation = 0;

        glm::mat4 m_TerrainTransform{ 1.0f };

        FoliageCensus m_Census;
        FoliageRegistryDelta m_LastDelta;

        // ── Transient generation state ───────────────────────────────────
        // Last generation's placement -> id, and nothing else: survival only
        // needs the id, and a foliage system can hold tens of thousands of
        // records that would otherwise be copied wholesale every regenerate.
        std::unordered_map<FoliagePlacementKey, FoliageInstanceId, FoliagePlacementKeyHash> m_Previous;
        bool m_Generating = false;
        bool m_InLayer = false;
        // Per layer index, its ordinal among layers sharing its name.
        // Computed in BeginGeneration over the WHOLE list so an enabled layer's
        // key never depends on whether a sibling was skipped.
        std::vector<u32> m_OrdinalByLayerIndex;
        u32 m_CurrentLayerIndex = 0;
        FoliagePlacementKey m_CurrentKeyPrototype;
        FoliageMaterialKey m_CurrentMaterialKey = kInvalidFoliageMaterialKey;
        FoliageRepresentation m_CurrentRepresentation = FoliageRepresentation::MeshCard;
        u32 m_PendingUnsupportedVariants = 0;
    };
} // namespace OloEngine

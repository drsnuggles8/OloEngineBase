#include "OloEnginePCH.h"
#include "FoliageInstanceRegistry.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>

namespace OloEngine
{
    namespace
    {
        // Mixes a 64-bit value into a running hash. Used to build the placement
        // signature from the cell -> XZ mapping inputs.
        constexpr void HashCombine(u64& h, u64 value)
        {
            h ^= value + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        }

        // Floats enter the signature by BIT PATTERN, never by value compare —
        // two spacings that differ in the last ulp are two different placement
        // grids and must produce two different signatures.
        [[nodiscard]] u64 FloatBits(f32 value)
        {
            u32 bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            return static_cast<u64>(bits);
        }

        [[nodiscard]] u64 HashString(const std::string& text)
        {
            return static_cast<u64>(std::hash<std::string>{}(text));
        }

        // Everything about a plant that is not its identity, by bit pattern
        // (never float ==). Fed into FoliageInstanceRecord::m_StateHash.
        [[nodiscard]] u64 StateHash(const FoliageInstanceRecord& record, u64 materialHash)
        {
            u64 h = 0x84222325CBF29CE4ull;
            HashCombine(h, FloatBits(record.m_Position.x));
            HashCombine(h, FloatBits(record.m_Position.y));
            HashCombine(h, FloatBits(record.m_Position.z));
            HashCombine(h, FloatBits(record.m_Scale));
            HashCombine(h, FloatBits(record.m_Rotation));
            HashCombine(h, FloatBits(record.m_Height));
            HashCombine(h, static_cast<u64>(record.m_Representation));
            HashCombine(h, materialHash);
            return h;
        }

    } // namespace

    void FoliageInstanceRegistry::BeginGeneration(const std::vector<FoliageLayer>& layers)
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_ASSERT(!m_Generating, "FoliageInstanceRegistry: BeginGeneration without EndGeneration");

        m_Generating = true;
        m_InLayer = false;
        m_PendingUnsupportedVariants = 0;

        // Ordinals over the WHOLE list, so a skipped layer cannot shift a
        // sibling with the same name onto a different stable key.
        m_OrdinalByLayerIndex.assign(layers.size(), 0);
        {
            std::unordered_map<std::string, u32> seen;
            for (sizet i = 0; i < layers.size(); ++i)
            {
                m_OrdinalByLayerIndex[i] = seen[layers[i].Name]++;
            }
        }

        // The intern table is rebuilt per generation. Left to accumulate it
        // grew one dead descriptor per keystroke while a mesh or albedo path
        // was being typed in the inspector, and every generation scanned the
        // whole thing linearly.
        m_Materials.clear();

        // Everything live becomes a candidate for survival. Whatever is still
        // here at EndGeneration was not re-emitted, and retires.
        m_Previous.clear();
        m_Previous.reserve(m_Records.size());
        for (const auto& record : m_Records)
        {
            m_Previous.emplace(record.m_Key, PreviousRecord{ record.m_Id, record.m_StateHash });
        }

        m_Records.clear();
        m_ById.clear();
        m_ByPlacement.clear();
        m_BufferRows.clear();
        m_LastDelta.m_Added.clear();
        m_LastDelta.m_Retired.clear();
        m_LastDelta.m_Survived = 0;
        m_LastDelta.m_Updated = 0;
    }

    u32 FoliageInstanceRegistry::AcquireLayerKey(const std::string& name, u32 nameOrdinal)
    {
        auto& keys = m_LayerKeysByName[name];
        while (keys.size() <= nameOrdinal)
        {
            keys.push_back(m_NextLayerKey++);
        }
        return keys[nameOrdinal];
    }

    FoliageMaterialKey FoliageInstanceRegistry::InternMaterial(const FoliageLayer& layer)
    {
        FoliageMaterialDesc desc;
        desc.m_MeshPath = layer.MeshPath;
        desc.m_AlbedoPath = layer.AlbedoPath;
        desc.m_BaseColor = layer.BaseColor;
        desc.m_Roughness = layer.Roughness;
        desc.m_AlphaCutoff = layer.AlphaCutoff;

        for (sizet i = 0; i < m_Materials.size(); ++i)
        {
            if (m_Materials[i] == desc)
            {
                return static_cast<FoliageMaterialKey>(i);
            }
        }

        m_Materials.push_back(std::move(desc));
        return static_cast<FoliageMaterialKey>(m_Materials.size() - 1);
    }

    void FoliageInstanceRegistry::BeginLayer(u32 layerIndex, const FoliageLayer& layer,
                                             u32 placementSeed, f32 spacing, f32 worldSizeX, f32 worldSizeZ,
                                             FoliageRepresentation representation, bool variantUnavailable,
                                             const FoliageBoundsProfile& boundsProfile)
    {
        OLO_CORE_ASSERT(m_Generating, "FoliageInstanceRegistry: BeginLayer outside a generation");
        OLO_CORE_ASSERT(!m_InLayer, "FoliageInstanceRegistry: BeginLayer without EndLayer");

        m_InLayer = true;
        m_CurrentLayerIndex = layerIndex;
        m_CurrentRepresentation = representation;
        m_CurrentBoundsProfile = boundsProfile;
        m_CurrentMaterialKey = InternMaterial(layer);
        // The material's CONTENT, not its intern index — the table is rebuilt
        // every generation, so an index can move while the material did not.
        {
            u64 h = 0xA5A5F00DC0FFEE11ull;
            HashCombine(h, HashString(layer.MeshPath));
            HashCombine(h, HashString(layer.AlbedoPath));
            HashCombine(h, FloatBits(layer.BaseColor.r));
            HashCombine(h, FloatBits(layer.BaseColor.g));
            HashCombine(h, FloatBits(layer.BaseColor.b));
            HashCombine(h, FloatBits(layer.Roughness));
            HashCombine(h, FloatBits(layer.AlphaCutoff));
            // The bounds profile too, because AddInstance derives m_LocalBounds
            // from it: without this a placement that survives a regeneration
            // with a DIFFERENT profile changes its bounds while its state hash
            // does not, so the reconcile counts no update and GetGeneration()
            // does not advance — the one thing the generation counter promises.
            //
            // The narrow case it covers is a mesh edited in place: MeshPath is
            // already hashed above, and a mesh that appears or fails to load
            // flips m_Representation (also hashed), so only "same path, changed
            // geometry" reaches here. That is exactly the reimport path.
            HashCombine(h, FloatBits(boundsProfile.m_HalfExtentXZ));
            HashCombine(h, FloatBits(boundsProfile.m_HalfExtentXZHeightScaled));
            HashCombine(h, FloatBits(boundsProfile.m_MinY));
            HashCombine(h, FloatBits(boundsProfile.m_MaxY));
            m_CurrentMaterialHash = h;
        }

        const u32 ordinal = layerIndex < m_OrdinalByLayerIndex.size() ? m_OrdinalByLayerIndex[layerIndex] : 0;

        // The placement signature covers exactly the inputs that map a grid
        // cell to a position: the generator seed, the grid spacing and the
        // terrain extent. Slope gates and splatmap masking are NOT here — they
        // only decide WHETHER a cell emits, and a cell that stops emitting
        // retires through the reconcile on its own.
        //
        // The #1254 habitat rules and clump field follow that same rule and are
        // deliberately ABSENT: they gate emission and modulate scale, neither
        // of which moves a plant, so tightening a moisture band retires the
        // plants that fall outside it and leaves every survivor its id — which
        // is the incremental behaviour acceptance criterion 3 asks for.
        //
        // DecorrelatedVariation is the one #1254 field that IS here, because it
        // is the one that re-draws the jitter: flipping it moves every plant in
        // the layer, so every id in the layer has to retire.
        u64 signature = 0xCBF29CE484222325ull;
        HashCombine(signature, static_cast<u64>(placementSeed));
        HashCombine(signature, FloatBits(spacing));
        HashCombine(signature, FloatBits(worldSizeX));
        HashCombine(signature, FloatBits(worldSizeZ));
        HashCombine(signature, layer.DecorrelatedVariation ? 1ull : 0ull);

        m_CurrentKeyPrototype = FoliagePlacementKey{
            .m_LayerKey = AcquireLayerKey(layer.Name, ordinal),
            .m_PlacementSignature = signature,
            .m_CellX = 0,
            .m_CellZ = 0,
        };

        if (variantUnavailable)
        {
            ++m_PendingUnsupportedVariants;
        }

        if (m_BufferRows.size() <= layerIndex)
        {
            m_BufferRows.resize(static_cast<sizet>(layerIndex) + 1);
        }
    }

    void FoliageInstanceRegistry::AddInstance(u32 cellX, u32 cellZ, const FoliageInstanceData& row, u32 bufferIndex)
    {
        OLO_CORE_ASSERT(m_InLayer, "FoliageInstanceRegistry: AddInstance outside a layer");

        FoliagePlacementKey key = m_CurrentKeyPrototype;
        key.m_CellX = cellX;
        key.m_CellZ = cellZ;

        FoliageInstanceRecord record;
        record.m_Key = key;
        record.m_LayerIndex = m_CurrentLayerIndex;
        record.m_BufferIndex = bufferIndex;
        record.m_MaterialKey = m_CurrentMaterialKey;
        record.m_Representation = m_CurrentRepresentation;
        record.m_Position = glm::vec3(row.PositionScale.x, row.PositionScale.y, row.PositionScale.z);
        record.m_Scale = row.PositionScale.w;
        record.m_Rotation = row.RotationHeight.x;
        record.m_Height = row.RotationHeight.y;
        record.m_LocalBounds = FoliageInstanceBounds(record.m_Position, record.m_Scale, record.m_Height,
                                                     m_CurrentBoundsProfile);
        record.m_StateHash = StateHash(record, m_CurrentMaterialHash);

        // Survival: this placement existed last generation, so it keeps its id
        // and only its attributes (position, scale, bounds, representation,
        // material) may move — and if they did, that is an UPDATE the
        // generation counter has to reflect. Otherwise it is a new plant and
        // gets a fresh id.
        if (const auto it = m_Previous.find(key); it != m_Previous.end())
        {
            record.m_Id = it->second.m_Id;
            if (it->second.m_StateHash != record.m_StateHash)
            {
                ++m_LastDelta.m_Updated;
            }
            m_Previous.erase(it);
            ++m_LastDelta.m_Survived;
        }
        else
        {
            record.m_Id = m_NextId++;
            m_LastDelta.m_Added.push_back(record.m_Id);
        }

        auto& rows = m_BufferRows[m_CurrentLayerIndex];
        if (rows.size() <= bufferIndex)
        {
            rows.resize(static_cast<sizet>(bufferIndex) + 1, kInvalidFoliageInstanceId);
        }
        rows[bufferIndex] = record.m_Id;

        m_Records.push_back(record);
    }

    void FoliageInstanceRegistry::EndLayer()
    {
        OLO_CORE_ASSERT(m_InLayer, "FoliageInstanceRegistry: EndLayer without BeginLayer");
        m_InLayer = false;
    }

    void FoliageInstanceRegistry::EndGeneration()
    {
        OLO_PROFILE_FUNCTION();
        OLO_CORE_ASSERT(m_Generating, "FoliageInstanceRegistry: EndGeneration without BeginGeneration");
        OLO_CORE_ASSERT(!m_InLayer, "FoliageInstanceRegistry: EndGeneration inside a layer");

        // Whatever was not re-emitted is gone. Ids are monotonic, so these can
        // never be handed out again — retirement is permanent by construction.
        m_LastDelta.m_Retired.reserve(m_Previous.size());
        for (const auto& [key, previous] : m_Previous)
        {
            m_LastDelta.m_Retired.push_back(previous.m_Id);
        }
        m_Previous.clear();

        // Deterministic order so a delta is comparable run to run; the hash
        // map's iteration order is not.
        std::ranges::sort(m_LastDelta.m_Retired);
        std::ranges::sort(m_LastDelta.m_Added);

        m_ById.reserve(m_Records.size());
        m_ByPlacement.reserve(m_Records.size());
        for (u32 i = 0; i < static_cast<u32>(m_Records.size()); ++i)
        {
            const auto& record = m_Records[i];
            [[maybe_unused]] const auto idInserted = m_ById.emplace(record.m_Id, i).second;
            OLO_CORE_ASSERT(idInserted, "FoliageInstanceRegistry: duplicate instance id");
            [[maybe_unused]] const auto keyInserted = m_ByPlacement.emplace(record.m_Key, i).second;
            OLO_CORE_ASSERT(keyInserted, "FoliageInstanceRegistry: duplicate placement key in one generation");
        }

        RebuildGroups();

        m_Census = FoliageCensus{};
        m_Census.m_CanonicalInstances = static_cast<u32>(m_Records.size());
        for (const auto& record : m_Records)
        {
            switch (record.m_Representation)
            {
                case FoliageRepresentation::MeshCard:
                    ++m_Census.m_MeshCardInstances;
                    break;
                case FoliageRepresentation::Impostor:
                    ++m_Census.m_ImpostorInstances;
                    break;
                case FoliageRepresentation::AuthoredMesh:
                    ++m_Census.m_AuthoredMeshInstances;
                    break;
                case FoliageRepresentation::Unsupported:
                    ++m_Census.m_UnsupportedInstances;
                    break;
                case FoliageRepresentation::Count:
                    break;
            }
        }
        m_Census.m_UnsupportedVariants = m_PendingUnsupportedVariants;
        m_Census.m_SpatialGroups = static_cast<u32>(m_Groups.size());

        if (!m_LastDelta.m_Added.empty() || !m_LastDelta.m_Retired.empty() || m_LastDelta.m_Updated > 0)
        {
            ++m_Generation;
        }

        m_Generating = false;
    }

    void FoliageInstanceRegistry::RebuildGroups()
    {
        OLO_PROFILE_FUNCTION();

        m_Groups.clear();

        std::unordered_map<GroupKey, u32, GroupKeyHash> groupLookup;
        groupLookup.reserve(m_Records.size() / 8 + 1);

        for (auto& record : m_Records)
        {
            const auto cellX = static_cast<i32>(std::floor(record.m_Position.x / kGroupSize));
            const auto cellZ = static_cast<i32>(std::floor(record.m_Position.z / kGroupSize));

            const GroupKey key{ .m_LayerIndex = record.m_LayerIndex, .m_CellX = cellX, .m_CellZ = cellZ };

            auto it = groupLookup.find(key);
            if (it == groupLookup.end())
            {
                FoliageSpatialGroup group;
                group.m_LayerIndex = record.m_LayerIndex;
                group.m_Cell = glm::ivec2(cellX, cellZ);
                group.m_LocalBounds = record.m_LocalBounds;
                m_Groups.push_back(std::move(group));
                it = groupLookup.emplace(key, static_cast<u32>(m_Groups.size() - 1)).first;
            }
            else
            {
                m_Groups[it->second].m_LocalBounds = m_Groups[it->second].m_LocalBounds.Union(record.m_LocalBounds);
            }

            auto& group = m_Groups[it->second];
            group.m_Instances.push_back(record.m_Id);
            if (record.m_Representation == FoliageRepresentation::Unsupported)
            {
                ++group.m_UnsupportedCount;
            }
            else
            {
                ++group.m_RepresentedCount;
            }
            record.m_GroupIndex = it->second;
        }

        RecomputeWorldBounds();
    }

    void FoliageInstanceRegistry::RecomputeWorldBounds()
    {
        for (auto& group : m_Groups)
        {
            group.m_WorldBounds = group.m_LocalBounds.Transform(m_TerrainTransform);
        }
    }

    void FoliageInstanceRegistry::Clear()
    {
        // Idempotent: Scene calls this every frame that a foliage system is
        // switched off, and a no-op clear must not look like a reconcile. The
        // census is part of what must already be empty — returning before
        // resetting it left a switched-off system still reporting unsupported
        // variants it no longer has.
        if (m_Records.empty() && m_Groups.empty() && !m_Generating && m_Census == FoliageCensus{})
        {
            return;
        }

        m_Records.clear();
        m_ById.clear();
        m_ByPlacement.clear();
        m_Groups.clear();
        m_BufferRows.clear();
        m_Previous.clear();
        m_Generating = false;
        m_InLayer = false;

        m_LastDelta.m_Added.clear();
        m_LastDelta.m_Retired.clear();
        m_LastDelta.m_Survived = 0;
        m_LastDelta.m_Updated = 0;
        m_Census = FoliageCensus{};
        m_Materials.clear();
        m_OrdinalByLayerIndex.clear();

        // m_NextId and m_NextLayerKey deliberately NOT reset: an id this
        // registry once issued must never name a different plant, even after a
        // full teardown and rebuild.
        ++m_Generation;
    }

    void FoliageInstanceRegistry::SetTerrainTransform(const glm::mat4& transform)
    {
        // Scene calls this every frame. Records are terrain-local, so a moved
        // terrain invalidates nothing — only the cached world bounds change,
        // and only when the matrix actually differs.
        if (Math::BitwiseEqual(m_TerrainTransform, transform))
        {
            return;
        }

        m_TerrainTransform = transform;
        RecomputeWorldBounds();
    }

    const FoliageInstanceRecord* FoliageInstanceRegistry::Find(FoliageInstanceId id) const
    {
        const auto it = m_ById.find(id);
        return it == m_ById.end() ? nullptr : &m_Records[it->second];
    }

    FoliageInstanceId FoliageInstanceRegistry::FindByPlacement(const FoliagePlacementKey& key) const
    {
        const auto it = m_ByPlacement.find(key);
        return it == m_ByPlacement.end() ? kInvalidFoliageInstanceId : m_Records[it->second].m_Id;
    }

    FoliageInstanceId FoliageInstanceRegistry::GetIdForBufferRow(u32 layerIndex, u32 row) const
    {
        if (layerIndex >= m_BufferRows.size())
        {
            return kInvalidFoliageInstanceId;
        }
        const auto& rows = m_BufferRows[layerIndex];
        return row < rows.size() ? rows[row] : kInvalidFoliageInstanceId;
    }

    const FoliageMaterialDesc* FoliageInstanceRegistry::GetMaterial(FoliageMaterialKey key) const
    {
        return key < m_Materials.size() ? &m_Materials[key] : nullptr;
    }

    std::vector<u32> FoliageInstanceRegistry::FindGroupsInWorldBounds(const BoundingBox& query) const
    {
        std::vector<u32> result;
        for (u32 i = 0; i < static_cast<u32>(m_Groups.size()); ++i)
        {
            const auto& bounds = m_Groups[i].m_WorldBounds;
            const bool disjoint = bounds.Max.x < query.Min.x || bounds.Min.x > query.Max.x || bounds.Max.y < query.Min.y || bounds.Min.y > query.Max.y || bounds.Max.z < query.Min.z || bounds.Min.z > query.Max.z;
            if (!disjoint)
            {
                result.push_back(i);
            }
        }
        return result;
    }
} // namespace OloEngine

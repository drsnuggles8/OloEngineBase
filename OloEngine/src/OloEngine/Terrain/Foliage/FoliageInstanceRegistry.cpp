#include "OloEnginePCH.h"
#include "FoliageInstanceRegistry.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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

        [[nodiscard]] BoundingBox InstanceBounds(const glm::vec3& position, f32 scale, f32 height)
        {
            // Matches the extent FoliageRenderer already uses for its per-layer AABB:
            // the card stands ON the sampled ground point and spans h*s upward,
            // with ~0.5*s of horizontal half-extent either side.
            const glm::vec3 lo = position - glm::vec3(0.5f * scale, 0.0f, 0.5f * scale);
            const glm::vec3 hi = position + glm::vec3(0.5f * scale, height * scale, 0.5f * scale);
            return BoundingBox(lo, hi);
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
            m_Previous.emplace(record.m_Key, record.m_Id);
        }

        m_Records.clear();
        m_ById.clear();
        m_ByPlacement.clear();
        m_BufferRows.clear();
        m_LastDelta.m_Added.clear();
        m_LastDelta.m_Retired.clear();
        m_LastDelta.m_Survived = 0;
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
                                             FoliageRepresentation representation, bool impostorUnavailable)
    {
        OLO_CORE_ASSERT(m_Generating, "FoliageInstanceRegistry: BeginLayer outside a generation");
        OLO_CORE_ASSERT(!m_InLayer, "FoliageInstanceRegistry: BeginLayer without EndLayer");

        m_InLayer = true;
        m_CurrentLayerIndex = layerIndex;
        m_CurrentRepresentation = representation;
        m_CurrentMaterialKey = InternMaterial(layer);

        const u32 ordinal = layerIndex < m_OrdinalByLayerIndex.size() ? m_OrdinalByLayerIndex[layerIndex] : 0;

        // The placement signature covers exactly the inputs that map a grid
        // cell to a position: the generator seed, the grid spacing and the
        // terrain extent. Slope gates and splatmap masking are NOT here — they
        // only decide WHETHER a cell emits, and a cell that stops emitting
        // retires through the reconcile on its own.
        u64 signature = 0xCBF29CE484222325ull;
        HashCombine(signature, static_cast<u64>(placementSeed));
        HashCombine(signature, FloatBits(spacing));
        HashCombine(signature, FloatBits(worldSizeX));
        HashCombine(signature, FloatBits(worldSizeZ));

        m_CurrentKeyPrototype = FoliagePlacementKey{
            .m_LayerKey = AcquireLayerKey(layer.Name, ordinal),
            .m_PlacementSignature = signature,
            .m_CellX = 0,
            .m_CellZ = 0,
        };

        if (impostorUnavailable)
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
        record.m_LocalBounds = InstanceBounds(record.m_Position, record.m_Scale, record.m_Height);

        // Survival: this placement existed last generation, so it keeps its id
        // and only its attributes (position, scale, bounds, representation)
        // move. Otherwise it is a new plant and gets a fresh id.
        if (const auto it = m_Previous.find(key); it != m_Previous.end())
        {
            record.m_Id = it->second;
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
        for (const auto& [key, id] : m_Previous)
        {
            m_LastDelta.m_Retired.push_back(id);
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
                case FoliageRepresentation::Unsupported:
                    ++m_Census.m_UnsupportedInstances;
                    break;
                case FoliageRepresentation::Count:
                    break;
            }
        }
        m_Census.m_UnsupportedVariants = m_PendingUnsupportedVariants;
        m_Census.m_SpatialGroups = static_cast<u32>(m_Groups.size());

        if (!m_LastDelta.m_Added.empty() || !m_LastDelta.m_Retired.empty())
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

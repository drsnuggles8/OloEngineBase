#include "OloEnginePCH.h"
#include "OloEngine/Terrain/Voxel/VoxelOverride.h"
#include "OloEngine/Terrain/TerrainData.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <tuple>

namespace OloEngine
{
    struct VoxelRun
    {
        f32 first;
        u16 second;
    };

    void VoxelOverride::Initialize(f32 worldSizeX, f32 worldSizeZ, f32 heightScale, f32 voxelSize)
    {
        OLO_PROFILE_FUNCTION();

        m_WorldSizeX = worldSizeX;
        m_WorldSizeZ = worldSizeZ;
        m_HeightScale = heightScale;
        // NaN must be rejected explicitly: `NaN <= 0.0f` is FALSE, so a
        // non-finite size sails through a sign check and poisons every derived
        // quantity - chunkWorldSize, GetChunkBounds, and the per-chunk model
        // matrix in VoxelGreedyMeshBuilder::UploadMesh - which renders as the
        // whole volume silently vanishing. Reachable: TerrainComponent's
        // m_VoxelSize round-trips through save-games, and that loader's
        // sanitize block does not cover it (the scene YAML path does).
        if (!std::isfinite(voxelSize) || voxelSize <= 0.0f)
        {
            OLO_CORE_WARN("VoxelOverride::Initialize: Invalid voxelSize {}, clamping to 0.5", voxelSize);
            voxelSize = 0.5f;
        }
        m_VoxelSize = voxelSize;
        m_Chunks.clear();
    }

    void VoxelOverride::CarveSphere(const glm::vec3& center, f32 radius)
    {
        OLO_PROFILE_FUNCTION();

        m_AutoSeeded = false;

        TArray<VoxelCoord> affectedChunks;
        GetChunksInSphere(center, radius, affectedChunks);

        for (const auto& coord : affectedChunks)
        {
            auto& chunk = GetOrCreateChunk(coord);

            for (u32 z = 0; z < VoxelChunk::CHUNK_SIZE; ++z)
            {
                for (u32 y = 0; y < VoxelChunk::CHUNK_SIZE; ++y)
                {
                    for (u32 x = 0; x < VoxelChunk::CHUNK_SIZE; ++x)
                    {
                        glm::vec3 worldPos = VoxelToWorld(coord, x, y, z);
                        f32 dist = glm::length(worldPos - center) - radius;
                        // Carve = make empty (take max with positive value)
                        chunk.At(x, y, z) = std::max(chunk.At(x, y, z), -dist);
                    }
                }
            }
            chunk.Dirty = true;
        }
    }

    void VoxelOverride::AddSphere(const glm::vec3& center, f32 radius)
    {
        OLO_PROFILE_FUNCTION();

        m_AutoSeeded = false;

        TArray<VoxelCoord> affectedChunks;
        GetChunksInSphere(center, radius, affectedChunks);

        for (const auto& coord : affectedChunks)
        {
            auto& chunk = GetOrCreateChunk(coord);

            for (u32 z = 0; z < VoxelChunk::CHUNK_SIZE; ++z)
            {
                for (u32 y = 0; y < VoxelChunk::CHUNK_SIZE; ++y)
                {
                    for (u32 x = 0; x < VoxelChunk::CHUNK_SIZE; ++x)
                    {
                        glm::vec3 worldPos = VoxelToWorld(coord, x, y, z);
                        f32 dist = glm::length(worldPos - center) - radius;
                        // Add = make solid (take min with negative value)
                        chunk.At(x, y, z) = std::min(chunk.At(x, y, z), dist);
                    }
                }
            }
            chunk.Dirty = true;
        }
    }

    void VoxelOverride::InitializeChunkFromHeightmap(const VoxelCoord& coord, const TerrainData& terrainData,
                                                     f32 worldSizeX, f32 worldSizeZ, f32 heightScale)
    {
        OLO_PROFILE_FUNCTION();

        if (worldSizeX <= 0.0f || worldSizeZ <= 0.0f)
        {
            OLO_CORE_WARN("VoxelOverride::InitializeChunkFromHeightmap: Invalid world size ({}, {})", worldSizeX, worldSizeZ);
            return;
        }

        auto& chunk = GetOrCreateChunk(coord);

        for (u32 z = 0; z < VoxelChunk::CHUNK_SIZE; ++z)
        {
            for (u32 y = 0; y < VoxelChunk::CHUNK_SIZE; ++y)
            {
                for (u32 x = 0; x < VoxelChunk::CHUNK_SIZE; ++x)
                {
                    glm::vec3 worldPos = VoxelToWorld(coord, x, y, z);
                    f32 normalizedX = std::clamp(worldPos.x / worldSizeX, 0.0f, 1.0f);
                    f32 normalizedZ = std::clamp(worldPos.z / worldSizeZ, 0.0f, 1.0f);
                    f32 terrainHeight = terrainData.GetHeightAt(normalizedX, normalizedZ) * heightScale;

                    // SDF: negative below surface (solid), positive above (empty)
                    chunk.At(x, y, z) = worldPos.y - terrainHeight;
                }
            }
        }
        chunk.Dirty = true;
    }

    void VoxelOverride::SeedFromHeightmap(const TerrainData& terrainData, f32 worldSizeX, f32 worldSizeZ,
                                          f32 heightScale, u32 maxChunks)
    {
        OLO_PROFILE_FUNCTION();

        if (worldSizeX <= 0.0f || worldSizeZ <= 0.0f || heightScale <= 0.0f)
        {
            OLO_CORE_WARN("VoxelOverride::SeedFromHeightmap: invalid extent ({}, {}, {})",
                          worldSizeX, worldSizeZ, heightScale);
            return;
        }

        const f32 chunkWorldSize = static_cast<f32>(VoxelChunk::CHUNK_SIZE) * m_VoxelSize;
        auto chunkSpan = [chunkWorldSize](f32 extent)
        {
            return std::max(1, static_cast<i32>(std::ceil(extent / chunkWorldSize)));
        };

        const i32 spanX = chunkSpan(worldSizeX);
        const i32 spanY = chunkSpan(heightScale);
        const i32 spanZ = chunkSpan(worldSizeZ);

        const u64 requested = static_cast<u64>(spanX) * static_cast<u64>(spanY) * static_cast<u64>(spanZ);
        if (requested > maxChunks)
        {
            OLO_CORE_WARN("VoxelOverride::SeedFromHeightmap: {} chunks requested, capping at {}. "
                          "Raise VoxelSize or shrink the terrain extent for a full fill.",
                          requested, maxChunks);
        }

        u32 filled = 0;
        for (i32 cy = 0; cy < spanY && filled < maxChunks; ++cy)
        {
            for (i32 cz = 0; cz < spanZ && filled < maxChunks; ++cz)
            {
                for (i32 cx = 0; cx < spanX && filled < maxChunks; ++cx)
                {
                    const VoxelCoord coord{ cx, cy, cz };
                    InitializeChunkFromHeightmap(coord, terrainData, worldSizeX, worldSizeZ, heightScale);
                    PaintDepthStrata(coord);
                    ++filled;
                }
            }
        }
        m_AutoSeeded = filled > 0;
    }

    void VoxelOverride::PaintDepthStrata(const VoxelCoord& coord)
    {
        // Classic surface / subsoil / bedrock banding. Material indices match
        // the terrain layer array order (and the shader's fallback palette):
        // 0 stone, 1 dirt, 2 grass.
        //
        // Depth comes from the SDF value, not from walking the column: right
        // after InitializeChunkFromHeightmap the stored value IS
        // `worldY - terrainHeight`, so -value is the world-space depth below
        // the surface. Walking the column instead would paint the top voxel of
        // every chunk as grass, including chunks buried entirely underground.
        auto it = m_Chunks.find(coord);
        if (it == m_Chunks.end())
        {
            return;
        }

        VoxelChunk& chunk = it->second;
        constexpr u32 S = VoxelChunk::CHUNK_SIZE;

        const f32 surfaceBand = m_VoxelSize * 1.5f;
        const f32 subsoilBand = m_VoxelSize * 5.0f;

        for (u32 z = 0; z < S; ++z)
        {
            for (u32 y = 0; y < S; ++y)
            {
                for (u32 x = 0; x < S; ++x)
                {
                    const f32 signedDistance = chunk.At(x, y, z);
                    if (signedDistance >= 0.0f)
                    {
                        continue; // empty
                    }

                    const f32 depth = -signedDistance;
                    const u8 material = (depth <= surfaceBand) ? u8{ 2 } : (depth <= subsoilBand ? u8{ 1 } : u8{ 0 });
                    chunk.SetMaterialAt(x, y, z, material);
                }
            }
        }
        chunk.Dirty = true;
    }

    VoxelChunk& VoxelOverride::GetOrCreateChunk(const VoxelCoord& coord)
    {
        auto [it, inserted] = m_Chunks.try_emplace(coord, VoxelChunk{});
        return it->second;
    }

    bool VoxelOverride::HasChunk(const VoxelCoord& coord) const
    {
        return m_Chunks.contains(coord);
    }

    namespace
    {
        i32 FloorDivide(i32 numerator, i32 denominator)
        {
            OLO_CORE_ASSERT(denominator > 0, "Voxel grid divisor must be positive");
            const i32 quotient = numerator / denominator;
            const i32 remainder = numerator % denominator;
            return remainder < 0 ? quotient - 1 : quotient;
        }

        u32 PositiveModulo(i32 value, i32 modulus)
        {
            const i32 remainder = value % modulus;
            return static_cast<u32>(remainder < 0 ? remainder + modulus : remainder);
        }
    } // namespace

    VoxelCoord VoxelOverride::GridToChunkCoord(const VoxelGridCoord& voxel) const
    {
        constexpr i32 chunkSize = static_cast<i32>(VoxelChunk::CHUNK_SIZE);
        return { FloorDivide(voxel.X, chunkSize), FloorDivide(voxel.Y, chunkSize), FloorDivide(voxel.Z, chunkSize) };
    }

    VoxelGridCoord VoxelOverride::ChunkToGridCoord(const VoxelCoord& chunk, u32 lx, u32 ly, u32 lz) const
    {
        return {
            chunk.X * static_cast<i32>(VoxelChunk::CHUNK_SIZE) + static_cast<i32>(lx),
            chunk.Y * static_cast<i32>(VoxelChunk::CHUNK_SIZE) + static_cast<i32>(ly),
            chunk.Z * static_cast<i32>(VoxelChunk::CHUNK_SIZE) + static_cast<i32>(lz)
        };
    }

    f32 VoxelOverride::GetVoxelSDF(const VoxelGridCoord& voxel) const
    {
        const auto it = m_Chunks.find(GridToChunkCoord(voxel));
        if (it == m_Chunks.end())
            return 1.0f;
        constexpr i32 chunkSize = static_cast<i32>(VoxelChunk::CHUNK_SIZE);
        return it->second.At(PositiveModulo(voxel.X, chunkSize), PositiveModulo(voxel.Y, chunkSize), PositiveModulo(voxel.Z, chunkSize));
    }

    u8 VoxelOverride::GetVoxelMaterial(const VoxelGridCoord& voxel) const
    {
        const auto it = m_Chunks.find(GridToChunkCoord(voxel));
        if (it == m_Chunks.end())
            return 0;
        constexpr i32 chunkSize = static_cast<i32>(VoxelChunk::CHUNK_SIZE);
        return it->second.MaterialAt(PositiveModulo(voxel.X, chunkSize), PositiveModulo(voxel.Y, chunkSize), PositiveModulo(voxel.Z, chunkSize));
    }

    void VoxelOverride::SetVoxel(const VoxelGridCoord& voxel, f32 sdf, u8 material)
    {
        constexpr i32 chunkSize = static_cast<i32>(VoxelChunk::CHUNK_SIZE);
        // A brush writes this per cell across its whole radius, so the chunk
        // coordinate, the three local indices and the owning chunk are each
        // resolved once and reused. Going through MarkVoxelAndNeighboursDirty
        // would redo all of it plus a second hash lookup for a chunk already
        // in hand.
        const VoxelCoord chunkCoord = GridToChunkCoord(voxel);
        const u32 lx = PositiveModulo(voxel.X, chunkSize);
        const u32 ly = PositiveModulo(voxel.Y, chunkSize);
        const u32 lz = PositiveModulo(voxel.Z, chunkSize);
        VoxelChunk& chunk = GetOrCreateChunk(chunkCoord);
        chunk.At(lx, ly, lz) = sdf;
        chunk.SetMaterialAt(lx, ly, lz, material);
        chunk.Dirty = true;
        MarkNeighbourChunksDirty(chunkCoord, lx, ly, lz);
        m_AutoSeeded = false;
    }

    Ref<VoxelOverride> VoxelOverride::Clone() const
    {
        OLO_PROFILE_FUNCTION();

        auto copy = Ref<VoxelOverride>::Create();
        copy->m_Chunks = m_Chunks;
        copy->m_VoxelSize = m_VoxelSize;
        copy->m_WorldSizeX = m_WorldSizeX;
        copy->m_WorldSizeZ = m_WorldSizeZ;
        copy->m_HeightScale = m_HeightScale;
        copy->m_AutoSeeded = m_AutoSeeded;
        copy->MarkAllChunksDirty();
        return copy;
    }

    void VoxelOverride::MarkAllChunksDirty()
    {
        for (auto& entry : m_Chunks)
            entry.second.Dirty = true;
    }

    void VoxelOverride::MarkVoxelAndNeighboursDirty(const VoxelGridCoord& voxel)
    {
        constexpr i32 chunkSize = static_cast<i32>(VoxelChunk::CHUNK_SIZE);
        const VoxelCoord chunkCoord = GridToChunkCoord(voxel);
        if (auto it = m_Chunks.find(chunkCoord); it != m_Chunks.end())
            it->second.Dirty = true;

        MarkNeighbourChunksDirty(chunkCoord, PositiveModulo(voxel.X, chunkSize), PositiveModulo(voxel.Y, chunkSize),
                                 PositiveModulo(voxel.Z, chunkSize));
    }

    void VoxelOverride::MarkNeighbourChunksDirty(const VoxelCoord& chunkCoord, u32 lx, u32 ly, u32 lz)
    {
        constexpr i32 chunkSize = static_cast<i32>(VoxelChunk::CHUNK_SIZE);
        const i32 local[] = { static_cast<i32>(lx), static_cast<i32>(ly), static_cast<i32>(lz) };
        for (i32 axis = 0; axis < 3; ++axis)
        {
            if (local[axis] != 0 && local[axis] != chunkSize - 1)
                continue;
            VoxelCoord neighbour = chunkCoord;
            if (axis == 0)
                neighbour.X += local[axis] == 0 ? -1 : 1;
            else if (axis == 1)
                neighbour.Y += local[axis] == 0 ? -1 : 1;
            else
                neighbour.Z += local[axis] == 0 ? -1 : 1;
            if (auto it = m_Chunks.find(neighbour); it != m_Chunks.end())
                it->second.Dirty = true;
        }
    }

    void VoxelOverride::GetDirtyChunks(TArray<VoxelCoord>& outCoords) const
    {
        OLO_PROFILE_FUNCTION();

        outCoords.Reset();
        for (const auto& [coord, chunk] : m_Chunks)
        {
            if (chunk.Dirty)
            {
                outCoords.Add(coord);
            }
        }
    }

    void VoxelOverride::MarkChunkClean(const VoxelCoord& coord)
    {
        auto it = m_Chunks.find(coord);
        if (it != m_Chunks.end())
        {
            it->second.Dirty = false;
        }
    }

    VoxelCoord VoxelOverride::WorldToChunkCoord(const glm::vec3& worldPos) const
    {
        f32 chunkWorldSize = static_cast<f32>(VoxelChunk::CHUNK_SIZE) * m_VoxelSize;
        return {
            static_cast<i32>(std::floor(worldPos.x / chunkWorldSize)),
            static_cast<i32>(std::floor(worldPos.y / chunkWorldSize)),
            static_cast<i32>(std::floor(worldPos.z / chunkWorldSize))
        };
    }

    glm::vec3 VoxelOverride::VoxelToWorld(const VoxelCoord& chunkCoord, u32 lx, u32 ly, u32 lz) const
    {
        f32 chunkWorldSize = static_cast<f32>(VoxelChunk::CHUNK_SIZE) * m_VoxelSize;
        return {
            static_cast<f32>(chunkCoord.X) * chunkWorldSize + (static_cast<f32>(lx) + 0.5f) * m_VoxelSize,
            static_cast<f32>(chunkCoord.Y) * chunkWorldSize + (static_cast<f32>(ly) + 0.5f) * m_VoxelSize,
            static_cast<f32>(chunkCoord.Z) * chunkWorldSize + (static_cast<f32>(lz) + 0.5f) * m_VoxelSize
        };
    }

    BoundingBox VoxelOverride::GetChunkBounds(const VoxelCoord& coord) const
    {
        f32 chunkWorldSize = static_cast<f32>(VoxelChunk::CHUNK_SIZE) * m_VoxelSize;
        glm::vec3 minCorner(
            static_cast<f32>(coord.X) * chunkWorldSize,
            static_cast<f32>(coord.Y) * chunkWorldSize,
            static_cast<f32>(coord.Z) * chunkWorldSize);
        return { minCorner, minCorner + glm::vec3(chunkWorldSize) };
    }

    void VoxelOverride::GetChunksInSphere(const glm::vec3& center, f32 radius, TArray<VoxelCoord>& outCoords) const
    {
        OLO_PROFILE_FUNCTION();

        VoxelCoord minCoord = WorldToChunkCoord(center - glm::vec3(radius));
        VoxelCoord maxCoord = WorldToChunkCoord(center + glm::vec3(radius));

        outCoords.Reset();
        for (i32 cz = minCoord.Z; cz <= maxCoord.Z; ++cz)
        {
            for (i32 cy = minCoord.Y; cy <= maxCoord.Y; ++cy)
            {
                for (i32 cx = minCoord.X; cx <= maxCoord.X; ++cx)
                {
                    outCoords.Add({ cx, cy, cz });
                }
            }
        }
    }

    // ── RLE Serialization ────────────────────────────────────────────────
    //
    // Format (RLEMagic / RLEVersion, VoxelOverride.h):
    //   [4 bytes: 'VOX1'][4 bytes: version][4 bytes: chunk count]
    //   Per chunk:
    //     [12 bytes: VoxelCoord (X, Y, Z as i32)]
    //     [4 bytes: run count]
    //     Per run:
    //       [4 bytes: f32 value]
    //       [2 bytes: u16 count]
    //     [1 byte: material-data-present][32768 material bytes if present]
    // The reader accepts exactly RLEVersion; there is no headerless variant.

    TArray<u8> VoxelOverride::SerializeRLE() const
    {
        OLO_PROFILE_FUNCTION();

        TArray<u8> data;

        auto writeI32 = [&data](i32 v)
        { data.Append(reinterpret_cast<const u8*>(&v), 4); };
        auto writeU16 = [&data](u16 v)
        { data.Append(reinterpret_cast<const u8*>(&v), 2); };
        auto writeF32 = [&data](f32 v)
        { data.Append(reinterpret_cast<const u8*>(&v), 4); };

        writeI32(RLEMagic);
        writeI32(RLEVersion);
        u32 chunkCount = static_cast<u32>(m_Chunks.size());
        writeI32(static_cast<i32>(chunkCount));

        // Coordinate order, not hash-map order: the same volume must encode to the
        // same bytes, or every scene save rewrites its voxel payload (#1566).
        TArray<VoxelCoord> order;
        order.Reserve(static_cast<TArray<VoxelCoord>::SizeType>(m_Chunks.size()));
        for (const auto& entry : m_Chunks)
            order.Add(entry.first);
        std::ranges::sort(std::span(order.GetData(), static_cast<sizet>(order.Num())),
                          [](const VoxelCoord& a, const VoxelCoord& b)
                          { return std::tie(a.Z, a.Y, a.X) < std::tie(b.Z, b.Y, b.X); });

        for (const VoxelCoord& coord : order)
        {
            const VoxelChunk& chunk = m_Chunks.at(coord);
            writeI32(coord.X);
            writeI32(coord.Y);
            writeI32(coord.Z);

            // RLE encode the SDF data
            TArray<VoxelRun> runs;
            if (!chunk.SDFData.IsEmpty())
            {
                f32 currentVal = chunk.SDFData[0];
                u16 runLen = 1;

                for (sizet i = 1; i < chunk.SDFData.Num(); ++i)
                {
                    // Use exact equality for RLE (SDF values are stored precisely)
                    if (chunk.SDFData[i] == currentVal && runLen < 65535)
                    {
                        ++runLen;
                    }
                    else
                    {
                        runs.Add({ currentVal, runLen });
                        currentVal = chunk.SDFData[i];
                        runLen = 1;
                    }
                }
                runs.Add({ currentVal, runLen });
            }

            writeI32(static_cast<i32>(runs.Num()));
            for (const auto& [val, count] : runs)
            {
                writeF32(val);
                writeU16(count);
            }

            data.Add(chunk.MaterialData.IsEmpty() ? u8{ 0 } : u8{ 1 });
            if (!chunk.MaterialData.IsEmpty())
                data.Append(chunk.MaterialData);
        }

        return data;
    }

    bool VoxelOverride::DeserializeRLE(const TArray<u8>& data)
    {
        OLO_PROFILE_FUNCTION();

        if (data.Num() < 12)
        {
            OLO_CORE_ERROR("VoxelOverride::DeserializeRLE: {} bytes is too short for the VOX1 header", data.Num());
            return false;
        }

        sizet offset = 0;
        auto readI32 = [&data, &offset]() -> i32
        { i32 v; std::memcpy(&v, &data[offset], 4); offset += 4; return v; };
        auto readU16 = [&data, &offset]() -> u16
        { u16 v; std::memcpy(&v, &data[offset], 2); offset += 2; return v; };
        auto readF32 = [&data, &offset]() -> f32
        { f32 v; std::memcpy(&v, &data[offset], 4); offset += 4; return v; };

        if (const i32 magic = readI32(); magic != RLEMagic)
        {
            OLO_CORE_ERROR("VoxelOverride::DeserializeRLE: missing 'VOX1' magic (read 0x{:08X}); "
                           "the blob is not a voxel override, re-save the voxel edits",
                           static_cast<u32>(magic));
            return false;
        }
        if (const i32 version = readI32(); version != RLEVersion)
        {
            OLO_CORE_ERROR("VoxelOverride::DeserializeRLE: format v{} is not supported (this build reads v{} only); "
                           "re-save the voxel edits",
                           version, RLEVersion);
            return false;
        }
        const i32 rawChunkCount = readI32();
        if (rawChunkCount < 0)
            return false;
        u32 chunkCount = static_cast<u32>(rawChunkCount);

        // Build into a temp map so m_Chunks stays intact on parse failure
        std::unordered_map<VoxelCoord, VoxelChunk, VoxelCoordHash> tempChunks;

        for (u32 ci = 0; ci < chunkCount; ++ci)
        {
            if (offset + 16 > data.Num())
            {
                return false;
            }

            VoxelCoord coord;
            coord.X = readI32();
            coord.Y = readI32();
            coord.Z = readI32();
            // Grid coordinates and six-neighbour meshing use signed i32.
            // Leave one chunk of headroom for neighbour arithmetic.
            constexpr i32 maxChunk = std::numeric_limits<i32>::max() / static_cast<i32>(VoxelChunk::CHUNK_SIZE) - 1;
            if (coord.X < -maxChunk || coord.X > maxChunk || coord.Y < -maxChunk || coord.Y > maxChunk ||
                coord.Z < -maxChunk || coord.Z > maxChunk)
                return false;

            i32 runCount = readI32();
            if (runCount < 0 || static_cast<u32>(runCount) > VoxelChunk::TOTAL_VOXELS)
                return false;

            u32 runCountU = static_cast<u32>(runCount);
            auto [it, inserted] = tempChunks.try_emplace(coord, VoxelChunk{});
            if (!inserted)
                return false;
            auto& chunk = it->second;
            sizet idx = 0;

            for (u32 ri = 0; ri < runCountU; ++ri)
            {
                if (offset + 6 > data.Num())
                {
                    return false;
                }

                f32 val = readF32();
                if (!std::isfinite(val))
                    return false;
                u16 count = readU16();

                if (idx + count > VoxelChunk::TOTAL_VOXELS)
                    return false;

                for (u16 j = 0; j < count; ++j)
                {
                    chunk.SDFData[idx++] = val;
                }
            }

            if (idx != VoxelChunk::TOTAL_VOXELS)
                return false;

            if (offset + 1 > data.Num())
                return false;
            const bool materialPresent = data[offset++] != 0;
            if (materialPresent)
            {
                if (offset + VoxelChunk::TOTAL_VOXELS > data.Num())
                    return false;
                chunk.MaterialData.Reset();
                chunk.MaterialData.Append(data.GetData() + offset, VoxelChunk::TOTAL_VOXELS);
                offset += VoxelChunk::TOTAL_VOXELS;
            }

            chunk.Dirty = true;
        }

        if (offset != data.Num())
        {
            OLO_CORE_ERROR("VoxelOverride::DeserializeRLE: {} trailing bytes after {} chunks", data.Num() - offset, chunkCount);
            return false;
        }

        m_Chunks = std::move(tempChunks);
        m_AutoSeeded = false;
        return true;
    }
} // namespace OloEngine

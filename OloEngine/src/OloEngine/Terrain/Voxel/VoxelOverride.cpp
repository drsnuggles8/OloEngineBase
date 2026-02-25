#include "OloEnginePCH.h"
#include "OloEngine/Terrain/Voxel/VoxelOverride.h"
#include "OloEngine/Terrain/TerrainData.h"

#include <cmath>
#include <cstring>

namespace OloEngine
{
    void VoxelOverride::Initialize(f32 worldSizeX, f32 worldSizeZ, f32 heightScale, f32 voxelSize)
    {
        m_WorldSizeX = worldSizeX;
        m_WorldSizeZ = worldSizeZ;
        m_HeightScale = heightScale;
        m_VoxelSize = voxelSize;
        m_Chunks.clear();
    }

    void VoxelOverride::CarveSphere(const glm::vec3& center, f32 radius)
    {
        OLO_PROFILE_FUNCTION();

        std::vector<VoxelCoord> affectedChunks;
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

        std::vector<VoxelCoord> affectedChunks;
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

        auto& chunk = GetOrCreateChunk(coord);

        if (worldSizeX <= 0.0f || worldSizeZ <= 0.0f)
        {
            OLO_CORE_WARN("VoxelOverride::InitializeChunkFromHeightmap: Invalid world size ({}, {})", worldSizeX, worldSizeZ);
            return;
        }

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

    VoxelChunk& VoxelOverride::GetOrCreateChunk(const VoxelCoord& coord)
    {
        auto [it, inserted] = m_Chunks.try_emplace(coord, VoxelChunk{});
        return it->second;
    }

    bool VoxelOverride::HasChunk(const VoxelCoord& coord) const
    {
        return m_Chunks.contains(coord);
    }

    void VoxelOverride::GetDirtyChunks(std::vector<VoxelCoord>& outCoords) const
    {
        outCoords.clear();
        for (const auto& [coord, chunk] : m_Chunks)
        {
            if (chunk.Dirty)
            {
                outCoords.push_back(coord);
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

    void VoxelOverride::GetChunksInSphere(const glm::vec3& center, f32 radius, std::vector<VoxelCoord>& outCoords) const
    {
        VoxelCoord minCoord = WorldToChunkCoord(center - glm::vec3(radius));
        VoxelCoord maxCoord = WorldToChunkCoord(center + glm::vec3(radius));

        outCoords.clear();
        for (i32 cz = minCoord.Z; cz <= maxCoord.Z; ++cz)
        {
            for (i32 cy = minCoord.Y; cy <= maxCoord.Y; ++cy)
            {
                for (i32 cx = minCoord.X; cx <= maxCoord.X; ++cx)
                {
                    outCoords.push_back({ cx, cy, cz });
                }
            }
        }
    }

    // ── RLE Serialization ────────────────────────────────────────────────
    // Format:
    //   [4 bytes: chunk count]
    //   Per chunk:
    //     [12 bytes: VoxelCoord (X, Y, Z as i32)]
    //     [4 bytes: run count]
    //     Per run:
    //       [4 bytes: f32 value]
    //       [2 bytes: u16 count]

    std::vector<u8> VoxelOverride::SerializeRLE() const
    {
        OLO_PROFILE_FUNCTION();

        std::vector<u8> data;

        auto writeI32 = [&](i32 v)
        { data.insert(data.end(), reinterpret_cast<const u8*>(&v), reinterpret_cast<const u8*>(&v) + 4); };
        auto writeU16 = [&](u16 v)
        { data.insert(data.end(), reinterpret_cast<const u8*>(&v), reinterpret_cast<const u8*>(&v) + 2); };
        auto writeF32 = [&](f32 v)
        { data.insert(data.end(), reinterpret_cast<const u8*>(&v), reinterpret_cast<const u8*>(&v) + 4); };

        u32 chunkCount = static_cast<u32>(m_Chunks.size());
        writeI32(static_cast<i32>(chunkCount));

        for (const auto& [coord, chunk] : m_Chunks)
        {
            writeI32(coord.X);
            writeI32(coord.Y);
            writeI32(coord.Z);

            // RLE encode the SDF data
            std::vector<std::pair<f32, u16>> runs;
            if (!chunk.SDFData.empty())
            {
                f32 currentVal = chunk.SDFData[0];
                u16 runLen = 1;

                for (sizet i = 1; i < chunk.SDFData.size(); ++i)
                {
                    // Use exact equality for RLE (SDF values are stored precisely)
                    if (chunk.SDFData[i] == currentVal && runLen < 65535)
                    {
                        ++runLen;
                    }
                    else
                    {
                        runs.push_back({ currentVal, runLen });
                        currentVal = chunk.SDFData[i];
                        runLen = 1;
                    }
                }
                runs.push_back({ currentVal, runLen });
            }

            writeI32(static_cast<i32>(runs.size()));
            for (const auto& [val, count] : runs)
            {
                writeF32(val);
                writeU16(count);
            }
        }

        return data;
    }

    bool VoxelOverride::DeserializeRLE(const std::vector<u8>& data)
    {
        OLO_PROFILE_FUNCTION();

        if (data.size() < 4)
        {
            return false;
        }

        sizet offset = 0;
        auto readI32 = [&]() -> i32
        { i32 v; std::memcpy(&v, &data[offset], 4); offset += 4; return v; };
        auto readU16 = [&]() -> u16
        { u16 v; std::memcpy(&v, &data[offset], 2); offset += 2; return v; };
        auto readF32 = [&]() -> f32
        { f32 v; std::memcpy(&v, &data[offset], 4); offset += 4; return v; };

        i32 rawChunkCount = readI32();
        if (rawChunkCount < 0)
            return false;
        u32 chunkCount = static_cast<u32>(rawChunkCount);
        m_Chunks.clear();

        for (u32 ci = 0; ci < chunkCount; ++ci)
        {
            if (offset + 16 > data.size())
            {
                return false;
            }

            VoxelCoord coord;
            coord.X = readI32();
            coord.Y = readI32();
            coord.Z = readI32();

            i32 runCount = readI32();
            if (runCount < 0 || static_cast<u32>(runCount) > VoxelChunk::TOTAL_VOXELS)
                return false;

            u32 runCountU = static_cast<u32>(runCount);
            auto& chunk = m_Chunks.emplace(coord, VoxelChunk{}).first->second;
            sizet idx = 0;

            for (u32 ri = 0; ri < runCountU; ++ri)
            {
                if (offset + 6 > data.size())
                {
                    return false;
                }

                f32 val = readF32();
                u16 count = readU16();

                for (u16 j = 0; j < count && idx < VoxelChunk::TOTAL_VOXELS; ++j)
                {
                    chunk.SDFData[idx++] = val;
                }
            }
            chunk.Dirty = true;
        }

        return true;
    }
} // namespace OloEngine

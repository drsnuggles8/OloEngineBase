#include "OloEnginePCH.h"
#include "OloEngine/Navigation/NavMesh.h"
#include "OloEngine/Core/Log.h"

#include <DetourNavMesh.h>

#include <cstring>

namespace OloEngine
{
    NavMesh::~NavMesh()
    {
        if (m_NavMesh)
        {
            dtFreeNavMesh(m_NavMesh);
            m_NavMesh = nullptr;
        }
    }

    NavMesh::NavMesh(NavMesh&& other) noexcept
        : m_NavMesh(other.m_NavMesh), m_Settings(other.m_Settings)
    {
        other.m_NavMesh = nullptr;
    }

    NavMesh& NavMesh::operator=(NavMesh&& other) noexcept
    {
        if (this != &other)
        {
            if (m_NavMesh)
                dtFreeNavMesh(m_NavMesh);
            m_NavMesh = other.m_NavMesh;
            m_Settings = other.m_Settings;
            other.m_NavMesh = nullptr;
        }
        return *this;
    }

    void NavMesh::SetDetourNavMesh(dtNavMesh* navMesh)
    {
        if (m_NavMesh)
            dtFreeNavMesh(m_NavMesh);
        m_NavMesh = navMesh;
    }

    i32 NavMesh::GetPolyCount() const
    {
        if (!m_NavMesh)
            return 0;

        const dtNavMesh* navMesh = m_NavMesh;
        i32 count = 0;
        for (i32 i = 0; i < navMesh->getMaxTiles(); ++i)
        {
            const dtMeshTile* tile = navMesh->getTile(i);
            if (tile && tile->header)
                count += tile->header->polyCount;
        }
        return count;
    }

    // --- Serialization helpers ---------------------------------------------------
    static constexpr u32 NAVMESH_FORMAT_VERSION = 1;

    template <typename T>
    static void WriteValue(std::vector<u8>& buf, const T& val)
    {
        const auto off = buf.size();
        buf.resize(off + sizeof(T));
        std::memcpy(buf.data() + off, &val, sizeof(T));
    }

    template <typename T>
    static bool ReadValue(const std::vector<u8>& buf, size_t& offset, T& val)
    {
        if (offset + sizeof(T) > buf.size())
            return false;
        std::memcpy(&val, buf.data() + offset, sizeof(T));
        offset += sizeof(T);
        return true;
    }

    bool NavMesh::Serialize(std::vector<u8>& outData) const
    {
        OLO_PROFILE_FUNCTION();

        if (!m_NavMesh)
            return false;

        outData.clear();

        // Version header
        WriteValue(outData, NAVMESH_FORMAT_VERSION);

        // Settings — explicit per-field
        WriteValue(outData, m_Settings.CellSize);
        WriteValue(outData, m_Settings.CellHeight);
        WriteValue(outData, m_Settings.AgentRadius);
        WriteValue(outData, m_Settings.AgentHeight);
        WriteValue(outData, m_Settings.AgentMaxClimb);
        WriteValue(outData, m_Settings.AgentMaxSlope);
        WriteValue(outData, m_Settings.RegionMinSize);
        WriteValue(outData, m_Settings.RegionMergeSize);
        WriteValue(outData, m_Settings.EdgeMaxLen);
        WriteValue(outData, m_Settings.EdgeMaxError);
        WriteValue(outData, m_Settings.VertsPerPoly);
        WriteValue(outData, m_Settings.DetailSampleDist);
        WriteValue(outData, m_Settings.DetailSampleMaxError);

        // Persist dtNavMeshParams so Deserialize can reconstruct exactly
        const dtNavMeshParams* navParams = m_NavMesh->getParams();
        WriteValue(outData, navParams->orig[0]);
        WriteValue(outData, navParams->orig[1]);
        WriteValue(outData, navParams->orig[2]);
        WriteValue(outData, navParams->tileWidth);
        WriteValue(outData, navParams->tileHeight);
        WriteValue(outData, navParams->maxTiles);
        WriteValue(outData, navParams->maxPolys);

        // Serialize each tile
        const dtNavMesh* navMesh = m_NavMesh;
        for (i32 i = 0; i < navMesh->getMaxTiles(); ++i)
        {
            const dtMeshTile* tile = navMesh->getTile(i);
            if (!tile || !tile->header || tile->dataSize == 0)
                continue;

            const auto tileRef = navMesh->getTileRef(tile);

            WriteValue(outData, tileRef);
            WriteValue(outData, tile->dataSize);

            const auto off = outData.size();
            outData.resize(off + static_cast<size_t>(tile->dataSize));
            std::memcpy(outData.data() + off, tile->data, static_cast<size_t>(tile->dataSize));
        }

        return true;
    }

    bool NavMesh::Deserialize(const std::vector<u8>& data)
    {
        OLO_PROFILE_FUNCTION();

        size_t offset = 0;

        // Version header
        u32 version = 0;
        if (!ReadValue(data, offset, version) || version != NAVMESH_FORMAT_VERSION)
            return false;

        // Settings — per-field
        NavMeshSettings settings;
        if (!ReadValue(data, offset, settings.CellSize)) return false;
        if (!ReadValue(data, offset, settings.CellHeight)) return false;
        if (!ReadValue(data, offset, settings.AgentRadius)) return false;
        if (!ReadValue(data, offset, settings.AgentHeight)) return false;
        if (!ReadValue(data, offset, settings.AgentMaxClimb)) return false;
        if (!ReadValue(data, offset, settings.AgentMaxSlope)) return false;
        if (!ReadValue(data, offset, settings.RegionMinSize)) return false;
        if (!ReadValue(data, offset, settings.RegionMergeSize)) return false;
        if (!ReadValue(data, offset, settings.EdgeMaxLen)) return false;
        if (!ReadValue(data, offset, settings.EdgeMaxError)) return false;
        if (!ReadValue(data, offset, settings.VertsPerPoly)) return false;
        if (!ReadValue(data, offset, settings.DetailSampleDist)) return false;
        if (!ReadValue(data, offset, settings.DetailSampleMaxError)) return false;
        m_Settings = settings;

        // Restore dtNavMeshParams
        dtNavMeshParams params{};
        if (!ReadValue(data, offset, params.orig[0])) return false;
        if (!ReadValue(data, offset, params.orig[1])) return false;
        if (!ReadValue(data, offset, params.orig[2])) return false;
        if (!ReadValue(data, offset, params.tileWidth)) return false;
        if (!ReadValue(data, offset, params.tileHeight)) return false;
        if (!ReadValue(data, offset, params.maxTiles)) return false;
        if (!ReadValue(data, offset, params.maxPolys)) return false;

        auto* navMesh = dtAllocNavMesh();
        if (!navMesh)
            return false;

        if (dtStatusFailed(navMesh->init(&params)))
        {
            dtFreeNavMesh(navMesh);
            return false;
        }

        while (offset + sizeof(dtTileRef) + sizeof(i32) <= data.size())
        {
            dtTileRef tileRef{};
            if (!ReadValue(data, offset, tileRef)) break;

            i32 dataSize = 0;
            if (!ReadValue(data, offset, dataSize)) break;

            if (dataSize <= 0 || offset + static_cast<size_t>(dataSize) > data.size())
            {
                dtFreeNavMesh(navMesh);
                return false;
            }

            auto* tileData = static_cast<u8*>(dtAlloc(static_cast<size_t>(dataSize), DT_ALLOC_PERM));
            if (!tileData)
            {
                dtFreeNavMesh(navMesh);
                return false;
            }

            std::memcpy(tileData, data.data() + offset, static_cast<size_t>(dataSize));
            offset += static_cast<size_t>(dataSize);

            dtStatus status = navMesh->addTile(tileData, dataSize, DT_TILE_FREE_DATA, tileRef, nullptr);
            if (dtStatusFailed(status))
            {
                dtFree(tileData);
                dtFreeNavMesh(navMesh);
                return false;
            }
        }

        SetDetourNavMesh(navMesh);
        return true;
    }
} // namespace OloEngine

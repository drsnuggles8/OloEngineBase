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

    bool NavMesh::Serialize(std::vector<u8>& outData) const
    {
        if (!m_NavMesh)
            return false;

        // Write settings first
        outData.resize(sizeof(NavMeshSettings));
        std::memcpy(outData.data(), &m_Settings, sizeof(NavMeshSettings));

        // Serialize each tile
        const dtNavMesh* navMesh = m_NavMesh;
        for (i32 i = 0; i < navMesh->getMaxTiles(); ++i)
        {
            const dtMeshTile* tile = navMesh->getTile(i);
            if (!tile || !tile->header || tile->dataSize == 0)
                continue;

            const auto tileRef = navMesh->getTileRef(tile);
            const auto prevSize = outData.size();

            // Write tile ref, data size, then data
            outData.resize(prevSize + sizeof(dtTileRef) + sizeof(i32) + static_cast<size_t>(tile->dataSize));
            auto* ptr = outData.data() + prevSize;

            std::memcpy(ptr, &tileRef, sizeof(dtTileRef));
            ptr += sizeof(dtTileRef);

            i32 dataSize = tile->dataSize;
            std::memcpy(ptr, &dataSize, sizeof(i32));
            ptr += sizeof(i32);

            std::memcpy(ptr, tile->data, static_cast<size_t>(dataSize));
        }

        return true;
    }

    bool NavMesh::Deserialize(const std::vector<u8>& data)
    {
        if (data.size() < sizeof(NavMeshSettings))
            return false;

        std::memcpy(&m_Settings, data.data(), sizeof(NavMeshSettings));

        auto* navMesh = dtAllocNavMesh();
        if (!navMesh)
            return false;

        dtNavMeshParams params{};
        params.orig[0] = 0.0f;
        params.orig[1] = 0.0f;
        params.orig[2] = 0.0f;
        params.tileWidth = m_Settings.CellSize * 256.0f;
        params.tileHeight = m_Settings.CellSize * 256.0f;
        params.maxTiles = 128;
        params.maxPolys = 32768;

        if (dtStatusFailed(navMesh->init(&params)))
        {
            dtFreeNavMesh(navMesh);
            return false;
        }

        size_t offset = sizeof(NavMeshSettings);
        while (offset + sizeof(dtTileRef) + sizeof(i32) <= data.size())
        {
            dtTileRef tileRef{};
            std::memcpy(&tileRef, data.data() + offset, sizeof(dtTileRef));
            offset += sizeof(dtTileRef);

            i32 dataSize = 0;
            std::memcpy(&dataSize, data.data() + offset, sizeof(i32));
            offset += sizeof(i32);

            if (dataSize <= 0 || offset + static_cast<size_t>(dataSize) > data.size())
                break;

            auto* tileData = static_cast<u8*>(dtAlloc(static_cast<size_t>(dataSize), DT_ALLOC_PERM));
            if (!tileData)
                break;

            std::memcpy(tileData, data.data() + offset, static_cast<size_t>(dataSize));
            offset += static_cast<size_t>(dataSize);

            navMesh->addTile(tileData, dataSize, DT_TILE_FREE_DATA, tileRef, nullptr);
        }

        SetDetourNavMesh(navMesh);
        return true;
    }
} // namespace OloEngine

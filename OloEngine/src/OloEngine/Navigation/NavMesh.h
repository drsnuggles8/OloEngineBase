#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Navigation/NavMeshSettings.h"

#include <DetourNavMesh.h>

#include <vector>

namespace OloEngine
{
    class NavMesh : public Asset
    {
    public:
        NavMesh() = default;
        ~NavMesh() override;

        NavMesh(const NavMesh&) = delete;
        NavMesh& operator=(const NavMesh&) = delete;
        NavMesh(NavMesh&& other) noexcept;
        NavMesh& operator=(NavMesh&& other) noexcept;

        [[nodiscard]] dtNavMesh* GetDetourNavMesh() const { return m_NavMesh; }
        void SetDetourNavMesh(dtNavMesh* navMesh);

        [[nodiscard]] const NavMeshSettings& GetSettings() const { return m_Settings; }
        void SetSettings(const NavMeshSettings& settings) { m_Settings = settings; }

        [[nodiscard]] i32 GetPolyCount() const;
        [[nodiscard]] bool IsValid() const { return m_NavMesh != nullptr; }

        // Binary serialization for saving/loading baked navmesh
        [[nodiscard]] bool Serialize(std::vector<u8>& outData) const;
        [[nodiscard]] bool Deserialize(const std::vector<u8>& data);

        static AssetType GetStaticType() { return AssetType::NavMesh; }
        [[nodiscard]] AssetType GetAssetType() const override { return GetStaticType(); }

    private:
        dtNavMesh* m_NavMesh = nullptr;
        NavMeshSettings m_Settings;
    };
} // namespace OloEngine

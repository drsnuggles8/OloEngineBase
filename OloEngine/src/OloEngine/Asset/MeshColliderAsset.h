#pragma once

#include "OloEngine/Asset/Asset.h"
#include "OloEngine/Physics/ColliderMaterial.h"

#include <glm/glm.hpp>

namespace OloEngine
{
    enum class ECollisionComplexity : u8
    {
        Default = 0,              // Use simple for collision and complex for scene queries
        UseComplexAsSimple = 1,   // Use complex for collision AND scene queries
        UseSimpleAsComplex = 2    // Use simple for collision AND scene queries
    };

    class MeshColliderAsset : public Asset
    {
    public:
        AssetHandle m_ColliderMesh{0};
        ColliderMaterial m_Material{};
        bool m_EnableVertexWelding{true};
        f32 m_VertexWeldTolerance{0.1f};
        bool m_FlipNormals{false};
        bool m_CheckZeroAreaTriangles{true};
        f32 m_AreaTestEpsilon{0.06f};
        bool m_ShiftVerticesToOrigin{false};
        bool m_AlwaysShareShape{false};
        ECollisionComplexity m_CollisionComplexity{ECollisionComplexity::Default};
        glm::vec3 m_ColliderScale{1.0f};

        // Preview Settings (Only used in mesh collider editor)
        glm::vec3 m_PreviewScale{1.0f};

        MeshColliderAsset() = default;
        explicit MeshColliderAsset(AssetHandle colliderMesh) noexcept
            : m_ColliderMesh(colliderMesh), m_Material{}
        {
        }

        static constexpr AssetType GetStaticType() noexcept { return AssetType::MeshCollider; }
        AssetType GetAssetType() const noexcept override { return GetStaticType(); }
    };
}

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
        AssetHandle ColliderMesh = 0;
        ColliderMaterial Material;
        bool EnableVertexWelding = true;
        f32 VertexWeldTolerance = 0.1f;
        bool FlipNormals = false;
        bool CheckZeroAreaTriangles = true;
        f32 AreaTestEpsilon = 0.06f;
        bool ShiftVerticesToOrigin = false;
        bool AlwaysShareShape = false;
        ECollisionComplexity CollisionComplexity = ECollisionComplexity::Default;
        glm::vec3 ColliderScale = glm::vec3(1.0f);

        // Preview Settings (Only used in mesh collider editor)
        glm::vec3 PreviewScale = glm::vec3(1.0f);

        MeshColliderAsset() = default;
        MeshColliderAsset(AssetHandle colliderMesh, ColliderMaterial material = ColliderMaterial())
            : ColliderMesh(colliderMesh), Material(material)
        {
        }

        static AssetType GetStaticType() { return AssetType::MeshCollider; }
        virtual AssetType GetAssetType() const override { return GetStaticType(); }
    };
}

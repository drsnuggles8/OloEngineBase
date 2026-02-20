#pragma once

#include "OloEngine/Particle/EmissionShape.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"

#include <iterator>

namespace OloEngine
{
    // Build EmitMesh sampling data from a Mesh object
    inline void BuildEmitMeshFromMesh(EmitMesh& emitMesh, const Ref<Mesh>& mesh)
    {
        if (!mesh || !mesh->IsValid())
        {
            emitMesh.Build(nullptr, 0, nullptr, 0);
            return;
        }

        const auto& vertices = mesh->GetVertices();
        const auto& indices = mesh->GetIndices();

        std::vector<glm::vec3> positions(vertices.Num());
        for (i32 i = 0; i < vertices.Num(); ++i)
        {
            positions[i] = vertices[i].Position;
        }

        emitMesh.Build(positions.data(), static_cast<u32>(positions.size()),
                       indices.GetData(), static_cast<u32>(indices.Num()));
    }

    // Build EmitMesh from a primitive mesh type index
    // 0=Cube, 1=Sphere, 2=Cylinder, 3=Torus, 4=Icosphere, 5=Cone
    inline void BuildEmitMeshFromPrimitive(EmitMesh& emitMesh, i32 primitiveType)
    {
        Ref<Mesh> mesh;
        switch (primitiveType)
        {
            case 0:
                mesh = MeshPrimitives::CreateCube();
                break;
            case 1:
                mesh = MeshPrimitives::CreateSphere();
                break;
            case 2:
                mesh = MeshPrimitives::CreateCylinder();
                break;
            case 3:
                mesh = MeshPrimitives::CreateTorus();
                break;
            case 4:
                mesh = MeshPrimitives::CreateIcosphere();
                break;
            case 5:
                mesh = MeshPrimitives::CreateCone();
                break;
            default:
                primitiveType = 0;
                mesh = MeshPrimitives::CreateCube();
                break;
        }

        emitMesh.PrimitiveType = primitiveType;
        BuildEmitMeshFromMesh(emitMesh, mesh);
    }

    inline constexpr const char* EmitMeshPrimitiveNames[] = {
        "Cube", "Sphere", "Cylinder", "Torus", "Icosphere", "Cone"
    };
    inline constexpr i32 EmitMeshPrimitiveCount = static_cast<i32>(std::size(EmitMeshPrimitiveNames));
} // namespace OloEngine

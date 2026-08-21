#pragma once

// Shared procedural-mesh fixtures for the virtual-geometry tests (issue #813).
//
// One spelling of the icosphere generator instead of a per-file copy: tests
// whose comments claim "same fixture as X" must actually share the fixture, or
// an independent fix to one copy silently makes them test different meshes.
// (VirtualMeshBuilderTest keeps its own DELIBERATELY different fixtures —
// seamed/flat-normal variants exercising specific simplifier paths — and is
// not a consumer of this header.)
//
// Header-only and GL-free: MeshSource construction here is pure CPU, so unit
// and shaderpipe tests alike can include it headless.

#include "OloEngine/Containers/Array.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Templates/UnrealTemplate.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

namespace OloEngine::Tests::VirtualMeshFixtures
{
    // Icosphere with `subdivisions` rounds of 4-way triangle subdivision:
    // 0 -> 20 tris, 3 -> 1280 tris, 4 -> 5120 tris. Smooth normals (= the
    // position on a unit sphere), spherical UVs.
    inline Ref<MeshSource> MakeIcosphereMesh(u32 subdivisions)
    {
        const f32 t = (1.0f + std::sqrt(5.0f)) / 2.0f;
        std::vector<glm::vec3> positions = {
            { -1.0f, t, 0.0f },
            { 1.0f, t, 0.0f },
            { -1.0f, -t, 0.0f },
            { 1.0f, -t, 0.0f },
            { 0.0f, -1.0f, t },
            { 0.0f, 1.0f, t },
            { 0.0f, -1.0f, -t },
            { 0.0f, 1.0f, -t },
            { t, 0.0f, -1.0f },
            { t, 0.0f, 1.0f },
            { -t, 0.0f, -1.0f },
            { -t, 0.0f, 1.0f },
        };
        for (auto& p : positions)
        {
            p = glm::normalize(p);
        }
        std::vector<u32> indices = {
            0,
            11,
            5,
            0,
            5,
            1,
            0,
            1,
            7,
            0,
            7,
            10,
            0,
            10,
            11,
            1,
            5,
            9,
            5,
            11,
            4,
            11,
            10,
            2,
            10,
            7,
            6,
            7,
            1,
            8,
            3,
            9,
            4,
            3,
            4,
            2,
            3,
            2,
            6,
            3,
            6,
            8,
            3,
            8,
            9,
            4,
            9,
            5,
            2,
            4,
            11,
            6,
            2,
            10,
            8,
            6,
            7,
            9,
            8,
            1,
        };
        for (u32 s = 0; s < subdivisions; ++s)
        {
            std::map<std::pair<u32, u32>, u32> midpointCache;
            auto midpoint = [&](u32 a, u32 b) -> u32
            {
                std::pair<u32, u32> const key = std::minmax(a, b);
                if (auto it = midpointCache.find(key); it != midpointCache.end())
                {
                    return it->second;
                }
                auto index = static_cast<u32>(positions.size());
                positions.push_back(glm::normalize((positions[a] + positions[b]) * 0.5f));
                midpointCache.emplace(key, index);
                return index;
            };
            std::vector<u32> next;
            next.reserve(indices.size() * 4);
            for (sizet i = 0; i + 2 < indices.size(); i += 3)
            {
                u32 const a = indices[i];
                u32 const b = indices[i + 1];
                u32 const c = indices[i + 2];
                u32 const ab = midpoint(a, b);
                u32 const bc = midpoint(b, c);
                u32 const ca = midpoint(c, a);
                for (u32 idx : { a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca })
                {
                    next.push_back(idx);
                }
            }
            indices = std::move(next);
        }

        TArray<Vertex> vertices;
        vertices.Reserve(static_cast<i32>(positions.size()));
        constexpr f32 kPi = 3.14159265358979323846f;
        for (const glm::vec3& p : positions)
        {
            glm::vec2 const uv{ std::atan2(p.z, p.x) / (2.0f * kPi) + 0.5f,
                                std::asin(std::clamp(p.y, -1.0f, 1.0f)) / kPi + 0.5f };
            vertices.Add(Vertex(p, p, uv));
        }
        TArray<u32> meshIndices;
        meshIndices.Reserve(static_cast<i32>(indices.size()));
        for (u32 const index : indices)
        {
            meshIndices.Add(index);
        }
        return Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(meshIndices));
    }
} // namespace OloEngine::Tests::VirtualMeshFixtures

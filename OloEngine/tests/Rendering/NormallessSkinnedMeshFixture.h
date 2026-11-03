#pragma once

// Rigged glTFs with no NORMAL attribute, written on demand rather than shipped:
// every vertex is skinned to one identity joint, with explicit identity inverse
// bind matrices. Imported with shared normals a mesh keeps its vertex count;
// flat per-face normals split it into three vertices per triangle. Used by
// AnimatedModelImportSettingsTest (CPU), AnimatedModelNormalsImportTest (the
// full GL route) and SkinnedNormalsVisualEvidenceTest (rendered).

#include "OloEngine/Core/Base.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace NormallessSkinnedMesh
    {
        template<typename T>
        void Append(std::vector<char>& bytes, const T& value)
        {
            const auto* raw = reinterpret_cast<const char*>(&value);
            bytes.insert(bytes.end(), raw, raw + sizeof(T));
        }

        // Writes `<name>.gltf` + `<name>.bin` into `directory` and returns the .gltf.
        // Buffer layout: positions | indices (padded to 4 B) | joints | weights | inverse
        // bind matrix. The matrix is optional in glTF (identity by default), but Assimp
        // 6.0.5's glTF2Importer::ImportNode dereferences it unconditionally and crashes
        // on a skin without one, so the fixture carries it explicitly.
        inline std::filesystem::path Write(const std::filesystem::path& directory, const std::string& name,
                                           const std::vector<std::array<f32, 3>>& positions,
                                           const std::vector<u16>& indices)
        {
            const sizet vertexCount = positions.size();
            std::array<f32, 3> lo = positions.front();
            std::array<f32, 3> hi = positions.front();
            std::vector<char> buffer;
            for (const auto& position : positions)
            {
                for (sizet axis = 0; axis < 3; ++axis)
                {
                    lo[axis] = std::min(lo[axis], position[axis]);
                    hi[axis] = std::max(hi[axis], position[axis]);
                }
                Append(buffer, position);
            }
            const sizet indexOffset = buffer.size();
            for (u16 index : indices)
            {
                Append(buffer, index);
            }
            while (buffer.size() % 4 != 0)
            {
                buffer.push_back(0);
            }
            const sizet jointOffset = buffer.size();
            for (sizet i = 0; i < vertexCount; ++i)
            {
                Append(buffer, std::array<u8, 4>{ 0, 0, 0, 0 });
            }
            const sizet weightOffset = buffer.size();
            for (sizet i = 0; i < vertexCount; ++i)
            {
                Append(buffer, std::array<f32, 4>{ 1.0f, 0.0f, 0.0f, 0.0f });
            }
            const sizet matrixOffset = buffer.size();
            Append(buffer, std::array<f32, 16>{ 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                                0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f });

            std::ofstream(directory / (name + ".bin"), std::ios::binary)
                .write(buffer.data(), static_cast<std::streamsize>(buffer.size()));

            const auto view = [](sizet offset, sizet length, const char* target)
            {
                return "{ \"buffer\": 0, \"byteOffset\": " + std::to_string(offset) + ", \"byteLength\": " +
                       std::to_string(length) + target + " }";
            };
            const auto vec3 = [](const std::array<f32, 3>& v)
            {
                return "[ " + std::to_string(v[0]) + ", " + std::to_string(v[1]) + ", " + std::to_string(v[2]) + " ]";
            };
            const std::string count = std::to_string(vertexCount);
            const std::filesystem::path gltfPath = directory / (name + ".gltf");
            std::ofstream(gltfPath)
                << "{\n"
                << "  \"asset\": { \"version\": \"2.0\" },\n"
                << "  \"scene\": 0,\n"
                << "  \"scenes\": [ { \"nodes\": [ 0, 1 ] } ],\n"
                << "  \"nodes\": [ { \"name\": \"" << name << "\", \"mesh\": 0, \"skin\": 0 }, { \"name\": \"Root\" } ],\n"
                << "  \"skins\": [ { \"joints\": [ 1 ], \"inverseBindMatrices\": 4 } ],\n"
                << "  \"meshes\": [ { \"primitives\": [ { \"attributes\": { \"POSITION\": 0, \"JOINTS_0\": 2, \"WEIGHTS_0\": 3 }, \"indices\": 1 } ] } ],\n"
                << "  \"buffers\": [ { \"uri\": \"" << name << ".bin\", \"byteLength\": " << buffer.size() << " } ],\n"
                << "  \"bufferViews\": [\n"
                << "    " << view(0, indexOffset, ", \"target\": 34962") << ",\n"
                << "    " << view(indexOffset, indices.size() * sizeof(u16), ", \"target\": 34963") << ",\n"
                << "    " << view(jointOffset, weightOffset - jointOffset, ", \"target\": 34962") << ",\n"
                << "    " << view(weightOffset, matrixOffset - weightOffset, ", \"target\": 34962") << ",\n"
                << "    " << view(matrixOffset, 64, "") << "\n"
                << "  ],\n"
                << "  \"accessors\": [\n"
                << "    { \"bufferView\": 0, \"componentType\": 5126, \"count\": " << count
                << ", \"type\": \"VEC3\", \"min\": " << vec3(lo) << ", \"max\": " << vec3(hi) << " },\n"
                << "    { \"bufferView\": 1, \"componentType\": 5123, \"count\": " << indices.size() << ", \"type\": \"SCALAR\" },\n"
                << "    { \"bufferView\": 2, \"componentType\": 5121, \"count\": " << count << ", \"type\": \"VEC4\" },\n"
                << "    { \"bufferView\": 3, \"componentType\": 5126, \"count\": " << count << ", \"type\": \"VEC4\" },\n"
                << "    { \"bufferView\": 4, \"componentType\": 5126, \"count\": 1, \"type\": \"MAT4\" }\n"
                << "  ]\n"
                << "}\n";
            return gltfPath;
        }
    } // namespace NormallessSkinnedMesh

    // A flat 3x3 vertex grid (8 triangles) in the z = 0 plane, facing +Z.
    namespace NormallessSkinnedGrid
    {
        constexpr u32 kGridSide = 3;
        constexpr u32 kVertexCount = kGridSide * kGridSide;
        constexpr u32 kTriangleCount = (kGridSide - 1) * (kGridSide - 1) * 2;

        inline std::filesystem::path Write(const std::filesystem::path& directory)
        {
            std::vector<std::array<f32, 3>> positions;
            for (u32 row = 0; row < kGridSide; ++row)
            {
                for (u32 column = 0; column < kGridSide; ++column)
                {
                    positions.push_back({ static_cast<f32>(column), static_cast<f32>(row), 0.0f });
                }
            }
            std::vector<u16> indices;
            for (u32 row = 0; row + 1 < kGridSide; ++row)
            {
                for (u32 column = 0; column + 1 < kGridSide; ++column)
                {
                    // Counter-clockwise seen from +Z, so the generated normal is +Z.
                    const auto v = [](u32 r, u32 c)
                    { return static_cast<u16>(r * kGridSide + c); };
                    for (u16 index : { v(row, column), v(row, column + 1), v(row + 1, column + 1),
                                       v(row, column), v(row + 1, column + 1), v(row + 1, column) })
                    {
                        indices.push_back(index);
                    }
                }
            }
            return NormallessSkinnedMesh::Write(directory, "NormallessSkinnedGrid", positions, indices);
        }
    } // namespace NormallessSkinnedGrid

    // A closed unit UV sphere, y up, outward-facing, with one shared vertex per pole
    // and no seam: the curved surface where flat and smooth normals look different.
    namespace NormallessSkinnedSphere
    {
        constexpr u32 kSegments = 16;
        constexpr u32 kRings = 10;
        constexpr u32 kVertexCount = 2 + (kRings - 1) * kSegments;
        constexpr u32 kTriangleCount = 2 * kSegments * (kRings - 1);

        inline std::filesystem::path Write(const std::filesystem::path& directory)
        {
            std::vector<std::array<f32, 3>> positions;
            positions.push_back({ 0.0f, 1.0f, 0.0f });
            for (u32 ring = 1; ring < kRings; ++ring)
            {
                const f32 theta = glm::pi<f32>() * static_cast<f32>(ring) / static_cast<f32>(kRings);
                for (u32 segment = 0; segment < kSegments; ++segment)
                {
                    const f32 phi = glm::two_pi<f32>() * static_cast<f32>(segment) / static_cast<f32>(kSegments);
                    positions.push_back({ std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi) });
                }
            }
            positions.push_back({ 0.0f, -1.0f, 0.0f });

            const auto v = [](u32 ring, u32 segment)
            {
                if (ring == 0)
                    return static_cast<u16>(0);
                if (ring == kRings)
                    return static_cast<u16>(kVertexCount - 1);
                return static_cast<u16>(1 + (ring - 1) * kSegments + segment % kSegments);
            };
            std::vector<u16> indices;
            for (u32 ring = 0; ring < kRings; ++ring)
            {
                for (u32 segment = 0; segment < kSegments; ++segment)
                {
                    // Quad a-b-c-d, a top-left; (a, c, b) and (a, d, c) wind outward.
                    const u16 a = v(ring, segment);
                    const u16 b = v(ring + 1, segment);
                    const u16 c = v(ring + 1, segment + 1);
                    const u16 d = v(ring, segment + 1);
                    if (ring + 1 < kRings)
                        indices.insert(indices.end(), { a, c, b });
                    if (ring > 0)
                        indices.insert(indices.end(), { a, d, c });
                }
            }
            return NormallessSkinnedMesh::Write(directory, "NormallessSkinnedSphere", positions, indices);
        }
    } // namespace NormallessSkinnedSphere
} // namespace OloEngine::Tests

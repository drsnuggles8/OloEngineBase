#pragma once

// A rigged glTF with no NORMAL attribute, written on demand rather than shipped:
// a 3x3 vertex grid (8 triangles) in the z = 0 plane, skinned to one joint.
// Imported with shared normals it keeps 9 vertices; flat per-face normals split
// it into 24. Used by AnimatedModelImportSettingsTest (CPU) and
// AnimatedModelNormalsImportTest (the full GL route).

#include "OloEngine/Core/Base.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <vector>

namespace OloEngine::Tests::NormallessSkinnedGrid
{
    constexpr u32 kGridSide = 3;
    constexpr u32 kVertexCount = kGridSide * kGridSide;
    constexpr u32 kTriangleCount = (kGridSide - 1) * (kGridSide - 1) * 2;

    template<typename T>
    void Append(std::vector<char>& bytes, const T& value)
    {
        const auto* raw = reinterpret_cast<const char*>(&value);
        bytes.insert(bytes.end(), raw, raw + sizeof(T));
    }

    // Writes NormallessSkinnedGrid.gltf + .bin into `directory` and returns the .gltf.
    // Buffer layout: positions (108 B) | indices (48 B) | joints (36 B) | weights (144 B)
    // | inverse bind matrix (64 B). The matrix is optional in glTF (identity by default),
    // but Assimp 6.0.5's glTF2Importer::ImportNode dereferences it unconditionally and
    // crashes on a skin without one, so the fixture carries it explicitly.
    inline std::filesystem::path Write(const std::filesystem::path& directory)
    {
        std::vector<char> buffer;
        for (u32 row = 0; row < kGridSide; ++row)
        {
            for (u32 column = 0; column < kGridSide; ++column)
            {
                Append(buffer, std::array<f32, 3>{ static_cast<f32>(column), static_cast<f32>(row), 0.0f });
            }
        }
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
                    Append(buffer, index);
                }
            }
        }
        for (u32 i = 0; i < kVertexCount; ++i)
        {
            Append(buffer, std::array<u8, 4>{ 0, 0, 0, 0 });
        }
        for (u32 i = 0; i < kVertexCount; ++i)
        {
            Append(buffer, std::array<f32, 4>{ 1.0f, 0.0f, 0.0f, 0.0f });
        }
        Append(buffer, std::array<f32, 16>{ 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                            0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f });

        const std::filesystem::path binPath = directory / "NormallessSkinnedGrid.bin";
        std::ofstream(binPath, std::ios::binary).write(buffer.data(), static_cast<std::streamsize>(buffer.size()));

        const std::filesystem::path gltfPath = directory / "NormallessSkinnedGrid.gltf";
        std::ofstream(gltfPath) << R"json({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [ 0, 1 ] } ],
  "nodes": [
    { "name": "Grid", "mesh": 0, "skin": 0 },
    { "name": "Root" }
  ],
  "skins": [ { "joints": [ 1 ], "inverseBindMatrices": 4 } ],
  "meshes": [ { "primitives": [ {
    "attributes": { "POSITION": 0, "JOINTS_0": 2, "WEIGHTS_0": 3 },
    "indices": 1
  } ] } ],
  "buffers": [ { "uri": "NormallessSkinnedGrid.bin", "byteLength": 400 } ],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0, "byteLength": 108, "target": 34962 },
    { "buffer": 0, "byteOffset": 108, "byteLength": 48, "target": 34963 },
    { "buffer": 0, "byteOffset": 156, "byteLength": 36, "target": 34962 },
    { "buffer": 0, "byteOffset": 192, "byteLength": 144, "target": 34962 },
    { "buffer": 0, "byteOffset": 336, "byteLength": 64 }
  ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 9, "type": "VEC3", "min": [ 0, 0, 0 ], "max": [ 2, 2, 0 ] },
    { "bufferView": 1, "componentType": 5123, "count": 24, "type": "SCALAR" },
    { "bufferView": 2, "componentType": 5121, "count": 9, "type": "VEC4" },
    { "bufferView": 3, "componentType": 5126, "count": 9, "type": "VEC4" },
    { "bufferView": 4, "componentType": 5126, "count": 1, "type": "MAT4" }
  ]
})json";
        static_assert(kVertexCount * 12 + kTriangleCount * 6 + kVertexCount * 4 + kVertexCount * 16 + 64 == 400,
                      "the .gltf byteLength/offsets above assume this layout");
        return gltfPath;
    }
} // namespace OloEngine::Tests::NormallessSkinnedGrid

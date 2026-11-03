// OLO_TEST_LAYER: integration
// =============================================================================
// AnimatedModelNormalsImportTest — a rigged source without normals imports WELDED.
//
// The static import has generated shared smooth normals for a normal-less source
// since #653. The animated import kept aiProcess_GenNormals, which makes the mesh
// verbose to give every face its own normal, so a rigged normal-less source
// arrived as triangle soup: three vertices per triangle, no classic or
// virtual-geometry LOD for the skinned mesh (#1150), faceted shading.
//
// The fixture is written here, not shipped: a 3x3 vertex grid (8 triangles)
// skinned to one joint, with no NORMAL attribute. Welded it keeps 9 vertices;
// split it has 24. Needs a live GL context (AnimatedModel builds its
// MeshSource); skips cleanly without one.
// =============================================================================

#include "OloEnginePCH.h"
#include "ProcessRendererInit.h"

#include <gtest/gtest.h>

#include "TestTempDir.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "Rendering/PropertyTests/RenderPropertyTest.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

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
        fs::path WriteNormallessSkinnedGrid(const fs::path& directory)
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

            const fs::path binPath = directory / "NormallessSkinnedGrid.bin";
            std::ofstream(binPath, std::ios::binary).write(buffer.data(), static_cast<std::streamsize>(buffer.size()));

            const fs::path gltfPath = directory / "NormallessSkinnedGrid.gltf";
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
            EXPECT_EQ(buffer.size(), 400u) << "the .gltf byteLength/offsets above assume this layout";
            return gltfPath;
        }
    } // namespace

    TEST(AnimatedModelNormalsImport, ARiggedSourceWithoutNormalsImportsWeldedWithSmoothNormals)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        EnsureProcessRendererInitialized();
        GLStateGuard glGuard("AnimatedModelNormalsImport", GLStateGuard::Policy::Restore);

        const fs::path gltf = WriteNormallessSkinnedGrid(TempDir("grid"));
        const auto model = Ref<AnimatedModel>::Create(gltf.string());
        ASSERT_TRUE(model);
        ASSERT_TRUE(model->GetSkeleton()) << "the fixture is rigged and must take the animated route";
        ASSERT_EQ(model->GetMeshes().size(), 1u);

        const TArray<Vertex>& vertices = model->GetMeshes()[0]->GetVertices();
        EXPECT_EQ(vertices.Num(), static_cast<i32>(kVertexCount))
            << "flat normals split the grid into " << kTriangleCount * 3 << " vertices";
        for (const Vertex& vertex : vertices)
        {
            EXPECT_NEAR(vertex.Normal.x, 0.0f, 1e-4f);
            EXPECT_NEAR(vertex.Normal.y, 0.0f, 1e-4f);
            EXPECT_NEAR(vertex.Normal.z, 1.0f, 1e-4f);
        }
    }
} // namespace OloEngine::Tests

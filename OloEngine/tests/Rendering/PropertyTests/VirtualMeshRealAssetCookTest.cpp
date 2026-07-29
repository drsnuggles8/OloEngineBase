// OLO_TEST_LAYER: cullinglod
//
// The cluster-DAG cook on REAL content, which nothing else in the suite covers: every other
// VirtualMeshBuilder test builds a procedural quad/grid/icosphere. Sponza is the largest asset
// in the sandbox (262k triangles across 25 submeshes and 25 materials) and is the only fixture
// here with genuine UV seams, mixed submesh sizes, and imported-material boundaries.
//
// What it pins (issue #685): the largest real asset must cook into a DAG that can actually
// COARSEN. A part that comes back with a single level renders at full source density forever,
// in the main view and in every shadow cascade, and the DAG still looks structurally valid —
// that was issue #651, and it was silent. Only the genuinely tiny submeshes (a handful of
// triangles, below one cluster's worth) are allowed to be flat.
//
// It also prints cook time and DAG shape. That is deliberately NOT asserted — it is a
// hardware-dependent number with no baseline machinery behind it, so it is reported for the
// reader rather than enforced.
//
// The Sponza case needs a GL context (the Assimp import path builds GPU buffers) and SKIPs
// without one; the procedural case runs everywhere.

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h" // OLO_ENSURE_GPU_OR_SKIP

#include <gtest/gtest.h>

#include "OloEngine/Asset/Interchange/AssimpMeshImporter.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMesh.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshBuilder.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <string>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake"
#endif

namespace OloEngine::Tests
{
    namespace
    {
        std::filesystem::path AssetPath(const char* relative)
        {
            return std::filesystem::path{ OLO_TEST_EDITOR_ROOT } / "SandboxProject/Assets" / relative;
        }

        void CookAndReport(const char* label, const char* relative)
        {
            auto const path = AssetPath(relative);
            if (!std::filesystem::exists(path))
            {
                // Not a failure: some assets are deliberately not committed (too large for git
                // history, or a licence that asks users to fetch rather than redistribute). The
                // manifest requires consumers to SKIP naming the fetch command — a missing
                // optional asset is a setup step, not a broken build.
                GTEST_SKIP() << "optional asset not fetched: " << path.generic_string()
                             << "\n  fetch it with:  scripts/Fetch-Assets.ps1 -Tag nanite"
                             << "\n  (see scripts/assets/asset-manifest.json)";
            }

            AssimpMeshImporter importer;
            auto const imported = importer.Import(path);
            ASSERT_TRUE(imported.Source) << "import failed for " << path.string();

            const MeshSource& source = *imported.Source;
            u32 const sourceTriangles = static_cast<u32>(source.GetIndices().Num()) / 3u;

            auto const start = std::chrono::steady_clock::now();
            VirtualMeshSet const built = VirtualMeshBuilder::BuildSet(source);
            auto const ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();

            ASSERT_TRUE(built.IsValid()) << label << ": BuildSet produced no usable DAG";

            // A part with ONE level built no hierarchy: its cut can never coarsen, so virtual
            // geometry draws it at full source density from any distance (issue #651). That is
            // only legitimate when the submesh is too small to have a hierarchy in the first
            // place — below a couple of clusters' worth of triangles.
            constexpr u32 kMinTrianglesExpectedToBuildLevels = 4 * 128;

            u32 maxLevels = 0;
            u32 flatParts = 0;
            for (const VirtualMeshPart& part : built.Parts)
            {
                maxLevels = std::max(maxLevels, part.Dag.LevelCount);
                if (part.Dag.LevelCount <= 1)
                {
                    ++flatParts;
                    EXPECT_LE(part.Dag.SourceTriangleCount, kMinTrianglesExpectedToBuildLevels)
                        << label << ": submesh " << part.SubmeshIndex << " has "
                        << part.Dag.SourceTriangleCount
                        << " triangles but built a SINGLE-LEVEL DAG — it will render at full "
                           "source density forever, in the main view and every shadow cascade";
                }
            }

            auto const blob = VirtualMeshSerializer::SerializeSetToBlob(built);
            VirtualMeshSet reloaded;
            EXPECT_TRUE(VirtualMeshSerializer::DeserializeSetFromBlob(blob, reloaded))
                << label << ": the cooked blob does not round-trip through the reader";

            std::cout << "\n=== COOK " << label << " ===\n"
                      << "  source triangles : " << sourceTriangles << '\n'
                      << "  parts            : " << built.Parts.size() << '\n'
                      << "  clusters         : " << built.TotalClusters() << '\n'
                      << "  max DAG levels   : " << maxLevels << '\n'
                      << "  flat (1-level)   : " << flatParts << " of " << built.Parts.size() << '\n'
                      << "  blob size (KB)   : " << (blob.size() / 1024) << '\n'
                      << "  COOK MS          : " << ms << '\n'
                      << std::endl;
        }
    } // namespace

    namespace
    {
        // A dense grid whose every triangle owns its three vertices and carries the FLAT face
        // normal — the shape Assimp produces for any source without normals (aiProcess_
        // GenNormals splits every shared vertex; JoinIdenticalVertices cannot re-merge them
        // because the normals differ per face). Positions coincide exactly, so the surface is
        // closed geometrically while being a triangle soup in index space.
        //
        // This stands in for the issue #651 asset (Stanford xyzrgb_dragon), whose registry
        // entry survives in AssetRegistry.oar but whose .ply is not committed to the repo.
        Ref<MeshSource> MakeFlatNormalGrid(u32 gridSize)
        {
            TArray<Vertex> vertices;
            TArray<u32> indices;
            vertices.Reserve(static_cast<i32>(gridSize * gridSize * 6));
            indices.Reserve(static_cast<i32>(gridSize * gridSize * 6));

            auto positionAt = [gridSize](u32 x, u32 z)
            {
                auto fx = static_cast<f32>(x) / static_cast<f32>(gridSize);
                auto fz = static_cast<f32>(z) / static_cast<f32>(gridSize);
                auto fy = 0.08f * std::sin(fx * 11.0f) * std::cos(fz * 9.0f);
                return glm::vec3{ fx, fy, fz };
            };

            for (u32 z = 0; z < gridSize; ++z)
            {
                for (u32 x = 0; x < gridSize; ++x)
                {
                    const glm::vec3 p00 = positionAt(x, z);
                    const glm::vec3 p10 = positionAt(x + 1, z);
                    const glm::vec3 p01 = positionAt(x, z + 1);
                    const glm::vec3 p11 = positionAt(x + 1, z + 1);

                    for (const auto& tri : { std::array{ p00, p10, p01 }, std::array{ p10, p11, p01 } })
                    {
                        glm::vec3 const normal = glm::normalize(glm::cross(tri[1] - tri[0], tri[2] - tri[0]));
                        for (const glm::vec3& p : tri)
                        {
                            indices.Add(static_cast<u32>(vertices.Num()));
                            vertices.Add(Vertex(p, normal, { p.x, p.z }));
                        }
                    }
                }
            }

            return Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));
        }

        void CookSourceAndReport(const char* label, const MeshSource& source)
        {
            u32 const sourceTriangles = static_cast<u32>(source.GetIndices().Num()) / 3u;

            auto const start = std::chrono::steady_clock::now();
            VirtualMeshSet const built = VirtualMeshBuilder::BuildSet(source);
            auto const ms = std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();

            ASSERT_TRUE(built.IsValid()) << label << ": BuildSet produced no usable DAG";

            u32 maxLevels = 0;
            for (const VirtualMeshPart& part : built.Parts)
            {
                maxLevels = std::max(maxLevels, part.Dag.LevelCount);
            }

            std::cout << "\n=== COOK " << label << " ===\n"
                      << "  source triangles : " << sourceTriangles << '\n'
                      << "  vertices         : " << source.GetVertices().Num() << '\n'
                      << "  clusters         : " << built.TotalClusters() << '\n'
                      << "  max DAG levels   : " << maxLevels << '\n'
                      << "  COOK MS          : " << ms << '\n'
                      << std::endl;
        }
    } // namespace

    TEST(VirtualMeshRealAssetCook, Sponza)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        CookAndReport("Sponza", "Models/Sponza/Sponza.gltf");
    }

    // No GL needed: the source is procedural, so this runs everywhere.
    TEST(VirtualMeshRealAssetCook, FlatNormalGrid)
    {
        auto const source = MakeFlatNormalGrid(256); // 131,072 triangles, 393,216 split vertices
        CookSourceAndReport("flat-normal grid 256", *source);
    }

    // The issue #651 asset: a Stanford scan with no normals in the PLY, so Assimp hands over
    // flat per-face normals and the mesh arrives as an index-space triangle soup.
    TEST(VirtualMeshRealAssetCook, StanfordDragon)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        CookAndReport("xyzrgb_dragon", "Models/Stanford/xyzrgb_dragon.ply");
    }
} // namespace OloEngine::Tests

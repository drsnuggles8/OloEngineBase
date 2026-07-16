// OLO_TEST_LAYER: integration
//
// A model's GEOMETRY must be identical whether it was imported from the source file or
// served from the .omesh cache (issue #629).
//
// Model::CreateCombinedMeshSource duplicated the ENTIRE model once per submesh on a warm
// cache load. On the warm path every Mesh is a submesh *view* into one already-combined
// MeshSource — GetMeshSource() hands back that same shared source to all of them — but the
// concatenation loop copied each mesh's whole source. Sponza (25 submeshes) loaded as
// 4,812,300 vertices / 6.5M triangles instead of 192,492 / 262k, which stalled the
// virtual-geometry cook indefinitely.
//
// NOTHING in the suite noticed. The mesh count was unchanged, the material names were
// unchanged, every submesh still resolved the right material — only the geometry was 25x,
// and no test looked at the geometry across the cold/warm boundary. It could be reintroduced
// tomorrow and the whole suite would stay green.
//
// A note on what does NOT catch it: "the combined source's vertex count equals the sum of its
// submesh vertex counts" is asserted below because it is a real structural invariant, but it
// is NOT sufficient on its own — the duplication bug satisfies it (submesh i gets
// m_VertexCount = the WHOLE source's count and m_BaseVertex = i * that, so the sum still adds
// up to the inflated total). The load-bearing assertion is the exact COLD == WARM comparison.
//
// Needs a GL context (Model::LoadModel builds GPU buffers), so it SKIPs without a GPU.

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h" // OLO_ENSURE_GPU_OR_SKIP

#include <gtest/gtest.h>

#include "OloEngine/Asset/MeshCache.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Model.h"

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Tests
{
    namespace
    {
        std::filesystem::path ColoredCubesPath()
        {
            return std::filesystem::path{ OLO_TEST_EDITOR_ROOT } /
                   "assets/models/DeccerCubes/SM_Deccer_Cubes_Colored.glb";
        }

        // Everything about a combined MeshSource that a duplication (or a dropped/reordered
        // submesh, or a re-based index window) would move.
        struct GeometryFingerprint
        {
            u32 VertexCount = 0;
            u32 IndexCount = 0;
            u32 SubmeshCount = 0;
            std::vector<u32> BaseVertex;
            std::vector<u32> BaseIndex;
            std::vector<u32> SubmeshVertexCount;
            std::vector<u32> SubmeshIndexCount;

            [[nodiscard]] u32 TriangleCount() const
            {
                return IndexCount / 3u;
            }

            [[nodiscard]] std::string Describe() const
            {
                std::ostringstream out;
                out << VertexCount << " verts / " << IndexCount << " indices / " << TriangleCount() << " tris across "
                    << SubmeshCount << " submeshes";
                return out.str();
            }

            bool operator==(const GeometryFingerprint&) const = default;
        };

        GeometryFingerprint Fingerprint(const MeshSource& source)
        {
            GeometryFingerprint fp;
            fp.VertexCount = static_cast<u32>(source.GetVertices().Num());
            fp.IndexCount = static_cast<u32>(source.GetIndices().Num());
            const auto& submeshes = source.GetSubmeshes();
            fp.SubmeshCount = static_cast<u32>(submeshes.Num());
            for (i32 i = 0; i < submeshes.Num(); ++i)
            {
                fp.BaseVertex.push_back(submeshes[i].m_BaseVertex);
                fp.BaseIndex.push_back(submeshes[i].m_BaseIndex);
                fp.SubmeshVertexCount.push_back(submeshes[i].m_VertexCount);
                fp.SubmeshIndexCount.push_back(submeshes[i].m_IndexCount);
            }
            return fp;
        }
    } // namespace

    TEST(ModelWarmCacheGeometryIdentity, WarmCacheLoadProducesByteIdenticalGeometryToTheColdImport)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const std::filesystem::path path = ColoredCubesPath();
        ASSERT_TRUE(std::filesystem::exists(path)) << "deccer-cubes fixture missing: " << path.string();

        // ---- COLD: no cache. Model::LoadModel writes the .omesh as a side effect. ----
        MeshCache::InvalidateCache(path);
        ASSERT_FALSE(MeshCache::IsMeshCacheValid(path)) << "cache invalidation did not take effect";

        Model cold{ path.string() };
        ASSERT_GT(cold.GetMeshCount(), 1u) << "fixture must be MULTI-submesh or this test cannot see the bug "
                                              "(the duplication factor is the submesh count)";

        Ref<MeshSource> const coldCombined = cold.CreateCombinedMeshSource();
        ASSERT_TRUE(coldCombined);
        const GeometryFingerprint coldFp = Fingerprint(*coldCombined);
        ASSERT_GT(coldFp.VertexCount, 0u);
        ASSERT_EQ(coldFp.SubmeshCount, cold.GetMeshCount());

        // ---- WARM: the same file again, now served from the .omesh just written. ----
        ASSERT_TRUE(MeshCache::IsMeshCacheValid(path))
            << "the cold import did not write an .omesh — the warm half of this test would be vacuous";

        Model warm{ path.string() };
        ASSERT_EQ(warm.GetMeshCount(), cold.GetMeshCount()) << "warm load produced a different submesh count";

        Ref<MeshSource> const warmCombined = warm.CreateCombinedMeshSource();
        ASSERT_TRUE(warmCombined);
        const GeometryFingerprint warmFp = Fingerprint(*warmCombined);

        // The whole bug, in one comparison. Every other test in the suite stayed green while
        // this number was 25x too large.
        EXPECT_EQ(coldFp.VertexCount, warmFp.VertexCount)
            << "WARM cache load produced " << warmFp.Describe() << " but the COLD import produced "
            << coldFp.Describe() << ".\n"
            << "Ratio: " << (coldFp.VertexCount > 0 ? static_cast<f64>(warmFp.VertexCount) / static_cast<f64>(coldFp.VertexCount) : 0.0)
            << "x — a ratio equal to the SUBMESH COUNT means Model::CreateCombinedMeshSource is running its "
               "concatenation loop over the cached COMBINED source, copying the whole model once per submesh "
               "instead of returning the cached source as-is.";
        EXPECT_EQ(coldFp.IndexCount, warmFp.IndexCount);
        EXPECT_EQ(coldFp.TriangleCount(), warmFp.TriangleCount());
        EXPECT_EQ(coldFp, warmFp) << "cold and warm loads disagree about the submesh geometry windows "
                                     "(m_BaseVertex / m_BaseIndex / m_VertexCount / m_IndexCount)";

        // Structural invariant, on BOTH paths: the combined source is exactly the concatenation
        // of its submeshes — no vertex belongs to no submesh, and none belongs to two.
        // (Necessary, not sufficient: the duplication bug satisfies this. See the header.)
        for (const auto& [name, fp] : { std::pair{ "cold", coldFp }, std::pair{ "warm", warmFp } })
        {
            u32 vertexSum = 0;
            u32 indexSum = 0;
            for (u32 i = 0; i < fp.SubmeshCount; ++i)
            {
                EXPECT_EQ(fp.BaseVertex[i], vertexSum) << name << ": submesh " << i << " does not start where the "
                                                       << "previous one ended — the submesh windows overlap or gap";
                EXPECT_EQ(fp.BaseIndex[i], indexSum) << name << ": submesh " << i << " index window is misplaced";
                vertexSum += fp.SubmeshVertexCount[i];
                indexSum += fp.SubmeshIndexCount[i];
            }
            EXPECT_EQ(vertexSum, fp.VertexCount)
                << name << ": the combined source holds " << fp.VertexCount << " vertices but its submeshes only "
                << "account for " << vertexSum;
            EXPECT_EQ(indexSum, fp.IndexCount) << name << ": submesh index windows do not tile the index buffer";
        }
    }
} // namespace OloEngine::Tests

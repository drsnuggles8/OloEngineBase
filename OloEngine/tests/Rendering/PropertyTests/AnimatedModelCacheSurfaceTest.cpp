// OLO_TEST_LAYER: integration
// =============================================================================
// AnimatedModelCacheSurfaceTest -- a warm .omesh load of an animated model must
// be the SAME surface the cold import produced.
//
// Found by #1223's acceptance: a groom binding addresses a body's triangles by
// index and refuses a body whose topology hash differs. A binding cooked against
// a cold import was refused on the warm load of the same file (and vice versa),
// so whether a coat stayed on its animal depended on whether the mesh cache was
// warm -- in the editor, the coat stood at its bind pose while the body walked.
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h" // OLO_ENSURE_GPU_OR_SKIP

#include <gtest/gtest.h>

#include "OloEngine/Asset/MeshCache.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Renderer/MeshSource.h"

#include <cstring>
#include <filesystem>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Tests
{
    namespace
    {
        void MakeCold(const std::filesystem::path& path)
        {
            MeshCache::InvalidateCache(path);
            MeshCache::InvalidateCache(path, AnimatedModel::kCachePrefix);
        }

        // The first difference between two surfaces, or empty when identical.
        [[nodiscard]] std::string FirstDifference(const MeshSource& cold, const MeshSource& warm)
        {
            const auto& cv = cold.GetVertices();
            const auto& wv = warm.GetVertices();
            const auto& ci = cold.GetIndices();
            const auto& wi = warm.GetIndices();
            if (cv.Num() != wv.Num())
            {
                return "vertex count " + std::to_string(cv.Num()) + " vs " + std::to_string(wv.Num());
            }
            if (ci.Num() != wi.Num())
            {
                return "index count " + std::to_string(ci.Num()) + " vs " + std::to_string(wi.Num());
            }
            for (i32 i = 0; i < ci.Num(); ++i)
            {
                if (ci[i] != wi[i])
                {
                    std::string head = "index " + std::to_string(i) + ": " + std::to_string(ci[i]) + " vs " +
                                       std::to_string(wi[i]) + " (cold";
                    for (i32 k = 0; k < std::min<i32>(12, ci.Num()); ++k)
                    {
                        head += " " + std::to_string(ci[k]);
                    }
                    head += " | warm";
                    for (i32 k = 0; k < std::min<i32>(12, wi.Num()); ++k)
                    {
                        head += " " + std::to_string(wi[k]);
                    }
                    return head + ")";
                }
            }
            for (i32 i = 0; i < cv.Num(); ++i)
            {
                if (std::memcmp(&cv[i].Position, &wv[i].Position, sizeof(glm::vec3)) != 0)
                {
                    return "position of vertex " + std::to_string(i);
                }
            }
            const auto& cb = cold.GetBoneInfluences();
            const auto& wb = warm.GetBoneInfluences();
            if (cb.Num() != wb.Num())
            {
                return "influence count " + std::to_string(cb.Num()) + " vs " + std::to_string(wb.Num());
            }
            for (i32 i = 0; i < cb.Num(); ++i)
            {
                if (std::memcmp(&cb[i], &wb[i], sizeof(BoneInfluence)) != 0)
                {
                    return "bone influence of vertex " + std::to_string(i);
                }
            }
            return {};
        }
    } // namespace

    TEST(AnimatedModelCacheSurface, AWarmLoadIsTheSameSurfaceAsTheColdImport)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // The horse has TWO meshes (the body and its eyes), so the combined
        // .omesh is split back into two on a warm load. CesiumMan has one.
        for (const char* relative : { "SandboxProject/Assets/Models/Horse/Horse.gltf", "assets/models/CesiumMan/CesiumMan.gltf" })
        {
            const std::filesystem::path path = std::filesystem::path{ OLO_TEST_EDITOR_ROOT } / relative;
            SCOPED_TRACE(path.string());
            ASSERT_TRUE(std::filesystem::exists(path));
            MakeCold(path);

            const Ref<AnimatedModel> cold = Ref<AnimatedModel>::Create(path.string());
            ASSERT_TRUE(MeshCache::IsMeshCacheValid(path, AnimatedModel::kCachePrefix))
                << "the cold import wrote no cache; the warm half would re-run the cold one";
            const Ref<AnimatedModel> warm = Ref<AnimatedModel>::Create(path.string());

            ASSERT_EQ(cold->GetMeshes().size(), warm->GetMeshes().size());
            for (sizet m = 0; m < cold->GetMeshes().size(); ++m)
            {
                ASSERT_TRUE(cold->GetMeshes()[m]);
                ASSERT_TRUE(warm->GetMeshes()[m]);
                EXPECT_EQ(FirstDifference(*cold->GetMeshes()[m], *warm->GetMeshes()[m]), std::string{})
                    << "mesh " << m << " differs between the cold import and the warm cache load";
            }
            MakeCold(path);
        }
    }
} // namespace OloEngine::Tests

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
// This is the whole route, through AnimatedModel into its MeshSource, so it needs
// a live GL context and skips cleanly without one. AnimatedModelImportSettingsTest
// pins the same contract at the Assimp settings, with no GPU gate.
// =============================================================================

#include "OloEnginePCH.h"
#include "ProcessRendererInit.h"

#include <gtest/gtest.h>

#include "NormallessSkinnedGridFixture.h"
#include "TestTempDir.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "Rendering/PropertyTests/RenderPropertyTest.h"

namespace OloEngine::Tests
{
    TEST(AnimatedModelNormalsImport, ARiggedSourceWithoutNormalsImportsWeldedWithSmoothNormals)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        EnsureProcessRendererInitialized();
        GLStateGuard glGuard("AnimatedModelNormalsImport", GLStateGuard::Policy::Restore);

        const std::filesystem::path gltf = NormallessSkinnedGrid::Write(TempDir("grid"));
        const auto model = Ref<AnimatedModel>::Create(gltf.string());
        ASSERT_TRUE(model);
        ASSERT_TRUE(model->GetSkeleton()) << "the fixture is rigged and must take the animated route";
        ASSERT_EQ(model->GetMeshes().size(), 1u);

        const TArray<Vertex>& vertices = model->GetMeshes()[0]->GetVertices();
        EXPECT_EQ(vertices.Num(), static_cast<i32>(NormallessSkinnedGrid::kVertexCount))
            << "flat normals split the grid into " << NormallessSkinnedGrid::kTriangleCount * 3 << " vertices";
        for (const Vertex& vertex : vertices)
        {
            EXPECT_NEAR(vertex.Normal.x, 0.0f, 1e-4f);
            EXPECT_NEAR(vertex.Normal.y, 0.0f, 1e-4f);
            EXPECT_NEAR(vertex.Normal.z, 1.0f, 1e-4f);
        }
    }
} // namespace OloEngine::Tests

// OLO_TEST_LAYER: integration
//
// Two REGISTRY-LEVEL decisions of the virtualized-geometry path that shipped with zero
// coverage (issue #629). Neither is visible in a picture: one is a part that must NOT be
// drawn, the other is a cache that must be dropped.
//
//   1. AlphaMode::Blend REJECTION. Virtual geometry rasterizes into the deferred G-Buffer,
//      which stores one opaque surface per pixel — there is nowhere to put a blended
//      fragment, and the pass forces glDepthMask(TRUE) + glDisable(BLEND) for every
//      instance. A Blend part was therefore written into the G-Buffer FULLY OPAQUE: a
//      window rendered as a wall. It is now skipped, with a once-per-mesh warning.
//
//   2. VirtualMeshRegistry::Invalidate() ON MESHSOURCE HOT-RELOAD. RegisterMeshSource
//      caches a BAKED cluster DAG (its own copy of the vertex data) keyed by AssetHandle,
//      and every caller takes an IsRegistered() fast path. Without an invalidation on
//      reload, editing a mesh left the virtual path drawing the PRE-EDIT geometry for the
//      rest of the process — self-consistently, so nothing could trip a validation check,
//      while the classic path drew the new mesh. The blob-rejection half of that story is
//      covered by VirtualMeshCookIdentityTest; the TRIGGER — Renderer3D::OnAssetReloaded —
//      was not covered at all.
//
// (1) needs a GL context AND a live Renderer3D (PrepareFrame uploads the pooled SSBOs and
//     reads the frame's material slots out of FrameDataBufferManager, which only exists after
//     Renderer3D::Init) — hence the RendererAttachedTest fixture. It SKIPs without a GPU.
// (2) is headless: registration, invalidation and the reload hook are all CPU-side.

#include "OloEnginePCH.h"

#include "PropertyTests/RenderPropertyTest.h"   // OLO_ENSURE_GPU_OR_SKIP
#include "PropertyTests/RendererAttachedTest.h" // brings Renderer3D up (FrameDataBufferManager)

#include <gtest/gtest.h>

#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Events/ApplicationEvent.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshRegistry.h"

#include <filesystem>
#include <fstream>
#include <vector>

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

namespace
{
    // A flat grid sheet: enough triangles for the builder to produce a real DAG.
    Ref<MeshSource> MakeGridSheet(u32 quadsPerSide)
    {
        TArray<Vertex> vertices;
        TArray<u32> indices;
        u32 const verts = quadsPerSide + 1;
        for (u32 y = 0; y < verts; ++y)
        {
            for (u32 x = 0; x < verts; ++x)
            {
                f32 const u = static_cast<f32>(x) / static_cast<f32>(quadsPerSide);
                f32 const v = static_cast<f32>(y) / static_cast<f32>(quadsPerSide);
                vertices.Add(Vertex(glm::vec3(u * 2.0f - 1.0f, v * 2.0f - 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f),
                                    glm::vec2(u, v)));
            }
        }
        for (u32 y = 0; y < quadsPerSide; ++y)
        {
            for (u32 x = 0; x < quadsPerSide; ++x)
            {
                u32 const i0 = y * verts + x;
                for (u32 const idx : { i0, i0 + 1, i0 + verts + 1, i0, i0 + verts + 1, i0 + verts })
                {
                    indices.Add(idx);
                }
            }
        }
        return Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));
    }

    // A material data slot with the given glTF alpha mode (0 = Opaque, 2 = Blend).
    u32 AllocateMaterialSlot(AlphaMode mode)
    {
        PODMaterialData data;
        data.enablePBR = true;
        data.baseColorFactor = glm::vec4(1.0f);
        data.alphaMode = static_cast<i32>(mode);
        return FrameDataBufferManager::Get().AllocateMaterialData(data);
    }

    // A minimal project + asset manager: Renderer3D::OnAssetReloaded asks the AssetManager for
    // the asset's generation, which asserts without one.
    void EnsureProject()
    {
        if (Project::GetActive() && Project::GetAssetManager())
        {
            return;
        }
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path const projectDir = fs::temp_directory_path() / "OloEngineVirtualMeshRegistryRejection";
        fs::create_directories(projectDir / "Assets", ec);
        {
            std::ofstream proj(projectDir / "Rejection.oloproj");
            proj << "Project:\n"
                    "  Name: VirtualMeshRegistryRejection\n"
                    "  StartScene: \"\"\n"
                    "  AssetDirectory: \"Assets\"\n"
                    "  ScriptModulePath: \"\"\n";
        }
        ASSERT_TRUE(Project::Load(projectDir / "Rejection.oloproj"));
        auto assetManager = Ref<EditorAssetManager>::Create();
        assetManager->Initialize(false); // no file watcher in tests
        Project::SetAssetManager(assetManager);
    }
} // namespace

// The fixture exists only to bring Renderer3D (and with it FrameDataBufferManager) up — the
// test drives the registry directly and never renders a frame.
class VirtualMeshRegistryRejection : public OloEngine::Tests::RendererAttachedTest
{
  protected:
    void BuildScene() override
    {
    }
};

// C7: a Blend part must be SKIPPED by the virtual pass, not written opaque into the G-Buffer.
TEST_F(VirtualMeshRegistryRejection, AlphaModeBlendPartIsSkippedInsteadOfDrawnOpaque)
{
    OLO_ENSURE_GPU_OR_SKIP();

    auto& registry = VirtualMeshRegistry::Get();

    Ref<MeshSource> const sheet = MakeGridSheet(16);
    AssetHandle const opaqueMesh = AssetHandle(OloEngine::UUID());
    AssetHandle const blendMesh = AssetHandle(OloEngine::UUID());
    ASSERT_TRUE(registry.RegisterMeshSource(opaqueMesh, *sheet));
    ASSERT_TRUE(registry.RegisterMeshSource(blendMesh, *sheet));

    registry.BeginFrame();

    // Both submissions are identical except for their material's ALPHA MODE. The opaque one
    // is the control: it proves the frame was built at all, so a "0 instances" result cannot
    // be mistaken for a working rejection.
    VirtualMeshSubmission opaque;
    opaque.Mesh = opaqueMesh;
    opaque.EntityID = 1;
    opaque.MaterialDataIndices = { AllocateMaterialSlot(AlphaMode::Opaque) };
    opaque.PartAlphaMasked = { 0u };
    opaque.PartTwoSided = { 0u };
    registry.Submit(opaque);

    VirtualMeshSubmission blend;
    blend.Mesh = blendMesh;
    blend.EntityID = 2;
    blend.MaterialDataIndices = { AllocateMaterialSlot(AlphaMode::Blend) };
    blend.PartAlphaMasked = { 1u };
    blend.PartTwoSided = { 0u };
    registry.Submit(blend);

    ASSERT_TRUE(registry.PrepareFrame(glm::vec3(0.0f))) << "PrepareFrame built no frame at all";

    const auto& instances = registry.GetFrameInstances();
    ASSERT_EQ(instances.size(), 1u)
        << "expected exactly ONE frame instance: the OPAQUE submission. Got " << instances.size()
        << ". An AlphaMode::Blend part must be REJECTED from the virtualized-geometry path — the deferred G-Buffer "
           "stores one opaque surface per pixel and the pass forces depth-write on and blending off, so a Blend "
           "part is rasterized FULLY OPAQUE (a window renders as a wall). Drawing it wrong is worse than not "
           "drawing it: skip it and warn once per mesh.";
    EXPECT_EQ(instances[0].Gpu.EntityID, 1) << "the wrong submission survived — the OPAQUE one was rejected";

    // Re-preparing another frame must reject it again (the once-per-mesh WARNING is
    // suppressed after the first, the SKIP is not).
    registry.BeginFrame();
    registry.Submit(opaque);
    registry.Submit(blend);
    ASSERT_TRUE(registry.PrepareFrame(glm::vec3(0.0f)));
    EXPECT_EQ(registry.GetFrameInstances().size(), 1u)
        << "the Blend part was skipped on the first frame but drawn on the second — the rejection must be a "
           "per-frame decision, not a one-shot side effect of the warn-once bookkeeping";

    registry.BeginFrame(); // leave no submissions behind for the next test
}

// C6/#629: a MeshSource hot-reload must DROP the baked DAG, or the virtual path keeps
// drawing the pre-edit geometry for the whole process.
TEST(VirtualMeshRegistryReload, MeshSourceHotReloadInvalidatesTheBakedClusterDag)
{
    // Headless: registration + invalidation are CPU-side (the GPU pools are only touched by
    // PrepareFrame).
    EnsureProject();

    auto& registry = VirtualMeshRegistry::Get();

    Ref<MeshSource> const original = MakeGridSheet(8);
    AssetHandle const handle = AssetHandle(OloEngine::UUID());

    ASSERT_TRUE(registry.RegisterMeshSource(handle, *original));
    ASSERT_TRUE(registry.IsRegistered(handle));

    VirtualMeshRegistry::MeshParts const before = registry.FindParts(handle);
    ASSERT_TRUE(before.Valid);
    u32 const trianglesBefore = registry.GetEntry(before.FirstEntry).SourceTriangleCount;
    ASSERT_GT(trianglesBefore, 0u);

    // An unrelated asset type reloading must NOT drop the mesh (a texture edit would then
    // re-cook every virtual mesh in the scene).
    {
        AssetReloadedEvent unrelated(handle, AssetType::Texture2D, "some/texture.png");
        Renderer3D::OnAssetReloaded(unrelated);
    }
    EXPECT_TRUE(registry.IsRegistered(handle))
        << "a Texture2D reload dropped the cluster DAG — only a MeshSource reload should";

    // The real thing.
    {
        AssetReloadedEvent reloaded(handle, AssetType::MeshSource, "some/mesh.gltf");
        Renderer3D::OnAssetReloaded(reloaded);
    }
    EXPECT_FALSE(registry.IsRegistered(handle))
        << "a MeshSource hot-reload did NOT drop the baked cluster DAG. RegisterMeshSource caches by AssetHandle "
           "and every caller takes the IsRegistered() fast path, so the virtual path would keep drawing the "
           "PRE-EDIT geometry for the rest of the process — self-consistently (the DAG bakes its own copy of the "
           "vertices), with nothing to trip a validation check, while the classic path draws the new mesh. "
           "Renderer3D::OnAssetReloaded must call VirtualMeshRegistry::Invalidate() for AssetType::MeshSource.";
    EXPECT_FALSE(registry.FindParts(handle).Valid) << "the mesh's parts survived the invalidation";

    // And the next submission must re-cook from the NEW source: register a mesh with a
    // different triangle count under the SAME handle and require the registry to see it.
    Ref<MeshSource> const edited = MakeGridSheet(16); // 4x the triangles
    ASSERT_TRUE(registry.RegisterMeshSource(handle, *edited));
    VirtualMeshRegistry::MeshParts const after = registry.FindParts(handle);
    ASSERT_TRUE(after.Valid);
    u32 const trianglesAfter = registry.GetEntry(after.FirstEntry).SourceTriangleCount;
    EXPECT_GT(trianglesAfter, trianglesBefore)
        << "re-registering the reloaded MeshSource returned the OLD DAG (" << trianglesAfter << " source triangles, "
        << "the edited mesh has more) — Invalidate() dropped the lookup but the entry was reused";

    // Do not leave the singleton holding this test's meshes.
    AssetReloadedEvent cleanup(handle, AssetType::MeshSource, "some/mesh.gltf");
    Renderer3D::OnAssetReloaded(cleanup);
}

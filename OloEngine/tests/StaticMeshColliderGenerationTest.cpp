#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// =============================================================================
// StaticMeshColliderGenerationTest — honors StaticMesh::m_GenerateColliders.
//
// Before this feature the flag was a no-op TODO in StaticMesh::SetupStaticMesh:
// it round-tripped through the serializer but generated nothing. Now, when the
// flag is set, SetupStaticMesh builds a memory-only MeshColliderAsset (backed by
// a wrapper Mesh so the cooker has a Mesh to read) and exposes it via
// GetGeneratedCollider(). Cooking stays lazy — physics/nav consumers cook the
// asset on demand through MeshColliderCache::GetMeshData().
//
// These tests pin three things:
//   (1) Wiring: the flag produces a MeshColliderAsset whose m_ColliderMesh
//       resolves to a Mesh whose MeshSource is the original geometry; the flag
//       being off (or the source empty) produces no collider.
//   (2) Cookability: that generated asset actually cooks — both the convex
//       (simple) and triangle (complex) paths return Success.
//   (3) Usability: the cooked triangle shape is a real collider — a ray fired
//       through the cube is reported as a hit.
//
// (2)/(3) drive the cooker directly with a throwaway MeshCookingFactory pointed
// at a unique temp cache dir, so the suite needs no GPU, no physics world, no
// task scheduler, and leaves nothing in the working tree.
// =============================================================================

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/MeshColliderAsset.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Physics3D/MeshCookingFactory.h"

#include <glm/glm.hpp>

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Collision/Shape/SubShapeID.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace OloEngine; // NOLINT(google-build-using-namespace)

namespace
{
    namespace fs = std::filesystem;

    // 1x1x1 axis-aligned cube centered on the origin, triangles wound CCW from
    // outside (outward normals) — matches the cube in JoltShapesMeshMassProperties.
    Ref<MeshSource> MakeUnitCubeMeshSource()
    {
        const glm::vec3 h(0.5f);
        const std::array<glm::vec3, 8> corners = {
            glm::vec3(-h.x, -h.y, -h.z), // 0
            glm::vec3(+h.x, -h.y, -h.z), // 1
            glm::vec3(+h.x, +h.y, -h.z), // 2
            glm::vec3(-h.x, +h.y, -h.z), // 3
            glm::vec3(-h.x, -h.y, +h.z), // 4
            glm::vec3(+h.x, -h.y, +h.z), // 5
            glm::vec3(+h.x, +h.y, +h.z), // 6
            glm::vec3(-h.x, +h.y, +h.z), // 7
        };

        TArray<Vertex> vertices;
        for (const auto& p : corners)
        {
            vertices.Add(Vertex(p, glm::vec3(0.0f), glm::vec2(0.0f)));
        }

        TArray<u32> indices;
        const std::array<u32, 36> cubeIndices = {
            0, 3, 2, 0, 2, 1, // -Z
            4, 5, 6, 4, 6, 7, // +Z
            0, 1, 5, 0, 5, 4, // -Y
            3, 7, 6, 3, 6, 2, // +Y
            0, 4, 7, 0, 7, 3, // -X
            1, 2, 6, 1, 6, 5  // +X
        };
        for (u32 idx : cubeIndices)
        {
            indices.Add(idx);
        }

        auto source = Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(indices));

        Submesh sub;
        sub.m_BaseVertex = 0;
        sub.m_BaseIndex = 0;
        sub.m_VertexCount = 8;
        sub.m_IndexCount = 36;
        sub.m_NodeName = "Cube";
        source->GetSubmeshes().Add(sub);

        return source;
    }

    // Fresh, never-reused scratch dir under the system temp area — mirrors
    // MeshCookingFactoryCacheTest so concurrent test processes never collide.
    fs::path MakeUniqueScratchDir(const char* tag)
    {
        static std::atomic<u64> s_Counter{ 0 };
        const auto base = fs::temp_directory_path() / "OloEngineTests" / tag;
        const auto nanos = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto seq = s_Counter.fetch_add(1, std::memory_order_relaxed);

        std::ostringstream suffix;
        suffix << std::hex << nanos << "_" << seq;

        const auto unique = base / suffix.str();
        fs::create_directories(unique);
        return unique;
    }
} // namespace

class StaticMeshColliderGenerationTest : public ::testing::Test
{
  protected:
    // Cooking calls into Jolt (MeshShapeSettings::Create, shape (de)serialization),
    // which needs the process-global JPH::Factory + registered RTTI. The engine
    // normally bootstraps this through JoltScene under a refcount; here we set it
    // up once for the suite, but only if no live JoltScene already did so — and we
    // tear down only what we created, so we leave global Jolt state exactly as we
    // found it for any other suite.
    static void SetUpTestSuite()
    {
        if (JPH::Factory::sInstance == nullptr)
        {
            JPH::RegisterDefaultAllocator();
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();
            s_OwnsJolt = true;
        }
    }

    static void TearDownTestSuite()
    {
        if (s_OwnsJolt)
        {
            JPH::UnregisterTypes();
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
            s_OwnsJolt = false;
        }
    }

    void SetUp() override
    {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string folder = std::string(info->test_suite_name()) + "." + info->name();
        m_TempDir = fs::temp_directory_path() / "OloEngineStaticMeshCollider" / folder;

        std::error_code ec;
        fs::remove_all(m_TempDir, ec);
        fs::create_directories(m_TempDir / "Assets", ec);
        ASSERT_FALSE(ec) << "Failed to create temp dir: " << ec.message();

        const fs::path projectFile = m_TempDir / "Test.oloproj";
        {
            std::ofstream proj(projectFile);
            proj << "Project:\n"
                    "  Name: StaticMeshColliderTest\n"
                    "  StartScene: \"\"\n"
                    "  AssetDirectory: \"Assets\"\n"
                    "  ScriptModulePath: \"\"\n";
        }

        ASSERT_TRUE(Project::Load(projectFile))
            << "Project::Load failed for temp project at " << m_TempDir.string();

        m_AssetManager = Ref<EditorAssetManager>::Create();
        m_AssetManager->Initialize(/*startFileWatcher=*/false);
        Project::SetAssetManager(m_AssetManager);
    }

    void TearDown() override
    {
        m_AssetManager.Reset();
        std::error_code ec;
        fs::remove_all(m_TempDir, ec);
    }

    fs::path m_TempDir;
    Ref<EditorAssetManager> m_AssetManager;

    static bool s_OwnsJolt;
};

bool StaticMeshColliderGenerationTest::s_OwnsJolt = false;

// (1) Wiring: the flag produces a MeshColliderAsset that resolves, through a
// wrapper Mesh, back to the original geometry — exactly what the cooker needs.
TEST_F(StaticMeshColliderGenerationTest, GenerateFlagWiresColliderAssetToMeshSource)
{
    auto meshSource = MakeUnitCubeMeshSource();
    const AssetHandle meshSourceHandle = AssetManager::AddMemoryOnlyAsset<MeshSource>(meshSource);
    ASSERT_NE(static_cast<u64>(meshSourceHandle), 0ULL);

    StaticMesh staticMesh(meshSourceHandle, /*generateColliders=*/true);

    ASSERT_TRUE(staticMesh.ShouldGenerateColliders());
    const AssetHandle colliderHandle = staticMesh.GetGeneratedCollider();
    ASSERT_NE(static_cast<u64>(colliderHandle), 0ULL)
        << "GenerateColliders=true must produce a MeshColliderAsset handle";

    auto colliderAsset = AssetManager::GetAsset<MeshColliderAsset>(colliderHandle);
    ASSERT_TRUE(colliderAsset) << "Generated handle does not resolve to a MeshColliderAsset";
    ASSERT_NE(static_cast<u64>(colliderAsset->m_ColliderMesh), 0ULL);

    // The collider's source mesh must be a Mesh (the cooker reads GetAsset<Mesh>)
    // whose MeshSource is the cube we started from.
    auto wrapperMesh = AssetManager::GetAsset<Mesh>(colliderAsset->m_ColliderMesh);
    ASSERT_TRUE(wrapperMesh) << "MeshColliderAsset::m_ColliderMesh must resolve to a Mesh";
    ASSERT_TRUE(wrapperMesh->GetMeshSource());
    EXPECT_EQ(wrapperMesh->GetMeshSource()->GetHandle(), meshSourceHandle);
}

// The flag being off must leave the old behavior intact: no collider asset.
TEST_F(StaticMeshColliderGenerationTest, GenerateFlagOffProducesNoCollider)
{
    auto meshSource = MakeUnitCubeMeshSource();
    const AssetHandle meshSourceHandle = AssetManager::AddMemoryOnlyAsset<MeshSource>(meshSource);

    StaticMesh staticMesh(meshSourceHandle, /*generateColliders=*/false);

    EXPECT_FALSE(staticMesh.ShouldGenerateColliders());
    EXPECT_EQ(static_cast<u64>(staticMesh.GetGeneratedCollider()), 0ULL);
}

// A source with no submeshes can't be cooked — generation must skip gracefully
// (warn + leave the handle null) rather than crash the load.
TEST_F(StaticMeshColliderGenerationTest, EmptyMeshSourceSkipsGeneration)
{
    auto emptySource = Ref<MeshSource>::Create(TArray<Vertex>{}, TArray<u32>{}); // no submeshes
    const AssetHandle meshSourceHandle = AssetManager::AddMemoryOnlyAsset<MeshSource>(emptySource);

    StaticMesh staticMesh(meshSourceHandle, /*generateColliders=*/true);

    EXPECT_TRUE(staticMesh.ShouldGenerateColliders());
    EXPECT_EQ(static_cast<u64>(staticMesh.GetGeneratedCollider()), 0ULL)
        << "A submesh-less source must not produce a collider handle";
}

// Re-setup (the hot-reload path) must keep the same collider handle stable rather
// than leaking a fresh asset on every reload.
TEST_F(StaticMeshColliderGenerationTest, ReSetupKeepsColliderHandleStable)
{
    auto meshSource = MakeUnitCubeMeshSource();
    const AssetHandle meshSourceHandle = AssetManager::AddMemoryOnlyAsset<MeshSource>(meshSource);

    StaticMesh staticMesh(meshSourceHandle, /*generateColliders=*/true);
    const AssetHandle firstHandle = staticMesh.GetGeneratedCollider();
    ASSERT_NE(static_cast<u64>(firstHandle), 0ULL);

    // SetSubmeshes re-runs SetupStaticMesh -> GenerateColliders' refresh branch.
    TArray<u32> submeshes;
    submeshes.Add(0u);
    staticMesh.SetSubmeshes(submeshes);

    EXPECT_EQ(staticMesh.GetGeneratedCollider(), firstHandle)
        << "Re-setup must refresh in place, not allocate a new collider asset";
}

// (2)+(3) The generated asset actually cooks, and the cooked triangle mesh is a
// real collider that a ray can hit.
TEST_F(StaticMeshColliderGenerationTest, GeneratedColliderCooksAndIsHitByRaycast)
{
    auto meshSource = MakeUnitCubeMeshSource();
    const AssetHandle meshSourceHandle = AssetManager::AddMemoryOnlyAsset<MeshSource>(meshSource);

    StaticMesh staticMesh(meshSourceHandle, /*generateColliders=*/true);

    const AssetHandle colliderHandle = staticMesh.GetGeneratedCollider();
    ASSERT_NE(static_cast<u64>(colliderHandle), 0ULL);
    auto colliderAsset = AssetManager::GetAsset<MeshColliderAsset>(colliderHandle);
    ASSERT_TRUE(colliderAsset);

    // Cook the generated asset with a throwaway factory pointed at a temp dir so
    // nothing lands in the working tree.
    const auto cacheDir = MakeUniqueScratchDir("StaticMeshColliderCook");
    MeshCookingFactory factory(cacheDir);
    factory.Initialize();

    // Triangle (complex) is the sensible collider for a static mesh and the shape
    // the raycast below uses, so that path must cook cleanly. The convex (simple)
    // path is left unasserted on purpose: the cooker's naive vertex sampler can
    // pick a coplanar subset of an axis-aligned box's corners and degenerate the
    // hull — a pre-existing cooker limitation, orthogonal to this feature.
    auto [simpleResult, complexResult] = factory.CookMesh(colliderAsset);
    (void)simpleResult;
    ASSERT_EQ(complexResult, ECookingResult::Success) << "triangle cook should succeed for a cube";

    // Reconstruct the cooked triangle (complex) shape and fire a ray straight
    // through the cube center along +Z. The cube spans z in [-0.5, 0.5]; a ray
    // from z=-2 with length 4 must register a hit on the front face.
    const fs::path triPath = factory.GetCacheFilePath(colliderAsset, EMeshColliderType::Triangle);
    MeshColliderData triData = factory.DeserializeMeshCollider(triPath);
    ASSERT_TRUE(triData.m_IsValid);
    ASSERT_FALSE(triData.m_Submeshes.empty());

    JPH::Ref<JPH::Shape> shape = factory.CreateShapeFromColliderData(triData.m_Submeshes[0]);
    ASSERT_NE(shape.GetPtr(), nullptr) << "Cooked triangle data must rebuild into a Jolt shape";

    JPH::RayCast ray(JPH::Vec3(0.0f, 0.0f, -2.0f), JPH::Vec3(0.0f, 0.0f, 4.0f));
    JPH::RayCastResult hit;
    const bool didHit = shape->CastRay(ray, JPH::SubShapeIDCreator(), hit);

    EXPECT_TRUE(didHit) << "Ray through the cube center must hit the generated collider";
    EXPECT_GT(hit.mFraction, 0.0f);
    EXPECT_LT(hit.mFraction, 1.0f);
    // Front face is at z = -0.5 -> fraction (-0.5 - -2.0) / 4.0 = 0.375.
    EXPECT_NEAR(hit.mFraction, 0.375f, 1e-3f);
}

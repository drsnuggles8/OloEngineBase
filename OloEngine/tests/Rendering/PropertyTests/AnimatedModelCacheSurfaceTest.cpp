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
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include "OloEngine/Asset/MeshCache.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Renderer/MeshSource.h"

#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

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
        //
        // Each model is COPIED into this test's temp directory first. The cache
        // is keyed on the source path, so invalidating the committed files'
        // caches would pull them out from under any other test process reading
        // the same model at the same time.
        for (const char* relative : { "SandboxProject/Assets/Models/Horse", "assets/models/CesiumMan" })
        {
            const std::filesystem::path source = std::filesystem::path{ OLO_TEST_EDITOR_ROOT } / relative;
            ASSERT_TRUE(std::filesystem::exists(source)) << source.string();
            const std::filesystem::path copy = TempDir(source.filename().string());
            std::error_code ec;
            std::filesystem::copy(source, copy, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
                                  ec);
            ASSERT_FALSE(ec) << ec.message();
            const std::filesystem::path path = copy / (source.filename().string() + ".gltf");
            SCOPED_TRACE(path.string());
            ASSERT_TRUE(std::filesystem::exists(path));
            MakeCold(path);

            const Ref<AnimatedModel> cold = Ref<AnimatedModel>::Create(path.string());
            ASSERT_FALSE(cold->WasMeshLoadedFromCache());
            ASSERT_TRUE(MeshCache::IsMeshCacheValid(path, AnimatedModel::kCachePrefix))
                << "the cold import wrote no cache; the warm half would re-run the cold one";
            const Ref<AnimatedModel> warm = Ref<AnimatedModel>::Create(path.string());
            ASSERT_TRUE(warm->WasMeshLoadedFromCache())
                << "the second load fell back to a fresh mesh import instead of using the cache";

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
    TEST(AnimatedModelCacheSurface, AnExternalBufferEditInvalidatesBothCaches)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        namespace fs = std::filesystem;
        const fs::path source = fs::path{ OLO_TEST_EDITOR_ROOT } / "assets/models/CesiumMan";
        const fs::path copy = TempDir("buffer-edit");
        std::error_code ec;
        fs::copy(source, copy, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << ec.message();
        const fs::path path = copy / "CesiumMan.gltf";
        std::ifstream input(path);
        auto gltf = nlohmann::json::parse(input);
        input.close();
        const fs::path originalBuffer = copy / gltf.at("buffers").at(0).at("uri").get<std::string>();
        const fs::path buffer = copy / "editable buffer.bin";
        fs::rename(originalBuffer, buffer);
        gltf["buffers"][0]["uri"] = "editable%20buffer.bin";
        {
            std::ofstream descriptor(path);
            descriptor << gltf.dump();
        }
        const auto originalTime = fs::last_write_time(path);
        MakeCold(path);
        const Ref<AnimatedModel> cold = Ref<AnimatedModel>::Create(path.string());
        ASSERT_TRUE(cold && cold->HasSkeleton());
        ASSERT_TRUE(MeshCache::IsMeshCacheValid(path, AnimatedModel::kCachePrefix));
        ASSERT_TRUE(MeshCache::IsAnimationCacheValid(path));
        const Ref<AnimatedModel> warm = Ref<AnimatedModel>::Create(path.string());
        ASSERT_TRUE(warm->WasMeshLoadedFromCache());

        const auto position = gltf.at("meshes").at(0).at("primitives").at(0).at("attributes").at("POSITION").get<u32>();
        const auto& accessor = gltf.at("accessors").at(position);
        const auto& view = gltf.at("bufferViews").at(accessor.at("bufferView").get<u32>());
        ASSERT_EQ(accessor.at("componentType").get<u32>(), 5126u);
        ASSERT_EQ(view.at("buffer").get<u32>(), 0u);
        const auto offset = view.value("byteOffset", 0u) + accessor.value("byteOffset", 0u);
        {
            std::fstream binary(buffer, std::ios::binary | std::ios::in | std::ios::out);
            ASSERT_TRUE(binary);
            binary.seekg(offset);
            f32 x = 0.0f;
            ASSERT_TRUE(binary.read(reinterpret_cast<char*>(&x), sizeof(x)));
            x += 0.125f;
            binary.seekp(offset);
            ASSERT_TRUE(binary.write(reinterpret_cast<const char*>(&x), sizeof(x)));
        }
        // No clock-resolution assumption, and the glTF itself remains untouched.
        fs::last_write_time(buffer, fs::last_write_time(buffer) + std::chrono::seconds(2));
        EXPECT_EQ(fs::last_write_time(path), originalTime);
        EXPECT_FALSE(MeshCache::IsMeshCacheValid(path, AnimatedModel::kCachePrefix));
        EXPECT_FALSE(MeshCache::IsAnimationCacheValid(path));
        const Ref<AnimatedModel> edited = Ref<AnimatedModel>::Create(path.string());
        ASSERT_TRUE(edited);
        EXPECT_FALSE(edited->WasMeshLoadedFromCache());
        EXPECT_FALSE(FirstDifference(*cold->GetEntityMeshSource(), *edited->GetEntityMeshSource()).empty());
        ASSERT_TRUE(MeshCache::IsMeshCacheValid(path, AnimatedModel::kCachePrefix));
        ASSERT_TRUE(MeshCache::IsAnimationCacheValid(path));

        // Metadata-based validity deliberately does not read every buffer byte.
        // A timestamp-preserving restore must explicitly invalidate the caches.
        const auto bufferTime = fs::last_write_time(buffer);
        const auto bufferSize = fs::file_size(buffer);
        ASSERT_TRUE(fs::copy_file(source / originalBuffer.filename(), buffer, fs::copy_options::overwrite_existing, ec));
        ASSERT_FALSE(ec) << ec.message();
        fs::last_write_time(buffer, bufferTime);
        ASSERT_EQ(fs::file_size(buffer), bufferSize);
        EXPECT_TRUE(MeshCache::IsMeshCacheValid(path, AnimatedModel::kCachePrefix));
        EXPECT_TRUE(MeshCache::IsAnimationCacheValid(path));
        MeshCache::InvalidateCache(path);
        EXPECT_FALSE(MeshCache::IsMeshCacheValid(path, AnimatedModel::kCachePrefix));
        EXPECT_FALSE(MeshCache::IsAnimationCacheValid(path));
        const Ref<AnimatedModel> restored = Ref<AnimatedModel>::Create(path.string());
        ASSERT_TRUE(restored);
        EXPECT_FALSE(restored->WasMeshLoadedFromCache());
        EXPECT_EQ(FirstDifference(*cold->GetEntityMeshSource(), *restored->GetEntityMeshSource()), std::string{});
        ASSERT_TRUE(MeshCache::IsMeshCacheValid(path, AnimatedModel::kCachePrefix));
        ASSERT_TRUE(MeshCache::IsAnimationCacheValid(path));

        fs::rename(buffer, copy / "removed.bin");
        EXPECT_FALSE(MeshCache::IsMeshCacheValid(path, AnimatedModel::kCachePrefix));
        EXPECT_FALSE(MeshCache::IsAnimationCacheValid(path));
        MakeCold(path);
    }
} // namespace OloEngine::Tests

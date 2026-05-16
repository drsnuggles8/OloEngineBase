// =============================================================================
// ModelLoadDeterminismTest.cpp
//
// Diagnostic test: load a Model twice (first run: fresh from Assimp + write
// cache; second run: read from cache) and verify the deserialized geometry
// data + bound texture content are bit-identical between the two paths.
//
// If the cache round-trip is broken (vertices/UVs/indices shuffled, or the
// cache-load texture pipeline produces different GPU data than the fresh
// load), this test catches it without requiring a manual screenshot
// comparison. Each load also writes the loaded albedo texture pixels to
// PNG under `OloEditor/assets/tests/captures/ModelLoadDeterminism/` so the
// human reviewer can sanity-check visually.
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <gtest/gtest.h>

#include "OloEngine/Asset/MeshCache.h"
#include "OloEngine/Core/Hash.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Vertex.h"

#include <stb_image/stb_image_write.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        // The capture root is under OloEditor/ (tests run with that CWD).
        fs::path CaptureDir()
        {
            const auto root = fs::path("assets") / "tests" / "captures" / "ModelLoadDeterminism";
            std::error_code ec;
            fs::create_directories(root, ec);
            return root;
        }

        struct LoadedSnapshot
        {
            u32 m_VertexCount = 0;
            u32 m_IndexCount = 0;
            u32 m_SubmeshCount = 0;
            u32 m_MaterialCount = 0;
            u32 m_VertexCRC = 0;       // CRC32 over the raw vertex bytes
            u32 m_IndexCRC = 0;        // CRC32 over the raw index bytes
            u32 m_TextureCRC = 0;      // CRC32 over the albedo texture's RGBA pixels
            u32 m_TextureWidth = 0;
            u32 m_TextureHeight = 0;
            std::string m_AlbedoPath; // Source path of the albedo texture
        };

        // Read back the GPU pixels of a Texture2D into a CPU buffer and CRC them.
        // Returns false (and leaves the buffer empty) if readback fails.
        bool ReadbackTextureToCRC(const Ref<Texture2D>& tex, u32& outCRC, u32& outWidth, u32& outHeight, std::vector<u8>& outRGBA)
        {
            outCRC = 0;
            outWidth = 0;
            outHeight = 0;
            outRGBA.clear();
            if (!tex)
                return false;

            const auto width = tex->GetWidth();
            const auto height = tex->GetHeight();
            if (width == 0 || height == 0)
                return false;

            outWidth = width;
            outHeight = height;
            outRGBA.assign(static_cast<sizet>(width) * height * 4, 0);

            // Read back at mip level 0 as RGBA8.
            ::glGetTextureImage(tex->GetRendererID(),
                                /*level*/ 0,
                                GL_RGBA, GL_UNSIGNED_BYTE,
                                static_cast<GLsizei>(outRGBA.size()),
                                outRGBA.data());

            outCRC = Hash::CRC32(outRGBA.data(), outRGBA.size());
            return true;
        }

        // Capture everything we want to compare between fresh and cache load.
        LoadedSnapshot Snapshot(const Model& model, const fs::path& pngOut)
        {
            LoadedSnapshot s;
            s.m_SubmeshCount = static_cast<u32>(model.GetMeshes().size());
            s.m_MaterialCount = static_cast<u32>(model.GetMaterials().size());

            // CRC the FIRST mesh's vertex + index buffers (the LearnOpenGL
            // backpack collapses to a single merged mesh under
            // aiProcess_PreTransformVertices, so [0] covers the whole model).
            if (!model.GetMeshes().empty() && model.GetMeshes()[0])
            {
                const auto& mesh = model.GetMeshes()[0];
                const auto src = mesh->GetMeshSource();
                if (src)
                {
                    const auto& verts = src->GetVertices();
                    const auto& indices = src->GetIndices();
                    s.m_VertexCount = static_cast<u32>(verts.Num());
                    s.m_IndexCount = static_cast<u32>(indices.Num());
                    s.m_VertexCRC = Hash::CRC32(reinterpret_cast<const u8*>(verts.GetData()),
                                                static_cast<sizet>(verts.Num()) * sizeof(Vertex));
                    s.m_IndexCRC = Hash::CRC32(reinterpret_cast<const u8*>(indices.GetData()),
                                               static_cast<sizet>(indices.Num()) * sizeof(u32));
                }
            }

            // CRC the albedo texture pixel data and write a PNG.
            if (!model.GetMaterials().empty() && model.GetMaterials()[0])
            {
                const auto& mat = model.GetMaterials()[0];
                const auto albedo = mat->GetAlbedoMap();
                if (albedo)
                {
                    s.m_AlbedoPath = albedo->GetPath();
                    std::vector<u8> pixels;
                    if (ReadbackTextureToCRC(albedo, s.m_TextureCRC, s.m_TextureWidth, s.m_TextureHeight, pixels))
                    {
                        const auto pngPath = pngOut.string();
                        // glGetTextureImage returns rows top-to-bottom in our setup
                        // (stbi_set_flip_vertically_on_load(1) at load time), so
                        // write directly without flipping.
                        ::stbi_write_png(pngPath.c_str(),
                                         static_cast<int>(s.m_TextureWidth),
                                         static_cast<int>(s.m_TextureHeight),
                                         /*comp*/ 4,
                                         pixels.data(),
                                         /*stride*/ static_cast<int>(s.m_TextureWidth) * 4);
                    }
                }
            }

            return s;
        }

        void Dump(const char* label, const LoadedSnapshot& s)
        {
            std::printf("[%s] verts=%u indices=%u submeshes=%u materials=%u vCRC=0x%08x iCRC=0x%08x tex=%ux%u tCRC=0x%08x albedo='%s'\n",
                        label,
                        s.m_VertexCount, s.m_IndexCount,
                        s.m_SubmeshCount, s.m_MaterialCount,
                        s.m_VertexCRC, s.m_IndexCRC,
                        s.m_TextureWidth, s.m_TextureHeight,
                        s.m_TextureCRC,
                        s.m_AlbedoPath.c_str());
        }

        void DumpFirstIndices(const Model& model, const char* label)
        {
            if (model.GetMeshes().empty() || !model.GetMeshes()[0])
                return;
            const auto src = model.GetMeshes()[0]->GetMeshSource();
            if (!src) return;
            const auto& indices = src->GetIndices();
            const auto& subs = src->GetSubmeshes();
            std::printf("[%s] submesh[0] baseIdx=%u idxCount=%u baseVtx=%u vtxCount=%u preOpt=%d\n",
                        label,
                        subs.IsEmpty() ? 0u : subs[0].m_BaseIndex,
                        subs.IsEmpty() ? 0u : subs[0].m_IndexCount,
                        subs.IsEmpty() ? 0u : subs[0].m_BaseVertex,
                        subs.IsEmpty() ? 0u : subs[0].m_VertexCount,
                        src->IsPreOptimized() ? 1 : 0);
            std::printf("[%s] first 12 indices:", label);
            const auto n = std::min<i32>(12, indices.Num());
            for (i32 i = 0; i < n; ++i)
                std::printf(" %u", indices[i]);
            std::printf("\n");
        }
    } // namespace

    // Loads the LearnOpenGL backpack twice. Run 1: invalidate cache first, so
    // ProcessNode is exercised; this also writes the cache as a side effect.
    // Run 2: cache is now valid, so the cache-load path is exercised.
    //
    // The test FAILS if the two snapshots differ in any field — that means
    // the cache round-trip is lossy, which is the most likely cause of the
    // "Reload makes the model look different" symptom the editor exhibits.
    TEST(ModelLoadDeterminism, Backpack_FreshVsCache)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const std::string path = "assets/backpack/backpack.obj";
        ASSERT_TRUE(fs::exists(path)) << "Backpack OBJ not found at expected path '" << path << "' (CWD=" << fs::current_path().string() << ")";

        // Make sure the first load is genuinely fresh by invalidating any
        // pre-existing cache. (Mirrors Hazel-style cache key — both
        // unflipped and uv-flipped prefixes covered.)
        MeshCache::InvalidateCache(fs::path(path), "");
        MeshCache::InvalidateCache(fs::path(path), "static_uvflip_v1");
        ASSERT_FALSE(MeshCache::IsMeshCacheValid(fs::path(path), "static_uvflip_v1"));

        const auto outDir = CaptureDir();
        LoadedSnapshot fresh;
        LoadedSnapshot cached;

        {
            Model m(path);
            DumpFirstIndices(m, "FRESH");
            fresh = Snapshot(m, outDir / "backpack_fresh_albedo.png");
        }
        Dump("FRESH", fresh);

        // After the first load the cache file must exist.
        ASSERT_TRUE(MeshCache::IsMeshCacheValid(fs::path(path), "static_uvflip_v1"))
            << "First Model() didn't write the mesh cache — the cache-load path can't be tested.";

        {
            Model m(path);
            DumpFirstIndices(m, "CACHE");
            cached = Snapshot(m, outDir / "backpack_cached_albedo.png");
        }
        Dump("CACHE", cached);

        // The actual invariants. Each ASSERT_EQ below pinpoints which piece
        // of the round-trip is lossy when it fires.
        EXPECT_EQ(fresh.m_VertexCount, cached.m_VertexCount) << "Vertex count drifted between fresh and cache load.";
        EXPECT_EQ(fresh.m_IndexCount, cached.m_IndexCount) << "Index count drifted between fresh and cache load.";
        EXPECT_EQ(fresh.m_SubmeshCount, cached.m_SubmeshCount) << "Submesh count drifted.";
        EXPECT_EQ(fresh.m_MaterialCount, cached.m_MaterialCount) << "Material count drifted.";
        EXPECT_EQ(fresh.m_VertexCRC, cached.m_VertexCRC) << "Vertex DATA differs — UVs/positions/normals scrambled across the cache round-trip.";
        EXPECT_EQ(fresh.m_IndexCRC, cached.m_IndexCRC) << "Index DATA differs across the cache round-trip.";
        EXPECT_EQ(fresh.m_AlbedoPath, cached.m_AlbedoPath) << "Albedo texture path differs.";
        EXPECT_EQ(fresh.m_TextureWidth, cached.m_TextureWidth);
        EXPECT_EQ(fresh.m_TextureHeight, cached.m_TextureHeight);
        EXPECT_EQ(fresh.m_TextureCRC, cached.m_TextureCRC) << "Albedo TEXTURE pixel data differs between fresh and cache load. PNGs were dumped to " << outDir.string();
    }
} // namespace OloEngine::Tests

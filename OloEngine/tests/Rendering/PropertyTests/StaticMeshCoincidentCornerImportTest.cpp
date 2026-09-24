// OLO_TEST_LAYER: integration
//
// The static mesh import must read a triangle whose corners share a position as a
// triangle (issue #1440).
//
// A UV sphere's pole triangles have two corners at the pole. The static route
// (MeshImporterRegistry -> AssimpMeshImporter -> Model, the one every
// MeshComponent.MeshSourceHandle resolves through) ran aiProcess_FindDegenerates, which
// by default does not remove such a triangle: it rewrites it as a LINE face inside the
// same mesh. Model::ProcessMesh appended every face's indices, so each line face
// shifted the rest of the index stream by one index. The 64x48 sphere below came out
// as 6122 "triangles", 3119 of them degenerate, and meshoptimizer asserted on an index
// count that is not a multiple of three. The editor crashed on both backends. The
// AnimatedModel route never ran FindDegenerates, so the same file loaded correctly
// there.
//
// What these tests pin:
//
//   * The static import returns exactly the source's 6144 triangles, compared by corner
//     position and winding, including the zero-area pole triangles. That holds for u16
//     and u32 indices, with and without a glTF material (the crash reproduced without
//     one; the material changes nothing about geometry, but the issue asked for both).
//     The zero-area COUNT is not compared exactly: for the bottom pole it depends on
//     float rounding and on which corner the optimizer puts first.
//   * The warm .omesh load returns the same stream as the cold import. The cache stored
//     the shifted stream before, so a fixed importer behind a warm cache would still
//     serve garbage (OMeshFormat v12 invalidates those files).
//   * The static and AnimatedModel routes agree about what the mesh is.
//   * The asset-pack round trip (what a shipped build reads) keeps every triangle.
//   * A glTF LINES primitive, which Triangulate does not touch, is dropped from the
//     static import rather than spliced into a neighbouring triangle list.
//
// Model::LoadModel builds GPU buffers, so these need a GL context and SKIP without one,
// like their neighbour MeshAssetBoneInfluenceImportTest.

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h" // OLO_ENSURE_GPU_OR_SKIP
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Asset/Interchange/MeshImporterRegistry.h"
#include "OloEngine/Asset/MeshCache.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Renderer/MeshOptimization.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Serialization/AssetPackFile.h"
#include "OloEngine/Serialization/FileStream.h"

#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <tuple>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // The generator from the issue (make_morph_sphere.py), ported: a 64x48 UV sphere
        // with POSITION/NORMAL/TANGENT/TEXCOORD_0 and one POSITION morph target.
        constexpr u32 kSegments = 64;
        constexpr u32 kRings = 48;
        constexpr u32 kTriangleCount = kSegments * kRings * 2;
        // One triangle per segment at the TOP pole has two corners at exactly (0, 1, 0):
        // sin(0) is exactly zero. These are the ones FindDegenerates rewrote as lines. The
        // bottom pole's corners differ by float noise (sin(pi) is not zero), so how many of
        // those count as zero-area depends on rounding and on which corner comes first, so
        // the tests compare triangle sets and use this count only as a floor.
        constexpr u32 kCoincidentCornerTriangleCount = kSegments;

        enum class IndexType
        {
            U16,
            U32
        };

        struct SphereVariant
        {
            IndexType Indices = IndexType::U16;
            bool WithMaterial = false;
        };

        template<typename T>
        void AppendBytes(std::vector<char>& buffer, const T& value)
        {
            const auto* bytes = reinterpret_cast<const char*>(&value);
            buffer.insert(buffer.end(), bytes, bytes + sizeof(T));
        }

        void PadToFour(std::vector<char>& buffer)
        {
            while (buffer.size() % 4 != 0)
            {
                buffer.push_back(0);
            }
        }

        // A small tangent-space normal map for the material variant. Its content is
        // irrelevant here; it exists so the material has a texture to resolve.
        bool WriteNormalMap(const std::filesystem::path& path)
        {
            constexpr int kSize = 16;
            std::vector<unsigned char> pixels(static_cast<sizet>(kSize * kSize * 4));
            for (sizet i = 0; i < pixels.size(); i += 4)
            {
                pixels[i + 0] = 128;
                pixels[i + 1] = 128;
                pixels[i + 2] = 255;
                pixels[i + 3] = 255;
            }
            return stbi_write_png(path.string().c_str(), kSize, kSize, 4, pixels.data(), kSize * 4) != 0;
        }

        struct SphereGeometry
        {
            std::vector<f32> Positions;
            std::vector<f32> Normals;
            std::vector<f32> Tangents;
            std::vector<f32> Uvs;
            std::vector<f32> Deltas;
            std::vector<u32> Indices;
        };

        SphereGeometry BuildSphere()
        {
            SphereGeometry g;
            for (u32 r = 0; r <= kRings; ++r)
            {
                const f64 theta = std::numbers::pi * r / kRings; // 0 at +Y
                for (u32 s = 0; s <= kSegments; ++s)
                {
                    const f64 phi = 2.0 * std::numbers::pi * s / kSegments;
                    const auto x = static_cast<f32>(std::sin(theta) * std::sin(phi));
                    const auto y = static_cast<f32>(std::cos(theta));
                    const auto z = static_cast<f32>(std::sin(theta) * std::cos(phi));
                    g.Positions.insert(g.Positions.end(), { x, y, z });
                    g.Normals.insert(g.Normals.end(), { x, y, z });
                    g.Tangents.insert(g.Tangents.end(), { static_cast<f32>(std::cos(phi)), 0.0f, static_cast<f32>(-std::sin(phi)), 1.0f });
                    g.Uvs.insert(g.Uvs.end(), { static_cast<f32>(s) / kSegments, static_cast<f32>(r) / kRings });
                    const f32 weight = std::max(0.0f, -y) * std::max(0.0f, z) * 0.16f;
                    g.Deltas.insert(g.Deltas.end(), { 0.0f, -0.55f * weight, 1.0f * weight });
                }
            }
            for (u32 r = 0; r < kRings; ++r)
            {
                for (u32 s = 0; s < kSegments; ++s)
                {
                    const u32 a = r * (kSegments + 1) + s;
                    const u32 b = a + kSegments + 1;
                    g.Indices.insert(g.Indices.end(), { a, b, a + 1, a + 1, b, b + 1 });
                }
            }
            return g;
        }

        // A triangle as its three corner positions, rotated so the smallest corner comes
        // first. Rotation keeps the winding, and makes the triangle independent of which
        // corner the optimizer puts first (OptimizeMesh canonicalises rotation, #1223).
        using CanonicalTriangle = std::array<f32, 9>;

        CanonicalTriangle Canonical(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
        {
            const std::array<glm::vec3, 3> corners = { a, b, c };
            const auto less = [](const glm::vec3& l, const glm::vec3& r)
            { return std::tie(l.x, l.y, l.z) < std::tie(r.x, r.y, r.z); };
            sizet first = 0;
            for (sizet i = 1; i < 3; ++i)
            {
                if (less(corners[i], corners[first]))
                    first = i;
            }
            CanonicalTriangle out{};
            for (sizet i = 0; i < 3; ++i)
            {
                const glm::vec3& p = corners[(first + i) % 3];
                out[i * 3 + 0] = p.x;
                out[i * 3 + 1] = p.y;
                out[i * 3 + 2] = p.z;
            }
            return out;
        }

        // The triangles the glTF is written from, as a sorted multiset.
        std::vector<CanonicalTriangle> SourceTriangles()
        {
            const SphereGeometry g = BuildSphere();
            const auto at = [&](u32 i)
            { return glm::vec3(g.Positions[i * 3], g.Positions[i * 3 + 1], g.Positions[i * 3 + 2]); };
            std::vector<CanonicalTriangle> out;
            for (sizet t = 0; t + 2 < g.Indices.size(); t += 3)
            {
                out.push_back(Canonical(at(g.Indices[t]), at(g.Indices[t + 1]), at(g.Indices[t + 2])));
            }
            std::ranges::sort(out);
            return out;
        }

        // Writes <dir>/MorphSphere.gltf + .bin (+ PoreNormal.png) and returns the .gltf path.
        std::filesystem::path WriteMorphSphere(const std::filesystem::path& dir, SphereVariant variant)
        {
            const SphereGeometry g = BuildSphere();
            const std::vector<f32>& positions = g.Positions;
            const std::vector<f32>& normals = g.Normals;
            const std::vector<f32>& tangents = g.Tangents;
            const std::vector<f32>& uvs = g.Uvs;
            const std::vector<f32>& deltas = g.Deltas;
            const std::vector<u32>& indices = g.Indices;
            const auto vertexCount = static_cast<u32>(positions.size() / 3);

            std::vector<char> buffer;
            std::vector<std::pair<sizet, sizet>> views; // offset, length
            const auto addView = [&](const auto& data)
            {
                PadToFour(buffer);
                const sizet offset = buffer.size();
                for (const auto& v : data)
                {
                    AppendBytes(buffer, v);
                }
                views.emplace_back(offset, buffer.size() - offset);
            };
            addView(positions);
            addView(normals);
            addView(tangents);
            addView(uvs);
            addView(deltas);
            if (variant.Indices == IndexType::U16)
            {
                std::vector<u16> narrow(indices.begin(), indices.end());
                addView(narrow);
            }
            else
            {
                addView(indices);
            }

            std::ofstream bin(dir / "MorphSphere.bin", std::ios::binary);
            bin.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            bin.close();

            std::string viewsJson;
            for (sizet i = 0; i < views.size(); ++i)
            {
                viewsJson += fmt::format("{}{{\"buffer\":0,\"byteOffset\":{},\"byteLength\":{}}}", i ? "," : "",
                                         views[i].first, views[i].second);
            }

            const u32 indexComponentType = variant.Indices == IndexType::U16 ? 5123u : 5125u;
            std::string gltf = fmt::format(
                R"({{"asset":{{"version":"2.0","generator":"olo-1440-test"}},)"
                R"("buffers":[{{"byteLength":{},"uri":"MorphSphere.bin"}}],"bufferViews":[{}],)"
                R"("accessors":[)"
                R"({{"bufferView":0,"componentType":5126,"count":{},"type":"VEC3","min":[-1,-1,-1],"max":[1,1,1]}},)"
                R"({{"bufferView":1,"componentType":5126,"count":{},"type":"VEC3"}},)"
                R"({{"bufferView":2,"componentType":5126,"count":{},"type":"VEC4"}},)"
                R"({{"bufferView":3,"componentType":5126,"count":{},"type":"VEC2"}},)"
                R"({{"bufferView":4,"componentType":5126,"count":{},"type":"VEC3","min":[0,-0.1,0],"max":[0,0,0.2]}},)"
                R"({{"bufferView":5,"componentType":{},"count":{},"type":"SCALAR"}}],)"
                R"("meshes":[{{"name":"MorphSphere","weights":[0.0],"extras":{{"targetNames":["Smile"]}},)"
                R"("primitives":[{{"attributes":{{"POSITION":0,"NORMAL":1,"TANGENT":2,"TEXCOORD_0":3}},)"
                R"("indices":5,{}"targets":[{{"POSITION":4}}]}}]}}],)"
                R"("nodes":[{{"mesh":0,"name":"MorphSphere"}}],"scenes":[{{"nodes":[0]}}],"scene":0{}}})",
                buffer.size(), viewsJson, vertexCount, vertexCount, vertexCount, vertexCount, vertexCount,
                indexComponentType, indices.size(), variant.WithMaterial ? R"("material":0,)" : "",
                variant.WithMaterial
                    ? R"(,"images":[{"uri":"PoreNormal.png"}],"textures":[{"source":0}],)"
                      R"("materials":[{"name":"Skin","pbrMetallicRoughness":{"baseColorFactor":[0.66,0.49,0.42,1.0],)"
                      R"("metallicFactor":0.0,"roughnessFactor":0.42},"normalTexture":{"index":0}}])"
                    : "");

            if (variant.WithMaterial)
            {
                EXPECT_TRUE(WriteNormalMap(dir / "PoreNormal.png"));
            }

            const std::filesystem::path path = dir / "MorphSphere.gltf";
            std::ofstream out(path);
            out << gltf;
            return path;
        }

        // One LINES primitive, plus (withTriangle) one triangle primitive in the same glTF mesh.
        std::filesystem::path WriteTriangleWithLines(const std::filesystem::path& dir, bool withTriangle)
        {
            const std::vector<f32> positions = { 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1 };
            const std::vector<u16> triangle = { 0, 1, 2 };
            const std::vector<u16> lines = { 0, 3, 3, 4 };

            std::vector<char> buffer;
            for (f32 v : positions)
                AppendBytes(buffer, v);
            const sizet triangleOffset = buffer.size();
            for (u16 v : triangle)
                AppendBytes(buffer, v);
            PadToFour(buffer);
            const sizet linesOffset = buffer.size();
            for (u16 v : lines)
                AppendBytes(buffer, v);

            std::ofstream bin(dir / "TriangleWithLines.bin", std::ios::binary);
            bin.write(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            bin.close();

            const std::string gltf = fmt::format(
                R"({{"asset":{{"version":"2.0"}},"buffers":[{{"byteLength":{},"uri":"TriangleWithLines.bin"}}],)"
                R"("bufferViews":[{{"buffer":0,"byteOffset":0,"byteLength":60}},)"
                R"({{"buffer":0,"byteOffset":{},"byteLength":6}},{{"buffer":0,"byteOffset":{},"byteLength":8}}],)"
                R"("accessors":[{{"bufferView":0,"componentType":5126,"count":5,"type":"VEC3","min":[0,0,0],"max":[1,1,1]}},)"
                R"({{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}},)"
                R"({{"bufferView":2,"componentType":5123,"count":4,"type":"SCALAR"}}],)"
                R"("meshes":[{{"primitives":[{})"
                R"({{"attributes":{{"POSITION":0}},"indices":2,"mode":1}}]}}],)"
                R"("nodes":[{{"mesh":0}}],"scenes":[{{"nodes":[0]}}],"scene":0}})",
                buffer.size(), triangleOffset, linesOffset,
                withTriangle ? R"({"attributes":{"POSITION":0},"indices":1},)" : "");

            const std::filesystem::path path = dir / "TriangleWithLines.gltf";
            std::ofstream out(path);
            out << gltf;
            return path;
        }

        void MakeColdImport(const std::filesystem::path& path)
        {
            MeshCache::InvalidateCache(path);
            MeshCache::InvalidateCache(path, AnimatedModel::kCachePrefix);
        }

        // The stream is a triangle list over the vertex array, every position is finite,
        // and the degenerate census matches the source.
        ::testing::AssertionResult IsTheSourceSphere(const MeshSource& source)
        {
            const auto& indices = source.GetIndices();
            const auto& vertices = source.GetVertices();
            if (indices.Num() % 3 != 0)
            {
                return ::testing::AssertionFailure()
                       << indices.Num() << " indices is not a whole number of triangles — a non-triangle face "
                                           "was spliced into the stream";
            }
            for (i32 i = 0; i < indices.Num(); ++i)
            {
                if (indices[i] >= static_cast<u32>(vertices.Num()))
                {
                    return ::testing::AssertionFailure() << "index " << i << " = " << indices[i]
                                                         << " addresses past the " << vertices.Num() << " vertices";
                }
            }
            for (i32 i = 0; i < vertices.Num(); ++i)
            {
                const glm::vec3& p = vertices[i].Position;
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
                {
                    return ::testing::AssertionFailure() << "vertex " << i << " has a non-finite position";
                }
            }

            // The same triangles, not merely the same number: an import that shifts its index
            // stream (the #1440 misread) keeps every index in range and can keep the count.
            static const std::vector<CanonicalTriangle> expected = SourceTriangles();
            std::vector<CanonicalTriangle> actual;
            for (i32 t = 0; t + 2 < indices.Num(); t += 3)
            {
                actual.push_back(Canonical(vertices[indices[t]].Position, vertices[indices[t + 1]].Position,
                                           vertices[indices[t + 2]].Position));
            }
            std::ranges::sort(actual);
            if (actual.size() != expected.size())
            {
                return ::testing::AssertionFailure() << actual.size() << " triangles imported; the source has "
                                                     << expected.size() << " (the #1440 misread gave 6122)";
            }
            // Positions pass through the import unchanged today, but compare with a tolerance
            // so a ULP-level change in import math is not mistaken for a misread.
            constexpr f32 kPositionTolerance = 1e-6f;
            for (sizet t = 0; t < expected.size(); ++t)
            {
                for (sizet k = 0; k < expected[t].size(); ++k)
                {
                    if (std::abs(actual[t][k] - expected[t][k]) > kPositionTolerance)
                    {
                        return ::testing::AssertionFailure()
                               << "the imported triangles are not the source's: " << t << " of " << expected.size()
                               << " sorted triangles match before the first difference";
                    }
                }
            }

            // The triangles whose corners coincide EXACTLY (the top pole) are zero-area
            // whatever corner comes first, so they are a floor the census must reach.
            const DegenerateTriangleStats stats = MeshOptimization::AnalyzeDegenerateTriangles(source);
            if (stats.ZeroAreaCount < kCoincidentCornerTriangleCount)
            {
                return ::testing::AssertionFailure() << "only " << stats.ZeroAreaCount
                                                     << " zero-area triangles; the top pole alone has "
                                                     << kCoincidentCornerTriangleCount;
            }
            return ::testing::AssertionSuccess();
        }
    } // namespace

    // The expectation every import test below compares against must itself describe the
    // sphere, or a match proves nothing. No GPU needed.
    TEST(StaticMeshCoincidentCornerSource, SourceHasEveryTriangleAndTheCoincidentPoleTriangles)
    {
        const std::vector<CanonicalTriangle> triangles = SourceTriangles();
        EXPECT_EQ(triangles.size(), static_cast<sizet>(kTriangleCount));

        // Two corners closer than any real edge of this sphere (the shortest is ~0.005) —
        // a distance rather than a bit compare, because the top pole mixes 0.0 and -0.0.
        // Both poles qualify: 64 + 64, the issue's "128 of 6144".
        u32 collapsed = 0;
        for (const CanonicalTriangle& t : triangles)
        {
            const glm::vec3 a(t[0], t[1], t[2]), b(t[3], t[4], t[5]), c(t[6], t[7], t[8]);
            constexpr f32 kCollapsed = 1e-6f;
            if (glm::distance(a, b) < kCollapsed || glm::distance(b, c) < kCollapsed || glm::distance(a, c) < kCollapsed)
                ++collapsed;
        }
        EXPECT_EQ(collapsed, 2 * kCoincidentCornerTriangleCount)
            << "the fixture no longer has the pole triangles whose corners collapse";
    }

    // A temporary project, so MeshCache writes its .omesh files under the test's own temp
    // directory rather than a path relative to the working directory.
    class ImportProjectFixture : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_TempDir = Tests::TempDir();

            std::error_code ec;
            std::filesystem::create_directories(m_TempDir / "Assets", ec);
            ASSERT_FALSE(ec) << "Failed to create temp dir: " << ec.message();

            const std::filesystem::path projectFile = m_TempDir / "Test.oloproj";
            {
                std::ofstream proj(projectFile);
                proj << "Project:\n"
                        "  Name: StaticMeshCoincidentCornerImport\n"
                        "  StartScene: \"\"\n"
                        "  AssetDirectory: \"Assets\"\n"
                        "  ScriptModulePath: \"\"\n";
            }
            ASSERT_TRUE(Project::Load(projectFile)) << "Project::Load failed for " << m_TempDir.string();

            m_AssetManager = Ref<EditorAssetManager>::Create();
            m_AssetManager->Initialize(/*startFileWatcher=*/false);
            Project::SetAssetManager(m_AssetManager);
        }

        void TearDown() override
        {
            // Project::Load and SetAssetManager write process-wide statics; MeshCache resolves
            // its directory through the active project, so leave none pointing at a deleted tree.
            Project::Unload();
            m_AssetManager.Reset();
            std::error_code ec;
            std::filesystem::remove_all(m_TempDir, ec);
        }

        Ref<MeshSource> RoundTripThroughAssetPack(AssetHandle handle)
        {
            const std::filesystem::path packPath = m_TempDir / "mesh.pack";
            MeshSourceSerializer serializer;
            AssetSerializationInfo info{};
            {
                FileStreamWriter writer(packPath);
                EXPECT_TRUE(writer.IsStreamGood());
                EXPECT_TRUE(serializer.SerializeToAssetPack(handle, writer, info));
            }

            AssetPackFile::AssetInfo assetInfo{};
            assetInfo.Handle = static_cast<AssetHandle>(0x1440ULL);
            assetInfo.PackedOffset = info.Offset;
            assetInfo.PackedSize = info.Size;
            assetInfo.Type = AssetType::MeshSource;

            FileStreamReader reader(packPath);
            EXPECT_TRUE(reader.IsStreamGood());
            reader.SetArchiveVersion(AssetPackFile::Version);
            return serializer.DeserializeFromAssetPack(reader, assetInfo).As<MeshSource>();
        }

        std::filesystem::path AssetsDir() const
        {
            return m_TempDir / "Assets";
        }

        std::filesystem::path m_TempDir;
        Ref<EditorAssetManager> m_AssetManager;
    };

    class StaticMeshCoincidentCornerImportTest : public ImportProjectFixture, public ::testing::WithParamInterface<SphereVariant>
    {
    };

    TEST_P(StaticMeshCoincidentCornerImportTest, ColdStaticImportKeepsEveryTriangle)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const std::filesystem::path path = WriteMorphSphere(AssetsDir(), GetParam());
        MakeColdImport(path);

        MeshImportResult const result = MeshImporterRegistry::Get().Import(path);
        ASSERT_TRUE(result.Succeeded()) << result.Error;
        ASSERT_TRUE(result.Source);
        EXPECT_FALSE(result.Source->IsSourceRigged()) << "no bones in the file — this must be the STATIC route";
        EXPECT_TRUE(IsTheSourceSphere(*result.Source));
    }

    TEST_P(StaticMeshCoincidentCornerImportTest, WarmCacheLoadReturnsTheColdStream)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const std::filesystem::path path = WriteMorphSphere(AssetsDir(), GetParam());
        MakeColdImport(path);

        MeshImportResult const cold = MeshImporterRegistry::Get().Import(path);
        ASSERT_TRUE(cold.Succeeded()) << cold.Error;
        ASSERT_TRUE(IsTheSourceSphere(*cold.Source)) << "the cold half failed; the warm comparison would be vacuous";
        ASSERT_TRUE(MeshCache::IsMeshCacheValid(path, "")) << "the cold import wrote no static .omesh";

        MeshImportResult const warm = MeshImporterRegistry::Get().Import(path);
        ASSERT_TRUE(warm.Succeeded()) << warm.Error;
        EXPECT_TRUE(IsTheSourceSphere(*warm.Source));

        const auto& coldIndices = cold.Source->GetIndices();
        const auto& warmIndices = warm.Source->GetIndices();
        ASSERT_EQ(warmIndices.Num(), coldIndices.Num());
        EXPECT_EQ(std::memcmp(warmIndices.GetData(), coldIndices.GetData(), sizeof(u32) * static_cast<sizet>(coldIndices.Num())), 0)
            << "the warm .omesh load returned a different index stream from the cold import";
    }

    TEST_P(StaticMeshCoincidentCornerImportTest, StaticAndAnimatedRoutesAgree)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const std::filesystem::path path = WriteMorphSphere(AssetsDir(), GetParam());
        MakeColdImport(path);

        MeshImportResult const staticRoute = MeshImporterRegistry::Get().Import(path);
        ASSERT_TRUE(staticRoute.Succeeded()) << staticRoute.Error;

        // The route AnimationStateComponent.SourceFilePath takes, which loaded this file
        // correctly all along.
        AnimatedModel animated(path.string());
        Ref<MeshSource> const animatedRoute = animated.CreateCombinedMeshSource();
        ASSERT_TRUE(animatedRoute);
        EXPECT_TRUE(IsTheSourceSphere(*animatedRoute)) << "the AnimatedModel route regressed";

        EXPECT_TRUE(IsTheSourceSphere(*staticRoute.Source)) << "the static route disagrees with the source";
    }

    TEST_P(StaticMeshCoincidentCornerImportTest, AssetPackRoundTripKeepsEveryTriangle)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const std::filesystem::path path = WriteMorphSphere(AssetsDir(), GetParam());
        MakeColdImport(path);

        MeshImportResult const result = MeshImporterRegistry::Get().Import(path);
        ASSERT_TRUE(result.Succeeded()) << result.Error;
        ASSERT_TRUE(IsTheSourceSphere(*result.Source)) << "the import half failed; the pack half would be vacuous";

        // The asset pipeline Builds an imported source before it is packed.
        Ref<MeshSource> imported = result.Source;
        imported->Build();
        AssetHandle const handle = AssetManager::AddMemoryOnlyAsset(imported);
        Ref<MeshSource> const unpacked = RoundTripThroughAssetPack(handle);
        ASSERT_TRUE(unpacked) << "DeserializeFromAssetPack returned null";
        EXPECT_TRUE(IsTheSourceSphere(*unpacked));
    }

    INSTANTIATE_TEST_SUITE_P(IndexTypeAndMaterial, StaticMeshCoincidentCornerImportTest,
                             ::testing::Values(SphereVariant{ IndexType::U16, false }, SphereVariant{ IndexType::U32, false },
                                               SphereVariant{ IndexType::U16, true }, SphereVariant{ IndexType::U32, true }),
                             [](const ::testing::TestParamInfo<SphereVariant>& info)
                             {
                                 return std::string(info.param.Indices == IndexType::U16 ? "U16" : "U32") +
                                        (info.param.WithMaterial ? "_WithMaterial" : "_NoMaterial");
                             });

    // A glTF LINES primitive reaches Assimp as a mesh of two-index faces, which
    // Triangulate leaves alone. The static import draws triangles only: it must drop the
    // lines, not append them to the index stream.
    class StaticMeshLinePrimitiveImport : public ImportProjectFixture
    {
    };

    TEST_F(StaticMeshLinePrimitiveImport, LinesPrimitiveIsDroppedNotSplicedIntoTheTriangles)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const std::filesystem::path path = WriteTriangleWithLines(AssetsDir(), /*withTriangle=*/true);
        MakeColdImport(path);

        MeshImportResult const result = MeshImporterRegistry::Get().Import(path);
        ASSERT_TRUE(result.Succeeded()) << result.Error;
        ASSERT_TRUE(result.Source);

        const auto& indices = result.Source->GetIndices();
        EXPECT_EQ(indices.Num(), 3) << "expected the one triangle and nothing from the LINES primitive";
        for (i32 i = 0; i < indices.Num(); ++i)
        {
            EXPECT_LT(indices[i], static_cast<u32>(result.Source->GetVertices().Num()));
        }
    }

    // The AnimatedModel route removes line and point meshes the same way. Skipping their
    // faces one at a time instead left a mesh with no indices, which MeshSource::Build
    // dereferenced a null index buffer for.
    TEST_F(StaticMeshLinePrimitiveImport, AnimatedRouteDropsTheLinesPrimitiveToo)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const std::filesystem::path path = WriteTriangleWithLines(AssetsDir(), /*withTriangle=*/true);
        MakeColdImport(path);

        AnimatedModel animated(path.string());
        Ref<MeshSource> const source = animated.CreateCombinedMeshSource();
        ASSERT_TRUE(source);

        const auto& indices = source->GetIndices();
        EXPECT_EQ(indices.Num(), 3) << "expected the one triangle and nothing from the LINES primitive";
        for (i32 i = 0; i < indices.Num(); ++i)
        {
            EXPECT_LT(indices[i], static_cast<u32>(source->GetVertices().Num()));
        }
    }

    // A file with nothing but lines has nothing a static mesh can draw. The import must
    // fail and say so, not crash and not hand back an empty or line-shaped mesh.
    TEST_F(StaticMeshLinePrimitiveImport, ALinesOnlyFileFailsToImportInsteadOfCrashing)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const std::filesystem::path path = WriteTriangleWithLines(AssetsDir(), /*withTriangle=*/false);
        MakeColdImport(path);

        MeshImportResult const result = MeshImporterRegistry::Get().Import(path);
        EXPECT_FALSE(result.Succeeded()) << "a lines-only file imported as a static mesh";
        EXPECT_FALSE(result.Error.empty());
    }

    // Build used to bind the index buffer it never created for an empty index list: a mesh
    // an importer had emptied crashed on upload instead of being refused.
    TEST(StaticMeshBuildRefusal, BuildLeavesAMeshWithNoIndicesUnbuilt)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        auto mesh = Ref<MeshSource>::Create();
        mesh->GetVertices().Add(Vertex{});
        mesh->GetVertices().Add(Vertex{});
        mesh->GetVertices().Add(Vertex{});

        mesh->Build();

        EXPECT_FALSE(mesh->IsBuilt());
    }
} // namespace OloEngine::Tests

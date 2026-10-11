// OLO_TEST_LAYER: integration
// =============================================================================
// GroomAnimalsCookedBindingTest -- the COOKED bindings GroomAnimals.olo ships
// must accept the bodies the editor loads, cold and warm.
//
// GroomAnimalsAcceptanceEvidenceTest grows and cooks every coat in-process
// against a fresh import, so its bindings always match. The committed
// .ologroombinding files are a different artefact: they were cooked once and
// are read by the editor against whatever the importer produces today. When
// the two drift, the editor logs "Groom binding refused ... not the surface
// this binding was built against" and the coat stands at its bind pose while
// the body walks (#1259 saw it for both horses).
//
// Each model is copied into this test's temp directory, as
// AnimatedModelCacheSurfaceTest does, so invalidating its cache cannot pull
// the committed model's cache out from under another test process.
// =============================================================================

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h" // OLO_ENSURE_GPU_OR_SKIP
#include "TestTempDir.h"

#include <gtest/gtest.h>

#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Asset/MeshCache.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBinding.h"
#include "OloEngine/Groom/GroomBindingBuilder.h"
#include "OloEngine/Groom/GroomSurfaceFrame.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Renderer/MeshSource.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <type_traits>
#include <vector>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        // The surface exactly as Scene::MakeGroomSurfaceView builds it.
        [[nodiscard]] GroomSurfaceView SurfaceView(const MeshSource& surface, const Skeleton* skeleton)
        {
            GroomSurfaceView view;
            const auto& vertices = surface.GetVertices();
            const auto& indices = surface.GetIndices();
            view.PositionData = reinterpret_cast<const std::byte*>(vertices.GetData());
            view.PositionStride = static_cast<u32>(sizeof(Vertex));
            view.VertexCount = static_cast<u32>(vertices.Num());
            view.Indices = indices.GetData();
            view.IndexCount = static_cast<u32>(indices.Num());
            view.BoneCount = skeleton != nullptr ? static_cast<u32>(skeleton->m_FinalBoneMatrices.size()) : 0u;
            view.SkeletonNameHash = skeleton != nullptr ? HashGroomSkeletonNames(skeleton->m_BoneNames) : 0u;
            return view;
        }

        [[nodiscard]] std::string Describe(const GroomBindingTargetSignature& s)
        {
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer), "verts %u idx %u bones %u indexHash %016llx skelHash %016llx restHash %016llx",
                          s.VertexCount, s.IndexCount, s.BoneCount, static_cast<unsigned long long>(s.IndexHash),
                          static_cast<unsigned long long>(s.SkeletonNameHash),
                          static_cast<unsigned long long>(s.RestPositionHash));
            return buffer;
        }

        // A committed .ologroom / .ologroombinding, read with the shipped decoder.
        template<typename TAsset>
        [[nodiscard]] Ref<TAsset> LoadCooked(const std::string& name)
        {
            const fs::path file = fs::path{ OLO_TEST_EDITOR_ROOT } / "SandboxProject/Assets/Grooms/Animals" / name;
            std::ifstream in(file, std::ios::binary);
            const std::vector<u8> bytes{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
            Ref<TAsset> asset;
            std::string reason;
            bool decoded = false;
            if constexpr (std::is_same_v<TAsset, GroomAsset>)
                decoded = GroomSerializer::DecodeFromBytes(bytes.data(), bytes.size(), asset, reason);
            else
                decoded = GroomBindingSerializer::DecodeFromBytes(bytes.data(), bytes.size(), asset, reason);
            EXPECT_TRUE(decoded) << file.string() << ": " << reason;
            return asset;
        }

        struct Subject
        {
            const char* ModelDir;           // relative to OloEditor/
            const char* ModelFile;          // inside ModelDir
            std::vector<const char*> Coats; // <coat>.ologroom + <coat>.ologroombinding
        };
    } // namespace

    TEST(GroomAnimalsCookedBinding, EveryCookedBindingAcceptsTheBodyColdAndWarm)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const std::vector<Subject> subjects = {
            { "SandboxProject/Assets/Models/Horse", "Horse.gltf", { "ShortCoatHorse", "LongCoatHorse" } },
            { "assets/models/InfiniteScanHead", "HeadRigged.gltf", { "Human" } },
        };

        for (const Subject& subject : subjects)
        {
            const fs::path source = fs::path{ OLO_TEST_EDITOR_ROOT } / subject.ModelDir;
            const fs::path copy = TempDir(std::string("cooked-binding-") + source.filename().string());
            std::error_code ec;
            fs::copy(source, copy, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            ASSERT_FALSE(ec) << ec.message();
            const fs::path path = copy / subject.ModelFile;
            SCOPED_TRACE(path.string());
            MeshCache::InvalidateCache(path);
            MeshCache::InvalidateCache(path, AnimatedModel::kCachePrefix);

            const Ref<AnimatedModel> cold = Ref<AnimatedModel>::Create(path.string());
            ASSERT_FALSE(cold->WasMeshLoadedFromCache());
            const Ref<AnimatedModel> warm = Ref<AnimatedModel>::Create(path.string());
            ASSERT_TRUE(warm->WasMeshLoadedFromCache());

            const GroomBindingTargetSignature coldSignature =
                GroomBindingBuilder::SignTarget(SurfaceView(*cold->GetEntityMeshSource(), cold->GetSkeleton().Raw()));
            const GroomBindingTargetSignature warmSignature =
                GroomBindingBuilder::SignTarget(SurfaceView(*warm->GetEntityMeshSource(), warm->GetSkeleton().Raw()));
            std::printf("[cooked-binding] %s cold: %s\n", subject.ModelFile, Describe(coldSignature).c_str());
            std::printf("[cooked-binding] %s warm: %s\n", subject.ModelFile, Describe(warmSignature).c_str());
            EXPECT_TRUE(coldSignature.MatchesTopology(warmSignature))
                << subject.ModelFile << ": the warm load is a different surface from the cold import";

            for (const char* name : subject.Coats)
            {
                const Ref<GroomBindingAsset> binding = LoadCooked<GroomBindingAsset>(std::string(name) + ".ologroombinding");
                const Ref<GroomAsset> groom = LoadCooked<GroomAsset>(std::string(name) + ".ologroom");
                ASSERT_TRUE(binding && groom) << name;
                std::printf("[cooked-binding] %s cooked: %s\n", name, Describe(binding->GetTargetSignature()).c_str());
                // The verdict the Scene reaches when it attaches the coat, against
                // both loads of the body.
                const GroomBindingSourceSignature source = GroomBindingBuilder::SignGroom(*groom);
                const GroomBindingRejectReason cold = binding->CheckCompatibility(source, coldSignature);
                const GroomBindingRejectReason warm = binding->CheckCompatibility(source, warmSignature);
                EXPECT_EQ(cold, GroomBindingRejectReason::None)
                    << name << " refuses a cold import of " << subject.ModelFile << ": " << ToString(cold);
                EXPECT_EQ(warm, GroomBindingRejectReason::None)
                    << name << " refuses a warm load of " << subject.ModelFile << ": " << ToString(warm);
            }
            MeshCache::InvalidateCache(path);
            MeshCache::InvalidateCache(path, AnimatedModel::kCachePrefix);
        }
    }
} // namespace OloEngine::Tests

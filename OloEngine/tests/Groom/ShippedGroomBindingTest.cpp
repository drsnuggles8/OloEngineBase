// OLO_TEST_LAYER: integration
// =============================================================================
// ShippedGroomBindingTest -- every committed .ologroombinding must accept the
// body it ships with, checked on the CPU so that it runs on every PR.
//
// A binding is cooked once against a body and then read by the editor against
// whatever the importer makes of that body today, on whatever platform it runs.
// When the two disagree the coat is refused ("not the surface this binding was
// built against") and stands at its bind pose. The Samoyed's was refused on
// the AMD box for two weeks before the weekly showcase said so (#1674): that
// box's GCC fused meshoptimizer's float math into FMAs, which reordered one
// submesh, and the GPU suites that check bindings (DogShowcaseEvidenceTest,
// GroomAnimalsCookedBinding) skip on every hosted runner. Each case prints
// every submesh's index hash, so a platform that disagrees names the submesh. The check itself needs no GPU: with no graphics
// device MeshSource::Build keeps the CPU data, and AnimatedModel optimises it
// to the surface a device-backed import builds (#1223).
//
// Each model is copied into this test's temp directory, as
// GroomAnimalsCookedBindingTest does, so its cold import and the cache it
// writes cannot race another test process over the committed model's cache.
// =============================================================================

#include "OloEnginePCH.h"

#include "TestTempDir.h"

#include <gtest/gtest.h>

#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Asset/MeshCache.h"
#include "OloEngine/Groom/GroomMeshSurface.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBinding.h"
#include "OloEngine/Groom/GroomBindingBuilder.h"
#include "OloEngine/Groom/GroomSurfaceFrame.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Renderer/MeshSource.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
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

        const fs::path kGrooms = fs::path{ OLO_TEST_EDITOR_ROOT } / "SandboxProject/Assets/Grooms";

        [[nodiscard]] std::string Describe(const GroomBindingTargetSignature& s)
        {
            char buffer[256];
            std::snprintf(buffer, sizeof(buffer), "verts %u idx %u bones %u indexHash %016llx skelHash %016llx restHash %016llx",
                          s.VertexCount, s.IndexCount, s.BoneCount, static_cast<unsigned long long>(s.IndexHash),
                          static_cast<unsigned long long>(s.SkeletonNameHash),
                          static_cast<unsigned long long>(s.RestPositionHash));
            return buffer;
        }

        // Each submesh's slice of the index buffer, hashed as SignTarget hashes
        // the whole: when a platform's IndexHash differs, this names the
        // submesh whose triangle order moved.
        void PrintSubmeshIndexHashes(const char* modelFile, const MeshSource& surface)
        {
            const auto& indices = surface.GetIndices();
            const auto& submeshes = surface.GetSubmeshes();
            for (i32 s = 0; s < submeshes.Num(); ++s)
            {
                const Submesh& submesh = submeshes[s];
                const bool inRange = static_cast<sizet>(submesh.m_BaseIndex) + submesh.m_IndexCount <= static_cast<sizet>(indices.Num());
                const u64 hash = inRange ? GroomBindingBuilder::HashBytes(indices.GetData() + submesh.m_BaseIndex,
                                                                          static_cast<sizet>(submesh.m_IndexCount) * sizeof(u32))
                                         : 0u;
                std::printf("[shipped-binding] %s submesh %d %s: baseVertex %u vertices %u baseIndex %u indices %u indexHash %016llx\n",
                            modelFile, s, submesh.m_MeshName.ToStdString().c_str(), submesh.m_BaseVertex, submesh.m_VertexCount,
                            submesh.m_BaseIndex, submesh.m_IndexCount, static_cast<unsigned long long>(hash));
            }
        }

        // A committed .ologroom / .ologroombinding, read with the shipped decoder.
        template<typename TAsset>
        [[nodiscard]] Ref<TAsset> LoadCooked(const fs::path& file)
        {
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

        struct Body
        {
            const char* ModelDir;           // relative to OloEditor/
            const char* ModelFile;          // inside ModelDir
            std::vector<const char*> Coats; // relative to Assets/Grooms, without the extension
        };

        // Every committed binding that is cooked against a model. Keep this in
        // step with the scenes that pair them; ListsEveryShippedBinding fails
        // when a new binding is committed without an entry here.
        const std::vector<Body>& Bodies()
        {
            static const std::vector<Body> bodies = {
                { "SandboxProject/Assets/Models/Dog", "Dog.gltf", { "Dog/Dog" } },
                { "SandboxProject/Assets/Models/Samoyed", "Samoyed.gltf", { "Samoyed/Samoyed" } },
                { "SandboxProject/Assets/Models/Bernese", "Bernese.gltf", { "Bernese/Bernese" } },
                { "SandboxProject/Assets/Models/Horse", "Horse.gltf", { "Animals/ShortCoatHorse", "Animals/LongCoatHorse" } },
                { "assets/models/InfiniteScanHead", "HeadRigged.gltf", { "Animals/Human" } },
                { "assets/models/Fox", "Fox.gltf", { "Animals/FoxShortCoat", "Animals/FoxLongCoat" } },
            };
            return bodies;
        }

        // Bound to a procedural body rather than a model; pinned on the CPU by
        // GroomBindingRoundTrip.ReferenceBristleBodyBindingMatchesTheCurrentSphere.
        constexpr const char* kProceduralBindings[] = { "reference-human-scalp-BristleBody" };

        // The model's geometry only: the .gltf and its .bin buffers, which are
        // all a binding is signed against. Without its textures beside it the
        // importer never reaches Texture2D::Create, which this process cannot
        // serve: it has the OpenGL API selected and no context, so the upload
        // dereferences an unloaded GL entry point.
        void CopyModelGeometry(const fs::path& from, const fs::path& to)
        {
            fs::create_directories(to);
            for (const auto& entry : fs::directory_iterator(from))
            {
                if (entry.is_regular_file() && (entry.path().extension() == ".gltf" || entry.path().extension() == ".bin"))
                {
                    fs::copy_file(entry.path(), to / entry.path().filename(), fs::copy_options::overwrite_existing);
                }
            }
        }
    } // namespace

    TEST(ShippedGroomBinding, ListsEveryShippedBinding)
    {
        std::set<std::string> listed(std::begin(kProceduralBindings), std::end(kProceduralBindings));
        for (const Body& body : Bodies())
        {
            listed.insert(body.Coats.begin(), body.Coats.end());
        }
        std::set<std::string> shipped;
        for (const auto& entry : fs::recursive_directory_iterator(kGrooms))
        {
            if (entry.path().extension() == ".ologroombinding")
            {
                shipped.insert(fs::relative(entry.path(), kGrooms).replace_extension().generic_string());
            }
        }
        ASSERT_FALSE(shipped.empty()) << kGrooms.string();
        for (const std::string& name : shipped)
        {
            EXPECT_TRUE(listed.contains(name)) << name << ".ologroombinding ships but no body is checked against it";
        }
        for (const std::string& name : listed)
        {
            EXPECT_TRUE(shipped.contains(name)) << name << " is listed but no longer ships";
        }
    }

    // One body per case, so each is its own ctest entry under CI's per-entry
    // timeout and a refusal names the body in the test name.
    class ShippedGroomBindingBody : public ::testing::TestWithParam<sizet>
    {
    };

    TEST_P(ShippedGroomBindingBody, EveryCoatAcceptsTheBodyColdAndWarm)
    {
        const Body& body = Bodies()[GetParam()];
        const fs::path source = fs::path{ OLO_TEST_EDITOR_ROOT } / body.ModelDir;
        const fs::path copy = TempDir(std::string("shipped-binding-") + source.filename().string());
        ASSERT_NO_THROW(CopyModelGeometry(source, copy)) << source.string();
        const fs::path path = copy / body.ModelFile;
        SCOPED_TRACE(path.string());
        MeshCache::InvalidateCache(path);
        MeshCache::InvalidateCache(path, AnimatedModel::kCachePrefix);

        GroomBindingTargetSignature coldSignature;
        GroomBindingTargetSignature warmSignature;
        {
            const Ref<AnimatedModel> cold = Ref<AnimatedModel>::Create(path.string());
            ASSERT_TRUE(cold->GetEntityMeshSource() && cold->GetSkeleton()) << body.ModelFile;
            ASSERT_FALSE(cold->WasMeshLoadedFromCache());
            coldSignature = GroomBindingBuilder::SignTarget(MakeGroomSurfaceView(*cold->GetEntityMeshSource(), cold->GetSkeleton().Raw()));
            PrintSubmeshIndexHashes(body.ModelFile, *cold->GetEntityMeshSource());
        }
        {
            const Ref<AnimatedModel> warm = Ref<AnimatedModel>::Create(path.string());
            ASSERT_TRUE(warm->GetEntityMeshSource() && warm->GetSkeleton()) << body.ModelFile;
            ASSERT_TRUE(warm->WasMeshLoadedFromCache());
            warmSignature = GroomBindingBuilder::SignTarget(MakeGroomSurfaceView(*warm->GetEntityMeshSource(), warm->GetSkeleton().Raw()));
        }
        std::printf("[shipped-binding] %s cold: %s\n", body.ModelFile, Describe(coldSignature).c_str());
        std::printf("[shipped-binding] %s warm: %s\n", body.ModelFile, Describe(warmSignature).c_str());

        for (const char* coat : body.Coats)
        {
            // One coat at a time: the Samoyed's pair alone is ~170 MB on disk.
            const Ref<GroomBindingAsset> binding = LoadCooked<GroomBindingAsset>(kGrooms / (std::string(coat) + ".ologroombinding"));
            const Ref<GroomAsset> groom = LoadCooked<GroomAsset>(kGrooms / (std::string(coat) + ".ologroom"));
            ASSERT_TRUE(binding && groom) << coat;
            std::printf("[shipped-binding] %s cooked: %s\n", coat, Describe(binding->GetTargetSignature()).c_str());
            // The verdict the Scene reaches when it attaches the coat.
            const GroomBindingSourceSignature signature = GroomBindingBuilder::SignGroom(*groom);
            const GroomBindingRejectReason cold = binding->CheckCompatibility(signature, coldSignature);
            const GroomBindingRejectReason warm = binding->CheckCompatibility(signature, warmSignature);
            EXPECT_EQ(cold, GroomBindingRejectReason::None)
                << coat << " refuses a cold import of " << body.ModelFile << ": " << ToString(cold)
                << ". If the model changed, re-cook the binding with it (tools/dog-authoring/rebuild.py for a breed);"
                << " if only one platform refuses, compare the [shipped-binding] submesh hashes across platforms"
                << " and check the ports' FP flags (docs/agent-rules/vcpkg-dependency-management.md rule 21).";
            EXPECT_EQ(warm, GroomBindingRejectReason::None)
                << coat << " refuses a warm load of " << body.ModelFile << ": " << ToString(warm);
        }
        MeshCache::InvalidateCache(path);
        MeshCache::InvalidateCache(path, AnimatedModel::kCachePrefix);
    }

    INSTANTIATE_TEST_SUITE_P(Bodies, ShippedGroomBindingBody, ::testing::Range<sizet>(0, Bodies().size()),
                             [](const ::testing::TestParamInfo<sizet>& info)
                             {
                                 std::string name = fs::path(Bodies()[info.param].ModelFile).stem().string();
                                 std::erase_if(name, [](char c)
                                               { return !std::isalnum(static_cast<unsigned char>(c)); });
                                 return name;
                             });
} // namespace OloEngine::Tests

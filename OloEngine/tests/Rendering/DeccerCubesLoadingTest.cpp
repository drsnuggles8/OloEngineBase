// =============================================================================
// DeccerCubesLoadingTest — the "deccer cubes" import-robustness suite.
//
// What the cubes are
// ------------------
//   The "deccer cubes" (github.com/deccer/deccer-cubes) are a community-built
//   set of glTF/FBX/GLB files that engines load to prove their importers
//   handle the awkward edges the format permits:
//     * geometry-only mesh with a sidecar .bin
//     * vertex-color / per-material color (no textures)
//     * sidecar PNG textures referenced by relative URI
//     * base64-embedded buffers and base64-embedded textures
//     * non-identity scene-node rotations
//     * single mesh referencing a shared atlas
//
// Each variant exercises a different code path in `Model::LoadModel`, and any
// one of them silently breaking is a regression that's invisible until a user
// drops an exported file into the editor.
//
// What this test pins
// -------------------
//   For every shipped variant in OloEditor/assets/models/DeccerCubes/:
//     * Assimp parses the file without erroring.
//     * The loader produces at least one Mesh.
//     * The aggregated bounding box has finite, non-zero extent.
//   That's deliberately the minimum useful contract — anything stronger would
//   re-test Assimp itself. What we actually own is "we feed paths to Assimp
//   and walk the resulting scene without dropping data on the floor."
//
// Why "integration" classification, not Functional
// ------------------------------------------------
//   `Model::LoadModel` → `MeshSource::Build()` allocates GL VertexArray /
//   VertexBuffer / IndexBuffer objects. That requires a live GL context, so
//   the headless Functional fixture cannot run it. We use the lightweight
//   `RenderPropertyFixture` (hidden-window GL 4.6, no full Renderer::Init)
//   and skip when no GPU is available — same pattern as the property tests.
// =============================================================================

#include "OloEnginePCH.h"

#include "PropertyTests/RenderPropertyTest.h"

#include "OloEngine/Asset/MeshCache.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Renderer/Texture.h"

#include <glad/gl.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Tests
{
    namespace
    {
        struct DeccerCubeVariant
        {
            std::string_view Name;         // GTest test-name suffix
            std::string_view RelativePath; // relative to OloEditor/
            bool ExpectsAlbedoTexture;     // true iff the variant ships albedo maps (sidecar or embedded)
        };

        // Resolves to an absolute path so the test does not depend on which
        // working directory ctest happens to set when invoking the binary.
        std::filesystem::path EditorRoot()
        {
            return std::filesystem::path{ OLO_TEST_EDITOR_ROOT };
        }

        // gtest renames parameter suffixes by stripping non-alphanumerics, so
        // any disallowed character in `Name` becomes silent corruption of the
        // discovered test list. Keep `Name` to [A-Za-z0-9_].
        class DeccerCubesLoaderFixture : public ::testing::TestWithParam<DeccerCubeVariant>
        {
        };

        TEST_P(DeccerCubesLoaderFixture, LoadsWithMeshesAndFiniteBounds)
        {
            OLO_ENSURE_GPU_OR_SKIP();

            const auto param = GetParam();
            const auto absolutePath = EditorRoot() / param.RelativePath;

            ASSERT_TRUE(std::filesystem::exists(absolutePath))
                << "Fixture file missing: " << absolutePath.string()
                << " — was the deccer-cubes asset tree deleted, or did the CMake "
                   "WORKING_DIRECTORY change?";

            // Construct the model directly. Going through ModelComponent /
            // SceneSerializer would re-test the YAML path, which has its own
            // coverage. Here we want the loader, end-to-end.
            Model model{ absolutePath.string() };

            // GetMeshCount() == 0 is how Model::LoadModel signals that Assimp
            // rejected the file (it logs an error and early-returns). Any
            // future change that makes the loader silently produce zero meshes
            // for a real file shows up here, not in editor playtests.
            const auto meshCount = model.GetMeshCount();
            ASSERT_GT(meshCount, 0u)
                << "Loader produced no meshes for " << param.Name
                << " (" << absolutePath.string() << "). "
                                                    "Check the most recent Assimp upgrade or any change to the "
                                                    "post-process flag set in Model::LoadModel.";

            // Bounds are computed from vertex positions during ProcessMesh →
            // CalculateBounds. A degenerate box means either positions never
            // loaded or the bounds-accumulator regressed; either way pixels
            // would not be where the user expects them.
            const auto& box = model.GetBoundingBox();
            const glm::vec3 extent = box.GetSize();

            EXPECT_TRUE(std::isfinite(extent.x) && std::isfinite(extent.y) && std::isfinite(extent.z))
                << "Bounding box contains NaN/Inf for " << param.Name
                << " — vertex stream likely picked up an uninitialised attribute slot.";
            EXPECT_GT(extent.x + extent.y + extent.z, 0.0f)
                << "Bounding box is degenerate for " << param.Name
                << " — Model never received vertex positions, or CalculateBounds was skipped.";

            // For variants that ship textures (sidecar or embedded), at least
            // one of the materials Assimp produced must carry an albedo map.
            // The embedded-texture path in particular regressed in the past
            // because Model::LoadMaterialTextures treated Assimp's "*N" URI
            // (asterisk + texture index) as a filesystem path and silently
            // dropped the texture; pinning the contract here keeps that
            // class of regression from returning unnoticed.
            if (param.ExpectsAlbedoTexture)
            {
                bool sawAlbedoMap = false;
                for (sizet i = 0; i < model.GetMaterialCount(); ++i)
                {
                    auto material = model.GetMaterial(i);
                    if (material && material->GetAlbedoMap())
                    {
                        sawAlbedoMap = true;
                        break;
                    }
                }
                EXPECT_TRUE(sawAlbedoMap)
                    << "No material in " << param.Name << " carries an albedo map. "
                                                          "If this is the *Embedded variant, the loader is probably "
                                                          "feeding '*N' URIs to the filesystem texture loader instead "
                                                          "of resolving them via aiScene::GetEmbeddedTexture.";
            }
        }

        // The texture's ACTUAL contents off the GPU (level 0, RGBA8). Reading the GPU
        // rather than a file compares what the renderer will sample, whichever route
        // the material resolved through — cooked file, registry handle or in-memory
        // decode. A warm load that produced SOME texture but the wrong one would
        // satisfy a bare "has an albedo map" assertion; this does not.
        std::vector<u8> ReadTexturePixels(const Ref<Texture2D>& texture)
        {
            if (!texture || texture->GetWidth() == 0 || texture->GetHeight() == 0)
            {
                return {};
            }
            std::vector<u8> pixels(static_cast<sizet>(texture->GetWidth()) * texture->GetHeight() * 4u);
            ::glGetTextureImage(texture->GetRendererID(), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                                static_cast<GLsizei>(pixels.size()), pixels.data());
            return pixels;
        }

        // The first albedo map any material in the model carries, or null.
        Ref<Texture2D> FirstAlbedoMap(const Model& model)
        {
            for (sizet i = 0; i < model.GetMaterialCount(); ++i)
            {
                if (auto material = model.GetMaterial(i); material && material->GetAlbedoMap())
                {
                    return material->GetAlbedoMap();
                }
            }
            return nullptr;
        }

        // =====================================================================
        // The WARM half of the loader contract (issue #791).
        //
        // Everything above loads each variant exactly once, which only ever
        // exercises the COLD path: a fresh checkout has no
        // OloEditor/assets/cache/mesh/. That is precisely why CI could never
        // see this bug — a runner is cold by construction and always passes.
        // It bit a developer or an agent running the suite twice on one
        // machine, where the second run reads back the .omesh the first run
        // wrote, and it presented as a FLAKY test because the first run of the
        // day was green.
        //
        // What broke: an embedded texture (TexturedComplex and TexturedEmbedded
        // store their images in glTF bufferViews rather than as sidecar URIs)
        // has no file and no asset handle, so Model cooks it into a real .png
        // to give it an identity. That cook used to require an active project.
        // With no project — which is every load in this fixture — there was no
        // cook, so ImportedMaterialCodec recorded an EMPTY texture reference
        // into the .omesh and the warm load resolved a material with no albedo
        // map at all. No error, no warning: just fewer textures on run 2.
        //
        // So this case loads twice on purpose. Run 2 is a different experiment
        // than run 1, not a repeat of it.
        // =====================================================================
        TEST_P(DeccerCubesLoaderFixture, SurvivesAWarmMeshCacheRoundTrip)
        {
            OLO_ENSURE_GPU_OR_SKIP();

            const auto param = GetParam();
            const auto absolutePath = EditorRoot() / param.RelativePath;

            ASSERT_TRUE(std::filesystem::exists(absolutePath))
                << "Fixture file missing: " << absolutePath.string();

            // Start cold whatever the machine's cache state is, so the two halves below
            // are genuinely cold-then-warm rather than warm-then-warm. Both prefixes:
            // "static_uvflip_v1" is the default-flipped-UV namespace OBJ takes, and a
            // stale entry in the one we are not about to write would go unnoticed.
            MeshCache::InvalidateCache(absolutePath);
            MeshCache::InvalidateCache(absolutePath, "static_uvflip_v1");

            // -- Cold: straight from the source file via Assimp. Ground truth. --
            sizet coldMeshCount = 0;
            glm::vec3 coldExtent{ 0.0f };
            std::vector<u8> coldAlbedoPixels;
            {
                Model cold{ absolutePath.string() };
                coldMeshCount = cold.GetMeshCount();
                ASSERT_GT(coldMeshCount, 0u) << "the cold load produced no meshes for " << param.Name;
                coldExtent = cold.GetBoundingBox().GetSize();

                if (param.ExpectsAlbedoTexture)
                {
                    Ref<Texture2D> const albedo = FirstAlbedoMap(cold);
                    ASSERT_TRUE(albedo)
                        << "the COLD load of " << param.Name << " already has no albedo map, so the warm "
                                                                "comparison below would be vacuous — fix the importer before reading anything "
                                                                "into the cache half of this test";
                    coldAlbedoPixels = ReadTexturePixels(albedo);
                    ASSERT_FALSE(coldAlbedoPixels.empty());
                    ASSERT_TRUE(std::ranges::any_of(coldAlbedoPixels,
                                                    [&](u8 b)
                                                    { return b != coldAlbedoPixels[0]; }))
                        << "the cold albedo is a uniform block — comparing it to anything proves nothing";
                }
            }

            // A .gltf/.glb is not an OBJ, so it takes the un-prefixed cache namespace.
            ASSERT_TRUE(MeshCache::IsMeshCacheValid(absolutePath))
                << "the cold load wrote no .omesh for " << param.Name
                << ", so the warm half below would silently re-run the cold path and prove nothing";

            // The .omesh must carry the MATERIALS, not just the geometry.
            //
            // Without this the test has a hole big enough to pass on the bug it exists to
            // catch: Model::LoadModel drops the imported-material table when a texture cannot
            // be referenced, and a table-less cache sends the warm load down the Assimp
            // re-import branch — which rebuilds the materials from source and restores the
            // albedo. The pixel comparison below would then be comparing two *cold* imports
            // and would pass with the cache contributing nothing. Read the cached MeshSource
            // directly and require a non-empty table, so "warm" means what it says.
            if (param.ExpectsAlbedoTexture)
            {
                Ref<MeshSource> const cached = MeshCache::LoadMeshFromCache(absolutePath);
                ASSERT_TRUE(cached) << "the .omesh for " << param.Name << " reports valid but did not load";
                EXPECT_FALSE(cached->GetImportedMaterials().empty())
                    << "the .omesh for " << param.Name << " carries geometry but NO imported-material table, so the "
                                                          "warm load below will re-import materials from source and the albedo assertions would pass "
                                                          "without the cache ever round-tripping a texture. Model::LoadModel drops the table when "
                                                          "ImportedMaterialCodec cannot reference one of the material's textures.";
            }

            // -- Warm: geometry and materials come back out of the .omesh. --
            Model warm{ absolutePath.string() };

            EXPECT_EQ(warm.GetMeshCount(), coldMeshCount)
                << "the warm load produced a different number of meshes for " << param.Name;

            const glm::vec3 warmExtent = warm.GetBoundingBox().GetSize();
            EXPECT_TRUE(std::isfinite(warmExtent.x) && std::isfinite(warmExtent.y) && std::isfinite(warmExtent.z))
                << "the warm bounding box contains NaN/Inf for " << param.Name;
            EXPECT_NEAR(warmExtent.x, coldExtent.x, 1e-3f) << "warm bounds drifted for " << param.Name;
            EXPECT_NEAR(warmExtent.y, coldExtent.y, 1e-3f) << "warm bounds drifted for " << param.Name;
            EXPECT_NEAR(warmExtent.z, coldExtent.z, 1e-3f) << "warm bounds drifted for " << param.Name;

            if (!param.ExpectsAlbedoTexture)
            {
                return;
            }

            Ref<Texture2D> const warmAlbedo = FirstAlbedoMap(warm);
            ASSERT_TRUE(warmAlbedo)
                << "No material in " << param.Name << " carries an albedo map after a WARM cache load, "
                                                      "though the cold load a moment ago did. The .omesh is storing a texture reference "
                                                      "nothing can resolve — for an *Embedded/*Complex variant that means the embedded "
                                                      "bitmap was never cooked to a real file, so ImportedMaterialCodec had neither an "
                                                      "asset handle nor a path to write.";

            // Not merely SOME texture: the same pixels. A material holding a handle to the
            // wrong texture would pass the assertion above.
            const std::vector<u8> warmAlbedoPixels = ReadTexturePixels(warmAlbedo);
            EXPECT_EQ(warmAlbedoPixels, coldAlbedoPixels)
                << "the albedo survived the .omesh for " << param.Name << " but its CONTENT differs from "
                                                                          "the cold import — the cache round-trip must reproduce the exact pixels, orientation "
                                                                          "included, not merely produce a texture";
        }

        // Every shipped .gltf/.glb under OloEditor/assets/models/DeccerCubes/.
        // Each name is a distinct importer path:
        //   Plain               : geometry + sidecar .bin, no materials
        //   Colored             : .glb container, per-material baseColorFactor
        //   Textured            : .bin + 5 sidecar PNG diffuse maps
        //   TexturedComplex     : base64-embedded buffer, more cubes
        //   TexturedEmbedded    : base64-embedded textures inside the .gltf
        //   WithRotation        : exercises aiProcess_PreTransformVertices baking
        //                         non-identity node rotations into positions
        //   MergedAtlas         : single mesh + single atlas texture
        constexpr DeccerCubeVariant kVariants[] = {
            { "Plain", "assets/models/DeccerCubes/SM_Deccer_Cubes.gltf", false },
            { "Colored", "assets/models/DeccerCubes/SM_Deccer_Cubes_Colored.glb", false },
            { "Textured", "assets/models/DeccerCubes/SM_Deccer_Cubes_Textured.gltf", true },
            { "TexturedComplex", "assets/models/DeccerCubes/SM_Deccer_Cubes_Textured_Complex.gltf", true },
            { "TexturedEmbedded", "assets/models/DeccerCubes/SM_Deccer_Cubes_Textured_Embedded.gltf", true },
            { "WithRotation", "assets/models/DeccerCubes/SM_Deccer_Cubes_With_Rotation.gltf", false },
            { "MergedAtlas", "assets/models/DeccerCubes/SM_Deccer_Cubes_Merged_Texture_Atlas.gltf", true },
        };

        INSTANTIATE_TEST_SUITE_P(
            AllVariants,
            DeccerCubesLoaderFixture,
            ::testing::ValuesIn(kVariants),
            [](const ::testing::TestParamInfo<DeccerCubeVariant>& info)
            {
                return std::string{ info.param.Name };
            });
    } // namespace
} // namespace OloEngine::Tests

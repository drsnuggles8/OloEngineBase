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

#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Model.h"

#include <gtest/gtest.h>

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

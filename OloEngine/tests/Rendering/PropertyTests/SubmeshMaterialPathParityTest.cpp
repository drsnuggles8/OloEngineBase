// OLO_TEST_LAYER: integration
//
// THREE PATHS, ONE PICTURE (issue #629).
//
// The same multi-material mesh can reach the screen three ways — ModelComponent,
// MeshComponent, VirtualMeshComponent — and all three are supposed to render the same
// frame. None of them agreed:
//
//   * MeshComponent  resolved MaterialComponent -> engine default and NEVER consulted the
//                    submesh's imported material, so a multi-material glTF shaded every
//                    submesh with one flat grey material.
//   * ModelComponent resolved imported -> default and ignored an authored MaterialComponent.
//   * VirtualMeshComponent was the only path doing the full
//                    override -> imported -> default precedence.
//   * On top of that, Model::CreateCombinedMeshSource wrote Submesh::m_MaterialIndex = the
//                    submesh's own index, which only addresses the material array the WARM
//                    (.omesh cache) load rebuilds — never the DEDUPLICATED array the COLD
//                    import builds. So a first import and a second import of the same file
//                    rendered differently.
//
// Every one of those is invisible to a unit test of any single path, and all of them are
// caught by rendering the SAME MeshSource down all three paths and diffing the frames.
// The asset is the deccer-cubes "Colored" variant: a cube cluster whose materials are pure
// per-material baseColorFactors (red / green / blue / …) and no textures — so "did the
// per-submesh material survive?" is a question about the frame's COLOURS, which is exactly
// what the bug destroyed (everything went engine-default grey).
//
// SKIPs cleanly with no GL 4.6 context (suite gate).

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"
#include "RendererAttachedTest.h"

#include <gtest/gtest.h>

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/MeshCache.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/SubmeshMaterialResolve.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kWidth = 640;
        constexpr u32 kHeight = 360;

        // Parked far above the frame — the fixture builds all three renderables once and
        // moves the one under test to the origin, so every path is rendered by the exact
        // same scene, camera and lighting.
        constexpr glm::vec3 kParked{ 0.0f, 1000.0f, 0.0f };
        constexpr glm::vec3 kStage{ 0.0f, 0.0f, 0.0f };

        std::filesystem::path ColoredCubesPath()
        {
            return std::filesystem::path{ OLO_TEST_EDITOR_ROOT } / "assets/models/DeccerCubes/SM_Deccer_Cubes_Colored.glb";
        }

        // Per-hue pixel census. The cubes carry saturated per-material base colours; the
        // ground and the engine-default material are neutral grey. A frame that lost its
        // per-submesh materials has near-zero saturated pixels — which is the whole bug.
        struct ColourCensus
        {
            u32 Saturated = 0; // any clearly-non-grey pixel
            u32 RedDominant = 0;
            u32 GreenDominant = 0;
            u32 BlueDominant = 0;
        };

        ColourCensus Census(const std::vector<u8>& rgba)
        {
            ColourCensus census;
            for (sizet i = 0; i + 3 < rgba.size(); i += 4)
            {
                const i32 r = rgba[i];
                const i32 g = rgba[i + 1];
                const i32 b = rgba[i + 2];
                const i32 maxC = std::max({ r, g, b });
                const i32 minC = std::min({ r, g, b });
                if (maxC < 25 || (maxC - minC) < 40)
                {
                    continue; // background, ground, or an engine-default grey surface
                }

                ++census.Saturated;
                if (r == maxC)
                {
                    ++census.RedDominant;
                }
                else if (g == maxC)
                {
                    ++census.GreenDominant;
                }
                else
                {
                    ++census.BlueDominant;
                }
            }
            return census;
        }

        void WriteEvidencePng(const std::string& name, const std::vector<u8>& rgba, u32 width, u32 height)
        {
            std::vector<u8> flipped(rgba.size()); // GL readback is bottom-up
            const sizet rowBytes = static_cast<sizet>(width) * 4;
            for (u32 y = 0; y < height; ++y)
            {
                std::memcpy(flipped.data() + (static_cast<sizet>(y) * rowBytes),
                            rgba.data() + (static_cast<sizet>(height - 1 - y) * rowBytes), rowBytes);
            }

            std::filesystem::create_directories("assets/tests/visual");
            const std::string path = "assets/tests/visual/" + name;
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4,
                                               flipped.data(), static_cast<int>(rowBytes));
            EXPECT_NE(wrote, 0) << "failed to write evidence PNG " << path;
        }

        // Relative difference between two counts, guarded against a zero denominator.
        f32 RelativeDifference(u32 a, u32 b)
        {
            const auto larger = static_cast<f32>(std::max(a, b));
            if (larger <= 0.0f)
            {
                return 0.0f;
            }
            return std::abs(static_cast<f32>(a) - static_cast<f32>(b)) / larger;
        }
    } // namespace

    class SubmeshMaterialPathParity : public RendererAttachedTest
    {
      public:
        void BuildScene() override
        {
            // AssetManager::AddMemoryOnlyAsset needs an active project + asset manager
            // (same temp-project bootstrap as VirtualGeometryVisualEvidenceTest).
            if (!Project::GetActive() || !Project::GetAssetManager())
            {
                namespace fs = std::filesystem;
                std::error_code ec;
                const fs::path projectDir = fs::temp_directory_path() / "OloEngineSubmeshMaterialParity";
                fs::create_directories(projectDir / "Assets", ec);
                ASSERT_FALSE(ec) << "failed to create temp project dir";
                {
                    std::ofstream proj(projectDir / "Parity.oloproj");
                    proj << "Project:\n"
                            "  Name: SubmeshMaterialParity\n"
                            "  StartScene: \"\"\n"
                            "  AssetDirectory: \"Assets\"\n"
                            "  ScriptModulePath: \"\"\n";
                }
                ASSERT_TRUE(Project::Load(projectDir / "Parity.oloproj"));
                auto assetManager = Ref<EditorAssetManager>::Create();
                assetManager->Initialize(false); // no file watcher in tests
                Project::SetAssetManager(assetManager);
            }

            ASSERT_TRUE(std::filesystem::exists(ColoredCubesPath()))
                << "deccer-cubes fixture missing: " << ColoredCubesPath().string();

            EnableRendering(kWidth, kHeight);
            Scene& scene = GetScene();

            // One import, three consumers: the Model (ModelComponent), the combined
            // MeshSource it produces (MeshComponent), and that same MeshSource registered
            // as an asset (VirtualMeshComponent). Same geometry, same imported materials —
            // any difference in the frames is a difference in material RESOLUTION.
            m_Model = Ref<Model>::Create(ColoredCubesPath().string());
            ASSERT_GT(m_Model->GetMeshCount(), 0u) << "deccer-cubes Colored failed to import";
            ASSERT_GT(m_Model->GetMaterialCount(), 1u) << "fixture is supposed to be MULTI-material";

            m_Combined = m_Model->CreateCombinedMeshSource();
            ASSERT_TRUE(m_Combined);
            m_Combined->Build();
            ASSERT_GT(m_Combined->GetSubmeshes().Num(), 1) << "fixture is supposed to be MULTI-submesh";

            const AssetHandle handle = AssetManager::AddMemoryOnlyAsset(m_Combined);

            // Sun: near-vertical, so every cube face the camera sees is lit.
            {
                Entity sun = scene.CreateEntity("Sun");
                auto& dl = sun.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(0.2f, -0.95f, 0.25f));
                dl.m_Color = glm::vec3(1.0f, 0.98f, 0.95f);
                dl.m_Intensity = 3.0f;
            }

            // Ground so the subject isn't lit in a void (single-mesh-visual-test-lighting).
            {
                Entity ground = scene.CreateEntity("Ground");
                auto& tc = ground.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, -3.0f, 0.0f };
                tc.Scale = { 40.0f, 0.1f, 40.0f };
                auto& mc = ground.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Cube;
                if (Ref<Mesh> cube = MeshPrimitives::CreateCube(); cube)
                {
                    mc.m_MeshSource = cube->GetMeshSource();
                }
                auto& mat = ground.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.45f, 0.45f, 0.45f, 1.0f));
            }

            // Frame the cluster: scale it so its longest axis spans ~6 world units, which
            // (with the camera below) fills a large, countable part of the frame regardless
            // of what units the source file happens to use.
            const glm::vec3 extent = m_Model->GetBoundingBox().GetSize();
            const f32 longestAxis = std::max({ extent.x, extent.y, extent.z });
            ASSERT_GT(longestAxis, 0.0f) << "model has degenerate bounds";
            const f32 kModelScale = 6.0f / longestAxis;

            m_ModelEntity = scene.CreateEntity("ModelPath");
            {
                auto& tc = m_ModelEntity.GetComponent<TransformComponent>();
                tc.Translation = kParked;
                tc.Scale = glm::vec3(kModelScale);
                auto& mc = m_ModelEntity.AddComponent<ModelComponent>();
                mc.m_Model = m_Model;
                mc.m_Visible = true;
            }

            m_MeshEntity = scene.CreateEntity("MeshPath");
            {
                auto& tc = m_MeshEntity.GetComponent<TransformComponent>();
                tc.Translation = kParked;
                tc.Scale = glm::vec3(kModelScale);
                auto& mc = m_MeshEntity.AddComponent<MeshComponent>();
                mc.m_MeshSource = m_Combined;
            }

            m_VirtualEntity = scene.CreateEntity("VirtualPath");
            {
                auto& tc = m_VirtualEntity.GetComponent<TransformComponent>();
                tc.Translation = kParked;
                tc.Scale = glm::vec3(kModelScale);
                auto& vm = m_VirtualEntity.AddComponent<VirtualMeshComponent>();
                vm.m_MeshSource = handle;
                vm.m_ErrorThresholdPixels = 0.05f; // finest cut: LOD 0, the source triangles
            }
        }

        // Stages exactly one renderable at the origin and renders it.
        std::vector<u8> CapturePath(const char* name, Entity staged)
        {
            for (Entity e : { m_ModelEntity, m_MeshEntity, m_VirtualEntity })
            {
                e.GetComponent<TransformComponent>().Translation = (e == staged) ? kStage : kParked;
            }

            EditorCamera camera(45.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1f, 500.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(glm::vec3(0.0f, 2.0f, 9.0f), 0.0f, glm::radians(10.0f));
            RunEditorFrames(camera, 4);

            std::vector<u8> rgba;
            u32 w = 0;
            u32 h = 0;
            if (!ReadbackComposite(rgba, w, h))
            {
                ADD_FAILURE() << "composite readback unavailable for " << name;
                return {};
            }
            WriteEvidencePng(std::string("SubmeshMaterialParity_") + name + ".png", rgba, w, h);
            return rgba;
        }

        Ref<Model> m_Model;
        Ref<MeshSource> m_Combined;
        Entity m_ModelEntity;
        Entity m_MeshEntity;
        Entity m_VirtualEntity;
    };

    TEST_F(SubmeshMaterialPathParity, ClassicAndVirtualPathsShadeEverySubmeshWithItsImportedMaterial)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // VirtualGeometryPass is Deferred-only; the classic paths render there too, so
        // Deferred is the one path where all three are directly comparable.
        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        const ColourCensus model = Census(CapturePath("Model", m_ModelEntity));
        const ColourCensus mesh = Census(CapturePath("Mesh", m_MeshEntity));
        const ColourCensus virt = Census(CapturePath("Virtual", m_VirtualEntity));

        // 1. Every path must show the cubes' authored colours. Pre-fix, the MeshComponent
        //    frame was entirely engine-default grey: Saturated == 0.
        EXPECT_GT(model.Saturated, 500u) << "ModelComponent lost the imported materials";
        EXPECT_GT(mesh.Saturated, 500u)
            << "MeshComponent rendered no coloured pixels — it is shading every submesh with "
               "ONE material again (the imported material is being ignored).";
        EXPECT_GT(virt.Saturated, 500u) << "VirtualMeshComponent lost the imported materials";

        // 2. More than one distinct material must survive, on every path — a single-material
        //    flatten would still produce saturated pixels if that material happened to be red.
        for (const auto& [name, census] : std::array<std::pair<const char*, ColourCensus>, 3>{
                 { { "Model", model }, { "Mesh", mesh }, { "Virtual", virt } } })
        {
            const u32 hues = (census.RedDominant > 100 ? 1u : 0u) + (census.GreenDominant > 100 ? 1u : 0u) +
                             (census.BlueDominant > 100 ? 1u : 0u);
            EXPECT_GE(hues, 2u) << name << " shows fewer than two distinct material hues — the mesh's "
                                           "submeshes are being flattened onto a single material.";
        }

        // 3. The three frames must agree. Rasterization differs slightly (the virtual path is
        //    GPU-driven, the classic path goes through the depth prepass), so this is a
        //    tolerance on the per-hue coverage, not a pixel-exact diff — but a path that
        //    resolved the WRONG material would blow way past it.
        constexpr f32 kTolerance = 0.25f;
        EXPECT_LT(RelativeDifference(model.Saturated, mesh.Saturated), kTolerance)
            << "ModelComponent and MeshComponent disagree on how much of the frame is coloured";
        EXPECT_LT(RelativeDifference(model.Saturated, virt.Saturated), kTolerance)
            << "ModelComponent and VirtualMeshComponent disagree on how much of the frame is coloured";
        EXPECT_LT(RelativeDifference(model.RedDominant, mesh.RedDominant), kTolerance);
        EXPECT_LT(RelativeDifference(model.GreenDominant, mesh.GreenDominant), kTolerance);
        EXPECT_LT(RelativeDifference(model.BlueDominant, mesh.BlueDominant), kTolerance);
        EXPECT_LT(RelativeDifference(model.RedDominant, virt.RedDominant), kTolerance);
        EXPECT_LT(RelativeDifference(model.GreenDominant, virt.GreenDominant), kTolerance);
        EXPECT_LT(RelativeDifference(model.BlueDominant, virt.BlueDominant), kTolerance);
    }

    // -- C1: the combined MeshSource's material INDEX SPACE, cold vs warm --
    //
    // Model::CreateCombinedMeshSource used to write Submesh::m_MaterialIndex = the submesh's
    // OWN index, while the array that index is supposed to address is the DEDUPLICATED one the
    // cold path builds through m_MaterialIndexMap (one entry per unique aiMaterial) - and the
    // warm (.omesh) path rebuilt a different, one-entry-per-mesh array. Two index spaces, one
    // index. It now carries the submesh's real (deduplicated) material index on both paths,
    // and both paths build the same array.
    //
    // WHAT THIS TEST CANNOT DO, AND WHY (measured, not assumed):
    // Model::LoadModel imports with aiProcess_PreTransformVertices, which flattens the node
    // hierarchy and MERGES primitives that share a material. Every model therefore comes out of
    // Assimp with exactly one mesh per unique material - measured: deccer Colored 5 meshes / 5
    // materials; deccer Textured_Complex 19 glTF primitives -> 5 meshes / 5 materials;
    // With_Rotation 9 primitives -> 1 mesh / 1 material; Sponza 103 glTF primitives / 25
    // materials -> 25 meshes / 25 materials. So through THIS importer path "submesh index" and
    // "deduplicated material index" are always the same number, and NO fixture in the repo can
    // tell the two conventions apart. An earlier version of this test asserted
    // meshCount > materialCount to prove the fixture exercised deduplication; that assertion is
    // unsatisfiable by any asset here, which is itself the finding: the old convention was
    // coincidentally correct, so C1 is a LATENT bug, not an active one. It goes live the moment
    // a load yields two meshes sharing one material (importer flags change, PTV dropped for
    // instancing, a merge refused). The fix removes the coincidence; this test pins the
    // invariant that the two index spaces agree - cold, warm, and across the two consumers.
    //
    // Needs a GL context (Model::LoadModel builds GPU buffers), so it SKIPs without a GPU.
    TEST(ModelCombinedMaterialIndex, ColdImportAndWarmCacheLoadResolveTheSameMaterialPerSubmesh)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const std::filesystem::path path = ColoredCubesPath();
        ASSERT_TRUE(std::filesystem::exists(path)) << "deccer-cubes fixture missing: " << path.string();

        // What the ModelComponent path resolves per submesh: Model's own material array,
        // indexed by that submesh's material index.
        const auto modelMapping = [](const Model& model) -> std::vector<std::string>
        {
            const Material engineDefault{};
            std::vector<std::string> names;
            const auto& materials = model.GetMaterials();
            for (const Ref<Mesh>& submesh : model.GetMeshes())
            {
                if (!submesh)
                {
                    names.emplace_back("<null submesh>");
                    continue;
                }
                const u32 matIdx = submesh->GetSubmesh().m_MaterialIndex;
                const Material* imported = (matIdx < materials.size() && materials[matIdx]) ? materials[matIdx].get() : nullptr;
                names.push_back(imported != nullptr ? ResolveSubmeshMaterial(nullptr, imported, engineDefault).GetName()
                                                    : "<engine default>");
            }
            return names;
        };

        // What the MeshComponent / VirtualMeshComponent paths resolve: the COMBINED MeshSource
        // (the object the asset system hands them), indexed through ITS submesh table into ITS
        // imported-material array. If CreateCombinedMeshSource writes an index from one array
        // into a submesh that ships a different array, these two mappings diverge - that is
        // exactly the C1 defect, and it is invisible to any test that only looks at one of them.
        const auto combinedMapping = [](const MeshSource& combined) -> std::vector<std::string>
        {
            const Material engineDefault{};
            std::vector<std::string> names;
            for (i32 i = 0; i < combined.GetSubmeshes().Num(); ++i)
            {
                Ref<Material> const imported = combined.GetImportedMaterialForSubmesh(static_cast<u32>(i));
                names.push_back(imported ? ResolveSubmeshMaterial(nullptr, imported.get(), engineDefault).GetName()
                                         : "<engine default>");
            }
            return names;
        };

        // COLD: no cache. (Model::LoadModel writes the .omesh as a side effect of this load.)
        MeshCache::InvalidateCache(path);
        ASSERT_FALSE(MeshCache::IsMeshCacheValid(path)) << "cache invalidation did not take effect";

        Model cold{ path.string() };
        ASSERT_GT(cold.GetMeshCount(), 0u);
        ASSERT_GT(cold.GetMaterialCount(), 1u) << "fixture must be multi-material or this test proves nothing";

        Ref<MeshSource> const coldCombined = cold.CreateCombinedMeshSource();
        ASSERT_TRUE(coldCombined);
        const std::vector<std::string> coldModelNames = modelMapping(cold);
        const std::vector<std::string> coldCombinedNames = combinedMapping(*coldCombined);

        EXPECT_EQ(coldModelNames, coldCombinedNames)
            << "cold import: the combined MeshSource resolves different materials than the Model it came from - "
               "Submesh::m_MaterialIndex does not address the imported-material array shipped alongside it.";

        // WARM: the same file again, now served from the .omesh the cold load just wrote.
        ASSERT_TRUE(MeshCache::IsMeshCacheValid(path))
            << "the cold import did not write an .omesh - the warm half of this test would be vacuous";

        Model warm{ path.string() };
        ASSERT_GT(warm.GetMeshCount(), 0u);
        Ref<MeshSource> const warmCombined = warm.CreateCombinedMeshSource();
        ASSERT_TRUE(warmCombined);
        const std::vector<std::string> warmModelNames = modelMapping(warm);
        const std::vector<std::string> warmCombinedNames = combinedMapping(*warmCombined);

        EXPECT_EQ(warmModelNames, warmCombinedNames)
            << "warm cache load: the combined MeshSource and the Model disagree on the per-submesh material.";

        // And the two loads of the same file must agree with each other - the C1 symptom was
        // "the second load of a model renders differently from the first".
        ASSERT_EQ(coldModelNames.size(), warmModelNames.size()) << "cold and warm loads produced different submesh counts";
        EXPECT_EQ(coldModelNames, warmModelNames) << "the SAME model resolves different materials cold vs warm.";
        EXPECT_EQ(coldCombinedNames, warmCombinedNames)
            << "the SAME model's combined MeshSource resolves different materials cold vs warm.";

        // Non-vacuity: if every submesh resolved to the engine default (or all to one material),
        // every equality above would hold while proving nothing.
        const std::set<std::string> distinct(coldModelNames.begin(), coldModelNames.end());
        EXPECT_GT(distinct.size(), 1u)
            << "every submesh resolved to the same material - the multi-material fixture is being flattened";
        EXPECT_EQ(distinct.count("<engine default>"), 0u)
            << "at least one submesh fell through to the engine default: its m_MaterialIndex does not address "
               "the imported-material array.";
    }

    TEST_F(SubmeshMaterialPathParity, AMaterialComponentOverridesEverySubmeshOnEveryPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        // The other half of the precedence rule: an authored MaterialComponent must beat
        // every submesh's imported material. ModelComponent used to ignore it outright.
        for (Entity e : { m_ModelEntity, m_MeshEntity, m_VirtualEntity })
        {
            auto& mat = e.AddComponent<MaterialComponent>();
            mat.m_Material.SetBaseColorFactor(glm::vec4(0.05f, 0.05f, 0.9f, 1.0f)); // pure blue
            mat.m_Material.SetRoughnessFactor(0.7f);
        }

        const ColourCensus model = Census(CapturePath("ModelOverride", m_ModelEntity));
        const ColourCensus mesh = Census(CapturePath("MeshOverride", m_MeshEntity));
        const ColourCensus virt = Census(CapturePath("VirtualOverride", m_VirtualEntity));

        // Everything the mesh covers must now be blue: no red/green survivors from the
        // imported materials, on any path.
        for (const auto& [name, census] : std::array<std::pair<const char*, ColourCensus>, 3>{
                 { { "Model", model }, { "Mesh", mesh }, { "Virtual", virt } } })
        {
            EXPECT_GT(census.BlueDominant, 500u) << name << " did not apply the MaterialComponent override";
            EXPECT_LT(census.RedDominant + census.GreenDominant, census.BlueDominant / 10u)
                << name << " still shows the IMPORTED materials through a MaterialComponent override";
        }
    }
} // namespace OloEngine::Tests

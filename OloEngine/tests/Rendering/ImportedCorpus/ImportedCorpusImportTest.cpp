// OLO_TEST_LAYER: integration
// Imported glTF corpus: the production importer must retain every authored primitive,
// its material, and its index range on both the cold and cooked-cache paths.
#include "OloEnginePCH.h"

#include "ImportedCorpusFixtures.h"
#include "../PropertyTests/RenderPropertyTest.h"
#include "../PropertyTests/RendererAttachedTest.h"
#include "TestTempDir.h"
#include "../../TestOptions.h"

#include "OloEngine/Asset/Interchange/MeshImporterRegistry.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Asset/MeshCache.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetComponents.h"
#include "OloEngine/Animation/MorphTargets/MorphTargetSystem.h"
#include "OloEngine/Animation/SkeletalDeformation.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/DeferredForwardOverlayRoute.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/LOD.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/MeshOptimization.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Serialization/AssetPackFile.h"
#include "OloEngine/Serialization/FileStream.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <gtest/gtest.h>
#include <glad/gl.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace OloEngine::Tests::ImportedCorpus
{
    namespace
    {
        struct CorpusCase
        {
            const char* Path;
            i32 ExpectedSubmeshes;
            u32 MinimumMaterials;
            bool Rigged;
        };

        // Counts are pinned to authored primitive references (including repeated
        // node instances), not discovered from the importer. Losing one must fail.
        constexpr std::array kCases = {
            CorpusCase{ "AlphaBlendModeTest/AlphaBlendModeTest.gltf", 9, 6, false },
            CorpusCase{ "NegativeScaleTest/NegativeScaleTest.gltf", 11, 6, false },
            CorpusCase{ "NormalTangentMirrorTest/NormalTangentMirrorTest.gltf", 1, 1, false },
            CorpusCase{ "MorphPrimitivesTest/MorphPrimitivesTest.gltf", 2, 2, false },
            CorpusCase{ "AnimatedMorphCube/AnimatedMorphCube.gltf", 1, 1, false },
            CorpusCase{ "TransmissionTest/TransmissionTest.gltf", 3, 3, false },
            CorpusCase{ "RiggedSimple/RiggedSimple.gltf", 1, 1, true },
        };

        void CheckSource(const MeshSource& source, const CorpusCase& entry)
        {
            const auto& submeshes = source.GetSubmeshes();
            ASSERT_EQ(submeshes.Num(), entry.ExpectedSubmeshes) << entry.Path;
            ASSERT_GE(source.GetImportedMaterials().size(), entry.MinimumMaterials) << entry.Path;
            ASSERT_GT(source.GetVertices().Num(), 0) << entry.Path;
            ASSERT_GT(source.GetIndices().Num(), 0) << entry.Path;
            EXPECT_EQ(source.HasBoneInfluences(), entry.Rigged) << entry.Path;

            u32 indexCount = 0;
            for (i32 i = 0; i < submeshes.Num(); ++i)
            {
                const auto& submesh = submeshes[i];
                SCOPED_TRACE(entry.Path);
                SCOPED_TRACE(i);
                EXPECT_GT(submesh.m_VertexCount, 0u);
                EXPECT_GT(submesh.m_IndexCount, 0u);
                EXPECT_LE(static_cast<u64>(submesh.m_BaseVertex) + submesh.m_VertexCount,
                          static_cast<u64>(source.GetVertices().Num()));
                EXPECT_LE(static_cast<u64>(submesh.m_BaseIndex) + submesh.m_IndexCount,
                          static_cast<u64>(source.GetIndices().Num()));
                ASSERT_LT(submesh.m_MaterialIndex, source.GetImportedMaterials().size());
                EXPECT_TRUE(source.GetImportedMaterials()[submesh.m_MaterialIndex]);
                indexCount += submesh.m_IndexCount;
            }
            EXPECT_EQ(indexCount, static_cast<u32>(source.GetIndices().Num())) << entry.Path;

            const std::string_view name(entry.Path);
            if (name.starts_with("AlphaBlendModeTest/"))
            {
                u32 masked = 0;
                u32 blended = 0;
                for (const auto& material : source.GetImportedMaterials())
                {
                    if (material && material->GetAlphaMode() == AlphaMode::Mask)
                        ++masked;
                    if (material && material->GetAlphaMode() == AlphaMode::Blend)
                        ++blended;
                }
                EXPECT_GE(masked, 3u);
                EXPECT_GE(blended, 1u);
            }
            if (name.starts_with("TransmissionTest/"))
            {
                u32 transmissive = 0;
                for (const auto& material : source.GetImportedMaterials())
                    transmissive += material && material->GetTransmissionFactor() > 0.0f ? 1u : 0u;
                EXPECT_EQ(transmissive, 2u);
            }
            if (name.starts_with("AlphaBlendModeTest/") || name.starts_with("TransmissionTest/"))
            {
                for (const auto& material : source.GetImportedMaterials())
                {
                    ASSERT_TRUE(material);
                    const bool overlay = material->GetFlag(MaterialFlag::Blend) || material->IsTransmissive();
                    const auto route = SelectDeferredForwardOverlayRoute({
                        .Deferred = true,
                        .Blended = material->GetFlag(MaterialFlag::Blend),
                        .Transmissive = material->IsTransmissive(),
                        .HasForwardOverlayPass = true,
                        .HasForwardShader = true,
                    });
                    EXPECT_EQ(route, overlay ? DeferredForwardOverlayRoute::ForwardOverlay
                                             : DeferredForwardOverlayRoute::None);
                }
            }
        }
    } // namespace

    TEST(ImportedCorpus, RealAssetFilesAndLicensesHaveExactCase)
    {
        ASSERT_TRUE(std::filesystem::is_regular_file(CorpusManifestPath()));
        for (const auto& entry : kCases)
        {
            const auto path = ModelPath(entry.Path);
            SCOPED_TRACE(entry.Path);
            ASSERT_TRUE(std::filesystem::is_regular_file(path));
            const auto actual = path.parent_path() / path.filename();
            bool exactName = false;
            for (const auto& child : std::filesystem::directory_iterator(path.parent_path()))
                exactName |= child.path().filename() == actual.filename();
            EXPECT_TRUE(exactName) << "case differs from the committed file on this host";
            if (path.parent_path().filename() != "TransmissionTest" &&
                path.parent_path().filename() != "RiggedSimple")
                EXPECT_TRUE(std::filesystem::is_regular_file(path.parent_path() / "LICENSE.md"));
        }
    }

    TEST(ImportedCorpus, RealAssetsSurviveColdAndCookedImport)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        for (const auto& entry : kCases)
        {
            SCOPED_TRACE(entry.Path);
            const auto path = ModelPath(entry.Path);
            ASSERT_TRUE(std::filesystem::is_regular_file(path));
            MakeColdImport(path);

            auto cold = MeshImporterRegistry::Get().Import(path);
            ASSERT_TRUE(cold.Succeeded()) << cold.Error;
            ASSERT_TRUE(cold.Source);
            CheckSource(*cold.Source, entry);

            const char* prefix = entry.Rigged ? AnimatedModel::kCachePrefix : "";
            ASSERT_TRUE(MeshCache::IsMeshCacheValid(path, prefix));
            auto warm = MeshImporterRegistry::Get().Import(path);
            ASSERT_TRUE(warm.Succeeded()) << warm.Error;
            ASSERT_TRUE(warm.Source);
            CheckSource(*warm.Source, entry);
            const auto coldMaterials = cold.Source->GetImportedMaterials();
            const auto warmMaterials = warm.Source->GetImportedMaterials();
            ASSERT_EQ(coldMaterials.size(), warmMaterials.size());
            for (sizet i = 0; i < coldMaterials.size(); ++i)
            {
                SCOPED_TRACE(i);
                ASSERT_TRUE(coldMaterials[i]);
                ASSERT_TRUE(warmMaterials[i]);
                EXPECT_EQ(coldMaterials[i]->GetName(), warmMaterials[i]->GetName());
                EXPECT_EQ(coldMaterials[i]->GetAlphaMode(), warmMaterials[i]->GetAlphaMode());
                EXPECT_NEAR(coldMaterials[i]->GetTransmissionFactor(),
                            warmMaterials[i]->GetTransmissionFactor(), 1e-6f);
                EXPECT_EQ(static_cast<bool>(coldMaterials[i]->GetAlbedoMap()),
                          static_cast<bool>(warmMaterials[i]->GetAlbedoMap()));
                EXPECT_EQ(static_cast<bool>(coldMaterials[i]->GetNormalMap()),
                          static_cast<bool>(warmMaterials[i]->GetNormalMap()));
            }
        }
    }

    TEST(ImportedCorpus, MinimalAdversarialFixturesUseProductionImporter)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const auto directory = TempDir("imported_corpus_minimal");
        const auto rigidPath = AuthorRigidAttachmentFixture(directory);
        const auto invalidPath = AuthorInvalidInfluenceFixture(directory);
        const auto seamPath = AuthorSeamFixture(directory);
        for (const auto& [path, count] :
             std::array{ std::pair{ rigidPath, 2 }, std::pair{ invalidPath, 1 }, std::pair{ seamPath, 2 } })
        {
            auto result = MeshImporterRegistry::Get().Import(path);
            ASSERT_TRUE(result.Succeeded()) << path.string() << ": " << result.Error;
            ASSERT_TRUE(result.Source);
            EXPECT_EQ(result.Source->GetSubmeshes().Num(), count) << path.string();
            EXPECT_EQ(result.Source->HasBoneInfluences(), path != seamPath) << path.string();
            if (path == rigidPath)
            {
                const auto& vertices = result.Source->GetVertices();
                const auto& influences = result.Source->GetBoneInfluences();
                ASSERT_EQ(vertices.Num(), influences.Num());
                ASSERT_GT(result.Source->GetBoneInfo().Num(), 0);
                std::vector<glm::mat4> movedPalette(static_cast<sizet>(result.Source->GetBoneInfo().Num()),
                                                    glm::mat4(1.0f));
                for (auto& matrix : movedPalette)
                    matrix[3].x = 2.0f;
                u32 unweightedFlapVertices = 0;
                for (i32 vertex = 0; vertex < vertices.Num(); ++vertex)
                {
                    const auto position = vertices[vertex].Position;
                    if (position.x >= kFlapMinX - 0.001f && position.x <= kFlapMaxX + 0.001f &&
                        position.y >= -0.001f && position.y <= kFlapMaxY + 0.001f)
                    {
                        ++unweightedFlapVertices;
                        EXPECT_LT(glm::distance(SkinPosition(*result.Source, static_cast<u32>(vertex), movedPalette),
                                                position),
                                  0.001f);
                    }
                }
                EXPECT_EQ(unweightedFlapVertices, 4u);
            }
            if (path == seamPath)
            {
                const auto& vertices = result.Source->GetVertices();
                ASSERT_GE(vertices.Num(), 6);
                ASSERT_EQ(result.Source->GetImportedMaterials().size(), 2u);
                ASSERT_EQ(result.Source->GetSubmeshes()[0].m_MaterialIndex, 0u);
                ASSERT_EQ(result.Source->GetSubmeshes()[1].m_MaterialIndex, 1u);
                bool foundSeam = false;
                for (i32 a = 0; a < vertices.Num(); ++a)
                {
                    for (i32 b = a + 1; b < vertices.Num(); ++b)
                    {
                        if (glm::distance(vertices[a].Position, vertices[b].Position) < 1e-5f &&
                            glm::distance(vertices[a].TexCoord, vertices[b].TexCoord) > 0.1f &&
                            glm::distance(vertices[a].Normal, vertices[b].Normal) > 0.1f)
                            foundSeam = true;
                    }
                }
                EXPECT_TRUE(foundSeam) << "the importer welded away a UV and normal seam";
            }
            if (path == invalidPath)
            {
                const auto& influences = result.Source->GetBoneInfluences();
                ASSERT_EQ(influences.Num(), result.Source->GetVertices().Num());
                std::array<u32, kInfluenceCaseCount> verticesPerCase{};
                for (i32 vertex = 0; vertex < influences.Num(); ++vertex)
                {
                    SCOPED_TRACE(vertex);
                    const f32 x = result.Source->GetVertices()[vertex].Position.x;
                    const auto caseIndex = static_cast<u32>(std::floor(x / 0.5f + 0.0001f));
                    ASSERT_LT(caseIndex, kInfluenceCaseCount);
                    ++verticesPerCase[caseIndex];
                    f32 total = 0.0f;
                    for (f32 weight : influences[vertex].m_Weights)
                    {
                        EXPECT_TRUE(std::isfinite(weight));
                        EXPECT_GE(weight, 0.0f);
                        total += weight;
                    }
                    EXPECT_TRUE(total < 0.001f || std::abs(total - 1.0f) < 0.001f);
                    if (caseIndex == static_cast<u32>(InfluenceCase::ZeroSum) ||
                        caseIndex == static_cast<u32>(InfluenceCase::AllNaN))
                        EXPECT_LT(total, 0.001f) << ToString(static_cast<InfluenceCase>(caseIndex));
                    else
                        EXPECT_NEAR(total, 1.0f, 0.001f) << ToString(static_cast<InfluenceCase>(caseIndex));
                }
                for (u32 i = 0; i < kInfluenceCaseCount; ++i)
                    EXPECT_EQ(verticesPerCase[i], 4u) << ToString(static_cast<InfluenceCase>(i));
            }
        }
    }

    TEST(ImportedCorpus, MorphOnlyAssetsReachTheAnimatedImportPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        for (const auto& [file, primitiveCount] :
             std::array{ std::pair{ "MorphPrimitivesTest/MorphPrimitivesTest.gltf", 2 },
                         std::pair{ "AnimatedMorphCube/AnimatedMorphCube.gltf", 1 } })
        {
            SCOPED_TRACE(file);
            AnimatedModel model(ModelPath(file).string());
            // No bones, no skeleton (issue #1439). The importer used to invent a
            // one-bone skeleton here, which made every morph-only face a SKINNED
            // entity and left no way to build a morph-only one.
            EXPECT_FALSE(model.HasSkeleton());
            auto source = model.CreateCombinedMeshSource();
            ASSERT_TRUE(source);
            EXPECT_FALSE(source->HasSkeleton());
            ASSERT_EQ(source->GetSubmeshes().Num(), primitiveCount);
            ASSERT_TRUE(source->HasMorphTargets());
            EXPECT_GT(source->GetMorphTargets()->GetTargetCount(), 0u);

            std::vector<glm::vec3> restPositions;
            std::vector<glm::vec3> restNormals;
            for (const auto& vertex : source->GetVertices())
            {
                restPositions.push_back(vertex.Position);
                restNormals.push_back(vertex.Normal);
            }
            MorphTargetComponent morph;
            morph.MorphTargets = source->GetMorphTargets();
            morph.SetWeight(morph.MorphTargets->Targets.front().Name, 1.0f);
            std::vector<glm::vec3> posedPositions, posedNormals;
            ASSERT_TRUE(MorphTargetSystem::EvaluateMorphTargets(
                morph, restPositions, restNormals, posedPositions, posedNormals));
            ASSERT_EQ(posedPositions.size(), restPositions.size());
            u32 movedVertices = 0;
            for (sizet i = 0; i < restPositions.size(); ++i)
                movedVertices += glm::distance(posedPositions[i], restPositions[i]) > 0.0001f ? 1u : 0u;
            EXPECT_GT(movedVertices, 0u) << "the imported morph target produced no deformation";

            ASSERT_TRUE(morph.SetWeight(morph.MorphTargets->Targets.front().Name, 0.0f));
            EXPECT_FALSE(morph.HasActiveWeights());
            EXPECT_FALSE(MorphTargetSystem::EvaluateMorphTargets(
                morph, restPositions, restNormals, posedPositions, posedNormals));
        }
    }

    class ImportedCorpusHistoryTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            AnimatedModel model(ModelPath("RiggedSimple/RiggedSimple.gltf").string());
            m_Source = model.CreateCombinedMeshSource();
            ASSERT_TRUE(m_Source);
            ASSERT_TRUE(model.GetSkeleton());
            ASSERT_FALSE(model.GetAnimations().empty());
            m_Source->Build();

            m_Actor = GetScene().CreateEntity("Imported RiggedSimple");
            m_Actor.AddComponent<MeshComponent>().m_MeshSource = m_Source;
            m_Actor.AddComponent<SkeletonComponent>(model.GetSkeleton());
            auto& state = m_Actor.AddComponent<AnimationStateComponent>();
            state.m_CurrentClip = model.GetAnimations().front();
            state.m_IsPlaying = true;
        }

        [[nodiscard]] f32 BoneMotion() const
        {
            const auto& skeleton = *m_Actor.GetComponent<SkeletonComponent>().m_Skeleton;
            if (!skeleton.HasBoneHistory())
                return 0.0f;
            f32 maximum = 0.0f;
            for (sizet bone = 0; bone < skeleton.m_FinalBoneMatrices.size(); ++bone)
                for (int column = 0; column < 4; ++column)
                    for (int row = 0; row < 4; ++row)
                        maximum = std::max(maximum,
                                           std::abs(skeleton.m_FinalBoneMatrices[bone][column][row] -
                                                    skeleton.m_PrevFinalBoneMatrices[bone][column][row]));
            return maximum;
        }

        Entity m_Actor;
        Ref<MeshSource> m_Source;
    };

    TEST_F(ImportedCorpusHistoryTest, ImportedClipPausesResumesAndRejectsTeleportHistory)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        RunFrames(15);
        const f32 moving = BoneMotion();
        ASSERT_GT(moving, 1e-5f) << "the imported animation never moved a bone";

        GetScene().SetPaused(true);
        RunFrames(3);
        EXPECT_NEAR(BoneMotion(), 0.0f, 1e-6f);

        GetScene().SetPaused(false);
        RunFrames(1);
        EXPECT_GT(BoneMotion(), 1e-5f);
        EXPECT_LT(BoneMotion(), moving * 3.0f);

        m_Actor.GetComponent<TransformComponent>().Translation.x += 100.0f;
        const u32 resetCount = Animation::SkeletalDeformationSystem::ResetHistory(
            &GetScene(), Animation::DeformationHistoryResetCause::Teleport);
        EXPECT_EQ(resetCount, 1u);
        EXPECT_FALSE(m_Actor.GetComponent<SkeletonComponent>().m_Skeleton->HasBoneHistory());
        EXPECT_NEAR(BoneMotion(), 0.0f, 1e-6f);
        EXPECT_EQ(Animation::SkeletalDeformationSystem::GetStats().LastResetCause,
                  Animation::DeformationHistoryResetCause::Teleport);
    }

    TEST_F(ImportedCorpusHistoryTest, ImportedSkinAndMorphChangesReachRenderedColour)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        Time::SetMockTime(3.0f);
        struct RestoreTime
        {
            ~RestoreTime()
            {
                Time::ClearMockTime();
            }
        } restoreTime;
        constexpr u32 width = 512, height = 512;
        EnableRendering(width, height);
        auto light = GetScene().CreateEntity("Imported skin sun");
        auto& directional = light.AddComponent<DirectionalLightComponent>();
        directional.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.7f, -0.6f));
        directional.m_Intensity = 3.0f;

        AnimatedModel morphModel(ModelPath("AnimatedMorphCube/AnimatedMorphCube.gltf").string());
        auto morphSource = morphModel.CreateCombinedMeshSource();
        ASSERT_TRUE(morphSource);
        ASSERT_TRUE(morphSource->HasMorphTargets());
        morphSource->Build();
        auto morphActor = GetScene().CreateEntity("Imported morph actor");
        auto& morphTransform = morphActor.GetComponent<TransformComponent>();
        morphTransform.Translation.x = 3.0f;
        // AnimatedModel retains the cube's 0.02-unit mesh coordinates; the
        // glTF node's 100x presentation scale is not baked into this source.
        morphTransform.Scale = { 100.0f, 100.0f, 100.0f };
        morphActor.AddComponent<MeshComponent>().m_MeshSource = morphSource;
        morphActor.AddComponent<MaterialComponent>().m_Material.SetBaseColorFactor(
            glm::vec4(0.85f, 0.15f, 0.10f, 1.0f));
        auto& morph = morphActor.AddComponent<MorphTargetComponent>();
        morph.MorphTargets = morphSource->GetMorphTargets();

        EditorCamera camera(60.0f, 1.0f, 0.05f, 100.0f);
        camera.SetViewportSize(static_cast<f32>(width), static_cast<f32>(height));
        camera.Focus(glm::vec3(1.5f, 0.0f, 0.0f), 14.0f, glm::radians(-35.0f), glm::radians(10.0f));

        auto capture = [&](const char* state)
        {
            RunEditorFrames(camera, 2);
            EXPECT_GT(Renderer3D::GetStats().TotalMeshes, 0u);
            const u32 depth = Renderer3D::ResolveFrameGraphTexture(ResourceNames::SceneDepth);
            EXPECT_NE(depth, 0u);
            if (depth != 0u)
            {
                std::vector<f32> values(static_cast<sizet>(width) * height, 1.0f);
                glGetTextureImage(depth, 0, GL_DEPTH_COMPONENT, GL_FLOAT,
                                  static_cast<GLsizei>(values.size() * sizeof(f32)), values.data());
                u32 leftCoverage = 0, rightCoverage = 0;
                for (u32 y = 0; y < height; ++y)
                    for (u32 x = 0; x < width; ++x)
                        if (values[static_cast<sizet>(y) * width + x] < 0.999f)
                            (x < width / 2 ? leftCoverage : rightCoverage)++;
                EXPECT_GT(leftCoverage, 100u) << "the skinned actor has no depth representation";
                EXPECT_GT(rightCoverage, 100u) << "the morph actor has no depth representation";
            }
            std::vector<u8> pixels;
            u32 actualWidth = 0, actualHeight = 0;
            EXPECT_TRUE(ReadbackComposite(pixels, actualWidth, actualHeight));
            EXPECT_EQ(actualWidth, width);
            EXPECT_EQ(actualHeight, height);
            if (pixels.size() != static_cast<sizet>(width) * height * 4u)
                return pixels;

            u32 visibleMorphPixels = 0;
            for (u32 y = 0; y < height; ++y)
                for (u32 x = width / 2; x < width; ++x)
                {
                    const sizet i = (static_cast<sizet>(y) * width + x) * 4u;
                    visibleMorphPixels += pixels[i] > 60 && pixels[i] > pixels[i + 1] + 25 &&
                                                  pixels[i] > pixels[i + 2] + 25
                                              ? 1u
                                              : 0u;
                }
            EXPECT_GT(visibleMorphPixels, 100u) << "the imported morph actor vanished in " << state;

            const auto rowBytes = static_cast<sizet>(width) * 4u;
            std::vector<u8> row(rowBytes);
            for (u32 y = 0; y < height / 2; ++y)
            {
                u8* top = pixels.data() + static_cast<sizet>(y) * rowBytes;
                u8* bottom = pixels.data() + static_cast<sizet>(height - 1 - y) * rowBytes;
                std::memcpy(row.data(), top, rowBytes);
                std::memcpy(top, bottom, rowBytes);
                std::memcpy(bottom, row.data(), rowBytes);
            }
            const auto output = EditorRoot() / "assets/tests/visual" /
                                (std::string("ImportedCorpus_GL_Skinned_") + state + ".png");
            // The subject-region depth and motion oracles below are stable
            // across suite order; the editor background is not an RMSE oracle.
            if (Options().GoldenRebase)
                EXPECT_NE(stbi_write_png(output.string().c_str(), width, height, 4, pixels.data(), width * 4), 0);
            else
                EXPECT_TRUE(std::filesystem::is_regular_file(output)) << output.string();
            return pixels;
        };

        auto rest = capture("Rest");
        ASSERT_GT(morph.MorphTargets->GetTargetCount(), 0u);
        ASSERT_TRUE(morph.SetWeight(morph.MorphTargets->Targets.front().Name, 1.0f));
        auto morphed = capture("Morph");
        ASSERT_EQ(rest.size(), morphed.size());
        u32 morphChanged = 0;
        for (u32 y = 0; y < height; ++y)
            for (u32 x = width / 2; x < width; ++x)
            {
                const sizet i = (static_cast<sizet>(y) * width + x) * 4u;
                morphChanged += std::abs(static_cast<int>(rest[i]) - static_cast<int>(morphed[i])) > 8 ? 1u : 0u;
            }
        EXPECT_GT(morphChanged, 500u) << "the imported morph did not change the visible cube";

        RunFrames(15);
        ASSERT_GT(BoneMotion(), 1e-5f);
        auto posed = capture("Skin");
        ASSERT_EQ(morphed.size(), posed.size());
        u32 changed = 0;
        for (u32 y = 0; y < height; ++y)
            for (u32 x = 0; x < width / 2; ++x)
            {
                const sizet i = (static_cast<sizet>(y) * width + x) * 4u;
                changed += std::abs(static_cast<int>(morphed[i]) - static_cast<int>(posed[i])) > 8 ? 1u : 0u;
            }
        EXPECT_GT(changed, 500u) << "the imported animation did not change the visible skinned actor";
    }

    class ImportedCorpusPackTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_Directory = TempDir("imported_corpus_pack");
            std::filesystem::create_directories(m_Directory / "Assets");
            const auto projectFile = m_Directory / "Corpus.oloproj";
            std::ofstream project(projectFile);
            project << "Project:\n  Name: CorpusPack\n  StartScene: \"\"\n"
                       "  AssetDirectory: \"Assets\"\n  ScriptModulePath: \"\"\n";
            project.close();
            ASSERT_TRUE(Project::Load(projectFile));
            m_Manager = Ref<EditorAssetManager>::Create();
            m_Manager->Initialize(false);
            Project::SetAssetManager(m_Manager);
        }

        void TearDown() override
        {
            Project::Unload();
            m_Manager.Reset();
            std::error_code error;
            std::filesystem::remove_all(m_Directory, error);
        }

        std::filesystem::path m_Directory;
        Ref<EditorAssetManager> m_Manager;
    };

    // A morph-only face authored in a scene reloads as a morph-only ENTITY
    // (issue #1439): mesh + morph weights + the animation state that names its
    // source file, and no skeleton, so the rigid draw path owns it.
    //
    // The scene carries a SkeletonComponent key on purpose. Scenes saved while the
    // importer still invented a one-bone skeleton for a bone-less model have one,
    // and re-adding it EMPTY would hide the head from both draw loops. The rigged
    // control beside it must keep its real skeleton, so the rule cannot pass by
    // dropping every SkeletonComponent key.
    TEST_F(ImportedCorpusPackTest, MorphOnlyFaceReloadsAsAMorphOnlyEntity)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const std::string morphPath = ModelPath("MorphPrimitivesTest/MorphPrimitivesTest.gltf").generic_string();
        const std::string riggedPath = ModelPath("RiggedSimple/RiggedSimple.gltf").generic_string();
        // One entity per model; {MORPH} and {RIGGED} are replaced below.
        std::string yaml = R"(Scene: MorphOnly1439
Entities:
  - Entity: 1439000000000001
    TagComponent:
      Tag: Face
    TransformComponent:
      Translation: [0, 0, 0]
      Rotation: [0, 0, 0]
      Scale: [1, 1, 1]
    AnimationStateComponent:
      SourceFilePath: "{MORPH}"
    SkeletonComponent:
      Legacy: true
    MorphTargetComponent:
      Weights:
        MorphTarget_0: 0.75
  - Entity: 1439000000000002
    TagComponent:
      Tag: Rigged
    TransformComponent:
      Translation: [0, 0, 0]
      Rotation: [0, 0, 0]
      Scale: [1, 1, 1]
    AnimationStateComponent:
      SourceFilePath: "{RIGGED}"
    SkeletonComponent:
      Legacy: true
)";
        yaml.replace(yaml.find("{MORPH}"), 7, morphPath);
        yaml.replace(yaml.find("{RIGGED}"), 8, riggedPath);

        auto scene = Ref<Scene>::Create();
        SceneSerializer serializer(scene);
        ASSERT_TRUE(serializer.DeserializeFromYAML(yaml));

        Entity face = scene->FindEntityByName("Face");
        ASSERT_TRUE(static_cast<bool>(face));
        ASSERT_TRUE(face.HasComponent<MeshComponent>());
        const auto& mesh = face.GetComponent<MeshComponent>().m_MeshSource;
        ASSERT_TRUE(mesh) << "the morph-only source did not load, so nothing below is about a face";
        EXPECT_TRUE(mesh->HasMorphTargets());
        EXPECT_FALSE(face.HasComponent<SkeletonComponent>())
            << "a bone-less model came back with a SkeletonComponent, so the rigid draw loop skips it";
        ASSERT_TRUE(face.HasComponent<MorphTargetComponent>());
        const auto& weights = face.GetComponent<MorphTargetComponent>().Weights;
        ASSERT_TRUE(weights.contains("MorphTarget_0")) << "the saved weight did not reach the morph component";
        EXPECT_FLOAT_EQ(weights.at("MorphTarget_0"), 0.75f);
        ASSERT_TRUE(face.HasComponent<AnimationStateComponent>());
        EXPECT_FALSE(face.GetComponent<AnimationStateComponent>().m_SourceFilePath.empty());

        Entity rigged = scene->FindEntityByName("Rigged");
        ASSERT_TRUE(static_cast<bool>(rigged));
        ASSERT_TRUE(rigged.HasComponent<SkeletonComponent>()) << "the rigged control lost its skeleton";
        EXPECT_TRUE(static_cast<bool>(rigged.GetComponent<SkeletonComponent>().m_Skeleton));
    }

    TEST_F(ImportedCorpusPackTest, ImportedTransmissionSubmeshesAndMaterialsSurviveCookedPack)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const auto path = ModelPath("TransmissionTest/TransmissionTest.gltf");
        auto imported = MeshImporterRegistry::Get().Import(path);
        ASSERT_TRUE(imported.Succeeded()) << imported.Error;
        ASSERT_TRUE(imported.Source);
        ASSERT_EQ(imported.Source->GetSubmeshes().Num(), 3);
        imported.Source->Build();
        const auto handle = AssetManager::AddMemoryOnlyAsset(imported.Source);

        const auto packPath = m_Directory / "corpus.pack";
        MeshSourceSerializer serializer;
        AssetSerializationInfo info{};
        {
            FileStreamWriter writer(packPath);
            ASSERT_TRUE(writer.IsStreamGood());
            ASSERT_TRUE(serializer.SerializeToAssetPack(handle, writer, info));
        }
        AssetPackFile::AssetInfo assetInfo{};
        assetInfo.Handle = static_cast<AssetHandle>(0x1350ULL);
        assetInfo.PackedOffset = info.Offset;
        assetInfo.PackedSize = info.Size;
        assetInfo.Type = AssetType::MeshSource;
        FileStreamReader reader(packPath);
        ASSERT_TRUE(reader.IsStreamGood());
        reader.SetArchiveVersion(AssetPackFile::Version);
        auto loaded = serializer.DeserializeFromAssetPack(reader, assetInfo).As<MeshSource>();
        ASSERT_TRUE(loaded);
        EXPECT_EQ(loaded->GetSubmeshes().Num(), 3);
        ASSERT_GE(loaded->GetImportedMaterials().size(), 3u);
        u32 transmissive = 0;
        for (const auto& material : loaded->GetImportedMaterials())
            transmissive += material && material->GetTransmissionFactor() > 0.0f ? 1u : 0u;
        EXPECT_EQ(transmissive, 2u);
        EXPECT_EQ(loaded->GetIndices().Num(), imported.Source->GetIndices().Num());
    }

    TEST_F(ImportedCorpusPackTest, ImportedNormalMapMaterialSurvivesALODChange)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        auto imported = MeshImporterRegistry::Get().Import(
            ModelPath("NormalTangentMirrorTest/NormalTangentMirrorTest.gltf"));
        ASSERT_TRUE(imported.Succeeded()) << imported.Error;
        ASSERT_TRUE(imported.Source);
        ASSERT_EQ(imported.Source->GetSubmeshes().Num(), 1);
        const auto originalMaterial = imported.Source->GetImportedMaterialForSubmesh(0);
        ASSERT_TRUE(originalMaterial);
        ASSERT_TRUE(originalMaterial->GetNormalMap());

        auto lower = MeshOptimization::GenerateLODMeshWithAttributes(*imported.Source, 0.5f, 0.1f);
        ASSERT_TRUE(lower);
        ASSERT_EQ(lower->GetSubmeshes().Num(), 1);
        ASSERT_LT(lower->GetIndices().Num(), imported.Source->GetIndices().Num());
        const auto lowerMaterial = lower->GetImportedMaterialForSubmesh(0);
        ASSERT_TRUE(lowerMaterial) << "LOD dropped the imported material table";
        EXPECT_EQ(lowerMaterial->GetName(), originalMaterial->GetName());
        EXPECT_TRUE(lowerMaterial->GetNormalMap());

        imported.Source->Build();
        lower->Build();
        const auto highHandle = AssetManager::AddMemoryOnlyAsset(imported.Source);
        const auto lowHandle = AssetManager::AddMemoryOnlyAsset(lower);
        LODGroup group;
        group.Levels.Emplace(highHandle, 5.0f, static_cast<u32>(imported.Source->GetIndices().Num() / 3));
        group.Levels.Emplace(lowHandle, 100.0f, static_cast<u32>(lower->GetIndices().Num() / 3));
        ASSERT_EQ(group.SelectLOD(2.0f), 0);
        ASSERT_EQ(group.SelectLOD(20.0f), 1);
        auto selectedHigh = AssetManager::GetAsset<MeshSource>(group.Levels[0].MeshHandle);
        auto selectedLow = AssetManager::GetAsset<MeshSource>(group.Levels[1].MeshHandle);
        ASSERT_TRUE(selectedHigh);
        ASSERT_TRUE(selectedLow);
        EXPECT_GT(selectedHigh->GetIndices().Num(), selectedLow->GetIndices().Num());
        EXPECT_EQ(selectedHigh->GetImportedMaterialForSubmesh(0)->GetName(),
                  selectedLow->GetImportedMaterialForSubmesh(0)->GetName());

        const auto autoGroup = MeshOptimization::GenerateAutoLODGroup(*imported.Source, highHandle);
        ASSERT_GT(autoGroup.Levels.Num(), 1) << "the imported normal-map mesh produced no automatic LOD";
        for (i32 level = 1; level < autoGroup.Levels.Num(); ++level)
        {
            const auto autoMesh = AssetManager::GetAsset<Mesh>(autoGroup.Levels[level].MeshHandle);
            ASSERT_TRUE(autoMesh) << level;
            const auto autoSource = autoMesh->GetMeshSource();
            ASSERT_TRUE(autoSource) << level;
            const auto autoMaterial = autoSource->GetImportedMaterialForSubmesh(0);
            ASSERT_TRUE(autoMaterial) << "automatic LOD " << level << " dropped the imported material table";
            EXPECT_EQ(autoMaterial->GetName(), originalMaterial->GetName()) << level;
            EXPECT_TRUE(autoMaterial->GetNormalMap()) << level;
        }
    }

    class ImportedCorpusVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        static constexpr u32 kWidth = 640;
        static constexpr u32 kHeight = 360;

        void BuildScene() override
        {
            EnableRendering(kWidth, kHeight);
            const auto path = ModelPath("NegativeScaleTest/NegativeScaleTest.gltf");
            auto result = MeshImporterRegistry::Get().Import(path);
            ASSERT_TRUE(result.Succeeded()) << result.Error;
            ASSERT_TRUE(result.Source);
            ASSERT_EQ(result.Source->GetSubmeshes().Num(), 11);
            result.Source->Build();

            for (f32 x : { -2.5f, 2.5f })
            {
                auto entity = GetScene().CreateEntity("Imported mirrored actor");
                auto& transform = entity.GetComponent<TransformComponent>();
                transform.Translation = { x, 0.0f, 0.0f };
                transform.Scale = { 1.0f, 0.8f, 1.2f };
                entity.AddComponent<MeshComponent>().m_MeshSource = result.Source;
            }

            auto light = GetScene().CreateEntity("Corpus sun");
            auto& directional = light.AddComponent<DirectionalLightComponent>();
            directional.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.7f, -0.6f));
            directional.m_Intensity = 2.0f;
        }

        void Capture(RenderingPath path, const char* pathName, const char* angleName, const glm::vec3& eye,
                     f32 yaw)
        {
            struct RestorePath
            {
                RenderingPath Previous = Renderer3D::GetRendererSettings().Path;
                ~RestorePath()
                {
                    Renderer3D::GetRendererSettings().Path = Previous;
                    Renderer3D::ApplyRendererSettings();
                }
            } restorePath;
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::ApplyRendererSettings();

            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / kHeight, 0.05f, 100.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(eye, yaw, 0.0f);
            RunEditorFrames(camera, 2);
            Renderer3D::ResetStats();
            RunEditorFrames(camera, 1);

            // The full editor tick records 67 mesh submissions for this fixture.
            // Pin the count so one lost imported node reference cannot hide
            // inside a passing colour capture.
            EXPECT_EQ(Renderer3D::GetStats().TotalMeshes, 67u)
                << "Two imported actors must each submit all eleven node mesh instances";
            auto framebuffer = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::UIComposite);
            if (!framebuffer)
                framebuffer = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::ToneMapColor);
            ASSERT_TRUE(framebuffer);
            std::vector<u8> pixels;
            ReadbackRgba8(framebuffer->GetColorAttachmentRendererID(0), kWidth, kHeight, pixels);
            ASSERT_EQ(pixels.size(), static_cast<sizet>(kWidth) * kHeight * 4u);

            if (std::string_view(angleName) == "Front")
            {
                auto coveredDepthPixels = [](u32 texture) -> sizet
                {
                    if (texture == 0u)
                        return 0;
                    GLint width = 0, height = 0, layers = 0;
                    glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_WIDTH, &width);
                    glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_HEIGHT, &height);
                    glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_DEPTH, &layers);
                    if (width <= 0 || height <= 0)
                        return 0;
                    std::vector<f32> values(static_cast<sizet>(width) * static_cast<sizet>(height) *
                                                static_cast<sizet>(std::max(layers, 1)),
                                            1.0f);
                    glGetTextureImage(texture, 0, GL_DEPTH_COMPONENT, GL_FLOAT,
                                      static_cast<GLsizei>(values.size() * sizeof(f32)), values.data());
                    return static_cast<sizet>(std::count_if(values.begin(), values.end(),
                                                            [](f32 depthValue)
                                                            { return depthValue < 0.999f; }));
                };
                const u32 depth = Renderer3D::ResolveFrameGraphTexture(ResourceNames::SceneDepth);
                const u32 shadow = Renderer3D::ResolveFrameGraphTexture(ResourceNames::ShadowMapCSMCascade0);
                EXPECT_NE(depth, 0u) << "the imported actors have no scene-depth representation";
                EXPECT_NE(shadow, 0u) << "the imported actors have no shadow representation";
                EXPECT_GT(coveredDepthPixels(depth), 100u) << "the imported actors left scene depth empty";
                EXPECT_GT(coveredDepthPixels(shadow), 100u) << "the imported actors left cascade 0 empty";
                if (path == RenderingPath::Deferred)
                {
                    const u32 velocity = Renderer3D::ResolveFrameGraphTexture(ResourceNames::Velocity);
                    EXPECT_NE(velocity, 0u) << "the deferred imported actors have no velocity representation";
                    if (velocity != 0u)
                    {
                        std::vector<f32> velocityPixels;
                        ReadbackRgbaFloat(velocity, kWidth, kHeight, velocityPixels);
                        ASSERT_EQ(velocityPixels.size(), pixels.size());
                        u32 nonFinite = 0;
                        for (f32 value : velocityPixels)
                            nonFinite += std::isfinite(value) ? 0u : 1u;
                        EXPECT_EQ(nonFinite, 0u);
                    }
                }
            }

            const auto rowBytes = static_cast<sizet>(kWidth) * 4u;
            std::vector<u8> row(rowBytes);
            for (u32 y = 0; y < kHeight / 2; ++y)
            {
                u8* top = pixels.data() + static_cast<sizet>(y) * rowBytes;
                u8* bottom = pixels.data() + static_cast<sizet>(kHeight - 1 - y) * rowBytes;
                std::memcpy(row.data(), top, rowBytes);
                std::memcpy(top, bottom, rowBytes);
                std::memcpy(bottom, row.data(), rowBytes);
            }

            const auto output = EditorRoot() / "assets/tests/visual" /
                                (std::string("ImportedCorpus_GL_") + pathName + "_" + angleName + ".png");
            if (Options().GoldenRebase)
            {
                std::filesystem::create_directories(output.parent_path());
                ASSERT_NE(stbi_write_png(output.string().c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight),
                                         4, pixels.data(),
                                         static_cast<int>(rowBytes)),
                          0);
                return;
            }

            int width = 0, height = 0, channels = 0;
            stbi_uc* golden = stbi_load(output.string().c_str(), &width, &height, &channels, 4);
            ASSERT_NE(golden, nullptr) << "Missing visual reference: " << output.string();
            const bool dimensionsMatch = width == static_cast<int>(kWidth) && height == static_cast<int>(kHeight);
            if (!dimensionsMatch)
            {
                stbi_image_free(golden);
                FAIL() << "Visual reference has wrong dimensions: " << output.string();
            }
            f64 squareError = 0.0;
            for (sizet i = 0; i < pixels.size(); ++i)
            {
                const f64 delta = static_cast<f64>(pixels[i]) - golden[i];
                squareError += delta * delta;
            }
            stbi_image_free(golden);
            EXPECT_LT(std::sqrt(squareError / pixels.size()), 6.0) << output.string();
        }
    };

    TEST_F(ImportedCorpusVisualEvidenceTest, TwoImportedActorsSubmitAllSubmeshesOnEveryGLPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        Time::SetMockTime(3.0f);
        struct RestoreTime
        {
            ~RestoreTime()
            {
                Time::ClearMockTime();
            }
        } restoreTime;
        for (const auto& [path, name] :
             std::array{ std::pair{ RenderingPath::Forward, "Forward" },
                         std::pair{ RenderingPath::ForwardPlus, "ForwardPlus" },
                         std::pair{ RenderingPath::Deferred, "Deferred" } })
        {
            Capture(path, name, "Front", { 0.0f, 0.0f, 10.0f }, 0.0f);
            Capture(path, name, "Oblique", { 4.0f, 0.0f, 10.0f }, -0.38f);
            Capture(path, name, "Near", { -2.5f, 0.0f, 1.2f }, 0.0f);
        }
    }
} // namespace OloEngine::Tests::ImportedCorpus

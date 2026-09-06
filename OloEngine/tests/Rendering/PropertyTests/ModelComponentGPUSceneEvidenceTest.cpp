// OLO_TEST_LAYER: integration
// =============================================================================
// ModelComponentGPUSceneEvidenceTest.cpp
//
// The regression guard for issue #1065: "a ModelComponent's geometry never
// reaches the canonical GPU Scene, so ray tracing cannot see it."
//
// Why the counters, not just the pixels
// -------------------------------------
// A model-backed entity rendered perfectly on screen the whole time the bug
// existed — the raster path draws it directly. What was missing was invisible
// by construction: the submeshes were never STAGED as canonical instances, so
// the acceleration structure held two BLAS for a 25-mesh Sponza and the RT
// telemetry reported nothing rejected, because nothing had arrived to reject.
// A picture cannot distinguish that from a healthy frame, so this test asserts
// the renderer's own extraction counters first and captures the pixels second.
//
// The three assertions that would have failed before the fix:
//   1. every mesh of the ModelComponent has a live instance record, found by
//      the same (entity, vertex buffer, index buffer, submesh) key extraction
//      builds;
//   2. each of those records carries the ENTITY's world transform. Getting
//      this wrong is the plausible-looking failure — the counters go up while
//      the traced geometry sits somewhere the raster frame does not;
//   3. nothing is counted as "renderable geometry that produced no canonical
//      instance": neither LegacyModel (a path that never offered its geometry)
//      nor NotExtractable (offered and rejected).
//
// The pixels are still captured, from three angles, because the model path
// also feeds the shadow caster list and the DDGI capture out of the same loop
// this change restructured — and those only show up on screen.
//
// Classification: L8 / integration (full GL pipeline + RGBA8 readback + PNG).
// SKIPs cleanly without a GL 4.6 context, so CI runs the counters' CPU
// siblings (McpRayTracingStatsTest, GPUScenePropertyTests) and this one runs
// only on a workstation with a GPU.
// =============================================================================

#include "OloEnginePCH.h"
#include "../../TestOptions.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneDrawLink.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image.h>
#include <stb_image/stb_image_write.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 960;
        constexpr u32 kHeight = 540;

        // Whole-pipeline capture, not a tight pixel test: it exists to catch a
        // model that stopped drawing or moved, not a shading epsilon.
        constexpr f64 kGoldenRmseThreshold = 6.0;

        // The model under test. Shipped with the sample project and already
        // used by MeshVisibilityEvidenceTest, so it is a known-good multi-mesh
        // glTF rather than a fixture this test has to maintain.
        constexpr const char* kModelPath = "SandboxProject/Assets/Models/KenneyVehicles/ship-small-hull.glb";

        constexpr glm::vec3 kModelPosition{ 0.0f, 0.0f, 0.0f };
        constexpr f32 kModelScale = 1.5f;

        [[nodiscard]] f64 Rgba8Rmse(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
            {
                return std::numeric_limits<f64>::max();
            }
            f64 sumSquares = 0.0;
            std::size_t count = 0;
            for (std::size_t i = 0; i + 3 < a.size(); i += 4)
            {
                for (int channel = 0; channel < 3; ++channel)
                {
                    const f64 delta = static_cast<f64>(a[i + channel]) - static_cast<f64>(b[i + channel]);
                    sumSquares += delta * delta;
                    ++count;
                }
            }
            return count ? std::sqrt(sumSquares / static_cast<f64>(count)) : 0.0;
        }

        [[nodiscard]] ::testing::AssertionResult MatricesNearlyEqual(const glm::mat4& actual,
                                                                     const glm::mat4& expected)
        {
            constexpr f32 kEpsilon = 1e-4f;
            for (int column = 0; column < 4; ++column)
            {
                for (int row = 0; row < 4; ++row)
                {
                    if (std::abs(actual[column][row] - expected[column][row]) > kEpsilon)
                    {
                        return ::testing::AssertionFailure()
                               << "element [" << column << "][" << row << "] is " << actual[column][row]
                               << ", expected " << expected[column][row];
                    }
                }
            }
            return ::testing::AssertionSuccess();
        }

        [[nodiscard]] f32 MeanLuminance(const std::vector<u8>& rgba, u32 width, u32 height, u32 cx, u32 cy,
                                        u32 halfExtent)
        {
            const u32 x0 = cx > halfExtent ? cx - halfExtent : 0u;
            const u32 y0 = cy > halfExtent ? cy - halfExtent : 0u;
            const u32 x1 = std::min(cx + halfExtent, width);
            const u32 y1 = std::min(cy + halfExtent, height);
            f64 sum = 0.0;
            u64 count = 0;
            for (u32 y = y0; y < y1; ++y)
            {
                for (u32 x = x0; x < x1; ++x)
                {
                    const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4u;
                    sum += 0.2126 * rgba[i] + 0.7152 * rgba[i + 1] + 0.0722 * rgba[i + 2];
                    ++count;
                }
            }
            return count ? static_cast<f32>(sum / (count * 255.0)) : 0.0f;
        }
    } // namespace

    // One ModelComponent, one MeshComponent ground plane, one shadow-casting
    // sun. The ground is the control: it goes through the already-migrated
    // classic mesh path, so a failure that hits BOTH is a GPU Scene problem
    // rather than a model-path one.
    class ModelComponentGPUSceneScene : public RendererAttachedTest
    {
      protected:
        Ref<Model> m_Model;
        Entity m_ModelEntity;

        void BuildScene() override
        {
            Scene& scene = GetScene();

            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.85f, -0.35f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 3.0f;
                // Shadows ON: the model path feeds the shadow caster list out
                // of the same per-mesh loop this change restructured, and a
                // caster loop that stopped running is exactly the kind of
                // regression only a rendered frame shows.
                dl.m_CastShadows = true;
            }

            {
                Entity ground = scene.CreateEntity("Ground");
                auto& transform = ground.GetComponent<TransformComponent>();
                transform.Translation = { 0.0f, -1.0f, 0.0f };
                transform.Scale = { 24.0f, 1.0f, 24.0f };
                auto& mesh = ground.AddComponent<MeshComponent>();
                mesh.m_Primitive = MeshPrimitive::Plane;
                if (Ref<Mesh> plane = MeshPrimitives::CreatePlane(); plane)
                {
                    mesh.m_MeshSource = plane->GetMeshSource();
                }
                auto& material = ground.AddComponent<MaterialComponent>();
                material.m_Material.SetBaseColorFactor({ 0.45f, 0.45f, 0.48f, 1.0f });
                material.m_Material.SetRoughnessFactor(0.9f);
            }

            {
                m_Model = Ref<Model>::Create(std::string(kModelPath));
                // A load failure must not read as "the model loaded and staged
                // nothing" — that is the very hypothesis this test exists to
                // separate, so it fails loudly here instead.
                ASSERT_TRUE(m_Model && m_Model->GetMeshCount() > 0)
                    << "could not load '" << kModelPath << "' — the test binary runs from the repo root";

                Entity entity = scene.CreateEntity("Ship");
                auto& transform = entity.GetComponent<TransformComponent>();
                transform.Translation = kModelPosition;
                transform.Scale = glm::vec3(kModelScale);
                entity.AddComponent<ModelComponent>(m_Model, std::string(kModelPath));
                m_ModelEntity = entity;
            }

            EnableRendering(kWidth, kHeight);
        }

        // The instance key extraction builds for one mesh of the model, from
        // the same three identities: the entity's stable id, the mesh source's
        // GPU buffers, and the submesh index.
        [[nodiscard]] GPUSceneInstanceKey KeyForMesh(sizet meshIndex) const
        {
            const Ref<Mesh>& mesh = m_Model->GetMeshes()[meshIndex];
            const Ref<MeshSource>& source = mesh->GetMeshSource();
            const Ref<VertexBuffer>& vertexBuffer = source->GetVertexBuffer();
            const Ref<IndexBuffer>& indexBuffer = source->GetIndexBuffer();
            return GPUSceneInstanceKey{
                .m_EntityId = static_cast<u64>(m_ModelEntity.GetUUID()),
                .m_Geometry = GPUSceneGeometryKey{ .m_VertexBuffer = RHI::HashKey(vertexBuffer->GetRHIHandle()),
                                                   .m_IndexBuffer = RHI::HashKey(indexBuffer->GetRHIHandle()),
                                                   .m_SubmeshIndex = mesh->GetSubmeshIndex() },
                .m_InstanceId = 0,
            };
        }

        void Capture(const std::string& poseName, const glm::vec3& position, f32 yaw, f32 pitch,
                     std::vector<u8>& outPixels)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 500.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, yaw, pitch);

            // Two ticks: the first fills the GPU Scene with fresh records, the
            // second is the steady-state frame.
            RunEditorFrames(camera, 2);

            u32 width = 0;
            u32 height = 0;
            ASSERT_TRUE(ReadbackComposite(outPixels, width, height))
                << "no composite framebuffer for '" << poseName << "'";
            ASSERT_EQ(width, kWidth);
            ASSERT_EQ(height, kHeight);

            const fs::path directory = fs::path("assets") / "tests" / "visual";
            const std::string path = (directory / ("ModelComponentGPUScene_" + poseName + ".png")).string();
            if (Options().GoldenRebase)
            {
                std::error_code ec;
                fs::create_directories(directory, ec);
                ASSERT_FALSE(ec) << "failed to create '" << directory.string() << "': " << ec.message();
                const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight),
                                                   4, outPixels.data(), static_cast<int>(kWidth) * 4);
                ASSERT_NE(wrote, 0) << "failed to write golden '" << path << "'";
                return;
            }

            int goldenWidth = 0;
            int goldenHeight = 0;
            int goldenChannels = 0;
            stbi_uc* golden = ::stbi_load(path.c_str(), &goldenWidth, &goldenHeight, &goldenChannels, 4);
            ASSERT_NE(golden, nullptr)
                << "missing golden '" << path << "' — rerun with --olo-golden-rebase to create it";
            const bool sizeMatches =
                goldenWidth == static_cast<int>(kWidth) && goldenHeight == static_cast<int>(kHeight);
            std::vector<u8> goldenPixels;
            if (sizeMatches)
            {
                goldenPixels.assign(golden, golden + static_cast<std::size_t>(kWidth) * kHeight * 4u);
            }
            ::stbi_image_free(golden);
            ASSERT_TRUE(sizeMatches) << "golden '" << path << "' is " << goldenWidth << "x" << goldenHeight;

            const f64 rmse = Rgba8Rmse(outPixels, goldenPixels);
            EXPECT_LE(rmse, kGoldenRmseThreshold)
                << "pose '" << poseName << "' diverged from its golden (RMSE " << rmse
                << "). If this is an intended visual change, rerun with --olo-golden-rebase to update " << path;
        }
    };

    TEST_F(ModelComponentGPUSceneScene, EveryModelSubmeshReachesTheCanonicalSceneAtTheEntityTransform)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        struct Pose
        {
            const char* Name;
            glm::vec3 Position;
            f32 Yaw;
            f32 Pitch;
        };

        // Three angles. A transform that is transposed, or shifted by the
        // render origin twice, survives one of these and not three.
        const std::array<Pose, 3> poses = { {
            { "Front", { 0.0f, 3.5f, 16.0f }, 0.0f, 0.16f },
            { "Quarter", { -12.0f, 4.5f, 11.0f }, 0.83f, 0.22f },
            { "Above", { 0.0f, 13.0f, 9.0f }, 0.0f, 0.87f },
        } };

        for (const Pose& pose : poses)
        {
            std::vector<u8> pixels;
            Capture(pose.Name, pose.Position, pose.Yaw, pose.Pitch, pixels);
            if (::testing::Test::HasFatalFailure())
            {
                return;
            }

            const f32 frame = MeanLuminance(pixels, kWidth, kHeight, kWidth / 2u, kHeight / 2u, kHeight / 3u);
            EXPECT_GT(frame, 0.02f) << "pose '" << pose.Name << "' rendered (near-)black";

            // --- The assertions the pictures cannot make -------------------
            const GPUSceneFrameStats& stats = Renderer3D::GetGPUSceneStats();
            const sizet meshCount = m_Model->GetMeshCount();

            // The ground plane is one more instance on top of the model's.
            EXPECT_GE(stats.m_Instances.m_Live, static_cast<u32>(meshCount) + 1u)
                << "pose '" << pose.Name << "': the canonical scene holds " << stats.m_Instances.m_Live
                << " instances for a scene of " << meshCount << " model submeshes plus a ground plane";

            // Nothing may be counted as renderable-but-unstaged. Before the fix
            // this read as `meshCount` under LegacyModel.
            EXPECT_EQ(stats.m_UnsupportedCounts[static_cast<sizet>(GPUSceneUnsupportedCategory::LegacyModel)], 0u)
                << "pose '" << pose.Name << "': the model path still reports its geometry as unrepresentable";
            EXPECT_EQ(stats.m_UnsupportedCounts[static_cast<sizet>(GPUSceneUnsupportedCategory::NotExtractable)], 0u)
                << "pose '" << pose.Name << "': a model submesh was offered to the canonical scene and rejected";

            // Each submesh by identity, and at the right place. The transform
            // check is the one that separates "the counters went up" from "the
            // traced geometry is where the raster frame drew it".
            const GPUScene& gpuScene = Renderer3D::GetGPUScene();
            // The entity's OWN matrix, not a re-composition of the same numbers:
            // the record is encoded from this. Compared with a tolerance rather
            // than bitwise, because every failure this guards against — a
            // transpose, a render origin applied twice, a per-submesh node
            // transform that should not be there — is orders of magnitude
            // larger than 1e-4.
            const glm::mat4 expected = MakeModelRelative(
                m_ModelEntity.GetComponent<TransformComponent>().GetTransform(), Renderer3D::GetRenderOrigin());
            for (sizet i = 0; i < meshCount; ++i)
            {
                const GPUSceneInstanceKey key = KeyForMesh(i);
                const GPUSceneInstance* record = gpuScene.GetInstanceRecord(gpuScene.FindInstance(key));
                ASSERT_NE(record, nullptr)
                    << "pose '" << pose.Name << "': model mesh " << i << " has no canonical instance record";
                EXPECT_TRUE(MatricesNearlyEqual(DecodeGPUSceneTransform(record->CurrentTransform), expected))
                    << "pose '" << pose.Name << "': model mesh " << i
                    << " is staged at a transform that is not the entity's. The model's node transforms are baked "
                       "into its vertices (aiProcess_PreTransformVertices) and Model::DrawParallel draws every mesh "
                       "with the entity transform alone, so anything else here puts the traced geometry where the "
                       "raster frame did not draw it.";
            }
        }
    }
} // namespace OloEngine::Tests

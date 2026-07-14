// OLO_TEST_LAYER: integration
//
// Visual evidence for the virtualized-geometry hardware raster path
// (issue #629, slice 2): a VirtualMeshComponent icosphere rendered through
// the FULL Deferred pipeline — Scene loop -> Renderer3D::SubmitVirtualMesh ->
// VirtualGeometryPass (DAG-cut cull compute + glMultiDrawElementsIndirectCount
// into the borrowed G-Buffer) -> DeferredLightingPass -> composite.
//
// Writes multi-angle, multi-threshold PNGs to assets/tests/visual/ and then
// asserts driver-independent contracts on the readback:
//   1. The sphere is visible (a red-dominant pixel region exists) at the
//      fine threshold, from every camera angle. Catches "pass silently draws
//      nothing" — broken cull, broken MDI plumbing, broken vertex pulling.
//   2. A very coarse error threshold still renders the sphere (the coarsest
//      DAG cut is drawn, never a hole) — the terminal-group contract on the
//      real GPU path.
// The PNGs are written before any assertion so the frames are always
// available for review. SKIPs cleanly with no GL 4.6 context (suite gate).

#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"
#include "RendererAttachedTest.h"

#define GLFW_INCLUDE_NONE
#include <glad/gl.h>

#include <gtest/gtest.h>

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Core/Base.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshRegistry.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kWidth = 640;
        constexpr u32 kHeight = 360;

        // Icosphere with shared vertices (closed manifold) — same generator as
        // VirtualMeshBuilderTest, kept local per the self-contained-test rule.
        Ref<MeshSource> MakeIcosphereMeshSource(u32 subdivisions)
        {
            const f32 t = (1.0f + std::sqrt(5.0f)) / 2.0f;
            std::vector<glm::vec3> positions = {
                { -1.0f, t, 0.0f },
                { 1.0f, t, 0.0f },
                { -1.0f, -t, 0.0f },
                { 1.0f, -t, 0.0f },
                { 0.0f, -1.0f, t },
                { 0.0f, 1.0f, t },
                { 0.0f, -1.0f, -t },
                { 0.0f, 1.0f, -t },
                { t, 0.0f, -1.0f },
                { t, 0.0f, 1.0f },
                { -t, 0.0f, -1.0f },
                { -t, 0.0f, 1.0f },
            };
            for (auto& p : positions)
            {
                p = glm::normalize(p);
            }
            std::vector<u32> indices = {
                0,
                11,
                5,
                0,
                5,
                1,
                0,
                1,
                7,
                0,
                7,
                10,
                0,
                10,
                11,
                1,
                5,
                9,
                5,
                11,
                4,
                11,
                10,
                2,
                10,
                7,
                6,
                7,
                1,
                8,
                3,
                9,
                4,
                3,
                4,
                2,
                3,
                2,
                6,
                3,
                6,
                8,
                3,
                8,
                9,
                4,
                9,
                5,
                2,
                4,
                11,
                6,
                2,
                10,
                8,
                6,
                7,
                9,
                8,
                1,
            };
            for (u32 s = 0; s < subdivisions; ++s)
            {
                std::map<std::pair<u32, u32>, u32> midpointCache;
                auto midpoint = [&](u32 a, u32 b) -> u32
                {
                    std::pair<u32, u32> const key = std::minmax(a, b);
                    if (auto it = midpointCache.find(key); it != midpointCache.end())
                    {
                        return it->second;
                    }
                    auto index = static_cast<u32>(positions.size());
                    positions.push_back(glm::normalize((positions[a] + positions[b]) * 0.5f));
                    midpointCache.emplace(key, index);
                    return index;
                };
                std::vector<u32> next;
                next.reserve(indices.size() * 4);
                for (sizet i = 0; i + 2 < indices.size(); i += 3)
                {
                    u32 const a = indices[i];
                    u32 const b = indices[i + 1];
                    u32 const c = indices[i + 2];
                    u32 const ab = midpoint(a, b);
                    u32 const bc = midpoint(b, c);
                    u32 const ca = midpoint(c, a);
                    for (u32 idx : { a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca })
                    {
                        next.push_back(idx);
                    }
                }
                indices = std::move(next);
            }

            TArray<Vertex> vertices;
            vertices.Reserve(static_cast<i32>(positions.size()));
            constexpr f32 kPi = 3.14159265358979323846f;
            for (const glm::vec3& p : positions)
            {
                glm::vec2 const uv{ std::atan2(p.z, p.x) / (2.0f * kPi) + 0.5f,
                                    std::asin(std::clamp(p.y, -1.0f, 1.0f)) / kPi + 0.5f };
                vertices.Add(Vertex(p, p, uv));
            }
            TArray<u32> meshIndices;
            meshIndices.Reserve(static_cast<i32>(indices.size()));
            for (u32 const index : indices)
            {
                meshIndices.Add(index);
            }
            return Ref<MeshSource>::Create(MoveTemp(vertices), MoveTemp(meshIndices));
        }

        void WriteEvidencePng(const std::string& name, std::vector<u8> rgba, u32 width, u32 height)
        {
            // GL readback is bottom-up; flip for PNG
            std::vector<u8> flipped(rgba.size());
            sizet const rowBytes = static_cast<sizet>(width) * 4;
            for (u32 y = 0; y < height; ++y)
            {
                std::memcpy(flipped.data() + static_cast<sizet>(y) * rowBytes,
                            rgba.data() + static_cast<sizet>(height - 1 - y) * rowBytes, rowBytes);
            }

            namespace fs = std::filesystem;
            fs::create_directories("assets/tests/visual");
            std::string const path = "assets/tests/visual/" + name;
            int const wrote = ::stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height),
                                               4, flipped.data(), static_cast<int>(rowBytes));
            EXPECT_NE(wrote, 0) << "failed to write evidence PNG " << path;
        }

        // Count pixels that read as the sphere's red material under directional
        // lighting: red channel clearly dominant over blue (the sky/background
        // in this scene is dark blue-grey, the ground is neutral grey).
        u32 CountRedDominantPixels(const std::vector<u8>& rgba)
        {
            u32 count = 0;
            for (sizet i = 0; i + 3 < rgba.size(); i += 4)
            {
                u32 const r = rgba[i];
                u32 const g = rgba[i + 1];
                u32 const b = rgba[i + 2];
                if (r > 60 && r > g + 25 && r > b + 25)
                {
                    ++count;
                }
            }
            return count;
        }
    } // namespace

    class VirtualGeometryVisualEvidence : public RendererAttachedTest
    {
      public:
        void BuildScene() override
        {
            // AssetManager::AddMemoryOnlyAsset / GetAsset need an active project
            // + asset manager. Mount a throwaway temp project, mirroring
            // FunctionalTest::EnableAssetManager (incl. its "leave the manager
            // installed at teardown" rationale — SetAssetManager asserts non-null).
            if (!Project::GetActive() || !Project::GetAssetManager())
            {
                namespace fs = std::filesystem;
                std::error_code ec;
                fs::path const projectDir = fs::temp_directory_path() / "OloEngineVirtualGeometryEvidence";
                fs::create_directories(projectDir / "Assets", ec);
                ASSERT_FALSE(ec) << "failed to create temp project dir";
                {
                    std::ofstream proj(projectDir / "Evidence.oloproj");
                    proj << "Project:\n"
                            "  Name: VirtualGeometryEvidence\n"
                            "  StartScene: \"\"\n"
                            "  AssetDirectory: \"Assets\"\n"
                            "  ScriptModulePath: \"\"\n";
                }
                ASSERT_TRUE(Project::Load(projectDir / "Evidence.oloproj"));
                auto assetManager = Ref<EditorAssetManager>::Create();
                assetManager->Initialize(false); // no file watcher in tests
                Project::SetAssetManager(assetManager);
            }

            EnableRendering(kWidth, kHeight);
            Scene& scene = GetScene();

            // Sun — near-vertical so the upper hemisphere every camera sees is
            // directly lit (per docs/agent-rules/single-mesh-visual-test-lighting.md),
            // with a slight +X/+Z tilt so cast shadows land on the CAMERA side
            // of each object instead of hiding behind it (the cameras sit at
            // +X/+Z; light travelling toward +X/+Z displaces shadows that way).
            {
                m_SunEntity = scene.CreateEntity("Sun");
                auto& dl = m_SunEntity.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(0.2f, -0.95f, 0.25f));
                dl.m_Color = glm::vec3(1.0f, 0.98f, 0.95f);
                dl.m_Intensity = 3.0f;
            }

            // Static ground plane so the subject doesn't float in a void
            {
                Entity ground = scene.CreateEntity("Ground");
                auto& tc = ground.GetComponent<TransformComponent>();
                tc.Translation = { 0.0f, -1.2f, 0.0f };
                tc.Scale = { 20.0f, 0.1f, 20.0f };
                auto& mc = ground.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Cube;
                Ref<Mesh> cube = MeshPrimitives::CreateCube();
                if (cube)
                {
                    mc.m_MeshSource = cube->GetMeshSource();
                }
                auto& mat = ground.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.45f, 0.45f, 0.45f, 1.0f));
            }

            // Classic-mesh reference cube: proves classic and virtual geometry
            // coexist in the same G-Buffer, and acts as the control for shadow
            // casting (both it and the virtual sphere must shadow the ground).
            {
                Entity cube = scene.CreateEntity("ReferenceCube");
                auto& tc = cube.GetComponent<TransformComponent>();
                tc.Translation = { 2.2f, 0.2f, 0.0f };
                tc.Scale = { 0.8f, 0.8f, 0.8f };
                auto& mc = cube.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Cube;
                Ref<Mesh> cubeMesh = MeshPrimitives::CreateCube();
                if (cubeMesh)
                {
                    mc.m_MeshSource = cubeMesh->GetMeshSource();
                }
                auto& mat = cube.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.2f, 0.4f, 0.85f, 1.0f));
            }

            // Blocker slab for the shadow-RECEIVE check: parked far above the
            // scene (its shadow lands off the 20x20 ground there); the receive
            // test teleports it between the sun and the sphere so its shadow
            // bands across the sphere's lit top.
            {
                Entity blocker = scene.CreateEntity("ReceiveBlocker");
                auto& tc = blocker.GetComponent<TransformComponent>();
                tc.Translation = { -0.5f, 100.0f, -0.63f };
                tc.Scale = { 2.4f, 0.15f, 1.0f };
                auto& mc = blocker.AddComponent<MeshComponent>();
                mc.m_Primitive = MeshPrimitive::Cube;
                Ref<Mesh> blockerMesh = MeshPrimitives::CreateCube();
                if (blockerMesh)
                {
                    mc.m_MeshSource = blockerMesh->GetMeshSource();
                }
                auto& mat = blocker.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.35f, 0.35f, 0.35f, 1.0f));
                m_BlockerEntity = blocker;
            }

            // The virtual mesh: a 20k-triangle icosphere, bright red
            {
                auto meshSource = MakeIcosphereMeshSource(5); // 20480 triangles
                AssetHandle const handle = AssetManager::AddMemoryOnlyAsset(meshSource);

                Entity sphere = scene.CreateEntity("VirtualSphere");
                auto& vm = sphere.AddComponent<VirtualMeshComponent>();
                vm.m_MeshSource = handle;
                vm.m_ErrorThresholdPixels = 1.0f;
                auto& mat = sphere.AddComponent<MaterialComponent>();
                mat.m_Material.SetBaseColorFactor(glm::vec4(0.9f, 0.05f, 0.05f, 1.0f));
                mat.m_Material.SetRoughnessFactor(0.6f);
                m_SphereEntity = sphere;
            }
        }

        Entity m_SphereEntity;
        Entity m_SunEntity;
        Entity m_BlockerEntity;

        // Renders one pose and returns the composite pixels (also writes the
        // evidence PNG). Bottom-up GL row order — fine for counting.
        std::vector<u8> CaptureFrame(const char* name, const glm::vec3& position, f32 yaw, f32 pitch,
                                     f32 thresholdPixels)
        {
            m_SphereEntity.GetComponent<VirtualMeshComponent>().m_ErrorThresholdPixels = thresholdPixels;

            EditorCamera camera(45.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1f, 500.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(position, yaw, pitch);
            RunEditorFrames(camera, 3);

            std::vector<u8> rgba;
            u32 w = 0;
            u32 h = 0;
            if (!ReadbackComposite(rgba, w, h))
            {
                ADD_FAILURE() << "composite readback unavailable for " << name;
                return {};
            }
            WriteEvidencePng(std::string("VirtualGeometry_") + name + ".png", rgba, w, h);
            return rgba;
        }

        u32 CaptureAngle(const char* name, const glm::vec3& position, f32 yaw, f32 pitch, f32 thresholdPixels)
        {
            return CountRedDominantPixels(CaptureFrame(name, position, yaw, pitch, thresholdPixels));
        }
    };

    TEST_F(VirtualGeometryVisualEvidence, HardwarePathRendersTheDagCutFromMultipleAngles)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // The virtualized-geometry pass integrates through the G-Buffer:
        // Deferred path only. ApplyRendererSettings rebuilds the graph topology
        // (the fixture snapshots + restores the settings in TearDown).
        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        // Fine cut, three angles. The sphere (radius 1 at origin) should cover
        // a substantial pixel area from every viewpoint.
        struct AngleCase
        {
            const char* Name;
            glm::vec3 Position;
            f32 Yaw;
            f32 Pitch;
        };
        // SetPose yaw/pitch: yaw 0 looks toward -Z; camera positions chosen so
        // the origin-centred sphere is in frame.
        const AngleCase angles[] = {
            { "Front", { 0.0f, 0.5f, 5.0f }, 0.0f, 0.05f },
            { "ThreeQuarter", { 3.5f, 1.8f, 3.5f }, glm::radians(-45.0f), glm::radians(20.0f) },
            { "Above", { 0.0f, 5.5f, 0.6f }, 0.0f, glm::radians(80.0f) },
        };

        constexpr u32 kMinSpherePixels = 800; // ~0.35% of the frame — far below the real coverage, well above noise

        for (const AngleCase& angle : angles)
        {
            u32 const redPixels = CaptureAngle(angle.Name, angle.Position, angle.Yaw, angle.Pitch, 1.0f);
            EXPECT_GE(redPixels, kMinSpherePixels)
                << "virtual-geometry sphere not visible from angle '" << angle.Name
                << "' — the GPU cull/MDI/vertex-pulling path drew nothing (or the wrong thing)";
        }

        // TEMP-DIAG: dump the CSM cascade-0 depth as a min-max-normalized PNG.
        // Very coarse threshold: the DAG cut collapses toward the root
        // clusters, but terminal groups guarantee SOMETHING always renders.
        u32 const coarsePixels = CaptureAngle("CoarseCut", { 0.0f, 0.5f, 5.0f }, 0.0f, 0.05f, 64.0f);
        EXPECT_GE(coarsePixels, kMinSpherePixels)
            << "coarse LOD cut rendered nothing — terminal-group selection broken on the GPU path";
    }

    // SoftwareRasterizerMatchesHardwareRaster USED TO LIVE HERE — deleted (issue #629).
    //
    // It advertised SW-vs-HW parity but rendered an UNTEXTURED red icosphere and compared
    // RED-PIXEL COVERAGE at a 10% tolerance. That metric cannot see a shading difference at
    // all: it would have sailed straight through the inverted normal map that actually
    // shipped in VirtualVisibilityResolve.glsl (a blue-channel tangent z + a flipped
    // bitangent), because an inverted normal moves the colour, not the silhouette. A test
    // that cannot fail on the bug it claims to guard is worse than no test — it is a green
    // light.
    //
    // Its ONE unique contribution was coverage of the portable two-pass 2x32 software
    // rasterizer (SetForcePortableSwRaster). That is now folded into
    // VirtualGeometryRasterParity.SoftwareRasterShadesNormalMappedSurfaceLikeHardware, which
    // measures a PER-CHANNEL RGB diff on a normal-mapped, textured, two-sided subject — and
    // runs BOTH rasterizers through it.

    // The cluster / LOD / overdraw debug capture targets (issue #629): setting a
    // debug mode makes both raster paths populate the "VirtualGeometryDebug"
    // colour target, which is read back here and (for cluster id) must show many
    // distinct colours across the sphere.
    TEST_F(VirtualGeometryVisualEvidence, DebugCaptureTargetsRenderClusterLodAndOverdraw)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        auto& registry = VirtualMeshRegistry::Get();

        auto renderAndReadDebug = [&](VirtualDebugMode mode, const char* pngName) -> std::vector<u8>
        {
            registry.SetDebugMode(mode);
            EditorCamera camera(45.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1f, 500.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose({ 0.0f, 0.5f, 5.0f }, 0.0f, 0.05f);
            RunEditorFrames(camera, 3);

            std::vector<u8> rgba;
            u32 const texID = registry.GetDebugColorTextureID();
            u32 const w = registry.GetDebugWidth();
            u32 const h = registry.GetDebugHeight();
            if (texID == 0 || w == 0 || h == 0)
                return rgba;
            rgba.resize(static_cast<sizet>(w) * h * 4);
            glGetTextureImage(texID, 0, GL_RGBA, GL_UNSIGNED_BYTE, static_cast<GLsizei>(rgba.size()), rgba.data());
            if (pngName != nullptr)
                WriteEvidencePng(std::string("VirtualGeometry_") + pngName + ".png", rgba, w, h);
            return rgba;
        };

        // Returns { non-black pixel count, distinct non-black colour count }.
        auto analyze = [](const std::vector<u8>& rgba) -> std::pair<u32, u32>
        {
            std::set<u32> colors;
            u32 nonBlack = 0;
            for (sizet i = 0; i + 3 < rgba.size(); i += 4)
            {
                if ((rgba[i] | rgba[i + 1] | rgba[i + 2]) != 0u)
                {
                    ++nonBlack;
                    colors.insert(static_cast<u32>(rgba[i]) | (static_cast<u32>(rgba[i + 1]) << 8) |
                                  (static_cast<u32>(rgba[i + 2]) << 16));
                }
            }
            return { nonBlack, static_cast<u32>(colors.size()) };
        };

        auto const [clusterNonBlack, clusterColors] =
            analyze(renderAndReadDebug(VirtualDebugMode::ClusterId, "Debug_ClusterId"));
        EXPECT_GT(clusterNonBlack, 800u) << "cluster-id debug target is empty";
        EXPECT_GT(clusterColors, 4u) << "cluster-id debug target shows too few distinct clusters (" << clusterColors << ")";

        auto const [lodNonBlack, lodColors] = analyze(renderAndReadDebug(VirtualDebugMode::Lod, "Debug_Lod"));
        EXPECT_GT(lodNonBlack, 800u) << "LOD debug target is empty";
        EXPECT_GE(lodColors, 1u);

        auto const [odNonBlack, odColors] = analyze(renderAndReadDebug(VirtualDebugMode::Overdraw, "Debug_Overdraw"));
        EXPECT_GT(odNonBlack, 800u) << "overdraw debug target is empty";
        EXPECT_GE(odColors, 1u);

        registry.SetDebugMode(VirtualDebugMode::Off); // don't leak the mode
    }

    // AC5 extension: virtual geometry renders through a MULTISAMPLE deferred
    // G-Buffer (it previously skipped MSAA and vanished). Both the non-per-sample
    // (resolved-target) and per-sample (multisample-target + resolve) modes must
    // put the sphere on screen.
    TEST_F(VirtualGeometryVisualEvidence, RendersUnderDeferredMSAA)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        auto& settings = Renderer3D::GetRendererSettings();
        settings.Path = RenderingPath::Deferred;
        u32 const msaaWas = settings.Deferred.MSAASampleCount;
        bool const perSampleWas = settings.Deferred.PerSampleLighting;

        auto renderMsaa = [&](u32 samples, bool perSample, const char* name) -> u32
        {
            settings.Deferred.MSAASampleCount = samples;
            settings.Deferred.PerSampleLighting = perSample;
            Renderer3D::ApplyRendererSettings();
            return CaptureAngle(name, { 0.0f, 0.5f, 5.0f }, 0.0f, 0.05f, 1.0f);
        };

        u32 const redResolved = renderMsaa(4, false, "MSAA_Resolved");
        u32 const redPerSample = renderMsaa(4, true, "MSAA_PerSample");

        settings.Deferred.MSAASampleCount = msaaWas;
        settings.Deferred.PerSampleLighting = perSampleWas;
        Renderer3D::ApplyRendererSettings();

        EXPECT_GE(redResolved, 800u)
            << "virtual sphere did not render under deferred 4x MSAA (non-per-sample) — the pass skipped the MS G-Buffer";
        EXPECT_GE(redPerSample, 800u)
            << "virtual sphere did not render under deferred 4x MSAA per-sample — MS draw/resolve/re-export broken";
    }

    // The per-frame cull-stats readback (VirtualCullStats) that drives the editor
    // stats overlay must reflect the real cull result and honour the counting
    // invariant tested >= cut-selected >= drawn.
    TEST_F(VirtualGeometryVisualEvidence, FrameCullStatsReflectTheDrawnClusters)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        auto& registry = VirtualMeshRegistry::Get();
        Renderer3D::EnableHZBOcclusionCulling(false); // isolate stats from occlusion culling

        u32 const redPixels = CaptureAngle("StatsFront", { 0.0f, 0.5f, 5.0f }, 0.0f, 0.05f, 1.0f);
        ASSERT_GE(redPixels, 800u) << "sphere not rendered — stats would be meaningless";

        VirtualCullStats const stats = registry.ReadFrameCullStats();
        EXPECT_EQ(stats.InstanceCount, 1u) << "expected exactly one virtual-mesh instance";
        EXPECT_GT(stats.DrawnClusters(), 0u) << "cull stats report no drawn clusters despite a visible sphere";
        // tested >= cut-selected >= drawn (each stage is a subset of the prior).
        EXPECT_GE(stats.TestedClusters, stats.CutSelected);
        EXPECT_GE(stats.CutSelected, stats.DrawnClusters());
    }

    // Hi-Z cluster occlusion (issue #629): clusters hidden behind already-drawn
    // opaque geometry are culled before the MDI/SW-raster draw, and the cull is
    // conservative — it must NEVER remove a visible cluster.
    TEST_F(VirtualGeometryVisualEvidence, HiZOcclusionCullsHiddenClustersWithoutRemovingVisibleOnes)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        auto& registry = VirtualMeshRegistry::Get();
        registry.SetSwRasterMode(VirtualSwRasterMode::Disabled); // isolate the cull from SW routing

        // Total drawn clusters (hardware MDI + software raster) across every
        // instance, read back from the per-instance cull args after a frame.
        auto drawnClusters = [&registry]() -> u32
        {
            const auto& instances = registry.GetFrameInstances();
            if (instances.empty() || !registry.GetArgsBuffer())
                return 0;
            std::vector<VirtualDrawArgs> args(instances.size());
            glGetNamedBufferSubData(registry.GetArgsBuffer()->GetRendererID(), 0,
                                    static_cast<GLsizeiptr>(args.size() * sizeof(VirtualDrawArgs)), args.data());
            u32 drawn = 0;
            for (const VirtualDrawArgs& a : args)
                drawn += a.DrawCount + a.SwCount;
            return drawn;
        };

        m_SphereEntity.GetComponent<VirtualMeshComponent>().m_ErrorThresholdPixels = 1.0f;
        auto renderFront = [this](const char* pngName) -> std::vector<u8>
        {
            EditorCamera camera(45.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1f, 500.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose({ 0.0f, 0.5f, 5.0f }, 0.0f, 0.05f); // Front: looks toward -Z at the origin sphere
            RunEditorFrames(camera, 3);
            std::vector<u8> rgba;
            u32 w = 0;
            u32 h = 0;
            if (ReadbackComposite(rgba, w, h) && pngName != nullptr)
                WriteEvidencePng(std::string("VirtualGeometry_") + pngName + ".png", rgba, w, h);
            return rgba;
        };

        // ── Baseline: no occluder, HZB occlusion OFF ──
        Renderer3D::EnableHZBOcclusionCulling(false);
        std::vector<u8> const baseFrame = renderFront("HiZ_OcclusionOff");
        u32 const baseDrawn = drawnClusters();
        u32 const baseRed = CountRedDominantPixels(baseFrame);
        ASSERT_GE(baseRed, 800u) << "baseline sphere not visible";
        ASSERT_GT(baseDrawn, 0u) << "baseline drew no clusters";

        // ── Conservatism: no occluder, HZB occlusion ON. The visible sphere must
        // look the same and keep (essentially) the same drawn-cluster count — the
        // Hi-Z test must never cull a visible cluster (no holes/cracks). ──
        Renderer3D::EnableHZBOcclusionCulling(true);
        std::vector<u8> const visibleFrame = renderFront("HiZ_OcclusionOn");
        u32 const visibleDrawn = drawnClusters();
        u32 const visibleRed = CountRedDominantPixels(visibleFrame);

        EXPECT_GE(visibleRed, static_cast<u32>(static_cast<f64>(baseRed) * 0.95))
            << "HZB occlusion removed visible sphere pixels (" << visibleRed << " vs " << baseRed
            << ") — a false cull punched a hole";
        EXPECT_GE(visibleDrawn, static_cast<u32>(static_cast<f64>(baseDrawn) * 0.9))
            << "HZB occlusion culled visible clusters (" << visibleDrawn << " vs " << baseDrawn << ")";

        // ── Effectiveness: park a large opaque wall between the camera and the
        // sphere (the classic-mesh blocker → it lands in the scene depth the HZB
        // reads), HZB occlusion ON. The now-hidden sphere clusters must be culled
        // before drawing, so the drawn-cluster count collapses. ──
        {
            auto& tc = m_BlockerEntity.GetComponent<TransformComponent>();
            tc.Translation = { 0.0f, 0.5f, 2.5f }; // between camera (z=5) and sphere (z=0)
            tc.Scale = { 3.0f, 3.0f, 0.2f };       // fully covers the radius-1 sphere in screen space
        }
        std::vector<u8> const occludedFrame = renderFront("HiZ_BehindWall");
        u32 const occludedDrawn = drawnClusters();
        (void)occludedFrame;

        EXPECT_LT(occludedDrawn, static_cast<u32>(static_cast<f64>(baseDrawn) * 0.5))
            << "HZB occlusion did not cull the hidden sphere: " << occludedDrawn << " clusters drawn vs a "
            << baseDrawn << "-cluster baseline (the wall should hide most of them)";

        Renderer3D::EnableHZBOcclusionCulling(false); // don't leak the toggle
        registry.SetSwRasterMode(VirtualSwRasterMode::Auto);
    }

    TEST_F(VirtualGeometryVisualEvidence, StreamingResidencyConvergesUnderTightBudget)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        auto& registry = VirtualMeshRegistry::Get();
        registry.SetSwRasterMode(VirtualSwRasterMode::Disabled); // isolate residency from raster routing
        registry.SetPageBudgetSlots(4);                          // clamped up to pinned+2 internally if needed

        m_SphereEntity.GetComponent<VirtualMeshComponent>().m_ErrorThresholdPixels = 1.0f;
        EditorCamera camera(45.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1f, 500.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        camera.SetPose({ 0.0f, 0.5f, 5.0f }, 0.0f, 0.05f);

        // Two warmup ticks let ApplyRendererSettings rebuild the graph (the
        // first tick after a path switch renders nothing — same as the other
        // tests' warmup). The tight budget keeps streaming pressure on
        // regardless of how many pages get requested during warmup.
        RunEditorFrames(camera, 2);

        std::vector<u8> frame;
        u32 w = 0;
        u32 h = 0;
        RunEditorFrames(camera, 1);
        ASSERT_TRUE(ReadbackComposite(frame, w, h));
        u32 const coarseBaseline = CountRedDominantPixels(frame);
        EXPECT_GE(coarseBaseline, 800u)
            << "the mesh must render under a tight page budget (resident-ancestor fallback)";

        {
            const VirtualResidencyStats& stats = registry.GetResidencyStats();
            EXPECT_GT(stats.TotalPages, stats.BudgetSlots)
                << "test setup: the budget must be tighter than the page count to exercise streaming";
            EXPECT_LE(stats.ResidentPages, stats.BudgetSlots);
        }

        // Stream: each frame the cull requests the missing finer pages and the
        // residency manager uploads/evicts under the budget. The mesh must stay
        // visible (no holes) and the budget must never be exceeded.
        u32 minRed = coarseBaseline;
        for (int frameIndex = 0; frameIndex < 20; ++frameIndex)
        {
            RunEditorFrames(camera, 1);
            ASSERT_TRUE(ReadbackComposite(frame, w, h));
            minRed = std::min(minRed, CountRedDominantPixels(frame));

            const VirtualResidencyStats& stats = registry.GetResidencyStats();
            EXPECT_LE(stats.ResidentPages, stats.BudgetSlots)
                << "streaming exceeded the fixed page budget at frame " << frameIndex;
        }

        {
            const VirtualResidencyStats& stats = registry.GetResidencyStats();
            EXPECT_GT(stats.PageUploads, static_cast<u64>(stats.PinnedPages))
                << "no pages streamed in beyond the pinned set — request path broken";
            EXPECT_GT(stats.PageEvictions, 0ull)
                << "a fine cut over a tight budget must evict pages (LRU) — none happened";
        }
        EXPECT_GE(minRed, coarseBaseline / 2)
            << "the sphere dropped out (hole) during streaming — the residency-clamped cut must "
               "always fall back to a resident ancestor";

        // Restore the defaults for later tests in this process
        registry.SetPageBudgetSlots(0);
        registry.SetSwRasterMode(VirtualSwRasterMode::Auto);
    }

    // Slice-6 acceptance: virtual meshes rasterize into the CSM shadow map
    // (CAST) and their shaded G-Buffer pixels darken under another caster's
    // shadow (RECEIVE). Every contract is a pixel DIFFERENTIAL between two
    // captures that differ in exactly one flag — same geometry, same camera —
    // so the checks are driver-independent and need no committed goldens.
    TEST_F(VirtualGeometryVisualEvidence, VirtualMeshCastsAndReceivesShadows)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        // Defensive: another test in this process may have toggled the global.
        ShadowSettings settings = Renderer3D::GetShadowMap().GetSettings();
        settings.Enabled = true;
        Renderer3D::GetShadowMap().SetSettings(settings);

        // The CSM depth range is the camera sub-frustum extended by a fixed
        // ±200-unit caster padding (ShadowMap::ComputeCSMCascades, zPadding),
        // so the 0.005 depth bias equals ~2 WORLD units along the light: any
        // caster closer than that to its receiver is cancelled by its own
        // bias. Like ShadowEarlyOutVisualEvidenceTest's floating casters, the
        // sphere therefore FLOATS well above the ground for this test, and the
        // receive blocker sits several units above the sphere.
        auto& sphereTc = m_SphereEntity.GetComponent<TransformComponent>();
        glm::vec3 const sphereHome = sphereTc.Translation;
        sphereTc.Translation = { 0.0f, 4.0f, 0.0f };

        // Camera raised with the sphere and pulled back far enough that BOTH
        // the sphere's apex (where the receive blocker's shadow lands) and its
        // ground shadow fit in one frame; the near-vertical sun tilts toward
        // +X/+Z so that ground shadow lands on the camera side, not hidden
        // behind the sphere.
        const glm::vec3 pos{ 7.0f, 6.5f, 7.0f };
        f32 const yaw = glm::radians(-45.0f);
        f32 const pitch = glm::radians(26.0f);

        const auto luma = [](const u8* p) -> f64
        { return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]; };
        constexpr f64 kDarkenDelta = 25.0;
        constexpr f64 kLitLuma = 60.0;

        // Pixels lit in `lit` that turn meaningfully darker in `shadowed`.
        const auto darkened = [&](const std::vector<u8>& lit, const std::vector<u8>& shadowed) -> sizet
        {
            sizet count = 0;
            sizet const n = std::min(lit.size(), shadowed.size());
            for (sizet i = 0; i + 3 < n; i += 4)
            {
                f64 const lLit = luma(lit.data() + i);
                if (lLit > kLitLuma && luma(shadowed.data() + i) < lLit - kDarkenDelta)
                {
                    ++count;
                }
            }
            return count;
        };

        // ── CAST ──────────────────────────────────────────────────────────
        std::vector<u8> const shadowsOn = CaptureFrame("ShadowCastOn", pos, yaw, pitch, 1.0f);
        ASSERT_FALSE(shadowsOn.empty());

        auto& sun = m_SunEntity.GetComponent<DirectionalLightComponent>();
        sun.m_CastShadows = false;
        std::vector<u8> const shadowsOff = CaptureFrame("ShadowCastOff", pos, yaw, pitch, 1.0f);
        sun.m_CastShadows = true;
        ASSERT_FALSE(shadowsOff.empty());

        m_SphereEntity.GetComponent<VirtualMeshComponent>().m_CastShadows = false;
        std::vector<u8> const vgCastOff = CaptureFrame("ShadowVgCastOff", pos, yaw, pitch, 1.0f);
        m_SphereEntity.GetComponent<VirtualMeshComponent>().m_CastShadows = true;
        ASSERT_FALSE(vgCastOff.empty());

        sizet const sunDarkens = darkened(shadowsOff, shadowsOn);
        sizet const spurious = darkened(shadowsOn, shadowsOff);
        // The virtual sphere's own contribution: pixels shadowed with the VG
        // caster on that are lit with ONLY the VG caster's cast flag off.
        sizet const vgDarkens = darkened(vgCastOff, shadowsOn);

        EXPECT_GT(sunDarkens, static_cast<sizet>(500))
            << "enabling the sun's shadows darkened almost nothing — CSM broken in the deferred path";
        EXPECT_LT(spurious, sunDarkens / 10 + 10)
            << "the no-shadow frame has dark patches the shadowed frame lacks (stale shadow state)";
        EXPECT_GT(vgDarkens, static_cast<sizet>(200))
            << "toggling VirtualMeshComponent::m_CastShadows changed no ground pixels — the "
               "virtualized-geometry shadow replay is not reaching the CSM map";

        // ── RECEIVE ───────────────────────────────────────────────────────
        // Teleport the blocker slab between the sun and the sphere. The two
        // captures differ only in the sun's cast flag, so blocker-covered
        // pixels are identical geometry in both; the sphere pixels that
        // darken are receiving the blocker's shadow through the deferred
        // lighting's shadow factor.
        // Six units up the light ray from the floating sphere's centre, so the
        // slab's shadow lands back on the sphere's lit apex with ~5 units of
        // caster-receiver separation — comfortably past the world-space bias.
        auto& blockerTc = m_BlockerEntity.GetComponent<TransformComponent>();
        glm::vec3 const parked = blockerTc.Translation;
        blockerTc.Translation = { -1.2f, 9.7f, -1.5f };

        std::vector<u8> const receiveOn = CaptureFrame("ShadowReceiveOn", pos, yaw, pitch, 1.0f);
        sun.m_CastShadows = false;
        std::vector<u8> const receiveOff = CaptureFrame("ShadowReceiveOff", pos, yaw, pitch, 1.0f);
        sun.m_CastShadows = true;
        blockerTc.Translation = parked;
        sphereTc.Translation = sphereHome;
        ASSERT_FALSE(receiveOn.empty());
        ASSERT_FALSE(receiveOff.empty());

        // Restrict the diff to pixels that read as the RED sphere in the lit
        // capture — ground darkening doesn't count as the sphere receiving.
        sizet vgReceives = 0;
        sizet const n = std::min(receiveOn.size(), receiveOff.size());
        for (sizet i = 0; i + 3 < n; i += 4)
        {
            const u8* lit = receiveOff.data() + i;
            bool const redDominant = lit[0] > 60 && lit[0] > lit[1] + 25 && lit[0] > lit[2] + 25;
            if (redDominant && luma(receiveOn.data() + i) < luma(lit) - kDarkenDelta)
            {
                ++vgReceives;
            }
        }
        EXPECT_GT(vgReceives, static_cast<sizet>(200))
            << "no red sphere pixels darkened under the blocker's shadow — virtual-mesh G-Buffer "
               "pixels are not receiving CSM shadows in the deferred resolve";
    }

    // AC6 extension: virtual meshes also cast into the local-light shadow ATLAS
    // (spot / point perspective tiles), not just the CSM cascades. A spot light
    // straight above the sphere casts its shadow onto the ground; toggling the
    // virtual mesh's cast flag must change the ground darkening — proving the
    // cluster cull + depth replay reach the atlas tile (perspective VP).
    TEST_F(VirtualGeometryVisualEvidence, VirtualMeshCastsIntoTheLocalLightShadowAtlas)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        ShadowSettings settings = Renderer3D::GetShadowMap().GetSettings();
        settings.Enabled = true;
        Renderer3D::GetShadowMap().SetSettings(settings);

        // Isolate the atlas (spot) contribution: silence the directional sun
        // entirely (its fill would wash out the spot's shadow of the sphere) so
        // only the spot lights the scene and its shadow reads as a clear delta.
        auto& sun = m_SunEntity.GetComponent<DirectionalLightComponent>();
        bool const sunCastWas = sun.m_CastShadows;
        f32 const sunIntensityWas = sun.m_Intensity;
        sun.m_CastShadows = false;
        sun.m_Intensity = 0.0f;

        // Geometry mirrors ShadowAtlasVisualEvidenceTest (a proven atlas setup):
        // the sphere floats at y=3 with a spot straight down at y=8, and a
        // top-down camera so the ground shadow directly below the sphere is not
        // hidden behind the sphere itself.
        auto& sphereTc = m_SphereEntity.GetComponent<TransformComponent>();
        glm::vec3 const sphereHome = sphereTc.Translation;
        sphereTc.Translation = { 0.0f, 3.0f, 0.0f };

        Entity spot = GetScene().CreateEntity("AtlasSpot");
        {
            auto& stc = spot.GetComponent<TransformComponent>();
            stc.Translation = { 0.0f, 8.0f, 0.0f };
            auto& sl = spot.AddComponent<SpotLightComponent>();
            sl.m_Direction = { 0.0f, -1.0f, 0.0f };
            sl.m_Color = { 1.0f, 1.0f, 1.0f };
            sl.m_Intensity = 60.0f;
            sl.m_Range = 16.0f;
            sl.m_InnerCutoff = 25.0f;
            sl.m_OuterCutoff = 34.0f;
            sl.m_CastShadows = true;
        }

        const glm::vec3 pos{ 0.0f, 22.0f, 18.0f };
        f32 const yaw = 0.0f;
        f32 const pitch = 0.84f; // radians, tilts down (matches ShadowAtlasVisualEvidenceTest)

        std::vector<u8> const castOn = CaptureFrame("AtlasShadowVgOn", pos, yaw, pitch, 1.0f);
        // The spot must have secured an atlas tile, or there is nothing to cast into.
        ASSERT_GT(Renderer3D::GetShadowMap().GetAtlasEntryCount(), 0u)
            << "the spot light did not get a shadow-atlas entry — test setup, not the VG path";
        m_SphereEntity.GetComponent<VirtualMeshComponent>().m_CastShadows = false;
        std::vector<u8> const castOff = CaptureFrame("AtlasShadowVgOff", pos, yaw, pitch, 1.0f);
        m_SphereEntity.GetComponent<VirtualMeshComponent>().m_CastShadows = true;

        GetScene().DestroyEntity(spot);
        sphereTc.Translation = sphereHome;
        sun.m_CastShadows = sunCastWas;
        sun.m_Intensity = sunIntensityWas;

        ASSERT_FALSE(castOn.empty());
        ASSERT_FALSE(castOff.empty());

        // Ground pixels lit by the spot in castOff (no virtual shadow) that turn
        // darker in castOn (virtual sphere's atlas shadow) — the sphere's shadow.
        const auto luma = [](const u8* p) -> f64
        { return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]; };
        sizet vgAtlasDarkens = 0;
        sizet const n = std::min(castOn.size(), castOff.size());
        for (sizet i = 0; i + 3 < n; i += 4)
        {
            f64 const lOff = luma(castOff.data() + i);
            if (lOff > 40.0 && luma(castOn.data() + i) < lOff - 20.0)
            {
                ++vgAtlasDarkens;
            }
        }
        EXPECT_GT(vgAtlasDarkens, static_cast<sizet>(150))
            << "toggling VirtualMeshComponent::m_CastShadows changed no pixels under the spot light — "
               "the virtualized-geometry shadow replay is not reaching the local-light atlas tile";
    }
} // namespace OloEngine::Tests

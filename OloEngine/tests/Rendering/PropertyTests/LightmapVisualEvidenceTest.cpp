// OLO_TEST_LAYER: L8
// =============================================================================
// LightmapVisualEvidenceTest.cpp
//
// GPU visual evidence for the baked-GI lightmap path (issue #439): the bake's
// colour bleeding must be visible in the FINAL COMPOSITED FRAME, through the
// same pipeline the editor viewport uses — Scene::OnUpdateRuntime ->
// ProcessScene3DSharedLogic (LightmapUBO upload + per-draw
// InstanceData::LightmapScaleOffset) -> forward PBR_MultiLight ->
// sampleLightmapIrradiance -> tone-map -> UIComposite readback.
//
// This is the GPU complement of the headless LightmapBakeParityTest, which
// already pins the bake's determinism, its bit-exact oracle parity, and the
// red/green floor-bleed ordering IN THE ATLAS TEXELS. None of that is
// re-asserted here. What only a full-pipeline render can prove — and what this
// test owns — is the delivery chain: the unwrapped UV2 stream reaches the VAO,
// the runtime resolve accepts the bake (the staleness gate!), the atlas is
// bound and the scale/offset addresses the right region, and the sampled
// irradiance actually tints the pixels the physics says it must.
//
// Scene: the parity test's colour-bleed room, as ECS entities — a grey floor,
// a RED wall at -X, a GREEN wall at +X, a grey ceiling (bounce closure), one
// white point light inside, and a perspective camera INSIDE the room looking
// down its open -Z length so both walls and the floor are in frame. Every room
// entity is MeshComponent{ m_LightmapStatic = true } with its OWN
// MeshPrimitives::CreateCube() MeshSource (CreateCube returns a fresh
// MeshSource per call — verified — so the in-place unwrap of one entity can
// never alias another) and a MaterialComponent carrying the wall colour.
//
// The bake mirrors EditorLayer::BakeLightmaps step for step:
//   gather inputs -> Prepare (unwraps in place) -> re-Build() every mesh (GL
//   context present, so the VAOs pick up the seam-split vertices + UV2 stream)
//   -> ComputeBakeKey AFTER the unwrap (the key hashes vertex counts; a key
//   computed before Prepare would never match what Resolve() recomputes) ->
//   ReferenceSceneBuilder::AddScene with the m_LightmapStatic predicate ->
//   BakeTexels. The baked LightmapAsset is registered via
//   AssetManager::AddMemoryOnlyAsset (a throwaway temp project + Editor
//   AssetManager is mounted first, the VirtualGeometryVisualEvidenceTest
//   pattern), the scene's SceneLightmapSettings points at it, and
//   SceneLightmapRuntime::Resolve must come back IsValid() — if it does not,
//   the feature's staleness gate is miswired and the test says so loudly.
//
// Contracts (all driver-independent REGION MEANS, never per-pixel):
//   1. ON frame: the floor strip beside the RED wall is red-shifted
//      (r > g * 1.05) and the strip beside the GREEN wall green-shifted —
//      the direct light is white, so channel asymmetry on a grey floor can
//      only come from the baked indirect term.
//   2. ON vs OFF (SceneLightmapSettings::Enabled = false): the floor regions
//      differ measurably (mean |diff| > 3 grey levels — clear of the ~2-3
//      level cross-test TAA/exposure residue, light-path-photometric-parity
//      rule 4), ON >= OFF in mean luminance (baked indirect only ADDS), and
//      the ADDED light is itself colour-shifted toward the adjacent wall.
//   3. Basic sanity: the ON frame is neither flat nor black (the
//      SceneRenderEvidenceTest contracts; note the composite clears to
//      MID-GREY ~86/255, not black).
//
// Evidence PNGs (written BEFORE any assertion, so the frames survive a red
// run), under OloEditor/assets/tests/visual/ when run from OloEditor/:
//   Lightmap_VisualEvidence_On.png      — primary camera, bake enabled
//   Lightmap_VisualEvidence_Off.png     — same pose, Enabled = false
//   Lightmap_VisualEvidence_Raking.png  — editor camera raking down the floor
//   Lightmap_VisualEvidence_RedSide.png — editor camera facing the red wall
//
// Cost: ~600-800 texel jobs x 40 spp x <=3 bounces — the bake dominates at
// roughly a few seconds in Debug; the frames are cheap.
//
// Classification: L8 / integration (full GL pipeline, RGBA8 readback + PNG).
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "TestTempDir.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Baking/LightmapBaker.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/LightmapAsset.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/PathTracing/ReferenceSceneBuilder.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneLightmap.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>

#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kSize = 512; // square frame, aspect 1 — keeps region math symmetric

        // ---------------------------------------------------------------------
        // Screen-space analysis regions (fractions of the frame; rows TOP-DOWN
        // — the readback is flipped before analysis so PNG and math agree).
        //
        // Derivation. Camera eye E = (0, 1.5, 1.7), identity rotation (looks
        // down -Z), vertical FOV 90 deg => tan(fov/2) = 1, aspect 1. A floor
        // point (x, 0, z) has camera depth d = 1.7 - z and projects to
        //   u = (1 + x/d) / 2,   v = (1 + 1.5/d) / 2   (v top-down).
        // Unprojecting the red rect's corners onto the floor plane y = 0:
        //   (u=0.08, v=0.790) -> world (-2.17, 0, -0.89)
        //   (u=0.18, v=0.735) -> world (-2.04, 0, -1.49)
        //   (u=0.08, v=0.735) -> world (-2.68, 0, -1.49)
        //   (u=0.18, v=0.790) -> world (-1.66, 0, -0.89)
        // i.e. a floor strip x in ~[-2.7, -1.7], z in ~[-1.5, -0.9] — floor
        // only (the red wall's inner face is at x = -3; the seam projects at
        // u < 0.09 for these v), 0.3–1.3 m from the red wall, unoccluded from
        // the eye. The green rect is the exact x-mirror.
        // ---------------------------------------------------------------------
        constexpr f32 kFloorRegionY0 = 0.735f;
        constexpr f32 kFloorRegionY1 = 0.790f;
        constexpr f32 kRedRegionX0 = 0.08f;
        constexpr f32 kRedRegionX1 = 0.18f;
        constexpr f32 kGreenRegionX0 = 1.0f - kRedRegionX1;
        constexpr f32 kGreenRegionX1 = 1.0f - kRedRegionX0;

        struct PixelRect
        {
            u32 X0 = 0;
            u32 X1 = 0;
            u32 Y0 = 0;
            u32 Y1 = 0;

            [[nodiscard]] u32 PixelCount() const
            {
                return (X1 - X0) * (Y1 - Y0);
            }
        };

        [[nodiscard]] PixelRect MakeRect(f32 x0, f32 x1, f32 y0, f32 y1, u32 w, u32 h)
        {
            PixelRect rect;
            rect.X0 = static_cast<u32>(x0 * static_cast<f32>(w));
            rect.X1 = static_cast<u32>(x1 * static_cast<f32>(w));
            rect.Y0 = static_cast<u32>(y0 * static_cast<f32>(h));
            rect.Y1 = static_cast<u32>(y1 * static_cast<f32>(h));
            return rect;
        }

        struct RegionMeans
        {
            f32 R = 0.0f; // 0..255 units
            f32 G = 0.0f;
            f32 B = 0.0f;

            [[nodiscard]] f32 Luminance() const
            {
                return 0.2126f * R + 0.7152f * G + 0.0722f * B;
            }
        };

        // Mean per-channel value (0..255) over a rect of a tightly-packed,
        // top-down RGBA8 buffer.
        [[nodiscard]] RegionMeans MeanChannelsInRect(const std::vector<u8>& px, u32 w, const PixelRect& rect)
        {
            f64 sumR = 0.0;
            f64 sumG = 0.0;
            f64 sumB = 0.0;
            u32 count = 0;
            for (u32 y = rect.Y0; y < rect.Y1; ++y)
            {
                for (u32 x = rect.X0; x < rect.X1; ++x)
                {
                    const std::size_t idx = (static_cast<std::size_t>(y) * w + x) * 4;
                    sumR += px[idx + 0];
                    sumG += px[idx + 1];
                    sumB += px[idx + 2];
                    ++count;
                }
            }
            RegionMeans means;
            if (count > 0)
            {
                means.R = static_cast<f32>(sumR / count);
                means.G = static_cast<f32>(sumG / count);
                means.B = static_cast<f32>(sumB / count);
            }
            return means;
        }

        // Mean absolute per-channel (RGB) difference (0..255) between two
        // equal-size top-down RGBA8 buffers over a rect.
        [[nodiscard]] f32 MeanAbsDiffInRect(const std::vector<u8>& a, const std::vector<u8>& b, u32 w,
                                            const PixelRect& rect)
        {
            f64 sum = 0.0;
            u32 count = 0;
            for (u32 y = rect.Y0; y < rect.Y1; ++y)
            {
                for (u32 x = rect.X0; x < rect.X1; ++x)
                {
                    const std::size_t idx = (static_cast<std::size_t>(y) * w + x) * 4;
                    for (u32 c = 0; c < 3; ++c)
                    {
                        sum += std::abs(static_cast<f64>(a[idx + c]) - static_cast<f64>(b[idx + c]));
                        ++count;
                    }
                }
            }
            return count ? static_cast<f32>(sum / count) : 0.0f;
        }

        // GL readback is bottom-up; flip so row 0 is the TOP — the PNGs then
        // match what a viewer expects and the region math reads naturally.
        void FlipRowsInPlace(std::vector<u8>& px, u32 w, u32 h)
        {
            const std::size_t stride = static_cast<std::size_t>(w) * 4;
            std::vector<u8> rowScratch(stride);
            for (u32 y = 0; y < h / 2; ++y)
            {
                u8* top = px.data() + static_cast<std::size_t>(y) * stride;
                u8* bottom = px.data() + static_cast<std::size_t>(h - 1 - y) * stride;
                std::memcpy(rowScratch.data(), top, stride);
                std::memcpy(top, bottom, stride);
                std::memcpy(bottom, rowScratch.data(), stride);
            }
        }

        [[nodiscard]] fs::path VisualOutputPath(const char* fileName)
        {
            fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            return dir / fileName;
        }

        void WriteEvidencePng(const char* fileName, const std::vector<u8>& px, u32 w, u32 h)
        {
            const fs::path out = VisualOutputPath(fileName);
            const int wrote = ::stbi_write_png(out.string().c_str(), static_cast<int>(w), static_cast<int>(h), 4,
                                               px.data(), static_cast<int>(w) * 4);
            EXPECT_NE(wrote, 0) << "failed to write visual evidence PNG to " << out.string();
        }

        // EditorCamera (yaw, pitch) looking from `eye` toward `target` — the
        // DDGIVisualEvidenceTest derivation: forward =
        // (sin(yaw)cos(pitch), -sin(pitch), -cos(yaw)cos(pitch)), so
        //   pitch = asin(-d.y), yaw = atan2(d.x, -d.z).
        struct YawPitch
        {
            f32 Yaw = 0.0f;
            f32 Pitch = 0.0f;
        };

        [[nodiscard]] YawPitch LookAtYawPitch(const glm::vec3& eye, const glm::vec3& target)
        {
            const glm::vec3 d = glm::normalize(target - eye);
            return { std::atan2(d.x, -d.z), std::asin(glm::clamp(-d.y, -1.0f, 1.0f)) };
        }
    } // namespace

    // -------------------------------------------------------------------------
    // LightmapBleedRoom — the parity test's colour-bleed room as a real scene.
    // -------------------------------------------------------------------------
    class LightmapBleedRoom : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            // AssetManager::AddMemoryOnlyAsset / GetAsset (which
            // SceneLightmapRuntime::Resolve uses to load the baked asset) need
            // an active project + asset manager. Mount a throwaway temp
            // project, mirroring VirtualGeometryVisualEvidenceTest (incl. its
            // "leave the manager installed at teardown" rationale —
            // SetAssetManager asserts non-null).
            if (!Project::GetActive() || !Project::GetAssetManager())
            {
                std::error_code ec;
                fs::path const projectDir = TempDir("project");
                fs::create_directories(projectDir / "Assets", ec);
                ASSERT_FALSE(ec) << "failed to create temp project dir";
                {
                    std::ofstream proj(projectDir / "Evidence.oloproj");
                    proj << "Project:\n"
                            "  Name: LightmapEvidence\n"
                            "  StartScene: \"\"\n"
                            "  AssetDirectory: \"Assets\"\n"
                            "  ScriptModulePath: \"\"\n";
                }
                ASSERT_TRUE(Project::Load(projectDir / "Evidence.oloproj"));
                auto assetManager = Ref<EditorAssetManager>::Create();
                assetManager->Initialize(false); // no file watcher in tests
                Project::SetAssetManager(assetManager);
            }

            Scene& scene = GetScene();

            // Camera INSIDE the room at (0, 1.5, 1.7), identity rotation =>
            // looking down -Z (the room's open length), 90 deg vertical FOV so
            // both walls (x = -/+3) enter the frame from 1 m inside the open
            // +Z end. SceneCamera defaults to ORTHOGRAPHIC — force perspective
            // (SceneRenderEvidenceTest's lesson). The analysis regions above
            // are derived from exactly this pose; change one, change both.
            {
                Entity camera = scene.CreateEntity("Camera");
                camera.GetComponent<TransformComponent>().Translation = { 0.0f, 1.5f, 1.7f };
                auto& cameraComp = camera.AddComponent<CameraComponent>();
                cameraComp.Primary = true;
                cameraComp.Camera.SetProjectionType(SceneCamera::ProjectionType::Perspective);
                cameraComp.Camera.SetPerspectiveVerticalFOV(glm::radians(90.0f));
            }

            // White point light in the middle of the room — the parity test's
            // photometry (intensity 10, range 14, attenuation 2). The DIRECT
            // term on the grey floor is colour-neutral by construction, which
            // is what makes any floor r/g asymmetry attributable to the bake.
            {
                Entity light = scene.CreateEntity("RoomLight");
                light.GetComponent<TransformComponent>().Translation = { 0.0f, 1.6f, 0.0f };
                auto& pointLight = light.AddComponent<PointLightComponent>();
                pointLight.m_Color = { 1.0f, 1.0f, 1.0f };
                pointLight.m_Intensity = 10.0f;
                pointLight.m_Range = 14.0f;
                pointLight.m_Attenuation = 2.0f;
            }

            // The room. Same dimensions as LightmapBakeParityTest's BleedRoom:
            // interior x in [-3, 3], y in [0, 3], z in [-2, 2], open at -/+Z.
            m_Floor = MakeRoomPiece("Floor", { 0.0f, -0.1f, 0.0f }, { 6.0f, 0.2f, 4.0f },
                                    { 0.6f, 0.6f, 0.6f });
            m_RedWall = MakeRoomPiece("RedWall", { -3.1f, 1.5f, 0.0f }, { 0.2f, 3.0f, 4.0f },
                                      { 0.75f, 0.04f, 0.04f });
            m_GreenWall = MakeRoomPiece("GreenWall", { 3.1f, 1.5f, 0.0f }, { 0.2f, 3.0f, 4.0f },
                                        { 0.04f, 0.75f, 0.04f });
            m_Ceiling = MakeRoomPiece("Ceiling", { 0.0f, 3.1f, 0.0f }, { 6.0f, 0.2f, 4.0f },
                                      { 0.6f, 0.6f, 0.6f });

            EnableRendering(kSize, kSize);
        }

        // One lightmap-static room piece with its OWN fresh MeshSource (a
        // CreateCube per entity — the unwrap mutates the MeshSource in place,
        // and per-entity sources keep each mutation local; sharing would also
        // work, the parity test proves unwrap idempotence, but per-entity is
        // the shape an editor-authored room has). The colour lives in a
        // MaterialComponent, which both the raster path and
        // ReferenceSceneBuilder::AddScene resolve as the override material.
        Entity MakeRoomPiece(const char* name, const glm::vec3& translation, const glm::vec3& scale,
                             const glm::vec3& color)
        {
            Entity entity = GetScene().CreateEntity(name);
            auto& transform = entity.GetComponent<TransformComponent>();
            transform.Translation = translation;
            transform.Scale = scale;

            Ref<Mesh> cube = MeshPrimitives::CreateCube();
            auto& mesh = entity.AddComponent<MeshComponent>(cube->GetMeshSource());
            mesh.m_LightmapStatic = true;

            auto& material = entity.AddComponent<MaterialComponent>();
            material.m_Material.SetBaseColorFactor(glm::vec4(color, 1.0f));
            material.m_Material.SetMetallicFactor(0.0f);
            material.m_Material.SetRoughnessFactor(0.9f);
            return entity;
        }

        // Read back the composite and flip to top-down. Returns false when the
        // composite framebuffer is unavailable.
        [[nodiscard]] bool CaptureTopDown(std::vector<u8>& outPx, u32& outW, u32& outH)
        {
            if (!ReadbackComposite(outPx, outW, outH))
            {
                return false;
            }
            FlipRowsInPlace(outPx, outW, outH);
            return true;
        }

        Entity m_Floor;
        Entity m_RedWall;
        Entity m_GreenWall;
        Entity m_Ceiling;
    };

    TEST_F(LightmapBleedRoom, BakedBleedTintsTheFloorAndTogglesWithEnabled)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        Scene& scene = GetScene();

        // ── Gather every lightmap-static entity — EditorLayer::BakeLightmaps'
        // exact loop ──
        std::vector<LightmapBakeInput> inputs;
        {
            auto view = scene.GetAllEntitiesWith<IDComponent, MeshComponent>();
            for (auto entity : view)
            {
                const auto& mesh = view.get<MeshComponent>(entity);
                if (!mesh.m_LightmapStatic || !mesh.m_MeshSource || mesh.m_MeshSource->GetVertices().IsEmpty())
                {
                    continue;
                }
                LightmapBakeInput input;
                input.EntityUUID = static_cast<u64>(view.get<IDComponent>(entity).ID);
                input.Mesh = mesh.m_MeshSource;
                input.WorldTransform = scene.GetWorldTransform(entity);
                inputs.push_back(std::move(input));
            }
        }
        ASSERT_EQ(inputs.size(), 4u) << "expected exactly the 4 lightmap-static room pieces";

        // ── Bake settings. The scene-level SceneLightmapSettings fields are
        // set FIRST because ComputeBakeKey hashes them — the key must be
        // computed from the same settings Resolve() will see at render time.
        // Small on purpose: ~600-800 texels x 40 spp x <=3 bounces. ──
        SceneLightmapSettings& lmSettings = scene.GetLightmapSettings();
        lmSettings.AtlasSize = 128;
        lmSettings.SamplesPerTexel = 40;
        lmSettings.MaxBounces = 3;
        lmSettings.TexelsPerMeter = 2.0f;

        LightmapBakeSettings bakeSettings;
        bakeSettings.AtlasSize = lmSettings.AtlasSize;
        bakeSettings.SamplesPerTexel = lmSettings.SamplesPerTexel;
        bakeSettings.MaxBounces = lmSettings.MaxBounces;
        bakeSettings.TexelsPerMeter = lmSettings.TexelsPerMeter;
        bakeSettings.MinRegionSize = 8;
        bakeSettings.DilationPasses = 2;
        // Unwrap parameters are no longer settings: Prepare() hard-codes the
        // shared kLightmapUnwrap* constants so the runtime's self-healing
        // re-unwrap can always reproduce the baked layout.

        // ── Stage 1: unwrap (mutates the MeshSources in place) + rasterize ──
        LightmapBakePrepared prepared;
        std::string prepareError;
        ASSERT_TRUE(LightmapBaker::Prepare(inputs, bakeSettings, prepared, prepareError)) << prepareError;
        EXPECT_EQ(prepared.BakedEntityCount, 4u);
        EXPECT_EQ(prepared.SkippedEntityCount, 0u);
        EXPECT_GT(prepared.Jobs.size(), 200u) << "the room's charts cover too little of the atlas";

        // Re-Build every unwrapped mesh (a GL context exists here) so the VAOs
        // carry the seam-split vertices and the UV2 stream the shader reads.
        // Non-const iteration on purpose: Ref<T> propagates const through
        // operator-> and MeshSource::Build() is a mutator.
        for (auto& input : inputs)
        {
            ASSERT_TRUE(input.Mesh->HasLightmapUVs()) << "Prepare left a mesh without a complete UV2 stream";
            input.Mesh->Build();
        }

        // The bake key is computed AFTER the unwrap, on purpose (the exact
        // ordering EditorLayer::BakeLightmaps uses): ComputeBakeKey hashes
        // vertex counts, the unwrap seam-splits vertices, and Resolve()
        // recomputes the key against the POST-unwrap meshes at sample time. A
        // key computed before Prepare can never match — the runtime would
        // (correctly) refuse the bake as stale.
        bakeSettings.BakeKey = SceneLightmapRuntime::ComputeBakeKey(scene, lmSettings);

        // ── The reference world, from the SAME scene via the SAME predicate
        // production uses. Lights are gathered unconditionally (the predicate
        // gates geometry only); default build options = the production bake's
        // (no environment, full material model). ──
        PathTracing::ReferenceSceneBuilder builder;
        builder.AddScene(scene, [](Entity entity)
                         { return entity.HasComponent<MeshComponent>() &&
                                  entity.GetComponent<MeshComponent>().m_LightmapStatic; });
        EXPECT_EQ(builder.GetPendingLightCount(), 1u);
        const PathTracing::ReferenceScene world = builder.Build(PathTracing::ReferenceSceneBuildOptions{});

        // ── Stage 2: the texel bake ──
        const LightmapBakeResult result = LightmapBaker::BakeTexels(prepared, world, bakeSettings);
        ASSERT_TRUE(result.Success) << result.Error;
        ASSERT_TRUE(result.Asset);
        EXPECT_TRUE(result.Asset->Validate());
        EXPECT_EQ(result.Asset->GetBakeKey(), bakeSettings.BakeKey);

        // ── Register the baked asset and resolve the runtime. IsValid() here
        // IS the staleness-gate contract: the stored key must equal what
        // Resolve() just recomputed from the live scene. ──
        const AssetHandle lightmapHandle = AssetManager::AddMemoryOnlyAsset(result.Asset);
        ASSERT_NE(static_cast<u64>(lightmapHandle), 0u) << "AddMemoryOnlyAsset returned a null handle";

        lmSettings.LightmapAsset = lightmapHandle;
        lmSettings.Enabled = true;
        lmSettings.Intensity = 1.0f;

        auto& runtime = scene.GetLightmapRuntime();
        runtime->Resolve(scene);
        ASSERT_FALSE(runtime->IsStale())
            << "STALENESS GATE MISWIRED: the just-baked asset's key ("
            << result.Asset->GetBakeKey() << ") does not match what Resolve() recomputed from the "
            << "unchanged scene. ComputeBakeKey at bake time and at resolve time disagree — check "
            << "the post-unwrap key ordering and the SceneLightmapSettings fields the key hashes.";
        ASSERT_TRUE(runtime->IsValid())
            << "SceneLightmapRuntime::Resolve rejected a freshly baked, key-matching asset — "
            << "asset lookup (AddMemoryOnlyAsset/GetAsset), Validate(), or atlas creation failed.";
        EXPECT_GT(runtime->GetScaleOffset(m_Floor.GetUUID()).x, 0.0f)
            << "the floor entity has no atlas region — the entry table is mis-keyed";

        // ── ON capture: 2 frames (frame 1 seeds prev-frame history, frame 2
        // is the stable image — the SceneRenderEvidenceTest cadence). ──
        RunFrames(2);
        std::vector<u8> onPx;
        u32 width = 0;
        u32 height = 0;
        ASSERT_TRUE(CaptureTopDown(onPx, width, height))
            << "ReadbackComposite failed — UIComposite framebuffer unavailable";
        ASSERT_EQ(width, kSize);
        ASSERT_EQ(height, kSize);
        ASSERT_EQ(onPx.size(), static_cast<std::size_t>(width) * height * 4u);

        // ── OFF capture: the user toggle, not part of the bake key. 3 frames
        // so no ON-frame history bleeds into the captured image. ──
        lmSettings.Enabled = false;
        RunFrames(3);
        std::vector<u8> offPx;
        u32 offWidth = 0;
        u32 offHeight = 0;
        ASSERT_TRUE(CaptureTopDown(offPx, offWidth, offHeight));
        ASSERT_EQ(offWidth, width);
        ASSERT_EQ(offHeight, height);
        lmSettings.Enabled = true;

        // ── Write ALL evidence PNGs before asserting anything, so a red run
        // still leaves the full frame set for review. ──
        WriteEvidencePng("Lightmap_VisualEvidence_On.png", onPx, width, height);
        WriteEvidencePng("Lightmap_VisualEvidence_Off.png", offPx, width, height);

        // Multi-angle evidence (PNGs only, no assertions): the editor render
        // path with an explicitly posed EditorCamera — RunEditorFrames drives
        // the same ProcessScene3DSharedLogic, so the lightmap uploads there
        // too. Raking: low near the open +Z end, skimming down the floor.
        // RedSide: from the green half, facing the red wall base.
        {
            const struct
            {
                const char* FileName;
                glm::vec3 Eye;
                glm::vec3 Target;
            } kAngles[] = {
                { "Lightmap_VisualEvidence_Raking.png", { 0.0f, 0.7f, 1.85f }, { 0.0f, 0.0f, -1.8f } },
                { "Lightmap_VisualEvidence_RedSide.png", { 2.3f, 1.7f, 1.5f }, { -2.7f, 0.4f, -0.9f } },
            };
            for (const auto& angle : kAngles)
            {
                EditorCamera camera(60.0f, 1.0f, 0.05f, 1000.0f);
                camera.SetViewportSize(static_cast<f32>(kSize), static_cast<f32>(kSize));
                const YawPitch yp = LookAtYawPitch(angle.Eye, angle.Target);
                camera.SetPose(angle.Eye, yp.Yaw, yp.Pitch);
                RunEditorFrames(camera, 2);

                std::vector<u8> anglePx;
                u32 angleW = 0;
                u32 angleH = 0;
                if (CaptureTopDown(anglePx, angleW, angleH))
                {
                    WriteEvidencePng(angle.FileName, anglePx, angleW, angleH);
                }
            }
        }

        // ── Analysis regions — assert they are inside the frame before use. ──
        const PixelRect redRect = MakeRect(kRedRegionX0, kRedRegionX1, kFloorRegionY0, kFloorRegionY1, width, height);
        const PixelRect greenRect =
            MakeRect(kGreenRegionX0, kGreenRegionX1, kFloorRegionY0, kFloorRegionY1, width, height);
        ASSERT_LT(redRect.X0, redRect.X1);
        ASSERT_LT(redRect.Y0, redRect.Y1);
        ASSERT_LE(redRect.X1, width);
        ASSERT_LE(greenRect.X1, width);
        ASSERT_LE(redRect.Y1, height);
        ASSERT_GT(redRect.PixelCount(), 1000u) << "analysis region too small for a stable mean";

        // ── Contract 3: basic sanity on the ON frame (SceneRenderEvidence
        // contracts — luminance in 0..1 here). The composite clears to
        // mid-grey (~86/255 = 0.34), so a dead render is not black but IS
        // flat; both checks together catch it. ──
        {
            f32 minLum = 1.0f;
            f32 maxLum = 0.0f;
            for (std::size_t i = 0; i < onPx.size(); i += 4)
            {
                const f32 l = (0.2126f * onPx[i + 0] + 0.7152f * onPx[i + 1] + 0.0722f * onPx[i + 2]) / 255.0f;
                minLum = std::min(minLum, l);
                maxLum = std::max(maxLum, l);
            }
            EXPECT_GT(maxLum - minLum, 0.05f) << "ON frame is nearly flat — the room may not have drawn";
            EXPECT_GT(maxLum, 0.10f) << "ON frame has no bright pixels — pipeline may have output black";
        }

        const RegionMeans onRed = MeanChannelsInRect(onPx, width, redRect);
        const RegionMeans onGreen = MeanChannelsInRect(onPx, width, greenRect);
        const RegionMeans offRed = MeanChannelsInRect(offPx, width, redRect);
        const RegionMeans offGreen = MeanChannelsInRect(offPx, width, greenRect);

        // ── Contract 1: the ON-frame colour bleed. The direct point light is
        // white and the floor albedo grey, so the only red/green channel
        // asymmetry available on the floor is the baked indirect term. 1.05 is
        // deliberately modest: the white direct term dilutes the wall-albedo
        // tint, and tone-mapping compresses ratios further. ──
        EXPECT_GT(onRed.R, onRed.G * 1.05f)
            << "floor beside the RED wall is not red-shifted with the bake ON: r=" << onRed.R
            << " g=" << onRed.G << " — see Lightmap_VisualEvidence_On.png";
        EXPECT_GT(onGreen.G, onGreen.R * 1.05f)
            << "floor beside the GREEN wall is not green-shifted with the bake ON: g=" << onGreen.G
            << " r=" << onGreen.R << " — see Lightmap_VisualEvidence_On.png";

        // Differential control: with the bake OFF the floor is direct-only and
        // colour-neutral, so the r/g ratio must move toward red when the bake
        // turns ON (and mirrored for green). Pins the tint to the toggle.
        EXPECT_GT(onRed.R / std::max(onRed.G, 1.0f), offRed.R / std::max(offRed.G, 1.0f))
            << "enabling the bake did not shift the red-side floor ratio toward red";
        EXPECT_GT(onGreen.G / std::max(onGreen.R, 1.0f), offGreen.G / std::max(offGreen.R, 1.0f))
            << "enabling the bake did not shift the green-side floor ratio toward green";

        // ── Contract 2: ON vs OFF. The baked indirect term must contribute
        // measurable light. 3.0 grey levels clears the ~2-3 level cross-test
        // TAA/exposure residue (light-path-photometric-parity rule 4) — and
        // within THIS test the two captures share all process state, so a real
        // bake lands far above it. ──
        const f32 meanAbsDiff =
            0.5f * (MeanAbsDiffInRect(onPx, offPx, width, redRect) + MeanAbsDiffInRect(onPx, offPx, width, greenRect));
        EXPECT_GT(meanAbsDiff, 3.0f)
            << "ON and OFF frames barely differ over the floor (" << meanAbsDiff
            << " grey levels) — the baked term contributes nothing; compare the On/Off PNGs";

        // Indirect light only ADDS: ON must not be darker than OFF.
        const f32 onFloorLum = 0.5f * (onRed.Luminance() + onGreen.Luminance());
        const f32 offFloorLum = 0.5f * (offRed.Luminance() + offGreen.Luminance());
        EXPECT_GT(onFloorLum, offFloorLum)
            << "the bake made the floor DARKER (on=" << onFloorLum << ", off=" << offFloorLum
            << ") — the lightmap term must only add light";

        // The ADDED light itself carries the wall albedo: the ON-minus-OFF
        // gain beside the red wall must be red-shifted (and mirrored). This is
        // the sharpest signature — the white direct term cancels exactly in
        // the difference. 0.5 grey levels of margin absorbs quantisation.
        EXPECT_GT((onRed.R - offRed.R), (onRed.G - offRed.G) + 0.5f)
            << "the light ADDED beside the RED wall is not red-shifted: dR=" << (onRed.R - offRed.R)
            << " dG=" << (onRed.G - offRed.G);
        EXPECT_GT((onGreen.G - offGreen.G), (onGreen.R - offGreen.R) + 0.5f)
            << "the light ADDED beside the GREEN wall is not green-shifted: dG=" << (onGreen.G - offGreen.G)
            << " dR=" << (onGreen.R - offGreen.R);
    }
} // namespace OloEngine::Tests

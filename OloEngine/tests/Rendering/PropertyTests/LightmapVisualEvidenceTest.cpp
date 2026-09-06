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
#include "OloEngine/Renderer/LightmapPageEncoding.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/PathTracing/ReferenceSceneBuilder.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Renderer/Instancing/InstancedMeshComponent.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshRegistry.h"
#include "OloEngine/Scene/SceneLightmap.h"
#include "OloEngine/Scene/SceneLightmapGather.h"

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

        // Bake-layout knobs the multi-page case (issue #868) needs to vary.
        // Defaults reproduce the original single-page bake exactly, so the two
        // pre-existing tests are unaffected.
        //
        // Declared HERE rather than nested in the fixture: a defaulted
        // `const BakeLayout&` parameter inside the class body would need the
        // nested type's member initializers before the enclosing class is
        // complete, which clang rejects.
        struct BakeLayout
        {
            u32 AtlasSize = 128;
            u32 MinRegionSize = 8;
            u32 MaxAtlasPages = kMaxLightmapPages;
        };
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
            if (!Project::GetActive() || !Project::HasAssetManager())
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
            BuildFloor();
            m_RedWall = MakeRoomPiece("RedWall", { -3.1f, 1.5f, 0.0f }, { 0.2f, 3.0f, 4.0f },
                                      { 0.75f, 0.04f, 0.04f });
            m_GreenWall = MakeRoomPiece("GreenWall", { 3.1f, 1.5f, 0.0f }, { 0.2f, 3.0f, 4.0f },
                                        { 0.04f, 0.75f, 0.04f });
            m_Ceiling = MakeRoomPiece("Ceiling", { 0.0f, 3.1f, 0.0f }, { 6.0f, 0.2f, 4.0f },
                                      { 0.6f, 0.6f, 0.6f });

            EnableRendering(kSize, kSize);
        }

        // The room's floor. Overridden by the instanced variant below; the
        // walls and ceiling stay MeshComponents either way, so the ONLY
        // difference between the two fixtures is how the floor is drawn.
        virtual void BuildFloor()
        {
            m_Floor = MakeRoomPiece("Floor", { 0.0f, -0.1f, 0.0f }, { 6.0f, 0.2f, 4.0f },
                                    { 0.6f, 0.6f, 0.6f });
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

        // Bake the room, register the asset, point the scene at it and resolve
        // the runtime — EditorLayer::BakeLightmaps step for step. Shared by both
        // tests in this file so the deferred-path case cannot drift into baking
        // something subtly different from the forward case it is compared against.
        // ASSERT_* inside a void helper only returns from the HELPER, so callers
        // must wrap it in ASSERT_NO_FATAL_FAILURE.
        void BakeAndResolve(const BakeLayout& layout = {})
        {
            Scene& scene = GetScene();

            // ── Gather every lightmap-static RECEIVER through the production
            // walk (issue #867). Calling GatherLightmapReceivers rather than
            // re-writing EditorLayer::BakeLightmaps' loop is the point: this
            // suite is the only thing that renders the whole chain, so it should
            // render the code the editor actually runs. ──
            const std::vector<LightmapReceiver> receivers = GatherLightmapReceivers(scene);
            std::vector<LightmapBakeInput> inputs;
            inputs.reserve(receivers.size());
            for (const LightmapReceiver& receiver : receivers)
            {
                LightmapBakeInput input;
                input.EntityUUID = static_cast<u64>(receiver.EntityUUID);
                input.SubKey = receiver.SubKey;
                input.Mesh = receiver.Mesh;
                input.WorldTransform = receiver.WorldTransform;
                inputs.push_back(std::move(input));
            }
            ASSERT_EQ(inputs.size(), ExpectedReceiverCount())
                << "the gather did not produce the receiver set this room is built from";

            // ── Bake settings. The scene-level SceneLightmapSettings fields are
            // set FIRST because ComputeBakeKey hashes them — the key must be
            // computed from the same settings Resolve() will see at render time.
            // Small on purpose: ~600-800 texels x 40 spp x <=3 bounces. ──
            SceneLightmapSettings& lmSettings = scene.GetLightmapSettings();
            lmSettings.AtlasSize = layout.AtlasSize;
            lmSettings.SamplesPerTexel = 40;
            lmSettings.MaxBounces = 3;
            lmSettings.TexelsPerMeter = 2.0f;

            LightmapBakeSettings bakeSettings;
            bakeSettings.AtlasSize = lmSettings.AtlasSize;
            bakeSettings.SamplesPerTexel = lmSettings.SamplesPerTexel;
            bakeSettings.MaxBounces = lmSettings.MaxBounces;
            bakeSettings.TexelsPerMeter = lmSettings.TexelsPerMeter;
            bakeSettings.MinRegionSize = layout.MinRegionSize;
            bakeSettings.MaxAtlasPages = layout.MaxAtlasPages;
            bakeSettings.DilationPasses = 2;
            // Unwrap parameters are no longer settings: Prepare() hard-codes the
            // shared kLightmapUnwrap* constants so the runtime's self-healing
            // re-unwrap can always reproduce the baked layout.

            // ── Stage 1: unwrap (mutates the MeshSources in place) + rasterize ──
            LightmapBakePrepared prepared;
            std::string prepareError;
            ASSERT_TRUE(LightmapBaker::Prepare(inputs, bakeSettings, prepared, prepareError)) << prepareError;
            EXPECT_EQ(prepared.BakedEntityCount, ExpectedReceiverCount());
            EXPECT_EQ(prepared.SkippedEntityCount, 0u);
            EXPECT_GT(prepared.Jobs.size(), 200u) << "the room's charts cover too little of the atlas";
            m_BakedPageCount = prepared.PageCount;
            m_BakedEntries = prepared.Entries;

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
            bakeSettings.BakeKey = SceneLightmapRuntime::ComputeBakeKey(scene, lmSettings, receivers);

            // ── The reference world, from the SAME receiver list the bake is
            // about to consume (issue #867). AddScene's MeshComponent predicate
            // could not serve an instanced receiver at all: those surfaces have
            // no MeshComponent, so the bake would have traced a room they were
            // missing from — no occlusion, no bounce, and no error to say so. ──
            PathTracing::ReferenceSceneBuilder builder;
            builder.AddLightmapReceivers(scene, receivers);
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
            EXPECT_TRUE(runtime->HasAnyRegionForEntity(m_Floor.GetUUID()))
                << "the floor entity has no atlas region — the entry table is mis-keyed";
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

        // How many receivers this room's scene is built from. The instanced
        // variant below overrides it: its floor is ONE entity drawing N
        // instances, and each instance is its own receiver.
        [[nodiscard]] virtual u32 ExpectedReceiverCount() const
        {
            return 4u; // floor, red wall, green wall, ceiling
        }

        Entity m_Floor;
        Entity m_RedWall;
        Entity m_GreenWall;
        Entity m_Ceiling;

        // What the last BakeAndResolve() actually packed (issue #868).
        u32 m_BakedPageCount = 0;
        std::vector<LightmapEntityEntry> m_BakedEntries;
    };

    TEST_F(LightmapBleedRoom, BakedBleedTintsTheFloorAndTogglesWithEnabled)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        ASSERT_NO_FATAL_FAILURE(BakeAndResolve());

        Scene& scene = GetScene();
        SceneLightmapSettings& lmSettings = scene.GetLightmapSettings();

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
    // =========================================================================
    // Issue #865 — the deferred path samples the same bake as forward.
    //
    // Before #865 a scene on RenderingPath::Deferred resolved its bake and then
    // never sampled it: the deferred lighting pass shades from the G-Buffer, and
    // by then the fragment had neither UV2 nor instance identity, so the whole
    // feature was a scene-wide no-op (not a per-draw fallback). The fix moves the
    // atlas fetch into the G-Buffer pass — the last stage that still holds both —
    // and carries the resulting irradiance + coverage in G-Buffer RT5, which
    // DeferredLightingShared.glsl consumes at the same ambient-ladder rung
    // AmbientLadder.glsl uses on the forward path.
    //
    // So this test asks the two questions that fix has to answer, in the frame:
    //   A. Is the bake VISIBLE on deferred at all? (deferred ON vs deferred OFF)
    //   B. Does it deliver the SAME irradiance as forward? (region means, not
    //      per-pixel — the two paths run different shaders, shade at different
    //      points in the frame, and are not expected to be bit-identical.)
    //
    // Question A is the regression guard: it fails loudly if the RT5 chain breaks
    // anywhere (UV2 attribute, atlas fetch, target write, sampler bind, rung).
    // Question B is the correctness one — a deferred path that samples SOMETHING
    // passes A while shading from the wrong atlas address.
    // =========================================================================
    TEST_F(LightmapBleedRoom, BakedBleedSurvivesTheDeferredPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        ASSERT_NO_FATAL_FAILURE(BakeAndResolve());

        Scene& scene = GetScene();
        SceneLightmapSettings& lmSettings = scene.GetLightmapSettings();

        // The render path is process-global state on Renderer3D, so restore
        // whatever this process had before — a leaked Deferred would silently
        // re-target every later test in the binary.
        const RenderingPath originalPath = Renderer3D::GetRendererSettings().Path;
        struct PathRestore
        {
            RenderingPath Value;
            ~PathRestore()
            {
                Renderer3D::GetRendererSettings().Path = Value;
                Renderer3D::ApplyRendererSettings();
            }
        } const pathRestore{ originalPath };

        u32 width = 0;
        u32 height = 0;
        const auto capture = [&](RenderingPath path, bool bakeEnabled, const char* fileName,
                                 std::vector<u8>& out)
        {
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::ApplyRendererSettings();
            lmSettings.Enabled = bakeEnabled;
            // 3 frames: the first seeds prev-frame history, and a path switch
            // rebuilds the frame graph, so give the temporal state a frame to
            // settle before the captured one (the same cadence the ON/OFF toggle
            // in the sibling test uses).
            RunFrames(3);
            u32 w = 0;
            u32 h = 0;
            ASSERT_TRUE(CaptureTopDown(out, w, h)) << "ReadbackComposite failed for " << fileName;
            if (width == 0)
            {
                width = w;
                height = h;
            }
            ASSERT_EQ(w, width);
            ASSERT_EQ(h, height);
            // Same three guards the forward test applies to its captures. The
            // buffer-size one is not belt-and-braces: MeanChannelsInRect and
            // MeanAbsDiffInRect index the vector raw, so a short readback is a
            // heap over-read rather than a failed assertion. The kSize pair
            // matters because the analysis rectangles at the top of this file are
            // derived from one specific camera pose AT ASPECT 1 — a differently
            // shaped capture would still produce in-range rects, and would
            // silently measure the wrong patch of floor.
            ASSERT_EQ(w, kSize);
            ASSERT_EQ(h, kSize);
            ASSERT_EQ(out.size(), static_cast<std::size_t>(w) * h * 4u);
            WriteEvidencePng(fileName, out, w, h);
        };

        std::vector<u8> forwardOn;
        std::vector<u8> deferredOn;
        std::vector<u8> deferredOff;
        capture(RenderingPath::Forward, true, "Lightmap_PathParity_ForwardOn.png", forwardOn);
        if (::testing::Test::HasFatalFailure())
            return;
        capture(RenderingPath::Deferred, true, "Lightmap_PathParity_DeferredOn.png", deferredOn);
        if (::testing::Test::HasFatalFailure())
            return;
        capture(RenderingPath::Deferred, false, "Lightmap_PathParity_DeferredOff.png", deferredOff);
        if (::testing::Test::HasFatalFailure())
            return;
        lmSettings.Enabled = true;

        const PixelRect redRect = MakeRect(kRedRegionX0, kRedRegionX1, kFloorRegionY0, kFloorRegionY1, width, height);
        const PixelRect greenRect =
            MakeRect(kGreenRegionX0, kGreenRegionX1, kFloorRegionY0, kFloorRegionY1, width, height);
        ASSERT_LT(redRect.X0, redRect.X1);
        ASSERT_LT(redRect.Y0, redRect.Y1);
        ASSERT_LE(redRect.X1, width);
        ASSERT_LE(greenRect.X1, width);
        ASSERT_LE(redRect.Y1, height);
        ASSERT_GT(redRect.PixelCount(), 1000u) << "analysis region too small for a stable mean";

        const RegionMeans fwdRed = MeanChannelsInRect(forwardOn, width, redRect);
        const RegionMeans fwdGreen = MeanChannelsInRect(forwardOn, width, greenRect);
        const RegionMeans defRed = MeanChannelsInRect(deferredOn, width, redRect);
        const RegionMeans defGreen = MeanChannelsInRect(deferredOn, width, greenRect);
        const RegionMeans defOffRed = MeanChannelsInRect(deferredOff, width, redRect);
        const RegionMeans defOffGreen = MeanChannelsInRect(deferredOff, width, greenRect);

        // ── A. The bake is visible on deferred. Same three signatures the
        // forward test asserts, re-derived on the deferred frames: the direct
        // light is white and the floor albedo grey, so any red/green asymmetry
        // on the floor — and any ON-vs-OFF difference at all — can only be the
        // baked indirect term. Before #865 every one of these was flat. ──
        EXPECT_GT(defRed.R, defRed.G * 1.05f)
            << "DEFERRED floor beside the RED wall is not red-shifted: r=" << defRed.R << " g=" << defRed.G
            << " — the deferred path is not sampling the bake (see Lightmap_PathParity_DeferredOn.png)";
        EXPECT_GT(defGreen.G, defGreen.R * 1.05f)
            << "DEFERRED floor beside the GREEN wall is not green-shifted: g=" << defGreen.G
            << " r=" << defGreen.R;

        const f32 deferredToggleDiff =
            0.5f * (MeanAbsDiffInRect(deferredOn, deferredOff, width, redRect) +
                    MeanAbsDiffInRect(deferredOn, deferredOff, width, greenRect));
        EXPECT_GT(deferredToggleDiff, 3.0f)
            << "the lightmap toggle changes nothing on the DEFERRED path (" << deferredToggleDiff
            << " grey levels) — this is exactly the issue #865 no-op the RT5 chain exists to fix";

        EXPECT_GT((defRed.R - defOffRed.R), (defRed.G - defOffRed.G) + 0.5f)
            << "the light the bake ADDS beside the RED wall on deferred is not red-shifted: dR="
            << (defRed.R - defOffRed.R) << " dG=" << (defRed.G - defOffRed.G);
        EXPECT_GT((defGreen.G - defOffGreen.G), (defGreen.R - defOffGreen.R) + 0.5f)
            << "the light the bake ADDS beside the GREEN wall on deferred is not green-shifted: dG="
            << (defGreen.G - defOffGreen.G) << " dR=" << (defGreen.R - defOffGreen.R);

        // ── B. Forward and deferred agree. REGION MEANS, per the issue's
        // acceptance criterion — the two paths are different shaders over
        // different intermediates and will never be per-pixel equal. The bar is
        // on the floor strips where the baked term is the thing under test.
        //
        // 12 grey levels is deliberately loose: the paths differ in ways that
        // have nothing to do with the bake (deferred normals are octahedrally
        // encoded and round-trip through RGBA16F, AO and specular composite at
        // different points, and the same frame on the two paths already differs
        // for un-lightmapped scenes). What it is tight enough to catch is the
        // failure that matters — a deferred path sampling the WRONG atlas
        // address, or not sampling at all: the ON/OFF gap measured above is the
        // size of the signal, and a dropped or mis-addressed lightmap moves these
        // means by that much or more.
        constexpr f32 kMaxPathMeanDelta = 12.0f;
        const f32 redLumDelta = std::abs(fwdRed.Luminance() - defRed.Luminance());
        const f32 greenLumDelta = std::abs(fwdGreen.Luminance() - defGreen.Luminance());
        EXPECT_LT(redLumDelta, kMaxPathMeanDelta)
            << "forward and deferred disagree on the red-side floor luminance (forward="
            << fwdRed.Luminance() << ", deferred=" << defRed.Luminance()
            << ") — compare Lightmap_PathParity_ForwardOn.png / _DeferredOn.png";
        EXPECT_LT(greenLumDelta, kMaxPathMeanDelta)
            << "forward and deferred disagree on the green-side floor luminance (forward="
            << fwdGreen.Luminance() << ", deferred=" << defGreen.Luminance() << ")";

        // The COLOUR of the baked bleed must match too, not just its magnitude:
        // a wrong atlas address can land on a neighbouring chart and still carry
        // a plausible amount of light, but it will not carry the same hue as the
        // wall it sits next to. Compare the r/g ratio, which cancels exposure.
        const f32 fwdRedRatio = fwdRed.R / std::max(fwdRed.G, 1.0f);
        const f32 defRedRatio = defRed.R / std::max(defRed.G, 1.0f);
        const f32 fwdGreenRatio = fwdGreen.G / std::max(fwdGreen.R, 1.0f);
        const f32 defGreenRatio = defGreen.G / std::max(defGreen.R, 1.0f);
        EXPECT_NEAR(defRedRatio, fwdRedRatio, 0.10f)
            << "the deferred red-side bleed has a different hue than forward's (forward r/g=" << fwdRedRatio
            << ", deferred r/g=" << defRedRatio
            << ") — suspect the atlas address (scale/offset or UV2), not the ladder";
        EXPECT_NEAR(defGreenRatio, fwdGreenRatio, 0.10f)
            << "the deferred green-side bleed has a different hue than forward's (forward g/r=" << fwdGreenRatio
            << ", deferred g/r=" << defGreenRatio << ")";
    }

    // =========================================================================
    // Issue #868 — per-entity GI stays correct when the atlas SPANS PAGES.
    //
    // The acceptance criterion is "a static scene too large for one page bakes
    // across pages ... and renders correct per-entity GI (region means)". The
    // room is forced onto four pages by shrinking the atlas to exactly one
    // region: AtlasSize == MinRegionSize == 16 means a page holds precisely one
    // entity, so each of the four room pieces lands on its OWN page. The baked
    // texel density is unchanged from the sibling tests (they use a 16px region
    // inside a 128px atlas), so the same colour-bleed contracts apply verbatim
    // — which is the point: paging must be invisible in the frame.
    //
    // This is the test that would fail if the page index were dropped anywhere
    // between the entry table and the sampler. A lost page does not error; it
    // reads page 0 — the FLOOR's charts — for the walls and the ceiling too, so
    // the floor's own strips would keep some bleed while the differential ON/OFF
    // signature collapses. Both are asserted.
    // =========================================================================
    TEST_F(LightmapBleedRoom, BakedBleedSurvivesAMultiPageAtlas)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // AtlasSize == MinRegionSize: one region per page, four entities, so the
        // packer must open four pages. MaxAtlasPages is left at the default so
        // the page count proves LAZY creation (only what the scene needed), not
        // just "the budget was honoured".
        BakeLayout layout;
        layout.AtlasSize = 16;
        layout.MinRegionSize = 16;
        ASSERT_NO_FATAL_FAILURE(BakeAndResolve(layout));

        Scene& scene = GetScene();
        SceneLightmapSettings& lmSettings = scene.GetLightmapSettings();

        // ── The layout itself: four pages, one entity each, every entry inside
        // the reported page count. ──
        ASSERT_EQ(m_BakedPageCount, 4u)
            << "a 16px atlas with a 16px minimum region holds exactly one entity per page — "
               "the packer did not open one page per room piece";
        ASSERT_EQ(m_BakedEntries.size(), 4u);
        std::vector<u32> pages;
        for (const LightmapEntityEntry& entry : m_BakedEntries)
        {
            EXPECT_LT(entry.Page, m_BakedPageCount);
            pages.push_back(entry.Page);
        }
        std::sort(pages.begin(), pages.end());
        EXPECT_EQ(std::adjacent_find(pages.begin(), pages.end()), pages.end())
            << "two entities share a page although each page holds exactly one region";

        auto& runtime = scene.GetLightmapRuntime();
        EXPECT_EQ(runtime->GetPageCount(), 4u);

        // The runtime must hand every entity its own page back through the
        // ENCODED region — this is the CPU-side half of what the shader decodes.
        const Entity roomPieces[] = { m_Floor, m_RedWall, m_GreenWall, m_Ceiling };
        std::vector<u32> encodedPages;
        for (const Entity& piece : roomPieces)
        {
            const glm::vec4 region = runtime->GetScaleOffset(piece.GetUUID());
            ASSERT_GT(region.x, 0.0f) << "a room piece lost its atlas region";
            encodedPages.push_back(DecodeLightmapPage(region));
        }
        std::sort(encodedPages.begin(), encodedPages.end());
        EXPECT_EQ(std::adjacent_find(encodedPages.begin(), encodedPages.end()), encodedPages.end())
            << "the encoded per-draw regions collapsed two entities onto one page — the page index "
               "was lost between the entry table and InstanceData::LightmapScaleOffset";

        // ── The frame. Same cadence and same contracts as the forward test. ──
        RunFrames(2);
        std::vector<u8> onPx;
        u32 width = 0;
        u32 height = 0;
        ASSERT_TRUE(CaptureTopDown(onPx, width, height))
            << "ReadbackComposite failed — UIComposite framebuffer unavailable";
        ASSERT_EQ(width, kSize);
        ASSERT_EQ(height, kSize);
        ASSERT_EQ(onPx.size(), static_cast<std::size_t>(width) * height * 4u);

        lmSettings.Enabled = false;
        RunFrames(3);
        std::vector<u8> offPx;
        u32 offWidth = 0;
        u32 offHeight = 0;
        ASSERT_TRUE(CaptureTopDown(offPx, offWidth, offHeight));
        ASSERT_EQ(offWidth, width);
        ASSERT_EQ(offHeight, height);
        lmSettings.Enabled = true;

        WriteEvidencePng("Lightmap_MultiPage_On.png", onPx, width, height);
        WriteEvidencePng("Lightmap_MultiPage_Off.png", offPx, width, height);

        const PixelRect redRect = MakeRect(kRedRegionX0, kRedRegionX1, kFloorRegionY0, kFloorRegionY1, width, height);
        const PixelRect greenRect =
            MakeRect(kGreenRegionX0, kGreenRegionX1, kFloorRegionY0, kFloorRegionY1, width, height);
        ASSERT_LT(redRect.X0, redRect.X1);
        ASSERT_LT(redRect.Y0, redRect.Y1);
        ASSERT_LE(redRect.X1, width);
        ASSERT_LE(greenRect.X1, width);
        ASSERT_LE(redRect.Y1, height);

        const RegionMeans onRed = MeanChannelsInRect(onPx, width, redRect);
        const RegionMeans onGreen = MeanChannelsInRect(onPx, width, greenRect);
        const RegionMeans offRed = MeanChannelsInRect(offPx, width, redRect);
        const RegionMeans offGreen = MeanChannelsInRect(offPx, width, greenRect);

        // Per-entity correctness: the floor still picks up the wall NEXT TO IT.
        // The walls are on different pages from the floor now, so a bleed that
        // still carries the right hue is evidence the bounce came from the right
        // geometry AND that the floor's own page was addressed correctly.
        EXPECT_GT(onRed.R, onRed.G * 1.05f)
            << "multi-page: floor beside the RED wall is not red-shifted: r=" << onRed.R << " g=" << onRed.G
            << " — see Lightmap_MultiPage_On.png";
        EXPECT_GT(onGreen.G, onGreen.R * 1.05f)
            << "multi-page: floor beside the GREEN wall is not green-shifted: g=" << onGreen.G
            << " r=" << onGreen.R;

        const f32 meanAbsDiff =
            0.5f * (MeanAbsDiffInRect(onPx, offPx, width, redRect) + MeanAbsDiffInRect(onPx, offPx, width, greenRect));
        EXPECT_GT(meanAbsDiff, 3.0f)
            << "multi-page: ON and OFF frames barely differ over the floor (" << meanAbsDiff
            << " grey levels) — the paged atlas contributes nothing";

        EXPECT_GT((onRed.R - offRed.R), (onRed.G - offRed.G) + 0.5f)
            << "multi-page: the light ADDED beside the RED wall is not red-shifted: dR=" << (onRed.R - offRed.R)
            << " dG=" << (onRed.G - offRed.G);
        EXPECT_GT((onGreen.G - offGreen.G), (onGreen.R - offGreen.R) + 0.5f)
            << "multi-page: the light ADDED beside the GREEN wall is not green-shifted: dG="
            << (onGreen.G - offGreen.G) << " dR=" << (onGreen.R - offGreen.R);
    }

    // -------------------------------------------------------------------------
    // LightmapInstancedBleedRoom — the SAME room, with the floor drawn as an
    // InstancedMeshComponent (issue #867).
    //
    // The floor becomes ONE entity holding TWO world-space instances: a left
    // tile hugging the red wall and a right tile hugging the green wall. Walls,
    // ceiling, light and camera are untouched, so this fixture differs from
    // LightmapBleedRoom in exactly one thing — how the floor is submitted.
    //
    // That is what makes the hue assertion decisive rather than merely
    // encouraging. Under the pre-#867 model the whole component was one entity
    // and could carry at most ONE region, so both tiles would have sampled the
    // same charts and come out the same colour. Seeing the left tile red-shifted
    // AND the right tile green-shifted, in one frame, is only possible if each
    // instance received and addressed a region of its own — which is the entire
    // claim of this receiver.
    //
    // Evidence PNGs (written BEFORE any assertion, so a red run still leaves the
    // frames behind), under OloEditor/assets/tests/visual/:
    //   Lightmap_Instanced_On.png   — bake enabled
    //   Lightmap_Instanced_Off.png  — same pose, SceneLightmapSettings::Enabled = false
    // -------------------------------------------------------------------------
    class LightmapInstancedBleedRoom : public LightmapBleedRoom
    {
      protected:
        // Two instances, one region each.
        [[nodiscard]] u32 ExpectedReceiverCount() const override
        {
            return 5u; // 2 floor instances + red wall + green wall + ceiling
        }

        void BuildFloor() override
        {
            Scene& scene = GetScene();
            m_Floor = scene.CreateEntity("FloorInstances");

            auto& imc = m_Floor.AddComponent<InstancedMeshComponent>();
            Ref<Mesh> cube = MeshPrimitives::CreateCube();
            ASSERT_TRUE(cube);
            imc.MeshSource = cube->GetMeshSource();
            imc.LightmapStatic = true;

            // The floor's two halves. Instances are WORLD-space — the entity's
            // own TransformComponent is deliberately not applied on top — so the
            // pair tiles exactly the x in [-3, 3] the MeshComponent floor covers,
            // and the analysis rects derived for that floor stay valid verbatim.
            const auto tile = [](f32 centreX, u64 stableID)
            {
                InstanceData instance;
                instance.Transform = glm::translate(glm::mat4(1.0f), glm::vec3(centreX, -0.1f, 0.0f)) *
                                     glm::scale(glm::mat4(1.0f), glm::vec3(3.0f, 0.2f, 4.0f));
                instance.StableID = stableID;
                return instance;
            };
            imc.Instances.push_back(tile(-1.5f, kLeftTileStableID)); // beside the RED wall
            imc.Instances.push_back(tile(1.5f, kRightTileStableID)); // beside the GREEN wall

            // The grey the MeshComponent floor uses, so the two rooms are
            // photometrically comparable.
            auto& material = m_Floor.AddComponent<MaterialComponent>();
            material.m_Material.SetBaseColorFactor(glm::vec4(0.6f, 0.6f, 0.6f, 1.0f));
            material.m_Material.SetMetallicFactor(0.0f);
            material.m_Material.SetRoughnessFactor(0.9f);
        }

        static constexpr u64 kLeftTileStableID = 11;
        static constexpr u64 kRightTileStableID = 22;
    };

    TEST_F(LightmapInstancedBleedRoom, EachInstanceReceivesItsOwnBakedRegion)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        ASSERT_NO_FATAL_FAILURE(BakeAndResolve());

        Scene& scene = GetScene();
        auto& runtime = scene.GetLightmapRuntime();

        // ── The CPU half of the claim, before a pixel is read. Two distinct,
        // non-sentinel regions under one entity UUID is the thing the sub-key
        // exists to make possible, and asserting it here means a red pixel test
        // below can be read as "the shader mis-addressed it" rather than "maybe
        // nothing was baked at all". ──
        const UUID floorUUID = m_Floor.GetUUID();
        const glm::vec4 leftRegion = runtime->GetScaleOffset(floorUUID, kLeftTileStableID);
        const glm::vec4 rightRegion = runtime->GetScaleOffset(floorUUID, kRightTileStableID);
        ASSERT_GT(leftRegion.x, 0.0f) << "the left floor instance has no atlas region";
        ASSERT_GT(rightRegion.x, 0.0f) << "the right floor instance has no atlas region";
        EXPECT_NE(0, std::memcmp(&leftRegion, &rightRegion, sizeof(glm::vec4)))
            << "both instances share one region — the sub-key is not reaching the entry table, so "
               "every instance of a batch would shade from the first one's charts";
        // And the "whole entity" key must MISS: nothing bakes a region there for
        // an instanced component, and serving one would mean the draw could pick
        // up a region no instance owns.
        EXPECT_FLOAT_EQ(runtime->GetScaleOffset(floorUUID, 0).x, 0.0f);

        // ── The frame. ──
        std::vector<u8> onPx;
        u32 width = 0;
        u32 height = 0;
        // 2 frames: the first seeds prev-frame history, the second is the
        // stable image (the SceneRenderEvidenceTest cadence the tests above use).
        RunFrames(2);
        ASSERT_TRUE(CaptureTopDown(onPx, width, height))
            << "ReadbackComposite failed — UIComposite framebuffer unavailable";
        ASSERT_EQ(width, kSize);
        ASSERT_EQ(height, kSize);
        WriteEvidencePng("Lightmap_Instanced_On.png", onPx, width, height);

        scene.GetLightmapSettings().Enabled = false;
        std::vector<u8> offPx;
        u32 offW = 0;
        u32 offH = 0;
        // 3 frames so no ON-frame history bleeds into the captured image.
        RunFrames(3);
        ASSERT_TRUE(CaptureTopDown(offPx, offW, offH));
        WriteEvidencePng("Lightmap_Instanced_Off.png", offPx, offW, offH);
        ASSERT_EQ(offW, width);
        ASSERT_EQ(offH, height);
        scene.GetLightmapSettings().Enabled = true;

        const PixelRect redRect = MakeRect(kRedRegionX0, kRedRegionX1, kFloorRegionY0, kFloorRegionY1, width, height);
        const PixelRect greenRect =
            MakeRect(kGreenRegionX0, kGreenRegionX1, kFloorRegionY0, kFloorRegionY1, width, height);
        ASSERT_GT(redRect.PixelCount(), 0u);
        ASSERT_GT(greenRect.PixelCount(), 0u);

        const RegionMeans onRed = MeanChannelsInRect(onPx, width, redRect);
        const RegionMeans onGreen = MeanChannelsInRect(onPx, width, greenRect);
        const RegionMeans offRed = MeanChannelsInRect(offPx, width, redRect);
        const RegionMeans offGreen = MeanChannelsInRect(offPx, width, greenRect);

        // THE contract. Two instances of one entity, two different hues, in one
        // frame — impossible under the one-region-per-entity model this issue
        // replaced. The direct light is white, so channel asymmetry on a grey
        // floor can only have come from the baked indirect term.
        EXPECT_GT(onRed.R, onRed.G * 1.05f)
            << "instanced: the tile beside the RED wall is not red-shifted: r=" << onRed.R << " g=" << onRed.G
            << " — see Lightmap_Instanced_On.png";
        EXPECT_GT(onGreen.G, onGreen.R * 1.05f)
            << "instanced: the tile beside the GREEN wall is not green-shifted: g=" << onGreen.G
            << " r=" << onGreen.R << " — see Lightmap_Instanced_On.png";

        // ON vs OFF, the "is the chain connected at all" half. A missed path on
        // this receiver looks EXACTLY like the old probe/IBL fallback, so the
        // differential is the only thing that can tell them apart.
        const f32 meanAbsDiff =
            0.5f * (MeanAbsDiffInRect(onPx, offPx, width, redRect) + MeanAbsDiffInRect(onPx, offPx, width, greenRect));
        EXPECT_GT(meanAbsDiff, 3.0f)
            << "instanced: ON and OFF frames barely differ over the floor (" << meanAbsDiff
            << " grey levels) — the instance buffer's LightmapScaleOffset lane never reached the shader";

        EXPECT_GE(onRed.Luminance(), offRed.Luminance())
            << "instanced: baked indirect must only ADD light";

        EXPECT_GT((onRed.R - offRed.R), (onRed.G - offRed.G) + 0.5f)
            << "instanced: the light ADDED beside the RED wall is not red-shifted: dR=" << (onRed.R - offRed.R)
            << " dG=" << (onRed.G - offRed.G);
        EXPECT_GT((onGreen.G - offGreen.G), (onGreen.R - offGreen.R) + 0.5f)
            << "instanced: the light ADDED beside the GREEN wall is not green-shifted: dG="
            << (onGreen.G - offGreen.G) << " dR=" << (onGreen.R - offGreen.R);

        // ── Un-marking the batch must CLEAR the lanes, not merely stop
        // refreshing them ──
        //
        // These regions live in the component's own instance storage, so once
        // written they persist. If the fill only ran while the flag was set, an
        // un-ticked batch would keep sampling the rects it was baked into — and
        // the next bake, which repacks the atlas, would have it shading from
        // whatever surface owns them then. That is a wrong-address bug that
        // renders as a plausible patch of light, and nothing else in the
        // pipeline would catch it: the region is structurally valid, just not
        // this batch's any more.
        {
            auto& imc = m_Floor.GetComponent<InstancedMeshComponent>();
            ASSERT_FALSE(imc.Instances.empty());
            ASSERT_GT(imc.Instances[0].LightmapScaleOffset.x, 0.0f)
                << "the draw never wrote a region into the instance buffer at all";

            imc.LightmapStatic = false;
            RunFrames(2);
            for (const InstanceData& instance : imc.Instances)
            {
                EXPECT_FLOAT_EQ(instance.LightmapScaleOffset.x, 0.0f)
                    << "un-marking Lightmap Static left a stale atlas region in the instance buffer";
            }

            // And ticking it back on restores them, so the clear is a real
            // convergence rather than a one-way door.
            imc.LightmapStatic = true;
            RunFrames(2);
            EXPECT_GT(imc.Instances[0].LightmapScaleOffset.x, 0.0f)
                << "re-marking Lightmap Static did not restore the batch's regions";
        }
    }

    TEST_F(LightmapInstancedBleedRoom, PerInstanceRegionsSurviveTheDeferredPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        ASSERT_NO_FATAL_FAILURE(BakeAndResolve());

        Scene& scene = GetScene();
        SceneLightmapSettings& lmSettings = scene.GetLightmapSettings();

        // Two proven halves are not a proven whole, and this repo has the scars
        // to prove it (issue #629). The classic room already pins that RT5
        // carries baked irradiance through the deferred path, and the forward
        // test above pins that each instance addresses its own region — but the
        // INTERSECTION is its own chain: the G-Buffer shader reads the region
        // from `instances[gl_InstanceIndex]`, and on an instanced draw that
        // index is a real per-instance lookup rather than the constant 0 a
        // single-instance draw resolves to. Nothing else in the suite exercises
        // that.
        // Renderer3D settings are PROCESS-GLOBAL. Restore whatever this process
        // had before, or a leaked Deferred / disabled-VG state silently
        // re-targets every later test in the binary — the same hazard the
        // deferred-path test above documents, and the reason it keeps a
        // PathRestore guard. The virtual test needs both fields, and it must
        // restore the value it OBSERVED rather than a literal: the defaults are
        // not this test's to assert.
        struct SettingsRestore
        {
            RenderingPath Path;
            bool VirtualGeometry;
            ~SettingsRestore()
            {
                auto& restored = Renderer3D::GetRendererSettings();
                restored.Path = Path;
                restored.VirtualGeometryEnabled = VirtualGeometry;
                Renderer3D::ApplyRendererSettings();
            }
        } const settingsRestore{ Renderer3D::GetRendererSettings().Path,
                                 Renderer3D::GetRendererSettings().VirtualGeometryEnabled };

        u32 width = 0;
        u32 height = 0;
        const auto capture = [&](bool bakeEnabled, const char* fileName, std::vector<u8>& out)
        {
            Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
            Renderer3D::ApplyRendererSettings();
            lmSettings.Enabled = bakeEnabled;
            // 3 frames: a path switch rebuilds the frame graph, so the temporal
            // state gets a frame to settle before the captured one.
            RunFrames(3);
            u32 w = 0;
            u32 h = 0;
            ASSERT_TRUE(CaptureTopDown(out, w, h)) << "ReadbackComposite failed for " << fileName;
            if (width == 0)
            {
                width = w;
                height = h;
            }
            // The analysis rects are derived from one camera pose at aspect 1; a
            // differently shaped capture would still produce in-range rects and
            // silently measure the wrong patch of floor. The size check is not
            // belt-and-braces either: the mean helpers index the vector raw.
            ASSERT_EQ(w, kSize);
            ASSERT_EQ(h, kSize);
            ASSERT_EQ(out.size(), static_cast<std::size_t>(w) * h * 4u);
            WriteEvidencePng(fileName, out, w, h);
        };

        std::vector<u8> deferredOn;
        std::vector<u8> deferredOff;
        capture(true, "Lightmap_InstancedDeferred_On.png", deferredOn);
        if (::testing::Test::HasFatalFailure())
            return;
        capture(false, "Lightmap_InstancedDeferred_Off.png", deferredOff);
        if (::testing::Test::HasFatalFailure())
            return;
        lmSettings.Enabled = true;

        const PixelRect redRect = MakeRect(kRedRegionX0, kRedRegionX1, kFloorRegionY0, kFloorRegionY1, width, height);
        const PixelRect greenRect =
            MakeRect(kGreenRegionX0, kGreenRegionX1, kFloorRegionY0, kFloorRegionY1, width, height);

        const RegionMeans onRed = MeanChannelsInRect(deferredOn, width, redRect);
        const RegionMeans onGreen = MeanChannelsInRect(deferredOn, width, greenRect);
        const RegionMeans offRed = MeanChannelsInRect(deferredOff, width, redRect);
        const RegionMeans offGreen = MeanChannelsInRect(deferredOff, width, greenRect);

        // Per-instance hue, on the deferred path: the region reached RT5 through
        // the instance buffer, and each instance addressed its OWN charts.
        EXPECT_GT(onRed.R, onRed.G * 1.05f)
            << "deferred instanced: the tile beside the RED wall is not red-shifted: r=" << onRed.R
            << " g=" << onRed.G << " — see Lightmap_InstancedDeferred_On.png";
        EXPECT_GT(onGreen.G, onGreen.R * 1.05f)
            << "deferred instanced: the tile beside the GREEN wall is not green-shifted: g=" << onGreen.G
            << " r=" << onGreen.R;

        // ON vs OFF: "the chain is broken anywhere" — a G-Buffer pass that never
        // wrote RT5 for an instanced draw looks exactly like an unbaked scene.
        const f32 meanAbsDiff = 0.5f * (MeanAbsDiffInRect(deferredOn, deferredOff, width, redRect) +
                                        MeanAbsDiffInRect(deferredOn, deferredOff, width, greenRect));
        EXPECT_GT(meanAbsDiff, 3.0f)
            << "deferred instanced: ON and OFF barely differ over the floor (" << meanAbsDiff
            << " grey levels) — the per-instance region never reached the G-Buffer's RT5";

        EXPECT_GT((onRed.R - offRed.R), (onRed.G - offRed.G) + 0.5f)
            << "deferred instanced: the light ADDED beside the RED wall is not red-shifted";
        EXPECT_GT((onGreen.G - offGreen.G), (onGreen.R - offGreen.R) + 0.5f)
            << "deferred instanced: the light ADDED beside the GREEN wall is not green-shifted";
    }

    // -------------------------------------------------------------------------
    // LightmapVirtualBleedRoom — the SAME room, floor drawn as a
    // VirtualMeshComponent (issue #867, section 3).
    //
    // Virtual geometry submits only on Deferred, so every capture here forces
    // that path. The floor's MeshSource is registered with the asset manager and
    // referenced by handle, the shape a real virtual mesh has.
    //
    // ORDERING IS THE WHOLE TRICK. A cluster DAG is cooked when the mesh is
    // first registered, and UV2 only exists after the bake's unwrap — so a mesh
    // registered before the bake cooks WITHOUT the stream and can never sample,
    // silently. The fixture bakes first and only then lets the virtual path
    // register the mesh, which is exactly what EditorLayer::BakeLightmaps
    // arranges with its VirtualMeshRegistry::Invalidate call.
    // -------------------------------------------------------------------------
    class LightmapVirtualBleedRoom : public LightmapBleedRoom
    {
      protected:
        [[nodiscard]] u32 ExpectedReceiverCount() const override
        {
            return 4u; // virtual floor + red wall + green wall + ceiling
        }

        void BuildFloor() override
        {
            Scene& scene = GetScene();

            Ref<Mesh> cube = MeshPrimitives::CreateCube();
            ASSERT_TRUE(cube);
            Ref<MeshSource> source = cube->GetMeshSource();
            ASSERT_TRUE(source);

            m_FloorMeshHandle = AssetManager::AddMemoryOnlyAsset(source);
            ASSERT_NE(static_cast<u64>(m_FloorMeshHandle), 0u);

            m_Floor = scene.CreateEntity("VirtualFloor");
            auto& transform = m_Floor.GetComponent<TransformComponent>();
            transform.Translation = { 0.0f, -0.1f, 0.0f };
            transform.Scale = { 6.0f, 0.2f, 4.0f };

            auto& vm = m_Floor.AddComponent<VirtualMeshComponent>();
            vm.m_MeshSource = m_FloorMeshHandle;
            vm.m_Enabled = true;
            vm.m_ErrorThresholdPixels = 1.0f;
            vm.m_LightmapStatic = true;

            auto& material = m_Floor.AddComponent<MaterialComponent>();
            material.m_Material.SetBaseColorFactor(glm::vec4(0.6f, 0.6f, 0.6f, 1.0f));
            material.m_Material.SetMetallicFactor(0.0f);
            material.m_Material.SetRoughnessFactor(0.9f);
        }

        AssetHandle m_FloorMeshHandle = 0;
    };

    TEST_F(LightmapVirtualBleedRoom, VirtualGeometryReceivesBakedGIOnBothSidesOfTheToggle)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        ASSERT_NO_FATAL_FAILURE(BakeAndResolve());

        Scene& scene = GetScene();
        SceneLightmapSettings& lmSettings = scene.GetLightmapSettings();
        auto& runtime = scene.GetLightmapRuntime();

        // The CPU half first: one MeshSource is one unwrap and one region, so the
        // virtual receiver's sub-key is 0 like the classic path's.
        ASSERT_GT(runtime->GetScaleOffset(m_Floor.GetUUID(), 0).x, 0.0f)
            << "the virtual floor has no atlas region — the gather did not pick the receiver up";

        // The cook must have been invalidated by the unwrap, or the DAG carries
        // no UV2 and the runtime is right to refuse to sample. Asserting it here
        // means a red pixel test below reads as "the shader mis-addressed it"
        // rather than "the geometry never had the stream".
        {
            Ref<MeshSource> floorSource = AssetManager::GetAsset<MeshSource>(m_FloorMeshHandle);
            ASSERT_TRUE(floorSource);
            ASSERT_TRUE(floorSource->HasLightmapUVs())
                << "the floor's MeshSource has no UV2 after the bake's unwrap, so the cluster DAG could "
                   "never carry one either";
        }

        // Renderer3D settings are PROCESS-GLOBAL. Restore whatever this process
        // had before, or a leaked Deferred / disabled-VG state silently
        // re-targets every later test in the binary — the same hazard the
        // deferred-path test above documents, and the reason it keeps a
        // PathRestore guard. The virtual test needs both fields, and it must
        // restore the value it OBSERVED rather than a literal: the defaults are
        // not this test's to assert.
        struct SettingsRestore
        {
            RenderingPath Path;
            bool VirtualGeometry;
            ~SettingsRestore()
            {
                auto& restored = Renderer3D::GetRendererSettings();
                restored.Path = Path;
                restored.VirtualGeometryEnabled = VirtualGeometry;
                Renderer3D::ApplyRendererSettings();
            }
        } const settingsRestore{ Renderer3D::GetRendererSettings().Path,
                                 Renderer3D::GetRendererSettings().VirtualGeometryEnabled };

        u32 width = 0;
        u32 height = 0;
        const auto capture = [&](bool virtualEnabled, bool bakeEnabled, const char* fileName,
                                 std::vector<u8>& out)
        {
            auto& settings = Renderer3D::GetRendererSettings();
            settings.Path = RenderingPath::Deferred; // virtual geometry submits nowhere else
            settings.VirtualGeometryEnabled = virtualEnabled;
            Renderer3D::ApplyRendererSettings();
            lmSettings.Enabled = bakeEnabled;
            // 3 frames: switching the path or the master switch rebuilds the
            // frame graph, so the temporal state gets a frame to settle.
            RunFrames(3);
            u32 w = 0;
            u32 h = 0;
            ASSERT_TRUE(CaptureTopDown(out, w, h)) << "ReadbackComposite failed for " << fileName;
            if (width == 0)
            {
                width = w;
                height = h;
            }
            ASSERT_EQ(w, kSize);
            ASSERT_EQ(h, kSize);
            ASSERT_EQ(out.size(), static_cast<std::size_t>(w) * h * 4u);
            WriteEvidencePng(fileName, out, w, h);
        };

        std::vector<u8> virtualOn;
        std::vector<u8> virtualOff;
        std::vector<u8> bakeOff;
        capture(true, true, "Lightmap_Virtual_On.png", virtualOn);
        if (::testing::Test::HasFatalFailure())
            return;
        capture(false, true, "Lightmap_Virtual_ClassicFallback.png", virtualOff);
        if (::testing::Test::HasFatalFailure())
            return;
        capture(true, false, "Lightmap_Virtual_Off.png", bakeOff);
        if (::testing::Test::HasFatalFailure())
            return;
        lmSettings.Enabled = true; // settingsRestore puts the renderer back

        const PixelRect redRect = MakeRect(kRedRegionX0, kRedRegionX1, kFloorRegionY0, kFloorRegionY1, width, height);
        const PixelRect greenRect =
            MakeRect(kGreenRegionX0, kGreenRegionX1, kFloorRegionY0, kFloorRegionY1, width, height);

        const RegionMeans onRed = MeanChannelsInRect(virtualOn, width, redRect);
        const RegionMeans onGreen = MeanChannelsInRect(virtualOn, width, greenRect);
        const RegionMeans offRed = MeanChannelsInRect(bakeOff, width, redRect);
        const RegionMeans offGreen = MeanChannelsInRect(bakeOff, width, greenRect);

        // 1. The virtual raster samples the atlas at all.
        EXPECT_GT(onRed.R, onRed.G * 1.05f)
            << "virtual: the floor beside the RED wall is not red-shifted: r=" << onRed.R << " g=" << onRed.G
            << " — the cluster raster is not reading the uv2 tail. See Lightmap_Virtual_On.png";
        EXPECT_GT(onGreen.G, onGreen.R * 1.05f)
            << "virtual: the floor beside the GREEN wall is not green-shifted: g=" << onGreen.G
            << " r=" << onGreen.R;

        // 2. ON vs OFF — "the chain is broken anywhere" looks exactly like the
        //    old probe/IBL fallback, so only the differential separates them.
        const f32 bakeDiff = 0.5f * (MeanAbsDiffInRect(virtualOn, bakeOff, width, redRect) +
                                     MeanAbsDiffInRect(virtualOn, bakeOff, width, greenRect));
        EXPECT_GT(bakeDiff, 3.0f)
            << "virtual: ON and OFF barely differ over the floor (" << bakeDiff
            << " grey levels) — the region never reached the cluster raster's RT5 write";

        // 3. THE ACCEPTANCE CRITERION #867 states for this receiver: identical
        //    baked GI with the VG toggle on and off. The two paths are different
        //    rasterizers over different intermediates, so this is a REGION MEAN
        //    comparison, never per-pixel — but it must be tight, because the
        //    whole point of the toggle is that only the renderer differs.
        //
        //    Wiring only the fallback would sail through contracts 1 and 2 and
        //    fail exactly here, which is why this assertion is the one that
        //    matters.
        const f32 toggleDiff = 0.5f * (MeanAbsDiffInRect(virtualOn, virtualOff, width, redRect) +
                                       MeanAbsDiffInRect(virtualOn, virtualOff, width, greenRect));

        // The criterion is "only the RENDERER differs", so bound the toggle's
        // effect ABSOLUTELY. The relative check below is only an ordering guard:
        // on its own it passes at 0.9 * bakeDiff, which would be a gross parity
        // failure between the two rasterizers while still reading green.
        //
        // Sized like the forward-vs-deferred kMaxPathMeanDelta above, and for
        // the same reason: two rasterizers over different intermediates are not
        // per-pixel equal, but a dropped or mis-addressed region moves these
        // means by the ON/OFF signal or more. Live MCP probing of RT5 on a real
        // imported asset reads BIT-IDENTICAL across the toggle, so the true
        // value here is far below the bound.
        constexpr f32 kMaxToggleMeanDelta = 12.0f;
        EXPECT_LT(toggleDiff, kMaxToggleMeanDelta)
            << "virtual: the VG master switch changed the floor by " << toggleDiff
            << " grey levels — the two rasterizers must deliver the same baked GI. Compare "
            << "Lightmap_Virtual_On.png against Lightmap_Virtual_ClassicFallback.png";

        EXPECT_LT(toggleDiff, bakeDiff)
            << "virtual: the VG master switch changes the floor MORE than the bake itself does ("
            << toggleDiff << " vs " << bakeDiff << " grey levels). Baked GI that appears and "
            << "disappears with VirtualGeometryEnabled destroys the toggle's value as an A/B — see "
            << "Lightmap_Virtual_On.png against Lightmap_Virtual_ClassicFallback.png";

        const RegionMeans fallbackRed = MeanChannelsInRect(virtualOff, width, redRect);
        const RegionMeans fallbackGreen = MeanChannelsInRect(virtualOff, width, greenRect);
        EXPECT_GT(fallbackRed.R, fallbackRed.G * 1.05f)
            << "virtual: the CLASSIC FALLBACK lost the red bleed, so the toggle is not an honest A/B";
        EXPECT_GT(fallbackGreen.G, fallbackGreen.R * 1.05f)
            << "virtual: the CLASSIC FALLBACK lost the green bleed";
    }
} // namespace OloEngine::Tests

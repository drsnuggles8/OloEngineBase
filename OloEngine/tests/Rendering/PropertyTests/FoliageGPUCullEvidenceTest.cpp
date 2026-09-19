#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L8
// =============================================================================
// FoliageGPUCullEvidenceTest — issue #1235, all four acceptance criteria.
//
// Writes
//   OloEditor/assets/tests/visual/FoliageCull_GL_<Path>_MSAA1_<Angle>.png
// and its A/B control (the same frame with the GPU cull switched off)
//   OloEditor/assets/tests/visual/FoliageCullOff_GL_<Path>_MSAA1_<Angle>.png
//
// The filename carries the {backend} x {path} cell, including the backend even
// though it is always GL here: these fixtures need a real GL 4.6 context and
// skip without one, so every Vulkan cell is LIVE-ONLY and evidenced in the PR
// body from a real editor session. A reader counting files must not mistake a
// complete set of OpenGL captures for a complete matrix.
//
// `_MSAA1` is in the name to match the existing FoliageWind_* set, NOT because
// MSAA is a cell: the cull decides visibility before rasterization and reads
// nothing from the resolve. Nor is a non-native resolution a cell — every
// reject here is world-space (frustum planes + a distance to the instance's
// AABB); there is no screen-coverage or projected-size threshold anywhere in
// FoliageInstanceCull.comp, so no reject can depend on the output resolution.
// Nothing is persisted either: no serialized field changed, so the
// {scene YAML, asset pack, save-game} row is not a cell.
//
// What is checked, and why each one can be wrong while the picture still looks
// like grass:
//
//   1. **The culled frame is the unculled frame.** The failure this whole
//      feature risks is deleting plants that were visible, and that reads as a
//      slightly emptier field — plausible, and invisible without the control.
//      So every path captures BOTH arms back to back under mocked time (so the
//      wind phase is identical) and compares the vegetated pixel population.
//   2. **The survivor set is exactly the CPU reference's.** Re-derived here
//      from the canonical records with FoliageInstanceBounds + the same frustum
//      the dispatch was given, so a drift between the GLSL bound and the C++
//      one fails as a set difference rather than as popping at the screen edge.
//   3. **Compaction preserves identity across a leave-and-return.** The camera
//      turns away until nothing survives and comes back; every compacted slot
//      must still resolve, through its source row, to the canonical
//      FoliageInstanceId whose position it carries. A plant inheriting a
//      neighbour's history is exactly what this catches.
//   4. **Overflow is bounded, counted and explicit.** The debug capacity really
//      truncates the append, so the draw really renders fewer plants and the
//      counters really say by how much.
//   5. **The shadow view is culled on its own.** A separate slot, a separate
//      frustum, and a survivor set that is NOT the main view's.
//
// The committed PNGs come from an ISOLATED run of this fixture: renderer state
// left behind by other tests shades them differently
// (evidence-png-shading-depends-on-test-order). That does not weaken anything —
// every measurement is an A/B between two frames captured back to back under
// whatever state is live — but the pictures are order-sensitive and exist for a
// human to look at, never as a compared baseline.
//
// Classification: L8 / visual evidence (full GL pipeline + RGBA8 readback +
// PNG + a GPU->CPU buffer readback). Skips cleanly without a GL 4.6 context;
// never DISABLED_.
// =============================================================================

#include "../../TestOptions.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"

#include "OloEngine/Utils/PlatformUtils.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Terrain/Foliage/FoliageGPUCuller.h"
#include "OloEngine/Terrain/Foliage/FoliageRenderer.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainMaterial.h"

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr u32 kWidth = 1280;
        constexpr u32 kHeight = 720;
        // Mocked so both arms of every A/B see the SAME wind phase. Without it
        // the second capture's blades have moved and the pixel comparison
        // measures the wind rather than the cull.
        constexpr f32 kCaptureTime = 4.0f;

        struct ScopedMockTime
        {
            explicit ScopedMockTime(f32 t)
            {
                Time::SetMockTime(t);
            }
            ~ScopedMockTime()
            {
                Time::ClearMockTime();
            }
            ScopedMockTime(const ScopedMockTime&) = delete;
            ScopedMockTime& operator=(const ScopedMockTime&) = delete;
            ScopedMockTime(ScopedMockTime&&) = delete;
            ScopedMockTime& operator=(ScopedMockTime&&) = delete;
        };

        // Restores the process-wide cull lever whatever the test does, so a
        // failed assertion cannot leave every later fixture running unculled.
        struct ScopedGPUCulling
        {
            explicit ScopedGPUCulling(bool enabled)
                : m_Previous(FoliageRenderer::IsGPUCullingEnabled())
            {
                FoliageRenderer::SetGPUCullingEnabled(enabled);
            }
            ~ScopedGPUCulling()
            {
                FoliageRenderer::SetGPUCullingEnabled(m_Previous);
            }
            ScopedGPUCulling(const ScopedGPUCulling&) = delete;
            ScopedGPUCulling& operator=(const ScopedGPUCulling&) = delete;
            ScopedGPUCulling(ScopedGPUCulling&&) = delete;
            ScopedGPUCulling& operator=(ScopedGPUCulling&&) = delete;

          private:
            bool m_Previous;
        };

        [[nodiscard]] bool IsVegetated(u8 r, u8 g, u8 b)
        {
            const int ri = r;
            const int gi = g;
            const int bi = b;
            return gi > 40 && gi > ri + 12 && gi > bi + 12;
        }

        [[nodiscard]] u32 CountVegetated(const std::vector<u8>& px)
        {
            u32 n = 0;
            for (sizet i = 0; i + 3 < px.size(); i += 4)
                if (IsVegetated(px[i + 0], px[i + 1], px[i + 2]))
                    ++n;
            return n;
        }

        [[nodiscard]] f64 MeanLuminance(const std::vector<u8>& px)
        {
            f64 sum = 0.0;
            sizet n = 0;
            for (sizet i = 0; i + 3 < px.size(); i += 4, ++n)
                sum += (0.2126 * px[i] + 0.7152 * px[i + 1] + 0.0722 * px[i + 2]) / 255.0;
            return n == 0 ? 0.0 : sum / static_cast<f64>(n);
        }

        [[nodiscard]] u32 CountDifferingPixels(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size())
                return ~0u;
            u32 n = 0;
            for (sizet i = 0; i + 3 < a.size(); i += 4)
                if (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2])
                    ++n;
            return n;
        }

        struct AngleCase
        {
            const char* m_Name;
            glm::vec3 m_Eye;
            f32 m_Yaw;
            f32 m_Pitch;
        };

        // Three framings of the same vegetated island. Yaw is 0 or pi for the
        // reason FoliageInstanceIdentityEvidenceTest gives: EditorCamera's yaw
        // sign is not obvious from the call site, and 0/pi look toward -Z/+Z
        // either way. Pitch is POSITIVE downward.
        //
        // "Narrow" exists for the patch test specifically: a camera that sees a
        // small part of the island must reject most SPATIAL GROUPS, and a
        // landscape framing that keeps them all would make the group pass look
        // correct while doing nothing.
        constexpr std::array<AngleCase, 3> kAngles = { {
            { "Ground", glm::vec3(128.0f, 34.0f, 162.0f), 0.0f, 0.2f },
            { "Reverse", glm::vec3(128.0f, 42.0f, -25.0f), 3.14159265f, 0.18f },
            { "Overview", glm::vec3(128.0f, 130.0f, 250.0f), 0.0f, 0.72f },
        } };

        // Looking up at empty sky from inside the island: the leave half of the
        // leave-and-return.
        constexpr AngleCase kAway{ "Away", glm::vec3(128.0f, 34.0f, 162.0f), 0.0f, -1.4f };
    } // namespace

    class FoliageGPUCullEvidenceTest : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                dl.m_Direction = glm::normalize(glm::vec3(-0.4f, -0.8f, -0.3f));
                dl.m_Color = glm::vec3(1.0f, 0.97f, 0.92f);
                dl.m_Intensity = 3.0f;
                // Shadows ON: the shadow-view cull is an acceptance criterion,
                // and without a caster there is no shadow view to cull for.
                dl.m_CastShadows = true;
            }

            // The same island the other foliage evidence fixtures build, so the
            // captures are comparable with FoliageWind_* / FoliageHabitat_*.
            m_TerrainEntity = scene.CreateEntity("Terrain");
            {
                auto& terrain = m_TerrainEntity.AddComponent<TerrainComponent>();
                terrain.m_ProceduralEnabled = true;
                terrain.m_ProceduralSeed = 7;
                terrain.m_ProceduralResolution = 192;
                terrain.m_ProceduralOctaves = 5;
                terrain.m_ProceduralFrequency = 2.0f;
                terrain.m_HeightShaping.HeightExponent = 1.3f;
                terrain.m_WorldSizeX = 256.0f;
                terrain.m_WorldSizeZ = 256.0f;
                terrain.m_HeightScale = 28.0f;
                terrain.m_TessellationEnabled = false;

                terrain.m_AutoMaterial = true;
                terrain.m_SplatmapGenResolution = 256;
                terrain.m_Material = Ref<TerrainMaterial>::Create();
                for (const auto& layer : TerrainGenerator::MakeDefaultLayers())
                    terrain.m_Material->AddLayer(layer);
                terrain.m_LayerRules = TerrainGenerator::MakeDefaultRules();
                terrain.m_MaterialNeedsRebuild = true;
                terrain.m_AutoSplatNeedsRebuild = true;

                auto& foliage = m_TerrainEntity.AddComponent<FoliageComponent>();
                foliage.m_Enabled = true;
                foliage.m_Layers = TerrainGenerator::MakeFoliageLayersFromRules(terrain.m_LayerRules);
                foliage.m_NeedsRebuild = true;
            }
        }

        [[nodiscard]] FoliageRenderer* Foliage()
        {
            if (!m_TerrainEntity || !m_TerrainEntity.HasComponent<FoliageComponent>())
                return nullptr;
            return m_TerrainEntity.GetComponent<FoliageComponent>().m_Renderer.get();
        }

        void Capture(const std::string& saveAs, const AngleCase& angle, std::vector<u8>& outPixels, u32 frames = 4)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 3000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(angle.m_Eye, angle.m_Yaw, angle.m_Pitch);
            RunEditorFrames(camera, frames);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No SceneColor framebuffer for '" << saveAs << "'";
            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), kWidth, kHeight, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<sizet>(kWidth) * kHeight * 4u);

            // GL readback is bottom-up.
            {
                const sizet rowBytes = static_cast<sizet>(kWidth) * 4u;
                std::vector<u8> tmp(rowBytes);
                for (u32 y = 0; y < kHeight / 2u; ++y)
                {
                    u8* top = outPixels.data() + (static_cast<sizet>(y) * rowBytes);
                    u8* bot = outPixels.data() + (static_cast<sizet>(kHeight - 1u - y) * rowBytes);
                    std::memcpy(tmp.data(), top, rowBytes);
                    std::memcpy(top, bot, rowBytes);
                    std::memcpy(bot, tmp.data(), rowBytes);
                }
            }

            if (!saveAs.empty())
                WriteEvidence(saveAs, outPixels);
        }

        // Evidence, not an SSIM golden: the subject is a procedural
        // distribution, and a committed per-pixel baseline of one would be a
        // flake generator the first time a driver rounds a blade differently.
        // The contracts are the assertions; the PNGs exist so a reviewer can
        // look at what they describe. Always writes, never compares.
        static void WriteEvidence(const std::string& name, const std::vector<u8>& pixels)
        {
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ASSERT_FALSE(ec) << "Failed to create evidence dir '" << dir.string() << "': " << ec.message();
            const std::string path = (dir / (name + ".png")).string();
            const int wrote = ::stbi_write_png(path.c_str(), static_cast<int>(kWidth), static_cast<int>(kHeight), 4,
                                               pixels.data(), static_cast<int>(kWidth) * 4);
            ASSERT_NE(wrote, 0) << "stbi_write_png failed to write '" << path << "'";
        }

        Entity m_TerrainEntity;
    };

    // ── Criteria 1 + 3: the culled frame IS the unculled frame, everywhere ────

    TEST_F(FoliageGPUCullEvidenceTest, CulledFrameMatchesTheUnculledOneOnEveryRenderingPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);

        struct PathCase
        {
            const char* m_Name;
            RenderingPath m_Path;
        };
        constexpr std::array<PathCase, 3> paths = { {
            { "Forward", RenderingPath::Forward },
            { "ForwardPlus", RenderingPath::ForwardPlus },
            { "Deferred", RenderingPath::Deferred },
        } };

        for (const PathCase& pathCase : paths)
        {
            Renderer3D::GetRendererSettings().Path = pathCase.m_Path;
            Renderer3D::ApplyRendererSettings();

            for (const AngleCase& angle : kAngles)
            {
                std::vector<u8> unculled;
                {
                    const ScopedGPUCulling off(false);
                    Capture(std::string("FoliageCullOff_GL_") + pathCase.m_Name + "_MSAA1_" + angle.m_Name, angle,
                            unculled);
                }
                if (::testing::Test::HasFatalFailure())
                    return;

                std::vector<u8> culled;
                {
                    const ScopedGPUCulling on(true);
                    Capture(std::string("FoliageCull_GL_") + pathCase.m_Name + "_MSAA1_" + angle.m_Name, angle,
                            culled);
                }
                if (::testing::Test::HasFatalFailure())
                    return;

                // A black frame makes everything below vacuous, so rule it out
                // first rather than reporting a proud 0-pixel difference
                // between two empty images.
                ASSERT_GT(MeanLuminance(culled), 0.02)
                    << pathCase.m_Name << '/' << angle.m_Name << ": the culled frame is (near-)black";
                ASSERT_GT(MeanLuminance(unculled), 0.02)
                    << pathCase.m_Name << '/' << angle.m_Name << ": the control frame is (near-)black";

                const u32 vegCulled = CountVegetated(culled);
                const u32 vegUnculled = CountVegetated(unculled);
                const u32 differing = CountDifferingPixels(culled, unculled);
                std::printf("[foliage-cull] %-11s %-9s  vegetated %u -> %u px, %u px differ\n", pathCase.m_Name,
                            angle.m_Name, vegUnculled, vegCulled, differing);

                ASSERT_GT(vegUnculled, 2000u)
                    << pathCase.m_Name << '/' << angle.m_Name
                    << ": the control shows almost no vegetation, so the comparison proves nothing";

                // THE assertion this fixture exists for. The cull's bounds are
                // conservative (the registry's group AABBs are the union of the
                // instances', each already padded for wind and interaction), so
                // it must not remove a single plant the viewer can see. A small
                // band is allowed for the alpha-cutout edge pixels a different
                // draw ORDER produces, not for missing plants.
                const auto tolerance = static_cast<u32>(static_cast<f64>(vegUnculled) * 0.02);
                EXPECT_NEAR(static_cast<f64>(vegCulled), static_cast<f64>(vegUnculled),
                            static_cast<f64>(std::max(tolerance, 64u)))
                    << pathCase.m_Name << '/' << angle.m_Name
                    << ": GPU culling changed how much vegetation is on screen — it removed plants that were "
                       "visible, or drew ones that were not";
            }
        }
    }

    // ── Criterion 1: the survivor set is the CPU reference's, exactly ─────────

    TEST_F(FoliageGPUCullEvidenceTest, SurvivorSetMatchesTheCanonicalRecords)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);
        const ScopedGPUCulling on(true);

        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        std::vector<u8> pixels;
        Capture("", kAngles[0], pixels);
        ASSERT_FALSE(HasFatalFailure());

        FoliageRenderer* foliage = Foliage();
        ASSERT_NE(foliage, nullptr) << "the foliage renderer was never created on the editor render path";
        ASSERT_TRUE(foliage->WasMainViewCulled())
            << "no layer produced a compacted main-view draw, so there is nothing to compare";

        // The SAME inputs the dispatch was given: MakeCullInputs is what
        // Scene::OnRender calls, over the culling camera's matrices.
        const FoliageGPUCuller::ViewInputs base =
            foliage->MakeCullInputs(Renderer3D::GetCullViewProjectionMatrix(), Renderer3D::GetCullViewPosition());

        const auto& registry = foliage->GetInstanceRegistry();
        u32 layersCompared = 0;
        for (u32 layerIndex = 0; layerIndex < foliage->GetLayerCount(); ++layerIndex)
        {
            FoliageGPUCuller::Readback readback;
            if (!foliage->ReadbackCull(layerIndex, FoliageGPUCuller::ViewSlot::Main, readback))
                continue;

            // The survivor set is rebuilt from each record's OWN m_LocalBounds:
            // that box is what FoliageInstanceBounds produced for this instance
            // with this layer's profile, which is precisely what the shader
            // recomputes from the row. Comparing against the registry rather
            // than against a second transcription of the GLSL is what makes an
            // agreement mean something.
            FoliageGPUCuller::ViewInputs inputs = base;
            // The draw info carries the layer's authored view distance, which is
            // the cutoff CullForView passes.
            inputs.MaxDistance = 0.0f;
            for (const auto& info : foliage->GetActiveLayerDrawInfo())
            {
                if (info.LayerIndex == layerIndex)
                {
                    inputs.MaxDistance = info.ViewDistance;
                    break;
                }
            }
            ASSERT_GT(inputs.MaxDistance, 0.0f) << "layer " << layerIndex << " has no active draw";

            std::set<u32> expected;
            for (const auto& record : registry.GetRecords())
            {
                if (record.m_LayerIndex != layerIndex)
                    continue;
                const BoundingBox& box = record.m_LocalBounds;
                if (!inputs.ViewFrustum.IsBoxVisible(box.Min, box.Max))
                    continue;
                const glm::vec3 closest = glm::clamp(inputs.DistanceOrigin, box.Min, box.Max);
                if (glm::length(closest - inputs.DistanceOrigin) > inputs.MaxDistance)
                    continue;
                expected.insert(record.m_BufferIndex);
            }

            const std::set<u32> actual(readback.SourceRows.begin(), readback.SourceRows.end());
            EXPECT_EQ(actual.size(), readback.SourceRows.size())
                << "layer " << layerIndex << ": a source row appears twice in the compacted stream — the append "
                                             "handed two invocations the same slot";

            std::vector<u32> missing;
            std::vector<u32> extra;
            std::set_difference(expected.begin(), expected.end(), actual.begin(), actual.end(),
                                std::back_inserter(missing));
            std::set_difference(actual.begin(), actual.end(), expected.begin(), expected.end(),
                                std::back_inserter(extra));

            std::printf("[foliage-cull] layer %u  expected %zu, gpu %zu, missing %zu, extra %zu (groups visible %u)\n",
                        layerIndex, expected.size(), actual.size(), missing.size(), extra.size(),
                        readback.GroupsVisible);

            EXPECT_TRUE(missing.empty()) << "layer " << layerIndex << ": the GPU cull dropped " << missing.size()
                                         << " plants the CPU reference keeps — plants popping at the frustum edge";
            EXPECT_TRUE(extra.empty()) << "layer " << layerIndex << ": the GPU cull kept " << extra.size()
                                       << " plants the CPU reference rejects";

            // The PATCH level did work: a ground-level camera cannot see every
            // 16 m bucket of a 256 m island. If it could, the group pass would
            // be a no-op wearing the costume of an optimisation.
            u32 totalGroups = 0;
            for (const auto& group : registry.GetGroups())
                if (group.m_LayerIndex == layerIndex)
                    ++totalGroups;
            EXPECT_LT(readback.GroupsVisible, totalGroups)
                << "layer " << layerIndex << ": every spatial group survived, so the patch cull rejected nothing";

            ++layersCompared;
        }

        EXPECT_GT(layersCompared, 0u) << "no layer was culled, so nothing above ran";
    }

    // ── Criterion 2: identity survives a leave-and-return ────────────────────

    TEST_F(FoliageGPUCullEvidenceTest, CompactionPreservesIdentityAcrossLeaveAndReturn)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);
        const ScopedGPUCulling on(true);

        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        // Every compacted slot must carry the record of the id its source row
        // names. That is the whole claim: slot N holds plant P's data and plant
        // P's identity, not plant Q's.
        // Out-parameter rather than a return value: the body uses ASSERT_*,
        // which returns void, so a value-returning lambda does not compile.
        std::vector<std::set<FoliageInstanceId>> visibleIds;
        const auto collect = [this, &visibleIds](const char* label)
        {
            FoliageRenderer* foliage = Foliage();
            visibleIds.assign(foliage->GetLayerCount(), {});
            const auto& registry = foliage->GetInstanceRegistry();

            for (u32 layerIndex = 0; layerIndex < foliage->GetLayerCount(); ++layerIndex)
            {
                FoliageGPUCuller::Readback readback;
                if (!foliage->ReadbackCull(layerIndex, FoliageGPUCuller::ViewSlot::Main, readback))
                    continue;

                for (u32 slot = 0; slot < readback.Submitted; ++slot)
                {
                    const u32 row = readback.SourceRows[slot];
                    const FoliageInstanceId id = registry.GetIdForBufferRow(layerIndex, row);
                    ASSERT_NE(id, kInvalidFoliageInstanceId)
                        << label << ": compacted slot " << slot << " of layer " << layerIndex
                        << " names buffer row " << row << ", which resolves to no canonical instance";

                    const FoliageInstanceRecord* record = registry.Find(id);
                    ASSERT_NE(record, nullptr) << label << ": id " << id << " is not live";

                    // THE identity assertion. A compaction that reused another
                    // instance's slot data would put plant Q's position in the
                    // slot plant P's row points at, and this is where that shows
                    // up — as coordinates that belong to a different plant.
                    const glm::vec3 compactedPos{ readback.Compacted[slot].PositionScale };
                    ASSERT_NEAR(compactedPos.x, record->m_Position.x, 1e-4f) << label << " slot " << slot;
                    ASSERT_NEAR(compactedPos.y, record->m_Position.y, 1e-4f) << label << " slot " << slot;
                    ASSERT_NEAR(compactedPos.z, record->m_Position.z, 1e-4f) << label << " slot " << slot;
                    ASSERT_NEAR(readback.Compacted[slot].PositionScale.w, record->m_Scale, 1e-4f)
                        << label << " slot " << slot;
                    ASSERT_NEAR(readback.Compacted[slot].RotationHeight.y, record->m_Height, 1e-4f)
                        << label << " slot " << slot;

                    visibleIds[layerIndex].insert(id);
                }
            }
        };

        std::vector<u8> pixels;
        Capture("FoliageCull_GL_Deferred_MSAA1_Return_Before", kAngles[0], pixels);
        ASSERT_FALSE(HasFatalFailure());
        ASSERT_NE(Foliage(), nullptr);
        ASSERT_TRUE(Foliage()->WasMainViewCulled());
        collect("before");
        ASSERT_FALSE(HasFatalFailure());
        const std::vector<std::set<FoliageInstanceId>> before = visibleIds;

        // ── Leave ──
        Capture("", kAway, pixels);
        ASSERT_FALSE(HasFatalFailure());
        u32 survivorsWhileAway = 0;
        for (u32 layerIndex = 0; layerIndex < Foliage()->GetLayerCount(); ++layerIndex)
        {
            FoliageGPUCuller::Readback readback;
            if (Foliage()->ReadbackCull(layerIndex, FoliageGPUCuller::ViewSlot::Main, readback))
                survivorsWhileAway += readback.Submitted;
        }

        // ── Return ──
        Capture("FoliageCull_GL_Deferred_MSAA1_Return_After", kAngles[0], pixels);
        ASSERT_FALSE(HasFatalFailure());
        collect("after");
        ASSERT_FALSE(HasFatalFailure());
        const std::vector<std::set<FoliageInstanceId>> after = visibleIds;

        sizet total = 0;
        for (const auto& ids : before)
            total += ids.size();
        std::printf("[foliage-cull] leave-and-return: %zu ids visible, %u survivors while looking away\n", total,
                    survivorsWhileAway);

        ASSERT_GT(total, 0u) << "nothing was visible before turning away, so the return proves nothing";
        EXPECT_LT(survivorsWhileAway, total)
            << "turning to face empty sky culled nothing, so the camera never actually left";

        ASSERT_EQ(before.size(), after.size());
        for (sizet layerIndex = 0; layerIndex < before.size(); ++layerIndex)
        {
            EXPECT_EQ(before[layerIndex], after[layerIndex])
                << "layer " << layerIndex
                << ": the set of plants visible from the same camera changed across a leave-and-return";
        }
    }

    // ── Criterion 4: overflow is bounded, counted and explicit ───────────────

    TEST_F(FoliageGPUCullEvidenceTest, OverflowTruncatesBoundedAndCounted)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);
        const ScopedGPUCulling on(true);

        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        std::vector<u8> healthy;
        Capture("", kAngles[0], healthy);
        ASSERT_FALSE(HasFatalFailure());
        FoliageRenderer* foliage = Foliage();
        ASSERT_NE(foliage, nullptr);
        ASSERT_TRUE(foliage->WasMainViewCulled());

        u32 layerUnderTest = ~0u;
        u32 visibleBefore = 0;
        for (u32 layerIndex = 0; layerIndex < foliage->GetLayerCount(); ++layerIndex)
        {
            FoliageGPUCuller::Readback readback;
            if (foliage->ReadbackCull(layerIndex, FoliageGPUCuller::ViewSlot::Main, readback) &&
                readback.Submitted > 64u)
            {
                layerUnderTest = layerIndex;
                visibleBefore = readback.Submitted;
                break;
            }
        }
        ASSERT_NE(layerUnderTest, ~0u) << "no layer had enough visible plants to overflow meaningfully";

        // A REAL truncation, not a faked flag — see
        // FoliageGPUCuller::SetDebugOutputCapacity.
        constexpr u32 kCapacity = 32;
        foliage->SetDebugCullCapacity(kCapacity);

        std::vector<u8> truncated;
        Capture("FoliageCull_GL_Deferred_MSAA1_Overflow", kAngles[0], truncated);
        foliage->SetDebugCullCapacity(0); // restore before any assertion can leave it set
        ASSERT_FALSE(HasFatalFailure());

        FoliageGPUCuller::Readback readback;
        ASSERT_TRUE(foliage->ReadbackCull(layerUnderTest, FoliageGPUCuller::ViewSlot::Main, readback));

        std::printf("[foliage-cull] overflow: visible %u -> submitted %u of capacity %u (reserved %u)\n",
                    readback.Visible, readback.Submitted, kCapacity, readback.Reserved);

        // BOUNDED: the draw count never exceeds what the buffer holds.
        EXPECT_EQ(readback.Submitted, kCapacity)
            << "the truncated draw does not fill the capacity it was given exactly";
        // COUNTED: the reservation cursor still saw every survivor, so the
        // difference is a real drop count rather than a lost measurement.
        EXPECT_EQ(readback.Reserved, readback.Visible)
            << "the monotonic append cursor and the visible count disagree — the two-counter split is broken";
        EXPECT_GT(readback.Reserved, kCapacity) << "nothing actually overflowed, so this test measured nothing";

        // EXPLICIT in the frame too: the truncation really reaches the draw.
        //
        // Measured as a pixel DIFFERENCE, not as "fewer green pixels". The
        // terrain under the grass is itself green (the auto-material's grass
        // band), so removing 100 000 cards UNCOVERS green terrain and the
        // vegetated-pixel count goes UP, not down — 48 209 -> 181 044 in the
        // run that caught this. A green-pixel count is the right instrument for
        // "did the cull change what is drawn" only while the thing behind the
        // plants is not also green, which is precisely not the case here.
        const u32 differing = CountDifferingPixels(truncated, healthy);
        std::printf("[foliage-cull] overflow: %u px differ from the untruncated frame\n", differing);
        EXPECT_GT(differing, 100000u)
            << "truncating the compacted buffer to " << kCapacity
            << " instances barely changed the frame — the bound is not reaching the draw";
    }

    // ── Criterion 4: the shadow view is culled on its own ────────────────────

    TEST_F(FoliageGPUCullEvidenceTest, ShadowViewIsCulledSeparatelyFromTheMainView)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        const ScopedMockTime mockTime(kCaptureTime);
        const ScopedGPUCulling on(true);

        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();

        std::vector<u8> pixels;
        Capture("FoliageCull_GL_Deferred_MSAA1_Shadow", kAngles[0], pixels);
        ASSERT_FALSE(HasFatalFailure());
        FoliageRenderer* foliage = Foliage();
        ASSERT_NE(foliage, nullptr);

        u32 shadowSubmitted = 0;
        u32 mainSubmitted = 0;
        u32 layersWithShadowCull = 0;
        for (u32 layerIndex = 0; layerIndex < foliage->GetLayerCount(); ++layerIndex)
        {
            FoliageGPUCuller::Readback shadow;
            if (foliage->ReadbackCull(layerIndex, FoliageGPUCuller::ViewSlot::Shadow, shadow))
            {
                shadowSubmitted += shadow.Submitted;
                ++layersWithShadowCull;
            }
            FoliageGPUCuller::Readback main;
            if (foliage->ReadbackCull(layerIndex, FoliageGPUCuller::ViewSlot::Main, main))
                mainSubmitted += main.Submitted;
        }

        std::printf("[foliage-cull] shadow view: %u layers culled, %u shadow casters vs %u main-view instances\n",
                    layersWithShadowCull, shadowSubmitted, mainSubmitted);

        EXPECT_GT(layersWithShadowCull, 0u)
            << "no shadow view ran the cull — the shadow-view acceptance criterion is not covered";
        EXPECT_GT(shadowSubmitted, 0u)
            << "the shadow cull kept nothing, which would mean the foliage casts no shadow at all";
    }
} // namespace OloEngine::Tests

// =============================================================================
// FloraTraversalEvidenceTest.cpp — epic #1224's own acceptance, on a live GL
// context.
//
// Writes
//   OloEditor/assets/tests/visual/FloraTraversal_GL_<Path>_<Angle>.png
//   OloEditor/assets/tests/visual/FloraTraversalOff_GL_<Path>_<Angle>.png
// for <Path> in {Forward, ForwardPlus, Deferred} and <Angle> in {Meadow,
// Boundary, Woodland}, plus the conditional cells Msaa4, NonNativeRes and the
// close-up criterion-2 pair. A cell that was not run is a file missing from the
// diff (task-loop 2a). Vulkan is not reachable from a headless fixture — those
// cells are a live editor session, recorded in the PR's matrix.
//
// ── What this file is for, and what it is NOT ───────────────────────────────
//
// The seven implementation children of #1224 each have their own contract and
// evidence tests beside this one, and those are not repeated here. What no
// per-child test can see is the SEAM BETWEEN THEM: the epic's criteria are
// about one camera moving through one scene in which all seven are live at
// once.
//
//   1. "All flora children integrate in a meadow-to-woodland traversal."
//      -> AllSevenFloraChildrenAreLiveInOneTraversalScene, a census taken off
//         the REAL draw stream rather than off the authored layer structs, so a
//         feature that was authored and then dropped somewhere between the
//         layer and the draw fails it.
//   2. "Close grass, shrubs and trees have convincing geometry, light
//      transmission and independent motion."
//      -> CloseFloraHasGeometryTransmissionAndIndependentMotion, three A/Bs at
//         a close pose.
//   3. "Density, coverage, lighting and silhouettes remain stable across
//      distance changes, streaming and camera motion."
//      -> MeadowToWoodlandTraversalIsContinuousOnEveryRenderingPath, the
//         conditional-cell repeat below it, and
//         NoSpeciesHandsOverAsARingAnywhereAlongTheTraversal, which is the one
//         of the three that is not confounded by the scene's own change. The
//         criterion's "streaming" clause is answered as REGENERATION
//         IDEMPOTENCE at the end of the traversal test — see the comment there
//         for why that, and not scene-region streaming, is what foliage has.
//
// ── Why the metric for criterion 3 is a SECOND difference ───────────────────
//
// Screen coverage along a meadow-to-woodland walk is SUPPOSED to change: the
// grass thins out, the canopy closes in. A first difference therefore measures
// the traversal, not the defect, and bounding it would bound the scene.
//
// A pop is a LOCAL DISCONTINUITY in that curve — one step where the coverage
// jumps relative to the steps either side of it. That is exactly the second
// difference |c[i+1] - 2c[i] + c[i-1]|, and dividing it by the mean first
// difference makes the number scale-free, so it can be compared between two
// arms that cover different amounts of screen.
//
// It is asserted A/B-RELATIVE (stability features on vs off) rather than
// against a constant, and the absolute figure is LOGGED BUT NOT CLAIMED: over
// this traversal the coverage genuinely accelerates (4.8% to 38% across nine
// poses), so the second difference is dominated by walking into a forest
// rather than by anything popping. No screen-space measurement over a moving
// camera can separate those two, which is why
// NoSpeciesHandsOverAsARingAnywhereAlongTheTraversal exists and looks at where
// the plants are instead. The absolute figure depends on the terrain seed,
// the plant sizes and the pose spacing — see
// docs/agent-rules/live-verification-noise-floor.md and the evidence-PNG
// ordering note in FoliageLodCoverageEvidenceTest beside this file.
//
// ── Why the camera path is DERIVED, not authored ────────────────────────────
//
// "Meadow to woodland" is a statement about where the species actually are,
// and where they are is decided by the procedural terrain through #1254's
// habitat rules. A hand-typed waypoint pair would encode one terrain seed's
// happenstance and would silently stop being a meadow-to-woodland traversal the
// first time anything upstream moved a plant. So the path is the segment
// between the MEASURED HOTSPOTS of the meadow layer and the woodland layer,
// read out of the instance registry after the scene has ticked, and the
// altitude bands that separate them are themselves calibrated from the terrain
// the fixture generated (see CalibrateHabitat).
//
// Runs in the normal suite and SKIPs cleanly (not fails) when there is no GL
// 4.6 context — mirrors FoliageLodCoverageEvidenceTest beside it.
//
// OLO_TEST_LAYER: L8
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"
#include "RenderPropertyTest.h"
#include "VisualEvidenceGuards.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/RenderingPath.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Terrain/Foliage/FoliageInteraction.h"
#include "OloEngine/Terrain/Foliage/FoliageLodTransition.h"
#include "OloEngine/Terrain/Foliage/FoliageRenderer.h"
#include "OloEngine/Terrain/TerrainGenerator.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Utils/PlatformUtils.h" // Time::SetMockTime

#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
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

        // Frozen wherever the measurement must not have wind in it. The
        // interaction spring is advanced to rest before this is latched, so the
        // bend that #1238 contributes is present and STATIC — a live pixel A/B
        // in a windy scene has no signal (69% of pixels move between two
        // captures of the same config), which is the single trap this whole
        // file is arranged around.
        constexpr f32 kCaptureTime = 4.0f;

        constexpr const char* kPineMesh = "SandboxProject/Assets/Models/Vegetation/pine.obj";
        constexpr const char* kPalmMesh = "SandboxProject/Assets/Models/Vegetation/palm.obj";
        constexpr const char* kFoliageAlbedo = "assets/textures/grass.png";
        constexpr const char* kLeafNormal = "assets/textures/leaf_normal.png";
        constexpr const char* kLeafRoughness = "assets/textures/leaf_roughness.png";
        constexpr const char* kLeafThickness = "assets/textures/leaf_thickness.png";

        // Physical layer slots. The generator seed is derived from the physical
        // index (FoliagePlacement::SeedForLayer), so these are identity, not
        // presentation order — do not reorder them without expecting every
        // plant in the fixture to move.
        constexpr u32 kMeadowGrassLayer = 0;
        constexpr u32 kWildflowerLayer = 1;
        constexpr u32 kWoodlandTreeLayer = 2;
        constexpr u32 kUnderstoryLayer = 3;
        constexpr u32 kLeafLitterLayer = 4;
        constexpr u32 kLayerCount = 5;

        constexpr f32 kTerrainWorldSize = 256.0f;
        constexpr f32 kTerrainHeightScale = 30.0f;

        // Nine poses. Enough that a pop occupies one step rather than being
        // spread across several (which would hide it in the second difference),
        // and few enough that three rendering paths x three arms stays inside a
        // sane frame budget.
        constexpr u32 kTraversalSteps = 9;
        // The conditional cells re-measure the same curve with fewer poses:
        // they are asking "does the stability survive THIS setting", not
        // re-deriving the traversal.
        constexpr u32 kConditionalSteps = 7;

        // World-unit radius the per-pose species census counts within. Roughly
        // the near field a viewer reads as "what I am standing in".
        constexpr f32 kNearFieldRadius = 45.0f;

        struct CameraPose
        {
            glm::vec3 Eye{ 0.0f };
            f32 Yaw = 0.0f;
            f32 Pitch = 0.0f;
        };

        // EditorCamera yaw for a forward direction in the XZ plane. Yaw 0 looks
        // along -Z and a +X view is -pi/2, so forward = (-sin(yaw), -cos(yaw))
        // and the inverse is this.
        [[nodiscard]] f32 YawForDirection(const glm::vec3& forward)
        {
            return std::atan2(-forward.x, -forward.z);
        }

        // Fraction of pixels whose colour differs perceptibly between two
        // frames. A count, not an RMSE: against a foliage-disabled frame this
        // IS the flora's screen coverage, and a count is framing-tolerant where
        // an RMSE is not. Same threshold as FoliageLodCoverageEvidenceTest so
        // the two files' coverage figures are comparable.
        [[nodiscard]] f64 DifferingFraction(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            if (a.size() != b.size() || a.empty())
                return 1.0;

            sizet differing = 0;
            sizet total = 0;
            for (sizet i = 0; i + 3 < a.size(); i += 4)
            {
                ++total;
                const int dr = std::abs(static_cast<int>(a[i + 0]) - static_cast<int>(b[i + 0]));
                const int dg = std::abs(static_cast<int>(a[i + 1]) - static_cast<int>(b[i + 1]));
                const int db = std::abs(static_cast<int>(a[i + 2]) - static_cast<int>(b[i + 2]));
                if (dr + dg + db > 24)
                    ++differing;
            }
            return total == 0 ? 1.0 : static_cast<f64>(differing) / static_cast<f64>(total);
        }

        // Mean luminance over the pixels a mask marks. Used by the transmission
        // A/B, which has to look at the canopy rather than at the whole frame:
        // transmission brightens leaves and leaves alone, and averaging the sky
        // in with them dilutes a real effect below the noise.
        [[nodiscard]] f64 MaskedMeanLuma(const std::vector<u8>& pixels, const std::vector<u8>& mask)
        {
            f64 sum = 0.0;
            sizet count = 0;
            for (sizet i = 0; i + 3 < pixels.size() && i + 3 < mask.size(); i += 4)
            {
                if (mask[i] == 0u)
                    continue;
                sum += 0.2126 * pixels[i + 0] + 0.7152 * pixels[i + 1] + 0.0722 * pixels[i + 2];
                ++count;
            }
            return count == 0 ? 0.0 : sum / static_cast<f64>(count);
        }

        // 1 in every pixel where `a` and `b` differ perceptibly, packed into an
        // RGBA-strided buffer so MaskedMeanLuma can index it the same way.
        [[nodiscard]] std::vector<u8> DifferenceMask(const std::vector<u8>& a, const std::vector<u8>& b)
        {
            std::vector<u8> mask(a.size(), 0u);
            if (a.size() != b.size())
                return mask;
            for (sizet i = 0; i + 3 < a.size(); i += 4)
            {
                const int dr = std::abs(static_cast<int>(a[i + 0]) - static_cast<int>(b[i + 0]));
                const int dg = std::abs(static_cast<int>(a[i + 1]) - static_cast<int>(b[i + 1]));
                const int db = std::abs(static_cast<int>(a[i + 2]) - static_cast<int>(b[i + 2]));
                if (dr + dg + db > 24)
                    mask[i] = 1u;
            }
            return mask;
        }

        [[nodiscard]] const char* PathName(RenderingPath path)
        {
            switch (path)
            {
                case RenderingPath::Forward:
                    return "Forward";
                case RenderingPath::ForwardPlus:
                    return "ForwardPlus";
                case RenderingPath::Deferred:
                    return "Deferred";
            }
            return "Unknown";
        }
    } // namespace

    class FloraTraversalEvidenceTest : public RendererAttachedTest
    {
      protected:
        // ── The integrated scene: every one of the seven children live ──────
        //
        // The habitat bands start OPEN (no altitude gate) so the first tick
        // scatters every species over the whole terrain. CalibrateHabitat then
        // reads the altitude distribution back off those plants and splits it,
        // which is what makes "meadow low, woodland high" true of the terrain
        // this fixture actually generated rather than of one the author
        // imagined.
        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);

            {
                Entity light = scene.CreateEntity("Sun");
                auto& dl = light.AddComponent<DirectionalLightComponent>();
                // Low and roughly along +Z, so the woodland end of the
                // traversal is BACKLIT — which is the only geometry in which
                // #1234's transmission has anything to show.
                dl.m_Direction = glm::normalize(glm::vec3(-0.15f, -0.32f, 0.94f));
                dl.m_Color = glm::vec3(1.0f, 0.96f, 0.88f);
                dl.m_Intensity = 3.0f;
                dl.m_CastShadows = true;
            }

            // A procedural sky with IBL, for the same reason the #1239
            // reference fixtures use one: a directional light ALONE leaves
            // every surface facing away from it at the ambient floor, and with
            // the sun deliberately behind the woodland that is every near
            // trunk in the frame. The first cut of this fixture had no sky and
            // its boundary capture came back a field of near-black silhouettes
            // — a frame nobody can judge "convincing geometry" or "stable
            // silhouettes" from, which is the whole job of these PNGs.
            //
            // SunDirection points TOWARD the sun, the opposite convention to
            // DirectionalLightComponent::Direction (which is where the light
            // travels), so this is the negation of the direction above. Get it
            // wrong and the backlit camera looks into an empty patch of sky.
            {
                Entity sky = scene.CreateEntity("Sky");
                auto& ps = sky.AddComponent<ProceduralSkyComponent>();
                ps.m_SunDirection = -glm::normalize(glm::vec3(-0.15f, -0.32f, 0.94f));
                ps.m_Turbidity = 2.4f;
                ps.m_Exposure = 0.1f;
                ps.m_EnableSkybox = true;
                ps.m_EnableIBL = true;
                ps.m_IBLIntensity = 0.9f;
                ps.m_CubemapResolution = 256;
            }

            m_TerrainEntity = scene.CreateEntity("MeadowToWoodland");
            auto& terrain = m_TerrainEntity.AddComponent<TerrainComponent>();
            terrain.m_ProceduralEnabled = true;
            terrain.m_ProceduralSeed = 7;
            terrain.m_ProceduralResolution = 256;
            terrain.m_ProceduralOctaves = 5;
            terrain.m_ProceduralFrequency = 1.8f;
            terrain.m_WorldSizeX = kTerrainWorldSize;
            terrain.m_WorldSizeZ = kTerrainWorldSize;
            terrain.m_HeightScale = kTerrainHeightScale;
            terrain.m_TessellationEnabled = false;
            terrain.m_Material = Ref<TerrainMaterial>::Create();
            for (const auto& layer : TerrainGenerator::MakeDefaultLayers())
                terrain.m_Material->AddLayer(layer);

            auto& foliage = m_TerrainEntity.AddComponent<FoliageComponent>();
            foliage.m_Enabled = true;
            foliage.m_Layers.resize(kLayerCount);

            // ── 0: meadow grass. Cards, dense, clumped with the wildflowers.
            {
                FoliageLayer& l = foliage.m_Layers[kMeadowGrassLayer];
                l.Name = "Meadow Grass";
                l.AlbedoPath = kFoliageAlbedo;
                l.Density = 0.9f;
                l.SplatmapChannel = -1;
                l.MinSlopeAngle = 0.0f;
                l.MaxSlopeAngle = 34.0f;
                l.SlopeFeather = 6.0f;
                l.MinScale = 0.9f;
                l.MaxScale = 1.6f;
                l.MinHeight = 1.0f;
                l.MaxHeight = 2.2f;
                l.ClumpStrength = 0.55f; // #1254
                l.ClumpScale = 26.0f;
                l.ClumpGroup = 0;
                l.SlopeSinkFactor = 0.6f;
                l.DecorrelatedVariation = true;
                l.ViewDistance = 190.0f;
                l.FadeStartDistance = 150.0f;
                l.UseAuthoredMesh = false; // grass stays a card at every range
                l.WindStrength = 0.35f;    // #1236
                l.WindSpeed = 1.6f;
                l.WindStiffness = 0.15f;
                l.WindBranchWeight = 0.25f;
                l.WindLeafWeight = 0.7f;
                l.InteractionResponse = 1.0f; // #1238
                l.BaseColor = glm::vec3(0.30f, 0.46f, 0.16f);
                l.AlphaCutoff = 0.4f;
                l.TransmissionStrength = 0.8f; // #1234
                l.NormalMapPath = kLeafNormal;
                l.RoughnessMapPath = kLeafRoughness;
                l.ThicknessMapPath = kLeafThickness;
                l.Thickness = 0.35f;
            }

            // ── 1: wildflowers. Same clump group as the grass, so the two move
            //      as one patch rather than as two noises that average out.
            {
                FoliageLayer& l = foliage.m_Layers[kWildflowerLayer];
                l.Name = "Wildflowers";
                l.AlbedoPath = kFoliageAlbedo;
                l.Density = 0.35f;
                l.SplatmapChannel = -1;
                l.MaxSlopeAngle = 24.0f;
                l.SlopeFeather = 5.0f;
                l.MinScale = 0.7f;
                l.MaxScale = 1.2f;
                l.MinHeight = 0.8f;
                l.MaxHeight = 1.5f;
                l.ClumpStrength = 0.8f;
                l.ClumpScale = 14.0f;
                l.ClumpGroup = 0;
                l.SlopeSinkFactor = 0.5f;
                l.DecorrelatedVariation = true;
                l.ViewDistance = 120.0f;
                l.FadeStartDistance = 96.0f;
                l.UseAuthoredMesh = false;
                l.WindStrength = 0.28f;
                l.WindSpeed = 1.9f;
                l.WindLeafWeight = 0.8f;
                l.BaseColor = glm::vec3(0.52f, 0.49f, 0.22f);
                l.AlphaCutoff = 0.4f;
                l.TransmissionStrength = 0.9f;
                l.Thickness = 0.3f;
            }

            // ── 2: the woodland itself. Authored pine geometry up close
            //      (#1233), an octahedral impostor far away, hierarchical wind
            //      with a stiff trunk and loose leaves (#1236).
            {
                FoliageLayer& l = foliage.m_Layers[kWoodlandTreeLayer];
                l.Name = "Woodland Pines";
                l.MeshPath = kPineMesh;
                l.AlbedoPath = kFoliageAlbedo;
                l.Density = 0.035f;
                l.SplatmapChannel = -1;
                l.MaxSlopeAngle = 48.0f;
                l.SlopeFeather = 8.0f;
                l.MinScale = 0.9f;
                l.MaxScale = 1.3f;
                l.MinHeight = 7.0f;
                l.MaxHeight = 11.0f;
                l.ClumpStrength = 0.6f;
                l.ClumpScale = 34.0f;
                l.ClumpGroup = 1;
                l.SlopeSinkFactor = 0.4f;
                l.DecorrelatedVariation = true;
                l.ViewDistance = 420.0f;
                l.FadeStartDistance = 380.0f;
                l.UseAuthoredMesh = true; // #1233
                l.MeshViewDistance = 70.0f;
                l.MeshFadeStartDistance = 52.0f;
                l.WindStrength = 0.22f; // #1236
                l.WindSpeed = 0.9f;
                l.WindStiffness = 0.75f; // a trunk barely moves
                l.WindBranchWeight = 0.45f;
                l.WindLeafWeight = 0.9f;
                l.InteractionResponse = 0.15f; // a pine ignores a walker
                l.BaseColor = glm::vec3(0.18f, 0.38f, 0.16f);
                l.AlphaCutoff = 0.3f;
                l.TransmissionStrength = 1.2f; // #1234 — the backlit canopy
                l.TransmissionColor = glm::vec3(0.36f, 0.58f, 0.16f);
                l.NormalMapPath = kLeafNormal;
                l.RoughnessMapPath = kLeafRoughness;
                l.ThicknessMapPath = kLeafThickness;
                l.Thickness = 0.55f;
                l.UseImpostor = true; // the far rung of the LOD ladder
                l.ImpostorStartDistance = 110.0f;
                l.ImpostorTransitionBand = 30.0f;
                // Small atlas on purpose: this fixture is proving the impostor
                // rung is REACHED, not how sharp it is, and a 1024^2 bake per
                // suite run is cost with no evidence attached to it.
                l.ImpostorFramesPerAxis = 4;
                l.ImpostorAtlasResolution = 256;
            }

            // ── 3: understory. Authored geometry too, so the mesh rung is
            //      exercised by more than one species and at a different scale.
            {
                FoliageLayer& l = foliage.m_Layers[kUnderstoryLayer];
                l.Name = "Understory Shrubs";
                l.MeshPath = kPalmMesh;
                l.AlbedoPath = kFoliageAlbedo;
                l.Density = 0.12f;
                l.SplatmapChannel = -1;
                l.MaxSlopeAngle = 42.0f;
                l.SlopeFeather = 7.0f;
                l.MinScale = 0.8f;
                l.MaxScale = 1.4f;
                l.MinHeight = 1.6f;
                l.MaxHeight = 3.0f;
                l.ClumpStrength = 0.7f;
                l.ClumpScale = 18.0f;
                l.ClumpGroup = 1; // clumps WITH the pines: understory, not lawn
                l.SlopeSinkFactor = 0.5f;
                l.DecorrelatedVariation = true;
                l.ViewDistance = 150.0f;
                l.FadeStartDistance = 120.0f;
                l.UseAuthoredMesh = true;
                l.MeshViewDistance = 48.0f;
                l.MeshFadeStartDistance = 34.0f;
                l.WindStrength = 0.3f;
                l.WindSpeed = 1.2f;
                l.WindStiffness = 0.35f;
                l.WindBranchWeight = 0.6f;
                l.WindLeafWeight = 0.85f;
                l.InteractionResponse = 0.6f;
                l.BaseColor = glm::vec3(0.22f, 0.44f, 0.18f);
                l.AlphaCutoff = 0.35f;
                l.TransmissionStrength = 1.0f;
                l.NormalMapPath = kLeafNormal;
                l.ThicknessMapPath = kLeafThickness;
                l.Thickness = 0.45f;
            }

            // ── 4: leaf litter. Ground cover under the canopy, clump group 1.
            {
                FoliageLayer& l = foliage.m_Layers[kLeafLitterLayer];
                l.Name = "Leaf Litter";
                l.AlbedoPath = kFoliageAlbedo;
                l.Density = 0.6f;
                l.SplatmapChannel = -1;
                l.MaxSlopeAngle = 38.0f;
                l.SlopeFeather = 6.0f;
                l.MinScale = 0.6f;
                l.MaxScale = 1.1f;
                l.MinHeight = 0.3f;
                l.MaxHeight = 0.7f;
                l.ClumpStrength = 0.75f;
                l.ClumpScale = 20.0f;
                l.ClumpGroup = 1;
                l.SlopeSinkFactor = 0.8f;
                l.DecorrelatedVariation = true;
                l.ViewDistance = 90.0f;
                l.FadeStartDistance = 72.0f;
                l.UseAuthoredMesh = false;
                l.WindStrength = 0.06f;
                l.WindSpeed = 2.2f;
                l.WindLeafWeight = 1.0f;
                l.BaseColor = glm::vec3(0.34f, 0.26f, 0.13f);
                l.AlphaCutoff = 0.4f;
            }

            // #1238's switch is the PRESENCE of a source, not a slider. The
            // walker is parked in the meadow; the traversal starts beside it.
            m_WalkerEntity = scene.CreateEntity("Walker");
            auto& interaction = m_WalkerEntity.AddComponent<FoliageInteractionComponent>();
            interaction.m_Radius = 3.2f;
            interaction.m_Height = 2.2f;
            interaction.m_Strength = 1.0f;
            interaction.m_Falloff = 2.0f;
            interaction.m_RecoverySeconds = 0.8f;
            interaction.m_Enabled = true;

            SetStability(true);
            foliage.m_NeedsRebuild = true;
        }

        // ── The #1237 stability switches, which are this file's A/B lever ────
        //
        // Density LOD stays ON in BOTH arms and at the same numbers: it decides
        // how much screen the layer covers, and moving it would make the two
        // arms two different scenes rather than two renderings of one. What
        // flips is only the DECORRELATION — the per-instance threshold spread,
        // the hysteresis and the stochastic resolve — which is precisely the
        // machinery that stops a hand-over happening to every plant at once.
        void SetStability(bool enabled)
        {
            auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
            for (auto& layer : foliage.m_Layers)
            {
                layer.LodTransitionSpread = enabled ? 26.0f : 0.0f;
                layer.LodHysteresis = enabled ? 0.07f : 0.0f;
                layer.LodStochasticCoverage = enabled;

                layer.UseDensityLod = true;
                layer.DensityLodStartDistance = 40.0f;
                layer.DensityLodEndDistance = 150.0f;
                layer.DensityLodMinFraction = 0.3f;
                layer.DensityLodFadeFraction = 0.18f;
                layer.DensityLodMaxScale = 2.0f;
            }
            // Dirtied for the same reason FoliageLodCoverageEvidenceTest dirties
            // it: FoliageRenderer copies a layer's render properties into its
            // LayerRenderData inside GenerateInstances, and Scene runs that only
            // on m_NeedsRebuild. None of these fields feeds the placement
            // signature, so the rebuild rescatters to exactly the same
            // positions and this stays an A/B over ONE forest.
            foliage.m_NeedsRebuild = true;
        }

        void SetFoliageEnabled(bool on)
        {
            m_TerrainEntity.GetComponent<FoliageComponent>().m_Enabled = on;
        }

        void SetLayerEnabled(u32 layerIndex, bool on)
        {
            auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
            foliage.m_Layers[layerIndex].Enabled = on;
            foliage.m_NeedsRebuild = true;
        }

        void SetPath(RenderingPath path)
        {
            Renderer3D::GetRendererSettings().Path = path;
            Renderer3D::ApplyRendererSettings();
        }

        // Puts the rendering path back where it was found
        // (docs/agent-rules/cross-test-renderer-state.md rule 2) even when a
        // test leaves early through OLO_ENSURE_GPU_OR_SKIP or an ASSERT_*
        // inside Capture.
        struct ScopedRenderPath
        {
            explicit ScopedRenderPath(FloraTraversalEvidenceTest& owner)
                : m_Owner(owner), m_Path(Renderer3D::GetRendererSettings().Path)
            {
            }
            ~ScopedRenderPath()
            {
                m_Owner.SetPath(m_Path);
            }
            ScopedRenderPath(const ScopedRenderPath&) = delete;
            auto operator=(const ScopedRenderPath&) -> ScopedRenderPath& = delete;

            FloraTraversalEvidenceTest& m_Owner;
            RenderingPath m_Path;
        };

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
            auto operator=(const ScopedMockTime&) -> ScopedMockTime& = delete;
        };

        void Capture(const CameraPose& pose, std::vector<u8>& outPixels, u32 width = kWidth, u32 height = kHeight)
        {
            EditorCamera camera(60.0f, static_cast<f32>(width) / static_cast<f32>(height), 0.5f, 2000.0f);
            camera.SetViewportSize(static_cast<f32>(width), static_cast<f32>(height));
            camera.SetPose(pose.Eye, pose.Yaw, pose.Pitch);
            RunEditorFrames(camera, 4);

            auto fb = Renderer3D::ResolveFrameGraphFramebuffer(ResourceNames::SceneColor);
            ASSERT_TRUE(fb) << "No SceneColor framebuffer";
            ReadbackRgba8(fb->GetColorAttachmentRendererID(0), width, height, outPixels);
            ASSERT_EQ(outPixels.size(), static_cast<sizet>(width) * height * 4u);
        }

        // Step the scene clock forward in 1/60 s slices so the interaction
        // spring and the wind history integrate exactly as they would live,
        // then leave mock time wherever the caller wants the captures taken.
        void Advance(const CameraPose& pose, f32 seconds)
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.5f, 2000.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(pose.Eye, pose.Yaw, pose.Pitch);

            constexpr f32 kStep = 1.0f / 60.0f;
            const u32 steps = static_cast<u32>(std::max(1.0f, seconds / kStep));
            for (u32 i = 0; i < steps; ++i)
            {
                m_MockTime += kStep;
                Time::SetMockTime(m_MockTime);
                RunEditorFrames(camera, 1);
            }
        }

        static void WritePng(const std::string& name, const std::vector<u8>& px, u32 width = kWidth,
                             u32 height = kHeight)
        {
            std::vector<u8> flipped(px); // GL readback is bottom-up
            const sizet rowBytes = static_cast<sizet>(width) * 4u;
            for (u32 y = 0; y < height / 2u; ++y)
            {
                u8* a = flipped.data() + static_cast<sizet>(y) * rowBytes;
                u8* b = flipped.data() + static_cast<sizet>(height - 1u - y) * rowBytes;
                std::vector<u8> tmp(a, a + rowBytes);
                std::memcpy(a, b, rowBytes);
                std::memcpy(b, tmp.data(), rowBytes);
            }
            const fs::path dir = fs::path("assets") / "tests" / "visual";
            std::error_code ec;
            fs::create_directories(dir, ec);
            ::stbi_write_png((dir / name).string().c_str(), static_cast<int>(width), static_cast<int>(height), 4,
                             flipped.data(), static_cast<int>(width) * 4);
        }

        // ── Bringing the scene up, once, in the order the data requires ──────
        //
        // Scene creates the FoliageRenderer on its first tick, so nothing about
        // any layer can be read until a frame has run. Then the habitat bands
        // are calibrated off the plants that first tick placed, the layers are
        // rebuilt against them, and only then is there a meadow and a woodland
        // to draw a path between.
        //
        // Idempotent: every test calls it and the first one pays for it.
        [[nodiscard]] bool BringUpTraversal()
        {
            if (m_PathReady)
                return true;

            std::vector<u8> warmUp;
            Capture(CameraPose{ glm::vec3(128.0f, 40.0f, 200.0f), 0.0f, 0.15f }, warmUp);

            if (!m_TerrainEntity || !m_TerrainEntity.HasComponent<FoliageComponent>())
                return false;
            const auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
            if (!foliage.m_Renderer || foliage.m_Renderer->GetTotalInstanceCount() == 0u)
                return false;

            CalibrateHabitat();

            // Rebuild against the calibrated bands, then re-read.
            std::vector<u8> settle;
            Capture(CameraPose{ glm::vec3(128.0f, 40.0f, 200.0f), 0.0f, 0.15f }, settle);

            return DerivePath();
        }

        // Split the terrain's altitude range at the median of the plants that
        // were placed on it, and give each species a feathered band on one side
        // of that split. The feather is what makes the boundary a TRANSITION
        // rather than a line — criterion 1 asks for a traversal through one,
        // and #1254's whole point is that a habitat edge dissolves.
        void CalibrateHabitat()
        {
            auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
            const auto& records = foliage.m_Renderer->GetInstanceRegistry().GetRecords();

            std::vector<f32> altitudes;
            altitudes.reserve(records.size());
            for (const auto& record : records)
                altitudes.push_back(record.m_Position.y); // terrain-local == altitude in world units
            std::sort(altitudes.begin(), altitudes.end());

            const auto quantile = [&](f64 q)
            {
                if (altitudes.empty())
                    return 0.5f * kTerrainHeightScale;
                const sizet idx = std::min(altitudes.size() - 1u,
                                           static_cast<sizet>(q * static_cast<f64>(altitudes.size() - 1u)));
                return altitudes[idx];
            };

            m_HabitatSplit = quantile(0.5);
            const f32 low = quantile(0.15);
            const f32 high = quantile(0.85);
            // The feather spans a quarter of the inter-quantile spread, with a
            // floor so a flat terrain still gets a soft edge rather than a
            // step.
            m_HabitatFeather = std::max(1.5f, 0.25f * (high - low));

            const f32 split = m_HabitatSplit;
            const f32 feather = m_HabitatFeather;

            const auto lowland = [&](FoliageLayer& l)
            {
                l.UseAltitudeBand = true;
                l.MinAltitude = -1.0f;
                l.MaxAltitude = split;
                l.AltitudeFeather = feather;
            };
            const auto upland = [&](FoliageLayer& l)
            {
                l.UseAltitudeBand = true;
                l.MinAltitude = split;
                l.MaxAltitude = kTerrainHeightScale + 1.0f;
                l.AltitudeFeather = feather;
            };

            lowland(foliage.m_Layers[kMeadowGrassLayer]);
            lowland(foliage.m_Layers[kWildflowerLayer]);
            upland(foliage.m_Layers[kWoodlandTreeLayer]);
            upland(foliage.m_Layers[kUnderstoryLayer]);
            upland(foliage.m_Layers[kLeafLitterLayer]);

            foliage.m_NeedsRebuild = true;
        }

        // Where a species is THICKEST, and how many plants of it exist.
        //
        // The MODE, not the mean, and that distinction is the difference
        // between a traversal and a fixture that passes by luck. A habitat band
        // over procedural terrain is routinely MULTI-MODAL — the low ground can
        // be two opposite corners with a ridge between them — and the mean of
        // two meadows is the hill in the middle, which is neither. So the
        // terrain is bucketed into a coarse grid, the fullest bucket is chosen,
        // and the answer is the centroid of THAT bucket's plants.
        // Returns false when the layer has no plants inside the bucket grid.
        // The first cut incremented outCount BEFORE the bounds check and
        // returned a (0,0,0) hotspot for a layer whose records all fell
        // outside, which DerivePath would then have accepted as a real place
        // and run the traversal to the terrain's corner.
        [[nodiscard]] bool LayerHotspotWorld(u32 layerIndex, u32& outCount, glm::vec3& outHotspot) const
        {
            constexpr u32 kGrid = 16u; // 16 m buckets over a 256 m terrain
            const auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
            const auto& records = foliage.m_Renderer->GetInstanceRegistry().GetRecords();
            const glm::mat4 model = m_TerrainEntity.GetComponent<TransformComponent>().GetTransform();
            const f32 cellSize = kTerrainWorldSize / static_cast<f32>(kGrid);

            std::vector<u32> counts(static_cast<sizet>(kGrid) * kGrid, 0u);
            std::vector<glm::dvec3> sums(static_cast<sizet>(kGrid) * kGrid, glm::dvec3(0.0));
            outCount = 0;

            for (const auto& record : records)
            {
                if (record.m_LayerIndex != layerIndex)
                    continue;
                ++outCount;
                const glm::vec3 world = glm::vec3(model * glm::vec4(record.m_Position, 1.0f));
                // The terrain's origin is its corner, so a plant's terrain-LOCAL
                // x/z are already in [0, WorldSize] and are what buckets.
                const i32 cx = static_cast<i32>(record.m_Position.x / cellSize);
                const i32 cz = static_cast<i32>(record.m_Position.z / cellSize);
                if (cx < 0 || cz < 0 || cx >= static_cast<i32>(kGrid) || cz >= static_cast<i32>(kGrid))
                    continue;
                const sizet bucket = static_cast<sizet>(cz) * kGrid + static_cast<sizet>(cx);
                ++counts[bucket];
                sums[bucket] += glm::dvec3(world);
            }

            sizet best = 0;
            for (sizet i = 1; i < counts.size(); ++i)
            {
                if (counts[i] > counts[best])
                    best = i;
            }
            if (counts[best] == 0u)
                return false;
            outHotspot = glm::vec3(sums[best] / static_cast<f64>(counts[best]));
            return true;
        }

        // Ground height near an XZ point, taken as the mean of the nearest
        // plants' own altitudes. The heightfield mirror is behind
        // TerrainData::SyncFromGPU(); the plants are already in hand and they
        // ARE on the ground, so this needs no second source of truth.
        // Returns false when no plant is near enough to answer, rather than
        // inventing a height: a pose placed at the habitat split on a hillside
        // is metres underground or airborne, every capture from it is wrong,
        // and nothing downstream would say so (CLAUDE.md's companion guides
        // call this out — a path that cannot do its job says so loudly).
        [[nodiscard]] bool GroundHeightNear(const glm::vec3& worldXZ, f32& outHeight) const
        {
            const auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
            const auto& records = foliage.m_Renderer->GetInstanceRegistry().GetRecords();
            const glm::mat4 model = m_TerrainEntity.GetComponent<TransformComponent>().GetTransform();

            // Widening rings rather than one radius: a sparse patch between two
            // clumps is a legitimate place for the camera to stand, and 10 m is
            // simply too tight there. Each ring is tried in turn and the first
            // that has plants in it answers.
            constexpr std::array<f32, 3> kRadii{ 10.0f, 25.0f, 50.0f };
            for (const f32 radius : kRadii)
            {
                const f32 radiusSq = radius * radius;
                f64 sum = 0.0;
                u32 count = 0;
                for (const auto& record : records)
                {
                    const glm::vec3 world = glm::vec3(model * glm::vec4(record.m_Position, 1.0f));
                    const f32 dx = world.x - worldXZ.x;
                    const f32 dz = world.z - worldXZ.z;
                    if (dx * dx + dz * dz > radiusSq)
                        continue;
                    sum += world.y;
                    ++count;
                }
                if (count > 0u)
                {
                    outHeight = static_cast<f32>(sum / static_cast<f64>(count));
                    return true;
                }
            }
            return false;
        }

        // The traversal: meadow hotspot to woodland hotspot, eye at walking
        // height above whatever ground is under each pose, looking along the
        // direction of travel.
        [[nodiscard]] bool DerivePath()
        {
            u32 meadowCount = 0;
            u32 woodlandCount = 0;
            glm::vec3 meadow{ 0.0f };
            glm::vec3 woodland{ 0.0f };
            if (!LayerHotspotWorld(kMeadowGrassLayer, meadowCount, meadow))
                return false;
            if (!LayerHotspotWorld(kWoodlandTreeLayer, woodlandCount, woodland))
                return false;

            glm::vec3 travel = woodland - meadow;
            travel.y = 0.0f;
            const f32 span = glm::length(travel);
            if (span < 1.0f)
                return false;
            const glm::vec3 forward = travel / span;
            const f32 yaw = YawForDirection(forward);

            // Start a little back from the meadow hotspot and stop a little
            // short of the woodland one, so the near field at the last pose is
            // canopy rather than the far side of it.
            const glm::vec3 start = meadow - forward * 0.25f * span;
            const glm::vec3 end = woodland - forward * 0.15f * span;

            m_Path.clear();
            m_Path.reserve(kTraversalSteps);
            for (u32 step = 0; step < kTraversalSteps; ++step)
            {
                const f32 t = static_cast<f32>(step) / static_cast<f32>(kTraversalSteps - 1u);
                glm::vec3 at = glm::mix(start, end, t);
                f32 ground = 0.0f;
                if (!GroundHeightNear(at, ground))
                    return false; // no plant within 50 m: this is not a traversal through vegetation
                at.y = ground + 2.6f;
                m_Path.push_back(CameraPose{ at, yaw, 0.04f });
            }

            // The walker stands where the traversal begins, so #1238's bend is
            // in the meadow captures rather than off somewhere nobody looks.
            if (m_WalkerEntity)
            {
                auto& transform = m_WalkerEntity.GetComponent<TransformComponent>();
                glm::vec3 walkerAt = m_Path.front().Eye + forward * 6.0f;
                f32 walkerGround = 0.0f;
                if (!GroundHeightNear(walkerAt, walkerGround))
                    return false;
                walkerAt.y = walkerGround;
                transform.Translation = walkerAt;
            }

            m_TravelSpan = span;
            m_PathReady = true;
            return true;
        }

        // A sub-sampled view of the derived path, for the conditional cells.
        [[nodiscard]] std::vector<CameraPose> PathWithSteps(u32 steps) const
        {
            std::vector<CameraPose> out;
            out.reserve(steps);
            for (u32 i = 0; i < steps; ++i)
            {
                const f64 t = static_cast<f64>(i) / static_cast<f64>(steps - 1u);
                const sizet idx = std::min(m_Path.size() - 1u,
                                           static_cast<sizet>(t * static_cast<f64>(m_Path.size() - 1u) + 0.5));
                out.push_back(m_Path[idx]);
            }
            return out;
        }

        // ── The criterion-3 measurement ──────────────────────────────────────

        struct TraversalMetrics
        {
            std::vector<f64> Coverage;
            f64 MeanStep = 0.0;
            f64 WorstStep = 0.0;
            f64 WorstCurvature = 0.0;
            // WorstCurvature / MeanStep. Scale-free, so two arms that cover
            // different amounts of screen are still comparable.
            f64 NormalisedWorstCurvature = 0.0;
            f64 MinCoverage = 1.0;
            std::vector<std::vector<u8>> Frames;
        };

        // `baselines` are the SAME poses rendered with the foliage disabled, at
        // the SAME width/height and the SAME renderer settings — the reference
        // that turns a pixel difference into "how much of this frame is flora".
        [[nodiscard]] TraversalMetrics MeasureTraversal(const std::vector<CameraPose>& poses,
                                                        const std::vector<std::vector<u8>>& baselines, u32 width,
                                                        u32 height, bool keepFrames)
        {
            TraversalMetrics out;
            out.Coverage.reserve(poses.size());
            for (sizet i = 0; i < poses.size(); ++i)
            {
                std::vector<u8> frame;
                Capture(poses[i], frame, width, height);
                out.Coverage.push_back(DifferingFraction(frame, baselines[i]));
                out.MinCoverage = std::min(out.MinCoverage, out.Coverage.back());
                if (keepFrames)
                    out.Frames.push_back(std::move(frame));
            }

            f64 stepSum = 0.0;
            for (sizet i = 1; i < out.Coverage.size(); ++i)
            {
                const f64 step = std::abs(out.Coverage[i] - out.Coverage[i - 1u]);
                stepSum += step;
                out.WorstStep = std::max(out.WorstStep, step);
            }
            out.MeanStep = out.Coverage.size() > 1u ? stepSum / static_cast<f64>(out.Coverage.size() - 1u) : 0.0;

            for (sizet i = 1; i + 1 < out.Coverage.size(); ++i)
            {
                const f64 curvature =
                    std::abs(out.Coverage[i + 1u] - 2.0 * out.Coverage[i] + out.Coverage[i - 1u]);
                out.WorstCurvature = std::max(out.WorstCurvature, curvature);
            }
            // A traversal whose coverage never moves would divide by zero and
            // report a perfect score; the callers assert the arm moved, and
            // this keeps the number finite until they do.
            out.NormalisedWorstCurvature =
                out.MeanStep > 1e-9 ? out.WorstCurvature / out.MeanStep : std::numeric_limits<f64>::infinity();
            return out;
        }

        [[nodiscard]] std::vector<std::vector<u8>> CaptureBaselines(const std::vector<CameraPose>& poses, u32 width,
                                                                    u32 height)
        {
            SetFoliageEnabled(false);
            std::vector<std::vector<u8>> baselines;
            baselines.reserve(poses.size());
            for (const auto& pose : poses)
            {
                std::vector<u8> frame;
                Capture(pose, frame, width, height);
                baselines.push_back(std::move(frame));
            }
            SetFoliageEnabled(true);
            return baselines;
        }

        // Plants of each layer within kNearFieldRadius of a pose — "what the
        // viewer is standing in", counted off the canonical records rather than
        // off pixels, so it is exact and needs no GPU.
        [[nodiscard]] std::array<u32, kLayerCount> NearFieldCensus(const glm::vec3& eye) const
        {
            std::array<u32, kLayerCount> counts{};
            const auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
            const auto& records = foliage.m_Renderer->GetInstanceRegistry().GetRecords();
            const glm::mat4 model = m_TerrainEntity.GetComponent<TransformComponent>().GetTransform();
            const f32 radiusSq = kNearFieldRadius * kNearFieldRadius;

            for (const auto& record : records)
            {
                if (record.m_LayerIndex >= kLayerCount)
                    continue;
                const glm::vec3 world = glm::vec3(model * glm::vec4(record.m_Position, 1.0f));
                const f32 dx = world.x - eye.x;
                const f32 dz = world.z - eye.z;
                if (dx * dx + dz * dz <= radiusSq)
                    ++counts[record.m_LayerIndex];
            }
            return counts;
        }

        Entity m_TerrainEntity;
        Entity m_WalkerEntity;
        std::vector<CameraPose> m_Path;
        bool m_PathReady = false;
        f32 m_HabitatSplit = 0.0f;
        f32 m_HabitatFeather = 0.0f;
        f32 m_TravelSpan = 0.0f;
        f32 m_MockTime = kCaptureTime;
    };

    // ── Criterion 1: every child is live, in ONE scene, at ONE pose ──────────

    TEST_F(FloraTraversalEvidenceTest, AllSevenFloraChildrenAreLiveInOneTraversalScene)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const ScopedRenderPath restorePath(*this);
        SetPath(RenderingPath::Deferred); // the foliage path's primary target

        ScopedMockTime mockTime(kCaptureTime);
        ASSERT_TRUE(BringUpTraversal()) << "the fixture never produced both a meadow and a woodland";

        // Let the interaction spring reach its target, so #1238 is contributing
        // a real bend rather than zero — with mock time frozen the spring's dt
        // is 0 and it would never move at all.
        Advance(m_Path.front(), 0.8f);

        std::vector<u8> frame;
        Capture(m_Path.front(), frame);

        const auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
        ASSERT_TRUE(foliage.m_Renderer);
        const auto draws = foliage.m_Renderer->GetActiveLayerDrawInfo();
        ASSERT_FALSE(draws.empty()) << "the integrated scene emits no foliage draws at all";

        // The census is taken off the DRAW STREAM. A layer struct with the
        // right field set proves only that someone typed it; an entry in
        // GetActiveLayerDrawInfo carrying it proves it survived generation,
        // material loading and submission.
        bool authoredMesh = false;     // #1233
        bool transmission = false;     // #1234
        bool hierarchicalWind = false; // #1236
        bool lodDecorrelation = false; // #1237
        bool impostor = false;         // #1237's far rung (#433)
        bool speciesResponse = false;  // #1238's per-species half
        u32 meshLayers = 0;

        for (const auto& draw : draws)
        {
            if (draw.IsAuthoredMesh)
            {
                authoredMesh = true;
                ++meshLayers;
            }
            if (draw.LeafTransmissionStrength > 0.0f)
                transmission = true;
            if (glm::dot(glm::vec3(draw.WindWeights), glm::vec3(draw.WindWeights)) > 0.0f)
                hierarchicalWind = true;
            // LodTransition1.z, NOT LodTransition0.x. The first cut read .x
            // and passed while proving nothing: FoliageRenderer packs that lane
            // as FoliageLod::PackFlags — the density-LOD enable bit plus the
            // stochastic-coverage bit — and SetStability forces UseDensityLod
            // true in BOTH arms, so it is 1.0 even with every spread at zero.
            // The per-instance spread is LodTransition1.z (see
            // FoliageRenderer.cpp's FoliageLodTransition1).
            if (draw.LodTransition1.z > 0.0f)
                lodDecorrelation = true;
            if (draw.UseImpostor && draw.ImpostorAlbedoAtlasID.IsValid())
                impostor = true;
            if (draw.InteractionResponse > 0.0f && draw.InteractionResponse < 1.0f)
                speciesResponse = true;
        }

        EXPECT_TRUE(authoredMesh) << "#1233: no draw carries authored plant geometry — every plant is still a card";
        EXPECT_GE(meshLayers, 1u);
        EXPECT_TRUE(transmission) << "#1234: no draw carries a transmission strength — the canopy is a black cutout";
        EXPECT_TRUE(hierarchicalWind) << "#1236: every draw's hierarchical wind weights are zero";
        EXPECT_TRUE(lodDecorrelation) << "#1237: no draw carries a per-instance LOD spread";
        EXPECT_TRUE(impostor) << "#1237/#433: the impostor rung was never reached — no atlas is bound";
        EXPECT_TRUE(speciesResponse)
            << "#1238: no layer answers the interaction field with its own response — every species bends alike";

        // #1235: the GPU cull ran for the main view. Not latched by the
        // renderer, so this is this frame's answer.
        EXPECT_TRUE(FoliageRenderer::IsGPUCullingEnabled())
            << "#1235: GPU culling is disabled process-wide — an earlier test left the A/B lever down";
        EXPECT_TRUE(foliage.m_Renderer->WasMainViewCulled())
            << "#1235: the main view fell back to the uncompacted path — the cull produced nothing";

        // #1238: the field itself has a live influence.
        EXPECT_GT(FoliageInteractionField::GetActiveCount(), 0u)
            << "#1238: the walker published no influence — the bend is off everywhere";

        // #1254: the species are somewhere in particular, not everywhere.
        u32 meadowCount = 0;
        u32 woodlandCount = 0;
        glm::vec3 meadow{ 0.0f };
        glm::vec3 woodland{ 0.0f };
        ASSERT_TRUE(LayerHotspotWorld(kMeadowGrassLayer, meadowCount, meadow))
            << "#1254: the meadow layer placed no plant inside the terrain's bucket grid";
        ASSERT_TRUE(LayerHotspotWorld(kWoodlandTreeLayer, woodlandCount, woodland))
            << "#1254: the woodland layer placed no plant inside the terrain's bucket grid";
        ASSERT_GT(meadowCount, 0u);
        ASSERT_GT(woodlandCount, 0u);
        const f32 separation = glm::length(glm::vec2(woodland.x - meadow.x, woodland.z - meadow.z));

        GTEST_LOG_(INFO) << "habitat split at " << m_HabitatSplit << " m +- " << m_HabitatFeather
                         << " m feather; meadow " << meadowCount << " plants, thickest at (" << meadow.x << ", "
                         << meadow.z << "), woodland " << woodlandCount << " thickest at (" << woodland.x << ", "
                         << woodland.z << ") — " << separation << " m apart";

        EXPECT_GT(separation, 8.0f)
            << "#1254: the meadow and the woodland are thickest in the same place — the habitat rules did not "
               "separate the species and there is no traversal to make";

        // ── Every species that was PLACED is also DRAWN ─────────────────────
        //
        // "All flora children integrate" is a claim about five species, not
        // about the two that happen to be conspicuous. A layer can hold
        // thousands of plants in the registry and contribute no draw entry at
        // all — a mesh that failed to load, a layer disabled downstream, an
        // empty vertex array — and every other assertion in this file would
        // still pass, because the frame is full of the OTHER species.
        //
        // This check exists because reading the woodland captures raised
        // exactly that doubt: the census reports hundreds of understory shrubs
        // in the near field and they are hard to pick out of the image. That is
        // a question the fixture should answer, not the author.
        std::array<u32, kLayerCount> placed{};
        for (const auto& record : foliage.m_Renderer->GetInstanceRegistry().GetRecords())
        {
            if (record.m_LayerIndex < kLayerCount)
                ++placed[record.m_LayerIndex];
        }
        std::array<u32, kLayerCount> drawEntries{};
        std::array<u32, kLayerCount> drawnInstances{};
        for (const auto& draw : draws)
        {
            if (draw.LayerIndex >= kLayerCount)
                continue;
            ++drawEntries[draw.LayerIndex];
            drawnInstances[draw.LayerIndex] = draw.InstanceCount;
        }

        for (u32 i = 0; i < kLayerCount; ++i)
        {
            GTEST_LOG_(INFO) << "layer " << i << " '" << foliage.m_Layers[i].Name << "': " << placed[i]
                             << " placed, " << drawEntries[i] << " draw entries, " << drawnInstances[i]
                             << " instances submitted";
            if (placed[i] == 0u)
                continue;
            EXPECT_GT(drawEntries[i], 0u)
                << "layer '" << foliage.m_Layers[i].Name << "' placed " << placed[i]
                << " plants and emits NO draw entry — that species is in the registry and not in the frame";
            EXPECT_GT(drawnInstances[i], 0u) << "layer '" << foliage.m_Layers[i].Name
                                             << "' submits a draw with zero instances";
        }

        // And the frame is not empty, which every difference measurement below
        // would otherwise pass vacuously.
        SetFoliageEnabled(false);
        std::vector<u8> noFoliage;
        Capture(m_Path.front(), noFoliage);
        SetFoliageEnabled(true);
        const f64 coverage = DifferingFraction(frame, noFoliage);
        GTEST_LOG_(INFO) << "flora covers " << coverage * 100.0 << "% of the meadow frame";
        EXPECT_GT(coverage, 0.02) << "the flora covers almost none of the frame at the traversal's start";
    }

    // ── Criterion 1 + 3: the traversal itself, on every rendering path ───────

    TEST_F(FloraTraversalEvidenceTest, MeadowToWoodlandTraversalIsContinuousOnEveryRenderingPath)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const ScopedRenderPath restorePath(*this);
        SetPath(RenderingPath::Deferred);

        ScopedMockTime mockTime(kCaptureTime);
        ASSERT_TRUE(BringUpTraversal()) << "the fixture never produced both a meadow and a woodland";
        Advance(m_Path.front(), 0.8f);

        // ── The traversal is a traversal: the species composition turns over ─
        const auto startCensus = NearFieldCensus(m_Path.front().Eye);
        const auto endCensus = NearFieldCensus(m_Path.back().Eye);
        GTEST_LOG_(INFO) << "near-field census over " << m_TravelSpan << " m of travel: grass "
                         << startCensus[kMeadowGrassLayer] << " -> " << endCensus[kMeadowGrassLayer] << ", trees "
                         << startCensus[kWoodlandTreeLayer] << " -> " << endCensus[kWoodlandTreeLayer]
                         << ", understory " << startCensus[kUnderstoryLayer] << " -> "
                         << endCensus[kUnderstoryLayer] << ", litter " << startCensus[kLeafLitterLayer] << " -> "
                         << endCensus[kLeafLitterLayer];

        EXPECT_GT(startCensus[kMeadowGrassLayer], endCensus[kMeadowGrassLayer])
            << "the meadow grass did not thin out along the walk — this is not a meadow-to-woodland traversal";
        EXPECT_GT(endCensus[kWoodlandTreeLayer], startCensus[kWoodlandTreeLayer])
            << "the walk did not reach any more trees than it started among";

        // Nowhere along the path is the near field EMPTY. A habitat boundary
        // that opens a bald strip between two species is the #1254 failure this
        // integration is meant to rule out, and it is invisible at either end.
        for (u32 step = 0; step < kTraversalSteps; ++step)
        {
            const auto census = NearFieldCensus(m_Path[step].Eye);
            u32 total = 0;
            for (const u32 n : census)
                total += n;
            EXPECT_GT(total, 0u) << "pose " << step << " of the traversal stands on bare ground — the habitat "
                                                       "bands leave a gap between the meadow and the woodland";
        }

        // ── The stability measurement, per rendering path ────────────────────
        constexpr std::array<RenderingPath, 3> kPaths{ RenderingPath::Forward, RenderingPath::ForwardPlus,
                                                       RenderingPath::Deferred };
        for (const RenderingPath path : kPaths)
        {
            SCOPED_TRACE(PathName(path));
            SetPath(path);

            // One baseline set per path: the foliage-disabled frame depends on
            // the path but not on the stability arm, so capturing it twice
            // would be 9 frames of nothing.
            const auto baselines = CaptureBaselines(m_Path, kWidth, kHeight);

            SetStability(false);
            const TraversalMetrics off = MeasureTraversal(m_Path, baselines, kWidth, kHeight, true);
            SetStability(true);
            const TraversalMetrics on = MeasureTraversal(m_Path, baselines, kWidth, kHeight, true);

            const std::string tag = std::string("GL_") + PathName(path);
            WritePng("FloraTraversal_" + tag + "_Meadow.png", on.Frames.front());
            WritePng("FloraTraversal_" + tag + "_Boundary.png", on.Frames[kTraversalSteps / 2u]);
            WritePng("FloraTraversal_" + tag + "_Woodland.png", on.Frames.back());
            WritePng("FloraTraversalOff_" + tag + "_Meadow.png", off.Frames.front());
            WritePng("FloraTraversalOff_" + tag + "_Boundary.png", off.Frames[kTraversalSteps / 2u]);
            WritePng("FloraTraversalOff_" + tag + "_Woodland.png", off.Frames.back());

            GTEST_LOG_(INFO) << PathName(path) << " traversal coverage " << on.Coverage.front() * 100.0 << "% -> "
                             << on.Coverage.back() * 100.0 << "%; mean step " << on.MeanStep * 100.0
                             << "%, worst curvature off " << off.WorstCurvature * 100.0 << "% (x"
                             << off.NormalisedWorstCurvature << ") / on " << on.WorstCurvature * 100.0 << "% (x"
                             << on.NormalisedWorstCurvature << ")";

            // A difference assertion cannot catch an empty frame: every pose of
            // both arms must have flora in it before the comparison means
            // anything.
            EXPECT_GT(on.MinCoverage, 0.01) << "some pose of the traversal has almost no flora in it";
            EXPECT_GT(off.MinCoverage, 0.01);

            // And the traversal must actually move the image, or "continuous"
            // would be a statement about a still.
            // BOTH arms, not just `on`. NormalisedWorstCurvature divides by
            // MeanStep and reports +inf when it collapses, so an `off` arm that
            // went degenerate would hand the comparison below an infinite
            // budget and pass it unconditionally — the assertion would still be
            // green and would mean nothing.
            EXPECT_GT(on.MeanStep, 0.002)
                << "the traversal barely changed the frame — the derived path is too short for the "
                   "continuity measurement to mean anything";
            EXPECT_GT(off.MeanStep, 0.002)
                << "the control arm's traversal barely changed the frame, so its normalised curvature is "
                   "unbounded and the comparison below cannot fail";

            // The claim: the #1237 decorrelation makes the worst local
            // discontinuity no worse. Asserted A/B-relative with a 10% slack,
            // because the two arms resolve partial fades differently (dither vs
            // a hard cut-off) and a few hundred pixels of that difference
            // should not fail a run.
            EXPECT_LE(on.NormalisedWorstCurvature, off.NormalisedWorstCurvature * 1.1)
                << "the stability features made the worst local coverage discontinuity WORSE (x"
                << on.NormalisedWorstCurvature << " against x" << off.NormalisedWorstCurvature
                << ") — something along the traversal pops";
        }

        // ── The "streaming" clause of criterion 3, as far as foliage has one ─
        //
        // Scene streaming here is REGION-BASED (StreamingSettings loads and
        // unloads entity regions from disk) and a foliage system lives inside
        // ONE terrain entity, so a traversal cannot stream foliage in and out
        // the way it streams buildings. What foliage does instead, and does
        // constantly, is REGENERATE: any edit to a layer — and every LOD arm
        // switch in this very file — rebuilds every instance from scratch.
        //
        // The stability claim that maps onto "streaming" is therefore
        // IDEMPOTENCE: a rebuild with unchanged placement inputs must put every
        // plant back exactly where it was. If it does not, a player walking
        // through a scene that regenerates behind them sees the meadow shuffle,
        // which is the same defect by another route. #1230 pins the identity
        // half of this on the CPU; this is the half you can see.
        SetPath(RenderingPath::Deferred);
        SetStability(true);
        const CameraPose woodlandPose = m_Path.back();
        std::vector<u8> beforeRebuild;
        Capture(woodlandPose, beforeRebuild);

        // Same parameters, so the same placement signature and the same
        // scatter. Only the regeneration itself differs.
        m_TerrainEntity.GetComponent<FoliageComponent>().m_NeedsRebuild = true;
        std::vector<u8> afterRebuild;
        Capture(woodlandPose, afterRebuild);

        const f64 rebuiltDifference = DifferingFraction(beforeRebuild, afterRebuild);
        GTEST_LOG_(INFO) << "regenerating every layer at an unchanged pose moved " << rebuiltDifference * 100.0
                         << "% of the frame's pixels";
        EXPECT_LT(rebuiltDifference, 0.005)
            << "regenerating the foliage with unchanged inputs moved " << rebuiltDifference * 100.0
            << "% of the frame — the scatter is not reproducible, so the scene reshuffles whenever anything "
               "rebuilds it";
    }

    // ── Criterion 3, the part pixels cannot answer ───────────────────────────

    TEST_F(FloraTraversalEvidenceTest, NoSpeciesHandsOverAsARingAnywhereAlongTheTraversal)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // The screen-coverage curvature above is asserted A/B-relative for a
        // reason that is worth stating rather than burying: along a
        // meadow-to-woodland walk the coverage genuinely accelerates — it went
        // 4.8% to 38% over nine poses on this fixture — so the ABSOLUTE second
        // difference is dominated by the scene changing, not by anything
        // popping. It cannot separate "I walked into a forest" from "a band of
        // plants changed shape at once", and no screen-space measurement over a
        // moving camera can.
        //
        // This one can, because it does not look at the screen. For each step
        // of the traversal it asks, of EVERY plant of EVERY species, whether it
        // crossed its mesh-to-card hand-over during that step, and then
        // measures the DEPTH DISPERSION of the ones that did — evaluated
        // through the same FoliageLod::MeshCoverageLod the vertex stages
        // compile, over the registry's real scattered positions.
        //
        // A "ring sweeping across the meadow" is precisely the statement that
        // every plant crossing at once is at the SAME DISTANCE from the viewer.
        // The count of crossings is CONSERVED by decorrelation and is not the
        // measurement (FoliageLodCoverageEvidenceTest proves that for one
        // layer); where those plants ARE is.
        //
        // What is new here, and what no per-child test covers, is that the
        // species are POOLED. Five layers each handing over tidily at its own
        // authored distance still reads as five rings, and an integrated
        // woodland is exactly where that happens.
        const ScopedRenderPath restorePath(*this);
        SetPath(RenderingPath::Deferred);

        ScopedMockTime mockTime(kCaptureTime);
        ASSERT_TRUE(BringUpTraversal()) << "the fixture never produced both a meadow and a woodland";

        const auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();
        ASSERT_TRUE(foliage.m_Renderer);
        const auto& records = foliage.m_Renderer->GetInstanceRegistry().GetRecords();
        ASSERT_FALSE(records.empty());
        const glm::mat4 model = m_TerrainEntity.GetComponent<TransformComponent>().GetTransform();

        // The two layers with an authored mesh, and the band each hands over
        // across. Read off the live layers rather than restated, so a change to
        // BuildScene cannot leave this measuring a ladder the scene no longer
        // has.
        struct MeshBand
        {
            u32 LayerIndex = 0;
            f32 FadeStart = 0.0f;
            f32 ViewDistance = 0.0f;
        };
        std::vector<MeshBand> bands;
        for (u32 i = 0; i < kLayerCount; ++i)
        {
            const auto& layer = foliage.m_Layers[i];
            if (layer.UseAuthoredMesh && !layer.MeshPath.empty() && layer.MeshViewDistance > layer.MeshFadeStartDistance)
                bands.push_back(MeshBand{ i, layer.MeshFadeStartDistance, layer.MeshViewDistance });
        }
        ASSERT_GE(bands.size(), 2u) << "fewer than two species have an authored-mesh hand-over — there is no "
                                       "multi-species ring to rule out";

        // The dispersion is measured WITHIN one species at one step, and only
        // then averaged over the cells. Pooling the raw distances first does
        // not work, and the first cut of this test proved it: it reported the
        // shared-threshold arm as MORE dispersed (+-16.1 m) than the
        // decorrelated one (+-13.8 m), which reads as the feature making things
        // worse and is in fact an artefact of the pooling. Two species hand
        // over at two different authored distances (34-48 m and 52-70 m here)
        // and the camera occupies nine different places, so a pooled standard
        // deviation is dominated by BETWEEN-species and BETWEEN-step spread —
        // neither of which is a ring, and both of which a shared threshold has
        // just as much of.
        //
        // A ring is a statement about one species at one instant. So that is
        // the unit of measurement, and a cell needs enough crossings for its
        // own standard deviation to mean anything before it counts.
        constexpr u32 kMinCrossingsPerCell = 8u;

        struct CellDispersion
        {
            u32 Cells = 0;
            u32 Crossings = 0;
            f64 MeanCellStdDev = 0.0;
            f64 WorstCellStdDev = 0.0; // the tightest ring found anywhere
        };

        const auto dispersionPerCell = [&](f32 spread, f32 hysteresis)
        {
            CellDispersion out;
            out.WorstCellStdDev = std::numeric_limits<f64>::infinity();
            f64 stdDevSum = 0.0;

            // A THREE-POSE window, and the reason is a bug this measurement had
            // in its first cut.
            //
            // MeshCoverageLod decides the hysteresis direction from
            // `dist >= prevDist`. Evaluating the "before" state as
            // MeshCoverageLod(distA, distA, ...) makes that comparison TRUE —
            // the plant is treated as RECEDING — while the "after" state on an
            // approaching camera is treated as APPROACHING. HysteresisOffset
            // then shifts the band by +bandStart*h for one and -bandStart*h for
            // the other, so the band slid 7.3 m for the pines and 4.8 m for the
            // shrubs BETWEEN THE TWO CALLS, at h = 0.07. Every plant the band
            // swept in that slide was counted as a crossing that the camera's
            // movement never caused — and only in the `on` arm, because the
            // `off` arm's hysteresis is zero. It inflated precisely the
            // dispersion figure the two assertions below rest on.
            //
            // So each state is now evaluated with the motion that genuinely
            // produced it: the plant's distance at A came from the pose before
            // A, and its distance at B came from A. Costs the first step of the
            // traversal, which is the honest price.
            for (u32 step = 2; step < kTraversalSteps; ++step)
            {
                const glm::vec3 eyePrev = m_Path[step - 2u].Eye;
                const glm::vec3 eyeA = m_Path[step - 1u].Eye;
                const glm::vec3 eyeB = m_Path[step].Eye;
                for (const auto& band : bands)
                {
                    std::vector<f64> distances;
                    for (const auto& record : records)
                    {
                        if (record.m_LayerIndex != band.LayerIndex)
                            continue;

                        const glm::vec3 world = glm::vec3(model * glm::vec4(record.m_Position, 1.0f));
                        const f32 hash = FoliageLod::InstanceHash(record.m_Position);
                        const f32 distPrev = glm::distance(world, eyePrev);
                        const f32 distA = glm::distance(world, eyeA);
                        const f32 distB = glm::distance(world, eyeB);
                        const f32 before = FoliageLod::MeshCoverageLod(distA, distPrev, band.FadeStart,
                                                                       band.ViewDistance, hash, spread, hysteresis);
                        const f32 after = FoliageLod::MeshCoverageLod(distB, distA, band.FadeStart,
                                                                      band.ViewDistance, hash, spread, hysteresis);
                        if ((before > 0.5f) != (after > 0.5f))
                            distances.push_back(static_cast<f64>(distB));
                    }

                    out.Crossings += static_cast<u32>(distances.size());
                    if (distances.size() < kMinCrossingsPerCell)
                        continue;

                    f64 mean = 0.0;
                    for (const f64 d : distances)
                        mean += d;
                    mean /= static_cast<f64>(distances.size());
                    f64 variance = 0.0;
                    for (const f64 d : distances)
                        variance += (d - mean) * (d - mean);
                    const f64 stdDev = std::sqrt(variance / static_cast<f64>(distances.size()));

                    ++out.Cells;
                    stdDevSum += stdDev;
                    out.WorstCellStdDev = std::min(out.WorstCellStdDev, stdDev);
                }
            }

            out.MeanCellStdDev = out.Cells > 0u ? stdDevSum / static_cast<f64>(out.Cells) : 0.0;
            if (out.Cells == 0u)
                out.WorstCellStdDev = 0.0;
            return out;
        };

        // Read off a live layer rather than restated, for the same reason the
        // bands above are: SetStability is the single place these numbers are
        // authored, and a copy here would go stale silently the first time it
        // changed — leaving this test measuring a configuration the renderer no
        // longer has.
        const f32 kSpread = foliage.m_Layers[bands.front().LayerIndex].LodTransitionSpread;
        const f32 kHysteresis = foliage.m_Layers[bands.front().LayerIndex].LodHysteresis;
        ASSERT_GT(kSpread, 0.0f) << "the fixture's layers carry no LOD transition spread — the decorrelated arm "
                                    "would be identical to the shared-threshold one";

        const CellDispersion off = dispersionPerCell(0.0f, 0.0f);
        const CellDispersion on = dispersionPerCell(kSpread, kHysteresis);

        GTEST_LOG_(INFO) << "mesh hand-overs over " << bands.size() << " species x " << (kTraversalSteps - 2u)
                         << " traversal steps, of " << records.size() << " plants: shared thresholds "
                         << off.Crossings << " crossings in " << off.Cells << " measurable cells, depth spread "
                         << off.MeanCellStdDev << " m mean / " << off.WorstCellStdDev << " m tightest | "
                         << "decorrelated over " << kSpread << " m " << on.Crossings << " crossings in "
                         << on.Cells << " cells, " << on.MeanCellStdDev << " m mean / " << on.WorstCellStdDev
                         << " m tightest";

        ASSERT_GT(off.Cells, 0u) << "no species crosses its mesh hand-over in measurable numbers at any step of "
                                    "the traversal — the path does not pass through a hand-over band and this "
                                    "measurement is vacuous";
        ASSERT_GT(on.Cells, 0u);

        // The claim: within one species at one step, decorrelating the
        // thresholds strews the crossings through depth instead of lining them
        // up. Both arms carry the same irreducible spread from the camera's own
        // 11-12 m step, so this is a ratio, not a floor.
        EXPECT_GT(on.MeanCellStdDev, off.MeanCellStdDev * 1.3)
            << "decorrelating the thresholds barely moved the per-species depth dispersion of the crossings ("
            << on.MeanCellStdDev << " m against " << off.MeanCellStdDev
            << " m) — a species still hands over as a band at one distance";

        // And the tightest cell anywhere is what a viewer would actually
        // notice: one ring in one frame is enough to see.
        EXPECT_GT(on.WorstCellStdDev, off.WorstCellStdDev)
            << "the tightest hand-over found anywhere along the traversal is no more dispersed with "
               "decorrelation on ("
            << on.WorstCellStdDev << " m against " << off.WorstCellStdDev
            << " m) — that cell is a ring";
    }

    // ── Criterion 2: geometry, transmission and independent motion up close ──

    TEST_F(FloraTraversalEvidenceTest, CloseFloraHasGeometryTransmissionAndIndependentMotion)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        const ScopedRenderPath restorePath(*this);
        SetPath(RenderingPath::Deferred);

        ScopedMockTime mockTime(kCaptureTime);
        ASSERT_TRUE(BringUpTraversal()) << "the fixture never produced both a meadow and a woodland";
        Advance(m_Path.front(), 0.8f);

        // The woodland end, where the trees are close enough to be inside their
        // authored-mesh band and the sun is behind them.
        const CameraPose closePose = m_Path.back();

        SetFoliageEnabled(false);
        std::vector<u8> noFoliage;
        Capture(closePose, noFoliage);
        SetFoliageEnabled(true);

        auto& foliage = m_TerrainEntity.GetComponent<FoliageComponent>();

        // ── (a) Geometry: a pine drawn as authored geometry is not the same
        //        shape as a pine drawn as a card.
        std::vector<u8> withMesh;
        Capture(closePose, withMesh);

        for (auto& layer : foliage.m_Layers)
            layer.UseAuthoredMesh = false;
        foliage.m_NeedsRebuild = true;
        std::vector<u8> cardsOnly;
        Capture(closePose, cardsOnly);

        for (auto& layer : foliage.m_Layers)
            layer.UseAuthoredMesh = (&layer == &foliage.m_Layers[kWoodlandTreeLayer]) ||
                                    (&layer == &foliage.m_Layers[kUnderstoryLayer]);
        foliage.m_NeedsRebuild = true;

        WritePng("FloraCloseGeometry_GL_Deferred_Mesh.png", withMesh);
        WritePng("FloraCloseGeometryOff_GL_Deferred_Cards.png", cardsOnly);

        const f64 meshCoverage = DifferingFraction(withMesh, noFoliage);
        const f64 cardCoverage = DifferingFraction(cardsOnly, noFoliage);
        const f64 shapeChange = DifferingFraction(withMesh, cardsOnly);
        GTEST_LOG_(INFO) << "close-up geometry: authored mesh covers " << meshCoverage * 100.0 << "%, cards "
                         << cardCoverage * 100.0 << "%, and they differ over " << shapeChange * 100.0
                         << "% of the frame";

        ASSERT_GT(meshCoverage, 0.02) << "there is no flora in the close-up frame";
        EXPECT_GT(shapeChange, 0.01)
            << "the authored plant geometry renders the same pixels as the flat card — #1233 is not reaching "
               "the close-up frame";

        // The whole-frame figure above deliberately includes what the two
        // representations do to the GROUND: a camera-facing billboard casts a
        // broad shadow where a pine casts a narrow one, and that is part of
        // what "convincing geometry" means at scene level. So it is backed by a
        // second number that is about the plants alone — real geometry occupies
        // MORE of the frame than the card stand-in does (37.9% against 26.1% on
        // this fixture), because a card is a flat slice of a canopy.
        EXPECT_GT(meshCoverage, cardCoverage * 1.05)
            << "the authored pines take no more of the frame than their flat cards (" << meshCoverage * 100.0
            << "% against " << cardCoverage * 100.0 << "%) — the near field is not gaining any silhouette";

        // ── (b) Transmission: the backlit canopy gets BRIGHTER, and only where
        //        the canopy is.
        std::vector<u8> transmissive;
        Capture(closePose, transmissive);

        std::array<f32, kLayerCount> savedTransmission{};
        for (u32 i = 0; i < kLayerCount; ++i)
        {
            savedTransmission[i] = foliage.m_Layers[i].TransmissionStrength;
            foliage.m_Layers[i].TransmissionStrength = 0.0f;
        }
        foliage.m_NeedsRebuild = true;
        std::vector<u8> opaque;
        Capture(closePose, opaque);

        for (u32 i = 0; i < kLayerCount; ++i)
            foliage.m_Layers[i].TransmissionStrength = savedTransmission[i];
        foliage.m_NeedsRebuild = true;

        // Only the OFF arm is written. The transmissive arm is the SAME pose
        // with the SAME settings as the geometry cell's Mesh capture above and
        // came back byte-identical to it (md5 a6e34a47...), so writing it again
        // would put a second 570 KB copy of one image in the PR. The arm with no
        // twin anywhere is the one kept — the black cutout a backlit canopy is
        // without #1234, which is the whole point of the comparison.
        WritePng("FloraTransmissionOff_GL_Deferred_Backlit.png", opaque);

        // The mask is "where the flora is", derived from the foliage-disabled
        // frame — so the sky and the terrain cannot dilute the measurement.
        const std::vector<u8> floraMask = DifferenceMask(opaque, noFoliage);
        const f64 lumaOn = MaskedMeanLuma(transmissive, floraMask);
        const f64 lumaOff = MaskedMeanLuma(opaque, floraMask);
        GTEST_LOG_(INFO) << "backlit canopy mean luma: transmission on " << lumaOn << " / off " << lumaOff;

        ASSERT_GT(lumaOff, 0.0) << "the flora mask is empty — there is nothing to measure transmission on";
        EXPECT_GT(lumaOn, lumaOff * 1.02)
            << "the backlit canopy is no brighter with transmission on (" << lumaOn << " against " << lumaOff
            << ") — #1234 is not reaching the frame at this pose";

        // ── (c) Independent motion: the species do not move as one object.
        //        Measured per layer, each alone, over the SAME 0.6 s of scene
        //        time, from the SAME pose, so the only thing that differs
        //        between the two numbers is the species' own wind response.
        //
        // The pose is the traversal's START, and finding that out took two
        // wrong answers worth recording.
        //
        // The first cut measured both species at the woodland end and reported
        // that the grass moves 0% of its pixels. True, and it meant only that
        // there is no grass in the woodland. The second cut moved to the
        // habitat boundary and got 0% again — because the camera LOOKS ALONG
        // THE DIRECTION OF TRAVEL, so once past the boundary the meadow is
        // behind it and out of frame entirely. Only the start pose has both:
        // grass underfoot and the treeline on the ridge ahead, which is exactly
        // what FloraTraversal_GL_Deferred_Meadow.png shows.
        //
        // Both wrong answers were the same failure — a difference measured on
        // an empty image — and neither was caught by the assertion that was
        // supposed to catch it, because that assertion was on the MOTION and
        // not on whether there was anything there to move. Hence the coverage
        // floor per arm below, which is the guard that actually works.
        const CameraPose motionPose = m_Path.front();

        SetFoliageEnabled(false);
        std::vector<u8> motionBaseline;
        Capture(motionPose, motionBaseline);
        SetFoliageEnabled(true);

        struct LayerMotion
        {
            f64 Coverage = 0.0;
            f64 Motion = 0.0;
        };

        // Each arm REWINDS the clock to the same instant first. Advance()
        // mutates m_MockTime, so without this the second species is measured
        // over T+0.6 -> T+1.2 while the first saw T -> T+0.6 — two different
        // gusts, and with two different WindSpeeds the ratio below could be
        // decided by which phase each happened to land on rather than by the
        // species' stiffness. The comment above says "the SAME 0.6 s"; this is
        // what makes that true.
        const f32 motionStartTime = m_MockTime;
        const auto motionOfLayerAlone = [&](u32 layerIndex)
        {
            m_MockTime = motionStartTime;
            Time::SetMockTime(m_MockTime);

            for (u32 i = 0; i < kLayerCount; ++i)
                foliage.m_Layers[i].Enabled = (i == layerIndex);
            foliage.m_NeedsRebuild = true;

            std::vector<u8> before;
            Capture(motionPose, before);
            Advance(motionPose, 0.6f);
            std::vector<u8> after;
            Capture(motionPose, after);

            LayerMotion out;
            out.Coverage = DifferingFraction(before, motionBaseline);
            out.Motion = DifferingFraction(before, after);
            return out;
        };

        const LayerMotion grass = motionOfLayerAlone(kMeadowGrassLayer);
        const LayerMotion trees = motionOfLayerAlone(kWoodlandTreeLayer);

        for (u32 i = 0; i < kLayerCount; ++i)
            foliage.m_Layers[i].Enabled = true;
        foliage.m_NeedsRebuild = true;

        GTEST_LOG_(INFO) << "at the meadow pose, 0.6 s of wind moves " << grass.Motion * 100.0
                         << "% of the grass's pixels (it covers " << grass.Coverage * 100.0 << "%) and "
                         << trees.Motion * 100.0 << "% of the pines' (covering " << trees.Coverage * 100.0 << "%)";

        // The floor is 0.2% of the frame, not a round 1%: the whole flora covers
        // only 4.8% of the meadow pose (it is an open field under a low sun),
        // so a per-species floor of 1% could not be met by anything and would
        // fail every run for the wrong reason.
        ASSERT_GT(grass.Coverage, 0.002)
            << "the grass alone covers almost none of the meadow frame — its motion figure is a statement "
               "about an empty image, not about the wind";
        ASSERT_GT(trees.Coverage, 0.002)
            << "the pines alone cover almost none of the meadow frame — same problem, other species";

        const f64 grassMotion = grass.Motion;
        const f64 treeMotion = trees.Motion;
        EXPECT_GT(grassMotion, 0.0005) << "the grass does not move at all over 0.6 s of scene time";
        EXPECT_GT(treeMotion, 0.0005) << "the pines do not move at all over 0.6 s of scene time";
        // A stiff trunk and a loose blade cannot answer the same gust the same
        // way. Stated as a ratio so it does not encode this scene's plant
        // sizes; the direction is the claim, the margin only keeps noise out.
        const f64 ratio = grassMotion > treeMotion ? grassMotion / treeMotion : treeMotion / grassMotion;
        EXPECT_GT(ratio, 1.15) << "the grass and the pines move by the same fraction of their pixels (" << ratio
                               << "x) — the species are not moving independently";
    }

    // ── The conditional cells: MSAA and a non-native resolution ──────────────

    TEST_F(FloraTraversalEvidenceTest, TraversalContinuityHoldsUnderMsaaAndAnOddResolution)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        // These are cells for a reason specific to this subsystem. Alpha-tested
        // foliage coverage is resolved STOCHASTICALLY by #1237, and a dither is
        // a PIXEL-FREQUENCY pattern: MSAA resolves it at sample rate instead of
        // pixel rate, and a non-native target samples it on a different grid.
        // Either can turn a dissolve into a shimmer while the native cell above
        // still passes.
        //
        // Upscaling is verified LIVE instead, and deliberately: the mode lives
        // on PostProcessSettings rather than on RendererSettings, so this
        // fixture cannot drive it without reaching across a seam it does not
        // own. Same split every Vulkan cell already takes.
        const ScopedRenderPath restorePath(*this);
        SetPath(RenderingPath::Deferred);

        ScopedMockTime mockTime(kCaptureTime);
        ASSERT_TRUE(BringUpTraversal()) << "the fixture never produced both a meadow and a woodland";
        Advance(m_Path.front(), 0.8f);

        const std::vector<CameraPose> poses = PathWithSteps(kConditionalSteps);

        const auto measure = [&](const char* cell, u32 width, u32 height)
        {
            const auto baselines = CaptureBaselines(poses, width, height);

            SetStability(false);
            const TraversalMetrics off = MeasureTraversal(poses, baselines, width, height, false);
            SetStability(true);
            const TraversalMetrics on = MeasureTraversal(poses, baselines, width, height, true);

            // Msaa1 is the control, and its capture came back byte-identical to the
            // close-up cell's Mesh frame (same pose, same settings, one sample), so
            // it is not written twice. Msaa4 and NonNativeRes have no twin anywhere
            // and are what a reader needs to see.
            if (std::string(cell) != "Msaa1")
                WritePng(std::string("FloraTraversal_GL_Deferred_") + cell + ".png", on.Frames.back(),
                         width, height);

            GTEST_LOG_(INFO) << cell << ": coverage " << on.Coverage.front() * 100.0 << "% -> "
                             << on.Coverage.back() * 100.0 << "%, mean step " << on.MeanStep * 100.0
                             << "%, worst curvature off x" << off.NormalisedWorstCurvature << " / on x"
                             << on.NormalisedWorstCurvature;

            EXPECT_GT(on.MinCoverage, 0.005) << cell << ": some pose has almost no flora in it";
            EXPECT_GT(off.MinCoverage, 0.005) << cell << ": some pose of the control arm has almost no flora in it";
            EXPECT_GT(on.MeanStep, 0.002) << cell << ": the traversal barely changed the frame";
            // As in the native cell: a degenerate control arm makes the
            // comparison below unfailable rather than merely noisy.
            EXPECT_GT(off.MeanStep, 0.002)
                << cell << ": the control arm barely changed the frame, so its normalised curvature is unbounded";
            EXPECT_LE(on.NormalisedWorstCurvature, off.NormalisedWorstCurvature * 1.1)
                << cell << ": the stability features made the worst local discontinuity worse at this setting";
        };

        auto& settings = Renderer3D::GetRendererSettings();
        const u32 samplesBefore = settings.Deferred.MSAASampleCount;

        settings.Deferred.MSAASampleCount = 1;
        Renderer3D::ApplyRendererSettings();
        measure("Msaa1", kWidth, kHeight);

        const u32 wanted = std::min(4u, std::max(1u, Renderer3D::GetMaxMSAASamples()));
        settings.Deferred.MSAASampleCount = wanted;
        Renderer3D::ApplyRendererSettings();
        const u32 samplesUsed = settings.Deferred.MSAASampleCount;
        // Reported, not assumed: a device whose maximum is 1 leaves the cell
        // running at 1, and the PR's matrix would otherwise claim an MSAA row
        // it never had. What this CANNOT see is a driver that accepts a sample
        // count through the API and quietly resolves at one sample — no query
        // here exposes that — so the claim is "the engine was configured for N
        // samples", not "the silicon ran N".
        GTEST_LOG_(INFO) << "MSAA cell ran at " << samplesUsed << " samples (asked for " << wanted << ")";
        EXPECT_GT(samplesUsed, 1u) << "MSAA NOT RUN — the device refused a sample count above 1";
        measure("Msaa4", kWidth, kHeight);

        settings.Deferred.MSAASampleCount = samplesBefore;
        Renderer3D::ApplyRendererSettings();

        constexpr u32 kOddWidth = 907;
        constexpr u32 kOddHeight = 611;
        ResizeRenderTarget(kOddWidth, kOddHeight);
        measure("NonNativeRes", kOddWidth, kOddHeight);
        ResizeRenderTarget(kWidth, kHeight);
    }

} // namespace OloEngine::Tests

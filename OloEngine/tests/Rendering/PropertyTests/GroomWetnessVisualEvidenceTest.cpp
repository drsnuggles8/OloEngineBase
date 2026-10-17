// OLO_TEST_LAYER: L8
// =============================================================================
// GroomWetnessVisualEvidenceTest.cpp — waterline-driven wet coats (#1615).
//
// The issue's acceptance, on the shipped coats and their real bound bodies:
//
//   * a HORSE WADING in its scene's shallow water and a DOG LEAVING a pond,
//     each captured DRY, WET and DRYING, on strands and on cards, from two
//     angles, on every render path. The wetness is advanced by the scene's own
//     gameplay tick (Scene::UpdateGroomWetness through the scheduler), not set
//     by hand: the water is found, the soak rises, the line recedes.
//   * WHERE it is wet: the pixels a wet coat changes lie in the lower part of
//     the coat -- legs before the back.
//   * RAIN wets the whole coat.
//   * CARDS MATCH STRANDS WHILE WET within the ladder's bars
//     (GroomAnimalsAcceptanceEvidenceTest: coverage 0.90-1.20, energy
//     0.75-1.40), and the same pair dry is the control.
//
// PNGs land in .v-build/1615/visual, named after the cell.
// =============================================================================
#include "OloEnginePCH.h"
#include "RendererAttachedTest.h"
#include "AttachmentFeedbackEvidence.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomWetness.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Passes/GroomRenderPass.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        const std::filesystem::path kEditor{ OLO_TEST_EDITOR_ROOT };
        constexpr u32 kWidth = 960;
        constexpr u32 kHeight = 540;
        constexpr std::array kPaths{ std::pair{ RenderingPath::Forward, "Forward" },
                                     std::pair{ RenderingPath::ForwardPlus, "ForwardPlus" },
                                     std::pair{ RenderingPath::Deferred, "Deferred" } };
        // GroomAnimalsAcceptanceEvidenceTest's card-tier bars, not loosened.
        constexpr f64 kCoverageFloor = 0.90;
        constexpr f64 kCoverageCeiling = 1.20;
        constexpr f64 kEnergyFloor = 0.75;
        constexpr f64 kEnergyCeiling = 1.40;

        f64 Linear(u8 value)
        {
            return std::pow(static_cast<f64>(value) / 255.0, 2.2);
        }

        struct CoatImage
        {
            std::vector<u8> Pixels;
            std::vector<i32> Ids;
            // World height of what each pixel shows, from the depth the coat
            // left (NaN where nothing was drawn).
            std::vector<f32> Heights;
        };

        struct CoatMeasure
        {
            u32 Pixels = 0;
            f64 MeanLuma = 0.0;
            u32 MinRow = 0;
            u32 MaxRow = 0;
        };

        CoatMeasure Measure(const CoatImage& image, i32 coat)
        {
            CoatMeasure m;
            m.MinRow = kHeight;
            f64 luma = 0.0;
            for (u32 row = 0; row < kHeight; ++row)
            {
                for (u32 col = 0; col < kWidth; ++col)
                {
                    const sizet i = static_cast<sizet>(row) * kWidth + col;
                    if (image.Ids[i] != coat)
                        continue;
                    ++m.Pixels;
                    m.MinRow = std::min(m.MinRow, row);
                    m.MaxRow = std::max(m.MaxRow, row);
                    luma += 0.2126 * Linear(image.Pixels[i * 4u]) + 0.7152 * Linear(image.Pixels[i * 4u + 1u]) +
                            0.0722 * Linear(image.Pixels[i * 4u + 2u]);
                }
            }
            m.MeanLuma = m.Pixels > 0u ? luma / static_cast<f64>(m.Pixels) : 0.0;
            return m;
        }

        // Pixels of the coat in either frame whose colour moved more than the
        // 8-bit noise floor, and how many of them show something at or below
        // world height `below` -- the water line plus a strand's reach. In the
        // WORLD, not in screen rows: a dog lying in a pond seen from above shows
        // its waterline across the middle of the frame.
        struct Change
        {
            u32 Changed = 0;
            u32 Low = 0;
        };

        bool Moved(const CoatImage& a, const CoatImage& b, sizet i)
        {
            bool moved = a.Ids[i] != b.Ids[i];
            for (u32 k = 0; k < 3u && !moved; ++k)
                moved = std::abs(static_cast<i32>(a.Pixels[i * 4u + k]) - b.Pixels[i * 4u + k]) > 6;
            return moved;
        }

        // `noise`, when given, is a repeat of `dry`'s frame: a pixel that moves
        // between the two identical captures is the coat's own frame-to-frame
        // noise (stochastic coverage, the temporal history of long hair) and is
        // not counted against or for the wetness.
        Change Compare(const CoatImage& dry, const CoatImage& other, i32 coat, f32 below, const CoatImage* noise = nullptr)
        {
            Change c;
            for (sizet i = 0; i < dry.Ids.size(); ++i)
            {
                if (dry.Ids[i] != coat && other.Ids[i] != coat)
                    continue;
                if (!Moved(dry, other, i) || (noise != nullptr && Moved(dry, *noise, i)))
                    continue;
                ++c.Changed;
                const f32 height = dry.Ids[i] == coat ? dry.Heights[i] : other.Heights[i];
                c.Low += height <= below ? 1u : 0u;
            }
            return c;
        }
    } // namespace

    class GroomWetnessVisualEvidenceTest : public RendererAttachedTest
    {
      protected:
        Entity m_Coat;
        Entity m_Body;
        bool m_Dog = true;
        // The water's surface, world height, and how far above it a soaked
        // strand can reach once laid down: the band and a strand's length.
        f32 m_WaterHeight = 0.0f;
        f32 m_WetReach = 0.0f;
        Ref<Project> m_PreviousProject;
        Ref<AssetManagerBase> m_PreviousAssets;
        bool m_StateCaptured = false;

        void BuildScene() override
        {
            m_PreviousProject = Project::GetActive();
            m_PreviousAssets = Project::HasAssetManager() ? Project::GetAssetManager() : nullptr;
            m_StateCaptured = true;
            ASSERT_TRUE(Project::Load(kEditor / "SandboxProject/Sandbox.oloproj"));
            auto assets = Ref<EditorAssetManager>::Create();
            assets->Initialize(false);
            Project::SetAssetManager(assets);
            EnableRendering(kWidth, kHeight);
            auto& settings = Renderer3D::GetRendererSettings();
            settings.EditorDebugDrawsEnabled = false;
            settings.ShowGrid = false;
            settings.ShowWorldAxisHelper = false;
            settings.ShowComponentGizmos = false;
        }

        void TearDown() override
        {
            Time::ClearMockTime();
            RendererAttachedTest::TearDown();
            if (!m_StateCaptured)
                return;
            Project::Unload();
            if (m_PreviousProject)
                Project::NewInMemory(m_PreviousProject->GetDirectory(), m_PreviousProject->GetConfig());
            if (m_PreviousAssets)
                Project::SetAssetManager(m_PreviousAssets);
        }

        [[nodiscard]] i32 CoatId() const
        {
            return static_cast<i32>(static_cast<u32>(static_cast<entt::entity>(m_Coat)));
        }

        // The horse in its scene's shallow water, or the dog with a pond under
        // it; the coat wetted by nothing yet.
        void LoadSubject(bool dog)
        {
            m_Dog = dog;
            std::vector<UUID> previous;
            const auto view = GetScene().GetAllEntitiesWith<IDComponent>();
            for (auto entity : view)
                previous.push_back(view.get<IDComponent>(entity).ID);
            for (const UUID id : previous)
                if (auto entity = GetScene().TryGetEntityWithUUID(id))
                    GetScene().DestroyEntity(*entity);
            const auto fixture = kEditor / "SandboxProject/Assets/Scenes/Tests" /
                                 (dog ? "GroomSoftIntersectionDog.olo" : "GroomSoftIntersectionHorse.olo");
            ASSERT_TRUE(SceneSerializer(GetSceneRef()).Deserialize(fixture.string()));
            EnableRendering(kWidth, kHeight);
            m_Coat = GetScene().FindEntityByName(dog ? "DogCoat" : "LongCoatHorseCoat");
            m_Body = GetScene().FindEntityByName(dog ? "Dog" : "LongCoatHorse");
            ASSERT_TRUE(m_Coat && m_Body);
            if (!dog)
            {
                m_WaterHeight = GetScene().FindEntityByName("ShallowWater").GetComponent<TransformComponent>().Translation.y;
                m_WetReach = 0.15f; // the feathering at the fetlocks is a hand long
            }
            if (dog)
            {
                // A pond the dog stands in to a quarter of its coat's height --
                // its legs -- measured on the coat as it stands, the way the
                // wetness system measures it.
                const auto groom = AssetManager::GetAsset<GroomAsset>(m_Coat.GetComponent<GroomComponent>().m_Groom);
                ASSERT_TRUE(groom);
                const GroomWetnessAxis axis = MakeGroomWetnessAxis(GetScene().GetWorldTransform(static_cast<entt::entity>(m_Coat)));
                f32 lowest = std::numeric_limits<f32>::max();
                f32 highest = std::numeric_limits<f32>::lowest();
                for (u32 corner = 0; corner < 8u; ++corner)
                {
                    const glm::vec3 lo = groom->GetBoundsMin();
                    const glm::vec3 hi = groom->GetBoundsMax();
                    const glm::vec3 c((corner & 1u) != 0u ? hi.x : lo.x, (corner & 2u) != 0u ? hi.y : lo.y,
                                      (corner & 4u) != 0u ? hi.z : lo.z);
                    lowest = std::min(lowest, glm::dot(c, axis.Height));
                    highest = std::max(highest, glm::dot(c, axis.Height));
                }
                const f32 surface = axis.Origin.y + lowest + 0.25f * (highest - lowest);
                std::printf("[groom-wet] dog coat spans %.3f..%.3f m world; pond at %.3f m\n", axis.Origin.y + lowest,
                            axis.Origin.y + highest, surface);
                Entity pond = GetScene().CreateEntity("Pond");
                pond.GetComponent<TransformComponent>().Translation = { -0.3f, surface, 0.0f };
                m_WaterHeight = surface;
                m_WetReach = 0.08f;
                auto& water = pond.AddComponent<WaterComponent>();
                water.m_WorldSizeX = 4.0f;
                water.m_WorldSizeZ = 4.0f;
                water.m_WaveAmplitude = 0.0f;
                water.m_WaveSpeed = 0.0f;
                water.m_GridResolutionX = 32;
                water.m_GridResolutionZ = 32;
            }
            auto& wet = m_Coat.AddOrReplaceComponent<GroomWetnessComponent>();
            wet.m_SoakRate = 2.0f;
            wet.m_DryRate = 0.1f;
            // The soft intersection the fixture authors fades the coat at the
            // water; the wet response is what is measured here.
            m_Coat.GetComponent<GroomSoftIntersectionComponent>().m_Enabled = false;
        }

        [[nodiscard]] Entity Water()
        {
            return GetScene().FindEntityByName(m_Dog ? "Pond" : "ShallowWater");
        }

        // The scene's own tick: the scheduler runs Scene::UpdateGroomWetness.
        void Tick(u32 ticks, f32 dt = 0.1f)
        {
            for (u32 i = 0; i < ticks; ++i)
                RunFrames(1u, dt);
        }

        void SetRepresentation(bool cards)
        {
            auto& lod = m_Coat.GetComponent<GroomLodComponent>();
            lod.m_Enabled = cards;
            lod.m_CardPixelSize = 16384.0f;
            lod.m_MeshPixelSize = 0.5f;
            lod.m_HoldFrames = 0;
            lod.m_VisibilitySteps = 0;
            lod.m_SimulationSteps = 0;
            lod.m_ShadowSteps = 0;
        }

        void Capture(const std::string& name, bool cards, i32 angle, f32 distance, CoatImage& out)
        {
            Time::SetMockTime(12.0f);
            // THE TIER IS FORCED, NOT HANDED OVER TO (GroomSoftIntersectionVisual-
            // EvidenceTest has the same guard, for the same reason): a #1612
            // cross-fade from the previous capture's tier would leave the
            // outgoing tier in the stochastic coat's TAA history through the
            // settle, and a capture would differ from its own repeat by that
            // tail rather than by the water. Held off for this capture only.
            struct RestoreCrossFade
            {
                u32 Frames = Renderer3D::GetRendererSettings().GroomLodCrossFadeFrames;
                ~RestoreCrossFade()
                {
                    Renderer3D::GetRendererSettings().GroomLodCrossFadeFrames = Frames;
                }
            } restoreCrossFade;
            Renderer3D::GetRendererSettings().GroomLodCrossFadeFrames = 0u;
            SetRepresentation(cards);
            EditorCamera camera(45.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.02f, 200.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            const glm::vec3 target = m_Dog ? glm::vec3(-0.3f, 0.2f, 0.04f) : glm::vec3(0.0f, 1.0f, 0.0f);
            const glm::vec3 direction = glm::normalize(m_Dog ? (angle == 0 ? glm::vec3(0.95f, 0.15f, 1.36f) : glm::vec3(-0.8f, 0.35f, 1.3f))
                                                             : (angle == 0 ? glm::vec3(3.0f, 0.2f, 4.0f) : glm::vec3(-3.0f, 0.5f, 4.0f)));
            const glm::vec3 position = target + direction * distance;
            const glm::vec3 delta = position - target;
            camera.SetPose(position, -std::atan2(delta.x, delta.z), std::asin(delta.y / glm::length(delta)));
            Renderer3D::ResetFrameSequences();
            RunEditorFrames(camera, 16u, 0.0f);

            auto graph = RenderGraphDebugRuntime::GetActiveGraph();
            ASSERT_TRUE(graph);
            out.Ids.assign(static_cast<sizet>(kWidth) * kHeight, -1);
            std::vector<f32> depth(static_cast<sizet>(kWidth) * kHeight, 1.0f);
            struct RemoveHook
            {
                Ref<RenderGraph> Graph;
                ~RemoveHook()
                {
                    Graph->RemovePostPassHook("GroomWetnessEvidence");
                }
            } removeHook{ graph };
            graph->AddPostPassHook("GroomWetnessEvidence", [&](std::string_view pass, RenderGraph& g)
                                   {
                if (pass != "GroomPass")
                    return;
                const u32 texture = g.ResolveTexture(g.GetTextureHandle("SceneEntityID"));
                if (texture != 0u)
                    glGetTextureImage(texture, 0, GL_RED_INTEGER, GL_INT, static_cast<GLsizei>(out.Ids.size() * sizeof(i32)),
                                      out.Ids.data());
                const u32 depthTexture = g.ResolveTexture(g.GetTextureHandle("SceneDepthAttachment"));
                if (depthTexture != 0u)
                    glGetTextureImage(depthTexture, 0, GL_DEPTH_COMPONENT, GL_FLOAT,
                                      static_cast<GLsizei>(depth.size() * sizeof(f32)), depth.data()); });
            RunEditorFrames(camera, 1u, 0.0f);
            const glm::mat4 inverse = glm::inverse(camera.GetViewProjection());
            out.Heights.assign(depth.size(), std::numeric_limits<f32>::quiet_NaN());
            for (u32 row = 0; row < kHeight; ++row)
            {
                for (u32 col = 0; col < kWidth; ++col)
                {
                    const sizet i = static_cast<sizet>(row) * kWidth + col;
                    if (!(depth[i] < 1.0f))
                        continue;
                    const glm::vec4 ndc((static_cast<f32>(col) + 0.5f) / kWidth * 2.0f - 1.0f,
                                        (static_cast<f32>(row) + 0.5f) / kHeight * 2.0f - 1.0f, depth[i] * 2.0f - 1.0f, 1.0f);
                    const glm::vec4 world = inverse * ndc;
                    out.Heights[i] = world.y / world.w;
                }
            }
            u32 width = 0;
            u32 height = 0;
            ASSERT_TRUE(ReadbackComposite(out.Pixels, width, height));
            ASSERT_EQ(width, kWidth);
            ASSERT_EQ(height, kHeight);
            const auto pass = Renderer3D::GetGroomRenderPass();
            ASSERT_TRUE(pass);
            EXPECT_GT(pass->GetStats().GroomsDrawn, 0u);
            EXPECT_EQ(pass->GetStats().GroomsBindingRefused, 0u);
            EXPECT_GT(pass->GetStats().Lod.ByRepresentation[static_cast<sizet>(cards ? GroomRepresentation::Card
                                                                                     : GroomRepresentation::Strand)],
                      0u);
            if (!name.empty())
            {
                const auto directory = kEditor.parent_path() / ".v-build/1615/visual";
                std::filesystem::create_directories(directory);
                stbi_flip_vertically_on_write(1);
                EXPECT_NE(stbi_write_png((directory / (name + ".png")).string().c_str(), static_cast<i32>(width),
                                         static_cast<i32>(height), 4, out.Pixels.data(), static_cast<i32>(width * 4u)),
                          0);
                stbi_flip_vertically_on_write(0);
            }
        }
    };

    // DRY -> WET -> DRYING, through the scene's own tick, on every path, both
    // representations and two angles; the horse wading, the dog leaving a pond.
    TEST_F(GroomWetnessVisualEvidenceTest, AHorseWadesAndADogLeavesWaterThroughADryWetDryingCycle)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        for (const bool dog : { false, true })
        {
            const std::string subject = dog ? "DogLeavingPond" : "HorseWading";
            for (const auto& [path, pathName] : kPaths)
            {
                LoadSubject(dog);
                Renderer3D::GetRendererSettings().Path = path;
                Renderer3D::ApplyRendererSettings();
                auto& wet = m_Coat.GetComponent<GroomWetnessComponent>();
                const f32 distance = dog ? 1.65f : 5.0f;

                // THE SAME SCENE IN EVERY CAPTURE: the water stays where it is
                // and drawn, so the only thing that differs between the three
                // frames is the coat's wetness. Drying ticks run with the water
                // drained (the horse) or the dog walked out of its pond, and the
                // scene is put back before the capture, which ticks nothing.

                // The coat's self-shadow volume is baked AT REST here. Baked from
                // the drawn pose (the scenes' default), it follows the soaked legs
                // as their fur lies down: the box shrinks, every voxel shifts, and
                // the mane's self-shadowing moves with it -- a real consequence of
                // the posed bake, measured on the horse at 26 % of the changed
                // pixels, and not what this case asks (where the water reached).
                if (m_Coat.HasComponent<GroomCoatShadowComponent>())
                    m_Coat.GetComponent<GroomCoatShadowComponent>().m_BakeAtRest = true;

                // DRY: the coat stands in the water but has not been ticked wet
                // -- its wetness component sits out the settling ticks, 8 s of
                // them, so the long hair's guide simulation has come to rest and
                // the three frames differ by their wetness alone (a mane still
                // swinging at 0.3 s showed up as "wet" on the horse's neck).
                wet.m_Enabled = false;
                Tick(80u);
                wet.m_Enabled = true;
                EXPECT_FLOAT_EQ(wet.m_Immersion, 0.0f);
                std::array<std::array<CoatImage, 2>, 2> dry{};
                for (const bool cards : { false, true })
                    for (i32 angle = 0; angle < 2; ++angle)
                        Capture("GroomWetOff_GL_" + std::string(pathName) + "_" + subject + (cards ? "_Cards_" : "_Strands_") +
                                    std::to_string(angle),
                                cards, angle, distance, dry[cards][angle]);

                // WET: in the water, soaking for 1.5 s at 2/s.
                Tick(15u);
                std::printf("[groom-wet] %s %s soaked immersion=%.3f line=%.3f m above the coat's origin\n", subject.c_str(),
                            pathName, wet.m_Immersion, wet.m_SoakedLine);
                EXPECT_FLOAT_EQ(wet.m_Immersion, 1.0f) << "standing in the water soaks the coat";
                EXPECT_GT(wet.m_SoakedLine, 0.0f) << "up to the water's height on the coat";
                EXPECT_LT(wet.m_SoakedLine, dog ? 0.3f : 0.6f) << "and no higher";
                // Each wet frame has a CONTROL taken at the same instant -- no tick
                // between them -- with the wetness switched off: the long hair's
                // simulation keeps moving between ticks, so only the pair isolates
                // what the wetness did.
                std::array<std::array<CoatImage, 2>, 2> soaked{};
                std::array<std::array<CoatImage, 2>, 2> soakedControl{};
                std::array<std::array<CoatImage, 2>, 2> soakedRepeat{};
                for (const bool cards : { false, true })
                    for (i32 angle = 0; angle < 2; ++angle)
                    {
                        Capture("GroomWet_GL_" + std::string(pathName) + "_" + subject + (cards ? "_Cards_" : "_Strands_") +
                                    std::to_string(angle),
                                cards, angle, distance, soaked[cards][angle]);
                        wet.m_Enabled = false;
                        Capture("", cards, angle, distance, soakedControl[cards][angle]);
                        Capture("", cards, angle, distance, soakedRepeat[cards][angle]);
                        wet.m_Enabled = true;
                    }

                // DRYING: out of the water -- the dog walks out of its pond, the
                // horse's water drains -- for 4 s at 0.1/s: 40 % dry.
                if (dog)
                    m_Body.GetComponent<TransformComponent>().Translation.x += 3.0f;
                else
                    Water().GetComponent<WaterComponent>().m_Enabled = false;
                Tick(40u);
                std::printf("[groom-wet] %s %s drying immersion=%.3f\n", subject.c_str(), pathName, wet.m_Immersion);
                EXPECT_NEAR(wet.m_Immersion, 0.6f, 0.02f) << "drying at the dry rate";
                EXPECT_GT(wet.m_SoakedLine, 0.0f) << "the line remembers how high the water came";
                if (dog)
                    m_Body.GetComponent<TransformComponent>().Translation.x -= 3.0f;
                else
                    Water().GetComponent<WaterComponent>().m_Enabled = true;
                std::array<std::array<CoatImage, 2>, 2> drying{};
                std::array<std::array<CoatImage, 2>, 2> dryingControl{};
                std::array<std::array<CoatImage, 2>, 2> dryingRepeat{};
                for (const bool cards : { false, true })
                    for (i32 angle = 0; angle < 2; ++angle)
                    {
                        Capture("GroomWetDrying_GL_" + std::string(pathName) + "_" + subject + (cards ? "_Cards_" : "_Strands_") +
                                    std::to_string(angle),
                                cards, angle, distance, drying[cards][angle]);
                        wet.m_Enabled = false;
                        Capture("", cards, angle, distance, dryingControl[cards][angle]);
                        Capture("", cards, angle, distance, dryingRepeat[cards][angle]);
                        wet.m_Enabled = true;
                    }

                for (const bool cards : { false, true })
                {
                    for (i32 angle = 0; angle < 2; ++angle)
                    {
                        const std::string cell = std::string(pathName) + "_" + subject + (cards ? "_Cards_" : "_Strands_") +
                                                 std::to_string(angle);
                        SCOPED_TRACE(cell);
                        const CoatMeasure dryCoat = Measure(dry[cards][angle], CoatId());
                        const CoatMeasure wetCoat = Measure(soaked[cards][angle], CoatId());
                        const f32 reach = m_WaterHeight + m_WetReach;
                        const Change wetChange = Compare(soakedControl[cards][angle], soaked[cards][angle], CoatId(), reach,
                                                         &soakedRepeat[cards][angle]);
                        const Change dryingChange = Compare(dryingControl[cards][angle], drying[cards][angle], CoatId(), reach,
                                                            &dryingRepeat[cards][angle]);
                        const Change noise = Compare(soakedControl[cards][angle], soakedRepeat[cards][angle], CoatId(), reach);
                        std::printf("[groom-wet] %s repeat noise=%u px (low share %.2f)\n", cell.c_str(), noise.Changed,
                                    noise.Changed > 0u ? static_cast<f64>(noise.Low) / noise.Changed : 0.0);
                        std::printf("[groom-wet] %s dry px=%u luma=%.4f | wet px=%u luma=%.4f changed=%u low=%.2f | drying changed=%u low=%.2f\n",
                                    cell.c_str(), dryCoat.Pixels, dryCoat.MeanLuma, wetCoat.Pixels, wetCoat.MeanLuma,
                                    wetChange.Changed,
                                    wetChange.Changed > 0u ? static_cast<f64>(wetChange.Low) / wetChange.Changed : 0.0,
                                    dryingChange.Changed,
                                    dryingChange.Changed > 0u ? static_cast<f64>(dryingChange.Low) / dryingChange.Changed : 0.0);
                        ASSERT_GT(dryCoat.Pixels, 1000u);
                        EXPECT_GT(wetChange.Changed, dryCoat.Pixels / 50u) << "the water wet the coat";
                        EXPECT_GT(wetChange.Low * 10u, wetChange.Changed * 8u)
                            << "and only where the water reached: legs before the back";
                        EXPECT_LT(wetCoat.MeanLuma, Measure(soakedControl[cards][angle], CoatId()).MeanLuma) << "wet fur is darker";
                        EXPECT_LT(dryingChange.Changed, wetChange.Changed) << "drying recedes the wet coat";
                        EXPECT_GT(dryingChange.Changed, 0u) << "but 40 % dry is not dry";
                    }
                }
            }
        }
    }

    // RAIN wets the whole coat, back and all: the scene's weather wetness times
    // the coat's soak, advanced by the same tick.
    TEST_F(GroomWetnessVisualEvidenceTest, RainWetsTheWholeCoat)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        LoadSubject(false);
        Water().GetComponent<WaterComponent>().m_Enabled = false;
        CoatImage dry;
        Capture("GroomWetRainOff_GL_Forward_Horse", false, 0, 5.0f, dry);
        Entity weather = GetScene().CreateEntity("Weather");
        auto& state = weather.AddComponent<WeatherStateComponent>();
        state.m_Wetness = 1.0f;
        state.m_WetnessRiseRate = 100.0f;
        state.m_WetnessDryRate = 0.0f;
        Tick(10u);
        const auto& wet = m_Coat.GetComponent<GroomWetnessComponent>();
        std::printf("[groom-wet-rain] coat rain wetness=%.3f weather=%.3f\n", wet.m_Rain, state.m_Wetness);
        EXPECT_GT(wet.m_Rain, 0.3f) << "the coat soaked up the rain";
        CoatImage rained;
        Capture("GroomWetRain_GL_Forward_Horse", false, 0, 5.0f, rained);
        const CoatMeasure dryCoat = Measure(dry, CoatId());
        const Change change = Compare(dry, rained, CoatId(), m_WaterHeight + m_WetReach);
        std::printf("[groom-wet-rain] changed=%u of %u low share=%.2f\n", change.Changed, dryCoat.Pixels,
                    change.Changed > 0u ? static_cast<f64>(change.Low) / change.Changed : 0.0);
        EXPECT_GT(change.Changed, dryCoat.Pixels / 4u) << "rain reaches the whole coat";
        EXPECT_LT(change.Low * 10u, change.Changed * 5u) << "not just its legs";
    }

    // CARDS MATCH STRANDS WHILE WET, within the ladder's bars -- coverage and
    // energy at a distance the card tier serves -- and dry, as the control.
    TEST_F(GroomWetnessVisualEvidenceTest, CardsMatchStrandsWhileWetWithinTheLaddersBars)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        for (const bool dog : { false, true })
        {
            LoadSubject(dog);
            auto& wet = m_Coat.GetComponent<GroomWetnessComponent>();
            const f32 distance = dog ? 6.0f : 19.0f;
            // Soaked ALL OVER for the wet arm -- the water to its belly and a
            // downpour on its back -- so the card tier is judged against a coat
            // whose every strand is laid down, clumped and darkened.
            Entity weather = GetScene().CreateEntity("Weather");
            auto& rain = weather.AddComponent<WeatherStateComponent>();
            rain.m_WetnessRiseRate = 100.0f;
            rain.m_WetnessDryRate = 0.0f; // the weather holds what the test sets
            wet.m_RainSoak = 1.0f;
            for (const bool soaked : { false, true })
            {
                Water().GetComponent<WaterComponent>().m_Enabled = true;
                rain.m_Wetness = soaked ? 1.0f : 0.0f;
                wet.m_Enabled = soaked;
                wet.m_Immersion = 0.0f;
                wet.m_SoakedLine = GroomWetnessLimits::DryLine;
                wet.m_Rain = 0.0f;
                Tick(15u);
                if (soaked)
                {
                    EXPECT_GT(wet.m_Rain, 0.95f) << "the downpour soaked the coat";
                }
                for (i32 angle = 0; angle < 2; ++angle)
                {
                    const std::string cell = std::string(dog ? "Dog" : "Horse") + (soaked ? "_Wet_" : "_Dry_") + std::to_string(angle);
                    CoatImage strands;
                    CoatImage cards;
                    Capture("GroomWetLadder_GL_Forward_" + cell + "_Strands", false, angle, distance, strands);
                    Capture("GroomWetLadder_GL_Forward_" + cell + "_Cards", true, angle, distance, cards);
                    const CoatMeasure s = Measure(strands, CoatId());
                    const CoatMeasure c = Measure(cards, CoatId());
                    const f64 coverage = s.Pixels > 0u ? static_cast<f64>(c.Pixels) / s.Pixels : 0.0;
                    const f64 energy = s.Pixels > 0u && s.MeanLuma > 0.0
                                           ? (static_cast<f64>(c.Pixels) * c.MeanLuma) / (static_cast<f64>(s.Pixels) * s.MeanLuma)
                                           : 0.0;
                    std::printf("[groom-wet-ladder] %s strands px=%u luma=%.4f cards px=%u luma=%.4f coverage=%.3f energy=%.3f\n",
                                cell.c_str(), s.Pixels, s.MeanLuma, c.Pixels, c.MeanLuma, coverage, energy);
                    ASSERT_GT(s.Pixels, 200u) << cell;
                    EXPECT_GT(coverage, kCoverageFloor) << cell;
                    EXPECT_LT(coverage, kCoverageCeiling) << cell;
                    EXPECT_GT(energy, kEnergyFloor) << cell;
                    EXPECT_LT(energy, kEnergyCeiling) << cell;
                }
            }
        }
    }
    // THE CONDITIONAL CELLS: MSAA (1x and the device's 4x request, which the
    // forward paths keep single-sampled for grooms), native, bilinear and
    // FSR2-scaled frames, at two resolutions, on every path. In each, the wet
    // coat must differ from the same frame with its wetness off by more than
    // two identical frames differ, the groom pass must be in the graph and
    // hazard-free, and on Forward+ (and Deferred, which always culls) the
    // strands must have taken the cluster walk -- which needs a local light
    // to walk to, so one lamp stands beside the horse. Deferred dynamic
    // scaling is the known #1537 rejection and is recorded, not run.
    TEST_F(GroomWetnessVisualEvidenceTest, WetnessAndClusterWalkSurviveMsaaUpscaleAndViewportChanges)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        for (const u32 width : { 1280u, 1920u })
        {
            const u32 height = width == 1280u ? 720u : 1080u;
            ResizeRenderTarget(width, height);
            LoadSubject(false);
            ResizeRenderTarget(width, height);
            auto& wet = m_Coat.GetComponent<GroomWetnessComponent>();
            wet.m_Immersion = 1.0f;
            wet.m_SoakedLine = 0.6f; // the horse soaked to its knees
            SetRepresentation(false);
            Entity lamp = GetScene().CreateEntity("WetCellsLamp");
            lamp.GetComponent<TransformComponent>().Translation = { 1.2f, 1.6f, 1.4f };
            auto& point = lamp.AddComponent<PointLightComponent>();
            point.m_Intensity = 2.0f;
            point.m_Range = 4.0f;
            point.m_CastShadows = false;
            for (const auto& [path, pathName] : kPaths)
            {
                ForEachAttachmentFeedbackCell(path, pathName, [&](const std::string& cell, u32 settle)
                                              {
                    if (cell.find("Rejected1537") != std::string::npos)
                    {
                        std::printf("[groom-wet-cells] %s %ux%u rejected (#1537)\n", cell.c_str(), width, height);
                        return;
                    }
                    EditorCamera camera(45.0f, static_cast<f32>(width) / static_cast<f32>(height), 0.02f, 200.0f);
                    camera.SetViewportSize(static_cast<f32>(width), static_cast<f32>(height));
                    camera.SetPose(glm::vec3(3.0f, 1.2f, 4.0f), -std::atan2(3.0f, 4.0f), std::asin(0.2f / glm::length(glm::vec3(3.0f, 0.2f, 4.0f))));
                    const auto frame = [&](bool wetOn, std::vector<u8>& pixels)
                    {
                        Time::SetMockTime(12.0f);
                        wet.m_Enabled = wetOn;
                        Renderer3D::ResetFrameSequences();
                        RunEditorFrames(camera, std::max(8u, settle), 0.0f);
                        u32 w = 0;
                        u32 h = 0;
                        ASSERT_TRUE(ReadbackComposite(pixels, w, h));
                    };
                    std::vector<u8> dry, repeat, soaked;
                    frame(false, dry);
                    frame(false, repeat);
                    frame(true, soaked);
                    const auto differing = [](const std::vector<u8>& a, const std::vector<u8>& b)
                    {
                        u32 count = 0;
                        for (sizet i = 0; i + 3u < std::min(a.size(), b.size()); i += 4u)
                            count += std::abs(static_cast<i32>(a[i]) - b[i]) > 2 || std::abs(static_cast<i32>(a[i + 1]) - b[i + 1]) > 2 ||
                                     std::abs(static_cast<i32>(a[i + 2]) - b[i + 2]) > 2;
                        return count;
                    };
                    const u32 noise = differing(dry, repeat);
                    const u32 changed = differing(dry, soaked);
                    const auto& stats = Renderer3D::GetGroomRenderPass()->GetStats();
                    std::printf("[groom-wet-cells] %s %ux%u changed=%u repeat=%u clustered=%d\n", cell.c_str(), width, height,
                                changed, noise, stats.ClusteredLightLists ? 1 : 0);
                    EXPECT_GT(changed, noise + 8u) << "the wet coat is not in this cell's frame";
                    EXPECT_EQ(stats.ClusteredLightLists, path != RenderingPath::Forward)
                        << "the strands walk the clusters exactly where the clusters are live";
                    ExpectAttachmentFeedbackGraphClean("GroomPass"); });
            }
            GetScene().DestroyEntity(lamp);
        }
    }
} // namespace OloEngine::Tests

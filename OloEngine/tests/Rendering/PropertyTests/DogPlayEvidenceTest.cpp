// OLO_TEST_LAYER: L8
//
// =============================================================================
// DogPlayEvidenceTest — playing with the three dogs (issue #1559).
//
// Every case loads a breed's play scene as shipped (Scenes/<Breed>Play.olo),
// starts the pieces Play starts (3D physics, the navmesh, the scene's own Lua
// scripts) and drives it on RUNTIME frames through Scene::OnUpdateRuntime, the
// way the editor's Play does. The player is simulated through SyntheticInput --
// the cursor and the buttons the PlayHand script reads -- so a case exercises
// the whole chain: screen ray, physics pick or pick volumes, carry, throw, the
// dog's navigation on root motion, its clips and its socket.
//
//   * Fetch: a ball flicked across the lawn is chased, picked up in the mouth,
//     carried back and dropped at the player's feet within a bound. Negative
//     control: with fetching switched off the ball stays where it landed.
//   * Petting: a stroke along the back displaces the coat's guides where the
//     hand passes and nowhere else; the dog switches to its petted reaction, a
//     favourite spot (the ears) more strongly than the back. Negative control: a
//     stroke that misses the dog leaves the coat as an untouched coat is.
//
// The fur-parting evidence (several angles, every render path, both coat tiers,
// with and without TAA) is DogPlayEvidenceTest.TheHandPartsTheCoat*, further down.
// =============================================================================

#include "OloEnginePCH.h"
#include "RendererAttachedTest.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"
#include "OloEngine/Animation/IKTargetComponent.h"
#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Core/Input.h"
#include "OloEngine/Core/SyntheticInput.h"
#include "OloEngine/Utils/PlatformUtils.h"
#include "OloEngine/Groom/GroomGuideSimulation.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Passes/GroomRenderPass.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"
#include "OloEngine/Scripting/C#/ScriptEngine.h"
#include "OloEngine/Scripting/Lua/LuaScriptEngine.h"

#include <glad/gl.h>
#include <gtest/gtest.h>
#include <stb_image/stb_image_write.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
        constexpr f32 kDt = 1.0f / 60.0f;

        struct Breed
        {
            const char* Name;
            const char* Scene;
            // An OpaqueRibbon coat (the composition that needs no temporal
            // resolve) draws visible fibres. The full-fur breeds' strands are
            // under half a pixel wide at native resolution and drop out, so
            // their TAA-off cell has no coat to measure.
            bool RibbonCoatVisible;
        };
        constexpr std::array kBreeds{ Breed{ "Golden", "DogPlay", true }, Breed{ "Bernese", "BernesePlay", false },
                                      Breed{ "Samoyed", "SamoyedPlay", false } };

        std::string BreedName(const ::testing::TestParamInfo<Breed>& info)
        {
            return info.param.Name;
        }

        struct Frame
        {
            std::vector<u8> Pixels;
            std::vector<i32> Ids;
        };

        struct Cell
        {
            const char* Name;
            RenderingPath Path;
            bool Cards = false;
            bool Stochastic = true; // false: OpaqueRibbon, which needs no temporal resolve
            bool Upscaled = false;
        };

        struct ChangeCount
        {
            u32 Coat = 0;    // pixels the coat covers in either frame
            u32 Inside = 0;  // coat pixels that changed within the hand's reach on screen
            u32 Outside = 0; // ... and beyond it
        };

        // Coat pixels (by entity id) whose colour moved past the 8-bit noise of a
        // repeat, split by the distance from the hand on screen.
        ChangeCount CountChanges(const Frame& a, const Frame& b, i32 coat, const glm::vec2& centre, f32 reach)
        {
            ChangeCount c;
            for (u32 row = 0; row < kHeight; ++row)
            {
                for (u32 col = 0; col < kWidth; ++col)
                {
                    // Readback rows run bottom-up; the centre is top-down.
                    const sizet i = static_cast<sizet>(row) * kWidth + col;
                    if (a.Ids[i] != coat && b.Ids[i] != coat)
                        continue;
                    ++c.Coat;
                    bool moved = a.Ids[i] != b.Ids[i];
                    for (u32 k = 0; k < 3u && !moved; ++k)
                        moved = std::abs(static_cast<i32>(a.Pixels[i * 4u + k]) - b.Pixels[i * 4u + k]) > 8;
                    if (!moved)
                        continue;
                    const glm::vec2 pixel(static_cast<f32>(col) + 0.5f, static_cast<f32>(kHeight - row) - 0.5f);
                    (glm::length(pixel - centre) <= reach ? c.Inside : c.Outside) += 1u;
                }
            }
            return c;
        }
    } // namespace

    class DogPlayEvidenceTest : public RendererAttachedTest, public ::testing::WithParamInterface<Breed>
    {
      protected:
        Ref<Project> m_PreviousProject;
        Ref<AssetManagerBase> m_PreviousAssets;
        bool m_StateCaptured = false;
        bool m_Playing = false;
        bool m_LuaStarted = false;
        std::vector<std::filesystem::path> m_TempScripts;

        Entity m_Dog;
        Entity m_Camera;
        Entity m_Ball;
        Entity m_Hand;
        Entity m_Coat;

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
            StopPlay();
            SyntheticInput::Reset();
            Time::ClearMockTime();
            for (const auto& path : m_TempScripts)
            {
                std::error_code ec;
                std::filesystem::remove(path, ec);
            }
            RendererAttachedTest::TearDown();
            if (!m_StateCaptured)
                return;
            Project::Unload();
            if (m_PreviousProject)
                Project::NewInMemory(m_PreviousProject->GetDirectory(), m_PreviousProject->GetConfig());
            if (m_PreviousAssets)
                Project::SetAssetManager(m_PreviousAssets);
        }

        // The breed's play scene as shipped, and Play's start: physics, the
        // navmesh, and the scene's Lua scripts (resolved project-relative, as
        // Scene::OnRuntimeStart does). `scripts` false leaves the dog and the
        // player's hand unscripted, for the evidence cases that place the hand.
        void LoadPlayScene(const Breed& breed, bool scripts = true)
        {
            Time::SetMockTime(12.0f); // the lawn's wind runs on this clock: pinned, the grass repeats
            const auto path = kEditor / "SandboxProject/Assets/Scenes" / (std::string(breed.Scene) + ".olo");
            ASSERT_TRUE(SceneSerializer(GetSceneRef()).Deserialize(path.string())) << path.string();
            EnableRendering(kWidth, kHeight);
            GetScene().OnViewportResize(kWidth, kHeight);
            m_Dog = GetScene().FindEntityByName("Dog");
            m_Camera = GetScene().FindEntityByName("RuntimeCamera");
            m_Ball = GetScene().FindEntityByName("ToyBall");
            m_Hand = GetScene().FindEntityByName("Hand");
            m_Coat = GetScene().FindEntityByName("DogCoat");
            ASSERT_TRUE(m_Dog && m_Camera && m_Ball && m_Hand && m_Coat);

            GetScene().OnPhysics3DStart();
            ASSERT_TRUE(GetScene().BakeNavMesh()) << "the lawn gave no navmesh";
            if (scripts)
            {
                LuaScriptEngine::Init();
                LuaScriptEngine::OnRuntimeStart(&GetScene());
                m_LuaStarted = true;
                ScriptEngine::SetSceneContextForTesting(&GetScene());
                GetScene().SetRunning(true);
                for (const auto e : GetScene().GetAllEntitiesWith<LuaScriptComponent>())
                {
                    Entity entity{ e, &GetScene() };
                    const auto& file = entity.GetComponent<LuaScriptComponent>().ScriptFile;
                    LuaScriptEngine::OnCreateEntity(entity, Project::GetAssetFileSystemPath(file).string());
                }
            }
            m_Playing = true;
        }

        void StopPlay()
        {
            if (!m_Playing)
                return;
            m_Playing = false;
            if (m_LuaStarted)
            {
                // The Lua state outlives the scene: a case's mood or probe must not reach the next.
                RunLua("PlayConfig = nil; PlayBus = nil; ProbeState = nil; ProbeEpoch = nil");
                m_LuaStarted = false;
                LuaScriptEngine::OnRuntimeStop();
                ScriptEngine::SetSceneContextForTesting(nullptr);
            }
            GetScene().OnPhysics3DStop();
        }

        // Run `source` once, as a Lua script's OnCreate, in the scene's Lua state.
        void RunLua(const std::string& source)
        {
            const auto path = std::filesystem::temp_directory_path() /
                              ("olo_dog_play_" + std::to_string(m_TempScripts.size()) + ".lua");
            std::ofstream(path, std::ios::binary | std::ios::trunc)
                << "return { OnCreate = function(id)\n"
                << source << "\nend }\n";
            m_TempScripts.push_back(path);
            Entity probe = GetScene().CreateEntity("LuaProbe");
            LuaScriptEngine::OnCreateEntity(probe, path.string());
        }

        [[nodiscard]] glm::vec3 WorldOf(Entity entity) const
        {
            return glm::vec3(GetScene().GetWorldTransform(static_cast<entt::entity>(entity))[3]);
        }

        // Where the cursor must be (pixels, top-left origin) to point at `world`
        // through the runtime camera.
        [[nodiscard]] glm::vec2 PixelOf(const glm::vec3& world) const
        {
            const glm::mat4 view = glm::inverse(GetScene().GetWorldTransform(static_cast<entt::entity>(m_Camera)));
            const glm::mat4 projection = m_Camera.GetComponent<CameraComponent>().Camera.GetProjection();
            const glm::vec4 clip = projection * view * glm::vec4(world, 1.0f);
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            return { (ndc.x * 0.5f + 0.5f) * kWidth, (0.5f - ndc.y * 0.5f) * kHeight };
        }

        void SetMouse(const glm::vec2& pixel, bool left, bool right = false)
        {
            SyntheticInput::SetMousePosition(pixel);
            SyntheticInput::SetMouseButton(Mouse::ButtonLeft, left);
            SyntheticInput::SetMouseButton(Mouse::ButtonRight, right);
        }

        void Tick(u32 frames = 1)
        {
            for (u32 i = 0; i < frames; ++i)
            {
                Input::Update();
                RunFrames(1, kDt);
            }
        }

        [[nodiscard]] Entity ParentOf(Entity entity) const
        {
            return entity.GetParent();
        }

        // The player's feet as both scripts define them: on the ground, 1.3 m in
        // front of the camera toward the play spot.
        [[nodiscard]] glm::vec2 PlayerFeet() const
        {
            const glm::vec3 camera = WorldOf(m_Camera);
            const glm::vec3 focus = WorldOf(GetScene().FindEntityByName("PlayFocus"));
            const glm::vec2 toFocus = glm::normalize(glm::vec2(focus.x - camera.x, focus.z - camera.z));
            return glm::vec2(camera.x, camera.z) + toFocus * 1.3f;
        }

        // Grab the ball under the cursor and flick it up the screen: a throw
        // away from the camera.
        void ThrowBall()
        {
            glm::vec2 pixel = PixelOf(WorldOf(m_Ball));
            SetMouse(pixel, false);
            Tick(2);
            SetMouse(pixel, true);
            Tick(2);
            ASSERT_EQ(ParentOf(m_Ball), m_Hand) << "the cursor on the ball did not grab it";
            for (u32 i = 0; i < 6; ++i)
            {
                pixel.y -= 40.0f;
                SetMouse(pixel, true);
                Tick();
            }
            SetMouse(pixel, false);
            Tick();
            ASSERT_FALSE(ParentOf(m_Ball)) << "releasing the button did not let the ball go";
        }

        [[nodiscard]] std::string CurrentClip() const
        {
            const auto& anim = m_Dog.GetComponent<AnimationStateComponent>();
            return anim.m_CurrentClip ? anim.m_CurrentClip->Name : std::string();
        }

        [[nodiscard]] const GroomGuideSimulationState* Guides() const
        {
            return GetScene().FindGroomGuideSimulation(m_Coat.GetUUID());
        }

        // Per guide: how far its particles sit from the groomed shape, worst particle.
        [[nodiscard]] std::vector<f32> GuideDisplacement() const
        {
            std::vector<f32> out;
            const auto* state = Guides();
            if (state == nullptr || state->LastTargets.size() != state->Curr.size())
                return out;
            for (sizet g = 0; g + 1 < state->GuideOffsets.size(); ++g)
            {
                f32 worst = 0.0f;
                for (u32 i = state->GuideOffsets[g]; i < state->GuideOffsets[g + 1]; ++i)
                    worst = std::max(worst, glm::length(state->Curr[i] - state->LastTargets[i]));
                out.push_back(worst);
            }
            return out;
        }

        [[nodiscard]] std::vector<glm::vec3> GuideRoots() const
        {
            std::vector<glm::vec3> out;
            if (const auto* state = Guides())
                for (sizet g = 0; g + 1 < state->GuideOffsets.size(); ++g)
                    out.push_back(state->LastTargets[state->GuideOffsets[g]]);
            return out;
        }

        // A point on the dog's coat under the cursor, through the same pick the
        // hand uses.
        [[nodiscard]] bool PickDog(const glm::vec2& pixel, PickVolumeHit& hit)
        {
            glm::vec3 origin;
            glm::vec3 direction;
            const glm::vec2 uv(pixel.x / kWidth, 1.0f - pixel.y / kHeight);
            return GetScene().ScreenToWorldRay(m_Camera, uv, origin, direction) &&
                   GetScene().RaycastPickVolumes(origin, direction, 40.0f, hit) && hit.Entity == m_Dog.GetUUID();
        }

        // Right-drag the camera round to the dog's upper flank, through PlayHand's
        // own orbit control: from there the cursor reaches the back.
        void OrbitToTheFlank()
        {
            glm::vec2 pixel(kWidth * 0.5f, kHeight * 0.5f);
            SetMouse(pixel, false, false);
            Tick(1);
            SetMouse(pixel, false, true);
            Tick(1);
            for (u32 i = 0; i < 10u; ++i)
            {
                pixel += glm::vec2(-15.3f, 6.7f);
                SetMouse(pixel, false, true);
                Tick(1);
            }
            SetMouse(pixel, false, false);
            Tick(1);
        }

        // External (hand or toy) colliders the coat's last frame was solved against.
        [[nodiscard]] u32 ExternalColliders() const
        {
            u32 count = 0;
            for (const GroomCollider& collider : GetScene().FindGroomSimulationColliders(m_Coat.GetUUID()))
                count += (collider.Flags & GroomColliderFlags::External) != 0u ? 1u : 0u;
            return count;
        }

        // How far the nearest simulated guide particle's target is from the hand,
        // and how many guides the coat simulates this frame.
        [[nodiscard]] std::pair<f32, u32> NearestGuideToTheHand() const
        {
            const auto* state = Guides();
            if (state == nullptr || state->GuideOffsets.size() < 2)
                return { std::numeric_limits<f32>::max(), 0u };
            const glm::vec3 hand = WorldOf(m_Hand);
            f32 nearest = std::numeric_limits<f32>::max();
            for (const glm::vec3& p : state->LastTargets)
                nearest = std::min(nearest, glm::length(p - hand));
            return { nearest, static_cast<u32>(state->GuideOffsets.size() - 1) };
        }

        // Particles the GROOM puts inside the hand this frame that COULD leave it,
        // and how many of those the SIMULATED coat still holds well inside.
        // `skinNormal` is the coat's outward normal under the hand, whose centre
        // sits `lift` above the skin. The hand pushes a particle away from its
        // centre: one below the centre, within the disc where the sphere dips
        // into the skin (sqrt(r^2 - lift^2) of the axis), is pushed INTO the body
        // and is held between the two -- a hand pressing a thick coat flat. It
        // has nowhere to go and is not counted.
        [[nodiscard]] std::pair<u32, u32> ParticlesInsideTheHand(const glm::vec3& skinNormal, f32 lift) const
        {
            const auto* state = Guides();
            if (state == nullptr || state->LastTargets.size() != state->Curr.size())
                return { 0u, 0u };
            const glm::vec3 hand = WorldOf(m_Hand);
            const f32 radius = m_Hand.GetComponent<GroomColliderComponent>().m_Radius;
            const f32 pressedDisc = std::sqrt(std::max(0.0f, radius * radius - lift * lift));
            u32 rest = 0;
            u32 now = 0;
            for (sizet g = 0; g + 1 < state->GuideOffsets.size(); ++g)
            {
                for (u32 i = state->GuideOffsets[g] + 1u; i < state->GuideOffsets[g + 1]; ++i)
                {
                    const glm::vec3 offset = state->LastTargets[i] - hand;
                    if (glm::length(offset) >= radius)
                        continue;
                    const f32 up = glm::dot(offset, skinNormal);
                    if (up < 0.0f && glm::length(offset - up * skinNormal) < pressedDisc)
                        continue;
                    ++rest;
                    now += glm::length(state->Curr[i] - hand) < 0.8f * radius ? 1u : 0u;
                }
            }
            return { rest, now };
        }

        // Run `body` every frame as `entity`'s Lua OnUpdate (`id` is the entity).
        void RunLuaEachFrame(Entity entity, const std::string& body)
        {
            const auto path = std::filesystem::temp_directory_path() /
                              ("olo_dog_play_" + std::to_string(m_TempScripts.size()) + ".lua");
            std::ofstream(path, std::ios::binary | std::ios::trunc)
                << "return { OnUpdate = function(id, dt)\n"
                << body << "\nend }\n";
            m_TempScripts.push_back(path);
            // The script engine updates entities that carry the component.
            entity.AddOrReplaceComponent<LuaScriptComponent>().ScriptFile = path.string();
            LuaScriptEngine::OnCreateEntity(entity, path.string());
        }

        // The runtime camera on a sphere round the middle of the dog's back.
        void PlaceCamera(f32 yaw, f32 pitch, f32 distance)
        {
            const glm::vec3 focus = BoneWorld("spine_02");
            const glm::vec3 position = focus + distance * glm::vec3(std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                                                                    std::cos(pitch) * std::cos(yaw));
            auto& transform = m_Camera.GetComponent<TransformComponent>();
            transform.Translation = position;
            transform.SetRotationEuler(glm::vec3(-pitch, yaw, 0.0f));
            Tick(1); // compose the camera's world transform before anything projects through it
        }

        // Where PlayHand would put the hand for a touch on the middle of the
        // back from this camera: the skin the pick hit, then 25 mm out. NaN on a miss.
        [[nodiscard]] glm::vec3 HandOnTheBack()
        {
            PickVolumeHit hit;
            if (!PickDog(PixelOf(BoneWorld("spine_02")), hit))
                return glm::vec3(std::numeric_limits<f32>::quiet_NaN());
            return hit.Position + hit.Normal * 0.025f;
        }

        void SetHand(bool onTheCoat, const glm::vec3& at)
        {
            m_Hand.GetComponent<TransformComponent>().Translation = onTheCoat ? at : glm::vec3(0.0f, -50.0f, 0.0f);
            m_Hand.GetComponent<GroomColliderComponent>().m_Enabled = onTheCoat;
        }

        // One more runtime frame, read back: the composite and the entity ids the
        // groom pass left.
        void CaptureFrame(Frame& out)
        {
            auto graph = RenderGraphDebugRuntime::GetActiveGraph();
            ASSERT_TRUE(graph);
            out.Ids.assign(static_cast<sizet>(kWidth) * kHeight, -1);
            struct RemoveHook
            {
                Ref<RenderGraph> Graph;
                ~RemoveHook()
                {
                    Graph->RemovePostPassHook("DogPlayEvidence");
                }
            } removeHook{ graph };
            graph->AddPostPassHook("DogPlayEvidence", [&](std::string_view pass, RenderGraph& g)
                                   {
                if (pass != "GroomPass")
                    return;
                const u32 texture = g.ResolveTexture(g.GetTextureHandle("SceneEntityID"));
                if (texture == 0u)
                    return;
                // At a reduced render scale the id target is smaller than the
                // composite: read it at its own size and map it onto the frame.
                GLint w = 0;
                GLint h = 0;
                glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_WIDTH, &w);
                glGetTextureLevelParameteriv(texture, 0, GL_TEXTURE_HEIGHT, &h);
                if (w <= 0 || h <= 0)
                    return;
                std::vector<i32> ids(static_cast<sizet>(w) * static_cast<sizet>(h), -1);
                glGetTextureImage(texture, 0, GL_RED_INTEGER, GL_INT, static_cast<GLsizei>(ids.size() * sizeof(i32)), ids.data());
                for (u32 row = 0; row < kHeight; ++row)
                    for (u32 col = 0; col < kWidth; ++col)
                        out.Ids[static_cast<sizet>(row) * kWidth + col] =
                            ids[static_cast<sizet>(row * static_cast<u32>(h) / kHeight) * static_cast<sizet>(w) + col * static_cast<u32>(w) / kWidth]; });
            Tick(1);
            u32 width = 0;
            u32 height = 0;
            ASSERT_TRUE(ReadbackComposite(out.Pixels, width, height));
            ASSERT_EQ(width, kWidth);
            ASSERT_EQ(height, kHeight);
        }

        // `frames` consecutive runtime frames, averaged: a stochastic coat changes
        // every one of its pixels from frame to frame, so a single frame cannot
        // be compared with another. The ids are the union: coat wherever any
        // averaged frame drew coat.
        void CaptureAveraged(Frame& out, u32 frames = 16u)
        {
            // Every arm replays the same jitter and stochastic sequences, so two
            // arms of an unchanged coat draw the same pixels and a difference is
            // the coat's.
            Renderer3D::ResetFrameSequences();
            std::vector<u32> sum;
            out.Ids.assign(static_cast<sizet>(kWidth) * kHeight, -1);
            const auto coatId = static_cast<i32>(static_cast<u32>(static_cast<entt::entity>(m_Coat)));
            for (u32 f = 0; f < frames; ++f)
            {
                Frame frame;
                CaptureFrame(frame);
                if (sum.empty())
                    sum.assign(frame.Pixels.size(), 0u);
                for (sizet i = 0; i < frame.Pixels.size(); ++i)
                    sum[i] += frame.Pixels[i];
                for (sizet i = 0; i < frame.Ids.size(); ++i)
                    if (frame.Ids[i] == coatId || out.Ids[i] == -1)
                        out.Ids[i] = frame.Ids[i];
            }
            out.Pixels.resize(sum.size());
            for (sizet i = 0; i < sum.size(); ++i)
                out.Pixels[i] = static_cast<u8>((sum[i] + frames / 2u) / frames);
        }

        // Evidence stays out of git (generated-evidence-stays-out-of-git.md).
        static void WritePng(const Frame& frame, const std::string& name)
        {
            const auto directory = kEditor.parent_path() / ".v-build/1559/visual";
            std::filesystem::create_directories(directory);
            stbi_flip_vertically_on_write(1);
            EXPECT_NE(stbi_write_png((directory / (name + ".png")).string().c_str(), static_cast<i32>(kWidth),
                                     static_cast<i32>(kHeight), 4, frame.Pixels.data(), static_cast<i32>(kWidth * 4u)),
                      0);
            stbi_flip_vertically_on_write(0);
        }

        // Start the dog from this mood (DogPlay's PlayConfig), its choices seeded.
        void SetMood(f32 energy, f32 excitement, f32 affection, f32 curiosity)
        {
            char source[256];
            std::snprintf(source, sizeof(source),
                          "PlayConfig = { seed = 1559, mood = { energy = %.3f, excitement = %.3f, affection = %.3f, "
                          "curiosity = %.3f } }",
                          energy, excitement, affection, curiosity);
            RunLua(source);
        }

        // The cursor up in the sky, button up: nothing for the dog to look at.
        void ParkCursor()
        {
            SetMouse({ kWidth * 0.5f, 2.0f }, false);
        }

        // Where the dog's head points, relative to its body: radians about up,
        // positive toward the body's +X.
        [[nodiscard]] f32 HeadYaw() const
        {
            const glm::mat4 body = GetScene().GetWorldTransform(static_cast<entt::entity>(m_Dog));
            const glm::vec3 forward = glm::normalize(glm::vec3(body[2]));
            const glm::vec3 side = glm::normalize(glm::vec3(body[0]));
            const glm::vec3 look = BoneWorld("jaw") - BoneWorld("head");
            return std::atan2(glm::dot(look, side), glm::dot(look, forward));
        }

        // The head's median yaw over `frames`, after a second to turn. The
        // median passes over the dog's brief glances elsewhere.
        [[nodiscard]] f32 HeadYawOver(u32 frames)
        {
            Tick(60);
            std::vector<f32> samples;
            for (u32 i = 0; i < frames; ++i)
            {
                Tick();
                samples.push_back(HeadYaw());
            }
            std::ranges::sort(samples);
            return samples[samples.size() / 2];
        }

        [[nodiscard]] static bool OnScreen(const glm::vec2& pixel)
        {
            return pixel.x > 0.0f && pixel.y > 0.0f && pixel.x < kWidth && pixel.y < kHeight;
        }

        // The bone's world position on the current pose.
        [[nodiscard]] glm::vec3 BoneWorld(const char* bone) const
        {
            const auto& skeleton = m_Dog.GetComponent<SkeletonComponent>().m_Skeleton;
            const auto& names = skeleton->m_BoneNames;
            const auto it = std::ranges::find(names, std::string(bone));
            EXPECT_NE(it, names.end()) << bone;
            const sizet index = static_cast<sizet>(std::distance(names.begin(), it));
            return glm::vec3(GetScene().GetWorldTransform(static_cast<entt::entity>(m_Dog)) * skeleton->m_GlobalTransforms[index][3]);
        }
    };

    // -------------------------------------------------------------------------
    // Fetch
    // -------------------------------------------------------------------------

    TEST_P(DogPlayEvidenceTest, AFlickedBallIsFetchedAndDroppedAtThePlayersFeet)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        LoadPlayScene(GetParam());
        GetScene().SetRenderingEnabled(false); // fetching needs no coat on screen; the clock is the same
        Tick(60);

        const glm::vec3 start = WorldOf(m_Ball);
        ThrowBall();

        const Entity ballSocket = GetScene().FindEntityByName("MouthSocketBall");
        f32 landedAt = -1.0f;
        f32 pickedAt = -1.0f;
        f32 droppedAt = -1.0f;
        f32 farthest = 0.0f;
        bool pickedBeforeLanding = false;
        f32 time = 0.0f;
        // Reported live: the dog carried the toy home looking off to the side,
        // and its head flicked left and right as it went from one thing to the
        // next. The head's yaw off the body, every frame of the fetch, and
        // while the toy is carried.
        std::vector<f32> headYaw;
        std::vector<f32> carryYaw;
        for (u32 frame = 0; frame < 60u * 40u && droppedAt < 0.0f; ++frame)
        {
            Tick();
            time += kDt;
            headYaw.push_back(HeadYaw());
            if (pickedAt >= 0.0f && ParentOf(m_Ball) == ballSocket && time > pickedAt + 1.0f)
                carryYaw.push_back(std::abs(headYaw.back()));
            const glm::vec3 ball = WorldOf(m_Ball);
            farthest = std::max(farthest, glm::length(glm::vec2(ball.x - start.x, ball.z - start.z)));
            if (landedAt < 0.0f && ball.y < 0.06f && !ParentOf(m_Ball))
                landedAt = time;
            const bool inMouth = ParentOf(m_Ball) == ballSocket;
            if (inMouth && pickedAt < 0.0f)
            {
                pickedAt = time;
                pickedBeforeLanding = landedAt < 0.0f;
            }
            if (pickedAt >= 0.0f && !inMouth && !ParentOf(m_Ball))
                droppedAt = time;
        }
        std::printf("[dog-play] %s fetch: thrown %.2f m, landed %.2f s, picked up %.2f s, dropped %.2f s\n",
                    GetParam().Name, farthest, landedAt, pickedAt, droppedAt);

        EXPECT_GT(farthest, 1.5f) << "the flick did not throw the ball across the lawn";
        ASSERT_GE(pickedAt, 0.0f) << "the dog never picked the ball up";
        EXPECT_FALSE(pickedBeforeLanding) << "the dog caught a ball that had not come down";
        EXPECT_LT(pickedAt, 15.0f) << "the dog took too long to reach the ball";
        ASSERT_GE(droppedAt, 0.0f) << "the dog never brought the ball back";
        EXPECT_LT(droppedAt, 30.0f) << "the dog took too long to bring the ball back";

        // A flick: the head swings more than 0.2 rad one way and back again
        // within 0.6 s. The turning points of the yaw, then any out-and-back.
        std::vector<sizet> turns;
        for (sizet i = 1; i + 1 < headYaw.size(); ++i)
            if ((headYaw[i] - headYaw[i - 1]) * (headYaw[i + 1] - headYaw[i]) < 0.0f)
                turns.push_back(i);
        u32 flicks = 0;
        for (sizet t = 0; t + 2 < turns.size(); ++t)
        {
            const f32 out = std::abs(headYaw[turns[t + 1]] - headYaw[turns[t]]);
            const f32 back = std::abs(headYaw[turns[t + 2]] - headYaw[turns[t + 1]]);
            if (out > 0.2f && back > 0.2f && static_cast<f32>(turns[t + 2] - turns[t]) * kDt < 0.6f)
                ++flicks;
        }
        ASSERT_FALSE(carryYaw.empty()) << "no frame of the carry home was seen";
        std::ranges::sort(carryYaw);
        const f32 carryMedian = carryYaw[carryYaw.size() / 2];
        std::printf("[dog-play] %s fetch head: %u flicks; carrying home, head %.2f rad off the body (median), %.2f at most\n",
                    GetParam().Name, flicks, carryMedian, carryYaw.back());
        EXPECT_EQ(flicks, 0u) << "the head flicked one way and back";
        EXPECT_LT(carryMedian, 0.3f) << "the dog carried the toy home looking off to the side";

        // Let it come to rest, then judge where it lies.
        Tick(90);
        const glm::vec3 ball = WorldOf(m_Ball);
        const glm::vec2 feet = PlayerFeet();
        const f32 fromFeet = glm::length(glm::vec2(ball.x, ball.z) - feet);
        std::printf("[dog-play] %s ball rests %.2f m from the player's feet\n", GetParam().Name, fromFeet);
        EXPECT_LT(fromFeet, 1.0f) << "the ball was not dropped at the player's feet";
        EXPECT_FALSE(ParentOf(m_Ball)) << "the ball is still carried";
    }

    TEST_P(DogPlayEvidenceTest, WithFetchingOffAThrownBallStaysWhereItLanded)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        LoadPlayScene(GetParam());
        GetScene().SetRenderingEnabled(false);
        RunLua("PlayConfig = { fetchDisabled = true }");
        Tick(60);
        ThrowBall();
        // Until the ball has stopped: a second with under a millimetre of travel.
        bool everCarried = false;
        glm::vec3 last = WorldOf(m_Ball);
        u32 still = 0;
        for (u32 frame = 0; frame < 60u * 20u && still < 60u; ++frame)
        {
            Tick();
            everCarried = everCarried || static_cast<bool>(ParentOf(m_Ball));
            const glm::vec3 now = WorldOf(m_Ball);
            still = glm::length(now - last) < 1.0e-3f / 60.0f ? still + 1u : 0u;
            last = now;
        }
        ASSERT_GE(still, 60u) << "the thrown ball never came to rest";
        const glm::vec3 rest = WorldOf(m_Ball);
        for (u32 frame = 0; frame < 60u * 15u; ++frame)
        {
            Tick();
            everCarried = everCarried || static_cast<bool>(ParentOf(m_Ball));
        }
        EXPECT_FALSE(everCarried) << "the dog fetched a ball with fetching switched off";
        EXPECT_LT(glm::length(WorldOf(m_Ball) - rest), 0.05f) << "the ball moved after it came to rest";
    }

    // -------------------------------------------------------------------------
    // Petting, through the player's hand
    // -------------------------------------------------------------------------

    TEST_P(DogPlayEvidenceTest, AStrokeAlongTheBackPushesTheCoatOutOfTheHandAndTheDogReacts)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        LoadPlayScene(GetParam());
        OrbitToTheFlank();
        Tick(60); // the coat's solver settles on the idling dog

        // Along the back, shoulders to hips: the cursor sweeps between the two
        // points over a second.
        const glm::vec2 from = PixelOf(BoneWorld("spine_03"));
        const glm::vec2 to = PixelOf(BoneWorld("spine_01"));
        PickVolumeHit hit;
        ASSERT_TRUE(PickDog(from, hit)) << "the stroke's start does not point at the dog";
        EXPECT_EQ(hit.Region, "Torso");

        u32 framesOnCoat = 0;
        u32 framesSolvedAgainstTheHand = 0;
        f32 nearestGuide = std::numeric_limits<f32>::max();
        u32 simulatedGuides = 0;
        u64 restInside = 0;
        u64 nowInside = 0;
        bool reacted = false;
        constexpr u32 kStroke = 60;
        for (u32 i = 0; i <= kStroke; ++i)
        {
            const glm::vec2 pixel = glm::mix(from, to, static_cast<f32>(i) / kStroke);
            SetMouse(pixel, true);
            Tick();
            reacted = reacted || CurrentClip() == "Petted";
            const auto& hand = m_Hand.GetComponent<GroomColliderComponent>();
            if (!hand.m_Enabled)
                continue;
            ++framesOnCoat;
            framesSolvedAgainstTheHand += ExternalColliders() > 0u ? 1u : 0u;
            const auto [nearest, guides] = NearestGuideToTheHand();
            nearestGuide = std::min(nearestGuide, nearest);
            simulatedGuides = std::max(simulatedGuides, guides);
            PickVolumeHit under;
            if (!PickDog(pixel, under))
                continue;
            const auto [rest, now] = ParticlesInsideTheHand(under.Normal, glm::dot(WorldOf(m_Hand) - under.Position, under.Normal));
            restInside += rest;
            nowInside += now;
        }
        SetMouse(to, false);
        Tick();
        std::printf("[dog-play] %s stroke: %u frames on the coat, %u solved against the hand; %u guides simulated, nearest "
                    "%.1f mm from the hand's centre; groomed inside the hand %llu, simulated inside %llu\n",
                    GetParam().Name, framesOnCoat, framesSolvedAgainstTheHand, simulatedGuides, nearestGuide * 1000.0f,
                    static_cast<unsigned long long>(restInside), static_cast<unsigned long long>(nowInside));
        ASSERT_GT(framesOnCoat, kStroke / 2) << "the hand was not on the coat through the stroke";
        EXPECT_EQ(framesSolvedAgainstTheHand, framesOnCoat) << "the coat was not solved against the hand on it";
        ASSERT_GT(restInside, 0u) << "no groomed particle ever lay inside the hand: the stroke measured nothing";
        // THE parting, measured: the groom puts these particles inside the hand,
        // and the simulated coat holds few of them there.
        EXPECT_LT(static_cast<f64>(nowInside), 0.35 * static_cast<f64>(restInside))
            << "the hand did not push the coat's guides out of its way";
        EXPECT_TRUE(reacted) << "the dog did not react to being petted";
        EXPECT_FALSE(m_Hand.GetComponent<GroomColliderComponent>().m_Enabled) << "the lifted hand still parts the coat";
        EXPECT_EQ(ExternalColliders(), 0u) << "the lifted hand still reaches the coat";
    }

    TEST_P(DogPlayEvidenceTest, AStrokeThatMissesTheDogNeverReachesItsCoat)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        LoadPlayScene(GetParam());
        OrbitToTheFlank();
        Tick(60);
        // Beside the dog, on the lawn: no pick volume under the cursor.
        const glm::vec3 beside = WorldOf(m_Dog) + glm::vec3(0.0f, 0.02f, -1.4f);
        PickVolumeHit hit;
        ASSERT_FALSE(PickDog(PixelOf(beside), hit)) << "the miss must not point at the dog";
        for (u32 i = 0; i <= 60u; ++i)
        {
            SetMouse(PixelOf(beside) + glm::vec2(static_cast<f32>(i) * 2.0f, 0.0f), true);
            Tick();
            ASSERT_FALSE(m_Hand.GetComponent<GroomColliderComponent>().m_Enabled) << "a miss put the hand on the coat";
            ASSERT_EQ(ExternalColliders(), 0u) << "the coat was solved against a hand that never touched it";
            ASSERT_NE(CurrentClip(), "Petted") << "the dog reacted to a hand that never touched it";
        }
        SetMouse(PixelOf(beside), false);
        Tick();
    }

    TEST_P(DogPlayEvidenceTest, AFavouriteSpotEarnsAStrongerReactionThanTheBack)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        LoadPlayScene(GetParam());
        OrbitToTheFlank();
        Tick(30);
        // The dog's decision over one touch, as DogPlay published it: favourite
        // at any frame (x), the region it first felt (y: 1 Ears, 2 Torso, 3
        // other), the fastest its clock ran (z). Over the touch, not on its last
        // frame: the dog leans into the hand and tilts its head, so a cursor held
        // still ends up on another part of it.
        const Entity observer = GetScene().CreateEntity("ReactionObserver");
        RunLuaEachFrame(observer, R"(
if ProbeState == nil or ProbeState.epoch ~= ProbeEpoch then
    ProbeState = { epoch = ProbeEpoch, fav = false, code = 0, rate = 0 }
end
local r = PlayBus and PlayBus.reaction
if r then
    if ProbeState.code == 0 then
        ProbeState.code = (r.region == "Ears" and 1) or (r.region == "Torso" and 2) or 3
    end
    ProbeState.fav = ProbeState.fav or r.favourite
    local anim = entity_utils.get_component(entity_utils.find_by_name("Dog"), "AnimationStateComponent")
    ProbeState.rate = math.max(ProbeState.rate, anim.playbackSpeed)
end
entity_utils.set_translation(id, vec3.new(ProbeState.fav and 1 or 0, ProbeState.code, ProbeState.rate))
)");
        struct Reaction
        {
            bool Favourite = false;
            i32 Region = 0;
            f32 Rate = 0.0f;
            f32 Lean = 0.0f;
        };
        // The hand hovers over the spot first: the dog turns its head to a hand
        // coming near, so the spot is found again on the pose it settles into,
        // through whichever of the bones the camera sees as `region`.
        // `drop` lowers the spot from the bone: the spine runs along the top of
        // the back, and a cursor on its line sits on the silhouette's edge, which
        // a breathing, leaning dog moves out from under it.
        const auto reactionTo = [&](std::initializer_list<const char*> bones, const char* region, f32 drop) -> Reaction
        {
            RunLua("ProbeEpoch = (ProbeEpoch or 0) + 1; if PlayBus then PlayBus.reaction = nil end");
            const glm::vec3 down(0.0f, -drop, 0.0f);
            SetMouse(PixelOf(BoneWorld(*bones.begin()) + down), false);
            Tick(60);
            glm::vec2 pixel(0.0f);
            bool found = false;
            for (const char* bone : bones)
            {
                PickVolumeHit hit;
                const glm::vec2 candidate = PixelOf(BoneWorld(bone) + down);
                if (!found && PickDog(candidate, hit) && hit.Region == region)
                {
                    pixel = candidate;
                    found = true;
                }
            }
            EXPECT_TRUE(found) << "no " << region << " bone is under the cursor from this camera";
            f32 lean = 0.0f;
            u32 leaningOnTheHand = 0;
            u32 handOnTheCoat = 0;
            constexpr u32 kTouch = 45u;
            for (u32 i = 0; i < kTouch; ++i)
            {
                SetMouse(pixel + glm::vec2(std::sin(static_cast<f32>(i) * 0.4f) * 1.5f, 0.0f), true);
                Tick();
                const auto& ik = m_Dog.GetComponent<IKTargetComponent>();
                lean = ik.AimWeight; // where the lean has settled by the end of the touch
                leaningOnTheHand += ik.AimTargetEntity == m_Hand.GetUUID() ? 1u : 0u;
                handOnTheCoat += m_Hand.GetComponent<GroomColliderComponent>().m_Enabled ? 1u : 0u;
            }
            const glm::vec3 seen = observer.GetComponent<TransformComponent>().Translation;
            EXPECT_EQ(CurrentClip(), "Petted") << region;
            // The dog keeps the spot under the hand: it does not pull away from it.
            EXPECT_GT(handOnTheCoat, kTouch * 3u / 4u) << "the dog moved the spot out from under the hand";
            // On the body it turns into the hand. (On the head it holds its head
            // still in the hand and looks up at the player instead.)
            if (std::string(region) == "Torso")
                EXPECT_GT(leaningOnTheHand, kTouch * 3u / 4u) << "the dog does not lean into the hand";
            SetMouse(pixel, false);
            Tick(60);
            std::printf("[dog-play] %s petting %s: the dog felt region %d (1 Ears, 2 Torso, 3 other), favourite %d, "
                        "clip rate x%.2f, lean weight %.2f\n",
                        GetParam().Name, region, static_cast<i32>(seen.y + 0.5f), seen.x > 0.5f ? 1 : 0, seen.z, lean);
            return { seen.x > 0.5f, static_cast<i32>(seen.y + 0.5f), seen.z, lean };
        };
        const Reaction back = reactionTo({ "spine_02" }, "Torso", 0.08f);
        const Reaction ear = reactionTo({ "ear_L_02", "ear_R_02", "ear_L_03", "ear_R_03", "ear_L_01", "ear_R_01" }, "Ears", 0.0f);
        EXPECT_FALSE(back.Favourite);
        EXPECT_EQ(back.Region, 2);
        EXPECT_TRUE(ear.Favourite);
        EXPECT_EQ(ear.Region, 1);
        EXPECT_GT(ear.Rate, back.Rate) << "the tail does not wag faster on a favourite spot";
        EXPECT_GT(ear.Lean, back.Lean) << "the dog does not lean in harder on a favourite spot";
    }

    // -------------------------------------------------------------------------
    // Alive: attention and initiative between the games
    // -------------------------------------------------------------------------
    //
    // Each case starts the dog from a set mood (PlayConfig.mood, seeded), so the
    // initiative under test is the one the mood calls for, and holds the
    // opposite mood (or nothing to react to) as its negative control.

    TEST_P(DogPlayEvidenceTest, WasdWalksThePlaySpotAndTheCameraFollows)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        LoadPlayScene(GetParam());
        GetScene().SetRenderingEnabled(false);
        ParkCursor();
        Tick(10);
        const Entity focus = GetScene().FindEntityByName("PlayFocus");
        const glm::vec3 focusStart = WorldOf(focus);
        const glm::vec3 cameraStart = WorldOf(m_Camera);
        const glm::vec2 away = glm::normalize(glm::vec2(focusStart.x - cameraStart.x, focusStart.z - cameraStart.z));
        // A second of W: forward, away from the camera.
        SyntheticInput::SetKey(Key::W, true);
        Tick(60);
        SyntheticInput::SetKey(Key::W, false);
        Tick(1);
        const glm::vec3 focusAfter = WorldOf(focus);
        const glm::vec3 cameraAfter = WorldOf(m_Camera);
        const glm::vec2 moved(focusAfter.x - focusStart.x, focusAfter.z - focusStart.z);
        std::printf("[dog-play] %s W for 1 s: play spot moved %.2f m (%.2f m along the view)\n", GetParam().Name,
                    glm::length(moved), glm::dot(moved, away));
        EXPECT_NEAR(glm::dot(moved, away), 2.5f, 0.3f) << "W did not walk the play spot away from the camera";
        EXPECT_NEAR(focusAfter.y, focusStart.y, 1e-4f);
        EXPECT_NEAR(glm::length(cameraAfter - cameraStart), glm::length(focusAfter - focusStart), 0.05f)
            << "the camera did not follow the play spot";
        // Nothing pressed: nothing moves (the control).
        Tick(30);
        EXPECT_LT(glm::length(WorldOf(focus) - focusAfter), 1e-4f) << "the play spot drifts with no key held";
    }

    TEST_P(DogPlayEvidenceTest, ThePlayerWalkingOffWithAToyIsFollowed)
    {
        // Reported live: walking off with a toy in hand, the dog stood where it
        // was and watched it go. The player picks the ball up, backs away with S
        // for two seconds (five metres) and stands. The dog must come after them.
        OLO_ENSURE_GPU_OR_SKIP();
        LoadPlayScene(GetParam());
        GetScene().SetRenderingEnabled(false);
        SetMood(0.8f, 0.5f, 0.3f, 0.0f);
        Tick(10);
        const glm::vec2 pixel = PixelOf(WorldOf(m_Ball));
        SetMouse(pixel, false);
        Tick(2);
        SetMouse(pixel, true);
        Tick(2);
        ASSERT_EQ(ParentOf(m_Ball), m_Hand) << "the cursor on the ball did not grab it";
        SetMouse(pixel - glm::vec2(0.0f, 60.0f), true);
        Tick(60);
        const auto toFeet = [&]
        {
            const glm::vec3 dog = WorldOf(m_Dog);
            return glm::length(glm::vec2(dog.x, dog.z) - PlayerFeet());
        };
        const glm::vec3 dogStart = WorldOf(m_Dog);
        const f32 before = toFeet();
        SyntheticInput::SetKey(Key::S, true);
        Tick(120);
        SyntheticInput::SetKey(Key::S, false);
        const f32 leftBehind = toFeet();
        Tick(360);
        const f32 after = toFeet();
        const glm::vec3 dogEnd = WorldOf(m_Dog);
        const f32 walked = glm::length(glm::vec2(dogEnd.x - dogStart.x, dogEnd.z - dogStart.z));
        SetMouse(pixel, false);
        Tick();
        std::printf("[dog-play] %s player walked off with the toy: dog %.2f m from their feet, %.2f when they "
                    "stopped, %.2f six seconds later; it walked %.2f m\n",
                    GetParam().Name, before, leftBehind, after, walked);
        ASSERT_GT(leftBehind, 3.0f) << "the player did not walk away from the dog: this measures nothing";
        EXPECT_GT(walked, 2.5f) << "the dog stayed where it was";
        EXPECT_LT(after, 2.2f) << "the dog did not come up to the player";
        EXPECT_GT(after, 0.5f) << "the dog walked into the player";
    }

    TEST_P(DogPlayEvidenceTest, AHeldToyDrawnDownTheScreenStaysOnTheLawn)
    {
        // Found live: held at the depth it was picked up at, a toy drawn down the
        // screen went under the lawn, and let go there it fell for ever.
        OLO_ENSURE_GPU_OR_SKIP();
        LoadPlayScene(GetParam());
        GetScene().SetRenderingEnabled(false);
        Tick(10);
        const glm::vec2 pixel = PixelOf(WorldOf(m_Ball));
        SetMouse(pixel, false);
        Tick(2);
        SetMouse(pixel, true);
        Tick(2);
        ASSERT_EQ(ParentOf(m_Ball), m_Hand) << "the cursor on the ball did not grab it";
        // Up first: the toy follows the cursor (the control: holding works at all).
        SetMouse(pixel - glm::vec2(0.0f, 80.0f), true);
        Tick(10);
        const f32 raised = WorldOf(m_Ball).y;
        // Then down to the bottom of the screen, well past where the ball lay.
        f32 lowest = std::numeric_limits<f32>::max();
        for (u32 i = 0; i < 30u; ++i)
        {
            SetMouse({ pixel.x, std::min(static_cast<f32>(kHeight) - 2.0f, pixel.y + 20.0f * static_cast<f32>(i)) }, true);
            Tick();
            lowest = std::min(lowest, WorldOf(m_Ball).y);
        }
        SetMouse({ pixel.x, static_cast<f32>(kHeight) - 2.0f }, false);
        Tick(90);
        const f32 rested = WorldOf(m_Ball).y;
        std::printf("[dog-play] %s held ball: raised to %.3f m, lowest held %.3f m, rests at %.3f m\n", GetParam().Name,
                    raised, lowest, rested);
        EXPECT_GT(raised, 0.1f) << "the held toy did not follow the cursor up";
        EXPECT_GT(lowest, 0.0f) << "the held toy went under the lawn";
        EXPECT_GT(rested, 0.0f) << "the toy let go fell through the lawn";
        EXPECT_LT(rested, 0.2f) << "the toy let go did not come down to rest on the lawn";
    }

    TEST_P(DogPlayEvidenceTest, AToyCarriedRoundTheDogTurnsItsWholeBodyNotJustItsHead)
    {
        // Reported live: with a toy moved round it, the dog turned its head alone,
        // and swept across it the head spun on the neck. The toy is swept the
        // way a player sweeps it: across the screen at the depth it was picked
        // up at, left to right and back, then straight across the top of the
        // dog's head. The body must follow the toy round, the head must stay
        // within a neck's reach of the body, and it must never whip round.
        OLO_ENSURE_GPU_OR_SKIP();
        LoadPlayScene(GetParam());
        GetScene().SetRenderingEnabled(false);
        SetMood(0.8f, 0.5f, 0.2f, 0.0f);
        ParkCursor();
        Tick(30);
        const glm::vec2 pixel = PixelOf(WorldOf(m_Ball));
        SetMouse(pixel, false);
        Tick(2);
        SetMouse(pixel, true);
        Tick(2);
        ASSERT_EQ(ParentOf(m_Ball), m_Hand) << "the cursor on the ball did not grab it";

        const auto bodyYaw = [&]
        {
            const glm::mat4 body = GetScene().GetWorldTransform(static_cast<entt::entity>(m_Dog));
            return std::atan2(body[2].x, body[2].z);
        };
        const glm::vec3 centre = WorldOf(m_Dog);
        const f32 startYaw = bodyYaw();
        f32 minYaw = 0.0f;
        f32 maxYaw = 0.0f;
        // The head's look (head bone to jaw) in the body's frame: its whole angle
        // off the body's forward, pitch and roll as well as yaw, and how fast that
        // look direction swings.
        const auto headLook = [&]
        {
            const glm::mat4 bodyNow = GetScene().GetWorldTransform(static_cast<entt::entity>(m_Dog));
            const glm::vec3 look = glm::normalize(BoneWorld("jaw") - BoneWorld("head"));
            return glm::vec3(glm::dot(look, glm::normalize(glm::vec3(bodyNow[0]))),
                             glm::dot(look, glm::normalize(glm::vec3(bodyNow[1]))),
                             glm::dot(look, glm::normalize(glm::vec3(bodyNow[2]))));
        };
        const glm::vec3 restLook = headLook();
        f32 worstHead = 0.0f;
        f32 fastestHead = 0.0f;
        glm::vec3 lastLook = restLook;
        f32 minToy = 0.0f;
        f32 maxToy = 0.0f;
        const auto track = [&]
        {
            const f32 turned = std::remainder(bodyYaw() - startYaw, 6.2831853f);
            minYaw = std::min(minYaw, turned);
            maxYaw = std::max(maxYaw, turned);
            const glm::vec3 look = headLook();
            {
                const glm::vec3 ball = WorldOf(m_Ball);
                const f32 toy = std::remainder(std::atan2(ball.x - centre.x, ball.z - centre.z) - startYaw, 6.2831853f);
                minToy = std::min(minToy, toy);
                maxToy = std::max(maxToy, toy);
            }
            worstHead = std::max(worstHead, std::acos(std::clamp(glm::dot(look, restLook), -1.0f, 1.0f)));
            fastestHead = std::max(fastestHead, std::acos(std::clamp(glm::dot(look, lastLook), -1.0f, 1.0f)) / kDt);
            lastLook = look;
        };
        constexpr u32 kSweep = 240u;
        const f32 row = std::max(40.0f, pixel.y - 120.0f);
        for (u32 i = 0; i <= 2u * kSweep; ++i)
        {
            // Out to the left edge, across to the right one, and back to the middle.
            const f32 t = static_cast<f32>(i) / kSweep;
            const f32 u = t < 0.25f ? 0.5f - 2.0f * t : (t < 1.25f ? -0.0f + (t - 0.25f) : 1.0f - (t - 1.25f) * (0.5f / 0.75f));
            SetMouse({ std::clamp(u, 0.03f, 0.97f) * static_cast<f32>(kWidth), row }, true);
            Tick();
            track();
        }
        // Then the toy is picked up again from right under the dog's chin, so it is
        // held at the head's own depth, and swept straight across the head, side
        // to side, at a brisk pace: it slides along the fur within centimetres of
        // the head, which is where the head spun.
        SetMouse(pixel, false);
        Tick(30);
        const glm::mat4 body = GetScene().GetWorldTransform(static_cast<entt::entity>(m_Dog));
        const glm::vec3 forward = glm::normalize(glm::vec3(body[2].x, 0.0f, body[2].z));
        const glm::vec3 across = glm::normalize(glm::vec3(body[0].x, 0.0f, body[0].z));
        const glm::vec3 chin = BoneWorld("head");
        m_Ball.GetComponent<TransformComponent>().Translation = glm::vec3(chin.x, 0.05f, chin.z) + forward * 0.15f;
        Tick(30);
        const glm::vec2 chinPixel = PixelOf(WorldOf(m_Ball));
        SetMouse(chinPixel, false);
        Tick(2);
        SetMouse(chinPixel, true);
        Tick(2);
        ASSERT_EQ(ParentOf(m_Ball), m_Hand) << "the cursor on the ball under the chin did not grab it";
        const glm::vec3 headTop = BoneWorld("head") + glm::vec3(0.0f, 0.12f, 0.0f);
        f32 nearest = std::numeric_limits<f32>::max();
        for (u32 i = 0; i <= 90u; ++i)
        {
            const f32 sweep = -0.5f + static_cast<f32>(i) / 90.0f;
            SetMouse(PixelOf(headTop + across * sweep), true);
            Tick();
            track();
            nearest = std::min(nearest, glm::length(WorldOf(m_Ball) - BoneWorld("head")));
        }
        SetMouse(PixelOf(headTop), false);
        Tick();
        std::printf("[dog-play] %s toy swept across the head, nearest %.2f m from it\n", GetParam().Name, nearest);
        EXPECT_LT(nearest, 0.4f) << "the sweep never brought the toy near the head: the spin case is not exercised";
        std::printf("[dog-play] %s toy carried round: toy from %.2f to %.2f rad, body turned %.2f to %.2f rad, head at "
                    "most %.2f rad off the body, head at most %.1f rad/s\n",
                    GetParam().Name, minToy, maxToy, minYaw, maxYaw, worstHead, fastestHead);
        // Across the whole screen the toy spans about a radian as the dog sees it.
        ASSERT_GT(maxToy - minToy, 0.8f) << "the sweep did not carry the toy round the dog: this measures nothing";
        // At either end of the sweep the body comes round to within the head's
        // easy reach of the toy; the head covers the rest.
        EXPECT_GT(maxYaw, maxToy - 0.7f) << "the body did not follow the toy round to one side";
        EXPECT_LT(minYaw, minToy + 0.7f) << "the body did not follow the toy round to the other side";
        EXPECT_LT(worstHead, 1.3f) << "the head turned further off the body than a neck allows";
        EXPECT_LT(fastestHead, 6.0f) << "the head whipped round";
    }

    TEST_P(DogPlayEvidenceTest, AToyHeldUpBehindTheDogTurnsItRoundWithoutTwistingItsNeck)
    {
        // Reported live: with the dog facing away and a toy held up behind it,
        // the head turned round on the neck while the body stood still. The dog
        // is turned to face away from the camera, the toy is held up between the
        // camera and the dog -- behind it -- and moved a little side to side.
        // The dog must turn round to it; its head must stay within a neck's reach.
        OLO_ENSURE_GPU_OR_SKIP();
        LoadPlayScene(GetParam());
        GetScene().SetRenderingEnabled(false);
        SetMood(0.8f, 0.5f, 0.2f, 0.0f);
        ParkCursor();
        const glm::vec3 camera = WorldOf(m_Camera);
        const glm::vec3 dog = WorldOf(m_Dog);
        const f32 away = std::atan2(dog.x - camera.x, dog.z - camera.z);
        m_Dog.GetComponent<TransformComponent>().SetRotationEuler(glm::vec3(0.0f, away, 0.0f));
        Tick(20);
        const glm::vec2 pixel = PixelOf(WorldOf(m_Ball));
        SetMouse(pixel, false);
        Tick(2);
        SetMouse(pixel, true);
        Tick(2);
        ASSERT_EQ(ParentOf(m_Ball), m_Hand) << "the cursor on the ball did not grab it";

        const auto bodyYaw = [&]
        {
            const glm::mat4 body = GetScene().GetWorldTransform(static_cast<entt::entity>(m_Dog));
            return std::atan2(body[2].x, body[2].z);
        };
        const glm::vec2 up = pixel - glm::vec2(0.0f, 100.0f);
        f32 worstHead = 0.0f;
        f32 startOff = 0.0f;
        f32 endOff = 0.0f;
        for (u32 i = 0; i <= 300u; ++i)
        {
            SetMouse(up + glm::vec2(std::sin(static_cast<f32>(i) * 0.05f) * 60.0f, 0.0f), true);
            Tick();
            worstHead = std::max(worstHead, std::abs(HeadYaw()));
            const glm::vec3 ball = WorldOf(m_Ball);
            const glm::vec3 here = WorldOf(m_Dog);
            const f32 off = std::abs(std::remainder(std::atan2(ball.x - here.x, ball.z - here.z) - bodyYaw(), 6.2831853f));
            if (i == 0u)
                startOff = off;
            endOff = off;
        }
        SetMouse(up, false);
        Tick();
        std::printf("[dog-play] %s toy held up behind: toy %.2f rad off the body at first, %.2f after five seconds; head "
                    "at most %.2f rad off the body\n",
                    GetParam().Name, startOff, endOff, worstHead);
        ASSERT_GT(startOff, 2.0f) << "the toy did not start behind the dog: this measures nothing";
        EXPECT_LT(endOff, 0.6f) << "the dog did not turn round to the toy behind it";
        EXPECT_LT(worstHead, 1.3f) << "the head turned further off the body than a neck allows";
    }

    TEST_P(DogPlayEvidenceTest, TheDogWatchesACursorNearItAndNotOneFarAway)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        LoadPlayScene(GetParam());
        GetScene().SetRenderingEnabled(false);
        SetMood(0.8f, 0.25f, 0.2f, 0.0f); // content: stays put, nothing to fetch or to ask for
        ParkCursor();
        Tick(60);

        // On the lawn beside the dog's muzzle, then out on the lawn behind the
        // dog as the camera sees it, left and right, beyond what the dog notices.
        const glm::mat4 body = GetScene().GetWorldTransform(static_cast<entt::entity>(m_Dog));
        const glm::vec3 forward = glm::normalize(glm::vec3(body[2].x, 0.0f, body[2].z));
        const glm::vec3 side = glm::normalize(glm::vec3(body[0].x, 0.0f, body[0].z));
        const glm::vec3 muzzle = WorldOf(m_Dog) + forward * 0.7f + glm::vec3(0.0f, 0.02f, 0.0f);
        const auto yawAt = [&](const glm::vec3& point)
        {
            const glm::vec2 pixel = PixelOf(point);
            EXPECT_TRUE(OnScreen(pixel)) << "the cursor's point is off the screen";
            PickVolumeHit onDog;
            EXPECT_FALSE(PickDog(pixel, onDog)) << "the cursor's ray meets the dog on its way to the lawn";
            SetMouse(pixel, false);
            return HeadYawOver(180);
        };
        const f32 nearLeft = yawAt(muzzle + side * 0.6f);
        const f32 nearRight = yawAt(muzzle - side * 0.6f);
        const glm::vec3 camera = WorldOf(m_Camera);
        const glm::vec3 away = glm::normalize(glm::vec3(muzzle.x - camera.x, 0.0f, muzzle.z - camera.z));
        const glm::vec3 across = glm::cross(away, glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::vec3 behind = muzzle + away * 3.5f;
        const f32 farLeft = yawAt(behind + across * (glm::dot(across, side) > 0.0f ? 1.8f : -1.8f));
        const f32 farRight = yawAt(behind - across * (glm::dot(across, side) > 0.0f ? 1.8f : -1.8f));
        std::printf("[dog-play] %s head yaw: near cursor left %.2f right %.2f, far cursor left %.2f right %.2f rad\n",
                    GetParam().Name, nearLeft, nearRight, farLeft, farRight);
        EXPECT_GT(nearLeft - nearRight, 0.3f) << "the head does not follow a cursor beside it";
        EXPECT_LT(std::abs(farLeft - farRight), 0.5f * (nearLeft - nearRight))
            << "the head follows a cursor too far away to matter: the measure is not the cursor";
    }

    TEST_P(DogPlayEvidenceTest, ADogThatWantsCompanyComesToThePlayer)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        LoadPlayScene(GetParam());
        GetScene().SetRenderingEnabled(false);
        ParkCursor();
        const auto fromFeet = [&]
        {
            const glm::vec3 dog = WorldOf(m_Dog);
            return glm::length(glm::vec2(dog.x, dog.z) - PlayerFeet());
        };
        const f32 start = fromFeet();
        ASSERT_GT(start, 1.3f) << "the dog starts at the player's feet: nothing to come over for";

        // Negative control first: had its fill of company, it stays where it is.
        SetMood(0.8f, 0.25f, 0.0f, 0.0f);
        f32 nearestContent = start;
        for (u32 frame = 0; frame < 60u * 8u; ++frame)
        {
            Tick();
            nearestContent = std::min(nearestContent, fromFeet());
        }
        // Then left alone wanting company: it comes over.
        SetMood(0.8f, 0.25f, 0.95f, 0.0f);
        f32 nearestLonely = fromFeet();
        f32 arrivedAt = -1.0f;
        for (u32 frame = 0; frame < 60u * 12u && arrivedAt < 0.0f; ++frame)
        {
            Tick();
            nearestLonely = std::min(nearestLonely, fromFeet());
            if (nearestLonely < 0.9f)
                arrivedAt = static_cast<f32>(frame) * kDt;
        }
        std::printf("[dog-play] %s company: starts %.2f m from the feet; content, nearest %.2f m; lonely, nearest %.2f m "
                    "(arrived %.2f s)\n",
                    GetParam().Name, start, nearestContent, nearestLonely, arrivedAt);
        EXPECT_GT(nearestContent, start - 0.4f) << "a content dog walked over to the player anyway";
        EXPECT_GE(arrivedAt, 0.0f) << "a dog that wants company did not come to the player";
    }

    TEST_P(DogPlayEvidenceTest, ATiredDogLiesDownAndRollsOverWhenPettedARestedOneLeansIn)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        LoadPlayScene(GetParam());
        GetScene().SetRenderingEnabled(false);
        OrbitToTheFlank();
        ParkCursor();
        const auto record = [&](std::vector<std::string>& clips)
        {
            if (clips.empty() || clips.back() != CurrentClip())
                clips.push_back(CurrentClip());
        };
        const auto pet = [&](u32 frames, std::vector<std::string>& clips)
        {
            const glm::vec2 pixel = PixelOf(BoneWorld("spine_02"));
            PickVolumeHit hit;
            EXPECT_TRUE(PickDog(pixel, hit)) << "the back is not under the cursor";
            for (u32 i = 0; i < frames; ++i)
            {
                SetMouse(pixel + glm::vec2(std::sin(static_cast<f32>(i) * 0.3f) * 6.0f, 0.0f), true);
                Tick();
                record(clips);
            }
            ParkCursor();
            Tick();
        };
        const auto saw = [](const std::vector<std::string>& clips, const char* clip)
        { return std::ranges::find(clips, std::string(clip)) != clips.end(); };
        const auto list = [](const std::vector<std::string>& clips)
        {
            std::string out;
            for (const auto& clip : clips)
                out += (out.empty() ? "" : " ") + clip;
            return out;
        };

        // Rested: never lies down, and petting makes it lean in, standing.
        SetMood(0.8f, 0.25f, 0.2f, 0.0f);
        std::vector<std::string> rested;
        for (u32 frame = 0; frame < 60u * 6u; ++frame)
        {
            Tick();
            record(rested);
        }
        std::vector<std::string> restedPetted;
        pet(120u, restedPetted);

        // Tired: lies down, and petting rolls it over for a belly rub.
        SetMood(0.1f, 0.25f, 0.2f, 0.0f);
        std::vector<std::string> tired;
        for (u32 frame = 0; frame < 60u * 6u; ++frame)
        {
            Tick();
            record(tired);
        }
        std::vector<std::string> tiredPetted;
        pet(180u, tiredPetted);

        std::printf("[dog-play] %s rested: %s | petted: %s\n", GetParam().Name, list(rested).c_str(),
                    list(restedPetted).c_str());
        std::printf("[dog-play] %s tired: %s | petted: %s\n", GetParam().Name, list(tired).c_str(),
                    list(tiredPetted).c_str());
        EXPECT_FALSE(saw(rested, "LieDown") || saw(rested, "Lie")) << "a rested dog lay down";
        EXPECT_TRUE(saw(restedPetted, "Petted")) << "a standing dog did not lean into the hand";
        EXPECT_FALSE(saw(restedPetted, "BellyUp")) << "a standing dog rolled over";
        EXPECT_TRUE(saw(tired, "LieDown")) << "a tired dog did not lie down";
        EXPECT_EQ(tired.back(), "Lie") << "a tired dog did not stay lying down";
        EXPECT_TRUE(saw(tiredPetted, "RollOver")) << "a lying dog did not roll over when petted";
        EXPECT_TRUE(saw(tiredPetted, "BellyUp")) << "a lying dog did not offer its belly";
    }

    TEST_P(DogPlayEvidenceTest, AToyHeldUpExcitesTheDog)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        LoadPlayScene(GetParam());
        GetScene().SetRenderingEnabled(false);
        SetMood(0.8f, 0.45f, 0.2f, 0.0f);
        ParkCursor();
        const auto invites = [&](u32 frames)
        {
            bool invited = false;
            f32 lookAtToy = 0.0f;
            for (u32 i = 0; i < frames; ++i)
            {
                Tick();
                invited = invited || CurrentClip() == "Excited" || CurrentClip() == "PlayBow";
                const auto& ik = m_Dog.GetComponent<IKTargetComponent>();
                if (glm::length(ik.AimTarget - WorldOf(m_Ball)) < 0.2f)
                    lookAtToy = std::max(lookAtToy, ik.AimWeight);
            }
            return std::pair{ invited, lookAtToy };
        };
        // Negative control: the ball lies where it is; nothing to get excited about.
        const auto [idleInvited, idleLook] = invites(60u * 4u);

        // The player picks the ball up and holds it up.
        const glm::vec2 pixel = PixelOf(WorldOf(m_Ball));
        SetMouse(pixel, false);
        Tick(2);
        SetMouse(pixel, true);
        Tick(2);
        ASSERT_EQ(ParentOf(m_Ball), m_Hand) << "the cursor on the ball did not grab it";
        const glm::vec2 heldUp = pixel + glm::vec2(0.0f, -60.0f);
        for (u32 i = 1; i <= 10u; ++i)
        {
            SetMouse(glm::mix(pixel, heldUp, static_cast<f32>(i) / 10.0f), true);
            Tick();
        }
        const auto [heldInvited, heldLook] = invites(60u * 4u);
        SetMouse(heldUp, false);
        Tick();
        std::printf("[dog-play] %s toy: lying, invited %d look %.2f; held up, invited %d look %.2f\n", GetParam().Name,
                    idleInvited ? 1 : 0, idleLook, heldInvited ? 1 : 0, heldLook);
        EXPECT_FALSE(idleInvited) << "the dog asked to play with nothing offered";
        EXPECT_TRUE(heldInvited) << "a toy held up did not make the dog ask to play";
        EXPECT_GT(heldLook, 0.5f) << "the dog did not watch the toy in the player's hand";
    }

    TEST_P(DogPlayEvidenceTest, AnExcitedDogFetchesAToyOnItsOwn)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        LoadPlayScene(GetParam());
        GetScene().SetRenderingEnabled(false);
        ParkCursor();
        // The ball out on the lawn, well away from the player's feet.
        m_Ball.GetComponent<TransformComponent>().Translation = WorldOf(m_Dog) + glm::vec3(-2.0f, 0.2f, -1.5f);
        const Entity stick = GetScene().FindEntityByName("ToyStick");
        ASSERT_TRUE(stick);
        const auto inMouth = [&](Entity toy) { return ParentOf(toy) && ParentOf(toy) != m_Hand; };
        const auto carried = [&] { return inMouth(m_Ball) || inMouth(stick); };

        // Negative control: calm, it leaves the toys lying.
        SetMood(0.9f, 0.2f, 0.6f, 0.0f);
        bool carriedCalm = false;
        for (u32 frame = 0; frame < 60u * 12u; ++frame)
        {
            Tick();
            carriedCalm = carriedCalm || carried();
        }
        // Excited: it fetches one and brings it to the player.
        SetMood(0.9f, 0.7f, 0.6f, 0.0f);
        bool carriedExcited = false;
        f32 restsFromFeet = std::numeric_limits<f32>::max();
        for (u32 frame = 0; frame < 60u * 35u; ++frame)
        {
            Tick();
            carriedExcited = carriedExcited || carried();
            if (carriedExcited && !carried())
            {
                Tick(90); // let it come to rest
                for (const Entity toy : { m_Ball, stick })
                {
                    const glm::vec3 at = WorldOf(toy);
                    restsFromFeet = std::min(restsFromFeet, glm::length(glm::vec2(at.x, at.z) - PlayerFeet()));
                }
                break;
            }
        }
        std::printf("[dog-play] %s own fetch: calm carried %d; excited carried %d, a toy rests %.2f m from the feet\n",
                    GetParam().Name, carriedCalm ? 1 : 0, carriedExcited ? 1 : 0, restsFromFeet);
        EXPECT_FALSE(carriedCalm) << "a calm dog fetched a toy nobody threw";
        EXPECT_TRUE(carriedExcited) << "an excited dog left the toys lying";
        EXPECT_LT(restsFromFeet, 1.0f) << "the dog did not bring the toy to the player";
    }

    // -------------------------------------------------------------------------
    // The fur parts under the hand: evidence on every path, both coat tiers,
    // with and without the temporal resolve, and at a reduced render scale.
    // -------------------------------------------------------------------------
    //
    // No scripts: the hand is placed on the coat the way PlayHand places it (on
    // the skin the pick hit, 25 mm out), but the dog does not react, so between the
    // hand-off and hand-on captures the only thing that differs is the coat under
    // the hand. The pose is HELD (playback speed 0) so the coat comes to rest in
    // both arms; each arm settles for the same number of runtime frames.
    // A third, hand-off repeat is the noise floor of the comparison.


    TEST_P(DogPlayEvidenceTest, CloseUpThePlaySceneDrawsTheCoatOnStrands)
    {
        // Reported live: close up, the face and legs looked scaly. The coat is
        // framed front three-quarter at a metre, as the player sees it, and
        // must be drawn on strands -- streamed in, not left on the card floor.
        OLO_ENSURE_GPU_OR_SKIP();
        LoadPlayScene(GetParam(), false);
        Renderer3D::GetPostProcessSettings() = GetScene().GetPostProcessSettings();
        Renderer3D::ApplyRendererSettings();
        m_Dog.GetComponent<AnimationStateComponent>().m_PlaybackSpeed = 0.0f;
        Tick(10);
        const glm::mat4 body = GetScene().GetWorldTransform(static_cast<entt::entity>(m_Dog));
        const f32 dogYaw = std::atan2(body[2].x, body[2].z);
        PlaceCamera(dogYaw + 0.6f, 0.2f, 1.0f);
        const auto* pass = Renderer3D::GetGroomRenderPass();
        ASSERT_NE(pass, nullptr);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        u32 frames = 0;
        while (std::chrono::steady_clock::now() < deadline)
        {
            Tick();
            ++frames;
            if (pass->GetStreamingStats().DetailDraws >= 1u && frames > 60u)
                break;
        }
        const auto stats = pass->GetStreamingStats();
        const auto& render = pass->GetStats();
        const auto budget = RepresentationStreaming::Get().GetStats();
        std::printf("[dog-play] %s close-up after %u frames: %u detail draws, %u fallback (reason %u), %u pending; %u "
                    "strands drawn; resident %llu / %llu MiB, upload cap %llu MiB, staging %llu / %llu MiB, denied "
                    "resident %llu upload %llu staging %llu, failed loads %llu\n",
                    GetParam().Name, frames, stats.DetailDraws, stats.FallbackDraws,
                    static_cast<u32>(stats.LastFallback), stats.Pending, render.StrandsDrawn,
                    budget.OptionalResidentGpuBytes >> 20u, budget.MaxResidentGpuBytes >> 20u,
                    budget.MaxUploadBytesPerFrame >> 20u, budget.StagingCpuBytes >> 20u,
                    budget.MaxStagingCpuBytes >> 20u, budget.DeniedForResident, budget.DeniedForUpload,
                    budget.DeniedForStaging, budget.FailedLoads);
        Frame frame;
        CaptureAveraged(frame);
        WritePng(frame, std::string("DogCloseUp_") + GetParam().Name);
        EXPECT_GE(stats.DetailDraws, 1u) << "the coat never left the card floor close up";
        EXPECT_EQ(stats.FallbackDraws, 0u) << "the coat is still drawn on its streaming floor close up";
    }

    TEST_P(DogPlayEvidenceTest, ACoatComingBackCloseDoesNotMarkItsBodyAgain)
    {
        // Seen live: as the dog ran off and came back, each coat tier it changed
        // to marked its body into the coat volume again on the main thread,
        // 150-200 ms a time -- the stutter of the "coat volume holds its body"
        // log line. The camera goes close, far, and close again; coming back,
        // no body is marked afresh: the earlier bake's is reused.
        OLO_ENSURE_GPU_OR_SKIP();
        LoadPlayScene(GetParam(), false);
        Renderer3D::GetPostProcessSettings() = GetScene().GetPostProcessSettings();
        Renderer3D::ApplyRendererSettings();
        m_Dog.GetComponent<AnimationStateComponent>().m_PlaybackSpeed = 0.0f;
        Tick(10);
        const glm::mat4 body = GetScene().GetWorldTransform(static_cast<entt::entity>(m_Dog));
        const f32 dogYaw = std::atan2(body[2].x, body[2].z);
        const auto* pass = Renderer3D::GetGroomRenderPass();
        ASSERT_NE(pass, nullptr);
        struct Marks
        {
            u32 Fresh = 0;
            u32 Reused = 0;
        };
        const auto run = [&](f32 distance, u32 frames)
        {
            PlaceCamera(dogYaw + 0.6f, 0.2f, distance);
            Marks marks;
            for (u32 i = 0; i < frames; ++i)
            {
                Tick();
                const auto& shadow = pass->GetStats().CoatShadow;
                marks.Fresh += shadow.BakeBodyMicroseconds > 0u ? 1u : 0u; // a reused body takes no marking time
                marks.Reused += shadow.BodyBakesReused;
            }
            return marks;
        };
        const Marks first = run(1.0f, 240u);
        const Marks away = run(14.0f, 240u);
        const Marks back = run(1.0f, 240u);
        std::printf("[dog-play] %s coat body: close %u marked (%u reused), far %u (%u), back close %u (%u)\n",
                    GetParam().Name, first.Fresh, first.Reused, away.Fresh, away.Reused, back.Fresh, back.Reused);
        // The first close-up bakes come with the scene's first frames, before the
        // camera is placed; what is measured is the return.
        ASSERT_GT(back.Fresh + back.Reused, 0u) << "coming back close baked no coat at all: this measures nothing";
        EXPECT_EQ(back.Fresh, 0u) << "coming back close, the coat marked its body again";
    }

    TEST_P(DogPlayEvidenceTest, TheHandPartsTheCoatUnderItAndNowhereElse)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        LoadPlayScene(GetParam(), false);
        Renderer3D::GetPostProcessSettings() = GetScene().GetPostProcessSettings();
        Renderer3D::ApplyRendererSettings();
        m_Dog.GetComponent<AnimationStateComponent>().m_PlaybackSpeed = 0.0f; // hold the pose
        Tick(60);

        const std::array cells{
            Cell{ "GL_Forward", RenderingPath::Forward },
            Cell{ "GL_ForwardPlus", RenderingPath::ForwardPlus },
            Cell{ "GL_Deferred", RenderingPath::Deferred },
            Cell{ "GL_Forward_Cards", RenderingPath::Forward, true },
            Cell{ "GL_Forward_NoTAA", RenderingPath::Forward, false, false },
            Cell{ "GL_Forward_Upscaled", RenderingPath::Forward, false, true, true },
        };
        struct Angle
        {
            const char* Name;
            f32 Yaw;
            f32 Pitch;
            f32 Distance;
        };
        const std::array angles{ Angle{ "Flank", 1.57f, 0.55f, 1.3f }, Angle{ "Above", 0.9f, 1.05f, 1.2f },
                                 Angle{ "Rear", 2.6f, 0.45f, 1.4f } };

        auto& groom = m_Coat.GetComponent<GroomComponent>();
        auto& lod = m_Coat.GetComponent<GroomLodComponent>();
        const GroomLodComponent shippedLod = lod;
        const u8 shippedMode = groom.m_CompositionMode;
        auto& pp = Renderer3D::GetPostProcessSettings();
        const PostProcessSettings shippedPost = pp;

        for (const Cell& cell : cells)
        {
            if (!cell.Stochastic && !GetParam().RibbonCoatVisible)
            {
                std::printf("[dog-play] %s %s: not measured, the opaque-ribbon coat draws no fibre at native "
                            "resolution on this breed (sub-half-pixel strands)\n",
                            GetParam().Name, cell.Name);
                continue;
            }
            // The cross-path cells are taken from every angle; the tier, resolve
            // and scale cells from the first, which shows the parting side-on.
            const u32 angleCount = (cell.Cards || !cell.Stochastic || cell.Upscaled) ? 1u : static_cast<u32>(angles.size());
            Renderer3D::GetRendererSettings().Path = cell.Path;
            lod = shippedLod;
            if (cell.Cards)
            {
                lod.m_Enabled = true;
                lod.m_CardPixelSize = 16384.0f; // every coat on screen is "far": the card tier
                lod.m_HoldFrames = 0;
            }
            groom.m_CompositionMode = cell.Stochastic ? shippedMode : static_cast<u8>(GroomCompositionMode::OpaqueRibbon);
            pp = shippedPost;
            if (cell.Upscaled)
            {
                pp.Upscale = UpscaleMode::Quality;
                pp.Technique = TemporalUpscalerUsable() ? UpscalerTechnique::Temporal : UpscalerTechnique::Spatial;
            }
            Renderer3D::ApplyRendererSettings();

            for (u32 a = 0; a < angleCount; ++a)
            {
                const Angle& angle = angles[a];
                SCOPED_TRACE(std::string(cell.Name) + "/" + angle.Name);
                PlaceCamera(angle.Yaw, angle.Pitch, angle.Distance);
                const glm::vec3 handAt = HandOnTheBack();
                ASSERT_TRUE(std::isfinite(handAt.x)) << "the back is not under the camera";

                Frame off;
                Frame repeat;
                Frame on;
                // Each arm settles 48 frames (six TAA jitter periods) and is the
                // average of the next 16 (two periods); the repeat is a second
                // hand-off arm, the comparison's noise floor.
                SetHand(false, handAt);
                Tick(48);
                CaptureAveraged(off);
                Tick(32);
                CaptureAveraged(repeat);
                SetHand(true, handAt);
                Tick(48);
                CaptureAveraged(on);
                SetHand(false, handAt);
                Tick(48);

                const std::string stem = std::string("DogPetting_") + GetParam().Name + "_" + cell.Name + "_" + angle.Name;
                WritePng(off, std::string("DogPettingOff_") + GetParam().Name + "_" + cell.Name + "_" + angle.Name);
                WritePng(on, stem);

                // Where the hand is on screen, and how far its reach extends there.
                const glm::vec2 centre = PixelOf(handAt);
                const glm::vec3 right = glm::normalize(glm::vec3(GetScene().GetWorldTransform(static_cast<entt::entity>(m_Camera))[0]));
                const f32 reach = glm::length(PixelOf(handAt + right * (m_Hand.GetComponent<GroomColliderComponent>().m_Radius + 0.05f)) - centre);
                const auto coatId = static_cast<i32>(static_cast<u32>(static_cast<entt::entity>(m_Coat)));
                const ChangeCount parted = CountChanges(off, on, coatId, centre, reach);
                const ChangeCount noise = CountChanges(off, repeat, coatId, centre, reach);
                std::printf("[dog-play] %s %s %s: coat %u px; hand at (%.0f, %.0f) r %.0f px: %u px changed inside its reach, "
                            "%u outside; noise %u / %u\n",
                            GetParam().Name, cell.Name, angle.Name, parted.Coat, centre.x, centre.y, reach, parted.Inside, parted.Outside,
                            noise.Inside, noise.Outside);
                EXPECT_GT(parted.Coat, 20000u) << "the coat hardly covers the frame: this capture measures nothing";
                // Under the hand: well past what an unchanged coat changes by.
                // Away from it: no more than the repeat's own drift (long hair
                // still settling in both arms) and a little spill from strands
                // rooted under the hand that reach past its footprint.
                EXPECT_GT(parted.Inside, std::max(500u, 2u * noise.Inside + 300u)) << "the coat did not part under the hand";
                // The temporal upscaler keeps a reconstruction history that
                // ResetFrameSequences does not replay, so its repeat drifts by
                // tens of thousands of pixels over the frame and the allowance
                // scales with that drift rather than with the footprint.
                const f64 drift = cell.Upscaled ? 1.5 : 1.25;
                EXPECT_LE(static_cast<f64>(parted.Outside), drift * noise.Outside + (cell.Upscaled ? 1.0 : 0.5) * parted.Inside)
                    << "the coat changed away from the hand";
            }
        }
        lod = shippedLod;
        groom.m_CompositionMode = shippedMode;
        pp = shippedPost;
        Renderer3D::GetRendererSettings().Path = RenderingPath::Forward;
        Renderer3D::ApplyRendererSettings();
    }

    INSTANTIATE_TEST_SUITE_P(Breeds, DogPlayEvidenceTest, ::testing::ValuesIn(kBreeds), BreedName);
} // namespace OloEngine::Tests

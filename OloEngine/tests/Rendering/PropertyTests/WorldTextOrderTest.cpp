// OLO_TEST_LAYER: L8
// =============================================================================
// WorldTextOrderTest.cpp -- which of two overlapping world texts is on top.
//
// World text (TextComponent) is drawn by the UI composite pass with the depth
// test off, so the order of the draws decides which text covers the other. It
// was the order the registry yields entities in. That is not a property of the
// scene: Scene::Copy, which Simulate and Play run on, yields them in another
// order, and two overlapping texts swapped places when the editor changed mode
// (#1700, found in the live verification of #1352).
//
// The order is now the camera's: farthest first, the nearest on top, ties by
// UUID. Scene::DrawWorldText is the one place every text site goes through.
// Sprites and circles are drawn by the same loops and swapped the same way;
// they go through DrawWorldSprites and DrawWorldCircles under the same rule.
//
// THE MEASURE. Two texts of the same string at the same place on screen, one
// red and one blue, one a little nearer the camera. The one on top shows all of
// its ink; the one below shows only what the other does not cover. Each text's
// ink is counted with the other text emptied, as the reference for "all of it".
//
// Classification: L8 (full pipeline, editor and runtime camera, readback).
// Runs in both test processes.
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kWidth = 384;
        constexpr u32 kHeight = 256;
        const char* const kString = "MMMMMM";

        struct Ink
        {
            u32 Red = 0;
            u32 Blue = 0;
        };

        // Pixels that are one text's own colour. A pixel where the two blend
        // (an antialiased edge of the top text over the other's ink) is neither.
        [[nodiscard]] Ink CountInk(const std::vector<u8>& rgba)
        {
            Ink ink;
            for (sizet i = 0; i + 3 < rgba.size(); i += 4)
            {
                if (rgba[i] > 200u && rgba[i + 1] < 70u && rgba[i + 2] < 70u)
                {
                    ++ink.Red;
                }
                else if (rgba[i] < 70u && rgba[i + 1] < 70u && rgba[i + 2] > 200u)
                {
                    ++ink.Blue;
                }
            }
            return ink;
        }
    } // namespace

    class WorldTextOrderTest : public RendererAttachedTest
    {
      protected:
        // Runs in the --olo-rhi=vulkan process too: frames are read back by
        // RHI handle and nothing here is a GL call.
        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return true;
        }

        void BuildScene() override
        {
            Scene& scene = GetScene();
            EnableRendering(kWidth, kHeight);
            scene.SetGridVisible(false);
            scene.SetWorldAxisHelperVisible(false);
            scene.SetLightGizmosVisible(false);

            // The runtime camera, where the editor camera of the test is.
            Entity camera = scene.CreateEntity("Camera");
            auto& cameraComponent = camera.AddComponent<CameraComponent>();
            cameraComponent.Primary = true;
            cameraComponent.Camera.SetPerspective(glm::radians(60.0f), 0.05f, 100.0f);
            cameraComponent.Camera.SetViewportSize(kWidth, kHeight);
            camera.GetComponent<TransformComponent>().Translation = { 0.0f, 0.0f, 6.0f };
        }

        // Created in this order on purpose: the test places each of them
        // nearer in turn, so one of the two cases has the registry's order
        // against it whichever way the registry iterates.
        void AddTexts(f32 redZ, f32 blueZ)
        {
            m_Red = AddText("Red", { 1.0f, 0.0f, 0.0f, 1.0f }, redZ);
            m_Blue = AddText("Blue", { 0.0f, 0.0f, 1.0f, 1.0f }, blueZ);
        }

        Entity AddText(const char* name, const glm::vec4& colour, f32 z)
        {
            Entity entity = GetScene().CreateEntity(name);
            auto& transform = entity.GetComponent<TransformComponent>();
            transform.Translation = { -2.6f, -0.4f, z };
            auto& text = entity.AddComponent<TextComponent>();
            text.TextString = kString;
            text.Color = colour;
            return entity;
        }

        [[nodiscard]] static EditorCamera MakeCamera()
        {
            EditorCamera camera(60.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 100.0f);
            camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
            camera.SetPose(glm::vec3(0.0f, 0.0f, 6.0f), 0.0f, 0.0f);
            return camera;
        }

        [[nodiscard]] std::vector<u8> EditFrame(Scene& scene)
        {
            const EditorCamera camera = MakeCamera();
            RunEditorFramesOn(scene, camera, 3);
            return Readback();
        }

        [[nodiscard]] std::vector<u8> Readback()
        {
            std::vector<u8> rgba;
            u32 width = 0;
            u32 height = 0;
            EXPECT_TRUE(ReadbackComposite(rgba, width, height)) << "the composited frame could not be read back";
            EXPECT_EQ(rgba.size(), static_cast<sizet>(kWidth) * kHeight * 4u);
            return rgba;
        }

        [[nodiscard]] std::vector<sizet> CaptureNameplatePixels()
        {
            RunFrames(3);
            const auto rgba = Readback();
            std::vector<sizet> pixels;
            for (sizet i = 0; i + 3 < rgba.size(); i += 4)
            {
                if (rgba[i] > 200u && rgba[i + 1] < 70u && rgba[i + 2] > 200u)
                    pixels.push_back(i / 4u);
            }
            return pixels;
        }

        // Each text's ink with the other one emptied: what "all of it" is.
        [[nodiscard]] Ink InkAlone()
        {
            Ink alone;
            m_Blue.GetComponent<TextComponent>().TextString.clear();
            alone.Red = CountInk(EditFrame(GetScene())).Red;
            m_Blue.GetComponent<TextComponent>().TextString = kString;
            m_Red.GetComponent<TextComponent>().TextString.clear();
            alone.Blue = CountInk(EditFrame(GetScene())).Blue;
            m_Red.GetComponent<TextComponent>().TextString = kString;
            return alone;
        }

        Entity m_Red;
        Entity m_Blue;
    };

    TEST_F(WorldTextOrderTest, ParentedNameplateCoversTheSamePixelsAsItsRootTwin)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        auto addPlate = [this](const char* name, const glm::vec3& position)
        {
            Entity entity = GetScene().CreateEntity(name);
            entity.GetComponent<TransformComponent>().Translation = position;
            auto& plate = entity.AddComponent<NameplateComponent>();
            plate.m_ShowHealthBar = true;
            plate.m_ShowManaBar = false;
            plate.m_WorldOffset = { 0.0f, 0.2f, 0.0f };
            plate.m_BarSize = { 48.0f, 12.0f };
            plate.m_BarBackgroundColor = { 1.0f, 0.0f, 1.0f, 1.0f };
            return entity;
        };

        Entity rig = GetScene().CreateEntity("Nameplate Rig");
        rig.GetComponent<TransformComponent>().Translation = { 8.0f, 0.0f, 0.0f };
        Entity child = addPlate("Child Nameplate", { -8.0f, 0.0f, 0.0f });
        child.SetParent(rig);
        Entity root = addPlate("Root Nameplate", glm::vec3(0.0f));
        root.GetComponent<NameplateComponent>().m_Enabled = false;
        const auto childPixels = CaptureNameplatePixels();

        child.GetComponent<NameplateComponent>().m_Enabled = false;
        root.GetComponent<NameplateComponent>().m_Enabled = true;
        const auto rootPixels = CaptureNameplatePixels();
        std::printf("[nameplate] parented covered %zu pixels, root covered %zu pixels\n", childPixels.size(), rootPixels.size());
        EXPECT_GT(rootPixels.size(), 400u);
        EXPECT_EQ(childPixels, rootPixels);
    }

    // The nearer text is on top, whichever of the two was created first, in an
    // editor frame and in a runtime frame.
    TEST_F(WorldTextOrderTest, TheTextNearerTheCameraIsOnTop)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        AddTexts(/*redZ=*/0.0f, /*blueZ=*/0.0f);
        const Ink alone = InkAlone();
        std::printf("[world-text] ink alone: red %u px, blue %u px\n", alone.Red, alone.Blue);
        ASSERT_GT(alone.Red, 600u) << "the red text is not on screen: this measures nothing";
        ASSERT_GT(alone.Blue, 600u) << "the blue text is not on screen: this measures nothing";

        struct Case
        {
            const char* Name;
            f32 RedZ;
            f32 BlueZ;
            bool RedOnTop;
        };
        for (const Case& c : { Case{ "red nearer", 0.06f, 0.0f, true }, Case{ "blue nearer", 0.0f, 0.06f, false } })
        {
            m_Red.GetComponent<TransformComponent>().Translation.z = c.RedZ;
            m_Blue.GetComponent<TransformComponent>().Translation.z = c.BlueZ;
            const Ink editor = CountInk(EditFrame(GetScene()));
            RunFrames(3);
            const Ink runtime = CountInk(Readback());
            std::printf("[world-text] %s: editor red %u blue %u, runtime red %u blue %u\n", c.Name, editor.Red, editor.Blue,
                        runtime.Red, runtime.Blue);
            for (const Ink& frame : { editor, runtime })
            {
                const u32 top = c.RedOnTop ? frame.Red : frame.Blue;
                const u32 under = c.RedOnTop ? frame.Blue : frame.Red;
                const u32 topAlone = c.RedOnTop ? alone.Red : alone.Blue;
                const u32 underAlone = c.RedOnTop ? alone.Blue : alone.Red;
                // The top text keeps its ink (it is a little larger for being
                // nearer, never smaller); the one under it loses most of its.
                EXPECT_GE(top, topAlone * 9u / 10u) << c.Name << ": the nearer text is covered";
                EXPECT_LT(under, underAlone / 2u) << c.Name << ": the farther text is drawn over the nearer one";
            }
        }
    }

    // Two texts at one depth keep their order in a copy of the scene, which is
    // what Simulate and Play draw. Before the order was defined, the copy drew
    // them the other way round.
    TEST_F(WorldTextOrderTest, ACopyOfTheSceneStacksCoplanarTextsTheSameWay)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        AddTexts(/*redZ=*/0.0f, /*blueZ=*/0.0f);
        const Ink alone = InkAlone();
        ASSERT_GT(alone.Red, 600u);
        ASSERT_GT(alone.Blue, 600u);

        const std::vector<u8> original = EditFrame(GetScene());
        const Ink originalInk = CountInk(original);
        // The premise: one of the two is on top, so their order is visible.
        ASSERT_TRUE(originalInk.Red < alone.Red / 2u || originalInk.Blue < alone.Blue / 2u)
            << "neither text covers the other: the two frames below would be equal whatever the order";

        Ref<Scene> source = GetSceneRef();
        Ref<Scene> copy = Scene::Copy(source);
        ASSERT_TRUE(copy);
        copy->SetIs3DModeEnabled(true);
        copy->OnViewportResize(kWidth, kHeight);
        copy->SetRenderingEnabled(true);
        copy->SetGridVisible(false);
        copy->SetWorldAxisHelperVisible(false);
        copy->SetLightGizmosVisible(false);
        const std::vector<u8> copied = EditFrame(*copy);
        const Ink copiedInk = CountInk(copied);
        std::printf("[world-text] coplanar: original red %u blue %u, copy red %u blue %u\n", originalInk.Red, originalInk.Blue,
                    copiedInk.Red, copiedInk.Blue);

        ASSERT_EQ(copied.size(), original.size());
        u32 differing = 0;
        for (sizet i = 0; i + 3 < original.size(); i += 4)
        {
            if (original[i] != copied[i] || original[i + 1] != copied[i + 1] || original[i + 2] != copied[i + 2])
            {
                ++differing;
            }
        }
        EXPECT_EQ(differing, 0u) << "the copy of the scene draws its overlapping texts in another order";
    }

    // The same question for the other two world-space 2D primitives: do two
    // overlapping sprites, and two overlapping circles, stack the same way in a
    // copy of the scene? The runtime overlay draws them (the editor's 3D
    // overlay draws neither), so these are runtime frames.
    TEST_F(WorldTextOrderTest, ACopyOfTheSceneStacksCoplanarSpritesAndCirclesTheSameWay)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        Scene& scene = GetScene();
        const auto place = [&scene](const char* name, const glm::vec3& at)
        {
            Entity entity = scene.CreateEntity(name);
            entity.GetComponent<TransformComponent>().Translation = at;
            return entity;
        };
        // Each pair overlaps by half, so both colours of a pair are on screen
        // and the overlap shows which is on top.
        place("RedSprite", { -2.0f, 1.0f, 0.0f }).AddComponent<SpriteRendererComponent>(glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
        place("BlueSprite", { -1.5f, 1.0f, 0.0f }).AddComponent<SpriteRendererComponent>(glm::vec4(0.0f, 0.0f, 1.0f, 1.0f));
        place("RedCircle", { 1.5f, -1.0f, 0.0f }).AddComponent<CircleRendererComponent>().Color = { 1.0f, 0.0f, 0.0f, 1.0f };
        place("BlueCircle", { 2.0f, -1.0f, 0.0f }).AddComponent<CircleRendererComponent>().Color = { 0.0f, 0.0f, 1.0f, 1.0f };

        const auto runtimeFrame = [this](Scene& target)
        {
            for (u32 i = 0; i < 3u; ++i)
            {
                RunInFrame([&target]()
                           { target.OnUpdateRuntime(Timestep(1.0f / 60.0f)); });
            }
            return Readback();
        };
        const std::vector<u8> original = runtimeFrame(scene);
        const Ink originalInk = CountInk(original);
        ASSERT_GT(originalInk.Red, 1000u) << "the red sprite and circle are not on screen";
        ASSERT_GT(originalInk.Blue, 1000u) << "the blue sprite and circle are not on screen";

        Ref<Scene> source = GetSceneRef();
        Ref<Scene> copy = Scene::Copy(source);
        ASSERT_TRUE(copy);
        copy->SetIs3DModeEnabled(true);
        copy->OnViewportResize(kWidth, kHeight);
        copy->SetRenderingEnabled(true);
        copy->SetGridVisible(false);
        copy->SetWorldAxisHelperVisible(false);
        copy->SetLightGizmosVisible(false);
        const std::vector<u8> copied = runtimeFrame(*copy);
        const Ink copiedInk = CountInk(copied);
        std::printf("[world-text] sprites and circles: original red %u blue %u, copy red %u blue %u\n", originalInk.Red,
                    originalInk.Blue, copiedInk.Red, copiedInk.Blue);
        ASSERT_EQ(copied.size(), original.size());
        u32 differing = 0;
        for (sizet i = 0; i + 3 < original.size(); i += 4)
        {
            if (original[i] != copied[i] || original[i + 1] != copied[i + 1] || original[i + 2] != copied[i + 2])
            {
                ++differing;
            }
        }
        EXPECT_EQ(differing, 0u) << "the copy of the scene draws its overlapping sprites or circles in another order";
    }
} // namespace OloEngine::Tests

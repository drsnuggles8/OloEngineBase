// OLO_TEST_LAYER: L4
#include "OloEnginePCH.h"
#include "RendererAttachedTest.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Debug/GLStateGuard.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Passes/WaterRenderPass.h"
#include "OloEngine/Renderer/RGCommandContext.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <gtest/gtest.h>
#include <glad/gl.h>

namespace OloEngine::Tests
{
    class GroomSoftIntersectionWaterReplay : public RendererAttachedTest
    {
        void BuildScene() override
        {
            auto water = GetScene().CreateEntity("Water");
            auto& component = water.AddComponent<WaterComponent>();
            component.m_WorldSizeX = component.m_WorldSizeZ = 8.0f;
            EnableRendering(320, 180);
        }
    };

    TEST_F(GroomSoftIntersectionWaterReplay, ColourReplayRebindsAfterDepthRestoresADifferentEntryVao)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        struct RestoreClock
        {
            ~RestoreClock()
            {
                Time::ClearMockTime();
            }
        } restoreClock;
        Time::SetMockTime(12.0f);
        GLStateGuard guard("GroomWaterReplayTest", GLStateGuard::Policy::Restore);
        EditorCamera camera(45.0f, 320.0f / 180.0f, 0.02f, 100.0f);
        camera.SetViewportSize(320.0f, 180.0f);
        camera.SetPose(glm::vec3(0, 2, 6), 0.0f, 0.2f);
        RunEditorFrames(camera, 2, 0.0f);
        auto graph = RenderGraphDebugRuntime::GetActiveGraph();
        ASSERT_TRUE(graph);
        auto water = graph->GetNode<WaterRenderPass>("WaterPass");
        ASSERT_TRUE(water);
        FramebufferSpecification spec;
        spec.Width = 320;
        spec.Height = 180;
        spec.Attachments = { FramebufferTextureFormat::DEPTH_COMPONENT32F };
        auto depth = Framebuffer::Create(spec);
        ASSERT_TRUE(depth);
        struct EntryVao
        {
            GLuint Id = 0;
            EntryVao()
            {
                glCreateVertexArrays(1, &Id);
            }
            ~EntryVao()
            {
                glBindVertexArray(0);
                glDeleteVertexArrays(1, &Id);
                CommandDispatch::InvalidateBindingCaches();
            }
        } entry;
        bool captured = false;
        bool colourReplayed = false;
        constexpr auto key = "GroomWaterReplay";
        graph->AddPostPassHook(key, [&](std::string_view pass, RenderGraph& active)
                               {
            if (pass == "SceneViewNormalsSnapshotPass")
            {
                ASSERT_TRUE(water->HasSubmittedCommands());
                glBindVertexArray(entry.Id);
                RGCommandContext context;
                context.SetRenderGraph(&active);
                                   context.BeginPass("WaterPass");
                water->CaptureSurfaceDepth(context, depth, 320, 180);
                GLint restored = 0;
                glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &restored);
                EXPECT_EQ(static_cast<GLuint>(restored), entry.Id);
                context.EndPass();
                captured = true;
            }
                               else if (pass == "WaterPass" && captured)
            {
                // The normal graph just executed the production depth/colour
                // pair while its submitted packets were still alive.
                GLint colourVao = 0;
                glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &colourVao);
                EXPECT_NE(static_cast<GLuint>(colourVao), entry.Id);
                EXPECT_NE(colourVao, 0);
                colourReplayed = true;
            } });
        RunEditorFrames(camera, 1, 0.0f);
        graph->RemovePostPassHook(key);
        EXPECT_TRUE(captured);
        EXPECT_TRUE(colourReplayed);
        glFinish();
        EXPECT_EQ(glGetError(), GL_NO_ERROR);
    }

} // namespace OloEngine::Tests

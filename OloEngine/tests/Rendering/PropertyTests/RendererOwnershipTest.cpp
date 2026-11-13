// OLO_TEST_LAYER: integration
//
// The renderer's four owners (ADR 0025, issue #1330): a reset of one owner
// returns that owner to a fresh renderer's state and leaves the others alone.
//
// Each test renders real frames on the Deferred path first, because that is the
// path that fills every owner: object pose history (scene), camera history and
// the temporal-projection observation (view), the sampling sequences (clock)
// and the per-frame hand-offs. It then performs one reset and reads
// Renderer3D::DebugOwnedStateCensus, which is zero in every count on a fresh
// renderer.
//
// What would fail here if the ownership were wrong:
//  * a scene reset that restarted the sampling sequence or dropped the camera
//    history (the old coupling: one flag cleared both);
//  * a view reset that dropped object poses;
//  * a restart that kept the previous run's camera history, pose records,
//    selection ids or a pending pass callback, which is what Shutdown did
//    before the owners were reset by assignment.
//
// Runs in both processes (testing-architecture.md rule 13): the default one and
// `OloEngine-Tests --olo-rhi=vulkan`.

#include "OloEnginePCH.h"
#include "RendererAttachedTest.h"

#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <array>

namespace OloEngine::Tests
{
    class RendererOwnership : public RendererAttachedTest
    {
      protected:
        [[nodiscard]] bool RunsInVulkanProcess() const override
        {
            return true;
        }

        void BuildScene() override
        {
            Entity camera = GetScene().CreateEntity("MainCamera");
            camera.GetComponent<TransformComponent>().Translation = { 0.0f, 1.0f, 6.0f };
            camera.AddComponent<CameraComponent>().Primary = true;

            Entity sun = GetScene().CreateEntity("Sun");
            sun.GetComponent<TransformComponent>().SetRotationEuler({ -0.9f, 0.4f, 0.0f });
            sun.AddComponent<DirectionalLightComponent>();

            m_Cube = GetScene().CreateEntity("Cube");
            auto& mesh = m_Cube.AddComponent<MeshComponent>();
            mesh.m_Primitive = MeshPrimitive::Cube;
            if (Ref<Mesh> cube = MeshPrimitives::CreateCube())
                mesh.m_MeshSource = cube->GetMeshSource();
            m_Cube.AddComponent<MaterialComponent>();

            // Deferred: the only path that records per-entity previous
            // transforms, so the only one where the scene owner is non-empty.
            Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
            Renderer3D::ApplyRendererSettings();

            EnableRendering(256, 256);
        }

        // Frames with the cube moving, so its pose history is a real history.
        // Through the editor camera: that path draws a MeshComponent directly,
        // where the runtime path needs the submesh entities a load creates.
        void RenderMovingFrames(u32 count)
        {
            EditorCamera camera(60.0f, 1.0f, 0.05f, 1000.0f);
            camera.SetViewportSize(256.0f, 256.0f);
            camera.SetPose({ 0.0f, 1.0f, 6.0f }, 0.0f, 0.0f);
            for (u32 i = 0; i < count; ++i)
            {
                m_Cube.GetComponent<TransformComponent>().Translation.x += 0.1f;
                RunEditorFrames(camera, 1);
            }
        }

        // The frame clock's sequence indices. Not FrameSamplingContext's
        // fingerprint: that also hashes the current and previous jitter, which
        // belong to the view and are meant to go with it.
        [[nodiscard]] static std::array<u32, 6> ClockIndices()
        {
            const auto context = Renderer3D::GetFrameSamplingContext();
            return { context.SequenceSeed, context.StochasticFrameIndex, context.TAAJitterFrameIndex,
                     context.TemporalUpscalePhaseIndex, context.CloudFrameIndex, context.FogFrameIndex };
        }

        Entity m_Cube;
    };

    TEST_F(RendererOwnership, SceneResetDropsSceneHistoryAndKeepsTheViewAndTheClock)
    {
        RenderMovingFrames(3);

        const auto before = Renderer3D::DebugOwnedStateCensus();
        const auto clockBefore = ClockIndices();
        ASSERT_GT(before.ScenePoseRecords, 0u) << "the Deferred frames recorded no object pose, so this test would pass on an empty owner";
        ASSERT_GT(before.ViewHistoryFacts, 0u) << "the frames left no camera history to be preserved";
        ASSERT_FALSE(before.ClockAtRest) << "the frames did not advance the sampling sequences";

        Renderer3D::ResetSceneState();

        const auto after = Renderer3D::DebugOwnedStateCensus();
        EXPECT_EQ(after.ScenePoseRecords, 0u) << "a scene reset left object pose records keyed by the old world's entity ids";
        EXPECT_EQ(after.ScenePublications, 0u) << "a scene reset left the old world's environment or cloudscape published";
        EXPECT_EQ(after.ViewHistoryFacts, before.ViewHistoryFacts) << "a scene reset is not a camera reset";
        EXPECT_EQ(ClockIndices(), clockBefore)
            << "replacing visible content must not restart the sampling sequences or the clocks";

        // The first frame of the new world has this frame's poses and no
        // previous ones: half of the steady state's records.
        RenderMovingFrames(1);
        const u32 firstFrame = Renderer3D::DebugOwnedStateCensus().ScenePoseRecords;
        EXPECT_GT(firstFrame, 0u) << "the renderer stopped drawing after a scene reset";
        EXPECT_LT(firstFrame, before.ScenePoseRecords) << "the frame after a scene reset still had previous poses";
    }

    TEST_F(RendererOwnership, ViewResetDropsTheViewAndKeepsTheSceneAndTheClock)
    {
        RenderMovingFrames(3);

        const auto before = Renderer3D::DebugOwnedStateCensus();
        const auto clockBefore = ClockIndices();
        ASSERT_GT(before.ViewHistoryFacts, 0u);
        ASSERT_GT(before.ScenePoseRecords, 0u);

        Renderer3D::ResetFrameCamera();

        const auto after = Renderer3D::DebugOwnedStateCensus();
        EXPECT_EQ(after.ViewHistoryFacts, 0u) << "a view reset left camera history behind";
        EXPECT_EQ(after.ScenePoseRecords, before.ScenePoseRecords)
            << "object poses are measured once per physical frame whatever camera looks at them; a view reset must not drop them";
        EXPECT_EQ(ClockIndices(), clockBefore);

        // The reset is applied when the next frame is prepared, so that frame
        // is where a view reset that also dropped object poses would show: it
        // would hold this frame's poses only, half of the steady state.
        RenderMovingFrames(1);
        const auto next = Renderer3D::DebugOwnedStateCensus();
        EXPECT_EQ(next.ScenePoseRecords, before.ScenePoseRecords)
            << "the frame after a view reset lost the objects' previous poses";
        EXPECT_GT(next.ViewHistoryFacts, 0u);
    }

    TEST_F(RendererOwnership, RestartStartsFromAFreshViewSceneFrameAndClock)
    {
        RenderMovingFrames(3);

        // Hand-offs a frame would normally drain, left pending across the
        // restart: selection ids, and a pass callback that must never run on
        // the renderer that comes back.
        static u32 s_CallbackRuns;
        s_CallbackRuns = 0;
        const std::array<i32, 2> selected{ 1, 2 };
        Renderer3D::SetSelectionOutlineEntityIDs(selected);
        Renderer3D::SetParticleRenderCallback([]
                                              { ++s_CallbackRuns; });

        const auto before = Renderer3D::DebugOwnedStateCensus();
        ASSERT_GT(before.ViewHistoryFacts, 0u);
        ASSERT_GT(before.ScenePoseRecords, 0u);
        ASSERT_GT(before.FrameHandOffs, 0u);
        ASSERT_FALSE(before.ClockAtRest);

        Renderer3D::Shutdown();

        const auto down = Renderer3D::DebugOwnedStateCensus();
        EXPECT_EQ(down.ViewHistoryFacts, 0u) << "Shutdown left camera history for the next renderer";
        EXPECT_EQ(down.ScenePoseRecords, 0u) << "Shutdown left object pose records for the next renderer";
        EXPECT_EQ(down.ScenePublications, 0u);
        EXPECT_EQ(down.FrameHandOffs, 0u) << "Shutdown left a selection, a request or a pending pass callback";
        EXPECT_TRUE(down.ClockAtRest) << "Shutdown left the sampling sequences where the previous run stopped";
        EXPECT_TRUE(Renderer3D::DebugLiveGpuOwningStatics().empty());

        Renderer3D::Init();
        ASSERT_TRUE(Renderer3D::HasInitialized());
        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();
        EnableRendering(256, 256);

        RenderMovingFrames(2);
        EXPECT_EQ(s_CallbackRuns, 0u) << "a pass callback handed to the previous renderer ran on the restarted one";
        EXPECT_GT(Renderer3D::DebugOwnedStateCensus().ScenePoseRecords, 0u) << "the restarted renderer did not draw the scene";
    }
} // namespace OloEngine::Tests

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
#include "RendererStateCheck.h"
#include "TestRenderBackend.h"

#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/AtmosphereSky.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Commands/FrameDataBuffer.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/Debug/GPUPassTimerPool.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/FrontendRecordingContext.h"
#include "OloEngine/Renderer/IBLPrecompute.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/ProceduralSky.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/ShaderLibrary.h"
#include "OloEngine/Renderer/SkyCubemapBake.h"
#include "OloEngine/Renderer/StarNestSky.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Renderer/UniformBlockContract.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "Platform/OpenGL/OpenGLBlockContract.h"

#include <glad/gl.h>

#include <algorithm>
#include <array>
#include <span>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // A test's own render target, unbound however the body leaves: an
        // ASSERT that returns early must not hand the next test a bound
        // framebuffer.
        class BoundTarget
        {
          public:
            explicit BoundTarget(Ref<Framebuffer> target) : m_Target(std::move(target))
            {
                m_Target->Bind();
            }
            ~BoundTarget()
            {
                m_Target->Unbind();
            }
            BoundTarget(const BoundTarget&) = delete;
            BoundTarget& operator=(const BoundTarget&) = delete;

          private:
            Ref<Framebuffer> m_Target;
        };
    } // namespace

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

        // The process-wide facilities Renderer3D::Init brought up. No reset of
        // an owned context (scene, view, recording) may take one down: only the
        // renderer that owns them does, in Shutdown.
        static void ExpectRendererSingletonsAlive(const char* after)
        {
            EXPECT_TRUE(Renderer3D::HasInitialized()) << after;
            EXPECT_TRUE(CommandDispatch::HasUBOReferences()) << after << " dropped the dispatcher's shared buffers";
            EXPECT_TRUE(FrameDataBufferManager::IsInitialized()) << after << " shut the frame data buffer down";
            EXPECT_TRUE(GPUPassTimerPool::GetInstance().IsInitialized()) << after << " shut the GPU pass timer pool down";
            EXPECT_NE(CommandDispatch::GetDispatchFunction(CommandType::DrawMesh), nullptr) << after;
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
        ExpectRendererSingletonsAlive("a scene reset");

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

    // A scene reset assigns the scene owner's state back to its default. Five
    // settings structs in it (water disturbance, wake and foam, cloudscape,
    // underwater fog) are in the cross-test state check. For a scene that
    // publishes none of them the reset must be invisible to that check: the
    // defaults it assigns are the values a non-publishing frame leaves.
    //
    // It was not invisible. The check compared bytes, padding included, and an
    // assignment from a temporary moves padding, so every test that changed
    // scene was reported as leaking settings it never set and the report
    // stopped saying anything about real leaks. The check now compares the
    // bits members own, where the compiler can tell which those are.
    TEST_F(RendererOwnership, ASceneResetMovesNoBitOfASettingNoSceneSet)
    {
        if (!RendererState::ComparesValueBitsOnly())
            GTEST_SKIP() << "this compiler cannot clear padding, so the state check still compares it";
        RenderMovingFrames(1);
        RendererState::Snapshot before;
        ASSERT_TRUE(RendererState::Capture(before));

        Renderer3D::ResetSceneState();

        RendererState::Snapshot after;
        ASSERT_TRUE(RendererState::Capture(after));
        std::string changed;
        EXPECT_EQ(RendererState::Describe(before, after, changed), 0u)
            << "a scene reset of a scene that publishes none of them changed: " << changed;
    }

    // A sequence restart rewinds every pass's own index (#1348), so two paired
    // arms start from the same context. The shadow pass's foliage VSM rotation
    // (#1580) advanced once a frame and had no restart: the arms rotated
    // through different layers, and the folded context could not show it.
    TEST_F(RendererOwnership, ASequenceRestartRewindsEveryPassesOwnIndex)
    {
        ShadowSettings shadow = Renderer3D::GetShadowMap().GetSettings();
        shadow.Enabled = true;
        shadow.VSM.Enabled = true;
        Renderer3D::GetShadowMap().SetSettings(shadow);
        if (!Renderer3D::GetShadowMap().GetVirtualShadowMap().IsEnabledAndInitialized())
            GTEST_SKIP() << "the VSM did not initialise on this device; the rotation only runs on it";

        Renderer3D::ResetFrameSequences(0u);
        const u64 restarted = Renderer3D::GetFrameSamplingContext().PassSequenceState;
        RenderMovingFrames(3);
        ASSERT_NE(Renderer3D::GetFrameSamplingContext().PassSequenceState, restarted)
            << "the frames advanced no pass's own index, so the restart below would pass on nothing";

        Renderer3D::ResetFrameSequences(0u);
        EXPECT_EQ(Renderer3D::GetFrameSamplingContext().PassSequenceState, restarted)
            << "a pass's own index survived the sequence restart";
    }

    TEST_F(RendererOwnership, ViewResetDropsTheViewAndKeepsTheSceneAndTheClock)
    {
        RenderMovingFrames(3);

        const auto before = Renderer3D::DebugOwnedStateCensus();
        const auto clockBefore = ClockIndices();
        ASSERT_GT(before.ViewHistoryFacts, 0u);
        ASSERT_GT(before.ScenePoseRecords, 0u);

        Renderer3D::ResetFrameCamera();
        ExpectRendererSingletonsAlive("a view reset");

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
        // The renderer brought these up, so its Shutdown is what takes them down.
        EXPECT_FALSE(CommandDispatch::HasUBOReferences());
        EXPECT_FALSE(FrameDataBufferManager::IsInitialized());

        Renderer3D::Init();
        ASSERT_TRUE(Renderer3D::HasInitialized());
        Renderer3D::GetRendererSettings().Path = RenderingPath::Deferred;
        Renderer3D::ApplyRendererSettings();
        EnableRendering(256, 256);

        ExpectRendererSingletonsAlive("Init after Shutdown");
        RenderMovingFrames(2);
        EXPECT_EQ(s_CallbackRuns, 0u) << "a pass callback handed to the previous renderer ran on the restarted one";
        EXPECT_GT(Renderer3D::DebugOwnedStateCensus().ScenePoseRecords, 0u) << "the restarted renderer did not draw the scene";
    }

    // Which buffer occupies a uniform binding point has one owner: the
    // backend's occupancy record (issue #1688). The dispatcher used to keep its
    // own answer, a table of what IT last bound at bindings 0 to 7, and skip a
    // bind the table called redundant. A buffer anyone else bound there (a
    // shadow item's camera, a bake, the particle pass) was invisible to that
    // table, so the next draw read the intruder through a bind it skipped, and
    // every such pass had to remember to invalidate the table.
    TEST_F(RendererOwnership, ABindBehindTheDispatchersBackCannotMakeItSkipTheCameraBind)
    {
        RenderMovingFrames(1);
        RunInFrame([]
                   {
            const RendererAPI& api = RenderCommand::GetRendererAPI();
            CommandDispatch::BindSceneResources();
            const RHI::ResourceHandle frameCamera = api.GetUniformBufferOccupant(ShaderBindingLayout::UBO_CAMERA);
            ASSERT_TRUE(frameCamera.IsValid()) << "the backend cannot name the buffer the dispatcher just bound";

            // Same layout, same declaration: nothing but identity tells the two apart.
            const Ref<UniformBuffer> intruder = UniformBuffer::Create(
                ShaderBindingLayout::CameraUBO::GetSize(), ShaderBindingLayout::UBO_CAMERA, "CameraMatrices", "Camera");
            intruder->Bind();
            ASSERT_EQ(api.GetUniformBufferOccupant(ShaderBindingLayout::UBO_CAMERA), intruder->GetRHIHandle())
                << "a direct bind did not reach the backend's occupancy record";

            CommandDispatch::BindSceneResources();
            EXPECT_EQ(api.GetUniformBufferOccupant(ShaderBindingLayout::UBO_CAMERA), frameCamera)
                << "the dispatcher skipped its camera bind although another buffer occupied the binding point"; });
    }

    // The other half of #1688. On OpenGL the occupancy record follows the
    // engine's bind calls, but the binding point is driver state, and a raw
    // glBindBufferBase (some thirty tests, and the FSR2 integration) changes it
    // without the record hearing of it. The dispatcher's own table had the same
    // blind spot and forgot once a frame and at pass entries; the record never
    // forgot, so one raw bind made the dispatcher skip its camera or material
    // bind for the rest of the process. In a whole-suite run that turned a
    // deferred resolve test red that passes alone.
    //
    // The backend now vouches for a point only while it has seen it bound since
    // the dispatcher last dropped its caches.
    TEST_F(RendererOwnership, ARawBindBehindTheRecordIsNotTrustedPastACacheReset)
    {
        if (TestRenderBackend::IsVulkanProcess())
            GTEST_SKIP() << "Vulkan has no driver state behind its occupancy record for a raw call to change";
        RenderMovingFrames(1);
        RunInFrame([]
                   {
            CommandDispatch::BindSceneResources();
            GLint frameCamera = 0;
            ::glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, ShaderBindingLayout::UBO_CAMERA, &frameCamera);
            ASSERT_NE(frameCamera, 0);

            // Behind the record: it still names the frame's camera.
            ::glBindBufferBase(GL_UNIFORM_BUFFER, ShaderBindingLayout::UBO_CAMERA, 0);
            // What every frame start and six pass entries do.
            CommandDispatch::InvalidateBindingCaches();
            CommandDispatch::BindSceneResources();

            GLint bound = 0;
            ::glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, ShaderBindingLayout::UBO_CAMERA, &bound);
            EXPECT_EQ(bound, frameCamera)
                << "the dispatcher skipped its camera bind on the word of a record that a raw bind had made stale"; });
    }

    // The camera binding is frame-published: passes read it without binding it.
    // A bake draws through its own camera buffer there, which has the same
    // layout and so passes every contract check, and a pass that ran after a
    // mid-frame bake read the bake's cube-face camera (issue #1687). A tenant
    // that displaces a frame-published block puts it back.
    TEST_F(RendererOwnership, ASkyBakeLeavesTheFramesCameraBlockBound)
    {
        RenderMovingFrames(1);
        RunInFrame([]
                   {
            const RendererAPI& api = RenderCommand::GetRendererAPI();
            CommandDispatch::BindSceneResources();
            const RHI::ResourceHandle frameCamera = api.GetUniformBufferOccupant(ShaderBindingLayout::UBO_CAMERA);
            ASSERT_TRUE(frameCamera.IsValid());

            PreethamParameters params;
            params.SunDirection = glm::normalize(glm::vec3(0.3f, 0.7f, 0.4f));

            // The six-face bake on its own. ProceduralSky::Generate follows it
            // with the IBL convolution, whose own restore would hide a face
            // bake that left its camera behind (measured: with only the face
            // bake's restore removed, the check at the end still passed).
            ShaderLibrary& library = Renderer3D::GetShaderLibrary();
            ASSERT_TRUE(library.Exists("ProceduralSky"));
            CubemapSpecification faceSpec;
            faceSpec.Width = 32;
            faceSpec.Height = 32;
            faceSpec.Format = ImageFormat::RGBA32F;
            faceSpec.GenerateMips = true;
            Ref<TextureCubemap> faces = TextureCubemap::Create(faceSpec);
            Ref<UniformBuffer> bakeCamera = UniformBuffer::Create(
                ShaderBindingLayout::CameraUBO::GetSize(), ShaderBindingLayout::UBO_CAMERA, "CameraMatrices", "Camera");
            const PreethamCoefficientsUBO coefficients = ProceduralSky::ComputeCoefficients(params);
            Ref<UniformBuffer> skyBlock =
                UniformBuffer::Create(sizeof(PreethamCoefficientsUBO), ShaderBindingLayout::UBO_PROCEDURAL_SKY);
            skyBlock->SetData(&coefficients, sizeof(PreethamCoefficientsUBO));
            ASSERT_TRUE(SkyBake::RenderSkyToCubemap(faces, library.Get("ProceduralSky"), bakeCamera, skyBlock));
            EXPECT_EQ(api.GetUniformBufferOccupant(ShaderBindingLayout::UBO_CAMERA), frameCamera)
                << "the sky face bake left its own camera buffer on the frame-published camera binding";

            // The IBL convolution on its own, for the same reason: the sky's
            // Generate holds a restore of its own around both.
            CommandDispatch::BindSceneResources();
            ASSERT_EQ(api.GetUniformBufferOccupant(ShaderBindingLayout::UBO_CAMERA), frameCamera);
            CubemapSpecification irradianceSpec;
            irradianceSpec.Width = 16;
            irradianceSpec.Height = 16;
            irradianceSpec.Format = ImageFormat::RGBA32F;
            irradianceSpec.GenerateMips = false;
            Ref<TextureCubemap> irradiance = TextureCubemap::Create(irradianceSpec);
            IBLConfiguration convolution;
            convolution.IrradianceSamples = 16;
            IBLPrecompute::GenerateIrradianceMapAdvanced(faces, irradiance, library, convolution);
            EXPECT_EQ(api.GetUniformBufferOccupant(ShaderBindingLayout::UBO_CAMERA), frameCamera)
                << "the IBL convolution left its capture camera on the frame-published camera binding";

            // The whole bake, IBL convolution included.
            CommandDispatch::BindSceneResources();
            ASSERT_EQ(api.GetUniformBufferOccupant(ShaderBindingLayout::UBO_CAMERA), frameCamera);
            const Ref<EnvironmentMap> sky = ProceduralSky::Generate(params, 64);
            ASSERT_TRUE(sky) << "the sky bake produced nothing, so this proves nothing about what it leaves bound";
            EXPECT_EQ(api.GetUniformBufferOccupant(ShaderBindingLayout::UBO_CAMERA), frameCamera)
                << "the bake left a camera buffer of its own on the frame-published camera binding";

            // The other two sky generators create a camera buffer of their own
            // on the same binding and go through the same two bakes.
            CommandDispatch::BindSceneResources();
            ASSERT_EQ(api.GetUniformBufferOccupant(ShaderBindingLayout::UBO_CAMERA), frameCamera);
            const Ref<EnvironmentMap> atmosphere = AtmosphereSky::Generate(AtmosphereSkyParameters{}, 32);
            ASSERT_TRUE(atmosphere) << "the atmosphere bake produced nothing";
            EXPECT_EQ(api.GetUniformBufferOccupant(ShaderBindingLayout::UBO_CAMERA), frameCamera)
                << "the atmosphere sky bake left a camera buffer of its own on the frame-published camera binding";

            CommandDispatch::BindSceneResources();
            ASSERT_EQ(api.GetUniformBufferOccupant(ShaderBindingLayout::UBO_CAMERA), frameCamera);
            const Ref<EnvironmentMap> stars = StarNestSky::Generate(StarNestParameters{}, 32);
            ASSERT_TRUE(stars) << "the star field bake produced nothing";
            EXPECT_EQ(api.GetUniformBufferOccupant(ShaderBindingLayout::UBO_CAMERA), frameCamera)
                << "the star field bake left a camera buffer of its own on the frame-published camera binding"; });
    }

    // A refusal belongs to the program that was refused. The OpenGL backend
    // gives that program a zero block in place of the occupant, and the zero
    // block used to stay on the binding point afterwards: the next program,
    // for which the occupant was the right buffer, read zeros and was told
    // nothing. Vulkan withholds one draw's address and has no binding to leave
    // behind; the same sequence holds there.
    TEST_F(RendererOwnership, ARefusedReadDoesNotTakeTheBindingFromTheNextProgram)
    {
        RenderMovingFrames(1);
        RunInFrame([]
                   {
            const RendererAPI& api = RenderCommand::GetRendererAPI();
            ShaderLibrary& library = Renderer3D::GetShaderLibrary();
            ASSERT_TRUE(library.Exists("IBLPrefilter"));
            // The post passes own their shaders; this is the tone-map pass's.
            const Ref<Shader> toneMap = Shader::Create("assets/shaders/PostProcess_ToneMap.glsl");
            ASSERT_TRUE(toneMap);

            // Two programs, one binding point, two layouts: the tone mapper
            // reads PostProcessUBO at UBO_USER_0, the prefilter IBLParameters.
            Ref<UniformBuffer> prefilterBlock = UniformBuffer::Create(
                ShaderBindingLayout::IBLParametersUBO::GetSize(), ShaderBindingLayout::UBO_USER_0, "IBLParameters");
            const ShaderBindingLayout::IBLParametersUBO parameters{};
            prefilterBlock->SetData(&parameters, ShaderBindingLayout::IBLParametersUBO::GetSize());
            // The tone mapper also declares the auto-exposure state. Nothing
            // here publishes it, and the one refusal this test provokes should
            // be the only thing it puts in the log: Vulkan reports a declared
            // storage binding with no occupant as an error of its own.
            const std::array<f32, 4> manualExposure{};
            Ref<StorageBuffer> exposureState = StorageBuffer::Create(
                static_cast<u32>(sizeof(manualExposure)), ShaderBindingLayout::SSBO_AUTO_EXPOSURE_STATE);
            exposureState->SetData(manualExposure.data(), static_cast<u32>(sizeof(manualExposure)));

            FramebufferSpecification spec;
            spec.Width = 32;
            spec.Height = 32;
            spec.Attachments = { FramebufferTextureFormat::RGBA32F, FramebufferTextureFormat::Depth };
            Ref<Framebuffer> target = Framebuffer::Create(spec);
            ASSERT_TRUE(target);
            Ref<Mesh> cube = MeshPrimitives::CreateSkyboxCube();
            ASSERT_TRUE(cube);
            const BoundTarget bound(target);
            RenderCommand::SetViewport(0, 0, 32, 32);
            CommandDispatch::BindSceneResources();
            prefilterBlock->Bind();
            const auto vao = cube->GetVertexArray();
            vao->Bind();

            const u64 refusedBefore = BindingContract::ViolationCount();
            toneMap->Bind();
            RenderCommand::DrawIndexed(vao);
            ASSERT_EQ(BindingContract::ViolationCount() - refusedBefore, 1u)
                << "the tone mapper read the prefilter's block and was not refused, so nothing below is tested";

            library.Get("IBLPrefilter")->Bind();
            RenderCommand::DrawIndexed(vao);
            EXPECT_EQ(BindingContract::ViolationCount() - refusedBefore, 1u)
                << "the prefilter was refused its own block";
            EXPECT_EQ(api.GetUniformBufferOccupant(ShaderBindingLayout::UBO_USER_0), prefilterBlock->GetRHIHandle())
                << "after another program's refusal the prefilter did not find its own block on the binding point"; });
    }

    // Whose word a block's name is. A GL program linked from SPIR-V carries no
    // names for a driver to report (ARB_gl_spirv): NVIDIA reports the SPIR-V
    // debug names anyway and Mesa reports an empty string. While the check
    // asked the driver, every block read on Mesa was "another layout", every
    // block was replaced by zeros and every frame on the AMD box was black;
    // on NVIDIA nothing showed. The name is what the shader's own SPIR-V
    // declares, on every driver, and this holds it on a driver that would
    // have answered: the declaration is changed under a program whose driver
    // name stays what it was, and the verdict follows the declaration.
    TEST_F(RendererOwnership, ABlocksNameIsWhatTheShadersSpirvDeclaresNotWhatTheDriverReports)
    {
        if (TestRenderBackend::IsVulkanProcess())
            GTEST_SKIP() << "Vulkan takes block names from SPIR-V reflection and has no driver to ask";
        RenderMovingFrames(1);
        RunInFrame([]
                   {
            const Ref<Shader> toneMap = Shader::Create("assets/shaders/PostProcess_ToneMap.glsl");
            ASSERT_TRUE(toneMap);
            const u32 program = toneMap->GetRendererID();

            // What building the program declared for it: the tone mapper's
            // block at UBO_USER_0, by the name its source gives it.
            const std::span<const OpenGLBlockContract::DeclaredBlock> declared = OpenGLBlockContract::DeclaredBlocks(program);
            ASSERT_FALSE(declared.empty()) << "building a program from SPIR-V declared none of its blocks to the binding contract";
            std::vector<OpenGLBlockContract::DeclaredBlock> own(declared.begin(), declared.end());
            const auto postProcess = std::ranges::find_if(
                own, [](const OpenGLBlockContract::DeclaredBlock& block) { return block.Binding == ShaderBindingLayout::UBO_USER_0; });
            ASSERT_NE(postProcess, own.end()) << "the tone mapper's block at UBO_USER_0 was not declared";
            ASSERT_EQ(postProcess->Name.ToView(), "PostProcessUBO");

            constexpr u32 kBlockBytes = 4096;
            const std::array<u8, kBlockBytes> zeros{};
            Ref<UniformBuffer> block = UniformBuffer::Create(kBlockBytes, ShaderBindingLayout::UBO_USER_0, "PostProcessUBO");
            block->SetData(zeros.data(), kBlockBytes);
            // As in the test above: the tone mapper declares the auto-exposure
            // state, and nothing here publishes it.
            const std::array<f32, 4> manualExposure{};
            Ref<StorageBuffer> exposureState = StorageBuffer::Create(
                static_cast<u32>(sizeof(manualExposure)), ShaderBindingLayout::SSBO_AUTO_EXPOSURE_STATE);
            exposureState->SetData(manualExposure.data(), static_cast<u32>(sizeof(manualExposure)));

            FramebufferSpecification spec;
            spec.Width = 32;
            spec.Height = 32;
            spec.Attachments = { FramebufferTextureFormat::RGBA32F, FramebufferTextureFormat::Depth };
            Ref<Framebuffer> target = Framebuffer::Create(spec);
            ASSERT_TRUE(target);
            Ref<Mesh> cube = MeshPrimitives::CreateSkyboxCube();
            ASSERT_TRUE(cube);
            const BoundTarget bound(target);
            RenderCommand::SetViewport(0, 0, 32, 32);
            CommandDispatch::BindSceneResources();
            block->Bind();
            const auto vao = cube->GetVertexArray();
            vao->Bind();

            const u64 refusedBefore = BindingContract::ViolationCount();
            toneMap->Bind();
            RenderCommand::DrawIndexed(vao);
            ASSERT_EQ(BindingContract::ViolationCount() - refusedBefore, 0u) << "the tone mapper was refused its own block";

            // The same program, the same driver, another declared name.
            std::vector<OpenGLBlockContract::DeclaredBlock> renamed = own;
            renamed[static_cast<sizet>(postProcess - own.begin())].Name = FString("IBLParameters");
            OpenGLBlockContract::DeclareProgramBlocks(program, renamed);
            block->Bind();
            RenderCommand::DrawIndexed(vao);
            EXPECT_EQ(BindingContract::ViolationCount() - refusedBefore, 1u)
                << "the verdict did not follow the declared name: the check is asking the driver what the block is called";

            OpenGLBlockContract::DeclareProgramBlocks(program, own);
            block->Bind();
            RenderCommand::DrawIndexed(vao);
            EXPECT_EQ(BindingContract::ViolationCount() - refusedBefore, 1u) << "the block was refused under its own declared name";

            // Two stages that call one binding by two names. Their blocks are
            // the same size, so nothing says which of the program's blocks is
            // which, and the occupant has to be both: it is the first and not
            // the second, and that is one refusal in whichever order the
            // stages were reflected. A check that took the first name it found
            // accepted this read in one order and refused it in the other.
            OpenGLBlockContract::DeclaredBlock second = own[static_cast<sizet>(postProcess - own.begin())];
            second.Name = FString("IBLParameters");
            for (const bool ownNameFirst : { true, false })
            {
                std::vector<OpenGLBlockContract::DeclaredBlock> twoNames = own;
                if (ownNameFirst)
                    twoNames.push_back(second);
                else
                    twoNames.insert(twoNames.begin(), second);
                OpenGLBlockContract::DeclareProgramBlocks(program, twoNames);
                const u64 refusedBeforeTwo = BindingContract::ViolationCount();
                block->Bind();
                RenderCommand::DrawIndexed(vao);
                EXPECT_EQ(BindingContract::ViolationCount() - refusedBeforeTwo, 1u)
                    << "two names at one binding, the occupant's own name " << (ownNameFirst ? "first" : "second")
                    << ": the occupant was not held to both";
            }

            // Declaring a program's own declaration again is a copy, not a
            // read of freed storage: the span is into what the call replaces.
            OpenGLBlockContract::DeclareProgramBlocks(program, own);
            OpenGLBlockContract::DeclareProgramBlocks(program, OpenGLBlockContract::DeclaredBlocks(program));
            const std::span<const OpenGLBlockContract::DeclaredBlock> again = OpenGLBlockContract::DeclaredBlocks(program);
            ASSERT_EQ(again.size(), own.size());
            for (sizet i = 0; i < own.size(); ++i)
                EXPECT_EQ(again[i].Name.ToView(), own[i].Name.ToView());
            const u64 refusedBeforeOwn = BindingContract::ViolationCount();
            block->Bind();
            RenderCommand::DrawIndexed(vao);
            EXPECT_EQ(BindingContract::ViolationCount() - refusedBeforeOwn, 0u) << "the block was refused under its own declared name"; });
    }

    // The alternate-camera seam with a real draw, on both backends. A replay
    // through another camera (the planar reflection, a bucket's view override)
    // hands the frame back as it found it: the main view, its camera block on
    // the frame-published binding, the view- and scene-owned history, and every
    // process-wide facility. Neither it nor a recording item's teardown owns
    // any of them.
    TEST_F(RendererOwnership, AnAlternateViewReplayAndARecordingItemHandTheFrameBackWhole)
    {
        RenderMovingFrames(2);
        RunInFrame([]
                   {
            const RendererAPI& api = RenderCommand::GetRendererAPI();
            ShaderLibrary& library = Renderer3D::GetShaderLibrary();
            ASSERT_TRUE(library.Exists("IBLPrefilter"));
            CommandDispatch::BindSceneResources();
            const RHI::ResourceHandle frameCamera = api.GetUniformBufferOccupant(ShaderBindingLayout::UBO_CAMERA);
            ASSERT_TRUE(frameCamera.IsValid());
            const DispatchView main = CommandDispatch::GetView();
            ASSERT_TRUE(main.MainViewLanes);
            const auto before = Renderer3D::DebugOwnedStateCensus();
            ASSERT_GT(before.ViewHistoryFacts, 0u);
            ASSERT_GT(before.ScenePoseRecords, 0u);

            // The main camera mirrored about y = 0, as the planar reflection builds it.
            DispatchView mirror = main;
            mirror.ViewMatrix = main.ViewMatrix * glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, -1.0f, 1.0f));
            mirror.ViewProjectionMatrix = mirror.ProjectionMatrix * mirror.ViewMatrix;
            mirror.PrevViewProjectionMatrix = mirror.ViewProjectionMatrix;
            mirror.ViewPosition = glm::vec3(main.ViewPosition.x, -main.ViewPosition.y, main.ViewPosition.z);
            mirror.MainViewLanes = false;

            Ref<UniformBuffer> prefilterBlock = UniformBuffer::Create(
                ShaderBindingLayout::IBLParametersUBO::GetSize(), ShaderBindingLayout::UBO_USER_0, "IBLParameters");
            const ShaderBindingLayout::IBLParametersUBO parameters{};
            prefilterBlock->SetData(&parameters, ShaderBindingLayout::IBLParametersUBO::GetSize());
            FramebufferSpecification spec;
            spec.Width = 32;
            spec.Height = 32;
            spec.Attachments = { FramebufferTextureFormat::RGBA32F, FramebufferTextureFormat::Depth };
            Ref<Framebuffer> target = Framebuffer::Create(spec);
            ASSERT_TRUE(target);
            Ref<Mesh> cube = MeshPrimitives::CreateSkyboxCube();
            ASSERT_TRUE(cube);
            const auto vao = cube->GetVertexArray();

            const auto expectFrameHandedBack = [&](const char* after)
            {
                const DispatchView& now = CommandDispatch::GetView();
                EXPECT_TRUE(Math::BitwiseEqual(now.ViewMatrix, main.ViewMatrix)) << after;
                EXPECT_TRUE(Math::BitwiseEqual(now.ViewProjectionMatrix, main.ViewProjectionMatrix)) << after;
                EXPECT_TRUE(Math::BitwiseEqual(now.PrevViewProjectionMatrix, main.PrevViewProjectionMatrix))
                    << after << " left another camera's history in the main view";
                EXPECT_TRUE(Math::BitwiseEqual(now.ViewPosition, main.ViewPosition)) << after;
                EXPECT_TRUE(now.MainViewLanes) << after << " left the main view's screen-space inputs switched off";
                EXPECT_EQ(api.GetUniformBufferOccupant(ShaderBindingLayout::UBO_CAMERA), frameCamera)
                    << after << " left another buffer on the frame-published camera binding";
                const auto census = Renderer3D::DebugOwnedStateCensus();
                EXPECT_EQ(census.ViewHistoryFacts, before.ViewHistoryFacts) << after << " changed the view's history";
                EXPECT_EQ(census.ScenePoseRecords, before.ScenePoseRecords) << after << " changed the scene's pose records";
                ExpectRendererSingletonsAlive(after);
            };

            {
                const CommandDispatch::ScopedAlternateView mirrorView(mirror);
                EXPECT_FALSE(CommandDispatch::GetView().MainViewLanes);
                target->Bind();
                RenderCommand::SetViewport(0, 0, 32, 32);
                CommandDispatch::BindSceneResources();
                prefilterBlock->Bind();
                vao->Bind();
                library.Get("IBLPrefilter")->Bind();
                RenderCommand::DrawIndexed(vao);
                target->Unbind();
            }
            expectFrameHandedBack("an alternate-view replay");

            {
                // Entered and left on the render thread, unprepared and with
                // nothing recorded. Preparing an item creates its upload
                // buffers, and a buffer claims its binding point when it is
                // created: the backend prepares its items with those claims
                // suppressed and gives each a binding state of its own
                // (VulkanRendererAPI::RecordParallel), neither of which this
                // test has. What is under test is that the dispatcher's half,
                // selecting an item and dropping it, is invisible to the frame.
                FrontendRecordingContext item;
                const ScopedFrontendRecordingContext recording(item);
            }
            expectFrameHandedBack("a recording item's teardown"); });
    }

    // The rule that stands in for "nobody shuts down what it does not own"
    // where a device-level teardown has to (ADR 0025 section 5): a Vulkan
    // device test shuts the dispatcher and the GPU timer pool down, because
    // their GPU objects die with its device, and the renderer that owns them
    // re-arms both on its own backend when it next begins a frame.
    TEST_F(RendererOwnership, WhatADeviceTeardownShutDownTheRendererReArmsAtItsNextFrame)
    {
        RenderMovingFrames(2);

        CommandDispatch::Shutdown();
        GPUPassTimerPool::GetInstance().Shutdown();
        ASSERT_FALSE(CommandDispatch::HasUBOReferences());
        ASSERT_FALSE(GPUPassTimerPool::GetInstance().IsInitialized());

        RenderMovingFrames(2);
        ExpectRendererSingletonsAlive("the frame after a device teardown");
        RunInFrame([]
                   {
            CommandDispatch::BindSceneResources();
            EXPECT_TRUE(RenderCommand::GetRendererAPI().GetUniformBufferOccupant(ShaderBindingLayout::UBO_CAMERA).IsValid())
                << "the re-armed dispatcher bound no camera block"; });
        EXPECT_GT(Renderer3D::DebugOwnedStateCensus().ScenePoseRecords, 0u)
            << "the frame after a device teardown did not draw the scene";
    }
} // namespace OloEngine::Tests

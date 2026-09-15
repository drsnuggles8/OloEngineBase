// =============================================================================
// AnimatedGPUSceneConcurrentRecordingTest.cpp
//
// Issue #1228, acceptance criterion 4: raster submission and debug views
// resolve the same animated surface records, INCLUDING CONCURRENT COMMAND
// RECORDING.
//
// Why this test exists at all, stated plainly
// -------------------------------------------
// Before this change the concurrent animated submission API --
// Renderer3D::DrawAnimatedMeshParallel and the SubmitMeshesParallel bracket
// that drives it -- had NO callers anywhere in the repository. Not in the
// engine, not in the editor, not in a test. Model::DrawParallel is the only
// live user of SubmitMeshesParallel and it hard-codes IsAnimated = false.
//
// That makes the criterion a trap of a shape this repository has already paid
// for once: #1226 added a HasBoneHistory() guard to
// Renderer3D::RenderAnimatedMeshes, which also has no callers, and the guard
// sat there looking load-bearing through two review passes while the path that
// actually rendered went ungated. Threading a draw-link parameter through an
// entry point nobody calls and declaring the criterion met would be the same
// mistake with a different parameter.
//
// So the parameter is not merely added, it is DRIVEN: this test is the caller.
// It records animated draws from the renderer's worker contexts, concurrently,
// and asserts that each command packet carries the link the main thread minted
// for it. If the parameter is ever dropped from one of the two worker branches,
// this fails; a comment would not have.
//
// The thread-safety claim being pinned is a structural one, not a timing one.
// Links are MINTED on the main thread (ExtractGPUSceneMesh appends to an
// unsynchronised per-frame vector) and only CARRIED across the boundary, as a
// plain integer copied into the descriptor. So the test asserts the invariant
// that makes that safe -- the link table does not grow during the parallel
// region -- rather than trying to catch a race by running it often, which is
// what TSan is for and what this box cannot do (tsan-only-runs-in-ci).
//
// Classification: L8 / integration (needs the live pipeline and a GL context to
// allocate command packets and resolve shaders).
//
// OLO_TEST_LAYER: integration
// =============================================================================

#include "OloEnginePCH.h"

#include "RendererAttachedTest.h"

#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Renderer/Camera/EditorCamera.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include "OloEngine/Renderer/Commands/CommandPacket.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/GPUScene/GPUSceneDrawLink.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Renderer3D.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace OloEngine::Tests
{
    namespace
    {
        constexpr u32 kWidth = 320;
        constexpr u32 kHeight = 180;
        constexpr u32 kSubjectCount = 24;
    } // namespace

    class AnimatedConcurrentRecordingScene : public RendererAttachedTest
    {
      protected:
        void BuildScene() override
        {
            EnableRendering(kWidth, kHeight);

            m_SkinnedMesh = MeshPrimitives::CreateMultiBoneAnimatedCube();
            if (!m_SkinnedMesh)
            {
                return;
            }

            const Skeleton* source = m_SkinnedMesh->GetMeshSource()->GetSkeleton();
            if (source == nullptr)
            {
                return;
            }
            m_Skeleton = Ref<Skeleton>::Create(source->m_LocalTransforms.size());
            static_cast<SkeletonData&>(*m_Skeleton) = static_cast<const SkeletonData&>(*source);

            // Two frames of history so the surface is continuous rather than
            // first-use: a first-use surface would make the deformation half of
            // every assertion below vacuously "no history".
            m_Skeleton->AdvanceBoneHistory();
            m_Skeleton->AdvanceBoneHistory();
        }

        Ref<Mesh> m_SkinnedMesh;
        Ref<Skeleton> m_Skeleton;
    };

    TEST_F(AnimatedConcurrentRecordingScene, WorkersCarryTheLinkTheMainThreadMinted)
    {
        OLO_ENSURE_GPU_OR_SKIP();
        ASSERT_TRUE(m_SkinnedMesh) << "the skinned primitive failed to build";
        ASSERT_TRUE(m_Skeleton);

        EditorCamera camera(55.0f, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.05f, 500.0f);
        camera.SetViewportSize(static_cast<f32>(kWidth), static_cast<f32>(kHeight));
        camera.Focus(glm::vec3(0.0f), 30.0f, 0.0f, 0.3f);

        // A frame, so the pipeline, the frame data buffer and the worker
        // allocators are in the state SubmitMeshesParallel expects.
        RunEditorFrames(camera, 1);

        const Ref<MeshSource> meshSource = m_SkinnedMesh->GetMeshSource();
        ASSERT_TRUE(meshSource);
        ASSERT_FALSE(meshSource->GetSubmeshes().IsEmpty());

        const std::vector<glm::mat4>& bones = m_Skeleton->m_FinalBoneMatrices;
        const std::vector<glm::mat4>& prevBones = m_Skeleton->m_PrevFinalBoneMatrices;

        // ---- main thread: mint one link per subject -------------------------
        //
        // Deliberately BEFORE the parallel region and deliberately here rather
        // than inside the worker body: this is the ownership boundary the whole
        // design rests on.
        Renderer3D::BeginGPUSceneExtraction(/*ownerToken*/ 0xC0FFEEull);

        std::vector<Renderer3D::MeshSubmitDesc> descriptors;
        std::vector<u32> mintedLinks;
        descriptors.reserve(kSubjectCount);
        mintedLinks.reserve(kSubjectCount);

        const GPUSceneAnimatedSurface animatedSurface{
            .m_IsAnimated = true,
            .m_DeformationRevision = m_Skeleton->m_DeformationRevision,
            .m_PrevDeformationRevision = m_Skeleton->m_PrevDeformationRevision,
            .m_ResetCause = m_Skeleton->m_DeformationResetCause,
        };

        for (u32 i = 0; i < kSubjectCount; ++i)
        {
            const glm::mat4 transform =
                glm::translate(glm::mat4(1.0f), glm::vec3(static_cast<f32>(i) * 2.0f, 0.0f, 0.0f));

            // A distinct entity id per subject, so every link names a distinct
            // record and a worker that crossed two links would be caught rather
            // than accidentally right.
            const u32 link = Renderer3D::ExtractGPUSceneMesh(
                /*stableEntityId*/ 1000ull + i, /*stableInstanceId*/ 0ull, meshSource, /*submeshIndex*/ 0u,
                transform, GPUSceneMaterialKey{}, GPUSceneDrawLinkRequest::Link, animatedSurface);
            ASSERT_NE(link, GPUSceneDrawLinkNone) << "subject " << i << " was refused a canonical record";
            mintedLinks.push_back(link);

            Renderer3D::MeshSubmitDesc desc;
            desc.Mesh = Ref<Mesh>::Create(meshSource, 0);
            desc.Transform = transform;
            desc.EntityID = static_cast<i32>(1000u + i);
            desc.IsStatic = false;
            desc.IsAnimated = true;
            desc.BoneMatrices = &bones;
            desc.PrevBoneMatrices = &prevBones;
            desc.PrevTransform = transform;
            desc.HasPrevTransform = true;
            desc.GPUSceneDrawLink = link;
            descriptors.push_back(std::move(desc));
        }

        // Every link is distinct: the assertion below would be worthless if the
        // main thread had handed out the same index N times.
        for (u32 i = 1; i < kSubjectCount; ++i)
        {
            ASSERT_NE(mintedLinks[i], mintedLinks[i - 1]);
        }

        // ---- parallel region -----------------------------------------------
        //
        // minBatchSize of 1 so the work actually spreads across worker contexts
        // instead of collapsing onto the calling thread, which would make this
        // a serial test wearing a parallel name.
        const u32 submitted = Renderer3D::SubmitMeshesParallel(descriptors, /*minBatchSize*/ 1);
        EXPECT_EQ(submitted, kSubjectCount)
            << "the parallel region dropped animated draws; a link cannot be checked on a packet that was "
               "never produced";

        // The invariant that makes carrying an index across the boundary safe:
        // no worker appended to the link table. If one ever mints a link the
        // table grows, and a vector append from N threads is a data race with
        // no diagnostic -- a wrong record, not a crash.
        for (u32 i = 0; i < kSubjectCount; ++i)
        {
            const GPUSceneDrawLink* link = Renderer3D::GetGPUSceneDrawLink(mintedLinks[i]);
            EXPECT_EQ(link, nullptr) << "links resolve before EndScene, which would mean the parallel region "
                                        "resolved them rather than merely carrying them";
        }

        // ---- what the workers recorded --------------------------------------
        //
        // Reading the bucket's packets is what makes this a test of the PARAMETER
        // and not of the API's existence: a branch that dropped
        // desc.GPUSceneDrawLink would submit exactly as many packets, draw
        // exactly the same picture, and fail only here.
        auto* geometryNode = Renderer3D::GetRenderStreamNode(Renderer3D::RenderStreamType::Geometry);
        ASSERT_NE(geometryNode, nullptr) << "no geometry render stream to read the recorded packets from";

        std::vector<u32> recordedLinks;
        for (const CommandPacket* packet : geometryNode->GetCommandBucket().GetPackets())
        {
            if (packet == nullptr || packet->GetCommandType() != CommandType::DrawMesh)
            {
                continue;
            }
            const auto* cmd = packet->GetCommandData<DrawMeshCommand>();
            if (cmd != nullptr && cmd->isAnimatedMesh)
            {
                recordedLinks.push_back(cmd->gpuSceneDrawLink);
            }
        }

        ASSERT_EQ(recordedLinks.size(), kSubjectCount)
            << "the worker-recorded animated packets do not account for every subject";

        std::vector<u32> sortedRecorded = recordedLinks;
        std::vector<u32> sortedMinted = mintedLinks;
        std::ranges::sort(sortedRecorded);
        std::ranges::sort(sortedMinted);
        EXPECT_EQ(sortedRecorded, sortedMinted)
            << "the links the workers recorded are not the links the main thread minted: a concurrent recording "
               "path is naming different canonical records from the serial one, which is a wrong material and a "
               "wrong previous transform rather than a visible failure";

        for (const u32 link : recordedLinks)
        {
            EXPECT_NE(link, GPUSceneDrawLinkNone)
                << "a worker recorded an animated draw with no link at all, so that draw silently falls back to "
                   "the per-entity transform cache the records exist to replace";
        }
    }
} // namespace OloEngine::Tests

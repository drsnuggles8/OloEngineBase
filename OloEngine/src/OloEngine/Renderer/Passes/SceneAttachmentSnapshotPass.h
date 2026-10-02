#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraphNode.h"

namespace OloEngine
{
    class SceneRenderPass;

    // @brief An explicit copy of one SceneColor attachment, for the passes that
    // must sample it while they draw into SceneColor (issue #1332).
    //
    // Since #1332 SceneDepth, SceneNormals and Velocity are attachment views of
    // SceneColor on the forward paths: a consumer samples the attachment
    // itself, so it always sees the newest write the graph ordered before it,
    // and no pass copies anything to keep an export current. A view cannot
    // serve a pass that samples the attachment WHILE that attachment is bound
    // to the framebuffer it draws into: that is a rendering feedback loop,
    // undefined in GL and a layout conflict in Vulkan. Two passes do exactly
    // that, and this node is their copy, made once, at a fixed point, under a
    // name of its own:
    //
    //   Depth        -> SceneDepthSnapshot, for DecalRenderPass (projection)
    //                   and WaterRenderPass (refraction floor, soft edges).
    //                   Registered after the last depth-writing geometry
    //                   (groom) and before the decals. Deferred needs no copy:
    //                   SceneDepth is the G-Buffer's depth there, which is not
    //                   the framebuffer either pass draws into, so the snapshot
    //                   handle IS SceneDepth and this node declares nothing.
    //   ViewNormals  -> SceneViewNormalsSnapshot, for WaterRenderPass's
    //                   screen-space reflection march. Registered right before
    //                   the water. Water.glsl writes view normals to the same
    //                   attachment it marched over, so this copy is needed on
    //                   every path.
    //
    // A node with no reader is culled, so a frame without decals or water pays
    // for no copy.
    class SceneAttachmentSnapshotPass : public RenderGraphNode
    {
      public:
        enum class Attachment : u8
        {
            Depth,
            ViewNormals,
        };

        SceneAttachmentSnapshotPass(SceneRenderPass* scene, Attachment attachment);
        ~SceneAttachmentSnapshotPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Execute(RGCommandContext& context) override;

      private:
        SceneRenderPass* m_Scene = nullptr;
        Attachment m_Attachment = Attachment::Depth;
        RGTextureHandle m_Source{};
        RGTextureHandle m_Snapshot{};
    };
} // namespace OloEngine

#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/PostProcessSettings.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/SphereProxyAO.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <glm/glm.hpp>

#include <span>
#include <vector>

namespace OloEngine
{
    // @brief Analytic sphere-proxy ambient occlusion (issue #710).
    //
    // The complement to the screen-space AO producers. GTAO and SSAO can only
    // occlude with geometry that is in the depth buffer, so a large object just
    // off the edge of the frame — or behind the camera — contributes nothing and
    // its contact darkening pops away as the camera pans. This pass evaluates a
    // closed-form sphere-occlusion integral over coarse proxy spheres against
    // the RECEIVER's hemisphere, which has no camera term in it at all, and
    // multiplies the result into the AO buffer the producer just wrote:
    //
    //     AOBuffer *= mix(1, product of (1 - occlusion_i), Strength)
    //
    // Multiplicative and in place, deliberately. GTAO keeps the fine contact
    // detail it is good at; this supplies the large-scale term it structurally
    // cannot see; and because the combine happens IN AOBuffer, every existing
    // consumer of that resource (AOApplyPass and DeferredLightingPass both) picks
    // the combined value up with no change of its own. When the pass is disabled
    // it is not registered, so AOBuffer holds exactly what the producer wrote —
    // the neutral state is the absence of the pass, not a branch inside it.
    //
    // Bound to the GTAO technique. SSAO's AOBuffer is a half-resolution RG16F
    // target while GTAO's is a full-resolution R8UNorm one, and a storage image's
    // format is part of its binding contract, so covering both would mean a
    // second shader variant for a technique the issue does not name. Under SSAO
    // this pass declines and says so once.
    //
    // Proxies come from the world AABBs the frame's DDGI caster funnel already
    // builds (SetProxySourceBounds, fed in
    // RenderPipeline::ConfigurePassesForFrame from Renderer3DData::AOProxyBounds).
    // Two properties of that list decide the choice, and both are load-bearing:
    // it is not view-frustum culled, so an occluder leaving the frame keeps its
    // proxy — which is the entire feature — and it is not gated on shadow
    // casting, so an artist turning a shadow off does not silently turn the
    // occlusion off with it. The shadow-caster list has only the first of those.
    //
    // It inherits that funnel's own filters: static opaque geometry only. A
    // skinned character contributes no proxy, and a transparent one contributes
    // none either, which is the right answer in both cases for a coarse
    // volumetric occluder.
    class SphereProxyAORenderPass : public RenderGraphNode
    {
      public:
        SphereProxyAORenderPass();
        ~SphereProxyAORenderPass() override = default;

        void Setup(RGBuilder& builder, FrameBlackboard& blackboard) override;
        void Init(const FramebufferSpecification& spec) override;
        void Execute(RGCommandContext& context) override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;

        void SetSettings(const PostProcessSettings& settings)
        {
            m_Settings = settings;
        }

        void SetEnabled(bool enabled) noexcept
        {
            m_Enabled = enabled;
        }
        [[nodiscard]] bool IsEnabled() const noexcept override
        {
            return m_Enabled;
        }

        void SetProjectionMatrix(const glm::mat4& projection)
        {
            m_Projection = projection;
        }

        // World-space view matrix and the frame's render origin. Kept apart
        // because the proxy centres arrive in ABSOLUTE world coordinates, which
        // can be kilometres out: transforming those with the world view matrix
        // in f32 is the catastrophic cancellation camera-relative rendering
        // exists to avoid, so the pass shifts both sides by the origin first.
        void SetViewMatrix(const glm::mat4& view, const glm::vec3& renderOrigin)
        {
            m_ViewMatrix = view;
            m_RenderOrigin = renderOrigin;
        }

        // World-space view position, used only to rank proxies when there are
        // more candidates than the UBO can carry.
        void SetViewPosition(const glm::vec3& viewPosition)
        {
            m_ViewPosition = viewPosition;
        }

        // The frame's candidate occluder bounds, in absolute world space. Copied,
        // not referenced: the source is a per-frame buffer that is cleared and
        // refilled at the next BeginScene, long before this pass executes.
        void SetProxySourceBounds(std::span<const BoundingBox> bounds);

        [[nodiscard]] bool IsReadyForExecution() const noexcept override
        {
            return m_Shader && m_Shader->IsValid() && m_Width > 0u && m_Height > 0u;
        }

        // Proxies the last Execute actually uploaded. Reports what happened, not
        // what the settings asked for — the two differ whenever the source bounds
        // were empty or every candidate was filtered out.
        [[nodiscard]] u32 GetLastUploadedProxyCount() const noexcept
        {
            return m_LastUploadedProxyCount;
        }

      private:
        void UploadUniforms(u32 proxyCount);

        bool m_Enabled = false;

        Ref<ComputeShader> m_Shader;
        Ref<UniformBuffer> m_UBO;
        UBOStructures::SphereProxyAOUBO m_GPUData{};

        PostProcessSettings m_Settings;
        glm::mat4 m_Projection{ 1.0f };
        glm::mat4 m_ViewMatrix{ 1.0f };
        glm::vec3 m_RenderOrigin{ 0.0f };
        glm::vec3 m_ViewPosition{ 0.0f };

        std::vector<BoundingBox> m_SourceBounds;
        std::vector<SphereProxyAO::Proxy> m_Proxies;

        RGTextureHandle m_SelectedSceneDepthTexture{};
        RGTextureHandle m_SelectedSceneNormalsTexture{};
        // True when the bound normals are already view space (the forward path).
        // Same contract as GTAORenderPass's flag of the same name: it decides
        // whether the shader's world-to-view rotation runs or is replaced by
        // identity, and getting it wrong rotates every normal out of the
        // hemisphere the integral assumes.
        bool m_SceneNormalsAreViewSpace = false;
        RGTextureHandle m_SelectedAOTexture{};

        u32 m_Width = 0;
        u32 m_Height = 0;
        u32 m_LastUploadedProxyCount = 0;
    };
} // namespace OloEngine

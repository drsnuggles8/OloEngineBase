#pragma once

#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ReSTIR/ReSTIRPTTechnique.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include <glm/glm.hpp>
#include <array>

namespace OloEngine
{
    class GPUScene;
    class EmissiveTriangleTable;
    class MaterialTextureTable;
    namespace RayTracing
    {
        class RayTracingScene;
    }

    // Four ordered fullscreen stages, with device-address suffix pools. Only
    // initial pools survive across frames; mixed reservoirs never feed history.
    class ReSTIRPTPass : public RenderGraphNode
    {
      public:
        ReSTIRPTPass();
        void Init(const FramebufferSpecification& spec) override;
        void Setup(RGBuilder& builder, FrameBlackboard& board) override;
        void Execute(RGCommandContext& context) override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;
        void SetEnabled(bool enabled) noexcept
        {
            m_Enabled = enabled;
        }
        [[nodiscard("Use the PT request state")]] bool IsEnabled() const noexcept override
        {
            return m_Enabled;
        }
        [[nodiscard("Check PT shader readiness before graph declaration")]] bool IsReadyForExecution() const noexcept override;
        void SetSettings(const ReSTIRPTSettings& settings) noexcept;
        [[nodiscard("Use the sanitized PT settings")]] const ReSTIRPTSettings& GetSettings() const noexcept
        {
            return m_Settings;
        }
        void SetRayTracingScene(const RayTracing::RayTracingScene* scene) noexcept
        {
            m_RayTracingScene = scene;
        }
        void SetGPUScene(const GPUScene* scene) noexcept
        {
            m_GPUScene = scene;
        }
        void SetEmissiveTable(const EmissiveTriangleTable* table) noexcept
        {
            m_EmissiveTable = table;
        }
        void SetMaterialTextureTable(const MaterialTextureTable* table) noexcept
        {
            m_MaterialTextures = table;
        }
        void SetEnvironment(const glm::vec3& rgb, f32 intensity, bool cubeBound) noexcept;
        void SetCameraMatrices(const glm::mat4& view, const glm::mat4& projection, const glm::vec3& origin) noexcept;
        void SetFrameIndex(u32 frame) noexcept
        {
            m_FrameIndex = frame;
        }
        void SetSceneEpoch(u64 epoch) noexcept;
        void ResolveAvailabilityForFrame(bool deferredPathActive = true, bool participatingMedia = false);
        [[nodiscard("Use PT engagement and measured diagnostics")]] const ReSTIRPTStats& GetStats() const noexcept
        {
            return m_Stats;
        }

      private:
        bool EnsureBuffers();
        void StandDown(std::string_view reason);
        bool m_Enabled = false;
        bool m_DeferredPathActive = false;
        bool m_ParticipatingMedia = false;
        bool m_HaveHistory = false;
        bool m_HaveCounters = false;
        u32 m_FrameIndex = 0;
        u32 m_LastFrame = 0;
        u32 m_CounterFrame = 0;
        u32 m_Width = 0;
        u32 m_Height = 0;
        u64 m_SceneEpoch = 0;
        u64 m_LastEpoch = 0;
        u64 m_ShaderReloadRevision = 0;
        ReSTIRPTSettings m_Settings{};
        ReSTIRPTStats m_Stats{};
        std::string_view m_LastFallback = "disabled";
        Ref<Shader> m_Shader;
        Ref<UniformBuffer> m_Parameters;
        std::array<Ref<StorageBuffer>, 4> m_Pools{};
        Ref<StorageBuffer> m_Counters;
        const RayTracing::RayTracingScene* m_RayTracingScene = nullptr;
        const GPUScene* m_GPUScene = nullptr;
        const EmissiveTriangleTable* m_EmissiveTable = nullptr;
        const MaterialTextureTable* m_MaterialTextures = nullptr;
        glm::mat4 m_View{ 1.0f };
        glm::mat4 m_Projection{ 1.0f };
        glm::vec3 m_Origin{ 0.0f };
        glm::vec3 m_Environment{ 0.0f };
        f32 m_EnvironmentIntensity = 0.0f;
        bool m_CubeBound = false;
        std::array<RGTextureHandle, 6> m_Inputs{}; // depth, albedo, normal, emissive, velocity, cube
        std::array<RGFramebufferHandle, 3> m_Targets{};
    };
} // namespace OloEngine

#pragma once

#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/ReSTIR/ReSTIRPTTechnique.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/Debug/RendererMemoryReport.h"
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

        // Setup() declares nothing while the tier is stood down. It can also
        // stand the tier down ITSELF, after the configuration captured Active
        // as true, and Execute() may resolve Active back to true before the next
        // capture; so the stand-down is recorded separately and keyed too, or
        // the next frame would reproduce the key of a build that promised a tier
        // Setup() withdrew.
        void AppendDeclarationInputs(RGDeclarationKey& key) const override
        {
            key.Add(m_Stats.Active);
            key.Add(m_StoodDownInSetup);
        }
        void Execute(RGCommandContext& context) override;
        void SetupFramebuffer(u32 width, u32 height) override;
        void ResizeFramebuffer(u32 width, u32 height) override;
        void OnReset() override;
        // ReSTIR PT keeps its own ping-pong pools outside the registry, so a
        // sequence restart drops their lineage here (#1348).
        void ResetFrameSequence([[maybe_unused]] u32 sequenceSeed) override
        {
            m_HaveHistory = false;
            m_LastFrame = 0;
        }
        [[nodiscard]] u64 GetFrameSequenceState() const override
        {
            return (static_cast<u64>(m_HaveHistory) << 32u) | m_LastFrame;
        }
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
        // Whether the registry lineage of the path records continued from the
        // previous frame (#1348): false after any invalidation that reaches
        // the reprojecting histories, a camera cut first among them.
        void SetLineageContinues(bool continues) noexcept
        {
            m_LineageContinues = continues;
        }
        // Whether this frame's Execute wrote the path records next frame
        // reuses (the end of its external lineage, #1348). The frame index
        // advances every frame, so a frame that never ran leaves it behind.
        [[nodiscard]] bool ProducedRecordsThisFrame() const noexcept
        {
            return m_HaveHistory && m_LastFrame == m_FrameIndex;
        }
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
        bool m_LineageContinues = true;
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
        bool m_StoodDownInSetup = false;
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

        // LAST member: unregistered before the pools it reads are destroyed (#1342).
        RendererMemoryReporterHandle m_MemoryReporter;
    };
} // namespace OloEngine

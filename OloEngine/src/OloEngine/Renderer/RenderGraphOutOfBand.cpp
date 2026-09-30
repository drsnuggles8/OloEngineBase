#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RenderGraphOutOfBand.h"

#include "OloEngine/Core/DebugLevers.h"

#include <array>
#include <atomic>

namespace OloEngine
{
    namespace
    {
        using namespace RGOutOfBandBoundaries;

        constexpr std::array kProductionBoundaries{
            RGOutOfBandBoundary{
                .Name = SceneTLAS,
                .Kind = RGOutOfBandKind::GpuResource,
                .Owner = "RayTracingScene (RayTracing/RayTracingScene.cpp), built by RayTracingScenePass",
                .Reason = "An acceleration structure is read by device address; the graph has no acceleration-structure "
                          "resource, and RayTracingScenePass records its own build-to-read barrier.",
            },
            RGOutOfBandBoundary{
                .Name = DeformedVertices,
                .Kind = RGOutOfBandKind::GpuResource,
                .Prologue = RGFramePhaseUse::Write,
                .Owner = "DeformedSurfaceCache (RayTracing/DeformedSurfaceCache.cpp), dispatched by SkeletalDeformPass",
                .Reason = "Per-surface buffers read by device address from the BLAS build and the RT hit shaders. The "
                          "prologue (EndScene -> DeformedSurfaceCache::EndFrame) uploads the bone palettes the dispatch "
                          "reads; SkeletalDeformPass records the deform-to-build barrier.",
            },
            RGOutOfBandBoundary{
                .Name = OcclusionHZB,
                .Kind = RGOutOfBandKind::GpuResource,
                .Prologue = RGFramePhaseUse::Read,
                .Epilogue = RGFramePhaseUse::Write,
                .Owner = "Renderer3D OcclusionHZB (HZBGenerator)",
                .Reason = "A pyramid that outlives the frame and is rebuilt IN PLACE. The prologue hands the retained "
                          "pyramid to the GPU instance cull (phase 1 dispatches at submission); phase-2 passes rebuild "
                          "it from partial depth; the epilogue (Renderer3D::GenerateOcclusionHZB) rebuilds it from the "
                          "final depth for next frame.",
            },
            RGOutOfBandBoundary{
                .Name = ForwardPlusLightClusters,
                .Kind = RGOutOfBandKind::CpuPublication,
                .Owner = "Renderer3D ForwardPlus",
                .Reason = "SceneRenderPass dispatches the Forward+ light cull inline and leaves the cluster buffers and "
                          "parameters for later passes to capture or rebind; they are not graph resources.",
            },
            RGOutOfBandBoundary{
                .Name = SceneOpaqueCommandBucket,
                .Kind = RGOutOfBandKind::CpuPublication,
                .Owner = "SceneRenderPass command bucket",
                .Reason = "A CPU command list SceneRenderPass sorts and batches, then other passes replay.",
            },
            RGOutOfBandBoundary{
                .Name = FroxelFogVolume,
                .Kind = RGOutOfBandKind::CpuPublication,
                .Owner = "VolumetricFogPass",
                .Reason = "The integrated froxel volume, its UBO and the ran-this-frame flag are published for FogPass, "
                          "which samples the volume at TEX_FROXEL_FOG outside graph tracking.",
            },
            RGOutOfBandBoundary{
                .Name = PlanarReflectionTexture,
                .Kind = RGOutOfBandKind::CpuPublication,
                .Owner = "PlanarReflectionRenderPass",
                .Reason = "A pass-owned framebuffer whose texture id reaches Water.glsl through "
                          "Renderer3D::SetPlanarReflectionTextureID and UBO 43.",
            },
            RGOutOfBandBoundary{
                .Name = DDGIProbeVolume,
                .Kind = RGOutOfBandKind::GpuResource,
                .Owner = "DDGIProbeUpdatePass",
                .Reason = "Irradiance / visibility atlases and the DDGI UBO are sampled at engine slots by every lit "
                          "shader, not through a graph read.",
            },
            RGOutOfBandBoundary{
                .Name = FluidIntermediates,
                .Kind = RGOutOfBandKind::GpuResource,
                .Owner = "FluidIntermediatesPass",
                .Reason = "Pass-owned raw targets FluidCompositePass samples by texture id, plus the ran-this-frame flag.",
            },
            RGOutOfBandBoundary{
                .Name = SSAOParameters,
                .Kind = RGOutOfBandKind::CpuPublication,
                .Prologue = RGFramePhaseUse::Write,
                .Owner = "Renderer3D PostProcessGPU.SSAO (UBO_SSAO)",
                .Reason = "SSAOPass publishes its parameters into the shared SSAO UBO after recording; AOApplyPass "
                          "binds that UBO. With GTAO selected the prologue (RenderPipeline::UploadExecutionState) "
                          "uploads it instead.",
            },
        };

        constexpr std::array kFramePhaseWork{
            RGFramePhaseWork{ "GPUSceneUpload", RGFramePhase::Prologue, "Renderer3D::EndScene -> GPUScene::Upload",
                              "every raster draw (CommandDispatch) and ray-tracing pass",
                              "Committed once per frame before any pass can read a table." },
            RGFramePhaseWork{ "GPUSceneDrawLinks", RGFramePhase::Prologue, "Renderer3D::ResolveGPUSceneDrawLinks",
                              "CommandDispatch", "CPU resolution of staged draw links after the commit." },
            RGFramePhaseWork{ "RasterMaterialTextures", RGFramePhase::Prologue,
                              "Renderer3D::EndScene -> MaterialShaderHeapTable::Update",
                              "Deferred raster draws, RayTracedShadowPass, RayTracedReflectionPass",
                              "Built from the committed GPU-scene materials." },
            RGFramePhaseWork{ "PathTracerTables", RGFramePhase::Prologue,
                              "Renderer3D::EndScene -> MaterialTextureTable / EmissiveTriangleTable::EndFrame",
                              "ReSTIRDI, ReSTIRGI, ReSTIRPT, GpuPathTracer",
                              "Resolved against the committed records; handed to passes in ConfigurePassesForFrame." },
            RGFramePhaseWork{ "VegetationAndGroomProxies", RGFramePhase::Prologue,
                              "Renderer3D::EndScene -> VegetationSurfaceCache::FinishExtraction, GroomSurfaceCache::Extract",
                              "RayTracingScenePass (BLAS build)", "CPU-built geometry uploaded with the GPU scene." },
            RGFramePhaseWork{ "GPUInstanceCullPhase1", RGFramePhase::Prologue,
                              "Renderer3D::SubmitGPUCulledInstanced -> GPUFrustumCuller::Cull / CullTwoPhasePhase1",
                              "ScenePass / GPUDrivenOcclusionPass draw packets",
                              "Dispatched at submission time; reads the retained OcclusionHZB (declared on that boundary)." },
            RGFramePhaseWork{ "WindField", RGFramePhase::Prologue, "RenderPipeline::UploadExecutionState -> WindSystem",
                              "foliage, snow and water shading (bound globally)", "A per-frame simulation input." },
            RGFramePhaseWork{ "SnowAccumulation", RGFramePhase::Prologue,
                              "RenderPipeline::UploadExecutionState -> SnowAccumulationSystem; Scene::UpdateSnowDeformers",
                              "mesh shading via CommandDispatch::SetSnowDepthTexture", "A per-frame simulation input." },
            RGFramePhaseWork{ "WaterDisturbance", RGFramePhase::Prologue,
                              "RenderPipeline::UploadExecutionState -> WaterDisturbanceSystem", "WaterPass",
                              "A persistent field advanced once per frame." },
            RGFramePhaseWork{ "GPUParticleSimulation", RGFramePhase::Prologue,
                              "RenderPipeline::UploadExecutionState (spray, ejecta, precipitation); Scene::UpdateParticlesGPU",
                              "ParticlePass render callback", "Simulated before the frame is drawn." },
            RGFramePhaseWork{ "GPUFluidSolver", RGFramePhase::Prologue, "Scene -> FluidSystem::OnUpdate -> GPUFluidSolver",
                              "FluidIntermediatesPass", "Simulated during scene update." },
            RGFramePhaseWork{ "CloudNoiseAndShadow", RGFramePhase::Prologue,
                              "RenderPipeline::UploadExecutionState -> CloudNoise::EnsureGenerated, CloudShadowMap",
                              "Cloudscape pass, mesh shading via SetCloudShadowTexture", "Baked once / per frame before the graph." },
            RGFramePhaseWork{ "VolumetricShadowVolume", RGFramePhase::Prologue,
                              "RenderPipeline::UploadExecutionState -> VolumetricShadowMap",
                              "fog and atmosphere shading", "Imported into the graph for capture only; it orders nothing." },
            RGFramePhaseWork{ "TerrainVirtualTextureAndLod", RGFramePhase::Prologue,
                              "Scene::ProcessScene3DSharedLogic -> TerrainVirtualTexture, TerrainChunkManager",
                              "terrain draws", "Baked between BeginScene and EndScene." },
            RGFramePhaseWork{ "FoliageMainViewCull", RGFramePhase::Prologue,
                              "Scene -> FoliageRenderer -> FoliageGPUCuller", "FoliagePass", "Dispatched at submission." },
            RGFramePhaseWork{ "OceanFFT", RGFramePhase::Prologue, "Scene -> OceanFFTField", "WaterPass",
                              "Simulated during scene update." },
            RGFramePhaseWork{ "GPUReadbackStatsBegin", RGFramePhase::Prologue, "RenderPipeline::PrepareFrame -> GPUReadbackStats::BeginFrame",
                              "every counter-publishing dispatch", "Zeroes the live block before the first publisher." },
            RGFramePhaseWork{ "OcclusionHZBFinalRebuild", RGFramePhase::Epilogue, "Renderer3D::GenerateOcclusionHZB",
                              "next frame's GPU instance cull and VirtualGeometryPass",
                              "Reads the final depth through a declared frame-epilogue read; writes OcclusionHZB." },
            RGFramePhaseWork{ "FrameCaptureCommit", RGFramePhase::Epilogue, "FrameCaptureManager::CommitFrame",
                              "the frame debugger", "CPU only: every bucket pass has accumulated its capture." },
            RGFramePhaseWork{ "OcclusionQueryEndFrame", RGFramePhase::Epilogue, "OcclusionQueryPool::EndFrame",
                              "next frame's OcclusionQueryPool::BeginFrame", "Queries were issued inside ScenePass." },
            RGFramePhaseWork{ "GPUReadbackStatsEnd", RGFramePhase::Epilogue, "GPUReadbackStats::EndFrame",
                              "olo_gpu_readback_stats, RendererProfiler", "Copies the stats block after every publisher ran." },
            RGFramePhaseWork{ "GPUFrameTimestamp", RGFramePhase::Epilogue, "GPUPassTimerPool::EndFrame",
                              "olo_perf_pass_timings", "Stamps the frame end after all GPU work." },
            RGFramePhaseWork{ "FrameFence", RGFramePhase::Epilogue, "FrameResourceManager::EndFrame",
                              "the next use of this frame's resources", "Fences the frame." },
        };

        constexpr std::array kSideEffectReasons{
            RGSideEffectReason{ "FinalPass", "Present: writes the backbuffer / swap chain." },
            RGSideEffectReason{ "RayTracingScenePass",
                                "NeverCull: the BLAS/TLAS refit chain must advance every frame, and the MCP ray probe "
                                "runs inside this pass; its RT consumers are declared on SceneTLAS." },
            RGSideEffectReason{ "DDGIProbeUpdatePass",
                                "NeverCull: the probe volume is sampled at engine slots by every lit shader, including "
                                "passes that declare nothing; its in-graph consumers are declared on DDGIProbeVolume." },
            RGSideEffectReason{ "VirtualShadowMapMarkPass",
                                "NeverCull: the page requests it writes are consumed by NEXT frame's ShadowPass." },
        };

        std::atomic<RGOutOfBandLedger*> s_ActiveLedger{ nullptr };

        struct OmitFaultState
        {
            std::mutex Mutex;
            bool Seeded = false;
            std::optional<std::string> Spec;
        };

        OmitFaultState& GetOmitFaultState()
        {
            static OmitFaultState s_State;
            return s_State;
        }
    } // namespace

    std::span<const RGOutOfBandBoundary> GetProductionOutOfBandBoundaries()
    {
        return kProductionBoundaries;
    }

    std::span<const RGFramePhaseWork> GetFramePhaseWork()
    {
        return kFramePhaseWork;
    }

    std::span<const RGSideEffectReason> GetSideEffectReasons()
    {
        return kSideEffectReasons;
    }

    std::string_view FindSideEffectReason(const std::string_view passName)
    {
        for (const RGSideEffectReason& reason : kSideEffectReasons)
        {
            if (reason.PassName == passName)
                return reason.Reason;
        }
        return {};
    }

    std::string_view ToString(const RGOutOfBandKind kind)
    {
        switch (kind)
        {
            case RGOutOfBandKind::GpuResource:
                return "gpu-resource";
            case RGOutOfBandKind::CpuPublication:
                return "cpu-publication";
        }
        return "unknown";
    }

    std::string_view ToString(const RGOutOfBandAccess access)
    {
        switch (access)
        {
            case RGOutOfBandAccess::Read:
                return "read";
            case RGOutOfBandAccess::ReadPreviousFrame:
                return "read-previous-frame";
            case RGOutOfBandAccess::Write:
                return "write";
        }
        return "unknown";
    }

    std::string_view ToString(const RGFramePhaseUse use)
    {
        switch (use)
        {
            case RGFramePhaseUse::None:
                return "none";
            case RGFramePhaseUse::Read:
                return "read";
            case RGFramePhaseUse::Write:
                return "write";
            case RGFramePhaseUse::ReadWrite:
                return "read-write";
        }
        return "unknown";
    }

    std::string_view ToString(const RGFramePhase phase)
    {
        return phase == RGFramePhase::Prologue ? "prologue" : "epilogue";
    }

    std::string_view ToString(const RGLedgerPhase phase)
    {
        switch (phase)
        {
            case RGLedgerPhase::Idle:
                return "idle";
            case RGLedgerPhase::Prologue:
                return "prologue";
            case RGLedgerPhase::Graph:
                return "graph";
            case RGLedgerPhase::Epilogue:
                return "epilogue";
        }
        return "unknown";
    }

    // ------------------------------------------------------------------------
    // RGOutOfBandLedger
    // ------------------------------------------------------------------------
    void RGOutOfBandLedger::BeginFrame()
    {
        const std::scoped_lock lock(m_Mutex);
        m_Phase = RGLedgerPhase::Prologue;
        m_ActivePass.Reset();
        m_Entries.Reset();
        ++m_FrameSerial;
    }

    void RGOutOfBandLedger::BeginGraph()
    {
        const std::scoped_lock lock(m_Mutex);
        if (m_Phase != RGLedgerPhase::Idle)
            m_Phase = RGLedgerPhase::Graph;
    }

    void RGOutOfBandLedger::BeginEpilogue()
    {
        const std::scoped_lock lock(m_Mutex);
        if (m_Phase != RGLedgerPhase::Idle)
            m_Phase = RGLedgerPhase::Epilogue;
        m_ActivePass.Reset();
    }

    void RGOutOfBandLedger::EndFrame()
    {
        const std::scoped_lock lock(m_Mutex);
        m_Phase = RGLedgerPhase::Idle;
        m_ActivePass.Reset();
    }

    void RGOutOfBandLedger::SetActivePass(const std::string_view passName)
    {
        const std::scoped_lock lock(m_Mutex);
        m_ActivePass = passName;
    }

    void RGOutOfBandLedger::ClearActivePass()
    {
        const std::scoped_lock lock(m_Mutex);
        m_ActivePass.Reset();
    }

    void RGOutOfBandLedger::Note(const std::string_view boundary, const RGOutOfBandAccess access)
    {
        const std::scoped_lock lock(m_Mutex);
        if (m_Phase == RGLedgerPhase::Idle)
            return;
        // A pass touching the same boundary the same way twice is one access;
        // keeping one entry per (pass, boundary, access) bounds the ledger by
        // the declarations rather than by call count.
        for (const Entry& entry : m_Entries)
        {
            if (entry.Boundary == boundary && entry.Pass == m_ActivePass && entry.Phase == m_Phase &&
                entry.Access == access)
            {
                return;
            }
        }
        m_Entries.Add(Entry{ .Boundary = FString(boundary), .Pass = m_ActivePass, .Phase = m_Phase, .Access = access });
    }

    u64 RGOutOfBandLedger::GetFrameSerial() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_FrameSerial;
    }

    RGLedgerPhase RGOutOfBandLedger::GetPhase() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_Phase;
    }

    TArray64<RGOutOfBandLedger::Entry> RGOutOfBandLedger::GetEntries() const
    {
        const std::scoped_lock lock(m_Mutex);
        return m_Entries;
    }

    // ------------------------------------------------------------------------
    // RGOutOfBand free functions
    // ------------------------------------------------------------------------
    namespace RGOutOfBand
    {
        void SetActiveLedger(RGOutOfBandLedger* ledger)
        {
            s_ActiveLedger.store(ledger, std::memory_order_release);
        }

        RGOutOfBandLedger* GetActiveLedger()
        {
            return s_ActiveLedger.load(std::memory_order_acquire);
        }

        void Note(const std::string_view boundary, const RGOutOfBandAccess access)
        {
            if (RGOutOfBandLedger* ledger = GetActiveLedger())
                ledger->Note(boundary, access);
        }

        u64 GetFrameSerial()
        {
            const RGOutOfBandLedger* ledger = GetActiveLedger();
            return ledger ? ledger->GetFrameSerial() : 0u;
        }

        void SetOmittedDeclarationFault(std::optional<std::string> spec)
        {
            OmitFaultState& state = GetOmitFaultState();
            const std::scoped_lock lock(state.Mutex);
            state.Seeded = true;
            state.Spec = std::move(spec);
        }

        bool IsDeclarationOmittedByFault(const std::string_view passName, const std::string_view boundary)
        {
            OmitFaultState& state = GetOmitFaultState();
            const std::scoped_lock lock(state.Mutex);
            if (!state.Seeded)
            {
                state.Seeded = true;
                if (std::optional<std::string> fromLever = Levers::FaultOmitOutOfBandDeclaration();
                    fromLever.has_value() && !fromLever->empty())
                {
                    state.Spec = std::move(fromLever);
                }
            }
            if (!state.Spec.has_value())
                return false;

            const std::string_view spec = *state.Spec;
            const auto slash = spec.find('/');
            if (slash == std::string_view::npos)
                return spec == boundary;
            return spec.substr(0, slash) == passName && spec.substr(slash + 1) == boundary;
        }
    } // namespace RGOutOfBand
} // namespace OloEngine

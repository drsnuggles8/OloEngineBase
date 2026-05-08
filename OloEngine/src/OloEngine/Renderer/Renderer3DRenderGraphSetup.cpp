#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/Renderer3DInternal.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/RenderPipelineBuilder.h"
#include "OloEngine/Renderer/ShaderConstants.h"

#include <algorithm>
#include <cstdlib>

namespace OloEngine
{
    namespace
    {
        bool IsTruthyEnvironmentVariable(const char* name)
        {
            const char* value = std::getenv(name);
            return value && value[0] != '\0' && value[0] != '0' && value[0] != 'f' && value[0] != 'F';
        }

        bool IsRenderGraphDiagnosticsEnabled()
        {
            static const bool enabled = IsTruthyEnvironmentVariable("OLO_RENDERGRAPH_DIAGNOSTICS");
            return enabled;
        }
    } // namespace

    void Renderer3D::SetupRenderGraph(u32 width, u32 height)
    {
        OLO_PROFILE_FUNCTION();
        if (IsRenderGraphDiagnosticsEnabled())
            OLO_CORE_TRACE("Setting up Renderer3D RenderGraph with dimensions: {}x{}", width, height);

        if (width == 0 || height == 0)
        {
            OLO_CORE_WARN("Invalid dimensions for RenderGraph: {}x{}", width, height);
            return;
        }

        s_Data.RGraph->Init(width, height);

        // Phase D: enable runtime materialization of compiled transient plan
        // entries. Unit tests keep this path disabled by default; production
        // renderer setup opts in explicitly.
        s_Data.RGraph->SetTransientMaterializationEnabled(true);

        FramebufferSpecification shadowPassSpec;
        shadowPassSpec.Width = static_cast<u32>(ShaderConstants::SHADOW_MAP_SIZE);
        shadowPassSpec.Height = static_cast<u32>(ShaderConstants::SHADOW_MAP_SIZE);

        FramebufferSpecification scenePassSpec;
        scenePassSpec.Width = width;
        scenePassSpec.Height = height;
        scenePassSpec.Samples = 1;
        scenePassSpec.Attachments = {
            FramebufferTextureFormat::RGBA16F,     // [0] HDR color output
            FramebufferTextureFormat::RED_INTEGER, // [1] Entity ID attachment
            FramebufferTextureFormat::RG16F,       // [2] View-space normals (octahedral encoded for SSAO)
            FramebufferTextureFormat::RG16F,       // [3] Screen-space velocity (forward-path TAA input; unused in Deferred, which reads G-Buffer RT3)
            FramebufferTextureFormat::Depth
        };

        FramebufferSpecification finalPassSpec;
        finalPassSpec.Width = width;
        finalPassSpec.Height = height;

        s_Data.Pipeline->Setup(s_Data, m_ShaderLibrary, shadowPassSpec, scenePassSpec, finalPassSpec);

        // All passes are now constructed. Build the initial graph topology
        // for the currently-configured rendering path. Runtime switches
        // between Forward / Forward+ / Deferred re-run ConfigureRenderGraph
        // from ApplyRendererSettings so the graph only ever contains the
        // passes that are relevant for the active path.
        ConfigureRenderGraph(s_Data.Settings.Path);
    }

    void Renderer3D::FinalizeConfiguredRenderGraph(RenderingPath path)
    {
        const bool deferred = (path == RenderingPath::Deferred);

        // The topology is graph-native now: ordering is derived primarily from
        // per-pass resource declarations, while this helper is responsible for
        // reporting the configured chain, validating the resource contract, and
        // committing the active-path bookkeeping once the rebuild succeeds.
        if (IsRenderGraphDiagnosticsEnabled())
        {
            if (deferred)
            {
                OLO_CORE_TRACE("Renderer3D: Render graph (Deferred): Shadow -> Scene -> DeferredOpaqueDecal -> DeferredLighting -> ForwardOverlay -> Foliage -> Decal -> Water -> SSAO/GTAO -> Particle -> OITResolve -> SSS -> AOApply -> Bloom -> DOF -> MotionBlur -> TAA -> Precipitation -> Fog -> ChromAb -> ColorGrading -> ToneMap -> Vignette -> FXAA{} -> UIComposite -> Final",
                               s_Data.EnableSelectionOutline ? " -> SelectionOutline" : "");
            }
            else
            {
                OLO_CORE_TRACE("Renderer3D: Render graph ({}): Shadow -> Scene -> Foliage -> Decal -> Water -> SSAO/GTAO -> Particle -> OITResolve -> SSS -> AOApply -> Bloom -> DOF -> MotionBlur -> TAA -> Precipitation -> Fog -> ChromAb -> ColorGrading -> ToneMap -> Vignette -> FXAA{} -> UIComposite -> Final",
                               path == RenderingPath::ForwardPlus ? "Forward+" : "Forward",
                               s_Data.EnableSelectionOutline ? " -> SelectionOutline" : "");
            }
        }

        // Validate the resource-aware RDG contract: every pass's declared
        // reads must have a transitive execution dependency on their
        // producer. Hazards are logged to the engine logger; in debug
        // builds we additionally assert so regressions surface immediately.
        const auto hazards = s_Data.RGraph->ValidateResourceHazards();
        if (!hazards.empty())
        {
            // Any Cycle entry means validation couldn't even run;
            // surface that distinctly from genuine resource hazards.
            const bool cycle = std::any_of(hazards.begin(), hazards.end(),
                                           [](const auto& h)
                                           { return h.Kind == RenderGraph::HazardKind::Cycle; });
            if (cycle)
            {
                OLO_CORE_ERROR("Renderer3D: RenderGraph dependency cycle detected — resource hazard validation aborted.");
                OLO_CORE_ASSERT(!cycle, "RenderGraph dependency cycle detected. Break the cycle and retry.");
            }
            else
            {
                OLO_CORE_ERROR("Renderer3D: RenderGraph has {} resource hazards — see previous log entries for details.", hazards.size());
                OLO_CORE_ASSERT(hazards.empty(), "RenderGraph resource hazard detected (see log). Add ConnectPass / AddExecutionDependency for the reported producer -> consumer edge.");
            }
        }
        else if (IsRenderGraphDiagnosticsEnabled())
        {
            OLO_CORE_TRACE("Renderer3D: RenderGraph resource hazard validation passed.");
        }

        s_Data.ActiveGraphPath = path;
        s_Data.ActiveGraphAOTechnique = s_Data.PostProcess.ActiveAOTechnique;
    }

    void Renderer3D::ConfigureRenderGraph(RenderingPath path)
    {
        OLO_PROFILE_FUNCTION();
        if (IsRenderGraphDiagnosticsEnabled())
        {
            OLO_CORE_TRACE("Renderer3D: Configuring RenderGraph for path = {}",
                           path == RenderingPath::Forward       ? "Forward"
                           : path == RenderingPath::ForwardPlus ? "Forward+"
                                                                : "Deferred");
        }

        const auto inputs = s_Data.Pipeline->BuildInputs(s_Data);

        // Delegate the production frame topology to the dedicated builder so
        // Renderer3D stays the frontend/facade that owns live pass instances,
        // diagnostics, and path-switch validation rather than the entire
        // node-registration recipe.
        BuildRenderPipelineGraph(inputs, path);

        FinalizeConfiguredRenderGraph(path);
    }
} // namespace OloEngine

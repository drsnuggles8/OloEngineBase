#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderingPath.h"

namespace OloEngine
{
    enum class AOTechnique : i32;
    class RenderGraph;
    class RenderGraphNode;

    // Pipeline pass inputs. Each field carries a pointer to a graph node the
    // builder will register under the corresponding canonical name. The
    // fields are typed as the unified `RenderGraphNode*` base — the pipeline
    // builder doesn't need to know any concrete pass class, it just adds the
    // node and lets the graph compiler resolve the rest from the node's own
    // Setup-time declarations. Callers populate the fields via implicit
    // upcast from each concrete pass class (which derives `RenderGraphNode`).
    struct RenderPipelinePassInputs
    {
        RenderGraphNode* Scene = nullptr;
        RenderGraphNode* Shadow = nullptr;
        RenderGraphNode* DDGIProbeUpdate = nullptr; // #632 realtime DDGI capture/relight/blend
        RenderGraphNode* DeferredLighting = nullptr;
        RenderGraphNode* DeferredOpaqueDecal = nullptr;
        RenderGraphNode* DeferredGPUOcclusion = nullptr;
        RenderGraphNode* PlanarReflection = nullptr;
        RenderGraphNode* GPUOcclusion = nullptr;
        RenderGraphNode* ForwardOverlay = nullptr;
        RenderGraphNode* Foliage = nullptr;
        RenderGraphNode* Water = nullptr;
        RenderGraphNode* FluidIntermediates = nullptr;
        RenderGraphNode* FluidComposite = nullptr;
        RenderGraphNode* VirtualGeometry = nullptr; // #629 cluster LOD DAG cull + raster
        RenderGraphNode* Decal = nullptr;
        RenderGraphNode* SSAO = nullptr;
        RenderGraphNode* GTAO = nullptr;
        RenderGraphNode* Particle = nullptr;
        RenderGraphNode* OITPrepare = nullptr;
        RenderGraphNode* OITResolve = nullptr;
        RenderGraphNode* SSS = nullptr;
        RenderGraphNode* AOApply = nullptr;
        RenderGraphNode* SSGI = nullptr;
        RenderGraphNode* SSR = nullptr;
        RenderGraphNode* ContactShadow = nullptr;
        RenderGraphNode* EASU = nullptr;
        RenderGraphNode* DepthVelocityUpscale = nullptr;
        RenderGraphNode* Bloom = nullptr;
        RenderGraphNode* DOF = nullptr;
        RenderGraphNode* MotionBlur = nullptr;
        RenderGraphNode* TAA = nullptr;
        RenderGraphNode* Cloudscape = nullptr; // #633 volumetric cloudscape (between TAA and Precipitation)
        RenderGraphNode* Precipitation = nullptr;
        RenderGraphNode* VolumetricFog = nullptr;
        RenderGraphNode* Fog = nullptr;
        RenderGraphNode* ChromAberration = nullptr;
        RenderGraphNode* ColorGrading = nullptr;
        RenderGraphNode* ToneMap = nullptr;
        RenderGraphNode* Upscaler = nullptr;
        RenderGraphNode* Vignette = nullptr;
        RenderGraphNode* FXAA = nullptr;
        RenderGraphNode* SelectionOutline = nullptr;
        RenderGraphNode* Overdraw = nullptr;
        RenderGraphNode* UIComposite = nullptr;
        RenderGraphNode* Final = nullptr;
    };

    struct RenderPipelineInputs
    {
        RenderGraph* Graph = nullptr;
        RenderPipelinePassInputs Passes;
        AOTechnique ActiveAOTechnique{};
    };

    void BuildRenderPipelineGraph(const RenderPipelineInputs& inputs, RenderingPath path);
} // namespace OloEngine

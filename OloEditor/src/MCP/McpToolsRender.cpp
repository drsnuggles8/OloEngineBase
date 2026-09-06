#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RHI/RHIProjectionSeam.h"
#include "MCP/McpEditorLiveness.h"
#include "MCP/McpToolsCommon.h"
#include "MCP/McpCaptureRegion.h"
#include "MCP/McpPostProcessSettings.h"
#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpClusterGridStats.h"
#include "MCP/McpDDGIProbeStats.h"
#include "MCP/McpFrameBreakdown.h"
#include "MCP/McpFroxelFogProbe.h"
#include "MCP/McpGpuReadbackStats.h"
#include "MCP/McpGoldenCompare.h"
#include "MCP/McpRenderExplain.h"
#include "MCP/McpRenderGraphTopology.h"
#include "MCP/McpRenderOverrides.h"
#include "MCP/McpRenderProbePixel.h"
#include "MCP/McpRenderLODStats.h"
#include "MCP/McpRayTracingStats.h"

#include "OloEngine/Renderer/RayTracing/RayTracingScene.h"
#include "MCP/McpRenderTargetStats.h"
#include "MCP/McpRenderValidate.h"
#include "MCP/McpRendererSettings.h"
#include "MCP/McpShadowCapture.h"
#include "MCP/McpVirtualShadowMapStats.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Atmosphere/Ephemeris.h"
#include "OloEngine/Atmosphere/WeatherSystem.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Debug/CapturedFrameData.h"
#include "OloEngine/Renderer/Debug/CommandPacketDebugger.h"
#include "OloEngine/Renderer/Debug/FrameCaptureManager.h"
#include "OloEngine/Renderer/Debug/GPUReadbackStats.h"
#include "OloEngine/Renderer/Debug/GPUResourceInspector.h"
#include "OloEngine/Renderer/Debug/ResourceInspectorBackend.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Debug/ShaderDebugDraw.h"
#include "OloEngine/Renderer/Debug/ShaderDebugDrawTypes.h"
#include "OloEngine/Renderer/Debug/ShaderDebugger.h"
#include "OloEngine/Renderer/DDGI/DDGIProbeUpdatePass.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
#include "OloEngine/Renderer/RenderCommand.h"

#include <stb_image/stb_image_write.h>
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Renderer/LightCulling/ClusteredLighting.h"
#include "OloEngine/Renderer/LightCulling/LightGrid.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Renderer/Passes/CommandBufferRenderPass.h"
#include "OloEngine/Renderer/Passes/VolumetricFogPass.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/Debug/RenderGraphResourceIdentity.h"
#include "OloEngine/Renderer/TransientPool.h"
#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/SubmeshMaterialResolve.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Shadow/ShadowAtlas.h"
#include "OloEngine/Renderer/Passes/RayTracedShadowPass.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshRegistry.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Terrain/VirtualTexture/TerrainVirtualTexture.h"

#include <glad/gl.h>
#include <stb_image/stb_image.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Rendering MCP tools: frame breakdown, render-target listing/capture, the
// render-graph topology export, the ephemeral override A/B tools (toggle pass,
// debug view, renderer settings), the atmosphere tools (time-of-day / sun-angle
// / weather component writes + the read-only atmosphere report, issue #633),
// golden-image comparison, and the olo_render_why_not_visible explainer. Split
// out of the McpTools.cpp monolith (issue #357).

namespace OloEngine::MCP
{
    namespace
    {
        // Defined further down (next to the topology export handler that shares it);
        // forward-declared so the frame-breakdown handler can reuse the same
        // enum -> string mapping the topology export uses.
        const char* PassWorkTypeName(RenderGraphPassWorkType type);

        // Defined in the issue-#607 probe/snapshot section further down;
        // forward-declared so the capture handler (earlier in the TU) can share
        // the afterPass snapshot machinery with probe/stats/validate.
        std::string ArmAfterPassSnapshot(McpServer& server, const std::string& passName,
                                         const std::vector<std::string>& resources,
                                         bool& outFrameRendered);
        std::string CollectAfterPassSnapshot(const std::string& passName, const std::string& name,
                                             bool frameRendered, RenderGraphPassSnapshot::Result& outResult);

        // Defined further down in the virtual-geometry section. Forward-declared so
        // olo_render_set_debug_view's vg* modes read and write the SAME
        // VirtualMeshRegistry state olo_virtual_geometry_set does — one write path,
        // so the two tools cannot disagree about what is currently on (issue #607).
        const char* VirtualDebugModeToken(VirtualDebugMode mode);
        void ApplyVirtualDebugMode(VirtualDebugMode mode);
        // Frames to settle after a virtual-geometry debug-mode change: the mode gates
        // a render-graph DECLARATION (the "VirtualGeometryDebug" import), so the
        // topology must rebuild before a following capture can resolve the target.
        constexpr int kVirtualDebugSettleFrames = 3;

        // Defined further down. Forward-declared so olo_gpu_readback_stats (#721,
        // earlier in the TU) can settle a few frames after flipping the channel —
        // its counters are read a ring's-worth of frames late, so a report taken
        // immediately after the write would describe the OLD state and read as
        // "the write did nothing".
        bool ForceFreshFrame(McpServer& server, int settleFrames);

        // ---- olo_render_frame_breakdown (main-marshaled) -----------------------
        // The per-command / per-pipeline-stage structural view olo_perf_capture_frame
        // omits. Same capture-then-poll trigger as Handle_PerfCaptureFrame, then the
        // freshly captured frame is shaped by the pure FrameBreakdown::BuildBreakdown
        // (JSON) or CommandPacketDebugger::BuildMarkdownReport (the Command Bucket
        // Inspector's LLM-analysis report). After capture, the live render graph is
        // read once more to attribute the captured bucket to its owning pass and place
        // it in the whole-graph command-bucket landscape (#316). Pure read — no
        // override / mutation.
        ToolResult Handle_RenderFrameBreakdown(McpServer& server, const Json& args)
        {
            const bool explicitViewMode = args.contains("viewMode");
            const bool explicitMaxCommands = args.contains("maxCommands");

            FrameBreakdown::ViewMode requested = FrameBreakdown::ViewMode::PostBatch;
            if (explicitViewMode && args["viewMode"].is_string())
                requested = FrameBreakdown::ParseViewMode(args["viewMode"].get<std::string>());

            int maxCommands = 200;
            if (explicitMaxCommands && args["maxCommands"].is_number_integer())
                maxCommands = static_cast<int>(std::clamp<long long>(args["maxCommands"].get<long long>(), 1, 5000));

            std::string format = "json";
            if (args.contains("format") && args["format"].is_string())
            {
                format = args["format"].get<std::string>();
                if (format != "json" && format != "markdown")
                    return ToolResult::Error("format must be \"json\" or \"markdown\".");
            }

            // viewMode / maxCommands shape the JSON command list; the markdown report
            // is a fixed document that always covers all stages and every command, so
            // reject them rather than silently ignoring them.
            if (format == "markdown" && (explicitViewMode || explicitMaxCommands))
                return ToolResult::Error("viewMode and maxCommands apply to format:\"json\" only — the markdown "
                                         "report always covers all pipeline stages and every command. Omit them, "
                                         "or use format:\"json\".");

            // Trigger a one-frame capture on the game thread and note how many frames
            // were already retained, so we can detect the new one (identical to
            // Handle_PerfCaptureFrame).
            const Json trigger = server.MarshalRead([]() -> Json
                                                    {
                FrameCaptureManager& fcm = FrameCaptureManager::GetInstance();
                const auto before = static_cast<u64>(fcm.GetCapturedFramesCopy().size());
                fcm.CaptureNextFrame();
                return Json{ { "before", before } }; });
            const auto before = trigger.value("before", static_cast<u64>(0));

            std::deque<CapturedFrameData> frames;
            bool captured = false;
            int polls = 0;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (server.IsCurrentCallCancelled())
                    return ToolResult::Error("Cancelled while waiting for the frame capture.");
                frames = FrameCaptureManager::GetInstance().GetCapturedFramesCopy();
                if (static_cast<u64>(frames.size()) > before && !frames.empty())
                {
                    captured = true;
                    break;
                }
                server.EmitProgress(static_cast<f64>(++polls), -1.0, "waiting for the captured frame");
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (!captured)
                return ToolResult::Error("Frame capture timed out (is the editor rendering the viewport?).");

            const CapturedFrameData& cap = frames.back();

            if (format == "markdown")
                return ToolResult::Text(CommandPacketDebugger::BuildMarkdownReport(cap));

            // Gather the live render graph's command-bucket landscape so the
            // captured single-pass bucket can be placed in the whole-graph picture
            // (#316). The graph is main-thread state, so the walk runs inside
            // MarshalRead; the shaping stays in the pure FrameBreakdown core. A
            // missing graph (2D mode / no frame yet) just omits the attribution.
            FrameBreakdown::GraphAttribution attribution;
            attribution.CaptureSourcePass = cap.SourcePassName;
            const Json gathered = server.MarshalRead([&attribution]() -> Json
                                                     {
                const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph();
                if (!graph)
                    return Json{ { "haveGraph", false } };

                const std::vector<std::string>& order = graph->GetExecutionOrder();
                std::unordered_map<std::string, int> executionIndex;
                executionIndex.reserve(order.size());
                for (int i = 0; i < static_cast<int>(order.size()); ++i)
                    executionIndex.emplace(order[i], i);

                const auto& culled = graph->GetCulledPasses();
                const std::unordered_set<std::string> culledSet(culled.begin(), culled.end());
                const std::string& finalPass = graph->GetFinalPassName();

                for (const auto& info : graph->GetNodeSubmissionInfo())
                {
                    FrameBreakdown::GraphPassInfo pass;
                    pass.Name = info.NodeName;
                    pass.WorkType = PassWorkTypeName(info.WorkType);
                    // A pass owns a command bucket iff it is a CommandBufferRenderPass
                    // (Ref::As uses dynamic_cast, so this is an exact type test).
                    pass.UsesCommandBucket = graph->GetNode<CommandBufferRenderPass>(info.NodeName) != nullptr;
                    pass.Culled = culledSet.contains(info.NodeName);
                    pass.IsFinalPass = !finalPass.empty() && info.NodeName == finalPass;
                    if (const auto it = executionIndex.find(info.NodeName); it != executionIndex.end())
                        pass.ExecutionIndex = it->second;
                    attribution.Passes.push_back(std::move(pass));
                }
                return Json{ { "haveGraph", true } }; });

            const bool haveGraph = gathered.is_object() && gathered.value("haveGraph", false);
            const Json o = FrameBreakdown::BuildBreakdown(cap, requested, maxCommands,
                                                          haveGraph ? &attribution : nullptr);
            return ToolResult::Structured(o);
        }

        // ---- Render-target capture (#316) ----------------------------

        const char* RGFormatName(RGResourceFormat format)
        {
            switch (format)
            {
                case RGResourceFormat::Unknown:
                    return "Unknown";
                case RGResourceFormat::R8UNorm:
                    return "R8UNorm";
                case RGResourceFormat::R32Float:
                    return "R32Float";
                case RGResourceFormat::RG16Float:
                    return "RG16Float";
                case RGResourceFormat::RGBA8UNorm:
                    return "RGBA8UNorm";
                case RGResourceFormat::RGBA16Float:
                    return "RGBA16Float";
                case RGResourceFormat::RGBA32Float:
                    return "RGBA32Float";
                case RGResourceFormat::Depth24Stencil8:
                    return "Depth24Stencil8";
                case RGResourceFormat::Depth32Float:
                    return "Depth32Float";
                case RGResourceFormat::R32Int:
                    return "R32Int";
            }
            return "Unknown";
        }

        const char* PassWorkTypeName(RenderGraphPassWorkType type)
        {
            switch (type)
            {
                case RenderGraphPassWorkType::Graphics:
                    return "Graphics";
                case RenderGraphPassWorkType::Compute:
                    return "Compute";
                case RenderGraphPassWorkType::Copy:
                    return "Copy";
            }
            return "Graphics";
        }

        const char* TemporalEffectName(TemporalHistoryEffect effect)
        {
            switch (effect)
            {
                case TemporalHistoryEffect::TAA:
                    return "TAA";
                case TemporalHistoryEffect::SSGI:
                    return "SSGI";
                case TemporalHistoryEffect::SSR:
                    return "SSR";
                case TemporalHistoryEffect::Cloudscape:
                    return "Cloudscape";
                case TemporalHistoryEffect::RayTracedShadow:
                    return "RayTracedShadow";
            }
            return "Unknown";
        }

        const char* TemporalPlaneName(TemporalHistoryPlane plane)
        {
            switch (plane)
            {
                case TemporalHistoryPlane::Signal:
                    return "Signal";
                case TemporalHistoryPlane::SurfaceDepth:
                    return "SurfaceDepth";
                case TemporalHistoryPlane::SurfaceGeometry:
                    return "SurfaceGeometry";
                case TemporalHistoryPlane::SurfaceIdentity:
                    return "SurfaceIdentity";
                case TemporalHistoryPlane::MomentsFirst:
                    return "MomentsFirst";
                case TemporalHistoryPlane::MomentsSecond:
                    return "MomentsSecond";
                case TemporalHistoryPlane::Diagnostics:
                    return "Diagnostics";
            }
            return "Unknown";
        }

        const char* TemporalResolutionName(TemporalHistoryResolution resolution)
        {
            switch (resolution)
            {
                case TemporalHistoryResolution::Display:
                    return "Display";
                case TemporalHistoryResolution::Scene:
                    return "Scene";
                case TemporalHistoryResolution::Half:
                    return "Half";
                case TemporalHistoryResolution::Quarter:
                    return "Quarter";
            }
            return "Unknown";
        }

        const char* TemporalBackendName(TemporalHistoryBackend backend)
        {
            switch (backend)
            {
                case TemporalHistoryBackend::Unknown:
                    return "Unknown";
                case TemporalHistoryBackend::OpenGL:
                    return "OpenGL";
                case TemporalHistoryBackend::Vulkan:
                    return "Vulkan";
            }
            return "Unknown";
        }

        const char* TemporalInvalidationName(TemporalHistoryInvalidationCause cause)
        {
            switch (cause)
            {
                case TemporalHistoryInvalidationCause::None:
                    return "None";
                case TemporalHistoryInvalidationCause::FirstUse:
                    return "FirstUse";
                case TemporalHistoryInvalidationCause::DescriptorChanged:
                    return "DescriptorChanged";
                case TemporalHistoryInvalidationCause::CameraCut:
                    return "CameraCut";
                case TemporalHistoryInvalidationCause::ProjectionChanged:
                    return "ProjectionChanged";
                case TemporalHistoryInvalidationCause::ViewportResized:
                    return "ViewportResized";
                case TemporalHistoryInvalidationCause::DynamicResolutionChanged:
                    return "DynamicResolutionChanged";
                case TemporalHistoryInvalidationCause::SceneReset:
                    return "SceneReset";
                case TemporalHistoryInvalidationCause::FeatureToggled:
                    return "FeatureToggled";
                case TemporalHistoryInvalidationCause::BackendChanged:
                    return "BackendChanged";
                case TemporalHistoryInvalidationCause::JitterReset:
                    return "JitterReset";
                case TemporalHistoryInvalidationCause::CopyFailed:
                    return "CopyFailed";
                case TemporalHistoryInvalidationCause::Manual:
                    return "Manual";
            }
            return "Unknown";
        }

        const char* ImageFormatName(ImageFormat format)
        {
            switch (format)
            {
                case ImageFormat::None:
                    return "None";
                case ImageFormat::R8:
                    return "R8";
                case ImageFormat::R8UI:
                    return "R8UI";
                case ImageFormat::R16UI:
                    return "R16UI";
                case ImageFormat::RG16UI:
                    return "RG16UI";
                case ImageFormat::RGB8:
                    return "RGB8";
                case ImageFormat::RGBA8:
                    return "RGBA8";
                case ImageFormat::RGBA16F:
                    return "RGBA16F";
                case ImageFormat::RGBA32F:
                    return "RGBA32F";
                case ImageFormat::R32F:
                    return "R32F";
                case ImageFormat::RG32F:
                    return "RG32F";
                case ImageFormat::RGB32F:
                    return "RGB32F";
                case ImageFormat::DEPTH24STENCIL8:
                    return "DEPTH24STENCIL8";
                case ImageFormat::RG16F:
                    return "RG16F";
                case ImageFormat::R32I:
                    return "R32I";
                case ImageFormat::RG8:
                    return "RG8";
                case ImageFormat::BC7:
                    return "BC7";
                case ImageFormat::BC5:
                    return "BC5";
                case ImageFormat::BC6H:
                    return "BC6H";
                case ImageFormat::BC6HS:
                    return "BC6HS";
                case ImageFormat::RGBA32UI:
                    return "RGBA32UI";
                case ImageFormat::BC4:
                    return "BC4";
                case ImageFormat::R32UI:
                    return "R32UI";
            }
            return "Unknown";
        }

        // Layers addressable through one render-graph resource name, and the layer
        // it addresses inside its parent texture object (issue #607). Shared by
        // olo_render_list_targets (which reports the count so an agent can
        // discover how many cascades exist) and olo_render_capture_target (which
        // validates the requested layer against it).
        //
        // A cube map's desc leaves DepthOrLayers at its 1 default, so the six
        // faces are supplied here — otherwise a legitimate face request on a
        // cubemap target would be rejected as "not an array".
        CaptureLayer::TargetLayers ResolveTargetLayers(const RenderGraph& graph, const std::string& name)
        {
            CaptureLayer::TargetLayers layers;
            if (const auto* resource = graph.FindRegisteredResource(name))
            {
                layers.LayerCount = std::max(resource->Desc.DepthOrLayers, 1u);
                if (resource->Desc.Kind == RGResourceHandle::Kind::TextureCube)
                    layers.LayerCount = std::max(layers.LayerCount, 6u);
            }
            // A layer/face VIEW resolves to its PARENT texture object, so a
            // readback must apply the view's own layer itself or it silently
            // reads layer 0 (see RenderGraph::GetTextureViewLayerIndex).
            layers.ViewLayer = graph.GetTextureViewLayerIndex(name);
            return layers;
        }

        // ---- olo_render_list_targets (main-marshaled) --------------------------
        ToolResult Handle_RenderListTargets(McpServer& server, const Json& /*args*/)
        {
            Json result = server.MarshalRead([]() -> Json
                                             {
                const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph();
                if (!graph)
                    return Json{ { "__error", "No active render graph (the editor is not in 3D mode, or no frame has been rendered yet)." } };

                Json targets = Json::array();
                for (const auto& resource : graph->GetRegisteredResources())
                {
                    // Only capturable resources: those backed by a texture or a
                    // framebuffer. Buffers (UBO/SSBO) are not image-capturable.
                    if (!resource.TextureHandle.IsValid() && !resource.FramebufferHandle.IsValid())
                        continue;
                    Json e;
                    e["name"] = resource.Name;
                    e["kind"] = std::string(ToString(resource.Desc.Kind));
                    if (resource.Desc.Format != RGResourceFormat::Unknown)
                        e["format"] = RGFormatName(resource.Desc.Format);
                    if (resource.Desc.Width > 0 && resource.Desc.Height > 0)
                    {
                        e["width"] = resource.Desc.Width;
                        e["height"] = resource.Desc.Height;
                    }
                    // Layer count of an array / cube / 3D target, so an agent can
                    // DISCOVER how many cascades (or faces, or froxel slices) there
                    // are instead of guessing at olo_render_capture_target's 'layer'.
                    const CaptureLayer::TargetLayers layers = ResolveTargetLayers(*graph, resource.Name);
                    if (CaptureLayer::IsArrayTarget(layers))
                        e["layers"] = layers.LayerCount;
                    if (layers.ViewLayer != 0)
                        e["viewOfParentLayer"] = layers.ViewLayer;
                    if (!resource.Producers.empty())
                        e["producers"] = resource.Producers;
                    targets.push_back(std::move(e));
                }
                Json j;
                j["count"] = static_cast<int>(targets.size());
                j["targets"] = std::move(targets);
                return j; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Structured(result);
        }

        // (main thread) Flatten the channel's live state into the engine-free
        // snapshot McpGpuReadbackStats.h shapes. A named function rather than a
        // lambda inside the handler: it is the whole body of the read, and the
        // handler reads better as "optionally write, then report".
        [[nodiscard("builds the report; it does not send it")]] Json GpuReadbackStatsReportJson()
        {
            namespace Stats = OloEngine::MCP::GpuReadbackStats;
            Stats::StatsSnapshot snapshot;
            snapshot.Enabled = GPUReadbackStats::IsEnabled();
            snapshot.RingSlots = GPUReadbackStats::kRingSlots;
            snapshot.SlotsInFlight = GPUReadbackStats::GetSlotsInFlight();

            const auto& frame = GPUReadbackStats::GetLatest();
            snapshot.Valid = frame.Valid;
            snapshot.FrameIndex = frame.FrameIndex;
            snapshot.LatencyFrames = frame.Latency;

            snapshot.Counters.reserve(kGPUStatCounterCount);
            for (u32 i = 0; i < kGPUStatCounterCount; ++i)
            {
                const auto counter = static_cast<GPUStatCounter>(i);
                snapshot.Counters.emplace_back(std::string{ GPUStatCounterName(counter) },
                                               std::string{ GPUStatCounterDescription(counter) },
                                               frame.Get(counter));
            }

            // Only the flags that FIRED. A list of every flag with a boolean
            // beside it makes "nothing is wrong" and "three things are wrong" the
            // same shape, and an agent has to scan to tell them apart.
            for (u32 i = 0; i < kGPUStatFlagCount; ++i)
            {
                if (const auto flag = static_cast<GPUStatFlag>(i); frame.Overflowed(flag))
                {
                    snapshot.Overflows.emplace_back(std::string{ GPUStatFlagName(flag) },
                                                    std::string{ GPUStatFlagDescription(flag) });
                }
            }
            return Stats::BuildStatsReport(snapshot);
        }

        // ---- olo_gpu_readback_stats (main-marshaled) ---------------------------
        // The structured GPU readback-stats channel (issue #721). Read-only, and
        // read-only in the strong sense: it drains what the channel has ALREADY
        // brought back rather than triggering a readback, so calling it never
        // stalls the frame and never changes what the next frame reports. That
        // matters more here than for most tools — an agent polling a diagnostic
        // must not be able to perturb the thing it is diagnosing.
        ToolResult Handle_GpuReadbackStats(McpServer& server, const Json& args)
        {
            // The one optional argument is a WRITE, so the tool is registered with
            // mutating annotations and goes through the consent gate. Without it
            // an agent cannot A/B the channel's own cost — which is the question
            // most likely to be asked of a diagnostic that is on by default.
            if (const bool hasEnabled = args.contains("enabled") && args["enabled"].is_boolean(); hasEnabled)
            {
                const bool enabled = args["enabled"].get<bool>();
                (void)server.MarshalRead(
                    [enabled]
                    {
                        // The SETTING, not GPUReadbackStats::SetEnabled directly:
                        // PrepareFrame pushes the setting into the channel every
                        // frame, so writing the channel would be reverted on the
                        // very next frame and the tool would report a change that
                        // silently did not stick.
                        Renderer3D::GetRendererSettings().GPUReadbackStatsEnabled = enabled;
                        return Json::object();
                    });
                // The counters are read a few frames late by construction, so a
                // report taken immediately would describe the OLD state and read
                // as "the write did nothing".
                if (server.Context().GetFrameIndex)
                    (void)ForceFreshFrame(server, kVirtualDebugSettleFrames);
            }

            Json result = server.MarshalRead([]
                                             { return GpuReadbackStatsReportJson(); });
            return ToolResult::Structured(result);
        }

        // ---- olo_render_graph_topology_export (main-marshaled) -----------------
        // Read-only structured export of the live RenderGraph topology — passes,
        // execution order, pass-dependency edges, and resources with their
        // producers/consumers — so an agent can reason about the render pipeline
        // (#316, "LLM-analysis exports"). The RenderGraph is main-thread
        // state, so the enumeration runs inside MarshalRead; the JSON / Mermaid
        // shaping is the pure, unit-tested RenderGraphTopology core.
        ToolResult Handle_RenderGraphTopologyExport(McpServer& server, const Json& args)
        {
            std::string format = "json";
            if (args.contains("format") && args["format"].is_string())
            {
                format = args["format"].get<std::string>();
                if (format != "json" && format != "mermaid" && format != "dot")
                    return ToolResult::Error("format must be \"json\", \"mermaid\", or \"dot\".");
            }

            Json transport = server.MarshalRead([format]() -> Json
                                                {
                const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph();
                if (!graph)
                    return Json{ { "__error", "No active render graph (the editor is not in 3D mode, or no frame has been rendered yet)." } };

                RenderGraphTopology::Snapshot snap;
                snap.FinalPass = graph->GetFinalPassName();

                const auto& culled = graph->GetCulledPasses();
                const std::unordered_set<std::string> culledSet(culled.begin(), culled.end());

                for (const auto& info : graph->GetNodeSubmissionInfo())
                {
                    RenderGraphTopology::PassInfo pass;
                    pass.Name = info.NodeName;
                    pass.WorkType = PassWorkTypeName(info.WorkType);
                    pass.DeclaresResources = info.DeclaresResources;
                    pass.AsyncComputeCandidate = info.AsyncComputeCandidate;
                    pass.Culled = culledSet.contains(info.NodeName);
                    pass.IsFinalPass = !snap.FinalPass.empty() && info.NodeName == snap.FinalPass;
                    snap.Passes.push_back(std::move(pass));
                }

                snap.ExecutionOrder = graph->GetExecutionOrder();

                for (const auto& connection : graph->GetConnections())
                    snap.Edges.push_back(RenderGraphTopology::EdgeInfo{ connection.OutputPass, connection.InputPass });

                for (const auto& resource : graph->GetRegisteredResources())
                {
                    RenderGraphTopology::ResourceInfo info;
                    info.Name = resource.Name;
                    info.Kind = std::string(ToString(resource.Desc.Kind));
                    if (resource.Desc.Format != RGResourceFormat::Unknown)
                        info.Format = RGFormatName(resource.Desc.Format);
                    info.Width = resource.Desc.Width;
                    info.Height = resource.Desc.Height;
                    info.Samples = resource.Desc.Samples;
                    info.Imported = resource.Desc.Imported;
                    info.HasExternalBacking = resource.HasExternalBacking;
                    info.Producers = resource.Producers;
                    info.Consumers = resource.Consumers;

                    // Resolved physical backing (issue #607) — the one-call
                    // answer to "do these two passes touch the same physical
                    // texture this frame". Resolved inside this same
                    // main-thread job as the enumeration so the snapshot is
                    // internally consistent; the values are the LAST EXECUTED
                    // frame's (transients can re-alias next frame).
                    //
                    // BOTH currencies (issue #890, ADR 0011 amendment (90)):
                    // the identity is what the "same physical object?" question
                    // is answered in, the native handle is what a RenderDoc
                    // capture shows. Filling only the latter is what made this
                    // export unreadable on Vulkan, where every framebuffer
                    // attachment reports 0.
                    if (resource.TextureHandle.IsValid())
                    {
                        info.NativeTextureHandle =
                            Debug::NativeHandleForDiagnostics(*graph, resource.TextureHandle);
                        info.TextureIdentity = RHI::HashKey(graph->ResolveTextureHandle(resource.TextureHandle));
                        info.ViewOfParentLayer = graph->GetTextureViewLayerIndex(resource.Name);
                    }
                    if (resource.FramebufferHandle.IsValid())
                    {
                        if (const Ref<Framebuffer> framebuffer = graph->ResolveFramebuffer(resource.FramebufferHandle))
                        {
                            info.NativeFramebufferHandle = static_cast<u64>(framebuffer->GetRendererID());
                            const auto& attachments = framebuffer->GetSpecification().Attachments.Attachments;
                            u32 colorIndex = 0;
                            for (const auto& attachment : attachments)
                            {
                                if (attachment.TextureFormat == FramebufferTextureFormat::None)
                                    continue;
                                if (attachment.TextureFormat == FramebufferTextureFormat::DEPTH24STENCIL8 ||
                                    attachment.TextureFormat == FramebufferTextureFormat::DEPTH_COMPONENT32F)
                                {
                                    const RHI::ResourceHandle depth = framebuffer->GetDepthAttachmentHandle();
                                    info.NativeDepthAttachmentHandle = Debug::NativeHandleForDiagnostics(depth);
                                    info.DepthAttachmentIdentity = RHI::HashKey(depth);
                                }
                                else
                                {
                                    const RHI::ResourceHandle color =
                                        framebuffer->GetColorAttachmentHandle(colorIndex);
                                    info.NativeColorAttachmentHandles.push_back(
                                        Debug::NativeHandleForDiagnostics(color));
                                    info.ColorAttachmentIdentities.push_back(RHI::HashKey(color));
                                    ++colorIndex;
                                }
                            }
                        }
                    }
                    if (resource.BufferHandle.IsValid())
                    {
                        // Through the IDENTITY where there is one: every Vulkan
                        // buffer class answers 0 to GetRendererID(), so widening
                        // ResolveBuffer's u32 would report 0 for a buffer whose
                        // real VkBuffer handle we are holding.
                        const RHI::ResourceHandle buffer = graph->ResolveBufferHandle(resource.BufferHandle);
                        info.NativeBufferHandle =
                            buffer.IsValid() ? Debug::NativeHandleForDiagnostics(buffer)
                                             : static_cast<u64>(graph->ResolveBuffer(resource.BufferHandle));
                        info.BufferIdentity = RHI::HashKey(buffer);
                    }

                    snap.Resources.push_back(std::move(info));
                }

                for (const auto& history : graph->GetTemporalHistoryRegistry().Snapshot())
                {
                    snap.Histories.push_back(RenderGraphTopology::HistoryInfo{
                        .Name = history.DebugName,
                        .Effect = TemporalEffectName(history.Key.Effect),
                        .Plane = TemporalPlaneName(history.Key.Plane),
                        .Resolution = TemporalResolutionName(history.Key.Resolution),
                        .Backend = TemporalBackendName(history.Descriptor.Backend),
                        .Format = ImageFormatName(history.Descriptor.Format),
                        .LastInvalidation = TemporalInvalidationName(history.LastInvalidation),
                        .View = history.Key.View,
                        .Width = history.Descriptor.Width,
                        .Height = history.Descriptor.Height,
                        .MipLevels = history.Descriptor.MipLevels,
                        .Samples = history.Descriptor.Samples,
                        .LayoutVersion = history.Descriptor.LayoutVersion,
                        .Generation = history.Token.Generation,
                        .Valid = history.Valid,
                        .HasTexture = history.HasTexture,
                    });
                }

                // Mermaid / DOT are pure transforms of the snapshot, but the snapshot
                // can only be gathered on the main thread, so build the text here and
                // ferry it out under a sentinel key.
                if (format == "mermaid")
                    return Json{ { "__text", RenderGraphTopology::BuildMermaid(snap) } };
                if (format == "dot")
                    return Json{ { "__text", RenderGraphTopology::BuildDot(snap) } };
                return RenderGraphTopology::BuildJson(snap); });

            if (transport.is_object() && transport.contains("__error"))
                return ToolResult::Error(transport["__error"].get<std::string>());
            if (transport.is_object() && transport.contains("__text"))
                return ToolResult::Text(transport["__text"].get<std::string>());
            return ToolResult::Structured(transport);
        }

        // (main thread) `ResolveTargetTexture(name)` used to live here: a
        // render-graph resource name to a truncated backend-native texture id.
        // It is GONE (issue #890). Both of its remaining callers only REPORTED
        // the value, but the name read like "the texture you can use", and the
        // next caller to hand the result to GL would have re-created the
        // SEH-at-0x0 crash #888 had just fixed — a `VkImage` truncated into a
        // u32 is nonzero garbage that passes every validity check on the way.
        // Both callers now resolve through ResolveTargetHandle below and report
        // BOTH currencies; the native handle they print comes from
        // Debug::NativeHandleForDiagnostics at full 64-bit width.

        const char* GpuResourceTypeName(GPUResourceInspector::ResourceType type)
        {
            using RT = GPUResourceInspector::ResourceType;
            switch (type)
            {
                case RT::Texture2D:
                    return "Texture2D";
                case RT::TextureCubemap:
                    return "TextureCubemap";
                case RT::VertexBuffer:
                    return "VertexBuffer";
                case RT::IndexBuffer:
                    return "IndexBuffer";
                case RT::UniformBuffer:
                    return "UniformBuffer";
                case RT::Framebuffer:
                    return "Framebuffer";
                case RT::VertexArray:
                    return "VertexArray";
                case RT::ShaderProgram:
                    return "ShaderProgram";
                case RT::Query:
                    return "Query";
                case RT::Other:
                case RT::COUNT:
                    break;
            }
            return "Other";
        }

        const char* GpuResourceBackendName(RHI::Backend backend)
        {
            switch (backend)
            {
                case RHI::Backend::OpenGL:
                    return "opengl";
                case RHI::Backend::Vulkan:
                    return "vulkan";
                case RHI::Backend::None:
                    break;
            }
            return "none";
        }

        // (main thread) A render-graph resource name to the RHI handle that
        // names it: as a graph texture first (covers attachment views like
        // SceneDepth / GBufferAlbedo and imported textures like ShadowMapCSM),
        // then as a framebuffer (colour attachment 0, or the depth attachment
        // for depth-only targets). This is the ONE resolve every render tool
        // uses since #810, and since #890 the only one there is — the
        // native-id twin that used to sit above it is gone.
        //
        // `outDepthFromFramebuffer` reports that the name resolved through a
        // depth-only framebuffer attachment, which is the one case where the
        // graph's own format label is absent and depth-ness has to be carried
        // out of the resolve.
        RHI::ResourceHandle ResolveTargetHandle(const std::string& name, bool& outDepthFromFramebuffer)
        {
            outDepthFromFramebuffer = false;

            RHI::ResourceHandle handle = Renderer3D::ResolveFrameGraphTextureHandle(name);
            if (handle.IsValid())
                return handle;

            const Ref<Framebuffer> framebuffer = Renderer3D::ResolveFrameGraphFramebuffer(name);
            if (!framebuffer)
                return {};

            handle = framebuffer->GetColorAttachmentHandle(0);
            if (handle.IsValid())
                return handle;

            handle = framebuffer->GetDepthAttachmentHandle();
            outDepthFromFramebuffer = handle.IsValid();
            return handle;
        }

        // How to READ one texel of a target, derived from the backend's own
        // description of its storage (#810). The destination format is chosen
        // so the readback works identically on both backends:
        //
        //  * depth  -> D32Float. GL needs a depth destination because only
        //    those map to GL_DEPTH_COMPONENT (reading a depth texture as
        //    GL_RED is GL_INVALID_OPERATION); Vulkan needs the same name so
        //    the decode path has a case for it (its identity path only fires
        //    when the image really is VK_FORMAT_D32_SFLOAT, and this hardware
        //    backs the graph's Depth24Stencil8 with D32_SFLOAT_S8_UINT).
        //  * 1-channel integer -> R32Int, so the R32I entity-id attachment
        //    comes back as the exact integer an agent compares against a
        //    scene entity id — never as a float.
        //  * everything else -> R32Float / RG32Float / RGBA32Float by channel
        //    count. A 3-channel source is read as 4 and only its first 3
        //    channels reported: the readback vocabulary has no 3-component
        //    float destination on both backends, and the reported values are
        //    identical either way.
        struct ProbeReadPlan
        {
            RHI::TextureFormatInfo Format;
            RHI::Format DestFormat = RHI::Format::RGBA32Float;
            i32 ReadChannels = 4;   ///< components the readback writes per texel
            i32 ReportChannels = 4; ///< components that mean something
            bool IsInteger = false;
        };

        bool PlanProbeRead(RHI::ResourceHandle handle, u32 mipLevel, ProbeReadPlan& out, std::string& outError)
        {
            RHI::TextureFormatInfo info;
            if (!RenderCommand::QueryTextureFormat(handle, mipLevel, info))
            {
                outError = "no storage at mip " + std::to_string(mipLevel) +
                           ", or a storage format this probe cannot decode";
                return false;
            }

            ProbeReadPlan plan;
            plan.Format = info;
            plan.IsInteger = info.IsInteger;
            plan.ReportChannels = info.Channels > 0 ? info.Channels : 4;

            if (info.IsDepth)
            {
                plan.DestFormat = RHI::Format::D32Float;
                plan.ReadChannels = 1;
                plan.ReportChannels = 1;
                plan.IsInteger = false;
            }
            else if (info.IsInteger)
            {
                if (info.Channels != 1)
                {
                    outError = std::string("multi-channel integer target (") + info.Token +
                               ") — the readback spine has no multi-component integer destination";
                    return false;
                }
                plan.DestFormat = RHI::Format::R32Int;
                plan.ReadChannels = 1;
            }
            else if (info.Channels <= 1)
            {
                plan.DestFormat = RHI::Format::R32Float;
                plan.ReadChannels = 1;
            }
            else if (info.Channels == 2)
            {
                plan.DestFormat = RHI::Format::RG32Float;
                plan.ReadChannels = 2;
            }
            else
            {
                plan.DestFormat = RHI::Format::RGBA32Float;
                plan.ReadChannels = 4;
            }

            // ReportChannels is seeded from the format's own channel count and
            // must never exceed what the destination format actually reads: the
            // compaction in ReadRectFloatsThroughFacade indexes
            // raw[t * ReadChannels + c] for c < ReportChannels, so a larger
            // report count walks off the end of the readback buffer. The two
            // only diverge for a format claiming 0 channels (no mapped format
            // does today), where the seed defaults to 4 while the <= 1 branch
            // reads one — an out-of-bounds read waiting for the first entry
            // that reports zero.
            plan.ReportChannels = std::min(plan.ReportChannels, plan.ReadChannels);

            out = plan;
            return true;
        }

        // The non-GL twin of GPUResourceInspector::CaptureTexturePng (#691
        // Phase 8b): resolves the target by RHI handle and reads through
        // RenderCommand::ReadTextureSubImage — the backend-neutral readback
        // spine — instead of raw glGetTextureSubImage. Lives here rather than
        // in GPUResourceInspector because Renderer/Debug is destined for
        // Platform/OpenGL (ADR 0011 §1.6); when that relocation lands, the
        // GL arm can fold into this shape and the id-based path retires.
        // `overrideHandle`, when valid, is an afterPass snapshot clone: read
        // THAT instead of resolving the live resource by name. The clone
        // carries the source's exact storage, so everything downstream —
        // format, extent, row order — is identical either way (#810).
        GPUResourceInspector::TextureCaptureResult CaptureTargetThroughFacade(
            const RenderGraph& graph, const std::string& name, u32 mipLevel, u32 faceOrLayer,
            GPUResourceInspector::CaptureNormalizeMode normalize, int maxWidth,
            GPUResourceInspector::CaptureRegion region,
            RHI::ResourceHandle overrideHandle = {})
        {
            GPUResourceInspector::TextureCaptureResult result;

            const auto* resource = graph.FindRegisteredResource(name);
            bool depthFromFramebuffer = false;
            RHI::ResourceHandle handle = overrideHandle;
            if (!handle.IsValid())
            {
                handle = ResolveTargetHandle(name, depthFromFramebuffer);
            }
            if (!handle.IsValid())
            {
                result.Error = "no GPU backing this frame";
                return result;
            }

            // Channels / depth-ness from the graph's registered format; a
            // handle that resolved outside the registry defaults to a 4-channel
            // float read (ReadTextureSubImage fails cleanly if it cannot).
            i32 channels = 4;
            bool isDepth = depthFromFramebuffer;
            std::string formatName = "Unknown";
            const RGResourceFormat rgFormat =
                resource != nullptr ? resource->Desc.Format : RGResourceFormat::Unknown;
            switch (rgFormat)
            {
                case RGResourceFormat::R8UNorm:
                case RGResourceFormat::R32Float:
                    channels = 1;
                    break;
                case RGResourceFormat::RG16Float:
                    channels = 2;
                    break;
                case RGResourceFormat::RGBA8UNorm:
                case RGResourceFormat::RGBA16Float:
                case RGResourceFormat::RGBA32Float:
                    channels = 4;
                    break;
                case RGResourceFormat::Depth24Stencil8:
                case RGResourceFormat::Depth32Float:
                    channels = 1;
                    isDepth = true;
                    break;
                case RGResourceFormat::R32Int:
                    result.Error = "integer targets are not image-capturable (same contract as the GL arm)";
                    return result;
                case RGResourceFormat::Unknown:
                    break;
            }
            if (rgFormat != RGResourceFormat::Unknown)
                formatName = std::string(RGFormatName(rgFormat));

            u32 fullWidth = 0;
            u32 fullHeight = 0;
            RenderCommand::GetTextureDimensions(handle, mipLevel, fullWidth, fullHeight);
            if (fullWidth == 0 || fullHeight == 0)
            {
                result.Error = "texture has no storage at the requested mip level";
                return result;
            }
            if (region.IsWholeTexture())
                region = GPUResourceInspector::CaptureRegion{ 0, 0, fullWidth, fullHeight };
            else if (region.X >= fullWidth || region.Y >= fullHeight ||
                     region.Width > fullWidth - region.X || region.Height > fullHeight - region.Y)
            {
                result.Error = "region (" + std::to_string(region.X) + ", " + std::to_string(region.Y) + ", " +
                               std::to_string(region.Width) + "x" + std::to_string(region.Height) +
                               ") exceeds mip " + std::to_string(mipLevel) + " (" + std::to_string(fullWidth) +
                               "x" + std::to_string(fullHeight) + ")";
                return result;
            }

            // ONE row order per backend (#691, ADR 0011 amendment
            // (85), retiring (79)'s per-target flipY knob): every off-screen
            // target is top-down under Vulkan and bottom-up under GL, so the
            // top-left-origin capture region converts iff the backend is GL —
            // the single predicate that replaced the per-call argument.
            const bool glRowOrder = RHI::RenderTargetRowsAreBottomUp();
            const u32 readY = glRowOrder ? fullHeight - region.Y - region.Height : region.Y;
            // A DEPTH source must name a DEPTH destination — on GL because only
            // those lower to GL_DEPTH_COMPONENT (asking for GL_RED on a depth
            // texture is GL_INVALID_OPERATION), on Vulkan because its identity
            // fast path only fires when the image really is D32_SFLOAT while
            // this hardware backs Depth24Stencil8 with D32_SFLOAT_S8_UINT.
            // Before #810 folded the capture fork this line was Vulkan-only, so
            // R32Float went unnoticed here; GL reaches it now.
            const RHI::Format destFormat = isDepth         ? RHI::Format::D32Float
                                           : channels == 1 ? RHI::Format::R32Float
                                           : channels == 2 ? RHI::Format::RG32Float
                                                           : RHI::Format::RGBA32Float;
            // Derived from destFormat, not `channels`, so buffer sizing, the
            // readback and the PNG channel count always agree — including the
            // depth-from-framebuffer fallback, where destFormat collapses to a
            // single channel while `channels` stayed at its 4-channel default.
            const i32 readChannels = destFormat == RHI::Format::R32Float ||
                                             destFormat == RHI::Format::D32Float
                                         ? 1
                                     : destFormat == RHI::Format::RG32Float ? 2
                                                                            : 4;
            const sizet valueCount =
                static_cast<sizet>(region.Width) * region.Height * static_cast<sizet>(readChannels);
            std::vector<f32> values(valueCount);
            if (!RenderCommand::ReadTextureSubImage(handle, mipLevel, static_cast<i32>(region.X),
                                                    static_cast<i32>(readY), static_cast<i32>(faceOrLayer),
                                                    region.Width, region.Height, 1u, destFormat,
                                                    values.size() * sizeof(f32), values.data()))
            {
                result.Error = "readback failed (format " + formatName + " — see the editor log)";
                return result;
            }

            // Min-max + 8-bit quantisation — the GL arm's float tail.
            const bool wantNormalize = normalize == GPUResourceInspector::CaptureNormalizeMode::On ||
                                       (normalize == GPUResourceInspector::CaptureNormalizeMode::Auto && isDepth);
            f32 minV = std::numeric_limits<f32>::max();
            f32 maxV = std::numeric_limits<f32>::lowest();
            for (const f32 v : values)
            {
                if (std::isfinite(v))
                {
                    minV = std::min(minV, v);
                    maxV = std::max(maxV, v);
                }
            }
            const bool haveRange = maxV > minV;
            if (haveRange)
            {
                result.MinValue = minV;
                result.MaxValue = maxV;
            }
            const bool doNormalize = wantNormalize && haveRange;
            result.Normalized = doNormalize;
            const f32 scale = doNormalize ? 1.0f / (maxV - minV) : 1.0f;
            const f32 bias = doNormalize ? -minV : 0.0f;
            std::vector<u8> pixels8(valueCount);
            for (sizet i = 0; i < valueCount; ++i)
            {
                const f32 safe = std::isnan(values[i]) ? 0.0f : values[i];
                pixels8[i] = static_cast<u8>(std::clamp((safe + bias) * scale, 0.0f, 1.0f) * 255.0f + 0.5f);
            }

            // Widen 2-channel to RGB (PNG comp=2 is grey+alpha), like the GL arm.
            i32 outChannels = readChannels;
            const sizet texelCount = static_cast<sizet>(region.Width) * region.Height;
            if (readChannels == 2)
            {
                outChannels = 3;
                std::vector<u8> widened(texelCount * 3u, 0u);
                for (sizet i = 0; i < texelCount; ++i)
                {
                    widened[i * 3 + 0] = pixels8[i * 2 + 0];
                    widened[i * 3 + 1] = pixels8[i * 2 + 1];
                }
                pixels8 = std::move(widened);
            }

            const sizet rowBytes = static_cast<sizet>(region.Width) * outChannels;
            std::vector<u8> flipped;
            if (glRowOrder)
            {
                flipped.resize(pixels8.size());
                for (sizet y = 0; y < region.Height; ++y)
                    std::memcpy(flipped.data() + y * rowBytes,
                                pixels8.data() + (static_cast<sizet>(region.Height) - 1 - y) * rowBytes,
                                rowBytes);
            }
            else
            {
                flipped = std::move(pixels8);
            }

            u32 outW = region.Width;
            u32 outH = region.Height;
            const std::vector<u8>* encodeSrc = &flipped;
            std::vector<u8> scaled;
            if (maxWidth > 0 && outW > static_cast<u32>(maxWidth))
            {
                const u32 srcW = outW;
                const u32 srcH = outH;
                outW = static_cast<u32>(maxWidth);
                outH = std::max<u32>(1, static_cast<u32>((static_cast<u64>(srcH) * outW) / srcW));
                scaled.assign(static_cast<sizet>(outW) * outH * outChannels, 0u);
                for (u32 y = 0; y < outH; ++y)
                {
                    const u32 sy = std::min(srcH - 1, static_cast<u32>((static_cast<u64>(y) * srcH) / outH));
                    for (u32 x = 0; x < outW; ++x)
                    {
                        const u32 sx = std::min(srcW - 1, static_cast<u32>((static_cast<u64>(x) * srcW) / outW));
                        std::memcpy(&scaled[(static_cast<sizet>(y) * outW + x) * outChannels],
                                    &flipped[(static_cast<sizet>(sy) * srcW + sx) * outChannels],
                                    static_cast<sizet>(outChannels));
                    }
                }
                encodeSrc = &scaled;
            }

            std::vector<u8> png;
            const auto appendToVector = [](void* context, void* data, int size)
            {
                auto* out = static_cast<std::vector<u8>*>(context);
                const auto* bytes = static_cast<const u8*>(data);
                out->insert(out->end(), bytes, bytes + size);
            };
            if (stbi_write_png_to_func(appendToVector, &png, static_cast<int>(outW), static_cast<int>(outH),
                                       outChannels, encodeSrc->data(), static_cast<int>(outW) * outChannels) == 0)
            {
                result.Error = "PNG encode failed";
                return result;
            }
            result.PngBytes = std::move(png);
            result.Width = outW;
            result.Height = outH;
            result.SourceWidth = fullWidth;
            result.SourceHeight = fullHeight;
            result.RegionX = region.X;
            result.RegionY = region.Y;
            result.RegionWidth = region.Width;
            result.RegionHeight = region.Height;
            result.FormatName = formatName;
            result.IsDepth = isDepth;
            return result;
        }

        // Wall-clock + frame stamp attached to every capture/probe response so a
        // STALE answer is detectable (issue #607). The motivating bug: after an
        // olo_scene_open, a capture came back byte-identical (same md5) to the
        // previous scene's — a silent wrong answer, because the render-graph
        // texture had not been redrawn yet and nothing in the response said so.
        // With the frame index in the meta, an agent can see two captures came
        // from the SAME frame; with `forceFrame` it can demand a fresh one.
        Json CaptureStampJson(u64 frameIndex, const EditorMcpContext& context)
        {
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            Json j;
            j["frameIndex"] = frameIndex;
            j["timestampMs"] = static_cast<u64>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
            // The frame index above tells you two captures came from the same frame,
            // but not WHY the frame stopped advancing. A parked editor (minimized ⇒
            // Application::Run skips the whole layer/render block) keeps answering
            // every read tool normally, so the liveness verdict has to travel with
            // the capture rather than being a separate question (issue #607).
            if (context.GetEditorLiveness)
            {
                const McpEditorLiveness liveness = context.GetEditorLiveness();
                j["stale"] = EditorLiveness::IsStale(liveness);
                j["liveness"] = EditorLiveness::ToJson(liveness);
            }
            return j;
        }

        // Render + settle `settleFrames` fresh frames before reading GPU state
        // back, so the answer describes the CURRENT scene/settings rather than
        // whatever was last drawn. Returns false on timeout (or MCP cancellation).
        bool ForceFreshFrame(McpServer& server, int settleFrames)
        {
            if (!server.Context().GetFrameIndex)
                return true; // older context: best effort
            const u64 baseFrame = server.MarshalRead([&server]() -> Json
                                                     { return Json{ { "frame", server.Context().GetFrameIndex() } }; })
                                      .value("frame", static_cast<u64>(0));
            return AwaitRenderedFrames(server, baseFrame, settleFrames);
        }

        // ---- olo_render_capture_target (main-marshaled; GL readback) -----------
        ToolResult Handle_RenderCaptureTarget(McpServer& server, const Json& args)
        {
            if (!args.contains("name") || !args["name"].is_string())
                return ToolResult::Error("Missing required argument 'name' (render-graph resource name; see olo_render_list_targets).");
            const std::string name = args["name"].get<std::string>();

            u32 mipLevel = 0;
            if (args.contains("mip") && args["mip"].is_number_integer())
                mipLevel = static_cast<u32>(std::clamp<long long>(args["mip"].get<long long>(), 0, 16));

            // 'layer' is the array-layer / cube-face selector; 'face' is the
            // original spelling and stays a pure alias. Both name the SAME
            // glGetTextureSubImage z offset, so giving two different values is a
            // contradiction, not a merge — reject it rather than pick one.
            const bool hasLayerArg = args.contains("layer") && args["layer"].is_number_integer();
            const bool hasFaceArg = args.contains("face") && args["face"].is_number_integer();
            long long requestedLayer = 0;
            if (hasLayerArg && hasFaceArg &&
                args["layer"].get<long long>() != args["face"].get<long long>())
                return ToolResult::Error("'layer' and 'face' are two names for the same array-layer / cube-face "
                                         "selector; give only one.");
            if (hasLayerArg)
                requestedLayer = args["layer"].get<long long>();
            else if (hasFaceArg)
                requestedLayer = args["face"].get<long long>();
            const bool hasLayerSelector = hasLayerArg || hasFaceArg;

            int maxWidth = 1024;
            if (args.contains("maxWidth") && args["maxWidth"].is_number_integer())
                maxWidth = static_cast<int>(std::clamp<long long>(args["maxWidth"].get<long long>(), 16, 4096));

            // Optional native-resolution sub-rect (issue #607). The maxWidth
            // downscale then applies to the REGION, so a region narrower than
            // maxWidth comes back 1:1 — the only way to measure a pixel-scale
            // artifact on a target wider than 4096. Omitted = whole mip, as before.
            McpCaptureRegion region;
            if (const auto regionError = CaptureRegionArg::Parse(args, region))
                return ToolResult::Error(*regionError);

            // Opt-in resource-link delivery (issue #673): publish the PNG
            // as an ephemeral olo://capture resource instead of inlining base64.
            const bool deliverLink = args.value("delivery", std::string{ "inline" }) == "resource_link";

            auto normalizeMode = GPUResourceInspector::CaptureNormalizeMode::Auto;
            if (args.contains("normalize") && args["normalize"].is_boolean())
                normalizeMode = args["normalize"].get<bool>() ? GPUResourceInspector::CaptureNormalizeMode::On
                                                              : GPUResourceInspector::CaptureNormalizeMode::Off;

            // afterPass (issue #607): snapshot the resource AS OF that pass's
            // execution and capture the snapshot clone — end-of-frame contents
            // can differ (ParticlePass re-exports SceneDepth after GTAOPass).
            std::string afterPass;
            if (args.contains("afterPass") && args["afterPass"].is_string())
                afterPass = args["afterPass"].get<std::string>();

            // Staleness (issue #607). A render-graph texture holds whatever was
            // last drawn into it: right after an olo_scene_open the new scene has
            // not been rendered yet, so a capture silently returns the PREVIOUS
            // scene's pixels — byte-identical, no error, no clue. 'forceFrame'
            // renders + settles fresh frames first; the meta always reports the
            // frame index the capture came from so the hazard is at least visible.
            // (afterPass always renders a fresh frame — the snapshot fires
            // during it — so forceFrame is implied there.)
            const bool forceFrame = args.value("forceFrame", false);
            bool freshFrameTimedOut = false;
            if (forceFrame && afterPass.empty())
                freshFrameTimedOut = !ForceFreshFrame(server, /*settleFrames*/ 2);

            bool afterPassFrameRendered = true;
            if (!afterPass.empty())
            {
                if (const std::string error = ArmAfterPassSnapshot(server, afterPass, { name }, afterPassFrameRendered);
                    !error.empty())
                    return ToolResult::Error(error);
            }

            Json result = server.MarshalRead([&server, name, mipLevel, hasLayerSelector, requestedLayer,
                                              normalizeMode, maxWidth, region, afterPass, afterPassFrameRendered,
                                              deliverLink]() -> Json
                                             {
                const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph();
                if (!graph)
                    return Json{ { "__error", "No active render graph (the editor is not in 3D mode, or no frame has been rendered yet)." } };

                RHI::ResourceHandle cloneHandle;
                RenderGraphPassSnapshot::Result snapshotResult;
                if (!afterPass.empty())
                {
                    if (const std::string error =
                            CollectAfterPassSnapshot(afterPass, name, afterPassFrameRendered, snapshotResult);
                        !error.empty())
                        return Json{ { "__error", error } };
                    cloneHandle = snapshotResult.Handle;
                }

                // Which GL layer to read. The default is NOT unconditionally 0: a
                // per-cascade layer view resolves to the whole parent array, so
                // capturing "ShadowMapCSMCascade3" without applying the view's own
                // layer would silently return cascade 0's pixels. (The snapshot
                // clone preserves every layer, so the same selection applies.)
                const CaptureLayer::TargetLayers layers = ResolveTargetLayers(*graph, name);
                const CaptureLayer::Selection selection =
                    CaptureLayer::SelectLayer(layers, name, hasLayerSelector, requestedLayer);
                if (!selection.Error.empty())
                    return Json{ { "__error", selection.Error } };

                // ONE capture path, one backend-neutral readback, live target
                // or mid-frame clone (#810). The Phase 8b fork existed because
                // Renderer/Debug still owned a GL readback; #801 relocated it
                // and the snapshot clone now carries an identity of its own, so
                // the id-based arm has nothing left to do anywhere.
                auto capture = CaptureTargetThroughFacade(
                    *graph, name, mipLevel, selection.Layer, normalizeMode, maxWidth,
                    GPUResourceInspector::CaptureRegion{ region.X, region.Y, region.Width, region.Height },
                    cloneHandle);
                if (!capture.Error.empty())
                    return Json{ { "__error", "Capture of '" + name + "' failed: " + capture.Error } };

                Json meta = CaptureStampJson(server.Context().GetFrameIndex ? server.Context().GetFrameIndex() : 0, server.Context());
                meta["name"] = name;
                if (!afterPass.empty())
                {
                    meta["afterPass"] = afterPass;
                    meta["snapshotSourceNativeHandle"] =
                        MCP::NativeHandleHex(snapshotResult.NativeSourceHandle);
                    meta["snapshotSourceIdentity"] =
                        MCP::IdentityToken(RHI::HashKey(snapshotResult.SourceHandle));
                    meta["frameIndexNote"] = "frameIndex is the collect-time frame; the snapshot was cloned "
                                             "mid-frame during the immediately preceding rendered frame.";
                }
                meta["layer"] = selection.Layer;
                if (CaptureLayer::IsArrayTarget(layers))
                    meta["layers"] = layers.LayerCount;
                if (!selection.Note.empty())
                    meta["layerNote"] = selection.Note;
                meta["width"] = capture.Width;
                meta["height"] = capture.Height;
                meta["sourceWidth"] = capture.SourceWidth;
                meta["sourceHeight"] = capture.SourceHeight;
                // Always echo the rect actually read plus whether the PNG is 1:1 —
                // a measurement that assumes native resolution must be able to
                // CHECK it, not infer it from maxWidth arithmetic (issue #607).
                meta["region"] = CaptureRegionArg::MetaJson(
                    McpCaptureRegion{ capture.RegionX, capture.RegionY, capture.RegionWidth, capture.RegionHeight },
                    capture.Width, capture.Height);
                meta["format"] = capture.FormatName;
                meta["isDepth"] = capture.IsDepth;
                meta["normalized"] = capture.Normalized;
                if (capture.MaxValue > capture.MinValue)
                {
                    meta["minValue"] = capture.MinValue;
                    meta["maxValue"] = capture.MaxValue;
                }
                Json out{ { "meta", std::move(meta) } };
                // Link mode hands the RAW bytes out (base64 happens lazily at
                // resources/read); inline keeps encoding here, unchanged.
                if (deliverLink)
                    out["png"] = Json::binary(std::move(capture.PngBytes));
                else
                    out["b64"] = Base64Encode(capture.PngBytes);
                return out; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());

            Json meta = result["meta"];
            meta["forcedFreshFrame"] = forceFrame;
            if (freshFrameTimedOut)
                meta["warning"] = "Timed out waiting for a fresh frame; this capture may be stale (compare 'frameIndex' "
                                  "against a previous call — an identical value means the same frame was read twice).";
            else if (!forceFrame)
                meta["note"] = "This is whatever was last rendered into the target. If the scene/settings changed "
                               "moments ago (e.g. after olo_scene_open), pass forceFrame:true to render and settle a "
                               "fresh frame first, or compare 'frameIndex' between calls.";

            ToolResult toolResult;
            if (deliverLink)
            {
                // Publish the capture as an ephemeral resource and hand back a
                // resource_link instead of inline base64 (issue #673).
                const Json::binary_t& png = result["png"].get_binary();
                std::vector<u8> bytes(png.begin(), png.end());
                Json linkBlock = PublishCaptureResourceLink(
                    server, std::move(bytes), "target",
                    "Render-target PNG capture of '" + name + "' (capture meta in the tool result).",
                    "Render-target capture of '" + name + "' (PNG); fetch via resources/read.", meta);
                toolResult.Content = Json::array(
                    { Json{ { "type", "text" }, { "text", meta.dump(2) } }, std::move(linkBlock) });
            }
            else
            {
                toolResult.Content = Json::array({ Json{ { "type", "text" }, { "text", meta.dump(2) } },
                                                   Json{ { "type", "image" },
                                                         { "data", result["b64"] },
                                                         { "mimeType", "image/png" } } });
            }
            // structuredContent must be a JSON object, so it mirrors the text meta
            // block only; the PNG stays an image content block / linked resource.
            toolResult.StructuredContent = std::move(meta);
            toolResult.IsError = false;
            return toolResult;
        }

        // ---- olo_render_toggle_pass / olo_render_set_debug_view (#316) ---
        // Ephemeral render-override A/B harness. Both tools mutate ONLY the
        // renderer's session-global settings (Renderer3D::GetPostProcessSettings()
        // / GetFogSettings()), never the loaded scene's own copy — so a change is
        // visible on the next rendered frame and a scene reload restores it. That
        // keeps the server read-only with respect to the project, the same boundary
        // the camera/viewport tools respect. All renderer state is main-thread-only,
        // so the work runs inside MarshalRead.

        // Canonical token for the active AO technique (reported by the toggle-pass
        // introspection so the agent can tell whether enabling SSAO vs GTAO will
        // actually apply — the two share PostProcessSettings::ActiveAOTechnique).
        const char* AOTechniqueToken(AOTechnique technique)
        {
            switch (technique)
            {
                case AOTechnique::None:
                    return "none";
                case AOTechnique::SSAO:
                    return "ssao";
                case AOTechnique::GTAO:
                    return "gtao";
            }
            return "unknown";
        }

        // Human-readable name of a rendering path, for the SSR/SSGI deferred-only
        // precondition note.
        const char* RenderingPathName(RenderingPath path)
        {
            switch (path)
            {
                case RenderingPath::Forward:
                    return "Forward";
                case RenderingPath::ForwardPlus:
                    return "Forward+";
                case RenderingPath::Deferred:
                    return "Deferred";
            }
            return "Unknown";
        }

        // Map a pass token to the single bool field it flips. PostProcess* fields
        // live on PostProcessSettings; Fog* on FogSettings. Returns nullptr only if
        // a new RenderOverrides::Pass enumerator is added without a mapping here
        // (the caller surfaces that as an internal error rather than crashing).
        bool* ResolvePassField(RenderOverrides::Pass pass, PostProcessSettings& pp, FogSettings& fog)
        {
            using RenderOverrides::Pass;
            switch (pass)
            {
                case Pass::Bloom:
                    return &pp.BloomEnabled;
                case Pass::SSAO:
                    return &pp.SSAOEnabled;
                case Pass::GTAO:
                    return &pp.GTAOEnabled;
                case Pass::SSR:
                    return &pp.SSREnabled;
                case Pass::SSGI:
                    return &pp.SSGIEnabled;
                case Pass::FXAA:
                    return &pp.FXAAEnabled;
                case Pass::TAA:
                    return &pp.TAAEnabled;
                case Pass::Vignette:
                    return &pp.VignetteEnabled;
                case Pass::ChromaticAberration:
                    return &pp.ChromaticAberrationEnabled;
                case Pass::DepthOfField:
                    return &pp.DOFEnabled;
                case Pass::MotionBlur:
                    return &pp.MotionBlurEnabled;
                case Pass::ColorGrading:
                    return &pp.ColorGradingEnabled;
                case Pass::AutoExposure:
                    return &pp.AutoExposureEnabled;
                case Pass::Fog:
                    return &fog.Enabled;
                case Pass::FogScattering:
                    return &fog.EnableScattering;
                case Pass::FogVolumetric:
                    return &fog.EnableVolumetric;
                case Pass::GodRays:
                    return &fog.EnableLightShafts;
            }
            return nullptr;
        }

        ToolResult Handle_RenderTogglePass(McpServer& server, const Json& args)
        {
            using namespace RenderOverrides;

            const bool hasName = args.contains("name") && args["name"].is_string() &&
                                 !args["name"].get<std::string>().empty();

            // Introspection: no name -> list every toggleable pass with its live
            // enabled state plus the active AO technique.
            if (!hasName)
            {
                const Json result = server.MarshalRead([]() -> Json
                                                       {
                    PostProcessSettings& pp = Renderer3D::GetPostProcessSettings();
                    FogSettings& fog = Renderer3D::GetFogSettings();
                    Json passes = DescribePasses();
                    for (auto& entry : passes)
                    {
                        Pass pass{};
                        if (ParsePass(entry.at("name").get<std::string>(), pass))
                        {
                            const bool* field = ResolvePassField(pass, pp, fog);
                            entry["enabled"] = (field != nullptr) && *field;
                        }
                    }
                    Json j;
                    j["passes"] = std::move(passes);
                    j["activeAOTechnique"] = AOTechniqueToken(pp.ActiveAOTechnique);
                    return j; });
                return ToolResult::Structured(result);
            }

            const std::string name = args["name"].get<std::string>();
            Pass pass{};
            if (!ParsePass(name, pass))
                return ToolResult::Error("Unknown pass '" + name + "'. Valid passes: " + JoinTokens(PassTokens()) +
                                         ". Call olo_render_toggle_pass with no arguments to list them with their current state.");

            // 'enabled' is optional: when given, set explicitly; when omitted, flip
            // the current value (the quick A/B form).
            const bool hasEnabled = args.contains("enabled") && args["enabled"].is_boolean();
            const bool desired = hasEnabled && args["enabled"].get<bool>();

            const Json result = server.MarshalRead([pass, hasEnabled, desired]() -> Json
                                                   {
                PostProcessSettings& pp = Renderer3D::GetPostProcessSettings();
                FogSettings& fog = Renderer3D::GetFogSettings();
                bool* field = ResolvePassField(pass, pp, fog);
                if (field == nullptr)
                    return Json{ { "__error", "Internal error: pass has no field mapping." } };

                ToggleResult r;
                r.Pass = PassToken(pass);
                r.Previous = *field;
                r.Enabled = hasEnabled ? desired : !*field;
                *field = r.Enabled;
                r.Changed = r.Enabled != r.Previous;

                // Side effects + preconditions, so a freshly enabled effect actually
                // appears (otherwise an agent A/Bs a toggle and sees no change).
                bool aoTechniqueChanged = false;
                if (r.Enabled)
                {
                    switch (pass)
                    {
                        case Pass::SSAO:
                            // SSAO and GTAO share ActiveAOTechnique; point it at SSAO
                            // so enabling SSAO is what renders.
                            if (pp.ActiveAOTechnique != AOTechnique::SSAO)
                            {
                                pp.ActiveAOTechnique = AOTechnique::SSAO;
                                aoTechniqueChanged = true;
                                r.Note = "Active AO technique set to SSAO so the effect is visible.";
                            }
                            break;
                        case Pass::GTAO:
                            if (pp.ActiveAOTechnique != AOTechnique::GTAO)
                            {
                                pp.ActiveAOTechnique = AOTechnique::GTAO;
                                aoTechniqueChanged = true;
                                r.Note = "Active AO technique set to GTAO so the effect is visible.";
                            }
                            break;
                        case Pass::SSR:
                        case Pass::SSGI:
                            if (Renderer3D::GetRendererSettings().Path != RenderingPath::Deferred)
                                r.Note = std::string(PassToken(pass)) +
                                         " renders only in the Deferred rendering path (current path: " +
                                         RenderingPathName(Renderer3D::GetRendererSettings().Path) + ").";
                            break;
                        case Pass::FogScattering:
                        case Pass::FogVolumetric:
                        case Pass::GodRays:
                            if (!fog.Enabled)
                                r.Note = "Fog is disabled; enable the 'fog' pass for this to take effect.";
                            break;
                        default:
                            break;
                    }
                }
                // An ActiveAOTechnique change swaps which AO pass is registered in
                // the render graph (RegisterSceneAndLightingNodes's switch), so it
                // must go through ApplyRendererSettings' dirty-check the same way
                // PostProcessSettingsPanel's technique combo box does (see
                // Renderer3DState.cpp's aoTechniqueChanged detection). Without this,
                // the previously-active technique's pass stays wired in, the newly
                // selected one's compute pass never runs, its AOBuffer is never
                // written (stays zero-initialized), and AOApplyRenderPass still
                // multiplies the WHOLE composited frame by that all-zero buffer at
                // intensity 1.0 -- an all-black frame indistinguishable from a
                // genuine rendering bug (issue #533's "essentially fully black"
                // symptom, when reproduced via this A/B toggle tool).
                if (aoTechniqueChanged)
                    Renderer3D::ApplyRendererSettings();
                return ToJson(r); });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Structured(result);
        }

        // The virtual-geometry debug mode a vg* debug-view token selects. Returns
        // false for every non-vg view (whose state lives on PostProcessSettings).
        bool VirtualModeForDebugView(RenderOverrides::DebugView view, VirtualDebugMode& out)
        {
            using RenderOverrides::DebugView;
            switch (view)
            {
                case DebugView::VGClusterId:
                    out = VirtualDebugMode::ClusterId;
                    return true;
                case DebugView::VGLod:
                    out = VirtualDebugMode::Lod;
                    return true;
                case DebugView::VGOverdraw:
                    out = VirtualDebugMode::Overdraw;
                    return true;
                default:
                    return false;
            }
        }

        // (main thread) Which debug view (if any) is currently on. Our tool keeps
        // these mutually exclusive, but the editor panel can set several; report the
        // first. The vg* modes live on the VirtualMeshRegistry, NOT on
        // PostProcessSettings, so they are read from there — that is what makes
        // `current` reflect a mode set through olo_virtual_geometry_set (and via the
        // Statistics panel) instead of reporting "none" while the visualisation is
        // plainly on screen.
        RenderOverrides::DebugView ActiveDebugView(const PostProcessSettings& pp)
        {
            using RenderOverrides::DebugView;
            if (pp.SSAODebugView)
                return DebugView::SSAO;
            if (pp.GTAODebugView)
                return DebugView::GTAO;
            if (pp.SSRDebugView)
                return DebugView::SSR;
            if (pp.SSGIDebugView)
                return DebugView::SSGI;
            if (pp.OverdrawDebugView)
                return DebugView::Overdraw;
            switch (VirtualMeshRegistry::Get().GetDebugMode())
            {
                case VirtualDebugMode::ClusterId:
                    return DebugView::VGClusterId;
                case VirtualDebugMode::Lod:
                    return DebugView::VGLod;
                case VirtualDebugMode::Overdraw:
                    return DebugView::VGOverdraw;
                case VirtualDebugMode::Off:
                    break;
            }
            return DebugView::None;
        }

        // Build the debug-view result for a (post-change or current) state: the four
        // flags from `pp`, the requested mode, and whether the backing pass is
        // actually producing the buffer this frame (with an actionable hint if not).
        RenderOverrides::DebugViewResult BuildDebugViewResult(const PostProcessSettings& pp, RenderOverrides::DebugView view)
        {
            using RenderOverrides::DebugView;
            RenderOverrides::DebugViewResult r;
            r.Mode = RenderOverrides::DebugViewToken(view);
            r.SSAODebugView = pp.SSAODebugView;
            r.GTAODebugView = pp.GTAODebugView;
            r.SSRDebugView = pp.SSRDebugView;
            r.SSGIDebugView = pp.SSGIDebugView;
            r.OverdrawDebugView = pp.OverdrawDebugView;
            const auto& virtualRegistry = VirtualMeshRegistry::Get();
            r.VirtualGeometryDebugMode = VirtualDebugModeToken(virtualRegistry.GetDebugMode());
            const bool deferred = Renderer3D::GetRendererSettings().Path == RenderingPath::Deferred;

            // The vg* modes render into their own target rather than the viewport,
            // so "the view is on" is only half the answer — an agent still has to
            // capture it, and the target only exists on the Deferred path with a
            // virtual mesh in view. Say both, instead of leaving a black capture to
            // be misread as a broken visualisation.
            if (RenderOverrides::IsVirtualGeometryView(view))
            {
                r.CaptureTarget = "VirtualGeometryDebug";
                r.PassEnabled = deferred && virtualRegistry.GetDebugColorTexture().IsValid();
                if (!deferred)
                    r.Note = "Virtual geometry renders on the DEFERRED path only (current path: " +
                             std::string(RenderingPathName(Renderer3D::GetRendererSettings().Path)) +
                             "). Switch with olo_renderer_settings_set { setting: 'renderpath', value: 'deferred' }.";
                else if (!r.PassEnabled)
                    r.Note = "The debug target is not backed yet — no VirtualMeshComponent was submitted this "
                             "frame. Check olo_virtual_geometry_stats.";
                else
                    r.Note = "Capture it with olo_render_capture_target { name: 'VirtualGeometryDebug' }.";
                return r;
            }

            switch (view)
            {
                case DebugView::None:
                    r.PassEnabled = true;
                    break;
                case DebugView::SSAO:
                    r.PassEnabled = pp.ActiveAOTechnique == AOTechnique::SSAO && pp.SSAOEnabled;
                    if (!r.PassEnabled)
                        r.Note = "SSAO is not active; enable it with olo_render_toggle_pass { name: 'ssao' }.";
                    break;
                case DebugView::GTAO:
                    r.PassEnabled = pp.ActiveAOTechnique == AOTechnique::GTAO && pp.GTAOEnabled;
                    if (!r.PassEnabled)
                        r.Note = "GTAO is not active; enable it with olo_render_toggle_pass { name: 'gtao' }.";
                    break;
                case DebugView::SSR:
                    r.PassEnabled = pp.SSREnabled && deferred;
                    if (!r.PassEnabled)
                        r.Note = "SSR is not active; enable it with olo_render_toggle_pass { name: 'ssr' } (Deferred path only).";
                    break;
                case DebugView::SSGI:
                    r.PassEnabled = pp.SSGIEnabled && deferred;
                    if (!r.PassEnabled)
                        r.Note = "SSGI is not active; enable it with olo_render_toggle_pass { name: 'ssgi' } (Deferred path only).";
                    break;
                case DebugView::Overdraw:
                    // Overdraw re-draws the opaque geometry itself into its own
                    // accumulation target — no backing effect pass to enable, and
                    // it works on every rendering path.
                    r.PassEnabled = true;
                    break;
                case DebugView::VGClusterId:
                case DebugView::VGLod:
                case DebugView::VGOverdraw:
                    // Already fully answered above (early return); listed so a new
                    // enumerator can never be silently dropped by this switch.
                    break;
            }
            return r;
        }

        ToolResult Handle_RenderSetDebugView(McpServer& server, const Json& args)
        {
            using namespace RenderOverrides;

            const bool hasMode = args.contains("mode") && args["mode"].is_string();
            // Accept enabled:false as an alias for mode:"none" (turn all views off).
            const bool disableViaEnabled = args.contains("enabled") && args["enabled"].is_boolean() &&
                                           !args["enabled"].get<bool>();

            // Introspection: no actionable argument -> list modes + current state.
            if (!hasMode && !disableViaEnabled)
            {
                const Json result = server.MarshalRead([]() -> Json
                                                       {
                    const PostProcessSettings& pp = Renderer3D::GetPostProcessSettings();
                    Json j;
                    j["modes"] = DescribeDebugViews();
                    j["current"] = ToJson(BuildDebugViewResult(pp, ActiveDebugView(pp)));
                    return j; });
                return ToolResult::Structured(result);
            }

            // enabled:false takes precedence over mode: it is the explicit
            // "clear all views" intent, so honour it even if a mode is also given
            // (leaving view at None) rather than letting the mode override it.
            DebugView view = DebugView::None;
            if (hasMode && !disableViaEnabled)
            {
                const std::string mode = args["mode"].get<std::string>();
                if (!ParseDebugView(mode, view))
                    return ToolResult::Error("Unknown debug view '" + mode + "'. Valid modes: " +
                                             JoinTokens(DebugViewModes()) + ".");
            }

            Json result = server.MarshalRead([view]() -> Json
                                             {
                PostProcessSettings& pp = Renderer3D::GetPostProcessSettings();
                // Exactly one debug view active at a time (or none) — including the
                // virtual-geometry ones, which is why selecting a non-vg mode also
                // turns the registry's mode OFF: two visualisations fighting over
                // the frame is never what was asked for, and it would leave the two
                // tools reporting different "current" states.
                pp.SSAODebugView = (view == DebugView::SSAO);
                pp.GTAODebugView = (view == DebugView::GTAO);
                pp.SSRDebugView = (view == DebugView::SSR);
                pp.SSGIDebugView = (view == DebugView::SSGI);
                pp.OverdrawDebugView = (view == DebugView::Overdraw);

                VirtualDebugMode virtualMode = VirtualDebugMode::Off;
                (void)VirtualModeForDebugView(view, virtualMode);
                const bool virtualChanged = VirtualMeshRegistry::Get().GetDebugMode() != virtualMode;
                // The SAME write path olo_virtual_geometry_set uses — never a
                // second copy of the logic.
                ApplyVirtualDebugMode(virtualMode);

                Json j = ToJson(BuildDebugViewResult(pp, view));
                j["__virtualChanged"] = virtualChanged;
                return j; });

            const bool virtualChanged = result.value("__virtualChanged", false);
            result.erase("__virtualChanged");

            // A virtual-geometry mode change gates a render-graph declaration, so the
            // topology must rebuild before the "VirtualGeometryDebug" target can be
            // captured. Settle here (exactly as olo_virtual_geometry_set does) and
            // re-read the state afterwards, so `passEnabled` describes the frame the
            // caller will actually capture rather than the one mid-rebuild.
            if (virtualChanged && server.Context().GetFrameIndex)
            {
                (void)ForceFreshFrame(server, kVirtualDebugSettleFrames);
                result = server.MarshalRead([view]() -> Json
                                            { return ToJson(BuildDebugViewResult(Renderer3D::GetPostProcessSettings(), view)); });
            }
            return ToolResult::Structured(result);
        }

        // ---- olo_scene_set_time_of_day / olo_scene_set_sun_angle (#633) --------
        // Both write the active scene's FIRST TimeOfDayComponent — the serialized,
        // single authoritative sun source (the ephemeral Renderer3D sun-direction
        // override these tools drove before issue #633 is retired).
        // TimeOfDaySystem::Apply recomputes the derived outputs and drives the
        // directional light + sky from the component on the next rendered frame in
        // BOTH edit and play modes, so a write here is visible immediately without
        // any extra apply step. The angle->time solver is the pure RenderOverrides
        // module; the handlers do the scene-bound lookup/write on the main thread
        // inside MarshalRead, like every other scene-touching tool.

        // Shared no-component guidance. Deliberately not auto-creating an entity:
        // the write tools edit what the scene AUTHORS, they never add to it (and
        // no MCP tool adds components — olo_entity_set_field edits existing ones).
        constexpr const char* kNoTimeOfDayComponent =
            "No TimeOfDayComponent in the active scene. Add a 'Time Of Day' component to an entity in the "
            "editor (Add Component > Time Of Day) and retry — no MCP tool adds components.";

        // (main thread) The scene's first TimeOfDayComponent, or nullptr. First
        // registry entity by convention — the same "one clock drives the scene"
        // rule TimeOfDaySystem applies. Takes a non-const Ref: Ref<T>
        // propagates const through operator->, and the write tools mutate the
        // component through this pointer.
        TimeOfDayComponent* FirstTimeOfDayComponent(Ref<Scene>& scene)
        {
            if (!scene)
                return nullptr;
            auto view = scene->GetAllEntitiesWith<TimeOfDayComponent>();
            if (view.begin() == view.end())
                return nullptr;
            return &view.get<TimeOfDayComponent>(*view.begin());
        }

        // (main thread) Shape the component's resulting state — the authored clock
        // fields plus the derived sun facts, computed through the SAME ephemeris
        // TimeOfDaySystem::Apply uses so the reported numbers cannot drift from
        // what the next frame renders.
        Json TimeOfDayStateJson(const TimeOfDayComponent& tod)
        {
            EphemerisInputs inputs;
            inputs.TimeOfDayHours = tod.m_TimeOfDayHours;
            inputs.DayOfYear = tod.m_DayOfYear;
            inputs.LatitudeDegrees = tod.m_LatitudeDegrees;
            inputs.NorthOffsetDegrees = tod.m_NorthOffsetDegrees;
            inputs.MoonPhase = tod.m_MoonPhase;
            const SunMoonState state = Ephemeris::ComputeSunMoon(inputs);

            Json j;
            j["enabled"] = tod.m_Enabled;
            j["hours"] = tod.m_TimeOfDayHours;
            j["dayOfYear"] = tod.m_DayOfYear;
            j["latitudeDegrees"] = tod.m_LatitudeDegrees;
            j["timeScale"] = tod.m_TimeScale;
            j["paused"] = tod.m_Paused;
            j["sunElevationDegrees"] = glm::degrees(state.SunElevationRadians);
            j["isNight"] = Ephemeris::NightBlend(state.SunElevationRadians) > 0.5f;
            j["sunDirection"] = Json::array({ state.SunDirection.x, state.SunDirection.y, state.SunDirection.z });
            j["moonDirection"] = Json::array({ state.MoonDirection.x, state.MoonDirection.y, state.MoonDirection.z });
            return j;
        }

        // (worker thread) Legacy 'clear':true from the retired override interface:
        // there is no override left to clear — the component is authoritative — so
        // answer with the current state + a note instead of erroring, shared by
        // both sun tools.
        ToolResult LegacySunClearResult(McpServer& server)
        {
            const Json result = server.MarshalRead([&server]() -> Json
                                                   {
                Json j;
                Ref<Scene> scene = server.Context().GetActiveScene
                                       ? server.Context().GetActiveScene()
                                       : nullptr;
                if (const TimeOfDayComponent* tod = FirstTimeOfDayComponent(scene))
                    j = TimeOfDayStateJson(*tod);
                j["note"] = "Nothing to clear: the ephemeral sun override is retired (issue #633) and the "
                            "scene's TimeOfDayComponent is authoritative. Set 'hours' (or the other fields) "
                            "to move the sun instead.";
                return j; });
            return ToolResult::Structured(result);
        }

        ToolResult Handle_SceneSetTimeOfDay(McpServer& server, const Json& args)
        {
            if (args.contains("clear") && args["clear"].is_boolean() && args["clear"].get<bool>())
                return LegacySunClearResult(server);

            const bool hasHours = args.contains("hours");
            const bool hasDay = args.contains("dayOfYear");
            const bool hasLatitude = args.contains("latitudeDegrees");
            const bool hasTimeScale = args.contains("timeScale");
            const bool hasPaused = args.contains("paused");
            const bool hasEnabled = args.contains("enabled");
            if (!hasHours && !hasDay && !hasLatitude && !hasTimeScale && !hasPaused && !hasEnabled)
                return ToolResult::Error("Nothing to set: give 'hours' (24-hour clock time) and/or "
                                         "'dayOfYear', 'latitudeDegrees', 'timeScale', 'paused', 'enabled'. "
                                         "To READ the current state use olo_scene_get_atmosphere.");

            f64 hours = 0.0;
            if (hasHours)
            {
                if (!args["hours"].is_number())
                    return ToolResult::Error("Invalid 'hours': expected a number.");
                hours = args["hours"].get<f64>();
                if (!std::isfinite(hours) || hours < 0.0 || hours > 24.0)
                    return ToolResult::Error("Invalid 'hours': expected a finite number in [0, 24) "
                                             "(0 = midnight, 6 = morning, 12 = noon, 18 = evening; 24 wraps "
                                             "to 0).");
                if (hours >= 24.0)
                    hours = 0.0; // the component's clock lives in [0, 24)
            }

            i32 dayOfYear = 0;
            if (hasDay)
            {
                if (!args["dayOfYear"].is_number_integer())
                    return ToolResult::Error("Invalid 'dayOfYear': expected an integer in [1, 365].");
                const auto day = args["dayOfYear"].get<long long>();
                if (day < 1 || day > 365)
                    return ToolResult::Error("Invalid 'dayOfYear': expected an integer in [1, 365].");
                dayOfYear = static_cast<i32>(day);
            }

            f64 latitude = 0.0;
            if (hasLatitude)
            {
                if (!args["latitudeDegrees"].is_number())
                    return ToolResult::Error("Invalid 'latitudeDegrees': expected a number.");
                latitude = args["latitudeDegrees"].get<f64>();
                if (!std::isfinite(latitude) || latitude < -90.0 || latitude > 90.0)
                    return ToolResult::Error("Invalid 'latitudeDegrees': expected a finite number in "
                                             "[-90, 90] (positive = northern hemisphere).");
            }

            f64 timeScale = 0.0;
            if (hasTimeScale)
            {
                if (!args["timeScale"].is_number())
                    return ToolResult::Error("Invalid 'timeScale': expected a number.");
                timeScale = args["timeScale"].get<f64>();
                if (!std::isfinite(timeScale) || timeScale < 0.0 || timeScale > 1000.0)
                    return ToolResult::Error("Invalid 'timeScale': expected a finite number in [0, 1000].");
            }

            if (hasPaused && !args["paused"].is_boolean())
                return ToolResult::Error("Invalid 'paused': expected a boolean.");
            if (hasEnabled && !args["enabled"].is_boolean())
                return ToolResult::Error("Invalid 'enabled': expected a boolean.");
            const bool paused = hasPaused && args["paused"].get<bool>();
            const bool enabled = hasEnabled && args["enabled"].get<bool>();

            const Json result = server.MarshalRead(
                [&server, hasHours, hours, hasDay, dayOfYear, hasLatitude, latitude, hasTimeScale, timeScale,
                 hasPaused, paused, hasEnabled, enabled]() -> Json
                {
                    Ref<Scene> scene = server.Context().GetActiveScene
                                           ? server.Context().GetActiveScene()
                                           : nullptr;
                    if (!scene)
                        return Json{ { "__error", "No active scene." } };
                    TimeOfDayComponent* tod = FirstTimeOfDayComponent(scene);
                    if (!tod)
                        return Json{ { "__error", kNoTimeOfDayComponent } };

                    if (hasHours)
                        tod->m_TimeOfDayHours = static_cast<f32>(hours);
                    if (hasDay)
                        tod->m_DayOfYear = dayOfYear;
                    if (hasLatitude)
                        tod->m_LatitudeDegrees = static_cast<f32>(latitude);
                    if (hasTimeScale)
                        tod->m_TimeScale = static_cast<f32>(timeScale);
                    if (hasPaused)
                        tod->m_Paused = paused;
                    if (hasEnabled)
                        tod->m_Enabled = enabled;

                    Json j = TimeOfDayStateJson(*tod);
                    if (!tod->m_Enabled)
                        j["note"] = "The TimeOfDayComponent is disabled, so TimeOfDaySystem will not drive "
                                    "the sun/sky from it until it is re-enabled ('enabled': true).";
                    return j;
                });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Structured(result);
        }

        ToolResult Handle_SceneSetSunAngle(McpServer& server, const Json& args)
        {
            if (args.contains("clear") && args["clear"].is_boolean() && args["clear"].get<bool>())
                return LegacySunClearResult(server);

            // A set needs BOTH angles — a half-specified direction is ambiguous, so
            // reject it with guidance rather than silently using a default.
            if (!args.contains("yaw") || !args["yaw"].is_number() ||
                !args.contains("pitch") || !args["pitch"].is_number())
                return ToolResult::Error("olo_scene_set_sun_angle needs both 'yaw' (azimuth, degrees) and "
                                         "'pitch' (elevation, degrees). To READ the current sun state use "
                                         "olo_scene_get_atmosphere.");

            const f64 yaw = args["yaw"].get<f64>();
            const f64 pitch = args["pitch"].get<f64>();
            if (!std::isfinite(yaw) || !std::isfinite(pitch))
                return ToolResult::Error("Invalid 'yaw'/'pitch': expected finite numbers in degrees.");
            if (pitch < -90.0 || pitch > 90.0)
                return ToolResult::Error("Invalid 'pitch': expected an elevation in [-90, 90] degrees "
                                         "(90 = straight up, 0 = horizon, negative = below the horizon).");

            const Json result = server.MarshalRead([&server, yaw, pitch]() -> Json
                                                   {
                Ref<Scene> scene = server.Context().GetActiveScene
                                       ? server.Context().GetActiveScene()
                                       : nullptr;
                if (!scene)
                    return Json{ { "__error", "No active scene." } };
                TimeOfDayComponent* tod = FirstTimeOfDayComponent(scene);
                if (!tod)
                    return Json{ { "__error", kNoTimeOfDayComponent } };

                // The solver works in the ephemeris frame (north = +Z at zero
                // offset); the requested azimuth is world-space, so undo the
                // component's authored north-offset yaw before solving.
                const RenderOverrides::SunAngleSolve solve = RenderOverrides::SolveTimeForSunAngle(
                    static_cast<f32>(pitch), static_cast<f32>(yaw) - tod->m_NorthOffsetDegrees,
                    tod->m_DayOfYear, tod->m_LatitudeDegrees);
                tod->m_TimeOfDayHours = solve.Hours;

                Json j = TimeOfDayStateJson(*tod);
                j["achievedElevationDeg"] = solve.AchievedElevationDeg;
                j["clamped"] = solve.Clamped;
                if (solve.Clamped)
                    j["note"] = "The requested elevation (" + std::to_string(pitch) + " deg) is outside what "
                                "day " + std::to_string(tod->m_DayOfYear) + " at latitude " +
                                std::to_string(tod->m_LatitudeDegrees) + " deg can reach; the time of day "
                                "was clamped to the closest achievable elevation (" +
                                std::to_string(solve.AchievedElevationDeg) + " deg). Change 'dayOfYear' / "
                                "'latitudeDegrees' via olo_scene_set_time_of_day for a higher (or lower) "
                                "sun.";
                else if (!tod->m_Enabled)
                    j["note"] = "The TimeOfDayComponent is disabled, so TimeOfDaySystem will not drive "
                                "the sun/sky from it until it is re-enabled (olo_scene_set_time_of_day "
                                "{ 'enabled': true }).";
                return j; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Structured(result);
        }

        // ---- olo_scene_set_weather / olo_scene_get_atmosphere (#633) -----------

        // WeatherStateId <-> name mapping (case-sensitive; the names mirror the
        // enumerators exactly — the same contract as the Lua weather bindings'
        // "targetState" / "currentState" string properties).
        constexpr std::array<std::pair<std::string_view, WeatherStateId>, 6> kWeatherStates = { {
            { "Clear", WeatherStateId::Clear },
            { "Overcast", WeatherStateId::Overcast },
            { "Rain", WeatherStateId::Rain },
            { "Storm", WeatherStateId::Storm },
            { "Snow", WeatherStateId::Snow },
            { "FogBank", WeatherStateId::FogBank },
        } };

        const char* WeatherStateName(WeatherStateId id)
        {
            for (const auto& [name, value] : kWeatherStates)
            {
                if (value == id)
                    return name.data();
            }
            return "Clear";
        }

        bool ParseWeatherState(std::string_view name, WeatherStateId& out)
        {
            for (const auto& [token, value] : kWeatherStates)
            {
                if (token == name)
                {
                    out = value;
                    return true;
                }
            }
            return false;
        }

        ToolResult Handle_SceneSetWeather(McpServer& server, const Json& args)
        {
            if (!args.contains("state") || !args["state"].is_string())
                return ToolResult::Error("Missing required argument 'state': one of Clear, Overcast, Rain, "
                                         "Storm, Snow, FogBank (case-sensitive).");
            const std::string stateName = args["state"].get<std::string>();
            WeatherStateId state{};
            if (!ParseWeatherState(stateName, state))
                return ToolResult::Error("Unknown weather state '" + stateName + "'. Valid states "
                                                                                 "(case-sensitive): Clear, Overcast, Rain, Storm, Snow, FogBank.");

            const bool hasTransition = args.contains("transitionSeconds");
            f64 transitionSeconds = 0.0;
            if (hasTransition)
            {
                if (!args["transitionSeconds"].is_number())
                    return ToolResult::Error("Invalid 'transitionSeconds': expected a number.");
                transitionSeconds = args["transitionSeconds"].get<f64>();
                if (!std::isfinite(transitionSeconds) || transitionSeconds < 0.0 || transitionSeconds > 600.0)
                    return ToolResult::Error("Invalid 'transitionSeconds': expected a finite number in "
                                             "[0, 600].");
            }
            const bool immediate = args.contains("immediate") && args["immediate"].is_boolean() &&
                                   args["immediate"].get<bool>();

            const Json result = server.MarshalRead(
                [&server, state, hasTransition, transitionSeconds, immediate]() -> Json
                {
                    Ref<Scene> scene = server.Context().GetActiveScene
                                           ? server.Context().GetActiveScene()
                                           : nullptr;
                    if (!scene)
                        return Json{ { "__error", "No active scene." } };
                    auto view = scene->GetAllEntitiesWith<WeatherStateComponent>();
                    if (view.begin() == view.end())
                        return Json{ { "__error",
                                       "No WeatherStateComponent in the active scene. Add a 'Weather Director' "
                                       "component to an entity in the editor (Add Component > Weather Director) "
                                       "and retry — no MCP tool adds components." } };
                    auto& weather = view.get<WeatherStateComponent>(*view.begin());

                    if (hasTransition)
                        weather.m_TransitionDuration = static_cast<f32>(transitionSeconds);
                    // Setting the target alone is enough: WeatherSystem's transition
                    // bookkeeping detects the edit (m_PrevTargetSeen) and starts the
                    // cross-blend from whatever is currently applied.
                    weather.m_TargetState = state;
                    if (immediate)
                    {
                        // Snap: settled on the target as if the transition already
                        // finished; m_BlendedValid = false makes UpdateTransition
                        // re-seed its bookkeeping from the new current state.
                        weather.m_CurrentState = state;
                        weather.m_TransitionProgress = 1.0f;
                        weather.m_PrevTargetSeen = state;
                        weather.m_BlendedValid = false;
                    }

                    // Apply the (re)started blend to the scene + renderer settings
                    // now, so edit mode reflects the change without a scheduler tick
                    // (the same call the editor inspector uses for previews).
                    WeatherSystem::ApplyImmediate(*scene);

                    Json j;
                    j["currentState"] = WeatherStateName(weather.m_CurrentState);
                    j["targetState"] = WeatherStateName(weather.m_TargetState);
                    j["transitionDuration"] = weather.m_TransitionDuration;
                    j["transitionProgress"] = weather.m_TransitionProgress;
                    j["wetness"] = weather.m_Wetness;
                    if (!weather.m_Enabled)
                        j["note"] = "The WeatherStateComponent is disabled, so the weather director ignores "
                                    "it until it is re-enabled (the editor inspector's Enabled checkbox, or "
                                    "olo_entity_set_field).";
                    return j;
                });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Structured(result);
        }

        // Read-only one-call report over the three atmosphere components (#633):
        // whatever exists is reported, absent blocks are omitted, and the note
        // says what was found — so an agent learns the scene's atmosphere setup
        // before reaching for the write tools.
        ToolResult Handle_SceneGetAtmosphere(McpServer& server, const Json&)
        {
            const Json result = server.MarshalRead([&server]() -> Json
                                                   {
                Ref<Scene> scene = server.Context().GetActiveScene
                                       ? server.Context().GetActiveScene()
                                       : nullptr;
                if (!scene)
                    return Json{ { "__error", "No active scene." } };

                Json j;
                std::vector<std::string> found;

                if (const TimeOfDayComponent* tod = FirstTimeOfDayComponent(scene))
                {
                    found.emplace_back("TimeOfDayComponent");
                    j["timeOfDay"] = TimeOfDayStateJson(*tod);
                }

                if (auto view = scene->GetAllEntitiesWith<WeatherStateComponent>();
                    view.begin() != view.end())
                {
                    found.emplace_back("WeatherStateComponent");
                    const auto& weather = view.get<WeatherStateComponent>(*view.begin());
                    Json w;
                    w["enabled"] = weather.m_Enabled;
                    w["currentState"] = WeatherStateName(weather.m_CurrentState);
                    w["targetState"] = WeatherStateName(weather.m_TargetState);
                    w["transitionDuration"] = weather.m_TransitionDuration;
                    w["transitionProgress"] = weather.m_TransitionProgress;
                    w["wetness"] = weather.m_Wetness;
                    w["blendedCloudCoverage"] = weather.m_Blended.CloudCoverage;
                    j["weather"] = w;
                }

                if (auto view = scene->GetAllEntitiesWith<CloudscapeComponent>();
                    view.begin() != view.end())
                {
                    found.emplace_back("CloudscapeComponent");
                    const auto& clouds = view.get<CloudscapeComponent>(*view.begin());
                    Json c;
                    c["enabled"] = clouds.m_Enabled;
                    c["coverage"] = clouds.m_Coverage;
                    c["layerBottom"] = clouds.m_LayerBottom;
                    c["layerTop"] = clouds.m_LayerTop;
                    c["castCloudShadows"] = clouds.m_CastCloudShadows;
                    j["cloudscape"] = c;
                }

                if (found.empty())
                    j["note"] = "No atmosphere components (TimeOfDayComponent / WeatherStateComponent / "
                                "CloudscapeComponent) in the active scene.";
                else
                    j["note"] = "Components found: " + RenderOverrides::JoinTokens(found) + ".";
                return j; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Structured(result);
        }

        // ---- olo_render_compare_golden (#316) ------------------------

        // Resolve a caller-supplied golden path to a safe, repo-relative location
        // under the visual-test artifact root. Rejects absolute paths and ".."
        // traversal so a server that is read-only w.r.t. the project can only ever
        // touch test artifacts (HANDOVER / #316 Tier-0 framing). On success `out`
        // is relative to the editor CWD (OloEditor/), so a bare "foo.png" lands in
        // assets/tests/visual/foo.png — the same root the suite visual tests use.
        std::string ResolveGoldenPath(const std::string& input, std::filesystem::path& out)
        {
            namespace fs = std::filesystem;
            if (input.empty())
                return "Invalid 'goldenPath': must not be empty.";
            const fs::path p = fs::path(input).lexically_normal();
            if (p.is_absolute() || p.has_root_name() || p.has_root_directory())
                return "Invalid 'goldenPath': must be a relative path (no drive letter or leading slash); "
                       "it is resolved under assets/tests/visual/.";
            for (const auto& part : p)
            {
                if (part == "..")
                    return "Invalid 'goldenPath': must not contain '..' (no directory traversal).";
            }
            const fs::path root = fs::path("assets") / "tests" / "visual";
            const fs::path rootNorm = root.lexically_normal();
            // Accept either a bare name/subpath (placed under the root) or a path
            // already rooted at assets/tests/visual.
            const auto mm = std::mismatch(rootNorm.begin(), rootNorm.end(), p.begin(), p.end());
            const bool alreadyUnderRoot = mm.first == rootNorm.end();
            out = (alreadyUnderRoot ? p : (root / p)).lexically_normal();
            // Force a .png extension so the format is unambiguous.
            if (const fs::path ext = out.extension(); ext != ".png" && ext != ".PNG")
                out += ".png";

            // Defence-in-depth against symlink escape: the lexical checks above
            // stop '..'/absolute paths, but a symlinked component inside the
            // artifact root (e.g. a symlinked assets/tests/visual) could still
            // redirect the write outside it. Resolve symlinks and confirm the
            // real path stays under assets/tests/visual/, honouring the "only
            // ever touches test artifacts" guarantee. weakly_canonical resolves
            // the existing prefix (catching a symlinked root) and handles the
            // not-yet-created golden file lexically.
            std::error_code ec;
            const fs::path canonicalRoot = fs::weakly_canonical(root, ec);
            if (ec)
                return "Could not resolve the golden artifact root (assets/tests/visual/).";
            const fs::path canonicalOut = fs::weakly_canonical(out, ec);
            if (ec)
                return "Could not resolve 'goldenPath' to a canonical location.";
            if (const fs::path rel = canonicalOut.lexically_relative(canonicalRoot); rel.empty() || *rel.begin() == "..")
                return "Invalid 'goldenPath': resolves outside assets/tests/visual/ (possible symlink escape).";
            return {};
        }

        // ---- olo_render_compare_golden (main-marshaled; GL readback + diff) -----
        // Capture the viewport (optionally from a fixed pose), diff it against a
        // golden PNG, and return a numeric similarity + pass/fail verdict — the
        // numeric half of CLAUDE.md's "rendering changes MUST be visually verified"
        // loop. When the golden is missing (or 'rebase' is set) the capture is
        // written as the new golden instead of failing, mirroring the test suite's
        // --olo-golden-rebase workflow. The diff math itself lives in the pure,
        // GL-free McpGoldenCompare.h so it is unit-tested headlessly and stays
        // consistent with the GoldenImageTests.cpp suite metric.
        ToolResult Handle_RenderCompareGolden(McpServer& server, const Json& args)
        {
            if (!args.contains("goldenPath") || !args["goldenPath"].is_string())
                return ToolResult::Error("Missing required argument 'goldenPath' (PNG path under assets/tests/visual/).");
            std::filesystem::path goldenPath;
            if (const std::string err = ResolveGoldenPath(args["goldenPath"].get<std::string>(), goldenPath); !err.empty())
                return ToolResult::Error(err);

            if (!server.Context().CaptureViewportPng)
                return ToolResult::Error("Screenshot capture is not available in this editor build.");

            // Optional camera placement for this capture only (same shape as olo_screenshot).
            const bool hasCamera = args.contains("camera");
            const bool hasOrbit = args.contains("orbit");
            if (hasCamera && hasOrbit)
                return ToolResult::Error("Give either 'camera' or 'orbit', not both.");
            CameraRequest request;
            if (hasCamera || hasOrbit)
            {
                if (!CameraContextAvailable(server.Context()))
                    return ToolResult::Error("Camera control is not available in this editor build.");
                const std::string error = hasCamera ? ParsePoseRequest(args["camera"], request)
                                                    : ParseOrbitRequest(args["orbit"], request);
                if (!error.empty())
                    return ToolResult::Error(error);
            }

            // Optional explicit similarity threshold in [0, 1]. Absent => the
            // suite-cascade verdict (RMSE -> SSIM) from GoldenImageTests.cpp.
            std::optional<f32> threshold;
            if (args.contains("threshold"))
            {
                if (!args["threshold"].is_number())
                    return ToolResult::Error("Invalid 'threshold': expected a number in [0, 1].");
                const f32 t = args.value("threshold", -1.0f);
                if (!std::isfinite(t) || t < 0.0f || t > 1.0f)
                    return ToolResult::Error("Invalid 'threshold': expected a finite number in [0, 1].");
                threshold = t;
            }
            const bool rebase = args.value("rebase", false);

            int settleFrames = 2;
            if (args.contains("settleFrames") && args["settleFrames"].is_number_integer())
                settleFrames = static_cast<int>(std::clamp<long long>(args["settleFrames"].get<long long>(), 1, 30));
            int maxWidth = 1024;
            if (args.contains("maxWidth") && args["maxWidth"].is_number_integer())
                maxWidth = static_cast<int>(std::clamp<long long>(args["maxWidth"].get<long long>(), 16, 4096));

            // Opt-in resource-link delivery for the returned capture (issue #673
            // Tier 1); the golden FILE write below is unaffected either way.
            const bool deliverLink = args.value("delivery", std::string{ "inline" }) == "resource_link";

            // Save the user's pose and apply the requested one (identical machinery
            // to Handle_Screenshot — the restore happens in the capture job / the
            // error path so it runs exactly once).
            bool posed = false;
            Json savedPose;
            bool waitTimedOut = false;
            if (hasCamera || hasOrbit)
            {
                const Json applied = server.MarshalRead([&server, request]() -> Json
                                                        {
                    const McpCameraPose prior = server.Context().GetCameraPose();
                    ApplyCameraRequest(server.Context(), request);
                    Json j;
                    j["focalPoint"] = Json::array({ prior.FocalPoint.x, prior.FocalPoint.y, prior.FocalPoint.z });
                    j["distance"] = prior.Distance;
                    j["yaw"] = prior.YawRadians;
                    j["pitch"] = prior.PitchRadians;
                    j["fov"] = prior.FovDegrees;
                    j["frame"] = server.Context().GetFrameIndex ? server.Context().GetFrameIndex() : 0;
                    return j; });
                savedPose = applied;
                posed = true;
            }
            const auto restorePriorPose = [&server, &savedPose]()
            {
                McpCameraPose prior;
                prior.FocalPoint = glm::vec3{ savedPose["focalPoint"][0].get<f32>(),
                                              savedPose["focalPoint"][1].get<f32>(),
                                              savedPose["focalPoint"][2].get<f32>() };
                prior.Distance = savedPose.value("distance", 0.0f);
                prior.YawRadians = savedPose.value("yaw", 0.0f);
                prior.PitchRadians = savedPose.value("pitch", 0.0f);
                prior.FovDegrees = savedPose.value("fov", 45.0f);
                server.Context().RestoreCameraPose(prior);
            };

            // Capture the viewport PNG (and restore the user's camera in the same
            // main-thread job). The bytes are written into `capturedPng` by
            // reference — MarshalRead runs synchronously, so this is safe.
            std::vector<u8> capturedPng;
            try
            {
                if (posed)
                    waitTimedOut = !AwaitRenderedFrames(server, savedPose.value("frame", static_cast<u64>(0)), settleFrames);
                const Json cap = server.MarshalRead([&server, maxWidth, posed, &restorePriorPose, &capturedPng]() -> Json
                                                    {
                    // Whole viewport, never a region: a golden was recorded whole,
                    // so comparing a sub-rect against it would be meaningless.
                    capturedPng = server.Context().CaptureViewportPng(maxWidth, McpCaptureRegion{});
                    if (posed)
                        restorePriorPose();
                    if (capturedPng.empty())
                        return Json{ { "__error", "Viewport capture failed (no framebuffer or empty viewport)." } };
                    return Json{ { "ok", true } }; });
                if (cap.is_object() && cap.contains("__error"))
                    return ToolResult::Error(cap["__error"].get<std::string>());
            }
            catch (...)
            {
                if (posed)
                {
                    try
                    {
                        (void)server.MarshalRead([&restorePriorPose]() -> Json
                                                 {
                            restorePriorPose();
                            return Json{}; });
                    }
                    catch (...)
                    {
                    }
                }
                throw;
            }

            namespace fs = std::filesystem;
            const std::string goldenPathStr = goldenPath.generic_string();
            const bool goldenExists = fs::exists(goldenPath);

            // The second content block for either result shape: an inline base64
            // image, or a published olo://capture resource + resource_link block
            // (which also stamps resourceUri into the verdict object BEFORE it
            // is mirrored into the text block).
            // attachCapture runs exactly once per handler execution (the rebase
            // branch and the compare branch are mutually exclusive and each is the
            // last use of capturedPng), so the link branch may MOVE the buffer.
            const auto attachCapture = [&server, &capturedPng, deliverLink](Json& verdict) -> Json
            {
                if (!deliverLink)
                    return Json{ { "type", "image" },
                                 { "data", Base64Encode(capturedPng) },
                                 { "mimeType", "image/png" } };
                return PublishCaptureResourceLink(
                    server, std::move(capturedPng), "golden-capture",
                    "Viewport capture from olo_render_compare_golden (verdict in the tool result).",
                    "Captured viewport frame (PNG); fetch via resources/read.", verdict);
            };

            // Golden-missing / rebase: write the capture as the new golden and
            // report it (a test-artifact write under assets/tests/visual/, never
            // the user's scene/assets — see #316 Tier-0 framing).
            if (!goldenExists || rebase)
            {
                std::error_code ec;
                if (goldenPath.has_parent_path())
                    fs::create_directories(goldenPath.parent_path(), ec);
                std::ofstream f(goldenPath, std::ios::binary | std::ios::trunc);
                if (f)
                    f.write(reinterpret_cast<const char*>(capturedPng.data()), static_cast<std::streamsize>(capturedPng.size()));
                if (!f)
                    return ToolResult::Error("Failed to write golden PNG to: " + goldenPathStr);

                Json j;
                j["goldenPath"] = goldenPathStr;
                j["created"] = true;
                j["rebased"] = goldenExists; // existed AND we overwrote it
                j["bytes"] = static_cast<u64>(capturedPng.size());
                j["message"] = (goldenExists ? "Rebased golden at " : "Golden created at ") + goldenPathStr +
                               " — captured the current frame as the new baseline. Re-run the tool (without 'rebase', "
                               "same capture size) to compare against it.";
                if (waitTimedOut)
                    j["warning"] = "Timed out waiting for the new camera pose to render; the golden may be a stale frame.";

                ToolResult result;
                const Json captureBlock = attachCapture(j);
                result.Content = Json::array({ Json{ { "type", "text" }, { "text", j.dump(2) } }, captureBlock });
                // The verdict JSON also goes out as structuredContent; the image
                // stays a content block / linked resource (structuredContent
                // cannot carry images).
                result.StructuredContent = std::move(j);
                result.IsError = false;
                return result;
            }

            // Compare path: decode both PNGs to RGBA8 and run the pure diff core.
            // stb's flip flags can be left set by production asset-loading paths
            // (Model.cpp / AssetSerializer.cpp set the thread-local one), which
            // would load an image upside-down and produce a false mismatch — reset
            // both global and thread-local flags so decode orientation is
            // deterministic and matches how goldens are written (no flip). Same
            // precaution as GoldenImageTests.cpp::CompareOrBootstrap.
            ::stbi_set_flip_vertically_on_load(0);
            ::stbi_set_flip_vertically_on_load_thread(0);

            int aw = 0, ah = 0, ach = 0;
            stbi_uc* actualRaw = ::stbi_load_from_memory(capturedPng.data(), static_cast<int>(capturedPng.size()), &aw, &ah, &ach, 4);
            if (actualRaw == nullptr)
                return ToolResult::Error("Failed to decode the captured frame PNG in memory.");
            std::vector<u8> actual(actualRaw, actualRaw + (static_cast<std::size_t>(aw) * ah * 4));
            ::stbi_image_free(actualRaw);

            int gw = 0, gh = 0, gch = 0;
            stbi_uc* goldenRaw = ::stbi_load(goldenPathStr.c_str(), &gw, &gh, &gch, 4);
            if (goldenRaw == nullptr)
            {
                const char* reason = ::stbi_failure_reason();
                return ToolResult::Error("Failed to read/decode the golden PNG at " + goldenPathStr + ": " +
                                         (reason ? reason : "unknown error"));
            }
            std::vector<u8> golden(goldenRaw, goldenRaw + (static_cast<std::size_t>(gw) * gh * 4));
            ::stbi_image_free(goldenRaw);

            const GoldenCompare::CompareResult cmp =
                GoldenCompare::Compare(actual, static_cast<u32>(aw), static_cast<u32>(ah),
                                       golden, static_cast<u32>(gw), static_cast<u32>(gh), threshold);

            Json j;
            j["goldenPath"] = goldenPathStr;
            j["created"] = false;
            j["pass"] = cmp.Pass;
            j["dimensionsMatch"] = cmp.DimensionsMatch;
            j["actual"] = Json{ { "width", cmp.ActualWidth }, { "height", cmp.ActualHeight } };
            j["golden"] = Json{ { "width", cmp.GoldenWidth }, { "height", cmp.GoldenHeight } };
            if (cmp.DimensionsMatch)
            {
                j["similarity"] = cmp.Similarity;
                j["ssim"] = cmp.Ssim;
                j["rmse"] = cmp.Rmse;
                j["mse"] = cmp.Mse;
                j["threshold"] = cmp.Threshold;
                j["thresholdMode"] = cmp.ThresholdMode;
                j["mismatchPixels"] = cmp.MismatchPixels;
                j["totalPixels"] = cmp.TotalPixels;
                j["maxChannelDelta"] = cmp.MaxChannelDelta;
                j["worstPixel"] = Json{ { "x", cmp.WorstX }, { "y", cmp.WorstY } };
            }
            j["message"] = cmp.Message;
            if (waitTimedOut)
                j["warning"] = "Timed out waiting for the new camera pose to render; the comparison may use a stale frame.";

            ToolResult result;
            const Json captureBlock = attachCapture(j);
            result.Content = Json::array({ Json{ { "type", "text" }, { "text", j.dump(2) } }, captureBlock });
            // The verdict JSON also goes out as structuredContent; the image
            // stays a content block / linked resource (structuredContent cannot
            // carry images).
            result.StructuredContent = std::move(j);
            result.IsError = false;
            return result;
        }

        // ---- olo_renderer_settings_set (main-marshaled; PROJECT WRITE) ---------
        // Set a multi-valued, session-global renderer / post-process setting — the
        // FSR1 spatial-upscale mode, the tone-map operator, or the rendering path —
        // so an agent can verify a rendering feature LIVE at each setting over MCP
        // (the motivating case is #480's FSR1 "Spatial Upscale" dropdown, which the
        // read-only server couldn't drive). It is the ENUM-valued sibling of the
        // boolean olo_render_toggle_pass. Gated at dispatch by the "Allow writes"
        // session toggle (ToolDef::ProjectWrite): a settings write crosses the
        // read-only line, though it is session-scoped and restorable, never a
        // project mutation. Restore is restore-PRIOR-VALUE, NOT CommandHistory —
        // these are global renderer settings, not scene/ECS data, so the tool
        // reports `previousValue` and the agent reverts by setting it back (a scene
        // reload also restores them). The shared schema + parse + apply core lives
        // in McpRendererSettings.h so it is unit-tested at the dispatch seam without
        // this TU; the render-path switch's render-graph rebuild
        // (Renderer3D::ApplyRendererSettings) stays here, on the main thread.
        ToolResult Handle_RendererSettingsSet(McpServer& server, const Json& args)
        {
            using namespace RendererSettings;

            bool introspect = false;
            Setting setting{};
            i32 value = 0;
            if (const auto error = ParseArgs(args, introspect, setting, value))
                return ToolResult::Error(*error);

            // Snapshot the live renderer toggles the perf-lever settings read/write
            // (#316). Main-thread only — always called inside a MarshalRead.
            const auto snapshotLever = []() -> LeverState
            {
                LeverState lever;
                lever.DepthPrepassEnabled = Renderer3D::IsDepthPrepassEnabled();
                lever.DepthPrepassAuto = Renderer3D::ComputeSettingsDerivedDepthPrepass();
                lever.DepthAwareCulling = Renderer3D::IsDepthAwareClusterCullingEnabled();
                lever.SoftShadows = Renderer3D::GetShadowMap().GetSettings().SoftShadows;
                lever.HZBOcclusion = Renderer3D::IsHZBOcclusionCullingEnabled();
                // The EFFECTIVE value, not the requested one: VirtualShadowMap::Init
                // clears its own Enabled flag when it cannot come up, so a request
                // that fell back to CSM must not read back as 'on' (issue #702).
                lever.VirtualShadowMaps = Renderer3D::GetShadowMap().IsVirtualShadowMapActive();
                lever.VSMDebugMode = Renderer3D::GetShadowMap().GetSettings().VSM.DebugMode;
                // The REQUEST, unlike VirtualShadowMaps above. The technique
                // is a per-light decision made inside the frame, so there is
                // no single effective bool to report: a scene can have one
                // ray-traced light and three that fell back. Reporting the
                // request keeps the lever able to say what it was set to, and
                // the per-light truth is the fallback counters (issue #1056).
                lever.RayTracedShadows =
                    Renderer3D::GetShadowMap().GetSettings().Technique == ShadowTechnique::RayTraced;
                lever.RayTracedShadowSoftness = RayTracedSoftnessPreset(
                    Renderer3D::GetShadowMap().GetSettings().RayTraced.LightAngularRadiusDegrees);
                return lever;
            };

            // Introspection: no `setting` -> list every setting with its live current
            // value and the allowed-value catalogue.
            if (introspect)
            {
                const Json result = server.MarshalRead([&snapshotLever]() -> Json
                                                       { return Describe(Renderer3D::GetPostProcessSettings(), Renderer3D::GetRendererSettings(), snapshotLever()); });
                return ToolResult::Structured(result);
            }

            const Json result = server.MarshalRead([setting, value, &snapshotLever]() -> Json
                                                   {
                PostProcessSettings& pp = Renderer3D::GetPostProcessSettings();
                // Fully qualified: `using namespace RendererSettings` is in scope, so
                // unqualified `RendererSettings` would name that MCP namespace, not the
                // engine struct.
                ::OloEngine::RendererSettings& rs = Renderer3D::GetRendererSettings();
                LeverState lever = snapshotLever();
                ApplyResult applied = Apply(setting, value, pp, rs, lever);
                if (!applied.Ok)
                    return Json{ { "__error", applied.Error } };
                // A render-path switch changes the registered pass list, so the
                // render-graph topology must be rebuilt for the new value to render.
                if (applied.RequiresRendererApply)
                    Renderer3D::ApplyRendererSettings();
                if (setting == Setting::MSAA)
                {
                    const std::string effective =
                        ValueToken(setting, static_cast<i32>(rs.Deferred.MSAASampleCount));
                    applied.Data["changed"] = applied.Data.value("previousValue", std::string{}) != effective;
                    applied.Data["value"] = effective;
                    if (effective != ValueToken(setting, value))
                        applied.Data["note"] = "requested MSAA sample count exceeded the driver cap; reporting the effective value";
                }
                // Push mutated lever fields back to the renderer — Apply only wrote
                // the POD snapshot (the header stays renderer-free).
                if (setting == Setting::DepthPrepass)
                {
                    Renderer3D::EnableDepthPrepass(lever.DepthPrepassEnabled);
                }
                else if (setting == Setting::DepthAwareCulling)
                {
                    Renderer3D::EnableDepthAwareClusterCulling(lever.DepthAwareCulling);
                }
                else if (setting == Setting::SoftShadows)
                {
                    ShadowSettings shadow = Renderer3D::GetShadowMap().GetSettings();
                    shadow.SoftShadows = lever.SoftShadows;
                    Renderer3D::GetShadowMap().SetSettings(shadow);
                }
                else if (setting == Setting::VirtualShadowMaps)
                {
                    // SetSettings recreates the whole VSM resource set on a change
                    // and mirrors the effective flag back, so a failed enable leaves
                    // the CSM path running rather than an unshadowed frame.
                    ShadowSettings shadow = Renderer3D::GetShadowMap().GetSettings();
                    shadow.VSM.Enabled = lever.VirtualShadowMaps;
                    Renderer3D::GetShadowMap().SetSettings(shadow);

                    // Report the EFFECTIVE state, not the requested one. Apply
                    // builds the reply BEFORE this branch runs, and
                    // VirtualShadowMap::Init clears its own Enabled flag when a
                    // shader fails to load — so a refused enable runs CSM while
                    // the pre-built reply would still say "on". An agent that
                    // trusts that reply then verifies frames that were never
                    // VSM's, which is precisely the class of lie this tool
                    // surface must not tell.
                    const bool effective = Renderer3D::GetShadowMap().IsVirtualShadowMapActive();
                    if (effective != lever.VirtualShadowMaps)
                    {
                        const std::string effectiveToken = effective ? "on" : "off";
                        applied.Data["changed"] =
                            applied.Data.value("previousValue", std::string{}) != effectiveToken;
                        applied.Data["value"] = effectiveToken;
                        applied.Data["note"] = "virtual shadow maps refused to initialise (see "
                                               "OloEngine.log); reporting the effective state";
                    }
                }
                else if (setting == Setting::RayTracedShadowSoftness)
                {
                    ShadowSettings shadow = Renderer3D::GetShadowMap().GetSettings();
                    shadow.RayTraced.LightAngularRadiusDegrees =
                        RayTracedSoftnessDegrees(lever.RayTracedShadowSoftness);
                    Renderer3D::GetShadowMap().SetSettings(shadow);
                    applied.Data["angularRadiusDegrees"] = shadow.RayTraced.LightAngularRadiusDegrees;
                }
                else if (setting == Setting::VSMDebug)
                {
                    ShadowSettings shadow = Renderer3D::GetShadowMap().GetSettings();
                    shadow.VSM.DebugMode = lever.VSMDebugMode;
                    Renderer3D::GetShadowMap().SetSettings(shadow);
                }
                else if (setting == Setting::RayTracedShadows)
                {
                    ShadowSettings shadow = Renderer3D::GetShadowMap().GetSettings();
                    shadow.Technique = lever.RayTracedShadows ? ShadowTechnique::RayTraced
                                                              : ShadowTechnique::ShadowMap;
                    Renderer3D::GetShadowMap().SetSettings(shadow);

                    // No effective-value correction here, deliberately, and it is
                    // the opposite call from VirtualShadowMaps just above. VSM has
                    // ONE flag that Init can refuse, so reporting the request would
                    // be a lie. The shadow technique has no such flag: it is decided
                    // per light, inside the frame, after this call returns — a scene
                    // can end up with one ray-traced light and three fallbacks, and
                    // there is no single bool that describes that honestly. So the
                    // lever reports the request and points at the thing that does.
                    if (const RayTracedShadowPass* pass = Renderer3D::GetRayTracedShadowPass();
                        pass != nullptr && lever.RayTracedShadows)
                    {
                        const auto& stats = pass->GetStats();
                        applied.Data["rayTracedLights"] = stats.RayTracedLights;
                        applied.Data["fallbackLights"] = stats.FallbackLights;
                        if (stats.FallbackLights > 0)
                        {
                            applied.Data["fallbackReason"] =
                                std::string(ToString(stats.DominantFallbackReason()));
                        }
                        // One frame stale by construction: these are last frame's
                        // numbers, because this write lands before the frame that
                        // acts on it. Say so rather than letting a caller read a
                        // zero as "it did not work".
                        applied.Data["note"] = "counters are from the PREVIOUS frame; re-read after a frame "
                                               "has rendered with the new setting";
                    }
                }
                else if (setting == Setting::HZBOcclusion)
                {
                    // Turning it OFF also invalidates the retained pyramid, so a
                    // later re-enable starts cleanly from that frame's depth — the
                    // first frame after an 'on' is therefore frustum-only.
                    Renderer3D::EnableHZBOcclusionCulling(lever.HZBOcclusion);
                }
                else
                {
                    // The remaining settings (upscale / tonemap / renderpath) live
                    // entirely in the settings structs Apply already wrote.
                }
                if (applied.RequiresRenderGraphRebuild)
                    Renderer3D::RequestRenderGraphRebuild();
                return applied.Data; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());

            // Settle before returning (#519 "first perf-lever write right after
            // scene load doesn't take effect on the GPU"). A lever flip right
            // after a heavy scene load lands while the editor's render-budget
            // throttle is still skipping frames — the just-blocked main thread
            // reports a huge timestep for the next OnUpdate, which trips
            // skipRender for a beat. While throttled, Renderer3D::BeginScene
            // never runs, so RendererProfiler::BeginFrame/EndFrame don't either:
            // GetLastCompletedFrameData() (what olo_perf_snapshot reads) stays
            // frozen on whatever rendered before the write, making the change
            // invisible until an unrelated later frame finally renders. Waiting
            // out the same throttle/resize transient the screenshot tools
            // already respect (AwaitRenderedFrames/IsCaptureUnready) guarantees
            // at least a couple of real frames have executed with the new
            // setting by the time this call returns, so an immediately
            // following olo_perf_snapshot reads live data instead of a stale
            // pre-change frame.
            constexpr int kSettingsSettleFrames = 2;
            if (server.Context().GetFrameIndex)
            {
                const u64 baseFrame = server.MarshalRead([&server]() -> Json
                                                         { return Json{ { "frame", server.Context().GetFrameIndex() } }; })
                                          .value("frame", static_cast<u64>(0));
                AwaitRenderedFrames(server, baseFrame, kSettingsSettleFrames);
            }

            return ToolResult::Structured(result);
        }
        // ---- olo_postprocess_settings_get / _set (main-marshaled) --------------
        // The post-process / AO / fog block of the renderer, which
        // olo_renderer_settings_set never reached (issue #607). Read and write are
        // SEPARATE tools on purpose: the write is gated behind "Allow writes", and
        // an agent without that consent must still be able to read the parameters
        // in play — during the GTAO hunt they had to be read off a screenshot of
        // the Post Processing panel. The shared field table + coercion core lives
        // in McpPostProcessSettings.h so it is unit-tested without this TU; the one
        // renderer-bound side effect (an ActiveAOTechnique switch re-registering
        // the AO pass, issue #533) stays here, on the main thread.

        // (main thread) Both live settings PODs, as a pair, so the field table's
        // accessors can reach either without knowing which owns a given field.
        Json DescribePostProcess(std::string_view group, bool& unknownGroup)
        {
            return PostProcess::Describe(Renderer3D::GetPostProcessSettings(), Renderer3D::GetFogSettings(),
                                         group, unknownGroup);
        }

        ToolResult Handle_PostProcessSettingsGet(McpServer& server, const Json& args)
        {
            if (args.contains("field") && !args["field"].is_null())
            {
                if (!args["field"].is_string())
                    return ToolResult::Error("Invalid 'field': expected a string.");
                const std::string token = args["field"].get<std::string>();
                const PostProcess::FieldInfo* field = PostProcess::FindField(token);
                if (field == nullptr)
                {
                    std::string message = "Unknown post-process field '" + token + "'.";
                    if (const std::vector<std::string> suggestions = PostProcess::SuggestFields(token);
                        !suggestions.empty())
                    {
                        message += " Did you mean: ";
                        for (sizet i = 0; i < suggestions.size(); ++i)
                            message += (i == 0 ? "" : ", ") + suggestions[i];
                        message += "?";
                    }
                    message += " Call with no arguments to list every field; groups: " + PostProcess::JoinGroupTokens() + ".";
                    return ToolResult::Error(message);
                }
                const Json result = server.MarshalRead([field]() -> Json
                                                       { return PostProcess::DescribeField(*field, Renderer3D::GetPostProcessSettings(),
                                                                                           Renderer3D::GetFogSettings()); });
                return ToolResult::Structured(result);
            }

            std::string group;
            if (args.contains("group") && args["group"].is_string())
                group = args["group"].get<std::string>();

            bool unknownGroup = false;
            const Json result = server.MarshalRead([&group, &unknownGroup]() -> Json
                                                   { return DescribePostProcess(group, unknownGroup); });
            if (unknownGroup)
                return ToolResult::Error("Unknown group '" + group + "'. Valid groups: " + PostProcess::JoinGroupTokens() + ".");
            return ToolResult::Structured(result);
        }

        ToolResult Handle_PostProcessSettingsSet(McpServer& server, const Json& args)
        {
            bool introspect = false;
            const PostProcess::FieldInfo* field = nullptr;
            Json value;
            if (const auto error = PostProcess::ParseSetArgs(args, introspect, field, value))
                return ToolResult::Error(*error);

            if (introspect)
            {
                bool unknownGroup = false;
                const Json result = server.MarshalRead([&unknownGroup]() -> Json
                                                       { return DescribePostProcess({}, unknownGroup); });
                return ToolResult::Structured(result);
            }

            const Json result = server.MarshalRead([field, &value]() -> Json
                                                   {
                const PostProcess::ApplyResult applied =
                    PostProcess::Apply(*field, value, Renderer3D::GetPostProcessSettings(), Renderer3D::GetFogSettings());
                if (!applied.Ok)
                    return Json{ { "__error", applied.Error } };
                // ActiveAOTechnique swaps which AO pass is registered in the render
                // graph, so the topology must be rebuilt for the new value to render
                // at all (issue #533) — the same reason a renderpath switch does it.
                if (applied.RequiresRendererApply)
                    Renderer3D::ApplyRendererSettings();
                if (applied.RequiresRenderGraphRebuild)
                    Renderer3D::RequestRenderGraphRebuild();
                return applied.Data; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());

            // Settle before returning, exactly as olo_renderer_settings_set does
            // (#519): a write landing while the editor's render-budget throttle is
            // skipping frames stays invisible to an immediately following
            // screenshot / perf snapshot until some later frame renders.
            constexpr int kSettingsSettleFrames = 2;
            if (server.Context().GetFrameIndex)
            {
                const u64 baseFrame = server.MarshalRead([&server]() -> Json
                                                         { return Json{ { "frame", server.Context().GetFrameIndex() } }; })
                                          .value("frame", static_cast<u64>(0));
                AwaitRenderedFrames(server, baseFrame, kSettingsSettleFrames);
            }
            return ToolResult::Structured(result);
        }

        // Stable JSON token for a resource kind. Written out rather than derived
        // from the enum so a reordered/renamed enumerator is a compile error here
        // instead of a silently changed wire value.
        const char* ResourceKindName(RGResourceHandle::Kind kind)
        {
            switch (kind)
            {
                case RGResourceHandle::Kind::Texture2D:
                    return "texture2d";
                case RGResourceHandle::Kind::Texture2DArray:
                    return "texture2darray";
                case RGResourceHandle::Kind::Texture3D:
                    return "texture3d";
                case RGResourceHandle::Kind::TextureCube:
                    return "texturecube";
                case RGResourceHandle::Kind::TextureCubeArray:
                    return "texturecubearray";
                case RGResourceHandle::Kind::Framebuffer:
                    return "framebuffer";
                case RGResourceHandle::Kind::UniformBuffer:
                    return "uniformbuffer";
                case RGResourceHandle::Kind::StorageBuffer:
                    return "storagebuffer";
                case RGResourceHandle::Kind::Unknown:
                    break;
            }
            return "unknown";
        }

        // ---- olo_render_transient_plan (main-marshaled) ------------------------
        // The render graph's transient PLAN and the pool state behind it (issue
        // #607). olo_render_graph_topology_export shows per-pass resolved ids for
        // one frame, but nothing exposed the layer where the aliasing decisions are
        // actually made: which alias group/slot a resource landed in, whether it
        // allocated at all (and why not), its FirstPass->LastPass lifetime, what it
        // is a version-alias OF, and which pool bucket handed out which object in
        // what order. Root-causing the one-frame black-square artifact needed
        // exactly this and had to be obtained by rebuilding the engine with
        // hand-rolled instrumentation.
        ToolResult Handle_RenderTransientPlan(McpServer& server, const Json& args)
        {
            const bool includePool = args.value("includePool", true);
            const bool includeAcquireOrder = args.value("includeAcquireOrder", true);
            std::string filter;
            if (args.contains("resource") && args["resource"].is_string())
                filter = args["resource"].get<std::string>();

            const Json result = server.MarshalRead([includePool, includeAcquireOrder, filter]() -> Json
                                                   {
                const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph();
                if (!graph)
                    return Json{ { "__error", "No active render graph (the editor is not in 3D mode, or no frame has been rendered yet)." } };

                const auto& versionAliases = graph->GetVersionAliasTargets();
                const RenderGraph::TransientDebugFlags flags = RenderGraph::GetTransientDebugFlags();

                Json entries = Json::array();
                for (const auto& entry : graph->GetTransientPlan())
                {
                    if (!filter.empty() && entry.Resource.find(filter) == std::string::npos)
                        continue;

                    Json j{
                        { "resource", entry.Resource },
                        { "kind", ResourceKindName(entry.Kind) },
                        { "reachable", entry.Reachable },
                        { "willAllocate", entry.WillAllocate },
                        { "aliasGroup", entry.AliasGroup },
                        { "estimatedBytes", entry.EstimatedBytes },
                        { "firstPass", entry.FirstPass },
                        { "lastPass", entry.LastPass },
                    };
                    if (entry.AliasSlot != std::numeric_limits<u32>::max())
                        j["aliasSlot"] = entry.AliasSlot;
                    if (entry.FirstPassIndex != std::numeric_limits<u32>::max())
                        j["firstPassIndex"] = entry.FirstPassIndex;
                    j["lastPassIndex"] = entry.LastPassIndex;
                    if (!entry.SkipReason.empty())
                        j["skipReason"] = entry.SkipReason;
                    // A "version-alias" skip is meaningless without its target: the
                    // versioned name is a RENAME of the source's physical, and a
                    // version whose physical differs from its base's is the
                    // orphan-allocation bug this whole diagnostic exists to catch.
                    if (const auto aliasIt = versionAliases.find(entry.Resource); aliasIt != versionAliases.end())
                        j["versionAliasOf"] = aliasIt->second;
                    // The resolved physical backing, so "did these two plan
                    // entries get the same GPU object" is one lookup rather
                    // than an inference from alias group + slot. Compare
                    // `identity` for that — it is the currency that answers it
                    // on both backends (issue #890); `nativeTexture` is the
                    // hex a RenderDoc capture shows and is display only.
                    {
                        bool depthFromFramebuffer = false;
                        const RHI::ResourceHandle texture =
                            ResolveTargetHandle(entry.Resource, depthFromFramebuffer);
                        if (const u64 native = Debug::NativeHandleForDiagnostics(texture); native != 0)
                            j["nativeTexture"] = MCP::NativeHandleHex(native);
                        if (const std::string token = MCP::IdentityToken(RHI::HashKey(texture)); !token.empty())
                            j["identity"] = token;
                    }
                    // Poison hue is reported unconditionally — knowing which colour
                    // a resource WOULD leak as is what lets you plan the hunt before
                    // turning poison mode on.
                    j["poisonColor"] = std::string(RenderGraph::PoisonColorNameForResource(entry.Resource));
                    entries.push_back(std::move(j));
                }

                Json out{
                    { "entries", std::move(entries) },
                    { "planSize", graph->GetTransientPlan().size() },
                    { "topologyGeneration", graph->GetTopologyGeneration() },
                    { "debugFlags", Json{ { "poisonTransients", flags.PoisonTransients },
                                          { "disableAliasing", flags.DisableAliasing } } },
                };
                if (!filter.empty())
                    out["resourceFilter"] = filter;

                if (includePool)
                {
                    // GetActiveGraph() hands back a CONST Ref, and Ref<T> propagates
                    // constness through operator-> — so reach the pool through an
                    // own (non-const) Ref rather than const_cast'ing the graph.
                    Ref<RenderGraph> mutableGraph = graph;
                    TransientPool& pool = mutableGraph->GetTransientPool();
                    const auto stats = pool.GetStats();
                    const auto aliasReport = pool.ComputeAliasReport();

                    Json buckets = Json::array();
                    for (const auto& bucket : pool.GetBucketReport())
                    {
                        Json b{ { "kind", bucket.Kind }, { "key", bucket.Key }, { "pooledCount", bucket.PooledCount } };
                        if (bucket.Kind == "texture")
                        {
                            b["width"] = bucket.Width;
                            b["height"] = bucket.Height;
                            b["format"] = bucket.Format;
                            b["mipLevels"] = bucket.MipLevels;
                            b["samples"] = bucket.Samples;
                        }
                        else if (bucket.Kind == "buffer")
                        {
                            b["sizeBytes"] = bucket.SizeBytes;
                        }
                        buckets.push_back(std::move(b));
                    }

                    out["pool"] = Json{
                        { "texturePoolSize", stats.TexturePoolSize },
                        { "textureBuckets", stats.TextureAliasGroups },
                        { "framebufferPoolSize", stats.FramebufferPoolSize },
                        { "framebufferBuckets", stats.FramebufferAliasGroups },
                        { "bufferPoolSize", stats.BufferPoolSize },
                        { "bufferBuckets", stats.BufferAliasGroups },
                        { "estimatedBytes", pool.EstimateMemoryUsage() },
                        { "totalAcquiredBytes", aliasReport.TotalAcquiredBytes },
                        { "potentialAliasingBytes", aliasReport.PotentialAliasingBytes },
                        { "buckets", std::move(buckets) },
                    };

                    if (includeAcquireOrder)
                    {
                        Json order = Json::array();
                        bool liveFrame = false;
                        for (const auto& acquired : pool.GetAcquireOrder(&liveFrame))
                        {
                            Json a{ { "kind", acquired.Kind },
                                    { "nativeHandle", MCP::NativeHandleHex(
                                                          Debug::NativeHandleForDiagnostics(acquired.Handle)) },
                                    { "identity", MCP::IdentityToken(RHI::HashKey(acquired.Handle)) },
                                    { "identityIndex", acquired.Handle.Index },
                                    { "identityGeneration", acquired.Handle.Generation } };
                            if (acquired.Kind == "buffer")
                                a["sizeBytes"] = acquired.SizeBytes;
                            else
                            {
                                a["width"] = acquired.Width;
                                a["height"] = acquired.Height;
                            }
                            order.push_back(std::move(a));
                        }
                        out["pool"]["acquireOrder"] = std::move(order);
                        // Which frame the order describes. An MCP read marshals at
                        // a frame boundary, i.e. AFTER ReleaseAll() emptied the
                        // live lists, so the honest answer is normally the last
                        // COMPLETED frame — say so rather than let a reader take
                        // it for the frame they are about to capture.
                        out["pool"]["acquireOrderIsLiveFrame"] = liveFrame;
                        out["pool"]["acquireOrderNote"] =
                            std::string("Acquisition order, not sorted — the order the alias-slot assigner consumed "
                                        "the pool. A LIFO pool's reuse pattern is only readable in this order; two "
                                        "entries sharing an identity shared one GPU object. Compare 'identity', not "
                                        "'nativeHandle' - the native handle is display only and is legitimately 0 on "
                                        "Vulkan, where every entry would then appear to share one object. ") +
                            (liveFrame ? "This is the IN-PROGRESS frame."
                                       : "This is the last COMPLETED frame (ReleaseAll() returns every object to the "
                                         "pool at end of frame, so a between-frames read has no live acquisitions).");
                    }
                }
                return out; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Structured(result);
        }

        // ---- olo_render_debug_set (main-marshaled; session WRITE) ---------------
        // Flip the two permanent transient-corruption instruments live instead of
        // via env var + editor restart (issue #607). Poison mode in particular
        // turned a ~3% stochastic camera-move artifact into a deterministic
        // every-frame signal; as a toggle against a running editor that diagnosis
        // takes seconds, and it doubles as the fix-verification probe.
        ToolResult Handle_RenderDebugSet(McpServer& server, const Json& args)
        {
            const RenderGraph::TransientDebugFlags before = RenderGraph::GetTransientDebugFlags();
            RenderGraph::TransientDebugFlags wanted = before;

            bool anyRequested = false;
            if (args.contains("poisonTransients") && !args["poisonTransients"].is_null())
            {
                if (!args["poisonTransients"].is_boolean())
                    return ToolResult::Error("Invalid 'poisonTransients': expected a boolean.");
                wanted.PoisonTransients = args["poisonTransients"].get<bool>();
                anyRequested = true;
            }
            if (args.contains("disableAliasing") && !args["disableAliasing"].is_null())
            {
                if (!args["disableAliasing"].is_boolean())
                    return ToolResult::Error("Invalid 'disableAliasing': expected a boolean.");
                wanted.DisableAliasing = args["disableAliasing"].get<bool>();
                anyRequested = true;
            }

            const bool aliasingChanged = wanted.DisableAliasing != before.DisableAliasing;
            const Json result = server.MarshalRead([wanted, before, anyRequested, aliasingChanged]() -> Json
                                                   {
                if (anyRequested)
                {
                    RenderGraph::SetTransientDebugFlags(wanted);
                    // Pooled objects acquired under the previous aliasing policy are
                    // still bucketed and would be handed straight back out under the
                    // new one, so an A/B would compare a mixed state. Evict.
                    if (aliasingChanged)
                    {
                        // Own (non-const) Ref: Ref<T> propagates constness through
                        // operator->, and GetActiveGraph() returns a const Ref.
                        if (Ref<RenderGraph> graph = RenderGraphDebugRuntime::GetActiveGraph(); graph)
                            graph->GetTransientPool().Clear();
                    }
                }

                Json out{
                    { "poisonTransients", wanted.PoisonTransients },
                    { "disableAliasing", wanted.DisableAliasing },
                    { "previous", Json{ { "poisonTransients", before.PoisonTransients },
                                        { "disableAliasing", before.DisableAliasing } } },
                    { "changed", anyRequested && (wanted.PoisonTransients != before.PoisonTransients || aliasingChanged) },
                    { "restoreWith", Json{ { "poisonTransients", before.PoisonTransients },
                                           { "disableAliasing", before.DisableAliasing } } },
                };

                // The resource->hue map, up front. The engine logs it one line per
                // resource as each is first materialized, which is useless if you
                // want to read a poisoned screenshot immediately.
                if (wanted.PoisonTransients)
                {
                    Json map = Json::array();
                    if (const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph(); graph)
                    {
                        for (const auto& entry : graph->GetTransientPlan())
                        {
                            map.push_back(Json{ { "resource", entry.Resource },
                                                { "color", std::string(RenderGraph::PoisonColorNameForResource(entry.Resource)) } });
                        }
                    }
                    out["poisonColorMap"] = std::move(map);
                    out["poisonNote"] =
                        "Every pool-acquired transient is cleared to its hue at materialize time from the NEXT "
                        "rendered frame. Any texel reaching the screen in one of these colours was never written "
                        "this frame, and the colour names the resource it leaked from. Capture with "
                        "olo_screenshot { forceFrame: true }.";
                }
                return out; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());

            // The flags only take effect at the next MaterializeTransientResources,
            // so settle a couple of frames — otherwise the screenshot an agent takes
            // straight after enabling poison shows the pre-poison frame and "proves"
            // the instrument does nothing.
            if (anyRequested && server.Context().GetFrameIndex)
            {
                const u64 baseFrame = server.MarshalRead([&server]() -> Json
                                                         { return Json{ { "frame", server.Context().GetFrameIndex() } }; })
                                          .value("frame", static_cast<u64>(0));
                AwaitRenderedFrames(server, baseFrame, 2);
            }
            return ToolResult::Structured(result);
        }

        // ---- olo_render_why_not_visible (main-marshaled) -----------------------
        // The rendering counterpart of olo_physics_why_no_collision: explain why an
        // entity isn't on screen. Gathers the render-relevant facts off the live
        // scene/renderer, then runs the pure ExplainWhyNotVisible cascade. The
        // per-frame occlusion (HZB) and LOD state are NOT queryable from here, so
        // the tool reports them honestly as not-observable rather than guessing.
        ToolResult Handle_RenderWhyNotVisible(McpServer& server, const Json& args)
        {
            if (!args.contains("entity"))
                return ToolResult::Error("Missing required argument 'entity' (entity UUID).");
            u64 id = 0;
            if (!ParseUuid(args["entity"], id))
                return ToolResult::Error("Invalid 'entity': expected a UUID as a string or number.");

            const auto gather = [&server, id]() -> Json
            {
                Json j;
                const Ref<Scene> scene = server.Context().GetActiveScene
                                             ? server.Context().GetActiveScene()
                                             : nullptr;

                RenderExplain::WhyNotVisibleInput in;
                in.SceneLoaded = scene != nullptr;

                // Global shader-compile hint: a broken shared mesh shader can hide
                // an otherwise correctly-configured object, but isn't attributable
                // to one entity — surfaced as a warning, not a per-entity verdict.
                int shaderErrorCount = 0;
                const auto& allShaders = ShaderDebugger::GetInstance().GetAllShaders();
                for (const auto& [sid, info] : allShaders)
                {
                    if (info.m_HasErrors || !info.m_LastCompilation.m_Success)
                        ++shaderErrorCount;
                }
                in.ShaderErrorCount = shaderErrorCount;
                in.AnyShaderHasErrors = shaderErrorCount > 0;

                RenderExplain::EntityRenderFacts& f = in.Entity;

                if (scene)
                {
                    const auto entityOpt = scene->TryGetEntityWithUUID(UUID(id));
                    f.EntityExists = entityOpt.has_value();
                    if (f.EntityExists)
                    {
                        Entity entity = *entityOpt;

                        // World-space bounding sphere (from the entity's local
                        // transform — the exact transform the renderer submits;
                        // there is no world-transform flattening in that path).
                        // Every scene entity carries a TransformComponent, but guard
                        // defensively so a transform-less entity can't crash the read.
                        const bool hasTransform = entity.HasComponent<TransformComponent>();
                        const glm::mat4 modelMatrix = hasTransform
                                                          ? entity.GetComponent<TransformComponent>().GetTransform()
                                                          : glm::mat4(1.0f);
                        BoundingSphere worldSphere;
                        const auto setBoundsFromLocal = [&](const BoundingSphere& localSphere)
                        {
                            worldSphere = localSphere.Transform(modelMatrix);
                            f.BoundsKnown = true;
                        };

                        // Resolve the entity material's OWN shader (custom-shader /
                        // shader-graph materials). Standard meshes render through a
                        // shared deferred PBR shader, so most materials carry no
                        // m_Shader and this stays unresolved (the global hint covers
                        // a broken shared shader).
                        const auto resolveMaterialShader = [&](const Material& mat)
                        {
                            const Ref<Shader>& shader = mat.GetShader();
                            if (!shader)
                                return;
                            f.HasMaterialShader = true;
                            f.MaterialShaderName = shader->GetName();
                            for (const auto& [sid, info] : allShaders)
                            {
                                if (info.m_Name == f.MaterialShaderName)
                                {
                                    f.MaterialShaderHasErrors = info.m_HasErrors || !info.m_LastCompilation.m_Success;
                                    break;
                                }
                            }
                        };

                        // Pick the primary renderable, most-specific first. Detailed
                        // geometry/visibility checks for the verified set; the rest
                        // are detected as renderable (kind label only) so a niche
                        // renderable is never mis-reported as "not renderable".
                        if (entity.HasComponent<MeshComponent>())
                        {
                            f.HasRenderable = true;
                            f.RenderableKind = "MeshComponent";
                            const auto& mc = entity.GetComponent<MeshComponent>();
                            f.GeometryRequired = true;
                            f.GeometryPresent = static_cast<bool>(mc.m_MeshSource);
                            if (!f.GeometryPresent)
                                f.GeometryDetail = "the MeshComponent's MeshSource is null";
                            else
                                setBoundsFromLocal(mc.m_MeshSource->GetBoundingSphere());
                            if (entity.HasComponent<MaterialComponent>())
                                resolveMaterialShader(entity.GetComponent<MaterialComponent>().m_Material);
                        }
                        else if (entity.HasComponent<ModelComponent>())
                        {
                            f.HasRenderable = true;
                            f.RenderableKind = "ModelComponent";
                            const auto& model = entity.GetComponent<ModelComponent>();
                            f.GeometryRequired = true;
                            f.GeometryPresent = model.IsLoaded();
                            if (!f.GeometryPresent)
                                f.GeometryDetail = (model.m_Model == nullptr)
                                                       ? "the ModelComponent's model is null"
                                                       : "the model has no meshes loaded";
                            else
                                setBoundsFromLocal(model.m_Model->GetBoundingSphere());
                            f.HasVisibilityFlag = true;
                            f.VisibilityFlagOn = model.m_Visible;
                            f.VisibilityFlagName = "ModelComponent.m_Visible";
                            if (entity.HasComponent<MaterialComponent>())
                                resolveMaterialShader(entity.GetComponent<MaterialComponent>().m_Material);
                        }
                        else if (entity.HasComponent<InstancedMeshComponent>())
                        {
                            f.HasRenderable = true;
                            f.RenderableKind = "InstancedMeshComponent";
                            const auto& imc = entity.GetComponent<InstancedMeshComponent>();
                            f.GeometryRequired = true;
                            const bool hasMesh = static_cast<bool>(imc.MeshSource);
                            const bool hasInstances = !imc.Instances.empty() || imc.PlacementAssetHandle != 0;
                            f.GeometryPresent = hasMesh && hasInstances;
                            if (!hasMesh)
                                f.GeometryDetail = "the InstancedMeshComponent's MeshSource is null";
                            else if (!hasInstances)
                                f.GeometryDetail = "no instances (inline list empty and no placement asset)";
                            // Instances live in world space, not under the entity
                            // transform, so a combined bound isn't computed here —
                            // camera-relative checks are skipped (BoundsKnown stays false).
                            if (imc.OverrideMaterial)
                                resolveMaterialShader(*imc.OverrideMaterial);
                            else if (entity.HasComponent<MaterialComponent>())
                                resolveMaterialShader(entity.GetComponent<MaterialComponent>().m_Material);
                        }
                        else if (entity.HasComponent<SubmeshComponent>())
                        {
                            f.HasRenderable = true;
                            f.RenderableKind = "SubmeshComponent";
                            const auto& sm = entity.GetComponent<SubmeshComponent>();
                            f.GeometryRequired = true;
                            f.GeometryPresent = static_cast<bool>(sm.m_Mesh);
                            if (!f.GeometryPresent)
                                f.GeometryDetail = "the SubmeshComponent's mesh is null";
                            else
                                setBoundsFromLocal(sm.m_Mesh->GetBoundingSphere());
                            f.HasVisibilityFlag = true;
                            f.VisibilityFlagOn = sm.m_Visible;
                            f.VisibilityFlagName = "SubmeshComponent.m_Visible";
                            if (entity.HasComponent<MaterialComponent>())
                                resolveMaterialShader(entity.GetComponent<MaterialComponent>().m_Material);
                        }
                        else if (entity.HasComponent<SpriteRendererComponent>())
                        {
                            f.HasRenderable = true;
                            f.RenderableKind = "SpriteRendererComponent";
                            f.GeometryRequired = false; // always drawn from the transform
                            f.GeometryPresent = true;
                            // 2D renderable: culled via the 2D path, not the 3D view
                            // frustum, so camera-relative checks are left unevaluated.
                        }
                        else if (entity.HasComponent<CircleRendererComponent>())
                        {
                            f.HasRenderable = true;
                            f.RenderableKind = "CircleRendererComponent";
                            f.GeometryRequired = false;
                            f.GeometryPresent = true;
                        }
                        else if (entity.HasComponent<TextComponent>())
                        {
                            f.HasRenderable = true;
                            f.RenderableKind = "TextComponent";
                            f.GeometryRequired = false;
                            f.GeometryPresent = true;
                        }
                        else if (entity.HasComponent<WaterComponent>())
                        {
                            f.HasRenderable = true;
                            f.RenderableKind = "WaterComponent";
                            f.GeometryRequired = false;
                            f.GeometryPresent = true;
                            f.HasVisibilityFlag = true;
                            f.VisibilityFlagOn = entity.GetComponent<WaterComponent>().m_Enabled;
                            f.VisibilityFlagName = "WaterComponent.m_Enabled";
                        }
                        else if (entity.HasComponent<TerrainComponent>())
                        {
                            f.HasRenderable = true;
                            f.RenderableKind = "TerrainComponent";
                            f.GeometryRequired = false;
                            f.GeometryPresent = true;
                        }
                        else if (entity.HasComponent<ParticleSystemComponent>())
                        {
                            f.HasRenderable = true;
                            f.RenderableKind = "ParticleSystemComponent";
                            f.GeometryRequired = false;
                            f.GeometryPresent = true;
                        }
                        else if (entity.HasComponent<TileRendererComponent>())
                        {
                            f.HasRenderable = true;
                            f.RenderableKind = "TileRendererComponent";
                            f.GeometryRequired = false;
                            f.GeometryPresent = true;
                        }
                        else if (entity.HasComponent<DecalComponent>())
                        {
                            f.HasRenderable = true;
                            f.RenderableKind = "DecalComponent";
                            f.IsDecal = true;
                            f.GeometryRequired = false;
                            f.GeometryPresent = true;
                            const RenderingPath renderingPath = Renderer3D::GetRendererSettings().Path;
                            f.RenderingPath = RenderingPathName(renderingPath);
                            const auto& decal = entity.GetComponent<DecalComponent>();
                            switch (decal.m_Mode)
                            {
                                case DecalMode::Normal:
                                    f.DecalMode = "normal";
                                    f.DecalTextureRequired = true;
                                    f.DecalTexturePresent = static_cast<bool>(decal.m_NormalTexture);
                                    f.DecalTextureSlot = "normal";
                                    break;
                                case DecalMode::RMA:
                                    f.DecalMode = "rma";
                                    f.DecalTextureRequired = true;
                                    f.DecalTexturePresent = static_cast<bool>(decal.m_RMATexture);
                                    f.DecalTextureSlot = "roughnessMetallicAO";
                                    break;
                                case DecalMode::Emissive:
                                    f.DecalMode = "emissive";
                                    f.DecalTextureRequired = true;
                                    f.DecalTexturePresent = static_cast<bool>(decal.m_EmissiveTexture);
                                    f.DecalTextureSlot = "emissive";
                                    break;
                                case DecalMode::Albedo:
                                default:
                                    f.DecalMode = "albedo";
                                    // Albedo intentionally falls back to the renderer's white texture.
                                    f.DecalTextureRequired = false;
                                    f.DecalTexturePresent = true;
                                    break;
                            }

                            // Forward and Forward+ route every authored mode
                            // through the transparent albedo-overlay shader.
                            // Normal/RMA/emissive slots are not consulted there.
                            if (!RenderExplain::DecalUsesModeSpecificTexture(f.RenderingPath, decal.m_Transparent))
                            {
                                f.DecalTextureRequired = false;
                                f.DecalTexturePresent = true;
                                f.DecalTextureSlot.clear();
                            }

                            const i32 entityHandle = static_cast<i32>(static_cast<entt::entity>(entity));
                            const Renderer3D::DecalVisibilityObservation observation =
                                Renderer3D::ObserveDecalVisibility(entityHandle);
                            f.SubmissionKnown = observation.HasSample;
                            f.Submitted = observation.Submitted;
                            f.DrawIssuedKnown = observation.HasSample;
                            f.DrawIssued = observation.DrawIssued;
                            f.ReceiverIntersectionKnown = observation.ReceiverIntersectionKnown;
                            f.ReceiverIntersectsProjection = observation.ReceiverIntersectsProjection;
                            f.FragmentResultKnown = observation.FragmentResultKnown;
                            f.FragmentsSurvived = observation.FragmentsSurvived;

                            const glm::vec3 safeSize = glm::max(decal.m_Size, glm::vec3(1.0e-4f));
                            setBoundsFromLocal(BoundingSphere(glm::vec3(0.0f), 0.5f * glm::length(safeSize)));
                        }
                        else if (entity.HasComponent<EnvironmentMapComponent>())
                        {
                            // The skybox background. It is gated on m_EnableSkybox
                            // and a loaded environment map, not the entity transform.
                            f.HasRenderable = true;
                            f.RenderableKind = "EnvironmentMapComponent (skybox)";
                            f.GeometryRequired = false;
                            const auto& env = entity.GetComponent<EnvironmentMapComponent>();
                            f.GeometryPresent = static_cast<bool>(env.m_EnvironmentMap);
                            f.HasVisibilityFlag = true;
                            f.VisibilityFlagOn = env.m_EnableSkybox;
                            f.VisibilityFlagName = "EnvironmentMapComponent.m_EnableSkybox";
                        }
                        else if (entity.HasComponent<ProceduralSkyComponent>())
                        {
                            f.HasRenderable = true;
                            f.RenderableKind = "ProceduralSkyComponent";
                            f.GeometryRequired = false;
                            f.GeometryPresent = true;
                        }
                        else if (entity.HasComponent<StarNestSkyComponent>())
                        {
                            f.HasRenderable = true;
                            f.RenderableKind = "StarNestSkyComponent";
                            f.GeometryRequired = false;
                            f.GeometryPresent = true;
                        }

                        // Degenerate scale (any axis ~0) collapses the geometry to nothing.
                        if (hasTransform)
                        {
                            const glm::vec3& scale = entity.GetComponent<TransformComponent>().Scale;
                            constexpr f32 kScaleEpsilon = 1e-6f;
                            f.ScaleDegenerate = std::abs(scale.x) < kScaleEpsilon ||
                                                std::abs(scale.y) < kScaleEpsilon ||
                                                std::abs(scale.z) < kScaleEpsilon;
                        }

                        // Camera-relative checks against the editor camera. BehindCamera
                        // comes from the camera pose (robust); the frustum check uses the
                        // engine's actual view frustum from the last rendered frame.
                        if (server.Context().GetCameraPose && f.BoundsKnown)
                        {
                            const McpCameraPose pose = server.Context().GetCameraPose();
                            in.CameraKnown = true;
                            if (glm::dot(pose.Forward, pose.Forward) > 1e-12f)
                            {
                                const glm::vec3 forward = glm::normalize(pose.Forward);
                                const glm::vec3 toCenter = worldSphere.Center - pose.Position;
                                const f32 along = glm::dot(toCenter, forward);
                                f.BehindCamera = (along + worldSphere.Radius) < 0.0f;
                            }
                            BoundingSphere cullSphere = worldSphere;
                            cullSphere.Radius *= 1.3f; // match Renderer3D::IsVisibleInFrustum expansion
                            f.InFrustum = Renderer3D::GetViewFrustum().IsBoundingSphereVisible(cullSphere);
                        }
                    }
                }

                const RenderExplain::WhyNotVisibleVerdict verdict = RenderExplain::ExplainWhyNotVisible(in);

                Json facts;
                facts["entityExists"] = f.EntityExists;
                facts["hasRenderable"] = f.HasRenderable;
                facts["renderableKind"] = f.RenderableKind;
                facts["geometryRequired"] = f.GeometryRequired;
                facts["geometryPresent"] = f.GeometryPresent;
                facts["geometryDetail"] = f.GeometryDetail;
                facts["hasVisibilityFlag"] = f.HasVisibilityFlag;
                facts["visibilityFlagName"] = f.VisibilityFlagName;
                facts["visibilityFlagOn"] = f.VisibilityFlagOn;
                facts["scaleDegenerate"] = f.ScaleDegenerate;
                facts["hasMaterialShader"] = f.HasMaterialShader;
                facts["materialShaderName"] = f.MaterialShaderName;
                facts["materialShaderHasErrors"] = f.MaterialShaderHasErrors;
                facts["boundsKnown"] = f.BoundsKnown;
                facts["behindCamera"] = f.BehindCamera;
                facts["inFrustum"] = f.InFrustum;
                facts["isDecal"] = f.IsDecal;
                if (f.IsDecal)
                {
                    facts["renderingPath"] = f.RenderingPath;
                    facts["decalMode"] = f.DecalMode;
                    facts["decalTextureRequired"] = f.DecalTextureRequired;
                    facts["decalTexturePresent"] = f.DecalTexturePresent;
                    facts["decalTextureSlot"] = f.DecalTextureSlot;
                    facts["receiverIntersectionKnown"] = f.ReceiverIntersectionKnown;
                    facts["receiverIntersectsProjection"] = f.ReceiverIntersectsProjection;
                    facts["submissionKnown"] = f.SubmissionKnown;
                    facts["submitted"] = f.Submitted;
                    facts["drawIssuedKnown"] = f.DrawIssuedKnown;
                    facts["drawIssued"] = f.DrawIssued;
                    facts["fragmentResultKnown"] = f.FragmentResultKnown;
                    facts["fragmentsSurvived"] = f.FragmentsSurvived;
                }

                j["entity"] = UuidToString(UUID(id));
                j["reasonCode"] = verdict.ReasonCode;
                j["summary"] = verdict.Summary;
                j["renderableConfigOk"] = verdict.RenderableConfigOk;
                j["visible"] = verdict.Visible;
                j["checks"] = verdict.Checks;
                j["facts"] = facts;
                j["sceneLoaded"] = in.SceneLoaded;
                j["cameraKnown"] = in.CameraKnown;
                j["anyShaderHasErrors"] = in.AnyShaderHasErrors;
                j["shaderErrorCount"] = in.ShaderErrorCount;
                return j;
            };

            Json result = server.MarshalRead(gather);
            if (result.value("facts", Json::object()).value("isDecal", false) &&
                !result.value("facts", Json::object()).value("submissionKnown", false) &&
                server.Context().GetFrameIndex)
            {
                const u64 baseFrame = server.MarshalRead([&server]() -> Json
                                                         { return Json(server.Context().GetFrameIndex()); })
                                          .get<u64>();
                (void)AwaitRenderedFrames(server, baseFrame, 2, std::chrono::seconds(8));
                result = server.MarshalRead(gather);
            }

            return ToolResult::Structured(result);
        }

        // =====================================================================
        // Issue #607 — render-diagnostics gaps found while doing real work.
        // =====================================================================

        // ---- olo_render_probe_pixel (main-marshaled; facade readback) ----------
        //
        // The NUMERIC counterpart of olo_render_capture_target: instead of an
        // image of a whole target, the exact decoded values under ONE pixel,
        // across every G-Buffer channel at once. A capture shows a normal map
        // "looks bluish"; this says the normal is (0.0, 0.0, 1.0) when it should
        // be (0, 1, 0) — which is the difference between an hour of shader
        // patching and a one-call diagnosis.

        // Options for one texel probe (issue #607). Defaults reproduce the
        // simple "viewport pixel at mip 0" probe; the G-Buffer mode fills the
        // viewport reference dims, the single-target mode adds space / mip /
        // layer / afterPass control.
        struct ProbeRequest
        {
            ProbePixel::ProbeSpace Space = ProbePixel::ProbeSpace::Viewport;
            u32 Mip = 0;
            // Viewport-space reference dims (the render size a screenshot
            // shows). 0 = use the target mip's own dims (identity mapping).
            u32 RefWidth = 0;
            u32 RefHeight = 0;
            // Read THIS resource instead of resolving `name` — the afterPass
            // snapshot scratch clone, which carries an identity like any other
            // texture since #810.
            RHI::ResourceHandle OverrideHandle;
            // Explicit array-layer / cube-face / 3D-slice selector; when
            // absent the target's own view layer applies (a CSM cascade view
            // must not silently read cascade 0).
            bool HasLayer = false;
            long long Layer = 0;
        };

        // (main thread) Read back a SINGLE texel of one named render-graph target.
        // Goes through RenderCommand::ReadTextureSubImage — the facade readback
        // spine — with a 1x1 region, so probing a 4K G-Buffer costs a handful of
        // bytes, not 64 MB, and works on both backends (#810; this was raw
        // glGetTextureSubImage and hard-refused under Vulkan before).
        //
        // Coordinates go through ProbePixel::MapProbeCoord (issue #607):
        // viewport space maps proportionally onto the target mip, texel space
        // addresses the exact texel; both are top-left-origin (the screenshot
        // convention) and the mapping is echoed in the sample so it is never
        // guesswork. Which ROW that becomes is the one backend predicate,
        // RHI::RenderTargetRowsAreBottomUp() (ADR 0011 amendment (85)).
        ProbePixel::TexelSample ProbeTexel(const RenderGraph& graph, const std::string& name,
                                           u32 x, u32 yTopLeft, const ProbeRequest& request = {})
        {
            ProbePixel::TexelSample sample;
            sample.Target = name;

            // An afterPass clone is read exactly like a live target: it carries
            // the source's storage AND an identity of its own (#810), so there
            // is one code path here rather than two.
            RHI::ResourceHandle handle = request.OverrideHandle;
            bool depthFromFramebuffer = false;
            if (!handle.IsValid())
            {
                handle = ResolveTargetHandle(name, depthFromFramebuffer);
                if (!handle.IsValid())
                {
                    sample.Unavailable = "Render-graph resource '" + name +
                                         "' has no GPU backing this frame (wrong rendering path, effect "
                                         "disabled, or not yet rendered).";
                    return sample;
                }
            }

            // Which layer (z offset) to read: an explicit selector, or the
            // target's own view layer — a per-cascade view resolves to the
            // whole parent array, so reading z=0 unconditionally would
            // silently answer from cascade 0 (the capture tool's exact rule).
            const CaptureLayer::TargetLayers layers = ResolveTargetLayers(graph, name);
            const CaptureLayer::Selection selection =
                CaptureLayer::SelectLayer(layers, name, request.HasLayer, request.Layer);
            if (!selection.Error.empty())
            {
                sample.Unavailable = selection.Error;
                return sample;
            }
            sample.Layer = selection.Layer;

            u32 mipWidth = 0;
            u32 mipHeight = 0;
            RenderCommand::GetTextureDimensions(handle, request.Mip, mipWidth, mipHeight);
            if (mipWidth == 0 || mipHeight == 0)
            {
                sample.Unavailable = "'" + name + "' has no storage at mip " + std::to_string(request.Mip) + ".";
                return sample;
            }

            ProbeReadPlan plan;
            if (std::string planError; !PlanProbeRead(handle, request.Mip, plan, planError))
            {
                sample.Unavailable = "'" + name + "': " + planError + ".";
                return sample;
            }
            if (depthFromFramebuffer && !plan.Format.IsDepth)
            {
                // The name resolved through a depth-only framebuffer but the
                // backend describes colour storage. Contradictory — reading
                // either way would be a guess, so say so instead.
                sample.Unavailable = "'" + name + "' resolved to a depth attachment whose storage reports "
                                                  "as colour — refusing rather than guessing.";
                return sample;
            }

            sample.SourceWidth = mipWidth;
            sample.SourceHeight = mipHeight;
            sample.Format = plan.Format.Token;

            const u32 refWidth = request.RefWidth != 0 ? request.RefWidth : sample.SourceWidth;
            const u32 refHeight = request.RefHeight != 0 ? request.RefHeight : sample.SourceHeight;
            sample.Mapped = ProbePixel::MapProbeCoord(request.Space, x, yTopLeft, refWidth, refHeight,
                                                      sample.SourceWidth, sample.SourceHeight, request.Mip);
            if (!sample.Mapped.Valid)
            {
                sample.Unavailable = sample.Mapped.Error;
                return sample;
            }

            // ONE row order per backend (amendment (85)). The clone is a copy of
            // a backend-native image, so it shares the backend's convention.
            const u32 readRow =
                RHI::RenderTargetRowsAreBottomUp() ? sample.Mapped.GLRowBottomUp : sample.Mapped.TexelY;

            std::array<f32, 4> floats{ 0.0f, 0.0f, 0.0f, 0.0f };
            std::array<i32, 4> ints{ 0, 0, 0, 0 };
            void* destination =
                plan.IsInteger ? static_cast<void*>(ints.data()) : static_cast<void*>(floats.data());

            if (!RenderCommand::ReadTextureSubImage(handle, request.Mip, static_cast<i32>(sample.Mapped.TexelX),
                                                    static_cast<i32>(readRow), static_cast<i32>(selection.Layer),
                                                    1u, 1u, 1u, plan.DestFormat, 4 * sizeof(f32), destination))
            {
                sample.Unavailable =
                    "Readback of '" + name + "' (" + plan.Format.Token + ") failed — see the editor log.";
                return sample;
            }

            sample.Available = true;
            sample.Channels = plan.ReportChannels;
            sample.Kind = plan.IsInteger ? ProbePixel::SampleKind::Int : ProbePixel::SampleKind::Float;
            sample.F = floats;
            sample.I = ints;
            return sample;
        }

        // ---- afterPass snapshots (issue #607) ---------------------------------
        // Arm the shared RenderGraphPassSnapshot for `passName` over the named
        // resources, then force a frame so the post-pass hook fires. Returns an
        // empty string on success; `outFrameRendered` reports whether a frame
        // actually rendered inside the wait (false = throttle/stall/cancel —
        // the collect side uses it to diagnose an unfired hook honestly). The
        // CALLER's next MarshalRead job must read
        // RenderGraphDebugRuntime::GetPassSnapshot() (checking IsPending() for
        // "the pass never executed") and Disarm() when done — reading and
        // disarming in the same main-thread job keeps the scratch textures from
        // being re-armed under a concurrent tool call.
        std::string ArmAfterPassSnapshot(McpServer& server, const std::string& passName,
                                         const std::vector<std::string>& resources,
                                         bool& outFrameRendered)
        {
            const Json armed = server.MarshalRead([passName, resources]() -> Json
                                                  {
                const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph();
                if (!graph)
                    return Json{ { "__error", "No active render graph (the editor is not in 3D mode, or no frame has been rendered yet)." } };

                const auto& order = graph->GetExecutionOrder();
                if (std::find(order.begin(), order.end(), passName) == order.end())
                {
                    std::string valid;
                    for (const auto& pass : order)
                    {
                        if (!valid.empty())
                            valid += ", ";
                        valid += pass;
                    }
                    return Json{ { "__error", "Unknown pass '" + passName +
                                                  "' for afterPass. Passes in this frame's execution order: " + valid + "." } };
                }

                std::vector<RenderGraphPassSnapshot::Request> requests;
                requests.reserve(resources.size());
                for (const auto& resourceName : resources)
                {
                    requests.push_back(RenderGraphPassSnapshot::Request{
                        resourceName,
                        [resourceName]() -> RHI::ResourceHandle
                        {
                            bool depthFromFramebuffer = false;
                            return ResolveTargetHandle(resourceName, depthFromFramebuffer);
                        } });
                }
                // Installing a post-pass hook is a logical mutation of the
                // live graph behind a read-only accessor — the same confined
                // const_cast RenderGraphDebugger uses for its frame capture.
                RenderGraphDebugRuntime::GetPassSnapshot().Arm(const_cast<RenderGraph*>(graph.Raw()),
                                                               passName, std::move(requests));
                return Json::object(); });

            if (armed.is_object() && armed.contains("__error"))
                return armed["__error"].get<std::string>();

            // Render a frame so the armed hook actually fires. A false return
            // (throttle / main-thread stall / cancellation) is threaded to the
            // caller's collect job so an unfired hook is diagnosed honestly
            // instead of as "culled".
            outFrameRendered = ForceFreshFrame(server, /*settleFrames*/ 1);
            return {};
        }

        // (main thread) The collect half: fetch the snapshot result for `name`
        // after ArmAfterPassSnapshot + a rendered frame, or an error message.
        // Leaves the snapshot disarmed. `outResult` is only valid when the
        // returned string is empty. `frameRendered` is ArmAfterPassSnapshot's
        // out-flag — it picks the honest diagnosis when the hook never fired.
        std::string CollectAfterPassSnapshot(const std::string& passName, const std::string& name,
                                             bool frameRendered, RenderGraphPassSnapshot::Result& outResult)
        {
            auto& snapshot = RenderGraphDebugRuntime::GetPassSnapshot();
            // A concurrent afterPass tool call could have re-armed the shared
            // snapshot between this call's arm and collect jobs — matching by
            // resource name alone would then silently hand back the OTHER
            // call's clone. The armed pass name is the discriminator.
            if (snapshot.GetPassName() != passName)
            {
                return "Another tool call re-armed the afterPass snapshot concurrently (now armed for pass '" +
                       snapshot.GetPassName() + "', expected '" + passName +
                       "'). afterPass requests are one-at-a-time; retry.";
            }
            if (snapshot.IsPending())
            {
                snapshot.Disarm();
                if (!frameRendered)
                    return "Timed out waiting for a frame to render after arming the afterPass snapshot (viewport "
                           "render-throttled, the editor stalled, or the call was cancelled) — nothing was "
                           "snapshotted. Retry, or make sure the viewport is rendering.";
                return "Pass '" + passName + "' did not execute this frame (culled or disabled) — nothing was "
                                             "snapshotted. Check olo_render_graph_topology_export for the culled list.";
            }
            for (const auto& result : snapshot.GetResults())
            {
                if (result.ResourceName != name)
                    continue;
                if (!result.Captured)
                {
                    const std::string error = result.Error;
                    snapshot.Disarm();
                    return error;
                }
                outResult = result;
                snapshot.Disarm();
                return {};
            }
            snapshot.Disarm();
            return "Internal error: no snapshot result recorded for '" + name + "'.";
        }

        ToolResult Handle_RenderProbePixel(McpServer& server, const Json& args)
        {
            // Backend-neutral since #810: the probe reads through the facade
            // spine, and 'afterPass' snapshots are neutral too.
            if (!args.contains("x") || !args["x"].is_number_integer() ||
                !args.contains("y") || !args["y"].is_number_integer())
                return ToolResult::Error("Missing required arguments 'x' and 'y' (pixel, top-left origin).");
            const auto x = static_cast<u32>(std::max<long long>(0, args["x"].get<long long>()));
            const auto y = static_cast<u32>(std::max<long long>(0, args["y"].get<long long>()));

            std::string target;
            if (args.contains("target") && args["target"].is_string())
                target = args["target"].get<std::string>();

            // space:"texel" + mip + layer + afterPass (issue #607). All four
            // address ONE specific resource, so they require 'target' — the
            // G-Buffer multi-target mode probes seven differently-sized
            // resources for which a single texel coordinate is ambiguous.
            auto space = ProbePixel::ProbeSpace::Viewport;
            if (args.contains("space") && args["space"].is_string())
            {
                if (!ProbePixel::ParseProbeSpace(args["space"].get<std::string>(), space))
                    return ToolResult::Error("Invalid 'space': expected \"viewport\" or \"texel\".");
            }
            u32 mip = 0;
            if (args.contains("mip") && args["mip"].is_number_integer())
                mip = static_cast<u32>(std::clamp<long long>(args["mip"].get<long long>(), 0, 16));
            const bool hasLayer = args.contains("layer") && args["layer"].is_number_integer();
            const long long layer = hasLayer ? args["layer"].get<long long>() : 0;
            std::string afterPass;
            if (args.contains("afterPass") && args["afterPass"].is_string())
                afterPass = args["afterPass"].get<std::string>();
            if (target.empty() && (space == ProbePixel::ProbeSpace::Texel || mip != 0 || hasLayer || !afterPass.empty()))
                return ToolResult::Error("'space':\"texel\", 'mip', 'layer' and 'afterPass' require 'target' — the "
                                         "G-Buffer multi-target probe spans several differently-sized resources.");

            if (args.value("forceFrame", false) && afterPass.empty())
                (void)ForceFreshFrame(server, /*settleFrames*/ 2);

            // afterPass: snapshot the target as of that pass's execution, then
            // probe the snapshot clone instead of the (later-overwritten) live
            // resource.
            bool afterPassFrameRendered = true;
            if (!afterPass.empty())
            {
                if (const std::string error = ArmAfterPassSnapshot(server, afterPass, { target }, afterPassFrameRendered);
                    !error.empty())
                    return ToolResult::Error(error);
            }

            const Json result = server.MarshalRead([&server, x, y, target, space, mip, hasLayer, layer,
                                                    afterPass, afterPassFrameRendered]() -> Json
                                                   {
                const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph();
                if (!graph)
                    return Json{ { "__error", "No active render graph (the editor is not in 3D mode, or no frame has been rendered yet)." } };

                const u64 frameIndex = server.Context().GetFrameIndex ? server.Context().GetFrameIndex() : 0;

                // Viewport-space reference: the physical (display) render size —
                // the frame olo_screenshot shows, so a pixel picked off a
                // screenshot maps to the same visual location on any target.
                const u32 refWidth = graph->GetPhysicalWidth();
                const u32 refHeight = graph->GetPhysicalHeight();

                // Single-target mode: raw channels of whatever was asked for, so
                // the tool works for ANY capturable target (AOBuffer, BloomColor,
                // VirtualGeometryDebug, ...), not just the G-Buffer.
                if (!target.empty())
                {
                    ProbeRequest request;
                    request.Space = space;
                    request.Mip = mip;
                    request.RefWidth = refWidth;
                    request.RefHeight = refHeight;
                    request.HasLayer = hasLayer;
                    request.Layer = layer;

                    if (!afterPass.empty())
                    {
                        RenderGraphPassSnapshot::Result snapshotResult;
                        if (const std::string error =
                                CollectAfterPassSnapshot(afterPass, target, afterPassFrameRendered, snapshotResult);
                            !error.empty())
                            return Json{ { "__error", error } };
                        request.OverrideHandle = snapshotResult.Handle;
                    }

                    const ProbePixel::TexelSample sample = ProbeTexel(*graph, target, x, y, request);
                    if (!sample.Available && sample.SourceWidth == 0)
                        return Json{ { "__error", sample.Unavailable +
                                                      " Call olo_render_list_targets for the live list." } };
                    Json j = ProbePixel::BuildRawProbe(sample, x, y);
                    if (!afterPass.empty())
                    {
                        j["afterPass"] = afterPass;
                        j["afterPassNote"] = "meta.frameIndex is the collect-time frame; the snapshot was cloned "
                                             "mid-frame during the immediately preceding rendered frame.";
                    }
                    j["meta"] = CaptureStampJson(frameIndex, server.Context());
                    return j;
                }

                ProbePixel::GBufferProbeInput in;
                in.X = x;
                in.Y = y;
                in.RenderingPath = RenderingPathName(Renderer3D::GetRendererSettings().Path);

                if (server.Context().GetCameraPose)
                {
                    const McpCameraPose pose = server.Context().GetCameraPose();
                    in.CameraKnown = pose.FarClip > pose.NearClip && pose.NearClip > 0.0f;
                    in.NearClip = pose.NearClip;
                    in.FarClip = pose.FarClip;
                }

                ProbeRequest gbufferRequest;
                gbufferRequest.RefWidth = refWidth;
                gbufferRequest.RefHeight = refHeight;

                in.Albedo = ProbeTexel(*graph, std::string(ResourceNames::GBufferAlbedo), x, y, gbufferRequest);
                in.Normal = ProbeTexel(*graph, std::string(ResourceNames::GBufferNormal), x, y, gbufferRequest);
                in.Emissive = ProbeTexel(*graph, std::string(ResourceNames::GBufferEmissive), x, y, gbufferRequest);
                in.BakedGI = ProbeTexel(*graph, std::string(ResourceNames::GBufferBakedGI), x, y, gbufferRequest);
                in.Velocity = ProbeTexel(*graph, std::string(ResourceNames::Velocity), x, y, gbufferRequest);
                in.EntityId = ProbeTexel(*graph, std::string(ResourceNames::SceneEntityID), x, y, gbufferRequest);
                in.Depth = ProbeTexel(*graph, std::string(ResourceNames::SceneDepth), x, y, gbufferRequest);

                // The presented colour is whatever the LAST enabled post stage
                // wrote, and which stage that is depends on the live toggles — so
                // walk the chain backwards and take the first one that resolved.
                static constexpr std::array<std::string_view, 6> kFinalColorChain{
                    ResourceNames::SelectionOutlineColorTexture,
                    ResourceNames::FXAAColorTexture,
                    ResourceNames::VignetteColorTexture,
                    ResourceNames::UpscalerColorTexture,
                    ResourceNames::ToneMapColorTexture,
                    ResourceNames::SceneColorTexture,
                };
                for (const std::string_view candidate : kFinalColorChain)
                {
                    ProbePixel::TexelSample sample = ProbeTexel(*graph, std::string(candidate), x, y, gbufferRequest);
                    if (sample.Available)
                    {
                        in.FinalColor = std::move(sample);
                        break;
                    }
                    if (in.FinalColor.Target.empty())
                        in.FinalColor = std::move(sample); // keep the first failure's reason
                }

                Json j = ProbePixel::BuildGBufferProbe(in);
                j["meta"] = CaptureStampJson(frameIndex, server.Context());
                return j; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Structured(result);
        }

        // ---- olo_render_target_stats (main-marshaled; GL readback) -------------
        // Exact float min/max/mean + bit-exact unique-value histogram over a
        // rect of one target (issue #607). An 8-bit PNG capture hides 1-ULP
        // corruption (1.0 and 0.99999994 both encode as 255), so mapping a
        // corrupt region by single-texel probes took hundreds of round-trips
        // during the GTAO hunt; this answers "is this region EXACTLY 1.0f,
        // and if not what distinct values does it hold" in one call.

        // Rect readback ceiling: 4M texels * 4 channels * 4 bytes = 64 MB —
        // roomy for any full-HD mip while keeping a stray 8K request from
        // stalling the main thread for seconds.
        constexpr u64 kMaxStatsRectTexels = 4ull * 1024ull * 1024ull;

        // (main thread) The backend-neutral twin: a rect of one mip of a LIVE
        // render-graph target, channel-interleaved, through
        // RenderCommand::ReadTextureSubImage (#810). `rectY` is in the
        // BACKEND's row order — the caller flips a top-left rect to bottom-up
        // rows iff RHI::RenderTargetRowsAreBottomUp(), exactly as the capture
        // path does; stats aggregate per channel so only the rect's PLACEMENT
        // depends on row order, never the values.
        //
        // Returns the values the PLAN says are meaningful: a 3-channel source
        // is read as 4 components and compacted back to 3 here, so the stats
        // JSON reports the same channel count the GL arm always reported.
        std::vector<f32> ReadRectFloatsThroughFacade(RHI::ResourceHandle handle, const u32 mip, const u32 layer,
                                                     const u32 rectX, const u32 rectY, const u32 rectW,
                                                     const u32 rectH, const ProbeReadPlan& plan,
                                                     std::string& outError)
        {
            const sizet texels = static_cast<sizet>(rectW) * rectH;
            const sizet readValues = texels * static_cast<sizet>(plan.ReadChannels);

            std::vector<f32> raw(readValues, 0.0f);
            void* destination = raw.data();
            std::vector<i32> ints;
            if (plan.IsInteger)
            {
                ints.assign(readValues, 0);
                destination = ints.data();
            }

            if (!RenderCommand::ReadTextureSubImage(handle, mip, static_cast<i32>(rectX), static_cast<i32>(rectY),
                                                    static_cast<i32>(layer), rectW, rectH, 1u, plan.DestFormat,
                                                    readValues * sizeof(f32), destination))
            {
                outError = std::string("readback failed (format ") + plan.Format.Token +
                           " — see the editor log)";
                return {};
            }

            if (plan.IsInteger)
            {
                for (sizet i = 0; i < readValues; ++i)
                    raw[i] = static_cast<f32>(ints[i]);
            }

            if (plan.ReportChannels == plan.ReadChannels)
                return raw;

            std::vector<f32> compacted(texels * static_cast<sizet>(plan.ReportChannels), 0.0f);
            for (sizet t = 0; t < texels; ++t)
            {
                for (i32 c = 0; c < plan.ReportChannels; ++c)
                    compacted[t * plan.ReportChannels + c] = raw[t * plan.ReadChannels + c];
            }
            return compacted;
        }

        ToolResult Handle_RenderTargetStats(McpServer& server, const Json& args)
        {
            // Backend-neutral since #810 — same contract as
            // olo_render_probe_pixel above, 'afterPass' included.
            if (!args.contains("name") || !args["name"].is_string())
                return ToolResult::Error("Missing required argument 'name' (render-graph resource name; see olo_render_list_targets).");
            const std::string name = args["name"].get<std::string>();

            u32 mip = 0;
            if (args.contains("mip") && args["mip"].is_number_integer())
                mip = static_cast<u32>(std::clamp<long long>(args["mip"].get<long long>(), 0, 16));

            const bool hasLayer = args.contains("layer") && args["layer"].is_number_integer();
            const long long requestedLayer = hasLayer ? args["layer"].get<long long>() : 0;

            // rect {x, y, w, h} in texel coordinates of the mip, top-left
            // origin (a capture PNG's orientation). Omitted = the whole mip.
            bool hasRect = false;
            u32 rectX = 0;
            u32 rectY = 0;
            u32 rectW = 0;
            u32 rectH = 0;
            if (args.contains("rect"))
            {
                const Json& rect = args["rect"];
                if (!rect.is_object() || !rect.contains("x") || !rect.contains("y") ||
                    !rect.contains("w") || !rect.contains("h") ||
                    !rect["x"].is_number_integer() || !rect["y"].is_number_integer() ||
                    !rect["w"].is_number_integer() || !rect["h"].is_number_integer())
                    return ToolResult::Error("Invalid 'rect': expected { x, y, w, h } integers (texel coords of the mip, top-left origin).");
                const long long rx = rect["x"].get<long long>();
                const long long ry = rect["y"].get<long long>();
                const long long rw = rect["w"].get<long long>();
                const long long rh = rect["h"].get<long long>();
                if (rx < 0 || ry < 0 || rw <= 0 || rh <= 0)
                    return ToolResult::Error("Invalid 'rect': x/y must be >= 0 and w/h > 0.");
                hasRect = true;
                rectX = static_cast<u32>(rx);
                rectY = static_cast<u32>(ry);
                rectW = static_cast<u32>(rw);
                rectH = static_cast<u32>(rh);
            }

            std::string afterPass;
            if (args.contains("afterPass") && args["afterPass"].is_string())
                afterPass = args["afterPass"].get<std::string>();
            if (args.value("forceFrame", false) && afterPass.empty())
                (void)ForceFreshFrame(server, /*settleFrames*/ 2);
            bool afterPassFrameRendered = true;
            if (!afterPass.empty())
            {
                if (const std::string error = ArmAfterPassSnapshot(server, afterPass, { name }, afterPassFrameRendered);
                    !error.empty())
                    return ToolResult::Error(error);
            }

            const Json result = server.MarshalRead([&server, name, mip, hasLayer, requestedLayer, hasRect,
                                                    rectX, rectY, rectW, rectH, afterPass,
                                                    afterPassFrameRendered]() -> Json
                                                   {
                const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph();
                if (!graph)
                    return Json{ { "__error", "No active render graph (the editor is not in 3D mode, or no frame has been rendered yet)." } };

                // Live target or mid-frame clone, one path: the clone carries an
                // identity of its own since #810, so both resolve to a handle
                // and read through the facade spine.
                RHI::ResourceHandle handle;
                RenderGraphPassSnapshot::Result snapshotResult;
                if (!afterPass.empty())
                {
                    if (const std::string error =
                            CollectAfterPassSnapshot(afterPass, name, afterPassFrameRendered, snapshotResult);
                        !error.empty())
                        return Json{ { "__error", error } };
                    handle = snapshotResult.Handle;
                    if (!handle.IsValid())
                        return Json{ { "__error", "The afterPass snapshot clone for '" + name +
                                                      "' did not resolve to a texture." } };
                }
                else
                {
                    bool depthFromFramebuffer = false;
                    handle = ResolveTargetHandle(name, depthFromFramebuffer);
                    if (!handle.IsValid())
                        return Json{ { "__error", "Unknown render-graph resource '" + name +
                                                      "' (or it has no GPU backing this frame). Call olo_render_list_targets for the live list." } };
                }

                const CaptureLayer::TargetLayers layers = ResolveTargetLayers(*graph, name);
                const CaptureLayer::Selection selection =
                    CaptureLayer::SelectLayer(layers, name, hasLayer, requestedLayer);
                if (!selection.Error.empty())
                    return Json{ { "__error", selection.Error } };

                u32 fullW = 0;
                u32 fullH = 0;
                RenderCommand::GetTextureDimensions(handle, mip, fullW, fullH);
                if (fullW == 0 || fullH == 0)
                    return Json{ { "__error", "'" + name + "' has no storage at mip " + std::to_string(mip) + "." } };
                ProbeReadPlan plan;
                if (std::string planError; !PlanProbeRead(handle, mip, plan, planError))
                    return Json{ { "__error", "'" + name + "': " + planError + "." } };

                u32 x = hasRect ? rectX : 0u;
                u32 y = hasRect ? rectY : 0u;
                u32 w = hasRect ? rectW : fullW;
                u32 h = hasRect ? rectH : fullH;
                if (x >= fullW || y >= fullH || x + w > fullW || y + h > fullH)
                    return Json{ { "__error", "rect (" + std::to_string(x) + ", " + std::to_string(y) + ", " +
                                                  std::to_string(w) + "x" + std::to_string(h) + ") exceeds mip " +
                                                  std::to_string(mip) + " (" + std::to_string(fullW) + "x" +
                                                  std::to_string(fullH) + ")." } };
                if (static_cast<u64>(w) * h > kMaxStatsRectTexels)
                    return Json{ { "__error", "rect covers " + std::to_string(static_cast<u64>(w) * h) +
                                                  " texels; the ceiling is " + std::to_string(kMaxStatsRectTexels) +
                                                  ". Shrink the rect or use a higher mip." } };

                // Stats aggregate per channel, so row order does not change any
                // VALUE — only the rect's PLACEMENT. Under a bottom-up backend a
                // top-left rect row range y..y+h maps to rows
                // [mipH - y - h, mipH - y); under a top-down one it is y itself.
                // One predicate decides which (ADR 0011 amendment (85)), and a
                // clone shares its source backend's convention.
                const u32 readRectY = RHI::RenderTargetRowsAreBottomUp() ? fullH - y - h : y;
                std::string readError;
                const std::vector<f32> interleaved =
                    ReadRectFloatsThroughFacade(handle, mip, selection.Layer, x, readRectY, w, h, plan, readError);
                if (!readError.empty())
                    return Json{ { "__error", "Stats readback of '" + name + "' failed: " + readError } };

                Json j = RenderTargetStats::BuildStatsJson(name, plan.Format.Token, x, y, w, h, mip, fullW, fullH,
                                                           selection.Layer, interleaved, plan.ReportChannels);
                if (plan.IsInteger)
                    j["integerNote"] = "Integer target: values converted to float for stats (bit-exact below 2^24).";
                if (!afterPass.empty())
                {
                    j["afterPass"] = afterPass;
                    j["afterPassNote"] = "meta.frameIndex is the collect-time frame; the snapshot was cloned "
                                         "mid-frame during the immediately preceding rendered frame.";
                }
                if (!selection.Note.empty())
                    j["layerNote"] = selection.Note;
                j["meta"] = CaptureStampJson(server.Context().GetFrameIndex ? server.Context().GetFrameIndex() : 0, server.Context());
                return j; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Structured(result);
        }

        // ---- olo_render_validate (main-marshaled; GL readback for compare) -----
        // On-demand render-graph frame validation (issue #607): the compiled
        // hazard sweep + barrier/build diagnostics + resolve failures +
        // physical-identity report, plus an optional bit-exact texture compare
        // ("assert HZB mip0 == scene depth bitwise after GTAOPass").

        const char* HazardKindName(RenderGraph::HazardKind kind)
        {
            using HK = RenderGraph::HazardKind;
            switch (kind)
            {
                case HK::ReadAfterWrite:
                    return "ReadAfterWrite";
                case HK::WriteAfterWrite:
                    return "WriteAfterWrite";
                case HK::WriteAfterRead:
                    return "WriteAfterRead";
                case HK::ResourceKindMismatch:
                    return "ResourceKindMismatch";
                case HK::FeedbackWithoutDeclaration:
                    return "FeedbackWithoutDeclaration";
                case HK::ImportedResourceLifetimeMisuse:
                    return "ImportedResourceLifetimeMisuse";
                case HK::Cycle:
                    return "Cycle";
            }
            return "Unknown";
        }

        const char* BarrierDiagnosticKindName(RenderGraph::BarrierDiagnosticKind kind)
        {
            using BK = RenderGraph::BarrierDiagnosticKind;
            switch (kind)
            {
                case BK::MissingProducer:
                    return "MissingProducer";
                case BK::CulledProducer:
                    return "CulledProducer";
                case BK::UnmappedTransition:
                    return "UnmappedTransition";
                case BK::StaleExtractionHandle:
                    return "StaleExtractionHandle";
                case BK::ExtractionOfCulledResource:
                    return "ExtractionOfCulledResource";
                case BK::InvalidHistoryContract:
                    return "InvalidHistoryContract";
            }
            return "Unknown";
        }

        // (main thread) Channel 0 of one mip/layer as row-major floats with
        // rows flipped to TOP-LEFT order, so diff coordinates match the
        // capture/probe convention. Depth formats read the depth plane.
        //
        // Backend-neutral since #810 — and this one was not merely refusing on
        // Vulkan, it was CRASHING. olo_render_validate had no backend guard at
        // all, and its old GL-id resolve went through
        // Debug::NativeTextureIdForDiagnostics, which truncates a VkImage
        // pointer to a NONZERO garbage u32; that then reached
        // glGetTextureLevelParameteriv with no GL context. Resolving an
        // identity and reading through the facade removes the whole class.
        std::vector<f32> ReadChannel0TopLeft(RHI::ResourceHandle handle, const u32 mip, const u32 layer,
                                             u32& outWidth, u32& outHeight, std::string& outFormat,
                                             std::string& outError)
        {
            outWidth = 0;
            outHeight = 0;

            u32 w = 0;
            u32 h = 0;
            RenderCommand::GetTextureDimensions(handle, mip, w, h);
            if (w == 0 || h == 0)
            {
                outError = "no storage at mip " + std::to_string(mip);
                return {};
            }

            ProbeReadPlan plan;
            if (std::string planError; !PlanProbeRead(handle, mip, plan, planError))
            {
                outError = planError;
                return {};
            }
            outFormat = plan.Format.Token;

            if (static_cast<u64>(w) * h > kMaxStatsRectTexels * 4ull)
            {
                outError = "mip is larger than the compare ceiling (" + std::to_string(w) + "x" +
                           std::to_string(h) + "); compare a higher mip";
                return {};
            }

            // Read every channel the plan says the format has, then keep
            // channel 0. Asking for fewer components than the destination
            // format names would mis-size the buffer on both backends.
            const std::vector<f32> interleaved =
                ReadRectFloatsThroughFacade(handle, mip, layer, 0u, 0u, w, h, plan, outError);
            if (!outError.empty())
                return {};

            const sizet texels = static_cast<sizet>(w) * h;
            if (interleaved.size() < texels * static_cast<sizet>(plan.ReportChannels))
            {
                outError = "readback returned fewer values than the rect covers";
                return {};
            }

            std::vector<f32> channel0(texels, 0.0f);
            for (sizet i = 0; i < texels; ++i)
                channel0[i] = interleaved[i * plan.ReportChannels];

            // ONE row order per backend (amendment (85)): the readback comes
            // back in the BACKEND's row order, and the compare convention is
            // top-left, so flip iff the backend runs bottom-up.
            if (RHI::RenderTargetRowsAreBottomUp())
            {
                std::vector<f32> topLeft(channel0.size());
                for (u32 row = 0; row < h; ++row)
                {
                    const f32* src = channel0.data() + static_cast<sizet>(h - 1u - row) * w;
                    f32* dst = topLeft.data() + static_cast<sizet>(row) * w;
                    std::copy(src, src + w, dst);
                }
                channel0 = std::move(topLeft);
            }

            outWidth = w;
            outHeight = h;
            return channel0;
        }

        ToolResult Handle_RenderValidate(McpServer& server, const Json& args)
        {
            // Optional bit-exact compare of two targets.
            RenderValidate::CompareRequest compare;
            bool hasCompare = false;
            if (args.contains("compare"))
            {
                const Json& c = args["compare"];
                if (!c.is_object() || !c.contains("a") || !c.contains("b") ||
                    !c["a"].is_string() || !c["b"].is_string())
                    return ToolResult::Error("Invalid 'compare': expected { a, b, mipA?, mipB?, layerA?, layerB?, afterPass? } with 'a'/'b' target names.");
                hasCompare = true;
                compare.A = c["a"].get<std::string>();
                compare.B = c["b"].get<std::string>();
                if (c.contains("mipA") && c["mipA"].is_number_integer())
                    compare.MipA = static_cast<u32>(std::clamp<long long>(c["mipA"].get<long long>(), 0, 16));
                if (c.contains("mipB") && c["mipB"].is_number_integer())
                    compare.MipB = static_cast<u32>(std::clamp<long long>(c["mipB"].get<long long>(), 0, 16));
                if (c.contains("layerA") && c["layerA"].is_number_integer())
                {
                    compare.LayerA = static_cast<u32>(std::max<long long>(0, c["layerA"].get<long long>()));
                    compare.HasLayerA = true;
                }
                if (c.contains("layerB") && c["layerB"].is_number_integer())
                {
                    compare.LayerB = static_cast<u32>(std::max<long long>(0, c["layerB"].get<long long>()));
                    compare.HasLayerB = true;
                }
                if (c.contains("afterPass") && c["afterPass"].is_string())
                    compare.AfterPass = c["afterPass"].get<std::string>();
            }

            if (args.value("forceFrame", false) && compare.AfterPass.empty())
                (void)ForceFreshFrame(server, /*settleFrames*/ 2);

            // Both compare sides are snapshotted by the SAME hook firing, so
            // the bitwise verdict describes one consistent frame.
            bool afterPassFrameRendered = true;
            if (hasCompare && !compare.AfterPass.empty())
            {
                if (const std::string error =
                        ArmAfterPassSnapshot(server, compare.AfterPass, { compare.A, compare.B },
                                             afterPassFrameRendered);
                    !error.empty())
                    return ToolResult::Error(error);
            }

            const Json result = server.MarshalRead([&server, hasCompare, compare, afterPassFrameRendered]() -> Json
                                                   {
                const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph();
                if (!graph)
                    return Json{ { "__error", "No active render graph (the editor is not in 3D mode, or no frame has been rendered yet)." } };

                // The graph getters below are const but ValidateCompiledResourceHazards
                // is not — the same confined const_cast the debugger's capture uses.
                auto& mutableGraph = *const_cast<RenderGraph*>(graph.Raw());

                std::vector<RenderValidate::HazardInfo> hazards;
                for (const auto& hazard : mutableGraph.ValidateCompiledResourceHazards())
                {
                    hazards.push_back(RenderValidate::HazardInfo{ HazardKindName(hazard.Kind), hazard.Resource,
                                                                  hazard.Producer, hazard.Consumer, hazard.Message });
                }

                std::vector<RenderValidate::DiagnosticInfo> barrierDiagnostics;
                for (const auto& diagnostic : graph->GetBarrierDiagnostics())
                {
                    barrierDiagnostics.push_back(RenderValidate::DiagnosticInfo{
                        BarrierDiagnosticKindName(diagnostic.Kind), diagnostic.PassName, diagnostic.Resource,
                        diagnostic.Message });
                }

                std::vector<RenderValidate::DiagnosticInfo> buildDiagnostics;
                for (const auto& diagnostic : graph->GetBuildDiagnostics())
                {
                    buildDiagnostics.push_back(RenderValidate::DiagnosticInfo{
                        "RegistrationOrderSensitivity", std::string{}, diagnostic.Resource, diagnostic.Message });
                }

                std::vector<RenderValidate::ResolveFailureInfo> resolveFailures;
                for (const auto& failure : graph->GetResolveFailures())
                {
                    resolveFailures.push_back(
                        RenderValidate::ResolveFailureInfo{ failure.PassName, failure.Reason, failure.Count });
                }

                std::vector<RenderValidate::ResourceIdentity> identities;
                for (const auto& resource : graph->GetRegisteredResources())
                {
                    RenderValidate::ResourceIdentity identity;
                    identity.Name = resource.Name;

                    // BOTH currencies, and the BACKING verdict comes from the
                    // identity leg (issue #890, ADR 0011 amendment (90)). The
                    // old code decided `consumedButUnbacked` from a GL-shaped
                    // u32. Measured live, that was wrong on BOTH backends in
                    // opposite directions: on Vulkan it reported 11 readable,
                    // scene-content-carrying resources as unbacked (a native 0
                    // is what every framebuffer attachment reports there, by
                    // design), and on OpenGL it MISSED two genuinely storage-
                    // less resources carrying recycled non-zero GL names.
                    if (resource.TextureHandle.IsValid())
                    {
                        const RHI::ResourceHandle texture = graph->ResolveTextureHandle(resource.TextureHandle);
                        identity.NativeTextureHandle =
                            Debug::NativeHandleForDiagnostics(*graph, resource.TextureHandle);
                        identity.TextureIdentity = RHI::HashKey(texture);
                        identity.TextureHasStorage =
                            Debug::HasLiveTextureStorage(*graph, resource.TextureHandle);
                    }
                    else if (resource.FramebufferHandle.IsValid())
                    {
                        // A framebuffer-backed resource is asked the same
                        // question through its attachment, which is where the
                        // identity lives. This is the branch that produced all
                        // 11 of the Vulkan false positives.
                        bool depthFromFramebuffer = false;
                        const RHI::ResourceHandle attachment =
                            ResolveTargetHandle(resource.Name, depthFromFramebuffer);
                        identity.NativeTextureHandle = Debug::NativeHandleForDiagnostics(attachment);
                        identity.TextureIdentity = RHI::HashKey(attachment);
                        identity.TextureHasStorage = Debug::HasLiveTextureStorage(attachment);
                    }
                    if (resource.BufferHandle.IsValid())
                    {
                        // Same as the topology export: the identity carries the
                        // real native handle, ResolveBuffer's u32 is 0 on Vulkan.
                        const RHI::ResourceHandle buffer = graph->ResolveBufferHandle(resource.BufferHandle);
                        identity.NativeBufferHandle =
                            buffer.IsValid() ? Debug::NativeHandleForDiagnostics(buffer)
                                             : static_cast<u64>(graph->ResolveBuffer(resource.BufferHandle));
                        identity.BufferIdentity = RHI::HashKey(buffer);
                    }
                    identity.HasProducers = !resource.Producers.empty();
                    identity.HasConsumers = !resource.Consumers.empty();
                    identity.LastWriter = graph->GetLastWriterPassName(resource.Name);
                    identities.push_back(std::move(identity));
                }

                Json j = RenderValidate::BuildValidateJson(hazards, barrierDiagnostics, buildDiagnostics,
                                                           resolveFailures, identities);

                if (hasCompare)
                {
                    RenderValidate::CompareResult compareResult;
                    RHI::ResourceHandle textureA;
                    RHI::ResourceHandle textureB;
                    if (!compare.AfterPass.empty())
                    {
                        // Both results come from the one armed snapshot; read
                        // them here and disarm exactly once.
                        auto& snapshot = RenderGraphDebugRuntime::GetPassSnapshot();
                        if (snapshot.GetPassName() != compare.AfterPass)
                        {
                            compareResult.Error = "Another tool call re-armed the afterPass snapshot concurrently "
                                                  "(now armed for pass '" + snapshot.GetPassName() +
                                                  "'). afterPass requests are one-at-a-time; retry.";
                        }
                        else if (snapshot.IsPending())
                        {
                            snapshot.Disarm();
                            compareResult.Error =
                                !afterPassFrameRendered
                                    ? "Timed out waiting for a frame to render after arming the afterPass "
                                      "snapshot (viewport render-throttled, editor stalled, or cancelled)."
                                    : "Pass '" + compare.AfterPass +
                                          "' did not execute this frame (culled or disabled).";
                        }
                        else
                        {
                            for (const auto& snapshotResult : snapshot.GetResults())
                            {
                                if (!snapshotResult.Captured)
                                {
                                    compareResult.Error = snapshotResult.Error;
                                    continue;
                                }
                                if (snapshotResult.ResourceName == compare.A)
                                    textureA = snapshotResult.Handle;
                                if (snapshotResult.ResourceName == compare.B)
                                    textureB = snapshotResult.Handle;
                            }
                            snapshot.Disarm();
                        }
                    }
                    else
                    {
                        bool depthFromFramebuffer = false;
                        textureA = ResolveTargetHandle(compare.A, depthFromFramebuffer);
                        textureB = ResolveTargetHandle(compare.B, depthFromFramebuffer);
                    }

                    // Per-side layer selection, same rule as capture/probe/
                    // stats: an explicit layerA/layerB is validated, and a
                    // layer-VIEW resource (ShadowMapCSMCascade3) defaults to
                    // ITS OWN layer — reading z=0 unconditionally would
                    // silently compare cascade 0 vs cascade 0. The snapshot
                    // clone preserves every layer, so the same selection
                    // applies on the afterPass path.
                    u32 layerA = compare.LayerA;
                    u32 layerB = compare.LayerB;
                    if (compareResult.Error.empty())
                    {
                        const auto selectLayer = [&graph](const std::string& targetName, bool hasLayer,
                                                          u32 requestedLayer, u32& outLayer) -> std::string
                        {
                            const CaptureLayer::TargetLayers layers = ResolveTargetLayers(*graph, targetName);
                            const CaptureLayer::Selection selection = CaptureLayer::SelectLayer(
                                layers, targetName, hasLayer, static_cast<long long>(requestedLayer));
                            if (!selection.Error.empty())
                                return selection.Error;
                            outLayer = selection.Layer;
                            return {};
                        };
                        if (const std::string error = selectLayer(compare.A, compare.HasLayerA, compare.LayerA, layerA);
                            !error.empty())
                            compareResult.Error = error;
                        else if (const std::string error =
                                     selectLayer(compare.B, compare.HasLayerB, compare.LayerB, layerB);
                                 !error.empty())
                            compareResult.Error = error;
                    }

                    if (compareResult.Error.empty())
                    {
                        if (!textureA.IsValid() || !textureB.IsValid())
                        {
                            compareResult.Error = std::string("Unknown compare target '") +
                                                  (!textureA.IsValid() ? compare.A : compare.B) +
                                                  "' (or it has no GPU backing). Call olo_render_list_targets.";
                        }
                        else
                        {
                            u32 widthA = 0;
                            u32 heightA = 0;
                            u32 widthB = 0;
                            u32 heightB = 0;
                            std::string errorA;
                            std::string errorB;
                            const std::vector<f32> a = ReadChannel0TopLeft(textureA, compare.MipA, layerA,
                                                                           widthA, heightA, compareResult.FormatA,
                                                                           errorA);
                            const std::vector<f32> b = ReadChannel0TopLeft(textureB, compare.MipB, layerB,
                                                                           widthB, heightB, compareResult.FormatB,
                                                                           errorB);
                            if (!errorA.empty())
                                compareResult.Error = "'" + compare.A + "': " + errorA;
                            else if (!errorB.empty())
                                compareResult.Error = "'" + compare.B + "': " + errorB;
                            else
                            {
                                const std::string formatA = compareResult.FormatA;
                                const std::string formatB = compareResult.FormatB;
                                compareResult = RenderValidate::CompareFloatBuffers(a, widthA, heightA,
                                                                                    b, widthB, heightB);
                                compareResult.FormatA = formatA;
                                compareResult.FormatB = formatB;
                            }
                        }
                    }
                    // Echo the RESOLVED layers (a view's own layer may have
                    // been applied) so the reply states which layers were
                    // actually compared.
                    RenderValidate::CompareRequest compareEcho = compare;
                    compareEcho.LayerA = layerA;
                    compareEcho.LayerB = layerB;
                    j["compare"] = RenderValidate::CompareResultJson(compareEcho, compareResult);
                    if (!compareResult.Error.empty())
                        j["ok"] = false;
                }

                j["meta"] = CaptureStampJson(server.Context().GetFrameIndex ? server.Context().GetFrameIndex() : 0, server.Context());
                return j; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Structured(result);
        }

        // ---- olo_virtual_geometry_set / _stats (main-marshaled) ----------------
        // The Nanite-style virtualized-geometry debug surface (issue #629) was
        // reachable ONLY from the Statistics panel's ImGui combo — an agent could
        // neither flip the cluster/LOD/overdraw visualisation nor read the cull
        // counters, so verifying the GPU cull meant adding a one-off test.

        const char* VirtualDebugModeToken(VirtualDebugMode mode)
        {
            switch (mode)
            {
                case VirtualDebugMode::Off:
                    return "off";
                case VirtualDebugMode::ClusterId:
                    return "clusterid";
                case VirtualDebugMode::Lod:
                    return "lod";
                case VirtualDebugMode::Overdraw:
                    return "overdraw";
            }
            return "off";
        }

        bool ParseVirtualDebugMode(const std::string& token, VirtualDebugMode& out)
        {
            if (token == "off")
                out = VirtualDebugMode::Off;
            else if (token == "clusterid")
                out = VirtualDebugMode::ClusterId;
            else if (token == "lod")
                out = VirtualDebugMode::Lod;
            else if (token == "overdraw")
                out = VirtualDebugMode::Overdraw;
            else
                return false;
            return true;
        }

        // (main thread) THE single write path for the virtualized-geometry debug
        // mode. Both olo_virtual_geometry_set { debugMode } and
        // olo_render_set_debug_view { mode: 'vgclusterid' | 'vglod' | 'vgoverdraw' }
        // go through here, so the two tools can never disagree about the current
        // state or drift in what a mode change actually does.
        void ApplyVirtualDebugMode(VirtualDebugMode mode)
        {
            VirtualMeshRegistry::Get().SetDebugMode(mode);
        }

        const char* VirtualSwRasterModeToken(VirtualSwRasterMode mode)
        {
            switch (mode)
            {
                case VirtualSwRasterMode::Auto:
                    return "auto";
                case VirtualSwRasterMode::ForceSoftware:
                    return "forcesoftware";
                case VirtualSwRasterMode::Disabled:
                    return "disabled";
            }
            return "auto";
        }

        bool ParseVirtualSwRasterMode(const std::string& token, VirtualSwRasterMode& out)
        {
            if (token == "auto")
                out = VirtualSwRasterMode::Auto;
            else if (token == "forcesoftware")
                out = VirtualSwRasterMode::ForceSoftware;
            else if (token == "disabled")
                out = VirtualSwRasterMode::Disabled;
            else
                return false;
            return true;
        }

        const char* VirtualHwRasterModeToken(VirtualHwRasterMode mode)
        {
            switch (mode)
            {
                case VirtualHwRasterMode::Auto:
                    return "auto";
                case VirtualHwRasterMode::ForceMdi:
                    return "forcemdi";
            }
            return "auto";
        }

        bool ParseVirtualHwRasterMode(const std::string& token, VirtualHwRasterMode& out)
        {
            if (token == "auto")
                out = VirtualHwRasterMode::Auto;
            else if (token == "forcemdi")
                out = VirtualHwRasterMode::ForceMdi;
            else
                return false;
            return true;
        }

        // (main thread) The live knob state, echoed by both virtual-geometry tools.
        Json VirtualGeometrySettingsJson()
        {
            const auto& registry = VirtualMeshRegistry::Get();
            Json j;
            const auto& settings = Renderer3D::GetRendererSettings();
            j["enabled"] = settings.VirtualGeometryEnabled;
            j["debugToViewport"] = settings.VirtualDebugToViewport;
            j["debugMode"] = VirtualDebugModeToken(registry.GetDebugMode());
            j["swRasterMode"] = VirtualSwRasterModeToken(registry.GetSwRasterMode());
            j["swRasterThresholdPixels"] = registry.GetSwRasterThresholdPixels();
            j["forcePortableSwRaster"] = registry.GetForcePortableSwRaster();
            // Mesh-shader hardware raster path (#813). `meshShadersSupported` is
            // the raw DEVICE capability; `meshRasterAvailable` is the pass's
            // EFFECTIVE availability (device capability AND the meshlet shader
            // compiled — published by VirtualGeometryPass::Init). An A/B must
            // key on the latter, or a compile-failure demotion silently turns
            // the comparison into MDI-vs-MDI.
            j["hwRasterMode"] = VirtualHwRasterModeToken(registry.GetHwRasterMode());
            j["meshShadersSupported"] = RenderCommand::SupportsMeshShaders();
            j["meshRasterAvailable"] = registry.GetMeshRasterAvailable();
            j["debugTargetAvailable"] = registry.GetDebugColorTexture().IsValid();
            return j;
        }

        // The outputSchema sub-shape matching VirtualGeometrySettingsJson above,
        // shared by both virtual-geometry tools (previous/current/settings).
        Schema::Node VirtualGeometrySettingsSchema()
        {
            return Schema::Object()
                .Prop("enabled", Schema::Bool())
                .Prop("debugToViewport", Schema::Bool())
                .Prop("debugMode", Schema::String().Enum({ "off", "clusterid", "lod", "overdraw" }))
                .Prop("swRasterMode", Schema::String().Enum({ "auto", "forcesoftware", "disabled" }))
                .Prop("swRasterThresholdPixels", Schema::Number())
                .Prop("forcePortableSwRaster", Schema::Bool())
                .Prop("hwRasterMode", Schema::String().Enum({ "auto", "forcemdi" }).Desc("How hardware-routed clusters draw: 'auto' = mesh-shader pipeline where supported, 'forcemdi' = classic MDI (the A/B lever)."))
                .Prop("meshShadersSupported", Schema::Bool().Desc("Device/backend exposes VK_EXT_mesh_shader (task+mesh). False on OpenGL."))
                .Prop("meshRasterAvailable", Schema::Bool().Desc("The pass's EFFECTIVE mesh-raster availability: device capability AND the meshlet shader compiled. Key any mesh-vs-MDI A/B on THIS, not on meshShadersSupported."))
                .Prop("debugTargetAvailable", Schema::Bool().Desc("True when the 'VirtualGeometryDebug' target has GPU backing this frame."));
        }

        ToolResult Handle_VirtualGeometrySet(McpServer& server, const Json& args)
        {
            const bool hasDebugMode = args.contains("debugMode") && args["debugMode"].is_string();
            const bool hasSwRasterMode = args.contains("swRasterMode") && args["swRasterMode"].is_string();
            const bool hasHwRasterMode = args.contains("hwRasterMode") && args["hwRasterMode"].is_string();
            const bool hasThreshold = args.contains("swRasterThresholdPixels") && args["swRasterThresholdPixels"].is_number();
            const bool hasForcePortable = args.contains("forcePortableSwRaster") && args["forcePortableSwRaster"].is_boolean();
            const bool hasEnabled = args.contains("enabled") && args["enabled"].is_boolean();
            const bool hasDebugToViewport = args.contains("debugToViewport") && args["debugToViewport"].is_boolean();

            VirtualDebugMode debugMode{};
            if (hasDebugMode && !ParseVirtualDebugMode(args["debugMode"].get<std::string>(), debugMode))
                return ToolResult::Error("Unknown 'debugMode'. Valid: off, clusterid, lod, overdraw.");

            VirtualSwRasterMode swRasterMode{};
            if (hasSwRasterMode && !ParseVirtualSwRasterMode(args["swRasterMode"].get<std::string>(), swRasterMode))
                return ToolResult::Error("Unknown 'swRasterMode'. Valid: auto, forcesoftware, disabled.");

            VirtualHwRasterMode hwRasterMode{};
            if (hasHwRasterMode && !ParseVirtualHwRasterMode(args["hwRasterMode"].get<std::string>(), hwRasterMode))
                return ToolResult::Error("Unknown 'hwRasterMode'. Valid: auto, forcemdi.");

            f32 threshold = 0.0f;
            if (hasThreshold)
            {
                threshold = args["swRasterThresholdPixels"].get<f32>();
                if (!std::isfinite(threshold) || threshold < 0.0f || threshold > 4096.0f)
                    return ToolResult::Error("Invalid 'swRasterThresholdPixels': expected a finite number in [0, 4096].");
            }

            const bool anyChange =
                hasDebugMode || hasSwRasterMode || hasHwRasterMode || hasThreshold || hasForcePortable || hasEnabled || hasDebugToViewport;
            const bool forcePortable = hasForcePortable && args["forcePortableSwRaster"].get<bool>();
            const bool enabled = hasEnabled && args["enabled"].get<bool>();
            const bool debugToViewport = hasDebugToViewport && args["debugToViewport"].get<bool>();

            const Json applied = server.MarshalRead(
                [hasDebugMode, debugMode, hasSwRasterMode, swRasterMode, hasHwRasterMode, hwRasterMode,
                 hasThreshold, threshold,
                 hasForcePortable, forcePortable, hasEnabled, enabled, hasDebugToViewport, debugToViewport]() -> Json
                {
                    auto& registry = VirtualMeshRegistry::Get();
                    Json previous = VirtualGeometrySettingsJson();
                    if (hasDebugMode)
                        ApplyVirtualDebugMode(debugMode); // shared with olo_render_set_debug_view's vg* modes
                    if (hasSwRasterMode)
                        registry.SetSwRasterMode(swRasterMode);
                    if (hasHwRasterMode)
                        registry.SetHwRasterMode(hwRasterMode);
                    if (hasThreshold)
                        registry.SetSwRasterThresholdPixels(threshold);
                    if (hasForcePortable)
                        registry.SetForcePortableSwRaster(forcePortable);

                    // The master switch and the viewport-overlay toggle live on RendererSettings,
                    // not the registry — `enabled` changes which SUBMISSION path Scene.cpp takes
                    // (virtual vs classic), which is a scene-level decision, not a registry one.
                    if (hasEnabled || hasDebugToViewport)
                    {
                        auto& rs = Renderer3D::GetRendererSettings();
                        if (hasEnabled)
                            rs.VirtualGeometryEnabled = enabled;
                        if (hasDebugToViewport)
                            rs.VirtualDebugToViewport = debugToViewport;
                        Renderer3D::ApplyRendererSettings();
                    }
                    return Json{ { "previous", std::move(previous) } };
                });

            // A debug-mode change gates a render-graph DECLARATION
            // (VirtualGeometryPass::Setup ImportTexture()s "VirtualGeometryDebug"
            // only while a mode is on). The blackboard fingerprint hashes the mode
            // + the debug texture id, so the next EndScene rebuilds the topology —
            // but the caller must not race it: settle a couple of frames here so
            // the target really IS capturable by the time this call returns and an
            // immediately-following olo_render_capture_target succeeds instead of
            // answering "Unknown render-graph resource".
            if (anyChange && server.Context().GetFrameIndex)
                (void)ForceFreshFrame(server, kVirtualDebugSettleFrames);

            const Json current = server.MarshalRead([]() -> Json
                                                    { return VirtualGeometrySettingsJson(); });

            Json j;
            j["changed"] = anyChange;
            j["previous"] = applied.value("previous", Json::object());
            j["current"] = current;
            if (hasDebugMode && debugMode != VirtualDebugMode::Off)
            {
                j["captureTarget"] = "VirtualGeometryDebug";
                j["message"] = current.value("debugTargetAvailable", false)
                                   ? "Debug visualization on — capture it with olo_render_capture_target "
                                     "{ name: 'VirtualGeometryDebug' }."
                                   : "Debug visualization requested, but the target is not backed yet. It only "
                                     "exists on the Deferred rendering path with at least one VirtualMeshComponent "
                                     "in view — check olo_renderer_settings_set { setting: 'renderpath' } and "
                                     "olo_virtual_geometry_stats.";
            }
            return ToolResult::Structured(j);
        }

        // ---- olo_shader_debug_draw (main-marshaled) ----------------------------
        // The agent-facing half of issue #725. The whole point of GPU-pushable
        // debug draws is that a cull decision computed on the GPU becomes visible
        // without a code edit + reload; an agent that cannot flip the switch and
        // read the overflow counters is back to the debug-colour-output loop.

        // (main thread) The live knob + per-channel state, echoed by the tool.
        Json ShaderDebugDrawStateJson()
        {
            const auto& settings = Renderer3D::GetRendererSettings();
            Json j;
            j["enabled"] = settings.ShaderDebugDrawEnabled;
            j["lineWidth"] = settings.ShaderDebugDrawLineWidth;
            j["clusterBounds"] = settings.ShaderDebugDrawClusterBounds;
            j["clusterStride"] = settings.ShaderDebugDrawClusterStride;

            // Channel stats are from the PREVIOUS frame by construction: the
            // headers are copied into DeviceToHost staging at the end of the
            // debug pass and read at the next BeginFrame, so the read never
            // stalls on this frame's GPU work. For an overflow flag that latency
            // does not matter; saying so here stops a caller reading a stale zero
            // as "nothing was pushed".
            const auto& stats = ShaderDebugDraw::GetStats();
            j["statsValid"] = stats.StatsValid;
            j["statsAreFromPreviousFrame"] = true;
            Json channels = Json::array();
            for (u32 i = 0; i < kShaderDebugDrawPrimitiveCount; ++i)
            {
                const auto& channel = stats.Channels[i];
                channels.push_back(Json{
                    { "primitive", ShaderDebugDrawContract::Name(static_cast<ShaderDebugDrawPrimitive>(i)) },
                    { "capacity", channel.Capacity },
                    { "drawn", channel.Drawn },
                    { "requested", channel.Requested },
                    { "cpuPushes", channel.CpuPushes },
                    { "overflowed", channel.Overflowed() },
                    { "dropped", channel.Dropped() },
                });
            }
            j["channels"] = std::move(channels);
            j["anyOverflow"] = stats.AnyOverflow();
            return j;
        }

        Schema::Node ShaderDebugDrawStateSchema()
        {
            return Schema::Object()
                .Prop("enabled", Schema::Bool())
                .Prop("lineWidth", Schema::Number())
                .Prop("clusterBounds", Schema::Int())
                .Prop("clusterStride", Schema::Int())
                .Prop("statsValid", Schema::Bool())
                .Prop("statsAreFromPreviousFrame", Schema::Bool())
                .Prop("anyOverflow", Schema::Bool())
                .Prop("channels", Schema::Array(Schema::Object()
                                                    .Prop("primitive", Schema::String())
                                                    .Prop("capacity", Schema::Int())
                                                    .Prop("drawn", Schema::Int())
                                                    .Prop("requested", Schema::Int())
                                                    .Prop("cpuPushes", Schema::Int())
                                                    .Prop("overflowed", Schema::Bool())
                                                    .Prop("dropped", Schema::Int())));
        }

        ToolResult Handle_ShaderDebugDrawSet(McpServer& server, const Json& args)
        {
            const bool hasEnabled = args.contains("enabled") && args["enabled"].is_boolean();
            const bool hasLineWidth = args.contains("lineWidth") && args["lineWidth"].is_number();
            const bool hasClusterBounds = args.contains("clusterBounds") && args["clusterBounds"].is_number_integer();
            const bool hasClusterStride = args.contains("clusterStride") && args["clusterStride"].is_number_integer();

            f32 lineWidth = 0.0f;
            if (hasLineWidth)
            {
                lineWidth = args["lineWidth"].get<f32>();
                if (!std::isfinite(lineWidth) || lineWidth < 1.0f || lineWidth > 32.0f)
                    return ToolResult::Error("Invalid 'lineWidth': expected a finite number in [1, 32].");
            }

            i64 clusterBounds = 0;
            if (hasClusterBounds)
            {
                clusterBounds = args["clusterBounds"].get<i64>();
                if (clusterBounds < 0 || clusterBounds > 15)
                    return ToolResult::Error("Invalid 'clusterBounds': expected a bit field in [0, 15] "
                                             "(1 drawn | 2 frustum-culled | 4 cone-culled | 8 Hi-Z occluded).");
            }

            i64 clusterStride = 0;
            if (hasClusterStride)
            {
                clusterStride = args["clusterStride"].get<i64>();
                if (clusterStride < 1 || clusterStride > 4096)
                    return ToolResult::Error("Invalid 'clusterStride': expected an integer in [1, 4096].");
            }

            const bool enabled = hasEnabled && args["enabled"].get<bool>();
            const bool anyChange = hasEnabled || hasLineWidth || hasClusterBounds || hasClusterStride;

            const Json applied = server.MarshalRead(
                [hasEnabled, enabled, hasLineWidth, lineWidth, hasClusterBounds, clusterBounds, hasClusterStride,
                 clusterStride]() -> Json
                {
                    Json previous = ShaderDebugDrawStateJson();
                    auto& rs = Renderer3D::GetRendererSettings();
                    if (hasEnabled)
                        rs.ShaderDebugDrawEnabled = enabled;
                    if (hasLineWidth)
                        rs.ShaderDebugDrawLineWidth = lineWidth;
                    if (hasClusterBounds)
                        rs.ShaderDebugDrawClusterBounds = static_cast<u32>(clusterBounds);
                    if (hasClusterStride)
                        rs.ShaderDebugDrawClusterStride = static_cast<u32>(clusterStride);
                    return Json{ { "previous", std::move(previous) } };
                });

            // The enable gates a render-graph DECLARATION (the pass declares its
            // SceneColor RMW only while on) AND the channel capacities the GLSL
            // push helpers test, and the stats are a frame behind on top of that.
            // Settle so the state this call reports back is the one a screenshot
            // taken immediately afterwards will show.
            if (anyChange && server.Context().GetFrameIndex)
                (void)ForceFreshFrame(server, kVirtualDebugSettleFrames);

            const Json current = server.MarshalRead([]() -> Json
                                                    { return ShaderDebugDrawStateJson(); });

            Json j;
            j["changed"] = anyChange;
            j["previous"] = applied.value("previous", Json::object());
            j["current"] = current;
            if (current.value("anyOverflow", false))
            {
                j["message"] = "At least one channel OVERFLOWED - the excess draws were dropped, so what you "
                               "see is a prefix, not the whole set. Raise clusterStride, or expect an "
                               "arbitrary subset.";
            }
            else if (current.value("enabled", false))
            {
                j["message"] = "Shader debug draws are on. Push from a shader via "
                               "include/DebugDrawCommon.glsl, or set clusterBounds for the shipped "
                               "virtual-geometry cluster visualization (Deferred path only). Capture with "
                               "olo_screenshot { forceFrame: true }.";
            }
            return ToolResult::Structured(j);
        }

        ToolResult Handle_VirtualGeometryStats(McpServer& server, const Json& /*args*/)
        {
            const Json result = server.MarshalRead([]() -> Json
                                                   {
                auto& registry = VirtualMeshRegistry::Get();

                // A small blocking GPU readback of the cull args buffer (staged
                // through a GL_DYNAMIC_READ copy inside ReadFrameCullStats — never
                // read the DYNAMIC_COPY args buffer directly, see the comment there).
                const VirtualCullStats cull = registry.ReadFrameCullStats();
                const VirtualResidencyStats& residency = registry.GetResidencyStats();

                Json cullJson;
                cullJson["instances"] = cull.InstanceCount;
                cullJson["testedClusters"] = cull.TestedClusters;
                cullJson["cutSelected"] = cull.CutSelected;
                cullJson["hardwareDraws"] = cull.HardwareDraws;
                cullJson["softwareRasterized"] = cull.SoftwareRasterized;
                cullJson["drawnClusters"] = cull.DrawnClusters();
                // Two-phase occlusion (issue #682): clusters phase 1 found hidden
                // by the PREVIOUS frame's depth pyramid that phase 2 recovered
                // against this frame's. Already included in the counts above.
                cullJson["phase2Recovered"] = cull.Phase2Recovered;

                Json residencyJson;
                residencyJson["totalPages"] = residency.TotalPages;
                residencyJson["residentPages"] = residency.ResidentPages;
                residencyJson["pinnedPages"] = residency.PinnedPages;
                residencyJson["budgetSlots"] = residency.BudgetSlots;
                residencyJson["budget"] = residency.BudgetSlots == 0 ? "unbounded (eager)" : "budgeted";
                residencyJson["pageUploads"] = residency.PageUploads;
                residencyJson["pageEvictions"] = residency.PageEvictions;

                Json j;
                j["renderingPath"] = RenderingPathName(Renderer3D::GetRendererSettings().Path);
                j["frameInstances"] = static_cast<u32>(registry.GetFrameInstances().size());
                j["frameClusters"] = registry.GetTotalFrameClusterCount();
                j["cull"] = std::move(cullJson);
                j["residency"] = std::move(residencyJson);
                j["settings"] = VirtualGeometrySettingsJson();
                // Explain a zero rather than guess at it (issue #864). The old
                // "no VirtualMeshComponent in the scene, or all of them are
                // disabled" note was actively MISLEADING on the one case that
                // matters: a scene full of VirtualMeshComponents whose mesh
                // assets did not load reads exactly zero here, and that note
                // sent the reader looking for a scene-authoring mistake that
                // does not exist. The submission loop now records WHY, so say so.
                const auto& diagnostics = registry.GetSubmissionDiagnostics();
                j["diagnostics"] = Json{
                    { "enabledComponents", diagnostics.EnabledComponents },
                    { "unresolvedAssets", diagnostics.UnresolvedAssets },
                    { "registrationFailures", diagnostics.RegistrationFailures },
                    { "submitted", diagnostics.Submitted },
                    { "fellBackToClassic", diagnostics.FellBackToClassic },
                    { "silentlyDrewNothing", diagnostics.SilentlyDrewNothing() },
                };

                if (Renderer3D::GetRendererSettings().Path != RenderingPath::Deferred)
                    j["note"] = "Virtual geometry only renders on the Deferred path; the scene does not submit "
                                "VirtualMeshComponents on Forward/Forward+, so every counter reads zero.";
                else if (diagnostics.SilentlyDrewNothing())
                    j["note"] = "WARNING: this scene contains " + std::to_string(diagnostics.EnabledComponents) +
                                " enabled VirtualMeshComponent(s) but submitted ZERO virtual-geometry instances (" +
                                std::to_string(diagnostics.UnresolvedAssets) +
                                " mesh-source asset(s) did not load, " +
                                std::to_string(diagnostics.RegistrationFailures) +
                                " failed to build a cluster DAG). Every counter below reads zero for that reason, "
                                "NOT because virtual geometry is working and finding nothing to draw. A missing "
                                "asset is usually a fetch-on-demand one — run scripts/Fetch-Assets.ps1 (see "
                                "scripts/assets/asset-manifest.json) — otherwise check OloEngine.log. Do NOT treat "
                                "an A/B or performance measurement taken on this scene as meaningful: it will pass "
                                "vacuously because nothing draws in either mode.";
                else if (diagnostics.FellBackToClassic && diagnostics.EnabledComponents > 0)
                    j["note"] = "The virtual-geometry master switch is OFF, so this scene's " +
                                std::to_string(diagnostics.EnabledComponents) +
                                " VirtualMeshComponent(s) were drawn through the CLASSIC mesh path instead. Zero "
                                "counters are expected here — this is the intended A/B baseline, not a fault.";
                else if (registry.GetFrameInstances().empty())
                    j["note"] = "No virtual-mesh instances were submitted this frame: the scene contains no "
                                "VirtualMeshComponent, or every one of them is disabled or has no mesh assigned.";
                return j; });
            return ToolResult::Structured(result);
        }

        // ---- olo_terrain_virtual_texture_stats (main-marshaled) ----------------
        // The terrain VT loop's own instrumentation (issue #715), verbatim. A
        // thrashing cache and a converged one render the same frame, so the
        // counters — not the pixels — are what separate "bake budget too small"
        // from "cache too small" from "a loop that never converges". Pure read of
        // the CPU-side Stats struct Update() already maintains: no GPU readback
        // anywhere in this handler.

        // Every field of TerrainVirtualTexture::Stats, m_-stripped to camelCase.
        // Verbatim on purpose: the struct IS the diagnostic surface, and a field
        // dropped here is a counter an agent can never ask for.
        Json TerrainVirtualTextureStatsJson(const TerrainVirtualTexture::Stats& stats)
        {
            Json j;
            j["cacheTileCount"] = stats.m_CacheTileCount;
            j["residentTiles"] = stats.m_ResidentTiles;
            j["pagesRequested"] = stats.m_PagesRequested;
            j["feedbackTexelsWritten"] = stats.m_FeedbackTexelsWritten;
            j["tilesBakedThisFrame"] = stats.m_TilesBakedThisFrame;
            j["tilesBakedTotal"] = stats.m_TilesBakedTotal;
            j["evictionsTotal"] = stats.m_EvictionsTotal;
            j["budgetStarvedRequests"] = stats.m_BudgetStarvedRequests;
            j["workingSetExceedsCache"] = stats.m_WorkingSetExceedsCache;
            j["readbackSlotsInFlight"] = stats.m_ReadbackSlotsInFlight;
            j["cacheBytes"] = stats.m_CacheBytes;
            j["indirectionBytes"] = stats.m_IndirectionBytes;
            j["indirectionTexelsWritten"] = stats.m_IndirectionTexelsWritten;
            j["indirectionTexelsFilled"] = stats.m_IndirectionTexelsFilled;
            j["indirectionPublishes"] = stats.m_IndirectionPublishes;
            j["indirectionFullRebuilds"] = stats.m_IndirectionFullRebuilds;
            j["framesUpdated"] = stats.m_FramesUpdated;
            j["indirectionRebuildGpuMs"] = stats.m_IndirectionRebuildGpuMs;
            j["indirectionDeltaGpuMs"] = stats.m_IndirectionDeltaGpuMs;
            j["sectorCount"] = stats.m_SectorCount;
            j["sectorsReady"] = stats.m_SectorsReady;
            j["imageResizesTotal"] = stats.m_ImageResizesTotal;
            j["pagesRemappedTotal"] = stats.m_PagesRemappedTotal;
            j["pagesDroppedOnShrink"] = stats.m_PagesDroppedOnShrink;
            j["staleFeedbackTexels"] = stats.m_StaleFeedbackTexels;
            j["atlasPagesAllocated"] = stats.m_AtlasPagesAllocated;
            j["imageAllocFailures"] = stats.m_ImageAllocFailures;
            j["cacheCompressed"] = stats.m_CacheCompressed;
            j["tilesCompressedTotal"] = stats.m_TilesCompressedTotal;
            return j;
        }

        // The config knobs that give the counters their denominators (a
        // residentTiles without a cacheTileCount ceiling, or a tilesBakedThisFrame
        // without maxTileBakesPerFrame, is a number with no meaning).
        Json TerrainVirtualTextureConfigJson(const TerrainVirtualTextureConfig& config)
        {
            Json j;
            j["virtualPagesWide"] = config.VirtualPagesWide;
            j["pageTexels"] = config.PageTexels;
            j["borderTexels"] = config.BorderTexels;
            j["cacheTilesWide"] = config.CacheTilesWide;
            j["maxTileBakesPerFrame"] = config.MaxTileBakesPerFrame;
            j["adaptiveEnabled"] = config.AdaptiveEnabled;
            j["sectorsWide"] = config.SectorsWide;
            j["minImagePagesWide"] = config.MinImagePagesWide;
            j["maxImagePagesWide"] = config.MaxImagePagesWide;
            j["trilinearEnabled"] = config.TrilinearEnabled;
            j["compressedCache"] = config.CompressedCache;
            return j;
        }

        ToolResult Handle_TerrainVirtualTextureStats(McpServer& server, const Json& /*args*/)
        {
            const Json result = server.MarshalRead([&server]() -> Json
                                                   {
                const Ref<Scene> scene = server.Context().GetActiveScene ? server.Context().GetActiveScene() : nullptr;
                if (!scene)
                    return Json{ { "__error", "No active scene." } };

                u32 terrainEntities = 0;
                Json terrains = Json::array();
                for (const auto handle : scene->GetAllEntitiesWith<TerrainComponent>())
                {
                    Entity entity{ handle, scene.get() };
                    ++terrainEntities;
                    const auto& terrain = entity.GetComponent<TerrainComponent>();
                    // The VT object is created lazily on the first frame it is
                    // enabled AND the terrain renders; until then there is no
                    // Stats to report, so the terrain is counted but not listed.
                    if (!terrain.m_VirtualTexture)
                        continue;

                    Json t;
                    t["entity"] = UuidToString(entity.GetUUID());
                    t["name"] = entity.HasComponent<TagComponent>() ? entity.GetComponent<TagComponent>().Tag
                                                                    : std::string{};
                    t["readyForShading"] = terrain.m_VirtualTexture->IsReadyForShading();
                    t["stats"] = TerrainVirtualTextureStatsJson(terrain.m_VirtualTexture->GetStats());
                    t["config"] = TerrainVirtualTextureConfigJson(terrain.m_VirtualTexture->GetConfig());
                    terrains.push_back(std::move(t));
                }

                Json j;
                j["terrainEntities"] = terrainEntities;
                if (terrains.empty())
                {
                    j["note"] = terrainEntities == 0
                                    ? "No TerrainComponent in the active scene."
                                    : "No terrain has a live virtual texture. The VT is created lazily on the "
                                      "first frame VirtualTextureEnabled is true and the terrain renders, so "
                                      "there are no counters to report until then.";
                }
                j["terrains"] = std::move(terrains);
                return j; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Structured(result);
        }

        // ---- olo_material_get (main-marshaled) ---------------------------------
        // What the GPU was actually given, not what the asset says. The two differ
        // more often than is comfortable: a MaterialComponent silently overrides
        // every submesh's imported material, and the engine default quietly stands
        // in when neither exists. Resolution goes through the renderer's own
        // OloEngine::ResolveSubmeshMaterial so this tool cannot drift from the
        // truth it is supposed to report.

        const char* AlphaModeToken(AlphaMode mode)
        {
            switch (mode)
            {
                case AlphaMode::Opaque:
                    return "Opaque";
                case AlphaMode::Mask:
                    return "Mask";
                case AlphaMode::Blend:
                    return "Blend";
            }
            return "Opaque";
        }

        // Shape one resolved PODMaterialData — the EXACT struct
        // Renderer3D::CreatePODMaterialDataForMaterial builds and uploads into the
        // frame material table, so every value here is what the shader will read.
        Json ResolvedMaterialJson(const Material& material, const PODMaterialData& data,
                                  u32 submeshIndex, std::string_view source)
        {
            // BOTH currencies per slot (issue #890). The fields are identities
            // since issue #691; `textures` reports the backend-native handle
            // as hex for RenderDoc correlation, and `textureIdentities` the
            // RHI handle, which is the only one that means anything under
            // Vulkan — where a truncated `VkImage` correlates with nothing a
            // capture shows, and slots the shader really samples would print
            // as an arbitrary 32-bit number.
            const auto nativeSlot = [](RHI::ResourceHandle handle)
            { return MCP::NativeHandleHex(Debug::NativeHandleForDiagnostics(handle)); };
            const auto identitySlot = [](RHI::ResourceHandle handle)
            {
                const std::string token = MCP::IdentityToken(RHI::HashKey(handle));
                return token.empty() ? std::string("<none>") : token;
            };

            Json textures;
            textures["albedo"] = nativeSlot(data.albedoMapID);
            textures["metallicRoughness"] = nativeSlot(data.metallicRoughnessMapID);
            textures["normal"] = nativeSlot(data.normalMapID);
            textures["ao"] = nativeSlot(data.aoMapID);
            textures["emissive"] = nativeSlot(data.emissiveMapID);

            Json textureIdentities;
            textureIdentities["albedo"] = identitySlot(data.albedoMapID);
            textureIdentities["metallicRoughness"] = identitySlot(data.metallicRoughnessMapID);
            textureIdentities["normal"] = identitySlot(data.normalMapID);
            textureIdentities["ao"] = identitySlot(data.aoMapID);
            textureIdentities["emissive"] = identitySlot(data.emissiveMapID);

            Json useMaps;
            useMaps["useAlbedoMap"] = data.albedoMapID.IsValid();
            useMaps["useMetallicRoughnessMap"] = data.metallicRoughnessMapID.IsValid();
            useMaps["useNormalMap"] = data.normalMapID.IsValid();
            useMaps["useAOMap"] = data.aoMapID.IsValid();
            useMaps["useEmissiveMap"] = data.emissiveMapID.IsValid();

            Json j;
            j["submesh"] = submeshIndex;
            j["source"] = std::string(source);
            j["name"] = material.GetName();
            j["pbr"] = data.enablePBR;
            j["alphaMode"] = AlphaModeToken(material.GetAlphaMode());
            j["alphaCutoff"] = data.alphaCutoff;
            j["twoSided"] = material.GetFlag(MaterialFlag::TwoSided);
            j["baseColorFactor"] = Json::array({ data.baseColorFactor.r, data.baseColorFactor.g,
                                                 data.baseColorFactor.b, data.baseColorFactor.a });
            j["metallicFactor"] = data.metallicFactor;
            j["roughnessFactor"] = data.roughnessFactor;
            j["normalScale"] = data.normalScale;
            j["occlusionStrength"] = data.occlusionStrength;
            j["emissiveFactor"] = Json::array({ data.emissiveFactor.r, data.emissiveFactor.g,
                                                data.emissiveFactor.b });
            j["enableIBL"] = data.enableIBL;
            j["iblIntensity"] = data.iblIntensity;
            j["useMaps"] = std::move(useMaps);
            j["textureIds"] = std::move(textures);
            j["textureIdentities"] = std::move(textureIdentities);
            return j;
        }

        ToolResult Handle_MaterialGet(McpServer& server, const Json& args)
        {
            if (!args.contains("entity"))
                return ToolResult::Error("Missing required argument 'entity' (entity UUID).");
            u64 id = 0;
            if (!ParseUuid(args["entity"], id))
                return ToolResult::Error("Invalid 'entity': expected a UUID as a string or number.");

            i32 requestedSubmesh = -1;
            if (args.contains("submesh") && args["submesh"].is_number_integer())
            {
                const long long value = args["submesh"].get<long long>();
                if (value < 0)
                    return ToolResult::Error("Invalid 'submesh': expected a non-negative index.");
                requestedSubmesh = static_cast<i32>(std::min<long long>(value, 65535));
            }

            const Json result = server.MarshalRead([&server, id, requestedSubmesh]() -> Json
                                                   {
                const Ref<Scene> scene = server.Context().GetActiveScene ? server.Context().GetActiveScene() : nullptr;
                if (!scene)
                    return Json{ { "__error", "No active scene." } };

                const auto entityOpt = scene->TryGetEntityWithUUID(UUID(id));
                if (!entityOpt.has_value())
                    return Json{ { "__error", "No entity with UUID " + UuidToString(UUID(id)) + " in the active scene." } };
                Entity entity = *entityOpt;

                // The engine's stand-in when neither an override nor an imported
                // material exists. Constructed exactly like Scene's cached default
                // (Scene.cpp::GetDefaultMaterial), which is file-static there.
                //
                // Deliberately NOT static: this is a per-call temporary. As a function
                // static it was a process-wide Ref<Material> with no release site — the
                // same shape as everything #839 swept — and caching buys nothing here,
                // since an MCP tool handler runs at agent speed, not per frame.
                const Ref<Material> engineDefault =
                    Material::CreatePBR("Default", glm::vec3(0.8f, 0.8f, 0.8f), 0.0f, 0.5f);

                const Material* overrideMaterial = entity.HasComponent<MaterialComponent>()
                                                       ? &entity.GetComponent<MaterialComponent>().m_Material
                                                       : nullptr;

                Ref<MeshSource> meshSource;
                std::string renderableKind;
                if (entity.HasComponent<VirtualMeshComponent>())
                {
                    renderableKind = "VirtualMeshComponent";
                    const auto& vmc = entity.GetComponent<VirtualMeshComponent>();
                    if (vmc.m_MeshSource != 0)
                        meshSource = AssetManager::GetAsset<MeshSource>(vmc.m_MeshSource);
                }
                else if (entity.HasComponent<MeshComponent>())
                {
                    renderableKind = "MeshComponent";
                    meshSource = entity.GetComponent<MeshComponent>().m_MeshSource;
                }
                else
                {
                    return Json{ { "__error", "Entity " + UuidToString(UUID(id)) +
                                                  " has no MeshComponent or VirtualMeshComponent (olo_material_get "
                                                  "reports the material resolved for a mesh draw)." } };
                }

                if (!meshSource)
                    return Json{ { "__error", "Entity " + UuidToString(UUID(id)) + "'s " + renderableKind +
                                                  " has no MeshSource loaded, so no material is resolved for it." } };

                const auto submeshCount = static_cast<u32>(std::max(0, meshSource->GetSubmeshes().Num()));
                if (submeshCount == 0)
                    return Json{ { "__error", "The MeshSource has no submeshes." } };
                if (requestedSubmesh >= 0 && static_cast<u32>(requestedSubmesh) >= submeshCount)
                    return Json{ { "__error", "Invalid 'submesh' " + std::to_string(requestedSubmesh) + ": the mesh has " +
                                                  std::to_string(submeshCount) + " submesh(es)." } };

                const u32 first = requestedSubmesh >= 0 ? static_cast<u32>(requestedSubmesh) : 0u;
                const u32 last = requestedSubmesh >= 0 ? first + 1u : submeshCount;

                Json submeshes = Json::array();
                for (u32 index = first; index < last; ++index)
                {
                    // One precedence rule on EVERY path — MaterialComponent override ->
                    // the submesh's imported material -> engine default — resolved through
                    // the same OloEngine::ResolveSubmeshMaterial the renderer itself calls.
                    // This tool used to special-case the classic path because it genuinely
                    // ignored imported materials; that divergence is fixed, and reporting a
                    // rule the renderer no longer follows would make this tool lie to the
                    // next person debugging a material.
                    const Material& resolved =
                        ResolveSubmeshMaterial(overrideMaterial, meshSource.get(), index, *engineDefault);
                    const Material* material = &resolved;
                    std::string_view source;
                    if (material == overrideMaterial)
                    {
                        source = "MaterialComponent (override)";
                    }
                    else if (material != engineDefault.get())
                    {
                        source = "MeshSource imported material (per-submesh)";
                    }
                    else
                    {
                        source = "engine default material";
                    }

                    const PODMaterialData data = Renderer3D::CreatePODMaterialDataForMaterial(*material, RHI::NullResource);
                    submeshes.push_back(ResolvedMaterialJson(*material, data, index, source));
                }

                Json j;
                j["entity"] = UuidToString(UUID(id));
                j["renderableKind"] = renderableKind;
                j["submeshCount"] = submeshCount;
                j["hasMaterialComponentOverride"] = overrideMaterial != nullptr;
                j["submeshes"] = std::move(submeshes);
                return j; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Structured(result);
        }

        // ---- olo_cluster_grid_stats (main-marshaled; SSBO readback) ------------

        // Read `bytes` bytes out of a GPU storage buffer through a temporary
        // GL_DYNAMIC_READ staging copy.
        //
        // Do NOT glGetNamedBufferSubData a GL_DYNAMIC_COPY buffer directly: the
        // light grid / index list are written by the culling compute and read by
        // every lit fragment, so they must stay in video memory. A CPU read
        // straight off one makes NVIDIA log "Analysis of buffer object N usage
        // indicates that CPU is consuming buffer object data. The usage hint ...
        // GL_DYNAMIC_COPY, is inconsistent with this usage pattern" (131188) and
        // then migrate the buffer VIDEO -> HOST (perf warning 131186) —
        // permanently slowing every frame that samples it. Same bug, same fix, as
        // VirtualMeshRegistry::ReadFrameCullStats.
        bool ReadStorageBufferStaged(const Ref<StorageBuffer>& buffer, u32 bytes, void* destination)
        {
            if (!buffer || bytes == 0 || buffer->GetSize() < bytes)
                return false;

            // The GL staging dance below exists ONLY to dodge the NVIDIA
            // VIDEO -> HOST migration described above, which is a GL driver
            // heuristic keyed on the buffer's usage hint. Vulkan has no such
            // hint and no such migration: ReadBufferSubData resolves the
            // StorageBuffer identity and runs a one-shot copy with its own
            // availability barrier, so the portable route IS the right route
            // there (#810; this tool used to refuse outright).
            if (RendererAPI::GetAPI() != RendererAPI::API::OpenGL)
            {
                const RHI::ResourceHandle handle = buffer->GetRHIHandle();
                if (!handle.IsValid())
                    return false;
                RenderCommand::ReadBufferSubData(handle, 0u, bytes, destination);
                return true;
            }

            u32 stagingId = 0;
            glCreateBuffers(1, &stagingId);
            glNamedBufferData(stagingId, static_cast<GLsizeiptr>(bytes), nullptr, GL_DYNAMIC_READ);
            glCopyNamedBufferSubData(buffer->GetRendererID(), stagingId, 0, 0, static_cast<GLsizeiptr>(bytes));
            glGetNamedBufferSubData(stagingId, 0, static_cast<GLsizeiptr>(bytes), destination);
            glDeleteBuffers(1, &stagingId);
            return glGetError() == GL_NO_ERROR;
        }

        ToolResult Handle_ClusterGridStats(McpServer& server, const Json& /*args*/)
        {
            const Json result = server.MarshalRead([&server]() -> Json
                                                   {
                const auto& forwardPlus = Renderer3D::GetForwardPlus();
                const LightGrid& lightGrid = forwardPlus.GetLightGrid();
                if (!lightGrid.IsInitialized())
                    return Json{ { "__error", "The clustered light grid is not initialized (no frame rendered yet)." } };

                ClusterGrid::GridDims dims;
                dims.CountX = lightGrid.GetClusterCountX();
                dims.CountY = lightGrid.GetClusterCountY();
                dims.CountZ = lightGrid.GetClusterCountZ();
                dims.MaxLightsPerCluster = lightGrid.GetMaxLightsPerCluster();

                const u32 totalClusters = dims.TotalClusters();
                const u32 tileCount = lightGrid.GetTileCount();
                const u32 gridStorageWords = ClusteredLighting::LightGridStorageWords(totalClusters, tileCount);
                std::vector<u32> gridWords(gridStorageWords, 0u);
                const auto gridBytes = static_cast<u32>(gridWords.size() * sizeof(u32));
                if (!ReadStorageBufferStaged(lightGrid.GetLightGridSSBO(), gridBytes, gridWords.data()))
                    return Json{ { "__error", "Failed to read back the light-grid SSBO." } };

                std::array<u32, ClusteredLighting::kGlobalCounterAndDispatchWordCount> counters{};
                if (!ReadStorageBufferStaged(lightGrid.GetGlobalIndexSSBO(), static_cast<u32>(sizeof(counters)),
                                             counters.data()))
                {
                    return Json{ { "__error", "Failed to read back the clustered-culling counters." } };
                }
                const u32 globalIndexCount = counters[0];

                const u32 lightIndexCapacity = lightGrid.GetLightIndexCapacityWords();

                // Two u32 per cluster: (offset, count), followed by the
                // depth-aware per-tile metadata used to cross-check the
                // independently written indirect active-cluster count.
                const std::span<const u32> gridPairs(gridWords.data(), static_cast<std::size_t>(totalClusters) * 2u);
                const ClusterGrid::Stats stats = ClusterGrid::Compute(gridPairs, dims);
                Json j = ClusterGrid::ToJson(stats, dims, globalIndexCount, lightIndexCapacity);
                j["renderingPath"] = RenderingPathName(Renderer3D::GetRendererSettings().Path);
                j["screen"] = Json{ { "width", lightGrid.GetScreenWidth() },
                                    { "height", lightGrid.GetScreenHeight() } };
                const bool depthAware = forwardPlus.WasLastCullingDepthAware();
                const auto activeFromMetadata = ClusterGrid::ActiveClusterCountFromMetadata(
                    gridWords, lightGrid.GetDepthTileMetadataOffsetWords(), tileCount);
                const bool counterVerified = depthAware && activeFromMetadata.has_value() &&
                                             *activeFromMetadata == counters[1];
                const u64 producerFrameIndex = forwardPlus.GetLastCullingFrameIndex();
                const u64 currentFrameIndex = GPUReadbackStats::GetFrameIndex();
                Json culling = ClusterGrid::CullingToJson(depthAware, counters[1], totalClusters,
                                                          producerFrameIndex, currentFrameIndex, counterVerified);
                if (depthAware)
                {
                    culling["indirectActiveClusters"] = counters[1];
                    if (activeFromMetadata)
                        culling["metadataActiveClusters"] = *activeFromMetadata;
                    if (!counterVerified)
                    {
                        culling["counterVerificationError"] = activeFromMetadata
                                                                   ? "Indirect and metadata active-cluster counts differ."
                                                                   : "Depth-tile metadata was unavailable for the cross-check.";
                    }
                }
                j["culling"] = std::move(culling);
                if (Renderer3D::GetRendererSettings().Path == RenderingPath::Forward)
                    j["note"] = "The plain Forward path does not run the clustered cull, so the grid holds whatever "
                                "the last Forward+/Deferred frame left in it (or zeroes).";
                return j; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Structured(result);
        }

        // ---- olo_shadow_atlas_layout (main-marshaled) --------------------------
        // The frame's shadow-atlas allocation: who won a tile, at what size, and
        // — the part a screenshot can never show — who was STARVED. A missing
        // local-light shadow looks exactly like a shadow bug until you can see
        // that the light simply lost the priority contest.
        ToolResult Handle_ShadowAtlasLayout(McpServer& server, const Json& /*args*/)
        {
            const Json result = server.MarshalRead([]() -> Json
                                                   {
                const ShadowMap& shadowMap = Renderer3D::GetShadowMap();
                const auto& layout = shadowMap.GetAtlasLayout();
                const u32 atlasResolution = shadowMap.GetAtlasResolution();
                const u32 entryCount = shadowMap.GetAtlasEntryCount();

                Json casters = Json::array();
                Json entries = Json::array();
                u32 allocatedCasters = 0;
                u32 starvedCasters = 0;
                u64 usedTilePixels = 0;

                for (const auto& record : layout)
                {
                    Json caster;
                    caster["lightEntity"] = UuidToString(UUID(record.LightEntity));
                    caster["casterType"] = record.Type == ShadowAtlas::CasterType::Spot ? "Spot" : "Point";
                    caster["sourceKind"] = record.SourceKind;
                    caster["score"] = record.Score;
                    caster["allocated"] = record.Allocated;
                    if (record.Allocated)
                    {
                        ++allocatedCasters;
                        caster["rank"] = record.Rank;
                        caster["baseEntry"] = record.BaseEntry;
                        caster["entryCount"] = record.EntryCount;

                        Json tiles = Json::array();
                        for (u32 face = 0; face < record.EntryCount; ++face)
                        {
                            const u32 entryIndex = record.BaseEntry + face;
                            const ShadowAtlas::TileRect& rect = shadowMap.GetAtlasEntryRect(entryIndex);
                            usedTilePixels += static_cast<u64>(rect.Size) * rect.Size;

                            Json tile;
                            tile["entry"] = entryIndex;
                            tile["face"] = face; // 0..5 = +X,-X,+Y,-Y,+Z,-Z for a point caster
                            tile["x"] = rect.X;
                            tile["y"] = rect.Y;
                            tile["width"] = rect.Size;
                            tile["height"] = rect.Size;
                            tile["resolution"] = rect.Size;
                            tiles.push_back(tile);

                            Json flat = tile;
                            flat["lightEntity"] = UuidToString(UUID(record.LightEntity));
                            flat["casterType"] = caster["casterType"];
                            flat["sourceKind"] = record.SourceKind;
                            flat["rank"] = record.Rank;
                            flat["score"] = record.Score;
                            entries.push_back(std::move(flat));
                        }
                        caster["tiles"] = std::move(tiles);
                    }
                    else
                    {
                        ++starvedCasters;
                        caster["starvedReason"] =
                            record.Score <= 0.0f
                                ? "Score 0: the light's range sphere is outside the camera frustum (or its range/"
                                  "intensity is <= 0), so it never competes for a tile."
                                : "Out of atlas budget: higher-scoring casters consumed the entry / light / space "
                                  "budget before this one (a point caster needs 6 tiles and is skipped whole if they "
                                  "don't all fit).";
                    }
                    casters.push_back(std::move(caster));
                }

                const u64 atlasPixels = static_cast<u64>(atlasResolution) * atlasResolution;

                Json j;
                j["enabled"] = shadowMap.IsEnabled();
                j["atlasResolution"] = atlasResolution;
                j["maxEntries"] = ShadowMap::MAX_SHADOW_ATLAS_ENTRIES;
                j["maxShadowedLights"] = ShadowAtlas::kMaxShadowedLights;
                j["entriesUsed"] = entryCount;
                j["candidateCount"] = static_cast<u32>(layout.size());
                j["allocatedCasters"] = allocatedCasters;
                j["starvedCasters"] = starvedCasters;
                j["atlasAreaUsed"] = atlasPixels > 0 ? static_cast<f64>(usedTilePixels) / static_cast<f64>(atlasPixels)
                                                     : 0.0;
                j["casters"] = std::move(casters);
                j["entries"] = std::move(entries);
                j["directionalShadow"] = Json{ { "csmCascades", ShadowMap::MAX_CSM_CASCADES },
                                               { "resolution", shadowMap.GetResolution() } };
                if (!shadowMap.IsEnabled())
                    j["note"] = "Shadows are globally disabled, so no atlas allocation runs.";
                else if (layout.empty())
                    j["note"] = "No local light requested a shadow this frame (no spot / point / sphere-area light "
                                "with m_CastShadows), so the atlas is empty. The directional CSM is separate and is "
                                "not packed into this atlas.";
                else if (starvedCasters > 0)
                    j["note"] = std::to_string(starvedCasters) +
                                " shadow caster(s) were STARVED this frame — they requested a shadow but got no "
                                "atlas tile, so they cast none. Check their 'starvedReason'.";
                return j; });

            return ToolResult::Structured(result);
        }

        // ---- olo_froxel_fog_probe (main-marshaled; 1x1x1 facade readback) ------
        // Sample the froxel volumetric-fog volume at a froxel or a world position
        // (issue #607; relates to #435). Every fog contract we have compares FINAL
        // FRAME pixels, which cannot tell "the scatter pass injected nothing" from
        // "the composite tapped the wrong froxel" — this can: it reports the RAW
        // (per-froxel scatter + extinction) and INTEGRATED (accumulated in-scatter
        // + transmittance) values at one cell, with the cell's world bounds.
        //
        // The froxel <-> world mapping is the pure, unit-tested McpFroxelFogProbe.h
        // core, fed from VolumetricFogPass::GetFroxelVolumeState() — the CPU mirror
        // of the FroxelFogData UBO the two compute shaders read. Re-deriving the
        // mapping from the camera here would be the classic confident liar: the z
        // slices are EXPONENTIAL, so a linear guess is wrong everywhere but the two
        // end slices.

        // (main thread) Read one texel out of a 3D volume — see the body for why
        // a 1x1x1 region is the whole trick.
        FroxelFog::VolumeSample ProbeVolumeTexel(RHI::ResourceHandle volume, const char* label, i32 x, i32 y, i32 z)
        {
            FroxelFog::VolumeSample sample;
            if (!volume.IsValid())
            {
                sample.Unavailable = std::string(label) + " volume does not exist this frame.";
                return sample;
            }

            // Both backends since #810: ReadTextureSubImage addresses a volume
            // with z as the depth SLICE (the Vulkan arm branches on the image's
            // 3D view type to do exactly that), so a 1x1x1 read is the same
            // call on either. Never the whole 160x90x64 RGBA16F volume — that
            // is 7 MB per read, per volume.
            std::array<f32, 4> texel{ 0.0f, 0.0f, 0.0f, 0.0f };
            if (!RenderCommand::ReadTextureSubImage(volume, 0u, x, y, z, 1u, 1u, 1u, RHI::Format::RGBA32Float,
                                                    texel.size() * sizeof(f32), texel.data()))
            {
                sample.Unavailable =
                    std::string("Readback of the ") + label + " volume failed — see the editor log.";
                return sample;
            }

            sample.Available = true;
            sample.Value = texel;
            return sample;
        }

        ToolResult Handle_FroxelFogProbe(McpServer& server, const Json& args)
        {
            const bool hasFroxel = args.contains("froxel");
            const bool hasWorldPos = args.contains("worldPos");
            if (hasFroxel == hasWorldPos)
                return ToolResult::Error("Give exactly one of 'froxel' ([x, y, z] froxel coords) or 'worldPos' "
                                         "([x, y, z] world position).");

            const auto parseVec3 = [&args](const char* key, std::array<f64, 3>& out) -> std::string
            {
                const Json& value = args[key];
                if (!value.is_array() || value.size() != 3)
                    return std::string("Invalid '") + key + "': expected an array of 3 numbers.";
                for (std::size_t i = 0; i < 3; ++i)
                {
                    if (!value[i].is_number())
                        return std::string("Invalid '") + key + "': expected an array of 3 numbers.";
                    out[i] = value[i].get<f64>();
                    if (!std::isfinite(out[i]))
                        return std::string("Invalid '") + key + "': values must be finite.";
                }
                return {};
            };

            std::array<f64, 3> requested{ 0.0, 0.0, 0.0 };
            if (const std::string error = parseVec3(hasFroxel ? "froxel" : "worldPos", requested); !error.empty())
                return ToolResult::Error(error);

            if (args.value("forceFrame", false))
                (void)ForceFreshFrame(server, /*settleFrames*/ 2);

            const Json result = server.MarshalRead([&server, hasFroxel, requested]() -> Json
                                                   {
                const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph();
                if (!graph)
                    return Json{ { "__error", "No active render graph (the editor is not in 3D mode, or no frame has been rendered yet)." } };

                const Ref<VolumetricFogPass> pass = graph->GetNode<VolumetricFogPass>("VolumetricFogPass");
                if (!pass)
                    return Json{ { "__error", "The volumetric fog pass is not registered in the render graph." } };

                const FogSettings& fog = Renderer3D::GetFogSettings();
                const FroxelVolumeState& state = pass->GetFroxelVolumeState();
                if (!state.Valid)
                {
                    // Degrade with the ACTIONABLE reason, not a bare failure: the
                    // froxel chain only runs with fog + volumetric fog both on.
                    std::string reason = "The froxel fog volume has not been produced this session";
                    if (!fog.Enabled)
                        reason += " — fog is disabled. Enable it with olo_render_toggle_pass { name: 'fog' }";
                    else if (!fog.EnableVolumetric)
                        reason += " — volumetric fog is disabled. Enable it with olo_render_toggle_pass "
                                  "{ name: 'fogvolumetric' }";
                    else
                        reason += " (the compute chain has not run yet — render a frame first)";
                    return Json{ { "__error", reason + "." } };
                }

                FroxelFog::ProbeResult probe;
                probe.Vol.DimX = static_cast<i32>(state.DimX);
                probe.Vol.DimY = static_cast<i32>(state.DimY);
                probe.Vol.DimZ = static_cast<i32>(state.DimZ);
                probe.Vol.Near = state.Near;
                probe.Vol.Far = state.Far;
                probe.Vol.LogFarOverNear = state.LogFarOverNear;
                probe.Vol.View = state.View;
                probe.Vol.InverseView = state.InverseView;
                probe.Vol.Projection = state.Projection;
                probe.Vol.InverseProjection = state.InverseProjection;
                probe.Vol.RenderOrigin = state.RenderOrigin;
                if (!FroxelFog::IsUsable(probe.Vol))
                    return Json{ { "__error", "The froxel fog volume's frame state is degenerate (near/far/dims)." } };

                if (hasFroxel)
                {
                    probe.Coord.X = static_cast<f32>(requested[0]);
                    probe.Coord.Y = static_cast<f32>(requested[1]);
                    probe.Coord.Z = static_cast<f32>(requested[2]);
                    const auto clampIndex = [&probe](f64 value, i32 count)
                    {
                        const auto raw = static_cast<i32>(std::floor(value));
                        const i32 clamped = std::clamp(raw, 0, count - 1);
                        if (clamped != raw)
                            probe.Coord.Clamped = true;
                        return clamped;
                    };
                    probe.Coord.IX = clampIndex(requested[0], probe.Vol.DimX);
                    probe.Coord.IY = clampIndex(requested[1], probe.Vol.DimY);
                    probe.Coord.IZ = clampIndex(requested[2], probe.Vol.DimZ);
                    probe.Coord.ViewDepth = FroxelFog::SliceViewDepth(
                        probe.Vol, (static_cast<f32>(probe.Coord.IZ) + 0.5f) / static_cast<f32>(probe.Vol.DimZ));
                    probe.Coord.InFrustum = !probe.Coord.Clamped;
                    probe.Coord.InDepthRange = !probe.Coord.Clamped;
                    if (probe.Coord.Clamped)
                        probe.Note = "The requested froxel is outside the volume; the nearest cell was sampled.";
                }
                else
                {
                    probe.FromWorldPos = true;
                    probe.RequestedWorldPos = glm::vec3(static_cast<f32>(requested[0]),
                                                        static_cast<f32>(requested[1]),
                                                        static_cast<f32>(requested[2]));
                    probe.Coord = FroxelFog::WorldToFroxel(probe.Vol, probe.RequestedWorldPos);
                    if (!probe.Coord.InFrustum)
                        probe.Note = "The requested world position is outside the camera frustum, so no froxel "
                                     "covers it; the nearest cell was sampled and its values do not describe that "
                                     "point.";
                    else if (!probe.Coord.InDepthRange)
                        probe.Note = "The requested world position is outside the fog volume's depth range [" +
                                     std::to_string(probe.Vol.Near) + ", " + std::to_string(probe.Vol.Far) +
                                     "] (the volume ends at FogSettings::End, clamped to [20, 500]); the nearest "
                                     "slice was sampled.";
                }

                // The fog volumes are identities (issue #691) and the probe reads
                // them through the facade spine (#810), so no native id and no
                // diagnostics hatch is involved on either backend.
                probe.Raw = ProbeVolumeTexel(state.ScatterTextureID, "scatter", probe.Coord.IX, probe.Coord.IY,
                                             probe.Coord.IZ);
                probe.Integrated = ProbeVolumeTexel(state.IntegratedTextureID, "integrated", probe.Coord.IX,
                                                    probe.Coord.IY, probe.Coord.IZ);

                Json j = FroxelFog::ToJson(probe);
                j["fog"] = Json{ { "enabled", fog.Enabled },
                                 { "volumetric", fog.EnableVolumetric },
                                 { "ranThisFrame", pass->RanThisFrame() } };
                j["meta"] = CaptureStampJson(server.Context().GetFrameIndex ? server.Context().GetFrameIndex() : 0, server.Context());
                if (!pass->RanThisFrame())
                    j["staleness"] = "The froxel chain did NOT run on the last frame (fog or volumetric fog was "
                                     "turned off); these are the values from the last frame it did run.";
                return j; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
            return ToolResult::Structured(result);
        }

        // ---- olo_gpu_resources (main-marshaled) --------------------------------
        //
        // The machine-readable face of the GPU Resource Inspector panel (#810).
        // Both currencies on every row (ADR 0011 amendment (77)): the RHI
        // identity, and the native object a RenderDoc / RGP capture shows.
        //
        // Why it exists as an MCP tool at all: the panel answered this only to a
        // human looking at the editor, and on Vulkan it answered nothing to
        // anyone. olo_memory_report gives ENGINE-tracked totals by category;
        // this gives the actual live object list plus, where the backend can
        // report it, the DEVICE allocator's own heap budgets — which is the
        // number that says whether you are about to run out of VRAM.
        ToolResult Handle_GpuResources(McpServer& server, const Json& args)
        {
            const std::string typeFilter = args.contains("type") && args["type"].is_string()
                                               ? args["type"].get<std::string>()
                                               : std::string{};
            const auto limit = args.contains("limit") && args["limit"].is_number_integer()
                                   ? static_cast<sizet>(std::clamp<long long>(args["limit"].get<long long>(), 1, 4096))
                                   : sizet{ 512 };
            const std::string nameFilter = args.contains("nameContains") && args["nameContains"].is_string()
                                               ? args["nameContains"].get<std::string>()
                                               : std::string{};

            Json result = server.MarshalRead([typeFilter, nameFilter, limit]() -> Json
                                             {
                auto& inspector = GPUResourceInspector::GetInstance();
                // Vulkan discovers its live set from RHI::ResourceRegistry on
                // demand rather than being pushed into by registration macros,
                // so refresh before reading. No-op on OpenGL. Must run on the
                // main thread — the Vulkan side tables it reads are
                // render-thread-only, which is what MarshalRead guarantees.
                inspector.RefreshDiscoveredResources();

                const auto rows = inspector.SnapshotResources();

                Json entries = Json::array();
                sizet matched = 0;
                std::unordered_map<std::string, u64> countByType;
                std::unordered_map<std::string, u64> bytesByType;
                u64 totalBytes = 0;

                for (const auto& row : rows)
                {
                    const std::string typeName = GpuResourceTypeName(row.Type);
                    ++countByType[typeName];
                    bytesByType[typeName] += static_cast<u64>(row.MemoryUsage);
                    totalBytes += static_cast<u64>(row.MemoryUsage);

                    if (!typeFilter.empty() && typeName != typeFilter)
                        continue;
                    if (!nameFilter.empty() && row.Name.find(nameFilter) == std::string::npos &&
                        row.DebugName.find(nameFilter) == std::string::npos)
                        continue;
                    ++matched;
                    if (entries.size() >= limit)
                        continue;

                    Json e;
                    e["type"] = typeName;
                    e["name"] = row.Name;
                    if (!row.DebugName.empty() && row.DebugName != row.Name)
                        e["debugName"] = row.DebugName;
                    // BOTH currencies, always.
                    e["nativeHandle"] = std::format("0x{:X}", row.NativeHandle);
                    if (row.Handle.IsValid())
                    {
                        e["handle"] = Json{ { "index", row.Handle.Index },
                                            { "generation", row.Handle.Generation } };
                    }
                    e["backend"] = GpuResourceBackendName(row.Backend);
                    if (row.MemoryUsage > 0)
                        e["bytes"] = static_cast<u64>(row.MemoryUsage);
                    if (row.Width > 0 || row.Height > 0)
                    {
                        e["width"] = row.Width;
                        e["height"] = row.Height;
                    }
                    if (row.MipLevels > 1)
                        e["mipLevels"] = row.MipLevels;
                    if (row.SizeBytes > 0)
                        e["sizeBytes"] = row.SizeBytes;
                    if (!row.FormatName.empty())
                        e["format"] = row.FormatName;
                    e["nativeFormat"] = std::format("0x{:X}", row.NativeFormat);
                    if (row.IsBound)
                    {
                        e["bound"] = true;
                        e["bindingSlot"] = row.BindingSlot;
                    }
                    entries.push_back(std::move(e));
                }

                Json byType = Json::array();
                for (const auto& [typeName, count] : countByType)
                {
                    byType.push_back(Json{ { "type", typeName },
                                           { "count", count },
                                           { "bytes", bytesByType[typeName] } });
                }
                std::sort(byType.begin(), byType.end(),
                          [](const Json& a, const Json& b)
                          { return a["type"].get<std::string>() < b["type"].get<std::string>(); });

                Json j;
                j["backend"] = GpuResourceBackendName(RendererAPI::GetAPI() == RendererAPI::API::Vulkan
                                                          ? RHI::Backend::Vulkan
                                                          : RendererAPI::GetAPI() == RendererAPI::API::OpenGL
                                                                ? RHI::Backend::OpenGL
                                                                : RHI::Backend::None);
                j["trackedCount"] = static_cast<u64>(rows.size());
                j["matchedCount"] = static_cast<u64>(matched);
                j["returnedCount"] = static_cast<u64>(entries.size());
                j["trackedBytes"] = totalBytes;
                j["byType"] = std::move(byType);
                j["resources"] = std::move(entries);
                j["previewsAvailable"] = inspector.SupportsPreviews();

                // The DEVICE allocator's own numbers, when the backend has
                // them. Deliberately separate from trackedBytes: that is the
                // sum of this tool's per-resource ESTIMATES (no tiling padding,
                // no suballocation slack, nothing created before the inspector
                // existed), while these are what the allocator and the OS
                // actually say. When they disagree, believe these.
                std::vector<IResourceInspectorBackend::MemoryHeap> heaps;
                if (inspector.QueryMemoryHeaps(heaps))
                {
                    Json heapJson = Json::array();
                    for (const auto& heap : heaps)
                    {
                        heapJson.push_back(Json{ { "index", heap.Index },
                                                 { "deviceLocal", heap.DeviceLocal },
                                                 { "usageBytes", heap.UsageBytes },
                                                 { "budgetBytes", heap.BudgetBytes },
                                                 { "blockBytes", heap.BlockBytes },
                                                 { "blockCount", heap.BlockCount },
                                                 { "allocationCount", heap.AllocationCount } });
                    }
                    j["memoryHeaps"] = std::move(heapJson);
                }

                if (rows.empty())
                {
                    j["note"] = "No tracked GPU resources. In a Debug editor this means the inspector never "
                                "initialised (it is compiled out of Release/Dist builds); otherwise the "
                                "renderer has not created anything yet.";
                }
                return j; });

            return ToolResult::Structured(result);
        }

        Json BuildVirtualShadowMapStatsReport()
        {
            VirtualShadowMapStats::Snapshot snapshot;
            snapshot.State.Available = Renderer3D::HasInitialized();
            snapshot.State.Freshness = StatsSnapshot::FreshnessModel::PreviousFrame;
            if (!snapshot.State.Available)
                return VirtualShadowMapStats::BuildReport(snapshot);

            const auto& vsm = Renderer3D::GetShadowMap().GetVirtualShadowMap();
            snapshot.State.Enabled = vsm.IsActive();
            snapshot.State.HasData = vsm.HasStatistics();
            if (!snapshot.State.Enabled || !snapshot.State.HasData)
                return VirtualShadowMapStats::BuildReport(snapshot);

            snapshot.State.SampleAgeFrames = 1;
            snapshot.PhysicalResolution = vsm.GetPhysicalResolution();
            snapshot.PageSize = VSM::kPageSize;
            const u64 pagesWide = snapshot.PhysicalResolution / snapshot.PageSize;
            snapshot.PhysicalPageCount = pagesWide * pagesWide;
            snapshot.VRAMBytes = vsm.GetVRAMBytes();

            const VSM::Statistics& stats = vsm.GetStatistics();
            snapshot.PagesRequested = stats.PagesRequested;
            snapshot.PagesAllocated = stats.PagesAllocated;
            snapshot.PagesFailed = stats.PagesFailed;
            snapshot.PagesDrawn = stats.PagesDrawn;
            snapshot.PagesResident = stats.PagesResident;
            snapshot.PagesFreed = stats.PagesFreed;
            snapshot.DrawInstances = stats.DrawInstances;
            snapshot.CullOverflows = stats.CullOverflows;
            snapshot.LocalPagesResident = stats.LocalPagesResident;
            snapshot.LocalPagesDrawn = stats.LocalPagesDrawn;
            return VirtualShadowMapStats::BuildReport(snapshot);
        }

        ToolResult Handle_VirtualShadowMapStats(McpServer& server, const Json& /*args*/)
        {
            const Json result = server.MarshalRead([]() -> Json
                                                   { return BuildVirtualShadowMapStatsReport(); });

            return ToolResult::Structured(result);
        }

        Json BuildRenderLODStatsReport()
        {
            RenderLODStats::Snapshot snapshot;
            snapshot.State.Available = Renderer3D::HasInitialized();
            snapshot.State.Enabled = snapshot.State.Available;
            snapshot.State.HasData = snapshot.State.Available;
            snapshot.State.Freshness = StatsSnapshot::FreshnessModel::SessionCumulative;
            if (!snapshot.State.HasData)
                return RenderLODStats::BuildReport(snapshot);

            const Renderer3D::Statistics& stats = Renderer3D::GetStats();
            snapshot.LODSwitches = stats.LODSwitches;
            snapshot.ObjectsPerLODLevel = stats.ObjectsPerLODLevel;
            return RenderLODStats::BuildReport(snapshot);
        }

        ToolResult Handle_RenderLODStats(McpServer& server, const Json& /*args*/)
        {
            const Json result = server.MarshalRead([]() -> Json
                                                   { return BuildRenderLODStatsReport(); });

            return ToolResult::Structured(result);
        }

        Json BuildRayTracingStatsReport()
        {
            RayTracingStats::Snapshot snapshot;
            snapshot.State.Available = Renderer3D::HasInitialized();
            if (snapshot.State.Available)
            {
                const auto& scene = Renderer3D::GetRayTracingScene();
                snapshot.Capabilities = scene.GetCapabilities();
                snapshot.Stats = Renderer3D::GetRayTracingStats();
                // "Enabled" is the capability, not a user toggle: there is no
                // switch to turn ray tracing off, only a device that does or
                // does not have it.
                snapshot.State.Enabled = snapshot.Capabilities.Supported;
                // A TLAS that has never been built is noData, NOT ready with
                // zeros — an unsupported GPU and an empty scene must not
                // produce the same payload.
                snapshot.State.HasData = snapshot.Capabilities.Supported && scene.GetTlasDeviceAddress() != 0u;
                // The canonical scene the structures are built from (issue
                // #1065). Reported whatever the RT status is: "nothing was
                // offered" and "nothing could be built" produce the same RT
                // payload and need different fixes.
                snapshot.GPUSceneAvailable = true;
                snapshot.GPUScene = Renderer3D::GetGPUSceneStats();
            }
            snapshot.State.Freshness = StatsSnapshot::FreshnessModel::PreviousFrame;
            return RayTracingStats::BuildReport(snapshot);
        }

        ToolResult Handle_RayTracingStats(McpServer& server, const Json& /*args*/)
        {
            const Json result = server.MarshalRead([]() -> Json
                                                   { return BuildRayTracingStatsReport(); });

            return ToolResult::Structured(result);
        }

        Json BuildDDGIProbeStatsReport()
        {
            DDGIProbeStats::Snapshot snapshot;
            snapshot.State.Freshness = StatsSnapshot::FreshnessModel::CurrentBlockingReadback;

            DDGIProbeUpdatePass* pass = Renderer3D::HasInitialized() ? Renderer3D::GetDDGIPass() : nullptr;
            snapshot.State.Available = pass != nullptr;
            if (pass == nullptr)
                return DDGIProbeStats::BuildReport(snapshot);

            snapshot.State.Enabled = Renderer3D::GetRendererSettings().EnableDDGI;
            snapshot.State.HasData = snapshot.State.Enabled && pass->RanThisFrame();
            if (!snapshot.State.HasData)
                return DDGIProbeStats::BuildReport(snapshot);

            // GetProbeStats performs the diagnostics readback and refreshes
            // the GPU-owned fields in GetProbeRecords(). Call it exactly once.
            const DDGIProbeUpdatePass::ProbeStats stats = pass->GetProbeStats();
            snapshot.State.SampleAgeFrames = 0;
            snapshot.TotalProbes = static_cast<u32>(std::max(pass->GetTotalProbeCount(), 0));
            snapshot.LiveProbes = stats.LiveProbes;
            snapshot.ActiveProbes = stats.ActiveProbes;
            snapshot.RelitProbes = stats.RelitProbes;
            snapshot.CapturedProbes = stats.CapturedProbes;
            snapshot.BlendedProbes = stats.BlendedProbes;
            snapshot.UncapturedLive = stats.UncapturedLive;

            f64 bounceWeightSum = 0.0;
            i64 bounceHitCount = 0;
            for (const DDGIProbeUpdatePass::ProbeRecord& record : pass->GetProbeRecords())
            {
                if (record.State != DDGI::ProbeState::Active)
                    continue;
                bounceWeightSum += static_cast<f64>(record.BounceWeightSum);
                bounceHitCount += static_cast<i64>(record.BounceHitCount);
            }
            if (bounceHitCount > 0)
                snapshot.BounceCoverage = static_cast<f32>(bounceWeightSum / static_cast<f64>(bounceHitCount));

            const auto& cascades = pass->GetCascades();
            const i32 cascadeCount = std::clamp(pass->GetCascadeCount(), 0, static_cast<i32>(cascades.size()));
            snapshot.Cascades.reserve(static_cast<sizet>(cascadeCount));
            for (i32 level = 0; level < cascadeCount; ++level)
            {
                const DDGI::CascadeGrid& cascade = cascades[static_cast<sizet>(level)];
                snapshot.Cascades.push_back(DDGIProbeStats::Cascade{
                    .Origin = { cascade.Origin.x, cascade.Origin.y, cascade.Origin.z },
                    .Spacing = { cascade.Spacing.x, cascade.Spacing.y, cascade.Spacing.z },
                    .LatticeMin = { cascade.LatticeMin.x, cascade.LatticeMin.y, cascade.LatticeMin.z },
                    .Dimensions = { cascade.Dims.x, cascade.Dims.y, cascade.Dims.z },
                });
            }
            return DDGIProbeStats::BuildReport(snapshot);
        }

        ToolResult Handle_DDGIProbeStats(McpServer& server, const Json& /*args*/)
        {
            const Json result = server.MarshalRead([]() -> Json
                                                   { return BuildDDGIProbeStatsReport(); });

            return ToolResult::Structured(result);
        }

    } // namespace

    void RegisterRenderTools(McpServer& server)
    {
        {
            ToolDef tool;
            tool.Name = "olo_render_frame_breakdown";
            tool.Toolset = "render";
            tool.Title = "Frame command breakdown";
            // The command list + per-pass breakdown are tables a human reads down.
            tool.DualAudienceContent = true;
            // Same transient one-frame capture as olo_perf_capture_frame — read-only.
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Capture the current frame and return its per-command / per-pipeline-stage structural "
                "breakdown — the granularity olo_perf_capture_frame omits. Triggers a one-frame capture of one "
                "render-graph pass's command bucket ('sourcePass', the scene render pass) and returns the "
                "pipeline stats plus the ordered command list for the chosen stage: each command's type, "
                "debug-name pass label, draw key (shader/material/depth/view-layer/render-mode), group id, "
                "execution order, static flag and GPU time, plus a command-type histogram. A 'graphAttribution' "
                "block places that bucket in the whole live render graph: every pass, which ones own a command "
                "bucket, which one is the capture source, and each pass's work type / cull state / execution "
                "order — so you can see which pass emitted the captured commands and which other passes emit "
                "commands that are not yet per-command attributed. Use format:\"markdown\" for the Command "
                "Bucket Inspector's LLM-analysis report (sort displacement, state-change deltas, batching "
                "analysis, optimization hints) instead of JSON.";
            tool.InputSchema = Schema::Object()
                                   .Prop("viewMode", Schema::String()
                                                         .Enum({ "presort", "postsort", "postbatch" })
                                                         .Desc("(json format only) Pipeline stage to list: 'presort' (submission order), 'postsort' "
                                                               "(after the radix sort), or 'postbatch' (what actually executed; default). Falls back "
                                                               "to an earlier, populated stage when the requested one is empty."))
                                   .Prop("maxCommands", Schema::Int()
                                                            .Min(1)
                                                            .Max(5000)
                                                            .Desc("(json format only) Cap on commands returned (default 200). The full count and a "
                                                                  "'truncated' flag are always reported."))
                                   .Prop("format", Schema::String()
                                                       .Enum({ "json", "markdown" })
                                                       .Desc("'json' (default): structured per-command breakdown shaped by viewMode/maxCommands. "
                                                             "'markdown': the human/LLM analysis report (covers all stages and commands)."))
                                   .NoAdditional();
            // outputSchema (#357-P2) describes the json format only; markdown returns
            // free text, which an outputSchema cannot constrain.
            tool.OutputSchema = Schema::Object()
                                    .Prop("frameNumber", Schema::Int().Min(0).Desc("Captured frame counter."))
                                    .Prop("sourcePass", Schema::String().Desc("Render-graph pass whose command bucket these commands were emitted by."))
                                    .Prop("viewMode", Schema::String().Desc("Pipeline stage actually listed (after empty-stage fallback)."))
                                    .Prop("commandCount", Schema::Int().Min(0).Desc("Total commands in the listed stage (pre-truncation)."))
                                    .Prop("returnedCommands", Schema::Int().Min(0).Desc("Commands actually included in 'commands' (<= maxCommands)."))
                                    .Prop("truncated", Schema::Bool().Desc("True when maxCommands capped the returned list."))
                                    .Prop("stats", Schema::Object().Desc("Aggregate frame stats (draw calls, state changes, sort/batch/execute ms, ...)."))
                                    .Prop("stageCounts", Schema::Object().Desc("Command counts at the preSort / postSort / postBatch stages."))
                                    .Prop("commandTypeHistogram", Schema::Object().Desc("Count of each command type over the full listed stage."))
                                    .Prop("commands", Schema::Array(Schema::Object()
                                                                        .Prop("index", Schema::Int())
                                                                        .Prop("type", Schema::String())
                                                                        .Prop("debugName", Schema::String())
                                                                        .Prop("isDraw", Schema::Bool())
                                                                        .Prop("executionOrder", Schema::Int())
                                                                        .Prop("drawKey", Schema::Object())
                                                                        .Prop("gpuMs", Schema::Number()))
                                                          .Desc("Ordered commands for the listed stage."))
                                    .Prop("graphAttribution", Schema::Object()
                                                                  .Prop("captureSourcePass", Schema::String().Desc("Pass whose bucket the per-command breakdown describes."))
                                                                  .Prop("passCount", Schema::Int().Min(0).Desc("Total passes in the live graph."))
                                                                  .Prop("commandBucketPassCount", Schema::Int().Min(0).Desc("Passes that own a command bucket."))
                                                                  .Prop("capturedPassCount", Schema::Int().Min(0).Desc("Command-bucket passes actually captured (currently 0 or 1)."))
                                                                  .Prop("passes", Schema::Array(Schema::Object()
                                                                                                    .Prop("name", Schema::String())
                                                                                                    .Prop("workType", Schema::String())
                                                                                                    .Prop("usesCommandBucket", Schema::Bool())
                                                                                                    .Prop("isCaptureSource", Schema::Bool())
                                                                                                    .Prop("culled", Schema::Bool())
                                                                                                    .Prop("isFinalPass", Schema::Bool())
                                                                                                    .Prop("executionIndex", Schema::Int()))
                                                                                      .Desc("Every pass in the live render graph."))
                                                                  .Desc("Whole-graph command-bucket landscape (omitted when no active render graph)."))
                                    .Required({ "frameNumber", "sourcePass", "commandCount", "commands" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_RenderFrameBreakdown;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_render_list_targets";
            tool.Toolset = "render";
            tool.Title = "List render targets";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "List the render graph's live texture/framebuffer resources for the current frame — "
                "scene colour, depth, G-buffer attachments, shadow maps, AO, post-process chain stages, "
                "water/OIT buffers, etc. Each entry has the canonical resource name (pass to "
                "olo_render_capture_target), kind, format, size, and producing passes. Requires the "
                "editor to be rendering in 3D mode.";
            tool.InputSchema = Schema::EmptyObject();
            tool.OutputSchema = Schema::Object()
                                    .Prop("count", Schema::Int().Min(0).Desc("Number of capturable targets listed."))
                                    .Prop("targets", Schema::Array(Schema::Object()
                                                                       .Prop("name", Schema::String().Desc("Canonical resource name (pass to olo_render_capture_target)."))
                                                                       .Prop("kind", Schema::String())
                                                                       .Prop("format", Schema::String().Desc("Omitted when the format is unknown."))
                                                                       .Prop("width", Schema::Int().Desc("Omitted when the size is unknown."))
                                                                       .Prop("height", Schema::Int().Desc("Omitted when the size is unknown."))
                                                                       .Prop("layers", Schema::Int().Desc("Layer count; array/cube/3D targets only."))
                                                                       .Prop("viewOfParentLayer", Schema::Int().Desc("Parent-array layer this view resolves to; per-layer views only."))
                                                                       .Prop("producers", Schema::Array(Schema::String()).Desc("Passes that write the target; omitted when empty."))))
                                    .Required({ "count", "targets" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_RenderListTargets;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_gpu_resources";
            tool.Toolset = "render";
            tool.Title = "List live GPU resources";
            // A table of objects plus a heap summary — worth rendering for a
            // human as well as returning structured.
            tool.DualAudienceContent = true;
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "List the GPU objects that exist RIGHT NOW — textures, cubemaps, buffers, framebuffers, "
                "vertex arrays, shader programs — each with BOTH identities: the RHI handle "
                "(index+generation, which survives a hot-reload and distinguishes a recycled native name "
                "from a reused one) and the native object id a RenderDoc / RGP capture shows. Textures "
                "report extent, mip count and storage format; buffers report their size. Works on OpenGL "
                "and Vulkan: under Vulkan the list is discovered from the RHI resource registry and "
                "enriched from the backend's image / root-object registries, so a Vulkan-only session can "
                "answer 'what is on the GPU and how big is it' for the first time. "
                "'memoryHeaps' (Vulkan only) reports the DEVICE allocator's own per-heap usage and budget "
                "— when it disagrees with 'trackedBytes' believe the heaps, because trackedBytes is a sum "
                "of per-resource estimates that ignores tiling padding and suballocation slack. "
                "Compare with olo_memory_report, which reports engine-tracked totals by category rather "
                "than the live object list. The inspector is a Debug-build instrument: a Release/Dist "
                "editor returns an empty list with a note.";
            tool.InputSchema = Schema::Object()
                                   .Prop("type", Schema::String().Desc("Only resources of this type (Texture2D, TextureCubemap, VertexBuffer, IndexBuffer, UniformBuffer, Framebuffer, VertexArray, ShaderProgram, Query, Other). Omit for all; 'byType' always covers everything regardless of the filter."))
                                   .Prop("nameContains", Schema::String().Desc("Only resources whose name or debug name contains this substring."))
                                   .Prop("limit", Schema::Int().Min(1).Max(4096).Desc("Maximum resources to return (default 512). 'matchedCount' reports how many passed the filters."))
                                   .NoAdditional();
            tool.OutputSchema =
                Schema::Object()
                    .Prop("backend", Schema::String().Desc("The active renderer backend ('opengl' / 'vulkan' / 'none')."))
                    .Prop("trackedCount", Schema::Int().Min(0).Desc("Total tracked resources, before filtering."))
                    .Prop("matchedCount", Schema::Int().Min(0).Desc("Resources passing the filters."))
                    .Prop("returnedCount", Schema::Int().Min(0).Desc("Resources actually listed (bounded by 'limit')."))
                    .Prop("trackedBytes", Schema::Int().Min(0).Desc("Sum of per-resource size ESTIMATES; see 'memoryHeaps' for the allocator's own numbers."))
                    .Prop("previewsAvailable", Schema::Bool().Desc("Whether the editor panel can render texture previews on this backend (OpenGL only)."))
                    .Prop("note", Schema::String().Desc("Present only when nothing is tracked — says whether that means the inspector never initialised (Release/Dist) or the renderer has created nothing yet."))
                    .Prop("byType", Schema::Array(Schema::Object()
                                                      .Prop("type", Schema::String())
                                                      .Prop("count", Schema::Int().Min(0))
                                                      .Prop("bytes", Schema::Int().Min(0))))
                    .Prop("memoryHeaps", Schema::Array(Schema::Object()
                                                           .Prop("index", Schema::Int().Min(0))
                                                           .Prop("deviceLocal", Schema::Bool())
                                                           .Prop("usageBytes", Schema::Int().Min(0))
                                                           .Prop("budgetBytes", Schema::Int().Min(0))
                                                           .Prop("blockBytes", Schema::Int().Min(0))
                                                           .Prop("blockCount", Schema::Int().Min(0))
                                                           .Prop("allocationCount", Schema::Int().Min(0)))
                                             .Desc("Device memory heaps. Omitted on backends with no portable budget query (OpenGL)."))
                    .Prop("resources", Schema::Array(Schema::Object()
                                                         .Prop("type", Schema::String())
                                                         .Prop("name", Schema::String())
                                                         .Prop("debugName", Schema::String())
                                                         .Prop("nativeHandle", Schema::String().Desc("Native object id as hex — what a RenderDoc / RGP capture shows. 0x0 is legitimate for a Vulkan framebuffer (dynamic rendering) or an arena-backed uniform buffer."))
                                                         .Prop("handle", Schema::Object()
                                                                             .Prop("index", Schema::Int().Min(0))
                                                                             .Prop("generation", Schema::Int().Min(0))
                                                                             .Desc("RHI identity. Omitted for a resource registered before its identity was minted."))
                                                         .Prop("backend", Schema::String())
                                                         .Prop("bytes", Schema::Int().Min(0))
                                                         .Prop("width", Schema::Int().Min(0))
                                                         .Prop("height", Schema::Int().Min(0))
                                                         .Prop("mipLevels", Schema::Int().Min(1))
                                                         .Prop("sizeBytes", Schema::Int().Min(0))
                                                         .Prop("format", Schema::String())
                                                         .Prop("nativeFormat", Schema::String().Desc("Native format enum value as hex (GL internal format / VkFormat)."))
                                                         .Prop("bound", Schema::Bool())
                                                         .Prop("bindingSlot", Schema::Int().Min(0))))
                    .Required({ "backend", "trackedCount", "matchedCount", "returnedCount", "resources" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_GpuResources;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_gpu_readback_stats";
            tool.Toolset = "render";
            tool.Title = "GPU readback stats";
            // Mutating only because of the optional 'enabled' argument; with no
            // arguments it is a pure read that does not even trigger a readback.
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Read the structured GPU readback-stats channel: per-frame counters that GPU-driven passes "
                "publish by atomic (instance-cull decisions by test, virtual-shadow page allocations / "
                "evictions / failures) plus CAPACITY-OVERFLOW FLAGS naming any pass that silently truncated "
                "its output this frame. Check 'overflows' first — a raised flag means the frame rendered "
                "wrong and the counters explain by how much. Numbers are read a few frames late by design "
                "(never synchronously); 'frameIndex' and 'latencyFrames' say which frame they describe. "
                "Call with no arguments to read; pass 'enabled' to switch the channel off or on (the only "
                "way to A/B its own cost against frame time).";
            tool.InputSchema =
                Schema::Object()
                    .Prop("enabled", Schema::Bool().Desc("Switch the channel on/off. Omit to read without changing anything. Off: the GLSL helpers early-out on one scalar load and no clear, copy or fence is issued."))
                    .NoAdditional();
            tool.OutputSchema =
                Schema::Object()
                    .Prop("enabled", Schema::Bool().Desc("False when the channel is switched off and nothing is being collected."))
                    .Prop("valid", Schema::Bool().Desc("False until the first frame has come back through the ring."))
                    .Prop("anyOverflow", Schema::Bool().Desc("True when any capacity-overflow flag fired this frame."))
                    .Prop("overflows", Schema::Array(Schema::Object()
                                                         .Prop("name", Schema::String().Desc("Flag name, e.g. InstanceCullOutput."))
                                                         .Prop("description", Schema::String().Desc("What truncated.")))
                                           .Desc("Only the flags that FIRED; empty means nothing truncated."))
                    .Prop("frameIndex", Schema::Int().Min(0).Desc("Engine frame the counters belong to — NOT the frame that read them."))
                    .Prop("latencyFrames", Schema::Int().Min(0).Desc("How many frames ago that frame was."))
                    .Prop("ringSlots", Schema::Int().Min(0).Desc("Staging slots in the readback ring."))
                    .Prop("slotsInFlight", Schema::Int().Min(0).Desc("Slots the GPU has not finished with yet."))
                    .Prop("ringSaturated", Schema::Bool().Desc("True when every slot is busy, so captures are being skipped and the counters are staler than latencyFrames implies."))
                    .Prop("counters", Schema::Array(Schema::Object()
                                                        .Prop("name", Schema::String())
                                                        .Prop("description", Schema::String())
                                                        .Prop("value", Schema::Int().Min(0)))
                                          .Desc("Every registered counter, in registry order."))
                    .Prop("note", Schema::String().Desc("Present only when the channel is disabled or has not returned a frame yet."))
                    .Required({ "enabled", "valid", "anyOverflow", "overflows", "counters" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_GpuReadbackStats;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_render_graph_topology_export";
            tool.Toolset = "render";
            tool.Title = "Export render graph topology";
            // passes / edges / resources are three tables a human scans to see the
            // graph's shape. The `format: "mermaid"` variant returns free text with
            // no structuredContent, so the dispatcher's guard leaves it untouched.
            tool.DualAudienceContent = true;
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Export the live render graph's topology as structured data for reasoning about the render "
                "pipeline — the passes, their topologically-sorted execution order, the pass-to-pass "
                "dependency edges, every registered resource (texture/framebuffer/buffer) with the passes "
                "that produce and consume it. Each pass reports its work type (Graphics/Compute/Copy), whether "
                "it declares resources, whether it is an async-compute candidate, whether it was culled "
                "(unreachable from the final pass this frame), whether it is the final/output pass, and its "
                "'accesses' — every resource it reads/writes WITH its resolved physical identity, so 'do "
                "these two passes touch the same physical texture this frame' is a single lookup (each "
                "resource also carries a 'native' block: texture/framebuffer/attachment/buffer ids as of the last "
                "executed frame; texture views resolve to their parent object). The JSON form also reports the "
                "persistent temporal-history registry with each plane's generation, validity, descriptor, and "
                "last invalidation cause. Use format:\"mermaid\" or "
                "format:\"dot\" for a drawable DAG of the pass graph instead of JSON. Read-only; requires the "
                "editor to be rendering in 3D mode. See olo_scheduler_graph for the engine's OTHER derived DAG, "
                "the per-tick gameplay system schedule.";
            tool.InputSchema = Schema::Object()
                                   .Prop("format", Schema::String()
                                                       .Enum({ "json", "mermaid", "dot" })
                                                       .Desc("'json' (default): full structured topology (passes, executionOrder, edges, resources). "
                                                             "'mermaid': a flowchart-LR DAG of the pass dependency graph. "
                                                             "'dot': the same DAG as Graphviz DOT."))
                                   .NoAdditional();
            // outputSchema describes the json format only; mermaid returns
            // free text, which an outputSchema cannot constrain.
            tool.OutputSchema = Schema::Object()
                                    .Prop("finalPass", Schema::String().Desc("The graph's designated final/output pass."))
                                    .Prop("passCount", Schema::Int().Min(0))
                                    .Prop("passes", Schema::Array(Schema::Object()
                                                                      .Prop("name", Schema::String())
                                                                      .Prop("workType", Schema::String())
                                                                      .Prop("declaresResources", Schema::Bool())
                                                                      .Prop("asyncComputeCandidate", Schema::Bool())
                                                                      .Prop("culled", Schema::Bool())
                                                                      .Prop("isFinalPass", Schema::Bool())
                                                                      .Prop("accesses", Schema::Array(Schema::Object()
                                                                                                          .Prop("resource", Schema::String())
                                                                                                          .Prop("mode", Schema::String().Enum({ "write", "read" }))
                                                                                                          .Prop("physicalKey", Schema::String().Desc("Resolved physical object as \"#index:generation\" - two accesses sharing it touch the same object, on every backend. Omitted when unbacked."))
                                                                                                          .Prop("nativeTexture", Schema::String().Desc("Backend-native texture handle as hex; display only, omitted when 0."))
                                                                                                          .Prop("nativeBuffer", Schema::String().Desc("Backend-native buffer handle as hex; display only, omitted when 0."))
                                                                                                          .Prop("bufferIdentity", Schema::String().Desc("Resolved buffer identity; omitted when not buffer-backed.")))
                                                                                            .Desc("Resources the pass reads/writes; omitted when it accesses none."))))
                                    .Prop("executionOrder", Schema::Array(Schema::String()).Desc("Topologically-sorted run order."))
                                    .Prop("edgeCount", Schema::Int().Min(0))
                                    .Prop("edges", Schema::Array(Schema::Object()
                                                                     .Prop("from", Schema::String())
                                                                     .Prop("to", Schema::String()))
                                                       .Desc("Execution-ordering dependencies (from must run before to)."))
                                    .Prop("resourceCount", Schema::Int().Min(0))
                                    .Prop("resources", Schema::Array(Schema::Object()
                                                                         .Prop("name", Schema::String())
                                                                         .Prop("kind", Schema::String())
                                                                         .Prop("format", Schema::String().Desc("Omitted when the format is unknown."))
                                                                         .Prop("width", Schema::Int().Desc("Omitted when the size is unknown."))
                                                                         .Prop("height", Schema::Int().Desc("Omitted when the size is unknown."))
                                                                         .Prop("samples", Schema::Int().Desc("Omitted when single-sampled."))
                                                                         .Prop("imported", Schema::Bool())
                                                                         .Prop("hasExternalBacking", Schema::Bool())
                                                                         .Prop("producers", Schema::Array(Schema::String()))
                                                                         .Prop("consumers", Schema::Array(Schema::String()))
                                                                         .Prop("native", Schema::Object().Desc("Backend-native object handles as hex, as of the last executed frame (texture, framebuffer, colorAttachments, depthAttachment, buffer). DISPLAY ONLY - what a RenderDoc / RGP capture shows. \"0x0\" is legitimate under Vulkan, so absence never means unbacked. Omitted when nothing native resolved."))
                                                                         .Prop("identity", Schema::Object().Desc("Resolved RHI identities as \"#index:generation\" (texture, buffer, colorAttachments, depthAttachment), plus viewOfParentLayer for a layer/face view. This is the currency to COMPARE and to decide on - two resources sharing one answer touch the same physical object on every backend. Omitted when unbacked."))))
                                    .Prop("historyCount", Schema::Int().Min(0))
                                    .Prop("histories", Schema::Array(Schema::Object()
                                                                         .Prop("name", Schema::String())
                                                                         .Prop("effect", Schema::String())
                                                                         .Prop("plane", Schema::String())
                                                                         .Prop("resolution", Schema::String())
                                                                         .Prop("view", Schema::Int().Min(0))
                                                                         .Prop("generation", Schema::Int().Min(0))
                                                                         .Prop("valid", Schema::Bool())
                                                                         .Prop("hasTexture", Schema::Bool())
                                                                         .Prop("lastInvalidation", Schema::String())
                                                                         .Prop("descriptor", Schema::Object()
                                                                                                 .Prop("width", Schema::Int().Min(0))
                                                                                                 .Prop("height", Schema::Int().Min(0))
                                                                                                 .Prop("format", Schema::String())
                                                                                                 .Prop("mipLevels", Schema::Int().Min(1))
                                                                                                 .Prop("samples", Schema::Int().Min(1))
                                                                                                 .Prop("layoutVersion", Schema::Int().Min(1))
                                                                                                 .Prop("backend", Schema::String())))
                                                           .Desc("Persistent typed temporal histories, separate from transient graph resources."))
                                    .Prop("note", Schema::String())
                                    .Required({ "finalPass", "passCount", "passes", "executionOrder", "edgeCount", "edges", "resourceCount", "resources", "historyCount", "histories", "note" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_RenderGraphTopologyExport;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_render_capture_target";
            tool.Toolset = "render";
            tool.Title = "Capture render target";
            // Reads back one render-graph texture; changes no observable state.
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Read back one intermediate render target as a PNG image — THE tool for rendering-feature "
                "development: inspect depth, normals, G-buffer albedo/emissive, shadow maps, AO, bloom, or "
                "any post-process stage directly instead of guessing from the final frame. 'name' is a "
                "render-graph resource name from olo_render_list_targets (e.g. SceneColor, SceneDepth, "
                "GBufferNormal, ShadowMapCSM, AOBuffer, BloomColor). Float/HDR sources are clamped to "
                "[0,1]; depth is min-max normalised by default ('normalize' overrides). Returns metadata "
                "(format, size, value range, and the 'frameIndex' the pixels came from) plus the image. "
                "STALENESS: the target holds whatever was LAST rendered into it — right after an "
                "olo_scene_open the previous scene's pixels come back byte-identical with no error. Pass "
                "'forceFrame':true to render and settle a fresh frame first, or compare 'frameIndex' "
                "between calls (an identical value means you read the same frame twice). For the NUMBERS "
                "under one pixel rather than a picture, use olo_render_probe_pixel. ARRAY TARGETS (the CSM "
                "cascade array 'ShadowMapCSM', the raw-depth views 'ShadowCSMRaw' / 'ShadowAtlasRaw'): pass "
                "'layer' to pick one cascade — olo_render_list_targets reports each array target's 'layers' "
                "count, and an out-of-range layer is an error, never a silent layer-0 capture. MID-FRAME "
                "STATE: pass 'afterPass' to capture the resource AS OF that pass's execution instead of "
                "end-of-frame — decisive when a later pass overwrites it (ParticlePass re-exports "
                "SceneDepth after GTAOPass, so an end-of-frame SceneDepth can never show what GTAO "
                "sampled). Pass names come from olo_render_graph_topology_export's executionOrder. "
                "PIXEL-SCALE MEASUREMENT: the whole target is rescaled to 'maxWidth' (max 4096), which "
                "destroys any spatial period you might want to measure — pass 'region' {x,y,w,h} to read "
                "back a sub-rectangle at NATIVE resolution instead. The reply's meta.region.nativeResolution "
                "says whether the returned PNG is genuinely 1:1.";
            tool.InputSchema = Schema::Object()
                                   .Prop("name", Schema::String().Desc("Render-graph resource name (see olo_render_list_targets)."))
                                   .Prop("mip", Schema::Int().Min(0).Max(16).Desc("Mip level to capture (default 0)."))
                                   .Prop("layer", Schema::Int().Min(0).Max(64).Desc("Texture-array layer (e.g. CSM cascade 0..3), cubemap face (0..5 = +X,-X,+Y,-Y,+Z,-Z), or 3D-volume z-slice (e.g. the froxel fog volumes). Default 0, or the resource's own layer when it is a per-layer view. Out of range is an error."))
                                   .Prop("face", Schema::Int().Min(0).Max(64).Desc("Alias of 'layer' (the original spelling); give only one."))
                                   .Prop("normalize", Schema::Bool().Desc("Min-max normalise float values to [0,1] before encoding (default: true for depth, false otherwise)."))
                                   .Prop("maxWidth", Schema::Int().Min(16).Max(4096).Desc("Max output width in pixels (default 1024); aspect ratio preserved."))
                                   .Prop("region", CaptureRegionArg::SchemaNode())
                                   .Prop("forceFrame", Schema::Bool().Desc("Render and settle a fresh frame before capturing (default false). Use after any change (scene open, setting flip) so you cannot read a stale target. Implied by 'afterPass'."))
                                   .Prop("afterPass", Schema::String().Desc("Capture the resource AS OF this pass's execution (mid-frame snapshot), not end-of-frame. A pass name from olo_render_graph_topology_export's executionOrder; a culled/unknown pass is an error."))
                                   .Prop("delivery", Schema::String().Enum({ "inline", "resource_link" }).Desc("How to return the PNG: 'inline' (default) embeds a base64 image block; 'resource_link' publishes an ephemeral olo://capture resource and returns a link to fetch via resources/read — for large captures."))
                                   .Required({ "name" })
                                   .NoAdditional();
            // outputSchema describes the capture-meta object (the text block); the
            // PNG stays an image content block, which structuredContent cannot carry.
            tool.OutputSchema = Schema::Object()
                                    .Prop("frameIndex", Schema::Int().Min(0).Desc("Frame the pixels came from (compare between calls to detect a stale read)."))
                                    .Prop("timestampMs", Schema::Int().Min(0).Desc("Wall-clock capture stamp, ms since epoch."))
                                    .Prop("stale", Schema::Bool().Desc("The editor's loop was parked, so this capture is the last frame drawn before it stopped — not a current one."))
                                    .Prop("liveness", EditorLiveness::SchemaNode())
                                    .Prop("name", Schema::String().Desc("Captured render-graph resource name."))
                                    .Prop("afterPass", Schema::String().Desc("Mid-frame snapshot pass; omitted unless 'afterPass' was given."))
                                    .Prop("snapshotSourceNativeHandle", Schema::String().Desc("Backend-native handle of the afterPass snapshot's SOURCE texture, as hex; display only. Omitted without 'afterPass'."))
                                    .Prop("snapshotSourceIdentity", Schema::String().Desc("RHI identity of the afterPass snapshot's SOURCE texture; omitted without 'afterPass'."))
                                    .Prop("frameIndexNote", Schema::String().Desc("afterPass frameIndex semantics; omitted without 'afterPass'."))
                                    .Prop("layer", Schema::Int().Min(0).Desc("GL array layer / cube face / z-slice actually read."))
                                    .Prop("layers", Schema::Int().Desc("Layer count; array/cube/3D targets only."))
                                    .Prop("layerNote", Schema::String().Desc("Layer-selection note; omitted when there is nothing to flag."))
                                    .Prop("width", Schema::Int().Desc("Output PNG width."))
                                    .Prop("height", Schema::Int().Desc("Output PNG height."))
                                    .Prop("sourceWidth", Schema::Int().Desc("Full mip width (NOT the region's)."))
                                    .Prop("sourceHeight", Schema::Int().Desc("Full mip height (NOT the region's)."))
                                    .Prop("region", Schema::Object().Desc("The rect actually read {x, y, w, h} (the whole mip when none was requested) plus 'nativeResolution' — whether the returned PNG is 1:1 with it."))
                                    .Prop("format", Schema::String())
                                    .Prop("isDepth", Schema::Bool())
                                    .Prop("normalized", Schema::Bool().Desc("Min-max normalisation was applied."))
                                    .Prop("minValue", Schema::Number().Desc("Pre-normalisation minimum; present only when max > min."))
                                    .Prop("maxValue", Schema::Number().Desc("Pre-normalisation maximum; present only when max > min."))
                                    .Prop("forcedFreshFrame", Schema::Bool())
                                    .Prop("warning", Schema::String().Desc("Fresh-frame timeout warning; omitted otherwise."))
                                    .Prop("note", Schema::String().Desc("Staleness guidance; omitted when forceFrame was used."))
                                    .Prop("resourceUri", Schema::String().Desc("Present only with delivery:'resource_link' — the olo://capture/... resource holding the PNG."))
                                    .Required({ "frameIndex", "timestampMs", "name", "layer", "width", "height",
                                                "sourceWidth", "sourceHeight", "format", "isDepth", "normalized",
                                                "forcedFreshFrame" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_RenderCaptureTarget;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_render_toggle_pass";
            tool.Toolset = "render";
            tool.Title = "Toggle render pass";
            // Edits ephemeral session render settings; flips state when 'enabled' is
            // omitted (so not idempotent), destroys nothing.
            tool.Annotations = MutatingAnnotations(/*idempotent*/ false);
            tool.Description =
                "Flip a post-process / fog feature on or off — the rendering A/B loop: toggle off, "
                "olo_screenshot, toggle on, olo_screenshot, compare. 'name' is one of bloom, ssao, gtao, "
                "ssr, ssgi, fxaa, taa, vignette, chromaticaberration (ca), depthoffield (dof), motionblur, "
                "colorgrading, autoexposure, fog, fogscattering, fogvolumetric, godrays. 'enabled' sets the "
                "state explicitly; omit it to flip the current value. Returns the affected pass and its "
                "new/previous state. Enabling ssao/gtao also selects that AO technique (they share one "
                "slot); ssr/ssgi render only in the Deferred path and the fog sub-features need fog enabled "
                "— a 'note' flags these. The change is EPHEMERAL: it edits the renderer's session-global "
                "settings, not the scene, so it is never saved and a scene reload restores it. Call with no "
                "arguments to list every pass with its current enabled state.";
            tool.InputSchema = Schema::Object()
                                   .Prop("name", Schema::String().Desc("Pass token (e.g. 'bloom', 'ssao', 'ssr', 'fog', 'godrays'). Omit to list all passes + state."))
                                   .Prop("enabled", Schema::Bool().Desc("Desired state. Omit to toggle (flip the current value)."))
                                   .NoAdditional();
            // Two result shapes (toggle vs introspection), so no field is
            // unconditionally present and nothing is required.
            tool.OutputSchema = Schema::Object()
                                    .Prop("pass", Schema::String().Desc("Canonical token of the affected pass (toggle form only)."))
                                    .Prop("enabled", Schema::Bool().Desc("State after the flip (toggle form only)."))
                                    .Prop("previous", Schema::Bool().Desc("State before the flip (toggle form only)."))
                                    .Prop("changed", Schema::Bool().Desc("enabled != previous (toggle form only)."))
                                    .Prop("note", Schema::String().Desc("Precondition hint (AO technique switched, Deferred-only, fog disabled); omitted when none applies."))
                                    .Prop("passes", Schema::Array(Schema::Object()
                                                                      .Prop("name", Schema::String())
                                                                      .Prop("description", Schema::String())
                                                                      .Prop("enabled", Schema::Bool()))
                                                        .Desc("Introspection form (no 'name' argument) only: every toggleable pass with its live state."))
                                    .Prop("activeAOTechnique", Schema::String().Desc("Introspection form only: 'none', 'ssao', or 'gtao'."));
            tool.MainMarshaled = true;
            tool.Handler = Handle_RenderTogglePass;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_postprocess_settings_get";
            tool.Toolset = "render";
            tool.Title = "Read post-process / AO / fog settings";
            // Pure read of two POD settings structs; changes nothing.
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Read the renderer's live post-process, ambient-occlusion and fog parameters — the whole Post "
                "Processing panel as JSON, which no other tool exposes (olo_renderer_settings_set covers only "
                "upscale/tonemap/renderpath/depthprepass/softshadows). Call with no arguments for every field "
                "with its current value, type, range and description; 'group' narrows to one block (ao, bloom, "
                "ssr, ssgi, contactshadow, fog, exposure, dof, antialiasing, motionblur, vignette, sharpen, "
                "colorgrading, chromaticaberration, debug); 'field' returns one field. Field tokens are the C++ "
                "field names (ActiveAOTechnique, GTAORadius, SSAOBias, FogDensity), matched case- and "
                "separator-insensitively. This is the READ half — olo_postprocess_settings_set writes, and is "
                "gated behind 'Allow writes'; reading never is, so parameter values no longer have to be read "
                "off a screenshot of the panel.";
            tool.InputSchema = PostProcess::GetInputSchema();
            // Two shapes: a single-field record, or the fields[]+groups[] listing.
            tool.OutputSchema = Schema::Object()
                                    .Prop("fields", Schema::Array(Schema::Object()
                                                                      .Prop("field", Schema::String())
                                                                      .Prop("group", Schema::String())
                                                                      .Prop("type", Schema::String().Desc("boolean | integer | number | enum | vec3."))
                                                                      .Prop("value", Schema::Raw(Json{ { "type", Json::array({ "boolean", "number", "string", "array" }) } }))
                                                                      .Prop("min", Schema::Number().Desc("Numeric fields only."))
                                                                      .Prop("max", Schema::Number().Desc("Numeric fields only."))
                                                                      .Prop("values", Schema::Array(Schema::Object().Prop("token", Schema::String()).Prop("description", Schema::String())).Desc("Enum fields only."))
                                                                      .Prop("rebuildsRenderGraph", Schema::Bool().Desc("Writing this field re-registers passes; omitted when false."))
                                                                      .Prop("description", Schema::String()))
                                                        .Desc("Listing shape: every field (optionally filtered by 'group')."))
                                    .Prop("groups", Schema::Array(Schema::String()).Desc("Listing shape: the group catalogue."))
                                    .Prop("note", Schema::String().Desc("Listing shape: which settings live on olo_renderer_settings_set instead."))
                                    .Prop("field", Schema::String().Desc("Single-field shape: the field token."))
                                    .Prop("group", Schema::String().Desc("Single-field shape."))
                                    .Prop("type", Schema::String().Desc("Single-field shape."))
                                    .Prop("value", Schema::Raw(Json{ { "type", Json::array({ "boolean", "number", "string", "array" }) } }).Desc("Single-field shape: the live value."));
            tool.MainMarshaled = true;
            tool.Handler = Handle_PostProcessSettingsGet;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_postprocess_settings_set";
            tool.Toolset = "render";
            tool.Title = "Set post-process / AO / fog setting";
            // Mutates the session-global post-process / fog settings — the same
            // read-only line olo_renderer_settings_set crosses, so the same gate.
            // Idempotent (writing a value twice leaves the same state); not
            // destructive (fully reversible via the reported previousValue).
            tool.ProjectWrite = true;
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Write one post-process / ambient-occlusion / fog parameter to verify a rendering change LIVE — "
                "the part of the renderer olo_renderer_settings_set never reached. THE motivating case: "
                "'ActiveAOTechnique' (none|ssao|gtao) makes the same-scene, same-pose GTAO-vs-SSAO A/B two "
                "calls — one per technique — rather than a scene edit and relaunch. Also reaches every AO/GTAO/SSAO parameter, the "
                "*DebugView flags, bloom, DOF, "
                "TAA, SSR, SSGI, contact shadows, exposure/auto-exposure and the whole fog block. 'field' is a "
                "C++ field name (GTAORadius, SSAOBias, FogDensity), case- and separator-insensitive; 'value' is "
                "a boolean, a number (CLAMPED to the field's declared range — the reply reports 'clamped'), an "
                "enum token, or a 3-number array for a colour. Call with NO arguments to list every field with "
                "its current value (same payload as olo_postprocess_settings_get). Tone-map operator, FSR1 "
                "upscale preset, rendering path, depth prepass and soft shadows live on "
                "olo_renderer_settings_set — one write path per field. The change is session-global and "
                "ephemeral (a scene reload restores it); the reply reports 'previousValue' so you restore by "
                "calling again with it — restore-prior-value, NOT an undo-stack entry. This is a WRITE tool: "
                "refused unless 'Allow writes' is enabled in the editor's MCP Server panel (off by default).";
            tool.InputSchema = PostProcess::SetInputSchema();
            // Two disjoint shapes — introspection (no arguments) returns the
            // fields[] listing; an apply returns the ack fields.
            tool.OutputSchema = Schema::Object()
                                    .Prop("fields", Schema::Array(Schema::Object()).Desc("Introspection shape only (no arguments): every field with its live value."))
                                    .Prop("groups", Schema::Array(Schema::String()).Desc("Introspection shape only."))
                                    .Prop("field", Schema::String().Desc("Apply shape: the field written."))
                                    .Prop("group", Schema::String().Desc("Apply shape: its group."))
                                    .Prop("previousValue", Schema::Raw(Json{ { "type", Json::array({ "boolean", "number", "string", "array" }) } }).Desc("Apply shape: the prior value — pass it back to revert."))
                                    .Prop("value", Schema::Raw(Json{ { "type", Json::array({ "boolean", "number", "string", "array" }) } }).Desc("Apply shape: the value actually stored (post-clamp)."))
                                    .Prop("changed", Schema::Bool().Desc("Apply shape: value != previousValue."))
                                    .Prop("clamped", Schema::Bool().Desc("Apply shape: the request was outside the field's range and was clamped."))
                                    .Prop("range", Schema::Object().Prop("min", Schema::Number()).Prop("max", Schema::Number()).Desc("Apply shape: present only when clamped."))
                                    .Prop("restoreWith", Schema::Raw(Json{ { "type", Json::array({ "boolean", "number", "string", "array" }) } }).Desc("Apply shape: alias of previousValue."));
            tool.MainMarshaled = true;
            tool.Handler = Handle_PostProcessSettingsSet;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_render_transient_plan";
            tool.Toolset = "render";
            tool.Title = "Render-graph transient plan + pool";
            // Pure read of the plan/pool bookkeeping; changes nothing.
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Dump the render graph's TRANSIENT PLAN and the pool state behind it — the layer where the "
                "resource-aliasing decisions are made, which olo_render_graph_topology_export (per-pass "
                "resolved ids) does not show. Per plan entry: alias group + slot, whether it allocates a GPU "
                "object (and the planner's skip reason if not), its FirstPass->LastPass lifetime, its resolved "
                "physical identity (plus the display-only native handle), what it is a version-alias OF, and the "
                "poison hue it would leak as. Per pool: "
                "bucket descriptors with free counts, estimated/acquired bytes, and this frame's ACQUIRE ORDER "
                "(unsorted — two entries sharing an identity shared one GPU object). Use it when a target shows "
                "one-frame garbage after a plan rebuild, when you suspect two live resources share a physical, "
                "or to check that a versioned name (SceneColor@SomePass) resolves to its base's physical rather "
                "than an orphan — the exact question behind the one-frame black-square artifact, which "
                "previously required rebuilding the engine with hand-rolled instrumentation. Pair it with "
                "olo_render_debug_set { poisonTransients: true } to turn a stochastic artifact into a "
                "deterministic per-resource-coloured one.";
            tool.InputSchema = Schema::Object()
                                   .Prop("resource", Schema::String().Desc("Only report plan entries whose resource name CONTAINS this substring (case-sensitive). Omit for the whole plan."))
                                   .Prop("includePool", Schema::Bool().Desc("Include the TransientPool bucket/size report (default true)."))
                                   .Prop("includeAcquireOrder", Schema::Bool().Desc("Include this frame's pool acquisition order (default true; ignored when includePool is false)."))
                                   .NoAdditional();
            tool.OutputSchema = Schema::Object()
                                    .Prop("entries", Schema::Array(Schema::Object()
                                                                       .Prop("resource", Schema::String())
                                                                       .Prop("kind", Schema::String())
                                                                       .Prop("reachable", Schema::Bool())
                                                                       .Prop("willAllocate", Schema::Bool())
                                                                       .Prop("aliasGroup", Schema::String())
                                                                       .Prop("aliasSlot", Schema::Int().Desc("Omitted when the planner assigned none."))
                                                                       .Prop("estimatedBytes", Schema::Int())
                                                                       .Prop("firstPass", Schema::String())
                                                                       .Prop("lastPass", Schema::String())
                                                                       .Prop("firstPassIndex", Schema::Int().Desc("Index into the execution order; omitted when never written."))
                                                                       .Prop("lastPassIndex", Schema::Int())
                                                                       .Prop("skipReason", Schema::String().Desc("Why the planner did not allocate; omitted when it did."))
                                                                       .Prop("versionAliasOf", Schema::String().Desc("Source resource this versioned name renames; omitted for a base name."))
                                                                       .Prop("nativeTexture", Schema::String().Desc("Backend-native texture handle as hex; display only, omitted when 0."))
                                                                       .Prop("identity", Schema::String().Desc("Resolved physical object as \"#index:generation\" - compare THIS to tell whether two entries got the same GPU object. Omitted when unbacked this frame."))
                                                                       .Prop("poisonColor", Schema::String().Desc("Hue this resource is cleared to under poisonTransients."))))
                                    .Prop("planSize", Schema::Int().Min(0).Desc("Total plan entries before any 'resource' filter."))
                                    .Prop("topologyGeneration", Schema::Int().Min(0).Desc("Bumped on every topology teardown — a change means the plan was rebuilt."))
                                    .Prop("debugFlags", Schema::Object().Prop("poisonTransients", Schema::Bool()).Prop("disableAliasing", Schema::Bool()))
                                    .Prop("resourceFilter", Schema::String().Desc("Echo of the 'resource' filter; omitted when none."))
                                    .Prop("pool", Schema::Object().Desc("TransientPool state; omitted when includePool is false."))
                                    .Required({ "entries", "planSize", "topologyGeneration", "debugFlags" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_RenderTransientPlan;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_render_debug_set";
            tool.Toolset = "render";
            tool.Title = "Set render-graph debug instruments";
            // Flips session-global renderer diagnostics — the same read-only line
            // the other session-setting writes cross, so the same gate. Idempotent;
            // not destructive (fully reversible via the reported 'restoreWith').
            tool.ProjectWrite = true;
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Flip the render graph's two transient-corruption instruments LIVE, instead of setting "
                "OLO_RG_POISON_TRANSIENTS / OLO_RG_DISABLE_ALIASING and restarting the editor. "
                "'poisonTransients' clears every pool-acquired transient to a per-resource hue at materialize "
                "time, so any texel that reaches a consumer WITHOUT being written this frame is unmistakable "
                "and its colour names the resource it leaked from — this is what turned a ~3% stochastic "
                "camera-move artifact into a deterministic every-frame signal, and it doubles as the "
                "fix-verification probe. The reply carries the whole resource->colour map up front (the engine "
                "otherwise logs it one line per resource as each is first materialized). 'disableAliasing' "
                "gives every transient its own physical backing; if an artifact disappears under it, the "
                "planner's lifetime analysis let two live resources share one GPU object — and the pool is "
                "evicted on the flip so the A/B is not comparing a mixed state. Call with no arguments to read "
                "the current flags. Both take effect from the next rendered frame (this call settles two), and "
                "are session-global and ephemeral; the reply's 'restoreWith' puts them back. Read the plan they "
                "act on with olo_render_transient_plan. This is a WRITE tool: refused unless 'Allow writes' is "
                "enabled in the editor's MCP Server panel (off by default).";
            tool.InputSchema = Schema::Object()
                                   .Prop("poisonTransients", Schema::Bool().Desc("Clear every pool-acquired transient to its per-resource hue at materialize time. Omit to leave unchanged."))
                                   .Prop("disableAliasing", Schema::Bool().Desc("Give every transient its own physical backing (no alias-slot sharing). Omit to leave unchanged."))
                                   .NoAdditional();
            tool.OutputSchema = Schema::Object()
                                    .Prop("poisonTransients", Schema::Bool().Desc("State after the call."))
                                    .Prop("disableAliasing", Schema::Bool().Desc("State after the call."))
                                    .Prop("previous", Schema::Object().Prop("poisonTransients", Schema::Bool()).Prop("disableAliasing", Schema::Bool()))
                                    .Prop("changed", Schema::Bool().Desc("Either flag actually differs from before."))
                                    .Prop("restoreWith", Schema::Object().Prop("poisonTransients", Schema::Bool()).Prop("disableAliasing", Schema::Bool()).Desc("Call again with these to restore."))
                                    .Prop("poisonColorMap", Schema::Array(Schema::Object().Prop("resource", Schema::String()).Prop("color", Schema::String())).Desc("Present only while poisonTransients is on: every plan resource and the hue it is cleared to."))
                                    .Prop("poisonNote", Schema::String().Desc("How to read a poisoned frame; present only while poisonTransients is on."))
                                    .Required({ "poisonTransients", "disableAliasing", "previous", "changed", "restoreWith" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_RenderDebugSet;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_render_set_debug_view";
            tool.Toolset = "render";
            tool.Title = "Set render debug view";
            // Edits ephemeral session render settings; destroys nothing.
            tool.Annotations = MutatingAnnotations(/*idempotent*/ false);
            tool.Description =
                "Switch the viewport to a raw intermediate buffer for AO/reflection/GI/overdraw/virtual-geometry "
                "debugging. 'mode' is one of none (the normal composite), ssao, gtao, ssr, ssgi, overdraw, "
                "vgclusterid, vglod, vgoverdraw — exactly one is shown at a time; mode 'none' (or "
                "'enabled':false) clears them all. 'overdraw' heat-maps per-pixel fragment count (how many "
                "layers deep the frame is: black=none, blue/green/yellow/red=increasing overlap) by re-drawing "
                "opaque geometry with depth test off + additive blend; it needs no backing pass and works on "
                "every rendering path. The three vg* modes are the virtualized-geometry (Nanite-style) "
                "visualisations — cluster id / DAG LOD level / cluster overdraw — which render into the "
                "'VirtualGeometryDebug' target (Deferred path only): set the mode, then capture it with "
                "olo_render_capture_target; the response's 'captureTarget' says so. They are the SAME knob as "
                "olo_virtual_geometry_set { debugMode }, so the two tools always agree on the current state. "
                "Returns the active mode, the *DebugView flag states, the virtual-geometry debug mode, and "
                "'passEnabled' — whether the pass that produces the chosen buffer is actually running this "
                "frame (with an actionable 'note' if not, e.g. enable SSAO first with olo_render_toggle_pass). "
                "The change is EPHEMERAL: it edits the renderer's session-global settings, not the scene, so "
                "it is never saved and a scene reload restores it. Call with no arguments to list the modes + "
                "current state.";
            tool.InputSchema = Schema::Object()
                                   .Prop("mode", Schema::String().Enum({ "none", "ssao", "gtao", "ssr", "ssgi", "overdraw", "vgclusterid", "vglod", "vgoverdraw" }).Desc("Debug view to show. 'none' clears all. The vg* modes write to the 'VirtualGeometryDebug' capture target. Omit to list modes + state."))
                                   .Prop("enabled", Schema::Bool().Desc("Set false as an alias for mode:'none' (clear all debug views)."))
                                   .NoAdditional();
            // Two result shapes (set vs introspection), so no field is
            // unconditionally present and nothing is required.
            tool.OutputSchema = Schema::Object()
                                    .Prop("mode", Schema::String().Desc("Active debug view token (set form only)."))
                                    .Prop("ssaoDebugView", Schema::Bool())
                                    .Prop("gtaoDebugView", Schema::Bool())
                                    .Prop("ssrDebugView", Schema::Bool())
                                    .Prop("ssgiDebugView", Schema::Bool())
                                    .Prop("overdrawDebugView", Schema::Bool())
                                    .Prop("virtualGeometryDebugMode", Schema::String().Desc("'off', 'clusterid', 'lod', or 'overdraw' — mirrors olo_virtual_geometry_set."))
                                    .Prop("passEnabled", Schema::Bool().Desc("The pass producing the chosen buffer is running this frame."))
                                    .Prop("captureTarget", Schema::String().Desc("Render-graph target to capture for this view; omitted when none applies."))
                                    .Prop("note", Schema::String().Desc("Actionable hint when passEnabled is false; omitted otherwise."))
                                    .Prop("modes", Schema::Array(Schema::Object()
                                                                     .Prop("name", Schema::String())
                                                                     .Prop("description", Schema::String()))
                                                       .Desc("Introspection form (no arguments) only: every debug-view mode."))
                                    .Prop("current", Schema::Object().Desc("Introspection form only: the live state in the set-form shape."));
            tool.MainMarshaled = true;
            tool.Handler = Handle_RenderSetDebugView;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_scene_set_time_of_day";
            tool.Toolset = "render";
            tool.Title = "Set time of day (TimeOfDayComponent)";
            // Writes the scene's SERIALIZED TimeOfDayComponent — a project write
            // (issue #633; the ephemeral sun override this tool used to drive is
            // retired), gated behind the session write consent like
            // olo_entity_set_field. Same values -> same state (idempotent); a
            // plain field set, not an undo-stack entry.
            tool.ProjectWrite = true;
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Set the scene's time-of-day clock for lighting iteration — writes the scene's "
                "TimeOfDayComponent (the serialized time-of-day clock and single authoritative sun source; "
                "the old ephemeral renderer override is retired), so TimeOfDaySystem recomputes the sun/moon "
                "ephemeris and drives the directional light + sky on the next frame, in edit and play mode "
                "alike. 'hours' is a 24-hour clock time in [0,24) (0 = midnight, 12 = noon; 24 wraps to 0); "
                "the other fields tune the ephemeris ('dayOfYear', 'latitudeDegrees') and the clock "
                "('timeScale', 'paused', 'enabled'). At least one field is required. Returns the resulting "
                "component state plus the derived sunElevationDegrees / isNight / sun+moon directions. The "
                "write edits the loaded scene IN MEMORY (persisted only when the scene is saved) and is not "
                "an undo-stack entry. Requires a TimeOfDayComponent in the scene — the error says how to add "
                "one when missing. To read without writing, use olo_scene_get_atmosphere. This is a WRITE "
                "tool: refused unless agent writes are enabled in the editor's MCP Server panel (off by "
                "default).";
            tool.InputSchema = Schema::Object()
                                   .Prop("hours", Schema::Number().Min(0).Max(24).Desc("Time of day on a 24-hour clock, [0, 24) (0=midnight, 6=morning, 12=noon, 18=evening; 24 wraps to 0)."))
                                   .Prop("dayOfYear", Schema::Int().Min(1).Max(365).Desc("Day of the year driving the solar declination (172 ~ June solstice, 355 ~ December solstice)."))
                                   .Prop("latitudeDegrees", Schema::Number().Min(-90).Max(90).Desc("Observer latitude in degrees (positive = northern hemisphere)."))
                                   .Prop("timeScale", Schema::Number().Min(0).Max(1000).Desc("Extra multiplier on the clock's advance while playing (0 = frozen)."))
                                   .Prop("paused", Schema::Bool().Desc("Pause/resume the clock's advance."))
                                   .Prop("enabled", Schema::Bool().Desc("Enable/disable the component (disabled = TimeOfDaySystem stops driving the sun)."))
                                   .Prop("clear", Schema::Bool().Desc("Legacy no-op from the retired override interface: returns the current state + a note (the component is authoritative; there is nothing to clear)."))
                                   .NoAdditional();
            // The legacy 'clear':true path succeeds with ONLY 'note' when no
            // TimeOfDayComponent exists, so no field is unconditionally present.
            tool.OutputSchema = Schema::Object()
                                    .Prop("enabled", Schema::Bool())
                                    .Prop("hours", Schema::Number().Desc("24-hour clock time in [0, 24)."))
                                    .Prop("dayOfYear", Schema::Int())
                                    .Prop("latitudeDegrees", Schema::Number())
                                    .Prop("timeScale", Schema::Number())
                                    .Prop("paused", Schema::Bool())
                                    .Prop("sunElevationDegrees", Schema::Number().Desc("Derived sun elevation in degrees."))
                                    .Prop("isNight", Schema::Bool().Desc("Derived night flag."))
                                    .Prop("sunDirection", Schema::Vec3("Derived [x, y, z] sun direction."))
                                    .Prop("moonDirection", Schema::Vec3("Derived [x, y, z] moon direction."))
                                    .Prop("note", Schema::String().Desc("Disabled-component warning or legacy-clear explanation; omitted otherwise."));
            tool.MainMarshaled = true;
            tool.Handler = Handle_SceneSetTimeOfDay;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_scene_set_sun_angle";
            tool.Toolset = "render";
            tool.Title = "Set sun angle (solve time of day)";
            // Writes the scene's SERIALIZED TimeOfDayComponent (the solved hours)
            // — a project write like olo_scene_set_time_of_day above; same angles
            // -> same solved time (idempotent).
            tool.ProjectWrite = true;
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Aim the sun from a yaw/pitch pair — the angle-first sibling of olo_scene_set_time_of_day. "
                "The TimeOfDayComponent's ephemeris is the single sun source (the old direct-direction "
                "override is retired), so this SOLVES for the time of day whose sun best matches the request "
                "and writes the solved hours into the component. 'yaw' is the azimuth in degrees (measured "
                "from +Z toward +X: 0=+Z/north, 90=+X/east, 180=south, 270=west) and 'pitch' is the "
                "elevation in degrees in [-90,90]; both are required. The pitch is matched exactly when the "
                "component's day/latitude can reach it; the yaw is honoured only for its east/west side "
                "(east = morning, west = afternoon) since one clock knob cannot match both angles. Returns "
                "the resulting component state plus 'achievedElevationDeg' and 'clamped' — true (with a "
                "'note') when the requested elevation is outside the day's range and the closest achievable "
                "sun was used. The write edits the loaded scene IN MEMORY (persisted only when the scene is "
                "saved). Requires a TimeOfDayComponent in the scene. 'clear':true is a legacy no-op that "
                "returns a note. This is a WRITE tool: refused unless agent writes are enabled in the "
                "editor's MCP Server panel (off by default).";
            tool.InputSchema = Schema::Object()
                                   .Prop("yaw", Schema::Number().Desc("Azimuth in degrees (0=+Z/north, 90=+X/east, 180=south, 270=west). Only the east/west side is honoured — see the description."))
                                   .Prop("pitch", Schema::Number().Min(-90).Max(90).Desc("Elevation in degrees above the horizon (90=up, 0=horizon, negative=below). Matched exactly when achievable, else clamped."))
                                   .Prop("clear", Schema::Bool().Desc("Legacy no-op from the retired override interface: returns the current state + a note (the component is authoritative; there is nothing to clear)."))
                                   .NoAdditional();
            // The legacy 'clear':true path succeeds with ONLY 'note' when no
            // TimeOfDayComponent exists, so no field is unconditionally present.
            tool.OutputSchema = Schema::Object()
                                    .Prop("enabled", Schema::Bool())
                                    .Prop("hours", Schema::Number().Desc("Solved 24-hour clock time written to the component."))
                                    .Prop("dayOfYear", Schema::Int())
                                    .Prop("latitudeDegrees", Schema::Number())
                                    .Prop("timeScale", Schema::Number())
                                    .Prop("paused", Schema::Bool())
                                    .Prop("sunElevationDegrees", Schema::Number().Desc("Derived sun elevation at the solved time."))
                                    .Prop("isNight", Schema::Bool().Desc("Derived night flag."))
                                    .Prop("sunDirection", Schema::Vec3("Derived [x, y, z] sun direction."))
                                    .Prop("moonDirection", Schema::Vec3("Derived [x, y, z] moon direction."))
                                    .Prop("achievedElevationDeg", Schema::Number().Desc("Elevation the solved time actually yields."))
                                    .Prop("clamped", Schema::Bool().Desc("True when the requested elevation was outside the day's range."))
                                    .Prop("note", Schema::String().Desc("Clamp explanation, disabled-component warning, or legacy-clear explanation; omitted otherwise."));
            tool.MainMarshaled = true;
            tool.Handler = Handle_SceneSetSunAngle;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_scene_set_weather";
            tool.Toolset = "render";
            tool.Title = "Set weather state";
            // Writes the scene's SERIALIZED WeatherStateComponent — a project
            // write like the two sun tools above; same state -> same result
            // (idempotent).
            tool.ProjectWrite = true;
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Drive the scene's weather director — writes the WeatherStateComponent's target state so "
                "WeatherSystem cross-blends clouds / fog / wind / precipitation / snow accumulation / "
                "wetness toward the named state. 'state' is one of Clear, Overcast, Rain, Storm, Snow, "
                "FogBank (case-sensitive). 'transitionSeconds' overrides the component's authored cross-"
                "blend duration (0 = instant); omit it to keep the authored value. 'immediate':true snaps "
                "current = target with the transition already settled. The blended result is applied to the "
                "scene + renderer settings right away (WeatherSystem::ApplyImmediate — the editor "
                "inspector's preview path), so edit mode reflects it without a play tick. Returns "
                "currentState / targetState / transitionDuration / transitionProgress / wetness. The write "
                "edits the loaded scene IN MEMORY (persisted only when the scene is saved). Requires a "
                "WeatherStateComponent in the scene — the error says how to add one when missing. This is a "
                "WRITE tool: refused unless agent writes are enabled in the editor's MCP Server panel (off "
                "by default).";
            tool.InputSchema = Schema::Object()
                                   .Prop("state", Schema::String().Enum({ "Clear", "Overcast", "Rain", "Storm", "Snow", "FogBank" }).Desc("Target weather state (case-sensitive)."))
                                   .Prop("transitionSeconds", Schema::Number().Min(0).Max(600).Desc("Cross-blend duration in seconds (0 = instant). Omit to keep the component's authored duration."))
                                   .Prop("immediate", Schema::Bool().Desc("true = snap to 'state' with no transition (current = target, progress settled)."))
                                   .Required({ "state" })
                                   .NoAdditional();
            tool.OutputSchema = Schema::Object()
                                    .Prop("currentState", Schema::String().Enum({ "Clear", "Overcast", "Rain", "Storm", "Snow", "FogBank" }))
                                    .Prop("targetState", Schema::String().Enum({ "Clear", "Overcast", "Rain", "Storm", "Snow", "FogBank" }))
                                    .Prop("transitionDuration", Schema::Number().Desc("Cross-blend duration in seconds."))
                                    .Prop("transitionProgress", Schema::Number().Desc("Blend progress (1.0 when settled/immediate)."))
                                    .Prop("wetness", Schema::Number())
                                    .Prop("note", Schema::String().Desc("Disabled-component warning; omitted when the component is enabled."))
                                    .Required({ "currentState", "targetState", "transitionDuration", "transitionProgress", "wetness" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_SceneSetWeather;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_scene_get_atmosphere";
            tool.Toolset = "render";
            tool.Title = "Get atmosphere state";
            // Pure component read; changes nothing.
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Read the scene's atmosphere state in one call — the read half of olo_scene_set_time_of_day "
                "/ olo_scene_set_sun_angle / olo_scene_set_weather. Reports a 'timeOfDay' block (hours, "
                "dayOfYear, latitudeDegrees, timeScale, paused, plus the derived sunElevationDegrees / "
                "isNight / sun+moon directions), a 'weather' block (current/target state names, "
                "transitionDuration, transitionProgress, wetness, blended cloud coverage), and a "
                "'cloudscape' block (enabled, coverage, layerBottom/layerTop, castCloudShadows). A block is "
                "omitted when its component is absent from the scene; the 'note' lists which components "
                "were found. Read-only.";
            tool.InputSchema = Schema::EmptyObject();
            tool.OutputSchema = Schema::Object()
                                    .Prop("timeOfDay", Schema::Object()
                                                           .Prop("enabled", Schema::Bool())
                                                           .Prop("hours", Schema::Number().Desc("24-hour clock time in [0, 24)."))
                                                           .Prop("dayOfYear", Schema::Int())
                                                           .Prop("latitudeDegrees", Schema::Number())
                                                           .Prop("timeScale", Schema::Number())
                                                           .Prop("paused", Schema::Bool())
                                                           .Prop("sunElevationDegrees", Schema::Number().Desc("Derived sun elevation in degrees."))
                                                           .Prop("isNight", Schema::Bool().Desc("Derived night flag."))
                                                           .Prop("sunDirection", Schema::Vec3("Derived [x, y, z] sun direction."))
                                                           .Prop("moonDirection", Schema::Vec3("Derived [x, y, z] moon direction."))
                                                           .Desc("Omitted when the scene has no TimeOfDayComponent."))
                                    .Prop("weather", Schema::Object()
                                                         .Prop("enabled", Schema::Bool())
                                                         .Prop("currentState", Schema::String())
                                                         .Prop("targetState", Schema::String())
                                                         .Prop("transitionDuration", Schema::Number())
                                                         .Prop("transitionProgress", Schema::Number())
                                                         .Prop("wetness", Schema::Number())
                                                         .Prop("blendedCloudCoverage", Schema::Number())
                                                         .Desc("Omitted when the scene has no WeatherStateComponent."))
                                    .Prop("cloudscape", Schema::Object()
                                                            .Prop("enabled", Schema::Bool())
                                                            .Prop("coverage", Schema::Number())
                                                            .Prop("layerBottom", Schema::Number())
                                                            .Prop("layerTop", Schema::Number())
                                                            .Prop("castCloudShadows", Schema::Bool())
                                                            .Desc("Omitted when the scene has no CloudscapeComponent."))
                                    .Prop("note", Schema::String().Desc("Which atmosphere components were found."))
                                    .Required({ "note" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_SceneGetAtmosphere;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_render_compare_golden";
            tool.Toolset = "render";
            tool.Title = "Compare against golden image";
            // Poses the camera (save/restore) and can WRITE/overwrite a golden PNG
            // on create or rebase — a real, potentially destructive filesystem
            // mutation, so leave destructiveHint at its spec default (true).
            tool.Annotations = DestructiveMutatingAnnotations();
            tool.Description =
                "Capture the editor viewport and diff it against a golden PNG, returning a numeric "
                "similarity + pass/fail verdict — the numeric half of the 'rendering changes MUST be "
                "visually verified' loop: get a deterministic yes/no instead of eyeballing a screenshot. "
                "'goldenPath' is a PNG under assets/tests/visual/ (bare names land there; '..'/absolute "
                "paths are rejected). Optionally pose the camera for this capture only via 'camera' or "
                "'orbit' (same shape as olo_screenshot; the user's camera is saved and restored). If the "
                "golden does not exist (or 'rebase':true), the captured frame is WRITTEN as the new "
                "golden and the tool reports 'created' instead of failing — mirroring the test suite's "
                "--olo-golden-rebase workflow. The verdict uses the same RMSE→SSIM metric as the "
                "GoldenImageTests suite; pass an explicit 'threshold' (min SSIM similarity in [0,1]) to "
                "override the default cascade. Use the SAME capture size when creating and comparing "
                "(set one with olo_viewport_set_size) or the dimensions will mismatch. Returns the "
                "verdict JSON plus the captured frame as an image block.";
            tool.InputSchema = Schema::Object()
                                   .Prop("goldenPath", Schema::String().Desc("Golden PNG path under assets/tests/visual/ (e.g. 'water_side.png'). A '.png' extension is added if missing. Relative only — no '..' or absolute paths."))
                                   .Prop("threshold", Schema::Number().Min(0).Max(1).Desc("Minimum SSIM similarity in [0,1] to pass (1 = identical). Omit to use the suite's RMSE→SSIM cascade verdict (the default, consistent with the golden test suite)."))
                                   .Prop("rebase", Schema::Bool().Desc("true = overwrite the golden with the current capture instead of comparing (re-baseline after a deliberate visual change). A missing golden is always created regardless."))
                                   .Prop("camera", Schema::Object().Desc("Capture from this pose, then restore the prior camera. Same shape as olo_camera_set_pose: position [x,y,z] plus target [x,y,z] or yaw/pitch (degrees); optional fov."))
                                   .Prop("orbit", Schema::Object().Desc("Capture from this orbit pose, then restore. Same shape as olo_camera_orbit: target [x,y,z], yaw/pitch (degrees), distance; optional fov."))
                                   .Prop("settleFrames", Schema::Int().Min(1).Max(30).Desc("Frames to render at the new pose before capturing (default 2). Raise for temporal effects (TAA, fog history) to settle."))
                                   .Prop("maxWidth", Schema::Int().Min(16).Max(4096).Desc("Max capture width in pixels (default 1024); aspect ratio preserved. Must match between create and compare."))
                                   .Prop("delivery", Schema::String().Enum({ "inline", "resource_link" }).Desc("How to return the captured frame: 'inline' (default) embeds a base64 image block; 'resource_link' publishes an ephemeral olo://capture resource and returns a link to fetch via resources/read. The golden FILE write is unaffected."))
                                   .Required({ "goldenPath" })
                                   .NoAdditional();
            // outputSchema describes the verdict JSON (mirrored into
            // structuredContent); the captured frame is a separate image content
            // block outside the structured result. Two shapes: created/rebased
            // vs compared — only the envelope is unconditional.
            tool.OutputSchema = Schema::Object()
                                    .Prop("goldenPath", Schema::String().Desc("Resolved golden PNG path."))
                                    .Prop("created", Schema::Bool().Desc("True when the capture was WRITTEN as the (new/rebased) golden instead of compared."))
                                    .Prop("rebased", Schema::Bool().Desc("Created path only: the golden existed and was overwritten."))
                                    .Prop("bytes", Schema::Int().Min(0).Desc("Created path only: PNG bytes written."))
                                    .Prop("pass", Schema::Bool().Desc("Compare path only: the verdict."))
                                    .Prop("dimensionsMatch", Schema::Bool().Desc("Compare path only; the metric fields below need matching dimensions."))
                                    .Prop("actual", Schema::Object().Prop("width", Schema::Int()).Prop("height", Schema::Int()).Desc("Compare path only: capture dimensions."))
                                    .Prop("golden", Schema::Object().Prop("width", Schema::Int()).Prop("height", Schema::Int()).Desc("Compare path only: golden dimensions."))
                                    .Prop("similarity", Schema::Number())
                                    .Prop("ssim", Schema::Number())
                                    .Prop("rmse", Schema::Number())
                                    .Prop("mse", Schema::Number())
                                    .Prop("threshold", Schema::Number().Desc("Effective minimum-SSIM pass threshold."))
                                    .Prop("thresholdMode", Schema::String().Enum({ "suite-cascade", "explicit" }))
                                    .Prop("mismatchPixels", Schema::Int().Min(0))
                                    .Prop("totalPixels", Schema::Int().Min(0))
                                    .Prop("maxChannelDelta", Schema::Int().Min(0))
                                    .Prop("worstPixel", Schema::Object().Prop("x", Schema::Int()).Prop("y", Schema::Int()))
                                    .Prop("message", Schema::String().Desc("Human-readable verdict / creation message."))
                                    .Prop("warning", Schema::String().Desc("Settle-timeout stale-frame warning; omitted otherwise."))
                                    .Prop("resourceUri", Schema::String().Desc("Present only with delivery:'resource_link' — the olo://capture/... resource holding the captured frame."))
                                    .Required({ "goldenPath", "created", "message" });
            tool.MainMarshaled = true; // reads main-thread-only camera/viewport state (like olo_screenshot)
            tool.Handler = Handle_RenderCompareGolden;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_renderer_settings_set";
            tool.Toolset = "render";
            tool.Title = "Set renderer / post-process setting";
            // A project-WRITE tool (#306): it mutates the session-global
            // renderer settings, which crosses the read-only line, so it is gated
            // behind the "Allow writes" session toggle like the other writes.
            // readOnlyHint:false; idempotent (setting a value twice leaves the same
            // state — the enum-valued sibling of the flip-based toggle_pass); not
            // destructive (fully reversible by setting the reported previousValue).
            tool.ProjectWrite = true;
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Set a multi-valued renderer / post-process setting to verify a rendering feature LIVE at each value — "
                "the enum-valued sibling of the on/off olo_render_toggle_pass. Settings: 'upscale' (FSR1 spatial-upscale "
                "mode: off|quality|balanced|performance|ultraperformance — the #480 case), 'tonemap' (none|reinhard|aces|"
                "uncharted2), 'renderpath' (forward|forwardplus|deferred; switching rebuilds the render graph, and "
                "Deferred is required for SSR/SSGI), plus the two big perf levers (#316): 'depthprepass' (off|on|auto — "
                "forces the live depth-prepass state; 'auto' restores the settings-derived value; Forward+/Deferred "
                "derive it on for tile culling) and 'softshadows' (pcf|pcss — PCSS is the dominant ScenePass cost in "
                "shadowed scenes; A/B it in one call instead of editing shader source). Also exposes 'msaa' (1|2|4|8), "
                "'persamplelighting', 'depthawareculling', 'virtualshadowmaps', 'vsmdebug' (off plus six diagnostic views), "
                "'ddgicascades', and 'hzbocclusion'. Two more drive the hybrid ray-traced shadow tier (#1056): "
                "'raytracedshadows' (off|on — routes opted-in lights through ray-query visibility instead of the shadow "
                "map; Vulkan + Deferred only, and it reports 'rayTracedLights'/'fallbackLights'/'fallbackReason' rather "
                "than an effective bool, because the technique is decided PER LIGHT inside the frame) and "
                "'raytracedsoftness' (sharp|sun|overcast|exaggerated — the light's angular radius, THE knob that makes "
                "the penumbra geometric; sweep it from one camera pose to show contact hardening, and read the chosen "
                "value back as 'angularRadiusDegrees'). Topology-affecting changes rebuild the render graph. Call with NO arguments to list "
                "every setting with its current value and allowed values. The change is session-global and ephemeral (a "
                "scene reload restores it); the response reports 'previousValue' so you can restore by calling again "
                "with that token — this is restore-prior-value, NOT an undo-stack entry (unlike olo_entity_set_field). "
                "This is a WRITE tool: it is refused unless 'Allow writes' is enabled in the editor's MCP Server panel "
                "(off by default).";
            tool.InputSchema = RendererSettings::InputSchema();
            // Two disjoint shapes — introspection (no arguments) returns only
            // 'settings'; an apply returns the ack fields — so no field is
            // unconditionally present and the required list stays empty.
            tool.OutputSchema = Schema::Object()
                                    .Prop("settings", Schema::Array(Schema::Object()
                                                                        .Prop("setting", Schema::String())
                                                                        .Prop("description", Schema::String())
                                                                        .Prop("currentValue", Schema::String())
                                                                        .Prop("values", Schema::Array(Schema::Object()
                                                                                                          .Prop("token", Schema::String())
                                                                                                          .Prop("description", Schema::String()))
                                                                                            .Desc("Allowed-value catalogue.")))
                                                          .Desc("Introspection shape only (called with no arguments): every setting with its live value."))
                                    .Prop("setting", Schema::String().Desc("Apply shape only: the setting written."))
                                    .Prop("previousValue", Schema::String().Desc("Apply shape only: the prior value token — set it back to revert."))
                                    .Prop("value", Schema::String().Desc("Apply shape only: the resulting value token ('auto' already resolved)."))
                                    .Prop("changed", Schema::Bool().Desc("Apply shape only."))
                                    .Prop("restoreWith", Schema::String().Desc("Apply shape only: same as previousValue, the explicit restore hint."))
                                    .Prop("requested", Schema::String().Desc("Apply shape only: 'auto' when depthprepass auto was requested; omitted otherwise."))
                                    // Setting-specific apply fields. Declared because the
                                    // handler populates them: a property a caller receives
                                    // but cannot find in the schema reads as an accident,
                                    // and an agent that validates the response drops it.
                                    .Prop("angularRadiusDegrees", Schema::Number().Desc("Apply shape, 'raytracedsoftness' only: the light angular radius in degrees the chosen preset resolved to."))
                                    .Prop("rayTracedLights", Schema::Int().Desc("Apply shape, 'raytracedshadows' on: lights routed to ray-traced visibility. From the PREVIOUS frame — see 'note'."))
                                    .Prop("fallbackLights", Schema::Int().Desc("Apply shape, 'raytracedshadows' on: lights that asked for it and kept their shadow map. From the PREVIOUS frame."))
                                    .Prop("fallbackReason", Schema::String().Desc("Apply shape, 'raytracedshadows' on and fallbackLights > 0: the dominant reason, as a sentence."))
                                    .Prop("note", Schema::String().Desc("Apply shape: a caveat about the values just reported — that the ray-traced counters are one frame stale, or that virtual shadow maps refused to initialise and the effective state is being reported."));
            tool.MainMarshaled = true;
            tool.Handler = Handle_RendererSettingsSet;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_render_why_not_visible";
            tool.Toolset = "render";
            tool.Title = "Explain entity not visible";
            // An explainer's ordered check list is the one result a human reads
            // end-to-end rather than greps.
            tool.DualAudienceContent = true;
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Explain why an entity is NOT visible on screen — the rendering counterpart of "
                "olo_physics_why_no_collision ('why can't I see my mesh?'). Given an entity UUID, it checks, in "
                "order: a scene is loaded, the entity exists, it has a renderable component (Mesh/Model/Sprite/"
                "Circle/Text/InstancedMesh/Decal/...), its geometry asset is present, its visibility flag is on, its "
                "transform scale is non-degenerate, its material's shader compiled, and (against the editor "
                "camera) it is in front of the camera and inside the view frustum. Decals additionally report their "
                "rendering route, required texture, per-entity submission/draw status, and whether any fragments "
                "survived the real draw. Returns the root-cause "
                "reasonCode, a human summary, the ordered checks, and the raw facts. Note: per-frame occlusion "
                "(HZB) and LOD culling are not queryable from the editor and are reported as not-observable.";
            tool.InputSchema = Schema::Object()
                                   .Prop("entity", Schema::String().Desc("Entity UUID (string; also accepts a number)."))
                                   .Required({ "entity" })
                                   .NoAdditional();
            tool.OutputSchema = Schema::Object()
                                    .Prop("entity", Schema::String().Desc("Echoed entity UUID."))
                                    .Prop("reasonCode", Schema::String()
                                                            .Enum({ "no_scene", "entity_missing", "not_renderable", "geometry_missing",
                                                                    "decal_no_receiver", "decal_texture_missing", "component_hidden",
                                                                    "degenerate_scale", "shader_compile_error", "not_submitted",
                                                                    "draw_not_issued", "zero_fragments",
                                                                    "behind_camera", "outside_frustum", "should_be_visible" })
                                                            .Desc("Machine-readable root cause."))
                                    .Prop("summary", Schema::String())
                                    .Prop("renderableConfigOk", Schema::Bool())
                                    .Prop("visible", Schema::Bool())
                                    .Prop("checks", Schema::Array(Schema::String()).Desc("Ordered '[ok]'/'[fail]'/'[warn]'-prefixed check trace."))
                                    .Prop("facts", Schema::Object()
                                                       .Prop("entityExists", Schema::Bool())
                                                       .Prop("hasRenderable", Schema::Bool())
                                                       .Prop("renderableKind", Schema::String())
                                                       .Prop("geometryRequired", Schema::Bool())
                                                       .Prop("geometryPresent", Schema::Bool())
                                                       .Prop("geometryDetail", Schema::String())
                                                       .Prop("hasVisibilityFlag", Schema::Bool())
                                                       .Prop("visibilityFlagName", Schema::String())
                                                       .Prop("visibilityFlagOn", Schema::Bool())
                                                       .Prop("scaleDegenerate", Schema::Bool())
                                                       .Prop("hasMaterialShader", Schema::Bool())
                                                       .Prop("materialShaderName", Schema::String())
                                                       .Prop("materialShaderHasErrors", Schema::Bool())
                                                       .Prop("boundsKnown", Schema::Bool())
                                                       .Prop("behindCamera", Schema::Bool())
                                                       .Prop("inFrustum", Schema::Bool())
                                                       .Prop("isDecal", Schema::Bool())
                                                       .Prop("renderingPath", Schema::String())
                                                       .Prop("decalMode", Schema::String())
                                                       .Prop("decalTextureRequired", Schema::Bool())
                                                       .Prop("decalTexturePresent", Schema::Bool())
                                                       .Prop("decalTextureSlot", Schema::String())
                                                       .Prop("receiverIntersectionKnown", Schema::Bool())
                                                       .Prop("receiverIntersectsProjection", Schema::Bool())
                                                       .Prop("submissionKnown", Schema::Bool())
                                                       .Prop("submitted", Schema::Bool())
                                                       .Prop("drawIssuedKnown", Schema::Bool())
                                                       .Prop("drawIssued", Schema::Bool())
                                                       .Prop("fragmentResultKnown", Schema::Bool())
                                                       .Prop("fragmentsSurvived", Schema::Bool())
                                                       .Desc("The raw gathered facts the verdict cascade ran on."))
                                    .Prop("sceneLoaded", Schema::Bool())
                                    .Prop("cameraKnown", Schema::Bool())
                                    .Prop("anyShaderHasErrors", Schema::Bool())
                                    .Prop("shaderErrorCount", Schema::Int().Min(0))
                                    .Required({ "entity", "reasonCode", "summary", "renderableConfigOk", "visible", "checks",
                                                "facts", "sceneLoaded", "cameraKnown", "anyShaderHasErrors", "shaderErrorCount" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_RenderWhyNotVisible;
            server.RegisterTool(std::move(tool));
        }

        // ---- issue #607: the render-diagnostics gaps -------------------------

        {
            ToolDef tool;
            tool.Name = "olo_render_probe_pixel";
            tool.Toolset = "render";
            tool.Title = "Probe one pixel (G-Buffer readout)";
            // A 1x1 readback through the facade spine; changes no camera /
            // setting / scene state.
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Read back the exact NUMBERS under one viewport pixel — the numeric counterpart of "
                "olo_render_capture_target, and the fastest way to diagnose a shading bug. Given "
                "viewport pixel coordinates (top-left origin, same as olo_screenshot), it decodes every "
                "G-Buffer channel for that pixel: albedo.rgb, metallic, the world-space NORMAL (the "
                "octahedral RT1.xy pair decoded exactly as the shaders do), roughness, ao, emissive.rgb, "
                "bakedGI irradiance/coverage, the screen-space velocity vector, the integer entityID, the raw device depth PLUS its "
                "linearized view-space distance, and the final post-tonemap colour actually presented. "
                "Reach for it whenever the frame 'looks wrong' but you cannot tell WHICH channel is "
                "wrong — a picture shows a normal map is bluish, this says the normal is (0,0,1) when "
                "it should be (0,1,0). Channels that do not exist on the current rendering path (the "
                "G-Buffer is Deferred-only) are reported as unavailable with a reason, never as a "
                "failed call. Pass 'target' to probe ONE named render-graph resource instead and get "
                "its raw channel values (works for any capturable target: AOBuffer, BloomColor, "
                "VirtualGeometryDebug, ...). Only a 1x1 region is read back, so it is cheap. "
                "COORDINATES: every reply echoes 'mappedCoord' — the exact texel read — so the mapping "
                "is never guesswork. Default space \"viewport\" maps the pixel proportionally onto the "
                "target; space \"texel\" (with optional 'mip') addresses an EXACT texel of the target — "
                "required for padded resources like the HZB pow2 pyramid, where proportional mapping "
                "reads the wrong texel. 'afterPass' probes the target AS OF that pass's execution "
                "(mid-frame snapshot) instead of end-of-frame. Works on both backends. "
                "space/mip/layer/afterPass require 'target'.";
            tool.InputSchema = Schema::Object()
                                   .Prop("x", Schema::Int().Min(0).Desc("Pixel column, 0 = left edge (in 'space' units)."))
                                   .Prop("y", Schema::Int().Min(0).Desc("Pixel row, 0 = TOP edge (screenshot convention; the GL bottom-up flip is handled for you)."))
                                   .Prop("target", Schema::String().Desc("Optional: probe only this render-graph resource (see olo_render_list_targets) and return its raw channels instead of the decoded G-Buffer."))
                                   .Prop("space", Schema::String().Enum({ "viewport", "texel" }).Desc("How x/y address the target (requires 'target'). \"viewport\" (default): viewport pixels mapped proportionally onto the target mip. \"texel\": exact texel coordinates of the target at 'mip' — use for padded resources (HZB pyramid). Both top-left origin; the reply's mappedCoord shows the texel actually read."))
                                   .Prop("mip", Schema::Int().Min(0).Max(16).Desc("Mip level to probe (default 0; requires 'target'). Texel coordinates address this mip's own grid."))
                                   .Prop("layer", Schema::Int().Min(0).Max(64).Desc("Texture-array layer / cubemap face / 3D z-slice to probe (requires 'target'). Default: the resource's own view layer (a CSM cascade view probes ITS cascade, never silently cascade 0)."))
                                   .Prop("afterPass", Schema::String().Desc("Probe the target AS OF this pass's execution (mid-frame snapshot), not end-of-frame (requires 'target'). A pass name from olo_render_graph_topology_export's executionOrder."))
                                   .Prop("forceFrame", Schema::Bool().Desc("Render and settle a fresh frame before probing (default false). Use after a scene open / setting change so you cannot read a stale target. Implied by 'afterPass'."))
                                   .Required({ "x", "y" })
                                   .NoAdditional();
            // Two modes share only the envelope: single-target ('target' given)
            // returns the raw-channel fields, G-Buffer mode the decoded channels —
            // every mode-dependent field stays optional.
            tool.OutputSchema = Schema::Object()
                                    .Prop("x", Schema::Int().Min(0))
                                    .Prop("y", Schema::Int().Min(0))
                                    .Prop("origin", Schema::String().Desc("Coordinate-convention note (top-left)."))
                                    .Prop("meta", Schema::Object()
                                                      .Prop("frameIndex", Schema::Int().Min(0))
                                                      .Prop("timestampMs", Schema::Int().Min(0))
                                                      .Prop("stale", Schema::Bool().Desc("The editor's loop was parked, so these values describe the last frame drawn before it stopped."))
                                                      .Prop("liveness", EditorLiveness::SchemaNode())
                                                      .Desc("Staleness stamp: the frame/time the values were read."))
                                    .Prop("renderingPath", Schema::String().Desc("G-Buffer mode only."))
                                    .Prop("channels", Schema::Object().Desc("G-Buffer mode only: decoded per-channel objects (albedo/metallic/normal/roughness/ao/emissive/flags?/velocity/entityID/depth/finalColor), each { available, source?, format?, value?, reason? }; depth adds device/linearViewDepth (null when the camera is unknown)/nearClip/farClip, normal adds encoded/space."))
                                    .Prop("raw", Schema::Object().Desc("G-Buffer mode only: the undecoded texels per RT (GBufferAlbedo/GBufferNormal/GBufferEmissive/Velocity/EntityID/Depth/FinalColor)."))
                                    .Prop("unavailableChannels", Schema::Array(Schema::String()).Desc("G-Buffer mode only."))
                                    .Prop("note", Schema::String().Desc("G-Buffer mode: non-Deferred-path caveat; omitted otherwise."))
                                    .Prop("available", Schema::Bool().Desc("Single-target mode only."))
                                    .Prop("target", Schema::String().Desc("Single-target mode only: the probed resource."))
                                    .Prop("mappedCoord", Schema::Object().Desc("Single-target mode: the exact texel read (space/requested/texel/glRowBottomUp/mip/mipWidth/mipHeight/origin/layer?); omitted when no mapping was attempted."))
                                    .Prop("format", Schema::String().Desc("Single-target mode, when available."))
                                    .Prop("width", Schema::Int().Desc("Single-target mode, when available."))
                                    .Prop("height", Schema::Int().Desc("Single-target mode, when available."))
                                    .Prop("value", Schema::Array(Schema::Number()).Desc("Single-target mode, when available: raw channel values (int-exact for integer formats)."))
                                    .Prop("reason", Schema::String().Desc("Single-target mode: unavailability reason."))
                                    .Prop("afterPass", Schema::String().Desc("Echoed when an afterPass snapshot was probed."))
                                    .Prop("afterPassNote", Schema::String())
                                    .Required({ "x", "y", "origin", "meta" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_RenderProbePixel;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_render_target_stats";
            tool.Toolset = "render";
            tool.Title = "Exact stats over a render-target region";
            // Per-channel min/max/mean/NaN is a table — the numeric read that
            // replaces squinting at a capture PNG.
            tool.DualAudienceContent = true;
            // A bounded rect readback; changes no camera / setting / scene state.
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Exact float min/max/mean and a BIT-EXACT unique-value histogram over a rect of one "
                "render-graph target — the tool for 1-ULP questions an 8-bit PNG capture cannot answer "
                "(1.0 and 0.99999994 both encode as 255). Per channel it reports finite/NaN/Inf counts, "
                "min/max/mean over finite values, the number of DISTINCT bit patterns, and the most "
                "frequent values with their exact counts — so 'is this HZB region exactly 1.0f' or 'what "
                "garbage values leaked into this buffer' is one call, not hundreds of probes. 'rect' is "
                "in texel coordinates of the chosen 'mip' (top-left origin, a capture PNG's orientation); "
                "omit it for the whole mip (ceiling 4M texels — shrink the rect or raise the mip above "
                "that). 'afterPass' computes the stats over the resource AS OF that pass's execution. "
                "Works on both backends. "
                "Use olo_render_capture_target to SEE the region, this to know its numbers.";
            tool.InputSchema = Schema::Object()
                                   .Prop("name", Schema::String().Desc("Render-graph resource name (see olo_render_list_targets)."))
                                   .Prop("rect", Schema::Object()
                                                     .Prop("x", Schema::Int().Min(0).Desc("Left texel column of the region."))
                                                     .Prop("y", Schema::Int().Min(0).Desc("Top texel row of the region."))
                                                     .Prop("w", Schema::Int().Min(1).Desc("Region width in texels."))
                                                     .Prop("h", Schema::Int().Min(1).Desc("Region height in texels."))
                                                     .Required({ "x", "y", "w", "h" })
                                                     .NoAdditional()
                                                     .Desc("Region in texel coords of the mip, top-left origin. Omit for the whole mip."))
                                   .Prop("mip", Schema::Int().Min(0).Max(16).Desc("Mip level (default 0). rect addresses this mip's texel grid."))
                                   .Prop("layer", Schema::Int().Min(0).Max(64).Desc("Texture-array layer / cubemap face / 3D z-slice. Default: the resource's own view layer."))
                                   .Prop("afterPass", Schema::String().Desc("Compute stats over the resource AS OF this pass's execution (mid-frame snapshot). A pass name from olo_render_graph_topology_export's executionOrder."))
                                   .Prop("forceFrame", Schema::Bool().Desc("Render and settle a fresh frame first (default false). Implied by 'afterPass'."))
                                   .Required({ "name" })
                                   .NoAdditional();
            tool.OutputSchema = Schema::Object()
                                    .Prop("name", Schema::String())
                                    .Prop("format", Schema::String().Desc("Internal-format token (RGBA16F, R32I, ...)."))
                                    .Prop("mip", Schema::Int().Min(0))
                                    .Prop("mipWidth", Schema::Int().Min(0))
                                    .Prop("mipHeight", Schema::Int().Min(0))
                                    .Prop("layer", Schema::Int().Desc("Omitted when the resolved layer is 0."))
                                    .Prop("rect", Schema::Object()
                                                      .Prop("x", Schema::Int())
                                                      .Prop("y", Schema::Int())
                                                      .Prop("w", Schema::Int())
                                                      .Prop("h", Schema::Int())
                                                      .Desc("Texel region actually read (top-left origin)."))
                                    .Prop("origin", Schema::String())
                                    .Prop("texelCount", Schema::Int().Min(0))
                                    .Prop("channels", Schema::Array(Schema::Object()
                                                                        .Prop("channel", Schema::String())
                                                                        .Prop("finiteCount", Schema::Int().Min(0))
                                                                        .Prop("nanCount", Schema::Int().Desc("Omitted when 0."))
                                                                        .Prop("infCount", Schema::Int().Desc("Omitted when 0."))
                                                                        .Prop("min", Schema::Number().Desc("Over finite values; omitted when none."))
                                                                        .Prop("max", Schema::Number().Desc("Over finite values; omitted when none."))
                                                                        .Prop("mean", Schema::Number().Desc("Over finite values; omitted when none."))
                                                                        .Prop("uniqueValues", Schema::Int().Min(0))
                                                                        .Prop("uniqueTruncated", Schema::Bool().Desc("Present (true) only when the unique-value scan was capped."))
                                                                        .Prop("uniqueNote", Schema::String())
                                                                        .Prop("topValues", Schema::Array(Schema::Object()
                                                                                                             .Prop("value", Schema::Raw(Json{ { "type", Json::array({ "number", "string" }) } }).Desc("Non-finite values encode as the strings 'NaN'/'+Inf'/'-Inf'."))
                                                                                                             .Prop("bits", Schema::Int().Min(0).Desc("The exact bit pattern."))
                                                                                                             .Prop("count", Schema::Int().Min(0)))
                                                                                               .Desc("Most frequent bit-exact values."))))
                                    .Prop("integerNote", Schema::String().Desc("Integer targets only."))
                                    .Prop("afterPass", Schema::String().Desc("Echoed when an afterPass snapshot was read."))
                                    .Prop("afterPassNote", Schema::String())
                                    .Prop("layerNote", Schema::String().Desc("Omitted unless the layer selection has a caveat."))
                                    .Prop("meta", Schema::Object()
                                                      .Prop("frameIndex", Schema::Int().Min(0))
                                                      .Prop("timestampMs", Schema::Int().Min(0))
                                                      .Prop("stale", Schema::Bool().Desc("The editor's loop was parked, so these values describe the last frame drawn before it stopped."))
                                                      .Prop("liveness", EditorLiveness::SchemaNode()))
                                    .Required({ "name", "format", "mip", "mipWidth", "mipHeight", "rect", "origin", "texelCount", "channels", "meta" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_RenderTargetStats;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_render_validate";
            tool.Toolset = "render";
            tool.Title = "Validate the render-graph frame";
            // Read-only diagnostics sweep (+ an optional readback compare).
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "On-demand render-graph frame validation: runs the compiled resource-hazard sweep "
                "(read-after-write / write-after-write / cycle / imported-lifetime misuse), reports the "
                "graph's barrier and build diagnostics and any execute-path resolve failures, and maps "
                "every resource's RESOLVED physical identity — flagging resources that are consumed but "
                "resolve to no backing, and grouping versioned names (SceneColor@PassB) with their "
                "physical ids so copy-on-write aliasing is visible. 'ok': true means the sweep found "
                "nothing. Optionally pass 'compare' to check two targets BIT-EXACTLY (channel 0, "
                "overlapping top-left region): e.g. compare:{a:\"SceneDepth\", b:\"HZB\", mipB:0, "
                "afterPass:\"GTAOPass\"} answers 'is HZB mip0 identical to the depth GTAO sampled' with "
                "the first differing texels listed. With compare.afterPass BOTH sides are snapshotted in "
                "the SAME frame by the same post-pass hook.";
            tool.InputSchema = Schema::Object()
                                   .Prop("compare", Schema::Object()
                                                        .Prop("a", Schema::String().Desc("First target name."))
                                                        .Prop("b", Schema::String().Desc("Second target name."))
                                                        .Prop("mipA", Schema::Int().Min(0).Max(16).Desc("Mip of 'a' to compare (default 0)."))
                                                        .Prop("mipB", Schema::Int().Min(0).Max(16).Desc("Mip of 'b' to compare (default 0)."))
                                                        .Prop("layerA", Schema::Int().Min(0).Max(64).Desc("Layer / face / z-slice of 'a' (default 0)."))
                                                        .Prop("layerB", Schema::Int().Min(0).Max(64).Desc("Layer / face / z-slice of 'b' (default 0)."))
                                                        .Prop("afterPass", Schema::String().Desc("Snapshot BOTH targets as of this pass's execution (same frame, same hook) before comparing."))
                                                        .Required({ "a", "b" })
                                                        .NoAdditional()
                                                        .Desc("Optional bit-exact channel-0 compare of two targets over their overlapping top-left region."))
                                   .Prop("forceFrame", Schema::Bool().Desc("Render and settle a fresh frame first (default false). Implied by compare.afterPass."))
                                   .NoAdditional();
            tool.OutputSchema = Schema::Object()
                                    .Prop("ok", Schema::Bool().Desc("True iff hazards, resolveFailures and consumedButUnbacked are all empty (and 'compare', when given, did not error)."))
                                    .Prop("hazardCount", Schema::Int().Min(0))
                                    .Prop("hazards", Schema::Array(Schema::Object()
                                                                       .Prop("kind", Schema::String())
                                                                       .Prop("resource", Schema::String())
                                                                       .Prop("producer", Schema::String().Desc("Omitted when unknown."))
                                                                       .Prop("consumer", Schema::String().Desc("Omitted when unknown."))
                                                                       .Prop("message", Schema::String())))
                                    .Prop("barrierDiagnostics", Schema::Array(Schema::Object()
                                                                                  .Prop("kind", Schema::String())
                                                                                  .Prop("pass", Schema::String().Desc("Omitted when not pass-specific."))
                                                                                  .Prop("resource", Schema::String().Desc("Omitted when not resource-specific."))
                                                                                  .Prop("message", Schema::String())))
                                    .Prop("buildDiagnostics", Schema::Array(Schema::Object()
                                                                                .Prop("kind", Schema::String())
                                                                                .Prop("pass", Schema::String().Desc("Omitted when not pass-specific."))
                                                                                .Prop("resource", Schema::String().Desc("Omitted when not resource-specific."))
                                                                                .Prop("message", Schema::String())))
                                    .Prop("resolveFailures", Schema::Array(Schema::Object()
                                                                               .Prop("pass", Schema::String())
                                                                               .Prop("reason", Schema::String())
                                                                               .Prop("count", Schema::Int().Min(0))))
                                    .Prop("consumedButUnbacked", Schema::Array(Schema::String()).Desc("Resources read by at least one pass that resolve to no GPU storage at all. Decided from the RHI identity and a storage query, never from a native handle - a native 0 is legitimate on Vulkan."))
                                    .Prop("versionGroups", Schema::Array(Schema::Object()
                                                                             .Prop("baseName", Schema::String())
                                                                             .Prop("versions", Schema::Array(Schema::Object()
                                                                                                                 .Prop("name", Schema::String())
                                                                                                                 .Prop("nativeTextureHandle", Schema::String().Desc("Backend-native texture handle as hex; display only, omitted when 0."))
                                                                                                                 .Prop("nativeBufferHandle", Schema::String().Desc("Backend-native buffer handle as hex; display only, omitted when 0."))
                                                                                                                 .Prop("textureIdentity", Schema::String().Desc("Resolved texture identity as \"#index:generation\"; omitted when there is none."))
                                                                                                                 .Prop("bufferIdentity", Schema::String().Desc("Resolved buffer identity; omitted when there is none."))
                                                                                                                 .Prop("backed", Schema::Bool().Desc("Whether this version resolves to real GPU storage."))
                                                                                                                 .Prop("lastWriter", Schema::String().Desc("Omitted when unknown."))))
                                                                             .Prop("multiplePhysicalIds", Schema::Bool()))
                                                               .Desc("Versioned names (Base@Pass) grouped with their resolved physical backing; single-version groups are dropped. multiplePhysicalIds compares the IDENTITY, not the native handle."))
                                    .Prop("compare", Schema::Object().Desc("Only when 'compare' was requested: 'a'/'b' echoes plus either 'error' or the bit-exact result (comparedRegion/comparedTexels/bitwiseEqual/differingTexels/maxAbsDiff?/firstDiffs/note; firstDiffs a/b encode non-finite floats as the strings 'NaN'/'Inf')."))
                                    .Prop("meta", Schema::Object()
                                                      .Prop("frameIndex", Schema::Int().Min(0))
                                                      .Prop("timestampMs", Schema::Int().Min(0))
                                                      .Prop("stale", Schema::Bool().Desc("The editor's loop was parked, so these values describe the last frame drawn before it stopped."))
                                                      .Prop("liveness", EditorLiveness::SchemaNode()))
                                    .Required({ "ok", "hazardCount", "hazards", "barrierDiagnostics", "buildDiagnostics",
                                                "resolveFailures", "consumedButUnbacked", "versionGroups", "meta" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_RenderValidate;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_virtual_geometry_set";
            tool.Toolset = "render";
            tool.Title = "Set virtual-geometry (Nanite) debug knobs";
            // A renderer-debug toggle like olo_render_set_debug_view: it edits
            // session-global renderer state, never the project, so it is NOT a
            // ProjectWrite. Idempotent — the same arguments leave the same state.
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Drive the virtualized-geometry (Nanite-style cluster LOD DAG) debug knobs, which were "
                "otherwise reachable only from the editor's Statistics panel. 'debugMode' turns on a "
                "per-pixel visualization written into the 'VirtualGeometryDebug' capture target — "
                "'clusterid' (each cluster a distinct hashed colour: see the cluster decomposition and "
                "spot a cut that is too coarse/fine), 'lod' (per-pixel DAG level as a ramp: verify the "
                "screen-space error target is selecting the LOD you expect), 'overdraw' (per-pixel "
                "cluster fragment count as a heat ramp) — then capture it with olo_render_capture_target "
                "{ name: 'VirtualGeometryDebug' }. 'swRasterMode' forces the HW/SW raster split "
                "('auto' = small clusters go to the compute software rasterizer, 'forcesoftware' = every "
                "safe cluster does, 'disabled' = hardware MDI only) and 'swRasterThresholdPixels' moves "
                "the auto-mode projected-radius cutoff — together they are the SW-vs-HW parity A/B. "
                "'forcePortableSwRaster' forces the portable two-pass 2x32 visibility path even on a "
                "driver with 64-bit atomics. 'hwRasterMode' picks how the HARDWARE-routed clusters draw: "
                "'auto' = the mesh-shader pipeline where the device supports VK_EXT_mesh_shader (Vulkan "
                "backend only), 'forcemdi' = the classic vertex-pipeline MDI — this is the "
                "mesh-shader-vs-MDI A/B (#813). Key that A/B on 'meshRasterAvailable' in the echoed "
                "settings, which reports the EFFECTIVE route: 'meshShadersSupported' is raw device "
                "capability, and the pass still demotes to MDI if VirtualMeshletGBuffer.glsl failed to "
                "compile — in which case 'auto' and 'forcemdi' both draw MDI and the A/B silently "
                "measures nothing against itself. "
                "'enabled' is the MASTER SWITCH: turning it off draws every "
                "VirtualMeshComponent through the CLASSIC mesh path instead (same geometry, same "
                "materials, no cluster LOD), which is the virtual-vs-classic A/B — the scene is "
                "unchanged and only the renderer differs. 'debugToViewport' composites the active "
                "debugMode over the lit viewport image instead of only into the capture target. "
                "Call with no arguments to read the current state. Virtual "
                "geometry renders on the DEFERRED path only. The change is EPHEMERAL renderer state: "
                "never saved, restored by a scene reload.";
            tool.InputSchema = Schema::Object()
                                   .Prop("enabled", Schema::Bool().Desc("Master switch. false = draw every VirtualMeshComponent through the classic mesh path instead (the virtual-vs-classic A/B); the geometry does not disappear."))
                                   .Prop("debugToViewport", Schema::Bool().Desc("Composite the active debugMode over the lit viewport image, not just into the 'VirtualGeometryDebug' capture target."))
                                   .Prop("debugMode", Schema::String().Enum({ "off", "clusterid", "lod", "overdraw" }).Desc("Per-pixel debug visualization written to the 'VirtualGeometryDebug' capture target. 'off' disables it (no cost)."))
                                   .Prop("swRasterMode", Schema::String().Enum({ "auto", "forcesoftware", "disabled" }).Desc("Software-rasterizer routing: 'auto' (coverage-based, default), 'forcesoftware' (every near-plane-safe cluster), 'disabled' (hardware MDI only)."))
                                   .Prop("swRasterThresholdPixels", Schema::Number().Min(0).Max(4096).Desc("Auto-mode cutoff: a cluster whose projected screen radius is below this many pixels is software-rasterized (default 24)."))
                                   .Prop("forcePortableSwRaster", Schema::Bool().Desc("Force the portable two-pass 2x32 SW visibility path even where 64-bit atomics exist (exercises both rasterizers on capable hardware)."))
                                   .Prop("hwRasterMode", Schema::String().Enum({ "auto", "forcemdi" }).Desc("Hardware-raster routing (#813): 'auto' = mesh-shader pipeline where the device supports it (Vulkan only), 'forcemdi' = classic vertex-pipeline MDI. The mesh-shader-vs-MDI A/B lever."))
                                   .NoAdditional();
            tool.OutputSchema = Schema::Object()
                                    .Prop("changed", Schema::Bool().Desc("True when any knob argument was present."))
                                    .Prop("previous", VirtualGeometrySettingsSchema().Desc("Knob state before the write."))
                                    .Prop("current", VirtualGeometrySettingsSchema().Desc("Knob state after the write + settle (equals 'previous' on a no-arg read)."))
                                    .Prop("captureTarget", Schema::String().Desc("'VirtualGeometryDebug' — only when a non-off debugMode was set."))
                                    .Prop("message", Schema::String().Desc("Capture hint / target-not-backed guidance; only when a non-off debugMode was set."))
                                    .Required({ "changed", "previous", "current" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_VirtualGeometrySet;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_shader_debug_draw";
            tool.Toolset = "render";
            tool.Title = "GPU-pushable shader debug draws";
            // Session-global renderer debug state, never the project. Idempotent:
            // the same arguments leave the same state.
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Drive the GPU-pushable shader debug-draw channels (issue #725) - the instrument for "
                "GPU-driven passes, whose cull decisions, cluster bounds and probe placements are computed "
                "on the GPU and otherwise never come back. Any shader that includes "
                "'include/DebugDrawCommon.glsl' can atomic-append a line / circle / rectangle / AABB / box / "
                "cone / sphere into a per-primitive channel, and this pass draws every channel with one "
                "indirect call at the end of the SceneColor chain, depth-tested against the real scene. "
                "'enabled' is the master switch (off costs nothing: the channels collapse to header-only and "
                "the push helpers early-out on one scalar load). 'lineWidth' sets the screen-space quad width "
                "in pixels. 'clusterBounds' turns on the SHIPPED consumer - VirtualClusterCull.comp emitting "
                "each cluster's world-space cull sphere colour-coded by verdict, as a bit field: 1 = drawn "
                "(green), 2 = frustum-culled (red), 4 = cone-culled (blue), 8 = Hi-Z occluded (yellow); "
                "combine them to answer 'which test removed that cluster' directly. It needs the DEFERRED "
                "path with virtual geometry in view. 'clusterStride' emits only every Nth cluster - a "
                "Nanite-class scene has far more clusters than a channel holds, so 1 simply overflows. "
                "The response's per-channel counters ARE the overflow flag: 'requested' is unclamped and "
                "'drawn' is capped at 'capacity', so 'overflowed'/'dropped' distinguish 'I pushed nothing' "
                "from 'I pushed too much and the rest was silently thrown away' - the failure mode this "
                "feature exists to remove. Counters are one frame behind by design (read back through a "
                "DeviceToHost staging copy so nothing stalls). Call with no arguments to read the state. "
                "EPHEMERAL renderer state: never saved, restored by a scene reload.";
            tool.InputSchema = Schema::Object()
                                   .Prop("enabled", Schema::Bool().Desc("Master switch for the debug-draw channels + the render pass. Off is free."))
                                   .Prop("lineWidth", Schema::Number().Min(1).Max(32).Desc("Screen-space line width in pixels (default 2). Every primitive is expanded to quads, so this is a real knob, not a GL_LINES hint."))
                                   .Prop("clusterBounds", Schema::Int().Min(0).Max(15).Desc("Virtual-geometry cluster-bounds bit field: 1 drawn | 2 frustum-culled | 4 cone-culled | 8 Hi-Z occluded. 0 = off. Deferred path only."))
                                   .Prop("clusterStride", Schema::Int().Min(1).Max(4096).Desc("Emit only every Nth cluster (default 32). Guards against overflowing the channel with a whole Nanite cut."))
                                   .NoAdditional();
            tool.OutputSchema = Schema::Object()
                                    .Prop("changed", Schema::Bool().Desc("True when any knob argument was present."))
                                    .Prop("previous", ShaderDebugDrawStateSchema().Desc("State before the write."))
                                    .Prop("current", ShaderDebugDrawStateSchema().Desc("State after the write + settle."))
                                    .Prop("message", Schema::String().Desc("Overflow warning or capture hint."))
                                    .Required({ "changed", "previous", "current" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_ShaderDebugDrawSet;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_virtual_geometry_stats";
            tool.Toolset = "render";
            tool.Title = "Virtual-geometry (Nanite) cull + streaming stats";
            // A small blocking GPU readback of the cull args buffer; observable
            // state is unchanged.
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Read this frame's virtualized-geometry (Nanite-style) counters: the GPU cluster cull "
                "(instances submitted, clusters tested, clusters selected by the view-dependent DAG-cut "
                "rule, how many were routed to the hardware MDI path vs the compute software rasterizer, "
                "and the drawn total) plus the streaming residency (total / resident / pinned pages, the "
                "page budget, and cumulative page uploads + evictions). Use it to verify the cull is "
                "actually culling (tested >> drawn), to check the HW/SW split after "
                "olo_virtual_geometry_set { swRasterMode }, and to catch streaming thrash (uploads and "
                "evictions climbing every frame under a tight budget). When everything reads zero, "
                "'diagnostics' says WHY — crucially it distinguishes a scene with no virtual meshes from "
                "a scene whose VirtualMeshComponents all failed to load their mesh asset, which looks "
                "identical in the counters and makes any A/B measured on that scene pass VACUOUSLY. "
                "Check diagnostics.silentlyDrewNothing before trusting a zero. Virtual geometry renders "
                "on the DEFERRED path only.";
            tool.InputSchema = Schema::EmptyObject();
            tool.OutputSchema = Schema::Object()
                                    .Prop("renderingPath", Schema::String())
                                    .Prop("frameInstances", Schema::Int().Min(0))
                                    .Prop("frameClusters", Schema::Int().Min(0))
                                    .Prop("cull", Schema::Object()
                                                      .Prop("instances", Schema::Int().Min(0))
                                                      .Prop("testedClusters", Schema::Int().Min(0))
                                                      .Prop("cutSelected", Schema::Int().Min(0))
                                                      .Prop("hardwareDraws", Schema::Int().Min(0))
                                                      .Prop("softwareRasterized", Schema::Int().Min(0))
                                                      .Prop("drawnClusters", Schema::Int().Min(0))
                                                      .Prop("phase2Recovered", Schema::Int().Min(0).Desc("Two-phase occlusion (#682): clusters the PREVIOUS frame's pyramid hid that this frame's recovered. Already included in hardwareDraws / softwareRasterized.")))
                                    .Prop("residency", Schema::Object()
                                                           .Prop("totalPages", Schema::Int().Min(0))
                                                           .Prop("residentPages", Schema::Int().Min(0))
                                                           .Prop("pinnedPages", Schema::Int().Min(0))
                                                           .Prop("budgetSlots", Schema::Int().Min(0))
                                                           .Prop("budget", Schema::String().Enum({ "unbounded (eager)", "budgeted" }))
                                                           .Prop("pageUploads", Schema::Int().Min(0))
                                                           .Prop("pageEvictions", Schema::Int().Min(0)))
                                    .Prop("settings", VirtualGeometrySettingsSchema().Desc("Live knob state (same shape as olo_virtual_geometry_set's previous/current)."))
                                    .Prop("diagnostics", Schema::Object()
                                                             .Desc("Why the counters read what they do (issue #864). Tells a real zero apart from a broken scene.")
                                                             .Prop("enabledComponents", Schema::Int().Min(0).Desc("VirtualMeshComponents that asked to render (enabled, with a mesh assigned)."))
                                                             .Prop("unresolvedAssets", Schema::Int().Min(0).Desc("...whose mesh-source asset did not load — usually an unfetched fetch-on-demand asset."))
                                                             .Prop("registrationFailures", Schema::Int().Min(0).Desc("...that resolved but whose cluster DAG failed to build."))
                                                             .Prop("submitted", Schema::Int().Min(0).Desc("...that actually reached the renderer."))
                                                             .Prop("fellBackToClassic", Schema::Bool().Desc("Master switch off: drawn through the classic mesh path, so zero VG counters are expected."))
                                                             .Prop("silentlyDrewNothing", Schema::Bool().Desc("TRUE means the scene asked for virtual geometry and got none. Any measurement taken here is vacuous.")))
                                    .Prop("note", Schema::String().Desc("Plain-language explanation of a zero (broken scene / classic fallback / non-Deferred path / genuinely no virtual meshes); omitted when there is nothing to caveat."))
                                    .Required({ "renderingPath", "frameInstances", "frameClusters", "cull", "residency", "settings", "diagnostics" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_VirtualGeometryStats;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_material_get";
            tool.Toolset = "render";
            tool.Title = "Get resolved material for a draw";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Return the material data the renderer ACTUALLY uploads to the GPU for an entity's "
                "draw — not what the asset file says. These differ more often than is comfortable, and "
                "the difference is invisible in the inspector: a MaterialComponent silently overrides "
                "every submesh, and the engine's grey default quietly stands in when nothing else "
                "exists. For each submesh the tool reports WHICH material won (MaterialComponent "
                "override / the submesh's imported material / the engine default), the alpha mode "
                "(Opaque/Mask/Blend) and cutoff, the base-colour, metallic, roughness, normal-scale, "
                "occlusion-strength and emissive factors, the useXMap booleans, and the bound GL "
                "texture id per slot (0 = no texture bound — the usual cause of 'my normal map does "
                "nothing'). Handles both MeshComponent and VirtualMeshComponent; omit 'submesh' to get "
                "every submesh. Both paths now resolve through the same rule — MaterialComponent "
                "override -> the submesh's imported material -> the engine default — so this reports "
                "what the GPU actually got, not what the component nominally asked for.";
            tool.InputSchema = Schema::Object()
                                   .Prop("entity", Schema::EntityId("Entity UUID (string; also accepts a number). Must have a MeshComponent or VirtualMeshComponent."))
                                   .Prop("submesh", Schema::Int().Min(0).Desc("Submesh index. Omit for an array covering every submesh."))
                                   .Required({ "entity" })
                                   .NoAdditional();
            tool.OutputSchema = Schema::Object()
                                    .Prop("entity", Schema::String())
                                    .Prop("renderableKind", Schema::String().Enum({ "MeshComponent", "VirtualMeshComponent" }))
                                    .Prop("submeshCount", Schema::Int().Min(0))
                                    .Prop("hasMaterialComponentOverride", Schema::Bool())
                                    .Prop("submeshes", Schema::Array(Schema::Object()
                                                                         .Prop("submesh", Schema::Int().Min(0))
                                                                         .Prop("source", Schema::String().Desc("Which material won: the MaterialComponent override, the submesh's imported material, or the engine default."))
                                                                         .Prop("name", Schema::String())
                                                                         .Prop("pbr", Schema::Bool())
                                                                         .Prop("alphaMode", Schema::String().Enum({ "Opaque", "Mask", "Blend" }))
                                                                         .Prop("alphaCutoff", Schema::Number())
                                                                         .Prop("twoSided", Schema::Bool())
                                                                         .Prop("baseColorFactor", Schema::Array(Schema::Number()).Desc("RGBA."))
                                                                         .Prop("metallicFactor", Schema::Number())
                                                                         .Prop("roughnessFactor", Schema::Number())
                                                                         .Prop("normalScale", Schema::Number())
                                                                         .Prop("occlusionStrength", Schema::Number())
                                                                         .Prop("emissiveFactor", Schema::Array(Schema::Number()).Desc("RGB."))
                                                                         .Prop("enableIBL", Schema::Bool())
                                                                         .Prop("iblIntensity", Schema::Number())
                                                                         .Prop("useMaps", Schema::Object().Desc("Booleans per slot: useAlbedoMap/useMetallicRoughnessMap/useNormalMap/useAOMap/useEmissiveMap."))
                                                                         .Prop("textureIds", Schema::Object().Desc("Bound backend-native texture handle per slot as hex (albedo/metallicRoughness/normal/ao/emissive). DISPLAY ONLY - what a RenderDoc capture shows; \"0x0\" is legitimate on Vulkan and does not mean the slot is unbound."))
                                                                         .Prop("textureIdentities", Schema::Object().Desc("Bound RHI identity per slot as \"#index:generation\", or \"<none>\" when the slot is unbound. This is the currency to compare and to decide on."))))
                                    .Required({ "entity", "renderableKind", "submeshCount", "hasMaterialComponentOverride", "submeshes" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_MaterialGet;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_cluster_grid_stats";
            tool.Toolset = "render";
            tool.Title = "Clustered light-grid stats";
            // The per-slice + histogram tables are how a human sees WHERE the cull
            // is hot, not merely that it is.
            tool.DualAudienceContent = true;
            // Stages the light-grid SSBOs through a temporary read buffer; no
            // observable state changes.
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Summarise the clustered (Forward+ / froxel) light grid for the current frame — the "
                "only way to see the light cull without writing a one-off readback test. Reports the "
                "grid dimensions and per-cluster light cap, the total assigned light indices, a "
                "per-z-slice breakdown (assigned / empty / max / mean lights per depth band), a "
                "count-bucket histogram over every cluster, the mean and MAX lights in any cluster with "
                "the busiest cluster's coordinates, and — the important number — how many clusters are "
                "EMPTY and how many are OVERFLOWING (at the cap, where the cull silently DROPS the "
                "extra lights, so a light in that froxel just stops lighting). Also reports the light "
                "index list's used slots vs capacity. Use it to verify the cull is assigning lights at "
                "all, to find the depth slices that are hot, and to catch a scene that has quietly "
                "exceeded the per-cluster budget. 'culling' reports whether depth-aware 2.5D compaction "
                "or the fixed-grid fallback ran, plus active/culled cluster counts. The plain Forward "
                "path does not run the cull.";
            tool.InputSchema = Schema::EmptyObject();
            tool.OutputSchema = Schema::Object()
                                    .Prop("grid", Schema::Object()
                                                      .Prop("countX", Schema::Int().Min(0))
                                                      .Prop("countY", Schema::Int().Min(0))
                                                      .Prop("countZ", Schema::Int().Min(0))
                                                      .Prop("totalClusters", Schema::Int().Min(0))
                                                      .Prop("maxLightsPerCluster", Schema::Int().Min(0)))
                                    .Prop("clustersSampled", Schema::Int().Min(0))
                                    .Prop("totalAssignedIndices", Schema::Int().Min(0))
                                    .Prop("emptyClusters", Schema::Int().Min(0))
                                    .Prop("overflowClusters", Schema::Int().Min(0).Desc("Clusters at the light cap — lights beyond it are DROPPED there."))
                                    .Prop("maxLightsInAnyCluster", Schema::Int().Min(0))
                                    .Prop("meanLightsPerCluster", Schema::Number())
                                    .Prop("meanLightsPerNonEmptyCluster", Schema::Number())
                                    .Prop("busiestCluster", Schema::Object()
                                                                .Prop("index", Schema::Int().Min(0))
                                                                .Prop("x", Schema::Int().Min(0))
                                                                .Prop("y", Schema::Int().Min(0))
                                                                .Prop("z", Schema::Int().Min(0))
                                                                .Prop("lights", Schema::Int().Min(0)))
                                    .Prop("perSlice", Schema::Array(Schema::Object()
                                                                        .Prop("slice", Schema::Int().Min(0))
                                                                        .Prop("clusters", Schema::Int().Min(0))
                                                                        .Prop("assignedIndices", Schema::Int().Min(0))
                                                                        .Prop("emptyClusters", Schema::Int().Min(0))
                                                                        .Prop("overflowClusters", Schema::Int().Min(0))
                                                                        .Prop("maxLights", Schema::Int().Min(0))
                                                                        .Prop("meanLights", Schema::Number()))
                                                          .Desc("Per-z-slice breakdown."))
                                    .Prop("histogram", Schema::Array(Schema::Object()
                                                                         .Prop("low", Schema::Int().Min(0))
                                                                         .Prop("high", Schema::Int().Min(0))
                                                                         .Prop("clusters", Schema::Int().Min(0)))
                                                           .Desc("Light-count buckets over every cluster."))
                                    .Prop("lightIndexList", Schema::Object()
                                                                .Prop("usedSlots", Schema::Int().Min(0))
                                                                .Prop("capacity", Schema::Int().Min(0))
                                                                .Prop("utilization", Schema::Number()))
                                    .Prop("warning", Schema::String().Desc("Per-cluster light-cap overflow warning; omitted when no cluster overflowed."))
                                    .Prop("renderingPath", Schema::String())
                                    .Prop("screen", Schema::Object()
                                                        .Prop("width", Schema::Int().Min(0))
                                                        .Prop("height", Schema::Int().Min(0)))
                                    .Prop("culling", Schema::Object()
                                                         .Prop("mode", Schema::String())
                                                         .Prop("depthAware", Schema::Bool())
                                                         .Prop("activeClusters", Schema::Int().Min(0))
                                                         .Prop("culledClusters", Schema::Int().Min(0))
                                                         .Prop("activeFraction", Schema::Number())
                                                         .Prop("frameIndex", Schema::Int().Min(0).Desc("Engine render frame that produced the retained culling buffers."))
                                                         .Prop("sampleAgeFrames", Schema::Int().Min(0))
                                                         .Prop("stale", Schema::Bool())
                                                         .Prop("counterVerified", Schema::Bool())
                                                         .Prop("indirectActiveClusters", Schema::Int().Min(0))
                                                         .Prop("metadataActiveClusters", Schema::Int().Min(0))
                                                         .Prop("counterVerificationError", Schema::String()))
                                    .Prop("note", Schema::String().Desc("Plain-Forward staleness caveat; omitted otherwise."))
                                    .Required({ "grid", "clustersSampled", "totalAssignedIndices", "emptyClusters", "overflowClusters",
                                                "maxLightsInAnyCluster", "meanLightsPerCluster", "meanLightsPerNonEmptyCluster",
                                                "busiestCluster", "perSlice", "histogram", "lightIndexList", "renderingPath", "screen",
                                                "culling" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_ClusterGridStats;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_virtual_shadow_map_stats";
            tool.Toolset = "render";
            tool.Title = "Virtual shadow-map statistics";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Return the virtual shadow map's cached previous-frame counters, physical page-pool dimensions, "
                "and owned VRAM bytes. The availability/freshness envelope distinguishes an unavailable renderer, "
                "a disabled VSM, and counters that have not completed their first non-blocking readback from a valid "
                "idle sample whose values are legitimately zero.";
            tool.InputSchema = Schema::EmptyObject();
            tool.OutputSchema = Schema::Object()
                                    .Prop("availability", Schema::Object()
                                                              .Prop("available", Schema::Bool())
                                                              .Prop("enabled", Schema::Bool())
                                                              .Prop("hasData", Schema::Bool())
                                                              .Prop("status", Schema::String().Enum({ "unavailable", "disabled", "noData", "ready" }))
                                                              .Required({ "available", "enabled", "hasData", "status" }))
                                    .Prop("freshness", Schema::Object()
                                                           .Prop("model", Schema::String().Enum({ "previousFrame" }))
                                                           .Prop("stale", Schema::Bool())
                                                           .Prop("sampleAgeFrames", Schema::Raw(Json{ { "type", Json::array({ "integer", "null" }) }, { "minimum", 0 } }))
                                                           .Required({ "model", "stale", "sampleAgeFrames" }))
                                    .Prop("physicalPool", Schema::Object()
                                                              .Prop("resolution", Schema::Int().Min(0))
                                                              .Prop("pageSize", Schema::Int().Min(1))
                                                              .Prop("pageCount", Schema::Int().Min(0))
                                                              .Required({ "resolution", "pageSize", "pageCount" }))
                                    .Prop("vramBytes", Schema::Int().Min(0))
                                    .Prop("statistics", Schema::Object()
                                                            .Prop("pagesRequested", Schema::Int().Min(0))
                                                            .Prop("pagesAllocated", Schema::Int().Min(0))
                                                            .Prop("pagesFailed", Schema::Int().Min(0))
                                                            .Prop("pagesDrawn", Schema::Int().Min(0))
                                                            .Prop("pagesResident", Schema::Int().Min(0))
                                                            .Prop("pagesFreed", Schema::Int().Min(0))
                                                            .Prop("drawInstances", Schema::Int().Min(0))
                                                            .Prop("cullOverflows", Schema::Int().Min(0))
                                                            .Prop("localPagesResident", Schema::Int().Min(0))
                                                            .Prop("localPagesDrawn", Schema::Int().Min(0))
                                                            .Required({ "pagesRequested", "pagesAllocated", "pagesFailed", "pagesDrawn",
                                                                        "pagesResident", "pagesFreed", "drawInstances", "cullOverflows",
                                                                        "localPagesResident", "localPagesDrawn" }))
                                    .Required({ "availability", "freshness" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_VirtualShadowMapStats;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_render_lod_stats";
            tool.Toolset = "render";
            tool.Title = "Renderer LOD statistics";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Return Renderer3D's classic-mesh LOD-switch count and per-LOD object histogram. These counters are "
                "session-cumulative since renderer initialization (or an explicit internal ResetStats), not a single "
                "frame; zero switches and an empty or zero-filled histogram are legitimate data.";
            tool.InputSchema = Schema::EmptyObject();
            tool.OutputSchema = Schema::Object()
                                    .Prop("availability", Schema::Object()
                                                              .Prop("available", Schema::Bool())
                                                              .Prop("enabled", Schema::Bool())
                                                              .Prop("hasData", Schema::Bool())
                                                              .Prop("status", Schema::String().Enum({ "unavailable", "disabled", "noData", "ready" }))
                                                              .Required({ "available", "enabled", "hasData", "status" }))
                                    .Prop("freshness", Schema::Object()
                                                           .Prop("model", Schema::String().Enum({ "sessionCumulative" }))
                                                           .Prop("stale", Schema::Bool())
                                                           .Prop("sampleAgeFrames", Schema::Raw(Json{ { "type", Json::array({ "integer", "null" }) }, { "minimum", 0 } }))
                                                           .Required({ "model", "stale", "sampleAgeFrames" }))
                                    .Prop("lodSwitches", Schema::Int().Min(0))
                                    .Prop("objectsPerLODLevel", Schema::Array(Schema::Int().Min(0)))
                                    .Required({ "availability", "freshness" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_RenderLODStats;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_rt_scene_stats";
            tool.Toolset = "render";
            tool.Title = "Ray-tracing scene statistics";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Return the hardware ray-tracing acceleration-structure scene's capability and counters (issue "
                "#978): whether ray query is usable on this device and WHY it is not when it is not, the resident "
                "BLAS population by geometry class, TLAS instance count, acceleration-structure and scratch memory, "
                "compaction savings, and this frame's build/refit/compaction/retire counts. Status distinguishes "
                "'unavailable' (no ray tracing on this device or backend) from 'noData' (ray tracing is live but no "
                "TLAS has been built yet, e.g. a scene with no traceable geometry) — an all-zero payload from those "
                "two causes means different things. The 'gpuScene' block reports the canonical scene the structures "
                "are BUILT FROM and is emitted whatever the status is: how many instance records are live, and how "
                "much renderable geometry produced none ('notStagedTotal'). Read it before trusting any ray-traced "
                "evidence — a small tlasInstances beside a large notStagedTotal means most of the scene is not in "
                "the acceleration structure at all (issue #1065).";
            tool.InputSchema = Schema::EmptyObject();
            tool.OutputSchema =
                Schema::Object()
                    .Prop("availability", Schema::Object()
                                              .Prop("available", Schema::Bool())
                                              .Prop("enabled", Schema::Bool())
                                              .Prop("hasData", Schema::Bool())
                                              .Prop("status", Schema::String().Enum({ "unavailable", "disabled", "noData", "ready" }))
                                              .Required({ "available", "enabled", "hasData", "status" }))
                    .Prop("freshness", Schema::Object()
                                           .Prop("model", Schema::String().Enum({ "previousFrame" }))
                                           .Prop("stale", Schema::Bool())
                                           .Prop("sampleAgeFrames", Schema::Raw(Json{ { "type", Json::array({ "integer", "null" }) }, { "minimum", 0 } }))
                                           .Required({ "model", "stale", "sampleAgeFrames" }))
                    .Prop("capability", Schema::Object()
                                            .Prop("supported", Schema::Bool())
                                            .Prop("rayTracingPipeline", Schema::Bool())
                                            .Prop("reason", Schema::String().Desc("Human-readable reason; 'supported' when it is."))
                                            .Prop("properties", Schema::Object()
                                                                    .Prop("minScratchOffsetAlignment", Schema::Int().Min(0))
                                                                    .Prop("maxInstanceCount", Schema::Int().Min(0))
                                                                    .Prop("maxGeometryCount", Schema::Int().Min(0))
                                                                    .Prop("maxPrimitiveCount", Schema::Int().Min(0)))
                                            .Required({ "supported", "rayTracingPipeline", "reason" }))
                    .Prop("resident", Schema::Object()
                                          .Prop("blasByClass", Schema::Object()
                                                                   .Prop("static", Schema::Int().Min(0))
                                                                   .Prop("rigidDynamic", Schema::Int().Min(0))
                                                                   .Prop("deformed", Schema::Int().Min(0))
                                                                   .Prop("masked", Schema::Int().Min(0))
                                                                   .Prop("unsupported", Schema::Int().Min(0).Desc("Always 0: an unsupported record produces no acceleration structure. See resident.unsupportedInstances.")))
                                          .Prop("tlasInstances", Schema::Int().Min(0))
                                          .Prop("unsupportedInstances", Schema::Int().Min(0).Desc("Live GPU Scene instances the RT scene cannot trace, which stay raster-only. A real, expected population, not an error count."))
                                          .Prop("accelerationStructureBytes", Schema::Int().Min(0))
                                          .Prop("scratchBytes", Schema::Int().Min(0))
                                          .Prop("compactionSavedBytes", Schema::Int().Min(0)))
                    .Prop("frame", Schema::Object()
                                       .Prop("blasBuilds", Schema::Int().Min(0))
                                       .Prop("blasRefits", Schema::Int().Min(0))
                                       .Prop("blasCompactions", Schema::Int().Min(0))
                                       .Prop("blasRetired", Schema::Int().Min(0))
                                       .Prop("tlasBuilds", Schema::Int().Min(0))
                                       .Prop("tlasUpdates", Schema::Int().Min(0))
                                       .Prop("instancesTraced", Schema::Int().Min(0))
                                       .Prop("instancesSkipped", Schema::Int().Min(0))
                                       .Prop("blasBuildGpuNs", Schema::Int().Min(0).Desc("Nanoseconds; 0 means no sample has resolved yet, not that it was free."))
                                       .Prop("tlasBuildGpuNs", Schema::Int().Min(0)))
                    .Prop("lastTlasReason", Schema::String())
                    .Prop("gpuScene", Schema::Object()
                                          .Prop("available", Schema::Bool().Desc("False when the renderer is not up — NOT 'the scene is empty'."))
                                          .Prop("instances", Schema::Int().Min(0).Desc("Live canonical instance records: the population the TLAS is built from. Compare with resident.tlasInstances."))
                                          .Prop("geometries", Schema::Int().Min(0))
                                          .Prop("materials", Schema::Int().Min(0))
                                          .Prop("lights", Schema::Int().Min(0))
                                          .Prop("notStagedTotal", Schema::Int().Min(0).Desc("Renderable geometry this frame that produced NO canonical instance. Large next to a small 'instances' means the ray tracer is tracing a fraction of the scene (issue #1065)."))
                                          .Prop("notStagedByCategory", Schema::Object().Desc("The same total split by diagnostics category; 'notExtractable' is geometry that was offered and rejected, the rest is geometry a path knows it cannot represent."))
                                          .Required({ "available" }))
                    .Required({ "availability", "freshness", "capability", "gpuScene" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_RayTracingStats;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_ddgi_probe_stats";
            tool.Toolset = "render";
            tool.Title = "DDGI probe statistics";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Synchronously read the DDGI probe diagnostics once and return probe lifecycle counters, measured "
                "active-probe bounce coverage, and each active cascade's lattice. The availability/freshness envelope "
                "makes a missing pass or a pass that did not run this frame explicit; bounceCoverage is null when no "
                "active probe recorded a bounce hit, while a numeric zero is a legitimate measurement.";
            tool.InputSchema = Schema::EmptyObject();
            tool.OutputSchema = Schema::Object()
                                    .Prop("availability", Schema::Object()
                                                              .Prop("available", Schema::Bool())
                                                              .Prop("enabled", Schema::Bool())
                                                              .Prop("hasData", Schema::Bool())
                                                              .Prop("status", Schema::String().Enum({ "unavailable", "disabled", "noData", "ready" }))
                                                              .Required({ "available", "enabled", "hasData", "status" }))
                                    .Prop("freshness", Schema::Object()
                                                           .Prop("model", Schema::String().Enum({ "currentBlockingReadback" }))
                                                           .Prop("stale", Schema::Bool())
                                                           .Prop("sampleAgeFrames", Schema::Raw(Json{ { "type", Json::array({ "integer", "null" }) }, { "minimum", 0 } }))
                                                           .Required({ "model", "stale", "sampleAgeFrames" }))
                                    .Prop("totalProbes", Schema::Int().Min(0))
                                    .Prop("statistics", Schema::Object()
                                                            .Prop("liveProbes", Schema::Int().Min(0))
                                                            .Prop("activeProbes", Schema::Int().Min(0))
                                                            .Prop("relitProbes", Schema::Int().Min(0))
                                                            .Prop("capturedProbes", Schema::Int().Min(0))
                                                            .Prop("blendedProbes", Schema::Int().Min(0))
                                                            .Prop("uncapturedLive", Schema::Int().Min(0))
                                                            .Required({ "liveProbes", "activeProbes", "relitProbes", "capturedProbes",
                                                                        "blendedProbes", "uncapturedLive" }))
                                    .Prop("bounceCoverage", Schema::Raw(Json{ { "type", Json::array({ "number", "null" }) } }))
                                    .Prop("cascades", Schema::Array(Schema::Object()
                                                                        .Prop("level", Schema::Int().Min(0))
                                                                        .Prop("origin", Schema::Vec3("World position of lattice coordinate (0,0,0)."))
                                                                        .Prop("spacing", Schema::Vec3("Per-axis probe spacing."))
                                                                        .Prop("latticeMin", Schema::Array(Schema::Int()).MinItems(3).MaxItems(3))
                                                                        .Prop("dimensions", Schema::Array(Schema::Int()).MinItems(3).MaxItems(3))
                                                                        .Required({ "level", "origin", "spacing", "latticeMin", "dimensions" })))
                                    .Required({ "availability", "freshness" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_DDGIProbeStats;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_froxel_fog_probe";
            tool.Toolset = "render";
            tool.Title = "Probe the froxel fog volume";
            // A 1x1x1 readback out of two 3D volumes; observable state unchanged.
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Sample the volumetric-fog froxel volume at one cell — the way to tell a BROKEN SCATTER "
                "PASS from a BROKEN COMPOSITE TAP without an intermediate-buffer PNG round trip. Every fog "
                "check we have compares final-frame pixels, which cannot distinguish 'no fog was injected "
                "here' from 'fog was injected but the composite sampled the wrong froxel'. This returns "
                "BOTH volumes at the sampled cell: 'scatter' (FroxelFogScatter.comp's output — per-froxel "
                "in-scattered radiance + extinction, i.e. what the media/lighting injection produced) and "
                "'integrated' (FroxelFogIntegrate.comp's — in-scatter accumulated from the camera to that "
                "slice + the transmittance, i.e. exactly what the fog composite trilinearly taps). Address "
                "the cell either directly with 'froxel':[x,y,z], or with 'worldPos':[x,y,z] — a world "
                "position projected through the SAME mapping the shaders use, including the exponential "
                "z-slice distribution (viewDepth = near * exp2(log2(far/near) * (z+0.5)/dimZ)). Also "
                "reports the froxel coords used, that cell's world-space bounds and view-depth range, and "
                "the volume's dims/near/far. A world position outside the frustum or the fog volume's "
                "depth range is reported as such, never silently answered from the nearest cell as if it "
                "were the point. Degrades with the reason when fog / volumetric fog is off (the froxel "
                "compute chain then never runs).";
            tool.InputSchema = Schema::Object()
                                   .Prop("froxel", Schema::Vec3("Froxel coordinates [x, y, z] into the fog volume (default dims 160x90x64; see the response's volume.dims)."))
                                   .Prop("worldPos", Schema::Vec3("World-space position [x, y, z], projected into froxel space with the shader's exact mapping."))
                                   .Prop("forceFrame", Schema::Bool().Desc("Render and settle a fresh frame before probing (default false). Use after a scene open / fog toggle so you cannot read a stale volume."))
                                   .NoAdditional();
            tool.OutputSchema = Schema::Object()
                                    .Prop("volume", Schema::Object()
                                                        .Prop("dims", Schema::Array(Schema::Int()).Desc("[x, y, z] froxel counts."))
                                                        .Prop("near", Schema::Number())
                                                        .Prop("far", Schema::Number())
                                                        .Prop("depthDistribution", Schema::String())
                                                        .Prop("renderOrigin", Schema::Vec3("Camera-relative rendering origin.")))
                                    .Prop("froxel", Schema::Object()
                                                        .Prop("coords", Schema::Array(Schema::Int()).Desc("The integer cell actually sampled."))
                                                        .Prop("continuous", Schema::Array(Schema::Number()))
                                                        .Prop("centerWorld", Schema::Vec3("World-space centre of the cell."))
                                                        .Prop("viewDepth", Schema::Number())
                                                        .Prop("clamped", Schema::Bool())
                                                        .Prop("inFrustum", Schema::Bool())
                                                        .Prop("inDepthRange", Schema::Bool())
                                                        .Prop("cellBounds", Schema::Object()
                                                                                .Prop("min", Schema::Vec3("World-space min corner."))
                                                                                .Prop("max", Schema::Vec3("World-space max corner."))
                                                                                .Prop("nearViewDepth", Schema::Number())
                                                                                .Prop("farViewDepth", Schema::Number())))
                                    .Prop("requestedWorldPos", Schema::Vec3("worldPos-mode echo; omitted in froxel mode."))
                                    .Prop("scatter", Schema::Object()
                                                         .Prop("available", Schema::Bool())
                                                         .Prop("inScatter", Schema::Vec3("Per-froxel in-scattered radiance; only when available."))
                                                         .Prop("extinction", Schema::Number().Desc("Only when available."))
                                                         .Prop("reason", Schema::String().Desc("Unavailability reason."))
                                                         .Desc("FroxelFogScatter.comp's output at the cell."))
                                    .Prop("integrated", Schema::Object()
                                                            .Prop("available", Schema::Bool())
                                                            .Prop("inScatter", Schema::Vec3("Accumulated in-scatter camera->slice; only when available."))
                                                            .Prop("transmittance", Schema::Number().Desc("Only when available."))
                                                            .Prop("reason", Schema::String().Desc("Unavailability reason."))
                                                            .Desc("FroxelFogIntegrate.comp's output — what the fog composite taps."))
                                    .Prop("note", Schema::String().Desc("Out-of-frustum / out-of-depth-range / clamped-cell caveat; omitted otherwise."))
                                    .Prop("fog", Schema::Object()
                                                     .Prop("enabled", Schema::Bool())
                                                     .Prop("volumetric", Schema::Bool())
                                                     .Prop("ranThisFrame", Schema::Bool()))
                                    .Prop("meta", Schema::Object()
                                                      .Prop("frameIndex", Schema::Int().Min(0))
                                                      .Prop("timestampMs", Schema::Int().Min(0))
                                                      .Prop("stale", Schema::Bool().Desc("The editor's loop was parked, so these values describe the last frame drawn before it stopped."))
                                                      .Prop("liveness", EditorLiveness::SchemaNode()))
                                    .Prop("staleness", Schema::String().Desc("Present only when the froxel chain did not run last frame."))
                                    .Required({ "volume", "froxel", "scatter", "integrated", "fog", "meta" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_FroxelFogProbe;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_shadow_atlas_layout";
            tool.Toolset = "render";
            tool.Title = "Shadow atlas layout";
            // Who won a tile vs who was STARVED is a two-table read at a glance.
            tool.DualAudienceContent = true;
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Report this frame's local-light shadow-atlas allocation — every shadow-casting spot / "
                "point / sphere-area light that COMPETED for a tile, whether it won one, and at what "
                "resolution. Per caster: the light entity UUID, the caster type, its priority score, "
                "its rank, and (when allocated) its atlas entries with each tile's x/y/width/height. "
                "The part a screenshot can never show is the losers: a caster that requested a shadow "
                "and was STARVED (out of entry / light / atlas-space budget, or scored 0 because its "
                "range sphere is outside the frustum) casts NO shadow, which is indistinguishable from "
                "a shadow bug until you can see it lost the contest. Also reports atlas area used, so a "
                "light packed into a tiny 256px tile (blocky shadow) is obvious. Read-only; the "
                "directional CSM is separate and is not packed into this atlas.";
            tool.InputSchema = Schema::EmptyObject();
            tool.OutputSchema = Schema::Object()
                                    .Prop("enabled", Schema::Bool())
                                    .Prop("atlasResolution", Schema::Int().Min(0))
                                    .Prop("maxEntries", Schema::Int().Min(0))
                                    .Prop("maxShadowedLights", Schema::Int().Min(0))
                                    .Prop("entriesUsed", Schema::Int().Min(0))
                                    .Prop("candidateCount", Schema::Int().Min(0))
                                    .Prop("allocatedCasters", Schema::Int().Min(0))
                                    .Prop("starvedCasters", Schema::Int().Min(0))
                                    .Prop("atlasAreaUsed", Schema::Number().Desc("Fraction of atlas pixels used [0, 1]."))
                                    .Prop("casters", Schema::Array(Schema::Object()
                                                                       .Prop("lightEntity", Schema::String())
                                                                       .Prop("casterType", Schema::String().Enum({ "Spot", "Point" }))
                                                                       .Prop("sourceKind", Schema::String())
                                                                       .Prop("score", Schema::Number())
                                                                       .Prop("allocated", Schema::Bool())
                                                                       .Prop("rank", Schema::Int().Desc("Allocated casters only."))
                                                                       .Prop("baseEntry", Schema::Int().Desc("Allocated casters only."))
                                                                       .Prop("entryCount", Schema::Int().Desc("Allocated casters only."))
                                                                       .Prop("tiles", Schema::Array(Schema::Object()
                                                                                                        .Prop("entry", Schema::Int())
                                                                                                        .Prop("face", Schema::Int().Desc("0..5 = +X,-X,+Y,-Y,+Z,-Z for a point caster."))
                                                                                                        .Prop("x", Schema::Int())
                                                                                                        .Prop("y", Schema::Int())
                                                                                                        .Prop("width", Schema::Int())
                                                                                                        .Prop("height", Schema::Int())
                                                                                                        .Prop("resolution", Schema::Int()))
                                                                                          .Desc("Allocated casters only."))
                                                                       .Prop("starvedReason", Schema::String().Desc("Starved casters only."))))
                                    .Prop("entries", Schema::Array(Schema::Object().Desc("The same tiles flattened: the tile keys plus lightEntity/casterType/sourceKind/rank/score.")))
                                    .Prop("directionalShadow", Schema::Object()
                                                                   .Prop("csmCascades", Schema::Int().Min(0))
                                                                   .Prop("resolution", Schema::Int().Min(0))
                                                                   .Desc("The separate directional CSM (not packed into this atlas)."))
                                    .Prop("note", Schema::String().Desc("Disabled / empty-atlas / starvation summary; omitted otherwise."))
                                    .Required({ "enabled", "atlasResolution", "maxEntries", "maxShadowedLights", "entriesUsed",
                                                "candidateCount", "allocatedCasters", "starvedCasters", "atlasAreaUsed",
                                                "casters", "entries", "directionalShadow" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_ShadowAtlasLayout;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_terrain_virtual_texture_stats";
            tool.Toolset = "render";
            tool.Title = "Terrain virtual-texture stats";
            // Pure read of the CPU-side counters the VT loop already maintains
            // every Update(); no GPU readback, no observable state change.
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Read the terrain adaptive virtual texture's (issue #715) own counters for every terrain "
                "in the active scene with a LIVE virtual texture. A thrashing cache and a converged one "
                "render the same frame, and only these counters separate 'bake budget too small' from "
                "'cache too small' from 'loop never converging'. What each counter answers: "
                "residentTiles vs cacheTileCount is cache occupancy; budgetStarvedRequests climbing while "
                "tilesBakedThisFrame sits at maxTileBakesPerFrame = the BAKE BUDGET is too small; "
                "workingSetExceedsCache (with evictionsTotal climbing) = the CACHE is too small — the "
                "camera wants more pages than physical tiles exist, so the LRU churns (not an error: the "
                "coarse-mip fallback covers the misses, at reduced sharpness); tilesBakedTotal AND "
                "evictionsTotal both still climbing under a STATIONARY camera = the loop never converges "
                "(feedback keeps re-requesting what was just evicted). readyForShading false = the "
                "coarsest page is not yet resident and the terrain still shades through the splat "
                "fallback. pagesRequested / feedbackTexelsWritten say whether feedback is arriving at "
                "all (both zero = the loop is not being driven); readbackSlotsInFlight is the feedback "
                "ring's depth. The indirection block (texels written/filled, publishes vs fullRebuilds, "
                "framesUpdated as the denominator, and the two GPU ms figures — each the LOWEST resolved "
                "sample since Configure(), not the latest) is the delta-vs-rebuild publish A/B "
                "(OLO_TERRAIN_VT_FULL_REBUILD drives the rebuild side). The adaptive block: sectorsReady "
                "vs sectorCount is per-sector readiness, imageResizesTotal / pagesRemappedTotal / "
                "pagesDroppedOnShrink count feedback-driven image resizes and what they carried or "
                "dropped, staleFeedbackTexels counts feedback no live image owns (normal for a few "
                "frames after a resize), atlasPagesAllocated vs virtualPagesWide^2 is atlas pressure, "
                "and imageAllocFailures means the atlas was FULL and a sector kept its old size. "
                "cacheCompressed / tilesCompressedTotal cover the BC7 cache path. Terrains whose VT is "
                "disabled or not yet created (it is created lazily on the first enabled frame) are "
                "counted in terrainEntities but not listed.";
            tool.InputSchema = Schema::EmptyObject();
            tool.OutputSchema = Schema::Object()
                                    .Prop("terrainEntities", Schema::Int().Min(0).Desc("Entities with a TerrainComponent, including ones without a live virtual texture."))
                                    .Prop("terrains", Schema::Array(Schema::Object()
                                                                        .Prop("entity", Schema::String())
                                                                        .Prop("name", Schema::String())
                                                                        .Prop("readyForShading", Schema::Bool().Desc("Coarsest page resident + published — the shader's VT-branch gate; false = splat fallback."))
                                                                        .Prop("stats", Schema::Object()
                                                                                           .Prop("cacheTileCount", Schema::Int().Min(0).Desc("Physical tiles the cache holds."))
                                                                                           .Prop("residentTiles", Schema::Int().Min(0).Desc("Tiles currently mapped to a page."))
                                                                                           .Prop("pagesRequested", Schema::Int().Min(0).Desc("Unique pages the last feedback analysis asked for."))
                                                                                           .Prop("feedbackTexelsWritten", Schema::Int().Min(0).Desc("Feedback texels that carried a request."))
                                                                                           .Prop("tilesBakedThisFrame", Schema::Int().Min(0))
                                                                                           .Prop("tilesBakedTotal", Schema::Int().Min(0))
                                                                                           .Prop("evictionsTotal", Schema::Int().Min(0))
                                                                                           .Prop("budgetStarvedRequests", Schema::Int().Min(0).Desc("Requests deferred past the per-frame bake budget — climbing = bake budget too small."))
                                                                                           .Prop("workingSetExceedsCache", Schema::Bool().Desc("The camera wants more pages than the cache holds — cache too small (coarse-mip fallback covers the misses)."))
                                                                                           .Prop("readbackSlotsInFlight", Schema::Int().Min(0))
                                                                                           .Prop("cacheBytes", Schema::Int().Min(0))
                                                                                           .Prop("indirectionBytes", Schema::Int().Min(0))
                                                                                           .Prop("indirectionTexelsWritten", Schema::Int().Min(0).Desc("Texels the last publish wrote (clear pass included)."))
                                                                                           .Prop("indirectionTexelsFilled", Schema::Int().Min(0).Desc("Texels the last publish re-propagated coarse->fine."))
                                                                                           .Prop("indirectionPublishes", Schema::Int().Min(0))
                                                                                           .Prop("indirectionFullRebuilds", Schema::Int().Min(0).Desc("Of the publishes, how many rebuilt the whole map."))
                                                                                           .Prop("framesUpdated", Schema::Int().Min(0).Desc("Frames Update() ran — the denominator for the publish counters."))
                                                                                           .Prop("indirectionRebuildGpuMs", Schema::Number().Desc("LOWEST resolved sample since Configure(), not the latest; 0 until one resolves."))
                                                                                           .Prop("indirectionDeltaGpuMs", Schema::Number().Desc("LOWEST resolved sample since Configure(), not the latest; 0 until one resolves."))
                                                                                           .Prop("sectorCount", Schema::Int().Min(0))
                                                                                           .Prop("sectorsReady", Schema::Int().Min(0).Desc("Sectors whose coarsest page is resident + published."))
                                                                                           .Prop("imageResizesTotal", Schema::Int().Min(0))
                                                                                           .Prop("pagesRemappedTotal", Schema::Int().Min(0).Desc("Pages carried across a resize without a rebake."))
                                                                                           .Prop("pagesDroppedOnShrink", Schema::Int().Min(0).Desc("Finest-level pages a shrink discarded."))
                                                                                           .Prop("staleFeedbackTexels", Schema::Int().Min(0).Desc("Feedback no live image owns — normal for a few frames after a resize."))
                                                                                           .Prop("atlasPagesAllocated", Schema::Int().Min(0).Desc("Sum of sizePages^2 over the sector images — over virtualPagesWide^2."))
                                                                                           .Prop("imageAllocFailures", Schema::Int().Min(0).Desc("Atlas full: a sector kept its old size."))
                                                                                           .Prop("cacheCompressed", Schema::Bool())
                                                                                           .Prop("tilesCompressedTotal", Schema::Int().Min(0))
                                                                                           .Desc("TerrainVirtualTexture::Stats, verbatim."))
                                                                        .Prop("config", Schema::Object()
                                                                                            .Prop("virtualPagesWide", Schema::Int().Min(0))
                                                                                            .Prop("pageTexels", Schema::Int().Min(0))
                                                                                            .Prop("borderTexels", Schema::Int().Min(0))
                                                                                            .Prop("cacheTilesWide", Schema::Int().Min(0))
                                                                                            .Prop("maxTileBakesPerFrame", Schema::Int().Min(0))
                                                                                            .Prop("adaptiveEnabled", Schema::Bool())
                                                                                            .Prop("sectorsWide", Schema::Int().Min(0))
                                                                                            .Prop("minImagePagesWide", Schema::Int().Min(0))
                                                                                            .Prop("maxImagePagesWide", Schema::Int().Min(0))
                                                                                            .Prop("trilinearEnabled", Schema::Bool())
                                                                                            .Prop("compressedCache", Schema::Bool())
                                                                                            .Desc("The knobs that give the counters their denominators.")))
                                                          .Desc("One entry per terrain with a LIVE virtual texture."))
                                    .Prop("note", Schema::String().Desc("Why the list is empty (no terrain, or no live VT yet); omitted otherwise."))
                                    .Required({ "terrainEntities", "terrains" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_TerrainVirtualTextureStats;
            server.RegisterTool(std::move(tool));
        }
    }
} // namespace OloEngine::MCP

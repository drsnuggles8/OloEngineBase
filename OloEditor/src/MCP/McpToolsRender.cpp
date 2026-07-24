#include "OloEnginePCH.h"
#include "MCP/McpToolsCommon.h"
#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpClusterGridStats.h"
#include "MCP/McpFrameBreakdown.h"
#include "MCP/McpFroxelFogProbe.h"
#include "MCP/McpGoldenCompare.h"
#include "MCP/McpRenderExplain.h"
#include "MCP/McpRenderGraphTopology.h"
#include "MCP/McpRenderOverrides.h"
#include "MCP/McpRenderProbePixel.h"
#include "MCP/McpRenderTargetStats.h"
#include "MCP/McpRenderValidate.h"
#include "MCP/McpRendererSettings.h"
#include "MCP/McpShadowCapture.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Atmosphere/Ephemeris.h"
#include "OloEngine/Atmosphere/WeatherSystem.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Debug/CapturedFrameData.h"
#include "OloEngine/Renderer/Debug/CommandPacketDebugger.h"
#include "OloEngine/Renderer/Debug/FrameCaptureManager.h"
#include "OloEngine/Renderer/Debug/GPUResourceInspector.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Debug/ShaderDebugger.h"
#include "OloEngine/Renderer/Commands/RenderCommand.h"
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
#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/SubmeshMaterialResolve.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Shadow/ShadowAtlas.h"
#include "OloEngine/Renderer/Shadow/ShadowMap.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/VirtualGeometry/VirtualMeshRegistry.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

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

        // ---- olo_render_frame_breakdown (main-marshaled) -----------------------
        // The per-command / per-pipeline-stage structural view olo_perf_capture_frame
        // omits. Same capture-then-poll trigger as Handle_PerfCaptureFrame, then the
        // freshly captured frame is shaped by the pure FrameBreakdown::BuildBreakdown
        // (JSON) or CommandPacketDebugger::BuildMarkdownReport (the Command Bucket
        // Inspector's LLM-analysis report). After capture, the live render graph is
        // read once more to attribute the captured bucket to its owning pass and place
        // it in the whole-graph command-bucket landscape (#316 Part 4). Pure read — no
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
            // (#316 Part 4). The graph is main-thread state, so the walk runs inside
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

        // ---- Render-target capture (Part 4 of #316) ----------------------------

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
                if (resource->Desc.Kind == ResourceHandle::Kind::TextureCube)
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

        // ---- olo_render_graph_topology_export (main-marshaled) -----------------
        // Read-only structured export of the live RenderGraph topology — passes,
        // execution order, pass-dependency edges, and resources with their
        // producers/consumers — so an agent can reason about the render pipeline
        // (#316 Part 4, "LLM-analysis exports"). The RenderGraph is main-thread
        // state, so the enumeration runs inside MarshalRead; the JSON / Mermaid
        // shaping is the pure, unit-tested RenderGraphTopology core.
        ToolResult Handle_RenderGraphTopologyExport(McpServer& server, const Json& args)
        {
            std::string format = "json";
            if (args.contains("format") && args["format"].is_string())
            {
                format = args["format"].get<std::string>();
                if (format != "json" && format != "mermaid")
                    return ToolResult::Error("format must be \"json\" or \"mermaid\".");
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

                    // Resolved physical GL object ids (issue #607) — the
                    // one-call answer to "do these two passes touch the same
                    // physical texture this frame". Resolved inside this same
                    // main-thread job as the enumeration so the snapshot is
                    // internally consistent; the ids are the LAST EXECUTED
                    // frame's (transients can re-alias next frame).
                    if (resource.TextureHandle.IsValid())
                    {
                        info.GLTextureId = graph->ResolveTexture(resource.TextureHandle);
                        info.ViewOfParentLayer = graph->GetTextureViewLayerIndex(resource.Name);
                    }
                    if (resource.FramebufferHandle.IsValid())
                    {
                        if (const Ref<Framebuffer> framebuffer = graph->ResolveFramebuffer(resource.FramebufferHandle))
                        {
                            info.GLFramebufferId = framebuffer->GetRendererID();
                            const auto& attachments = framebuffer->GetSpecification().Attachments.Attachments;
                            u32 colorIndex = 0;
                            for (const auto& attachment : attachments)
                            {
                                if (attachment.TextureFormat == FramebufferTextureFormat::None)
                                    continue;
                                if (attachment.TextureFormat == FramebufferTextureFormat::DEPTH24STENCIL8 ||
                                    attachment.TextureFormat == FramebufferTextureFormat::DEPTH_COMPONENT32F)
                                {
                                    info.GLDepthAttachmentId = framebuffer->GetDepthAttachmentRendererID();
                                }
                                else
                                {
                                    info.GLColorAttachmentIds.push_back(
                                        framebuffer->GetColorAttachmentRendererID(colorIndex));
                                    ++colorIndex;
                                }
                            }
                        }
                    }
                    if (resource.BufferHandle.IsValid())
                        info.GLBufferId = graph->ResolveBuffer(resource.BufferHandle);

                    snap.Resources.push_back(std::move(info));
                }

                // Mermaid is a pure transform of the snapshot, but the snapshot can
                // only be gathered on the main thread, so build the text here and
                // ferry it out under a sentinel key.
                if (format == "mermaid")
                    return Json{ { "__text", RenderGraphTopology::BuildMermaid(snap) } };
                return RenderGraphTopology::BuildJson(snap); });

            if (transport.is_object() && transport.contains("__error"))
                return ToolResult::Error(transport["__error"].get<std::string>());
            if (transport.is_object() && transport.contains("__text"))
                return ToolResult::Text(transport["__text"].get<std::string>());
            return ToolResult::Structured(transport);
        }

        // (main thread) Resolve a render-graph resource name to a physical GL
        // texture id, or 0 when the name is unknown / has no GPU backing this
        // frame. First as a graph texture (covers attachment views like
        // SceneDepth / GBufferAlbedo and imported textures like ShadowMapCSM),
        // then as a framebuffer (colour attachment 0, or the depth attachment for
        // depth-only targets). Shared by olo_render_capture_target and
        // olo_render_probe_pixel so both resolve names identically.
        u32 ResolveTargetTexture(const std::string& name)
        {
            if (const u32 textureId = Renderer3D::ResolveFrameGraphTexture(name); textureId != 0)
                return textureId;

            const Ref<Framebuffer> framebuffer = Renderer3D::ResolveFrameGraphFramebuffer(name);
            if (!framebuffer)
                return 0;

            bool hasColor = false;
            for (const auto& attachment : framebuffer->GetSpecification().Attachments.Attachments)
            {
                if (attachment.TextureFormat != FramebufferTextureFormat::DEPTH24STENCIL8 &&
                    attachment.TextureFormat != FramebufferTextureFormat::DEPTH_COMPONENT32F &&
                    attachment.TextureFormat != FramebufferTextureFormat::None)
                {
                    hasColor = true;
                    break;
                }
            }
            return hasColor ? framebuffer->GetColorAttachmentRendererID(0)
                            : framebuffer->GetDepthAttachmentRendererID();
        }

        // Wall-clock + frame stamp attached to every capture/probe response so a
        // STALE answer is detectable (issue #607). The motivating bug: after an
        // olo_scene_open, a capture came back byte-identical (same md5) to the
        // previous scene's — a silent wrong answer, because the render-graph
        // texture had not been redrawn yet and nothing in the response said so.
        // With the frame index in the meta, an agent can see two captures came
        // from the SAME frame; with `forceFrame` it can demand a fresh one.
        Json CaptureStampJson(u64 frameIndex)
        {
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            Json j;
            j["frameIndex"] = frameIndex;
            j["timestampMs"] = static_cast<u64>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
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

            // Opt-in resource-link delivery (issue #673 Tier 1): publish the PNG
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
                                              normalizeMode, maxWidth, afterPass, afterPassFrameRendered,
                                              deliverLink]() -> Json
                                             {
                const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph();
                if (!graph)
                    return Json{ { "__error", "No active render graph (the editor is not in 3D mode, or no frame has been rendered yet)." } };

                u32 textureId = 0;
                RenderGraphPassSnapshot::Result snapshotResult;
                if (!afterPass.empty())
                {
                    if (const std::string error =
                            CollectAfterPassSnapshot(afterPass, name, afterPassFrameRendered, snapshotResult);
                        !error.empty())
                        return Json{ { "__error", error } };
                    textureId = snapshotResult.TextureID;
                }
                else
                {
                    textureId = ResolveTargetTexture(name);
                }
                if (textureId == 0)
                    return Json{ { "__error", "Unknown render-graph resource '" + name +
                                                  "' (or it has no GPU backing this frame). Call olo_render_list_targets for the live list." } };

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

                auto capture = GPUResourceInspector::CaptureTexturePng(textureId, mipLevel, selection.Layer,
                                                                       normalizeMode, maxWidth);
                if (!capture.Error.empty())
                    return Json{ { "__error", "Capture of '" + name + "' failed: " + capture.Error } };

                Json meta = CaptureStampJson(server.Context().GetFrameIndex ? server.Context().GetFrameIndex() : 0);
                meta["name"] = name;
                if (!afterPass.empty())
                {
                    meta["afterPass"] = afterPass;
                    meta["snapshotSourceTextureId"] = snapshotResult.SourceTextureID;
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
                // resource_link instead of inline base64 (issue #673 Tier 1).
                const Json::binary_t& png = result["png"].get_binary();
                std::vector<u8> bytes(png.begin(), png.end());
                const u64 sizeBytes = static_cast<u64>(bytes.size());
                const u64 sequence = NextCaptureSequence();
                const std::string uri = "olo://capture/" + std::to_string(sequence) + "/target.png";
                const std::string linkName = "target-" + std::to_string(sequence) + ".png";

                ResourceDef captureResource;
                captureResource.Uri = uri;
                captureResource.Name = linkName;
                captureResource.Description =
                    "Render-target PNG capture of '" + name + "' (capture meta in the tool result).";
                captureResource.MimeType = "image/png";
                captureResource.SizeBytes = sizeBytes;
                captureResource.BlobReader = [bytes = std::move(bytes)](McpServer&)
                { return bytes; };
                server.RegisterEphemeralResource(std::move(captureResource));

                meta["resourceUri"] = uri;
                toolResult.Content = Json::array(
                    { Json{ { "type", "text" }, { "text", meta.dump(2) } },
                      ToolResult::ResourceLinkBlock(uri, linkName,
                                                    "Render-target capture of '" + name +
                                                        "' (PNG); fetch via resources/read.",
                                                    "image/png", sizeBytes) });
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

        // ---- olo_render_toggle_pass / olo_render_set_debug_view (#316 Part 4) ---
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
                r.PassEnabled = deferred && virtualRegistry.GetDebugColorTextureID() != 0;
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

        // ---- olo_render_compare_golden (Part 4 of #316) ------------------------

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
        // OLOENGINE_GOLDEN_REBASE workflow. The diff math itself lives in the pure,
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
                    capturedPng = server.Context().CaptureViewportPng(maxWidth);
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
            const auto attachCapture = [&server, &capturedPng, deliverLink](Json& verdict) -> Json
            {
                if (!deliverLink)
                    return Json{ { "type", "image" },
                                 { "data", Base64Encode(capturedPng) },
                                 { "mimeType", "image/png" } };
                const u64 sizeBytes = static_cast<u64>(capturedPng.size());
                const u64 sequence = NextCaptureSequence();
                const std::string uri = "olo://capture/" + std::to_string(sequence) + "/golden-capture.png";
                const std::string linkName = "golden-capture-" + std::to_string(sequence) + ".png";

                ResourceDef captureResource;
                captureResource.Uri = uri;
                captureResource.Name = linkName;
                captureResource.Description =
                    "Viewport capture from olo_render_compare_golden (verdict in the tool result).";
                captureResource.MimeType = "image/png";
                captureResource.SizeBytes = sizeBytes;
                captureResource.BlobReader = [bytes = capturedPng](McpServer&)
                { return bytes; };
                server.RegisterEphemeralResource(std::move(captureResource));

                verdict["resourceUri"] = uri;
                return ToolResult::ResourceLinkBlock(uri, linkName,
                                                     "Captured viewport frame (PNG); fetch via resources/read.",
                                                     "image/png", sizeBytes);
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
                lever.SoftShadows = Renderer3D::GetShadowMap().GetSettings().SoftShadows;
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
                const ApplyResult applied = Apply(setting, value, pp, rs, lever);
                if (!applied.Ok)
                    return Json{ { "__error", applied.Error } };
                // A render-path switch changes the registered pass list, so the
                // render-graph topology must be rebuilt for the new value to render.
                if (applied.PathChanged)
                    Renderer3D::ApplyRendererSettings();
                // Push mutated lever fields back to the renderer — Apply only wrote
                // the POD snapshot (the header stays renderer-free).
                if (setting == Setting::DepthPrepass)
                {
                    Renderer3D::EnableDepthPrepass(lever.DepthPrepassEnabled);
                }
                else if (setting == Setting::SoftShadows)
                {
                    ShadowSettings shadow = Renderer3D::GetShadowMap().GetSettings();
                    shadow.SoftShadows = lever.SoftShadows;
                    Renderer3D::GetShadowMap().SetSettings(shadow);
                }
                else
                {
                    // The remaining settings (upscale / tonemap / renderpath) live
                    // entirely in the settings structs Apply already wrote.
                }
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

            Json result = server.MarshalRead([&server, id]() -> Json
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
                return j; });

            return ToolResult::Structured(result);
        }

        // =====================================================================
        // Issue #607 — render-diagnostics gaps found while doing real work.
        // =====================================================================

        // ---- olo_render_probe_pixel (main-marshaled; GL readback) --------------
        //
        // The NUMERIC counterpart of olo_render_capture_target: instead of an
        // image of a whole target, the exact decoded values under ONE pixel,
        // across every G-Buffer channel at once. A capture shows a normal map
        // "looks bluish"; this says the normal is (0.0, 0.0, 1.0) when it should
        // be (0, 1, 0) — which is the difference between an hour of shader
        // patching and a one-call diagnosis.

        // GL internal format -> readback (format, type) + a stable token. Mirrors
        // GPUResourceInspector::CaptureTexturePng's table with two deliberate
        // differences: INTEGER formats stay integer (the R32I entity-id
        // attachment must never be reported as a float — an agent compares it
        // against a real entity id), and nothing is normalised (a probe wants the
        // raw number, not a display-friendly remap).
        struct ProbeFormat
        {
            GLenum ReadFormat = GL_NONE;
            GLenum ReadType = GL_NONE;
            i32 Channels = 0;
            const char* Token = "Unknown";
            bool IsInteger = false;
        };

        bool DescribeProbeFormat(GLint internalFormat, ProbeFormat& out)
        {
            switch (internalFormat)
            {
                case GL_RGBA8:
                    out = { GL_RGBA, GL_FLOAT, 4, "RGBA8", false };
                    return true;
                case GL_SRGB8_ALPHA8:
                    out = { GL_RGBA, GL_FLOAT, 4, "SRGB8_ALPHA8", false };
                    return true;
                case GL_RGB8:
                    out = { GL_RGB, GL_FLOAT, 3, "RGB8", false };
                    return true;
                case GL_SRGB8:
                    out = { GL_RGB, GL_FLOAT, 3, "SRGB8", false };
                    return true;
                case GL_RG8:
                    out = { GL_RG, GL_FLOAT, 2, "RG8", false };
                    return true;
                case GL_R8:
                    out = { GL_RED, GL_FLOAT, 1, "R8", false };
                    return true;
                case GL_RGBA16F:
                    out = { GL_RGBA, GL_FLOAT, 4, "RGBA16F", false };
                    return true;
                case GL_RGBA32F:
                    out = { GL_RGBA, GL_FLOAT, 4, "RGBA32F", false };
                    return true;
                case GL_RGB16F:
                    out = { GL_RGB, GL_FLOAT, 3, "RGB16F", false };
                    return true;
                case GL_RGB32F:
                    out = { GL_RGB, GL_FLOAT, 3, "RGB32F", false };
                    return true;
                case GL_R11F_G11F_B10F:
                    out = { GL_RGB, GL_FLOAT, 3, "R11F_G11F_B10F", false };
                    return true;
                case GL_RG16F:
                    out = { GL_RG, GL_FLOAT, 2, "RG16F", false };
                    return true;
                case GL_RG32F:
                    out = { GL_RG, GL_FLOAT, 2, "RG32F", false };
                    return true;
                case GL_R16F:
                    out = { GL_RED, GL_FLOAT, 1, "R16F", false };
                    return true;
                case GL_R32F:
                    out = { GL_RED, GL_FLOAT, 1, "R32F", false };
                    return true;
                case GL_R32I:
                    out = { GL_RED_INTEGER, GL_INT, 1, "R32I", true };
                    return true;
                case GL_R32UI:
                    out = { GL_RED_INTEGER, GL_INT, 1, "R32UI", true };
                    return true;
                case GL_RG32I:
                    out = { GL_RG_INTEGER, GL_INT, 2, "RG32I", true };
                    return true;
                case GL_RGBA32I:
                    out = { GL_RGBA_INTEGER, GL_INT, 4, "RGBA32I", true };
                    return true;
                case GL_DEPTH_COMPONENT16:
                    out = { GL_DEPTH_COMPONENT, GL_FLOAT, 1, "DEPTH16", false };
                    return true;
                case GL_DEPTH_COMPONENT24:
                    out = { GL_DEPTH_COMPONENT, GL_FLOAT, 1, "DEPTH24", false };
                    return true;
                case GL_DEPTH_COMPONENT32:
                    out = { GL_DEPTH_COMPONENT, GL_FLOAT, 1, "DEPTH32", false };
                    return true;
                case GL_DEPTH_COMPONENT32F:
                    out = { GL_DEPTH_COMPONENT, GL_FLOAT, 1, "DEPTH32F", false };
                    return true;
                case GL_DEPTH24_STENCIL8:
                    out = { GL_DEPTH_COMPONENT, GL_FLOAT, 1, "DEPTH24_STENCIL8", false };
                    return true;
                case GL_DEPTH32F_STENCIL8:
                    out = { GL_DEPTH_COMPONENT, GL_FLOAT, 1, "DEPTH32F_STENCIL8", false };
                    return true;
                default:
                    return false;
            }
        }

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
            // Read THIS texture instead of resolving `name` — the afterPass
            // snapshot scratch clone.
            u32 OverrideTextureId = 0;
            // Explicit array-layer / cube-face / 3D-slice selector; when
            // absent the target's own view layer applies (a CSM cascade view
            // must not silently read cascade 0).
            bool HasLayer = false;
            long long Layer = 0;
        };

        // (main thread) Read back a SINGLE texel of one named render-graph target.
        // Uses glGetTextureSubImage (DSA, GL 4.5+) with a 1x1 region — never a
        // whole-texture readback, so probing a 4K G-Buffer costs a handful of
        // bytes, not 64 MB.
        //
        // Coordinates go through ProbePixel::MapProbeCoord (issue #607):
        // viewport space maps proportionally onto the target mip, texel space
        // addresses the exact texel; both are top-left-origin (the screenshot
        // convention — GL's bottom-up flip happens here) and the mapping is
        // echoed in the sample so it is never guesswork.
        ProbePixel::TexelSample ProbeTexel(const RenderGraph& graph, const std::string& name,
                                           u32 x, u32 yTopLeft, const ProbeRequest& request = {})
        {
            ProbePixel::TexelSample sample;
            sample.Target = name;

            const u32 textureId = request.OverrideTextureId != 0 ? request.OverrideTextureId
                                                                 : ResolveTargetTexture(name);
            if (textureId == 0 || glIsTexture(textureId) == GL_FALSE)
            {
                sample.Unavailable = "Render-graph resource '" + name +
                                     "' has no GPU backing this frame (wrong rendering path, effect disabled, or not yet rendered).";
                return sample;
            }

            // Which GL layer (z offset) to read: an explicit selector, or the
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

            GLint width = 0;
            GLint height = 0;
            GLint internalFormat = 0;
            const auto mipLevel = static_cast<GLint>(request.Mip);
            glGetTextureLevelParameteriv(textureId, mipLevel, GL_TEXTURE_WIDTH, &width);
            glGetTextureLevelParameteriv(textureId, mipLevel, GL_TEXTURE_HEIGHT, &height);
            glGetTextureLevelParameteriv(textureId, mipLevel, GL_TEXTURE_INTERNAL_FORMAT, &internalFormat);
            if (width <= 0 || height <= 0)
            {
                sample.Unavailable = "'" + name + "' has no storage at mip " + std::to_string(request.Mip) + ".";
                return sample;
            }
            sample.SourceWidth = static_cast<u32>(width);
            sample.SourceHeight = static_cast<u32>(height);

            ProbeFormat format;
            if (!DescribeProbeFormat(internalFormat, format))
            {
                sample.Unavailable = "'" + name + "' has an internal format this probe cannot decode (0x" +
                                     std::format("{:X}", static_cast<u32>(internalFormat)) + ").";
                return sample;
            }
            sample.Format = format.Token;

            const u32 refWidth = request.RefWidth != 0 ? request.RefWidth : sample.SourceWidth;
            const u32 refHeight = request.RefHeight != 0 ? request.RefHeight : sample.SourceHeight;
            sample.Mapped = ProbePixel::MapProbeCoord(request.Space, x, yTopLeft, refWidth, refHeight,
                                                      sample.SourceWidth, sample.SourceHeight, request.Mip);
            if (!sample.Mapped.Valid)
            {
                sample.Unavailable = sample.Mapped.Error;
                return sample;
            }

            // Tight packing + PBO unbind guard, same rationale as
            // GPUResourceInspector::CaptureTexturePng: a bound pixel-pack buffer
            // would silently redirect the read into GPU memory.
            GLint prevPackAlignment = 4;
            glGetIntegerv(GL_PACK_ALIGNMENT, &prevPackAlignment);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            GLint prevPackPBO = 0;
            glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prevPackPBO);
            if (prevPackPBO != 0)
                glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

            // Asking GL for GL_FLOAT on an RGBA8/RGBA16F source lets the driver do
            // the (half|unorm)->float conversion, so no manual half decode is needed.
            std::array<f32, 4> floats{ 0.0f, 0.0f, 0.0f, 0.0f };
            std::array<i32, 4> ints{ 0, 0, 0, 0 };
            void* destination = format.IsInteger ? static_cast<void*>(ints.data())
                                                 : static_cast<void*>(floats.data());
            const auto bytes = static_cast<GLsizei>(4 * sizeof(f32));

            glGetTextureSubImage(textureId, mipLevel,
                                 static_cast<GLint>(sample.Mapped.TexelX),
                                 static_cast<GLint>(sample.Mapped.GLRowBottomUp),
                                 static_cast<GLint>(selection.Layer),
                                 1, 1, 1,
                                 format.ReadFormat, format.ReadType,
                                 bytes, destination);

            if (prevPackPBO != 0)
                glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(prevPackPBO));
            glPixelStorei(GL_PACK_ALIGNMENT, prevPackAlignment);

            if (const GLenum error = glGetError(); error != GL_NO_ERROR)
            {
                sample.Unavailable = "glGetTextureSubImage on '" + name + "' failed (GL 0x" +
                                     std::format("{:X}", static_cast<u32>(error)) + ").";
                return sample;
            }

            sample.Available = true;
            sample.Channels = format.Channels;
            sample.Kind = format.IsInteger ? ProbePixel::SampleKind::Int : ProbePixel::SampleKind::Float;
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
                        [resourceName]() -> u32
                        { return ResolveTargetTexture(resourceName); } });
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
                        request.OverrideTextureId = snapshotResult.TextureID;
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
                    j["meta"] = CaptureStampJson(frameIndex);
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
                j["meta"] = CaptureStampJson(frameIndex);
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

        // (main thread) Read a rect of one mip as channel-interleaved floats.
        // Integer formats are converted (exact below 2^24 — entity ids and
        // counters qualify); the caller notes the conversion in the reply.
        std::vector<f32> ReadRectFloats(const u32 textureId, const u32 mip, const u32 layer,
                                        const u32 rectX, const u32 glRectY, const u32 rectW, const u32 rectH,
                                        const ProbeFormat& format, std::string& outError)
        {
            std::vector<f32> interleaved;
            const sizet texels = static_cast<sizet>(rectW) * rectH;
            const sizet valueCount = texels * static_cast<sizet>(format.Channels);

            // Drain any stale error left by earlier rendering so the check
            // below attributes failures to THIS readback — a lingering error
            // would otherwise discard a perfectly valid read.
            while (glGetError() != GL_NO_ERROR)
            {
            }

            GLint prevPackAlignment = 4;
            glGetIntegerv(GL_PACK_ALIGNMENT, &prevPackAlignment);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            GLint prevPackPBO = 0;
            glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prevPackPBO);
            if (prevPackPBO != 0)
                glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

            if (format.IsInteger)
            {
                std::vector<i32> ints(valueCount, 0);
                glGetTextureSubImage(textureId, static_cast<GLint>(mip),
                                     static_cast<GLint>(rectX), static_cast<GLint>(glRectY),
                                     static_cast<GLint>(layer),
                                     static_cast<GLsizei>(rectW), static_cast<GLsizei>(rectH), 1,
                                     format.ReadFormat, format.ReadType,
                                     static_cast<GLsizei>(ints.size() * sizeof(i32)), ints.data());
                interleaved.reserve(valueCount);
                for (const i32 v : ints)
                    interleaved.push_back(static_cast<f32>(v));
            }
            else
            {
                interleaved.assign(valueCount, 0.0f);
                glGetTextureSubImage(textureId, static_cast<GLint>(mip),
                                     static_cast<GLint>(rectX), static_cast<GLint>(glRectY),
                                     static_cast<GLint>(layer),
                                     static_cast<GLsizei>(rectW), static_cast<GLsizei>(rectH), 1,
                                     format.ReadFormat, format.ReadType,
                                     static_cast<GLsizei>(interleaved.size() * sizeof(f32)), interleaved.data());
            }

            if (prevPackPBO != 0)
                glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(prevPackPBO));
            glPixelStorei(GL_PACK_ALIGNMENT, prevPackAlignment);

            if (const GLenum error = glGetError(); error != GL_NO_ERROR)
            {
                outError = "glGetTextureSubImage failed (GL 0x" +
                           std::format("{:X}", static_cast<u32>(error)) + ").";
                interleaved.clear();
            }
            return interleaved;
        }

        ToolResult Handle_RenderTargetStats(McpServer& server, const Json& args)
        {
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

                u32 textureId = 0;
                RenderGraphPassSnapshot::Result snapshotResult;
                if (!afterPass.empty())
                {
                    if (const std::string error =
                            CollectAfterPassSnapshot(afterPass, name, afterPassFrameRendered, snapshotResult);
                        !error.empty())
                        return Json{ { "__error", error } };
                    textureId = snapshotResult.TextureID;
                }
                else
                {
                    textureId = ResolveTargetTexture(name);
                }
                if (textureId == 0 || glIsTexture(textureId) == GL_FALSE)
                    return Json{ { "__error", "Unknown render-graph resource '" + name +
                                                  "' (or it has no GPU backing this frame). Call olo_render_list_targets for the live list." } };

                const CaptureLayer::TargetLayers layers = ResolveTargetLayers(*graph, name);
                const CaptureLayer::Selection selection =
                    CaptureLayer::SelectLayer(layers, name, hasLayer, requestedLayer);
                if (!selection.Error.empty())
                    return Json{ { "__error", selection.Error } };

                GLint mipWidth = 0;
                GLint mipHeight = 0;
                GLint internalFormat = 0;
                glGetTextureLevelParameteriv(textureId, static_cast<GLint>(mip), GL_TEXTURE_WIDTH, &mipWidth);
                glGetTextureLevelParameteriv(textureId, static_cast<GLint>(mip), GL_TEXTURE_HEIGHT, &mipHeight);
                glGetTextureLevelParameteriv(textureId, static_cast<GLint>(mip), GL_TEXTURE_INTERNAL_FORMAT,
                                             &internalFormat);
                if (mipWidth <= 0 || mipHeight <= 0)
                    return Json{ { "__error", "'" + name + "' has no storage at mip " + std::to_string(mip) + "." } };

                ProbeFormat format;
                if (!DescribeProbeFormat(internalFormat, format))
                    return Json{ { "__error", "'" + name + "' has an internal format stats cannot decode (0x" +
                                                  std::format("{:X}", static_cast<u32>(internalFormat)) + ")." } };

                const auto fullW = static_cast<u32>(mipWidth);
                const auto fullH = static_cast<u32>(mipHeight);
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

                // Stats aggregate per channel, so GL row order is irrelevant —
                // only the rect's PLACEMENT must be flipped to GL's bottom-up
                // rows: top-left rect row y..y+h maps to GL rows
                // [mipH - y - h, mipH - y).
                const u32 glRectY = fullH - y - h;
                std::string readError;
                const std::vector<f32> interleaved =
                    ReadRectFloats(textureId, mip, selection.Layer, x, glRectY, w, h, format, readError);
                if (!readError.empty())
                    return Json{ { "__error", "Stats readback of '" + name + "' failed: " + readError } };

                Json j = RenderTargetStats::BuildStatsJson(name, format.Token, x, y, w, h, mip, fullW, fullH,
                                                           selection.Layer, interleaved, format.Channels);
                if (format.IsInteger)
                    j["integerNote"] = "Integer target: values converted to float for stats (bit-exact below 2^24).";
                if (!afterPass.empty())
                {
                    j["afterPass"] = afterPass;
                    j["afterPassNote"] = "meta.frameIndex is the collect-time frame; the snapshot was cloned "
                                         "mid-frame during the immediately preceding rendered frame.";
                }
                if (!selection.Note.empty())
                    j["layerNote"] = selection.Note;
                j["meta"] = CaptureStampJson(server.Context().GetFrameIndex ? server.Context().GetFrameIndex() : 0);
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
        std::vector<f32> ReadChannel0TopLeft(const u32 textureId, const u32 mip, const u32 layer,
                                             u32& outWidth, u32& outHeight, std::string& outFormat,
                                             std::string& outError)
        {
            outWidth = 0;
            outHeight = 0;
            GLint mipWidth = 0;
            GLint mipHeight = 0;
            GLint internalFormat = 0;
            glGetTextureLevelParameteriv(textureId, static_cast<GLint>(mip), GL_TEXTURE_WIDTH, &mipWidth);
            glGetTextureLevelParameteriv(textureId, static_cast<GLint>(mip), GL_TEXTURE_HEIGHT, &mipHeight);
            glGetTextureLevelParameteriv(textureId, static_cast<GLint>(mip), GL_TEXTURE_INTERNAL_FORMAT,
                                         &internalFormat);
            if (mipWidth <= 0 || mipHeight <= 0)
            {
                outError = "no storage at mip " + std::to_string(mip);
                return {};
            }
            ProbeFormat format;
            if (!DescribeProbeFormat(internalFormat, format))
            {
                outError = "undecodable internal format (0x" +
                           std::format("{:X}", static_cast<u32>(internalFormat)) + ")";
                return {};
            }
            outFormat = format.Token;
            const auto w = static_cast<u32>(mipWidth);
            const auto h = static_cast<u32>(mipHeight);
            if (static_cast<u64>(w) * h > kMaxStatsRectTexels * 4ull)
            {
                outError = "mip is larger than the compare ceiling (" + std::to_string(w) + "x" +
                           std::to_string(h) + "); compare a higher mip";
                return {};
            }

            // Same stale-error drain rationale as ReadRectFloats.
            while (glGetError() != GL_NO_ERROR)
            {
            }

            const bool isDepth = format.ReadFormat == GL_DEPTH_COMPONENT;
            const GLenum readFormat = isDepth ? GL_DEPTH_COMPONENT
                                              : (format.IsInteger ? GL_RED_INTEGER : GL_RED);

            GLint prevPackAlignment = 4;
            glGetIntegerv(GL_PACK_ALIGNMENT, &prevPackAlignment);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            GLint prevPackPBO = 0;
            glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prevPackPBO);
            if (prevPackPBO != 0)
                glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

            std::vector<f32> bottomUp;
            if (format.IsInteger)
            {
                std::vector<i32> ints(static_cast<sizet>(w) * h, 0);
                glGetTextureSubImage(textureId, static_cast<GLint>(mip), 0, 0, static_cast<GLint>(layer),
                                     static_cast<GLsizei>(w), static_cast<GLsizei>(h), 1,
                                     readFormat, GL_INT,
                                     static_cast<GLsizei>(ints.size() * sizeof(i32)), ints.data());
                bottomUp.reserve(ints.size());
                for (const i32 v : ints)
                    bottomUp.push_back(static_cast<f32>(v));
            }
            else
            {
                bottomUp.assign(static_cast<sizet>(w) * h, 0.0f);
                glGetTextureSubImage(textureId, static_cast<GLint>(mip), 0, 0, static_cast<GLint>(layer),
                                     static_cast<GLsizei>(w), static_cast<GLsizei>(h), 1,
                                     readFormat, GL_FLOAT,
                                     static_cast<GLsizei>(bottomUp.size() * sizeof(f32)), bottomUp.data());
            }

            if (prevPackPBO != 0)
                glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(prevPackPBO));
            glPixelStorei(GL_PACK_ALIGNMENT, prevPackAlignment);

            if (const GLenum error = glGetError(); error != GL_NO_ERROR)
            {
                outError = "glGetTextureSubImage failed (GL 0x" +
                           std::format("{:X}", static_cast<u32>(error)) + ")";
                return {};
            }

            // Flip rows to top-left order.
            std::vector<f32> topLeft(bottomUp.size());
            for (u32 row = 0; row < h; ++row)
            {
                const f32* src = bottomUp.data() + static_cast<sizet>(h - 1u - row) * w;
                f32* dst = topLeft.data() + static_cast<sizet>(row) * w;
                std::copy(src, src + w, dst);
            }
            outWidth = w;
            outHeight = h;
            return topLeft;
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
                    if (resource.TextureHandle.IsValid())
                        identity.GLTextureId = graph->ResolveTexture(resource.TextureHandle);
                    else if (resource.FramebufferHandle.IsValid())
                        identity.GLTextureId = ResolveTargetTexture(resource.Name);
                    if (resource.BufferHandle.IsValid())
                        identity.GLBufferId = graph->ResolveBuffer(resource.BufferHandle);
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
                    u32 textureA = 0;
                    u32 textureB = 0;
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
                                    textureA = snapshotResult.TextureID;
                                if (snapshotResult.ResourceName == compare.B)
                                    textureB = snapshotResult.TextureID;
                            }
                            snapshot.Disarm();
                        }
                    }
                    else
                    {
                        textureA = ResolveTargetTexture(compare.A);
                        textureB = ResolveTargetTexture(compare.B);
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
                        if (textureA == 0 || textureB == 0)
                        {
                            compareResult.Error = std::string("Unknown compare target '") +
                                                  (textureA == 0 ? compare.A : compare.B) +
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

                j["meta"] = CaptureStampJson(server.Context().GetFrameIndex ? server.Context().GetFrameIndex() : 0);
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
            j["debugTargetAvailable"] = registry.GetDebugColorTextureID() != 0;
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
                .Prop("debugTargetAvailable", Schema::Bool().Desc("True when the 'VirtualGeometryDebug' target has GPU backing this frame."));
        }

        ToolResult Handle_VirtualGeometrySet(McpServer& server, const Json& args)
        {
            const bool hasDebugMode = args.contains("debugMode") && args["debugMode"].is_string();
            const bool hasSwRasterMode = args.contains("swRasterMode") && args["swRasterMode"].is_string();
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

            f32 threshold = 0.0f;
            if (hasThreshold)
            {
                threshold = args["swRasterThresholdPixels"].get<f32>();
                if (!std::isfinite(threshold) || threshold < 0.0f || threshold > 4096.0f)
                    return ToolResult::Error("Invalid 'swRasterThresholdPixels': expected a finite number in [0, 4096].");
            }

            const bool anyChange =
                hasDebugMode || hasSwRasterMode || hasThreshold || hasForcePortable || hasEnabled || hasDebugToViewport;
            const bool forcePortable = hasForcePortable && args["forcePortableSwRaster"].get<bool>();
            const bool enabled = hasEnabled && args["enabled"].get<bool>();
            const bool debugToViewport = hasDebugToViewport && args["debugToViewport"].get<bool>();

            const Json applied = server.MarshalRead(
                [hasDebugMode, debugMode, hasSwRasterMode, swRasterMode, hasThreshold, threshold,
                 hasForcePortable, forcePortable, hasEnabled, enabled, hasDebugToViewport, debugToViewport]() -> Json
                {
                    auto& registry = VirtualMeshRegistry::Get();
                    Json previous = VirtualGeometrySettingsJson();
                    if (hasDebugMode)
                        ApplyVirtualDebugMode(debugMode); // shared with olo_render_set_debug_view's vg* modes
                    if (hasSwRasterMode)
                        registry.SetSwRasterMode(swRasterMode);
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
                if (Renderer3D::GetRendererSettings().Path != RenderingPath::Deferred)
                    j["note"] = "Virtual geometry only renders on the Deferred path; the scene does not submit "
                                "VirtualMeshComponents on Forward/Forward+, so every counter reads zero.";
                else if (registry.GetFrameInstances().empty())
                    j["note"] = "No virtual-mesh instances were submitted this frame (no VirtualMeshComponent in "
                                "the scene, or all of them are disabled).";
                return j; });
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
            Json textures;
            textures["albedo"] = data.albedoMapID;
            textures["metallicRoughness"] = data.metallicRoughnessMapID;
            textures["normal"] = data.normalMapID;
            textures["ao"] = data.aoMapID;
            textures["emissive"] = data.emissiveMapID;

            Json useMaps;
            useMaps["useAlbedoMap"] = data.albedoMapID != 0;
            useMaps["useMetallicRoughnessMap"] = data.metallicRoughnessMapID != 0;
            useMaps["useNormalMap"] = data.normalMapID != 0;
            useMaps["useAOMap"] = data.aoMapID != 0;
            useMaps["useEmissiveMap"] = data.emissiveMapID != 0;

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
                static const Ref<Material> s_EngineDefault =
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
                        ResolveSubmeshMaterial(overrideMaterial, meshSource.get(), index, *s_EngineDefault);
                    const Material* material = &resolved;
                    std::string_view source = "engine default material";
                    if (material == overrideMaterial)
                    {
                        source = "MaterialComponent (override)";
                    }
                    else if (material != s_EngineDefault.get())
                    {
                        source = "MeshSource imported material (per-submesh)";
                    }

                    const PODMaterialData data = Renderer3D::CreatePODMaterialDataForMaterial(*material, 0);
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
            const Json result = server.MarshalRead([]() -> Json
                                                   {
                const LightGrid& lightGrid = Renderer3D::GetForwardPlus().GetLightGrid();
                if (!lightGrid.IsInitialized())
                    return Json{ { "__error", "The clustered light grid is not initialized (no frame rendered yet)." } };

                ClusterGrid::GridDims dims;
                dims.CountX = lightGrid.GetClusterCountX();
                dims.CountY = lightGrid.GetClusterCountY();
                dims.CountZ = lightGrid.GetClusterCountZ();
                dims.MaxLightsPerCluster = lightGrid.GetMaxLightsPerCluster();

                // Two u32 per cluster: (offset, count), in LightCulling.comp's
                // clusterIndex = (z * countY + y) * countX + x order.
                const u32 totalClusters = dims.TotalClusters();
                std::vector<u32> gridPairs(static_cast<std::size_t>(totalClusters) * 2u, 0u);
                const auto gridBytes = static_cast<u32>(gridPairs.size() * sizeof(u32));
                if (!ReadStorageBufferStaged(lightGrid.GetLightGridSSBO(), gridBytes, gridPairs.data()))
                    return Json{ { "__error", "Failed to read back the light-grid SSBO." } };

                u32 globalIndexCount = 0;
                (void)ReadStorageBufferStaged(lightGrid.GetGlobalIndexSSBO(), static_cast<u32>(sizeof(u32)),
                                              &globalIndexCount);

                const u32 lightIndexCapacity = lightGrid.GetLightIndexSSBO()
                                                   ? lightGrid.GetLightIndexSSBO()->GetSize() / static_cast<u32>(sizeof(u32))
                                                   : 0u;

                const ClusterGrid::Stats stats = ClusterGrid::Compute(std::span<const u32>(gridPairs), dims);
                Json j = ClusterGrid::ToJson(stats, dims, globalIndexCount, lightIndexCapacity);
                j["renderingPath"] = RenderingPathName(Renderer3D::GetRendererSettings().Path);
                j["screen"] = Json{ { "width", lightGrid.GetScreenWidth() },
                                    { "height", lightGrid.GetScreenHeight() } };
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

        // ---- olo_froxel_fog_probe (main-marshaled; 1x1x1 GL readback) ----------
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

        // (main thread) Read one texel out of a 3D volume with glGetTextureSubImage
        // over a 1x1x1 region — NEVER the whole 160x90x64 RGBA16F volume (that is
        // 7 MB per read, per volume).
        FroxelFog::VolumeSample ProbeVolumeTexel(u32 textureId, const char* label, i32 x, i32 y, i32 z)
        {
            FroxelFog::VolumeSample sample;
            if (textureId == 0 || glIsTexture(textureId) == GL_FALSE)
            {
                sample.Unavailable = std::string(label) + " volume does not exist this frame.";
                return sample;
            }

            GLint prevPackAlignment = 4;
            glGetIntegerv(GL_PACK_ALIGNMENT, &prevPackAlignment);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            GLint prevPackPBO = 0;
            glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prevPackPBO);
            if (prevPackPBO != 0)
                glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

            std::array<f32, 4> texel{ 0.0f, 0.0f, 0.0f, 0.0f };
            glGetTextureSubImage(textureId, 0, x, y, z, 1, 1, 1, GL_RGBA, GL_FLOAT,
                                 static_cast<GLsizei>(texel.size() * sizeof(f32)), texel.data());

            if (prevPackPBO != 0)
                glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(prevPackPBO));
            glPixelStorei(GL_PACK_ALIGNMENT, prevPackAlignment);

            if (const GLenum error = glGetError(); error != GL_NO_ERROR)
            {
                sample.Unavailable = std::string("glGetTextureSubImage on the ") + label + " volume failed (GL 0x" +
                                     std::format("{:X}", static_cast<u32>(error)) + ").";
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

                probe.Raw = ProbeVolumeTexel(state.ScatterTextureID, "scatter", probe.Coord.IX, probe.Coord.IY,
                                             probe.Coord.IZ);
                probe.Integrated = ProbeVolumeTexel(state.IntegratedTextureID, "integrated", probe.Coord.IX,
                                                    probe.Coord.IY, probe.Coord.IZ);

                Json j = FroxelFog::ToJson(probe);
                j["fog"] = Json{ { "enabled", fog.Enabled },
                                 { "volumetric", fog.EnableVolumetric },
                                 { "ranThisFrame", pass->RanThisFrame() } };
                j["meta"] = CaptureStampJson(server.Context().GetFrameIndex ? server.Context().GetFrameIndex() : 0);
                if (!pass->RanThisFrame())
                    j["staleness"] = "The froxel chain did NOT run on the last frame (fog or volumetric fog was "
                                     "turned off); these are the values from the last frame it did run.";
                return j; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());
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
            tool.Name = "olo_render_graph_topology_export";
            tool.Toolset = "render";
            tool.Title = "Export render graph topology";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Export the live render graph's topology as structured data for reasoning about the render "
                "pipeline — the passes, their topologically-sorted execution order, the pass-to-pass "
                "dependency edges, and every registered resource (texture/framebuffer/buffer) with the passes "
                "that produce and consume it. Each pass reports its work type (Graphics/Compute/Copy), whether "
                "it declares resources, whether it is an async-compute candidate, whether it was culled "
                "(unreachable from the final pass this frame), whether it is the final/output pass, and its "
                "'accesses' — every resource it reads/writes WITH the resolved physical GL object id, so 'do "
                "these two passes touch the same physical texture this frame' is a single lookup (each "
                "resource also carries a 'gl' block: texture/framebuffer/attachment/buffer ids as of the last "
                "executed frame; texture views resolve to their parent object). Use format:\"mermaid\" for a "
                "flowchart DAG of the pass graph instead of JSON. Read-only; requires the editor to be "
                "rendering in 3D mode.";
            tool.InputSchema = Schema::Object()
                                   .Prop("format", Schema::String()
                                                       .Enum({ "json", "mermaid" })
                                                       .Desc("'json' (default): full structured topology (passes, executionOrder, edges, resources). "
                                                             "'mermaid': a flowchart-LR DAG of the pass dependency graph."))
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
                                                                                                          .Prop("glTextureId", Schema::Int().Desc("Resolved physical texture id; omitted when unbacked."))
                                                                                                          .Prop("glBufferId", Schema::Int().Desc("Omitted when not buffer-backed.")))
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
                                                                         .Prop("gl", Schema::Object().Desc("Resolved physical GL object ids as of the last executed frame (textureId, framebufferId, colorAttachmentIds, depthAttachmentId, bufferId, viewOfParentLayer); omitted when unbacked."))))
                                    .Prop("note", Schema::String())
                                    .Required({ "finalPass", "passCount", "passes", "executionOrder", "edgeCount", "edges", "resourceCount", "resources", "note" });
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
                "sampled). Pass names come from olo_render_graph_topology_export's executionOrder.";
            tool.InputSchema = Schema::Object()
                                   .Prop("name", Schema::String().Desc("Render-graph resource name (see olo_render_list_targets)."))
                                   .Prop("mip", Schema::Int().Min(0).Max(16).Desc("Mip level to capture (default 0)."))
                                   .Prop("layer", Schema::Int().Min(0).Max(64).Desc("Texture-array layer (e.g. CSM cascade 0..3), cubemap face (0..5 = +X,-X,+Y,-Y,+Z,-Z), or 3D-volume z-slice (e.g. the froxel fog volumes). Default 0, or the resource's own layer when it is a per-layer view. Out of range is an error."))
                                   .Prop("face", Schema::Int().Min(0).Max(64).Desc("Alias of 'layer' (the original spelling); give only one."))
                                   .Prop("normalize", Schema::Bool().Desc("Min-max normalise float values to [0,1] before encoding (default: true for depth, false otherwise)."))
                                   .Prop("maxWidth", Schema::Int().Min(16).Max(4096).Desc("Max output width in pixels (default 1024); aspect ratio preserved."))
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
                                    .Prop("name", Schema::String().Desc("Captured render-graph resource name."))
                                    .Prop("afterPass", Schema::String().Desc("Mid-frame snapshot pass; omitted unless 'afterPass' was given."))
                                    .Prop("snapshotSourceTextureId", Schema::Int().Desc("Source texture of the afterPass snapshot clone; omitted without 'afterPass'."))
                                    .Prop("frameIndexNote", Schema::String().Desc("afterPass frameIndex semantics; omitted without 'afterPass'."))
                                    .Prop("layer", Schema::Int().Min(0).Desc("GL array layer / cube face / z-slice actually read."))
                                    .Prop("layers", Schema::Int().Desc("Layer count; array/cube/3D targets only."))
                                    .Prop("layerNote", Schema::String().Desc("Layer-selection note; omitted when there is nothing to flag."))
                                    .Prop("width", Schema::Int().Desc("Output PNG width."))
                                    .Prop("height", Schema::Int().Desc("Output PNG height."))
                                    .Prop("sourceWidth", Schema::Int())
                                    .Prop("sourceHeight", Schema::Int())
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
                "OLOENGINE_GOLDEN_REBASE workflow. The verdict uses the same RMSE→SSIM metric as the "
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
            // A project-WRITE tool (#306 item C): it mutates the session-global
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
                "shadowed scenes; A/B it in one call instead of editing shader source). Call with NO arguments to list "
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
                                    .Prop("requested", Schema::String().Desc("Apply shape only: 'auto' when depthprepass auto was requested; omitted otherwise."));
            tool.MainMarshaled = true;
            tool.Handler = Handle_RendererSettingsSet;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_render_why_not_visible";
            tool.Toolset = "render";
            tool.Title = "Explain entity not visible";
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Explain why an entity is NOT visible on screen — the rendering counterpart of "
                "olo_physics_why_no_collision ('why can't I see my mesh?'). Given an entity UUID, it checks, in "
                "order: a scene is loaded, the entity exists, it has a renderable component (Mesh/Model/Sprite/"
                "Circle/Text/InstancedMesh/...), its geometry asset is present, its visibility flag is on, its "
                "transform scale is non-degenerate, its material's shader compiled, and (against the editor "
                "camera) it is in front of the camera and inside the view frustum. Returns the root-cause "
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
                                                                    "component_hidden", "degenerate_scale", "shader_compile_error",
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
            // A 1x1 GL readback; changes no camera / setting / scene state.
            tool.Annotations = ReadOnlyAnnotations();
            tool.Description =
                "Read back the exact NUMBERS under one viewport pixel — the numeric counterpart of "
                "olo_render_capture_target, and the fastest way to diagnose a shading bug. Given "
                "viewport pixel coordinates (top-left origin, same as olo_screenshot), it decodes every "
                "G-Buffer channel for that pixel: albedo.rgb, metallic, the world-space NORMAL (the "
                "octahedral RT1.xy pair decoded exactly as the shaders do), roughness, ao, emissive.rgb, "
                "the screen-space velocity vector, the integer entityID, the raw device depth PLUS its "
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
                "(mid-frame snapshot) instead of end-of-frame. space/mip/layer/afterPass require "
                "'target'.";
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
                                                      .Prop("timestampMs", Schema::Int().Min(0)))
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
                "every resource's RESOLVED physical GL id — flagging resources that are consumed but "
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
                                    .Prop("consumedButUnbacked", Schema::Array(Schema::String()).Desc("Resources consumed but resolving to no GL backing."))
                                    .Prop("versionGroups", Schema::Array(Schema::Object()
                                                                             .Prop("baseName", Schema::String())
                                                                             .Prop("versions", Schema::Array(Schema::Object()
                                                                                                                 .Prop("name", Schema::String())
                                                                                                                 .Prop("glTextureId", Schema::Int().Desc("Omitted when unbacked."))
                                                                                                                 .Prop("glBufferId", Schema::Int().Desc("Omitted when not buffer-backed."))
                                                                                                                 .Prop("lastWriter", Schema::String().Desc("Omitted when unknown."))))
                                                                             .Prop("multiplePhysicalIds", Schema::Bool()))
                                                               .Desc("Versioned names (Base@Pass) grouped with their physical ids; single-version groups are dropped."))
                                    .Prop("compare", Schema::Object().Desc("Only when 'compare' was requested: 'a'/'b' echoes plus either 'error' or the bit-exact result (comparedRegion/comparedTexels/bitwiseEqual/differingTexels/maxAbsDiff?/firstDiffs/note; firstDiffs a/b encode non-finite floats as the strings 'NaN'/'Inf')."))
                                    .Prop("meta", Schema::Object()
                                                      .Prop("frameIndex", Schema::Int().Min(0))
                                                      .Prop("timestampMs", Schema::Int().Min(0)))
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
                "driver with 64-bit atomics. 'enabled' is the MASTER SWITCH: turning it off draws every "
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
                "evictions climbing every frame under a tight budget). Zero everywhere means no "
                "VirtualMeshComponent was submitted — virtual geometry renders on the DEFERRED path only.";
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
                                                      .Prop("drawnClusters", Schema::Int().Min(0)))
                                    .Prop("residency", Schema::Object()
                                                           .Prop("totalPages", Schema::Int().Min(0))
                                                           .Prop("residentPages", Schema::Int().Min(0))
                                                           .Prop("pinnedPages", Schema::Int().Min(0))
                                                           .Prop("budgetSlots", Schema::Int().Min(0))
                                                           .Prop("budget", Schema::String().Enum({ "unbounded (eager)", "budgeted" }))
                                                           .Prop("pageUploads", Schema::Int().Min(0))
                                                           .Prop("pageEvictions", Schema::Int().Min(0)))
                                    .Prop("settings", VirtualGeometrySettingsSchema().Desc("Live knob state (same shape as olo_virtual_geometry_set's previous/current)."))
                                    .Prop("note", Schema::String().Desc("Non-Deferred-path / no-instances caveat; omitted otherwise."))
                                    .Required({ "renderingPath", "frameInstances", "frameClusters", "cull", "residency", "settings" });
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
                                                                         .Prop("textureIds", Schema::Object().Desc("Bound GL texture id per slot (albedo/metallicRoughness/normal/ao/emissive; 0 = none)."))))
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
                "exceeded the per-cluster budget. The plain Forward path does not run the cull.";
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
                                    .Prop("note", Schema::String().Desc("Plain-Forward staleness caveat; omitted otherwise."))
                                    .Required({ "grid", "clustersSampled", "totalAssignedIndices", "emptyClusters", "overflowClusters",
                                                "maxLightsInAnyCluster", "meanLightsPerCluster", "meanLightsPerNonEmptyCluster",
                                                "busiestCluster", "perSlice", "histogram", "lightIndexList", "renderingPath", "screen" });
            tool.MainMarshaled = true;
            tool.Handler = Handle_ClusterGridStats;
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
                                                      .Prop("timestampMs", Schema::Int().Min(0)))
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
    }
} // namespace OloEngine::MCP

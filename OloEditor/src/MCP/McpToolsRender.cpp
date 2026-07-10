#include "OloEnginePCH.h"
#include "MCP/McpToolsCommon.h"
#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpFrameBreakdown.h"
#include "MCP/McpGoldenCompare.h"
#include "MCP/McpRenderExplain.h"
#include "MCP/McpRenderGraphTopology.h"
#include "MCP/McpRenderOverrides.h"
#include "MCP/McpRendererSettings.h"
#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Core/UUID.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Debug/CapturedFrameData.h"
#include "OloEngine/Renderer/Debug/CommandPacketDebugger.h"
#include "OloEngine/Renderer/Debug/FrameCaptureManager.h"
#include "OloEngine/Renderer/Debug/GPUResourceInspector.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugRuntime.h"
#include "OloEngine/Renderer/Debug/ShaderDebugger.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Renderer/Passes/CommandBufferRenderPass.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/Renderer2D.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ResourceHandle.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"

#include <stb_image/stb_image.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Rendering MCP tools: frame breakdown, render-target listing/capture, the
// render-graph topology export, the ephemeral override A/B tools (toggle pass,
// debug view, renderer settings, sun/time-of-day), golden-image comparison, and
// the olo_render_why_not_visible explainer. Split out of the McpTools.cpp
// monolith (issue #357).

namespace OloEngine::MCP
{
    namespace
    {
        // Defined further down (next to the topology export handler that shares it);
        // forward-declared so the frame-breakdown handler can reuse the same
        // enum -> string mapping the topology export uses.
        const char* PassWorkTypeName(RenderGraphPassWorkType type);

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
            return ToolResult::Text(o.dump(2));
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
            return ToolResult::Text(result.dump(2));
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
            return ToolResult::Text(transport.dump(2));
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
            u32 faceOrLayer = 0;
            if (args.contains("face") && args["face"].is_number_integer())
                faceOrLayer = static_cast<u32>(std::clamp<long long>(args["face"].get<long long>(), 0, 64));
            int maxWidth = 1024;
            if (args.contains("maxWidth") && args["maxWidth"].is_number_integer())
                maxWidth = static_cast<int>(std::clamp<long long>(args["maxWidth"].get<long long>(), 16, 4096));

            auto normalizeMode = GPUResourceInspector::CaptureNormalizeMode::Auto;
            if (args.contains("normalize") && args["normalize"].is_boolean())
                normalizeMode = args["normalize"].get<bool>() ? GPUResourceInspector::CaptureNormalizeMode::On
                                                              : GPUResourceInspector::CaptureNormalizeMode::Off;

            Json result = server.MarshalRead([name, mipLevel, faceOrLayer, normalizeMode, maxWidth]() -> Json
                                             {
                const Ref<RenderGraph>& graph = RenderGraphDebugRuntime::GetActiveGraph();
                if (!graph)
                    return Json{ { "__error", "No active render graph (the editor is not in 3D mode, or no frame has been rendered yet)." } };

                // Resolve the name to a physical GL texture: first as a graph
                // texture (covers attachment views like SceneDepth / GBufferAlbedo
                // and imported textures like ShadowMapCSM), then as a framebuffer
                // (capture colour attachment 0, or the depth attachment for
                // depth-only targets).
                u32 textureId = Renderer3D::ResolveFrameGraphTexture(name);
                if (textureId == 0)
                {
                    if (const Ref<Framebuffer> framebuffer = Renderer3D::ResolveFrameGraphFramebuffer(name); framebuffer)
                    {
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
                        textureId = hasColor ? framebuffer->GetColorAttachmentRendererID(0)
                                             : framebuffer->GetDepthAttachmentRendererID();
                    }
                }
                if (textureId == 0)
                    return Json{ { "__error", "Unknown render-graph resource '" + name +
                                                  "' (or it has no GPU backing this frame). Call olo_render_list_targets for the live list." } };

                auto capture = GPUResourceInspector::CaptureTexturePng(textureId, mipLevel, faceOrLayer, normalizeMode, maxWidth);
                if (!capture.Error.empty())
                    return Json{ { "__error", "Capture of '" + name + "' failed: " + capture.Error } };

                Json meta;
                meta["name"] = name;
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
                return Json{ { "meta", std::move(meta) },
                             { "b64", Base64Encode(capture.PngBytes) } }; });

            if (result.is_object() && result.contains("__error"))
                return ToolResult::Error(result["__error"].get<std::string>());

            ToolResult toolResult;
            toolResult.Content = Json::array({ Json{ { "type", "text" }, { "text", result["meta"].dump(2) } },
                                               Json{ { "type", "image" },
                                                     { "data", result["b64"] },
                                                     { "mimeType", "image/png" } } });
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
                return ToolResult::Text(result.dump(2));
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
            return ToolResult::Text(result.dump(2));
        }

        // Which debug view (if any) is currently on. Our tool keeps these mutually
        // exclusive, but the editor panel can set several; report the first.
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
            const bool deferred = Renderer3D::GetRendererSettings().Path == RenderingPath::Deferred;
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
                return ToolResult::Text(result.dump(2));
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

            const Json result = server.MarshalRead([view]() -> Json
                                                   {
                PostProcessSettings& pp = Renderer3D::GetPostProcessSettings();
                // Exactly one debug view active at a time (or none).
                pp.SSAODebugView = (view == DebugView::SSAO);
                pp.GTAODebugView = (view == DebugView::GTAO);
                pp.SSRDebugView = (view == DebugView::SSR);
                pp.SSGIDebugView = (view == DebugView::SSGI);
                pp.OverdrawDebugView = (view == DebugView::Overdraw);
                return ToJson(BuildDebugViewResult(pp, view)); });
            return ToolResult::Text(result.dump(2));
        }

        // ---- olo_scene_set_time_of_day / olo_scene_set_sun_angle (#316 Part 4) -
        // Ephemeral sun-direction override for lighting iteration. Like the
        // toggle/debug-view tools above, these edit ONLY a session-global renderer
        // override (Renderer3D::SetSunDirectionOverride), never the
        // ProceduralSkyComponent's serialized m_SunDirection — so the moved sun is
        // visible next frame, never saved, and resets on scene reload / play-stop /
        // server-stop / explicit clear. The sun-direction math + result shaping is
        // the pure RenderOverrides module; the handler does the renderer/scene-bound
        // work on the main thread inside MarshalRead.

        // (main thread) Finish a sun-override result: annotate it from the active
        // scene (the override only affects a procedural-sky bake) and the resulting
        // sun elevation (below the horizon bakes a dark night sky), then shape JSON.
        Json FinalizeSunOverride(McpServer& server, RenderOverrides::SunOverrideResult& r)
        {
            bool skyPresent = false;
            const Ref<Scene> scene = server.Context().GetActiveScene
                                         ? server.Context().GetActiveScene()
                                         : nullptr;
            if (scene)
            {
                auto view = scene->GetAllEntitiesWith<ProceduralSkyComponent>();
                skyPresent = view.begin() != view.end();
            }

            if (r.Active && !skyPresent)
                r.Note = "No ProceduralSkyComponent in the active scene, so this sun override has no visible "
                         "effect yet. Add a Procedural Sky to the scene to iterate time-of-day lighting.";
            else if (r.Active && RenderOverrides::SunElevationDegrees(r.Direction) < 0.0)
                r.Note = "The sun is below the horizon at this setting, so the sky bakes dark (night). Move "
                         "toward noon (hours ~12) or a positive pitch for daylight.";
            else
            {
                // Override inactive, or active with the sun visibly above the
                // horizon — nothing to warn about.
            }

            return RenderOverrides::ToJson(r);
        }

        // (main thread) Clear the ephemeral sun override and record the outcome —
        // the clear branch shared verbatim by both sun-override handlers.
        void ApplySunClear(RenderOverrides::SunOverrideResult& r)
        {
            r.Cleared = Renderer3D::HasSunDirectionOverride();
            Renderer3D::ClearSunDirectionOverride();
            r.Active = false;
            r.Source = "cleared";
        }

        // (main thread) Read the current override state into the result — the
        // no-argument introspection branch shared by both sun-override handlers.
        void ApplySunIntrospect(RenderOverrides::SunOverrideResult& r)
        {
            r.Active = Renderer3D::HasSunDirectionOverride();
            if (r.Active)
            {
                const glm::vec3& d = Renderer3D::GetSunDirectionOverride();
                r.Direction = RenderOverrides::SunVec3{ d.x, d.y, d.z };
            }
            r.Source = "current";
        }

        ToolResult Handle_SceneSetTimeOfDay(McpServer& server, const Json& args)
        {
            using namespace RenderOverrides;

            const bool wantClear = args.contains("clear") && args["clear"].is_boolean() && args["clear"].get<bool>();
            const bool hasHours = !wantClear && args.contains("hours") && args["hours"].is_number();
            double hours = 0.0;
            if (hasHours)
            {
                hours = args["hours"].get<double>();
                if (!std::isfinite(hours) || hours < 0.0 || hours > 24.0)
                    return ToolResult::Error("Invalid 'hours': expected a finite number in [0, 24] "
                                             "(0 = midnight, 6 = sunrise, 12 = noon, 18 = sunset).");
            }

            const Json result = server.MarshalRead([&server, wantClear, hasHours, hours]() -> Json
                                                   {
                SunOverrideResult r;
                if (wantClear)
                {
                    ApplySunClear(r);
                }
                else if (hasHours)
                {
                    const SunVec3 dir = SunDirectionFromTimeOfDay(hours);
                    Renderer3D::SetSunDirectionOverride(glm::vec3(
                        static_cast<f32>(dir.X), static_cast<f32>(dir.Y), static_cast<f32>(dir.Z)));
                    r.Active = true;
                    r.Direction = dir;
                    r.HasHours = true;
                    r.Hours = hours;
                    r.Source = "timeOfDay";
                }
                else
                {
                    ApplySunIntrospect(r);
                }
                return FinalizeSunOverride(server, r); });
            return ToolResult::Text(result.dump(2));
        }

        ToolResult Handle_SceneSetSunAngle(McpServer& server, const Json& args)
        {
            using namespace RenderOverrides;

            const bool wantClear = args.contains("clear") && args["clear"].is_boolean() && args["clear"].get<bool>();
            const bool hasYaw = !wantClear && args.contains("yaw") && args["yaw"].is_number();
            const bool hasPitch = !wantClear && args.contains("pitch") && args["pitch"].is_number();

            // A set needs BOTH angles — a half-specified direction is ambiguous, so
            // reject it with guidance rather than silently using a default.
            if (!wantClear && (hasYaw != hasPitch))
                return ToolResult::Error("olo_scene_set_sun_angle needs both 'yaw' (azimuth, degrees) and "
                                         "'pitch' (elevation, degrees). Provide both, pass 'clear':true to "
                                         "remove the override, or call with no arguments to read the current "
                                         "state.");

            const bool doSet = !wantClear && hasYaw && hasPitch;
            double yaw = 0.0;
            double pitch = 0.0;
            if (doSet)
            {
                yaw = args["yaw"].get<double>();
                pitch = args["pitch"].get<double>();
                if (!std::isfinite(yaw) || !std::isfinite(pitch))
                    return ToolResult::Error("Invalid 'yaw'/'pitch': expected finite numbers in degrees.");
                if (pitch < -90.0 || pitch > 90.0)
                    return ToolResult::Error("Invalid 'pitch': expected an elevation in [-90, 90] degrees "
                                             "(90 = straight up, 0 = horizon, negative = below the horizon).");
            }

            const Json result = server.MarshalRead([&server, wantClear, doSet, yaw, pitch]() -> Json
                                                   {
                SunOverrideResult r;
                if (wantClear)
                {
                    ApplySunClear(r);
                }
                else if (doSet)
                {
                    const SunVec3 dir = SunDirectionFromAngles(yaw, pitch);
                    Renderer3D::SetSunDirectionOverride(glm::vec3(
                        static_cast<f32>(dir.X), static_cast<f32>(dir.Y), static_cast<f32>(dir.Z)));
                    r.Active = true;
                    r.Direction = dir;
                    r.Source = "sunAngle";
                }
                else
                {
                    ApplySunIntrospect(r);
                }
                return FinalizeSunOverride(server, r); });
            return ToolResult::Text(result.dump(2));
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
                result.Content = Json::array({ Json{ { "type", "text" }, { "text", j.dump(2) } },
                                               Json{ { "type", "image" }, { "data", Base64Encode(capturedPng) }, { "mimeType", "image/png" } } });
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
            result.Content = Json::array({ Json{ { "type", "text" }, { "text", j.dump(2) } },
                                           Json{ { "type", "image" }, { "data", Base64Encode(capturedPng) }, { "mimeType", "image/png" } } });
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
                return ToolResult::Text(result.dump(2));
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

            return ToolResult::Text(result.dump(2));
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

            return ToolResult::Text(result.dump(2));
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
                "(unreachable from the final pass this frame), and whether it is the final/output pass. Use "
                "format:\"mermaid\" for a flowchart DAG of the pass graph instead of JSON. Read-only; requires "
                "the editor to be rendering in 3D mode.";
            tool.InputSchema = Schema::Object()
                                   .Prop("format", Schema::String()
                                                       .Enum({ "json", "mermaid" })
                                                       .Desc("'json' (default): full structured topology (passes, executionOrder, edges, resources). "
                                                             "'mermaid': a flowchart-LR DAG of the pass dependency graph."))
                                   .NoAdditional();
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
                "(format, size, value range) plus the image.";
            tool.InputSchema = Schema::Object()
                                   .Prop("name", Schema::String().Desc("Render-graph resource name (see olo_render_list_targets)."))
                                   .Prop("mip", Schema::Int().Min(0).Max(16).Desc("Mip level to capture (default 0)."))
                                   .Prop("face", Schema::Int().Min(0).Max(64).Desc("Cubemap face (0..5 = +X,-X,+Y,-Y,+Z,-Z) or texture-array layer (default 0)."))
                                   .Prop("normalize", Schema::Bool().Desc("Min-max normalise float values to [0,1] before encoding (default: true for depth, false otherwise)."))
                                   .Prop("maxWidth", Schema::Int().Min(16).Max(4096).Desc("Max output width in pixels (default 1024); aspect ratio preserved."))
                                   .Required({ "name" })
                                   .NoAdditional();
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
                "Switch the viewport to a raw intermediate buffer for AO/reflection/GI/overdraw debugging. "
                "'mode' is one of none (the normal composite), ssao, gtao, ssr, ssgi, overdraw — exactly one "
                "is shown at a time; mode 'none' (or 'enabled':false) clears them all. 'overdraw' heat-maps "
                "per-pixel fragment count (how many layers deep the frame is: black=none, blue/green/yellow/"
                "red=increasing overlap) by re-drawing opaque geometry with depth test off + additive blend; "
                "it needs no backing pass and works on every rendering path. Returns the active mode, the "
                "*DebugView flag states, and 'passEnabled' — whether the pass that produces the chosen "
                "buffer is actually running this frame (with an actionable 'note' if not, e.g. enable SSAO "
                "first with olo_render_toggle_pass). The change is EPHEMERAL: it edits the renderer's "
                "session-global settings, not the scene, so it is never saved and a scene reload restores "
                "it. Call with no arguments to list the modes + current state.";
            tool.InputSchema = Schema::Object()
                                   .Prop("mode", Schema::String().Enum({ "none", "ssao", "gtao", "ssr", "ssgi", "overdraw" }).Desc("Debug view to show. 'none' clears all. Omit to list modes + state."))
                                   .Prop("enabled", Schema::Bool().Desc("Set false as an alias for mode:'none' (clear all debug views)."))
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_RenderSetDebugView;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_scene_set_time_of_day";
            tool.Toolset = "render";
            tool.Title = "Set time of day (sun)";
            // Edits an ephemeral session render override; setting the same hours
            // twice yields the same sun (idempotent), destroys nothing.
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Move the procedural sky's sun to a time of day for lighting iteration — the lighting inner "
                "loop: set the hour, olo_screenshot, set another, compare. 'hours' is a 24-hour clock time "
                "in [0,24] (0 = midnight, 6 = sunrise, 12 = noon/overhead, 18 = sunset); the sun rises in "
                "the east, peaks overhead at noon, and sets in the west, dropping below the horizon at night. "
                "Returns the resulting toward-sun direction with its elevation/azimuth (a 'note' warns when "
                "the scene has no Procedural Sky to affect, or when the sun is below the horizon). The change "
                "is EPHEMERAL: it edits a session-global renderer override, NOT the ProceduralSkyComponent, "
                "so it is never saved and resets on scene reload, play-stop, server-stop, or 'clear':true. "
                "Pass 'clear':true to restore the authored sun; call with no arguments to read the current "
                "override state.";
            tool.InputSchema = Schema::Object()
                                   .Prop("hours", Schema::Number().Min(0).Max(24).Desc("Time of day on a 24-hour clock (0=midnight, 6=sunrise, 12=noon, 18=sunset). Omit to read current state."))
                                   .Prop("clear", Schema::Bool().Desc("Set true to remove the override and restore the scene's authored sun direction."))
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_SceneSetTimeOfDay;
            server.RegisterTool(std::move(tool));
        }

        {
            ToolDef tool;
            tool.Name = "olo_scene_set_sun_angle";
            tool.Toolset = "render";
            tool.Title = "Set sun angle (yaw/pitch)";
            // Edits an ephemeral session render override; same angles -> same sun
            // (idempotent), destroys nothing.
            tool.Annotations = MutatingAnnotations(/*idempotent*/ true);
            tool.Description =
                "Aim the procedural sky's sun directly from a yaw/pitch pair — the precise sibling of "
                "olo_scene_set_time_of_day for lighting iteration. 'yaw' is the azimuth in degrees (measured "
                "from +Z toward +X: 0=+Z, 90=+X/east, 270=-X/west) and 'pitch' is the elevation in degrees "
                "in [-90,90] (90=straight up, 0=horizon, negative=below horizon); both are required to set. "
                "Returns the resulting toward-sun direction with its elevation/azimuth (a 'note' warns when "
                "the scene has no Procedural Sky to affect, or when the sun is below the horizon). The change "
                "is EPHEMERAL: it edits a session-global renderer override, NOT the ProceduralSkyComponent, "
                "so it is never saved and resets on scene reload, play-stop, server-stop, or 'clear':true. "
                "Pass 'clear':true to restore the authored sun; call with no arguments to read the current "
                "override state.";
            tool.InputSchema = Schema::Object()
                                   .Prop("yaw", Schema::Number().Desc("Azimuth in degrees (0=+Z, 90=+X/east, 180=-Z, 270=-X/west). Required together with 'pitch' to set."))
                                   .Prop("pitch", Schema::Number().Min(-90).Max(90).Desc("Elevation in degrees above the horizon (90=up, 0=horizon, negative=below). Required together with 'yaw' to set."))
                                   .Prop("clear", Schema::Bool().Desc("Set true to remove the override and restore the scene's authored sun direction."))
                                   .NoAdditional();
            tool.MainMarshaled = true;
            tool.Handler = Handle_SceneSetSunAngle;
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
                                   .Required({ "goldenPath" })
                                   .NoAdditional();
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
            tool.MainMarshaled = true;
            tool.Handler = Handle_RenderWhyNotVisible;
            server.RegisterTool(std::move(tool));
        }
    }
} // namespace OloEngine::MCP

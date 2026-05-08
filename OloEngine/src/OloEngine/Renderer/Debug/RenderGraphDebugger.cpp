#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugger.h"
#include "OloEngine/Renderer/Passes/RenderPass.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <queue>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace OloEngine
{
    namespace Utils
    {
        // Helper function to convert FramebufferTextureFormat to string
        std::string FormatToString(FramebufferTextureFormat format)
        {
            switch (format)
            {
                case FramebufferTextureFormat::None:
                    return "None";
                case FramebufferTextureFormat::RGBA8:
                    return "RGBA8";
                case FramebufferTextureFormat::RED_INTEGER:
                    return "RED_INTEGER";
                case FramebufferTextureFormat::DEPTH24STENCIL8:
                    return "Depth24Stencil8";
                default:
                    return "Unknown";
            }
        }
    } // namespace Utils

    namespace
    {
        std::unordered_set<std::string> BuildCulledPassSet(const Ref<RenderGraph>& graph)
        {
            std::unordered_set<std::string> culled;
            if (!graph)
                return culled;

            const auto& culledPasses = graph->GetCulledPasses();
            culled.reserve(culledPasses.size());
            for (const auto& passName : culledPasses)
            {
                culled.insert(passName);
            }
            return culled;
        }

        std::vector<Ref<RenderPass>> GetVisiblePasses(const Ref<RenderGraph>& graph)
        {
            std::vector<Ref<RenderPass>> visible;
            if (!graph)
                return visible;

            const auto allEntries = graph->GetNodeSubmissionInfo();
            std::unordered_map<std::string, Ref<RenderPass>> passByName;
            passByName.reserve(allEntries.size());
            for (const auto& entry : allEntries)
            {
                if (const auto pass = graph->GetNode<RenderPass>(entry.NodeName))
                {
                    passByName.emplace(entry.NodeName, pass);
                }
            }

            const auto culled = BuildCulledPassSet(graph);
            std::unordered_set<std::string> appended;
            appended.reserve(passByName.size());

            const auto appendIfVisible = [&](const std::string& passName)
            {
                if (culled.contains(passName) || appended.contains(passName))
                    return;

                if (const auto passIt = passByName.find(passName); passIt != passByName.end() && passIt->second)
                {
                    visible.push_back(passIt->second);
                    appended.insert(passName);
                }
            };

            for (const auto& passName : graph->GetExecutionOrder())
            {
                appendIfVisible(passName);
            }

            // Before the first frame graph build, GetExecutionOrder() can be empty.
            // Fall back to all registered wrapped pass nodes so the debugger still has a view.
            for (const auto& entry : allEntries)
            {
                appendIfVisible(entry.NodeName);
            }

            return visible;
        }

        std::unordered_set<std::string> BuildVisiblePassNameSet(const std::vector<Ref<RenderPass>>& passes)
        {
            std::unordered_set<std::string> names;
            names.reserve(passes.size());
            for (const auto& pass : passes)
            {
                if (pass)
                    names.insert(pass->GetName());
            }
            return names;
        }

        std::string BuildVisiblePassDigest(const std::vector<Ref<RenderPass>>& passes)
        {
            std::string digest;
            for (const auto& pass : passes)
            {
                if (!pass)
                    continue;
                if (!digest.empty())
                    digest += '|';
                digest += pass->GetName();
            }
            return digest;
        }

        std::string MakeConnectionKey(const std::string& outputPass, const std::string& inputPass)
        {
            std::string key = outputPass;
            key += "->";
            key += inputPass;
            return key;
        }

        std::unordered_map<std::string, std::string> BuildConnectionResourceLabels(const Ref<RenderGraph>& graph)
        {
            std::unordered_map<std::string, std::string> labels;
            if (!graph)
                return labels;

            for (const auto& transition : graph->GetResourceTransitions())
            {
                if (transition.ProducerPass.empty() || transition.ConsumerPass.empty())
                    continue;

                auto& label = labels[MakeConnectionKey(transition.ProducerPass, transition.ConsumerPass)];
                if (!label.empty())
                    label += ", ";
                label += transition.ResourceName;
            }

            return labels;
        }
    } // namespace

    void RenderGraphDebugger::RenderDebugView(const Ref<RenderGraph>& graph, bool* open, const char* title)
    {
        OLO_PROFILE_FUNCTION();

        // Begin ImGui window
        ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(title, open))
        {
            ImGui::End();
            return;
        }

        if (!graph)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "No valid render graph provided!");
            ImGui::End();
            return;
        }

        auto passes = GetVisiblePasses(graph);
        if (passes.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Render graph has no passes to visualize");
            ImGui::End();
            return;
        }

        const std::string visibleDigest = BuildVisiblePassDigest(passes);
        if (visibleDigest != m_VisiblePassDigest)
        {
            m_VisiblePassDigest = visibleDigest;
            m_NeedsLayout = true;
        }

        // Calculate layout if needed
        if (m_NeedsLayout)
        {
            CalculateLayout(graph);
            m_NeedsLayout = false;
        }

        // Controls at the top with more padding
        const f32 controlsPaddingY = 10.0f; // Padding between controls and title bar
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + controlsPaddingY);

        // Controls group
        ImGui::BeginGroup();
        ImGui::TextDisabled("Active passes: %zu  Culled/hidden: %zu", passes.size(), graph->GetCulledPasses().size());

        bool resetViewRequested = false;
        if (ImGui::Button("Reset View"))
        {
            m_Settings.ScrollOffset = ImVec2(0.0f, 0.0f);
            resetViewRequested = true;
        }

        ImGui::SameLine();
        if (ImGui::Button("Export to DOT"))
        {
            std::string filePath = FileDialogs::SaveFile("GraphViz DOT (*.dot)\0*.dot\0");
            if (!filePath.empty())
            {
                ExportGraphViz(graph, filePath);
            }
        }

        ImGui::SameLine();
        if (ImGui::Button("Recalculate Layout"))
        {
            m_NeedsLayout = true;
        }

        // Per-pass GPU capture controls — installs the post-pass hook on
        // the live render graph, captures one frame's intermediate scene
        // FB contents, displays them as thumbnails for ghost / regression
        // debugging.
        DrawCapturePanel(graph);

        ImGui::EndGroup();

        // Add spacing between controls and canvas
        ImGui::Spacing();
        ImGui::Spacing();

        // Canvas setup with real ImGui scrollbars. The content item is sized
        // to the graph bounds, while drawing is clipped to the visible child.
        auto graphContentSize = ImVec2(m_Settings.NodeWidth + m_Settings.CanvasPadding * 2.0f,
                                       m_Settings.NodeHeight + m_Settings.CanvasPadding * 2.0f);
        for (const auto& [passName, nodeData] : m_NodePositions)
        {
            graphContentSize.x = std::max(graphContentSize.x, nodeData.Position.x + nodeData.Size.x + m_Settings.CanvasPadding);
            graphContentSize.y = std::max(graphContentSize.y, nodeData.Position.y + nodeData.Size.y + m_Settings.CanvasPadding);
        }

        ImVec2 canvasSize = ImGui::GetContentRegionAvail();
        if (canvasSize.x <= 0.0f || canvasSize.y <= 0.0f)
        {
            canvasSize.x = std::max(100.0f, canvasSize.x);
            canvasSize.y = std::max(100.0f, canvasSize.y);
        }

        constexpr ImGuiWindowFlags canvasFlags = ImGuiWindowFlags_HorizontalScrollbar;
        if (ImGui::BeginChild("##render_graph_canvas", canvasSize, true, canvasFlags))
        {
            if (resetViewRequested)
            {
                ImGui::SetScrollX(0.0f);
                ImGui::SetScrollY(0.0f);
            }

            const ImVec2 visibleSize = ImGui::GetContentRegionAvail();
            graphContentSize.x = std::max(graphContentSize.x, visibleSize.x);
            graphContentSize.y = std::max(graphContentSize.y, visibleSize.y);

            ImVec2 canvasPos = ImGui::GetCursorScreenPos();
            const ImVec2 viewMin = ImGui::GetWindowPos();
            const ImVec2 windowSize = ImGui::GetWindowSize();
            const ImVec2 viewMax(viewMin.x + windowSize.x, viewMin.y + windowSize.y);

            // Register the full graph area as an interactive item so the child
            // window knows how large its scrollable content is.
            ImGui::InvisibleButton("##canvas_scroll_surface", graphContentSize,
                                   ImGuiButtonFlags_MouseButtonLeft |
                                       ImGuiButtonFlags_MouseButtonMiddle |
                                       ImGuiButtonFlags_MouseButtonRight);
            const bool isCanvasHovered = ImGui::IsItemHovered();

            if (isCanvasHovered && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))
            {
                const ImVec2 mouseDelta = ImGui::GetIO().MouseDelta;
                ImGui::SetScrollX(ImGui::GetScrollX() - mouseDelta.x);
                ImGui::SetScrollY(ImGui::GetScrollY() - mouseDelta.y);
            }

            // Get draw list for canvas operations
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->PushClipRect(viewMin, viewMax, true);

            // Draw background
            drawList->AddRectFilled(viewMin, viewMax, m_Settings.BackgroundColor);

            // Draw grid if enabled
            if (m_Settings.DrawGrid)
            {
                const f32 gridSize = 32.0f;
                const ImU32 gridColor = IM_COL32(200, 200, 200, 40);

                f32 gridX = canvasPos.x + m_Settings.ScrollOffset.x;
                while (gridX > viewMin.x)
                    gridX -= gridSize;
                for (f32 x = gridX; x < viewMax.x; x += gridSize)
                    drawList->AddLine(ImVec2(x, viewMin.y), ImVec2(x, viewMax.y), gridColor);

                f32 gridY = canvasPos.y + m_Settings.ScrollOffset.y;
                while (gridY > viewMin.y)
                    gridY -= gridSize;
                for (f32 y = gridY; y < viewMax.y; y += gridSize)
                    drawList->AddLine(ImVec2(viewMin.x, y), ImVec2(viewMax.x, y), gridColor);
            }

            // Apply scroll position and any debug pan offset.
            auto offset = ImVec2(canvasPos.x + m_Settings.ScrollOffset.x, canvasPos.y + m_Settings.ScrollOffset.y);

            // Draw connections between nodes
            DrawConnections(graph, drawList, offset);

            // Draw nodes
            f32 maxWidth = 0.0f;
            for (const auto& pass : passes)
            {
                DrawNode(pass, drawList, offset, maxWidth);
            }

            // Show tooltip when hovering over a node
            if (isCanvasHovered && ImGui::IsMouseHoveringRect(viewMin, viewMax))
            {
                const ImVec2 mousePos = ImGui::GetIO().MousePos;
                for (const auto& [passName, nodeData] : m_NodePositions)
                {
                    auto nodeMin = ImVec2(offset.x + nodeData.Position.x, offset.y + nodeData.Position.y);
                    auto nodeMax = ImVec2(nodeMin.x + nodeData.Size.x, nodeMin.y + nodeData.Size.y);

                    if (mousePos.x >= nodeMin.x && mousePos.x <= nodeMax.x &&
                        mousePos.y >= nodeMin.y && mousePos.y <= nodeMax.y)
                    {
                        // Find the pass and show tooltip
                        for (const auto& pass : passes)
                        {
                            if (pass->GetName() == passName)
                            {
                                DrawTooltip(pass);
                                break;
                            }
                        }
                        break;
                    }
                }
            }

            drawList->PopClipRect();
        }
        ImGui::EndChild();

        ImGui::End();
    }

    bool RenderGraphDebugger::ExportGraphViz(const Ref<RenderGraph>& graph, const std::string& outputPath) const
    {
        if (!graph)
        {
            OLO_CORE_ERROR("RenderGraphDebugger::ExportGraphViz: No valid graph provided!");
            return false;
        }

        std::ofstream dotFile(outputPath);
        if (!dotFile.is_open())
        {
            OLO_CORE_ERROR("Failed to open file for writing: {0}", outputPath);
            return false;
        }

        // Write DOT file header
        dotFile << "digraph RenderGraph {\n";
        dotFile << "  bgcolor=\"#282828\";\n";
        dotFile << "  node [shape=box, style=filled, color=\"#CCCCCC\", fillcolor=\"#444444\", fontcolor=\"#FFFFFF\", fontname=\"Arial\"];\n";
        dotFile << "  edge [color=\"#AAAAAA\"];\n\n";

        // Get all passes
        auto passes = GetVisiblePasses(graph);
        const auto visibleNames = BuildVisiblePassNameSet(passes);

        // Write nodes
        for (const auto& pass : passes)
        {
            dotFile << "  \"" << pass->GetName() << "\" [";

            if (graph->IsFinalPass(pass->GetName()))
            {
                dotFile << "fillcolor=\"#446044\"";
            }

            dotFile << "label=\"" << pass->GetName();

            if (const auto& framebuffer = pass->GetTarget(); framebuffer)
            {
                const auto& spec = framebuffer->GetSpecification();
                dotFile << "\\n"
                        << spec.Width << "x" << spec.Height;

                if (!spec.Attachments.Attachments.empty())
                {
                    dotFile << "\\nAttachments: " << spec.Attachments.Attachments.size();
                }
            }
            else
            {
                dotFile << "\\n[Default FB]";
            }

            dotFile << "\"];\n";
        }

        // Write edges
        const auto& connections = graph->GetConnections();
        for (const auto& connection : connections)
        {
            if (!visibleNames.contains(connection.OutputPass) || !visibleNames.contains(connection.InputPass))
                continue;

            dotFile << "  \"" << connection.OutputPass << "\" -> \"" << connection.InputPass << "\";\n";
        }

        // Close DOT file
        dotFile << "}\n";
        dotFile.close();

        OLO_CORE_INFO("Render graph exported to {0}", outputPath);
        return true;
    }

    void RenderGraphDebugger::DrawNode(const Ref<RenderPass>& pass, ImDrawList* drawList, const ImVec2& offset, f32& maxWidth)
    {
        const std::string& passName = pass->GetName();

        if (!m_NodePositions.contains(passName))
        {
            // Should never happen if CalculateLayout was called
            OLO_CORE_WARN("RenderGraphDebugger::DrawNode: No position data for pass: {0}", passName);
            return;
        }

        const NodeData& nodeData = m_NodePositions[passName];

        auto nodePos = ImVec2(offset.x + nodeData.Position.x, offset.y + nodeData.Position.y);
        ImVec2 nodeSize = nodeData.Size;

        // Adjust max width if needed
        maxWidth = std::max(maxWidth, nodePos.x + nodeSize.x);

        // Draw node background
        drawList->AddRectFilled(
            nodePos,
            ImVec2(nodePos.x + nodeSize.x, nodePos.y + nodeSize.y),
            nodeData.Color,
            4.0f);

        // Draw node border
        drawList->AddRect(
            nodePos,
            ImVec2(nodePos.x + nodeSize.x, nodePos.y + nodeSize.y),
            m_Settings.NodeBorderColor,
            4.0f,
            ImDrawFlags_None,
            m_Settings.NodeBorderThickness);

        // Draw node title
        ImVec2 textSize = ImGui::CalcTextSize(passName.c_str());
        drawList->AddText(
            ImVec2(nodePos.x + (nodeSize.x - textSize.x) * 0.5f, nodePos.y + 10.0f),
            IM_COL32_WHITE,
            passName.c_str());

        // Draw framebuffer info if available
        if (auto framebuffer = pass->GetTarget(); framebuffer)
        {
            const auto& spec = framebuffer->GetSpecification();
            std::string fbInfo = std::format("{}x{}", spec.Width, spec.Height);

            textSize = ImGui::CalcTextSize(fbInfo.c_str());
            drawList->AddText(
                ImVec2(nodePos.x + (nodeSize.x - textSize.x) * 0.5f, nodePos.y + 30.0f),
                IM_COL32(200, 200, 200, 255),
                fbInfo.c_str());
        }
        else
        {
            std::string fbInfo = "[Default FB]";
            textSize = ImGui::CalcTextSize(fbInfo.c_str());
            drawList->AddText(
                ImVec2(nodePos.x + (nodeSize.x - textSize.x) * 0.5f, nodePos.y + 30.0f),
                IM_COL32(150, 200, 150, 255),
                fbInfo.c_str());
        }
    }

    void RenderGraphDebugger::DrawConnections(const Ref<RenderGraph>& graph, ImDrawList* drawList, const ImVec2& offset)
    {
        const auto& connections = graph->GetConnections();
        const auto resourceLabels = BuildConnectionResourceLabels(graph);
        for (const auto& connection : connections)
        {
            const std::string& outputName = connection.OutputPass;
            const std::string& inputName = connection.InputPass;

            if (!m_NodePositions.contains(inputName) || !m_NodePositions.contains(outputName))
            {
                continue;
            }

            const NodeData& inputNode = m_NodePositions[inputName];
            const NodeData& outputNode = m_NodePositions[outputName];

            // Calculate connection points with proper offset
            ImVec2 start(
                offset.x + outputNode.Position.x + outputNode.Size.x / 2.0f,
                offset.y + outputNode.Position.y + outputNode.Size.y);

            ImVec2 end(
                offset.x + inputNode.Position.x + inputNode.Size.x / 2.0f,
                offset.y + inputNode.Position.y);

            // Draw bezier curve
            const f32 curveHeight = 40.0f;
            ImVec2 cp1(start.x, start.y + curveHeight);
            ImVec2 cp2(end.x, end.y - curveHeight);

            drawList->AddBezierCubic(
                start, cp1, cp2, end,
                m_Settings.ConnectionColor,
                m_Settings.ConnectionThickness);

            // Draw arrow
            constexpr f32 arrowSize = 7.0f;
            auto dir = ImVec2(cp2.x - end.x, cp2.y - end.y);
            f32 len = sqrtf(dir.x * dir.x + dir.y * dir.y);
            dir.x /= len;
            dir.y /= len;

            auto norm = ImVec2(-dir.y, dir.x);
            auto p1 = ImVec2(end.x + dir.x * arrowSize + norm.x * arrowSize,
                             end.y + dir.y * arrowSize + norm.y * arrowSize);
            auto p2 = ImVec2(end.x + dir.x * arrowSize - norm.x * arrowSize,
                             end.y + dir.y * arrowSize - norm.y * arrowSize);

            drawList->AddTriangleFilled(end, p1, p2, m_Settings.ConnectionColor);

            if (const auto labelIt = resourceLabels.find(MakeConnectionKey(outputName, inputName)); labelIt != resourceLabels.end())
            {
                const ImVec2 mid((start.x + end.x) * 0.5f, (start.y + end.y) * 0.5f);
                drawList->AddText(ImVec2(mid.x + 4.0f, mid.y - 10.0f), IM_COL32(180, 210, 255, 220), labelIt->second.c_str());
            }
        }
    }

    void RenderGraphDebugger::DrawTooltip(const Ref<RenderPass>& pass) const
    {
        ImGui::BeginTooltip();
        ImGui::Text("Pass: %s", pass->GetName().c_str());

        if (auto framebuffer = pass->GetTarget(); framebuffer)
        {
            const auto& spec = framebuffer->GetSpecification();
            ImGui::Text("Size: %dx%d", spec.Width, spec.Height);
            ImGui::Text("Samples: %d", spec.Samples);

            ImGui::Text("Attachments:");
            for (sizet i = 0; i < spec.Attachments.Attachments.size(); i++)
            {
                const auto& format = spec.Attachments.Attachments[i].TextureFormat;
                ImGui::Text("  [%zu] %s", i, Utils::FormatToString(format).c_str());
            }
        }
        else
        {
            ImGui::Text("Target: Default Framebuffer");
        }

        ImGui::EndTooltip();
    }

    void RenderGraphDebugger::CalculateLayout(const Ref<RenderGraph>& graph)
    {
        OLO_PROFILE_FUNCTION();

        m_NodePositions.clear();

        auto passes = GetVisiblePasses(graph);

        // Step 1: Create a dependency graph
        std::unordered_map<std::string, std::vector<std::string>> dependsOn;  // pass -> passes it depends on
        std::unordered_map<std::string, std::vector<std::string>> dependedBy; // pass -> passes that depend on it
        std::unordered_map<std::string, int> inDegree;                        // Number of dependencies

        // Initialize maps for all passes first
        for (const auto& pass : passes)
        {
            const std::string& passName = pass->GetName();
            dependsOn[passName] = {};
            dependedBy[passName] = {};
            inDegree[passName] = 0;
        }

        // Now process the connections
        const auto& connections = graph->GetConnections();
        for (const auto& connection : connections)
        {
            const std::string& outputName = connection.OutputPass;
            const std::string& inputName = connection.InputPass;

            dependsOn[inputName].push_back(outputName);
            dependedBy[outputName].push_back(inputName);
            inDegree[inputName]++;
        }

        // Step 2: Assign layers using topological sorting
        std::unordered_map<std::string, int> layers;
        std::queue<std::string> queue;

        // Find nodes with no dependencies (sources)
        for (const auto& [passName, deps] : inDegree)
        {
            if (deps == 0)
            {
                queue.push(passName);
                layers[passName] = 0; // Source nodes are at layer 0
            }
        }

        while (!queue.empty())
        {
            std::string current = queue.front();
            queue.pop();

            for (const auto& dependent : dependedBy[current])
            {
                // Update layer of dependent
                layers[dependent] = std::max(layers[dependent], layers[current] + 1);

                // Decrease in-degree and check if ready
                inDegree[dependent]--;
                if (inDegree[dependent] == 0)
                {
                    queue.push(dependent);
                }
            }
        }

        // Step 3: Count nodes per layer
        std::unordered_map<int, int> nodesPerLayer;
        int maxLayer = 0;

        for (const auto& [passName, layer] : layers)
        {
            nodesPerLayer[layer]++;
            maxLayer = std::max(maxLayer, layer);
        }

        // Step 4: Assign positions based on layers
        std::unordered_map<int, int> layerCounts; // Current count for each layer

        for (const auto& pass : passes)
        {
            const std::string& passName = pass->GetName();
            int layer = layers[passName];

            if (!layerCounts.contains(layer))
            {
                layerCounts[layer] = 0;
            }

            // Calculate position
            f32 x = m_Settings.CanvasPadding +
                    (m_Settings.NodeWidth + m_Settings.NodeSpacingX) * layerCounts[layer];

            f32 y = m_Settings.CanvasPadding +
                    (m_Settings.NodeHeight + m_Settings.NodeSpacingY) * layer;

            // Store node data
            NodeData nodeData;
            nodeData.Position = ImVec2(x, y);
            nodeData.Size = ImVec2(m_Settings.NodeWidth, m_Settings.NodeHeight);

            // Set color based on whether it's the final pass
            if (graph->IsFinalPass(passName))
            {
                nodeData.Color = m_Settings.FinalNodeFillColor;
            }
            else
            {
                nodeData.Color = m_Settings.NodeFillColor;
            }

            m_NodePositions[passName] = nodeData;

            // Increment counter for this layer
            layerCounts[layer]++;
        }
    }

    void RenderGraphDebugger::DrawCapturePanel(const Ref<RenderGraph>& graph)
    {
        ImGui::SameLine();
        const bool hookInstalled = graph && graph->HasPostPassHook();
        if (ImGui::Button(hookInstalled ? "Recapture Frame" : "Capture Frame"))
        {
            // Debug instrumentation: installing a post-pass hook on the live
            // graph is a logical mutation, but RenderDebugView() takes the
            // graph by const reference (read-only intent for normal UI). The
            // const_cast is intentional and confined to debug capture.
            m_FrameCapture.InstallHook(const_cast<RenderGraph*>(graph.get()));
            m_FrameCapture.RequestCapture();
            m_SelectedCaptureIndex = -1;
            m_CaptureWindowOpen = true;
        }

        if (m_FrameCapture.HasCapture())
        {
            ImGui::SameLine();
            if (ImGui::Button("Clear Captures"))
            {
                m_FrameCapture.ClearCaptures();
                m_SelectedCaptureIndex = -1;
            }
        }

        if (!m_CaptureWindowOpen)
        {
            return;
        }

        // Render the capture viewer in a separate window so it doesn't fight
        // the graph node canvas for screen space.
        ImGui::SetNextWindowSize(ImVec2(900, 600), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Render Graph Per-Pass Capture", &m_CaptureWindowOpen))
        {
            const auto& captures = m_FrameCapture.GetCaptures();
            if (captures.empty())
            {
                ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f),
                                   "No captures yet. Click \"Capture Frame\" and render at least one frame "
                                   "(unpause the editor / move the camera) to fill this view.");
            }
            else
            {
                ImGui::TextWrapped("%zu capture(s). Click a thumbnail to enlarge. SceneColor is captured as a timeline; "
                                   "G-Buffer, AO, post-chain, UIComposite, and Backbuffer captures are recorded after their owning pass writes them.",
                                   captures.size());
                ImGui::Separator();

                // Scrollable thumbnail strip on the left, viewer on the right.
                const f32 thumbWidth = 196.0f;
                const f32 listWidth = thumbWidth + 36.0f;
                if (ImGui::BeginChild("##captures_list", ImVec2(listWidth, 0), true))
                {
                    for (i32 i = 0; i < static_cast<i32>(captures.size()); ++i)
                    {
                        const auto& entry = captures[static_cast<sizet>(i)];
                        ImGui::PushID(i);

                        const f32 aspect = entry.Height > 0 ? static_cast<f32>(entry.Width) / static_cast<f32>(entry.Height) : 1.0f;
                        const f32 thumbHeight = thumbWidth / std::max(0.001f, aspect);

                        const std::string label = std::format("{} | {}", entry.PassName, RenderGraphFrameCapture::SourceName(entry.SourceKind));
                        ImGui::TextUnformatted(label.c_str());
                        if (!entry.ResourceName.empty())
                        {
                            ImGui::TextDisabled("%s", entry.ResourceName.c_str());
                        }

                        if (entry.NonBlackSamples == 0)
                        {
                            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "BLACK (nonBlack: %u/9)", entry.NonBlackSamples);
                        }
                        else if (entry.NonTransparentSamples == 0)
                        {
                            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.35f, 1.0f), "TRANSPARENT (alpha: %u/9)", entry.NonTransparentSamples);
                        }
                        else
                        {
                            ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.55f, 1.0f), "VISIBLE (nonBlack: %u/9)", entry.NonBlackSamples);
                        }

                        const ImTextureID texID = static_cast<ImTextureID>(static_cast<uintptr_t>(entry.TextureID));
                        if (ImGui::ImageButton("##thumb", texID, ImVec2(thumbWidth, thumbHeight), ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f)))
                        {
                            m_SelectedCaptureIndex = i;
                        }
                        ImGui::Spacing();
                        ImGui::PopID();
                    }
                }
                ImGui::EndChild();

                ImGui::SameLine();

                if (ImGui::BeginChild("##capture_viewer", ImVec2(0, 0), true))
                {
                    if (m_SelectedCaptureIndex < 0 || m_SelectedCaptureIndex >= static_cast<i32>(captures.size()))
                    {
                        ImGui::TextDisabled("Select a thumbnail on the left to view it full size.");
                    }
                    else
                    {
                        const auto& entry = captures[static_cast<sizet>(m_SelectedCaptureIndex)];
                        ImGui::Text("Pass:   %s", entry.PassName.c_str());
                        ImGui::Text("Source: %s", RenderGraphFrameCapture::SourceName(entry.SourceKind));
                        ImGui::Text("Resource: %s", entry.ResourceName.empty() ? "<unknown>" : entry.ResourceName.c_str());
                        ImGui::Text("Size:   %u x %u", entry.Width, entry.Height);
                        ImGui::Text("GL IDs: sourceTex=%u  sourceFB=%u  captureTex=%u",
                                    entry.SourceTextureID, entry.SourceFramebufferID, entry.TextureID);
                        if (entry.PassOrderIndex != std::numeric_limits<u32>::max())
                        {
                            ImGui::Text("Graph:  passIndex=%u  resources=%u  barriers=%u  culled=%u",
                                        entry.PassOrderIndex,
                                        entry.ResourceCount,
                                        entry.PlannedBarrierCount,
                                        entry.CulledPassCount);
                        }
                        ImGui::Text("Probe:  nonBlack=%u/9  nonTransparent=%u/9  center=(%u,%u,%u,%u)",
                                    entry.NonBlackSamples,
                                    entry.NonTransparentSamples,
                                    static_cast<u32>(entry.CenterRGBA[0]),
                                    static_cast<u32>(entry.CenterRGBA[1]),
                                    static_cast<u32>(entry.CenterRGBA[2]),
                                    static_cast<u32>(entry.CenterRGBA[3]));
                        ImGui::Separator();

                        const ImVec2 avail = ImGui::GetContentRegionAvail();
                        const f32 aspect = entry.Height > 0 ? static_cast<f32>(entry.Width) / static_cast<f32>(entry.Height) : 1.0f;
                        f32 displayW = avail.x;
                        f32 displayH = displayW / std::max(0.001f, aspect);
                        if (displayH > avail.y)
                        {
                            displayH = avail.y;
                            displayW = displayH * aspect;
                        }
                        const ImTextureID texID = static_cast<ImTextureID>(static_cast<uintptr_t>(entry.TextureID));
                        ImGui::Image(texID, ImVec2(displayW, displayH), ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
                    }
                }
                ImGui::EndChild();
            }
        }
        ImGui::End();

        // If the user closed the capture window, also remove the hook to
        // avoid pointless per-pass GPU copies on every frame.
        if (!m_CaptureWindowOpen)
        {
            m_FrameCapture.InstallHook(nullptr);
        }
    }
} // namespace OloEngine

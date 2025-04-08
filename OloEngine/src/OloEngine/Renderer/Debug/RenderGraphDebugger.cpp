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

namespace OloEngine
{
    namespace Utils
    {
        // Helper function to convert FramebufferTextureFormat to string
        std::string FormatToString(FramebufferTextureFormat format)
        {
            switch (format)
            {
                case FramebufferTextureFormat::None:          return "None";
                case FramebufferTextureFormat::RGBA8:         return "RGBA8";
                case FramebufferTextureFormat::RED_INTEGER:   return "RED_INTEGER";
                case FramebufferTextureFormat::DEPTH24STENCIL8: return "Depth24Stencil8";
                default: return "Unknown";
            }
        }
    }

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

        auto passes = graph->GetAllPasses();
        if (passes.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Render graph has no passes to visualize");
            ImGui::End();
            return;
        }

        // Calculate layout if needed
        if (m_NeedsLayout)
        {
            CalculateLayout(graph);
            m_NeedsLayout = false;
        }

        // Controls at the top with more padding
        const f32 controlsPaddingY = 10.0f;  // Padding between controls and title bar
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + controlsPaddingY);
        
        // Controls group
        ImGui::BeginGroup();
        if (ImGui::Button("Reset View"))
        {
            m_Settings.ScrollOffset = ImVec2(0.0f, 0.0f);
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
        ImGui::EndGroup();

        // Add spacing between controls and canvas
        ImGui::Spacing();
        ImGui::Spacing();
        
        // Canvas setup with adjusted positioning
        ImVec2 canvasSize = ImGui::GetContentRegionAvail();
        if (canvasSize.x <= 0.0f || canvasSize.y <= 0.0f)
        {
            canvasSize.x = std::max(100.0f, canvasSize.x);
            canvasSize.y = std::max(100.0f, canvasSize.y);
        }
        
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        
        // Get draw list for canvas operations
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        
        // Draw background
        drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), m_Settings.BackgroundColor);
        
        // Draw grid if enabled
        if (m_Settings.DrawGrid)
        {
            const f32 gridSize = 32.0f;
            const ImU32 gridColor = IM_COL32(200, 200, 200, 40);
            
            for (f32 x = canvasPos.x; x < canvasPos.x + canvasSize.x; x += gridSize)
                drawList->AddLine(ImVec2(x, canvasPos.y), ImVec2(x, canvasPos.y + canvasSize.y), gridColor);
            
            for (f32 y = canvasPos.y; y < canvasPos.y + canvasSize.y; y += gridSize)
                drawList->AddLine(ImVec2(canvasPos.x, y), ImVec2(canvasPos.x + canvasSize.x, y), gridColor);
        }
        
        // Set canvas origin for mouse interaction
        ImGui::SetCursorScreenPos(canvasPos);
        ImGui::InvisibleButton("canvas", canvasSize);
        bool isCanvasHovered = ImGui::IsItemHovered();
        
        // Handle scrolling and panning
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f))
        {
            m_Settings.ScrollOffset.x += ImGui::GetIO().MouseDelta.x;
            m_Settings.ScrollOffset.y += ImGui::GetIO().MouseDelta.y;
        }
        
        // Apply scrolling
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
        if (isCanvasHovered && ImGui::IsMouseHoveringRect(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y)))
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
        auto passes = graph->GetAllPasses();
        
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
                dotFile << "\\n" << spec.Width << "x" << spec.Height;
                
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
            4.0f
        );
        
        // Draw node border
        drawList->AddRect(
            nodePos,
            ImVec2(nodePos.x + nodeSize.x, nodePos.y + nodeSize.y),
            m_Settings.NodeBorderColor,
            4.0f,
            ImDrawFlags_None,
            m_Settings.NodeBorderThickness
        );
        
        // Draw node title
        ImVec2 textSize = ImGui::CalcTextSize(passName.c_str());
        drawList->AddText(
            ImVec2(nodePos.x + (nodeSize.x - textSize.x) * 0.5f, nodePos.y + 10.0f),
            IM_COL32_WHITE,
            passName.c_str()
        );
        
        // Draw framebuffer info if available        
        if (auto framebuffer = pass->GetTarget(); framebuffer)
        {
            const auto& spec = framebuffer->GetSpecification();
			std::string fbInfo = std::format("{}x{}", spec.Width, spec.Height);
            
            textSize = ImGui::CalcTextSize(fbInfo.c_str());
            drawList->AddText(
                ImVec2(nodePos.x + (nodeSize.x - textSize.x) * 0.5f, nodePos.y + 30.0f),
                IM_COL32(200, 200, 200, 255),
                fbInfo.c_str()
            );
        }
        else
        {
            std::string fbInfo = "[Default FB]";
            textSize = ImGui::CalcTextSize(fbInfo.c_str());
            drawList->AddText(
                ImVec2(nodePos.x + (nodeSize.x - textSize.x) * 0.5f, nodePos.y + 30.0f),
                IM_COL32(150, 200, 150, 255),
                fbInfo.c_str()
            );
        }
    }
    
    void RenderGraphDebugger::DrawConnections(const Ref<RenderGraph>& graph, ImDrawList* drawList, const ImVec2& offset)
    {   
        const auto& connections = graph->GetConnections();
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
				offset.y + outputNode.Position.y + outputNode.Size.y
			);
			
			ImVec2 end(
				offset.x + inputNode.Position.x + inputNode.Size.x / 2.0f,
				offset.y + inputNode.Position.y
			);
            
            // Draw bezier curve
            const f32 curveHeight = 40.0f;
            ImVec2 cp1(start.x, start.y + curveHeight);
            ImVec2 cp2(end.x, end.y - curveHeight);
            
            drawList->AddBezierCubic(
                start, cp1, cp2, end,
                m_Settings.ConnectionColor,
                m_Settings.ConnectionThickness
            );
            
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
            for (size_t i = 0; i < spec.Attachments.Attachments.size(); i++)
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
		
		auto passes = graph->GetAllPasses();
		
		// Step 1: Create a dependency graph
		std::unordered_map<std::string, std::vector<std::string>> dependsOn; // pass -> passes it depends on
		std::unordered_map<std::string, std::vector<std::string>> dependedBy; // pass -> passes that depend on it
		std::unordered_map<std::string, int> inDegree; // Number of dependencies
		
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

    // Update the LayoutSettings struct to include scroll offset
    struct LayoutSettings 
    {
        f32 NodeWidth = 150.0f;
        f32 NodeHeight = 60.0f;
        f32 NodeSpacingX = 50.0f;
        f32 NodeSpacingY = 100.0f;
        f32 CanvasPadding = 20.0f;
        ImColor BackgroundColor = ImColor(40, 40, 40, 255);
        ImColor ConnectionColor = ImColor(180, 180, 180, 255);
        ImColor NodeBorderColor = ImColor(200, 200, 200, 255);
        ImColor NodeFillColor = ImColor(70, 70, 70, 255);
        ImColor FinalNodeFillColor = ImColor(70, 100, 70, 255);
        f32 ConnectionThickness = 2.0f;
        f32 NodeBorderThickness = 1.0f;
        bool DrawGrid = true;
        ImVec2 ScrollOffset = ImVec2(0.0f, 0.0f);
    };
}

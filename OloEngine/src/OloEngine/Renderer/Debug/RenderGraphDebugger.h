#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/RenderGraph.h"
#include "OloEngine/Renderer/RenderGraphNode.h"
#include "OloEngine/Renderer/Debug/RenderGraphFrameCapture.h"

#include <imgui.h>
#include <string>
#include <unordered_map>

namespace OloEngine
{
    // @brief Debug visualization tool for render graphs
    //
    // Provides functionality to visualize render graphs in the ImGui interface,
    // showing passes, connections and framebuffer details.
    class RenderGraphDebugger
    {
      public:
        RenderGraphDebugger() = default;
        ~RenderGraphDebugger() = default;

        // @brief Renders a debug view of the render graph in an ImGui window
        //
        // @param graph The render graph to visualize
        // @param open Pointer to a boolean that controls the visibility of the window
        // @param title The title of the ImGui window
        void RenderDebugView(const Ref<RenderGraph>& graph, bool* open = nullptr, const char* title = "Render Graph Debugger");

        // @brief Exports the render graph visualization to a DOT file for GraphViz
        //
        // @param graph The render graph to export
        // @param outputPath The file path to save the DOT file
        // @return True if the export was successful, false otherwise
        bool ExportGraphViz(const Ref<RenderGraph>& graph, const std::string& outputPath) const;

      private:
        // Helper methods for visualization
        void DrawNode(const Ref<RenderGraphNode>& node, ImDrawList* drawList, const ImVec2& offset, f32& maxWidth);
        void DrawConnections(const Ref<RenderGraph>& graph, ImDrawList* drawList, const ImVec2& offset);
        void DrawTooltip(const Ref<RenderGraphNode>& node) const;

        // Cache for node positions and sizes
        struct NodeData
        {
            ImVec2 Position;
            ImVec2 Size;
            ImColor Color;
        };

        // Layout parameters
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
            ImVec2 ScrollOffset = ImVec2(0.0f, 0.0f); // Added for pan/zoom support
        };

        std::unordered_map<std::string, NodeData> m_NodePositions;
        LayoutSettings m_Settings;
        bool m_NeedsLayout = true;

        // Per-pass GPU capture for ghost / regression debugging.
        RenderGraphFrameCapture m_FrameCapture;
        // Selected capture index for the full-size preview pane (-1 = none).
        i32 m_SelectedCaptureIndex = -1;
        bool m_CaptureWindowOpen = false;
        std::string m_VisiblePassDigest;

        // Currently inspected pass (left-click on canvas selects). Empty when
        // no pass is selected — the inspector section is then hidden.
        std::string m_SelectedPassName;

        // Auto-arm the per-pass capture whenever the debugger panel is open
        // so the thumbnail strip always reflects the current frame's outputs.
        // Toggle off to freeze on the last captured frame.
        bool m_AutoCaptureEachFrame = true;

        // Layout algorithm
        void CalculateLayout(const Ref<RenderGraph>& graph);

        // Renders the per-pass capture pane (button + thumbnail strip + viewer).
        void DrawCapturePanel(const Ref<RenderGraph>& graph);

        // Renders the inspector for m_SelectedPassName: declared reads/writes,
        // resolved primary input/output handles, enabled / ready / culled flags.
        void DrawPassInspector(const Ref<RenderGraph>& graph);

        // Renders a compact horizontal thumbnail strip below the canvas
        // showing one mini-image per captured pass output, with VISIBLE /
        // BLACK / TRANSPARENT badges. Click a thumbnail to open the full
        // capture viewer focused on that entry.
        void DrawCaptureThumbnailStrip();
    };
} // namespace OloEngine

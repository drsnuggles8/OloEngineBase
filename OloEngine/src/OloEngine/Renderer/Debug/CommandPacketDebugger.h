#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Commands/CommandPacket.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include "OloEngine/Renderer/Commands/DrawKey.h"
#include "CapturedFrameData.h"
#include "DebugUtils.h"

#include <imgui.h>
#include <deque>
#include <vector>
#include <string>
#include <unordered_map>

namespace OloEngine
{
    class RenderGraph;

    // @brief Debug visualization tool for command packets and draw keys
    //
    // RenderDoc-inspired recording and analysis tool for the command bucket system.
    // Supports single-frame capture, multi-frame recording, and deep analysis of
    // command ordering, state changes, batching, and GPU timing.
    class CommandPacketDebugger
    {
      public:
        static CommandPacketDebugger& GetInstance();

        // @brief Renders the full debug view
        // @param graph Live render graph (used to resolve the geometry stream bucket when no captures exist)
        // @param open Window visibility toggle
        // @param title Window title
        void RenderDebugView(const RenderGraph* graph, bool* open = nullptr, const char* title = "Command Bucket Inspector");

        // @brief Exports captured frame data to CSV
        bool ExportToCSV(const std::string& outputPath) const;

        // @brief Exports captured frame data to Markdown for LLM analysis
        bool ExportToMarkdown(const std::string& outputPath) const;

        // @brief Builds the LLM-analysis Markdown report for an arbitrary captured
        // frame (the exact text ExportToMarkdown writes to disk). Pure w.r.t. the
        // passed-in frame copy — no selected-frame / file-I/O coupling — so callers
        // that already hold a CapturedFrameData (e.g. the olo_render_frame_breakdown
        // MCP tool) can reuse the report without touching the UI selection state.
        static std::string BuildMarkdownReport(const CapturedFrameData& frame);

        // @brief Generates a timestamped filename for exports
        static std::string GenerateExportFilename(const char* extension, u32 frameNumber);

      private:
        CommandPacketDebugger() = default;
        ~CommandPacketDebugger() = default;
        CommandPacketDebugger(const CommandPacketDebugger&) = delete;
        CommandPacketDebugger& operator=(const CommandPacketDebugger&) = delete;

        // Tab rendering methods
        void RenderRecordingToolbar();
        void RenderFrameSelector();
        void RenderCommandList(const CapturedFrameData* frame);
        void RenderCommandDetail(const CapturedCommandData& cmd, const CapturedFrameData* frame);
        void RenderSortAnalysis(const CapturedFrameData* frame) const;
        void RenderStateChanges(const CapturedFrameData* frame) const;
        void RenderBatchingAnalysis(const CapturedFrameData* frame) const;
        void RenderTimeline(const CapturedFrameData* frame);
        void RenderLiveView(const CommandBucket* bucket) const;

        // Helpers
        static ImVec4 GetColorForCommandType(CommandType type);

        // Render state detail for DrawMeshCommand
        void RenderPODRenderStateDetail(const PODRenderState& state) const;
        void RenderDrawMeshDetail(const DrawMeshCommand& cmd, const CapturedFrameData* frame) const;
        void RenderDrawMeshInstancedDetail(const DrawMeshInstancedCommand& cmd, const CapturedFrameData* frame) const;

        enum class CommandViewMode : i32
        {
            PreSort = 0,
            PostSort = 1,
            PostBatch = 2
        };

        // UI state
        i32 m_SelectedTab = 0;
        i32 m_SelectedCommandIndex = -1;
        CommandViewMode m_CommandViewMode = CommandViewMode::PostSort;
        bool m_FilterByType = false;
        i32 m_TypeFilter = 0;
        bool m_FilterByStatic = false;
        bool m_StaticFilter = true;

        // Cached frame data for RenderFrameSelector (avoids per-frame deep copy)
        std::deque<CapturedFrameData> m_CachedFrames;
        sizet m_CachedFrameCount = 0;
        u64 m_CachedGeneration = 0;
    };
} // namespace OloEngine

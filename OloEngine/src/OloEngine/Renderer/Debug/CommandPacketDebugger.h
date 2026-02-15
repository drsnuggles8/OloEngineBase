#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Commands/CommandPacket.h"
#include "OloEngine/Renderer/Commands/CommandBucket.h"
#include "OloEngine/Renderer/Commands/DrawKey.h"
#include "CapturedFrameData.h"
#include "FrameCaptureManager.h"
#include "DebugUtils.h"

#include <imgui.h>
#include <vector>
#include <string>
#include <unordered_map>

namespace OloEngine
{
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
        // @param bucket Live command bucket (used for live view when no captures exist)
        // @param open Window visibility toggle
        // @param title Window title
        void RenderDebugView(const CommandBucket* bucket, bool* open = nullptr, const char* title = "Command Bucket Inspector");

        // @brief Exports captured frame data to CSV
        bool ExportToCSV(const std::string& outputPath) const;

    private:
        CommandPacketDebugger() = default;
        ~CommandPacketDebugger() = default;
        CommandPacketDebugger(const CommandPacketDebugger&) = delete;
        CommandPacketDebugger& operator=(const CommandPacketDebugger&) = delete;

        // Tab rendering methods
        void RenderRecordingToolbar();
        void RenderFrameSelector();
        void RenderCommandList(const CapturedFrameData* frame, const CommandBucket* liveBucket);
        void RenderCommandDetail(const CapturedCommandData& cmd);
        void RenderSortAnalysis(const CapturedFrameData* frame);
        void RenderStateChanges(const CapturedFrameData* frame);
        void RenderBatchingAnalysis(const CapturedFrameData* frame);
        void RenderTimeline(const CapturedFrameData* frame);
        void RenderLiveView(const CommandBucket* bucket);

        // Helpers
        static ImVec4 GetColorForCommandType(CommandType type);
        static const char* GetCommandTypeString(CommandType type);

        // Render state detail for DrawMeshCommand
        void RenderPODRenderStateDetail(const PODRenderState& state);
        void RenderDrawMeshDetail(const DrawMeshCommand& cmd);
        void RenderDrawMeshInstancedDetail(const DrawMeshInstancedCommand& cmd);

        // UI state
        i32 m_SelectedTab = 0;
        i32 m_SelectedCommandIndex = -1;
        i32 m_CommandViewMode = 1; // 0=PreSort, 1=PostSort, 2=PostBatch
        bool m_FilterByType = false;
        i32 m_TypeFilter = 0;
        bool m_FilterByStatic = false;
        bool m_StaticFilter = true;
    };
} // namespace OloEngine

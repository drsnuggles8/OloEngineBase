#include "OloEnginePCH.h"
#include "CommandPacketDebugger.h"
#include "FrameCaptureManager.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Renderer/Commands/DrawKey.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <cmath>

namespace OloEngine
{
    namespace
    {
        std::string EscapeCsvField(const std::string& field)
        {
            if (field.find_first_of(",\"\n") == std::string::npos)
                return field;
            std::string escaped = "\"";
            for (char c : field)
            {
                if (c == '"')
                    escaped += "\"\"";
                else
                    escaped += c;
            }
            escaped += '"';
            return escaped;
        }

        const PODRenderState* GetRenderStateFromCommand(const CapturedCommandData& cmd)
        {
            switch (cmd.GetCommandType())
            {
                case CommandType::DrawMesh:
                    if (const auto* c = cmd.GetCommandData<DrawMeshCommand>())
                        return &c->renderState;
                    break;
                case CommandType::DrawMeshInstanced:
                    if (const auto* c = cmd.GetCommandData<DrawMeshInstancedCommand>())
                        return &c->renderState;
                    break;
                case CommandType::DrawSkybox:
                    if (const auto* c = cmd.GetCommandData<DrawSkyboxCommand>())
                        return &c->renderState;
                    break;
                case CommandType::DrawInfiniteGrid:
                    if (const auto* c = cmd.GetCommandData<DrawInfiniteGridCommand>())
                        return &c->renderState;
                    break;
                case CommandType::DrawQuad:
                    if (const auto* c = cmd.GetCommandData<DrawQuadCommand>())
                        return &c->renderState;
                    break;
                default:
                    break;
            }
            return nullptr;
        }
    } // anonymous namespace

    CommandPacketDebugger& CommandPacketDebugger::GetInstance()
    {
        static CommandPacketDebugger instance;
        return instance;
    }

    // ========================================================================
    // Main entry point
    // ========================================================================

    void CommandPacketDebugger::RenderDebugView(const CommandBucket* bucket, bool* open, const char* title)
    {
        OLO_PROFILE_FUNCTION();

        if (open && !*open)
            return;

        ImGui::SetNextWindowSize(ImVec2(900, 700), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin(title, open, ImGuiWindowFlags_MenuBar))
        {
            ImGui::End();
            return;
        }

        // Menu bar
        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("Export"))
            {
                auto& cm = FrameCaptureManager::GetInstance();
                auto selectedFrame = cm.GetSelectedFrame();
                u32 frameNum = selectedFrame ? selectedFrame->FrameNumber : 0;

                if (ImGui::MenuItem("Export to CSV"))
                    ExportToCSV(GenerateExportFilename("csv", frameNum));
                if (ImGui::MenuItem("Export to Markdown (LLM Analysis)"))
                    ExportToMarkdown(GenerateExportFilename("md", frameNum));
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        // Recording toolbar (always visible)
        RenderRecordingToolbar();
        ImGui::Separator();

        auto& captureManager = FrameCaptureManager::GetInstance();
        auto selectedFrame = captureManager.GetSelectedFrame();
        const CapturedFrameData* selectedFramePtr = selectedFrame ? &*selectedFrame : nullptr;

        if (captureManager.GetCapturedFrameCount() > 0)
        {
            // Frame selector
            RenderFrameSelector();
            ImGui::Separator();

            // Tabs for analysis views
            if (ImGui::BeginTabBar("AnalysisTabs"))
            {
                if (ImGui::BeginTabItem("Commands"))
                {
                    RenderCommandList(selectedFramePtr, bucket);
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Sort Analysis"))
                {
                    RenderSortAnalysis(selectedFramePtr);
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("State Changes"))
                {
                    RenderStateChanges(selectedFramePtr);
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Batching"))
                {
                    RenderBatchingAnalysis(selectedFramePtr);
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Timeline"))
                {
                    RenderTimeline(selectedFramePtr);
                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }
        }
        else
        {
            // No captures — show live view
            RenderLiveView(bucket);
        }

        ImGui::End();
    }

    // ========================================================================
    // Recording Toolbar
    // ========================================================================

    void CommandPacketDebugger::RenderRecordingToolbar()
    {
        auto& captureManager = FrameCaptureManager::GetInstance();
        CaptureState state = captureManager.GetState();

        // Capture single frame
        if (ImGui::Button("Capture Frame"))
        {
            captureManager.CaptureNextFrame();
        }
        ImGui::SameLine();

        // Start/Stop recording
        if (state == CaptureState::Recording)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
            if (ImGui::Button("Stop Recording"))
                captureManager.StopRecording();
            ImGui::PopStyleColor();

            // Pulsing red indicator
            ImGui::SameLine();
            f32 pulse = (std::sin(static_cast<f32>(ImGui::GetTime()) * 4.0f) + 1.0f) * 0.5f;
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 0.5f + pulse * 0.5f), "REC");
        }
        else
        {
            if (ImGui::Button("Start Recording"))
                captureManager.StartRecording();
        }

        ImGui::SameLine();
        ImGui::Text("|");
        ImGui::SameLine();

        // Frame count
        ImGui::Text("Captured: %zu", captureManager.GetCapturedFrameCount());
        ImGui::SameLine();

        // Max frames config
        i32 maxFrames = static_cast<i32>(captureManager.GetMaxCapturedFrames());
        ImGui::SetNextItemWidth(100.0f);
        if (ImGui::SliderInt("Max", &maxFrames, 1, 300))
            captureManager.SetMaxCapturedFrames(static_cast<u32>(maxFrames));

        ImGui::SameLine();
        if (ImGui::Button("Clear"))
        {
            captureManager.ClearCaptures();
            m_SelectedCommandIndex = -1;
        }

        // Status text
        if (state == CaptureState::CaptureNextFrame)
        {
            ImGui::SameLine();
            ImGui::TextColored(DebugUtils::Colors::Warning, "Capturing next frame...");
        }
    }

    // ========================================================================
    // Frame Selector
    // ========================================================================

    void CommandPacketDebugger::RenderFrameSelector()
    {
        auto& captureManager = FrameCaptureManager::GetInstance();

        // Refresh cached frames only when generation changes
        u64 currentGen = captureManager.GetCaptureGeneration();
        if (currentGen != m_CachedGeneration)
        {
            m_CachedFrames = captureManager.GetCapturedFramesCopy();
            m_CachedFrameCount = m_CachedFrames.size();
            m_CachedGeneration = currentGen;
        }

        i32 selectedIdx = captureManager.GetSelectedFrameIndex();

        ImGui::Text("Frames:");
        ImGui::SameLine();

        // Horizontal scrolling frame list
        ImGui::BeginChild("FrameList", ImVec2(0, 120), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);

        for (i32 i = 0; i < static_cast<i32>(m_CachedFrames.size()); ++i)
        {
            const auto& frame = m_CachedFrames[i];
            bool isSelected = (i == selectedIdx);

            ImGui::PushID(i);
            char label[64];
            snprintf(label, sizeof(label), "#%u\n%u cmds\n%.2fms",
                     frame.FrameNumber,
                     frame.Stats.TotalCommands,
                     frame.Stats.TotalFrameTimeMs);

            if (isSelected)
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));

            if (ImGui::Button(label, ImVec2(80, 65)))
            {
                captureManager.SetSelectedFrameIndex(i);
                m_SelectedCommandIndex = -1;
            }

            if (isSelected)
                ImGui::PopStyleColor();

            ImGui::SameLine();
            ImGui::PopID();
        }

        ImGui::EndChild();
    }

    // ========================================================================
    // Command List Tab
    // ========================================================================

    void CommandPacketDebugger::RenderCommandList(const CapturedFrameData* frame, const CommandBucket* liveBucket)
    {
        if (!frame)
        {
            ImGui::TextColored(DebugUtils::Colors::Warning, "No frame selected.");
            return;
        }

        // View mode selector
        ImGui::Text("View:");
        ImGui::SameLine();
        i32 viewMode = static_cast<i32>(m_CommandViewMode);
        ImGui::RadioButton("Pre-Sort", &viewMode, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Post-Sort", &viewMode, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Post-Batch", &viewMode, 2);
        m_CommandViewMode = static_cast<CommandViewMode>(viewMode);

        // Filter controls
        ImGui::SameLine();
        ImGui::Text("|");
        ImGui::SameLine();
        ImGui::Checkbox("Filter Type", &m_FilterByType);
        if (m_FilterByType)
        {
            ImGui::SameLine();
            const char* typeNames[] = { "Draw", "Clear", "State", "Other" };
            ImGui::SetNextItemWidth(80.0f);
            ImGui::Combo("##TypeFilter", &m_TypeFilter, typeNames, IM_ARRAYSIZE(typeNames));
        }

        ImGui::Separator();

        // Select the command list based on view mode
        const std::vector<CapturedCommandData>* commands = nullptr;
        switch (m_CommandViewMode)
        {
            case CommandViewMode::PreSort:
                commands = &frame->PreSortCommands;
                break;
            case CommandViewMode::PostSort:
                commands = &frame->PostSortCommands;
                break;
            case CommandViewMode::PostBatch:
                commands = &frame->PostBatchCommands;
                break;
            default:
                commands = &frame->PostSortCommands;
                break;
        }

        if (!commands || commands->empty())
        {
            ImGui::TextColored(DebugUtils::Colors::Warning, "No commands in this view.");
            return;
        }

        // Summary
        ImGui::Text("Commands: %zu | Draw: %u | State: %u | Sort: %.3fms | Execute: %.3fms",
                    commands->size(), frame->Stats.DrawCalls, frame->Stats.StateChanges,
                    frame->Stats.SortTimeMs, frame->Stats.ExecuteTimeMs);
        ImGui::Separator();

        // Split: command table on left, detail on right
        f32 availWidth = ImGui::GetContentRegionAvail().x;
        f32 listWidth = availWidth * 0.55f;

        // Left panel: command table
        ImGui::BeginChild("CmdList", ImVec2(listWidth, 0), ImGuiChildFlags_Borders);

        if (ImGui::BeginTable("Commands", 6,
                              ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders))
        {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40.0f);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn("DrawKey", ImGuiTableColumnFlags_WidthFixed, 140.0f);
            ImGui::TableSetupColumn("Shader", ImGuiTableColumnFlags_WidthFixed, 55.0f);
            ImGui::TableSetupColumn("Material", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("Debug Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            for (i32 i = 0; i < static_cast<i32>(commands->size()); ++i)
            {
                const auto& cmd = (*commands)[i];

                // Apply filter
                if (m_FilterByType)
                {
                    bool pass = false;
                    switch (m_TypeFilter)
                    {
                        case 0:
                            pass = cmd.IsDrawCommand();
                            break;
                        case 1:
                            pass = (cmd.GetCommandType() == CommandType::Clear || cmd.GetCommandType() == CommandType::ClearStencil);
                            break;
                        case 2:
                            pass = cmd.IsStateCommand();
                            break;
                        case 3:
                            pass = !cmd.IsDrawCommand() && !cmd.IsStateCommand();
                            break;
                    }
                    if (!pass)
                        continue;
                }

                ImGui::TableNextRow();

                // Index
                ImGui::TableSetColumnIndex(0);
                bool selected = (m_SelectedCommandIndex == i);
                ImGui::PushID(i);
                if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_SpanAllColumns))
                    m_SelectedCommandIndex = i;
                ImGui::SameLine();
                ImGui::Text("%d", i);
                ImGui::PopID();

                // Type (color-coded)
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(GetColorForCommandType(cmd.GetCommandType()), "%s", cmd.GetCommandTypeString());

                // DrawKey
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("0x%016llX", cmd.GetSortKey().GetKey());

                // Shader ID
                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%u", cmd.GetSortKey().GetShaderID());

                // Material ID
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%u", cmd.GetSortKey().GetMaterialID());

                // Debug Name
                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%s", cmd.GetDebugName().empty() ? "-" : cmd.GetDebugName().c_str());
            }

            ImGui::EndTable();
        }

        ImGui::EndChild();

        ImGui::SameLine();

        // Right panel: detail view
        ImGui::BeginChild("CmdDetail", ImVec2(0, 0), ImGuiChildFlags_Borders);

        if (m_SelectedCommandIndex >= 0 && m_SelectedCommandIndex < static_cast<i32>(commands->size()))
        {
            RenderCommandDetail((*commands)[m_SelectedCommandIndex]);
        }
        else
        {
            ImGui::TextColored(DebugUtils::Colors::Disabled, "Select a command to see details.");
        }

        ImGui::EndChild();
    }

    // ========================================================================
    // Command Detail Panel
    // ========================================================================

    void CommandPacketDebugger::RenderCommandDetail(const CapturedCommandData& cmd)
    {
        ImGui::TextColored(GetColorForCommandType(cmd.GetCommandType()), "%s", cmd.GetCommandTypeString());
        ImGui::Separator();

        // DrawKey breakdown
        if (ImGui::CollapsingHeader("Draw Key", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const DrawKey& key = cmd.GetSortKey();
            ImGui::Text("Raw: 0x%016llX", key.GetKey());
            ImGui::Text("Viewport: %u", key.GetViewportID());
            ImGui::Text("View Layer: %s", ToString(key.GetViewLayer()));
            ImGui::Text("Render Mode: %s", ToString(key.GetRenderMode()));
            ImGui::Text("Shader ID: %u", key.GetShaderID());
            ImGui::Text("Material ID: %u", key.GetMaterialID());
            ImGui::Text("Depth: %u", key.GetDepth());
        }

        // Metadata
        if (ImGui::CollapsingHeader("Metadata", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Original Index: %u", cmd.GetOriginalIndex());
            ImGui::Text("Group ID: %u", cmd.GetGroupID());
            ImGui::Text("Execution Order: %u", cmd.GetExecutionOrder());
            ImGui::Text("Static: %s", cmd.IsStatic() ? "Yes" : "No");
            ImGui::Text("Depends on Previous: %s", cmd.DependsOnPrevious() ? "Yes" : "No");
            ImGui::Text("Debug Name: %s", cmd.GetDebugName().empty() ? "None" : cmd.GetDebugName().c_str());
            if (cmd.GetGpuTimeMs() > 0.0)
                ImGui::Text("GPU Time: %.4f ms", cmd.GetGpuTimeMs());
        }

        // Command-specific detail
        if (cmd.GetCommandType() == CommandType::DrawMesh)
        {
            if (const auto* meshCmd = cmd.GetCommandData<DrawMeshCommand>())
            {
                if (ImGui::CollapsingHeader("Draw Mesh", ImGuiTreeNodeFlags_DefaultOpen))
                    RenderDrawMeshDetail(*meshCmd);
            }
        }
        else if (cmd.GetCommandType() == CommandType::DrawMeshInstanced)
        {
            if (const auto* meshCmd = cmd.GetCommandData<DrawMeshInstancedCommand>())
            {
                if (ImGui::CollapsingHeader("Draw Mesh Instanced", ImGuiTreeNodeFlags_DefaultOpen))
                    RenderDrawMeshInstancedDetail(*meshCmd);
            }
        }

        // Render state for commands that have one
        const PODRenderState* state = GetRenderStateFromCommand(cmd);
        if (state && ImGui::CollapsingHeader("Render State"))
            RenderPODRenderStateDetail(*state);
    }

    void CommandPacketDebugger::RenderDrawMeshDetail(const DrawMeshCommand& cmd)
    {
        ImGui::Text("Mesh Handle: %llu", static_cast<u64>(cmd.meshHandle));
        ImGui::Text("VAO: %u", cmd.vertexArrayID);
        ImGui::Text("Index Count: %u", cmd.indexCount);
        ImGui::Text("Entity ID: %d", cmd.entityID);
        ImGui::Text("Shader: %u (handle: %llu)", cmd.shaderRendererID, static_cast<u64>(cmd.shaderHandle));

        ImGui::Separator();
        ImGui::Text("Transform:");
        const auto& t = cmd.transform;
        for (int row = 0; row < 4; ++row)
            ImGui::Text("  [%.2f, %.2f, %.2f, %.2f]", t[row][0], t[row][1], t[row][2], t[row][3]);

        ImGui::Separator();
        if (cmd.enablePBR)
        {
            ImGui::Text("PBR Material:");
            ImGui::Text("  Base Color: (%.2f, %.2f, %.2f, %.2f)", cmd.baseColorFactor.r, cmd.baseColorFactor.g, cmd.baseColorFactor.b, cmd.baseColorFactor.a);
            ImGui::Text("  Metallic: %.2f", cmd.metallicFactor);
            ImGui::Text("  Roughness: %.2f", cmd.roughnessFactor);
            ImGui::Text("  Normal Scale: %.2f", cmd.normalScale);
            ImGui::Text("  Occlusion: %.2f", cmd.occlusionStrength);
            ImGui::Text("  IBL: %s", cmd.enableIBL ? "Yes" : "No");
            ImGui::Text("  Textures: albedo=%u, metallicRough=%u, normal=%u, ao=%u, emissive=%u",
                        cmd.albedoMapID, cmd.metallicRoughnessMapID, cmd.normalMapID, cmd.aoMapID, cmd.emissiveMapID);
        }
        else
        {
            ImGui::Text("Legacy Material:");
            ImGui::Text("  Ambient: (%.2f, %.2f, %.2f)", cmd.ambient.r, cmd.ambient.g, cmd.ambient.b);
            ImGui::Text("  Diffuse: (%.2f, %.2f, %.2f)", cmd.diffuse.r, cmd.diffuse.g, cmd.diffuse.b);
            ImGui::Text("  Specular: (%.2f, %.2f, %.2f)", cmd.specular.r, cmd.specular.g, cmd.specular.b);
            ImGui::Text("  Shininess: %.1f", cmd.shininess);
            ImGui::Text("  Textures: diffuse=%u, specular=%u", cmd.diffuseMapID, cmd.specularMapID);
        }

        if (cmd.isAnimatedMesh)
        {
            ImGui::Separator();
            ImGui::Text("Animation: boneOffset=%u, boneCount=%u, worker=%u", cmd.boneBufferOffset, cmd.boneCount, cmd.workerIndex);
        }
    }

    void CommandPacketDebugger::RenderDrawMeshInstancedDetail(const DrawMeshInstancedCommand& cmd)
    {
        ImGui::Text("Mesh Handle: %llu", static_cast<u64>(cmd.meshHandle));
        ImGui::Text("VAO: %u", cmd.vertexArrayID);
        ImGui::Text("Index Count: %u", cmd.indexCount);
        ImGui::Text("Instance Count: %u", cmd.instanceCount);
        ImGui::Text("Transform Buffer: offset=%u, count=%u", cmd.transformBufferOffset, cmd.transformCount);
        ImGui::Text("Shader: %u (handle: %llu)", cmd.shaderRendererID, static_cast<u64>(cmd.shaderHandle));
    }

    void CommandPacketDebugger::RenderPODRenderStateDetail(const PODRenderState& state)
    {
        if (ImGui::TreeNode("Blend"))
        {
            ImGui::Text("Enabled: %s", state.blendEnabled ? "Yes" : "No");
            if (state.blendEnabled)
            {
                ImGui::Text("Src Factor: 0x%X", state.blendSrcFactor);
                ImGui::Text("Dst Factor: 0x%X", state.blendDstFactor);
                ImGui::Text("Equation: 0x%X", state.blendEquation);
            }
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Depth"))
        {
            ImGui::Text("Test: %s", state.depthTestEnabled ? "Yes" : "No");
            ImGui::Text("Write: %s", state.depthWriteMask ? "Yes" : "No");
            ImGui::Text("Function: 0x%X", state.depthFunction);
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Stencil"))
        {
            ImGui::Text("Enabled: %s", state.stencilEnabled ? "Yes" : "No");
            if (state.stencilEnabled)
            {
                ImGui::Text("Function: 0x%X, Ref: %d, Mask: 0x%X", state.stencilFunction, state.stencilReference, state.stencilReadMask);
                ImGui::Text("Fail: 0x%X, DepthFail: 0x%X, Pass: 0x%X", state.stencilFail, state.stencilDepthFail, state.stencilDepthPass);
            }
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Culling"))
        {
            ImGui::Text("Enabled: %s", state.cullingEnabled ? "Yes" : "No");
            ImGui::Text("Face: 0x%X", state.cullFace);
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Polygon"))
        {
            ImGui::Text("Mode: 0x%X (Face: 0x%X)", state.polygonMode, state.polygonFace);
            ImGui::Text("Offset: %s (factor=%.2f, units=%.2f)", state.polygonOffsetEnabled ? "Yes" : "No", state.polygonOffsetFactor, state.polygonOffsetUnits);
            ImGui::Text("Line Width: %.1f", state.lineWidth);
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Scissor"))
        {
            ImGui::Text("Enabled: %s", state.scissorEnabled ? "Yes" : "No");
            if (state.scissorEnabled)
                ImGui::Text("Rect: (%d, %d, %d, %d)", state.scissorX, state.scissorY, state.scissorWidth, state.scissorHeight);
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Other"))
        {
            ImGui::Text("Color Mask: R=%s G=%s B=%s A=%s",
                        state.colorMaskR ? "Y" : "N", state.colorMaskG ? "Y" : "N",
                        state.colorMaskB ? "Y" : "N", state.colorMaskA ? "Y" : "N");
            ImGui::Text("Multisampling: %s", state.multisamplingEnabled ? "Yes" : "No");
            ImGui::TreePop();
        }
    }

    // ========================================================================
    // Sort Analysis Tab
    // ========================================================================

    void CommandPacketDebugger::RenderSortAnalysis(const CapturedFrameData* frame)
    {
        if (!frame)
        {
            ImGui::TextColored(DebugUtils::Colors::Warning, "No frame selected.");
            return;
        }

        const auto& pre = frame->PreSortCommands;
        const auto& post = frame->PostSortCommands;

        if (pre.empty() || post.empty())
        {
            ImGui::TextColored(DebugUtils::Colors::Warning, "Insufficient data for sort analysis.");
            return;
        }

        ImGui::Text("Sort Time: %.3f ms | Commands: %zu -> %zu",
                    frame->Stats.SortTimeMs, pre.size(), post.size());
        ImGui::Separator();

        // Sort displacement metric
        f64 totalDisplacement = 0.0;
        u32 maxDisplacement = 0;

        for (u32 postIdx = 0; postIdx < static_cast<u32>(post.size()); ++postIdx)
        {
            u32 origIdx = post[postIdx].GetOriginalIndex();
            u32 displacement = (origIdx > postIdx) ? origIdx - postIdx : postIdx - origIdx;
            totalDisplacement += displacement;
            maxDisplacement = std::max(maxDisplacement, displacement);
        }

        f64 avgDisplacement = post.empty() ? 0.0 : totalDisplacement / post.size();
        ImGui::Text("Avg Sort Displacement: %.1f positions", avgDisplacement);
        ImGui::Text("Max Sort Displacement: %u positions", maxDisplacement);
        ImGui::Separator();

        // Side-by-side view
        f32 halfWidth = ImGui::GetContentRegionAvail().x * 0.5f - 5.0f;

        // Pre-sort column
        ImGui::BeginChild("PreSort", ImVec2(halfWidth, 0), ImGuiChildFlags_Borders);
        ImGui::TextColored(DebugUtils::Colors::Info, "Pre-Sort (Submission Order)");
        ImGui::Separator();

        for (u32 i = 0; i < static_cast<u32>(pre.size()); ++i)
        {
            const auto& cmd = pre[i];
            ImGui::TextColored(GetColorForCommandType(cmd.GetCommandType()),
                               "%3u: %s [S=%u M=%u D=%u]", i, cmd.GetCommandTypeString(),
                               cmd.GetSortKey().GetShaderID(), cmd.GetSortKey().GetMaterialID(), cmd.GetSortKey().GetDepth());
        }

        ImGui::EndChild();
        ImGui::SameLine();

        // Post-sort column
        ImGui::BeginChild("PostSort", ImVec2(halfWidth, 0), ImGuiChildFlags_Borders);
        ImGui::TextColored(DebugUtils::Colors::Info, "Post-Sort (Execution Order)");
        ImGui::Separator();

        for (u32 i = 0; i < static_cast<u32>(post.size()); ++i)
        {
            const auto& cmd = post[i];
            ImGui::TextColored(GetColorForCommandType(cmd.GetCommandType()),
                               "%3u: %s [S=%u M=%u D=%u]", i, cmd.GetCommandTypeString(),
                               cmd.GetSortKey().GetShaderID(), cmd.GetSortKey().GetMaterialID(), cmd.GetSortKey().GetDepth());
        }

        ImGui::EndChild();
    }

    // ========================================================================
    // State Change Delta Tab
    // ========================================================================

    void CommandPacketDebugger::RenderStateChanges(const CapturedFrameData* frame)
    {
        if (!frame)
        {
            ImGui::TextColored(DebugUtils::Colors::Warning, "No frame selected.");
            return;
        }

        // Use post-sort or post-batch
        const auto& commands = !frame->PostBatchCommands.empty() ? frame->PostBatchCommands : frame->PostSortCommands;

        if (commands.size() < 2)
        {
            ImGui::TextColored(DebugUtils::Colors::Warning, "Need at least 2 commands for delta analysis.");
            return;
        }

        // Analyze consecutive commands
        u32 shaderChanges = 0;
        u32 materialChanges = 0;
        u32 blendChanges = 0;
        u32 depthChanges = 0;
        u32 polygonChanges = 0;

        struct StateChangeEntry
        {
            u32 fromIndex;
            u32 toIndex;
            std::string description;
        };
        std::vector<StateChangeEntry> changeLog;

        for (u32 i = 1; i < static_cast<u32>(commands.size()); ++i)
        {
            const auto& prev = commands[i - 1];
            const auto& curr = commands[i];

            // Shader change (from DrawKey)
            if (prev.GetSortKey().GetShaderID() != curr.GetSortKey().GetShaderID())
            {
                shaderChanges++;
                changeLog.push_back({ i - 1, i,
                                      "Shader: " + std::to_string(prev.GetSortKey().GetShaderID()) + " -> " + std::to_string(curr.GetSortKey().GetShaderID()) });
            }

            // Material change (from DrawKey)
            if (prev.GetSortKey().GetMaterialID() != curr.GetSortKey().GetMaterialID())
            {
                materialChanges++;
            }

            // Compare PODRenderState if both are DrawMesh commands
            if (prev.GetCommandType() == CommandType::DrawMesh && curr.GetCommandType() == CommandType::DrawMesh)
            {
                const auto* prevCmd = prev.GetCommandData<DrawMeshCommand>();
                const auto* currCmd = curr.GetCommandData<DrawMeshCommand>();
                if (prevCmd && currCmd)
                {
                    if (prevCmd->renderState.blendEnabled != currCmd->renderState.blendEnabled ||
                        prevCmd->renderState.blendSrcFactor != currCmd->renderState.blendSrcFactor ||
                        prevCmd->renderState.blendDstFactor != currCmd->renderState.blendDstFactor)
                    {
                        blendChanges++;
                    }
                    if (prevCmd->renderState.depthTestEnabled != currCmd->renderState.depthTestEnabled ||
                        prevCmd->renderState.depthFunction != currCmd->renderState.depthFunction)
                    {
                        depthChanges++;
                    }
                    if (prevCmd->renderState.polygonMode != currCmd->renderState.polygonMode ||
                        prevCmd->renderState.cullingEnabled != currCmd->renderState.cullingEnabled)
                    {
                        polygonChanges++;
                    }
                }
            }
        }

        u32 totalRenderStateChanges = blendChanges + depthChanges + polygonChanges;

        // Summary
        ImGui::Text("State Change Summary (%zu commands):", commands.size());
        ImGui::Separator();
        ImGui::Text("Shader Binds: %u", shaderChanges);
        ImGui::Text("Material Changes: %u", materialChanges);
        ImGui::Text("Blend State Changes: %u", blendChanges);
        ImGui::Text("Depth State Changes: %u", depthChanges);
        ImGui::Text("Polygon State Changes: %u", polygonChanges);
        ImGui::Text("Total Render State Changes: %u", totalRenderStateChanges);

        // Efficiency metric
        if (commands.size() > 1)
        {
            f32 efficiency = 1.0f - (static_cast<f32>(shaderChanges) / static_cast<f32>(commands.size() - 1));
            ImVec4 color = DebugUtils::GetPerformanceColor((1.0f - efficiency) * 100.0f, 70.0f, 40.0f);
            ImGui::TextColored(color, "Shader Coherence: %.1f%%", efficiency * 100.0f);
        }

        ImGui::Separator();

        // Change log (scrollable)
        if (!changeLog.empty())
        {
            ImGui::Text("Change Log:");
            ImGui::BeginChild("ChangeLog", ImVec2(0, 200), ImGuiChildFlags_Borders);
            for (const auto& entry : changeLog)
            {
                ImGui::Text("[%u -> %u] %s", entry.fromIndex, entry.toIndex, entry.description.c_str());
            }
            ImGui::EndChild();
        }
    }

    // ========================================================================
    // Batching Analysis Tab
    // ========================================================================

    void CommandPacketDebugger::RenderBatchingAnalysis(const CapturedFrameData* frame)
    {
        if (!frame)
        {
            ImGui::TextColored(DebugUtils::Colors::Warning, "No frame selected.");
            return;
        }

        const auto& preBatch = frame->PostSortCommands;
        const auto& postBatch = frame->PostBatchCommands;

        ImGui::Text("Batching Analysis");
        ImGui::Separator();

        if (postBatch.empty())
        {
            ImGui::TextColored(DebugUtils::Colors::Warning, "No post-batch data (batching may be disabled).");
            ImGui::Text("Pre-batch commands: %zu", preBatch.size());

            // Show missed batch opportunities
            if (preBatch.size() >= 2)
            {
                u32 missedBatches = 0;
                for (u32 i = 1; i < static_cast<u32>(preBatch.size()); ++i)
                {
                    const auto& prev = preBatch[i - 1];
                    const auto& curr = preBatch[i];
                    if (prev.GetCommandType() == CommandType::DrawMesh &&
                        curr.GetCommandType() == CommandType::DrawMesh &&
                        prev.GetSortKey().GetShaderID() == curr.GetSortKey().GetShaderID() &&
                        prev.GetSortKey().GetMaterialID() == curr.GetSortKey().GetMaterialID())
                    {
                        missedBatches++;
                    }
                }
                ImGui::Text("Potential batch merges (same shader+material): %u", missedBatches);
            }
            return;
        }

        i32 merged = static_cast<i32>(preBatch.size()) - static_cast<i32>(postBatch.size());
        ImGui::Text("Pre-batch: %zu commands", preBatch.size());
        ImGui::Text("Post-batch: %zu commands", postBatch.size());
        ImGui::Text("Merged: %d commands", merged > 0 ? merged : 0);

        if (!preBatch.empty())
        {
            f32 ratio = static_cast<f32>(postBatch.size()) / static_cast<f32>(preBatch.size());
            ImGui::Text("Batch Ratio: %.1f%%", ratio * 100.0f);
        }

        ImGui::Separator();

        // Show instanced commands in post-batch
        u32 instancedCount = 0;
        u32 totalInstances = 0;
        for (const auto& cmd : postBatch)
        {
            if (cmd.GetCommandType() == CommandType::DrawMeshInstanced)
            {
                instancedCount++;
                if (const auto* instCmd = cmd.GetCommandData<DrawMeshInstancedCommand>())
                    totalInstances += instCmd->instanceCount;
            }
        }

        if (instancedCount > 0)
        {
            ImGui::Text("Instanced Draw Calls: %u", instancedCount);
            ImGui::Text("Total Instances: %u", totalInstances);
            ImGui::Text("Avg Instances/Call: %.1f", instancedCount > 0 ? static_cast<f32>(totalInstances) / instancedCount : 0.0f);
        }
    }

    // ========================================================================
    // Timeline Tab
    // ========================================================================

    void CommandPacketDebugger::RenderTimeline(const CapturedFrameData* frame)
    {
        if (!frame)
        {
            ImGui::TextColored(DebugUtils::Colors::Warning, "No frame selected.");
            return;
        }

        const auto& commands = !frame->PostBatchCommands.empty() ? frame->PostBatchCommands : frame->PostSortCommands;

        if (commands.empty())
        {
            ImGui::TextColored(DebugUtils::Colors::Warning, "No commands to display.");
            return;
        }

        ImGui::Text("Frame #%u Timeline (%zu commands)", frame->FrameNumber, commands.size());
        ImGui::Separator();

        // Check if GPU timing data is available
        bool hasGpuTiming = false;
        f64 totalGpuTime = 0.0;
        for (const auto& cmd : commands)
        {
            if (cmd.GetGpuTimeMs() > 0.0)
            {
                hasGpuTiming = true;
                totalGpuTime += cmd.GetGpuTimeMs();
            }
        }

        if (hasGpuTiming)
            ImGui::Text("Total GPU Time: %.3f ms", totalGpuTime);
        else
            ImGui::TextColored(DebugUtils::Colors::Disabled, "No GPU timing data (enable GPU timer capture)");

        ImGui::Separator();

        // Timeline rendering using ImGui drawing
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImVec2 canvasSize = ImGui::GetContentRegionAvail();
        canvasSize.y = std::max(canvasSize.y, 120.0f);

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                                IM_COL32(30, 30, 30, 255));

        f32 barHeight = 20.0f;
        f32 padding = 2.0f;
        f32 totalWidth = canvasSize.x - padding * 2.0f;

        // Calculate bar widths
        f32 uniformWidth = totalWidth / static_cast<f32>(commands.size());

        f32 xOffset = canvasPos.x + padding;
        f32 yOffset = canvasPos.y + padding;

        for (u32 i = 0; i < static_cast<u32>(commands.size()); ++i)
        {
            const auto& cmd = commands[i];

            f32 barWidth = hasGpuTiming && totalGpuTime > 0.0
                               ? static_cast<f32>(cmd.GetGpuTimeMs() / totalGpuTime) * totalWidth
                               : uniformWidth;

            barWidth = std::max(barWidth, 1.0f);

            ImVec4 color = GetColorForCommandType(cmd.GetCommandType());
            ImU32 col = ImGui::GetColorU32(color);

            f32 x0 = xOffset;
            f32 y0 = yOffset;
            f32 x1 = xOffset + barWidth - 1.0f;
            f32 y1 = yOffset + barHeight;

            drawList->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), col);

            // Highlight selected
            if (i == static_cast<u32>(m_SelectedCommandIndex))
            {
                drawList->AddRect(ImVec2(x0 - 1, y0 - 1), ImVec2(x1 + 1, y1 + 1), IM_COL32(255, 255, 255, 255), 0.0f, 0, 2.0f);
            }

            // Hover tooltip and click to select
            ImGui::SetCursorScreenPos(ImVec2(x0, y0));
            char tlId[16];
            snprintf(tlId, sizeof(tlId), "##tl%u", i);
            ImGui::InvisibleButton(tlId, ImVec2(barWidth, barHeight));
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::Text("#%u: %s", i, cmd.GetCommandTypeString());
                ImGui::Text("Shader: %u, Material: %u", cmd.GetSortKey().GetShaderID(), cmd.GetSortKey().GetMaterialID());
                if (cmd.GetGpuTimeMs() > 0.0)
                    ImGui::Text("GPU: %.4f ms", cmd.GetGpuTimeMs());
                ImGui::EndTooltip();
            }
            if (ImGui::IsItemClicked())
                m_SelectedCommandIndex = static_cast<i32>(i);

            xOffset += barWidth;
        }

        // Legend below timeline
        ImGui::SetCursorScreenPos(ImVec2(canvasPos.x, canvasPos.y + barHeight + padding * 3));
        ImGui::Dummy(ImVec2(canvasSize.x, barHeight + padding * 4));

        ImGui::Text("Legend:");
        ImGui::SameLine();
        ImGui::TextColored(GetColorForCommandType(CommandType::DrawMesh), "Draw");
        ImGui::SameLine();
        ImGui::TextColored(GetColorForCommandType(CommandType::Clear), "Clear");
        ImGui::SameLine();
        ImGui::TextColored(GetColorForCommandType(CommandType::SetViewport), "State");
        ImGui::SameLine();
        ImGui::TextColored(GetColorForCommandType(CommandType::BindTexture), "Bind");
    }

    // ========================================================================
    // Live View (fallback when no captures exist)
    // ========================================================================

    void CommandPacketDebugger::RenderLiveView(const CommandBucket* bucket)
    {
        ImGui::TextColored(DebugUtils::Colors::Info,
                           "No captured frames. Use the toolbar above to capture frames for analysis.");
        ImGui::Separator();

        if (!bucket)
        {
            ImGui::TextColored(DebugUtils::Colors::Warning, "No command bucket available.");
            return;
        }

        sizet cmdCount = bucket->GetCommandCount();
        bool isSorted = bucket->IsSorted();

        ImGui::Text("Live Bucket: %zu commands, Sorted: %s", cmdCount, isSorted ? "Yes" : "No");
        ImGui::Text("Last Sort: %.3f ms | Last Execute: %.3f ms",
                    bucket->GetLastSortTimeMs(), bucket->GetLastExecuteTimeMs());

        auto stats = bucket->GetStatistics();
        ImGui::Text("Draw Calls: %u | State Changes: %u | Batched: %u",
                    stats.DrawCalls, stats.StateChanges, stats.BatchedCommands);
    }

    // ========================================================================
    // Helpers
    // ========================================================================

    ImVec4 CommandPacketDebugger::GetColorForCommandType(CommandType type)
    {
        switch (type)
        {
            case CommandType::DrawMesh:
            case CommandType::DrawMeshInstanced:
            case CommandType::DrawQuad:
            case CommandType::DrawIndexed:
            case CommandType::DrawArrays:
            case CommandType::DrawLines:
            case CommandType::DrawSkybox:
            case CommandType::DrawInfiniteGrid:
            case CommandType::DrawIndexedInstanced:
                return ImVec4(0.3f, 0.8f, 0.3f, 1.0f); // Green

            case CommandType::Clear:
            case CommandType::ClearStencil:
                return ImVec4(0.8f, 0.3f, 0.3f, 1.0f); // Red

            case CommandType::BindTexture:
            case CommandType::BindDefaultFramebuffer:
                return ImVec4(0.8f, 0.8f, 0.3f, 1.0f); // Yellow

            case CommandType::SetViewport:
            case CommandType::SetClearColor:
            case CommandType::SetBlendState:
            case CommandType::SetDepthTest:
            case CommandType::SetDepthMask:
            case CommandType::SetDepthFunc:
            case CommandType::SetStencilTest:
            case CommandType::SetCulling:
            case CommandType::SetPolygonMode:
                return ImVec4(0.3f, 0.3f, 0.8f, 1.0f); // Blue

            default:
                return ImVec4(0.8f, 0.5f, 0.3f, 1.0f); // Orange
        }
    }

    // ========================================================================
    // CSV Export
    // ========================================================================

    std::string CommandPacketDebugger::GenerateExportFilename(const char* extension, u32 frameNumber)
    {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &time);
#else
        localtime_r(&time, &tm);
#endif

        std::ostringstream oss;
        oss << "cmd_bucket_frame" << frameNumber
            << "_" << std::put_time(&tm, "%Y%m%d_%H%M%S")
            << "." << extension;
        return oss.str();
    }

    bool CommandPacketDebugger::ExportToCSV(const std::string& outputPath) const
    {
        OLO_PROFILE_FUNCTION();

        auto& captureManager = FrameCaptureManager::GetInstance();
        auto selectedFrame = captureManager.GetSelectedFrame();

        if (!selectedFrame)
        {
            OLO_CORE_ERROR("Cannot export CSV: no frame selected");
            return false;
        }

        try
        {
            std::ofstream file(outputPath);
            if (!file.is_open())
                return false;

            file << "Index,Type,DrawKey,ViewportID,ViewLayer,RenderMode,MaterialID,ShaderID,Depth,Static,GroupID,DebugName,GpuTimeMs\n";

            const auto& commands = !selectedFrame->PostSortCommands.empty() ? selectedFrame->PostSortCommands : selectedFrame->PreSortCommands;

            for (sizet i = 0; i < commands.size(); ++i)
            {
                const auto& cmd = commands[i];
                const DrawKey& key = cmd.GetSortKey();

                file << i << ","
                     << EscapeCsvField(cmd.GetCommandTypeString()) << ","
                     << "0x" << std::hex << key.GetKey() << std::dec << ","
                     << key.GetViewportID() << ","
                     << EscapeCsvField(ToString(key.GetViewLayer())) << ","
                     << EscapeCsvField(ToString(key.GetRenderMode())) << ","
                     << key.GetMaterialID() << ","
                     << key.GetShaderID() << ","
                     << key.GetDepth() << ","
                     << (cmd.IsStatic() ? "true" : "false") << ","
                     << cmd.GetGroupID() << ","
                     << EscapeCsvField(cmd.GetDebugName().empty() ? std::string("None") : cmd.GetDebugName()) << ","
                     << cmd.GetGpuTimeMs() << "\n";
            }

            file.close();
            OLO_CORE_INFO("Command packet data exported to: {}", outputPath);
            return true;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("Failed to export command packet data: {}", e.what());
            return false;
        }
    }

    // ========================================================================
    // Markdown / LLM Analysis Export
    // ========================================================================

    bool CommandPacketDebugger::ExportToMarkdown(const std::string& outputPath) const
    {
        OLO_PROFILE_FUNCTION();

        auto& captureManager = FrameCaptureManager::GetInstance();
        auto selectedFrame = captureManager.GetSelectedFrame();

        if (!selectedFrame)
        {
            OLO_CORE_ERROR("Cannot export Markdown: no frame selected");
            return false;
        }

        const auto* frame = &*selectedFrame;

        try
        {
            std::ofstream file(outputPath);
            if (!file.is_open())
                return false;

            // Header
            file << "# Command Bucket Frame Capture Report\n\n";
            file << "## Frame Info\n\n";
            file << "- **Frame Number:** " << frame->FrameNumber << "\n";
            file << "- **Timestamp:** " << std::fixed << std::setprecision(3) << frame->TimestampSeconds << "s\n";
            file << "- **Total Commands (pre-sort):** " << frame->PreSortCommands.size() << "\n";
            file << "- **Total Commands (post-sort):** " << frame->PostSortCommands.size() << "\n";
            file << "- **Total Commands (post-batch):** " << frame->PostBatchCommands.size() << "\n";
            if (!frame->Notes.empty())
                file << "- **Notes:** " << frame->Notes << "\n";
            file << "\n";

            // Pipeline statistics
            file << "## Pipeline Statistics\n\n";
            file << "| Metric | Value |\n";
            file << "|--------|-------|\n";
            file << "| Sort Time | " << std::fixed << std::setprecision(3) << frame->Stats.SortTimeMs << " ms |\n";
            file << "| Batch Time | " << frame->Stats.BatchTimeMs << " ms |\n";
            file << "| Execute Time | " << frame->Stats.ExecuteTimeMs << " ms |\n";
            file << "| Total Frame Time | " << frame->Stats.TotalFrameTimeMs << " ms |\n";
            file << "| Draw Calls | " << frame->Stats.DrawCalls << " |\n";
            file << "| State Changes | " << frame->Stats.StateChanges << " |\n";
            file << "| Shader Binds | " << frame->Stats.ShaderBinds << " |\n";
            file << "| Texture Binds | " << frame->Stats.TextureBinds << " |\n";
            file << "| Batched Commands | " << frame->Stats.BatchedCommands << " |\n";
            file << "\n";

            // Command list (post-sort is the most useful for analysis)
            const auto& commands = !frame->PostSortCommands.empty() ? frame->PostSortCommands : frame->PreSortCommands;

            file << "## Command List (Post-Sort Order)\n\n";
            file << "| # | Type | ShaderID | MaterialID | Depth | ViewLayer | RenderMode | Static | DebugName | GpuTimeMs |\n";
            file << "|---|------|----------|------------|-------|-----------|------------|--------|-----------|----------|\n";

            for (sizet i = 0; i < commands.size(); ++i)
            {
                const auto& cmd = commands[i];
                const DrawKey& key = cmd.GetSortKey();

                file << "| " << i
                     << " | " << cmd.GetCommandTypeString()
                     << " | " << key.GetShaderID()
                     << " | " << key.GetMaterialID()
                     << " | " << key.GetDepth()
                     << " | " << ToString(key.GetViewLayer())
                     << " | " << ToString(key.GetRenderMode())
                     << " | " << (cmd.IsStatic() ? "Yes" : "No")
                     << " | " << (cmd.GetDebugName().empty() ? "-" : cmd.GetDebugName())
                     << " | " << std::fixed << std::setprecision(4) << cmd.GetGpuTimeMs()
                     << " |\n";
            }
            file << "\n";

            // Sort analysis
            file << "## Sort Analysis\n\n";
            if (!frame->PreSortCommands.empty() && !frame->PostSortCommands.empty())
            {
                const auto& pre = frame->PreSortCommands;
                const auto& post = frame->PostSortCommands;

                f64 totalDisplacement = 0.0;
                u32 maxDisplacement = 0;
                u32 movedCount = 0;

                for (u32 postIdx = 0; postIdx < static_cast<u32>(post.size()); ++postIdx)
                {
                    u32 origIdx = post[postIdx].GetOriginalIndex();
                    u32 displacement = (origIdx > postIdx) ? origIdx - postIdx : postIdx - origIdx;
                    totalDisplacement += displacement;
                    maxDisplacement = std::max(maxDisplacement, displacement);
                    if (displacement > 0)
                        movedCount++;
                }

                f64 avgDisplacement = post.empty() ? 0.0 : totalDisplacement / post.size();
                file << "- **Commands moved:** " << movedCount << "/" << post.size() << "\n";
                file << "- **Average displacement:** " << std::fixed << std::setprecision(1) << avgDisplacement << " positions\n";
                file << "- **Max displacement:** " << maxDisplacement << " positions\n\n";
            }
            else
            {
                file << "Insufficient data for sort analysis.\n\n";
            }

            // State change analysis
            file << "## State Change Analysis\n\n";
            if (commands.size() >= 2)
            {
                u32 shaderChanges = 0;
                u32 materialChanges = 0;
                u32 blendChanges = 0;
                u32 depthChanges = 0;

                for (u32 i = 1; i < static_cast<u32>(commands.size()); ++i)
                {
                    const auto& prev = commands[i - 1];
                    const auto& curr = commands[i];

                    if (prev.GetSortKey().GetShaderID() != curr.GetSortKey().GetShaderID())
                        shaderChanges++;
                    if (prev.GetSortKey().GetMaterialID() != curr.GetSortKey().GetMaterialID())
                        materialChanges++;

                    if (prev.GetCommandType() == CommandType::DrawMesh && curr.GetCommandType() == CommandType::DrawMesh)
                    {
                        const auto* prevCmd = prev.GetCommandData<DrawMeshCommand>();
                        const auto* currCmd = curr.GetCommandData<DrawMeshCommand>();
                        if (prevCmd && currCmd)
                        {
                            if (prevCmd->renderState.blendEnabled != currCmd->renderState.blendEnabled ||
                                prevCmd->renderState.blendSrcFactor != currCmd->renderState.blendSrcFactor)
                                blendChanges++;
                            if (prevCmd->renderState.depthTestEnabled != currCmd->renderState.depthTestEnabled ||
                                prevCmd->renderState.depthFunction != currCmd->renderState.depthFunction)
                                depthChanges++;
                        }
                    }
                }

                f32 shaderCoherence = 1.0f - (static_cast<f32>(shaderChanges) / static_cast<f32>(commands.size() - 1));

                file << "| Metric | Count |\n";
                file << "|--------|-------|\n";
                file << "| Shader Changes | " << shaderChanges << " |\n";
                file << "| Material Changes | " << materialChanges << " |\n";
                file << "| Blend State Changes | " << blendChanges << " |\n";
                file << "| Depth State Changes | " << depthChanges << " |\n";
                file << "| **Shader Coherence** | **" << std::fixed << std::setprecision(1) << (shaderCoherence * 100.0f) << "%** |\n\n";
            }

            // Batching analysis
            file << "## Batching Analysis\n\n";
            if (!frame->PostBatchCommands.empty())
            {
                i32 merged = static_cast<i32>(frame->PostSortCommands.size()) - static_cast<i32>(frame->PostBatchCommands.size());
                f32 batchRatio = frame->PostSortCommands.empty() ? 1.0f
                                                                 : static_cast<f32>(frame->PostBatchCommands.size()) / static_cast<f32>(frame->PostSortCommands.size());

                file << "- **Pre-batch commands:** " << frame->PostSortCommands.size() << "\n";
                file << "- **Post-batch commands:** " << frame->PostBatchCommands.size() << "\n";
                file << "- **Merged:** " << (merged > 0 ? merged : 0) << " commands\n";
                file << "- **Batch ratio:** " << std::fixed << std::setprecision(1) << (batchRatio * 100.0f) << "%\n\n";
            }
            else
            {
                file << "No post-batch data available (batching may be disabled).\n\n";

                // Count potential batch merges
                u32 potentialMerges = 0;
                for (u32 i = 1; i < static_cast<u32>(commands.size()); ++i)
                {
                    const auto& prev = commands[i - 1];
                    const auto& curr = commands[i];
                    if (prev.GetCommandType() == CommandType::DrawMesh &&
                        curr.GetCommandType() == CommandType::DrawMesh &&
                        prev.GetSortKey().GetShaderID() == curr.GetSortKey().GetShaderID() &&
                        prev.GetSortKey().GetMaterialID() == curr.GetSortKey().GetMaterialID())
                    {
                        potentialMerges++;
                    }
                }
                if (potentialMerges > 0)
                    file << "- **Potential batch merges** (same shader+material): " << potentialMerges << "\n\n";
            }

            // Draw command material summary
            file << "## Draw Command Details\n\n";
            u32 drawIdx = 0;
            for (const auto& cmd : commands)
            {
                if (cmd.GetCommandType() == CommandType::DrawMesh)
                {
                    if (const auto* meshCmd = cmd.GetCommandData<DrawMeshCommand>())
                    {
                        file << "### Draw #" << drawIdx++ << ": " << cmd.GetCommandTypeString() << "\n\n";
                        file << "- Shader: " << meshCmd->shaderRendererID << " (handle: " << static_cast<u64>(meshCmd->shaderHandle) << ")\n";
                        file << "- VAO: " << meshCmd->vertexArrayID << ", Index Count: " << meshCmd->indexCount << "\n";
                        file << "- Entity ID: " << meshCmd->entityID << "\n";
                        if (meshCmd->enablePBR)
                        {
                            file << "- PBR Material: baseColor=(" << meshCmd->baseColorFactor.r << "," << meshCmd->baseColorFactor.g << "," << meshCmd->baseColorFactor.b << ")"
                                 << " metallic=" << meshCmd->metallicFactor << " roughness=" << meshCmd->roughnessFactor << "\n";
                            file << "- Textures: albedo=" << meshCmd->albedoMapID << " metallicRough=" << meshCmd->metallicRoughnessMapID
                                 << " normal=" << meshCmd->normalMapID << " ao=" << meshCmd->aoMapID << " emissive=" << meshCmd->emissiveMapID << "\n";
                        }
                        file << "- Depth: write=" << (meshCmd->renderState.depthWriteMask ? "yes" : "no")
                             << " test=" << (meshCmd->renderState.depthTestEnabled ? "yes" : "no") << "\n";
                        file << "- Blend: " << (meshCmd->renderState.blendEnabled ? "enabled" : "disabled") << "\n";
                        if (meshCmd->isAnimatedMesh)
                            file << "- Animated: boneOffset=" << meshCmd->boneBufferOffset << " boneCount=" << meshCmd->boneCount << "\n";
                        file << "\n";
                    }
                }
                else if (cmd.GetCommandType() == CommandType::DrawMeshInstanced)
                {
                    if (const auto* instCmd = cmd.GetCommandData<DrawMeshInstancedCommand>())
                    {
                        file << "### Draw #" << drawIdx++ << ": " << cmd.GetCommandTypeString() << "\n\n";
                        file << "- Instances: " << instCmd->instanceCount << "\n";
                        file << "- Shader: " << instCmd->shaderRendererID << "\n";
                        file << "- VAO: " << instCmd->vertexArrayID << ", Index Count: " << instCmd->indexCount << "\n\n";
                    }
                }
            }

            // GPU timing section
            bool hasGpuTiming = false;
            f64 totalGpuTime = 0.0;
            for (const auto& cmd : commands)
            {
                if (cmd.GetGpuTimeMs() > 0.0)
                {
                    hasGpuTiming = true;
                    totalGpuTime += cmd.GetGpuTimeMs();
                }
            }

            if (hasGpuTiming)
            {
                file << "## GPU Timing\n\n";
                file << "- **Total GPU Time:** " << std::fixed << std::setprecision(3) << totalGpuTime << " ms\n\n";

                file << "| # | Type | GPU Time (ms) | % of Total |\n";
                file << "|---|------|--------------|------------|\n";
                for (sizet i = 0; i < commands.size(); ++i)
                {
                    const auto& cmd = commands[i];
                    if (cmd.GetGpuTimeMs() > 0.0)
                    {
                        f64 pct = (totalGpuTime > 0.0) ? (cmd.GetGpuTimeMs() / totalGpuTime * 100.0) : 0.0;
                        file << "| " << i << " | " << cmd.GetCommandTypeString()
                             << " | " << std::fixed << std::setprecision(4) << cmd.GetGpuTimeMs()
                             << " | " << std::setprecision(1) << pct << "% |\n";
                    }
                }
                file << "\n";
            }

            // Auto-generated optimization suggestions
            file << "## Optimization Suggestions\n\n";
            file << "The following are auto-detected observations. An LLM or engineer should review these in context.\n\n";

            bool hasSuggestions = false;

            // High shader change ratio
            if (commands.size() >= 2)
            {
                u32 shaderChanges = 0;
                for (u32 i = 1; i < static_cast<u32>(commands.size()); ++i)
                {
                    if (commands[i - 1].GetSortKey().GetShaderID() != commands[i].GetSortKey().GetShaderID())
                        shaderChanges++;
                }
                f32 changeRatio = static_cast<f32>(shaderChanges) / static_cast<f32>(commands.size() - 1);
                if (changeRatio > 0.5f)
                {
                    file << "- **High shader change frequency** (" << std::fixed << std::setprecision(0) << (changeRatio * 100.0f) << "%): "
                         << "Consider grouping objects by shader/material to reduce GPU pipeline flushes.\n";
                    hasSuggestions = true;
                }
            }

            // Many unique materials with same shader
            {
                std::unordered_map<u32, std::set<u32>> shaderToMaterials;
                for (const auto& cmd : commands)
                {
                    if (cmd.IsDrawCommand())
                        shaderToMaterials[cmd.GetSortKey().GetShaderID()].insert(cmd.GetSortKey().GetMaterialID());
                }
                for (const auto& [shaderId, materials] : shaderToMaterials)
                {
                    if (materials.size() > 10)
                    {
                        file << "- **Shader " << shaderId << " has " << materials.size() << " distinct materials**: "
                             << "Consider using texture atlases or material instancing to reduce bind calls.\n";
                        hasSuggestions = true;
                    }
                }
            }

            // Low batch merge rate
            if (!frame->PostBatchCommands.empty() && !frame->PostSortCommands.empty())
            {
                f32 batchRatio = static_cast<f32>(frame->PostBatchCommands.size()) / static_cast<f32>(frame->PostSortCommands.size());
                if (batchRatio > 0.95f && frame->PostSortCommands.size() > 10)
                {
                    file << "- **Low batch merge rate** (" << std::fixed << std::setprecision(1) << ((1.0f - batchRatio) * 100.0f) << "% merged): "
                         << "Check if meshes with the same shader/material could share vertex buffers for instancing.\n";
                    hasSuggestions = true;
                }
            }

            // Many small draw calls
            {
                u32 smallDraws = 0;
                for (const auto& cmd : commands)
                {
                    if (cmd.GetCommandType() == CommandType::DrawMesh)
                    {
                        if (const auto* meshCmd = cmd.GetCommandData<DrawMeshCommand>())
                        {
                            if (meshCmd->indexCount < 100)
                                smallDraws++;
                        }
                    }
                }
                if (smallDraws > 5)
                {
                    file << "- **" << smallDraws << " draw calls with <100 indices**: "
                         << "Consider merging small meshes or using instanced rendering.\n";
                    hasSuggestions = true;
                }
            }

            if (!hasSuggestions)
                file << "No obvious optimization issues detected.\n";

            file << "\n---\n*Generated by OloEngine Command Bucket Inspector*\n";

            file.close();
            OLO_CORE_INFO("Command packet analysis exported to: {}", outputPath);
            return true;
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("Failed to export Markdown analysis: {}", e.what());
            return false;
        }
    }
} // namespace OloEngine

#include "OloEnginePCH.h"
#include "CommandPacketDebugger.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Renderer/Commands/DrawKey.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <cmath>

namespace OloEngine
{
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
                if (ImGui::MenuItem("Export to CSV"))
                    ExportToCSV("command_bucket_capture.csv");
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        // Recording toolbar (always visible)
        RenderRecordingToolbar();
        ImGui::Separator();

        auto& captureManager = FrameCaptureManager::GetInstance();
        const CapturedFrameData* selectedFrame = captureManager.GetSelectedFrame();

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
                    RenderCommandList(selectedFrame, bucket);
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Sort Analysis"))
                {
                    RenderSortAnalysis(selectedFrame);
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("State Changes"))
                {
                    RenderStateChanges(selectedFrame);
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Batching"))
                {
                    RenderBatchingAnalysis(selectedFrame);
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("Timeline"))
                {
                    RenderTimeline(selectedFrame);
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
        const auto& frames = captureManager.GetCapturedFrames();
        i32 selectedIdx = captureManager.GetSelectedFrameIndex();

        ImGui::Text("Frames:");
        ImGui::SameLine();

        // Horizontal scrolling frame list
        ImGui::BeginChild("FrameList", ImVec2(0, 50), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);

        for (i32 i = 0; i < static_cast<i32>(frames.size()); ++i)
        {
            const auto& frame = frames[i];
            bool isSelected = (i == selectedIdx);

            ImGui::PushID(i);
            char label[64];
            snprintf(label, sizeof(label), "#%u\n%u cmds\n%.2fms",
                     frame.FrameNumber,
                     frame.Stats.TotalCommands,
                     frame.Stats.TotalFrameTimeMs);

            if (isSelected)
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.3f, 0.5f, 0.8f, 1.0f));

            if (ImGui::Button(label, ImVec2(80, 40)))
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
        ImGui::RadioButton("Pre-Sort", &m_CommandViewMode, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Post-Sort", &m_CommandViewMode, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Post-Batch", &m_CommandViewMode, 2);

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
            case 0: commands = &frame->PreSortCommands; break;
            case 1: commands = &frame->PostSortCommands; break;
            case 2: commands = &frame->PostBatchCommands; break;
            default: commands = &frame->PostSortCommands; break;
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
                        case 0: pass = cmd.IsDrawCommand(); break;
                        case 1: pass = (cmd.GetCommandType() == CommandType::Clear || cmd.GetCommandType() == CommandType::ClearStencil); break;
                        case 2: pass = cmd.IsStateCommand(); break;
                        case 3: pass = !cmd.IsDrawCommand() && !cmd.IsStateCommand(); break;
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
        if (cmd.GetCommandType() == CommandType::DrawMesh ||
            cmd.GetCommandType() == CommandType::DrawMeshInstanced ||
            cmd.GetCommandType() == CommandType::DrawSkybox ||
            cmd.GetCommandType() == CommandType::DrawInfiniteGrid ||
            cmd.GetCommandType() == CommandType::DrawQuad)
        {
            const PODRenderState* state = nullptr;
            if (cmd.GetCommandType() == CommandType::DrawMesh)
            {
                if (const auto* c = cmd.GetCommandData<DrawMeshCommand>())
                    state = &c->renderState;
            }
            else if (cmd.GetCommandType() == CommandType::DrawMeshInstanced)
            {
                if (const auto* c = cmd.GetCommandData<DrawMeshInstancedCommand>())
                    state = &c->renderState;
            }
            else if (cmd.GetCommandType() == CommandType::DrawSkybox)
            {
                if (const auto* c = cmd.GetCommandData<DrawSkyboxCommand>())
                    state = &c->renderState;
            }
            else if (cmd.GetCommandType() == CommandType::DrawInfiniteGrid)
            {
                if (const auto* c = cmd.GetCommandData<DrawInfiniteGridCommand>())
                    state = &c->renderState;
            }
            else if (cmd.GetCommandType() == CommandType::DrawQuad)
            {
                if (const auto* c = cmd.GetCommandData<DrawQuadCommand>())
                    state = &c->renderState;
            }

            if (state && ImGui::CollapsingHeader("Render State"))
                RenderPODRenderStateDetail(*state);
        }
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
            ImVec4 color = DebugUtils::GetPerformanceColor(efficiency * 100.0f, 70.0f, 40.0f);
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
            ImGui::InvisibleButton(("##tl" + std::to_string(i)).c_str(), ImVec2(barWidth, barHeight));
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

    const char* CommandPacketDebugger::GetCommandTypeString(CommandType type)
    {
        switch (type)
        {
            case CommandType::DrawMesh: return "DrawMesh";
            case CommandType::DrawMeshInstanced: return "DrawMeshInstanced";
            case CommandType::DrawQuad: return "DrawQuad";
            case CommandType::DrawIndexed: return "DrawIndexed";
            case CommandType::DrawArrays: return "DrawArrays";
            case CommandType::DrawLines: return "DrawLines";
            case CommandType::DrawSkybox: return "DrawSkybox";
            case CommandType::DrawInfiniteGrid: return "DrawInfiniteGrid";
            case CommandType::DrawIndexedInstanced: return "DrawIndexedInstanced";
            case CommandType::Clear: return "Clear";
            case CommandType::ClearStencil: return "ClearStencil";
            case CommandType::BindTexture: return "BindTexture";
            case CommandType::BindDefaultFramebuffer: return "BindDefaultFB";
            case CommandType::SetViewport: return "SetViewport";
            case CommandType::SetClearColor: return "SetClearColor";
            case CommandType::SetBlendState: return "SetBlendState";
            case CommandType::SetDepthTest: return "SetDepthTest";
            case CommandType::SetCulling: return "SetCulling";
            case CommandType::SetPolygonMode: return "SetPolygonMode";
            default: return "Other";
        }
    }

    // ========================================================================
    // CSV Export
    // ========================================================================

    bool CommandPacketDebugger::ExportToCSV(const std::string& outputPath) const
    {
        OLO_PROFILE_FUNCTION();

        auto& captureManager = FrameCaptureManager::GetInstance();
        const auto* frame = captureManager.GetSelectedFrame();

        if (!frame)
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

            const auto& commands = !frame->PostSortCommands.empty() ? frame->PostSortCommands : frame->PreSortCommands;

            for (sizet i = 0; i < commands.size(); ++i)
            {
                const auto& cmd = commands[i];
                const DrawKey& key = cmd.GetSortKey();

                file << i << ","
                     << cmd.GetCommandTypeString() << ","
                     << "0x" << std::hex << key.GetKey() << std::dec << ","
                     << key.GetViewportID() << ","
                     << ToString(key.GetViewLayer()) << ","
                     << ToString(key.GetRenderMode()) << ","
                     << key.GetMaterialID() << ","
                     << key.GetShaderID() << ","
                     << key.GetDepth() << ","
                     << (cmd.IsStatic() ? "true" : "false") << ","
                     << cmd.GetGroupID() << ","
                     << (cmd.GetDebugName().empty() ? "None" : cmd.GetDebugName()) << ","
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
} // namespace OloEngine

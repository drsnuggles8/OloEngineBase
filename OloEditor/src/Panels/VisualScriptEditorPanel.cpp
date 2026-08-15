#include "OloEnginePCH.h"
#include "VisualScriptEditorPanel.h"

#include "OloEngine/Asset/AssetManager.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Math/Math.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptSerializer.h"
#include "OloEngine/Scripting/VisualScript/VisualScriptSystem.h"
#include "OloEngine/Asset/AssetManager/EditorAssetManager.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>

namespace OloEngine
{
    using namespace OloEngine::VisualScript;

    namespace
    {
        constexpr ImU32 kNodeBody = IM_COL32(48, 48, 56, 235);
        constexpr ImU32 kNodeBorder = IM_COL32(20, 20, 24, 255);
        constexpr ImU32 kNodeSelected = IM_COL32(255, 190, 60, 255);
        constexpr ImU32 kExecWire = IM_COL32(235, 235, 235, 235);
        constexpr ImU32 kBreakpoint = IM_COL32(230, 70, 70, 255);
        constexpr ImU32 kExecGlow = IM_COL32(120, 220, 120, 255);
        constexpr ImU32 kPausedGlow = IM_COL32(255, 120, 60, 255);

        f32 TextWidth(const std::string& text)
        {
            return ImGui::CalcTextSize(text.c_str()).x;
        }

        // A pin's clickable radius is bigger than its drawn one — a 4.5px circle
        // at 0.4x zoom is 2 screen pixels, which is not a mouse target.
        f32 PinHitRadius(f32 zoom)
        {
            return std::max(9.0f, 9.0f * zoom);
        }
    } // namespace

    //==============================================================================
    // Colours
    //==============================================================================

    ImU32 VisualScriptEditorPanel::PinColor(PinType type)
    {
        // Roughly Blueprint's palette, because anyone who has used a node editor
        // already reads these: white exec, red bool, teal int, green float,
        // yellow vector, magenta string, blue entity/asset.
        switch (type)
        {
            case PinType::Exec:
                return kExecWire;
            case PinType::Bool:
                return IM_COL32(200, 60, 60, 255);
            case PinType::Int:
                return IM_COL32(80, 200, 190, 255);
            case PinType::Float:
                return IM_COL32(120, 220, 100, 255);
            case PinType::Vec2:
            case PinType::Vec3:
            case PinType::Vec4:
                return IM_COL32(230, 200, 70, 255);
            case PinType::String:
                return IM_COL32(220, 100, 210, 255);
            case PinType::Entity:
                return IM_COL32(90, 140, 235, 255);
            case PinType::Asset:
                return IM_COL32(140, 110, 230, 255);
            case PinType::Any:
                break;
        }
        return IM_COL32(170, 170, 170, 255);
    }

    ImU32 VisualScriptEditorPanel::CategoryColor(const std::string& category)
    {
        if (category == "Events")
            return IM_COL32(150, 40, 40, 255);
        if (category == "Flow")
            return IM_COL32(70, 70, 80, 255);
        if (category == "Math" || category == "Logic" || category == "Vector")
            return IM_COL32(45, 95, 60, 255);
        if (category == "Variables")
            return IM_COL32(40, 75, 120, 255);
        if (category == "Entity" || category == "Queries")
            return IM_COL32(100, 65, 30, 255);
        if (category == "Functions")
            return IM_COL32(70, 45, 110, 255);
        if (category == "Scripting")
            return IM_COL32(30, 90, 100, 255);
        return IM_COL32(60, 60, 68, 255);
    }

    //==============================================================================
    // Asset lifecycle
    //==============================================================================

    void VisualScriptEditorPanel::NewGraph()
    {
        NodeRegistry::EnsureStandardLibrary();

        m_Asset = Ref<VisualScriptAsset>::Create();
        // A brand-new graph with no entry point can never run, and "why does
        // nothing happen" is a bad first experience — seed the one node every
        // graph needs.
        m_Asset->m_EventGraph.AddNode(std::string(NodeTypes::kOnBeginPlay), { 60.0f, 60.0f });

        m_FilePath.clear();
        m_AssetHandle = 0;
        m_ActiveFunction = -1;
        m_Selection.clear();
        m_DetailsNode = kInvalidNodeId;
        m_UndoStack.clear();
        m_RedoStack.clear();
        m_IsDirty = false;
        m_Canvas.ResetView();
        Compile();
    }

    void VisualScriptEditorPanel::LoadFrom(const std::filesystem::path& path)
    {
        NodeRegistry::EnsureStandardLibrary();

        auto asset = Ref<VisualScriptAsset>::Create();
        if (!VisualScriptSerializer::Deserialize(*asset, path))
        {
            OLO_CORE_ERROR("[VisualScriptEditor] Failed to load '{}'", path.string());
            return;
        }

        m_Asset = asset;
        m_FilePath = path;
        m_ActiveFunction = -1;
        m_Selection.clear();
        m_DetailsNode = kInvalidNodeId;
        m_UndoStack.clear();
        m_RedoStack.clear();
        m_IsDirty = false;
        Compile();
        // Rebuild once so FitView has real node extents to work with; the canvas
        // has no size until Begin() runs, so this only primes the layout.
        RebuildLayout();
    }

    void VisualScriptEditorPanel::OpenGraph(const std::filesystem::path& path)
    {
        LoadFrom(path);

        // Resolve (importing if the registry has not seen it) so hot-reload can
        // match a reload event against the graph this panel has open. A failure
        // here is not fatal — the graph still edits and saves, it just will not
        // auto-reload.
        m_AssetHandle = 0;
        if (Project::HasAssetManager())
        {
            if (auto manager = Project::GetAssetManager().As<EditorAssetManager>(); manager)
            {
                m_AssetHandle = manager->ImportAsset(path);
            }
        }
        m_IsOpen = true;
    }

    void VisualScriptEditorPanel::OpenGraph(AssetHandle handle)
    {
        auto asset = AssetManager::GetAsset<VisualScriptAsset>(handle);
        if (!asset)
        {
            OLO_CORE_ERROR("[VisualScriptEditor] Asset {} is not a visual script", static_cast<u64>(handle));
            return;
        }
        NodeRegistry::EnsureStandardLibrary();

        m_Asset = asset;
        m_AssetHandle = handle;
        m_FilePath.clear();
        if (Project::HasAssetManager())
        {
            if (auto manager = Project::GetAssetManager().As<EditorAssetManager>(); manager)
            {
                // Save writes through m_FilePath, so without this an asset opened
                // by handle would silently fall through to Save As.
                m_FilePath = Project::GetProjectDirectory() / manager->GetMetadata(handle).FilePath;
            }
        }
        m_ActiveFunction = -1;
        m_Selection.clear();
        m_DetailsNode = kInvalidNodeId;
        m_UndoStack.clear();
        m_RedoStack.clear();
        m_IsDirty = false;
        m_IsOpen = true;
        Compile();
        RebuildLayout();
    }

    void VisualScriptEditorPanel::Save()
    {
        if (!m_Asset)
        {
            return;
        }
        if (m_FilePath.empty())
        {
            SaveAs();
            return;
        }
        if (!VisualScriptSerializer::Serialize(*m_Asset, m_FilePath))
        {
            OLO_CORE_ERROR("[VisualScriptEditor] Failed to save '{}'", m_FilePath.string());
            return;
        }
        m_IsDirty = false;
        OLO_CORE_INFO("[VisualScriptEditor] Saved '{}'", m_FilePath.string());
    }

    void VisualScriptEditorPanel::SaveAs()
    {
        const std::string path = FileDialogs::SaveFile("Visual Script (*.olovs)\0*.olovs\0");
        if (path.empty())
        {
            return;
        }
        m_FilePath = path;
        if (m_FilePath.extension() != ".olovs")
        {
            m_FilePath += ".olovs";
        }
        Save();
    }

    bool VisualScriptEditorPanel::SaveIfNeeded()
    {
        if (!m_IsDirty)
        {
            return true;
        }
        Save();
        return !m_IsDirty;
    }

    void VisualScriptEditorPanel::NotifyAssetReloaded(AssetHandle handle, const std::filesystem::path& path)
    {
        if (!m_Asset || (static_cast<u64>(handle) != 0 && handle != m_AssetHandle))
        {
            return;
        }
        if (m_IsDirty)
        {
            // Reloading over unsaved edits silently destroys work. Leave the
            // in-memory graph alone and say so; the author can save (winning) or
            // reopen (losing) deliberately.
            OLO_CORE_WARN("[VisualScriptEditor] '{}' changed on disk but the open graph has unsaved edits — not reloading",
                          path.string());
            return;
        }
        LoadFrom(path.empty() ? m_FilePath : path);
    }

    void VisualScriptEditorPanel::Compile()
    {
        m_CompileErrors.clear();
        m_CompiledOk = false;
        if (!m_Asset)
        {
            return;
        }
        m_CompiledOk = VisualScriptPlan::Compile(*m_Asset, m_CompileErrors) != nullptr;
    }

    //==============================================================================
    // Undo
    //==============================================================================

    VisualScriptEditorPanel::Snapshot VisualScriptEditorPanel::CaptureSnapshot() const
    {
        Snapshot snapshot;
        if (m_Asset)
        {
            snapshot.m_EventGraph = m_Asset->m_EventGraph;
            snapshot.m_Functions = m_Asset->m_Functions;
            snapshot.m_Variables = m_Asset->m_Variables;
            snapshot.m_NodeBudgetPerTick = m_Asset->m_NodeBudgetPerTick;
        }
        return snapshot;
    }

    void VisualScriptEditorPanel::ApplySnapshot(const Snapshot& snapshot)
    {
        if (!m_Asset)
        {
            return;
        }
        m_Asset->m_EventGraph = snapshot.m_EventGraph;
        m_Asset->m_Functions = snapshot.m_Functions;
        m_Asset->m_Variables = snapshot.m_Variables;
        m_Asset->m_NodeBudgetPerTick = snapshot.m_NodeBudgetPerTick;

        // A snapshot may not contain the function the sidebar was showing.
        if (m_ActiveFunction >= static_cast<i32>(m_Asset->m_Functions.size()))
        {
            m_ActiveFunction = -1;
        }
        m_Selection.clear();
        m_DetailsNode = kInvalidNodeId;
        Compile();
    }

    void VisualScriptEditorPanel::PushUndo()
    {
        if (!m_Asset)
        {
            return;
        }
        m_UndoStack.push_back(CaptureSnapshot());
        if (m_UndoStack.size() > s_MaxUndoDepth)
        {
            m_UndoStack.erase(m_UndoStack.begin());
        }
        m_RedoStack.clear();
        m_IsDirty = true;
    }

    void VisualScriptEditorPanel::Undo()
    {
        if (m_UndoStack.empty() || !m_Asset)
        {
            return;
        }
        m_RedoStack.push_back(CaptureSnapshot());
        const Snapshot snapshot = m_UndoStack.back();
        m_UndoStack.pop_back();
        ApplySnapshot(snapshot);
        m_IsDirty = true;
    }

    void VisualScriptEditorPanel::Redo()
    {
        if (m_RedoStack.empty() || !m_Asset)
        {
            return;
        }
        m_UndoStack.push_back(CaptureSnapshot());
        const Snapshot snapshot = m_RedoStack.back();
        m_RedoStack.pop_back();
        ApplySnapshot(snapshot);
        m_IsDirty = true;
    }

    //==============================================================================
    // Layout
    //==============================================================================

    VisualScriptGraph& VisualScriptEditorPanel::ActiveGraph()
    {
        if (m_ActiveFunction >= 0 && m_ActiveFunction < static_cast<i32>(m_Asset->m_Functions.size()))
        {
            return m_Asset->m_Functions[static_cast<sizet>(m_ActiveFunction)];
        }
        return m_Asset->m_EventGraph;
    }

    const VisualScriptGraph& VisualScriptEditorPanel::ActiveGraph() const
    {
        if (m_ActiveFunction >= 0 && m_ActiveFunction < static_cast<i32>(m_Asset->m_Functions.size()))
        {
            return m_Asset->m_Functions[static_cast<sizet>(m_ActiveFunction)];
        }
        return m_Asset->m_EventGraph;
    }

    void VisualScriptEditorPanel::RebuildLayout()
    {
        m_Layout.clear();
        if (!m_Asset)
        {
            return;
        }

        const NodeRegistry& registry = NodeRegistry::Get();
        const VisualScriptGraph& graph = ActiveGraph();
        m_Layout.reserve(graph.m_Nodes.size());

        for (const VisualScriptNode& node : graph.m_Nodes)
        {
            NodeLayout layout;
            layout.m_Id = node.m_Id;
            layout.m_Type = registry.Find(node.m_TypeName);
            layout.m_Position = node.m_Position;
            if (layout.m_Type == nullptr)
            {
                // An unknown type still gets a box, so a graph saved by a newer
                // build is visible and fixable rather than invisibly missing.
                layout.m_Size = glm::vec2(s_NodeMinWidth, s_HeaderHeight + s_RowHeight);
                m_Layout.push_back(std::move(layout));
                continue;
            }

            layout.m_Pins = layout.m_Type->m_ResolvePins ? layout.m_Type->m_ResolvePins(node, *m_Asset)
                                                         : layout.m_Type->m_Pins;

            // Inputs down the left, outputs down the right, one row each. Width
            // must fit the title AND the widest input+output label pair, or long
            // pin names overlap in the middle.
            sizet inputs = 0;
            sizet outputs = 0;
            f32 widestInput = 0.0f;
            f32 widestOutput = 0.0f;
            for (const PinDescriptor& pin : layout.m_Pins)
            {
                if (pin.m_Direction == PinDirection::Input)
                {
                    widestInput = std::max(widestInput, TextWidth(pin.m_Name));
                    ++inputs;
                }
                else
                {
                    widestOutput = std::max(widestOutput, TextWidth(pin.m_Name));
                    ++outputs;
                }
            }

            const f32 titleWidth = TextWidth(layout.m_Type->m_DisplayName) + 24.0f;
            const f32 pinWidth = widestInput + widestOutput + 46.0f;
            layout.m_Size.x = std::max({ s_NodeMinWidth, titleWidth, pinWidth });
            layout.m_Size.y = s_HeaderHeight + static_cast<f32>(std::max(inputs, outputs)) * s_RowHeight + 8.0f;

            m_Layout.push_back(std::move(layout));
        }

        // Second pass for screen positions — they depend on the canvas transform,
        // which Begin() has already set by the time this runs.
        for (NodeLayout& layout : m_Layout)
        {
            layout.m_PinPositions.assign(layout.m_Pins.size(), ImVec2(0.0f, 0.0f));
            sizet inputRow = 0;
            sizet outputRow = 0;
            for (sizet i = 0; i < layout.m_Pins.size(); ++i)
            {
                const bool isInput = layout.m_Pins[i].m_Direction == PinDirection::Input;
                const sizet row = isInput ? inputRow++ : outputRow++;
                const f32 localY = s_HeaderHeight + static_cast<f32>(row) * s_RowHeight + s_RowHeight * 0.5f;
                const f32 localX = isInput ? 0.0f : layout.m_Size.x;
                layout.m_PinPositions[i] = m_Canvas.ToScreen(layout.m_Position + glm::vec2(localX, localY));
            }
        }
    }

    const VisualScriptEditorPanel::NodeLayout* VisualScriptEditorPanel::FindLayout(NodeId id) const
    {
        const auto it = std::ranges::find(m_Layout, id, &NodeLayout::m_Id);
        return it == m_Layout.end() ? nullptr : &(*it);
    }

    //==============================================================================
    // Rendering
    //==============================================================================

    void VisualScriptEditorPanel::OnImGuiRender()
    {
        if (!m_IsOpen)
        {
            return;
        }

        ImGui::SetNextWindowSize(ImVec2(1100.0f, 700.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Visual Script Editor", &m_IsOpen, ImGuiWindowFlags_MenuBar))
        {
            m_IsFocused = false;
            ImGui::End();
            return;
        }
        m_IsFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

        if (!m_Asset)
        {
            NewGraph();
        }

        DrawToolbar();
        HandleShortcuts();

        DrawSidebar();
        ImGui::SameLine();
        DrawCanvas();

        ImGui::End();
    }

    void VisualScriptEditorPanel::DrawToolbar()
    {
        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("New"))
                    NewGraph();
                if (ImGui::MenuItem("Open..."))
                {
                    if (const std::string path = FileDialogs::OpenFile("Visual Script (*.olovs)\0*.olovs\0"); !path.empty())
                        OpenGraph(std::filesystem::path(path));
                }
                if (ImGui::MenuItem("Save", "Ctrl+S"))
                    Save();
                if (ImGui::MenuItem("Save As..."))
                    SaveAs();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Edit"))
            {
                if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !m_UndoStack.empty()))
                    Undo();
                if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !m_RedoStack.empty()))
                    Redo();
                ImGui::Separator();
                if (ImGui::MenuItem("Copy", "Ctrl+C", false, !m_Selection.empty()))
                    CopySelection();
                if (ImGui::MenuItem("Paste", "Ctrl+V", false, !m_Clipboard.empty()))
                    PasteClipboard(m_Canvas.ToGraph(ImGui::GetMousePos()));
                if (ImGui::MenuItem("Delete", "Del", false, !m_Selection.empty()))
                    DeleteSelection();
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("View"))
            {
                if (ImGui::MenuItem("Frame All", "F"))
                    FitView();
                if (ImGui::MenuItem("Reset View"))
                    m_Canvas.ResetView();
                if (ImGui::MenuItem("Auto Layout"))
                    AutoLayout();
                ImGui::Separator();
                ImGui::MenuItem("Trace Execution", nullptr, &m_TraceEnabled);
                ImGui::MenuItem("Show Pin Values", nullptr, &m_ShowPinValues);
                ImGui::EndMenu();
            }

            ImGui::Separator();
            ImGui::TextUnformatted(m_FilePath.empty() ? "<unsaved>" : m_FilePath.filename().string().c_str());
            if (m_IsDirty)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "*");
            }

            // The compile state is the single most useful thing to keep on
            // screen: a graph that does not compile does not run at all, and the
            // runtime's only other signal is a log line at play time.
            ImGui::SameLine();
            if (m_CompiledOk)
            {
                ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1.0f), "  [compiles]");
            }
            else
            {
                ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "  [%d error(s)]", static_cast<i32>(m_CompileErrors.size()));
                if (ImGui::IsItemHovered() && !m_CompileErrors.empty())
                {
                    ImGui::BeginTooltip();
                    for (const CompileDiagnostic& error : m_CompileErrors)
                    {
                        ImGui::Text("%s node %u: %s", error.m_Graph.c_str(), error.m_Node, error.m_Message.c_str());
                    }
                    ImGui::EndTooltip();
                }
            }
            ImGui::EndMenuBar();
        }
    }

    void VisualScriptEditorPanel::DrawCanvas()
    {
        if (!m_Canvas.Begin("##vs_canvas"))
        {
            return;
        }

        RebuildLayout();
        // One lookup per frame, shared by every node's decoration.
        m_FrameDebugInstance = ResolveDebugInstance();
        DrawLinks();
        DrawNodes();
        DrawPendingLink();
        HandleInput();
        DrawContextMenu();

        m_Canvas.End();
    }

    void VisualScriptEditorPanel::DrawLinks()
    {
        const VisualScriptGraph& graph = ActiveGraph();
        for (const VisualScriptLink& link : graph.m_Links)
        {
            const NodeLayout* source = FindLayout(link.m_SourceNode);
            const NodeLayout* target = FindLayout(link.m_TargetNode);
            if (source == nullptr || target == nullptr)
            {
                continue;
            }

            const auto findPin = [](const NodeLayout& layout, const std::string& name, PinDirection direction) -> i32
            {
                for (sizet i = 0; i < layout.m_Pins.size(); ++i)
                {
                    if (layout.m_Pins[i].m_Direction == direction && layout.m_Pins[i].m_Name == name)
                        return static_cast<i32>(i);
                }
                return -1;
            };

            const i32 sourcePin = findPin(*source, link.m_SourcePin, PinDirection::Output);
            const i32 targetPin = findPin(*target, link.m_TargetPin, PinDirection::Input);
            if (sourcePin < 0 || targetPin < 0)
            {
                // A dangling wire — the compiler rejects it too. Skipping it here
                // keeps the canvas readable; the error panel names it.
                continue;
            }

            const PinType type = source->m_Pins[static_cast<sizet>(sourcePin)].m_Type;
            const ImVec2 from = source->m_PinPositions[static_cast<sizet>(sourcePin)];
            const ImVec2 to = target->m_PinPositions[static_cast<sizet>(targetPin)];

            if (IsExecPin(type))
            {
                m_Canvas.DrawDirectionalWire(from, to, kExecWire, 3.5f);
            }
            else
            {
                m_Canvas.DrawWire(from, to, PinColor(type), 2.0f);
            }
        }
    }

    void VisualScriptEditorPanel::DrawPendingLink()
    {
        if (!m_DraggingLink)
        {
            return;
        }
        const NodeLayout* source = FindLayout(m_LinkSourceNode);
        if (source == nullptr || m_LinkSourcePin < 0 || static_cast<sizet>(m_LinkSourcePin) >= source->m_Pins.size())
        {
            return;
        }

        const PinType type = source->m_Pins[static_cast<sizet>(m_LinkSourcePin)].m_Type;
        const ImVec2 anchor = source->m_PinPositions[static_cast<sizet>(m_LinkSourcePin)];
        const ImVec2 cursor = ImGui::GetMousePos();

        // Colour the in-flight wire by whether the pin under the cursor would
        // actually accept it, using the SAME function the compiler uses. Red
        // means "this will be refused", so an illegal wire is obvious before the
        // author lets go rather than after the next compile.
        ImU32 color = IsExecPin(type) ? kExecWire : PinColor(type);
        NodeId hoveredNode = kInvalidNodeId;
        if (const i32 hoveredPin = HitTestPin(cursor, hoveredNode); hoveredPin >= 0)
        {
            const NodeLayout* target = FindLayout(hoveredNode);
            if (target != nullptr)
            {
                const PinType other = target->m_Pins[static_cast<sizet>(hoveredPin)].m_Type;
                const PinType a = m_LinkFromOutput ? type : other;
                const PinType b = m_LinkFromOutput ? other : type;
                color = CheckLinkCompatibility(a, b) == LinkCompatibility::Incompatible
                            ? IM_COL32(230, 60, 60, 255)
                            : IM_COL32(120, 255, 120, 255);
            }
        }

        if (m_LinkFromOutput)
        {
            m_Canvas.DrawWire(anchor, cursor, color, IsExecPin(type) ? 3.5f : 2.0f);
        }
        else
        {
            m_Canvas.DrawWire(cursor, anchor, color, IsExecPin(type) ? 3.5f : 2.0f);
        }
    }

    void VisualScriptEditorPanel::DrawNodes()
    {
        for (const NodeLayout& layout : m_Layout)
        {
            DrawNode(layout);
        }
    }

    void VisualScriptEditorPanel::DrawNode(const NodeLayout& layout)
    {
        ImDrawList* draw = m_Canvas.GetDrawList();
        const f32 zoom = m_Canvas.GetZoom();
        const ImVec2 topLeft = m_Canvas.ToScreen(layout.m_Position);
        const ImVec2 bottomRight = m_Canvas.ToScreen(layout.m_Position + layout.m_Size);
        const f32 rounding = 5.0f * zoom;

        // Body + header
        draw->AddRectFilled(topLeft, bottomRight, kNodeBody, rounding);
        const ImVec2 headerBottom(bottomRight.x, topLeft.y + s_HeaderHeight * zoom);
        const ImU32 headerColor = layout.m_Type != nullptr ? CategoryColor(layout.m_Type->m_Category)
                                                           : IM_COL32(120, 40, 40, 255);
        draw->AddRectFilled(topLeft, headerBottom, headerColor, rounding, ImDrawFlags_RoundCornersTop);

        //-- Debug decoration ------------------------------------------------------
        const VisualScriptInstance* instance = m_FrameDebugInstance;
        const u64 nodeKey = DebugState::MakeKey(ActiveGraphIndex(), layout.m_Id);
        bool isPaused = false;
        if (instance != nullptr)
        {
            const DebugState& debug = instance->Debug();
            isPaused = debug.m_Paused && debug.m_PausedAt == nodeKey;

            if (const auto it = debug.m_ExecutionOrder.find(nodeKey); it != debug.m_ExecutionOrder.end())
            {
                // Fade by how far back in the execution order this node was, not
                // by wall time: a graph that ran 4 nodes shows all 4 at full
                // strength, and one in a tight loop shows only the recent ones.
                const u64 age = debug.m_ExecutionCounter - it->second;
                if (age < s_TraceWindow)
                {
                    const f32 strength = 1.0f - static_cast<f32>(age) / static_cast<f32>(s_TraceWindow);
                    const ImU32 glow = IM_COL32(120, 220, 120, static_cast<i32>(60.0f + 180.0f * strength));
                    draw->AddRect(ImVec2(topLeft.x - 3.0f, topLeft.y - 3.0f),
                                  ImVec2(bottomRight.x + 3.0f, bottomRight.y + 3.0f),
                                  glow, rounding, 0, 3.0f);
                }
            }
        }

        const bool selected = m_Selection.contains(layout.m_Id);
        if (isPaused)
        {
            draw->AddRect(topLeft, bottomRight, kPausedGlow, rounding, 0, 3.5f);
        }
        else
        {
            draw->AddRect(topLeft, bottomRight, selected ? kNodeSelected : kNodeBorder, rounding, 0, selected ? 2.5f : 1.5f);
        }

        if (m_Breakpoints.contains(nodeKey))
        {
            draw->AddCircleFilled(ImVec2(topLeft.x + 7.0f * zoom, topLeft.y + 7.0f * zoom), 5.0f * zoom, kBreakpoint);
        }

        // Below roughly a third scale, glyphs are unreadable mush; drawing them
        // costs a lot and communicates nothing. The coloured boxes and wires
        // still convey the graph's shape at that zoom.
        if (zoom < 0.35f)
        {
            return;
        }

        //-- Title -----------------------------------------------------------------
        const std::string& title = layout.m_Type != nullptr ? layout.m_Type->m_DisplayName : std::string("<unknown node>");
        draw->AddText(ImVec2(topLeft.x + 8.0f * zoom, topLeft.y + 5.0f * zoom), IM_COL32(240, 240, 240, 255), title.c_str());

        //-- Pins ------------------------------------------------------------------
        for (sizet i = 0; i < layout.m_Pins.size(); ++i)
        {
            const PinDescriptor& pin = layout.m_Pins[i];
            const ImVec2 centre = layout.m_PinPositions[i];
            const ImU32 color = PinColor(pin.m_Type);
            const bool isInput = pin.m_Direction == PinDirection::Input;

            if (IsExecPin(pin.m_Type))
            {
                // A triangle, not a circle: exec and data pins must be tellable
                // apart at a glance, and colour alone fails for the ~8% of people
                // with a colour-vision deficiency.
                const f32 s = s_PinRadius * zoom * 1.3f;
                draw->AddTriangleFilled(ImVec2(centre.x - s, centre.y - s),
                                        ImVec2(centre.x - s, centre.y + s),
                                        ImVec2(centre.x + s, centre.y), color);
            }
            else
            {
                draw->AddCircleFilled(centre, s_PinRadius * zoom, color);
                draw->AddCircle(centre, s_PinRadius * zoom, IM_COL32(20, 20, 20, 200));
            }

            const ImVec2 textSize = ImGui::CalcTextSize(pin.m_Name.c_str());
            const f32 textX = isInput ? centre.x + 10.0f * zoom : centre.x - 10.0f * zoom - textSize.x;
            draw->AddText(ImVec2(textX, centre.y - textSize.y * 0.5f), IM_COL32(215, 215, 215, 255), pin.m_Name.c_str());

            //-- Live pin value ----------------------------------------------------
            if (m_ShowPinValues && instance != nullptr && !isInput && IsDataPin(pin.m_Type))
            {
                const PinValue value = instance->PeekOutput(ActiveGraphIndex(), layout.m_Id, pin.m_Name);
                const std::string text = value.AsString();
                if (!text.empty())
                {
                    const ImVec2 size = ImGui::CalcTextSize(text.c_str());
                    const ImVec2 at(centre.x + 12.0f * zoom, centre.y - size.y * 0.5f);
                    draw->AddRectFilled(ImVec2(at.x - 3.0f, at.y - 1.0f), ImVec2(at.x + size.x + 3.0f, at.y + size.y + 1.0f),
                                        IM_COL32(15, 15, 15, 200), 3.0f);
                    draw->AddText(at, kExecGlow, text.c_str());
                }
            }
        }
    }

    //==============================================================================
    // Interaction
    //==============================================================================

    i32 VisualScriptEditorPanel::HitTestPin(ImVec2 screenPos, NodeId& outNode) const
    {
        const f32 radius = PinHitRadius(m_Canvas.GetZoom());
        for (const NodeLayout& layout : m_Layout)
        {
            for (sizet i = 0; i < layout.m_PinPositions.size(); ++i)
            {
                const ImVec2 centre = layout.m_PinPositions[i];
                const f32 dx = centre.x - screenPos.x;
                const f32 dy = centre.y - screenPos.y;
                if (dx * dx + dy * dy <= radius * radius)
                {
                    outNode = layout.m_Id;
                    return static_cast<i32>(i);
                }
            }
        }
        outNode = kInvalidNodeId;
        return -1;
    }

    NodeId VisualScriptEditorPanel::HitTestNode(ImVec2 screenPos) const
    {
        // Back to front: later nodes draw on top, so they must win the hit-test.
        for (auto it = m_Layout.rbegin(); it != m_Layout.rend(); ++it)
        {
            const ImVec2 topLeft = m_Canvas.ToScreen(it->m_Position);
            const ImVec2 bottomRight = m_Canvas.ToScreen(it->m_Position + it->m_Size);
            if (screenPos.x >= topLeft.x && screenPos.x <= bottomRight.x && screenPos.y >= topLeft.y && screenPos.y <= bottomRight.y)
            {
                return it->m_Id;
            }
        }
        return kInvalidNodeId;
    }

    LinkId VisualScriptEditorPanel::HitTestLink(ImVec2 screenPos) const
    {
        const VisualScriptGraph& graph = ActiveGraph();
        for (const VisualScriptLink& link : graph.m_Links)
        {
            const NodeLayout* source = FindLayout(link.m_SourceNode);
            const NodeLayout* target = FindLayout(link.m_TargetNode);
            if (source == nullptr || target == nullptr)
            {
                continue;
            }
            for (sizet i = 0; i < source->m_Pins.size(); ++i)
            {
                if (source->m_Pins[i].m_Direction != PinDirection::Output || source->m_Pins[i].m_Name != link.m_SourcePin)
                {
                    continue;
                }
                for (sizet j = 0; j < target->m_Pins.size(); ++j)
                {
                    if (target->m_Pins[j].m_Direction != PinDirection::Input || target->m_Pins[j].m_Name != link.m_TargetPin)
                    {
                        continue;
                    }
                    if (m_Canvas.DistanceToWire(source->m_PinPositions[i], target->m_PinPositions[j], screenPos) <= 8.0f)
                    {
                        return link.m_Id;
                    }
                }
            }
        }
        return kInvalidLinkId;
    }

    void VisualScriptEditorPanel::CompleteLinkDrag(NodeId targetNode, i32 targetPin)
    {
        const NodeLayout* source = FindLayout(m_LinkSourceNode);
        const NodeLayout* target = FindLayout(targetNode);
        if (source == nullptr || target == nullptr || m_LinkSourcePin < 0 || targetPin < 0)
        {
            return;
        }

        // Normalise so `out` is always the output end, whichever end the drag
        // started from — dragging backwards from an input is the same wire.
        const NodeLayout* outNode = m_LinkFromOutput ? source : target;
        const NodeLayout* inNode = m_LinkFromOutput ? target : source;
        const i32 outPin = m_LinkFromOutput ? m_LinkSourcePin : targetPin;
        const i32 inPin = m_LinkFromOutput ? targetPin : m_LinkSourcePin;

        if (outNode->m_Pins[static_cast<sizet>(outPin)].m_Direction != PinDirection::Output ||
            inNode->m_Pins[static_cast<sizet>(inPin)].m_Direction != PinDirection::Input)
        {
            return; // output-to-output or input-to-input
        }
        if (outNode->m_Id == inNode->m_Id)
        {
            return; // a node wired to itself
        }

        const PinType outType = outNode->m_Pins[static_cast<sizet>(outPin)].m_Type;
        const PinType inType = inNode->m_Pins[static_cast<sizet>(inPin)].m_Type;
        if (CheckLinkCompatibility(outType, inType) == LinkCompatibility::Incompatible)
        {
            return;
        }

        PushUndo();
        VisualScriptGraph& graph = ActiveGraph();

        // An input DATA pin takes exactly one wire (the compiler rejects two), so
        // dropping a second one replaces the first — the behaviour every node
        // editor has, and the alternative is an edit that silently fails to
        // compile. Exec inputs are the opposite: many callers, all legal.
        if (IsDataPin(inType))
        {
            const std::string& inName = inNode->m_Pins[static_cast<sizet>(inPin)].m_Name;
            std::erase_if(graph.m_Links, [&](const VisualScriptLink& link)
                          { return link.m_TargetNode == inNode->m_Id && link.m_TargetPin == inName; });
        }
        // An exec OUTPUT also takes one wire — it is a single "next", and two
        // would make the order depend on link order rather than on a Sequence.
        if (IsExecPin(outType))
        {
            const std::string& outName = outNode->m_Pins[static_cast<sizet>(outPin)].m_Name;
            std::erase_if(graph.m_Links, [&](const VisualScriptLink& link)
                          { return link.m_SourceNode == outNode->m_Id && link.m_SourcePin == outName; });
        }

        (void)graph.AddLink(outNode->m_Id, outNode->m_Pins[static_cast<sizet>(outPin)].m_Name,
                            inNode->m_Id, inNode->m_Pins[static_cast<sizet>(inPin)].m_Name);
        Compile();
    }

    void VisualScriptEditorPanel::HandleInput()
    {
        if (!m_Canvas.IsHovered() && !m_DraggingNodes && !m_DraggingLink && !m_BoxSelecting)
        {
            return;
        }
        if (m_Canvas.IsPanning())
        {
            return;
        }

        const ImVec2 mouse = ImGui::GetMousePos();
        const ImGuiIO& io = ImGui::GetIO();

        //-- Press -----------------------------------------------------------------
        if (m_Canvas.IsHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            NodeId pinNode = kInvalidNodeId;
            const i32 pin = HitTestPin(mouse, pinNode);
            if (pin >= 0)
            {
                const NodeLayout* layout = FindLayout(pinNode);
                m_DraggingLink = true;
                m_LinkSourceNode = pinNode;
                m_LinkSourcePin = pin;
                m_LinkFromOutput = layout != nullptr && layout->m_Pins[static_cast<sizet>(pin)].m_Direction == PinDirection::Output;
            }
            else if (const NodeId node = HitTestNode(mouse); node != kInvalidNodeId)
            {
                if (io.KeyCtrl)
                {
                    if (!m_Selection.insert(node).second)
                        m_Selection.erase(node);
                }
                else if (!m_Selection.contains(node))
                {
                    m_Selection.clear();
                    m_Selection.insert(node);
                }
                m_DetailsNode = node;

                // Alt+click a node toggles a breakpoint — the same gesture as
                // alt+click on a wire to cut it, and it keeps the canvas free of
                // per-node gutter chrome.
                if (io.KeyAlt)
                {
                    const u64 key = DebugState::MakeKey(ActiveGraphIndex(), node);
                    if (!m_Breakpoints.insert(key).second)
                        m_Breakpoints.erase(key);
                }
                else
                {
                    m_DraggingNodes = true;
                    m_DragOrigin = m_Canvas.ToGraph(mouse);
                    m_DragStart.clear();
                    VisualScriptGraph& graph = ActiveGraph();
                    for (const NodeId selected : m_Selection)
                    {
                        if (const VisualScriptNode* source = graph.FindNode(selected); source != nullptr)
                            m_DragStart.emplace_back(selected, source->m_Position);
                    }
                    // Held, not pushed: see m_PendingDragUndo.
                    m_PendingDragUndo = CaptureSnapshot();
                }
            }
            else if (io.KeyAlt)
            {
                if (const LinkId link = HitTestLink(mouse); link != kInvalidLinkId)
                {
                    PushUndo();
                    (void)ActiveGraph().RemoveLink(link);
                    Compile();
                }
            }
            else
            {
                m_Selection.clear();
                m_DetailsNode = kInvalidNodeId;
                m_BoxSelecting = true;
                m_BoxStart = mouse;
            }
        }

        //-- Drag ------------------------------------------------------------------
        if (m_DraggingNodes && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            const glm::vec2 delta = m_Canvas.ToGraph(mouse) - m_DragOrigin;
            VisualScriptGraph& graph = ActiveGraph();
            for (const auto& [id, origin] : m_DragStart)
            {
                if (VisualScriptNode* node = graph.FindNode(id); node != nullptr)
                    node->m_Position = origin + delta;
            }
        }

        if (m_BoxSelecting)
        {
            m_Canvas.GetDrawList()->AddRect(m_BoxStart, mouse, IM_COL32(255, 200, 80, 200));
            m_Canvas.GetDrawList()->AddRectFilled(m_BoxStart, mouse, IM_COL32(255, 200, 80, 30));
        }

        //-- Release ---------------------------------------------------------------
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            if (m_DraggingLink)
            {
                NodeId targetNode = kInvalidNodeId;
                if (const i32 targetPin = HitTestPin(mouse, targetNode); targetPin >= 0)
                {
                    CompleteLinkDrag(targetNode, targetPin);
                }
                m_DraggingLink = false;
                m_LinkSourceNode = kInvalidNodeId;
                m_LinkSourcePin = -1;
            }

            if (m_BoxSelecting)
            {
                const f32 minX = std::min(m_BoxStart.x, mouse.x);
                const f32 maxX = std::max(m_BoxStart.x, mouse.x);
                const f32 minY = std::min(m_BoxStart.y, mouse.y);
                const f32 maxY = std::max(m_BoxStart.y, mouse.y);
                for (const NodeLayout& layout : m_Layout)
                {
                    const ImVec2 topLeft = m_Canvas.ToScreen(layout.m_Position);
                    const ImVec2 bottomRight = m_Canvas.ToScreen(layout.m_Position + layout.m_Size);
                    // Intersection, not containment: a marquee that only catches
                    // fully-enclosed nodes misses everything at the edges.
                    if (bottomRight.x >= minX && topLeft.x <= maxX && bottomRight.y >= minY && topLeft.y <= maxY)
                        m_Selection.insert(layout.m_Id);
                }
                m_BoxSelecting = false;
            }

            if (m_DraggingNodes && m_PendingDragUndo.has_value())
            {
                // Commit the drag as ONE undo entry, and only if the nodes
                // actually moved — a click that selected without dragging must
                // not dirty the graph or push an entry that undoes nothing.
                VisualScriptGraph& graph = ActiveGraph();
                const bool moved = std::ranges::any_of(m_DragStart, [&](const auto& entry)
                                                       {
                    const VisualScriptNode* node = graph.FindNode(entry.first);
                    return node != nullptr && !Math::BitwiseEqual(node->m_Position, entry.second); });
                if (moved)
                {
                    m_UndoStack.push_back(std::move(*m_PendingDragUndo));
                    if (m_UndoStack.size() > s_MaxUndoDepth)
                        m_UndoStack.erase(m_UndoStack.begin());
                    m_RedoStack.clear();
                    m_IsDirty = true;
                }
                m_PendingDragUndo.reset();
            }

            m_DraggingNodes = false;
            m_DragStart.clear();
        }

        //-- Context menu ----------------------------------------------------------
        // Right-CLICK, meaning a press and release that did not move. GraphCanvas
        // treats a right DRAG as a pan, so the two gestures must be told apart —
        // and tracking the press position here avoids depending on ImGui's
        // internal drag bookkeeping, which has moved between versions.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            m_RightPressPos = mouse;
        }
        const f32 rightDx = mouse.x - m_RightPressPos.x;
        const f32 rightDy = mouse.y - m_RightPressPos.y;
        if (m_Canvas.IsHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
            (rightDx * rightDx + rightDy * rightDy) < 16.0f)
        {
            m_ContextMenuOpen = true;
            m_ContextMenuGraphPos = m_Canvas.ToGraph(mouse);
            m_NodeSearch[0] = '\0';
            ImGui::OpenPopup("##vs_add_node");
        }
    }

    void VisualScriptEditorPanel::HandleShortcuts()
    {
        if (!m_IsFocused)
        {
            return;
        }
        const ImGuiIO& io = ImGui::GetIO();
        // A shortcut while a text box has focus would eat the keystroke — Ctrl+V
        // into the variable-name field being the obvious one.
        if (io.WantTextInput)
        {
            return;
        }

        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
            Save();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
            Undo();
        if (io.KeyCtrl && (ImGui::IsKeyPressed(ImGuiKey_Y, false) || (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false))))
            Redo();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false))
            CopySelection();
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false))
            PasteClipboard(m_Canvas.ToGraph(ImGui::GetMousePos()));
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
            DeleteSelection();
        if (ImGui::IsKeyPressed(ImGuiKey_F, false))
            FitView();
    }

    void VisualScriptEditorPanel::DrawContextMenu()
    {
        if (ImGui::BeginPopup("##vs_add_node"))
        {
            ImGui::TextUnformatted("Add Node");
            ImGui::Separator();
            ImGui::SetNextItemWidth(240.0f);
            if (ImGui::IsWindowAppearing())
            {
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::InputTextWithHint("##search", "Search...", m_NodeSearch, sizeof(m_NodeSearch));

            std::string filter = m_NodeSearch;
            std::ranges::transform(filter, filter.begin(), [](unsigned char c)
                                   { return static_cast<char>(std::tolower(c)); });

            ImGui::BeginChild("##results", ImVec2(280.0f, 340.0f));
            std::string currentCategory;
            for (const NodeTypeDescriptor* type : NodeRegistry::Get().GetSorted())
            {
                if (!filter.empty())
                {
                    std::string haystack = type->m_DisplayName + " " + type->m_TypeName + " " + type->m_Category;
                    std::ranges::transform(haystack, haystack.begin(), [](unsigned char c)
                                           { return static_cast<char>(std::tolower(c)); });
                    if (haystack.find(filter) == std::string::npos)
                        continue;
                }

                if (type->m_Category != currentCategory)
                {
                    currentCategory = type->m_Category;
                    ImGui::SeparatorText(currentCategory.c_str());
                }
                if (ImGui::Selectable(type->m_DisplayName.c_str()))
                {
                    CreateNode(type->m_TypeName, m_ContextMenuGraphPos);
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::IsItemHovered() && !type->m_Tooltip.empty())
                {
                    ImGui::SetTooltip("%s", type->m_Tooltip.c_str());
                }
            }
            ImGui::EndChild();
            ImGui::EndPopup();
        }
        else
        {
            m_ContextMenuOpen = false;
        }
    }

    //==============================================================================
    // Graph operations
    //==============================================================================

    void VisualScriptEditorPanel::CreateNode(const std::string& typeName, glm::vec2 position)
    {
        PushUndo();
        VisualScriptNode& node = ActiveGraph().AddNode(typeName, position);

        // Seed the properties whose absence would make the node meaningless, so a
        // freshly placed Custom Event has a name to edit rather than an empty one
        // that silently never fires.
        if (typeName == NodeTypes::kCustomEvent || typeName == NodeTypes::kOnGameplayEvent)
        {
            node.SetProperty(std::string(NodeProps::kEventName), "MyEvent");
        }
        else if (typeName == NodeTypes::kSequence)
        {
            node.SetProperty(std::string(NodeProps::kOutputCount), "2");
        }
        else if ((typeName == NodeTypes::kGetVariable || typeName == NodeTypes::kSetVariable) && !m_Asset->m_Variables.empty())
        {
            node.SetProperty(std::string(NodeProps::kVariableName), m_Asset->m_Variables.front().m_Name);
        }

        m_Selection.clear();
        m_Selection.insert(node.m_Id);
        m_DetailsNode = node.m_Id;
        Compile();
    }

    void VisualScriptEditorPanel::DeleteSelection()
    {
        if (m_Selection.empty())
        {
            return;
        }
        PushUndo();
        VisualScriptGraph& graph = ActiveGraph();
        for (const NodeId id : m_Selection)
        {
            (void)graph.RemoveNode(id); // also drops every wire touching it
        }
        m_Selection.clear();
        m_DetailsNode = kInvalidNodeId;
        Compile();
    }

    void VisualScriptEditorPanel::CopySelection()
    {
        m_Clipboard.clear();
        const VisualScriptGraph& graph = ActiveGraph();
        for (const NodeId id : m_Selection)
        {
            if (const VisualScriptNode* node = graph.FindNode(id); node != nullptr)
                m_Clipboard.push_back(*node);
        }
    }

    void VisualScriptEditorPanel::PasteClipboard(glm::vec2 position)
    {
        if (m_Clipboard.empty())
        {
            return;
        }
        PushUndo();

        // Paste relative to the cursor, preserving the copied nodes' relative
        // arrangement rather than stacking them all on one point.
        glm::vec2 min(std::numeric_limits<f32>::max());
        for (const VisualScriptNode& node : m_Clipboard)
        {
            min = glm::min(min, node.m_Position);
        }

        VisualScriptGraph& graph = ActiveGraph();
        m_Selection.clear();
        for (const VisualScriptNode& source : m_Clipboard)
        {
            VisualScriptNode& node = graph.AddNode(source.m_TypeName, position + (source.m_Position - min));
            // Fresh id, everything else carried over. Wires are deliberately NOT
            // copied: a pasted wire would have to be remapped onto the new ids,
            // and a half-remapped one is a dangling link the compiler rejects.
            node.m_PinDefaults = source.m_PinDefaults;
            node.m_Properties = source.m_Properties;
            m_Selection.insert(node.m_Id);
        }
        Compile();
    }

    void VisualScriptEditorPanel::AutoLayout()
    {
        if (!m_Asset)
        {
            return;
        }
        PushUndo();

        // Longest-path layering over EXEC edges only: the exec chain is what a
        // reader follows, and laying out by data edges would put a constant three
        // columns left of the node that consumes it.
        VisualScriptGraph& graph = ActiveGraph();
        std::unordered_map<NodeId, i32> depth;
        for (const VisualScriptNode& node : graph.m_Nodes)
        {
            depth[node.m_Id] = 0;
        }

        // Relax |V| times — cheap for authored graph sizes and correct even with
        // an exec cycle present (it just stops improving).
        for (sizet pass = 0; pass < graph.m_Nodes.size(); ++pass)
        {
            bool changed = false;
            for (const VisualScriptLink& link : graph.m_Links)
            {
                const NodeLayout* source = FindLayout(link.m_SourceNode);
                if (source == nullptr)
                    continue;
                const auto pin = std::ranges::find_if(source->m_Pins, [&](const PinDescriptor& p)
                                                      { return p.m_Direction == PinDirection::Output && p.m_Name == link.m_SourcePin; });
                if (pin == source->m_Pins.end() || !IsExecPin(pin->m_Type))
                    continue;

                const i32 candidate = depth[link.m_SourceNode] + 1;
                if (candidate > depth[link.m_TargetNode] && candidate < static_cast<i32>(graph.m_Nodes.size()))
                {
                    depth[link.m_TargetNode] = candidate;
                    changed = true;
                }
            }
            if (!changed)
                break;
        }

        std::unordered_map<i32, i32> rowInColumn;
        for (VisualScriptNode& node : graph.m_Nodes)
        {
            const i32 column = depth[node.m_Id];
            const i32 row = rowInColumn[column]++;
            node.m_Position = glm::vec2(60.0f + static_cast<f32>(column) * 280.0f,
                                        60.0f + static_cast<f32>(row) * 170.0f);
        }
        Compile();
    }

    void VisualScriptEditorPanel::FitView()
    {
        if (m_Layout.empty())
        {
            m_Canvas.ResetView();
            return;
        }
        glm::vec2 min(std::numeric_limits<f32>::max());
        glm::vec2 max(std::numeric_limits<f32>::lowest());
        for (const NodeLayout& layout : m_Layout)
        {
            min = glm::min(min, layout.m_Position);
            max = glm::max(max, layout.m_Position + layout.m_Size);
        }
        m_Canvas.FitToBounds(min, max);
    }

} // namespace OloEngine

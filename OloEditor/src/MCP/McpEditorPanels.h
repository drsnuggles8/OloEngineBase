#pragma once

#include "MCP/McpSchemaBuilder.h"
#include "MCP/McpServer.h"
#include "MCP/McpTokenNormalization.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace OloEngine::MCP::EditorPanels
{
    enum class PanelId : u32
    {
        ShaderDebugger,
        GPUResourceInspector,
        CommandBucketInspector,
        RendererProfiler,
        RenderGraphDebugger,
        AssetPackBuilder,
        BuildGame,
        InstanceScatterBrush,
        SkillTreeEditor,
        CinematicTimeline,
        Console,
        Statistics,
        Animation,
        PostProcessSettings,
        RendererSettings,
        TerrainEditor,
        SceneStreaming,
        InputSettings,
        NetworkDebug,
        ThreadInspector,
        DialogueEditor,
        NavMesh,
        BehaviorTreeEditor,
        StateMachineEditor,
        ShaderGraphEditor,
        VisualScriptEditor,
        SoundGraphEditor,
        AnimationGraphEditor,
        SaveGame,
        Localization,
        GamepadDebug,
        ShaderEditor,
        AudioEvents,
        McpServer,
        TilemapPainter,
        Count
    };

    struct PanelDescriptor
    {
        PanelId Id;
        std::string_view Name;
        std::string_view Title;
    };

    inline constexpr std::array kPanels{
        PanelDescriptor{ PanelId::ShaderDebugger, "shader_debugger", "Shader Debugger" },
        PanelDescriptor{ PanelId::GPUResourceInspector, "gpu_resource_inspector", "GPU Resource Inspector" },
        PanelDescriptor{ PanelId::CommandBucketInspector, "command_bucket_inspector", "Command Bucket Inspector" },
        PanelDescriptor{ PanelId::RendererProfiler, "renderer_profiler", "Renderer Profiler" },
        PanelDescriptor{ PanelId::RenderGraphDebugger, "render_graph_debugger", "Render Graph Debugger" },
        PanelDescriptor{ PanelId::AssetPackBuilder, "asset_pack_builder", "Asset Pack Builder" },
        PanelDescriptor{ PanelId::BuildGame, "build_game", "Build Game" },
        PanelDescriptor{ PanelId::InstanceScatterBrush, "instance_scatter_brush", "Instance Scatter Brush" },
        PanelDescriptor{ PanelId::SkillTreeEditor, "skill_tree_editor", "Skill Tree Editor" },
        PanelDescriptor{ PanelId::CinematicTimeline, "cinematic_timeline", "Cinematic Timeline" },
        PanelDescriptor{ PanelId::Console, "console", "Console" },
        PanelDescriptor{ PanelId::Statistics, "statistics", "Statistics" },
        PanelDescriptor{ PanelId::Animation, "animation", "Animation Panel" },
        PanelDescriptor{ PanelId::PostProcessSettings, "post_process_settings", "Post Process Settings" },
        PanelDescriptor{ PanelId::RendererSettings, "renderer_settings", "Renderer Settings" },
        PanelDescriptor{ PanelId::TerrainEditor, "terrain_editor", "Terrain Editor" },
        PanelDescriptor{ PanelId::SceneStreaming, "scene_streaming", "Scene Streaming" },
        PanelDescriptor{ PanelId::InputSettings, "input_settings", "Input Settings" },
        PanelDescriptor{ PanelId::NetworkDebug, "network_debug", "Network Debug" },
        PanelDescriptor{ PanelId::ThreadInspector, "thread_inspector", "Thread Inspector" },
        PanelDescriptor{ PanelId::DialogueEditor, "dialogue_editor", "Dialogue Editor" },
        PanelDescriptor{ PanelId::NavMesh, "navmesh", "NavMesh Panel" },
        PanelDescriptor{ PanelId::BehaviorTreeEditor, "behavior_tree_editor", "Behavior Tree Editor" },
        PanelDescriptor{ PanelId::StateMachineEditor, "state_machine_editor", "State Machine Editor" },
        PanelDescriptor{ PanelId::ShaderGraphEditor, "shader_graph_editor", "Shader Graph Editor" },
        PanelDescriptor{ PanelId::VisualScriptEditor, "visual_script_editor", "Visual Script Editor" },
        PanelDescriptor{ PanelId::SoundGraphEditor, "sound_graph_editor", "Sound Graph Editor" },
        PanelDescriptor{ PanelId::AnimationGraphEditor, "animation_graph_editor", "Animation Graph Editor" },
        PanelDescriptor{ PanelId::SaveGame, "save_game", "Save Game Panel" },
        PanelDescriptor{ PanelId::Localization, "localization", "Localization" },
        PanelDescriptor{ PanelId::GamepadDebug, "gamepad_debug", "Gamepad Debug" },
        PanelDescriptor{ PanelId::ShaderEditor, "shader_editor", "Shader Editor" },
        PanelDescriptor{ PanelId::AudioEvents, "audio_events", "Audio Events" },
        PanelDescriptor{ PanelId::McpServer, "mcp_server", "MCP Server" },
        PanelDescriptor{ PanelId::TilemapPainter, "tilemap_painter", "Tilemap Painter" },
    };
    static_assert(kPanels.size() == static_cast<sizet>(PanelId::Count));

    [[nodiscard]] inline const PanelDescriptor* Find(std::string_view name)
    {
        const std::string normalized = NormalizeToken(name);
        for (const auto& panel : kPanels)
        {
            if (NormalizeToken(panel.Name) == normalized || NormalizeToken(panel.Title) == normalized)
                return &panel;
        }
        return nullptr;
    }

    [[nodiscard]] inline Json ListInputSchema()
    {
        return Schema::EmptyObject();
    }

    [[nodiscard]] inline Json SetInputSchema()
    {
        return Schema::Object()
            .Prop("panel", Schema::String().Desc("Panel canonical name or display title (see olo_editor_panel_list)."))
            .Prop("open", Schema::Bool().Desc("True to open the panel; false to close it."))
            .Required({ "panel", "open" })
            .NoAdditional();
    }

    [[nodiscard]] inline Json ToJson(const McpEditorPanelState& state)
    {
        return Json{ { "name", state.Name }, { "title", state.Title }, { "open", state.Open } };
    }

    [[nodiscard]] inline Json ToJson(const McpEditorPanelSetResult& result)
    {
        return Json{
            { "available", result.Available },
            { "ok", result.Ok },
            { "changed", result.Changed },
            { "panel", ToJson(result.Panel) },
            { "message", result.Message },
        };
    }
} // namespace OloEngine::MCP::EditorPanels

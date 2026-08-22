set(EDITOR_SOURCES
		"EditorLayer.cpp"
		"EditorLayer.h"
		"OloEditorApp.cpp"
		"GameplayEventLogger.h"
		"GameplayEventLogger.cpp"

		"MCP/McpServer.cpp"
		"MCP/McpServer.h"
		"MCP/McpClient.cpp"
		"MCP/McpClient.h"
		"MCP/McpTools.cpp"
		"MCP/McpTools.h"
		"MCP/McpToolsCommon.h"
		"MCP/McpClusterGridStats.h"
		"MCP/McpRenderProbePixel.h"
		"MCP/McpRenderTargetStats.h"
		"MCP/McpRenderValidate.h"
		"MCP/McpFroxelFogProbe.h"
		"MCP/McpGpuReadbackStats.h"
		"MCP/McpShadowCapture.h"
		"MCP/McpInputInject.h"
		"MCP/McpToolsAssets.cpp"
		"MCP/McpToolsCamera.cpp"
		"MCP/McpToolsInput.cpp"
		"MCP/McpToolsDiagnostics.cpp"
		"MCP/McpToolsPerf.cpp"
		"MCP/McpToolsPhysics.cpp"
		"MCP/McpToolsRender.cpp"
		"MCP/McpToolsScene.cpp"
		# The generated writable-field registry in its own TU (see the file's
		# header comment: keeping it inline in McpGenericFieldWrite.h OOM'd
		# clang-cl Release+ASan compiling McpToolsScene.cpp). Conditionally pulled
		# back out below (issue #822) so it can join the heavy-compile file set.
		"MCP/McpFieldRegistry.cpp"
		"MCP/McpToolsScripting.cpp"
		"MCP/McpToolsShader.cpp"
		"MCP/McpScriptTools.cpp"
		"MCP/McpScriptTools.h"
		"MCP/McpScriptApi.cpp"
		"MCP/McpScriptApi.h"
		"MCP/McpServerPanel.cpp"
		"MCP/McpServerPanel.h"

		"UndoRedo/EditorCommand.h"
		"UndoRedo/EntityCommands.h"
		"UndoRedo/ComponentCommands.h"
		"UndoRedo/SpecializedCommands.h"
		"UndoRedo/AnimationGraphCommands.h"
		"UndoRedo/SoundGraphCommands.h"

		"Preview/AssetThumbnailCache.cpp"
		"Preview/AssetThumbnailCache.h"
		
		"Panels/AnimationPanel.cpp"
		"Panels/AnimationPanel.h"
		"Panels/AudioEventsPanel.cpp"
		"Panels/AudioEventsPanel.h"
		"Panels/AssetPackBuilderPanel.cpp"
		"Panels/AssetPackBuilderPanel.h"
		"Panels/ContentBrowserPanel.cpp"
		"Panels/ContentBrowserPanel.h"
		"Panels/ContentBrowser/ContentBrowserItem.cpp"
		"Panels/ContentBrowser/ContentBrowserItem.h"
		"Panels/ContentBrowser/DirectoryTree.cpp"
		"Panels/ContentBrowser/DirectoryTree.h"
		"Panels/ConsolePanel.cpp"
		"Panels/ConsolePanel.h"
		"Panels/StatisticsPanel.cpp"
		"Panels/StatisticsPanel.h"
		"Panels/EditorPreferencesPanel.cpp"
		"Panels/EditorPreferencesPanel.h"
		"Panels/DialogueEditorPanel.cpp"
		"Panels/DialogueEditorPanel.h"
		"Panels/SkillTreeEditorPanel.cpp"
		"Panels/SkillTreeEditorPanel.h"
		"Panels/CinematicTimelinePanel.cpp"
		"Panels/CinematicTimelinePanel.h"
		"Panels/InputSettingsPanel.cpp"
		"Panels/InputSettingsPanel.h"
		"Panels/PostProcessSettingsPanel.cpp"
		"Panels/PostProcessSettingsPanel.h"
		"Panels/RendererSettingsPanel.cpp"
		"Panels/RendererSettingsPanel.h"
		"Panels/SettingsChangeLog.h"
		"Panels/SceneHierarchyPanel.cpp"
		"Panels/SceneHierarchyPanel.h"
		"Panels/TerrainEditorPanel.cpp"
		"Panels/TerrainEditorPanel.h"
		"Panels/InstanceScatterBrushPanel.cpp"
		"Panels/InstanceScatterBrushPanel.h"
		"Panels/StreamingPanel.cpp"
		"Panels/StreamingPanel.h"
		"Panels/NetworkDebugPanel.cpp"
		"Panels/NetworkDebugPanel.h"
		"Panels/ThreadInspectorPanel.cpp"
		"Panels/ThreadInspectorPanel.h"
		"Panels/SaveGamePanel.cpp"
		"Panels/SaveGamePanel.h"
		"Panels/LocalizationPanel.cpp"
		"Panels/LocalizationPanel.h"
		"Panels/ShaderGraphEditorPanel.cpp"
		"Panels/ShaderGraphEditorPanel.h"
		"Panels/Graph/GraphCanvas.cpp"
		"Panels/Graph/GraphCanvas.h"
		"Panels/VisualScriptEditorPanel.cpp"
		"Panels/VisualScriptEditorPanelSidebar.cpp"
		"Panels/VisualScriptEditorPanel.h"
		"Panels/SoundGraphEditorPanel.cpp"
		"Panels/SoundGraphEditorPanel.h"
		"Panels/GamepadDebugPanel.cpp"
		"Panels/GamepadDebugPanel.h"
		"Panels/NavMeshPanel.cpp"
		"Panels/NavMeshPanel.h"
		"Panels/AnimationGraphEditorPanel.cpp"
		"Panels/AnimationGraphEditorPanel.h"
		"Panels/BehaviorTreeEditorPanel.cpp"
		"Panels/BehaviorTreeEditorPanel.h"
		"Panels/FSMEditorPanel.cpp"
		"Panels/FSMEditorPanel.h"
		"Panels/BuildGamePanel.cpp"
		"Panels/BuildGamePanel.h"
		"Panels/ShaderEditorPanel.cpp"
		"Panels/ShaderEditorPanel.h"
)

file(GLOB_RECURSE GLSL_SOURCE_FILES "../assets/shaders/*.glsl")

source_group(Shaders FILES ${GLSL_SOURCE_FILES})

set(EDITOR_RESOURCES "")
if(WIN32)
	list(APPEND EDITOR_RESOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../Resources/OloEditor.rc")
endif()

# Heavy-TU compile pool (issue #822): see the matching block in OloEngine/src/CMakeLists.txt for
# why the removal must stay conditional on the pool actually being available.
if(OLO_HEAVY_COMPILE_POOL_AVAILABLE)
	list(REMOVE_ITEM EDITOR_SOURCES "MCP/McpFieldRegistry.cpp")
endif()

add_executable(OloEditor
		${EDITOR_SOURCES}
		${GLSL_SOURCE_FILES}
		${EDITOR_RESOURCES}
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
			FILES ${EDITOR_SOURCES}
)

if(OLO_HEAVY_COMPILE_POOL_AVAILABLE)
	olo_bind_heavy_compile_pool(OloEditor "${CMAKE_CURRENT_SOURCE_DIR}"
		"MCP/McpFieldRegistry.cpp"
	)
endif()

# Deploy the FFmpeg runtime libs next to the exe (no-op when OLO_VIDEO_FFMPEG=OFF).
# Must live in the same directory scope as add_executable — see the note on
# olo_copy_ffmpeg_runtime in cmake/CommonProperties.cmake.
olo_copy_ffmpeg_runtime(OloEditor)

# Stage steam_api64.dll next to the exe (#644). No-op unless OLO_WITH_STEAM is on with the real
# SDK. Same directory-scope rule as the ffmpeg copy above.
olo_copy_steam_runtime(OloEditor)

# Stage OpenUSD's plugInfo resource tree next to the editor exe so PlugRegistry can map
# .usda/.usdc/.usdz -> file-format plugin at runtime (#655). OLO_USD_PLUGIN_TREE is set by
# OloEngine's OLO_WITH_USD block; the tree exists after the USD build (OloEditor depends on it
# transitively via OloEngine), so the POST_BUILD copy is correctly ordered. Must live in this
# scope (where OloEditor is created), same as the ffmpeg copy above.
if(OLO_WITH_USD AND OLO_USD_PLUGIN_TREE)
    add_custom_command(TARGET OloEditor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${OLO_USD_PLUGIN_TREE}" "$<TARGET_FILE_DIR:OloEditor>/usd"
        COMMENT "Staging USD plugInfo tree next to OloEditor")
endif()


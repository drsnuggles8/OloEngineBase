set(EDITOR_SOURCES
		"EditorLayer.cpp"
		"EditorLayer.h"
		"OloEditorApp.cpp"
		"GameplayEventLogger.h"
		"GameplayEventLogger.cpp"

		# The transport-independent automation layer (issue #1123): the command
		# type, the registry that owns them, the host seam they run against, and
		# the input-schema validator. Nothing in here knows what MCP is.
		"Automation/AutomationAudienceReport.h"
		"Automation/AutomationBuildCommands.cpp"
		"Automation/AutomationBuildCommands.h"
		"Automation/AutomationBuildInvocation.h"
		"Automation/AutomationCatalogue.cpp"
		"Automation/AutomationCatalogue.h"
		"Automation/AutomationCommand.h"
		"Automation/AutomationHost.h"
		"Automation/AutomationMainThreadJob.h"
		"Automation/AutomationRegistry.cpp"
		"Automation/AutomationRegistry.h"
		"Automation/AutomationResult.cpp"
		"Automation/AutomationResult.h"
		"Automation/AutomationSchemaValidation.cpp"
		"Automation/AutomationSchemaValidation.h"
		"Automation/AutomationSceneAuthoring.h"
		"Automation/AutomationComponentRegistry.h"
		"Automation/AutomationComponentRegistry.cpp"
		"Automation/AutomationEditorCommands.cpp"
		"Automation/AutomationEditorCommands.h"
		"Automation/AutomationComponentCommands.cpp"
		"Automation/AutomationEntityCommands.cpp"
		"Automation/AutomationEntityCommands.h"
		"Automation/AutomationPrefabCommands.cpp"
		"Automation/AutomationPrefabCommands.h"
		"Automation/AutomationSceneCommands.h"
		"Automation/AutomationGroomCommands.cpp"
		"Automation/AutomationGroomCommands.h"
		"Groom/GroomBindingAuthoring.cpp"
		"Groom/GroomBindingAuthoring.h"
		"Automation/AutomationSceneCommands.cpp"
		"Automation/AutomationSceneDocument.h"
		"Automation/AutomationSceneDocument.cpp"
		"Automation/AutomationAssetIndex.h"
		"Automation/AutomationAssetIndex.cpp"
		"Automation/AutomationAssetCommands.h"
		"Automation/AutomationAssetCommands.cpp"
		"Automation/AutomationFileWrite.h"
		"Automation/AutomationFileWrite.cpp"
		"Automation/AutomationTransaction.h"
		"Automation/AutomationTransaction.cpp"
		"Automation/AutomationTransactionCommands.h"
		"Automation/AutomationTransactionCommands.cpp"
		# The publish side of the automation event bus (issue #1131).
		"Automation/AutomationEvents.h"
		"Automation/AutomationEvents.cpp"

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
		"MCP/McpToolsBenchmark.cpp"
		"MCP/McpToolsCamera.cpp"
		"MCP/McpToolsInput.cpp"
		"MCP/McpToolsDiagnostics.cpp"
		"MCP/McpToolsEditor.cpp"
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
		# Structured test execution + whole-project validation (issue #1130).
		"MCP/McpTestExecution.h"
		"MCP/McpToolsTesting.cpp"
		"MCP/McpProjectValidation.h"
		"MCP/McpToolsValidation.cpp"
		"MCP/McpToolsGateway.cpp"
		"MCP/McpExposure.h"
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
		"Panels/TilemapPainterPanel.cpp"
		"Panels/TilemapPainterPanel.h"
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
# --------------------------------------------------------------------------
# OloEditorCore — the editor's headless automation/MCP core (issue #1314).
#
# WHY THIS LIBRARY EXISTS: OloEngine-Tests does not link OloEditor (an executable
# cannot be linked), so it used to RECOMPILE these sources as its own. Measured on a
# build of both targets, that cost 47 duplicated translation units and 231 s — 3.8% of
# the build's entire compile time — and it duplicated the worst TU in the tree:
# McpFieldRegistry.cpp is #1 by peak compiler RSS (8.09 GiB under ASan) and was paid for
# twice. Extracting the shared set to a static library both consumers link removes the
# duplication outright.
#
# THE SEAM IS THE ONE THE TESTS ALREADY DREW, not a new one: every file here was already
# being compiled into the test binary, so the set is known to be self-contained against
# OloEngine. What stays in the executable is the ImGui editor — 34 Panels/*, EditorLayer,
# OloEditorApp, McpServerPanel and the thumbnail cache — none of which any test compiles.
#
# SAFE AS A STATIC LIBRARY because nothing here self-registers. Automation commands are
# published through explicit `Register*Commands(AutomationRegistry&)` entry points that a
# caller invokes, so no object depends on being pulled in for a static initializer's side
# effect — the failure mode that makes naive static-library extraction drop behaviour
# silently. Checked before the move; if a self-registering TU is ever added here it needs
# WHOLE_ARCHIVE or an explicit reference, not a quiet regression.
set(OLO_EDITOR_CORE_SOURCES
		"Automation/AutomationAssetCommands.cpp"
		"Automation/AutomationAssetIndex.cpp"
		"Automation/AutomationBuildCommands.cpp"
		"Automation/AutomationCatalogue.cpp"
		"Automation/AutomationComponentCommands.cpp"
		"Automation/AutomationComponentRegistry.cpp"
		"Automation/AutomationEditorCommands.cpp"
		"Automation/AutomationEntityCommands.cpp"
		"Automation/AutomationEvents.cpp"
		"Automation/AutomationFileWrite.cpp"
		"Automation/AutomationPrefabCommands.cpp"
		"Automation/AutomationRegistry.cpp"
		"Automation/AutomationResult.cpp"
		"Automation/AutomationSceneCommands.cpp"
		"Automation/AutomationSceneDocument.cpp"
		"Automation/AutomationSchemaValidation.cpp"
		"Automation/AutomationTransaction.cpp"
		"Automation/AutomationTransactionCommands.cpp"
		"MCP/McpClient.cpp"
		"MCP/McpFieldRegistry.cpp"
		"MCP/McpScriptApi.cpp"
		"MCP/McpScriptTools.cpp"
		"MCP/McpServer.cpp"
		"MCP/McpTools.cpp"
		"MCP/McpToolsAssets.cpp"
		"MCP/McpToolsBenchmark.cpp"
		"MCP/McpToolsCamera.cpp"
		"MCP/McpToolsDiagnostics.cpp"
		"MCP/McpToolsEditor.cpp"
		"MCP/McpToolsGateway.cpp"
		"MCP/McpToolsInput.cpp"
		"MCP/McpToolsPerf.cpp"
		"MCP/McpToolsPhysics.cpp"
		"MCP/McpToolsRender.cpp"
		"MCP/McpToolsScene.cpp"
		"MCP/McpToolsScripting.cpp"
		"MCP/McpToolsShader.cpp"
		"MCP/McpToolsTesting.cpp"
		"MCP/McpToolsValidation.cpp"
		"Panels/ContentBrowser/ContentBrowserItem.cpp"
		"Panels/ContentBrowser/DirectoryTree.cpp"
		# Arrived on master (#1249) while this library was in flight, listed as ordinary
		# editor sources that OloEngine-Tests recompiled. They belong here for the same
		# reason as everything above: the test target needs them, and the seam is "what
		# both consumers use", not "what the editor happens to ship".
		"Automation/AutomationGroomCommands.cpp"
		"Groom/GroomBindingAuthoring.cpp"
)
list(REMOVE_ITEM EDITOR_SOURCES ${OLO_EDITOR_CORE_SOURCES})

# Same conditional-removal rule as the engine's pool block: pull the heavy TUs out of the
# plain source list ONLY when the pool exists to re-add them via a FILE_SET, or they are
# dropped from the build entirely.
if(OLO_HEAVY_COMPILE_POOL_AVAILABLE)
	list(REMOVE_ITEM OLO_EDITOR_CORE_SOURCES "MCP/McpFieldRegistry.cpp" "MCP/McpToolsRender.cpp" "Automation/AutomationComponentRegistry.cpp")
endif()

add_library(OloEditorCore STATIC ${OLO_EDITOR_CORE_SOURCES})

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
			FILES ${OLO_EDITOR_CORE_SOURCES}
)

if(OLO_HEAVY_COMPILE_POOL_AVAILABLE)
	olo_bind_heavy_compile_pool(OloEditorCore "${CMAKE_CURRENT_SOURCE_DIR}"
		# Ranks #1 and #2 of 1761 TUs by peak compiler RSS (8.09 and 3.85 GiB, Linux
		# Debug + ASan, 2026-09-17). Both instantiate typed snapshots over the complete
		# generated component set.
		"MCP/McpFieldRegistry.cpp"
		"Automation/AutomationComponentRegistry.cpp"
		# 1.50 GiB, #17 — at the >= 1.5 GiB membership threshold (#1307).
		"MCP/McpToolsRender.cpp"
	)
endif()

add_executable(OloEditor
		${EDITOR_SOURCES}
		${GLSL_SOURCE_FILES}
		${EDITOR_RESOURCES}
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
			FILES ${EDITOR_SOURCES}
)

# Deploy the FFmpeg runtime libs next to the exe (no-op when OLO_VIDEO_FFMPEG=OFF).
# Must live in the same directory scope as add_executable — see the note on
# olo_copy_ffmpeg_runtime in cmake/CommonProperties.cmake.
olo_copy_ffmpeg_runtime(OloEditor)

# Stage steam_api64.dll next to the exe (#644). No-op unless OLO_WITH_STEAM is on with the real
# SDK. Same directory-scope rule as the ffmpeg copy above.
olo_copy_steam_runtime(OloEditor)
olo_copy_aftermath_runtime(OloEditor)

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


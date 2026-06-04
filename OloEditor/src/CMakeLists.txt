set(EDITOR_SOURCES
		"EditorLayer.cpp"
		"EditorLayer.h"
		"OloEditorApp.cpp"
		"GameplayEventLogger.h"
		"GameplayEventLogger.cpp"

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
		"Panels/SaveGamePanel.cpp"
		"Panels/SaveGamePanel.h"
		"Panels/LocalizationPanel.cpp"
		"Panels/LocalizationPanel.h"
		"Panels/ShaderGraphEditorPanel.cpp"
		"Panels/ShaderGraphEditorPanel.h"
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

add_executable(OloEditor
		${EDITOR_SOURCES}
		${GLSL_SOURCE_FILES}
		${EDITOR_RESOURCES}
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
			FILES ${EDITOR_SOURCES}
)


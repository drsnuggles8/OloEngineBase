set(EDITOR_SOURCES
		"EditorLayer.cpp"
		"EditorLayer.h"
		"OloEditorApp.cpp"

		"UndoRedo/EditorCommand.h"
		"UndoRedo/EntityCommands.h"
		"UndoRedo/ComponentCommands.h"
		"UndoRedo/SpecializedCommands.h"
		
		"Panels/AnimationPanel.cpp"
		"Panels/AnimationPanel.h"
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
		"Panels/SceneStatisticsPanel.cpp"
		"Panels/SceneStatisticsPanel.h"
		"Panels/EditorPreferencesPanel.cpp"
		"Panels/EditorPreferencesPanel.h"
		"Panels/DialogueEditorPanel.cpp"
		"Panels/DialogueEditorPanel.h"
		"Panels/InputSettingsPanel.cpp"
		"Panels/InputSettingsPanel.h"
		"Panels/PostProcessSettingsPanel.cpp"
		"Panels/PostProcessSettingsPanel.h"
		"Panels/RendererSettingsPanel.cpp"
		"Panels/RendererSettingsPanel.h"
		"Panels/SceneHierarchyPanel.cpp"
		"Panels/SceneHierarchyPanel.h"
		"Panels/TerrainEditorPanel.cpp"
		"Panels/TerrainEditorPanel.h"
		"Panels/StreamingPanel.cpp"
		"Panels/StreamingPanel.h"
		"Panels/NetworkDebugPanel.cpp"
		"Panels/NetworkDebugPanel.h"
		"Panels/SaveGamePanel.cpp"
		"Panels/SaveGamePanel.h"
		"Panels/ShaderGraphEditorPanel.cpp"
		"Panels/ShaderGraphEditorPanel.h"
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

add_executable(OloEditor
		${EDITOR_SOURCES}
		${GLSL_SOURCE_FILES}
		"${CMAKE_CURRENT_SOURCE_DIR}/../Resources/OloEditor.rc"
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
			FILES ${EDITOR_SOURCES}
)

# Post Build Commands
add_custom_command(TARGET OloEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/OloEngine/vendor/libpng-build/$<CONFIG>/libpng16$<$<CONFIG:Debug>:d>.dll"
        $<TARGET_FILE_DIR:OloEditor>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/OloEngine/vendor/zlib-build/$<CONFIG>/zlib$<$<CONFIG:Debug>:d>.dll"
        $<TARGET_FILE_DIR:OloEditor>)

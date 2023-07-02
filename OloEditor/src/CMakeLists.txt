set(EDITOR_SOURCES
		"EditorLayer.cpp"
		"EditorLayer.h"
		"OloEditorApp.cpp"
		
		"Panels/ContentBrowserPanel.cpp"
		"Panels/ContentBrowserPanel.h"
		"Panels/SceneHierarchyPanel.cpp"
		"Panels/SceneHierarchyPanel.h"
)

file(GLOB_RECURSE GLSL_SOURCE_FILES "../assets/shaders/*.glsl")

source_group(Shaders FILES ${GLSL_SOURCE_FILES})

add_executable(OloEditor
		${EDITOR_SOURCES}
		${GLSL_SOURCE_FILES}
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
			FILES ${EDITOR_SOURCES}
)
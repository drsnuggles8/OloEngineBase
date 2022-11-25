set(EDITOR_SOURCES
		"EditorLayer.cpp"
		"EditorLayer.h"
		"OloEditorApp.cpp"
		
		"Panels/ContentBrowserPanel.cpp"
		"Panels/ContentBrowserPanel.h"
		"Panels/SceneHierarchyPanel.cpp"
		"Panels/SceneHierarchyPanel.h"
)

add_executable("OloEditor"
		${EDITOR_SOURCES}
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
			FILES ${EDITOR_SOURCES}
)
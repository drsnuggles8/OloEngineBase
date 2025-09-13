set(EDITOR_SOURCES
		"EditorLayer.cpp"
		"EditorLayer.h"
		"OloEditorApp.cpp"
		
		"Panels/ContentBrowserPanel.cpp"
		"Panels/ContentBrowserPanel.h"
		"Panels/SceneHierarchyPanel.cpp"
		"Panels/SceneHierarchyPanel.h"
		"Panels/AssetPackBuilderPanel.cpp"
		"Panels/AssetPackBuilderPanel.h"
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

# Post Build Commands
add_custom_command(TARGET OloEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/OloEngine/vendor/libpng-build/$<CONFIG>/libpng16$<$<CONFIG:Debug>:d>.dll"
        $<TARGET_FILE_DIR:OloEditor>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/OloEngine/vendor/zlib-build/$<CONFIG>/zlib$<$<CONFIG:Debug>:d>.dll"
        $<TARGET_FILE_DIR:OloEditor>)

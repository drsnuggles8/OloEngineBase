set(SOURCES
		"OloEngine/Input.cs"
		"OloEngine/InternalCalls.cs"
		"OloEngine/KeyCode.cs"
		"OloEngine/Vector2.cs"
		"OloEngine/Vector3.cs"
		
		"OloEngine/Scene/Components.cs"
		"OloEngine/Scene/Entity.cs"
)

add_library(OloEngine-ScriptCore SHARED
		${SOURCES}
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
			FILES ${SOURCES}
)

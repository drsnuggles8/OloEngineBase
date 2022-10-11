set(SOURCES
		"Camera.cs"
		"Player.cs"
)

add_library("Sandbox-Scripting" SHARED
		${SOURCES}
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
			FILES ${SOURCES}
)

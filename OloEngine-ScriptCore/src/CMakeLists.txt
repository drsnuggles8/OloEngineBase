set(SOURCES
		"Main.cs"
)

add_library(OloEngine-ScriptCore SHARED
		${SOURCES}
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
			FILES ${SOURCES}
)

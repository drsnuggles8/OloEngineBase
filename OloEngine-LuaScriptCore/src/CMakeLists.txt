set(SOURCES
		"Main.cpp"
)

add_library(OloEngine-LuaScriptCore SHARED
		${SOURCES}
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
			FILES ${SOURCES}
)

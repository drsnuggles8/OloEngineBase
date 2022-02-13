file (GLOB_RECURSE source *.cpp)
file (GLOB_RECURSE header *.h)

add_library(OloEngine STATIC
		${source}
		${header}
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
			FILES ${source} ${header}
)

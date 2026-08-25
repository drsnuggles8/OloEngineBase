set(SOURCES
		"OpenVDBVolumeCook.h"
		"OpenVDBVolumeCook.cpp"
)

add_library(OloEngine-VolumeCook STATIC
		${SOURCES}
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
			FILES ${SOURCES}
)

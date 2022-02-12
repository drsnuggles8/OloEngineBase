file (GLOB_RECURSE source *.cpp)
file (GLOB_RECURSE header *.h)

add_library(OloEngine STATIC
		${source}
		${header}
)

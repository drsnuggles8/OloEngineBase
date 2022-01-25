file (GLOB_RECURSE source *.cpp)
file (GLOB_RECURSE header *.h)

add_executable("OloEditor"
        ${source}
        ${header}
)
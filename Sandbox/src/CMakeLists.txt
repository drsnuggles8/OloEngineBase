file (GLOB_RECURSE source *.cpp)
file (GLOB_RECURSE header *.h)

add_executable(Sandbox
        ${source}
        ${header}
)


# Post Build Commands
add_custom_command(TARGET Sandbox POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/OloEngine/vendor/libpng-build/$<CONFIG>/libpng16$<$<CONFIG:Debug>:d>.dll"
        $<TARGET_FILE_DIR:Sandbox>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/OloEngine/vendor/zlib-build/$<CONFIG>/zlib$<$<CONFIG:Debug>:d>.dll"
        $<TARGET_FILE_DIR:Sandbox>)

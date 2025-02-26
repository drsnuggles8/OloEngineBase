set(Sandbox2D_SOURCES
		"Sandbox2D.cpp"
		"Sandbox2D.h"
		"SandboxApp.cpp"
)

add_executable(Sandbox2D
        ${Sandbox2D_SOURCES}
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
			FILES ${Sandbox2D_SOURCES}
)


# Post Build Commands
add_custom_command(TARGET Sandbox2D POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/OloEngine/vendor/libpng-build/$<CONFIG>/libpng16$<$<CONFIG:Debug>:d>.dll"
        $<TARGET_FILE_DIR:Sandbox2D>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/OloEngine/vendor/zlib-build/$<CONFIG>/zlib$<$<CONFIG:Debug>:d>.dll"
        $<TARGET_FILE_DIR:Sandbox2D>)

set(SANDBOX3D_SOURCES
		"Sandbox3D.cpp"
		"Sandbox3D.h"
		"SandboxApp.cpp"
)

add_executable(Sandbox3D
        ${SANDBOX3D_SOURCES}
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}
			FILES ${SANDBOX3D_SOURCES}
)


# Post Build Commands
add_custom_command(TARGET Sandbox3D POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/OloEngine/vendor/libpng-build/$<CONFIG>/libpng16$<$<CONFIG:Debug>:d>.dll"
        $<TARGET_FILE_DIR:Sandbox3D>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/OloEngine/vendor/zlib-build/$<CONFIG>/zlib$<$<CONFIG:Debug>:d>.dll"
        $<TARGET_FILE_DIR:Sandbox3D>)

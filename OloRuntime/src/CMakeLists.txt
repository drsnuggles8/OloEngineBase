if(WIN32)
    add_executable(OloRuntime
        OloRuntimeApp.cpp
        "${CMAKE_CURRENT_SOURCE_DIR}/../Resources/OloRuntime.rc"
    )
else()
    add_executable(OloRuntime
        OloRuntimeApp.cpp
    )
endif()

# Post Build Commands - copy required DLLs
if(WIN32)
    add_custom_command(TARGET OloRuntime POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/OloEngine/vendor/libpng-build/$<CONFIG>/libpng16$<$<CONFIG:Debug>:d>.dll"
            $<TARGET_FILE_DIR:OloRuntime>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/OloEngine/vendor/zlib-build/$<CONFIG>/zlib$<$<CONFIG:Debug>:d>.dll"
            $<TARGET_FILE_DIR:OloRuntime>)
endif()

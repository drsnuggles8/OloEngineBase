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


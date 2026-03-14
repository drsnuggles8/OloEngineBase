# Custom FindProtobuf.cmake — redirects to FetchContent-built protobuf targets.
# Placed in cmake/ which is on CMAKE_MODULE_PATH, so this is found before
# CMake's built-in FindProtobuf.cmake.

# Helper: set all the variables the built-in module would set
macro(_olo_set_protobuf_vars)
    set(Protobuf_FOUND TRUE PARENT_SCOPE)
    set(PROTOBUF_FOUND TRUE PARENT_SCOPE)
    set(Protobuf_FOUND TRUE)
    set(PROTOBUF_FOUND TRUE)
    set(Protobuf_LIBRARIES protobuf::libprotobuf)
    set(PROTOBUF_LIBRARIES protobuf::libprotobuf)
    if(NOT Protobuf_INCLUDE_DIR)
        # Prefer the source dir variable set during FetchContent
        if(protobuf_SOURCE_DIR)
            set(Protobuf_INCLUDE_DIR "${protobuf_SOURCE_DIR}/src")
        else()
            # Fall back to querying the target, but strip generator expressions
            get_target_property(_proto_inc protobuf::libprotobuf INTERFACE_INCLUDE_DIRECTORIES)
            if(_proto_inc)
                foreach(_dir IN LISTS _proto_inc)
                    # Strip $<BUILD_INTERFACE:...> wrapper
                    string(REGEX REPLACE "^\\$<BUILD_INTERFACE:(.+)>$" "\\1" _dir_clean "${_dir}")
                    if(NOT _dir_clean MATCHES "^\\$<" AND IS_DIRECTORY "${_dir_clean}")
                        set(Protobuf_INCLUDE_DIR "${_dir_clean}")
                        break()
                    endif()
                endforeach()
            endif()
        endif()
    endif()
    set(Protobuf_INCLUDE_DIRS "${Protobuf_INCLUDE_DIR}")
    set(PROTOBUF_INCLUDE_DIR "${Protobuf_INCLUDE_DIR}")
    set(PROTOBUF_INCLUDE_DIRS "${Protobuf_INCLUDE_DIR}")
    if(TARGET protoc)
        set(Protobuf_PROTOC_EXECUTABLE "$<TARGET_FILE:protoc>")
        set(PROTOBUF_PROTOC_EXECUTABLE "$<TARGET_FILE:protoc>")
    endif()
endmacro()

# If protobuf targets already exist from FetchContent, use them
if(TARGET protobuf::libprotobuf)
    _olo_set_protobuf_vars()
    # Load built-in FindProtobuf ONLY for protobuf_generate_cpp()
    if(NOT COMMAND protobuf_generate_cpp)
        include(${CMAKE_ROOT}/Modules/FindProtobuf.cmake)
    endif()
    return()
endif()

# If non-namespaced libprotobuf target exists, create alias
if(TARGET libprotobuf AND NOT TARGET protobuf::libprotobuf)
    add_library(protobuf::libprotobuf ALIAS libprotobuf)
    _olo_set_protobuf_vars()
    if(NOT COMMAND protobuf_generate_cpp)
        include(${CMAKE_ROOT}/Modules/FindProtobuf.cmake)
    endif()
    return()
endif()

# Fall back to system search
include(${CMAKE_ROOT}/Modules/FindProtobuf.cmake)

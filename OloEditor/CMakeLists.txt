project(OloEditor
        LANGUAGES CXX)

set(OloEditor_VERSION_MAJOR 0)
set(OloEditor_VERSION_MINOR 0)
set(OloEditor_VERSION_PATCH 1)
set(OloEditor_VERSION
${OloEditor_VERSION_MAJOR}.${OloEditor_VERSION_MINOR}.${OloEditor_VERSION_PATCH}
)

# Main targets built by this project
add_subdirectory(src)

set_target_properties("OloEditor" PROPERTIES
                      CXX_STANDARD 20
                      CXX_STANDARD_REQUIRED YES
                      CXX_EXTENSIONS NO
                      POSITION_INDEPENDENT_CODE False
                      INTERPROCEDURAL_OPTIMIZATION False
)

#Rather than referring to CMAKE_BUILD_TYPE in
#the CMakeLists.txt file, projects should instead use other more robust alternative techniques, such
#as generator expressions based on $<CONFIG:…>
#NOT SURE IF THIS WHOLE SHIT IS EVEN NEEDED
# FIX THIS SECTION, COMBINE SHIT AND STUFF

if(CMAKE_BUILD_TYPE STREQUAL Debug)
  set_target_properties("OloEditor" PROPERTIES
    OUTPUT_NAME "OloEditor"
    ARCHIVE_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Debug-windows-x86_64/OloEditor"
    LIBRARY_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Debug-windows-x86_64/OloEditor"
    RUNTIME_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Debug-windows-x86_64/OloEditor"
  )
endif()
if(CMAKE_BUILD_TYPE STREQUAL Release)
  set_target_properties("OloEditor" PROPERTIES
    OUTPUT_NAME "OloEditor"
    ARCHIVE_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Release-windows-x86_64/OloEditor"
    LIBRARY_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Release-windows-x86_64/OloEditor"
    RUNTIME_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Release-windows-x86_64/OloEditor"
  )
endif()
if(CMAKE_BUILD_TYPE STREQUAL Dist)
  set_target_properties("OloEditor" PROPERTIES
    OUTPUT_NAME "OloEditor"
    ARCHIVE_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Dist-windows-x86_64/OloEditor"
    LIBRARY_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Dist-windows-x86_64/OloEditor"
    RUNTIME_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Dist-windows-x86_64/OloEditor"
  )
endif()

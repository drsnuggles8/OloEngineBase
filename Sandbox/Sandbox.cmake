add_executable("Sandbox"
  "Sandbox/src/ExampleLayer.cpp"
  "Sandbox/src/ExampleLayer.h"
  "Sandbox/src/Sandbox2D.cpp"
  "Sandbox/src/Sandbox2D.h"
  "Sandbox/src/SandboxApp.cpp"
)
if(CMAKE_BUILD_TYPE STREQUAL Debug)
  add_dependencies("Sandbox"
    "OloEngine"
  )
  set_target_properties("Sandbox" PROPERTIES
    OUTPUT_NAME "Sandbox"
    ARCHIVE_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Debug-windows-x86_64/Sandbox"
    LIBRARY_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Debug-windows-x86_64/Sandbox"
    RUNTIME_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Debug-windows-x86_64/Sandbox"
  )
endif()
target_include_directories("Sandbox" PRIVATE
  $<$<CONFIG:Debug>:C:/Users/ole/source/repos/OloEngine/OloEngine/vendor/spdlog/include>
  $<$<CONFIG:Debug>:C:/Users/ole/source/repos/OloEngine/OloEngine/src>
  $<$<CONFIG:Debug>:C:/Users/ole/source/repos/OloEngine/OloEngine/vendor>
  $<$<CONFIG:Debug>:C:/Users/ole/source/repos/OloEngine/OloEngine/vendor/glm>
  $<$<CONFIG:Debug>:C:/Users/ole/source/repos/OloEngine/OloEngine/vendor/entt/include>
)
target_compile_definitions("Sandbox" PRIVATE
  $<$<CONFIG:Debug>:OLO_DEBUG>
)
target_link_directories("Sandbox" PRIVATE
)
target_link_libraries("Sandbox"
  $<$<CONFIG:Debug>:OloEngine>
)
target_compile_options("Sandbox" PRIVATE
  $<$<AND:$<CONFIG:Debug>,$<COMPILE_LANGUAGE:C>>:-m64>
  $<$<AND:$<CONFIG:Debug>,$<COMPILE_LANGUAGE:C>>:-g>
  $<$<AND:$<CONFIG:Debug>,$<COMPILE_LANGUAGE:CXX>>:-m64>
  $<$<AND:$<CONFIG:Debug>,$<COMPILE_LANGUAGE:CXX>>:-g>
  $<$<AND:$<CONFIG:Debug>,$<COMPILE_LANGUAGE:CXX>>:-std=c++20>
)
if(CMAKE_BUILD_TYPE STREQUAL Debug)
  set_target_properties("Sandbox" PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE False
    INTERPROCEDURAL_OPTIMIZATION False
  )
endif()
if(CMAKE_BUILD_TYPE STREQUAL Release)
  add_dependencies("Sandbox"
    "OloEngine"
  )
  set_target_properties("Sandbox" PROPERTIES
    OUTPUT_NAME "Sandbox"
    ARCHIVE_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Release-windows-x86_64/Sandbox"
    LIBRARY_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Release-windows-x86_64/Sandbox"
    RUNTIME_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Release-windows-x86_64/Sandbox"
  )
endif()
target_include_directories("Sandbox" PRIVATE
  $<$<CONFIG:Release>:C:/Users/ole/source/repos/OloEngine/OloEngine/vendor/spdlog/include>
  $<$<CONFIG:Release>:C:/Users/ole/source/repos/OloEngine/OloEngine/src>
  $<$<CONFIG:Release>:C:/Users/ole/source/repos/OloEngine/OloEngine/vendor>
  $<$<CONFIG:Release>:C:/Users/ole/source/repos/OloEngine/OloEngine/vendor/glm>
  $<$<CONFIG:Release>:C:/Users/ole/source/repos/OloEngine/OloEngine/vendor/entt/include>
)
target_compile_definitions("Sandbox" PRIVATE
  $<$<CONFIG:Release>:OLO_RELEASE>
)
target_link_directories("Sandbox" PRIVATE
)
target_link_libraries("Sandbox"
  $<$<CONFIG:Release>:OloEngine>
)
target_compile_options("Sandbox" PRIVATE
  $<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C>>:-m64>
  $<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C>>:-O2>
  $<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:CXX>>:-m64>
  $<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:CXX>>:-O2>
  $<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:CXX>>:-std=c++20>
)
if(CMAKE_BUILD_TYPE STREQUAL Release)
  set_target_properties("Sandbox" PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE False
    INTERPROCEDURAL_OPTIMIZATION False
  )
endif()
if(CMAKE_BUILD_TYPE STREQUAL Dist)
  add_dependencies("Sandbox"
    "OloEngine"
  )
  set_target_properties("Sandbox" PROPERTIES
    OUTPUT_NAME "Sandbox"
    ARCHIVE_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Dist-windows-x86_64/Sandbox"
    LIBRARY_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Dist-windows-x86_64/Sandbox"
    RUNTIME_OUTPUT_DIRECTORY "C:/Users/ole/source/repos/OloEngine/bin/Dist-windows-x86_64/Sandbox"
  )
endif()
target_include_directories("Sandbox" PRIVATE
  $<$<CONFIG:Dist>:C:/Users/ole/source/repos/OloEngine/OloEngine/vendor/spdlog/include>
  $<$<CONFIG:Dist>:C:/Users/ole/source/repos/OloEngine/OloEngine/src>
  $<$<CONFIG:Dist>:C:/Users/ole/source/repos/OloEngine/OloEngine/vendor>
  $<$<CONFIG:Dist>:C:/Users/ole/source/repos/OloEngine/OloEngine/vendor/glm>
  $<$<CONFIG:Dist>:C:/Users/ole/source/repos/OloEngine/OloEngine/vendor/entt/include>
)
target_compile_definitions("Sandbox" PRIVATE
  $<$<CONFIG:Dist>:OLO_DIST>
)
target_link_directories("Sandbox" PRIVATE
)
target_link_libraries("Sandbox"
  $<$<CONFIG:Dist>:OloEngine>
)
target_compile_options("Sandbox" PRIVATE
  $<$<AND:$<CONFIG:Dist>,$<COMPILE_LANGUAGE:C>>:-m64>
  $<$<AND:$<CONFIG:Dist>,$<COMPILE_LANGUAGE:C>>:-O2>
  $<$<AND:$<CONFIG:Dist>,$<COMPILE_LANGUAGE:CXX>>:-m64>
  $<$<AND:$<CONFIG:Dist>,$<COMPILE_LANGUAGE:CXX>>:-O2>
  $<$<AND:$<CONFIG:Dist>,$<COMPILE_LANGUAGE:CXX>>:-std=c++20>
)
if(CMAKE_BUILD_TYPE STREQUAL Dist)
  set_target_properties("Sandbox" PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE False
    INTERPROCEDURAL_OPTIMIZATION False
  )
endif()
project(OloEngine-ScriptCore
		VERSION 0.0.1
		LANGUAGES CSharp)

# Main targets built by this project
add_subdirectory(src)

# Configure the C# project with our common properties
olo_configure_csharp_project(OloEngine-ScriptCore ${CMAKE_SOURCE_DIR}/OloEditor/Resources/Scripts)

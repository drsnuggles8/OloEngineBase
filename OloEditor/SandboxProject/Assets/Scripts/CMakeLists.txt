# The Sandbox project's game scripts (Sandbox-Scripting.dll), built by `dotnet build` on the
# tracked Sandbox-Scripting.csproj (cmake/CSharpAssembly.cmake, #1405). The csproj writes the
# assembly to Binaries/, the ScriptModulePath in Sandbox.oloproj.
olo_add_csharp_assembly(Sandbox-Scripting
    PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/Sandbox-Scripting.csproj"
    OUTPUT  "${CMAKE_CURRENT_SOURCE_DIR}/Binaries/Sandbox-Scripting.dll"
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Source/*.cs"
    # It compiles against the built OloEngine-ScriptCore.dll, so a rebuilt engine API rebuilds it.
    DEPENDS OloEngine-ScriptCore "${CMAKE_SOURCE_DIR}/OloEditor/Resources/Scripts/OloEngine-ScriptCore.dll"
)

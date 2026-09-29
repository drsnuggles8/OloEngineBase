# The engine's C# API (OloEngine-ScriptCore.dll), built by `dotnet build` on the tracked
# OloEngine-ScriptCore.csproj under any generator (cmake/CSharpAssembly.cmake, #1405). The
# csproj writes the assembly straight into OloEditor/Resources/Scripts/, where ScriptEngine
# loads it.
olo_add_csharp_assembly(OloEngine-ScriptCore
    PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/OloEngine-ScriptCore.csproj"
    OUTPUT  "${CMAKE_SOURCE_DIR}/OloEditor/Resources/Scripts/OloEngine-ScriptCore.dll"
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cs"
    # OloHeaderTool writes Components.Generated.cs and InternalCalls.Generated.cs. The target
    # edge orders codegen first; the stamp is the file-level input that makes a regenerated
    # .cs recompile in the same build (see OLO_STAMP in tools/OloHeaderTool/CMakeLists.txt).
    DEPENDS "$<TARGET_PROPERTY:GenerateBindings,OLO_STAMP>"
)
add_dependencies(OloEngine-ScriptCore GenerateBindings)

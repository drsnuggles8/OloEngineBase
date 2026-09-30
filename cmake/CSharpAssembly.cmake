# C# assemblies for the embedded Mono runtime, built by `dotnet build` under any generator (#1405).
#
# THE ONE WAY. Each assembly is a tracked SDK-style .csproj (OloEngine-ScriptCore/, and the
# Sandbox project's Assets/Scripts/) and this file drives it with `dotnet build`. That works
# under Ninja, Ninja Multi-Config and the Visual Studio generators alike, and from an IDE with
# no CMake at all. It replaced CMake's own `CSharp` language, which exists only for the Visual
# Studio generators: every CI job and the default dev-cached preset use Ninja, so the tracked
# OloHeaderTool output (Components.Generated.cs, InternalCalls.Generated.cs) was compiled by
# nothing that runs on a pull request, and invalid generated C# reached master green.
#
# References come from the Mono class libraries vendored in OloEditor/mono/lib/mono/4.5, set
# in OloEngine-ScriptCore/OloEngine.CSharp.props. No .NET Framework targeting pack, no NuGet.
#
# OLO_WITH_CSHARP defaults ON on Windows, the only platform with C# scripting
# (IsScriptingAvailableOnPlatform, ADR 0015). ON without a usable .NET SDK is a configure
# error, not a quiet skip: a skipped assembly is exactly the silent
# "[ScriptEngine] OloEngine-ScriptCore assembly unavailable" this file exists to remove.

option(OLO_WITH_CSHARP "Build the C# scripting assemblies (OloEngine-ScriptCore, Sandbox-Scripting) with the .NET SDK" ${WIN32})

if(OLO_WITH_CSHARP)
    find_program(OLO_DOTNET_EXECUTABLE dotnet)
    if(NOT OLO_DOTNET_EXECUTABLE)
        message(FATAL_ERROR
            "OLO_WITH_CSHARP=ON, but no `dotnet` was found on PATH. The C# scripting assemblies are "
            "built with the .NET SDK (8 or newer): install it from "
            "https://dotnet.microsoft.com/download, or configure with -DOLO_WITH_CSHARP=OFF to build "
            "without C# scripting.")
    endif()
    execute_process(
        COMMAND "${OLO_DOTNET_EXECUTABLE}" --list-sdks
        OUTPUT_VARIABLE _olo_dotnet_sdks
        ERROR_QUIET
        RESULT_VARIABLE _olo_dotnet_result
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _olo_dotnet_result EQUAL 0 OR _olo_dotnet_sdks STREQUAL "")
        message(FATAL_ERROR
            "OLO_WITH_CSHARP=ON, but `${OLO_DOTNET_EXECUTABLE} --list-sdks` lists no .NET SDK (a "
            "runtime-only install cannot build). Install the .NET SDK, or configure with "
            "-DOLO_WITH_CSHARP=OFF to build without C# scripting.")
    endif()
    string(REPLACE "\n" "; " _olo_dotnet_sdks "${_olo_dotnet_sdks}")
    message(STATUS "C# scripting assemblies: dotnet build with ${OLO_DOTNET_EXECUTABLE} (SDKs: ${_olo_dotnet_sdks})")
else()
    message(STATUS "C# scripting assemblies: OFF (OLO_WITH_CSHARP=OFF)")
endif()

# olo_add_csharp_assembly(<target>
#     PROJECT  <path to .csproj>
#     OUTPUT   <path of the assembly the csproj writes>
#     SOURCES  <glob patterns for every .cs the csproj compiles>
#     [DEPENDS <files or targets the build must see first>])
#
# Adds <target>, which runs `dotnet build -c <config>` on PROJECT. Incremental in two layers:
# the build graph re-runs the command only when the csproj, the shared props, a matched .cs
# or a DEPENDS entry changed, and MSBuild then skips the compile itself if its inputs did not.
#
# The declared OUTPUT is a per-config marker and the assembly is a BYPRODUCT. MSBuild leaves an
# up-to-date assembly untouched, so the assembly alone as an OUTPUT would stay older than its
# inputs and re-run the command on every build under the Visual Studio generator, which has no
# equivalent of Ninja's restat. The byproduct still makes a deleted assembly rebuild.
#
# The marker sits NEXT TO THE ASSEMBLY, not in the build tree, because the assembly path is
# shared by every configuration and every build tree (the engine loads one fixed path). Each
# build deletes the other configurations' markers, so a Debug build re-runs after a Release
# build of the same assembly and puts a Debug assembly back. Within one Ninja tree the shared
# byproduct's .ninja_log entry already forces that (it carries the other config's command);
# the marker is what catches it across build trees (build-cached/ then build/) and under the
# Visual Studio generator, which share no log. MSBuild's copy to OutDir then replaces the other
# configuration's file even when its own compile is current.
#
# SOURCES are globbed with CONFIGURE_DEPENDS, so adding or deleting a .cs re-globs. Pass the
# same tree the csproj's single Compile glob names; AssetCSharpScriptValidityTest pins that
# glob, so the two cannot drift apart the way the old hand-kept CMake SOURCES lists did.
function(olo_add_csharp_assembly target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "PROJECT;OUTPUT" "SOURCES;DEPENDS")
    if(NOT arg_PROJECT OR NOT arg_OUTPUT OR NOT arg_SOURCES)
        message(FATAL_ERROR "olo_add_csharp_assembly(${target}): PROJECT, OUTPUT and SOURCES are required")
    endif()

    file(GLOB_RECURSE _sources CONFIGURE_DEPENDS ${arg_SOURCES})
    if(NOT _sources)
        message(FATAL_ERROR "olo_add_csharp_assembly(${target}): no .cs file matches ${arg_SOURCES}")
    endif()

    set(_props "${CMAKE_SOURCE_DIR}/OloEngine-ScriptCore/OloEngine.CSharp.props")
    set(_obj "${CMAKE_CURRENT_BINARY_DIR}/${target}.obj/")
    # A single-config generator with no CMAKE_BUILD_TYPE has an empty $<CONFIG>; build Debug.
    set(_config "$<IF:$<BOOL:$<CONFIG>>,$<CONFIG>,Debug>")
    get_filename_component(_outdir "${arg_OUTPUT}" DIRECTORY)
    set(_marker "${_outdir}/.${target}.${_config}.built")
    set(_all_configs Debug Release Dist ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    list(REMOVE_DUPLICATES _all_configs)
    set(_all_markers "")
    foreach(_c IN LISTS _all_configs)
        list(APPEND _all_markers "${_outdir}/.${target}.${_c}.built")
    endforeach()

    add_custom_command(
        OUTPUT "${_marker}"
        BYPRODUCTS "${arg_OUTPUT}"
        # BaseIntermediateOutputPath keeps obj/ in the build tree, one per tree, so two build
        # trees never share restore or compile state. It must be a global property: MSBuild
        # reads it before the project body is evaluated.
        COMMAND "${OLO_DOTNET_EXECUTABLE}" build "${arg_PROJECT}"
                --configuration ${_config}
                --nologo
                --verbosity minimal
                "-p:BaseIntermediateOutputPath=${_obj}"
                # No MSBuild worker nodes or compiler server left running after the build: they
                # outlive the build step, hold files in the tree and are invisible to the
                # build lock that bounds concurrent builds on a shared box.
                -nodeReuse:false
                -p:UseSharedCompilation=false
        COMMAND "${CMAKE_COMMAND}" -E rm -f ${_all_markers}
        COMMAND "${CMAKE_COMMAND}" -E touch "${_marker}"
        DEPENDS "${arg_PROJECT}" "${_props}" ${_sources} ${arg_DEPENDS}
        COMMENT "Building C# assembly ${target} (dotnet build, ${_config})"
        VERBATIM
    )
    add_custom_target(${target} DEPENDS "${_marker}" SOURCES "${arg_PROJECT}" ${_sources})
    set_target_properties(${target} PROPERTIES FOLDER "Scripting")
endfunction()

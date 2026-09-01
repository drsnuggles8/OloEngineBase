project(OloGgxEnergyTableGen LANGUAGES CXX)

# The GGX energy-table baker (issue #998). Unlike OloHeaderTool this is NOT
# wired into the build graph: it is run by hand when the tables need rebaking,
# and its outputs are committed source. Nothing depends on it, so a normal build
# does not pay for it — but it lives in the default target set so a plain build
# still catches a compile break here rather than deferring it to the next
# regeneration, months later, when the tables are already suspect.

add_executable(OloGgxEnergyTableGen main.cpp)

# It reuses the engine's OWN estimator (ReferenceBRDF.h + PathSampler.h), which
# is the entire reason it is a C++ tool rather than a script: generator-vs-engine
# drift becomes a compile-time impossibility instead of a thing to test for.
# Those headers are header-only and pull in Core/Base.h (spdlog via Log.h) and
# glm, so the include path plus two interface libraries is the whole dependency.
target_include_directories(OloGgxEnergyTableGen PRIVATE "${CMAKE_SOURCE_DIR}/OloEngine/src")

# find_package here rather than relying on OloEngine/vendor's calls: this
# directory is added BEFORE OloEngine so those calls have not run yet, and a
# tool that silently depends on subdirectory ordering is a configure-time trap
# of exactly the kind configure-time-variable-visibility.md is about.
find_package(glm CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
target_link_libraries(OloGgxEnergyTableGen PRIVATE glm::glm spdlog::spdlog)

set_target_properties(OloGgxEnergyTableGen PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    FOLDER "Tools"
)

#pragma once

// =============================================================================
// ShaderSourceScan.h — reading a GLSL constant out of the shader tree, for the
// contract tests that pin a C++ value against its GLSL twin.
//
// It exists because the DI and GI contract tests each carried a copy and the
// copies had already diverged: one resolved the shader tree from
// OLO_TEST_EDITOR_ROOT and the other walked up from the current directory. That
// is not a style difference. Several suites chdir during a full run, so the
// walking copy could resolve to a different worktree's shaders — reporting "the
// C++ and GLSL constants disagree" when what actually happened is that it
// scanned another branch, and passing in a filtered run while failing in a full
// one.
//
// Every scanner returns a LOUD miss rather than a plausible one: ~0u for an
// integer, a NaN for a float. A missing constant must fail the comparison it
// was written for, not quietly compare two zeroes.
// =============================================================================

#include "OloEngine/Core/Base.h"

#include <filesystem>
#include <fstream>
#include <limits>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>

namespace OloEngine::Tests::ShaderScan
{
    // THE COMPILE-TIME ANCHOR, NOT THE WORKING DIRECTORY. These shaders are
    // tracked repo assets: they are always present, so failing to find one is a
    // broken checkout or a moved file, never an environment a test should
    // excuse itself from with a skip.
    [[nodiscard]] inline std::filesystem::path ResolveShaderPath(std::string_view relative)
    {
        return std::filesystem::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders" / relative;
    }

    [[nodiscard]] inline std::string ReadTextFile(const std::filesystem::path& path)
    {
        std::ifstream file(path);
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    // `#define NAME 12u` or `#define NAME 12`.
    [[nodiscard]] inline u32 ScanDefine(const std::string& source, const char* name)
    {
        const std::regex pattern(std::string("#define\\s+") + name + "\\s+(\\d+)u?");
        std::smatch match;
        if (!std::regex_search(source, match, pattern))
            return ~0u;
        return static_cast<u32>(std::stoul(match[1].str()));
    }

    // `const uint NAME = 12u;`
    [[nodiscard]] inline u32 ScanConstUint(const std::string& source, const char* name)
    {
        const std::regex pattern(std::string("const\\s+uint\\s+") + name + "\\s*=\\s*(\\d+)u?\\s*;");
        std::smatch match;
        if (!std::regex_search(source, match, pattern))
            return ~0u;
        return static_cast<u32>(std::stoul(match[1].str()));
    }

    // `const float NAME = 4095.0;`. A NaN on no match rather than a sentinel
    // value: every float this scans has a legitimate value a sentinel could
    // collide with, and a NaN fails EXPECT_FLOAT_EQ loudly.
    [[nodiscard]] inline f32 ScanConstFloat(const std::string& source, const char* name)
    {
        const std::regex pattern(std::string("const\\s+float\\s+") + name +
                                 "\\s*=\\s*(-?[0-9.eE+-]+)f?\\s*;");
        std::smatch match;
        if (!std::regex_search(source, match, pattern))
            return std::numeric_limits<f32>::quiet_NaN();
        return std::stof(match[1].str());
    }
} // namespace OloEngine::Tests::ShaderScan

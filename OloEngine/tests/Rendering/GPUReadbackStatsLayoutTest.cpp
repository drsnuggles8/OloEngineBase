// OLO_TEST_LAYER: shaderpipe
//
// The two-place contract behind the GPU readback-stats channel (issue #721).
//
// The channel's counter registry lives in C++ (GPUReadbackStatsRegistry.h) and
// in GLSL (include/GPUReadbackStats.glsl). Nothing generates one from the other,
// so the two can drift — and drift here does not crash, does not fail to
// compile, and does not render wrong. It makes one counter report another
// counter's value: a plausible number, which for a diagnostic channel whose
// entire purpose is to be trusted is the worst possible failure.
//
// WHAT THIS TEST DELIBERATELY IS NOT. #847 had to fix
// `CrossShaderUBOMemberOffsetsAgree`, which compared `("", 0)` against `("", 0)`
// for every member because both parsers silently returned nothing — it was
// green and structurally incapable of failing. Every parse below is therefore
// asserted NON-EMPTY and asserted against an independently known count before
// anything is compared, so a parser that stops matching fails loudly instead of
// passing vacuously. The three self-checks at the top of each test are the
// point, not boilerplate.

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/Debug/GPUReadbackStatsRegistry.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace OloEngine;

namespace
{
    std::string ReadEditorFile(const std::filesystem::path& relative)
    {
        const auto path = std::filesystem::path{ OLO_TEST_EDITOR_ROOT } / relative;
        std::ifstream f(path, std::ios::binary);
        EXPECT_TRUE(f.is_open()) << "cannot open " << path.string();
        std::ostringstream buf;
        buf << f.rdbuf();
        return buf.str();
    }

    std::filesystem::path StatsGlslPath()
    {
        return std::filesystem::path{ "assets" } / "shaders" / "include" / "GPUReadbackStats.glsl";
    }

    // Pull every `const uint <prefix><Name> = <n>u;` out of the GLSL.
    std::map<std::string, u32> ParseGlslSlots(const std::string& source, const std::string& prefix)
    {
        std::map<std::string, u32> slots;
        const std::regex pattern{ R"(const\s+uint\s+)" + prefix + R"(([A-Za-z0-9_]+)\s*=\s*([0-9]+)u\s*;)" };
        for (std::sregex_iterator it{ source.begin(), source.end(), pattern }, end; it != end; ++it)
        {
            slots.emplace((*it)[1].str(), static_cast<u32>(std::stoul((*it)[2].str())));
        }
        return slots;
    }

    // The C++ side of the same map, built from the X-macro registry.
    std::map<std::string, u32> CppCounterSlots()
    {
        std::map<std::string, u32> slots;
        for (u32 i = 0; i < kGPUStatCounterCount; ++i)
        {
            slots.emplace(std::string{ GPUStatCounterName(static_cast<GPUStatCounter>(i)) }, i);
        }
        return slots;
    }

    std::map<std::string, u32> CppFlagSlots()
    {
        std::map<std::string, u32> slots;
        for (u32 i = 0; i < kGPUStatFlagCount; ++i)
        {
            slots.emplace(std::string{ GPUStatFlagName(static_cast<GPUStatFlag>(i)) }, i);
        }
        return slots;
    }
} // namespace

// The counter registry must agree, name for name and index for index.
TEST(GPUReadbackStatsLayout, CounterSlotsAgreeBetweenCppAndGlsl)
{
    const std::string glsl = ReadEditorFile(StatsGlslPath());
    ASSERT_FALSE(glsl.empty()) << "GPUReadbackStats.glsl is empty or unreadable";

    const auto glslSlots = ParseGlslSlots(glsl, "OLO_STAT_");
    const auto cppSlots = CppCounterSlots();

    // --- Anti-vacuity self-checks. A parser that matches nothing must FAIL,
    // not silently agree with an empty C++ side. ---
    ASSERT_FALSE(cppSlots.empty()) << "the C++ counter registry is empty — the X-macro list broke";
    ASSERT_FALSE(glslSlots.empty()) << "parsed ZERO OLO_STAT_* constants out of GPUReadbackStats.glsl — the "
                                       "declaration style changed and this test stopped checking anything";
    ASSERT_EQ(cppSlots.size(), static_cast<sizet>(kGPUStatCounterCount))
        << "duplicate counter NAME in OLO_GPU_STAT_COUNTERS — the map collapsed two entries";

    EXPECT_EQ(glslSlots.size(), cppSlots.size())
        << "GPUReadbackStats.glsl declares " << glslSlots.size() << " counter slots but the C++ registry has "
        << cppSlots.size();

    for (const auto& [name, cppIndex] : cppSlots)
    {
        const auto found = glslSlots.find(name);
        ASSERT_NE(found, glslSlots.end())
            << "counter '" << name << "' exists in OLO_GPU_STAT_COUNTERS but NOT as OLO_STAT_" << name
            << " in include/GPUReadbackStats.glsl — a shader cannot publish it";
        EXPECT_EQ(found->second, cppIndex)
            << "counter '" << name << "' is slot " << cppIndex << " in C++ but slot " << found->second
            << " in GLSL — every read of it returns another counter's value";
    }

    for (const auto& [name, glslIndex] : glslSlots)
    {
        EXPECT_NE(cppSlots.find(name), cppSlots.end())
            << "OLO_STAT_" << name << " (slot " << glslIndex
            << ") exists in GLSL but not in OLO_GPU_STAT_COUNTERS — anything a shader adds to it is unreadable";
    }
}

// The overflow-flag registry, same contract. Separate test so a flag-only drift
// names the flags rather than being buried in the counter diff.
TEST(GPUReadbackStatsLayout, FlagBitsAgreeBetweenCppAndGlsl)
{
    const std::string glsl = ReadEditorFile(StatsGlslPath());
    ASSERT_FALSE(glsl.empty());

    const auto glslFlags = ParseGlslSlots(glsl, "OLO_STATFLAG_");
    const auto cppFlags = CppFlagSlots();

    ASSERT_FALSE(cppFlags.empty()) << "the C++ flag registry is empty — the X-macro list broke";
    ASSERT_FALSE(glslFlags.empty()) << "parsed ZERO OLO_STATFLAG_* constants — this test stopped checking anything";
    ASSERT_EQ(cppFlags.size(), static_cast<sizet>(kGPUStatFlagCount)) << "duplicate flag NAME in OLO_GPU_STAT_FLAGS";

    EXPECT_EQ(glslFlags.size(), cppFlags.size());

    for (const auto& [name, cppBit] : cppFlags)
    {
        const auto found = glslFlags.find(name);
        ASSERT_NE(found, glslFlags.end()) << "flag '" << name << "' has no OLO_STATFLAG_" << name << " in GLSL";
        EXPECT_EQ(found->second, cppBit) << "flag '" << name << "' is bit " << cppBit << " in C++ but bit "
                                         << found->second << " in GLSL — the overlay would name the wrong condition";
    }

    for (const auto& [name, glslBit] : glslFlags)
    {
        EXPECT_NE(cppFlags.find(name), cppFlags.end())
            << "OLO_STATFLAG_" << name << " (bit " << glslBit << ") exists only in GLSL";
    }

    // A flag bit at >= 32 would silently shift out of the single atomicOr word.
    for (const auto& [name, bit] : glslFlags)
    {
        EXPECT_LT(bit, 32u) << "flag '" << name << "' is bit " << bit << ", past the one-word bitmask";
    }
}

// The block's shape — binding number and array length — is the OTHER half of the
// contract, and unlike the names it is not a text match anywhere else. A
// std430 array whose length disagrees with the C++ allocation is a silent layout
// mismatch, not a compile error.
TEST(GPUReadbackStatsLayout, BlockShapeMatchesTheCppAllocation)
{
    const std::string glsl = ReadEditorFile(StatsGlslPath());
    ASSERT_FALSE(glsl.empty());

    std::smatch match;
    const std::regex bindingPattern{ R"(layout\s*\(\s*std430\s*,\s*binding\s*=\s*([0-9]+)\s*\)\s*buffer\s+OloGpuReadbackStats)" };
    ASSERT_TRUE(std::regex_search(glsl, match, bindingPattern))
        << "could not find the OloGpuReadbackStats block declaration — this test stopped checking anything";
    EXPECT_EQ(static_cast<u32>(std::stoul(match[1].str())), ShaderBindingLayout::SSBO_GPU_STATS)
        << "include/GPUReadbackStats.glsl binds the stats block at a different number than "
           "ShaderBindingLayout::SSBO_GPU_STATS — every atomic lands in whatever is bound there instead";

    const std::regex slotsPattern{ R"(#define\s+OLO_STAT_COUNTER_SLOTS\s+([0-9]+))" };
    ASSERT_TRUE(std::regex_search(glsl, match, slotsPattern)) << "OLO_STAT_COUNTER_SLOTS is gone from the GLSL";
    EXPECT_EQ(static_cast<u32>(std::stoul(match[1].str())), kGPUStatCounterSlots)
        << "the GLSL counter array length disagrees with kGPUStatCounterSlots — std430 will happily lay the "
           "block out at the wrong size and the readback reads past/short of the counters";

    // Every declared slot must be inside the array. An out-of-range slot is an
    // out-of-bounds atomic, which GL is permitted to do anything at all with.
    for (const auto& [name, slot] : ParseGlslSlots(glsl, "OLO_STAT_"))
    {
        EXPECT_LT(slot, kGPUStatCounterSlots) << "counter '" << name << "' is slot " << slot << ", past the array";
    }
}

// SSBO 64 is TEX_DDGI_VISIBILITY in the sampler namespace. That is legal on GL
// (disjoint namespaces) and legal on Vulkan too, EXCEPT within one shader — the
// single-set model is exactly why TEX_DDGI_VISIBILITY had to move off 57 in
// issue #691 (ADR item A2), when DDGI_Capture.glsl became the
// first shader to both pull vertices (SSBO 57) and include DDGICommon.glsl
// (sampler 57).
//
// The SSBO namespace under the GL 4.6 minimum of 84 is full, so #721 had no
// other number to take. This test is what keeps the reuse safe: as long as no
// translation unit reaches both blocks, the collision cannot occur. If you are
// reading this because the test failed, the fix is to renumber one side — not
// to delete the assertion.
TEST(GPUReadbackStatsLayout, NoStatsConsumerAlsoSamplesBinding64)
{
    namespace fs = std::filesystem;
    const fs::path shaderRoot = fs::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders";
    ASSERT_TRUE(fs::exists(shaderRoot)) << "shader root not found at " << shaderRoot.string();

    // Resolve the transitive include set of one shader. One level of nesting is
    // enough here and is asserted below: the stats include has no includes of its
    // own, and DDGICommon.glsl is reached directly by everything that samples it.
    const auto includesOf = [](const std::string& source)
    {
        std::vector<std::string> names;
        // Custom delimiter: the pattern itself contains `)"`, which would close a
        // plain R"(...)" literal early.
        const std::regex pattern{ R"INC(#\s*include\s*"([^"]+)")INC" };
        for (std::sregex_iterator it{ source.begin(), source.end(), pattern }, end; it != end; ++it)
        {
            names.push_back(fs::path{ (*it)[1].str() }.filename().string());
        }
        return names;
    };

    u32 statsConsumers = 0;
    u32 scanned = 0;
    for (const auto& entry : fs::recursive_directory_iterator(shaderRoot))
    {
        if (!entry.is_regular_file())
            continue;
        const auto ext = entry.path().extension().string();
        if (ext != ".comp" && ext != ".glsl" && ext != ".vert" && ext != ".frag" && ext != ".geom")
            continue;

        std::ifstream f(entry.path(), std::ios::binary);
        std::ostringstream buf;
        buf << f.rdbuf();
        const std::string source = buf.str();
        ++scanned;

        bool includesStats = false;
        bool includesDDGI = false;
        for (const auto& name : includesOf(source))
        {
            includesStats = includesStats || name == "GPUReadbackStats.glsl";
            includesDDGI = includesDDGI || name == "DDGICommon.glsl";
        }
        if (includesStats)
            ++statsConsumers;

        EXPECT_FALSE(includesStats && includesDDGI)
            << entry.path().filename().string()
            << " includes BOTH GPUReadbackStats.glsl (SSBO 64) and DDGICommon.glsl (sampler 64). On Vulkan's "
               "single-set model that is a within-shader binding collision. Renumber one side.";
    }

    // Anti-vacuity again: a scan that walked nothing, or found no consumer at
    // all, proves nothing about the constraint it claims to enforce.
    ASSERT_GT(scanned, 0u) << "scanned no shader sources at all";
    EXPECT_GE(statsConsumers, 2u) << "expected at least the two adopter passes (#721 acceptance criterion 1) to "
                                     "include GPUReadbackStats.glsl; found "
                                  << statsConsumers;
}

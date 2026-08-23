// OLO_TEST_LAYER: shaderpipe
//
// =============================================================================
// The two-place contract behind GPU terrain picking (issue #717).
//
// `TerrainPickState` is declared in GLSL (include/TerrainPickCommon.glsl) and
// mirrored in C++ (`TerrainGPUPicker::PickStateHeader`). Nothing generates one
// from the other, and three separate consumers depend on the byte offsets being
// what both sides think they are:
//
//   * `DispatchComputeIndirect` reads the group counts at
//     kDescentDispatchOffset / kResolveDispatchOffset. A wrong offset there
//     dispatches whatever integers happen to sit at that address — usually a
//     plausible small number, so the pass runs and answers with a truncated
//     descent rather than failing.
//   * `CopyBufferSubData` copies exactly kResultBytes at kResultOffset into the
//     readback ring. A wrong offset returns the neighbouring counters as if they
//     were a hit distance, and `floatBitsToUint` of a small integer is a
//     denormal — i.e. a hit a fraction of a nanometre away, which reads as a
//     cursor pinned to the camera rather than as an error.
//   * One `SetData` writes the whole header, so a member that moved on one side
//     silently feeds the shader the wrong parameter.
//
// None of those three fail loudly. This test is what makes them fail at all.
//
// WHAT THIS TEST DELIBERATELY IS NOT. The layout is derived here from the GLSL
// text by applying the std430 rules, and compared against `offsetof` on the C++
// struct — two independent derivations. Every parse is asserted NON-EMPTY and
// against an independently known member count BEFORE anything is compared, so a
// parser that stops matching fails loudly rather than passing vacuously. That
// is not boilerplate: #847 had to fix `CrossShaderUBOMemberOffsetsAgree`, which
// compared ("", 0) against ("", 0) for every member and was structurally
// incapable of failing.
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Terrain/TerrainGPUPicker.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
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

    std::string PickCommonSource()
    {
        return ReadEditorFile(std::filesystem::path{ "assets" } / "shaders" / "include" / "TerrainPickCommon.glsl");
    }

    struct GlslMember
    {
        std::string Type;
        std::string Name;
        u32 Offset = 0;
    };

    // std430 base alignment / size for the handful of types this block uses.
    // Returns false for anything else, which is the point: a member type the
    // rules below do not cover must not be silently laid out as a guess.
    bool Std430Rule(const std::string& type, u32& outAlign, u32& outSize)
    {
        if (type == "uint" || type == "int" || type == "float")
        {
            outAlign = 4;
            outSize = 4;
            return true;
        }
        if (type == "uvec3" || type == "ivec3" || type == "vec3")
        {
            outAlign = 16;
            outSize = 12;
            return true;
        }
        if (type == "uvec4" || type == "ivec4" || type == "vec4")
        {
            outAlign = 16;
            outSize = 16;
            return true;
        }
        if (type == "uvec2" || type == "ivec2" || type == "vec2")
        {
            outAlign = 8;
            outSize = 8;
            return true;
        }
        return false;
    }

    // Pull the TerrainPickState block body out of the GLSL and lay its members
    // out under std430, INCLUDING the trailing runtime-sized array (whose offset
    // is the header size the C++ side writes in one SetData).
    std::vector<GlslMember> ParsePickStateBlock()
    {
        const std::string source = PickCommonSource();
        const auto blockStart = source.find("buffer TerrainPickState");
        EXPECT_NE(blockStart, std::string::npos) << "TerrainPickState block not found";
        if (blockStart == std::string::npos)
        {
            return {};
        }
        const auto open = source.find('{', blockStart);
        const auto close = source.find("} b_Pick;", open);
        EXPECT_NE(open, std::string::npos);
        EXPECT_NE(close, std::string::npos) << "the block must close with `} b_Pick;`";
        if (open == std::string::npos || close == std::string::npos)
        {
            return {};
        }

        const std::string body = source.substr(open + 1, close - open - 1);

        std::vector<GlslMember> members;
        u32 cursor = 0;
        // `<type> <name>;` or `<type> <name>[];` — one declaration per line, no
        // comma lists, which is how the block is written and how it must stay.
        const std::regex pattern{
            R"(^\s*(uint|int|float|vec2|vec3|vec4|uvec2|uvec3|uvec4|ivec2|ivec3|ivec4)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(\[\s*\])?\s*;)"
        };
        std::istringstream lines(body);
        for (std::string line; std::getline(lines, line);)
        {
            // Strip a trailing `// ...` comment before matching: the comments in
            // this block carry byte offsets, and a regex over the whole line
            // could otherwise match inside one.
            if (const auto comment = line.find("//"); comment != std::string::npos)
            {
                line = line.substr(0, comment);
            }
            std::smatch match;
            if (!std::regex_search(line, match, pattern))
            {
                continue;
            }

            const std::string type = match[1].str();
            u32 align = 0;
            u32 size = 0;
            EXPECT_TRUE(Std430Rule(type, align, size)) << "unhandled std430 type " << type;
            if (align == 0)
            {
                break;
            }
            cursor = (cursor + align - 1u) / align * align;
            members.push_back(GlslMember{ type, match[2].str(), cursor });
            // A runtime-sized array ends the block; its offset is what matters.
            if (match[3].matched)
            {
                break;
            }
            cursor += size;
        }
        return members;
    }

    // The C++ side of the same list, in declaration order, with offsets the
    // COMPILER computed rather than any this test typed.
    std::vector<std::pair<std::string, u32>> CppHeaderMembers()
    {
        using H = TerrainGPUPicker::PickStateHeader;
        return {
            { "PendingCount", static_cast<u32>(offsetof(H, PendingCount)) },
            { "NextCount", static_cast<u32>(offsetof(H, NextCount)) },
            { "CandidateCount", static_cast<u32>(offsetof(H, CandidateCount)) },
            { "OverflowFlags", static_cast<u32>(offsetof(H, OverflowFlags)) },
            { "DescentDispatch", static_cast<u32>(offsetof(H, DescentDispatch)) },
            { "_pickPad0", static_cast<u32>(offsetof(H, _Pad0)) },
            { "ResolveDispatch", static_cast<u32>(offsetof(H, ResolveDispatch)) },
            { "_pickPad1", static_cast<u32>(offsetof(H, _Pad1)) },
            { "HitTBits", static_cast<u32>(offsetof(H, HitTBits)) },
            { "ResultFlags", static_cast<u32>(offsetof(H, ResultFlags)) },
            { "RayId", static_cast<u32>(offsetof(H, RayId)) },
            { "_pickPad2", static_cast<u32>(offsetof(H, _Pad2)) },
            { "RayOriginAndMaxDist", static_cast<u32>(offsetof(H, RayOriginAndMaxDist)) },
            { "RayDirAndInflate", static_cast<u32>(offsetof(H, RayDirAndInflate)) },
            { "TerrainSizeAndScale", static_cast<u32>(offsetof(H, TerrainSizeAndScale)) },
            { "PickParams", static_cast<u32>(offsetof(H, PickParams)) },
        };
    }

    // A `#define <name> <value>u` in the pick include, so the sentinel and the
    // overflow bits can be compared against their C++ twins.
    u32 ParseGlslDefine(const std::string& source, const std::string& name)
    {
        const std::regex pattern{ R"(#define\s+)" + name + R"(\s+(0x[0-9A-Fa-f]+|[0-9]+)u)" };
        std::smatch match;
        EXPECT_TRUE(std::regex_search(source, match, pattern)) << "missing #define " << name;
        if (match.size() < 2)
        {
            return 0;
        }
        return static_cast<u32>(std::stoul(match[1].str(), nullptr, 0));
    }
} // namespace

// The header the C++ writes and the block the shaders read must lay out the
// same members at the same offsets, in the same order.
TEST(TerrainGPUPickerLayoutTest, GlslPickStateBlockMatchesTheCppHeader)
{
    const std::vector<GlslMember> glsl = ParsePickStateBlock();
    const std::vector<std::pair<std::string, u32>> cpp = CppHeaderMembers();

    // ---- anti-vacuous self-checks, before any comparison --------------------
    ASSERT_FALSE(glsl.empty()) << "the GLSL parse matched nothing — fix the parser, do not delete the test";
    // 16 header members + the trailing runtime-sized Candidates array.
    ASSERT_EQ(glsl.size(), 17u) << "TerrainPickState gained or lost a member; mirror it in PickStateHeader";
    ASSERT_EQ(cpp.size(), 16u);
    ASSERT_EQ(glsl.back().Name, "Candidates") << "the candidate array must stay the block's last member";

    for (sizet i = 0; i < cpp.size(); ++i)
    {
        EXPECT_EQ(glsl[i].Name, cpp[i].first) << "member " << i << " is named differently on the two sides";
        EXPECT_EQ(glsl[i].Offset, cpp[i].second) << "member '" << glsl[i].Name << "' sits at a different offset";
    }

    // The candidate array's offset IS the header size — the number one SetData
    // writes and the number the state buffer is sized from.
    EXPECT_EQ(glsl.back().Offset, TerrainGPUPicker::kHeaderBytes);
    EXPECT_EQ(sizeof(TerrainGPUPicker::PickStateHeader), TerrainGPUPicker::kHeaderBytes);
}

// The three offsets that are handed to the RHI rather than read by a shader.
TEST(TerrainGPUPickerLayoutTest, DispatchAndReadbackOffsetsPointAtTheRightMembers)
{
    const std::vector<GlslMember> glsl = ParsePickStateBlock();
    ASSERT_EQ(glsl.size(), 17u);

    auto offsetOf = [&glsl](const std::string& name) -> u32
    {
        for (const auto& member : glsl)
        {
            if (member.Name == name)
            {
                return member.Offset;
            }
        }
        ADD_FAILURE() << "no GLSL member named " << name;
        return 0xFFFFFFFFu;
    };

    EXPECT_EQ(offsetOf("DescentDispatch"), TerrainGPUPicker::kDescentDispatchOffset);
    EXPECT_EQ(offsetOf("ResolveDispatch"), TerrainGPUPicker::kResolveDispatchOffset);
    // The readback copies kResultBytes starting here, and must land exactly on
    // { HitTBits, ResultFlags, RayId, pad } — four uints, no more, no less.
    EXPECT_EQ(offsetOf("HitTBits"), TerrainGPUPicker::kResultOffset);
    EXPECT_EQ(offsetOf("ResultFlags"), TerrainGPUPicker::kResultOffset + 4u);
    EXPECT_EQ(offsetOf("RayId"), TerrainGPUPicker::kResultOffset + 8u);
    EXPECT_EQ(TerrainGPUPicker::kResultBytes, 16u);
}

// The sentinel and the overflow bits are compared by VALUE, not by name: a
// mismatch here makes every miss read as a hit at a denormal distance, or an
// overflow report the wrong cause.
TEST(TerrainGPUPickerLayoutTest, GlslConstantsMatchTheirCppTwins)
{
    const std::string source = PickCommonSource();
    ASSERT_FALSE(source.empty());

    EXPECT_EQ(ParseGlslDefine(source, "OLO_TERRAIN_PICK_NO_HIT"), TerrainGPUPicker::kNoHitBits);
    EXPECT_EQ(ParseGlslDefine(source, "OLO_TERRAIN_PICK_OVERFLOW_NODES"), TerrainGPUPicker::kOverflowNodes);
    EXPECT_EQ(ParseGlslDefine(source, "OLO_TERRAIN_PICK_OVERFLOW_CANDIDATES"), TerrainGPUPicker::kOverflowCandidates);

    // The no-hit sentinel has to be unreachable as a real answer: it must be
    // strictly above every finite positive float's bit pattern, or a far enough
    // hit would lose the atomicMin against it and be reported as a miss.
    constexpr u32 kFloatMaxBits = 0x7F7FFFFFu;
    EXPECT_GT(TerrainGPUPicker::kNoHitBits, kFloatMaxBits);
}

// TerrainPickArgs.comp hard-codes /64 when it turns a node count into a group
// count, which is only correct while the descent kernel really is 64 wide.
TEST(TerrainGPUPickerLayoutTest, DescentWorkgroupSizeMatchesTheArgsKernelsDivisor)
{
    const std::string descent =
        ReadEditorFile(std::filesystem::path{ "assets" } / "shaders" / "compute" / "TerrainRayNodeSelect.comp");
    const std::string args =
        ReadEditorFile(std::filesystem::path{ "assets" } / "shaders" / "compute" / "TerrainPickArgs.comp");
    ASSERT_FALSE(descent.empty());
    ASSERT_FALSE(args.empty());

    std::smatch match;
    const std::regex localSize{ R"(layout\(local_size_x\s*=\s*([0-9]+)\)\s*in\s*;)" };
    ASSERT_TRUE(std::regex_search(descent, match, localSize)) << "TerrainRayNodeSelect.comp declares no local_size_x";
    EXPECT_EQ(match[1].str(), "64");

    // And the divisor the args kernel actually spells.
    EXPECT_NE(args.find("+ 63u) / 64u"), std::string::npos)
        << "TerrainPickArgs.comp no longer divides by 64 — the descent's local_size_x must follow";

    // The resolve kernel is ONE WORK GROUP PER CANDIDATE, so the args kernel
    // must NOT divide the candidate count. Catching that here is cheap; the
    // symptom otherwise is 63 of every 64 intersected patches never marched,
    // which shows up as a cursor that lands on only some of the terrain.
    EXPECT_NE(args.find("uvec3(candidates, 1u, 1u)"), std::string::npos)
        << "the resolve dispatch must be one group per candidate, undivided";
}

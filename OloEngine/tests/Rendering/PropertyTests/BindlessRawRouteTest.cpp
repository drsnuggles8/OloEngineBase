// OLO_TEST_LAYER: shaderpipe
#include "OloEnginePCH.h"

#include "RenderPropertyTest.h"
#include "Rendering/ShaderHarness.h"

#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/ShaderSourceScan.h"
#include "Platform/OpenGL/OpenGLComputeShader.h"
#include "Platform/OpenGL/OpenGLDescriptorHeap.h"
#include "Platform/OpenGL/OpenGLShader.h"

#include <gtest/gtest.h>

#include <glad/gl.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// =============================================================================
// BindlessRawRouteTest -- what the raw-GLSL bindless route (OLO_RHI_BINDLESS=1,
// OpenGLShader::CreateProgramFromRawGLSL) needs from the driver's own GLSL
// front end, which no SPIR-V check reaches (#1565, #1567).
//
// The slotted route hands the driver SPIR-V or SPIRV-Cross text, so shaderc and
// SPIRV-Cross stand between the source and the driver. The raw route hands the
// driver the include-resolved source itself. What the slotted route hid:
//
//   * NVIDIA's front end rejects some identifiers glslang accepts. Measured on
//     an RTX 4090 (driver 617.14) with a one-function probe, `#version 460 core`:
//     `packed`, `row_major`, `register` and `char` as a parameter or local name
//     fail to compile (`C1012`, then `C1503` for every use); `std140`, `std430`,
//     `column_major`, `binding`, `offset` and every other layout-qualifier name
//     compile. Every word that BOTH compilers reject is already caught by the
//     slotted route. `oloGroomUnpackTint(float packed)` took GroomStrand and
//     VSM_GroomDepth off the route (#1567). The guard covers every shader file,
//     because which programs reach the driver as text is a per-file opt-in and
//     an include travels: ReservoirCore's `packed` parameters reached GL only
//     as SPIR-V, until something includes them from a raw-route program.
//
// GlslDriverPortability.* is CPU-only and runs in CI.
// =============================================================================

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;
        namespace SH = ShaderHarness;

        // Identifiers the NVIDIA GLSL front end reserves and glslang does not;
        // see the header comment for how the list was measured.
        constexpr std::array<std::string_view, 4> kNvidiaReservedIdentifiers{ "packed", "row_major", "register",
                                                                              "char" };

        // Replaces every comment character with a space, keeping newlines, so
        // line numbers survive and prose naming a word is not a use of it.
        [[nodiscard]] std::string BlankComments(std::string text)
        {
            for (sizet i = 0; i + 1 < text.size(); ++i)
            {
                if (text[i] == '/' && text[i + 1] == '/')
                {
                    while (i < text.size() && text[i] != '\n')
                    {
                        text[i++] = ' ';
                    }
                }
                else if (text[i] == '/' && text[i + 1] == '*')
                {
                    while (i < text.size() && !(text[i] == '*' && i + 1 < text.size() && text[i + 1] == '/'))
                    {
                        if (text[i] != '\n')
                        {
                            text[i] = ' ';
                        }
                        ++i;
                    }
                    if (i + 1 < text.size())
                    {
                        text[i] = ' ';
                        text[i + 1] = ' ';
                    }
                }
            }
            return text;
        }

        // Blanks the parenthesised argument of every `layout(...)`: inside it,
        // `packed` and `row_major` are qualifiers, not identifiers.
        [[nodiscard]] std::string BlankLayoutQualifiers(std::string text)
        {
            static constexpr std::string_view kLayout = "layout";
            for (sizet pos = text.find(kLayout); pos != std::string::npos; pos = text.find(kLayout, pos + 1))
            {
                if (pos > 0 && ShaderSourceScan::IsIdentifierChar(text[pos - 1]))
                {
                    continue;
                }
                sizet open = pos + kLayout.size();
                while (open < text.size() && ShaderSourceScan::IsAsciiSpace(text[open]))
                {
                    ++open;
                }
                if (open >= text.size() || text[open] != '(')
                {
                    continue;
                }
                for (sizet i = open + 1; i < text.size() && text[i] != ')'; ++i)
                {
                    if (text[i] != '\n')
                    {
                        text[i] = ' ';
                    }
                }
            }
            return text;
        }

        struct ReservedUse
        {
            u32 Line = 0;
            std::string_view Word;
        };

        // Every whole-identifier use of a word in kNvidiaReservedIdentifiers,
        // outside comments and layout qualifiers.
        [[nodiscard]] std::vector<ReservedUse> FindReservedIdentifierUses(const std::string& source)
        {
            const std::string code = BlankLayoutQualifiers(BlankComments(source));
            std::vector<ReservedUse> uses;
            for (const std::string_view word : kNvidiaReservedIdentifiers)
            {
                for (sizet pos = code.find(word); pos != std::string::npos; pos = code.find(word, pos + 1))
                {
                    const sizet after = pos + word.size();
                    const bool leftOk = pos == 0 || !ShaderSourceScan::IsIdentifierChar(code[pos - 1]);
                    const bool rightOk = after >= code.size() || !ShaderSourceScan::IsIdentifierChar(code[after]);
                    if (leftOk && rightOk)
                    {
                        const auto line = static_cast<u32>(std::count(code.begin(), code.begin() + static_cast<std::ptrdiff_t>(pos), '\n')) + 1u;
                        uses.push_back({ line, word });
                    }
                }
            }
            return uses;
        }

    } // namespace

    // The scanner itself, on the defect's shapes: a parameter, a local and a
    // use are each counted, the layout-qualifier spelling and prose are not.
    TEST(GlslDriverPortability, TheReservedIdentifierScanCountsUsesAndSkipsQualifiersAndProse)
    {
        const std::string source = "layout(std140, row_major) uniform A { mat4 m; };\n"
                                   "layout(packed) uniform B { vec4 v; };\n"
                                   "// a packed lane, see row_major above\n"
                                   "vec3 f(float packed) { return vec3(packed); }\n"
                                   "void g() { int register = 0; char c; }\n"
                                   "float packedTint; float unpacked; float row_majorish;\n";
        const std::vector<ReservedUse> uses = FindReservedIdentifierUses(source);
        ASSERT_EQ(uses.size(), 4u) << "expected packed x2 (line 4), register and char (line 5)";
        EXPECT_EQ(std::ranges::count_if(uses, [](const ReservedUse& u) { return u.Word == "packed" && u.Line == 4u; }), 2);
        EXPECT_EQ(std::ranges::count_if(uses, [](const ReservedUse& u) { return u.Line == 5u; }), 2);
    }

    // #1567. Every shader file, not only the ones on the raw route today: the
    // route is a per-file opt-in, and an include reaches many programs.
    TEST(GlslDriverPortability, NoShaderUsesAnIdentifierTheNvidiaFrontEndReserves)
    {
        const fs::path root = fs::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders";
        ASSERT_TRUE(fs::exists(root)) << root.string();

        u32 scanned = 0;
        std::string report;
        for (const fs::path& file : SH::EnumerateShaderSources(root))
        {
            ++scanned;
            for (const ReservedUse& use : FindReservedIdentifierUses(SH::ReadWholeFile(file)))
            {
                report += "\n    " + fs::relative(file, root).generic_string() + ":" + std::to_string(use.Line) +
                          "  '" + std::string(use.Word) + "'";
            }
        }
        EXPECT_GT(scanned, 300u) << "the scan found too few shader files; the walk is broken, not the tree";
        EXPECT_TRUE(report.empty())
            << "These identifiers compile through glslang (the slotted route) but NVIDIA's GLSL front end "
               "rejects them on the raw bindless route (C1012 / C1503), so the program silently leaves the "
               "route. Rename them:"
            << report;
    }
} // namespace OloEngine::Tests

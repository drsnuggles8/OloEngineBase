// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// ShaderToolchainFloorTest.cpp — the named failure that replaces two cryptic ones.
//
// Issue #1139, ADR 0011 amendment (97). Before this file, a build whose shader
// toolchain could not compile `GL_EXT_descriptor_heap` failed as
// `ShaderStageContract.FullGBufferWritersMatchTheProductionAttachmentNumericTypes`
// and `ShaderCompilation.AllProductionShadersCompileUnderVulkanTarget` — two
// tests about G-Buffer attachment types and about shader compilation in general,
// reporting `'descriptor_heap' : unrecognized layout identifier` against a line
// in an include file. Every word of that reads like a shader bug. It was a
// toolchain that was three years old.
//
// So the FIRST assertion here is the whole contract, stated once, with the
// toolchain's own diagnostic attached. The rest pins the pieces that decide
// which shaders the refusal applies to, because a scanner that answered wrongly
// would either refuse every Vulkan compile on an old toolchain (a lie) or none
// of them (the silence this amendment forbids).
// =============================================================================

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/ShaderToolchainFloor.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        // Same probe order as ShaderCompilationTest's own root resolution: ctest
        // runs from the repo root, an editor-launched suite from OloEditor/.
        fs::path ResolveShaderRoot()
        {
            const fs::path candidates[] = {
                fs::path("OloEditor/assets/shaders"),
                fs::current_path() / "OloEditor/assets/shaders",
                fs::current_path().parent_path() / "OloEditor/assets/shaders",
                fs::current_path() / "assets" / "shaders",
            };
            for (const auto& c : candidates)
            {
                std::error_code ec;
                if (fs::exists(c, ec) && fs::is_directory(c, ec))
                {
                    return fs::canonical(c, ec);
                }
            }
            return {};
        }

        std::string ReadWholeFile(const fs::path& p)
        {
            std::ifstream f(p, std::ios::binary);
            std::ostringstream oss;
            oss << f.rdbuf();
            return oss.str();
        }

        // "Does a LINE of this file begin with `#extension <name>`?" — the crude,
        // scanner-independent question, so the test below cannot pass by agreeing
        // with the code it is checking.
        //
        // LINE START IS THE WHOLE POINT, and finding that out cost a red test:
        // DescriptorHeapTextures.glsl's header comment contains the literal text
        // "`#extension GL_EXT_descriptor_heap`" mid-sentence, so a plain
        // `source.find(...)` calls that include file a declaring shader — which it
        // is not. It declares nothing; it tells its INCLUDERS to declare it, and
        // they do, at line start, which is the only place GLSL accepts a
        // directive anyway.
        bool DeclaresDirectiveAtLineStart(std::string_view source, std::string_view extension)
        {
            const std::string needle = std::string("#extension ").append(extension);
            for (sizet lineStart = 0; lineStart <= source.size();)
            {
                const sizet lineEnd = std::min(source.find('\n', lineStart), source.size());
                std::string_view line = source.substr(lineStart, lineEnd - lineStart);
                while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
                {
                    line.remove_prefix(1);
                }
                if (line.starts_with(needle))
                {
                    return true;
                }
                lineStart = lineEnd + 1;
            }
            return false;
        }

        std::string DescribeReport(const ShaderToolchainReport& report)
        {
            std::ostringstream oss;
            oss << "missing:";
            for (const std::string& entry : report.Missing)
            {
                oss << "\n  - " << entry;
            }
            oss << "\ntoolchain diagnostic:\n"
                << report.Diagnostic;
            return oss.str();
        }
    } // namespace

    // THE CONTRACT. A red here means the box or the CI runner is below the floor;
    // it does not mean a shader is wrong. The message says so, because the whole
    // point of this test is that the previous failure did not.
    TEST(ShaderToolchainFloor, LinkedToolchainCompilesTheDescriptorHeapExtension)
    {
        const ShaderToolchainReport& report = ShaderToolchainFloor::Report();
        EXPECT_TRUE(report.Satisfied)
            << "The shaderc/glslang this binary LINKS cannot compile what production shaders declare.\n"
            << DescribeReport(report) << "\n\n"
            << "This is a TOOLCHAIN problem, not a shader problem: install Vulkan SDK "
            << ShaderToolchainFloor::kMinimumVulkanSdk << " or newer (equivalently shaderc "
            << ShaderToolchainFloor::kMinimumShadercTag << "). Ubuntu 24.04's libshaderc-dev is 2023.8 and is "
                                                           "below the floor; CI's hosted Linux arm gets a conforming one from "
                                                           ".github/actions/setup-shader-toolchain-linux. See ADR 0011 amendment (97).";
    }

    // A satisfied toolchain must refuse NOTHING. The counter is the countable
    // half of "loud and countable": a refusal that happened silently would show
    // up here even if the log line were lost.
    TEST(ShaderToolchainFloor, NothingIsRefusedWhenTheFloorIsMet)
    {
        if (!ShaderToolchainFloor::Report().Satisfied)
        {
            GTEST_SKIP() << "toolchain is below the floor — the test above is the one that reports it";
        }
        ShaderToolchainFloor::ResetRefusalCountForTesting();
        const std::string heapUsingSource(ShaderToolchainFloor::kLayoutProbeSource);
        EXPECT_FALSE(ShaderToolchainFloor::RefuseIfBelowFloor(heapUsingSource, "probe"));
        EXPECT_EQ(ShaderToolchainFloor::RefusalCount(), 0u);
    }

    // THE REFUSAL ITSELF, which on every machine this engine is developed on
    // never fires — the floor is met here, so without a synthetic report its
    // message, its counter and its missing-extension list would all ship
    // unexercised. "Loud and countable" would be a claim rather than a fact.
    TEST(ShaderToolchainFloor, ABelowFloorReportRefusesTheShadersThatNeedItAndCountsEachOne)
    {
        ShaderToolchainReport below;
        below.Satisfied = false;
        below.Missing = { std::string(ShaderToolchainFloor::kRequiredExtensions[0]) };
        below.Diagnostic = "error: '#extension' : extension not supported: GL_EXT_descriptor_heap";

        ShaderToolchainFloor::ResetRefusalCountForTesting();

        const std::string needsIt(ShaderToolchainFloor::kLayoutProbeSource);
        EXPECT_TRUE(ShaderToolchainFloor::RefuseIfBelowFloor(below, needsIt, "NeedsTheHeap.glsl"));
        EXPECT_EQ(ShaderToolchainFloor::RefusalCount(), 1u);

        // Counted per compile, not per process: a run that refuses five shaders
        // must be able to say five.
        EXPECT_TRUE(ShaderToolchainFloor::RefuseIfBelowFloor(below, needsIt, "AlsoNeedsIt.glsl"));
        EXPECT_EQ(ShaderToolchainFloor::RefusalCount(), 2u);

        // A shader that declares none of them compiles fine on a below-floor
        // toolchain, so refusing it would be a lie — and would turn one named
        // toolchain problem into "every Vulkan shader is broken", which is the
        // misattribution this whole contract exists to end.
        EXPECT_FALSE(ShaderToolchainFloor::RefuseIfBelowFloor(
            below, "#version 460 core\nvoid main() {}\n", "PlainShader.glsl"));
        EXPECT_EQ(ShaderToolchainFloor::RefusalCount(), 2u);

        ShaderToolchainFloor::ResetRefusalCountForTesting();
        EXPECT_EQ(ShaderToolchainFloor::RefusalCount(), 0u);
    }

    // The scanner decides which shaders the refusal covers. Both directions
    // matter, and the negative one is the subtle half: DescriptorHeapTextures.glsl
    // names the extension four times in its header comment, so a substring search
    // would opt in every shader that merely includes it.
    TEST(ShaderToolchainFloor, SourceNeedsFloorReadsDeclarationsAndNotProse)
    {
        EXPECT_TRUE(ShaderToolchainFloor::SourceNeedsFloor(
            "#version 460 core\n#extension GL_EXT_descriptor_heap : require\nvoid main() {}\n"));
        EXPECT_FALSE(ShaderToolchainFloor::SourceNeedsFloor(
            "#version 460 core\n// GL_EXT_descriptor_heap is discussed here, not declared\nvoid main() {}\n"));
        EXPECT_FALSE(ShaderToolchainFloor::SourceNeedsFloor(
            "#version 460 core\n/* GL_EXT_nonuniform_qualifier, in a block comment */\nvoid main() {}\n"));
        EXPECT_FALSE(ShaderToolchainFloor::SourceNeedsFloor("#version 460 core\nvoid main() {}\n"));
    }

    // The floor's own probe must declare every extension the floor claims to
    // cover — otherwise a toolchain could pass the layout probe while missing one
    // of them, and the report would say "satisfied" about a contract it never
    // tested.
    TEST(ShaderToolchainFloor, TheLayoutProbeDeclaresEveryRequiredExtension)
    {
        const std::string_view probe = ShaderToolchainFloor::kLayoutProbeSource;
        ASSERT_FALSE(std::empty(ShaderToolchainFloor::kRequiredExtensions));
        for (const std::string_view extension : ShaderToolchainFloor::kRequiredExtensions)
        {
            EXPECT_NE(probe.find(extension), std::string_view::npos)
                << "kLayoutProbeSource does not declare " << extension;
            const std::string single = ShaderToolchainFloor::ExtensionProbeSource(extension);
            EXPECT_NE(single.find(extension), std::string_view::npos);
            EXPECT_NE(single.find(" : require"), std::string::npos)
                << "`: enable` would let an unsupported extension pass as a warning";
        }
    }

    // The link between the constant and the tree: every production shader that
    // actually declares one of these extensions must be one the refusal covers.
    // A shader spelling the directive in a way the scanner misses would compile
    // straight into glslang's own diagnostic — the state #1139 was in.
    TEST(ShaderToolchainFloor, EveryProductionShaderDeclaringTheExtensionIsCovered)
    {
        const fs::path root = ResolveShaderRoot();
        ASSERT_FALSE(root.empty()) << "could not locate OloEditor/assets/shaders";

        std::vector<std::string> declaring;
        for (const auto& entry : fs::recursive_directory_iterator(root))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            const fs::path& path = entry.path();
            if (path.extension() != ".glsl" && path.extension() != ".comp")
            {
                continue;
            }
            const std::string source = ReadWholeFile(path);
            if (!DeclaresDirectiveAtLineStart(source, "GL_EXT_descriptor_heap"))
            {
                continue;
            }
            declaring.push_back(path.filename().string());
            EXPECT_TRUE(ShaderToolchainFloor::SourceNeedsFloor(source))
                << path.generic_string() << " declares GL_EXT_descriptor_heap but the scanner does not see it";
        }

        // Amendments (95) and (96) put it in the ray-query tracer and the material
        // families. If this ever reaches zero the scan has broken, not the engine.
        EXPECT_FALSE(declaring.empty())
            << "no production shader declares GL_EXT_descriptor_heap — the scan under " << root.generic_string()
            << " is broken";
    }
} // namespace OloEngine::Tests

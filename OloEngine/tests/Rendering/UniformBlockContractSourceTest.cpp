// OLO_TEST_LAYER: plumbing
//
// The two hand-maintained halves of the uniform block binding contract (ADR
// 0025 section 5a, issue #1330), checked against the sources they describe.
//
//  1. ShaderBindingLayout::IsMultiLayoutUBOBinding lists the binding points
//     that host more than one block layout. It is derived here from every
//     `layout(std140, binding = N) uniform Name` in OloEditor/assets/shaders
//     and compared: a shader that puts a second layout on a binding makes this
//     fail until the table says so.
//  2. A buffer on such a binding must declare the GLSL block it holds
//     (UniformBuffer::Create's block name), or every read of it is refused at
//     run time. A run-time refusal only shows on a path a test renders, so the
//     creation sites are scanned instead: every UniformBuffer::Create whose
//     binding is a multi-layout one, or cannot be resolved, must pass a block
//     name, and every name passed must be a block some shader declares at that
//     binding.
//
// A text scan because the touch-points are hand-maintained and spread over the
// engine, the same reason RendererShutdownTest is one.

#include "OloEnginePCH.h"

#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // Captured at static init: other tests in this binary change directory.
        const std::filesystem::path s_StartCwd = std::filesystem::current_path();

        [[nodiscard]] std::filesystem::path RepoRoot()
        {
            std::error_code ec;
            for (std::filesystem::path dir = s_StartCwd; !dir.empty(); dir = dir.parent_path())
            {
                if (std::filesystem::exists(dir / "OloEngine" / "src" / "OloEngine" / "Renderer" / "ShaderBindingLayout.h", ec))
                    return dir;
                if (!dir.has_relative_path())
                    break;
            }
            return {};
        }

        [[nodiscard]] std::string ReadFile(const std::filesystem::path& path)
        {
            std::ifstream in(path, std::ios::binary);
            std::stringstream contents;
            contents << in.rdbuf();
            return contents.str();
        }

        // binding point -> the GLSL block names declared there, over every shader.
        using ShaderBlocks = std::map<u32, std::set<std::string>>;

        [[nodiscard]] ShaderBlocks ScanShaderBlocks(const std::filesystem::path& root)
        {
            ShaderBlocks blocks;
            const std::regex declaration(R"(layout\s*\(([^)]*)\)\s*uniform\s+(\w+)\s*\{)");
            const std::regex bindingNumber(R"(binding\s*=\s*(\d+))");
            for (const auto& entry : std::filesystem::recursive_directory_iterator(root / "OloEditor" / "assets" / "shaders"))
            {
                const auto extension = entry.path().extension();
                if (!entry.is_regular_file() || (extension != ".glsl" && extension != ".comp"))
                    continue;
                const std::string source = ReadFile(entry.path());
                for (std::sregex_iterator it(source.begin(), source.end(), declaration), end; it != end; ++it)
                {
                    const std::string qualifiers = (*it)[1].str();
                    std::smatch number;
                    if (std::regex_search(qualifiers, number, bindingNumber))
                        blocks[static_cast<u32>(std::stoul(number[1].str()))].insert((*it)[2].str());
                }
            }
            return blocks;
        }

        // `UBO_NAME = 12;` -> { "UBO_NAME", 12 }, from ShaderBindingLayout.h.
        [[nodiscard]] std::map<std::string, u32> ScanBindingConstants(const std::filesystem::path& root)
        {
            std::map<std::string, u32> constants;
            const std::string header = ReadFile(root / "OloEngine" / "src" / "OloEngine" / "Renderer" / "ShaderBindingLayout.h");
            const std::regex constant(R"(static constexpr u32 (UBO_\w+) = (\d+);)");
            for (std::sregex_iterator it(header.begin(), header.end(), constant), end; it != end; ++it)
                constants[(*it)[1].str()] = static_cast<u32>(std::stoul((*it)[2].str()));
            return constants;
        }

        struct CreateCall
        {
            std::string File;
            u32 Line = 0;
            std::vector<std::string> Arguments;
            // The statement after the call is `x->DeclareBlockLike(...)`: a
            // copy of another buffer, which makes that buffer's declaration.
            bool AdoptsDeclaration = false;
            bool Exempt = false;
        };

        [[nodiscard]] std::string Trim(std::string text)
        {
            const auto first = text.find_first_not_of(" \t\r\n");
            const auto last = text.find_last_not_of(" \t\r\n");
            return first == std::string::npos ? std::string{} : text.substr(first, last - first + 1);
        }

        // Every `UniformBuffer::Create(...)` call with its top-level arguments.
        void ScanCreateCalls(const std::filesystem::path& root, const std::filesystem::path& directory, std::vector<CreateCall>& out)
        {
            static const std::string kCall = "UniformBuffer::Create(";
            for (const auto& entry : std::filesystem::recursive_directory_iterator(root / directory))
            {
                const auto extension = entry.path().extension();
                if (!entry.is_regular_file() || (extension != ".cpp" && extension != ".h" && extension != ".inl"))
                    continue;
                // The factory itself, and its declaration.
                if (entry.path().filename() == "UniformBuffer.cpp" || entry.path().filename() == "UniformBuffer.h")
                    continue;
                const std::string source = ReadFile(entry.path());
                for (sizet at = source.find(kCall); at != std::string::npos; at = source.find(kCall, at + 1))
                {
                    // Skip a mention inside a line comment.
                    const sizet lineStart = source.rfind('\n', at) == std::string::npos ? 0 : source.rfind('\n', at) + 1;
                    if (source.substr(lineStart, at - lineStart).find("//") != std::string::npos)
                        continue;
                    CreateCall call;
                    call.File = std::filesystem::relative(entry.path(), root).generic_string();
                    call.Line = static_cast<u32>(std::count(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(at), '\n')) + 1u;
                    int depth = 1;
                    bool inString = false;
                    std::string argument;
                    sizet i = at + kCall.size();
                    for (; i < source.size() && depth > 0; ++i)
                    {
                        const char c = source[i];
                        if (c == '"' && source[i - 1] != '\\')
                            inString = !inString;
                        if (!inString)
                        {
                            // Parentheses and braces only: `<` and `>` are
                            // also comparison and `->`.
                            if (c == '(' || c == '{')
                                ++depth;
                            else if (c == ')' || c == '}')
                                --depth;
                            if (depth == 0 || (depth == 1 && c == ','))
                            {
                                call.Arguments.push_back(Trim(argument));
                                argument.clear();
                                continue;
                            }
                        }
                        argument += c;
                    }
                    // Up to the end of the next statement.
                    const sizet thisStatement = source.find(';', i);
                    const sizet nextStatement = thisStatement == std::string::npos ? std::string::npos : source.find(';', thisStatement + 1);
                    if (nextStatement != std::string::npos)
                        call.AdoptsDeclaration = source.substr(thisStatement, nextStatement - thisStatement).find("DeclareBlockLike(") != std::string::npos;
                    // `// OLO_BLOCK_UNDECLARED: <reason>` on the call's line: a
                    // buffer no shader block corresponds to. The reason is for
                    // the reader; the marker keeps the exemption greppable.
                    const sizet lineEnd = source.find('\n', at);
                    call.Exempt = source.substr(at, lineEnd == std::string::npos ? std::string::npos : lineEnd - at).find("OLO_BLOCK_UNDECLARED:") != std::string::npos;
                    out.push_back(std::move(call));
                }
            }
        }

        // The binding argument as a number, when it is a literal or a
        // ShaderBindingLayout constant.
        [[nodiscard]] std::optional<u32> ResolveBinding(const std::string& argument, const std::map<std::string, u32>& constants)
        {
            if (!argument.empty() && std::all_of(argument.begin(), argument.end(), [](unsigned char c)
                                                 { return std::isdigit(c) != 0 || c == 'u' || c == 'U'; }))
                return static_cast<u32>(std::stoul(argument));
            const sizet name = argument.rfind("UBO_");
            if (name == std::string::npos)
                return std::nullopt;
            const std::string constant = argument.substr(name);
            // Exactly `[qualifier::]UBO_NAME`, not an expression that merely contains one.
            if (!std::all_of(constant.begin(), constant.end(), [](unsigned char c)
                             { return std::isalnum(c) != 0 || c == '_'; }))
                return std::nullopt;
            if (const auto found = constants.find(constant); found != constants.end())
                return found->second;
            return std::nullopt;
        }

        [[nodiscard]] std::optional<std::string> StringLiteral(const std::string& argument)
        {
            if (argument.size() >= 2 && argument.front() == '"' && argument.back() == '"')
                return argument.substr(1, argument.size() - 2);
            return std::nullopt;
        }
    } // namespace

    TEST(UniformBlockContractSource, TheMultiLayoutTableMatchesTheShaderSources)
    {
        const std::filesystem::path root = RepoRoot();
        ASSERT_FALSE(root.empty()) << "repository root not found from " << s_StartCwd.string();
        const ShaderBlocks blocks = ScanShaderBlocks(root);
        ASSERT_GT(blocks.size(), 40u) << "the shader scan found too few uniform blocks to be trusted";

        for (u32 binding = 0; binding < ShaderBindingLayout::MIN_GUARANTEED_BUFFER_BINDINGS; ++binding)
        {
            const auto found = blocks.find(binding);
            const bool severalLayouts = found != blocks.end() && found->second.size() > 1;
            std::string names;
            if (found != blocks.end())
            {
                for (const std::string& name : found->second)
                    names += " " + name;
            }
            if (found != blocks.end())
            {
                // The OpenGL backend recovers a block name from the driver by
                // stripping the `_<digits>` suffix SPIRV-Cross adds to a later
                // stage's copy of a block (OpenGLBlockContract.cpp). That is
                // exact only while no block is really named that way.
                for (const std::string& name : found->second)
                {
                    const sizet underscore = name.find_last_of('_');
                    const bool numericSuffix = underscore != std::string::npos && underscore + 1 < name.size() &&
                                               std::all_of(name.begin() + static_cast<std::ptrdiff_t>(underscore) + 1, name.end(),
                                                           [](unsigned char c)
                                                           { return std::isdigit(c) != 0; });
                    EXPECT_FALSE(numericSuffix) << "uniform block `" << name << "` at binding " << binding
                                                << " ends in _<digits>, which the OpenGL binding contract reads as a "
                                                   "SPIRV-Cross stage suffix. Rename the block.";
                }
            }
            EXPECT_EQ(ShaderBindingLayout::IsMultiLayoutUBOBinding(binding), severalLayouts)
                << "uniform binding " << binding << " is declared with these block names in the shaders:" << names
                << "\nShaderBindingLayout::IsMultiLayoutUBOBinding must list exactly the bindings with more than one. "
                   "Adding a binding there means every buffer created on it must declare its block "
                   "(UniformBuffer::Create(size, binding, \"BlockName\")).";
        }
    }

    TEST(UniformBlockContractSource, EveryBufferOnAMultiLayoutBindingDeclaresABlockTheShadersHave)
    {
        const std::filesystem::path root = RepoRoot();
        ASSERT_FALSE(root.empty());
        const ShaderBlocks blocks = ScanShaderBlocks(root);
        const std::map<std::string, u32> constants = ScanBindingConstants(root);
        ASSERT_GT(constants.size(), 60u) << "ShaderBindingLayout.h's UBO constants were not parsed";

        std::vector<CreateCall> calls;
        ScanCreateCalls(root, "OloEngine/src", calls);
        ScanCreateCalls(root, "OloEditor/src", calls);
        ASSERT_GT(calls.size(), 100u) << "the creation-site scan found too few UniformBuffer::Create calls to be trusted";

        u32 declared = 0;
        u32 exempt = 0;
        for (const CreateCall& call : calls)
        {
            const std::string where = call.File + ":" + std::to_string(call.Line);
            ASSERT_GE(call.Arguments.size(), 2u) << where << ": could not parse the call's arguments";
            const std::optional<u32> binding = ResolveBinding(call.Arguments[1], constants);
            const bool declares = call.Arguments.size() >= 3;

            if (!declares && call.AdoptsDeclaration)
            {
                ++declared;
                continue;
            }
            if (!declares && call.Exempt)
            {
                ++exempt;
                continue;
            }
            if (!declares)
            {
                EXPECT_TRUE(binding.has_value())
                    << where << ": the binding `" << call.Arguments[1]
                    << "` is not a literal or a ShaderBindingLayout::UBO_ constant, so this scan cannot tell whether it is a "
                       "multi-layout binding. Pass the GLSL block name as the third argument.";
                if (binding)
                {
                    EXPECT_FALSE(ShaderBindingLayout::IsMultiLayoutUBOBinding(*binding))
                        << where << ": binding " << *binding
                        << " hosts more than one block layout, and this buffer does not say which it holds. Every read of it "
                           "is refused at run time (ADR 0025 section 5a). Pass the GLSL block name as the third argument.";
                }
                continue;
            }

            ++declared;
            for (sizet index = 2; index < call.Arguments.size(); ++index)
            {
                const std::optional<std::string> name = StringLiteral(call.Arguments[index]);
                ASSERT_TRUE(name.has_value())
                    << where << ": the block name must be a string literal (the buffer keeps a view of it): " << call.Arguments[index];
                if (!binding)
                    continue;
                const auto found = blocks.find(*binding);
                const bool known = found != blocks.end() && found->second.contains(*name);
                EXPECT_TRUE(known) << where << ": no shader declares a uniform block named `" << *name << "` at binding " << *binding
                                   << ". The name is compared with the reflected block name, so a misspelt one refuses every read.";
            }
        }
        EXPECT_GT(declared, 20u) << "fewer declared buffers than the multi-layout bindings have producers";
        // An exemption is a buffer the contract cannot vouch for. One exists
        // (InflightFrameManager's generic named buffer); a second needs a reason
        // good enough to raise this number in the same change.
        EXPECT_LE(exempt, 1u) << "a new OLO_BLOCK_UNDECLARED exemption was added";
    }
} // namespace OloEngine::Tests

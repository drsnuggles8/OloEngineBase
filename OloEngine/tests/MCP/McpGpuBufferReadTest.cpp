// OLO_TEST_LAYER: unit
//
// Selection, range checks and decoding behind `olo_gpu_buffer_read` (issue
// #607, gap logged from #1105). The read needs a live buffer; the decisions an
// agent acts on are in MCP/McpGpuBufferRead.h:
//
//   * every SSBO_* binding in ShaderBindingLayout.h has a name entry (checked
//     against the header TEXT, so a new binding cannot be unreachable by name);
//   * a binding shared by several live buffers is an error listing them, not a
//     guess;
//   * no live buffer, an unknown name and a range past the end are errors;
//   * decoding is little-endian per element, and NaN cannot break the JSON.

#include "OloEnginePCH.h"

#include "MCP/McpGpuBufferRead.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace Read = OloEngine::MCP::GpuBufferRead;
using OloEngine::ShaderBindingLayout;
using OloEngine::StorageBufferUsage;
using Json = nlohmann::json;

namespace
{
    namespace fs = std::filesystem;

    Read::Request Parse(const Json& args)
    {
        Read::Request request;
        const auto error = Read::ParseRequest(args, request);
        EXPECT_FALSE(error.has_value()) << *error;
        return request;
    }

    std::string ParseError(const Json& args)
    {
        Read::Request request;
        const auto error = Read::ParseRequest(args, request);
        EXPECT_TRUE(error.has_value()) << "expected a refusal for " << args.dump();
        return error.value_or("");
    }

    // Runs from the repo root under ctest and from OloEditor/ when launched by
    // the editor; walk up until the header resolves.
    fs::path BindingLayoutHeader()
    {
        const fs::path relative = "OloEngine/src/OloEngine/Renderer/ShaderBindingLayout.h";
        for (fs::path dir = fs::current_path(); !dir.empty(); dir = dir.parent_path())
        {
            if (fs::exists(dir / relative))
                return dir / relative;
            if (dir == dir.parent_path())
                break;
        }
        return {};
    }
} // namespace

// The table takes its VALUES from ShaderBindingLayout, so only coverage can
// drift. Read the header as text and require a name for every SSBO_* binding.
TEST(McpGpuBufferRead, EveryStorageBindingHasAName)
{
    const fs::path header = BindingLayoutHeader();
    ASSERT_FALSE(header.empty()) << "ShaderBindingLayout.h not found from " << fs::current_path();
    std::ifstream in(header);
    std::stringstream text;
    text << in.rdbuf();
    const std::string source = text.str();

    // Constants that are not a binding of their own.
    const std::set<std::string> notBindings{ "SSBO_HIGHEST_BINDING", "SSBO_BINDING_LIMIT", "SSBO_DEBUG_DRAW_FIRST",
                                             "SSBO_DEBUG_DRAW_COUNT" };
    std::set<std::string> declared;
    const std::regex constant(R"(static\s+constexpr\s+u32\s+(SSBO_[A-Z0-9_]+)\s*=)");
    for (auto it = std::sregex_iterator(source.begin(), source.end(), constant); it != std::sregex_iterator(); ++it)
    {
        if (!notBindings.contains((*it)[1].str()))
            declared.insert((*it)[1].str());
    }
    ASSERT_GT(declared.size(), 70u) << "the header scan found too few constants; the regex no longer matches";

    std::set<std::string> named;
    for (const auto& entry : Read::kStorageBindings)
        named.insert(std::string(entry.Name));
    for (const auto& name : declared)
        EXPECT_TRUE(named.contains(name)) << name << " is declared in ShaderBindingLayout.h but has no entry in kStorageBindings";
    for (const auto& name : named)
        EXPECT_TRUE(declared.contains(name)) << name << " is in kStorageBindings but no longer in ShaderBindingLayout.h";
}

TEST(McpGpuBufferRead, NamesResolveWithOrWithoutThePrefix)
{
    EXPECT_EQ(Read::BindingForName("SSBO_FPLUS_LIGHT_GRID"), ShaderBindingLayout::SSBO_FPLUS_LIGHT_GRID);
    EXPECT_EQ(Read::BindingForName("FPLUS_LIGHT_GRID"), ShaderBindingLayout::SSBO_FPLUS_LIGHT_GRID);
    EXPECT_FALSE(Read::BindingForName("SSBO_NOT_A_BUFFER").has_value());

    // A shared number lists every name for it.
    const auto names = Read::NamesForBinding(ShaderBindingLayout::SSBO_GROOM_DEFORMATION);
    EXPECT_NE(std::ranges::find(names, "SSBO_GROOM_DEFORMATION"), names.end());
    EXPECT_NE(std::ranges::find(names, "SSBO_TERRAIN_VT"), names.end());
}

TEST(McpGpuBufferRead, RejectsMalformedRequests)
{
    EXPECT_NE(ParseError(Json::object()).find("'binding'"), std::string::npos);
    EXPECT_NE(ParseError(Json{ { "binding", 3 }, { "name", "SSBO_COUNTERS" } }).find("not both"), std::string::npos);
    EXPECT_NE(ParseError(Json{ { "name", "SSBO_NOPE" } }).find("Known names"), std::string::npos);
    EXPECT_NE(ParseError(Json{ { "binding", 3 }, { "lengthBytes", 4097 } }).find("lengthBytes"), std::string::npos);
    EXPECT_NE(ParseError(Json{ { "binding", 3 }, { "format", "u64" } }).find("format"), std::string::npos);
    EXPECT_NE(ParseError(Json{ { "binding", 3 }, { "offsetBytes", 2 } }).find("multiple of 4"), std::string::npos);
    (void)Parse(Json{ { "binding", 3 }, { "offsetBytes", 2 }, { "format", "hex" } });
}

// THE ONE THAT MATTERS: one SSBO_GPU_PARTICLES per particle system, so a
// binding can match several buffers. Picking one would be a silent guess.
TEST(McpGpuBufferRead, SharedBindingIsAnErrorThatListsCandidates)
{
    const std::vector<Read::Candidate> live{
        { 1, ShaderBindingLayout::SSBO_GPU_PARTICLES, 4096, StorageBufferUsage::DynamicCopy },
        { 2, ShaderBindingLayout::SSBO_GPU_PARTICLES, 8192, StorageBufferUsage::DynamicCopy },
        { 3, ShaderBindingLayout::SSBO_COUNTERS, 16, StorageBufferUsage::DynamicCopy },
    };

    const auto ambiguous = Read::SelectBuffer(Parse(Json{ { "name", "SSBO_GPU_PARTICLES" } }), live);
    EXPECT_FALSE(ambiguous.Chosen.has_value());
    EXPECT_NE(ambiguous.Error.find("2 live storage buffers"), std::string::npos) << ambiguous.Error;
    ASSERT_EQ(ambiguous.Candidates.size(), 2u);
    EXPECT_EQ(ambiguous.Candidates[1].at("sizeBytes").get<u32>(), 8192u);

    const auto picked = Read::SelectBuffer(Parse(Json{ { "name", "SSBO_GPU_PARTICLES" }, { "id", 2 } }), live);
    ASSERT_TRUE(picked.Chosen.has_value());
    EXPECT_EQ(picked.Chosen->Id, 2u);

    const auto byId = Read::SelectBuffer(Parse(Json{ { "id", 3 } }), live);
    ASSERT_TRUE(byId.Chosen.has_value());
    EXPECT_EQ(byId.Chosen->Binding, ShaderBindingLayout::SSBO_COUNTERS);

    const auto none = Read::SelectBuffer(Parse(Json{ { "name", "SSBO_FLUID_POSITIONS" } }), live);
    EXPECT_FALSE(none.Chosen.has_value());
    EXPECT_NE(none.Error.find("No live storage buffer"), std::string::npos);
    EXPECT_NE(none.Error.find("SSBO_FLUID_POSITIONS"), std::string::npos);
}

// TransientPool creates every pooled buffer with binding 15 (SSBO_INSTANCE_DATA)
// whatever it holds. Matched by binding, a pool buffer would be decoded as
// instance data, or bury the real one in an "ambiguous" list.
TEST(McpGpuBufferRead, PooledBuffersAreSkippedByBindingButReachableById)
{
    const std::vector<Read::Candidate> live{
        { 7, ShaderBindingLayout::SSBO_INSTANCE_DATA, 9216, StorageBufferUsage::DynamicDraw, "", false },
        { 8, ShaderBindingLayout::SSBO_INSTANCE_DATA, 4096, StorageBufferUsage::DynamicDraw, "TransientPool", true },
    };

    const auto byName = Read::SelectBuffer(Parse(Json{ { "name", "SSBO_INSTANCE_DATA" } }), live);
    ASSERT_TRUE(byName.Chosen.has_value()) << byName.Error;
    EXPECT_EQ(byName.Chosen->Id, 7u);

    const auto byId = Read::SelectBuffer(Parse(Json{ { "id", 8 } }), live);
    ASSERT_TRUE(byId.Chosen.has_value());
    const Json described = Read::CandidateJson(*byId.Chosen);
    EXPECT_TRUE(described.at("pooled").get<bool>());
    EXPECT_EQ(described.at("owner").get<std::string>(), "TransientPool");
    EXPECT_TRUE(described.at("names").empty()) << "a nominal binding must not be named";

    const std::vector<Read::Candidate> poolOnly{ live[1] };
    const auto none = Read::SelectBuffer(Parse(Json{ { "name", "SSBO_INSTANCE_DATA" } }), poolOnly);
    EXPECT_FALSE(none.Chosen.has_value());
    EXPECT_NE(none.Error.find("pooled buffer(s)"), std::string::npos) << none.Error;
}

TEST(McpGpuBufferRead, RangePastTheEndIsAnErrorNotATruncation)
{
    const auto past = Read::ResolveRange(Parse(Json{ { "binding", 0 }, { "offsetBytes", 8 }, { "lengthBytes", 16 } }), 16);
    EXPECT_FALSE(past.Ok);
    EXPECT_NE(past.Error.find("exceeds the 16-byte buffer"), std::string::npos);

    const auto start = Read::ResolveRange(Parse(Json{ { "binding", 0 }, { "offsetBytes", 16 } }), 16);
    EXPECT_FALSE(start.Ok);

    const auto partial = Read::ResolveRange(Parse(Json{ { "binding", 0 }, { "lengthBytes", 20 }, { "format", "vec4" } }), 64);
    EXPECT_FALSE(partial.Ok);
    EXPECT_NE(partial.Error.find("whole number"), std::string::npos);

    // Defaulted length: as much as fits, whole elements only, capped.
    const auto defaulted = Read::ResolveRange(Parse(Json{ { "binding", 0 }, { "offsetBytes", 4 }, { "format", "vec4" } }), 40);
    ASSERT_TRUE(defaulted.Ok);
    EXPECT_EQ(defaulted.LengthBytes, 32u);
    const auto capped = Read::ResolveRange(Parse(Json{ { "binding", 0 } }), 1u << 20);
    EXPECT_EQ(capped.LengthBytes, Read::kDefaultReadBytes);
}

TEST(McpGpuBufferRead, DecodesEachFormat)
{
    std::vector<u8> bytes(16);
    const u32 u = 0xDEADBEEFu;
    const i32 i = -7;
    const f32 f = 2.5f;
    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    std::memcpy(bytes.data(), &u, 4);
    std::memcpy(bytes.data() + 4, &i, 4);
    std::memcpy(bytes.data() + 8, &f, 4);
    std::memcpy(bytes.data() + 12, &nan, 4);

    EXPECT_EQ(Read::Decode(Read::Format::U32, bytes)[0].get<u32>(), 0xDEADBEEFu);
    EXPECT_EQ(Read::Decode(Read::Format::I32, bytes)[1].get<i32>(), -7);
    const auto floats = Read::Decode(Read::Format::F32, bytes);
    EXPECT_FLOAT_EQ(floats[2].get<f32>(), 2.5f);
    EXPECT_EQ(floats[3].get<std::string>(), "nan") << "JSON has no NaN; it must not serialize as null";
    const auto vec4 = Read::Decode(Read::Format::Vec4, bytes);
    ASSERT_EQ(vec4.size(), 1u);
    EXPECT_EQ(vec4[0].size(), 4u);
    const auto hex = Read::Decode(Read::Format::Hex, bytes);
    ASSERT_EQ(hex.size(), 1u);
    EXPECT_EQ(hex[0].get<std::string>().substr(0, 11), "ef be ad de");
}

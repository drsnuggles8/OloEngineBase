// OLO_TEST_LAYER: unit
// =============================================================================
// McpDocsCoverageTest — the MCP surface vs. its guide.
//
// `docs/guides/mcp-diagnostics-server.md` is the only description of the MCP
// tool surface an agent reads before deciding what is possible. When it drifts
// from the code the failure is not a broken build, it is a reader who believes
// something false and stops — which is strictly worse than a missing doc,
// because a missing doc gets discovered and a wrong one does not.
//
// Two real instances motivated this file (issue #725):
//
//   * `olo_shader_debug_draw` shipped without a guide entry, so the tool existed
//     but nothing told anyone it did.
//   * The write-consent section carried a hand-maintained list of the gated
//     tools that had drifted to 9 of 14. Everything missing from it — the
//     sun/time-of-day/weather setters, `olo_postprocess_settings_set`,
//     `olo_render_debug_set` — read as ungated.
//
// (A third, in `docs/agent-rules/mcp-setter-based-field-registry.md`, claimed
// the write-consent gate had no headless bypass thirteen days after
// `OLO_MCP_ALLOW_WRITES` was added. That one is prose, not a list, so it is out
// of reach of a test — but it is why this file exists.)
//
// So this test parses BOTH sides as text and checks:
//   1. every registered `olo_*` tool is mentioned in the guide;
//   2. every `ProjectWrite` tool is marked **(consented write)** there.
//
// Text parsing is crude and deliberately so — the alternative is linking the
// whole editor tool registry into a headless test, which is what
// McpConsentedWriteTest specifically avoids (it registers a fake tool instead).
// Scanning the registration sources keeps this free of that dependency.
//
// Classification: unit (no GL, no editor, no live server).
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        // The suite runs from the repo root under ctest and from OloEditor/ when
        // launched by the editor; walk up until both anchors resolve.
        [[nodiscard]] fs::path RepoRoot()
        {
            std::error_code ec;
            fs::path dir = fs::current_path(ec);
            for (int hop = 0; hop < 4 && !dir.empty(); ++hop)
            {
                if (fs::exists(dir / "docs" / "guides" / "mcp-diagnostics-server.md", ec) &&
                    fs::exists(dir / "OloEditor" / "src" / "MCP", ec))
                {
                    return dir;
                }
                dir = dir.parent_path();
            }
            return {};
        }

        [[nodiscard]] std::string ReadFile(const fs::path& path)
        {
            std::ifstream file(path, std::ios::binary);
            std::ostringstream contents;
            contents << file.rdbuf();
            return contents.str();
        }

        struct RegisteredTool
        {
            std::string Name;
            bool ProjectWrite = false;
        };

        // Walk the registration sources, pairing each `tool.Name = "olo_..."`
        // with whether its block later sets `ProjectWrite = true`. The
        // registrations are one `{ ToolDef tool; ... RegisterTool(...); }` block
        // each, so "the most recent name wins until the next name" is exact.
        [[nodiscard]] std::vector<RegisteredTool> ScanRegisteredTools(const fs::path& mcpDir)
        {
            static const std::regex nameRe(R"(tool\.Name\s*=\s*\"(olo_[a-z0-9_]+)\")");
            std::vector<RegisteredTool> tools;

            std::error_code ec;
            std::vector<fs::path> sources;
            for (const auto& entry : fs::recursive_directory_iterator(mcpDir, ec))
            {
                if (entry.is_regular_file(ec) && entry.path().extension() == ".cpp")
                    sources.push_back(entry.path());
            }
            std::ranges::sort(sources);

            for (const auto& source : sources)
            {
                std::istringstream stream(ReadFile(source));
                std::string line;
                std::string current;
                while (std::getline(stream, line))
                {
                    if (std::smatch match; std::regex_search(line, match, nameRe))
                    {
                        current = match[1].str();
                        tools.push_back(RegisteredTool{ current, false });
                    }
                    else if (line.find("ProjectWrite = true") != std::string::npos && !current.empty())
                    {
                        for (auto& tool : tools)
                        {
                            if (tool.Name == current)
                                tool.ProjectWrite = true;
                        }
                    }
                }
            }
            return tools;
        }

        // Lines of the guide that carry the "(consented write)" marking, so a
        // tool named anywhere on such a line counts as marked. Coarse on purpose:
        // the guide is a Markdown table and a tool's row is one line.
        [[nodiscard]] std::set<std::string> MarkedConsentedWrite(const std::string& guide)
        {
            static const std::regex toolRe(R"((olo_[a-z0-9_]+))");
            std::set<std::string> marked;
            std::istringstream stream(guide);
            std::string line;
            while (std::getline(stream, line))
            {
                if (line.find("(consented write)") == std::string::npos)
                    continue;

                // Credit the tools in the row's SUBJECT cell — the text between
                // the first and second table pipes — not every tool named on the
                // line. Both halves of that matter:
                //   * a row can have several subjects (`olo_scene_play` /
                //     `olo_scene_stop` share one), so taking only the first
                //     under-credits and reports a false drift;
                //   * a row's prose often names OTHER tools ("the same knob as
                //     olo_virtual_geometry_set"), so taking all of them
                //     over-credits and would let a genuinely unmarked tool pass.
                const auto firstPipe = line.find('|');
                if (firstPipe == std::string::npos)
                    continue;
                const auto secondPipe = line.find('|', firstPipe + 1);
                if (secondPipe == std::string::npos)
                    continue;
                const std::string subject = line.substr(firstPipe + 1, secondPipe - firstPipe - 1);

                for (auto it = std::sregex_iterator(subject.begin(), subject.end(), toolRe);
                     it != std::sregex_iterator(); ++it)
                {
                    marked.insert((*it)[1].str());
                }
            }
            return marked;
        }
    } // namespace

    TEST(McpDocsCoverage, EveryRegisteredToolAppearsInTheGuide)
    {
        const fs::path root = RepoRoot();
        ASSERT_FALSE(root.empty()) << "Could not locate the repo root from " << fs::current_path().string();

        const std::string guide = ReadFile(root / "docs" / "guides" / "mcp-diagnostics-server.md");
        ASSERT_FALSE(guide.empty()) << "mcp-diagnostics-server.md is empty or unreadable";

        const auto tools = ScanRegisteredTools(root / "OloEditor" / "src" / "MCP");
        ASSERT_GT(tools.size(), 50u) << "Scanned implausibly few tools — the registration pattern probably changed, "
                                        "which would make this whole test pass vacuously.";

        std::vector<std::string> undocumented;
        for (const auto& tool : tools)
        {
            if (guide.find(tool.Name) == std::string::npos)
                undocumented.push_back(tool.Name);
        }

        EXPECT_TRUE(undocumented.empty())
            << "These MCP tools are registered but never mentioned in "
               "docs/guides/mcp-diagnostics-server.md, so nothing tells a reader they exist: "
            << [&undocumented]
        {
            std::string joined;
            for (const auto& name : undocumented)
                joined += (joined.empty() ? "" : ", ") + name;
            return joined;
        }();
    }

    TEST(McpDocsCoverage, EveryConsentGatedToolIsMarkedConsentedWriteInTheGuide)
    {
        const fs::path root = RepoRoot();
        ASSERT_FALSE(root.empty());

        const std::string guide = ReadFile(root / "docs" / "guides" / "mcp-diagnostics-server.md");
        ASSERT_FALSE(guide.empty());

        const auto tools = ScanRegisteredTools(root / "OloEditor" / "src" / "MCP");
        const auto marked = MarkedConsentedWrite(guide);

        std::vector<std::string> writeTools;
        for (const auto& tool : tools)
        {
            if (tool.ProjectWrite)
                writeTools.push_back(tool.Name);
        }
        ASSERT_FALSE(writeTools.empty()) << "Found no ProjectWrite tools — the scan is broken, not the docs.";

        std::vector<std::string> unmarked;
        for (const auto& name : writeTools)
        {
            if (!marked.contains(name))
                unmarked.push_back(name);
        }

        EXPECT_TRUE(unmarked.empty())
            << "These tools set ToolDef::ProjectWrite (so they are refused until the human enables Agent writes, "
               "or the session was launched with OLO_MCP_ALLOW_WRITES=1) but are NOT marked '(consented write)' in "
               "the guide. A reader would expect them to just work: "
            << [&unmarked]
        {
            std::string joined;
            for (const auto& name : unmarked)
                joined += (joined.empty() ? "" : ", ") + name;
            return joined;
        }();
    }
} // namespace OloEngine::Tests

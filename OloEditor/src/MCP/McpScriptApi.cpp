#include "MCP/McpScriptApi.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::MCP
{
    namespace
    {
        using Json = nlohmann::json;
        namespace fs = std::filesystem;

        std::string Trim(const std::string& s)
        {
            const auto begin = s.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos)
                return {};
            const auto end = s.find_last_not_of(" \t\r\n");
            return s.substr(begin, end - begin + 1);
        }

        bool ContainsCI(std::string_view hay, std::string_view needle)
        {
            if (needle.empty())
                return true;
            const auto lower = [](std::string in)
            {
                std::transform(in.begin(), in.end(), in.begin(),
                               [](unsigned char c)
                               { return static_cast<char>(std::tolower(c)); });
                return in;
            };
            return lower(std::string(hay)).find(lower(std::string(needle))) != std::string::npos;
        }

        // Resolve a repo-root-relative path against the editor's working directory
        // (cwd = OloEditor/ at runtime, so the repo root is its parent). Tries a few
        // plausible roots; returns empty if none exist.
        fs::path ResolveRepoPath(const std::string& relFromRepoRoot)
        {
            std::error_code ec;
            const fs::path cwd = fs::current_path(ec);
            if (ec)
                return {};
            const std::array<fs::path, 3> candidates = {
                cwd.parent_path() / relFromRepoRoot, // cwd = OloEditor/ → repo root is parent
                cwd / relFromRepoRoot,               // cwd already repo root
                cwd / ".." / ".." / relFromRepoRoot
            };
            for (const auto& candidate : candidates)
            {
                if (fs::exists(candidate, ec))
                    return candidate;
            }
            return {};
        }

        Json BuildCSharpApi(const std::string& typeFilter)
        {
            Json result;
            result["language"] = "csharp";
            result["engineVersion"] = "0.0.1";

            const fs::path root = ResolveRepoPath("OloEngine-ScriptCore/src/OloEngine");
            if (root.empty())
            {
                result["error"] = "C# script bindings not found (expected OloEngine-ScriptCore/src/OloEngine "
                                  "relative to the editor's working directory). Available only in a source checkout.";
                return result;
            }

            // public [partial|static|abstract|sealed] class|struct|enum|interface Name
            static const std::regex typeRe(
                R"(\bpublic\s+(?:(?:partial|static|abstract|sealed)\s+)*(class|struct|enum|interface)\s+([A-Za-z_]\w*))");
            static const std::regex publicMemberRe(R"(^\s*public\s+\S)");

            // Members are attributed to the most recently declared type. C# here is
            // mostly flat top-level (partial) types, so this is accurate enough for a
            // digest without full brace tracking.
            Json types = Json::array();
            std::error_code ec;
            for (auto it = fs::recursive_directory_iterator(root, ec);
                 !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                if (!it->is_regular_file() || it->path().extension() != ".cs")
                    continue;
                std::ifstream file(it->path());
                if (!file)
                    continue;

                const std::string fileName = it->path().filename().string();
                int currentTypeIdx = -1;
                std::string line;
                while (std::getline(file, line))
                {
                    if (std::smatch m; std::regex_search(line, m, typeRe))
                    {
                        // C# types are often `partial`, split across files (e.g.
                        // Components.cs + Components.Generated.cs). Merge declarations
                        // with the same name+kind into one entry instead of fragmenting.
                        const std::string name = m[2].str();
                        const std::string kind = m[1].str();
                        currentTypeIdx = -1;
                        for (std::size_t ti = 0; ti < types.size(); ++ti)
                        {
                            if (types[ti]["name"] == name && types[ti]["kind"] == kind)
                            {
                                currentTypeIdx = static_cast<int>(ti);
                                break;
                            }
                        }
                        if (currentTypeIdx < 0)
                        {
                            Json entry;
                            entry["name"] = name;
                            entry["kind"] = kind;
                            entry["file"] = fileName;
                            entry["members"] = Json::array();
                            types.push_back(std::move(entry));
                            currentTypeIdx = static_cast<int>(types.size()) - 1;
                        }
                    }
                    else if (currentTypeIdx >= 0 && std::regex_search(line, publicMemberRe))
                    {
                        types[static_cast<std::size_t>(currentTypeIdx)]["members"].push_back(Trim(line));
                    }
                }
            }

            result["typeCount"] = static_cast<int>(types.size());
            if (typeFilter.empty())
            {
                // Cheap index: names + kinds only.
                Json index = Json::array();
                for (const auto& t : types)
                    index.push_back(Json{ { "name", t["name"] }, { "kind", t["kind"] }, { "file", t["file"] } });
                result["types"] = std::move(index);
                result["note"] = "Pass a 'typeFilter' substring to get a type's members.";
            }
            else
            {
                Json matched = Json::array();
                for (const auto& t : types)
                {
                    if (ContainsCI(t["name"].get<std::string>(), typeFilter))
                        matched.push_back(t);
                }
                result["matched"] = static_cast<int>(matched.size());
                result["types"] = std::move(matched);
            }
            return result;
        }

        Json BuildLuaApi(const std::string& typeFilter)
        {
            Json result;
            result["language"] = "lua";
            result["engineVersion"] = "0.0.1";

            const fs::path glue = ResolveRepoPath("OloEngine/src/OloEngine/Scripting/Lua/LuaScriptGlue.cpp");
            if (glue.empty())
            {
                result["error"] = "Lua bindings (LuaScriptGlue.cpp) not found relative to the editor's "
                                  "working directory. Available only in a source checkout.";
                return result;
            }

            std::ifstream file(glue, std::ios::binary);
            std::stringstream buffer;
            buffer << file.rdbuf();
            const std::string source = buffer.str();

            // lua.new_usertype<...>("Name", ...)
            // Custom raw-string delimiter (RE) because the pattern itself contains )"
            // which would otherwise close a default R"(...)" literal early.
            static const std::regex usertypeRe(R"RE(new_usertype\s*<[^>]*>\s*\(\s*"([A-Za-z_]\w*)")RE");

            struct Match
            {
                std::string name;
                std::size_t pos;
            };
            std::vector<Match> matches;
            for (auto it = std::sregex_iterator(source.begin(), source.end(), usertypeRe);
                 it != std::sregex_iterator(); ++it)
            {
                matches.push_back({ (*it)[1].str(), static_cast<std::size_t>(it->position()) });
            }

            result["typeCount"] = static_cast<int>(matches.size());
            if (typeFilter.empty())
            {
                Json names = Json::array();
                for (const auto& m : matches)
                    names.push_back(Json{ { "name", m.name } });
                result["types"] = std::move(names);
                result["note"] = "Pass a 'typeFilter' substring to get a type's full Sol2 registration block.";
                return result;
            }

            // For matching usertypes, return the raw registration block (from the
            // match to the next usertype, capped) — robust and informative.
            constexpr std::size_t kMaxBlock = 3000;
            Json matched = Json::array();
            for (std::size_t i = 0; i < matches.size(); ++i)
            {
                if (!ContainsCI(matches[i].name, typeFilter))
                    continue;
                const std::size_t start = matches[i].pos;
                const std::size_t end = (i + 1 < matches.size()) ? matches[i + 1].pos : source.size();
                std::string block = source.substr(start, std::min(end - start, kMaxBlock));
                matched.push_back(Json{ { "name", matches[i].name },
                                        { "registration", Trim(block) },
                                        { "truncated", (end - start) > kMaxBlock } });
            }
            result["matched"] = static_cast<int>(matched.size());
            result["types"] = std::move(matched);
            return result;
        }
    } // namespace

    Json BuildScriptApiDigest(const std::string& language, const std::string& typeFilter)
    {
        if (language == "csharp")
            return BuildCSharpApi(typeFilter);
        if (language == "lua")
            return BuildLuaApi(typeFilter);

        Json err;
        err["error"] = "Unknown language '" + language + "'. Use 'csharp' or 'lua'.";
        return err;
    }
} // namespace OloEngine::MCP

#include "OloEnginePCH.h"
#include "Automation/AutomationAssetIndex.h"

#include "OloEngine/Asset/AssetExtensions.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace OloEngine::Automation
{
    namespace
    {
        // ---- extension sets ---------------------------------------------------

        std::string LowerExtension(const std::filesystem::path& path)
        {
            std::string extension = path.extension().string();
            std::ranges::transform(extension, extension.begin(), [](unsigned char ch)
                                   { return static_cast<char>(std::tolower(ch)); });
            return extension;
        }

        // The formats this scan can read as text. Every one is YAML (or, for the
        // two script languages, plain source that may name an asset by path).
        //
        // Hand-listed rather than derived from AssetExtensions, because the two
        // sets answer different questions: AssetExtensions says "can the registry
        // import this", and half of what it imports is binary (.png, .glb, .olmap,
        // .olotex, .olovol). It also does NOT contain ".olo", which is the
        // extension every scene in the project actually uses -- scenes are loaded
        // by SceneSerializer directly rather than through the registry, so a set
        // derived from the extension map would skip the single most important
        // referrer format there is.
        const std::unordered_set<std::string>& ScannableSet()
        {
            static const std::unordered_set<std::string> set = {
                ".olo", ".oloscene", ".olomaterial", ".oloprefab", ".olomesh", ".olosmesh",
                ".oloanimation", ".oloanimgraph", ".olomc", ".olosoundc", ".olosoundgraph",
                ".oloparticle", ".oloprobe", ".olodialogue", ".olosg", ".olobt", ".olofsm",
                ".oloinstances", ".olocine", ".olofluid", ".oloxpcurve", ".oloskilltree",
                ".olocharclass", ".olovs", ".olotileset", ".oloitem", ".oloquest", ".oloproj",
                ".yaml", ".yml", ".json", ".lua", ".cs",
                // glTF is JSON, and it names its textures by URI relative to
                // itself. Left out, a texture reachable only from a .gltf reads as
                // unreferenced -- the exact silent-loss case this index exists for.
                // A MINIFIED one is read too, by the quoted-literal sweep below:
                // the whole document is one line, so no key parses, but the URIs
                // are still quoted strings. Its sibling .glb is a binary container
                // and stays genuinely unscanned, counted in BinaryFilesSkipped.
                ".gltf"
            };
            return set;
        }

        // Extensions a VALUE may end with for it to be a path-reference candidate:
        // everything the registry can import, plus the two the registry does not
        // know but scenes reference anyway (.olo scenes, .lua scripts). Derived
        // from the live extension map on purpose -- a new importable format then
        // becomes referenceable without touching this file, which is the failure
        // mode a hand-copied list has.
        const std::unordered_set<std::string>& ReferenceableSet()
        {
            static const std::unordered_set<std::string> set = []
            {
                std::unordered_set<std::string> built;
                for (const std::string& extension : AssetExtensions::GetAllSupportedExtensions())
                    built.insert(extension); // already lowercase and dotted.
                built.insert(".olo");
                built.insert(".lua");
                return built;
            }();
            return set;
        }

        // ---- key classification for HANDLE references -------------------------
        //
        // Path candidates are recognised by their value, so they need no key list.
        // Handle candidates cannot be: a bare u64 in a scene is far more often an
        // ENTITY UUID than an asset handle, and listing every entity in a scene as
        // one of its asset dependencies would make the dependency answer useless.
        // So handles are key-filtered, and the filter is stated here rather than
        // implied -- a caller reports it as part of the coverage boundary.

        // Entity identity, never an asset. PrefabEntityID names an entity INSIDE a
        // prefab; PrefabID next to it is the prefab asset and is allowed below.
        bool IsEntityIdentityKey(std::string_view key)
        {
            static constexpr std::string_view kKeys[] = { "Entity", "Handle", "ParentHandle",
                                                          "PrefabEntityID", "ID", "EntityID" };
            return std::ranges::find(kKeys, key) != std::end(kKeys);
        }

        bool HandleKeyQualifies(std::string_view key)
        {
            if (IsEntityIdentityKey(key))
                return false;
            if (key == "PrefabID")
                return true;
            static constexpr std::string_view kSuffixes[] = {
                "Handle", "Asset", "Source", "Material", "Mesh", "Texture", "Prefab",
                "Graph", "Clip", "Sequence", "Tileset", "Collider", "Map", "Font", "Sound"
            };
            const auto endsWithAny = [](std::string_view candidate)
            {
                return std::ranges::any_of(kSuffixes, [candidate](std::string_view suffix)
                                           { return candidate.size() >= suffix.size() &&
                                                    candidate.ends_with(suffix); });
            };
            if (endsWithAny(key))
                return true;
            // ...and again without a trailing 's'. The block that owns an indexed
            // handle map is named in the PLURAL -- a StaticMesh writes
            // `MaterialTable: Materials: {0: <handle>}` -- so matching only the
            // singular missed every material a static mesh uses, which is exactly
            // the set a delete must refuse on.
            return key.size() > 1 && key.ends_with('s') && endsWithAny(key.substr(0, key.size() - 1));
        }

        // ---- line-level scalar extraction -------------------------------------
        //
        // A hand-rolled scanner rather than yaml-cpp, for one reason: a rewrite
        // needs the BYTE SPAN of the original value so the edit lands on that
        // value and nothing else. Round-tripping through a YAML emitter would
        // reformat every file it touched, turning a one-line reference fix into a
        // whole-file diff -- and these formats carry hand-written comments and
        // deliberate layout that a re-emit would destroy.

        struct ScalarLine
        {
            bool Found = false;    // a `Key:` was parsed on this line.
            bool HasValue = false; // ...and it carried a scalar after the colon.
            std::string Key;
            std::string Value;
            u32 Indent = 0; // column the key starts at, for nesting.
            u32 ValueColumn = 0;
        };

        bool IsKeyChar(char ch)
        {
            return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
                   ch == '_' || ch == '-' || ch == '.';
        }

        // Strip an unquoted trailing comment. A '#' only starts one at the start of
        // the (trimmed) text or after whitespace -- '#' inside a value, e.g. a
        // colour literal, is not a comment. Quotes suppress it entirely.
        std::string_view StripComment(std::string_view line)
        {
            bool inSingle = false;
            bool inDouble = false;
            for (sizet i = 0; i < line.size(); ++i)
            {
                const char ch = line[i];
                if (ch == '\'' && !inDouble)
                    inSingle = !inSingle;
                else if (ch == '"' && !inSingle)
                    inDouble = !inDouble;
                else if (ch == '#' && !inSingle && !inDouble && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t'))
                    return line.substr(0, i);
            }
            return line;
        }

        // Parse `  - Key: value` / `  Key: value` into key, value and the value's
        // column in the ORIGINAL line (not the stripped view), so an edit is
        // addressable.
        ScalarLine ParseScalarLine(std::string_view line)
        {
            const std::string_view text = StripComment(line);
            sizet cursor = 0;
            while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '\t'))
                ++cursor;
            // A sequence entry may carry the key: "- Key: value".
            if (cursor + 1 < text.size() && text[cursor] == '-' && (text[cursor + 1] == ' ' || text[cursor + 1] == '\t'))
            {
                cursor += 2;
                while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '\t'))
                    ++cursor;
            }
            // A JSON key is quoted, and glTF writes `"uri" : "x.jpg"` -- quoted key
            // AND a space before the colon. Accepting both is what makes a .gltf
            // readable by this scanner at all; without it every texture reachable
            // only from a glTF looks unreferenced. Harmless for YAML, which permits
            // a quoted key and a space before the colon too.
            const bool quotedKey = cursor < text.size() && text[cursor] == '"';
            if (quotedKey)
                ++cursor;
            const sizet keyStart = cursor;
            while (cursor < text.size() && IsKeyChar(text[cursor]))
                ++cursor;
            const sizet keyEnd = cursor;
            if (quotedKey)
            {
                if (cursor >= text.size() || text[cursor] != '"')
                    return {};
                ++cursor;
            }
            while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '\t'))
                ++cursor;
            if (keyEnd == keyStart || cursor >= text.size() || text[cursor] != ':')
                return {};
            // A key must be followed by whitespace or end of line; "http://x" is a
            // value with a colon in it, not a key.
            const sizet colon = keyEnd;
            ++cursor;
            if (cursor < text.size() && text[cursor] != ' ' && text[cursor] != '\t')
                return {};
            ScalarLine parsed;
            parsed.Found = true;
            parsed.Key = std::string(text.substr(keyStart, colon - keyStart));
            parsed.Indent = static_cast<u32>(keyStart);
            while (cursor < text.size() && (text[cursor] == ' ' || text[cursor] == '\t'))
                ++cursor;
            sizet valueEnd = text.size();
            while (valueEnd > cursor && (text[valueEnd - 1] == ' ' || text[valueEnd - 1] == '\t' ||
                                         text[valueEnd - 1] == '\r'))
                --valueEnd;
            // JSON separates members with a comma; it is punctuation, not part of
            // the value. Trailing whitespace is re-trimmed after it.
            if (valueEnd > cursor && text[valueEnd - 1] == ',')
            {
                --valueEnd;
                while (valueEnd > cursor && (text[valueEnd - 1] == ' ' || text[valueEnd - 1] == '\t'))
                    --valueEnd;
            }
            // A key with no scalar after it opens a nested block. Report it so the
            // scanner can push it as the parent of the indented lines below --
            // that is what makes `Materials:` / `  0: <handle>` readable as a
            // material reference rather than as a stray integer under the key "0".
            if (valueEnd <= cursor)
                return parsed;
            // Strip one layer of matching quotes, keeping the column aligned with
            // the UNQUOTED text so an edit replaces the value and leaves the quotes.
            if (valueEnd - cursor >= 2 && (text[cursor] == '"' || text[cursor] == '\'') &&
                text[valueEnd - 1] == text[cursor])
            {
                ++cursor;
                --valueEnd;
                if (valueEnd <= cursor)
                    return parsed; // an empty quoted value: a key with nothing in it.
            }
            parsed.HasValue = true;
            parsed.Value = std::string(text.substr(cursor, valueEnd - cursor));
            parsed.ValueColumn = static_cast<u32>(cursor);
            return parsed;
        }

        bool IsDecimalDigits(std::string_view value)
        {
            return !value.empty() && std::ranges::all_of(value, [](char ch)
                                                         { return ch >= '0' && ch <= '9'; });
        }

        // ---- path resolution --------------------------------------------------
        //
        // Step for step, EditorAssetManager::ImportAsset's own order (#887, #1098).
        // Deliberately a mirror rather than an improvement: the point of the
        // referrer answer is to predict what the ENGINE will load, so a smarter
        // resolver here would make the index disagree with reality.

        bool FileExists(const std::filesystem::path& path)
        {
            std::error_code ec;
            const bool exists = std::filesystem::exists(path, ec);
            return exists && !ec;
        }

        std::filesystem::path Canonical(const std::filesystem::path& path)
        {
            std::error_code ec;
            std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
            return ec ? path.lexically_normal() : canonical;
        }

        // EditorAssetManager's TryLegacyProjectPrefixedPath, same three guards:
        // at least two components, a leading component NOT present in the project
        // (so a real "Assets/..." directory is never eaten), and a remainder that
        // exists. Returns the leading component through `leading` so a re-spell can
        // put it back.
        std::filesystem::path TryLegacyPrefixed(const std::filesystem::path& projectRoot,
                                                const std::filesystem::path& value,
                                                std::filesystem::path& leading)
        {
            if (projectRoot.empty() || value.empty() || value.is_absolute())
                return {};
            auto it = value.begin();
            const auto end = value.end();
            if (it == end)
                return {};
            const std::filesystem::path first = *it;
            if (first.empty() || first == "." || first == "..")
                return {};
            std::filesystem::path remainder;
            for (++it; it != end; ++it)
                remainder /= *it;
            if (remainder.empty())
                return {};
            if (FileExists(projectRoot / first))
                return {};
            const std::filesystem::path candidate = projectRoot / remainder;
            if (!FileExists(candidate))
                return {};
            leading = first;
            return candidate;
        }

        struct Resolution
        {
            std::filesystem::path File;
            AssetReferenceAnchor Anchor = AssetReferenceAnchor::Unresolved;
            u32 BaseDirectoryIndex = 0;
        };

        // glTF is the ONE format whose references the engine resolves relative to
        // the referring file -- Assimp reads a URI that way. Everything else goes
        // through EditorAssetManager::ImportAsset, which has no such anchor, so
        // enabling it for a .olo would let this index claim a sibling file resolves
        // when the engine would never load it: a refused delete that should have
        // gone ahead, and a rewrite of a path nothing reads.
        bool UsesSourceRelativeReferences(const std::filesystem::path& sourceFile)
        {
            return LowerExtension(sourceFile) == ".gltf";
        }

        Resolution ResolveReferencePath(const std::string& rawValue, const AssetIndexScope& scope,
                                        const std::filesystem::path& sourceDirectory)
        {
            std::string normalized = rawValue;
            std::ranges::replace(normalized, '\\', '/');
            const std::filesystem::path value(normalized);
            Resolution resolution;
            if (value.is_absolute())
            {
                if (FileExists(value))
                {
                    resolution.File = Canonical(value);
                    resolution.Anchor = AssetReferenceAnchor::Absolute;
                }
                return resolution;
            }
            if (!scope.ProjectRoot.empty())
            {
                if (const std::filesystem::path projectRelative = scope.ProjectRoot / value; FileExists(projectRelative))
                {
                    resolution.File = Canonical(projectRelative);
                    resolution.Anchor = AssetReferenceAnchor::ProjectRelative;
                    return resolution;
                }
            }
            if (!scope.AssetDirectory.empty())
            {
                // Project::GetAssetFileSystemPath, which is what Scene.cpp uses for a
                // LuaScriptComponent's ScriptFile and SceneSerializer for several
                // component paths. Checked-in content leans on it
                // ("Scripts/LuaScripts/x.lua", "Audio/ding.wav"), so leaving it out
                // drops real referrers.
                if (const std::filesystem::path assetRelative = scope.AssetDirectory / value; FileExists(assetRelative))
                {
                    resolution.File = Canonical(assetRelative);
                    resolution.Anchor = AssetReferenceAnchor::AssetDirectoryRelative;
                    return resolution;
                }
            }
            if (!scope.ProjectRoot.empty())
            {
                std::filesystem::path leading;
                if (const std::filesystem::path legacy = TryLegacyPrefixed(scope.ProjectRoot, value, leading);
                    !legacy.empty())
                {
                    resolution.File = Canonical(legacy);
                    resolution.Anchor = AssetReferenceAnchor::LegacyProjectPrefixed;
                    return resolution;
                }
            }
            for (u32 index = 0; index < static_cast<u32>(scope.BaseDirectories.size()); ++index)
            {
                const std::filesystem::path candidate = scope.BaseDirectories[index] / value;
                if (!FileExists(candidate))
                    continue;
                resolution.File = Canonical(candidate);
                resolution.Anchor = AssetReferenceAnchor::BaseDirectory;
                resolution.BaseDirectoryIndex = index;
                return resolution;
            }
            // LAST, after every anchor the engine's own importer uses, so this can
            // only ever rescue a value that would otherwise be unresolved. glTF
            // URIs are relative to the .gltf file and Assimp reads them that way.
            if (!sourceDirectory.empty())
            {
                if (const std::filesystem::path candidate = sourceDirectory / value; FileExists(candidate))
                {
                    resolution.File = Canonical(candidate);
                    resolution.Anchor = AssetReferenceAnchor::SourceRelative;
                    return resolution;
                }
            }
            return resolution;
        }

        // ---- file reading -----------------------------------------------------

        bool ReadFileText(const std::filesystem::path& path, std::string& out)
        {
            std::ifstream input(path, std::ios::binary);
            if (!input)
                return false;
            out.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
            return !input.bad();
        }

        // ---- quoted string literals -------------------------------------------
        //
        // The line scanner above reads `key: value`. That is most of what these
        // formats contain, and none of what some of them contain:
        //
        //   * a MINIFIED .gltf is the whole document on one line, so no key ever
        //     parses and every texture it names was invisible;
        //   * a .lua or .cs names an asset inside a call -- AssetManager.Load(
        //     "Assets/Textures/x.png") -- which is not a key/value line either;
        //   * a YAML flow sequence, ["a.png", "b.png"], likewise.
        //
        // All three are the same shape: a quoted string that happens to be a path.
        // So after the key parse fails to produce one, sweep the line for quoted
        // literals ending in a known asset extension. Over-collecting is the safe
        // direction -- resolution is the filter -- and it turns three separate
        // "this format is not really scanned" admissions into actual coverage.
        struct QuotedLiteral
        {
            std::string Text;
            std::string Key; // the `"key":` it follows, when there is one.
            u32 Column = 0;
        };

        std::vector<QuotedLiteral> QuotedLiterals(std::string_view line)
        {
            std::vector<QuotedLiteral> found;
            std::string previous;
            bool previousWasKey = false;
            for (sizet i = 0; i < line.size(); ++i)
            {
                if (line[i] != '"')
                    continue;
                const sizet start = i + 1;
                sizet end = start;
                while (end < line.size() && line[end] != '"')
                {
                    // A backslash escapes the next character, so an escaped quote
                    // does not end the literal.
                    if (line[end] == '\\' && end + 1 < line.size())
                        ++end;
                    ++end;
                }
                if (end >= line.size())
                    break;
                std::string text(line.substr(start, end - start));
                // `"key" : "value"` -- a literal followed by a colon is the KEY of
                // the next one, which is what gives a minified glTF's URI its name.
                sizet after = end + 1;
                while (after < line.size() && (line[after] == ' ' || line[after] == '	'))
                    ++after;
                const bool isKey = after < line.size() && line[after] == ':';
                if (!isKey)
                    found.push_back(QuotedLiteral{ text, previousWasKey ? previous : std::string{},
                                                   static_cast<u32>(start) });
                previous = std::move(text);
                previousWasKey = isKey;
                i = end;
            }
            return found;
        }

        // One open block: the key that owns the more-indented lines below it.
        struct BlockScope
        {
            u32 Indent = 0;
            std::string Key;
        };

        // A handle under a NUMERIC key is an entry in an indexed map -- the
        // StaticMesh MaterialTable writes `Materials:` then `0: <handle>`,
        // `1: <handle>`. The key "0" qualifies nothing on its own, so without the
        // enclosing block name every material a static mesh uses would be missed,
        // and a delete would report zero referrers for a material half the meshes
        // in the project point at. Inherit the nearest non-numeric ancestor.
        const BlockScope* QualifyingAncestor(const std::vector<BlockScope>& stack)
        {
            for (auto it = stack.rbegin(); it != stack.rend(); ++it)
            {
                if (!IsDecimalDigits(it->Key))
                    return &*it;
            }
            return nullptr;
        }

        // Shared by the key/value path and the literal sweep so the two cannot
        // drift on what counts, what resolves, or what gets recorded.
        bool LooksReferenceable(const std::string& value)
        {
            std::string lowered = value;
            std::ranges::transform(lowered, lowered.begin(), [](unsigned char ch)
                                   { return static_cast<char>(std::tolower(ch)); });
            const sizet dot = lowered.find_last_of('.');
            return dot != std::string::npos && ReferenceableSet().contains(lowered.substr(dot));
        }

        void RecordPathReference(const std::filesystem::path& file, u32 lineNumber, std::string key,
                                 const std::string& value, u32 column, const AssetIndexScope& scope,
                                 AssetIndex& index)
        {
            const Resolution resolution = ResolveReferencePath(
                value, scope,
                UsesSourceRelativeReferences(file) ? file.parent_path() : std::filesystem::path{});
            // A file naming ITSELF is never a reference. Every scene opens with
            // its own title and every Lua script here carries a header comment
            // quoting its own path; both resolve straight back to the file they
            // are written in. Dropped at extraction rather than only at query
            // time so the counts mean something.
            if (resolution.File == file)
                return;
            // A bare filename with no separator is ambiguous between a reference
            // and a NAME that happens to end in an asset extension, and there is
            // no way to tell; it counts only when it resolves.
            if (resolution.Anchor == AssetReferenceAnchor::Unresolved &&
                value.find('/') == std::string::npos &&
                value.find('\\') == std::string::npos)
            {
                return;
            }
            AssetReference reference;
            reference.SourceFile = file;
            reference.Line = lineNumber;
            reference.Key = std::move(key);
            reference.RawValue = value;
            reference.Kind = AssetReferenceKind::Path;
            reference.ResolvedFile = resolution.File;
            reference.Anchor = resolution.Anchor;
            reference.BaseDirectoryIndex = resolution.BaseDirectoryIndex;
            reference.ValueColumn = column;
            if (resolution.Anchor == AssetReferenceAnchor::Unresolved)
                ++index.Coverage.UnresolvedReferences;
            index.References.push_back(std::move(reference));
        }

        void ScanFile(const std::filesystem::path& file, const std::string& text, const AssetIndexScope& scope,
                      AssetIndex& index)
        {
            std::vector<BlockScope> stack;
            u32 lineNumber = 0;
            sizet lineStart = 0;
            while (lineStart <= text.size())
            {
                ++lineNumber;
                const sizet lineEnd = std::min(text.find('\n', lineStart), text.size());
                const std::string_view line(text.data() + lineStart, lineEnd - lineStart);
                const bool lastLine = lineEnd == text.size();
                lineStart = lineEnd + 1;

                // Whether the key/value parse already claimed a path on this line,
                // so the literal sweep below does not record it a second time.
                bool claimedByKeyParse = false;
                const ScalarLine parsed = ParseScalarLine(line);
                if (parsed.Found)
                {
                    while (!stack.empty() && stack.back().Indent >= parsed.Indent)
                        stack.pop_back();
                }

                if (parsed.Found && !parsed.HasValue)
                {
                    stack.push_back(BlockScope{ parsed.Indent, parsed.Key });
                }
                else if (parsed.Found)
                {
                    // The name a key filter is applied to, and the name reported.
                    // They differ only for an indexed map entry, where "Materials/0"
                    // says far more to a reader than "0".
                    std::string qualifyKey = parsed.Key;
                    std::string reportedKey = parsed.Key;
                    if (IsDecimalDigits(parsed.Key))
                    {
                        if (const BlockScope* ancestor = QualifyingAncestor(stack); ancestor != nullptr)
                        {
                            qualifyKey = ancestor->Key;
                            reportedKey = ancestor->Key + "/" + parsed.Key;
                        }
                    }

                    if (IsDecimalDigits(parsed.Value))
                    {
                        u64 handle = 0;
                        const char* first = parsed.Value.data();
                        const char* last = first + parsed.Value.size();
                        if (HandleKeyQualifies(qualifyKey) &&
                            std::from_chars(first, last, handle).ec == std::errc{} && handle != 0)
                        {
                            AssetReference reference;
                            reference.SourceFile = file;
                            reference.Line = lineNumber;
                            reference.Key = std::move(reportedKey);
                            reference.RawValue = parsed.Value;
                            reference.Kind = AssetReferenceKind::Handle;
                            reference.HandleValue = handle;
                            reference.ValueColumn = parsed.ValueColumn;
                            index.References.push_back(std::move(reference));
                        }
                    }
                    else if (LooksReferenceable(parsed.Value))
                    {
                        RecordPathReference(file, lineNumber, std::move(reportedKey), parsed.Value,
                                            parsed.ValueColumn, scope, index);
                        claimedByKeyParse = true;
                    }
                }

                // The literal sweep, for everything the key parse cannot read: a
                // MINIFIED glTF (the whole document on one line, so no key parses
                // at all), a script call such as AssetManager.Load("Assets/x.png"),
                // a YAML flow sequence. Without it those formats counted toward
                // FilesScanned while contributing nothing, which made an index that
                // had never looked at them report itself complete.
                if (!claimedByKeyParse)
                {
                    for (const QuotedLiteral& literal : QuotedLiterals(line))
                    {
                        if (!LooksReferenceable(literal.Text))
                            continue;
                        RecordPathReference(file, lineNumber,
                                            literal.Key.empty() ? std::string("(string)") : literal.Key,
                                            literal.Text, literal.Column, scope, index);
                    }
                }

                if (lastLine)
                    break;
            }
        }
    } // namespace

    bool IsScannableAssetFile(const std::filesystem::path& path)
    {
        return ScannableSet().contains(LowerExtension(path));
    }

    std::vector<std::string> ScannableExtensions()
    {
        std::vector<std::string> extensions(ScannableSet().begin(), ScannableSet().end());
        std::ranges::sort(extensions);
        return extensions;
    }

    AssetIndex BuildAssetIndex(const AssetIndexScope& scope)
    {
        AssetIndex index;
        // An empty index from a scan that never ran is indistinguishable, at the
        // call site, from "nothing references this asset" -- so say so here rather
        // than handing back a clean-looking zero.
        if (scope.ProjectRoot.empty())
        {
            index.Coverage.Complete = false;
            index.Coverage.IncompleteReason = "No project root was given; nothing was scanned.";
            return index;
        }
        std::error_code ec;
        if (!std::filesystem::is_directory(scope.ProjectRoot, ec) || ec)
        {
            index.Coverage.Complete = false;
            index.Coverage.IncompleteReason =
                "The project root is not a readable directory (" + scope.ProjectRoot.generic_string() +
                (ec ? "): " + ec.message() : ")") + "; nothing was scanned.";
            return index;
        }

        // Walk from the CANONICAL root, so every path the iterator hands back is
        // already canonical and can be compared to a resolved reference directly.
        // One syscall here instead of a weakly_canonical per file -- and, more to
        // the point, without it a referrer lookup silently misses whenever the
        // caller's spelling of the root differs from the scan's (a symlinked
        // project directory, or Windows handing back a different case).
        AssetIndexScope walked = scope;
        walked.ProjectRoot = Canonical(scope.ProjectRoot);

        std::unordered_set<std::string> binaryExtensions;
        // NOT skip_permission_denied. That option drops an unreadable directory
        // without setting an error, so its files simply never appear and the scan
        // looks complete -- a lower FilesScanned that nothing points at, which is
        // precisely the silent shortfall this coverage block exists to prevent.
        // Letting the error surface instead ends the walk and marks the index
        // incomplete, so a destructive command refuses and says why. Aborting on
        // an unreadable directory is the safe direction: the alternative is
        // deleting an asset that something in it referenced.
        std::filesystem::recursive_directory_iterator it(walked.ProjectRoot, ec);
        if (ec)
        {
            index.Coverage.Complete = false;
            index.Coverage.IncompleteReason = "Could not start walking the project: " + ec.message();
            return index;
        }
        const std::filesystem::recursive_directory_iterator end;
        while (it != end)
        {
            const std::filesystem::path file = it->path();
            std::error_code entryEc;
            const bool regular = it->is_regular_file(entryEc) && !entryEc;

            // Advance FIRST and bail on an increment error. After a failed
            // increment the iterator is unspecified, so touching it again -- to
            // report the path it was on, say -- is undefined; the directory that
            // vanished mid-walk is named from the entry we still hold.
            it.increment(ec);
            const bool walkBroke = static_cast<bool>(ec);

            if (!regular)
            {
                if (walkBroke)
                {
                    index.Coverage.Complete = false;
                    index.Coverage.IncompleteReason = "The directory walk stopped early at " +
                                                      file.parent_path().generic_string() + ".";
                    index.Coverage.UnreadableFiles.push_back(file.parent_path().generic_string() +
                                                             " (directory walk stopped here)");
                    break;
                }
                continue;
            }
            if (!IsScannableAssetFile(file))
            {
                if (std::string extension = LowerExtension(file);
                    !extension.empty() && AssetExtensions::IsExtensionSupported(extension))
                {
                    ++index.Coverage.BinaryFilesSkipped;
                    binaryExtensions.insert(std::move(extension));
                }
                if (walkBroke)
                {
                    index.Coverage.Complete = false;
                    index.Coverage.IncompleteReason = "The directory walk stopped early at " +
                                                      file.parent_path().generic_string() + ".";
                    index.Coverage.UnreadableFiles.push_back(file.parent_path().generic_string() +
                                                             " (directory walk stopped here)");
                    break;
                }
                continue;
            }
            if (index.Coverage.FilesScanned >= walked.MaxFiles)
            {
                index.Coverage.Truncated = true;
                break;
            }
            std::string text;
            if (!ReadFileText(file, text))
            {
                // A file we could not read is a file whose references we do not
                // know. Countable AND disqualifying, not merely listed.
                index.Coverage.Complete = false;
                index.Coverage.IncompleteReason =
                    "One or more project files could not be read; their references are unknown.";
                index.Coverage.UnreadableFiles.push_back(file.generic_string());
            }
            else
            {
                ++index.Coverage.FilesScanned;
                ScanFile(file, text, walked, index);
            }
            if (walkBroke)
            {
                index.Coverage.Complete = false;
                index.Coverage.IncompleteReason = "The directory walk stopped early at " +
                                                  file.parent_path().generic_string() + ".";
                index.Coverage.UnreadableFiles.push_back(file.parent_path().generic_string() +
                                                         " (directory walk stopped here)");
                break;
            }
        }

        index.Coverage.BinaryExtensionsSkipped.assign(binaryExtensions.begin(), binaryExtensions.end());
        std::ranges::sort(index.Coverage.BinaryExtensionsSkipped);
        std::ranges::sort(index.Coverage.UnreadableFiles);
        index.Coverage.ReferencesFound = static_cast<u32>(index.References.size());
        return index;
    }

    std::vector<AssetReference> FindReferrers(const AssetIndex& index, const std::filesystem::path& targetFile,
                                              u64 targetHandle)
    {
        const std::filesystem::path target = targetFile.empty() ? targetFile : Canonical(targetFile);
        std::vector<AssetReference> referrers;
        for (const AssetReference& reference : index.References)
        {
            // An asset is not its own referrer -- a material that names its own
            // file, or a scene saved with its own name in a header field, is not
            // something a move would break.
            if (!target.empty() && reference.SourceFile == target)
                continue;
            const bool matches = reference.Kind == AssetReferenceKind::Handle
                                     ? (targetHandle != 0 && reference.HandleValue == targetHandle)
                                     : (!target.empty() && reference.ResolvedFile == target);
            if (matches)
                referrers.push_back(reference);
        }
        return referrers;
    }

    std::vector<AssetReference> FindDependencies(const AssetIndex& index, const std::filesystem::path& sourceFile)
    {
        const std::filesystem::path source = Canonical(sourceFile);
        std::vector<AssetReference> dependencies;
        for (const AssetReference& reference : index.References)
        {
            if (reference.SourceFile != source)
                continue;
            // A self-reference is not a dependency, for the same reason it is not
            // a referrer above.
            if (reference.Kind == AssetReferenceKind::Path && reference.ResolvedFile == source)
                continue;
            dependencies.push_back(reference);
        }
        return dependencies;
    }

    std::string RespellReference(const AssetReference& reference, const std::filesystem::path& oldTarget,
                                 const std::filesystem::path& newTarget, const AssetIndexScope& scope)
    {
        // A handle reference survives a move untouched -- the handle is the
        // identity and the registry re-keys it. Nothing to rewrite, and saying so
        // by returning empty is what keeps a caller from writing a path into a
        // handle field.
        if (reference.Kind != AssetReferenceKind::Path)
            return {};
        if (reference.Anchor == AssetReferenceAnchor::Unresolved)
            return {};
        if (Canonical(oldTarget) != reference.ResolvedFile)
            return {};

        // The non-throwing overload throughout: a filesystem error here would
        // otherwise escape as filesystem_error before PlanReferenceEdits can turn
        // it into its controlled refusal, and a move that throws mid-plan is
        // exactly the half-applied state the plan-first design exists to avoid.
        // An error becomes an empty path, which every branch below already treats
        // as "cannot re-spell".
        const auto relativeTo = [](const std::filesystem::path& target,
                                   const std::filesystem::path& base) -> std::filesystem::path
        {
            std::error_code ec;
            std::filesystem::path result = std::filesystem::relative(target, base, ec);
            return ec ? std::filesystem::path{} : result;
        };

        std::filesystem::path respelled;
        switch (reference.Anchor)
        {
            case AssetReferenceAnchor::Absolute:
                respelled = newTarget;
                break;
            case AssetReferenceAnchor::ProjectRelative:
                respelled = relativeTo(newTarget, scope.ProjectRoot);
                break;
            case AssetReferenceAnchor::AssetDirectoryRelative:
                if (scope.AssetDirectory.empty())
                    return {};
                respelled = relativeTo(newTarget, scope.AssetDirectory);
                break;
            case AssetReferenceAnchor::LegacyProjectPrefixed:
            {
                // Put the stale leading component back so the file keeps the
                // spelling it had. Re-spelling it into the modern form would be a
                // second, unrelated change smuggled into a move -- and one that
                // silently depends on the legacy fallback still existing.
                const std::filesystem::path original(reference.RawValue);
                const std::filesystem::path leading = original.begin() == original.end()
                                                          ? std::filesystem::path{}
                                                          : *original.begin();
                // Check the remainder BEFORE re-attaching the prefix: joining a
                // stale leading component onto an empty relative path would
                // produce a path that names the prefix directory itself.
                if (const std::filesystem::path remainder = relativeTo(newTarget, scope.ProjectRoot);
                    !remainder.empty())
                {
                    respelled = leading / remainder;
                }
                break;
            }
            case AssetReferenceAnchor::BaseDirectory:
            {
                if (reference.BaseDirectoryIndex >= scope.BaseDirectories.size())
                    return {};
                respelled = relativeTo(newTarget, scope.BaseDirectories[reference.BaseDirectoryIndex]);
                break;
            }
            case AssetReferenceAnchor::SourceRelative:
                respelled = relativeTo(newTarget, reference.SourceFile.parent_path());
                break;
            case AssetReferenceAnchor::Unresolved:
                return {};
        }
        if (respelled.empty())
            return {};

        std::string text = respelled.generic_string();

        // Keep the ORIGINAL CASING of the part of the path that did not change.
        // On a case-insensitive filesystem a value spelled "assets/textures/x.png"
        // resolves against a directory named "Assets/Textures", so re-spelling
        // from the filesystem alone rewrites the case too: the move edits a line
        // it was not asked to change, and moving back does not restore it. A round
        // trip has to be a no-op, and a move must not smuggle an unrelated
        // re-spelling into somebody's diff -- the same reason the legacy prefix is
        // put back rather than modernised. So take the original's characters for
        // the longest prefix that matches case-insensitively, and the computed
        // path's for the rest.
        {
            const auto lower = [](char ch)
            { return static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); };
            sizet shared = 0;
            while (shared < text.size() && shared < reference.RawValue.size() &&
                   lower(text[shared]) == lower(reference.RawValue[shared]))
            {
                ++shared;
            }
            text.replace(0, shared, reference.RawValue, 0, shared);
        }
        // Keep the separator the file already used. Mixing '\' and '/' inside one
        // file is legal but reads as churn in the diff.
        if (reference.RawValue.find('\\') != std::string::npos &&
            reference.RawValue.find('/') == std::string::npos)
        {
            std::ranges::replace(text, '/', '\\');
        }
        return text;
    }

    bool ApplyReferenceEdit(std::string& fileText, const AssetReference& reference, const std::string& replacement)
    {
        if (reference.Line == 0 || replacement.empty())
            return false;
        sizet lineStart = 0;
        for (u32 line = 1; line < reference.Line; ++line)
        {
            const sizet newline = fileText.find('\n', lineStart);
            if (newline == std::string::npos)
                return false;
            lineStart = newline + 1;
        }
        const sizet lineEnd = std::min(fileText.find('\n', lineStart), fileText.size());
        const sizet valueStart = lineStart + reference.ValueColumn;
        if (valueStart + reference.RawValue.size() > lineEnd)
            return false;
        // The span must still hold exactly what the index recorded. If it does
        // not, the file moved under us and the honest answer is to refuse -- a
        // fallback to search-and-replace here would edit some other line that
        // happens to carry the same text, which is precisely the silent corruption
        // this whole command set exists to prevent.
        if (fileText.compare(valueStart, reference.RawValue.size(), reference.RawValue) != 0)
            return false;
        fileText.replace(valueStart, reference.RawValue.size(), replacement);
        return true;
    }
} // namespace OloEngine::Automation

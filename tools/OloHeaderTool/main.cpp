// OloHeaderTool — Generates scripting bindings from OLO_PROPERTY() annotations,
// plus the ECS AllComponents type list from `struct *Component` definitions.
//
// Scans C++ headers and emits:
//   1. C++ Mono getter/setter functions       (ScriptGlueBindings.inl)
//   2. C++ OLO_ADD_INTERNAL_CALL registrations (ScriptGlueRegistrations.inl)
//   3. C# proxy Component classes             (Components.Generated.cs)
//   4. C# InternalCall declarations           (InternalCalls.Generated.cs)
//   5. The AllComponents type list            (AllComponents.Generated.inl)
//      — one entry per `struct *Component` definition under the scan dir,
//        included by Scene/Components.h. This collapses one of the six
//        hand-maintained ECS component touch-points into codegen so a new
//        component is registered for scene-copy / prefab / HasComponent<T>()
//        automatically instead of by remembering to edit the tuple.
//   6. The save-game capture/restore lists     (SaveGameComponentCapture/
//      Restore.Generated.inl) — the SAVE_COMPONENT(...) and
//      TRY_LOAD_COMPONENT(...) enumerations #include'd by
//      SaveGame/SaveGameSerializer.cpp. Same `struct *Component` scan as the
//      tuple, minus a save-game-specific exclusion set (components without a
//      SaveGameComponentSerializer::Serialize() overload). Collapses the two
//      most dangerous unguarded ECS touch-points — a component missing from
//      either list was silently dropped from every save-game.
//   7. The Scene OnComponent{Added,Removed} no-op lists (OnComponentAdded/
//      Removed.Generated.inl in the same Scene/Generated dir) — the
//      OLO_ON_COMPONENT_ADDED_NOOP(T) / OLO_ON_COMPONENT_REMOVED_NOOP(T)
//      invocations #include'd by Scene/Scene.cpp. Same `struct *Component` scan
//      as the tuple, minus two custom-handler exclusion sets (the components
//      whose add/remove callback is hand-written because it does real init/
//      teardown). The OnComponentAdded/Removed primary templates are
//      declaration-only, so a component added/removed without a specialization
//      is an engine/editor link error — this collapses the touch-point while
//      keeping that link error as the safety net for anything mis-excluded.
//   8. The Scene serializer per-component serialize/deserialize blocks
//      (Scene{Serialize,Deserialize}Components.Generated.inl in the same
//      Scene/Generated dir) — one `if (entity.HasComponent<T>()) { … }` /
//      `if (auto node = entity["T"]; node) { … }` block per component whose
//      EVERY data member is a primitive / glm::vec* / std::string (a separate
//      full data-member scan, NOT the OLO_PROPERTY scan — the serializer
//      persists every field, not just script-exposed ones), minus the
//      kComponentsCustomSerialize exclusion set. #include'd by
//      SceneSerializer.cpp. Enum, AssetHandle, and Ref<T> (of an Asset-derived T)
//      fields, std::vector of a trivial element, all-trivial nested structs,
//      std::unordered_set of a sortable trivial scalar, and std::unordered_map
//      keyed by std::string are handled too; a component with any STILL-unhandled
//      non-trivial field (std::array, a non-string-keyed map, Ref<T> of a
//      non-asset type, …) is classified non-trivial and stays hand-written.
//      Collapses the last big *unguarded* ECS touch-point — a forgotten field was
//      silent scene-data loss.
//   9. The MCP `olo_entity_set_field` writable-field registry
//      (McpFieldRegistry.Generated.inl, #include'd by
//      OloEditor/src/MCP/McpGenericFieldWrite.h). One entry per public,
//      JSON-coercible member of every component (reusing the item 8 field scan),
//      PLUS a small allowlisted set of setter-based entries reusing the
//      OLO_PROPERTY scan (item 1's data source) for a component whose fields live
//      behind a private member reached only through a Get/Set expression pair —
//      see EmitMcpSetterFields' comment (issue #607's AudioSourceComponent slice).
//
// Usage:
//   OloHeaderTool <scan_dir> <cpp_out_dir> <cs_out_dir> <scene_out_dir> <savegame_out_dir>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

// ─── Data Structures ────────────────────────────────────────────────────────────

enum class PropType
{
    Float,
    Bool,
    Int,
    UInt,
    U64,
    // Small integer members (u8/u16/i8/i16). Scene-serializer-only: written as a
    // wider int (static_cast<u32>/<i32> on emit) so yaml-cpp does NOT serialize a
    // u8/i8 as a raw character (its convert<unsigned char>::encode does `stream <<
    // rhs`, emitting the byte as a char); read back via .as<decltype(member)>,
    // whose char-type decode special-case parses the numeric string and range-
    // checks before narrowing. Matches the hand-written `static_cast<u32>(...)`
    // blocks (e.g. InstancePortalComponent::InstanceType). The scripting path never
    // produces these (CppTypeToPropType doesn't map them) — only SceneSerType does.
    SmallUInt,   // u8 / u16
    SmallInt,    // i8 / i16
    AssetHandle, // AssetHandle / UUID — a u64 wrapper. Scene-serializer-only:
                 // round-trips as a u64 (static_cast<u64> on write, .as<u64> on
                 // read) but is emitted distinctly from a plain u64 so the
                 // serializer codegen knows to bridge the UUID<->u64 conversion.
                 // The scripting path never produces this (CppTypeToPropType maps
                 // AssetHandle -> U64), so only SceneSerType emits it.
    Vec2,
    Vec3,
    Vec4,
    // glm integer-vector / quaternion / matrix members. Scene-serializer-only:
    // round-trip through the glm Encode/Decode helpers in Core/YAMLConverters.h
    // (Emitter<< on write, .as<glm::T>(default) on read — float components are
    // finiteness-validated by the Decode helpers, integers need no check). The
    // scripting path never produces these (CppTypeToPropType doesn't map them) —
    // only SceneSerType does.
    IVec2,
    IVec3,
    IVec4,
    Quat,
    Mat3,
    Mat4,
    String,
    Enum,   // An `enum` / `enum class` type. Scene-serializer-only: round-trips as
            // an int (static_cast<int> on write, .as<int> on read), cast back to the
            // field's own type via decltype so a nested enum (e.g.
            // AnimationStateComponent::State) needs no qualified spelling at the
            // SceneSerializer level. Detected by name against the collected enum-type
            // set (see CollectEnumTypes) in ParseComponentFields, not by SceneSerType
            // (which only knows built-in type names). The scripting path never produces
            // this — only the scene serializer codegen emits it.
    Struct, // A nested `struct` (or `std::vector<struct>`) whose every member is
            // itself a serializer-trivial type (recursively). Scene-serializer-only:
            // round-trips as a nested YAML sub-map (scalar member) or a sequence of
            // sub-maps (vector member). The sub-struct's fields are carried in
            // SerField::subFields; SerField::isVector distinguishes the two shapes.
            // Detected by recursively parsing the member type's struct definition
            // (see CollectStructBodies / ClassifyStruct in ParseComponentFields);
            // #451's nested-struct slice. The scripting path never produces this.
    Ref,    // A `Ref<T>` runtime asset handle, where T transitively derives from
            // Asset (see CollectAssetTypes) and is therefore resolvable via
            // AssetManager::GetAsset<T>(handle). Scene-serializer-only: persisted as
            // a "<Key>Handle" u64 (matches the hand-written MeshComponent::m_MeshSource
            // -> "MeshSourceHandle" idiom) written only when the Ref is non-null and
            // actually asset-manager-registered (GetHandle() != 0); resolved back via
            // AssetManager::GetAsset<T> on read, falling back to a null Ref if the key
            // is absent or the asset no longer resolves. SerField::refType carries the
            // spelling of T for the generated AssetManager::GetAsset<T> call. A
            // Ref<T> of a non-asset type (Skeleton, FoliageRenderer, …) is NOT this
            // PropType — it stays Unknown and marks the component non-trivial, unless
            // the field carries OLO_SERIALIZE(Skip) (issue #451's Ref<T> slice). The
            // scripting path never produces this — only the scene serializer codegen
            // emits it.
    Unknown
};

struct PropertyDef
{
    std::string scriptName; // Name exposed to scripts (e.g., "Color")
    PropType type{ PropType::Unknown };
    std::string cppField;  // C++ field name (e.g., "m_Color")
    std::string customGet; // Custom getter expression (empty = direct field)
    std::string customSet; // Custom setter expression (empty = direct field)
};

struct ComponentDef
{
    std::string name;       // e.g., "DirectionalLightComponent"
    std::string sourceFile; // e.g., "Components.h"
    std::vector<PropertyDef> properties;
};

// ─── Type Mapping ───────────────────────────────────────────────────────────────

static PropType CppTypeToPropType(const std::string& t)
{
    if (t == "f32" || t == "float")
        return PropType::Float;
    if (t == "bool")
        return PropType::Bool;
    if (t == "i32" || t == "int")
        return PropType::Int;
    if (t == "u32")
        return PropType::UInt;
    // AssetHandle is a UUID typedef (u64 wrapper) with implicit operator u64() /
    // implicit ctor(u64), so getter/setter codegen can treat it as a plain 64-bit
    // scalar. UUID itself isn't mapped here — entity IDs cross the Mono boundary
    // separately and exposing them as authored properties would be a footgun.
    if (t == "u64" || t == "AssetHandle")
        return PropType::U64;
    if (t == "glm::vec2")
        return PropType::Vec2;
    if (t == "glm::vec3")
        return PropType::Vec3;
    if (t == "glm::vec4")
        return PropType::Vec4;
    if (t == "std::string")
        return PropType::String;
    return PropType::Unknown;
}

static PropType StringToPropType(const std::string& s)
{
    if (s == "float")
        return PropType::Float;
    if (s == "bool")
        return PropType::Bool;
    if (s == "int")
        return PropType::Int;
    if (s == "uint")
        return PropType::UInt;
    if (s == "ulong" || s == "u64" || s == "AssetHandle")
        return PropType::U64;
    if (s == "vec2")
        return PropType::Vec2;
    if (s == "vec3")
        return PropType::Vec3;
    if (s == "vec4")
        return PropType::Vec4;
    if (s == "string")
        return PropType::String;
    return PropType::Unknown;
}

static bool IsScalar(PropType t)
{
    return t == PropType::Float || t == PropType::Bool || t == PropType::Int || t == PropType::UInt || t == PropType::U64;
}

static std::string CppReturnType(PropType t)
{
    switch (t)
    {
        case PropType::Float:
            return "float";
        case PropType::Bool:
            return "bool";
        case PropType::Int:
            return "int";
        case PropType::UInt:
            return "unsigned int";
        case PropType::U64:
            return "u64";
        default:
            return "void";
    }
}

static std::string GlmType(PropType t)
{
    switch (t)
    {
        case PropType::Vec2:
            return "glm::vec2";
        case PropType::Vec3:
            return "glm::vec3";
        case PropType::Vec4:
            return "glm::vec4";
        default:
            return "";
    }
}

static std::string CsType(PropType t)
{
    switch (t)
    {
        case PropType::Float:
            return "float";
        case PropType::Bool:
            return "bool";
        case PropType::Int:
            return "int";
        case PropType::UInt:
            return "uint";
        case PropType::U64:
            return "ulong";
        case PropType::Vec2:
            return "Vector2";
        case PropType::Vec3:
            return "Vector3";
        case PropType::Vec4:
            return "Vector4";
        case PropType::String:
            return "string";
        default:
            return "???";
    }
}

// ─── String Utilities ──────────────────────────────────────────────────────────

static std::string Trim(const std::string& s)
{
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Strips /* ... */ block comment content from a single line, updating the
// inBlockComment state for multi-line comments.  Returns the line with
// block-comment regions removed.
static std::string StripBlockComments(const std::string& input, bool& inBlockComment)
{
    std::string stripped;
    stripped.reserve(input.size());
    size_t pos = 0;
    while (pos < input.size())
    {
        if (inBlockComment)
        {
            if (auto end = input.find("*/", pos); end != std::string::npos)
            {
                inBlockComment = false;
                pos = end + 2;
            }
            else
            {
                pos = input.size();
            }
        }
        else
        {
            auto blockStart = input.find("/*", pos);
            if (blockStart != std::string::npos)
            {
                stripped += input.substr(pos, blockStart - pos);
                inBlockComment = true;
                pos = blockStart + 2;
            }
            else
            {
                stripped += input.substr(pos);
                pos = input.size();
            }
        }
    }
    return stripped;
}

static std::string StripPrefix(const std::string& name, const std::string& prefix)
{
    if (name.size() > prefix.size() && name.substr(0, prefix.size()) == prefix)
        return name.substr(prefix.size());
    return name;
}

static std::string ReplaceAll(std::string s, const std::string& from, const std::string& to)
{
    for (size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size())
        s.replace(pos, from.size(), to);
    return s;
}

// Split a template-argument-list string at top-level commas — i.e. commas at
// angle-bracket depth 0 — so `std::unordered_map<std::string, f32>`'s inner text
// "std::string, f32" splits into ["std::string", "f32"] while a hypothetical
// nested-template argument keeps its internal comma intact. Used to separate a
// std::unordered_map<K, V[, Hash[, KeyEq]]> member's template arguments; the
// caller rejects anything other than exactly 2 parts (a custom hash/equality
// functor, or a malformed declaration).
static std::vector<std::string> SplitTopLevelCommas(const std::string& s)
{
    std::vector<std::string> parts;
    int depth = 0;
    std::string cur;
    for (char c : s)
    {
        if (c == '<')
            ++depth;
        else if (c == '>')
            --depth;
        if (c == ',' && depth == 0)
        {
            parts.push_back(Trim(cur));
            cur.clear();
        }
        else
        {
            cur += c;
        }
    }
    parts.push_back(Trim(cur));
    return parts;
}

// Splits a multi-statement expression (separated by ';') and emits each
// statement on its own line.  'if' statements without braces are split so
// that the condition and body appear on separate lines (Allman-compatible).
static void EmitStatements(std::ostream& out, const std::string& indent, const std::string& expr)
{
    // Split by ';'
    std::vector<std::string> parts;
    std::istringstream iss(expr);
    std::string part;
    while (std::getline(iss, part, ';'))
    {
        auto trimmed = Trim(part);
        if (!trimmed.empty())
            parts.push_back(trimmed);
    }

    for (auto const& stmt : parts)
    {
        // Detect "if (...) body" pattern — split condition and body onto separate lines.
        if (stmt.starts_with("if ") || stmt.starts_with("if("))
        {
            if (auto openParen = stmt.find('('); openParen != std::string::npos)
            {
                int depth = 0;
                size_t closeParen = std::string::npos;
                for (size_t i = openParen; i < stmt.size(); ++i)
                {
                    if (stmt[i] == '(')
                        ++depth;
                    else if (stmt[i] == ')')
                    {
                        --depth;
                        if (depth == 0)
                        {
                            closeParen = i;
                            break;
                        }
                    }
                    else
                    {
                        // No additional handling required.
                    }
                }

                if (closeParen != std::string::npos && closeParen + 1 < stmt.size())
                {
                    auto condition = stmt.substr(0, closeParen + 1);
                    auto body = Trim(stmt.substr(closeParen + 1));
                    out << indent << condition << "\n";
                    out << indent << "    " << body << ";\n";
                    continue;
                }
            }
        }

        out << indent << stmt << ";\n";
    }
}

// ─── Metadata Parser ───────────────────────────────────────────────────────────

// Parses key="value" pairs from OLO_PROPERTY(...) content.
// Handles nested quotes and skips whitespace.
static std::map<std::string, std::string> ParseMetadata(const std::string& content)
{
    std::map<std::string, std::string> result;
    size_t i = 0;
    while (i < content.size())
    {
        // Skip whitespace and commas
        while (i < content.size() && (content[i] == ' ' || content[i] == '\t' || content[i] == ','))
            ++i;
        if (i >= content.size())
            break;

        // Read key
        auto keyStart = i;
        while (i < content.size() && content[i] != '=' && content[i] != ' ' && content[i] != ',')
            ++i;
        std::string key = Trim(content.substr(keyStart, i - keyStart));

        // Skip whitespace and '='
        while (i < content.size() && (content[i] == ' ' || content[i] == '='))
            ++i;
        if (i >= content.size())
            break;

        // Read value — either quoted string or unquoted token
        std::string value;
        if (content[i] == '"')
        {
            ++i; // skip opening quote
            auto valStart = i;
            while (i < content.size() && content[i] != '"')
                ++i;
            value = content.substr(valStart, i - valStart);
            if (i < content.size())
                ++i; // skip closing quote
        }
        else
        {
            auto valStart = i;
            while (i < content.size() && content[i] != ',' && content[i] != ' ')
                ++i;
            value = Trim(content.substr(valStart, i - valStart));
        }

        if (!key.empty())
            result[key] = value;
    }
    return result;
}

// ─── OLO_SERIALIZE(...) marker helpers ───────────────────────────────────────

// If `s` begins with an `OLO_SERIALIZE(...)` marker (optionally whitespace before
// the '('), peel it: `args` receives the text inside the outer parens (no parens)
// and `rest` the leading-trimmed remainder after the closing ')', and returns
// true. Otherwise returns false and leaves `args`/`rest` untouched. Shared by the
// OLO_PROPERTY scanner (ParseHeaders — to peel a marker glued onto a field's
// anchor line) and the scene-serializer field scan (ParseComponentFields — to
// honour Skip), so both treat the glued and own-line forms identically. An
// unterminated marker (no matching ')') yields an empty `rest` so the field is
// safely skipped rather than mis-parsed; such source fails to compile anyway.
static bool PeelSerializeMarker(const std::string& s, std::string& args, std::string& rest)
{
    static const std::string kMarker = "OLO_SERIALIZE";
    if (s.rfind(kMarker, 0) != 0)
        return false;
    size_t j = kMarker.size();
    while (j < s.size() && (s[j] == ' ' || s[j] == '\t'))
        ++j;
    if (j >= s.size() || s[j] != '(')
        return false; // a bare identifier starting with OLO_SERIALIZE, not a call

    int depth = 0;
    size_t closeParen = std::string::npos;
    for (size_t k = j; k < s.size(); ++k)
    {
        if (s[k] == '(')
            ++depth;
        else if (s[k] == ')' && --depth == 0)
        {
            closeParen = k;
            break;
        }
    }
    if (closeParen == std::string::npos)
    {
        args = s.substr(j + 1);
        rest.clear();
        return true;
    }
    args = s.substr(j + 1, closeParen - (j + 1));
    rest = Trim(s.substr(closeParen + 1));
    return true;
}

// True iff an OLO_SERIALIZE argument list contains a bare `Skip` token, or
// `Skip = true`. Whole-word / whole-argument match: comma-split the args and
// compare the key (the part before any '=') exactly, so `NoSkip` / `SkipCount`
// do NOT count and a genuine `Skip` after another argument is still found;
// `Skip = false` is not a skip. `args` is the text inside the parens.
static bool SerializeArgsHaveSkip(const std::string& args)
{
    for (size_t start = 0; start <= args.size();)
    {
        size_t comma = args.find(',', start);
        const size_t end = (comma == std::string::npos) ? args.size() : comma;
        std::string part = Trim(args.substr(start, end - start));

        std::string key = part;
        std::string value;
        if (auto eq = part.find('='); eq != std::string::npos)
        {
            key = Trim(part.substr(0, eq));
            value = Trim(part.substr(eq + 1));
        }
        if (key == "Skip")
            return value.empty() || value.rfind("false", 0) != 0;

        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return false;
}

// True iff an OLO_SERIALIZE argument list contains a bare `Clamp` token (issue
// #451's Clamp slice). `minOut`/`maxOut` receive the raw text of `Min=`/`Max=`
// arguments when present (unset otherwise) — the caller casts them to the field's
// own type at emit time, so `Min=0` is fine on a float field. Same whole-argument
// comma-split as SerializeArgsHaveSkip (a bare key or a key=value pair per
// argument); unrecognised keys are ignored here (this call only extracts Clamp's
// own arguments — Skip is checked separately and the two are mutually exclusive by
// construction, since Skip drops the field before this is consulted).
static bool SerializeArgsClampBounds(const std::string& args, std::optional<std::string>& minOut,
                                     std::optional<std::string>& maxOut)
{
    bool hasClamp = false;
    minOut.reset();
    maxOut.reset();
    for (size_t start = 0; start <= args.size();)
    {
        size_t comma = args.find(',', start);
        const size_t end = (comma == std::string::npos) ? args.size() : comma;
        std::string part = Trim(args.substr(start, end - start));
        if (!part.empty())
        {
            std::string key = part;
            std::string value;
            if (auto eq = part.find('='); eq != std::string::npos)
            {
                key = Trim(part.substr(0, eq));
                value = Trim(part.substr(eq + 1));
            }
            if (key == "Clamp")
                hasClamp = true;
            else if (key == "Min")
                minOut = value;
            else if (key == "Max")
                maxOut = value;
        }
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return hasClamp;
}

// ─── Header Parser ─────────────────────────────────────────────────────────────

static std::vector<ComponentDef> ParseHeaders(const fs::path& scanDir)
{
    std::vector<ComponentDef> components;

    for (auto const& entry : fs::recursive_directory_iterator(scanDir))
    {
        if (!entry.is_regular_file())
            continue;
        if (auto ext = entry.path().extension().string(); ext != ".h" && ext != ".hpp")
            continue;

        std::ifstream file(entry.path());
        if (!file.is_open())
            continue;

        // Quick check: does this file contain OLO_PROPERTY?
        std::string fileContent((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
        if (fileContent.find("OLO_PROPERTY") == std::string::npos)
            continue;

        // Parse line by line
        std::istringstream stream(fileContent);
        std::string line;
        std::string currentComponent;
        int braceDepth = 0;
        bool insideStruct = false;
        std::vector<std::string> pendingMetadataList;

        // Track struct names paired with their opening brace depth for nesting
        std::vector<std::pair<std::string, int>> structStack;

        // Map component name → index in components vector
        std::map<std::string, size_t> compMap;

        bool inBlockComment = false;

        while (std::getline(stream, line))
        {
            std::string trimmed = Trim(line);

            // Strip block comments and track state across lines.
            trimmed = Trim(StripBlockComments(trimmed, inBlockComment));
            if (trimmed.empty() && inBlockComment)
                continue;

            // Track struct/class declarations
            // Match: "struct Name" or "struct Name {"  (skip forward declarations with ;)
            if ((trimmed.starts_with("struct ") || trimmed.starts_with("class ")) &&
                trimmed.find(';') == std::string::npos)
            {
                // Extract name: "struct Foo" or "struct Foo {" or "struct Foo : Base {"
                auto nameStart = trimmed.find(' ') + 1;
                auto nameEnd = trimmed.find_first_of(" :{", nameStart);
                if (nameEnd == std::string::npos)
                    nameEnd = trimmed.size();
                std::string name = Trim(trimmed.substr(nameStart, nameEnd - nameStart));

                if (!name.empty() && name.find('(') == std::string::npos)
                {
                    // Check if this line also opens a brace
                    if (trimmed.find('{') != std::string::npos)
                    {
                        // Don't increment braceDepth here — the generic
                        // brace-tracking loop below will count it.
                        structStack.emplace_back(name, braceDepth);
                        currentComponent = name;
                        insideStruct = true;
                    }
                    else
                    {
                        // Next line might have the brace — mark pending
                        structStack.emplace_back(name, braceDepth);
                        currentComponent = name;
                        insideStruct = false; // Will become true when we see {
                    }
                }
            }

            // Track braces
            for (char c : trimmed)
            {
                if (c == '{')
                {
                    ++braceDepth;
                    if (!currentComponent.empty() && !insideStruct)
                        insideStruct = true;
                }
                else if (c == '}')
                {
                    --braceDepth;
                    // Pop all structs whose opening depth >= current braceDepth
                    while (!structStack.empty() && structStack.back().second >= braceDepth)
                    {
                        structStack.pop_back();
                    }
                    currentComponent = structStack.empty() ? "" : structStack.back().first;
                    insideStruct = !structStack.empty();
                    braceDepth = std::max(braceDepth, 0);
                }
                else
                {
                    // No additional handling required.
                }
            }

            // Check for OLO_PROPERTY(...) — skip if inside a line comment
            if (auto propPos = trimmed.find("OLO_PROPERTY("); propPos != std::string::npos)
            {
                if (auto commentPos = trimmed.find("//"); commentPos != std::string::npos && commentPos < propPos)
                {
                    continue; // OLO_PROPERTY is inside a comment — ignore
                }
                // Extract the content between OLO_PROPERTY( and )
                auto parenStart = propPos + 13; // length of "OLO_PROPERTY("
                // Find matching closing paren (handle nested parens, multi-line)
                std::string content = trimmed.substr(parenStart);
                int depth = 1;
                size_t scanPos = 0;
                bool streamExhausted = false;
                while (depth > 0)
                {
                    while (scanPos < content.size() && depth > 0)
                    {
                        if (content[scanPos] == '(')
                            ++depth;
                        else if (content[scanPos] == ')')
                            --depth;
                        else
                        { /* No additional handling required. */
                        }
                        if (depth > 0)
                            ++scanPos;
                    }
                    if (depth > 0)
                    {
                        // Closing paren on a subsequent line — keep reading
                        std::string nextLine;
                        if (!std::getline(stream, nextLine))
                        {
                            streamExhausted = true;
                            break;
                        }
                        // Strip block comments from continuation line
                        std::string cleaned = Trim(StripBlockComments(Trim(nextLine), inBlockComment));
                        content += ' ';
                        content += cleaned;
                    }
                }
                if (depth > 0)
                {
                    std::cerr << "WARNING: Unterminated OLO_PROPERTY( in "
                              << entry.path().filename().string() << "\n";
                    if (streamExhausted)
                        break; // Exit the outer file-reading loop
                    continue;
                }
                pendingMetadataList.push_back(content.substr(0, scanPos));
                continue; // The NEXT line has the field declaration (or another OLO_PROPERTY)
            }

            // Process pending OLO_PROPERTY(s) — parse the field declaration
            if (!pendingMetadataList.empty())
            {

                if (currentComponent.empty())
                {
                    std::cerr << "WARNING: OLO_PROPERTY found outside struct in "
                              << entry.path().filename().string() << "\n";
                    pendingMetadataList.clear();
                    continue;
                }

                // Skip access specifiers, comments, blank lines — retry on next line.
                if (trimmed.starts_with("//") || trimmed.starts_with("/*") ||
                    trimmed.starts_with("private:") || trimmed.starts_with("public:") ||
                    trimmed.starts_with("protected:") || trimmed.empty())
                {
                    continue; // pendingMetadataList stays populated for next line
                }

                // Peel a leading OLO_SERIALIZE(...) scene-serializer directive off the
                // anchor line. It is not a script binding, so it must not be mistaken for
                // the field anchor (which would drop the pending OLO_PROPERTY and re-bind
                // it to a later field). On its own line the remainder is empty → retry on
                // the next line. Glued to the field (`OLO_SERIALIZE(Skip) f32 m_X = 1;`)
                // the remainder IS the field declaration — parse it as the anchor, so the
                // marker/field on one line works exactly as the scene-serializer scan
                // (ParseComponentFields) already handles it.
                if (std::string sArgs, sRest; PeelSerializeMarker(trimmed, sArgs, sRest))
                {
                    if (sRest.empty())
                        continue; // marker alone on its line — pending stays for next line
                    trimmed = sRest;
                }

                // Parse field type and name from anchor line, e.g.:
                //   "glm::vec3 m_Color = { 1.0f, 1.0f, 1.0f };"
                //   "f32 m_Intensity = 1.0f;"
                //   "SceneCamera Camera;"
                std::string fieldType;
                std::string fieldName;
                {
                    std::istringstream tokens(trimmed);
                    tokens >> fieldType;
                    tokens >> fieldName;
                    auto cutPos = fieldName.find_first_of("={;");
                    if (cutPos != std::string::npos)
                        fieldName.resize(cutPos);
                }

                if (fieldName.empty())
                {
                    pendingMetadataList.clear();
                    continue;
                }

                // Process each stacked OLO_PROPERTY against this field line
                for (auto const& pendingMeta : pendingMetadataList)
                {
                    auto meta = ParseMetadata(pendingMeta);

                    // Honour Skip metadata — useful for fields that should not
                    // be exposed to scripts.
                    if (auto it = meta.find("Skip"); it != meta.end() && it->second != "false")
                        continue;

                    PropertyDef prop;
                    prop.cppField = fieldName;

                    // Determine script name: metadata Name, or strip m_ prefix
                    if (auto it = meta.find("Name"); it != meta.end())
                        prop.scriptName = it->second;
                    else
                        prop.scriptName = StripPrefix(fieldName, "m_");

                    // Determine type: metadata Type override, or derive from C++ type
                    if (auto it = meta.find("Type"); it != meta.end())
                        prop.type = StringToPropType(it->second);
                    else
                        prop.type = CppTypeToPropType(fieldType);

                    if (prop.type == PropType::Unknown)
                    {
                        if (meta.find("Get") == meta.end())
                            continue;
                        std::cerr << "WARNING: OLO_PROPERTY on " << currentComponent
                                  << "::" << fieldName
                                  << " has custom Get but no Type override\n";
                        continue;
                    }

                    // Custom getter/setter — full expressions using 'comp'
                    if (auto it = meta.find("Get"); it != meta.end())
                        prop.customGet = it->second;
                    if (auto it = meta.find("Set"); it != meta.end())
                        prop.customSet = it->second;

                    // Find or create ComponentDef
                    auto [it, inserted] = compMap.try_emplace(currentComponent, components.size());
                    if (inserted)
                    {
                        components.push_back({ currentComponent,
                                               entry.path().filename().string(),
                                               {} });
                    }
                    components[it->second].properties.push_back(prop);
                }
                pendingMetadataList.clear();
            }
        }
    }

    // Sort by component name for deterministic output
    std::ranges::sort(components,
                      [](const ComponentDef& a, const ComponentDef& b)
                      { return a.name < b.name; });

    return components;
}

// ─── Component Struct Collector (for the AllComponents tuple) ─────────────────────

// Components intentionally kept OUT of the AllComponents tuple. This MUST mirror
// the `kNotInTuple` set in OloEngine/tests/ComponentTupleCoverageTest.cpp — that
// test guards the generated tuple in both directions, but it cannot catch a
// runtime-only component that the generator wrongly *adds* to the tuple, so these
// two lists are the shared source of truth and have to be kept in sync. See the
// test for the per-entry rationale:
//   * IDComponent / TagComponent — entity identity, copied by hand in Scene.cpp.
//   * *StateComponent / UIResolvedRectComponent — per-tick runtime-derived state,
//     recomputed each frame and never copied / serialized / script-registered.
static const std::set<std::string> kComponentsNotInTuple = {
    "IDComponent",
    "TagComponent",
    "UIResolvedRectComponent",
    "DialogueStateComponent",
    "SpringBoneStateComponent",
    "NoiseAnimationStateComponent",
    "RetargetingStateComponent",
    "FootIKStateComponent",
    "LocomotionStateComponent",
    "WorldTransformComponent",
};

// Components intentionally kept OUT of the save-game capture/restore lists
// (the SAVE_COMPONENT / TRY_LOAD_COMPONENT enumerations in SaveGameSerializer.cpp).
// This is DELIBERATELY NOT the same set as kComponentsNotInTuple — save-games and
// the scene-copy tuple have different membership:
//   * IDComponent / TagComponent — excluded from the tuple (hand-copied identity)
//     but ARE persisted by save-games, so they are NOT in this set.
//   * The four per-tick runtime *StateComponent / UIResolvedRectComponent are
//     excluded from BOTH (recomputed each frame, never serialized).
//   * AudioSoundGraphComponent / LocalizedTextComponent ARE in the tuple (they
//     round-trip through scene YAML) but have no SaveGameComponentSerializer::
//     Serialize() overload yet, so they cannot appear in the SAVE_COMPONENT list
//     (it would fail to compile) — excluded here until a serializer is added.
// A component is in this set iff it has no save serializer. Mirror this with the
// REGISTER_SAVE_COMPONENT list in SaveGameComponentSerializer.cpp; the guard test
// SaveGameComponentSerializerCoverageTest fails if the generated lists and the
// registered serializers drift apart. To make a listed component saveable, add a
// Serialize() overload + REGISTER_SAVE_COMPONENT and remove it from this set.
static const std::set<std::string> kComponentsNotInSaveGame = {
    "UIResolvedRectComponent",
    "DialogueStateComponent",
    "SpringBoneStateComponent",
    "NoiseAnimationStateComponent",
    "RetargetingStateComponent",
    "FootIKStateComponent",
    "LocomotionStateComponent",
    "WorldTransformComponent",
    "AudioSoundGraphComponent",
    "LocalizedTextComponent",
};

// Components whose Scene::OnComponentAdded<T> specialization is HAND-WRITTEN in
// Scene.cpp — either it does real init work, or it is an intentionally-empty
// exception whose inline comment documents why (and must not be regenerated).
// Every OTHER `struct *Component` gets a generated no-op
// `template<> void Scene::OnComponentAdded<T>(Entity, T&) {}`.
//
// DELIBERATELY a THIRD, distinct exclusion set — unrelated to scene-copy tuple
// membership (kComponentsNotInTuple) or save serialization (kComponentsNotInSaveGame).
// It even differs from the remove set below: CameraComponent / CinematicComponent /
// LocalizedTextComponent / AudioSoundGraphComponent do real work on ADD but their
// removal is a plain no-op, while Rigidbody2D / SpringBone / NoiseAnimation are the
// reverse (no-op add, real teardown on remove).
//
// Self-checking against drift: the OnComponentAdded primary template is
// declaration-only, so a component listed here but lacking a hand-written
// specialization is an engine LINK error, and a component with a hand-written
// body NOT listed here is a DUPLICATE-DEFINITION compile error against the
// generated no-op. ComponentHandlerCoverageTest also guards the generated list.
// NOTE: `Skeleton` is intentionally absent — it is not a `struct *Component`, so
// the scan never emits it and its no-op specialization stays hand-written.
static const std::set<std::string> kComponentsCustomOnAdd = {
    "CameraComponent",
    "LocalizedTextComponent",
    "CinematicComponent",
    "AudioSoundGraphComponent",
    "VideoOverlayComponent",
    "VideoSurfaceComponent",
    "Rigidbody3DComponent",
    "PhysicsJoint3DComponent",
    "VehicleComponent",
    "RagdollComponent",
    "CharacterController3DComponent",
};

// Components whose Scene::OnComponentRemoved<T> specialization is HAND-WRITTEN in
// Scene.cpp because removal must release an external resource (a Jolt body /
// constraint / vehicle / ragdoll / character-controller, a Box2D body, an audio
// SoundGraph source, a video decode thread, a DetourCrowd agent slot) or drop
// cached runtime state (the SpringBone / NoiseAnimation state component). Every
// OTHER `struct *Component` gets a generated no-op `OLO_ON_COMPONENT_REMOVED_NOOP(T)`.
//
// See kComponentsCustomOnAdd for why this is its own set and how it self-checks
// (a missing specialization fails the OloEditor link via RemoveComponent<T>; a
// stray hand-written body collides with the generated no-op). `Skeleton` is again
// intentionally absent — not a `struct *Component`, so its no-op stays hand-written.
static const std::set<std::string> kComponentsCustomOnRemove = {
    "Rigidbody2DComponent",
    "Rigidbody3DComponent",
    "PhysicsJoint3DComponent",
    "VehicleComponent",
    "RagdollComponent",
    "CharacterController3DComponent",
    "AudioSoundGraphComponent",
    "VideoOverlayComponent",
    "VideoSurfaceComponent",
    "SpringBoneComponent",
    "NoiseAnimationComponent",
    "RetargetingComponent",
    "FootIKComponent",
    "LocomotionComponent",
    "TerrainComponent",
    "NavAgentComponent",
};

// Components that ARE all-trivial-fields (every data member is a primitive /
// small-int / glm::vec*/ivec*/quat/mat* / std::string / AssetHandle / UUID / enum /
// std::vector<one-of-those> — see SceneSerType, the CollectEnumTypes-driven enum
// check, and the std::vector handling in ParseComponentFields) yet are deliberately
// kept HAND-WRITTEN in SceneSerializer.cpp rather than auto-generated.
// The scene serialize/deserialize codegen (issue #380; AssetHandle/UUID added by
// the #451 first slice, enums by the #451 enum slice, glm-math + small-int +
// std::vector<primitive> by the #451 glm/vector slice, an all-trivial nested
// struct / std::vector<struct> — recursively — by the #451 nested-struct slice,
// Ref<T> of an Asset-derived T by the #451 Ref<T> slice, and a sortable-scalar
// std::unordered_set / a std::string-keyed std::unordered_map by the #451
// unordered_map/set slice) emits a block for every all-trivial component EXCEPT
// these; anything with a still-unhandled non-trivial field (Ref<T> of a
// non-asset type, std::array, a non-string-keyed map, a
// std::vector<Ref<T>> / vector-of-non-trivial-struct, or a non-public data member) is
// classified non-trivial by the parser and skipped automatically without an entry here.
//
// Each entry is a trivial component whose hand-written block does something the
// plain round-trip generator must NOT silently drop:
//   * SphereAreaLightComponent — REJECT-not-clamp semantics (keeps the constructor
//     default rather than clamping an out-of-bounds value), a different semantic
//     from OLO_SERIALIZE(Clamp) — see ComponentReflection.h's Clamp doc comment.
//   * DialogueComponent / PerceptionComponent / IKTargetComponent — deserialize
//     clamps / Sanitize()s float (or vector) ranges beyond what a per-field Clamp
//     annotation alone expresses; auto-generating would relax those guards.
//     (PerceptionComponent also intentionally does NOT restore its runtime-derived
//     fields — HasVisibleTarget / VisibleTarget / LastKnownPosition / … — on load.)
//     BuoyancyComponent / NoiseAnimationComponent / SpringBoneComponent /
//     SnowDeformerComponent / FogVolumeComponent / NavAgentComponent MIGRATED off
//     this set onto OLO_SERIALIZE(Clamp) — the last two, BuoyancyComponent and
//     NoiseAnimationComponent, by the vec3-Clamp follow-up slice (their
//     SanitizeVec3Clamped-equivalent bounds are now per-field
//     OLO_SERIALIZE(Clamp, Min=…, Max=…) annotations on the glm::vec3 members).
//   * ScriptComponent — serializes the C# ScriptField map owned by ScriptEngine,
//     not just its ClassName member (the parser only sees ClassName).
//   * VehicleComponent — has a runtime-only RuntimeVehicleToken field the
//     hand-written serializer deliberately omits (auto-gen would persist it).
//   * Rigidbody3DComponent — (a) its enum is keyed "BodyType" not "Type" (the
//     m_-stripped default), so on-disk compatibility needs the hand-written key;
//     (b) the hand-written serializer deliberately omits m_LayerID, m_LockedAxes,
//     the initial/max velocities, and the runtime-only m_RuntimeBodyToken — auto-
//     gen would persist all of them (the runtime token included).
//   * StreamingVolumeComponent — (a) the runtime-only `IsLoaded` bool is omitted by
//     the hand-written serializer (auto-gen would persist it); (b) deserialize range-
//     clamps ActivationMode and the load/unload radii.
//   * FogVolumeComponent — deserialize delegates to a hand-written helper that
//     clamps the Shape enum (0..2), Priority (-100..100), and Sanitize()s extents /
//     color / density / falloff / blend-weight; auto-gen would relax those guards.
//   * UIButtonComponent / UISliderComponent — MIGRATED off this set in the #451
//     OLO_SERIALIZE(Skip) slice: their only reason to stay hand-written was a single
//     runtime-only field (m_State / m_IsDragging), now marked OLO_SERIALIZE(Skip) so
//     the generated block omits it while round-tripping every authored field. Left
//     here as a breadcrumb for the migration pattern; both are fully generated now.
//   * LightProbeVolumeComponent — (a) the runtime-only `m_Dirty` (set by property
//     setters, cleared by the bake) and editor-only `m_ShowDebugProbes` are omitted
//     by the hand-written serializer (now that glm::ivec3 m_Resolution is a
//     SceneSerType the parser sees it as all-trivial); (b) deserialize Sanitize()s
//     Spacing / Intensity, clamps Resolution to >= 1, and orders the bounds — auto-
//     gen would persist the runtime fields and relax those guards. (#451 glm slice.)
//   * NavAgentComponent — the hand-written serializer persists only the 7 authored
//     agent params (Radius … LockYAxis); m_TargetPosition / m_HasTarget / m_HasPath /
//     m_PathCorners / m_CurrentCornerIndex / m_CrowdAgentId are runtime pathfinding
//     state. Now that std::vector<glm::vec3> m_PathCorners is serializable the parser
//     sees it as all-trivial, so auto-gen would persist all the runtime state.
//     (#451 vector slice.)
//   * PhysicsJoint3DComponent — has a runtime-only `m_RuntimeConstraintToken` the
//     hand-written serializer omits, and its deserialize clamps/Sanitize()s dozens of
//     joint limits/spring params. Now that its enum + std::vector<glm::vec3> path
//     fields are serializable the parser sees it as all-trivial; auto-gen would
//     persist the runtime token and relax every clamp. (#451 glm/vector slice.)
//   * TagComponent — entity identity, hand-copied; not a normal sub-map.
//   * IDComponent — entity identity: its UUID is serialized once as the top-level
//     `Entity: <uuid>` line and re-applied via CreateEntityWithUUID on load, never
//     as a component sub-map. Now that UUID is a SceneSerType the parser sees it as
//     all-trivial, so it MUST be excluded here — auto-generating a block would emit
//     a bogus IDComponent sub-map AND call AddComponent<IDComponent> on an entity
//     that already has one (an EnTT double-add). The coverage test's kRuntimeOnly
//     set hides it from the existence check but does NOT guard this, so the
//     exclusion is the only safety net.
//   * PhaseComponent — animation phase runtime state (kRuntimeOnly in the
//     coverage test); its persistence is intentionally hand-managed.
//   * UIResolvedRectComponent — per-tick layout-resolved rect, never serialized.
//   * LODGroupComponent — the #451 nested-struct slice made its `LODGroup m_LODGroup`
//     member (a nested struct wrapping a std::vector<LODLevel> + Bias) serializable,
//     so the parser now sees the whole component as all-trivial. But the hand-written
//     serializer (a) FLATTENS the sub-struct — it emits top-level `Bias` / `Levels`
//     keys, not a nested `LODGroup:` sub-map, so auto-gen changes the on-disk format;
//     and (b) omits the runtime-baked `m_GeneratedLODHandles` vector that auto-gen
//     would persist. Kept hand-written for on-disk compatibility. (#451 nested slice.)
//   * NavMeshBoundsComponent — the #451 nested-struct slice made its
//     `std::vector<OffMeshLink> m_Links` (a vector of an all-trivial struct)
//     serializable → the parser sees it all-trivial. But deserialize SanitizeVec3()s
//     Min/Max, orders Min<=Max per axis, and drops links with non-finite endpoints /
//     clamps each link Radius — guards auto-gen would relax. Kept hand-written.
//     (#451 nested/vector-of-struct slice.)
//   * DialogueStateComponent — active dialogue progression (kRuntimeOnly in the
//     coverage test). The #451 nested-struct slice made its
//     `std::vector<DialogueChoice> m_AvailableChoices` serializable, flipping it to
//     all-trivial; it MUST be excluded so this per-tick runtime state is not written
//     into scene files (same blind-spot as IDComponent — it has no hand-written block,
//     so the disjointness test can't catch an unwanted auto-generation).
//   * NoiseAnimationStateComponent / SpringBoneStateComponent — per-tick runtime
//     simulation state (a `*StateComponent`; excluded from the AllComponents tuple as
//     runtime-derived). The #451 nested-struct slice made their nested `State` member
//     (Animation::NoiseAnimationState / Animation::SpringBoneState — the latter carries
//     std::vector<glm::vec3> position buffers) serializable, flipping them all-trivial.
//     Excluded so the runtime state is never persisted; like DialogueStateComponent
//     they have no hand-written block, so nothing else guards against the drift.
//   * WorldTransformComponent — the composed parent-chain world matrix written every
//     tick by Scene::PropagateWorldTransforms() (issue #499). A single glm::mat4 is
//     all-trivial, so it MUST be excluded here (same no-hand-written-block blind spot
//     as DialogueStateComponent) or auto-gen would persist a purely-derived value that
//     gets recomputed from TransformComponent + RelationshipComponent next frame anyway.
//
// The #451 Ref<T> slice (CollectAssetTypes) flipped 16 components whose only
// remaining obstacle was a Ref<T> field to all-trivial. Every one is kept
// hand-written, for one of three reasons — none is a clean auto-gen migration
// (unlike MeshComponent's Ref<MeshSource>, which turned out NOT to be clean either,
// see below):
//   * ON-DISK FORMAT INCOMPATIBILITY — the hand-written block persists the asset by
//     FILE PATH (a "*Path" string key resolved via LoadSceneTexture / Font::Create),
//     not by AssetManager handle, so auto-gen's "<Key>Handle" u64 key would silently
//     stop round-tripping existing scenes' texture/font references: DecalComponent
//     (AlbedoTexturePath/NormalTexturePath/RMATexturePath/EmissiveTexturePath, plus
//     Sanitize/clamp on Color/Size/FadeDistance/NormalAngleThreshold),
//     SpriteRendererComponent (TexturePath), TextComponent (FontPath),
//     UIImageComponent (TexturePath), UIPanelComponent (BackgroundTexturePath),
//     UITextComponent / UIInputFieldComponent / UIDropdownComponent (FontPath).
//   * DROPPED CUSTOM LOAD LOGIC — the hand-written deserialize does something
//     auto-gen's plain field-by-field read cannot express: ModelComponent calls
//     `mc.Reload()` from `m_FilePath` when present (auto-gen would leave m_Model
//     null); MeshComponent range-validates `Primitive` (falls back to `None` +
//     warns on an out-of-range value) AND reconstructs `m_MeshSource` from the
//     primitive when no explicit mesh asset was authored (auto-gen would silently
//     leave a primitive-only mesh with a null MeshSource); SubmeshComponent's
//     `m_Mesh` is explicitly NOT persisted — reconstructed from the parent
//     MeshComponent at runtime (a hand-written comment says so); ProceduralSkyComponent
//     / StarNestSkyComponent / ReflectionProbeComponent deserialize with per-field
//     positivity/range guards (turbidity > 0, resolution clamped to a sane range,
//     iterations/volSteps bounds, …) well beyond plain finiteness.
//   * RUNTIME-ONLY FIELDS AUTO-GEN WOULD NEWLY PERSIST — CinematicComponent's
//     `RuntimeSequence` Ref plus `Playing`/`Time`/`PreviousTime`/`Finished`/
//     `EventsFiredThisFrame` are explicitly "never serialized" runtime playback
//     state (per the component's own header comment); ReflectionProbeComponent's
//     `m_BakedEnvironment` likewise (baked at runtime, not persisted — the header
//     comment on m_NeedsBake says so) and its deserialize forces `m_NeedsBake = true`
//     unconditionally regardless of what a plain round-trip would read back.
// None of the 16 is a false positive needing correction here — see each component's
// own header/serializer comments for confirmation. A future slice could migrate any
// of these off this set with OLO_SERIALIZE(Skip) on the runtime fields (mirroring how
// UIButtonComponent/UISliderComponent migrated off in the earlier Skip slice) or, for
// the format-incompatible ones, by first migrating their on-disk format to the
// handle-based scheme in a dedicated slice.
//
// DISJOINTNESS is guarded by ComponentSerializerCoverageTest: a component must be
// handled by EXACTLY ONE of the generated .inl or the hand-written serializer —
// listing one here while ALSO leaving (or removing) its hand-written block is a
// loud test failure, never a silent double-emit / drop.
static const std::set<std::string> kComponentsCustomSerialize = {
    "CinematicComponent",
    "DecalComponent",
    "DialogueComponent",
    "DialogueStateComponent",
    "IDComponent",
    "IKTargetComponent",
    // InstancedMeshComponent was NON-trivial (its runtime MergedCache sub-object was an
    // unrecognised type) until that field was tagged OLO_SERIALIZE(Skip) for the MCP
    // sub-object slice, which removed the last blocker and flipped it trivial. Its
    // hand-written block stays: it must keep the exact on-disk shape existing scenes
    // (InstancingDemo.olo) were saved with. Without this entry the component is emitted
    // AND hand-written — a double AddComponent on load, which is an entt "Slot not
    // available" abort, caught by ComponentSerializerCoverage.NoComponentIsBothHandWrittenAndGenerated.
    "InstancedMeshComponent",
    "LODGroupComponent",
    "LightProbeVolumeComponent",
    "MeshComponent",
    "ModelComponent",
    "NavMeshBoundsComponent",
    "NoiseAnimationStateComponent",
    "PerceptionComponent",
    "PhaseComponent",
    "PhysicsJoint3DComponent",
    "FootIKStateComponent",
    "LocomotionStateComponent",
    "ProceduralSkyComponent",
    "ReflectionProbeComponent",
    "RetargetingStateComponent",
    "Rigidbody3DComponent",
    "ScriptComponent",
    "SphereAreaLightComponent",
    "SpriteRendererComponent",
    "SpringBoneStateComponent",
    "StarNestSkyComponent",
    "StreamingVolumeComponent",
    "SubmeshComponent",
    "TagComponent",
    "TextComponent",
    "UIDropdownComponent",
    "UIImageComponent",
    "UIInputFieldComponent",
    "UIPanelComponent",
    "UIResolvedRectComponent",
    "UITextComponent",
    "VehicleComponent",
    "WorldTransformComponent",
};

// Collect the name of every `struct *Component` *definition* under the scan dir.
// This is the input to the generated AllComponents tuple — independent of the
// OLO_PROPERTY scan above, since most components have no scripting properties.
//
// Discrimination rules are kept deliberately close to ComponentTupleCoverageTest
// (which guards the output) so the two agree:
//   * definitions only — a `struct Foo;` forward declaration (the line contains
//     a ';') is skipped; only the brace-opening definition counts.
//   * line- and block-comments are stripped first, so a `// struct FooComponent`
//     mention or a commented-out struct is never collected.
//   * the type name must end in "Component".
static std::set<std::string> CollectComponentStructs(const fs::path& scanDir)
{
    std::set<std::string> names;

    for (auto const& entry : fs::recursive_directory_iterator(scanDir))
    {
        if (!entry.is_regular_file())
            continue;
        if (auto ext = entry.path().extension().string(); ext != ".h" && ext != ".hpp")
            continue;

        std::ifstream file(entry.path());
        if (!file.is_open())
            continue;

        bool inBlockComment = false;
        std::string line;
        while (std::getline(file, line))
        {
            std::string trimmed = Trim(StripBlockComments(Trim(line), inBlockComment));
            if (trimmed.empty())
                continue;
            // Whole-line comment — ignore (block comments already stripped above).
            if (trimmed.starts_with("//"))
                continue;
            if (!trimmed.starts_with("struct ") && !trimmed.starts_with("class "))
                continue;
            // Skip forward declarations / semicolon-only declarations (`struct
            // Foo;`) — they have a ';' and never open a body. A definition that
            // opens a body ('{') on this line is kept even when it's a one-liner
            // like `struct MarkerComponent {};` (a valid empty/tag component); a
            // multi-line definition opens its body on a later line, so the line
            // here has neither ';' nor '{' and is kept too. The skip therefore
            // fires only for a ';' with no brace on the same line.
            if (trimmed.find('{') == std::string::npos && trimmed.find(';') != std::string::npos)
                continue;

            // The type name is the identifier just before the base-class list or
            // body. Scan the tokens between the keyword and the first ':' or '{'
            // and take the one ending in "Component". Picking by suffix (rather
            // than grabbing the first token after "struct") tolerates leading
            // specifiers/attributes — `struct alignas(16) FooComponent`,
            // `struct [[deprecated]] BarComponent` — which would otherwise be
            // *silently* dropped from the tuple. Requiring a non-empty prefix
            // before "Component" matches the guard test's `\w+Component` regex
            // ("Component" alone or "ComponentGroup" won't pass).
            constexpr std::string_view kSuffix = "Component";
            auto bodyPos = trimmed.find_first_of(":{");
            std::string head = (bodyPos == std::string::npos) ? trimmed : trimmed.substr(0, bodyPos);
            std::istringstream tokenStream(head);
            std::string token;
            std::string name;
            while (tokenStream >> token)
            {
                if (token.size() > kSuffix.size() && token.ends_with(kSuffix))
                    name = token;
            }
            if (!name.empty())
                names.insert(name);
        }
    }

    return names;
}

// ─── Component Field Collector (for SceneSerializer serialize/deserialize codegen) ─

// A single auto-serializable data member of a component.
struct SerField
{
    std::string member; // C++ member name, e.g. "m_Intensity" or "Color"
    std::string key;    // YAML key, e.g. "Intensity" / "Color" (member minus m_)
    PropType type{ PropType::Unknown };
    // When true the member is a std::vector<E> serialized as a YAML sequence; `type`
    // holds the ELEMENT PropType E (which must itself be a trivial serializer type).
    // For a std::vector<struct>, type == PropType::Struct and subFields holds the
    // element struct's fields.
    bool isVector{ false };
    // When true the member is a std::unordered_set<E> serialized as a YAML
    // sequence, SORTED before emit (unlike isVector, whose declaration order is
    // already deterministic) — issue #451's unordered_map/set slice. `type` holds
    // the element PropType E; mutually exclusive with isVector/isMap.
    bool isSet{ false };
    // When true the member is a std::unordered_map<std::string, V> serialized as
    // a YAML mapping, sorted BY KEY before emit for the same determinism reason
    // as isSet — issue #451's unordered_map/set slice. `type` holds the value
    // PropType V; the key is always std::string (the only key type any real
    // component or precedent uses). Mutually exclusive with isVector/isSet.
    bool isMap{ false };
    // Populated only when type == PropType::Struct: the recursively-classified
    // fields of the nested struct (scalar member) or the vector's element struct
    // (isVector). Empty for every scalar / primitive / enum type.
    std::vector<SerField> subFields;
    // Set only on a scalar PropType::Struct member whose nested struct/class was
    // recognised but is NOT serializer-trivial (some member of it is a container,
    // a Ref<T>, a private field, an unrecognised type, …) — the MCP sub-object
    // slice. `subFields` then holds the PARTIAL classification: the subset of the
    // nested type's public members the shared scan DID recognise.
    //
    // A partial struct always marks its OWNER non-trivial (ParseComponentFields
    // sets `ambiguous`), so it can never reach the scene-serializer emitters —
    // those only ever run on a fully-trivial component, and a fully-trivial
    // component cannot transitively contain a partial struct. The MCP field
    // emitter, whose acceptance rule is already "take whatever the scan
    // recognised" (it emits fields of non-trivial components at the top level),
    // simply applies that same rule one level down and descends into subFields.
    // ONE classifier, two acceptance rules — no second type scan to drift.
    bool structPartial{ false };

    // OLO_SERIALIZE(Clamp, Min=…, Max=…) — issue #451's Clamp slice. When set, the
    // generated deserialize ranges the read value into [clampMin, clampMax] (both
    // set) or applies a one-sided std::max/std::min (only one set). Set for a
    // scalar Float/Int/UInt/SmallInt/SmallUInt/Enum field, or (the vec3-Clamp
    // follow-up slice) a glm::vec3 field — clamped per-component via glm::clamp/
    // glm::max/glm::min instead of std::clamp. ParseComponentFields fails the
    // whole component non-trivial if Clamp is requested on any other type rather
    // than silently dropping it.
    bool hasClamp{ false };
    std::optional<std::string> clampMin;
    std::optional<std::string> clampMax;

    // Populated only when type == PropType::Ref: the spelling of T in `Ref<T>`
    // (elaborated-type keyword already peeled), used for the generated
    // AssetManager::GetAsset<T> call (issue #451 Ref<T> slice).
    std::string refType;
};

// Per-component result of the field scan.
struct ComponentSerInfo
{
    std::vector<SerField> fields;
    bool trivial{ false }; // every member is a trivial serializable type, non-empty
};

// The serialize-codegen-trivial field types. A component is auto-serializable iff
// EVERY data member maps to one of these. AssetHandle / UUID (a u64 wrapper with an
// implicit operator u64() / implicit ctor(u64)) round-trips as a u64 — issue #451's
// first slice brought it in scope, since "missing AssetHandle block ⇒ silent
// scene-data loss" was the single most pervasive instance of the footgun (materials,
// meshes, colliders, dialogue, streaming, …). Small ints (u8/u16/i8/i16) and the glm
// integer-vector / quaternion / matrix types (ivec2/3/4, quat, mat3/mat4) are detected
// here too — #451's glm/small-int slice — using the Encode/Decode helpers in
// Core/YAMLConverters.h (small ints widened to u32/i32 on emit to dodge yaml-cpp's
// raw-char encode). Enum / enum-class members round-trip as an int — issue #451's enum
// slice — but are NOT detected here: an enum type name is user-defined, so SceneSerType
// can't recognise it by spelling. ParseComponentFields classifies a member
// PropType::Enum by matching its type against the collected enum-type set
// (CollectEnumTypes), and a std::vector<E> by parsing its element type, both AFTER this
// returns Unknown. A member that names an all-trivial nested struct (or a
// std::vector<struct> of one) is classified PropType::Struct by ClassifyStruct —
// #451's nested-struct slice — also AFTER this returns Unknown. A `Ref<T>` member
// (T transitively deriving from Asset) is likewise classified PropType::Ref by
// ParseComponentFields directly (against CollectAssetTypes), AFTER this returns
// Unknown — #451's Ref<T> slice. A std::unordered_set<E> (E a sortable trivial
// scalar) and a std::string-keyed std::unordered_map<std::string, V> are also
// classified directly by ParseComponentFields after this returns Unknown —
// #451's unordered_map/set slice. Anything still unhandled — Ref<T> of a
// non-asset type, std::array, a non-string-keyed map, a vector-of-non-trivial-
// struct, raw pointer, … — returns Unknown and (being neither a built-in, an
// enum, a recognised vector/set/map, a trivial nested struct, nor an
// asset-backed Ref<T>) marks the component
// non-trivial so it stays hand-written in SceneSerializer.cpp (a future #451 slice).
static PropType SceneSerType(const std::string& t)
{
    if (t == "f32" || t == "float")
        return PropType::Float;
    if (t == "bool")
        return PropType::Bool;
    if (t == "i32" || t == "int")
        return PropType::Int;
    if (t == "u32")
        return PropType::UInt;
    if (t == "u64")
        return PropType::U64;
    if (t == "u8" || t == "u16" || t == "uint8_t" || t == "uint16_t")
        return PropType::SmallUInt;
    if (t == "i8" || t == "i16" || t == "int8_t" || t == "int16_t")
        return PropType::SmallInt;
    if (t == "AssetHandle" || t == "UUID")
        return PropType::AssetHandle;
    if (t == "glm::vec2")
        return PropType::Vec2;
    if (t == "glm::vec3")
        return PropType::Vec3;
    if (t == "glm::vec4")
        return PropType::Vec4;
    if (t == "glm::ivec2")
        return PropType::IVec2;
    if (t == "glm::ivec3")
        return PropType::IVec3;
    if (t == "glm::ivec4")
        return PropType::IVec4;
    if (t == "glm::quat")
        return PropType::Quat;
    if (t == "glm::mat3" || t == "glm::mat3x3")
        return PropType::Mat3;
    if (t == "glm::mat4" || t == "glm::mat4x4")
        return PropType::Mat4;
    if (t == "std::string")
        return PropType::String;
    return PropType::Unknown;
}

static bool IsIdentifier(const std::string& s)
{
    if (s.empty())
        return false;
    if (!(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_'))
        return false;
    for (char c : s)
    {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
            return false;
    }
    return true;
}

// Strip // line- and /* */ block-comments from a whole source string, while
// respecting "..." and '...' literals so a `//` or `/*` inside a string default
// initializer is not treated as a comment. Used only by the field collector.
static std::string StripComments(const std::string& src)
{
    std::string out;
    out.reserve(src.size());
    enum class State
    {
        Code,
        Line,
        Block,
        Str,
        Chr
    } state = State::Code;
    for (size_t i = 0; i < src.size(); ++i)
    {
        char c = src[i];
        char n = (i + 1 < src.size()) ? src[i + 1] : '\0';
        switch (state)
        {
            case State::Code:
                if (c == '/' && n == '/')
                {
                    state = State::Line;
                    ++i;
                }
                else if (c == '/' && n == '*')
                {
                    state = State::Block;
                    ++i;
                }
                else
                {
                    if (c == '"')
                        state = State::Str;
                    else if (c == '\'')
                        state = State::Chr;
                    out += c;
                }
                break;
            case State::Line:
                if (c == '\n')
                {
                    state = State::Code;
                    out += c;
                }
                break;
            case State::Block:
                if (c == '*' && n == '/')
                {
                    state = State::Code;
                    ++i;
                }
                else if (c == '\n')
                {
                    out += c; // keep line structure
                }
                break;
            case State::Str:
                out += c;
                if (c == '\\')
                {
                    if (n != '\0')
                    {
                        out += n;
                        ++i;
                    }
                }
                else if (c == '"')
                {
                    state = State::Code;
                }
                break;
            case State::Chr:
                out += c;
                if (c == '\\')
                {
                    if (n != '\0')
                    {
                        out += n;
                        ++i;
                    }
                }
                else if (c == '\'')
                {
                    state = State::Code;
                }
                break;
        }
    }
    return out;
}

// Remove every balanced `MACRO( ... )` call (e.g. OLO_PROPERTY(...) annotations)
// from a struct body so the macro — which expands to nothing — does not merge into
// the following field's declaration and get mis-parsed as a function (it has '(').
static std::string StripBalancedMacro(const std::string& s, const std::string& macro)
{
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size())
    {
        bool atBoundary = (i == 0) ||
                          !(std::isalnum(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '_');
        if (atBoundary && s.compare(i, macro.size(), macro) == 0)
        {
            size_t j = i + macro.size();
            while (j < s.size() && (s[j] == ' ' || s[j] == '\t'))
                ++j;
            if (j < s.size() && s[j] == '(')
            {
                int depth = 0;
                size_t k = j;
                for (; k < s.size(); ++k)
                {
                    if (s[k] == '(')
                        ++depth;
                    else if (s[k] == ')')
                    {
                        --depth;
                        if (depth == 0)
                        {
                            ++k;
                            break;
                        }
                    }
                }
                i = k; // skip the whole macro call
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

// Collect the *leaf* name of every `enum` / `enum class` / `enum struct`
// definition under the scan dir, used by the scene-serializer field parser to
// recognise an enum-typed member (a SceneSerType the built-in type table can't
// know about, since enum type names are user-defined). Leaf == the part after
// the last "::": a field is always declared in (or relative to) the enum's
// scope, so an unqualified `State m_State` and a qualified
// `AnimationStateComponent::State` both match the collected leaf "State".
//
// We intentionally store leaf names only:
//   * Most enum members are declared unqualified (same scope as the enum) or
//     with a leading qualifier the field strips down to the same leaf, so leaf
//     matching covers both without tracking enclosing scope.
//   * A false positive — a non-enum value field whose type's leaf coincides
//     with some enum name — only flips an otherwise-trivial component to "enum",
//     which then emits `static_cast<int>(structValue)` and FAILS TO COMPILE
//     loudly (caught at build time), never a silent data issue. A false
//     negative just leaves the component hand-written (status quo). Both fail
//     safe, so leaf matching is acceptable.
//
// Forward declarations (`enum class Foo : u8;`) are collected too — harmless,
// the name still denotes an enum type. Anonymous enums (`enum { A, B };`) have
// no name and are skipped by the regex.
static std::set<std::string> CollectEnumTypes(const fs::path& scanDir)
{
    std::set<std::string> names;
    // `enum`, optional `class`/`struct`, then the type name. The name must be a
    // plain identifier; an opaque-enum colon (`: u8`), `{`, or `;` ends it.
    static const std::regex enumRe(R"(\benum\s+(?:class\s+|struct\s+)?([A-Za-z_]\w*))");

    for (auto const& entry : fs::recursive_directory_iterator(scanDir))
    {
        if (!entry.is_regular_file())
            continue;
        if (auto ext = entry.path().extension().string(); ext != ".h" && ext != ".hpp")
            continue;

        std::ifstream file(entry.path());
        if (!file.is_open())
            continue;
        std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (raw.find("enum") == std::string::npos)
            continue;

        std::string content = StripComments(raw);
        for (auto it = std::sregex_iterator(content.begin(), content.end(), enumRe);
             it != std::sregex_iterator(); ++it)
        {
            names.insert((*it)[1].str());
        }
    }

    return names;
}

// The leaf identifier of a (possibly qualified) type name — everything after
// the last "::". `BodyType3D` → `BodyType3D`; `Ocean::SpectrumType` →
// `SpectrumType`. Used to match a component field's enum type against the
// CollectEnumTypes leaf set.
static std::string LeafTypeName(const std::string& type)
{
    if (auto pos = type.rfind("::"); pos != std::string::npos)
        return type.substr(pos + 2);
    return type;
}

// Collect the set of type names that transitively derive from `Asset`, used by the
// scene-serializer field parser to recognise a `Ref<T>` member as a persistable
// asset handle (issue #451 Ref<T> slice) — only such a T is resolvable via
// AssetManager::GetAsset<T>(handle). Scans `class Derived : public Base` (or
// `: Base` without an access-specifier) declarations under scanDir and builds a
// derived -> first-base edge map, then walks each type's base chain up to "Asset".
//
// One level of indirection is common and load-bearing here: `MeshSource`/`Mesh`
// derive from `Asset` directly, but `Material`/`Model`/`EnvironmentMap` derive from
// the intermediate `RendererResource : public Asset` — a textual "is it spelled
// `: public Asset`" check alone would miss them, so this walks the full chain
// rather than pattern-matching one hop.
//
// Only the FIRST base in a multiple-inheritance list is tracked (mirrors
// CollectEnumTypes' "fail safe on ambiguity" discipline) — a class that reaches
// Asset only through a second/third base is not recognised and its Ref<T> fields
// stay non-trivial (hand-written), same as any other unhandled construct. No
// false positive is possible this way: every name this function returns
// genuinely has an Asset-deriving base chain from its FIRST listed base.
static std::set<std::string> CollectAssetTypes(const fs::path& scanDir)
{
    std::map<std::string, std::string> bases; // derived leaf -> first-base leaf
    static const std::regex classRe(
        R"(\bclass\s+([A-Za-z_]\w*)\s*:\s*(?:public\s+|private\s+|protected\s+)?([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*))");

    for (auto const& entry : fs::recursive_directory_iterator(scanDir))
    {
        if (!entry.is_regular_file())
            continue;
        if (auto ext = entry.path().extension().string(); ext != ".h" && ext != ".hpp")
            continue;

        std::ifstream file(entry.path());
        if (!file.is_open())
            continue;
        std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (raw.find("class") == std::string::npos)
            continue;

        std::string content = StripComments(raw);
        for (auto it = std::sregex_iterator(content.begin(), content.end(), classRe);
             it != std::sregex_iterator(); ++it)
        {
            std::string derived = (*it)[1].str();
            std::string base = LeafTypeName((*it)[2].str());
            bases.try_emplace(derived, base); // first definition wins
        }
    }

    std::set<std::string> assetTypes;
    for (auto const& [name, firstBase] : bases)
    {
        std::set<std::string> chainVisited;
        std::string cur = name;
        while (true)
        {
            if (cur == "Asset")
            {
                assetTypes.insert(name);
                break;
            }
            if (!chainVisited.insert(cur).second)
                break; // cycle guard — shouldn't happen in valid C++, fail safe
            auto it = bases.find(cur);
            if (it == bases.end())
                break; // base is outside the scanned set (e.g. RefCounted) — not an Asset
            cur = it->second;
        }
    }
    return assetTypes;
}

// One entry of the record-body registry (CollectStructBodies): the text between a
// record's outermost braces, plus whether that record's members start out PRIVATE.
// A `class` defaults to private and a `struct` to public — getting this wrong on a
// class would let the parser collect a private member and emit `comp.member`, which
// would not compile. `class` bodies joined the registry with the MCP sub-object
// slice (ParticleSystemComponent's authored parameters all live inside the
// `class ParticleSystem` member).
struct StructDef
{
    std::string body;
    bool defaultPrivate{ false }; // true for `class`, false for `struct`
};

// Forward declaration: ParseComponentFields and ClassifyStruct are mutually
// recursive (a component member of struct type is classified by re-parsing that
// struct's body, which may itself contain nested-struct members).
static ComponentSerInfo ParseComponentFields(std::string body,
                                             const std::set<std::string>& enumTypes,
                                             const std::set<std::string>& assetTypes,
                                             const std::map<std::string, StructDef>& structDefs,
                                             std::set<std::string> visited,
                                             bool publicByDefault = true);

// Classify a nested record member type by its leaf name — the SHARED classifier
// behind both consumers. Returns std::nullopt only when `leaf` is not a known
// record at all (a Ref<T>, a std::array, a type defined outside OloEngine/src) or
// when the recursion re-enters it (`visited`). Otherwise it returns the recursive
// classification, whose `trivial` flag says whether EVERY member was itself a
// serializer-trivial, public type.
//
// Two acceptance rules ride on this one classification:
//   * the SCENE SERIALIZER (issue #451's nested-struct slice) takes it only when
//     `trivial` — see ClassifyStruct below, which is the strict wrapper;
//   * the MCP FIELD REGISTRY takes the PARTIAL result too (SerField::structPartial),
//     descending into whatever public, JSON-coercible members the scan recognised.
//     That is the same acceptance rule it already applies at the TOP level, where a
//     non-trivial component still contributes its recognised fields.
//
// `visited` carries the chain of record leaf names currently being classified. A
// value member can't form a real cycle in C++ (A-by-value-in-A won't compile), but
// the guard makes the recursion provably terminating and tolerates a
// pointer/reference back-edge (already non-trivial, so it fails safe anyway).
static std::optional<ComponentSerInfo> ClassifyStructInfo(
    const std::string& leaf,
    const std::set<std::string>& enumTypes,
    const std::set<std::string>& assetTypes,
    const std::map<std::string, StructDef>& structDefs,
    std::set<std::string> visited)
{
    if (visited.contains(leaf))
        return std::nullopt; // defensive cycle break
    auto it = structDefs.find(leaf);
    if (it == structDefs.end())
        return std::nullopt; // not a known record (e.g. Ref<T>, std::array, a vendor type)
    visited.insert(leaf);
    return ParseComponentFields(it->second.body, enumTypes, assetTypes, structDefs, std::move(visited),
                                !it->second.defaultPrivate);
}

// The STRICT wrapper the scene serializer uses: the recursively-classified field
// list iff `leaf` names a record whose EVERY member is itself a serializer-trivial
// type — a primitive / small-int / glm / string / AssetHandle / enum /
// std::vector<one-of-those> / a further nested-trivial-record — and all members are
// public. std::nullopt otherwise (unknown type, any non-trivial or non-public
// member, an empty record, or a re-entrant type).
static std::optional<std::vector<SerField>> ClassifyStruct(
    const std::string& leaf,
    const std::set<std::string>& enumTypes,
    const std::set<std::string>& assetTypes,
    const std::map<std::string, StructDef>& structDefs,
    std::set<std::string> visited)
{
    std::optional<ComponentSerInfo> sub =
        ClassifyStructInfo(leaf, enumTypes, assetTypes, structDefs, std::move(visited));
    if (!sub || !sub->trivial)
        return std::nullopt; // a member of the nested record is itself non-trivial
    return sub->fields;
}

// std::unordered_set<E> element eligibility (issue #451's unordered_map/set
// slice) — the same trivial scalar types std::vector<E> accepts, MINUS the glm
// vector/matrix/quat types and nested struct: none of those define a meaningful
// operator< for the sort-before-emit step in EmitSerializeFields, and no real
// component needs one as a set element. Float is excluded too — sorting a
// sequence containing NaN is undefined behavior (violates strict-weak
// ordering), and no real component needs a float set.
static bool IsSetEligiblePropType(PropType t)
{
    switch (t)
    {
        case PropType::Bool:
        case PropType::Int:
        case PropType::UInt:
        case PropType::U64:
        case PropType::SmallUInt:
        case PropType::SmallInt:
        case PropType::AssetHandle:
        case PropType::String:
        case PropType::Enum:
            return true;
        default:
            return false;
    }
}

// Parse the top-level data members of a `struct { ... }` body and classify it as
// auto-serializable or not — used for both `struct *Component` bodies and (via
// ClassifyStruct) nested struct member types. A member is collected only when it is
// unambiguously a `<trivial-type> <name>` field declaration. Anything the parser
// cannot confidently classify as a trivial field — a non-trivial type, a
// pointer/reference/template/array/bitfield, a const member — marks the whole
// struct non-trivial so it is left hand-written (ambiguity always fails safe).
static ComponentSerInfo ParseComponentFields(std::string body,
                                             const std::set<std::string>& enumTypes,
                                             const std::set<std::string>& assetTypes,
                                             const std::map<std::string, StructDef>& structDefs,
                                             std::set<std::string> visited,
                                             bool publicByDefault)
{
    ComponentSerInfo info;
    bool ambiguous = false;
    // Current access level. A struct defaults to public and a class to PRIVATE
    // (`publicByDefault`); a `private:` / `protected:` / `public:` label flips it.
    // A non-public data member cannot be referenced as `comp.member` by the
    // generated serializer, so it marks the component non-trivial (fail-safe: keep
    // it hand-written rather than emit code that won't compile — or, if the field is
    // glued to the label in the same ;-statement, silently drop it).
    bool publicSection = publicByDefault;

    // OLO_PROPERTY(...) annotations expand to nothing — drop them (paren-balanced)
    // before statement splitting so they never merge into the next field's line.
    body = StripBalancedMacro(body, "OLO_PROPERTY");

    // Split the body into top-level statements at ';' (brace-depth 0) AND at the
    // `}` that returns to brace-depth 0. The extra `}` split isolates an inline
    // method body — `void Foo() { ... }` carries no trailing ';', so without this
    // it would MERGE forward into the next member's statement, dropping that member
    // and (worse) swallowing an intervening `private:` label so a private member is
    // mis-seen as public. Flushing at the balancing `}` makes the method its own
    // statement (skipped later — it has '('), and the following member / access
    // label parses cleanly. Braced default-initializers (`glm::vec3 v = { … };`,
    // `T x{};`) also flush at their closing `}`, but the declarator is taken before
    // the first '=' / '{', so they still classify correctly; the leftover ';' just
    // yields an empty statement that Trim() drops. This robustness is what lets a
    // nested struct with inline methods + private members (e.g. ColliderMaterial)
    // be correctly classified non-trivial instead of emitting private-access code.
    std::vector<std::string> statements;
    {
        int depth = 0;
        std::string buf;
        for (char c : body)
        {
            if (c == '{')
            {
                ++depth;
                buf += c;
            }
            else if (c == '}')
            {
                if (depth > 0)
                    --depth;
                buf += c;
                if (depth == 0)
                {
                    statements.push_back(buf);
                    buf.clear();
                }
            }
            else if (c == ';' && depth == 0)
            {
                statements.push_back(buf);
                buf.clear();
            }
            else
            {
                buf += c;
            }
        }
    }

    for (auto const& raw : statements)
    {
        std::string s = Trim(raw);
        if (s.empty())
            continue;

        // Peel any leading access-specifier label(s), tracking the current access
        // level. A label can be glued to the following member in the same ;-statement
        // (e.g. "private: glm::quat Rotation"), so loop until none remain. The actual
        // public/non-public decision is applied where a field would be collected.
        for (bool peeled = true; peeled;)
        {
            peeled = false;
            for (auto const& [label, makesPublic] : std::initializer_list<std::pair<const char*, bool>>{
                     { "public:", true }, { "private:", false }, { "protected:", false } })
            {
                const std::string lbl = label;
                if (s.rfind(lbl, 0) == 0)
                {
                    publicSection = makesPublic;
                    s = Trim(s.substr(lbl.size()));
                    peeled = true;
                    break;
                }
            }
        }
        if (s.empty())
            continue; // the statement was nothing but access-specifier label(s)

        // A leading OLO_SERIALIZE(...) annotation controls how the scene-serializer
        // codegen treats this member (issue #451). `Skip` drops the member from the
        // generated serialize/deserialize — a runtime-only field the round-trip must
        // NOT persist (e.g. UIButtonComponent::m_State, UISliderComponent::m_IsDragging)
        // — and, crucially, does NOT mark the component non-trivial. So an otherwise
        // all-trivial component with one runtime field is now fully generated instead
        // of being kept hand-written via kComponentsCustomSerialize (per-field control
        // replacing the old all-or-nothing-per-component exclusion). `Clamp` (with
        // `Min`/`Max`) range-validates the field on deserialize — its own field-type
        // eligibility check happens below, once the field's PropType is known. The
        // macro expands to nothing (see Scene/ComponentReflection.h) and has no ';',
        // so it is glued to the annotated field inside one statement; OLO_PROPERTY was
        // already stripped above, so OLO_SERIALIZE is the statement prefix here.
        bool fieldHasClamp = false;
        std::optional<std::string> fieldClampMin, fieldClampMax;
        if (std::string serArgs, serRest; PeelSerializeMarker(s, serArgs, serRest))
        {
            if (SerializeArgsHaveSkip(serArgs))
                continue; // runtime-only field — not serialized, not non-trivial
            fieldHasClamp = SerializeArgsClampBounds(serArgs, fieldClampMin, fieldClampMax);
            s = serRest; // peel the annotation; the field then serializes normally
            if (s.empty())
                continue;
        }

        // The declarator is everything before the first '=' (default initializer)
        // or '{' (braced initializer / method body).
        size_t cut = s.size();
        if (auto k = s.find('='); k != std::string::npos)
            cut = std::min(cut, k);
        if (auto k = s.find('{'); k != std::string::npos)
            cut = std::min(cut, k);
        std::string decl = Trim(s.substr(0, cut));
        if (decl.empty())
            continue;

        std::string first;
        {
            std::istringstream ts(decl);
            ts >> first;
        }
        // Skip non-data-member constructs (these are not silent drops — a method /
        // static / using is genuinely not a serialized field).
        static const std::set<std::string> kSkipFirst = {
            "static", "constexpr", "inline", "friend", "using", "typedef",
            "struct", "class", "enum", "union", "template", "return",
            "public:", "private:", "protected:", "mutable"
        };
        if (kSkipFirst.contains(first))
            continue;
        if (decl[0] == '~' || decl.find("operator") != std::string::npos)
            continue;
        if (decl.find('(') != std::string::npos) // function / constructor / destructor
            continue;

        // std::vector<E> member — serialized as a YAML sequence. Handled before the
        // generic complex-declarator rejection below (which bans '<' / '>'). A vector
        // whose element E is itself a trivial serializer type (scalar / small-int /
        // glm / string / AssetHandle / enum) is auto-serializable; anything else
        // (vector of struct / Ref / nested template, or any non-vector template such
        // as Ref<T> / std::unordered_map) marks the component non-trivial and is left
        // hand-written. Only single-level vectors are accepted (a nested '<' in the
        // element type fails safe to non-trivial).
        if (auto lt = decl.find('<'); lt != std::string::npos &&
                                      Trim(decl.substr(0, lt)) == "std::vector")
        {
            auto gt = decl.rfind('>');
            std::string inner = (gt != std::string::npos && gt > lt) ? Trim(decl.substr(lt + 1, gt - lt - 1)) : "";
            std::string rest = (gt != std::string::npos) ? Trim(decl.substr(gt + 1)) : "";
            PropType ept = SceneSerType(inner);
            if (ept == PropType::Unknown && enumTypes.contains(LeafTypeName(inner)))
                ept = PropType::Enum; // a vector of enum — each element round-trips as an int
            // A vector whose element is itself an all-trivial nested struct — each
            // element round-trips as a YAML sub-map (issue #451 nested-struct slice).
            // Guarded by inner.find('<') == npos so a vector<vector<…>> / vector<Ref<T>>
            // stays non-trivial (nested templates aren't parsed here).
            std::optional<std::vector<SerField>> elemStruct;
            if (ept == PropType::Unknown && inner.find('<') == std::string::npos)
            {
                elemStruct = ClassifyStruct(LeafTypeName(inner), enumTypes, assetTypes, structDefs, visited);
                if (elemStruct)
                    ept = PropType::Struct;
            }
            if (inner.find('<') != std::string::npos || ept == PropType::Unknown || !IsIdentifier(rest) ||
                !publicSection || fieldHasClamp)
            {
                // vector of Ref / nested template / non-trivial struct / bad name, a
                // non-public member the serializer can't reach as comp.member, or a
                // Clamp annotation on a vector field (unsupported — element-wise
                // clamping is a follow-up, not this slice) — non-trivial.
                ambiguous = true;
                continue;
            }
            SerField f;
            f.member = rest;
            f.key = StripPrefix(rest, "m_");
            f.type = ept; // element type
            f.isVector = true;
            if (ept == PropType::Struct)
                f.subFields = std::move(*elemStruct);
            info.fields.push_back(f);
            continue;
        }

        // std::unordered_set<E> member (issue #451 unordered_map/set slice) —
        // serialized as a YAML sequence, SORTED before emit since (unlike
        // std::vector) an unordered_set's iteration order is not deterministic —
        // see EmitSerializeFields. E must be one of the sortable trivial scalar
        // types (IsSetEligiblePropType); a set of struct / Ref / glm-vector /
        // float / nested template stays non-trivial, same fail-safe discipline as
        // every other unhandled construct here.
        if (auto lt = decl.find('<'); lt != std::string::npos &&
                                      Trim(decl.substr(0, lt)) == "std::unordered_set")
        {
            auto gt = decl.rfind('>');
            std::string inner = (gt != std::string::npos && gt > lt) ? Trim(decl.substr(lt + 1, gt - lt - 1)) : "";
            std::string rest = (gt != std::string::npos) ? Trim(decl.substr(gt + 1)) : "";
            PropType ept = SceneSerType(inner);
            if (ept == PropType::Unknown && enumTypes.contains(LeafTypeName(inner)))
                ept = PropType::Enum;
            if (inner.find('<') != std::string::npos || !IsSetEligiblePropType(ept) || !IsIdentifier(rest) ||
                !publicSection || fieldHasClamp)
            {
                ambiguous = true;
                continue;
            }
            SerField f;
            f.member = rest;
            f.key = StripPrefix(rest, "m_");
            f.type = ept;
            f.isSet = true;
            info.fields.push_back(f);
            continue;
        }

        // std::unordered_map<std::string, V> member (issue #451 unordered_map/set
        // slice) — serialized as a genuine YAML mapping, sorted BY KEY before emit
        // for the same determinism reason as std::unordered_set above (matches the
        // on-disk shape of every hand-written string-keyed map in this codebase,
        // e.g. MorphTargetComponent::Weights). Scoped to a std::string key — the
        // only key type any real component or hand-written precedent uses — via
        // SplitTopLevelCommas: exactly 2 top-level template args are required, so
        // a 3rd arg (custom hash/equality functor) or a non-string key stays
        // non-trivial. V is any trivial scalar/glm/string/enum/AssetHandle element
        // type std::vector accepts; a struct-valued map is not handled by this
        // slice (no real component needs one).
        if (auto lt = decl.find('<'); lt != std::string::npos &&
                                      Trim(decl.substr(0, lt)) == "std::unordered_map")
        {
            auto gt = decl.rfind('>');
            std::string inner = (gt != std::string::npos && gt > lt) ? Trim(decl.substr(lt + 1, gt - lt - 1)) : "";
            std::string rest = (gt != std::string::npos) ? Trim(decl.substr(gt + 1)) : "";
            std::vector<std::string> parts = SplitTopLevelCommas(inner);
            PropType vt = PropType::Unknown;
            bool keyOk = false;
            if (parts.size() == 2)
            {
                keyOk = parts[0] == "std::string";
                vt = SceneSerType(parts[1]);
                if (vt == PropType::Unknown && enumTypes.contains(LeafTypeName(parts[1])))
                    vt = PropType::Enum;
            }
            if (!keyOk || vt == PropType::Unknown || !IsIdentifier(rest) || !publicSection || fieldHasClamp)
            {
                ambiguous = true;
                continue;
            }
            SerField f;
            f.member = rest;
            f.key = StripPrefix(rest, "m_");
            f.type = vt;
            f.isMap = true;
            info.fields.push_back(f);
            continue;
        }

        // Ref<T> member (issue #451 Ref<T> slice) — handled before the generic
        // complex-declarator rejection below (which bans '<' / '>'), same as
        // std::vector<E> above. Auto-serializable iff T transitively derives from
        // Asset (assetTypes, from CollectAssetTypes) and is therefore resolvable via
        // AssetManager::GetAsset<T>(handle); a Ref<T> of a non-asset type (Skeleton,
        // FoliageRenderer, a nested Ref<Ref<T>>, …) marks the component non-trivial,
        // same as any other unhandled construct (stays hand-written unless the field
        // carries OLO_SERIALIZE(Skip), which drops it before this point regardless of
        // type — see the Skip handling above).
        if (auto lt = decl.find('<'); lt != std::string::npos &&
                                      Trim(decl.substr(0, lt)) == "Ref")
        {
            auto gt = decl.rfind('>');
            std::string inner = (gt != std::string::npos && gt > lt) ? Trim(decl.substr(lt + 1, gt - lt - 1)) : "";
            std::string rest = (gt != std::string::npos) ? Trim(decl.substr(gt + 1)) : "";
            // Peel an elaborated-type keyword (`Ref<class Material>`, `Ref<struct Foo>`)
            // — some declarations spell the template argument this way to avoid a
            // forward-declaration include.
            for (std::string_view kw : { "class ", "struct " })
            {
                if (inner.starts_with(kw))
                {
                    inner = Trim(inner.substr(kw.size()));
                    break;
                }
            }
            std::string leaf = LeafTypeName(inner);
            if (inner.find('<') != std::string::npos || !assetTypes.contains(leaf) ||
                !IsIdentifier(rest) || !publicSection || fieldHasClamp)
            {
                // Ref<non-asset-type> / Ref<Ref<T>> / bad name, a non-public member the
                // serializer can't reach as comp.member, or a Clamp annotation on a Ref
                // field (unsupported) — non-trivial.
                ambiguous = true;
                continue;
            }
            SerField f;
            f.member = rest;
            f.key = StripPrefix(rest, "m_");
            f.type = PropType::Ref;
            f.refType = inner;
            info.fields.push_back(f);
            continue;
        }

        // A complex declarator — pointer/reference/template/array/bitfield/multi —
        // is a member we cannot trivially serialize: fail the whole component safe.
        // ('::' in glm::vec3 / std::string is legitimate; strip it before the test.)
        std::string declNoScope = ReplaceAll(decl, "::", "");
        if (declNoScope.find_first_of("*&<>[],:") != std::string::npos)
        {
            ambiguous = true;
            continue;
        }

        std::vector<std::string> toks;
        {
            std::istringstream ts(decl);
            std::string w;
            while (ts >> w)
                toks.push_back(w);
        }
        if (toks.size() < 2)
            continue; // a lone token (macro, label) — not a field

        std::string name = toks.back();
        std::string type;
        for (size_t i = 0; i + 1 < toks.size(); ++i)
        {
            if (i)
                type += ' ';
            type += toks[i];
        }
        if (!IsIdentifier(name))
        {
            ambiguous = true;
            continue;
        }

        PropType pt = SceneSerType(type);
        if (pt == PropType::Unknown && enumTypes.contains(LeafTypeName(type)))
            pt = PropType::Enum; // an enum-typed member — round-trips as an int
        // A scalar member whose type is an all-trivial nested struct — round-trips as
        // a YAML sub-map (issue #451 nested-struct slice). Tried after the built-in
        // and enum checks; `type` here has already passed the complex-declarator
        // rejection above (no '*'/'&'/'<'/'['), so a Ref<T> / std::array never reaches
        // this point.
        //
        // A nested record that is RECOGNISED but not trivial (some member of it is a
        // container / Ref<T> / private / unrecognised) is kept as a PARTIAL struct
        // (SerField::structPartial) instead of being dropped: it still marks the owner
        // non-trivial — so the scene serializer keeps the component hand-written,
        // exactly as before — but the MCP field emitter can descend into the public,
        // JSON-coercible members the scan did recognise. This is what makes
        // `ParticleSystemComponent.System.Emitter.RateOverTime` writable without a
        // second type classifier.
        std::optional<ComponentSerInfo> nested;
        if (pt == PropType::Unknown)
        {
            nested = ClassifyStructInfo(LeafTypeName(type), enumTypes, assetTypes, structDefs, visited);
            if (nested)
                pt = PropType::Struct;
        }
        if (pt == PropType::Unknown)
        {
            ambiguous = true; // an unrecognised type — keep the component hand-written
            continue;
        }
        if (!publicSection)
        {
            // A non-public data member can't be reached as comp.member by the
            // generated serializer (and emitting it would not compile) — keep the
            // whole component hand-written.
            ambiguous = true;
            continue;
        }
        // Clamp (issue #451) is supported on scalar Float/Int/UInt/SmallInt/
        // SmallUInt/Enum fields, and — the vec3-Clamp follow-up slice — glm::vec3
        // (clamped per-component, mirroring the hand-written SanitizeVec3Clamped
        // idiom). Requesting it on any other type (Vec2/Vec4/Struct/…), or with
        // neither Min nor Max given, marks the whole component non-trivial rather
        // than silently dropping the annotation (ambiguity fails safe, same
        // discipline as every other unsupported construct in this parser).
        static const std::set<PropType> kClampEligible = {
            PropType::Float, PropType::Int, PropType::UInt,
            PropType::SmallInt, PropType::SmallUInt, PropType::Enum, PropType::Vec3
        };
        if (fieldHasClamp && (!kClampEligible.contains(pt) || (!fieldClampMin && !fieldClampMax)))
        {
            ambiguous = true;
            continue;
        }

        SerField f;
        f.member = name;
        f.key = StripPrefix(name, "m_");
        f.type = pt;
        if (pt == PropType::Struct)
        {
            f.structPartial = !nested->trivial;
            f.subFields = std::move(nested->fields);
            if (f.structPartial)
            {
                // The owner stays non-trivial (hand-written serializer) — a partial
                // struct has members the serializer codegen cannot round-trip.
                ambiguous = true;
                // An empty partial struct carries nothing for anyone: drop it.
                if (f.subFields.empty())
                    continue;
            }
        }
        if (fieldHasClamp)
        {
            f.hasClamp = true;
            f.clampMin = fieldClampMin;
            f.clampMax = fieldClampMax;
        }
        info.fields.push_back(f);
    }

    info.trivial = !ambiguous && !info.fields.empty();
    return info;
}

// Scan every header for `struct <Name> { ... }` / `class <Name> { ... }`
// *definitions* and return a leaf-name → record registry (the body text between the
// outermost braces, plus the record's default access). This is the input to the
// nested-record classifier (ClassifyStructInfo re-parses a member's body). It covers
// ALL records, not just `*Component`, since a component's nested member type
// (LODGroup, OffMeshLink, ParticleSystem, …) is an ordinary struct or class.
//
// `class` bodies joined this registry with the MCP sub-object slice — every authored
// parameter of ParticleSystemComponent lives inside a `class ParticleSystem` member,
// so a struct-only registry left the component with ZERO writable fields. A class's
// members start PRIVATE, which ParseComponentFields is told via
// StructDef::defaultPrivate; getting that wrong would let the parser collect a
// private member and emit code that does not compile.
//
// `enum class E { … }` is explicitly NOT a record — its body is an enumerator list,
// and enum members are already classified by the CollectEnumTypes set. Forward
// declarations (`friend class X;`) and unbalanced bodies are skipped. First
// definition of a name wins, EXCEPT that a `struct` always outranks a `class` of the
// same leaf name, so adding classes cannot change any pre-existing struct's entry —
// the scene-serializer codegen is bit-for-bit unaffected by that widening.
static std::map<std::string, StructDef> CollectStructBodies(const fs::path& scanDir)
{
    std::map<std::string, StructDef> result;
    static const std::regex recordRe(R"(\b(struct|class)\s+([A-Za-z_]\w*)\b)");

    for (auto const& entry : fs::recursive_directory_iterator(scanDir))
    {
        if (!entry.is_regular_file())
            continue;
        if (auto ext = entry.path().extension().string(); ext != ".h" && ext != ".hpp")
            continue;

        std::ifstream file(entry.path());
        if (!file.is_open())
            continue;
        std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (raw.find("struct") == std::string::npos && raw.find("class") == std::string::npos)
            continue;

        std::string content = StripComments(raw);
        for (auto it = std::sregex_iterator(content.begin(), content.end(), recordRe);
             it != std::sregex_iterator(); ++it)
        {
            const bool isClass = (*it)[1].str() == "class";
            std::string name = (*it)[2].str();
            const auto matchPos = static_cast<size_t>(it->position());

            // `enum class E : u8 { A, B }` — an enumerator list, not a record body.
            if (isClass)
            {
                size_t b = matchPos;
                while (b > 0 && std::isspace(static_cast<unsigned char>(content[b - 1])))
                    --b;
                if (b >= 4 && content.compare(b - 4, 4, "enum") == 0)
                    continue;
            }

            size_t p = matchPos + static_cast<size_t>(it->length());
            // Skip to the body-opening '{' or a ';' (forward declaration).
            while (p < content.size() && content[p] != '{' && content[p] != ';')
                ++p;
            if (p >= content.size() || content[p] == ';')
                continue; // forward decl — no body

            // Brace-match the body.
            int depth = 0;
            size_t start = p;
            size_t q = p;
            for (; q < content.size(); ++q)
            {
                if (content[q] == '{')
                    ++depth;
                else if (content[q] == '}')
                {
                    --depth;
                    if (depth == 0)
                        break;
                }
            }
            if (q >= content.size())
                continue; // unbalanced — skip

            if (auto existing = result.find(name); existing != result.end())
            {
                // A struct definition supersedes a class one of the same leaf name;
                // otherwise the first definition wins (matches CollectComponentStructs).
                if (isClass || !existing->second.defaultPrivate)
                    continue;
                existing->second = StructDef{ content.substr(start + 1, q - start - 1), false };
                continue;
            }
            result.emplace(name, StructDef{ content.substr(start + 1, q - start - 1), isClass });
        }
    }

    return result;
}

// Parse each `struct *Component`'s data members into a ComponentSerInfo, so the
// scene serializer codegen can emit per-field read/writes. Built on the shared
// struct-body registry (CollectStructBodies), which also feeds the nested-struct
// classifier. Returns a name → ComponentSerInfo map (std::map for deterministic,
// alphabetical emit order).
static std::map<std::string, ComponentSerInfo> CollectComponentFields(const fs::path& scanDir,
                                                                      const std::set<std::string>& enumTypes)
{
    const std::map<std::string, StructDef> structDefs = CollectStructBodies(scanDir);
    const std::set<std::string> assetTypes = CollectAssetTypes(scanDir);
    std::map<std::string, ComponentSerInfo> result;
    for (auto const& [name, def] : structDefs)
    {
        if (!name.ends_with("Component"))
            continue;
        // ECS components are `struct *Component` by convention — and every other
        // generated touch-point (the AllComponents tuple, the SaveGame lists) is
        // driven by the struct-only CollectComponentStructs scan. A `class *Component`
        // in the registry (it holds classes since the MCP sub-object slice) is NOT an
        // ECS component; skipping it keeps this scan's component set identical to
        // theirs.
        if (def.defaultPrivate)
            continue;
        result.emplace(name, ParseComponentFields(def.body, enumTypes, assetTypes, structDefs, {}));
    }
    return result;
}

// ─── AllComponents Tuple Emitter ─────────────────────────────────────────────────

static void EmitAllComponentsTuple(std::ostream& out, const std::set<std::string>& componentNames)
{
    out << "// Auto-generated by OloHeaderTool — DO NOT EDIT MANUALLY\n";
    out << "// Re-generate with: cmake --build build --target GenerateBindings\n";
    out << "//\n";
    out << "// The AllComponents type list iterated by Scene::CopyComponent / prefab\n";
    out << "// instantiation and ScriptGlue's C# HasComponent<T>() registration. One entry\n";
    out << "// per `struct *Component` definition under OloEngine/src, minus the entity-\n";
    out << "// identity / runtime-derived components the generator excludes (kept in sync\n";
    out << "// with ComponentTupleCoverageTest::kNotInTuple). Entries are alphabetical;\n";
    out << "// tuple order is irrelevant to copy / registration correctness.\n";
    out << "//\n";
    out << "// Included at namespace scope from Scene/Components.h, immediately after the\n";
    out << "// ComponentGroup<> template and every component definition.\n\n";

    out << "using AllComponents = ComponentGroup<\n";
    bool first = true;
    for (auto const& name : componentNames)
    {
        if (kComponentsNotInTuple.contains(name))
            continue;
        if (!first)
            out << ",\n";
        out << "    " << name;
        first = false;
    }
    out << ">;\n";
}

// ─── Save-Game Capture / Restore List Emitters ───────────────────────────────────

// Shared doc header for both save-game list files. `verb` is the human label for
// what the file does ("capture" / "restore"); `macro` names the macro the caller
// must have in scope at the #include site.
static void EmitSaveGameListHeader(std::ostream& out, const char* verb, const char* macro)
{
    out << "// Auto-generated by OloHeaderTool — DO NOT EDIT MANUALLY\n";
    out << "// Re-generate with: cmake --build build --target GenerateBindings\n";
    out << "//\n";
    out << "// The per-entity save-game " << verb << " list — one " << macro << "(...)\n";
    out << "// per `struct *Component` definition under OloEngine/src that has a\n";
    out << "// SaveGameComponentSerializer::Serialize() overload (i.e. minus the\n";
    out << "// generator's kComponentsNotInSaveGame exclusion set). #include'd inside\n";
    out << "// SaveGameSerializer.cpp where " << macro << " and the entity/archive\n";
    out << "// locals are in scope. Entries are alphabetical; per-entity order is\n";
    out << "// irrelevant — capture writes a typeHash per block and restore matches\n";
    out << "// each block by hash, so neither path depends on list order.\n";
    out << "//\n";
    out << "// A component missing from these lists is silently dropped from every\n";
    out << "// save-game (it round-trips through scene YAML but vanishes through\n";
    out << "// save-games). Guarded by SaveGameComponentSerializerCoverageTest.\n\n";
}

// SAVE_COMPONENT(Name, entity, writer); — the capture loop in CaptureSceneState.
static void EmitSaveGameCaptureList(std::ostream& out, const std::set<std::string>& componentNames)
{
    EmitSaveGameListHeader(out, "capture", "SAVE_COMPONENT");
    for (auto const& name : componentNames)
    {
        if (kComponentsNotInSaveGame.contains(name))
            continue;
        out << "SAVE_COMPONENT(" << name << ", entity, writer);\n";
    }
}

// TRY_LOAD_COMPONENT(Name); — the restore matcher in DeserializeEntitiesInto.
static void EmitSaveGameRestoreList(std::ostream& out, const std::set<std::string>& componentNames)
{
    EmitSaveGameListHeader(out, "restore", "TRY_LOAD_COMPONENT");
    for (auto const& name : componentNames)
    {
        if (kComponentsNotInSaveGame.contains(name))
            continue;
        out << "TRY_LOAD_COMPONENT(" << name << ");\n";
    }
}

// ─── Scene OnComponent{Added,Removed} No-op List Emitters ─────────────────────

// Shared doc header for both Scene handler no-op files. `verb` is the callback
// ("Added" / "Removed"); `macro` names the macro the caller must have in scope at
// the #include site; `customSet` is the name of the exclusion set in this file.
static void EmitHandlerNoopHeader(std::ostream& out, const char* verb, const char* macro, const char* customSet)
{
    out << "// Auto-generated by OloHeaderTool — DO NOT EDIT MANUALLY\n";
    out << "// Re-generate with: cmake --build build --target GenerateBindings\n";
    out << "//\n";
    out << "// One " << macro << "(T) per `struct *Component` definition under\n";
    out << "// OloEngine/src whose Scene::OnComponent" << verb << "<T> is a pure no-op\n";
    out << "// (i.e. minus the generator's " << customSet << " exclusion set — the\n";
    out << "// components whose " << verb << " callback is hand-written in Scene.cpp because\n";
    out << "// it does real work). #include'd inside Scene.cpp where " << macro << "\n";
    out << "// is defined and the OloEngine namespace is open. Entries are alphabetical;\n";
    out << "// specialization order is irrelevant (the primary template is declaration-only).\n";
    out << "//\n";
    out << "// A component added/removed via Add/RemoveComponent<T>() with no specialization\n";
    out << "// is a link error (engine for add, OloEditor for remove) — this list is what\n";
    out << "// keeps every component linkable. Guarded by ComponentHandlerCoverageTest.\n";
    out << "// `Skeleton` is deliberately absent (not a `struct *Component`); its no-op\n";
    out << "// stays hand-written in Scene.cpp.\n\n";
}

// OLO_ON_COMPONENT_ADDED_NOOP(Name) — the empty-body add specializations.
static void EmitOnComponentAddedNoops(std::ostream& out, const std::set<std::string>& componentNames)
{
    EmitHandlerNoopHeader(out, "Added", "OLO_ON_COMPONENT_ADDED_NOOP", "kComponentsCustomOnAdd");
    for (auto const& name : componentNames)
    {
        if (kComponentsCustomOnAdd.contains(name))
            continue;
        out << "OLO_ON_COMPONENT_ADDED_NOOP(" << name << ")\n";
    }
}

// OLO_ON_COMPONENT_REMOVED_NOOP(Name) — the empty-body remove specializations.
static void EmitOnComponentRemovedNoops(std::ostream& out, const std::set<std::string>& componentNames)
{
    EmitHandlerNoopHeader(out, "Removed", "OLO_ON_COMPONENT_REMOVED_NOOP", "kComponentsCustomOnRemove");
    for (auto const& name : componentNames)
    {
        if (kComponentsCustomOnRemove.contains(name))
            continue;
        out << "OLO_ON_COMPONENT_REMOVED_NOOP(" << name << ")\n";
    }
}

// ─── Scene Serializer Serialize/Deserialize Block Emitters ────────────────────

// Shared doc header for the two scene-serializer generated files. `verb` is the
// human label ("serialize" / "deserialize"); `func` names the SceneSerializer.cpp
// member function the file is #include'd into; `locals` documents the in-scope
// locals the generated code references.
static void EmitSceneSerializerHeader(std::ostream& out, const char* verb, const char* func, const char* locals)
{
    out << "// Auto-generated by OloHeaderTool — DO NOT EDIT MANUALLY\n";
    out << "// Re-generate with: cmake --build build --target GenerateBindings\n";
    out << "//\n";
    out << "// Per-component scene-YAML " << verb << " blocks — one block per\n";
    out << "// `struct *Component` whose every data member is a primitive / small-int /\n";
    out << "// glm::vec*/ivec*/quat/mat* / std::string / AssetHandle / enum / Ref<T> of an\n";
    out << "// Asset-derived T, an all-trivial nested struct, a std::vector of one of those,\n";
    out << "// a std::unordered_set of a sortable trivial scalar, or a std::string-keyed\n";
    out << "// std::unordered_map (the generator's SceneSerType plus the enum-type,\n";
    out << "// Ref<T>/CollectAssetTypes, nested-struct, std::vector, and unordered_set/map\n";
    out << "// handling), minus the kComponentsCustomSerialize exclusion set (trivial\n";
    out << "// components deliberately kept hand-written). A component with any still-\n";
    out << "// unhandled non-trivial field (Ref<T> of a non-asset type, std::array, a\n";
    out << "// non-string-keyed map, a vector-of-non-trivial-struct, or a non-public member)\n";
    out << "// is classified non-trivial and stays hand-written.\n";
    out << "//\n";
    out << "// #include'd inside SceneSerializer::" << func << ", where " << locals << "\n";
    out << "// are in scope. Floats are validated with std::isfinite via TryReadFiniteF32 /\n";
    out << "// the glm Decode helpers (NaN/Inf in YAML keeps the constructor default). A\n";
    out << "// field annotated OLO_SERIALIZE(Clamp, Min=…, Max=…) additionally ranges a\n";
    out << "// successfully-read scalar value into [Min, Max] (issue #451).\n";
    out << "//\n";
    out << "// Each component is handled by EXACTLY ONE of this file or the hand-written\n";
    out << "// serializer — ComponentSerializerCoverageTest fails loudly on a drop or a\n";
    out << "// double-emit, so a new trivial component is auto-serialized and a new complex\n";
    out << "// one fails the coverage test until hand-written.\n\n";
}

// The streamed write expression for a single trivial value `expr` of PropType `t`.
// Used for both scalar field writes and per-element writes inside a std::vector
// sequence. AssetHandle / enum / small-int need a cast (no/ wrong YAML::Emitter
// overload); everything else streams directly via an existing Emitter<< overload.
static std::string SceneWriteExpr(PropType t, const std::string& expr)
{
    switch (t)
    {
        case PropType::AssetHandle:
            return "static_cast<u64>(" + expr + ")";
        case PropType::Enum:
            return "static_cast<int>(" + expr + ")";
        case PropType::SmallUInt:
            return "static_cast<u32>(" + expr + ")";
        case PropType::SmallInt:
            return "static_cast<i32>(" + expr + ")";
        default:
            return expr;
    }
}

// Shared clamp-expression assembler behind ApplyClamp/ApplyVec3Clamp: given the
// clamp-function namespace prefix ("std::" or "glm::") and the ALREADY-WRAPPED
// Min/Max bound expressions (a scalar static_cast for ApplyClamp, a glm::vec3(...)
// broadcast for ApplyVec3Clamp), assembles the same both-bounds -> clamp /
// one-sided -> max/min / neither -> unchanged branching (issue #451's Clamp
// annotation: both bounds given -> clamp, one bound given -> max/min, neither
// given -> returns `expr` unchanged — ParseComponentFields never produces this
// last case since Clamp requires at least one of Min/Max, but the no-op fallback
// keeps the callers safe to call unconditionally once they've checked f.hasClamp).
static std::string AssembleClampExpr(const std::string& expr, const std::string& ns,
                                     const std::optional<std::string>& wrappedMin,
                                     const std::optional<std::string>& wrappedMax)
{
    if (wrappedMin && wrappedMax)
        return ns + "clamp(" + expr + ", " + *wrappedMin + ", " + *wrappedMax + ")";
    if (wrappedMin)
        return ns + "max(" + expr + ", " + *wrappedMin + ")";
    if (wrappedMax)
        return ns + "min(" + expr + ", " + *wrappedMax + ")";
    return expr;
}

// Wrap a deserialize value expression `expr` (already of type `castType`, e.g.
// "f32" / "i32" / "decltype(lhs)") in a range clamp per the field's Clamp
// annotation (issue #451): each bound is cast to the field's own type, so
// `Min = 0` is fine on a float field.
static std::string ApplyClamp(const std::string& expr, const std::string& castType, const SerField& f)
{
    std::optional<std::string> wrappedMin, wrappedMax;
    if (f.clampMin)
        wrappedMin = "static_cast<" + castType + ">(" + *f.clampMin + ")";
    if (f.clampMax)
        wrappedMax = "static_cast<" + castType + ">(" + *f.clampMax + ")";
    return AssembleClampExpr(expr, "std::", wrappedMin, wrappedMax);
}

// Wrap a glm::vec3 deserialize expression `expr` in a PER-COMPONENT range clamp
// per the field's Clamp annotation (issue #451's vec3-Clamp slice) — mirrors the
// hand-written SanitizeVec3Clamped idiom, applied AFTER the plain `.as<glm::vec3>`
// read (which already finiteness-validates the whole vector, falling back to the
// pre-existing value on any non-finite component — see DecodeVec3). glm::clamp /
// glm::max / glm::min have vector-vs-scalar overloads that broadcast the scalar
// bound across all three components, matching std::clamp/max/min per component.
static std::string ApplyVec3Clamp(const std::string& expr, const SerField& f)
{
    std::optional<std::string> wrappedMin, wrappedMax;
    if (f.clampMin)
        wrappedMin = "glm::vec3(" + *f.clampMin + ")";
    if (f.clampMax)
        wrappedMax = "glm::vec3(" + *f.clampMax + ")";
    return AssembleClampExpr(expr, "glm::", wrappedMin, wrappedMax);
}

// The element loop / temp variable base name for depth `d`. Depth 0 keeps the
// legacy names (`e`, `seqNode`) so the generated .inl for the pre-existing
// scalar/primitive-vector components is byte-for-byte unchanged; deeper nesting
// (introduced by the #451 nested-struct slice) suffixes the depth to stay unique
// and dodge -Wshadow across nested loops.
static std::string ElemVar(int depth)
{
    return depth == 0 ? "e" : "e" + std::to_string(depth);
}
static std::string SeqVar(int depth)
{
    return depth == 0 ? "seqNode" : "seqNode" + std::to_string(depth);
}

// Recursively emit the YAML writes for `fields` of a struct instance reached via
// `inst` (e.g. "comp", "comp.m_LODGroup", or an element loop var). A PropType::Struct
// member recurses: a scalar nested struct as a YAML sub-map, a std::vector<struct> as
// a sequence of sub-maps. `indent` is the leading whitespace; `depth` uniquifies the
// element loop variable across nesting levels.
static void EmitSerializeFields(std::ostream& out, const std::vector<SerField>& fields,
                                const std::string& inst, const std::string& indent, int depth)
{
    for (auto const& f : fields)
    {
        const std::string access = inst + "." + f.member;
        if (f.type == PropType::Struct)
        {
            if (f.isVector)
            {
                const std::string ev = ElemVar(depth);
                out << indent << "out << YAML::Key << \"" << f.key << "\" << YAML::Value << YAML::BeginSeq;\n";
                out << indent << "for (auto const& " << ev << " : " << access << ")\n";
                out << indent << "{\n";
                out << indent << "    out << YAML::BeginMap;\n";
                EmitSerializeFields(out, f.subFields, ev, indent + "    ", depth + 1);
                out << indent << "    out << YAML::EndMap;\n";
                out << indent << "}\n";
                out << indent << "out << YAML::EndSeq;\n";
            }
            else
            {
                // Scalar nested struct — a YAML sub-map keyed by the member name.
                out << indent << "out << YAML::Key << \"" << f.key << "\" << YAML::Value << YAML::BeginMap;\n";
                EmitSerializeFields(out, f.subFields, access, indent, depth);
                out << indent << "out << YAML::EndMap;\n";
            }
            continue;
        }
        // Ref<T> (issue #451 Ref<T> slice) — only serialize a non-null, actually
        // asset-manager-registered reference (GetHandle() != 0). A Ref constructed
        // directly (Ref<T>::Create(...), never registered with the asset manager)
        // has handle 0 and is deliberately left unpersisted — matches the
        // hand-written MeshComponent::m_MeshSource -> "MeshSourceHandle" idiom.
        if (f.type == PropType::Ref)
        {
            out << indent << "if (" << access << " && " << access << "->GetHandle() != 0)\n";
            out << indent << "    out << YAML::Key << \"" << f.key << "Handle\" << YAML::Value << static_cast<u64>("
                << access << "->GetHandle());\n";
            continue;
        }
        // std::unordered_set<E> (issue #451 unordered_map/set slice) — copy into a
        // temporary vector and sort it before emitting, so the YAML output is
        // deterministic (an unordered_set's iteration order is implementation-
        // defined and can vary run-to-run, which would otherwise break the
        // "serialize -> deserialize -> serialize produces identical YAML"
        // round-trip guarantee). AssetHandle elements sort via an explicit u64
        // cast — UUID has no operator< of its own, only an implicit conversion to
        // u64, same reasoning as SceneWriteExpr's own AssetHandle cast below —
        // every other IsSetEligiblePropType element has a native operator<.
        if (f.isSet)
        {
            const std::string sorted = "sorted" + std::to_string(depth);
            out << indent << "{\n";
            out << indent << "    std::vector<decltype(" << access << ")::value_type> " << sorted << "("
                << access << ".begin(), " << access << ".end());\n";
            if (f.type == PropType::AssetHandle)
            {
                out << indent << "    std::ranges::sort(" << sorted
                    << ", [](auto const& lhs, auto const& rhs) { return static_cast<u64>(lhs) < static_cast<u64>(rhs); });\n";
            }
            else
            {
                out << indent << "    std::ranges::sort(" << sorted << ");\n";
            }
            out << indent << "    out << YAML::Key << \"" << f.key << "\" << YAML::Value << YAML::BeginSeq;\n";
            out << indent << "    for (auto const& " << ElemVar(depth) << " : " << sorted << ")\n";
            out << indent << "        out << " << SceneWriteExpr(f.type, ElemVar(depth)) << ";\n";
            out << indent << "    out << YAML::EndSeq;\n";
            out << indent << "}\n";
            continue;
        }
        // std::unordered_map<std::string, V> (issue #451 unordered_map/set slice)
        // — sorted by key for the same determinism reason as std::unordered_set
        // above. Emitted as a genuine YAML mapping (not a sequence of pairs) —
        // matches the on-disk shape of every hand-written string-keyed map in
        // this codebase (e.g. MorphTargetComponent::Weights).
        if (f.isMap)
        {
            const std::string keys = "keys" + std::to_string(depth);
            const std::string entry = "entry" + std::to_string(depth);
            const std::string k = "k" + std::to_string(depth);
            out << indent << "{\n";
            out << indent << "    std::vector<std::string> " << keys << ";\n";
            out << indent << "    " << keys << ".reserve(" << access << ".size());\n";
            out << indent << "    for (auto const& " << entry << " : " << access << ")\n";
            out << indent << "        " << keys << ".push_back(" << entry << ".first);\n";
            out << indent << "    std::ranges::sort(" << keys << ");\n";
            out << indent << "    out << YAML::Key << \"" << f.key << "\" << YAML::Value << YAML::BeginMap;\n";
            out << indent << "    for (auto const& " << k << " : " << keys << ")\n";
            out << indent << "        out << YAML::Key << " << k << " << YAML::Value << "
                << SceneWriteExpr(f.type, access + ".at(" + k + ")") << ";\n";
            out << indent << "    out << YAML::EndMap;\n";
            out << indent << "}\n";
            continue;
        }
        // std::vector<E> of a trivial element — a YAML sequence. Each element uses the
        // same per-value write expression (cast for handle/enum/small-int) as a scalar.
        if (f.isVector)
        {
            const std::string ev = ElemVar(depth);
            out << indent << "out << YAML::Key << \"" << f.key << "\" << YAML::Value << YAML::BeginSeq;\n";
            out << indent << "for (auto const& " << ev << " : " << access << ")\n";
            out << indent << "    out << " << SceneWriteExpr(f.type, ev) << ";\n";
            out << indent << "out << YAML::EndSeq;\n";
            continue;
        }
        // AssetHandle / UUID has an implicit operator u64() but YAML::Emitter has no
        // UUID overload, so emit the explicit u64 cast (matches every existing
        // hand-written AssetHandle block). Enum / u8 / u16 likewise need a cast (no /
        // wrong Emitter overload) — SceneWriteExpr encapsulates the per-type choice,
        // everything else streams directly via an existing Emitter<< overload.
        out << indent << "out << YAML::Key << \"" << f.key << "\" << YAML::Value << "
            << SceneWriteExpr(f.type, access) << ";\n";
    }
}

// Recursively emit the YAML reads for `fields` into a struct instance reached via
// `inst`, from the YAML node `node`. Mirrors EmitSerializeFields: a scalar nested
// struct reads from its sub-map; a std::vector<struct> rebuilds each element from a
// sub-map. Floats are finiteness-validated; a missing key keeps the constructor
// default (the instance was default-constructed by AddComponent / value_type{}).
static void EmitDeserializeFields(std::ostream& out, const std::vector<SerField>& fields,
                                  const std::string& inst, const std::string& node,
                                  const std::string& indent, int depth)
{
    for (auto const& f : fields)
    {
        const std::string lhs = inst + "." + f.member;
        const std::string key = node + "[\"" + f.key + "\"]";
        if (f.type == PropType::Struct)
        {
            if (f.isVector)
            {
                // Rebuild the sequence element-by-element. A missing / non-sequence
                // node keeps the constructor-default vector (.clear() is inside the
                // IsSequence guard). Each element is a fresh default-constructed
                // value_type populated from its sub-map, then appended.
                const std::string sv = SeqVar(depth);
                const std::string el = ElemVar(depth);
                const std::string tmp = "tmp" + std::to_string(depth);
                out << indent << "if (auto " << sv << " = " << key << "; " << sv << " && " << sv << ".IsSequence())\n";
                out << indent << "{\n";
                out << indent << "    " << lhs << ".clear();\n";
                out << indent << "    for (auto const& " << el << " : " << sv << ")\n";
                out << indent << "    {\n";
                out << indent << "        decltype(" << lhs << ")::value_type " << tmp << "{};\n";
                EmitDeserializeFields(out, f.subFields, tmp, el, indent + "        ", depth + 1);
                out << indent << "        " << lhs << ".push_back(" << tmp << ");\n";
                out << indent << "    }\n";
                out << indent << "}\n";
            }
            else
            {
                // Scalar nested struct — read its members from the sub-map if present.
                const std::string sn = "sub" + std::to_string(depth);
                out << indent << "if (auto " << sn << " = " << key << "; " << sn << ")\n";
                out << indent << "{\n";
                EmitDeserializeFields(out, f.subFields, lhs, sn, indent + "    ", depth + 1);
                out << indent << "}\n";
            }
            continue;
        }
        // Ref<T> (issue #451 Ref<T> slice) — resolve via AssetManager::GetAsset<T>. A
        // missing key or an unresolvable handle leaves the Ref at its constructor
        // default (null) — matches the hand-written MeshComponent::m_MeshSource
        // pattern, which never crashes on a stale/deleted asset reference.
        if (f.type == PropType::Ref)
        {
            const std::string handleKey = node + "[\"" + f.key + "Handle\"]";
            out << indent << "if (auto h" << depth << " = " << handleKey << "; h" << depth << ")\n";
            out << indent << "    " << lhs << " = AssetManager::GetAsset<" << f.refType << ">(h" << depth
                << ".as<u64>());\n";
            continue;
        }
        // std::unordered_set<E> (issue #451 unordered_map/set slice) — rebuild the
        // set element-by-element, same validation as std::vector<E> below (an
        // IsSetEligiblePropType element is never Float, so there is no
        // TryReadFiniteF32 branch here — every eligible type goes through the
        // Enum int-decode or the generic YAML::convert<> path).
        if (f.isSet)
        {
            const std::string sv = SeqVar(depth);
            const std::string el = ElemVar(depth);
            out << indent << "if (auto " << sv << " = " << key << "; " << sv << " && " << sv << ".IsSequence())\n";
            out << indent << "{\n";
            out << indent << "    " << lhs << ".clear();\n";
            out << indent << "    for (auto const& " << el << " : " << sv << ")\n";
            if (f.type == PropType::Enum)
            {
                out << indent << "        if (int ev; ::YAML::convert<int>::decode(" << el << ", ev))\n";
                out << indent << "            " << lhs << ".insert(static_cast<decltype(" << lhs
                    << ")::value_type>(ev));\n";
            }
            else
            {
                out << indent << "        if (decltype(" << lhs << ")::value_type v{}; ::YAML::convert<decltype("
                    << lhs << ")::value_type>::decode(" << el << ", v))\n";
                out << indent << "            " << lhs << ".insert(v);\n";
            }
            out << indent << "}\n";
            continue;
        }
        // std::unordered_map<std::string, V> (issue #451 unordered_map/set slice)
        // — rebuild the map entry-by-entry from the YAML mapping. A missing /
        // non-map node leaves the map at its default (.clear() is inside the
        // IsMap guard). Mirrors the vector-element decode (Float / Enum / generic
        // YAML::convert<>), reading each entry's key via entry.first and value via
        // entry.second (the standard yaml-cpp map-iteration shape already used by
        // the hand-written ItemInstance::CustomData blocks in this file).
        if (f.isMap)
        {
            const std::string mn = "mapNode" + std::to_string(depth);
            const std::string entry = "entry" + std::to_string(depth);
            out << indent << "if (auto " << mn << " = " << key << "; " << mn << " && " << mn << ".IsMap())\n";
            out << indent << "{\n";
            out << indent << "    " << lhs << ".clear();\n";
            out << indent << "    for (auto const& " << entry << " : " << mn << ")\n";
            out << indent << "    {\n";
            out << indent << "        const std::string k" << depth << " = " << entry << ".first.as<std::string>();\n";
            if (f.type == PropType::Float)
            {
                out << indent << "        if (f32 v; ::OloEngine::YAMLUtils::TryReadFiniteF32(" << entry
                    << ".second, v))\n";
                out << indent << "            " << lhs << "[k" << depth << "] = v;\n";
            }
            else if (f.type == PropType::Enum)
            {
                out << indent << "        if (int ev; ::YAML::convert<int>::decode(" << entry << ".second, ev))\n";
                out << indent << "            " << lhs << "[k" << depth << "] = static_cast<decltype(" << lhs
                    << ")::mapped_type>(ev);\n";
            }
            else
            {
                out << indent << "        if (decltype(" << lhs << ")::mapped_type v{}; ::YAML::convert<decltype("
                    << lhs << ")::mapped_type>::decode(" << entry << ".second, v))\n";
                out << indent << "            " << lhs << "[k" << depth << "] = v;\n";
            }
            out << indent << "    }\n";
            out << indent << "}\n";
            continue;
        }
        // std::vector<E> of a trivial element — rebuild the sequence element-by-element.
        // A missing key / non-sequence node leaves the vector at its default (.clear()
        // is inside the IsSequence guard, matching the scalar path's keep-default-on-
        // absent). Each element is validated: floats via TryReadFiniteF32, enums via an
        // int decode, everything else via the element type's YAML::convert<> (the glm
        // Decode helpers finiteness-validate); a malformed element is dropped, not
        // pushed as garbage. decltype(lhs)::value_type spells the element type without
        // tracking it textually (robust to nested enums / type aliases).
        if (f.isVector)
        {
            const std::string sv = SeqVar(depth);
            const std::string el = ElemVar(depth);
            out << indent << "if (auto " << sv << " = " << key << "; " << sv << " && " << sv << ".IsSequence())\n";
            out << indent << "{\n";
            out << indent << "    " << lhs << ".clear();\n";
            out << indent << "    for (auto const& " << el << " : " << sv << ")\n";
            if (f.type == PropType::Float)
            {
                out << indent << "        if (f32 v; ::OloEngine::YAMLUtils::TryReadFiniteF32(" << el << ", v))\n";
                out << indent << "            " << lhs << ".push_back(v);\n";
            }
            else if (f.type == PropType::Enum)
            {
                out << indent << "        if (int ev; ::YAML::convert<int>::decode(" << el << ", ev))\n";
                out << indent << "            " << lhs << ".push_back(static_cast<decltype(" << lhs
                    << ")::value_type>(ev));\n";
            }
            else
            {
                out << indent << "        if (decltype(" << lhs << ")::value_type v{}; ::YAML::convert<decltype("
                    << lhs << ")::value_type>::decode(" << el << ", v))\n";
                out << indent << "            " << lhs << ".push_back(v);\n";
            }
            out << indent << "}\n";
            continue;
        }
        switch (f.type)
        {
            case PropType::Float:
                // Validate finiteness; NaN/Inf or a missing key keeps the default. A
                // Clamp annotation (issue #451) additionally ranges a finite read value
                // — mirrors the hand-written SanitizeFloat idiom, which only clamps the
                // successfully-read value and otherwise leaves the (already in-range)
                // default alone.
                out << indent << "if (f32 v; ::OloEngine::YAMLUtils::TryReadFiniteF32(" << key << ", v))\n";
                out << indent << "    " << lhs << " = " << (f.hasClamp ? ApplyClamp("v", "f32", f) : "v") << ";\n";
                break;
            case PropType::Vec2:
                out << indent << lhs << " = " << key << ".as<glm::vec2>(" << lhs << ");\n";
                break;
            case PropType::Vec3:
                // A Clamp annotation (issue #451's vec3-Clamp slice) ranges each
                // component of the decoded value into [Min, Max] AFTER the finite-
                // validated read — mirrors the hand-written SanitizeVec3Clamped
                // idiom (component-wise fallback-then-clamp), applied here as a
                // separate clamp step on top of DecodeVec3's whole-vector fallback.
                out << indent << lhs << " = " << key << ".as<glm::vec3>(" << lhs << ");\n";
                if (f.hasClamp)
                    out << indent << lhs << " = " << ApplyVec3Clamp(lhs, f) << ";\n";
                break;
            case PropType::Vec4:
                out << indent << lhs << " = " << key << ".as<glm::vec4>(" << lhs << ");\n";
                break;
            case PropType::IVec2:
                out << indent << lhs << " = " << key << ".as<glm::ivec2>(" << lhs << ");\n";
                break;
            case PropType::IVec3:
                out << indent << lhs << " = " << key << ".as<glm::ivec3>(" << lhs << ");\n";
                break;
            case PropType::IVec4:
                out << indent << lhs << " = " << key << ".as<glm::ivec4>(" << lhs << ");\n";
                break;
            case PropType::Quat:
                // DecodeQuat finiteness-validates all four components; NaN/Inf or a
                // missing key keeps the default. Decode does NOT normalize.
                out << indent << lhs << " = " << key << ".as<glm::quat>(" << lhs << ");\n";
                break;
            case PropType::Mat3:
                out << indent << lhs << " = " << key << ".as<glm::mat3>(" << lhs << ");\n";
                break;
            case PropType::Mat4:
                out << indent << lhs << " = " << key << ".as<glm::mat4>(" << lhs << ");\n";
                break;
            case PropType::Bool:
                out << indent << lhs << " = " << key << ".as<bool>(" << lhs << ");\n";
                break;
            case PropType::Int:
                out << indent << lhs << " = "
                    << (f.hasClamp ? ApplyClamp(key + ".as<i32>(" + lhs + ")", "i32", f) : key + ".as<i32>(" + lhs + ")")
                    << ";\n";
                break;
            case PropType::UInt:
                out << indent << lhs << " = "
                    << (f.hasClamp ? ApplyClamp(key + ".as<u32>(" + lhs + ")", "u32", f) : key + ".as<u32>(" + lhs + ")")
                    << ";\n";
                break;
            case PropType::SmallUInt:
            case PropType::SmallInt:
            {
                // u8/u16/i8/i16: read via .as<decltype(member)> — yaml-cpp's char-type
                // decode special-case parses the numeric string and range-checks before
                // narrowing (an out-of-range / missing value keeps the default). Pairs
                // with the static_cast<u32>/<i32> emit. A Clamp annotation (issue #451)
                // casts its bounds to decltype(lhs) too, so the comparison stays within
                // the field's own (narrow) integer type.
                const std::string castT = "decltype(" + lhs + ")";
                const std::string readExpr = key + ".as<" + castT + ">(" + lhs + ")";
                out << indent << lhs << " = " << (f.hasClamp ? ApplyClamp(readExpr, castT, f) : readExpr) << ";\n";
                break;
            }
            case PropType::U64:
                out << indent << lhs << " = " << key << ".as<u64>(" << lhs << ");\n";
                break;
            case PropType::AssetHandle:
                // Read as u64, bridge back through the implicit UUID(u64) ctor; a
                // missing key keeps the constructor default (cast it to u64 for the
                // yaml-cpp fallback overload). Matches the hand-written blocks.
                out << indent << lhs << " = " << key << ".as<u64>(static_cast<u64>(" << lhs << "));\n";
                break;
            case PropType::Enum:
            {
                // Read as int, cast back to the member's own enum type via decltype —
                // robust to nested enums (e.g. Component::State) that aren't in scope by
                // their unqualified name at the SceneSerializer level. A missing key
                // keeps the default (cast it to int for the yaml-cpp fallback overload).
                // A Clamp annotation (issue #451) ranges the int value BEFORE the cast
                // back to the enum type — mirrors the hand-written
                // `static_cast<EnumType>(std::clamp(intValue, lo, hi))` idiom.
                const std::string readExpr = key + ".as<int>(static_cast<int>(" + lhs + "))";
                out << indent << lhs << " = static_cast<decltype(" << lhs << ")>("
                    << (f.hasClamp ? ApplyClamp(readExpr, "int", f) : readExpr) << ");\n";
                break;
            }
            case PropType::String:
                out << indent << lhs << " = " << key << ".as<std::string>(" << lhs << ");\n";
                break;
            default:
                break;
        }
    }
}

// SerializeEntity: `if (entity.HasComponent<T>()) { out << YAML::Key << "T"; … }`.
static void EmitSceneSerializeBlocks(std::ostream& out, const std::map<std::string, ComponentSerInfo>& comps)
{
    EmitSceneSerializerHeader(out, "serialize", "SerializeEntity", "`out` (YAML::Emitter&) and `entity` (Entity)");
    for (auto const& [name, info] : comps)
    {
        if (!info.trivial || kComponentsCustomSerialize.contains(name))
            continue;
        out << "if (entity.HasComponent<" << name << ">())\n";
        out << "{\n";
        out << "    out << YAML::Key << \"" << name << "\";\n";
        out << "    out << YAML::BeginMap; // " << name << "\n";
        out << "    auto const& comp = entity.GetComponent<" << name << ">();\n";
        EmitSerializeFields(out, info.fields, "comp", "    ", 0);
        out << "    out << YAML::EndMap; // " << name << "\n";
        out << "}\n\n";
    }
}

// DeserializeEntityComponents: `if (auto node = entity["T"]; node) { … }`.
static void EmitSceneDeserializeBlocks(std::ostream& out, const std::map<std::string, ComponentSerInfo>& comps)
{
    EmitSceneSerializerHeader(out, "deserialize", "DeserializeEntityComponents",
                              "`entity` (const YAML::Node&) and `deserializedEntity` (Entity&)");
    for (auto const& [name, info] : comps)
    {
        if (!info.trivial || kComponentsCustomSerialize.contains(name))
            continue;
        out << "if (auto node = entity[\"" << name << "\"]; node)\n";
        out << "{\n";
        out << "    auto& comp = deserializedEntity.AddComponent<" << name << ">();\n";
        EmitDeserializeFields(out, info.fields, "comp", "node", "    ", 0);
        out << "}\n\n";
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Binary scene-sidecar codegen (issue #525). Parallels the YAML emitters above
// but drives the .scenebin fast path: typed binary read/write for exactly the
// same auto-serializable component set. The classification (SerField trees) is
// reused verbatim — only the emit differs. Leaf values bottom out in the
// SceneBinIO::Write/Read overloads (Scene/SceneBinaryIO.h); container / nested-
// struct / Ref framing is emitted inline here, mirroring EmitSerializeFields.
// ════════════════════════════════════════════════════════════════════════════

// FNV-1a 32-bit of the component type name — the stable on-disk ComponentId used
// to tag each binary component block. Compiler-independent (unlike entt's
// type_hash), so sidecars are portable. MUST stay bit-identical to the constexpr
// SceneBinIO::ComponentId in Scene/SceneBinaryIO.h (same seed / prime).
static std::uint32_t SceneBinComponentId(const std::string& name)
{
    std::uint32_t hash = 2166136261u;
    for (char c : name)
    {
        hash ^= static_cast<unsigned char>(c);
        hash *= 16777619u;
    }
    return hash;
}

// A depth-suffixed local name, e.g. "bn0" / "btmp1", so nested container loops
// don't shadow each other (-Wshadow).
static std::string BinVar(const char* base, int depth)
{
    return std::string(base) + std::to_string(depth);
}

// Emit the post-read range clamp for a scalar / vec3 field carrying
// OLO_SERIALIZE(Clamp, …). Applied after the value has been read into `lhs`;
// reuses the same ApplyClamp / ApplyVec3Clamp assemblers as the YAML path.
static void EmitBinaryClamp(std::ostream& out, const std::string& indent, const std::string& lhs, const SerField& f)
{
    switch (f.type)
    {
        case PropType::Float:
            out << indent << lhs << " = " << ApplyClamp(lhs, "f32", f) << ";\n";
            break;
        case PropType::Int:
            out << indent << lhs << " = " << ApplyClamp(lhs, "i32", f) << ";\n";
            break;
        case PropType::UInt:
            out << indent << lhs << " = " << ApplyClamp(lhs, "u32", f) << ";\n";
            break;
        case PropType::SmallUInt:
        case PropType::SmallInt:
            out << indent << lhs << " = " << ApplyClamp(lhs, "decltype(" + lhs + ")", f) << ";\n";
            break;
        case PropType::Enum:
            out << indent << lhs << " = static_cast<decltype(" << lhs << ")>("
                << ApplyClamp("static_cast<int>(" + lhs + ")", "int", f) << ");\n";
            break;
        case PropType::Vec3:
            out << indent << lhs << " = " << ApplyVec3Clamp(lhs, f) << ";\n";
            break;
        default:
            break; // ParseComponentFields never sets hasClamp on other types
    }
}

// Recursively emit the binary WRITES for `fields` of a struct instance reached
// via `inst`. Mirrors EmitSerializeFields: nested structs write their members
// inline (no keys — fixed field order), a std::vector/std::unordered_set writes a
// u32 count then each element, a std::unordered_map<std::string,V> writes a u32
// count then sorted (key,value) pairs, a Ref<T> writes its asset handle as a u64.
static void EmitBinaryWriteFields(std::ostream& out, const std::vector<SerField>& fields,
                                  const std::string& inst, const std::string& indent, int depth)
{
    for (auto const& f : fields)
    {
        const std::string access = inst + "." + f.member;
        if (f.type == PropType::Struct)
        {
            if (f.isVector)
            {
                const std::string ev = BinVar("be", depth);
                out << indent << "SceneBinIO::WriteU32(out, static_cast<u32>(" << access << ".size()));\n";
                out << indent << "for (auto const& " << ev << " : " << access << ")\n";
                out << indent << "{\n";
                EmitBinaryWriteFields(out, f.subFields, ev, indent + "    ", depth + 1);
                out << indent << "}\n";
            }
            else
            {
                EmitBinaryWriteFields(out, f.subFields, access, indent, depth);
            }
            continue;
        }
        if (f.type == PropType::Ref)
        {
            out << indent << "SceneBinIO::WriteU64(out, (" << access << " && " << access
                << "->GetHandle() != 0) ? static_cast<u64>(" << access << "->GetHandle()) : 0ull);\n";
            continue;
        }
        if (f.isSet)
        {
            const std::string sorted = BinVar("bset", depth);
            const std::string ev = BinVar("be", depth);
            out << indent << "{\n";
            out << indent << "    std::vector<decltype(" << access << ")::value_type> " << sorted << "("
                << access << ".begin(), " << access << ".end());\n";
            if (f.type == PropType::AssetHandle)
                out << indent << "    std::ranges::sort(" << sorted
                    << ", [](auto const& lhs, auto const& rhs) { return static_cast<u64>(lhs) < static_cast<u64>(rhs); });\n";
            else
                out << indent << "    std::ranges::sort(" << sorted << ");\n";
            out << indent << "    SceneBinIO::WriteU32(out, static_cast<u32>(" << sorted << ".size()));\n";
            out << indent << "    for (auto const& " << ev << " : " << sorted << ")\n";
            out << indent << "        SceneBinIO::Write(out, " << ev << ");\n";
            out << indent << "}\n";
            continue;
        }
        if (f.isMap)
        {
            const std::string keys = BinVar("bkeys", depth);
            const std::string entry = BinVar("bentry", depth);
            const std::string k = BinVar("bk", depth);
            out << indent << "{\n";
            out << indent << "    std::vector<std::string> " << keys << ";\n";
            out << indent << "    " << keys << ".reserve(" << access << ".size());\n";
            out << indent << "    for (auto const& " << entry << " : " << access << ")\n";
            out << indent << "        " << keys << ".push_back(" << entry << ".first);\n";
            out << indent << "    std::ranges::sort(" << keys << ");\n";
            out << indent << "    SceneBinIO::WriteU32(out, static_cast<u32>(" << keys << ".size()));\n";
            out << indent << "    for (auto const& " << k << " : " << keys << ")\n";
            out << indent << "    {\n";
            out << indent << "        SceneBinIO::Write(out, " << k << ");\n";
            out << indent << "        SceneBinIO::Write(out, " << access << ".at(" << k << "));\n";
            out << indent << "    }\n";
            out << indent << "}\n";
            continue;
        }
        if (f.isVector)
        {
            const std::string ev = BinVar("be", depth);
            out << indent << "SceneBinIO::WriteU32(out, static_cast<u32>(" << access << ".size()));\n";
            out << indent << "for (auto const& " << ev << " : " << access << ")\n";
            out << indent << "    SceneBinIO::Write(out, " << ev << ");\n";
            continue;
        }
        // Scalar / string / glm / enum / AssetHandle leaf — the SceneBinIO::Write
        // overload set picks the encoding from the member's actual C++ type.
        out << indent << "SceneBinIO::Write(out, " << access << ");\n";
    }
}

// Recursively emit the binary READS for `fields` into a struct instance reached
// via `inst`, from the SceneBinIO::Reader `reader`. Any short read / bad value
// makes the enclosing Read function `return false` (the caller rolls the whole
// scene back and falls to YAML). Mirrors EmitBinaryWriteFields exactly.
static void EmitBinaryReadFields(std::ostream& out, const std::vector<SerField>& fields,
                                 const std::string& inst, const std::string& indent, int depth)
{
    for (auto const& f : fields)
    {
        const std::string lhs = inst + "." + f.member;
        if (f.type == PropType::Struct)
        {
            if (f.isVector)
            {
                const std::string n = BinVar("bn", depth);
                const std::string tmp = BinVar("btmp", depth);
                out << indent << "{\n";
                out << indent << "    u32 " << n << " = 0;\n";
                out << indent << "    if (!SceneBinIO::ReadU32(reader, " << n << ") || " << n
                    << " > SceneBinIO::MaxContainerElements) return false;\n";
                out << indent << "    " << lhs << ".clear();\n";
                out << indent << "    " << lhs << ".reserve(" << n << ");\n";
                out << indent << "    for (u32 bi" << depth << " = 0; bi" << depth << " < " << n << "; ++bi" << depth << ")\n";
                out << indent << "    {\n";
                out << indent << "        decltype(" << lhs << ")::value_type " << tmp << "{};\n";
                EmitBinaryReadFields(out, f.subFields, tmp, indent + "        ", depth + 1);
                out << indent << "        " << lhs << ".push_back(std::move(" << tmp << "));\n";
                out << indent << "    }\n";
                out << indent << "}\n";
            }
            else
            {
                EmitBinaryReadFields(out, f.subFields, lhs, indent, depth);
            }
            continue;
        }
        if (f.type == PropType::Ref)
        {
            const std::string h = BinVar("bh", depth);
            out << indent << "{\n";
            out << indent << "    u64 " << h << " = 0;\n";
            out << indent << "    if (!SceneBinIO::ReadU64(reader, " << h << ")) return false;\n";
            out << indent << "    if (" << h << " != 0) " << lhs << " = AssetManager::GetAsset<" << f.refType << ">(" << h << ");\n";
            out << indent << "}\n";
            continue;
        }
        if (f.isSet)
        {
            const std::string n = BinVar("bn", depth);
            const std::string v = BinVar("bv", depth);
            out << indent << "{\n";
            out << indent << "    u32 " << n << " = 0;\n";
            out << indent << "    if (!SceneBinIO::ReadU32(reader, " << n << ") || " << n
                << " > SceneBinIO::MaxContainerElements) return false;\n";
            out << indent << "    " << lhs << ".clear();\n";
            out << indent << "    for (u32 bi" << depth << " = 0; bi" << depth << " < " << n << "; ++bi" << depth << ")\n";
            out << indent << "    {\n";
            out << indent << "        decltype(" << lhs << ")::value_type " << v << "{};\n";
            out << indent << "        if (!SceneBinIO::Read(reader, " << v << ")) return false;\n";
            out << indent << "        " << lhs << ".insert(std::move(" << v << "));\n";
            out << indent << "    }\n";
            out << indent << "}\n";
            continue;
        }
        if (f.isMap)
        {
            const std::string n = BinVar("bn", depth);
            const std::string k = BinVar("bk", depth);
            const std::string v = BinVar("bv", depth);
            out << indent << "{\n";
            out << indent << "    u32 " << n << " = 0;\n";
            out << indent << "    if (!SceneBinIO::ReadU32(reader, " << n << ") || " << n
                << " > SceneBinIO::MaxContainerElements) return false;\n";
            out << indent << "    " << lhs << ".clear();\n";
            out << indent << "    for (u32 bi" << depth << " = 0; bi" << depth << " < " << n << "; ++bi" << depth << ")\n";
            out << indent << "    {\n";
            out << indent << "        std::string " << k << ";\n";
            out << indent << "        if (!SceneBinIO::Read(reader, " << k << ")) return false;\n";
            out << indent << "        decltype(" << lhs << ")::mapped_type " << v << "{};\n";
            out << indent << "        if (!SceneBinIO::Read(reader, " << v << ")) return false;\n";
            out << indent << "        " << lhs << "[std::move(" << k << ")] = std::move(" << v << ");\n";
            out << indent << "    }\n";
            out << indent << "}\n";
            continue;
        }
        if (f.isVector)
        {
            const std::string n = BinVar("bn", depth);
            const std::string v = BinVar("bv", depth);
            out << indent << "{\n";
            out << indent << "    u32 " << n << " = 0;\n";
            out << indent << "    if (!SceneBinIO::ReadU32(reader, " << n << ") || " << n
                << " > SceneBinIO::MaxContainerElements) return false;\n";
            out << indent << "    " << lhs << ".clear();\n";
            out << indent << "    " << lhs << ".reserve(" << n << ");\n";
            out << indent << "    for (u32 bi" << depth << " = 0; bi" << depth << " < " << n << "; ++bi" << depth << ")\n";
            out << indent << "    {\n";
            out << indent << "        decltype(" << lhs << ")::value_type " << v << "{};\n";
            out << indent << "        if (!SceneBinIO::Read(reader, " << v << ")) return false;\n";
            out << indent << "        " << lhs << ".push_back(std::move(" << v << "));\n";
            out << indent << "    }\n";
            out << indent << "}\n";
            continue;
        }
        // Scalar / string / glm / enum / AssetHandle leaf.
        out << indent << "if (!SceneBinIO::Read(reader, " << lhs << ")) return false;\n";
        if (f.hasClamp)
            EmitBinaryClamp(out, indent, lhs, f);
    }
}

static void EmitSceneBinaryHeader(std::ostream& out, const char* verb, const char* locals)
{
    out << "// Auto-generated by OloHeaderTool — DO NOT EDIT MANUALLY\n";
    out << "// Re-generate with: cmake --build build --target GenerateBindings\n";
    out << "//\n";
    out << "// Per-component .scenebin binary " << verb << " blocks (issue #525) for the same\n";
    out << "// auto-serializable component set as the YAML SceneSerializer codegen. Leaf\n";
    out << "// values go through the SceneBinIO::Write/Read overloads; each block is tagged\n";
    out << "// with a stable FNV-1a-32 ComponentId of the type name.\n";
    out << "//\n";
    out << "// #include'd where " << locals << " are in scope.\n\n";
}

// SceneBinaryWriteComponents.Generated.inl — `if (entity.HasComponent<T>()) {
// WriteU32(id); <writes>; }` blocks, appended after the hand-written Transform block.
static void EmitSceneBinaryWriteBlocks(std::ostream& out, const std::map<std::string, ComponentSerInfo>& comps)
{
    EmitSceneBinaryHeader(out, "write", "`out` (std::ostream&) and `entity` (Entity)");
    for (auto const& [name, info] : comps)
    {
        if (!info.trivial || kComponentsCustomSerialize.contains(name))
            continue;
        out << "if (entity.HasComponent<" << name << ">())\n";
        out << "{\n";
        out << "    SceneBinIO::WriteU32(out, " << SceneBinComponentId(name) << "u); // " << name << "\n";
        out << "    auto const& comp = entity.GetComponent<" << name << ">();\n";
        EmitBinaryWriteFields(out, info.fields, "comp", "    ", 0);
        out << "}\n\n";
    }
}

// SceneBinaryReadComponents.Generated.inl — `case id: { comp = AddComponent<T>();
// <reads>; break; }` cases, spliced into the component-id switch.
static void EmitSceneBinaryReadBlocks(std::ostream& out, const std::map<std::string, ComponentSerInfo>& comps)
{
    EmitSceneBinaryHeader(out, "read", "`reader` (SceneBinIO::Reader&) and `deserializedEntity` (Entity&)");
    for (auto const& [name, info] : comps)
    {
        if (!info.trivial || kComponentsCustomSerialize.contains(name))
            continue;
        out << "case " << SceneBinComponentId(name) << "u: // " << name << "\n";
        out << "{\n";
        out << "    auto& comp = deserializedEntity.AddComponent<" << name << ">();\n";
        EmitBinaryReadFields(out, info.fields, "comp", "    ", 0);
        out << "    break;\n";
        out << "}\n";
    }
}

// SceneBinaryCoveredComponents.Generated.inl — inserts each covered component's
// entt::type_hash into the runtime `ids` set used by the per-entity
// representability check (is every component of this entity binary-covered?).
static void EmitSceneBinaryCoveredIds(std::ostream& out, const std::map<std::string, ComponentSerInfo>& comps)
{
    out << "// Auto-generated by OloHeaderTool — DO NOT EDIT MANUALLY\n";
    out << "// Re-generate with: cmake --build build --target GenerateBindings\n";
    out << "//\n";
    out << "// Runtime entt::type_hash of every binary-covered component (issue #525),\n";
    out << "// inserted into the `ids` set. #include'd where `ids`\n";
    out << "// (std::unordered_set<entt::id_type>&) is in scope.\n\n";
    for (auto const& [name, info] : comps)
    {
        if (!info.trivial || kComponentsCustomSerialize.contains(name))
            continue;
        out << "ids.insert(entt::type_hash<" << name << ">::value());\n";
    }
}

// ════════════════════════════════════════════════════════════════════════════
// MCP generic-field-write registry codegen (issue #607)
//
// The editor's MCP diagnostics server exposes `olo_entity_set_field`, an
// undoable, UUID-keyed write over a (component, field) -> typed-setter registry
// (OloEditor/src/MCP/McpGenericFieldWrite.h). That registry used to be a
// hand-written list of 9 components — exactly the sort of hand-maintained
// touch-point that silently rots as components are added. It is now generated
// from the SAME full data-member scan (CollectComponentFields) that drives the
// scene serializer, so every component whose fields are practically editable is
// reachable from MCP the moment it compiles.
//
// The parser already does the hard, fail-safe part for us:
//   * a NON-PUBLIC member is never collected (it only marks the component
//     non-trivial), so the generated `&Comp::Member` always compiles;
//   * an OLO_SERIALIZE(Skip) member (runtime-only state) is dropped entirely;
//   * an unrecognised / non-trivial member type is skipped.
// Crucially, `ComponentSerInfo::fields` is populated even for a NON-trivial
// component — `trivial` only says "every member was recognised". So a component
// the scene serializer keeps hand-written (Rigidbody3DComponent, MeshComponent,
// …) still contributes its recognised public fields here, which is exactly what
// a debugging agent reaches for.
// ════════════════════════════════════════════════════════════════════════════

// Components deliberately NOT exposed to `olo_entity_set_field`: per-tick
// runtime-derived state that is recomputed every frame (writing it from MCP is
// meaningless at best, corrupting at worst) and entity identity (the UUID is the
// addressing key — rewriting it would break the very handle used to address the
// entity). This mirrors kComponentsNotInTuple minus TagComponent, which IS a
// legitimate editable label. Guarded by McpFieldRegistryTest.
static const std::set<std::string> kComponentsNotMcpEditable = {
    "IDComponent",
    "WorldTransformComponent",
    // AnimationStateComponent is in the scene-copy tuple and IS serialized, but every
    // field is playback state the animation system rewrites each tick (current time,
    // blend factor, clip index): an MCP write is overwritten on the next frame at best
    // and desyncs the blend at worst. Drive animation through the graph/state machine,
    // not by poking this.
    "AnimationStateComponent",
    "UIResolvedRectComponent",
    "DialogueStateComponent",
    "SpringBoneStateComponent",
    "NoiseAnimationStateComponent",
    "RetargetingStateComponent",
    "FootIKStateComponent",
    "LocomotionStateComponent",
};

// Ranges the SceneSerializer's HAND-WRITTEN deserialize enforces but which are
// not expressible as an OLO_SERIALIZE(Clamp) annotation yet (the component is in
// kComponentsCustomSerialize, so its annotations would be ignored by the
// serializer codegen — the range lives in the hand-written block instead). An MCP
// write that bypassed these would put the component into a state a scene load
// could never produce, so they are re-declared here, keyed "Component.Field".
//
// ONLY inclusive numeric bounds that are literally present in the hand-written
// deserialize are listed — each entry is traceable to a line in
// SceneSerializer.cpp::DeserializeEntityComponents. Strict `> 0` guards (e.g.
// LightProbeVolumeComponent::Spacing, ReflectionProbeComponent::InfluenceRadius)
// are deliberately NOT approximated with an invented epsilon; they stay
// unclamped here and are called out in docs/guides/mcp-diagnostics-server.md.
// The real fix — and the follow-up this leaves behind — is to migrate those
// hand-written clamps onto OLO_SERIALIZE(Clamp, Min=…, Max=…), after which MCP
// inherits them for free with no entry in this table.
struct McpRange
{
    std::optional<std::string> min;
    std::optional<std::string> max;
};
static const std::map<std::string, McpRange> kMcpFieldClamps = {
    // SphereAreaLightComponent — deserialize REJECTS a negative value (keeps the
    // default); MCP clamps to the same floor rather than silently accepting it.
    { "SphereAreaLightComponent.Intensity", { std::string("0.0f"), std::nullopt } },
    { "SphereAreaLightComponent.Radius", { std::string("0.0f"), std::nullopt } },
    { "SphereAreaLightComponent.Range", { std::string("0.0f"), std::nullopt } },
    // ReflectionProbeComponent — `std::clamp(m_Resolution, 16u, 2048u)` + `< 0.0f`
    // rejects on Intensity / BlendDistance.
    { "ReflectionProbeComponent.Resolution", { std::string("16u"), std::string("2048u") } },
    { "ReflectionProbeComponent.Intensity", { std::string("0.0f"), std::nullopt } },
    { "ReflectionProbeComponent.BlendDistance", { std::string("0.0f"), std::nullopt } },
    // LightProbeVolumeComponent — `< 0.0f` reject on Intensity.
    { "LightProbeVolumeComponent.Intensity", { std::string("0.0f"), std::nullopt } },
    // StreamingVolumeComponent — `std::max(LoadRadius, 1.0f)`.
    { "StreamingVolumeComponent.LoadRadius", { std::string("1.0f"), std::nullopt } },
};

// The field types the MCP write tool can coerce from JSON and echo back
// (McpGenericFieldWrite.h's CoerceJson / FieldToJson / FieldChanged). Everything
// else — a plain u64, an integer-vector / quaternion / matrix, a nested struct, a
// Ref<T>, and every container (vector / set / map) — has no scalar JSON shape in
// the tool's contract and is skipped. Skipping is safe and silent: the field is
// simply not writable, and `olo_entity_list_fields` shows exactly what is.
static bool IsMcpEditableField(const SerField& f)
{
    if (f.isVector || f.isSet || f.isMap)
        return false;
    switch (f.type)
    {
        case PropType::Bool:
        case PropType::Int:
        case PropType::UInt:
        case PropType::SmallInt:
        case PropType::SmallUInt:
        case PropType::Float:
        case PropType::Vec2:
        case PropType::Vec3:
        case PropType::Vec4:
        case PropType::String:
        case PropType::AssetHandle:
        case PropType::Enum:
            return true;
        default:
            return false;
    }
}

// Whether a range (clamp) can be applied to this field type by the MCP tool —
// the numeric types plus the float vectors (clamped component-wise), mirroring
// the serializer's own kClampEligible set (scalars + glm::vec3) but widened to
// vec2/vec4 since the same component-wise clamp is well-defined there.
static bool IsMcpRangeableField(const SerField& f)
{
    switch (f.type)
    {
        case PropType::Int:
        case PropType::UInt:
        case PropType::SmallInt:
        case PropType::SmallUInt:
        case PropType::Float:
        case PropType::Enum:
        case PropType::Vec2:
        case PropType::Vec3:
        case PropType::Vec4:
            return true;
        default:
            return false;
    }
}

static std::string McpBound(const std::optional<std::string>& expr)
{
    return expr ? ("OLO_GFW_BOUND(" + *expr + ")") : std::string("OLO_GFW_NO_BOUND");
}

// How deep the MCP emitter follows a nested-record chain. `System.Emitter.Shape.X`
// is depth 3; nothing real needs more, and the cap keeps a pathological type graph
// (or a leaf-name collision in the record registry) from exploding the registry.
constexpr int kMcpMaxNestingDepth = 4;

// Recursively emit the registry entries for one component's field list. A
// PropType::Struct member — trivial (LODGroupComponent's LODGroup) or PARTIAL
// (ParticleSystemComponent's ParticleSystem) — is DESCENDED into rather than
// skipped: its public, JSON-coercible leaves become dotted fields addressed by a
// member-access chain (`System.Emitter.RateOverTime`), which the OLO_GFW_FIELD macro
// turns into an accessor lambda. Containers (vector / set / map) are not descended:
// a dotted path cannot address an element, and there is no static field name for one.
static void EmitMcpFieldsRecursive(std::ostream& out, const std::string& component,
                                   const std::vector<SerField>& fields,
                                   const std::string& keyPrefix, const std::string& memberPrefix,
                                   int depth, std::size_t& emitted);

// Emit the registry body #include'd inside GenericFieldWrite::BuildRegistry().
// Returns the number of (component, field) pairs emitted.
static std::size_t EmitMcpFieldRegistry(std::ostream& out, const std::map<std::string, ComponentSerInfo>& comps)
{
    out << "// Auto-generated by OloHeaderTool — DO NOT EDIT MANUALLY\n";
    out << "// Re-generate with: cmake --build build --target GenerateBindings\n";
    out << "//\n";
    out << "// The MCP `olo_entity_set_field` / `olo_entity_list_fields` field registry\n";
    out << "// (issue #607). One entry per public, JSON-coercible data member of every\n";
    out << "// `struct *Component` under OloEngine/src, minus the runtime-only components\n";
    out << "// in the generator's kComponentsNotMcpEditable set and minus every field the\n";
    out << "// shared field scan drops (non-public, OLO_SERIALIZE(Skip), or a type with no\n";
    out << "// scalar JSON shape: u64 / ivec / quat / mat / struct / Ref<T> / container).\n";
    out << "//\n";
    out << "// A ranged entry (OLO_GFW_FIELD_RANGE) carries the SAME bounds the scene\n";
    out << "// serializer enforces on load — from the field's OLO_SERIALIZE(Clamp, …)\n";
    out << "// annotation, or (for a component the serializer keeps hand-written) from the\n";
    out << "// generator's kMcpFieldClamps table. A write outside the range is CLAMPED and\n";
    out << "// reported as `clamped: true` with the original `requestedValue`.\n";
    out << "//\n";
    out << "// #include'd inside GenericFieldWrite::BuildRegistry(), where `registry`\n";
    out << "// (std::vector<FieldEntry>&) and the OLO_GFW_* macros are in scope.\n\n";

    std::size_t emitted = 0;
    for (auto const& [name, info] : comps)
    {
        if (kComponentsNotMcpEditable.contains(name))
            continue;

        std::ostringstream body;
        std::size_t before = emitted;
        EmitMcpFieldsRecursive(body, name, info.fields, "", "", 0, emitted);
        if (emitted == before)
            continue; // no writable leaf anywhere in this component

        out << "// " << name << "\n"
            << body.str() << "\n";
    }
    return emitted;
}

static void EmitMcpFieldsRecursive(std::ostream& out, const std::string& component,
                                   const std::vector<SerField>& fields,
                                   const std::string& keyPrefix, const std::string& memberPrefix,
                                   int depth, std::size_t& emitted)
{
    for (const SerField& f : fields)
    {
        if (f.isVector || f.isSet || f.isMap)
            continue; // no static field name for a container element

        if (f.type == PropType::Struct)
        {
            if (depth + 1 >= kMcpMaxNestingDepth)
                continue;
            EmitMcpFieldsRecursive(out, component, f.subFields, keyPrefix + f.key + ".",
                                   memberPrefix + f.member + ".", depth + 1, emitted);
            continue;
        }
        if (!IsMcpEditableField(f))
            continue;

        const std::string key = keyPrefix + f.key;

        McpRange range;
        if (f.hasClamp && IsMcpRangeableField(f))
            range = McpRange{ f.clampMin, f.clampMax };
        // kMcpFieldClamps is keyed by the FULL dotted field name, so a hand-written
        // serializer's clamp on a nested field would be spelled "Comp.Sub.Field".
        if (auto it = kMcpFieldClamps.find(component + "." + key);
            it != kMcpFieldClamps.end() && IsMcpRangeableField(f))
            range = it->second;

        if (range.min || range.max)
        {
            out << "registry.push_back(OLO_GFW_FIELD_RANGE(" << component << ", \"" << key << "\", "
                << memberPrefix << f.member << ", " << McpBound(range.min) << ", " << McpBound(range.max) << "));\n";
        }
        else
        {
            out << "registry.push_back(OLO_GFW_FIELD(" << component << ", \"" << key << "\", "
                << memberPrefix << f.member << "));\n";
        }
        ++emitted;
    }
}

// ─── MCP setter-based field registry (OLO_PROPERTY reuse) ───────────────────────
//
// AudioSourceComponent slice (issue #607). Some components keep their authored
// data behind a PRIVATE member reached only through OLO_PROPERTY's custom Get/Set
// expressions — AudioSourceComponent's 16 parameters all live behind `private
// std::unique_ptr<AudioSourceColdData> m_Cold` (see Components.h). Those fields are
// invisible to CollectComponentFields/ParseComponentFields (a non-public member
// only marks the component non-trivial and is dropped) and unsound to write through
// EmitMcpFieldsRecursive's MakeFieldAccess path, which copies the whole component,
// mutates the copy, and swaps it in via ComponentChangeCommand<C>:
// AudioSourceComponent::operator= resets ActiveEventID to 0 and never re-invokes
// Source->SetVolume()/SetPitch()/etc, so that path would silently detach a playing
// sound on every write. This emitter reuses the ALREADY-COLLECTED OLO_PROPERTY scan
// (the same `components` ParseHeaders produces for EmitCppBindings/scripting) to
// generate MakeSetterField<C, F> registrations instead, which apply the Get/Set
// expression pair DIRECTLY on the live component (see McpGenericFieldWrite.h's
// MakeSetterField doc comment).
//
// Deliberately an ALLOWLIST, not "every OLO_PROPERTY component": most components
// with an OLO_PROPERTY custom accessor ALSO have the field reachable as a plain
// public (nested-)struct member, already covered by EmitMcpFieldRegistry's
// MakeFieldAccess path — routing those through MakeSetterField too would just
// double-register the same (Component, Field) pair (Find() would still resolve
// correctly, since it returns the first match, but the second entry is dead weight
// and a future edit could make the two silently disagree). Extend this set only for
// a component whose OLO_PROPERTY fields are hidden behind a private member the same
// way AudioSourceComponent's are — MorphTargetComponent's map-keyed Weights is the
// next known candidate (issue #607 follow-up) once map-key addressing exists; adding
// it here today would just generate field names for keys that don't exist until a
// MorphTargetSet is bound at runtime.
static const std::set<std::string> kOloPropertySetterMcpComponents = {
    "AudioSourceComponent",
};

// The C++ type MakeSetterField<C, F> should use for a PropType from the OLO_PROPERTY
// scan. U64 maps to AssetHandle (== UUID), NOT a raw 64-bit integer:
// McpGenericFieldWrite.h's CoerceJson/FieldToJson only support a 64-bit field via the
// UUID specialisation (a decimal-digit JSON string, matching entity-UUID's own JSON
// contract — a raw u64 would exceed JSON's safe-integer range). Every current
// OLO_PROPERTY(Type = "ulong") property IS an asset handle (AudioSourceComponent's
// SoundConfigHandle converts through AssetHandle(u64)/static_cast<u64> in its Get/Set
// expressions already); this emitter is scoped to a small allowlist (see above), so
// that assumption doesn't need re-deriving from the property's Get/Set text.
// PropType::String is intentionally unmapped here: no allowlisted component has a
// private OLO_PROPERTY string today, and MakeSetterField's std::string-by-const-ref
// getFn/setFn signature would need its own (untested) codegen path — add it when a
// real property needs it, not speculatively.
static std::string McpSetterCppType(PropType t)
{
    switch (t)
    {
        case PropType::Float:
            return "float";
        case PropType::Bool:
            return "bool";
        case PropType::Int:
            return "int";
        case PropType::UInt:
            return "unsigned int";
        case PropType::U64:
            return "AssetHandle";
        case PropType::Vec2:
            return "glm::vec2";
        case PropType::Vec3:
            return "glm::vec3";
        case PropType::Vec4:
            return "glm::vec4";
        default:
            return "";
    }
}

// Emit the setter-based MCP registry entries (kOloPropertySetterMcpComponents
// above), appended to the same McpFieldRegistry.Generated.inl body as
// EmitMcpFieldRegistry's plain-field entries. `fieldScan` is CollectComponentFields'
// output, consulted ONLY for dedup: a property whose script name is ALSO an
// MCP-editable field there (the plain member/nested-member scan already reaches it)
// is skipped here rather than double-registered. Returns the number of (component,
// field) pairs emitted, added to EmitMcpFieldRegistry's own count by the caller.
static std::size_t EmitMcpSetterFields(std::ostream& out, const std::vector<ComponentDef>& components,
                                       const std::map<std::string, ComponentSerInfo>& fieldScan)
{
    std::size_t emitted = 0;
    for (auto const& comp : components)
    {
        if (!kOloPropertySetterMcpComponents.contains(comp.name))
            continue;
        if (kComponentsNotMcpEditable.contains(comp.name))
            continue;

        const ComponentSerInfo* scanned = nullptr;
        if (auto it = fieldScan.find(comp.name); it != fieldScan.end())
            scanned = &it->second;

        std::ostringstream body;
        const std::size_t before = emitted;
        for (auto const& prop : comp.properties)
        {
            // Dedup: already reachable via the plain field scan — don't register twice.
            if (scanned != nullptr &&
                std::any_of(scanned->fields.begin(), scanned->fields.end(), [&](const SerField& f)
                            { return f.key == prop.scriptName && IsMcpEditableField(f); }))
            {
                continue;
            }

            const std::string cppType = McpSetterCppType(prop.type);
            if (cppType.empty())
            {
                std::cerr << "WARNING: OloHeaderTool MCP setter emitter: " << comp.name << "::"
                          << prop.scriptName << " has a type not yet supported by MakeSetterField — skipped\n";
                continue;
            }

            // Same {v} substitution EmitCppBindings uses for the scripting glue, so the
            // MCP write path and the Lua/C# path execute IDENTICAL setter logic.
            const std::string getExpr = prop.customGet.empty() ? ("comp." + prop.cppField) : prop.customGet;
            const std::string setExpr = ReplaceAll(
                prop.customSet.empty() ? ("comp." + prop.cppField + " = {v}") : prop.customSet, "{v}", "v");

            body << "registry.push_back(MakeSetterField<" << comp.name << ", " << cppType << ">(\n";
            body << "    \"" << comp.name << "\", \"" << prop.scriptName << "\",\n";
            body << "    [](const " << comp.name << "& comp) -> " << cppType << " { return (" << getExpr << "); },\n";
            body << "    [](" << comp.name << "& comp, const " << cppType << "& v)\n";
            body << "    {\n";
            EmitStatements(body, "        ", setExpr);
            body << "    }));\n";
            ++emitted;
        }
        if (emitted == before)
            continue;
        out << "// " << comp.name << " (OLO_PROPERTY setter-based — private cold-data fields)\n"
            << body.str() << "\n";
    }
    return emitted;
}

// ─── C++ Mono Bindings Emitter ──────────────────────────────────────────────────

static void EmitCppBindings(std::ostream& out, const std::vector<ComponentDef>& components)
{
    out << "// Auto-generated by OloHeaderTool — DO NOT EDIT MANUALLY\n";
    out << "// Re-generate with: cmake --build build --target GenerateBindings\n\n";

    for (auto const& comp : components)
    {
        out << "///////////////////////////////////////////////////////////////////////////////////////////\n";
        out << "// " << comp.name << std::string(std::max(0, 85 - 7 - static_cast<int>(comp.name.size())), ' ')
            << " //\n";
        out << "///////////////////////////////////////////////////////////////////////////////////////////\n\n";

        for (auto const& prop : comp.properties)
        {
            std::string funcBase = comp.name + "_Get" + prop.scriptName;
            std::string funcSet = comp.name + "_Set" + prop.scriptName;
            bool scalar = IsScalar(prop.type);
            bool isString = prop.type == PropType::String;

            // --- Getter ---
            if (isString)
            {
                out << "static MonoString* " << funcBase << "(UUID entityID)\n";
                out << "{\n";
                out << "    Scene* scene = ScriptEngine::GetSceneContext();\n";
                out << "    OLO_CORE_ASSERT(scene);\n";
                out << "    Entity entity = scene->GetEntityByUUID(entityID);\n";
                out << "    OLO_CORE_ASSERT(entity);\n";
                out << "    auto& comp = entity.GetComponent<" << comp.name << ">();\n";

                if (prop.customGet.empty())
                    out << "    return ScriptEngine::CreateString(comp." << prop.cppField << ".c_str());\n";
                else
                    out << "    return ScriptEngine::CreateString((" << prop.customGet << ").c_str());\n";

                out << "}\n\n";
            }
            else if (scalar)
            {
                out << "static " << CppReturnType(prop.type) << " " << funcBase << "(UUID entityID)\n";
                out << "{\n";
                out << "    Scene* scene = ScriptEngine::GetSceneContext();\n";
                out << "    OLO_CORE_ASSERT(scene);\n";
                out << "    Entity entity = scene->GetEntityByUUID(entityID);\n";
                out << "    OLO_CORE_ASSERT(entity);\n";
                out << "    auto& comp = entity.GetComponent<" << comp.name << ">();\n";

                if (prop.customGet.empty())
                    out << "    return comp." << prop.cppField << ";\n";
                else
                    out << "    return " << prop.customGet << ";\n";

                out << "}\n\n";
            }
            else
            {
                std::string glm = GlmType(prop.type);
                out << "static void " << funcBase << "(UUID entityID, " << glm << "* outValue)\n";
                out << "{\n";
                out << "    Scene* scene = ScriptEngine::GetSceneContext();\n";
                out << "    OLO_CORE_ASSERT(scene);\n";
                out << "    Entity entity = scene->GetEntityByUUID(entityID);\n";
                out << "    OLO_CORE_ASSERT(entity);\n";
                out << "    auto& comp = entity.GetComponent<" << comp.name << ">();\n";

                if (prop.customGet.empty())
                    out << "    *outValue = comp." << prop.cppField << ";\n";
                else
                    out << "    *outValue = " << prop.customGet << ";\n";

                out << "}\n\n";
            }

            // --- Setter ---
            if (isString)
            {
                out << "static void " << funcSet << "(UUID entityID, MonoString* value)\n";
                out << "{\n";
                out << "    Scene* scene = ScriptEngine::GetSceneContext();\n";
                out << "    OLO_CORE_ASSERT(scene);\n";
                out << "    Entity entity = scene->GetEntityByUUID(entityID);\n";
                out << "    OLO_CORE_ASSERT(entity);\n";
                out << "    if (!value)\n";
                out << "        return;\n";
                out << "    auto& comp = entity.GetComponent<" << comp.name << ">();\n";

                if (prop.customSet.empty())
                    out << "    comp." << prop.cppField << " = Utils::MonoStringToString(value);\n";
                else
                {
                    out << "    std::string converted = Utils::MonoStringToString(value);\n";
                    std::string expr = ReplaceAll(prop.customSet, "{v}", "converted");
                    EmitStatements(out, "    ", expr);
                }

                out << "}\n\n";
            }
            else if (scalar)
            {
                out << "static void " << funcSet << "(UUID entityID, " << CppReturnType(prop.type) << " value)\n";
                out << "{\n";
                out << "    Scene* scene = ScriptEngine::GetSceneContext();\n";
                out << "    OLO_CORE_ASSERT(scene);\n";
                out << "    Entity entity = scene->GetEntityByUUID(entityID);\n";
                out << "    OLO_CORE_ASSERT(entity);\n";

                // Guard against NaN/Inf for floating-point scalars
                if (prop.type == PropType::Float)
                {
                    out << "    if (!std::isfinite(value))\n";
                    out << "        return;\n";
                }

                out << "    auto& comp = entity.GetComponent<" << comp.name << ">();\n";

                if (prop.customSet.empty())
                    out << "    comp." << prop.cppField << " = value;\n";
                else
                {
                    std::string expr = ReplaceAll(prop.customSet, "{v}", "value");
                    EmitStatements(out, "    ", expr);
                }

                out << "}\n\n";
            }
            else
            {
                std::string glm = GlmType(prop.type);
                out << "static void " << funcSet << "(UUID entityID, " << glm << " const* value)\n";
                out << "{\n";
                out << "    Scene* scene = ScriptEngine::GetSceneContext();\n";
                out << "    OLO_CORE_ASSERT(scene);\n";
                out << "    Entity entity = scene->GetEntityByUUID(entityID);\n";
                out << "    OLO_CORE_ASSERT(entity);\n";
                out << "    auto& comp = entity.GetComponent<" << comp.name << ">();\n";

                // Guard against NaN/Inf in any vector component
                out << "    for (glm::length_t i = 0; i < value->length(); ++i)\n";
                out << "        if (!std::isfinite((*value)[i]))\n";
                out << "            return;\n";

                if (prop.customSet.empty())
                    out << "    comp." << prop.cppField << " = *value;\n";
                else
                {
                    std::string expr = ReplaceAll(prop.customSet, "{v}", "*value");
                    EmitStatements(out, "    ", expr);
                }

                out << "}\n\n";
            }
        }
    }
}

// ─── C++ Registrations Emitter ──────────────────────────────────────────────────

static void EmitCppRegistrations(std::ostream& out, const std::vector<ComponentDef>& components)
{
    out << "// Auto-generated by OloHeaderTool — DO NOT EDIT MANUALLY\n\n";

    for (auto const& comp : components)
    {
        out << "// " << comp.name << "\n";
        for (auto const& prop : comp.properties)
        {
            out << "OLO_ADD_INTERNAL_CALL(" << comp.name << "_Get" << prop.scriptName << ");\n";
            out << "OLO_ADD_INTERNAL_CALL(" << comp.name << "_Set" << prop.scriptName << ");\n";
        }
        out << "\n";
    }
}

// ─── C# Components Emitter ─────────────────────────────────────────────────────

static void EmitCsComponents(std::ostream& out, const std::vector<ComponentDef>& components)
{
    out << "// Auto-generated by OloHeaderTool — DO NOT EDIT MANUALLY\n\n";
    out << "namespace OloEngine\n{\n";

    for (auto const& comp : components)
    {
        out << "\tpublic partial class " << comp.name << " : Component\n";
        out << "\t{\n";

        for (size_t i = 0; i < comp.properties.size(); ++i)
        {
            auto const& prop = comp.properties[i];
            std::string cs = CsType(prop.type);

            if (bool scalar = IsScalar(prop.type); scalar || prop.type == PropType::String)
            {
                out << "\t\tpublic " << cs << " " << prop.scriptName << "\n";
                out << "\t\t{\n";
                out << "\t\t\tget => InternalCalls." << comp.name << "_Get" << prop.scriptName << "(Entity.ID);\n";
                out << "\t\t\tset => InternalCalls." << comp.name << "_Set" << prop.scriptName << "(Entity.ID, value);\n";
                out << "\t\t}\n";
            }
            else
            {
                out << "\t\tpublic " << cs << " " << prop.scriptName << "\n";
                out << "\t\t{\n";
                out << "\t\t\tget\n";
                out << "\t\t\t{\n";
                out << "\t\t\t\tInternalCalls." << comp.name << "_Get" << prop.scriptName
                    << "(Entity.ID, out " << cs << " value);\n";
                out << "\t\t\t\treturn value;\n";
                out << "\t\t\t}\n";
                out << "\t\t\tset => InternalCalls." << comp.name << "_Set" << prop.scriptName
                    << "(Entity.ID, ref value);\n";
                out << "\t\t}\n";
            }

            if (i + 1 < comp.properties.size())
                out << "\n";
        }

        out << "\t}\n\n";
    }

    out << "}\n";
}

// ─── C# InternalCalls Emitter ──────────────────────────────────────────────────

static void EmitCsInternalCalls(std::ostream& out, const std::vector<ComponentDef>& components)
{
    out << "// Auto-generated by OloHeaderTool — DO NOT EDIT MANUALLY\n\n";
    out << "using System.Runtime.CompilerServices;\n\n";
    out << "namespace OloEngine\n{\n";
    out << "\tpublic static partial class InternalCalls\n";
    out << "\t{\n";

    for (auto const& comp : components)
    {
        out << "\t\t#region " << comp.name << "\n";

        for (auto const& prop : comp.properties)
        {
            std::string cs = CsType(prop.type);
            bool scalar = IsScalar(prop.type);

            if (scalar || prop.type == PropType::String)
            {
                out << "\t\t[MethodImpl(MethodImplOptions.InternalCall)]\n";
                out << "\t\tinternal static extern " << cs << " " << comp.name
                    << "_Get" << prop.scriptName << "(ulong entityID);\n";
                out << "\t\t[MethodImpl(MethodImplOptions.InternalCall)]\n";
                out << "\t\tinternal static extern void " << comp.name
                    << "_Set" << prop.scriptName << "(ulong entityID, " << cs << " value);\n";
            }
            else
            {
                out << "\t\t[MethodImpl(MethodImplOptions.InternalCall)]\n";
                out << "\t\tinternal static extern void " << comp.name
                    << "_Get" << prop.scriptName << "(ulong entityID, out " << cs << " value);\n";
                out << "\t\t[MethodImpl(MethodImplOptions.InternalCall)]\n";
                out << "\t\tinternal static extern void " << comp.name
                    << "_Set" << prop.scriptName << "(ulong entityID, ref " << cs << " value);\n";
            }
        }

        out << "\t\t#endregion\n\n";
    }

    out << "\t}\n}\n";
}

// ─── File Writing Helper ────────────────────────────────────────────────────────

enum class WriteResult
{
    Unchanged,
    Written,
    Failed
};

static WriteResult WriteIfChanged(const fs::path& path, const std::string& content)
{
    // Normalize: ensure content ends with exactly one newline
    std::string normalized = content;
    while (!normalized.empty() && normalized.back() == '\n')
        normalized.pop_back();
    normalized.push_back('\n');

    // Read existing file
    if (fs::exists(path))
    {
        if (std::ifstream existing(path); existing.is_open())
        {
            std::string old((std::istreambuf_iterator<char>(existing)),
                            std::istreambuf_iterator<char>());
            if (old == normalized)
                return WriteResult::Unchanged;
        }
    }

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec)
    {
        std::cerr << "ERROR: Failed to create directories for " << path << ": " << ec.message() << "\n";
        return WriteResult::Failed;
    }

    std::ofstream out(path);
    if (!out.is_open())
    {
        std::cerr << "ERROR: Failed to open " << path << " for writing\n";
        return WriteResult::Failed;
    }
    out << normalized;
    out.flush();
    if (!out.good())
    {
        std::cerr << "ERROR: Failed to write to " << path << "\n";
        return WriteResult::Failed;
    }
    return WriteResult::Written;
}

// ─── Main ───────────────────────────────────────────────────────────────────────

// Emit the AllComponents tuple to <scene_out_dir>/AllComponents.Generated.inl,
// reporting the outcome. Returns false on a write failure.
static bool WriteAllComponentsTuple(const fs::path& sceneOutDir, const std::set<std::string>& componentStructs)
{
    std::ostringstream ss;
    EmitAllComponentsTuple(ss, componentStructs);
    auto path = sceneOutDir / "AllComponents.Generated.inl";
    switch (WriteIfChanged(path, ss.str()))
    {
        case WriteResult::Written:
            std::cout << "  Wrote " << path << "\n";
            return true;
        case WriteResult::Unchanged:
            std::cout << "  " << path << " (unchanged)\n";
            return true;
        case WriteResult::Failed:
            std::cerr << "  FAILED " << path << "\n";
            return false;
    }
    return false;
}

// Report a single WriteIfChanged outcome; returns false only on a write failure.
static bool ReportWrite(const fs::path& path, const std::string& content)
{
    switch (WriteIfChanged(path, content))
    {
        case WriteResult::Written:
            std::cout << "  Wrote " << path << "\n";
            return true;
        case WriteResult::Unchanged:
            std::cout << "  " << path << " (unchanged)\n";
            return true;
        case WriteResult::Failed:
            std::cerr << "  FAILED " << path << "\n";
            return false;
    }
    return false;
}

// Emit the save-game capture/restore enumeration lists to
// <savegame_out_dir>/SaveGameComponent{Capture,Restore}.Generated.inl.
// Returns false on a write failure. Both lists are always written (no
// short-circuit) so a single failure still reports the other file's outcome.
static bool WriteSaveGameComponentLists(const fs::path& saveGameOutDir, const std::set<std::string>& componentStructs)
{
    std::ostringstream captureSs;
    EmitSaveGameCaptureList(captureSs, componentStructs);
    const bool captureOk = ReportWrite(saveGameOutDir / "SaveGameComponentCapture.Generated.inl", captureSs.str());

    std::ostringstream restoreSs;
    EmitSaveGameRestoreList(restoreSs, componentStructs);
    const bool restoreOk = ReportWrite(saveGameOutDir / "SaveGameComponentRestore.Generated.inl", restoreSs.str());

    return captureOk && restoreOk;
}

// Emit the Scene OnComponent{Added,Removed} no-op lists to
// <scene_out_dir>/OnComponent{Added,Removed}.Generated.inl (the same Scene/Generated
// dir as the AllComponents tuple). Returns false on a write failure. Both lists are
// always written (no short-circuit) so a single failure still reports the other.
static bool WriteSceneComponentHandlerLists(const fs::path& sceneOutDir, const std::set<std::string>& componentStructs)
{
    std::ostringstream addedSs;
    EmitOnComponentAddedNoops(addedSs, componentStructs);
    const bool addedOk = ReportWrite(sceneOutDir / "OnComponentAdded.Generated.inl", addedSs.str());

    std::ostringstream removedSs;
    EmitOnComponentRemovedNoops(removedSs, componentStructs);
    const bool removedOk = ReportWrite(sceneOutDir / "OnComponentRemoved.Generated.inl", removedSs.str());

    return addedOk && removedOk;
}

// Emit the scene-serializer per-component serialize/deserialize blocks to
// <scene_out_dir>/Scene{Serialize,Deserialize}Components.Generated.inl (the same
// Scene/Generated dir as the tuple), #include'd by SceneSerializer.cpp. Returns
// false on a write failure OR if the field scan produced zero auto-serializable
// components — an empty file would silently drop the hand-written blocks that were
// migrated into it, so (like the empty-tuple guard) we refuse and fail loudly.
static bool WriteSceneSerializerBlocks(const fs::path& sceneOutDir, const std::map<std::string, ComponentSerInfo>& componentFields)
{
    size_t trivialEmitted = 0;
    for (auto const& [name, info] : componentFields)
    {
        if (info.trivial && !kComponentsCustomSerialize.contains(name))
            ++trivialEmitted;
    }
    if (trivialEmitted == 0)
    {
        std::cerr << "ERROR: scene-serializer codegen found 0 auto-serializable components — "
                     "the field parser almost certainly broke. Refusing to overwrite "
                     "Scene{Serialize,Deserialize}Components.Generated.inl with empty blocks "
                     "(would silently drop the migrated hand-written serialization).\n";
        return false;
    }
    std::cout << "OloHeaderTool: scene-serializer codegen — " << trivialEmitted
              << " auto-serializable components ("
              << kComponentsCustomSerialize.size() << " trivial components kept hand-written)\n";

    std::ostringstream serSs;
    EmitSceneSerializeBlocks(serSs, componentFields);
    const bool serOk = ReportWrite(sceneOutDir / "SceneSerializeComponents.Generated.inl", serSs.str());

    std::ostringstream deserSs;
    EmitSceneDeserializeBlocks(deserSs, componentFields);
    const bool deserOk = ReportWrite(sceneOutDir / "SceneDeserializeComponents.Generated.inl", deserSs.str());

    // ── Binary scene-sidecar codegen (issue #525) — same covered set, typed
    //    binary read/write. Guard the on-disk ComponentId space against a hash
    //    collision (an FNV-1a-32 clash between two component names, or with the
    //    hand-written TransformComponent block's id) before emitting: a clash
    //    would silently misdispatch one component's bytes as another's. Loud
    //    build failure beats a corrupt cache.
    std::map<std::uint32_t, std::string> binIds;
    binIds.emplace(SceneBinComponentId("TransformComponent"), "TransformComponent (hand-written)");
    for (auto const& [name, info] : componentFields)
    {
        if (!info.trivial || kComponentsCustomSerialize.contains(name))
            continue;
        const std::uint32_t id = SceneBinComponentId(name);
        if (auto [it, inserted] = binIds.emplace(id, name); !inserted)
        {
            std::cerr << "ERROR: .scenebin ComponentId FNV-1a-32 collision (0x" << std::hex << id << std::dec
                      << ") between '" << name << "' and '" << it->second
                      << "'. Rename one component or switch the id hash.\n";
            return false;
        }
    }

    std::ostringstream binWriteSs;
    EmitSceneBinaryWriteBlocks(binWriteSs, componentFields);
    const bool binWriteOk = ReportWrite(sceneOutDir / "SceneBinaryWriteComponents.Generated.inl", binWriteSs.str());

    std::ostringstream binReadSs;
    EmitSceneBinaryReadBlocks(binReadSs, componentFields);
    const bool binReadOk = ReportWrite(sceneOutDir / "SceneBinaryReadComponents.Generated.inl", binReadSs.str());

    std::ostringstream binCoveredSs;
    EmitSceneBinaryCoveredIds(binCoveredSs, componentFields);
    const bool binCoveredOk = ReportWrite(sceneOutDir / "SceneBinaryCoveredComponents.Generated.inl", binCoveredSs.str());

    return serOk && deserOk && binWriteOk && binReadOk && binCoveredOk;
}

// Emit the MCP generic-field-write registry to
// <mcp_out_dir>/McpFieldRegistry.Generated.inl, #include'd by
// OloEditor/src/MCP/McpGenericFieldWrite.h inside BuildRegistry(). Same
// non-empty guard as every other list: an empty registry would silently strip
// EVERY writable field from `olo_entity_set_field` (the tool would still answer,
// just refuse everything) — refuse and fail the build loudly instead.
static bool WriteMcpFieldRegistry(const fs::path& mcpOutDir, const std::map<std::string, ComponentSerInfo>& componentFields,
                                  const std::vector<ComponentDef>& properties)
{
    std::ostringstream ss;
    std::size_t emitted = EmitMcpFieldRegistry(ss, componentFields);
    // Setter-based entries (OLO_PROPERTY reuse, issue #607's AudioSourceComponent
    // slice) — a small allowlisted addition on top of the plain field scan above;
    // see EmitMcpSetterFields' own comment for why it isn't every OLO_PROPERTY
    // component. Appended as its own section so the two codegen sources stay
    // visually distinguishable in the generated .inl.
    emitted += EmitMcpSetterFields(ss, properties, componentFields);
    if (emitted == 0)
    {
        std::cerr << "ERROR: MCP field-registry codegen found 0 writable component fields — "
                     "the field parser almost certainly broke. Refusing to overwrite "
                     "McpFieldRegistry.Generated.inl with an empty registry.\n";
        return false;
    }
    std::cout << "OloHeaderTool: MCP field registry — " << emitted << " writable fields ("
              << kComponentsNotMcpEditable.size() << " runtime-only components excluded)\n";
    return ReportWrite(mcpOutDir / "McpFieldRegistry.Generated.inl", ss.str());
}

int main(int argc, char* argv[])
{
    if (argc < 7)
    {
        std::cerr << "Usage: OloHeaderTool <scan_dir> <cpp_out_dir> <cs_out_dir> <scene_out_dir> "
                     "<savegame_out_dir> <mcp_out_dir>\n";
        return 1;
    }

    fs::path scanDir = argv[1];
    fs::path cppOutDir = argv[2];
    fs::path csOutDir = argv[3];
    fs::path sceneOutDir = argv[4];
    fs::path saveGameOutDir = argv[5];
    fs::path mcpOutDir = argv[6];

    if (!fs::exists(scanDir))
    {
        std::cerr << "ERROR: Scan directory does not exist: " << scanDir << "\n";
        return 1;
    }

    std::cout << "OloHeaderTool: Scanning " << scanDir << " ...\n";

    auto components = ParseHeaders(scanDir);

    // The AllComponents tuple is generated from `struct *Component` definitions and
    // is independent of OLO_PROPERTY — emit it before the no-properties early-out so
    // it is always written, even for a tree with components but no scripting props.
    auto componentStructs = CollectComponentStructs(scanDir);

    bool errors = false;

    if (componentStructs.empty())
    {
        // Almost certainly a misconfiguration (wrong scan dir, IO failure walking
        // the tree) — NOT a legitimately component-free engine. Writing an empty
        // `ComponentGroup<>` would silently strip every component from scene copy /
        // prefab / HasComponent<T>() registration. Refuse: leave the existing good
        // generated file untouched and fail the build loudly instead.
        std::cerr << "ERROR: No `struct *Component` definitions found under " << scanDir
                  << " — refusing to overwrite AllComponents.Generated.inl with an empty "
                     "tuple. Is the scan dir correct?\n";
        errors = true;
    }
    else
    {
        std::cout << "OloHeaderTool: Found " << componentStructs.size()
                  << " component structs (" << kComponentsNotInTuple.size()
                  << " excluded from the tuple, " << kComponentsNotInSaveGame.size()
                  << " excluded from save-games)\n";
        if (!WriteAllComponentsTuple(sceneOutDir, componentStructs))
            errors = true;
        // Same non-empty componentStructs guard as the tuple: an empty save-game
        // list would silently drop EVERY component from every save-game.
        if (!WriteSaveGameComponentLists(saveGameOutDir, componentStructs))
            errors = true;
        // Same guard again: an empty handler list would drop every component's
        // OnComponentAdded/Removed specialization → a wall of engine/editor link
        // errors. Written to the same Scene/Generated dir as the tuple.
        if (!WriteSceneComponentHandlerLists(sceneOutDir, componentStructs))
            errors = true;
        // Scene-serializer per-component serialize/deserialize blocks. Driven by a
        // full data-member scan (CollectComponentFields), not the OLO_PROPERTY scan
        // — the serializer persists every field, not just script-exposed ones. The
        // enum-type set lets the field parser recognise enum-typed members (which
        // round-trip as an int) instead of treating them as unknown non-trivial.
        auto enumTypes = CollectEnumTypes(scanDir);
        const std::map<std::string, ComponentSerInfo> componentFields = CollectComponentFields(scanDir, enumTypes);
        if (!WriteSceneSerializerBlocks(sceneOutDir, componentFields))
            errors = true;
        // The MCP `olo_entity_set_field` registry (issue #607) — same field scan,
        // a different consumer: every public, JSON-coercible member of every
        // component (including the ones the serializer keeps hand-written, whose
        // recognised fields the scan still collects).
        if (!WriteMcpFieldRegistry(mcpOutDir, componentFields, components))
            errors = true;
    }

    if (components.empty())
    {
        std::cout << "OloHeaderTool: No OLO_PROPERTY() annotations found. Writing empty stubs.\n";
        auto const stub = "// Auto-generated by OloHeaderTool — DO NOT EDIT MANUALLY\n"
                          "// No OLO_PROPERTY() annotations found.\n";
        // Write empty generated files to avoid leaving stale content from previous runs
        if (WriteIfChanged(cppOutDir / "ScriptGlueBindings.inl", stub) == WriteResult::Failed)
            errors = true;
        if (WriteIfChanged(cppOutDir / "ScriptGlueRegistrations.inl", stub) == WriteResult::Failed)
            errors = true;
        if (WriteIfChanged(csOutDir / "Scene" / "Components.Generated.cs", stub) == WriteResult::Failed)
            errors = true;
        if (WriteIfChanged(csOutDir / "InternalCalls.Generated.cs", stub) == WriteResult::Failed)
            errors = true;
        return errors ? 1 : 0;
    }

    // Count stats
    int totalProps = 0;
    for (auto const& comp : components)
        totalProps += static_cast<int>(comp.properties.size());

    std::cout << "OloHeaderTool: Found " << components.size() << " components, "
              << totalProps << " properties (" << totalProps * 2 << " getter/setter pairs)\n";

    // Generate C++ bindings
    {
        std::ostringstream ss;
        EmitCppBindings(ss, components);
        auto path = cppOutDir / "ScriptGlueBindings.inl";
        switch (WriteIfChanged(path, ss.str()))
        {
            case WriteResult::Written:
                std::cout << "  Wrote " << path << "\n";
                break;
            case WriteResult::Unchanged:
                std::cout << "  " << path << " (unchanged)\n";
                break;
            case WriteResult::Failed:
                std::cerr << "  FAILED " << path << "\n";
                errors = true;
                break;
        }
    }

    // Generate C++ registrations
    {
        std::ostringstream ss;
        EmitCppRegistrations(ss, components);
        auto path = cppOutDir / "ScriptGlueRegistrations.inl";
        switch (WriteIfChanged(path, ss.str()))
        {
            case WriteResult::Written:
                std::cout << "  Wrote " << path << "\n";
                break;
            case WriteResult::Unchanged:
                std::cout << "  " << path << " (unchanged)\n";
                break;
            case WriteResult::Failed:
                std::cerr << "  FAILED " << path << "\n";
                errors = true;
                break;
        }
    }

    // Generate C# Components
    {
        std::ostringstream ss;
        EmitCsComponents(ss, components);
        auto path = csOutDir / "Scene" / "Components.Generated.cs";
        switch (WriteIfChanged(path, ss.str()))
        {
            case WriteResult::Written:
                std::cout << "  Wrote " << path << "\n";
                break;
            case WriteResult::Unchanged:
                std::cout << "  " << path << " (unchanged)\n";
                break;
            case WriteResult::Failed:
                std::cerr << "  FAILED " << path << "\n";
                errors = true;
                break;
        }
    }

    // Generate C# InternalCalls
    {
        std::ostringstream ss;
        EmitCsInternalCalls(ss, components);
        auto path = csOutDir / "InternalCalls.Generated.cs";
        switch (WriteIfChanged(path, ss.str()))
        {
            case WriteResult::Written:
                std::cout << "  Wrote " << path << "\n";
                break;
            case WriteResult::Unchanged:
                std::cout << "  " << path << " (unchanged)\n";
                break;
            case WriteResult::Failed:
                std::cerr << "  FAILED " << path << "\n";
                errors = true;
                break;
        }
    }

    std::cout << "OloHeaderTool: Done.\n";
    return errors ? 1 : 0;
}

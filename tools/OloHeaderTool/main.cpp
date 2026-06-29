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
//      SceneSerializer.cpp. A component with any non-trivial field (enum,
//      AssetHandle, Ref<T>, std::vector, nested struct, …) is classified
//      non-trivial and stays hand-written. Collapses the last big *unguarded*
//      ECS touch-point — a forgotten field was silent scene-data loss.
//
// Usage:
//   OloHeaderTool <scan_dir> <cpp_out_dir> <cs_out_dir> <scene_out_dir> <savegame_out_dir>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
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
    AssetHandle, // AssetHandle / UUID — a u64 wrapper. Scene-serializer-only:
                 // round-trips as a u64 (static_cast<u64> on write, .as<u64> on
                 // read) but is emitted distinctly from a plain u64 so the
                 // serializer codegen knows to bridge the UUID<->u64 conversion.
                 // The scripting path never produces this (CppTypeToPropType maps
                 // AssetHandle -> U64), so only SceneSerType emits it.
    Vec2,
    Vec3,
    Vec4,
    String,
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

                // Skip access specifiers, comments, blank lines — retry on next line
                if (trimmed.starts_with("//") || trimmed.starts_with("/*") ||
                    trimmed.starts_with("private:") || trimmed.starts_with("public:") ||
                    trimmed.starts_with("protected:") || trimmed.empty())
                {
                    continue; // pendingMetadataList stays populated for next line
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
// SoundGraph source, a video decode thread) or drop cached runtime state (the
// SpringBone / NoiseAnimation state component). Every OTHER `struct *Component`
// gets a generated no-op `OLO_ON_COMPONENT_REMOVED_NOOP(T)`.
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
    "TerrainComponent",
};

// Components that ARE all-trivial-fields (every data member is a primitive /
// glm::vec* / std::string / AssetHandle / UUID — see SceneSerType) yet are
// deliberately kept HAND-WRITTEN in SceneSerializer.cpp rather than auto-generated.
// The scene serialize/deserialize codegen (issue #380; AssetHandle/UUID added by
// the #451 first slice) emits a block for every all-trivial component EXCEPT these;
// anything with a still-unhandled non-trivial field (enum, Ref<T>, std::vector,
// nested struct, glm::quat/mat/ivec, …) is classified non-trivial by the parser and
// skipped automatically without needing an entry here.
//
// Each entry is a trivial component whose hand-written block does something the
// plain round-trip generator must NOT silently drop:
//   * BuoyancyComponent / SphereAreaLightComponent / SpringBoneComponent /
//     NoiseAnimationComponent / DialogueComponent / PerceptionComponent /
//     IKTargetComponent — deserialize clamps / Sanitize()s float (or u32 chain-
//     length / vector) ranges; auto-generating would relax those guards.
//     (PerceptionComponent also intentionally does NOT restore its runtime-derived
//     fields — HasVisibleTarget / VisibleTarget / LastKnownPosition / … — on load.)
//   * SnowDeformerComponent — serialize/deserialize delegate to a hand-written
//     helper (Serialize/DeserializeSnowDeformerComponent), not a flat field list.
//   * ScriptComponent — serializes the C# ScriptField map owned by ScriptEngine,
//     not just its ClassName member (the parser only sees ClassName).
//   * VehicleComponent — has a runtime-only RuntimeVehicleToken field the
//     hand-written serializer deliberately omits (auto-gen would persist it).
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
//
// DISJOINTNESS is guarded by ComponentSerializerCoverageTest: a component must be
// handled by EXACTLY ONE of the generated .inl or the hand-written serializer —
// listing one here while ALSO leaving (or removing) its hand-written block is a
// loud test failure, never a silent double-emit / drop.
static const std::set<std::string> kComponentsCustomSerialize = {
    "BuoyancyComponent",
    "DialogueComponent",
    "IDComponent",
    "IKTargetComponent",
    "NoiseAnimationComponent",
    "PerceptionComponent",
    "PhaseComponent",
    "ScriptComponent",
    "SnowDeformerComponent",
    "SphereAreaLightComponent",
    "SpringBoneComponent",
    "TagComponent",
    "UIResolvedRectComponent",
    "VehicleComponent",
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
// meshes, colliders, dialogue, streaming, …). Anything still unhandled — enum,
// Ref<T>, std::vector, nested struct, glm::quat/mat/ivec, raw pointer, u8/u16, … —
// returns Unknown, marking the component non-trivial so it stays hand-written in
// SceneSerializer.cpp (handled by a future #451 slice).
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
    if (t == "AssetHandle" || t == "UUID")
        return PropType::AssetHandle;
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

// Parse the top-level data members of a `struct *Component { ... }` body and
// classify the component as auto-serializable or not. A member is collected only
// when it is unambiguously a `<trivial-type> <name>` field declaration. Anything
// the parser cannot confidently classify as a trivial field — a non-trivial type,
// a pointer/reference/template/array/bitfield, a const member — marks the whole
// component non-trivial so it is left hand-written (ambiguity always fails safe).
static ComponentSerInfo ParseComponentFields(std::string body)
{
    ComponentSerInfo info;
    bool ambiguous = false;

    // OLO_PROPERTY(...) annotations expand to nothing — drop them (paren-balanced)
    // before statement splitting so they never merge into the next field's line.
    body = StripBalancedMacro(body, "OLO_PROPERTY");

    // Split the body into top-level statements at ';' (brace-depth 0). Nested
    // braces — initializer lists, method bodies — are kept inside one statement.
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
                --depth;
                buf += c;
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
        if (pt == PropType::Unknown)
        {
            ambiguous = true; // an unrecognised type — keep the component hand-written
            continue;
        }

        SerField f;
        f.member = name;
        f.key = StripPrefix(name, "m_");
        f.type = pt;
        info.fields.push_back(f);
    }

    info.trivial = !ambiguous && !info.fields.empty();
    return info;
}

// Scan every header for `struct *Component` definitions and parse each one's data
// members. Mirrors CollectComponentStructs' discrimination (definitions only, the
// first definition of a given name wins) but extracts the full member list rather
// than just the type name, so the scene serializer codegen can emit per-field
// read/writes. Returns a name → ComponentSerInfo map (std::map for deterministic,
// alphabetical emit order).
static std::map<std::string, ComponentSerInfo> CollectComponentFields(const fs::path& scanDir)
{
    std::map<std::string, ComponentSerInfo> result;
    static const std::regex structRe(R"(\bstruct\s+([A-Za-z_]\w*Component)\b)");

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
        if (raw.find("Component") == std::string::npos)
            continue;

        std::string content = StripComments(raw);
        for (auto it = std::sregex_iterator(content.begin(), content.end(), structRe);
             it != std::sregex_iterator(); ++it)
        {
            std::string name = (*it)[1].str();
            size_t p = static_cast<size_t>(it->position()) + static_cast<size_t>(it->length());
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

            if (result.contains(name))
                continue; // first definition wins (matches CollectComponentStructs)
            result.emplace(name, ParseComponentFields(content.substr(start + 1, q - start - 1)));
        }
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
    out << "// `struct *Component` whose every data member is a primitive / glm::vec* /\n";
    out << "// std::string (the generator's SceneSerType), minus the kComponentsCustomSerialize\n";
    out << "// exclusion set (trivial components deliberately kept hand-written). A component\n";
    out << "// with any non-trivial field (enum, AssetHandle, Ref<T>, std::vector, nested\n";
    out << "// struct, …) is classified non-trivial and stays hand-written in SceneSerializer.cpp.\n";
    out << "//\n";
    out << "// #include'd inside SceneSerializer::" << func << ", where " << locals << "\n";
    out << "// are in scope. Floats are validated with std::isfinite via TryReadFiniteF32 /\n";
    out << "// the glm Decode helpers (NaN/Inf in YAML keeps the constructor default).\n";
    out << "//\n";
    out << "// Each component is handled by EXACTLY ONE of this file or the hand-written\n";
    out << "// serializer — ComponentSerializerCoverageTest fails loudly on a drop or a\n";
    out << "// double-emit, so a new trivial component is auto-serialized and a new complex\n";
    out << "// one fails the coverage test until hand-written.\n\n";
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
        for (auto const& f : info.fields)
        {
            // AssetHandle / UUID has an implicit operator u64() but YAML::Emitter has
            // no UUID overload, so emit the explicit u64 cast (matches every existing
            // hand-written AssetHandle block, e.g. SoundConfigHandle / ColliderAsset).
            if (f.type == PropType::AssetHandle)
                out << "    out << YAML::Key << \"" << f.key << "\" << YAML::Value << static_cast<u64>(comp." << f.member << ");\n";
            else
                out << "    out << YAML::Key << \"" << f.key << "\" << YAML::Value << comp." << f.member << ";\n";
        }
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
        for (auto const& f : info.fields)
        {
            const std::string lhs = "comp." + f.member;
            const std::string key = "node[\"" + f.key + "\"]";
            switch (f.type)
            {
                case PropType::Float:
                    // Validate finiteness; NaN/Inf or a missing key keeps the default.
                    out << "    if (f32 v; ::OloEngine::YAMLUtils::TryReadFiniteF32(" << key << ", v))\n";
                    out << "        " << lhs << " = v;\n";
                    break;
                case PropType::Vec2:
                    out << "    " << lhs << " = " << key << ".as<glm::vec2>(" << lhs << ");\n";
                    break;
                case PropType::Vec3:
                    out << "    " << lhs << " = " << key << ".as<glm::vec3>(" << lhs << ");\n";
                    break;
                case PropType::Vec4:
                    out << "    " << lhs << " = " << key << ".as<glm::vec4>(" << lhs << ");\n";
                    break;
                case PropType::Bool:
                    out << "    " << lhs << " = " << key << ".as<bool>(" << lhs << ");\n";
                    break;
                case PropType::Int:
                    out << "    " << lhs << " = " << key << ".as<i32>(" << lhs << ");\n";
                    break;
                case PropType::UInt:
                    out << "    " << lhs << " = " << key << ".as<u32>(" << lhs << ");\n";
                    break;
                case PropType::U64:
                    out << "    " << lhs << " = " << key << ".as<u64>(" << lhs << ");\n";
                    break;
                case PropType::AssetHandle:
                    // Read as u64, bridge back through the implicit UUID(u64) ctor; a
                    // missing key keeps the constructor default (cast it to u64 for the
                    // yaml-cpp fallback overload). Matches the hand-written blocks.
                    out << "    " << lhs << " = " << key << ".as<u64>(static_cast<u64>(" << lhs << "));\n";
                    break;
                case PropType::String:
                    out << "    " << lhs << " = " << key << ".as<std::string>(" << lhs << ");\n";
                    break;
                default:
                    break;
            }
        }
        out << "}\n\n";
    }
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

    return serOk && deserOk;
}

int main(int argc, char* argv[])
{
    if (argc < 6)
    {
        std::cerr << "Usage: OloHeaderTool <scan_dir> <cpp_out_dir> <cs_out_dir> <scene_out_dir> <savegame_out_dir>\n";
        return 1;
    }

    fs::path scanDir = argv[1];
    fs::path cppOutDir = argv[2];
    fs::path csOutDir = argv[3];
    fs::path sceneOutDir = argv[4];
    fs::path saveGameOutDir = argv[5];

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
        // — the serializer persists every field, not just script-exposed ones.
        if (!WriteSceneSerializerBlocks(sceneOutDir, CollectComponentFields(scanDir)))
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

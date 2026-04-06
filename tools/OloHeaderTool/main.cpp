// OloHeaderTool — Generates scripting bindings from OLO_PROPERTY() annotations.
//
// Scans C++ headers for OLO_PROPERTY() markers, parses the next field
// declaration, and emits:
//   1. C++ Mono getter/setter functions       (ScriptGlueBindings.inl)
//   2. C++ OLO_ADD_INTERNAL_CALL registrations (ScriptGlueRegistrations.inl)
//   3. C# proxy Component classes             (Components.Generated.cs)
//   4. C# InternalCall declarations           (InternalCalls.Generated.cs)
//
// Usage:
//   OloHeaderTool <scan_dir> <cpp_out_dir> <cs_out_dir>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ─── Data Structures ────────────────────────────────────────────────────────────

enum class PropType
{
    Float,
    Bool,
    Int,
    UInt,
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
    return t == PropType::Float || t == PropType::Bool || t == PropType::Int || t == PropType::UInt;
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
        auto ext = entry.path().extension().string();
        if (ext != ".h" && ext != ".hpp")
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

        // Track struct names at each brace depth for nesting
        std::vector<std::string> structStack;

        // Map component name → index in components vector
        std::map<std::string, size_t> compMap;

        while (std::getline(stream, line))
        {
            std::string trimmed = Trim(line);

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
                        ++braceDepth;
                        structStack.push_back(name);
                        currentComponent = name;
                        insideStruct = true;
                    }
                    else
                    {
                        // Next line might have the brace — mark pending
                        structStack.push_back(name);
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
                    if (braceDepth <= 0 && !structStack.empty())
                    {
                        structStack.pop_back();
                        currentComponent = structStack.empty() ? "" : structStack.back();
                        insideStruct = !structStack.empty();
                        braceDepth = std::max(braceDepth, 0);
                    }
                }
            }

            // Check for OLO_PROPERTY(...) — skip if inside a line comment
            auto propPos = trimmed.find("OLO_PROPERTY(");
            if (propPos != std::string::npos)
            {
                auto commentPos = trimmed.find("//");
                if (commentPos != std::string::npos && commentPos < propPos)
                {
                    continue; // OLO_PROPERTY is inside a comment — ignore
                }
                // Extract the content between OLO_PROPERTY( and )
                auto parenStart = propPos + 13; // length of "OLO_PROPERTY("
                // Find matching closing paren (handle nested parens)
                int depth = 1;
                auto pos = parenStart;
                while (pos < trimmed.size() && depth > 0)
                {
                    if (trimmed[pos] == '(')
                        ++depth;
                    else if (trimmed[pos] == ')')
                        --depth;
                    if (depth > 0)
                        ++pos;
                }
                pendingMetadataList.push_back(trimmed.substr(parenStart, pos - parenStart));
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
                        fieldName = fieldName.substr(0, cutPos);
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
                    if (compMap.find(currentComponent) == compMap.end())
                    {
                        compMap[currentComponent] = components.size();
                        components.push_back({ currentComponent,
                                               entry.path().filename().string(),
                                               {} });
                    }
                    components[compMap[currentComponent]].properties.push_back(prop);
                }
                pendingMetadataList.clear();
            }
        }
    }

    // Sort by component name for deterministic output
    std::sort(components.begin(), components.end(),
              [](const ComponentDef& a, const ComponentDef& b)
              { return a.name < b.name; });

    return components;
}

// ─── C++ Mono Bindings Emitter ──────────────────────────────────────────────────

static void EmitCppBindings(std::ostream& out, const std::vector<ComponentDef>& components)
{
    out << "// Auto-generated by OloHeaderTool — DO NOT EDIT MANUALLY\n";
    out << "// Re-generate with: cmake --build build --target GenerateBindings\n\n";

    for (auto const& comp : components)
    {
        out << "    ///////////////////////////////////////////////////////////////////////////////////////////\n";
        out << "    // " << comp.name << std::string(std::max(0, 85 - 7 - static_cast<int>(comp.name.size())), ' ')
            << " //\n";
        out << "    ///////////////////////////////////////////////////////////////////////////////////////////\n\n";

        for (auto const& prop : comp.properties)
        {
            std::string funcBase = comp.name + "_Get" + prop.scriptName;
            std::string funcSet = comp.name + "_Set" + prop.scriptName;
            bool scalar = IsScalar(prop.type);
            bool isString = prop.type == PropType::String;

            // --- Getter ---
            if (isString)
            {
                out << "    static MonoString* " << funcBase << "(UUID entityID)\n";
                out << "    {\n";
                out << "        Scene* scene = ScriptEngine::GetSceneContext();\n";
                out << "        OLO_CORE_ASSERT(scene);\n";
                out << "        Entity entity = scene->GetEntityByUUID(entityID);\n";
                out << "        OLO_CORE_ASSERT(entity);\n";
                out << "        auto& comp = entity.GetComponent<" << comp.name << ">();\n";

                if (prop.customGet.empty())
                    out << "        return ScriptEngine::CreateString(comp." << prop.cppField << ".c_str());\n";
                else
                    out << "        return ScriptEngine::CreateString((" << prop.customGet << ").c_str());\n";

                out << "    }\n\n";
            }
            else if (scalar)
            {
                out << "    static " << CppReturnType(prop.type) << " " << funcBase << "(UUID entityID)\n";
                out << "    {\n";
                out << "        Scene* scene = ScriptEngine::GetSceneContext();\n";
                out << "        OLO_CORE_ASSERT(scene);\n";
                out << "        Entity entity = scene->GetEntityByUUID(entityID);\n";
                out << "        OLO_CORE_ASSERT(entity);\n";
                out << "        auto& comp = entity.GetComponent<" << comp.name << ">();\n";

                if (prop.customGet.empty())
                    out << "        return comp." << prop.cppField << ";\n";
                else
                    out << "        return " << prop.customGet << ";\n";

                out << "    }\n\n";
            }
            else
            {
                std::string glm = GlmType(prop.type);
                out << "    static void " << funcBase << "(UUID entityID, " << glm << "* outValue)\n";
                out << "    {\n";
                out << "        Scene* scene = ScriptEngine::GetSceneContext();\n";
                out << "        OLO_CORE_ASSERT(scene);\n";
                out << "        Entity entity = scene->GetEntityByUUID(entityID);\n";
                out << "        OLO_CORE_ASSERT(entity);\n";
                out << "        auto& comp = entity.GetComponent<" << comp.name << ">();\n";

                if (prop.customGet.empty())
                    out << "        *outValue = comp." << prop.cppField << ";\n";
                else
                    out << "        *outValue = " << prop.customGet << ";\n";

                out << "    }\n\n";
            }

            // --- Setter ---
            if (isString)
            {
                out << "    static void " << funcSet << "(UUID entityID, MonoString* value)\n";
                out << "    {\n";
                out << "        Scene* scene = ScriptEngine::GetSceneContext();\n";
                out << "        OLO_CORE_ASSERT(scene);\n";
                out << "        Entity entity = scene->GetEntityByUUID(entityID);\n";
                out << "        OLO_CORE_ASSERT(entity);\n";
                out << "        if (!value) return;\n";
                out << "        auto& comp = entity.GetComponent<" << comp.name << ">();\n";

                if (prop.customSet.empty())
                    out << "        comp." << prop.cppField << " = Utils::MonoStringToString(value);\n";
                else
                {
                    std::string expr = ReplaceAll(prop.customSet, "{v}", "Utils::MonoStringToString(value)");
                    out << "        " << expr << ";\n";
                }

                out << "    }\n\n";
            }
            else if (scalar)
            {
                out << "    static void " << funcSet << "(UUID entityID, " << CppReturnType(prop.type) << " value)\n";
                out << "    {\n";
                out << "        Scene* scene = ScriptEngine::GetSceneContext();\n";
                out << "        OLO_CORE_ASSERT(scene);\n";
                out << "        Entity entity = scene->GetEntityByUUID(entityID);\n";
                out << "        OLO_CORE_ASSERT(entity);\n";
                out << "        auto& comp = entity.GetComponent<" << comp.name << ">();\n";

                if (prop.customSet.empty())
                    out << "        comp." << prop.cppField << " = value;\n";
                else
                {
                    std::string expr = ReplaceAll(prop.customSet, "{v}", "value");
                    out << "        " << expr << ";\n";
                }

                out << "    }\n\n";
            }
            else
            {
                std::string glm = GlmType(prop.type);
                out << "    static void " << funcSet << "(UUID entityID, " << glm << " const* value)\n";
                out << "    {\n";
                out << "        Scene* scene = ScriptEngine::GetSceneContext();\n";
                out << "        OLO_CORE_ASSERT(scene);\n";
                out << "        Entity entity = scene->GetEntityByUUID(entityID);\n";
                out << "        OLO_CORE_ASSERT(entity);\n";
                out << "        auto& comp = entity.GetComponent<" << comp.name << ">();\n";

                if (prop.customSet.empty())
                    out << "        comp." << prop.cppField << " = *value;\n";
                else
                {
                    std::string expr = ReplaceAll(prop.customSet, "{v}", "*value");
                    out << "        " << expr << ";\n";
                }

                out << "    }\n\n";
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
        out << "        // " << comp.name << "\n";
        for (auto const& prop : comp.properties)
        {
            out << "        OLO_ADD_INTERNAL_CALL(" << comp.name << "_Get" << prop.scriptName << ");\n";
            out << "        OLO_ADD_INTERNAL_CALL(" << comp.name << "_Set" << prop.scriptName << ");\n";
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
            bool scalar = IsScalar(prop.type);

            if (scalar || prop.type == PropType::String)
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

static bool WriteIfChanged(const fs::path& path, const std::string& content)
{
    // Normalize: ensure content ends with exactly one newline
    std::string normalized = content;
    while (!normalized.empty() && normalized.back() == '\n')
        normalized.pop_back();
    normalized.push_back('\n');

    // Read existing file
    if (fs::exists(path))
    {
        std::ifstream existing(path);
        std::string old((std::istreambuf_iterator<char>(existing)),
                        std::istreambuf_iterator<char>());
        if (old == normalized)
            return false; // No change
    }

    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << normalized;
    return true;
}

// ─── Main ───────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[])
{
    if (argc < 4)
    {
        std::cerr << "Usage: OloHeaderTool <scan_dir> <cpp_out_dir> <cs_out_dir>\n";
        return 1;
    }

    fs::path scanDir = argv[1];
    fs::path cppOutDir = argv[2];
    fs::path csOutDir = argv[3];

    if (!fs::exists(scanDir))
    {
        std::cerr << "ERROR: Scan directory does not exist: " << scanDir << "\n";
        return 1;
    }

    std::cout << "OloHeaderTool: Scanning " << scanDir << " ...\n";

    auto components = ParseHeaders(scanDir);

    if (components.empty())
    {
        std::cout << "OloHeaderTool: No OLO_PROPERTY() annotations found. Writing empty stubs.\n";
        // Write empty generated files to avoid leaving stale content from previous runs
        WriteIfChanged(cppOutDir / "ScriptGlueBindings.inl",
                       "// Auto-generated by OloHeaderTool — DO NOT EDIT MANUALLY\n"
                       "// No OLO_PROPERTY() annotations found.\n");
        WriteIfChanged(cppOutDir / "ScriptGlueRegistrations.inl",
                       "// Auto-generated by OloHeaderTool — DO NOT EDIT MANUALLY\n"
                       "// No OLO_PROPERTY() annotations found.\n");
        WriteIfChanged(csOutDir / "Scene" / "Components.Generated.cs",
                       "// Auto-generated by OloHeaderTool — DO NOT EDIT MANUALLY\n"
                       "// No OLO_PROPERTY() annotations found.\n");
        WriteIfChanged(csOutDir / "InternalCalls.Generated.cs",
                       "// Auto-generated by OloHeaderTool — DO NOT EDIT MANUALLY\n"
                       "// No OLO_PROPERTY() annotations found.\n");
        return 0;
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
        if (WriteIfChanged(path, ss.str()))
            std::cout << "  Wrote " << path << "\n";
        else
            std::cout << "  " << path << " (unchanged)\n";
    }

    // Generate C++ registrations
    {
        std::ostringstream ss;
        EmitCppRegistrations(ss, components);
        auto path = cppOutDir / "ScriptGlueRegistrations.inl";
        if (WriteIfChanged(path, ss.str()))
            std::cout << "  Wrote " << path << "\n";
        else
            std::cout << "  " << path << " (unchanged)\n";
    }

    // Generate C# Components
    {
        std::ostringstream ss;
        EmitCsComponents(ss, components);
        auto path = csOutDir / "Scene" / "Components.Generated.cs";
        if (WriteIfChanged(path, ss.str()))
            std::cout << "  Wrote " << path << "\n";
        else
            std::cout << "  " << path << " (unchanged)\n";
    }

    // Generate C# InternalCalls
    {
        std::ostringstream ss;
        EmitCsInternalCalls(ss, components);
        auto path = csOutDir / "InternalCalls.Generated.cs";
        if (WriteIfChanged(path, ss.str()))
            std::cout << "  Wrote " << path << "\n";
        else
            std::cout << "  " << path << " (unchanged)\n";
    }

    std::cout << "OloHeaderTool: Done.\n";
    return 0;
}

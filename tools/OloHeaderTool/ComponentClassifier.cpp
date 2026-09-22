#include "ComponentClassifier.h"
#include "TextParsing.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <regex>
#include <set>
#include <sstream>
#include <utility>

namespace OloHeaderTool
{
    using Detail::PeelSerializeMarker;
    using Detail::ReplaceAll;
    using Detail::StripPrefix;
    using Detail::Trim;

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
    //
    // `rejectOut` receives the sibling `Reject` token (issue #451's Reject slice),
    // which shares the same Min=/Max= bounds but means REJECT-out-of-range rather
    // than CLAMP-to-range: an out-of-bounds value keeps the constructor default
    // instead of saturating at the nearest bound. The two are mutually exclusive —
    // ParseComponentFields fails a field carrying both non-trivial rather than
    // picking one.
    static bool SerializeArgsClampBounds(const std::string& args, std::optional<std::string>& minOut,
                                         std::optional<std::string>& maxOut, bool& rejectOut)
    {
        bool hasClamp = false;
        rejectOut = false;
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
                else if (key == "Reject")
                    rejectOut = true;
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
        if (t == "FString" || t == "OloEngine::FString")
            return PropType::FString;
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
    static std::set<std::string> CollectEnumTypes(std::span<const std::string_view> headerSources)
    {
        std::set<std::string> names;
        // `enum`, optional `class`/`struct`, then the type name. The name must be a
        // plain identifier; an opaque-enum colon (`: u8`), `{`, or `;` ends it.
        static const std::regex enumRe(R"(\benum\s+(?:class\s+|struct\s+)?([A-Za-z_]\w*))");

        for (const auto source : headerSources)
        {
            const std::string raw(source);
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
    static std::set<std::string> CollectAssetTypes(std::span<const std::string_view> headerSources)
    {
        std::map<std::string, std::string> bases; // derived leaf -> first-base leaf
        static const std::regex classRe(
            R"(\bclass\s+([A-Za-z_]\w*)\s*:\s*(?:public\s+|private\s+|protected\s+)?([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*))");

        for (const auto source : headerSources)
        {
            const std::string raw(source);
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
            bool fieldHasReject = false;
            std::optional<std::string> fieldClampMin, fieldClampMax;
            if (std::string serArgs, serRest; PeelSerializeMarker(s, serArgs, serRest))
            {
                if (SerializeArgsHaveSkip(serArgs))
                    continue; // runtime-only field — not serialized, not non-trivial
                fieldHasClamp = SerializeArgsClampBounds(serArgs, fieldClampMin, fieldClampMax, fieldHasReject);
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

            // std::vector<E> / TArray<E> share a sequence wire shape. Recognize the
            // storage spelling before generic template rejection; elements may be
            // scalar types, enums, or recursively classified all-public structs.
            // Nested container templates and custom allocator arguments stay
            // unsupported and fail closed instead of guessing their element type.
            if (auto lt = decl.find('<'); lt != std::string::npos &&
                                          (Trim(decl.substr(0, lt)) == "std::vector" || Trim(decl.substr(0, lt)) == "TArray"))
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
                if (inner.find('<') != std::string::npos || SplitTopLevelCommas(inner).size() != 1 ||
                    ept == PropType::Unknown || !IsIdentifier(rest) ||
                    !publicSection || fieldHasClamp || fieldHasReject)
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
                f.isTArray = Trim(decl.substr(0, lt)) == "TArray";
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
                    !publicSection || fieldHasClamp || fieldHasReject)
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
                if (!keyOk || vt == PropType::Unknown || !IsIdentifier(rest) || !publicSection || fieldHasClamp || fieldHasReject)
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
                    !IsIdentifier(rest) || !publicSection || fieldHasClamp || fieldHasReject)
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
            // Reject (issue #451's Reject slice) takes the same bounds as Clamp but
            // keeps the constructor default when the read value falls outside them.
            // Scalars only — Vec3 is deliberately excluded (see SerField::hasReject):
            // per-component rejection would leave a partially-updated vector, and
            // whole-vector rejection on one bad component is not what any of the
            // hand-written vec3 sanitizers do. Both annotations at once is a
            // contradiction (saturate AND keep-default), so it fails safe too.
            static const std::set<PropType> kRejectEligible = {
                PropType::Float, PropType::Int, PropType::UInt,
                PropType::SmallInt, PropType::SmallUInt, PropType::Enum
            };
            if (fieldHasReject &&
                (fieldHasClamp || !kRejectEligible.contains(pt) || (!fieldClampMin && !fieldClampMax)))
            {
                ambiguous = true;
                continue;
            }

            SerField f;
            f.member = name;
            f.key = StripPrefix(name, "m_");
            f.type = pt;
            f.cppType = LeafTypeName(type);
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
            if (fieldHasClamp || fieldHasReject)
            {
                f.hasClamp = fieldHasClamp;
                f.hasReject = fieldHasReject;
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
    static std::map<std::string, StructDef> CollectStructBodies(std::span<const std::string_view> headerSources)
    {
        std::map<std::string, StructDef> result;
        static const std::regex recordRe(R"(\b(struct|class)\s+([A-Za-z_]\w*)\b)");

        for (const auto source : headerSources)
        {
            const std::string raw(source);
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
    std::map<std::string, ComponentSerInfo> ClassifyComponents(std::span<const std::string_view> headerSources)
    {
        const auto enumTypes = CollectEnumTypes(headerSources);
        const std::map<std::string, StructDef> structDefs = CollectStructBodies(headerSources);
        const std::set<std::string> assetTypes = CollectAssetTypes(headerSources);
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

} // namespace OloHeaderTool

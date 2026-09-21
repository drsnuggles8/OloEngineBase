#pragma once

#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OloHeaderTool
{
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
        FString, // UTF-8 wire string with explicit conversion at third-party seams.
        Enum,    // An `enum` / `enum class` type. Scene-serializer-only: round-trips as
                 // an int (static_cast<int> on write, .as<int> on read), cast back to the
                 // field's own type via decltype so a nested enum (e.g.
                 // AnimationStateComponent::State) needs no qualified spelling at the
                 // SceneSerializer level. Detected by name against the collected enum-type
                 // set (see CollectEnumTypes) in ParseComponentFields, not by SceneSerType
                 // (which only knows built-in type names). The scripting path never produces
                 // this — only the scene serializer codegen emits it.
        Struct,  // A nested `struct` (or `std::vector<struct>`) whose every member is
                 // itself a serializer-trivial type (recursively). Scene-serializer-only:
                 // round-trips as a nested YAML sub-map (scalar member) or a sequence of
                 // sub-maps (vector member). The sub-struct's fields are carried in
                 // SerField::subFields; SerField::isVector distinguishes the two shapes.
                 // Detected by recursively parsing the member type's struct definition
                 // (see CollectStructBodies / ClassifyStruct in ParseComponentFields);
                 // #451's nested-struct slice. The scripting path never produces this.
        Ref,     // A `Ref<T>` runtime asset handle, where T transitively derives from
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

    // A single auto-serializable data member of a component.
    struct SerField
    {
        std::string member; // C++ member name, e.g. "m_Intensity" or "Color"
        std::string key;    // YAML key, e.g. "Intensity" / "Color" (member minus m_)
        PropType type{ PropType::Unknown };
        // When true the member is a std::vector<E> or TArray<E> YAML sequence; `type`
        // holds the ELEMENT PropType E (which must itself be a trivial serializer type).
        // For a std::vector<struct>, type == PropType::Struct and subFields holds the
        // element struct's fields.
        bool isVector{ false };
        // The sequence wire shape is shared; only the emitted storage API differs.
        bool isTArray{ false };
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

        // OLO_SERIALIZE(Reject, Min=…, Max=…) — issue #451's Reject slice, the
        // sibling of Clamp. Shares clampMin/clampMax (the bounds are the same data)
        // but flips the SEMANTIC: an out-of-range read keeps the constructor default
        // instead of saturating at the nearest bound. Use it wherever saturating
        // would silently turn a corrupt value into a DIFFERENT VALID one — the
        // motivating case is an enum, where Clamp(0, 2) quietly maps a corrupt `7`
        // to enumerator 2 while every other load path (save-game, the Jolt
        // consumer) maps anything unrecognised back to enumerator 0.
        //
        // Supported on the scalar Float/Int/UInt/SmallInt/SmallUInt/Enum types only
        // — NOT Vec3, since "out of range" has no single meaning for a vector
        // (per-component reject would leave a half-updated value). Mutually
        // exclusive with Clamp; requesting both, or requesting Reject on an
        // unsupported type / with neither bound, marks the whole component
        // non-trivial rather than silently dropping the annotation.
        bool hasReject{ false };

        // Populated only when type == PropType::Ref: the spelling of T in `Ref<T>`
        // (elaborated-type keyword already peeled), used for the generated
        // AssetManager::GetAsset<T> call (issue #451 Ref<T> slice).
        std::string refType;

        // The member's WRITTEN type, leaf name only ("AssetHandle", "UUID", "f32",
        // "MyEnum"), for the scalar path only — empty for a container element, whose
        // `type` already carries the element's PropType and whose spelling nothing
        // needs. Recorded because PropType is deliberately lossy in one place that
        // matters downstream: SceneSerType maps BOTH `AssetHandle` and `UUID` onto
        // PropType::AssetHandle, since the scene serializer round-trips them
        // identically (a u64). The visual-script field registry cannot: an
        // AssetHandle is a PinType::Asset and a UUID is a PinType::Entity, and
        // presenting an entity reference to a graph author as an asset pin is a wire
        // the compiler would accept and the runtime could never satisfy. One string
        // per field is cheaper than a second type classifier.
        std::string cppType;
    };

    // Per-component result of the field scan.
    struct ComponentSerInfo
    {
        std::vector<SerField> fields;
        bool trivial{ false }; // every member is a trivial serializable type, non-empty
    };

    // Classify component fields from a complete set of header texts. Results own all
    // their strings; no source views are retained. Source order preserves the existing
    // first-definition-wins rule, with struct definitions preferred over classes.
    std::map<std::string, ComponentSerInfo> ClassifyComponents(std::span<const std::string_view> headerSources);
} // namespace OloHeaderTool

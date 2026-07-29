#pragma once
// Field-level serialization metadata as real C++26 annotations (P3394).
// Included BEFORE component definitions so the components can be annotated;
// OloReflect.h includes this too (idempotent) to read them back via reflection.
//
// In the real engine, OLO_SERIALIZE(Clamp, Min=.., Max=..) / OLO_SERIALIZE(Skip)
// would expand to these ONLY under __cpp_reflection, so one source feeds both the
// text tool (which sees the macro comment form) and reflection (which sees [[=..]]).
namespace OloEngine::Reflect
{
    struct Clamp { double min; double max; };  // double holds any scalar; cast at apply
    struct Skip  {};                           // omit this field from (de)serialize
    struct Key {                               // custom YAML key (rename), like serde(rename)
        char name[64]{};                       // fixed buffer -> structural (annotations can't hold const char*)
        consteval Key(const char* s) { for (int i = 0; s[i] != '\0' && i < 63; ++i) name[i] = s[i]; }
    };
    struct Reject { double min; double max; }; // out-of-range on load -> KEEP the default (reject, not clamp)
    struct Flatten {};                          // serialize this nested-struct member's fields at the PARENT level (no sub-map)
    // Marks a field script-exposed (OLO_PROPERTY). Carries the annotation's args verbatim
    // (Name/Type/Get/Set/…) so the neutral schema is self-sufficient (ADR 0009 commitment #3):
    // the script-facing name and access are parsed out of `raw` at consteval — no second scan.
    struct Property {
        char raw[256]{};                        // OLO_PROPERTY(...) args captured verbatim ("" for a bare OLO_PROPERTY())
        consteval Property() = default;
        consteval Property(const char* s) { for (int i = 0; s[i] != '\0' && i < 255; ++i) raw[i] = s[i]; }
    };
    // Field-level side-effect on set (ADR 0009 commitment #4): generated setters run comp.<method>()
    // after the assignment, so physics-synced fields (Rigidbody2D->Box2D) need no hand-written glue.
    struct OnSet {
        char method[64]{};                      // name of a zero-arg member function to call after set
        consteval OnSet(const char* s) { for (int i = 0; s[i] != '\0' && i < 63; ++i) method[i] = s[i]; }
    };
    // Marks a member FUNCTION script-exposed (the schema's reserved kind="method"). Reflection reads
    // its return type + parameter types directly, so methods flow through the neutral schema too.
    struct Method {};
}
#define OLO_METHOD(...)   [[=::OloEngine::Reflect::Method{}]]

#define OLO_CLAMP(mn, mx) [[=::OloEngine::Reflect::Clamp{ (double)(mn), (double)(mx) }]]
#define OLO_SKIP          [[=::OloEngine::Reflect::Skip{}]]
#define OLO_KEY(k)        [[=::OloEngine::Reflect::Key{ k }]]
#define OLO_REJECT(mn,mx) [[=::OloEngine::Reflect::Reject{ (double)(mn), (double)(mx) }]]
#define OLO_FLATTEN       [[=::OloEngine::Reflect::Flatten{}]]

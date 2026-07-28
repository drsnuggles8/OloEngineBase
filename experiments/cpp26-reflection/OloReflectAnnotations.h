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
}

#define OLO_CLAMP(mn, mx) [[=::OloEngine::Reflect::Clamp{ (double)(mn), (double)(mx) }]]
#define OLO_SKIP          [[=::OloEngine::Reflect::Skip{}]]
#define OLO_KEY(k)        [[=::OloEngine::Reflect::Key{ k }]]
#define OLO_REJECT(mn,mx) [[=::OloEngine::Reflect::Reject{ (double)(mn), (double)(mx) }]]
#define OLO_FLATTEN       [[=::OloEngine::Reflect::Flatten{}]]

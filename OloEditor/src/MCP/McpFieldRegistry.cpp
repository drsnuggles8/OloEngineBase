// The ONE translation unit that materialises the generated MCP writable-field
// registry (issue #607; see McpGenericFieldWrite.h for the full contract).
//
// Why a dedicated TU: the generated .inl is thousands of MakeFieldAccess /
// MakeSetterField template instantiations — two type-erased lambdas per field
// across ~100 components — and as an inline header function it was recompiled
// by EVERY includer of McpGenericFieldWrite.h (McpToolsScene.cpp + two MCP
// test TUs). Under clang-cl Release+ASan that pushed McpToolsScene.cpp past
// the CI runner's memory ("LLVM ERROR: out of memory") once the #635 + #633
// component additions landed. Here the instantiation storm compiles exactly
// once, in a TU that contains nothing else.
//
// Linked by BOTH consumers: OloEditor (OloEditor/src/CMakeLists.txt) and
// OloEngine-Tests (OloEngine/tests/CMakeLists.txt, MCP section — the test
// binary deliberately compiles the real registry, not a re-implementation).

// OloEnginePCH.h first, like every sibling MCP TU: it defines NOMINMAX before
// anything can pull in windows.h, so std::numeric_limits<T>::max() deeper in
// the include chain (ContainerAllocationPolicies.h) isn't macro-clobbered. The
// local VS-generator build force-includes the CMake PCH and masked its absence;
// the CI Ninja builds (MSVC Release + clang-cl ASan) do not.
#include "OloEnginePCH.h"

#include "MCP/McpGenericFieldWrite.h"

namespace OloEngine::MCP::GenericFieldWrite
{
    namespace
    {
// The generated .inl is a series of BuildRegistryChunkN functions — each a
// bounded run of
//   registry.push_back(OLO_GFW_FIELD(Comp, "FieldName", MemberExpr));
//   registry.push_back(OLO_GFW_FIELD_RANGE(Comp, "FieldName", MemberExpr, min, max));
// (plus MakeSetterField calls for OLO_PROPERTY getter/setter fields) — and a
// BuildRegistryChunks driver that calls them all. Chunked because compiler peak
// memory scales with the largest FUNCTION: as one flat BuildRegistry() body the
// ~1600 closure-heavy entries ran clang-cl Release+ASan out of memory on CI
// even with this dedicated-TU split. The macros are file-scoped and #undef'd
// below so they never leak out of this TU.
#define OLO_GFW_FIELD(Comp, FieldName, MemberExpr) \
    MakeFieldAccess<Comp>(#Comp, FieldName, [](Comp& c) -> auto& { return c.MemberExpr; })
#define OLO_GFW_FIELD_RANGE(Comp, FieldName, MemberExpr, MinBound, MaxBound) \
    MakeFieldAccess<Comp>(                                                   \
        #Comp, FieldName, [](Comp& c) -> auto& { return c.MemberExpr; },     \
        FieldRange{ MinBound, MaxBound })
#define OLO_GFW_BOUND(Expr) std::optional<double>(static_cast<double>(Expr))
#define OLO_GFW_NO_BOUND std::optional<double>()
#include "MCP/Generated/McpFieldRegistry.Generated.inl"
#undef OLO_GFW_FIELD
#undef OLO_GFW_FIELD_RANGE
#undef OLO_GFW_BOUND
#undef OLO_GFW_NO_BOUND

        // MorphTargetComponent::Weights (issue #607's map-key addressing slice) —
        // the ONE map-typed MCP field in the engine today, hand-registered rather
        // than generated: its keyset (target/bone name) doesn't exist until a
        // MorphTargetSet is bound at runtime, so no static field name could ever be
        // emitted for it the way a nested struct's dotted chain is (see
        // MakeMapKeyField's doc comment in McpGenericFieldWrite.h). A caller
        // addresses one entry via a dotted key ("Weights.Smile"); the range mirrors
        // MorphTargetComponent::SetWeight's own [0, 1] clamp so a write can never put
        // a weight outside what SetWeight would already allow.
        [[nodiscard]] FieldEntry BuildMorphTargetWeightsField()
        {
            return MakeMapKeyField<MorphTargetComponent, f32>(
                "MorphTargetComponent", "Weights",
                [](const MorphTargetComponent& c, const std::string& key) -> f32
                { return c.GetWeight(key); },
                [](MorphTargetComponent& c, const std::string& key, const f32& v)
                { c.SetWeight(key, v); },
                [](const MorphTargetComponent& c) -> std::vector<std::string>
                {
                    std::vector<std::string> keys;
                    keys.reserve(c.Weights.size());
                    for (const auto& [name, weight] : c.Weights)
                        keys.push_back(name);
                    return keys;
                },
                FieldRange{ std::optional<double>(0.0), std::optional<double>(1.0) });
        }

        [[nodiscard]] std::vector<FieldEntry> BuildRegistry()
        {
            std::vector<FieldEntry> registry;
            BuildRegistryChunks(registry);
            registry.push_back(BuildMorphTargetWeightsField());
            return registry;
        }
    } // namespace

    const std::vector<FieldEntry>& Registry()
    {
        static const std::vector<FieldEntry> s_Registry = BuildRegistry();
        return s_Registry;
    }
} // namespace OloEngine::MCP::GenericFieldWrite

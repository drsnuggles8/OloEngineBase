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

#include "MCP/McpGenericFieldWrite.h"

namespace OloEngine::MCP::GenericFieldWrite
{
    namespace
    {
// The generated .inl is a flat list of
//   registry.push_back(OLO_GFW_FIELD(Comp, "FieldName", MemberExpr));
//   registry.push_back(OLO_GFW_FIELD_RANGE(Comp, "FieldName", MemberExpr, min, max));
// plus MakeSetterField calls for OLO_PROPERTY getter/setter fields. The macros
// are file-scoped and #undef'd below so they never leak out of this TU.
#define OLO_GFW_FIELD(Comp, FieldName, MemberExpr) \
    MakeFieldAccess<Comp>(#Comp, FieldName, [](Comp& c) -> auto& { return c.MemberExpr; })
#define OLO_GFW_FIELD_RANGE(Comp, FieldName, MemberExpr, MinBound, MaxBound) \
    MakeFieldAccess<Comp>(                                                   \
        #Comp, FieldName, [](Comp& c) -> auto& { return c.MemberExpr; },     \
        FieldRange{ MinBound, MaxBound })
#define OLO_GFW_BOUND(Expr) std::optional<double>(static_cast<double>(Expr))
#define OLO_GFW_NO_BOUND std::optional<double>()
        [[nodiscard]] std::vector<FieldEntry> BuildRegistry()
        {
            std::vector<FieldEntry> registry;
#include "MCP/Generated/McpFieldRegistry.Generated.inl"
            return registry;
        }
#undef OLO_GFW_FIELD
#undef OLO_GFW_FIELD_RANGE
#undef OLO_GFW_BOUND
#undef OLO_GFW_NO_BOUND
    } // namespace

    const std::vector<FieldEntry>& Registry()
    {
        static const std::vector<FieldEntry> s_Registry = BuildRegistry();
        return s_Registry;
    }
} // namespace OloEngine::MCP::GenericFieldWrite

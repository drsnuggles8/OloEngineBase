// Compile-time PROOF that OloReflect's discovered set == the real engine's
// generated AllComponents tuple, member-for-member (both directions). No link.
#include "OloEngine/Scene/Components.h"
#include "OloReflect.h"
#include "real_tuple_names.h"       // constexpr std::string_view kRealTupleNames[] (from the .inl)
#include <string_view>
#include <iterator>

namespace sm = std::meta;
namespace R  = OloEngine::Reflect;

consteval bool EveryRealNameDiscovered() {
    auto disc = R::DiscoverComponents();
    for (std::string_view rn : kRealTupleNames) {
        bool found = false;
        for (sm::info d : disc) if (sm::identifier_of(d) == rn) { found = true; break; }
        if (!found) return false;                 // a generated-tuple component reflection missed
    }
    return true;
}
consteval bool EveryDiscoveredNameInReal() {
    for (sm::info d : R::DiscoverComponents()) {
        std::string_view dn = sm::identifier_of(d);
        bool found = false;
        for (std::string_view rn : kRealTupleNames) if (rn == dn) { found = true; break; }
        if (!found) return false;                 // reflection found a component not in the tuple
    }
    return true;
}

static_assert(R::DiscoverComponents().size() == std::size(kRealTupleNames),
              "count mismatch: discovered vs real tuple");
static_assert(EveryRealNameDiscovered(),
              "a real AllComponents member was NOT discovered by reflection");
static_assert(EveryDiscoveredNameInReal(),
              "reflection discovered a component NOT in the real AllComponents tuple");
// If this TU compiles, the two sets are identical member-for-member.

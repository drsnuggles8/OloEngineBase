// ============================================================================
//  THE real thing: OloReflect discovery over the ACTUAL engine Components.h
//  (all 86 real components). Replaces OloHeaderTool's whole-tree scan +
//  AllComponents.Generated.inl with a single namespace reflection.
//  GCC 16.1.0: -std=c++26 -freflection -fpermissive, real engine + PCH.
// ============================================================================
#include "OloEngine/Scene/Components.h"     // the REAL 86 components
#include "OloReflect.h"                      // discovery + tuple over ^^OloEngine
#include <cstdio>

namespace R = OloEngine::Reflect;

// --- minimal HAL stub ---------------------------------------------------------
// The ONLY engine symbol this discovery TU ODR-uses is RefUtils::Release (emitted
// for header-global Ref<> destructors). A no-op is safe here: nothing releases a
// Ref on the enumerate-and-print path, and any exit-time leak is irrelevant.
// Stubbing this LEAF avoids the Ref.cpp -> FMutex -> FMemory HAL cascade entirely.
namespace OloEngine::RefUtils { bool Release(RefCounted*) { return false; } }  // false = "not deleted" (no-op)

int main(int /*argc*/, char** /*argv*/) {   // signature matches the engine's befriended ::main
    std::printf("OloReflect discovered AllComponents arity: %zu\n\n", std::tuple_size_v<R::AllComponents>);
    int i = 0;
    R::for_each_component([&i]<class C>(std::type_identity<C>) {
        constexpr std::string_view n = R::ComponentName<C>();
        std::printf("%3d  %.*s\n", ++i, (int)n.size(), n.data());
    });
    return 0;
}

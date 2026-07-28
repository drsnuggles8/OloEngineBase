// ============================================================================
//  THE last part: RUN OloReflect's serializer on REAL, constructed engine
//  components (from the real Components.h) and diff vs the generated .inl.
//  GCC 16.1.0: -std=c++26 -freflection -fpermissive, real engine + PCH + 1 stub.
// ============================================================================
#include "OloEngine/Scene/Components.h"        // the REAL components
#include "OloEngine/Core/YAMLConverters.h"     // the REAL glm YAML converters
#include <cstdio>

#include "OloReflectYaml.h"                     // generic serializer over the real types
namespace R = OloEngine::Reflect;
using namespace OloEngine;

// same single leaf stub as the discovery build (no-op; nothing is released here)
namespace OloEngine::RefUtils { bool Release(RefCounted*) { return false; } }

int main(int /*argc*/, char** /*argv*/) {
    // all-trivial, all-public real components the serializer fully handles
    std::fputs(R::SerializeComponent(BuoyancyComponent{}).c_str(), stdout);
    std::fputs(R::SerializeComponent(CharacterController3DComponent{}).c_str(), stdout);
    std::fputs(R::SerializeComponent(SnowDeformerComponent{}).c_str(), stdout);
    std::fputs(R::SerializeComponent(RelationshipComponent{}).c_str(), stdout);
    return 0;
}

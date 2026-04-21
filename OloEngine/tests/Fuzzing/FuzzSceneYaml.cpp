// =============================================================================
// FuzzSceneYaml.cpp
//
// libFuzzer harness for `SceneSerializer::DeserializeFromYAML`. Scene files
// are user-authored YAML that go through yaml-cpp and then into ~80 component
// deserializers. This harness targets the full schema — unknown keys,
// malformed type tags, negative sizes on vectors/arrays, huge repetition
// counts, unterminated strings, circular `&anchor`/`*alias` references.
//
// The harness intentionally forwards raw bytes to the string-overload form
// so neither filesystem access nor encoding guesses sit between libFuzzer
// and the parser. A fresh empty `Scene` is constructed per input so leaked
// entity state from one input cannot influence the next.
// =============================================================================

#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Scene/SceneSerializer.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    // yaml-cpp pathological-depth inputs have historically triggered
    // stack-overflow at >10 MiB. Cap here so ASan can reliably report
    // the interesting failures under 64-bit stack limits.
    if (size > static_cast<size_t>(1 << 20))
        return 0;

    std::string yaml(reinterpret_cast<const char*>(data), size);

    auto scene = OloEngine::Scene::Create();
    if (!scene)
        return 0;

    OloEngine::SceneSerializer serializer(scene);
    (void)serializer.DeserializeFromYAML(yaml);

    return 0;
}

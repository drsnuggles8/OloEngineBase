// =============================================================================
// FuzzInputActionYaml.cpp
//
// libFuzzer harness for `InputActionSerializer::Deserialize`. The input
// action map is YAML and goes through yaml-cpp before reaching our schema
// code — both layers must be robust to arbitrary bytes. yaml-cpp has a
// documented history of OOM/stack-overflow on deeply-nested inputs; the
// harness intentionally forwards unmodified bytes so the fuzzer can
// explore both the YAML parser and our schema validation simultaneously.
// =============================================================================

#include "OloEngine/Core/InputActionSerializer.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace
{
    std::filesystem::path MakeTempPath()
    {
        auto dir = std::filesystem::temp_directory_path() / "olofuzz_input_action";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir / (std::string("in_") +
                      std::to_string(
#if defined(_WIN32)
                          static_cast<unsigned long>(::_getpid())
#else
                          static_cast<unsigned long>(::getpid())
#endif
                              ) +
                      ".yaml");
    }
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    // Size cap mirrors sibling harnesses (FuzzAssimpMesh, FuzzAnimationBinary).
    // 4 MB is well above any realistic input-action asset and keeps the
    // temp-file I/O cost bounded under ASan.
    if (size > 4 * 1024 * 1024)
        return 0;

    const auto path = MakeTempPath();
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
            return 0;
        if (size > 0)
            out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }

    (void)OloEngine::InputActionSerializer::Deserialize(path);

    std::error_code ec;
    std::filesystem::remove(path, ec);
    return 0;
}

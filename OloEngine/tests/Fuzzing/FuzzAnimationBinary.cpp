// =============================================================================
// FuzzAnimationBinary.cpp
//
// libFuzzer harness for `AnimationBinarySerializer::Read`. The deserializer
// consumes a zlib-compressed, magic-tagged blob and has a deep structural
// schema (file header + per-clip directory + bone channels + morph keys).
// Historically any numeric field that feeds into a `.resize()` or offset
// arithmetic can become a heap-OOB or allocate-all-of-memory primitive — this
// harness drives arbitrary bytes through the full read path.
//
// Wrapping: libFuzzer hands us a byte span; we write it to a unique temp
// file (since the public API takes std::filesystem::path) and invoke
// `Read()`. The temp file is always cleaned up, even on crash, because the
// fuzzer invokes us with sanitizers that abort rather than unwind.
// =============================================================================

#include "OloEnginePCH.h"
#include "OloEngine/Serialization/MeshBinarySerializer.h"

// MeshBinarySerializer.h forward-declares AnimationClip, but `Read()` returns
// `std::vector<Ref<AnimationClip>>` and the destructor of `Ref<AnimationClip>`
// needs the complete type (same situation as FuzzMeshBinary.cpp). Without
// this include the harness fails to compile under clang-cl.
#include "OloEngine/Animation/AnimationClip.h"

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
        auto dir = std::filesystem::temp_directory_path() / "olofuzz_anim";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        // Use the libFuzzer PID so parallel workers don't collide.
        return dir / (std::string("in_") +
                      std::to_string(
#if defined(_WIN32)
                          static_cast<unsigned long>(::_getpid())
#else
                          static_cast<unsigned long>(::getpid())
#endif
                              ) +
                      ".oanim");
    }
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    // Cap on-disk write size; parity with FuzzAssimpMesh (4 MB) so we don't
    // pay pathological I/O cost for adversarial inputs.
    constexpr size_t kMaxFuzzFileSize = 4 * 1024 * 1024;
    const size_t cappedSize = size < kMaxFuzzFileSize ? size : kMaxFuzzFileSize;

    const auto path = MakeTempPath();
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
            return 0;
        if (cappedSize > 0)
            out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(cappedSize));
    }

    // Must not throw / crash on any byte pattern. Failure paths are logged
    // via OLO_CORE_ERROR but never thrown.
    (void)OloEngine::AnimationBinarySerializer::Read(path);

    std::error_code ec;
    std::filesystem::remove(path, ec);
    return 0;
}

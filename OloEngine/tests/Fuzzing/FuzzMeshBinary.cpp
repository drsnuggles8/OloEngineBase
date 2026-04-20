// =============================================================================
// FuzzMeshBinary.cpp
//
// libFuzzer harness for `MeshBinarySerializer::Read`. Same pattern as the
// AnimationBinary fuzzer: arbitrary bytes → temp file → Read(). The mesh
// format encodes vertex/index buffers via meshoptimizer plus a skeleton
// block, so coverage is broader than the animation format. Crashers tend
// to live in:
//   - meshopt decode with truncated buffers
//   - submesh directory offsets that underflow past the header
//   - bone name / morph target name length fields
// =============================================================================

#include "OloEngine/Serialization/MeshBinarySerializer.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace
{
    std::filesystem::path MakeTempPath()
    {
        auto dir = std::filesystem::temp_directory_path() / "olofuzz_mesh";
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
                      ".omesh");
    }
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    const auto path = MakeTempPath();
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
            return 0;
        if (size > 0)
            out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }

    (void)OloEngine::MeshBinarySerializer::Read(path);

    std::error_code ec;
    std::filesystem::remove(path, ec);
    return 0;
}

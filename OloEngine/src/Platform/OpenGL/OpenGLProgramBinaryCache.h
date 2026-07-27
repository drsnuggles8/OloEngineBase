#pragma once

#include "OloEngine/Core/Base.h"

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine
{
    // On-disk layout of an OpenGL program-binary cache file (*.cached_opengl.pgr):
    //
    //     [u32 format][N bytes of driver-specific program binary]
    //
    // The framing below is intentionally GL-free so it can be unit-tested on a
    // CI runner without an OpenGL context. The bug that motivated extracting it
    // (issue #267) was a hand-copied loader on the AMD path that read the wrong
    // number of bytes — sizing the buffer to the *whole* file (header included)
    // and then re-reading `fileSize` bytes after already consuming the 4-byte
    // header, which over-reads by 4, trips the stream failbit, and hands
    // glProgramBinary a buffer 4 bytes too long with an uninitialised tail.
    // Centralising the framing means every load/save path is correct by
    // construction and the round-trip is pinned by ShaderBinaryCacheRoundTripTest.
    struct ProgramBinary
    {
        u32 Format = 0;
        std::vector<char> Data;
    };

    // Parse a program-binary cache file from a binary input stream. The stream is
    // repositioned internally; on entry it need only be open in binary mode.
    //
    // Returns std::nullopt on any framing error: a file smaller than the 4-byte
    // format header, or a short read of either the header or the payload. On
    // success, `Data` holds exactly (fileSize - sizeof(u32)) bytes — never the
    // header, never a garbage tail.
    [[nodiscard]] std::optional<ProgramBinary> ReadProgramBinary(std::istream& in);

    // Write the [format][data] framing to a binary output stream. Returns false if
    // the stream is in a failed state after the writes.
    bool WriteProgramBinary(std::ostream& out, u32 format, const char* data, sizet dataSize);

    // Result of reconciling the program-binary cache with the current driver.
    struct DriverStampSyncResult
    {
        bool Mismatched = false;   // true when the stored stamp differed (or none existed)
        u32 RemovedBinaries = 0;   // .pgr files deleted because they predate this driver
        std::string PreviousStamp; // empty on a fresh cache
    };

    // A GL program binary is only loadable by the exact driver build that
    // produced it (glProgramBinary otherwise rejects it as "not compatible with
    // current driver/hardware combination"). A driver update therefore
    // invalidates every saved .pgr at once, which used to surface as one WARN
    // per shader (~105 lines) on every launch. This helper keeps a driver
    // identity stamp file in the cache directory and, when the given stamp
    // differs from the stored one, deletes every `*.cached_opengl.pgr` in one
    // sweep and stores the new stamp — so a driver change costs a single quiet
    // rebuild instead of a rejection flood.
    //
    // GL-free by design (the caller supplies the stamp string, typically
    // GL_VENDOR|GL_RENDERER|GL_VERSION) so it is unit-testable without a GL
    // context — same rationale as the framing helpers above.
    DriverStampSyncResult SyncProgramBinaryCacheDriverStamp(const std::filesystem::path& cacheDirectory,
                                                            std::string_view driverStamp);

    // Name of the stamp file SyncProgramBinaryCacheDriverStamp maintains inside
    // the cache directory. Exposed for the cache-content validity test.
    inline constexpr const char* kProgramBinaryDriverStampFileName = "program_binary_driver_stamp.txt";
} // namespace OloEngine

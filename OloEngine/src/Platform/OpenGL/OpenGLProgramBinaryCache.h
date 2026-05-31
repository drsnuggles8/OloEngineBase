#pragma once

#include "OloEngine/Core/Base.h"

#include <iosfwd>
#include <optional>
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
} // namespace OloEngine

// =============================================================================
// ShaderBinaryCacheRoundTripTest.cpp
//
// Pins the on-disk framing of the OpenGL program-binary cache
// (*.cached_opengl.pgr), whose layout is:
//
//     [u32 format][N bytes of driver program binary]
//
// Regression guard for issue #267. The AMD load path used to size its read
// buffer to the *whole* file (4-byte header included) and then, after
// consuming the 4-byte format header, read `fileSize` more bytes — an
// over-read by exactly sizeof(u32). That tripped the stream failbit, left the
// final 4 bytes of the buffer uninitialised, and handed glProgramBinary a
// buffer 4 bytes too long with a garbage tail, so the cache *never* hit on
// AMD/Mesa (every shader re-cross-compiled on every launch).
//
// The framing now lives in one GL-free place
// (Platform/OpenGL/OpenGLProgramBinaryCache.{h,cpp}) shared by every load/save
// path, so it can be verified on a CI runner without a GPU. The invariant that
// caught nobody before and matters most here:
//
//     ReadProgramBinary must return exactly (fileSize - sizeof(u32)) payload
//     bytes — never the header, never a garbage tail.
//
// Classification: L3 (data round-trip), pure CPU, no GL context needed.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "Platform/OpenGL/OpenGLProgramBinaryCache.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        // A payload whose final bytes are a recognisable sentinel — the #267 bug
        // corrupted/dropped precisely the tail, so guarding it explicitly is the point.
        std::vector<char> MakePayload(sizet count)
        {
            std::vector<char> data(count);
            for (sizet i = 0; i < count; ++i)
                data[i] = static_cast<char>((i * 7 + 1) & 0xFF);
            if (count >= 4)
            {
                data[count - 4] = static_cast<char>(0xCA);
                data[count - 3] = static_cast<char>(0xFE);
                data[count - 2] = static_cast<char>(0xBA);
                data[count - 1] = static_cast<char>(0xBE);
            }
            return data;
        }

        std::stringstream MakeBinaryStream()
        {
            return std::stringstream(std::ios::in | std::ios::out | std::ios::binary);
        }
    } // namespace

    TEST(ShaderBinaryCacheRoundTrip, RoundTripIsByteExact)
    {
        const u32 format = 0xDEADBEEFu;
        const std::vector<char> payload = MakePayload(256);

        std::stringstream ss = MakeBinaryStream();
        ASSERT_TRUE(WriteProgramBinary(ss, format, payload.data(), payload.size()));

        const std::optional<ProgramBinary> result = ReadProgramBinary(ss);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->Format, format);
        ASSERT_EQ(result->Data.size(), payload.size());
        EXPECT_EQ(result->Data, payload);
    }

    // The single invariant #267 violated: the recovered payload length is the
    // file size minus the 4-byte format header — not the whole file, not the
    // whole file with a clipped/garbage tail.
    TEST(ShaderBinaryCacheRoundTrip, RecoveredDataLengthExcludesHeader)
    {
        const std::vector<char> payload = MakePayload(100);

        std::stringstream ss = MakeBinaryStream();
        ASSERT_TRUE(WriteProgramBinary(ss, 1u, payload.data(), payload.size()));

        // Total bytes on the wire = header + payload.
        ss.seekg(0, std::ios::end);
        EXPECT_EQ(static_cast<sizet>(ss.tellg()), sizeof(u32) + payload.size());

        const std::optional<ProgramBinary> result = ReadProgramBinary(ss);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->Data.size(), payload.size());
        EXPECT_NE(result->Data.size(), sizeof(u32) + payload.size());
    }

    // The tail bytes must survive intact — the old over-read left them uninitialised.
    TEST(ShaderBinaryCacheRoundTrip, TailBytesSurvive)
    {
        const std::vector<char> payload = MakePayload(64);

        std::stringstream ss = MakeBinaryStream();
        ASSERT_TRUE(WriteProgramBinary(ss, 7u, payload.data(), payload.size()));

        const std::optional<ProgramBinary> result = ReadProgramBinary(ss);
        ASSERT_TRUE(result.has_value());
        ASSERT_GE(result->Data.size(), static_cast<sizet>(4));
        EXPECT_EQ(static_cast<unsigned char>(result->Data[result->Data.size() - 4]), 0xCAu);
        EXPECT_EQ(static_cast<unsigned char>(result->Data[result->Data.size() - 3]), 0xFEu);
        EXPECT_EQ(static_cast<unsigned char>(result->Data[result->Data.size() - 2]), 0xBAu);
        EXPECT_EQ(static_cast<unsigned char>(result->Data[result->Data.size() - 1]), 0xBEu);
    }

    TEST(ShaderBinaryCacheRoundTrip, RejectsFileSmallerThanHeader)
    {
        // Three bytes — can't even hold the 4-byte format header.
        std::stringstream ss = MakeBinaryStream();
        ss.write("\x01\x02\x03", 3);

        const std::optional<ProgramBinary> result = ReadProgramBinary(ss);
        EXPECT_FALSE(result.has_value());
    }

    TEST(ShaderBinaryCacheRoundTrip, RejectsEmptyStream)
    {
        std::stringstream ss = MakeBinaryStream();
        const std::optional<ProgramBinary> result = ReadProgramBinary(ss);
        EXPECT_FALSE(result.has_value());
    }

    // A header with no payload is well-framed: format parsed, zero data bytes.
    // (glProgramBinary will reject a zero-length binary downstream and the loader
    // falls back to recompilation — but that's the GL layer's call, not framing's.)
    TEST(ShaderBinaryCacheRoundTrip, HeaderOnlyYieldsEmptyPayload)
    {
        const u32 format = 0x12345678u;

        std::stringstream ss = MakeBinaryStream();
        ASSERT_TRUE(WriteProgramBinary(ss, format, nullptr, 0));

        const std::optional<ProgramBinary> result = ReadProgramBinary(ss);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->Format, format);
        EXPECT_TRUE(result->Data.empty());
    }

    TEST(ShaderBinaryCacheRoundTrip, LargeBinaryRoundTrip)
    {
        const std::vector<char> payload = MakePayload(64 * 1024 + 13); // non-power-of-two size
        std::stringstream ss = MakeBinaryStream();
        ASSERT_TRUE(WriteProgramBinary(ss, 0xABCDu, payload.data(), payload.size()));

        const std::optional<ProgramBinary> result = ReadProgramBinary(ss);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->Format, 0xABCDu);
        EXPECT_EQ(result->Data, payload);
    }

    // Exercises the exact stream flags the production loader/saver use
    // (std::ios::binary on disk), not just an in-memory stringstream.
    TEST(ShaderBinaryCacheRoundTrip, RoundTripThroughRealFile)
    {
        const u32 format = 0xFEEDFACEu;
        const std::vector<char> payload = MakePayload(513);

        const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "olo_shader_bincache_roundtrip_test.pgr";
        std::filesystem::remove(path);

        {
            std::ofstream out(path, std::ios::out | std::ios::binary);
            ASSERT_TRUE(out.is_open());
            ASSERT_TRUE(WriteProgramBinary(out, format, payload.data(), payload.size()));
        }

        {
            std::ifstream in(path, std::ios::binary);
            ASSERT_TRUE(in.is_open());
            const std::optional<ProgramBinary> result = ReadProgramBinary(in);
            ASSERT_TRUE(result.has_value());
            EXPECT_EQ(result->Format, format);
            EXPECT_EQ(result->Data, payload);
        }

        std::filesystem::remove(path);
    }
} // namespace OloEngine::Tests

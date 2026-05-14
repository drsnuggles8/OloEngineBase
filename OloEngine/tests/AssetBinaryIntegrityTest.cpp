// =============================================================================
// AssetBinaryIntegrityTest.cpp
//
// Catches truncated / corrupted binary asset files committed to git.
// A `.png` that's missing its trailing IDAT chunks, or a `.wav` whose
// RIFF header was clipped by a merge tool, would surface in OloEditor
// as "the texture loads as the missing-texture checker pattern" or
// "the sound plays as silence" — silent failures that depend on the
// importer's error handling. We catch them here as a much sharper
// "this on-disk file is malformed" diagnostic.
//
// What this test does
// -------------------
//   For every `.png` / `.wav` under `OloEditor/SandboxProject/Assets/`:
//     1. Open the file.
//     2. Verify the first N bytes match the format's magic bytes.
//     3. For WAV, also verify the RIFF "WAVE" chunk identifier four bytes
//        later (RIFF<size>WAVE is the standard prefix).
//   Aggregate failures so one truncated file doesn't mask others.
//
// What this test does NOT do
// --------------------------
//   * Decode the full asset (libpng / a real WAV decoder). The goal is
//     to catch hard truncation / corruption at the format-prefix level,
//     not to validate every byte. A `.png` with a valid header but a
//     truncated tail is the importer's problem to surface.
//   * Cover every binary format. PNG and WAV are the formats actually
//     used in the sandbox today; new types should be added here as they
//     ship.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        std::vector<fs::path> EnumerateByExtension(const fs::path& root, std::string_view extension)
        {
            std::vector<fs::path> out;
            std::error_code ec;
            if (!fs::exists(root, ec)) return out;
            for (auto& entry : fs::recursive_directory_iterator(root, ec))
            {
                if (ec) break;
                if (!entry.is_regular_file()) continue;
                if (entry.path().extension() == extension)
                    out.push_back(entry.path());
            }
            std::sort(out.begin(), out.end());
            return out;
        }

        struct Failure
        {
            std::string Path;
            std::string Reason;
        };

        // PNG magic: 89 50 4E 47 0D 0A 1A 0A
        constexpr std::array<u8, 8> kPngMagic{
            0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
        };

        // WAV: 'R' 'I' 'F' 'F' <4 size bytes> 'W' 'A' 'V' 'E'
        constexpr std::string_view kWavRiff = "RIFF";
        constexpr std::string_view kWavWave = "WAVE";

        fs::path AssetsRoot()
        {
            return fs::path{ OLO_TEST_EDITOR_ROOT } / "SandboxProject" / "Assets";
        }
    } // namespace

    // -------------------------------------------------------------------------
    // PNG magic-byte integrity
    // -------------------------------------------------------------------------
    TEST(AssetBinaryIntegrity, AllPngFilesHaveValidPngMagic)
    {
        const auto files = EnumerateByExtension(AssetsRoot(), ".png");
        std::vector<Failure> failures;

        for (const auto& path : files)
        {
            std::ifstream f(path, std::ios::binary);
            if (!f)
            {
                failures.push_back({ path.generic_string(), "could not open for read" });
                continue;
            }
            std::array<u8, kPngMagic.size()> header{};
            f.read(reinterpret_cast<char*>(header.data()), header.size());
            if (!f || static_cast<sizet>(f.gcount()) != header.size())
            {
                failures.push_back({ path.generic_string(),
                                     "file too short to contain a PNG header (need 8 bytes)" });
                continue;
            }
            if (header != kPngMagic)
            {
                std::ostringstream hex;
                for (u8 b : header)
                {
                    hex << std::hex << std::uppercase;
                    if (b < 0x10) hex << '0';
                    hex << static_cast<u32>(b) << ' ';
                }
                failures.push_back({ path.generic_string(),
                                     "PNG magic bytes mismatch — got: " + hex.str() +
                                     "(expected 89 50 4E 47 0D 0A 1A 0A)" });
            }
        }

        // Files set is not empty in the current sandbox, but we don't hard-
        // assert that — PNG-free projects are a legitimate state.
        if (!failures.empty())
        {
            std::ostringstream oss;
            oss << failures.size() << " .png file(s) failed magic-byte check:\n";
            for (const auto& f : failures)
                oss << "----\n" << f.Path << "\n    " << f.Reason << "\n";
            FAIL() << oss.str();
        }
    }

    // -------------------------------------------------------------------------
    // WAV RIFF/WAVE prefix integrity
    // -------------------------------------------------------------------------
    TEST(AssetBinaryIntegrity, AllWavFilesHaveValidRiffWavePrefix)
    {
        const auto files = EnumerateByExtension(AssetsRoot(), ".wav");
        std::vector<Failure> failures;

        for (const auto& path : files)
        {
            std::ifstream f(path, std::ios::binary);
            if (!f)
            {
                failures.push_back({ path.generic_string(), "could not open for read" });
                continue;
            }
            // First 4 bytes: "RIFF". Next 4: chunk size (ignored). Next 4: "WAVE".
            std::array<char, 12> header{};
            f.read(header.data(), header.size());
            if (!f || static_cast<sizet>(f.gcount()) != header.size())
            {
                failures.push_back({ path.generic_string(),
                                     "file too short to contain a RIFF/WAVE header (need 12 bytes)" });
                continue;
            }
            const std::string_view riff(header.data(), 4);
            const std::string_view wave(header.data() + 8, 4);
            if (riff != kWavRiff)
            {
                failures.push_back({ path.generic_string(),
                                     "missing RIFF prefix (got '" + std::string(riff) + "')" });
                continue;
            }
            if (wave != kWavWave)
            {
                failures.push_back({ path.generic_string(),
                                     "missing WAVE chunk identifier (got '" + std::string(wave) + "')" });
            }
        }

        if (!failures.empty())
        {
            std::ostringstream oss;
            oss << failures.size() << " .wav file(s) failed RIFF/WAVE prefix check:\n";
            for (const auto& f : failures)
                oss << "----\n" << f.Path << "\n    " << f.Reason << "\n";
            FAIL() << oss.str();
        }
    }
} // namespace OloEngine::Tests

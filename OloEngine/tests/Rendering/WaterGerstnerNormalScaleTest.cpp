// OLO_TEST_LAYER: L1
//
// Pins one invariant of `assets/shaders/include/WaterCommon.glsl ::
// sumGerstnerWaves` — issue #943.
//
// The function sums eight Gerstner octaves. Each octave adds its DISPLACEMENT
// scaled by `waveAmplitude * <octave weight>`, and separately accumulates a
// tangent/binormal perturbation that the surface NORMAL is built from:
//
//     displaced += gerstnerWave(pos, d, st, wl, t, ph) * waveAmplitude * 0.5;
//     gerstnerWaveNormal(pos, d, st, wl, t, ph, tangent, binormal);
//
// Both helpers are LINEAR in `steepness` (`gerstnerWave` derives its amplitude
// as `steepness / k`; `gerstnerWaveNormal`'s tangent/binormal terms are each a
// bare multiple of `steepness`, and its phase `f` does not involve steepness at
// all). So an octave whose displacement is scaled by `waveAmplitude * w`
// describes a wave whose real steepness is `steepness * waveAmplitude * w` —
// and that is the steepness its normal has to be derived from.
//
// Originally the normal calls passed the RAW `st`, so every octave's normal was
// derived for a full-amplitude wave the surface never had. Two consequences,
// both visible and neither caught by any existing test:
//
//   * `WaveAmplitude` did not affect shading normals at all. Drift authors
//     0.12, so the sea shaded as if its slopes were ~8x steeper than the
//     geometry actually was — confirmed live by setting WaveAmplitude to 1e-4
//     and watching the surface stay fully rippled while the geometry flattened.
//   * the per-octave weights (0.55 ... 0.1) were ignored by the normal, so the
//     finest octave contributed 10% of the displacement but 100% of the slope.
//     The normal therefore carried far more high-frequency energy than the
//     surface it described, which is what aliased into the hard-edged flats
//     reported in #943 (and, on the FFT path's own normals, #898).
//
// This is a headless TEXT pin rather than a math test on a C++ mirror, and that
// is deliberate. The bug lives in GLSL, and `Renderer/WaterSurface.cpp` — the
// CPU mirror the buoyancy path samples — computes only the displacement delta
// and no normal at all, so a C++-side math test could not have caught it and
// would stay green through a re-break of the shader. What can regress is the
// pairing itself, so the pairing is what is asserted: for every octave, the
// scale factor applied to the normal's steepness argument must equal the scale
// factor applied to that octave's displacement.

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    // The repo's idiom for "a source-tree path, independent of the binary's
    // cwd" (ADR 0003); the cwd walk is the fallback for a standalone harness
    // that does not define the macro.
    [[nodiscard]] auto FindWaterCommon() -> fs::path
    {
#ifdef OLO_TEST_EDITOR_ROOT
        if (const fs::path fromRoot =
                fs::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders" / "include" / "WaterCommon.glsl";
            fs::exists(fromRoot))
        {
            return fromRoot;
        }
#endif
        fs::path candidate = fs::current_path();
        for (int depth = 0; depth < 6; ++depth)
        {
            for (const char* relative : { "assets/shaders/include/WaterCommon.glsl",
                                          "OloEditor/assets/shaders/include/WaterCommon.glsl" })
            {
                if (fs::exists(candidate / relative))
                {
                    return candidate / relative;
                }
            }
            if (!candidate.has_parent_path() || candidate == candidate.parent_path())
            {
                break;
            }
            candidate = candidate.parent_path();
        }
        return {};
    }

    [[nodiscard]] auto ReadFile(const fs::path& path) -> std::string
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            return {};
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    // "* waveAmplitude * 0.55" -> "0.55"; absent -> "" (meaning unscaled).
    [[nodiscard]] auto ExtractAmplitudeWeight(const std::string& text) -> std::string
    {
        static const std::regex kWeight(R"(\*\s*waveAmplitude\s*\*\s*([0-9]*\.?[0-9]+))");
        std::smatch match;
        if (std::regex_search(text, match, kWeight))
        {
            return match[1].str();
        }
        return {};
    }
} // namespace

// Every `displaced += gerstnerWave(...)` must be followed by a
// `gerstnerWaveNormal(...)` whose steepness argument carries the SAME
// `waveAmplitude * weight` scale. A normal derived from an unscaled steepness
// describes a different wave than the one the vertex was displaced onto.
TEST(WaterGerstnerNormalScale, EveryOctaveScalesItsNormalLikeItsDisplacement)
{
    const fs::path path = FindWaterCommon();
    ASSERT_FALSE(path.empty()) << "WaterCommon.glsl not found from " << fs::current_path();

    const std::string source = ReadFile(path);
    ASSERT_FALSE(source.empty()) << "failed to read " << path;

    // Pair each displacement line with the normal call that follows it. Both
    // appear once per octave, displacement first, in sumGerstnerWaves.
    static const std::regex kPair(
        R"(displaced\s*\+=\s*gerstnerWave\([^;]*;\s*\n\s*gerstnerWaveNormal\(([^;]*)\);)");

    std::vector<std::pair<std::string, std::string>> octaves; // displacement text, normal args
    for (auto it = std::sregex_iterator(source.begin(), source.end(), kPair);
         it != std::sregex_iterator(); ++it)
    {
        octaves.emplace_back(it->str(), (*it)[1].str());
    }

    // 2 artist-controlled primaries + 6 procedural detail octaves. If this
    // count changes the octave set was edited — extend the check, don't relax
    // it, or a new octave silently reintroduces the mismatch.
    ASSERT_EQ(octaves.size(), 8u)
        << "expected 8 displacement/normal pairs in sumGerstnerWaves, found " << octaves.size()
        << " in " << path;

    for (std::size_t i = 0; i < octaves.size(); ++i)
    {
        const auto& [pairText, normalArgs] = octaves[i];

        // The displacement's own `* waveAmplitude * W` — everything before the
        // normal call in the matched pair.
        const std::string displacementText = pairText.substr(0, pairText.find("gerstnerWaveNormal"));
        const std::string displacementWeight = ExtractAmplitudeWeight(displacementText);
        ASSERT_FALSE(displacementWeight.empty())
            << "octave " << i << ": displacement is not scaled by `waveAmplitude * <weight>`;"
                                 " the pairing this test checks no longer applies. Pair:\n"
            << pairText;

        const std::string normalWeight = ExtractAmplitudeWeight(normalArgs);
        EXPECT_EQ(normalWeight, displacementWeight)
            << "octave " << i << ": displacement is scaled by `waveAmplitude * " << displacementWeight
            << "` but its normal is built from steepness scaled by `"
            << (normalWeight.empty() ? std::string("<nothing>") : normalWeight)
            << "`.\nBoth gerstnerWave() and gerstnerWaveNormal() are linear in steepness, so the"
               " normal must carry the same factor or it describes a wave the surface never had"
               " (issue #943 — WaveAmplitude stopped affecting shading normals entirely, and the"
               " finest octave contributed 10% of the displacement but 100% of the slope).\nPair:\n"
            << pairText;
    }
}

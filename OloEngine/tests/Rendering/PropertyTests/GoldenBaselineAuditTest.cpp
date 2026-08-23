// OLO_TEST_LAYER: L9
// =============================================================================
// GoldenBaselineAuditTest.cpp
//
// Layer 9 — Cross-vendor conformance. The first *code* in this layer; until
// now L9 was a workflow (`.github/workflows/cross-vendor.yml`) and nothing
// else.
//
// WHAT THIS ANSWERS THAT NOTHING ELSE DOES
// ----------------------------------------
//   GoldenImageTests / AtmosphereVisualEvidenceTest compare a LIVE render
//   against a recorded PNG. That answers "did it change?". #734 added property
//   guards so they also answer "is the effect still doing its job?" — but only
//   for the frame the running GPU just produced. The recorded baseline itself
//   is never audited.
//
//   That gap has a name (GoldenImageTests.cpp, the #734 comment block):
//
//       "a baseline is only ever a recording of one machine: where a vendor
//        baseline was baked separately (#735, assets/tests/golden/amd) the
//        compare can pass on a frame nobody has verified is correct."
//
//   A per-vendor baseline set is baked on hardware the reviewer does not have.
//   If the bake captured a defect, every later run on that vendor compares
//   green against the defect and the nightly *defends* it. The concrete
//   precedent is in 5d776ba2's own commit message: integer-format textures
//   sampled with GL_LINEAR read as all-zero on Mesa, so every glyph in the
//   engine was invisible on AMD while the font loaded, 189 glyphs packed and
//   852 quads were submitted with nothing logged. Had the baselines been baked
//   before that fix, a blank UI would now be the reference.
//
//   So this file evaluates #734's invariants against the COMMITTED PNGs, for
//   every baseline set on disk — the shared one and each vendor subdirectory.
//   A vendor bake that froze a defect fails here, on any machine, without that
//   vendor's hardware.
//
// WHY IT IS A SECOND IMPLEMENTATION, DELIBERATELY
// -----------------------------------------------
//   The #734 guards live inline in the golden tests' bodies and close over GPU
//   readbacks, so they cannot be called from here. More importantly they
//   SHOULD not be: this test's input is the recorded artefact, theirs is a live
//   frame. Sharing code would tie the audit's fate to a GPU context and lose
//   the property that makes it useful — it runs anywhere, including headless
//   CI with no GL at all.
//
//   Thresholds below are mirrored from the #734 guards on purpose and each one
//   names its source assertion. When a guard's threshold moves there, move it
//   here; `GoldenBaselineAuditCoverage` fails loudly if the audit stops seeing
//   the files it is supposed to police, which is the failure mode that would
//   otherwise make all of this pass vacuously.
//
// NO GPU REQUIRED. Deliberately no OLO_ENSURE_GPU_OR_SKIP() — file reads and
// CPU math only.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"

#include <stb_image/stb_image.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        // The suite's CTest WORKING_DIRECTORY is <repo>/OloEditor (see
        // OloEngine/tests/CMakeLists.txt), which is why every other test spells
        // these paths "assets/...". Probing the repo-root spelling too keeps an
        // ad-hoc `build/.../OloEngine-Tests.exe --gtest_filter=GoldenBaseline*`
        // from the repo root working, instead of reporting "no baselines" —
        // which would look exactly like a genuine audit pass.
        [[nodiscard]] fs::path BaselineRoot()
        {
            const std::array<fs::path, 2> candidates{
                fs::path("assets") / "tests",
                fs::path("OloEditor") / "assets" / "tests",
            };
            for (const fs::path& candidate : candidates)
            {
                std::error_code ec;
                if (fs::exists(candidate / "golden", ec))
                    return candidate;
            }
            return candidates[0];
        }

        struct Image
        {
            std::vector<u8> m_Rgba;
            u32 m_Width = 0;
            u32 m_Height = 0;

            [[nodiscard]] bool Valid() const
            {
                return m_Width > 0 && m_Height > 0 &&
                       m_Rgba.size() == static_cast<std::size_t>(m_Width) * m_Height * 4u;
            }

            [[nodiscard]] u8 At(u32 x, u32 y, u32 channel) const
            {
                return m_Rgba[((static_cast<std::size_t>(y) * m_Width) + x) * 4u + channel];
            }
        };

        // Baselines were written by stbi_write_png straight from a GL readback,
        // unflipped. Clear BOTH flip flags before loading (the thread-local one
        // takes precedence and production asset paths set it) or the image
        // arrives upside-down and every orientation-sensitive guard below
        // becomes meaningless — the same trap CompareOrBootstrap documents.
        [[nodiscard]] Image LoadBaseline(const fs::path& path)
        {
            Image image;
            ::stbi_set_flip_vertically_on_load(0);
            ::stbi_set_flip_vertically_on_load_thread(0);
            int w = 0;
            int h = 0;
            int channels = 0;
            stbi_uc* raw = ::stbi_load(path.string().c_str(), &w, &h, &channels, 4);
            if (raw == nullptr)
                return image;
            image.m_Width = static_cast<u32>(w);
            image.m_Height = static_cast<u32>(h);
            image.m_Rgba.assign(raw, raw + (static_cast<std::size_t>(w) * h * 4));
            ::stbi_image_free(raw);
            return image;
        }

        // The files every baseline set is expected to hold. Defined ONCE, at
        // namespace scope, because three things consume them — the coverage
        // guard, the cross-vendor parity check, and (for the goldens) the
        // per-file audits. Two copies of these lists drifting apart would mean
        // a baseline silently policed by one of them and not the others.
        constexpr std::array<const char*, 4> kRendererGoldenFiles{
            "fxaa_hard_edge.png",
            "scene_shadow_integration.png",
            "scene_splatmap_integration.png",
            "tonemap_reinhard_hdr_ramp.png",
        };
        constexpr std::array<const char*, 13> kAtmosphereFiles{
            "Atmosphere_DawnClear.png",
            "Atmosphere_DawnOvercast.png",
            "Atmosphere_DawnStorm.png",
            "Atmosphere_NoonClear.png",
            "Atmosphere_NoonOvercast.png",
            "Atmosphere_NoonStorm.png",
            "Atmosphere_NoonClearAerial.png",
            "Atmosphere_DuskClear.png",
            "Atmosphere_DuskOvercast.png",
            "Atmosphere_DuskStorm.png",
            "Atmosphere_NightClear.png",
            "Atmosphere_NightOvercast.png",
            "Atmosphere_NightStorm.png",
        };

        // The Drift weather/time-of-day matrix (issue #882). A SEPARATE list
        // from kAtmosphereFiles even though the two grids look alike, because
        // they are different pictures with different band layouts: the
        // Atmosphere set is a lit ground plane and reads a "ground" band from
        // the bottom 25%, while Drift's subject is open water and reads a
        // "sea" band from the bottom 40%, below a horizon that sits lower in
        // frame. Folding them together would compare the wrong rows.
        constexpr std::array<const char*, 13> kDriftFiles{
            "Drift_DawnClear.png",
            "Drift_DawnOvercast.png",
            "Drift_DawnStorm.png",
            "Drift_NoonClear.png",
            "Drift_NoonOvercast.png",
            "Drift_NoonStorm.png",
            "Drift_DuskClear.png",
            "Drift_DuskOvercast.png",
            "Drift_DuskStorm.png",
            "Drift_NightClear.png",
            "Drift_NightOvercast.png",
            "Drift_NightStorm.png",
            "Drift_DawnHero.png",
        };

        // One baseline set: the shared root, or a per-vendor subdirectory.
        struct BaselineSet
        {
            std::string m_Label;
            fs::path m_GoldenDir;
            fs::path m_VisualDir;
        };

        // True when a directory holds at least one .png, readable or not.
        // "Holds a PNG" rather than "holds a READABLE PNG" on purpose: a
        // truncated or mis-encoded baseline must still count as a set, so the
        // coverage guard can report it, instead of vanishing from discovery
        // and being silently policed by nobody.
        [[nodiscard]] bool HoldsAnyPng(const fs::path& dir)
        {
            std::error_code ec;
            if (!fs::is_directory(dir, ec))
                return false;
            for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec))
            {
                if (entry.is_regular_file() && entry.path().extension() == ".png")
                    return true;
            }
            return false;
        }

        // Discovered rather than hard-coded, so a vendor set added later
        // (intel/, llvmpipe/, ...) is audited the day it lands without anyone
        // remembering to edit this file.
        //
        // A vendor directory holding NO PNG is not a baseline set and is
        // excluded here: CompareOrBootstrap (GoldenImageTests.cpp) calls
        // fs::create_directories on the composed path UNCONDITIONALLY, before
        // it checks whether the baseline exists, so every run passing
        // --olo-golden-vendor=<v> leaves an empty golden/<v>/ behind even when
        // it then fails. cross-vendor.yml does exactly that with llvmpipe.
        // Counting those would make the parity check below fail with "nothing
        // was cross-compared" for a reason that has nothing to do with
        // baselines — reproduced before this filter existed.
        [[nodiscard]] std::vector<BaselineSet> DiscoverBaselineSets()
        {
            const fs::path root = BaselineRoot();
            std::vector<BaselineSet> sets;
            sets.push_back(BaselineSet{ "shared", root / "golden", root / "visual" });

            std::error_code ec;
            std::vector<std::string> vendors;
            for (const fs::path& parent : { root / "golden", root / "visual" })
            {
                if (!fs::is_directory(parent, ec))
                    continue;
                for (const fs::directory_entry& entry : fs::directory_iterator(parent, ec))
                {
                    if (!entry.is_directory())
                        continue;
                    std::string name = entry.path().filename().string();
                    if (std::find(vendors.begin(), vendors.end(), name) == vendors.end())
                        vendors.push_back(std::move(name));
                }
            }
            std::sort(vendors.begin(), vendors.end());
            for (const std::string& vendor : vendors)
            {
                BaselineSet set{ vendor, root / "golden" / vendor, root / "visual" / vendor };
                if (HoldsAnyPng(set.m_GoldenDir) || HoldsAnyPng(set.m_VisualDir))
                    sets.push_back(std::move(set));
            }
            return sets;
        }

        // ── shared helpers, mirroring the #734 guards ─────────────────────────

        [[nodiscard]] f32 Rec601Luma(u8 r, u8 g, u8 b)
        {
            return 0.299f * static_cast<f32>(r) + 0.587f * static_cast<f32>(g) + 0.114f * static_cast<f32>(b);
        }

        struct ColorPopulation
        {
            u32 m_Rgb = 0;
            u32 m_Count = 0;
        };

        [[nodiscard]] std::vector<ColorPopulation> RankColorsByPopulation(const Image& image)
        {
            std::unordered_map<u32, u32> counts;
            for (std::size_t i = 0; i + 3 < image.m_Rgba.size(); i += 4)
            {
                const u32 key = (static_cast<u32>(image.m_Rgba[i + 0]) << 16) |
                                (static_cast<u32>(image.m_Rgba[i + 1]) << 8) |
                                static_cast<u32>(image.m_Rgba[i + 2]);
                ++counts[key];
            }
            std::vector<ColorPopulation> ranked;
            ranked.reserve(counts.size());
            for (const auto& [rgb, count] : counts)
                ranked.push_back(ColorPopulation{ rgb, count });
            std::sort(ranked.begin(), ranked.end(),
                      [](const ColorPopulation& a, const ColorPopulation& b)
                      { return a.m_Count > b.m_Count; });
            return ranked;
        }

        [[nodiscard]] std::array<f32, 3> PatchMeanRgb(const Image& image, u32 cx, u32 cy, u32 radius)
        {
            std::array<f64, 3> sum{ 0.0, 0.0, 0.0 };
            u32 taps = 0;
            const u32 x0 = cx > radius ? cx - radius : 0u;
            const u32 y0 = cy > radius ? cy - radius : 0u;
            const u32 x1 = std::min(cx + radius, image.m_Width - 1u);
            const u32 y1 = std::min(cy + radius, image.m_Height - 1u);
            for (u32 y = y0; y <= y1; ++y)
            {
                for (u32 x = x0; x <= x1; ++x)
                {
                    for (u32 c = 0; c < 3; ++c)
                        sum[c] += static_cast<f64>(image.At(x, y, c));
                    ++taps;
                }
            }
            const f64 n = taps > 0 ? static_cast<f64>(taps) : 1.0;
            return { static_cast<f32>(sum[0] / n), static_cast<f32>(sum[1] / n), static_cast<f32>(sum[2] / n) };
        }

        struct BandStats
        {
            f64 m_R = 0.0;
            f64 m_G = 0.0;
            f64 m_B = 0.0;

            [[nodiscard]] f64 Luma() const
            {
                return 0.2126 * m_R + 0.7152 * m_G + 0.0722 * m_B;
            }
        };

        // Mirrors AtmosphereVisualEvidenceTest::MeanBand over a top-down RGBA8
        // capture; the committed PNGs are already top-down (Capture() flips
        // before writing).
        [[nodiscard]] BandStats MeanBandPercent(const Image& image, u32 loPercent, u32 hiPercent)
        {
            BandStats stats;
            const u32 rowBegin = image.m_Height * loPercent / 100u;
            const u32 rowEnd = image.m_Height * hiPercent / 100u;
            u64 count = 0;
            for (u32 y = rowBegin; y < rowEnd; ++y)
            {
                for (u32 x = 0; x < image.m_Width; ++x)
                {
                    stats.m_R += image.At(x, y, 0);
                    stats.m_G += image.At(x, y, 1);
                    stats.m_B += image.At(x, y, 2);
                    ++count;
                }
            }
            if (count > 0)
            {
                stats.m_R /= static_cast<f64>(count);
                stats.m_G /= static_cast<f64>(count);
                stats.m_B /= static_cast<f64>(count);
            }
            return stats;
        }

        // A missing baseline is NOT a failure here: a vendor set is allowed to
        // cover only part of the matrix (the AMD set has all 4 goldens and all
        // 13 Atmosphere captures; a future one might not). Completeness of the
        // SHARED set is asserted separately, by GoldenBaselineAuditCoverage —
        // keeping "this file is absent" and "this file is wrong" as distinct
        // failures rather than one ambiguous red.
        [[nodiscard]] bool LoadForAudit(const fs::path& path, Image& out)
        {
            out = LoadBaseline(path);
            if (!out.Valid())
                return false;
            return true;
        }

        // ── cross-vendor comparison (§ VendorSetsAgreeWithTheSharedSet) ──
        //
        // Two metrics, because they fail in opposite directions and the whole
        // point of the cross-vendor check is to not confuse them:
        //
        //   * PIXEL metrics are vendor-sensitive AND drift-sensitive. Two sets
        //     baked a month apart differ far more from calendar than from
        //     silicon (#735 measured 2.60 of drift against 0.72 of vendor), so
        //     a tight pixel gate across vendors reports bake-date skew as a
        //     conformance bug.
        //   * DERIVED band quantities are vendor-sensitive but drift-INsensitive
        //     — across that same month of skew the worst band-luminance
        //     disagreement was 0.43/255, while pixel RMSE ranged to 2.60.
        //
        // So the frames that are procedural and drift-free (the four renderer
        // goldens) get the tight pixel gate, and the scene captures get the
        // derived gate plus a loose pixel backstop.

        // GoldenImageTests::ComputeRgbRmse — RGB RMSE normalised to [0, 1].
        [[nodiscard]] f32 ComputeRgbRmseUnit(const Image& a, const Image& b)
        {
            const std::size_t pixels = a.m_Rgba.size() / 4;
            f64 sumSq = 0.0;
            for (std::size_t i = 0; i < pixels; ++i)
            {
                for (u32 c = 0; c < 3; ++c)
                {
                    const f64 d = (static_cast<f64>(a.m_Rgba[i * 4 + c]) -
                                   static_cast<f64>(b.m_Rgba[i * 4 + c])) /
                                  255.0;
                    sumSq += d * d;
                }
            }
            return static_cast<f32>(std::sqrt(sumSq / static_cast<f64>(pixels * 3)));
        }

        // AtmosphereVisualEvidenceTest::Rgba8Rmse — RGB RMSE in 0..255 units.
        [[nodiscard]] f64 ComputeRgbRmse255(const Image& a, const Image& b)
        {
            f64 sumSq = 0.0;
            std::size_t count = 0;
            for (std::size_t i = 0; i + 3 < a.m_Rgba.size(); i += 4)
            {
                for (u32 c = 0; c < 3; ++c)
                {
                    const f64 d = static_cast<f64>(a.m_Rgba[i + c]) - static_cast<f64>(b.m_Rgba[i + c]);
                    sumSq += d * d;
                    ++count;
                }
            }
            return count ? std::sqrt(sumSq / static_cast<f64>(count)) : 0.0;
        }

        [[nodiscard]] u32 MaxChannelDelta(const Image& a, const Image& b)
        {
            u32 worst = 0;
            for (std::size_t i = 0; i + 3 < a.m_Rgba.size(); i += 4)
            {
                for (u32 c = 0; c < 3; ++c)
                {
                    const i32 d = static_cast<i32>(a.m_Rgba[i + c]) - static_cast<i32>(b.m_Rgba[i + c]);
                    worst = std::max(worst, static_cast<u32>(d < 0 ? -d : d));
                }
            }
            return worst;
        }

        // True when both images loaded AND have identical dimensions. A size
        // mismatch is reported by the caller as its own failure rather than
        // silently skipped: it means one side was baked at a different
        // resolution, which no threshold should paper over.
        [[nodiscard]] bool LoadPairForCompare(const fs::path& lhs, const fs::path& rhs,
                                              Image& outLhs, Image& outRhs, bool& outSizeMismatch)
        {
            outSizeMismatch = false;
            if (!LoadForAudit(lhs, outLhs) || !LoadForAudit(rhs, outRhs))
                return false;
            if (outLhs.m_Width != outRhs.m_Width || outLhs.m_Height != outRhs.m_Height)
            {
                outSizeMismatch = true;
                return false;
            }
            return true;
        }

    } // namespace

    // =========================================================================
    // Reinhard HDR ramp — mirrors GoldenImageTest.ReinhardHdrRampGolden's guards.
    //
    // Reinhard is x/(1+x): asymptotic to 1, so a correct frame CANNOT contain a
    // saturated channel however bright the input. A passthrough, a clipping
    // operator substituted for Reinhard, or an exposure blowout all pin the top
    // of the 0..4 ramp at 255 — and a baseline baked in that state records it
    // as the reference forever.
    // =========================================================================
    TEST(GoldenBaselineAudit, ReinhardRampBaselinesAreToneMapped)
    {
        u32 audited = 0;
        for (const BaselineSet& set : DiscoverBaselineSets())
        {
            Image image;
            if (!LoadForAudit(set.m_GoldenDir / "tonemap_reinhard_hdr_ramp.png", image))
                continue;
            SCOPED_TRACE("baseline set: " + set.m_Label);
            ++audited;

            u32 peakChannel = 0;
            for (std::size_t i = 0; i + 3 < image.m_Rgba.size(); i += 4)
            {
                peakChannel = std::max({ peakChannel,
                                         static_cast<u32>(image.m_Rgba[i + 0]),
                                         static_cast<u32>(image.m_Rgba[i + 1]),
                                         static_cast<u32>(image.m_Rgba[i + 2]) });
            }
            // Theoretical peak: pow(4 / (1 + 4), 1 / 2.2) * 255 = 230.4.
            EXPECT_LT(peakChannel, 245u)
                << "peak channel is " << peakChannel
                << " — x/(1+x) is asymptotic to 1, so a (near-)saturated channel means this baseline "
                   "recorded a frame that did not go through Reinhard";
            EXPECT_GT(peakChannel, 200u)
                << "peak channel is only " << peakChannel
                << " — the ramp never reaches the shoulder, so the ceiling assertion above would pass "
                   "vacuously";

            // Input luminance rises with x and every ramp coefficient is
            // positive, so each row is non-decreasing left to right in every
            // channel. One LSB of tolerance covers quantisation only.
            constexpr u32 kMonotonicityTolerance = 1;
            u32 violations = 0;
            u32 worstDrop = 0;
            for (u32 y = 0; y < image.m_Height; ++y)
            {
                for (u32 c = 0; c < 3; ++c)
                {
                    u32 previous = image.At(0, y, c);
                    for (u32 x = 1; x < image.m_Width; ++x)
                    {
                        const u32 current = image.At(x, y, c);
                        if (current + kMonotonicityTolerance < previous)
                        {
                            ++violations;
                            worstDrop = std::max(worstDrop, previous - current);
                        }
                        previous = current;
                    }
                }
            }
            EXPECT_EQ(violations, 0u)
                << violations << " decreasing steps along the ramp (worst drop " << worstDrop
                << " LSB) — the recorded tone curve is not monotone in input luminance";
        }
        EXPECT_GT(audited, 0u) << "no tonemap baseline was audited — the audit cannot pass vacuously";
    }

    // =========================================================================
    // FXAA hard edge — mirrors GoldenImageTest.FxaaHardEdgeGolden's guards.
    //
    // The fixture's input is procedural, so it can be recomputed here exactly
    // and each recorded pixel classified as "was black" / "was white". That is
    // what makes the complementary-blend invariant checkable offline: every
    // altered pixel is a bilinear tap t of the way across a black/white edge,
    // so lifting a black pixel to t must be mirrored by dropping a white one to
    // 1 - t, and the sorted multisets must pair to 255.
    // =========================================================================
    TEST(GoldenBaselineAudit, FxaaBaselinesShowSymmetricEdgeBlending)
    {
        u32 audited = 0;
        for (const BaselineSet& set : DiscoverBaselineSets())
        {
            Image image;
            if (!LoadForAudit(set.m_GoldenDir / "fxaa_hard_edge.png", image))
                continue;
            SCOPED_TRACE("baseline set: " + set.m_Label);
            // EXPECT + continue, never ASSERT: an ASSERT here returns from the
            // whole test, so one malformed vendor set would stop every LATER
            // set from being audited — losing coverage exactly when something
            // is already wrong. Same reasoning at every in-loop check below.
            if (image.m_Width != image.m_Height)
            {
                ADD_FAILURE() << "the FXAA fixture is square, but this baseline is "
                              << image.m_Width << "x" << image.m_Height;
                continue;
            }
            ++audited;

            const u32 size = image.m_Width;
            constexpr u32 kLsbTolerance = 1; // ignore pure float->RGBA8 rounding
            std::vector<u32> darkBlends;
            std::vector<u32> brightBlends;
            for (u32 y = 0; y < size; ++y)
            {
                for (u32 x = 0; x < size; ++x)
                {
                    // Recomputed byte-for-byte from FxaaHardEdgeGolden.
                    const bool inputWasWhite = (x + (y % 8u)) >= (size / 2u + ((y / 8u) % 2u) * 4u);
                    const u32 outValue = image.At(x, y, 0);
                    if (inputWasWhite)
                    {
                        if (outValue + kLsbTolerance < 255u)
                            brightBlends.push_back(outValue);
                    }
                    else if (outValue > kLsbTolerance)
                    {
                        darkBlends.push_back(outValue);
                    }
                }
            }

            const u32 alteredCount = static_cast<u32>(darkBlends.size() + brightBlends.size());
            const f32 alteredFraction = static_cast<f32>(alteredCount) / static_cast<f32>(size * size);
            EXPECT_GT(alteredFraction, 0.005f)
                << "the recorded frame altered only " << alteredCount << " of " << (size * size)
                << " pixels — this baseline captured an FXAA that anti-aliases nothing";
            EXPECT_LT(alteredFraction, 0.10f)
                << "the recorded frame altered " << alteredCount << " of " << (size * size)
                << " pixels — that is a whole-frame smear, not edge anti-aliasing";

            if (darkBlends.size() != brightBlends.size())
            {
                // The complementary-pair check below indexes both multisets in
                // lockstep, so it is only meaningful when they are the same
                // size. Report and move to the next set rather than pairing
                // mismatched lists.
                ADD_FAILURE() << "recorded " << darkBlends.size() << " dark blends but "
                              << brightBlends.size()
                              << " bright ones — the captured filter is not symmetric across the edge";
                continue;
            }

            std::sort(darkBlends.begin(), darkBlends.end());
            std::sort(brightBlends.begin(), brightBlends.end());
            u32 worstPairDeviation = 0;
            std::size_t worstPairIndex = 0;
            for (std::size_t i = 0; i < darkBlends.size(); ++i)
            {
                const u32 sum = darkBlends[i] + brightBlends[brightBlends.size() - 1 - i];
                if (const u32 deviation = sum > 255u ? sum - 255u : 255u - sum; deviation > worstPairDeviation)
                {
                    worstPairDeviation = deviation;
                    worstPairIndex = i;
                }
            }
            if (!darkBlends.empty())
            {
                EXPECT_LE(worstPairDeviation, 2u)
                    << "blend pair " << worstPairIndex << " is not complementary: dark->"
                    << darkBlends[worstPairIndex] << ", bright->"
                    << brightBlends[brightBlends.size() - 1 - worstPairIndex]
                    << " (should sum to 255, off by " << worstPairDeviation << ")";
            }
        }
        EXPECT_GT(audited, 0u) << "no FXAA baseline was audited — the audit cannot pass vacuously";
    }

    // =========================================================================
    // Scene shadow integration — mirrors GoldenImageTest.SceneShadowIntegrationGolden.
    //
    // Orientation-free by construction: regions are found by colour population,
    // never by sampling a hard-coded screen position, so a vertically flipped
    // recording cannot satisfy these by accident.
    // =========================================================================
    TEST(GoldenBaselineAudit, ShadowBaselinesRetainPcfPenumbra)
    {
        u32 audited = 0;
        for (const BaselineSet& set : DiscoverBaselineSets())
        {
            Image image;
            if (!LoadForAudit(set.m_GoldenDir / "scene_shadow_integration.png", image))
                continue;
            SCOPED_TRACE("baseline set: " + set.m_Label);
            ++audited;

            const std::vector<ColorPopulation> colors = RankColorsByPopulation(image);
            if (colors.size() < 2u)
            {
                ADD_FAILURE() << "the recorded shadow frame has fewer than two distinct colours";
                continue;
            }
            EXPECT_GE(colors.size(), 4u)
                << "the recorded frame has only " << colors.size()
                << " distinct colours — a 3x3 PCF kernel produces intermediate shadow factors (k/9), so "
                   "this baseline captured a single-tap hard shadow";

            const auto unpackRgb = [](u32 rgb)
            {
                return std::array<u8, 3>{ static_cast<u8>(rgb >> 16),
                                          static_cast<u8>((rgb >> 8) & 0xFFu),
                                          static_cast<u8>(rgb & 0xFFu) };
            };
            const std::array<u8, 3> flatA = unpackRgb(colors[0].m_Rgb);
            const std::array<u8, 3> flatB = unpackRgb(colors[1].m_Rgb);
            const f32 lumaA = Rec601Luma(flatA[0], flatA[1], flatA[2]);
            const f32 lumaB = Rec601Luma(flatB[0], flatB[1], flatB[2]);
            const f32 litLuma = std::max(lumaA, lumaB);
            const f32 shadowedLuma = std::min(lumaA, lumaB);
            if (!(litLuma > 0.0f))
            {
                ADD_FAILURE() << "both flat regions are black — the recording has nothing lit";
                continue;
            }

            const f32 attenuation = shadowedLuma / litLuma;
            EXPECT_GT(attenuation, 0.50f)
                << "shadowed/lit luma ratio " << attenuation
                << " — the recorded shadow is darker than the 0.15 ambient floor allows";
            EXPECT_LT(attenuation, 0.75f)
                << "shadowed/lit luma ratio " << attenuation
                << " — the recorded shadow barely attenuates; the light term or shadow factor was not applied";

            const u32 pixelCount = image.m_Width * image.m_Height;
            const f32 flatFraction = static_cast<f32>(colors[0].m_Count + colors[1].m_Count) /
                                     static_cast<f32>(pixelCount);
            EXPECT_GT(flatFraction, 0.80f)
                << "the two most populous colours cover only " << (flatFraction * 100.0f)
                << "% of the recording — the flat lit/shadowed regions have broken up";
            EXPECT_LT(flatFraction, 0.98f)
                << "the two most populous colours cover " << (flatFraction * 100.0f)
                << "% of the recording — the PCF penumbra is gone (hard shadow edge)";
        }
        EXPECT_GT(audited, 0u) << "no shadow baseline was audited — the audit cannot pass vacuously";
    }

    // =========================================================================
    // Scene splatmap integration — mirrors the FINAL-IMAGE half of
    // GoldenImageTest.SceneSplatmapIntegrationGolden's guards.
    //
    // Its first guard compares the blended HDR against the CPU-authored
    // sum(w_i * layer_i) and needs the RGBA16F intermediate, which a recorded
    // 8-bit PNG does not carry — so it is deliberately NOT mirrored here. What
    // survives the tone map is the layer-identity signature at each edge
    // midpoint, and that is enough to catch the failure this exists for: a
    // channel swizzle or an array-layer off-by-one still looks like a smooth
    // four-way blend, and still compares fine against a baseline recorded while
    // the bug was present.
    // =========================================================================
    TEST(GoldenBaselineAudit, SplatmapBaselinesMapWeightsToTheirOwnLayers)
    {
        u32 audited = 0;
        for (const BaselineSet& set : DiscoverBaselineSets())
        {
            Image image;
            if (!LoadForAudit(set.m_GoldenDir / "scene_splatmap_integration.png", image))
                continue;
            SCOPED_TRACE("baseline set: " + set.m_Label);
            if (image.m_Width != image.m_Height)
            {
                ADD_FAILURE() << "the splatmap fixture is square, but this baseline is "
                              << image.m_Width << "x" << image.m_Height;
                continue;
            }
            ++audited;

            const u32 size = image.m_Width;
            constexpr u32 kEdgeInset = 3;
            constexpr u32 kEdgeRadius = 2;
            struct EdgeDominance
            {
                const char* m_Name;
                u32 m_X;
                u32 m_Y;
                u32 m_DominantChannel;
            };
            // Rock and sand share a dominant red channel; they are separated by
            // their green content in the ratio checks below.
            const EdgeDominance edges[4] = {
                { "layer 0 (red rock), u->0", kEdgeInset, size / 2, 0 },
                { "layer 1 (grass), v->0", size / 2, kEdgeInset, 1 },
                { "layer 2 (water blue), u->1", size - 1 - kEdgeInset, size / 2, 2 },
                { "layer 3 (warm sand), v->1", size / 2, size - 1 - kEdgeInset, 0 },
            };
            for (const EdgeDominance& edge : edges)
            {
                const std::array<f32, 3> mean = PatchMeanRgb(image, edge.m_X, edge.m_Y, kEdgeRadius);
                const u32 dominant = static_cast<u32>(std::distance(
                    mean.begin(), std::max_element(mean.begin(), mean.end())));
                EXPECT_EQ(dominant, edge.m_DominantChannel)
                    << edge.m_Name << " is not dominated by its own layer at (" << edge.m_X << ","
                    << edge.m_Y << "): rgb(" << mean[0] << ", " << mean[1] << ", " << mean[2] << ")";
            }

            // Rock (1.8, 0.25, 0.2) tone-maps to G/R near 0.59; sand
            // (1.7, 1.4, 0.25) to near 0.97. Anything between means the two red
            // layers were confused for each other when this was recorded.
            const std::array<f32, 3> rockMean = PatchMeanRgb(image, kEdgeInset, size / 2, kEdgeRadius);
            const std::array<f32, 3> sandMean =
                PatchMeanRgb(image, size / 2, size - 1 - kEdgeInset, kEdgeRadius);
            EXPECT_LT(rockMean[1] / rockMean[0], 0.75f)
                << "the u->0 edge has too much green for layer 0 (red rock): rgb(" << rockMean[0] << ", "
                << rockMean[1] << ", " << rockMean[2] << ")";
            EXPECT_GT(sandMean[1] / sandMean[0], 0.85f)
                << "the v->1 edge has too little green for layer 3 (warm sand): rgb(" << sandMean[0]
                << ", " << sandMean[1] << ", " << sandMean[2] << ")";
        }
        EXPECT_GT(audited, 0u) << "no splatmap baseline was audited — the audit cannot pass vacuously";
    }

    // =========================================================================
    // Atmosphere matrix — mirrors AtmosphereVisualEvidenceTest's four
    // cross-capture physical contracts.
    //
    // These are the strongest thing in this file, because they are RELATIONS
    // BETWEEN captures in the same set. A vendor whose sky path is broken
    // uniformly (a dead star field, a fog term that swallows the frame, a
    // night that is not dark) breaks a relation even though every individual
    // capture still compares green against its own recording.
    // =========================================================================
    TEST(GoldenBaselineAudit, AtmosphereBaselinesHoldTheirPhysicalContracts)
    {
        u32 audited = 0;
        for (const BaselineSet& set : DiscoverBaselineSets())
        {
            Image noonClear;
            Image nightClear;
            Image noonStorm;
            Image dawnClear;
            if (!LoadForAudit(set.m_VisualDir / "Atmosphere_NoonClear.png", noonClear) ||
                !LoadForAudit(set.m_VisualDir / "Atmosphere_NightClear.png", nightClear) ||
                !LoadForAudit(set.m_VisualDir / "Atmosphere_NoonStorm.png", noonStorm) ||
                !LoadForAudit(set.m_VisualDir / "Atmosphere_DawnClear.png", dawnClear))
            {
                continue;
            }
            SCOPED_TRACE("baseline set: " + set.m_Label);
            ++audited;

            // Bands: sky = top 18%, horizon = 38-46%, ground = bottom 25%.
            const BandStats noonClearSky = MeanBandPercent(noonClear, 0, 18);
            const BandStats nightClearSky = MeanBandPercent(nightClear, 0, 18);
            const BandStats noonClearGround = MeanBandPercent(noonClear, 75, 100);
            const BandStats noonStormGround = MeanBandPercent(noonStorm, 75, 100);
            const BandStats dawnClearHorizon = MeanBandPercent(dawnClear, 38, 46);
            const BandStats noonClearHorizon = MeanBandPercent(noonClear, 38, 46);

            EXPECT_GT(noonClearSky.Luma(), 60.0)
                << "recorded noon clear sky is not bright (luma " << noonClearSky.Luma() << ")";
            EXPECT_GE(noonClearSky.m_B, noonClearSky.m_R)
                << "recorded noon clear sky does not read blue (B " << noonClearSky.m_B << " < R "
                << noonClearSky.m_R << ")";
            EXPECT_LT(nightClearSky.Luma(), noonClearSky.Luma() * 0.4)
                << "recorded night sky is not much darker than day (night " << nightClearSky.Luma()
                << " vs 0.4*noon " << (noonClearSky.Luma() * 0.4) << ")";
            EXPECT_LT(noonStormGround.Luma(), noonClearGround.Luma())
                << "recorded storm does not darken the noon ground (storm " << noonStormGround.Luma()
                << " vs clear " << noonClearGround.Luma() << ")";

            const f64 dawnWarmth = dawnClearHorizon.m_R / std::max(dawnClearHorizon.m_B, 1.0);
            const f64 noonWarmth = noonClearHorizon.m_R / std::max(noonClearHorizon.m_B, 1.0);
            EXPECT_GT(dawnWarmth, noonWarmth)
                << "recorded dawn horizon is not warmer than noon (dawn " << dawnWarmth << " vs noon "
                << noonWarmth << ")";
        }
        EXPECT_GT(audited, 0u) << "no Atmosphere baseline set was audited — the audit cannot pass vacuously";
    }

    // =========================================================================
    // Cross-vendor parity — each vendor set against the shared set.
    //
    // The per-vendor audits above answer "did THIS vendor change?" with full
    // sensitivity, because each set is only ever compared against itself. What
    // that structurally cannot answer is "do the two vendors still agree?" —
    // and a conformance divergence lives exactly in that blind spot: the bake
    // records it, that vendor's nightly then compares green against its own
    // recording forever, and nothing contrasts the two sets. This test is that
    // contrast, and it needs no GPU and no second machine because BOTH sets are
    // committed.
    //
    // Two tiers, because the two metrics fail in opposite directions:
    //
    //   The four renderer goldens: TIGHT PIXEL parity. These frames
    //   are procedural (no scene load, no assets, nothing accumulating across
    //   frames) and drift-free, so a pixel gate here means what it says. The
    //   bar is GoldenImageTests' own kRmsePassBelow: the vendor golden must sit
    //   close enough to the shared one that it would PASS that test's compare.
    //   #735 measured max |delta| = 1 LSB, RMSE <= 0.0016.
    //
    //   The Atmosphere captures: DERIVED band parity plus a loose
    //   pixel backstop. A tight pixel gate would be actively wrong here, and
    //   this is the trap the whole #735 cross-check had to untangle: the two
    //   sets are NOT required to be baked at the same commit, and when they are
    //   not, pixel distance measures the calendar more than the silicon. The
    //   AMD and shared sets sat 2.60 apart in RMSE — of which only 0.72 was
    //   vendor, the rest a month of drift. Band luminances do not have that
    //   problem: across that same month they agreed to 0.43/255. So the physics
    //   gets the tight gate, and the pixels get a backstop sized to catch
    //   structural divergence (a dead star field, fog swallowing the frame, a
    //   UI that vanished) rather than bake-date skew.
    // =========================================================================
    TEST(GoldenBaselineAudit, VendorSetsAgreeWithTheSharedSet)
    {
        const std::vector<BaselineSet> sets = DiscoverBaselineSets();
        ASSERT_FALSE(sets.empty());
        const BaselineSet& shared = sets.front();
        ASSERT_EQ(shared.m_Label, "shared");

        // Not a failure: a repo with no per-vendor set is exactly the end state
        // if vendor scoping is ever dropped, and this test simply has nothing
        // to do then. It must not become the reason that change cannot land.
        if (sets.size() == 1)
        {
            GTEST_SKIP() << "no per-vendor baseline set on disk — nothing to cross-compare";
        }

        // The two RMSE bars are CHOSEN to equal the gates the golden tests
        // apply to themselves — kRmsePassBelow (GoldenImageTests.cpp, an
        // inclusive `m_Rmse <= …`, matched by EXPECT_LE below) and
        // kGoldenRmseThreshold (AtmosphereVisualEvidenceTest.cpp) — so this
        // check reads as "the vendor set would pass the shared set's own gate".
        //
        // They are COPIES, not references: both originals are function-local /
        // TU-local constexprs, and hoisting them into a shared header would
        // restructure two test files this change has no other reason to touch.
        // So the link is a convention, not a compile-time fact — if either gate
        // moves, move it here too. The other two bars are this test's own.
        constexpr f32 kGoldenRmseUnit = 0.004f;
        constexpr u32 kGoldenMaxDelta = 2;
        constexpr f64 kBandLumaDelta = 2.0;
        constexpr f64 kAtmosphereRmse255 = 8.0;

        struct Band
        {
            const char* m_Name;
            u32 m_Lo;
            u32 m_Hi;
        };
        static constexpr std::array<Band, 3> kBands{
            Band{ "sky", 0u, 18u },
            Band{ "horizon", 38u, 46u },
            Band{ "ground", 75u, 100u },
        };
        // Mirrors DriftWeatherVisualEvidenceTest::Capture's own bands — the
        // sea band deliberately starts below the horizon line so the headland
        // and sky cannot contribute to it.
        static constexpr std::array<Band, 3> kDriftBands{
            Band{ "sky", 0u, 20u },
            Band{ "horizon", 42u, 50u },
            Band{ "sea", 60u, 100u },
        };

        u32 comparisons = 0;
        for (std::size_t i = 1; i < sets.size(); ++i)
        {
            const BaselineSet& vendor = sets[i];
            SCOPED_TRACE("cross-vendor: " + vendor.m_Label + " vs shared");

            // ---- Tier 1: renderer goldens, tight pixel parity ----
            for (const char* name : kRendererGoldenFiles)
            {
                Image lhs;
                Image rhs;
                bool sizeMismatch = false;
                if (!LoadPairForCompare(vendor.m_GoldenDir / name, shared.m_GoldenDir / name,
                                        lhs, rhs, sizeMismatch))
                {
                    // Absent on either side is fine — a vendor set may cover
                    // only part of the matrix. Differing DIMENSIONS is not:
                    // no threshold covers a resolution change.
                    EXPECT_FALSE(sizeMismatch)
                        << name << " is " << lhs.m_Width << "x" << lhs.m_Height << " in '"
                        << vendor.m_Label << "' but " << rhs.m_Width << "x" << rhs.m_Height
                        << " in the shared set — one was baked at a different resolution";
                    continue;
                }
                ++comparisons;

                const f32 rmse = ComputeRgbRmseUnit(lhs, rhs);
                const u32 maxDelta = MaxChannelDelta(lhs, rhs);
                EXPECT_LE(rmse, kGoldenRmseUnit)
                    << name << ": '" << vendor.m_Label << "' differs from the shared baseline by RMSE "
                    << rmse << ", above GoldenImageTests' kRmsePassBelow — so this vendor's recording "
                               "would not pass the shared comparison. These frames are procedural and "
                               "drift-free, which makes this a conformance difference, not bake-date skew";
                EXPECT_LE(maxDelta, kGoldenMaxDelta)
                    << name << ": worst channel delta " << maxDelta << " between '" << vendor.m_Label
                    << "' and the shared baseline — last-bit rounding is <= " << kGoldenMaxDelta;
            }

            // ---- Tier 2: Atmosphere, derived band parity + pixel backstop ----
            for (const char* cell : kAtmosphereFiles)
            {
                Image lhs;
                Image rhs;
                bool sizeMismatch = false;
                if (!LoadPairForCompare(vendor.m_VisualDir / cell, shared.m_VisualDir / cell,
                                        lhs, rhs, sizeMismatch))
                {
                    EXPECT_FALSE(sizeMismatch)
                        << cell << " differs in resolution between '" << vendor.m_Label
                        << "' and the shared set";
                    continue;
                }
                ++comparisons;

                // The physics. Drift-insensitive, vendor-sensitive: a divergence
                // that changes what the frame depicts moves these by tens, while
                // a month of bake-date skew moved them by 0.43.
                for (const Band& band : kBands)
                {
                    const f64 vendorLuma = MeanBandPercent(lhs, band.m_Lo, band.m_Hi).Luma();
                    const f64 sharedLuma = MeanBandPercent(rhs, band.m_Lo, band.m_Hi).Luma();
                    const f64 delta = std::abs(vendorLuma - sharedLuma);
                    EXPECT_LT(delta, kBandLumaDelta)
                        << cell << ", " << band.m_Name << " band: '" << vendor.m_Label
                        << "' reads luma " << vendorLuma << " but the shared baseline reads "
                        << sharedLuma << " (delta " << delta
                        << ") — band means are insensitive to bake-date drift, so this is the two "
                           "vendors depicting different scenes";
                }

                // Backstop for a structural divergence that somehow left the
                // band means intact. Deliberately loose: sized against the
                // Atmosphere test's own threshold, NOT against the vendor
                // difference, precisely so bake-date skew cannot trip it.
                const f64 rmse255 = ComputeRgbRmse255(lhs, rhs);
                EXPECT_LE(rmse255, kAtmosphereRmse255)
                    << cell << ": '" << vendor.m_Label << "' vs the shared baseline is RMSE " << rmse255
                    << ", past the threshold that test applies to its own goldens. Before calling this "
                       "a vendor bug, check whether the two sets were baked at different commits — see "
                       "docs/agent-rules/vendor-golden-baseline-crosscheck.md";
            }

            // ---- Tier 3: Drift, same contract as Atmosphere, own bands ----
            for (const char* cell : kDriftFiles)
            {
                Image lhs;
                Image rhs;
                bool sizeMismatch = false;
                if (!LoadPairForCompare(vendor.m_VisualDir / cell, shared.m_VisualDir / cell,
                                        lhs, rhs, sizeMismatch))
                {
                    EXPECT_FALSE(sizeMismatch)
                        << cell << " differs in resolution between '" << vendor.m_Label
                        << "' and the shared set";
                    continue;
                }
                ++comparisons;

                for (const Band& band : kDriftBands)
                {
                    const f64 vendorLuma = MeanBandPercent(lhs, band.m_Lo, band.m_Hi).Luma();
                    const f64 sharedLuma = MeanBandPercent(rhs, band.m_Lo, band.m_Hi).Luma();
                    const f64 delta = std::abs(vendorLuma - sharedLuma);
                    EXPECT_LT(delta, kBandLumaDelta)
                        << cell << ", " << band.m_Name << " band: '" << vendor.m_Label
                        << "' reads luma " << vendorLuma << " but the shared baseline reads "
                        << sharedLuma << " (delta " << delta
                        << ") — band means are insensitive to bake-date drift, so this is the two "
                           "vendors depicting different scenes";
                }

                const f64 rmse255 = ComputeRgbRmse255(lhs, rhs);
                EXPECT_LE(rmse255, kAtmosphereRmse255)
                    << cell << ": '" << vendor.m_Label << "' vs the shared baseline is RMSE " << rmse255
                    << ", past the threshold DriftWeatherVisualEvidenceTest applies to its own "
                       "goldens. Before calling this a vendor bug, check whether the two sets were "
                       "baked at different commits — see "
                       "docs/agent-rules/vendor-golden-baseline-crosscheck.md";
            }
        }

        EXPECT_GT(comparisons, 0u)
            << "a vendor directory exists but nothing was cross-compared — the parity check cannot "
               "pass vacuously";
    }

    // =========================================================================
    // The audit's own coverage guard.
    //
    // Every test above skips a baseline it cannot load, which is what lets a
    // partial vendor set be audited for the part it has. The cost of that is
    // that a wrong working directory, a renamed asset folder or a deleted
    // vendor set would make all of them pass while auditing NOTHING. Each test
    // carries an `audited > 0` floor for that; this test states the stronger
    // property — the shared set is complete, and every vendor directory that
    // exists is actually reachable.
    // =========================================================================
    TEST(GoldenBaselineAudit, CoversTheCommittedBaselineSets)
    {
        const std::vector<BaselineSet> sets = DiscoverBaselineSets();
        ASSERT_FALSE(sets.empty());

        // The shared set is the one the whole suite compares against — and the
        // one every vendor set is compared AGAINST by the parity check — so it
        // must be complete in BOTH halves. A missing file here is the "wrong
        // working directory" failure the per-test floors can only hint at; and
        // requiring the Atmosphere captures closes a partial-loss gap, since
        // the per-cell audits skip whatever they cannot load.
        const BaselineSet& shared = sets.front();
        ASSERT_EQ(shared.m_Label, "shared");
        for (const char* name : kRendererGoldenFiles)
        {
            Image image;
            EXPECT_TRUE(LoadForAudit(shared.m_GoldenDir / name, image))
                << "shared baseline missing or unreadable: " << (shared.m_GoldenDir / name).string()
                << " — if this is the only failure, the test binary's working directory is wrong "
                   "(expected <repo>/OloEditor)";
        }
        for (const char* name : kAtmosphereFiles)
        {
            Image image;
            EXPECT_TRUE(LoadForAudit(shared.m_VisualDir / name, image))
                << "shared baseline missing or unreadable: " << (shared.m_VisualDir / name).string();
        }
        for (const char* name : kDriftFiles)
        {
            Image image;
            EXPECT_TRUE(LoadForAudit(shared.m_VisualDir / name, image))
                << "shared baseline missing or unreadable: " << (shared.m_VisualDir / name).string();
        }

        // A vendor directory that holds PNGs must hold at least one this audit
        // can actually read. A PNG that is present but unreadable (truncated,
        // wrong format, renamed) is the silent-skip shape: the nightly for that
        // vendor sees a baseline, this audit sees nothing, and neither says so.
        //
        // A directory holding NO PNG never reaches here — DiscoverBaselineSets
        // drops it, because the golden machinery creates empty vendor
        // directories as a side effect (see the note there). This loop is
        // therefore about CONTENT, not existence.
        for (std::size_t i = 1; i < sets.size(); ++i)
        {
            const BaselineSet& set = sets[i];
            SCOPED_TRACE("vendor set: " + set.m_Label);

            u32 pngsPresent = 0;
            u32 auditable = 0;
            for (const fs::path& dir : { set.m_GoldenDir, set.m_VisualDir })
            {
                std::error_code ec;
                if (!fs::is_directory(dir, ec))
                    continue;
                for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec))
                {
                    if (!entry.is_regular_file() || entry.path().extension() != ".png")
                        continue;
                    ++pngsPresent;
                    Image image;
                    if (LoadForAudit(entry.path(), image))
                        ++auditable;
                }
            }

            if (pngsPresent == 0)
                continue; // side-effect directory, not a baseline set

            EXPECT_GT(auditable, 0u)
                << "vendor baseline directory '" << set.m_Label << "' holds " << pngsPresent
                << " PNG(s) but none this audit can read — they would silently audit nothing";
        }
    }
} // namespace OloEngine::Tests

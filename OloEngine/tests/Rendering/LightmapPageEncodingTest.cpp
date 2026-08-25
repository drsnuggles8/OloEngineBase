// OLO_TEST_LAYER: shaderpipe
// =============================================================================
// LightmapPageEncodingTest.cpp
//
// The per-draw lightmap region encoding (issue #868). Multi-paging needed a
// fifth number in a vec4 whose four lanes were all spoken for, and the page
// ended up in the INTEGER PART of `.z`. That is a contract mirrored in GLSL
// (OloEditor/assets/shaders/include/LightmapSampling.glsl's
// decodeLightmapPage / decodeLightmapOffset), so it is exactly the kind of
// two-mirrors encoding that fails silently: a lost page index does not error,
// it samples ANOTHER ENTITY'S CHARTS through a valid-looking region.
//
// Three properties are pinned:
//
//   1. EXACTNESS. EXPECT_EQ on floats is correct here because bit-exact
//      round-tripping IS the contract — a page-local offset is a binary
//      fraction of at most 14 bits and the page needs at most 3, so their sum fits
//      f32's 24-bit significand with room to spare. If the encoding is ever
//      changed to something lossy, this test must fail rather than be widened.
//   2. THE ALL-ZERO "NO LIGHTMAP" SENTINEL SURVIVES. The issue calls this out
//      as the easiest thing to silently break. It survives because the encoding
//      only ever biases `.z`, while every gate in the engine and the shader
//      reads the `.x` SCALE lane.
//   3. THE BUDGET POLICY, including the one place it is NOT a total ceiling:
//      LightmapPageBudget never exceeds the format's page cap, never returns 0,
//      and shrinks as the atlas grows — but the one-page floor is
//      unconditional, so an atlas whose single page already exceeds the budget
//      still bakes. That exception is pinned explicitly rather than waved
//      through by a disjunction, because `budget == 1 || bytes <= ceiling`
//      holds for any implementation, including one that ignores the budget.
//
// Pure CPU maths — no GL, no ECS, no scene.
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/LightmapPageEncoding.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Serialization/LightmapBinaryFormat.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        [[nodiscard]] std::string ReadLightmapSamplingGlsl()
        {
            const fs::path path = fs::path{ OLO_TEST_EDITOR_ROOT } / "assets" / "shaders" / "include" /
                                  "LightmapSampling.glsl";
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                return {};
            }
            std::ostringstream buffer;
            buffer << file.rdbuf();
            return buffer.str();
        }

        // Whitespace-insensitive substring search — the shader is clang-format's
        // to reflow, so an exact-spacing match would be a false failure waiting
        // to happen.
        [[nodiscard]] std::string StripWhitespace(std::string_view text)
        {
            std::string out;
            out.reserve(text.size());
            for (const char c : text)
            {
                if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
                {
                    out.push_back(c);
                }
            }
            return out;
        }

        // Every page-local scale/offset the baker can actually produce for a
        // given atlas: a power-of-two region at a multiple-of-its-own-size
        // position, expressed as fractions of the atlas.
        [[nodiscard]] glm::vec4 PageLocalRegion(u32 atlasSize, u32 regionSize, u32 x, u32 y)
        {
            const f32 atlasSizeF = static_cast<f32>(atlasSize);
            return glm::vec4(static_cast<f32>(regionSize) / atlasSizeF,
                             static_cast<f32>(regionSize) / atlasSizeF,
                             static_cast<f32>(x) / atlasSizeF,
                             static_cast<f32>(y) / atlasSizeF);
        }
    } // namespace

    // ── 1. Exactness, across every legal atlas size / region / page ─────────
    TEST(LightmapPageEncodingTest, RoundTripsBitExactlyForEveryLegalPage)
    {
        for (const u32 atlasSize : { 16u, 64u, 256u, 1024u, 4096u, 16384u })
        {
            for (u32 regionSize = 8; regionSize <= atlasSize; regionSize *= 2)
            {
                // The four corners plus one interior placement — the region
                // grid is uniform, so the extremes are what can overflow the
                // fraction, not the middle.
                const u32 last = atlasSize - regionSize;
                for (const glm::uvec2 corner : { glm::uvec2(0, 0), glm::uvec2(last, 0),
                                                 glm::uvec2(0, last), glm::uvec2(last, last),
                                                 glm::uvec2(last / 2, last / 2) })
                {
                    const glm::vec4 pageLocal = PageLocalRegion(atlasSize, regionSize, corner.x, corner.y);
                    for (u32 page = 0; page < kMaxLightmapPages; ++page)
                    {
                        const glm::vec4 encoded = EncodeLightmapRegion(pageLocal, page);

                        EXPECT_EQ(DecodeLightmapPage(encoded), page)
                            << "atlas " << atlasSize << " region " << regionSize
                            << " at (" << corner.x << "," << corner.y << ") page " << page;

                        const glm::vec4 decoded = DecodeLightmapPageLocalRegion(encoded);
                        EXPECT_EQ(decoded.x, pageLocal.x);
                        EXPECT_EQ(decoded.y, pageLocal.y);
                        EXPECT_EQ(decoded.z, pageLocal.z)
                            << "offset lost precision at atlas " << atlasSize << " page " << page;
                        EXPECT_EQ(decoded.w, pageLocal.w);
                    }
                }
            }
        }
    }

    // The scale lanes — every gate in the engine and the shader reads `.x` —
    // must be untouched by the encoding, whatever the page.
    TEST(LightmapPageEncodingTest, ScaleLanesAreNeverTouched)
    {
        const glm::vec4 pageLocal = PageLocalRegion(1024, 128, 256, 512);
        for (u32 page = 0; page < kMaxLightmapPages; ++page)
        {
            const glm::vec4 encoded = EncodeLightmapRegion(pageLocal, page);
            EXPECT_EQ(encoded.x, pageLocal.x) << "page " << page;
            EXPECT_EQ(encoded.y, pageLocal.y) << "page " << page;
            EXPECT_EQ(encoded.w, pageLocal.w) << "page " << page;
            EXPECT_GT(encoded.x, 0.0f) << "a real region must pass the shader's scaleOffset.x > 0 gate";
        }
    }

    // ── 2. The all-zero "no lightmap" sentinel ─────────────────────────────
    TEST(LightmapPageEncodingTest, AllZeroSentinelSurvivesTheEncoding)
    {
        const glm::vec4 kSentinel{ 0.0f, 0.0f, 0.0f, 0.0f };

        // Component-wise on purpose: the repo bans operator== on glm::vec*
        // (cpp-coding-quality.md §2), and spelling the four comparisons out
        // makes it explicit that BIT-exactness is the contract being asserted,
        // not approximate equality.
        const auto expectExactlyZero = [](const glm::vec4& v)
        {
            EXPECT_EQ(v.x, 0.0f);
            EXPECT_EQ(v.y, 0.0f);
            EXPECT_EQ(v.z, 0.0f);
            EXPECT_EQ(v.w, 0.0f);
        };

        // A draw with no lightmap never goes through EncodeLightmapRegion at
        // all (SceneLightmapRuntime::GetScaleOffset returns the sentinel
        // directly), but even if it did, the sentinel must come back unchanged
        // on page 0 and must still read as "no lightmap" via the scale gate.
        const glm::vec4 encoded = EncodeLightmapRegion(kSentinel, 0);
        expectExactlyZero(encoded);
        EXPECT_EQ(DecodeLightmapPage(encoded), 0u);
        EXPECT_FALSE(encoded.x > 0.0f) << "the sentinel must fail the shader's scaleOffset.x > 0 gate";

        // Decoding the raw sentinel is well-defined too — page 0, zero offset.
        EXPECT_EQ(DecodeLightmapPage(kSentinel), 0u);
        expectExactlyZero(DecodeLightmapPageLocalRegion(kSentinel));

        // And a real region is never mistakable for it, on any page: the scale
        // lane is what separates them, and it is strictly positive.
        for (u32 page = 0; page < kMaxLightmapPages; ++page)
        {
            const glm::vec4 real = EncodeLightmapRegion(PageLocalRegion(1024, 8, 0, 0), page);
            EXPECT_TRUE(real.x > 0.0f) << "a real region is never the sentinel, on any page (page " << page << ")";
        }
    }

    // A degenerate `.z` (negative or NaN — neither is producible by the baker,
    // but a corrupt asset could carry one past Validate's finiteness check for
    // the negative case) must decode to page 0 rather than a wild layer index.
    TEST(LightmapPageEncodingTest, DegenerateOffsetDecodesToPageZero)
    {
        EXPECT_EQ(DecodeLightmapPage(glm::vec4(0.5f, 0.5f, -1.0f, 0.0f)), 0u);
        EXPECT_EQ(DecodeLightmapPage(glm::vec4(0.5f, 0.5f, -0.25f, 0.0f)), 0u);
        EXPECT_EQ(DecodeLightmapPage(glm::vec4(0.5f, 0.5f, std::numeric_limits<f32>::quiet_NaN(), 0.0f)), 0u);

        // The values where a naive `static_cast<u32>(floor(z))` is UNDEFINED
        // BEHAVIOUR rather than a large number: infinities, and finite values
        // past what u32 can hold. `Validate()` keeps these out of any real
        // asset, but this helper is public and inline, so it has to be total.
        EXPECT_EQ(DecodeLightmapPage(glm::vec4(0.5f, 0.5f, std::numeric_limits<f32>::infinity(), 0.0f)), 0u);
        EXPECT_EQ(DecodeLightmapPage(glm::vec4(0.5f, 0.5f, -std::numeric_limits<f32>::infinity(), 0.0f)), 0u);
        EXPECT_EQ(DecodeLightmapPage(glm::vec4(0.5f, 0.5f, std::numeric_limits<f32>::max(), 0.0f)), 0u);
        EXPECT_EQ(DecodeLightmapPage(glm::vec4(0.5f, 0.5f, 5.0e9f, 0.0f)), 0u); // past UINT32_MAX
        EXPECT_EQ(DecodeLightmapPage(glm::vec4(0.5f, 0.5f, 4.0e9f, 0.0f)), 0u); // inside u32, past the cap

        // And the boundary: the last legal page decodes, the first illegal one
        // does not.
        EXPECT_EQ(DecodeLightmapPage(glm::vec4(0.5f, 0.5f, static_cast<f32>(kMaxLightmapPages - 1) + 0.5f, 0.0f)),
                  kMaxLightmapPages - 1);
        EXPECT_EQ(DecodeLightmapPage(glm::vec4(0.5f, 0.5f, static_cast<f32>(kMaxLightmapPages), 0.0f)), 0u);
    }

    // ── 3. The budget policy ───────────────────────────────────────────────
    TEST(LightmapPageEncodingTest, PageBudgetIsABoundedDecreasingCeiling)
    {
        // Never zero: a single page always packs (by degrading), which beats
        // refusing to bake.
        EXPECT_EQ(LightmapPageBudget(0), 1u);
        EXPECT_GE(LightmapPageBudget(16), 1u);

        u32 previous = LightmapPageBudget(16);
        for (u32 atlasSize = 32; atlasSize <= 16384; atlasSize *= 2)
        {
            const u32 budget = LightmapPageBudget(atlasSize);
            EXPECT_GE(budget, 1u) << "atlas " << atlasSize;
            EXPECT_LE(budget, kMaxLightmapPages) << "atlas " << atlasSize;
            EXPECT_LE(budget, previous) << "a bigger atlas must never afford MORE pages (atlas " << atlasSize << ")";

            // The ceiling bounds PAGING, and the one-page floor is the single
            // documented exception. Asserting `budget == 1 || bytes <= ceiling`
            // would be circular — it holds for ANY implementation, including
            // one that ignores the budget entirely — so split the two cases and
            // give each a real bound:
            //   - a multi-page budget must fit the ceiling exactly;
            //   - a single-page budget is only allowed when one page ALREADY
            //     exceeds the ceiling, i.e. no smaller answer existed.
            const u64 pageBytes = static_cast<u64>(atlasSize) * atlasSize * kLightmapAtlasBytesPerTexel;
            const u64 bytes = static_cast<u64>(budget) * pageBytes;
            if (budget > 1u)
            {
                EXPECT_LE(bytes, kLightmapAtlasMemoryBudgetBytes)
                    << "atlas " << atlasSize << " budget " << budget << " => " << bytes << " bytes";
            }
            else
            {
                EXPECT_TRUE(SinglePageExceedsLightmapBudget(atlasSize))
                    << "atlas " << atlasSize << " was cut to one page although two would have fitted the budget";
            }
            previous = budget;
        }

        // The default 1024 atlas gets the format's full page count.
        EXPECT_EQ(LightmapPageBudget(1024), kMaxLightmapPages);
    }

    // The one documented exception, pinned explicitly so it stays a decision
    // rather than drifting into an accident: an atlas whose SINGLE page already
    // exceeds the ceiling still gets one page, and the predicate that says so
    // agrees with the budget function.
    TEST(LightmapPageEncodingTest, SinglePageOverrunIsAllowedAndReported)
    {
        // 2048² x 8 B = 32 MiB — two pages fit the 64 MiB ceiling.
        EXPECT_FALSE(SinglePageExceedsLightmapBudget(2048));
        EXPECT_GE(LightmapPageBudget(2048), 2u);

        // 4096² x 8 B = 128 MiB, and 16384² x 8 B = 2 GiB: one page each, both
        // over the ceiling, both permitted — that is the pre-#868 footprint.
        for (const u32 atlasSize : { 4096u, 8192u, 16384u })
        {
            EXPECT_TRUE(SinglePageExceedsLightmapBudget(atlasSize)) << "atlas " << atlasSize;
            EXPECT_EQ(LightmapPageBudget(atlasSize), 1u) << "atlas " << atlasSize;
        }

        // The two must never disagree: a single-page budget and "one page fits"
        // cannot both be true, or the ceiling would be silently under-spent.
        for (u32 atlasSize = 16; atlasSize <= 16384; atlasSize *= 2)
        {
            if (!SinglePageExceedsLightmapBudget(atlasSize))
            {
                EXPECT_GE(LightmapPageBudget(atlasSize), 1u) << "atlas " << atlasSize;
            }
        }
    }

    // The encoding's exactness argument assumes at most kMaxLightmapPages, and
    // the .olmap reader rejects anything above its own cap — the two must agree
    // or a bake can produce a file that cannot be loaded back.
    TEST(LightmapPageEncodingTest, PageCapMatchesTheOnDiskFormat)
    {
        EXPECT_LE(kMaxLightmapPages, OLmapFormat::MaxPageCount);
    }

    // ── 4. The GLSL half of the two-mirrors pair ───────────────────────────
    //
    // Everything above tests the C++ helpers, which NOTHING in the engine calls
    // on the sampling path — the shader decodes the region itself. So deleting
    // `- floor(scaleOffset.z)` from `decodeLightmapOffset` would leave every
    // assertion above green, and the only test that actually renders a paged
    // atlas (LightmapVisualEvidenceTest.BakedBleedSurvivesAMultiPageAtlas) is
    // behind OLO_ENSURE_GPU_OR_SKIP() — so headless CI would never see it.
    //
    // This scans the shader text instead, the same instrument
    // GBufferBakedGIContractTest uses for the same reason. It cannot prove the
    // decode is CORRECT (the GPU test does that); what it proves is that the
    // shader still carries a decode at all, and that the sampler is an array —
    // the two ways this silently reverts to single-page addressing.
    TEST(LightmapPageEncodingTest, ShaderCarriesTheMatchingPageDecode)
    {
        const std::string source = ReadLightmapSamplingGlsl();
        ASSERT_FALSE(source.empty()) << "could not read include/LightmapSampling.glsl";
        const std::string packed = StripWhitespace(source);

        // The atlas must be an ARRAY at TEX_LIGHTMAP — a sampler2D here is
        // single-page addressing with the page silently dropped.
        const std::string expectedBinding =
            "layout(binding=" + std::to_string(ShaderBindingLayout::TEX_LIGHTMAP) + ")uniformsampler2DArrayu_LightmapAtlas";
        EXPECT_NE(packed.find(expectedBinding), std::string::npos)
            << "u_LightmapAtlas must be a sampler2DArray at binding "
            << ShaderBindingLayout::TEX_LIGHTMAP << " (issue #868)";

        // The decode must be the exact inverse of EncodeLightmapRegion: page =
        // floor(.z), offset.x = .z - floor(.z).
        EXPECT_NE(packed.find("floatdecodeLightmapPage(vec4scaleOffset){returnfloor(scaleOffset.z);}"),
                  std::string::npos)
            << "decodeLightmapPage must be floor(scaleOffset.z) — the inverse of EncodeLightmapRegion";
        EXPECT_NE(packed.find("vec2decodeLightmapOffset(vec4scaleOffset){returnvec2(scaleOffset.z-floor(scaleOffset.z),scaleOffset.w);}"),
                  std::string::npos)
            << "decodeLightmapOffset must subtract floor(scaleOffset.z) from the x lane — without it the "
               "page bias leaks into the UV and every paged draw addresses the wrong texels";

        // And the sample must actually USE both halves: the array lookup takes
        // the decoded page as its layer, and the UV uses the decoded offset.
        EXPECT_NE(packed.find("texture(u_LightmapAtlas,vec3(atlasUV,decodeLightmapPage(scaleOffset)))"),
                  std::string::npos)
            << "the atlas fetch must index the decoded page as the array layer";
        EXPECT_NE(packed.find("lightmapUV*scaleOffset.xy+decodeLightmapOffset(scaleOffset)"), std::string::npos)
            << "the atlas UV must use the DECODED offset, not the raw page-biased .zw";

        // The scale-lane gate is what keeps the all-zero sentinel working; it
        // must survive any future edit to this helper.
        EXPECT_NE(packed.find("scaleOffset.x<=0.0"), std::string::npos)
            << "the 'no lightmap' gate must stay on the SCALE lane — the encoding biases .z";
    }
} // namespace OloEngine::Tests

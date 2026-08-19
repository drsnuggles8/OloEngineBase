#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L1
// =============================================================================
// DDGIMathTest — L1 contract tests for the pure CPU DDGI probe math
// (OloEngine/Renderer/DDGI/DDGICommon.h, issue #632).
//
// Every function in that header is a documented one-for-one mirror of the GLSL
// in OloEditor/assets/shaders/include/DDGICommon.glsl; these tests pin the C++
// side headlessly (no GL), and the shaderpipe parity tests pin GLSL == C++.
//
// Expected values are either derived inside the test or carry a citation:
//   - Octahedral mapping + border/wrap rules: Cigolle et al., JCGT 2014,
//     "A Survey of Efficient Representations for Independent Unit Vectors";
//     NVIDIA RTXGI-DDGI SDK border-update kernels.
//   - Probe relocation / classification / hysteresis: Majercik et al., JCGT
//     2021, "Scaling Probe-Based Real-Time Dynamic Global Illumination for
//     Production" (RTXGI conventions quoted in the header comments).
//   - Spherical Fibonacci direction set: Keinert et al. 2015, "Spherical
//     Fibonacci Mapping" (golden-angle spiral).
// =============================================================================

#include "OloEngine/Renderer/DDGI/DDGICommon.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <random>
#include <set>
#include <utility>
#include <vector>

using namespace OloEngine;

namespace
{
    // Golden-angle spiral over the sphere (Keinert et al. 2015): a cheap,
    // deterministic, well-distributed direction set. z_i = 1 - (2i+1)/n spans
    // (-1, 1); the azimuth advances by the golden angle pi*(3 - sqrt(5)).
    glm::vec3 SphericalFibonacci(i32 i, i32 n)
    {
        f32 const goldenAngle = glm::pi<f32>() * (3.0f - std::sqrt(5.0f));
        f32 const z = 1.0f - (2.0f * static_cast<f32>(i) + 1.0f) / static_cast<f32>(n);
        f32 const r = std::sqrt(glm::max(0.0f, 1.0f - z * z));
        f32 const phi = goldenAngle * static_cast<f32>(i);
        return glm::normalize(glm::vec3(r * std::cos(phi), r * std::sin(phi), z));
    }

    // Uniform random unit vector via normalized Gaussian triple (the standard
    // rotation-invariant construction).
    glm::vec3 RandomUnitVector(std::mt19937& rng)
    {
        std::normal_distribution<f32> gauss(0.0f, 1.0f);
        glm::vec3 v(0.0f);
        do
        {
            v = { gauss(rng), gauss(rng), gauss(rng) };
        } while (glm::dot(v, v) < 1e-6f);
        return glm::normalize(v);
    }

    // Octahedral tiling rule: a point one step beyond a square edge maps to the
    // in-range point given by a 180-degree rotation about that edge's midpoint
    // (Cigolle et al. 2014; the rule RTXGI's border-update kernels implement).
    // It follows from the map's boundary self-glue, which BorderSeamContinuity
    // asserts directly from OctDecode before relying on this helper:
    //   OctDecode(+-1, v) == OctDecode(+-1, -v) and
    //   OctDecode(u, +-1) == OctDecode(-u, +-1),
    // plus continuity of the direction field across the boundary.
    glm::vec2 OctWrap(glm::vec2 uv)
    {
        // A one-texel border ring needs at most two folds (edge, then corner).
        for (i32 fold = 0; fold < 4; ++fold)
        {
            if (uv.x > 1.0f)
            {
                uv = { 2.0f - uv.x, -uv.y };
            }
            else if (uv.x < -1.0f)
            {
                uv = { -2.0f - uv.x, -uv.y };
            }
            else if (uv.y > 1.0f)
            {
                uv = { -uv.x, 2.0f - uv.y };
            }
            else if (uv.y < -1.0f)
            {
                uv = { -uv.x, -2.0f - uv.y };
            }
            else
            {
                break;
            }
        }
        return uv;
    }

    // Center of interior texel t of an N x N grid in octahedral [-1,1]^2 —
    // the same uv TexelDirection() feeds to OctDecode (derived from its body).
    glm::vec2 InteriorTexelCenterUV(const glm::ivec2& texel, i32 interiorResolution)
    {
        glm::vec2 const uv01 = (glm::vec2(texel) + 0.5f) / static_cast<f32>(interiorResolution);
        return uv01 * 2.0f - 1.0f;
    }
} // namespace

// --- 1. Octahedral encode/decode round trip --------------------------------

TEST(DDGIMath, OctRoundTrip)
{
    std::vector<glm::vec3> dirs;
    for (i32 i = 0; i < 256; ++i)
    {
        dirs.push_back(SphericalFibonacci(i, 256));
    }
    // The 6 axes (octahedron vertices — encode maps them to the square's
    // center, edge midpoints, and corners).
    dirs.emplace_back(1.0f, 0.0f, 0.0f);
    dirs.emplace_back(-1.0f, 0.0f, 0.0f);
    dirs.emplace_back(0.0f, 1.0f, 0.0f);
    dirs.emplace_back(0.0f, -1.0f, 0.0f);
    dirs.emplace_back(0.0f, 0.0f, 1.0f);
    dirs.emplace_back(0.0f, 0.0f, -1.0f);
    // Equator fold-seam diagonals |u|+|v| = 1 (z == 0, where the lower-
    // hemisphere fold meets the upper hemisphere).
    dirs.push_back(glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f)));
    dirs.push_back(glm::normalize(glm::vec3(1.0f, -1.0f, 0.0f)));
    dirs.push_back(glm::normalize(glm::vec3(-1.0f, 1.0f, 0.0f)));
    dirs.push_back(glm::normalize(glm::vec3(-1.0f, -1.0f, 0.0f)));
    // Lower-hemisphere fold seams (x == 0 or y == 0, z < 0 — the square's
    // diagonals after the fold).
    dirs.push_back(glm::normalize(glm::vec3(1.0f, 0.0f, -1.0f)));
    dirs.push_back(glm::normalize(glm::vec3(-1.0f, 0.0f, -1.0f)));
    dirs.push_back(glm::normalize(glm::vec3(0.0f, 1.0f, -1.0f)));
    dirs.push_back(glm::normalize(glm::vec3(0.0f, -1.0f, -1.0f)));

    for (const glm::vec3& d : dirs)
    {
        glm::vec2 const e = DDGI::OctEncode(d);
        // Encode must stay inside the octahedral square. Exact bound: pre-fold
        // components are |x|/l1 <= 1, and the fold produces (1 - |uv|) with
        // |uv| in [0, 1].
        EXPECT_GE(e.x, -1.0f - 1e-6f);
        EXPECT_LE(e.x, 1.0f + 1e-6f);
        EXPECT_GE(e.y, -1.0f - 1e-6f);
        EXPECT_LE(e.y, 1.0f + 1e-6f);

        glm::vec3 const back = DDGI::OctDecode(e);
        EXPECT_NEAR(back.x, d.x, 1e-5f) << "dir (" << d.x << ", " << d.y << ", " << d.z << ")";
        EXPECT_NEAR(back.y, d.y, 1e-5f) << "dir (" << d.x << ", " << d.y << ", " << d.z << ")";
        EXPECT_NEAR(back.z, d.z, 1e-5f) << "dir (" << d.x << ", " << d.y << ", " << d.z << ")";
    }
}

// --- 2. Decode always yields unit vectors ----------------------------------

TEST(DDGIMath, OctDecodeIsUnit)
{
    // A 41 x 41 grid over [-1,1]^2 including the boundary and both diagonals.
    for (i32 iy = 0; iy <= 40; ++iy)
    {
        for (i32 ix = 0; ix <= 40; ++ix)
        {
            glm::vec2 const f(static_cast<f32>(ix) / 20.0f - 1.0f,
                              static_cast<f32>(iy) / 20.0f - 1.0f);
            glm::vec3 const d = DDGI::OctDecode(f);
            EXPECT_NEAR(glm::length(d), 1.0f, 1e-5f)
                << "oct uv (" << f.x << ", " << f.y << ")";
        }
    }
}

// --- 3. TexelDirection is inverted by OctEncode ----------------------------

TEST(DDGIMath, TexelDirectionCentersInvert)
{
    // Encoding a texel-center direction and rescaling to the texel grid must
    // land back inside that texel (the map is a bijection away from seams, and
    // texel centers never sit outside the square, so the inversion is exact up
    // to float rounding — measured error ~1e-6 texels; half a texel is the
    // "same texel" criterion).
    for (i32 n : { 8, 16 })
    {
        for (i32 ty = 0; ty < n; ++ty)
        {
            for (i32 tx = 0; tx < n; ++tx)
            {
                glm::vec3 const dir = DDGI::TexelDirection({ tx, ty }, n);
                glm::vec2 const uv01 = DDGI::OctEncode(dir) * 0.5f + 0.5f;
                glm::vec2 const texelPos = uv01 * static_cast<f32>(n);
                EXPECT_NEAR(texelPos.x, static_cast<f32>(tx) + 0.5f, 0.5f)
                    << "N=" << n << " texel (" << tx << ", " << ty << ")";
                EXPECT_NEAR(texelPos.y, static_cast<f32>(ty) + 0.5f, 0.5f)
                    << "N=" << n << " texel (" << tx << ", " << ty << ")";
            }
        }
    }
}

// --- 4. Atlas texel coordinate stays inside the probe's own tile -----------

TEST(DDGIMath, ProbeAtlasTexelLandsInOwnTile)
{
    std::mt19937 rng(20260716u);

    for (const glm::ivec3& dims : { glm::ivec3(4, 2, 4), glm::ivec3(16, 8, 16) })
    {
        i32 const probeCount = dims.x * dims.y * dims.z;
        std::uniform_int_distribution<i32> probeDist(0, probeCount - 1);

        for (i32 interior : { DDGI::kIrradianceInteriorTexels, DDGI::kVisibilityInteriorTexels })
        {
            i32 const tileTexels = interior + 2;
            for (i32 sample = 0; sample < 128; ++sample)
            {
                i32 const probe = probeDist(rng);
                glm::vec3 const dir = RandomUnitVector(rng);

                glm::vec2 const texel = DDGI::ProbeAtlasTexel(probe, dims, dir, interior);
                glm::vec2 const tileOrigin = glm::vec2(DDGI::ProbeTileCoord(probe, dims) * tileTexels);

                // Contract from the header: border-safe range
                // [tileOrigin + 1, tileOrigin + 1 + interior] (uv01 in [0,1]
                // scaled by the interior size, offset past the 1-texel border).
                EXPECT_GE(texel.x, tileOrigin.x + 1.0f - 1e-4f) << "probe " << probe;
                EXPECT_LE(texel.x, tileOrigin.x + 1.0f + static_cast<f32>(interior) + 1e-4f) << "probe " << probe;
                EXPECT_GE(texel.y, tileOrigin.y + 1.0f - 1e-4f) << "probe " << probe;
                EXPECT_LE(texel.y, tileOrigin.y + 1.0f + static_cast<f32>(interior) + 1e-4f) << "probe " << probe;
            }
        }
    }
}

// --- 5. Border gutter source lookup -----------------------------------------

TEST(DDGIMath, BorderSourceTexel)
{
    // (a) + (b): structural properties over both production tile sizes
    // (irradiance 8x8, visibility 16x16).
    for (i32 tileTexels : { DDGI::kIrradianceTileTexels, DDGI::kVisibilityTileTexels })
    {
        i32 const maxT = tileTexels - 1;
        for (i32 y = 0; y < tileTexels; ++y)
        {
            for (i32 x = 0; x < tileTexels; ++x)
            {
                glm::ivec2 const src = DDGI::BorderSourceTexel({ x, y }, tileTexels);
                bool const isBorder = (x == 0) || (x == maxT) || (y == 0) || (y == maxT);
                if (isBorder)
                {
                    // (b) every border texel copies from a strict-interior texel.
                    EXPECT_GE(src.x, 1) << "T=" << tileTexels << " (" << x << ", " << y << ")";
                    EXPECT_LE(src.x, maxT - 1) << "T=" << tileTexels << " (" << x << ", " << y << ")";
                    EXPECT_GE(src.y, 1) << "T=" << tileTexels << " (" << x << ", " << y << ")";
                    EXPECT_LE(src.y, maxT - 1) << "T=" << tileTexels << " (" << x << ", " << y << ")";
                }
                else
                {
                    // (a) interior texels map to themselves.
                    EXPECT_EQ(src.x, x) << "T=" << tileTexels;
                    EXPECT_EQ(src.y, y) << "T=" << tileTexels;
                }
            }
        }
    }

    // (c) hand-derived values from the RTXGI convention documented in the
    // header: edge (x, 0) -> (T-1-x, 1); corner (0, 0) -> (T-2, T-2).
    // T = 8 (maxT = 7):
    //   corners — diagonally opposite interior corner:
    //     (0,0) -> (6,6); (7,0) -> (1,6); (0,7) -> (6,1); (7,7) -> (1,1)
    //   bottom edge (x,0) -> (7-x, 1):  (1,0) -> (6,1); (3,0) -> (4,1); (6,0) -> (1,1)
    //   top edge (x,7) -> (7-x, 6):     (2,7) -> (5,6)
    //   left edge (0,y) -> (1, 7-y):    (0,2) -> (1,5)
    //   right edge (7,y) -> (6, 7-y):   (7,4) -> (6,3)
    struct Expected
    {
        glm::ivec2 Local;
        glm::ivec2 Source;
    };
    const Expected kExpected8[] = {
        { { 0, 0 }, { 6, 6 } },
        { { 7, 0 }, { 1, 6 } },
        { { 0, 7 }, { 6, 1 } },
        { { 7, 7 }, { 1, 1 } },
        { { 1, 0 }, { 6, 1 } },
        { { 3, 0 }, { 4, 1 } },
        { { 6, 0 }, { 1, 1 } },
        { { 2, 7 }, { 5, 6 } },
        { { 0, 2 }, { 1, 5 } },
        { { 7, 4 }, { 6, 3 } },
    };
    for (const Expected& e : kExpected8)
    {
        glm::ivec2 const src = DDGI::BorderSourceTexel(e.Local, 8);
        EXPECT_EQ(src.x, e.Source.x) << "T=8 local (" << e.Local.x << ", " << e.Local.y << ")";
        EXPECT_EQ(src.y, e.Source.y) << "T=8 local (" << e.Local.x << ", " << e.Local.y << ")";
    }

    // T = 16 (maxT = 15): corners (0,0) -> (14,14), (15,15) -> (1,1);
    // bottom edge (1,0) -> (15-1, 1) = (14,1); right edge (15,7) -> (14, 15-7) = (14,8).
    const Expected kExpected16[] = {
        { { 0, 0 }, { 14, 14 } },
        { { 15, 15 }, { 1, 1 } },
        { { 1, 0 }, { 14, 1 } },
        { { 15, 7 }, { 14, 8 } },
    };
    for (const Expected& e : kExpected16)
    {
        glm::ivec2 const src = DDGI::BorderSourceTexel(e.Local, 16);
        EXPECT_EQ(src.x, e.Source.x) << "T=16 local (" << e.Local.x << ", " << e.Local.y << ")";
        EXPECT_EQ(src.y, e.Source.y) << "T=16 local (" << e.Local.x << ", " << e.Local.y << ")";
    }
}

// --- 6. Border seam continuity (the property that makes the gutter correct) -

TEST(DDGIMath, BorderSeamContinuity)
{
    // Step 1 — pin the octahedral boundary self-glue directly from OctDecode
    // (the non-circular anchor for the wrap rule): each square edge maps to
    // the sphere mirrored about its own midpoint, so
    //   OctDecode(+-1, v) == OctDecode(+-1, -v),
    //   OctDecode(u, +-1) == OctDecode(-u, +-1).
    // Derivation for the u = -1 edge, v > 0: n = (-1, v, -v), t = v, giving
    // n = (v-1, 0, -v) — identical for (-1, -v). (Cigolle et al. 2014.)
    for (i32 i = 0; i <= 20; ++i)
    {
        f32 const v = static_cast<f32>(i) / 10.0f - 1.0f;
        struct GluePair
        {
            glm::vec2 A;
            glm::vec2 B;
        };
        const GluePair pairs[] = {
            { { 1.0f, v }, { 1.0f, -v } },
            { { -1.0f, v }, { -1.0f, -v } },
            { { v, 1.0f }, { -v, 1.0f } },
            { { v, -1.0f }, { -v, -1.0f } },
        };
        for (const GluePair& p : pairs)
        {
            glm::vec3 const da = DDGI::OctDecode(p.A);
            glm::vec3 const db = DDGI::OctDecode(p.B);
            EXPECT_NEAR(da.x, db.x, 1e-6f) << "glue at v=" << v;
            EXPECT_NEAR(da.y, db.y, 1e-6f) << "glue at v=" << v;
            EXPECT_NEAR(da.z, db.z, 1e-6f) << "glue at v=" << v;
        }
    }

    // Step 2 — for EVERY border texel B of both production tile sizes, the
    // source S chosen by BorderSourceTexel must hold the direction of B's own
    // texel center continued across the seam, i.e. the direction at the
    // octahedral-wrap image of B's (out-of-range) virtual texel center.
    //
    // Hand-worked example, 8x8 tile (T=8, N=6), left-edge border texel
    // B = local (0, 1):
    //   S = BorderSourceTexel(B, 8) = (1, T-1-y) = (1, 6) -> interior (0, 5).
    //   B's virtual interior texel is (-1, 0); its center in oct coords is
    //     u = 2*(-0.5/6) - 1 = -7/6,  v = 2*(0.5/6) - 1 = -5/6.
    //   Crossing u = -1, the tiling rule (u, v) -> (-2-u, -v) gives
    //     (-5/6, 5/6),
    //   which is exactly the center of interior texel (0, 5):
    //     u = 2*(0.5/6) - 1 = -5/6,  v = 2*(5.5/6) - 1 = 5/6.   QED.
    for (i32 tileTexels : { DDGI::kIrradianceTileTexels, DDGI::kVisibilityTileTexels })
    {
        i32 const interior = tileTexels - 2;
        i32 const maxT = tileTexels - 1;
        for (i32 y = 0; y < tileTexels; ++y)
        {
            for (i32 x = 0; x < tileTexels; ++x)
            {
                bool const isBorder = (x == 0) || (x == maxT) || (y == 0) || (y == maxT);
                if (!isBorder)
                {
                    continue;
                }

                glm::ivec2 const src = DDGI::BorderSourceTexel({ x, y }, tileTexels);
                glm::vec3 const actual = DDGI::TexelDirection(src - glm::ivec2(1), interior);

                // Virtual interior coords of the border texel (may be -1 or N).
                glm::ivec2 const virtualTexel(x - 1, y - 1);
                glm::vec2 const wrapped = OctWrap(InteriorTexelCenterUV(virtualTexel, interior));
                glm::vec3 const expected = DDGI::OctDecode(wrapped);

                EXPECT_NEAR(actual.x, expected.x, 1e-5f)
                    << "T=" << tileTexels << " border (" << x << ", " << y << ")";
                EXPECT_NEAR(actual.y, expected.y, 1e-5f)
                    << "T=" << tileTexels << " border (" << x << ", " << y << ")";
                EXPECT_NEAR(actual.z, expected.z, 1e-5f)
                    << "T=" << tileTexels << " border (" << x << ", " << y << ")";
            }
        }
    }
}

// --- 7. Probe linear index round trip ---------------------------------------

TEST(DDGIMath, LinearIndexRoundTrip)
{
    // The linear-index convention is z-major: z*dimY*dimX + y*dimX + x —
    // deliberately identical to LightProbeVolumeComponent::GridIndex
    // (Scene/Components.h) so baked and realtime probes agree on identity.
    // (Pinned as a formula here instead of including Components.h, which
    // would drag the whole ECS into an L1 math test.)
    glm::ivec3 const dims(5, 3, 4);
    for (i32 z = 0; z < dims.z; ++z)
    {
        for (i32 y = 0; y < dims.y; ++y)
        {
            for (i32 x = 0; x < dims.x; ++x)
            {
                glm::ivec3 const coord(x, y, z);
                i32 const linear = DDGI::ProbeLinearIndex(coord, dims);
                EXPECT_EQ(linear, z * dims.y * dims.x + y * dims.x + x);

                glm::ivec3 const back = DDGI::ProbeGridCoord(linear, dims);
                EXPECT_EQ(back.x, x);
                EXPECT_EQ(back.y, y);
                EXPECT_EQ(back.z, z);
            }
        }
    }
}

// --- 8. Probe tiles are disjoint and inside the atlas ------------------------

TEST(DDGIMath, TileCoordDisjoint)
{
    for (const glm::ivec3& dims : { glm::ivec3(5, 3, 4), glm::ivec3(4, 2, 4) })
    {
        glm::ivec2 const atlasDims = DDGI::AtlasTileDimensions(dims);
        // Header contract: atlas is dims.x tiles wide, dims.y * dims.z tall.
        EXPECT_EQ(atlasDims.x, dims.x);
        EXPECT_EQ(atlasDims.y, dims.y * dims.z);

        i32 const probeCount = dims.x * dims.y * dims.z;
        std::set<std::pair<i32, i32>> seen;
        for (i32 probe = 0; probe < probeCount; ++probe)
        {
            glm::ivec2 const tile = DDGI::ProbeTileCoord(probe, dims);
            EXPECT_GE(tile.x, 0);
            EXPECT_LT(tile.x, atlasDims.x);
            EXPECT_GE(tile.y, 0);
            EXPECT_LT(tile.y, atlasDims.y);

            bool const inserted = seen.insert({ tile.x, tile.y }).second;
            EXPECT_TRUE(inserted) << "duplicate tile (" << tile.x << ", " << tile.y
                                  << ") for probe " << probe;
        }
        // probeCount unique tiles == a bijection onto the atlas grid.
        EXPECT_EQ(static_cast<i32>(seen.size()), probeCount);
    }
}

// --- 9. Corner-anchored probe placement --------------------------------------

TEST(DDGIMath, SpacingAndPlacement)
{
    glm::vec3 const boundsMin(-10.0f);
    glm::vec3 const boundsMax(10.0f);

    // extent / (dims - 1): (20,20,20) / (4,2,4) = (5, 10, 5).
    glm::ivec3 const dims(5, 3, 5);
    glm::vec3 const spacing = DDGI::ProbeSpacing(boundsMin, boundsMax, dims);
    EXPECT_NEAR(spacing.x, 5.0f, 1e-5f);
    EXPECT_NEAR(spacing.y, 10.0f, 1e-5f);
    EXPECT_NEAR(spacing.z, 5.0f, 1e-5f);

    // Corner-anchored lattice: first probe at boundsMin, last at boundsMax.
    glm::vec3 const first = DDGI::ProbeGridPosition({ 0, 0, 0 }, boundsMin, boundsMax, dims);
    EXPECT_NEAR(first.x, boundsMin.x, 1e-4f);
    EXPECT_NEAR(first.y, boundsMin.y, 1e-4f);
    EXPECT_NEAR(first.z, boundsMin.z, 1e-4f);

    glm::vec3 const last = DDGI::ProbeGridPosition(dims - glm::ivec3(1), boundsMin, boundsMax, dims);
    EXPECT_NEAR(last.x, boundsMax.x, 1e-4f);
    EXPECT_NEAR(last.y, boundsMax.y, 1e-4f);
    EXPECT_NEAR(last.z, boundsMax.z, 1e-4f);

    // res == 1 axis (the guard the baker lacks, per the header): no NaN,
    // probe sits at the min corner, spacing degenerates to the full extent
    // (extent / max(dims-1, 1) = 20 / 1).
    glm::ivec3 const flatDims(1, 3, 1);
    glm::vec3 const flatSpacing = DDGI::ProbeSpacing(boundsMin, boundsMax, flatDims);
    EXPECT_TRUE(std::isfinite(flatSpacing.x));
    EXPECT_TRUE(std::isfinite(flatSpacing.y));
    EXPECT_TRUE(std::isfinite(flatSpacing.z));
    EXPECT_NEAR(flatSpacing.x, 20.0f, 1e-4f);
    EXPECT_NEAR(flatSpacing.z, 20.0f, 1e-4f);

    glm::vec3 const flatProbe = DDGI::ProbeGridPosition({ 0, 1, 0 }, boundsMin, boundsMax, flatDims);
    EXPECT_TRUE(std::isfinite(flatProbe.x));
    EXPECT_TRUE(std::isfinite(flatProbe.y));
    EXPECT_TRUE(std::isfinite(flatProbe.z));
    EXPECT_NEAR(flatProbe.x, boundsMin.x, 1e-4f); // collapsed axis: at min
    EXPECT_NEAR(flatProbe.y, 0.0f, 1e-4f);        // -10 + 10*1 (spacing.y = 20/2)
    EXPECT_NEAR(flatProbe.z, boundsMin.z, 1e-4f);
}

// --- 10. Chebyshev visibility weight ------------------------------------------

TEST(DDGIMath, ChebyshevContract)
{
    constexpr f32 kMean = 2.0f;
    constexpr f32 kMeanSq = 4.5f; // variance = 4.5 - 4 = 0.5

    // r <= mean: the surface is closer than the average occluder -> fully lit.
    EXPECT_FLOAT_EQ(DDGI::ChebyshevWeight(kMean, kMeanSq, 0.5f), 1.0f);
    EXPECT_FLOAT_EQ(DDGI::ChebyshevWeight(kMean, kMeanSq, 1.9999f), 1.0f);
    EXPECT_FLOAT_EQ(DDGI::ChebyshevWeight(kMean, kMeanSq, kMean), 1.0f);

    // Monotonically non-increasing past the mean: the Chebyshev bound
    // p = var/(var + (r-mean)^2) decreases in r, and x -> x^3 and max(., floor)
    // preserve monotonicity.
    f32 prev = DDGI::ChebyshevWeight(kMean, kMeanSq, kMean);
    for (i32 i = 1; i <= 40; ++i)
    {
        f32 const r = kMean + static_cast<f32>(i) * 0.1f;
        f32 const w = DDGI::ChebyshevWeight(kMean, kMeanSq, r);
        EXPECT_LE(w, prev + 1e-7f) << "r=" << r;
        EXPECT_GE(w, DDGI::kChebyshevWeightFloor - 1e-7f) << "r=" << r;
        prev = w;
    }

    // Far behind every occluder: floored exactly at kChebyshevWeightFloor
    // (p^3 ~ 1e-13 << 0.05 at r = 100).
    EXPECT_FLOAT_EQ(DDGI::ChebyshevWeight(kMean, kMeanSq, 100.0f), DDGI::kChebyshevWeightFloor);

    // Degenerate distribution (meanSquared == mean^2 -> true variance 0): the
    // 1e-6 variance floor keeps the weight finite (no 0/0 NaN) and the result
    // collapses to the weight floor for r > mean.
    f32 const degenerate = DDGI::ChebyshevWeight(3.0f, 9.0f, 5.0f);
    EXPECT_TRUE(std::isfinite(degenerate));
    EXPECT_FLOAT_EQ(degenerate, DDGI::kChebyshevWeightFloor);
}

// --- 11. Wrap-shading weight bounds -------------------------------------------

TEST(DDGIMath, WrapShadingBounds)
{
    // w = ((dot+1)/2)^2 + 0.2 (RTXGI wrapped backface weight, header comment):
    // dot in [-1,1] -> w in [0.2, 1.2].
    std::mt19937 rng(632u);
    for (i32 i = 0; i < 200; ++i)
    {
        glm::vec3 const a = RandomUnitVector(rng);
        glm::vec3 const b = RandomUnitVector(rng);
        f32 const w = DDGI::WrapShadingWeight(a, b);
        EXPECT_GE(w, 0.2f - 1e-6f);
        EXPECT_LE(w, 1.2f + 1e-6f);
    }

    // Endpoints: dot = 1 -> 1^2 + 0.2 = 1.2; dot = -1 -> 0^2 + 0.2 = 0.2.
    glm::vec3 const n(0.0f, 0.0f, 1.0f);
    EXPECT_NEAR(DDGI::WrapShadingWeight(n, n), 1.2f, 1e-5f);
    EXPECT_NEAR(DDGI::WrapShadingWeight(-n, n), 0.2f, 1e-5f);
}

// --- 12. Weight crush: continuous, monotonic, identity above threshold -------

TEST(DDGIMath, CrushWeightContinuity)
{
    constexpr f32 kThreshold = DDGI::kWeightCrushThreshold; // 0.2

    // Continuity at the threshold: below, w^3/T^2 -> T^3/T^2 = T as w -> T,
    // matching the identity branch. Slope below is 3w^2/T^2 = 3 at w = T, so
    // |f(T+e) - f(T-e)| ~ 4e; with e = 1e-7 that is ~4e-7 < 1e-6.
    f32 const left = DDGI::CrushWeight(kThreshold - 1e-7f);
    f32 const right = DDGI::CrushWeight(kThreshold + 1e-7f);
    EXPECT_NEAR(left, right, 1e-6f);

    // Monotonic non-decreasing on [0, 1] (w^3/T^2 and identity both increase).
    f32 prev = DDGI::CrushWeight(0.0f);
    EXPECT_NEAR(prev, 0.0f, 1e-7f);
    for (i32 i = 1; i <= 1000; ++i)
    {
        f32 const w = static_cast<f32>(i) / 1000.0f;
        f32 const c = DDGI::CrushWeight(w);
        EXPECT_GE(c, prev - 1e-7f) << "w=" << w;
        prev = c;
    }

    // Identity above the threshold.
    EXPECT_FLOAT_EQ(DDGI::CrushWeight(0.25f), 0.25f);
    EXPECT_FLOAT_EQ(DDGI::CrushWeight(0.5f), 0.5f);
    EXPECT_FLOAT_EQ(DDGI::CrushWeight(1.0f), 1.0f);

    // Hand value below: 0.1^3 / 0.2^2 = 0.001 / 0.04 = 0.025.
    EXPECT_NEAR(DDGI::CrushWeight(0.1f), 0.025f, 1e-6f);
}

// --- 13. EMA blend convergence + hysteresis adjustment ------------------------

TEST(DDGIMath, EMAConvergence)
{
    // BlendEMA(new, old, h) = mix(new, old, h) = (1-h)*new + h*old, so the
    // error against a constant target is an exact geometric sequence:
    //   x_k - c = h^k * (x_0 - c).
    glm::vec3 const target(2.0f, -1.0f, 0.5f);
    glm::vec3 const x0(5.0f, 3.0f, -2.0f);
    f32 const h = 0.9f;

    // One-step hand value: mix(2, 5, 0.9) = 2 + 0.9*(5-2) = 4.7.
    glm::vec3 const oneStep = DDGI::BlendEMA(target, x0, h);
    EXPECT_NEAR(oneStep.x, 4.7f, 1e-6f);

    glm::vec3 x = x0;
    constexpr i32 kSteps = 20;
    for (i32 k = 0; k < kSteps; ++k)
    {
        x = DDGI::BlendEMA(target, x, h);
    }
    // 0.9^20 computed in double to keep the reference itself exact.
    f64 const decay = std::pow(static_cast<f64>(h), kSteps);
    for (i32 c = 0; c < 3; ++c)
    {
        f64 const expected = static_cast<f64>(target[c]) + decay * static_cast<f64>(x0[c] - target[c]);
        EXPECT_NEAR(x[c], static_cast<f32>(expected), 1e-5f) << "component " << c;
    }

    // AdjustHysteresis (JCGT 2021 thresholds via the header: >0.8 -> drop
    // history, >0.25 -> reduce by 0.15 floored at 0, else unchanged).
    glm::vec3 const zero(0.0f);
    EXPECT_NEAR(DDGI::AdjustHysteresis(0.97f, glm::vec3(0.3f, 0.0f, 0.0f), zero), 0.82f, 1e-6f);
    EXPECT_FLOAT_EQ(DDGI::AdjustHysteresis(0.1f, glm::vec3(0.3f, 0.0f, 0.0f), zero), 0.0f); // floored
    EXPECT_FLOAT_EQ(DDGI::AdjustHysteresis(0.97f, glm::vec3(0.9f, 0.0f, 0.0f), zero), 0.0f);
    EXPECT_FLOAT_EQ(DDGI::AdjustHysteresis(0.97f, glm::vec3(0.1f, 0.05f, 0.0f), zero), 0.97f);
    // Threshold is strict (> 0.25, exactly representable): 0.25 stays unchanged.
    EXPECT_FLOAT_EQ(DDGI::AdjustHysteresis(0.97f, glm::vec3(0.25f, 0.0f, 0.0f), zero), 0.97f);
    // Max-component semantics: the largest channel decides.
    EXPECT_NEAR(DDGI::AdjustHysteresis(0.97f, glm::vec3(0.05f, 0.3f, 0.1f), zero), 0.82f, 1e-6f);
}

// --- 14. Relocation: backface (inside-geometry) path --------------------------

TEST(DDGIMath, RelocationBackfacePath)
{
    // Targeted: probe inside a wall. BackfaceFraction 0.5 > kBackfaceFraction,
    // closest backface at d = 0.05 along +X, minFrontfaceDistance = 0.1:
    // offsetWorld = d + 0.5*mfd = 0.05 + 0.05 = 0.1 along +X — through the
    // wall and past it by half the comfort margin (>= d).
    {
        DDGI::ProbeHitAggregates agg;
        agg.BackfaceFraction = 0.5f;
        agg.ClosestBackfaceDir = glm::vec3(1.0f, 0.0f, 0.0f);
        agg.ClosestBackfaceDist = 0.05f;

        glm::vec3 const spacing(1.0f);
        glm::vec3 const result = DDGI::RelocateProbe(glm::vec3(0.0f), agg, spacing, 0.1f);
        EXPECT_GE(result.x * spacing.x, agg.ClosestBackfaceDist); // moved through the wall
        EXPECT_NEAR(result.x, 0.1f, 1e-6f);
        EXPECT_NEAR(result.y, 0.0f, 1e-6f);
        EXPECT_NEAR(result.z, 0.0f, 1e-6f);
    }

    // Targeted: raw move exceeds the ellipsoid -> the PREVIOUS offset is kept
    // verbatim (RTXGI accept/reject, header comment). Raw offsetN would be
    // (0.1 + 2.0 + 0.5, 0, 0) = (2.6, 0, 0), dot = 6.76 > 0.45^2.
    {
        DDGI::ProbeHitAggregates agg;
        agg.BackfaceFraction = 0.5f;
        agg.ClosestBackfaceDir = glm::vec3(1.0f, 0.0f, 0.0f);
        agg.ClosestBackfaceDist = 2.0f;

        glm::vec3 const current(0.1f, 0.0f, 0.0f);
        glm::vec3 const result = DDGI::RelocateProbe(current, agg, glm::vec3(1.0f), 1.0f);
        EXPECT_FLOAT_EQ(result.x, current.x);
        EXPECT_FLOAT_EQ(result.y, current.y);
        EXPECT_FLOAT_EQ(result.z, current.z);
    }

    // Property: over randomized aggregates the result NEVER leaves the
    // kMaxProbeOffsetFraction ellipsoid: dot(offsetN, offsetN) < 0.45^2 =
    // 0.2025 (accepted moves satisfy it by the guard; rejected moves return
    // the current offset, generated here with norm <= 0.4 -> dot <= 0.16).
    std::mt19937 rng(14632u);
    std::uniform_real_distribution<f32> u01(0.0f, 1.0f);
    constexpr f32 kMaxDotSq = DDGI::kMaxProbeOffsetFraction * DDGI::kMaxProbeOffsetFraction; // 0.2025

    for (i32 i = 0; i < 200; ++i)
    {
        glm::vec3 const current = RandomUnitVector(rng) * (u01(rng) * 0.4f);
        glm::vec3 const spacing(0.5f + 3.5f * u01(rng), 0.5f + 3.5f * u01(rng), 0.5f + 3.5f * u01(rng));
        f32 const mfd = 0.1f + 1.9f * u01(rng);

        DDGI::ProbeHitAggregates agg;
        agg.BackfaceFraction = u01(rng);
        agg.ClosestBackfaceDir = RandomUnitVector(rng);
        agg.ClosestBackfaceDist = u01(rng) * 4.0f - 1.0f; // [-1, 3), < 0 = none
        agg.ClosestFrontfaceDir = RandomUnitVector(rng);
        agg.ClosestFrontfaceDist = u01(rng) * 6.0f - 1.0f; // [-1, 5), < 0 = none
        agg.FarthestFrontfaceDir = RandomUnitVector(rng);
        agg.FarthestFrontfaceDist = (agg.ClosestFrontfaceDist >= 0.0f)
                                        ? agg.ClosestFrontfaceDist + u01(rng) * 5.0f
                                        : u01(rng) * 6.0f - 1.0f;
        agg.AnyHitWithinCell = (u01(rng) < 0.5f);

        glm::vec3 const result = DDGI::RelocateProbe(current, agg, spacing, mfd);
        EXPECT_TRUE(std::isfinite(result.x) && std::isfinite(result.y) && std::isfinite(result.z));
        EXPECT_LT(glm::dot(result, result), kMaxDotSq + 1e-6f)
            << "config " << i << ": offsetN escaped the 0.45 ellipsoid";
    }
}

// --- 15. Relocation: comfortable probes decay back to the grid point ----------

TEST(DDGIMath, RelocationComfortDecay)
{
    glm::vec3 const spacing(1.0f);
    f32 const mfd = 1.0f;

    // Frontface at 1.2 > mfd: per-iteration headroom is min(1.2 - 1.0, |off|)
    // = 0.2 world units, so |(0.3, 0.2, 0.1)| = sqrt(0.14) ~ 0.374 shrinks
    // 0.374 -> 0.174 -> 0 over two iterations; length never increases.
    {
        DDGI::ProbeHitAggregates agg;
        agg.BackfaceFraction = 0.0f;
        agg.ClosestBackfaceDist = -1.0f;
        agg.ClosestFrontfaceDir = glm::vec3(0.0f, 1.0f, 0.0f);
        agg.ClosestFrontfaceDist = 1.2f;
        agg.FarthestFrontfaceDir = glm::vec3(0.0f, -1.0f, 0.0f);
        agg.FarthestFrontfaceDist = 4.0f;

        glm::vec3 offset(0.3f, 0.2f, 0.1f);
        f32 prevLen = glm::length(offset);
        for (i32 iter = 0; iter < 10; ++iter)
        {
            offset = DDGI::RelocateProbe(offset, agg, spacing, mfd);
            f32 const len = glm::length(offset);
            EXPECT_LE(len, prevLen + 1e-6f) << "iteration " << iter;
            prevLen = len;
        }
        EXPECT_LT(glm::length(offset), 1e-5f);
    }

    // No frontface at all (dist < 0): headroom = full offset length, so the
    // decay completes in a single step.
    {
        DDGI::ProbeHitAggregates agg;
        agg.BackfaceFraction = 0.0f;
        agg.ClosestBackfaceDist = -1.0f;
        agg.ClosestFrontfaceDist = -1.0f;
        agg.FarthestFrontfaceDist = -1.0f;

        glm::vec3 const result = DDGI::RelocateProbe(glm::vec3(0.2f, -0.1f, 0.15f), agg, spacing, mfd);
        EXPECT_LT(glm::length(result), 1e-6f);
    }
}

// --- 16. Relocation: oscillation guard -----------------------------------------

TEST(DDGIMath, RelocationOscillationGuard)
{
    glm::vec3 const spacing(4.0f);
    f32 const mfd = 1.0f;
    glm::vec3 const current(0.05f, 0.0125f, 0.0f);

    // Too close to a wall (closest 0.2 < mfd), but the farthest frontface
    // points the SAME way (dot > 0): sliding along it would push into the
    // near wall, so the guard keeps the offset unchanged.
    {
        DDGI::ProbeHitAggregates agg;
        agg.BackfaceFraction = 0.0f;
        agg.ClosestBackfaceDist = -1.0f;
        agg.ClosestFrontfaceDir = glm::vec3(1.0f, 0.0f, 0.0f);
        agg.ClosestFrontfaceDist = 0.2f;
        agg.FarthestFrontfaceDir = glm::normalize(glm::vec3(1.0f, 0.2f, 0.0f)); // dot > 0
        agg.FarthestFrontfaceDist = 3.0f;

        glm::vec3 const result = DDGI::RelocateProbe(current, agg, spacing, mfd);
        EXPECT_NEAR(result.x, current.x, 1e-6f);
        EXPECT_NEAR(result.y, current.y, 1e-6f);
        EXPECT_NEAR(result.z, current.z, 1e-6f);
    }

    // Control: with an OPPOSING farthest frontface (dot <= 0) the same setup
    // does move — proving the guard (not the ellipsoid or branch selection)
    // froze the offset above. World move = -min(3, 1) = -1 along +X:
    // offsetN.x = (0.05*4 - 1)/4 = -0.2, dot = 0.04+... < 0.2025 -> accepted.
    {
        DDGI::ProbeHitAggregates agg;
        agg.BackfaceFraction = 0.0f;
        agg.ClosestBackfaceDist = -1.0f;
        agg.ClosestFrontfaceDir = glm::vec3(1.0f, 0.0f, 0.0f);
        agg.ClosestFrontfaceDist = 0.2f;
        agg.FarthestFrontfaceDir = glm::vec3(-1.0f, 0.0f, 0.0f); // dot = -1
        agg.FarthestFrontfaceDist = 3.0f;

        glm::vec3 const result = DDGI::RelocateProbe(current, agg, spacing, mfd);
        EXPECT_NEAR(result.x, -0.2f, 1e-5f);
        EXPECT_LT(result.x, current.x); // actually moved away from the wall
    }
}

// --- 17. Classification --------------------------------------------------------

TEST(DDGIMath, Classification)
{
    // Pinned behavior: the CURRENT ClassifyProbe looks at BackfaceFraction
    // only (strict > kBackfaceFraction = 0.25). The ProbeState::Active enum
    // comment ("geometry within its cell") hints at a cell-occupancy
    // refinement, but the header implements none and documents none as
    // pending — AnyHitWithinCell must therefore not affect the result. If a
    // future slice adds cell-based refinement, this test is the one to update.
    DDGI::ProbeHitAggregates agg;

    agg.BackfaceFraction = 0.3f; // > 0.25 -> inside geometry
    agg.AnyHitWithinCell = false;
    EXPECT_EQ(DDGI::ClassifyProbe(agg), DDGI::ProbeState::Inactive);
    agg.AnyHitWithinCell = true;
    EXPECT_EQ(DDGI::ClassifyProbe(agg), DDGI::ProbeState::Inactive);

    agg.BackfaceFraction = 0.1f; // <= 0.25 -> active
    agg.AnyHitWithinCell = false;
    EXPECT_EQ(DDGI::ClassifyProbe(agg), DDGI::ProbeState::Active);
    agg.AnyHitWithinCell = true;
    EXPECT_EQ(DDGI::ClassifyProbe(agg), DDGI::ProbeState::Active);

    // Strict inequality at the exactly-representable threshold 0.25.
    agg.BackfaceFraction = 0.25f;
    EXPECT_EQ(DDGI::ClassifyProbe(agg), DDGI::ProbeState::Active);
}

// --- 18. Hit-cache resolution snapping ------------------------------------------

TEST(DDGIMath, HitCacheSnap)
{
    // Snap thresholds are the squares of the supported angular resolutions:
    // <= 8^2 = 64 -> 8; <= 16^2 = 256 -> 16; above -> 32 (header constants).
    EXPECT_EQ(DDGI::HitCacheResolutionForRayCount(1), 8);
    EXPECT_EQ(DDGI::HitCacheResolutionForRayCount(64), 8);
    EXPECT_EQ(DDGI::HitCacheResolutionForRayCount(65), 16);
    EXPECT_EQ(DDGI::HitCacheResolutionForRayCount(256), 16);
    EXPECT_EQ(DDGI::HitCacheResolutionForRayCount(257), 32);
    EXPECT_EQ(DDGI::HitCacheResolutionForRayCount(1024), 32);
    EXPECT_EQ(DDGI::HitCacheResolutionForRayCount(4096), 32);
}

// --- 19. Irradiance ratio estimator: exact for a constant field -----------------
// THE storage-convention pin: the atlas stores FULL irradiance
// E = pi * sum(w * L) / sum(w), w = max(0, dot(texelDir, hitDir)) — no
// extra 2*pi factor. Shared with DDGI_BlendIrradiance.glsl and
// ddgiSampleIrradiance; a convention drift on either side shows up as a
// pi-vs-2pi brightness factor. For constant radiance L the ratio estimator
// is exact regardless of the direction set: sum(w*L)/sum(w) = L, so E = pi*L.
// (The pi comes from the Lambertian estimator normalization: with
// cosine-weighted ratio weights, E = pi * (weighted mean radiance) — RTXGI
// DDGIProbeBlendingCS convention.)

TEST(DDGIMath, IrradianceEstimatorConstantField)
{
    // The actual medium hit-cache direction set: TexelDirection over all
    // 16 x 16 = 256 hit texels.
    std::vector<glm::vec3> hits;
    for (i32 ty = 0; ty < DDGI::kHitCacheResolutionMedium; ++ty)
    {
        for (i32 tx = 0; tx < DDGI::kHitCacheResolutionMedium; ++tx)
        {
            hits.push_back(DDGI::TexelDirection({ tx, ty }, DDGI::kHitCacheResolutionMedium));
        }
    }

    glm::vec3 const radiance(0.7f, 0.3f, 1.5f);
    glm::vec3 const expected = glm::pi<f32>() * radiance;

    // Evaluate at every irradiance texel direction of the 6x6 interior.
    for (i32 ty = 0; ty < DDGI::kIrradianceInteriorTexels; ++ty)
    {
        for (i32 tx = 0; tx < DDGI::kIrradianceInteriorTexels; ++tx)
        {
            glm::vec3 const texelDir = DDGI::TexelDirection({ tx, ty }, DDGI::kIrradianceInteriorTexels);

            f32 sumW = 0.0f;
            glm::vec3 sumWL(0.0f);
            for (const glm::vec3& hit : hits)
            {
                f32 const w = DDGI::IrradianceBlendWeight(texelDir, hit);
                sumW += w;
                sumWL += w * radiance;
            }
            ASSERT_GT(sumW, 0.0f);

            glm::vec3 const estimate = glm::pi<f32>() * sumWL / sumW;
            EXPECT_NEAR(estimate.x, expected.x, 1e-4f) << "texel (" << tx << ", " << ty << ")";
            EXPECT_NEAR(estimate.y, expected.y, 1e-4f) << "texel (" << tx << ", " << ty << ")";
            EXPECT_NEAR(estimate.z, expected.z, 1e-4f) << "texel (" << tx << ", " << ty << ")";
        }
    }
}

// --- 20. Irradiance ratio estimator: cosine-lobe field (bias documentation) -----

TEST(DDGIMath, IrradianceEstimatorCosineField)
{
    // Radiance field L(w) = max(0, dot(w, +Z)) — a cosine lobe about +Z.
    // Analytic irradiance at a surface with normal +Z:
    //   E = Int_{hemisphere} L(w) cos(theta) dw
    //     = Int_0^{2pi} Int_0^{pi/2} cos^2(theta) sin(theta) dtheta dphi
    //     = 2*pi * [-cos^3(theta)/3]_0^{pi/2} = 2*pi/3 ~ 2.0944.
    // The discrete ratio estimator over the 16x16 octahedral texel-center
    // direction set is biased at finite resolution (octahedral texels subtend
    // unequal solid angles, correlated with the cosine weight); measured bias
    // is ~+4.4% at N = 16 (double-precision reference computation), so the 5%
    // budget documents the estimator's finite-resolution accuracy.
    std::vector<glm::vec3> hits;
    for (i32 ty = 0; ty < DDGI::kHitCacheResolutionMedium; ++ty)
    {
        for (i32 tx = 0; tx < DDGI::kHitCacheResolutionMedium; ++tx)
        {
            hits.push_back(DDGI::TexelDirection({ tx, ty }, DDGI::kHitCacheResolutionMedium));
        }
    }

    glm::vec3 const texelDir(0.0f, 0.0f, 1.0f);
    f32 sumW = 0.0f;
    f32 sumWL = 0.0f;
    for (const glm::vec3& hit : hits)
    {
        f32 const w = DDGI::IrradianceBlendWeight(texelDir, hit);
        f32 const radiance = glm::max(0.0f, hit.z); // cosine lobe about +Z
        sumW += w;
        sumWL += w * radiance;
    }
    ASSERT_GT(sumW, 0.0f);

    f32 const estimate = glm::pi<f32>() * sumWL / sumW;
    f32 const analytic = 2.0f * glm::pi<f32>() / 3.0f;
    EXPECT_NEAR(estimate, analytic, 0.05f * analytic);
}

// --- 21. Volume weight: the bounce path's margined membership test ------------
//
// Issue #751. `VolumeWeight` is the one function both DDGI sample paths go
// through, and the whole fix rests on two properties of it:
//
//   * margin == 0 is EXACTLY the old hard inside-test, so the lit path did not
//     change; and
//   * with a margin it is continuous — no step at the bounds, no step at the
//     margin surface — because a discontinuity in the feedback term would show
//     up as a hard edge in the GI wherever geometry crossed it.

TEST(DDGIMath, VolumeWeightZeroMarginIsTheHardInsideTest)
{
    glm::vec3 const boundsMin(-3.0f, 0.8f, -3.0f);
    glm::vec3 const boundsMax(3.0f, 3.2f, 3.0f);

    std::mt19937 rng(0x751u);
    std::uniform_real_distribution<f32> dist(-6.0f, 6.0f);
    for (i32 i = 0; i < 4000; ++i)
    {
        glm::vec3 const p(dist(rng), dist(rng), dist(rng));
        f32 const weight = DDGI::VolumeWeight(p, boundsMin, boundsMax, glm::vec3(0.0f));
        bool const inside = DDGI::IsInsideVolume(p, boundsMin, boundsMax);
        EXPECT_FLOAT_EQ(weight, inside ? 1.0f : 0.0f)
            << "margin 0 must reproduce the hard test exactly at (" << p.x << ", " << p.y << ", " << p.z << ")";
    }

    // The boundary itself is INCLUSIVE in both, corners included.
    for (i32 corner = 0; corner < 8; ++corner)
    {
        glm::vec3 const p((corner & 1) ? boundsMax.x : boundsMin.x, (corner & 2) ? boundsMax.y : boundsMin.y,
                          (corner & 4) ? boundsMax.z : boundsMin.z);
        EXPECT_FLOAT_EQ(DDGI::VolumeWeight(p, boundsMin, boundsMax, glm::vec3(0.0f)), 1.0f);
        EXPECT_TRUE(DDGI::IsInsideVolume(p, boundsMin, boundsMax));
    }
}

TEST(DDGIMath, VolumeWeightFadesOverTheMarginAndStopsThere)
{
    glm::vec3 const boundsMin(-3.0f, 0.8f, -3.0f);
    glm::vec3 const boundsMax(3.0f, 3.2f, 3.0f);
    glm::vec3 const spacing = DDGI::ProbeSpacing(boundsMin, boundsMax, glm::ivec3(3, 2, 3));
    glm::vec3 const margin = DDGI::BounceMargin(spacing);
    ASSERT_GT(margin.x, 0.0f);

    // Inside is still exactly 1 — a margin must not dim the interior.
    EXPECT_FLOAT_EQ(DDGI::VolumeWeight(glm::vec3(0.0f, 2.0f, 0.0f), boundsMin, boundsMax, margin), 1.0f);
    EXPECT_FLOAT_EQ(DDGI::VolumeWeight(boundsMax, boundsMin, boundsMax, margin), 1.0f);

    // Walking straight out along +x: monotonically decreasing, 1 at the face,
    // 0 at exactly one margin out, 0 beyond.
    f32 previous = 1.0f;
    for (i32 step = 0; step <= 20; ++step)
    {
        f32 const t = static_cast<f32>(step) / 20.0f;
        glm::vec3 const p(boundsMax.x + t * margin.x, 2.0f, 0.0f);
        f32 const weight = DDGI::VolumeWeight(p, boundsMin, boundsMax, margin);
        EXPECT_LE(weight, previous + 1e-6f) << "weight must not increase as the point leaves the volume (t = " << t << ")";
        EXPECT_GE(weight, 0.0f);
        EXPECT_LE(weight, 1.0f);
        previous = weight;
    }
    EXPECT_NEAR(DDGI::VolumeWeight(glm::vec3(boundsMax.x + margin.x, 2.0f, 0.0f), boundsMin, boundsMax, margin), 0.0f,
                1e-6f);
    EXPECT_FLOAT_EQ(DDGI::VolumeWeight(glm::vec3(boundsMax.x + 1.001f * margin.x, 2.0f, 0.0f), boundsMin, boundsMax, margin),
                    0.0f);

    // Half a margin out is the smoothstep midpoint by construction.
    EXPECT_NEAR(DDGI::VolumeWeight(glm::vec3(boundsMax.x + 0.5f * margin.x, 2.0f, 0.0f), boundsMin, boundsMax, margin),
                0.5f, 1e-5f);
}

TEST(DDGIMath, VolumeWeightIsContinuousEverywhere)
{
    // A step anywhere in this function is a step in the bounce term, which
    // draws a hard edge in the GI wherever geometry crosses it. Sample dense
    // pairs across both interfaces (bounds face and margin surface) and across
    // an edge and a corner, where the per-axis distances combine.
    glm::vec3 const boundsMin(-3.0f, 0.8f, -3.0f);
    glm::vec3 const boundsMax(3.0f, 3.2f, 3.0f);
    glm::vec3 const margin(3.0f, 2.4f, 3.0f);
    constexpr f32 kStep = 1e-3f;

    auto const walk = [&](const glm::vec3& from, const glm::vec3& to)
    {
        i32 const steps = 4000;
        f32 previousWeight = DDGI::VolumeWeight(from, boundsMin, boundsMax, margin);
        for (i32 i = 1; i <= steps; ++i)
        {
            glm::vec3 const p = glm::mix(from, to, static_cast<f32>(i) / static_cast<f32>(steps));
            f32 const weight = DDGI::VolumeWeight(p, boundsMin, boundsMax, margin);
            // The smoothstep's Lipschitz constant is 1.5 per margin; the walk
            // step is far smaller than that, so any real discontinuity dwarfs
            // this bound.
            f32 const walkLength = glm::length(to - from) / static_cast<f32>(steps);
            EXPECT_LT(std::abs(weight - previousWeight), 2.0f * walkLength / glm::min(margin.x, glm::min(margin.y, margin.z)) + kStep)
                << "discontinuity at (" << p.x << ", " << p.y << ", " << p.z << ")";
            previousWeight = weight;
        }
    };

    walk(glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(9.0f, 2.0f, 0.0f));    // through a face
    walk(glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(9.0f, 2.0f, 9.0f));    // through an edge
    walk(glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(9.0f, 8.0f, 9.0f));    // through a corner
    walk(glm::vec3(-9.0f, -6.0f, -9.0f), glm::vec3(9.0f, 8.0f, 9.0f)); // corner to corner
}

TEST(DDGIMath, VolumeWeightCoversAnAirFittedRoomButNotAnArbitrarySurface)
{
    // The concrete case from issue #751 and DDGIReferenceParityTest: a 3x2x3
    // volume fitted to the AIR of a room whose wall inner faces are 0.9 out in
    // x/z and whose floor/ceiling are 0.7 out in y. Every one of those
    // surfaces must now carry real bounce weight — that is the entire bug —
    // while a surface a long way outside must still contribute nothing, which
    // is what keeps the extrapolation bounded.
    glm::vec3 const boundsMin(-3.0f, 0.8f, -3.0f);
    glm::vec3 const boundsMax(3.0f, 3.2f, 3.0f);
    glm::vec3 const margin = DDGI::BounceMargin(DDGI::ProbeSpacing(boundsMin, boundsMax, glm::ivec3(3, 2, 3)));

    struct Surface
    {
        const char* Name;
        glm::vec3 Point;
    };
    const Surface roomSurfaces[] = {
        { "left wall", { -3.9f, 2.0f, 0.0f } },
        { "right wall", { 3.9f, 2.0f, 0.0f } },
        { "back wall", { 0.0f, 2.0f, -3.9f } },
        { "front wall", { 0.0f, 2.0f, 3.9f } },
        { "floor", { 0.0f, 0.1f, 0.0f } },
        { "ceiling", { 0.0f, 3.9f, 0.0f } },
        { "floor/wall corner", { -3.9f, 0.1f, 0.0f } },
    };
    for (const Surface& surface : roomSurfaces)
    {
        f32 const weight = DDGI::VolumeWeight(surface.Point, boundsMin, boundsMax, margin);
        EXPECT_GT(weight, 0.4f) << surface.Name << " contributes almost no bounce light — this is the #751 bug";
        EXPECT_EQ(DDGI::VolumeWeight(surface.Point, boundsMin, boundsMax, glm::vec3(0.0f)), 0.0f)
            << surface.Name << " was expected to be OUTSIDE the volume (otherwise the fixture is wrong, "
                               "not the code)";
    }

    // Bounded extrapolation: one spacing is the reach, not "anywhere outside".
    EXPECT_FLOAT_EQ(DDGI::VolumeWeight({ 40.0f, 2.0f, 0.0f }, boundsMin, boundsMax, margin), 0.0f);
    EXPECT_FLOAT_EQ(DDGI::VolumeWeight({ 0.0f, -30.0f, 0.0f }, boundsMin, boundsMax, margin), 0.0f);
}

// --- 22. Volume weight: a non-finite position reads as OUTSIDE -----------------
//
// The predicate this replaced got this for free — `all(greaterThanEqual(NaN,
// ...))` is false, so a NaN shading point simply fell out of the volume and
// the caller dropped to the baked-SH ladder. A smoothstep does not: every
// comparison against NaN is false, so without an explicit positive test the
// weight comes out NaN, the caller's `weight <= 0` guard does not fire, and
// NaN irradiance reaches the lit passes — where their `dot(ddgi, ddgi) <= 0`
// fallback does not catch it either.

TEST(DDGIMath, VolumeWeightRejectsNonFinitePositions)
{
    glm::vec3 const boundsMin(-3.0f, 0.8f, -3.0f);
    glm::vec3 const boundsMax(3.0f, 3.2f, 3.0f);
    glm::vec3 const margin(3.0f, 2.4f, 3.0f);

    f32 const nan = std::numeric_limits<f32>::quiet_NaN();
    f32 const inf = std::numeric_limits<f32>::infinity();

    for (i32 axis = 0; axis < 3; ++axis)
    {
        for (f32 bad : { nan, inf, -inf })
        {
            glm::vec3 p(0.0f, 2.0f, 0.0f);
            p[axis] = bad;
            EXPECT_EQ(DDGI::VolumeWeight(p, boundsMin, boundsMax, margin), 0.0f)
                << "a non-finite component on axis " << axis << " must read as outside, not poison the gather";
            EXPECT_EQ(DDGI::VolumeWeight(p, boundsMin, boundsMax, glm::vec3(0.0f)), 0.0f)
                << "...and likewise on the lit path's zero-margin lookup";
        }
    }

    EXPECT_EQ(DDGI::VolumeWeight(glm::vec3(nan), boundsMin, boundsMax, margin), 0.0f);
    // The old hard test agrees, which is the property being preserved.
    EXPECT_FALSE(DDGI::IsInsideVolume(glm::vec3(nan), boundsMin, boundsMax));
}

// --- 23. Bounce margin on a single-probe axis ---------------------------------
//
// ProbeSpacing divides by max(dims - 1, 1), so a resolution-1 axis reports the
// whole extent as its spacing and the bounce margin inherits it. That is the
// one-spacing rule applied consistently — with a single probe layer the
// volume's interior is ALREADY constant-extrapolated across the whole extent —
// but it means a deliberately flat volume reaches a full extent past its
// bounds, so it is pinned here rather than left to be discovered.

TEST(DDGIMath, BounceMarginOnASingleProbeAxisIsTheWholeExtent)
{
    glm::vec3 const boundsMin(-10.0f, 0.0f, -10.0f);
    glm::vec3 const boundsMax(10.0f, 4.0f, 10.0f);

    glm::ivec3 const flat(5, 1, 5);
    glm::vec3 const flatMargin = DDGI::BounceMargin(DDGI::ProbeSpacing(boundsMin, boundsMax, flat));
    EXPECT_FLOAT_EQ(flatMargin.x, 5.0f); // 20 extent / 4 cells
    EXPECT_FLOAT_EQ(flatMargin.y, 4.0f); // one probe layer -> the whole extent
    EXPECT_FLOAT_EQ(flatMargin.z, 5.0f);

    // A point one full extent above the volume therefore still gathers...
    EXPECT_GT(DDGI::VolumeWeight({ 0.0f, 4.0f + 0.25f * flatMargin.y, 0.0f }, boundsMin, boundsMax, flatMargin), 0.0f);
    // ...but the reach is still bounded, which is the property that matters.
    EXPECT_FLOAT_EQ(
        DDGI::VolumeWeight({ 0.0f, 4.0f + 1.01f * flatMargin.y, 0.0f }, boundsMin, boundsMax, flatMargin), 0.0f);

    // With two probe layers the same volume gets the ordinary cell-sized margin.
    glm::vec3 const normalMargin = DDGI::BounceMargin(DDGI::ProbeSpacing(boundsMin, boundsMax, glm::ivec3(5, 2, 5)));
    EXPECT_FLOAT_EQ(normalMargin.y, 4.0f);
    glm::vec3 const denseMargin = DDGI::BounceMargin(DDGI::ProbeSpacing(boundsMin, boundsMax, glm::ivec3(5, 5, 5)));
    EXPECT_FLOAT_EQ(denseMargin.y, 1.0f);
}

// =============================================================================
// Issue #707 — cascades, toroidal storage, sparsity, variable update rate and
// the spring-force relocation.
//
// Every one of these fails SILENTLY in the frame: a wrong wrap shows as GI
// smearing behind the camera, a wrong blend band as a soft seam, a wrong
// update-rate phase as blotchy convergence, a wrong spring as probes drifting
// into geometry over many frames. None of them make a picture obviously wrong
// on the frame they happen, which is exactly why they are pinned here.
// =============================================================================

// The authored single-volume path must reproduce the pre-#707 layout EXACTLY.
// This is the load-bearing compatibility claim: the reference-path-tracer
// parity tests, the goldens and the visual-evidence PNGs were all measured
// against that layout, so "equivalent" is not good enough — it has to be the
// same probe index, the same tile and the same world position.
TEST(DDGIMath, AuthoredCascadeReproducesTheLegacyLayoutExactly)
{
    glm::vec3 const boundsMin(-10.0f, -2.0f, -10.0f);
    glm::vec3 const boundsMax(10.0f, 2.0f, 10.0f);
    glm::ivec3 const dims(4, 3, 4);

    DDGI::CascadeGrid const grid = DDGI::MakeAuthoredCascade(boundsMin, boundsMax, dims);
    EXPECT_EQ(grid.LatticeMin, glm::ivec3(0));
    EXPECT_EQ(grid.Dims, dims);
    EXPECT_EQ(grid.Spacing, DDGI::ProbeSpacing(boundsMin, boundsMax, dims));

    for (i32 z = 0; z < dims.z; ++z)
    {
        for (i32 y = 0; y < dims.y; ++y)
        {
            for (i32 x = 0; x < dims.x; ++x)
            {
                glm::ivec3 const coord(x, y, z);
                i32 const legacyIndex = DDGI::ProbeLinearIndex(coord, dims);

                EXPECT_EQ(DDGI::CascadedProbeIndex(0, coord, dims), legacyIndex);
                EXPECT_EQ(DDGI::CascadedProbeTileCoord(0, coord, dims), DDGI::ProbeTileCoord(legacyIndex, dims));
                EXPECT_EQ(DDGI::CascadedProbeTileCoord(legacyIndex, dims), DDGI::ProbeTileCoord(legacyIndex, dims));
                EXPECT_EQ(DDGI::CascadeOfProbeIndex(legacyIndex, dims), 0);
                EXPECT_EQ(DDGI::StorageCoordOfProbeIndex(legacyIndex, dims), coord);

                glm::vec3 const legacyPos = DDGI::ProbeGridPosition(coord, boundsMin, boundsMax, dims);
                glm::vec3 const cascadePos = DDGI::CascadeProbeGridPosition(coord, grid);
                EXPECT_NEAR(cascadePos.x, legacyPos.x, 1e-5f);
                EXPECT_NEAR(cascadePos.y, legacyPos.y, 1e-5f);
                EXPECT_NEAR(cascadePos.z, legacyPos.z, 1e-5f);
            }
        }
    }

    EXPECT_EQ(DDGI::CascadedAtlasTileDimensions(dims, 1), DDGI::AtlasTileDimensions(dims));
    glm::vec3 const cascadeMin = DDGI::CascadeBoundsMin(grid);
    glm::vec3 const cascadeMax = DDGI::CascadeBoundsMax(grid);
    EXPECT_NEAR(cascadeMin.x, boundsMin.x, 1e-5f);
    EXPECT_NEAR(cascadeMax.y, boundsMax.y, 1e-5f);
    EXPECT_NEAR(cascadeMax.z, boundsMax.z, 1e-5f);
}

// The property the entire cascade design rests on: moving the window by one
// cell must reassign exactly ONE SLAB of probes, not the whole grid. Our
// capture is a rasterized cube-face bake, so a scheme that invalidated every
// probe on every one-cell camera step could never converge.
TEST(DDGIMath, ToroidalStorageInvalidatesOnlyOneSlabPerCellOfCameraMotion)
{
    glm::ivec3 const dims(16, 16, 16);
    DDGI::CascadeGrid const before = DDGI::MakeCameraCascade(0, glm::vec3(0.0f), glm::vec3(1.0f), dims);
    DDGI::CascadeGrid after = before;
    after.LatticeMin.x += 1;

    i32 changed = 0;
    for (i32 z = 0; z < dims.z; ++z)
    {
        for (i32 y = 0; y < dims.y; ++y)
        {
            for (i32 x = 0; x < dims.x; ++x)
            {
                glm::ivec3 const storage(x, y, z);
                if (DDGI::CascadeProbeGridPosition(storage, before) != DDGI::CascadeProbeGridPosition(storage, after))
                {
                    ++changed;
                }
            }
        }
    }
    EXPECT_EQ(changed, dims.y * dims.z) << "a one-cell shift must move exactly one YZ slab";

    // ...and shifting by a whole window length must move everything, which is
    // the other end of the same property (and would silently pass if the wrap
    // were a no-op).
    DDGI::CascadeGrid wrappedAround = before;
    wrappedAround.LatticeMin.x += dims.x;
    i32 farChanged = 0;
    for (i32 x = 0; x < dims.x; ++x)
    {
        if (DDGI::CascadeProbeGridPosition({ x, 0, 0 }, before) != DDGI::CascadeProbeGridPosition({ x, 0, 0 }, wrappedAround))
        {
            ++farChanged;
        }
    }
    EXPECT_EQ(farChanged, dims.x);
}

// The single most load-bearing line in the toroidal scheme: C++ % and GLSL %
// both truncate toward zero, so a negative lattice coordinate maps to a
// NEGATIVE storage index unless the modulo is Euclidean. Cascades go negative
// the moment the camera moves toward -x, so this is not an edge case.
TEST(DDGIMath, NegativeLatticeCoordinatesWrapIntoTheStorageWindow)
{
    glm::ivec3 const dims(16, 8, 16);
    DDGI::CascadeGrid const grid = DDGI::MakeCameraCascade(0, glm::vec3(-137.0f, -41.0f, -9.0f), glm::vec3(1.0f), dims);
    ASSERT_LT(grid.LatticeMin.x, 0) << "the fixture must actually exercise negative lattice coordinates";

    for (i32 z = 0; z < dims.z; ++z)
    {
        for (i32 y = 0; y < dims.y; ++y)
        {
            for (i32 x = 0; x < dims.x; ++x)
            {
                glm::ivec3 const storage(x, y, z);
                glm::ivec3 const lattice = DDGI::LatticeForStorageCoord(storage, grid);

                // Inside the window on every axis...
                EXPECT_GE(lattice.x, grid.LatticeMin.x);
                EXPECT_LT(lattice.x, grid.LatticeMin.x + dims.x);
                EXPECT_GE(lattice.y, grid.LatticeMin.y);
                EXPECT_LT(lattice.y, grid.LatticeMin.y + dims.y);
                EXPECT_GE(lattice.z, grid.LatticeMin.z);
                EXPECT_LT(lattice.z, grid.LatticeMin.z + dims.z);

                // ...and a round trip back to the same storage slot.
                EXPECT_EQ(DDGI::StorageCoordForLattice(lattice, dims), storage);
            }
        }
    }

    EXPECT_EQ(DDGI::WrapIndex(-1, 16), 15);
    EXPECT_EQ(DDGI::WrapIndex(-16, 16), 0);
    EXPECT_EQ(DDGI::WrapIndex(-17, 16), 15);
    EXPECT_EQ(DDGI::WrapIndex(17, 16), 1);
}

TEST(DDGIMath, CascadeSpacingDoublesPerLevel)
{
    glm::vec3 const base(1.5f, 1.5f, 1.5f);
    EXPECT_EQ(DDGI::CascadeSpacing(base, 0), base);
    EXPECT_EQ(DDGI::CascadeSpacing(base, 1), base * 2.0f);
    EXPECT_EQ(DDGI::CascadeSpacing(base, 3), base * 8.0f);

    // Each cascade therefore covers twice the extent of the previous one,
    // which is what makes N cascades reach 2^N times cascade 0.
    glm::ivec3 const dims(16, 16, 16);
    DDGI::CascadeGrid const c0 = DDGI::MakeCameraCascade(0, glm::vec3(0.0f), base, dims);
    DDGI::CascadeGrid const c3 = DDGI::MakeCameraCascade(3, glm::vec3(0.0f), base, dims);
    f32 const extent0 = (DDGI::CascadeBoundsMax(c0) - DDGI::CascadeBoundsMin(c0)).x;
    f32 const extent3 = (DDGI::CascadeBoundsMax(c3) - DDGI::CascadeBoundsMin(c3)).x;
    EXPECT_NEAR(extent3, extent0 * 8.0f, 1e-3f);
}

// The blend band is where cascades go wrong, and a subtly wrong one reads as
// "GI is a bit soft near the transition" rather than as a failure.
TEST(DDGIMath, CascadeInteriorWeightFallsMonotonicallyToZeroAcrossTheBand)
{
    glm::ivec3 const dims(16, 16, 16);
    DDGI::CascadeGrid const grid = DDGI::MakeCameraCascade(0, glm::vec3(0.0f), glm::vec3(1.0f), dims);
    glm::vec3 const boundsMin = DDGI::CascadeBoundsMin(grid);
    glm::vec3 const boundsMax = DDGI::CascadeBoundsMax(grid);
    glm::vec3 const centre = (boundsMin + boundsMax) * 0.5f;
    constexpr f32 kBand = 0.2f;

    EXPECT_FLOAT_EQ(DDGI::CascadeInteriorWeight(centre, grid, kBand), 1.0f);
    EXPECT_FLOAT_EQ(DDGI::CascadeInteriorWeight(boundsMax + glm::vec3(0.5f), grid, kBand), 0.0f);
    EXPECT_FLOAT_EQ(DDGI::CascadeInteriorWeight(boundsMax, grid, kBand), 0.0f);

    f32 previous = 1.0f;
    bool sawIntermediate = false;
    for (i32 step = 0; step <= 40; ++step)
    {
        f32 const t = static_cast<f32>(step) / 40.0f;
        glm::vec3 const p = centre + (boundsMax - centre) * t;
        f32 const w = DDGI::CascadeInteriorWeight(p, grid, kBand);
        EXPECT_LE(w, previous + 1e-5f) << "weight must never rise as the sample leaves the cascade (step " << step << ")";
        if (w > 0.01f && w < 0.99f)
        {
            sawIntermediate = true;
        }
        previous = w;
    }
    EXPECT_TRUE(sawIntermediate) << "the band must actually ramp, not step";

    // Band 0 is the AUTHORED path's setting and must reproduce the hard
    // membership test — boundary inclusive, exactly as ddgiIsInsideVolume.
    EXPECT_FLOAT_EQ(DDGI::CascadeInteriorWeight(boundsMax, grid, 0.0f), 1.0f);
    EXPECT_FLOAT_EQ(DDGI::CascadeInteriorWeight(boundsMin, grid, 0.0f), 1.0f);
    EXPECT_FLOAT_EQ(DDGI::CascadeInteriorWeight(boundsMax + glm::vec3(1e-3f), grid, 0.0f), 0.0f);
}

TEST(DDGIMath, SelectCascadePicksTheFinestOneOwningThePoint)
{
    glm::ivec3 const dims(16, 16, 16);
    std::array<DDGI::CascadeGrid, 4> grids{};
    for (i32 level = 0; level < 4; ++level)
    {
        grids[static_cast<std::size_t>(level)] =
            DDGI::MakeCameraCascade(level, glm::vec3(0.0f), glm::vec3(1.0f), dims);
    }

    // At the camera, cascade 0.
    EXPECT_EQ(DDGI::SelectCascade(glm::vec3(0.0f), grids.data(), 4, 0.2f), 0);

    // Just past cascade 0's window, the next one takes over.
    glm::vec3 const beyond0 = DDGI::CascadeBoundsMax(grids[0]) + glm::vec3(1.0f, 0.0f, 0.0f);
    EXPECT_GE(DDGI::SelectCascade(beyond0, grids.data(), 4, 0.2f), 1);

    // Beyond every cascade, nothing owns the point and the gather must fall
    // through to the ambient ladder rather than clamp to a boundary probe.
    glm::vec3 const beyondAll = DDGI::CascadeBoundsMax(grids[3]) + glm::vec3(1000.0f);
    EXPECT_EQ(DDGI::SelectCascade(beyondAll, grids.data(), 4, 0.2f), -1);
}

// The variable update rate must be an exact partition: every probe once per N
// frames, 1/N of the field per frame. A hash would satisfy neither, and the
// failure (some probes never updating) is invisible in a single frame.
TEST(DDGIMath, UpdateRatePartitionsProbesExactlyOncePerPeriod)
{
    constexpr i32 kProbes = 512;
    for (i32 divisor : { 1, 2, 8, 16, 32, 64 })
    {
        std::vector<i32> updates(static_cast<std::size_t>(kProbes), 0);
        for (u32 frame = 0; frame < static_cast<u32>(divisor); ++frame)
        {
            i32 thisFrame = 0;
            for (i32 probe = 0; probe < kProbes; ++probe)
            {
                if (DDGI::ProbeUpdatesThisFrame(probe, frame, divisor))
                {
                    ++updates[static_cast<std::size_t>(probe)];
                    ++thisFrame;
                }
            }
            EXPECT_EQ(thisFrame, kProbes / divisor)
                << "divisor " << divisor << " frame " << frame << " updated the wrong share of the field";
        }
        for (i32 probe = 0; probe < kProbes; ++probe)
        {
            EXPECT_EQ(updates[static_cast<std::size_t>(probe)], 1)
                << "probe " << probe << " did not update exactly once per " << divisor << " frames";
        }
    }
}

TEST(DDGIMath, UpdateRateSnapsDownToASupportedRate)
{
    EXPECT_EQ(DDGI::SnapUpdateRate(1), DDGI::ProbeUpdateRate::EveryFrame);
    EXPECT_EQ(DDGI::SnapUpdateRate(0), DDGI::ProbeUpdateRate::EveryFrame);
    EXPECT_EQ(DDGI::SnapUpdateRate(-5), DDGI::ProbeUpdateRate::EveryFrame);
    EXPECT_EQ(DDGI::SnapUpdateRate(3), DDGI::ProbeUpdateRate::OneInTwo);
    EXPECT_EQ(DDGI::SnapUpdateRate(8), DDGI::ProbeUpdateRate::OneInEight);
    EXPECT_EQ(DDGI::SnapUpdateRate(15), DDGI::ProbeUpdateRate::OneInEight);
    EXPECT_EQ(DDGI::SnapUpdateRate(1000), DDGI::ProbeUpdateRate::OneInSixtyFour);
    EXPECT_EQ(DDGI::UpdateRateDivisor(DDGI::kDefaultProbeUpdateRate), 8);
}

TEST(DDGIMath, ProbeLivenessExpiresAfterTheRequestLifetime)
{
    constexpr u32 kLifetime = 16u;
    // 0 is the "never requested" sentinel — deliberately not "requested at
    // frame 0", which is why the pass's frame counter starts at 1.
    EXPECT_FALSE(DDGI::IsProbeLive(0u, 1u, kLifetime));
    EXPECT_FALSE(DDGI::IsProbeLive(0u, 100u, kLifetime));

    EXPECT_TRUE(DDGI::IsProbeLive(100u, 100u, kLifetime));
    EXPECT_TRUE(DDGI::IsProbeLive(100u, 116u, kLifetime));
    EXPECT_FALSE(DDGI::IsProbeLive(100u, 117u, kLifetime));
    // A request from "the future" (the pass re-created its resources and the
    // counter restarted) must read live, not as an enormous elapsed time —
    // unsigned wraparound would otherwise park the probe dead forever.
    EXPECT_TRUE(DDGI::IsProbeLive(200u, 100u, kLifetime));
}

// =============================================================================
// Spring-force relocation (issue #707's biggest quality delta over stock DDGI).
// =============================================================================

namespace
{
    // A probe with a wall close on -X and open space on +X: the case stock
    // RTXGI relocation does nothing about (the probe is not inside anything)
    // and PGI's notes call out as the visible "blind spot" producer.
    DDGI::ProbeHitAggregates MakeAgainstWallAggregates()
    {
        DDGI::ProbeHitAggregates agg;
        agg.BackfaceFraction = 0.0f;
        agg.ClosestFrontfaceDir = glm::vec3(-1.0f, 0.0f, 0.0f);
        agg.ClosestFrontfaceDist = 0.05f;
        agg.FarthestFrontfaceDir = glm::vec3(1.0f, 0.0f, 0.0f);
        agg.FarthestFrontfaceDist = 4.0f;
        agg.CrowdingSum = glm::vec3(-1.0f, 0.0f, 0.0f) * 0.9f;
        agg.CrowdingWeight = 0.9f;
        agg.FreeDirectionSum = glm::vec3(1.0f, 0.0f, 0.0f) * 0.8f;
        agg.FreeDirectionWeight = 1.0f;
        return agg;
    }
} // namespace

TEST(DDGIMath, SpringRelocationPushesAProbeOffAWallItIsPressedAgainst)
{
    DDGI::ProbeHitAggregates const agg = MakeAgainstWallAggregates();
    glm::vec3 const spacing(1.0f);
    constexpr f32 kMinFrontface = 0.25f;

    glm::vec3 offset = DDGI::RelocateProbeSpring(glm::vec3(0.0f), agg, spacing, kMinFrontface);
    EXPECT_GT(offset.x, 0.0f) << "the probe must move AWAY from the crowding wall, into the free direction";
    EXPECT_FLOAT_EQ(offset.y, 0.0f);
    EXPECT_FLOAT_EQ(offset.z, 0.0f);

    // Iterating (the pass re-runs the spring on every recapture) must converge
    // inside the cell rather than run away: the offset clamp is what keeps the
    // trilinear gather's "probe i is near lattice point i" assumption true.
    for (i32 iteration = 0; iteration < 64; ++iteration)
    {
        offset = DDGI::RelocateProbeSpring(offset, agg, spacing, kMinFrontface);
        ASSERT_LE(glm::length(offset), DDGI::kMaxProbeOffsetFraction)
            << "iteration " << iteration << " left the 45%-of-cell ellipsoid";
        ASSERT_TRUE(std::isfinite(offset.x) && std::isfinite(offset.y) && std::isfinite(offset.z));
    }
}

TEST(DDGIMath, SpringRelocationDecaysBackToTheLatticePointWhenNothingIsClose)
{
    DDGI::ProbeHitAggregates agg;
    agg.BackfaceFraction = 0.0f;
    agg.ClosestFrontfaceDir = glm::vec3(0.0f, 0.0f, 1.0f);
    agg.ClosestFrontfaceDist = 5.0f; // comfortable

    glm::vec3 offset(0.3f, -0.2f, 0.1f);
    for (i32 iteration = 0; iteration < 64; ++iteration)
    {
        offset = DDGI::RelocateProbeSpring(offset, agg, glm::vec3(1.0f), 0.25f);
    }
    EXPECT_LT(glm::length(offset), 1e-2f)
        << "the grid-restore term must pull an unconstrained probe back to its lattice point, "
           "or the field loses the regular spacing the trilinear gather assumes";
}

TEST(DDGIMath, SpringRelocationKeepsTheInsideGeometryEscape)
{
    // The one case where the single closest face IS unambiguous, and where a
    // pure spring would have to climb out through the crowding term alone.
    DDGI::ProbeHitAggregates agg;
    agg.BackfaceFraction = 0.8f; // > kBackfaceFraction: inside geometry
    agg.ClosestBackfaceDir = glm::vec3(0.0f, 1.0f, 0.0f);
    agg.ClosestBackfaceDist = 0.1f;

    glm::vec3 const offset = DDGI::RelocateProbeSpring(glm::vec3(0.0f), agg, glm::vec3(1.0f), 0.25f);
    EXPECT_GT(offset.y, 0.0f) << "an in-wall probe must be pushed through the closest backface";
    EXPECT_LE(glm::length(offset), DDGI::kMaxProbeOffsetFraction);

    // ...and classification still marks it Inactive so the gather de-weights it
    // while it waits for the recapture from the new position.
    EXPECT_EQ(DDGI::ClassifyProbe(agg), DDGI::ProbeState::Inactive);
}

TEST(DDGIMath, SpringRelocationClampProjectsRatherThanRejects)
{
    // A permanently crowded probe pushes forever; the clamp must place it ON
    // the ellipsoid rather than snap it back to its previous offset. Rejection
    // is right for RTXGI's large discrete steps and wrong for a spring — it
    // leaves the probe pinned with an unsatisfied force, reading as a probe
    // that never converges.
    DDGI::ProbeHitAggregates agg;
    agg.BackfaceFraction = 0.0f;
    agg.CrowdingSum = glm::vec3(-1.0f, 0.0f, 0.0f);
    agg.CrowdingWeight = 1.0f;

    glm::vec3 offset(0.0f);
    for (i32 iteration = 0; iteration < 128; ++iteration)
    {
        offset = DDGI::RelocateProbeSpring(offset, agg, glm::vec3(1.0f), 1.0f);
    }
    EXPECT_NEAR(glm::length(offset), DDGI::kMaxProbeOffsetFraction, 1e-3f)
        << "a permanently crowded probe must settle ON the ellipsoid boundary";
}

// The capture-priority ordering. This is here because getting it wrong is
// INVISIBLE: every tier is still captured eventually, so coverage still reaches
// 100% and every DDGI test still passes — just many frames later. Ordering
// refinement ahead of first capture took the wide-volume parity rig from 12
// frames to full coverage to 56, against a 60-frame budget.
TEST(DDGIMath, CaptureTiersOrderCoverageBeforeRefinementBeforeRefresh)
{
    // Deliberately stacked against the tier order: the never-captured probe is
    // the FURTHEST away and in the COARSEST cascade, which is the case an
    // unclamped `distance + tier * bias` gets wrong.
    const f32 neverCaptured = DDGI::CaptureScore(DDGI::CaptureTier::NeverCaptured, 5000.0f, 7, 0u);
    const f32 refinement = DDGI::CaptureScore(DDGI::CaptureTier::RelocationRefinement, 0.0f, 0, 0u);
    const f32 refresh = DDGI::CaptureScore(DDGI::CaptureTier::PeriodicRefresh, 0.0f, 0, 100000u);

    EXPECT_LT(neverCaptured, refinement)
        << "a probe with no capture at all contributes NOTHING to the gather; it must outrank a probe "
           "that is merely slightly mis-positioned, however far away it is";
    EXPECT_LT(refinement, refresh)
        << "a probe mid-relocation is actively wrong; a stale one is merely old";

    // The separation must hold for an absurd distance too — that is what the
    // intra-tier clamp is for.
    const f32 absurd = DDGI::CaptureScore(DDGI::CaptureTier::NeverCaptured, 1.0e9f, 7, 0u);
    EXPECT_LT(absurd, DDGI::CaptureScore(DDGI::CaptureTier::RelocationRefinement, 0.0f, 0, 0u))
        << "the intra-tier rank must be clamped below the tier bias, or a distant probe crosses tiers";
    EXPECT_TRUE(std::isfinite(absurd));
}

TEST(DDGIMath, CaptureScoreOrdersNearerAndFinerCascadesFirstWithinATier)
{
    for (DDGI::CaptureTier tier : { DDGI::CaptureTier::NeverCaptured, DDGI::CaptureTier::RelocationRefinement })
    {
        EXPECT_LT(DDGI::CaptureScore(tier, 1.0f, 0, 0u), DDGI::CaptureScore(tier, 50.0f, 0, 0u))
            << "nearer probes first";
        EXPECT_LT(DDGI::CaptureScore(tier, 1.0f, 0, 0u), DDGI::CaptureScore(tier, 1.0f, 2, 0u))
            << "finer cascades first — they are what the viewer is standing in";
        // A negative distance cannot happen, but a NaN-free, monotone response
        // to garbage is cheaper to guarantee than to debug.
        EXPECT_TRUE(std::isfinite(DDGI::CaptureScore(tier, -5.0f, -1, 0u)));
    }
}

TEST(DDGIMath, CaptureRefreshTierOrdersOldestFirstAndIgnoresDistance)
{
    const f32 old = DDGI::CaptureScore(DDGI::CaptureTier::PeriodicRefresh, 100.0f, 3, 500u);
    const f32 recent = DDGI::CaptureScore(DDGI::CaptureTier::PeriodicRefresh, 0.0f, 0, 5u);
    EXPECT_LT(old, recent) << "oldest first inside the refresh tier";

    // Distance deliberately does not enter the refresh tier: letting it win
    // would starve the far field of refreshes forever, and a stale far probe is
    // exactly as wrong as a stale near one.
    EXPECT_FLOAT_EQ(DDGI::CaptureScore(DDGI::CaptureTier::PeriodicRefresh, 0.0f, 0, 42u),
                    DDGI::CaptureScore(DDGI::CaptureTier::PeriodicRefresh, 900.0f, 5, 42u));

    // ...and an enormous age must still stay inside the tier rather than
    // wrapping below it.
    const f32 ancient = DDGI::CaptureScore(DDGI::CaptureTier::PeriodicRefresh, 0.0f, 0, 0xFFFFFFFFu);
    EXPECT_GE(ancient, 2.0f * DDGI::kCaptureTierBias);
    EXPECT_LT(ancient, 3.0f * DDGI::kCaptureTierBias);
}

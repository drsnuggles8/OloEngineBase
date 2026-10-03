#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L1
// =============================================================================
// GroomCoatBodyPropertyTests — the body inside the coat volume (#1533).
//
// GroomCoatShadow::MarkBodyInDensityVolume puts the body a coat grows on into
// that coat's density volume, in an array of its own (DensityVolume::Body), so
// the lights no shadow map answers for at the strand -- a light that does not
// cast, the sky, the VSM at the coat's exit point -- are stopped by it. These
// pin what the shader and the shipped casting lights rely on:
//
//   * the coat's arrays are never touched, so a march that does not count the
//     body (every casting light's) is the same to the bit with it there;
//   * the body is the closed parts' box-filtered occupancy, which adds up to
//     their volume, reaching no further out than the floor allows and stopping
//     every ray that dips a quarter of a voxel into them -- the shell the old
//     erosion left is gone;
//   * a part thinner than a voxel keeps its share;
//   * only closed parts are filled, a seam-split mesh welds into one, and an
//     open shell or a corrupt surface leaves the volume untouched and says so.
//
// The fixture is a furred ball: a UV sphere, split at its seam and poles the way
// a glTF splits vertices, under radial strands.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Groom/GroomCoatShadow.h"
#include "OloEngine/Groom/GroomSurfaceFrame.h"

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <bit>
#include <cmath>
#include <limits>
#include <vector>

using namespace OloEngine;
using namespace OloEngine::GroomCoatShadow;

namespace
{
    constexpr f32 kBodyRadius = 0.10f;
    constexpr f32 kHairLength = 0.03f;
    constexpr u32 kResolution = 64;

    struct Mesh
    {
        std::vector<glm::vec3> Positions;
        std::vector<u32> Indices;

        [[nodiscard]] GroomSurfaceView View() const
        {
            GroomSurfaceView view;
            view.PositionData = reinterpret_cast<const std::byte*>(Positions.data());
            view.PositionStride = static_cast<u32>(sizeof(glm::vec3));
            view.VertexCount = static_cast<u32>(Positions.size());
            view.Indices = Indices.data();
            view.IndexCount = static_cast<u32>(Indices.size());
            return view;
        }
    };

    // A UV sphere with its seam column and both poles DUPLICATED per segment,
    // as an exporter splits them for UVs: unwelded, every seam and pole edge is
    // open. `hemisphere` stops at the equator, which leaves a real hole.
    [[nodiscard]] Mesh UvSphere(f32 radius, u32 rings, u32 segments, bool hemisphere = false)
    {
        Mesh mesh;
        const u32 lastRing = hemisphere ? rings / 2u : rings;
        for (u32 r = 0; r <= lastRing; ++r)
        {
            const f32 theta = glm::pi<f32>() * static_cast<f32>(r) / static_cast<f32>(rings);
            for (u32 s = 0; s <= segments; ++s)
            {
                const f32 phi = glm::two_pi<f32>() * static_cast<f32>(s) / static_cast<f32>(segments);
                mesh.Positions.emplace_back(radius * std::sin(theta) * std::cos(phi), radius * std::cos(theta),
                                            radius * std::sin(theta) * std::sin(phi));
            }
        }
        const u32 row = segments + 1u;
        for (u32 r = 0; r < lastRing; ++r)
        {
            for (u32 s = 0; s < segments; ++s)
            {
                const u32 a = (r * row) + s;
                const u32 b = a + 1u;
                const u32 c = a + row;
                const u32 d = c + 1u;
                // Outward-facing, counter-clockwise seen from outside. The pole
                // rows' collapsed triangles are dropped by the weld.
                mesh.Indices.insert(mesh.Indices.end(), { a, c, b, b, c, d });
            }
        }
        return mesh;
    }

    // A closed box, 24 vertices (four per face), so it too needs the weld.
    [[nodiscard]] Mesh SplitBox(const glm::vec3& lo, const glm::vec3& hi)
    {
        Mesh mesh;
        const glm::vec3 c[8] = { { lo.x, lo.y, lo.z }, { hi.x, lo.y, lo.z }, { hi.x, hi.y, lo.z }, { lo.x, hi.y, lo.z }, { lo.x, lo.y, hi.z }, { hi.x, lo.y, hi.z }, { hi.x, hi.y, hi.z }, { lo.x, hi.y, hi.z } };
        const u32 faces[6][4] = { { 0, 3, 2, 1 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 }, { 2, 3, 7, 6 }, { 1, 2, 6, 5 }, { 0, 4, 7, 3 } };
        for (const auto& face : faces)
        {
            const u32 base = static_cast<u32>(mesh.Positions.size());
            for (const u32 corner : face)
            {
                mesh.Positions.push_back(c[corner]);
            }
            mesh.Indices.insert(mesh.Indices.end(), { base, base + 1u, base + 2u, base, base + 2u, base + 3u });
        }
        return mesh;
    }

    // Radial strands over the ball, a Fibonacci spiral of roots.
    [[nodiscard]] std::vector<CoatSegment> RadialCoat(u32 strands)
    {
        std::vector<CoatSegment> segments;
        constexpr u32 kSegmentsPerStrand = 4;
        constexpr f32 kStrandRadius = 0.0004f;
        for (u32 k = 0; k < strands; ++k)
        {
            const f32 up = 1.0f - (2.0f * (static_cast<f32>(k) + 0.5f) / static_cast<f32>(strands));
            const f32 across = std::sqrt(std::max(0.0f, 1.0f - (up * up)));
            const f32 phi = static_cast<f32>(k) * 2.39996323f;
            const glm::vec3 d{ across * std::cos(phi), up, across * std::sin(phi) };
            for (u32 s = 0; s < kSegmentsPerStrand; ++s)
            {
                const f32 t0 = static_cast<f32>(s) / static_cast<f32>(kSegmentsPerStrand);
                const f32 t1 = static_cast<f32>(s + 1u) / static_cast<f32>(kSegmentsPerStrand);
                segments.push_back(CoatSegment{ d * (kBodyRadius + (kHairLength * t0)), d * (kBodyRadius + (kHairLength * t1)),
                                                kStrandRadius, kStrandRadius });
            }
        }
        return segments;
    }

    [[nodiscard]] DensityVolume FurredBallCoat()
    {
        DensityVolumeSettings settings;
        settings.Resolution = kResolution;
        DensityVolume volume;
        EXPECT_TRUE(BuildDensityVolume(RadialCoat(6000), settings, volume));
        return volume;
    }

    [[nodiscard]] bool SameBits(f32 a, f32 b)
    {
        return std::bit_cast<u32>(a) == std::bit_cast<u32>(b);
    }

    [[nodiscard]] bool SameBits(f64 a, f64 b)
    {
        return std::bit_cast<u64>(a) == std::bit_cast<u64>(b);
    }

    [[nodiscard]] bool SameVolume(const DensityVolume& a, const DensityVolume& b)
    {
        if (a.Density.size() != b.Density.size() || a.Body.size() != b.Body.size())
        {
            return false;
        }
        for (sizet i = 0; i < a.Body.size(); ++i)
        {
            if (a.Body[i] != b.Body[i])
            {
                return false;
            }
        }
        for (sizet i = 0; i < a.Density.size(); ++i)
        {
            if (!SameBits(a.Density[i], b.Density[i]) || !SameBits(a.Direction[i].x, b.Direction[i].x) ||
                !SameBits(a.Direction[i].y, b.Direction[i].y) || !SameBits(a.Direction[i].z, b.Direction[i].z))
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] glm::vec3 VoxelCentre(const DensityVolume& volume, i32 x, i32 y, i32 z)
    {
        return volume.BoundsMin + (glm::vec3(static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(z)) + 0.5f) *
                                      volume.VoxelSize();
    }

    [[nodiscard]] sizet Index(const DensityVolume& volume, i32 x, i32 y, i32 z)
    {
        return static_cast<sizet>(x) + (static_cast<sizet>(volume.Dimensions.x) *
                                        (static_cast<sizet>(y) + (static_cast<sizet>(volume.Dimensions.y) * static_cast<sizet>(z))));
    }

    // A voxel's occupancy: the texel's alpha.
    [[nodiscard]] f64 Occupancy(const DensityVolume& volume, sizet index)
    {
        return static_cast<f64>(volume.Body[index].w) / 255.0;
    }

    // A deterministic scatter of rays starting in the coat, in every direction.
    struct Ray
    {
        glm::vec3 Origin;
        glm::vec3 Direction;
    };

    [[nodiscard]] std::vector<Ray> CoatRays(u32 count)
    {
        std::vector<Ray> rays;
        for (u32 k = 0; k < count; ++k)
        {
            const f32 up = 1.0f - (2.0f * (static_cast<f32>(k) + 0.5f) / static_cast<f32>(count));
            const f32 across = std::sqrt(std::max(0.0f, 1.0f - (up * up)));
            const f32 phi = static_cast<f32>(k) * 2.39996323f;
            const glm::vec3 d{ across * std::cos(phi), up, across * std::sin(phi) };
            const glm::vec3 origin = d * (kBodyRadius + (kHairLength * 0.4f));
            // A second spiral for the directions, offset so the pairs do not line up.
            const f32 up2 = 1.0f - (2.0f * std::fmod((static_cast<f32>(k) * 0.618034f) + 0.25f, 1.0f));
            const f32 across2 = std::sqrt(std::max(0.0f, 1.0f - (up2 * up2)));
            const f32 phi2 = static_cast<f32>(k) * 1.3f;
            rays.push_back(Ray{ origin, glm::vec3(across2 * std::cos(phi2), up2, across2 * std::sin(phi2)) });
        }
        return rays;
    }
} // namespace

TEST(GroomCoatBody, TheBodyHasItsOwnArrayAndLeavesTheCoatAlone)
{
    const DensityVolume coat = FurredBallCoat();
    ASSERT_TRUE(coat.IsValid());
    ASSERT_TRUE(coat.Body.empty());
    DensityVolume withBody = coat;
    const Mesh ball = UvSphere(kBodyRadius, 48, 96);
    BodyVoxelStats stats;
    ASSERT_TRUE(MarkBodyInDensityVolume(withBody, ball.View(), glm::mat4(1.0f), BodyVoxelSettings{}, &stats));

    // The seam- and pole-split sphere welded into ONE closed part: unwelded,
    // every seam and pole edge has one face and nothing would be filled.
    EXPECT_EQ(stats.ClosedComponents, 1u);
    EXPECT_EQ(stats.OpenComponents, 0u);
    EXPECT_EQ(stats.OddColumns, 0u) << "a closed sphere left sub-columns whose crossings did not pair up";
    EXPECT_GT(stats.FullVoxels, 0u);
    EXPECT_GE(stats.MarkedVoxels, stats.FullVoxels);
    EXPECT_GE(stats.InsideVoxels, stats.MarkedVoxels);
    ASSERT_TRUE(withBody.IsValid());
    ASSERT_EQ(withBody.Body.size(), coat.Density.size());

    // The coat's arrays to the bit: the body never takes a coat voxel's value.
    for (sizet i = 0; i < coat.Density.size(); ++i)
    {
        ASSERT_TRUE(SameBits(withBody.Density[i], coat.Density[i]) && SameBits(withBody.Direction[i].x, coat.Direction[i].x) &&
                    SameBits(withBody.Direction[i].y, coat.Direction[i].y) &&
                    SameBits(withBody.Direction[i].z, coat.Direction[i].z))
            << "a coat voxel changed: the body has an array of its own";
    }

    // The occupancy is the ball, box-filtered: whole a voxel inside the skin,
    // empty a voxel outside it, and adding up to the ball's volume in voxels.
    const f32 h = coat.VoxelSize().x;
    f64 occupied = 0.0;
    u32 marked = 0;
    for (i32 z = 0; z < coat.Dimensions.z; ++z)
    {
        for (i32 y = 0; y < coat.Dimensions.y; ++y)
        {
            for (i32 x = 0; x < coat.Dimensions.x; ++x)
            {
                const sizet i = Index(coat, x, y, z);
                const f64 o = Occupancy(withBody, i);
                occupied += o;
                marked += o > static_cast<f64>(kBodyOccupancyFloor) ? 1u : 0u;
                const f32 distance = glm::length(VoxelCentre(coat, x, y, z));
                if (distance < kBodyRadius - (h * 0.9f))
                {
                    EXPECT_NEAR(o, 1.0, 1.0e-5) << "a voxel wholly inside the ball is not full";
                }
                else if (distance > kBodyRadius + (h * 0.9f))
                {
                    EXPECT_NEAR(o, 0.0, 1.0e-5) << "a voxel wholly outside the ball holds body";
                }
            }
        }
    }
    EXPECT_EQ(marked, stats.MarkedVoxels);
    // The UV sphere's own volume, a little under the round ball's.
    const f64 ballVoxels = (4.0 / 3.0) * glm::pi<f64>() * std::pow(static_cast<f64>(kBodyRadius / h), 3.0);
    std::printf("[coat-body] the ball's occupancy adds up to %.1f voxels of the round ball's %.1f\n", occupied, ballVoxels);
    EXPECT_NEAR(occupied / ballVoxels, 1.0, 0.01) << "the box filter lost or invented body";
}

// THE CASTING LIGHTS' MARCH IS UNCHANGED, to the bit: it never reads the body.
TEST(GroomCoatBody, AMarchThatDoesNotCountTheBodyIsBitIdentical)
{
    const DensityVolume coat = FurredBallCoat();
    DensityVolume withBody = coat;
    const Mesh ball = UvSphere(kBodyRadius, 48, 96);
    ASSERT_TRUE(MarkBodyInDensityVolume(withBody, ball.View(), glm::mat4(1.0f), BodyVoxelSettings{}));

    u32 throughBody = 0;
    for (const Ray& ray : CoatRays(512))
    {
        for (const bool anisotropic : { false, true })
        {
            const f64 before = SampleDensityVolume(coat, ray.Origin, ray.Direction, anisotropic, 1.5f);
            const f64 after = SampleDensityVolume(withBody, ray.Origin, ray.Direction, anisotropic, 1.5f);
            ASSERT_TRUE(SameBits(before, after)) << "the coat-only march changed with the body in the volume: "
                                                 << before << " -> " << after;
        }
        f64 bodyTau = 0.0;
        (void)SampleDensityVolume(withBody, ray.Origin, ray.Direction, true, 1.5f, nullptr, &bodyTau);
        throughBody += bodyTau > 1.0 ? 1u : 0u;
    }
    // Not vacuous: a good share of those rays did cross the body.
    EXPECT_GT(throughBody, 100u);
}

TEST(GroomCoatBody, ARayIntoTheBodyIsStoppedAndOneOutOfItIsNot)
{
    DensityVolume volume = FurredBallCoat();
    const Mesh ball = UvSphere(kBodyRadius, 48, 96);
    ASSERT_TRUE(MarkBodyInDensityVolume(volume, ball.View(), glm::mat4(1.0f), BodyVoxelSettings{}));
    const f32 h = volume.VoxelSize().x;

    const glm::vec3 inCoat{ kBodyRadius + 0.005f, 0.0f, 0.0f };
    f64 inward = 0.0;
    (void)SampleDensityVolume(volume, inCoat, glm::vec3(-1.0f, 0.0f, 0.0f), true, 1.5f, nullptr, &inward);
    EXPECT_GT(inward, kBodyOpaqueTau) << "a ray straight through the ball was not stopped by it";
    EXPECT_LT(BodyTransmittance(inward), 1.0e-5);

    f64 outward = 0.0;
    (void)SampleDensityVolume(volume, inCoat, glm::vec3(1.0f, 0.0f, 0.0f), true, 1.5f, nullptr, &outward);
    EXPECT_FALSE(outward > 0.0) << "a ray leaving the skin found body in front of it";

    // Along the skin a voxel out -- a strand lit along its own tangent: the
    // floor cuts the body off about 0.65 voxels from the skin, so no body.
    f64 grazing = 0.0;
    (void)SampleDensityVolume(volume, glm::vec3(kBodyRadius + h, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), true, 1.5f,
                              nullptr, &grazing);
    EXPECT_FALSE(grazing > 0.0) << "a ray along the skin a voxel out was stopped by the body";
}

// THE VOLUME NEVER SHADOWS PAST THE FLOOR'S REACH AND MISSES NO RAY THAT DIPS
// INTO THE BODY (#1533). Against the ball's exact geometry, from four thousand
// points in the coat in every direction, at the shipped step and at one voxel.
// Both bounds are the representation's, derived rather than fitted:
//   * outward: the filtered occupancy of a flat skin falls to the floor (0.1)
//     0.66 voxels out, and a convex one sooner, so a ray whose closest approach
//     is a voxel outside the ball crosses NO body at all -- not just too little
//     to count;
//   * inward: past the skin the filtered occupancy is at least about 0.5, an
//     extinction of at least 0.44 * kBodyOpacityPerVoxel a voxel, so a ray that dips
//     a quarter of a voxel into a ball 24 voxels across runs a chord of about 7
//     voxels through it -- an optical depth of nearly 20, stopped many times
//     over.
// Between the two is the band a box filter at this voxel size cannot resolve:
// rays that pass within a voxel of the skin may be stopped, and are counted.
// The old erosion put the whole band inside the skin and left a shell four
// voxels deep that let every grazing chord through
// (DogShowcaseEvidenceTest.TheBodyInTheCoatVolumeIsSlicedForLooking).
TEST(GroomCoatBody, TheVolumeShadowsNoFurtherThanTheFloorAndMissesNoRayIntoTheBody)
{
    DensityVolume volume = FurredBallCoat();
    const Mesh ball = UvSphere(kBodyRadius, 48, 96);
    ASSERT_TRUE(MarkBodyInDensityVolume(volume, ball.View(), glm::mat4(1.0f), BodyVoxelSettings{}));
    const f32 voxel = volume.VoxelSize().x;

    for (const f32 stepScale : { 1.5f, 1.0f })
    {
        SCOPED_TRACE(stepScale);
        u32 crossing = 0;
        u32 stopped = 0;
        u32 bodyPastReach = 0;
        u32 inBand = 0;
        u32 bandStopped = 0;
        u32 deepMisses = 0;
        f32 deepestMiss = 0.0f;
        for (const Ray& ray : CoatRays(4000))
        {
            // How deep the ray reaches into the ball: the radius less its closest
            // approach to the centre, where that lies ahead of the origin.
            const f32 along = std::max(0.0f, -glm::dot(ray.Origin, ray.Direction));
            const f32 closest = glm::length(ray.Origin + ray.Direction * along);
            const f32 reach = kBodyRadius - closest;
            f64 bodyTau = 0.0;
            (void)SampleDensityVolume(volume, ray.Origin, ray.Direction, true, stepScale, nullptr, &bodyTau);
            const bool volumeStops = bodyTau > 3.0;
            crossing += reach > 0.0f ? 1u : 0u;
            stopped += volumeStops ? 1u : 0u;
            if (reach < -voxel)
            {
                bodyPastReach += bodyTau > 0.0 ? 1u : 0u;
            }
            else if (reach < 0.25f * voxel)
            {
                ++inBand;
                bandStopped += volumeStops ? 1u : 0u;
            }
            else if (!volumeStops)
            {
                ++deepMisses;
                deepestMiss = std::max(deepestMiss, reach);
            }
        }
        std::printf("[coat-body] step %.1f: %u of 4000 coat rays cross the ball, the volume stops %u; %u pass within a "
                    "voxel of the skin or a quarter into it (%u stopped); %u reach deeper and are missed\n",
                    static_cast<f64>(stepScale), crossing, stopped, inBand, bandStopped, deepMisses);
        ASSERT_GT(crossing, 1000u) << "too few rays cross the ball to say anything";
        EXPECT_EQ(bodyPastReach, 0u) << "rays passing a voxel or more outside the ball crossed body";
        EXPECT_EQ(deepMisses, 0u) << "rays reaching a quarter voxel or more into the ball were let through; the deepest "
                                     "reached "
                                  << deepestMiss / voxel << " voxels";
    }
}

// THE SKY THE BODY LEAVES (#1533), against the ball's geometry. From a point
// at height h in the coat, the ball fills a cone of half-angle a around the
// way in, sin a = R / (R + h): it shuts (1 - cos a) / 2 of the sphere of
// directions, and a cosine lobe pointed straight at it loses sin^2 a. So at
// h = 2 cm over the 10 cm ball: open share 0.777, a lobe pointed out sees all
// of the sky, one pointed in 0.31 of it, and the open direction is straight
// out. BodySkyVisibility's cap reading gives the lobe pointed in v - 2v(1 - v)
// = 0.43 there, which is what the bound below allows for; the bake's 32 rays
// and its cells, 1.7 cm apart, move the open share by a few hundredths.
TEST(GroomCoatBody, TheSkyTheBodyLeavesIsTheBallsShare)
{
    DensityVolume volume = FurredBallCoat();
    const Mesh ball = UvSphere(kBodyRadius, 48, 96);
    BodyVoxelStats stats;
    ASSERT_TRUE(MarkBodyInDensityVolume(volume, ball.View(), glm::mat4(1.0f), BodyVoxelSettings{}, &stats));
    EXPECT_GT(stats.SkyCellsOutside, 0u);
    EXPECT_LT(stats.SkyCellsOutside, stats.SkyCells) << "no sky cell lies inside the ball";

    constexpr f32 kHeight = 0.02f;
    const f32 sinA = kBodyRadius / (kBodyRadius + kHeight);
    const f32 cosA = std::sqrt(1.0f - (sinA * sinA));
    const f32 expectedOpen = 0.5f * (1.0f + cosA);
    f32 worstOpen = 0.0f;
    f32 worstAlign = 1.0f;
    for (const Ray& ray : CoatRays(64))
    {
        const glm::vec3 out = glm::normalize(ray.Origin);
        const glm::vec4 texel = SampleBody(volume, out * (kBodyRadius + kHeight));
        const glm::vec3 skyVector(texel);
        const f32 open = glm::length(skyVector);
        worstOpen = std::max(worstOpen, std::abs(open - expectedOpen));
        worstAlign = std::min(worstAlign, glm::dot(skyVector / std::max(open, 1.0e-6f), out));
        EXPECT_FLOAT_EQ(texel.w, 0.0f) << "a point 2 cm out holds body";
        EXPECT_GT(BodySkyVisibility(skyVector, out), 0.95f) << "a lobe pointed away from the ball lost sky to it";
        const f32 inward = BodySkyVisibility(skyVector, -out);
        EXPECT_GT(inward, 0.15f);
        EXPECT_LT(inward, 0.55f) << "a lobe pointed at the ball kept most of the sky";
    }
    std::printf("[coat-body] the sky 2 cm out: open share within %.3f of the ball's %.3f, open direction within %.1f "
                "degrees of straight out\n",
                static_cast<f64>(worstOpen), static_cast<f64>(expectedOpen),
                static_cast<f64>(glm::degrees(std::acos(std::clamp(worstAlign, -1.0f, 1.0f)))));
    EXPECT_LT(worstOpen, 0.08f) << "the open share is not the ball's";
    EXPECT_GT(worstAlign, 0.95f) << "the open direction does not point away from the ball";

    // Deep inside the ball nothing is open; far outside it, nearly all is.
    EXPECT_LT(glm::length(glm::vec3(SampleBody(volume, glm::vec3(0.0f)))), 0.05f);
    EXPECT_GT(glm::length(glm::vec3(SampleBody(volume, glm::vec3(0.0f, kBodyRadius + 0.028f, 0.0f)))), 0.7f);
}

TEST(GroomCoatBody, AnOpenShellIsLeftOutAndSaysSo)
{
    const DensityVolume coat = FurredBallCoat();
    DensityVolume volume = coat;
    const Mesh bowl = UvSphere(kBodyRadius, 48, 96, /*hemisphere*/ true);
    BodyVoxelStats stats;
    EXPECT_FALSE(MarkBodyInDensityVolume(volume, bowl.View(), glm::mat4(1.0f), BodyVoxelSettings{}, &stats));
    EXPECT_EQ(stats.OpenComponents, 1u);
    EXPECT_EQ(stats.ClosedComponents, 0u);
    EXPECT_TRUE(SameVolume(volume, coat)) << "a surface that was refused still changed the volume";
}

// A PART THINNER THAN A VOXEL KEEPS ITS SHARE. The erosion this replaced took
// every part under five voxels -- the dog's ears and tail tip -- out of the
// volume. A slab a voxel thick adds up to its volume and stops a ray across it;
// half a voxel adds up to its volume too and stops a share of the light.
// Derived: a voxel-thick slab centred on a voxel row filters to a tent of peak
// 1, an optical depth of kBodyOpacityPerVoxel * 0.9^2 / 0.9 = 5.4 across it; half a
// voxel peaks at 0.5, (0.4 / 0.5)^2 * 0.5 * 6 / 0.9 = 2.1, and less where it
// straddles two rows. The march's samples move the sum either way.
TEST(GroomCoatBody, APartThinnerThanAVoxelKeepsItsShare)
{
    const DensityVolume coat = FurredBallCoat();
    const f32 h = coat.VoxelSize().x;
    // A slab in the ball's coat-free core, centred on the voxel row nearest
    // z = 0 so the derivation above holds as stated.
    const f32 row = coat.BoundsMin.z + ((std::floor((0.0f - coat.BoundsMin.z) / h) + 0.5f) * h);
    for (const f32 thickness : { 1.0f, 0.5f })
    {
        SCOPED_TRACE(thickness);
        DensityVolume volume = coat;
        const Mesh slab = SplitBox(glm::vec3(-0.03f, -0.03f, row - (0.5f * thickness * h)),
                                   glm::vec3(0.03f, 0.03f, row + (0.5f * thickness * h)));
        BodyVoxelStats stats;
        ASSERT_TRUE(MarkBodyInDensityVolume(volume, slab.View(), glm::mat4(1.0f), BodyVoxelSettings{}, &stats));
        EXPECT_EQ(stats.ClosedComponents, 1u) << "the 24-vertex box did not weld into one closed part";
        f64 occupied = 0.0;
        for (sizet i = 0; i < volume.Body.size(); ++i)
        {
            occupied += Occupancy(volume, i);
        }
        // Exact through its thickness; across, the sub-columns sample its four
        // edges a quarter voxel apart, up to 2 * 0.25 / 14 of each side.
        const f64 slabVoxels = (0.06 / h) * (0.06 / h) * thickness;
        EXPECT_NEAR(occupied / slabVoxels, 1.0, 0.04) << "the slab's occupancy is not its volume";

        f64 across = 0.0;
        (void)SampleDensityVolume(volume, glm::vec3(0.003f, 0.002f, row - 0.04f), glm::vec3(0.0f, 0.0f, 1.0f), true, 1.5f,
                                  nullptr, &across);
        f64 beside = 0.0;
        (void)SampleDensityVolume(volume, glm::vec3(-0.05f, 0.002f, row + (2.0f * h)), glm::vec3(1.0f, 0.0f, 0.0f), true,
                                  1.5f, nullptr, &beside);
        std::printf("[coat-body] a slab %.1f voxels thick: optical depth %.2f across it\n", static_cast<f64>(thickness), across);
        EXPECT_FALSE(beside > 0.0) << "a ray two voxels above the slab crossed body";
        if (thickness >= 1.0f)
        {
            EXPECT_GT(across, 3.0) << "a slab a voxel thick did not stop a ray across it";
        }
        else
        {
            EXPECT_GT(across, 1.0) << "a slab half a voxel thick stopped almost none of the light";
        }
    }
}

TEST(GroomCoatBody, ACorruptSurfaceIsRefusedWithTheVolumeUntouched)
{
    const DensityVolume coat = FurredBallCoat();
    const Mesh ball = UvSphere(kBodyRadius, 24, 48);

    DensityVolume volume = coat;
    glm::mat4 poisoned(1.0f);
    poisoned[3][0] = std::numeric_limits<f32>::quiet_NaN();
    EXPECT_FALSE(MarkBodyInDensityVolume(volume, ball.View(), poisoned, BodyVoxelSettings{}));
    EXPECT_TRUE(SameVolume(volume, coat));

    GroomSurfaceView empty;
    EXPECT_FALSE(MarkBodyInDensityVolume(volume, empty, glm::mat4(1.0f), BodyVoxelSettings{}));
    EXPECT_TRUE(SameVolume(volume, coat));

    for (const f32 tolerance : { -1.0f, 0.0f, std::numeric_limits<f32>::quiet_NaN() })
    {
        BodyVoxelSettings bad;
        bad.WeldTolerance = tolerance;
        EXPECT_FALSE(MarkBodyInDensityVolume(volume, ball.View(), glm::mat4(1.0f), bad));
        EXPECT_TRUE(SameVolume(volume, coat));
    }
}

// The body is placed through the transform it is given, as a bound body is
// through SurfaceToGroom: a small ball moved off centre, inside the coat-free
// core, stops the ray from the centre that points at it and not the one that
// points away. Placed without the transform it would sit on the centre and stop
// both.
TEST(GroomCoatBody, TheSurfaceIsTakenToTheVolumeByItsTransform)
{
    DensityVolume volume = FurredBallCoat();
    const Mesh pebble = UvSphere(0.03f, 32, 64);
    const glm::mat4 moved = glm::translate(glm::mat4(1.0f), glm::vec3(0.04f, 0.0f, 0.0f));
    ASSERT_TRUE(MarkBodyInDensityVolume(volume, pebble.View(), moved, BodyVoxelSettings{}));

    f64 towards = 0.0;
    (void)SampleDensityVolume(volume, glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), true, 1.5f, nullptr, &towards);
    EXPECT_GT(towards, kBodyOpaqueTau) << "the ray at the moved ball was not stopped by it";
    f64 away = 0.0;
    (void)SampleDensityVolume(volume, glm::vec3(0.0f), glm::vec3(-1.0f, 0.0f, 0.0f), true, 1.5f, nullptr, &away);
    EXPECT_FALSE(away > 0.0) << "the ball was not moved: the ray away from it still found it";
}

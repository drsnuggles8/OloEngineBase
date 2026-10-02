#include "OloEnginePCH.h"

// OLO_TEST_LAYER: L1
// =============================================================================
// GroomCoatBodyPropertyTests — the body inside the coat volume (#1533).
//
// GroomCoatShadow::MarkBodyInDensityVolume puts the body a coat grows on into
// that coat's density volume, as negative density, so the lights no shadow map
// answers for at the strand -- a light that does not cast, the sky, the VSM at
// the coat's exit point -- are stopped by it. These pin what the shader and the
// shipped casting lights rely on:
//
//   * every coat voxel keeps its value, and no body voxel sits beside one, so a
//     march that skips negative samples (every casting light's) is the same to
//     the bit with the body in the volume;
//   * a ray into the body is stopped and a ray out of it, or along the skin, is
//     not;
//   * only closed parts are filled, a seam-split mesh welds into one, and an
//     open shell, a part too thin to survive the erosion or a corrupt surface
//     leaves the volume untouched and says so.
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
        if (a.Density.size() != b.Density.size())
        {
            return false;
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

TEST(GroomCoatBody, TheClosedBodyIsMarkedWhereTheCoatIsNot)
{
    const DensityVolume coat = FurredBallCoat();
    ASSERT_TRUE(coat.IsValid());
    DensityVolume withBody = coat;
    const Mesh ball = UvSphere(kBodyRadius, 48, 96);
    BodyVoxelStats stats;
    ASSERT_TRUE(MarkBodyInDensityVolume(withBody, ball.View(), glm::mat4(1.0f), BodyVoxelSettings{}, &stats));

    // The seam- and pole-split sphere welded into ONE closed part: unwelded,
    // every seam and pole edge has one face and nothing would be filled.
    EXPECT_EQ(stats.ClosedComponents, 1u);
    EXPECT_EQ(stats.OpenComponents, 0u);
    EXPECT_EQ(stats.OddColumns, 0u) << "a closed sphere left columns whose crossings did not pair up";
    EXPECT_GT(stats.MarkedVoxels, 0u);

    const f32 h = coat.VoxelSize().x;
    u32 marked = 0;
    for (i32 z = 0; z < coat.Dimensions.z; ++z)
    {
        for (i32 y = 0; y < coat.Dimensions.y; ++y)
        {
            for (i32 x = 0; x < coat.Dimensions.x; ++x)
            {
                const sizet i = Index(coat, x, y, z);
                if (coat.Density[i] > 0.0f)
                {
                    ASSERT_TRUE(SameBits(withBody.Density[i], coat.Density[i]) &&
                                SameBits(withBody.Direction[i].x, coat.Direction[i].x) &&
                                SameBits(withBody.Direction[i].y, coat.Direction[i].y) &&
                                SameBits(withBody.Direction[i].z, coat.Direction[i].z))
                        << "a coat voxel changed: the body must only ever take empty voxels";
                    continue;
                }
                if (!(withBody.Density[i] < 0.0f))
                {
                    continue;
                }
                ++marked;
                // Inside the ball, and eroded from its skin.
                const f32 distance = glm::length(VoxelCentre(coat, x, y, z));
                EXPECT_LT(distance, kBodyRadius - (1.5f * h)) << "a body voxel at the skin: the erosion did not hold";
            }
        }
    }
    EXPECT_EQ(marked, stats.MarkedVoxels);
    // Most of the eroded core: (R - 2h)^3 of the ball's voxels, give or take
    // the coat's roots that reach in and the voxelisation of a sphere.
    const f64 core = (4.0 / 3.0) * glm::pi<f64>() * std::pow((kBodyRadius - (2.0 * h)) / h, 3.0);
    EXPECT_GT(static_cast<f64>(marked), core * 0.7);
    EXPECT_LT(static_cast<f64>(marked), core * 1.1);
}

TEST(GroomCoatBody, NoBodyVoxelHasACoatVoxelBesideIt)
{
    DensityVolume volume = FurredBallCoat();
    const Mesh ball = UvSphere(kBodyRadius, 48, 96);
    ASSERT_TRUE(MarkBodyInDensityVolume(volume, ball.View(), glm::mat4(1.0f), BodyVoxelSettings{}));
    const glm::ivec3 dims = volume.Dimensions;
    for (i32 z = 0; z < dims.z; ++z)
    {
        for (i32 y = 0; y < dims.y; ++y)
        {
            for (i32 x = 0; x < dims.x; ++x)
            {
                if (!(volume.Density[Index(volume, x, y, z)] < 0.0f))
                {
                    continue;
                }
                for (i32 dz = -1; dz <= 1; ++dz)
                {
                    for (i32 dy = -1; dy <= 1; ++dy)
                    {
                        for (i32 dx = -1; dx <= 1; ++dx)
                        {
                            const i32 nx = x + dx;
                            const i32 ny = y + dy;
                            const i32 nz = z + dz;
                            if (nx < 0 || ny < 0 || nz < 0 || nx >= dims.x || ny >= dims.y || nz >= dims.z)
                            {
                                continue;
                            }
                            ASSERT_FALSE(volume.Density[Index(volume, nx, ny, nz)] > 0.0f)
                                << "a body voxel beside a coat voxel: trilinear filtering would mix them";
                        }
                    }
                }
            }
        }
    }
}

// THE CASTING LIGHTS' MARCH IS UNCHANGED, to the bit: it skips a negative
// sample exactly as an empty one, and no filtered sample mixes body and coat.
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

    const glm::vec3 inCoat{ kBodyRadius + 0.005f, 0.0f, 0.0f };
    f64 inward = 0.0;
    (void)SampleDensityVolume(volume, inCoat, glm::vec3(-1.0f, 0.0f, 0.0f), true, 1.5f, nullptr, &inward);
    EXPECT_GT(inward, kBodyOpaqueTau) << "a ray straight through the ball was not stopped by it";
    EXPECT_LT(BodyTransmittance(inward), 1.0e-5);

    f64 outward = 0.0;
    (void)SampleDensityVolume(volume, inCoat, glm::vec3(1.0f, 0.0f, 0.0f), true, 1.5f, nullptr, &outward);
    EXPECT_FALSE(outward > 0.0) << "a ray leaving the skin found body in front of it";

    // Along the skin, a millimetre out: the body sits ErodeVoxels inside it,
    // where the coat's roots already stop a grazing ray.
    f64 grazing = 0.0;
    (void)SampleDensityVolume(volume, glm::vec3(kBodyRadius + 0.001f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), true,
                              1.5f, nullptr, &grazing);
    EXPECT_FALSE(grazing > 0.0) << "a ray along the skin was stopped by the body";
}

// THE VOLUME NEVER SHADOWS WHERE THE BODY DOES NOT, AND MISSES ONLY ITS SHELL
// (#1533). Against the ball's exact geometry, from four thousand points in the
// coat in every direction: a ray the volume stops must cross the ball, and a
// ray that reaches deeper into the ball than the volume's inset must be
// stopped. The inset is the representation's, derived rather than fitted: the
// body is marked ErodeVoxels (2) inside the skin, cleared within one voxel of a
// coat voxel, and a root's voxel reaches up to one voxel inside the skin -- four
// voxels -- and the march may step one voxel over what is left. A chord that
// only grazes that outer shell crosses no body voxel and is not stopped, which
// is what the dog's rim measured too: its legs, face and torso lost the rays
// that skim them, and its ears and tail tip, thinner than the inset, all of
// theirs (DogShowcaseEvidenceTest.TheBodyInTheCoatVolumeIsSlicedForLooking).
TEST(GroomCoatBody, TheVolumeNeverShadowsWhereTheBodyDoesNotAndMissesOnlyItsShell)
{
    DensityVolume volume = FurredBallCoat();
    const Mesh ball = UvSphere(kBodyRadius, 48, 96);
    const BodyVoxelSettings settings;
    ASSERT_TRUE(MarkBodyInDensityVolume(volume, ball.View(), glm::mat4(1.0f), settings));
    const f32 voxel = volume.VoxelSize().x;
    const f32 inset = (static_cast<f32>(settings.ErodeVoxels) + 3.0f) * voxel;

    u32 crossing = 0;
    u32 stopped = 0;
    u32 falseShadow = 0;
    u32 deepMisses = 0;
    f32 deepestMiss = 0.0f;
    for (const Ray& ray : CoatRays(4000))
    {
        // How deep the ray reaches into the ball: the radius less its closest
        // approach to the centre, where that lies ahead of the origin.
        const f32 along = std::max(0.0f, -glm::dot(ray.Origin, ray.Direction));
        const f32 closest = glm::length(ray.Origin + ray.Direction * along);
        const f32 reach = kBodyRadius - closest;
        const bool crossesBall = reach > 0.0f;
        f64 bodyTau = 0.0;
        (void)SampleDensityVolume(volume, ray.Origin, ray.Direction, true, 1.0f, nullptr, &bodyTau);
        const bool volumeStops = bodyTau > 3.0;
        crossing += crossesBall ? 1u : 0u;
        stopped += volumeStops ? 1u : 0u;
        falseShadow += (volumeStops && !crossesBall) ? 1u : 0u;
        if (crossesBall && !volumeStops)
        {
            deepestMiss = std::max(deepestMiss, reach);
            deepMisses += reach > inset ? 1u : 0u;
        }
    }
    std::printf("[coat-body] %u of 4000 coat rays cross the ball, the volume stops %u; the deepest it missed reached %.2f "
                "voxels in (inset %.1f)\n",
                crossing, stopped, static_cast<f64>(deepestMiss / voxel), static_cast<f64>(inset / voxel));
    ASSERT_GT(crossing, 1000u) << "too few rays cross the ball to say anything";
    EXPECT_EQ(falseShadow, 0u) << "the volume stopped rays that never reach the body";
    EXPECT_EQ(deepMisses, 0u) << "the volume let through rays that reach past its inset into the body; the deepest reached "
                              << deepestMiss / voxel << " voxels";
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

TEST(GroomCoatBody, APartTooThinForTheErosionMarksNothing)
{
    const DensityVolume coat = FurredBallCoat();
    DensityVolume volume = coat;
    // Three voxels thick, well inside the ball's coat-free core: the closed box
    // is found and filled, and two voxels of erosion leave nothing of it.
    const f32 h = coat.VoxelSize().x;
    const Mesh slab = SplitBox(glm::vec3(-0.03f, -0.03f, -1.5f * h), glm::vec3(0.03f, 0.03f, 1.5f * h));
    BodyVoxelStats stats;
    EXPECT_FALSE(MarkBodyInDensityVolume(volume, slab.View(), glm::mat4(1.0f), BodyVoxelSettings{}, &stats));
    EXPECT_EQ(stats.ClosedComponents, 1u) << "the 24-vertex box did not weld into one closed part";
    EXPECT_GT(stats.InsideVoxels, 0u);
    EXPECT_EQ(stats.MarkedVoxels, 0u);
    EXPECT_TRUE(SameVolume(volume, coat));

    // The same box, thick, is kept: the erosion is what removed the thin one.
    DensityVolume thick = coat;
    const Mesh block = SplitBox(glm::vec3(-0.03f), glm::vec3(0.03f));
    EXPECT_TRUE(MarkBodyInDensityVolume(thick, block.View(), glm::mat4(1.0f), BodyVoxelSettings{}, &stats));
    EXPECT_GT(stats.MarkedVoxels, 0u);
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

    BodyVoxelSettings bad;
    bad.OpacityPerVoxel = -1.0f;
    EXPECT_FALSE(MarkBodyInDensityVolume(volume, ball.View(), glm::mat4(1.0f), bad));
    EXPECT_TRUE(SameVolume(volume, coat));
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

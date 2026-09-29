#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// GroomCoatCurlTest — coat authoring v4 (#1533): curl, wave and the
// root-to-tip tint.
//
// Curl and wave are baked into a strand's REST points at strand build — after
// the length and clump shape, before any root transform — by ApplyGroomCoatCurl,
// inside the one walk both builds share. So the CPU-deformed stream, the GPU
// rest stream (#1427), the coat bake and the ray-tracing proxy all carry the
// same curled points, and none of them can disagree about where a curl is.
//
// Every case here MEASURES what the build emitted rather than re-deriving it
// through the function under test:
//
//   * a straight strand curls into a helix whose radius and pitch are the
//     strand's own draws, inside the documented jitter of the authored values;
//   * a wave stays in one plane, across the strand, and reaches its amplitude;
//   * the root is an exact fixed point, bit for bit;
//   * a group that authors none of it emits exactly the stream the v3 build
//     wrote — beside a group that does, so a scratch buffer that leaked one
//     strand's curl into the next would show;
//   * a card carries its members' MEAN curl, attenuated by the JitterScale;
//   * the tint runs from Tint at the root to Tint * TipTint at the tip, per corner,
//     and an unauthored tip packs every corner exactly as v3 packed the strand.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomCoat.h"
#include "OloEngine/Groom/GroomStrandMesh.h"
#include "OloEngine/Groom/GroomVisibility.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <numbers>
#include <span>
#include <string>
#include <vector>

using namespace OloEngine;

namespace
{
    // One group of a test groom: its authored description and its strands.
    struct StraightGroup
    {
        const char* Name = "flank";
        GroomCoatGroupDesc Desc{};
        u32 Strands = 1;
        u32 Points = 200;
        f32 Length = 0.5f;
        glm::vec3 Direction{ 0.0f, 1.0f, 0.0f };
    };

    // Straight strands, rooted along x, each growing along its group's
    // direction. STRAIGHT because the helix a curl draws around a straight axis
    // has a radius and a pitch that can be read off the emitted points; around a
    // bent axis both are the output of the frame transport under test.
    //
    // The groups' strands are INTERLEAVED in curve order, so consecutive curves
    // alternate between groups — which is what exposes a per-strand scratch
    // buffer that carries one strand's shape into the next.
    [[nodiscard]] Ref<GroomAsset> MakeStraightGroom(std::span<const StraightGroup> groups)
    {
        GroomBuilder builder;
        std::string reason;
        std::vector<u16> ids(groups.size(), 0);
        u32 mostStrands = 0;
        for (sizet g = 0; g < groups.size(); ++g)
        {
            EXPECT_TRUE(builder.AddGroup(groups[g].Name, ids[g], reason)) << reason;
            std::vector<std::string> repairs;
            EXPECT_TRUE(builder.SetGroupCoat(ids[g], groups[g].Desc, repairs));
            EXPECT_TRUE(repairs.empty()) << "the fixture authors only legal values: " << repairs.front();
            mostStrands = std::max(mostStrands, groups[g].Strands);
        }

        std::vector<glm::vec3> points;
        std::vector<f32> widths;
        u32 strand = 0;
        for (u32 s = 0; s < mostStrands; ++s)
        {
            for (sizet g = 0; g < groups.size(); ++g)
            {
                const StraightGroup& group = groups[g];
                if (s >= group.Strands)
                {
                    continue;
                }
                const glm::vec3 root(0.05f * static_cast<f32>(strand), 0.0f, 0.0f);
                const glm::vec3 direction = glm::normalize(group.Direction);
                points.clear();
                widths.clear();
                for (u32 p = 0; p < group.Points; ++p)
                {
                    const f32 along = static_cast<f32>(p) / static_cast<f32>(group.Points - 1u);
                    points.push_back(root + (direction * (group.Length * along)));
                    widths.push_back(1.0e-4f * (1.0f - (0.5f * along)));
                }
                GroomCurveInput input;
                input.Points = points;
                input.Widths = widths;
                // Distinct cells, so no clump could ever join two strands.
                input.RootUV = { 0.1f + (0.05f * static_cast<f32>(strand % 16u)), 0.1f + (0.05f * static_cast<f32>(strand / 16u)) };
                input.GroupId = ids[g];
                EXPECT_TRUE(builder.AddCurve(input, reason)) << reason;
                ++strand;
            }
        }
        Ref<GroomAsset> groom = builder.Build(reason);
        EXPECT_TRUE(groom) << reason;
        return groom;
    }

    [[nodiscard]] GroomCoatSettings Active()
    {
        GroomCoatSettings settings;
        settings.Enabled = true;
        settings.Seed = 1533u;
        return settings;
    }

    // A flat RGB map: one colour everywhere.
    [[nodiscard]] Ref<GroomRegionMap> FlatMap(u8 r, u8 g, u8 b)
    {
        std::vector<u8> pixels(4u * 4u * 4u);
        for (sizet i = 0; i < pixels.size(); i += 4)
        {
            pixels[i + 0] = r;
            pixels[i + 1] = g;
            pixels[i + 2] = b;
            pixels[i + 3] = 255;
        }
        return GroomRegionMap::FromRGBA8(4u, 4u, pixels);
    }

    // The whole groom at full budget, so the stream is every curve in curve
    // order, four corners per segment.
    [[nodiscard]] std::vector<GroomStrandVertex> Build(const GroomAsset& groom, const GroomCoatContext& coat,
                                                       GroomStrandMeshStats* outStats = nullptr)
    {
        GroomStrandBuildSettings build;
        build.MaxStrands = groom.GetCurveCount();
        build.CoatDigest = GroomCoatDigest(*coat.Settings);
        std::vector<GroomStrandVertex> vertices;
        std::vector<u32> indices;
        const GroomStrandMeshStats stats = BuildGroomStrandMesh(groom, build, vertices, indices, nullptr, &coat);
        EXPECT_EQ(stats.StrandsSelected, groom.GetCurveCount()) << "the budget must keep every curve";
        if (outStats != nullptr)
        {
            *outStats = stats;
        }
        return vertices;
    }

    // The points a build emitted for each curve, in curve order: segment i's P0
    // corner is point i, and the last segment's P1 corner is the tip.
    [[nodiscard]] std::vector<std::vector<glm::vec3>> EmittedPoints(const GroomAsset& groom,
                                                                    const std::vector<GroomStrandVertex>& vertices)
    {
        std::vector<std::vector<glm::vec3>> out;
        sizet corner = 0;
        for (u32 curve = 0; curve < groom.GetCurveCount(); ++curve)
        {
            const u32 count = groom.GetCurvePointCount(curve);
            std::vector<glm::vec3> points;
            for (u32 i = 0; i + 1u < count; ++i, corner += 4u)
            {
                points.push_back(vertices[corner].Position);
            }
            points.push_back(vertices[corner - 4u + 2u].Position);
            out.push_back(std::move(points));
        }
        EXPECT_EQ(corner, vertices.size());
        return out;
    }

    [[nodiscard]] GroomCoatStrandParams ParamsOf(const GroomCoatContext& coat, const GroomAsset& groom, u32 curve)
    {
        return EvaluateGroomCoatStrand(coat, curve, groom.GetRootUVs()[curve], groom.GetCurveGroupIds()[curve]);
    }

    [[nodiscard]] bool SameBits(const glm::vec3& a, const glm::vec3& b) noexcept
    {
        return std::bit_cast<u32>(a.x) == std::bit_cast<u32>(b.x) && std::bit_cast<u32>(a.y) == std::bit_cast<u32>(b.y) &&
               std::bit_cast<u32>(a.z) == std::bit_cast<u32>(b.z);
    }
} // namespace

// ── Curl ─────────────────────────────────────────────────────────────────────

TEST(GroomCoatCurl, ACurledStrandIsAHelixOfItsOwnRadiusAndPitch)
{
    // Eight straight half-metre strands of 400 points, curled at 4 mm and 48
    // turns a metre: 24 turns each and about sixteen points a turn, twice the
    // eight GroomCoat.h asks a grower for.
    constexpr f32 kRadius = 0.004f;
    constexpr f32 kTurnsPerMetre = 48.0f;
    StraightGroup group;
    group.Desc.CurlRadius = kRadius;
    group.Desc.CurlFrequency = kTurnsPerMetre;
    group.Strands = 8;
    group.Points = 400;
    const Ref<GroomAsset> groom = MakeStraightGroom(std::span<const StraightGroup>(&group, 1u));
    ASSERT_TRUE(groom);

    const GroomCoatSettings settings = Active();
    const GroomCoatContext coat{ &settings, groom->GetGroupCoats() };
    const std::vector<std::vector<glm::vec3>> emitted = EmittedPoints(*groom, Build(*groom, coat));
    ASSERT_EQ(emitted.size(), groom->GetCurveCount());

    f32 smallestRadius = 1.0f;
    f32 largestRadius = 0.0f;
    for (u32 curve = 0; curve < groom->GetCurveCount(); ++curve)
    {
        SCOPED_TRACE("curve " + std::to_string(curve));
        const GroomCoatStrandParams params = ParamsOf(coat, *groom, curve);

        // The strand's own draw, inside the documented jitter.
        EXPECT_GE(params.CurlRadius, kRadius * (1.0f - GroomCoatCurl::AmplitudeJitter) * 0.9999f);
        EXPECT_LE(params.CurlRadius, kRadius * (1.0f + GroomCoatCurl::AmplitudeJitter) * 1.0001f);
        EXPECT_GE(params.CurlFrequency, kTurnsPerMetre * (1.0f - GroomCoatCurl::FrequencyJitter) * 0.9999f);
        EXPECT_LE(params.CurlFrequency, kTurnsPerMetre * (1.0f + GroomCoatCurl::FrequencyJitter) * 1.0001f);
        smallestRadius = std::min(smallestRadius, params.CurlRadius);
        largestRadius = std::max(largestRadius, params.CurlRadius);

        const u32 first = groom->GetCurveFirstPoint(curve);
        const u32 count = groom->GetCurvePointCount(curve);
        const glm::vec3 root = groom->GetPoints()[first];
        const glm::vec3 axis = glm::normalize(groom->GetPoints()[first + count - 1u] - root);

        // Past the envelope the offset from the straight axis IS the helix: as
        // long as the radius, with nothing along the axis, and turning about it
        // at the strand's pitch.
        f64 turned = 0.0;
        f32 startAlong = -1.0f;
        f32 endAlong = 0.0f;
        glm::vec3 previousOffset(0.0f);
        u32 measured = 0;
        for (u32 i = 0; i < count; ++i)
        {
            const f32 t = static_cast<f32>(i) / static_cast<f32>(count - 1u);
            if (t < GroomCoatCurl::EnvelopeEnd)
            {
                continue;
            }
            const glm::vec3 original = groom->GetPoints()[first + i];
            const glm::vec3 offset = emitted[curve][i] - original;
            EXPECT_NEAR(glm::length(offset), params.CurlRadius, params.CurlRadius * 1.0e-3f) << "point " << i;
            EXPECT_NEAR(glm::dot(offset, axis), 0.0f, params.CurlRadius * 1.0e-3f) << "point " << i;

            const f32 along = glm::dot(original - root, axis);
            if (startAlong >= 0.0f)
            {
                const f32 sine = glm::dot(glm::cross(previousOffset, offset), axis);
                const f32 cosine = glm::dot(previousOffset, offset);
                turned += static_cast<f64>(std::atan2(sine, cosine));
            }
            else
            {
                startAlong = along;
            }
            endAlong = along;
            previousOffset = offset;
            ++measured;
        }
        ASSERT_GT(measured, 100u);
        const f64 turnsPerMetre =
            std::abs(turned) / (2.0 * std::numbers::pi) / static_cast<f64>(endAlong - startAlong);
        EXPECT_NEAR(turnsPerMetre, static_cast<f64>(params.CurlFrequency), static_cast<f64>(params.CurlFrequency) * 2.0e-3)
            << "the helix turns at " << turnsPerMetre << " per metre";
    }

    // And the jitter really varies the radius: eight strands at one radius would
    // be a coat of identical corkscrews.
    EXPECT_GT(largestRadius - smallestRadius, kRadius * 0.05f);
}

TEST(GroomCoatCurl, AWaveStaysInOnePlaneAndReachesItsAmplitude)
{
    // A wave is PLANAR: every offset lies along one direction across the strand
    // (the frame's normal, which on a straight strand never turns). Its plane is
    // a per-strand hash draw, so two strands do not wave in step.
    constexpr f32 kAmplitude = 0.006f;
    constexpr f32 kPeriodsPerMetre = 20.0f;
    StraightGroup group;
    group.Desc.WaveAmplitude = kAmplitude;
    group.Desc.WaveFrequency = kPeriodsPerMetre;
    group.Strands = 6;
    group.Points = 300;
    const Ref<GroomAsset> groom = MakeStraightGroom(std::span<const StraightGroup>(&group, 1u));
    ASSERT_TRUE(groom);

    const GroomCoatSettings settings = Active();
    const GroomCoatContext coat{ &settings, groom->GetGroupCoats() };
    const std::vector<std::vector<glm::vec3>> emitted = EmittedPoints(*groom, Build(*groom, coat));

    std::vector<glm::vec3> planes;
    for (u32 curve = 0; curve < groom->GetCurveCount(); ++curve)
    {
        SCOPED_TRACE("curve " + std::to_string(curve));
        const GroomCoatStrandParams params = ParamsOf(coat, *groom, curve);
        EXPECT_GE(params.WaveAmplitude, kAmplitude * (1.0f - GroomCoatCurl::AmplitudeJitter) * 0.9999f);
        EXPECT_LE(params.WaveAmplitude, kAmplitude * (1.0f + GroomCoatCurl::AmplitudeJitter) * 1.0001f);
        EXPECT_GE(params.WaveFrequency, kPeriodsPerMetre * (1.0f - GroomCoatCurl::FrequencyJitter) * 0.9999f);
        EXPECT_LE(params.WaveFrequency, kPeriodsPerMetre * (1.0f + GroomCoatCurl::FrequencyJitter) * 1.0001f);

        const u32 first = groom->GetCurveFirstPoint(curve);
        const u32 count = groom->GetCurvePointCount(curve);
        const glm::vec3 root = groom->GetPoints()[first];
        const glm::vec3 axis = glm::normalize(groom->GetPoints()[first + count - 1u] - root);

        // The plane: the direction of the largest offset.
        glm::vec3 direction(0.0f);
        f32 largest = 0.0f;
        for (u32 i = 1; i < count; ++i)
        {
            const glm::vec3 offset = emitted[curve][i] - groom->GetPoints()[first + i];
            if (glm::length(offset) > largest)
            {
                largest = glm::length(offset);
                direction = offset / largest;
            }
        }
        ASSERT_GT(largest, 0.0f);
        EXPECT_NEAR(glm::dot(direction, axis), 0.0f, 1.0e-4f) << "the wave must lie across the strand";

        f32 peak = 0.0f;
        for (u32 i = 1; i < count; ++i)
        {
            const glm::vec3 offset = emitted[curve][i] - groom->GetPoints()[first + i];
            const f32 along = glm::dot(offset, direction);
            EXPECT_LT(glm::length(offset - (direction * along)), kAmplitude * 1.0e-3f)
                << "point " << i << " left the wave's plane";
            if (static_cast<f32>(i) / static_cast<f32>(count - 1u) >= GroomCoatCurl::EnvelopeEnd)
            {
                peak = std::max(peak, std::abs(along));
            }
        }
        // About thirty samples a period, so the sampled crest is within cos(6
        // degrees) of the true one.
        EXPECT_GE(peak, params.WaveAmplitude * 0.99f);
        EXPECT_LE(peak, params.WaveAmplitude * 1.0001f);
        planes.push_back(direction);
    }

    u32 outOfStep = 0;
    for (sizet a = 0; a < planes.size(); ++a)
    {
        for (sizet b = a + 1u; b < planes.size(); ++b)
        {
            outOfStep += std::abs(glm::dot(planes[a], planes[b])) < 0.9f ? 1u : 0u;
        }
    }
    EXPECT_GT(outOfStep, 0u) << "every strand waves in one plane: the per-strand azimuth draw does nothing";
}

TEST(GroomCoatCurl, TheRootIsAnExactFixedPointOfCurlAndWave)
{
    // The envelope is zero at the root and ApplyGroomCoatCurl never writes the
    // first point -- not "adds zero to it", which turns a negative zero positive.
    std::vector<glm::vec3> line;
    for (u32 i = 0; i < 32; ++i)
    {
        line.emplace_back(-0.0f, 0.01f * static_cast<f32>(i), -3.5f + (0.002f * static_cast<f32>(i)));
    }
    const std::vector<glm::vec3> original = line;
    GroomCoatStrandParams extreme;
    extreme.CurlRadius = GroomCoatLimits::MaxCurlRadius;
    extreme.CurlFrequency = GroomCoatLimits::MaxCurlFrequency;
    extreme.CurlPhase = 0.37f;
    extreme.CurlAzimuth = 0.81f;
    extreme.WaveAmplitude = GroomCoatLimits::MaxWaveAmplitude;
    extreme.WaveFrequency = GroomCoatLimits::MaxWaveFrequency;
    extreme.WavePhase = 0.11f;
    ApplyGroomCoatCurl(line, extreme);
    EXPECT_TRUE(SameBits(line.front(), original.front())) << "the root moved, or its negative zero turned positive";
    f32 moved = 0.0f;
    for (sizet i = 0; i < line.size(); ++i)
    {
        moved = std::max(moved, glm::length(line[i] - original[i]));
    }
    EXPECT_GT(moved, GroomCoatLimits::MaxCurlRadius * 0.5f) << "the extreme curl moved nothing";

    // And through the build, on every strand of a curled and waved coat: the
    // first corner of the first segment IS the cooked root, bit for bit.
    StraightGroup group;
    group.Desc.CurlRadius = 0.01f;
    group.Desc.CurlFrequency = 120.0f;
    group.Desc.WaveAmplitude = 0.02f;
    group.Desc.WaveFrequency = 40.0f;
    group.Desc.Length = 0.7f;
    group.Strands = 12;
    group.Points = 24;
    group.Direction = glm::vec3(0.3f, 1.0f, -0.2f);
    const Ref<GroomAsset> groom = MakeStraightGroom(std::span<const StraightGroup>(&group, 1u));
    ASSERT_TRUE(groom);
    const GroomCoatSettings settings = Active();
    const GroomCoatContext coat{ &settings, groom->GetGroupCoats() };
    const std::vector<GroomStrandVertex> vertices = Build(*groom, coat);
    sizet corner = 0;
    for (u32 curve = 0; curve < groom->GetCurveCount(); ++curve)
    {
        const glm::vec3 cooked = groom->GetPoints()[groom->GetCurveFirstPoint(curve)];
        EXPECT_TRUE(SameBits(vertices[corner + 0u].Position, cooked)) << "curve " << curve;
        EXPECT_TRUE(SameBits(vertices[corner + 1u].Position, cooked)) << "curve " << curve;
        corner += static_cast<sizet>(groom->GetCurvePointCount(curve) - 1u) * 4u;
    }
}

TEST(GroomCoatCurl, AGroupThatAuthorsNoCurlEmitsTheV3StreamBitForBit)
{
    // A group that authors length, width and a tint -- but no curl, no wave and
    // a tip tint equal to its tint -- must emit, lane for lane, exactly what the
    // v3 build wrote: positions through ApplyGroomCoatShape alone, Other = P0 +
    // (P1 - P0), the strand's one packed tint on all four corners. Built
    // INTERLEAVED with a group that does curl, so a scratch buffer that carried
    // one strand's shape into the next would fail here.
    StraightGroup plain;
    plain.Name = "plain";
    plain.Desc.Length = 0.8f;
    plain.Desc.Width = 1.5f;
    plain.Desc.Tint = glm::vec3(0.6f, 0.5f, 0.4f);
    plain.Strands = 6;
    plain.Points = 12;
    plain.Direction = glm::vec3(0.2f, 1.0f, 0.1f);
    StraightGroup curled;
    curled.Name = "curled";
    curled.Desc.CurlRadius = 0.003f;
    curled.Desc.CurlFrequency = 60.0f;
    curled.Desc.WaveAmplitude = 0.002f;
    curled.Desc.WaveFrequency = 30.0f;
    curled.Strands = 6;
    curled.Points = 40;
    const std::vector<StraightGroup> groups{ curled, plain };
    const Ref<GroomAsset> groom = MakeStraightGroom(groups);
    ASSERT_TRUE(groom);

    GroomCoatSettings settings = Active();
    settings.ColorMap = FlatMap(200, 150, 100);
    settings.ShadeJitter = 0.3f;
    const GroomCoatContext coat{ &settings, groom->GetGroupCoats() };
    GroomStrandMeshStats stats;
    const std::vector<GroomStrandVertex> vertices = Build(*groom, coat, &stats);

    u32 plainCorners = 0;
    u32 curledCornersMoved = 0;
    sizet corner = 0;
    for (u32 curve = 0; curve < groom->GetCurveCount(); ++curve)
    {
        const GroomCoatStrandParams params = ParamsOf(coat, *groom, curve);
        const bool isPlain = groom->GetGroupNames()[groom->GetCurveGroupIds()[curve]] == "plain";
        const u32 first = groom->GetCurveFirstPoint(curve);
        const u32 count = groom->GetCurvePointCount(curve);
        const f32 invSpan = 1.0f / static_cast<f32>(count - 1u);
        const glm::vec3 root = groom->GetPoints()[first];
        const auto role = static_cast<sizet>(groom->GetGroupCoat(groom->GetCurveGroupIds()[curve]).GetRole());
        const f32 widthScale =
            params.Width * GroomRoleWidthCompensation(stats.AvailableByRole[role], stats.StrideByRole[role], 1.0f);
        const f32 packedTint = PackGroomCoatTint(params.Tint);

        for (u32 i = 0; i + 1u < count; ++i, corner += 4u)
        {
            const f32 t0 = static_cast<f32>(i) * invSpan;
            const f32 t1 = static_cast<f32>(i + 1u) * invSpan;
            const glm::vec3 p0 = ApplyGroomCoatShape(root, groom->GetPoints()[first + i], t0, params.Length,
                                                     params.Clump, glm::vec3(0.0f));
            const glm::vec3 p1 = ApplyGroomCoatShape(root, groom->GetPoints()[first + i + 1u], t1, params.Length,
                                                     params.Clump, glm::vec3(0.0f));
            if (!isPlain)
            {
                curledCornersMoved += SameBits(vertices[corner + 2u].Position, p1) ? 0u : 1u;
                continue;
            }

            // The v3 vertex, built here field by field.
            const glm::vec3 delta = p1 - p0;
            std::array<GroomStrandVertex, 4> expected{};
            for (u32 c = 0; c < 4u; ++c)
            {
                GroomStrandVertex& v = expected[c];
                const bool atP1 = c >= 2u;
                v.Position = atP1 ? p1 : p0;
                v.Other = (atP1 ? p1 : p0) + delta;
                v.PrevPosition = v.Position;
                v.Radius = groom->GetPointWidths()[first + i + (atP1 ? 1u : 0u)] * 0.5f * widthScale;
                v.Side = (c == 0u || c == 3u) ? -1.0f : 1.0f;
                v.Coords = { atP1 ? t1 : t0, v.Side };
                v.SegmentId = std::bit_cast<f32>(GroomSegmentIdentity(curve, i));
                v.Tint = packedTint;
            }
            for (u32 c = 0; c < 4u; ++c)
            {
                EXPECT_EQ(std::memcmp(&vertices[corner + c], &expected[c], sizeof(GroomStrandVertex)), 0)
                    << "curve " << curve << " segment " << i << " corner " << c << " is not the v3 vertex";
                ++plainCorners;
            }
        }
    }
    EXPECT_EQ(corner, vertices.size());
    EXPECT_EQ(plainCorners, 6u * 11u * 4u);
    // NEGATIVE CONTROL: the curled group really is not the v3 stream.
    EXPECT_GT(curledCornersMoved, 6u * 30u) << "the curled group emitted its uncurled shape";
}

TEST(GroomCoatCurl, ACardCarriesItsMembersMeanCurl)
{
    // A card stands for N strands whose curls have independent phases, and the
    // mean of N unit phasors of random phase has magnitude 1/sqrt(N). So a card's
    // curl radius and wave amplitude are the strand's times its group's
    // JitterScale -- the factor its length jitter already carries (#1428) --
    // while its frequency and phases are the strand's own draws. (The walk fills
    // JitterScales for a card level; GroomLodContractTest measures that end to
    // end on a cooked level.)
    GroomCoatGroupDesc desc;
    desc.CurlRadius = 0.01f;
    desc.CurlFrequency = 50.0f;
    desc.WaveAmplitude = 0.004f;
    desc.WaveFrequency = 25.0f;
    const std::vector<GroomCoatGroupDesc> table{ desc };
    const GroomCoatSettings settings = Active();
    const std::vector<f32> scales{ 0.25f };
    const GroomCoatContext strands{ &settings, table };
    GroomCoatContext cards{ &settings, table };
    cards.JitterScales = scales;

    for (u32 curve = 0; curve < 64u; ++curve)
    {
        const glm::vec2 uv(0.01f * static_cast<f32>(curve), 0.5f);
        const GroomCoatStrandParams strand = EvaluateGroomCoatStrand(strands, curve, uv, 0u);
        const GroomCoatStrandParams card = EvaluateGroomCoatStrand(cards, curve, uv, 0u);
        EXPECT_FLOAT_EQ(card.CurlRadius, strand.CurlRadius * 0.25f) << "curve " << curve;
        EXPECT_FLOAT_EQ(card.WaveAmplitude, strand.WaveAmplitude * 0.25f) << "curve " << curve;
        EXPECT_EQ(std::bit_cast<u32>(card.CurlFrequency), std::bit_cast<u32>(strand.CurlFrequency));
        EXPECT_EQ(std::bit_cast<u32>(card.WaveFrequency), std::bit_cast<u32>(strand.WaveFrequency));
        EXPECT_EQ(std::bit_cast<u32>(card.CurlPhase), std::bit_cast<u32>(strand.CurlPhase));
        EXPECT_EQ(std::bit_cast<u32>(card.WavePhase), std::bit_cast<u32>(strand.WavePhase));
    }
}

TEST(GroomCoatCurl, TheCurlDrawsAreKeyedOnTheStrandAlone)
{
    // The same keys as every other coat decision -- curve index, root UV, group
    // and the seed -- and independent salts: a strand's phase must not predict
    // its radius, or its wave's phase its curl's.
    GroomCoatGroupDesc desc;
    desc.CurlRadius = 0.01f;
    desc.CurlFrequency = 50.0f;
    desc.WaveAmplitude = 0.004f;
    desc.WaveFrequency = 25.0f;
    const std::vector<GroomCoatGroupDesc> table{ desc };
    const GroomCoatSettings settings = Active();
    const GroomCoatContext coat{ &settings, table };

    constexpr u32 kStrands = 4096;
    u32 phaseAgreesWithRadius = 0;
    u32 wavePhaseAgreesWithCurlPhase = 0;
    for (u32 curve = 0; curve < kStrands; ++curve)
    {
        const glm::vec2 uv(0.3f, 0.7f);
        const GroomCoatStrandParams a = EvaluateGroomCoatStrand(coat, curve, uv, 0u);
        const GroomCoatStrandParams b = EvaluateGroomCoatStrand(coat, curve, uv, 0u);
        ASSERT_EQ(a, b) << "curve " << curve << ": the evaluation is not a pure function of its keys";
        phaseAgreesWithRadius += ((a.CurlPhase > 0.5f) == (a.CurlRadius > desc.CurlRadius)) ? 1u : 0u;
        wavePhaseAgreesWithCurlPhase += ((a.WavePhase > 0.5f) == (a.CurlPhase > 0.5f)) ? 1u : 0u;
        EXPECT_GE(a.CurlPhase, 0.0f);
        EXPECT_LT(a.CurlPhase, 1.0f);
        EXPECT_GE(a.CurlAzimuth, 0.0f);
        EXPECT_LT(a.CurlAzimuth, 1.0f);
    }
    EXPECT_NEAR(static_cast<f32>(phaseAgreesWithRadius) / kStrands, 0.5f, 0.05f);
    EXPECT_NEAR(static_cast<f32>(wavePhaseAgreesWithCurlPhase) / kStrands, 0.5f, 0.05f);

    // A different seed is a different coat.
    GroomCoatSettings reseeded = Active();
    reseeded.Seed = 7u;
    const GroomCoatContext other{ &reseeded, table };
    EXPECT_NE(std::bit_cast<u32>(EvaluateGroomCoatStrand(other, 3u, glm::vec2(0.3f), 0u).CurlPhase),
              std::bit_cast<u32>(EvaluateGroomCoatStrand(coat, 3u, glm::vec2(0.3f), 0u).CurlPhase));
}

// ── The root-to-tip tint ─────────────────────────────────────────────────────

TEST(GroomCoatCurl, TheTintRunsFromTheRootTintToTheTipTint)
{
    // Per corner: pack(ColorMap(rootUV) * shade * mix(Tint, Tint * TipTint,
    // t)): the group's TipTint multiplies its Tint at the tip. The fragment
    // then interpolates the unpacked colour along the ribbon.
    StraightGroup group;
    group.Desc.Tint = glm::vec3(0.2f, 0.4f, 0.6f);
    group.Desc.TipTint = glm::vec3(0.9f, 0.7f, 0.5f);
    group.Strands = 3;
    group.Points = 9;
    const Ref<GroomAsset> groom = MakeStraightGroom(std::span<const StraightGroup>(&group, 1u));
    ASSERT_TRUE(groom);

    GroomCoatSettings settings = Active();
    settings.ColorMap = FlatMap(128, 204, 255);
    const GroomCoatContext coat{ &settings, groom->GetGroupCoats() };
    const glm::vec3 map = settings.ColorMap->Sample(glm::vec2(0.5f));
    const std::vector<GroomStrandVertex> vertices = Build(*groom, coat);

    constexpr f32 kStep = (1.0f / 255.0f) + 1.0e-5f; // the lane is 8 bits a channel
    sizet corner = 0;
    for (u32 curve = 0; curve < groom->GetCurveCount(); ++curve)
    {
        const u32 count = groom->GetCurvePointCount(curve);
        for (u32 i = 0; i + 1u < count; ++i, corner += 4u)
        {
            for (u32 c = 0; c < 4u; ++c)
            {
                const GroomStrandVertex& v = vertices[corner + c];
                const f32 t = v.Coords.x;
                const glm::vec3 expected = map * glm::mix(group.Desc.Tint, group.Desc.Tint * group.Desc.TipTint, t);
                const glm::vec3 got = UnpackGroomCoatTint(v.Tint);
                EXPECT_NEAR(got.x, expected.x, kStep) << "curve " << curve << " segment " << i << " corner " << c;
                EXPECT_NEAR(got.y, expected.y, kStep) << "curve " << curve << " segment " << i << " corner " << c;
                EXPECT_NEAR(got.z, expected.z, kStep) << "curve " << curve << " segment " << i << " corner " << c;
            }
            // Both corners at one end of a segment carry one value, and the
            // ribbon is continuous: this segment's P0 is the last one's P1.
            EXPECT_EQ(std::bit_cast<u32>(vertices[corner].Tint), std::bit_cast<u32>(vertices[corner + 1u].Tint));
            EXPECT_EQ(std::bit_cast<u32>(vertices[corner + 2u].Tint), std::bit_cast<u32>(vertices[corner + 3u].Tint));
            if (i > 0u)
            {
                EXPECT_EQ(std::bit_cast<u32>(vertices[corner].Tint), std::bit_cast<u32>(vertices[corner - 2u].Tint));
            }
        }
        // The two ends, stated directly.
        const sizet rootCorner = corner - (static_cast<sizet>(count - 1u) * 4u);
        const glm::vec3 atRoot = UnpackGroomCoatTint(vertices[rootCorner].Tint);
        const glm::vec3 atTip = UnpackGroomCoatTint(vertices[corner - 2u].Tint);
        EXPECT_NEAR(atRoot.x, group.Desc.Tint.x * map.x, kStep);
        EXPECT_NEAR(atRoot.z, group.Desc.Tint.z * map.z, kStep);
        EXPECT_NEAR(atTip.x, group.Desc.Tint.x * group.Desc.TipTint.x * map.x, kStep);
        EXPECT_NEAR(atTip.z, group.Desc.Tint.z * group.Desc.TipTint.z * map.z, kStep);
    }
}

TEST(GroomCoatCurl, AnUnauthoredTipLeavesEveryCornerAtTheStrandsTint)
{
    // TipTint multiplies Tint and defaults to white, so a coat that authors
    // no tip -- tinted or not -- packs every corner of a strand exactly as v3
    // packed the whole strand: one value, bit for bit, through the colour map
    // and the shade jitter alike. The tinted group is the case an ABSOLUTE
    // white default would have faded to white at the tips.
    StraightGroup untinted;
    untinted.Name = "untinted";
    untinted.Strands = 5;
    untinted.Points = 7;
    StraightGroup uniform;
    uniform.Name = "uniform";
    uniform.Desc.Tint = glm::vec3(0.3f, 0.6f, 0.9f);
    uniform.Strands = 5;
    uniform.Points = 7;
    const std::vector<StraightGroup> groups{ untinted, uniform };
    const Ref<GroomAsset> groom = MakeStraightGroom(groups);
    ASSERT_TRUE(groom);

    GroomCoatSettings settings = Active();
    settings.ColorMap = FlatMap(90, 180, 250);
    settings.ShadeJitter = 0.4f;
    const GroomCoatContext coat{ &settings, groom->GetGroupCoats() };
    const std::vector<GroomStrandVertex> vertices = Build(*groom, coat);

    sizet corner = 0;
    for (u32 curve = 0; curve < groom->GetCurveCount(); ++curve)
    {
        const GroomCoatStrandParams params = ParamsOf(coat, *groom, curve);
        const u32 strandTint = std::bit_cast<u32>(PackGroomCoatTint(params.Tint));
        const u32 corners = (groom->GetCurvePointCount(curve) - 1u) * 4u;
        for (u32 c = 0; c < corners; ++c, ++corner)
        {
            EXPECT_EQ(std::bit_cast<u32>(vertices[corner].Tint), strandTint) << "curve " << curve << " corner " << c;
        }
    }
    EXPECT_EQ(corner, vertices.size());
}

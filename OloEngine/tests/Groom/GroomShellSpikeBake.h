#pragma once

// Experiment-only cylindrical charts matching GroomAlembicFixture's committed
// reference coats. A chart is selected by region, not UV alone: mane and muzzle
// deliberately overlap the head's UV band. No production asset is modified.
#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Groom/GroomAsset.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace OloEngine::Tests::GroomShellSpike
{
    constexpr u32 MapWidth = 64;
    constexpr u32 MapHeight = 32;
    constexpr u32 HeightSlices = 32;
    constexpr f32 TwoPi = 6.283185307f;

    struct Chart
    {
        std::string Name;
        f32 Radius;
        f32 From;
        f32 To;
        f32 VFrom;
        f32 VTo;
    };

    inline std::vector<Chart> Charts(bool longCoat)
    {
        std::vector<Chart> result{
            { "body", longCoat ? 0.12f : 0.11f, -0.35f, 0.30f, 0.0f, 0.5f },
            { "head", longCoat ? 0.08f : 0.075f, 0.30f, 0.42f, 0.5f, 0.7f },
            { "ears", longCoat ? 0.055f : 0.05f, 0.42f, 0.48f, 0.7f, 0.85f },
            { longCoat ? "tail_plume" : "tail", longCoat ? 0.04f : 0.035f,
              longCoat ? -0.52f : -0.50f, -0.35f, 0.85f, 1.0f },
            { "muzzle_whiskers", longCoat ? 0.048f : 0.045f, 0.44f, 0.47f, 0.5f, 0.52f }
        };
        if (longCoat)
        {
            result.push_back({ "mane", 0.115f, 0.16f, 0.32f, 0.52f, 0.68f });
        }
        return result;
    }

    inline Ref<GroomAsset> Load(bool longCoat)
    {
        const std::filesystem::path path = std::filesystem::path("SandboxProject/Assets/Grooms") /
                                           (longCoat ? "reference-longcoat-animal.ologroom" : "reference-shortcoat-animal.ologroom");
        std::ifstream input(path, std::ios::binary);
        std::vector<char> bytes{ std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
        Ref<GroomAsset> groom;
        std::string reason;
        if (!input || !GroomSerializer::DecodeFromBytes(bytes.data(), bytes.size(), groom, reason))
        {
            throw std::runtime_error("shell spike cannot load " + path.string() + ": " + reason);
        }
        return groom;
    }

    inline const Chart& ChartFor(const GroomAsset& groom, u32 curve, const std::vector<Chart>& charts)
    {
        const std::string& group = groom.GetGroupNames()[groom.GetCurveGroupIds()[curve]];
        for (const Chart& chart : charts)
        {
            if (group.starts_with("/" + chart.Name + "#"))
            {
                return chart;
            }
        }
        throw std::runtime_error("unmapped reference-coat group " + group);
    }

    inline glm::vec3 Normal(f32 u)
    {
        return { std::cos(u * TwoPi), std::sin(u * TwoPi), 0.0f };
    }

    inline glm::vec3 Surface(const Chart& chart, glm::vec2 uv)
    {
        return Normal(uv.x) * chart.Radius + glm::vec3(0.0f, 0.0f, chart.From + uv.y * (chart.To - chart.From));
    }

    inline glm::vec2 LocalUV(const Chart& chart, glm::vec2 uv)
    {
        return { uv.x, (uv.y - chart.VFrom) / (chart.VTo - chart.VFrom) };
    }

    inline sizet Texel(glm::vec2 uv)
    {
        const f32 u = uv.x - std::floor(uv.x);
        const u32 x = std::min(static_cast<u32>(u * MapWidth), MapWidth - 1);
        const u32 y = std::min(static_cast<u32>(std::clamp(uv.y, 0.0f, 1.0f) * MapHeight), MapHeight - 1);
        return static_cast<sizet>(y) * MapWidth + x;
    }

    struct Bake
    {
        Chart Patch;
        // RGBA: maximum normal reach in metres, mean tangent displacement /
        // normal reach along circumferential and axial bases, contributing roots.
        std::vector<glm::vec4> LengthDirection;
        // Filtered fibre measure in each normal-height slab, stored as
        // 1-exp(-tau). Contributions use cooked widths and actual 3D paths;
        // residual lean stays spatial. U wraps; V never crosses charts.
        // R: fibre width times 3D arc length integrated through the height slab
        // per surface texel area. The shader projects this measure by
        // |cross(tangent, view)| / |dot(normal, view)|, so an outward fibre
        // contributes its oblique projected length, not just tangent-plane lean.
        // G: cross-section occupancy at the slab centre, correlated per pixel
        // across layers. A cross-section does not scale with slab thickness.
        std::vector<glm::vec2> Density;
        f32 MaxRootError = 0.0f;
        u32 Curves = 0;
    };

    inline Bake BakeChart(const GroomAsset& groom, const Chart& chart, const std::vector<Chart>& charts)
    {
        Bake bake{ chart, std::vector<glm::vec4>(MapWidth * MapHeight, glm::vec4(0.0f)),
                   std::vector<glm::vec2>(MapWidth * MapHeight * HeightSlices, glm::vec2(0.0f)) };
        for (u32 c = 0; c < groom.GetCurveCount(); ++c)
        {
            if (ChartFor(groom, c, charts).Name != chart.Name)
            {
                continue;
            }
            const glm::vec2 uv = LocalUV(chart, groom.GetRootUVs()[c]);
            const u32 first = groom.GetCurveFirstPoint(c);
            const u32 count = groom.GetCurvePointCount(c);
            const glm::vec3 root = groom.GetPoints()[first];
            const glm::vec3 normal = Normal(uv.x);
            const glm::vec3 tangent{ -normal.y, normal.x, 0.0f };
            const glm::vec3 tip = groom.GetPoints()[first + count - 1] - root;
            f32 reach = 0.0f;
            for (u32 p = 1; p < count; ++p)
            {
                reach = std::max(reach, glm::dot(groom.GetPoints()[first + p] - root, normal));
            }
            if (reach <= 1.0e-7f)
            {
                throw std::runtime_error("reference curve has no outward normal reach");
            }
            bake.MaxRootError = std::max(bake.MaxRootError, glm::length(root - Surface(chart, uv)));
            auto& map = bake.LengthDirection[Texel(uv)];
            map.x = std::max(map.x, reach);
            map.y += glm::dot(tip, tangent) / reach;
            map.z += tip.z / reach;
            map.w += 1.0f;
            ++bake.Curves;
        }
        if (bake.Curves == 0 || bake.MaxRootError > 1.0e-3f)
        {
            throw std::runtime_error("reference chart/root binding mismatch");
        }
        for (auto& map : bake.LengthDirection)
        {
            if (map.w > 0.0f)
            {
                map.y /= map.w;
                map.z /= map.w;
            }
        }
        // Empty texels have no density, but geometry needs a defined envelope.
        // Fill only envelope/direction from the nearest occupied texel, using
        // periodic U distance and bounded V. Never invent contributing roots.
        const auto occupied = bake.LengthDirection;
        for (u32 y = 0; y < MapHeight; ++y)
        {
            for (u32 x = 0; x < MapWidth; ++x)
            {
                auto& map = bake.LengthDirection[y * MapWidth + x];
                if (map.w > 0.0f)
                {
                    continue;
                }
                u32 best = MapWidth * MapWidth + MapHeight * MapHeight;
                for (u32 sy = 0; sy < MapHeight; ++sy)
                {
                    for (u32 sx = 0; sx < MapWidth; ++sx)
                    {
                        const auto& source = occupied[sy * MapWidth + sx];
                        if (source.w <= 0.0f)
                        {
                            continue;
                        }
                        u32 dx = x > sx ? x - sx : sx - x;
                        dx = std::min(dx, MapWidth - dx);
                        const u32 dy = y > sy ? y - sy : sy - y;
                        if (const u32 distance = dx * dx + dy * dy; distance < best)
                        {
                            best = distance;
                            map = { source.x, source.y, source.z, 0.0f };
                        }
                    }
                }
            }
        }
        const f32 area = TwoPi * chart.Radius * (chart.To - chart.From) / (MapWidth * MapHeight);
        for (u32 c = 0; c < groom.GetCurveCount(); ++c)
        {
            if (ChartFor(groom, c, charts).Name != chart.Name)
            {
                continue;
            }
            const glm::vec2 uv = LocalUV(chart, groom.GetRootUVs()[c]);
            const sizet cell = Texel(uv);
            const f32 length = bake.LengthDirection[cell].x;
            const glm::vec3 normal = Normal(uv.x);
            const glm::vec3 circumferential{ -normal.y, normal.x, 0.0f };
            const u32 first = groom.GetCurveFirstPoint(c);
            const u32 count = groom.GetCurvePointCount(c);
            const glm::vec3 root = groom.GetPoints()[first];
            for (u32 p = 1; p < count; ++p)
            {
                const glm::vec3 a = groom.GetPoints()[first + p - 1] - root;
                const glm::vec3 b = groom.GetPoints()[first + p] - root;
                const f32 ha = glm::dot(a, normal) / length;
                const f32 hb = glm::dot(b, normal) / length;
                if (hb <= ha + 1.0e-7f)
                {
                    throw std::runtime_error("non-monotone reference normal height needs a different bake");
                }
                const glm::vec3 tangential = b - a - normal * glm::dot(b - a, normal);
                for (u32 h = 0; h < HeightSlices; ++h)
                {
                    const f32 low = std::max(ha, static_cast<f32>(h) / HeightSlices);
                    const f32 high = std::min(hb, static_cast<f32>(h + 1) / HeightSlices);
                    if (high <= low)
                    {
                        continue;
                    }
                    const f32 tLow = (low - ha) / (hb - ha);
                    const f32 tHigh = (high - ha) / (hb - ha);
                    const f32 width = std::lerp(groom.GetPointWidths()[first + p - 1], groom.GetPointWidths()[first + p], (tLow + tHigh) * 0.5f);
                    const f32 sweptArea = width * glm::length(b - a) * (tHigh - tLow);
                    // Deposit actual height-dependent positions, with the mean
                    // lean removed because the shader restores that lean. The
                    // residual preserves curved/divergent members of a patch.
                    const auto map = bake.LengthDirection[cell];
                    const glm::vec2 uvScale{ TwoPi * chart.Radius, chart.To - chart.From };
                    const f32 cellSize = std::min(uvScale.x / MapWidth, uvScale.y / MapHeight);
                    const u32 taps = std::max(1u, static_cast<u32>(std::ceil(glm::length(tangential) * (tHigh - tLow) / cellSize * 4.0f)));
                    for (u32 tap = 0; tap < taps; ++tap)
                    {
                        const f32 t = std::lerp(tLow, tHigh, (static_cast<f32>(tap) + 0.5f) / taps);
                        const glm::vec3 point = glm::mix(a, b, t);
                        const f32 height = std::lerp(ha, hb, t);
                        const glm::vec2 residual{ glm::dot(point, circumferential) - height * length * map.y,
                                                  point.z - height * length * map.z };
                        const glm::vec2 position = uv + residual / uvScale;
                        if (position.y >= 0.0f && position.y <= 1.0f)
                        {
                            bake.Density[h * MapWidth * MapHeight + Texel(position)].x += sweptArea / (area * taps);
                        }
                    }
                    // Exactly one cross-section per curve/slab. Subdividing a
                    // polyline cannot add additional caps to the same fibre.
                    const f32 centreHeight = (static_cast<f32>(h) + 0.5f) / HeightSlices;
                    if (centreHeight >= ha && centreHeight < hb)
                    {
                        const f32 t = (centreHeight - ha) / (hb - ha);
                        const glm::vec3 point = glm::mix(a, b, t);
                        const f32 diameter = std::lerp(groom.GetPointWidths()[first + p - 1], groom.GetPointWidths()[first + p], t);
                        const glm::vec2 residual{ glm::dot(point, circumferential) - centreHeight * length * map.y,
                                                  point.z - centreHeight * length * map.z };
                        const glm::vec2 position = uv + residual / uvScale;
                        if (position.y >= 0.0f && position.y <= 1.0f)
                        {
                            bake.Density[h * MapWidth * MapHeight + Texel(position)].y += 0.25f * 3.141592654f * diameter * diameter / area;
                        }
                    }
                }
            }
        }
        for (auto& density : bake.Density)
        {
            density = { -std::expm1(-density.x), -std::expm1(-density.y) };
        }
        return bake;
    }
} // namespace OloEngine::Tests::GroomShellSpike

#pragma once

// =============================================================================
// GroomBindingFixture.h — a body and a coat, built in memory. Issue #1249.
//
// Every binding test needs the same two things: a triangulated surface with
// known geometry, and a groom whose roots sit on it at known places. Building
// them here rather than loading an asset is what lets these tests assert on
// EXACT positions — a strand rooted at (0.5, 0, 0.5) on a flat grid is at
// (0.5, 0, 0.5), so "the root did not float" is an equality against a number
// rather than a tolerance against a picture.
//
// The surface is a FLAT GRID in the XZ plane at y = 0, with the +Y normal. Flat
// because the interesting failures are about frames and skinning, and a curved
// surface would make every expected value the output of the thing under test.
// =============================================================================

#include "OloEngine/Core/Base.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomBuilder.h"
#include "OloEngine/Groom/GroomDeformation.h"
#include "OloEngine/Groom/GroomBinding.h"
#include "OloEngine/Groom/GroomSurfaceFrame.h"

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace OloEngine::GroomBindingTest
{
    // A flat grid on the XZ plane spanning [0,1] x [0,1] at y = 0.
    //
    // Owns its arrays; the view it hands out borrows them, so a fixture must
    // outlive every view taken from it. That is the same ownership rule
    // GroomSurfaceView states, exercised.
    struct GridSurface
    {
        std::vector<glm::vec3> Positions;
        std::vector<u32> Indices;
        // Four bone ids and four weights per vertex, laid out exactly as
        // MeshSource's BoneInfluence is: ids at offset 0, weights at offset 16,
        // stride 32. Held as raw bytes so the test exercises the STRIDED reader
        // rather than a convenience path the runtime does not use.
        std::vector<std::byte> Influences;

        [[nodiscard]] u32 VertexCount() const
        {
            return static_cast<u32>(Positions.size());
        }

        [[nodiscard]] GroomSurfaceView View(u32 boneCount = 0u, u64 skeletonHash = 0u) const
        {
            GroomSurfaceView view;
            view.PositionData = reinterpret_cast<const std::byte*>(Positions.data());
            view.PositionStride = static_cast<u32>(sizeof(glm::vec3));
            view.VertexCount = VertexCount();
            view.Indices = Indices.data();
            view.IndexCount = static_cast<u32>(Indices.size());
            view.BoneCount = boneCount;
            view.SkeletonNameHash = skeletonHash;
            return view;
        }

        // Takes the palettes by CONST REFERENCE and deletes the rvalue
        // overloads, so `Skinning(BentPalette(40.0f), ...)` is a compile error
        // rather than a dangling span.
        //
        // That is not a style preference — it was a real bug in these very
        // tests. GroomSkinningView holds std::spans (documented as "borrowed for
        // the call, never retained"), and a span over a temporary vector
        // survives the semicolon as a pointer and a size. The symptom was not a
        // crash: the freed memory still read as SOMETHING, the previous-pose
        // frame came out degenerate, the evaluation aliased prev to current, and
        // three tests failed with "a bent coat emitted no strand motion at all".
        // A lifetime bug that reports itself as a feature not working is exactly
        // the kind this fixture should make impossible to write.
        [[nodiscard]] GroomSkinningView Skinning(const std::vector<glm::mat4>& palette,
                                                 const std::vector<glm::mat4>& prevPalette,
                                                 bool hasPreviousPose) const
        {
            GroomSkinningView view;
            if (Influences.empty())
            {
                return view;
            }
            view.BoneIds = reinterpret_cast<const u32*>(Influences.data());
            view.Weights = reinterpret_cast<const f32*>(Influences.data() + 16);
            view.Stride = 32u;
            view.VertexCount = VertexCount();
            view.Palette = palette;
            view.PrevPalette = prevPalette;
            view.HasPreviousPose = hasPreviousPose;
            return view;
        }

        GroomSkinningView Skinning(std::vector<glm::mat4>&&, const std::vector<glm::mat4>&, bool) const = delete;
        GroomSkinningView Skinning(const std::vector<glm::mat4>&, std::vector<glm::mat4>&&, bool) const = delete;
        GroomSkinningView Skinning(std::vector<glm::mat4>&&, std::vector<glm::mat4>&&, bool) const = delete;

        void SetInfluence(u32 vertex, u32 bone0, f32 weight0, u32 bone1 = 0u, f32 weight1 = 0.0f)
        {
            std::byte* base = Influences.data() + static_cast<sizet>(vertex) * 32u;
            const u32 ids[4] = { bone0, bone1, 0u, 0u };
            const f32 weights[4] = { weight0, weight1, 0.0f, 0.0f };
            std::memcpy(base, ids, sizeof(ids));
            std::memcpy(base + 16, weights, sizeof(weights));
        }
    };

    /// `divisions` cells per side, so (divisions + 1)^2 vertices and
    /// divisions^2 * 2 triangles. Winding is counter-clockwise seen from +Y, so
    /// the geometric normal is +Y.
    [[nodiscard]] inline GridSurface MakeGrid(u32 divisions)
    {
        GridSurface grid;
        const u32 side = divisions + 1u;
        grid.Positions.reserve(static_cast<sizet>(side) * side);
        for (u32 z = 0; z < side; ++z)
        {
            for (u32 x = 0; x < side; ++x)
            {
                grid.Positions.push_back({ static_cast<f32>(x) / static_cast<f32>(divisions), 0.0f,
                                           static_cast<f32>(z) / static_cast<f32>(divisions) });
            }
        }
        for (u32 z = 0; z < divisions; ++z)
        {
            for (u32 x = 0; x < divisions; ++x)
            {
                const u32 v00 = z * side + x;
                const u32 v10 = v00 + 1u;
                const u32 v01 = v00 + side;
                const u32 v11 = v01 + 1u;
                // CCW from +Y: (v00, v01, v10) has cross((v01-v00),(v10-v00))
                // pointing along +Y.
                grid.Indices.insert(grid.Indices.end(), { v00, v01, v10 });
                grid.Indices.insert(grid.Indices.end(), { v10, v01, v11 });
            }
        }
        grid.Influences.assign(static_cast<sizet>(grid.Positions.size()) * 32u, std::byte{ 0 });
        return grid;
    }

    /// Weights every vertex entirely to bone 0 — a body that is rigged but not
    /// yet bent. Deforming with an identity palette must then be a no-op, which
    /// is the baseline every other deformation assertion is measured against.
    inline void WeightAllToBone0(GridSurface& grid)
    {
        for (u32 vertex = 0; vertex < grid.VertexCount(); ++vertex)
        {
            grid.SetInfluence(vertex, 0u, 1.0f);
        }
    }

    /// A HINGE along x: vertices at x <= 0.5 belong to bone 0, the rest to bone
    /// 1, with no blend region. A hard split rather than a smooth one on
    /// purpose — a smooth blend makes every expected position the output of the
    /// blend, and what these tests are about is what happens to a strand when
    /// the triangle under it moves, not how nicely it was interpolated there.
    inline void WeightAsHinge(GridSurface& grid, f32 splitX = 0.5f)
    {
        for (u32 vertex = 0; vertex < grid.VertexCount(); ++vertex)
        {
            const bool secondHalf = grid.Positions[vertex].x > splitX;
            grid.SetInfluence(vertex, secondHalf ? 1u : 0u, 1.0f);
        }
    }

    /// A groom of `count` straight strands, each `pointsPerCurve` points tall,
    /// rooted ON the grid at evenly spaced positions along its diagonal and
    /// rising along +Y.
    /// `footprint` scales where the roots sit in X/Z without touching the strand
    /// height, so a coat can be authored over a body that is SCALED relative to
    /// it — the case GroomStrandCoat.olo has and the binder has to handle (see
    /// GroomBindingBuildSettings::SurfaceToGroom).
    [[nodiscard]] inline Ref<GroomAsset> MakeCoat(u32 count, u32 pointsPerCurve = 4u, f32 height = 0.1f,
                                                  f32 rootOffsetY = 0.0f, f32 footprint = 1.0f)
    {
        GroomBuilder builder;
        std::string reason;
        u16 group = 0;
        EXPECT_TRUE(builder.AddGroup("coat", group, reason)) << reason;

        std::vector<glm::vec3> points(pointsPerCurve);
        std::vector<f32> widths(pointsPerCurve, 0.001f);

        for (u32 c = 0; c < count; ++c)
        {
            // Spread along the diagonal, inset a little so no root lands
            // exactly on the grid's outer edge — an edge root is a legitimate
            // case but it is the CLAMPED one, and the baseline fixture should
            // produce Exact roots so a test that cares can say so.
            const f32 spread = count > 1u ? (0.1f + 0.8f * static_cast<f32>(c) / static_cast<f32>(count - 1u)) : 0.5f;
            const f32 t = spread * footprint;
            for (u32 p = 0; p < pointsPerCurve; ++p)
            {
                const f32 up = height * static_cast<f32>(p) / static_cast<f32>(pointsPerCurve - 1u);
                points[p] = { t, rootOffsetY + up, t };
            }
            GroomCurveInput input;
            input.Points = points;
            input.Widths = widths;
            input.RootUV = { spread, spread };
            input.GroupId = group;
            input.IsGuide = (c % 4u) == 0u;
            EXPECT_TRUE(builder.AddCurve(input, reason)) << reason;
        }

        Ref<GroomAsset> groom = builder.Build(reason);
        EXPECT_TRUE(groom) << reason;
        return groom;
    }

    /// The length of curve `curveIndex` measured through `points`, which must
    /// be parallel to the groom's own point array. The one number that says
    /// whether a coat collapsed: a rigid transfer preserves it exactly.
    [[nodiscard]] inline f32 CurveLength(const GroomAsset& groom, u32 curveIndex,
                                         const std::vector<glm::vec3>& points)
    {
        const u32 first = groom.GetCurveFirstPoint(curveIndex);
        const u32 count = groom.GetCurvePointCount(curveIndex);
        f32 length = 0.0f;
        for (u32 i = 0; i + 1u < count; ++i)
        {
            length += glm::length(points[first + i + 1u] - points[first + i]);
        }
        return length;
    }

    /// Apply a whole groom's worth of root transforms, producing a point array
    /// parallel to the groom's own. The tests' stand-in for what the ribbon
    /// build does per vertex, so an assertion is about the DEFORMATION rather
    /// than about the geometry expansion around it.
    [[nodiscard]] inline std::vector<glm::vec3> DeformAllPoints(const GroomAsset& groom,
                                                                const GroomBindingAsset& binding,
                                                                const std::vector<GroomRootTransform>& transforms,
                                                                bool previous = false)
    {
        std::vector<glm::vec3> out = groom.GetPoints();
        for (u32 curve = 0; curve < groom.GetCurveCount(); ++curve)
        {
            const GroomRootBinding& record = binding.GetRoot(curve);
            const GroomRootTransform& transform = transforms[curve];
            const u32 first = groom.GetCurveFirstPoint(curve);
            const u32 count = groom.GetCurvePointCount(curve);
            for (u32 i = 0; i < count; ++i)
            {
                out[first + i] =
                    ApplyGroomRootTransform(record, transform, groom.GetPoints()[first + i], previous);
            }
        }
        return out;
    }
} // namespace OloEngine::GroomBindingTest

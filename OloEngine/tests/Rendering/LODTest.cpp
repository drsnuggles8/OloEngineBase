#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Renderer/LOD.h"

using namespace OloEngine; // NOLINT(google-build-using-namespace) — test file, brevity preferred

// =============================================================================
// LODLevel Tests
// =============================================================================

TEST(LODLevel, DefaultConstruction)
{
	LODLevel level;
	EXPECT_EQ(level.MeshHandle, AssetHandle(0));
	EXPECT_FLOAT_EQ(level.MaxDistance, 0.0f);
	EXPECT_EQ(level.TriangleCount, 0u);
}

TEST(LODLevel, ParameterizedConstruction)
{
	LODLevel level(AssetHandle(42), 100.0f, 5000);
	EXPECT_EQ(level.MeshHandle, AssetHandle(42));
	EXPECT_FLOAT_EQ(level.MaxDistance, 100.0f);
	EXPECT_EQ(level.TriangleCount, 5000u);
}

TEST(LODLevel, ParameterizedConstructionDefaultTriangles)
{
	LODLevel level(AssetHandle(7), 50.0f);
	EXPECT_EQ(level.MeshHandle, AssetHandle(7));
	EXPECT_FLOAT_EQ(level.MaxDistance, 50.0f);
	EXPECT_EQ(level.TriangleCount, 0u);
}

// =============================================================================
// LODGroup Tests — Empty / Single Level
// =============================================================================

TEST(LODGroup, DefaultConstruction)
{
	LODGroup group;
	EXPECT_TRUE(group.Levels.empty());
	EXPECT_FLOAT_EQ(group.Bias, 1.0f);
}

TEST(LODGroup, EmptyGroupReturnsInvalid)
{
	LODGroup group;
	EXPECT_EQ(group.SelectLOD(0.0f), -1);
	EXPECT_EQ(group.SelectLOD(100.0f), -1);
	EXPECT_EQ(group.SelectLOD(-5.0f), -1);
}

TEST(LODGroup, SingleLevelAlwaysSelected)
{
	LODGroup group;
	group.Levels.emplace_back(AssetHandle(1), 100.0f, 1000);

	EXPECT_EQ(group.SelectLOD(0.0f), 0);
	EXPECT_EQ(group.SelectLOD(50.0f), 0);
	EXPECT_EQ(group.SelectLOD(100.0f), 0);
	// Beyond threshold — still returns last (lowest detail)
	EXPECT_EQ(group.SelectLOD(200.0f), 0);
}

// =============================================================================
// LODGroup Tests — Multiple Levels
// =============================================================================

TEST(LODGroup, MultipleLevelsSelectCorrectly)
{
	LODGroup group;
	group.Levels.emplace_back(AssetHandle(1), 50.0f, 10000);  // LOD 0: high detail
	group.Levels.emplace_back(AssetHandle(2), 150.0f, 2500);  // LOD 1: medium
	group.Levels.emplace_back(AssetHandle(3), 500.0f, 500);   // LOD 2: low

	// Within LOD 0 range
	EXPECT_EQ(group.SelectLOD(0.0f), 0);
	EXPECT_EQ(group.SelectLOD(25.0f), 0);
	EXPECT_EQ(group.SelectLOD(50.0f), 0);

	// Within LOD 1 range
	EXPECT_EQ(group.SelectLOD(51.0f), 1);
	EXPECT_EQ(group.SelectLOD(100.0f), 1);
	EXPECT_EQ(group.SelectLOD(150.0f), 1);

	// Within LOD 2 range
	EXPECT_EQ(group.SelectLOD(151.0f), 2);
	EXPECT_EQ(group.SelectLOD(300.0f), 2);
	EXPECT_EQ(group.SelectLOD(500.0f), 2);

	// Beyond all thresholds — returns lowest detail (last)
	EXPECT_EQ(group.SelectLOD(1000.0f), 2);
}

TEST(LODGroup, BoundaryDistancesSelectCorrectLevel)
{
	LODGroup group;
	group.Levels.emplace_back(AssetHandle(1), 100.0f);
	group.Levels.emplace_back(AssetHandle(2), 200.0f);

	// Exactly at boundary selects current level (distance <= maxDistance)
	EXPECT_EQ(group.SelectLOD(100.0f), 0);
	EXPECT_EQ(group.SelectLOD(200.0f), 1);

	// Just past boundary selects next level
	EXPECT_EQ(group.SelectLOD(100.001f), 1);
}

// =============================================================================
// LODGroup Tests — Bias Factor
// =============================================================================

TEST(LODGroup, BiasOneHasNoEffect)
{
	LODGroup group;
	group.Bias = 1.0f;
	group.Levels.emplace_back(AssetHandle(1), 100.0f);
	group.Levels.emplace_back(AssetHandle(2), 200.0f);

	EXPECT_EQ(group.SelectLOD(50.0f), 0);
	EXPECT_EQ(group.SelectLOD(150.0f), 1);
}

TEST(LODGroup, BiasLessThanOneKeepsHighDetailLonger)
{
	// Bias < 1 means effectiveDistance = distance / bias is LARGER,
	// so objects switch to lower detail sooner.
	// Wait — bias < 1: effectiveDistance = distance / 0.5 = distance * 2
	// That means a distance of 75 becomes 150, pushing to LOD 1.
	// Actually for "keeps high detail longer" we want bias > 1.
	// Let's test the actual math:

	LODGroup group;
	group.Bias = 2.0f; // effectiveDistance = distance / 2.0
	group.Levels.emplace_back(AssetHandle(1), 100.0f);
	group.Levels.emplace_back(AssetHandle(2), 200.0f);

	// At distance 150, effective = 75, which is <= 100 → LOD 0
	EXPECT_EQ(group.SelectLOD(150.0f), 0);

	// Without bias (bias=1), distance 150 would select LOD 1
	LODGroup noBias;
	noBias.Levels = group.Levels;
	noBias.Bias = 1.0f;
	EXPECT_EQ(noBias.SelectLOD(150.0f), 1);
}

TEST(LODGroup, BiasGreaterThanOneFavorsLowerDetail)
{
	LODGroup group;
	group.Bias = 0.5f; // effectiveDistance = distance / 0.5 = distance * 2
	group.Levels.emplace_back(AssetHandle(1), 100.0f);
	group.Levels.emplace_back(AssetHandle(2), 200.0f);

	// At distance 75, effective = 150, which is > 100 → LOD 1
	EXPECT_EQ(group.SelectLOD(75.0f), 1);

	// Without bias, distance 75 would select LOD 0
	LODGroup noBias;
	noBias.Levels = group.Levels;
	noBias.Bias = 1.0f;
	EXPECT_EQ(noBias.SelectLOD(75.0f), 0);
}

TEST(LODGroup, VeryHighBiasAlwaysSelectsHighestDetail)
{
	LODGroup group;
	group.Bias = 1000.0f; // effectiveDistance ≈ 0 for reasonable distances
	group.Levels.emplace_back(AssetHandle(1), 10.0f);
	group.Levels.emplace_back(AssetHandle(2), 50.0f);
	group.Levels.emplace_back(AssetHandle(3), 200.0f);

	EXPECT_EQ(group.SelectLOD(100.0f), 0);
	EXPECT_EQ(group.SelectLOD(5000.0f), 0);
}

TEST(LODGroup, VeryLowBiasAlwaysSelectsLowestDetail)
{
	LODGroup group;
	group.Bias = 0.001f; // effectiveDistance = distance * 1000
	group.Levels.emplace_back(AssetHandle(1), 100.0f);
	group.Levels.emplace_back(AssetHandle(2), 200.0f);
	group.Levels.emplace_back(AssetHandle(3), 500.0f);

	// Even at distance 1, effective = 1000 → beyond all thresholds → last
	EXPECT_EQ(group.SelectLOD(1.0f), 2);
}

// =============================================================================
// LODGroup Tests — Edge Cases
// =============================================================================

TEST(LODGroup, ZeroDistanceSelectsFirstLevel)
{
	LODGroup group;
	group.Levels.emplace_back(AssetHandle(1), 50.0f);
	group.Levels.emplace_back(AssetHandle(2), 150.0f);

	EXPECT_EQ(group.SelectLOD(0.0f), 0);
}

TEST(LODGroup, NegativeDistanceTreatedAsVeryClose)
{
	LODGroup group;
	group.Levels.emplace_back(AssetHandle(1), 50.0f);
	group.Levels.emplace_back(AssetHandle(2), 150.0f);

	// Negative distance (shouldn't happen normally) — effective distance is negative,
	// which is <= first threshold → selects LOD 0
	EXPECT_EQ(group.SelectLOD(-10.0f), 0);
}

TEST(LODGroup, VeryLargeDistanceSelectsLastLevel)
{
	LODGroup group;
	group.Levels.emplace_back(AssetHandle(1), 100.0f);
	group.Levels.emplace_back(AssetHandle(2), 500.0f);
	group.Levels.emplace_back(AssetHandle(3), 1000.0f);

	EXPECT_EQ(group.SelectLOD(999999.0f), 2);
}

TEST(LODGroup, AllLevelsSameDistanceSelectsFirst)
{
	LODGroup group;
	group.Levels.emplace_back(AssetHandle(1), 100.0f, 10000);
	group.Levels.emplace_back(AssetHandle(2), 100.0f, 5000);
	group.Levels.emplace_back(AssetHandle(3), 100.0f, 1000);

	// At distance 100, first level matches (distance <= 100)
	EXPECT_EQ(group.SelectLOD(100.0f), 0);
	// Beyond all (same) thresholds → last
	EXPECT_EQ(group.SelectLOD(101.0f), 2);
}

TEST(LODGroup, ManyLevelsCorrectSelection)
{
	LODGroup group;
	for (i32 i = 0; i < 8; ++i)
	{
		group.Levels.emplace_back(AssetHandle(i + 1), static_cast<f32>((i + 1) * 50));
	}
	// Levels at 50, 100, 150, 200, 250, 300, 350, 400

	EXPECT_EQ(group.SelectLOD(25.0f), 0);
	EXPECT_EQ(group.SelectLOD(75.0f), 1);
	EXPECT_EQ(group.SelectLOD(125.0f), 2);
	EXPECT_EQ(group.SelectLOD(375.0f), 7);
	EXPECT_EQ(group.SelectLOD(401.0f), 7); // Beyond all → last
}

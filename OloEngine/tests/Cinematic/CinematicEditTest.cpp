#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Cinematic/CinematicEdit.h"
#include "OloEngine/Cinematic/CinematicCurve.h"
#include "OloEngine/Cinematic/CinematicTrack.h"

#include <limits>
#include <vector>

// =============================================================================
// CinematicEditTest — unit tests for the pure keyframe-editing operations the
// timeline panel mutates its tracks with. The single contract under test is the
// sort-by-Time-ascending invariant that CinematicCurve / visibility / event
// evaluation depends on: every insert, drag (MoveKeyTime), and delete must leave
// the key vector sorted, and MoveKeyTime must report where the key ended up so
// editor selection can follow it. No Scene / GL involved.
// =============================================================================

using namespace OloEngine;

namespace
{
    constexpr f32 kEps = 1e-6f;

    // Build a float channel from a plain list of times (values mirror times so a
    // key is identifiable after a move).
    std::vector<CinematicFloatKey> FloatKeys(std::initializer_list<f32> times)
    {
        std::vector<CinematicFloatKey> keys;
        for (f32 t : times)
        {
            keys.push_back({ t, t * 10.0f, CinematicInterp::Linear });
        }
        return keys;
    }
} // namespace

// ------------------------------- InsertKeySorted -----------------------------

TEST(CinematicEditTest, InsertIntoEmptyReturnsZero)
{
    std::vector<CinematicFloatKey> keys;
    const sizet idx = CinematicEdit::InsertKeySorted(keys, CinematicFloatKey{ 2.0f, 20.0f, CinematicInterp::Linear });
    EXPECT_EQ(idx, 0u);
    ASSERT_EQ(keys.size(), 1u);
    EXPECT_NEAR(keys[0].Time, 2.0f, kEps);
}

TEST(CinematicEditTest, InsertKeepsAscendingOrder)
{
    auto keys = FloatKeys({ 0.0f, 1.0f, 3.0f });
    const sizet idx = CinematicEdit::InsertKeySorted(keys, CinematicFloatKey{ 2.0f, 99.0f, CinematicInterp::Linear });
    EXPECT_EQ(idx, 2u); // lands between 1.0 and 3.0
    ASSERT_EQ(keys.size(), 4u);
    EXPECT_NEAR(keys[0].Time, 0.0f, kEps);
    EXPECT_NEAR(keys[1].Time, 1.0f, kEps);
    EXPECT_NEAR(keys[2].Time, 2.0f, kEps);
    EXPECT_NEAR(keys[3].Time, 3.0f, kEps);
    EXPECT_TRUE(CinematicEdit::IsSortedByTime(keys));
}

TEST(CinematicEditTest, InsertAtFrontAndBack)
{
    auto keys = FloatKeys({ 1.0f, 2.0f });
    EXPECT_EQ(CinematicEdit::InsertKeySorted(keys, CinematicFloatKey{ 0.0f }), 0u);
    // Sequence the back-insert before reading size() — EXPECT_EQ does not order
    // its argument evaluations.
    const sizet backIdx = CinematicEdit::InsertKeySorted(keys, CinematicFloatKey{ 5.0f });
    EXPECT_EQ(backIdx, keys.size() - 1);
    EXPECT_TRUE(CinematicEdit::IsSortedByTime(keys));
}

TEST(CinematicEditTest, InsertTieGoesAfterExisting)
{
    // Two keys already at t==1; the inserted one must land after both (upper_bound),
    // never reordering the equal-time neighbours.
    std::vector<CinematicFloatKey> keys;
    keys.push_back({ 1.0f, 10.0f, CinematicInterp::Linear });
    keys.push_back({ 1.0f, 11.0f, CinematicInterp::Constant });
    const sizet idx = CinematicEdit::InsertKeySorted(keys, CinematicFloatKey{ 1.0f, 12.0f, CinematicInterp::EaseInOut });
    EXPECT_EQ(idx, 2u);
    EXPECT_NEAR(keys[0].Value, 10.0f, kEps);
    EXPECT_NEAR(keys[1].Value, 11.0f, kEps);
    EXPECT_NEAR(keys[2].Value, 12.0f, kEps);
}

// ------------------------------- MoveKeyTime ---------------------------------

TEST(CinematicEditTest, MoveKeyForwardReindexesAndResorts)
{
    auto keys = FloatKeys({ 0.0f, 1.0f, 2.0f, 3.0f }); // values 0,10,20,30
    // Drag key[1] (value 10) from t=1 to t=2.5 -> should sit between 2.0 and 3.0.
    const sizet newIdx = CinematicEdit::MoveKeyTime(keys, 1, 2.5f);
    EXPECT_EQ(newIdx, 2u);
    EXPECT_NEAR(keys[newIdx].Value, 10.0f, kEps); // identity preserved
    EXPECT_NEAR(keys[newIdx].Time, 2.5f, kEps);
    EXPECT_TRUE(CinematicEdit::IsSortedByTime(keys));
    EXPECT_EQ(keys.size(), 4u);
}

TEST(CinematicEditTest, MoveKeyBackwardReindexes)
{
    auto keys = FloatKeys({ 0.0f, 1.0f, 2.0f, 3.0f });
    const sizet newIdx = CinematicEdit::MoveKeyTime(keys, 3, 0.5f); // value 30 -> t=0.5
    EXPECT_EQ(newIdx, 1u);
    EXPECT_NEAR(keys[newIdx].Value, 30.0f, kEps);
    EXPECT_TRUE(CinematicEdit::IsSortedByTime(keys));
}

TEST(CinematicEditTest, MoveKeyClampsNegativeTimeToZero)
{
    auto keys = FloatKeys({ 0.0f, 1.0f, 2.0f });
    const sizet newIdx = CinematicEdit::MoveKeyTime(keys, 2, -5.0f); // value 20 -> clamped 0
    EXPECT_NEAR(keys[newIdx].Time, 0.0f, kEps);
    EXPECT_NEAR(keys[newIdx].Value, 20.0f, kEps);
    EXPECT_TRUE(CinematicEdit::IsSortedByTime(keys));
}

TEST(CinematicEditTest, MoveKeyRejectsNonFiniteTime)
{
    auto keys = FloatKeys({ 0.0f, 1.0f, 2.0f });
    const f32 before = keys[1].Time;
    const sizet idx = CinematicEdit::MoveKeyTime(keys, 1, std::numeric_limits<f32>::quiet_NaN());
    EXPECT_EQ(idx, 1u);             // unchanged
    EXPECT_NEAR(keys[1].Time, before, kEps);
    EXPECT_TRUE(CinematicEdit::IsSortedByTime(keys));
}

TEST(CinematicEditTest, MoveKeyOutOfRangeIsNoOp)
{
    auto keys = FloatKeys({ 0.0f, 1.0f });
    const sizet idx = CinematicEdit::MoveKeyTime(keys, 7, 0.5f);
    EXPECT_EQ(idx, 7u);
    EXPECT_EQ(keys.size(), 2u);
}

// ------------------------------- RemoveKeyAt ---------------------------------

TEST(CinematicEditTest, RemoveKeyErasesAndKeepsOrder)
{
    auto keys = FloatKeys({ 0.0f, 1.0f, 2.0f });
    EXPECT_TRUE(CinematicEdit::RemoveKeyAt(keys, 1));
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_NEAR(keys[0].Time, 0.0f, kEps);
    EXPECT_NEAR(keys[1].Time, 2.0f, kEps);
    EXPECT_TRUE(CinematicEdit::IsSortedByTime(keys));
}

TEST(CinematicEditTest, RemoveKeyOutOfRangeReturnsFalse)
{
    auto keys = FloatKeys({ 0.0f, 1.0f });
    EXPECT_FALSE(CinematicEdit::RemoveKeyAt(keys, 5));
    EXPECT_EQ(keys.size(), 2u);
}

// ------------------- Works across the other key types ------------------------

TEST(CinematicEditTest, WorksOnVisibilityKeys)
{
    std::vector<CinematicVisibilityKey> keys;
    CinematicEdit::InsertKeySorted(keys, CinematicVisibilityKey{ 2.0f, false });
    CinematicEdit::InsertKeySorted(keys, CinematicVisibilityKey{ 0.0f, true });
    const sizet moved = CinematicEdit::MoveKeyTime(keys, 1, -1.0f); // the t=2 key to clamped 0
    EXPECT_TRUE(CinematicEdit::IsSortedByTime(keys));
    EXPECT_GE(moved, 0u);
    EXPECT_EQ(keys.size(), 2u);
}

TEST(CinematicEditTest, WorksOnEventKeys)
{
    std::vector<CinematicEventKey> keys;
    CinematicEdit::InsertKeySorted(keys, CinematicEventKey{ 1.0f, "mid" });
    CinematicEdit::InsertKeySorted(keys, CinematicEventKey{ 0.0f, "start" });
    CinematicEdit::InsertKeySorted(keys, CinematicEventKey{ 2.0f, "end" });
    ASSERT_EQ(keys.size(), 3u);
    EXPECT_EQ(keys[0].Name, "start");
    EXPECT_EQ(keys[1].Name, "mid");
    EXPECT_EQ(keys[2].Name, "end");
    EXPECT_TRUE(CinematicEdit::RemoveKeyAt(keys, 1));
    EXPECT_EQ(keys[1].Name, "end");
}

TEST(CinematicEditTest, WorksOnQuatKeys)
{
    std::vector<CinematicQuatKey> keys;
    CinematicEdit::InsertKeySorted(keys, CinematicQuatKey{ 3.0f });
    CinematicEdit::InsertKeySorted(keys, CinematicQuatKey{ 1.0f });
    const sizet idx = CinematicEdit::InsertKeySorted(keys, CinematicQuatKey{ 2.0f });
    EXPECT_EQ(idx, 1u);
    EXPECT_TRUE(CinematicEdit::IsSortedByTime(keys));
}

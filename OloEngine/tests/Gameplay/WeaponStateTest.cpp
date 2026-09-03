// OLO_TEST_LAYER: unit

#include "OloEnginePCH.h"
#include <gtest/gtest.h>

#include "OloEngine/Gameplay/Combat/WeaponState.h"
#include "OloEngine/Gameplay/Combat/WeaponDamage.h"
#include "OloEngine/Gameplay/Combat/CombatComponents.h"
#include "OloEngine/SaveGame/SaveGameComponentSerializer.h"
#include "OloEngine/Serialization/Archive.h"

#include <limits>
#include <vector>

namespace OloEngine::Tests
{
    TEST(WeaponState, FiringConsumesOneRoundAndCadenceBlocksTheNextShot)
    {
        WeaponDefinition definition;
        definition.MagazineSize = 3;
        definition.ReserveAmmo = 9;
        definition.RoundsPerMinute = 600.0f;

        WeaponState state = WeaponState::Loaded(definition);

        EXPECT_EQ(state.TryFire(definition), WeaponFireResult::Fired);
        EXPECT_EQ(state.GetMagazineAmmo(), 2);
        EXPECT_EQ(state.TryFire(definition), WeaponFireResult::CadenceBlocked);

        state.Advance(definition, 0.1f);

        EXPECT_EQ(state.TryFire(definition), WeaponFireResult::Fired);
        EXPECT_EQ(state.GetMagazineAmmo(), 1);
    }

    TEST(WeaponState, ReloadTransfersReserveAmmoAfterTheReloadDuration)
    {
        WeaponDefinition definition;
        definition.MagazineSize = 5;
        definition.ReserveAmmo = 2;
        definition.RoundsPerMinute = 600.0f;
        definition.ReloadSeconds = 1.0f;

        WeaponState state = WeaponState::Loaded(definition);
        EXPECT_EQ(state.TryFire(definition), WeaponFireResult::Fired);
        state.Advance(definition, 0.1f);

        EXPECT_EQ(state.BeginReload(definition), WeaponReloadResult::Started);
        EXPECT_EQ(state.TryFire(definition), WeaponFireResult::Reloading);
        state.Advance(definition, 0.75f);
        EXPECT_EQ(state.GetMagazineAmmo(), 4);
        state.Advance(definition, 0.25f);

        EXPECT_EQ(state.GetMagazineAmmo(), 5);
        EXPECT_EQ(state.GetReserveAmmo(), 1);
        EXPECT_EQ(state.TryFire(definition), WeaponFireResult::Fired);
    }

    TEST(WeaponState, ElapsedCadenceDebtRemainsAvailableForCatchUpShots)
    {
        WeaponDefinition definition;
        definition.MagazineSize = 4;
        definition.RoundsPerMinute = 600.0f;

        WeaponState state = WeaponState::Loaded(definition);
        ASSERT_EQ(state.TryFire(definition), WeaponFireResult::Fired);
        state.Advance(definition, 0.25f, true);

        EXPECT_EQ(state.TryFire(definition), WeaponFireResult::Fired);
        EXPECT_EQ(state.TryFire(definition), WeaponFireResult::Fired);
        EXPECT_EQ(state.TryFire(definition), WeaponFireResult::CadenceBlocked);
        EXPECT_EQ(state.GetShotsFired(), 3u);
    }

    TEST(WeaponState, IdleTimeDoesNotCreateCadenceDebt)
    {
        WeaponDefinition definition;
        definition.MagazineSize = 3;
        definition.RoundsPerMinute = 600.0f;

        WeaponState state = WeaponState::Loaded(definition);
        state.Advance(definition, 1.0f);

        EXPECT_EQ(state.TryFire(definition), WeaponFireResult::Fired);
        EXPECT_EQ(state.TryFire(definition), WeaponFireResult::CadenceBlocked);
    }

    TEST(WeaponComponentSaveLoadSanitize, NonFiniteMuzzleOffsetFallsBackToZero)
    {
        WeaponComponent seed;
        seed.m_MuzzleOffset = { 1.0f, std::numeric_limits<f32>::quiet_NaN(), 3.0f };

        std::vector<u8> buffer;
        FMemoryWriter writer(buffer);
        writer.ArIsSaveGame = true;
        SaveGameComponentSerializer::Serialize(writer, seed);

        WeaponComponent loaded;
        FMemoryReader reader(buffer);
        reader.ArIsSaveGame = true;
        SaveGameComponentSerializer::Serialize(reader, loaded);

        ASSERT_FALSE(reader.IsError());
        EXPECT_TRUE(reader.AtEnd());
        EXPECT_FLOAT_EQ(loaded.m_MuzzleOffset.x, 0.0f);
        EXPECT_FLOAT_EQ(loaded.m_MuzzleOffset.y, 0.0f);
        EXPECT_FLOAT_EQ(loaded.m_MuzzleOffset.z, 0.0f);
    }

    TEST(WeaponDamage, FalloffInterpolatesAndClampsAtTheAuthoredDistances)
    {
        WeaponDefinition definition;
        definition.Damage = 24.0f;
        definition.FalloffStart = 50.0f;
        definition.FalloffEnd = 100.0f;
        definition.MinimumDamageMultiplier = 0.5f;

        EXPECT_FLOAT_EQ(ComputeWeaponDamage(definition, 25.0f), 24.0f);
        EXPECT_FLOAT_EQ(ComputeWeaponDamage(definition, 75.0f), 18.0f);
        EXPECT_FLOAT_EQ(ComputeWeaponDamage(definition, 125.0f), 12.0f);
    }
} // namespace OloEngine::Tests

#include <gtest/gtest.h>

#include "OloEngine/SaveGame/SaveGameManager.h"
#include "OloEngine/SaveGame/SaveGameTypes.h"

#include <filesystem>

using namespace OloEngine;

// ========================================================================
// SaveGameManager unit tests (no active project/scene — tests utilities)
// ========================================================================

TEST(SaveGameManagerTest, AutoSaveInterval)
{
    // Preserve original interval
    f32 original = SaveGameManager::GetAutoSaveInterval();

    SaveGameManager::SetAutoSaveInterval(120.0f);
    EXPECT_FLOAT_EQ(SaveGameManager::GetAutoSaveInterval(), 120.0f);

    SaveGameManager::SetAutoSaveInterval(0.0f);
    EXPECT_FLOAT_EQ(SaveGameManager::GetAutoSaveInterval(), 0.0f);

    // Restore
    SaveGameManager::SetAutoSaveInterval(original);
}

// GetSaveFilePath requires an active project, so we only test utility functions here

// ========================================================================
// ISaveable Registry
// ========================================================================

#include "OloEngine/SaveGame/ISaveable.h"
#include "OloEngine/Serialization/ArchiveExtensions.h"

TEST(SaveableRegistryTest, RegisterAndInvoke)
{
    SaveableRegistry::Clear();

    bool called = false;
    SaveableRegistry::Register("TestClass", [&called](FArchive& ar, UUID entityID)
                               { called = true; });

    EXPECT_TRUE(SaveableRegistry::Has("TestClass"));
    EXPECT_FALSE(SaveableRegistry::Has("OtherClass"));

    std::vector<u8> buf;
    FMemoryWriter writer(buf);
    writer.ArIsSaveGame = true;
    SaveableRegistry::Invoke("TestClass", writer, UUID());
    EXPECT_TRUE(called);

    SaveableRegistry::Unregister("TestClass");
    EXPECT_FALSE(SaveableRegistry::Has("TestClass"));

    SaveableRegistry::Clear();
}

TEST(SaveableRegistryTest, InvokeNonExistent)
{
    SaveableRegistry::Clear();

    // Should not crash
    std::vector<u8> buf;
    FMemoryWriter writer(buf);
    SaveableRegistry::Invoke("NonExistent", writer, UUID());
}

TEST(SaveableRegistryTest, RoundTripWithData)
{
    SaveableRegistry::Clear();

    // Register a saveable that writes/reads an int via captured value
    i32 savedValue = 42;
    SaveableRegistry::Register("IntSaveable", [&savedValue](FArchive& ar, UUID entityID)
                               { ar << savedValue; });

    // Save
    std::vector<u8> saveData;
    {
        FMemoryWriter writer(saveData);
        writer.ArIsSaveGame = true;
        SaveableRegistry::Invoke("IntSaveable", writer, UUID());
    }
    EXPECT_GT(saveData.size(), 0u);

    // Modify to verify load overwrites it
    savedValue = 0;

    // Load
    {
        FMemoryReader reader(saveData);
        reader.ArIsSaveGame = true;
        SaveableRegistry::Invoke("IntSaveable", reader, UUID());
    }
    EXPECT_EQ(savedValue, 42);

    SaveableRegistry::Clear();
}

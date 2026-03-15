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
    SaveGameManager::SetAutoSaveInterval(120.0f);
    EXPECT_FLOAT_EQ(SaveGameManager::GetAutoSaveInterval(), 120.0f);

    SaveGameManager::SetAutoSaveInterval(0.0f);
    EXPECT_FLOAT_EQ(SaveGameManager::GetAutoSaveInterval(), 0.0f);
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
    SaveableRegistry::Register("TestClass", [&called](FArchive& ar)
    {
        called = true;
    });

    EXPECT_TRUE(SaveableRegistry::Has("TestClass"));
    EXPECT_FALSE(SaveableRegistry::Has("OtherClass"));

    std::vector<u8> buf;
    FMemoryWriter writer(buf);
    writer.ArIsSaveGame = true;
    SaveableRegistry::Invoke("TestClass", writer);
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
    SaveableRegistry::Invoke("NonExistent", writer);
}

TEST(SaveableRegistryTest, RoundTripWithData)
{
    SaveableRegistry::Clear();

    // Register a saveable that writes/reads an int
    SaveableRegistry::Register("IntSaveable", [](FArchive& ar)
    {
        static i32 value = 42;
        ar << value;
    });

    // Save
    std::vector<u8> saveData;
    {
        FMemoryWriter writer(saveData);
        writer.ArIsSaveGame = true;
        SaveableRegistry::Invoke("IntSaveable", writer);
    }
    EXPECT_GT(saveData.size(), 0u);

    // Load
    {
        FMemoryReader reader(saveData);
        reader.ArIsSaveGame = true;
        SaveableRegistry::Invoke("IntSaveable", reader);
    }

    SaveableRegistry::Clear();
}

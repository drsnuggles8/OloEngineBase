#include <gtest/gtest.h>

#include "OloEngine/Core/Application.h"
#include "OloEngine/Server/ServerConfig.h"
#include "OloEngine/Server/ServerConfigSerializer.h"
#include "OloEngine/Server/ServerConsole.h"

#include <filesystem>
#include <fstream>

using namespace OloEngine;

// ============================================================================
// ApplicationSpecification headless flag
// ============================================================================

TEST(HeadlessMode, ApplicationSpecDefaultsToNonHeadless)
{
    ApplicationSpecification spec;
    EXPECT_FALSE(spec.IsHeadless);
}

TEST(HeadlessMode, ApplicationSpecHeadlessFlag)
{
    ApplicationSpecification spec;
    spec.IsHeadless = true;
    EXPECT_TRUE(spec.IsHeadless);
}

// ============================================================================
// ServerConfig defaults
// ============================================================================

TEST(ServerConfig, DefaultValues)
{
    ServerConfig config;
    EXPECT_EQ(config.Port, 7777);
    EXPECT_EQ(config.MaxPlayers, 64u);
    EXPECT_EQ(config.TickRate, 60u);
    EXPECT_TRUE(config.ScenePath.empty());
    EXPECT_TRUE(config.Password.empty());
    EXPECT_EQ(config.LogLevel, "Info");
    EXPECT_EQ(config.AutoSaveInterval, 300u);
}

// ============================================================================
// ServerConfigSerializer - command line parsing
// ============================================================================

TEST(ServerConfigSerializer, ParseEmptyCommandLine)
{
    char* argv[] = { const_cast<char*>("server") };
    ServerConfig config = ServerConfigSerializer::ParseCommandLine(1, argv);
    EXPECT_EQ(config.Port, 7777); // defaults
}

TEST(ServerConfigSerializer, ParsePortFlag)
{
    char* argv[] = { const_cast<char*>("server"), const_cast<char*>("--port"), const_cast<char*>("9999") };
    ServerConfig config = ServerConfigSerializer::ParseCommandLine(3, argv);
    EXPECT_EQ(config.Port, 9999);
}

TEST(ServerConfigSerializer, ParseMultipleFlags)
{
    char* argv[] = {
        const_cast<char*>("server"),
        const_cast<char*>("--port"), const_cast<char*>("8888"),
        const_cast<char*>("--max-players"), const_cast<char*>("32"),
        const_cast<char*>("--tick-rate"), const_cast<char*>("30"),
        const_cast<char*>("--scene"), const_cast<char*>("Scenes/Test.oloscene"),
    };
    ServerConfig config = ServerConfigSerializer::ParseCommandLine(9, argv);
    EXPECT_EQ(config.Port, 8888);
    EXPECT_EQ(config.MaxPlayers, 32u);
    EXPECT_EQ(config.TickRate, 30u);
    EXPECT_EQ(config.ScenePath, "Scenes/Test.oloscene");
}

// ============================================================================
// ServerConfigSerializer - YAML load/save round-trip
// ============================================================================

TEST(ServerConfigSerializer, YAMLRoundTrip)
{
    const std::string testFile = "test_server_config.yaml";

    // Create config
    ServerConfig original;
    original.Port = 1234;
    original.MaxPlayers = 16;
    original.TickRate = 30;
    original.ScenePath = "Scenes/World.oloscene";
    original.Password = "testpass";
    original.LogLevel = "Debug";
    original.AutoSaveInterval = 600;

    // Save
    ServerConfigSerializer::SaveToFile(original, testFile);
    ASSERT_TRUE(std::filesystem::exists(testFile));

    // Load
    ServerConfig loaded = ServerConfigSerializer::LoadFromFile(testFile);

    EXPECT_EQ(loaded.Port, original.Port);
    EXPECT_EQ(loaded.MaxPlayers, original.MaxPlayers);
    EXPECT_EQ(loaded.TickRate, original.TickRate);
    EXPECT_EQ(loaded.ScenePath, original.ScenePath);
    EXPECT_EQ(loaded.Password, original.Password);
    EXPECT_EQ(loaded.LogLevel, original.LogLevel);
    EXPECT_EQ(loaded.AutoSaveInterval, original.AutoSaveInterval);

    // Cleanup
    std::filesystem::remove(testFile);
}

TEST(ServerConfigSerializer, LoadMissingFileReturnsDefaults)
{
    ServerConfig config = ServerConfigSerializer::LoadFromFile("nonexistent_config.yaml");
    EXPECT_EQ(config.Port, 7777);
    EXPECT_EQ(config.MaxPlayers, 64u);
}

TEST(ServerConfigSerializer, CommandLineOverridesConfigFile)
{
    const std::string testFile = "test_override_config.yaml";

    // Save a config file with port 1234
    ServerConfig fileConfig;
    fileConfig.Port = 1234;
    fileConfig.MaxPlayers = 8;
    ServerConfigSerializer::SaveToFile(fileConfig, testFile);

    // Command line overrides port to 9999
    char* argv[] = {
        const_cast<char*>("server"),
        const_cast<char*>("--config"), const_cast<char*>("test_override_config.yaml"),
        const_cast<char*>("--port"), const_cast<char*>("9999"),
    };
    ServerConfig config = ServerConfigSerializer::ParseCommandLine(5, argv);

    EXPECT_EQ(config.Port, 9999);      // overridden
    EXPECT_EQ(config.MaxPlayers, 8u);   // from file

    std::filesystem::remove(testFile);
}

// ============================================================================
// ServerConsole - basic functionality
// ============================================================================

TEST(ServerConsole, RegisterAndHasCommands)
{
    ServerConsole console;
    console.Initialize();

    // After initialization, built-in commands should be registered
    // We can't easily test ProcessInput without stdin, but we verify it doesn't crash
    console.Shutdown();
}

TEST(ServerConsole, RegisterCustomCommand)
{
    ServerConsole console;
    console.Initialize();

    bool called = false;
    console.RegisterCommand("test", [&called](const std::vector<std::string>&) { called = true; });

    // The command is registered; actual dispatch requires stdin input
    console.Shutdown();
}

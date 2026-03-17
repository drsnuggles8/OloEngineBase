#include <gtest/gtest.h>

#include "OloEngine/Core/Application.h"
#include "OloEngine/Server/ServerConfig.h"
#include "OloEngine/Server/ServerConfigSerializer.h"
#include "OloEngine/Server/ServerConsole.h"
#include "OloEngine/Server/ServerMonitor.h"

#include <filesystem>
#include <fstream>
#include <process.h>
#include <sstream>

using namespace OloEngine;

// Helper to generate a unique temp filename using PID to avoid parallel test collisions
static std::filesystem::path UniqueTempPath(const char* baseName)
{
    std::ostringstream oss;
    oss << baseName << "_" << _getpid() << ".yaml";
    return std::filesystem::temp_directory_path() / oss.str();
}

// RAII guard that removes a file on destruction
struct TempFileGuard
{
    std::filesystem::path Path;

    explicit TempFileGuard(std::filesystem::path p) : Path(std::move(p)) {}

    ~TempFileGuard()
    {
        std::error_code ec;
        std::filesystem::remove(Path, ec);
    }

    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;
};

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
        const_cast<char*>("--port"),
        const_cast<char*>("8888"),
        const_cast<char*>("--max-players"),
        const_cast<char*>("32"),
        const_cast<char*>("--tick-rate"),
        const_cast<char*>("30"),
        const_cast<char*>("--scene"),
        const_cast<char*>("Scenes/Test.oloscene"),
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
    auto testPath = UniqueTempPath("olotest_roundtrip");
    TempFileGuard guard(testPath);
    const std::string testFile = testPath.string();

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
}

TEST(ServerConfigSerializer, LoadMissingFileReturnsDefaults)
{
    ServerConfig config = ServerConfigSerializer::LoadFromFile("nonexistent_config.yaml");
    EXPECT_EQ(config.Port, 7777);
    EXPECT_EQ(config.MaxPlayers, 64u);
}

TEST(ServerConfigSerializer, CommandLineOverridesConfigFile)
{
    auto testPath = UniqueTempPath("olotest_override");
    TempFileGuard guard(testPath);
    const std::string testFile = testPath.string();

    // Save a config file with port 1234
    ServerConfig fileConfig;
    fileConfig.Port = 1234;
    fileConfig.MaxPlayers = 8;
    ServerConfigSerializer::SaveToFile(fileConfig, testFile);

    // Command line overrides port to 9999
    std::string configArg = testFile;
    char* argv[] = {
        const_cast<char*>("server"),
        const_cast<char*>("--config"),
        const_cast<char*>(configArg.c_str()),
        const_cast<char*>("--port"),
        const_cast<char*>("9999"),
    };
    ServerConfig config = ServerConfigSerializer::ParseCommandLine(5, argv);

    EXPECT_EQ(config.Port, 9999);     // overridden
    EXPECT_EQ(config.MaxPlayers, 8u); // from file
}

// ============================================================================
// ServerConsole - basic functionality
// ============================================================================

TEST(ServerConsole, ImplementsIConsoleInterface)
{
    ServerConsole console;
    IConsole* iface = &console;

    // Verify the IConsole interface is satisfied
    EXPECT_NE(iface, nullptr);
}

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

    bool called = false;
    console.RegisterCommand("test", [&called](const std::vector<std::string>&)
                            { called = true; });

    // Inject input and process to trigger the handler (no stdin thread needed)
    console.InjectInput("test");
    console.ProcessInput();
    EXPECT_TRUE(called);
}

TEST(ServerConsole, MessageSendCallback)
{
    ServerConsole console;

    std::string lastMessage;
    console.SetMessageSendCallback([&lastMessage](const std::string& msg)
                                   { lastMessage = msg; });

    // Inject input — the callback receives the raw line before command dispatch
    console.InjectInput("hello world");
    console.ProcessInput();
    EXPECT_EQ(lastMessage, "hello world");
}

// ============================================================================
// ApplicationSpecification - configurable tick rate
// ============================================================================

TEST(HeadlessMode, DefaultTickRate)
{
    ApplicationSpecification spec;
    EXPECT_EQ(spec.HeadlessTickRate, 60u);
}

TEST(HeadlessMode, CustomTickRate)
{
    ApplicationSpecification spec;
    spec.HeadlessTickRate = 30;
    EXPECT_EQ(spec.HeadlessTickRate, 30u);
}

TEST(HeadlessMode, TickRateFromServerConfig)
{
    char* argv[] = {
        const_cast<char*>("server"),
        const_cast<char*>("--tick-rate"),
        const_cast<char*>("128"),
    };
    ServerConfig config = ServerConfigSerializer::ParseCommandLine(3, argv);
    EXPECT_EQ(config.TickRate, 128u);

    // Verify it would map to ApplicationSpecification
    ApplicationSpecification spec;
    spec.HeadlessTickRate = config.TickRate;
    EXPECT_EQ(spec.HeadlessTickRate, 128u);
}

// ============================================================================
// ServerMonitor - basic functionality
// ============================================================================

TEST(ServerMonitor, DefaultConstruction)
{
    ServerMonitor monitor;
    // Should not crash on construction or immediate report
    monitor.ForceReport();
}

TEST(ServerMonitor, RecordTicksWithoutCrash)
{
    ServerMonitor monitor(1000.0f, 0.020f); // 20ms budget
    for (int i = 0; i < 100; ++i)
    {
        monitor.RecordTick(0.016f); // ~60 Hz, under budget
    }
    monitor.ForceReport();
}

TEST(ServerMonitor, CustomReportInterval)
{
    ServerMonitor monitor(10.0f);
    monitor.SetReportInterval(5.0f);
    monitor.SetTickBudget(0.033f);
    // No crash; interval/budget change is accepted
    monitor.RecordTick(0.016f);
}

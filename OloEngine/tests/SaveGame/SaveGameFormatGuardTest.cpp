// OLO_TEST_LAYER: unit
#include "OloEnginePCH.h"
#include <gtest/gtest.h>
#include "TestTempDir.h"

#include "OloEngine/Core/Hash.h"
#include "OloEngine/Core/Ref.h"
#include "OloEngine/Project/Project.h"
#include "OloEngine/SaveGame/SaveGameFile.h"
#include "OloEngine/SaveGame/SaveGameComponentSerializer.h"
#include "OloEngine/SaveGame/SaveGameManager.h"
#include "OloEngine/SaveGame/SaveGameSerializer.h"
#include "OloEngine/SaveGame/SaveGameTypes.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Entity.h"
#include "OloEngine/Scene/Scene.h"
#include "OloEngine/Serialization/Archive.h"
#include "OloEngine/Serialization/ArchiveExtensions.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

// The save game reads exactly one format version, and a short or malformed
// payload fails instead of being read as an older layout (#1498). Before, the
// per-component reads probed AtEnd() to decide whether a trailing field was
// present, so a block cut short inside such a field loaded with defaults, and
// the scene-settings section had no framing at all.

using namespace OloEngine;

namespace
{
    // A scene whose capture has at least one settings block and one component
    // block with trailing fields: a transform and a directional light.
    Ref<Scene> MakeScene()
    {
        Ref<Scene> scene = Ref<Scene>::Create();
        Entity e = scene->CreateEntity("Guarded");
        e.GetComponent<TransformComponent>().Translation = { 1.0f, 2.0f, 3.0f };
        e.AddComponent<DirectionalLightComponent>();
        return scene;
    }

    u32 ReadU32(const std::vector<u8>& data, sizet offset)
    {
        u32 value = 0;
        std::memcpy(&value, data.data() + offset, sizeof(value));
        return value;
    }

    void WriteU32(std::vector<u8>& data, sizet offset, u32 value)
    {
        std::memcpy(data.data() + offset, &value, sizeof(value));
    }

    // Offset of the {typeHash, byteCount} header of the first block carrying
    // this type hash, or npos.
    sizet FindComponentBlock(const std::vector<u8>& data, u32 typeHash)
    {
        for (sizet i = 0; i + 8 <= data.size(); ++i)
        {
            if (ReadU32(data, i) == typeHash)
                return i;
        }
        return std::string::npos;
    }
} // namespace

TEST(SaveGameFormatGuard, TheIntactPayloadRestores)
{
    // Positive control for every case below: the unmodified capture loads.
    Ref<Scene> source = MakeScene();
    const std::vector<u8> payload = SaveGameSerializer::CaptureSceneState(*source);
    Ref<Scene> target = Ref<Scene>::Create();
    EXPECT_TRUE(SaveGameSerializer::RestoreSceneState(*target, payload));
}

TEST(SaveGameFormatGuard, EveryTruncationOfThePayloadFails)
{
    Ref<Scene> source = MakeScene();
    const std::vector<u8> payload = SaveGameSerializer::CaptureSceneState(*source);
    ASSERT_GT(payload.size(), 64u);

    // Every proper prefix: the cut lands inside the settings section, inside a
    // component block, between blocks and inside the entity trailer in turn.
    for (sizet length = 0; length < payload.size(); ++length)
    {
        SCOPED_TRACE(length);
        const std::vector<u8> truncated(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(length));
        Ref<Scene> target = Ref<Scene>::Create();
        EXPECT_FALSE(SaveGameSerializer::RestoreSceneState(*target, truncated));
    }
}

TEST(SaveGameFormatGuard, ASettingsBlockLongerThanItsReaderFails)
{
    // The first settings block follows the 4-byte "SETS" marker. Grow it by one
    // byte and declare the new length: the stream stays in sync, but the
    // reader consumes one byte less than the block holds, which is exactly a
    // writer/reader disagreement by one field.
    Ref<Scene> source = MakeScene();
    std::vector<u8> payload = SaveGameSerializer::CaptureSceneState(*source);
    constexpr sizet kFirstBlock = 4;
    const u32 byteCount = ReadU32(payload, kFirstBlock);
    ASSERT_GT(byteCount, 0u);
    WriteU32(payload, kFirstBlock, byteCount + 1);
    payload.insert(payload.begin() + static_cast<std::ptrdiff_t>(kFirstBlock + 4 + byteCount), u8{ 0 });

    Ref<Scene> target = Ref<Scene>::Create();
    EXPECT_FALSE(SaveGameSerializer::RestoreSceneState(*target, payload));
}

TEST(SaveGameFormatGuard, AComponentBlockCutShortInsideTheBlockFails)
{
    // Drop the last byte of the directional light's payload and declare the
    // shorter length, so the framing is consistent and only the component's
    // own read runs out. This is the case AtEnd() probes used to load with
    // defaults.
    Ref<Scene> source = MakeScene();
    std::vector<u8> payload = SaveGameSerializer::CaptureSceneState(*source);
    const sizet block = FindComponentBlock(payload, Hash::GenerateFNVHash("DirectionalLightComponent"));
    ASSERT_NE(block, std::string::npos);
    const u32 byteCount = ReadU32(payload, block + 4);
    ASSERT_GT(byteCount, 4u);
    WriteU32(payload, block + 4, byteCount - 1);
    payload.erase(payload.begin() + static_cast<std::ptrdiff_t>(block + 8 + byteCount - 1));

    Ref<Scene> target = Ref<Scene>::Create();
    EXPECT_FALSE(SaveGameSerializer::RestoreSceneState(*target, payload));
}

TEST(SaveGameFormatGuard, AFileFromAnyOtherFormatVersionIsRejectedAtTheHeader)
{
    const std::filesystem::path path = OloEngine::Tests::TempFile("guard.olosave");
    Ref<Scene> source = MakeScene();
    const std::vector<u8> payload = SaveGameSerializer::CaptureSceneState(*source);
    std::vector<u8> compressed;
    ASSERT_TRUE(SaveGameFile::Compress(payload, compressed));
    SaveGameHeader header;
    header.EntityCount = 1;
    header.SetCompression(SaveGameCompression::Zlib);
    header.PayloadUncompressedSize = payload.size();
    ASSERT_TRUE(SaveGameFile::Write(path, header, SaveGameMetadata{}, {}, compressed));

    SaveGameHeader readBack;
    ASSERT_TRUE(SaveGameFile::ReadHeader(path, readBack)) << "positive control: the current version reads";

    for (const u32 version : { kSaveGameFormatVersion - 1, kSaveGameFormatVersion + 1 })
    {
        SCOPED_TRACE(version);
        {
            std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
            ASSERT_TRUE(file.is_open());
            file.seekp(static_cast<std::streamoff>(offsetof(SaveGameHeader, FormatVersion)));
            file.write(reinterpret_cast<const char*>(&version), sizeof(version));
        }
        EXPECT_FALSE(SaveGameFile::ReadHeader(path, readBack));
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(SaveGameFormatGuard, LoadReportsAnotherFormatVersionAsUnsupportedNotAsCorruption)
{
    // The header is outside the CRC, so a save of another FormatVersion has an
    // intact checksum. Load used to validate the checksum first, whose header
    // read failed on the version, and reported ChecksumMismatch.
    const Ref<Project> previousProject = Project::GetActive();
    const std::filesystem::path root = OloEngine::Tests::TempDir("guard-load");
    ProjectConfig config;
    config.Name = "SaveGameFormatGuard";
    config.AssetDirectory = "Assets";
    ASSERT_TRUE(Project::NewInMemory(root, config));

    Ref<Scene> source = MakeScene();
    const std::vector<u8> payload = SaveGameSerializer::CaptureSceneState(*source);
    SaveGameHeader header;
    header.EntityCount = 1;
    std::filesystem::create_directories(SaveGameManager::GetSaveDirectory());
    const std::filesystem::path path = SaveGameManager::GetSaveFilePath("guard_version");
    ASSERT_TRUE(SaveGameFile::Write(path, header, SaveGameMetadata{}, {}, payload));

    {
        Ref<Scene> target = Ref<Scene>::Create();
        EXPECT_EQ(SaveGameManager::Load(*target, "guard_version"), SaveLoadResult::Success)
            << "positive control: the current version loads";
    }

    for (const u32 version : { kSaveGameFormatVersion - 1, kSaveGameFormatVersion + 1 })
    {
        SCOPED_TRACE(version);
        {
            std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
            ASSERT_TRUE(file.is_open());
            file.seekp(static_cast<std::streamoff>(offsetof(SaveGameHeader, FormatVersion)));
            file.write(reinterpret_cast<const char*>(&version), sizeof(version));
        }
        Ref<Scene> target = Ref<Scene>::Create();
        EXPECT_EQ(SaveGameManager::Load(*target, "guard_version"), SaveLoadResult::UnsupportedVersion);
    }

    // Negative control: a current-version file with a flipped payload byte is
    // still a checksum failure, so the new result did not swallow corruption.
    {
        const u32 current = kSaveGameFormatVersion;
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(file.is_open());
        file.seekp(static_cast<std::streamoff>(offsetof(SaveGameHeader, FormatVersion)));
        file.write(reinterpret_cast<const char*>(&current), sizeof(current));
        file.seekg(-1, std::ios::end);
        char last = 0;
        file.read(&last, 1);
        last = static_cast<char>(last ^ 0x5A);
        file.seekp(-1, std::ios::end);
        file.write(&last, 1);
    }
    {
        Ref<Scene> target = Ref<Scene>::Create();
        EXPECT_EQ(SaveGameManager::Load(*target, "guard_version"), SaveLoadResult::ChecksumMismatch);
    }

    if (previousProject)
    {
        Project::NewInMemory(previousProject->GetDirectory(), previousProject->GetConfig());
    }
    else
    {
        Project::Unload();
    }
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

// ============================================================================
// Current-format component round trips for fields that used to sit behind a
// version gate or an AtEnd() probe. Each asserts the value survives and the
// reader consumes exactly the block the writer produced.
// ============================================================================

namespace
{
    template<typename T>
    T RoundTrip(T& seed)
    {
        std::vector<u8> buffer;
        {
            FMemoryWriter writer(buffer);
            writer.ArIsSaveGame = true;
            SaveGameComponentSerializer::Serialize(writer, seed);
            EXPECT_FALSE(writer.IsError());
        }
        T loaded;
        FMemoryReader reader(buffer);
        reader.ArIsSaveGame = true;
        SaveGameComponentSerializer::Serialize(reader, loaded);
        EXPECT_FALSE(reader.IsError());
        EXPECT_TRUE(reader.AtEnd()) << "the reader did not consume exactly the block";
        return loaded;
    }
} // namespace

TEST(SaveGameComponentRoundTrip, TerrainKeepsTheVoxelMesher)
{
    TerrainComponent seed;
    seed.m_VoxelEnabled = true;
    seed.m_VoxelMesher = VoxelMesherKind::GreedyCubic;
    const TerrainComponent loaded = RoundTrip(seed);
    EXPECT_TRUE(loaded.m_VoxelEnabled);
    EXPECT_EQ(loaded.m_VoxelMesher, VoxelMesherKind::GreedyCubic);
}

TEST(SaveGameComponentRoundTrip, AudioSourceKeepsTheVoicePriority)
{
    AudioSourceComponent seed;
    seed.GetConfig().Priority = 0.87f;
    seed.SetSoundConfigHandle(AssetHandle(42));
    const AudioSourceComponent loaded = RoundTrip(seed);
    EXPECT_FLOAT_EQ(loaded.GetConfig().Priority, 0.87f);
    EXPECT_EQ(static_cast<u64>(loaded.GetSoundConfigHandle()), 42ull);
}

TEST(SaveGameComponentRoundTrip, LODGroupKeepsEveryLevelsError)
{
    LODGroupComponent seed;
    seed.m_LODGroup.Levels.Emplace(AssetHandle(0xAAAAull), 10.0f, 900u, 0.0f);
    seed.m_LODGroup.Levels.Emplace(AssetHandle(0xBBBBull), 50.0f, 450u, 0.0125f);
    seed.m_LODGroup.Levels.Emplace(AssetHandle(0xCCCCull), 200.0f, 100u, 0.0625f);
    const LODGroupComponent loaded = RoundTrip(seed);
    ASSERT_EQ(loaded.m_LODGroup.Levels.Num(), 3u);
    EXPECT_FLOAT_EQ(loaded.m_LODGroup.Levels[0].Error, 0.0f);
    EXPECT_FLOAT_EQ(loaded.m_LODGroup.Levels[1].Error, 0.0125f);
    EXPECT_FLOAT_EQ(loaded.m_LODGroup.Levels[2].Error, 0.0625f);
    EXPECT_TRUE(loaded.m_LODGroup.HasErrorData());
}

TEST(SaveGameComponentRoundTrip, WaterKeepsTheCascadeCountAndTheFieldsAfterIt)
{
    WaterComponent seed;
    seed.m_FFTCascades = Ocean::kThreeBandCascadeCount;
    seed.m_PlanarReflectionsEnabled = true;
    seed.m_WakeShapeEnabled = true;
    const WaterComponent loaded = RoundTrip(seed);
    EXPECT_EQ(loaded.m_FFTCascades, Ocean::kThreeBandCascadeCount);
    EXPECT_TRUE(loaded.m_PlanarReflectionsEnabled);
    EXPECT_TRUE(loaded.m_WakeShapeEnabled);
}

// A conversation's save block (#1712) loads untrusted bytes sanitized: a
// Processing state exists only inside one DialogueSystem call.
TEST(SaveGameComponentRoundTrip, AHostileConversationLoadsSanitized)
{
    DialogueStateComponent seed;
    seed.m_State = DialogueState::Processing;
    seed.m_CurrentNodeID = UUID(0xC1);
    seed.m_CurrentText = "The lamp still turns.";
    seed.m_AvailableChoices.Add(DialogueChoice{ .Text = "Only", .TargetNodeID = UUID(0xD2), .Condition = "has_key" });
    seed.m_HoveredChoiceIndex = 7;
    seed.m_SelectedChoiceIndex = -5;
    seed.m_TextRevealProgress = std::numeric_limits<f32>::quiet_NaN();
    seed.m_TextRevealSpeed = -3.0f;
    const DialogueStateComponent loaded = RoundTrip(seed);
    EXPECT_EQ(loaded.m_State, DialogueState::Inactive);
    EXPECT_EQ(static_cast<u64>(loaded.m_CurrentNodeID), 0xC1ull);
    EXPECT_EQ(loaded.m_CurrentText, "The lamp still turns.");
    ASSERT_EQ(loaded.m_AvailableChoices.Num(), 1);
    EXPECT_EQ(loaded.m_AvailableChoices[0].Text, FString("Only"));
    EXPECT_EQ(static_cast<u64>(loaded.m_AvailableChoices[0].TargetNodeID), 0xD2ull);
    EXPECT_EQ(loaded.m_AvailableChoices[0].Condition, FString("has_key"));
    EXPECT_EQ(loaded.m_HoveredChoiceIndex, -1);
    EXPECT_EQ(loaded.m_SelectedChoiceIndex, -1);
    EXPECT_FLOAT_EQ(loaded.m_TextRevealProgress, 1.0f);
    EXPECT_FLOAT_EQ(loaded.m_TextRevealSpeed, 30.0f);
}

// A choice state with nothing to choose could never be left (#1712).
TEST(SaveGameComponentRoundTrip, AChoiceStateWithNoChoicesLoadsInactive)
{
    DialogueStateComponent seed;
    seed.m_State = DialogueState::WaitingForChoice;
    EXPECT_EQ(RoundTrip(seed).m_State, DialogueState::Inactive);

    seed.m_AvailableChoices.Add(DialogueChoice{ .Text = "Go", .TargetNodeID = UUID(0xD3), .Condition = {} });
    EXPECT_EQ(RoundTrip(seed).m_State, DialogueState::WaitingForChoice) << "positive control: with a choice it stays";
}

// A finished TriggerOnce conversation stays finished after Continue (#1712):
// m_HasTriggered used to load as false and replay it, action nodes included.
TEST(SaveGameComponentRoundTrip, DialogueKeepsThatItHasTriggered)
{
    DialogueComponent seed;
    seed.m_DialogueTree = AssetHandle(0x1712);
    seed.m_TriggerOnce = true;
    seed.m_HasTriggered = true;
    const DialogueComponent loaded = RoundTrip(seed);
    EXPECT_EQ(static_cast<u64>(loaded.m_DialogueTree), 0x1712ull);
    EXPECT_TRUE(loaded.m_TriggerOnce);
    EXPECT_TRUE(loaded.m_HasTriggered);
}

// A runtime-owned entity (#1712) and everything parented under it stay out of
// the save, and a saved parent's child list stops naming them. The live scene
// keeps its child list: the capture scrubs a copy, not the scene.
TEST(SaveGameRuntimeOwned, MarkedSubtreesAreNotCapturedAndSavedParentsForgetThem)
{
    Ref<Scene> scene = MakeScene();
    Entity anchor = scene->CreateEntity("SavedAnchor");
    Entity owned = scene->CreateEntity("RuntimeOverlay");
    Entity ownedChild = scene->CreateEntity("RuntimeOverlayText");
    Entity savedChild = scene->CreateEntity("SavedChild");
    owned.SetParent(anchor);
    ownedChild.SetParent(owned);
    savedChild.SetParent(anchor);
    scene->MarkRuntimeOwned(owned);
    ASSERT_TRUE(scene->IsRuntimeOwned(ownedChild)) << "a child of a marked entity is runtime-owned";
    ASSERT_FALSE(scene->IsRuntimeOwned(anchor));
    const sizet total = scene->GetAllEntitiesWith<IDComponent>().size();

    u32 savedCount = 0;
    const std::vector<u8> payload = SaveGameSerializer::CaptureSceneState(*scene, &savedCount);
    EXPECT_EQ(anchor.Children().Num(), 2) << "the capture changed the live scene's child list";
    EXPECT_EQ(savedCount, total - 2) << "the count a header or slot list shows must be what the save holds";

    Ref<Scene> loaded = Ref<Scene>::Create();
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*loaded, payload));
    EXPECT_EQ(loaded->GetAllEntitiesWith<IDComponent>().size(), total - 2);
    EXPECT_FALSE(loaded->TryGetEntityWithUUID(owned.GetUUID()).has_value());
    EXPECT_FALSE(loaded->TryGetEntityWithUUID(ownedChild.GetUUID()).has_value());
    const auto loadedAnchor = loaded->TryGetEntityWithUUID(anchor.GetUUID());
    ASSERT_TRUE(loadedAnchor.has_value());
    const TArray<UUID> children = loadedAnchor->Children();
    ASSERT_EQ(children.Num(), 1);
    EXPECT_EQ(static_cast<u64>(children[0]), static_cast<u64>(savedChild.GetUUID()));
}

// A script can change the streaming byte budgets at runtime (#1365), so a save must
// carry them: before #1531 the Streaming block stopped at RegionDirectory and a loaded
// save came back with the scene file's budgets instead of the saved ones.
TEST(SaveGameSceneSettingsRoundTrip, StreamingByteBudgetsSurviveASave)
{
    Ref<Scene> saved = MakeScene();
    saved->GetStreamingSettings().MaxResidentMegabytes = 256.0f;
    saved->GetStreamingSettings().MaxAdmittedMegabytesPerFrame = 8.5f;
    saved->GetStreamingSettings().RepresentationResidentMegabytes = 128.0f;
    saved->GetStreamingSettings().RepresentationUploadMegabytesPerFrame = 4.5f;
    saved->GetStreamingSettings().RepresentationStagingMegabytes = 64.0f;
    const std::vector<u8> payload = SaveGameSerializer::CaptureSceneState(*saved);

    Ref<Scene> loaded = MakeScene(); // authored with no budget
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*loaded, payload));
    EXPECT_FLOAT_EQ(loaded->GetStreamingSettings().MaxResidentMegabytes, 256.0f);
    EXPECT_FLOAT_EQ(loaded->GetStreamingSettings().MaxAdmittedMegabytesPerFrame, 8.5f);
    EXPECT_FLOAT_EQ(loaded->GetStreamingSettings().RepresentationResidentMegabytes, 128.0f);
    EXPECT_FLOAT_EQ(loaded->GetStreamingSettings().RepresentationUploadMegabytesPerFrame, 4.5f);
    EXPECT_FLOAT_EQ(loaded->GetStreamingSettings().RepresentationStagingMegabytes, 64.0f);
}

TEST(SaveGameSceneSettingsRoundTrip, CorruptStreamingBudgetsLoadAsNoBudget)
{
    Ref<Scene> saved = MakeScene();
    // Written straight into the settings, past every sanitiser, as a corrupt save would carry them.
    saved->GetStreamingSettings().MaxResidentMegabytes = std::numeric_limits<f32>::quiet_NaN();
    saved->GetStreamingSettings().MaxAdmittedMegabytesPerFrame = -4.0f;
    saved->GetStreamingSettings().RepresentationResidentMegabytes = std::numeric_limits<f32>::infinity();
    saved->GetStreamingSettings().RepresentationUploadMegabytesPerFrame = -1.0f;
    saved->GetStreamingSettings().RepresentationStagingMegabytes = std::numeric_limits<f32>::quiet_NaN();
    const std::vector<u8> payload = SaveGameSerializer::CaptureSceneState(*saved);

    Ref<Scene> loaded = MakeScene();
    loaded->GetStreamingSettings().MaxResidentMegabytes = 64.0f;
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*loaded, payload));
    EXPECT_FLOAT_EQ(loaded->GetStreamingSettings().MaxResidentMegabytes, 0.0f);
    EXPECT_FLOAT_EQ(loaded->GetStreamingSettings().MaxAdmittedMegabytesPerFrame, 0.0f);
    EXPECT_FLOAT_EQ(loaded->GetStreamingSettings().RepresentationResidentMegabytes, 0.0f);
    EXPECT_FLOAT_EQ(loaded->GetStreamingSettings().RepresentationUploadMegabytesPerFrame, 0.0f);
    EXPECT_FLOAT_EQ(loaded->GetStreamingSettings().RepresentationStagingMegabytes, 0.0f);
}

// Scripts set dialogue variables and condition nodes read them. Before #1712 the
// save did not carry them, so a Continue reset every branch a conversation had
// unlocked. Every variant alternative round-trips, and the restore replaces the
// loading scene's table rather than merging into it.
TEST(SaveGameSceneSettingsRoundTrip, DialogueVariablesSurviveASave)
{
    Ref<Scene> saved = MakeScene();
    saved->GetDialogueVariables().SetBool("met_keeper", true);
    saved->GetDialogueVariables().SetInt("lamps_lit", 3);
    saved->GetDialogueVariables().SetFloat("trust", 0.75f);
    saved->GetDialogueVariables().SetString("ship_name", "Drift");
    const std::vector<u8> payload = SaveGameSerializer::CaptureSceneState(*saved);

    Ref<Scene> loaded = MakeScene();
    loaded->GetDialogueVariables().SetBool("stale", true);
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*loaded, payload));
    const DialogueVariables& variables = loaded->GetDialogueVariables();
    EXPECT_TRUE(variables.GetBool("met_keeper"));
    EXPECT_EQ(variables.GetInt("lamps_lit"), 3);
    EXPECT_FLOAT_EQ(variables.GetFloat("trust"), 0.75f);
    EXPECT_EQ(variables.GetString("ship_name"), "Drift");
    EXPECT_FALSE(variables.Has("stale")) << "the restore merged into the loading scene's table";
    EXPECT_EQ(variables.GetAll().size(), 4u);
}

TEST(SaveGameSceneSettingsRoundTrip, MoreDialogueVariablesThanTheReaderAcceptsFailTheCapture)
{
    // A block that refuses to write fails the whole capture instead of saving
    // a file whose settings no load can parse.
    Ref<Scene> saved = MakeScene();
    for (u32 i = 0; i <= 65'536; ++i)
        saved->GetDialogueVariables().SetBool("flag_" + std::to_string(i), true);
    EXPECT_TRUE(SaveGameSerializer::CaptureSceneState(*saved).empty());
}

TEST(SaveGameSceneSettingsRoundTrip, ANonFiniteDialogueVariableLoadsAsZero)
{
    Ref<Scene> saved = MakeScene();
    saved->GetDialogueVariables().SetFloat("trust", std::numeric_limits<f32>::quiet_NaN());
    const std::vector<u8> payload = SaveGameSerializer::CaptureSceneState(*saved);

    Ref<Scene> loaded = MakeScene();
    ASSERT_TRUE(SaveGameSerializer::RestoreSceneState(*loaded, payload));
    ASSERT_TRUE(loaded->GetDialogueVariables().Has("trust"));
    EXPECT_FLOAT_EQ(loaded->GetDialogueVariables().GetFloat("trust", -1.0f), 0.0f);
}

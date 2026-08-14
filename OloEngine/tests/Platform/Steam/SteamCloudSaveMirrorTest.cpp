#include "OloEnginePCH.h"
#include <gtest/gtest.h>

// OLO_TEST_LAYER: unit
//
// Steam Cloud <-> SaveGame reconciliation (#644).
//
// This is the payoff of the ISteamBackend seam. The cloud/local reconciliation rules are the part
// of the feature most likely to be wrong, and every one of them is a player-visible failure:
//
//   * a cloud copy shadowing NEWER local progress    → the player loses a session
//   * cloud-only slots missing from EnumerateSaves    → an empty load menu on a fresh machine,
//                                                       which looks exactly like "cloud is broken"
//   * DeleteSave leaving the cloud copy behind        → the deleted save reappears on next launch
//
// None of it needs a Steam client, an App ID or a Valve account, because SaveGameManager reaches
// Steam only through SteamManager, which is driven here by FakeSteamBackend. So these run on every
// machine and in CI — which matters, because App ID 480's cloud quota is 4096 bytes and cannot
// hold a realistic save, so this logic can NOT be verified end-to-end against live Steam.

#include "FakeSteamBackend.h"

#include "OloEngine/Project/Project.h"
#include "OloEngine/SaveGame/SaveGameFile.h"
#include "OloEngine/SaveGame/SaveGameManager.h"
#include "OloEngine/SaveGame/SaveGameTypes.h"
#include "Platform/Steam/SteamManager.h"

#include "TestTempDir.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using OloEngine::Project;
using OloEngine::ProjectConfig;
using OloEngine::SaveFileInfo;
using OloEngine::SaveGameFile;
using OloEngine::SaveGameHeader;
using OloEngine::SaveGameManager;
using OloEngine::SaveGameMetadata;
using OloEngine::SteamManager;
using OloEngine::Testing::FakeSteamBackend;

namespace
{
    constexpr const char* kSlot = "cloudslot";

    class SteamCloudSaveMirrorTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            // A real project directory so GetSaveDirectory() resolves; TempDir() is
            // process-exclusive (TestTempDir.h), so parallel ctest cases cannot collide.
            m_ProjectDir = OloEngine::Tests::TempDir();
            std::filesystem::create_directories(m_ProjectDir);
            ProjectConfig config;
            config.Name = "SteamCloudTestProject";
            m_Project = Project::NewInMemory(m_ProjectDir, config);
            ASSERT_TRUE(m_Project) << "could not establish an active project";

            auto backend = OloEngine::CreateScope<FakeSteamBackend>();
            m_Fake = backend.get();
            SteamManager::SetBackendForTesting(std::move(backend));
            SteamManager::Initialize();
            ASSERT_TRUE(SteamManager::IsAvailable());
        }

        void TearDown() override
        {
            SteamManager::ResetForTesting();
            m_Fake = nullptr;
            std::error_code ec;
            std::filesystem::remove_all(m_ProjectDir, ec);
        }

        // A byte-for-byte valid save file, so ReadMetadata/checksum paths behave as in production.
        [[nodiscard]] static std::vector<u8> MakeSaveFileBytes(const std::filesystem::path& scratch,
                                                               const std::string& displayName)
        {
            SaveGameHeader header;
            SaveGameMetadata meta;
            meta.DisplayName = displayName;
            meta.SceneName = "CloudScene";
            meta.TimestampUTC = 1700000000;
            meta.EntityCount = 3;

            const std::vector<u8> payload{ 1, 2, 3, 4, 5 };
            if (!SaveGameFile::Write(scratch, header, meta, {}, payload))
            {
                return {};
            }

            // Validate before allocating. An unopened stream makes tellg() return -1, which then
            // casts to a colossal size_t and either throws bad_alloc or allocates absurdly — a
            // confusing crash in a helper, instead of a clear failure at the real cause.
            std::ifstream in(scratch, std::ios::binary | std::ios::ate);
            if (!in)
            {
                ADD_FAILURE() << "could not reopen the synthesized save at " << scratch.string();
                return {};
            }

            const std::streamoff size = in.tellg();
            if (size <= 0)
            {
                ADD_FAILURE() << "synthesized save at " << scratch.string() << " has size " << size;
                return {};
            }

            in.seekg(0, std::ios::beg);
            std::vector<u8> bytes(static_cast<sizet>(size));
            if (!in.read(reinterpret_cast<char*>(bytes.data()), size))
            {
                ADD_FAILURE() << "short read on the synthesized save at " << scratch.string();
                return {};
            }
            return bytes;
        }

        // Put a valid save into the fake cloud WITHOUT leaving a local copy.
        void SeedCloudOnlySave(const std::string& slot, const std::string& displayName)
        {
            const auto scratch = m_ProjectDir / "scratch.olosave";
            const auto bytes = MakeSaveFileBytes(scratch, displayName);
            ASSERT_FALSE(bytes.empty()) << "could not synthesize a save file";
            std::error_code ec;
            std::filesystem::remove(scratch, ec);

            m_Fake->CloudFiles[slot + std::string(SaveGameManager::kSaveFileExtension)] = bytes;
        }

        [[nodiscard]] bool CloudHas(const std::string& slot) const
        {
            return m_Fake->CloudFiles.contains(slot + std::string(SaveGameManager::kSaveFileExtension));
        }

        [[nodiscard]] std::filesystem::path LocalPath(const std::string& slot) const
        {
            return SaveGameManager::GetSaveFilePath(slot);
        }

        std::filesystem::path m_ProjectDir;
        OloEngine::Ref<Project> m_Project;
        FakeSteamBackend* m_Fake = nullptr;
    };

    // A fresh machine: Steam has the save, the disk does not. Without the union in
    // EnumerateSaves the player sees an empty load menu and concludes cloud is broken.
    TEST_F(SteamCloudSaveMirrorTest, EnumerateSavesSurfacesCloudOnlySlots)
    {
        SeedCloudOnlySave(kSlot, "FromCloud");
        ASSERT_FALSE(std::filesystem::exists(LocalPath(kSlot)));

        const std::vector<SaveFileInfo> saves = SaveGameManager::EnumerateSaves();

        ASSERT_EQ(saves.size(), 1u) << "the cloud-only slot was not surfaced";
        EXPECT_EQ(saves[0].Metadata.DisplayName, "FromCloud");

        // Having pulled it down, the slot is no longer cloud-only.
        EXPECT_TRUE(std::filesystem::exists(LocalPath(kSlot)));
    }

    // The rule that protects player progress: a local save always wins, and a cloud copy must
    // never overwrite it. Enumerating is a read-only browse operation and must not mutate saves.
    TEST_F(SteamCloudSaveMirrorTest, LocalSaveIsNeverOverwrittenByTheCloudCopy)
    {
        // Local says "Local", cloud says "Cloud", same slot.
        const auto local = LocalPath(kSlot);
        std::filesystem::create_directories(local.parent_path());
        const auto localBytes = MakeSaveFileBytes(local, "Local");
        ASSERT_FALSE(localBytes.empty());
        ASSERT_TRUE(std::filesystem::exists(local));

        SeedCloudOnlySave(kSlot, "Cloud");

        const std::vector<SaveFileInfo> saves = SaveGameManager::EnumerateSaves();

        ASSERT_EQ(saves.size(), 1u) << "the same slot was listed twice";
        EXPECT_EQ(saves[0].Metadata.DisplayName, "Local") << "the cloud copy shadowed a local save";

        // ...and the local file on disk is untouched.
        SaveGameHeader header;
        SaveGameMetadata meta;
        ASSERT_TRUE(SaveGameFile::ReadMetadata(local, header, meta));
        EXPECT_EQ(meta.DisplayName, "Local");
    }

    // Without deleting the cloud copy, the very next EnumerateSaves re-downloads the save the
    // player just deleted.
    TEST_F(SteamCloudSaveMirrorTest, DeleteSaveAlsoRemovesTheCloudCopy)
    {
        const auto local = LocalPath(kSlot);
        std::filesystem::create_directories(local.parent_path());
        ASSERT_FALSE(MakeSaveFileBytes(local, "ToDelete").empty());
        SeedCloudOnlySave(kSlot, "ToDelete");
        ASSERT_TRUE(CloudHas(kSlot));

        EXPECT_TRUE(SaveGameManager::DeleteSave(kSlot));

        EXPECT_FALSE(std::filesystem::exists(local));
        EXPECT_FALSE(CloudHas(kSlot)) << "the cloud copy survived and will reappear";
        EXPECT_TRUE(SaveGameManager::EnumerateSaves().empty()) << "the deleted save came back";
    }

    // A cloud-only slot must be deletable too — otherwise it is impossible to remove from a load
    // menu on a machine that never had it locally.
    TEST_F(SteamCloudSaveMirrorTest, CloudOnlySlotCanBeDeleted)
    {
        SeedCloudOnlySave(kSlot, "CloudOnly");
        ASSERT_FALSE(std::filesystem::exists(LocalPath(kSlot)));

        EXPECT_TRUE(SaveGameManager::DeleteSave(kSlot)) << "deleting a cloud-only slot reported failure";
        EXPECT_FALSE(CloudHas(kSlot));
    }

    // With Cloud switched off (account-wide or per-app) the SaveGame system must behave exactly
    // as it does with no Steam at all — local only, no surprises.
    TEST_F(SteamCloudSaveMirrorTest, CloudDisabledLeavesSaveGameBehaviourUnchanged)
    {
        SeedCloudOnlySave(kSlot, "Invisible");
        m_Fake->CloudEnabled = false;

        EXPECT_TRUE(SaveGameManager::EnumerateSaves().empty()) << "cloud slots leaked in with Cloud disabled";
        EXPECT_FALSE(std::filesystem::exists(LocalPath(kSlot))) << "a file was pulled down with Cloud disabled";

        // Deleting a slot that exists nowhere locally and cannot be reached in the cloud is a
        // no-op failure, not a crash.
        EXPECT_FALSE(SaveGameManager::DeleteSave(kSlot));
    }

    // Steam absent entirely (the OFF path, and every CI runner).
    TEST_F(SteamCloudSaveMirrorTest, SteamUnavailableLeavesSaveGameBehaviourUnchanged)
    {
        SeedCloudOnlySave(kSlot, "Invisible");
        SteamManager::ResetForTesting(); // no backend at all
        ASSERT_FALSE(SteamManager::IsAvailable());

        EXPECT_TRUE(SaveGameManager::EnumerateSaves().empty());
        EXPECT_FALSE(SaveGameManager::DeleteSave(kSlot));
    }

    // A corrupt cloud blob must never land on disk at all.
    //
    // This is the failure that would be permanent rather than transient. SaveGameManager::Load
    // tests `exists(path)` FIRST and only falls back to cloud when there is no local file — so a
    // bad blob renamed into place is found by every subsequent Load, fails its checksum, and
    // returns ChecksumMismatch forever, shadowing a cloud copy that may since have finished
    // syncing and become perfectly good. The restore therefore validates BEFORE the rename.
    TEST_F(SteamCloudSaveMirrorTest, CorruptCloudBlobIsNeverWrittenToTheRealSavePath)
    {
        m_Fake->CloudFiles[std::string(kSlot) + std::string(SaveGameManager::kSaveFileExtension)] =
            std::vector<u8>{ 'n', 'o', 't', 'a', 's', 'a', 'v', 'e' };

        const std::vector<SaveFileInfo> saves = SaveGameManager::EnumerateSaves();
        EXPECT_TRUE(saves.empty()) << "an unparseable cloud blob was listed as a save";

        // The real path must be untouched, so the cloud fallback stays available to retry.
        EXPECT_FALSE(std::filesystem::exists(LocalPath(kSlot)))
            << "a corrupt cloud blob was written to the save path; it would shadow the cloud copy "
               "on every future Load";

        const std::filesystem::path leftover = LocalPath(kSlot).string() + ".cloudtmp";
        EXPECT_FALSE(std::filesystem::exists(leftover)) << "a .cloudtmp temp file was left behind";
    }

    // Deleting locally while the cloud copy survives is strictly worse than failing: the next
    // EnumerateSaves unions the cloud slot back in, so the save the player just deleted returns —
    // and the local copy they could still have loaded is gone. Both must stay, and the call fails.
    TEST_F(SteamCloudSaveMirrorTest, FailedCloudDeleteDoesNotRemoveTheLocalSave)
    {
        const auto local = LocalPath(kSlot);
        std::filesystem::create_directories(local.parent_path());
        ASSERT_FALSE(MakeSaveFileBytes(local, "KeepMe").empty());
        SeedCloudOnlySave(kSlot, "KeepMe");
        ASSERT_TRUE(CloudHas(kSlot));

        m_Fake->CloudDeleteFails = true;

        EXPECT_FALSE(SaveGameManager::DeleteSave(kSlot)) << "a failed cloud delete reported success";
        EXPECT_TRUE(std::filesystem::exists(local))
            << "the local save was deleted even though the cloud copy survived — it will reappear";
        EXPECT_TRUE(CloudHas(kSlot));
    }

    // The mirror-image failure: the cloud delete succeeds and the LOCAL delete then fails.
    //
    // Without a rollback that reports failure while having already destroyed the off-machine copy
    // — the player retries, it fails again, and the cloud copy never returns. There is no
    // two-phase commit across the filesystem and Steam Cloud, so DeleteSave keeps the bytes and
    // re-uploads them, making a reported failure mean "nothing was removed".
    TEST_F(SteamCloudSaveMirrorTest, FailedLocalDeleteRestoresTheCloudCopy)
    {
        const auto local = LocalPath(kSlot);
        std::filesystem::create_directories(local.parent_path());
        ASSERT_FALSE(MakeSaveFileBytes(local, "Rollback").empty());
        SeedCloudOnlySave(kSlot, "Rollback");
        ASSERT_TRUE(CloudHas(kSlot));

        // Hold the file open so std::filesystem::remove fails, the way a real lock would.
        std::ifstream lock(local, std::ios::binary);
        ASSERT_TRUE(lock.is_open());

        const bool deleted = SaveGameManager::DeleteSave(kSlot);
        lock.close();

        if (deleted)
        {
            // Windows let the delete through despite the open handle (share mode permitting).
            // Nothing to assert about rollback then — the delete simply succeeded.
            GTEST_SKIP() << "the platform allowed deletion of an open file; rollback path not reachable here";
        }

        EXPECT_TRUE(std::filesystem::exists(local)) << "the local file vanished despite the reported failure";
        EXPECT_TRUE(CloudHas(kSlot))
            << "the cloud copy was destroyed by a delete that reported failure — it must be rolled back";
    }
} // namespace

// OLO_TEST_LAYER: unit
// =============================================================================
// TerrainAuthoringReadbackTest.cpp
//
// Issue #716's second acceptance criterion, made mechanical: "no
// glGetTexImage / GetData on the terrain authoring path outside save/export".
//
// This is a SOURCE SCAN, not a behavioural test, and that is deliberate. The
// property being defended is an absence, and an absence has no observable
// signature at runtime that a test could assert cheaply: a readback that crept
// back into the brush path produces a perfectly correct terrain, just a stuttery
// one, on a machine that may not be the one running CI. Measuring the stutter
// needs a GPU and a stable frame budget; grepping for the call needs neither, so
// this one runs everywhere and runs in milliseconds.
//
// It scans the authoring sources for texture-readback calls and requires each
// hit to sit at one of a small number of NAMED sync points. When you add a
// legitimate readback, add its site here with the reason — the point is that
// growing the list is a deliberate, reviewable act rather than something that
// happens by forgetting.
// =============================================================================

#include <gtest/gtest.h>

#include "OloEngine/Core/Base.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef OLO_TEST_EDITOR_ROOT
#error "OLO_TEST_EDITOR_ROOT must be defined by the test target's CMake — see OloEngine/tests/CMakeLists.txt"
#endif

namespace OloEngine
{
    namespace
    {
        namespace fs = std::filesystem;

        fs::path RepoRoot()
        {
            return fs::path{ OLO_TEST_EDITOR_ROOT }.parent_path();
        }

        std::string ReadFile(const fs::path& path)
        {
            std::ifstream f(path, std::ios::binary);
            std::ostringstream buf;
            buf << f.rdbuf();
            return buf.str();
        }

        fs::path TerrainSrc(const std::string& relative)
        {
            return RepoRoot() / "OloEngine" / "src" / "OloEngine" / "Terrain" / relative;
        }

        struct Hit
        {
            std::string File;
            u32 Line = 0;
            std::string Text;
        };

        // Every way a terrain source can pull texels back to the CPU. GetData is the
        // engine-level spelling; glGetTexImage is the raw one that a well-meaning
        // "just this once" fix would reach for and that the RHI boundary would
        // otherwise not object to in a Terrain TU.
        std::vector<Hit> FindReadbacks(const fs::path& path)
        {
            std::vector<Hit> hits;
            std::istringstream stream(ReadFile(path));
            std::string line;
            u32 lineNumber = 0;
            while (std::getline(stream, line))
            {
                ++lineNumber;

                // Skip comments — this file's own prose, and the explanatory comments
                // in the sources, name these calls constantly.
                const sizet firstNonSpace = line.find_first_not_of(" \t");
                if (firstNonSpace != std::string::npos && line.compare(firstNonSpace, 2, "//") == 0)
                    continue;

                if (line.find("GetData(") != std::string::npos ||
                    line.find("glGetTexImage") != std::string::npos ||
                    line.find("ReadPixels") != std::string::npos)
                {
                    hits.push_back(Hit{ path.filename().string(), lineNumber, line });
                }
            }
            return hits;
        }
    } // namespace

    // The brush and erosion sources must contain NO readback at all. These are the
    // per-operation paths — anything here runs once per stroke frame or once per
    // erosion iteration, which is precisely the cost the issue removes.
    TEST(TerrainAuthoringReadbackTest, PerOperationAuthoringPathsHaveNoReadback)
    {
        const std::vector<std::string> perOperationSources{
            "Editor/TerrainBrush.cpp",
            "Editor/TerrainPaintBrush.cpp",
            "Editor/TerrainErosion.cpp",
            "Editor/TerrainGPUBrush.cpp",
            "Editor/TerrainTextureUndoStack.cpp",
        };

        for (const std::string& relative : perOperationSources)
        {
            const fs::path path = TerrainSrc(relative);
            ASSERT_TRUE(fs::exists(path)) << "Authoring source moved or was renamed: " << relative
                                          << " — repoint this test rather than deleting the check";

            const std::vector<Hit> hits = FindReadbacks(path);
            for (const Hit& hit : hits)
            {
                ADD_FAILURE() << relative << ":" << hit.Line << " performs a GPU->CPU readback on the "
                              << "terrain authoring path (issue #716):\n    " << hit.Text
                              << "\n  The authoring textures are GPU-resident. If a CPU consumer needs "
                              << "the heights, let it go through TerrainData::SyncFromGPU (or "
                              << "TerrainMaterial::SyncSplatmapsFromGPU) rather than reading back here.";
            }
        }
    }

    // TerrainData and TerrainMaterial DO read back — that is what makes the brushes
    // able not to. Each readback must live in the one function named as the sync
    // point (or in load-time initialisation), so there is exactly one place the cost
    // can be reasoned about.
    TEST(TerrainAuthoringReadbackTest, HeightAndSplatReadbacksLiveOnlyAtTheNamedSyncPoints)
    {
        struct Allowed
        {
            std::string Source;
            std::vector<std::string> Functions;
        };

        const std::vector<Allowed> allowed{
            // The single height sync point. Nothing else in TerrainData reads texels
            // back — LoadFromFile and the exporters go through the CPU mirror, which
            // SyncFromGPU refreshes for them.
            { "TerrainData.cpp", { "TerrainData::SyncFromGPU" } },
            // The splat twin, plus load-time initialisation: InitializeCPUSplatmaps
            // seeds the mirror from a splatmap that was loaded from disk, which
            // happens once per material rather than once per stroke.
            { "TerrainMaterial.cpp", { "TerrainMaterial::SyncSplatmapsFromGPU", "TerrainMaterial::InitializeCPUSplatmaps" } },
        };

        for (const Allowed& entry : allowed)
        {
            const fs::path path = TerrainSrc(entry.Source);
            ASSERT_TRUE(fs::exists(path)) << "Terrain source moved: " << entry.Source;

            const std::string contents = ReadFile(path);

            // Map each readback line to the most recent function definition above it.
            // A crude "last `Type Class::Method(` seen" scan is enough here because
            // these two files are flat function lists with no nested classes.
            std::istringstream stream(contents);
            std::string line;
            std::string currentFunction;
            u32 lineNumber = 0;
            while (std::getline(stream, line))
            {
                ++lineNumber;

                // A definition line at namespace scope: exactly four spaces of indent
                // (the file is wrapped in `namespace OloEngine`), a `::` qualifier and
                // an open paren. Matching on the indent is what distinguishes a
                // definition from a CALL to a qualified function inside a body, which
                // is indented further.
                if (line.starts_with("    ") && !line.starts_with("     ") &&
                    line.find("::") != std::string::npos && line.find('(') != std::string::npos)
                {
                    currentFunction = line;
                }

                const sizet firstNonSpace = line.find_first_not_of(" \t");
                if (firstNonSpace != std::string::npos && line.compare(firstNonSpace, 2, "//") == 0)
                    continue;

                const bool isReadback = line.find("GetData(") != std::string::npos ||
                                        line.find("glGetTexImage") != std::string::npos;
                if (!isReadback)
                    continue;

                bool ok = false;
                for (const std::string& fn : entry.Functions)
                {
                    if (currentFunction.find(fn) != std::string::npos)
                    {
                        ok = true;
                        break;
                    }
                }

                EXPECT_TRUE(ok) << entry.Source << ":" << lineNumber
                                << " reads texels back from a function that is not a declared sync point "
                                << "(issue #716).\n    " << line
                                << "\n  Enclosing function: " << currentFunction
                                << "\n  Either move the readback into the sync point, or add this function "
                                << "to the allow-list above WITH the reason it is not per-operation.";
            }
        }
    }

    // The editor panel used to re-upload the CPU heightmap after erosion, which was
    // correct only while erosion read the map back on every iteration. With the
    // readback gone that upload would push the pre-erosion mirror over the eroded
    // GPU texture and silently discard the whole pass — a bug that leaves the test
    // suite green and the terrain visibly unchanged, so it is worth a tripwire.
    TEST(TerrainAuthoringReadbackTest, ErosionUiDoesNotPushTheStaleMirrorBack)
    {
        const fs::path panel = RepoRoot() / "OloEditor" / "src" / "Panels" / "TerrainEditorPanel.cpp";
        ASSERT_TRUE(fs::exists(panel));

        // Comment lines are skipped: the panel explains at two sites WHY the upload
        // is gone, and a naive substring scan would fail on the explanation.
        std::istringstream stream(ReadFile(panel));
        std::string line;
        u32 lineNumber = 0;
        while (std::getline(stream, line))
        {
            ++lineNumber;
            const sizet firstNonSpace = line.find_first_not_of(" 	");
            if (firstNonSpace != std::string::npos && line.compare(firstNonSpace, 2, "//") == 0)
                continue;

            EXPECT_EQ(line.find("UploadToGPU("), std::string::npos)
                << "TerrainEditorPanel.cpp:" << lineNumber << " calls TerrainData::UploadToGPU(). Since "
                << "issue #716 the GPU heightmap is the newer copy during authoring, so uploading the CPU "
                << "mirror over it discards the edit — silently, with the suite still green. Sync in the "
                << "other direction instead (the mirror refreshes itself on the next read).";
        }
    }
} // namespace OloEngine

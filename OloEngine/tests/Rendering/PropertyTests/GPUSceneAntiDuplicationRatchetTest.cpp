// OLO_TEST_LAYER: plumbing
// =============================================================================
// GPUSceneAntiDuplicationRatchetTest.cpp
//
// The ratchet the GPU Scene epic asked for (issue #994, parent #977).
//
// One raster path — Scene::SubmitMeshSourceClassic through Renderer3D::DrawMesh
// — renders through the canonical instance/material records. The failure mode
// the epic exists to prevent is not that path regressing; it is the NEXT path
// quietly growing a second scene representation, because doing so is always
// locally easier than routing through the records. Nothing in a compiler or a
// pixel test notices that.
//
// So this test enumerates it. The duplication it recognises is the concrete
// one: assembling a draw's PREVIOUS-FRAME transform truth outside the GPU Scene
// — the per-entity previous-transform cache, or any assignment into a
// PrevTransform / prevTransform lane. Those are the mechanisms, and searching
// for the mechanism rather than a type name is deliberate: a new path that
// invents a third way to carry previous-frame truth is a new mechanism and
// belongs in kMarkers, not in a gap around it.
//
// Every file that does it must be named in GPUSceneLegacyAdapters.h, and every
// name there must still do it. The first direction stops new duplication; the
// second stops the list from outliving the duplication and turning into
// decoration.
//
// Classification: L1 / plumbing (source scan, no GL context).
// =============================================================================

#include "OloEnginePCH.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/GPUScene/GPUSceneLegacyAdapters.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace OloEngine::Tests
{
    namespace
    {
        namespace fs = std::filesystem;

        // The repo root, found by walking up from the working directory until
        // the engine source tree appears. The test binary runs from the repo
        // root (docs/testing.md), but a ctest working directory that differs
        // should skip cleanly rather than pass vacuously.
        [[nodiscard]] fs::path ResolveRepoRoot()
        {
            fs::path current = fs::current_path();
            for (int depth = 0; depth < 8; ++depth)
            {
                if (fs::exists(current / "OloEngine" / "src" / "OloEngine" / "Renderer" / "Renderer3D.h"))
                {
                    return current;
                }
                if (!current.has_parent_path() || current.parent_path() == current)
                {
                    break;
                }
                current = current.parent_path();
            }
            return {};
        }

        [[nodiscard]] std::string ReadWholeFile(const fs::path& path)
        {
            std::ifstream stream(path, std::ios::binary);
            std::ostringstream buffer;
            buffer << stream.rdbuf();
            return buffer.str();
        }

        // Comments are not code: the adapters are discussed in several headers
        // that assemble nothing, and this very file names the markers it looks
        // for. Both comment forms go — a `/* ... */` block spanning a marker
        // would otherwise fail the ratchet for a file that duplicates nothing,
        // and that is the failure mode that gets a ratchet deleted.
        //
        // This is not a parser: a `//` inside a string literal still ends the
        // line early. Acceptable here because the markers are assignment syntax,
        // which does not appear inside string literals in this tree — and it is
        // why the scan is deliberately narrow rather than "anything that
        // mentions transforms".
        [[nodiscard]] std::string StripComments(const std::string& source)
        {
            std::string withoutBlocks;
            withoutBlocks.reserve(source.size());
            for (std::size_t i = 0; i < source.size();)
            {
                if (source.compare(i, 2, "/*") == 0)
                {
                    const std::size_t close = source.find("*/", i + 2);
                    const std::size_t stop = close == std::string::npos ? source.size() : close + 2;
                    // Newlines survive, so stripping a block cannot join two
                    // lines into a marker that neither line contains.
                    for (std::size_t j = i; j < stop; ++j)
                    {
                        if (source[j] == '\n')
                        {
                            withoutBlocks.push_back('\n');
                        }
                    }
                    if (close == std::string::npos)
                    {
                        break;
                    }
                    i = stop;
                    continue;
                }
                withoutBlocks.push_back(source[i]);
                ++i;
            }

            std::string code;
            code.reserve(withoutBlocks.size());
            std::istringstream lines(withoutBlocks);
            for (std::string line; std::getline(lines, line);)
            {
                const auto comment = line.find("//");
                code.append(comment == std::string::npos ? line : line.substr(0, comment));
                code.push_back('\n');
            }
            return code;
        }

        // The mechanisms that carry previous-frame transform truth outside the
        // GPU Scene. `GetAndRecordPrevTransform` and its instanced sibling are
        // the per-entity cache; the assignment forms catch a path that writes a
        // previous-frame lane directly, whatever it names the variable.
        [[nodiscard]] bool DuplicatesTransformTruth(const std::string& source)
        {
            static const std::regex kMarkers{
                R"(GetAndRecordPrev(Transform|InstanceTransforms)|(\.|->)[Pp]rev[Tt]ransform\s*=[^=])"
            };
            return std::regex_search(StripComments(source), kMarkers);
        }

        [[nodiscard]] std::set<std::string> ScanEngineSources(const fs::path& root)
        {
            std::set<std::string> duplicating;
            const fs::path engine = root / "OloEngine" / "src";
            for (const fs::directory_entry& entry : fs::recursive_directory_iterator(engine))
            {
                if (!entry.is_regular_file())
                {
                    continue;
                }
                const fs::path& path = entry.path();
                const std::string extension = path.extension().string();
                if (extension != ".cpp" && extension != ".h")
                {
                    continue;
                }
                if (DuplicatesTransformTruth(ReadWholeFile(path)))
                {
                    duplicating.insert(fs::relative(path, root).generic_string());
                }
            }
            return duplicating;
        }
    } // namespace

    TEST(GPUSceneAntiDuplicationRatchet, EveryDuplicatingSourceIsANamedLegacyAdapter)
    {
        const fs::path root = ResolveRepoRoot();
        if (root.empty())
        {
            GTEST_SKIP() << "engine source tree not reachable from the working directory";
        }

        const std::set<std::string> duplicating = ScanEngineSources(root);
        // Anti-vacuity: the scan must see the sites that exist today. A regex
        // that silently stopped matching would otherwise turn this whole test
        // into a green no-op, which is the one failure a ratchet cannot have.
        ASSERT_FALSE(duplicating.empty()) << "the duplication scan matched nothing at all";

        std::string unnamed;
        for (const std::string& file : duplicating)
        {
            const bool named = std::ranges::any_of(GPUSceneLegacyAdapters::kAdapters,
                                                   [&](const GPUSceneLegacyAdapters::Adapter& adapter)
                                                   { return adapter.m_File == file; });
            if (!named)
            {
                unnamed += (unnamed.empty() ? "" : ", ") + file;
            }
        }

        EXPECT_TRUE(unnamed.empty())
            << "new code assembles previous-frame transform truth outside the GPU Scene records.\n"
               "Either consume a GPUSceneDrawLink (see Scene::SubmitMeshSourceClassic for the migrated path),\n"
               "or add an entry to OloEngine/Renderer/GPUScene/GPUSceneLegacyAdapters.h saying what the path is\n"
               "and what has to happen before it can go. Unnamed: "
            << unnamed;
    }

    TEST(GPUSceneAntiDuplicationRatchet, EveryNamedLegacyAdapterStillDuplicates)
    {
        const fs::path root = ResolveRepoRoot();
        if (root.empty())
        {
            GTEST_SKIP() << "engine source tree not reachable from the working directory";
        }

        const std::set<std::string> duplicating = ScanEngineSources(root);
        std::string stale;
        for (const GPUSceneLegacyAdapters::Adapter& adapter : GPUSceneLegacyAdapters::kAdapters)
        {
            EXPECT_TRUE(fs::exists(root / adapter.m_File))
                << "named legacy adapter points at a file that no longer exists: " << adapter.m_File;
            if (!duplicating.contains(std::string(adapter.m_File)))
            {
                stale += (stale.empty() ? "" : ", ") + std::string(adapter.m_File);
            }
        }

        EXPECT_TRUE(stale.empty()) << "a named legacy adapter no longer duplicates transform truth — it was migrated,\n"
                                      "so delete its entry from GPUSceneLegacyAdapters.h and let the list shrink. "
                                      "Stale: "
                                   << stale;
    }

    TEST(GPUSceneAntiDuplicationRatchet, AdapterEntriesAreUsableDocumentation)
    {
        // An adapter whose entry says nothing is worse than no entry: it passes
        // the ratchet while telling the next reader nothing about how to remove
        // it. Names and exit conditions are therefore required, and unique.
        std::set<std::string_view> files;
        for (const GPUSceneLegacyAdapters::Adapter& adapter : GPUSceneLegacyAdapters::kAdapters)
        {
            EXPECT_FALSE(adapter.m_Name.empty()) << adapter.m_File << " has no name";
            EXPECT_GT(adapter.m_Exit.size(), 20u)
                << adapter.m_File << " has no usable exit condition; say what has to be true before it can go";
            EXPECT_TRUE(files.insert(adapter.m_File).second) << "duplicate adapter entry for " << adapter.m_File;
            if (adapter.m_UnsupportedCategory.has_value())
            {
                // A category that submits geometry the records cannot carry has
                // to be one the profiler can name, or the diagnostics criterion
                // ("unsupported geometry never disappears silently") is not met.
                EXPECT_LT(static_cast<u32>(*adapter.m_UnsupportedCategory),
                          static_cast<u32>(GPUSceneUnsupportedCategory::Count));
                EXPECT_STRNE(GetGPUSceneUnsupportedCategoryName(*adapter.m_UnsupportedCategory), "Unknown");
            }
        }
    }
} // namespace OloEngine::Tests

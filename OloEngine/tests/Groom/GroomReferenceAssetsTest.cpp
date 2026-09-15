#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// GroomReferenceAssetsTest — issue #1232, acceptance criterion 4.
//
// "Reference animal and human grooms load with source provenance and
//  meaningful diagnostics."
//
// The two reference grooms are the HUMAN SCALP and the ANIMAL PELT built by
// Groom/GroomAlembicFixture.h, at production-plausible strand counts rather
// than the handful the unit cases use. They are authored here rather than
// committed as binaries for the reason given in that header, and the SAME
// generators produce the committed editor fixtures — set OLO_GROOM_EXPORT_DIR
// and run this suite to regenerate them, so the assets in the repo and the
// assets under test can never drift apart.
//
// This file also records the measurements the issue's delivery contract asks
// for — cook time, cooked size, CPU memory — through gtest's RecordProperty,
// so they land in the XML report instead of in a comment nobody re-runs.
// =============================================================================

#include <gtest/gtest.h>

#include "Groom/GroomAlembicFixture.h"
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Groom/GroomAsset.h"
#include "OloEngine/Groom/GroomCooker.h"

#if defined(OLO_WITH_ALEMBIC)
#include "OloEngine/Asset/Interchange/Alembic/AlembicGroomImporter.h"
#endif

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace OloEngine;

#if defined(OLO_WITH_ALEMBIC)

namespace
{
    // Production-plausible but CI-affordable. A shipping human groom is
    // 100k-200k strands; 20k keeps this case under a second while still
    // exercising the multi-megabyte cook path that a 200-strand unit case
    // never reaches.
    constexpr u32 kHumanStrands = 20000;
    constexpr u32 kHumanPoints = 8;
    constexpr u32 kHumanGuideStride = 64;

    constexpr u32 kAnimalStrands = 30000;
    constexpr u32 kAnimalPoints = 4;
    constexpr u32 kAnimalSubGroups = 3;

    struct ReferenceMeasurement
    {
        u32 CurveCount = 0;
        u32 PointCount = 0;
        u32 GroupCount = 0;
        u32 GuideCount = 0;
        u64 CpuBytes = 0;
        u64 CookedBytes = 0;
        f64 ImportMilliseconds = 0.0;
        f64 CookMilliseconds = 0.0;
    };

    void RecordMeasurement(const char* label, const ReferenceMeasurement& m)
    {
        ::testing::Test::RecordProperty(std::string(label) + "_curves", static_cast<int>(m.CurveCount));
        ::testing::Test::RecordProperty(std::string(label) + "_points", static_cast<int>(m.PointCount));
        ::testing::Test::RecordProperty(std::string(label) + "_groups", static_cast<int>(m.GroupCount));
        ::testing::Test::RecordProperty(std::string(label) + "_guides", static_cast<int>(m.GuideCount));
        ::testing::Test::RecordProperty(std::string(label) + "_cpu_bytes", static_cast<int>(m.CpuBytes));
        ::testing::Test::RecordProperty(std::string(label) + "_cooked_bytes", static_cast<int>(m.CookedBytes));
        ::testing::Test::RecordProperty(std::string(label) + "_import_ms", static_cast<int>(m.ImportMilliseconds));
        ::testing::Test::RecordProperty(std::string(label) + "_cook_ms", static_cast<int>(m.CookMilliseconds));

        // Also on stdout: the XML report is not what a person reads when they
        // run one case by hand, and these numbers are the issue's baseline.
        std::printf("[groom] %-14s curves=%u points=%u groups=%u guides=%u "
                    "cpu=%.2f MiB cooked=%.2f MiB import=%.1f ms cook=%.1f ms\n",
                    label, m.CurveCount, m.PointCount, m.GroupCount, m.GuideCount,
                    static_cast<f64>(m.CpuBytes) / (1024.0 * 1024.0),
                    static_cast<f64>(m.CookedBytes) / (1024.0 * 1024.0),
                    m.ImportMilliseconds, m.CookMilliseconds);
    }

    // When OLO_GROOM_EXPORT_DIR names a directory, the source .abc and the
    // cooked .ologroom are copied there. That is how the committed editor
    // fixtures are produced — the test IS the generator, so the committed
    // bytes are always bytes this code path made.
    void ExportIfRequested(const std::filesystem::path& abcPath, const std::vector<u8>& cooked,
                           const std::string& stem)
    {
        const char* exportDir = std::getenv("OLO_GROOM_EXPORT_DIR");
        if (exportDir == nullptr || *exportDir == '\0')
        {
            return;
        }
        std::error_code ec;
        const std::filesystem::path outDir(exportDir);
        std::filesystem::create_directories(outDir, ec);
        if (ec)
        {
            ADD_FAILURE() << "OLO_GROOM_EXPORT_DIR '" << exportDir << "' could not be created: " << ec.message();
            return;
        }

        std::filesystem::copy_file(abcPath, outDir / (stem + ".abc"),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
        {
            ADD_FAILURE() << "failed to export the source archive: " << ec.message();
            return;
        }

        std::ofstream out(outDir / (stem + ".ologroom"), std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out.write(reinterpret_cast<const char*>(cooked.data()), static_cast<std::streamsize>(cooked.size()));
        std::printf("[groom] exported %s.abc + %s.ologroom to %s\n", stem.c_str(), stem.c_str(), exportDir);
    }

    // Imports, cooks, reloads and checks the whole chain for one reference
    // groom. Returns the measurement so the caller can assert on it.
    ReferenceMeasurement LoadReferenceGroom(const Tests::GroomFixture::CurvesPrim& prim, const std::string& stem,
                                            const std::string& provenancePath)
    {
        ReferenceMeasurement measurement;

        const std::filesystem::path abcPath = Tests::TempFile(stem + ".abc");
        EXPECT_TRUE(Tests::GroomFixture::WriteArchive(abcPath, { prim })) << "failed to author " << stem;

        AlembicGroomImporter::Options options;
        options.ProvenancePath = provenancePath;

        const auto importStart = std::chrono::steady_clock::now();
        const auto result = AlembicGroomImporter::Import(abcPath, options);
        const auto importEnd = std::chrono::steady_clock::now();

        EXPECT_TRUE(result.Succeeded()) << result.Diagnostic;
        if (!result.Succeeded())
        {
            return measurement;
        }
        const GroomAsset& groom = *result.Groom;

        // ── Provenance (the criterion's own words) ──
        EXPECT_EQ(groom.GetProvenance().SourcePath, provenancePath)
            << "the cooked groom must name where it came from";
        EXPECT_EQ(groom.GetProvenance().SourceFormat, "AlembicCurves");
        EXPECT_NE(groom.GetProvenance().SourceContentHash, 0ull)
            << "a zero content hash cannot tell a stale cook from a current one";
        EXPECT_EQ(groom.GetProvenance().ImporterVersion, AlembicGroomImporter::kImporterVersion);
        // Never an absolute path: it would make the cooked bytes machine-specific.
        EXPECT_FALSE(std::filesystem::path(groom.GetProvenance().SourcePath).is_absolute())
            << "provenance path must be relative: " << groom.GetProvenance().SourcePath;

        std::string reason;
        std::vector<u8> cooked;
        const auto cookStart = std::chrono::steady_clock::now();
        EXPECT_TRUE(GroomCooker::CookToBytes(groom, cooked, reason)) << reason;
        const auto cookEnd = std::chrono::steady_clock::now();

        // ── Reload, and check the provenance survived the cook ──
        Ref<GroomAsset> reloaded;
        EXPECT_TRUE(GroomSerializer::DecodeFromBytes(cooked.data(), cooked.size(), reloaded, reason)) << reason;
        if (reloaded)
        {
            EXPECT_EQ(reloaded->GetProvenance(), groom.GetProvenance())
                << "provenance did not survive the cook/load round trip";
            EXPECT_EQ(reloaded->GetCurveCount(), groom.GetCurveCount());
            EXPECT_EQ(reloaded->GetGuideCount(), groom.GetGuideCount());
        }

        ExportIfRequested(abcPath, cooked, stem);

        measurement.CurveCount = groom.GetCurveCount();
        measurement.PointCount = groom.GetPointCount();
        measurement.GroupCount = groom.GetGroupCount();
        measurement.GuideCount = groom.GetGuideCount();
        measurement.CpuBytes = groom.GetCpuMemoryBytes();
        measurement.CookedBytes = cooked.size();
        measurement.ImportMilliseconds =
            std::chrono::duration<f64, std::milli>(importEnd - importStart).count();
        measurement.CookMilliseconds = std::chrono::duration<f64, std::milli>(cookEnd - cookStart).count();
        return measurement;
    }
} // namespace

TEST(GroomReferenceAssets, HumanScalpGroomLoadsWithProvenance)
{
    const auto prim = Tests::GroomFixture::MakeHumanScalpGroom(kHumanStrands, kHumanPoints, kHumanGuideStride,
                                                               "scalp");
    const ReferenceMeasurement measurement =
        LoadReferenceGroom(prim, "reference-human-scalp", "grooms/reference-human-scalp.abc");

    EXPECT_EQ(measurement.CurveCount, kHumanStrands);
    EXPECT_EQ(measurement.PointCount, kHumanStrands * kHumanPoints);
    EXPECT_EQ(measurement.GroupCount, 1u) << "the scalp authors one group";
    // Strand 0 is a guide, so the count rounds UP: ceil(N / stride).
    EXPECT_EQ(measurement.GuideCount, (kHumanStrands + kHumanGuideStride - 1u) / kHumanGuideStride);

    // The cook must actually compress: a groom is highly coherent float data,
    // so an .ologroom larger than the raw arrays means the payload is not
    // being deflated at all.
    EXPECT_LT(measurement.CookedBytes, measurement.CpuBytes)
        << "cooked " << measurement.CookedBytes << " B vs " << measurement.CpuBytes << " B resident";

    RecordMeasurement("human-scalp", measurement);
}

TEST(GroomReferenceAssets, AnimalPeltGroomLoadsWithProvenanceAndGroups)
{
    const auto prim = Tests::GroomFixture::MakeAnimalFurGroom(kAnimalStrands, kAnimalPoints, kAnimalSubGroups,
                                                              "pelt");
    const ReferenceMeasurement measurement =
        LoadReferenceGroom(prim, "reference-animal-pelt", "grooms/reference-animal-pelt.abc");

    EXPECT_EQ(measurement.CurveCount, kAnimalStrands);
    EXPECT_EQ(measurement.PointCount, kAnimalStrands * kAnimalPoints);
    EXPECT_EQ(measurement.GroupCount, kAnimalSubGroups) << "the pelt authors three sub-groups";
    EXPECT_GT(measurement.GuideCount, 0u) << "the pelt authors guides";

    RecordMeasurement("animal-pelt", measurement);
}

// The editor fixtures under OloEditor/SandboxProject/Assets/Grooms are produced
// by THIS case, at a tenth of the strand count the measurement cases use: a 20k
// groom is ~4 MB of .abc plus ~2 MB of .ologroom, and a repo fixture that large
// buys nothing a 2k one does not already show on screen. Run
//   OLO_GROOM_EXPORT_DIR=<repo>/OloEditor/SandboxProject/Assets/Grooms \
//     OloEngine-Tests.exe --gtest_filter=GroomReferenceAssets.ExportsTheEditorFixtures
// to regenerate them. Without the variable set it is an ordinary import test.
TEST(GroomReferenceAssets, ExportsTheEditorFixtures)
{
    constexpr u32 kEditorHumanStrands = 2000;
    constexpr u32 kEditorAnimalStrands = 2500;

    const auto scalp = Tests::GroomFixture::MakeHumanScalpGroom(kEditorHumanStrands, kHumanPoints,
                                                                kHumanGuideStride, "scalp");
    const ReferenceMeasurement human =
        LoadReferenceGroom(scalp, "reference-human-scalp", "Grooms/reference-human-scalp.abc");
    EXPECT_EQ(human.CurveCount, kEditorHumanStrands);

    const auto pelt = Tests::GroomFixture::MakeAnimalFurGroom(kEditorAnimalStrands, kAnimalPoints,
                                                              kAnimalSubGroups, "pelt");
    const ReferenceMeasurement animal =
        LoadReferenceGroom(pelt, "reference-animal-pelt", "Grooms/reference-animal-pelt.abc");
    EXPECT_EQ(animal.CurveCount, kEditorAnimalStrands);
    EXPECT_EQ(animal.GroupCount, kAnimalSubGroups);

    RecordMeasurement("editor-human", human);
    RecordMeasurement("editor-animal", animal);
}

TEST(GroomReferenceAssets, AMissingWidthsParamIsDiagnosedRatherThanGuessed)
{
    // "Meaningful diagnostics" cuts both ways: a groom with no authored widths
    // still IMPORTS (refusing it would make half the world's grooms
    // un-importable), but the substitution must be announced, and the
    // substituted value must be identifiable as a placeholder rather than
    // looking like authored data.
    auto prim = Tests::GroomFixture::MakeHumanScalpGroom(64, 4, 0, "nowidths");
    prim.Widths.clear();

    const std::filesystem::path path = Tests::TempFile("nowidths.abc");
    ASSERT_TRUE(Tests::GroomFixture::WriteArchive(path, { prim }));

    const auto result = AlembicGroomImporter::Import(path);
    ASSERT_TRUE(result.Succeeded()) << result.Diagnostic;

    for (const f32 width : result.Groom->GetPointWidths())
    {
        EXPECT_FLOAT_EQ(width, AlembicGroomImporter::kDefaultWidth)
            << "an unauthored width must be the documented placeholder, not an invented taper";
    }
}

TEST(GroomReferenceAssets, AMissingUVsParamStillImportsWithZeroRootUVs)
{
    auto prim = Tests::GroomFixture::MakeHumanScalpGroom(64, 4, 0, "nouvs");
    prim.UVs.clear();

    const std::filesystem::path path = Tests::TempFile("nouvs.abc");
    ASSERT_TRUE(Tests::GroomFixture::WriteArchive(path, { prim }));

    const auto result = AlembicGroomImporter::Import(path);
    ASSERT_TRUE(result.Succeeded()) << result.Diagnostic;

    for (const glm::vec2& uv : result.Groom->GetRootUVs())
    {
        EXPECT_EQ(uv, glm::vec2(0.0f));
    }
}

TEST(GroomReferenceAssets, ArchiveContainsCurvesDistinguishesAGroomFromAPolygonArchive)
{
    // The routing predicate: one extension, two schemas. If this returned true
    // for a polygon archive the editor would send meshes to the groom importer.
    const auto prim = Tests::GroomFixture::MakeHumanScalpGroom(16, 3, 0, "curvy");
    const std::filesystem::path curvePath = Tests::TempFile("curvy.abc");
    ASSERT_TRUE(Tests::GroomFixture::WriteArchive(curvePath, { prim }));
    EXPECT_TRUE(AlembicGroomImporter::ArchiveContainsCurves(curvePath));

    const std::filesystem::path emptyPath = Tests::TempFile("empty.abc");
    ASSERT_TRUE(Tests::GroomFixture::WriteArchive(emptyPath, {}));
    EXPECT_FALSE(AlembicGroomImporter::ArchiveContainsCurves(emptyPath));

    // A file that is not an Alembic archive at all must answer false rather
    // than throwing out of the predicate.
    const std::filesystem::path bogusPath = Tests::TempFile("bogus.abc");
    {
        std::ofstream out(bogusPath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());
        out << "this is not an Alembic archive";
    }
    EXPECT_FALSE(AlembicGroomImporter::ArchiveContainsCurves(bogusPath));
}

#endif // OLO_WITH_ALEMBIC

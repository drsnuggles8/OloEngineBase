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
#include "OloEngine/Groom/GroomCoat.h"
#include "OloEngine/Groom/GroomCooker.h"

#if defined(OLO_WITH_ALEMBIC)
#include "OloEngine/Asset/Interchange/Alembic/AlembicGroomImporter.h"
#endif

#include <chrono>
#include <map>
#include <cstdlib>
#include <filesystem>
#include <format>
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
        // The std::string overload, not the int one. These properties ARE the
        // issue's delivery baseline, and static_cast<int> flattened every
        // sub-millisecond timing to "0" and narrowed any byte count above
        // INT_MAX — so the recorded numbers did not match the measured ones.
        const std::string prefix(label);
        ::testing::Test::RecordProperty(prefix + "_curves", std::to_string(m.CurveCount));
        ::testing::Test::RecordProperty(prefix + "_points", std::to_string(m.PointCount));
        ::testing::Test::RecordProperty(prefix + "_groups", std::to_string(m.GroupCount));
        ::testing::Test::RecordProperty(prefix + "_guides", std::to_string(m.GuideCount));
        ::testing::Test::RecordProperty(prefix + "_cpu_bytes", std::to_string(m.CpuBytes));
        ::testing::Test::RecordProperty(prefix + "_cooked_bytes", std::to_string(m.CookedBytes));
        ::testing::Test::RecordProperty(prefix + "_import_ms", std::format("{:.3f}", m.ImportMilliseconds));
        ::testing::Test::RecordProperty(prefix + "_cook_ms", std::format("{:.3f}", m.CookMilliseconds));

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
    //
    // The vector overload is what the multi-REGION animals need (#1251): one
    // ICurves prim per region, so the regions are separable by group name.
    ReferenceMeasurement LoadReferenceGroom(const Tests::GroomFixture::CurvesPrim& prim, const std::string& stem,
                                            const std::string& provenancePath);

    ReferenceMeasurement LoadReferenceGroom(const std::vector<Tests::GroomFixture::CurvesPrim>& prims,
                                            const std::string& stem, const std::string& provenancePath)
    {
        ReferenceMeasurement measurement;

        const std::filesystem::path abcPath = Tests::TempFile(stem + ".abc");
        EXPECT_TRUE(Tests::GroomFixture::WriteArchive(abcPath, prims)) << "failed to author " << stem;

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

namespace
{
    ReferenceMeasurement LoadReferenceGroom(const Tests::GroomFixture::CurvesPrim& prim, const std::string& stem,
                                            const std::string& provenancePath)
    {
        return LoadReferenceGroom(std::vector<Tests::GroomFixture::CurvesPrim>{ prim }, stem, provenancePath);
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

    // ── The two coat-authoring reference animals (#1251) ────────────────
    //
    // Criterion 4's "short- and long-coated animal references". They are here
    // rather than in their own test for the reason this one exists at all: the
    // committed editor fixtures and the fixtures under test must be the same
    // bytes, and the only way to guarantee that is for one code path to produce
    // both.
    //
    // Unlike the two grooms above, these carry a `groom_role` per curve, so
    // their groups come back with real Undercoat / GuardHair / Whisker /
    // LongHair roles and the live editor scene has something for the coat
    // sliders and the visibility mask to act on. A groom with no roles is a
    // groom where hiding "Undercoat" correctly removes nothing — which is not a
    // bug, but is also not a demonstration.
    const ReferenceMeasurement shortCoat = LoadReferenceGroom(
        Tests::GroomFixture::MakeShortCoatAnimal(600), "reference-shortcoat-animal",
        "Grooms/reference-shortcoat-animal.abc");
    const ReferenceMeasurement longCoat = LoadReferenceGroom(
        Tests::GroomFixture::MakeLongCoatAnimal(600), "reference-longcoat-animal",
        "Grooms/reference-longcoat-animal.abc");

    // One group per (region, layer): the short coat has four regions with an
    // undercoat and a guard layer each, plus a single whisker group.
    EXPECT_EQ(shortCoat.GroupCount, 9u) << "four regions x two layers, plus whiskers";
    // The long coat trades the ears' and tail's guard layers for a mane and a
    // tail plume, both single-layer LongHair groups.
    EXPECT_EQ(longCoat.GroupCount, 9u) << "three regions x two layers, plus mane, plume and whiskers";
    EXPECT_GT(longCoat.CurveCount, 0u);
    EXPECT_GT(shortCoat.CurveCount, 0u);

    RecordMeasurement("editor-human", human);
    RecordMeasurement("editor-animal", animal);
    RecordMeasurement("editor-shortcoat", shortCoat);
    RecordMeasurement("editor-longcoat", longCoat);
}

// The ROLES survive the whole chain — importer heuristic or `groom_role`
// attribute, builder, cook, reload — which is what makes the committed fixtures
// usable as a live demonstration of the coat sliders rather than only as a
// topology test.
TEST(GroomReferenceAssets, TheReferenceAnimalsCarryRealCoatRoles)
{
    const auto prims = Tests::GroomFixture::MakeLongCoatAnimal(200);
    const std::filesystem::path abcPath = Tests::TempFile("roles.abc");
    ASSERT_TRUE(Tests::GroomFixture::WriteArchive(abcPath, prims));

    const auto result = AlembicGroomImporter::Import(abcPath);
    ASSERT_TRUE(result.Succeeded()) << result.Diagnostic;

    std::string reason;
    std::vector<u8> cooked;
    ASSERT_TRUE(GroomCooker::CookToBytes(*result.Groom, cooked, reason)) << reason;
    Ref<GroomAsset> reloaded;
    ASSERT_TRUE(GroomSerializer::DecodeFromBytes(cooked.data(), cooked.size(), reloaded, reason)) << reason;

    std::map<GroomCoatRole, u32> curvesByRole;
    for (u32 curve = 0; curve < reloaded->GetCurveCount(); ++curve)
    {
        ++curvesByRole[reloaded->GetGroupCoat(reloaded->GetCurveGroupIds()[curve]).GetRole()];
    }

    for (const auto& [role, count] : curvesByRole)
    {
        std::printf("[groom] longcoat role %-11s %u curves\n", std::string(ToString(role)).c_str(), count);
    }

    // All four authored roles are present after the round trip, and NOTHING is
    // Unassigned: a fixture that came back Unassigned would still render, and
    // every coat slider in the editor would silently do nothing.
    EXPECT_GT(curvesByRole[GroomCoatRole::Undercoat], 0u);
    EXPECT_GT(curvesByRole[GroomCoatRole::GuardHair], 0u);
    EXPECT_GT(curvesByRole[GroomCoatRole::Whisker], 0u);
    EXPECT_GT(curvesByRole[GroomCoatRole::LongHair], 0u);
    EXPECT_EQ(curvesByRole[GroomCoatRole::Unassigned], 0u)
        << "every group of a reference animal must have an authored role";

    // And the undercoat really is the numerous one, which is what makes the
    // per-role budget weighting meaningful on this asset.
    EXPECT_GT(curvesByRole[GroomCoatRole::Undercoat], curvesByRole[GroomCoatRole::GuardHair]);
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

    // The value alone is only half the criterion: the import must SAY it
    // substituted. Diagnostic is empty on success, so the announcement lives in
    // Warnings.
    bool announced = false;
    for (const std::string& warning : result.Warnings)
    {
        if (warning.find("widths") != std::string::npos)
        {
            announced = true;
        }
    }
    EXPECT_TRUE(announced) << "the substitution was applied but never announced; Warnings held "
                           << result.Warnings.size() << " entries";
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
        EXPECT_FLOAT_EQ(uv.x, 0.0f);
        EXPECT_FLOAT_EQ(uv.y, 0.0f);
    }
}

// ── The editor's import route, minus the UI ─────────────────────────────────
// These cover AlembicGroomImporter::ImportAndCookToSidecar, which is what the
// content browser's "Import as Groom" action runs. It exists as an engine
// function specifically so it can be tested: the first version of this feature
// had the importer wired to NOTHING — every test called Import() directly, so
// nothing noticed that a groom .abc could not actually be imported in the
// editor at all.

TEST(GroomReferenceAssets, TheSidecarCookWritesALoadableOloGroomNextToTheSource)
{
    const auto prim = Tests::GroomFixture::MakeHumanScalpGroom(128, 5, 8, "scalp");
    const std::filesystem::path abcPath = Tests::TempFile("sidecar-source.abc");
    ASSERT_TRUE(Tests::GroomFixture::WriteArchive(abcPath, { prim }));

    AlembicGroomImporter::Options options;
    options.ProvenancePath = "Grooms/sidecar-source.abc";

    const auto cooked = AlembicGroomImporter::ImportAndCookToSidecar(abcPath, options);
    ASSERT_TRUE(cooked.Ok) << cooked.Diagnostic;

    // Next to the source, same stem, .ologroom extension — that is the file the
    // asset system is then asked to register.
    EXPECT_EQ(cooked.OutputPath.extension(), ".ologroom");
    EXPECT_EQ(cooked.OutputPath.stem(), abcPath.stem());
    EXPECT_EQ(cooked.OutputPath.parent_path(), abcPath.parent_path());
    ASSERT_TRUE(std::filesystem::exists(cooked.OutputPath));

    EXPECT_EQ(cooked.CurveCount, 128u);
    EXPECT_EQ(cooked.GroupCount, 1u);
    EXPECT_GT(cooked.CookedBytes, 0u);

    // And the bytes it wrote are a groom the runtime reader accepts, with the
    // provenance intact — the sidecar is not just a file of the right length.
    std::vector<u8> bytes;
    {
        std::ifstream in(cooked.OutputPath, std::ios::binary | std::ios::ate);
        ASSERT_TRUE(in.is_open());
        bytes.resize(static_cast<sizet>(in.tellg()));
        in.seekg(0, std::ios::beg);
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    EXPECT_EQ(bytes.size(), cooked.CookedBytes);

    Ref<GroomAsset> loaded;
    std::string reason;
    ASSERT_TRUE(GroomSerializer::DecodeFromBytes(bytes.data(), bytes.size(), loaded, reason)) << reason;
    EXPECT_EQ(loaded->GetCurveCount(), 128u);
    EXPECT_EQ(loaded->GetProvenance().SourcePath, "Grooms/sidecar-source.abc");
    EXPECT_EQ(loaded->GetProvenance().SourceFormat, "AlembicCurves");
}

TEST(GroomReferenceAssets, TheSidecarCookIsByteIdenticalToImportThenCook)
{
    // The route the editor takes must not be a second, subtly different
    // implementation of the cook.
    const auto prim = Tests::GroomFixture::MakeAnimalFurGroom(96, 4, 3, "pelt");
    const std::filesystem::path abcPath = Tests::TempFile("sidecar-parity.abc");
    ASSERT_TRUE(Tests::GroomFixture::WriteArchive(abcPath, { prim }));

    AlembicGroomImporter::Options options;
    options.ProvenancePath = "Grooms/sidecar-parity.abc";

    const auto viaSidecar = AlembicGroomImporter::ImportAndCookToSidecar(abcPath, options);
    ASSERT_TRUE(viaSidecar.Ok) << viaSidecar.Diagnostic;
    std::vector<u8> sidecarBytes;
    {
        std::ifstream in(viaSidecar.OutputPath, std::ios::binary | std::ios::ate);
        ASSERT_TRUE(in.is_open());
        sidecarBytes.resize(static_cast<sizet>(in.tellg()));
        in.seekg(0, std::ios::beg);
        in.read(reinterpret_cast<char*>(sidecarBytes.data()), static_cast<std::streamsize>(sidecarBytes.size()));
    }

    const auto direct = AlembicGroomImporter::Import(abcPath, options);
    ASSERT_TRUE(direct.Succeeded()) << direct.Diagnostic;
    std::vector<u8> directBytes;
    std::string reason;
    ASSERT_TRUE(GroomCooker::CookToBytes(*direct.Groom, directBytes, reason)) << reason;

    EXPECT_EQ(sidecarBytes, directBytes) << "the editor's route and the direct route produced different bytes";
}

TEST(GroomReferenceAssets, TheSidecarCookRefusesAPolygonArchiveByName)
{
    // The distinction the feature rests on. An archive with no ICurves must be
    // refused with a diagnostic that says so, not silently produce an empty
    // groom — and it must NOT leave a sidecar behind.
    const std::filesystem::path abcPath = Tests::TempFile("no-curves.abc");
    ASSERT_TRUE(Tests::GroomFixture::WriteArchive(abcPath, {}));

    const auto cooked = AlembicGroomImporter::ImportAndCookToSidecar(abcPath);
    EXPECT_FALSE(cooked.Ok);
    EXPECT_NE(cooked.Diagnostic.find("ICurves"), std::string::npos) << cooked.Diagnostic;
    EXPECT_FALSE(std::filesystem::exists(cooked.OutputPath))
        << "a refused import must not leave a sidecar for the asset system to pick up";
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

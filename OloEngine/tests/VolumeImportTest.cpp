#include "OloEnginePCH.h"

// OLO_TEST_LAYER: unit
// =============================================================================
// VolumeImportTest — unit/contract tests (headless, NO GL context needed).
//
// Pins issue #724's two seams:
//
//   1. .olovol BINARY FORMAT round-trip (VolumeSerializer::EncodeToBytes /
//      DecodeFromBytes) — always compiled, no OpenVDB dependency. Corrupt
//      input (bad magic, wrong checksum, truncated payload) must be rejected
//      rather than partially decoded (docs/agent-rules/binary-format-
//      versioning.md).
//
//   2. OpenVDB IMPORT transform/voxel-size preservation — only compiled when
//      OLO_WITH_OPENVDB is on (matches MeshInterchangeTest's OLO_WITH_USD/
//      OLO_WITH_ALEMBIC pattern). Authors a synthetic .vdb with a KNOWN
//      translation + voxel size, imports it via
//      OloEngine::VolumeCook::ImportOpenVDBVolume, and asserts the resulting
//      GridTransform reproduces openvdb::math::Transform::indexToWorld()
//      EXACTLY at several sample points — this is the test that catches a
//      wrong row/column convention in DeriveGridTransform() (see the long
//      comment there), rather than trusting the by-construction derivation
//      by eye.
// =============================================================================

#include <gtest/gtest.h>
#include "TestTempDir.h"

#include "OloEngine/Asset/AssetSerializer.h"

#include <glm/glm.hpp>
#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <limits>
#include <vector>

using namespace OloEngine;

namespace
{
    constexpr f32 kEpsilon = 1e-4f;
}

// ── Section 1: .olovol binary format ────────────────────────────────────────

TEST(VolumeImport, EncodeDecodeRoundTripPreservesEverything)
{
    const glm::uvec3 dims(2u, 3u, 4u);
    std::vector<f32> density(static_cast<sizet>(dims.x) * dims.y * dims.z);
    for (sizet i = 0; i < density.size(); ++i)
    {
        density[i] = static_cast<f32>(i) * 0.5f - 1.0f;
    }
    const glm::vec3 voxelSize(0.25f, 0.5f, 1.0f);
    glm::mat4 transform = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, -5.0f, 2.5f));
    const f32 background = -3.5f;

    std::vector<u8> bytes;
    ASSERT_TRUE(VolumeSerializer::EncodeToBytes(dims, voxelSize, transform, background, density, bytes, "test"));
    ASSERT_FALSE(bytes.empty());

    RawVolumeData decoded;
    ASSERT_TRUE(VolumeSerializer::DecodeFromBytes(bytes.data(), bytes.size(), decoded, "test"));

    EXPECT_EQ(decoded.Dimensions, dims);
    EXPECT_TRUE(decoded.IsValid());
    ASSERT_EQ(decoded.Density.size(), density.size());
    for (sizet i = 0; i < density.size(); ++i)
    {
        EXPECT_FLOAT_EQ(decoded.Density[i], density[i]) << "texel " << i;
    }
    EXPECT_NEAR(decoded.VoxelSize.x, voxelSize.x, kEpsilon);
    EXPECT_NEAR(decoded.VoxelSize.y, voxelSize.y, kEpsilon);
    EXPECT_NEAR(decoded.VoxelSize.z, voxelSize.z, kEpsilon);
    EXPECT_FLOAT_EQ(decoded.BackgroundValue, background);
    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            EXPECT_NEAR(decoded.GridTransform[col][row], transform[col][row], kEpsilon) << "col " << col << " row " << row;
        }
    }
}

TEST(VolumeImport, DecodeRejectsBadMagic)
{
    const glm::uvec3 dims(2u, 2u, 2u);
    std::vector<f32> density(8, 1.0f);
    std::vector<u8> bytes;
    ASSERT_TRUE(VolumeSerializer::EncodeToBytes(dims, glm::vec3(1.0f), glm::mat4(1.0f), 0.0f, density, bytes, "test"));

    // Corrupt the magic (first 4 bytes, little-endian FileHeader::Magic).
    bytes[0] ^= 0xFF;

    RawVolumeData decoded;
    EXPECT_FALSE(VolumeSerializer::DecodeFromBytes(bytes.data(), bytes.size(), decoded, "test"));
}

TEST(VolumeImport, DecodeRejectsChecksumMismatch)
{
    const glm::uvec3 dims(2u, 2u, 2u);
    std::vector<f32> density(8, 1.0f);
    std::vector<u8> bytes;
    ASSERT_TRUE(VolumeSerializer::EncodeToBytes(dims, glm::vec3(1.0f), glm::mat4(1.0f), 0.0f, density, bytes, "test"));

    // Flip a byte inside the compressed payload (after the 24-byte header) —
    // the CRC32 must catch this even though the magic/version are untouched.
    ASSERT_GT(bytes.size(), 25u);
    bytes[24] ^= 0xFF;

    RawVolumeData decoded;
    EXPECT_FALSE(VolumeSerializer::DecodeFromBytes(bytes.data(), bytes.size(), decoded, "test"));
}

TEST(VolumeImport, DecodeRejectsTruncatedFile)
{
    const glm::uvec3 dims(2u, 2u, 2u);
    std::vector<f32> density(8, 1.0f);
    std::vector<u8> bytes;
    ASSERT_TRUE(VolumeSerializer::EncodeToBytes(dims, glm::vec3(1.0f), glm::mat4(1.0f), 0.0f, density, bytes, "test"));

    bytes.resize(bytes.size() / 2);

    RawVolumeData decoded;
    EXPECT_FALSE(VolumeSerializer::DecodeFromBytes(bytes.data(), bytes.size(), decoded, "test"));
}

TEST(VolumeImport, EncodeRejectsNonFiniteDensity)
{
    const glm::uvec3 dims(1u, 1u, 1u);
    std::vector<f32> density = { std::numeric_limits<f32>::quiet_NaN() };
    std::vector<u8> bytes;
    EXPECT_FALSE(VolumeSerializer::EncodeToBytes(dims, glm::vec3(1.0f), glm::mat4(1.0f), 0.0f, density, bytes, "test"));
}

TEST(VolumeImport, EncodeRejectsMismatchedDensitySize)
{
    const glm::uvec3 dims(2u, 2u, 2u); // expects 8 texels
    std::vector<f32> density(4, 1.0f); // wrong size
    std::vector<u8> bytes;
    EXPECT_FALSE(VolumeSerializer::EncodeToBytes(dims, glm::vec3(1.0f), glm::mat4(1.0f), 0.0f, density, bytes, "test"));
}

TEST(VolumeImport, FileRoundTrip)
{
    const std::filesystem::path path = OloEngine::Tests::TempFile("volume_roundtrip.olovol");

    const glm::uvec3 dims(3u, 3u, 3u);
    std::vector<f32> density(27);
    for (sizet i = 0; i < density.size(); ++i)
    {
        density[i] = static_cast<f32>(i);
    }

    ASSERT_TRUE(VolumeSerializer::SerializeToFile(path, dims, glm::vec3(0.1f), glm::mat4(1.0f), -1.0f, density));
    ASSERT_TRUE(std::filesystem::exists(path));

    RawVolumeData decoded;
    ASSERT_TRUE(VolumeSerializer::DeserializeFromFile(path, decoded));
    EXPECT_EQ(decoded.Dimensions, dims);
    EXPECT_FLOAT_EQ(decoded.BackgroundValue, -1.0f);
}

#if defined(OLO_WITH_OPENVDB)

#include "OpenVDBVolumeCook.h"

#include <openvdb/openvdb.h>

namespace
{
    // Writes a small synthetic .vdb with a KNOWN affine transform (uniform
    // voxel size + translation) and a handful of active voxels with distinct
    // values, so the test can assert exact reproduction rather than "looks
    // plausible".
    void WriteSyntheticVdb(const std::filesystem::path& path, double voxelSize, const openvdb::Vec3d& translation)
    {
        openvdb::initialize();

        openvdb::FloatGrid::Ptr grid = openvdb::FloatGrid::create(/*background=*/-7.0f);
        grid->setName("density");
        grid->setGridClass(openvdb::GRID_FOG_VOLUME);
        openvdb::math::Transform::Ptr xform = openvdb::math::Transform::createLinearTransform(voxelSize);
        xform->postTranslate(translation);
        grid->setTransform(xform);

        openvdb::FloatGrid::Accessor accessor = grid->getAccessor();
        for (int x = 0; x < 4; ++x)
        {
            for (int y = 0; y < 4; ++y)
            {
                for (int z = 0; z < 4; ++z)
                {
                    const openvdb::Coord coord(x, y, z);
                    const float value = static_cast<float>(x + y * 4 + z * 16) * 0.1f;
                    accessor.setValue(coord, value);
                }
            }
        }

        openvdb::GridPtrVec grids;
        grids.push_back(grid);
        openvdb::io::File file(path.string());
        file.write(grids);
        file.close();
    }
} // namespace

TEST(VolumeImportOpenVDB, ImportsKnownGridDimensionsAndBackground)
{
    const std::filesystem::path vdbPath = OloEngine::Tests::TempFile("synthetic.vdb");
    WriteSyntheticVdb(vdbPath, /*voxelSize=*/0.5, openvdb::Vec3d(1.0, 2.0, 3.0));

    OloEngine::VolumeCook::VolumeCookOptions options;
    auto result = OloEngine::VolumeCook::ImportOpenVDBVolume(vdbPath, options);

    ASSERT_TRUE(result.Success) << result.ErrorMessage;
    // The active region is exactly the 4x4x4 block written above, and it is
    // well under MaxAxisResolution — no downsampling should occur.
    EXPECT_EQ(result.Grid.Dimensions, glm::uvec3(4u, 4u, 4u));
    EXPECT_FLOAT_EQ(result.Grid.BackgroundValue, -7.0f);
    EXPECT_EQ(result.Grid.SourceGridName, "density");
    EXPECT_NEAR(result.Grid.VoxelSize.x, 0.5f, kEpsilon);
    EXPECT_NEAR(result.Grid.VoxelSize.y, 0.5f, kEpsilon);
    EXPECT_NEAR(result.Grid.VoxelSize.z, 0.5f, kEpsilon);
}

TEST(VolumeImportOpenVDB, GridTransformReproducesIndexToWorldExactly)
{
    const std::filesystem::path vdbPath = OloEngine::Tests::TempFile("synthetic_transform.vdb");
    const double voxelSize = 0.25;
    const openvdb::Vec3d translation(10.0, -4.0, 6.5);
    WriteSyntheticVdb(vdbPath, voxelSize, translation);

    openvdb::initialize();
    openvdb::io::File readFile(vdbPath.string());
    readFile.open();
    openvdb::GridBase::Ptr baseGrid = readFile.readGrid("density");
    readFile.close();
    ASSERT_NE(baseGrid, nullptr);
    auto floatGrid = openvdb::gridPtrCast<openvdb::FloatGrid>(baseGrid);
    ASSERT_NE(floatGrid, nullptr);
    const openvdb::CoordBBox bbox = floatGrid->evalActiveVoxelBoundingBox();
    ASSERT_FALSE(bbox.empty());

    OloEngine::VolumeCook::VolumeCookOptions options;
    auto result = OloEngine::VolumeCook::ImportOpenVDBVolume(vdbPath, options);
    ASSERT_TRUE(result.Success) << result.ErrorMessage;

    // Ground truth, straight from OpenVDB's own transform — the dense grid
    // was NOT downsampled (asserted above via Dimensions == the source
    // bbox), so output-corner index i maps 1:1 onto source index (bbox.min()+i).
    const openvdb::Vec3d srcMin(bbox.min().x(), bbox.min().y(), bbox.min().z());
    const auto expectedWorld = [&](double i, double j, double k) -> glm::vec3
    {
        const openvdb::Vec3d w = floatGrid->transform().indexToWorld(srcMin + openvdb::Vec3d(i, j, k));
        return { static_cast<f32>(w.x()), static_cast<f32>(w.y()), static_cast<f32>(w.z()) };
    };

    struct SamplePoint
    {
        glm::vec4 Corner;
    };
    const SamplePoint samples[] = {
        { glm::vec4(0.0f, 0.0f, 0.0f, 1.0f) },
        { glm::vec4(1.0f, 0.0f, 0.0f, 1.0f) },
        { glm::vec4(0.0f, 1.0f, 0.0f, 1.0f) },
        { glm::vec4(0.0f, 0.0f, 1.0f, 1.0f) },
        { glm::vec4(2.0f, 3.0f, 1.0f, 1.0f) },
    };
    for (const auto& sample : samples)
    {
        const glm::vec3 actual = glm::vec3(result.Grid.GridTransform * sample.Corner);
        const glm::vec3 expected = expectedWorld(sample.Corner.x, sample.Corner.y, sample.Corner.z);
        EXPECT_NEAR(actual.x, expected.x, kEpsilon) << "corner " << sample.Corner.x << "," << sample.Corner.y << "," << sample.Corner.z;
        EXPECT_NEAR(actual.y, expected.y, kEpsilon) << "corner " << sample.Corner.x << "," << sample.Corner.y << "," << sample.Corner.z;
        EXPECT_NEAR(actual.z, expected.z, kEpsilon) << "corner " << sample.Corner.x << "," << sample.Corner.y << "," << sample.Corner.z;
    }
}

TEST(VolumeImportOpenVDB, CookToNativeFileRoundTripsThroughVolumeSerializer)
{
    const std::filesystem::path vdbPath = OloEngine::Tests::TempFile("cook_source.vdb");
    const std::filesystem::path olovolPath = OloEngine::Tests::TempFile("cook_output.olovol");
    WriteSyntheticVdb(vdbPath, 1.0, openvdb::Vec3d(0.0, 0.0, 0.0));

    std::string error;
    ASSERT_TRUE(OloEngine::VolumeCook::CookOpenVDBToNativeFile(vdbPath, olovolPath, {}, &error)) << error;
    ASSERT_TRUE(std::filesystem::exists(olovolPath));

    RawVolumeData decoded;
    ASSERT_TRUE(VolumeSerializer::DeserializeFromFile(olovolPath, decoded));
    EXPECT_EQ(decoded.Dimensions, glm::uvec3(4u, 4u, 4u));
    EXPECT_FLOAT_EQ(decoded.BackgroundValue, -7.0f);
}

TEST(VolumeImportOpenVDB, DownsamplesActiveRegionLargerThanMaxAxisResolution)
{
    const std::filesystem::path vdbPath = OloEngine::Tests::TempFile("large.vdb");
    WriteSyntheticVdb(vdbPath, 1.0, openvdb::Vec3d(0.0, 0.0, 0.0));

    OloEngine::VolumeCook::VolumeCookOptions options;
    options.MaxAxisResolution = 2; // force downsampling of the 4x4x4 fixture
    auto result = OloEngine::VolumeCook::ImportOpenVDBVolume(vdbPath, options);

    ASSERT_TRUE(result.Success) << result.ErrorMessage;
    EXPECT_LE(result.Grid.Dimensions.x, 2u);
    EXPECT_LE(result.Grid.Dimensions.y, 2u);
    EXPECT_LE(result.Grid.Dimensions.z, 2u);
    EXPECT_EQ(result.Grid.Density.size(),
              static_cast<sizet>(result.Grid.Dimensions.x) * result.Grid.Dimensions.y * result.Grid.Dimensions.z);
}

TEST(VolumeImportOpenVDB, MissingGridFailsCleanly)
{
    const std::filesystem::path vdbPath = OloEngine::Tests::TempFile("named.vdb");
    WriteSyntheticVdb(vdbPath, 1.0, openvdb::Vec3d(0.0, 0.0, 0.0));

    OloEngine::VolumeCook::VolumeCookOptions options;
    options.GridName = "does_not_exist";
    auto result = OloEngine::VolumeCook::ImportOpenVDBVolume(vdbPath, options);

    EXPECT_FALSE(result.Success);
    EXPECT_FALSE(result.ErrorMessage.empty());
}

TEST(VolumeImportOpenVDB, ListScalarGridNamesFindsTheGrid)
{
    const std::filesystem::path vdbPath = OloEngine::Tests::TempFile("listed.vdb");
    WriteSyntheticVdb(vdbPath, 1.0, openvdb::Vec3d(0.0, 0.0, 0.0));

    auto names = OloEngine::VolumeCook::ListScalarGridNames(vdbPath);
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "density");
}

#endif // OLO_WITH_OPENVDB

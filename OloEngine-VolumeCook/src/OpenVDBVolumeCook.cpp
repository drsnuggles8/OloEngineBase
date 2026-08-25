#include "OpenVDBVolumeCook.h"

#include "OloEngine/Asset/AssetSerializer.h"
#include "OloEngine/Core/Log.h"

#include <openvdb/openvdb.h>
#include <openvdb/tools/Interpolation.h>

#include <algorithm>
#include <cmath>
#include <mutex>

// OpenVDB headers reach only this TU and OloEngine/tests/VolumeImportTest.cpp
// (which authors synthetic .vdb fixtures directly against the OpenVDB API to
// pin the transform math) — never OloEngine/OloEditor's other sources,
// OloRuntime, or OloServer. This file is compiled only into the editor/cook-
// only OloEngine-VolumeCook target.

namespace OloEngine::VolumeCook
{
    namespace
    {
        void EnsureInitialized()
        {
            static std::once_flag s_InitFlag;
            std::call_once(s_InitFlag, []()
                           { openvdb::initialize(); });
        }

        // World-space position of a point in the RESAMPLED dense grid's own
        // index-corner space (NOT the source grid's native voxel index —
        // src.min() + outputIndex * step maps between the two). Defined once
        // and reused for both the transform basis (§ below) and density
        // sampling, so the two can never silently disagree on the mapping.
        openvdb::Vec3d ResampledCornerToSourceIndex(const openvdb::Vec3d& srcMin, const openvdb::Vec3d& step,
                                                    double oi, double oj, double ok)
        {
            return { srcMin.x() + (oi * step.x()), srcMin.y() + (oj * step.y()), srcMin.z() + (ok * step.z()) };
        }

        // Derives GridTransform PURELY from calling the source grid's own
        // indexToWorld() at the four corners spanning one output-index unit
        // along each axis, then reading off the resulting world-space
        // displacement as glm column vectors. This sidesteps OpenVDB's
        // internal row/column storage convention entirely (its Mat4d applies
        // as `world = indexPos(row) * M`, a row-vector convention that is
        // easy to transpose backwards by hand) — the basis vectors are
        // whatever indexToWorld() actually returns, so there is no
        // convention to get wrong. Verified against
        // openvdb::math::Transform::indexToWorld() directly in
        // VolumeImportTest.cpp.
        glm::mat4 DeriveGridTransform(const openvdb::math::Transform& xform, const openvdb::Vec3d& srcMin,
                                      const openvdb::Vec3d& step)
        {
            auto worldAt = [&](double oi, double oj, double ok) -> openvdb::Vec3d
            {
                return xform.indexToWorld(ResampledCornerToSourceIndex(srcMin, step, oi, oj, ok));
            };

            const openvdb::Vec3d origin = worldAt(0.0, 0.0, 0.0);
            const openvdb::Vec3d axisX = worldAt(1.0, 0.0, 0.0) - origin;
            const openvdb::Vec3d axisY = worldAt(0.0, 1.0, 0.0) - origin;
            const openvdb::Vec3d axisZ = worldAt(0.0, 0.0, 1.0) - origin;

            glm::mat4 out(1.0f);
            out[0] = glm::vec4(static_cast<f32>(axisX.x()), static_cast<f32>(axisX.y()), static_cast<f32>(axisX.z()), 0.0f);
            out[1] = glm::vec4(static_cast<f32>(axisY.x()), static_cast<f32>(axisY.y()), static_cast<f32>(axisY.z()), 0.0f);
            out[2] = glm::vec4(static_cast<f32>(axisZ.x()), static_cast<f32>(axisZ.y()), static_cast<f32>(axisZ.z()), 0.0f);
            out[3] = glm::vec4(static_cast<f32>(origin.x()), static_cast<f32>(origin.y()), static_cast<f32>(origin.z()), 1.0f);
            return out;
        }

        // First scalar (float) grid whose name matches `gridName`, or the
        // first scalar grid overall when `gridName` is empty.
        openvdb::FloatGrid::ConstPtr FindScalarGrid(const openvdb::GridPtrVec& grids, const std::string& gridName,
                                                    std::string& outFoundName)
        {
            for (const auto& gridBase : grids)
            {
                if (!gridBase || !gridBase->isType<openvdb::FloatGrid>())
                {
                    continue;
                }
                if (!gridName.empty() && gridBase->getName() != gridName)
                {
                    continue;
                }
                outFoundName = gridBase->getName();
                return openvdb::gridConstPtrCast<openvdb::FloatGrid>(gridBase);
            }
            return nullptr;
        }
    } // namespace

    std::vector<std::string> ListScalarGridNames(const std::filesystem::path& vdbPath)
    {
        EnsureInitialized();

        std::vector<std::string> names;
        try
        {
            openvdb::io::File file(vdbPath.string());
            file.open(/*delayLoad=*/false);
            openvdb::GridPtrVecPtr grids = file.getGrids();
            if (grids)
            {
                for (const auto& gridBase : *grids)
                {
                    if (gridBase && gridBase->isType<openvdb::FloatGrid>())
                    {
                        names.push_back(gridBase->getName());
                    }
                }
            }
            file.close();
        }
        catch (const std::exception& e)
        {
            OLO_CORE_ERROR("VolumeCook::ListScalarGridNames - failed to read '{}': {}", vdbPath.string(), e.what());
        }
        return names;
    }

    VolumeCookResult ImportOpenVDBVolume(const std::filesystem::path& vdbPath, const VolumeCookOptions& options)
    {
        EnsureInitialized();

        VolumeCookResult result;

        if (options.MaxAxisResolution == 0)
        {
            result.ErrorMessage = "VolumeCookOptions::MaxAxisResolution must be > 0";
            OLO_CORE_ERROR("VolumeCook::ImportOpenVDBVolume - {}", result.ErrorMessage);
            return result;
        }

        openvdb::FloatGrid::ConstPtr floatGrid;
        std::string foundName;
        try
        {
            openvdb::io::File file(vdbPath.string());
            file.open(/*delayLoad=*/false);
            openvdb::GridPtrVecPtr grids = file.getGrids();
            file.close();

            if (!grids || grids->empty())
            {
                result.ErrorMessage = "no grids found in " + vdbPath.string();
                OLO_CORE_ERROR("VolumeCook::ImportOpenVDBVolume - {}", result.ErrorMessage);
                return result;
            }

            floatGrid = FindScalarGrid(*grids, options.GridName, foundName);
            if (!floatGrid)
            {
                result.ErrorMessage = options.GridName.empty()
                                          ? "no scalar (float) grid found in " + vdbPath.string()
                                          : "grid '" + options.GridName + "' not found (or not scalar) in " + vdbPath.string();
                OLO_CORE_ERROR("VolumeCook::ImportOpenVDBVolume - {}", result.ErrorMessage);
                return result;
            }
        }
        catch (const std::exception& e)
        {
            result.ErrorMessage = std::string("exception reading ") + vdbPath.string() + ": " + e.what();
            OLO_CORE_ERROR("VolumeCook::ImportOpenVDBVolume - {}", result.ErrorMessage);
            return result;
        }

        const openvdb::CoordBBox bbox = floatGrid->evalActiveVoxelBoundingBox();
        if (bbox.empty())
        {
            result.ErrorMessage = "grid '" + foundName + "' in " + vdbPath.string() + " has no active voxels";
            OLO_CORE_ERROR("VolumeCook::ImportOpenVDBVolume - {}", result.ErrorMessage);
            return result;
        }

        const openvdb::Coord dim = bbox.dim(); // inclusive extent per axis, always >= 1
        const openvdb::Vec3d srcMin(bbox.min().x(), bbox.min().y(), bbox.min().z());

        // Dense-resample cap: downsample uniformly (preserving aspect) when
        // the active region's longest axis exceeds MaxAxisResolution, so a
        // huge or malformed source can't allocate an unbounded buffer.
        const double maxSrcAxis = static_cast<double>(std::max({ dim.x(), dim.y(), dim.z() }));
        const double downsampleScale = maxSrcAxis > options.MaxAxisResolution ? maxSrcAxis / options.MaxAxisResolution : 1.0;

        const auto axisTarget = [&](int srcExtent) -> u32
        {
            return std::max(1u, static_cast<u32>(std::ceil(static_cast<double>(srcExtent) / downsampleScale)));
        };
        const glm::uvec3 target(axisTarget(dim.x()), axisTarget(dim.y()), axisTarget(dim.z()));

        const openvdb::Vec3d step(static_cast<double>(dim.x()) / target.x, static_cast<double>(dim.y()) / target.y,
                                  static_cast<double>(dim.z()) / target.z);

        glm::mat4 gridTransform = DeriveGridTransform(floatGrid->transform(), srcMin, step);
        glm::vec3 voxelSize(glm::length(glm::vec3(gridTransform[0])), glm::length(glm::vec3(gridTransform[1])),
                            glm::length(glm::vec3(gridTransform[2])));

        std::vector<f32> density(static_cast<sizet>(target.x) * target.y * target.z);
        auto accessor = floatGrid->getConstAccessor();
        using Sampler = openvdb::tools::BoxSampler;

        for (u32 k = 0; k < target.z; ++k)
        {
            for (u32 j = 0; j < target.y; ++j)
            {
                for (u32 i = 0; i < target.x; ++i)
                {
                    // Sample at texel CENTERS (the +0.5 offset) for the best
                    // representative value per output texel — independent of
                    // GridTransform's corner-space convention above (the
                    // bounding box is unaffected by where within it density
                    // is sampled from).
                    const openvdb::Vec3d srcIdx =
                        ResampledCornerToSourceIndex(srcMin, step, static_cast<double>(i) + 0.5, static_cast<double>(j) + 0.5,
                                                     static_cast<double>(k) + 0.5);
                    const float value = Sampler::sample(accessor, srcIdx);
                    density[(static_cast<sizet>(k) * target.y + j) * target.x + i] = value;
                }
            }
        }

        result.Success = true;
        result.Grid.Dimensions = target;
        result.Grid.VoxelSize = voxelSize;
        result.Grid.GridTransform = gridTransform;
        result.Grid.BackgroundValue = floatGrid->background();
        result.Grid.Density = std::move(density);
        result.Grid.SourceGridName = foundName;

        OLO_CORE_INFO("VolumeCook: imported '{}' grid '{}' from '{}' -> {}x{}x{} dense volume",
                      vdbPath.filename().string(), foundName, vdbPath.string(), target.x, target.y, target.z);
        return result;
    }

    bool CookOpenVDBToNativeFile(const std::filesystem::path& vdbPath, const std::filesystem::path& outputOlovolPath,
                                 const VolumeCookOptions& options, std::string* outError)
    {
        VolumeCookResult imported = ImportOpenVDBVolume(vdbPath, options);
        if (!imported.Success)
        {
            if (outError)
            {
                *outError = imported.ErrorMessage;
            }
            return false;
        }

        const bool ok = VolumeSerializer::SerializeToFile(outputOlovolPath, imported.Grid.Dimensions, imported.Grid.VoxelSize,
                                                          imported.Grid.GridTransform, imported.Grid.BackgroundValue,
                                                          imported.Grid.Density);
        if (!ok && outError)
        {
            *outError = "failed to write " + outputOlovolPath.string();
        }
        return ok;
    }
} // namespace OloEngine::VolumeCook

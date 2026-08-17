#include "OloEnginePCH.h"
#include "OloEngine/Core/Environment.h"
#include "OloEngine/Asset/Interchange/USD/UsdMeshImporter.h"

#if defined(OLO_WITH_USD)

#include "OloEngine/Core/Log.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Vertex.h"

#include <pxr/base/arch/systemInfo.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/rotation.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>

#include <glm/glm.hpp>

#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace OloEngine
{
    namespace
    {
        // Register USD's built-in Sdf file-format plugins (usda/usdc/usdz) once. In a static
        // monolithic build the plugin CODE is compiled into the exe, but PlugRegistry still
        // needs the plugInfo.json resource tree to map extension->format. We look for it via
        // (a) OLO_USD_PLUGIN_PATH env (used by the headless smoke test) then (b) <exe>/usd (the
        // tree the engine build copies next to OloEditor). Without this UsdStage::Open returns
        // null on every file, including a plain .usda.
        void EnsureUsdPluginsRegistered()
        {
            static std::once_flag flag;
            std::call_once(flag,
                           []()
                           {
                               if (const std::optional<std::string> env = Env::Get("OLO_USD_PLUGIN_PATH"))
                                   PlugRegistry::GetInstance().RegisterPlugins(*env);

                               namespace fs = std::filesystem;
                               std::error_code ec;
                               const fs::path exeDir = fs::path(ArchGetExecutablePath()).parent_path();
                               if (const fs::path usdDir = exeDir / "usd"; fs::exists(usdDir, ec))
                                   PlugRegistry::GetInstance().RegisterPlugins(usdDir.string());
                           });
        }

        glm::vec3 ToGlm(const GfVec3f& v)
        {
            return glm::vec3(v[0], v[1], v[2]);
        }

        // USD interpolation -> the index into a primvar's value (or index) array for a given
        // face/point/corner. Mirrors the UsdGeomPrimvar element-count semantics.
        i32 ElementIndex(const TfToken& interp, i32 faceIndex, i32 pointIndex, i32 corner)
        {
            if (interp == UsdGeomTokens->constant)
                return 0;
            if (interp == UsdGeomTokens->uniform)
                return faceIndex;
            if (interp == UsdGeomTokens->faceVarying)
                return corner;
            // vertex / varying (and the safe default): one value per point.
            return pointIndex;
        }

        glm::vec3 ComputeFaceNormal(const std::vector<glm::vec3>& ring)
        {
            glm::vec3 n(0.0f);
            const sizet count = ring.size();
            for (sizet i = 0; i < count; ++i)
            {
                const glm::vec3& cur = ring[i];
                const glm::vec3& nxt = ring[(i + 1) % count];
                n.x += (cur.y - nxt.y) * (cur.z + nxt.z);
                n.y += (cur.z - nxt.z) * (cur.x + nxt.x);
                n.z += (cur.x - nxt.x) * (cur.y + nxt.y);
            }
            const f32 len = glm::length(n);
            return (len > 1e-8f) ? (n / len) : glm::vec3(0.0f, 1.0f, 0.0f);
        }

        // Read UsdPreviewSurface factors into the engine PBR material. Texture-driven inputs are
        // logged (upload needs a GL context + asset manager — a documented follow-up).
        void ReadPreviewSurface(const UsdShadeMaterial& material, Material& out)
        {
            UsdShadeShader surface = material.ComputeSurfaceSource();
            if (!surface)
                return;

            out.SetType(MaterialType::PBR);

            auto readFloat = [&](const char* name) -> std::optional<f32>
            {
                UsdShadeInput input = surface.GetInput(TfToken(name));
                if (!input || input.HasConnectedSource())
                    return std::nullopt; // connected => texture-driven; handled below
                float v = 0.0f;
                if (input.Get(&v))
                    return std::isfinite(v) ? std::optional<f32>(v) : std::nullopt;
                return std::nullopt;
            };
            auto readColor = [&](const char* name) -> std::optional<glm::vec3>
            {
                UsdShadeInput input = surface.GetInput(TfToken(name));
                if (!input || input.HasConnectedSource())
                    return std::nullopt;
                GfVec3f v;
                if (input.Get(&v))
                {
                    // Reject NaN/inf components — same finiteness requirement as readFloat.
                    if (std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]))
                        return glm::vec3(v[0], v[1], v[2]);
                }
                return std::nullopt;
            };

            if (auto diffuse = readColor("diffuseColor"))
                out.SetBaseColorFactor(glm::vec4(*diffuse, 1.0f));
            if (auto metallic = readFloat("metallic"))
                out.SetMetallicFactor(*metallic);
            if (auto roughness = readFloat("roughness"))
                out.SetRoughnessFactor(*roughness);
            if (auto emissive = readColor("emissiveColor"))
                out.SetEmissiveFactor(glm::vec4(*emissive, 1.0f));
            if (auto opacity = readFloat("opacity"); opacity && *opacity < 0.999f)
            {
                glm::vec4 base = out.GetBaseColorFactor();
                base.a = *opacity;
                out.SetBaseColorFactor(base);
            }

            for (const char* channel : { "diffuseColor", "normal", "roughness", "metallic", "emissiveColor" })
            {
                if (UsdShadeInput input = surface.GetInput(TfToken(channel)); input && input.HasConnectedSource())
                    OLO_CORE_TRACE("OpenUSD: material channel '{}' is texture-driven (texture wiring is a follow-up)",
                                   channel);
            }
        }
    } // namespace

    MeshImportResult UsdMeshImporter::Import(const std::filesystem::path& path, const MeshImportOptions& options)
    {
        // Non-throwing existence check — the throwing overload can throw on a filesystem error,
        // breaching Import's no-throw boundary.
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec)
            return MeshImportResult::Failure("UsdMeshImporter: file does not exist: " + path.string());

        EnsureUsdPluginsRegistered();

        // USD stage open + traversal below can throw (bad_alloc from the large vertex/index
        // buffers, or a USD-internal error); convert any exception into a Failure so Import
        // stays no-throw for its callers.
        try
        {
            UsdStageRefPtr stage = UsdStage::Open(path.string());
            if (!stage)
                return MeshImportResult::Failure(
                    "UsdMeshImporter: UsdStage::Open failed for '" + path.string() +
                    "' (is the USD plugInfo resource tree registered? set OLO_USD_PLUGIN_PATH or ship <exe>/usd)");

            // Stage normalization: metersPerUnit scale + up-axis conversion to the engine's Y-up,
            // meter convention (matching the glTF path). A -90deg X rotation is a proper rotation,
            // so it does not flip handedness (winding is driven only by the prim orientation attr).
            const double metersPerUnit = UsdGeomGetStageMetersPerUnit(stage);
            const bool zUp = (UsdGeomGetStageUpAxis(stage) == UsdGeomTokens->z);
            const auto unitScale = static_cast<f32>((metersPerUnit > 0.0 && std::isfinite(metersPerUnit)) ? metersPerUnit : 1.0);

            GfMatrix4d conversion(1.0);
            conversion.SetScale(unitScale);
            if (zUp)
            {
                GfMatrix4d upRotation(1.0);
                upRotation.SetRotate(GfRotation(GfVec3d(1.0, 0.0, 0.0), -90.0));
                conversion = conversion * upRotation;
            }

            // USD 'st' has a bottom-left UV origin (same as Alembic); a V-flip to the engine's
            // top-left space is this format's default. FlipUV means "invert relative to the format's
            // default origin" (see MeshImporter.h), so XOR with the origin constant, not a bare negation.
            constexpr bool kUsdUVOriginBottomLeft = true;
            const bool flipV = kUsdUVOriginBottomLeft != options.FlipUV;

            UsdGeomXformCache xformCache(UsdTimeCode::Default());

            std::vector<Vertex> vertices;
            std::vector<u32> indices;
            std::vector<Submesh> submeshes;
            std::vector<Ref<Material>> materials;
            // Index into `materials` of a lazily-created shared default (assigned to prims with no
            // bound material); stays UINT32_MAX until first needed.
            u32 defaultMaterialIndex = UINT32_MAX;
            u32 meshCount = 0;

            for (const UsdPrim& prim : stage->Traverse())
            {
                if (!prim.IsA<UsdGeomMesh>())
                    continue;
                UsdGeomMesh mesh(prim);

                VtVec3fArray points;
                VtIntArray faceVertexCounts;
                VtIntArray faceVertexIndices;
                if (!mesh.GetPointsAttr().Get(&points) || !mesh.GetFaceVertexCountsAttr().Get(&faceVertexCounts) ||
                    !mesh.GetFaceVertexIndicesAttr().Get(&faceVertexIndices))
                    continue;
                if (points.empty() || faceVertexCounts.empty() || faceVertexIndices.empty())
                    continue;

                TfToken orientation = UsdGeomTokens->rightHanded;
                mesh.GetOrientationAttr().Get(&orientation);
                const bool leftHanded = (orientation == UsdGeomTokens->leftHanded);

                // Normals: primvars:normals, else the normals attr; capture interpolation + indices.
                VtVec3fArray normals;
                VtIntArray normalIndices;
                TfToken normalInterp;
                {
                    UsdGeomPrimvarsAPI primvarsApi(prim);
                    if (UsdGeomPrimvar normalPv = primvarsApi.GetPrimvar(TfToken("primvars:normals"));
                        normalPv && normalPv.HasValue())
                    {
                        normalPv.Get(&normals);
                        normalInterp = normalPv.GetInterpolation();
                        normalPv.GetIndices(&normalIndices);
                    }
                    else if (mesh.GetNormalsAttr().HasValue())
                    {
                        mesh.GetNormalsAttr().Get(&normals);
                        normalInterp = mesh.GetNormalsInterpolation();
                    }
                }
                const bool hasNormals = !normals.empty();

                // UVs: primvars:st (fallback to 'st').
                VtVec2fArray uvs;
                VtIntArray uvIndices;
                TfToken uvInterp;
                {
                    UsdGeomPrimvarsAPI primvarsApi(prim);
                    UsdGeomPrimvar uvPv = primvarsApi.GetPrimvar(TfToken("primvars:st"));
                    if (!uvPv)
                        uvPv = primvarsApi.GetPrimvar(TfToken("st"));
                    if (uvPv && uvPv.HasValue())
                    {
                        uvPv.Get(&uvs);
                        uvInterp = uvPv.GetInterpolation();
                        uvPv.GetIndices(&uvIndices);
                    }
                }
                const bool hasUVs = !uvs.empty();

                const GfMatrix4d localToWorld = xformCache.GetLocalToWorldTransform(prim);
                const GfMatrix4d full = localToWorld * conversion;
                const GfMatrix4d normalMatrix = full.GetInverse().GetTranspose();

                const auto baseVertex = static_cast<u32>(vertices.size());
                const auto baseIndex = static_cast<u32>(indices.size());
                const auto pointCount = static_cast<i32>(points.size());
                const auto cornerTotal = static_cast<i32>(faceVertexIndices.size());

                i32 corner = 0;
                const auto faceCount = static_cast<i32>(faceVertexCounts.size());
                for (i32 f = 0; f < faceCount; ++f)
                {
                    const i32 vertsInFace = faceVertexCounts[static_cast<sizet>(f)];
                    if (vertsInFace < 3 || corner + vertsInFace > cornerTotal)
                    {
                        corner += (vertsInFace > 0 ? vertsInFace : 0);
                        continue;
                    }

                    std::vector<u32> ring(static_cast<sizet>(vertsInFace));
                    std::vector<glm::vec3> ringPositions(static_cast<sizet>(vertsInFace));
                    const sizet ringStart = vertices.size();

                    bool faceValid = true;
                    for (i32 k = 0; k < vertsInFace; ++k, ++corner)
                    {
                        const i32 pointIndex = faceVertexIndices[static_cast<sizet>(corner)];
                        if (pointIndex < 0 || pointIndex >= pointCount)
                        {
                            // Invalid corner index — abandon the whole face (a partial ring would
                            // triangulate against zero-initialized entries). Advance corner past the
                            // face's remaining corners so the next face stays aligned; the vertex
                            // rollback + skip happen after the loop.
                            corner += (vertsInFace - k);
                            faceValid = false;
                            break;
                        }

                        Vertex vertex;
                        const GfVec3f worldPos(full.Transform(GfVec3d(points[static_cast<sizet>(pointIndex)])));
                        vertex.Position = ToGlm(worldPos);
                        ringPositions[static_cast<sizet>(k)] = vertex.Position;

                        if (hasNormals)
                        {
                            i32 e = ElementIndex(normalInterp, f, pointIndex, corner);
                            if (!normalIndices.empty() && e >= 0 && e < static_cast<i32>(normalIndices.size()))
                                e = normalIndices[static_cast<sizet>(e)];
                            if (e >= 0 && e < static_cast<i32>(normals.size()))
                            {
                                const GfVec3f wn(normalMatrix.TransformDir(GfVec3d(normals[static_cast<sizet>(e)])));
                                vertex.Normal = glm::normalize(ToGlm(wn));
                            }
                        }

                        if (hasUVs)
                        {
                            i32 e = ElementIndex(uvInterp, f, pointIndex, corner);
                            if (!uvIndices.empty() && e >= 0 && e < static_cast<i32>(uvIndices.size()))
                                e = uvIndices[static_cast<sizet>(e)];
                            if (e >= 0 && e < static_cast<i32>(uvs.size()))
                            {
                                const GfVec2f uv = uvs[static_cast<sizet>(e)];
                                vertex.TexCoord = glm::vec2(uv[0], flipV ? (1.0f - uv[1]) : uv[1]);
                            }
                        }

                        ring[static_cast<sizet>(k)] = static_cast<u32>(vertices.size());
                        vertices.push_back(vertex);
                    }

                    if (!faceValid)
                    {
                        // Roll back the vertices pushed for this abandoned face; skip normal
                        // generation + triangulation.
                        vertices.resize(ringStart);
                        continue;
                    }

                    if (!hasNormals)
                    {
                        const glm::vec3 faceNormal = ComputeFaceNormal(ringPositions);
                        for (i32 k = 0; k < vertsInFace; ++k)
                            vertices[ringStart + static_cast<sizet>(k)].Normal = faceNormal;
                    }

                    // Fan-triangulate (0,k,k+1); reverse the last two indices for a left-handed prim
                    // so front faces stay consistent with the right-handed glTF convention.
                    for (i32 k = 1; k + 1 < vertsInFace; ++k)
                    {
                        indices.push_back(ring[0]);
                        if (leftHanded)
                        {
                            indices.push_back(ring[static_cast<sizet>(k) + 1]);
                            indices.push_back(ring[static_cast<sizet>(k)]);
                        }
                        else
                        {
                            indices.push_back(ring[static_cast<sizet>(k)]);
                            indices.push_back(ring[static_cast<sizet>(k) + 1]);
                        }
                    }
                }

                const auto vertexCount = static_cast<u32>(vertices.size()) - baseVertex;
                const auto indexCount = static_cast<u32>(indices.size()) - baseIndex;
                if (vertexCount == 0 || indexCount == 0)
                    continue;

                // One material per mesh prim (mesh-level binding). GeomSubset per-face-group
                // materials are a follow-up.
                u32 materialIndex;
                if (UsdShadeMaterial boundMaterial = UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial())
                {
                    auto engineMaterial = Material::CreatePBR(prim.GetName().GetString(), glm::vec3(0.8f), 0.0f, 0.6f);
                    ReadPreviewSurface(boundMaterial, *engineMaterial);
                    materialIndex = static_cast<u32>(materials.size());
                    materials.push_back(engineMaterial);
                }
                else
                {
                    // Unbound prim -> the shared, lazily-created default material (a valid index, not UINT32_MAX).
                    if (defaultMaterialIndex == UINT32_MAX)
                    {
                        defaultMaterialIndex = static_cast<u32>(materials.size());
                        materials.push_back(Material::CreatePBR("UsdDefault", glm::vec3(0.8f), 0.0f, 0.6f));
                    }
                    materialIndex = defaultMaterialIndex;
                }

                Submesh submesh;
                submesh.m_BaseVertex = baseVertex;
                submesh.m_BaseIndex = baseIndex;
                submesh.m_VertexCount = vertexCount;
                submesh.m_IndexCount = indexCount;
                submesh.m_MaterialIndex = materialIndex;
                submesh.m_MeshName = prim.GetName().GetString();
                submesh.m_NodeName = prim.GetPath().GetString();
                submeshes.push_back(submesh);
                meshCount++;
            }

            if (vertices.empty() || submeshes.empty())
                return MeshImportResult::Failure("UsdMeshImporter: stage contained no UsdGeomMesh geometry: " + path.string());

            auto meshSource = Ref<MeshSource>::Create(std::move(vertices), std::move(indices));

            TArray<Submesh> submeshArray;
            submeshArray.Reserve(static_cast<i32>(submeshes.size()));
            for (const auto& submesh : submeshes)
                submeshArray.Add(submesh);
            meshSource->SetSubmeshes(submeshArray);

            // Every submesh references a valid material index (bound, or the shared default created
            // during traversal), so `materials` is non-empty whenever geometry was produced.
            meshSource->SetImportedMaterials(std::move(materials));

            // Fresh-vertex-per-corner combined buffer must not be re-optimized across submeshes.
            meshSource->SetPreOptimized(true);

            OLO_CORE_TRACE("UsdMeshImporter: imported '{}' ({} mesh prims, {} verts, upAxis={}, mpu={}).", path.string(),
                           meshCount, meshSource->GetVertices().Num(), zUp ? "Z" : "Y", metersPerUnit);

            return MeshImportResult::Ok(std::move(meshSource));
        } // try
        catch (const std::exception& e)
        {
            return MeshImportResult::Failure(std::string("UsdMeshImporter: import failed: ") + e.what());
        }
    }
} // namespace OloEngine

#endif // OLO_WITH_USD

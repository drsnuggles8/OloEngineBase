#include "OloEnginePCH.h"
#include "OloEngine/Asset/Interchange/Alembic/AlembicMeshImporter.h"

#if defined(OLO_WITH_ALEMBIC)

#include "OloEngine/Core/Log.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/Vertex.h"

#include <Alembic/Abc/All.h>
#include <Alembic/AbcCoreFactory/All.h>
#include <Alembic/AbcGeom/All.h>

#include <glm/glm.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace OloEngine
{
    namespace
    {
        namespace Abc = Alembic::Abc;
        namespace AbcG = Alembic::AbcGeom;
        namespace AbcF = Alembic::AbcCoreFactory;

        // Combined geometry accumulated across every mesh prim in the archive. One submesh per
        // IPolyMesh / ISubD prim; every submesh shares the default material (Alembic carries no
        // PBR material — see the header note). We emit a fresh vertex per face-vertex (no
        // welding), which keeps facevarying normals/UVs correct in the simplest way, and mark
        // the result pre-optimized so MeshSource::Build() won't run OptimizeMesh across the
        // multi-submesh combined buffer (the same reason Model::CreateCombinedMeshSource does).
        struct MeshAccumulator
        {
            std::vector<Vertex> Vertices;
            std::vector<u32> Indices;
            std::vector<Submesh> Submeshes;
            u32 MeshCount = 0;
            bool AnyAnimated = false;
        };

        glm::vec3 ToGlm(const Imath::V3d& v)
        {
            return glm::vec3(static_cast<f32>(v.x), static_cast<f32>(v.y), static_cast<f32>(v.z));
        }

        // Newell's method: a robust planar-polygon normal from the ring's world positions.
        // Used when a mesh carries no authored normals (all ISubD, some IPolyMesh).
        glm::vec3 ComputeFaceNormal(const std::vector<glm::vec3>& ringPositions)
        {
            glm::vec3 normal(0.0f);
            const sizet n = ringPositions.size();
            for (sizet i = 0; i < n; ++i)
            {
                const glm::vec3& cur = ringPositions[i];
                const glm::vec3& nxt = ringPositions[(i + 1) % n];
                normal.x += (cur.y - nxt.y) * (cur.z + nxt.z);
                normal.y += (cur.z - nxt.z) * (cur.x + nxt.x);
                normal.z += (cur.x - nxt.x) * (cur.y + nxt.y);
            }
            const f32 len = glm::length(normal);
            return (len > 1e-8f) ? (normal / len) : glm::vec3(0.0f, 1.0f, 0.0f);
        }

        // Emit one mesh's triangulated geometry into `out` as a new submesh.
        //   P/faceIndices/faceCounts — the geometry.
        //   worldXf — accumulated Alembic world transform (row-vector M44d convention).
        //   N* / UV* — optional facevarying/vertex params (getVals + optional getIndices + scope).
        //   flipV — flip the V texcoord so the bottom-left Alembic UV origin matches the engine's
        //           glTF-tuned top-left convention (the assumption to verify with a textured .abc).
        void EmitMesh(MeshAccumulator& out, const Imath::M44d& worldXf,
                      const Abc::P3fArraySamplePtr& P, const Abc::Int32ArraySamplePtr& faceIndices,
                      const Abc::Int32ArraySamplePtr& faceCounts, const AbcG::N3fArraySamplePtr& normalVals,
                      const Abc::UInt32ArraySamplePtr& normalIndices, AbcG::GeometryScope normalScope,
                      const AbcG::V2fArraySamplePtr& uvVals, const Abc::UInt32ArraySamplePtr& uvIndices,
                      AbcG::GeometryScope uvScope, bool flipV, const std::string& name)
        {
            if (!P || !faceIndices || !faceCounts)
                return;

            Imath::M44d normalMatrix = worldXf.inverse().transpose();
            const bool hasNormals = static_cast<bool>(normalVals);
            const bool hasUVs = static_cast<bool>(uvVals);

            const auto baseVertex = static_cast<u32>(out.Vertices.size());
            const auto baseIndex = static_cast<u32>(out.Indices.size());

            sizet faceVertex = 0; // running face-vertex counter across the whole mesh
            const sizet faceCount = faceCounts->size();
            for (sizet f = 0; f < faceCount; ++f)
            {
                const int vertsInFace = (*faceCounts)[f];
                if (vertsInFace < 3)
                {
                    faceVertex += static_cast<sizet>(vertsInFace < 0 ? 0 : vertsInFace);
                    continue;
                }

                std::vector<u32> ring(static_cast<sizet>(vertsInFace));
                std::vector<glm::vec3> ringPositions(static_cast<sizet>(vertsInFace));
                const sizet ringStart = out.Vertices.size();

                for (int k = 0; k < vertsInFace; ++k, ++faceVertex)
                {
                    const int pointIndex = (*faceIndices)[faceVertex];
                    Vertex vertex;

                    const Imath::V3f& p = (*P)[static_cast<sizet>(pointIndex)];
                    Imath::V3d worldPos = Imath::V3d(p.x, p.y, p.z) * worldXf;
                    vertex.Position = ToGlm(worldPos);
                    ringPositions[static_cast<sizet>(k)] = vertex.Position;

                    if (hasNormals)
                    {
                        // facevarying: index by the param's own indices (or the running fv);
                        // vertex/varying: index by the P point index.
                        sizet ni;
                        if (normalScope == AbcG::kFacevaryingScope)
                            ni = normalIndices ? (*normalIndices)[faceVertex] : faceVertex;
                        else
                            ni = static_cast<sizet>(pointIndex);
                        if (ni < normalVals->size())
                        {
                            const Imath::V3f& nrm = (*normalVals)[ni];
                            Imath::V3d worldN;
                            normalMatrix.multDirMatrix(Imath::V3d(nrm.x, nrm.y, nrm.z), worldN);
                            worldN.normalize();
                            vertex.Normal = ToGlm(worldN);
                        }
                    }

                    if (hasUVs)
                    {
                        sizet ui;
                        if (uvScope == AbcG::kFacevaryingScope)
                            ui = uvIndices ? (*uvIndices)[faceVertex] : faceVertex;
                        else
                            ui = static_cast<sizet>(pointIndex);
                        if (ui < uvVals->size())
                        {
                            const Imath::V2f& uv = (*uvVals)[ui];
                            vertex.TexCoord = glm::vec2(uv.x, flipV ? (1.0f - uv.y) : uv.y);
                        }
                    }

                    ring[static_cast<sizet>(k)] = static_cast<u32>(out.Vertices.size());
                    out.Vertices.push_back(vertex);
                }

                // Fill in geometric normals when the mesh carried none.
                if (!hasNormals)
                {
                    const glm::vec3 faceNormal = ComputeFaceNormal(ringPositions);
                    for (int k = 0; k < vertsInFace; ++k)
                        out.Vertices[ringStart + static_cast<sizet>(k)].Normal = faceNormal;
                }

                // Fan-triangulate the (assumed planar convex) polygon: (0, k, k+1).
                for (int k = 1; k + 1 < vertsInFace; ++k)
                {
                    out.Indices.push_back(ring[0]);
                    out.Indices.push_back(ring[static_cast<sizet>(k)]);
                    out.Indices.push_back(ring[static_cast<sizet>(k) + 1]);
                }
            }

            const auto vertexCount = static_cast<u32>(out.Vertices.size()) - baseVertex;
            const auto indexCount = static_cast<u32>(out.Indices.size()) - baseIndex;
            if (vertexCount == 0 || indexCount == 0)
                return;

            Submesh submesh;
            submesh.m_BaseVertex = baseVertex;
            submesh.m_BaseIndex = baseIndex;
            submesh.m_VertexCount = vertexCount;
            submesh.m_IndexCount = indexCount;
            submesh.m_MaterialIndex = 0; // single default material for all Alembic submeshes
            submesh.m_MeshName = name;
            submesh.m_NodeName = name;
            out.Submeshes.push_back(submesh);
            out.MeshCount++;
        }

        void ReadPolyMesh(MeshAccumulator& out, const Abc::IObject& obj, const Imath::M44d& worldXf, bool flipV)
        {
            AbcG::IPolyMesh mesh(obj, Abc::kWrapExisting);
            AbcG::IPolyMeshSchema& schema = mesh.getSchema();

            AbcG::IPolyMeshSchema::Sample sample;
            schema.get(sample, Abc::ISampleSelector(Abc::index_t(0)));

            if (schema.getNumSamples() > 1)
                out.AnyAnimated = true;

            AbcG::N3fArraySamplePtr normalVals;
            Abc::UInt32ArraySamplePtr normalIndices;
            AbcG::GeometryScope normalScope = AbcG::kUnknownScope;
            if (AbcG::IN3fGeomParam normalsParam = schema.getNormalsParam(); normalsParam.valid())
            {
                AbcG::IN3fGeomParam::Sample normalSample;
                normalsParam.getIndexed(normalSample, Abc::ISampleSelector(Abc::index_t(0)));
                normalVals = normalSample.getVals();
                normalIndices = normalSample.getIndices();
                normalScope = normalsParam.getScope();
            }

            AbcG::V2fArraySamplePtr uvVals;
            Abc::UInt32ArraySamplePtr uvIndices;
            AbcG::GeometryScope uvScope = AbcG::kUnknownScope;
            if (AbcG::IV2fGeomParam uvParam = schema.getUVsParam(); uvParam.valid())
            {
                AbcG::IV2fGeomParam::Sample uvSample;
                uvParam.getIndexed(uvSample, Abc::ISampleSelector(Abc::index_t(0)));
                uvVals = uvSample.getVals();
                uvIndices = uvSample.getIndices();
                uvScope = uvParam.getScope();
            }

            EmitMesh(out, worldXf, sample.getPositions(), sample.getFaceIndices(), sample.getFaceCounts(),
                     normalVals, normalIndices, normalScope, uvVals, uvIndices, uvScope, flipV,
                     obj.getName());
        }

        void ReadSubD(MeshAccumulator& out, const Abc::IObject& obj, const Imath::M44d& worldXf, bool flipV)
        {
            // Base cage only for the first slice — no subdivision/creases (documented). ISubD
            // carries no normals, so EmitMesh computes geometric ones.
            AbcG::ISubD subd(obj, Abc::kWrapExisting);
            AbcG::ISubDSchema& schema = subd.getSchema();

            AbcG::ISubDSchema::Sample sample;
            schema.get(sample, Abc::ISampleSelector(Abc::index_t(0)));

            if (schema.getNumSamples() > 1)
                out.AnyAnimated = true;

            AbcG::V2fArraySamplePtr uvVals;
            Abc::UInt32ArraySamplePtr uvIndices;
            AbcG::GeometryScope uvScope = AbcG::kUnknownScope;
            if (AbcG::IV2fGeomParam uvParam = schema.getUVsParam(); uvParam.valid())
            {
                AbcG::IV2fGeomParam::Sample uvSample;
                uvParam.getIndexed(uvSample, Abc::ISampleSelector(Abc::index_t(0)));
                uvVals = uvSample.getVals();
                uvIndices = uvSample.getIndices();
                uvScope = uvParam.getScope();
            }

            EmitMesh(out, worldXf, sample.getPositions(), sample.getFaceIndices(), sample.getFaceCounts(),
                     nullptr, nullptr, AbcG::kUnknownScope, uvVals, uvIndices, uvScope, flipV, obj.getName());
        }

        void Visit(const Abc::IObject& obj, const Imath::M44d& parentXf, MeshAccumulator& out, bool flipV)
        {
            Imath::M44d worldXf = parentXf;

            if (AbcG::IXform::matches(obj.getHeader()))
            {
                AbcG::IXform xform(obj, Abc::kWrapExisting);
                const AbcG::XformSample xformSample = xform.getSchema().getValue(Abc::ISampleSelector(Abc::index_t(0)));
                // Alembic composes child-times-parent in the row-vector convention.
                worldXf = xformSample.getMatrix() * parentXf;
            }

            if (AbcG::IPolyMesh::matches(obj.getHeader()))
                ReadPolyMesh(out, obj, worldXf, flipV);
            else if (AbcG::ISubD::matches(obj.getHeader()))
                ReadSubD(out, obj, worldXf, flipV);

            for (sizet i = 0; i < obj.getNumChildren(); ++i)
                Visit(obj.getChild(i), worldXf, out, flipV);
        }
    } // namespace

    MeshImportResult AlembicMeshImporter::Import(const std::filesystem::path& path, const MeshImportOptions& options)
    {
        if (!std::filesystem::exists(path))
            return MeshImportResult::Failure("AlembicMeshImporter: file does not exist: " + path.string());

        AbcF::IFactory factory;
        factory.setPolicy(Abc::ErrorHandler::kQuietNoopPolicy);

        AbcF::IFactory::CoreType coreType = AbcF::IFactory::kUnknown;
        Abc::IArchive archive;
        try
        {
            archive = factory.getArchive(path.string(), coreType);
        }
        catch (const std::exception& e)
        {
            return MeshImportResult::Failure(std::string("AlembicMeshImporter: failed to open archive: ") + e.what());
        }

        if (!archive.valid())
            return MeshImportResult::Failure("AlembicMeshImporter: not a readable Alembic (Ogawa) archive: " + path.string());

        MeshAccumulator accumulator;
        // flipV so the bottom-left Alembic UV origin matches the engine's glTF-tuned convention;
        // MeshImportOptions::FlipUV inverts that when a caller knows the source differs.
        const bool flipV = !options.FlipUV;

        try
        {
            Visit(archive.getTop(), Imath::M44d(), accumulator, flipV);
        }
        catch (const std::exception& e)
        {
            return MeshImportResult::Failure(std::string("AlembicMeshImporter: traversal error: ") + e.what());
        }

        if (accumulator.Vertices.empty() || accumulator.Submeshes.empty())
            return MeshImportResult::Failure("AlembicMeshImporter: archive contained no polymesh/subd geometry: " + path.string());

        auto meshSource = Ref<MeshSource>::Create(std::move(accumulator.Vertices), std::move(accumulator.Indices));

        TArray<Submesh> submeshes;
        submeshes.Reserve(static_cast<i32>(accumulator.Submeshes.size()));
        for (const auto& submesh : accumulator.Submeshes)
            submeshes.Add(submesh);
        meshSource->SetSubmeshes(submeshes);

        // Alembic has no PBR material; hand back one engine-default so consumers resolve a
        // material instead of null. A neutral mid-grey dielectric.
        auto defaultMaterial = Material::CreatePBR("AlembicDefault", glm::vec3(0.8f), 0.0f, 0.6f);
        meshSource->SetImportedMaterials({ defaultMaterial });

        // Multi-submesh combined data must not be re-optimized (would scramble cross-submesh
        // offsets); mark pre-optimized so Build() skips OptimizeMesh — same as the Assimp path.
        meshSource->SetPreOptimized(true);

        if (accumulator.AnyAnimated)
        {
            OLO_CORE_INFO("AlembicMeshImporter: '{}' has animated samples; imported REST POSE only "
                          "(baked vertex animation is a follow-up). {} submeshes, {} verts.",
                          path.string(), accumulator.MeshCount, meshSource->GetVertices().Num());
        }
        else
        {
            OLO_CORE_TRACE("AlembicMeshImporter: imported '{}' ({} submeshes, {} verts).", path.string(),
                           accumulator.MeshCount, meshSource->GetVertices().Num());
        }

        return MeshImportResult::Ok(std::move(meshSource));
    }
} // namespace OloEngine

#endif // OLO_WITH_ALEMBIC

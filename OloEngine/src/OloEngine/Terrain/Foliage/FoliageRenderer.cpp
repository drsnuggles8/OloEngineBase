#include "OloEnginePCH.h"
#include "OloEngine/Terrain/Foliage/FoliageWind.h"
#include "OloEngine/Terrain/Foliage/FoliageInteraction.h"
#include "OloEngine/Wind/WindSystem.h"
#include "FoliageRenderer.h"
#include "OloEngine/Renderer/VertexArray.h"
#include "OloEngine/Renderer/VertexBuffer.h"
#include "OloEngine/Renderer/IndexBuffer.h"
#include "OloEngine/Renderer/Buffer.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Commands/CommandDispatch.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/CameraRelative.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/Instancing/InstanceBuffer.h"
#include "OloEngine/Renderer/Instancing/InstanceData.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Terrain/Foliage/FoliagePlacement.h"
#include "OloEngine/Terrain/TerrainData.h"
#include "OloEngine/Terrain/TerrainMaterial.h"
#include "OloEngine/Renderer/BoundingVolume.h"
#include "OloEngine/Renderer/Frustum.h"
#include "OloEngine/Renderer/Impostor/ImpostorBaker.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Renderer/MaterialKind.h"
#include "OloEngine/Renderer/PBRModel.h"

#include <glm/gtc/constants.hpp>

namespace OloEngine
{
    void FoliageRenderer::QueueRayTracing(u64 owner, const glm::vec3& cameraPosition) const
    {
        if (!Renderer3D::WantsRayTracingVegetation())
            return;
        auto& cache = Renderer3D::GetVegetationSurfaceCache();
        const auto field = WindSystem::GetGPUData();
        // sqrt(||A||_1 * ||A||_infinity) bounds the spectral norm and retains
        // norm 1 for an identity terrain (unlike the Frobenius bound).
        f32 maxColumn = 0.0f, maxRow = 0.0f;
        for (u32 axis = 0u; axis < 3u; ++axis)
        {
            f32 column = 0.0f, row = 0.0f;
            for (u32 lane = 0u; lane < 3u; ++lane)
            {
                column += std::abs(m_TerrainTransform[axis][lane]);
                row += std::abs(m_TerrainTransform[lane][axis]);
            }
            maxColumn = std::max(maxColumn, column);
            maxRow = std::max(maxRow, row);
        }
        const f32 transformNorm = std::sqrt(maxColumn * maxRow);
        for (const auto& group : m_Registry.GetGroups())
        {
            if (group.m_LayerIndex >= m_Layers.size())
                continue;
            const auto& layer = m_Layers[group.m_LayerIndex];
            if (layer.UseImpostor && layer.Impostor.IsValid() && !layer.MeshVBO)
            {
                // An atlas without its source mesh cannot produce a ray-space
                // canopy. Count refusal and retain the whole raster tier.
                cache.Queue({});
                continue;
            }
            std::vector<const FoliageInstanceRecord*> meshRecords, impostorRecords, cardRecords;
            // RebuildGroups keeps these in ascending canonical ID for us.
            for (const auto id : group.m_Instances)
            {
                const auto* record = m_Registry.Find(id);
                if (!record)
                    continue;
                const glm::vec3 worldRoot = glm::vec3(m_TerrainTransform * glm::vec4(record->m_Position, 1.0f));
                const f32 distance = glm::length(worldRoot - cameraPosition);
                if (distance > layer.ViewDistance)
                    continue;
                // Retain the authored canopy for an octahedral far LOD: a
                // single main-view-facing card is not a ray-space silhouette.
                const bool mesh = layer.MeshVBO && (!layer.MeshParts.empty()) &&
                                  ((layer.UseImpostor && layer.Impostor.IsValid()) || distance <= layer.MeshViewDistance);
                const bool impostor = mesh && layer.UseImpostor && layer.Impostor.IsValid() &&
                                      distance > layer.MeshViewDistance;
                (impostor ? impostorRecords : (mesh ? meshRecords : cardRecords)).push_back(record);
            }
            for (const u32 representation : { 0u, 1u, 2u })
            {
                const bool mesh = representation != 2u, impostor = representation == 1u;
                const auto& records = impostor ? impostorRecords : (mesh ? meshRecords : cardRecords);
                const u32 plantsPerGroup = mesh ? RayTracing::VegetationPolicy::PlantsPerGroup : RayTracing::VegetationPolicy::CardPlantsPerGroup;
                for (sizet first = 0u; first < records.size(); first += plantsPerGroup)
                {
                    const sizet count = std::min(records.size() - first, static_cast<sizet>(plantsPerGroup));
                    const u64 estimatedBytes = count * ((mesh ? static_cast<u64>(layer.MeshVertexCount) : 4u) * sizeof(Vertex) +
                                                        (mesh ? static_cast<u64>(layer.MeshRayTracingIndices.size()) : 6u) * sizeof(u32) + sizeof(FoliageInstanceData));
                    if (!cache.HasQueueCapacity() || estimatedBytes > RayTracing::VegetationPolicy::GeometryBytes)
                    {
                        cache.Queue({});
                        continue;
                    }
                    RayTracing::VegetationSurfaceInput input;
                    input.Owner = owner;
                    input.FirstPlantId = records[first]->m_Id;
                    input.Rest = mesh ? layer.MeshVBO : layer.QuadVBO;
                    input.VertexCount = mesh ? layer.MeshVertexCount : 4u;
                    input.WorldTransform = m_TerrainTransform;
                    input.DetailedDistance = mesh ? std::max(50.0f, layer.MeshFadeStartDistance) : RayTracing::VegetationPolicy::DetailedCardDistance;
                    input.DistanceToView = std::numeric_limits<f32>::infinity();
                    const u32 partCount = mesh ? static_cast<u32>(layer.MeshParts.size()) : 1u;
                    bool validParts = true;
                    for (u32 part = 0u; part < partCount; ++part)
                    {
                        RayTracing::VegetationSurfacePart surface;
                        surface.Slot = part;
                        if (mesh)
                        {
                            const auto& range = layer.MeshParts[part];
                            if (range.IndexCount == 0u)
                                continue;
                            if (static_cast<u64>(range.BaseIndex) + range.IndexCount > layer.MeshRayTracingIndices.size())
                            {
                                validParts = false;
                                break;
                            }
                            surface.Indices.assign(layer.MeshRayTracingIndices.begin() + range.BaseIndex,
                                                   layer.MeshRayTracingIndices.begin() + range.BaseIndex + range.IndexCount);
                        }
                        else
                            surface.Indices = { 0u, 1u, 2u, 2u, 3u, 0u };
                        surface.Material.m_BaseColorFactor = glm::vec4(layer.BaseColor, 1.0f);
                        surface.Material.m_RoughnessFactor = layer.LeafRoughness;
                        surface.Material.m_AlphaMode = std::to_underlying(AlphaMode::Mask);
                        surface.Material.m_AlphaCutoff = layer.AlphaCutoff;
                        surface.Material.m_ClosureVersion = std::to_underlying(PBRModel::ClosureV2);
                        surface.Material.m_MaterialKind = std::to_underlying(MaterialKind::Foliage);
                        surface.Material.m_Flags = GPUSceneMaterialFlagPBR | GPUSceneMaterialFlagTwoSided | GPUSceneMaterialFlagDepthTest;
                        const auto albedo = mesh && layer.MeshParts[part].Albedo ? layer.MeshParts[part].Albedo : layer.AlbedoTexture;
                        if (albedo && albedo->IsLoaded())
                        {
                            surface.Material.m_Albedo.m_Handle = albedo->GetRHIHandle();
                            surface.Material.m_Albedo.m_HeapOffset = HeapBinding::ResolveRecordTextureOffset(
                                                                         albedo->GetRHIHandle(), HeapBinding::MaterialTexture2DSampler())
                                                                         .Value;
                        }
                        input.Parts.push_back(std::move(surface));
                    }
                    if (!validParts)
                    {
                        cache.Queue({});
                        continue;
                    }
                    const sizet end = std::min(records.size(), first + plantsPerGroup);
                    for (sizet plant = first; plant < end; ++plant)
                    {
                        const auto& record = *records[plant];
                        input.Rows.push_back({ glm::vec4(record.m_Position, record.m_Scale),
                                               glm::vec4(record.m_Rotation, record.m_Height, 1.0f, FoliageWindPhase(record.m_Id)),
                                               glm::vec4(layer.BaseColor, layer.AlphaCutoff) });
                        const auto worldRoot = glm::vec3(m_TerrainTransform * glm::vec4(record.m_Position, 1.0f));
                        input.DistanceToView = std::min(input.DistanceToView, glm::length(worldRoot - cameraPosition));
                    }
                    auto& wind = input.Wind;
                    wind.Time = m_Time;
                    wind.PrevTime = m_PrevTime;
                    wind.WindStrength = layer.WindStrength;
                    wind.WindSpeed = layer.WindSpeed;
                    wind.WindWeights = layer.WindWeights;
                    wind.WindDirection = field.DirectionAndSpeed;
                    wind.WindGust = field.GustAndTurbulence;
                    wind.WindClock = field.TimeAndFlags;
                    wind.WindFlags = glm::vec4(Renderer3D::GetRenderOrigin(), field.TimeAndFlags.y);
                    wind.WindHistoryValid = WindSystem::HasStableParameters() ? 1.0f : 0.0f;
                    wind.MeshParams.x = mesh ? 1.0f : 0.0f;
                    wind.MeshParams.z = impostor ? 1.0f : 0.0f;
                    wind.ImpostorParams1.x = impostor ? 1.0f : 0.0f;
                    // The ray-traced vegetation representation (issue #1240)
                    // takes the same influence set as the raster passes, so a
                    // plant a character is standing on casts a bent ray-traced
                    // shadow too. It is a SNAPSHOT: the cache's refresh key
                    // deliberately excludes time (see VegetationSurfaceCache),
                    // so interaction reaches RT on the same cadence wind phase
                    // does, not per frame. The velocity bound below is what
                    // keeps that cadence honest.
                    ApplyFoliageInteraction(wind, layer.InteractionResponse);
                    input.HistoryContinuous = WindSystem::HasStableParameters() && m_Time >= m_PrevTime;
                    const bool hierarchy = layer.WindWeights.x + layer.WindWeights.y + layer.WindWeights.z > 0.0f;
                    input.VelocityBound = RayTracing::VegetationPolicy::WindVelocityBound(
                        layer.WindStrength, layer.WindSpeed, layer.WindWeights.y, layer.WindWeights.z,
                        hierarchy, field.TimeAndFlags.y > 0.5f && (hierarchy || !impostor), field.DirectionAndSpeed.w,
                        field.GustAndTurbulence.x, field.GustAndTurbulence.y, transformNorm);
                    // A running actor moves a plant far faster than wind does,
                    // and ProxyAgeLimit is error/velocity — so a bound that
                    // ignored interaction would keep serving a snapshot taken
                    // before the actor arrived. Reported by the field from its
                    // own springs rather than estimated here.
                    if (const f32 bendRate = FoliageInteractionField::GetMaximumBendRate();
                        std::isfinite(bendRate) && std::isfinite(input.VelocityBound))
                        input.VelocityBound += bendRate * layer.InteractionResponse * transformNorm;
                    cache.Queue(std::move(input));
                }
            }
        }
    }

    FoliageRenderer::~FoliageRenderer()
    {
        for (auto& layer : m_Layers)
            ImpostorBaker::Free(layer.Impostor);
    }

    void FoliageRenderer::BuildQuadGeometry(LayerRenderData& data) const
    {
        // Billboard quad: 4 vertices, centered at bottom.
        // Positions in local space, billboard rotation handled in shader.
        //
        // The layout is the ENGINE's Vertex (position, normal, texcoord) rather
        // than the old 20-byte {position, texcoord}: since #1233 the same
        // vertex stage draws this card AND an authored plant mesh, and one
        // stream layout for both is what keeps the beauty, G-Buffer and shadow
        // programs from each needing a card variant and a mesh variant to drift
        // apart. The card's normal is +Y — exactly the constant the vertex
        // stage used to hard-code — so the card renders bit-identically.
        const Vertex quadVertices[] = {
            { { -0.5f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f } }, // bottom-left
            { { 0.5f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f } },  // bottom-right
            { { 0.5f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 1.0f } },  // top-right
            { { -0.5f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f } }, // top-left
        };

        u32 indices[] = { 0, 1, 2, 2, 3, 0 };

        data.QuadVBO = VertexBuffer::Create(quadVertices, static_cast<u32>(sizeof(quadVertices)));
        data.QuadVBO->SetLayout(Vertex::GetLayout());

        data.IBO = IndexBuffer::Create(indices, 6);
        data.IndexCount = 6;
    }

    bool FoliageRenderer::BuildMeshGeometry(LayerRenderData& data, const FoliageLayer& layer) const
    {
        OLO_PROFILE_FUNCTION();

        data.MeshVAO = nullptr;
        data.MeshVBO = nullptr;
        data.MeshIBO = nullptr;
        data.MeshParts.clear();
        data.MeshRayTracingIndices.clear();
        data.MeshModel = nullptr;
        data.MeshVertexCount = 0;
        data.MeshIndexCount = 0;
        data.MeshGeometryPath.clear();
        data.BoundsProfile = FoliageBoundsProfile{};

        auto model = Ref<Model>::Create(layer.MeshPath);
        if (model->GetMeshCount() == 0)
        {
            OLO_CORE_ERROR("FoliageRenderer: layer '{}' asks for the authored mesh '{}' and it did not load. The "
                           "layer draws its flat card at ALL distances instead; the census counts the variant as "
                           "unavailable. Fix the path or clear UseAuthoredMesh.",
                           layer.Name, layer.MeshPath);
            return false;
        }

        // Concatenate every submesh into ONE private vertex/index buffer, with
        // each submesh's base vertex folded into its indices. That makes a
        // submesh a plain [BaseIndex, IndexCount) range of a single buffer, so
        // the per-submesh draws differ only in a first-index offset and a
        // texture — no base-vertex plumbing through the command packet, and one
        // vertex array for the whole plant.
        const Ref<MeshSource> source = model->CreateCombinedMeshSource();
        if (!source || source->GetVertices().Num() == 0 || source->GetIndices().Num() == 0)
        {
            OLO_CORE_ERROR("FoliageRenderer: layer '{}' mesh '{}' loaded but carries no geometry "
                           "({} vertices, {} indices). Drawing the flat card instead.",
                           layer.Name, layer.MeshPath,
                           source ? source->GetVertices().Num() : 0,
                           source ? source->GetIndices().Num() : 0);
            return false;
        }

        const auto& srcVertices = source->GetVertices();
        const auto& srcIndices = source->GetIndices();
        const auto& submeshes = source->GetSubmeshes();

        std::vector<u32> indices;
        indices.reserve(static_cast<sizet>(srcIndices.Num()));

        const sizet submeshCount = submeshes.Num() > 0 ? static_cast<sizet>(submeshes.Num()) : 1;
        for (sizet i = 0; i < submeshCount; ++i)
        {
            LayerDrawPart part;
            part.BaseIndex = static_cast<u32>(indices.size());

            if (submeshes.Num() > 0)
            {
                const auto& sub = submeshes[static_cast<i32>(i)];
                for (u32 k = 0; k < sub.m_IndexCount; ++k)
                {
                    const u32 srcSlot = sub.m_BaseIndex + k;
                    if (srcSlot >= static_cast<u32>(srcIndices.Num()))
                        break;
                    indices.push_back(srcIndices[static_cast<i32>(srcSlot)] + sub.m_BaseVertex);
                }
                part.IndexCount = static_cast<u32>(indices.size()) - part.BaseIndex;
                // Per-submesh material assignment, through the submesh's OWN
                // material index — NOT the loop index.
                //
                // Model::m_Materials holds one entry per UNIQUE aiMaterial
                // (ProcessMesh dedups through m_MaterialIndexMap), while
                // CreateCombinedMeshSource emits one submesh per mesh. Those two
                // counts only coincide when every submesh has a distinct
                // material, so indexing by submesh ordinal silently picks the
                // wrong material the moment a plant reuses one — e.g. a tree
                // whose trunk and branches share bark. Model.cpp carries the
                // same warning from #629, where rebuilding the array per-mesh
                // made warm and cold loads resolve different materials.
                //
                // UINT32_MAX is the "no material resolved" sentinel
                // (Model.cpp sets it when the lookup misses); the bounds check
                // covers it the same way every other consumer in Model.cpp does.
                if (sub.m_MaterialIndex < static_cast<u32>(model->GetMaterialCount()))
                {
                    if (const Ref<Material>& material = model->GetMaterial(sub.m_MaterialIndex); material)
                    {
                        part.Albedo = material->GetAlbedoMap();
                        if (!part.Albedo)
                            part.Albedo = material->GetDiffuseMap();
                    }
                }
            }
            else
            {
                for (i32 k = 0; k < srcIndices.Num(); ++k)
                    indices.push_back(srcIndices[k]);
                part.IndexCount = static_cast<u32>(indices.size());
            }

            if (part.IndexCount > 0)
                data.MeshParts.push_back(std::move(part));
        }

        if (data.MeshParts.empty() || indices.empty())
        {
            OLO_CORE_ERROR("FoliageRenderer: layer '{}' mesh '{}' produced no drawable submesh range. "
                           "Drawing the flat card instead.",
                           layer.Name, layer.MeshPath);
            data.MeshParts.clear();
            return false;
        }

        data.MeshVBO = VertexBuffer::Create(srcVertices.GetData(),
                                            static_cast<u32>(srcVertices.Num() * sizeof(Vertex)));
        data.MeshVBO->SetLayout(Vertex::GetLayout());
        data.MeshIBO = IndexBuffer::Create(indices.data(), static_cast<u32>(indices.size()));
        data.MeshRayTracingIndices = indices;
        data.MeshVertexCount = static_cast<u32>(srcVertices.Num());
        data.MeshIndexCount = static_cast<u32>(indices.size());
        data.MeshModel = model;
        data.MeshGeometryPath = layer.MeshPath;

        // Conservative bounds from the REAL geometry (issue #1233, second
        // criterion). A quad's box is not a pine's: the canopy is wider than
        // 0.5 and the trunk can start below the origin, and culling against the
        // quad's box pops the tree out at the screen edge. The horizontal
        // half-extent is the largest XZ radius of the source AABB's corners, so
        // it holds for ANY of the per-instance Y rotations.
        // Measured from the vertices THIS path copied, not from
        // MeshSource::GetBoundingBox(): that field is populated on a fresh
        // assimp import and comes back empty on the warm .omesh cache path, so
        // reading it made the bound depend on whether the mesh had been
        // imported before in this process. The failure was invisible — the
        // plant rendered correctly and only its AABB collapsed to the card's,
        // which is a culling pop nobody sees until the canopy blinks out at the
        // screen edge. The vertices are already in hand and a foliage mesh is
        // small, so measuring is cheaper than trusting.
        // Braces, not parentheses: `BoundingBox box(glm::vec3(a), glm::vec3(b))`
        // is a function declaration, not a variable (most vexing parse).
        BoundingBox box{ glm::vec3(std::numeric_limits<f32>::max()),
                         glm::vec3(std::numeric_limits<f32>::lowest()) };
        for (i32 i = 0; i < srcVertices.Num(); ++i)
        {
            const glm::vec3& position = srcVertices[i].Position;
            box.Min = glm::min(box.Min, position);
            box.Max = glm::max(box.Max, position);
        }

        const f32 radiusXZ = std::max(std::max(std::abs(box.Min.x), std::abs(box.Max.x)),
                                      std::max(std::abs(box.Min.z), std::abs(box.Max.z)));
        // The profile keeps the CARD terms as well, because a layer with a mesh
        // still draws its card past MeshViewDistance — the bound has to hold
        // for both shapes, not just the near one.
        data.BoundsProfile = FoliageBoundsProfile{};
        data.BoundsProfile.m_HalfExtentXZHeightScaled = std::max(radiusXZ * glm::root_two<f32>(), 1e-3f);
        data.BoundsProfile.m_MinY = std::min(box.Min.y, 0.0f);
        data.BoundsProfile.m_MaxY = std::max(box.Max.y, 1.0f);

        // The authoring convention both this path and the impostor bake assume:
        // base at the origin, unit height. Neither rescales — a mesh authored at
        // some other size is drawn at the wrong size in BOTH, consistently — so
        // say so rather than let the author discover it as "my tree is tiny".
        constexpr f32 kUnitTolerance = 0.05f;
        if (std::abs(box.Max.y - 1.0f) > kUnitTolerance || std::abs(box.Min.y) > kUnitTolerance)
        {
            OLO_CORE_WARN("FoliageRenderer: layer '{}' mesh '{}' spans y in [{:.3f}, {:.3f}], not the base-at-origin "
                          "unit height ([0, 1]) foliage authoring assumes. It is scaled by the instance's "
                          "height * scale as-is, so every plant is drawn {:.2f}x the authored height — near mesh and "
                          "far impostor alike. Re-author the mesh or compensate with MinHeight/MaxHeight.",
                          layer.Name, layer.MeshPath, box.Min.y, box.Max.y,
                          std::max(box.Max.y - std::min(box.Min.y, 0.0f), 1e-3f));
        }

        OLO_CORE_INFO("FoliageRenderer: layer '{}' authored mesh '{}' ready — {} vertices, {} indices, {} submesh(es), "
                      "{:.1f} KiB geometry",
                      layer.Name, layer.MeshPath, data.MeshVertexCount, data.MeshIndexCount, data.MeshParts.size(),
                      static_cast<f32>(data.MeshVertexCount * sizeof(Vertex) + data.MeshIndexCount * sizeof(u32)) / 1024.0f);
        return true;
    }

    void FoliageRenderer::RebuildVertexArrays(LayerRenderData& data) const
    {
        // The instance stream has to be bound into EVERY vertex array the layer
        // draws from, and a capacity grow replaces that buffer — so the arrays
        // are rebuilt from the surviving geometry buffers rather than each call
        // site remembering to re-add it to both.
        if (data.QuadVBO && data.IBO)
        {
            data.VAO = VertexArray::Create();
            data.VAO->AddVertexBuffer(data.QuadVBO);
            data.VAO->SetIndexBuffer(data.IBO);
            if (data.InstanceVBO)
                data.VAO->AddInstanceBuffer(data.InstanceVBO);
        }

        if (data.MeshVBO && data.MeshIBO)
        {
            data.MeshVAO = VertexArray::Create();
            data.MeshVAO->AddVertexBuffer(data.MeshVBO);
            data.MeshVAO->SetIndexBuffer(data.MeshIBO);
            if (data.InstanceVBO)
                data.MeshVAO->AddInstanceBuffer(data.InstanceVBO);
        }
    }

    void FoliageRenderer::UploadInstances(LayerRenderData& data, const std::vector<FoliageInstanceData>& instances)
    {
        if (instances.empty())
        {
            data.InstanceCount = 0;
            return;
        }

        auto requiredCount = static_cast<u32>(instances.size());
        auto dataSize = static_cast<u32>(instances.size() * sizeof(FoliageInstanceData));

        const bool grew = data.InstanceVBO && data.InstanceCapacity < requiredCount;
        if (!data.InstanceVBO || grew)
        {
            if (grew)
            {
                OLO_CORE_INFO("FoliageRenderer: instance VBO GROW {} -> {} instances", data.InstanceCapacity,
                              requiredCount * 2);
                data.InstanceCapacity = requiredCount * 2;
            }
            else
            {
                OLO_CORE_INFO("FoliageRenderer: instance VBO create ({} instances)", requiredCount);
                data.InstanceCapacity = std::max(requiredCount, 256u);
            }

            u32 allocSize = data.InstanceCapacity * static_cast<u32>(sizeof(FoliageInstanceData));
            data.InstanceVBO = VertexBuffer::Create(allocSize);
            data.InstanceVBO->SetLayout({
                { ShaderDataType::Float4, "a_PositionScale" },
                { ShaderDataType::Float4, "a_RotationHeight" },
                { ShaderDataType::Float4, "a_ColorAlpha" },
            });
            // A new instance buffer has to reach EVERY vertex array the layer
            // draws from — the card's and, since #1233, the authored mesh's.
            // Rebuilding them both also avoids the duplicate attribute bindings
            // that re-adding an instance buffer to a live array would leave.
            RebuildVertexArrays(data);
        }

        data.InstanceVBO->SetData({ instances.data(), dataSize });
        data.InstanceCount = requiredCount;
    }

    void FoliageRenderer::EnumerateLayerDraws(const LayerRenderData& data, std::vector<LayerDraw>& out) const
    {
        out.clear();
        if (data.InstanceCount == 0)
            return;

        const bool meshDrawable = data.MeshVAO && !data.MeshParts.empty() && data.MeshViewDistance > 0.0f;
        const f32 handoverStart = meshDrawable ? data.MeshFadeStartDistance : 0.0f;
        const f32 handoverEnd = meshDrawable ? data.MeshViewDistance : 0.0f;

        // Near field: the authored plant mesh, one draw per submesh so a plant
        // whose bark and leaves are different materials renders as authored
        // (issue #1233, first criterion).
        if (meshDrawable)
        {
            for (const auto& part : data.MeshParts)
            {
                LayerDraw draw;
                draw.VAO = data.MeshVAO;
                draw.BaseIndex = part.BaseIndex;
                draw.IndexCount = part.IndexCount;
                draw.Albedo = part.Albedo ? part.Albedo : data.AlbedoTexture;
                draw.IsAuthoredMesh = true;
                draw.HandoverStart = handoverStart;
                draw.HandoverEnd = handoverEnd;
                draw.FadeStart = data.FadeStartDistance;
                draw.ViewDistance = data.ViewDistance;
                out.push_back(std::move(draw));
            }
        }

        // Far field: the flat card, which the impostor path also rides. It
        // carries the SAME hand-over band as the mesh draws above, and keeps
        // exactly the pixels they do not. With no mesh the band is zero-width
        // and this is the single draw the layer has always emitted.
        if (data.VAO && data.IndexCount > 0)
        {
            LayerDraw draw;
            draw.VAO = data.VAO;
            draw.BaseIndex = 0;
            draw.IndexCount = data.IndexCount;
            draw.Albedo = data.AlbedoTexture;
            draw.IsAuthoredMesh = false;
            draw.HandoverStart = handoverStart;
            draw.HandoverEnd = handoverEnd;
            draw.FadeStart = data.FadeStartDistance;
            draw.ViewDistance = data.ViewDistance;
            out.push_back(std::move(draw));
        }
    }

    void FoliageRenderer::GenerateInstances(
        const std::vector<FoliageLayer>& layers,
        const TerrainData& terrainData,
        const TerrainMaterial* material,
        f32 worldSizeX, f32 worldSizeZ, f32 heightScale)
    {
        OLO_PROFILE_FUNCTION();

        m_WindHistory.Reset();

        // A shrinking layer list drops the trailing LayerRenderData entries
        // below — free their impostor VRAM budget claims first, or resize()
        // destroying them silently leaks the claims for the rest of the
        // process (issue #718; ImpostorAtlas has no destructor of its own).
        for (sizet i = layers.size(); i < m_Layers.size(); ++i)
            ImpostorBaker::Free(m_Layers[i].Impostor);
        m_Layers.resize(layers.size());

        // Canonical identity (issue #1230). Everything live becomes a candidate
        // for survival; a placement this pass does not re-emit — because its
        // layer vanished, was disabled, or its cell stopped qualifying — retires
        // at EndGeneration and its id is never handed to another plant.
        m_Registry.BeginGeneration(layers);

        // One CPU/GPU height sync for the whole generation rather than two per
        // grid cell, which is what going through TerrainData::GetHeightAt and
        // GetNormalAt cost (each calls SyncFromGPU).
        const std::vector<f32>& heights = terrainData.GetHeightData();
        const u32 heightResolution = terrainData.GetResolution();

        std::vector<FoliagePlacement::Placement> placements;
        std::vector<FoliageInstanceData> instances;

        for (sizet layerIdx = 0; layerIdx < layers.size(); ++layerIdx)
        {
            const auto& layer = layers[layerIdx];
            auto& renderData = m_Layers[layerIdx];

            if (!layer.Enabled || layer.Density <= 0.0f)
            {
                // Draws nothing, so it owns no canonical instances. No
                // BeginLayer means EndGeneration retires whatever it had;
                // re-enabling the layer issues FRESH ids rather than reviving
                // the old ones, which is the deterministic answer and never a
                // silent reuse.
                renderData.InstanceCount = 0;
                continue;
            }

            // Geometry. The card is always built — it is what covers the
            // distance band and what a layer with no authored mesh draws
            // everywhere. The authored mesh (issue #1233) is built beside it,
            // never instead of it, and only re-imported when the path changes.
            bool geometryChanged = false;
            if (!renderData.QuadVBO)
            {
                BuildQuadGeometry(renderData);
                geometryChanged = true;
            }

            const bool meshRequested = layer.UseAuthoredMesh && !layer.MeshPath.empty();
            if (!meshRequested)
            {
                if (renderData.MeshVBO || !renderData.MeshGeometryPath.empty())
                {
                    renderData.MeshVAO = nullptr;
                    renderData.MeshVBO = nullptr;
                    renderData.MeshIBO = nullptr;
                    renderData.MeshParts.clear();
                    renderData.MeshModel = nullptr;
                    renderData.MeshVertexCount = 0;
                    renderData.MeshIndexCount = 0;
                    renderData.MeshGeometryPath.clear();
                    renderData.BoundsProfile = FoliageBoundsProfile{};
                    geometryChanged = true;
                }
            }
            else if (renderData.MeshGeometryPath != layer.MeshPath)
            {
                BuildMeshGeometry(renderData, layer);
                // Recorded even when the import FAILED, so a broken path is
                // reported once per edit rather than re-imported and re-logged
                // on every regeneration.
                renderData.MeshGeometryPath = layer.MeshPath;
                geometryChanged = true;
            }
            renderData.MeshRequested = meshRequested;

            if (geometryChanged || !renderData.VAO)
            {
                RebuildVertexArrays(renderData);
            }

            // Store layer render properties
            renderData.ViewDistance = layer.ViewDistance;
            renderData.FadeStartDistance = layer.FadeStartDistance;
            renderData.WindStrength = layer.WindStrength;
            renderData.WindSpeed = layer.WindSpeed;
            renderData.WindWeights = SanitizeFoliageWind(layer.WindStiffness, layer.WindBranchWeight, layer.WindLeafWeight, layer.WindDebugDisplacement);
            // Sanitised here, once, rather than at each of the three UBO-fill
            // sites: this number scales a displacement AND derives the bound
            // that displacement is padded into, so the two must come from the
            // same value or a layer can bend further than its own AABB.
            renderData.InteractionResponse =
                std::isfinite(layer.InteractionResponse) ? std::clamp(layer.InteractionResponse, 0.0f, 8.0f) : 1.0f;
            renderData.BaseColor = layer.BaseColor;
            renderData.AlphaCutoff = layer.AlphaCutoff;

            // Near-field hand-over band (issue #1233). Sanitised here rather
            // than trusted: these reach a smoothstep in the vertex and fragment
            // stages, where a NaN or an inverted band silently drops the layer.
            const bool meshDrawable = meshRequested && renderData.MeshVBO && !renderData.MeshParts.empty();
            if (meshDrawable)
            {
                renderData.MeshViewDistance = std::isfinite(layer.MeshViewDistance)
                                                  ? std::max(layer.MeshViewDistance, 0.0f)
                                                  : 30.0f;
                renderData.MeshFadeStartDistance = std::isfinite(layer.MeshFadeStartDistance)
                                                       ? std::clamp(layer.MeshFadeStartDistance, 0.0f,
                                                                    renderData.MeshViewDistance)
                                                       : std::min(22.0f, renderData.MeshViewDistance);
            }
            else
            {
                // No mesh: the card covers everything, exactly as before #1233.
                renderData.MeshViewDistance = 0.0f;
                renderData.MeshFadeStartDistance = 0.0f;
            }

            // Load albedo texture if needed — foliage albedo is authored
            // colour and needs sRGB->linear conversion on sample.
            if (!layer.AlbedoPath.empty() && !renderData.AlbedoTexture)
            {
                renderData.AlbedoTexture = Texture2D::Create(layer.AlbedoPath, /*srgb=*/true);
            }

            // ── The leaf material (issue #1234) ─────────────────────────────
            // All three maps are LINEAR data, not authored colour: a tangent
            // normal, a roughness and a thickness. sRGB-decoding any of them
            // would bend the normals and darken the roughness by a gamma
            // nobody could see in the inspector.
            //
            // Keyed on the PATH: a changed path re-opens, an unchanged one does
            // not, and CLEARING the path drops the Ref so the map bitfield goes
            // back to "not authored" instead of leaving the last texture bound.
            const auto loadLeafMap = [](const std::string& path, std::string& loadedPath,
                                        Ref<Texture2D>& texture)
            {
                // The PATH is the cache key, and it is recorded even when the
                // load FAILS — same rule as the authored mesh above, for the
                // same reason: a broken path is re-opened and re-logged on
                // every regeneration otherwise. Clearing the path in the
                // inspector drops the Ref, so the map bitfield goes back to
                // "not authored" rather than leaving the last texture bound.
                if (loadedPath == path)
                    return;
                loadedPath = path;
                texture = path.empty() ? nullptr : Texture2D::Create(path, /*srgb=*/false);

                // Texture2D::Create NEVER RETURNS NULL — a file that will not
                // open still yields a Ref, and IsLoaded() is the only thing
                // that says so. Dropping the Ref here is what makes the failure
                // behave as "no map authored" (the shader uses the layer's
                // constant, because the map bitfield is derived from the
                // HANDLE) instead of sampling whatever the failed texture
                // object contains. Never a silent fallback: a leaf shaded by a
                // normal the author did not write is worse than one shaded by
                // the constant they did.
                if (texture && !texture->IsLoaded())
                {
                    OLO_CORE_WARN("FoliageRenderer - leaf map '{}' could not be loaded; the layer shades with its "
                                  "authored constant instead of the map.",
                                  path);
                    texture = nullptr;
                }
            };
            loadLeafMap(layer.NormalMapPath, renderData.LoadedNormalPath, renderData.LeafNormalTexture);
            loadLeafMap(layer.RoughnessMapPath, renderData.LoadedRoughnessPath, renderData.LeafRoughnessTexture);
            loadLeafMap(layer.ThicknessMapPath, renderData.LoadedThicknessPath, renderData.LeafThicknessTexture);

            // The scalars, sanitised here as well as at both deserializers —
            // the inspector writes straight into the component, so a value
            // typed into the editor never passes through either of those.
            // Every one of these reaches a pow() exponent, a normalize() or a
            // clamp bound in the shared shader evaluation.
            const auto finiteOr = [](f32 v, f32 fallback, f32 lo, f32 hi)
            { return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback; };
            renderData.LeafRoughness = finiteOr(layer.Roughness, 0.8f, 0.02f, 1.0f);
            renderData.LeafNormalStrength = finiteOr(layer.NormalStrength, 1.0f, 0.0f, 4.0f);
            renderData.LeafThickness = finiteOr(layer.Thickness, 0.5f, 0.0f, 1.0f);
            renderData.LeafTransmissionStrength = finiteOr(layer.TransmissionStrength, 0.0f, 0.0f, 8.0f);
            renderData.LeafTransmissionColor =
                glm::vec3(finiteOr(layer.TransmissionColor.x, 0.42f, 0.0f, 1.0f),
                          finiteOr(layer.TransmissionColor.y, 0.62f, 0.0f, 1.0f),
                          finiteOr(layer.TransmissionColor.z, 0.18f, 0.0f, 1.0f));
            renderData.LeafTransmissionDistortion = finiteOr(layer.TransmissionDistortion, 0.35f, 0.0f, 1.0f);
            renderData.LeafTransmissionPower = finiteOr(layer.TransmissionPower, 4.0f, 1.0f, 64.0f);
            renderData.LeafTransmissionWrap = finiteOr(layer.TransmissionWrap, 0.5f, 0.0f, 1.0f);
            renderData.LeafTransmissionAmbient = finiteOr(layer.TransmissionAmbient, 0.35f, 0.0f, 4.0f);

            // Octahedral impostor LOD (issue #433): store the per-layer params and
            // bake/re-bake the atlas from the layer mesh if needed.
            renderData.UseImpostor = layer.UseImpostor;
            renderData.ImpostorStartDistance = layer.ImpostorStartDistance;
            renderData.ImpostorTransitionBand = layer.ImpostorTransitionBand;
            UpdateImpostorAtlas(renderData, layer);

            FoliagePlacement::GenerateLayer(layer, static_cast<u32>(layerIdx), heights, heightResolution,
                                            material, worldSizeX, worldSizeZ, heightScale, placements);

            // Explicit representation metadata, not a flag a consumer has to
            // re-derive. A layer that asked for an authored mesh or an impostor
            // and got neither still draws as a flat card, so its instances are
            // MeshCard — but the VARIANT it authored is unavailable, and that is
            // counted rather than left to the one-off log line.
            const bool impostorRequested = layer.UseImpostor;
            const bool impostorAvailable = impostorRequested && renderData.Impostor.IsValid();
            if (impostorAvailable)
            {
                if (!m_ImpostorDepthShader)
                    m_ImpostorDepthShader = Shader::Create("assets/shaders/Foliage_Impostor_Depth.glsl");
                const f32 radius = FoliageImpostorBoundsRadius(renderData.Impostor.Radius);
                renderData.BoundsProfile.m_HalfExtentXZHeightScaled = std::max(renderData.BoundsProfile.m_HalfExtentXZHeightScaled, radius);
                renderData.BoundsProfile.m_MinY = std::min(renderData.BoundsProfile.m_MinY, 0.5f - radius);
                renderData.BoundsProfile.m_MaxY = std::max(renderData.BoundsProfile.m_MaxY, 0.5f + radius);
            }
            // The NEAR field names the representation, because that is what the
            // instance's bounds and its material assignment are derived from:
            // an authored-mesh plant still hands over to a card or an impostor
            // at distance, and reporting it as a card would hide the geometry
            // that actually costs and actually bounds (issue #1233).
            FoliageRepresentation representation = FoliageRepresentation::Unsupported;
            if (meshDrawable)
            {
                representation = FoliageRepresentation::AuthoredMesh;
            }
            else if (impostorAvailable)
            {
                representation = FoliageRepresentation::Impostor;
            }
            else if (renderData.VAO)
            {
                representation = FoliageRepresentation::MeshCard;
            }

            // Either authored variant asked for and not delivered counts, and
            // BuildMeshGeometry / UpdateImpostorAtlas have already said which,
            // loudly, in the log.
            const bool variantUnavailable = (impostorRequested && !impostorAvailable) ||
                                            (meshRequested && !meshDrawable);

            renderData.BoundsProfile.m_WindDisplacement = FoliageWindMaximumDisplacement(layer.WindStrength, renderData.WindWeights, m_LegacyWindEnvelope);
            // Derived from the AUTHORED response, not from whatever the field
            // happens to hold this frame: the profile is hashed into every
            // instance's identity, so a bound that moved with a passing actor
            // would retire and re-issue the whole layer's ids as it walked past.
            renderData.BoundsProfile.m_InteractionDisplacement =
                FoliageInteractionMaximumDisplacement(renderData.InteractionResponse);
            m_Registry.BeginLayer(static_cast<u32>(layerIdx), layer,
                                  FoliagePlacement::SeedForLayer(static_cast<u32>(layerIdx)),
                                  FoliagePlacement::SpacingForDensity(layer.Density),
                                  worldSizeX, worldSizeZ,
                                  representation, variantUnavailable, renderData.BoundsProfile);

            // The buffer row is assigned here and recorded as a PROJECTION of
            // the record. Identity comes from the placement cell, so a
            // regeneration that emits the same plants in a different order
            // leaves every id untouched.
            instances.clear();
            instances.reserve(placements.size());
            for (const auto& placement : placements)
            {
                m_Registry.AddInstance(placement.m_CellX, placement.m_CellZ, placement.m_Row,
                                       static_cast<u32>(instances.size()));
                auto row = placement.m_Row;
                row.RotationHeight.w = FoliageWindPhase(m_Registry.GetRecords().back().m_Id);
                instances.push_back(row);
            }
            m_Registry.EndLayer();

            // Compute bounding box from all instance positions (with height expansion)
            if (!instances.empty())
            {
                glm::vec3 bMin(std::numeric_limits<f32>::max());
                glm::vec3 bMax(std::numeric_limits<f32>::lowest());
                for (const auto& inst : instances)
                {
                    const glm::vec3 pos(inst.PositionScale.x, inst.PositionScale.y, inst.PositionScale.z);
                    // ONE bounds rule for the per-layer AABB and the registry's
                    // per-instance records — the same function, so a mesh that
                    // widens one cannot leave the other bounding a quad.
                    const BoundingBox instanceBox = FoliageInstanceBounds(
                        pos, inst.PositionScale.w, inst.RotationHeight.y, renderData.BoundsProfile);
                    bMin = glm::min(bMin, instanceBox.Min);
                    bMax = glm::max(bMax, instanceBox.Max);
                }
                renderData.Bounds = BoundingBox(bMin, bMax);
            }
            else
            {
                renderData.Bounds = BoundingBox(glm::vec3(0.0f), glm::vec3(0.0f));
            }

            UploadInstances(renderData, instances);
        }

        m_Registry.EndGeneration();
    }

    void FoliageRenderer::ClearInstances()
    {
        m_Registry.Clear();
        for (auto& layer : m_Layers)
        {
            layer.InstanceCount = 0;
        }
    }

    void FoliageRenderer::Render(
        [[maybe_unused]] const Frustum& frustum,
        [[maybe_unused]] const glm::vec3& cameraPos,
        const Ref<Shader>& shader)
    {
        OLO_PROFILE_FUNCTION();

        if (!shader)
        {
            return;
        }

        shader->Bind();
        m_VisibleInstances = 0;

        // Foliage's per-blade transforms come from its own instance VBO
        // (a_PositionScale, a_RotationHeight) in TERRAIN-LOCAL space — the same
        // space the terrain mesh is authored in, because GenerateInstances
        // derives them straight from the heightfield (x/z in [0, WorldSize], y
        // the raw sampled height, no base offset). The shaders read `u_Model`
        // from the engine's ModelInstanceBuffer (binding 15). Upload the owning
        // terrain's render-relative model matrix once so it (a) overrides
        // whatever the previous DrawMesh wrote into the SSBO, (b) places the
        // plants on their island, and (c) shifts them into render-relative
        // space — camera-relative rendering (issue #429). The foliage shaders
        // add u_RenderOrigin back for the world-anchored wind field.
        //
        // This used to upload plain identity, on the stated belief that the
        // instance positions were already absolute world (issue #953). They are
        // not, and no island in Drift sits at the origin, so all six islands'
        // foliage was drawn in one heap over open water near (0,0,0) — read
        // from the boat as a swarm of dark specks hanging in the sky.
        if (auto instanceBuffer = Renderer3D::GetModelInstanceBuffer())
        {
            InstanceData bladeModel{};
            bladeModel.EntityID = -1;
            bladeModel.Transform = MakeModelRelative(m_TerrainTransform, Renderer3D::GetRenderOrigin());
            bladeModel.PrevTransform = bladeModel.Transform;
            const std::span<const InstanceData> one(&bladeModel, 1);
            instanceBuffer->Upload(one);
            instanceBuffer->Bind();
        }

        // The main view's position, made render-relative exactly as
        // CommandDispatch makes it for the camera UBO — the hand-over between a
        // plant's mesh and its card is measured from here in every pass.
        const glm::vec3 renderRelativeViewPos =
            MakePositionRelative(CommandDispatch::GetViewPosition(), Renderer3D::GetRenderOrigin());

        std::vector<LayerDraw> draws;
        for (auto& layer : m_Layers)
        {
            if (layer.InstanceCount == 0)
                continue;

            EnumerateLayerDraws(layer, draws);
            for (const auto& draw : draws)
            {
                // Upload per-draw foliage UBO
                ShaderBindingLayout::FoliageUBO foliageUBOData{};
                foliageUBOData.Time = m_Time;
                foliageUBOData.WindStrength = layer.WindStrength;
                foliageUBOData.WindSpeed = layer.WindSpeed;
                foliageUBOData.WindWeights = layer.WindWeights;
                const auto wind = WindSystem::GetGPUData();
                foliageUBOData.WindDirection = wind.DirectionAndSpeed;
                foliageUBOData.WindGust = wind.GustAndTurbulence;
                foliageUBOData.WindClock = wind.TimeAndFlags;
                foliageUBOData.PrevMeshViewPos = glm::vec4(MakePositionRelative(Renderer3D::GetPreviousViewPosition(), Renderer3D::GetRenderOrigin()), 0.0f);
                foliageUBOData.WindFlags = glm::vec4(Renderer3D::GetRenderOrigin(), wind.TimeAndFlags.y);
                foliageUBOData.WindHistoryValid = WindSystem::HasStableParameters() ? 1.0f : 0.0f;
                foliageUBOData.ViewDistance = draw.ViewDistance;
                foliageUBOData.FadeStart = draw.FadeStart;
                foliageUBOData.AlphaCutoff = layer.AlphaCutoff;
                foliageUBOData.PrevTime = m_PrevTime;
                foliageUBOData.BaseColor = glm::vec4(layer.BaseColor, 0.0f);
                foliageUBOData.MeshParams = glm::vec4(draw.IsAuthoredMesh ? 1.0f : 0.0f,
                                                      draw.HandoverStart, draw.HandoverEnd, 0.0f);
                foliageUBOData.MeshViewPos = glm::vec4(renderRelativeViewPos, 0.0f);
                // The interaction field (issue #1238). Read from the field
                // itself, exactly as the wind snapshot above is — the set is
                // global per frame, so only the layer's response is per draw.
                ApplyFoliageInteraction(foliageUBOData, layer.InteractionResponse);

                // The leaf material (issue #1234). Filled here so this path
                // stays coherent: a default-constructed FoliageUBO would upload
                // LeafSurface.x = 0 — a roughness of zero, i.e. a mirror leaf —
                // rather than the layer's authored value.
                //
                // NOT AT PARITY WITH THE COMMAND PATH, AND SAY SO. Render() has
                // no callers anywhere in the tree today — every foliage draw
                // goes through Renderer3D::DrawFoliageLayer and the command
                // bucket — and unlike that dispatch it binds neither the shadow
                // contract nor the global IBL trio. Reviving it means porting
                // those binds too, or a canopy here lights from whatever a
                // previous draw happened to leave on those slots.
                f32 leafMapFlags = 0.0f;
                if (layer.LeafNormalTexture)
                    leafMapFlags += 1.0f; // OLO_LEAF_MAP_NORMAL
                if (layer.LeafRoughnessTexture)
                    leafMapFlags += 2.0f; // OLO_LEAF_MAP_ROUGHNESS
                if (layer.LeafThicknessTexture)
                    leafMapFlags += 4.0f; // OLO_LEAF_MAP_THICKNESS
                foliageUBOData.LeafSurface = glm::vec4(layer.LeafRoughness, layer.LeafNormalStrength,
                                                       layer.LeafThickness, leafMapFlags);
                foliageUBOData.LeafTransmit =
                    glm::vec4(layer.LeafTransmissionColor * layer.LeafTransmissionStrength,
                              layer.LeafTransmissionStrength);
                foliageUBOData.LeafLobe =
                    glm::vec4(layer.LeafTransmissionDistortion, layer.LeafTransmissionPower,
                              layer.LeafTransmissionWrap, layer.LeafTransmissionAmbient);
                // This path never writes a G-Buffer, so no slot is interned for
                // it: the forward program carries the lobe in the two lanes
                // above and reads no slot. kFoliageLeafSlotNone says so.
                {
                    const bool globalIblBound = Renderer3D::GetGlobalIrradianceMapHandle().IsValid() &&
                                                Renderer3D::GetGlobalPrefilterMapHandle().IsValid() &&
                                                Renderer3D::GetGlobalBRDFLutMapHandle().IsValid();
                    foliageUBOData.LeafIds = glm::vec4(static_cast<f32>(kFoliageLeafSlotNone),
                                                       globalIblBound ? 1.0f : 0.0f,
                                                       Renderer3D::GetGlobalIBLIntensity(), 0.0f);
                }

                // The leaf maps, through the SAME seam the albedo goes through
                // below — a direct Texture::Bind is invisible to the heap.
                if (layer.LeafNormalTexture)
                    HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_NORMAL,
                                                     layer.LeafNormalTexture->GetRHIHandle(),
                                                     RHI::HeapSlotLifetime::Persistent);
                if (layer.LeafRoughnessTexture)
                    HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_ROUGHNESS,
                                                     layer.LeafRoughnessTexture->GetRHIHandle(),
                                                     RHI::HeapSlotLifetime::Persistent);
                if (layer.LeafThicknessTexture)
                    HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_METALLIC,
                                                     layer.LeafThicknessTexture->GetRHIHandle(),
                                                     RHI::HeapSlotLifetime::Persistent);

                auto foliageUBO = Renderer3D::GetFoliageUBO();
                foliageUBO->SetData(&foliageUBOData, ShaderBindingLayout::FoliageUBO::GetSize());

                // Bind albedo texture. THROUGH THE SEAM, not Texture::Bind — a direct
                // bind is invisible to the heap, so a converted Foliage_Instance would
                // read an offset nobody staged (issue #691). Persistent: the
                // atlas is asset-owned and outlives the frame.
                if (draw.Albedo)
                {
                    HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_DIFFUSE,
                                                     draw.Albedo->GetRHIHandle(),
                                                     RHI::HeapSlotLifetime::Persistent);
                }

                draw.VAO->Bind();
                HeapBinding::FlushOffsets();
                RenderCommand::DrawIndexedInstancedRaw(draw.VAO->GetRHIHandle(), draw.IndexCount,
                                                       draw.BaseIndex, layer.InstanceCount);
            }
            m_VisibleInstances += layer.InstanceCount;
        }
    }

    void FoliageRenderer::RenderShadows(const Ref<Shader>& depthShader, f32 time) const
    {
        OLO_PROFILE_FUNCTION();

        if (!depthShader)
        {
            return;
        }

        depthShader->Bind();

        // Same render-relative model pattern as Render() (issue #429): the depth
        // shader reads u_Model from the ModelInstanceBuffer and foliage's per-blade
        // data is TERRAIN-LOCAL (see Render), so go through the owning terrain's
        // transform and then -renderOrigin, to render the shadow caster in the
        // same render-relative space as the shifted lightVP. Must stay identical
        // to the main pass or the shadow detaches from the plant (issue #953).
        if (auto instanceBuffer = Renderer3D::GetModelInstanceBuffer())
        {
            InstanceData bladeModel{};
            bladeModel.EntityID = -1;
            bladeModel.Transform = MakeModelRelative(m_TerrainTransform, Renderer3D::GetRenderOrigin());
            bladeModel.PrevTransform = bladeModel.Transform;
            const std::span<const InstanceData> one(&bladeModel, 1);
            instanceBuffer->Upload(one);
            instanceBuffer->Bind();
        }

        // The SAME draw list the beauty pass walks (issue #1233, fourth
        // criterion): a shadow cast from a quad while the lit plant is a pine
        // passes every CPU test and reads downstream as a completely different
        // bug. EnumerateLayerDraws is the one place that decides.
        // See Render(): the MAIN view, not the shadow camera.
        const glm::vec3 renderRelativeViewPos =
            MakePositionRelative(CommandDispatch::GetViewPosition(), Renderer3D::GetRenderOrigin());

        std::vector<LayerDraw> draws;
        for (auto& layer : m_Layers)
        {
            if (layer.InstanceCount == 0)
                continue;

            EnumerateLayerDraws(layer, draws);
            for (const auto& draw : draws)
            {
                const bool impostor = !draw.IsAuthoredMesh && layer.UseImpostor && layer.Impostor.IsValid();
                const auto& program = impostor ? m_ImpostorDepthShader : depthShader;
                if (!program || !program->IsReady())
                    continue;
                program->Bind();

                // Upload per-draw foliage UBO for depth pass
                ShaderBindingLayout::FoliageUBO foliageUBOData{};
                foliageUBOData.Time = m_Time;
                foliageUBOData.PrevTime = m_PrevTime;
                foliageUBOData.WindStrength = layer.WindStrength;
                foliageUBOData.WindSpeed = layer.WindSpeed;
                foliageUBOData.WindWeights = layer.WindWeights;
                const auto wind = WindSystem::GetGPUData();
                foliageUBOData.WindDirection = wind.DirectionAndSpeed;
                foliageUBOData.WindGust = wind.GustAndTurbulence;
                foliageUBOData.WindClock = wind.TimeAndFlags;
                foliageUBOData.PrevMeshViewPos = glm::vec4(MakePositionRelative(Renderer3D::GetPreviousViewPosition(), Renderer3D::GetRenderOrigin()), 0.0f);
                foliageUBOData.WindFlags = glm::vec4(Renderer3D::GetRenderOrigin(), wind.TimeAndFlags.y);
                foliageUBOData.WindHistoryValid = WindSystem::HasStableParameters() ? 1.0f : 0.0f;
                foliageUBOData.AlphaCutoff = layer.AlphaCutoff;
                foliageUBOData.MeshParams = glm::vec4(draw.IsAuthoredMesh ? 1.0f : 0.0f,
                                                      draw.HandoverStart, draw.HandoverEnd, 0.0f);
                // The MAIN view's position, not this pass's camera — that one is
                // the light. Without it the shadow pass would pick the mesh where
                // the lit frame drew the card and the plant's shadow would be a
                // different shape than the plant (issue #1233, fourth criterion).
                foliageUBOData.MeshViewPos = glm::vec4(renderRelativeViewPos, 0.0f);
                // The SAME influence set the lit pass reads (issue #1238).
                // Without it a plant bends and its shadow does not, which reads
                // as a detached shadow rather than as a missing feature.
                ApplyFoliageInteraction(foliageUBOData, layer.InteractionResponse);
                if (impostor)
                {
                    foliageUBOData.ViewDistance = draw.ViewDistance;
                    foliageUBOData.FadeStart = draw.FadeStart;
                    foliageUBOData.ImpostorParams0 = glm::vec4(layer.Impostor.FramesPerAxis, layer.Impostor.Hemi ? 1.0f : 0.0f, layer.ImpostorStartDistance, layer.ImpostorTransitionBand);
                    foliageUBOData.ImpostorParams1 = glm::vec4(1.0f, layer.Impostor.Radius, 0.5f, 0.0f);
                    HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_DIFFUSE, layer.Impostor.Albedo->GetRHIHandle(), RHI::HeapSlotLifetime::Persistent);
                    HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_SPECULAR, layer.Impostor.NormalDepth->GetRHIHandle(), RHI::HeapSlotLifetime::Persistent);
                }

                auto foliageUBO = Renderer3D::GetFoliageUBO();
                foliageUBO->SetData(&foliageUBOData, ShaderBindingLayout::FoliageUBO::GetSize());
                foliageUBO->Bind();

                // Bind albedo for alpha test in shadow pass (see the seam note above).
                if (!impostor)
                {
                    const auto albedo = draw.Albedo ? draw.Albedo : Renderer3D::GetWhiteTexture();
                    if (!albedo || !albedo->IsLoaded())
                        continue;
                    HeapBinding::BindTextureOrOffset(ShaderBindingLayout::TEX_DIFFUSE,
                                                     albedo->GetRHIHandle(),
                                                     RHI::HeapSlotLifetime::Persistent);
                }

                draw.VAO->Bind();
                HeapBinding::FlushOffsets();
                RenderCommand::DrawIndexedInstancedRaw(draw.VAO->GetRHIHandle(), draw.IndexCount,
                                                       draw.BaseIndex, layer.InstanceCount);
            }
        }
    }

    u32 FoliageRenderer::GetTotalInstanceCount() const
    {
        u32 total = 0;
        for (const auto& layer : m_Layers)
        {
            total += layer.InstanceCount;
        }
        return total;
    }

    void FoliageRenderer::UpdateImpostorAtlas(LayerRenderData& data, const FoliageLayer& layer)
    {
        OLO_PROFILE_FUNCTION();

        if (!layer.UseImpostor || layer.MeshPath.empty())
        {
            // Impostor turned off (or no mesh) — drop any stale atlas. Free
            // its VRAM budget claim (issue #718) before discarding it.
            ImpostorBaker::Free(data.Impostor);
            data.Impostor = ImpostorAtlas{};
            data.ImpostorBakedMeshPath.clear();
            data.ImpostorBakedFrames = 0;
            data.ImpostorBakedResolution = 0;
            return;
        }

        // Re-bake only when anything the atlas is baked FROM changed: mesh, grid,
        // atlas resolution, layout, AND the material inputs (albedo texture path,
        // tint, alpha cutoff) — the bake bakes those in, so a tint/cutoff change
        // with the same mesh must still re-bake or the atlas goes stale.
        const bool upToDate = data.Impostor.IsValid() && data.ImpostorBakedMeshPath == layer.MeshPath && data.ImpostorBakedFrames == layer.ImpostorFramesPerAxis && data.ImpostorBakedResolution == layer.ImpostorAtlasResolution && data.ImpostorBakedHemi == layer.ImpostorHemiOctahedral && data.ImpostorBakedAlbedoPath == layer.AlbedoPath && Math::BitwiseEqual(data.ImpostorBakedBaseColor, layer.BaseColor) && Math::BitwiseEqual(data.ImpostorBakedAlphaCutoff, layer.AlphaCutoff);
        if (upToDate)
            return;

        // Reuse the copy the authored-mesh path already imported for this exact
        // path (issue #1233) rather than parsing the file a second time — the
        // bake and the near geometry are framed from the SAME source, which is
        // also what keeps the impostor card and the mesh the same tree.
        Ref<Model> owned;
        if (!data.MeshModel || data.MeshGeometryPath != layer.MeshPath)
            owned = Ref<Model>::Create(layer.MeshPath);
        const Model& model = owned ? *owned : *data.MeshModel;
        if (model.GetMeshCount() == 0)
        {
            OLO_CORE_WARN("FoliageRenderer: impostor layer '{}' mesh '{}' failed to load — impostor disabled for this layer",
                          layer.Name, layer.MeshPath);
            ImpostorBaker::Free(data.Impostor);
            data.Impostor = ImpostorAtlas{};
            return;
        }

        // Bake with the layer albedo (if any) applied to the mesh UVs, tinted by
        // BaseColor. A mesh with its own material texture is a natural follow-up.
        // Free the OUTGOING atlas's budget claim first — Bake() below reserves
        // a fresh one, and freeing after would either double-count briefly or,
        // worse, free the NEW claim if the assignment races the wrong way.
        ImpostorBaker::Free(data.Impostor);
        const Ref<Mesh> mesh = model.GetMesh(0);
        const Ref<Texture2D> albedo = data.AlbedoTexture; // may be null -> white fallback
        data.Impostor = ImpostorBaker::Bake(
            mesh, albedo, layer.BaseColor,
            layer.ImpostorFramesPerAxis, layer.ImpostorAtlasResolution,
            layer.ImpostorHemiOctahedral, layer.AlphaCutoff);

        if (data.Impostor.IsValid())
        {
            data.ImpostorBakedMeshPath = layer.MeshPath;
            data.ImpostorBakedAlbedoPath = layer.AlbedoPath;
            data.ImpostorBakedBaseColor = layer.BaseColor;
            data.ImpostorBakedAlphaCutoff = layer.AlphaCutoff;
            data.ImpostorBakedFrames = layer.ImpostorFramesPerAxis;
            data.ImpostorBakedResolution = layer.ImpostorAtlasResolution;
            data.ImpostorBakedHemi = layer.ImpostorHemiOctahedral;
        }
    }

    std::vector<FoliageLayerDrawInfo> FoliageRenderer::GetActiveLayerDrawInfo() const
    {
        std::vector<FoliageLayerDrawInfo> result;
        result.reserve(m_Layers.size());

        std::vector<LayerDraw> draws;
        for (u32 layerIndex = 0; layerIndex < static_cast<u32>(m_Layers.size()); ++layerIndex)
        {
            const auto& layer = m_Layers[layerIndex];
            if (layer.InstanceCount == 0)
            {
                continue;
            }

            // Same enumeration as Render / RenderShadows — see EnumerateLayerDraws.
            EnumerateLayerDraws(layer, draws);
            for (const auto& draw : draws)
            {
                FoliageLayerDrawInfo info;
                info.LayerIndex = layerIndex;
                info.VertexArrayID = draw.VAO->GetRHIHandle();
                info.BaseIndex = draw.BaseIndex;
                info.IndexCount = draw.IndexCount;
                info.InstanceCount = layer.InstanceCount;
                info.AlbedoTextureID = draw.Albedo ? draw.Albedo->GetRHIHandle() : RHI::NullResource;
                info.IsAuthoredMesh = draw.IsAuthoredMesh;
                info.MeshHandoverStartDistance = draw.HandoverStart;
                info.MeshHandoverEndDistance = draw.HandoverEnd;
                info.ViewDistance = draw.ViewDistance;
                info.FadeStartDistance = draw.FadeStart;
                info.WindStrength = layer.WindStrength;
                info.WindSpeed = layer.WindSpeed;
                info.WindWeights = layer.WindWeights;
                info.InteractionResponse = layer.InteractionResponse;
                info.BaseColor = layer.BaseColor;
                info.AlphaCutoff = layer.AlphaCutoff;
                info.Bounds = layer.Bounds;

                // The leaf material (issue #1234), identical on every draw the
                // layer emits — see FoliageLayerDrawInfo for why that is the
                // point rather than a convenience.
                info.LeafNormalTextureID =
                    layer.LeafNormalTexture ? layer.LeafNormalTexture->GetRHIHandle() : RHI::NullResource;
                info.LeafRoughnessTextureID =
                    layer.LeafRoughnessTexture ? layer.LeafRoughnessTexture->GetRHIHandle() : RHI::NullResource;
                info.LeafThicknessTextureID =
                    layer.LeafThicknessTexture ? layer.LeafThicknessTexture->GetRHIHandle() : RHI::NullResource;
                info.LeafRoughness = layer.LeafRoughness;
                info.LeafNormalStrength = layer.LeafNormalStrength;
                info.LeafThickness = layer.LeafThickness;
                info.LeafTransmissionStrength = layer.LeafTransmissionStrength;
                info.LeafTransmissionColor = layer.LeafTransmissionColor;
                info.LeafTransmissionDistortion = layer.LeafTransmissionDistortion;
                info.LeafTransmissionPower = layer.LeafTransmissionPower;
                info.LeafTransmissionWrap = layer.LeafTransmissionWrap;
                info.LeafTransmissionAmbient = layer.LeafTransmissionAmbient;

                // Octahedral impostor (issue #433) — only when the atlas baked OK,
                // and only for the CARD draw: the impostor IS the far-field card,
                // so routing the near mesh through it would replace the geometry
                // this task exists to draw.
                if (!draw.IsAuthoredMesh && layer.UseImpostor && layer.Impostor.IsValid())
                {
                    info.UseImpostor = true;
                    info.ImpostorAlbedoAtlasID = layer.Impostor.Albedo->GetRHIHandle();
                    info.ImpostorNormalDepthAtlasID = layer.Impostor.NormalDepth->GetRHIHandle();
                    info.ImpostorFramesPerAxis = layer.Impostor.FramesPerAxis;
                    info.ImpostorHemi = layer.Impostor.Hemi;
                    info.ImpostorStartDistance = layer.ImpostorStartDistance;
                    info.ImpostorTransitionBand = layer.ImpostorTransitionBand;
                    info.ImpostorRadius = layer.Impostor.Radius;
                }

                result.push_back(info);
            }
        }

        return result;
    }
} // namespace OloEngine

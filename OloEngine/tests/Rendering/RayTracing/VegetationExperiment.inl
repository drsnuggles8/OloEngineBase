// Scratch experiment: real AS costs and alpha-masked ray silhouettes; no production caller.
TEST_F(RayTracingDevice, VegetationDetailedVersusCardExperiment)
{
    ScopedVulkanRenderCommandSelection selection;
    auto model = Ref<Model>::Create("SandboxProject/Assets/Models/Vegetation/pine.obj");
    auto source = model->CreateCombinedMeshSource();
    ASSERT_TRUE(source);
    std::vector<Vertex> detailed(source->GetVertices().GetData(), source->GetVertices().GetData() + source->GetVertices().Num());
    std::vector<u32> detailedIndices;
    for (const auto& sub : source->GetSubmeshes())
        for (u32 i = 0; i < sub.m_IndexCount; ++i)
            detailedIndices.push_back(source->GetIndices()[static_cast<i32>(sub.m_BaseIndex + i)] + sub.m_BaseVertex);
    ASSERT_FALSE(detailedIndices.empty());
    auto lod = MeshOptimization::GenerateLODMeshWithAttributes(*source, 0.5f, 0.01f);
    ASSERT_TRUE(lod);
    std::vector<Vertex> coarse;
    std::vector<u32> coarseIndices;
    std::vector<u32> remap(static_cast<sizet>(lod->GetVertices().Num()), ~0u);
    for (const u32 index : lod->GetIndices())
    {
        ASSERT_LT(index, remap.size());
        if (remap[index] == ~0u)
        {
            remap[index] = static_cast<u32>(coarse.size());
            coarse.push_back(lod->GetVertices()[static_cast<i32>(index)]);
        }
        coarseIndices.push_back(remap[index]);
    }
    const std::vector<Vertex> card = {
        Vertex({ -0.5f, 0, 0 }, { 0, 0, 1 }, { 0, 0 }), Vertex({ 0.5f, 0, 0 }, { 0, 0, 1 }, { 1, 0 }),
        Vertex({ 0.5f, 1, 0 }, { 0, 0, 1 }, { 1, 1 }), Vertex({ -0.5f, 1, 0 }, { 0, 0, 1 }, { 0, 1 })
    };
    auto deform = ComputeShader::Create("assets/shaders/tests/VegetationExperimentDeform.comp");
    auto productionDeform = ComputeShader::Create("assets/shaders/VegetationDeformToBuffer.comp");
    ASSERT_TRUE(productionDeform && productionDeform->IsValid());
    auto probe = ComputeShader::Create("assets/shaders/tests/VegetationExperimentProbe.comp");
    auto alpha = Texture2D::Create("assets/textures/grass.png");
    ASSERT_TRUE(deform && deform->IsValid());
    ASSERT_TRUE(probe && probe->IsValid());
    ASSERT_TRUE(alpha && alpha->IsLoaded());
    VkPhysicalDeviceProperties deviceProperties{};
    vkGetPhysicalDeviceProperties(m_Device->GetPhysicalDevice(), &deviceProperties);
    std::filesystem::create_directories("assets/tests/visual/vegetation-experiment");
    std::ofstream report("assets/tests/visual/vegetation-experiment/measurements.csv");
    report << "# device=" << deviceProperties.deviceName << " driver=" << deviceProperties.driverVersion << "\n";
    report << "plants,representation,blas_count,vertices,triangles,geometry_bytes,as_bytes,scratch_bytes,gpu_update_p50_ms,gpu_update_p95_ms,gpu_update_p99_ms,submit_fence_p50_ms,submit_fence_p95_ms,submit_fence_p99_ms\n";
    VkQueryPool queryPool = VK_NULL_HANDLE;
    VkQueryPoolCreateInfo queryInfo{};
    queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queryInfo.queryCount = 2;
    ASSERT_EQ(vkCreateQueryPool(m_Device->GetDevice(), &queryInfo, nullptr, &queryPool), VK_SUCCESS);
    struct ReleaseQueries
    {
        VkDevice Device;
        VkQueryPool Pool;
        ~ReleaseQueries()
        {
            vkDestroyQueryPool(Device, Pool, nullptr);
        }
    } releaseQueries{ m_Device->GetDevice(), queryPool };
    auto foliageUbo = UniformBuffer::Create(sizeof(ShaderBindingLayout::FoliageUBO), ShaderBindingLayout::UBO_FOLIAGE);
    struct DeformParams
    {
        glm::uvec2 Rest, Rows, Output;
        u32 VertexCount, PlantCount;
    };
    auto paramsUbo = UniformBuffer::Create(96u, ShaderBindingLayout::UBO_RAY_TRACING);
    constexpr u32 width = 384, height = 256;
    std::array<std::vector<Vertex>, 3> detailedSnapshots;
    for (const u32 plants : { 64u, 1024u })
    {
        std::vector<FoliageInstanceData> rows(plants);
        for (u32 i = 0; i < plants; ++i)
        {
            const u32 side = plants == 64 ? 8 : 32;
            rows[i].PositionScale = glm::vec4(static_cast<f32>(i % side) * 5, 0, static_cast<f32>(i / side) * 5, 1);
            rows[i].RotationHeight = glm::vec4(static_cast<f32>(i % 7) * 0.31f, 10, 1, FoliageWindPhase(i + 1));
            rows[i].ColorAlpha = glm::vec4(1, 1, 1, 0.25f);
        }
        auto rowBuffer = VertexBuffer::Create(rows.data(), static_cast<u32>(rows.size() * sizeof(rows[0])));
        for (const u32 representation : { 0u, 1u, 2u, 3u, 4u })
        {
            const bool proxy = representation == 1u;
            const char* name = representation == 0u ? "detailed" : (proxy ? "card" : (representation == 2u ? "coarse" : (representation == 3u ? "affine" : "temporal")));
            const auto& rest = proxy ? card : (representation == 2u ? coarse : detailed);
            const std::vector<u32> cardIndices{ 0, 1, 2, 2, 3, 0 };
            const auto& restIndices = proxy ? cardIndices : (representation == 2u ? coarseIndices : detailedIndices);
            auto restBuffer = VertexBuffer::Create(rest.data(), static_cast<u32>(rest.size() * sizeof(Vertex)));
            auto output = VertexBuffer::Create(static_cast<u32>(rest.size() * plants * sizeof(Vertex)));
            std::vector<u32> indices;
            for (u32 i = 0; i < plants; ++i)
                for (u32 index : restIndices)
                    indices.push_back(index + i * static_cast<u32>(rest.size()));
            auto ibo = IndexBuffer::Create(indices.data(), static_cast<u32>(indices.size()));
            m_Backend = RT::CreateVulkanRayTracingBackend();
            RT::BlasBuildRequest build{};
            build.Key = { 0, 1 };
            build.Class = RT::GeometryClass::Deformed;
            build.VertexAddress = output->GetDeviceAddress();
            build.IndexAddress = ibo->GetDeviceAddress();
            build.VertexStride = sizeof(Vertex);
            build.VertexCount = static_cast<u32>(rest.size() * plants);
            build.IndexCount = static_cast<u32>(indices.size());
            RT::InstanceRecord instance{};
            instance.Geometry = build.Key;
            instance.ForceOpaque = false;
            constexpr u32 plantsPerBlas = 32;
            std::vector<RT::BlasBuildRequest> builds;
            std::vector<RT::InstanceRecord> instances;
            for (u32 group = 0; group < plants / plantsPerBlas; ++group)
            {
                auto groupBuild = build;
                groupBuild.Key = { group, 1 };
                groupBuild.FirstIndex = group * plantsPerBlas * static_cast<u32>(restIndices.size());
                groupBuild.IndexCount = plantsPerBlas * static_cast<u32>(restIndices.size());
                builds.push_back(groupBuild);
                auto groupInstance = instance;
                groupInstance.Geometry = groupBuild.Key;
                groupInstance.CustomIndex = group;
                instances.push_back(groupInstance);
            }
            const DeformParams deformParams{ SplitAddress(restBuffer->GetDeviceAddress()), SplitAddress(rowBuffer->GetDeviceAddress()),
                                             SplitAddress(output->GetDeviceAddress()), static_cast<u32>(rest.size()), plants };
            ShaderBindingLayout::FoliageUBO wind{};
            wind.WindStrength = 2;
            wind.WindSpeed = 1.1f;
            wind.WindWeights = { 0.4f, 0.7f, 0.8f, 0 };
            wind.WindFlags.w = 1;
            wind.WindDirection = glm::vec4(glm::normalize(glm::vec3(1, 0, 0.3f)), 8);
            wind.WindGust = { 0.6f, 0.4f, 0, 0 };
            wind.MeshParams.x = proxy ? 0.0f : 1.0f;
            wind.MeshParams.y = representation == 3u ? 1.0f : (representation == 4u ? 2.0f : 0.0f);
            struct alignas(8) DispatchGroup
            {
                glm::uvec2 Rows, Output;
                u32 VertexCount, PlantCount;
            };
            struct alignas(16) BatchParams
            {
                glm::mat4 Model{ 1 };
                glm::uvec2 Rest, Jobs, Tasks;
                u32 TaskCount, Padding;
            };
            static_assert(sizeof(DispatchGroup) == 24 && sizeof(BatchParams) == 96);
            std::vector<DispatchGroup> jobs;
            std::vector<glm::uvec2> tasks;
            for (u32 group = 0; group < plants / plantsPerBlas; ++group)
            {
                jobs.push_back({ SplitAddress(rowBuffer->GetDeviceAddress() + group * plantsPerBlas * sizeof(FoliageInstanceData)),
                                 SplitAddress(output->GetDeviceAddress() + group * plantsPerBlas * rest.size() * sizeof(Vertex)),
                                 static_cast<u32>(rest.size()), plantsPerBlas });
                for (u32 vertex = 0; vertex < plantsPerBlas * rest.size(); vertex += 64u)
                    tasks.emplace_back(group, vertex);
            }
            auto jobBuffer = StorageBuffer::Create(static_cast<u32>(jobs.size() * sizeof(DispatchGroup)), StorageBuffer::kNoBinding);
            auto taskBuffer = StorageBuffer::Create(static_cast<u32>(tasks.size() * sizeof(glm::uvec2)), StorageBuffer::kNoBinding);
            jobBuffer->SetData(jobs.data(), static_cast<u32>(jobs.size() * sizeof(DispatchGroup)));
            taskBuffer->SetData(tasks.data(), static_cast<u32>(tasks.size() * sizeof(glm::uvec2)));
            const BatchParams batchParams{ glm::mat4(1), SplitAddress(restBuffer->GetDeviceAddress()),
                                           SplitAddress(jobBuffer->GetDeviceAddress()), SplitAddress(taskBuffer->GetDeviceAddress()), static_cast<u32>(tasks.size()), 0 };
            const auto dispatchDeform = [&]
            {
                auto snapshotWind = wind;
                if (representation == 4u)
                    snapshotWind.Time = std::floor(wind.Time * 20.0f) / 20.0f;
                foliageUbo->SetData(&snapshotWind, sizeof(snapshotWind));
                if (representation == 3u)
                {
                    paramsUbo->SetData(&deformParams, sizeof(deformParams));
                    deform->Bind();
                    RenderCommand::DispatchCompute((build.VertexCount + 63) / 64, 1, 1);
                }
                else
                {
                    paramsUbo->SetData(&batchParams, sizeof(batchParams));
                    productionDeform->Bind();
                    RenderCommand::DispatchCompute(batchParams.TaskCount, 1, 1);
                }
            };
            std::vector<double> timings, gpuTimings;
            for (u32 frame = 0; frame < 80; ++frame)
            {
                wind.Time = static_cast<f32>(frame) * 0.025f;
                const bool update = representation != 4u || frame % 2u == 0u;
                const u32 updateIndex = representation == 4u ? frame / 2u : frame;
                build.Reason = frame == 0 ? RT::BuildReason::FirstBuild : (updateIndex % 9 == 0 ? RT::BuildReason::DeformedRefitBudget : RT::BuildReason::DeformedRefit);
                for (auto& request : builds)
                    request.Reason = build.Reason;
                const auto start = std::chrono::steady_clock::now();
                RecordAndSubmit([&]
                                {
                    vkCmdResetQueryPool(m_Cmd, queryPool, 0, 2);
                    vkCmdWriteTimestamp2(m_Cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queryPool, 0);
                    if (update)
                    {
                        dispatchDeform();
                        m_Backend->RecordDeformToBuildBarrier();
                        EXPECT_EQ(m_Backend->RecordBlasBuilds(builds),builds.size());
                    }
                    static_cast<void>(m_Backend->RecordTlasBuild(instances, frame==0?RT::TlasBuildReason::FirstBuild:RT::TlasBuildReason::Update));
                    m_Backend->RecordBuildToReadBarrier();
                    vkCmdWriteTimestamp2(m_Cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queryPool, 1); });
                const double cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                std::array<u64, 2> timestamps{};
                ASSERT_EQ(vkGetQueryPoolResults(m_Device->GetDevice(), queryPool, 0, 2, sizeof(timestamps), timestamps.data(), sizeof(u64), VK_QUERY_RESULT_64_BIT), VK_SUCCESS);
                // This harness fences each submission; release retired rebuilds
                // only after that fence, so 80 simulated frames do not accumulate
                // an artificial unbounded retirement queue.
                VulkanDeferredReclaim::Get().FlushAll();
                if (frame >= 16)
                {
                    timings.push_back(cpuMs);
                    gpuTimings.push_back(static_cast<double>(timestamps[1] - timestamps[0]) * deviceProperties.limits.timestampPeriod / 1e6);
                }
            }
            std::sort(timings.begin(), timings.end());
            std::sort(gpuTimings.begin(), gpuTimings.end());
            RT::SceneStats stats{};
            m_Backend->PublishStats(stats);
            report << plants << ',' << name << ',' << builds.size() << ',' << build.VertexCount << ',' << build.TriangleCount() << ','
                   << (rest.size() * sizeof(Vertex) * (plants + 1u) + indices.size() * sizeof(u32) + rows.size() * sizeof(FoliageInstanceData)) << ','
                   << stats.Resident.AccelerationStructureBytes << ',' << stats.Resident.ScratchBytes << ','
                   << gpuTimings[32] << ',' << gpuTimings[60] << ',' << gpuTimings[63] << ','
                   << timings[32] << ',' << timings[60] << ',' << timings[63] << '\n';
            report.flush();
            if (plants == 1024)
            {
                m_Backend.reset();
                continue;
            }
            GPUSceneGeometry geo{};
            geo.VertexAddress = output->GetDeviceAddress();
            geo.IndexAddress = ibo->GetDeviceAddress();
            geo.VertexFormat = static_cast<u32>(GPUSceneVertexFormat::OloVertex);
            geo.IndexFormat = static_cast<u32>(GPUSceneIndexFormat::UInt32);
            geo.IndexCount = build.IndexCount;
            geo.VertexCount = build.VertexCount;
            geo.Flags = GPUSceneGeometryFlagActive;
            std::vector<GPUSceneGeometry> geos(builds.size(), geo);
            std::vector<GPUSceneInstance> insts(builds.size());
            for (u32 group = 0; group < builds.size(); ++group)
            {
                geos[group].FirstIndex = builds[group].FirstIndex;
                geos[group].IndexCount = builds[group].IndexCount;
                insts[group].Flags = GPUSceneInstanceFlagActive;
                insts[group].GeometryIndex = group;
                insts[group].MaterialIndex = 0;
            }
            GPUSceneMaterial mat{};
            mat.Flags = GPUSceneMaterialFlagActive;
            mat.AlphaMode = static_cast<u32>(AlphaMode::Mask);
            mat.AlphaCutoff = 0.25f;
            auto geoBuffer = StorageBuffer::Create(static_cast<u32>(geos.size() * sizeof(geo)), 42);
            geoBuffer->SetData(geos.data(), static_cast<u32>(geos.size() * sizeof(geo)));
            auto instBuffer = StorageBuffer::Create(static_cast<u32>(insts.size() * sizeof(GPUSceneInstance)), 43);
            instBuffer->SetData(insts.data(), static_cast<u32>(insts.size() * sizeof(GPUSceneInstance)));
            auto matBuffer = StorageBuffer::Create(sizeof(mat), 44);
            matBuffer->SetData(&mat, sizeof(mat));
            for (u32 angle = 0; angle < 3; ++angle)
                for (u32 time = 0; time < 3; ++time)
                {
                    const glm::vec3 target(17.5f, 5, 17.5f);
                    const glm::vec3 eyes[]{ { 17.5f, 12, 70 }, { 65, 12, 40 }, { 17.5f, 65, 50 } };
                    const glm::vec3 eye = eyes[angle], forward = glm::normalize(target - eye);
                    const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0))), up = glm::cross(right, forward);
                    std::vector<ProbeRay> rays;
                    for (u32 y = 0; y < height; ++y)
                        for (u32 x = 0; x < width; ++x)
                        {
                            const glm::vec3 direction = glm::normalize(forward + right * ((static_cast<f32>(x) + 0.5f) / width - 0.5f) * 1.5f + up * (0.5f - (static_cast<f32>(y) + 0.5f) / height));
                            rays.push_back({ glm::vec4(eye, 0.01f), glm::vec4(direction, 200) });
                        }
                    auto rayBuffer = StorageBuffer::Create(static_cast<u32>(rays.size() * sizeof(ProbeRay)), 45);
                    rayBuffer->SetData(rays.data(), static_cast<u32>(rays.size() * sizeof(ProbeRay)));
                    auto hitBuffer = StorageBuffer::Create(width * height * sizeof(ProbeHit), 46);
                    ProbeParams pp{};
                    pp.TlasAddress = SplitAddress(m_Backend->GetTlasDeviceAddress());
                    pp.RayAddress = SplitAddress(StorageDeviceAddress(rayBuffer));
                    pp.HitAddress = SplitAddress(StorageDeviceAddress(hitBuffer));
                    pp.InstanceTableAddress = SplitAddress(StorageDeviceAddress(instBuffer));
                    pp.GeometryTableAddress = SplitAddress(StorageDeviceAddress(geoBuffer));
                    pp.MaterialTableAddress = SplitAddress(StorageDeviceAddress(matBuffer));
                    pp.RayCount = width * height;
                    pp.InstanceSlotCount = pp.GeometrySlotCount = static_cast<u32>(builds.size());
                    pp.MaterialSlotCount = 1;
                    wind.Time = static_cast<f32>(time) + 0.035f;
                    build.Reason = RT::BuildReason::DeformedRefit;
                    for (auto& request : builds)
                        request.Reason = build.Reason;
                    RecordAndSubmit([&]
                                    {
                        dispatchDeform();
                        m_Backend->RecordDeformToBuildBarrier(); EXPECT_EQ(m_Backend->RecordBlasBuilds(builds),builds.size());
                        static_cast<void>(m_Backend->RecordTlasBuild(instances,RT::TlasBuildReason::Update));
                        m_Backend->RecordBuildToReadBarrier(); paramsUbo->SetData(&pp,sizeof(pp)); alpha->Bind(0); probe->Bind();
                        RenderCommand::DispatchCompute((width*height+63)/64,1,1); });
                    if (angle == 0u && (representation == 0u || representation == 4u))
                    {
                        const u32 bytes = build.VertexCount * sizeof(Vertex);
                        auto readback = StorageBuffer::Create(bytes, StorageBuffer::kNoBinding, StorageBufferUsage::DynamicCopy);
                        RecordAndSubmit([&]
                                        {
                            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);
                            VkBufferCopy copy{}; copy.size=bytes;
                            vkCmdCopyBuffer(m_Cmd,static_cast<VulkanVertexBuffer&>(*output).GetVkBuffer(),
                                static_cast<VulkanStorageBuffer&>(*readback).GetVkBuffer(),1,&copy); });
                        std::vector<Vertex> vertices(build.VertexCount);
                        readback->GetData(vertices.data(), bytes);
                        if (representation == 0u)
                            detailedSnapshots[time] = std::move(vertices);
                        else
                        {
                            ASSERT_EQ(vertices.size(), detailedSnapshots[time].size());
                            f32 maximumError = 0;
                            for (sizet vertex = 0; vertex < vertices.size(); ++vertex)
                                maximumError = std::max(maximumError, glm::length(vertices[vertex].Position - detailedSnapshots[time][vertex].Position));
                            EXPECT_GT(maximumError, 0.001f);
                            EXPECT_LE(maximumError, 0.25f);
                            std::cout << "Temporal snapshot time " << wind.Time << " maximum vertex error " << maximumError << " m\n";
                        }
                    }
                    std::vector<ProbeHit> hits(width * height);
                    hitBuffer->GetData(hits.data(), static_cast<u32>(hits.size() * sizeof(ProbeHit)));
                    std::vector<u8> pixels(width * height * 3, 0);
                    u32 coverage = 0;
                    for (sizet i = 0; i < hits.size(); ++i)
                        if (hits[i].DistanceAndBarycentrics.w > 0.5f)
                        {
                            ++coverage;
                            pixels[i * 3] = 80;
                            pixels[i * 3 + 1] = 220;
                            pixels[i * 3 + 2] = 80;
                        }
                    EXPECT_GT(coverage, 100u);
                    const std::string path = "assets/tests/visual/vegetation-experiment/" + std::string(name) + "_angle" + std::to_string(angle) + "_time" + std::to_string(time) + ".png";
                    EXPECT_NE(stbi_write_png(path.c_str(), width, height, 3, pixels.data(), width * 3), 0);
                }
            m_Backend.reset();
        }
    }
}

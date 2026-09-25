// OLO_TEST_LAYER: plumbing
TEST_F(RayTracingDevice, VegetationSharesPartStreamsAndRecoversAfterUnsubmittedOrRefusedWork)
{
    ScopedVulkanRenderCommandSelection selection;
    const std::array<Vertex, 4> vertices{
        Vertex({ -0.5f, 0, 0 }, { 0, 1, 0 }, { 0, 0 }), Vertex({ 0.5f, 0, 0 }, { 0, 1, 0 }, { 1, 0 }),
        Vertex({ 0.5f, 1, 0 }, { 0, 1, 0 }, { 1, 1 }), Vertex({ -0.5f, 1, 0 }, { 0, 1, 0 }, { 0, 1 })
    };
    RT::VegetationSurfaceInput input;
    input.Owner = 17u;
    input.FirstPlantId = 91u;
    input.Rest = VertexBuffer::Create(vertices.data(), sizeof(vertices));
    input.VertexCount = 4u;
    input.Rows.Add({ glm::vec4(10, 0, 5, 1), glm::vec4(0, 2, 1, FoliageWindPhase(91u)), glm::vec4(1) });
    input.Parts.Add({ 0u, { 0u, 1u, 2u }, {} });
    input.Parts.Add({ 1u, { 2u, 3u, 0u }, {} });
    input.DistanceToView = 0.0f;
    input.DetailedDistance = 12.0f;
    input.HistoryContinuous = true;
    input.Wind.WindStrength = 1.0f;
    input.Wind.WindSpeed = 1.0f;
    input.VelocityBound = 3.0f;
    GPUScene scene;
    RT::VegetationSurfaceCache cache;
    cache.SetEnabled(true);
    const auto extract = [&](const RT::VegetationSurfaceInput& data)
    {
        cache.BeginFrame();
        scene.BeginExtraction(17u, glm::vec3(0));
        cache.Queue(data);
        cache.FinishExtraction(scene);
        static_cast<void>(scene.EndExtraction());
    };
    extract(input);
    ASSERT_TRUE(cache.GetStats().Complete);
    // No recording bracket: Vulkan refuses the command. A compiled module
    // alone must never authorize an AS build from unwritten output.
    EXPECT_EQ(cache.Dispatch(), 0u);
    EXPECT_TRUE(cache.GetStats().ProducerFailed);
    EXPECT_FALSE(cache.GetStats().Complete);
    // Abandoned recording: the next extraction must schedule the write again.
    extract(input);
    EXPECT_EQ(cache.GetStats().SnapshotsReused, 0u);
    RecordAndSubmit([&]
                    { EXPECT_EQ(cache.Dispatch(), 1u); });
    EXPECT_EQ(cache.GetStats().DispatchBatches, 1u);
    EXPECT_EQ(cache.GetStats().PlantsRepresented, 1u);
    EXPECT_EQ(scene.GetGeometrySlotCount(), 2u);
    std::vector<const GPUSceneGeometry*> geometry;
    for (u32 slot = 0u; slot < scene.GetInstanceSlotCount(); ++slot)
        if (const auto* instance = scene.GetLiveInstanceRecordBySlot(slot))
            geometry.push_back(scene.GetLiveGeometryRecordBySlot(instance->GeometryIndex, instance->GeometryGeneration));
    ASSERT_EQ(geometry.size(), 2u);
    ASSERT_NE(geometry[0], nullptr);
    ASSERT_NE(geometry[1], nullptr);
    EXPECT_EQ(geometry[0]->VertexAddress, geometry[1]->VertexAddress);
    EXPECT_EQ(geometry[0]->IndexAddress, geometry[1]->IndexAddress);
    EXPECT_NE(geometry[0]->FirstIndex, geometry[1]->FirstIndex);
    extract(input);
    EXPECT_FALSE(cache.HasWork());
    EXPECT_EQ(cache.GetStats().SnapshotsReused, 1u);
    EXPECT_FALSE(cache.GetStats().HistoryReset);
    struct RestoreDiagnostic
    {
        bool Previous = RT::VegetationDiagnostics::GetForceDetailed();
        ~RestoreDiagnostic()
        {
            RT::VegetationDiagnostics::SetForceDetailed(Previous);
        }
    } restoreDiagnostic;
    RT::VegetationDiagnostics::SetForceDetailed(false);
    input.DistanceToView = 100.0f;
    const f32 phase = FoliageWindPhase(input.FirstPlantId) / 6.2831853f;
    input.Wind.Time = 0.05f * (1.25f - phase); // safely inside the staggered bucket
    extract(input);
    EXPECT_EQ(cache.GetStats().ProxyGroups, 1u);
    EXPECT_TRUE(cache.GetStats().HistoryReset);
    RecordAndSubmit([&]
                    { EXPECT_EQ(cache.Dispatch(), 1u); });
    input.Wind.Time += 0.001f;
    extract(input);
    EXPECT_FALSE(cache.HasWork());
    EXPECT_EQ(cache.GetStats().SnapshotsReused, 1u);
    input.Wind.Time += 0.06f;
    extract(input);
    EXPECT_TRUE(cache.HasWork());
    RecordAndSubmit([&]
                    { EXPECT_EQ(cache.Dispatch(), 1u); });
    RT::VegetationDiagnostics::SetForceDetailed(true);
    extract(input);
    EXPECT_EQ(cache.GetStats().DetailedGroups, 1u);
    EXPECT_EQ(cache.GetStats().ProxyGroups, 0u);
    EXPECT_TRUE(cache.GetStats().HistoryReset);
    RecordAndSubmit([&]
                    { EXPECT_EQ(cache.Dispatch(), 1u); });
    RT::VegetationDiagnostics::SetForceDetailed(false);
    // Invalid source indices never reach an AS producer. Removing the bad
    // request restores readiness and invalidates the fallback history.
    auto invalid = input;
    invalid.Parts[0].Indices[0] = 4u;
    extract(invalid);
    EXPECT_FALSE(cache.GetStats().Complete);
    EXPECT_EQ(cache.GetStats().Refused, 1u);
    EXPECT_FALSE(cache.HasWork());
    extract(input);
    EXPECT_TRUE(cache.GetStats().Complete);
    EXPECT_TRUE(cache.GetStats().HistoryReset);
    RecordAndSubmit([&]
                    { EXPECT_EQ(cache.Dispatch(), 1u); });
    cache.BeginFrame();
    scene.BeginExtraction(17u, glm::vec3(0));
    cache.FinishExtraction(scene);
    static_cast<void>(scene.EndExtraction());
    EXPECT_EQ(cache.GetStats().ResidentBytes, 0u);
    EXPECT_TRUE(cache.GetStats().HistoryReset);
    cache.Shutdown();
}

TEST_F(RayTracingDevice, HybridConsumersUseTextureAlphaFactorCutoffAndHeapGeneration)
{
    ScopedVulkanRenderCommandSelection selection;
    // The headless fixture brings up a device but not the engine's descriptor
    // heap facade, so arm it before deciding this device cannot reach the
    // shader-visible heap at all. Installing it is PROCESS-WIDE state, so it
    // is restored on the way out: leaving a Vulkan backend on the engine heap
    // makes a later OpenGL test in the same binary fail, which is how this
    // was found (FoliageWindEvidenceTest passed alone and failed after it).
    auto& engineHeap = RHI::DescriptorHeap::Get();
    struct RestoreHeap
    {
        RHI::IDescriptorHeapBackend* Backend;
        RHI::HeapDesc Desc;
        bool Enabled;
        bool Restore;
        ~RestoreHeap()
        {
            if (!Restore)
                return;
            auto& heap = RHI::DescriptorHeap::Get();
            if (Backend)
            {
                heap.Initialize(Desc, Backend);
                heap.SetEnabled(Enabled);
            }
            else
                heap.Shutdown();
        }
    } restoreHeap{ engineHeap.GetBackend(), engineHeap.GetDesc(), engineHeap.IsEnabled(), false };
    if (!HeapBinding::ShaderHeapIndexingSupported())
    {
        restoreHeap.Restore = true;
        static_cast<void>(VulkanDescriptorHeapBackend::InstallOntoEngineHeap());
    }
    if (!HeapBinding::ShaderHeapIndexingSupported())
        GTEST_SKIP() << "Hybrid material alpha requires the Vulkan descriptor heap";
    const std::array<Vertex, 3> vertices{
        Vertex({ 0, 0, 5 }, { 0, 0, -1 }, { 0, 0 }), Vertex({ 1, 0, 5 }, { 0, 0, -1 }, { 1, 0 }),
        Vertex({ 0, 1, 5 }, { 0, 0, -1 }, { 0, 1 })
    };
    std::array<u32, 3> indices{ 0u, 1u, 2u };
    auto vertexBuffer = VertexBuffer::Create(vertices.data(), sizeof(vertices));
    auto indexBuffer = IndexBuffer::Create(indices.data(), 3u);
    auto alpha = Texture2D::Create({ .Width = 2u, .Height = 1u, .GenerateMips = false });
    std::array<u8, 8> texels{ 255, 255, 255, 0, 255, 255, 255, 255 };
    alpha->SetData(texels.data(), sizeof(texels));
    const auto texture = HeapBinding::ResolveShaderHeapTexture(alpha->GetRHIHandle());
    const auto sampler = HeapBinding::ResolveShaderHeapSampler(HeapBinding::MaterialTexture2DSampler());
    ASSERT_TRUE(texture.IsValid() && sampler.IsValid());
    m_Backend = RT::CreateVulkanRayTracingBackend();
    RT::BlasBuildRequest build;
    build.Key = { 0u, 1u };
    build.Class = RT::GeometryClass::Masked;
    build.VertexAddress = vertexBuffer->GetDeviceAddress();
    build.IndexAddress = indexBuffer->GetDeviceAddress();
    build.VertexStride = sizeof(Vertex);
    build.VertexCount = 3u;
    build.IndexCount = 3u;
    RT::InstanceRecord rtInstance;
    rtInstance.Geometry = build.Key;
    rtInstance.ForceOpaque = false;
    RecordAndSubmit([&]
                    {
        ASSERT_EQ(m_Backend->RecordBlasBuilds(std::span(&build,1)), 1u);
        static_cast<void>(m_Backend->RecordTlasBuild(std::span(&rtInstance,1),RT::TlasBuildReason::FirstBuild));
        m_Backend->RecordBuildToReadBarrier(); });
    GPUSceneGeometry geometry;
    geometry.Generation = 1u;
    geometry.Flags = GPUSceneGeometryFlagActive;
    geometry.VertexAddress = build.VertexAddress;
    geometry.IndexAddress = build.IndexAddress;
    geometry.VertexFormat = std::to_underlying(GPUSceneVertexFormat::OloVertex);
    geometry.IndexFormat = std::to_underlying(GPUSceneIndexFormat::UInt32);
    geometry.VertexCount = 3u;
    geometry.IndexCount = 3u;
    GPUSceneInstance instance;
    instance.Generation = 1u;
    instance.Flags = GPUSceneInstanceFlagActive;
    instance.GeometryIndex = 0u;
    instance.GeometryGeneration = 1u;
    instance.MaterialIndex = 0u;
    instance.MaterialGeneration = 1u;
    GPUSceneMaterial material;
    material.Generation = 1u;
    material.Flags = GPUSceneMaterialFlagActive | GPUSceneMaterialFlagAlbedoMap;
    material.AlphaMode = std::to_underlying(AlphaMode::Mask);
    material.AlphaCutoff = 0.25f;
    material.BaseColorFactor = glm::vec4(1.0f);
    MaterialShaderHeapRecord heap;
    heap.Generation = 1u;
    heap.Flags = material.Flags;
    heap.Textures.x = texture.Value;
    const auto upload = [](const auto& data)
    {
        auto buffer = StorageBuffer::Create(sizeof(data), StorageBuffer::kNoBinding);
        buffer->SetData(&data, sizeof(data));
        return buffer;
    };
    auto geometries = upload(geometry), instances = upload(instance), materials = upload(material), heaps = upload(heap);
    const std::array<ProbeRay, 2> rays{
        ProbeRay{ glm::vec4(0.25f, 0.25f, 0, 0.001f), glm::vec4(0, 0, 1, 10) },
        ProbeRay{ glm::vec4(0.75f, 0.125f, 0, 0.001f), glm::vec4(0, 0, 1, 10) }
    };
    auto rayBuffer = upload(rays);
    auto hitBuffer = StorageBuffer::Create(2u * sizeof(glm::vec4), StorageBuffer::kNoBinding, StorageBufferUsage::DynamicCopy);
    struct Params
    {
        glm::uvec4 TlasAndRays, HitsAndCount, InstancesAndGeometry, MaterialsAndHeap, CountsAndSampler, HeapCount;
    };
    static_assert(sizeof(Params) == 96u);
    const Params params{
        glm::uvec4(SplitAddress(m_Backend->GetTlasDeviceAddress()), SplitAddress(rayBuffer->GetDeviceAddress())),
        glm::uvec4(SplitAddress(hitBuffer->GetDeviceAddress()), 2u, 0u),
        glm::uvec4(SplitAddress(instances->GetDeviceAddress()), SplitAddress(geometries->GetDeviceAddress())),
        glm::uvec4(SplitAddress(materials->GetDeviceAddress()), SplitAddress(heaps->GetDeviceAddress())),
        glm::uvec4(1u, 1u, 1u, sampler.Value), glm::uvec4(1u, 0u, 0u, 0u)
    };
    auto uniform = UniformBuffer::Create(sizeof(params), ShaderBindingLayout::UBO_RAY_TRACING);
    auto probe = ComputeShader::Create("assets/shaders/tests/HybridRayTracingAlphaProbe.comp");
    ASSERT_TRUE(probe && probe->IsValid());
    const auto trace = [&]
    {
        RecordAndSubmit([&]
                        { uniform->SetData(&params,sizeof(params)); probe->Bind(); RenderCommand::DispatchCompute(1u,1u,1u); });
        std::array<glm::vec4, 2> hits;
        hitBuffer->GetData(hits.data(), sizeof(hits));
        return hits;
    };
    auto hits = trace();
    EXPECT_NEAR(hits[0].y, 0.0f, 1e-6f);
    EXPECT_NEAR(hits[1].y, 1.0f, 1e-6f);
    EXPECT_NEAR(hits[1].x, 5.0f, 1e-5f);
    material.BaseColorFactor.a = 0.1f;
    materials->SetData(&material, sizeof(material));
    hits = trace();
    EXPECT_NEAR(hits[1].y, 0.0f, 1e-6f);
    material.BaseColorFactor.a = 1.0f;
    material.AlphaCutoff = 0.75f;
    materials->SetData(&material, sizeof(material));
    heap.Generation = 2u;
    heaps->SetData(&heap, sizeof(heap));
    hits = trace();
    EXPECT_NEAR(hits[1].y, 0.0f, 1e-6f);
    heap.Generation = 1u;
    heaps->SetData(&heap, sizeof(heap));
    hits = trace();
    EXPECT_NEAR(hits[1].y, 1.0f, 1e-6f);
}

// #1437: the vegetation deformer shares UBO_RAY_TRACING with six other
// producers, and on Vulkan a uniform buffer reaches a dispatch only as its
// binding point's current OCCUPANT; SetData alone publishes nothing. When an
// animated surface deformed first, DeformedSurfaceCache's 48-byte block sat at
// the slot, the vegetation shader read its 96-byte block from it, and the
// device faulted in RayTracingScenePass on the Deferred integrated benchmark.
//
// The stand-in tenants here are ZERO-filled on purpose. A shader reading them
// sees TaskCount == 0 and returns, so a regression writes nothing and fails
// the comparison below instead of faulting the device the way the real one did.
TEST_F(RayTracingDevice, VegetationDispatchReadsItsOwnParamsAfterAnotherProducerTakesTheSharedBinding)
{
    ScopedVulkanRenderCommandSelection selection;
    const std::array<Vertex, 4> vertices{
        Vertex({ -0.5f, 0, 0 }, { 0, 1, 0 }, { 0, 0 }), Vertex({ 0.5f, 0, 0 }, { 0, 1, 0 }, { 1, 0 }),
        Vertex({ 0.5f, 1, 0 }, { 0, 1, 0 }, { 1, 1 }), Vertex({ -0.5f, 1, 0 }, { 0, 1, 0 }, { 0, 1 })
    };
    RT::VegetationSurfaceInput input;
    input.Owner = 17u;
    input.FirstPlantId = 91u;
    input.Rest = VertexBuffer::Create(vertices.data(), sizeof(vertices));
    input.VertexCount = 4u;
    input.Rows.Add({ glm::vec4(10, 0, 5, 1), glm::vec4(0, 2, 1, FoliageWindPhase(91u)), glm::vec4(1) });
    input.Parts.Add({ 0u, { 0u, 1u, 2u, 2u, 3u, 0u }, {} });
    input.DistanceToView = 0.0f;
    input.DetailedDistance = 12.0f;
    input.HistoryContinuous = true;
    input.Wind.WindStrength = 1.0f;
    input.Wind.WindSpeed = 1.0f;
    input.VelocityBound = 3.0f;

    constexpr u32 kOutputBytes = 4u * static_cast<u32>(sizeof(Vertex));
    auto readback = StorageBuffer::Create(kOutputBytes, StorageBuffer::kNoBinding, StorageBufferUsage::DynamicCopy);
    // Dispatch one extraction and read back the deformed stream its geometry
    // record names, in the same recording, behind a full barrier.
    const auto deform = [&](RT::VegetationSurfaceCache& cache, GPUScene& scene)
    {
        cache.BeginFrame();
        scene.BeginExtraction(17u, glm::vec3(0));
        cache.Queue(input);
        cache.FinishExtraction(scene);
        static_cast<void>(scene.EndExtraction());
        EXPECT_TRUE(cache.HasWork());
        const GPUSceneInstance* instance = scene.GetLiveInstanceRecordBySlot(0u);
        const GPUSceneGeometry* geometry =
            instance != nullptr ? scene.GetLiveGeometryRecordBySlot(instance->GeometryIndex, instance->GeometryGeneration)
                                : nullptr;
        std::array<Vertex, 4> out{};
        if (geometry == nullptr || geometry->VertexAddress == 0u)
        {
            ADD_FAILURE() << "the extraction staged no vegetation geometry";
            return out;
        }
        RecordAndSubmit([&]
                        {
            EXPECT_EQ(cache.Dispatch(), 1u);
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::All);
            VulkanAddressCommands::CmdCopyRange(m_Cmd, geometry->VertexAddress, VulkanAddressCommands::StorageUsage::Present,
                                                readback->GetDeviceAddress(), VulkanAddressCommands::StorageUsage::Present,
                                                kOutputBytes); });
        readback->GetData(out.data(), kOutputBytes);
        return out;
    };

    // The cache under test dispatches once, which creates its uniform
    // buffers and lets them claim both binding points.
    RT::VegetationSurfaceCache cache;
    cache.SetEnabled(true);
    GPUScene scene;
    const std::array<Vertex, 4> first = deform(cache, scene);

    // Then two other producers take the slots, the way DeformedSurfaceCache
    // and the raster foliage path do every frame they run.
    const std::array<u8, 96> zeros{};
    auto rayTracingTenant = UniformBuffer::Create(static_cast<u32>(zeros.size()), ShaderBindingLayout::UBO_RAY_TRACING);
    rayTracingTenant->SetData(zeros.data(), static_cast<u32>(zeros.size()));
    rayTracingTenant->Bind();
    const std::vector<u8> foliageZeros(sizeof(ShaderBindingLayout::FoliageUBO), 0u);
    auto foliageTenant = UniformBuffer::Create(static_cast<u32>(foliageZeros.size()), ShaderBindingLayout::UBO_FOLIAGE);
    foliageTenant->SetData(foliageZeros.data(), static_cast<u32>(foliageZeros.size()));
    foliageTenant->Bind();

    // A later wind time, so the correct answer differs from what the first
    // dispatch left in the buffer.
    input.Wind.Time += 0.5f;
    const std::array<Vertex, 4> displaced = deform(cache, scene);

    // The oracle: a fresh cache whose own buffers are created during this
    // dispatch, so nothing can have displaced them.
    RT::VegetationSurfaceCache reference;
    reference.SetEnabled(true);
    GPUScene referenceScene;
    const std::array<Vertex, 4> expected = deform(reference, referenceScene);

    bool moved = false;
    for (sizet i = 0; i < expected.size(); ++i)
    {
        moved = moved || std::memcmp(&expected[i].Position, &first[i].Position, sizeof(glm::vec3)) != 0;
        // Bitwise: one shader, one input, one device. Any difference means
        // the dispatch read parameters other than its own.
        EXPECT_EQ(std::memcmp(&displaced[i], &expected[i], sizeof(Vertex)), 0)
            << "vertex " << i << ": the dispatch did not read its own uniform blocks after another producer bound "
            << "UBO_RAY_TRACING / UBO_FOLIAGE";
    }
    // The negative control: without a pose change a regression that writes
    // nothing would still compare equal to the stale first dispatch.
    EXPECT_TRUE(moved) << "the wind time step did not move any vertex, so this test cannot see a skipped write";
    reference.Shutdown();
    cache.Shutdown();
}

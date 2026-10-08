// OLO_TEST_LAYER: plumbing
// Included by RayTracingDeviceTest.cpp: reuse its real device, probe shader ABI,
// recording/submission bracket and RT capability gate.
TEST_F(RayTracingDevice, GroomStreamingEvictionRetiresActualAsAfterHeldTraceCompletes)
{
    ScopedVulkanRenderCommandSelection selection;
    struct RestoreLever
    {
        bool Previous = Levers::GroomProxyOnCpu();
        ~RestoreLever()
        {
            Levers::SetGroomProxyOnCpu(Previous);
        }
    } restoreLever;
    Levers::SetGroomProxyOnCpu(true);
    using namespace OloEngine::GroomBindingTest;
    GridSurface grid = MakeGrid(8u);
    WeightAsHinge(grid);
    auto groom = MakeCoat(96u, 8u, 0.6f);
    ASSERT_TRUE(groom);
    Ref<GroomBindingAsset> binding;
    GroomBindingBuildStats bindingStats;
    std::string reason;
    ASSERT_TRUE(GroomBindingBuilder::Build(*groom, grid.View(2u), "StreamingRtBody",
                                           GroomBindingBuildSettings{}, binding, bindingStats, reason))
        << reason;
    const std::vector<glm::mat4> palette{ glm::mat4(1.0f), glm::mat4(1.0f) };
    GroomStrandRequest fine;
    fine.Groom = groom;
    fine.Binding = binding;
    fine.Handle = 0x1257u;
    fine.EntityID = 13;
    fine.WidthScale = 24.0f;
    fine.ApparentPixelSize = 400.0f;
    fine.Build.MaxStrands = 96u;
    fine.StreamingEnabled = true;
    fine.StreamingKey = 0x12570001u;
    GroomDeformationInputs inputs;
    inputs.Surface = grid.View(2u);
    inputs.Skinning = grid.Skinning(palette, palette, true);
    inputs.HasHistory = true;
    fine.DeformationStats = EvaluateGroomRootTransforms(*groom, *binding, inputs, std::nullopt, fine.RootTransforms);
    ASSERT_GT(fine.DeformationStats.RootsDeformed, 0u);
    auto competing = fine;
    competing.EntityID = 12;
    competing.Transform = glm::translate(glm::mat4(1.0f), glm::vec3(20.0f, 0.0f, 0.0f));

    GPUScene scene;
    scene.InitializeGPU(GPUSceneCapacities{ .m_Instances = 4, .m_Geometries = 4 });
    RT::GroomSurfaceCache proxies;
    proxies.SetEnabled(true);
    RT::RayTracingScene rayScene;
    // The existing injection seam supplies the REAL Vulkan backend, whose
    // resident keys can be inspected. No AS policy or device work is mocked.
    m_Backend = RT::CreateVulkanRayTracingBackend();
    ASSERT_NE(m_Backend, nullptr);
    auto* backend = m_Backend.get();
    rayScene.SetBackendForTesting(std::move(m_Backend));
    ASSERT_TRUE(rayScene.IsAvailable());
    const auto extract = [&](std::span<const GroomStrandRequest> requests, bool wanted = true)
    {
        scene.BeginExtraction(0x1257u, glm::vec3(0.0f));
        proxies.Extract(scene, requests, wanted);
        static_cast<void>(scene.EndExtraction());
        scene.Upload();
    };
    const auto build = [&]
    {
        RecordAndSubmit([&]
                        {
            rayScene.Update(scene);
            rayScene.RecordBuildToReadBarrier(); });
    };
    const auto completeFrames = []
    {
        for (u64 frame = 0; frame < VulkanDeferredReclaim::kFramesInFlight; ++frame)
            VulkanDeferredReclaim::Get().NotifyFrameCompleted();
    };
    const auto retiringAsBytes = []
    {
        u64 bytes = 0;
        for (const auto& owner : RendererMemoryTracker::GetInstance().BuildReport().Owners)
            if (owner.Owner.ToView() == "RayTracing")
                bytes += owner.GpuRetiringBytes;
        return bytes;
    };

    const std::array<GroomStrandRequest, 2> initial{ competing, fine };
    extract(initial);
    build();
    ASSERT_EQ(rayScene.GetStats().Resident.TotalBlas(), 2u);
    ASSERT_EQ(rayScene.GetStats().Resident.TlasInstances, 2u);
    const GPUSceneGeometryKey geometryKey{ 13u, 0x1257u, std::numeric_limits<u32>::max() };
    const GPUSceneHandle oldGeometry = scene.FindGeometry(geometryKey);
    ASSERT_TRUE(scene.IsGeometryHandleLive(oldGeometry));
    const RT::GeometryKey oldAsKey{ oldGeometry.m_Index, oldGeometry.m_Generation };
    ASSERT_TRUE(backend->IsBlasResident(oldAsKey));
    const u64 oldTlas = rayScene.GetTlasDeviceAddress();
    ASSERT_NE(oldTlas, 0u);
    completeFrames();
    const u64 retiringBefore = retiringAsBytes();

    auto probe = ComputeShader::Create("assets/shaders/compute/RayTracingProbe.comp");
    ASSERT_TRUE(probe && probe->IsValid());
    constexpr u32 width = 32u, height = 16u;
    std::vector<ProbeRay> rays;
    for (u32 y = 0; y < height; ++y)
        for (u32 x = 0; x < width; ++x)
            rays.push_back({ glm::vec4(0.075f + 0.85f * static_cast<f32>(x) / static_cast<f32>(width - 1u),
                                       0.03f + 0.53f * static_cast<f32>(y) / static_cast<f32>(height - 1u), 2.0f, 0.001f),
                             glm::vec4(0.0f, 0.0f, -1.0f, 4.0f) });
    const u32 rayCount = static_cast<u32>(rays.size());
    auto rayBuffer = StorageBuffer::Create(rayCount * sizeof(ProbeRay), 45u);
    auto oldHits = StorageBuffer::Create(rayCount * sizeof(ProbeHit), 46u);
    auto currentHits = StorageBuffer::Create(rayCount * sizeof(ProbeHit), 47u);
    ASSERT_TRUE(rayBuffer && oldHits && currentHits);
    rayBuffer->SetData(rays.data(), rayCount * sizeof(ProbeRay));

    // The held dispatch needs the canonical tables from its own frame. Copy
    // them into immutable test-owned snapshots so this test isolates AS and
    // proxy retirement; it does not validate GPUScene's table upload lifetime.
    std::vector<GPUSceneInstance> instances(scene.GetInstanceSlotCount());
    std::vector<GPUSceneGeometry> geometries(scene.GetGeometrySlotCount());
    std::vector<GPUSceneMaterial> materials(scene.GetMaterialSlotCount());
    for (u32 slot = 0; slot < instances.size(); ++slot)
        if (const auto* record = scene.GetLiveInstanceRecordBySlot(slot))
        {
            instances[slot] = *record;
            if (const auto* geometry = scene.GetLiveGeometryRecordBySlot(record->GeometryIndex, record->GeometryGeneration))
                geometries[record->GeometryIndex] = *geometry;
            if (const auto* material = scene.GetLiveMaterialRecordBySlot(record->MaterialIndex, record->MaterialGeneration))
                materials[record->MaterialIndex] = *material;
        }
    auto instanceSnapshot = StorageBuffer::Create(instances.size() * sizeof(GPUSceneInstance), 42u);
    auto geometrySnapshot = StorageBuffer::Create(geometries.size() * sizeof(GPUSceneGeometry), 43u);
    auto materialSnapshot = StorageBuffer::Create(materials.size() * sizeof(GPUSceneMaterial), 44u);
    ASSERT_TRUE(instanceSnapshot && geometrySnapshot && materialSnapshot);
    instanceSnapshot->SetData(instances.data(), instances.size() * sizeof(GPUSceneInstance));
    geometrySnapshot->SetData(geometries.data(), geometries.size() * sizeof(GPUSceneGeometry));
    materialSnapshot->SetData(materials.data(), materials.size() * sizeof(GPUSceneMaterial));
    auto oldParams = UniformBuffer::Create(sizeof(ProbeParams), ShaderBindingLayout::UBO_RAY_TRACING);
    auto currentParams = UniformBuffer::Create(sizeof(ProbeParams), ShaderBindingLayout::UBO_RAY_TRACING);
    ASSERT_TRUE(oldParams && currentParams);
    const auto traceCurrent = [&]
    {
        RecordAndSubmit([&]
                        {
            const auto tables = scene.GetRayTracingReadAddresses();
            ProbeParams params;
            params.TlasAddress = SplitAddress(rayScene.GetTlasDeviceAddress());
            params.RayAddress = SplitAddress(StorageDeviceAddress(rayBuffer));
            params.HitAddress = SplitAddress(StorageDeviceAddress(currentHits));
            params.InstanceTableAddress = SplitAddress(tables[0]);
            params.GeometryTableAddress = SplitAddress(tables[1]);
            params.MaterialTableAddress = SplitAddress(tables[2]);
            params.RayCount = rayCount;
            params.InstanceSlotCount = scene.GetInstanceSlotCount();
            params.GeometrySlotCount = scene.GetGeometrySlotCount();
            params.MaterialSlotCount = scene.GetMaterialSlotCount();
            currentParams->Bind();
            currentParams->SetData(&params, sizeof(params));
            probe->Bind();
            RenderCommand::DispatchCompute((rayCount + 63u) / 64u, 1u, 1u); });
        std::vector<ProbeHit> hits(rayCount);
        EXPECT_TRUE(currentHits->GetData(hits.data(), rayCount * sizeof(ProbeHit)));
        return static_cast<u32>(std::ranges::count_if(hits, [](const ProbeHit& hit)
                                                      { return hit.DistanceAndBarycentrics.x > 0.0f; }));
    };
    ASSERT_GT(traceCurrent(), 0u) << "initial fine coat must hit before retirement is tested";

    ProbeParams heldParams;
    heldParams.TlasAddress = SplitAddress(oldTlas);
    heldParams.RayAddress = SplitAddress(StorageDeviceAddress(rayBuffer));
    heldParams.HitAddress = SplitAddress(StorageDeviceAddress(oldHits));
    heldParams.InstanceTableAddress = SplitAddress(StorageDeviceAddress(instanceSnapshot));
    heldParams.GeometryTableAddress = SplitAddress(StorageDeviceAddress(geometrySnapshot));
    heldParams.MaterialTableAddress = SplitAddress(StorageDeviceAddress(materialSnapshot));
    heldParams.RayCount = rayCount;
    heldParams.InstanceSlotCount = static_cast<u32>(instances.size());
    heldParams.GeometrySlotCount = static_cast<u32>(geometries.size());
    heldParams.MaterialSlotCount = static_cast<u32>(materials.size());

    VkCommandBuffer heldCmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocation.commandPool = m_Device->GetCommandPool();
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    ASSERT_EQ(vkAllocateCommandBuffers(m_Device->GetDevice(), &allocation, &heldCmd), VK_SUCCESS);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ASSERT_EQ(vkBeginCommandBuffer(heldCmd, &begin), VK_SUCCESS);
    auto& api = static_cast<VulkanRendererAPI&>(RenderCommand::GetRendererAPI());
    auto heldCompletion = api.BeginRecording(heldCmd);
    // Both parameter objects share a binding. Construction claims it, while
    // SetData only writes bytes, so each dispatch must select its own object.
    oldParams->Bind();
    oldParams->SetData(&heldParams, sizeof(heldParams));
    probe->Bind();
    RenderCommand::DispatchCompute((rayCount + 63u) / 64u, 1u, 1u);
    api.EndRecording();
    ASSERT_EQ(vkEndCommandBuffer(heldCmd), VK_SUCCESS);

    auto floor = fine;
    floor.StreamingFloor = true;
    floor.StreamingKey = 0x12570002u;
    floor.Build.MaxStrands = 48u;
    // As in the VulkanPassSuite regression, repeated bound candidates spend
    // the fixed policy budget with two identities, without huge VRAM pressure.
    std::vector<GroomStrandRequest> saturated(GroomProxyPolicy::UpdatesPerFrame, competing);
    saturated.push_back(floor);
    extract(saturated);
    ASSERT_EQ(proxies.GetStats().Rebuilds, GroomProxyPolicy::UpdatesPerFrame);
    ASSERT_EQ(proxies.GetStats().StreamingInvalidations, 1u);
    ASSERT_FALSE(scene.IsGeometryHandleLive(oldGeometry));
    build();
    EXPECT_FALSE(backend->IsBlasResident(oldAsKey));
    EXPECT_EQ(rayScene.GetStats().Frame.BlasRetired, 1u);
    EXPECT_EQ(rayScene.GetStats().Resident.TotalBlas(), 1u);
    EXPECT_EQ(rayScene.GetStats().Resident.TlasInstances, 1u);
    EXPECT_NE(rayScene.GetTlasDeviceAddress(), oldTlas);
    EXPECT_GT(retiringAsBytes(), retiringBefore) << "old AS backing vanished before the held trace completed";

    EXPECT_EQ(traceCurrent(), 0u) << "new rays still hit the evicted fine coat";

    // Submit the old recording AFTER eviction. No generation may be completed
    // until its real fence signals; old BLAS/TLAS and proxy inputs must survive.
    VkCommandBufferSubmitInfo commandInfo{};
    commandInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    commandInfo.commandBuffer = heldCmd;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &commandInfo;
    ASSERT_EQ(vkResetFences(m_Device->GetDevice(), 1, &m_Fence), VK_SUCCESS);
    ASSERT_EQ(vkQueueSubmit2(m_Device->GetQueue(), 1, &submit, m_Fence), VK_SUCCESS);
    heldCompletion->SubmittedFence = m_Fence;
    ASSERT_EQ(vkWaitForFences(m_Device->GetDevice(), 1, &m_Fence, VK_TRUE, 10'000'000'000ull), VK_SUCCESS);
    heldCompletion->Completed = true;
    std::vector<ProbeHit> heldHits(rayCount);
    ASSERT_TRUE(oldHits->GetData(heldHits.data(), rayCount * sizeof(ProbeHit)));
    EXPECT_GT(std::ranges::count_if(heldHits, [](const ProbeHit& hit)
                                    { return hit.DistanceAndBarycentrics.x > 0.0f; }),
              0)
        << "the held old trace lost its coat after eviction";
    vkFreeCommandBuffers(m_Device->GetDevice(), m_Device->GetCommandPool(), 1, &heldCmd);
    completeFrames();
    EXPECT_EQ(retiringAsBytes(), retiringBefore) << "retired AS backing survived completed generations";
    EXPECT_EQ(RendererMemoryTracker::GetInstance().BuildReport().Reconciliation.Status,
              MemoryReconciliationStatus::Reconciled);

    const std::array<GroomStrandRequest, 2> reload{ competing, floor };
    extract(reload);
    build();
    const auto reloaded = scene.FindGeometry(geometryKey);
    ASSERT_TRUE(scene.IsGeometryHandleLive(reloaded));
    const RT::GeometryKey reloadedAsKey{ reloaded.m_Index, reloaded.m_Generation };
    EXPECT_FALSE(backend->IsBlasResident(oldAsKey));
    EXPECT_TRUE(backend->IsBlasResident(reloadedAsKey));
    EXPECT_EQ(rayScene.GetStats().Resident.TlasInstances, 2u);
    EXPECT_GT(traceCurrent(), 0u) << "the reloaded floor is missing from actual ray queries";
    // RT-off control exercises the same production cache departure and real
    // scene retirement without needing an unsupported-device substitution.
    extract(reload, false);
    build();
    EXPECT_EQ(proxies.GetStats().ResidentBytes, 0u);
    EXPECT_EQ(rayScene.GetStats().Resident.TotalBlas(), 0u);
    EXPECT_EQ(rayScene.GetStats().Resident.TlasInstances, 0u);
    EXPECT_EQ(traceCurrent(), 0u);
    completeFrames();
    rayScene.Shutdown();
    proxies.Shutdown();
    scene.Shutdown();
}

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

// #1533: a group from a layer that casts no raster shadow is staged OUT of the
// shadow-caster mask lane, so ray-traced shadow rays pass through it as the
// shadow maps do while every other ray still hits it. Read off the GPU Scene
// instances the TLAS is built from, folded the way the TLAS folds them.
TEST_F(RayTracingDevice, ANonCastingVegetationGroupIsStagedOutOfTheShadowCasterLane)
{
    ScopedVulkanRenderCommandSelection selection;
    const std::array<Vertex, 4> vertices{
        Vertex({ -0.5f, 0, 0 }, { 0, 1, 0 }, { 0, 0 }), Vertex({ 0.5f, 0, 0 }, { 0, 1, 0 }, { 1, 0 }),
        Vertex({ 0.5f, 1, 0 }, { 0, 1, 0 }, { 1, 1 }), Vertex({ -0.5f, 1, 0 }, { 0, 1, 0 }, { 0, 1 })
    };
    RT::VegetationSurfaceInput input;
    input.Owner = 23u;
    input.FirstPlantId = 1533u;
    input.Rest = VertexBuffer::Create(vertices.data(), sizeof(vertices));
    input.VertexCount = 4u;
    input.Rows.Add({ glm::vec4(0, 0, 0, 1), glm::vec4(0, 1, 1, FoliageWindPhase(1533u)), glm::vec4(1) });
    input.Parts.Add({ 0u, { 0u, 1u, 2u, 2u, 3u, 0u }, {} });
    input.DetailedDistance = 12.0f;
    GPUScene scene;
    RT::VegetationSurfaceCache cache;
    cache.SetEnabled(true);
    const auto stagedMasks = [&](const bool castShadows)
    {
        input.CastShadows = castShadows;
        cache.BeginFrame();
        scene.BeginExtraction(23u, glm::vec3(0));
        cache.Queue(input);
        cache.FinishExtraction(scene);
        static_cast<void>(scene.EndExtraction());
        std::vector<u32> masks;
        for (u32 slot = 0u; slot < scene.GetInstanceSlotCount(); ++slot)
            if (const auto* instance = scene.GetLiveInstanceRecordBySlot(slot))
                masks.push_back(RT::PackInstanceMask(instance->VisibilityMask));
        return masks;
    };

    const std::vector<u32> casting = stagedMasks(true);
    ASSERT_EQ(casting.size(), 1u);
    EXPECT_EQ(casting[0], RT::kInstanceMaskAll) << "a casting layer occludes every ray, shadow rays included";

    const std::vector<u32> nonCasting = stagedMasks(false);
    ASSERT_EQ(nonCasting.size(), 1u);
    EXPECT_EQ(nonCasting[0] & RT::kInstanceMaskShadowCaster, 0u) << "shadow rays would hit a layer the shadow maps skip";
    EXPECT_EQ(nonCasting[0], RT::kVisibilityMaskNoShadowCast) << "every ray but a shadow ray must still see it";
    cache.Shutdown();
}

// #1533: a REFLECTION-ONLY group whose wind refresh does not fit this frame's
// budget keeps the snapshot it has instead of being refused, so the frame
// stays complete and the TLAS stays; holding the oldest snapshot, it refreshes
// first next frame. A casting group in the same place is refused, as before.
TEST_F(RayTracingDevice, AReflectionOnlyGroupOverTheFrameBudgetKeepsItsSnapshot)
{
    ScopedVulkanRenderCommandSelection selection;
    const std::array<Vertex, 4> vertices{
        Vertex({ -0.5f, 0, 0 }, { 0, 1, 0 }, { 0, 0 }), Vertex({ 0.5f, 0, 0 }, { 0, 1, 0 }, { 1, 0 }),
        Vertex({ 0.5f, 1, 0 }, { 0, 1, 0 }, { 1, 1 }), Vertex({ -0.5f, 1, 0 }, { 0, 1, 0 }, { 0, 1 })
    };
    RT::VegetationSurfaceInput reflection;
    reflection.Owner = 31u;
    reflection.FirstPlantId = 7u;
    reflection.Rest = VertexBuffer::Create(vertices.data(), sizeof(vertices));
    reflection.VertexCount = 4u;
    reflection.Rows.Add({ glm::vec4(0, 0, 0, 1), glm::vec4(0, 1, 1, FoliageWindPhase(7u)), glm::vec4(1) });
    reflection.Parts.Add({ 0u, { 0u, 1u, 2u, 2u, 3u, 0u }, {} });
    reflection.DetailedDistance = 12.0f;
    reflection.HistoryContinuous = true;
    reflection.CastShadows = false;

    // A casting group, new this frame, that takes the frame's whole triangle
    // budget: new groups are served first.
    constexpr u32 hogPlants = 64u;
    RT::VegetationSurfaceInput hog = reflection;
    hog.Owner = 32u;
    hog.FirstPlantId = 8u;
    hog.CastShadows = true;
    hog.Rows.Reset();
    for (u32 plant = 0u; plant < hogPlants; ++plant)
        hog.Rows.Add({ glm::vec4(static_cast<f32>(plant), 0, 0, 1), glm::vec4(0, 1, 1, 0), glm::vec4(1) });
    hog.Parts.Reset();
    RT::VegetationSurfacePart hogPart;
    for (u32 triangle = 0u; triangle < RT::VegetationPolicy::TrianglesPerFrame / hogPlants; ++triangle)
        for (const u32 corner : { 0u, 1u, 2u })
            hogPart.Indices.Add(corner);
    hog.Parts.Add(std::move(hogPart));

    GPUScene scene;
    RT::VegetationSurfaceCache cache;
    cache.SetEnabled(true);
    const auto extract = [&](std::initializer_list<const RT::VegetationSurfaceInput*> inputs)
    {
        cache.BeginFrame();
        scene.BeginExtraction(31u, glm::vec3(0));
        for (const RT::VegetationSurfaceInput* input : inputs)
            cache.Queue(*input);
        cache.FinishExtraction(scene);
        static_cast<void>(scene.EndExtraction());
    };
    extract({ &reflection });
    RecordAndSubmit([&]
                    { EXPECT_GT(cache.Dispatch(), 0u); });

    reflection.Wind.Time += 0.1f;
    extract({ &reflection, &hog });
    EXPECT_EQ(cache.GetStats().Refused, 0u);
    EXPECT_EQ(cache.GetStats().StaleReflectionSnapshots, 1u);
    EXPECT_TRUE(cache.GetStats().Complete) << "a refresh that did not fit refused the group and withheld the TLAS";
    EXPECT_TRUE(cache.GetStats().CastersComplete);
    EXPECT_EQ(cache.GetStats().DetailedGroups, 2u) << "the stale group must still be staged";
    RecordAndSubmit([&]
                    { EXPECT_GT(cache.Dispatch(), 0u); });

    extract({ &reflection, &hog });
    EXPECT_EQ(cache.GetStats().StaleReflectionSnapshots, 0u) << "the oldest snapshot must refresh first";
    EXPECT_EQ(cache.GetStats().Refused, 0u);
    RecordAndSubmit([&]
                    { EXPECT_GT(cache.Dispatch(), 0u); });

    // Casting work is served first. Give the hog the NEWER snapshot (its
    // refresh alone this frame), then let both need a refresh: the hog takes
    // the budget and the reflection group keeps its snapshot. Served oldest
    // first, the reflection group would win and the caster be refused.
    hog.Wind.Time += 0.15f;
    extract({ &reflection, &hog });
    EXPECT_EQ(cache.GetStats().Refused, 0u);
    RecordAndSubmit([&]
                    { EXPECT_GT(cache.Dispatch(), 0u); });
    reflection.Wind.Time += 0.1f;
    hog.Wind.Time += 0.1f;
    extract({ &reflection, &hog });
    EXPECT_EQ(cache.GetStats().Refused, 0u);
    EXPECT_TRUE(cache.GetStats().CastersComplete) << "a reflection-only refresh took the budget a caster needed";
    EXPECT_EQ(cache.GetStats().StaleReflectionSnapshots, 1u);
    RecordAndSubmit([&]
                    { EXPECT_GT(cache.Dispatch(), 0u); });

    // Last frame's unrecorded builds are spent first: with one small build
    // owed, the hog's refresh no longer fits.
    cache.ChargeBuildDebt({ 1u, 4u, 2u });
    hog.Wind.Time += 0.1f;
    extract({ &reflection, &hog });
    EXPECT_EQ(cache.GetStats().CarriedBuilds, 1u);
    EXPECT_EQ(cache.GetStats().Refused, 1u) << "the refresh spent budget the backend's retries need";
    EXPECT_FALSE(cache.GetStats().CastersComplete);
    // The reflection group's refresh fitted the two triangles the debt left.
    RecordAndSubmit([&]
                    { EXPECT_GT(cache.Dispatch(), 0u); });
    extract({ &reflection, &hog });
    EXPECT_EQ(cache.GetStats().CarriedBuilds, 0u) << "a debt is charged once";
    EXPECT_EQ(cache.GetStats().Refused, 0u);
    RecordAndSubmit([&]
                    { EXPECT_GT(cache.Dispatch(), 0u); });

    // The same squeeze on a CASTING group refuses it: a shadow ray must not
    // trace wind the raster shadow no longer has.
    reflection.CastShadows = true;
    reflection.Wind.Time += 0.1f;
    hog.FirstPlantId = 9u;
    extract({ &reflection, &hog });
    EXPECT_EQ(cache.GetStats().StaleReflectionSnapshots, 0u);
    EXPECT_EQ(cache.GetStats().Refused, 1u);
    EXPECT_FALSE(cache.GetStats().CastersComplete);
    cache.Shutdown();
}

// #1533: the backend caps vegetation acceleration structures at
// VegetationPolicy::AccelerationStructureBytes by dropping any build past it,
// every frame. So the producer budgets the device's own size for each group:
// the estimate is what a real build occupies, to the byte, and the cache
// refuses the group that would cross the cap instead of leaving the backend
// to drop it forever.
TEST_F(RayTracingDevice, TheCacheBudgetsTheDevicesOwnAccelerationStructureSizes)
{
    ScopedVulkanRenderCommandSelection selection;
    m_Backend = RT::CreateVulkanRayTracingBackend();
    ASSERT_NE(m_Backend, nullptr);
    ASSERT_TRUE(m_Backend->GetCapabilities().Supported);
    const std::array<Vertex, 4> vertices{
        Vertex({ -0.5f, 0, 0 }, { 0, 1, 0 }, { 0, 0 }), Vertex({ 0.5f, 0, 0 }, { 0, 1, 0 }, { 1, 0 }),
        Vertex({ 0.5f, 1, 0 }, { 0, 1, 0 }, { 1, 1 }), Vertex({ -0.5f, 1, 0 }, { 0, 1, 0 }, { 0, 1 })
    };
    const std::array<u32, 6> indices{ 0u, 1u, 2u, 2u, 3u, 0u };
    auto vertexBuffer = VertexBuffer::Create(vertices.data(), static_cast<u32>(sizeof(vertices)));
    auto indexBuffer = IndexBuffer::Create(const_cast<u32*>(indices.data()), 6u);
    ASSERT_TRUE(vertexBuffer && indexBuffer);
    RT::BlasBuildRequest build{};
    build.Key = RT::GeometryKey{ 0u, 1u };
    build.Class = RT::GeometryClass::Deformed;
    build.Reason = RT::BuildReason::FirstBuild;
    build.VertexAddress = vertexBuffer->GetDeviceAddress();
    build.IndexAddress = indexBuffer->GetDeviceAddress();
    build.VertexStride = static_cast<u32>(sizeof(Vertex));
    build.VertexCount = 4u;
    build.IndexCount = 6u;
    build.Vegetation = true;
    const std::array<RT::BlasBuildRequest, 1> builds{ build };
    RecordAndSubmit([&]
                    { EXPECT_EQ(m_Backend->RecordBlasBuilds(builds), 1u); });
    RT::SceneStats stats{};
    m_Backend->PublishStats(stats);
    const u64 estimate = m_Backend->EstimateBlasBytes(RT::GeometryClass::Deformed, 4u, static_cast<u32>(sizeof(Vertex)), 2u);
    ASSERT_GT(estimate, 0u);
    EXPECT_EQ(stats.Resident.AccelerationStructureBytes, estimate) << "the producer would budget a size the build does not take";
    EXPECT_GT(m_Backend->EstimateBlasBytes(RT::GeometryClass::Deformed, 4096u, static_cast<u32>(sizeof(Vertex)), 8192u), estimate);
    m_Backend->Shutdown();

    RT::VegetationSurfaceInput input;
    input.Owner = 41u;
    input.FirstPlantId = 3u;
    input.Rest = VertexBuffer::Create(vertices.data(), sizeof(vertices));
    input.VertexCount = 4u;
    input.Rows.Add({ glm::vec4(0, 0, 0, 1), glm::vec4(0, 1, 1, FoliageWindPhase(3u)), glm::vec4(1) });
    input.Parts.Add({ 0u, { 0u, 1u, 2u, 2u, 3u, 0u }, {} });
    input.AccelerationBytes = RT::VegetationPolicy::AccelerationStructureBytes / 2u + 1u;
    RT::VegetationSurfaceCache cache;
    cache.SetEnabled(true);
    cache.BeginFrame();
    cache.Queue(input);
    input.FirstPlantId = 4u;
    cache.Queue(input);
    EXPECT_EQ(cache.GetStats().Refused, 1u) << "two groups past the cap were both queued";
    EXPECT_EQ(cache.GetStagedAccelerationBytes(), input.AccelerationBytes);
    input.FirstPlantId = 5u;
    input.AccelerationBytes = RT::VegetationPolicy::AccelerationStructureBytes / 2u - 1u;
    cache.Queue(input);
    EXPECT_EQ(cache.GetStats().Refused, 1u) << "the room the first group left";
    EXPECT_EQ(cache.GetStagedAccelerationBytes(), RT::VegetationPolicy::AccelerationStructureBytes);
    cache.Shutdown();
}

// #1533: a group the cache holds under its content key is sent without rows
// (216k rows a frame for the showcase lawn), and is staged as before. Rows
// named under a key the cache does not hold are refused, never guessed.
TEST_F(RayTracingDevice, AGroupHeldUnderItsContentKeyIsSentWithoutRows)
{
    ScopedVulkanRenderCommandSelection selection;
    const std::array<Vertex, 4> vertices{
        Vertex({ -0.5f, 0, 0 }, { 0, 1, 0 }, { 0, 0 }), Vertex({ 0.5f, 0, 0 }, { 0, 1, 0 }, { 1, 0 }),
        Vertex({ 0.5f, 1, 0 }, { 0, 1, 0 }, { 1, 1 }), Vertex({ -0.5f, 1, 0 }, { 0, 1, 0 }, { 0, 1 })
    };
    RT::VegetationSurfaceInput input;
    input.Owner = 51u;
    input.FirstPlantId = 6u;
    input.Rest = VertexBuffer::Create(vertices.data(), sizeof(vertices));
    input.VertexCount = 4u;
    input.Rows.Add({ glm::vec4(0, 0, 0, 1), glm::vec4(0, 1, 1, FoliageWindPhase(6u)), glm::vec4(1) });
    input.Rows.Add({ glm::vec4(1, 0, 0, 1), glm::vec4(0, 1, 1, FoliageWindPhase(7u)), glm::vec4(1) });
    input.Parts.Add({ 0u, { 0u, 1u, 2u, 2u, 3u, 0u }, {} });
    input.DetailedDistance = 12.0f;
    input.HistoryContinuous = true;
    input.CastShadows = false;
    input.ContentKey = 0x1533u;
    GPUScene scene;
    RT::VegetationSurfaceCache cache;
    cache.SetEnabled(true);
    const auto extract = [&](const RT::VegetationSurfaceInput& data)
    {
        cache.BeginFrame();
        scene.BeginExtraction(51u, glm::vec3(0));
        cache.Queue(data);
        cache.FinishExtraction(scene);
        static_cast<void>(scene.EndExtraction());
    };
    EXPECT_FALSE(cache.HoldsContent(input.Owner, input.FirstPlantId, input.Rest, input.ContentKey));
    extract(input);
    RecordAndSubmit([&]
                    { EXPECT_GT(cache.Dispatch(), 0u); });
    ASSERT_TRUE(cache.HoldsContent(input.Owner, input.FirstPlantId, input.Rest, input.ContentKey));

    RT::VegetationSurfaceInput held = input;
    held.Rows.Reset();
    held.HeldPlantCount = 2u;
    held.Wind.Time += 0.1f;
    extract(held);
    EXPECT_EQ(cache.GetStats().Refused, 0u);
    EXPECT_TRUE(cache.GetStats().Complete);
    EXPECT_EQ(cache.GetStats().PlantsRepresented, 2u);
    EXPECT_EQ(cache.GetStats().HistoryReset, false) << "the same rows read as new ones";
    RecordAndSubmit([&]
                    { EXPECT_GT(cache.Dispatch(), 0u); });

    held.ContentKey = 0x1534u;
    extract(held);
    EXPECT_EQ(cache.GetStats().Refused, 1u) << "rows named under a key the cache does not hold were staged";
    EXPECT_FALSE(cache.GetStats().Complete);
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

// #1354: one quad group, the shape every test below starts from.
namespace VegetationPressureTest
{
    inline RT::VegetationSurfaceInput Quad(u64 owner, u64 firstPlantId, const Ref<VertexBuffer>& rest, bool castShadows)
    {
        RT::VegetationSurfaceInput input;
        input.Owner = owner;
        input.FirstPlantId = firstPlantId;
        input.Rest = rest;
        input.VertexCount = 4u;
        input.Rows.Add({ glm::vec4(0, 0, 0, 1), glm::vec4(0, 1, 1, FoliageWindPhase(firstPlantId)), glm::vec4(1) });
        input.Parts.Add({ 0u, { 0u, 1u, 2u, 2u, 3u, 0u }, {} });
        input.DetailedDistance = 12.0f;
        input.HistoryContinuous = true;
        input.CastShadows = castShadows;
        input.PlantSetSum = RT::VegetationPlantTerm(firstPlantId);
        return input;
    }

    // A casting group, new this frame, whose refresh takes the frame's whole
    // triangle budget (new groups are served first among casters).
    inline RT::VegetationSurfaceInput Hog(u64 owner, u64 firstPlantId, const Ref<VertexBuffer>& rest)
    {
        constexpr u32 plants = 64u;
        RT::VegetationSurfaceInput hog = Quad(owner, firstPlantId, rest, true);
        hog.Rows.Reset();
        for (u32 plant = 0u; plant < plants; ++plant)
            hog.Rows.Add({ glm::vec4(static_cast<f32>(plant), 0, 0, 1), glm::vec4(0, 1, 1, 0), glm::vec4(1) });
        hog.Parts.Reset();
        RT::VegetationSurfacePart part;
        for (u32 triangle = 0u; triangle < RT::VegetationPolicy::TrianglesPerFrame / plants; ++triangle)
            for (const u32 corner : { 0u, 1u, 2u })
                part.Indices.Add(corner);
        hog.Parts.Add(std::move(part));
        hog.PlantSetSum = 0u;
        return hog;
    }

    inline Ref<VertexBuffer> QuadRest()
    {
        const std::array<Vertex, 4> vertices{
            Vertex({ -0.5f, 0, 0 }, { 0, 1, 0 }, { 0, 0 }), Vertex({ 0.5f, 0, 0 }, { 0, 1, 0 }, { 1, 0 }),
            Vertex({ 0.5f, 1, 0 }, { 0, 1, 0 }, { 1, 1 }), Vertex({ -0.5f, 1, 0 }, { 0, 1, 0 }, { 0, 1 })
        };
        return VertexBuffer::Create(vertices.data(), sizeof(vertices));
    }
} // namespace VegetationPressureTest

// #1354: a CASTING group whose refresh does not fit keeps its snapshot while
// that is inside the proxies' deadline: reduced cadence within the declared
// shadow error, and the casters stay complete, so no ray-traced effect is
// withheld. Past the deadline the snapshot is obsolete; the group is refused
// and the pressure is named.
TEST_F(RayTracingDevice, ACastingGroupOverTheFrameBudgetHoldsItsSnapshotInsideTheProxyDeadline)
{
    using namespace VegetationPressureTest;
    ScopedVulkanRenderCommandSelection selection;
    const Ref<VertexBuffer> rest = QuadRest();
    RT::VegetationSurfaceInput caster = Quad(41u, 5u, rest, true);
    caster.VelocityBound = 5.0f; // proxy deadline 0.05 s, 0.25 m at 5 m/s
    RT::VegetationSurfaceInput hog = Hog(42u, 6u, rest);

    GPUScene scene;
    RT::VegetationSurfaceCache cache;
    cache.SetEnabled(true);
    const auto extract = [&](std::initializer_list<const RT::VegetationSurfaceInput*> inputs)
    {
        cache.BeginFrame();
        scene.BeginExtraction(41u, glm::vec3(0));
        for (const RT::VegetationSurfaceInput* input : inputs)
            cache.Queue(*input);
        cache.FinishExtraction(scene);
        static_cast<void>(scene.EndExtraction());
    };
    extract({ &caster });
    RecordAndSubmit([&]
                    { EXPECT_GT(cache.Dispatch(), 0u); });

    // 20 ms later the hog takes the budget: the caster holds its snapshot.
    caster.Wind.Time += 0.02f;
    extract({ &caster, &hog });
    EXPECT_EQ(cache.GetStats().CadenceHolds, 1u);
    EXPECT_EQ(cache.GetStats().Refused, 0u);
    EXPECT_TRUE(cache.GetStats().CastersComplete) << "a caster inside its error bound withheld the shadow TLAS";
    EXPECT_GT(cache.GetStats().OldestSnapshotAge, 0.019f) << "the held age is not reported";
    EXPECT_LE(cache.GetStats().OldestSnapshotError, RT::VegetationPolicy::MaximumWorldDisplacementError);
    EXPECT_EQ(cache.GetStats().DominantPressure(), RT::VegetationPressure::FrameWork);
    EXPECT_GT(cache.GetStats().Recovery.RefreshTriangles, 0u) << "the held refresh is recovery work";
    RecordAndSubmit([&]
                    { EXPECT_GT(cache.Dispatch(), 0u); });

    // 100 ms after its snapshot, past the deadline, with a new hog in front:
    // the snapshot is obsolete and must not be published as current.
    caster.Wind.Time += 0.08f;
    hog.FirstPlantId = 7u;
    extract({ &caster, &hog });
    EXPECT_EQ(cache.GetStats().CadenceHolds, 0u);
    EXPECT_EQ(cache.GetStats().Refused, 1u);
    EXPECT_FALSE(cache.GetStats().CastersComplete) << "an obsolete caster snapshot was published as current";
    EXPECT_EQ(cache.GetStats().DominantPressure(), RT::VegetationPressure::FrameWork);
    cache.Shutdown();
}

// #1354: a REFLECTION-ONLY group with no snapshot whose build does not fit
// (a camera cut re-slices every group) is deferred: left out of the scene for
// the frame and counted, not refused, so reflections keep the TLAS. It is
// first in line next frame.
TEST_F(RayTracingDevice, ANewReflectionGroupThatDoesNotFitIsDeferredNotRefused)
{
    using namespace VegetationPressureTest;
    ScopedVulkanRenderCommandSelection selection;
    const Ref<VertexBuffer> rest = QuadRest();
    const RT::VegetationSurfaceInput reflection = Quad(51u, 11u, rest, false);
    const RT::VegetationSurfaceInput hog = Hog(52u, 12u, rest);

    GPUScene scene;
    RT::VegetationSurfaceCache cache;
    cache.SetEnabled(true);
    const auto extract = [&](std::initializer_list<const RT::VegetationSurfaceInput*> inputs)
    {
        cache.BeginFrame();
        scene.BeginExtraction(51u, glm::vec3(0));
        for (const RT::VegetationSurfaceInput* input : inputs)
            cache.Queue(*input);
        cache.FinishExtraction(scene);
        static_cast<void>(scene.EndExtraction());
    };
    extract({ &reflection, &hog });
    EXPECT_EQ(cache.GetStats().DeferredReflectionGroups, 1u);
    EXPECT_EQ(cache.GetStats().DeferredReflectionPlants, 1u);
    EXPECT_EQ(cache.GetStats().Refused, 0u);
    EXPECT_TRUE(cache.GetStats().Complete) << "a deferred reflection group withheld the TLAS from every reflection ray";
    EXPECT_EQ(cache.GetStats().PlantsRepresented, 64u) << "the deferred plants were published anyway";
    RecordAndSubmit([&]
                    { EXPECT_GT(cache.Dispatch(), 0u); });

    // The hog's snapshot is reused (its time did not move), so the deferred
    // group fits now.
    extract({ &reflection, &hog });
    EXPECT_EQ(cache.GetStats().DeferredReflectionGroups, 0u);
    EXPECT_EQ(cache.GetStats().PlantsRepresented, 65u);
    EXPECT_TRUE(cache.GetStats().HistoryReset) << "plants entering the traced scene are a representation change";
    RecordAndSubmit([&]
                    { EXPECT_GT(cache.Dispatch(), 0u); });
    cache.Shutdown();
}

// #1354: the RT histories reset when the TRACED representation changes, not
// whenever a group is created or retired. A camera move re-slices the same
// plants into new groups at the same tier; resetting on that wiped the
// shadow and TAA histories on every frame of camera motion. Plants leaving,
// or changing tier, still reset.
TEST_F(RayTracingDevice, ARegroupAtTheSameTierIsNotARepresentationChange)
{
    using namespace VegetationPressureTest;
    ScopedVulkanRenderCommandSelection selection;
    const Ref<VertexBuffer> rest = QuadRest();
    const Ref<VertexBuffer> otherTier = QuadRest();
    const auto group = [&](u64 firstPlantId, std::initializer_list<u64> plants, const Ref<VertexBuffer>& stream)
    {
        RT::VegetationSurfaceInput input = Quad(61u, firstPlantId, stream, true);
        input.Rows.Reset();
        input.PlantSetSum = 0u;
        for (const u64 plant : plants)
        {
            input.Rows.Add({ glm::vec4(static_cast<f32>(plant), 0, 0, 1), glm::vec4(0, 1, 1, FoliageWindPhase(plant)), glm::vec4(1) });
            input.PlantSetSum += RT::VegetationPlantTerm(plant);
        }
        return input;
    };
    GPUScene scene;
    RT::VegetationSurfaceCache cache;
    cache.SetEnabled(true);
    const auto extract = [&](std::initializer_list<RT::VegetationSurfaceInput> inputs)
    {
        cache.BeginFrame();
        scene.BeginExtraction(61u, glm::vec3(0));
        for (const RT::VegetationSurfaceInput& input : inputs)
            cache.Queue(input);
        cache.FinishExtraction(scene);
        static_cast<void>(scene.EndExtraction());
        RecordAndSubmit([&]
                        { static_cast<void>(cache.Dispatch()); });
        return cache.GetStats().HistoryReset;
    };
    EXPECT_TRUE(extract({ group(1u, { 1u, 2u }, rest), group(3u, { 3u }, rest) })) << "first sight";
    EXPECT_FALSE(extract({ group(1u, { 1u, 2u }, rest), group(3u, { 3u }, rest) }));
    EXPECT_FALSE(extract({ group(1u, { 1u }, rest), group(2u, { 2u, 3u }, rest) })) << "a regroup reset the RT histories";
    EXPECT_EQ(cache.GetStats().PlantsRepresented, 3u);
    EXPECT_TRUE(extract({ group(1u, { 1u }, rest), group(2u, { 2u, 3u }, otherTier) })) << "a tier switch went unnoticed";
    EXPECT_TRUE(extract({ group(1u, { 1u }, rest), group(2u, { 2u }, otherTier) })) << "a plant leaving went unnoticed";
    cache.Shutdown();
}

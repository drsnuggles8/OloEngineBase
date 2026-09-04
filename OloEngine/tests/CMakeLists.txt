enable_testing()

# Heavy-TU compile pool (issue #822): McpFieldRegistry.cpp is pulled OUT of the source list below
# and re-added via a FILE_SET after add_executable, but only when the pool is available — see the
# matching block in OloEngine/src/CMakeLists.txt for why the exclusion must stay conditional. This
# variable holds the one line so the huge inline source list below needs a single line touched,
# not restructured into a separate variable (a collision point for parallel branches on its own).
set(_olo_mcp_field_registry_src ${CMAKE_SOURCE_DIR}/OloEditor/src/MCP/McpFieldRegistry.cpp)
if(OLO_HEAVY_COMPILE_POOL_AVAILABLE)
	set(_olo_mcp_field_registry_src "")
endif()

add_executable(OloEngine-Tests
		OloEngineTest.cpp
		# The suite's own `--olo-*` flags. Parsed in main() before gtest sees
		# argv; replaced twelve environment variables read from 26 sites.
		TestOptions.cpp
		# Per-process, per-case scratch paths. Every test that writes a file goes
		# through this instead of std::filesystem::temp_directory_path() — see
		# docs/agent-rules/shared-temp-dir-test-isolation.md (issue #789).
		TestTempDir.cpp
		TestTempDirTest.cpp
		FastRandomTest.cpp
		RefTest.cpp
		Core/BuildInfoTest.cpp
		# Guards the debug-lever registry, including the grep test that stops a
		# new lever from being added as a bare Env:: read again.
		Core/CVarTest.cpp
		Core/DebugLeversTest.cpp
		ContainerTest.cpp
		SnowSettingsTest.cpp
		WindSettingsTest.cpp
		PrecipitationSettingsTest.cpp
		PrecipitationEmitterTest.cpp
		PrecipitationSystemTest.cpp
		CoreUtilitiesTest.cpp
		MathTest.cpp
		# Issue #1019: every type compared whole-object through Math::BitwiseEqual
		# must have no padding bytes.
		BitwiseEqualLayoutTest.cpp
		ModelImporterTest.cpp
		MeshInterchangeTest.cpp
		VolumeImportTest.cpp
		SceneMeshRaycastTest.cpp
		SlugFontRenderingTest.cpp
		AssetCreationTest.cpp
		AssetBinaryIntegrityTest.cpp
		AssetContentValidityTest.cpp
		AssetExtensionsCoverageTest.cpp
		AssetFileWatchPolicyTest.cpp
		SRGBTextureSupportTest.cpp
		AssetSceneLoadTest.cpp
		AssetGenerationTest.cpp
		AssetLuaScriptValidityTest.cpp
		AssetCSharpScriptValidityTest.cpp
		AssetPackTest.cpp
		RuntimeAssetPackTest.cpp
		AssetRegistryInvariantsTest.cpp
		RuntimeAssetManagerTest.cpp
		AssetLoadedEventTest.cpp
		ComponentRoundTripTest.cpp
		ComponentSerializerCoverageTest.cpp
		ComponentTupleCoverageTest.cpp
		ComponentHandlerCoverageTest.cpp
		OloHeaderToolOutputsTest.cpp
		SceneSerializerFuzzRegressionTest.cpp
		SceneVersionMigrationTest.cpp
		EngineSubsystemSmokeTest.cpp
		AppLaunchSmokeTest.cpp
		GpuGateSwitchTest.cpp
		FunctionalTestFixtureCoverageTest.cpp
		PreCommitToolingSmokeTest.cpp
		PrefabOverrideTest.cpp
		ShadowMapTest.cpp
		PostProcessTest.cpp
		LightCullingTest.cpp
		SoundGraphBasicTest.cpp
		SoundGraphInstantiationTest.cpp
		SoundGraphTypedConnectionTest.cpp
		SoundGraphSampleAccurateTriggerTest.cpp
		SoundGraphVoiceSuspendTest.cpp
		SoundGraphParameterWiringTest.cpp
		SoundGraphControlRoutingTest.cpp
		SoundGraphCacheTest.cpp
		SoundGraphNodeSchemaCoverageTest.cpp
		AudioEventQueueTest.cpp
		AudioDSPTest.cpp
		AudioSpatializerTest.cpp
		AudioSoundGraphSpatializerTest.cpp
		VideoPlaybackTest.cpp
		Audio/CommandIDTest.cpp
		Audio/AudioCommandRegistryTest.cpp
		Audio/AudioEventsManagerTest.cpp
		Audio/AudioEventsSceneIntegrationTest.cpp
		Audio/VoiceManagerTest.cpp
		SphericalHarmonicsTest.cpp

		Async/ParkingLotTest.cpp
		Async/MutexTest.cpp
		Async/MutexLockRateTest.cpp
		Async/SemaphoreTest.cpp
		Async/ManualResetEventTest.cpp
		Async/LockDebugTest.cpp
		Async/SharedMutexTest.cpp
		Async/SharedRecursiveMutexTest.cpp
		Async/ExternalMutexTest.cpp
		Tasks/TaskSystemTest.cpp
		HAL/ThreadManagerTest.cpp
		Containers/ConcurrentQueuesTest.cpp
		Containers/StringTest.cpp
		Memory/MemoryViewTest.cpp
		Memory/LockFreeAllocatorConcurrencyTest.cpp
		Templates/FunctionWithContextTest.cpp
		Templates/TypeTraitsTest.cpp

		# UI subsystem tests
		UI/UIInputFieldKeyboardTest.cpp

		# Rendering subsystem tests (Tiers 1-5)
		Rendering/DrawKeyTest.cpp
		Rendering/TextureCompressionTest.cpp
		Rendering/BoundingVolumeTest.cpp
		Rendering/FrustumCullingTest.cpp
		Rendering/ObserverCameraTest.cpp
		Rendering/RendererSettingsBootstrapTest.cpp
		Rendering/ReflectionProbeSelectionTest.cpp
		Rendering/ReflectionProbeDistanceFieldTest.cpp
		Rendering/PlanarReflectionMathTest.cpp
		Rendering/CameraRelativeTest.cpp
		Rendering/Renderer2DCameraRelativeTest.cpp
		Rendering/TerrainCameraRelativeLODTest.cpp
		Rendering/TerrainGPUQuadtreeTest.cpp
		Rendering/TerrainGPUPickerLayoutTest.cpp
		Rendering/LODTest.cpp
		Rendering/MeshOptimizationTest.cpp
		Rendering/SubmeshMaterialResolveTest.cpp
		Rendering/VirtualMeshBuilderTest.cpp
		Rendering/VirtualMeshCookIdentityTest.cpp
		Rendering/VirtualClusterCullParityTest.cpp
		Rendering/VirtualClusterTwoPhaseOcclusionTest.cpp
		Rendering/VirtualMeshletEligibilityTest.cpp
		Rendering/MeshSourceBoundsTest.cpp
		Rendering/MeshBVHRaycastTest.cpp
		Rendering/PathTracing/ReferenceBRDFTest.cpp
		Rendering/PathTracing/ClosureV2Test.cpp
		Rendering/PathTracing/ClosureV2ConsistencyTest.cpp
		Rendering/PathTracing/PathTracerFurnaceTest.cpp
		Rendering/PathTracing/PathTracerCornellBoxTest.cpp
		Rendering/PathTracing/ReferenceSceneBuilderTest.cpp
		Rendering/Baking/LightmapUnwrapTest.cpp
		Rendering/Baking/LightmapBakeParityTest.cpp
		Rendering/Baking/LightmapAtlasPagingTest.cpp
		Rendering/Baking/LightProbePathTracedBakeTest.cpp
		Asset/LightmapAssetSerializationTest.cpp
		Rendering/FoliageImpostorPlacementTest.cpp
		Rendering/PODCommandTest.cpp
		Rendering/ShaderBindingLayoutTest.cpp
		Rendering/ShaderReflectionBindingTest.cpp
		Rendering/ShaderCrossConsistencyTest.cpp
		Rendering/ShaderUBOSizeConsistencyTest.cpp
		Rendering/ShaderStageInterfaceTest.cpp
		Rendering/ShaderStageContractTest.cpp
		Rendering/ShaderPackTest.cpp
		Rendering/ShaderPackContentHashTest.cpp
		Rendering/ShaderPackBakeTest.cpp
		Rendering/ShaderBinaryCacheRoundTripTest.cpp
		Rendering/ShaderResourceRegistrySelfRefTest.cpp
		Rendering/GLDebugMessageParseTest.cpp
		Rendering/FrameDataBufferTest.cpp
		Rendering/InstanceDataLayoutTest.cpp
		Rendering/PropertyTests/GPUScenePropertyTests.cpp
		Rendering/PropertyTests/GPUSceneLayoutTest.cpp
		Rendering/PropertyTests/GPUSceneGpuTest.cpp
		Rendering/PropertyTests/GPUSceneMaterialRecordPropertyTests.cpp
		Rendering/PropertyTests/GPUSceneLightRecordPropertyTests.cpp
		Rendering/PropertyTests/GPUSceneEnvironmentAndLifecyclePropertyTests.cpp
		Rendering/PropertyTests/GPUSceneLightAdapterTest.cpp
		Rendering/PropertyTests/GPUSceneRasterMigrationTest.cpp
		Rendering/PropertyTests/GPUSceneRasterMigrationEvidenceTest.cpp
		Rendering/PropertyTests/GPUSceneAntiDuplicationRatchetTest.cpp
		Rendering/LightmapPageEncodingTest.cpp
		Rendering/GPUFrustumCullParityTest.cpp
		Rendering/GPUReadbackStatsLayoutTest.cpp
		Rendering/GPUPrefixSumTest.cpp
		Rendering/Splat/GaussianSplatPipelineTest.cpp
		Rendering/Splat/GaussianSplatVisualEvidenceTest.cpp
		Rendering/Splat/GaussianSplatLodTest.cpp
		Rendering/Splat/GaussianSplatGpuOrderingParityTest.cpp
		Rendering/PropertyTests/UniformBufferBindingOwnershipTest.cpp
		Rendering/GPUPrefixSumPerfProbe.cpp
		Rendering/GPUHashMapTest.cpp
		Rendering/GPUPagedCacheTest.cpp
		Rendering/GPUCacheShaderResolveTest.cpp
		Rendering/GPUParticleCompactionTest.cpp
		Rendering/ShaderDebugDrawContractTest.cpp
		Rendering/ShaderDebugDrawExpansionTest.cpp
		Rendering/GPUOcclusionCullParityTest.cpp
		Rendering/DeferredTwoPhaseOcclusionTest.cpp
		Rendering/ScatterBrushMathTest.cpp
		Rendering/SphereAreaLightMathTest.cpp
		Rendering/ProceduralSkyMathTest.cpp
		Rendering/StarNestSkyMathTest.cpp
		Atmosphere/EphemerisMathTest.cpp
		Atmosphere/WeatherBlendTest.cpp
		Atmosphere/CloudDensityMathTest.cpp
		Atmosphere/AtmosphereSkyMathTest.cpp
		Rendering/PropertyTests/AtmosphereVisualEvidenceTest.cpp
		Rendering/PropertyTests/DriftWeatherVisualEvidenceTest.cpp
		Rendering/PropertyTests/VolumetricShadowVisualEvidenceTest.cpp
		Rendering/AutoExposureMathTest.cpp
		Rendering/ScreenSpaceReflectionMathTest.cpp
		Rendering/ScreenSpaceGIMathTest.cpp
		Rendering/StochasticSamplerTest.cpp
		Rendering/SurfaceHistoryTest.cpp
		Rendering/TemporalHistoryRegistryTest.cpp
		Rendering/ContactShadowMathTest.cpp
		Rendering/OverdrawHeatmapMathTest.cpp
		Rendering/CASMathTest.cpp
		Rendering/EASUMathTest.cpp
		Rendering/RCASMathTest.cpp
		Rendering/FSR2PolicyTest.cpp
		Rendering/UpscaleSettingsSceneYAMLRoundTripTest.cpp
		Rendering/SSAOMathTest.cpp
		Rendering/GTAOMathTest.cpp
		Rendering/SphereProxyAOMathTest.cpp
		Rendering/VRCSContractTest.cpp
		Rendering/VRCSClassifierGpuTest.cpp
		Rendering/ThreadGroupSwizzleTest.cpp
		Rendering/BloomMathTest.cpp
		Rendering/FogMathTest.cpp
		Rendering/ClusteredLightingMathTest.cpp
		Rendering/AtlasAllocatorTest.cpp
		Rendering/ShadowAtlasPackingTest.cpp
		Rendering/VolumetricFogMathTest.cpp
		Rendering/VolumetricShadowMapMathTest.cpp
		Rendering/MotionVectorMathTest.cpp
		Rendering/DDGI/DDGIMathTest.cpp
		Rendering/SSRHiZTraversalTest.cpp
		Rendering/RenderGraphFingerprintTest.cpp
		Rendering/CommandPacketTest.cpp
		Rendering/CommandAllocatorTest.cpp
		Rendering/CommandBucketTest.cpp
		Rendering/PODRenderStateChannelMaskTest.cpp
		#Rendering/CommandDispatchTest.cpp  # Requires full engine (calls CommandDispatch::Initialize which pulls in OpenGL statics)
		Rendering/RenderGraphTest.cpp
		# Phase 2 ratchet for the Vulkan-RHI epic (#691, ADR 0011): counts raw
		# glXxx() outside Platform/OpenGL/ and fails when it rises above
		# Rendering/rhi_boundary_baseline.json. Also the only TU that compiles the
		# declaration-only Renderer/RHI/ headers.
		Rendering/RHIBoundaryRatchetTest.cpp
		Rendering/RHIResourceRegistryTest.cpp
		Rendering/RHIDescriptorHeapTest.cpp
		Rendering/RHIEnumLoweringTest.cpp
		# #691 Phase 4: Vulkan device-selection gate (ADR 0010 capability contract).
		# SKIPs cleanly without a loader/device; compiles to a single SKIP when
		# OLO_WITH_VULKAN=OFF.
		Rendering/VulkanBringUpTest.cpp
		# #691 Phase 5: the pure Access -> (stage2, access2, layout) lowering
		# tables + the image-layout tracker. Headless (no device — fabricated
		# handles); compiles to a single SKIP when OLO_WITH_VULKAN=OFF.
		Rendering/VulkanBarrierLoweringTest.cpp
		# #806: parallel command recording — the layout-overlay merge rule
		# (pure CPU, fabricated handles) and the device-gated fork/join
		# (N items into N targets through one vkCmdExecuteCommands).
		Rendering/VulkanParallelRecordingTest.cpp
		# #691 Phase 5: the render graph's execution layer on a REAL device —
		# VMA transients, vkCmdPipelineBarrier2 batches, CommandBucket dispatch
		# through VulkanRendererAPI, asserted validation-error count == 0.
		# Same SKIP ladder as VulkanBringUpTest.
		Rendering/VulkanRenderGraphExecutionTest.cpp
		# #691 Phase 6: the shader path on a REAL device — VulkanShader
		# (vulkan_1_4 SPIR-V tier), descriptor-heap sampling, thin PSOs with
		# root-data binding mappings, vertex pulling, and the FXAA golden-pass
		# checkpoint (ADR 0011 §4/§5). Same SKIP ladder + zero-validation bar.
		Rendering/VulkanShaderPipelineTest.cpp
		# #691 Phase 7 Stage 1.2: the buffer/texture factory arms on a REAL
		# device — arena-versioned UBO addresses, texture upload/readback with
		# the mip blit chain and RGB->RGBA widening, the descriptor slot
		# cache's keying + recycling, SSBO data paths. Same SKIP ladder +
		# zero-validation bar.
		Rendering/VulkanResourceFactoryTest.cpp
		# #691 Phase 7 Stage 1.6a: the facade draw path on a REAL device — an
		# unmodified GL-shaped pass body (FB bind, clear, shader bind, UBO
		# bind, BindTexture, DrawIndexed) renders through VulkanBindingState,
		# root-data assembly, the thin-PSO fetch and the lazy dynamic-
		# rendering scope. Same SKIP ladder + zero-validation bar.
		Rendering/VulkanDrawPathTest.cpp
		# #691 Phase 7 Stage 1.6b: REAL passes through the REAL render graph on
		# a device — transients, planner barriers, pass bodies recording via
		# the process-global backend; first tenant is the golden-gated FXAA
		# pass. Same SKIP ladder + zero-validation bar.
		Rendering/VulkanPassSuiteTest.cpp
		# #691 Phase 4: the --rhi= / config-file backend-selection chain (headless).
		Rendering/BackendSelectionTest.cpp
		# Coordinate math of GPUResourceInspector::CaptureTexturePng's `region`
		# argument (#607 native-resolution capture): the top-left-origin rect must
		# land on exactly those texels through GL's bottom-up rows. Needs a GL
		# context (skips cleanly without) and a SYNTHETIC gradient — measured
		# against a live animated scene the mirrored position scored BETTER than
		# the correct one, so a live pixel-compare cannot pin this.
		Rendering/CaptureRegionReadbackTest.cpp
		# The mip-level count the GL inspector backend reports, and the memory
		# estimate derived from it. GL_TEXTURE_MAX_LEVEL is a sampling ceiling
		# defaulting to 1000, not a level count — reading it as one made every
		# texture report 1001 levels. Needs a GL context (skips cleanly without);
		# covers immutable (exact), partial-chain and mutable (probed) storage.
		Rendering/ResourceInspectorTextureQueryTest.cpp
		Rendering/ResourceHazardValidationTests.cpp
		Rendering/RenderGraphPathSwitchTests.cpp
		Rendering/FramePipelineTest.cpp
		Rendering/FrameCaptureTest.cpp
		Rendering/CommandBucketBenchmarkTest.cpp
		Rendering/RenderStateTest.cpp
		Rendering/OcclusionStateTest.cpp
		Rendering/OcclusionIntegrationTest.cpp
		Rendering/WaterGerstnerNormalScaleTest.cpp
		Rendering/WaterRenderingTest.cpp
		Rendering/FluidRenderMathTest.cpp
		Fluid/FluidKernelContractTest.cpp
		Fluid/CPUFluidSolverContractTest.cpp
		Rendering/PCSSShadowTest.cpp
		Rendering/VirtualShadowMapMarkPassTest.cpp
		Rendering/VirtualShadowMapVulkanShaderTest.cpp
		Rendering/VirtualShadowMapTest.cpp
		Rendering/VirtualShadowMapLocalTest.cpp
		Rendering/RenderingRegressionTest.cpp
		Rendering/SelectionOutlineTest.cpp
		Rendering/DeccerCubesLoadingTest.cpp
		Rendering/AnimatedModelDedupTest.cpp
		# Rendering property tests (require GL 4.5+ context; skip gracefully if unavailable)
		Rendering/PropertyTests/RenderPropertyTest.cpp
		Rendering/PropertyTests/FacadeReadbackParityTest.cpp
		Rendering/PropertyTests/CameraRelativeVisualEvidenceTest.cpp
		Rendering/PropertyTests/WorldOriginRebaseVisualEvidenceTest.cpp
		Rendering/PropertyTests/Renderer2DCameraRelativeVisualEvidenceTest.cpp
		Rendering/PropertyTests/PlanarReflectionVisualEvidenceTest.cpp
		Rendering/PropertyTests/HZBGeneratorResizeTest.cpp
		Rendering/PropertyTests/GPUOcclusionCullGPUTest.cpp
		Rendering/PropertyTests/ShaderDebugDrawGpuPushTest.cpp
		Rendering/PropertyTests/ShaderDebugDrawVisualTest.cpp
		Rendering/PropertyTests/GPUFluidSolverParityTest.cpp
		Rendering/PropertyTests/OcclusionCullVisualEvidenceTest.cpp
		Rendering/PropertyTests/AutoMeshLODVisualEvidenceTest.cpp
		Rendering/PropertyTests/OcclusionCullDeferredVisualEvidenceTest.cpp
		Rendering/PropertyTests/DeferredInstancedShadingEvidenceTest.cpp
		Rendering/PropertyTests/OceanFFTGpuContractTest.cpp
		Rendering/PropertyTests/OceanCascadeTest.cpp
		Rendering/PropertyTests/OceanCascadeVisualEvidenceTest.cpp
		Rendering/PropertyTests/OceanFFTSpectrumTest.cpp
		Rendering/PropertyTests/PostProcessPropertyTests.cpp
		Rendering/PropertyTests/PbrPropertyTests.cpp
		Rendering/PathTracing/ReferenceBRDFGpuParityTest.cpp
		Rendering/PathTracing/ClosureV2GpuParityTest.cpp
		Rendering/PropertyTests/ShaderMathIdentityTests.cpp
		Rendering/PropertyTests/GLStateGuardTest.cpp
		Rendering/PropertyTests/RendererValidateTest.cpp
		Rendering/PropertyTests/ShaderUnitTests.cpp
		Rendering/PropertyTests/PerfRegressionTests.cpp
		Rendering/PropertyTests/DataRoundTripTests.cpp
		Rendering/PropertyTests/GoldenImageTests.cpp
		Rendering/PropertyTests/GoldenBaselineAuditTest.cpp
		Rendering/PropertyTests/SphereAreaLightVisualTest.cpp
		Rendering/PropertyTests/MaterialLabVisualEvidenceTest.cpp
		Rendering/PropertyTests/ClosureV2DeferredParityEvidenceTest.cpp
		Rendering/PropertyTests/ProceduralSkyBakeTest.cpp
		Rendering/PropertyTests/ProceduralSkyVisualTest.cpp
		Rendering/PropertyTests/StarNestSkyBakeTest.cpp
		Rendering/PropertyTests/StarNestSkyVisualTest.cpp
		Rendering/PropertyTests/StarNestReflectionEvidenceTest.cpp
		Rendering/PropertyTests/WaterVisualEvidenceTest.cpp
		Rendering/PropertyTests/BenchmarkCaptureTest.cpp
		Rendering/PropertyTests/MaterialLabSceneEvidenceTest.cpp
		Rendering/PropertyTests/PlayerCameraRigVisualEvidenceTest.cpp
		Rendering/PropertyTests/VirtualGeometryVisualEvidenceTest.cpp
		Rendering/PropertyTests/SubmeshMaterialPathParityTest.cpp
		Rendering/PropertyTests/ModelWarmCacheGeometryIdentityTest.cpp
		Rendering/PropertyTests/VirtualGeometryRasterParityTest.cpp
		Rendering/PropertyTests/VirtualMeshAssetPackTest.cpp
		Rendering/PropertyTests/VirtualMeshRealAssetCookTest.cpp
		Rendering/PropertyTests/ImportedMaterialPackTest.cpp
		Rendering/PropertyTests/FluidVisualEvidenceTest.cpp
		Rendering/PropertyTests/WaterStaleTexturePublicationTest.cpp
		Rendering/PropertyTests/CompressedTextureVisualEvidenceTest.cpp
		Rendering/PropertyTests/TextureAutoCookPackTest.cpp
		Rendering/PropertyTests/OceanFFTVisualEvidenceTest.cpp
		Rendering/PropertyTests/UnderwaterCausticsVisualTest.cpp
		Rendering/PropertyTests/SSRVisualEvidenceTest.cpp
		Rendering/PropertyTests/BloomVisualEvidenceTest.cpp
		Rendering/PropertyTests/ObserverCameraVisualEvidenceTest.cpp
		Rendering/PropertyTests/FogVisualEvidenceTest.cpp
		Rendering/PropertyTests/ClusteredLightingVisualEvidenceTest.cpp
		Rendering/PropertyTests/ShadowAtlasVisualEvidenceTest.cpp
		Rendering/PropertyTests/VolumetricFogVisualEvidenceTest.cpp
		Rendering/PropertyTests/VolumeVDBVisualEvidenceTest.cpp
		Rendering/PropertyTests/ReflectionProbeVisualEvidenceTest.cpp
		Rendering/PropertyTests/ReflectionProbeParallaxVisualEvidenceTest.cpp
		Rendering/PropertyTests/SSGIVisualEvidenceTest.cpp
		Rendering/PropertyTests/ScreenSpaceTemporalResolveEvidenceTest.cpp
		Rendering/DDGI/DDGIVisualEvidenceTest.cpp
		Rendering/PathTracing/DDGIReferenceParityTest.cpp
		Rendering/PropertyTests/OverdrawVisualEvidenceTest.cpp
		Rendering/PropertyTests/ContactShadowVisualEvidenceTest.cpp
		Rendering/PropertyTests/VirtualShadowMapVisualEvidenceTest.cpp
		Rendering/PropertyTests/VirtualShadowMapLocalVisualEvidenceTest.cpp
		Rendering/PropertyTests/CASVisualEvidenceTest.cpp
		Rendering/PropertyTests/EASUVisualEvidenceTest.cpp
		Rendering/PropertyTests/FSR2VisualEvidenceTest.cpp
		Rendering/PropertyTests/SSAOVisualEvidenceTest.cpp
		Rendering/PropertyTests/GTAOVisualEvidenceTest.cpp
		Rendering/PropertyTests/SphereProxyAOVisualEvidenceTest.cpp
		Rendering/PropertyTests/VRCSVisualEvidenceTest.cpp
		Rendering/PropertyTests/VRCSPerfProbe.cpp
		Rendering/PropertyTests/PCSSVisualEvidenceTest.cpp
		Rendering/PropertyTests/ShadowEarlyOutVisualEvidenceTest.cpp
		Rendering/PropertyTests/TwoSidedShadowCastTest.cpp
		Rendering/PropertyTests/WaterShoreVisualEvidenceTest.cpp
		Rendering/PropertyTests/WaterShoreWaveTest.cpp
		Rendering/PropertyTests/WaterSurfaceSamplerTest.cpp
		Rendering/PropertyTests/WaterDisturbanceFieldTest.cpp
		Rendering/PropertyTests/WaterWakeShapeTest.cpp
		Rendering/PropertyTests/WaterWakeParityTest.cpp
		Rendering/PropertyTests/WaterWakeShapeVisualEvidenceTest.cpp
		Rendering/PropertyTests/WaterWakeVisualEvidenceTest.cpp
		Rendering/PropertyTests/WaterBuoyancyVisualEvidenceTest.cpp
		Rendering/PropertyTests/ShadowTerrainPropertyTests.cpp
		Rendering/PropertyTests/TerrainGenerationEvidenceTest.cpp
		Rendering/PropertyTests/TerrainCameraRelativeVisualEvidenceTest.cpp
		Rendering/PropertyTests/TerrainGPUQuadtreeVisualEvidenceTest.cpp
		Rendering/PropertyTests/DriftIslandFieldEvidenceTest.cpp
		Rendering/PropertyTests/TerrainGPUPickEvidenceTest.cpp
		Rendering/PropertyTests/TerrainVTCompressBC7Test.cpp
		Rendering/PropertyTests/TerrainGPUBrushParityTest.cpp
		Rendering/PropertyTests/TerrainGPUBrushVisualEvidenceTest.cpp
		Rendering/PropertyTests/TerrainVirtualTextureVisualEvidenceTest.cpp
		Rendering/PropertyTests/FoliageGenerationEvidenceTest.cpp
		Rendering/PropertyTests/VoxelGreedyMeshVisualEvidenceTest.cpp
		Rendering/PropertyTests/VoxelMarchingCubesVisualEvidenceTest.cpp
		Rendering/PropertyTests/DeferredPropertyTests.cpp
		Rendering/PropertyTests/DeferredOverlayPassTests.cpp
		# Issue #865: the G-Buffer's sixth target (baked lightmap irradiance +
		# coverage) has three contracts a compiler cannot check — every G-Buffer
		# writer must write it, its sampler slot mirrors ShaderBindingLayout, and
		# the two paths' top ambient rung must stay the same rung. Headless text
		# scans in front of the GPU parity test.
		Rendering/PropertyTests/GBufferBakedGIContractTest.cpp
		Rendering/PropertyTests/GBufferFlagsResolveEvidenceTest.cpp
		Rendering/PropertyTests/OITPropertyTests.cpp
		Rendering/PropertyTests/ShaderCompilationTest.cpp
		# Issue #691 Phase 3. BindlessShaderPipelineTest pins the toolchain
		# constraint (bindless GLSL cannot enter the Vulkan-SPIR-V pipeline);
		# BindlessHeapGpuTest is the end-to-end GPU proof — a real texture reached
		# through nothing but an integer offset, with poison and the transient ring
		# checked against read-back pixels. Skips cleanly without GL 4.6 or without
		# GL_ARB_bindless_texture.
		Rendering/PropertyTests/BindlessShaderPipelineTest.cpp
		Rendering/PropertyTests/BindlessHeapGpuTest.cpp
		Rendering/PropertyTests/ShaderCompileFailureRecoveryTest.cpp
		Rendering/PropertyTests/AssetPreviewRendererTest.cpp
		Rendering/PropertyTests/RendererAttachedTest.cpp
		Rendering/RendererMemoryTrackerTest.cpp
		Rendering/MaterialCopyTest.cpp
		Rendering/RendererShutdownTest.cpp
		Rendering/RenderPathDriftTest.cpp
		Rendering/VirtualMeshRegistryRejectionTest.cpp
		Rendering/VirtualGeometrySceneCoverageTest.cpp
		Rendering/PropertyTests/RendererAttachedSmokeTest.cpp
		Rendering/PropertyTests/SceneRenderEvidenceTest.cpp
		Rendering/PropertyTests/LightmapVisualEvidenceTest.cpp
		Rendering/PropertyTests/ClothVisualEvidenceTest.cpp
		Rendering/PropertyTests/ClothCapeVisualEvidenceTest.cpp
		Rendering/PropertyTests/LocomotionVisualEvidenceTest.cpp
		Rendering/PropertyTests/RebindMenuVisualEvidenceTest.cpp
		# Headless-attach stretch of issue #316: hosts the read-only MCP server
		# inside the test binary against the offscreen render-graph FB so
		# olo_screenshot / olo_shader_errors work in the headless visual-verify
		# loop. Header-only host helper (McpHeadlessHost.h); pulls in the real MCP
		# tools (McpTools.cpp + McpScriptApi.cpp) added to the source list below.
		Rendering/PropertyTests/McpHeadlessAttachTest.cpp
		# Pins the #519 "first perf-lever write after scene load doesn't take
		# effect" settle-wait fix: olo_renderer_settings_set now waits out the
		# render-budget throttle transient before returning. Same McpHeadlessHost
		# infra as McpHeadlessAttachTest.cpp above.
		Rendering/PropertyTests/McpPerfSettleTest.cpp
		Rendering/PropertyTests/VideoOverlayVisualEvidenceTest.cpp
		Rendering/PropertyTests/AutoExposureEvidenceTest.cpp
		Rendering/PropertyTests/GLAttachmentBlendOpinionTest.cpp
		Rendering/PropertyTests/GPUReadbackStatsEvidenceTest.cpp
		Rendering/PropertyTests/TestFailureCapture.cpp
		Rendering/PropertyTests/TestFailureCaptureTest.cpp
		# Cross-test GL-error-state guard (issue #485): a global gtest listener
		# that drains + asserts a clean glGetError() after every test, pinning
		# GL-context pollution to its source. Registered from OloEngineTest.cpp.
		Rendering/PropertyTests/GLErrorStateCheck.cpp
		Rendering/PropertyTests/GLErrorStateCheckTest.cpp
		Rendering/PropertyTests/ModelLoadDeterminismTest.cpp
		Rendering/PropertyTests/DrawIndexedRawOffsetTest.cpp
		Rendering/PropertyTests/TextureSaveRoundTripTest.cpp
		Rendering/PropertyTests/Texture2DArrayRoundTripTest.cpp
		Rendering/PropertyTests/RHIHandleNativeIdentityTest.cpp
		Rendering/PropertyTests/TextureInPlaceReloadTest.cpp
		Streaming/SceneStreamingTest.cpp
		InputActionTest.cpp
		# The synthetic-input overlay behind olo_input_inject, and specifically the
		# absolute-vs-relative distinction (#607): an ABSOLUTE override integrates to
		# exactly zero for a delta-consuming mouse-look rig however long it is held
		# (the spike and its mirror cancel), which is why the fix had to be a
		# persistent cursor OFFSET rather than a longer-held override.
		SyntheticInputTest.cpp
		InputRebindControllerTest.cpp
		UI/RuntimeInputRebindMenuTest.cpp
		GamepadTest.cpp
		Networking/NetworkMessageTest.cpp
		Networking/NetworkMessageDispatcherTest.cpp
		Networking/NetworkThreadDispatchTest.cpp
		Networking/NetworkIntegrationTest.cpp
		Networking/ComponentReplicatorTest.cpp
		Networking/ComponentInterpolationRegistryTest.cpp
		Networking/EntitySnapshotTest.cpp
		Networking/NetworkIdentityComponentTest.cpp
		Networking/SnapshotBufferTest.cpp
		Networking/DeltaSnapshotTest.cpp
		Networking/SnapshotInterpolatorTest.cpp
		Networking/InputBufferTest.cpp
		Networking/PredictionReconciliationTest.cpp
		Networking/AuthorityRejectionTest.cpp
		Networking/LagCompensatorTest.cpp
		Networking/InterestManagerTest.cpp
		Networking/RpcMarshallingTest.cpp
		Networking/EntityLifecycleTest.cpp
		Networking/NetworkScriptBindingCoverageTest.cpp
		Networking/LockstepTest.cpp
		Networking/P2PRollbackTest.cpp
		Networking/SessionLobbyTest.cpp
		# MMO Networking Tests (Phases 8-14)
		Networking/SpatialGridTest.cpp
		Networking/ZoneServerTest.cpp
		Networking/ZoneHandoffTest.cpp
		Networking/InstanceLayerTest.cpp
		Networking/ChatTest.cpp
		Networking/PersistenceTest.cpp
		Networking/MMOOptimizationTest.cpp
		# Task / Scheduler config tests
		Task/SchedulerConfigTest.cpp
		Task/LocalQueueTest.cpp
		# Localization tests
		LocalizationTest.cpp
		# Dialogue system tests
		DialogueVariablesTest.cpp
		DialogueTreeSerializationTest.cpp
		DialogueTreeFloatValidationTest.cpp
		DialogueSystemTest.cpp
		# Shader Graph Tests
		ShaderGraph/ShaderGraphTest.cpp
		ShaderGraph/ShaderGraphCompilerTest.cpp
		ShaderGraph/ShaderGraphSerializationTest.cpp
		ShaderGraph/ShaderGraphCommandTest.cpp
		# Serialization Tests
		Serialization/ArchiveExtensionsTest.cpp
		Serialization/StreamHelpersTest.cpp
		Serialization/MeshBinarySerializerTest.cpp
		Serialization/SceneBinarySidecarTest.cpp
		Serialization/MeshAssetSerializerTest.cpp
		Serialization/SoundGraphSerializerTest.cpp
		Serialization/VisualScriptSerializerTest.cpp
		Scripting/VisualScriptVMTest.cpp
		Scripting/VisualScriptNodeLibraryTest.cpp
		Serialization/SoundConfigSerializerTest.cpp
		Serialization/EnvironmentSerializerTest.cpp
		Serialization/FontAssetPackSerializerTest.cpp
		Serialization/NestedStructSerializerCodegenTest.cpp
		Serialization/ContainerSerializerCodegenTest.cpp
		Serialization/Vec3ClampSerializerCodegenTest.cpp
		# SaveGame Tests
		SaveGame/SaveGameFileTest.cpp
		SaveGame/SaveGameManagerTest.cpp
		SaveGame/SaveGameIntegrationTest.cpp
		SaveGame/SaveGameComponentSerializerCoverageTest.cpp
		SaveGame/SaveGameVersionMigrationTest.cpp
		SaveGame/BoidComponentSaveLoadSanitizeTest.cpp
		SaveGame/DestructibleComponentSaveLoadSanitizeTest.cpp
		SaveGame/SaveFileWorldDatabaseTest.cpp
		# Navigation Tests
		NavMeshTest.cpp
		HeadlessTest.cpp
		# Animation State Machine & Blend Tree Tests
		AnimationStateMachineTest.cpp
		AnimationSystemTest.cpp
		BlendTreeTest.cpp
		Animation/AimIKSolverTest.cpp
		Animation/FABRIKSolverTest.cpp
		Animation/LimbIKSolverTest.cpp
		Animation/BlendUtilsTest.cpp
		Animation/OneShotBlendTest.cpp
		Animation/SpringBoneSolverTest.cpp
		Animation/NoiseSolverTest.cpp
		Animation/AnimationRetargetTest.cpp
		Animation/AnimationGraphBoneMappingTest.cpp
		Animation/RootMotionTest.cpp
		Animation/FootIKTest.cpp
		# Cinematic Sequencer Tests
		Cinematic/CinematicCurveTest.cpp
		Cinematic/CinematicPlayerTest.cpp
		Cinematic/CinematicSerializerTest.cpp
		Cinematic/CinematicEditTest.cpp
		# Procedural Terrain Generation Tests
		Terrain/DriftIslandSplatCoverageTest.cpp
		Terrain/TerrainGeneratorTest.cpp
		Terrain/TerrainVirtualTextureTest.cpp
		Terrain/TerrainAuthoringReadbackTest.cpp
		Terrain/VoxelGreedyMesherTest.cpp
		Terrain/VoxelEditTest.cpp
		Terrain/VoxelEditRemeshTest.cpp
		Terrain/VolumetricChunkWindowTest.cpp
		# Morph Target Tests
		MorphTargetTest.cpp
		# AI Behavior Tree & FSM Tests
		BehaviorTreeTest.cpp
		StateMachineTest.cpp
		# AI GOAP (Goal-Oriented Action Planning) Tests
		AI/GoapTest.cpp
		# AI Sight-Perception Tests
		AI/PerceptionMathTest.cpp
		# AI flocking / boids spatial hash (#731)
		AI/FlockSpatialHashTest.cpp
		# Scene spatial acceleration structure (#430)
		SpatialAccelerationTest.cpp
		# Inventory Tests
		InventoryTest.cpp
		ItemDatabaseFloatValidationTest.cpp
		# Quest Tests
		QuestSystemTest.cpp
		QuestDatabaseFloatValidationTest.cpp
		# Gameplay event bus
		GameplayEventBusTest.cpp
		# Auto-Save Tests
		AutoSaveTest.cpp
		# Quality Tiering Tests
		QualityTieringTest.cpp
		# PerformanceProfiler Tests
		PerformanceProfilerTest.cpp
		# TransformComponent Tests
		TransformComponentTest.cpp
		Scene/WorldTransformPropagationBenchmarkTest.cpp
		# System scheduler dependency-graph derivation (issue #453)
		Scene/SceneLightmapStalenessTest.cpp
		Scene/SystemSchedulerTest.cpp
		# Floating-origin / origin-rebasing math (issue #429)
		Scene/WorldOriginRebaseTest.cpp
		# Runtime scene load/switch resolution + loading (issue #642)
		Scene/SceneTransitionTest.cpp
		# Shipped-game project mount + asset-relative script path contract (issue #642)
		RuntimeProjectMountTest.cpp
		# Build target platform contract: naming, host-only support, fail-loud gate (issue #891)
		Build/GameBuildPipelineTest.cpp
		# Floating-origin rebase cost characterization over N entities (issue #613)
		Scene/WorldOriginRebaseBenchmarkTest.cpp
		# Physics3D mesh mass-properties (divergence-theorem volume + centroid)
		JoltShapesMeshMassPropertiesTest.cpp
		# Physics3D cloth soft-body shared-settings contract (issue #460)
		ClothSharedSettingsTest.cpp
		# Physics3D mesh collider cache invalidation
		MeshCookingFactoryCacheTest.cpp
		# StaticMesh GenerateColliders flag -> generated MeshColliderAsset (cook + raycast)
		StaticMeshColliderGenerationTest.cpp
		# CrashReporter / platform handlers (Windows SEH + Linux signal hooks)
		Debug/CrashReporterTest.cpp
		# RendererProfiler frame-time/CPU/GPU-wait alignment (#519)
		Debug/RendererProfilerTest.cpp
		# Gameplay Ability Tests
		Gameplay/GameplayAbilityTest.cpp
		# RPG progression tests (issue #635): experience curve / skill tree /
		# character class assets + serializers, the ProgressionSystem tick
		# semantics, and the ActiveEffectsContainer cleanup overloads that make
		# infinite skill passives revocable.
		Gameplay/ExperienceCurveTest.cpp
		Gameplay/SkillTreeDatabaseTest.cpp
		Gameplay/CharacterClassDatabaseTest.cpp
		Gameplay/ActiveEffectsCleanupTest.cpp
		Gameplay/ProgressionSystemTest.cpp
		Gameplay/PlayerRigTest.cpp
		Gameplay/WeaponStateTest.cpp
		# Content Browser Tests
		ContentBrowser/DirectoryTreeTest.cpp
		ContentBrowser/ContentBrowserItemTest.cpp
		${CMAKE_SOURCE_DIR}/OloEditor/src/Panels/ContentBrowser/DirectoryTree.cpp
		${CMAKE_SOURCE_DIR}/OloEditor/src/Panels/ContentBrowser/ContentBrowserItem.cpp
		# MCP JSON-RPC dispatch / framing tests (issue #306 item D). The dispatch
		# core lives in the OloEditor target, so pull its single httplib-free-at-
		# the-seam translation unit into the test binary (cpp_httplib is linked
		# below). McpServerPanel.* (ImGui) is NOT needed.
		MCP/McpDispatchTest.cpp
		${CMAKE_SOURCE_DIR}/OloEditor/src/MCP/McpServer.cpp
		# The outbound stdio MCP client (#673 Tier 1): McpServer's connection-
		# ownership members + the McpClientConnection protocol layer live here.
		# The Win32 pipe transport is compiled but never spawned by unit tests —
		# they inject in-memory fake transports (McpClientStdioTest.cpp).
		${CMAKE_SOURCE_DIR}/OloEditor/src/MCP/McpClient.cpp
		# Headless-attach stretch (issue #316): McpHeadlessAttachTest hosts the
		# REAL tool surface (RegisterBuiltinTools) against the offscreen FB, so it
		# needs the actual tool TUs too. These are editor-decoupled — they include
		# only OloEngine headers + the header-only MCP/* helpers — so they compile
		# cleanly into the test binary (which links OloEngine). McpScriptApi.cpp
		# supplies BuildScriptApiDigest, referenced by McpToolsScripting.cpp.
		# McpTools.cpp is the composition point; the per-domain TUs carry the
		# actual handlers + registrations (issue #357 split of the monolith).
		${CMAKE_SOURCE_DIR}/OloEditor/src/MCP/McpTools.cpp
		${CMAKE_SOURCE_DIR}/OloEditor/src/MCP/McpToolsAssets.cpp
		${CMAKE_SOURCE_DIR}/OloEditor/src/MCP/McpToolsBenchmark.cpp
		${CMAKE_SOURCE_DIR}/OloEditor/src/MCP/McpToolsCamera.cpp
		${CMAKE_SOURCE_DIR}/OloEditor/src/MCP/McpToolsDiagnostics.cpp
		${CMAKE_SOURCE_DIR}/OloEditor/src/MCP/McpToolsEditor.cpp
		${CMAKE_SOURCE_DIR}/OloEditor/src/MCP/McpToolsInput.cpp
		${CMAKE_SOURCE_DIR}/OloEditor/src/MCP/McpToolsPerf.cpp
		${CMAKE_SOURCE_DIR}/OloEditor/src/MCP/McpToolsPhysics.cpp
		${CMAKE_SOURCE_DIR}/OloEditor/src/MCP/McpToolsRender.cpp
		${CMAKE_SOURCE_DIR}/OloEditor/src/MCP/McpToolsScene.cpp
		# The generated writable-field registry TU (McpGenericFieldWrite.h's
		# Registry() definition). Compiled once here instead of inline in every
		# includer — the inline form OOM'd clang-cl Release+ASan on this very
		# target's McpToolsScene.cpp.obj once the registry crossed ~100 components.
		# Conditionally blanked above (issue #822) so it can join the heavy-compile
		# file set instead when the pool is available.
		${_olo_mcp_field_registry_src}
		${CMAKE_SOURCE_DIR}/OloEditor/src/MCP/McpToolsScripting.cpp
		${CMAKE_SOURCE_DIR}/OloEditor/src/MCP/McpToolsShader.cpp
		${CMAKE_SOURCE_DIR}/OloEditor/src/MCP/McpScriptApi.cpp
		# Consented MCP synthetic-input WRITE tool: olo_input_inject (#607) — click /
		# drag / key / text into the running editor so an agent can verify an
		# interactive handler actually FIRES. Same dispatch seam (session write gate +
		# inputSchema enforcement at McpServer.cpp) plus the pure core
		# (MCP/McpInputInject.h: key-name mapping, coordinate-space math, and the
		# frame-quantized plan whose timing decides whether an injected click is seen
		# at all). The injection ACTION is a context hook (ImGui/GLFW in the editor),
		# so the test injects a fake and never opens a window. Header-only on the
		# editor side, so no extra editor TU is pulled in.
		MCP/McpInputInjectTest.cpp
		# The editor-liveness predicate every capture/injection tool now consults
		# (#607): a MINIMIZED editor parks its update loop, so injection never drains
		# and captures silently go stale — with HTTP 200 and no clue, because
		# MarshalRead keeps working. Header-only (MCP/McpEditorLiveness.h).
		MCP/McpEditorLivenessTest.cpp
		# Pure JSON / Mermaid / DOT shaping behind olo_scheduler_graph (#607): the
		# gameplay SystemScheduler's DERIVED dependency graph, including which
		# Parallelizable systems may actually overlap (a missing edge is invisible in
		# the sequential order and only becomes a race under the parallel executor).
		# Header-only (MCP/McpSchedulerGraph.h), no extra editor TU pulled in.
		MCP/McpSchedulerGraphTest.cpp
		# Pure reasoning behind olo_physics_why_no_collision (#306 item A). Header-
		# only (MCP/McpPhysicsExplain.h), so no extra editor TU is pulled in.
		MCP/McpPhysicsExplainTest.cpp
		# Pure reasoning behind olo_render_why_not_visible (#306 item A, rendering
		# half). Header-only (MCP/McpRenderExplain.h), no extra editor TU pulled in.
		MCP/McpRenderExplainTest.cpp
		# Pure image-diff core behind olo_render_compare_golden (#316 Part 4).
		# Header-only (MCP/McpGoldenCompare.h), no extra editor TU pulled in.
		MCP/McpGoldenCompareTest.cpp
		# Pure per-command / per-stage frame-breakdown shaping behind
		# olo_render_frame_breakdown (#306 item D, bullet 2). Header-only
		# (MCP/McpFrameBreakdown.h), no extra editor TU pulled in.
		MCP/McpFrameBreakdownTest.cpp
		# Pure per-pass GPU/CPU timings shaping behind olo_perf_pass_timings
		# (#316: split whole-frame GPU time by render-graph pass). Header-only
		# (MCP/McpPassTimings.h), no extra editor TU pulled in.
		MCP/McpPassTimingsTest.cpp
		# Pure per-scope CPU timings shaping behind olo_perf_cpu_scopes (#519,
		# first slice: expose PerformanceProfiler's per-scope CPU timings, incl.
		# the Distribution-build "unavailable" degradation). Header-only
		# (MCP/McpCpuScopes.h), no extra editor TU pulled in.
		MCP/McpCpuScopesTest.cpp
		# Pure JSON / Mermaid topology shaping behind olo_render_graph_topology_export
		# (#316 Part 4, LLM-analysis exports). Header-only (MCP/McpRenderGraphTopology.h),
		# no extra editor TU pulled in.
		MCP/McpRenderGraphTopologyTest.cpp
		# Pure octahedral-normal decode / depth linearization / per-channel shaping
		# behind olo_render_probe_pixel (#607: the per-pixel numeric G-Buffer readout),
		# plus the space:"viewport"/"texel" coordinate mapping + mappedCoord echo
		# (#607 GTAO-hunt slice). Header-only (MCP/McpRenderProbePixel.h), no extra
		# editor TU pulled in.
		MCP/McpRenderProbePixelTest.cpp
		# Pure bit-exact unique-value histogram / finite min-max-mean math behind
		# olo_render_target_stats (#607: 1-ULP corruption an 8-bit PNG hides).
		# Header-only (MCP/McpRenderTargetStats.h), no extra editor TU pulled in.
		MCP/McpRenderTargetStatsTest.cpp
		# Pure version-group / consumed-but-unbacked shaping + the bit-exact
		# float-buffer compare behind olo_render_validate (#607). Header-only
		# (MCP/McpRenderValidate.h), no extra editor TU pulled in.
		MCP/McpRenderValidateTest.cpp
		# Pure light-grid aggregation (per-slice / per-cluster histogram, overflow +
		# empty cluster counts) behind olo_cluster_grid_stats (#607). Header-only
		# (MCP/McpClusterGridStats.h), no extra editor TU pulled in.
		MCP/McpClusterGridStatsTest.cpp
		# Pure availability/freshness + JSON shaping behind the VSM, classic-mesh
		# LOD and DDGI probe-stat diagnostics (#607). Header-only MCP cores; no
		# renderer or editor TU is pulled into these tests.
		MCP/McpVirtualShadowMapStatsTest.cpp
		MCP/McpRenderLODStatsTest.cpp
		MCP/McpDDGIProbeStatsTest.cpp
		# Pure froxel <-> world mapping (the shaders' exponential z-slice
		# distribution + view-ray secant scaling) behind olo_froxel_fog_probe (#607,
		# relates to #435). Header-only (MCP/McpFroxelFogProbe.h), no extra editor TU.
		MCP/McpFroxelFogProbeTest.cpp
		# Pure JSON shaping behind olo_gpu_readback_stats (#721) — in particular
		# that a raised overflow flag reaches the top of the response instead of
		# being derivable only by reading the counters. Header-only
		# (MCP/McpGpuReadbackStats.h), no extra editor TU pulled in.
		MCP/McpGpuReadbackStatsTest.cpp
		# Pure array-layer selection behind olo_render_capture_target's 'layer'
		# argument (#607): the CSM cascade array / shadow-atlas raw-depth views, and
		# the "a layer view must not silently capture layer 0" rule. Header-only
		# (MCP/McpShadowCapture.h), no extra editor TU pulled in.
		MCP/McpShadowCaptureLayerTest.cpp
		# Pure parse / reject / meta shaping behind the shared 'region' {x,y,w,h}
		# argument on olo_render_capture_target + olo_screenshot (#607: native-
		# resolution capture, so a pixel-scale artifact can be measured on 1:1
		# pixels instead of a maxWidth-resampled image). Header-only
		# (MCP/McpCaptureRegion.h), no extra editor TU pulled in.
		MCP/McpCaptureRegionTest.cpp
		# The post-process / AO / fog field table + coercion / clamp / enum-token
		# core behind olo_postprocess_settings_get / _set (#607: the GTAO-vs-SSAO
		# A/B that could not be driven from an agent session at all). Header-only
		# (MCP/McpPostProcessSettings.h), no extra editor TU pulled in.
		MCP/McpPostProcessSettingsTest.cpp
		# Process-global accessibility settings schema / strict parsing / engine
		# sanitization behind olo_accessibility_get / _set (#607). Header-only
		# editor seam; no McpTools or EditorLayer TU is pulled in.
		MCP/McpAccessibilityTest.cpp
		# Pure request validation and pending/hit/miss/overflow result shaping
		# behind olo_terrain_pick (#607); editor camera and GPU dispatch stay out.
		MCP/McpTerrainPickTest.cpp
		# Unified diagnostics event ring buffer behind olo_events_tail (#306 item B).
		# Header-only engine facility (Debug/DiagnosticsEventLog.h, compiled into
		# OloEngine), so no extra editor TU is pulled in.
		MCP/McpEventsTailTest.cpp
		# Event -> MCP-notification -> SSE-frame serialization behind the GET /mcp
		# server-push stream (#306 item B). Header-only (MCP/McpEventStream.h,
		# DiagnosticsEventLog + nlohmann::json only), so no extra editor TU is pulled in.
		MCP/McpEventStreamTest.cpp
		# Pure result-shaping behind olo_shader_reload (#316 Part 4). Header-only
		# (MCP/McpShaderReload.h, depends only on the engine's ShaderCompilationStatus
		# enum), so no extra editor TU is pulled in.
		MCP/McpShaderReloadTest.cpp
		# Pure name tables + result-shaping behind olo_render_toggle_pass /
		# olo_render_set_debug_view (#316 Part 4). Header-only (MCP/McpRenderOverrides.h,
		# nlohmann::json + stdlib only), so no extra editor TU is pulled in.
		MCP/McpRenderOverridesTest.cpp
		# Tool behavioural annotations + title serialization and tool-name validation
		# (#357 Tier 1). Exercises HandleToolsList / RegisterTool / IsValidToolName in
		# the McpServer.cpp TU already pulled in above; no extra editor TU is needed.
		MCP/McpToolAnnotationsTest.cpp
		# Structured/typed tool output: outputSchema in tools/list, structuredContent
		# in tools/call, and redaction of the typed result (#357 Tier 2 P2). Exercises
		# HandleToolsList / HandleToolsCall / ToolResult::Structured in the McpServer.cpp
		# TU already pulled in above; no extra editor TU is needed.
		MCP/McpStructuredOutputTest.cpp
		# Resource links + the dynamic (copy-on-write) resource registry (#673
		# Tier 1): the binary `blob` contents variant of resources/read, ephemeral
		# capture publish/eviction + notifications/resources/list_changed, the
		# held-snapshot-survives-eviction property, and the resource_link content
		# block incl. redaction. Exercises the McpServer.cpp TU already pulled in
		# above; no extra editor TU is needed.
		MCP/McpResourceLinkTest.cpp
		# OutputSchema adoption guard over the REAL builtin tool surface (#673
		# Tier 1 sweep): registration-only (no tools/call, so no handler runs and
		# no game thread / GL is needed) — asserts every declared outputSchema is
		# a well-formed open object and pins the set of schema-declaring tools.
		# Uses the McpTools*.cpp TUs already pulled in above.
		MCP/McpOutputSchemaCoverageTest.cpp
		# Audience-tagged content blocks (#673 Tier 2): the ToolResult::AnnotateBlock /
		# StructuredDualAudience mechanism + the ToolDef::DualAudienceContent
		# re-shaping in HandleToolsCall (fake tools over the dispatch seam), the pure
		# Markdown renderer (MCP/McpAudienceReport.h), and the adoption ratchet over
		# the REAL builtin surface — registration-only like the outputSchema guard
		# above, so no handler runs and no game thread / GL is needed.
		MCP/McpAudienceBlocksTest.cpp
		# JSON-Schema builder DSL + byte-identical equivalence with the migrated tool
		# schema literals (#357 Tier 2 P4). Header-only (MCP/McpSchemaBuilder.h,
		# nlohmann::json + stdlib only), so no extra editor TU is pulled in.
		MCP/McpSchemaBuilderTest.cpp
		# Server-side inputSchema enforcement for tools/call: McpServer::ValidateArguments
		# (the pure validator) + HandleToolsCall rejecting malformed calls with a
		# field-naming kInvalidParams (#357 conformance / #306 item D hardening).
		# Exercises the McpServer.cpp TU already pulled in above plus the header-only
		# schema-builder DSL; no extra editor TU is needed.
		MCP/McpInputValidationTest.cpp
		# Progress notifications + request cancellation (#357 item B): the
		# transport-agnostic notification sink on ProcessRequestBody, the in-flight
		# cancellation registry behind notifications/cancelled, and the SSE-upgraded
		# POST response over a real localhost socket (httplib client + server).
		# Exercises the McpServer.cpp TU already pulled in above.
		MCP/McpProgressCancelTest.cpp
		# Script-defined MCP tools, Lua v1 (#357; ADR 0005): sandboxed registration
		# from a temp directory of .lua files, schema-enforced dispatch, sandbox
		# escape containment, the watchdog, and the read-only olo.call_tool bridge.
		# Pulls the editor-decoupled McpScriptTools.cpp TU (sol2 + engine only).
		${CMAKE_SOURCE_DIR}/OloEditor/src/MCP/McpScriptTools.cpp
		MCP/McpScriptToolsTest.cpp
		# The registry-merge trust seam for tools bridged from an OUTBOUND MCP
		# client connection (#673 Tier 1): ReplaceClientTools validate-and-reject,
		# the reserved ext.<alias>.* namespace, the forced authority posture
		# (ProjectWrite + open-world annotations), per-alias replace semantics
		# coexisting with native/script tools, and External-marked consent
		# prompts. Exercises the McpServer.cpp TU already pulled in above.
		MCP/McpClientToolsTest.cpp
		# The outbound stdio client's protocol + lifecycle layer (#673 Tier 1):
		# handshake, bridged round-trip, timeouts, child death, disconnect, and
		# the polite -32601 to child-initiated requests — all through in-memory
		# fake transports (McpClient.cpp TU pulled in above; no process spawn).
		MCP/McpClientStdioTest.cpp
		# MCP protocol surface from #607: tool + serverInfo `icons` (SEP-973), the
		# now-true capabilities.tools.listChanged, and the copy-on-write tool-registry
		# snapshot the live script-tool reload rests on. Exercises the McpServer.cpp TU
		# already pulled in above; no extra editor TU.
		MCP/McpProtocolIconsTest.cpp
		# First consented, undoable MCP WRITE tool: olo_set_collision_layer (#306
		# item C, first slice). Exercises the session write gate + inputSchema
		# enforcement at the McpServer.cpp dispatch seam, and the shared apply core
		# (MCP/McpSetCollisionLayer.h). Header-only on the editor side (the schema
		# DSL + the header-only UndoRedo CommandHistory), so no extra editor TU is
		# pulled in — it links the engine Scene/ECS already in the test binary.
		MCP/McpConsentedWriteTest.cpp
		MCP/McpDocsCoverageTest.cpp
		# Generic consented, undoable MCP WRITE tool: olo_entity_set_field + its read
		# companion olo_entity_list_fields (#306 item C, second slice). Same seams as
		# McpConsentedWriteTest.cpp — the session write gate + inputSchema enforcement
		# at the McpServer.cpp dispatch seam, and the shared reflect+coerce+apply core
		# (MCP/McpGenericFieldWrite.h; its Registry() links from the
		# McpFieldRegistry.cpp TU pulled in above).
		MCP/McpGenericFieldWriteTest.cpp
		# The GENERATED writable-field registry behind those two tools (#607):
		# OloHeaderTool emits MCP/Generated/McpFieldRegistry.Generated.inl from the same
		# component data-member scan that drives the scene serializer, including the
		# sub-object descent that makes a field nested inside a non-POD member
		# ("System.Emitter.RateOverTime") addressable. Guards breadth, the runtime-only
		# exclusions, and that a nested field inherits its serializer clamp.
		# NOTE: this file existed but was never listed here, so none of it ran.
		MCP/McpFieldRegistryTest.cpp
		# Consented MCP WRITE tool: olo_reload_script (#306 item C) — reload the C#
		# script assembly. Same dispatch seam (session write gate + inputSchema
		# enforcement at McpServer.cpp) and shared shaping (MCP/McpReloadScript.h:
		# schema + McpScriptReloadResult->JSON). The reload ACTION is a context hook
		# (ScriptEngine/Mono in the editor), so the test injects a fake hook and never
		# touches Mono. Header-only on the editor side, so no extra editor TU.
		MCP/McpReloadScriptTest.cpp
		# Consented MCP WRITE tool: olo_renderer_settings_set (#306 item C) — set a
		# multi-valued, session-global renderer / post-process setting (FSR1 upscale
		# mode, tone-map operator, rendering path). Same dispatch seam (session write
		# gate + inputSchema enforcement at McpServer.cpp) and shared core
		# (MCP/McpRendererSettings.h: schema + parse + apply against the POD settings
		# structs, restore-prior-value not CommandHistory). Renderer-light header, so
		# no extra editor TU is pulled in.
		MCP/McpRendererSettingsTest.cpp
		# Consented MCP scene-control WRITE tools: olo_scene_open / olo_scene_play /
		# olo_scene_stop (#316 Part 5) — the scriptable scene switch + play-mode toggle.
		# Same dispatch seam (session write gate + inputSchema enforcement at
		# McpServer.cpp) and shared shaping/validation (MCP/McpSceneControl.h: schemas,
		# ValidateScenePath, result->JSON). The scene ACTIONS are context hooks (the
		# EnTT registry / runtime in the editor), so the test injects fakes and never
		# touches a scene. Header-only on the editor side, so no extra editor TU.
		MCP/McpSceneControlTest.cpp
		# Shared schema/parser/result contract for olo_reflection_probe_bake.
		MCP/McpReflectionProbeBakeTest.cpp
		# Pure request/status/result contract for async or blocking lightmap bakes.
		MCP/McpLightmapBakeTest.cpp
		# Editor panel visibility and clean viewport-overlay controls (#607).
		MCP/McpEditorPanelsTest.cpp
		MCP/McpEditorDebugDrawTest.cpp
		# Consented MCP WRITE tool: olo_editor_select_entity (#607) — select/clear
		# the Scene Hierarchy panel's selection so the Properties inspector draws a
		# given entity's components (unblocks screenshot verification of the whole
		# DrawComponent<T> surface; olo_input_inject can't reliably land a Scene
		# Hierarchy row click). Same dispatch seam (session write gate + inputSchema
		# enforcement at McpServer.cpp) and shared shaping/validation
		# (MCP/McpSelectEntity.h: schema, entity-XOR-clear ParseArgs, result->JSON).
		# The selection ACTION is a context hook (SceneHierarchyPanel in the
		# editor), so the test injects a fake and never opens a window. Header-only
		# on the editor side, so no extra editor TU is pulled in.
		MCP/McpSelectEntityTest.cpp
		# Lua Binding Tests
		Lua/LuaBindingTest.cpp
		# Steamworks platform integration (#644). All five run on EVERY configuration:
		# the manager/binding/routing tests drive a fake backend and need no Steam client
		# at all, and the stub-SDK ON-path test SKIPs cleanly unless the build was
		# configured with -DOLO_WITH_STEAM_STUB_SDK=ON.
		Platform/Steam/SteamManagerTest.cpp
		Platform/Steam/SteamScriptBindingCoverageTest.cpp
		Platform/Steam/SteamStubOnPathTest.cpp
		Platform/Steam/SteamCloudSaveMirrorTest.cpp
		Platform/Steam/SteamInputRoutingTest.cpp
		# Functional / cross-subsystem world-tick tests (see ADR 0001).
		Functional/FunctionalTest.cpp
		Functional/UI/UINavigationTest.cpp
		Functional/Cinematic/CinematicDrivesEntitiesTest.cpp
		Functional/Cinematic/CinematicAssetPlaybackTest.cpp
		Functional/AnimationPhysics/FreeFallLandingTest.cpp
		Functional/AnimationPhysics/TerrainCollisionTest.cpp
		Functional/AnimationPhysics/TerrainStreamingSculptCollisionTest.cpp
		Functional/AnimationPhysics/ClothSimulationTest.cpp
		Functional/AnimationPhysics/ClothSkeletonAttachTest.cpp
		Functional/AnimationPhysics/PhysicsJoint3DTest.cpp
		Functional/AnimationPhysics/PhysicsJointCollideConnectedTest.cpp
		Functional/AnimationPhysics/VehicleTest.cpp
		Functional/AnimationPhysics/VehicleDifferentialTest.cpp
		Functional/AnimationPhysics/BoatTest.cpp
		Functional/AnimationPhysics/SailTest.cpp
		Functional/AnimationPhysics/AircraftTest.cpp
		Functional/AnimationPhysics/RagdollTest.cpp
		Functional/AnimationPhysics/WaterBuoyancyTest.cpp
		Functional/AnimationPhysics/WaterWakeBuoyancyTest.cpp
		Functional/AnimationPhysics/FluidCouplingTest.cpp
		Functional/AnimationPhysics/AnimationKeepsTickingUnderPhysicsLoadTest.cpp
		Functional/SaveGame/SceneRoundTripAfterTickTest.cpp
		Functional/Networking/PhysicsTransformReplicationTest.cpp
		Functional/Networking/ServerAuthoritativeLoopTest.cpp
		Functional/Networking/InterpolatedComponentsSmoothOnRemoteClientTest.cpp
		Functional/Scene/ScenePauseFreezesAllSubsystemsTest.cpp
		Functional/AnimationPhysics/EntityDestroyedMidTickTest.cpp
		Functional/AnimationPhysics/PhysicsLayerFilteringTest.cpp
		Functional/AnimationPhysics/EntityAddedAtRuntimeTest.cpp
		Functional/Scene/PhysicsRestartIsCleanTest.cpp
		Functional/Scene/WorldOriginRebasePhysicsTest.cpp
		Functional/Scene/MultipleScenesCoexistTest.cpp
		Functional/Scene/RuntimeSceneSwitchTest.cpp
		Functional/Scene/HierarchyChildFollowsPhysicsParentTest.cpp
		Functional/Scene/WorldTransformPropagationTest.cpp
		Functional/AI/BehaviorTreeAdvancesViaSceneTickTest.cpp
		Functional/AI/GoapAgentPlansViaSceneTickTest.cpp
		Functional/AI/GoapAuthoredFromLuaViaSceneTickTest.cpp
		Functional/AI/PerceptionDetectsTargetViaSceneTickTest.cpp
		Functional/AI/PerceptionLineOfSightBlockedByWallViaSceneTickTest.cpp
		Functional/Particles/ParticleSystemEmitsAndExpiresTest.cpp
		Functional/Particles/ParticleSystemDeterminismTest.cpp
		Functional/AnimationPhysics/CharacterControllerWalksTest.cpp
		Functional/AnimationPhysics/AnimationBlendCompletesViaSceneTickTest.cpp
		Functional/AnimationPhysics/SpringBoneJiggleViaSceneTickTest.cpp
		Functional/AnimationPhysics/NoiseAnimatorViaSceneTickTest.cpp
		Functional/Networking/SnapshotApplyAcrossScenesTest.cpp
		Functional/AnimationPhysics/Physics2DSimulatesViaSceneTickTest.cpp
		Functional/AnimationPhysics/CharacterControllerContactCallbackTest.cpp
		Functional/AnimationPhysics/ComponentRemovedAtRuntimeTest.cpp
		Functional/AnimationPhysics/TriggerVolumeFiresOverlapEventTest.cpp
		Functional/Scene/SceneStepAdvancesOneFrameTest.cpp
		Functional/Scene/DeterministicReplayProducesSameStateTest.cpp
		Functional/Scene/FixedTimestepDeterminismTest.cpp
		Functional/Scene/RenderInterpolationSmoothsMotionTest.cpp
		Functional/Scene/PrimaryCameraResolutionTest.cpp
		Functional/Scene/EntityCreateWithUUIDTest.cpp
		Functional/AnimationPhysics/AnimationContinuesAfterPauseResumeTest.cpp
		Functional/AnimationPhysics/CharacterPushesDynamicBodyTest.cpp
		Functional/SaveGame/Save2DBodyVelocityRoundTripTest.cpp
		Functional/AnimationPhysics/TriggerEndEventFiresOnSeparationTest.cpp
		Functional/Gameplay/InventoryAutoPickupOnProximityTest.cpp
		Functional/Gameplay/InventoryEventsEmittedTest.cpp
		Functional/Gameplay/QuestEventsEmittedTest.cpp
		Functional/Scene/EntitySetParentCycleRejectionTest.cpp
		Functional/Scene/DuplicateTagFindableTest.cpp
		Functional/SaveGame/NetworkIdentitySurvivesSaveLoadTest.cpp
		Functional/SaveGame/RegisteredComponentsSurviveSaveLoadTest.cpp
		Functional/AnimationPhysics/CharacterControllerJumpsTest.cpp
		Functional/Scripting/LuaScriptMutatesTransformViaSceneTickTest.cpp
		Functional/Scripting/LuaScriptOnDestroyFiresTest.cpp
		Functional/AnimationPhysics/RigidbodyDisableGravityFlagTest.cpp
		Functional/AnimationPhysics/InitialLinearVelocityIsAppliedTest.cpp
		Functional/Audio/AudioListenerPositionUpdatesViaSceneTickTest.cpp
		Functional/AnimationPhysics/AnimationGraphStateMachineTickTest.cpp
		Functional/AnimationPhysics/LiveRetargetingTest.cpp
		Functional/AnimationPhysics/LocomotionControllerTest.cpp
		Functional/AnimationPhysics/HumanoidLocomotionWalkTest.cpp
		Functional/AnimationPhysics/MorphTargetGraphAnimationTest.cpp
		Functional/Navigation/NavAgentReachesTargetTest.cpp
		Functional/Asset/AssetHotReloadDoesNotDeadlockTest.cpp
		Functional/Asset/AssetManagerImportsStagedAssetTest.cpp
		Functional/Gameplay/TimedQuestFailsAfterDeadlineTest.cpp
		Functional/Gameplay/AbilityCooldownTicksDownViaSceneTickTest.cpp
		Functional/Atmosphere/WeatherTimeOfDayDirectorTest.cpp
		Functional/Atmosphere/DriftLegWeatherAndSeaStateTest.cpp
		Functional/Scripting/LuaScriptSetsRigidbody2DVelocityTest.cpp
		Functional/SaveGame/AbilityComponentRoundTripTest.cpp
		Functional/Dialogue/DialogueTextRevealsOverTicksTest.cpp
		Functional/Gameplay/DamageKillsEntityAndFlipsDeathTagTest.cpp
		Functional/Gameplay/WeaponImpactUsesGameplayDamageTest.cpp
		Functional/Gameplay/DestructibleDebrisTest.cpp
		Functional/Gameplay/QuestObjectiveCompletionAdvancesStageTest.cpp
		Functional/Gameplay/PeriodicDamageEffectTicksHealthDownTest.cpp
		Functional/AI/StateMachineTransitionsViaSceneTickTest.cpp
		Functional/Scripting/LuaSetsAbilityAttributeViaSceneTickTest.cpp
		Functional/Scripting/LuaDrivesTerrainRegenerationTest.cpp
		Functional/SaveGame/QuestJournalSceneYAMLRoundTripTest.cpp
		Functional/Gameplay/GameplayEffectExpiresAndRevertsAttributeTest.cpp
		Functional/Gameplay/AbilityBlockedByOwnerTagTest.cpp
		Functional/Gameplay/InventoryStackConsolidationTest.cpp
		Functional/Navigation/NavMeshQueryFindPathBetweenPointsTest.cpp
		Functional/Navigation/NavMeshOffMeshLinkBridgesGapTest.cpp
		Functional/Navigation/NavAgentUnreachableTargetTerminatesTest.cpp
		Functional/Navigation/NavMeshGeneratorBoundsValidationTest.cpp
		Functional/Navigation/CrowdAvoidancePreventsOverlapTest.cpp
		Functional/Navigation/WorldOriginRebaseNavTest.cpp
		Functional/Scripting/LuaRaycastHitsPhysicsBodyTest.cpp
		Functional/Scripting/LuaCompletesQuestViaIncrementObjectiveTest.cpp
		Functional/Dialogue/DialogueAdvanceMovesToNextNodeTest.cpp
		Functional/Dialogue/DialogueAdvanceGuardsRemovedComponentTest.cpp
		Functional/Gameplay/InventoryTransferItemBetweenContainersTest.cpp
		Functional/Gameplay/InventoryShopTradingTest.cpp
		Functional/Gameplay/ChanneledAbilityAutoDeactivatesAtEndOfDurationTest.cpp
		Functional/Gameplay/MultiStageQuestAdvancesThroughStagesTest.cpp
		Functional/AnimationPhysics/AnimationGraphMultipleEntitiesAdvanceIndependentlyTest.cpp
		Functional/Gameplay/ToggledAbilityReactivateCancelsTest.cpp
		Functional/Gameplay/PeriodicEffectStackingScalesDamageTest.cpp
		Functional/Gameplay/AbilityActivationEffectAppliesToCasterTest.cpp
		Functional/Gameplay/AbilityRequiredTagGatesActivationTest.cpp
		Functional/AnimationPhysics/TwoCharacterControllersCoexistTest.cpp
		Functional/AnimationPhysics/ContactConstraintLimitsHonouredTest.cpp
		Functional/Gameplay/AbilityResourceCostDeductedFromManaTest.cpp
		Functional/Dialogue/DialogueSelectChoiceBranchesToTargetTest.cpp
		Functional/Dialogue/DialogueAcceptsQuestAndGatesOnStateTest.cpp
		Functional/AI/BehaviorTreeSequenceShortCircuitsOnFailureTest.cpp
		Functional/AI/PerceptionMatchesBruteForceViaSpatialIndexTest.cpp
		Functional/AI/FlockingViaSceneTickTest.cpp
		Functional/Scripting/LuaReadsTransformOfAnotherEntityTest.cpp
		Functional/SaveGame/InventoryComponentSceneYAMLRoundTripTest.cpp
		Functional/SaveGame/HierarchyPreservedAcrossSaveLoadTest.cpp
		Functional/Scene/EntityNameMapStaysConsistentTest.cpp
		Functional/Diagnostics/EmptyTickReproTest.cpp
		# RPG progression Functional tests (issue #635): the full class/XP/
		# skill-tree acceptance loop over memory-only assets, the kill-XP hook
		# in ApplyDamage, quest-completion reward XP, and the Lua GrantExperience
		# binding driven through real scene ticks.
		Functional/Gameplay/ProgressionFullLoopTest.cpp
		Functional/Gameplay/PlayerRigDrivesCharacterViaSceneTickTest.cpp
		Functional/Gameplay/CameraRigSpringArmViaSceneTickTest.cpp
		Functional/Gameplay/KillGrantsExperienceViaDamageTest.cpp
		Functional/Gameplay/QuestRewardGrantsExperienceTest.cpp
		Functional/Scripting/LuaGrantsExperienceViaSceneTickTest.cpp
		# Discovery loop (issue #881): landing-trigger contact -> DiscoveredSetComponent,
		# no double-count on resting/repeated contact, and the save-game round-trip +
		# old-save (pre-dating the component) migration check the issue calls out.
		Functional/Gameplay/DiscoveryLoopLandingAndSaveTest.cpp
		# FramePacer: frame-rate cap sleep/spin math + EMA delta smoothing (#456)
		FramePacerTest.cpp
		# Octahedral foliage impostor atlas mapping (issue #433)
		Rendering/ImpostorOctahedralTest.cpp
		Rendering/PropertyTests/ImpostorBakeEvidenceTest.cpp
		# Animated MeshPrimitives (issue #592): CreateAnimatedCube / CreateMultiBoneAnimatedCube
		# skinning correctness against the MeshSource bone-influence contract.
		Rendering/PropertyTests/AnimatedMeshPrimitivesPropertyTests.cpp
		# Script-driven runtime spawning (issue #643): the deferred entity-command
		# queue driven from Lua through real Scene::OnUpdateRuntime ticks, under
		# both the parallel and the sequential gameplay executor.
		Functional/Scripting/LuaSpawnsAndDestroysEntitiesTest.cpp
		Functional/Scripting/VisualScriptDrivesGameplayTest.cpp

		# Accessibility (issue #458): global settings + text-scale policy +
		# prefs round-trip; the colour-vision math mirrored from
		# PostProcess_ColorBlind.glsl; and the caption overlay driven through
		# real Scene::OnUpdateRuntime ticks.
		Accessibility/AccessibilitySettingsTest.cpp
		Rendering/ColorBlindMathTest.cpp
		Rendering/PropertyTests/ColorBlindVisualEvidenceTest.cpp
		Rendering/PropertyTests/UITextScaleVisualEvidenceTest.cpp
		Functional/Accessibility/SubtitlesFollowDialogueStateTest.cpp
)

if(OLO_HEAVY_COMPILE_POOL_AVAILABLE)
	olo_bind_heavy_compile_pool(OloEngine-Tests "${CMAKE_SOURCE_DIR}/OloEditor/src"
		"MCP/McpFieldRegistry.cpp"
	)
endif()

target_link_libraries(OloEngine-Tests
	OloEngine
	GTest::gtest
	httplib::httplib)

# OpenVDB import (#724) CPU contract-test coverage — same reasoning as the
# editor link in OloEditor/CMakeLists.txt: OloEngine-VolumeCook is linked
# here only, never into OloEngine/OloRuntime/OloServer.
if(OLO_WITH_OPENVDB)
	target_link_libraries(OloEngine-Tests OloEngine-VolumeCook)
endif()

# Headless GPU support (docs/ops/self-hosted-gpu-runner.md).
#
# The fixture normally gets its GL 4.6 context from GLFW, which on Linux needs
# an X11 or Wayland display. A headless CI box has neither, so every test gated
# on RenderPropertyFixture::IsGpuAvailable() skips even when a perfectly good
# GPU is sitting there. Linking libEGL lets the fixture fall back to EGL's
# surfaceless platform and talk to the DRM render node directly.
#
# Strictly optional: where libEGL is absent we keep the GLFW-only path and
# those tests skip exactly as they did before, so this cannot break a build.
if(UNIX AND NOT APPLE)
	find_library(OLO_EGL_LIBRARY NAMES EGL)
	find_path(OLO_EGL_INCLUDE_DIR NAMES EGL/egl.h)
	if(OLO_EGL_LIBRARY AND OLO_EGL_INCLUDE_DIR)
		# Plain signature to match the target_link_libraries call above — CMake
		# rejects mixing the plain and keyword forms on a single target.
		target_link_libraries(OloEngine-Tests ${OLO_EGL_LIBRARY})
		target_include_directories(OloEngine-Tests PRIVATE ${OLO_EGL_INCLUDE_DIR})
		target_compile_definitions(OloEngine-Tests PRIVATE OLO_TESTS_HAVE_EGL=1)
		message(STATUS "OloEngine-Tests: headless EGL context path enabled (${OLO_EGL_LIBRARY})")
	else()
		message(STATUS "OloEngine-Tests: libEGL not found - headless GPU tests will skip")
	endif()
endif()

# Copy FFmpeg DLLs next to the test exe so OLO_VIDEO_FFMPEG decode tests can run.
olo_copy_ffmpeg_runtime(OloEngine-Tests)

# Stage steam_api64.dll next to the test exe (#644). Required for the same reason as OloServer:
# OloEngine links the SDK's IMPORT library, so the test binary carries a static import that
# Windows resolves at process load — without the DLL the whole suite fails to start, long before
# any Steam test would have skipped. No-op unless OLO_WITH_STEAM is on with the real SDK.
olo_copy_steam_runtime(OloEngine-Tests)

target_compile_definitions(OloEngine-Tests PRIVATE SOL_ALL_SAFETIES_ON=1)

# Functional fixtures resolve the editor asset root from this define so
# tests don't depend on the binary's working directory. See ADR 0003.
target_compile_definitions(OloEngine-Tests PRIVATE
    OLO_TEST_EDITOR_ROOT="${CMAKE_SOURCE_DIR}/OloEditor")

# The binary's own path, for the one test that has to observe the suite from
# the outside: GpuGateSwitchTest re-launches OloEngine-Tests with
# --olo-gl-backend=none / --olo-require-gpu and reads what the child reported,
# because the GPU gate's "skip vs fail" decision is process-wide and cannot be
# flipped from inside the process that already parsed its flags.
target_compile_definitions(OloEngine-Tests PRIVATE
    OLO_TEST_SELF_EXE="$<TARGET_FILE:OloEngine-Tests>")

# Fix UTF-8 compilation issue
if(MSVC)
	target_compile_options(OloEngine-Tests PRIVATE /utf-8 /FS /bigobj)
endif()

# atomic_queue and spdlog now arrive as vcpkg IMPORTED targets propagated by
# OloEngine's PUBLIC link interface (issue #773) — no explicit include dir needed.
target_include_directories(OloEngine-Tests PRIVATE
	${CMAKE_SOURCE_DIR}/OloEngine/src
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/Rendering
	${CMAKE_SOURCE_DIR}/OloEditor/src/Panels/ContentBrowser
	# MCP dispatch test includes the editor's MCP headers as "MCP/McpServer.h".
	${CMAKE_SOURCE_DIR}/OloEditor/src
)

# Register tests with CTest so `ctest` discovers them. The WORKING_DIRECTORY
# keeps repo-relative paths (shaders, assets) resolvable — many tests call
# Shader::Create("assets/shaders/...") which only works when CWD is OloEditor.
#
# DISCOVERY_TIMEOUT: the default of 5s is too tight for ASan-instrumented
# Release builds on CI runners — the sanitizer-wrapped startup + DLL load
# path can push `--gtest_list_tests` past 5s, causing the build itself to
# fail with "Process terminated due to timeout" before any test ever runs.
# 120s is very generous under normal conditions but gives sanitizer builds
# ample headroom without masking a genuinely hung binary.
include(GoogleTest)

# Parallel-safety: CI runs the suite with `ctest --parallel`, and because
# gtest_discover_tests registers every gtest case as its own ctest entry, each
# case runs in its own OloEngine-Tests.exe process — different cases of the same
# fixture run concurrently. Every test scratch path comes from
# OloEngine::Tests::TempDir() / TempFile() (TestTempDir.h), which roots them in a
# directory this process claimed exclusively and keys the leaf by gtest case; the
# `test-temp-dir-isolated` pre-commit hook fails any direct temp_directory_path()
# call, so the property is enforced rather than asserted (issue #789 — the earlier
# wording here, "most tests are process-isolated already", had never been
# measured). See the "Parallel-safety contract" section in
# docs/agent-rules/testing-architecture.md.
#
# The one resource that is NOT process-isolated is the engine's SHARED,
# repo-relative mesh cache (OloEditor/assets/cache/mesh/, content-hash keyed).
# These cases each load assets/backpack/backpack.obj and call
# MeshCache::InvalidateCache on it, so under `--parallel` one case's invalidate
# deletes another's just-written cache mid-test (observed:
# ModelLoadDeterminism.Backpack_FreshVsCache failing IsMeshCacheValid). The cache
# path is the engine's real on-disk location, not a test-controlled temp dir, so
# we serialize these offenders against EACH OTHER with a shared RESOURCE_LOCK;
# they still overlap freely with every unrelated test. Keep this list in sync if
# a new test loads + invalidates a shared model cache.
#
# DeccerCubesLoaderFixture joins the list for the same reason (issue #791): its
# SurvivesAWarmMeshCacheRoundTrip case invalidates a variant's .omesh, writes it
# cold, then reads it warm. LoadsWithMeshesAndFiniteBounds loads the SAME source
# files, so under --parallel the two cases race over one cache entry — the
# round-trip's invalidate can land between the other case's write and read. Both
# cases are matched with one wildcard.
set(OLO_MESH_CACHE_TESTS
	"ModelLoadDeterminism.Backpack_FreshVsCache:DataRoundTripTest.BackpackLegacyObjImportsPbrCompanionMaps:DataRoundTripTest.BackpackLegacyObjFlipsUvsToMatchTextureUploads:AllVariants/DeccerCubesLoaderFixture.*")

# --olo-* flags for every ctest-launched test process (a CMake list, so
# `-DOLO_TESTS_CTEST_ARGS=--olo-gl-backend=none` is one flag and
# `"--olo-gl-backend=egl;--olo-require-gpu"` is two). This is how a CI job that
# drives the suite through ctest, and therefore never writes the test command
# line itself, still states what it tested: the value is in the configure line
# the job log prints, and `ctest -V` shows it on every launched command. An
# environment variable would work too and is deliberately not offered -- see
# TestOptions.h for why the suite's knobs are flags. Empty by default; the
# direct-binary path (the AMD nightly, a developer's shell) is unaffected.
set(OLO_TESTS_CTEST_ARGS "" CACHE STRING
	"Extra --olo-* flags passed to every test process ctest launches (semicolon-separated)")
if(OLO_TESTS_CTEST_ARGS)
	message(STATUS "OloEngine-Tests: ctest will launch every test with: ${OLO_TESTS_CTEST_ARGS}")
endif()

gtest_discover_tests(OloEngine-Tests
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/OloEditor
	DISCOVERY_TIMEOUT 120
	TEST_FILTER "${OLO_MESH_CACHE_TESTS}"
	EXTRA_ARGS ${OLO_TESTS_CTEST_ARGS}
	PROPERTIES RESOURCE_LOCK "olo_mesh_cache"
)
gtest_discover_tests(OloEngine-Tests
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/OloEditor
	DISCOVERY_TIMEOUT 120
	TEST_FILTER "-${OLO_MESH_CACHE_TESTS}"
	EXTRA_ARGS ${OLO_TESTS_CTEST_ARGS}
)


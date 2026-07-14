enable_testing()
add_executable(OloEngine-Tests
		OloEngineTest.cpp
		FastRandomTest.cpp
		RefTest.cpp
		ContainerTest.cpp
		SnowSettingsTest.cpp
		WindSettingsTest.cpp
		PrecipitationSettingsTest.cpp
		PrecipitationEmitterTest.cpp
		PrecipitationSystemTest.cpp
		CoreUtilitiesTest.cpp
		MathTest.cpp
		ModelImporterTest.cpp
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
		SceneSerializerFuzzRegressionTest.cpp
		SceneVersionMigrationTest.cpp
		EngineSubsystemSmokeTest.cpp
		AppLaunchSmokeTest.cpp
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
		SphericalHarmonicsTest.cpp

		Async/ParkingLotTest.cpp
		Async/MutexTest.cpp
		Async/MutexLockRateTest.cpp
		Async/SemaphoreTest.cpp
		Async/ManualResetEventTest.cpp
		Async/SharedMutexTest.cpp
		Async/SharedRecursiveMutexTest.cpp
		Async/ExternalMutexTest.cpp
		Tasks/TaskSystemTest.cpp
		HAL/ThreadManagerTest.cpp
		Containers/ConcurrentQueuesTest.cpp
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
		Rendering/RendererSettingsBootstrapTest.cpp
		Rendering/ReflectionProbeSelectionTest.cpp
		Rendering/PlanarReflectionMathTest.cpp
		Rendering/CameraRelativeTest.cpp
		Rendering/Renderer2DCameraRelativeTest.cpp
		Rendering/TerrainCameraRelativeLODTest.cpp
		Rendering/LODTest.cpp
		Rendering/MeshOptimizationTest.cpp
		Rendering/SubmeshMaterialResolveTest.cpp
		Rendering/VirtualMeshBuilderTest.cpp
		Rendering/VirtualMeshCookIdentityTest.cpp
		Rendering/VirtualClusterCullParityTest.cpp
		Rendering/MeshSourceBoundsTest.cpp
		Rendering/MeshBVHRaycastTest.cpp
		Rendering/PODCommandTest.cpp
		Rendering/ShaderBindingLayoutTest.cpp
		Rendering/ShaderReflectionBindingTest.cpp
		Rendering/ShaderCrossConsistencyTest.cpp
		Rendering/ShaderUBOSizeConsistencyTest.cpp
		Rendering/ShaderStageInterfaceTest.cpp
		Rendering/ShaderStageContractTest.cpp
		Rendering/ShaderPackTest.cpp
		Rendering/ShaderBinaryCacheRoundTripTest.cpp
		Rendering/FrameDataBufferTest.cpp
		Rendering/InstanceDataLayoutTest.cpp
		Rendering/GPUFrustumCullParityTest.cpp
		Rendering/GPUOcclusionCullParityTest.cpp
		Rendering/DeferredTwoPhaseOcclusionTest.cpp
		Rendering/ScatterBrushMathTest.cpp
		Rendering/SphereAreaLightMathTest.cpp
		Rendering/ProceduralSkyMathTest.cpp
		Rendering/StarNestSkyMathTest.cpp
		Rendering/AutoExposureMathTest.cpp
		Rendering/ScreenSpaceReflectionMathTest.cpp
		Rendering/ScreenSpaceGIMathTest.cpp
		Rendering/ContactShadowMathTest.cpp
		Rendering/OverdrawHeatmapMathTest.cpp
		Rendering/CASMathTest.cpp
		Rendering/EASUMathTest.cpp
		Rendering/RCASMathTest.cpp
		Rendering/SSAOMathTest.cpp
		Rendering/GTAOMathTest.cpp
		Rendering/BloomMathTest.cpp
		Rendering/FogMathTest.cpp
		Rendering/ClusteredLightingMathTest.cpp
		Rendering/ShadowAtlasPackingTest.cpp
		Rendering/VolumetricFogMathTest.cpp
		Rendering/MotionVectorMathTest.cpp
		Rendering/SSRHiZTraversalTest.cpp
		Rendering/RenderGraphFingerprintTest.cpp
		Rendering/CommandPacketTest.cpp
		Rendering/CommandAllocatorTest.cpp
		Rendering/CommandBucketTest.cpp
		#Rendering/CommandDispatchTest.cpp  # Requires full engine (calls CommandDispatch::Initialize which pulls in OpenGL statics)
		Rendering/RenderGraphTest.cpp
		Rendering/ResourceHazardValidationTests.cpp
		Rendering/RenderGraphPathSwitchTests.cpp
		Rendering/FramePipelineTest.cpp
		Rendering/FrameCaptureTest.cpp
		Rendering/CommandBucketBenchmarkTest.cpp
		Rendering/RenderStateTest.cpp
		Rendering/OcclusionStateTest.cpp
		Rendering/OcclusionIntegrationTest.cpp
		Rendering/WaterRenderingTest.cpp
		Rendering/FluidRenderMathTest.cpp
		Fluid/FluidKernelContractTest.cpp
		Fluid/CPUFluidSolverContractTest.cpp
		Rendering/PCSSShadowTest.cpp
		Rendering/RenderingRegressionTest.cpp
		Rendering/SelectionOutlineTest.cpp
		Rendering/DeccerCubesLoadingTest.cpp
		Rendering/AnimatedModelDedupTest.cpp
		# Rendering property tests (require GL 4.5+ context; skip gracefully if unavailable)
		Rendering/PropertyTests/RenderPropertyTest.cpp
		Rendering/PropertyTests/CameraRelativeVisualEvidenceTest.cpp
		Rendering/PropertyTests/WorldOriginRebaseVisualEvidenceTest.cpp
		Rendering/PropertyTests/Renderer2DCameraRelativeVisualEvidenceTest.cpp
		Rendering/PropertyTests/PlanarReflectionVisualEvidenceTest.cpp
		Rendering/PropertyTests/HZBGeneratorResizeTest.cpp
		Rendering/PropertyTests/GPUOcclusionCullGPUTest.cpp
		Rendering/PropertyTests/GPUFluidSolverParityTest.cpp
		Rendering/PropertyTests/OcclusionCullVisualEvidenceTest.cpp
		Rendering/PropertyTests/OcclusionCullDeferredVisualEvidenceTest.cpp
		Rendering/PropertyTests/DeferredInstancedShadingEvidenceTest.cpp
		Rendering/PropertyTests/OceanFFTGpuContractTest.cpp
		Rendering/PropertyTests/OceanFFTSpectrumTest.cpp
		Rendering/PropertyTests/PostProcessPropertyTests.cpp
		Rendering/PropertyTests/PbrPropertyTests.cpp
		Rendering/PropertyTests/ShaderMathIdentityTests.cpp
		Rendering/PropertyTests/GLStateGuardTest.cpp
		Rendering/PropertyTests/RendererValidateTest.cpp
		Rendering/PropertyTests/ShaderUnitTests.cpp
		Rendering/PropertyTests/PerfRegressionTests.cpp
		Rendering/PropertyTests/DataRoundTripTests.cpp
		Rendering/PropertyTests/GoldenImageTests.cpp
		Rendering/PropertyTests/SphereAreaLightVisualTest.cpp
		Rendering/PropertyTests/ProceduralSkyBakeTest.cpp
		Rendering/PropertyTests/ProceduralSkyVisualTest.cpp
		Rendering/PropertyTests/StarNestSkyBakeTest.cpp
		Rendering/PropertyTests/StarNestSkyVisualTest.cpp
		Rendering/PropertyTests/StarNestReflectionEvidenceTest.cpp
		Rendering/PropertyTests/WaterVisualEvidenceTest.cpp
		Rendering/PropertyTests/VirtualGeometryVisualEvidenceTest.cpp
		Rendering/PropertyTests/SubmeshMaterialPathParityTest.cpp
		Rendering/PropertyTests/VirtualGeometryRasterParityTest.cpp
		Rendering/PropertyTests/VirtualMeshAssetPackTest.cpp
		Rendering/PropertyTests/ImportedMaterialPackTest.cpp
		Rendering/PropertyTests/FluidVisualEvidenceTest.cpp
		Rendering/PropertyTests/WaterStaleTexturePublicationTest.cpp
		Rendering/PropertyTests/CompressedTextureVisualEvidenceTest.cpp
		Rendering/PropertyTests/TextureAutoCookPackTest.cpp
		Rendering/PropertyTests/OceanFFTVisualEvidenceTest.cpp
		Rendering/PropertyTests/UnderwaterCausticsVisualTest.cpp
		Rendering/PropertyTests/SSRVisualEvidenceTest.cpp
		Rendering/PropertyTests/BloomVisualEvidenceTest.cpp
		Rendering/PropertyTests/FogVisualEvidenceTest.cpp
		Rendering/PropertyTests/ClusteredLightingVisualEvidenceTest.cpp
		Rendering/PropertyTests/ShadowAtlasVisualEvidenceTest.cpp
		Rendering/PropertyTests/VolumetricFogVisualEvidenceTest.cpp
		Rendering/PropertyTests/ReflectionProbeVisualEvidenceTest.cpp
		Rendering/PropertyTests/SSGIVisualEvidenceTest.cpp
		Rendering/PropertyTests/OverdrawVisualEvidenceTest.cpp
		Rendering/PropertyTests/ContactShadowVisualEvidenceTest.cpp
		Rendering/PropertyTests/CASVisualEvidenceTest.cpp
		Rendering/PropertyTests/EASUVisualEvidenceTest.cpp
		Rendering/PropertyTests/SSAOVisualEvidenceTest.cpp
		Rendering/PropertyTests/GTAOVisualEvidenceTest.cpp
		Rendering/PropertyTests/PCSSVisualEvidenceTest.cpp
		Rendering/PropertyTests/ShadowEarlyOutVisualEvidenceTest.cpp
		Rendering/PropertyTests/WaterSurfaceSamplerTest.cpp
		Rendering/PropertyTests/WaterBuoyancyVisualEvidenceTest.cpp
		Rendering/PropertyTests/ShadowTerrainPropertyTests.cpp
		Rendering/PropertyTests/TerrainGenerationEvidenceTest.cpp
		Rendering/PropertyTests/TerrainCameraRelativeVisualEvidenceTest.cpp
		Rendering/PropertyTests/FoliageGenerationEvidenceTest.cpp
		Rendering/PropertyTests/DeferredPropertyTests.cpp
		Rendering/PropertyTests/DeferredOverlayPassTests.cpp
		Rendering/PropertyTests/OITPropertyTests.cpp
		Rendering/PropertyTests/ShaderCompilationTest.cpp
		Rendering/PropertyTests/ShaderCompileFailureRecoveryTest.cpp
		Rendering/PropertyTests/AssetPreviewRendererTest.cpp
		Rendering/PropertyTests/RendererAttachedTest.cpp
		Rendering/RendererShutdownTest.cpp
		Rendering/PropertyTests/RendererAttachedSmokeTest.cpp
		Rendering/PropertyTests/SceneRenderEvidenceTest.cpp
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
		Rendering/PropertyTests/TextureInPlaceReloadTest.cpp
		Streaming/SceneStreamingTest.cpp
		InputActionTest.cpp
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
		Terrain/TerrainGeneratorTest.cpp
		# Morph Target Tests
		MorphTargetTest.cpp
		# AI Behavior Tree & FSM Tests
		BehaviorTreeTest.cpp
		StateMachineTest.cpp
		# AI GOAP (Goal-Oriented Action Planning) Tests
		AI/GoapTest.cpp
		# AI Sight-Perception Tests
		AI/PerceptionMathTest.cpp
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
		Scene/SystemSchedulerTest.cpp
		# Floating-origin / origin-rebasing math (issue #429)
		Scene/WorldOriginRebaseTest.cpp
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
		${CMAKE_SOURCE_DIR}/OloEditor/src/MCP/McpToolsCamera.cpp
		${CMAKE_SOURCE_DIR}/OloEditor/src/MCP/McpToolsDiagnostics.cpp
		${CMAKE_SOURCE_DIR}/OloEditor/src/MCP/McpToolsInput.cpp
		${CMAKE_SOURCE_DIR}/OloEditor/src/MCP/McpToolsPerf.cpp
		${CMAKE_SOURCE_DIR}/OloEditor/src/MCP/McpToolsPhysics.cpp
		${CMAKE_SOURCE_DIR}/OloEditor/src/MCP/McpToolsRender.cpp
		${CMAKE_SOURCE_DIR}/OloEditor/src/MCP/McpToolsScene.cpp
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
		# behind olo_render_probe_pixel (#607: the per-pixel numeric G-Buffer readout).
		# Header-only (MCP/McpRenderProbePixel.h), no extra editor TU pulled in.
		MCP/McpRenderProbePixelTest.cpp
		# Pure light-grid aggregation (per-slice / per-cluster histogram, overflow +
		# empty cluster counts) behind olo_cluster_grid_stats (#607). Header-only
		# (MCP/McpClusterGridStats.h), no extra editor TU pulled in.
		MCP/McpClusterGridStatsTest.cpp
		# Pure froxel <-> world mapping (the shaders' exponential z-slice
		# distribution + view-ray secant scaling) behind olo_froxel_fog_probe (#607,
		# relates to #435). Header-only (MCP/McpFroxelFogProbe.h), no extra editor TU.
		MCP/McpFroxelFogProbeTest.cpp
		# Pure array-layer selection behind olo_render_capture_target's 'layer'
		# argument (#607): the CSM cascade array / shadow-atlas raw-depth views, and
		# the "a layer view must not silently capture layer 0" rule. Header-only
		# (MCP/McpShadowCapture.h), no extra editor TU pulled in.
		MCP/McpShadowCaptureLayerTest.cpp
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
		# Generic consented, undoable MCP WRITE tool: olo_entity_set_field + its read
		# companion olo_entity_list_fields (#306 item C, second slice). Same seams as
		# McpConsentedWriteTest.cpp — the session write gate + inputSchema enforcement
		# at the McpServer.cpp dispatch seam, and the shared reflect+coerce+apply core
		# (MCP/McpGenericFieldWrite.h). Header-only on the editor side, so no extra
		# editor TU is pulled in.
		MCP/McpGenericFieldWriteTest.cpp
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
		# Lua Binding Tests
		Lua/LuaBindingTest.cpp
		# Functional / cross-subsystem world-tick tests (see ADR 0001).
		Functional/FunctionalTest.cpp
		Functional/UI/UINavigationTest.cpp
		Functional/Cinematic/CinematicDrivesEntitiesTest.cpp
		Functional/Cinematic/CinematicAssetPlaybackTest.cpp
		Functional/AnimationPhysics/FreeFallLandingTest.cpp
		Functional/AnimationPhysics/TerrainCollisionTest.cpp
		Functional/AnimationPhysics/ClothSimulationTest.cpp
		Functional/AnimationPhysics/ClothSkeletonAttachTest.cpp
		Functional/AnimationPhysics/PhysicsJoint3DTest.cpp
		Functional/AnimationPhysics/PhysicsJointCollideConnectedTest.cpp
		Functional/AnimationPhysics/VehicleTest.cpp
		Functional/AnimationPhysics/RagdollTest.cpp
		Functional/AnimationPhysics/WaterBuoyancyTest.cpp
		Functional/AnimationPhysics/FluidCouplingTest.cpp
		Functional/AnimationPhysics/AnimationKeepsTickingUnderPhysicsLoadTest.cpp
		Functional/SaveGame/SceneRoundTripAfterTickTest.cpp
		Functional/Networking/PhysicsTransformReplicationTest.cpp
		Functional/Networking/InterpolatedComponentsSmoothOnRemoteClientTest.cpp
		Functional/Scene/ScenePauseFreezesAllSubsystemsTest.cpp
		Functional/AnimationPhysics/EntityDestroyedMidTickTest.cpp
		Functional/AnimationPhysics/PhysicsLayerFilteringTest.cpp
		Functional/AnimationPhysics/EntityAddedAtRuntimeTest.cpp
		Functional/Scene/PhysicsRestartIsCleanTest.cpp
		Functional/Scene/WorldOriginRebasePhysicsTest.cpp
		Functional/Scene/MultipleScenesCoexistTest.cpp
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
		Functional/Asset/AssetManagerImportsStagedAssetTest.cpp
		Functional/Gameplay/TimedQuestFailsAfterDeadlineTest.cpp
		Functional/Gameplay/AbilityCooldownTicksDownViaSceneTickTest.cpp
		Functional/Scripting/LuaScriptSetsRigidbody2DVelocityTest.cpp
		Functional/SaveGame/AbilityComponentRoundTripTest.cpp
		Functional/Dialogue/DialogueTextRevealsOverTicksTest.cpp
		Functional/Gameplay/DamageKillsEntityAndFlipsDeathTagTest.cpp
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
		Functional/Scripting/LuaReadsTransformOfAnotherEntityTest.cpp
		Functional/SaveGame/InventoryComponentSceneYAMLRoundTripTest.cpp
		Functional/SaveGame/HierarchyPreservedAcrossSaveLoadTest.cpp
		Functional/Scene/EntityNameMapStaysConsistentTest.cpp
		Functional/Diagnostics/EmptyTickReproTest.cpp
		# FramePacer: frame-rate cap sleep/spin math + EMA delta smoothing (#456)
		FramePacerTest.cpp
)
target_link_libraries(OloEngine-Tests
	OloEngine
	gtest
	cpp_httplib)

# Copy FFmpeg DLLs next to the test exe so OLO_VIDEO_FFMPEG decode tests can run.
olo_copy_ffmpeg_runtime(OloEngine-Tests)

target_compile_definitions(OloEngine-Tests PRIVATE SOL_ALL_SAFETIES_ON=1)

# Functional fixtures resolve the editor asset root from this define so
# tests don't depend on the binary's working directory. See ADR 0003.
target_compile_definitions(OloEngine-Tests PRIVATE
    OLO_TEST_EDITOR_ROOT="${CMAKE_SOURCE_DIR}/OloEditor")

# Fix UTF-8 compilation issue
if(MSVC)
	target_compile_options(OloEngine-Tests PRIVATE /utf-8 /FS /bigobj)
endif()

target_include_directories(OloEngine-Tests PRIVATE
	${CMAKE_SOURCE_DIR}/OloEngine/src
	${atomic_queue_SOURCE_DIR}/include
	${spdlog_SOURCE_DIR}/include
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
# fixture run concurrently. Most tests are process-isolated already (temp paths
# keyed by PID / gtest case name); see the "Parallel-safety contract" section in
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
set(OLO_MESH_CACHE_TESTS
	"ModelLoadDeterminism.Backpack_FreshVsCache:DataRoundTripTest.BackpackLegacyObjImportsPbrCompanionMaps:DataRoundTripTest.BackpackLegacyObjFlipsUvsToMatchTextureUploads")

gtest_discover_tests(OloEngine-Tests
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/OloEditor
	DISCOVERY_TIMEOUT 120
	TEST_FILTER "${OLO_MESH_CACHE_TESTS}"
	PROPERTIES RESOURCE_LOCK "olo_mesh_cache"
)
gtest_discover_tests(OloEngine-Tests
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/OloEditor
	DISCOVERY_TIMEOUT 120
	TEST_FILTER "-${OLO_MESH_CACHE_TESTS}"
)


enable_testing()
add_executable(OloEngine-Tests
		OloEngineTest.cpp
		FastRandomTest.cpp
		ContainerTest.cpp
		SnowSettingsTest.cpp
		WindSettingsTest.cpp
		PrecipitationSettingsTest.cpp
		PrecipitationEmitterTest.cpp
		PrecipitationSystemTest.cpp
		CoreUtilitiesTest.cpp
		MathTest.cpp
		ModelImporterTest.cpp
		SlugFontRenderingTest.cpp
		AssetCreationTest.cpp
		AssetBinaryIntegrityTest.cpp
		AssetContentValidityTest.cpp
		AssetExtensionsCoverageTest.cpp
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
		SceneSerializerFuzzRegressionTest.cpp
		EngineSubsystemSmokeTest.cpp
		FunctionalTestFixtureCoverageTest.cpp
		PreCommitToolingSmokeTest.cpp
		PrefabOverrideTest.cpp
		ShadowMapTest.cpp
		PostProcessTest.cpp
		LightCullingTest.cpp
		SoundGraphBasicTest.cpp
		SoundGraphInstantiationTest.cpp
		AudioEventQueueTest.cpp
		AudioDSPTest.cpp
		AudioSpatializerTest.cpp
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
		Templates/FunctionWithContextTest.cpp
		Templates/TypeTraitsTest.cpp

		# Rendering subsystem tests (Tiers 1-5)
		Rendering/DrawKeyTest.cpp
		Rendering/BoundingVolumeTest.cpp
		Rendering/FrustumCullingTest.cpp
		Rendering/ReflectionProbeSelectionTest.cpp
		Rendering/LODTest.cpp
		Rendering/MeshOptimizationTest.cpp
		Rendering/MeshSourceBoundsTest.cpp
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
		Rendering/ScatterBrushMathTest.cpp
		Rendering/SphereAreaLightMathTest.cpp
		Rendering/ProceduralSkyMathTest.cpp
		Rendering/AutoExposureMathTest.cpp
		Rendering/ScreenSpaceReflectionMathTest.cpp
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
		Rendering/RenderingRegressionTest.cpp
		Rendering/SelectionOutlineTest.cpp
		Rendering/DeccerCubesLoadingTest.cpp
		# Rendering property tests (require GL 4.5+ context; skip gracefully if unavailable)
		Rendering/PropertyTests/RenderPropertyTest.cpp
		Rendering/PropertyTests/HZBGeneratorResizeTest.cpp
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
		Rendering/PropertyTests/WaterVisualEvidenceTest.cpp
		Rendering/PropertyTests/SSRVisualEvidenceTest.cpp
		Rendering/PropertyTests/WaterSurfaceSamplerTest.cpp
		Rendering/PropertyTests/WaterBuoyancyVisualEvidenceTest.cpp
		Rendering/PropertyTests/ShadowTerrainPropertyTests.cpp
		Rendering/PropertyTests/DeferredPropertyTests.cpp
		Rendering/PropertyTests/DeferredOverlayPassTests.cpp
		Rendering/PropertyTests/OITPropertyTests.cpp
		Rendering/PropertyTests/ShaderCompilationTest.cpp
		Rendering/PropertyTests/AssetPreviewRendererTest.cpp
		Rendering/PropertyTests/RendererAttachedTest.cpp
		Rendering/PropertyTests/RendererAttachedSmokeTest.cpp
		Rendering/PropertyTests/SceneRenderEvidenceTest.cpp
		Rendering/PropertyTests/AutoExposureEvidenceTest.cpp
		Rendering/PropertyTests/TestFailureCapture.cpp
		Rendering/PropertyTests/TestFailureCaptureTest.cpp
		Rendering/PropertyTests/ModelLoadDeterminismTest.cpp
		Rendering/PropertyTests/DrawIndexedRawOffsetTest.cpp
		Rendering/PropertyTests/TextureSaveRoundTripTest.cpp
		Streaming/SceneStreamingTest.cpp
		InputActionTest.cpp
		GamepadTest.cpp
		Networking/NetworkMessageTest.cpp
		Networking/NetworkMessageDispatcherTest.cpp
		Networking/NetworkThreadDispatchTest.cpp
		Networking/NetworkIntegrationTest.cpp
		Networking/ComponentReplicatorTest.cpp
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
		# Localization tests
		LocalizationTest.cpp
		# Dialogue system tests
		DialogueVariablesTest.cpp
		DialogueTreeSerializationTest.cpp
		DialogueSystemTest.cpp
		# Shader Graph Tests
		ShaderGraph/ShaderGraphTest.cpp
		ShaderGraph/ShaderGraphCompilerTest.cpp
		ShaderGraph/ShaderGraphSerializationTest.cpp
		ShaderGraph/ShaderGraphCommandTest.cpp
		# Serialization Tests
		Serialization/ArchiveExtensionsTest.cpp
		Serialization/MeshBinarySerializerTest.cpp
		Serialization/MeshAssetSerializerTest.cpp
		Serialization/SoundGraphSerializerTest.cpp
		Serialization/EnvironmentSerializerTest.cpp
		Serialization/FontAssetPackSerializerTest.cpp
		# SaveGame Tests
		SaveGame/SaveGameFileTest.cpp
		SaveGame/SaveGameManagerTest.cpp
		SaveGame/SaveGameIntegrationTest.cpp
		SaveGame/SaveFileWorldDatabaseTest.cpp
		# Navigation Tests
		NavMeshTest.cpp
		HeadlessTest.cpp
		# Animation State Machine & Blend Tree Tests
		AnimationStateMachineTest.cpp
		AnimationSystemTest.cpp
		BlendTreeTest.cpp
		Animation/AimIKSolverTest.cpp
		Animation/LimbIKSolverTest.cpp
		Animation/BlendUtilsTest.cpp
		Animation/OneShotBlendTest.cpp
		# Cinematic Sequencer Tests
		Cinematic/CinematicCurveTest.cpp
		Cinematic/CinematicPlayerTest.cpp
		Cinematic/CinematicSerializerTest.cpp
		Cinematic/CinematicEditTest.cpp
		# Morph Target Tests
		MorphTargetTest.cpp
		# AI Behavior Tree & FSM Tests
		BehaviorTreeTest.cpp
		StateMachineTest.cpp
		# AI GOAP (Goal-Oriented Action Planning) Tests
		AI/GoapTest.cpp
		# Inventory Tests
		InventoryTest.cpp
		# Quest Tests
		QuestSystemTest.cpp
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
		# Physics3D mesh mass-properties (divergence-theorem volume + centroid)
		JoltShapesMeshMassPropertiesTest.cpp
		# Physics3D mesh collider cache invalidation
		MeshCookingFactoryCacheTest.cpp
		# CrashReporter / platform handlers (Windows SEH + Linux signal hooks)
		Debug/CrashReporterTest.cpp
		# Gameplay Ability Tests
		Gameplay/GameplayAbilityTest.cpp
		# Content Browser Tests
		ContentBrowser/DirectoryTreeTest.cpp
		ContentBrowser/ContentBrowserItemTest.cpp
		${CMAKE_SOURCE_DIR}/OloEditor/src/Panels/ContentBrowser/DirectoryTree.cpp
		${CMAKE_SOURCE_DIR}/OloEditor/src/Panels/ContentBrowser/ContentBrowserItem.cpp
		# Lua Binding Tests
		Lua/LuaBindingTest.cpp
		# Functional / cross-subsystem world-tick tests (see ADR 0001).
		Functional/FunctionalTest.cpp
		Functional/Cinematic/CinematicDrivesEntitiesTest.cpp
		Functional/Cinematic/CinematicAssetPlaybackTest.cpp
		Functional/AnimationPhysics/FreeFallLandingTest.cpp
		Functional/AnimationPhysics/WaterBuoyancyTest.cpp
		Functional/AnimationPhysics/AnimationKeepsTickingUnderPhysicsLoadTest.cpp
		Functional/SaveGame/SceneRoundTripAfterTickTest.cpp
		Functional/Networking/PhysicsTransformReplicationTest.cpp
		Functional/Scene/ScenePauseFreezesAllSubsystemsTest.cpp
		Functional/AnimationPhysics/EntityDestroyedMidTickTest.cpp
		Functional/AnimationPhysics/PhysicsLayerFilteringTest.cpp
		Functional/AnimationPhysics/EntityAddedAtRuntimeTest.cpp
		Functional/Scene/PhysicsRestartIsCleanTest.cpp
		Functional/Scene/MultipleScenesCoexistTest.cpp
		Functional/Scene/HierarchyChildFollowsPhysicsParentTest.cpp
		Functional/AI/BehaviorTreeAdvancesViaSceneTickTest.cpp
		Functional/AI/GoapAgentPlansViaSceneTickTest.cpp
		Functional/AI/GoapAuthoredFromLuaViaSceneTickTest.cpp
		Functional/Particles/ParticleSystemEmitsAndExpiresTest.cpp
		Functional/AnimationPhysics/CharacterControllerWalksTest.cpp
		Functional/AnimationPhysics/AnimationBlendCompletesViaSceneTickTest.cpp
		Functional/Networking/SnapshotApplyAcrossScenesTest.cpp
		Functional/AnimationPhysics/Physics2DSimulatesViaSceneTickTest.cpp
		Functional/AnimationPhysics/CharacterControllerContactCallbackTest.cpp
		Functional/AnimationPhysics/ComponentRemovedAtRuntimeTest.cpp
		Functional/AnimationPhysics/TriggerVolumeFiresOverlapEventTest.cpp
		Functional/Scene/SceneStepAdvancesOneFrameTest.cpp
		Functional/Scene/DeterministicReplayProducesSameStateTest.cpp
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
		Functional/AnimationPhysics/CharacterControllerJumpsTest.cpp
		Functional/Scripting/LuaScriptMutatesTransformViaSceneTickTest.cpp
		Functional/Scripting/LuaScriptOnDestroyFiresTest.cpp
		Functional/AnimationPhysics/RigidbodyDisableGravityFlagTest.cpp
		Functional/AnimationPhysics/InitialLinearVelocityIsAppliedTest.cpp
		Functional/Audio/AudioListenerPositionUpdatesViaSceneTickTest.cpp
		Functional/AnimationPhysics/AnimationGraphStateMachineTickTest.cpp
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
		Functional/SaveGame/QuestJournalSceneYAMLRoundTripTest.cpp
		Functional/Gameplay/GameplayEffectExpiresAndRevertsAttributeTest.cpp
		Functional/Gameplay/AbilityBlockedByOwnerTagTest.cpp
		Functional/Gameplay/InventoryStackConsolidationTest.cpp
		Functional/Navigation/NavMeshQueryFindPathBetweenPointsTest.cpp
		Functional/Scripting/LuaRaycastHitsPhysicsBodyTest.cpp
		Functional/Scripting/LuaCompletesQuestViaIncrementObjectiveTest.cpp
		Functional/Dialogue/DialogueAdvanceMovesToNextNodeTest.cpp
		Functional/Gameplay/InventoryTransferItemBetweenContainersTest.cpp
		Functional/Gameplay/ChanneledAbilityAutoDeactivatesAtEndOfDurationTest.cpp
		Functional/Gameplay/MultiStageQuestAdvancesThroughStagesTest.cpp
		Functional/AnimationPhysics/AnimationGraphMultipleEntitiesAdvanceIndependentlyTest.cpp
		Functional/Gameplay/ToggledAbilityReactivateCancelsTest.cpp
		Functional/Gameplay/PeriodicEffectStackingScalesDamageTest.cpp
		Functional/Gameplay/AbilityActivationEffectAppliesToCasterTest.cpp
		Functional/Gameplay/AbilityRequiredTagGatesActivationTest.cpp
		Functional/AnimationPhysics/TwoCharacterControllersCoexistTest.cpp
		Functional/Gameplay/AbilityResourceCostDeductedFromManaTest.cpp
		Functional/Dialogue/DialogueSelectChoiceBranchesToTargetTest.cpp
		Functional/AI/BehaviorTreeSequenceShortCircuitsOnFailureTest.cpp
		Functional/Scripting/LuaReadsTransformOfAnotherEntityTest.cpp
		Functional/SaveGame/InventoryComponentSceneYAMLRoundTripTest.cpp
		Functional/SaveGame/HierarchyPreservedAcrossSaveLoadTest.cpp
		Functional/Scene/EntityNameMapStaysConsistentTest.cpp
		Functional/Diagnostics/EmptyTickReproTest.cpp
)
target_link_libraries(OloEngine-Tests
	OloEngine
	gtest)

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
gtest_discover_tests(OloEngine-Tests
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/OloEditor
	DISCOVERY_TIMEOUT 120
)


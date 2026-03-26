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
		PrecipitationBenchmarkTest.cpp
		CoreUtilitiesTest.cpp
		LoggerTest.cpp
		MathTest.cpp
		AssetCreationTest.cpp
		AssetGenerationTest.cpp
		AssetPackTest.cpp
		PrefabOverrideTest.cpp
		ShadowMapTest.cpp
		PostProcessTest.cpp
		ComputeShaderAndSSBOTest.cpp
		LightCullingTest.cpp
		SoundGraphBasicTest.cpp
		AudioEventQueueTest.cpp
		AudioDSPTest.cpp
		AudioSpatializerTest.cpp
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
		Containers/ConcurrentQueuesTest.cpp
		Memory/MemoryViewTest.cpp
		Templates/FunctionWithContextTest.cpp
		Templates/TypeTraitsTest.cpp

		# Rendering subsystem tests (Tiers 1-5)
		Rendering/DrawKeyTest.cpp
		Rendering/BoundingVolumeTest.cpp
		Rendering/FrustumCullingTest.cpp
		Rendering/LODTest.cpp
		Rendering/PODCommandTest.cpp
		Rendering/ShaderBindingLayoutTest.cpp
		Rendering/FrameDataBufferTest.cpp
		Rendering/CommandPacketTest.cpp
		Rendering/CommandAllocatorTest.cpp
		Rendering/CommandBucketTest.cpp
		#Rendering/CommandDispatchTest.cpp  # Requires full engine (calls CommandDispatch::Initialize which pulls in OpenGL statics)
		Rendering/RenderGraphTest.cpp
		Rendering/FramePipelineTest.cpp
		Rendering/FrameCaptureTest.cpp
		Rendering/CommandBucketBenchmarkTest.cpp
		Rendering/RenderStateTest.cpp
		Rendering/OcclusionStateTest.cpp
		Rendering/OcclusionIntegrationTest.cpp
		Rendering/WaterRenderingTest.cpp
		Rendering/RenderingRegressionTest.cpp
		Rendering/SelectionOutlineTest.cpp
		Streaming/SceneStreamingTest.cpp
		InputActionTest.cpp
		GamepadTest.cpp
		Networking/NetworkManagerTest.cpp
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
		# SaveGame Tests
		SaveGame/SaveGameFileTest.cpp
		SaveGame/SaveGameManagerTest.cpp
		SaveGame/SaveGameIntegrationTest.cpp
		SaveGame/SaveFileWorldDatabaseTest.cpp
		# Navigation Tests
		NavMeshTest.cpp
		UndoRedoTest.cpp
		HeadlessTest.cpp
		# Animation State Machine & Blend Tree Tests
		AnimationStateMachineTest.cpp
		AnimationSystemTest.cpp
		BlendTreeTest.cpp
		# Morph Target Tests
		MorphTargetTest.cpp
		# AI Behavior Tree & FSM Tests
		BehaviorTreeTest.cpp
		StateMachineTest.cpp
		# Inventory Tests
		InventoryTest.cpp
		# Quest Tests
		QuestSystemTest.cpp
		# Auto-Save Tests
		AutoSaveTest.cpp
		# Gameplay Ability Tests
		Gameplay/GameplayAbilityTest.cpp
)
target_link_libraries(OloEngine-Tests
	OloEngine
	gtest)

# Fix UTF-8 compilation issue
if(MSVC)
	target_compile_options(OloEngine-Tests PRIVATE /utf-8)
endif()

target_include_directories(OloEngine-Tests PRIVATE
	${CMAKE_SOURCE_DIR}/OloEngine/src
	${CMAKE_SOURCE_DIR}/OloEngine/vendor/atomic_queue-src/include
	${CMAKE_SOURCE_DIR}/OloEngine/vendor/spdlog-src/include
	${CMAKE_CURRENT_SOURCE_DIR}/Rendering
)

# Copy required shared library DLLs next to the test executable
if(WIN32)
    add_custom_command(TARGET OloEngine-Tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/OloEngine/vendor/libpng-build/$<CONFIG>/libpng16$<$<CONFIG:Debug>:d>.dll"
            $<TARGET_FILE_DIR:OloEngine-Tests>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/OloEngine/vendor/zlib-build/$<CONFIG>/zlib$<$<CONFIG:Debug>:d>.dll"
            $<TARGET_FILE_DIR:OloEngine-Tests>)
endif()

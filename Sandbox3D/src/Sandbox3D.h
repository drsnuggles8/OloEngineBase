
#pragma once

#include "OloEngine.h"
#include "OloEngine/Renderer/Camera/PerspectiveCameraController.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/SkinnedMesh.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Renderer/AnimatedModel.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Light.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Renderer/EnvironmentMap.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugger.h"
#include "OloEngine/Renderer/Debug/CommandPacketDebugger.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Debug/GPUResourceInspector.h"
#include "OloEngine/Renderer/Debug/ShaderDebugger.h"
#include "OloEngine/Scene/Components.h"

class Sandbox3D : public OloEngine::Layer
{
public:
	Sandbox3D();
	~Sandbox3D() final = default;

	void OnAttach() override;
	void OnDetach() override;
	void OnUpdate(OloEngine::Timestep ts) override;
	void OnImGuiRender() override;
	void OnEvent(OloEngine::Event& e) override;

	// Scene types for organized testing
	enum class SceneType
	{
		MaterialTesting = 0,
		AnimationTesting = 1,
		LightingTesting = 2,
		StateTesting = 3,
		ModelLoading = 4,
		PBRModelTesting = 5,
		Count // Used for compile-time array bounds checking
	};

private:
	// Configuration constants
	static constexpr int DEFAULT_SELECTED_MODEL_INDEX = 1; // Start with Fox to see its bone debugging first
	static constexpr float DEFAULT_JOINT_SIZE = 0.05f; // Increased from 0.02f for better visibility
	static constexpr float DEFAULT_BONE_THICKNESS = 3.0f; // Increased from 2.0f for better visibility
	static constexpr int DEFAULT_ANIMATED_MODEL_MATERIAL_TYPE = 0; // Silver for good contrast
	static constexpr int DEFAULT_CURRENT_ANIMATION_INDEX = 0;
	static constexpr int DEFAULT_SELECTED_PBR_MODEL_INDEX = 0;


private:
	// Scene management
	SceneType m_CurrentScene = SceneType::PBRModelTesting; // Start with PBR model testing
	const char* m_SceneNames[static_cast<int>(SceneType::Count)] = { 
		"Material Testing", 
		"Animation Testing", 
		"Lighting Testing", 
		"State Testing", 
		"Model Loading",
		"PBR Model Testing"
	};
	
	// Scene rendering methods
	void RenderMaterialTestingScene();
	void RenderAnimationTestingScene();
	void RenderLightingTestingScene();
	void RenderStateTestingScene();
	void RenderModelLoadingScene();
	void RenderPBRModelTestingScene();
	
	// Scene UI methods
	void RenderMaterialTestingUI();
	void RenderAnimationTestingUI();
	void RenderLightingTestingUI();
	void RenderStateTestingUI();
	void RenderModelLoadingUI();
	void RenderPBRModelTestingUI();
	
	// Helper methods
	OloEngine::Material& GetCurrentPBRMaterial();
	OloEngine::Material& GetCurrentAnimatedModelMaterial();

	// ECS Scene for model testing
	OloEngine::Ref<OloEngine::Scene> m_TestScene;
	OloEngine::Entity m_ImportedModelEntity;

	// Model selection
	int m_SelectedModelIndex = DEFAULT_SELECTED_MODEL_INDEX;
	int m_AnimatedModelMaterialType = DEFAULT_ANIMATED_MODEL_MATERIAL_TYPE;
	int m_CurrentAnimationIndex = DEFAULT_CURRENT_ANIMATION_INDEX;
	std::vector<std::string> m_AvailableModels = {
		"CesiumMan/CesiumMan.gltf",
		"Fox/Fox.gltf", 
		"RiggedSimple/RiggedSimple.gltf",
		"RiggedFigure/RiggedFigure.gltf",
		"SimpleSkin/SimpleSkin.gltf"
	};
	std::vector<std::string> m_ModelDisplayNames = {
		"CesiumMan (Test Character)",
		"Fox (Animated Animal)",
		"RiggedSimple (Basic)",
		"RiggedFigure (Complex)",
		"SimpleSkin (Minimal)"
	};
	
	float m_AnimationSpeed = 1.0f;
	
	// PBR Model selection for PBR Model Testing scene
	int m_SelectedPBRModelIndex = DEFAULT_SELECTED_PBR_MODEL_INDEX;
	std::vector<std::string> m_AvailablePBRModels = {
		"backpack/backpack.obj",
		"models/Cerberus/cerberus.fbx"
	};
	std::vector<std::string> m_PBRModelDisplayNames = {
		"Backpack (OBJ)",
		"Cerberus (FBX PBR)"
	};
	
	// Skeleton visualization settings
	bool m_ShowSkeleton = false;
	bool m_ShowBones = true;
	bool m_ShowJoints = true;
	float m_JointSize = DEFAULT_JOINT_SIZE;
	float m_BoneThickness = DEFAULT_BONE_THICKNESS;
	bool m_ModelWireframeMode = false; // Show model in wireframe to see skeleton through
	
	void RenderAnimationTestingPanel(); // New comprehensive animation testing UI
	
	// Helper functions
	void LoadTestAnimatedModel();
	void LoadTestPBRModel();
	
	// Scene lighting management
	void InitializeSceneLighting();
	void ApplySceneLighting(SceneType sceneType);
	void UpdateCurrentSceneLighting();
	
	// UI helper functions for different sections
	void RenderSceneSettings();
	void RenderLightingSettings();
	void RenderMaterialSettings();
	void RenderStateTestSettings();
	void RenderDebuggingTools();
	
	// UI helper functions for different light types
	void RenderDirectionalLightUI();
	void RenderPointLightUI();
	void RenderSpotlightUI();
	void RenderGraphDebuggerUI();
	void RenderDebuggingUI();
	void RenderStateTestObjects(f32 rotationAngle);

private:
	OloEngine::PerspectiveCameraController m_CameraController;

	// Mesh objects
	OloEngine::Ref<OloEngine::Mesh> m_CubeMesh;
	OloEngine::Ref<OloEngine::Mesh> m_SphereMesh;
	OloEngine::Ref<OloEngine::Mesh> m_PlaneMesh;
	
	// Model objects
	OloEngine::Ref<OloEngine::Model> m_BackpackModel;
	OloEngine::Ref<OloEngine::AnimatedModel> m_CesiumManModel;
	OloEngine::Ref<OloEngine::Model> m_CerberusModel;
	
	// Texture resources
	OloEngine::Ref<OloEngine::Texture2D> m_DiffuseMap;
	OloEngine::Ref<OloEngine::Texture2D> m_SpecularMap;
	OloEngine::Ref<OloEngine::Texture2D> m_GrassTexture;

	// Rotation animation state
	f32 m_RotationAngleY = 0.0f;
	f32 m_RotationAngleX = 0.0f;

	// Materials for different objects
	OloEngine::Material m_GoldMaterial;
	OloEngine::Material m_SilverMaterial;
	OloEngine::Material m_ChromeMaterial;
	OloEngine::Material m_TexturedMaterial;

	// PBR Materials for testing  
	OloEngine::Material m_PBRGoldMaterial;
	OloEngine::Material m_PBRSilverMaterial;
	OloEngine::Material m_PBRCopperMaterial;
	OloEngine::Material m_PBRPlasticMaterial;
	OloEngine::Material m_PBRRoughMaterial;
	OloEngine::Material m_PBRSmoothMaterial;
	
	// Environment map for IBL
	OloEngine::Ref<OloEngine::EnvironmentMap> m_EnvironmentMap;

	// Light properties (global for lighting test scene)
	OloEngine::Light m_Light;
	int m_LightTypeIndex = 0; // Default to directional light
	const char* m_LightTypeNames[3] = { "Directional Light", "Point Light", "Spotlight" };
	
	// Per-scene lighting configurations
	OloEngine::Light m_SceneLights[static_cast<int>(SceneType::Count)]; // One for each scene type
	
	// Material editor selection state
	int m_SelectedMaterial = 0;
	const char* m_MaterialNames[4] = { "Gold", "Silver", "Chrome", "Textured" };

	// PBR testing controls
	bool m_UsePBRMaterials = false;
	int m_PBRMaterialType = 0;
	const char* m_PBRMaterialNames[6] = { "PBR Gold", "PBR Silver", "PBR Copper", "PBR Plastic", "PBR Rough", "PBR Smooth" };

	// Light animation state
	f32 m_LightAnimTime = 0.0f;
	bool m_AnimateLight = true;

	// Input state tracking
	bool m_RotationEnabled = true;
	bool m_WasSpacePressed = false;
	bool m_CameraMovementEnabled = true;
	bool m_WasTabPressed = false;
	
	// Spotlight properties
	f32 m_SpotlightInnerAngle = 12.5f;
	f32 m_SpotlightOuterAngle = 17.5f;
	
	// Object type selection
	int m_PrimitiveTypeIndex = 0;
	const char* m_PrimitiveNames[3] = { "Cubes", "Spheres", "Mixed" };
	// FPS
	f32 m_FrameTime = 0.0f;
	f32 m_FPS = 0.0f;
	
	// Render Graph Debugger
	OloEngine::RenderGraphDebugger m_RenderGraphDebugger;
	bool m_RenderGraphDebuggerOpen = false;
	
	// Debugging Tools
	OloEngine::CommandPacketDebugger m_CommandPacketDebugger;
	OloEngine::RendererMemoryTracker& m_MemoryTracker = OloEngine::RendererMemoryTracker::GetInstance();
	OloEngine::RendererProfiler& m_RendererProfiler = OloEngine::RendererProfiler::GetInstance();
	OloEngine::GPUResourceInspector& m_GPUResourceInspector = OloEngine::GPUResourceInspector::GetInstance();
	OloEngine::ShaderDebugger& m_ShaderDebugger = OloEngine::ShaderDebugger::GetInstance();
	
	bool m_ShowCommandPacketDebugger = false;
	bool m_ShowMemoryTracker = false;
	bool m_ShowRendererProfiler = false;
	bool m_ShowGPUResourceInspector = false;
	bool m_ShowShaderDebugger = false;
	
	// State testing settings
	bool m_EnableStateTest = true;
	i32 m_StateTestMode = 0;
	const char* m_StateTestModes[4] = { "Wireframe", "Alpha Blend", "Polygon Offset", "All Effects" };
	bool m_UseQueuedStateChanges = true;
	
	// Common scene elements (shared across scenes)
	void RenderGroundPlane();
	void RenderGrassQuad();
	void RenderPerformanceInfo();
};

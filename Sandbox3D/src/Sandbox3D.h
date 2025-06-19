
#pragma once

#include "OloEngine.h"
#include "OloEngine/Renderer/Camera/PerspectiveCameraController.h"
#include "OloEngine/Renderer/Mesh.h"
#include "OloEngine/Renderer/Model.h"
#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Light.h"
#include "OloEngine/Renderer/TextureCubemap.h"
#include "OloEngine/Renderer/Debug/RenderGraphDebugger.h"
#include "OloEngine/Renderer/Debug/CommandPacketDebugger.h"
#include "OloEngine/Renderer/Debug/RendererMemoryTracker.h"
#include "OloEngine/Renderer/Debug/RendererProfiler.h"
#include "OloEngine/Renderer/Debug/GPUResourceInspector.h"
#include "OloEngine/Renderer/Debug/ShaderDebugger.h"

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

private:
	// Animated mesh ECS test (step 1)
	OloEngine::Ref<OloEngine::Mesh> m_AnimatedTestMesh;
	OloEngine::Ref<OloEngine::Skeleton> m_AnimatedTestSkeleton;
	OloEngine::AnimationStateComponent m_AnimatedTestAnimState;
	// UI helper functions for different sections
	void RenderPerformanceInfo();
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
	
	// Model object
	OloEngine::Ref<OloEngine::Model> m_BackpackModel;
	
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

	// Light properties
	OloEngine::Light m_Light;
	int m_LightTypeIndex = 1; // Default to point light
	const char* m_LightTypeNames[3] = { "Directional Light", "Point Light", "Spotlight" };
	
	// Material editor selection state
	int m_SelectedMaterial = 0;
	const char* m_MaterialNames[4] = { "Gold", "Silver", "Chrome", "Textured" };

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
};

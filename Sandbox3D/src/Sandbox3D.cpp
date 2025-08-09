#include <format>

#include <OloEnginePCH.h>
#include "Sandbox3D.h"
#include <imgui.h>
#include <GLFW/glfw3.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>

#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/MaterialPresets.h"
#include "OloEngine/Renderer/MeshPrimitives.h"
#include "OloEngine/Renderer/Light.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ShaderDebugUtils.h"
#include "OloEngine/Renderer/EnvironmentMap.h"


#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Animation/AnimationSystem.h"
#include "OloEngine/Animation/AnimationClip.h"
#include "OloEngine/Animation/AnimatedMeshComponents.h"

Sandbox3D::Sandbox3D()
    : Layer("Sandbox3D"),
    m_CameraController(45.0f, 1280.0f / 720.0f, 0.1f, 1000.0f)
{
    // Initialize materials with metallic properties
    m_GoldMaterial.Type = OloEngine::MaterialType::Legacy;  // Use legacy rendering path
    m_GoldMaterial.Ambient = glm::vec3(0.24725f, 0.1995f, 0.0745f);
    m_GoldMaterial.Diffuse = glm::vec3(0.75164f, 0.60648f, 0.22648f);
    m_GoldMaterial.Specular = glm::vec3(0.628281f, 0.555802f, 0.366065f);
    m_GoldMaterial.Shininess = 51.2f;
    m_GoldMaterial.UseTextureMaps = true;  // Enable texture mapping

    m_SilverMaterial.Type = OloEngine::MaterialType::Legacy;  // Use legacy rendering path
    m_SilverMaterial.Ambient = glm::vec3(0.19225f, 0.19225f, 0.19225f);
    m_SilverMaterial.Diffuse = glm::vec3(0.50754f, 0.50754f, 0.50754f);
    m_SilverMaterial.Specular = glm::vec3(0.508273f, 0.508273f, 0.508273f);
    m_SilverMaterial.Shininess = 76.8f;

    m_ChromeMaterial.Type = OloEngine::MaterialType::Legacy;  // Use legacy rendering path
    m_ChromeMaterial.Ambient = glm::vec3(0.25f, 0.25f, 0.25f);
    m_ChromeMaterial.Diffuse = glm::vec3(0.4f, 0.4f, 0.4f);
    m_ChromeMaterial.Specular = glm::vec3(0.774597f, 0.774597f, 0.774597f);
    m_ChromeMaterial.Shininess = 96.0f;

    // Initialize textured material
    m_TexturedMaterial.Type = OloEngine::MaterialType::Legacy;  // Use legacy rendering path
    m_TexturedMaterial.Ambient = glm::vec3(0.1f);
    m_TexturedMaterial.Diffuse = glm::vec3(1.0f);
    m_TexturedMaterial.Specular = glm::vec3(1.0f);
    m_TexturedMaterial.Shininess = 64.0f;
    m_TexturedMaterial.UseTextureMaps = true;
    
    // Initialize PBR materials with default values
    // These will be properly configured in OnAttach with Material factory methods
    // PBR Materials will be initialized as nullptr and created in OnAttach()
	// Initialize light with default values
    m_Light.Type = OloEngine::LightType::Directional;
    m_Light.Position = glm::vec3(1.2f, 1.0f, 2.0f);
    m_Light.Direction = glm::vec3(-0.2f, -1.0f, -0.3f); // Directional light direction
    m_Light.Ambient = glm::vec3(0.2f);
    m_Light.Diffuse = glm::vec3(0.8f);  // Increased diffuse for better visibility
    m_Light.Specular = glm::vec3(1.0f);
    
    // Point light attenuation defaults
    m_Light.Constant = 1.0f;
    m_Light.Linear = 0.09f;
    m_Light.Quadratic = 0.032f;
    
    // Spotlight defaults
    m_Light.CutOff = glm::cos(glm::radians(m_SpotlightInnerAngle));
    m_Light.OuterCutOff = glm::cos(glm::radians(m_SpotlightOuterAngle));
    
    // Initialize per-scene lighting
    InitializeSceneLighting();
}

void Sandbox3D::OnAttach()
{
    OLO_PROFILE_FUNCTION();
    
    // Initialize debugging tools FIRST before creating any resources
    OloEngine::RendererMemoryTracker::GetInstance().Initialize();
    OloEngine::RendererProfiler::GetInstance().Initialize();
    // Note: GPUResourceInspector is initialized in Application constructor
    
    // Create 3D meshes using MeshPrimitives for consistency
    m_CubeMesh = OloEngine::MeshPrimitives::CreateCube();
    m_SphereMesh = OloEngine::MeshPrimitives::CreateSphere();
    m_PlaneMesh = OloEngine::MeshPrimitives::CreatePlane(25.0f, 25.0f);

    // Load backpack model
    m_BackpackModel = OloEngine::Ref<OloEngine::Model>::Create("assets/backpack/backpack.obj");

    // Load textures
    OLO_CORE_INFO("Sandbox3D: Loading textures...");
    m_DiffuseMap = OloEngine::Texture2D::Create("assets/textures/container2.png");
    m_SpecularMap = OloEngine::Texture2D::Create("assets/textures/container2_specular.png");
    m_GrassTexture = OloEngine::Texture2D::Create("assets/textures/grass.png");

    // Assign textures to the materials
    m_TexturedMaterial.DiffuseMap = m_DiffuseMap;
    m_TexturedMaterial.SpecularMap = m_SpecularMap;
    
    // Also assign textures to gold material for the sphere
    m_GoldMaterial.DiffuseMap = m_DiffuseMap;
    m_GoldMaterial.SpecularMap = m_SpecularMap;
    // Initialize PBR materials using the new MaterialPresets utility
    m_PBRGoldMaterial = OloEngine::MaterialPresets::CreateGold("Gold Material");
    m_PBRSilverMaterial = OloEngine::MaterialPresets::CreateSilver("Silver Material");
    m_PBRCopperMaterial = OloEngine::MaterialPresets::CreateCopper("Copper Material");
    m_PBRPlasticMaterial = OloEngine::MaterialPresets::CreatePlastic("Blue Plastic", glm::vec3(0.1f, 0.1f, 0.8f));
    m_PBRRoughMaterial = *OloEngine::Material::CreatePBR("Rough Red", glm::vec3(0.8f, 0.2f, 0.2f), 0.0f, 0.9f);
    m_PBRSmoothMaterial = *OloEngine::Material::CreatePBR("Smooth Green", glm::vec3(0.2f, 0.8f, 0.2f), 0.0f, 0.1f);
    
    // Load environment map for IBL - using cubemap faces from OloEditor assets
    std::vector<std::string> skyboxFaces = {
        "assets/textures/Skybox/right.jpg",
        "assets/textures/Skybox/left.jpg", 
        "assets/textures/Skybox/top.jpg",
        "assets/textures/Skybox/bottom.jpg",
        "assets/textures/Skybox/front.jpg",
        "assets/textures/Skybox/back.jpg"
    };
    
    auto skyboxCubemap = OloEngine::TextureCubemap::Create(skyboxFaces);
    m_EnvironmentMap = OloEngine::EnvironmentMap::CreateFromCubemap(skyboxCubemap);
    
    // Configure IBL for all PBR materials
    if (m_EnvironmentMap && m_EnvironmentMap->HasIBL())
    {
        m_PBRGoldMaterial.ConfigureIBL(
            m_EnvironmentMap->GetEnvironmentMap(),
            m_EnvironmentMap->GetIrradianceMap(),
            m_EnvironmentMap->GetPrefilterMap(),
            m_EnvironmentMap->GetBRDFLutMap());
            
        m_PBRSilverMaterial.ConfigureIBL(
            m_EnvironmentMap->GetEnvironmentMap(),
            m_EnvironmentMap->GetIrradianceMap(), 
            m_EnvironmentMap->GetPrefilterMap(),
            m_EnvironmentMap->GetBRDFLutMap());
            
        m_PBRCopperMaterial.ConfigureIBL(
            m_EnvironmentMap->GetEnvironmentMap(),
            m_EnvironmentMap->GetIrradianceMap(),
            m_EnvironmentMap->GetPrefilterMap(), 
            m_EnvironmentMap->GetBRDFLutMap());
            
        m_PBRPlasticMaterial.ConfigureIBL(
            m_EnvironmentMap->GetEnvironmentMap(),
            m_EnvironmentMap->GetIrradianceMap(),
            m_EnvironmentMap->GetPrefilterMap(),
            m_EnvironmentMap->GetBRDFLutMap());
            
        m_PBRRoughMaterial.ConfigureIBL(
            m_EnvironmentMap->GetEnvironmentMap(),
            m_EnvironmentMap->GetIrradianceMap(), 
            m_EnvironmentMap->GetPrefilterMap(),
            m_EnvironmentMap->GetBRDFLutMap());
            
        m_PBRSmoothMaterial.ConfigureIBL(
            m_EnvironmentMap->GetEnvironmentMap(),
            m_EnvironmentMap->GetIrradianceMap(),
            m_EnvironmentMap->GetPrefilterMap(),
            m_EnvironmentMap->GetBRDFLutMap());
    }
    
    OloEngine::Renderer3D::SetLight(m_Light);
	
    m_TestScene = OloEngine::Ref<OloEngine::Scene>::Create();
    m_TestScene->OnRuntimeStart();
    
    LoadTestAnimatedModel();
    LoadTestPBRModel();
}

void Sandbox3D::OnDetach()
{
	OLO_PROFILE_FUNCTION();
    OloEngine::RendererMemoryTracker::GetInstance().Shutdown();
    OloEngine::RendererProfiler::GetInstance().Shutdown();
    // Note: GPUResourceInspector is shutdown in Application destructor
}

void Sandbox3D::OnUpdate(const OloEngine::Timestep ts)
{
    OLO_PROFILE_FUNCTION();

    m_FrameTime = ts.GetMilliseconds();
    m_FPS = 1.0f / ts.GetSeconds();
    OloEngine::RendererMemoryTracker::GetInstance().UpdateStats();

    // Update camera only if camera movement is enabled
    if (m_CameraMovementEnabled)
    {
        m_CameraController.OnUpdate(ts);
    }

    // Check for Tab key press to toggle camera movement
    bool tabPressed = OloEngine::Input::IsKeyPressed(OloEngine::Key::Tab);
    if (tabPressed && !m_WasTabPressed)
    {
        m_CameraMovementEnabled = !m_CameraMovementEnabled;
        if (m_CameraMovementEnabled)
            OLO_INFO("Camera movement enabled");
        else
            OLO_INFO("Camera movement disabled - UI mode active");
    }
    m_WasTabPressed = tabPressed;

    OloEngine::Renderer3D::SetViewPosition(m_CameraController.GetCamera().GetPosition());

    // Toggle rotation on spacebar press
    bool spacePressed = OloEngine::Input::IsKeyPressed(OloEngine::Key::Space);
    if (spacePressed && !m_WasSpacePressed)
        m_RotationEnabled = !m_RotationEnabled;
    m_WasSpacePressed = spacePressed;

    // Rotate only if enabled
    if (m_RotationEnabled)
    {
        m_RotationAngleY += ts * 45.0f;
        m_RotationAngleX += ts * 30.0f;

        // Keep angles in [0, 360)
        if (m_RotationAngleY > 360.0f)  m_RotationAngleY -= 360.0f;
        if (m_RotationAngleX > 360.0f)  m_RotationAngleX -= 360.0f;
    }

    // Animate the light position in a circular pattern (only for point and spot lights in lighting test scene)
    if (m_AnimateLight && m_Light.Type != OloEngine::LightType::Directional && m_CurrentScene == SceneType::LightingTesting)
    {
        m_LightAnimTime += ts;
        f32 radius = 3.0f;
        m_Light.Position.x = std::cos(m_LightAnimTime) * radius;
        m_Light.Position.z = std::sin(m_LightAnimTime) * radius;

        // For spotlights, make them always point toward the center
        if (m_Light.Type == OloEngine::LightType::Spot)
        {
            m_Light.Direction = -glm::normalize(m_Light.Position);
        }

        UpdateCurrentSceneLighting();
    }
	
    // Update animation for ALL animated entities in the scene
    if (m_TestScene)
    {
        auto animatedView = m_TestScene->GetAllEntitiesWith<OloEngine::AnimationStateComponent, OloEngine::SkeletonComponent>();
        
        for (auto entityID : animatedView)
        {
            OloEngine::Entity entity = { entityID, m_TestScene.get() };
            auto& animStateComp = entity.GetComponent<OloEngine::AnimationStateComponent>();
            auto& skeletonComp = entity.GetComponent<OloEngine::SkeletonComponent>();
            
            // For the current imported model entity, handle animation switching
            if (entity.HasComponent<OloEngine::TagComponent>() && 
                m_ImportedModelEntity.HasComponent<OloEngine::TagComponent>() &&
                entity.GetName() == m_ImportedModelEntity.GetName() && 
                m_CesiumManModel)
            {
                const auto& animations = m_CesiumManModel->GetAnimations();
                if (!animations.empty() && 
                    m_CurrentAnimationIndex >= 0 && 
                    m_CurrentAnimationIndex < static_cast<int>(animations.size()))
                {
                    std::string targetAnimation = animations[m_CurrentAnimationIndex]->Name;
                    if (!animStateComp.m_CurrentClip || 
                        animStateComp.m_CurrentClip->Name != targetAnimation)
                    {
                        auto newClip = m_CesiumManModel->GetAnimation(targetAnimation);
                        if (newClip)
                        {
                            animStateComp.m_CurrentClip = newClip;
                            animStateComp.m_CurrentTime = 0.0f;  // Reset timeline when switching animations
                        }
                    }
                }
            }
            
            if (animStateComp.m_CurrentClip)
            {
                OloEngine::Animation::AnimationSystem::Update(
                    animStateComp,
                    *skeletonComp.m_Skeleton,
                    ts.GetSeconds() * m_AnimationSpeed
                );
            }
        }
    }
	
    if (m_TestScene)
    {
        m_TestScene->OnUpdateRuntime(ts);
    }

    {
        OLO_PROFILE_SCOPE("Renderer Draw");
        OloEngine::Renderer3D::BeginScene(m_CameraController.GetCamera());
        
        // Render skybox first (background)
        if (m_EnvironmentMap && m_EnvironmentMap->GetEnvironmentMap())
        {
            auto* skyboxPacket = OloEngine::Renderer3D::DrawSkybox(m_EnvironmentMap->GetEnvironmentMap());
            if (skyboxPacket) 
            {
                OloEngine::Renderer3D::SubmitPacket(skyboxPacket);
            }
        }
        
        ApplySceneLighting(m_CurrentScene);
        
        RenderGroundPlane();
        
        switch (m_CurrentScene)
        {
            case SceneType::MaterialTesting:
                RenderMaterialTestingScene();
                break;
            case SceneType::AnimationTesting:
                RenderAnimationTestingScene();
                break;
            case SceneType::LightingTesting:
                RenderLightingTestingScene();
                break;
            case SceneType::StateTesting:
                RenderStateTestingScene();
                break;
            case SceneType::ModelLoading:
                RenderModelLoadingScene();
                break;
            case SceneType::PBRModelTesting:
                RenderPBRModelTestingScene();
                break;
        }
        
        OloEngine::Renderer3D::EndScene();
    }
}

void Sandbox3D::RenderGraphDebuggerUI()
{
    OLO_PROFILE_FUNCTION();
    
    if (m_RenderGraphDebuggerOpen)
    {
        // Get the renderer's active render graph
        auto renderGraph = OloEngine::Renderer3D::GetRenderGraph();
        if (renderGraph)
        {
            m_RenderGraphDebugger.RenderDebugView(renderGraph, &m_RenderGraphDebuggerOpen, "Render Graph");
        }
        else
        {
            ImGui::Begin("Render Graph", &m_RenderGraphDebuggerOpen);
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "No active render graph available!");
            if (ImGui::Button("Close"))
                m_RenderGraphDebuggerOpen = false;
            ImGui::End();
        }
    }
}

void Sandbox3D::OnImGuiRender()
{
    OLO_PROFILE_FUNCTION();

    // Render the RenderGraph debugger window if open
    RenderGraphDebuggerUI();

    ImGui::Begin("Settings & Controls");
    
    // Scene selector at the top
    if (ImGui::CollapsingHeader("Scene Selection", ImGuiTreeNodeFlags_DefaultOpen))
    {
        int currentSceneIndex = static_cast<int>(m_CurrentScene);
        
        // Ensure scene index is within valid bounds
        constexpr int sceneCount = static_cast<int>(SceneType::Count);
        static_assert(sizeof(m_SceneNames) / sizeof(m_SceneNames[0]) == sceneCount,
                      "Scene names array size must match SceneType::Count");
        if (currentSceneIndex < 0 || currentSceneIndex >= sceneCount)
        {
            currentSceneIndex = 0;
            m_CurrentScene = static_cast<SceneType>(currentSceneIndex);
        }
        
        if (ImGui::Combo("Active Scene", &currentSceneIndex, m_SceneNames, sceneCount))
        {
            SceneType newScene = static_cast<SceneType>(currentSceneIndex);
            // If switching to lighting test scene, load its saved light settings
            if (newScene == SceneType::LightingTesting)
            {
                m_Light = m_SceneLights[static_cast<int>(SceneType::LightingTesting)];
                m_LightTypeIndex = static_cast<int>(m_Light.Type);
            }
            
            m_CurrentScene = newScene;
        }
        ImGui::Separator();
    }
    
    // Performance info (always shown)
    if (ImGui::CollapsingHeader("Performance & Frame Info", ImGuiTreeNodeFlags_None))
    {
        RenderPerformanceInfo();
    }
    
    // Render scene-specific UI
    switch (m_CurrentScene)
    {
        case SceneType::MaterialTesting:
            RenderMaterialTestingUI();
            break;
        case SceneType::AnimationTesting:
            RenderAnimationTestingUI();
            break;
        case SceneType::LightingTesting:
            RenderLightingTestingUI();
            break;
        case SceneType::StateTesting:
            RenderStateTestingUI();
            break;
        case SceneType::ModelLoading:
            RenderModelLoadingUI();
            break;
        case SceneType::PBRModelTesting:
            RenderPBRModelTestingUI();
            break;
    }
    
    // Debugging tools available for all scenes
    if (ImGui::CollapsingHeader("Debugging Tools", ImGuiTreeNodeFlags_None))
    {
        RenderDebuggingTools();
    }

    ImGui::End();
}


void Sandbox3D::RenderPerformanceInfo()
{
    // Display frametime and FPS
    ImGui::Text("Frametime: %.2f ms", m_FrameTime);
    ImGui::Text("FPS: %.2f", m_FPS);
    
    // Add render graph button
    if (ImGui::Button("Show Render Graph"))
    {
        m_RenderGraphDebuggerOpen = true;
    }
    
    // Add camera control status indicator
    if (!m_CameraMovementEnabled)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Camera Movement: DISABLED");
        ImGui::Text("Press TAB to re-enable camera movement");
    }
}

void Sandbox3D::RenderGroundPlane()
{
    // Draw ground plane (always visible)
    auto planeMatrix = glm::mat4(1.0f);
    planeMatrix = glm::translate(planeMatrix, glm::vec3(0.0f, -1.0f, 0.0f));
    OloEngine::Material planeMaterial;
    planeMaterial.Ambient = glm::vec3(0.1f);
    planeMaterial.Diffuse = glm::vec3(0.3f);
    planeMaterial.Specular = glm::vec3(0.2f);
    planeMaterial.Shininess = 8.0f;
    auto* planePacket = OloEngine::Renderer3D::DrawMesh(m_PlaneMesh, planeMatrix, planeMaterial, true);
    if (planePacket) OloEngine::Renderer3D::SubmitPacket(planePacket);
}

void Sandbox3D::RenderGrassQuad()
{
    // Draw a grass quad (only in certain scenes)
    auto grassMatrix = glm::mat4(1.0f);
    grassMatrix = glm::translate(grassMatrix, glm::vec3(0.0f, 0.5f, -1.0f));
    grassMatrix = glm::rotate(grassMatrix, glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    auto* grassPacket = OloEngine::Renderer3D::DrawQuad(grassMatrix, m_GrassTexture);
    if (grassPacket) OloEngine::Renderer3D::SubmitPacket(grassPacket);
}

// === SCENE RENDERING METHODS ===

void Sandbox3D::RenderMaterialTestingScene()
{
    // Center rotating cube with wireframe overlay
    auto modelMatrix = glm::mat4(1.0f);
    modelMatrix = glm::rotate(modelMatrix, glm::radians(m_RotationAngleX), glm::vec3(1.0f, 0.0f, 0.0f));
    modelMatrix = glm::rotate(modelMatrix, glm::radians(m_RotationAngleY), glm::vec3(0.0f, 1.0f, 0.0f));
    
    // Choose material based on PBR toggle
    if (m_UsePBRMaterials)
	{
        auto& pbrMaterial = GetCurrentPBRMaterial();
        
        // Draw filled mesh (normal)
        auto* solidPacket = OloEngine::Renderer3D::DrawMesh(m_CubeMesh, modelMatrix, pbrMaterial);
        if (solidPacket) OloEngine::Renderer3D::SubmitPacket(solidPacket);
    }
	else
	{
        // Draw filled mesh (normal)
        auto* solidPacket = OloEngine::Renderer3D::DrawMesh(m_CubeMesh, modelMatrix, m_GoldMaterial);
        if (solidPacket) OloEngine::Renderer3D::SubmitPacket(solidPacket);
    }
    
    // Overlay wireframe (only if not using PBR for clarity)
    if (!m_UsePBRMaterials)
    {
        OloEngine::Material wireMaterial;
        wireMaterial.Ambient = glm::vec3(0.0f);
        wireMaterial.Diffuse = glm::vec3(0.0f, 0.0f, 0.0f);
        wireMaterial.Specular = glm::vec3(0.0f);
        wireMaterial.Shininess = 1.0f;
        auto* wirePacket = OloEngine::Renderer3D::DrawMesh(m_CubeMesh, modelMatrix, wireMaterial);
        if (wirePacket)
        {
            auto* drawCmd = wirePacket->GetCommandData<OloEngine::DrawMeshCommand>();
            drawCmd->renderState->PolygonMode.Mode = GL_LINE;
            drawCmd->renderState->LineWidth.Width = 2.5f;
            drawCmd->renderState->PolygonOffset.Enabled = true;
            drawCmd->renderState->PolygonOffset.Factor = -1.0f;
            drawCmd->renderState->PolygonOffset.Units = -1.0f;
            OloEngine::Renderer3D::SubmitPacket(wirePacket);
        }
    }

    // Draw objects based on primitive type
    switch (m_PrimitiveTypeIndex)
    {
        case 0: // Cubes
        {
            // Choose materials based on PBR toggle
            if (m_UsePBRMaterials)
			{
                OloEngine::Material silverMat = m_PBRSilverMaterial;
                OloEngine::Material chromeMat = m_PBRCopperMaterial;
                
                auto silverCubeMatrix = glm::mat4(1.0f);
                silverCubeMatrix = glm::translate(silverCubeMatrix, glm::vec3(2.0f, 0.0f, 0.0f));
                silverCubeMatrix = glm::rotate(silverCubeMatrix, glm::radians(m_RotationAngleY * 1.5f), glm::vec3(0.0f, 1.0f, 0.0f));
                auto* silverPacket = OloEngine::Renderer3D::DrawMesh(m_CubeMesh, silverCubeMatrix, silverMat);
                if (silverPacket) OloEngine::Renderer3D::SubmitPacket(silverPacket);

                auto chromeCubeMatrix = glm::mat4(1.0f);
                chromeCubeMatrix = glm::translate(chromeCubeMatrix, glm::vec3(-2.0f, 0.0f, 0.0f));
                chromeCubeMatrix = glm::rotate(chromeCubeMatrix, glm::radians(m_RotationAngleX * 1.5f), glm::vec3(1.0f, 0.0f, 0.0f));
                auto* chromePacket = OloEngine::Renderer3D::DrawMesh(m_CubeMesh, chromeCubeMatrix, chromeMat);
                if (chromePacket) OloEngine::Renderer3D::SubmitPacket(chromePacket);
            }
			else
			{
                auto silverCubeMatrix = glm::mat4(1.0f);
                silverCubeMatrix = glm::translate(silverCubeMatrix, glm::vec3(2.0f, 0.0f, 0.0f));
                silverCubeMatrix = glm::rotate(silverCubeMatrix, glm::radians(m_RotationAngleY * 1.5f), glm::vec3(0.0f, 1.0f, 0.0f));
                auto* silverPacket = OloEngine::Renderer3D::DrawMesh(m_CubeMesh, silverCubeMatrix, m_SilverMaterial);
                if (silverPacket) OloEngine::Renderer3D::SubmitPacket(silverPacket);

                auto chromeCubeMatrix = glm::mat4(1.0f);
                chromeCubeMatrix = glm::translate(chromeCubeMatrix, glm::vec3(-2.0f, 0.0f, 0.0f));
                chromeCubeMatrix = glm::rotate(chromeCubeMatrix, glm::radians(m_RotationAngleX * 1.5f), glm::vec3(1.0f, 0.0f, 0.0f));
                auto* chromePacket = OloEngine::Renderer3D::DrawMesh(m_CubeMesh, chromeCubeMatrix, m_ChromeMaterial);
                if (chromePacket) OloEngine::Renderer3D::SubmitPacket(chromePacket);
            }
            break;
        }

        case 1: // Spheres - PBR Material Showcase
        {
            // PBR materials showcase on spheres (LearnOpenGL style)
            if (m_UsePBRMaterials)
            {
                // Create a grid of spheres with varying metallic and roughness values
                int rows = 7;    // Different roughness values
                int cols = 7;    // Different metallic values
                float spacing = 2.5f;
                float startX = -(cols - 1) * spacing * 0.5f;
                float startZ = -(rows - 1) * spacing * 0.5f;
                
                for (int row = 0; row < rows; ++row)
                {
                    for (int col = 0; col < cols; ++col)
                    {
                        glm::vec3 position = glm::vec3(
                            startX + col * spacing,
                            0.0f,
                            startZ + row * spacing
                        );
                        
                        // Create material with varying metallic and roughness
                        float metallic = static_cast<float>(col) / static_cast<float>(cols - 1);
                        float roughness = static_cast<float>(row) / static_cast<float>(rows - 1);
                        roughness = glm::clamp(roughness, 0.05f, 1.0f); // Prevent completely smooth
                        
                        // Create dynamic material
                        OloEngine::Material dynamicMaterial = *OloEngine::Material::CreatePBR(
                            "Dynamic PBR",
                            glm::vec3(0.5f, 0.0f, 0.0f), // Red base color
                            metallic,
                            roughness
                        );
                        
                        // Configure IBL if available
                        if (m_EnvironmentMap && m_EnvironmentMap->HasIBL())
                        {
                            dynamicMaterial.ConfigureIBL(
                                m_EnvironmentMap->GetEnvironmentMap(),
                                m_EnvironmentMap->GetIrradianceMap(),
                                m_EnvironmentMap->GetPrefilterMap(),
                                m_EnvironmentMap->GetBRDFLutMap());
                        }
                        
                        auto sphereMatrix = glm::mat4(1.0f);
                        sphereMatrix = glm::translate(sphereMatrix, position);
                        sphereMatrix = glm::scale(sphereMatrix, glm::vec3(0.8f));
                        
                        auto* packet = OloEngine::Renderer3D::DrawMesh(m_SphereMesh, sphereMatrix, dynamicMaterial);
                        if (packet) OloEngine::Renderer3D::SubmitPacket(packet);
                    }
                }
                
                // Add some preset material spheres around the edges for comparison
                std::vector<std::pair<OloEngine::Material, glm::vec3>> presetMaterials = {
                    { m_PBRGoldMaterial, glm::vec3(-12.0f, 2.0f, 0.0f) },
                    { m_PBRSilverMaterial, glm::vec3(12.0f, 2.0f, 0.0f) },
                    { m_PBRCopperMaterial, glm::vec3(0.0f, 2.0f, -12.0f) },
                    { m_PBRPlasticMaterial, glm::vec3(0.0f, 2.0f, 12.0f) }
                };
                
                for (const auto& preset : presetMaterials)
                {
                    auto sphereMatrix = glm::mat4(1.0f);
                    sphereMatrix = glm::translate(sphereMatrix, preset.second);
                    sphereMatrix = glm::scale(sphereMatrix, glm::vec3(1.2f)); // Slightly larger
                    
                    auto* packet = OloEngine::Renderer3D::DrawMesh(m_SphereMesh, sphereMatrix, preset.first);
                    if (packet) OloEngine::Renderer3D::SubmitPacket(packet);
                }
            }
            else
            {
                // Original sphere arrangement for non-PBR materials
                auto centerGoldMatrix = glm::mat4(1.0f);
                auto* goldPacket = OloEngine::Renderer3D::DrawMesh(m_SphereMesh, centerGoldMatrix, m_GoldMaterial);
                if (goldPacket) OloEngine::Renderer3D::SubmitPacket(goldPacket);

                auto silverSphereMatrix = glm::mat4(1.0f);
                silverSphereMatrix = glm::translate(silverSphereMatrix, glm::vec3(2.0f, 0.0f, 0.0f));
                auto* silverPacket = OloEngine::Renderer3D::DrawMesh(m_SphereMesh, silverSphereMatrix, m_SilverMaterial);
                if (silverPacket) OloEngine::Renderer3D::SubmitPacket(silverPacket);

                auto chromeSphereMatrix = glm::mat4(1.0f);
                chromeSphereMatrix = glm::translate(chromeSphereMatrix, glm::vec3(-2.0f, 0.0f, 0.0f));
                auto* chromePacket = OloEngine::Renderer3D::DrawMesh(m_SphereMesh, chromeSphereMatrix, m_ChromeMaterial);
                if (chromePacket) OloEngine::Renderer3D::SubmitPacket(chromePacket);
            }
            break;
        }

        case 2: // Mixed
        default:
        {
            auto silverSphereMatrix = glm::mat4(1.0f);
            silverSphereMatrix = glm::translate(silverSphereMatrix, glm::vec3(2.0f, 0.0f, 0.0f));
            auto* silverPacket = OloEngine::Renderer3D::DrawMesh(m_SphereMesh, silverSphereMatrix, m_SilverMaterial);
            if (silverPacket) OloEngine::Renderer3D::SubmitPacket(silverPacket);

            auto chromeCubeMatrix = glm::mat4(1.0f);
            chromeCubeMatrix = glm::translate(chromeCubeMatrix, glm::vec3(-2.0f, 0.0f, 0.0f));
            chromeCubeMatrix = glm::rotate(chromeCubeMatrix, glm::radians(m_RotationAngleX * 1.5f), glm::vec3(1.0f, 0.0f, 0.0f));
            auto* chromePacket = OloEngine::Renderer3D::DrawMesh(m_CubeMesh, chromeCubeMatrix, m_ChromeMaterial);
            if (chromePacket) OloEngine::Renderer3D::SubmitPacket(chromePacket);
            break;
        }
    }

    // Textured sphere (shared across all modes)
    auto sphereMatrix = glm::mat4(1.0f);
    sphereMatrix = glm::translate(sphereMatrix, glm::vec3(0.0f, 2.0f, 0.0f));
    sphereMatrix = glm::rotate(sphereMatrix, glm::radians(m_RotationAngleX * 0.8f), glm::vec3(1.0f, 0.0f, 0.0f));
    sphereMatrix = glm::rotate(sphereMatrix, glm::radians(m_RotationAngleY * 0.8f), glm::vec3(0.0f, 1.0f, 0.0f));
    auto* texturedPacket = OloEngine::Renderer3D::DrawMesh(m_SphereMesh, sphereMatrix, m_TexturedMaterial);
    if (texturedPacket)
	{
		OloEngine::Renderer3D::SubmitPacket(texturedPacket);
	}
		
    
    // Add grass quad to demonstrate texture rendering
    RenderGrassQuad();
}

void Sandbox3D::RenderAnimationTestingScene()
{
    // Render animated entities using the new ECS animation system
    if (m_TestScene)
    {
        // Use the new animation rendering system that handles MeshComponent + SkeletonComponent entities
        OloEngine::Material defaultMaterial = *OloEngine::Material::CreatePBR("Default Animation", glm::vec3(0.8f), 0.0f, 0.5f);
        OloEngine::Renderer3D::RenderAnimatedMeshes(m_TestScene, defaultMaterial);
        
        // Render skeleton visualization if enabled
        if (m_ShowSkeleton && m_ImportedModelEntity.HasComponent<OloEngine::SkeletonComponent>())
        {
            auto& skeletonComp = m_ImportedModelEntity.GetComponent<OloEngine::SkeletonComponent>();
            auto& transformComp = m_ImportedModelEntity.GetComponent<OloEngine::TransformComponent>();
            
            if (skeletonComp.m_Skeleton)
            {
                glm::mat4 modelMatrix = transformComp.GetTransform();
                OloEngine::Renderer3D::DrawSkeleton(
                    *skeletonComp.m_Skeleton, 
                    modelMatrix,
                    m_ShowBones, 
                    m_ShowJoints, 
                    m_JointSize, 
                    m_BoneThickness
                );
            }
        }
    }
}

void Sandbox3D::RenderLightingTestingScene()
{
    // Simple objects to demonstrate lighting
    auto cubeMatrix = glm::mat4(1.0f);
    cubeMatrix = glm::rotate(cubeMatrix, glm::radians(m_RotationAngleY * 0.5f), glm::vec3(0.0f, 1.0f, 0.0f));
    auto* cubePacket = OloEngine::Renderer3D::DrawMesh(m_CubeMesh, cubeMatrix, m_GoldMaterial);
    if (cubePacket) OloEngine::Renderer3D::SubmitPacket(cubePacket);
    
    auto sphereMatrix = glm::mat4(1.0f);
    sphereMatrix = glm::translate(sphereMatrix, glm::vec3(3.0f, 0.0f, 0.0f));
    auto* spherePacket = OloEngine::Renderer3D::DrawMesh(m_SphereMesh, sphereMatrix, m_SilverMaterial);
    if (spherePacket) OloEngine::Renderer3D::SubmitPacket(spherePacket);
    
    auto texturedSphereMatrix = glm::mat4(1.0f);
    texturedSphereMatrix = glm::translate(texturedSphereMatrix, glm::vec3(-3.0f, 0.0f, 0.0f));
    auto* texturedSpherePacket = OloEngine::Renderer3D::DrawMesh(m_SphereMesh, texturedSphereMatrix, m_TexturedMaterial);
    if (texturedSpherePacket) OloEngine::Renderer3D::SubmitPacket(texturedSpherePacket);

    // Light cube (only for point and spot lights)
    if (m_Light.Type != OloEngine::LightType::Directional)
    {
        auto lightCubeModelMatrix = glm::mat4(1.0f);
        lightCubeModelMatrix = glm::translate(lightCubeModelMatrix, m_Light.Position);
        lightCubeModelMatrix = glm::scale(lightCubeModelMatrix, glm::vec3(0.2f));
        auto* lightCubePacket = OloEngine::Renderer3D::DrawLightCube(lightCubeModelMatrix);
        if (lightCubePacket) OloEngine::Renderer3D::SubmitPacket(lightCubePacket);
    }
}

void Sandbox3D::RenderStateTestingScene()
{
    if (m_EnableStateTest)
    {
        RenderStateTestObjects(m_RotationAngleY);
    }
}

void Sandbox3D::RenderModelLoadingScene()
{
    // Draw backpack model
    auto modelMatrix = glm::mat4(1.0f);
    modelMatrix = glm::translate(modelMatrix, glm::vec3(0.0f, 1.0f, -2.0f));
    modelMatrix = glm::scale(modelMatrix, glm::vec3(0.5f));
    modelMatrix = glm::rotate(modelMatrix, glm::radians(m_RotationAngleY), glm::vec3(0.0f, 1.0f, 0.0f));
    std::vector<OloEngine::CommandPacket*> backpackDrawCommands;
    m_BackpackModel->GetDrawCommands(modelMatrix, m_TexturedMaterial, backpackDrawCommands);
    for (auto* drawCmd : backpackDrawCommands)
    {
        OloEngine::Renderer3D::SubmitPacket(drawCmd);
    }
}

// === SCENE UI METHODS ===

void Sandbox3D::RenderMaterialTestingUI()
{
    if (ImGui::CollapsingHeader("Scene Settings", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Combo("Primitive Types", &m_PrimitiveTypeIndex, m_PrimitiveNames, 3);
        ImGui::Separator();
        
        ImGui::Text("Frustum Culling");
        ImGui::Indent();
        
        bool frustumCullingEnabled = OloEngine::Renderer3D::IsFrustumCullingEnabled();
        if (ImGui::Checkbox("Enable Frustum Culling", &frustumCullingEnabled))
        {
            OloEngine::Renderer3D::EnableFrustumCulling(frustumCullingEnabled);
        }
        
        bool dynamicCullingEnabled = OloEngine::Renderer3D::IsDynamicCullingEnabled();
        if (ImGui::Checkbox("Cull Dynamic Objects", &dynamicCullingEnabled))
        {
            OloEngine::Renderer3D::EnableDynamicCulling(dynamicCullingEnabled);
        }
        
        ImGui::Unindent();
    }

    if (ImGui::CollapsingHeader("Material Settings", ImGuiTreeNodeFlags_DefaultOpen))
    {
        RenderMaterialSettings();
    }
}

void Sandbox3D::RenderAnimationTestingUI()
{
    if (ImGui::CollapsingHeader("Model Loading", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextWrapped("Load and test glTF animated models with PBR materials and skeletal animation.");
        ImGui::Separator();
        
        // Model selection dropdown
        if (ImGui::Combo("Select Model", &m_SelectedModelIndex, 
                        [](void* data, int idx, const char** out_text) {
                            auto* names = static_cast<std::vector<std::string>*>(data);
                            if (idx >= 0 && idx < static_cast<int>(names->size())) {
                                *out_text = (*names)[idx].c_str();
                                return true;
                            }
                            return false;
                        }, &m_ModelDisplayNames, static_cast<int>(m_ModelDisplayNames.size())))
        {
            // Model selection changed, reload if needed
        }
        
        if (ImGui::Button("Load Selected Model"))
        {
            LoadTestAnimatedModel();
        }
        ImGui::SameLine();
        
        ImGui::SliderFloat("Animation Speed", &m_AnimationSpeed, 0.1f, 3.0f, "%.1f");
        
        ImGui::Separator();
        
        // Show information about the loaded model and its materials
        if (m_CesiumManModel)
        {
            ImGui::Text("Status: Loaded - %s", m_ModelDisplayNames[m_SelectedModelIndex].c_str());
            ImGui::Text("Meshes: %zu, Animations: %zu", 
                       m_CesiumManModel->GetMeshes().size(), 
                       m_CesiumManModel->GetAnimations().size());
            ImGui::Text("Materials: %zu", m_CesiumManModel->GetMaterials().size());
            
            // Dynamic animation switching - only show if model has multiple animations
            const auto& animations = m_CesiumManModel->GetAnimations();
            if (animations.size() > 1)
            {
                ImGui::Separator();
                ImGui::Text("Animation Controls:");
                
                // Create dropdown items for animations
                std::vector<const char*> animationNames;
                for (const auto& anim : animations)
                {
                    animationNames.push_back(anim->Name.c_str());
                }
                
                if (ImGui::Combo("Select Animation", &m_CurrentAnimationIndex, 
                                animationNames.data(), static_cast<int>(animationNames.size())))
                {
                    // Animation selection changed - the OnUpdate method will handle the actual switching
                }
                
                // Show current animation info
                if (m_CurrentAnimationIndex >= 0 && m_CurrentAnimationIndex < static_cast<int>(animations.size()))
                {
                    const auto& currentAnim = animations[m_CurrentAnimationIndex];
                    ImGui::Text("Duration: %.2f seconds", currentAnim->Duration);
                    
                    if (m_ImportedModelEntity.HasComponent<OloEngine::AnimationStateComponent>())
                    {
                        auto& animState = m_ImportedModelEntity.GetComponent<OloEngine::AnimationStateComponent>();
                        ImGui::Text("Progress: %.2f / %.2f", animState.m_CurrentTime, currentAnim->Duration);
                        
                        // Reset button
                        if (ImGui::Button("Reset Animation"))
                        {
                            animState.m_CurrentTime = 0.0f;
                        }
                        ImGui::SameLine();
                        
                        // Play/Pause toggle
                        const char* playPauseText = (m_AnimationSpeed > 0.0f) ? "Pause" : "Play";
                        if (ImGui::Button(playPauseText))
                        {
                            m_AnimationSpeed = (m_AnimationSpeed > 0.0f) ? 0.0f : 1.0f;
                        }
                    }
                }
            }
            
            // Skeleton visualization controls
            if (m_ImportedModelEntity.HasComponent<OloEngine::SkeletonComponent>())
            {
                ImGui::Separator();
                ImGui::Text("Skeleton Visualization:");
                
                ImGui::Checkbox("Show Skeleton", &m_ShowSkeleton);
                
                if (m_ShowSkeleton)
                {
                    ImGui::Indent();
                    ImGui::Checkbox("Show Bones", &m_ShowBones);
                    ImGui::Checkbox("Show Joints", &m_ShowJoints);
                    ImGui::SliderFloat("Joint Size", &m_JointSize, 0.005f, 0.1f, "%.3f");
                    ImGui::SliderFloat("Bone Thickness", &m_BoneThickness, 0.5f, 5.0f, "%.1f");
                    ImGui::Separator();
                    ImGui::Text("Visibility Options:");
                    ImGui::Checkbox("Wireframe Model", &m_ModelWireframeMode);
                    ImGui::SameLine();
                    if (ImGui::Button("?"))
                    {
                        ImGui::SetTooltip("Show model in wireframe to see skeleton through the mesh");
                    }
                    ImGui::TextWrapped("Note: Skeleton now renders on top with disabled depth testing for maximum visibility!");
                    ImGui::Unindent();
                }
            }
            
            // Show material information
            if (!m_CesiumManModel->GetMaterials().empty())
            {
                ImGui::Separator();
                ImGui::Text("Model Materials:");
                for (size_t i = 0; i < m_CesiumManModel->GetMaterials().size(); ++i)
                {
                    const auto& material = m_CesiumManModel->GetMaterials()[i];
                    ImGui::Text("  [%zu] %s", i, material.Name.c_str());
                    ImGui::Text("    Base Color: (%.2f, %.2f, %.2f)", 
                               material.BaseColorFactor.r, material.BaseColorFactor.g, material.BaseColorFactor.b);
                    ImGui::Text("    Metallic: %.2f, Roughness: %.2f", 
                               material.MetallicFactor, material.RoughnessFactor);
                    if (material.AlbedoMap)
                        ImGui::Text("    Has Albedo Map");
                    if (material.NormalMap)
                        ImGui::Text("    Has Normal Map");
                    if (material.MetallicRoughnessMap)
                        ImGui::Text("    Has Metallic-Roughness Map");
                }
            }
                       
            if (m_ImportedModelEntity.HasComponent<OloEngine::AnimationStateComponent>())
            {
                auto& animState = m_ImportedModelEntity.GetComponent<OloEngine::AnimationStateComponent>();
                ImGui::Separator();
                ImGui::Text("Animation: %s", animState.m_CurrentClip ? animState.m_CurrentClip->Name.c_str() : "None");
                ImGui::Text("Time: %.2f / %.2f", animState.m_CurrentTime, 
                           animState.m_CurrentClip ? animState.m_CurrentClip->Duration : 0.0f);
            }
        }
        else
        {
            ImGui::Text("Status: Not Loaded");
        }
        
        ImGui::TextWrapped("These are glTF test models with skeletal animation demonstrating PBR + Animation integration.");
    }
}

void Sandbox3D::RenderLightingTestingUI()
{
    if (ImGui::CollapsingHeader("Lighting Settings", ImGuiTreeNodeFlags_DefaultOpen))
    {
        RenderLightingSettings();
    }
}

void Sandbox3D::RenderStateTestingUI()
{
    if (ImGui::CollapsingHeader("State Management Test", ImGuiTreeNodeFlags_DefaultOpen))
    {
        RenderStateTestSettings();
    }
}

void Sandbox3D::RenderModelLoadingUI()
{
    if (ImGui::CollapsingHeader("Model Loading", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextWrapped("This scene demonstrates loading and rendering 3D models.");
        ImGui::Separator();
        
        ImGui::Text("Current Model: Backpack");
        ImGui::Text("Model loaded: %s", m_BackpackModel ? "Yes" : "No");
        
        ImGui::Separator();
        
        if (ImGui::Button("Reload Model"))
        {
            m_BackpackModel = OloEngine::Ref<OloEngine::Model>::Create("assets/backpack/backpack.obj");
        }
    }
}

void Sandbox3D::RenderDirectionalLightUI()
{
    // Direction control
    if (bool directionChanged = ImGui::DragFloat3("Direction##DirLight", glm::value_ptr(m_Light.Direction), 0.01f); directionChanged)
    {
        // Normalize direction
        if (glm::length(m_Light.Direction) > 0.0f)
        {
            m_Light.Direction = glm::normalize(m_Light.Direction);
        }
        else
        {
            m_Light.Direction = glm::vec3(0.0f, -1.0f, 0.0f);
        }

        OloEngine::Renderer3D::SetLight(m_Light);
        UpdateCurrentSceneLighting();
    }
    
    // Light colors
    bool lightChanged = false;
    lightChanged |= ImGui::ColorEdit3("Ambient##DirLight", glm::value_ptr(m_Light.Ambient));
    lightChanged |= ImGui::ColorEdit3("Diffuse##DirLight", glm::value_ptr(m_Light.Diffuse));
    lightChanged |= ImGui::ColorEdit3("Specular##DirLight", glm::value_ptr(m_Light.Specular));

    if (lightChanged)
    {
        UpdateCurrentSceneLighting();
    }
}

void Sandbox3D::RenderPointLightUI()
{
    if (!m_AnimateLight)
    {
        // Position control (only if not animating)
        bool positionChanged = ImGui::DragFloat3("Position##PointLight", glm::value_ptr(m_Light.Position), 0.1f);
        if (positionChanged)
        {
            UpdateCurrentSceneLighting();
        }
    }
    
    // Light colors
    bool lightChanged = false;
    lightChanged |= ImGui::ColorEdit3("Ambient##PointLight", glm::value_ptr(m_Light.Ambient));
    lightChanged |= ImGui::ColorEdit3("Diffuse##PointLight", glm::value_ptr(m_Light.Diffuse));
    lightChanged |= ImGui::ColorEdit3("Specular##PointLight", glm::value_ptr(m_Light.Specular));
    
    // Attenuation factors
    ImGui::Text("Attenuation Factors");
    lightChanged |= ImGui::DragFloat("Constant##PointLight", &m_Light.Constant, 0.01f, 0.1f, 10.0f);
    lightChanged |= ImGui::DragFloat("Linear##PointLight", &m_Light.Linear, 0.001f, 0.0f, 1.0f);
    lightChanged |= ImGui::DragFloat("Quadratic##PointLight", &m_Light.Quadratic, 0.0001f, 0.0f, 1.0f);

    if (lightChanged)
    {
        UpdateCurrentSceneLighting();
    }
}

void Sandbox3D::RenderSpotlightUI()
{
    if (!m_AnimateLight)
    {
        // Position control (only if not animating)
        if (bool positionChanged = ImGui::DragFloat3("Position##Spotlight", glm::value_ptr(m_Light.Position), 0.1f); positionChanged)
        {
            UpdateCurrentSceneLighting();
        }
        
        // Direction control (only if not animating)
        if (bool directionChanged = ImGui::DragFloat3("Direction##Spotlight", glm::value_ptr(m_Light.Direction), 0.01f); directionChanged)
        {
            // Normalize direction
            if (glm::length(m_Light.Direction) > 0.0f)
            {
                m_Light.Direction = glm::normalize(m_Light.Direction);
            }
            else
            {
                m_Light.Direction = glm::vec3(0.0f, -1.0f, 0.0f);
            }

            UpdateCurrentSceneLighting();
        }
    }
    else
    {
        ImGui::Text("Light Direction: Auto (points to center)");
    }
    
    // Light colors
    bool lightChanged = false;
    lightChanged |= ImGui::ColorEdit3("Ambient##Spotlight", glm::value_ptr(m_Light.Ambient));
    lightChanged |= ImGui::ColorEdit3("Diffuse##Spotlight", glm::value_ptr(m_Light.Diffuse));
    lightChanged |= ImGui::ColorEdit3("Specular##Spotlight", glm::value_ptr(m_Light.Specular));
    
    // Attenuation factors
    ImGui::Text("Attenuation Factors");
    lightChanged |= ImGui::DragFloat("Constant##Spotlight", &m_Light.Constant, 0.01f, 0.1f, 10.0f);
    lightChanged |= ImGui::DragFloat("Linear##Spotlight", &m_Light.Linear, 0.001f, 0.0f, 1.0f);
    lightChanged |= ImGui::DragFloat("Quadratic##Spotlight", &m_Light.Quadratic, 0.0001f, 0.0f, 1.0f);
    
    // Spotlight cutoff angles
    ImGui::Text("Spotlight Angles");
    bool cutoffChanged = false;
    cutoffChanged |= ImGui::SliderFloat("Inner Cone", &m_SpotlightInnerAngle, 0.0f, 90.0f);
    cutoffChanged |= ImGui::SliderFloat("Outer Cone", &m_SpotlightOuterAngle, 0.0f, 90.0f);
    
    if (cutoffChanged)
    {
        // Make sure inner angle is less than or equal to outer angle
        m_SpotlightInnerAngle = std::min(m_SpotlightInnerAngle, m_SpotlightOuterAngle);
        
        // Convert angles to cosines
        m_Light.CutOff = glm::cos(glm::radians(m_SpotlightInnerAngle));
        m_Light.OuterCutOff = glm::cos(glm::radians(m_SpotlightOuterAngle));
        
        lightChanged = true;
    }

    if (lightChanged)
    {
        UpdateCurrentSceneLighting();
    }
}

OloEngine::Material& Sandbox3D::GetCurrentPBRMaterial()
{
    switch (m_PBRMaterialType)
    {
        case 0: return m_PBRGoldMaterial;
        case 1: return m_PBRSilverMaterial;
        case 2: return m_PBRCopperMaterial;
        case 3: return m_PBRPlasticMaterial;
        case 4: return m_PBRRoughMaterial;
        case 5: return m_PBRSmoothMaterial;
        default: return m_PBRGoldMaterial;
    }
}

OloEngine::Material& Sandbox3D::GetCurrentAnimatedModelMaterial()
{
    switch (m_AnimatedModelMaterialType)
    {
        case 0: return m_PBRSilverMaterial;   // Default: Silver for good contrast
        case 1: return m_PBRGoldMaterial;
        case 2: return m_PBRCopperMaterial;
        case 3: return m_PBRPlasticMaterial;
        case 4: return m_PBRRoughMaterial;
        case 5: return m_PBRSmoothMaterial;
        default: return m_PBRSilverMaterial;
    }
}

void Sandbox3D::OnEvent(OloEngine::Event& e)
{
    // Only process camera events if camera movement is enabled
    if (m_CameraMovementEnabled)
    {
        m_CameraController.OnEvent(e);
    }

    if (e.GetEventType() == OloEngine::EventType::KeyPressed)
    {
        auto const& keyEvent = static_cast<OloEngine::KeyPressedEvent&>(e);
        if (keyEvent.GetKeyCode() == OloEngine::Key::Escape)
        {
            OloEngine::Application::Get().Close();
        }
    }
}

void Sandbox3D::RenderStateTestObjects(f32 rotationAngle)
{
    // Position our state test objects in a specific area
    const glm::vec3 stateTestPosition(0.0f, 3.0f, 3.0f);
    // Draw a marker sphere to indicate where the state test area is
    {
        auto markerMatrix = glm::mat4(1.0f);
        markerMatrix = glm::translate(markerMatrix, stateTestPosition + glm::vec3(0.0f, 1.0f, 0.0f));
        markerMatrix = glm::scale(markerMatrix, glm::vec3(0.2f));
        OloEngine::Material markerMaterial;
        markerMaterial.Ambient = glm::vec3(1.0f, 0.0f, 0.0f);
        markerMaterial.Diffuse = glm::vec3(1.0f, 0.0f, 0.0f);
        markerMaterial.Specular = glm::vec3(1.0f);
        markerMaterial.Shininess = 32.0f;
        auto* markerPacket = OloEngine::Renderer3D::DrawMesh(m_SphereMesh, markerMatrix, markerMaterial);
        if (markerPacket) OloEngine::Renderer3D::SubmitPacket(markerPacket);
    }
    switch (m_StateTestMode)
    {
        case 0: // Wireframe mode
        {
            for (int i = 0; i < 3; i++)
            {
                auto cubeMatrix = glm::mat4(1.0f);
                cubeMatrix = glm::translate(cubeMatrix, stateTestPosition + glm::vec3(i - 1, 0.0f, 0.0f));
                cubeMatrix = glm::rotate(cubeMatrix, glm::radians(rotationAngle), glm::vec3(0.0f, 1.0f, 0.0f));
                OloEngine::Material cubeMaterial;
                cubeMaterial.Ambient = glm::vec3(0.1f);
                cubeMaterial.Diffuse = glm::vec3((i + 1) * 0.25f, 0.5f, 0.7f);
                cubeMaterial.Specular = glm::vec3(0.5f);
                cubeMaterial.Shininess = 32.0f;
                auto* packet = OloEngine::Renderer3D::DrawMesh(m_CubeMesh, cubeMatrix, cubeMaterial);
                if (packet)
                {
                    auto* drawCmd = packet->GetCommandData<OloEngine::DrawMeshCommand>();
                    drawCmd->renderState->PolygonMode.Mode = GL_LINE;
                    drawCmd->renderState->LineWidth.Width = 2.0f + i;
                    OloEngine::Renderer3D::SubmitPacket(packet);
                }
                
            }
            break;
        }
        case 1: // Alpha blending mode
        {
            for (int i = 0; i < 3; i++)
            {
                auto sphereMatrix = glm::mat4(1.0f);
                sphereMatrix = glm::translate(sphereMatrix, stateTestPosition + glm::vec3((i - 1) * 0.5f, 0.0f, 0.0f));
                sphereMatrix = glm::scale(sphereMatrix, glm::vec3(0.6f));
                OloEngine::Material sphereMaterial;
                sphereMaterial.Ambient = glm::vec3(0.1f);
                switch (i) {
                    case 0: sphereMaterial.Diffuse = glm::vec3(1.0f, 0.0f, 0.0f); break;
                    case 1: sphereMaterial.Diffuse = glm::vec3(0.0f, 1.0f, 0.0f); break;
                    case 2: sphereMaterial.Diffuse = glm::vec3(0.0f, 0.0f, 1.0f); break;
                }
                sphereMaterial.Specular = glm::vec3(0.5f);
                sphereMaterial.Shininess = 32.0f;
                auto* packet = OloEngine::Renderer3D::DrawMesh(m_SphereMesh, sphereMatrix, sphereMaterial);
                if (packet) {
                    auto* drawCmd = packet->GetCommandData<OloEngine::DrawMeshCommand>();
                    drawCmd->renderState->Blend.Enabled = true;
                    drawCmd->renderState->Blend.SrcFactor = GL_SRC_ALPHA;
                    drawCmd->renderState->Blend.DstFactor = GL_ONE_MINUS_SRC_ALPHA;
                    OloEngine::Renderer3D::SubmitPacket(packet);
                }
            }
            break;
        }
        case 2: // Polygon offset test
        {
            auto cubeMatrix = glm::mat4(1.0f);
            cubeMatrix = glm::translate(cubeMatrix, stateTestPosition);
            cubeMatrix = glm::rotate(cubeMatrix, glm::radians(rotationAngle), glm::vec3(0.0f, 1.0f, 0.0f));
            cubeMatrix = glm::scale(cubeMatrix, glm::vec3(0.8f));
            OloEngine::Material solidMaterial;
            solidMaterial.Ambient = glm::vec3(0.1f);
            solidMaterial.Diffuse = glm::vec3(0.7f, 0.7f, 0.2f);
            solidMaterial.Specular = glm::vec3(0.5f);
            solidMaterial.Shininess = 32.0f;
            auto* solidPacket = OloEngine::Renderer3D::DrawMesh(m_CubeMesh, cubeMatrix, solidMaterial);
            if (solidPacket) OloEngine::Renderer3D::SubmitPacket(solidPacket);

            // Overlay wireframe
            OloEngine::Material wireMaterial;
            wireMaterial.Ambient = glm::vec3(0.0f);
            wireMaterial.Diffuse = glm::vec3(0.0f, 0.0f, 0.0f);
            wireMaterial.Specular = glm::vec3(0.0f);
            wireMaterial.Shininess = 1.0f;
            auto* wirePacket = OloEngine::Renderer3D::DrawMesh(m_CubeMesh, cubeMatrix, wireMaterial);
            if (wirePacket) {
                auto* drawCmd = wirePacket->GetCommandData<OloEngine::DrawMeshCommand>();
                drawCmd->renderState->PolygonMode.Mode = GL_LINE;
                drawCmd->renderState->LineWidth.Width = 1.5f;
                drawCmd->renderState->PolygonOffset.Enabled = true;
                drawCmd->renderState->PolygonOffset.Factor = -1.0f;
                drawCmd->renderState->PolygonOffset.Units = -1.0f;
                OloEngine::Renderer3D::SubmitPacket(wirePacket);
            }
            break;
        }
        case 3: // Combined effects
        {
            // Example: central wireframe sphere
            auto sphereMatrix = glm::mat4(1.0f);
            sphereMatrix = glm::translate(sphereMatrix, stateTestPosition);
            sphereMatrix = glm::rotate(sphereMatrix, glm::radians(rotationAngle), glm::vec3(0.0f, 1.0f, 0.0f));
            OloEngine::Material wireMaterial;
            wireMaterial.Ambient = glm::vec3(0.1f);
            wireMaterial.Diffuse = glm::vec3(1.0f, 1.0f, 0.0f);
            wireMaterial.Specular = glm::vec3(1.0f);
            wireMaterial.Shininess = 32.0f;
            auto* wirePacket = OloEngine::Renderer3D::DrawMesh(m_SphereMesh, sphereMatrix, wireMaterial);
            if (wirePacket) {
                auto* drawCmd = wirePacket->GetCommandData<OloEngine::DrawMeshCommand>();
                drawCmd->renderState->PolygonMode.Mode = GL_LINE;
                drawCmd->renderState->LineWidth.Width = 2.0f;
                OloEngine::Renderer3D::SubmitPacket(wirePacket);
            }
            // Transparent cubes around sphere
            for (int i = 0; i < 3; i++)
            {
                f32 angle = glm::radians(rotationAngle + i * 120.0f);
                glm::vec3 offset(std::cos(angle), 0.0f, std::sin(angle));
                auto cubeMatrix = glm::mat4(1.0f);
                cubeMatrix = glm::translate(cubeMatrix, stateTestPosition + offset * 1.5f);
                cubeMatrix = glm::rotate(cubeMatrix, angle, glm::vec3(0.0f, 1.0f, 0.0f));
                cubeMatrix = glm::scale(cubeMatrix, glm::vec3(0.4f));
                OloEngine::Material glassMaterial;
                glassMaterial.Ambient = glm::vec3(0.1f);
                switch (i) {
                    case 0: glassMaterial.Diffuse = glm::vec3(1.0f, 0.0f, 0.0f); break;
                    case 1: glassMaterial.Diffuse = glm::vec3(0.0f, 1.0f, 0.0f); break;
                    case 2: glassMaterial.Diffuse = glm::vec3(0.0f, 0.0f, 1.0f); break;
                }
                glassMaterial.Specular = glm::vec3(0.8f);
                glassMaterial.Shininess = 64.0f;
                auto* glassPacket = OloEngine::Renderer3D::DrawMesh(m_CubeMesh, cubeMatrix, glassMaterial);
                if (glassPacket) {
                    auto* drawCmd = glassPacket->GetCommandData<OloEngine::DrawMeshCommand>();
                    drawCmd->renderState->Blend.Enabled = true;
                    drawCmd->renderState->Blend.SrcFactor = GL_SRC_ALPHA;
                    drawCmd->renderState->Blend.DstFactor = GL_ONE_MINUS_SRC_ALPHA;
                    OloEngine::Renderer3D::SubmitPacket(glassPacket);
                }
            }
            break;
        }
    }
}

void Sandbox3D::LoadTestAnimatedModel()
{
    OLO_PROFILE_FUNCTION();
    
    if (m_ImportedModelEntity)
    {
        m_TestScene->DestroyEntity(m_ImportedModelEntity);
        m_ImportedModelEntity = {};
    }
    
    std::string modelPath = "assets/models/" + m_AvailableModels[m_SelectedModelIndex];
    std::string modelName = m_ModelDisplayNames[m_SelectedModelIndex];
    
    m_CurrentAnimationIndex = 0;
    
    OLO_INFO("Sandbox3D: Loading animated model: {}", modelName);
    
    try 
    {
        m_CesiumManModel = OloEngine::Ref<OloEngine::AnimatedModel>::Create(modelPath);
        
        if (!m_CesiumManModel->HasSkeleton())
        {
            OLO_WARN("{} model does not have a skeleton, using default", modelName);
        }
        
        if (!m_CesiumManModel->HasAnimations())
        {
            OLO_WARN("{} model does not have animations", modelName);
        }
        
        m_ImportedModelEntity = m_TestScene->CreateEntity(modelName);
        
        // Position the model with model-specific scaling adjustments
        auto& transformComp = m_ImportedModelEntity.GetComponent<OloEngine::TransformComponent>();
        transformComp.Translation = glm::vec3(0.0f, 0.0f, 0.0f);
        
        // Apply model-specific scaling corrections
        // Apply scaling based on the model
        glm::vec3 modelScale = glm::vec3(1.0f);
        
        // Special scaling for Fox model
        if (modelName.find("Fox") != std::string::npos)
        {
            modelScale = glm::vec3(0.01f);
        }
        
        transformComp.Scale = modelScale;
        
        // Create MeshComponent from AnimatedModel data (now uses MeshSource with separated bone influences)
        auto& meshComp = m_ImportedModelEntity.AddComponent<OloEngine::MeshComponent>();
        if (!m_CesiumManModel->GetMeshes().empty())
        {
            // Get the first MeshSource with separated bone influences (Hazel approach)
            meshComp.m_MeshSource = m_CesiumManModel->GetMeshes()[0];
            OLO_INFO("MeshComponent created with MeshSource containing {} submeshes and separated bone influences", 
                     meshComp.m_MeshSource->GetSubmeshes().size());
            
            // Create a child entity with SubmeshComponent for rendering
            auto submeshEntity = m_TestScene->CreateEntity(modelName + "_Submesh_0");
            auto& submeshComp = submeshEntity.AddComponent<OloEngine::SubmeshComponent>();
            
            // Create a regular Mesh from the MeshSource for the SubmeshComponent
            auto mesh = OloEngine::Ref<OloEngine::Mesh>::Create(meshComp.m_MeshSource, 0);
            submeshComp.m_Mesh = mesh;
            submeshComp.m_SubmeshIndex = 0;
            submeshComp.m_Visible = true;
            
            // Set up parent-child relationship
            submeshEntity.SetParent(m_ImportedModelEntity);
            
            OLO_INFO("Successfully created SubmeshComponent using MeshSource with separated bone influences");
        }
        else
        {
            OLO_ERROR("{} model has no meshes!", modelName);
            return;
        }
        
        auto& materialComp = m_ImportedModelEntity.AddComponent<OloEngine::MaterialComponent>();
        if (!m_CesiumManModel->GetMaterials().empty())
        {
            // Use the first material from the model (corresponds to first mesh)
            materialComp.m_Material = m_CesiumManModel->GetMaterials()[0];
            OLO_INFO("Using original material: {}", materialComp.m_Material.Name);
        }
        else
        {
            // Fallback to a default material
            auto defaultMaterialRef = OloEngine::Material::CreatePBR("Default Material", glm::vec3(0.8f), 0.0f, 0.5f);
            if (defaultMaterialRef)
            {
                materialComp.m_Material = *defaultMaterialRef;
            }
            else
            {
                OLO_ERROR("Failed to create default PBR material, material component will be invalid");
            }
            OLO_WARN("No materials found in model, using default material");
        }
        
        auto& skeletonComp = m_ImportedModelEntity.AddComponent<OloEngine::SkeletonComponent>();
        if (m_CesiumManModel->HasSkeleton())
        {
            skeletonComp.m_Skeleton = m_CesiumManModel->GetSkeleton();
            OLO_INFO("Skeleton loaded: {} bones, {} parents, {} transforms", 
                     skeletonComp.m_Skeleton->m_BoneNames.size(),
                     skeletonComp.m_Skeleton->m_ParentIndices.size(),
                     skeletonComp.m_Skeleton->m_GlobalTransforms.size());
        }

        // Add animation state component
        auto& animStateComp = m_ImportedModelEntity.AddComponent<OloEngine::AnimationStateComponent>();
        if (m_CesiumManModel->HasAnimations())
        {
            // Debug: List all animations available
            OLO_INFO("Available animations for {}:", modelName);
            for (sizet i = 0; i < m_CesiumManModel->GetAnimations().size(); i++)
            {
                auto& anim = m_CesiumManModel->GetAnimations()[i];
                OLO_INFO("  Animation [{}]: '{}' - Duration: {:.2f}s", i, anim->Name, anim->Duration);
            }
            
            // Use the first animation by default
            int animIndex = 0;
            
            animStateComp.m_CurrentClip = m_CesiumManModel->GetAnimations()[animIndex];
            OLO_INFO("Selected animation: {}", animStateComp.m_CurrentClip->Name);

			m_CurrentAnimationIndex = animIndex;
        }
        animStateComp.m_State = OloEngine::AnimationStateComponent::State::Idle;
        animStateComp.m_CurrentTime = 0.0f;
        
        OLO_INFO("Sandbox3D: Successfully loaded {} model with {} meshes, {} animations", 
                 modelName, m_CesiumManModel->GetMeshes().size(), m_CesiumManModel->GetAnimations().size());
    }
    catch (const std::exception& e)
    {
        OLO_ERROR("Failed to load {} model: {}", modelName, e.what());
        
        // Create a simple fallback entity
        m_ImportedModelEntity = m_TestScene->CreateEntity(modelName + " (Fallback)");
        auto& transformComp = m_ImportedModelEntity.GetComponent<OloEngine::TransformComponent>();
        transformComp.Translation = glm::vec3(0.0f, 0.0f, 0.0f);
        transformComp.Scale = glm::vec3(1.0f);
        
        // Note: Would need to add fallback mesh and components here if needed
        OLO_WARN("Using minimal fallback entity for failed model load");
    }
}

void Sandbox3D::RenderMaterialSettings()
{
    if (ImGui::Checkbox("Use PBR Materials", &m_UsePBRMaterials))
    {
        if (m_UsePBRMaterials)
        {
            m_PrimitiveTypeIndex = 1;
        }
    }
    
    ImGui::Separator();
    
    if (m_UsePBRMaterials)
    {
        ImGui::Text("PBR Material Showcase");
        ImGui::TextWrapped("Switch to 'Spheres' mode to see all PBR materials arranged in a circle:");
        
        // PBR Material information
        ImGui::Text("Available PBR Materials:");
        const size_t materialCount = sizeof(m_PBRMaterialNames) / sizeof(m_PBRMaterialNames[0]);
        for (size_t i = 0; i < materialCount; ++i)
        {
            ImGui::BulletText("%s", m_PBRMaterialNames[i]);
        }
        
        ImGui::Separator();
        
        ImGui::Text("PBR Material Properties:");
        
        // Ensure PBR material type is within valid bounds
        constexpr int pbrMaterialCount = 6; // Should match array size
        static_assert(sizeof(m_PBRMaterialNames) / sizeof(m_PBRMaterialNames[0]) == pbrMaterialCount,
                      "PBR material names array size mismatch");
        if (m_PBRMaterialType < 0 || m_PBRMaterialType >= pbrMaterialCount)
            m_PBRMaterialType = 0;
            
        ImGui::Combo("Select PBR Material", &m_PBRMaterialType, m_PBRMaterialNames, pbrMaterialCount);
        
        // Get the selected PBR material
        auto& currentPBRMaterial = GetCurrentPBRMaterial();
        
        // Edit PBR properties
        ImGui::ColorEdit3("Base Color", glm::value_ptr(currentPBRMaterial.BaseColorFactor));
        ImGui::SliderFloat("Metallic", &currentPBRMaterial.MetallicFactor, 0.0f, 1.0f);
        ImGui::SliderFloat("Roughness", &currentPBRMaterial.RoughnessFactor, 0.01f, 1.0f);
        ImGui::SliderFloat("Normal Scale", &currentPBRMaterial.NormalScale, 0.0f, 2.0f);
        ImGui::SliderFloat("Occlusion Strength", &currentPBRMaterial.OcclusionStrength, 0.0f, 1.0f);
        ImGui::ColorEdit3("Emissive", glm::value_ptr(currentPBRMaterial.EmissiveFactor));
        
        ImGui::Separator();
        ImGui::Text("Environment Mapping (IBL):");
        if (m_EnvironmentMap)
        {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Status: Loaded & Active");
            if (m_EnvironmentMap->HasIBL())
            {
                ImGui::BulletText("Environment Map: Loaded");
                ImGui::BulletText("Irradiance Map: Generated");
                ImGui::BulletText("Prefilter Map: Generated");
                ImGui::BulletText("BRDF LUT: Generated");
            }
            
            // Show IBL status for current material
            auto& materialForIBLCheck = GetCurrentPBRMaterial();
            if (materialForIBLCheck.EnableIBL)
            {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "IBL: Enabled for current material");
            }
            else
            {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "IBL: Disabled for current material");
            }
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Status: Not loaded (IBL disabled)");
            ImGui::TextWrapped("Load an environment map to enable realistic reflections and ambient lighting.");
        }
    }
    else
    {
        // Original material settings
        ImGui::Combo("Select Material", &m_SelectedMaterial, m_MaterialNames, 4);

        // Get the selected material based on the combo box selection
        OloEngine::Material* currentMaterial;
        switch (m_SelectedMaterial)
        {
            case 0:
                currentMaterial = &m_GoldMaterial;
                break;
            case 1:
                currentMaterial = &m_SilverMaterial;
                break;
            case 2:
                currentMaterial = &m_ChromeMaterial;
                break;
            case 3:
                currentMaterial = &m_TexturedMaterial;
                break;
            default:
                currentMaterial = &m_GoldMaterial;
        }

        // Edit the selected material
        if (m_SelectedMaterial == 3)
        {
            ImGui::Text("Textured Material Properties");
            ImGui::SliderFloat("Shininess", &currentMaterial->Shininess, 1.0f, 128.0f);
            
            if (m_DiffuseMap)
                ImGui::Text("Diffuse Map: Loaded");
            else
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Diffuse Map: Not Found!");
            
            if (m_SpecularMap)
                ImGui::Text("Specular Map: Loaded");
            else 
                ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Specular Map: Not Found!");
        }
        else
        {
            // For solid color materials, show the color controls
            ImGui::ColorEdit3(std::format("Ambient##Material{}", m_SelectedMaterial).c_str(), glm::value_ptr(currentMaterial->Ambient));
            ImGui::ColorEdit3(std::format("Diffuse##Material{}", m_SelectedMaterial).c_str(), glm::value_ptr(currentMaterial->Diffuse));
            ImGui::ColorEdit3(std::format("Specular##Material{}", m_SelectedMaterial).c_str(), glm::value_ptr(currentMaterial->Specular));
            ImGui::SliderFloat(std::format("Shininess##Material{}", m_SelectedMaterial).c_str(), &currentMaterial->Shininess, 1.0f, 128.0f);
        }
    }
}

void Sandbox3D::RenderLightingSettings()
{
    if (bool lightTypeChanged = ImGui::Combo("Light Type", &m_LightTypeIndex, m_LightTypeNames, 3); lightTypeChanged)
    {
        // Update light type
        m_Light.Type = static_cast<OloEngine::LightType>(m_LightTypeIndex);

        // Disable animation for directional lights
        if (m_Light.Type == OloEngine::LightType::Directional && m_AnimateLight)
        {
            m_AnimateLight = false;
        }

        UpdateCurrentSceneLighting();
    }
    
    // Show different UI controls based on light type
    ImGui::Separator();
    
    using enum OloEngine::LightType;
    switch (m_Light.Type)
    {
        case Directional:
            RenderDirectionalLightUI();
            break;

        case Point:
            // Only show animation toggle for positional lights
            ImGui::Checkbox("Animate Light", &m_AnimateLight);
            RenderPointLightUI();
            break;

        case Spot:
            // Only show animation toggle for positional lights
            ImGui::Checkbox("Animate Light", &m_AnimateLight);
            RenderSpotlightUI();
            break;
    }
}

void Sandbox3D::RenderStateTestSettings()
{
    ImGui::Checkbox("Enable State Test", &m_EnableStateTest);
    
    if (m_EnableStateTest)
    {
        ImGui::Combo("Test Mode", &m_StateTestMode, m_StateTestModes, 4);
        ImGui::Checkbox("Use Queued State Changes", &m_UseQueuedStateChanges);
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::Text("This option doesn't do anything yet - we're always using the queue now");
            ImGui::EndTooltip();
        }
    }
}

void Sandbox3D::RenderDebuggingTools()
{
    // Command Packet Debugger
    if (ImGui::CollapsingHeader("Command Packet Debugger", ImGuiTreeNodeFlags_None))
    {
        ImGui::Checkbox("Show Command Packets##CommandDebugger", &m_ShowCommandPacketDebugger);
        ImGui::SameLine();
        if (ImGui::Button("Export to CSV##CommandDebugger"))
        {
            const auto* commandBucket = OloEngine::Renderer3D::GetCommandBucket();
            if (commandBucket)
            {
                m_CommandPacketDebugger.ExportToCSV(commandBucket, "command_packets.csv");
            }
        }
        
        if (m_ShowCommandPacketDebugger)
        {
            const auto* commandBucket = OloEngine::Renderer3D::GetCommandBucket();
            if (commandBucket)
            {
                m_CommandPacketDebugger.RenderDebugView(commandBucket, &m_ShowCommandPacketDebugger, "Command Packets");
            }
        }
    }
    
    // Memory Tracker
    if (ImGui::CollapsingHeader("Memory Tracker", ImGuiTreeNodeFlags_None))
    {
        ImGui::Checkbox("Show Memory Stats##MemoryTracker", &m_ShowMemoryTracker);
        ImGui::SameLine();
        if (ImGui::Button("Reset Stats##MemoryTracker"))
        {
            m_MemoryTracker.Reset();
        }
        
        if (m_ShowMemoryTracker)
        {
            m_MemoryTracker.RenderUI(&m_ShowMemoryTracker);
        }
    }
    
    // Renderer Profiler
    if (ImGui::CollapsingHeader("Renderer Profiler", ImGuiTreeNodeFlags_None))
    {
        ImGui::Checkbox("Show Profiler##RendererProfiler", &m_ShowRendererProfiler);
        ImGui::SameLine();
        if (ImGui::Button("Reset Profiler##RendererProfiler"))
        {
            m_RendererProfiler.Reset();
        }
        
        if (m_ShowRendererProfiler)
        {
            m_RendererProfiler.RenderUI(&m_ShowRendererProfiler);
        }
    }
    
    // GPU Resource Inspector
    if (ImGui::CollapsingHeader("GPU Resource Inspector", ImGuiTreeNodeFlags_None))
    {
        ImGui::Checkbox("Show GPU Resources##GPUResourceInspector", &m_ShowGPUResourceInspector);
        
        if (m_ShowGPUResourceInspector)
        {
            m_GPUResourceInspector.RenderDebugView(&m_ShowGPUResourceInspector, "GPU Resource Inspector");
        }
    }

    // Shader Debugger
    if (ImGui::CollapsingHeader("Shader Debugger", ImGuiTreeNodeFlags_None))
    {
        ImGui::Checkbox("Show Shader Debugger##ShaderDebugger", &m_ShowShaderDebugger);
        
        if (m_ShowShaderDebugger)
        {
            m_ShaderDebugger.RenderDebugView(&m_ShowShaderDebugger, "Shader Debugger");
        }
    }
}

// === SCENE LIGHTING MANAGEMENT ===

void Sandbox3D::InitializeSceneLighting()
{
    // Material Testing Scene - Simple directional light for material showcase
    m_SceneLights[static_cast<int>(SceneType::MaterialTesting)].Type = OloEngine::LightType::Directional;
    m_SceneLights[static_cast<int>(SceneType::MaterialTesting)].Direction = glm::vec3(-0.2f, -1.0f, -0.3f);
    m_SceneLights[static_cast<int>(SceneType::MaterialTesting)].Ambient = glm::vec3(0.2f);
    m_SceneLights[static_cast<int>(SceneType::MaterialTesting)].Diffuse = glm::vec3(0.8f);
    m_SceneLights[static_cast<int>(SceneType::MaterialTesting)].Specular = glm::vec3(1.0f);
    
    // Animation Testing Scene - Bright directional light for clear animation visibility
    m_SceneLights[static_cast<int>(SceneType::AnimationTesting)].Type = OloEngine::LightType::Directional;
    m_SceneLights[static_cast<int>(SceneType::AnimationTesting)].Direction = glm::vec3(-0.3f, -1.0f, -0.2f);
    m_SceneLights[static_cast<int>(SceneType::AnimationTesting)].Ambient = glm::vec3(0.3f);
    m_SceneLights[static_cast<int>(SceneType::AnimationTesting)].Diffuse = glm::vec3(0.9f);
    m_SceneLights[static_cast<int>(SceneType::AnimationTesting)].Specular = glm::vec3(0.8f);
    
    // Lighting Testing Scene - Uses current m_Light (user-configurable)
    m_SceneLights[static_cast<int>(SceneType::LightingTesting)] = m_Light;
    
    // State Testing Scene - Simple lighting to focus on rendering states
    m_SceneLights[static_cast<int>(SceneType::StateTesting)].Type = OloEngine::LightType::Directional;
    m_SceneLights[static_cast<int>(SceneType::StateTesting)].Direction = glm::vec3(0.0f, -1.0f, 0.0f);
    m_SceneLights[static_cast<int>(SceneType::StateTesting)].Ambient = glm::vec3(0.25f);
    m_SceneLights[static_cast<int>(SceneType::StateTesting)].Diffuse = glm::vec3(0.7f);
    m_SceneLights[static_cast<int>(SceneType::StateTesting)].Specular = glm::vec3(0.6f);
    
    // Model Loading Scene - Point light to showcase 3D model details
    m_SceneLights[static_cast<int>(SceneType::ModelLoading)].Type = OloEngine::LightType::Point;
    m_SceneLights[static_cast<int>(SceneType::ModelLoading)].Position = glm::vec3(2.0f, 3.0f, 2.0f);
    m_SceneLights[static_cast<int>(SceneType::ModelLoading)].Ambient = glm::vec3(0.2f);
    m_SceneLights[static_cast<int>(SceneType::ModelLoading)].Diffuse = glm::vec3(0.8f);
    m_SceneLights[static_cast<int>(SceneType::ModelLoading)].Specular = glm::vec3(1.0f);
    m_SceneLights[static_cast<int>(SceneType::ModelLoading)].Constant = 1.0f;
    m_SceneLights[static_cast<int>(SceneType::ModelLoading)].Linear = 0.09f;
    m_SceneLights[static_cast<int>(SceneType::ModelLoading)].Quadratic = 0.032f;
    
    // PBR Model Testing Scene - Directional light optimized for PBR materials
    m_SceneLights[static_cast<int>(SceneType::PBRModelTesting)].Type = OloEngine::LightType::Directional;
    m_SceneLights[static_cast<int>(SceneType::PBRModelTesting)].Direction = glm::vec3(-0.4f, -1.0f, -0.3f);
    m_SceneLights[static_cast<int>(SceneType::PBRModelTesting)].Ambient = glm::vec3(0.3f);
    m_SceneLights[static_cast<int>(SceneType::PBRModelTesting)].Diffuse = glm::vec3(1.0f);
    m_SceneLights[static_cast<int>(SceneType::PBRModelTesting)].Specular = glm::vec3(1.0f);
}

void Sandbox3D::ApplySceneLighting(SceneType sceneType)
{
    if (sceneType == SceneType::LightingTesting)
    {
        // For lighting testing scene, use the configurable light
        OloEngine::Renderer3D::SetLight(m_Light);
    }
    else
    {
        // For other scenes, use their predefined lighting
        OloEngine::Renderer3D::SetLight(m_SceneLights[static_cast<int>(sceneType)]);
    }
}

void Sandbox3D::UpdateCurrentSceneLighting()
{
    // Update the lighting testing scene's saved state when user makes changes
    if (m_CurrentScene == SceneType::LightingTesting)
    {
        m_SceneLights[static_cast<int>(SceneType::LightingTesting)] = m_Light;
    }
}

void Sandbox3D::RenderPBRModelTestingScene()
{
    // Render the selected PBR model
    if (m_SelectedPBRModelIndex == 0 && m_BackpackModel)
    {
        // Render Backpack model using its own materials
        auto modelMatrix = glm::mat4(1.0f);
        
        // Position the model above the ground plane to prevent intersection
        modelMatrix = glm::translate(modelMatrix, glm::vec3(0.0f, 1.5f, 0.0f));
        
        modelMatrix = glm::rotate(modelMatrix, glm::radians(m_RotationAngleY), glm::vec3(0.0f, 1.0f, 0.0f));
        
        std::vector<OloEngine::CommandPacket*> drawCommands;
        m_BackpackModel->GetDrawCommands(modelMatrix, drawCommands);
        
        for (auto* cmd : drawCommands)
        {
            if (cmd) OloEngine::Renderer3D::SubmitPacket(cmd);
        }
    }
    else if (m_SelectedPBRModelIndex == 1 && m_CerberusModel)
    {
        // Render Cerberus model using its own materials
        auto modelMatrix = glm::mat4(1.0f);
        
        // Position the model above the ground plane
        modelMatrix = glm::translate(modelMatrix, glm::vec3(0.0f, 1.0f, 0.0f));
        
        // Apply user rotation
        modelMatrix = glm::rotate(modelMatrix, glm::radians(m_RotationAngleY), glm::vec3(0.0f, 1.0f, 0.0f));
        
        // Add initial rotation to orient the model properly (many FBX models need this)
        modelMatrix = glm::rotate(modelMatrix, glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f)); // Rotate around X-axis
        
        // Scale the model to appropriate size
        modelMatrix = glm::scale(modelMatrix, glm::vec3(0.02f));
        
        std::vector<OloEngine::CommandPacket*> drawCommands;
        m_CerberusModel->GetDrawCommands(modelMatrix, drawCommands);
        
        for (auto* cmd : drawCommands)
        {
            if (cmd) OloEngine::Renderer3D::SubmitPacket(cmd);
        }
    }
}

void Sandbox3D::RenderPBRModelTestingUI()
{
    ImGui::Text("PBR Model Testing");
    ImGui::Separator();
    
    // Model selection - convert std::vector<std::string> to const char* array
    std::vector<const char*> modelNames;
    for (const auto& name : m_PBRModelDisplayNames)
    {
        modelNames.push_back(name.c_str());
    }
    
    if (ImGui::Combo("Select PBR Model", &m_SelectedPBRModelIndex, 
                     modelNames.data(), static_cast<int>(modelNames.size())))
    {
        LoadTestPBRModel();
    }
    
    ImGui::Separator();
    
    // Model information
    if (m_SelectedPBRModelIndex == 0)
    {
        ImGui::Text("Model: Backpack (OBJ format)");
        ImGui::Text("Type: Static mesh with basic materials");
        ImGui::Text("Loaded: %s", m_BackpackModel ? "Yes" : "No");
    }
    else if (m_SelectedPBRModelIndex == 1)
    {
        ImGui::Text("Model: Cerberus (FBX format)");
        ImGui::Text("Type: PBR model with full texture set");
        ImGui::Text("Loaded: %s", m_CerberusModel ? "Yes" : "No");
        
        if (m_CerberusModel)
        {
            const auto& materials = m_CerberusModel->GetMaterials();
            ImGui::Text("Materials: %d", static_cast<int>(materials.size()));
            
            for (size_t i = 0; i < materials.size(); i++)
            {
                if (!materials[i]) continue;
                
                ImGui::Text("  Material %d: %s", static_cast<int>(i), materials[i]->Name.c_str());
                ImGui::Text("    Base Color: (%.2f, %.2f, %.2f)", 
                           materials[i]->BaseColorFactor.r, materials[i]->BaseColorFactor.g, materials[i]->BaseColorFactor.b);
                ImGui::Text("    Metallic: %.2f, Roughness: %.2f", 
                           materials[i]->MetallicFactor, materials[i]->RoughnessFactor);
                ImGui::Text("    Albedo: Has texture: %s", materials[i]->AlbedoMap ? "Yes" : "No");
                ImGui::Text("    Normal: Has texture: %s", materials[i]->NormalMap ? "Yes" : "No");
                ImGui::Text("    Metallic: Has texture: %s", materials[i]->MetallicRoughnessMap ? "Yes" : "No");
                ImGui::Text("    AO: Has texture: %s", materials[i]->AOMap ? "Yes" : "No");
                ImGui::Text("    IBL: Environment: %s, Irradiance: %s", 
                           materials[i]->EnvironmentMap ? "Yes" : "No",
                           materials[i]->IrradianceMap ? "Yes" : "No");
            }
            
            ImGui::Separator();
            ImGui::Text("Rendering Info:");
            ImGui::Text("IBL Available: %s", (m_EnvironmentMap && m_EnvironmentMap->HasIBL()) ? "Yes" : "No");
            ImGui::Text("Position: Above ground plane (Y=1.0)");
            ImGui::Text("Orientation: Rotated -90° on X-axis for proper upright display");
            ImGui::Text("Scale: 0.02x (properly sized - model should be fully visible)");
            ImGui::Text("Tip: Use WASDQE to move camera, mouse to look around");
        }
    }
    
    ImGui::Separator();
    
    // Rotation controls
    if (ImGui::SliderFloat("Model Rotation", &m_RotationAngleY, 0.0f, 360.0f))
    {
        // Rotation updated
    }
    
    if (ImGui::Button("Reset Rotation"))
    {
        m_RotationAngleY = 0.0f;
    }
}

void Sandbox3D::LoadTestPBRModel()
{
    std::string assetPath = "assets/" + m_AvailablePBRModels[m_SelectedPBRModelIndex];
    
    if (m_SelectedPBRModelIndex == 0)
    {
        // Load Backpack
        OLO_INFO("Loading Backpack model from: {}", assetPath);
        m_BackpackModel = OloEngine::Ref<OloEngine::Model>::Create(assetPath);
        m_CerberusModel.Reset(); // Clear other model
    }
    else if (m_SelectedPBRModelIndex == 1)
    {
        // Load Cerberus with texture overrides
        OLO_INFO("Loading Cerberus model from: {}", assetPath);
        
        // Create texture override configuration for Cerberus
        OloEngine::TextureOverride cerberusTextures;
        cerberusTextures.AlbedoPath = "assets/models/Cerberus/cerberus_A.png";
        cerberusTextures.MetallicPath = "assets/models/Cerberus/cerberus_M.png";
        cerberusTextures.NormalPath = "assets/models/Cerberus/cerberus_N.png";
        cerberusTextures.RoughnessPath = "assets/models/Cerberus/cerberus_R.png";
        cerberusTextures.AOPath = "assets/models/Cerberus/cerberus_R.png";
        
        // Validate texture loading before proceeding with model loading
        bool texturesValid = true;
        std::vector<std::pair<std::string, std::string>> textureChecks = {
            {"Albedo", cerberusTextures.AlbedoPath},
            {"Metallic", cerberusTextures.MetallicPath},
            {"Normal", cerberusTextures.NormalPath},
            {"Roughness", cerberusTextures.RoughnessPath},
            {"AO", cerberusTextures.AOPath}
        };
        
        for (const auto& [textureName, texturePath] : textureChecks)
        {
            auto testTexture = OloEngine::Texture2D::Create(texturePath);
            if (!testTexture || !testTexture->IsLoaded())
            {
                OLO_ERROR("Failed to load {} texture: {}", textureName, texturePath);
                texturesValid = false;
            }
        }
        
        if (!texturesValid)
        {
            OLO_ERROR("Some Cerberus textures failed to load. Model loading aborted.");
            return;
        }
        
        m_CerberusModel = OloEngine::Ref<OloEngine::Model>::Create(assetPath, cerberusTextures, true);
        m_BackpackModel.Reset();
    }
}

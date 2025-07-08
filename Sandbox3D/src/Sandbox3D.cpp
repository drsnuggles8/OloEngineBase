#include <format>

#include <OloEnginePCH.h>
#include "Sandbox3D.h"
#include <imgui.h>
#include <GLFW/glfw3.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "OloEngine/Renderer/Material.h"
#include "OloEngine/Renderer/Light.h"
#include "OloEngine/Renderer/Renderer3D.h"
#include "OloEngine/Renderer/ShaderDebugUtils.h"


#include "OloEngine/Animation/Skeleton.h"
#include "OloEngine/Animation/AnimationSystem.h"

Sandbox3D::Sandbox3D()
    : Layer("Sandbox3D"),
    m_CameraController(45.0f, 1280.0f / 720.0f, 0.1f, 1000.0f)
{
    // Initialize materials with metallic properties
    m_GoldMaterial.Ambient = glm::vec3(0.24725f, 0.1995f, 0.0745f);
    m_GoldMaterial.Diffuse = glm::vec3(0.75164f, 0.60648f, 0.22648f);
    m_GoldMaterial.Specular = glm::vec3(0.628281f, 0.555802f, 0.366065f);
    m_GoldMaterial.Shininess = 51.2f;

    m_SilverMaterial.Ambient = glm::vec3(0.19225f, 0.19225f, 0.19225f);
    m_SilverMaterial.Diffuse = glm::vec3(0.50754f, 0.50754f, 0.50754f);
    m_SilverMaterial.Specular = glm::vec3(0.508273f, 0.508273f, 0.508273f);
    m_SilverMaterial.Shininess = 76.8f;

    m_ChromeMaterial.Ambient = glm::vec3(0.25f, 0.25f, 0.25f);
    m_ChromeMaterial.Diffuse = glm::vec3(0.4f, 0.4f, 0.4f);
    m_ChromeMaterial.Specular = glm::vec3(0.774597f, 0.774597f, 0.774597f);
    m_ChromeMaterial.Shininess = 96.0f;

    // Initialize textured material
    m_TexturedMaterial.Ambient = glm::vec3(0.1f);
    m_TexturedMaterial.Diffuse = glm::vec3(1.0f);
    m_TexturedMaterial.Specular = glm::vec3(1.0f);
    m_TexturedMaterial.Shininess = 64.0f;
    m_TexturedMaterial.UseTextureMaps = true;              // Enable texture mapping
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
}

void Sandbox3D::OnAttach()
{
    OLO_PROFILE_FUNCTION();
    
    // Initialize debugging tools FIRST before creating any resources
    OloEngine::RendererMemoryTracker::GetInstance().Initialize();
    OloEngine::RendererProfiler::GetInstance().Initialize();
    // Note: GPUResourceInspector is now initialized in Application constructor

    // === DEBUG: Force disable shader cache and culling for troubleshooting ===
    // To always recompile shaders and render all objects, enable below:
    OloEngine::ShaderDebugUtils::SetDisableShaderCache(true);
    OloEngine::Renderer3D::SetForceDisableCulling(true);
    
    // Create 3D meshes
    m_CubeMesh = OloEngine::Mesh::CreateCube();
    m_SphereMesh = OloEngine::Mesh::CreateSphere();
    m_PlaneMesh = OloEngine::Mesh::CreatePlane(25.0f, 25.0f);

    // Load backpack model
    m_BackpackModel = OloEngine::CreateRef<OloEngine::Model>("assets/backpack/backpack.obj");

    // Load textures
    m_DiffuseMap = OloEngine::Texture2D::Create("assets/textures/container2.png");
    m_SpecularMap = OloEngine::Texture2D::Create("assets/textures/container2_specular.png");
    m_GrassTexture = OloEngine::Texture2D::Create("assets/textures/grass.png");
      // Temporarily disable frustum culling for debugging skinned mesh
    OloEngine::Renderer3D::EnableFrustumCulling(false);
    OLO_INFO("Sandbox3D: Frustum culling disabled for debugging");

    // Assign textures to the material
    m_TexturedMaterial.DiffuseMap = m_DiffuseMap;
    m_TexturedMaterial.SpecularMap = m_SpecularMap;    // Set initial lighting parameters
    OloEngine::Renderer3D::SetLight(m_Light);    // Create ECS test scene for animated mesh rendering
    m_TestScene = OloEngine::CreateRef<OloEngine::Scene>();
    m_TestScene->OnRuntimeStart(); // Initialize physics world and other runtime systems
    m_AnimatedMeshEntity = m_TestScene->CreateEntity("AnimatedTestMesh");
	// --- ECS Animated Mesh Test Entity ---
    // This is a minimal test: create a dummy entity with animated mesh components
    // In a real ECS, this would be managed by a Scene, but for now, we just test component construction and rendering    // Create a simple skinned cube for testing (with bone data)
    m_AnimatedTestMesh = CreateSkinnedCubeMesh();
    m_AnimatedTestSkeleton = OloEngine::CreateRef<OloEngine::Skeleton>();
    m_AnimatedTestSkeleton->m_BoneNames = { "Root" };
    m_AnimatedTestSkeleton->m_ParentIndices = { -1 };
    m_AnimatedTestSkeleton->m_LocalTransforms = { glm::mat4(1.0f) };
    m_AnimatedTestSkeleton->m_GlobalTransforms = { glm::mat4(1.0f) };
    m_AnimatedTestSkeleton->m_FinalBoneMatrices = { glm::mat4(1.0f) };
	// Add components to ECS entity
    auto& animMeshComp = m_AnimatedMeshEntity.AddComponent<OloEngine::AnimatedMeshComponent>();
    animMeshComp.m_Mesh = m_AnimatedTestMesh;
    // Note: m_Skeleton will be set separately via SkeletonComponent
     // Get the existing transform component (automatically added by CreateEntity) and configure it
    auto& transformComp = m_AnimatedMeshEntity.GetComponent<OloEngine::TransformComponent>();
    transformComp.Translation = glm::vec3(0.0f, 0.0f, 0.0f); // Move to center, directly in front of camera
    transformComp.Scale = glm::vec3(1.0f); // Make it full size for visibility
    
    auto& skeletonComp = m_AnimatedMeshEntity.AddComponent<OloEngine::SkeletonComponent>();
    // Copy skeleton data to component
    skeletonComp.m_ParentIndices = m_AnimatedTestSkeleton->m_ParentIndices;
    skeletonComp.m_BoneNames = m_AnimatedTestSkeleton->m_BoneNames;
    skeletonComp.m_LocalTransforms = m_AnimatedTestSkeleton->m_LocalTransforms;
    skeletonComp.m_GlobalTransforms = m_AnimatedTestSkeleton->m_GlobalTransforms;
    skeletonComp.m_FinalBoneMatrices = m_AnimatedTestSkeleton->m_FinalBoneMatrices;

    // --- Dummy Animation Clips (Idle and Bounce) ---
    using namespace OloEngine;
    // Idle: root bone stays at y=0
    m_IdleClip = CreateRef<AnimationClip>();
    m_IdleClip->Name = "Idle";
    m_IdleClip->Duration = 2.0f;
    BoneAnimation idleAnim;
    idleAnim.BoneName = "Root";
    idleAnim.Keyframes.push_back({ 0.0f, glm::vec3(0.0f, 0.0f, 0.0f), glm::quat(1,0,0,0), glm::vec3(1.0f) });
    idleAnim.Keyframes.push_back({ 2.0f, glm::vec3(0.0f, 0.0f, 0.0f), glm::quat(1,0,0,0), glm::vec3(1.0f) });
    m_IdleClip->BoneAnimations.push_back(idleAnim);

    // Bounce: root bone moves up and down
    m_BounceClip = CreateRef<AnimationClip>();
    m_BounceClip->Name = "Bounce";
    m_BounceClip->Duration = 2.0f;
    BoneAnimation bounceAnim;
    bounceAnim.BoneName = "Root";
    bounceAnim.Keyframes.push_back({ 0.0f, glm::vec3(0.0f, 0.0f, 0.0f), glm::quat(1,0,0,0), glm::vec3(1.0f) });
    bounceAnim.Keyframes.push_back({ 1.0f, glm::vec3(0.0f, 1.0f, 0.0f), glm::quat(1,0,0,0), glm::vec3(1.0f) });
    bounceAnim.Keyframes.push_back({ 2.0f, glm::vec3(0.0f, 0.0f, 0.0f), glm::quat(1,0,0,0), glm::vec3(1.0f) });
    m_BounceClip->BoneAnimations.push_back(bounceAnim);

    // Animation state: start with Idle
    m_AnimatedTestAnimState = AnimationStateComponent{ m_IdleClip };
    
    // Add animation state component to ECS entity
    auto& animStateComp = m_AnimatedMeshEntity.AddComponent<OloEngine::AnimationStateComponent>();
    animStateComp = m_AnimatedTestAnimState;

    // Create multi-bone test entity for complex animation testing
    CreateMultiBoneTestEntity();
    
    // Create placeholder for imported animated model
    LoadTestAnimatedModel();
}

void Sandbox3D::OnDetach()
{	OLO_PROFILE_FUNCTION();
        // Shutdown debugging tools
    OloEngine::RendererMemoryTracker::GetInstance().Shutdown();
    OloEngine::RendererProfiler::GetInstance().Shutdown();
    // Note: GPUResourceInspector is now shutdown in Application destructor
}

void Sandbox3D::OnUpdate(const OloEngine::Timestep ts)
{
    OLO_PROFILE_FUNCTION();

    m_FrameTime = ts.GetMilliseconds();
    m_FPS = 1.0f / ts.GetSeconds();
    // Update debugging tools
    OloEngine::RendererMemoryTracker::GetInstance().UpdateStats();
    // Note: RendererProfiler doesn't have UpdateStats method

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

    // Update view position for specular highlights
    OloEngine::Renderer3D::SetViewPosition(m_CameraController.GetCamera().GetPosition());

    // Toggle rotation on spacebar press
    bool spacePressed = OloEngine::Input::IsKeyPressed(OloEngine::Key::Space);
    if (spacePressed && !m_WasSpacePressed)
        m_RotationEnabled = !m_RotationEnabled;
    m_WasSpacePressed = spacePressed;

    // Rotate only if enabled
    if (m_RotationEnabled)
    {
        m_RotationAngleY += ts * 45.0f; // 45 deg/s around Y
        m_RotationAngleX += ts * 30.0f; // 30 deg/s around X

        // Keep angles in [0, 360)
        if (m_RotationAngleY > 360.0f)  m_RotationAngleY -= 360.0f;
        if (m_RotationAngleX > 360.0f)  m_RotationAngleX -= 360.0f;
    }

    // Animate the light position in a circular pattern (only for point and spot lights)
    if (m_AnimateLight && m_Light.Type != OloEngine::LightType::Directional)
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

        OloEngine::Renderer3D::SetLight(m_Light);
    }
	
	// --- Basic Animation State Machine: auto-switch Idle <-> Bounce every 2 seconds ---
    static float animStateTimer = 0.0f;
    animStateTimer += ts.GetSeconds();
    if (!m_AnimatedTestAnimState.Blending && m_AnimatedTestAnimState.m_CurrentClip)
    {
        float switchInterval = 2.0f;
        if (animStateTimer >= switchInterval)
        {
            auto requestedClip = (m_AnimatedTestAnimState.m_CurrentClip == m_IdleClip) ? m_BounceClip : m_IdleClip;
            m_AnimatedTestAnimState.m_NextClip = requestedClip;
            m_AnimatedTestAnimState.NextTime = 0.0f;
            m_AnimatedTestAnimState.Blending = true;
            m_AnimatedTestAnimState.BlendTime = 0.0f;
            m_AnimatedTestAnimState.BlendFactor = 0.0f;
            m_AnimatedTestAnimState.m_State = (requestedClip == m_IdleClip) ? OloEngine::AnimationStateComponent::State::Idle : OloEngine::AnimationStateComponent::State::Bounce;
            animStateTimer = 0.0f;
              // Update ECS entity animation state as well
            auto& ecsAnimState = m_AnimatedMeshEntity.GetComponent<OloEngine::AnimationStateComponent>();
            ecsAnimState = m_AnimatedTestAnimState;
        }
    }

    // Update animation and skeleton for both manual and ECS animated meshes
    {
        // Advance animation and compute bone transforms
        OloEngine::Animation::AnimationSystem::Update(
            m_AnimatedTestAnimState,
            *m_AnimatedTestSkeleton,
            ts.GetSeconds()
        );
        
        // Update ECS entity's skeleton component with computed bone matrices
        if (m_AnimatedMeshEntity.HasComponent<OloEngine::SkeletonComponent>())
        {
            auto& skeletonComp = m_AnimatedMeshEntity.GetComponent<OloEngine::SkeletonComponent>();
            skeletonComp.m_FinalBoneMatrices = m_AnimatedTestSkeleton->m_FinalBoneMatrices;
            skeletonComp.m_GlobalTransforms = m_AnimatedTestSkeleton->m_GlobalTransforms;
        }
        
        // Update multi-bone test entity animation
        if (m_MultiBoneTestEntity.HasComponent<OloEngine::AnimationStateComponent>())
        {
            auto& multiBoneAnimStateComp = m_MultiBoneTestEntity.GetComponent<OloEngine::AnimationStateComponent>();
            
            // Advance multi-bone animation
            OloEngine::Animation::AnimationSystem::Update(
                multiBoneAnimStateComp,
                *m_MultiBoneTestSkeleton,
                ts.GetSeconds() * m_AnimationSpeed
            );
            
            // Update skeleton component
            if (m_MultiBoneTestEntity.HasComponent<OloEngine::SkeletonComponent>())
            {
                auto& multiBoneSkeletonComp = m_MultiBoneTestEntity.GetComponent<OloEngine::SkeletonComponent>();
                multiBoneSkeletonComp.m_FinalBoneMatrices = m_MultiBoneTestSkeleton->m_FinalBoneMatrices;
                multiBoneSkeletonComp.m_GlobalTransforms = m_MultiBoneTestSkeleton->m_GlobalTransforms;
            }
        }
        
        // Update imported model animation (if loaded)
        if (m_ImportedModelEntity.HasComponent<OloEngine::AnimationStateComponent>())
        {
            auto& importedAnimStateComp = m_ImportedModelEntity.GetComponent<OloEngine::AnimationStateComponent>();
            // For now, use the same animation as the basic test since it's a placeholder
            // TODO: Replace with actual imported model animations
            OloEngine::Animation::AnimationSystem::Update(
                importedAnimStateComp,
                *m_AnimatedTestSkeleton, // Placeholder skeleton
                ts.GetSeconds() * m_AnimationSpeed
            );
        }
    }
	
	// Update ECS scene (but not rendering - that happens in render scope)
    if (m_TestScene)
    {
        m_TestScene->OnUpdateRuntime(ts);
    }

    {
        OLO_PROFILE_SCOPE("Renderer Draw");
		OloEngine::Renderer3D::BeginScene(m_CameraController.GetCamera());
        
        // === ECS-DRIVEN ANIMATED MESH TEST ===
        if (m_TestScene)
        {
            // Control entity visibility based on test settings
            if (m_AnimatedMeshEntity.HasComponent<OloEngine::AnimatedMeshComponent>())
            {
                if (m_ShowSingleBoneTest)
                {
                    // Update ECS entity transform to position it on the left side
                    auto& ecsTransformComp = m_AnimatedMeshEntity.GetComponent<OloEngine::TransformComponent>();
                    ecsTransformComp.Translation = glm::vec3(-3.0f, 0.0f, 0.0f); // Left side
                    ecsTransformComp.Scale = glm::vec3(1.5f); // Make it visible
                }
            }
            
            // Control multi-bone test entity visibility
            if (m_MultiBoneTestEntity.HasComponent<OloEngine::AnimatedMeshComponent>())
            {
                if (m_ShowMultiBoneTest)
                {
                    auto& multiBoneTransformComp = m_MultiBoneTestEntity.GetComponent<OloEngine::TransformComponent>();
                    multiBoneTransformComp.Translation = glm::vec3(3.0f, 0.0f, 0.0f); // Right side for multi-bone
                    multiBoneTransformComp.Scale = glm::vec3(1.0f);
                }
            }
            
            // Render all active animated entities in the scene
            // This will render entities that have the required components and are enabled
            if (m_ShowSingleBoneTest || m_ShowMultiBoneTest)
            {
                OloEngine::AnimatedMeshRenderSystem::RenderAnimatedMeshes(m_TestScene, m_GoldMaterial);
            }
        }

        // Draw ground plane
        {
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

        // Draw a grass quad
        {
            auto grassMatrix = glm::mat4(1.0f);
            grassMatrix = glm::translate(grassMatrix, glm::vec3(0.0f, 0.5f, -1.0f));
            // Make it face the camera by rotating around X axis
            grassMatrix = glm::rotate(grassMatrix, glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
            auto* grassPacket = OloEngine::Renderer3D::DrawQuad(grassMatrix, m_GrassTexture);
            if (grassPacket) OloEngine::Renderer3D::SubmitPacket(grassPacket);
        }

        // Draw backpack model using command-based renderer
        {
            auto modelMatrix = glm::mat4(1.0f);
            modelMatrix = glm::translate(modelMatrix, glm::vec3(0.0f, 1.0f, -2.0f)); // Raise it up and move it back
            modelMatrix = glm::scale(modelMatrix, glm::vec3(0.5f)); // Scale down to reasonable size
            modelMatrix = glm::rotate(modelMatrix, glm::radians(m_RotationAngleY), glm::vec3(0.0f, 1.0f, 0.0f));
            std::vector<OloEngine::CommandPacket*> backpackDrawCommands;
            m_BackpackModel->GetDrawCommands(modelMatrix, m_TexturedMaterial, backpackDrawCommands);
            for (auto* drawCmd : backpackDrawCommands)
            {
                OloEngine::Renderer3D::SubmitPacket(drawCmd);
            }        }

        // === MANUAL ANIMATED MESH TEST ===
        if (m_ShowSingleBoneTest)
        {
            // Update animation state and skeleton for the manual test
            OloEngine::Animation::AnimationSystem::Update(
                m_AnimatedTestAnimState,
                *m_AnimatedTestSkeleton,
                ts.GetSeconds()
            );

            // Position the manual test on the right side for easy comparison
            glm::mat4 manualAnimTestMatrix = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 0.0f, 0.0f));
            manualAnimTestMatrix = glm::scale(manualAnimTestMatrix, glm::vec3(1.5f));
            
            OloEngine::Material manualAnimTestMaterial = m_SilverMaterial;

            // Use DrawSkinnedMesh with bone matrices for GPU skinning
            auto* manualAnimTestPacket = OloEngine::Renderer3D::DrawSkinnedMesh(
                m_AnimatedTestMesh, 
                manualAnimTestMatrix, 
                manualAnimTestMaterial, 
                m_AnimatedTestSkeleton->m_FinalBoneMatrices
            );

            if (manualAnimTestPacket)
            {
                OloEngine::Renderer3D::SubmitPacket(manualAnimTestPacket);
            }
        }
		
		// === STATIC SCENE OBJECTS ===        
        // Draw ground plane
        {
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

        // Draw a grass quad
        {
            auto grassMatrix = glm::mat4(1.0f);
            grassMatrix = glm::translate(grassMatrix, glm::vec3(0.0f, 0.5f, -1.0f));
            grassMatrix = glm::rotate(grassMatrix, glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f));
            auto* grassPacket = OloEngine::Renderer3D::DrawQuad(grassMatrix, m_GrassTexture);
            if (grassPacket) OloEngine::Renderer3D::SubmitPacket(grassPacket);
        }

        // Draw backpack model using command-based renderer
        {
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

        // === ROTATING ANIMATED OBJECTS ===        
        // Center rotating cube with wireframe overlay
        {
            auto modelMatrix = glm::mat4(1.0f);
            modelMatrix = glm::rotate(modelMatrix, glm::radians(m_RotationAngleX), glm::vec3(1.0f, 0.0f, 0.0f));
            modelMatrix = glm::rotate(modelMatrix, glm::radians(m_RotationAngleY), glm::vec3(0.0f, 1.0f, 0.0f));
            // Draw filled mesh (normal)
            auto* solidPacket = OloEngine::Renderer3D::DrawMesh(m_CubeMesh, modelMatrix, m_GoldMaterial);
            if (solidPacket) OloEngine::Renderer3D::SubmitPacket(solidPacket);
            // Overlay wireframe
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
                drawCmd->renderState->LineWidth.Width = 2.5f; // Thicker line
                drawCmd->renderState->PolygonOffset.Enabled = true;
                drawCmd->renderState->PolygonOffset.Factor = -1.0f;
                drawCmd->renderState->PolygonOffset.Units = -1.0f;
                OloEngine::Renderer3D::SubmitPacket(wirePacket);
            }
        }

        // Draw other objects
        switch (m_PrimitiveTypeIndex)
        {
            case 0: // Cubes
                // Draw right silver cube
            {
                auto silverCubeMatrix = glm::mat4(1.0f);
                silverCubeMatrix = glm::translate(silverCubeMatrix, glm::vec3(2.0f, 0.0f, 0.0f));
                silverCubeMatrix = glm::rotate(silverCubeMatrix, glm::radians(m_RotationAngleY * 1.5f), glm::vec3(0.0f, 1.0f, 0.0f));
                auto* silverPacket = OloEngine::Renderer3D::DrawMesh(m_CubeMesh, silverCubeMatrix, m_SilverMaterial);
                if (silverPacket) OloEngine::Renderer3D::SubmitPacket(silverPacket);
            }

            // Draw left chrome cube
            {
                auto chromeCubeMatrix = glm::mat4(1.0f);
                chromeCubeMatrix = glm::translate(chromeCubeMatrix, glm::vec3(-2.0f, 0.0f, 0.0f));
                chromeCubeMatrix = glm::rotate(chromeCubeMatrix, glm::radians(m_RotationAngleX * 1.5f), glm::vec3(1.0f, 0.0f, 0.0f));
                auto* chromePacket = OloEngine::Renderer3D::DrawMesh(m_CubeMesh, chromeCubeMatrix, m_ChromeMaterial);
                if (chromePacket) OloEngine::Renderer3D::SubmitPacket(chromePacket);
            }
            break;

            case 1: // Spheres
                // Draw center gold sphere
            {
                auto modelMatrix = glm::mat4(1.0f);
                auto* goldPacket = OloEngine::Renderer3D::DrawMesh(m_SphereMesh, modelMatrix, m_GoldMaterial);
                if (goldPacket) OloEngine::Renderer3D::SubmitPacket(goldPacket);
            }

            // Draw right silver sphere
            {
                auto silverSphereMatrix = glm::mat4(1.0f);
                silverSphereMatrix = glm::translate(silverSphereMatrix, glm::vec3(2.0f, 0.0f, 0.0f));
                auto* silverPacket = OloEngine::Renderer3D::DrawMesh(m_SphereMesh, silverSphereMatrix, m_SilverMaterial);
                if (silverPacket) OloEngine::Renderer3D::SubmitPacket(silverPacket);
            }

            // Draw left chrome sphere
            {
                auto chromeSphereMatrix = glm::mat4(1.0f);
                chromeSphereMatrix = glm::translate(chromeSphereMatrix, glm::vec3(-2.0f, 0.0f, 0.0f));
                auto* chromePacket = OloEngine::Renderer3D::DrawMesh(m_SphereMesh, chromeSphereMatrix, m_ChromeMaterial);
                if (chromePacket) OloEngine::Renderer3D::SubmitPacket(chromePacket);
            }
            break;

            case 2: // Mixed
            default:
            {
                // Draw right silver sphere
                auto silverSphereMatrix = glm::mat4(1.0f);
                silverSphereMatrix = glm::translate(silverSphereMatrix, glm::vec3(2.0f, 0.0f, 0.0f));
                auto* silverPacket = OloEngine::Renderer3D::DrawMesh(m_SphereMesh, silverSphereMatrix, m_SilverMaterial);
                if (silverPacket) OloEngine::Renderer3D::SubmitPacket(silverPacket);

                // Draw left chrome cube
                auto chromeCubeMatrix = glm::mat4(1.0f);
                chromeCubeMatrix = glm::translate(chromeCubeMatrix, glm::vec3(-2.0f, 0.0f, 0.0f));
                chromeCubeMatrix = glm::rotate(chromeCubeMatrix, glm::radians(m_RotationAngleX * 1.5f), glm::vec3(1.0f, 0.0f, 0.0f));
                auto* chromePacket = OloEngine::Renderer3D::DrawMesh(m_CubeMesh, chromeCubeMatrix, m_ChromeMaterial);
                if (chromePacket) OloEngine::Renderer3D::SubmitPacket(chromePacket);
            }
            break;
        }

        // Textured sphere (shared across all modes)
        {
            auto sphereMatrix = glm::mat4(1.0f);
            sphereMatrix = glm::translate(sphereMatrix, glm::vec3(0.0f, 2.0f, 0.0f));
            sphereMatrix = glm::rotate(sphereMatrix, glm::radians(m_RotationAngleX * 0.8f), glm::vec3(1.0f, 0.0f, 0.0f));
            sphereMatrix = glm::rotate(sphereMatrix, glm::radians(m_RotationAngleY * 0.8f), glm::vec3(0.0f, 1.0f, 0.0f));
            auto* texturedPacket = OloEngine::Renderer3D::DrawMesh(m_SphereMesh, sphereMatrix, m_TexturedMaterial);
            if (texturedPacket) OloEngine::Renderer3D::SubmitPacket(texturedPacket);
        }

        // Light cube (only for point and spot lights)
        if (m_Light.Type != OloEngine::LightType::Directional)
        {
            auto lightCubeModelMatrix = glm::mat4(1.0f);
            lightCubeModelMatrix = glm::translate(lightCubeModelMatrix, m_Light.Position);
            lightCubeModelMatrix = glm::scale(lightCubeModelMatrix, glm::vec3(0.2f));
            auto* lightCubePacket = OloEngine::Renderer3D::DrawLightCube(lightCubeModelMatrix);
            if (lightCubePacket) OloEngine::Renderer3D::SubmitPacket(lightCubePacket);
        }

        // Draw our state test objects to demonstrate the new state system
        if (m_EnableStateTest)
        {
            RenderStateTestObjects(m_RotationAngleY);
        }
    }

    OloEngine::Renderer3D::EndScene();
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

    ImGui::Begin("Settings & Controls");    // Render different sections in collapsible headers
    if (ImGui::CollapsingHeader("Performance & Frame Info", ImGuiTreeNodeFlags_None))
    {
        RenderPerformanceInfo();
    }

    if (ImGui::CollapsingHeader("Scene Settings", ImGuiTreeNodeFlags_None))
    {
        RenderSceneSettings();
    }

    if (ImGui::CollapsingHeader("Lighting Settings", ImGuiTreeNodeFlags_None))
    {
        RenderLightingSettings();
    }

    if (ImGui::CollapsingHeader("Material Settings", ImGuiTreeNodeFlags_None))
    {
        RenderMaterialSettings();
    }    if (ImGui::CollapsingHeader("Animation Debug", ImGuiTreeNodeFlags_None))
    {
        ImGui::TextWrapped("Manual Animation System: Controls the silver cube on the right side using direct DrawSkinnedMesh calls.");
        ImGui::Separator();
        RenderAnimationDebugPanel();
    }

    if (ImGui::CollapsingHeader("ECS Animated Mesh", ImGuiTreeNodeFlags_None))
    {
        ImGui::TextWrapped("ECS Animation System: Controls the gold cube on the left side using AnimatedMeshRenderSystem and ECS components.");
        ImGui::Separator();
        RenderECSAnimatedMeshPanel();
    }

    if (ImGui::CollapsingHeader("State Management Test", ImGuiTreeNodeFlags_None))
    {
        RenderStateTestSettings();
    }

    if (ImGui::CollapsingHeader("Enhanced Animation Tests", ImGuiTreeNodeFlags_None))
    {
        ImGui::TextWrapped("Advanced testing for multi-bone animations and model imports.");
        ImGui::Separator();
        
        // Test visibility controls
        ImGui::Checkbox("Show Single Bone Test", &m_ShowSingleBoneTest);
        ImGui::SameLine();
        ImGui::Checkbox("Show Multi-Bone Test", &m_ShowMultiBoneTest);
        
        ImGui::SliderFloat("Animation Speed", &m_AnimationSpeed, 0.1f, 3.0f, "%.1f");
        
        ImGui::Separator();
        
        // Multi-bone test info
        if (m_MultiBoneTestEntity.HasComponent<OloEngine::AnimationStateComponent>())
        {
            auto& animState = m_MultiBoneTestEntity.GetComponent<OloEngine::AnimationStateComponent>();
            ImGui::Text("Multi-Bone Animation:");
            ImGui::Text("  Clip: %s", animState.m_CurrentClip ? animState.m_CurrentClip->Name.c_str() : "None");
            ImGui::Text("  Time: %.2f / %.2f", animState.CurrentTime, 
                       animState.m_CurrentClip ? animState.m_CurrentClip->Duration : 0.0f);
            ImGui::Text("  Bones: 4 (hierarchical)");
        }
        
        ImGui::Separator();
        
        // Model import section
        ImGui::Text("Model Import (Assimp):");
        if (ImGui::Button("Load Test Model"))
        {
            LoadTestAnimatedModel();
        }
        ImGui::SameLine();
        ImGui::Text("Status: %s", m_ImportedModelEntity.HasComponent<OloEngine::AnimatedMeshComponent>() 
                   ? "Placeholder Loaded" : "Not Loaded");
                   
        ImGui::TextWrapped("Note: Model import is placeholder implementation. Full assimp integration TODO.");
    }

    if (ImGui::CollapsingHeader("Renderer Debugging Tools", ImGuiTreeNodeFlags_None))
    {
        RenderDebuggingTools();
    }

    ImGui::End();
}

// Animation Debug Panel (must be at file scope, not inside another function)
void Sandbox3D::RenderAnimationDebugPanel()
{
    static const char* animNames[] = { "Idle", "Bounce" };
    int prevIndex = m_AnimClipIndex;
    ImGui::Text("Current State: %s", m_AnimatedTestAnimState.m_State == OloEngine::AnimationStateComponent::State::Idle ? "Idle" : "Bounce");
    ImGui::Text("Current Clip: %s", m_AnimatedTestAnimState.m_CurrentClip ? m_AnimatedTestAnimState.m_CurrentClip->Name.c_str() : "None");
    ImGui::Text("Time: %.2f", m_AnimatedTestAnimState.CurrentTime);
    ImGui::Text("Blending: %s", m_AnimatedTestAnimState.Blending ? "Yes" : "No");
    if (m_AnimatedTestAnimState.Blending)
    {
        ImGui::Text("Blend Factor: %.2f", m_AnimatedTestAnimState.BlendFactor);
        ImGui::Text("Next Clip: %s", m_AnimatedTestAnimState.m_NextClip ? m_AnimatedTestAnimState.m_NextClip->Name.c_str() : "None");
    }
    ImGui::Separator();
    ImGui::Text("Switch Animation State:");
    ImGui::RadioButton("Idle", &m_AnimClipIndex, 0); ImGui::SameLine();
    ImGui::RadioButton("Bounce", &m_AnimClipIndex, 1);
    if (prevIndex != m_AnimClipIndex)
    {
        m_AnimBlendRequested = true;
    }
    if (ImGui::Button("Blend Now") || m_AnimBlendRequested)
    {
        // Only trigger blend if not already blending and the requested state is different
        auto requestedClip = (m_AnimClipIndex == 0) ? m_IdleClip : m_BounceClip;
        if (!m_AnimatedTestAnimState.Blending && m_AnimatedTestAnimState.m_CurrentClip != requestedClip)
        {
            m_AnimatedTestAnimState.m_NextClip = requestedClip;
            m_AnimatedTestAnimState.NextTime = 0.0f;
            m_AnimatedTestAnimState.Blending = true;
            m_AnimatedTestAnimState.BlendTime = 0.0f;
            m_AnimatedTestAnimState.BlendFactor = 0.0f;
            m_AnimatedTestAnimState.m_State = (m_AnimClipIndex == 0) ? OloEngine::AnimationStateComponent::State::Idle : OloEngine::AnimationStateComponent::State::Bounce;
        }
        m_AnimBlendRequested = false;
    }

    // Sync UI state with animation state after blending completes
    if (!m_AnimatedTestAnimState.Blending)
    {
        if (m_AnimatedTestAnimState.m_CurrentClip == m_IdleClip)
            m_AnimClipIndex = 0;
        else if (m_AnimatedTestAnimState.m_CurrentClip == m_BounceClip)
            m_AnimClipIndex = 1;
    }
    ImGui::Separator();

    ImGui::Text("(TODO: Add automatic state machine transitions)");
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

void Sandbox3D::RenderSceneSettings()
{
    ImGui::Combo("Primitive Types", &m_PrimitiveTypeIndex, m_PrimitiveNames, 3);
    ImGui::Separator();

    // Add a section for frustum culling settings
    ImGui::Text("Frustum Culling");
    ImGui::Indent();
    
    bool frustumCullingEnabled = OloEngine::Renderer3D::IsFrustumCullingEnabled();
    if (ImGui::Checkbox("Enable Frustum Culling", &frustumCullingEnabled))
    {
        OloEngine::Renderer3D::EnableFrustumCulling(frustumCullingEnabled);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted("Enables frustum culling to skip rendering objects outside the camera view.");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    
    bool dynamicCullingEnabled = OloEngine::Renderer3D::IsDynamicCullingEnabled();
    if (ImGui::Checkbox("Cull Dynamic Objects", &dynamicCullingEnabled))
    {
        OloEngine::Renderer3D::EnableDynamicCulling(dynamicCullingEnabled);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted("Warning: Enabling this may cause visual glitches with rotating objects.");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    
    if (ImGui::Button("Reset to Defaults"))
    {
        OloEngine::Renderer3D::EnableFrustumCulling(true);
        OloEngine::Renderer3D::EnableDynamicCulling(false);
    }
    
    ImGui::Text("Meshes: Total %d, Culled %d (%.1f%%)", 
        OloEngine::Renderer3D::GetStats().TotalMeshes, 
        OloEngine::Renderer3D::GetStats().CulledMeshes,
        OloEngine::Renderer3D::GetStats().TotalMeshes > 0 
            ? 100.0f * OloEngine::Renderer3D::GetStats().CulledMeshes / OloEngine::Renderer3D::GetStats().TotalMeshes 
            : 0.0f);
    
    ImGui::Unindent();
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

        OloEngine::Renderer3D::SetLight(m_Light);
    }    // Show different UI controls based on light type
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

void Sandbox3D::RenderMaterialSettings()
{
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
    if (m_SelectedMaterial == 3) // Textured material
    {
        // For textured material, show the texture map toggle
        ImGui::Checkbox("Use Texture Maps", &currentMaterial->UseTextureMaps);
        ImGui::Text("Shininess");
        ImGui::SliderFloat("##TexturedShininess", &currentMaterial->Shininess, 1.0f, 128.0f);
        
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
            else
            {
                ImGui::Text("Command bucket not available");
            }
        }
    }
    
    // Memory Tracker
    if (ImGui::CollapsingHeader("Memory Tracker", ImGuiTreeNodeFlags_None))
    {
        ImGui::Checkbox("Show Memory Tracker##MemoryTracker", &m_ShowMemoryTracker);
        
        if (m_ShowMemoryTracker)
        {
            m_MemoryTracker.RenderUI(&m_ShowMemoryTracker);
        }
    }
    
    // Renderer Profiler
    if (ImGui::CollapsingHeader("Renderer Profiler", ImGuiTreeNodeFlags_None))
    {
        ImGui::Checkbox("Show Profiler##RendererProfiler", &m_ShowRendererProfiler);
        
        if (m_ShowRendererProfiler)
        {
            m_RendererProfiler.RenderUI(&m_ShowRendererProfiler);
        }
    }
    
    // GPU Resource Inspector
    if (ImGui::CollapsingHeader("GPU Resource Inspector", ImGuiTreeNodeFlags_None))
    {
        ImGui::Checkbox("Show GPU Resources##GPUResourceInspector", &m_ShowGPUResourceInspector);
        ImGui::SameLine();
        if (ImGui::Button("Export to CSV##GPUResourceInspector"))
        {
            m_GPUResourceInspector.ExportToCSV("gpu_resources.csv");
        }
        
        if (m_ShowGPUResourceInspector)
        {
            m_GPUResourceInspector.RenderDebugView(&m_ShowGPUResourceInspector, "GPU Resource Inspector");
        }
    }

    // Shader Debugger
    if (ImGui::CollapsingHeader("Shader Debugger", ImGuiTreeNodeFlags_None))
    {
        ImGui::Checkbox("Show Shader Debugger##ShaderDebugger", &m_ShowShaderDebugger);
        ImGui::SameLine();
        if (ImGui::Button("Export Report##ShaderDebugger"))
        {
            m_ShaderDebugger.ExportReport("shader_debug_report.txt");
        }
        
        if (m_ShowShaderDebugger)
        {
            m_ShaderDebugger.RenderDebugView(&m_ShowShaderDebugger, "Shader Debugger");
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
    }
    
    // Light colors
    bool lightChanged = false;
    lightChanged |= ImGui::ColorEdit3("Ambient##DirLight", glm::value_ptr(m_Light.Ambient));
    lightChanged |= ImGui::ColorEdit3("Diffuse##DirLight", glm::value_ptr(m_Light.Diffuse));
    lightChanged |= ImGui::ColorEdit3("Specular##DirLight", glm::value_ptr(m_Light.Specular));

    if (lightChanged)
    {
        OloEngine::Renderer3D::SetLight(m_Light);
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
            OloEngine::Renderer3D::SetLight(m_Light);
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
        OloEngine::Renderer3D::SetLight(m_Light);
    }
}

void Sandbox3D::RenderSpotlightUI()
{
    if (!m_AnimateLight)
    {
        // Position control (only if not animating)
        if (bool positionChanged = ImGui::DragFloat3("Position##Spotlight", glm::value_ptr(m_Light.Position), 0.1f); positionChanged)
        {
            OloEngine::Renderer3D::SetLight(m_Light);
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

            OloEngine::Renderer3D::SetLight(m_Light);
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
        OloEngine::Renderer3D::SetLight(m_Light);
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
            if (solidPacket) {
                OloEngine::Renderer3D::SubmitPacket(solidPacket);
            }
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

void Sandbox3D::RenderECSAnimatedMeshPanel()
{
    if (!m_TestScene || !m_AnimatedMeshEntity)
    {
        ImGui::Text("No ECS test scene available");
        return;
    }

    ImGui::Text("ECS Animated Mesh Demo");
    ImGui::Separator();
      // Entity info
    ImGui::Text("Entity ID: %d", static_cast<i32>(static_cast<u32>(m_AnimatedMeshEntity)));
    
    // Check if entity has all required components
    bool hasAnimMesh = m_AnimatedMeshEntity.HasComponent<OloEngine::AnimatedMeshComponent>();
    bool hasSkeleton = m_AnimatedMeshEntity.HasComponent<OloEngine::SkeletonComponent>();
    bool hasAnimState = m_AnimatedMeshEntity.HasComponent<OloEngine::AnimationStateComponent>();
    
    ImGui::Text("Components:");
    ImGui::Text("  AnimatedMeshComponent: %s", hasAnimMesh ? "✓" : "✗");
    ImGui::Text("  SkeletonComponent: %s", hasSkeleton ? "✓" : "✗");
    ImGui::Text("  AnimationStateComponent: %s", hasAnimState ? "✓" : "✗");
    
    if (hasAnimState)
    {
        ImGui::Separator();
        auto& animState = m_AnimatedMeshEntity.GetComponent<OloEngine::AnimationStateComponent>();
        ImGui::Text("Animation State:");
        ImGui::Text("  Current Clip: %s", animState.m_CurrentClip ? animState.m_CurrentClip->Name.c_str() : "None");
        ImGui::Text("  Time: %.2f", animState.CurrentTime);
        ImGui::Text("  Blending: %s", animState.Blending ? "Yes" : "No");
        if (animState.Blending)
        {
            ImGui::Text("  Blend Factor: %.2f", animState.BlendFactor);
            ImGui::Text("  Next Clip: %s", animState.m_NextClip ? animState.m_NextClip->Name.c_str() : "None");
        }
    }
      if (hasSkeleton)
    {
        ImGui::Separator();
        auto& skeleton = m_AnimatedMeshEntity.GetComponent<OloEngine::SkeletonComponent>();
        ImGui::Text("Skeleton Info:");
        ImGui::Text("  Bone Count: %zu", skeleton.m_BoneNames.size());
        ImGui::Text("  Root Bone: %s", 
            skeleton.m_BoneNames.empty() ? "None" : skeleton.m_BoneNames[0].c_str());
    }
    
    ImGui::Separator();
    ImGui::Text("Render System Status: Active");
    ImGui::Text("This entity is rendered via AnimatedMeshRenderSystem");
}

OloEngine::Ref<OloEngine::SkinnedMesh> Sandbox3D::CreateSkinnedCubeMesh()
{
    // Simple cube vertices with bone indices and weights (all affected by bone 0)
    std::vector<OloEngine::SkinnedVertex> skinnedVertices = {
        // Front face
        {{-0.5f, -0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
        {{ 0.5f, -0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
        {{ 0.5f,  0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
        {{-0.5f,  0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},

        // Back face
        {{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
        {{ 0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
        {{ 0.5f,  0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
        {{-0.5f,  0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},

        // Left face  
        {{-0.5f, -0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
        {{-0.5f, -0.5f,  0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
        {{-0.5f,  0.5f,  0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
        {{-0.5f,  0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},

        // Right face
        {{ 0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
        {{ 0.5f, -0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
        {{ 0.5f,  0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
        {{ 0.5f,  0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},

        // Top face
        {{-0.5f,  0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
        {{ 0.5f,  0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
        {{ 0.5f,  0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
        {{-0.5f,  0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},

        // Bottom face
        {{-0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
        {{ 0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
        {{ 0.5f, -0.5f,  0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}},
        {{-0.5f, -0.5f,  0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}, {0, -1, -1, -1}, {1.0f, 0.0f, 0.0f, 0.0f}}
    };    // Cube indices (same as regular cube)
    std::vector<u32> indices = {
        // Front face
        0, 1, 2, 2, 3, 0,
        // Back face
        4, 5, 6, 6, 7, 4,
        // Left face
        8, 9, 10, 10, 11, 8,
        // Right face
        12, 13, 14, 14, 15, 12,
        // Top face
        16, 17, 18, 18, 19, 16,
        // Bottom face
        20, 21, 22, 22, 23, 20
    };

    // Convert SkinnedVertex to regular Vertex for the Mesh class
    // The skinning will be handled by the DrawSkinnedMesh command and shader
    std::vector<OloEngine::Vertex> vertices;
    vertices.reserve(skinnedVertices.size());
    
    for (const auto& skinnedVert : skinnedVertices)
    {
        vertices.emplace_back(skinnedVert.Position, skinnedVert.Normal, skinnedVert.TexCoord);
    }
      // Create mesh using regular vertex data
    // Note: Bone data will be handled through uniform bone matrices in the shader
    return OloEngine::CreateRef<OloEngine::SkinnedMesh>(skinnedVertices, indices);
}

void Sandbox3D::CreateMultiBoneTestEntity()
{
    OLO_PROFILE_FUNCTION();
    
    // Create multi-bone test entity
    m_MultiBoneTestEntity = m_TestScene->CreateEntity("MultiBoneTestMesh");
    
    // Create a multi-bone cube mesh for advanced animation testing
    m_MultiBoneTestMesh = OloEngine::SkinnedMesh::CreateMultiBoneCube();
    
    // Create multi-bone skeleton with hierarchical bone structure
    m_MultiBoneTestSkeleton = OloEngine::CreateRef<OloEngine::Skeleton>();
    m_MultiBoneTestSkeleton->m_BoneNames = { "Root", "Child1", "Child2", "Child3" };
    m_MultiBoneTestSkeleton->m_ParentIndices = { -1, 0, 0, 1 }; // Root->Child1->Child3, Root->Child2
    
    // Initialize transforms
    m_MultiBoneTestSkeleton->m_LocalTransforms = {
        glm::mat4(1.0f), // Root
        glm::translate(glm::mat4(1.0f), glm::vec3(0.5f, 0.0f, 0.0f)), // Child1
        glm::translate(glm::mat4(1.0f), glm::vec3(-0.5f, 0.0f, 0.0f)), // Child2
        glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.5f, 0.0f))  // Child3
    };
    m_MultiBoneTestSkeleton->m_GlobalTransforms = { 
        glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f) 
    };
    m_MultiBoneTestSkeleton->m_FinalBoneMatrices = { 
        glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f), glm::mat4(1.0f) 
    };
    
    // Add components to ECS entity
    auto& animMeshComp = m_MultiBoneTestEntity.AddComponent<OloEngine::AnimatedMeshComponent>();
    animMeshComp.m_Mesh = m_MultiBoneTestMesh;
    
    // Position the multi-bone test entity to the side of the main test
    auto& transformComp = m_MultiBoneTestEntity.GetComponent<OloEngine::TransformComponent>();
    transformComp.Translation = glm::vec3(3.0f, 0.0f, 0.0f); // Move to the side
    transformComp.Scale = glm::vec3(1.0f);
    
    auto& skeletonComp = m_MultiBoneTestEntity.AddComponent<OloEngine::SkeletonComponent>();
    // Copy skeleton data to component
    skeletonComp.m_ParentIndices = m_MultiBoneTestSkeleton->m_ParentIndices;
    skeletonComp.m_BoneNames = m_MultiBoneTestSkeleton->m_BoneNames;
    skeletonComp.m_LocalTransforms = m_MultiBoneTestSkeleton->m_LocalTransforms;
    skeletonComp.m_GlobalTransforms = m_MultiBoneTestSkeleton->m_GlobalTransforms;
    skeletonComp.m_FinalBoneMatrices = m_MultiBoneTestSkeleton->m_FinalBoneMatrices;
    
    // Create multi-bone animation clips with complex animations
    m_MultiBoneIdleClip = OloEngine::CreateRef<OloEngine::AnimationClip>();
    m_MultiBoneIdleClip->Name = "MultiBoneIdle";
    m_MultiBoneIdleClip->Duration = 3.0f;
    
    // Create animations for each bone
    for (i32 boneIndex = 0; boneIndex < 4; ++boneIndex)
    {
        OloEngine::BoneAnimation boneAnim;
        boneAnim.BoneName = m_MultiBoneTestSkeleton->m_BoneNames[boneIndex];
        
        // Create rotation keyframes with different timing for each bone
        f32 rotationSpeed = 1.0f + (boneIndex * 0.5f);
        for (i32 i = 0; i <= 10; ++i)
        {
            f32 time = (i / 10.0f) * m_MultiBoneIdleClip->Duration;
            f32 angle = time * rotationSpeed;
            
            OloEngine::BoneKeyframe keyframe;
            keyframe.Time = time;
            keyframe.Translation = glm::vec3(0.0f);
            keyframe.Rotation = glm::angleAxis(angle, glm::vec3(0.0f, 1.0f, 0.0f));
            keyframe.Scale = glm::vec3(1.0f);
            boneAnim.Keyframes.push_back(keyframe);
        }
        
        m_MultiBoneIdleClip->BoneAnimations.push_back(boneAnim);
    }
    
    // Add animation state component
    auto& animStateComp = m_MultiBoneTestEntity.AddComponent<OloEngine::AnimationStateComponent>();
    animStateComp.m_CurrentClip = m_MultiBoneIdleClip;
    animStateComp.m_State = OloEngine::AnimationStateComponent::State::Idle;
    animStateComp.CurrentTime = 0.0f;
}

void Sandbox3D::LoadTestAnimatedModel()
{
    OLO_PROFILE_FUNCTION();
    
    // TODO: Implement assimp-based model loading
    // For now, create a placeholder entity that will be replaced with real model loading
    m_ImportedModelEntity = m_TestScene->CreateEntity("ImportedAnimatedModel");
    
    // Position for imported model (different from test entities)
    auto& transformComp = m_ImportedModelEntity.GetComponent<OloEngine::TransformComponent>();
    transformComp.Translation = glm::vec3(-3.0f, 0.0f, 0.0f); // Position to the left
    transformComp.Scale = glm::vec3(1.0f);
    
    // For now, use the same mesh as the simple test but mark it for future replacement
    auto& animMeshComp = m_ImportedModelEntity.AddComponent<OloEngine::AnimatedMeshComponent>();
    animMeshComp.m_Mesh = m_AnimatedTestMesh; // Temporary placeholder
    
    auto& skeletonComp = m_ImportedModelEntity.AddComponent<OloEngine::SkeletonComponent>();
    skeletonComp = m_AnimatedMeshEntity.GetComponent<OloEngine::SkeletonComponent>(); // Copy from main test entity
    
    auto& animStateComp = m_ImportedModelEntity.AddComponent<OloEngine::AnimationStateComponent>();
    animStateComp.m_CurrentClip = m_IdleClip; // Temporary placeholder
    animStateComp.m_State = OloEngine::AnimationStateComponent::State::Idle;
    animStateComp.CurrentTime = 0.0f;
    
    OLO_INFO("Sandbox3D: Imported model entity created (placeholder implementation)");
    OLO_INFO("TODO: Implement assimp-based skeletal animation import");
}

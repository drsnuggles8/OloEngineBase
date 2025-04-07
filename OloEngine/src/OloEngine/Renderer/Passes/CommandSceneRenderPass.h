#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Passes/CommandRenderPass.h"
#include "OloEngine/Renderer/Camera/Camera.h"
#include "OloEngine/Scene/Components.h"
#include "OloEngine/Scene/Scene.h"

namespace OloEngine
{
    /**
     * @brief Command-based render pass for rendering 3D scene content.
     * 
     * This pass handles the rendering of 3D scene objects to an offscreen framebuffer
     * using the command bucket system for efficient batching and sorting.
     */
    class CommandSceneRenderPass : public CommandRenderPass
    {
    public:
        CommandSceneRenderPass();
        ~CommandSceneRenderPass() override = default;

        /**
         * @brief Initialize the scene render pass with a framebuffer specification.
         * @param spec The specification for the framebuffer
         */
        void Init(const FramebufferSpecification& spec) override;

        /**
         * @brief Set up the framebuffer for this pass with the specified dimensions.
         * @param width The width of the framebuffer
         * @param height The height of the framebuffer
         */
        void SetupFramebuffer(u32 width, u32 height) override;
        
        /**
         * @brief Resize the framebuffer when the window is resized.
         * @param width The new width of the framebuffer
         * @param height The new height of the framebuffer
         */
        void ResizeFramebuffer(u32 width, u32 height) override;
        
        /**
         * @brief Reset the pass, recreating any necessary resources.
         */
        void OnReset() override;

        /**
         * @brief Execute the scene rendering, drawing all objects from the command bucket.
         */
        void Execute() override;

        /**
         * @brief Set the scene to render.
         * @param scene The scene to render
         */
        void SetScene(const Ref<Scene>& scene) { m_Scene = scene; }

        /**
         * @brief Set the camera to use for rendering.
         * @param camera The camera to use
         */
        void SetCamera(const Camera& camera, const glm::mat4& transform);

    protected:
        /**
         * @brief Build the command bucket with scene rendering commands.
         * Implements the abstract method from CommandRenderPass.
         * @param bucket Reference to the command bucket to populate
         */
        void BuildCommandBucket(CommandBucket& bucket) override;

        /**
         * @brief Begin rendering to the scene framebuffer.
         * Sets up view/projection matrices for rendering.
         */
        void BeginRender() override;

        /**
         * @brief End rendering to the scene framebuffer.
         */
        void EndRender() override;

    private:
        /**
         * @brief Add mesh render commands for all renderable entities in the scene.
         * @param bucket Reference to the command bucket to populate
         */
        void AddMeshRenderCommands(CommandBucket& bucket);
               
        /**
         * @brief Add any additional scene elements like grid, gizmos, etc.
         * @param bucket Reference to the command bucket to populate
         */
        void AddAuxiliaryRenderCommands(CommandBucket& bucket);

    private:
        Ref<Scene> m_Scene = nullptr; // Scene to render
        
        // Camera data
        Camera m_Camera;
        glm::mat4 m_CameraTransform = glm::mat4(1.0f);
        glm::mat4 m_ViewMatrix = glm::mat4(1.0f);
        glm::mat4 m_ProjectionMatrix = glm::mat4(1.0f);
        
        // State tracking
        bool m_HasValidCamera = false;
    };
}
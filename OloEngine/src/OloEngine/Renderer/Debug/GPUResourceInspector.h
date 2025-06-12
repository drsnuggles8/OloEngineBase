#pragma once

#include "OloEngine/Core/Base.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/Buffer.h"

#include <imgui.h>
#include <glad/gl.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>

// Convenience macros for resource registration (only in debug builds)
#ifdef OLO_DEBUG
    #define OLO_GPU_REGISTER_TEXTURE(id, name, debugName) \
        OloEngine::GPUResourceInspector::GetInstance().RegisterTexture(id, name, debugName)
    #define OLO_GPU_REGISTER_TEXTURE_CUBEMAP(id, name, debugName) \
        OloEngine::GPUResourceInspector::GetInstance().RegisterTextureCubemap(id, name, debugName)
    #define OLO_GPU_REGISTER_BUFFER(id, target, name, debugName) \
        OloEngine::GPUResourceInspector::GetInstance().RegisterBuffer(id, target, name, debugName)
    #define OLO_GPU_REGISTER_FRAMEBUFFER(id, name, debugName) \
        OloEngine::GPUResourceInspector::GetInstance().RegisterFramebuffer(id, name, debugName)
    #define OLO_GPU_UNREGISTER_RESOURCE(id) \
        OloEngine::GPUResourceInspector::GetInstance().UnregisterResource(id)
    #define OLO_GPU_UPDATE_BINDING(id, bound, slot) \
        OloEngine::GPUResourceInspector::GetInstance().UpdateResourceBinding(id, bound, slot)
    #define OLO_GPU_UPDATE_ACTIVE(id, active) \
        OloEngine::GPUResourceInspector::GetInstance().UpdateResourceActiveState(id, active)
#else
    #define OLO_GPU_REGISTER_TEXTURE(id, name, debugName)
    #define OLO_GPU_REGISTER_TEXTURE_CUBEMAP(id, name, debugName)
    #define OLO_GPU_REGISTER_BUFFER(id, target, name, debugName)
    #define OLO_GPU_REGISTER_FRAMEBUFFER(id, name, debugName)
    #define OLO_GPU_UNREGISTER_RESOURCE(id)
    #define OLO_GPU_UPDATE_BINDING(id, bound, slot)
    #define OLO_GPU_UPDATE_ACTIVE(id, active)
#endif

namespace OloEngine
{
    /**
     * @brief GPU Resource Inspector for debugging GPU resources
     * 
     * Provides detailed inspection of GPU resources including textures, buffers,
     * and their properties. Supports real-time preview and content visualization.
     */
    class GPUResourceInspector
    {
    public:
        enum class ResourceType : u8
        {
            Texture2D = 0,
            TextureCubemap,
            VertexBuffer,
            IndexBuffer,
            UniformBuffer,
            Framebuffer,
            COUNT
        };

        struct ResourceInfo
        {
            u32 m_RendererID = 0;
            ResourceType m_Type = ResourceType::Texture2D;
            std::string m_Name;
            std::string m_DebugName;
            sizet m_MemoryUsage = 0;
            bool m_IsActive = true;
            f64 m_CreationTime = 0.0;
            u32 m_BindingSlot = 0;
            bool m_IsBound = false;
        };

        struct TextureInfo : ResourceInfo
        {
            u32 m_Width = 0;
            u32 m_Height = 0;
            u32 m_MipLevels = 1;
            GLenum m_InternalFormat = GL_RGBA8;
            GLenum m_Format = GL_RGBA;
            GLenum m_DataType = GL_UNSIGNED_BYTE;
            bool m_HasMips = false;
            std::vector<u8> m_PreviewData;
            bool m_PreviewDataValid = false;
            u32 m_SelectedMipLevel = 0;
            ImTextureID m_ImGuiTextureID = 0;
        };

        struct BufferInfo : ResourceInfo
        {
            GLenum m_Target = GL_ARRAY_BUFFER;
            GLenum m_Usage = GL_STATIC_DRAW;
            u32 m_Size = 0;
            std::vector<u8> m_ContentPreview;
            bool m_ContentPreviewValid = false;
            u32 m_PreviewOffset = 0;
            u32 m_PreviewSize = 256; // Preview first 256 bytes by default
            u32 m_Stride = 0; // For vertex buffers
        };

        struct FramebufferInfo : ResourceInfo
        {
            u32 m_Width = 0;
            u32 m_Height = 0;
            u32 m_ColorAttachmentCount = 0;
            bool m_HasDepthAttachment = false;
            bool m_HasStencilAttachment = false;
            GLenum m_Status = GL_FRAMEBUFFER_COMPLETE;
            std::vector<GLenum> m_ColorAttachmentFormats;
            GLenum m_DepthAttachmentFormat = GL_NONE;
            GLenum m_StencilAttachmentFormat = GL_NONE;
        };

        // Async texture download data
        struct TextureDownloadRequest
        {
            u32 m_TextureID = 0;
            u32 m_MipLevel = 0;
            u32 m_PBO = 0;
			bool m_InProgress = false;
            f64 m_RequestTime = 0.0;
        };

        GPUResourceInspector();
        ~GPUResourceInspector();

        // Singleton access
        static GPUResourceInspector& GetInstance();

        /**
         * @brief Initialize the GPU resource inspector
         */
        void Initialize();

        /**
         * @brief Shutdown and cleanup resources
         */
        void Shutdown();

        /**
         * @brief Register a texture resource for tracking
         * @param rendererID OpenGL texture ID
         * @param name Resource name/path
         * @param debugName Optional debug name
         */
        void RegisterTexture(u32 rendererID, const std::string& name, const std::string& debugName = "");

        /**
         * @brief Register a texture cubemap resource for tracking
         * @param rendererID OpenGL texture ID
         * @param name Resource name/path
         * @param debugName Optional debug name
         */
        void RegisterTextureCubemap(u32 rendererID, const std::string& name, const std::string& debugName = "");

        /**
         * @brief Register a framebuffer resource for tracking
         * @param rendererID OpenGL framebuffer ID
         * @param name Resource name
         * @param debugName Optional debug name
         */
        void RegisterFramebuffer(u32 rendererID, const std::string& name, const std::string& debugName = "");

        /**
         * @brief Register a buffer resource for tracking
         * @param rendererID OpenGL buffer ID
         * @param target Buffer target (GL_ARRAY_BUFFER, etc.)
         * @param name Resource name
         * @param debugName Optional debug name
         */
        void RegisterBuffer(u32 rendererID, GLenum target, const std::string& name, const std::string& debugName = "");

        /**
         * @brief Update a resource's active state
         * @param rendererID OpenGL resource ID
         * @param isActive Whether the resource is currently active
         */
        void UpdateResourceActiveState(u32 rendererID, bool isActive);

        /**
         * @brief Update resource binding information
         * @param rendererID OpenGL resource ID
         * @param isBound Whether the resource is currently bound
         * @param bindingSlot The binding slot/unit (for textures, uniform buffers, etc.)
         */
        void UpdateResourceBinding(u32 rendererID, bool isBound, u32 bindingSlot = 0);

        /**
         * @brief Unregister a resource (called when resource is destroyed)
         * @param rendererID OpenGL resource ID
         */
        void UnregisterResource(u32 rendererID);

        /**
         * @brief Update resource binding states
         */
        void UpdateBindingStates();

        /**
         * @brief Render the debug view in ImGui
         * @param open Pointer to boolean controlling window visibility
         * @param title Window title
         */
        void RenderDebugView(bool* open = nullptr, const char* title = "GPU Resource Inspector");

        /**
         * @brief Export resource information to CSV
         * @param filename Output filename
         */
        void ExportToCSV(const std::string& filename);

        /**
         * @brief Get total number of tracked resources
         */
        u32 GetResourceCount() const { return static_cast<u32>(m_Resources.size()); }

        /**
         * @brief Get memory usage for a specific resource type
         */
        sizet GetMemoryUsage(ResourceType type) const;

        /**
         * @brief Get total memory usage of all tracked resources
         */
        sizet GetTotalMemoryUsage() const;

    private:
        void QueryTextureInfo(TextureInfo& info);
        void QueryTextureCubemapInfo(TextureInfo& info);
        void QueryBufferInfo(BufferInfo& info);
        void QueryFramebufferInfo(FramebufferInfo& info);
        void RequestTextureDownload(TextureInfo& info, u32 mipLevel);
        void ProcessTextureDownloads();
        void UpdateTexturePreview(TextureInfo& info);
        void UpdateBufferPreview(BufferInfo& info);
        
        void RenderResourceTree();
        void RenderResourceDetails();
        void RenderTexturePreview(TextureInfo& info);
        void RenderBufferContent(BufferInfo& info);
        void RenderFramebufferDetails(FramebufferInfo& info);
        void RenderResourceStatistics();
        
        std::string FormatTextureFormat(GLenum format) const;
        std::string FormatBufferUsage(GLenum usage) const;
        std::string FormatMemorySize(sizet bytes) const;
        const char* GetResourceTypeName(ResourceType type) const;
        const char* GetBufferTargetName(GLenum target) const;

    private:
        std::unordered_map<u32, std::unique_ptr<ResourceInfo>> m_Resources;
        std::vector<TextureDownloadRequest> m_TextureDownloads;
        
        // UI state
        u32 m_SelectedResourceID = 0;
        ResourceType m_FilterType = ResourceType::COUNT; // No filter by default
        std::string m_SearchFilter;
        bool m_ShowInactiveResources = true;
        bool m_AutoUpdatePreviews = true;
        
        // Statistics
        std::array<u32, static_cast<sizet>(ResourceType::COUNT)> m_ResourceCounts{};
        std::array<sizet, static_cast<sizet>(ResourceType::COUNT)> m_MemoryUsageByType{};
        
        // Threading
        mutable std::mutex m_ResourceMutex;
        
        // OpenGL state backup
        GLint m_PreviousTextureBinding = 0;
        GLint m_PreviousBufferBinding = 0;
        
        bool m_IsInitialized = false;
    };
}

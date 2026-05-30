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
#include <utility>

#include "OloEngine/Threading/Mutex.h"

// Convenience macros for resource registration (only in debug builds)
#ifdef OLO_DEBUG
#define OLO_GPU_REGISTER_TEXTURE(id, name, debugName)                                        \
    do                                                                                       \
    {                                                                                        \
        OloEngine::GPUResourceInspector::GetInstance().RegisterTexture(id, name, debugName); \
    } while (false)
#define OLO_GPU_REGISTER_TEXTURE_CUBEMAP(id, name, debugName)                                       \
    do                                                                                              \
    {                                                                                               \
        OloEngine::GPUResourceInspector::GetInstance().RegisterTextureCubemap(id, name, debugName); \
    } while (false)
#define OLO_GPU_REGISTER_BUFFER(id, target, name, debugName)                                        \
    do                                                                                              \
    {                                                                                               \
        OloEngine::GPUResourceInspector::GetInstance().RegisterBuffer(id, target, name, debugName); \
    } while (false)
#define OLO_GPU_REGISTER_FRAMEBUFFER(id, name, debugName)                                        \
    do                                                                                           \
    {                                                                                            \
        OloEngine::GPUResourceInspector::GetInstance().RegisterFramebuffer(id, name, debugName); \
    } while (false)
#define OLO_GPU_UNREGISTER_RESOURCE(id)                                        \
    do                                                                         \
    {                                                                          \
        OloEngine::GPUResourceInspector::GetInstance().UnregisterResource(id); \
    } while (false)
#define OLO_GPU_UPDATE_BINDING(id, bound, slot)                                                \
    do                                                                                         \
    {                                                                                          \
        OloEngine::GPUResourceInspector::GetInstance().UpdateResourceBinding(id, bound, slot); \
    } while (false)
#define OLO_GPU_UPDATE_ACTIVE(id, active)                                                     \
    do                                                                                        \
    {                                                                                         \
        OloEngine::GPUResourceInspector::GetInstance().UpdateResourceActiveState(id, active); \
    } while (false)
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
    // @brief GPU Resource Inspector for debugging GPU resources
    //
    // Provides detailed inspection of GPU resources including textures, buffers,
    // and their properties. Supports real-time preview and content visualization.
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
            u32 m_SelectedCubemapFace = 0; // 0..5 = +X,-X,+Y,-Y,+Z,-Z (cubemaps only)
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
            u32 m_Stride = 0;        // For vertex buffers
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
            u32 m_FaceIndex = 0; // Cubemap face (0..5 = +X,-X,+Y,-Y,+Z,-Z); ignored for Texture2D
            u32 m_PBO = 0;
            GLsync m_Fence = nullptr; // Modern OpenGL 3.2+ sync object for completion detection
            bool m_InProgress = false;
            f64 m_RequestTime = 0.0;
        };

        GPUResourceInspector();
        ~GPUResourceInspector();

        // Singleton access
        static GPUResourceInspector& GetInstance();

        // @brief Initialize the GPU resource inspector
        void Initialize();

        // @brief Shutdown and cleanup resources
        void Shutdown();

        // @brief Register a texture resource for tracking
        // @param rendererID OpenGL texture ID
        // @param name Resource name/path
        // @param debugName Optional debug name
        void RegisterTexture(u32 rendererID, const std::string& name, const std::string& debugName = "");

        // @brief Register a texture cubemap resource for tracking
        // @param rendererID OpenGL texture ID
        // @param name Resource name/path
        // @param debugName Optional debug name
        void RegisterTextureCubemap(u32 rendererID, const std::string& name, const std::string& debugName = "");

        // @brief Register a framebuffer resource for tracking
        // @param rendererID OpenGL framebuffer ID
        // @param name Resource name
        // @param debugName Optional debug name
        void RegisterFramebuffer(u32 rendererID, const std::string& name, const std::string& debugName = "");

        // @brief Register a buffer resource for tracking
        // @param rendererID OpenGL buffer ID
        // @param target Buffer target (GL_ARRAY_BUFFER, etc.)
        // @param name Resource name
        // @param debugName Optional debug name
        void RegisterBuffer(u32 rendererID, GLenum target, const std::string& name, const std::string& debugName = "");

        // @brief Update a resource's active state
        // @param rendererID OpenGL resource ID
        // @param isActive Whether the resource is currently active
        void UpdateResourceActiveState(u32 rendererID, bool isActive);

        // @brief Update resource binding information
        // @param rendererID OpenGL resource ID
        // @param isBound Whether the resource is currently bound
        // @param bindingSlot The binding slot/unit (for textures, uniform buffers, etc.)
        void UpdateResourceBinding(u32 rendererID, bool isBound, u32 bindingSlot = 0);

        // @brief Unregister a resource (called when resource is destroyed)
        // @param rendererID OpenGL resource ID
        void UnregisterResource(u32 rendererID);

        // @brief Update resource binding states
        void UpdateBindingStates();

        // @brief Render the debug view in ImGui
        // @param open Pointer to boolean controlling window visibility
        // @param title Window title
        void RenderDebugView(bool* open = nullptr, const char* title = "GPU Resource Inspector");

        // @brief Export resource information to CSV
        // @param filename Output filename
        void ExportToCSV(const std::string& filename);

        // @brief Save a tracked texture to an image file (PNG for 8-bit formats, HDR for float formats).
        //        Extension on `filePath` selects the encoder. For cubemaps, `faceIndex` (0..5 = +X,-X,+Y,-Y,+Z,-Z)
        //        picks the face to write; ignored for Texture2D. Requires an active OpenGL 4.5+ context.
        //        Pixels are written in raw GPU memory order (GL bottom-left origin, no software flip) so
        //        the file faithfully represents what's in the texture — Texture2Ds loaded through
        //        OpenGLTexture2D are pre-flipped on upload and so will appear right-side-up when opened,
        //        while cubemap faces (loaded without that flip) will look vertically mirrored.
        // @return true on success; false if the texture is invalid, the format is unsupported, or the file write fails.
        bool SaveTextureToFile(const TextureInfo& info, const std::string& filePath, u32 mipLevel, u32 faceIndex = 0) const;

        // @brief Get total number of tracked resources
        u32 GetResourceCount() const
        {
            return static_cast<u32>(m_Resources.size());
        }

        // @brief Get memory usage for a specific resource type
        sizet GetMemoryUsage(ResourceType type) const;

        // @brief Get total memory usage of all tracked resources
        sizet GetTotalMemoryUsage() const;

      private:
        void QueryTextureInfo(TextureInfo& info) const;
        void QueryTextureCubemapInfo(TextureInfo& info) const;
        void QueryBufferInfo(BufferInfo& info) const;
        void QueryFramebufferInfo(FramebufferInfo& info) const;
        void RequestTextureDownload(TextureInfo& info, u32 mipLevel, u32 faceIndex = 0);
        void ProcessTextureDownloads();
        void CompleteTextureDownload(TextureInfo& info, const TextureDownloadRequest& request) const;
        void UpdateTexturePreview(TextureInfo& info);
        void UpdateBufferPreview(BufferInfo& info) const;

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

        // Texture memory calculation utilities
        sizet CalculateAccurateTextureMemoryUsage(u32 textureId, GLenum target, GLenum internalFormat,
                                                  u32 width, u32 height, u32 mipLevels) const;
        sizet CalculateCompressedTextureMemory(u32 textureId, GLenum target, GLenum internalFormat,
                                               u32 /*width*/, u32 /*height*/, u32 mipLevels) const;
        sizet CalculateUncompressedTextureMemory(u32 width, u32 height, u32 bytesPerPixel, u32 mipLevels) const;
        u32 GetUncompressedBytesPerPixel(GLenum internalFormat) const;
        u32 GetCompressedBlockSize(GLenum internalFormat) const;

        // Buffer binding utility
        static GLenum GetBufferBindingQuery(GLenum target);

      private:
        // Deferred Save-to-File request. The Save button populates this snapshot
        // under m_ResourceMutex; RenderDebugView processes it AFTER the mutex
        // is released so the modal file dialog + GL readback can't block
        // background registrations / unregistrations.
        struct PendingSaveRequest
        {
            bool m_Active = false;
            TextureInfo m_Info{}; // member-wise copy; safe to use without the mutex
        };
        void ProcessPendingSaveRequest();

      private:
        std::unordered_map<u32, std::unique_ptr<ResourceInfo>> m_Resources;
        std::vector<TextureDownloadRequest> m_TextureDownloads;

        // UI state
        u32 m_SelectedResourceID = 0;
        ResourceType m_FilterType = ResourceType::COUNT; // No filter by default
        std::string m_SearchFilter;
        bool m_ShowInactiveResources = true;
        bool m_AutoUpdatePreviews = true;
        PendingSaveRequest m_PendingSaveRequest;

        // Statistics
        std::array<u32, static_cast<sizet>(std::to_underlying(ResourceType::COUNT))> m_ResourceCounts{};
        std::array<sizet, static_cast<sizet>(std::to_underlying(ResourceType::COUNT))> m_MemoryUsageByType{};
        // Threading
        mutable FMutex m_ResourceMutex;

        bool m_IsInitialized = false;
    };
} // namespace OloEngine

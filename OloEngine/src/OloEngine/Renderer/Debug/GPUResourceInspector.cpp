#include "GPUResourceInspector.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Utils/PlatformUtils.h"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>

namespace OloEngine
{
    GPUResourceInspector::GPUResourceInspector()
    {
        m_ResourceCounts.fill(0);
        m_MemoryUsageByType.fill(0);
    }    GPUResourceInspector::~GPUResourceInspector()
    {
        if (m_IsInitialized)
        {
            Shutdown();
        }
    }

    GPUResourceInspector& GPUResourceInspector::GetInstance()
    {
        static GPUResourceInspector instance;
        return instance;
    }

    void GPUResourceInspector::Initialize()
    {
        if (m_IsInitialized)
            return;

        OLO_CORE_INFO("Initializing GPU Resource Inspector");
        m_IsInitialized = true;
    }

    void GPUResourceInspector::Shutdown()
    {
        if (!m_IsInitialized)
            return;

        OLO_CORE_INFO("Shutting down GPU Resource Inspector");
        
        // Clean up any pending texture downloads
        for (auto& download : m_TextureDownloads)
        {
            if (download.m_PBO != 0)
            {
                glDeleteBuffers(1, &download.m_PBO);
            }
        }
        m_TextureDownloads.clear();

        // Clean up resources
        {
            std::lock_guard<std::mutex> lock(m_ResourceMutex);
            m_Resources.clear();
        }

        m_IsInitialized = false;
    }

    void GPUResourceInspector::RegisterTexture(u32 rendererID, const std::string& name, const std::string& debugName)
    {
        if (!m_IsInitialized || rendererID == 0)
            return;

        std::lock_guard<std::mutex> lock(m_ResourceMutex);
        
        auto textureInfo = CreateScope<TextureInfo>();
        textureInfo->m_RendererID = rendererID;
        textureInfo->m_Type = ResourceType::Texture2D;
        textureInfo->m_Name = name;
        textureInfo->m_DebugName = debugName.empty() ? name : debugName;
        textureInfo->m_CreationTime = Application::GetTime();
        
        // Query texture properties immediately
        QueryTextureInfo(*textureInfo);
        
        m_Resources[rendererID] = std::move(textureInfo);
        m_ResourceCounts[static_cast<size_t>(ResourceType::Texture2D)]++;
        
        OLO_CORE_TRACE("Registered texture: {} (ID: {})", name, rendererID);
    }

    void GPUResourceInspector::RegisterBuffer(u32 rendererID, GLenum target, const std::string& name, const std::string& debugName)
    {
        if (!m_IsInitialized || rendererID == 0)
            return;

        std::lock_guard<std::mutex> lock(m_ResourceMutex);
        
        auto bufferInfo = CreateScope<BufferInfo>();
        bufferInfo->m_RendererID = rendererID;
        bufferInfo->m_Target = target;
        bufferInfo->m_Name = name;
        bufferInfo->m_DebugName = debugName.empty() ? name : debugName;
        bufferInfo->m_CreationTime = Application::GetTime();
        
        // Determine resource type based on target
        switch (target)
        {
            case GL_ARRAY_BUFFER:
                bufferInfo->m_Type = ResourceType::VertexBuffer;
                break;
            case GL_ELEMENT_ARRAY_BUFFER:
                bufferInfo->m_Type = ResourceType::IndexBuffer;
                break;
            case GL_UNIFORM_BUFFER:
                bufferInfo->m_Type = ResourceType::UniformBuffer;
                break;
            default:
                bufferInfo->m_Type = ResourceType::VertexBuffer; // Default fallback
                break;
        }
        
        // Query buffer properties immediately
        QueryBufferInfo(*bufferInfo);
        
        m_Resources[rendererID] = std::move(bufferInfo);
        m_ResourceCounts[static_cast<size_t>(bufferInfo->m_Type)]++;
        
        OLO_CORE_TRACE("Registered buffer: {} (ID: {}, Target: 0x{:X})", name, rendererID, target);
    }

    void GPUResourceInspector::UnregisterResource(u32 rendererID)
    {
        if (!m_IsInitialized || rendererID == 0)
            return;

        std::lock_guard<std::mutex> lock(m_ResourceMutex);
        
        auto it = m_Resources.find(rendererID);
        if (it != m_Resources.end())
        {
            ResourceType type = it->second->m_Type;
            m_ResourceCounts[static_cast<size_t>(type)]--;
            m_Resources.erase(it);
            
            OLO_CORE_TRACE("Unregistered resource: ID {}", rendererID);
        }
    }

    void GPUResourceInspector::UpdateBindingStates()
    {
        if (!m_IsInitialized)
            return;

        // This would be called by the renderer to update binding states
        // For now, we'll implement basic texture binding detection
        std::lock_guard<std::mutex> lock(m_ResourceMutex);
        
        for (auto& [id, resource] : m_Resources)
        {
            resource->m_IsBound = false; // Reset binding state
            
            if (resource->m_Type == ResourceType::Texture2D)
            {
                // Check if this texture is bound to any texture unit
                // This is a simplified check - in practice, we'd need to track all texture units
                GLint currentTexture;
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &currentTexture);
                if (static_cast<u32>(currentTexture) == resource->m_RendererID)
                {
                    resource->m_IsBound = true;
                    resource->m_BindingSlot = 0; // Assume texture unit 0 for simplicity
                }
            }
        }
    }

    void GPUResourceInspector::QueryTextureInfo(TextureInfo& info)
    {
        // Save current texture binding
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &m_PreviousTextureBinding);
        
        // Bind the texture temporarily to query its properties
        glBindTexture(GL_TEXTURE_2D, info.m_RendererID);
        
        GLint width, height, internalFormat, format, type;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internalFormat);
        
        info.m_Width = static_cast<u32>(width);
        info.m_Height = static_cast<u32>(height);
        info.m_InternalFormat = static_cast<GLenum>(internalFormat);
        
        // Determine format and type based on internal format
        switch (internalFormat)
        {
            case GL_RGBA8:
                info.m_Format = GL_RGBA;
                info.m_DataType = GL_UNSIGNED_BYTE;
                break;
            case GL_RGB8:
                info.m_Format = GL_RGB;
                info.m_DataType = GL_UNSIGNED_BYTE;
                break;
            case GL_R8:
                info.m_Format = GL_RED;
                info.m_DataType = GL_UNSIGNED_BYTE;
                break;
            default:
                info.m_Format = GL_RGBA;
                info.m_DataType = GL_UNSIGNED_BYTE;
                break;
        }
        
        // Calculate memory usage (simplified)
        u32 bytesPerPixel = 4; // Assume RGBA for now
        if (internalFormat == GL_RGB8) bytesPerPixel = 3;
        else if (internalFormat == GL_R8) bytesPerPixel = 1;
        
        info.m_MemoryUsage = static_cast<size_t>(width * height * bytesPerPixel);
        
        // Check for mip levels
        GLint maxLevel;
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &maxLevel);
        info.m_MipLevels = static_cast<u32>(maxLevel + 1);
        info.m_HasMips = maxLevel > 0;
        
        // Restore previous texture binding
        glBindTexture(GL_TEXTURE_2D, m_PreviousTextureBinding);
    }

    void GPUResourceInspector::QueryBufferInfo(BufferInfo& info)
    {
        // Save current buffer binding
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &m_PreviousBufferBinding);
        
        // Bind the buffer temporarily to query its properties
        glBindBuffer(info.m_Target, info.m_RendererID);
        
        GLint size, usage;
        glGetBufferParameteriv(info.m_Target, GL_BUFFER_SIZE, &size);
        glGetBufferParameteriv(info.m_Target, GL_BUFFER_USAGE, &usage);
        
        info.m_Size = static_cast<u32>(size);
        info.m_Usage = static_cast<GLenum>(usage);
        info.m_MemoryUsage = static_cast<size_t>(size);
        
        // Restore previous buffer binding
        if (info.m_Target == GL_ARRAY_BUFFER)
            glBindBuffer(GL_ARRAY_BUFFER, m_PreviousBufferBinding);
    }

    void GPUResourceInspector::ProcessTextureDownloads()
    {
        // Process async texture downloads (simplified implementation)
        for (auto& download : m_TextureDownloads)
        {
            if (download.m_InProgress)
            {
                // Check if download is complete (simplified - should use glMapBuffer in practice)
                download.m_InProgress = false;
                
                // Find the corresponding texture and update preview data
                auto it = m_Resources.find(download.m_TextureID);
                if (it != m_Resources.end() && it->second->m_Type == ResourceType::Texture2D)
                {
                    auto* texInfo = static_cast<TextureInfo*>(it->second.get());
                    UpdateTexturePreview(*texInfo);
                }
            }
        }
    }

    void GPUResourceInspector::UpdateTexturePreview(TextureInfo& info)
    {
        if (info.m_PreviewDataValid)
            return;

        // Save current texture binding
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &m_PreviousTextureBinding);
        
        // Bind texture and download data
        glBindTexture(GL_TEXTURE_2D, info.m_RendererID);
        
        // Calculate preview data size (limit to reasonable size)
        u32 previewWidth = std::min(info.m_Width, 256u);
        u32 previewHeight = std::min(info.m_Height, 256u);
        u32 bytesPerPixel = (info.m_Format == GL_RGBA) ? 4 : (info.m_Format == GL_RGB) ? 3 : 1;
        
        info.m_PreviewData.resize(previewWidth * previewHeight * bytesPerPixel);
        
        // Download texture data (this could cause stalls - should be async in production)
        glGetTexImage(GL_TEXTURE_2D, info.m_SelectedMipLevel, info.m_Format, info.m_DataType, info.m_PreviewData.data());
        
        info.m_PreviewDataValid = true;
        
        // Restore texture binding
        glBindTexture(GL_TEXTURE_2D, m_PreviousTextureBinding);
    }

    void GPUResourceInspector::UpdateBufferPreview(BufferInfo& info)
    {
        if (info.m_ContentPreviewValid)
            return;

        // Save current buffer binding
        if (info.m_Target == GL_ARRAY_BUFFER)
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &m_PreviousBufferBinding);
        
        // Bind buffer and map data
        glBindBuffer(info.m_Target, info.m_RendererID);
        
        u32 previewSize = std::min(info.m_Size, info.m_PreviewSize);
        info.m_ContentPreview.resize(previewSize);
        
        // Map buffer and copy data
        void* data = glMapBuffer(info.m_Target, GL_READ_ONLY);
        if (data)
        {
            memcpy(info.m_ContentPreview.data(), static_cast<const u8*>(data) + info.m_PreviewOffset, previewSize);
            glUnmapBuffer(info.m_Target);
            info.m_ContentPreviewValid = true;
        }
        
        // Restore buffer binding
        if (info.m_Target == GL_ARRAY_BUFFER)
            glBindBuffer(GL_ARRAY_BUFFER, m_PreviousBufferBinding);
    }

    size_t GPUResourceInspector::GetMemoryUsage(ResourceType type) const
    {
        std::lock_guard<std::mutex> lock(m_ResourceMutex);
        return m_MemoryUsageByType[static_cast<size_t>(type)];
    }

    size_t GPUResourceInspector::GetTotalMemoryUsage() const
    {
        std::lock_guard<std::mutex> lock(m_ResourceMutex);
        size_t total = 0;
        for (const auto& [id, resource] : m_Resources)
        {
            total += resource->m_MemoryUsage;
        }
        return total;
    }

    void GPUResourceInspector::RenderDebugView(bool* open, const char* title)
    {
        if (!m_IsInitialized)
            return;

        if (!ImGui::Begin(title, open, ImGuiWindowFlags_MenuBar))
        {
            ImGui::End();
            return;
        }

        // Menu bar
        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("View"))
            {
                ImGui::Checkbox("Show Inactive Resources", &m_ShowInactiveResources);
                ImGui::Checkbox("Auto Update Previews", &m_AutoUpdatePreviews);
                ImGui::EndMenu();
            }
            
            if (ImGui::BeginMenu("Export"))
            {
                if (ImGui::MenuItem("Export to CSV"))
                {
                    ExportToCSV("gpu_resources.csv");
                }
                ImGui::EndMenu();
            }
            
            ImGui::EndMenuBar();
        }

        // Statistics section
        RenderResourceStatistics();
        
        ImGui::Separator();
        
        // Filter controls
        ImGui::Text("Filters:");
        ImGui::SameLine();
        
        const char* typeNames[] = { "All", "Textures", "Cubemaps", "Vertex Buffers", "Index Buffers", "Uniform Buffers", "Framebuffers" };
        int currentFilter = static_cast<int>(m_FilterType) + 1;
        if (ImGui::Combo("Type", &currentFilter, typeNames, IM_ARRAYSIZE(typeNames)))
        {
            m_FilterType = (currentFilter == 0) ? ResourceType::COUNT : static_cast<ResourceType>(currentFilter - 1);
        }
        
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputText("Search", &m_SearchFilter);
        
        ImGui::Separator();
        
        // Split view: resource tree on left, details on right
        static float leftPaneWidth = 300.0f;
        ImGui::BeginChild("ResourceTree", ImVec2(leftPaneWidth, -1), true);
        RenderResourceTree();
        ImGui::EndChild();
        
        ImGui::SameLine();
        
        ImGui::BeginChild("ResourceDetails", ImVec2(-1, -1), true);
        RenderResourceDetails();
        ImGui::EndChild();

        ImGui::End();
    }

    void GPUResourceInspector::RenderResourceTree()
    {
        std::lock_guard<std::mutex> lock(m_ResourceMutex);
        
        ImGui::Text("Resources (%u)", GetResourceCount());
        ImGui::Separator();
        
        // Group resources by type
        std::unordered_map<ResourceType, std::vector<ResourceInfo*>> groupedResources;
        
        for (const auto& [id, resource] : m_Resources)
        {
            // Apply filters
            if (m_FilterType != ResourceType::COUNT && resource->m_Type != m_FilterType)
                continue;
                
            if (!m_SearchFilter.empty())
            {
                std::string searchLower = m_SearchFilter;
                std::transform(searchLower.begin(), searchLower.end(), searchLower.begin(), ::tolower);
                
                std::string nameLower = resource->m_Name;
                std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
                
                if (nameLower.find(searchLower) == std::string::npos)
                    continue;
            }
            
            if (!m_ShowInactiveResources && !resource->m_IsActive)
                continue;
            
            groupedResources[resource->m_Type].push_back(resource.get());
        }
        
        // Render tree nodes by type
        for (int i = 0; i < static_cast<int>(ResourceType::COUNT); ++i)
        {
            ResourceType type = static_cast<ResourceType>(i);
            const auto& resources = groupedResources[type];
            
            if (resources.empty())
                continue;
                
            if (ImGui::TreeNode(GetResourceTypeName(type)))
            {
                for (const auto* resource : resources)
                {
                    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                    if (resource->m_RendererID == m_SelectedResourceID)
                        flags |= ImGuiTreeNodeFlags_Selected;
                    
                    std::string label = resource->m_DebugName.empty() ? resource->m_Name : resource->m_DebugName;
                    if (label.empty())
                        label = "Unnamed Resource";
                    
                    // Add memory usage to label
                    label += " (" + FormatMemorySize(resource->m_MemoryUsage) + ")";
                    
                    if (resource->m_IsBound)
                        label += " [BOUND]";
                    
                    ImGui::TreeNodeEx(label.c_str(), flags);
                    
                    if (ImGui::IsItemClicked())
                    {
                        m_SelectedResourceID = resource->m_RendererID;
                    }
                }
                ImGui::TreePop();
            }
        }
    }

    void GPUResourceInspector::RenderResourceDetails()
    {
        if (m_SelectedResourceID == 0)
        {
            ImGui::Text("Select a resource to view details");
            return;
        }
        
        std::lock_guard<std::mutex> lock(m_ResourceMutex);
        
        auto it = m_Resources.find(m_SelectedResourceID);
        if (it == m_Resources.end())
        {
            ImGui::Text("Selected resource not found");
            return;
        }
        
        ResourceInfo* resource = it->second.get();
        
        ImGui::Text("Resource Details");
        ImGui::Separator();
        
        ImGui::Text("ID: %u", resource->m_RendererID);
        ImGui::Text("Type: %s", GetResourceTypeName(resource->m_Type));
        ImGui::Text("Name: %s", resource->m_Name.c_str());
        if (!resource->m_DebugName.empty() && resource->m_DebugName != resource->m_Name)
            ImGui::Text("Debug Name: %s", resource->m_DebugName.c_str());
        ImGui::Text("Memory Usage: %s", FormatMemorySize(resource->m_MemoryUsage).c_str());
        ImGui::Text("Active: %s", resource->m_IsActive ? "Yes" : "No");
        ImGui::Text("Bound: %s", resource->m_IsBound ? "Yes" : "No");
        if (resource->m_IsBound)
            ImGui::Text("Binding Slot: %u", resource->m_BindingSlot);
        
        ImGui::Separator();
        
        // Type-specific details
        if (resource->m_Type == ResourceType::Texture2D)
        {
            RenderTexturePreview(*static_cast<TextureInfo*>(resource));
        }
        else if (resource->m_Type == ResourceType::VertexBuffer || 
                 resource->m_Type == ResourceType::IndexBuffer || 
                 resource->m_Type == ResourceType::UniformBuffer)
        {
            RenderBufferContent(*static_cast<BufferInfo*>(resource));
        }
    }

    void GPUResourceInspector::RenderTexturePreview(TextureInfo& info)
    {
        ImGui::Text("Texture Properties");
        ImGui::Text("Dimensions: %u x %u", info.m_Width, info.m_Height);
        ImGui::Text("Internal Format: %s", FormatTextureFormat(info.m_InternalFormat).c_str());
        ImGui::Text("Mip Levels: %u", info.m_MipLevels);
        ImGui::Text("Has Mipmaps: %s", info.m_HasMips ? "Yes" : "No");
        
        if (info.m_HasMips)
        {
            ImGui::SliderInt("Mip Level", reinterpret_cast<int*>(&info.m_SelectedMipLevel), 0, static_cast<int>(info.m_MipLevels - 1));
            if (ImGui::IsItemEdited())
            {
                info.m_PreviewDataValid = false; // Force refresh
            }
        }
        
        ImGui::Separator();
        
        if (ImGui::Button("Refresh Preview"))
        {
            info.m_PreviewDataValid = false;
        }
        
        if (m_AutoUpdatePreviews || ImGui::IsItemClicked())
        {
            UpdateTexturePreview(info);
        }
        
        if (info.m_PreviewDataValid && !info.m_PreviewData.empty())
        {
            // Create ImGui texture if not already created
            if (info.m_ImGuiTextureID == nullptr)
            {
                // This is simplified - in practice, we'd create a proper ImGui texture
                info.m_ImGuiTextureID = reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(info.m_RendererID));
            }
            
            // Display texture preview
            ImVec2 imageSize(256, 256);
            ImGui::Image(info.m_ImGuiTextureID, imageSize);
            
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Texture Preview\nSize: %u x %u\nClick to view full size", info.m_Width, info.m_Height);
            }
        }
        else
        {
            ImGui::Text("Preview not available");
        }
    }

    void GPUResourceInspector::RenderBufferContent(BufferInfo& info)
    {
        ImGui::Text("Buffer Properties");
        ImGui::Text("Target: 0x%X", info.m_Target);
        ImGui::Text("Usage: %s", FormatBufferUsage(info.m_Usage).c_str());
        ImGui::Text("Size: %s", FormatMemorySize(info.m_Size).c_str());
        
        if (info.m_Type == ResourceType::VertexBuffer)
        {
            ImGui::InputInt("Stride", reinterpret_cast<int*>(&info.m_Stride));
        }
        
        ImGui::Separator();
        
        ImGui::InputInt("Preview Offset", reinterpret_cast<int*>(&info.m_PreviewOffset));
        ImGui::InputInt("Preview Size", reinterpret_cast<int*>(&info.m_PreviewSize));
        
        if (ImGui::Button("Refresh Content"))
        {
            info.m_ContentPreviewValid = false;
        }
        
        if (m_AutoUpdatePreviews || ImGui::IsItemClicked())
        {
            UpdateBufferPreview(info);
        }
        
        if (info.m_ContentPreviewValid && !info.m_ContentPreview.empty())
        {
            ImGui::Separator();
            ImGui::Text("Content Preview (Hex Dump):");
            
            // Hex dump display
            const u8* data = info.m_ContentPreview.data();
            size_t size = info.m_ContentPreview.size();
            
            for (size_t i = 0; i < size; i += 16)
            {
                // Address
                ImGui::Text("%08X: ", static_cast<u32>(info.m_PreviewOffset + i));
                ImGui::SameLine();
                
                // Hex bytes
                for (size_t j = 0; j < 16 && (i + j) < size; ++j)
                {
                    ImGui::SameLine();
                    ImGui::Text("%02X", data[i + j]);
                }
                
                // ASCII representation
                ImGui::SameLine();
                ImGui::Text("  ");
                for (size_t j = 0; j < 16 && (i + j) < size; ++j)
                {
                    ImGui::SameLine();
                    char c = static_cast<char>(data[i + j]);
                    ImGui::Text("%c", (c >= 32 && c <= 126) ? c : '.');
                }
            }
        }
        else
        {
            ImGui::Text("Content preview not available");
        }
    }

    void GPUResourceInspector::RenderResourceStatistics()
    {
        ImGui::Text("Statistics");
        ImGui::Separator();
        
        ImGui::Text("Total Resources: %u", GetResourceCount());
        ImGui::Text("Total Memory: %s", FormatMemorySize(GetTotalMemoryUsage()).c_str());
        
        // Memory usage by type
        for (int i = 0; i < static_cast<int>(ResourceType::COUNT); ++i)
        {
            ResourceType type = static_cast<ResourceType>(i);
            u32 count = m_ResourceCounts[i];
            if (count > 0)
            {
                size_t memory = GetMemoryUsage(type);
                ImGui::Text("%s: %u (%s)", GetResourceTypeName(type), count, FormatMemorySize(memory).c_str());
            }
        }
    }

    void GPUResourceInspector::ExportToCSV(const std::string& filename)
    {
        std::ofstream file(filename);
        if (!file.is_open())
        {
            OLO_CORE_ERROR("Failed to open file for export: {}", filename);
            return;
        }
        
        // CSV header
        file << "ID,Type,Name,DebugName,MemoryUsage,Active,Bound,CreationTime\n";
        
        std::lock_guard<std::mutex> lock(m_ResourceMutex);
        
        for (const auto& [id, resource] : m_Resources)
        {
            file << resource->m_RendererID << ","
                 << GetResourceTypeName(resource->m_Type) << ","
                 << "\"" << resource->m_Name << "\","
                 << "\"" << resource->m_DebugName << "\","
                 << resource->m_MemoryUsage << ","
                 << (resource->m_IsActive ? "true" : "false") << ","
                 << (resource->m_IsBound ? "true" : "false") << ","
                 << resource->m_CreationTime << "\n";
        }
        
        file.close();
        OLO_CORE_INFO("Exported GPU resource information to: {}", filename);
    }

    std::string GPUResourceInspector::FormatTextureFormat(GLenum format) const
    {
        switch (format)
        {
            case GL_RGBA8: return "RGBA8";
            case GL_RGB8: return "RGB8";
            case GL_R8: return "R8";
            case GL_RGBA32F: return "RGBA32F";
            case GL_RGB32F: return "RGB32F";
            case GL_R32F: return "R32F";
            case GL_DEPTH24_STENCIL8: return "DEPTH24_STENCIL8";
            default: return "Unknown (0x" + std::to_string(format) + ")";
        }
    }

    std::string GPUResourceInspector::FormatBufferUsage(GLenum usage) const
    {
        switch (usage)
        {
            case GL_STATIC_DRAW: return "STATIC_DRAW";
            case GL_DYNAMIC_DRAW: return "DYNAMIC_DRAW";
            case GL_STREAM_DRAW: return "STREAM_DRAW";
            case GL_STATIC_READ: return "STATIC_READ";
            case GL_DYNAMIC_READ: return "DYNAMIC_READ";
            case GL_STREAM_READ: return "STREAM_READ";
            case GL_STATIC_COPY: return "STATIC_COPY";
            case GL_DYNAMIC_COPY: return "DYNAMIC_COPY";
            case GL_STREAM_COPY: return "STREAM_COPY";
            default: return "Unknown (0x" + std::to_string(usage) + ")";
        }
    }

    std::string GPUResourceInspector::FormatMemorySize(size_t bytes) const
    {
        const char* units[] = { "B", "KB", "MB", "GB" };
        int unit = 0;
        double size = static_cast<double>(bytes);
        
        while (size >= 1024.0 && unit < 3)
        {
            size /= 1024.0;
            unit++;
        }
        
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << size << " " << units[unit];
        return oss.str();
    }

    const char* GPUResourceInspector::GetResourceTypeName(ResourceType type) const
    {
        switch (type)
        {
            case ResourceType::Texture2D: return "Texture2D";
            case ResourceType::TextureCubemap: return "TextureCubemap";
            case ResourceType::VertexBuffer: return "Vertex Buffer";
            case ResourceType::IndexBuffer: return "Index Buffer";
            case ResourceType::UniformBuffer: return "Uniform Buffer";
            case ResourceType::Framebuffer: return "Framebuffer";
            default: return "Unknown";
        }
    }
}

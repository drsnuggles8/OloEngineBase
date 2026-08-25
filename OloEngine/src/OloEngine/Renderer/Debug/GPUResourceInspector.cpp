#include "OloEnginePCH.h"
#include "GPUResourceInspector.h"
#include "DebugUtils.h"
#include "OloEngine/Core/Log.h"
#include "OloEngine/Core/Application.h"
#include "OloEngine/Renderer/RendererAPI.h"
#include "OloEngine/Renderer/Debug/ResourceInspectorBackend.h"
#include "OloEngine/Renderer/RHI/RHIResources.h"
#include "OloEngine/Utils/PlatformUtils.h"
#include "OloEngine/Threading/UniqueLock.h"

#include <stb_image/stb_image_write.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <limits>
#include <ranges>
#include <vector>

namespace OloEngine
{
    namespace
    {
        // Encoder selection for SaveTextureToFile. We only emit PNG (for 8-bit
        // outputs) and Radiance HDR (for float outputs). Other extensions like
        // .bmp / .jpg / .tga used to silently fall through to PNG bytes in a
        // misnamed file — return Unsupported instead so the caller errors out.
        enum class TextureSaveEncoder
        {
            Png,
            Hdr,
            Unsupported
        };

        TextureSaveEncoder PickEncoderFromExtension(const std::string& filePath)
        {
            std::filesystem::path p(filePath);
            std::string ext = p.extension().string();
            std::ranges::transform(ext, ext.begin(),
                                   [](unsigned char c)
                                   { return static_cast<char>(std::tolower(c)); });
            if (ext == ".hdr")
                return TextureSaveEncoder::Hdr;
            if (ext == ".png")
                return TextureSaveEncoder::Png;
            return TextureSaveEncoder::Unsupported;
        }

        const char* RendererApiName(RendererAPI::API api)
        {
            switch (api)
            {
                case RendererAPI::API::None:
                    return "None";
                case RendererAPI::API::OpenGL:
                    return "OpenGL";
                case RendererAPI::API::Vulkan:
                    return "Vulkan";
            }
            return "Unknown";
        }

        const char* BackendName(RHI::Backend backend)
        {
            switch (backend)
            {
                case RHI::Backend::None:
                    return "none";
                case RHI::Backend::OpenGL:
                    return "OpenGL";
                case RHI::Backend::Vulkan:
                    return "Vulkan";
            }
            return "unknown";
        }

        GPUResourceInspector::ResourceType ResourceTypeForBufferKind(IResourceInspectorBackend::BufferKind kind)
        {
            using RT = GPUResourceInspector::ResourceType;
            switch (kind)
            {
                case IResourceInspectorBackend::BufferKind::Index:
                    return RT::IndexBuffer;
                case IResourceInspectorBackend::BufferKind::Uniform:
                    return RT::UniformBuffer;
                case IResourceInspectorBackend::BufferKind::Vertex:
                    break;
            }
            return RT::VertexBuffer;
        }

        // `backend` classifies the buffer family; it is the same
        // ClassifyBufferTarget the push path uses for a GL buffer target, so
        // both discovery models file a buffer the same way.
        GPUResourceInspector::ResourceType ResourceTypeForDiscovered(
            const IResourceInspectorBackend::DiscoveredResource& discovered,
            IResourceInspectorBackend& backend)
        {
            using RT = GPUResourceInspector::ResourceType;
            switch (discovered.Kind)
            {
                case RHI::ResourceKind::Texture:
                    return discovered.IsCubemap ? RT::TextureCubemap : RT::Texture2D;
                case RHI::ResourceKind::Framebuffer:
                    return RT::Framebuffer;
                case RHI::ResourceKind::VertexArray:
                    return RT::VertexArray;
                case RHI::ResourceKind::ShaderProgram:
                    return RT::ShaderProgram;
                case RHI::ResourceKind::Query:
                    return RT::Query;
                case RHI::ResourceKind::Buffer:
                    // The backend's classification of the native target decides
                    // vertex / index / uniform, exactly as the GL registration
                    // path does. Without this every discovered buffer files as
                    // a vertex buffer and the type counts, the per-type memory
                    // totals, the panel's type filter and olo_gpu_resources'
                    // byType all lose the distinction.
                    //
                    // A target of 0 is "nothing described this buffer" (the
                    // backends reserve it — see VulkanResourceInspectorBackend's
                    // EncodeRootKind). BufferKind has no Unknown to return, so
                    // the grouping says Other rather than picking a family the
                    // backend explicitly declined to name.
                    if (discovered.NativeTarget == 0u)
                        return RT::Other;
                    return ResourceTypeForBufferKind(backend.ClassifyBufferTarget(discovered.NativeTarget));
                case RHI::ResourceKind::Unknown:
                    break;
            }
            return RT::Other;
        }

    } // namespace

    GPUResourceInspector::GPUResourceInspector()
    {
        m_ResourceCounts.fill(0);
        m_MemoryUsageByType.fill(0);
    }

    GPUResourceInspector::~GPUResourceInspector()
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

    IResourceInspectorBackend* GPUResourceInspector::GetBackend() const
    {
        // Lazy, once: the static capture entry points (CaptureTexturePng) and
        // SaveTextureToFile are exercised by tests without Initialize() ever
        // running, and the factory must not run before the process has settled
        // on its renderer API (the Vulkan-to-GL window-creation fallback can
        // switch it inside Application's constructor).
        std::call_once(m_BackendOnce, [this]
                       { m_Backend = CreateResourceInspectorBackend(); });
        return m_Backend.get();
    }

    void GPUResourceInspector::Initialize()
    {
        if (m_IsInitialized)
            return;

        if (GetBackend() == nullptr)
        {
            // Mirrors the glOnlyTooling gating in Application.cpp: under a
            // backend with no inspector arm the singleton simply stays
            // un-initialized and every entry point no-ops.
            OLO_CORE_INFO("GPUResourceInspector: no backend for {} — resource registration is a "
                          "GL-side instrument; Vulkan visibility comes from the VMA/root-object registries",
                          RendererApiName(RendererAPI::GetAPI()));
            return;
        }

        OLO_CORE_INFO("Initializing GPU Resource Inspector");
        m_IsInitialized = true;
    }

    void GPUResourceInspector::Shutdown()
    {
        if (!m_IsInitialized)
            return;

        OLO_CORE_INFO("Shutting down GPU Resource Inspector");
        // Clean up any pending texture downloads
        if (IResourceInspectorBackend* backend = GetBackend())
        {
            for (auto& download : m_TextureDownloads)
            {
                backend->ReleaseDownload({ download.m_PBO, download.m_Fence });
            }
        }
        m_TextureDownloads.clear();

        // Clean up resources
        {
            TUniqueLock<FMutex> lock(m_ResourceMutex);
            m_Resources.clear();
        }

        m_IsInitialized = false;
    }

    void GPUResourceInspector::RegisterTexture(u64 rendererID, const std::string& name, const std::string& debugName)
    {
        if (!m_IsInitialized || rendererID == 0)
            return;

        TUniqueLock<FMutex> lock(m_ResourceMutex);

        auto textureInfo = CreateScope<TextureInfo>();
        textureInfo->m_RendererID = rendererID;
        textureInfo->m_Type = ResourceType::Texture2D;
        textureInfo->m_Name = name;
        textureInfo->m_DebugName = debugName.empty() ? name : debugName;
        textureInfo->m_CreationTime = DebugUtils::GetCurrentTimeSeconds();

        // Query texture properties immediately
        QueryTextureInfo(*textureInfo);

        sizet memoryUsage = textureInfo->m_MemoryUsage;

        // m_Resources is a single flat map keyed by raw GL renderer ID, but
        // OpenGL textures / framebuffers / buffers each live in their own ID
        // namespace — a Texture2D with GL ID 32 and a Framebuffer with GL ID
        // 32 are different objects that coexist legitimately and collide in
        // this map. Capture oldType / oldMemory *before* the move-assign so
        // the accounting decrements the right bucket; reading
        // existingIt->second after the move would see the newly-installed
        // resource's type instead of the one being replaced.
        auto existingIt = m_Resources.find(rendererID);
        const bool hadExisting = (existingIt != m_Resources.end());
        const ResourceType oldType = hadExisting ? existingIt->second->m_Type : ResourceType::COUNT;
        const sizet oldMemory = hadExisting ? existingIt->second->m_MemoryUsage : 0u;

        if (hadExisting)
        {
            m_MemoryUsageByType[static_cast<sizet>(std::to_underlying(oldType))] -= oldMemory;
            if (oldType != ResourceType::Texture2D)
                --m_ResourceCounts[static_cast<sizet>(std::to_underlying(oldType))];
        }

        m_Resources[rendererID] = std::move(textureInfo);
        m_MemoryUsageByType[static_cast<sizet>(std::to_underlying(ResourceType::Texture2D))] += memoryUsage;
        if (!hadExisting || oldType != ResourceType::Texture2D)
            ++m_ResourceCounts[static_cast<sizet>(std::to_underlying(ResourceType::Texture2D))];
    }

    void GPUResourceInspector::RegisterTextureCubemap(u64 rendererID, const std::string& name, const std::string& debugName)
    {
        if (!m_IsInitialized || rendererID == 0)
            return;

        TUniqueLock<FMutex> lock(m_ResourceMutex);

        auto textureInfo = CreateScope<TextureInfo>();
        textureInfo->m_RendererID = rendererID;
        textureInfo->m_Type = ResourceType::TextureCubemap;
        textureInfo->m_Name = name;
        textureInfo->m_DebugName = debugName.empty() ? name : debugName;
        textureInfo->m_CreationTime = DebugUtils::GetCurrentTimeSeconds();
        // Query cubemap properties
        QueryTextureCubemapInfo(*textureInfo);

        sizet memoryUsage = textureInfo->m_MemoryUsage;

        // See notes in RegisterTexture above re cross-namespace ID reuse and
        // why oldType / oldMemory are captured before the move.
        auto existingIt = m_Resources.find(rendererID);
        const bool hadExisting = (existingIt != m_Resources.end());
        const ResourceType oldType = hadExisting ? existingIt->second->m_Type : ResourceType::COUNT;
        const sizet oldMemory = hadExisting ? existingIt->second->m_MemoryUsage : 0u;

        if (hadExisting)
        {
            m_MemoryUsageByType[static_cast<sizet>(std::to_underlying(oldType))] -= oldMemory;
            if (oldType != ResourceType::TextureCubemap)
                --m_ResourceCounts[static_cast<sizet>(std::to_underlying(oldType))];
        }

        m_Resources[rendererID] = std::move(textureInfo);
        m_MemoryUsageByType[static_cast<sizet>(std::to_underlying(ResourceType::TextureCubemap))] += memoryUsage;
        if (!hadExisting || oldType != ResourceType::TextureCubemap)
            ++m_ResourceCounts[static_cast<sizet>(std::to_underlying(ResourceType::TextureCubemap))];
    }

    void GPUResourceInspector::RegisterBuffer(u64 rendererID, u32 target, const std::string& name, const std::string& debugName)
    {
        if (!m_IsInitialized || rendererID == 0)
            return;

        TUniqueLock<FMutex> lock(m_ResourceMutex);

        auto bufferInfo = CreateScope<BufferInfo>();
        bufferInfo->m_RendererID = rendererID;
        bufferInfo->m_Target = target;
        bufferInfo->m_Name = name;
        bufferInfo->m_DebugName = debugName.empty() ? name : debugName;
        bufferInfo->m_CreationTime = DebugUtils::GetCurrentTimeSeconds();

        // Determine resource type based on target (native-enum decoding is
        // backend work; without a backend the old default fallback applies)
        bufferInfo->m_Type = ResourceType::VertexBuffer; // Default fallback
        if (IResourceInspectorBackend* backend = GetBackend())
        {
            switch (backend->ClassifyBufferTarget(target))
            {
                case IResourceInspectorBackend::BufferKind::Vertex:
                    bufferInfo->m_Type = ResourceType::VertexBuffer;
                    break;
                case IResourceInspectorBackend::BufferKind::Index:
                    bufferInfo->m_Type = ResourceType::IndexBuffer;
                    break;
                case IResourceInspectorBackend::BufferKind::Uniform:
                    bufferInfo->m_Type = ResourceType::UniformBuffer;
                    break;
            }
        }

        // Query buffer properties immediately
        QueryBufferInfo(*bufferInfo);

        ResourceType bufferType = bufferInfo->m_Type;
        sizet memoryUsage = bufferInfo->m_MemoryUsage;
        // See notes in RegisterTexture above. Buffers cover Vertex/Index/Uniform target kinds.
        auto existingIt = m_Resources.find(rendererID);
        const bool hadExisting = (existingIt != m_Resources.end());
        const ResourceType oldType = hadExisting ? existingIt->second->m_Type : ResourceType::COUNT;
        const sizet oldMemory = hadExisting ? existingIt->second->m_MemoryUsage : 0u;

        if (hadExisting)
        {
            m_MemoryUsageByType[static_cast<sizet>(std::to_underlying(oldType))] -= oldMemory;
            if (oldType != bufferType)
                --m_ResourceCounts[static_cast<sizet>(std::to_underlying(oldType))];
        }

        m_Resources[rendererID] = std::move(bufferInfo);
        m_MemoryUsageByType[static_cast<sizet>(std::to_underlying(bufferType))] += memoryUsage;
        if (!hadExisting || oldType != bufferType)
            ++m_ResourceCounts[static_cast<sizet>(std::to_underlying(bufferType))];
    }

    void GPUResourceInspector::RegisterFramebuffer(u64 rendererID, const std::string& name, const std::string& debugName)
    {
        if (!m_IsInitialized || rendererID == 0)
            return;

        TUniqueLock<FMutex> lock(m_ResourceMutex);

        auto framebufferInfo = CreateScope<FramebufferInfo>();
        framebufferInfo->m_RendererID = rendererID;
        framebufferInfo->m_Type = ResourceType::Framebuffer;
        framebufferInfo->m_Name = name;
        framebufferInfo->m_DebugName = debugName.empty() ? name : debugName;
        framebufferInfo->m_CreationTime = DebugUtils::GetCurrentTimeSeconds();
        // Query framebuffer properties
        QueryFramebufferInfo(*framebufferInfo);

        sizet memoryUsage = framebufferInfo->m_MemoryUsage;

        // See notes in RegisterTexture above. Framebuffer GL IDs live in their
        // own namespace and may legitimately match the ID of a live texture / buffer.
        auto existingIt = m_Resources.find(rendererID);
        const bool hadExisting = (existingIt != m_Resources.end());
        const ResourceType oldType = hadExisting ? existingIt->second->m_Type : ResourceType::COUNT;
        const sizet oldMemory = hadExisting ? existingIt->second->m_MemoryUsage : 0u;

        if (hadExisting)
        {
            m_MemoryUsageByType[static_cast<sizet>(std::to_underlying(oldType))] -= oldMemory;
            if (oldType != ResourceType::Framebuffer)
                --m_ResourceCounts[static_cast<sizet>(std::to_underlying(oldType))];
        }

        m_Resources[rendererID] = std::move(framebufferInfo);
        m_MemoryUsageByType[static_cast<sizet>(std::to_underlying(ResourceType::Framebuffer))] += memoryUsage;
        if (!hadExisting || oldType != ResourceType::Framebuffer)
            ++m_ResourceCounts[static_cast<sizet>(std::to_underlying(ResourceType::Framebuffer))];
    }

    void GPUResourceInspector::UnregisterResource(u64 rendererID)
    {
        if (!m_IsInitialized || rendererID == 0)
            return;

        TUniqueLock<FMutex> lock(m_ResourceMutex);

        auto it = m_Resources.find(rendererID);
        if (it != m_Resources.end())
        {
            ResourceType type = it->second->m_Type;
            --m_ResourceCounts[static_cast<sizet>(std::to_underlying(type))];
            m_MemoryUsageByType[static_cast<sizet>(std::to_underlying(type))] -= it->second->m_MemoryUsage;
            m_Resources.erase(it);
        }
    }

    void GPUResourceInspector::UpdateBindingStates()
    {
        if (!m_IsInitialized)
            return;

        IResourceInspectorBackend* backend = GetBackend();
        if (backend == nullptr)
            return;

        // This would be called by the renderer to update binding states
        // For now, we'll implement basic texture binding detection
        TUniqueLock<FMutex> lock(m_ResourceMutex);

        for (const auto& [id, resource] : m_Resources)
        {
            resource->m_IsBound = false; // Reset binding state

            if (resource->m_Type == ResourceType::Texture2D)
            {
                // Check if this texture is bound to any texture unit
                // This is a simplified check - in practice, we'd need to track all texture units
                if (backend->GetBoundTexture2D() == resource->m_RendererID)
                {
                    resource->m_IsBound = true;
                    resource->m_BindingSlot = 0; // Assume texture unit 0 for simplicity
                }
            }
        }
    }

    void GPUResourceInspector::UpdateResourceActiveState(u64 rendererID, bool isActive)
    {
        if (!m_IsInitialized || rendererID == 0)
            return;

        TUniqueLock<FMutex> lock(m_ResourceMutex);

        auto it = m_Resources.find(rendererID);
        if (it != m_Resources.end())
        {
            it->second->m_IsActive = isActive;
        }
    }

    void GPUResourceInspector::UpdateResourceBinding(u64 rendererID, bool isBound, u32 bindingSlot)
    {
        if (!m_IsInitialized || rendererID == 0)
            return;

        TUniqueLock<FMutex> lock(m_ResourceMutex);

        auto it = m_Resources.find(rendererID);
        if (it != m_Resources.end())
        {
            it->second->m_IsBound = isBound;
            it->second->m_BindingSlot = bindingSlot;
        }
    }

    void GPUResourceInspector::QueryTextureInfo(TextureInfo& info) const
    {
        IResourceInspectorBackend* backend = GetBackend();
        if (backend == nullptr)
            return;

        IResourceInspectorBackend::TextureQuery query;
        backend->QueryTexture(info.m_RendererID, /*isCubemap*/ false, query);

        info.m_Width = query.Width;
        info.m_Height = query.Height;
        info.m_InternalFormat = query.InternalFormat;
        info.m_Format = query.PixelFormat;
        info.m_DataType = query.DataType;
        info.m_MipLevels = query.MipLevels;
        info.m_HasMips = query.HasMips;
        info.m_MemoryUsage = query.MemoryUsage;
    }

    void GPUResourceInspector::QueryTextureCubemapInfo(TextureInfo& info) const
    {
        IResourceInspectorBackend* backend = GetBackend();
        if (backend == nullptr)
            return;

        IResourceInspectorBackend::TextureQuery query;
        backend->QueryTexture(info.m_RendererID, /*isCubemap*/ true, query);

        info.m_Width = query.Width;
        info.m_Height = query.Height;
        info.m_InternalFormat = query.InternalFormat;
        info.m_Format = query.PixelFormat;
        info.m_DataType = query.DataType;
        info.m_MipLevels = query.MipLevels;
        info.m_HasMips = query.HasMips;
        info.m_MemoryUsage = query.MemoryUsage;
    }

    bool GPUResourceInspector::SaveTextureToFile(const TextureInfo& info, const std::string& filePath,
                                                 u32 mipLevel, u32 faceIndex) const
    {
        OLO_PROFILE_FUNCTION();

        IResourceInspectorBackend* backend = GetBackend();
        if (backend == nullptr)
        {
            OLO_CORE_ERROR("[GPUResourceInspector] SaveTextureToFile: no resource-inspector backend for the active renderer API");
            return false;
        }

        if (info.m_RendererID == 0)
        {
            OLO_CORE_ERROR("[GPUResourceInspector] SaveTextureToFile: invalid texture ID");
            return false;
        }
        if (mipLevel >= info.m_MipLevels)
        {
            OLO_CORE_ERROR("[GPUResourceInspector] SaveTextureToFile: mip level {} out of range (max {})",
                           mipLevel, info.m_MipLevels - 1);
            return false;
        }
        const bool isCubemap = (info.m_Type == ResourceType::TextureCubemap);
        if (isCubemap && faceIndex >= 6)
        {
            OLO_CORE_ERROR("[GPUResourceInspector] SaveTextureToFile: cubemap face index {} out of range", faceIndex);
            return false;
        }

        const i32 channels = backend->ChannelCountForPixelFormat(info.m_Format);
        if (channels == 0)
        {
            OLO_CORE_ERROR("[GPUResourceInspector] SaveTextureToFile: unsupported pixel format 0x{:X} "
                           "(packed depth/stencil and compressed formats can't be exported directly)",
                           info.m_Format);
            return false;
        }

        // Dimensions at the requested mip level (cubemap faces are square and share the same chain).
        const u32 width = std::max(1u, info.m_Width >> mipLevel);
        const u32 height = std::max(1u, info.m_Height >> mipLevel);

        const TextureSaveEncoder encoder = PickEncoderFromExtension(filePath);
        if (encoder == TextureSaveEncoder::Unsupported)
        {
            const std::string ext = std::filesystem::path(filePath).extension().string();
            OLO_CORE_ERROR("[GPUResourceInspector] SaveTextureToFile: unsupported file extension '{}' "
                           "(only .png and .hdr are supported)",
                           ext.empty() ? "<none>" : ext);
            return false;
        }
        const bool wantFloatOutput = (encoder == TextureSaveEncoder::Hdr);

        // The readback precision comes from the source's native data type —
        // the classification itself (with its depth-quantisation rationale)
        // lives in the backend. `sourceIsFloat` triggers the float→u8 clamp
        // path below for PNG output.
        const IResourceInspectorBackend::PixelPrecision precision = backend->ClassifyPixelDataType(info.m_DataType);
        if (precision == IResourceInspectorBackend::PixelPrecision::Unsupported)
        {
            OLO_CORE_ERROR("[GPUResourceInspector] SaveTextureToFile: unsupported pixel data type 0x{:X}",
                           info.m_DataType);
            return false;
        }
        const bool sourceIsFloat = (precision == IResourceInspectorBackend::PixelPrecision::Float);
        const sizet readBytesPerChannel = sourceIsFloat ? sizeof(f32) : sizeof(u8);
        const sizet readRowStride = static_cast<sizet>(width) * static_cast<sizet>(channels) * readBytesPerChannel;
        const sizet readBufferBytes = readRowStride * static_cast<sizet>(height);

        std::vector<u8> readBuffer(readBufferBytes);

        std::string readError;
        if (!backend->ReadTextureLevel(info.m_RendererID, isCubemap, mipLevel, faceIndex,
                                       width, height, info.m_Format, sourceIsFloat,
                                       readBuffer.data(), readBufferBytes, readError))
        {
            OLO_CORE_ERROR("[GPUResourceInspector] SaveTextureToFile: {}", readError);
            return false;
        }

        // No vertical flip — the file is the raw byte layout from GPU memory
        // (GL bottom-left origin). This matches RenderDoc / Nsight semantics:
        // an inspector tool reports what's there, it doesn't reinterpret. Note
        // that Texture2Ds loaded through OpenGLTexture2D are pre-flipped on
        // upload (`stbi_set_flip_vertically_on_load_thread(1)`), so a saved
        // file opened directly will look right-side-up; cubemap faces (loaded
        // without flip) will look upside-down — which is exactly the inversion
        // that exists in GPU memory.

        // Convert to encoder precision when source and output disagree. Float→PNG
        // clamps to [0,1] and quantises to 8-bit; uint→HDR normalises by 255.
        // HDR float values outside [0,1] are preserved (that's the point of HDR).
        std::vector<u8> convertBuffer;
        const void* encoderPixels = readBuffer.data();
        sizet encoderRowStride = readRowStride;
        const sizet pixelChannelCount = static_cast<sizet>(width) * static_cast<sizet>(height) * static_cast<sizet>(channels);

        if (sourceIsFloat && !wantFloatOutput)
        {
            convertBuffer.resize(pixelChannelCount * sizeof(u8));
            const f32* src = reinterpret_cast<const f32*>(readBuffer.data());
            for (sizet i = 0; i < pixelChannelCount; ++i)
            {
                // Substitute NaN with 0 before the float→u8 cast:
                // static_cast<u8>(NaN) is undefined behavior per [conv.fpint],
                // and std::clamp(NaN, 0, 1) propagates the NaN on all major
                // standard libraries. ±Inf is fine — std::clamp handles those
                // (+Inf → 1, -Inf → 0), preserving directional information
                // when an inspection target legitimately contains Inf (e.g. a
                // velocity buffer post-divide-by-zero).
                const f32 safe = std::isnan(src[i]) ? 0.0f : src[i];
                const f32 clamped = std::clamp(safe, 0.0f, 1.0f);
                convertBuffer[i] = static_cast<u8>(clamped * 255.0f + 0.5f);
            }
            encoderPixels = convertBuffer.data();
            encoderRowStride = static_cast<sizet>(width) * static_cast<sizet>(channels);
        }
        else if (!sourceIsFloat && wantFloatOutput)
        {
            convertBuffer.resize(pixelChannelCount * sizeof(f32));
            f32* dst = reinterpret_cast<f32*>(convertBuffer.data());
            for (sizet i = 0; i < pixelChannelCount; ++i)
            {
                dst[i] = static_cast<f32>(readBuffer[i]) / 255.0f;
            }
            encoderPixels = convertBuffer.data();
            encoderRowStride = static_cast<sizet>(width) * static_cast<sizet>(channels) * sizeof(f32);
        }
        else
        {
            // No additional handling required.
        }

        // Ensure the destination directory exists.
        std::error_code ec;
        std::filesystem::path outPath(filePath);
        if (outPath.has_parent_path())
        {
            std::filesystem::create_directories(outPath.parent_path(), ec);
            if (ec)
            {
                OLO_CORE_ERROR("[GPUResourceInspector] SaveTextureToFile: cannot create directory '{}': {}",
                               outPath.parent_path().string(), ec.message());
                return false;
            }
        }

        int writeResult = 0;
        const std::string pathStr = outPath.string();
        if (encoder == TextureSaveEncoder::Hdr)
        {
            writeResult = stbi_write_hdr(pathStr.c_str(),
                                         static_cast<int>(width),
                                         static_cast<int>(height),
                                         channels,
                                         reinterpret_cast<const float*>(encoderPixels));
        }
        else
        {
            writeResult = stbi_write_png(pathStr.c_str(),
                                         static_cast<int>(width),
                                         static_cast<int>(height),
                                         channels,
                                         encoderPixels,
                                         static_cast<int>(encoderRowStride));
        }

        if (writeResult == 0)
        {
            OLO_CORE_ERROR("[GPUResourceInspector] SaveTextureToFile: encoder rejected '{}' "
                           "(check file permissions and disk space)",
                           pathStr);
            return false;
        }

        return true;
    }

    GPUResourceInspector::TextureCaptureResult GPUResourceInspector::CaptureTexturePng(u64 textureId, u32 mipLevel,
                                                                                       u32 faceOrLayer,
                                                                                       CaptureNormalizeMode normalize,
                                                                                       int maxWidth,
                                                                                       CaptureRegion region)
    {
        OLO_PROFILE_FUNCTION();

        TextureCaptureResult result;
        IResourceInspectorBackend* backend = GetInstance().GetBackend();
        if (backend == nullptr)
        {
            result.Error = "no resource-inspector backend for the active renderer API";
            return result;
        }

        IResourceInspectorBackend::CaptureSource source;
        if (!backend->QueryCaptureSource(textureId, mipLevel, source))
        {
            result.Error = source.Error;
            return result;
        }

        // Resolve the read rect. A zero-sized region means the whole mip; an
        // out-of-bounds one is an error rather than a silent clamp, because a
        // silently shrunk rect would make a 1:1 measurement report the wrong
        // spatial period without ever saying so.
        const u32 fullWidth = source.FullWidth;
        const u32 fullHeight = source.FullHeight;
        if (region.IsWholeTexture())
            region = CaptureRegion{ 0, 0, fullWidth, fullHeight };
        // Bounds are tested as `extent > remaining` rather than `offset + extent >
        // size` so a huge offset/extent pair cannot wrap the u32 addition and slip
        // past the check into an out-of-range readback.
        else if (region.X >= fullWidth || region.Y >= fullHeight ||
                 region.Width > fullWidth - region.X || region.Height > fullHeight - region.Y)
        {
            result.Error = "region (" + std::to_string(region.X) + ", " + std::to_string(region.Y) + ", " +
                           std::to_string(region.Width) + "x" + std::to_string(region.Height) + ") exceeds mip " +
                           std::to_string(mipLevel) + " (" + std::to_string(fullWidth) + "x" +
                           std::to_string(fullHeight) + ")";
            return result;
        }

        const i32 channels = source.Channels;
        const bool sourceIsFloat = source.IsFloat;
        const bool isDepth = source.IsDepth;
        const sizet bytesPerChannel = sourceIsFloat ? sizeof(f32) : sizeof(u8);
        const sizet rowStride = static_cast<sizet>(region.Width) * static_cast<sizet>(channels) * bytesPerChannel;
        const sizet bufferBytes = rowStride * static_cast<sizet>(region.Height);
        std::vector<u8> readBuffer(bufferBytes);

        std::string readError;
        if (!backend->ReadCaptureRegion(textureId, mipLevel, faceOrLayer, source,
                                        region.X, region.Y, region.Width, region.Height,
                                        readBuffer.data(), bufferBytes, readError))
        {
            result.Error = readError;
            return result;
        }

        // Everything below works on the READ RECT, not the full mip — with no
        // region requested the two are the same. Note the min/max normalisation
        // range is therefore region-local, which is what a zoomed inspection
        // wants (a 64x64 crop of a flat-looking HDR target gets its own contrast).
        const auto capturedWidth = static_cast<sizet>(region.Width);
        const auto capturedHeight = static_cast<sizet>(region.Height);

        // Convert to 8-bit. Float sources optionally min-max normalise first
        // (Auto = depth only) so a depth buffer / HDR target isn't a flat
        // white/black image after the [0,1] clamp.
        const sizet valueCount = capturedWidth * capturedHeight * static_cast<sizet>(channels);
        std::vector<u8> pixels8;
        if (sourceIsFloat)
        {
            const f32* src = reinterpret_cast<const f32*>(readBuffer.data());
            const bool wantNormalize = normalize == CaptureNormalizeMode::On ||
                                       (normalize == CaptureNormalizeMode::Auto && isDepth);
            f32 minV = std::numeric_limits<f32>::max();
            f32 maxV = std::numeric_limits<f32>::lowest();
            for (sizet i = 0; i < valueCount; ++i)
            {
                if (std::isfinite(src[i]))
                {
                    minV = std::min(minV, src[i]);
                    maxV = std::max(maxV, src[i]);
                }
            }
            const bool haveRange = maxV > minV;
            if (haveRange)
            {
                result.MinValue = minV;
                result.MaxValue = maxV;
            }
            const bool doNormalize = wantNormalize && haveRange;
            result.Normalized = doNormalize;
            const f32 scale = doNormalize ? 1.0f / (maxV - minV) : 1.0f;
            const f32 bias = doNormalize ? -minV : 0.0f;
            pixels8.resize(valueCount);
            for (sizet i = 0; i < valueCount; ++i)
            {
                const f32 safe = std::isnan(src[i]) ? 0.0f : src[i];
                const f32 clamped = std::clamp((safe + bias) * scale, 0.0f, 1.0f);
                pixels8[i] = static_cast<u8>(clamped * 255.0f + 0.5f);
            }
        }
        else
        {
            pixels8 = std::move(readBuffer);
        }

        // Widen 2-channel output to RGB (B = 0): PNG comp=2 means grey+alpha,
        // which would hide the G channel of an RG target in the alpha plane.
        i32 outChannels = channels;
        if (channels == 2)
        {
            outChannels = 3;
            std::vector<u8> widened(capturedWidth * capturedHeight * 3, 0);
            for (sizet i = 0; i < capturedWidth * capturedHeight; ++i)
            {
                widened[i * 3 + 0] = pixels8[i * 2 + 0];
                widened[i * 3 + 1] = pixels8[i * 2 + 1];
            }
            pixels8 = std::move(widened);
        }

        // Flip to PNG top-down orientation so the capture is upright when an
        // agent views it (matches CaptureViewportPng / olo_screenshot, and
        // intentionally differs from SaveTextureToFile's raw-memory dump).
        // The backend reports its row order; a top-down backend needs no flip.
        const sizet outRowBytes = capturedWidth * static_cast<sizet>(outChannels);
        std::vector<u8> flipped;
        if (backend->CaptureRowsAreBottomUp())
        {
            flipped.resize(pixels8.size());
            for (sizet y = 0; y < capturedHeight; ++y)
                std::memcpy(flipped.data() + y * outRowBytes,
                            pixels8.data() + (capturedHeight - 1 - y) * outRowBytes, outRowBytes);
        }
        else
        {
            flipped = std::move(pixels8);
        }

        // Optional nearest-neighbour downscale so width <= maxWidth. A region
        // narrower than maxWidth skips this entirely and stays 1:1 — that is the
        // whole point of asking for a region.
        auto outW = static_cast<u32>(capturedWidth);
        auto outH = static_cast<u32>(capturedHeight);
        const std::vector<u8>* encodeSrc = &flipped;
        std::vector<u8> scaled;
        if (maxWidth > 0 && outW > static_cast<u32>(maxWidth))
        {
            const u32 srcW = outW;
            const u32 srcH = outH;
            outW = static_cast<u32>(maxWidth);
            outH = std::max<u32>(1, static_cast<u32>((static_cast<u64>(srcH) * outW) / srcW));
            scaled.assign(static_cast<sizet>(outW) * outH * outChannels, 0);
            for (u32 y = 0; y < outH; ++y)
            {
                const u32 sy = std::min(srcH - 1, static_cast<u32>((static_cast<u64>(y) * srcH) / outH));
                for (u32 x = 0; x < outW; ++x)
                {
                    const u32 sx = std::min(srcW - 1, static_cast<u32>((static_cast<u64>(x) * srcW) / outW));
                    std::memcpy(&scaled[(static_cast<sizet>(y) * outW + x) * outChannels],
                                &flipped[(static_cast<sizet>(sy) * srcW + sx) * outChannels],
                                static_cast<sizet>(outChannels));
                }
            }
            encodeSrc = &scaled;
        }

        std::vector<u8> png;
        const auto appendToVector = [](void* context, void* data, int size)
        {
            auto* out = static_cast<std::vector<u8>*>(context);
            const auto* bytes = static_cast<const u8*>(data);
            out->insert(out->end(), bytes, bytes + size);
        };
        if (stbi_write_png_to_func(appendToVector, &png, static_cast<int>(outW), static_cast<int>(outH),
                                   outChannels, encodeSrc->data(), static_cast<int>(outW) * outChannels) == 0)
        {
            result.Error = "PNG encode failed";
            return result;
        }

        result.PngBytes = std::move(png);
        result.Width = outW;
        result.Height = outH;
        result.SourceWidth = fullWidth;
        result.SourceHeight = fullHeight;
        result.RegionX = region.X;
        result.RegionY = region.Y;
        result.RegionWidth = region.Width;
        result.RegionHeight = region.Height;
        result.FormatName = source.FormatName;
        result.IsDepth = isDepth;
        return result;
    }

    void GPUResourceInspector::QueryBufferInfo(BufferInfo& info) const
    {
        IResourceInspectorBackend* backend = GetBackend();
        if (backend == nullptr)
            return;

        IResourceInspectorBackend::BufferQuery query;
        backend->QueryBuffer(info.m_RendererID, info.m_Target, query);

        info.m_Size = query.Size;
        info.m_Usage = query.Usage;
        info.m_MemoryUsage = query.MemoryUsage;
    }

    void GPUResourceInspector::QueryFramebufferInfo(FramebufferInfo& info) const
    {
        IResourceInspectorBackend* backend = GetBackend();
        if (backend == nullptr)
            return;

        IResourceInspectorBackend::FramebufferQuery query;
        backend->QueryFramebuffer(info.m_RendererID, query);

        info.m_Status = query.Status;
        info.m_ColorAttachmentCount = query.ColorAttachmentCount;
        info.m_ColorAttachmentFormats = query.ColorAttachmentFormats;
        info.m_HasDepthAttachment = query.HasDepthAttachment;
        info.m_DepthAttachmentFormat = query.DepthAttachmentFormat;
        info.m_HasStencilAttachment = query.HasStencilAttachment;
        info.m_StencilAttachmentFormat = query.StencilAttachmentFormat;
        info.m_Width = query.Width;
        info.m_Height = query.Height;
        info.m_MemoryUsage = query.MemoryUsage;
    }
    void GPUResourceInspector::ProcessTextureDownloads()
    {
        IResourceInspectorBackend* backend = GetBackend();

        // Process async texture downloads and check for completion using modern sync objects
        auto it = m_TextureDownloads.begin();
        while (it != m_TextureDownloads.end())
        {
            if (it->m_InProgress)
            {
                bool downloadComplete = false;
                // Modern OpenGL 4.5+ approach: Use sync objects for non-blocking completion detection
                if (it->m_Fence != nullptr && backend != nullptr)
                {
                    // Check fence status without blocking (0 timeout = non-blocking)
                    const IResourceInspectorBackend::DownloadStatus status =
                        backend->PollDownload({ it->m_PBO, it->m_Fence });

                    if (status == IResourceInspectorBackend::DownloadStatus::Complete)
                    {
                        // Download is complete!
                        downloadComplete = true;
                        OLO_CORE_TRACE("Texture download completed for texture {} (sync object signaled)", it->m_TextureID);
                    }
                    else if (status == IResourceInspectorBackend::DownloadStatus::Failed)
                    {
                        // Sync object failed - this shouldn't happen but handle gracefully
                        OLO_CORE_WARN("Sync object wait failed for texture {}", it->m_TextureID);
                        downloadComplete = true; // Force completion to avoid hanging
                    }
                    else
                    {
                        // No additional handling required.
                    }
                    // Pending means not ready yet - continue to next frame
                }
                else
                {
                    // No sync object available - this shouldn't happen with modern approach
                    OLO_CORE_WARN("No sync fence available for texture download {}", it->m_TextureID);
                    downloadComplete = true; // Force completion to avoid hanging
                }

                if (downloadComplete)
                {
                    // Find the corresponding texture and complete the download.
                    // m_Resources values are std::unique_ptr<ResourceInfo>, so we
                    // cannot copy the owning pointer to extend lifetime. The
                    // resource could be erased between unlock and the call below,
                    // dangling any raw pointer we took out — so hold m_ResourceMutex
                    // across CompleteTextureDownload. That call writes into the
                    // preview buffer and does a one-shot PBO map/unmap of at most
                    // 256×256 RGBA — brief contention is the lesser evil compared
                    // with a use-after-free.
                    TUniqueLock<FMutex> lock(m_ResourceMutex);
                    if (auto resourceIt = m_Resources.find(it->m_TextureID); resourceIt != m_Resources.end() &&
                                                                             (resourceIt->second->m_Type == ResourceType::Texture2D ||
                                                                              resourceIt->second->m_Type == ResourceType::TextureCubemap))
                    {
                        auto* texInfo = static_cast<TextureInfo*>(resourceIt->second.get());
                        CompleteTextureDownload(*texInfo, *it);
                    }

                    // Clean up resources
                    if (backend != nullptr)
                        backend->ReleaseDownload({ it->m_PBO, it->m_Fence });

                    // Remove completed download from queue
                    it = m_TextureDownloads.erase(it);
                }
                else
                {
                    // Check for timeout (5 seconds)
                    f64 currentTime = DebugUtils::GetCurrentTimeSeconds();
                    if (currentTime - it->m_RequestTime > 5.0)
                    {
                        OLO_CORE_WARN("Texture download timeout for texture {}, mip level {}", it->m_TextureID, it->m_MipLevel);

                        // Clean up resources
                        if (backend != nullptr)
                            backend->ReleaseDownload({ it->m_PBO, it->m_Fence });
                        it = m_TextureDownloads.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }
            }
            else
            {
                ++it;
            }
        }
    }

    void GPUResourceInspector::RequestTextureDownload(TextureInfo& info, u32 mipLevel, u32 faceIndex)
    {
        // Check if there's already a pending download for this texture/mip/face combo
        for (const auto& download : m_TextureDownloads)
        {
            if (download.m_TextureID == info.m_RendererID && download.m_MipLevel == mipLevel &&
                download.m_FaceIndex == faceIndex)
                return;
        }

        IResourceInspectorBackend* backend = GetBackend();
        if (backend == nullptr)
            return;

        // Calculate data size for this mip level - use RGBA format for consistency
        u32 width = std::max(1u, info.m_Width >> mipLevel);
        u32 height = std::max(1u, info.m_Height >> mipLevel);
        u32 bytesPerPixel = 4; // Always use RGBA format for downloads
        sizet dataSize = width * height * bytesPerPixel;

        // The staging buffer + fence machinery (PBO on GL) lives in the
        // backend; for cubemaps the face index selects the layer, matching
        // SaveTextureToFile's convention.
        const bool isCubemap = (info.m_Type == ResourceType::TextureCubemap);
        IResourceInspectorBackend::DownloadTicket ticket;
        if (!backend->BeginTextureDownload(info.m_RendererID, isCubemap, mipLevel, faceIndex,
                                           width, height, dataSize, ticket))
        {
            return;
        }

        // Add to download queue
        TextureDownloadRequest request;
        request.m_TextureID = info.m_RendererID;
        request.m_MipLevel = mipLevel;
        request.m_FaceIndex = faceIndex;
        request.m_PBO = ticket.NativeBuffer;
        request.m_Fence = ticket.Fence;
        request.m_InProgress = true;
        request.m_RequestTime = DebugUtils::GetCurrentTimeSeconds();

        m_TextureDownloads.push_back(request);

        OLO_CORE_TRACE("Requested async texture download for texture {} mip {} face {}",
                       info.m_RendererID, mipLevel, faceIndex);
    }
    void GPUResourceInspector::UpdateTexturePreview(TextureInfo& info)
    {
        if (info.m_PreviewDataValid)
            return;

        // Check if there's already a pending download for this texture / mip / face combo
        const u32 faceIndex = (info.m_Type == ResourceType::TextureCubemap) ? info.m_SelectedCubemapFace : 0u;
        for (const auto& download : m_TextureDownloads)
        {
            if (download.m_TextureID == info.m_RendererID && download.m_MipLevel == info.m_SelectedMipLevel &&
                download.m_FaceIndex == faceIndex)
            {
                // Download already in progress, just wait
                return;
            }
        }

        IResourceInspectorBackend* backend = GetBackend();
        if (backend == nullptr)
            return;

        // Check if texture is valid (mip-level storage query)
        u32 width = 0;
        u32 height = 0;
        backend->GetTextureLevelSize(info.m_RendererID, info.m_SelectedMipLevel, width, height);

        if (width == 0 || height == 0)
        {
            // Invalid mip level or texture
            return;
        }

        // Start async download instead of blocking
        RequestTextureDownload(info, info.m_SelectedMipLevel, faceIndex);
    }

    void GPUResourceInspector::UpdateBufferPreview(BufferInfo& info) const
    {
        if (info.m_ContentPreviewValid)
            return;

        IResourceInspectorBackend* backend = GetBackend();
        if (backend == nullptr)
            return;

        // Clamp the copy to what actually remains past the offset so we never
        // read past the end of the mapped buffer when m_PreviewOffset > 0.
        const u32 remaining = (info.m_Size > info.m_PreviewOffset) ? (info.m_Size - info.m_PreviewOffset) : 0;
        const u32 previewSize = std::min(info.m_PreviewSize, remaining);
        info.m_ContentPreview.resize(previewSize);

        if (backend->ReadBufferRange(info.m_RendererID, info.m_Target, info.m_PreviewOffset, previewSize,
                                     info.m_ContentPreview.data()))
        {
            info.m_ContentPreviewValid = true;
        }
        else
        {
            OLO_CORE_WARN("Failed to map buffer for preview: ID {}", info.m_RendererID);
            info.m_ContentPreviewValid = false;
        }
    }

    // ---- Registry-driven discovery + snapshot (#810) -----------------------

    namespace
    {
        // The registry kind a tracked resource type belongs to. Used only to
        // disambiguate the (kind, native) match below.
        RHI::ResourceKind RegistryKindFor(GPUResourceInspector::ResourceType type)
        {
            using RT = GPUResourceInspector::ResourceType;
            switch (type)
            {
                case RT::Texture2D:
                case RT::TextureCubemap:
                    return RHI::ResourceKind::Texture;
                case RT::Framebuffer:
                    return RHI::ResourceKind::Framebuffer;
                case RT::VertexArray:
                    return RHI::ResourceKind::VertexArray;
                case RT::ShaderProgram:
                    return RHI::ResourceKind::ShaderProgram;
                case RT::Query:
                    return RHI::ResourceKind::Query;
                default:
                    break;
            }
            return RHI::ResourceKind::Buffer;
        }
    } // namespace

    void GPUResourceInspector::ReconcileIdentitiesFromRegistry()
    {
        // (kind, native) -> identity. Unique on a self-registering backend:
        // GL gives textures, buffers, framebuffers and VAOs separate name
        // namespaces, so a collision inside one kind cannot happen. Anything
        // that DID collide is left alone rather than guessed at.
        std::unordered_map<u64, RHI::ResourceHandle> byKindAndNative;
        std::unordered_map<u64, u32> matchCount;
        for (const auto& entry : RHI::ResourceRegistry::Get().Snapshot())
        {
            if (entry.Native == 0u)
                continue;
            const u64 key = (static_cast<u64>(std::to_underlying(entry.Kind)) << 56) ^ entry.Native;
            byKindAndNative[key] = entry.Handle;
            ++matchCount[key];
        }

        TUniqueLock<FMutex> lock(m_ResourceMutex);
        for (auto& [mapKey, resource] : m_Resources)
        {
            if (resource->m_Handle.IsValid() || resource->m_RendererID == 0u)
                continue;
            const u64 key = (static_cast<u64>(std::to_underlying(RegistryKindFor(resource->m_Type))) << 56) ^
                            resource->m_RendererID;
            const auto it = byKindAndNative.find(key);
            if (it == byKindAndNative.end() || matchCount[key] != 1u)
                continue;
            resource->m_Handle = it->second;
            resource->m_Backend = RHI::GetNativeHandleForDebug(it->second).Owner;
        }
    }

    void GPUResourceInspector::RefreshDiscoveredResources()
    {
        IResourceInspectorBackend* backend = GetBackend();
        if (backend == nullptr)
            return;
        if (!backend->DiscoversResources())
        {
            // Self-registering backend: the map is already the live set, but
            // the macro-pushed rows have no identity on them.
            ReconcileIdentitiesFromRegistry();
            return;
        }

        // The backend reads render-thread-only side tables, so this must not
        // run under m_ResourceMutex with an MCP handler waiting on it — gather
        // first, then swap under the lock.
        std::vector<IResourceInspectorBackend::DiscoveredResource> discovered;
        backend->DiscoverResources(discovered);

        TUniqueLock<FMutex> lock(m_ResourceMutex);

        // A rebuild, not a merge. The registry snapshot IS the live set, so
        // reconciling incrementally would only add a way for a destroyed
        // resource to linger — the failure mode this whole tool exists to
        // make visible.
        m_Resources.clear();
        m_ResourceCounts.fill(0);
        m_MemoryUsageByType.fill(0);

        for (const auto& entry : discovered)
        {
            const ResourceType type = ResourceTypeForDiscovered(entry, *backend);
            const auto typeIndex = static_cast<sizet>(std::to_underlying(type));

            std::unique_ptr<ResourceInfo> info;
            if (type == ResourceType::Texture2D || type == ResourceType::TextureCubemap)
            {
                auto texture = CreateScope<TextureInfo>();
                texture->m_Width = entry.Width;
                texture->m_Height = entry.Height;
                texture->m_MipLevels = entry.MipLevels;
                texture->m_HasMips = entry.MipLevels > 1u;
                texture->m_InternalFormat = entry.NativeFormat;
                texture->m_Format = entry.NativeFormat;
                texture->m_DataType = 0u;
                info = std::move(texture);
            }
            else if (type == ResourceType::Framebuffer)
            {
                auto framebuffer = CreateScope<FramebufferInfo>();
                framebuffer->m_Width = entry.Width;
                framebuffer->m_Height = entry.Height;
                info = std::move(framebuffer);
            }
            else
            {
                auto buffer = CreateScope<BufferInfo>();
                buffer->m_Target = entry.NativeTarget;
                buffer->m_Usage = entry.NativeTarget;
                buffer->m_Size =
                    static_cast<u32>(std::min<u64>(entry.SizeBytes, std::numeric_limits<u32>::max()));
                info = std::move(buffer);
            }

            info->m_RendererID = entry.Native;
            info->m_Handle = entry.Handle;
            info->m_Backend = RHI::GetNativeHandleForDebug(entry.Handle).Owner;
            info->m_Type = type;
            info->m_Name = entry.Name;
            info->m_DebugName = entry.DebugName.empty() ? entry.Name : entry.DebugName;
            info->m_MemoryUsage = static_cast<sizet>(entry.SizeBytes);
            info->m_CreationTime = DebugUtils::GetCurrentTimeSeconds();

            // Key on the IDENTITY, not the native id. A Vulkan framebuffer
            // registers native 0 (no VkFramebuffer exists under dynamic
            // rendering) and an arena-backed UBO has no native object at all,
            // so several live resources legitimately share native 0 — keying on
            // it would collapse them into one row and silently under-report.
            // The identity is unique by construction.
            const u64 key = entry.Handle.IsValid()
                                ? ((static_cast<u64>(entry.Handle.Generation) << 32) | entry.Handle.Index)
                                : entry.Native;
            m_Resources[key] = std::move(info);
            ++m_ResourceCounts[typeIndex];
            m_MemoryUsageByType[typeIndex] += static_cast<sizet>(entry.SizeBytes);
        }
    }

    std::vector<GPUResourceInspector::ResourceSnapshotEntry> GPUResourceInspector::SnapshotResources() const
    {
        std::vector<ResourceSnapshotEntry> rows;

        TUniqueLock<FMutex> lock(m_ResourceMutex);
        rows.reserve(m_Resources.size());
        for (const auto& [key, resource] : m_Resources)
        {
            ResourceSnapshotEntry row;
            row.NativeHandle = resource->m_RendererID;
            row.Handle = resource->m_Handle;
            row.Backend = resource->m_Backend;
            row.Type = resource->m_Type;
            row.Name = resource->m_Name;
            row.DebugName = resource->m_DebugName;
            row.MemoryUsage = resource->m_MemoryUsage;
            row.IsActive = resource->m_IsActive;
            row.IsBound = resource->m_IsBound;
            row.BindingSlot = resource->m_BindingSlot;

            switch (resource->m_Type)
            {
                case ResourceType::Texture2D:
                case ResourceType::TextureCubemap:
                {
                    const auto& texture = static_cast<const TextureInfo&>(*resource);
                    row.Width = texture.m_Width;
                    row.Height = texture.m_Height;
                    row.MipLevels = texture.m_MipLevels;
                    row.NativeFormat = texture.m_InternalFormat;
                    row.FormatName = FormatTextureFormat(texture.m_InternalFormat);
                    break;
                }
                case ResourceType::Framebuffer:
                {
                    const auto& framebuffer = static_cast<const FramebufferInfo&>(*resource);
                    row.Width = framebuffer.m_Width;
                    row.Height = framebuffer.m_Height;
                    row.NativeFormat = framebuffer.m_Status;
                    break;
                }
                default:
                {
                    const auto& buffer = static_cast<const BufferInfo&>(*resource);
                    row.SizeBytes = buffer.m_Size;
                    row.NativeTarget = buffer.m_Target;
                    row.NativeFormat = buffer.m_Usage;
                    row.FormatName = FormatBufferUsage(buffer.m_Usage);
                    break;
                }
            }
            rows.push_back(std::move(row));
        }

        // Deterministic order so two consecutive reads of an unchanged scene
        // compare equal; unordered_map iteration order does not.
        std::ranges::sort(rows,
                          [](const ResourceSnapshotEntry& a, const ResourceSnapshotEntry& b)
                          {
                              if (a.Type != b.Type)
                                  return std::to_underlying(a.Type) < std::to_underlying(b.Type);
                              if (a.Handle.Index != b.Handle.Index)
                                  return a.Handle.Index < b.Handle.Index;
                              return a.NativeHandle < b.NativeHandle;
                          });
        return rows;
    }

    bool GPUResourceInspector::QueryMemoryHeaps(std::vector<IResourceInspectorBackend::MemoryHeap>& out) const
    {
        out.clear();
        IResourceInspectorBackend* backend = GetBackend();
        return backend != nullptr && backend->QueryMemoryHeaps(out);
    }

    bool GPUResourceInspector::SupportsPreviews() const
    {
        IResourceInspectorBackend* backend = GetBackend();
        // A discovering backend is exactly the one with no PBO download engine
        // and no GL ImGui texture binding — see the header's contract.
        return backend != nullptr && !backend->DiscoversResources();
    }

    sizet GPUResourceInspector::GetMemoryUsage(ResourceType type) const
    {
        TUniqueLock<FMutex> lock(m_ResourceMutex);
        return m_MemoryUsageByType[static_cast<sizet>(std::to_underlying(type))];
    }

    sizet GPUResourceInspector::GetTotalMemoryUsage() const
    {
        TUniqueLock<FMutex> lock(m_ResourceMutex);
        sizet total = 0;
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

        // Process any pending texture downloads to prevent stalls
        ProcessTextureDownloads();

        // Vulkan has no registration macros, so the live set has to be pulled
        // from RHI::ResourceRegistry each frame the panel is open (#810).
        // No-op on OpenGL.
        RefreshDiscoveredResources();

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
        RenderMemoryHeaps();

        if (!SupportsPreviews())
        {
            ImGui::TextColored(ImVec4(0.85f, 0.75f, 0.35f, 1.0f),
                               "Previews unavailable on this backend — the thumbnail path is the GL "
                               "PBO/fence download engine.");
            ImGui::TextDisabled("Use olo_render_capture_target / olo_render_probe_pixel for pixels; "
                                "they read through the facade readback spine.");
        }

        ImGui::Separator();

        // Filter controls
        ImGui::Text("Filters:");
        ImGui::SameLine();

        // Order must match ResourceType, offset by the leading "All".
        const char* typeNames[] = { "All", "Textures", "Cubemaps", "Vertex Buffers", "Index Buffers",
                                    "Uniform Buffers", "Framebuffers", "Vertex Arrays", "Shader Programs",
                                    "Queries", "Other" };
        static_assert(IM_ARRAYSIZE(typeNames) == static_cast<int>(std::to_underlying(ResourceType::COUNT)) + 1,
                      "typeNames must stay in lockstep with ResourceType");
        if (int currentFilter = static_cast<int>(std::to_underlying(m_FilterType)) + 1; ImGui::Combo("Type", &currentFilter, typeNames, IM_ARRAYSIZE(typeNames)))
        {
            m_FilterType = (currentFilter == 0) ? ResourceType::COUNT : static_cast<ResourceType>(currentFilter - 1);
        }

        ImGui::SameLine(); // Create a buffer for InputText (ImGui needs a char buffer)
        if (static char searchBuffer[256] = ""; ImGui::InputText("Search", searchBuffer, sizeof(searchBuffer)))
        {
            m_SearchFilter = std::string(searchBuffer);
        }
        ImGui::Separator();

        // Split view: resource tree on left, details on right
        static float leftPaneWidth = 300.0f;

        // Resource tree pane
        ImGui::BeginChild("ResourceTree", ImVec2(leftPaneWidth, -1), true);
        RenderResourceTree();
        ImGui::EndChild();

        // Splitter
        ImGui::SameLine();
        ImGui::Button("##splitter", ImVec2(8.0f, -1));

        if (ImGui::IsItemActive())
        {
            leftPaneWidth += ImGui::GetIO().MouseDelta.x;
            leftPaneWidth = std::clamp(leftPaneWidth, 100.0f, ImGui::GetContentRegionAvail().x - 100.0f);
        }

        if (ImGui::IsItemHovered())
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }

        // Resource details pane
        ImGui::SameLine();
        ImGui::BeginChild("ResourceDetails", ImVec2(-1, -1), true);
        RenderResourceDetails();
        ImGui::EndChild();

        ImGui::End();

        // Run any deferred Save-to-File now that m_ResourceMutex (held inside
        // RenderResourceDetails) is released. The dialog blocks the UI thread
        // but no longer blocks the background-thread registration paths.
        ProcessPendingSaveRequest();
    }

    void GPUResourceInspector::ProcessPendingSaveRequest()
    {
        if (!m_PendingSaveRequest.m_Active)
            return;

        // Move the snapshot out so a re-entrant Save click during the dialog
        // (theoretical: dialogs spin a message loop) can't stomp it.
        const TextureInfo snapshot = m_PendingSaveRequest.m_Info;
        m_PendingSaveRequest.m_Active = false;
        m_PendingSaveRequest.m_Info = {};

        // Filter pairs are "Label\0pattern\0..." per the existing FileDialogs convention.
        // The Windows backend pulls the default extension from the first pattern (the byte
        // after the first NUL of the wildcard), so order the float-format option first when
        // the texture is float so the user gets a sensible default.
        IResourceInspectorBackend* backend = GetBackend();
        const bool isFloat = backend != nullptr && backend->IsFloatPixelDataType(snapshot.m_DataType);
        const char* filter = isFloat
                                 ? "Radiance HDR (*.hdr)\0*.hdr\0PNG (*.png)\0*.png\0"
                                 : "PNG (*.png)\0*.png\0Radiance HDR (*.hdr)\0*.hdr\0";
        const std::string path = FileDialogs::SaveFile(filter);
        if (path.empty())
            return;

        // The dialog blocks the UI thread, but background threads can have
        // unregistered & re-registered resources during the wait. Re-check
        // that the snapshot's RendererID is still tracked AND still refers
        // to the same texture type before doing the GL readback — otherwise
        // a GL name reused for a different resource (or no resource at all)
        // would silently produce a garbage file.
        if (snapshot.m_RendererID == 0)
            return;
        {
            TUniqueLock<FMutex> lock(m_ResourceMutex);
            auto it = m_Resources.find(snapshot.m_RendererID);
            if (it == m_Resources.end())
            {
                OLO_CORE_WARN("[GPUResourceInspector] Save aborted: texture {} was unregistered during the file dialog",
                              snapshot.m_RendererID);
                return;
            }
            if (it->second->m_Type != snapshot.m_Type)
            {
                OLO_CORE_WARN("[GPUResourceInspector] Save aborted: GL name {} now refers to a different resource type",
                              snapshot.m_RendererID);
                return;
            }
        }

        if (SaveTextureToFile(snapshot, path, snapshot.m_SelectedMipLevel, snapshot.m_SelectedCubemapFace))
        {
            OLO_CORE_INFO("[GPUResourceInspector] Texture {} saved to '{}'", snapshot.m_RendererID, path);
        }
    }

    void GPUResourceInspector::RenderResourceTree()
    {
        TUniqueLock<FMutex> lock(m_ResourceMutex);

        ImGui::Text("Resources (%u)", GetResourceCount());
        ImGui::Separator();

        // Debug: Show filter state and actual resource counts
        u32 totalResources = static_cast<u32>(m_Resources.size());
        u32 activeResources = 0;
        u32 inactiveResources = 0;

        for (const auto& [id, resource] : m_Resources)
        {
            if (resource->m_IsActive)
                ++activeResources;
            else
                ++inactiveResources;
        }

        ImGui::Text("Total: %u, Active: %u, Inactive: %u", totalResources, activeResources, inactiveResources);
        if (!m_ShowInactiveResources && inactiveResources > 0)
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "(%u hidden)", inactiveResources);
        }
        ImGui::Separator();

        // Group resources by type
        std::unordered_map<ResourceType, std::vector<std::pair<u64, ResourceInfo*>>> groupedResources;

        for (const auto& [key, resource] : m_Resources)
        {
            // Apply filters
            if (m_FilterType != ResourceType::COUNT && resource->m_Type != m_FilterType)
                continue;

            if (!m_SearchFilter.empty())
            {
                constexpr auto toLowerChar = [](unsigned char c)
                { return static_cast<char>(std::tolower(c)); };

                std::string searchLower = m_SearchFilter;
                std::ranges::transform(searchLower, searchLower.begin(), toLowerChar);

                std::string nameLower = resource->m_Name;
                std::ranges::transform(nameLower, nameLower.begin(), toLowerChar);

                if (nameLower.find(searchLower) == std::string::npos)
                    continue;
            }

            if (!m_ShowInactiveResources && !resource->m_IsActive)
                continue;

            // Carry the MAP KEY, not the native id: under a discovering backend
            // the key is the RHI identity (a Vulkan framebuffer and an
            // arena-backed UBO both register native 0, so the native cannot
            // select a row). Under OpenGL the two are the same value.
            groupedResources[resource->m_Type].emplace_back(key, resource.get());
        }

        // Render tree nodes by type
        for (int i = 0; i < static_cast<int>(std::to_underlying(ResourceType::COUNT)); ++i)
        {
            ResourceType type = static_cast<ResourceType>(i);
            const auto& resources = groupedResources[type];

            if (resources.empty())
                continue;

            if (ImGui::TreeNode(GetResourceTypeName(type)))
            {
                for (const auto& [key, resource] : resources)
                {
                    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
                    if (key == m_SelectedResourceID)
                        flags |= ImGuiTreeNodeFlags_Selected;

                    std::string label = resource->m_DebugName.empty() ? resource->m_Name : resource->m_DebugName;
                    if (label.empty())
                        label = "Unnamed Resource";

                    // Add memory usage to label
                    label += " (" + FormatMemorySize(resource->m_MemoryUsage) + ")";
                    if (resource->m_IsBound)
                        label += " [BOUND]";

                    // Create unique ID for this tree node using resource ID
                    std::string uniqueID = label + "##" + std::to_string(key);
                    ImGui::TreeNodeEx(uniqueID.c_str(), flags);

                    if (ImGui::IsItemClicked())
                    {
                        m_SelectedResourceID = key;
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

        TUniqueLock<FMutex> lock(m_ResourceMutex);

        auto it = m_Resources.find(m_SelectedResourceID);
        if (it == m_Resources.end())
        {
            ImGui::Text("Selected resource not found");
            return;
        }

        ResourceInfo* resource = it->second.get();

        ImGui::Text("Resource Details");
        ImGui::Separator();

        // BOTH currencies, always (ADR 0011 amendment (77)): the native id is
        // what a RenderDoc / RGP capture shows, the identity is what answers
        // "same object, or a recycled name?".
        ImGui::Text("Native: 0x%llX (%llu)", static_cast<unsigned long long>(resource->m_RendererID),
                    static_cast<unsigned long long>(resource->m_RendererID));
        if (resource->m_Handle.IsValid())
        {
            ImGui::Text("RHI handle: #%u gen %u  [%s]", resource->m_Handle.Index,
                        resource->m_Handle.Generation, BackendName(resource->m_Backend));
        }
        else
        {
            ImGui::TextDisabled("RHI handle: (not recorded at registration)");
        }
        ImGui::Text("Type: %s", GetResourceTypeName(resource->m_Type));
        ImGui::Text("Name: %s", resource->m_Name.c_str());
        if (!resource->m_DebugName.empty() && resource->m_DebugName != resource->m_Name)
        {
            ImGui::Text("Debug Name: %s", resource->m_DebugName.c_str());
        }
        ImGui::Text("Memory Usage: %s", FormatMemorySize(resource->m_MemoryUsage).c_str());
        ImGui::Text("Active: %s", resource->m_IsActive ? "Yes" : "No");
        ImGui::Text("Bound: %s", resource->m_IsBound ? "Yes" : "No");
        if (resource->m_IsBound)
            ImGui::Text("Binding Slot: %u", resource->m_BindingSlot);

        ImGui::Separator();

        // Type-specific details
        if (resource->m_Type == ResourceType::Texture2D || resource->m_Type == ResourceType::TextureCubemap)
        {
            RenderTexturePreview(*static_cast<TextureInfo*>(resource));
        }
        else if (resource->m_Type == ResourceType::VertexBuffer ||
                 resource->m_Type == ResourceType::IndexBuffer ||
                 resource->m_Type == ResourceType::UniformBuffer)
        {
            RenderBufferContent(*static_cast<BufferInfo*>(resource));
        }
        else if (resource->m_Type == ResourceType::Framebuffer)
        {
            RenderFramebufferDetails(*static_cast<FramebufferInfo*>(resource));
        }
        else
        {
            // No additional handling required.
        }
    }

    void GPUResourceInspector::RenderTexturePreview(TextureInfo& info)
    {
        ImGui::Text("Texture Properties");
        ImGui::Text("Dimensions: %u x %u", info.m_Width, info.m_Height);
        ImGui::Text("Internal Format: %s", FormatTextureFormat(info.m_InternalFormat).c_str());
        ImGui::Text("Mip Levels: %u", info.m_MipLevels);
        ImGui::Text("Has Mipmaps: %s", info.m_HasMips ? "Yes" : "No");

        // Special handling for cubemaps
        if (info.m_Type == ResourceType::TextureCubemap)
        {
            ImGui::Text("Cubemap Faces: 6");

            // Face selection for cubemaps
            const char* faceNames[] = { "+X", "-X", "+Y", "-Y", "+Z", "-Z" };
            int selectedFace = static_cast<int>(info.m_SelectedCubemapFace);
            if (ImGui::Combo("Face", &selectedFace, faceNames, 6))
            {
                info.m_SelectedCubemapFace = static_cast<u32>(std::clamp(selectedFace, 0, 5));
                info.m_PreviewDataValid = false; // Force refresh for new face
            }
        }

        if (info.m_HasMips)
        {
            ImGui::SliderInt("Mip Level", reinterpret_cast<int*>(&info.m_SelectedMipLevel), 0, static_cast<int>(info.m_MipLevels - 1));
            if (ImGui::IsItemEdited())
            {
                info.m_PreviewDataValid = false; // Force refresh
            }
        }

        ImGui::Separator();

        const bool refreshClicked = ImGui::Button("Refresh Preview");
        if (refreshClicked)
        {
            info.m_PreviewDataValid = false;
        }

        ImGui::SameLine();
        if (ImGui::Button("Save to File"))
        {
            // Defer the actual save until after RenderResourceDetails returns.
            // Doing the modal FileDialogs::SaveFile() + GL readback here would
            // hold m_ResourceMutex across the dialog — any background thread
            // calling RegisterTexture / UnregisterResource would block until
            // the user dismissed the dialog. Snapshot the TextureInfo now
            // (cheap memberwise copy) and let ProcessPendingSaveRequest()
            // pick it up below.
            m_PendingSaveRequest.m_Active = true;
            m_PendingSaveRequest.m_Info = info;
        }
        // Trigger preview update in auto mode OR when Refresh was just clicked.
        // (Previously this read ImGui::IsItemClicked() which always referred to
        // the LAST item — the Save button — silently kicking a preview download
        // every time the user tried to save.)
        if (m_AutoUpdatePreviews || refreshClicked)
        {
            // Only try to update preview if we have valid dimensions
            if (info.m_Width > 0 && info.m_Height > 0 && info.m_SelectedMipLevel < info.m_MipLevels)
            {
                UpdateTexturePreview(info);
            }
        }

        if (info.m_PreviewDataValid && !info.m_PreviewData.empty())
        {
            // Create ImGui texture if not already created
            if (info.m_ImGuiTextureID == 0)
            {
                // Route through the backend rather than assuming the native id
                // is a valid ImTextureID — that is a GL-only truth, and the
                // backend is the party that knows it (same contract as
                // ImGuiLayer::GetTextureID; 0 means "no binding, skip draw").
                if (IResourceInspectorBackend* backend = GetBackend())
                    info.m_ImGuiTextureID = static_cast<ImTextureID>(backend->GetImGuiTextureID(info.m_RendererID));
            }

            static float zoom = 1.0f;
            ImGui::SliderFloat("Zoom", &zoom, 0.1f, 4.0f);

            ImVec2 imageSize(256 * zoom, 256 * zoom);

            ImVec2 availableSize = ImGui::GetContentRegionAvail();
            if (imageSize.x > availableSize.x)
            {
                float scale = availableSize.x / imageSize.x;
                imageSize.x *= scale;
                imageSize.y *= scale;
            }
            if (imageSize.y > availableSize.y - 60) // Leave some space for controls
            {
                float scale = (availableSize.y - 60) / imageSize.y;
                imageSize.x *= scale;
                imageSize.y *= scale;
            }

            if (info.m_ImGuiTextureID != 0)
            {
                ImGui::Image(info.m_ImGuiTextureID, imageSize);

                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Texture Preview\nSize: %u x %u\nFormat: %s\nClick to view full size",
                                      info.m_Width, info.m_Height, FormatTextureFormat(info.m_InternalFormat).c_str());
                }
            }

            // Show texture statistics
            ImGui::Separator();
            ImGui::Text("Preview Info");
            ImGui::Text("Displayed Size: %.0f x %.0f", imageSize.x, imageSize.y);
            ImGui::Text("Memory Usage: %s", FormatMemorySize(info.m_MemoryUsage).c_str());
        }
        else
        {
            ImGui::Text("Preview not available");
            if (info.m_Width == 0 || info.m_Height == 0)
            {
                ImGui::Text("(Invalid texture dimensions)");
            }
            else if (info.m_SelectedMipLevel >= info.m_MipLevels)
            {
                ImGui::Text("(Invalid mip level selected)");
            }
            else
            {
                ImGui::Text("(Texture may be too large, compressed, or use unsupported format)");
            }

            if (ImGui::Button("Try Download Preview"))
            {
                info.m_PreviewDataValid = false;
                if (info.m_Width > 0 && info.m_Height > 0 && info.m_SelectedMipLevel < info.m_MipLevels)
                {
                    UpdateTexturePreview(info);
                }
            }
        }
    }

    void GPUResourceInspector::RenderBufferContent(BufferInfo& info)
    {
        ImGui::Text("Buffer Properties");
        ImGui::Text("Target: 0x%X (%s)", info.m_Target, GetBufferTargetName(info.m_Target));
        ImGui::Text("Usage: %s", FormatBufferUsage(info.m_Usage).c_str());
        ImGui::Text("Size: %s", FormatMemorySize(info.m_Size).c_str());

        if (info.m_Type == ResourceType::VertexBuffer)
        {
            ImGui::Separator();
            ImGui::Text("Vertex Buffer Layout");
            if (ImGui::InputInt("Stride (bytes)", reinterpret_cast<int*>(&info.m_Stride)))
            {
                info.m_Stride = std::max(1u, info.m_Stride); // Ensure stride is at least 1
            }

            if (info.m_Stride > 0 && info.m_ContentPreviewValid && !info.m_ContentPreview.empty())
            {
                ImGui::Text("Vertex Count (estimated): %u", info.m_Size / info.m_Stride);

                // Show structured vertex data if stride is set
                ImGui::Separator();
                ImGui::Text("Vertex Data (first 10 vertices):");

                const u8* data = info.m_ContentPreview.data();
                sizet size = info.m_ContentPreview.size();
                u32 vertexCount = std::min(10u, static_cast<u32>(size / info.m_Stride));

                if (ImGui::BeginTable("VertexData", std::min(info.m_Stride / 4 + 2, 8u), ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
                {
                    ImGui::TableSetupColumn("Vertex");
                    for (u32 i = 0; i < std::min(info.m_Stride / 4, 7u); ++i)
                    {
                        ImGui::TableSetupColumn(("Float" + std::to_string(i)).c_str());
                    }
                    ImGui::TableHeadersRow();

                    for (u32 v = 0; v < vertexCount; ++v)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("%u", v);

                        const f32* vertexData = reinterpret_cast<const f32*>(data + v * info.m_Stride);
                        u32 floatCount = std::min(info.m_Stride / 4, 7u);

                        for (u32 f = 0; f < floatCount; ++f)
                        {
                            ImGui::TableSetColumnIndex(f + 1);
                            ImGui::Text("%.3f", vertexData[f]);
                        }
                    }
                    ImGui::EndTable();
                }
            }
        }
        else if (info.m_Type == ResourceType::IndexBuffer)
        {
            ImGui::Separator();
            ImGui::Text("Index Buffer");

            if (info.m_ContentPreviewValid && !info.m_ContentPreview.empty())
            {
                // Assume 32-bit indices for now (could be improved to detect 16-bit vs 32-bit)
                u32 indexCount = info.m_Size / sizeof(u32);
                ImGui::Text("Index Count (estimated): %u", indexCount);

                // Show first few indices
                ImGui::Text("Indices (first 20):");
                const u32* indices = reinterpret_cast<const u32*>(info.m_ContentPreview.data());
                sizet previewIndices = std::min(20u, static_cast<u32>(info.m_ContentPreview.size() / sizeof(u32)));

                std::string indexString;
                for (sizet i = 0; i < previewIndices; ++i)
                {
                    if (i > 0)
                        indexString += ", ";
                    indexString += std::to_string(indices[i]);
                }
                ImGui::Text("%s", indexString.c_str());
            }
        }
        else
        {
            // No additional handling required.
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
            ImGui::Text("Raw Content Preview (Hex Dump):");

            // Hex dump display
            const u8* data = info.m_ContentPreview.data();
            sizet size = info.m_ContentPreview.size();

            for (sizet i = 0; i < size; i += 16)
            {
                // Address
                ImGui::Text("%08X: ", static_cast<u32>(info.m_PreviewOffset + i));
                ImGui::SameLine();

                // Hex bytes
                for (sizet j = 0; j < 16 && (i + j) < size; ++j)
                {
                    ImGui::SameLine();
                    ImGui::Text("%02X", data[i + j]);
                }

                // ASCII representation
                ImGui::SameLine();
                ImGui::Text("  ");
                for (sizet j = 0; j < 16 && (i + j) < size; ++j)
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

    void GPUResourceInspector::RenderFramebufferDetails(FramebufferInfo& info)
    {
        ImGui::Text("Framebuffer Properties");
        ImGui::Text("Dimensions: %u x %u", info.m_Width, info.m_Height);

        // Framebuffer status (native-enum decoding is backend work)
        const char* statusText = "Unknown";
        ImVec4 statusColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        if (IResourceInspectorBackend* backend = GetBackend())
        {
            IResourceInspectorBackend::FramebufferStatusClass statusClass =
                IResourceInspectorBackend::FramebufferStatusClass::Unknown;
            statusText = backend->FramebufferStatusName(info.m_Status, statusClass);
            switch (statusClass)
            {
                case IResourceInspectorBackend::FramebufferStatusClass::Complete:
                    statusColor = ImVec4(0.0f, 1.0f, 0.0f, 1.0f); // Green
                    break;
                case IResourceInspectorBackend::FramebufferStatusClass::Incomplete:
                    statusColor = ImVec4(1.0f, 0.0f, 0.0f, 1.0f); // Red
                    break;
                case IResourceInspectorBackend::FramebufferStatusClass::Unsupported:
                    statusColor = ImVec4(1.0f, 0.5f, 0.0f, 1.0f); // Orange
                    break;
                case IResourceInspectorBackend::FramebufferStatusClass::Unknown:
                    break;
            }
        }

        ImGui::Text("Status: ");
        ImGui::SameLine();
        ImGui::TextColored(statusColor, "%s", statusText);

        ImGui::Separator();

        // Color attachments
        ImGui::Text("Color Attachments: %u", info.m_ColorAttachmentCount);
        for (u32 i = 0; i < info.m_ColorAttachmentCount; ++i)
        {
            if (i < info.m_ColorAttachmentFormats.size())
            {
                // Decoded, not raw hex: these carry sized internal formats
                // now that the GL backend queries the attached object rather
                // than GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE (#803).
                ImGui::Text("  Attachment %u: %s", i,
                            FormatTextureFormat(info.m_ColorAttachmentFormats[i]).c_str());
            }
            else
            {
                ImGui::Text("  Attachment %u: Unknown format", i);
            }
        }

        // Depth attachment
        if (info.m_HasDepthAttachment)
        {
            ImGui::Text("Depth Attachment: %s", FormatTextureFormat(info.m_DepthAttachmentFormat).c_str());
        }
        else
        {
            ImGui::Text("Depth Attachment: None");
        }

        // Stencil attachment
        if (info.m_HasStencilAttachment)
        {
            ImGui::Text("Stencil Attachment: %s", FormatTextureFormat(info.m_StencilAttachmentFormat).c_str());
        }
        else
        {
            ImGui::Text("Stencil Attachment: None");
        }

        ImGui::Separator();

        if (ImGui::Button("Refresh"))
        {
            // Force refresh of framebuffer info
            QueryFramebufferInfo(info);
        }
    }

    void GPUResourceInspector::RenderResourceStatistics()
    {
        ImGui::Text("Statistics");
        ImGui::Separator();

        // Count actual resources in map by type and calculate memory usage
        std::array<u32, static_cast<sizet>(std::to_underlying(ResourceType::COUNT))> actualCounts = {};
        std::array<sizet, static_cast<sizet>(std::to_underlying(ResourceType::COUNT))> actualMemoryUsage = {};
        sizet totalMemory = 0;

        {
            TUniqueLock<FMutex> lock(m_ResourceMutex);
            for (const auto& [id, resource] : m_Resources)
            {
                sizet typeIndex = static_cast<sizet>(std::to_underlying(resource->m_Type));
                ++actualCounts[typeIndex];
                actualMemoryUsage[typeIndex] += resource->m_MemoryUsage;
                totalMemory += resource->m_MemoryUsage;
            }
        }

        ImGui::Text("Total Resources: %u", GetResourceCount());
        ImGui::Text("Total Memory: %s", FormatMemorySize(totalMemory).c_str());

        // Memory usage by type (only show types that have resources)
        for (int i = 0; i < static_cast<int>(std::to_underlying(ResourceType::COUNT)); ++i)
        {
            ResourceType type = static_cast<ResourceType>(i);
            u32 count = actualCounts[i];
            if (count > 0)
            {
                sizet memory = actualMemoryUsage[i];
                ImGui::Text("%s: %u (%s)", GetResourceTypeName(type), count, FormatMemorySize(memory).c_str());
            }
        }
    }

    void GPUResourceInspector::RenderMemoryHeaps()
    {
        // The tracked-resource total above sums what this panel knows about;
        // this is what the DEVICE ALLOCATOR says, which is the number that
        // answers "am I about to run out of VRAM?". They deliberately differ:
        // the allocator's blocks include padding, suballocation slack and
        // anything created before the inspector existed.
        std::vector<IResourceInspectorBackend::MemoryHeap> heaps;
        if (!QueryMemoryHeaps(heaps) || heaps.empty())
            return;

        ImGui::Separator();
        if (!ImGui::CollapsingHeader("Device Memory Heaps", ImGuiTreeNodeFlags_DefaultOpen))
            return;

        if (ImGui::BeginTable("##gpuHeaps", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Heap");
            ImGui::TableSetupColumn("Device-local");
            ImGui::TableSetupColumn("Usage");
            ImGui::TableSetupColumn("Budget");
            ImGui::TableSetupColumn("Blocks");
            ImGui::TableSetupColumn("Allocations");
            ImGui::TableHeadersRow();

            for (const auto& heap : heaps)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%u", heap.Index);
                ImGui::TableNextColumn();
                ImGui::Text("%s", heap.DeviceLocal ? "yes" : "no");
                ImGui::TableNextColumn();
                ImGui::Text("%s", FormatMemorySize(static_cast<sizet>(heap.UsageBytes)).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%s", FormatMemorySize(static_cast<sizet>(heap.BudgetBytes)).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%llu (%s)", static_cast<unsigned long long>(heap.BlockCount),
                            FormatMemorySize(static_cast<sizet>(heap.BlockBytes)).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(heap.AllocationCount));
            }
            ImGui::EndTable();
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

        TUniqueLock<FMutex> lock(m_ResourceMutex);

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

    std::string GPUResourceInspector::FormatTextureFormat(u32 format) const
    {
        if (IResourceInspectorBackend* backend = GetBackend())
            return backend->FormatTextureFormatName(format);

        std::stringstream ss;
        ss << "Unknown (0x" << std::uppercase << std::hex << format << ")";
        return ss.str();
    }

    std::string GPUResourceInspector::FormatBufferUsage(u32 usage) const
    {
        if (IResourceInspectorBackend* backend = GetBackend())
            return backend->FormatBufferUsageName(usage);

        std::stringstream ss;
        ss << "Unknown (0x" << std::hex << usage << ")";
        return ss.str();
    }

    std::string GPUResourceInspector::FormatMemorySize(sizet bytes) const
    {
        const char* units[] = { "B", "KB", "MB", "GB" };
        int unit = 0;
        double size = static_cast<double>(bytes);

        while (size >= 1024.0 && unit < 3)
        {
            size /= 1024.0;
            ++unit;
        }

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << size << " " << units[unit];
        return oss.str();
    }

    const char* GPUResourceInspector::GetResourceTypeName(ResourceType type) const
    {
        switch (type)
        {
            case ResourceType::Texture2D:
                return "Texture2D";
            case ResourceType::TextureCubemap:
                return "TextureCubemap";
            case ResourceType::VertexBuffer:
                return "Vertex Buffer";
            case ResourceType::IndexBuffer:
                return "Index Buffer";
            case ResourceType::UniformBuffer:
                return "Uniform Buffer";
            case ResourceType::Framebuffer:
                return "Framebuffer";
            case ResourceType::VertexArray:
                return "Vertex Array";
            case ResourceType::ShaderProgram:
                return "Shader Program";
            case ResourceType::Query:
                return "Query";
            case ResourceType::Other:
                return "Other";
            default:
                return "Unknown";
        }
    }

    const char* GPUResourceInspector::GetBufferTargetName(u32 target) const
    {
        if (IResourceInspectorBackend* backend = GetBackend())
            return backend->GetBufferTargetName(target);
        return "Unknown";
    }
    void GPUResourceInspector::CompleteTextureDownload(TextureInfo& info, const TextureDownloadRequest& request) const
    {
        OLO_CORE_TRACE("Completing texture download for texture {} mip level {}", info.m_RendererID, request.m_MipLevel);

        IResourceInspectorBackend* backend = GetBackend();
        if (backend == nullptr)
            return;

        // Calculate data size for this mip level - using RGBA format consistently
        u32 width = std::max(1u, info.m_Width >> request.m_MipLevel);
        u32 height = std::max(1u, info.m_Height >> request.m_MipLevel);
        u32 bytesPerPixel = 4; // RGBA format
        sizet dataSize = width * height * bytesPerPixel;

        // Map the staging buffer to read the downloaded data
        if (const void* data = backend->MapDownloadData({ request.m_PBO, request.m_Fence }, dataSize); data != nullptr)
        {
            // Calculate preview size (limit to reasonable size for UI)
            u32 previewWidth = std::min(width, 256u);
            u32 previewHeight = std::min(height, 256u);

            // Allocate preview buffer
            info.m_PreviewData.resize(previewWidth * previewHeight * bytesPerPixel);

            if (previewWidth == width && previewHeight == height)
            {
                // Direct copy if no scaling needed
                std::memcpy(info.m_PreviewData.data(), data, dataSize);
            }
            else
            {
                // Simple nearest-neighbor downscaling for preview
                const u8* srcData = static_cast<const u8*>(data);

                for (u32 y = 0; y < previewHeight; ++y)
                {
                    for (u32 x = 0; x < previewWidth; ++x)
                    {
                        u32 srcX = (x * width) / previewWidth;
                        u32 srcY = (y * height) / previewHeight;
                        u32 srcIndex = (srcY * width + srcX) * bytesPerPixel;
                        u32 dstIndex = (y * previewWidth + x) * bytesPerPixel;

                        for (u32 c = 0; c < bytesPerPixel; ++c)
                        {
                            info.m_PreviewData[dstIndex + c] = srcData[srcIndex + c];
                        }
                    }
                }
            }

            // Mark preview as valid
            info.m_PreviewDataValid = true;

            OLO_CORE_TRACE("Completed async texture download for texture {} mip level {}", request.m_TextureID, request.m_MipLevel);

            // Unmap only when the buffer was successfully mapped; unmapping an
            // unmapped buffer is an error on the backend (GL_INVALID_OPERATION).
            backend->UnmapDownloadData({ request.m_PBO, request.m_Fence });
        }
        else
        {
            OLO_CORE_ERROR("Failed to map PBO data for texture {}", request.m_TextureID);
        }
    }
} // namespace OloEngine

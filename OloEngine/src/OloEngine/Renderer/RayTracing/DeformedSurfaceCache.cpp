#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RayTracing/DeformedSurfaceCache.h"

#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/MeshSource.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"
#include "OloEngine/Renderer/Vertex.h"
#include "OloEngine/Renderer/VertexBuffer.h"

#include <algorithm>

namespace OloEngine::RayTracing
{
    namespace
    {
        // Mirrors OloSkeletalDeformParams in
        // OloEditor/assets/shaders/compute/SkeletalDeformToBuffer.comp. std140,
        // and every address is a uvec2 there because GL_EXT_buffer_reference_uvec2
        // is how a 64-bit address travels in ordinary buffer data — so the two
        // u32 halves are spelled out here rather than a u64 being punned, which
        // is what makes the layout readable against the GLSL side.
        struct alignas(16) DeformParams
        {
            u32 RestVertexAddressLo = 0;
            u32 RestVertexAddressHi = 0;
            u32 BoneInfluenceAddressLo = 0;
            u32 BoneInfluenceAddressHi = 0;
            u32 PaletteAddressLo = 0;
            u32 PaletteAddressHi = 0;
            u32 OutputVertexAddressLo = 0;
            u32 OutputVertexAddressHi = 0;
            u32 VertexCount = 0;
            u32 BoneCount = 0;
            u32 Pad0 = 0;
            u32 Pad1 = 0;
        };
        static_assert(sizeof(DeformParams) == 48, "DeformParams must match the std140 block in "
                                                  "compute/SkeletalDeformToBuffer.comp");

        constexpr u32 kWorkgroupSize = 64; ///< local_size_x in the compute shader.

        // Growth rounding. A surface that gains vertices one at a time would
        // otherwise reallocate — and therefore force a BLAS REBUILD rather than
        // a refit — on every frame it grows.
        constexpr u32 kCapacityGranularity = 256;

        constexpr const char* kShaderPath = "assets/shaders/compute/SkeletalDeformToBuffer.comp";

        void SplitAddress(u64 address, u32& low, u32& high)
        {
            low = static_cast<u32>(address & 0xFFFFFFFFull);
            high = static_cast<u32>(address >> 32u);
        }
    } // namespace

    // Defined here rather than defaulted in the header: every Ref<> member
    // names a type the header forward-declares, and this TU is where they are
    // complete.
    DeformedSurfaceCache::DeformedSurfaceCache() = default;
    DeformedSurfaceCache::~DeformedSurfaceCache() = default;

    u64 DeformedSurfaceCache::StreamBytes(u32 vertexCount)
    {
        return static_cast<u64>(vertexCount) * static_cast<u64>(sizeof(Vertex));
    }

    bool DeformedSurfaceCache::CapacityServes(u32 capacity, u32 required)
    {
        return required != 0u && capacity >= required;
    }

    u32 DeformedSurfaceCache::CapacityFor(u32 vertexCount)
    {
        if (vertexCount == 0u)
        {
            return 0u;
        }
        // Saturating rather than wrapping: a vertex count within one
        // granularity of u32 max is not a real mesh, but rounding it would
        // produce a capacity SMALLER than the request and then allocate a
        // buffer every consumer would read past.
        constexpr u32 kMaxRoundable = std::numeric_limits<u32>::max() - (kCapacityGranularity - 1u);
        if (vertexCount > kMaxRoundable)
        {
            return vertexCount;
        }
        return ((vertexCount + kCapacityGranularity - 1u) / kCapacityGranularity) * kCapacityGranularity;
    }

    void DeformedSurfaceCache::SetEnabled(bool enabled)
    {
        if (m_Enabled == enabled)
        {
            return;
        }
        m_Enabled = enabled;
        if (!m_Enabled)
        {
            // Releasing rather than parking. The feature going away means the
            // RT device went away, and holding device buffers against a device
            // that may be replaced is the lifetime bug §8's capability flag
            // clearing exists to avoid.
            Shutdown();
        }
    }

    void DeformedSurfaceCache::Shutdown()
    {
        for (auto& [key, entry] : m_Surfaces)
        {
            ReleaseEntry(entry);
        }
        m_Surfaces.clear();
        m_Queue.clear();
        m_PendingRetires.clear();
        m_PaletteStaging.clear();
        m_PaletteBuffer.Reset();
        m_PaletteBufferBytes = 0;
        m_PaletteAddress = 0;
        m_Shader.Reset();
        m_Params.Reset();
        m_ShaderUnavailable = false;
        m_Stats = DeformedSurfaceStats{};
    }

    void DeformedSurfaceCache::ReleaseEntry(Entry& entry)
    {
        // The Ref drop is the release. Both backends defer the real destroy —
        // Vulkan through VulkanDeferredReclaim, GL through its own deletion
        // queue — so a buffer a still-executing frame reads is not freed under
        // it. There is deliberately no inline destroy here for that reason.
        entry.Output.Reset();
        entry.Capacity = 0;
        entry.VertexCount = 0;
        entry.DeviceAddress = 0;
        entry.PaletteHash = 0;
        entry.EverDeformed = false;
    }

    void DeformedSurfaceCache::BeginFrame()
    {
        ++m_FrameNumber;
        m_Stats.ResetFrame();
        m_Queue.clear();
        m_PaletteStaging.clear();
        m_PendingRetires.clear();
    }

    bool DeformedSurfaceCache::EnsureShader()
    {
        if (m_Shader)
        {
            return true;
        }
        if (m_ShaderUnavailable)
        {
            return false;
        }
        // Loaded lazily and only once. A failure latches: retrying a shader
        // compile every frame turns a missing asset into a per-frame log flood
        // and hides the one line that named it.
        m_Shader = ComputeShader::Create(kShaderPath);
        if (!m_Shader || !m_Shader->IsValid())
        {
            m_Shader.Reset();
            m_ShaderUnavailable = true;
            OLO_CORE_ERROR("[RayTracing] skeletal deformation compute shader unavailable ({}); animated surfaces will "
                           "trace their REST pose",
                           kShaderPath);
            return false;
        }
        m_Params = UniformBuffer::Create(static_cast<u32>(sizeof(DeformParams)),
                                         ShaderBindingLayout::UBO_RAY_TRACING);
        if (!m_Params)
        {
            m_Shader.Reset();
            m_ShaderUnavailable = true;
            OLO_CORE_ERROR("[RayTracing] could not create the skeletal deformation params buffer");
            return false;
        }
        return true;
    }

    u64 DeformedSurfaceCache::HashPalette(std::span<const glm::mat4> palette)
    {
        // FNV-1a over the raw matrix bytes. Bytes, not floats, and that is the
        // point: this asks "is this the same pose", which is a byte question.
        // Comparing floats numerically would need an epsilon, and an epsilon
        // here would let a slow drift accumulate silently under an
        // acceleration structure that was never refitted for it.
        u64 hash = 1469598103934665603ull;
        const auto* bytes = reinterpret_cast<const unsigned char*>(palette.data());
        const sizet count = palette.size() * sizeof(glm::mat4);
        for (sizet i = 0; i < count; ++i)
        {
            hash ^= static_cast<u64>(bytes[i]);
            hash *= 1099511628211ull;
        }
        // Fold the length in so a palette that shrank to a prefix of itself —
        // a bone-count change — cannot hash equal to what it was.
        hash ^= static_cast<u64>(palette.size());
        hash *= 1099511628211ull;
        return hash;
    }

    DeformedSurfaceBinding DeformedSurfaceCache::Acquire(const DeformedSurfaceKey& key, bool isAnimated,
                                                         const Ref<MeshSource>& meshSource,
                                                         std::span<const glm::mat4> palette)
    {
        // NOT refusals, and neither is counted. There is no ray tracing, or
        // this is an ordinary rigid mesh — in both cases nothing was offered,
        // and counting them would drown the one number that matters (a
        // character that should be traceable and is not) in a count of every
        // wall in the level.
        if (!m_Enabled || !isAnimated)
        {
            return DeformedSurfaceBinding{};
        }

        ++m_Stats.SurfacesRequested;

        const auto refuse = [this]() -> DeformedSurfaceBinding
        {
            ++m_Stats.Refused;
            return DeformedSurfaceBinding{};
        };

        // From here on every early return IS a refusal: the caller said this is
        // an animated surface, so a surface that leaves without a stream is one
        // the ray tracer will not see.
        if (!meshSource || palette.empty())
        {
            return refuse();
        }
        if (!meshSource->HasVertexBuffer() || !meshSource->HasBoneInfluenceBuffer())
        {
            // A skinned surface with no bone-influence stream cannot be
            // deformed by this shader at all. It is a real population — a
            // hand-authored LOD level that lost its influences is refused
            // upstream too (morph-and-lod-in-the-animated-surface.md) — so it
            // is counted, not asserted.
            return refuse();
        }

        const u64 restAddress = meshSource->GetVertexBuffer()->GetDeviceAddress();
        const u64 influenceAddress = meshSource->GetBoneInfluenceBuffer()->GetDeviceAddress();
        const auto vertexCount = static_cast<u32>(meshSource->GetVertices().Num());
        if (restAddress == 0u || influenceAddress == 0u || vertexCount == 0u)
        {
            return refuse();
        }

        Entry& entry = m_Surfaces[key];
        const bool firstSight = entry.Output == nullptr;

        bool reallocated = false;
        if (!CapacityServes(entry.Capacity, vertexCount))
        {
            const u32 capacity = CapacityFor(vertexCount);
            const u64 bytes = StreamBytes(capacity);
            if (bytes == 0u || bytes > static_cast<u64>(std::numeric_limits<u32>::max()))
            {
                return refuse();
            }
            Ref<VertexBuffer> allocated = VertexBuffer::Create(static_cast<u32>(bytes));
            if (!allocated || allocated->GetDeviceAddress() == 0u)
            {
                // Leave whatever was resident in place. A failed allocation
                // that blanked the entry would drop the surface out of the
                // TLAS entirely; keeping the previous stream traces a stale
                // pose for a frame, which is strictly the lesser wrong and is
                // counted either way.
                //
                // A surface that failed on its FIRST sight has nothing to keep,
                // and the default-constructed map entry the lookup above just
                // created is not a resident surface. Erase it, or the frame's
                // census reports a resident buffer holding no bytes and
                // EndFrame retires it as though something had been released.
                if (firstSight)
                {
                    m_Surfaces.erase(key);
                }
                return refuse();
            }
            if (!firstSight)
            {
                ReleaseEntry(entry);
                ++m_Stats.Reallocated;
            }
            else
            {
                ++m_Stats.Allocated;
            }
            entry.Output = allocated;
            entry.Capacity = capacity;
            entry.DeviceAddress = allocated->GetDeviceAddress();
            // A reallocation invalidates the contents as well as the address,
            // so the next dispatch is unconditional even if the pose is
            // unchanged. Without this a surface that reallocated while paused
            // would trace whatever the fresh allocation happened to contain.
            entry.EverDeformed = false;
            entry.PaletteHash = 0;
            reallocated = true;
        }

        entry.LastSeenFrame = m_FrameNumber;

        DeformedSurfaceBinding binding{};
        binding.Handle = entry.Output->GetRHIHandle();
        binding.DeviceAddress = entry.DeviceAddress;
        binding.Reallocated = reallocated;

        // The pose test. `EverDeformed` is what separates "we have already
        // produced this palette" from "we have never produced anything" — a
        // buffer that has never been written holds whatever the allocator left
        // in it, and a hash comparison alone cannot tell those apart.
        const u64 paletteHash = HashPalette(palette);
        const bool vertexCountChanged = entry.VertexCount != vertexCount;
        const bool poseChanged = !entry.EverDeformed || entry.PaletteHash != paletteHash;
        binding.ContentRevision = entry.ContentRevision;
        if (!poseChanged && !vertexCountChanged)
        {
            ++m_Stats.SkippedUnchanged;
            return binding;
        }

        // One palette per SURFACE, appended to the frame's shared staging. Two
        // submeshes of one entity Acquire with the same key and therefore reach
        // this point once — the second call sees an unchanged revision and an
        // unchanged count and takes the skip above.
        const auto paletteOffsetBytes = static_cast<u32>(m_PaletteStaging.size() * sizeof(glm::mat4));
        m_PaletteStaging.insert(m_PaletteStaging.end(), palette.begin(), palette.end());

        m_Queue.push_back(QueuedDispatch{
            .RestAddress = restAddress,
            .InfluenceAddress = influenceAddress,
            .OutputAddress = entry.DeviceAddress,
            .PaletteOffsetBytes = paletteOffsetBytes,
            .BoneCount = static_cast<u32>(palette.size()),
            .VertexCount = vertexCount,
        });

        entry.VertexCount = vertexCount;
        entry.PaletteHash = paletteHash;
        entry.EverDeformed = true;
        // Only here, where a dispatch was actually queued. This is what a
        // consumer compares to learn that the geometry under its acceleration
        // structure moved.
        ++entry.ContentRevision;
        binding.ContentRevision = entry.ContentRevision;
        return binding;
    }

    bool DeformedSurfaceCache::EnsurePaletteBuffer(u64 requiredBytes)
    {
        if (requiredBytes == 0u)
        {
            return false;
        }
        if (m_PaletteBuffer && m_PaletteBufferBytes >= requiredBytes)
        {
            return true;
        }
        if (requiredBytes > static_cast<u64>(std::numeric_limits<u32>::max()))
        {
            return false;
        }
        // Grown, never shrunk, and rounded to a power-of-two-ish step by
        // doubling: a crowd whose size oscillates by one character must not
        // reallocate the shared palette buffer every frame.
        u64 capacity = std::max<u64>(m_PaletteBufferBytes, sizeof(glm::mat4) * 128u);
        while (capacity < requiredBytes)
        {
            capacity *= 2u;
        }
        if (capacity > static_cast<u64>(std::numeric_limits<u32>::max()))
        {
            capacity = static_cast<u64>(std::numeric_limits<u32>::max());
        }
        // Binding 0 and never bound: the shader reaches this buffer by DEVICE
        // ADDRESS, so it consumes no number from the storage namespace, which
        // has had none free since SSBO_GPU_STATS.
        m_PaletteBuffer = StorageBuffer::Create(static_cast<u32>(capacity), 0, StorageBufferUsage::DynamicDraw);
        if (!m_PaletteBuffer)
        {
            m_PaletteBufferBytes = 0;
            m_PaletteAddress = 0;
            return false;
        }
        m_PaletteBufferBytes = capacity;
        m_PaletteAddress = m_PaletteBuffer->GetDeviceAddress();
        return m_PaletteAddress != 0u;
    }

    void DeformedSurfaceCache::EndFrame()
    {
        if (!m_Enabled)
        {
            return;
        }

        // Retire by absence, exactly as GPU Scene does: there is no despawn
        // signal, and a surface stops being offered the frame its entity dies,
        // its animation is removed, or its LOD switches to a different rest
        // buffer. This is also what stops a dead character's deformed stream
        // keeping a BLAS alive — the geometry record that referenced it is gone
        // the same frame, so RayTracingScene retires the structure and this
        // retires the memory behind it.
        for (auto& [key, entry] : m_Surfaces)
        {
            if (entry.LastSeenFrame != m_FrameNumber)
            {
                m_PendingRetires.push_back(key);
            }
        }
        for (const DeformedSurfaceKey& key : m_PendingRetires)
        {
            if (auto found = m_Surfaces.find(key); found != m_Surfaces.end())
            {
                ReleaseEntry(found->second);
                m_Surfaces.erase(found);
                ++m_Stats.Retired;
            }
        }
        m_PendingRetires.clear();

        if (!m_PaletteStaging.empty())
        {
            const u64 bytes = m_PaletteStaging.size() * sizeof(glm::mat4);
            if (EnsurePaletteBuffer(bytes))
            {
                m_PaletteBuffer->SetData(m_PaletteStaging.data(), static_cast<u32>(bytes), 0);
            }
            else
            {
                // Nothing can be deformed without palettes. Drop the queue
                // rather than dispatching against a null address, and say so —
                // every queued surface falls back to its rest pose this frame
                // and the refusal is counted where a reader will look for it.
                OLO_CORE_WARN("[RayTracing] could not stage {} bone-palette bytes; {} animated surfaces trace their "
                              "rest pose this frame",
                              bytes, m_Queue.size());
                m_Stats.Refused += static_cast<u32>(m_Queue.size());
                m_Queue.clear();
            }
        }

        // Standing totals, recomputed rather than incrementally maintained: an
        // incremental byte counter that drifts is indistinguishable from a leak
        // in exactly the report it exists to support.
        m_Stats.ResidentSurfaces = static_cast<u32>(m_Surfaces.size());
        m_Stats.ResidentBytes = 0;
        for (const auto& [key, entry] : m_Surfaces)
        {
            m_Stats.ResidentBytes += StreamBytes(entry.Capacity);
        }
        m_Stats.PaletteBytes = m_PaletteBufferBytes;
    }

    u32 DeformedSurfaceCache::Dispatch()
    {
        if (!m_Enabled || m_Queue.empty())
        {
            return 0;
        }
        if (m_PaletteAddress == 0u || !EnsureShader())
        {
            m_Stats.Refused += static_cast<u32>(m_Queue.size());
            m_Queue.clear();
            return 0;
        }

        m_Shader->Bind();

        u32 recorded = 0;
        for (const QueuedDispatch& item : m_Queue)
        {
            DeformParams params{};
            SplitAddress(item.RestAddress, params.RestVertexAddressLo, params.RestVertexAddressHi);
            SplitAddress(item.InfluenceAddress, params.BoneInfluenceAddressLo, params.BoneInfluenceAddressHi);
            SplitAddress(m_PaletteAddress + item.PaletteOffsetBytes, params.PaletteAddressLo, params.PaletteAddressHi);
            SplitAddress(item.OutputAddress, params.OutputVertexAddressLo, params.OutputVertexAddressHi);
            params.VertexCount = item.VertexCount;
            params.BoneCount = item.BoneCount;

            m_Params->SetData(&params, static_cast<u32>(sizeof(DeformParams)));
            m_Params->Bind();

            const u32 groups = (item.VertexCount + kWorkgroupSize - 1u) / kWorkgroupSize;
            RenderCommand::DispatchCompute(groups, 1u, 1u);

            ++recorded;
            m_Stats.VerticesDeformed += item.VertexCount;
        }

        m_Stats.Dispatched = recorded;
        m_Queue.clear();
        return recorded;
    }
} // namespace OloEngine::RayTracing

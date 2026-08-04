#include "OloEnginePCH.h"
#include "OloEngine/Renderer/Debug/ShaderDebugDraw.h"

#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace OloEngine
{
    namespace
    {
        [[nodiscard]] constexpr u32 ChannelIndex(ShaderDebugDrawPrimitive primitive)
        {
            return static_cast<u32>(std::to_underlying(primitive));
        }

        [[nodiscard]] constexpr u32 ChannelBinding(ShaderDebugDrawPrimitive primitive)
        {
            return ShaderBindingLayout::SSBO_DEBUG_DRAW_FIRST + ChannelIndex(primitive);
        }

        // Hard ceiling on entries per channel, applied to EVERY path that can
        // size a channel — including the CPU-push growth in BeginFrame, which
        // would otherwise bypass the SetChannelCapacity() cap entirely.
        //
        // At the widest entry (Box, 144 B) this is ~151 MB, which is already far
        // past useful for a debug overlay, and it keeps the byte computation
        // below comfortably inside 32 bits.
        inline constexpr u32 kMaxChannelCapacity = 1u << 20;

        [[nodiscard]] constexpr u32 ChannelBytes(ShaderDebugDrawPrimitive primitive, u32 capacity)
        {
            // Compute in u64 and narrow only after the clamp. The product is
            // capacity * up to 144 bytes, which overflows u32 around 30M
            // entries — and an overflowed size would have Resize() allocate a
            // SMALL buffer while channel.Capacity still records the large value,
            // so the following SetData writes past the allocation. The clamp
            // makes that unreachable; the u64 makes it unreachable even if the
            // clamp is ever loosened.
            const u64 clamped = std::min<u64>(capacity, kMaxChannelCapacity);
            const u64 bytes = static_cast<u64>(ShaderDebugDrawContract::kEntryArrayOffset) +
                              (static_cast<u64>(ShaderDebugDrawContract::EntryStride(primitive)) * clamped);
            return static_cast<u32>(bytes);
        }

        constexpr auto kAllPrimitives = std::array{
            ShaderDebugDrawPrimitive::Line,
            ShaderDebugDrawPrimitive::Circle,
            ShaderDebugDrawPrimitive::Rectangle,
            ShaderDebugDrawPrimitive::AABB,
            ShaderDebugDrawPrimitive::Box,
            ShaderDebugDrawPrimitive::Cone,
            ShaderDebugDrawPrimitive::Sphere,
        };
        static_assert(kAllPrimitives.size() == kShaderDebugDrawPrimitiveCount);
    } // namespace

    ShaderDebugDraw::Data& ShaderDebugDraw::Get()
    {
        static Data s_Data;
        return s_Data;
    }

    void ShaderDebugDraw::Init()
    {
        auto& data = Get();
        if (data.Initialised)
            return;

        // Allocate every channel at its header-only size and BIND IT NOW, once.
        //
        // The bind is not an optimisation, it is what makes the disabled path
        // safe: the GLSL push helpers open by reading `Capacity` out of the
        // channel, and reading an unbound SSBO is undefined in GL (the spec
        // explicitly permits program termination). A 32-byte allocation per
        // channel buys a guard that can never fault. Nothing else in the engine
        // binds 46..52, so this bind survives the whole process — the per-frame
        // rebind in BeginFrame() only exists to cover a reallocation.
        for (const auto primitive : kAllPrimitives)
        {
            auto& channel = data.Channels[ChannelIndex(primitive)];
            channel.Capacity = 0;
            channel.Buffer = StorageBuffer::Create(ChannelBytes(primitive, 0), ChannelBinding(primitive),
                                                   StorageBufferUsage::DynamicCopy);
            channel.Buffer->ClearData();
            channel.Buffer->Bind();
        }

        data.ParamsUBO = UniformBuffer::Create(static_cast<u32>(sizeof(ShaderDebugDrawParamsUBO)),
                                               ShaderBindingLayout::UBO_DEBUG_DRAW);
        data.Initialised = true;

        OLO_CORE_INFO("ShaderDebugDraw: initialised {} channels at bindings {}..{} (disabled, header-only).",
                      kShaderDebugDrawPrimitiveCount, ShaderBindingLayout::SSBO_DEBUG_DRAW_FIRST,
                      ShaderBindingLayout::SSBO_DEBUG_DRAW_FIRST + kShaderDebugDrawPrimitiveCount - 1);
    }

    void ShaderDebugDraw::Shutdown()
    {
        auto& data = Get();
        if (!data.Initialised)
            return;

        for (auto& channel : data.Channels)
        {
            if (channel.StagingBuffer.IsValid())
            {
                RenderCommand::DeleteBuffer(channel.StagingBuffer);
                channel.StagingBuffer = RHI::NullResource;
            }
            channel.Buffer.Reset();
            channel.Capacity = 0;
        }
        data.ParamsUBO.Reset();
        {
            // Close the gate and clear under ONE lock hold. Order matters: a
            // worker parked on this mutex re-checks IsEnabled() the moment it
            // acquires, so clearing the flags first means it observes the
            // shutdown and drops its append instead of repopulating the vectors
            // we just emptied.
            const std::scoped_lock lock(data.CpuMutex);
            data.Enabled = false;
            data.Initialised = false;
            ClearCpuEntries();
        }
        data.Stats = {};
        data.StatsStaged = false;
    }

    bool ShaderDebugDraw::IsInitialised()
    {
        return Get().Initialised;
    }

    void ShaderDebugDraw::SetEnabled(bool enabled)
    {
        Get().Enabled = enabled;
    }

    bool ShaderDebugDraw::IsEnabled()
    {
        const auto& data = Get();
        return data.Enabled && data.Initialised;
    }

    void ShaderDebugDraw::SetLineWidth(f32 pixels)
    {
        Get().LineWidth = std::clamp(pixels, 1.0f, 32.0f);
    }

    f32 ShaderDebugDraw::GetLineWidth()
    {
        return Get().LineWidth;
    }

    void ShaderDebugDraw::SetChannelCapacity(u32 entries)
    {
        Get().RequestedCapacity = std::min(entries, kMaxChannelCapacity);
    }

    u32 ShaderDebugDraw::GetChannelCapacity()
    {
        return Get().RequestedCapacity;
    }

    // -------------------------------------------------------------------------
    // CPU appenders
    // -------------------------------------------------------------------------

    void ShaderDebugDraw::DrawLine(const glm::vec3& start, const glm::vec3& end, const glm::vec3& color,
                                   ShaderDebugDrawSpace space)
    {
        if (!IsEnabled())
            return;
        auto& data = Get();
        const std::scoped_lock lock(data.CpuMutex);
        // Re-check under the lock: the pre-lock IsEnabled() only proves the
        // feature was on a moment ago, and Shutdown/BeginFrame clear these
        // vectors while holding this same mutex. Without the recheck a worker
        // parked on the lock appends AFTER the clear, leaving an entry that
        // outlives the frame that was allowed to produce it.
        if (!IsEnabled())
            return;
        data.CpuLines.push_back(ShaderDebugDrawLine{ start, static_cast<u32>(std::to_underlying(space)), end, 0.0f,
                                                     color, 0.0f });
    }

    void ShaderDebugDraw::DrawCircle(const glm::vec3& center, const glm::vec3& normal, f32 radius,
                                     const glm::vec3& color, ShaderDebugDrawSpace space)
    {
        if (!IsEnabled())
            return;
        auto& data = Get();
        const std::scoped_lock lock(data.CpuMutex);
        // Re-check under the lock: the pre-lock IsEnabled() only proves the
        // feature was on a moment ago, and Shutdown/BeginFrame clear these
        // vectors while holding this same mutex. Without the recheck a worker
        // parked on the lock appends AFTER the clear, leaving an entry that
        // outlives the frame that was allowed to produce it.
        if (!IsEnabled())
            return;
        data.CpuCircles.push_back(ShaderDebugDrawCircle{ center, static_cast<u32>(std::to_underlying(space)), normal,
                                                         radius, color, 0.0f });
    }

    void ShaderDebugDraw::DrawRectangle(const glm::vec3& center, const glm::vec3& axisU, const glm::vec3& axisV,
                                        const glm::vec3& color, ShaderDebugDrawSpace space)
    {
        if (!IsEnabled())
            return;
        auto& data = Get();
        const std::scoped_lock lock(data.CpuMutex);
        // Re-check under the lock: the pre-lock IsEnabled() only proves the
        // feature was on a moment ago, and Shutdown/BeginFrame clear these
        // vectors while holding this same mutex. Without the recheck a worker
        // parked on the lock appends AFTER the clear, leaving an entry that
        // outlives the frame that was allowed to produce it.
        if (!IsEnabled())
            return;
        data.CpuRectangles.push_back(ShaderDebugDrawRectangle{ center, static_cast<u32>(std::to_underlying(space)),
                                                               axisU, 0.0f, axisV, 0.0f, color, 0.0f });
    }

    void ShaderDebugDraw::DrawAABB(const glm::vec3& min, const glm::vec3& max, const glm::vec3& color,
                                   ShaderDebugDrawSpace space)
    {
        if (!IsEnabled())
            return;
        auto& data = Get();
        const std::scoped_lock lock(data.CpuMutex);
        // Re-check under the lock: the pre-lock IsEnabled() only proves the
        // feature was on a moment ago, and Shutdown/BeginFrame clear these
        // vectors while holding this same mutex. Without the recheck a worker
        // parked on the lock appends AFTER the clear, leaving an entry that
        // outlives the frame that was allowed to produce it.
        if (!IsEnabled())
            return;
        data.CpuAABBs.push_back(ShaderDebugDrawAABB{ min, static_cast<u32>(std::to_underlying(space)), max, 0.0f, color,
                                                     0.0f });
    }

    void ShaderDebugDraw::DrawBox(const std::array<glm::vec3, 8>& corners, const glm::vec3& color,
                                  ShaderDebugDrawSpace space)
    {
        if (!IsEnabled())
            return;
        ShaderDebugDrawBox entry;
        for (u32 i = 0; i < 8; ++i)
            entry.Corners[i] = glm::vec4(corners[i], 1.0f);
        entry.Color = color;
        entry.Space = static_cast<u32>(std::to_underlying(space));

        auto& data = Get();
        const std::scoped_lock lock(data.CpuMutex);
        // Re-check under the lock: the pre-lock IsEnabled() only proves the
        // feature was on a moment ago, and Shutdown/BeginFrame clear these
        // vectors while holding this same mutex. Without the recheck a worker
        // parked on the lock appends AFTER the clear, leaving an entry that
        // outlives the frame that was allowed to produce it.
        if (!IsEnabled())
            return;
        data.CpuBoxes.push_back(entry);
    }

    void ShaderDebugDraw::DrawCone(const glm::vec3& apex, const glm::vec3& axis, f32 radius, const glm::vec3& color,
                                   ShaderDebugDrawSpace space)
    {
        if (!IsEnabled())
            return;
        auto& data = Get();
        const std::scoped_lock lock(data.CpuMutex);
        // Re-check under the lock: the pre-lock IsEnabled() only proves the
        // feature was on a moment ago, and Shutdown/BeginFrame clear these
        // vectors while holding this same mutex. Without the recheck a worker
        // parked on the lock appends AFTER the clear, leaving an entry that
        // outlives the frame that was allowed to produce it.
        if (!IsEnabled())
            return;
        data.CpuCones.push_back(ShaderDebugDrawCone{ apex, static_cast<u32>(std::to_underlying(space)), axis, radius,
                                                     color, 0.0f });
    }

    void ShaderDebugDraw::DrawSphere(const glm::vec3& center, f32 radius, const glm::vec3& color,
                                     ShaderDebugDrawSpace space)
    {
        if (!IsEnabled())
            return;
        auto& data = Get();
        const std::scoped_lock lock(data.CpuMutex);
        // Re-check under the lock: the pre-lock IsEnabled() only proves the
        // feature was on a moment ago, and Shutdown/BeginFrame clear these
        // vectors while holding this same mutex. Without the recheck a worker
        // parked on the lock appends AFTER the clear, leaving an entry that
        // outlives the frame that was allowed to produce it.
        if (!IsEnabled())
            return;
        data.CpuSpheres.push_back(ShaderDebugDrawSphere{ center, radius, color,
                                                         static_cast<u32>(std::to_underlying(space)) });
    }

    void ShaderDebugDraw::ClearCpuEntries()
    {
        auto& data = Get();
        data.CpuLines.clear();
        data.CpuCircles.clear();
        data.CpuRectangles.clear();
        data.CpuAABBs.clear();
        data.CpuBoxes.clear();
        data.CpuCones.clear();
        data.CpuSpheres.clear();
    }

    // -------------------------------------------------------------------------
    // Frame lifecycle
    // -------------------------------------------------------------------------

    void ShaderDebugDraw::EnsureChannelCapacity(Channel& channel, ShaderDebugDrawPrimitive primitive, u32 capacity)
    {
        // Clamp HERE as well as at the call sites, so `channel.Capacity` and the
        // allocation ChannelBytes() sized can never disagree — a Capacity that
        // exceeds the buffer is exactly the state that turns the next SetData
        // into an out-of-bounds write.
        capacity = std::min(capacity, kMaxChannelCapacity);
        if (channel.Capacity == capacity || !channel.Buffer)
            return;

        channel.Buffer->Resize(ChannelBytes(primitive, capacity));
        channel.Capacity = capacity;
        // Resize reallocates the underlying object, so re-establish the binding.
        channel.Buffer->Bind();
    }

    void ShaderDebugDraw::UploadChannel(ShaderDebugDrawPrimitive primitive, const void* entries, u32 entryCount,
                                        u32 requestedCount)
    {
        auto& data = Get();
        auto& channel = data.Channels[ChannelIndex(primitive)];
        if (!channel.Buffer)
            return;

        ShaderDebugDrawChannelHeader header;
        header.VertexCount = ShaderDebugDrawContract::VertexCountPerInstance(primitive);
        header.InstanceCount = entryCount;
        header.First = 0;
        header.BaseInstance = 0;
        header.Capacity = channel.Capacity;
        // Unclamped on purpose — a CPU-side overflow is reported through the same
        // `RequestCount > Capacity` test a GPU-side one is, so a channel that is
        // too small for the CPU pushes alone still raises the visible flag.
        header.RequestCount = requestedCount;

        channel.Buffer->SetData(&header, static_cast<u32>(sizeof(header)), 0);
        if (entryCount > 0 && entries != nullptr)
        {
            channel.Buffer->SetData(entries, ShaderDebugDrawContract::EntryStride(primitive) * entryCount,
                                    ShaderDebugDrawContract::kEntryArrayOffset);
        }

        channel.CpuCount = entryCount;
        channel.CpuRequested = requestedCount;
    }

    void ShaderDebugDraw::BeginFrame()
    {
        auto& data = Get();
        if (!data.Initialised)
            return;

        // Drain last frame's staged headers first — the copy that produced them
        // was issued a whole frame ago, so the read is effectively free.
        ReadbackStats();

        if (!data.Enabled)
        {
            // Disabled: collapse every channel back to header-only with
            // Capacity == 0 and do NOTHING per frame after that. The one-time
            // collapse costs a Resize on the frame the toggle flips off; steady
            // state is a single `channel.Capacity == 0` test.
            bool needsCollapse = false;
            for (const auto& channel : data.Channels)
                needsCollapse = needsCollapse || channel.Capacity != 0;

            if (needsCollapse)
            {
                for (const auto primitive : kAllPrimitives)
                {
                    auto& channel = data.Channels[ChannelIndex(primitive)];
                    EnsureChannelCapacity(channel, primitive, 0);
                    UploadChannel(primitive, nullptr, 0, 0);
                }
                data.Stats = {};
            }
            // Under the lock, same as the enabled path below — and the clear is
            // final rather than best-effort, because data.Enabled is already
            // false here and the appenders re-check it after acquiring this
            // same mutex. A worker that passed its pre-lock check and is parked
            // here therefore observes the disable and drops its append instead
            // of repopulating what we just emptied.
            {
                const std::scoped_lock lock(data.CpuMutex);
                ClearCpuEntries();
            }
            return;
        }

        const std::scoped_lock lock(data.CpuMutex);

        const auto prepare = [&](ShaderDebugDrawPrimitive primitive, const auto& cpuEntries)
        {
            auto& channel = data.Channels[ChannelIndex(primitive)];
            const auto requested = static_cast<u32>(cpuEntries.size());
            // Grow past the configured capacity when the CPU alone needs more —
            // a CPU push is not a "best effort" append the way a GPU one is (the
            // caller can see the count), so silently dropping it would be a
            // worse failure than spending the memory.
            //
            // Still bounded by kMaxChannelCapacity: `requested` comes from an
            // unbounded caller (a gameplay system in a loop), and without the
            // clamp this path would bypass the SetChannelCapacity() cap
            // entirely. Past the ceiling the excess is reported as an overflow
            // like any other, which is the honest outcome — an allocation that
            // grows without bound is not.
            const u32 capacity = std::min(std::max(data.RequestedCapacity, requested), kMaxChannelCapacity);
            EnsureChannelCapacity(channel, primitive, capacity);
            const u32 accepted = std::min(requested, channel.Capacity);
            UploadChannel(primitive, cpuEntries.data(), accepted, requested);
        };

        prepare(ShaderDebugDrawPrimitive::Line, data.CpuLines);
        prepare(ShaderDebugDrawPrimitive::Circle, data.CpuCircles);
        prepare(ShaderDebugDrawPrimitive::Rectangle, data.CpuRectangles);
        prepare(ShaderDebugDrawPrimitive::AABB, data.CpuAABBs);
        prepare(ShaderDebugDrawPrimitive::Box, data.CpuBoxes);
        prepare(ShaderDebugDrawPrimitive::Cone, data.CpuCones);
        prepare(ShaderDebugDrawPrimitive::Sphere, data.CpuSpheres);

        data.CpuLines.clear();
        data.CpuCircles.clear();
        data.CpuRectangles.clear();
        data.CpuAABBs.clear();
        data.CpuBoxes.clear();
        data.CpuCones.clear();
        data.CpuSpheres.clear();
    }

    void ShaderDebugDraw::StageStatsForReadback()
    {
        auto& data = Get();
        if (!data.Initialised || !data.Enabled)
            return;

        // Copy each channel's 32-byte header into a dedicated DeviceToHost buffer
        // rather than reading the channel directly.
        //
        // The channel is GL_DYNAMIC_COPY and has to stay that way: the GPU writes
        // it (the push helpers' atomics) AND reads it every frame as the
        // GL_DRAW_INDIRECT_BUFFER of its own draw, so it must live in video
        // memory. A CPU glGetNamedBufferSubData straight off it makes NVIDIA log
        // "CPU is consuming buffer object data ... inconsistent with this usage
        // pattern" (131188) and then migrate the buffer VIDEO -> HOST (131186),
        // permanently slowing the indirect draw that reads it. Same trap, same
        // fix, as VirtualMeshRegistry::ReadFrameCullStats.
        //
        // The copy is issued here and READ at the next BeginFrame(), so the stats
        // are one frame old and the read never blocks on this frame's GPU work.
        // For an overflow flag that latency is irrelevant.
        constexpr u64 headerBytes = sizeof(ShaderDebugDrawChannelHeader);
        for (auto& channel : data.Channels)
        {
            if (!channel.Buffer)
                continue;
            if (!channel.StagingBuffer.IsValid())
            {
                channel.StagingBuffer = RenderCommand::CreateBufferHandle();
                RenderCommand::AllocateBufferStorage(channel.StagingBuffer, headerBytes,
                                                     RHI::MemoryResidency::DeviceToHost);
            }
            RenderCommand::CopyBufferSubData(channel.Buffer->GetRHIHandle(), channel.StagingBuffer, 0, 0, headerBytes);
        }
        data.StatsStaged = true;
    }

    void ShaderDebugDraw::ReadbackStats()
    {
        auto& data = Get();
        if (!data.StatsStaged)
            return;
        data.StatsStaged = false;

        for (u32 i = 0; i < kShaderDebugDrawPrimitiveCount; ++i)
        {
            auto& channel = data.Channels[i];
            if (!channel.StagingBuffer.IsValid())
                continue;

            ShaderDebugDrawChannelHeader header{};
            RenderCommand::ReadBufferSubData(channel.StagingBuffer, 0, sizeof(header), &header);

            auto& stats = data.Stats.Channels[i];
            stats.Capacity = header.Capacity;
            stats.Drawn = header.InstanceCount;
            stats.Requested = header.RequestCount;
            stats.CpuPushes = channel.CpuRequested;
        }
        data.Stats.StatsValid = true;

        // Overflow is a "your draws were silently dropped" condition, which is
        // the exact hour-wasting failure this feature exists to remove — so it is
        // logged, not just exposed. Warn-once per channel per session; the ImGui
        // overlay carries the live number.
        static std::array<bool, kShaderDebugDrawPrimitiveCount> s_Warned{};
        for (u32 i = 0; i < kShaderDebugDrawPrimitiveCount; ++i)
        {
            const auto& stats = data.Stats.Channels[i];
            if (!stats.Overflowed())
                continue;
            if (std::exchange(s_Warned[i], true))
                continue;
            OLO_CORE_WARN("ShaderDebugDraw: {} channel OVERFLOWED — {} draws requested, capacity {}, {} dropped. "
                          "Raise ShaderDebugDraw::SetChannelCapacity().",
                          ShaderDebugDrawContract::Name(static_cast<ShaderDebugDrawPrimitive>(i)), stats.Requested,
                          stats.Capacity, stats.Dropped());
        }
    }

    void ShaderDebugDraw::UploadDrawParams(const glm::mat4& viewProjection,
                                           const glm::mat4& observerInvViewProjection, const glm::vec2& viewportSize,
                                           ShaderDebugDrawPrimitive primitive)
    {
        auto& data = Get();
        if (!data.ParamsUBO)
            return;

        ShaderDebugDrawParamsUBO params;
        params.ViewProjection = viewProjection;
        params.ObserverInvViewProjection = observerInvViewProjection;
        params.ViewportSize = viewportSize;
        params.LineWidth = data.LineWidth;
        params.PrimitiveType = ChannelIndex(primitive);

        data.ParamsUBO->SetData(&params, static_cast<u32>(sizeof(params)));
        data.ParamsUBO->Bind();
    }

    Ref<StorageBuffer> ShaderDebugDraw::GetChannelBuffer(ShaderDebugDrawPrimitive primitive)
    {
        return Get().Channels[ChannelIndex(primitive)].Buffer;
    }

    Ref<UniformBuffer> ShaderDebugDraw::GetParamsUBO()
    {
        return Get().ParamsUBO;
    }

    bool ShaderDebugDraw::HasWorkThisFrame()
    {
        const auto& data = Get();
        if (!data.Initialised || !data.Enabled)
            return false;
        for (const auto& channel : data.Channels)
        {
            if (channel.Capacity > 0)
                return true;
        }
        return false;
    }

    const ShaderDebugDrawStats& ShaderDebugDraw::GetStats()
    {
        return Get().Stats;
    }
} // namespace OloEngine

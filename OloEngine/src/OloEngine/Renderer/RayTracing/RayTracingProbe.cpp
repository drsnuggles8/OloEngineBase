#include "OloEnginePCH.h"
#include "OloEngine/Renderer/RayTracing/RayTracingProbe.h"

#include "OloEngine/Renderer/GPUScene/GPUScene.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RayTracing/RayTracingScene.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"

#include <algorithm>
#include <cmath>

namespace OloEngine::RayTracing
{
    namespace
    {
        // RayTracingProbe.comp's local_size_x. The dispatch's group count is
        // derived from it, so the two must not drift apart.
        constexpr u32 kWorkgroupSize = 64;
        static_assert(RayTracingProbe::kMaxRays <= kWorkgroupSize,
                      "kMaxRays over one workgroup would need the group count to grow with it");

        // One ray as the shader reads it: origin + tMin, then direction + tMax.
        struct GpuRay
        {
            glm::vec4 OriginAndTMin{ 0.0f };
            glm::vec4 DirectionAndTMax{ 0.0f };
        };
        static_assert(sizeof(GpuRay) == 32, "GpuRay must match OloRtRayBuffer's two-vec4 stride");

        [[nodiscard]] glm::uvec2 SplitAddress(u64 address)
        {
            return glm::uvec2{ static_cast<u32>(address & 0xFFFFFFFFull), static_cast<u32>(address >> 32) };
        }

        [[nodiscard]] bool IsFinite(const glm::vec3& value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }
    } // namespace

    RayTracingProbe::RayTracingProbe() = default;

    RayTracingProbe::~RayTracingProbe()
    {
        // The ring's fences and buffers are raw handles: unlike the
        // Ref<StorageBuffer> members beside them nothing frees them on its own.
        // Releasing here as well as in Shutdown() puts them on the same lifetime
        // as those members, so a probe dropped with its renderer does not leak a
        // sync object per session.
        ReleaseRing();
    }

    bool RayTracingProbe::SubmitBatch(const Batch& batch, std::string& outError)
    {
        if (batch.Rays.empty())
        {
            outError = "No rays to trace.";
            return false;
        }
        if (batch.Rays.size() > static_cast<sizet>(kMaxRays))
        {
            outError = "Too many rays: " + std::to_string(batch.Rays.size()) + " (the probe traces at most " +
                       std::to_string(kMaxRays) + " per call).";
            return false;
        }

        Batch normalized = batch;
        // The instance mask is ANDed with each instance's own mask and the
        // shader takes the low byte; a zero mask makes EVERY instance
        // unhittable, so every ray would come back a miss with nothing to say
        // why. Refuse it rather than answer it.
        normalized.InstanceMask &= 0xFFu;
        if (normalized.InstanceMask == 0u)
        {
            outError = "instanceMask is 0, which makes every instance unhittable — every ray would report a "
                       "miss for a reason that has nothing to do with the scene.";
            return false;
        }

        for (sizet i = 0; i < normalized.Rays.size(); ++i)
        {
            Ray& ray = normalized.Rays[i];
            const std::string which = "ray " + std::to_string(i) + ": ";
            // A malformed ray is UNDEFINED BEHAVIOUR at rayQueryInitializeEXT,
            // not a miss — the spec requires finite origin/direction/bounds,
            // tMin >= 0 and tMin <= tMax. Refused here so the caller gets a
            // reason; the shader re-checks and reports a defined miss, because
            // it also traces rays this path never saw.
            if (!IsFinite(ray.Origin) || !IsFinite(ray.Direction))
            {
                outError = which + "origin and direction must be finite.";
                return false;
            }
            if (!std::isfinite(ray.TMin) || !std::isfinite(ray.TMax))
            {
                outError = which + "tMin and tMax must be finite.";
                return false;
            }
            if (ray.TMin < 0.0f || ray.TMin > ray.TMax)
            {
                outError = which + "requires 0 <= tMin <= tMax (got tMin " + std::to_string(ray.TMin) +
                           ", tMax " + std::to_string(ray.TMax) + ").";
                return false;
            }
            const f32 lengthSq = glm::dot(ray.Direction, ray.Direction);
            if (!std::isfinite(lengthSq) || lengthSq < 1e-12f)
            {
                outError = which + "direction must have finite, non-zero length.";
                return false;
            }
            // Normalized here rather than trusting the caller: the reported
            // distance is only a WORLD distance — and the CPU-side hit position
            // only agrees with the GPU's traversal — if the direction is unit
            // length.
            ray.Direction /= std::sqrt(lengthSq);
        }

        m_PendingBatch = std::move(normalized);
        m_HasPendingBatch = true;
        return true;
    }

    bool RayTracingProbe::EnsureShader()
    {
        if (m_ShaderLoaded)
            return true;
        if (m_ShaderLoadFailed)
            return false;

        m_Shader = ComputeShader::Create("assets/shaders/compute/RayTracingProbe.comp");
        if (!m_Shader || !m_Shader->IsValid())
        {
            // Latched, not retried every frame: a shader that failed to compile
            // will fail again, and a per-frame recompile attempt would make the
            // editor's log unreadable (the GLStateGuard lesson).
            m_ShaderLoadFailed = true;
            m_Shader = nullptr;
            OLO_CORE_ERROR("RayTracingProbe: RayTracingProbe.comp failed to compile — olo_rt_trace_ray is "
                           "unavailable this session. Check the ray-query extension set and the compute "
                           "include path.");
            return false;
        }
        m_ShaderLoaded = true;
        return true;
    }

    bool RayTracingProbe::EnsureBuffers()
    {
        if (m_RayBuffer && m_HitBuffer && m_ParamsUBO)
            return true;

        constexpr u32 rayBytes = kMaxRays * static_cast<u32>(sizeof(GpuRay));
        constexpr u32 hitBytes = kMaxRays * static_cast<u32>(sizeof(GpuHit));

        // kNoBinding: both buffers are reached ONLY by device address through
        // GL_EXT_buffer_reference, exactly as the shader declares them. The
        // portable SSBO namespace has been full since SSBO_GPU_STATS, and this
        // is how a by-address buffer stays out of it instead of squatting on a
        // slot some other pass owns.
        if (!m_RayBuffer)
            m_RayBuffer = StorageBuffer::Create(rayBytes, StorageBuffer::kNoBinding, StorageBufferUsage::DynamicDraw);
        if (!m_HitBuffer)
            m_HitBuffer = StorageBuffer::Create(hitBytes, StorageBuffer::kNoBinding, StorageBufferUsage::DynamicCopy);
        if (!m_ParamsUBO)
            m_ParamsUBO = UniformBuffer::Create(static_cast<u32>(sizeof(GpuParams)), ShaderBindingLayout::UBO_RAY_TRACING);

        if (!m_RayBuffer || !m_HitBuffer || !m_ParamsUBO)
        {
            OLO_CORE_ERROR("RayTracingProbe: GPU buffer allocation failed ({} ray bytes, {} hit bytes)", rayBytes,
                           hitBytes);
            m_RayBuffer = nullptr;
            m_HitBuffer = nullptr;
            m_ParamsUBO = nullptr;
            return false;
        }

        for (auto& slot : m_Ring)
        {
            if (slot.Buffer.IsValid())
                continue;
            slot.Buffer = RenderCommand::CreateBufferHandle();
            RenderCommand::AllocateBufferStorage(slot.Buffer, hitBytes, RHI::MemoryResidency::DeviceToHost);
            slot.Pending = false;
            slot.Fence = 0;
        }
        return true;
    }

    void RayTracingProbe::ReleaseRing()
    {
        for (auto& slot : m_Ring)
        {
            // Destroy the fence BEFORE the buffer, and destroy it even while the
            // slot is pending: a pending slot's fence is a live GPU sync object,
            // and dropping the handle without DestroyFence leaks it for the life
            // of the context.
            if (slot.Fence != 0)
            {
                RenderCommand::DestroyFence(slot.Fence);
                slot.Fence = 0;
            }
            if (slot.Buffer.IsValid())
            {
                RenderCommand::DeleteBuffer(slot.Buffer);
                slot.Buffer = RHI::NullResource;
            }
            slot.Pending = false;
            slot.Dispatched = {};
        }
        m_NextSlot = 0;
        m_SlotsInFlight = 0;
    }

    void RayTracingProbe::Shutdown()
    {
        ReleaseRing();
        m_RayBuffer = nullptr;
        m_HitBuffer = nullptr;
        m_ParamsUBO = nullptr;
        m_Shader = nullptr;
        m_ShaderLoaded = false;
        m_ShaderLoadFailed = false;
        m_HasPendingBatch = false;
        m_PendingBatch = {};
        m_Latest = {};
        m_FrameIndex = 0;
        m_UnavailableReason.clear();
    }

    void RayTracingProbe::Poll()
    {
        OLO_PROFILE_FUNCTION();

        // The frame counter advances HERE, not in Dispatch(). Latency counts
        // FRAMES between a request and its answer, and the probe only dispatches
        // on frames something asked it to — so counting dispatches would report
        // a two-frame-old answer as zero-latency, which is precisely the reading
        // that would make a synchronous readback and this ring look identical.
        ++m_FrameIndex;

        u32 inFlight = 0;
        // OLDEST FIRST, not array order. m_NextSlot is the slot the next capture
        // will use, so it is also the oldest one still in flight, and walking
        // from there wraps through the ring in ISSUE order. Array order would
        // let a wrapped ring publish an older answer after a newer one had
        // already retired.
        for (u32 offset = 0; offset < kRingSlots; ++offset)
        {
            RingSlot& slot = m_Ring[(m_NextSlot + offset) % kRingSlots];
            if (!slot.Pending)
                continue;

            // Ask, never wait. There is no ClientWaitFence here, and adding one
            // would stall the editor's frame on a diagnostic.
            if (!RenderCommand::IsFenceSignaled(slot.Fence))
            {
                ++inFlight;
                continue;
            }

            const sizet rayCount = slot.Dispatched.Rays.size();
            std::vector<GpuHit> gpuHits(rayCount);
            RenderCommand::ReadBufferSubData(slot.Buffer, 0, static_cast<u32>(rayCount * sizeof(GpuHit)),
                                             gpuHits.data());

            RenderCommand::DestroyFence(slot.Fence);
            slot.Fence = 0;
            slot.Pending = false;

            if (m_Latest.Valid && slot.FrameIndex <= m_Latest.FrameIndex)
                continue;

            m_Latest.Valid = true;
            m_Latest.BatchId = slot.Dispatched.BatchId;
            m_Latest.RayFlags = slot.Dispatched.RayFlags;
            m_Latest.InstanceMask = slot.Dispatched.InstanceMask;
            m_Latest.FrameIndex = slot.FrameIndex;
            m_Latest.Latency = static_cast<u32>(m_FrameIndex - slot.FrameIndex);
            m_Latest.Rays = slot.Dispatched.Rays;
            m_Latest.Hits.assign(rayCount, Hit{});

            for (sizet i = 0; i < rayCount; ++i)
            {
                const GpuHit& gpu = gpuHits[i];
                Hit& hit = m_Latest.Hits[i];
                // DECODE, THEN VALIDATE. The `w` lane carries the hit flag
                // explicitly so a legitimate zero-distance hit is not read as a
                // miss — but every one of these floats crossed a GPU buffer, and
                // this repo's rule is that such a float is validated. A NaN
                // distance reaching the reported result would propagate straight
                // into whatever the caller computes from it.
                const bool flagged = gpu.DistanceAndBarycentrics.w > 0.5f;
                const f32 distance = gpu.DistanceAndBarycentrics.x;
                hit.IsHit = flagged && std::isfinite(distance) && distance >= 0.0f;
                if (!hit.IsHit)
                    continue;

                hit.Distance = distance;
                hit.Barycentrics = { gpu.DistanceAndBarycentrics.y, gpu.DistanceAndBarycentrics.z };
                if (!std::isfinite(hit.Barycentrics.x) || !std::isfinite(hit.Barycentrics.y))
                    hit.Barycentrics = glm::vec2(0.0f);
                hit.InstanceSlot = gpu.Ids.x;
                hit.PrimitiveIndex = gpu.Ids.y;
                hit.MaterialSlot = gpu.Ids.z;
                hit.GeometrySlot = gpu.Ids.w;
                hit.UV = { gpu.UVAndPad.x, gpu.UVAndPad.y };
                if (!std::isfinite(hit.UV.x) || !std::isfinite(hit.UV.y))
                    hit.UV = glm::vec2(0.0f);
                const glm::vec3 normal{ gpu.WorldNormalAndWindingSign.x, gpu.WorldNormalAndWindingSign.y,
                                        gpu.WorldNormalAndWindingSign.z };
                hit.WorldNormal = IsFinite(normal) ? normal : glm::vec3(0.0f);
                hit.WindingSign = std::isfinite(gpu.WorldNormalAndWindingSign.w) ? gpu.WorldNormalAndWindingSign.w : 1.0f;
                // From the ray THIS slot was dispatched with, not from whatever
                // is current now: that is what keeps the position exact and what
                // makes a late answer still correct rather than merely stale.
                const Ray& ray = slot.Dispatched.Rays[i];
                hit.Position = ray.Origin + ray.Direction * hit.Distance;
            }
        }
        m_SlotsInFlight = inFlight;
    }

    bool RayTracingProbe::Dispatch(const RayTracingScene& scene, const GPUScene& gpuScene)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_HasPendingBatch)
            return false;

        // Every refusal below sets a reason and CONSUMES the batch. Leaving it
        // queued would make the tool above wait out its whole settle window for
        // a dispatch that cannot happen, and report a timeout instead of the
        // reason — which is the difference between "your GPU has no ray tracing"
        // and "something went wrong, try again".
        const auto refuse = [this](std::string reason)
        {
            m_UnavailableReason = std::move(reason);
            m_HasPendingBatch = false;
            return false;
        };

        if (!RenderCommand::SupportsRayTracing())
        {
            return refuse("This backend has no hardware ray tracing. GL_EXT_ray_query has no OpenGL "
                          "representation, so the probe is Vulkan-only — relaunch the editor with --rhi=vulkan.");
        }
        if (!scene.IsAvailable())
            return refuse("The ray-tracing scene is not available on this device.");

        const u64 tlasAddress = scene.GetTlasDeviceAddress();
        if (tlasAddress == 0u)
        {
            return refuse("No TLAS has been built yet. A scene with no traceable geometry is exactly this — "
                          "read olo_rt_scene_stats for what the builder did and did not accept.");
        }
        if (!EnsureShader())
            return refuse("RayTracingProbe.comp failed to compile on this device; see OloEngine.log.");
        if (!EnsureBuffers())
            return refuse("The probe's GPU buffers could not be allocated.");

        const Batch batch = std::move(m_PendingBatch);
        m_PendingBatch = {};
        m_HasPendingBatch = false;
        const u32 rayCount = static_cast<u32>(batch.Rays.size());

        std::vector<GpuRay> gpuRays(rayCount);
        for (u32 i = 0; i < rayCount; ++i)
        {
            gpuRays[i].OriginAndTMin = glm::vec4(batch.Rays[i].Origin, batch.Rays[i].TMin);
            gpuRays[i].DirectionAndTMax = glm::vec4(batch.Rays[i].Direction, batch.Rays[i].TMax);
        }
        m_RayBuffer->SetData(gpuRays.data(), rayCount * static_cast<u32>(sizeof(GpuRay)));

        // COMMAND-ORDERED addresses, not GetDeviceAddress(). A CPU write into a
        // live buffer installs a frame snapshot on the Vulkan backend, and the
        // dispatch must read the version visible at THIS point in the command
        // stream — the plain address can name the pre-write storage. The GPU
        // Scene tables come back the same way, through the accessor
        // RayTracedShadowPass already uses.
        const u64 rayAddress = m_RayBuffer->GetCommandOrderedDeviceAddress();
        const u64 hitAddress = m_HitBuffer->GetCommandOrderedDeviceAddress();
        const auto tableAddresses = gpuScene.GetRayTracingReadAddresses();
        if (rayAddress == 0u || hitAddress == 0u || tableAddresses[0] == 0u || tableAddresses[1] == 0u ||
            tableAddresses[2] == 0u)
        {
            // Not an assert and not a silent zero dispatch: a null address in
            // the UBO makes the shader dereference nothing, which is a device
            // fault rather than an empty answer.
            return refuse("A buffer device address resolved to zero, so the trace was not dispatched (the ray, "
                          "hit or GPU Scene table buffers are not addressable on this backend).");
        }

        GpuParams params{};
        params.TlasAddress = SplitAddress(tlasAddress);
        params.RayAddress = SplitAddress(rayAddress);
        params.HitAddress = SplitAddress(hitAddress);
        params.InstanceTableAddress = SplitAddress(tableAddresses[0]);
        params.GeometryTableAddress = SplitAddress(tableAddresses[1]);
        params.MaterialTableAddress = SplitAddress(tableAddresses[2]);
        params.RayCount = rayCount;
        params.InstanceSlotCount = gpuScene.GetInstanceSlotCount();
        params.GeometrySlotCount = gpuScene.GetGeometrySlotCount();
        params.MaterialSlotCount = gpuScene.GetMaterialSlotCount();
        params.RayFlags = batch.RayFlags;
        params.InstanceMask = batch.InstanceMask;

        // Rebind binding 65 before writing: other passes displace this indexed
        // binding, and RayTracedShadow.glsl declares its own block at the same
        // number (see UBO_RAY_TRACING's comment for why sharing it is safe).
        m_ParamsUBO->Bind();
        m_ParamsUBO->SetData(&params, static_cast<u32>(sizeof(params)));

        m_Shader->Bind();
        RenderCommand::DispatchCompute((rayCount + kWorkgroupSize - 1u) / kWorkgroupSize, 1u, 1u);

        m_UnavailableReason.clear();
        // A staged capture is what makes this batch answerable. If the ring had
        // no free slot, or the driver refused a fence, the dispatch still RAN
        // but nothing will ever read it back — so this is a refusal with a
        // reason, not a success. Without this the tool above waits out its whole
        // settle window and then reports 'pending', which tells the caller the
        // editor did not render: the exact misdiagnosis every other refusal
        // here exists to prevent.
        if (!CaptureResult(batch))
        {
            return refuse("The trace was dispatched but its readback could not be staged (" +
                          std::to_string(m_SlotsInFlight) + " of " + std::to_string(kRingSlots) +
                          " readback slots were still in flight, or the driver refused a fence). Retry — "
                          "a persistently full ring means the CPU is running far ahead of the GPU.");
        }
        return true;
    }

    bool RayTracingProbe::CaptureResult(const Batch& batch)
    {
        RingSlot& slot = m_Ring[m_NextSlot];
        if (slot.Pending || !slot.Buffer.IsValid())
        {
            // A full ring means every slot is still executing. Skip the capture
            // and keep the newest retired answer rather than blocking on the
            // oldest — but SAY SO, because this batch's answer is now never
            // coming and the caller has to hear that rather than wait for it.
            return false;
        }

        // The dispatch wrote the hits through a buffer reference; the copy below
        // is a buffer-update client and needs its own barrier class.
        // ShaderStorage orders the shader writes, BufferUpdate orders the copy
        // against them — both, because dropping either makes the copy read a
        // value that is right most of the time.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::BufferUpdate);
        const u32 bytes = static_cast<u32>(batch.Rays.size() * sizeof(GpuHit));
        RenderCommand::CopyBufferSubData(m_HitBuffer->GetRHIHandle(), slot.Buffer, 0, 0, bytes);

        slot.Fence = RenderCommand::CreateFence();
        if (slot.Fence == 0)
        {
            // Leave the slot FREE rather than pending. A pending slot with a
            // zero fence is either polled forever (IsFenceSignaled(0) is false)
            // or — worse, if it ever reported true — read before the copy
            // completed, which is the torn read this design exists to prevent.
            OLO_CORE_WARN("RayTracingProbe: fence creation failed; dropping this frame's trace result.");
            return false;
        }
        slot.Pending = true;
        slot.FrameIndex = m_FrameIndex;
        slot.Dispatched = batch;
        m_NextSlot = (m_NextSlot + 1u) % kRingSlots;
        return true;
    }
} // namespace OloEngine::RayTracing

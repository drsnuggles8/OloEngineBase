#include "OloEnginePCH.h"
#include "OloEngine/Terrain/TerrainGPUPicker.h"

#include "OloEngine/Math/Math.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/ShaderBindingLayout.h"
#include "OloEngine/Renderer/StorageBuffer.h"
#include "OloEngine/Terrain/TerrainGPUQuadtree.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace OloEngine
{
    namespace
    {
        // Must match TerrainRayNodeSelect.comp's local_size_x — and
        // TerrainPickArgs.comp, which is where the division actually happens.
        constexpr u32 kDescentWorkgroupSize = 64;
        static_assert(kDescentWorkgroupSize == 64,
                      "TerrainPickArgs.comp hard-codes /64 when writing the descent dispatch arguments");

        // How far the ray/AABB test inflates a node's box, in heightmap texels.
        // TWO, not one: the box has to absorb both the f32 error in the descent's
        // own bounds arithmetic and the fact that the marched surface is the
        // BILINEAR heightmap, whose value between two texels is bounded by its
        // corners but whose gradient is not — a ray grazing a node's edge must
        // land in that node's candidate set rather than only in its neighbour's.
        // Over-inclusion costs one extra marched segment and cannot change the
        // answer, since the resolve kernel takes the nearest hit across every
        // candidate.
        constexpr f32 kBoundsInflateTexels = 2.0f;
    } // namespace

    TerrainGPUPicker::TerrainGPUPicker() = default;

    TerrainGPUPicker::~TerrainGPUPicker()
    {
        // The ring's fences and buffers are raw handles, so unlike the
        // Ref<StorageBuffer> members beside them nothing frees them on its own.
        // Releasing here rather than only in Shutdown() puts them on the same
        // lifetime as those members — which already delete GPU buffers from
        // their own destructors, through the same deferred-deletion path — so a
        // picker dropped with its scene does not leak a sync object per session.
        ReleaseRing();
    }

    void TerrainGPUPicker::EnsureShaders()
    {
        if (m_ShadersLoaded || m_ShaderLoadFailed)
        {
            return;
        }

        m_DescentShader = ComputeShader::Create("assets/shaders/compute/TerrainRayNodeSelect.comp");
        m_ArgsShader = ComputeShader::Create("assets/shaders/compute/TerrainPickArgs.comp");
        m_ResolveShader = ComputeShader::Create("assets/shaders/compute/TerrainPickResolve.comp");

        const bool ok = m_DescentShader && m_DescentShader->IsValid() &&
                        m_ArgsShader && m_ArgsShader->IsValid() &&
                        m_ResolveShader && m_ResolveShader->IsValid();
        if (!ok)
        {
            // Non-fatal, same contract as TerrainGPUQuadtree: the caller keeps
            // the CPU raycast, which still produces a correct answer.
            OLO_CORE_ERROR("TerrainGPUPicker: compute shader load failed — falling back to the CPU terrain raycast");
            m_ShaderLoadFailed = true;
            return;
        }
        m_ShadersLoaded = true;
    }

    bool TerrainGPUPicker::EnsureBuffers()
    {
        if (m_StateBuffer && m_NodeListA && m_NodeListB)
        {
            return true;
        }

        using SBL = ShaderBindingLayout;
        const u32 stateBytes = kHeaderBytes + kMaxCandidates * static_cast<u32>(sizeof(u32));

        m_StateBuffer = StorageBuffer::Create(stateBytes, SBL::SSBO_TERRAIN_CULL_STATE, StorageBufferUsage::DynamicCopy);
        m_NodeListA = StorageBuffer::Create(kMaxNodeListEntries * static_cast<u32>(sizeof(u32)),
                                            SBL::SSBO_TERRAIN_NODE_LIST_IN, StorageBufferUsage::DynamicCopy);
        m_NodeListB = StorageBuffer::Create(kMaxNodeListEntries * static_cast<u32>(sizeof(u32)),
                                            SBL::SSBO_TERRAIN_NODE_LIST_OUT, StorageBufferUsage::DynamicCopy);

        if (!m_StateBuffer || !m_NodeListA || !m_NodeListB)
        {
            OLO_CORE_ERROR("TerrainGPUPicker: GPU buffer allocation failed ({} bytes of state)", stateBytes);
            m_StateBuffer = nullptr;
            m_NodeListA = nullptr;
            m_NodeListB = nullptr;
            return false;
        }

        for (auto& slot : m_Ring)
        {
            if (slot.Buffer.IsValid())
            {
                continue;
            }
            slot.Buffer = RenderCommand::CreateBufferHandle();
            RenderCommand::AllocateBufferStorage(slot.Buffer, kResultBytes, RHI::MemoryResidency::DeviceToHost);
            slot.Pending = false;
            slot.Fence = 0;
        }
        return true;
    }

    void TerrainGPUPicker::ReleaseRing()
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
        }
        m_NextSlot = 0;
        m_SlotsInFlight = 0;
    }

    void TerrainGPUPicker::Shutdown()
    {
        ReleaseRing();
        m_StateBuffer = nullptr;
        m_NodeListA = nullptr;
        m_NodeListB = nullptr;
        m_DescentShader = nullptr;
        m_ArgsShader = nullptr;
        m_ResolveShader = nullptr;
        m_ShadersLoaded = false;
        m_ShaderLoadFailed = false;
        m_HasPendingRay = false;
        m_Latest = {};
        m_FrameIndex = 0;
    }

    bool TerrainGPUPicker::SubmitRay(const RayRequest& request)
    {
        // A non-finite ray is not merely useless here, it is destructive: the
        // slab test divides by the direction, and a NaN makes every comparison
        // false, so the node test degenerates to "hit" and the descent splits
        // the entire tree. Refuse it at the door — the same reasoning that makes
        // BuildHeightPyramid skip non-finite samples.
        if (!Math::IsFinite(request.OriginLocal) || !Math::IsFinite(request.DirectionLocal))
        {
            return false;
        }
        if (!std::isfinite(request.MaxDistance) || request.MaxDistance <= 0.0f)
        {
            return false;
        }
        const f32 lengthSq = glm::dot(request.DirectionLocal, request.DirectionLocal);
        if (lengthSq < 1e-12f)
        {
            return false;
        }

        m_PendingRay = request;
        // Normalize here rather than trusting the caller: `t` is only a world
        // distance — and the CPU-side reconstruction of the hit position only
        // agrees with the GPU's march — if the direction has unit length.
        m_PendingRay.DirectionLocal = request.DirectionLocal / std::sqrt(lengthSq);
        m_HasPendingRay = true;
        return true;
    }

    void TerrainGPUPicker::Poll()
    {
        OLO_PROFILE_FUNCTION();

        // The frame counter advances HERE, not in Dispatch(). Latency is a count
        // of FRAMES between a query and its answer, and a picker only dispatches
        // on the frames something asked it to — so counting dispatches would
        // report a two-frame-old answer as zero-latency on any frame the mouse
        // did not move, which is precisely the reading that would make a
        // synchronous readback and this ring look identical.
        ++m_FrameIndex;

        u32 inFlight = 0;
        // OLDEST FIRST, not array order. `m_NextSlot` is the slot the next
        // capture will use, so it is also the oldest one still in flight, and
        // walking from there wraps through the ring in ISSUE order. Array order
        // would let a wrapped ring publish an older answer after a newer one had
        // already retired — a pick result that goes backwards in time, which
        // reads as the cursor jumping to where the mouse used to be.
        for (u32 offset = 0; offset < kRingSlots; ++offset)
        {
            RingSlot& slot = m_Ring[(m_NextSlot + offset) % kRingSlots];
            if (!slot.Pending)
            {
                continue;
            }

            // THE WHOLE POINT: ask, never wait. There is no ClientWaitFence in
            // this class, and adding one reintroduces exactly the stall the
            // issue asked to remove.
            if (!RenderCommand::IsFenceSignaled(slot.Fence))
            {
                ++inFlight;
                continue;
            }

            PickResultBlock block{};
            RenderCommand::ReadBufferSubData(slot.Buffer, 0, kResultBytes, &block);

            RenderCommand::DestroyFence(slot.Fence);
            slot.Fence = 0;
            slot.Pending = false;

            if (m_Latest.Valid && slot.FrameIndex <= m_Latest.FrameIndex)
            {
                continue;
            }

            m_Latest.Valid = true;
            m_Latest.FrameIndex = slot.FrameIndex;
            m_Latest.Latency = static_cast<u32>(m_FrameIndex - slot.FrameIndex);
            m_Latest.RayId = block.RayId;
            m_Latest.OverflowFlags = block.ResultFlags;
            // DECODE, THEN VALIDATE. Every bit pattern except kNoHitBits reads
            // as a hit, and memcpy reinterprets it with no range check — so a
            // pattern anywhere in the NaN range would decode to a NaN, and one
            // with the sign bit set to a negative distance, and either would go
            // straight into the editor's cursor and gizmo math. Today's resolve
            // kernel can only publish a finite t inside the clipped segment, so
            // this is hardening rather than a live defect; it is here because
            // the repo's rule is to validate every float crossing a boundary,
            // and a GPU buffer is one.
            f32 decoded = 0.0f;
            std::memcpy(&decoded, &block.HitTBits, sizeof(f32));
            m_Latest.Hit = block.HitTBits != kNoHitBits && std::isfinite(decoded) && decoded >= 0.0f;
            if (m_Latest.Hit)
            {
                m_Latest.Distance = decoded;
                // The position comes from the ray THIS slot was dispatched with,
                // not from whatever ray is current now. That is what keeps the
                // answer exact and what makes a late result still correct rather
                // than merely stale.
                m_Latest.PositionLocal = slot.Ray.OriginLocal + slot.Ray.DirectionLocal * m_Latest.Distance;
            }
            else
            {
                m_Latest.Distance = 0.0f;
                m_Latest.PositionLocal = glm::vec3(0.0f);
            }

            if (m_Latest.OverflowFlags != 0 && !m_OverflowWarned)
            {
                // Warn once. This costs no stall precisely because the flags ride
                // the result block through the ring — TerrainGPUQuadtree has to
                // spend a GetData() to learn the same thing, which is why it only
                // asks every 240 frames.
                m_OverflowWarned = true;
                if ((m_Latest.OverflowFlags & kOverflowNodes) != 0)
                {
                    OLO_CORE_WARN("TerrainGPUPicker: the ray worklist overflowed {} entries — the descent marched coarser nodes "
                                  "instead of splitting them. Raise kMaxNodeListEntries if picking looks imprecise.",
                                  kMaxNodeListEntries);
                }
                if ((m_Latest.OverflowFlags & kOverflowCandidates) != 0)
                {
                    OLO_CORE_WARN("TerrainGPUPicker: the candidate list overflowed {} entries — some intersected patches were "
                                  "not marched. Raise kMaxCandidates.",
                                  kMaxCandidates);
                }
                if ((m_Latest.OverflowFlags & kOverflowMarch) != 0)
                {
                    OLO_CORE_WARN("TerrainGPUPicker: the fine march ran out of samples before reaching heightmap-texel "
                                  "spacing, so a pick may be coarser than one texel or may have stepped over a thin "
                                  "crossing. This needs a tall height scale against a small world size; raise "
                                  "kMaxLaneSteps in TerrainPickResolve.comp if it is a real terrain rather than a "
                                  "degenerate one.");
                }
            }
        }
        m_SlotsInFlight = inFlight;
    }

    bool TerrainGPUPicker::Dispatch(const TerrainGPUQuadtree& tree, const TerrainInputs& terrain)
    {
        OLO_PROFILE_FUNCTION();

        if (!m_HasPendingRay)
        {
            return false;
        }
        if (!tree.IsBuilt() || !tree.GetNodeBoundsHandle().IsValid())
        {
            return false;
        }
        if (!terrain.Heightmap.IsValid() || terrain.HeightmapResolution < 2)
        {
            return false;
        }
        if (!(terrain.WorldSizeX > 0.0f) || !(terrain.WorldSizeZ > 0.0f) || !std::isfinite(terrain.HeightScale))
        {
            return false;
        }

        EnsureShaders();
        if (!m_ShadersLoaded || !EnsureBuffers())
        {
            return false;
        }

        // Consume the ray whatever happens below: a query that could not be
        // captured must not be silently re-dispatched next frame under a stale
        // mouse position.
        const RayRequest ray = m_PendingRay;
        m_HasPendingRay = false;

        const f32 texelWorld = std::max(terrain.WorldSizeX, terrain.WorldSizeZ) /
                               static_cast<f32>(terrain.HeightmapResolution - 1);

        PickStateHeader header{};
        // Level 0 has exactly one node, so the first dispatch is direct; every
        // later level's group count comes from TerrainPickArgs.comp and is read
        // back through DispatchComputeIndirect with no CPU round trip.
        header.PendingCount = 1;
        header.DescentDispatch = glm::uvec3(1u, 1u, 1u);
        header.ResolveDispatch = glm::uvec3(0u, 1u, 1u);
        header.HitTBits = kNoHitBits;
        header.RayId = ray.RayId;
        header.RayOriginAndMaxDist = glm::vec4(ray.OriginLocal, ray.MaxDistance);
        header.RayDirAndInflate = glm::vec4(ray.DirectionLocal, texelWorld * kBoundsInflateTexels);
        header.TerrainSizeAndScale = glm::vec4(terrain.WorldSizeX, terrain.WorldSizeZ, terrain.HeightScale, texelWorld);
        header.PickParams = glm::uvec4(tree.GetMaxDepth(), kMaxNodeListEntries, kMaxCandidates, terrain.HeightmapResolution);
        m_StateBuffer->SetData(&header, static_cast<u32>(sizeof(header)), 0);

        const u32 rootNode = TerrainGPUQuadtree::PackNode(0, 0, 0);
        m_NodeListA->SetData(&rootNode, static_cast<u32>(sizeof(rootNode)), 0);

        using SBL = ShaderBindingLayout;
        RenderCommand::BindStorageBuffer(SBL::SSBO_TERRAIN_CULL_STATE, m_StateBuffer->GetRHIHandle());
        RenderCommand::BindStorageBuffer(SBL::SSBO_TERRAIN_NODE_BOUNDS, tree.GetNodeBoundsHandle());

        StorageBuffer* listIn = m_NodeListA.get();
        StorageBuffer* listOut = m_NodeListB.get();

        const u32 maxDepth = tree.GetMaxDepth();
        for (u32 level = 0; level <= maxDepth; ++level)
        {
            RenderCommand::BindStorageBuffer(SBL::SSBO_TERRAIN_NODE_LIST_IN, listIn->GetRHIHandle());
            RenderCommand::BindStorageBuffer(SBL::SSBO_TERRAIN_NODE_LIST_OUT, listOut->GetRHIHandle());

            m_DescentShader->Bind();
            if (level == 0)
            {
                RenderCommand::DispatchCompute(1, 1, 1);
            }
            else
            {
                RenderCommand::DispatchComputeIndirect(m_StateBuffer->GetRHIHandle(), kDescentDispatchOffset);
            }
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage);

            m_ArgsShader->Bind();
            RenderCommand::DispatchCompute(1, 1, 1);
            // Command as well as ShaderStorage: the next iteration's
            // DispatchComputeIndirect — and the resolve dispatch below — source
            // their group counts from the buffer this kernel just wrote.
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::Command);

            std::swap(listIn, listOut);
        }

        // BIND THE PROGRAM FIRST, then the texture: HeapBinding forks on the
        // program IN FLIGHT, and called with some other program bound a bindless
        // answer would stage an offset and bind nothing — the march would then
        // sample whatever was left in slot 23.
        m_ResolveShader->Bind();
        HeapBinding::BindTextureOrOffset(SBL::TEX_TERRAIN_HEIGHTMAP, terrain.Heightmap,
                                         RHI::HeapSlotLifetime::Persistent);
        // The resolve kernel is on the heap-bindless route, so the bind above
        // STAGES an offset rather than binding a texture unit. Publishing it is
        // a separate step and it has to happen before the dispatch — an
        // unflushed offset indexes whatever the table held last, which samples a
        // stale texture rather than failing. No-op on the slot path.
        HeapBinding::FlushOffsets();
        RenderCommand::DispatchComputeIndirect(m_StateBuffer->GetRHIHandle(), kResolveDispatchOffset);

        CaptureResult(ray);
        return true;
    }

    void TerrainGPUPicker::CaptureResult(const RayRequest& ray)
    {
        RingSlot& slot = m_Ring[m_NextSlot];
        if (slot.Pending || !slot.Buffer.IsValid())
        {
            // A full ring means every slot is still executing. Skip the capture
            // and keep the newest retired answer rather than blocking on the
            // oldest — GetSlotsInFlight() is what tells a consumer this happened.
            return;
        }

        // The resolve kernel wrote the result with a SHADER atomic; the copy
        // below is a buffer-update client and needs its own barrier class.
        // ShaderStorage orders the atomics, BufferUpdate orders the copy against
        // them — both, because dropping either makes the copy read a value that
        // is right most of the time.
        RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderStorage | MemoryBarrierFlags::BufferUpdate);
        RenderCommand::CopyBufferSubData(m_StateBuffer->GetRHIHandle(), slot.Buffer, kResultOffset, 0, kResultBytes);

        slot.Fence = RenderCommand::CreateFence();
        if (slot.Fence == 0)
        {
            // Leave the slot FREE rather than pending. A pending slot with a
            // zero fence is either polled forever (IsFenceSignaled(0) is false)
            // or — worse, if it ever reported true — read before the copy
            // completed, which is the torn read this design exists to make
            // impossible.
            OLO_CORE_WARN("TerrainGPUPicker: fence creation failed; dropping this frame's pick result.");
            return;
        }
        slot.Pending = true;
        slot.FrameIndex = m_FrameIndex;
        slot.Ray = ray;
        m_NextSlot = (m_NextSlot + 1u) % kRingSlots;
    }
} // namespace OloEngine

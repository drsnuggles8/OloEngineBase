#include "OloEnginePCH.h"
#include "OloEngine/Renderer/BC6HGpuEncoder.h"

#include "OloEngine/Core/Log.h"
#include "OloEngine/Debug/Instrumentor.h"
#include "OloEngine/Renderer/ComputeShader.h"
#include "OloEngine/Renderer/HeapBindingSeam.h"
#include "OloEngine/Renderer/MemoryBarrierFlags.h"
#include "OloEngine/Renderer/RenderCommand.h"
#include "OloEngine/Renderer/Texture.h"
#include "OloEngine/Renderer/TextureCompression.h"
#include "OloEngine/Task/NamedThreads.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace OloEngine::BC6HGpu
{
    namespace
    {
        // Must match local_size_x/y in BC6HEncode.comp.
        constexpr u32 kLocalSize = 8;

        // How long a marshalled job waits for the context thread to run it. Generous
        // against a stalled frame, and paid at most ONCE per process: a timeout latches
        // the whole GPU path off, so the rest of a bake goes straight to the CPU
        // encoder instead of stalling per level.
        constexpr auto kJobTimeout = std::chrono::seconds(5);

        // One marshalled encode. Owns its input, so a waiter that gives up cannot leave
        // the context thread holding references into a dead stack frame.
        struct EncodeJob
        {
            std::vector<f32> Rgb;
            u32 Width = 0;
            u32 Height = 0;
            bool Signed = false;
            std::vector<u8> Blocks;
            bool Done = false;
            bool Ok = false;
            std::mutex Mutex;
            std::condition_variable Ready;
        };

        struct GpuEncoderState
        {
            Ref<ComputeShader> UnsignedShader;
            Ref<ComputeShader> SignedShader;
            Ref<Texture2D> SourceTexture; // RGBA32F, reused across levels
            Ref<Texture2D> BlocksTexture; // RGBA32UI, one texel per block
            bool Initialized = false;     // context thread only

            // ---- Shared across threads --------------------------------------------
            // A cook worker reads all of these and writes the two flags; the context
            // thread writes ContextThread and reads Unavailable. So each is either
            // atomic or taken under QueueMutex. As plain bools they were a data race,
            // and the latch was unreliable with it: a worker that failed to observe
            // Unavailable would pay the full timeout again on the next mip level —
            // exactly the cost the latch exists to prevent.
            //
            // Latched, not retried: a context without compute (or a missing shader
            // file) will not start working between mip levels.
            std::atomic<bool> HasContextThread{ false };
            std::atomic<bool> Unavailable{ false };
            std::atomic<bool> WarnedOffThread{ false };
            std::mutex QueueMutex;
            std::thread::id ContextThread{}; // guarded by QueueMutex
            std::deque<std::shared_ptr<EncodeJob>> Queue;
        };

        // True when the caller owns the graphics context. Reads ContextThread under
        // the mutex; the HasContextThread check short-circuits the common
        // nothing-registered case without taking it.
        bool CallerIsContextThread(GpuEncoderState& state)
        {
            if (!state.HasContextThread.load(std::memory_order_acquire))
                return false;
            std::lock_guard<std::mutex> lock(state.QueueMutex);
            return state.ContextThread == std::this_thread::get_id();
        }

        GpuEncoderState& State()
        {
            static GpuEncoderState s_State;
            return s_State;
        }

        bool EnsureShaders()
        {
            GpuEncoderState& state = State();
            if (state.Unavailable.load(std::memory_order_acquire))
                return false;
            if (state.Initialized)
                return true;

            state.UnsignedShader = ComputeShader::Create("assets/shaders/compute/BC6HEncode.comp");
            state.SignedShader = ComputeShader::Create("assets/shaders/compute/BC6HEncodeSigned.comp");
            if (!state.UnsignedShader || !state.UnsignedShader->IsValid() ||
                !state.SignedShader || !state.SignedShader->IsValid())
            {
                OLO_CORE_WARN("BC6HGpu: compute shaders unavailable — BC6H cooks stay on the CPU encoder");
                state.UnsignedShader.Reset();
                state.SignedShader.Reset();
                state.Unavailable.store(true, std::memory_order_release);
                return false;
            }
            state.Initialized = true;
            return true;
        }

        // Scratch textures sized exactly to the level: the shader takes both the block
        // grid and the source extent from imageSize(), which is what lets it run with no
        // parameter uniform at all, so a reused oversized texture would encode the wrong
        // region. Recreating drops the old view — and with it its heap slot.
        bool EnsureTextures(u32 width, u32 height, u32 blocksX, u32 blocksY)
        {
            GpuEncoderState& state = State();
            const bool needSource = !state.SourceTexture || state.SourceTexture->GetWidth() != width ||
                                    state.SourceTexture->GetHeight() != height;
            if (needSource)
            {
                TextureSpecification spec;
                spec.Width = width;
                spec.Height = height;
                spec.Format = ImageFormat::RGBA32F;
                spec.GenerateMips = false;
                state.SourceTexture = Texture2D::Create(spec);
            }
            const bool needBlocks = !state.BlocksTexture || state.BlocksTexture->GetWidth() != blocksX ||
                                    state.BlocksTexture->GetHeight() != blocksY;
            if (needBlocks)
            {
                TextureSpecification spec;
                spec.Width = blocksX;
                spec.Height = blocksY;
                spec.Format = ImageFormat::RGBA32UI;
                spec.GenerateMips = false;
                state.BlocksTexture = Texture2D::Create(spec);
            }

            if (!state.SourceTexture || !state.SourceTexture->IsLoaded() ||
                !state.BlocksTexture || !state.BlocksTexture->IsLoaded())
            {
                OLO_CORE_ERROR("BC6HGpu: could not create the {}x{} scratch textures", width, height);
                state.SourceTexture.Reset();
                state.BlocksTexture.Reset();
                return false;
            }
            return true;
        }
    } // namespace

    bool IsAvailable()
    {
        return EnsureShaders();
    }

    namespace
    {
        // The GL half. Only ever runs on the context thread — either called straight
        // through by EncodeLevel, or by PumpPendingJobs for a job another thread queued.
        bool EncodeOnContextThread(const f32* rgb, u32 width, u32 height, bool isSigned,
                                   std::vector<u8>& outBlocks)
        {
            OLO_PROFILE_FUNCTION();

            if (!EnsureShaders())
                return false;

            const u32 blocksX = TextureCompression::BlockCount(width);
            const u32 blocksY = TextureCompression::BlockCount(height);
            if (!EnsureTextures(width, height, blocksX, blocksY))
                return false;

            GpuEncoderState& state = State();

            // Upload the level as RGBA32F. The shader re-derives the half bit pattern with
            // packHalf2x16, exactly as the CPU encoder does with glm::packHalf1x16, so the
            // extra alpha channel is the only thing this widening costs.
            std::vector<f32> rgba(static_cast<sizet>(width) * height * 4);
            for (sizet texel = 0; texel < static_cast<sizet>(width) * height; ++texel)
            {
                rgba[texel * 4 + 0] = rgb[texel * 3 + 0];
                rgba[texel * 4 + 1] = rgb[texel * 3 + 1];
                rgba[texel * 4 + 2] = rgb[texel * 3 + 2];
                rgba[texel * 4 + 3] = 1.0f;
            }
            state.SourceTexture->SetData(rgba.data(), static_cast<u32>(rgba.size() * sizeof(f32)));

            // The shader is bound FIRST because the binding seam forks on
            // Shader::IsBoundProgramBindless(), which describes the program IN FLIGHT — a
            // bind issued before it silently takes the slot-path fallback even with the
            // heap on, and the bindless shader then reads descriptors nobody wrote. Same
            // rule and same reason as SSRRenderPass and GPUFrustumCuller.
            const Ref<ComputeShader>& shader = isSigned ? state.SignedShader : state.UnsignedShader;
            shader->Bind();

            // Persistent, not FrameTransient: a cook is not a frame, so the per-frame ring
            // that FrameTransient draws from would never reset between levels. A persistent
            // slot is freed when its view is destroyed, and these scratch textures ARE
            // destroyed whenever a level changes size — which a mip chain does every level —
            // so the slots are released as the chain walks down rather than accumulating.
            HeapBinding::BindImageOrOffset(0, state.SourceTexture->GetRHIHandle(), 0, false, 0,
                                           RHI::Access::StorageRead, RHI::Format::RGBA32Float,
                                           RHI::HeapSlotLifetime::Persistent);
            HeapBinding::BindImageOrOffset(1, state.BlocksTexture->GetRHIHandle(), 0, false, 0,
                                           RHI::Access::StorageWrite, RHI::Format::RGBA32UInt,
                                           RHI::HeapSlotLifetime::Persistent);
            HeapBinding::FlushOffsets();

            RenderCommand::DispatchCompute((blocksX + kLocalSize - 1) / kLocalSize,
                                           (blocksY + kLocalSize - 1) / kLocalSize, 1);

            // The image stores must land before the readback reads them; TextureUpdate and
            // PixelBuffer cover glGetTextureImage's route out.
            RenderCommand::MemoryBarrier(MemoryBarrierFlags::ShaderImageAccess | MemoryBarrierFlags::TextureUpdate |
                                         MemoryBarrierFlags::PixelBuffer | MemoryBarrierFlags::TextureFetch);

            outBlocks.assign(static_cast<sizet>(blocksX) * blocksY * 16, 0);
            if (!RenderCommand::ReadTextureImage(state.BlocksTexture->GetRHIHandle(), 0, RHI::Format::RGBA32UInt,
                                                 outBlocks.size(), outBlocks.data()))
            {
                OLO_CORE_ERROR("BC6HGpu::EncodeLevel - readback of the {}x{} block image failed", blocksX, blocksY);
                outBlocks.clear();
                return false;
            }
            return true;
        }
    } // namespace

    u32 PumpPendingJobs()
    {
        GpuEncoderState& state = State();
        if (!CallerIsContextThread(state))
            return 0;

        u32 ran = 0;
        while (true)
        {
            std::shared_ptr<EncodeJob> job;
            {
                std::lock_guard<std::mutex> lock(state.QueueMutex);
                if (state.Queue.empty())
                    break;
                job = state.Queue.front();
                state.Queue.pop_front();
            }

            std::vector<u8> blocks;
            const bool ok = EncodeOnContextThread(job->Rgb.data(), job->Width, job->Height, job->Signed, blocks);
            {
                std::lock_guard<std::mutex> lock(job->Mutex);
                job->Blocks = std::move(blocks);
                job->Ok = ok;
                job->Done = true;
            }
            job->Ready.notify_all();
            ++ran;
        }
        return ran;
    }

    void SetContextThreadToCurrent()
    {
        GpuEncoderState& state = State();
        {
            std::lock_guard<std::mutex> lock(state.QueueMutex);
            state.ContextThread = std::this_thread::get_id();
        }
        state.HasContextThread.store(true, std::memory_order_release);
        state.WarnedOffThread.store(false, std::memory_order_relaxed);
    }

    bool EncodeLevel(const f32* rgb, u32 width, u32 height, bool isSigned, std::vector<u8>& outBlocks)
    {
        if (!rgb || width == 0 || height == 0)
        {
            OLO_CORE_ERROR("BC6HGpu::EncodeLevel - invalid input ({}x{})", width, height);
            return false;
        }

        GpuEncoderState& state = State();
        if (CallerIsContextThread(state))
            return EncodeOnContextThread(rgb, width, height, isSigned, outBlocks);

        if (!state.HasContextThread.load(std::memory_order_acquire) ||
            state.Unavailable.load(std::memory_order_acquire))
        {
            // Nowhere to marshal to. One warning, then quiet: the caller falls back to
            // the CPU encoder and TextureCompression counts the level.
            if (!state.WarnedOffThread.exchange(true, std::memory_order_acq_rel))
            {
                OLO_CORE_WARN("BC6HGpu::EncodeLevel called with no context thread available - "
                              "BC6H stays on the CPU encoder for this run (further calls not logged)");
            }
            return false;
        }

        // Marshal. The job OWNS its input, so a waiter that times out cannot leave the
        // context thread reading a dead stack frame.
        auto job = std::make_shared<EncodeJob>();
        job->Rgb.assign(rgb, rgb + static_cast<sizet>(width) * height * 3);
        job->Width = width;
        job->Height = height;
        job->Signed = isSigned;
        {
            std::lock_guard<std::mutex> lock(state.QueueMutex);
            state.Queue.push_back(job);
        }

        // Ask the game thread to drain the queue. Application::Run already calls
        // ProcessTasks at the top of every frame, so this needs no pump of its own. A host
        // with no game thread (the test binary) drives PumpPendingJobs itself; the bounded
        // wait below is what keeps either case from hanging.
        auto& threads = Tasks::FNamedThreadManager::Get();
        if (threads.IsThreadAttached(Tasks::ENamedThread::GameThread))
        {
            threads.EnqueueTask(Tasks::ENamedThread::GameThread,
                                Tasks::FNamedThreadTask([]()
                                                        { (void)PumpPendingJobs(); },
                                                        Tasks::EExtendedTaskPriority::GameThreadNormalPri,
                                                        "BC6HGpuEncode"));
        }

        std::unique_lock<std::mutex> lock(job->Mutex);
        if (!job->Ready.wait_for(lock, kJobTimeout, [&job]
                                 { return job->Done; }))
        {
            lock.unlock();

            // Drop the abandoned job. Nobody will consume its result, so leaving it for a
            // context thread that wakes up later would spend a full GPU encode — upload,
            // dispatch and readback — producing blocks the caller already re-encoded on
            // the CPU. If the pump took it in the meantime, it is gone from the queue and
            // there is nothing to erase.
            {
                std::lock_guard<std::mutex> queueLock(state.QueueMutex);
                const auto it = std::find(state.Queue.begin(), state.Queue.end(), job);
                if (it != state.Queue.end())
                    state.Queue.erase(it);
            }

            // Latch the whole path off: whatever should have drained the queue is not
            // doing it, and paying five seconds per mip level to rediscover that would be
            // worse than any speed-up the GPU could have given.
            state.Unavailable.store(true, std::memory_order_release);
            OLO_CORE_WARN("BC6HGpu::EncodeLevel - no context thread drained the queue within {} s; "
                          "BC6H falls back to the CPU encoder for the rest of this run",
                          kJobTimeout.count());
            return false;
        }
        if (!job->Ok)
            return false;
        outBlocks = std::move(job->Blocks);
        return true;
    }

    void Shutdown()
    {
        GpuEncoderState& state = State();

        // Fail anything still queued before the resources go: a worker blocked inside
        // EncodeLevel would otherwise sit out the full timeout during teardown.
        std::deque<std::shared_ptr<EncodeJob>> pending;
        {
            std::lock_guard<std::mutex> lock(state.QueueMutex);
            pending.swap(state.Queue);
        }
        for (auto& job : pending)
        {
            {
                std::lock_guard<std::mutex> lock(job->Mutex);
                job->Ok = false;
                job->Done = true;
            }
            job->Ready.notify_all();
        }

        state.UnsignedShader.Reset();
        state.SignedShader.Reset();
        state.SourceTexture.Reset();
        state.BlocksTexture.Reset();
        state.Initialized = false;
        state.HasContextThread.store(false, std::memory_order_release);
        // Deliberately NOT clearing Unavailable: if the shaders could not load once in
        // this process they will not load later, and re-arming would re-log per level.
    }

    void RegisterWithTextureCompression()
    {
        // Deliberately thread-agnostic: the cook that registers this runs on a worker,
        // and the context thread is recorded by SetContextThreadToCurrent instead.
        TextureCompression::SetGpuBC6HEncoder(&EncodeLevel);
    }

    void UnregisterFromTextureCompression()
    {
        TextureCompression::SetGpuBC6HEncoder(nullptr);
    }
} // namespace OloEngine::BC6HGpu

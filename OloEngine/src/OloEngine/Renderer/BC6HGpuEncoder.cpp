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

#include <algorithm>
#include <thread>
#include <vector>

namespace OloEngine::BC6HGpu
{
    namespace
    {
        // Must match local_size_x/y in BC6HEncode.comp.
        constexpr u32 kLocalSize = 8;

        struct GpuEncoderState
        {
            Ref<ComputeShader> UnsignedShader;
            Ref<ComputeShader> SignedShader;
            Ref<Texture2D> SourceTexture; // RGBA32F, reused across levels
            Ref<Texture2D> BlocksTexture; // RGBA32UI, one texel per block
            // The thread that registered the hook — the one holding the graphics
            // context. Every GL call below must happen there.
            std::thread::id OwningThread{};
            bool HasOwningThread = false;
            bool WarnedOffThread = false;
            bool Initialized = false;
            // Latched: a context without compute (or a missing shader file) is not
            // going to start working between mip levels, and retrying would log the
            // same failure once per level of every texture in the bake.
            bool Unavailable = false;
        };

        GpuEncoderState& State()
        {
            static GpuEncoderState s_State;
            return s_State;
        }

        bool EnsureShaders()
        {
            GpuEncoderState& state = State();
            if (state.Unavailable)
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
                state.Unavailable = true;
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

    bool EncodeLevel(const f32* rgb, u32 width, u32 height, bool isSigned, std::vector<u8>& outBlocks)
    {
        OLO_PROFILE_FUNCTION();

        if (!rgb || width == 0 || height == 0)
        {
            OLO_CORE_ERROR("BC6HGpu::EncodeLevel - invalid input ({}x{})", width, height);
            return false;
        }
        GpuEncoderState& threadCheck = State();
        if (threadCheck.HasOwningThread && threadCheck.OwningThread != std::this_thread::get_id())
        {
            // Every call below is a GL call, and a graphics context belongs to one
            // thread. Refusing here turns "cook on a worker" into a counted CPU
            // fallback with one warning, instead of undefined driver behaviour.
            if (!threadCheck.WarnedOffThread)
            {
                threadCheck.WarnedOffThread = true;
                OLO_CORE_WARN("BC6HGpu::EncodeLevel called off the thread that registered it — "
                              "BC6H stays on the CPU encoder for this run (further calls not logged)");
            }
            return false;
        }
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

    void Shutdown()
    {
        GpuEncoderState& state = State();
        state.UnsignedShader.Reset();
        state.SignedShader.Reset();
        state.SourceTexture.Reset();
        state.BlocksTexture.Reset();
        state.Initialized = false;
        // Deliberately NOT clearing Unavailable: if the shaders could not load once in
        // this process they will not load later, and re-arming would re-log per level.
    }

    void RegisterWithTextureCompression()
    {
        GpuEncoderState& state = State();
        state.OwningThread = std::this_thread::get_id();
        state.HasOwningThread = true;
        state.WarnedOffThread = false;
        TextureCompression::SetGpuBC6HEncoder(&EncodeLevel);
    }

    void UnregisterFromTextureCompression()
    {
        TextureCompression::SetGpuBC6HEncoder(nullptr);
        State().HasOwningThread = false;
    }
} // namespace OloEngine::BC6HGpu

#pragma once

#include "OloEngine/Core/Ref.h"
#include "OloEngine/Renderer/RGPreparedPass.h"
#include "OloEngine/Renderer/RHI/RHIDescriptorHeap.h"
#include "OloEngine/Renderer/Framebuffer.h"
#include "OloEngine/Renderer/Shader.h"
#include "OloEngine/Renderer/UniformBuffer.h"

#include <string_view>

namespace OloEngine
{
    struct FullscreenTextureInput
    {
        u32 Slot = 0;
        RHI::ResourceHandle Texture;
        std::string_view SamplerUniform;
        RHI::HeapSlotLifetime Lifetime = RHI::HeapSlotLifetime::FrameTransient;
    };

    // Prepare the shared one-target fullscreen operation. Resource lookup and
    // parameter upload belong to the caller; the returned operation owns its
    // references and records only bindings, one clear and one triangle draw.
    [[nodiscard]] RGPreparedPass PrepareFullscreenPass(Ref<Framebuffer> target, Ref<Shader> shader,
                                                       std::vector<FullscreenTextureInput> textures,
                                                       std::vector<Ref<UniformBuffer>> uniforms,
                                                       bool clear = true, std::vector<u32> drawBuffers = { 0u },
                                                       glm::vec4 clearColor = { 0, 0, 0, 1 });
} // namespace OloEngine

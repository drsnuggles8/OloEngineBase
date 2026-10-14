#pragma once

#include "OloEngine/Core/Base.h"
#include <glad/gl.h>
#include <array>
#include <vector>

namespace OloEngine::Tests::GroomShellSpike
{
    struct ExtraState
    {
        const std::array<u32, 4> Enables{ GL_FRAMEBUFFER_SRGB, GL_SAMPLE_ALPHA_TO_COVERAGE, GL_MULTISAMPLE, GL_SAMPLE_MASK };
        std::array<GLboolean, 4> Enabled{};
        std::array<GLboolean, 4> ColorMask{};
        const std::array<u32, 10> PixelNames{ GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_PIXELS, GL_PACK_SKIP_ROWS,
                                              GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_PIXELS, GL_UNPACK_SKIP_ROWS,
                                              GL_UNPACK_IMAGE_HEIGHT, GL_UNPACK_SKIP_IMAGES };
        std::array<GLint, 10> PixelValues{};
        std::array<GLint, 2> Texture2D{};
        std::array<GLint, 2> Texture3D{};
        std::array<GLint, 2> Samplers{};
        GLint UniformSlot = 0;
        GLint UniformGeneric = 0;
        GLint64 UniformStart = 0;
        GLint64 UniformSize = 0;
        GLint PackBuffer = 0;
        GLint UnpackBuffer = 0;
        GLint ActiveTexture = 0;
        struct BlendSlot
        {
            GLboolean Enabled;
            std::array<GLint, 6> Values;
            std::array<GLboolean, 4> Mask;
        };
        const std::array<u32, 6> BlendNames{ GL_BLEND_SRC_RGB, GL_BLEND_DST_RGB, GL_BLEND_SRC_ALPHA, GL_BLEND_DST_ALPHA, GL_BLEND_EQUATION_RGB, GL_BLEND_EQUATION_ALPHA };
        std::vector<BlendSlot> BlendSlots;
        std::vector<GLboolean> ScissorSlots;
        ExtraState()
        {
            GLint drawBuffers = 0;
            GLint viewports = 0;
            glGetIntegerv(GL_MAX_DRAW_BUFFERS, &drawBuffers);
            glGetIntegerv(GL_MAX_VIEWPORTS, &viewports);
            BlendSlots.resize(static_cast<sizet>(drawBuffers));
            ScissorSlots.resize(static_cast<sizet>(viewports));
            for (u32 slot = 0; slot < BlendSlots.size(); ++slot)
            {
                auto& state = BlendSlots[slot];
                state.Enabled = glIsEnabledi(GL_BLEND, slot);
                glGetBooleani_v(GL_COLOR_WRITEMASK, slot, state.Mask.data());
                for (sizet field = 0; field < BlendNames.size(); ++field)
                {
                    glGetIntegeri_v(BlendNames[field], slot, &state.Values[field]);
                }
            }
            for (u32 slot = 0; slot < ScissorSlots.size(); ++slot)
            {
                ScissorSlots[slot] = glIsEnabledi(GL_SCISSOR_TEST, slot);
            }
            for (sizet i = 0; i < Enables.size(); ++i)
            {
                Enabled[i] = glIsEnabled(Enables[i]);
            }
            for (sizet i = 0; i < PixelNames.size(); ++i)
            {
                glGetIntegerv(PixelNames[i], &PixelValues[i]);
            }
            glGetBooleanv(GL_COLOR_WRITEMASK, ColorMask.data());
            glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, 30, &UniformSlot);
            glGetInteger64i_v(GL_UNIFORM_BUFFER_START, 30, &UniformStart);
            glGetInteger64i_v(GL_UNIFORM_BUFFER_SIZE, 30, &UniformSize);
            glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &UniformGeneric);
            glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &PackBuffer);
            glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &UnpackBuffer);
            glGetIntegerv(GL_ACTIVE_TEXTURE, &ActiveTexture);
            for (u32 i = 0; i < 2; ++i)
            {
                glActiveTexture(GL_TEXTURE0 + i);
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &Texture2D[i]);
                glGetIntegerv(GL_TEXTURE_BINDING_3D, &Texture3D[i]);
                glGetIntegeri_v(GL_SAMPLER_BINDING, i, &Samplers[i]);
                glBindSampler(i, 0);
            }
            glActiveTexture(static_cast<u32>(ActiveTexture));
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            for (u32 name : PixelNames)
            {
                glPixelStorei(name, name == GL_PACK_ALIGNMENT || name == GL_UNPACK_ALIGNMENT ? 1 : 0);
            }
        }
        ~ExtraState()
        {
            // GLStateGuard's global blend restore writes every indexed
            // slot. Restore those independent slots after the core guard.
            for (u32 slot = 0; slot < BlendSlots.size(); ++slot)
            {
                const auto& state = BlendSlots[slot];
                if (state.Enabled)
                    glEnablei(GL_BLEND, slot);
                else
                    glDisablei(GL_BLEND, slot);
                glBlendFuncSeparatei(slot, state.Values[0], state.Values[1], state.Values[2], state.Values[3]);
                glBlendEquationSeparatei(slot, state.Values[4], state.Values[5]);
                glColorMaski(slot, state.Mask[0], state.Mask[1], state.Mask[2], state.Mask[3]);
            }
            for (u32 slot = 0; slot < ScissorSlots.size(); ++slot)
            {
                if (ScissorSlots[slot])
                    glEnablei(GL_SCISSOR_TEST, slot);
                else
                    glDisablei(GL_SCISSOR_TEST, slot);
            }
            for (sizet i = 0; i < Enables.size(); ++i)
            {
                if (Enabled[i])
                    glEnable(Enables[i]);
                else
                    glDisable(Enables[i]);
            }
            for (sizet i = 0; i < PixelNames.size(); ++i)
            {
                glPixelStorei(PixelNames[i], PixelValues[i]);
            }
            glColorMaski(0, ColorMask[0], ColorMask[1], ColorMask[2], ColorMask[3]);
            if (UniformSlot != 0 && UniformSize > 0)
            {
                glBindBufferRange(GL_UNIFORM_BUFFER, 30, static_cast<u32>(UniformSlot), static_cast<GLintptr>(UniformStart), static_cast<GLsizeiptr>(UniformSize));
            }
            else
            {
                glBindBufferBase(GL_UNIFORM_BUFFER, 30, static_cast<u32>(UniformSlot));
            }
            glBindBuffer(GL_UNIFORM_BUFFER, static_cast<u32>(UniformGeneric));
            glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<u32>(PackBuffer));
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<u32>(UnpackBuffer));
            for (u32 i = 0; i < 2; ++i)
            {
                glActiveTexture(GL_TEXTURE0 + i);
                glBindTexture(GL_TEXTURE_2D, static_cast<u32>(Texture2D[i]));
                glBindTexture(GL_TEXTURE_3D, static_cast<u32>(Texture3D[i]));
                glBindSampler(i, static_cast<u32>(Samplers[i]));
            }
            glActiveTexture(static_cast<u32>(ActiveTexture));
        }
    };
} // namespace OloEngine::Tests::GroomShellSpike

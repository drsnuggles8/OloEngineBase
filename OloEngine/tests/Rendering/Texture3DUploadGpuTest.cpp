#include "OloEnginePCH.h"

// OLO_TEST_LAYER: plumbing
// =============================================================================
// Texture3DUploadGpuTest — a client upload into a volume lands as the texels it
// describes.
//
// RGBA16F TAKES HALF-FLOAT CLIENT DATA, eight bytes a texel, on both backends.
// Before #1445 the GL backend's size check asked for eight bytes a texel while
// the upload told the driver the data was GL_FLOAT, so the only buffer the
// check accepted was read as sixteen bytes a texel: half the volume from the
// buffer and the rest from past its end. Nothing hit it, because every RGBA16F
// volume in the engine was written by a compute image store -- until the groom
// coat's self-shadow volume wanted to upload one from the CPU.
// =============================================================================

#include "PropertyTests/RenderPropertyTest.h"

#include <gtest/gtest.h>

#include "OloEngine/Renderer/Texture3D.h"

#include <glad/gl.h>
#include <glm/gtc/packing.hpp>

#include <vector>

namespace OloEngine::Tests
{
    TEST(Texture3DUploadGpu, AnRgba16fUploadIsHalfFloatDataAndReadsBackAsWritten)
    {
        OLO_ENSURE_GPU_OR_SKIP();

        Texture3DSpecification spec;
        spec.Width = 2u;
        spec.Height = 3u;
        spec.Depth = 4u;
        spec.Format = Texture3DFormat::RGBA16F;
        spec.Repeat = false;
        Ref<Texture3D> volume = Texture3D::Create(spec);
        ASSERT_TRUE(volume) << "Texture3D::Create returned null";

        // Quarter steps are exact in a half float, so the readback can be
        // compared without a tolerance that would also admit a wrong texel.
        const u32 channels = spec.Width * spec.Height * spec.Depth * 4u;
        std::vector<f32> expected(channels);
        std::vector<u16> halves(channels);
        for (u32 i = 0; i < channels; ++i)
        {
            expected[i] = static_cast<f32>(i) * 0.25f - 11.0f;
            halves[i] = glm::packHalf1x16(expected[i]);
        }
        volume->SetData(halves.data(), static_cast<u32>(halves.size() * sizeof(u16)));

        std::vector<f32> readBack(channels, -1.0f);
        glGetTextureImage(volume->GetRendererID(), 0, GL_RGBA, GL_FLOAT,
                          static_cast<GLsizei>(readBack.size() * sizeof(f32)), readBack.data());
        ASSERT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
        for (u32 i = 0; i < channels; ++i)
        {
            ASSERT_FLOAT_EQ(readBack[i], expected[i]) << "channel " << i << " of texel " << i / 4u;
        }

        // A FLOAT buffer is the wrong size for this format and is refused
        // whole, leaving the volume as it was, rather than half-read.
        const std::vector<f32> floats(channels, 99.0f);
        volume->SetData(floats.data(), static_cast<u32>(floats.size() * sizeof(f32)));
        glGetTextureImage(volume->GetRendererID(), 0, GL_RGBA, GL_FLOAT,
                          static_cast<GLsizei>(readBack.size() * sizeof(f32)), readBack.data());
        for (u32 i = 0; i < channels; ++i)
        {
            ASSERT_FLOAT_EQ(readBack[i], expected[i]) << "a refused upload changed channel " << i;
        }
    }
} // namespace OloEngine::Tests

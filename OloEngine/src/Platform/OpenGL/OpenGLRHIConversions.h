#pragma once

// =============================================================================
// OpenGLRHIConversions.h — the ONE place RHI:: enums become GLenum.
//
// Issue #691 Phase 2, ADR 0011 §1.7. RendererAPI's virtuals used to be spelled
// in GLenum, which handed the whole OpenGL API to every translation unit that
// included RendererAPI.h and let 40 files include <glad/gl.h> while calling no
// GL at all — purely to name the GL_* constants those signatures demanded.
//
// The engine now speaks RHI:: enums; this header is where the OpenGL backend —
// and ONLY the OpenGL backend — lowers them. Deliberately NOT bit-equal
// mappings: RHITypes.h's enums are engine enums whose values are chosen for
// readability, so every conversion is an explicit switch. Making them alias GL
// values "for a free static_cast" is exactly how GL leaked upward the first
// time.
//
// Every switch logs and falls back rather than silently emitting 0 (which GL
// would take as GL_NONE / GL_ZERO / GL_POINTS depending on where it landed — a
// wrong-but-legal value, the silent failure mode this phase exists to avoid).
//
// WHERE that fallback lives is load-bearing, so do not "tidy" it. Sixteen of the
// nineteen switches below put it AFTER the switch and carry no `default:` label
// at all. That is deliberate: without a `default:`, the compiler's
// switch-exhaustiveness warning (clang/clang-cl `-Wswitch`, on by default) fires
// when someone APPENDS an enumerator to one of these enums — and that warning is
// the only thing that catches an append, because RHIEnumLoweringTest's
// last-enumerator `static_assert` cannot (appending leaves the previous last
// member's ordinal unchanged). Adding a `default:` here to "be safe" would trade
// a build error for a silent wrong mapping. Note MSVC's equivalent (C4062) is
// off by default even at /W4, so the clang-cl CI job is what enforces this.
//
// The three exceptions are intentional: ToGLPixelFormat / ToGLPixelType take
// RHI::Format, and ToGLImageAccess takes RHI::Access — enums whose members are
// mostly NOT valid for those particular conversions, so an exhaustive list would
// be noise rather than a guard.
// =============================================================================

#include "OloEngine/Core/Log.h"
#include "OloEngine/Renderer/RHI/RHITypes.h"

#include <glad/gl.h>

namespace OloEngine::Utils
{
    [[nodiscard]] inline GLenum ToGL(RHI::CompareOp op)
    {
        switch (op)
        {
            case RHI::CompareOp::Never:
                return GL_NEVER;
            case RHI::CompareOp::Less:
                return GL_LESS;
            case RHI::CompareOp::Equal:
                return GL_EQUAL;
            case RHI::CompareOp::LessOrEqual:
                return GL_LEQUAL;
            case RHI::CompareOp::Greater:
                return GL_GREATER;
            case RHI::CompareOp::NotEqual:
                return GL_NOTEQUAL;
            case RHI::CompareOp::GreaterOrEqual:
                return GL_GEQUAL;
            case RHI::CompareOp::Always:
                return GL_ALWAYS;
        }
        OLO_CORE_ERROR("ToGL(RHI::CompareOp): unhandled value {}", static_cast<int>(op));
        return GL_LESS;
    }

    [[nodiscard]] inline GLenum ToGL(RHI::BlendFactor factor)
    {
        switch (factor)
        {
            case RHI::BlendFactor::Zero:
                return GL_ZERO;
            case RHI::BlendFactor::One:
                return GL_ONE;
            case RHI::BlendFactor::SrcColor:
                return GL_SRC_COLOR;
            case RHI::BlendFactor::OneMinusSrcColor:
                return GL_ONE_MINUS_SRC_COLOR;
            case RHI::BlendFactor::DstColor:
                return GL_DST_COLOR;
            case RHI::BlendFactor::OneMinusDstColor:
                return GL_ONE_MINUS_DST_COLOR;
            case RHI::BlendFactor::SrcAlpha:
                return GL_SRC_ALPHA;
            case RHI::BlendFactor::OneMinusSrcAlpha:
                return GL_ONE_MINUS_SRC_ALPHA;
            case RHI::BlendFactor::DstAlpha:
                return GL_DST_ALPHA;
            case RHI::BlendFactor::OneMinusDstAlpha:
                return GL_ONE_MINUS_DST_ALPHA;
            case RHI::BlendFactor::ConstantColor:
                return GL_CONSTANT_COLOR;
            case RHI::BlendFactor::OneMinusConstantColor:
                return GL_ONE_MINUS_CONSTANT_COLOR;
            case RHI::BlendFactor::SrcAlphaSaturate:
                return GL_SRC_ALPHA_SATURATE;
        }
        OLO_CORE_ERROR("ToGL(RHI::BlendFactor): unhandled value {}", static_cast<int>(factor));
        return GL_ONE;
    }

    [[nodiscard]] inline GLenum ToGL(RHI::BlendOp op)
    {
        switch (op)
        {
            case RHI::BlendOp::Add:
                return GL_FUNC_ADD;
            case RHI::BlendOp::Subtract:
                return GL_FUNC_SUBTRACT;
            case RHI::BlendOp::ReverseSubtract:
                return GL_FUNC_REVERSE_SUBTRACT;
            case RHI::BlendOp::Min:
                return GL_MIN;
            case RHI::BlendOp::Max:
                return GL_MAX;
        }
        OLO_CORE_ERROR("ToGL(RHI::BlendOp): unhandled value {}", static_cast<int>(op));
        return GL_FUNC_ADD;
    }

    [[nodiscard]] inline GLenum ToGL(RHI::StencilOp op)
    {
        switch (op)
        {
            case RHI::StencilOp::Keep:
                return GL_KEEP;
            case RHI::StencilOp::Zero:
                return GL_ZERO;
            case RHI::StencilOp::Replace:
                return GL_REPLACE;
            case RHI::StencilOp::IncrementClamp:
                return GL_INCR;
            case RHI::StencilOp::DecrementClamp:
                return GL_DECR;
            case RHI::StencilOp::Invert:
                return GL_INVERT;
            case RHI::StencilOp::IncrementWrap:
                return GL_INCR_WRAP;
            case RHI::StencilOp::DecrementWrap:
                return GL_DECR_WRAP;
        }
        OLO_CORE_ERROR("ToGL(RHI::StencilOp): unhandled value {}", static_cast<int>(op));
        return GL_KEEP;
    }

    // RHI::CullMode::None has no glCullFace() spelling — culling-off is
    // glDisable(GL_CULL_FACE), a different call. Callers that can receive None
    // must branch before converting; this returns GL_BACK so a missed branch
    // degrades to the engine's default rather than to an invalid enum.
    [[nodiscard]] inline GLenum ToGL(RHI::CullMode mode)
    {
        switch (mode)
        {
            case RHI::CullMode::Front:
                return GL_FRONT;
            case RHI::CullMode::Back:
                return GL_BACK;
            case RHI::CullMode::FrontAndBack:
                return GL_FRONT_AND_BACK;
            case RHI::CullMode::None:
                OLO_CORE_ERROR("ToGL(RHI::CullMode): None is glDisable(GL_CULL_FACE), not a face enum");
                return GL_BACK;
        }
        OLO_CORE_ERROR("ToGL(RHI::CullMode): unhandled value {}", static_cast<int>(mode));
        return GL_BACK;
    }

    [[nodiscard]] inline GLenum ToGL(RHI::PolygonMode mode)
    {
        switch (mode)
        {
            case RHI::PolygonMode::Fill:
                return GL_FILL;
            case RHI::PolygonMode::Line:
                return GL_LINE;
            case RHI::PolygonMode::Point:
                return GL_POINT;
        }
        OLO_CORE_ERROR("ToGL(RHI::PolygonMode): unhandled value {}", static_cast<int>(mode));
        return GL_FILL;
    }

    [[nodiscard]] inline GLenum ToGL(RHI::Filter filter)
    {
        switch (filter)
        {
            case RHI::Filter::Nearest:
                return GL_NEAREST;
            case RHI::Filter::Linear:
                return GL_LINEAR;
        }
        OLO_CORE_ERROR("ToGL(RHI::Filter): unhandled value {}", static_cast<int>(filter));
        return GL_LINEAR;
    }

    [[nodiscard]] inline GLenum ToGL(RHI::AddressMode mode)
    {
        switch (mode)
        {
            case RHI::AddressMode::Repeat:
                return GL_REPEAT;
            case RHI::AddressMode::MirroredRepeat:
                return GL_MIRRORED_REPEAT;
            case RHI::AddressMode::ClampToEdge:
                return GL_CLAMP_TO_EDGE;
            case RHI::AddressMode::ClampToBorder:
                return GL_CLAMP_TO_BORDER;
        }
        OLO_CORE_ERROR("ToGL(RHI::AddressMode): unhandled value {}", static_cast<int>(mode));
        return GL_CLAMP_TO_EDGE;
    }

    [[nodiscard]] inline GLenum ToGL(RHI::PrimitiveTopology topology)
    {
        switch (topology)
        {
            case RHI::PrimitiveTopology::TriangleList:
                return GL_TRIANGLES;
            case RHI::PrimitiveTopology::TriangleStrip:
                return GL_TRIANGLE_STRIP;
            case RHI::PrimitiveTopology::LineList:
                return GL_LINES;
            case RHI::PrimitiveTopology::LineStrip:
                return GL_LINE_STRIP;
            case RHI::PrimitiveTopology::PointList:
                return GL_POINTS;
            case RHI::PrimitiveTopology::PatchList:
                return GL_PATCHES;
        }
        OLO_CORE_ERROR("ToGL(RHI::PrimitiveTopology): unhandled value {}", static_cast<int>(topology));
        return GL_TRIANGLES;
    }

    // Sized internal format — what glTextureStorage2D / glBindImageTexture want.
    [[nodiscard]] inline GLenum ToGLInternalFormat(RHI::Format format)
    {
        switch (format)
        {
            case RHI::Format::R8UNorm:
                return GL_R8;
            case RHI::Format::R8UInt:
                return GL_R8UI;
            case RHI::Format::RG8UNorm:
                return GL_RG8;
            case RHI::Format::RGB8UNorm:
                return GL_RGB8;
            case RHI::Format::RGBA8UNorm:
                return GL_RGBA8;
            case RHI::Format::RGBA8SRGB:
                return GL_SRGB8_ALPHA8;
            case RHI::Format::R16UInt:
                return GL_R16UI;
            case RHI::Format::RG16UInt:
                return GL_RG16UI;
            case RHI::Format::RG16Float:
                return GL_RG16F;
            case RHI::Format::RGBA16Float:
                return GL_RGBA16F;
            case RHI::Format::R32Float:
                return GL_R32F;
            case RHI::Format::R32Int:
                return GL_R32I;
            case RHI::Format::R32UInt:
                return GL_R32UI;
            case RHI::Format::RG32Float:
                return GL_RG32F;
            case RHI::Format::RGB32Float:
                return GL_RGB32F;
            case RHI::Format::RGBA32Float:
                return GL_RGBA32F;
            case RHI::Format::D24UNormS8UInt:
                return GL_DEPTH24_STENCIL8;
            case RHI::Format::D32Float:
                return GL_DEPTH_COMPONENT32F;
            case RHI::Format::BC5UNorm:
                return GL_COMPRESSED_RG_RGTC2;
            case RHI::Format::BC6HUFloat:
                return GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT;
            case RHI::Format::BC7UNorm:
                return GL_COMPRESSED_RGBA_BPTC_UNORM;
            case RHI::Format::BC7SRGB:
                return GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM;
            case RHI::Format::Unknown:
                break;
        }
        OLO_CORE_ERROR("ToGLInternalFormat: unhandled RHI::Format {}", static_cast<int>(format));
        return GL_RGBA8;
    }

    // Client-side pixel format + type — the (format, type) pair glTextureSubImage2D
    // wants to describe the CPU-side source data. NOTE this describes the *upload
    // buffer's* layout, not the texture's storage: an RG16F texture is legitimately
    // fed from RG32Float host data (see SSAORenderPass's noise texture), and GL
    // converts on upload.
    [[nodiscard]] inline GLenum ToGLPixelFormat(RHI::Format format)
    {
        switch (format)
        {
            case RHI::Format::R8UNorm:
            case RHI::Format::R32Float:
                return GL_RED;
            case RHI::Format::R8UInt:
            case RHI::Format::R16UInt:
            case RHI::Format::R32UInt:
            case RHI::Format::R32Int:
                return GL_RED_INTEGER;
            case RHI::Format::RG8UNorm:
            case RHI::Format::RG16Float:
            case RHI::Format::RG32Float:
                return GL_RG;
            case RHI::Format::RG16UInt:
                return GL_RG_INTEGER;
            case RHI::Format::RGB8UNorm:
            case RHI::Format::RGB32Float:
                return GL_RGB;
            case RHI::Format::RGBA8UNorm:
            case RHI::Format::RGBA8SRGB:
            case RHI::Format::RGBA16Float:
            case RHI::Format::RGBA32Float:
                return GL_RGBA;
            case RHI::Format::D32Float:
                return GL_DEPTH_COMPONENT;
            case RHI::Format::D24UNormS8UInt:
                return GL_DEPTH_STENCIL;
            default:
                break;
        }
        OLO_CORE_ERROR("ToGLPixelFormat: unhandled RHI::Format {}", static_cast<int>(format));
        return GL_RGBA;
    }

    [[nodiscard]] inline GLenum ToGLPixelType(RHI::Format format)
    {
        switch (format)
        {
            case RHI::Format::R8UNorm:
            case RHI::Format::R8UInt:
            case RHI::Format::RG8UNorm:
            case RHI::Format::RGB8UNorm:
            case RHI::Format::RGBA8UNorm:
            case RHI::Format::RGBA8SRGB:
                return GL_UNSIGNED_BYTE;
            case RHI::Format::R16UInt:
            case RHI::Format::RG16UInt:
                return GL_UNSIGNED_SHORT;
            case RHI::Format::R32UInt:
                return GL_UNSIGNED_INT;
            case RHI::Format::R32Int:
                return GL_INT;
            case RHI::Format::RG16Float:
            case RHI::Format::RGBA16Float:
                return GL_HALF_FLOAT;
            case RHI::Format::R32Float:
            case RHI::Format::RG32Float:
            case RHI::Format::RGB32Float:
            case RHI::Format::RGBA32Float:
            case RHI::Format::D32Float:
                return GL_FLOAT;
            case RHI::Format::D24UNormS8UInt:
                return GL_UNSIGNED_INT_24_8;
            default:
                break;
        }
        OLO_CORE_ERROR("ToGLPixelType: unhandled RHI::Format {}", static_cast<int>(format));
        return GL_UNSIGNED_BYTE;
    }

    [[nodiscard]] inline GLenum ToGL(RHI::IndexType type)
    {
        switch (type)
        {
            case RHI::IndexType::UInt16:
                return GL_UNSIGNED_SHORT;
            case RHI::IndexType::UInt32:
                return GL_UNSIGNED_INT;
        }
        OLO_CORE_ERROR("ToGL(RHI::IndexType): unhandled value {}", static_cast<int>(type));
        return GL_UNSIGNED_INT;
    }

    [[nodiscard]] inline GLenum ToGL(RHI::FrontFace face)
    {
        switch (face)
        {
            case RHI::FrontFace::CounterClockwise:
                return GL_CCW;
            case RHI::FrontFace::Clockwise:
                return GL_CW;
        }
        OLO_CORE_ERROR("ToGL(RHI::FrontFace): unhandled value {}", static_cast<int>(face));
        return GL_CCW;
    }

    // The GL query target a RHI::QueryType begins/ends against.
    [[nodiscard]] inline GLenum ToGL(RHI::QueryType type)
    {
        switch (type)
        {
            case RHI::QueryType::OcclusionAnySamples:
                return GL_ANY_SAMPLES_PASSED;
            case RHI::QueryType::TimeElapsed:
                return GL_TIME_ELAPSED;
            case RHI::QueryType::Timestamp:
                return GL_TIMESTAMP;
        }
        OLO_CORE_ERROR("ToGL(RHI::QueryType): unhandled value {}", static_cast<int>(type));
        return GL_ANY_SAMPLES_PASSED;
    }

    // glNamedBufferData's usage hint. GL treats it as a hint only, so a wrong
    // value here costs bandwidth rather than correctness — which is precisely why
    // it needs a table test: nothing would render wrong, and no other assertion
    // in the suite would notice.
    [[nodiscard]] inline GLenum ToGL(RHI::MemoryResidency residency)
    {
        switch (residency)
        {
            case RHI::MemoryResidency::HostToDevice:
                return GL_DYNAMIC_DRAW;
            case RHI::MemoryResidency::DeviceLocal:
                return GL_DYNAMIC_COPY;
            case RHI::MemoryResidency::DeviceToHost:
                return GL_DYNAMIC_READ;
        }
        OLO_CORE_ERROR("ToGL(RHI::MemoryResidency): unhandled value {}", static_cast<int>(residency));
        return GL_DYNAMIC_DRAW;
    }

    // glBlitNamedFramebuffer's mask. A GLbitfield rather than a GLenum — the
    // return type is the tell that this one is not an enum lowering.
    [[nodiscard]] inline GLbitfield ToGLBlitMask(RHI::BlitAspect aspect)
    {
        switch (aspect)
        {
            case RHI::BlitAspect::Color:
                return GL_COLOR_BUFFER_BIT;
            case RHI::BlitAspect::Depth:
                return GL_DEPTH_BUFFER_BIT;
            case RHI::BlitAspect::Stencil:
                return GL_STENCIL_BUFFER_BIT;
            case RHI::BlitAspect::DepthStencil:
                return GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
        }
        OLO_CORE_ERROR("ToGLBlitMask: unhandled RHI::BlitAspect {}", static_cast<int>(aspect));
        return GL_COLOR_BUFFER_BIT;
    }

    // A colour-attachment index, or RHI::NoAttachment for "writes nowhere".
    // The sentinel is the reason this is not a bare `GL_COLOR_ATTACHMENT0 + i`
    // at each call site: GL_NONE is not GL_COLOR_ATTACHMENT0 + anything, and a
    // Vulkan backend needs the same distinction for VK_ATTACHMENT_UNUSED.
    [[nodiscard]] inline GLenum ToGLColorAttachment(u32 attachmentIndex)
    {
        if (attachmentIndex == RHI::NoAttachment)
        {
            return GL_NONE;
        }
        return GL_COLOR_ATTACHMENT0 + attachmentIndex;
    }

    // glBindImageTexture's access parameter. Only the three storage accesses are
    // meaningful here; anything else is a caller bug rather than a lowering gap.
    [[nodiscard]] inline GLenum ToGLImageAccess(RHI::Access access)
    {
        switch (access)
        {
            case RHI::Access::StorageRead:
                return GL_READ_ONLY;
            case RHI::Access::StorageWrite:
                return GL_WRITE_ONLY;
            case RHI::Access::StorageReadWrite:
                return GL_READ_WRITE;
            default:
                break;
        }
        OLO_CORE_ERROR("ToGLImageAccess: RHI::Access {} is not an image-load/store access",
                       static_cast<int>(access));
        return GL_READ_WRITE;
    }
} // namespace OloEngine::Utils

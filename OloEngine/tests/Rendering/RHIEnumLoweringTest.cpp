// OLO_TEST_LAYER: plumbing
//
// Pins the RHI:: -> GLenum lowering table (Platform/OpenGL/OpenGLRHIConversions.h).
//
// Issue #691 Phase 2. This is the test that guards the phase's actual failure
// mode. Phase 2 rewrote ~270 call sites from `GL_SRC_ALPHA` to
// `RHI::BlendFactor::SrcAlpha`; a single wrong entry in the lowering table
// renders subtly wrong — a slightly off blend, a depth test that passes one
// fragment too many — while every existing test stays green, because nothing
// else in the suite compares a rendered pixel against a formula that depends on
// these constants.
//
// Two properties are asserted, and the second is the one that survives future
// edits:
//
//   1. Every enumerator lowers to the exact GL constant it names. Checked
//      against the literal GL_* token rather than a numeric value, so the test
//      states the intended mapping rather than restating whatever the code does.
//   2. Each enum's member COUNT is pinned by a static_assert on its last
//      enumerator. Adding a member without extending ToGL() would otherwise fall
//      through to the switch's error path, which logs and returns a plausible
//      default — a silent wrong mapping, exactly what (1) cannot catch on its
//      own because the new member has no table row.
//
// No GL context is required: the conversions are pure switches.

#include "OloEngine/Renderer/RHI/RHITypes.h"
#include "Platform/OpenGL/OpenGLRHIConversions.h"

#include <gtest/gtest.h>

#include <glad/gl.h>

using namespace OloEngine;

namespace
{
    // ---------------------------------------------------------------------
    // Member-count tripwires. If one of these fires, the enum gained or lost a
    // member: extend Utils::ToGL(...) in OpenGLRHIConversions.h AND the
    // corresponding table below, then update the expected ordinal here.
    // ---------------------------------------------------------------------
    static_assert(static_cast<int>(RHI::CompareOp::Always) == 7,
                  "RHI::CompareOp changed — update ToGL() and CompareOpLowering");
    static_assert(static_cast<int>(RHI::BlendFactor::SrcAlphaSaturate) == 12,
                  "RHI::BlendFactor changed — update ToGL() and BlendFactorLowering");
    static_assert(static_cast<int>(RHI::BlendOp::Max) == 4,
                  "RHI::BlendOp changed — update ToGL() and BlendOpLowering");
    static_assert(static_cast<int>(RHI::StencilOp::DecrementWrap) == 7,
                  "RHI::StencilOp changed — update ToGL() and StencilOpLowering");
    static_assert(static_cast<int>(RHI::CullMode::FrontAndBack) == 3,
                  "RHI::CullMode changed — update ToGL() and CullModeLowering");
    static_assert(static_cast<int>(RHI::PolygonMode::Point) == 2,
                  "RHI::PolygonMode changed — update ToGL() and PolygonModeLowering");
    static_assert(static_cast<int>(RHI::Filter::Linear) == 1,
                  "RHI::Filter changed — update ToGL() and FilterLowering");
    static_assert(static_cast<int>(RHI::AddressMode::ClampToBorder) == 3,
                  "RHI::AddressMode changed — update ToGL() and AddressModeLowering");
    static_assert(static_cast<int>(RHI::Format::BC7SRGB) == 22,
                  "RHI::Format changed — update ToGLInternalFormat() and FormatLowering");
    // Access and PrimitiveTopology are lowered by ToGLImageAccess() / ToGL() and
    // so need the same growth tripwire. Access is the one most likely to gain a
    // member — ADR 0011 §1.5 has Phase 5 unifying ResourceTransition's read/write
    // enum pair onto it — and only its three Storage* values are image-load/store
    // accesses, so a new member falling through ToGLImageAccess()'s default would
    // silently bind as GL_READ_WRITE.
    static_assert(static_cast<int>(RHI::Access::Present) == 18,
                  "RHI::Access changed — update ToGLImageAccess() and ImageAccessLowering");
    static_assert(static_cast<int>(RHI::PrimitiveTopology::PatchList) == 5,
                  "RHI::PrimitiveTopology changed — update ToGL() and PrimitiveTopologyLowering");

    // Phase 2 step 2 vocabulary (ADR 0011 amendment (10)). Same tripwire
    // discipline: IndexType in particular has only two members, which makes a
    // swapped mapping look harmless right up until a u16-indexed mesh reads its
    // element buffer at 4-byte stride and renders as scattered triangles.
    static_assert(static_cast<int>(RHI::IndexType::UInt32) == 1,
                  "RHI::IndexType changed — update ToGL() and IndexTypeLowering");
    static_assert(static_cast<int>(RHI::FrontFace::Clockwise) == 1,
                  "RHI::FrontFace changed — update ToGL() and FrontFaceLowering");
    static_assert(static_cast<int>(RHI::QueryType::TimeElapsed) == 1,
                  "RHI::QueryType changed — update ToGL() and QueryTypeLowering");
    static_assert(static_cast<int>(RHI::MemoryResidency::DeviceToHost) == 2,
                  "RHI::MemoryResidency changed — update ToGL() and MemoryResidencyLowering");
    static_assert(static_cast<int>(RHI::BlitAspect::DepthStencil) == 3,
                  "RHI::BlitAspect changed — update ToGLBlitMask() and BlitAspectLowering");
    // FenceStatus has no ToGL() — it is produced BY the backend, not consumed by
    // it — so its tripwire lives with ClientWaitFence's switch instead. Pinned
    // here anyway so a new member is noticed at the same place as its siblings.
    static_assert(static_cast<int>(RHI::FenceStatus::Failed) == 3,
                  "RHI::FenceStatus changed — update OpenGLRendererAPI::ClientWaitFence");
} // namespace

TEST(RHIEnumLowering, CompareOpLowersToTheNamedGLConstant)
{
    EXPECT_EQ(Utils::ToGL(RHI::CompareOp::Never), GLenum{ GL_NEVER });
    EXPECT_EQ(Utils::ToGL(RHI::CompareOp::Less), GLenum{ GL_LESS });
    EXPECT_EQ(Utils::ToGL(RHI::CompareOp::Equal), GLenum{ GL_EQUAL });
    EXPECT_EQ(Utils::ToGL(RHI::CompareOp::LessOrEqual), GLenum{ GL_LEQUAL });
    EXPECT_EQ(Utils::ToGL(RHI::CompareOp::Greater), GLenum{ GL_GREATER });
    EXPECT_EQ(Utils::ToGL(RHI::CompareOp::NotEqual), GLenum{ GL_NOTEQUAL });
    EXPECT_EQ(Utils::ToGL(RHI::CompareOp::GreaterOrEqual), GLenum{ GL_GEQUAL });
    EXPECT_EQ(Utils::ToGL(RHI::CompareOp::Always), GLenum{ GL_ALWAYS });
}

TEST(RHIEnumLowering, BlendFactorLowersToTheNamedGLConstant)
{
    EXPECT_EQ(Utils::ToGL(RHI::BlendFactor::Zero), GLenum{ GL_ZERO });
    EXPECT_EQ(Utils::ToGL(RHI::BlendFactor::One), GLenum{ GL_ONE });
    EXPECT_EQ(Utils::ToGL(RHI::BlendFactor::SrcColor), GLenum{ GL_SRC_COLOR });
    EXPECT_EQ(Utils::ToGL(RHI::BlendFactor::OneMinusSrcColor), GLenum{ GL_ONE_MINUS_SRC_COLOR });
    EXPECT_EQ(Utils::ToGL(RHI::BlendFactor::DstColor), GLenum{ GL_DST_COLOR });
    EXPECT_EQ(Utils::ToGL(RHI::BlendFactor::OneMinusDstColor), GLenum{ GL_ONE_MINUS_DST_COLOR });
    EXPECT_EQ(Utils::ToGL(RHI::BlendFactor::SrcAlpha), GLenum{ GL_SRC_ALPHA });
    EXPECT_EQ(Utils::ToGL(RHI::BlendFactor::OneMinusSrcAlpha), GLenum{ GL_ONE_MINUS_SRC_ALPHA });
    EXPECT_EQ(Utils::ToGL(RHI::BlendFactor::DstAlpha), GLenum{ GL_DST_ALPHA });
    EXPECT_EQ(Utils::ToGL(RHI::BlendFactor::OneMinusDstAlpha), GLenum{ GL_ONE_MINUS_DST_ALPHA });
    EXPECT_EQ(Utils::ToGL(RHI::BlendFactor::ConstantColor), GLenum{ GL_CONSTANT_COLOR });
    EXPECT_EQ(Utils::ToGL(RHI::BlendFactor::OneMinusConstantColor), GLenum{ GL_ONE_MINUS_CONSTANT_COLOR });
    EXPECT_EQ(Utils::ToGL(RHI::BlendFactor::SrcAlphaSaturate), GLenum{ GL_SRC_ALPHA_SATURATE });
}

TEST(RHIEnumLowering, BlendOpLowersToTheNamedGLConstant)
{
    EXPECT_EQ(Utils::ToGL(RHI::BlendOp::Add), GLenum{ GL_FUNC_ADD });
    EXPECT_EQ(Utils::ToGL(RHI::BlendOp::Subtract), GLenum{ GL_FUNC_SUBTRACT });
    EXPECT_EQ(Utils::ToGL(RHI::BlendOp::ReverseSubtract), GLenum{ GL_FUNC_REVERSE_SUBTRACT });
    EXPECT_EQ(Utils::ToGL(RHI::BlendOp::Min), GLenum{ GL_MIN });
    EXPECT_EQ(Utils::ToGL(RHI::BlendOp::Max), GLenum{ GL_MAX });
}

TEST(RHIEnumLowering, StencilOpLowersToTheNamedGLConstant)
{
    EXPECT_EQ(Utils::ToGL(RHI::StencilOp::Keep), GLenum{ GL_KEEP });
    EXPECT_EQ(Utils::ToGL(RHI::StencilOp::Zero), GLenum{ GL_ZERO });
    EXPECT_EQ(Utils::ToGL(RHI::StencilOp::Replace), GLenum{ GL_REPLACE });
    // The clamp/wrap distinction is the one pair here that is easy to swap and
    // impossible to see in a rendered frame until a stencil counter saturates.
    EXPECT_EQ(Utils::ToGL(RHI::StencilOp::IncrementClamp), GLenum{ GL_INCR });
    EXPECT_EQ(Utils::ToGL(RHI::StencilOp::DecrementClamp), GLenum{ GL_DECR });
    EXPECT_EQ(Utils::ToGL(RHI::StencilOp::Invert), GLenum{ GL_INVERT });
    EXPECT_EQ(Utils::ToGL(RHI::StencilOp::IncrementWrap), GLenum{ GL_INCR_WRAP });
    EXPECT_EQ(Utils::ToGL(RHI::StencilOp::DecrementWrap), GLenum{ GL_DECR_WRAP });
}

TEST(RHIEnumLowering, CullModeAndPolygonModeLowerToTheNamedGLConstant)
{
    EXPECT_EQ(Utils::ToGL(RHI::CullMode::Front), GLenum{ GL_FRONT });
    EXPECT_EQ(Utils::ToGL(RHI::CullMode::Back), GLenum{ GL_BACK });
    EXPECT_EQ(Utils::ToGL(RHI::CullMode::FrontAndBack), GLenum{ GL_FRONT_AND_BACK });

    EXPECT_EQ(Utils::ToGL(RHI::PolygonMode::Fill), GLenum{ GL_FILL });
    EXPECT_EQ(Utils::ToGL(RHI::PolygonMode::Line), GLenum{ GL_LINE });
    EXPECT_EQ(Utils::ToGL(RHI::PolygonMode::Point), GLenum{ GL_POINT });
}

TEST(RHIEnumLowering, SamplerStateLowersToTheNamedGLConstant)
{
    EXPECT_EQ(Utils::ToGL(RHI::Filter::Nearest), GLenum{ GL_NEAREST });
    EXPECT_EQ(Utils::ToGL(RHI::Filter::Linear), GLenum{ GL_LINEAR });

    EXPECT_EQ(Utils::ToGL(RHI::AddressMode::Repeat), GLenum{ GL_REPEAT });
    EXPECT_EQ(Utils::ToGL(RHI::AddressMode::MirroredRepeat), GLenum{ GL_MIRRORED_REPEAT });
    EXPECT_EQ(Utils::ToGL(RHI::AddressMode::ClampToEdge), GLenum{ GL_CLAMP_TO_EDGE });
    EXPECT_EQ(Utils::ToGL(RHI::AddressMode::ClampToBorder), GLenum{ GL_CLAMP_TO_BORDER });
}

TEST(RHIEnumLowering, InternalFormatLowersToTheNamedSizedGLFormat)
{
    EXPECT_EQ(Utils::ToGLInternalFormat(RHI::Format::R8UNorm), GLenum{ GL_R8 });
    EXPECT_EQ(Utils::ToGLInternalFormat(RHI::Format::R8UInt), GLenum{ GL_R8UI });
    EXPECT_EQ(Utils::ToGLInternalFormat(RHI::Format::RG8UNorm), GLenum{ GL_RG8 });
    EXPECT_EQ(Utils::ToGLInternalFormat(RHI::Format::RGB8UNorm), GLenum{ GL_RGB8 });
    EXPECT_EQ(Utils::ToGLInternalFormat(RHI::Format::RGBA8UNorm), GLenum{ GL_RGBA8 });
    EXPECT_EQ(Utils::ToGLInternalFormat(RHI::Format::RGBA8SRGB), GLenum{ GL_SRGB8_ALPHA8 });
    EXPECT_EQ(Utils::ToGLInternalFormat(RHI::Format::R16UInt), GLenum{ GL_R16UI });
    EXPECT_EQ(Utils::ToGLInternalFormat(RHI::Format::RG16UInt), GLenum{ GL_RG16UI });
    EXPECT_EQ(Utils::ToGLInternalFormat(RHI::Format::RG16Float), GLenum{ GL_RG16F });
    EXPECT_EQ(Utils::ToGLInternalFormat(RHI::Format::RGBA16Float), GLenum{ GL_RGBA16F });
    EXPECT_EQ(Utils::ToGLInternalFormat(RHI::Format::R32Float), GLenum{ GL_R32F });
    EXPECT_EQ(Utils::ToGLInternalFormat(RHI::Format::R32Int), GLenum{ GL_R32I });
    EXPECT_EQ(Utils::ToGLInternalFormat(RHI::Format::R32UInt), GLenum{ GL_R32UI });
    EXPECT_EQ(Utils::ToGLInternalFormat(RHI::Format::RG32Float), GLenum{ GL_RG32F });
    EXPECT_EQ(Utils::ToGLInternalFormat(RHI::Format::RGB32Float), GLenum{ GL_RGB32F });
    EXPECT_EQ(Utils::ToGLInternalFormat(RHI::Format::RGBA32Float), GLenum{ GL_RGBA32F });
    EXPECT_EQ(Utils::ToGLInternalFormat(RHI::Format::D24UNormS8UInt), GLenum{ GL_DEPTH24_STENCIL8 });
    EXPECT_EQ(Utils::ToGLInternalFormat(RHI::Format::D32Float), GLenum{ GL_DEPTH_COMPONENT32F });
    EXPECT_EQ(Utils::ToGLInternalFormat(RHI::Format::BC5UNorm), GLenum{ GL_COMPRESSED_RG_RGTC2 });
    EXPECT_EQ(Utils::ToGLInternalFormat(RHI::Format::BC6HUFloat), GLenum{ GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT });
    EXPECT_EQ(Utils::ToGLInternalFormat(RHI::Format::BC7UNorm), GLenum{ GL_COMPRESSED_RGBA_BPTC_UNORM });
    EXPECT_EQ(Utils::ToGLInternalFormat(RHI::Format::BC7SRGB), GLenum{ GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM });
}

// UploadTextureSubImage2D collapsed GL's (format, type) pair into one
// RHI::Format naming the SOURCE buffer's layout. These are the pairs the engine
// actually uploads with; getting the type wrong reads the caller's bytes at the
// wrong stride, which corrupts the texture rather than merely tinting it.
TEST(RHIEnumLowering, UploadFormatLowersToTheGLFormatTypePair)
{
    EXPECT_EQ(Utils::ToGLPixelFormat(RHI::Format::RGBA8UNorm), GLenum{ GL_RGBA });
    EXPECT_EQ(Utils::ToGLPixelType(RHI::Format::RGBA8UNorm), GLenum{ GL_UNSIGNED_BYTE });

    EXPECT_EQ(Utils::ToGLPixelFormat(RHI::Format::RG32Float), GLenum{ GL_RG });
    EXPECT_EQ(Utils::ToGLPixelType(RHI::Format::RG32Float), GLenum{ GL_FLOAT });

    EXPECT_EQ(Utils::ToGLPixelFormat(RHI::Format::R32Float), GLenum{ GL_RED });
    EXPECT_EQ(Utils::ToGLPixelType(RHI::Format::R32Float), GLenum{ GL_FLOAT });

    // Integer formats take the _INTEGER client format, not the plain one — a
    // mismatch here is a GL_INVALID_OPERATION, not a silent miscolour.
    EXPECT_EQ(Utils::ToGLPixelFormat(RHI::Format::R32UInt), GLenum{ GL_RED_INTEGER });
    EXPECT_EQ(Utils::ToGLPixelType(RHI::Format::R32UInt), GLenum{ GL_UNSIGNED_INT });
}

TEST(RHIEnumLowering, ImageAccessLowersToTheNamedGLAccess)
{
    EXPECT_EQ(Utils::ToGLImageAccess(RHI::Access::StorageRead), GLenum{ GL_READ_ONLY });
    EXPECT_EQ(Utils::ToGLImageAccess(RHI::Access::StorageWrite), GLenum{ GL_WRITE_ONLY });
    EXPECT_EQ(Utils::ToGLImageAccess(RHI::Access::StorageReadWrite), GLenum{ GL_READ_WRITE });
}

TEST(RHIEnumLowering, PrimitiveTopologyLowersToTheNamedGLPrimitive)
{
    EXPECT_EQ(Utils::ToGL(RHI::PrimitiveTopology::TriangleList), GLenum{ GL_TRIANGLES });
    EXPECT_EQ(Utils::ToGL(RHI::PrimitiveTopology::TriangleStrip), GLenum{ GL_TRIANGLE_STRIP });
    EXPECT_EQ(Utils::ToGL(RHI::PrimitiveTopology::LineList), GLenum{ GL_LINES });
    EXPECT_EQ(Utils::ToGL(RHI::PrimitiveTopology::LineStrip), GLenum{ GL_LINE_STRIP });
    EXPECT_EQ(Utils::ToGL(RHI::PrimitiveTopology::PointList), GLenum{ GL_POINTS });
    EXPECT_EQ(Utils::ToGL(RHI::PrimitiveTopology::PatchList), GLenum{ GL_PATCHES });
}

// ---------------------------------------------------------------------------
// Phase 2 step 2 vocabulary (ADR 0011 amendment (10)).
// ---------------------------------------------------------------------------

TEST(RHIEnumLowering, IndexTypeLowersToTheNamedGLType)
{
    // Two members, so a swap is invisible to a reviewer and catastrophic at run
    // time: a u16 index buffer read at u32 stride draws from the wrong vertices
    // AND overruns the buffer's tail.
    EXPECT_EQ(Utils::ToGL(RHI::IndexType::UInt16), GLenum{ GL_UNSIGNED_SHORT });
    EXPECT_EQ(Utils::ToGL(RHI::IndexType::UInt32), GLenum{ GL_UNSIGNED_INT });
}

TEST(RHIEnumLowering, FrontFaceLowersToTheNamedGLWinding)
{
    // PlanarReflectionRenderPass flips this to compensate for the mirror matrix
    // reversing triangle winding; a swap silently culls exactly the faces that
    // should be visible in the reflection.
    EXPECT_EQ(Utils::ToGL(RHI::FrontFace::CounterClockwise), GLenum{ GL_CCW });
    EXPECT_EQ(Utils::ToGL(RHI::FrontFace::Clockwise), GLenum{ GL_CW });
}

TEST(RHIEnumLowering, QueryTypeLowersToTheNamedGLTarget)
{
    EXPECT_EQ(Utils::ToGL(RHI::QueryType::OcclusionAnySamples), GLenum{ GL_ANY_SAMPLES_PASSED });
    EXPECT_EQ(Utils::ToGL(RHI::QueryType::TimeElapsed), GLenum{ GL_TIME_ELAPSED });
}

TEST(RHIEnumLowering, MemoryResidencyLowersToTheNamedGLHint)
{
    // GL treats these as hints, so a wrong entry costs bandwidth rather than
    // correctness — which is exactly why it needs a table test. Nothing would
    // render wrong and no other assertion in the suite would notice.
    EXPECT_EQ(Utils::ToGL(RHI::MemoryResidency::HostToDevice), GLenum{ GL_DYNAMIC_DRAW });
    EXPECT_EQ(Utils::ToGL(RHI::MemoryResidency::DeviceLocal), GLenum{ GL_DYNAMIC_COPY });
    EXPECT_EQ(Utils::ToGL(RHI::MemoryResidency::DeviceToHost), GLenum{ GL_DYNAMIC_READ });
}

TEST(RHIEnumLowering, BlitAspectLowersToTheNamedGLBitfield)
{
    EXPECT_EQ(Utils::ToGLBlitMask(RHI::BlitAspect::Color), GLbitfield{ GL_COLOR_BUFFER_BIT });
    EXPECT_EQ(Utils::ToGLBlitMask(RHI::BlitAspect::Depth), GLbitfield{ GL_DEPTH_BUFFER_BIT });
    EXPECT_EQ(Utils::ToGLBlitMask(RHI::BlitAspect::Stencil), GLbitfield{ GL_STENCIL_BUFFER_BIT });
    EXPECT_EQ(Utils::ToGLBlitMask(RHI::BlitAspect::DepthStencil),
              GLbitfield{ GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT });
}

// The sentinel is the whole reason draw-attachment lists go through a lowering
// function instead of `GL_COLOR_ATTACHMENT0 + i` at each call site: GL_NONE is
// not GL_COLOR_ATTACHMENT0 + anything. DecalRenderPass writes lists like
// { attachment 0, NONE, NONE, NONE, NONE } to steer a decal into exactly one
// G-Buffer target, and folding the sentinel into the arithmetic would turn
// "writes nowhere" into "writes to attachment 4294967295" — a GL_INVALID_ENUM
// that drops the whole draw-buffer assignment, leaving the PREVIOUS list live.
TEST(RHIEnumLowering, ColorAttachmentLoweringHonoursTheNoAttachmentSentinel)
{
    EXPECT_EQ(Utils::ToGLColorAttachment(0), GLenum{ GL_COLOR_ATTACHMENT0 });
    EXPECT_EQ(Utils::ToGLColorAttachment(1), GLenum{ GL_COLOR_ATTACHMENT1 });
    EXPECT_EQ(Utils::ToGLColorAttachment(4), GLenum{ GL_COLOR_ATTACHMENT4 });
    EXPECT_EQ(Utils::ToGLColorAttachment(RHI::NoAttachment), GLenum{ GL_NONE });

    // GL_NONE is 0, and so is GL_COLOR_ATTACHMENT0 + 0 in no sane reading —
    // pin that they are genuinely different values so the sentinel cannot be
    // "simplified" into attachment 0.
    EXPECT_NE(Utils::ToGLColorAttachment(RHI::NoAttachment), Utils::ToGLColorAttachment(0));
}

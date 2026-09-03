// =============================================================================
// SplatSpike_Gaussian.glsl
//
// The rasteriser half of the Gaussian-splat viability spike (issue #971): one
// screen-aligned quad per splat, sized and shaped by the splat's projected 2D
// covariance, composited with premultiplied "over" blending in the far-to-near
// order the CPU pass supplies.
//
// It lives under assets/shaders/tests/ ON PURPOSE. Splats are not part of any
// production pass and this shader claims no binding in the engine's UBO/SSBO
// namespace -- see the ADR. Everything it reads is bound by
// GaussianSplatVisualEvidenceTest, which owns the buffers.
//
// The projection is EWA splatting (Zwicker et al. 2001) as specialised by
// Kerbl et al. 2023: take the world covariance Sigma, push it through the view
// rotation W and the local affine approximation J of the perspective divide,
// keep the 2x2 block, and evaluate the resulting 2D Gaussian per fragment.
//
//     Sigma_2D = J W Sigma W^T J^T
//
// The one non-obvious term is the +0.3 added to the 2D diagonal. It is a
// low-pass filter: a splat that projects smaller than a pixel would otherwise
// alias into a flickering dot as the camera moves. It is the same constant the
// reference implementation uses, and it is why a splat can never shrink below
// roughly half a pixel on screen.
// =============================================================================

#type vertex
#version 460 core

// Mirrors OloEngine::GaussianSplat::GpuSplat (32 bytes). Declared as loose
// scalars rather than a vec3 + uints: in std430 a vec3 member has 16-byte
// ALIGNMENT but 12-byte size, which is exactly the rule that silently shifts a
// struct against its C++ mirror. All-scalar members have alignment 4 and pack
// identically on both sides.
struct SplatRecord
{
    float PosX;
    float PosY;
    float PosZ;
    uint ColorOpacity; // RGBA8, alpha = opacity
    uint CovXXXY;      // half2(Sigma[0][0], Sigma[0][1])
    uint CovXZYY;      // half2(Sigma[0][2], Sigma[1][1])
    uint CovYZZZ;      // half2(Sigma[1][2], Sigma[2][2])
    uint Pad0;
};

layout(std430, binding = 0) readonly buffer SplatBuffer
{
    SplatRecord b_Splats[];
};

// Draw order, far-to-near. One entry per instance; the buffer may be shorter
// than b_Splats when LOD or the budget dropped splats.
layout(std430, binding = 1) readonly buffer SplatOrderBuffer
{
    uint b_Order[];
};

layout(std140, binding = 7) uniform SplatViewUniforms
{
    mat4 u_View;
    mat4 u_Projection;
    vec4 u_ViewportFocal; // xy = viewport pixels, zw = focal length in pixels
};

layout(location = 0) out vec4 v_Color;    // rgb linear, a = splat opacity
layout(location = 1) out vec3 v_Conic;    // inverse Sigma_2D as (a, b, c)
layout(location = 2) out vec2 v_PixelOffset;

const vec2 kCorners[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));

void main()
{
    SplatRecord splat = b_Splats[b_Order[uint(gl_InstanceIndex)]];

    vec3 worldPos = vec3(splat.PosX, splat.PosY, splat.PosZ);

    vec2 covA = unpackHalf2x16(splat.CovXXXY);
    vec2 covB = unpackHalf2x16(splat.CovXZYY);
    vec2 covC = unpackHalf2x16(splat.CovYZZZ);
    mat3 sigmaWorld = mat3(covA.x, covA.y, covB.x, covA.y, covB.y, covC.x, covB.x, covC.x, covC.y);

    vec3 viewPos = (u_View * vec4(worldPos, 1.0)).xyz;

    // OpenGL view space looks down -z, so positive depth is -viewPos.z. Guarded
    // because the Jacobian divides by depth squared; the CPU pass already drops
    // these splats, so hitting the guard means the two disagree.
    float depth = max(-viewPos.z, 1e-4);

    float fx = u_ViewportFocal.z;
    float fy = u_ViewportFocal.w;

    // J: d(screen pixels) / d(view position), evaluated at this splat.
    mat3 jacobian =
        mat3(fx / depth, 0.0, 0.0, 0.0, fy / depth, 0.0, fx * viewPos.x / (depth * depth), fy * viewPos.y / (depth * depth), 0.0);

    mat3 W = mat3(u_View);
    mat3 T = jacobian * W;
    mat3 cov2 = T * sigmaWorld * transpose(T);

    float a = cov2[0][0] + 0.3;
    float b = cov2[0][1];
    float c = cov2[1][1] + 0.3;

    float det = a * c - b * b;
    if (det <= 0.0)
    {
        // Degenerate projection (a splat seen exactly edge-on). Collapse the
        // quad to nothing rather than emitting a NaN conic that would paint the
        // whole screen.
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        v_Color = vec4(0.0);
        v_Conic = vec3(0.0);
        v_PixelOffset = vec2(0.0);
        return;
    }

    v_Conic = vec3(c, -b, a) / det;

    // 3 sigma along the larger principal axis, as an axis-aligned quad. The
    // eigenvalues of a symmetric 2x2 are mid +/- sqrt(mid^2 - det).
    float mid = 0.5 * (a + c);
    float lambda = mid + sqrt(max(mid * mid - det, 0.0));
    float radiusPixels = ceil(3.0 * sqrt(lambda));

    vec4 clip = u_Projection * vec4(viewPos, 1.0);
    vec2 centerPixels = (clip.xy / clip.w * 0.5 + 0.5) * u_ViewportFocal.xy;

    v_PixelOffset = kCorners[gl_VertexIndex & 3] * radiusPixels;
    vec2 pixel = centerPixels + v_PixelOffset;

    float r = float((splat.ColorOpacity) & 0xFFu) / 255.0;
    float g = float((splat.ColorOpacity >> 8) & 0xFFu) / 255.0;
    float bl = float((splat.ColorOpacity >> 16) & 0xFFu) / 255.0;
    float alpha = float((splat.ColorOpacity >> 24) & 0xFFu) / 255.0;
    v_Color = vec4(r, g, bl, alpha);

    // Depth is carried through unchanged so the draw can be depth-TESTED
    // against opaque geometry even though it never depth-writes.
    gl_Position = vec4((pixel / u_ViewportFocal.xy) * 2.0 - 1.0, clip.z / clip.w, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) in vec4 v_Color;
layout(location = 1) in vec3 v_Conic;
layout(location = 2) in vec2 v_PixelOffset;

layout(location = 0) out vec4 o_Color;

void main()
{
    vec2 d = v_PixelOffset;
    float power = -0.5 * (v_Conic.x * d.x * d.x + v_Conic.z * d.y * d.y) - v_Conic.y * d.x * d.y;
    if (power > 0.0)
        discard;

    // Capped below 1 so a stack of splats can never saturate the transmittance
    // to exactly zero in one step, which is what the reference does and what
    // keeps the accumulation stable in fp16 targets.
    float alpha = min(0.99, v_Color.a * exp(power));
    if (alpha < 1.0 / 255.0)
        discard;

    // Premultiplied: blend with (ONE, ONE_MINUS_SRC_ALPHA).
    o_Color = vec4(v_Color.rgb * alpha, alpha);
}

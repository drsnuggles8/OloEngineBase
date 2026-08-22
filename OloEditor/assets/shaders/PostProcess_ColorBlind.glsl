#type vertex
#version 460 core

#ifdef OLO_VULKAN
// #691 (ADR 0011 §5): on the Vulkan backend vertex data is PULLED —
// binding 57 is the engine-wide vertex-pull binding; the root struct carries
// this buffer's device address, so the SAME 20-byte {vec3 position, vec2 uv}
// stream the attribute path consumes is read by index instead.
layout(std430, binding = 57) readonly buffer OloVertexPull
{
    float v[];
} b_Vertices;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    int base = gl_VertexIndex * 5;
    vec3 position = vec3(b_Vertices.v[base + 0], b_Vertices.v[base + 1], b_Vertices.v[base + 2]);
    v_TexCoord = vec2(b_Vertices.v[base + 3], b_Vertices.v[base + 4]);
    gl_Position = vec4(position, 1.0);
}
#else
layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}
#endif

#type fragment
#version 460 core

// Colour-vision deficiency adaptation (issue #458).
//
// Runs AFTER UICompositePass, so it remaps the HUD as well as the world — a
// colour-blind player needs the health bar and the minimap separable, not just
// the scenery. It is the last stage before FinalPass blits to the backbuffer.
//
// The input is DISPLAY-REFERRED: ToneMapPass has already applied 1/gamma. The
// cone-response math below is defined on LINEAR light, so the shader decodes,
// adapts, and re-encodes. Skipping that step is not a subtle error — it shifts
// every adapted hue.
//
// Every constant here is mirrored on the CPU in
// OloEngine/Accessibility/AccessibilitySettings.h (AdaptColorLinear) and
// compared term by term by ColorBlindMathTest. Change one, change both.

layout(location = 0) out vec4 o_Color;

layout(location = 0) in vec2 v_TexCoord;

#include "include/BindlessHeap.glsl"

// Heap-bindless conversion (issue #691, bucket 1). The BODY is
// byte-identical between the two variants — only the declaration moves, and it
// names the same binding number the pass binds with.
#ifdef OLO_BINDLESS
#define u_Texture OLO_HEAP_TEX_2D(0)
#else
layout(binding = 0) uniform sampler2D u_Texture;
#endif

// std140, binding = ShaderBindingLayout::UBO_COLORBLIND (78). One vec4 so the block
// is trivially 16-byte aligned; the C++ twin is ColorBlindUBOData.
layout(std140, binding = 78) uniform ColorBlindParams
{
    // x = mode (0 none, 1 protan, 2 deutan, 3 tritan)
    // y = severity in [0,1]
    // z = method (0 correct/daltonize, 1 simulate)
    // w = display gamma to decode/encode around
    vec4 u_ColorBlindParams;
};

// Hunt-Pointer-Estevez style linear-RGB -> LMS, in the normalisation the
// Viénot/Brettel-derived daltonization implementations use. GLSL mat3
// constructors take COLUMNS, matching the glm::mat3 in the C++ twin.
mat3 LinearRGBToLMS()
{
    return mat3(
        vec3(17.8824, 3.45565, 0.0299566),
        vec3(43.5161, 27.1554, 0.184309),
        vec3(4.11935, 3.86714, 1.46709));
}

mat3 LMSToLinearRGB()
{
    return mat3(
        vec3(0.0809444479, -0.0102485335, -0.000365296938),
        vec3(-0.130504409, 0.0540193266, -0.00412161469),
        vec3(0.116721066, -0.113614708, 0.693511405));
}

// The dichromat projection in LMS: the missing cone's response is rebuilt as a
// linear combination of the two that remain, collapsing the 3D colour space
// onto the 2D surface the viewer can actually distinguish.
mat3 DichromacyLMSProjection(int mode)
{
    if (mode == 1) // Protanopia — L rebuilt from M and S
    {
        return mat3(
            vec3(0.0, 0.0, 0.0),
            vec3(2.02344, 1.0, 0.0),
            vec3(-2.52581, 0.0, 1.0));
    }
    if (mode == 2) // Deuteranopia — M rebuilt from L and S
    {
        return mat3(
            vec3(1.0, 0.494207, 0.0),
            vec3(0.0, 0.0, 0.0),
            vec3(0.0, 1.24827, 1.0));
    }
    if (mode == 3) // Tritanopia — S rebuilt from L and M
    {
        return mat3(
            vec3(1.0, 0.0, -0.395913),
            vec3(0.0, 1.0, 0.801109),
            vec3(0.0, 0.0, 0.0));
    }
    return mat3(1.0);
}

// Where the unrecoverable signal is redistributed. For protan/deutan the
// surviving discrimination axis is blue-vs-yellow; for tritan it is red-vs-green.
mat3 ErrorShiftMatrix(int mode)
{
    if (mode == 3) // Tritanopia — fold the blue error into red+green
    {
        return mat3(
            vec3(1.0, 0.0, 0.0),
            vec3(0.0, 1.0, 0.0),
            vec3(0.7, 0.7, 0.0));
    }
    // Protan / deutan — fold the red error into green+blue
    return mat3(
        vec3(0.0, 0.7, 0.7),
        vec3(0.0, 1.0, 0.0),
        vec3(0.0, 0.0, 1.0));
}

void main()
{
    vec4 source = texture(u_Texture, v_TexCoord);

    int mode = int(u_ColorBlindParams.x + 0.5);
    if (mode <= 0 || mode > 3)
    {
        // Passthrough. The pass is normally not even declared when the mode is
        // None, so this only catches a UBO that has not been filled yet.
        o_Color = source;
        return;
    }

    float severity = clamp(u_ColorBlindParams.y, 0.0, 1.0);
    bool simulate = u_ColorBlindParams.z > 0.5;
    float gamma = max(u_ColorBlindParams.w, 0.1);

    // Display-referred -> linear. max() guards pow() against a negative input,
    // which an RGBA16Float LDR carrier can hold after a sharpen kernel
    // undershoots; pow(negative, fractional) is undefined in GLSL.
    vec3 linearColor = pow(max(source.rgb, vec3(0.0)), vec3(gamma));

    mat3 fullSim = LMSToLinearRGB() * DichromacyLMSProjection(mode) * LinearRGBToLMS();
    mat3 simMatrix = mat3(1.0) * (1.0 - severity) + fullSim * severity;

    vec3 simulated = simMatrix * linearColor;

    vec3 adapted;
    if (simulate)
    {
        // Authoring aid: show a trichromat what the dichromat sees.
        adapted = simulated;
    }
    else
    {
        // Daltonize: express what the viewer cannot see in channels they can.
        vec3 err = linearColor - simulated;
        adapted = linearColor + ErrorShiftMatrix(mode) * err;
    }

    // Back to display-referred. Clamping BEFORE the encode keeps the redistributed
    // error from re-encoding a negative value as NaN.
    vec3 encoded = pow(clamp(adapted, vec3(0.0), vec3(1.0)), vec3(1.0 / gamma));

    o_Color = vec4(encoded, source.a);
}

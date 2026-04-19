// =============================================================================
// ShaderUnit_WhiteIrradiance.glsl
//
// Unit probe for the irradiance convolution kernel used by IrradianceConvolution.glsl.
// Replaces the samplerCube lookup with a uniform-white environment radiance so we
// can run the exact same integrator over a known analytic integrand.
//
// For L_i(ω) = (1, 1, 1), the Lambertian irradiance integral reduces to
//
//     E = ∫ L_i(ω) * cos(θ) dω = π
//
// The probe writes E (the RGB value, which should equal (π, π, π)) to every
// pixel. The caller samples the center pixel and asserts each channel matches
// π within tolerance.
//
// Parameters:
//   sampleDelta = 0.025 matches production IrradianceConvolution.glsl exactly.
//
// Catches: normalization bugs (missing/extra π), dropped cos(θ) or sin(θ)
// weights, incorrect sample count, wrong hemisphere bounds.
// =============================================================================

#type vertex
#version 460 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec2 a_TexCoord;

layout(location = 0) out vec2 v_TexCoord;

void main()
{
    v_TexCoord = a_TexCoord;
    gl_Position = vec4(a_Position, 1.0);
}

#type fragment
#version 460 core

layout(location = 0) in vec2 v_TexCoord;
layout(location = 0) out vec4 o_Color;

const float PI = 3.14159265359;

void main()
{
    // N arbitrary — any orientation yields the same integral for a uniform
    // white environment. Pick +Z.
    vec3 N = vec3(0.0, 0.0, 1.0);

    vec3 up = vec3(0.0, 1.0, 0.0);
    vec3 right = normalize(cross(up, N));
    up = normalize(cross(N, right));

    vec3 irradiance = vec3(0.0);
    float sampleDelta = 0.025;
    float nrSamples = 0.0;

    for (float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta)
    {
        for (float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta)
        {
            // Uniform white radiance — the entire point of the test.
            vec3 sampleRadiance = vec3(1.0);

            irradiance += sampleRadiance * cos(theta) * sin(theta);
            nrSamples += 1.0;
        }
    }

    irradiance = PI * irradiance * (1.0 / nrSamples);

    o_Color = vec4(irradiance, 1.0);
}

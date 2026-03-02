// PrecipitationCommon.glsl — Shared precipitation uniforms and helpers
// Consumed by PostProcess_Precipitation.glsl and Precipitation_Feed.comp

#ifndef PRECIPITATION_COMMON_GLSL
#define PRECIPITATION_COMMON_GLSL

// ── Precipitation UBO (binding 18, std140) ──
layout(std140, binding = 18) uniform PrecipitationUBO
{
    // vec4 0: x = intensity, y = windInfluence, z = streakIntensity, w = streakLength
    vec4 u_PrecipIntensityAndScreenFX;
    // vec4 1: x = lensImpactRate, y = lensImpactLifetime, z = lensImpactSize, w = enabled (1.0/0.0)
    vec4 u_PrecipLensParams;
    // vec4 2: x,y = screenWindDir, z = time, w = streaksEnabled (1.0/0.0)
    vec4 u_PrecipScreenWindAndTime;
    // vec4 3: particle color (RGBA)
    vec4 u_PrecipParticleColor;
    // vec4 4: x = type (0=Snow, 1=Rain, 2=Hail, 3=Sleet)
    vec4 u_PrecipTypeParams;
};

// Precipitation type constants
const int PRECIP_SNOW  = 0;
const int PRECIP_RAIN  = 1;
const int PRECIP_HAIL  = 2;
const int PRECIP_SLEET = 3;

// Convenience accessors
float precipIntensity()     { return u_PrecipIntensityAndScreenFX.x; }
float precipWindInfluence() { return u_PrecipIntensityAndScreenFX.y; }
float precipStreakIntensity(){ return u_PrecipIntensityAndScreenFX.z; }
float precipStreakLength()  { return u_PrecipIntensityAndScreenFX.w; }

float precipLensImpactRate()    { return u_PrecipLensParams.x; }
float precipLensImpactLifetime(){ return u_PrecipLensParams.y; }
float precipLensImpactSize()    { return u_PrecipLensParams.z; }
bool  precipEnabled()           { return u_PrecipLensParams.w > 0.5; }

vec2  precipScreenWindDir()     { return u_PrecipScreenWindAndTime.xy; }
float precipTime()              { return u_PrecipScreenWindAndTime.z; }
bool  precipStreaksEnabled()    { return u_PrecipScreenWindAndTime.w > 0.5; }

vec4  precipParticleColor()     { return u_PrecipParticleColor; }
int   precipType()              { return int(u_PrecipTypeParams.x + 0.5); }
bool  precipIsSnow()            { return precipType() == PRECIP_SNOW; }
bool  precipIsRain()            { return precipType() == PRECIP_RAIN; }
bool  precipIsHail()            { return precipType() == PRECIP_HAIL; }
bool  precipIsSleet()           { return precipType() == PRECIP_SLEET; }

// ── Hash / noise helpers for screen-space effects ──

// Simple 2D hash (returns 0..1)
float precipHash21(vec2 p)
{
    p = fract(p * vec2(443.8975, 397.2973));
    p += dot(p, p + 19.19);
    return fract(p.x * p.y);
}

// Tileable value noise (returns -1..1)
float precipValueNoise(vec2 uv)
{
    vec2 i = floor(uv);
    vec2 f = fract(uv);
    f = f * f * (3.0 - 2.0 * f); // smoothstep

    float a = precipHash21(i);
    float b = precipHash21(i + vec2(1.0, 0.0));
    float c = precipHash21(i + vec2(0.0, 1.0));
    float d = precipHash21(i + vec2(1.0, 1.0));

    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y) * 2.0 - 1.0;
}

// Fractal Brownian Motion (3 octaves)
float precipFBM(vec2 uv)
{
    float val = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 3; ++i)
    {
        val += amp * precipValueNoise(uv);
        uv *= 2.1;
        amp *= 0.5;
    }
    return val;
}

// Soft radial falloff for lens impacts (bell-shaped)
float precipLensSpotFalloff(float dist, float radius)
{
    float t = clamp(dist / radius, 0.0, 1.0);
    return exp(-4.0 * t * t); // Gaussian-ish
}

#endif // PRECIPITATION_COMMON_GLSL

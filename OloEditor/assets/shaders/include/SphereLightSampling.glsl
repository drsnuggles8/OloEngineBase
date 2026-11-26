#ifndef OLO_SPHERE_LIGHT_SAMPLING_GLSL
#define OLO_SPHERE_LIGHT_SAMPLING_GLSL

#include "GPUScene.glsl"

// PathTracer.cpp's ViewSphereLight / SphereConePdf / SphereLightPointDistance,
// function for function. A sphere of radius r at distance d is a uniform
// emitter whose radiance reproduces the raster's diffuse irradiance at the
// receiver, sampled by uniform solid angle over the cone it subtends.
struct OloSphereLightView
{
    bool Valid;
    float Distance;
    float CosThetaMax;
    vec3 Radiance;
};

OloSphereLightView OloViewSphereLight(GPUSceneLight light, vec3 from)
{
    OloSphereLightView view;
    view.Valid = false;
    view.Distance = 0.0;
    view.CosThetaMax = 1.0;
    view.Radiance = vec3(0.0);
    const vec3 toCenter = light.PositionAndRange.xyz - from;
    const float distance = length(toCenter);
    const float radius = light.DirectionAndRadius.w;
    const float range = light.PositionAndRange.w;
    if (!(radius > 0.0) || !(distance > radius) || distance > range)
        return view;
    const float distRatio = distance / max(range, 1e-6);
    const float window = max(1.0 - distRatio * distRatio, 0.0);
    const float attenuation = window * window / (distance * distance + 1.0);
    view.Valid = true;
    view.Distance = distance;
    view.CosThetaMax = sqrt(max(0.0, 1.0 - (radius * radius) / (distance * distance)));
    view.Radiance = light.ColorAndIntensity.rgb * light.ColorAndIntensity.w * attenuation * (distance * distance) /
                    (PI * radius * radius);
    return view;
}

float OloSphereConePdf(float cosThetaMax)
{
    const float solidAngle = 2.0 * PI * (1.0 - cosThetaMax);
    return solidAngle > 0.0 ? 1.0 / solidAngle : 0.0;
}

// The direction drawn from the cone, in the receiver's frame. Split out from
// the pdf so a caller that only needs the density (a MIS weight on a BSDF hit)
// does not build a basis it will not use.
vec3 OloSampleSphereLightDirection(GPUSceneLight light, OloSphereLightView view, vec3 from, vec2 xi)
{
    const float cosTheta = 1.0 - xi.x * (1.0 - view.CosThetaMax);
    const float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    const float phi = 2.0 * PI * xi.y;
    const vec3 toCenter = (light.PositionAndRange.xyz - from) / view.Distance;
    vec3 tangent, bitangent;
    OrthonormalBasis(toCenter, tangent, bitangent);
    return tangent * (sinTheta * cos(phi)) + bitangent * (sinTheta * sin(phi)) + toCenter * cosTheta;
}

#endif

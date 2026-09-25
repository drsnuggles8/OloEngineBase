#ifndef PARTICLE_SCENE_COLOUR_FRAGMENT_GLSL
#define PARTICLE_SCENE_COLOUR_FRAGMENT_GLSL

// A particle FRAGMENT stage that writes scene colour and its auxiliary targets.
// Include straight after the stage's #version line.

layout(location = 0) out vec4 o_Color;
layout(location = 1) out int o_EntityID;
layout(location = 2) out vec2 o_ViewNormal;
// Scene FB RT3 velocity -- camera + per-particle motion, from the clip positions
// the vertex stage reprojects through u_PrevViewProjection.
layout(location = 3) out vec4 o_Velocity;
// Scene FB RT4: the diffuse half of a SKIN pixel's lighting, for the screen-space
// diffusion pass (issue #1241). This surface never shades skin, so it writes the
// "no diffusion here" code -- but it must WRITE it: an MRT output a shader leaves
// alone is undefined, not zero, and SkinDiffusion.glsl would blur the garbage
// into scene colour. See include/PBRCommon.glsl, "THE DIFFUSION HAND-OFF".
layout(location = 4) out vec4 o_SkinDiffuse;

layout(location = 2) in flat int v_EntityID;
layout(location = 3) in vec4 v_ClipPosCurr;
layout(location = 4) in vec4 v_ClipPosPrev;

#include "ParticleFragmentCommon.glsl"

void main()
{
	vec4 texColor = ParticleColor();

	o_Color = texColor;
	o_EntityID = v_EntityID;
	o_ViewNormal = vec2(-2.0);

	vec2 ndcCurr = v_ClipPosCurr.xy / v_ClipPosCurr.w;
	vec2 ndcPrev = v_ClipPosPrev.xy / v_ClipPosPrev.w;
	o_Velocity = vec4((ndcCurr - ndcPrev) * 0.5, 1.0, 0.0);
	o_SkinDiffuse = vec4(0.0); // not skin -- see the declaration above (#1241)
}

#endif

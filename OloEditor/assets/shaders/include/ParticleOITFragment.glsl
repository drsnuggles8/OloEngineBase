#ifndef PARTICLE_OIT_FRAGMENT_GLSL
#define PARTICLE_OIT_FRAGMENT_GLSL

// A particle FRAGMENT stage that packs into the weighted-blended OIT targets
// (#1417). Include straight after the stage's #version line. Without an OIT
// stage, a particle drawn inside ParticleRenderPass's OIT path wrote the
// scene-colour outputs into OITBuffer: nothing reached the revealage target, so
// OITResolve discarded every one of its pixels.

// OITBuffer attachments:
//   0 : RGBA16F accum (sum of Ci*ai*wi, sum of ai*wi)
//   1 : RG16F revealage (R = product factor for (1 - ai))
layout(location = 0) out vec4 o_Accum;
layout(location = 1) out vec4 o_Revealage;

#include "ParticleFragmentCommon.glsl"
#include "OITCommon.glsl"

void main()
{
	vec4 texColor = ParticleColor();
	// The weight wants linear view depth. gl_FragCoord.w is 1 / clip w, and clip w
	// is exactly that distance under a perspective projection -- no interpolant,
	// and no dependence on the soft-fade near/far pair, which is only uploaded
	// when a system asks for soft particles (0 / 0 otherwise).
	float weight = ComputeOITWeight(texColor.a, 1.0 / max(gl_FragCoord.w, 1e-6));
	OITPack(texColor.rgb, texColor.a, weight, o_Accum, o_Revealage);
}

#endif

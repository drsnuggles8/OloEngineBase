//--------------------------
// - OloEngine -
// GPU Particle Billboard Shader, weighted-blended OIT variant (#1417)
// The same particles as Particle_Billboard_GPU.glsl, packed into OITBuffer. Without
// it, GPU particles drew the scene-colour shader into the OIT targets while the
// pass was in OIT mode: nothing reached the revealage target, so OITResolve
// discarded every particle pixel.
// --------------------------
#type vertex
#version 450 core

#include "include/ParticleBillboardGPUVertex.glsl"

#type fragment
#version 450 core

// WB-OIT outputs. OITBuffer attachments:
//   0 : RGBA16F accum (sum of Ci*ai*wi, sum of ai*wi)
//   1 : RG16F revealage (R = product factor for (1 - ai))
layout(location = 0) out vec4 o_Accum;
layout(location = 1) out vec4 o_Revealage;

#include "include/ParticleBillboardGPUFragment.glsl"
#include "include/OITCommon.glsl"

void main()
{
	vec4 texColor = ParticleGPUBillboardColor();
	// The weight wants linear view depth. gl_FragCoord.w is 1 / clip w, and clip w
	// is exactly that distance under a perspective projection -- no interpolant,
	// and no dependence on the soft-fade near/far pair, which is only uploaded
	// when a system asks for soft particles (0 / 0 otherwise).
	float weight = ComputeOITWeight(texColor.a, 1.0 / max(gl_FragCoord.w, 1e-6));
	OITPack(texColor.rgb, texColor.a, weight, o_Accum, o_Revealage);
}

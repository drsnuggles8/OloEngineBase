#ifndef PARTICLE_FRAGMENT_COMMON_GLSL
#define PARTICLE_FRAGMENT_COMMON_GLSL

// What every particle FRAGMENT stage shares -- billboards drawn from the GPU
// simulation, trails and mesh particles: the interpolants, the textures and
// parameters, and the particle's colour after texturing and the soft-particle
// fade. The stage then packs that colour one of two ways:
// ParticleSceneColourFragment.glsl (scene colour and its auxiliary targets) or
// ParticleOITFragment.glsl (the weighted-blended OIT targets, #1417).

struct VertexOutput
{
	vec4 Color;
	vec2 TexCoord;
};

layout(location = 0) in VertexOutput Input;

#include "BindlessHeap.glsl"

// Heap-bindless conversion (issue #691, bucket 1). Both slots move
// together because ParticleBatchRenderer::BindParticleTextures stages both in
// one call — converting one and leaving the other would leave the unconverted
// sampler unbound once this program builds as the bindless variant (§5c).
#ifdef OLO_BINDLESS
#define u_Texture OLO_HEAP_TEX_2D(0)
#define u_DepthTexture OLO_HEAP_TEX_2D(1)
#else
layout(binding = 0) uniform sampler2D u_Texture;
layout(binding = 1) uniform sampler2D u_DepthTexture;
#endif

layout(std140, binding = 2) uniform ParticleParams
{
	vec3 u_CameraRight;
	vec3 u_CameraUp;
	int u_HasTexture;
	int u_SoftParticlesEnabled;
	float u_SoftParticleDistance;
	float u_NearClip;
	float u_FarClip;
	vec2 u_ViewportSize;
};

float LinearizeDepth(float depth)
{
	float ndc = depth * 2.0 - 1.0;
	return (2.0 * u_NearClip * u_FarClip) / (u_FarClip + u_NearClip - ndc * (u_FarClip - u_NearClip));
}

// The particle's colour for this fragment, after its texture and the soft
// fade. Discards what would contribute nothing.
vec4 ParticleColor()
{
	vec4 texColor = Input.Color;
	if (u_HasTexture != 0)
	{
		texColor *= texture(u_Texture, Input.TexCoord);
	}

	if (texColor.a < 0.001)
		discard;

	// Soft particle depth fade
	if (u_SoftParticlesEnabled != 0)
	{
		vec2 screenUV = gl_FragCoord.xy / u_ViewportSize;
		float sceneDepth = texture(u_DepthTexture, screenUV).r;
		float linearScene = LinearizeDepth(sceneDepth);
		float linearFrag = LinearizeDepth(gl_FragCoord.z);
		float depthDiff = linearScene - linearFrag;
		float fade = clamp(depthDiff / u_SoftParticleDistance, 0.0, 1.0);
		texColor.a *= fade;
	}

	if (texColor.a < 0.001)
		discard;
	return texColor;
}

#endif

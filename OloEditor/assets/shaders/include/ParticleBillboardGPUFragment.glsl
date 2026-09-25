#ifndef PARTICLE_BILLBOARD_GPU_FRAGMENT_GLSL
#define PARTICLE_BILLBOARD_GPU_FRAGMENT_GLSL

// What both GPU-particle billboard FRAGMENT stages share: the interpolants, the
// textures and parameters, and the particle's colour after texturing and the
// soft-particle fade. Each stage adds its own outputs and packs the colour its
// way -- scene colour for Particle_Billboard_GPU.glsl, the WB-OIT targets for
// Particle_Billboard_GPU_OIT.glsl (#1417).

struct VertexOutput
{
	vec4 Color;
	vec2 TexCoord;
};

layout(location = 0) in VertexOutput Input;

#include "BindlessHeap.glsl"

// Heap-bindless conversion (issue #691, bucket 1). Both slots move
// together — ParticleBatchRenderer::BindParticleTextures stages both in one
// call (glsl-shaders.md §5c).
//
// This shader reads gl_InstanceIndex, the VULKAN spelling. It used to be
// unconvertible for that reason; the bindless route now applies SPIRV-Cross's
// own translation itself, so the Vulkan spelling stays here — correct for the
// default SPIR-V path — and the GL route rewrites it.
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
vec4 ParticleGPUBillboardColor()
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

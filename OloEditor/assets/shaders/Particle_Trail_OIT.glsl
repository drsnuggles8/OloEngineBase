//--------------------------
// - OloEngine -
// Particle Trail Shader, weighted-blended OIT variant (#1417)
// The same particles as Particle_Trail.glsl, packed into OITBuffer. Selected by
// ParticleBatchRenderer while ParticleRenderPass is in OIT mode.
// --------------------------
#type vertex
#version 450 core

#include "include/ParticleTrailVertex.glsl"

#type fragment
#version 450 core

#include "include/ParticleOITFragment.glsl"

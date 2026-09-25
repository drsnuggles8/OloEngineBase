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

#include "include/ParticleOITFragment.glsl"

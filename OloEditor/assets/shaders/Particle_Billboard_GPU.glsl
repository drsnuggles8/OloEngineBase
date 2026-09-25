//--------------------------
// - OloEngine -
// GPU Particle Billboard Shader
// Reads particle data from SSBO (GPU-driven rendering via indirect draw)
// --------------------------
#type vertex
#version 450 core

#include "include/ParticleBillboardGPUVertex.glsl"

#type fragment
#version 450 core

#include "include/ParticleSceneColourFragment.glsl"

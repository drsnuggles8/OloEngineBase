//--------------------------
// - OloEngine -
// Particle Mesh Shader
// Per-draw-call mesh particle rendering
// Single instance data via UBO (binding 3)
// --------------------------
#type vertex
#version 450 core

#include "include/ParticleMeshVertex.glsl"

#type fragment
#version 450 core

#include "include/ParticleSceneColourFragment.glsl"

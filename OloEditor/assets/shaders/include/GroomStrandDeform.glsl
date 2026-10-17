// GroomStrandDeform.glsl — a bound coat deformed in the vertex stage, issue #1427.
//
// EVERYTHING IN THIS FILE HAS A C++ TWIN in OloEngine/Groom/GroomGpuDeformation.h
// (EvaluateGroomDeformedPoint), which reads the SAME packed bytes. The twin is
// what the coat self-shadow bake evaluates the drawn pose with, and what the
// parity tests compare the CPU-deformed reference against; a shader that moved
// strands differently would put the shadow somewhere the coat is not.
//
// The arithmetic is ApplyGroomRootTransform's second half and
// SampleGroomGuideDisplacement, in that order — BuildGroomStrandMesh's order:
//
//     placed = Origin + Rotation * local        (local is in the rest stream)
//     placed += the guide displacement at t     (when the coat is simulated)
//
// The buffer's layout is documented once, in GroomGpuDeformation.h. Read it as
// uvec4s and reinterpret: a float lane read through uintBitsToFloat is exact,
// while an integer lane read as a float could be a denormal a driver is entitled
// to flush.
//
// VERTEX STAGE ONLY. The block is declared here rather than in
// GroomStrandCommon.glsl so the fragment stage and the parity probes that
// include that file do not grow a storage binding they never read.

#ifndef GROOM_STRAND_DEFORM_GLSL
#define GROOM_STRAND_DEFORM_GLSL

// SSBO_GROOM_DEFORMATION — shared with SSBO_TERRAIN_VT under the rebound-per-use
// rule; see ShaderBindingLayout.h. GroomRenderPass binds a buffer here before
// every strand draw, a placeholder when the draw reads nothing.
layout(std430, binding = 79) readonly buffer GroomDeformation
{
	uvec4 b_GroomDeform[];
};

// oloGroomQuatRotate and its kin are include/GroomQuat.glsl's, which every
// shader including this one includes first.
#ifndef GROOM_QUAT_GLSL
#error "include/GroomQuat.glsl must be included before include/GroomStrandDeform.glsl"
#endif

#include "GroomWetness.glsl"

vec4 oloGroomDeformFloat4(uint unit)
{
	return uintBitsToFloat(b_GroomDeform[unit]);
}

// The four lanes of GroomStrandParams that describe the buffer, unpacked once
// per vertex. Counts come from the CPU rather than from .length(): a runtime
// array length is what brought Vulkan down in glsl-length-crashes-vulkan.
struct OloGroomDeformLayout
{
	uint RootCount;
	uint SlotCount;
	uint BindBase;
	// The wet coat's regions (#1615), derived like BindBase:
	// GroomDeformBufferLayout::Make writes ClumpBase = RootCount * 4 and
	// WetBase = RootCount * 5.
	uint ClumpBase;
	uint WetBase;
	uint RootBase;
	uint SlotBase;
	uint DisplacementBase;
	uint DisplacementCount;
	bool Simulated;
};

OloGroomDeformLayout oloGroomDeformLayout(ivec4 modes, ivec4 bases)
{
	OloGroomDeformLayout buf;
	buf.Simulated = modes.y != 0;
	buf.RootCount = uint(max(modes.z, 0));
	buf.SlotCount = uint(max(modes.w, 0));
	// Right after the two-unit guide weights, so derived rather than given a
	// lane: GroomDeformBufferLayout::Make writes BindBase = RootCount * 2.
	buf.BindBase = buf.RootCount * 2u;
	buf.ClumpBase = buf.RootCount * 4u;
	buf.WetBase = buf.RootCount * 5u;
	buf.RootBase = uint(max(bases.x, 0));
	buf.SlotBase = uint(max(bases.y, 0));
	buf.DisplacementBase = uint(max(bases.z, 0));
	buf.DisplacementCount = uint(max(bases.w, 0));
	return buf;
}

// SampleGroomGuideDisplacement over the packed records. Slots the budget did
// not simulate this frame carry Count == 0 (PackFrame folds every reason the
// CPU skips a slot into that), and the result is renormalised by the weight
// actually applied, exactly as the CPU does.
vec3 oloGroomSampleDisplacement(OloGroomDeformLayout buf, uint root, float t, bool previous)
{
	if (root >= buf.RootCount)
	{
		return vec3(0.0);
	}
	uvec4 slotIds = b_GroomDeform[root * 2u + 0u];
	vec4 weights = oloGroomDeformFloat4(root * 2u + 1u);
	float parameter = clamp(t, 0.0, 1.0);

	vec3 result = vec3(0.0);
	float applied = 0.0;
	for (int k = 0; k < 4; ++k)
	{
		uint slot = slotIds[k];
		float weight = weights[k];
		if (slot >= buf.SlotCount || !(weight > 0.0))
		{
			continue;
		}
		uvec4 record = b_GroomDeform[buf.SlotBase + slot];
		uint first = record.x;
		uint count = record.y;
		if (count == 0u || first + count > buf.DisplacementCount)
		{
			continue;
		}
		// Two uvec4s per sample: current, then previous.
		uint lane = previous ? 1u : 0u;
		if (count == 1u)
		{
			result += oloGroomDeformFloat4(buf.DisplacementBase + first * 2u + lane).xyz * weight;
			applied += weight;
			continue;
		}
		float scaled = parameter * float(count - 1u);
		float floored = floor(scaled);
		uint lower = uint(floored);
		uint upper = min(lower + 1u, count - 1u);
		float fraction = scaled - floored;
		vec3 a = oloGroomDeformFloat4(buf.DisplacementBase + (first + lower) * 2u + lane).xyz;
		vec3 b = oloGroomDeformFloat4(buf.DisplacementBase + (first + upper) * 2u + lane).xyz;
		result += mix(a, b, fraction) * weight;
		applied += weight;
	}
	return applied > 0.0 ? result / applied : vec3(0.0);
}

// The root's BIND frame (#1533): where the rest stream's bind-local points
// rest, `origin + rotation * local`. Past the buffer, the identity -- the same
// answer oloGroomDeformPoint gives an out-of-range root.
void oloGroomDeformBindFrame(OloGroomDeformLayout buf, uint root, out vec3 origin, out vec4 rotation)
{
	origin = vec3(0.0);
	rotation = vec4(0.0, 0.0, 0.0, 1.0);
	if (root < buf.RootCount)
	{
		uint base = buf.BindBase + root * 2u;
		origin = oloGroomDeformFloat4(base + 0u).xyz;
		rotation = oloGroomDeformFloat4(base + 1u);
	}
}

// The root's rotation this frame (the same record oloGroomDeformPoint reads).
vec4 oloGroomDeformRootRotation(OloGroomDeformLayout buf, uint root)
{
	return root < buf.RootCount ? oloGroomDeformFloat4(buf.RootBase + root * 4u + 1u) : vec4(0.0, 0.0, 0.0, 1.0);
}

// A WET COAT (#1615): the bind-local point laid down, shortened and clumped
// as the coat's wetness at this strand's root says. The record is per frame --
// the coat's gravity in object space; the world-up row a root's height above
// the coat's origin is measured on; the rain, the soaked line, the band and an
// "any of it wet" flag; the response -- and the clump record per strand is
// static. A dry coat's flag is zero and the point passes
// through untouched, so every coat that never gets wet deforms exactly as it
// did. The wetness is the ROOT's: a strand lies down as one. The frame whose
// root is evaluated supplies both the height and the gravity, so last frame's
// point is last frame's wet shape and a still, wet coat has no velocity.
// Twin: GroomDeformWetLocal in GroomGpuDeformation.cpp.
vec3 oloGroomDeformWetLocal(OloGroomDeformLayout buf, uint root, vec3 origin, vec4 rotation, vec3 local, float t)
{
	vec4 frame = oloGroomDeformFloat4(buf.WetBase + 2u);
	if (!(frame.w > 0.5))
	{
		return local;
	}
	vec4 gravity = oloGroomDeformFloat4(buf.WetBase + 0u);
	float wetness = oloGroomWetness(dot(origin, oloGroomDeformFloat4(buf.WetBase + 1u).xyz), frame.x, frame.y, frame.z);
	if (!(wetness > 0.0))
	{
		return local;
	}
	vec3 gravityLocal = oloGroomQuatRotate(oloGroomQuatConjugate(rotation), gravity.xyz);
	vec3 clump = oloGroomDeformFloat4(buf.ClumpBase + root).xyz;
	return oloGroomWetLocal(local, t, wetness, gravityLocal, clump, oloGroomDeformFloat4(buf.WetBase + 3u));
}

// One point of one strand, this frame (previous = false) or last.
vec3 oloGroomDeformPoint(OloGroomDeformLayout buf, uint root, vec3 local, float t, bool previous)
{
	if (root >= buf.RootCount)
	{
		// Past the buffer: the bind-local point, unmoved. Unreachable with a
		// stream and buffer built together; answered rather than read out of
		// bounds, and matching the CPU twin's answer for the same input.
		return local;
	}
	uint base = buf.RootBase + root * 4u + (previous ? 2u : 0u);
	vec3 origin = oloGroomDeformFloat4(base + 0u).xyz;
	vec4 rotation = oloGroomDeformFloat4(base + 1u);
	vec3 placed = origin + oloGroomQuatRotate(rotation, oloGroomDeformWetLocal(buf, root, origin, rotation, local, t));
	if (buf.Simulated)
	{
		placed += oloGroomSampleDisplacement(buf, root, t, previous);
	}
	return placed;
}

#endif // GROOM_STRAND_DEFORM_GLSL

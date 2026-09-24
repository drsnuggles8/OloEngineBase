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

// glm's quat * vec3, term for term (glm/detail/type_quat.inl), so the CPU twin
// and this agree to the rounding of the same operations. q is (x, y, z, w).
vec3 oloGroomQuatRotate(vec4 q, vec3 v)
{
	vec3 uv = cross(q.xyz, v);
	vec3 uuv = cross(q.xyz, uv);
	return v + ((uv * q.w) + uuv) * 2.0;
}

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
	vec3 placed = origin + oloGroomQuatRotate(rotation, local);
	if (buf.Simulated)
	{
		placed += oloGroomSampleDisplacement(buf, root, t, previous);
	}
	return placed;
}

#endif // GROOM_STRAND_DEFORM_GLSL
